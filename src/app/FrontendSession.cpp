// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <ai/openai/codex/frontend/Security.h>
#include <ai/openai/codex/frontend/client/Controller.h>
#include <ai/openai/codex/frontend/client/Models.h>
#include <ai/openai/codex/frontend/client/Requests.h>
#include <ai/openai/codex/frontend/client/Threads.h>
#include <ai/openai/codex/frontend/client/Turns.h>
#include <ai/openai/codex/typed/Conversation.h>

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <sys/socket.h>
#include <unistd.h>

namespace codexui {
namespace sdk = ai::openai::codex::frontend::client;
namespace frontend = ai::openai::codex::frontend;

namespace detail {

std::optional<QString> unixPeerCredentialError(qintptr socketDescriptor, uid_t expectedUserId) noexcept
{
    if (socketDescriptor < 0 || socketDescriptor > std::numeric_limits<int>::max())
        return QStringLiteral("Could not authenticate the Unix backend peer");
    ucred credentials{};
    socklen_t credentialsSize = sizeof(credentials);
    if (::getsockopt(static_cast<int>(socketDescriptor), SOL_SOCKET, SO_PEERCRED, &credentials, &credentialsSize) != 0
        || credentialsSize != sizeof(credentials))
        return QStringLiteral("Could not authenticate the Unix backend peer");
    if (credentials.uid != expectedUserId)
        return QStringLiteral("Unix backend peer belongs to a different user");
    return std::nullopt;
}

StateUpdateScope stateUpdateScope(const sdk::StateUpdate& update)
{
    StateUpdateScope scope;
    const auto addUnique = [](QStringList& ids, std::string_view id)
    {
        const QString threadId = QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        if (!ids.contains(threadId))
            ids.append(threadId);
    };
    const auto addThread = [&scope, &addUnique](std::string_view id) {
        addUnique(scope.affectedThreadIds, id);
    };
    const auto addFullyAffectedThread = [&scope, &addThread, &addUnique](std::string_view id) {
        addThread(id);
        addUnique(scope.fullyAffectedThreadIds, id);
    };
    const auto addInspectorThread = [&scope, &addUnique](std::string_view id) {
        addUnique(scope.affectedInspectorThreadIds, id);
    };
    const auto markThreadAndInspector = [&addFullyAffectedThread, &addInspectorThread](std::string_view id) {
        addFullyAffectedThread(id);
        addInspectorThread(id);
    };
    const auto addItemContent = [&scope, &addThread](const sdk::ItemContentReplacedChange& value) {
        const auto asQString = [](std::string_view id) {
            return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        };
        const QString threadId = asQString(value.threadId->value);
        const QString turnId = asQString(value.turnId->value);
        const QString itemId = asQString(value.itemId.value);
        addThread(value.threadId->value);
        const StateUpdateScope::ItemContentIdentity identity{threadId, turnId, itemId};
        if (std::find(scope.affectedItemContents.cbegin(),
                      scope.affectedItemContents.cend(),
                      identity) == scope.affectedItemContents.cend())
            scope.affectedItemContents.push_back(identity);
    };

    if (update.changes.empty())
    {
        scope.allThreadsAffected = true;
        scope.allInspectorsAffected = true;
        scope.sidebarAffected = true;
        scope.hasPresentationChange = true;
        return scope;
    }

    for (const auto& change : update.changes)
    {
        std::visit(
            [&](const auto& value)
            {
                using Change = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Change, sdk::CursorAdvancedChange>)
                {
                    // A cursor-only update changes only the Inspector's cheap
                    // revision value; it must not dirty any expensive pane.
                }
                else if constexpr (std::is_same_v<Change, sdk::StateReplacedChange>)
                {
                    scope.allThreadsAffected = true;
                    scope.allInspectorsAffected = true;
                    scope.sidebarAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::ThreadUpsertedChange>
                                   || std::is_same_v<Change, sdk::ThreadRemovedChange>)
                {
                    addFullyAffectedThread(value.threadId.value);
                    scope.sidebarAffected = true;
                    // The selected Inspector can show status/model facts from
                    // a linked subagent thread even when its conversation is
                    // not selected.
                    scope.allInspectorsAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::TurnUpsertedChange>)
                {
                    if (const auto* turn = update.state.turn(value.turnId))
                        markThreadAndInspector(turn->threadId.value);
                    else {
                        scope.allThreadsAffected = true;
                        scope.allInspectorsAffected = true;
                    }
                }
                else if constexpr (std::is_same_v<Change, sdk::ItemUpsertedChange>)
                {
                    if (value.threadId)
                        markThreadAndInspector(value.threadId->value);
                    else if (value.turnId) {
                        if (const auto* turn = update.state.turn(*value.turnId))
                            markThreadAndInspector(turn->threadId.value);
                        else {
                            scope.allThreadsAffected = true;
                            scope.allInspectorsAffected = true;
                        }
                    }
                    else {
                        scope.allThreadsAffected = true;
                        scope.allInspectorsAffected = true;
                    }
                }
                else if constexpr (std::is_same_v<Change, sdk::ItemContentReplacedChange>)
                {
                    // Streaming text/output changes affect the conversation,
                    // not Sidebar thread facts or Inspector semantics. The SDK
                    // exposes both replacement and negotiated append wire
                    // updates through this canonical public change.
                    if (value.threadId && value.turnId)
                        addItemContent(value);
                    else if (value.threadId)
                        addFullyAffectedThread(value.threadId->value);
                    else if (value.turnId) {
                        if (const auto* turn = update.state.turn(*value.turnId))
                            addFullyAffectedThread(turn->threadId.value);
                        else {
                            scope.allThreadsAffected = true;
                            scope.allInspectorsAffected = true;
                        }
                    }
                    else {
                        scope.allThreadsAffected = true;
                        scope.allInspectorsAffected = true;
                    }
                }
                else if constexpr (std::is_same_v<Change, sdk::PendingRequestsUpdatedChange>)
                {
                    // Pending requests contribute to activity-card status. The
                    // change has no thread identity, especially on removal.
                    scope.allThreadsAffected = true;
                    scope.allInspectorsAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::ThreadListUpdatedChange>)
                {
                    // A list replacement has no per-thread removal identity.
                    scope.allThreadsAffected = true;
                    scope.allInspectorsAffected = true;
                    scope.sidebarAffected = true;
                }
                else
                {
                    // Controller, pending-request, provider, capacity, and other
                    // domain projections can affect the Inspector and controls,
                    // but do not require rebuilding an unchanged conversation.
                    scope.allInspectorsAffected = true;
                }
                scope.hasPresentationChange = true;
            },
            change);
    }
    return scope;
}

} // namespace detail

