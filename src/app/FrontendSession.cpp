// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <ai/openai/codex/frontend/Security.h>
#include <ai/openai/codex/frontend/client/Controller.h>
#include <ai/openai/codex/frontend/client/Requests.h>
#include <ai/openai/codex/frontend/client/Threads.h>
#include <ai/openai/codex/frontend/client/Turns.h>

#include <QByteArray>
#include <QDir>
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

} // namespace detail

namespace {

constexpr qsizetype maximumFrameBytes = 16 * 1024 * 1024;
constexpr qsizetype maximumPromptBytes = 128 * 1024;
constexpr qsizetype maximumReceiveBatchBytes = 1024 * 1024;
constexpr int maximumReceiveBatchFrames = 32;

struct StateUpdateScope
{
    QStringList affectedThreadIds;
    bool allThreadsAffected = false;
    bool inspectorAffected = false;
    bool hasPresentationChange = false;
};

StateUpdateScope stateUpdateScope(const sdk::StateUpdate& update)
{
    StateUpdateScope scope;
    const auto addThread = [&scope](std::string_view id)
    {
        const QString threadId = QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        if (!scope.affectedThreadIds.contains(threadId))
            scope.affectedThreadIds.append(threadId);
    };

    if (update.changes.empty())
    {
        scope.allThreadsAffected = true;
        scope.inspectorAffected = true;
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
                    // Info shows the immutable State revision. Its label can be
                    // updated in place without rebuilding the conversation.
                    scope.inspectorAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::StateReplacedChange>)
                {
                    scope.allThreadsAffected = true;
                    scope.inspectorAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::ThreadUpsertedChange>
                                   || std::is_same_v<Change, sdk::ThreadRemovedChange>)
                {
                    addThread(value.threadId.value);
                    // The selected Inspector can show status/model facts from
                    // a linked subagent thread even when its conversation is
                    // not selected.
                    scope.inspectorAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::TurnUpsertedChange>)
                {
                    if (const auto* turn = update.state.turn(value.turnId))
                        addThread(turn->threadId.value);
                    else
                        scope.allThreadsAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::ItemUpsertedChange>
                                   || std::is_same_v<Change, sdk::ItemContentReplacedChange>)
                {
                    const auto* item = update.state.item(value.itemId);
                    if (item && item->threadId)
                        addThread(item->threadId->value);
                    else
                        scope.allThreadsAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::PendingRequestsUpdatedChange>)
                {
                    // Pending requests contribute to activity-card status. The
                    // change has no thread identity, especially on removal.
                    scope.allThreadsAffected = true;
                    scope.inspectorAffected = true;
                }
                else
                {
                    // Controller, pending-request, provider, capacity, and other
                    // domain projections can affect the Inspector and controls,
                    // but do not require rebuilding an unchanged conversation.
                    scope.inspectorAffected = true;
                }
                scope.hasPresentationChange = true;
            },
            change);
    }
    return scope;
}

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
        const StateUpdateScope scope = stateUpdateScope(update);
        if (scope.hasPresentationChange)
            emit stateChanged(scope.affectedThreadIds, scope.allThreadsAffected, scope.inspectorAffected);
    };
    callbacks.onSynchronized = [this](const sdk::SynchronizationInfo& info) {
        reconnectDelayMs = initialReconnectDelayMs;
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
        emit stateChanged({}, true, true);
    };
    callbacks.onDiagnostic = [this](const sdk::Diagnostic& diagnostic) {
        if (diagnostic.severity == sdk::Diagnostic::Severity::Error)
            reportDiagnostic(QString::fromStdString(diagnostic.message));
    };
    client = std::make_unique<Client>(std::move(options), std::move(callbacks));

    connect(&socket, &QLocalSocket::connected, this, &FrontendSession::socketConnected);
    connect(&socket, &QLocalSocket::readyRead, this, &FrontendSession::socketReadyRead);
    connect(&socket, &QLocalSocket::disconnected, this, &FrontendSession::socketDisconnected);
    connect(&socket, &QLocalSocket::errorOccurred, this, &FrontendSession::socketFailed);
    reconnectTimer.setSingleShot(true);
    connect(&reconnectTimer, &QTimer::timeout, this, &FrontendSession::retryConnection);
}