namespace {

constexpr qsizetype maximumFrameBytes = 16 * 1024 * 1024;
constexpr qsizetype maximumReceiveBatchBytes = 1024 * 1024;
constexpr qsizetype inboundCompactionThreshold = 256 * 1024;
constexpr int minimumReceiveBatchFrames = 1;
constexpr int maximumReceiveBatchFrames = 256;
constexpr qint64 maximumReceiveBatchTimeMs = 4;
constexpr std::uint32_t archivedThreadListPageSize = 100;
constexpr std::size_t maximumArchivedThreadListPages = 64;
constexpr std::uint32_t modelListPageSize = 100;
constexpr std::size_t maximumModelListPages = 64;

QString operationError(const std::optional<sdk::Error>& error, const QString& fallback)
{
    if (error && !error->message.empty())
        return QString::fromStdString(error->message);
    return fallback;
}

std::optional<QString> submissionError(const sdk::Submission& submission, const QString& fallback)
{
    if (submission)
        return std::nullopt;
    return operationError(submission.error, fallback);
}

void eraseFrame(std::string& frame) noexcept
{
    try {
        frame.resize(frame.capacity(), '\0');
    } catch (...) {
    }
    volatile char* bytes = frame.empty() ? nullptr : frame.data();
    for (std::size_t index = 0; index < frame.size(); ++index)
        bytes[index] = '\0';
    frame.clear();
}

} // namespace

FrontendSession::FrontendSession(QObject* parent)
    : QObject(parent)
{
    sdk::ClientOptions options;
    options.credentialProvider = [] {
        return sdk::AuthenticationContext{
            frontend::NoCredential{},
            std::string{"verified-local:"} + std::to_string(::geteuid()),
        };
    };

    sdk::ClientCallbacks callbacks;
    callbacks.onConnectionStateChanged = [this](const sdk::ConnectionStateChange& change) {
        handleConnectionStateChange(change);
    };
    callbacks.onStateUpdated = [this](const sdk::StateUpdate& update) {
        currentState = update.state;
        for (const auto& change : update.changes) {
            if (const auto* removed = std::get_if<sdk::ThreadRemovedChange>(&change))
                requestedThreadReads.erase(removed->threadId.value);
        }
        reconcileRequestedThreadReads();
        const detail::StateUpdateScope scope = detail::stateUpdateScope(update);
        if (scope.hasPresentationChange)
            emit stateChanged(scope);
    };
    callbacks.onSynchronized = [this](const sdk::SynchronizationInfo& info) {
        reconnectDelayMs = initialReconnectDelayMs;
        consecutivePreReadyDisconnects = 0;
        synchronizedCurrentConnection = true;
        automaticReconnectEnabled = true;
        currentState = info.state;
        reconcileRequestedThreadReads();
        const bool clearReadyDiagnostic = currentLifecycle == Lifecycle::Ready
                                          && detail.isEmpty()
                                          && !diagnosticDetail.isEmpty();
        if (clearReadyDiagnostic)
            diagnosticDetail.clear();
        setLifecycle(Lifecycle::Ready);
        if (clearReadyDiagnostic)
            emit statusChanged();
        detail::StateUpdateScope scope;
        scope.allThreadsAffected = true;
        scope.allInspectorsAffected = true;
        scope.sidebarAffected = true;
        scope.hasPresentationChange = true;
        emit stateChanged(scope);
        beginArchivedThreadRefresh();
        beginModelCatalogRefresh();
    };
    callbacks.onDiagnostic = [this](const sdk::Diagnostic& diagnostic) {
        if (diagnostic.severity == sdk::Diagnostic::Severity::Error)
            reportDiagnostic(QString::fromStdString(diagnostic.message));
    };
    client = std::make_unique<Client>(std::move(options), std::move(callbacks));

    connect(&socket, &QLocalSocket::connected, this, &FrontendSession::socketConnected);
    connect(&socket, &QLocalSocket::readyRead, this, &FrontendSession::socketReadyRead);
    connect(&socket, &QLocalSocket::bytesWritten, this, &FrontendSession::socketBytesWritten);
    connect(&socket, &QLocalSocket::disconnected, this, &FrontendSession::socketDisconnected);
    connect(&socket, &QLocalSocket::errorOccurred, this, &FrontendSession::socketFailed);
    reconnectTimer.setSingleShot(true);
    connect(&reconnectTimer, &QTimer::timeout, this, &FrontendSession::retryConnection);
    outboundDrainTimer.setSingleShot(true);
    connect(&outboundDrainTimer, &QTimer::timeout, this, &FrontendSession::drainSocketWrites);
}

FrontendSession::~FrontendSession()
{
    localShutdown = true;
    reconnectTimer.stop();
    clearOutbound();
    if (connection.isOpen())
        connection.close("CodexUI is closing");
    client->close("CodexUI is closing");
    socket.abort();
}

void FrontendSession::connectToBackend()
{
    resetReconnectPolicy();
    startConnection();
}

void FrontendSession::reconnectToBackend()
{
    reconnectTimer.stop();
    clearOutbound();
    if (connection.isOpen())
        connection.close("User requested reconnect");
    connection = Connection{};
    clearInbound();
    receiveContinuationScheduled = false;
    requestedThreadReads.clear();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.abort();
        resetReconnectPolicy();
        reconnectTimer.start(0);
        return;
    }
    resetReconnectPolicy();
    startConnection();
}

void FrontendSession::startConnection()
{
    if (socket.state() != QLocalSocket::UnconnectedState || connection.isOpen())
        return;
    ++connectionGeneration;
    synchronizedCurrentConnection = false;
    archivedThreadListCursors.clear();
    archivedThreadListInFlight = false;
    archivedThreadListComplete = false;
    modelListCursors.clear();
    pendingModelCatalog.clear();
    modelListInFlight = false;
    modelListComplete = false;
    if (!availableModelCatalog.empty()) {
        availableModelCatalog.clear();
        emit modelCatalogChanged();
    }
    localShutdown = false;
    setLifecycle(Lifecycle::Connecting);
    socket.connectToServer(defaultSocketPath(), QIODevice::ReadWrite);
}

FrontendSession::Lifecycle FrontendSession::lifecycle() const noexcept
{
    return currentLifecycle;
}

QString FrontendSession::statusText() const
{
    if (!detail.isEmpty())
        return detail;
    if (!diagnosticDetail.isEmpty())
        return diagnosticDetail;
    switch (currentLifecycle) {
        case Lifecycle::Disconnected:
            return QStringLiteral("Disconnected");
        case Lifecycle::Connecting:
            return QStringLiteral("Connecting…");
        case Lifecycle::Authenticating:
            return QStringLiteral("Authenticating…");
        case Lifecycle::Synchronizing:
            return QStringLiteral("Synchronizing…");
        case Lifecycle::Ready:
            return QStringLiteral("State synced");
        case Lifecycle::Failed:
            return QStringLiteral("Connection failed");
    }
    return QStringLiteral("Disconnected");
}

std::optional<QString> FrontendSession::promptValidationError(const QString& prompt)
{
    std::size_t scalarCount = 0;
    for (qsizetype index = 0; index < prompt.size(); ++index)
    {
        const QChar current = prompt.at(index);
        if (current.isHighSurrogate() && index + 1 < prompt.size()
            && prompt.at(index + 1).isLowSurrogate())
            ++index;
        ++scalarCount;
        if (scalarCount > ai::openai::codex::typed::MaximumTurnInputTextUnicodeScalars)
            return QStringLiteral("Prompt exceeds Codex's %1 Unicode-scalar text input limit")
                .arg(static_cast<qulonglong>(
                    ai::openai::codex::typed::MaximumTurnInputTextUnicodeScalars));
    }
    return std::nullopt;
}

const sdk::State& FrontendSession::state() const noexcept
{
    return currentState;
}

const std::vector<ai::openai::codex::typed::Model>& FrontendSession::modelCatalog() const noexcept
{
    return availableModelCatalog;
}

bool FrontendSession::archivedThreadDiscoveryComplete() const noexcept
{
    return archivedThreadListComplete;
}

bool FrontendSession::ownsController() const noexcept
{
    const auto& projection = currentState.controller();
    return projection.value && projection.value->ownedByThisClient;
}

void FrontendSession::loadThread(const QString& threadId)
{
    if (currentLifecycle != Lifecycle::Ready || threadId.isEmpty())
        return;
    const std::string id = threadId.toStdString();
    const auto* thread = currentState.thread(id);
    if (!thread || thread->fullyLoaded || requestedThreadReads.contains(id))
        return;

    requestedThreadReads.insert(id);
    sdk::Submission submission = client->threads().read(
        {ai::openai::codex::typed::ThreadId{id}, true},
        [this, id](const sdk::OperationResult<sdk::ThreadReadResult>& result) {
            // A successful operation acknowledgement can precede the State
            // projection. Keep suppressing duplicate reads until that thread is
            // fully loaded (or a failure/removal/disconnect makes retry valid).
            if (!result)
                requestedThreadReads.erase(id);
            else
                reconcileRequestedThreadReads();
        });
    if (!submission)
        requestedThreadReads.erase(id);
}

std::optional<QString> FrontendSession::acquireController(OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    sdk::Submission submission = client->controller().acquire(
        [completion = std::move(completion)](const sdk::OperationResult<sdk::ControllerResult>& result) {
            if (!result) {
                completion(operationError(result.error, QStringLiteral("Controller acquisition failed")));
                return;
            }
            if (!result.value->ownedByThisClient) {
                completion(QStringLiteral("Controller is owned by another frontend"));
                return;
            }
            completion({});
        });
    return submissionError(submission, QStringLiteral("Controller acquisition was not accepted"));
}

std::optional<QString> FrontendSession::startThread(ThreadStartCompletion completion)
{
    return startThread(ai::openai::codex::typed::ThreadStartParams{}, std::move(completion));
}

std::optional<QString>
FrontendSession::startThread(ai::openai::codex::typed::ThreadStartParams parameters,
                             ThreadStartCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    sdk::Submission submission = client->threads().start(
        std::move(parameters),
        [completion = std::move(completion)](const sdk::OperationResult<sdk::ThreadStartResult>& result) {
            if (!result) {
                completion({}, operationError(result.error, QStringLiteral("New thread could not be created")));
                return;
            }
            if (result.value->threadId.value.empty()) {
                completion({}, QStringLiteral("New thread response did not contain a stable thread ID"));
                return;
            }
            completion(QString::fromStdString(result.value->threadId.value), {});
        });
    return submissionError(submission, QStringLiteral("New thread submission was not accepted"));
}

std::optional<QString> FrontendSession::resumeThread(const QString& threadId, ThreadStartCompletion completion)
{
    ai::openai::codex::typed::ThreadResumeParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    return resumeThread(std::move(parameters), std::move(completion));
}

std::optional<QString>
FrontendSession::resumeThread(ai::openai::codex::typed::ThreadResumeParams parameters,
                              ThreadStartCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    const std::string requestedId = parameters.threadId.value;
    sdk::Submission submission = client->threads().resume(
        std::move(parameters),
        [completion = std::move(completion), requestedId](const sdk::OperationResult<sdk::ThreadResumeResult>& result) {
            if (!result) {
                completion({}, operationError(result.error, QStringLiteral("Thread could not be attached")));
                return;
            }
            if (result.value->threadId.value != requestedId) {
                completion({}, QStringLiteral("Thread attach returned an unexpected thread ID"));
                return;
            }
            completion(QString::fromStdString(result.value->threadId.value), {});
        });
    return submissionError(submission, QStringLiteral("Thread attach submission was not accepted"));
}