FrontendSession::~FrontendSession()
{
    localShutdown = true;
    reconnectTimer.stop();
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
    if (connection.isOpen())
        connection.close("User requested reconnect");
    connection = Connection{};
    inboundBuffer.clear();
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
    if (prompt.toUtf8().size() <= maximumPromptBytes)
        return std::nullopt;
    return QStringLiteral("Prompt exceeds the 128 KiB UTF-8 submission limit");
}

const sdk::State& FrontendSession::state() const noexcept
{
    return currentState;
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
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    sdk::Submission submission = client->threads().start(
        ai::openai::codex::typed::ThreadStartParams{},
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
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");

    const std::string requestedId = threadId.toStdString();
    ai::openai::codex::typed::ThreadResumeParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{requestedId};
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
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    if (const auto error = promptValidationError(prompt))
        return error;

    ai::openai::codex::typed::TurnStartParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    ai::openai::codex::typed::TextInput input;
    const QByteArray promptUtf8 = prompt.toUtf8();
    input.text.assign(promptUtf8.constData(), static_cast<std::size_t>(promptUtf8.size()));
    parameters.input.emplace_back(std::move(input));
    sdk::Submission submission = client->turns().start(
        std::move(parameters),
        [completion = std::move(completion)](const sdk::OperationResult<sdk::TurnStartResult>& result) {
            completion(result ? QString{} : operationError(result.error, QStringLiteral("Turn could not be started")));
        });
    return submissionError(submission, QStringLiteral("Turn submission was not accepted"));
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
    inboundBuffer.clear();
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
        inboundBuffer.clear();
        failWithoutReconnect(QStringLiteral("Backend frame exceeds the SDK input limit"));
        socket.abort();
    };

    qsizetype consumedBytes = 0;
    qsizetype receivedBytes = 0;
    int receivedFrames = 0;
    while (receivedFrames < maximumReceiveBatchFrames && receivedBytes < maximumReceiveBatchBytes)
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
            const QString reason = result.error ? QString::fromStdString(result.error->message)
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

    if (consumedBytes > 0)
        inboundBuffer.remove(0, consumedBytes);
    if (inboundBuffer.indexOf('\n') < 0 && inboundBuffer.size() > maximumFrameBytes + 1) {
        rejectOversizedFrame();
        return;
    }
    if (connection.isOpen()
        && (socket.bytesAvailable() > 0 || inboundBuffer.indexOf('\n') >= 0))
        scheduleSocketRead();
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
                               && (socket.bytesAvailable() > 0 || inboundBuffer.indexOf('\n') >= 0))
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

void FrontendSession::socketDisconnected()
{
    if (connection.isOpen()) {
        if (localShutdown)
            connection.transportDisconnected();
        else
            connection.transportDisconnected(sdk::TransportError{"Unix backend disconnected", true});
    }
    connection = Connection{};
    inboundBuffer.clear();
    receiveContinuationScheduled = false;
    requestedThreadReads.clear();
    if (!localShutdown) {
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
    inboundBuffer.clear();
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
}

FrontendSession::SendResult FrontendSession::send(OutboundMessage message) noexcept
{
    if (socket.state() != QLocalSocket::ConnectedState)
        return {sdk::SendStatus::Closed, sdk::TransportError{"Unix backend socket is closed", true}};
    QByteArray frame = QByteArray::fromStdString(message.compactJson);
    frame.append('\n');
    const qint64 written = socket.write(frame);
    if (written != frame.size())
        return {sdk::SendStatus::Failed, sdk::TransportError{socket.errorString().toStdString(), true}};
    return {sdk::SendStatus::Accepted, std::nullopt};
}

void FrontendSession::closeTransport(QString reason) noexcept
{
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