std::optional<QString> FrontendSession::startTurn(const QString& threadId,
                                                  const QString& prompt,
                                                  OperationCompletion completion)
{
    ai::openai::codex::typed::TurnStartParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    return startTurn(
        std::move(parameters),
        prompt,
        [completion = std::move(completion)](const QString&, const QString& error) {
            completion(error);
        });
}

std::optional<QString>
FrontendSession::startTurn(ai::openai::codex::typed::TurnStartParams parameters,
                           const QString& prompt,
                           TurnStartCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    if (const auto error = promptValidationError(prompt))
        return error;

    if (parameters.threadId.value.empty())
        return QStringLiteral("Turn submission requires a thread ID");
    ai::openai::codex::typed::TextInput input;
    const QByteArray promptUtf8 = prompt.toUtf8();
    input.text.assign(promptUtf8.constData(), static_cast<std::size_t>(promptUtf8.size()));
    parameters.input.emplace_back(std::move(input));
    sdk::Submission submission = client->turns().start(
        std::move(parameters),
        [completion = std::move(completion)](const sdk::OperationResult<sdk::TurnStartResult>& result) {
            if (!result) {
                completion({}, operationError(result.error, QStringLiteral("Turn could not be started")));
                return;
            }
            completion(QString::fromStdString(result.value->turnId.value), {});
        });
    return submissionError(submission, QStringLiteral("Turn submission was not accepted"));
}

std::optional<QString> FrontendSession::steerTurn(const QString& threadId,
                                                  const QString& expectedTurnId,
                                                  const QString& prompt,
                                                  OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    if (const auto error = promptValidationError(prompt))
        return error;
    if (threadId.isEmpty() || expectedTurnId.isEmpty())
        return QStringLiteral("Steering requires the active thread and turn identities");

    ai::openai::codex::typed::TurnSteerParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    parameters.expectedTurnId = ai::openai::codex::typed::TurnId{expectedTurnId.toStdString()};
    ai::openai::codex::typed::TextInput input;
    const QByteArray promptUtf8 = prompt.toUtf8();
    input.text.assign(promptUtf8.constData(), static_cast<std::size_t>(promptUtf8.size()));
    parameters.input.emplace_back(std::move(input));

    const std::string expectedId = parameters.expectedTurnId.value;
    sdk::Submission submission = client->turns().steer(
        std::move(parameters),
        [completion = std::move(completion), expectedId](
            const sdk::OperationResult<ai::openai::codex::typed::TurnSteerResponse>& result) {
            if (!result) {
                completion(operationError(result.error, QStringLiteral("Turn could not be steered")));
                return;
            }
            if (result.value->turnId.value != expectedId) {
                completion(QStringLiteral("Turn steering returned an unexpected turn ID"));
                return;
            }
            completion({});
        });
    return submissionError(submission, QStringLiteral("Turn steering was not accepted"));
}

std::optional<QString>
FrontendSession::forkThread(ai::openai::codex::typed::ThreadForkParams parameters,
                            ThreadStartCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    if (parameters.threadId.value.empty())
        return QStringLiteral("Fork requires a source thread ID");

    sdk::Submission submission = client->threads().fork(
        std::move(parameters),
        [completion = std::move(completion)](
            const sdk::OperationResult<ai::openai::codex::typed::ThreadForkResponse>& result)
        {
            if (!result)
            {
                completion({}, operationError(result.error, QStringLiteral("Thread could not be forked")));
                return;
            }
            if (result.value->thread.id.value.empty())
            {
                completion({}, QStringLiteral("Fork response did not contain a stable thread ID"));
                return;
            }
            completion(QString::fromStdString(result.value->thread.id.value), {});
        });
    return submissionError(submission, QStringLiteral("Fork submission was not accepted"));
}

std::optional<QString> FrontendSession::renameThread(const QString& threadId,
                                                     const QString& name,
                                                     OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    if (threadId.isEmpty() || name.trimmed().isEmpty())
        return QStringLiteral("Rename requires a thread ID and non-empty name");

    sdk::Submission submission = client->threads().setName(
        {ai::openai::codex::typed::ThreadId{threadId.toStdString()}, name.trimmed().toStdString()},
        [completion = std::move(completion)](
            const sdk::OperationResult<ai::openai::codex::typed::Unit>& result)
        {
            completion(result ? QString{}
                              : operationError(result.error, QStringLiteral("Thread could not be renamed")));
        });
    return submissionError(submission, QStringLiteral("Rename submission was not accepted"));
}

std::optional<QString> FrontendSession::archiveThread(const QString& threadId,
                                                      OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    sdk::Submission submission = client->threads().archive(
        {ai::openai::codex::typed::ThreadId{threadId.toStdString()}},
        [completion = std::move(completion)](
            const sdk::OperationResult<ai::openai::codex::typed::Unit>& result)
        {
            completion(result ? QString{}
                              : operationError(result.error, QStringLiteral("Thread could not be archived")));
        });
    return submissionError(submission, QStringLiteral("Archive submission was not accepted"));
}

std::optional<QString> FrontendSession::unarchiveThread(const QString& threadId,
                                                        OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    sdk::Submission submission = client->threads().unarchive(
        {ai::openai::codex::typed::ThreadId{threadId.toStdString()}},
        [completion = std::move(completion)](
            const sdk::OperationResult<ai::openai::codex::typed::ThreadUnarchiveResponse>& result)
        {
            completion(result ? QString{}
                              : operationError(result.error, QStringLiteral("Thread could not be unarchived")));
        });
    return submissionError(submission, QStringLiteral("Unarchive submission was not accepted"));
}

std::optional<QString> FrontendSession::deleteThread(const QString& threadId,
                                                     OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    sdk::Submission submission = client->threads().remove(
        {ai::openai::codex::typed::ThreadId{threadId.toStdString()}},
        [completion = std::move(completion)](
            const sdk::OperationResult<ai::openai::codex::typed::Unit>& result)
        {
            completion(result ? QString{}
                              : operationError(result.error, QStringLiteral("Thread could not be deleted")));
        });
    return submissionError(submission, QStringLiteral("Delete submission was not accepted"));
}

std::optional<QString> FrontendSession::interruptTurn(const QString& threadId,
                                                      const QString& turnId,
                                                      OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    ai::openai::codex::typed::TurnInterruptParams parameters{
        ai::openai::codex::typed::ThreadId{threadId.toStdString()},
        ai::openai::codex::typed::TurnId{turnId.toStdString()},
    };
    sdk::Submission submission = client->turns().interrupt(
        std::move(parameters),
        [completion = std::move(completion)](const sdk::OperationResult<ai::openai::codex::typed::Unit>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("Turn could not be interrupted")));
        });
    return submissionError(submission, QStringLiteral("Interrupt submission was not accepted"));
}

std::optional<QString> FrontendSession::respondApproval(const sdk::PendingRequestId& requestId,
                                                        ai::openai::codex::typed::ApprovalDecision decision,
                                                        OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    sdk::Submission submission = client->requests().respond(
        {requestId, std::move(decision)},
        [completion = std::move(completion)](const sdk::OperationResult<ai::openai::codex::typed::Unit>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("Approval response failed")));
        });
    return submissionError(submission, QStringLiteral("Approval response was not accepted"));
}

std::optional<QString>
FrontendSession::respondApplyPatchApproval(const sdk::PendingRequestId& requestId,
                                           ai::openai::codex::typed::ApplyPatchApprovalResponse response,
                                           OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    sdk::Submission submission = client->requests().respond(
        {requestId, std::move(response)},
        [completion = std::move(completion)](const sdk::OperationResult<ai::openai::codex::typed::Unit>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("Patch approval response failed")));
        });
    return submissionError(submission, QStringLiteral("Patch approval response was not accepted"));
}

std::optional<QString>
FrontendSession::respondExecCommandApproval(const sdk::PendingRequestId& requestId,
                                            ai::openai::codex::typed::ExecCommandApprovalResponse response,
                                            OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    sdk::Submission submission = client->requests().respond(
        {requestId, std::move(response)},
        [completion = std::move(completion)](const sdk::OperationResult<ai::openai::codex::typed::Unit>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("Command approval response failed")));
        });
    return submissionError(submission, QStringLiteral("Command approval response was not accepted"));
}

std::optional<QString> FrontendSession::respondUserInput(const sdk::PendingRequestId& requestId,
                                                         std::vector<ai::openai::codex::typed::UserInputAnswer> answers,
                                                         OperationCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    sdk::Submission submission = client->requests().respond(
        {requestId, std::move(answers)},
        [completion = std::move(completion)](const sdk::OperationResult<ai::openai::codex::typed::Unit>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("User-input response failed")));
        });
    return submissionError(submission, QStringLiteral("User-input response was not accepted"));
}

QString FrontendSession::defaultSocketPath()
{
    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDirectory.isEmpty())
        return QDir(runtimeDirectory).filePath(QStringLiteral("snodec-codex-backend.sock"));
    return QStringLiteral("/tmp/snodec-codex-backend-%1.sock").arg(::getuid());
}

void FrontendSession::socketConnected()
{
    reconnectTimer.stop();
    clearOutbound();
    clearInbound();
    receiveContinuationScheduled = false;
    if (const auto error = detail::unixPeerCredentialError(socket.socketDescriptor(), ::geteuid())) {
        failWithoutReconnect(*error);
        socket.abort();
        return;
    }
    connection = client->openConnection({
        [this](OutboundMessage message) { return send(std::move(message)); },
        [this](std::string reason) { closeTransport(QString::fromStdString(reason)); },
    });
    if (!connection.isOpen()) {
        failWithoutReconnect(QStringLiteral("Frontend SDK rejected the Unix connection"));
        socket.abort();
        return;
    }
    connection.transportConnected();
}

void FrontendSession::socketReadyRead()
{
    receiveContinuationScheduled = false;
    const auto rejectOversizedFrame = [this] {
        clearInbound();
        failWithoutReconnect(QStringLiteral("Backend frame exceeds the SDK input limit"));
        socket.abort();
    };

    QElapsedTimer processingTime;
    processingTime.start();
    qsizetype consumedBytes = inboundOffset;
    qsizetype receivedBytes = 0;
    int receivedFrames = 0;
    while (receivedFrames < maximumReceiveBatchFrames
           && receivedBytes < maximumReceiveBatchBytes
           && (receivedFrames < minimumReceiveBatchFrames
               || processingTime.elapsed() < maximumReceiveBatchTimeMs))
    {
        const qsizetype newline = inboundBuffer.indexOf('\n', consumedBytes);
        if (newline < 0)
        {
            const qsizetype available = socket.bytesAvailable();
            const qsizetype budget = maximumReceiveBatchBytes - receivedBytes;
            if (available <= 0 || budget <= 0)
                break;
            const QByteArray chunk = socket.read(qMin(available, budget));
            if (chunk.isEmpty())
                break;
            inboundBuffer.append(chunk);
            receivedBytes += chunk.size();
            continue;
        }

        const qsizetype payloadEnd = newline > consumedBytes && inboundBuffer.at(newline - 1) == '\r'
                                         ? newline - 1
                                         : newline;
        const qsizetype payloadBytes = payloadEnd - consumedBytes;
        if (payloadBytes > maximumFrameBytes) {
            rejectOversizedFrame();
            return;
        }

        ++receivedFrames;
        if (payloadBytes == 0)
        {
            consumedBytes = newline + 1;
            continue;
        }
        const sdk::ReceiveResult result = connection.receive(
            std::string_view(inboundBuffer.constData() + consumedBytes,
                             static_cast<std::size_t>(payloadBytes)));
        consumedBytes = newline + 1;
        if (!result.accepted) {
            const QString reason = currentLifecycle == Lifecycle::Failed && !detail.isEmpty()
                                       ? detail
                                       : result.error ? QString::fromStdString(result.error->message)
                                                      : QStringLiteral("Frontend SDK rejected a server message");
            if (!result.error || !result.error->retryable)
                failWithoutReconnect(reason);
            else
                setLifecycle(Lifecycle::Failed, reason);
            socket.abort();
            return;
        }
        if (!connection.isOpen())
            break;
    }

    inboundOffset = consumedBytes;
    compactInbound();
    if (!hasCompleteInboundFrame()
        && inboundBuffer.size() - inboundOffset > maximumFrameBytes + 1) {
        rejectOversizedFrame();
        return;
    }
    if (connection.isOpen()
        && (socket.bytesAvailable() > 0 || hasCompleteInboundFrame()))
        scheduleSocketRead();
}

void FrontendSession::clearInbound() noexcept
{
    inboundBuffer.clear();
    inboundOffset = 0;
}

bool FrontendSession::hasCompleteInboundFrame() const noexcept
{
    return inboundBuffer.indexOf('\n', inboundOffset) >= 0;
}

void FrontendSession::compactInbound() noexcept
{
    if (inboundOffset <= 0)
        return;
    if (inboundOffset >= inboundBuffer.size()) {
        clearInbound();
        return;
    }
    if (inboundOffset < inboundCompactionThreshold
        || inboundOffset < inboundBuffer.size() / 2)
        return;
    inboundBuffer.remove(0, inboundOffset);
    inboundOffset = 0;
}

void FrontendSession::scheduleSocketRead()
{
    if (receiveContinuationScheduled || localShutdown)
        return;
    receiveContinuationScheduled = true;
    QTimer::singleShot(0, this,
                       [this]
                       {
                           if (!receiveContinuationScheduled)
                               return;
                           receiveContinuationScheduled = false;
                           if (connection.isOpen()
                               && (socket.bytesAvailable() > 0 || hasCompleteInboundFrame()))
                               socketReadyRead();
                       });
}

void FrontendSession::reconcileRequestedThreadReads()
{
    const bool threadListComplete = currentState.threadList().value
                                    && currentState.threadList().value->complete;
    for (auto iterator = requestedThreadReads.begin(); iterator != requestedThreadReads.end();)
    {
        const auto* thread = currentState.thread(*iterator);
        if ((thread && thread->fullyLoaded) || (!thread && threadListComplete))
            iterator = requestedThreadReads.erase(iterator);
        else
            ++iterator;
    }
}

void FrontendSession::beginArchivedThreadRefresh()
{
    if (currentLifecycle != Lifecycle::Ready || archivedThreadListInFlight
        || archivedThreadListComplete)
        return;
    requestArchivedThreadPage(connectionGeneration, std::nullopt);
}

void FrontendSession::requestArchivedThreadPage(std::uint64_t generation,
                                                std::optional<std::string> cursor)
{
    if (generation != connectionGeneration || currentLifecycle != Lifecycle::Ready
        || archivedThreadListComplete || archivedThreadListInFlight)
        return;
    const std::string cursorIdentity = cursor.value_or(std::string{});
    if (archivedThreadListCursors.size() >= maximumArchivedThreadListPages
        || !archivedThreadListCursors.insert(cursorIdentity).second) {
        finishArchivedThreadRefresh(
            QStringLiteral("Archived thread listing stopped at an invalid pagination boundary"));
        return;
    }

    ai::openai::codex::typed::ThreadListParams parameters;
    parameters.archived = true;
    parameters.limit = archivedThreadListPageSize;
    if (cursor)
        parameters.cursor = std::move(*cursor);

    archivedThreadListInFlight = true;
    const auto submission = client->threads().list(
        std::move(parameters),
        [this, generation](const sdk::OperationResult<sdk::ThreadListResult>& result) {
            if (generation != connectionGeneration || currentLifecycle != Lifecycle::Ready)
                return;
            archivedThreadListInFlight = false;
            if (!result || !result.value) {
                finishArchivedThreadRefresh(operationError(
                    result.error, QStringLiteral("Archived threads could not be restored")));
                return;
            }
            if (result.value->nextCursor) {
                requestArchivedThreadPage(generation, result.value->nextCursor);
                return;
            }
            finishArchivedThreadRefresh();
        });
    if (const auto error = submissionError(submission,
                                           QStringLiteral("Archived thread listing could not be submitted"))) {
        archivedThreadListInFlight = false;
        finishArchivedThreadRefresh(*error);
    }
}

void FrontendSession::finishArchivedThreadRefresh(QString diagnostic)
{
    archivedThreadListInFlight = false;
    if (!diagnostic.isEmpty()) {
        reportDiagnostic(std::move(diagnostic));
        return;
    }

    archivedThreadListComplete = true;

    detail::StateUpdateScope scope;
    scope.allThreadsAffected = true;
    scope.allInspectorsAffected = true;
    scope.sidebarAffected = true;
    scope.hasPresentationChange = true;
    emit stateChanged(scope);
}

void FrontendSession::beginModelCatalogRefresh()
{
    if (currentLifecycle != Lifecycle::Ready || modelListInFlight || modelListComplete)
        return;
    requestModelCatalogPage(connectionGeneration, std::nullopt);
}

void FrontendSession::requestModelCatalogPage(std::uint64_t generation,
                                              std::optional<std::string> cursor)
{
    if (generation != connectionGeneration || currentLifecycle != Lifecycle::Ready
        || modelListInFlight || modelListComplete)
        return;
    const std::string cursorIdentity = cursor.value_or(std::string{});
    if (modelListCursors.size() >= maximumModelListPages
        || !modelListCursors.insert(cursorIdentity).second) {
        finishModelCatalogRefresh(
            QStringLiteral("Model listing stopped at an invalid pagination boundary"));
        return;
    }

    ai::openai::codex::typed::ModelListParams parameters;
    parameters.includeHidden = false;
    parameters.limit = modelListPageSize;
    if (cursor)
        parameters.cursor = std::move(*cursor);

    modelListInFlight = true;
    const auto submission = client->models().list(
        std::move(parameters),
        [this, generation](
            const sdk::OperationResult<ai::openai::codex::typed::ModelListResponse>& result) {
            if (generation != connectionGeneration || currentLifecycle != Lifecycle::Ready)
                return;
            modelListInFlight = false;
            if (!result || !result.value) {
                finishModelCatalogRefresh(operationError(
                    result.error, QStringLiteral("Available models could not be loaded")));
                return;
            }
            for (const auto& model : result.value->data) {
                if (model.hidden || model.model.value.empty())
                    continue;
                const bool duplicate = std::ranges::any_of(
                    pendingModelCatalog,
                    [&model](const auto& existing) {
                        return existing.model.value == model.model.value;
                    });
                if (!duplicate)
                    pendingModelCatalog.push_back(model);
            }
            if (result.value->nextCursor.hasValue()) {
                requestModelCatalogPage(generation, *result.value->nextCursor);
                return;
            }
            finishModelCatalogRefresh();
        });
    if (const auto error = submissionError(
            submission, QStringLiteral("Available-model listing could not be submitted"))) {
        modelListInFlight = false;
        finishModelCatalogRefresh(*error);
    }
}

void FrontendSession::finishModelCatalogRefresh(QString diagnostic)
{
    modelListInFlight = false;
    modelListComplete = true;
    if (!diagnostic.isEmpty()) {
        pendingModelCatalog.clear();
        reportDiagnostic(std::move(diagnostic));
        return;
    }
    if (availableModelCatalog == pendingModelCatalog)
        return;
    availableModelCatalog = std::move(pendingModelCatalog);
    QTimer::singleShot(0, this, [this] { emit modelCatalogChanged(); });
}

void FrontendSession::socketDisconnected()
{
    const bool disconnectedBeforeReady = !localShutdown && !synchronizedCurrentConnection;
    if (connection.isOpen()) {
        if (localShutdown)
            connection.transportDisconnected();
        else
            connection.transportDisconnected(sdk::TransportError{"Unix backend disconnected", true});
    }
    connection = Connection{};
    clearOutbound();
    clearInbound();
    receiveContinuationScheduled = false;
    requestedThreadReads.clear();
    if (!localShutdown) {
        if (disconnectedBeforeReady) {
            ++consecutivePreReadyDisconnects;
            if (consecutivePreReadyDisconnects >= maximumConsecutivePreReadyDisconnects) {
                failWithoutReconnect(
                    QStringLiteral("Backend disconnected before synchronization completed on %1 consecutive connections")
                        .arg(consecutivePreReadyDisconnects));
                return;
            }
        }
        if (currentLifecycle != Lifecycle::Failed)
            setLifecycle(Lifecycle::Disconnected);
        scheduleReconnect();
    }
}

void FrontendSession::socketFailed(QLocalSocket::LocalSocketError)
{
    if (localShutdown)
        return;
    if (connection.isOpen())
        connection.transportDisconnected(sdk::TransportError{socket.errorString().toStdString(), true});
    connection = Connection{};
    clearOutbound();
    clearInbound();
    receiveContinuationScheduled = false;
    requestedThreadReads.clear();
    if (automaticReconnectEnabled && currentLifecycle != Lifecycle::Failed)
        setLifecycle(Lifecycle::Failed, socket.errorString());
    scheduleReconnect();
}

void FrontendSession::handleConnectionStateChange(const sdk::ConnectionStateChange& change)
{
    if (change.error) {
        if (!change.error->retryable) {
            automaticReconnectEnabled = false;
            reconnectTimer.stop();
        }
        setLifecycle(Lifecycle::Failed, QString::fromStdString(change.error->message));
        return;
    }

    switch (change.current) {
        case sdk::ConnectionState::Connecting:
            setLifecycle(Lifecycle::Connecting);
            break;
        case sdk::ConnectionState::Authenticating:
            setLifecycle(Lifecycle::Authenticating);
            break;
        case sdk::ConnectionState::Synchronizing:
            setLifecycle(Lifecycle::Synchronizing);
            break;
        case sdk::ConnectionState::Ready:
            // onSynchronized owns the Ready transition so consumers never
            // treat a transient reconnect projection as authoritative.
            break;
        case sdk::ConnectionState::Disconnected:
        case sdk::ConnectionState::Closed:
            if (currentLifecycle != Lifecycle::Failed)
                setLifecycle(Lifecycle::Disconnected);
            break;
        case sdk::ConnectionState::Closing:
            break;
    }
}

void FrontendSession::reportDiagnostic(QString message)
{
    if (message.isEmpty() || diagnosticDetail == message)
        return;
    diagnosticDetail = std::move(message);
    emit statusChanged();
}

void FrontendSession::failWithoutReconnect(QString reason)
{
    // Retrying the same stream after a protocol/state rejection only replays
    // the offending suffix. The explicit reconnect action remains available
    // after the underlying incompatibility is corrected.
    automaticReconnectEnabled = false;
    reconnectTimer.stop();
    clearOutbound();
    setLifecycle(Lifecycle::Failed, std::move(reason));
}

void FrontendSession::scheduleReconnect()
{
    if (localShutdown || !automaticReconnectEnabled || reconnectTimer.isActive())
        return;
    reconnectTimer.start(reconnectDelayMs);
    reconnectDelayMs = std::min(reconnectDelayMs * 2, maximumReconnectDelayMs);
}

void FrontendSession::retryConnection()
{
    if (localShutdown)
        return;
    if (socket.state() != QLocalSocket::UnconnectedState) {
        scheduleReconnect();
        return;
    }
    startConnection();
}

void FrontendSession::resetReconnectPolicy()
{
    automaticReconnectEnabled = true;
    reconnectTimer.stop();
    reconnectDelayMs = initialReconnectDelayMs;
    consecutivePreReadyDisconnects = 0;
    synchronizedCurrentConnection = false;
}

FrontendSession::SendResult FrontendSession::send(OutboundMessage&& message)
{
    SendResult result = sendToTransport(
        std::move(message),
        socket.state() == QLocalSocket::ConnectedState,
        socket.bytesToWrite(),
        [this](const char* bytes, qint64 size) { return socket.write(bytes, size); });
    if (result.status == sdk::SendStatus::Failed && result.error && !socket.errorString().isEmpty())
        result.error->message = socket.errorString().toStdString();
    return result;
}

FrontendSession::SendResult FrontendSession::sendToTransport(OutboundMessage&& message,
                                                              bool transportConnected,
                                                              qint64 socketBufferedBytes,
                                                              const OutboundWriter& writer) noexcept
{
    if (!transportConnected) {
        eraseFrame(message.compactJson);
        return {sdk::SendStatus::Closed, sdk::TransportError{"Unix backend socket is closed", true}};
    }
    SendResult result = acceptOutbound(std::move(message), socketBufferedBytes, writer);
    if (result.status == sdk::SendStatus::Accepted && !pendingWrites.empty())
        scheduleOutboundDrain();
    return result;
}

FrontendSession::SendResult FrontendSession::acceptOutbound(OutboundMessage&& message,
                                                             qint64 socketBufferedBytes,
                                                             const OutboundWriter& writer) noexcept
{
    if (outboundClearPending) {
        eraseFrame(message.compactJson);
        return {sdk::SendStatus::Closed,
                sdk::TransportError{"Unix backend connection is closing", true}};
    }
    std::string frame = std::move(message.compactJson);
    eraseFrame(message.compactJson);
    try {
        frame.push_back('\n');
    } catch (...) {
        eraseFrame(frame);
        return {sdk::SendStatus::Failed,
                sdk::TransportError{"Unix backend frame allocation failed", true}};
    }

    const qint64 frameBytes = static_cast<qint64>(frame.size());
    const qint64 bufferedBytes = std::max<qint64>(0, socketBufferedBytes);
    const bool lacksCapacity = bufferedBytes > maximumBufferedOutboundBytes
                               || pendingWriteBytes > maximumBufferedOutboundBytes - bufferedBytes
                               || frameBytes > maximumBufferedOutboundBytes - bufferedBytes - pendingWriteBytes;
    if (lacksCapacity) {
        eraseFrame(frame);
        return {sdk::SendStatus::Backpressure,
                sdk::TransportError{"Unix backend output queue is full", true}};
    }

    const bool wasEmpty = pendingWrites.empty();
    try {
        pendingWrites.emplace_back();
        pendingWrites.back().frame = std::move(frame);
        eraseFrame(frame);
    } catch (...) {
        eraseFrame(frame);
        return {sdk::SendStatus::Failed,
                sdk::TransportError{"Unix backend output queue allocation failed", true}};
    }
    pendingWriteBytes += frameBytes;
    if (!wasEmpty)
        return {sdk::SendStatus::Accepted, std::nullopt};

    const std::uint64_t acceptedEpoch = outboundEpoch;
    const DrainResult drained = drainOutbound(writer);
    if (outboundEpoch != acceptedEpoch || drained == DrainResult::Reset)
        return {sdk::SendStatus::Closed,
                sdk::TransportError{"Unix backend connection changed during write", true}};
    if (drained == DrainResult::Failed) {
        clearOutbound();
        return {sdk::SendStatus::Failed,
                sdk::TransportError{"Unix backend socket write failed", true}};
    }
    return {sdk::SendStatus::Accepted, std::nullopt};
}

FrontendSession::DrainResult FrontendSession::drainOutbound(const OutboundWriter& writer) noexcept
{
    if (drainingOutbound || pendingWrites.empty())
        return DrainResult::Blocked;
    drainingOutbound = true;
    const std::uint64_t drainingEpoch = outboundEpoch;
    const qint64 offset = pendingWrites.front().offset;
    const qint64 remaining = static_cast<qint64>(pendingWrites.front().frame.size()) - offset;
    qint64 written = -1;
    try {
        written = writer(pendingWrites.front().frame.data() + offset, remaining);
    } catch (...) {
        written = -1;
    }
    drainingOutbound = false;

    if (outboundClearPending) {
        clearOutbound();
        return DrainResult::Reset;
    }
    if (outboundEpoch != drainingEpoch)
        return DrainResult::Reset;
    if (written < 0 || written > remaining)
        return DrainResult::Failed;
    if (written == 0)
        return DrainResult::Blocked;

    PendingWrite& pending = pendingWrites.front();
    pending.offset += written;
    pendingWriteBytes -= written;
    if (pending.offset == static_cast<qint64>(pending.frame.size())) {
        eraseFrame(pending.frame);
        pendingWrites.pop_front();
        if (pendingWrites.empty())
            outboundDrainTimer.stop();
    }
    return DrainResult::Progress;
}

void FrontendSession::socketBytesWritten(qint64)
{
    drainSocketWrites();
}

void FrontendSession::drainSocketWrites()
{
    outboundDrainTimer.stop();
    if (socket.state() != QLocalSocket::ConnectedState || pendingWrites.empty())
        return;
    const DrainResult result = drainOutbound(
        [this](const char* bytes, qint64 size) { return socket.write(bytes, size); });
    if (result == DrainResult::Failed) {
        socketFailed(socket.error());
        if (socket.state() != QLocalSocket::UnconnectedState)
            socket.abort();
        return;
    }
    if (!pendingWrites.empty())
        scheduleOutboundDrain();
}

void FrontendSession::scheduleOutboundDrain()
{
    if (!pendingWrites.empty()
        && !outboundDrainTimer.isActive())
        outboundDrainTimer.start(outboundDrainRetryMs);
}

void FrontendSession::clearOutbound() noexcept
{
    outboundDrainTimer.stop();
    ++outboundEpoch;
    if (drainingOutbound) {
        outboundClearPending = true;
        return;
    }
    outboundClearPending = false;
    for (PendingWrite& pending : pendingWrites)
        eraseFrame(pending.frame);
    pendingWrites.clear();
    pendingWriteBytes = 0;
}

void FrontendSession::closeTransport(QString reason) noexcept
{
    clearOutbound();
    if (!reason.isEmpty())
        detail = std::move(reason);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.abort();
}

void FrontendSession::setLifecycle(Lifecycle value, QString newDetail)
{
    QString nextDetail;
    if (value == Lifecycle::Failed || !newDetail.isEmpty())
        nextDetail = std::move(newDetail);
    if (currentLifecycle == value && detail == nextDetail)
        return;
    currentLifecycle = value;
    detail = std::move(nextDetail);
    diagnosticDetail.clear();
    emit lifecycleChanged();
}

} // namespace codexui
