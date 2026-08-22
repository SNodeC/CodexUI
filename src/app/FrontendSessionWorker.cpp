// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSessionWorker.h"

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
#include <QThread>

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
        if (ids.contains(threadId))
            return true;
        if (ids.size() >= maximumCoalescedPresentationIdentities)
            return false;
        ids.append(threadId);
        return true;
    };
    const auto addThread = [&scope, &addUnique](std::string_view id) {
        if (!scope.allThreadsAffected
            && !addUnique(scope.affectedThreadIds, id))
            scope.allThreadsAffected = true;
    };
    const auto addFullyAffectedThread = [&scope, &addThread, &addUnique](std::string_view id) {
        addThread(id);
        if (!scope.allThreadsAffected
            && !addUnique(scope.fullyAffectedThreadIds, id))
            scope.allThreadsAffected = true;
    };
    const auto addInspectorThread = [&scope, &addUnique](std::string_view id) {
        if (!scope.allInspectorsAffected
            && !addUnique(scope.affectedInspectorThreadIds, id))
            scope.allInspectorsAffected = true;
    };
    const auto addSidebarThread = [&scope, &addUnique](std::string_view id) {
        scope.sidebarAffected = true;
        if (!scope.allSidebarThreadsAffected
            && !addUnique(scope.affectedSidebarThreadIds, id))
            scope.allSidebarThreadsAffected = true;
    };
    const auto markThreadAndInspector = [&addFullyAffectedThread, &addInspectorThread](std::string_view id) {
        addFullyAffectedThread(id);
        addInspectorThread(id);
    };
    const auto addItemContent = [&scope, &addThread](const auto& value,
                                                     auto append) {
        const auto asQString = [](std::string_view id) {
            return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        };
        const QString threadId = asQString(value.threadId->value);
        const QString turnId = asQString(value.turnId->value);
        const QString itemId = asQString(value.itemId.value);
        addThread(value.threadId->value);
        if (scope.allThreadsAffected)
            return;
        StateUpdateScope::ItemContentIdentity identity{
            threadId,
            turnId,
            itemId,
            value.channel,
            std::move(append),
        };
        const auto existing = std::find_if(
            scope.affectedItemContents.begin(),
            scope.affectedItemContents.end(),
            [&identity](const auto& candidate) {
                return candidate.threadId == identity.threadId
                       && candidate.turnId == identity.turnId
                       && candidate.itemId == identity.itemId
                       && candidate.channel == identity.channel;
            });
        if (existing == scope.affectedItemContents.end()) {
            if (static_cast<qsizetype>(scope.affectedItemContents.size())
                >= maximumCoalescedPresentationIdentities) {
                scope.allThreadsAffected = true;
                return;
            }
            if (identity.append) {
                const std::uint64_t bytes =
                    static_cast<std::uint64_t>(
                        identity.append->deltaUtf8.size());
                if (scope.coalescedContentDeltaBytes
                        > maximumCoalescedContentDeltaBytes
                    || bytes
                           > maximumCoalescedContentDeltaBytes
                                 - scope.coalescedContentDeltaBytes) {
                    identity.append.reset();
                } else {
                    scope.coalescedContentDeltaBytes += bytes;
                }
            }
            scope.affectedItemContents.push_back(std::move(identity));
        } else {
            if (existing->append) {
                const std::uint64_t bytes =
                    static_cast<std::uint64_t>(
                        existing->append->deltaUtf8.size());
                scope.coalescedContentDeltaBytes =
                    bytes <= scope.coalescedContentDeltaBytes
                        ? scope.coalescedContentDeltaBytes - bytes
                        : 0;
            }
            existing->append.reset();
        }
    };

    if (update.changes.empty())
    {
        scope.allThreadsAffected = true;
        scope.allInspectorsAffected = true;
        scope.allSidebarThreadsAffected = true;
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
                    scope.allSidebarThreadsAffected = true;
                    scope.sidebarAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::ThreadUpsertedChange>)
                {
                    addFullyAffectedThread(value.threadId.value);
                    addSidebarThread(value.threadId.value);
                    // The selected Inspector can show status/model facts from
                    // a linked subagent thread even when its conversation is
                    // not selected. Workbench resolves this identity against
                    // its retained Inspector dependency set.
                    addInspectorThread(value.threadId.value);
                }
                else if constexpr (std::is_same_v<Change, sdk::ThreadRemovedChange>)
                {
                    addFullyAffectedThread(value.threadId.value);
                    if (!addUnique(scope.removedThreadIds,
                                   value.threadId.value)) {
                        scope.removedThreadIdsOverflowed = true;
                        scope.allThreadsAffected = true;
                    }
                    addSidebarThread(value.threadId.value);
                    addInspectorThread(value.threadId.value);
                }
                else if constexpr (std::is_same_v<Change, sdk::TurnUpsertedChange>)
                {
                    if (const auto* turn = update.state.turn(value.turnId)) {
                        markThreadAndInspector(turn->threadId.value);
                        addSidebarThread(turn->threadId.value);
                    } else {
                        scope.allThreadsAffected = true;
                        scope.allInspectorsAffected = true;
                        scope.allSidebarThreadsAffected = true;
                        scope.sidebarAffected = true;
                    }
                }
                else if constexpr (std::is_same_v<Change, sdk::ItemUpsertedChange>)
                {
                    if (value.threadId) {
                        // A new or changed item requires bounded timeline
                        // reconciliation, but it does not invalidate the
                        // complete selected-thread presentation. Keeping it
                        // out of fullyAffectedThreadIds lets ConversationWidget
                        // retain and reconcile its existing segment widgets.
                        addThread(value.threadId->value);
                        addInspectorThread(value.threadId->value);
                    }
                    else if (value.turnId) {
                        if (const auto* turn = update.state.turn(*value.turnId)) {
                            addThread(turn->threadId.value);
                            addInspectorThread(turn->threadId.value);
                        }
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
                else if constexpr (std::is_same_v<Change, sdk::ItemContentAppendedChange>
                                   || std::is_same_v<Change, sdk::ItemContentReplacedChange>)
                {
                    // Streaming text/output changes affect the conversation,
                    // not Sidebar thread facts or Inspector semantics. The SDK
                    // distinguishes an authoritative exact append hint from a
                    // replacement fallback in its canonical public changes.
                    if (value.threadId && value.turnId) {
                        if constexpr (std::is_same_v<Change,
                                                     sdk::ItemContentAppendedChange>) {
                            if (value.delta.size()
                                <= maximumCoalescedContentDeltaBytes) {
                                addItemContent(
                                    value,
                                    StateUpdateScope::ItemContentAppend{
                                        value.baseContentBytes,
                                        value.discardPrefixBytes,
                                        QByteArray(
                                            value.delta.data(),
                                            static_cast<qsizetype>(
                                                value.delta.size())),
                                    });
                            } else {
                                addItemContent(
                                    value,
                                    std::optional<StateUpdateScope::
                                                      ItemContentAppend>{});
                            }
                        } else {
                            addItemContent(value,
                                           std::optional<StateUpdateScope::
                                                             ItemContentAppend>{});
                        }
                    }
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
                    scope.allSidebarThreadsAffected = true;
                    scope.sidebarAffected = true;
                }
                else if constexpr (std::is_same_v<Change, sdk::ThreadListUpdatedChange>)
                {
                    // A list replacement has no per-thread removal identity.
                    scope.allThreadsAffected = true;
                    scope.allInspectorsAffected = true;
                    scope.allSidebarThreadsAffected = true;
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
    if (scope.allThreadsAffected) {
        scope.affectedThreadIds.clear();
        scope.fullyAffectedThreadIds.clear();
        scope.affectedItemContents.clear();
        scope.coalescedContentDeltaBytes = 0;
    }
    if (scope.allInspectorsAffected)
        scope.affectedInspectorThreadIds.clear();
    if (scope.allSidebarThreadsAffected)
        scope.affectedSidebarThreadIds.clear();
    return scope;
}

} // namespace detail

namespace {

constexpr qsizetype maximumReceiveBatchBytes = 1024 * 1024;
constexpr qsizetype inboundCompactionThreshold = 256 * 1024;
constexpr int minimumReceiveBatchFrames = 1;
constexpr int maximumReceiveBatchFrames = 256;
constexpr qint64 maximumReceiveBatchTimeMs = 4;
constexpr std::uint32_t archivedThreadListPageSize = 100;
constexpr std::size_t maximumArchivedThreadListPages = 64;
constexpr std::uint32_t modelListPageSize = 100;
constexpr std::size_t maximumModelListPages = 64;

bool synchronizedStateOmitsThreads(const sdk::State& state)
{
    const auto capacity = state.capacityProvenance();
    return capacity && capacity->omittedThreads > 0;
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

FrontendSessionWorker::FrontendSessionWorker(QObject* parent)
    : QObject(parent)
{
    sdk::ClientOptions options;
    // CodexUI consumes includeTurns thread/read results through AISuite's
    // authoritative State publication. This is an observed mechanism, not a
    // representation request: requiring it validates the server's Welcome
    // while leaving Hello's representation selection unchanged.
    options.requiredCapabilities.push_back(
        frontend::FrontendCapability::ThreadReadStateEffects);
    maximumFrameBytes = options.maximumInboundMessageBytes;
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
        handleStateUpdate(update);
    };
    callbacks.onSynchronized = [this](const sdk::SynchronizationInfo& info) {
        reconnectDelayMs = initialReconnectDelayMs;
        consecutivePreReadyDisconnects = 0;
        synchronizedCurrentConnection = true;
        automaticReconnectEnabled = true;
        currentState = info.state;
        reconcileIncompleteThreadReadAttempts();
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
        scope.allSidebarThreadsAffected = true;
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

    connect(&socket, &QLocalSocket::connected, this, &FrontendSessionWorker::socketConnected);
    connect(&socket, &QLocalSocket::readyRead, this, &FrontendSessionWorker::socketReadyRead);
    connect(&socket, &QLocalSocket::bytesWritten, this, &FrontendSessionWorker::socketBytesWritten);
    connect(&socket, &QLocalSocket::disconnected, this, &FrontendSessionWorker::socketDisconnected);
    connect(&socket, &QLocalSocket::errorOccurred, this, &FrontendSessionWorker::socketFailed);
    reconnectTimer.setSingleShot(true);
    connect(&reconnectTimer, &QTimer::timeout, this, &FrontendSessionWorker::retryConnection);
    outboundDrainTimer.setSingleShot(true);
    connect(&outboundDrainTimer, &QTimer::timeout, this, &FrontendSessionWorker::drainSocketWrites);
}

FrontendSessionWorker::~FrontendSessionWorker()
{
    shutdown();
}

void FrontendSessionWorker::shutdown()
{
    if (localShutdown)
        return;
    localShutdown = true;
    reconnectTimer.stop();
    clearOutbound();
    if (connection.isOpen())
        connection.close("CodexUI is closing");
    client->close("CodexUI is closing");
    socket.abort();
}

void FrontendSessionWorker::connectToBackend()
{
    resetReconnectPolicy();
    startConnection();
}

void FrontendSessionWorker::reconnectToBackend()
{
    reconnectTimer.stop();
    clearOutbound();
    if (connection.isOpen())
        connection.close("User requested reconnect");
    connection = Connection{};
    clearInbound();
    receiveContinuationScheduled = false;
    threadReadsInFlight.clear();
    attemptedIncompleteThreadReads.clear();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.abort();
        resetReconnectPolicy();
        reconnectTimer.start(0);
        return;
    }
    resetReconnectPolicy();
    startConnection();
}

void FrontendSessionWorker::startConnection()
{
    if (socket.state() != QLocalSocket::UnconnectedState || connection.isOpen())
        return;
    ++connectionGeneration;
    synchronizedCurrentConnection = false;
    preReadyFailureRecordedCurrentConnection = false;
    archivedThreadListCursors.clear();
    archivedThreadListInFlight = false;
    archivedThreadListStatus = ArchivedThreadDiscoveryStatus::InProgress;
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

FrontendSessionWorker::Lifecycle FrontendSessionWorker::lifecycle() const noexcept
{
    return currentLifecycle;
}

QString FrontendSessionWorker::statusText() const
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

std::optional<QString> FrontendSessionWorker::promptValidationError(const QString& prompt)
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

const sdk::State& FrontendSessionWorker::state() const noexcept
{
    return currentState;
}

const std::vector<ai::openai::codex::typed::Model>& FrontendSessionWorker::modelCatalog() const noexcept
{
    return availableModelCatalog;
}

bool FrontendSessionWorker::archivedThreadDiscoveryComplete() const noexcept
{
    return archivedThreadListStatus == ArchivedThreadDiscoveryStatus::Complete;
}

bool FrontendSessionWorker::archivedThreadDiscoveryTerminal() const noexcept
{
    return archivedThreadListStatus != ArchivedThreadDiscoveryStatus::InProgress;
}

FrontendSessionWorker::ArchivedThreadDiscoveryStatus
FrontendSessionWorker::archivedThreadDiscoveryStatus() const noexcept
{
    return archivedThreadListStatus;
}

bool FrontendSessionWorker::ownsController() const noexcept
{
    const auto& projection = currentState.controller();
    return projection.value && projection.value->ownedByThisClient;
}

std::uint64_t FrontendSessionWorker::generation() const noexcept
{
    return connectionGeneration;
}

bool FrontendSessionWorker::transportAffinityIsCurrentThread() const noexcept
{
    QThread* current = QThread::currentThread();
    return thread() == current && socket.thread() == current
           && reconnectTimer.thread() == current
           && outboundDrainTimer.thread() == current;
}

void FrontendSessionWorker::loadThread(const QString& threadId,
                                       bool retryIncomplete)
{
    if (currentLifecycle != Lifecycle::Ready || threadId.isEmpty())
        return;
    const std::string id = threadId.toStdString();
    const auto* thread = currentState.thread(id);
    const bool missingFromBoundedState = !thread && synchronizedStateOmitsThreads(currentState);
    if (retryIncomplete)
        attemptedIncompleteThreadReads.erase(id);
    const auto attempted = attemptedIncompleteThreadReads.find(id);
    const bool attemptedCurrentRecoveryEpoch =
        attempted != attemptedIncompleteThreadReads.end()
        && attempted->second == incompleteReadRecoveryEpoch;
    if ((!thread && !missingFromBoundedState) || (thread && thread->fullyLoaded)
        || threadReadsInFlight.contains(id)
        || attemptedCurrentRecoveryEpoch)
        return;
    if (threadReadsInFlight.size()
        >= static_cast<std::size_t>(
            detail::maximumCoalescedPresentationIdentities))
        return;

    threadReadsInFlight.insert(id);
    sdk::Submission submission = client->threads().read(
        {ai::openai::codex::typed::ThreadId{id}, true},
        [this, id](const sdk::OperationResult<sdk::ThreadReadResult>& result) {
            threadReadsInFlight.erase(id);
            reconcileIncompleteThreadReadAttempts();
            const auto* resolved = currentState.thread(id);
            const bool remainsIncomplete =
                (!resolved && synchronizedStateOmitsThreads(currentState))
                || (resolved && !resolved->fullyLoaded);
            // CapacityExceeded is a negotiated result, not proof that another
            // immediate full read can succeed. It consumes this replacement
            // epoch just like a successful read whose projection remains
            // incomplete; explicit user/reconnect recovery can still retry.
            if (!result) {
                if (remainsIncomplete)
                    rememberIncompleteThreadReadAttempt(id);
                return;
            }
            const bool authoritativelyAbsent = result.value
                && result.value->stateEffect
                && result.value->stateEffect->authority
                       == frontend::ThreadReadStateEffectAuthority::Absent;
            if (authoritativelyAbsent)
                return;
            if (remainsIncomplete)
                rememberIncompleteThreadReadAttempt(id);
        });
    if (!submission)
        threadReadsInFlight.erase(id);
}

void FrontendSessionWorker::handleStateUpdate(const sdk::StateUpdate& update)
{
    currentState = update.state;
    const bool stateReplaced = std::ranges::any_of(
        update.changes,
        [](const sdk::Change& change) {
            return std::holds_alternative<sdk::StateReplacedChange>(change);
        });
    if (stateReplaced)
        ++incompleteReadRecoveryEpoch;
    for (const auto& change : update.changes) {
        if (const auto* removed = std::get_if<sdk::ThreadRemovedChange>(&change))
            attemptedIncompleteThreadReads.erase(removed->threadId.value);
    }
    reconcileIncompleteThreadReadAttempts();
    const detail::StateUpdateScope scope = detail::stateUpdateScope(update);
    if (scope.hasPresentationChange)
        emit stateChanged(scope);
}

std::optional<QString> FrontendSessionWorker::acquireController(OperationCompletion completion)
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

std::optional<QString> FrontendSessionWorker::startThread(ThreadStartCompletion completion)
{
    return startThread(ai::openai::codex::typed::ThreadStartParams{}, std::move(completion));
}

std::optional<QString>
FrontendSessionWorker::startThread(ai::openai::codex::typed::ThreadStartParams parameters,
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

std::optional<QString> FrontendSessionWorker::resumeThread(const QString& threadId, ThreadStartCompletion completion)
{
    ai::openai::codex::typed::ThreadResumeParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    return resumeThread(std::move(parameters), std::move(completion));
}

std::optional<QString>
FrontendSessionWorker::resumeThread(ai::openai::codex::typed::ThreadResumeParams parameters,
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

std::optional<QString> FrontendSessionWorker::startTurn(const QString& threadId,
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
FrontendSessionWorker::startTurn(ai::openai::codex::typed::TurnStartParams parameters,
                           const QString& prompt,
                           TurnStartCompletion completion)
{
    return startTurn(std::move(parameters), prompt, {}, std::move(completion));
}

std::optional<QString>
FrontendSessionWorker::startTurn(ai::openai::codex::typed::TurnStartParams parameters,
                           const QString& prompt,
                           const QStringList& localImagePaths,
                           TurnStartCompletion completion)
{
    if (currentLifecycle != Lifecycle::Ready)
        return QStringLiteral("Backend is not ready");
    if (const auto error = promptValidationError(prompt))
        return error;

    if (parameters.threadId.value.empty())
        return QStringLiteral("Turn submission requires a thread ID");
    if (!prompt.trimmed().isEmpty()) {
        ai::openai::codex::typed::TextInput input;
        const QByteArray promptUtf8 = prompt.toUtf8();
        input.text.assign(promptUtf8.constData(), static_cast<std::size_t>(promptUtf8.size()));
        parameters.input.emplace_back(std::move(input));
    }
    for (const QString& path : localImagePaths) {
        ai::openai::codex::typed::LocalImageInput input;
        const QByteArray pathUtf8 = path.toUtf8();
        input.path.assign(pathUtf8.constData(), static_cast<std::size_t>(pathUtf8.size()));
        parameters.input.emplace_back(std::move(input));
    }
    if (parameters.input.empty())
        return QStringLiteral("Turn submission requires a prompt or attachment");
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

std::optional<QString> FrontendSessionWorker::steerTurn(const QString& threadId,
                                                  const QString& expectedTurnId,
                                                  const QString& prompt,
                                                  OperationCompletion completion)
{
    return steerTurn(threadId, expectedTurnId, prompt, {}, std::move(completion));
}

std::optional<QString> FrontendSessionWorker::steerTurn(const QString& threadId,
                                                  const QString& expectedTurnId,
                                                  const QString& prompt,
                                                  const QStringList& localImagePaths,
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
    if (!prompt.trimmed().isEmpty()) {
        ai::openai::codex::typed::TextInput input;
        const QByteArray promptUtf8 = prompt.toUtf8();
        input.text.assign(promptUtf8.constData(), static_cast<std::size_t>(promptUtf8.size()));
        parameters.input.emplace_back(std::move(input));
    }
    for (const QString& path : localImagePaths) {
        ai::openai::codex::typed::LocalImageInput input;
        const QByteArray pathUtf8 = path.toUtf8();
        input.path.assign(pathUtf8.constData(), static_cast<std::size_t>(pathUtf8.size()));
        parameters.input.emplace_back(std::move(input));
    }
    if (parameters.input.empty())
        return QStringLiteral("Steering requires a prompt or attachment");

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
FrontendSessionWorker::forkThread(ai::openai::codex::typed::ThreadForkParams parameters,
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

std::optional<QString> FrontendSessionWorker::renameThread(const QString& threadId,
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

std::optional<QString> FrontendSessionWorker::archiveThread(const QString& threadId,
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

std::optional<QString> FrontendSessionWorker::unarchiveThread(const QString& threadId,
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

std::optional<QString> FrontendSessionWorker::deleteThread(const QString& threadId,
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

std::optional<QString> FrontendSessionWorker::interruptTurn(const QString& threadId,
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

std::optional<QString> FrontendSessionWorker::respondApproval(const sdk::PendingRequestId& requestId,
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
FrontendSessionWorker::respondApplyPatchApproval(const sdk::PendingRequestId& requestId,
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
FrontendSessionWorker::respondExecCommandApproval(const sdk::PendingRequestId& requestId,
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

std::optional<QString> FrontendSessionWorker::respondUserInput(const sdk::PendingRequestId& requestId,
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

QString FrontendSessionWorker::defaultSocketPath()
{
    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDirectory.isEmpty())
        return QDir(runtimeDirectory).filePath(QStringLiteral("snodec-codex-backend.sock"));
    return QStringLiteral("/tmp/snodec-codex-backend-%1.sock").arg(::getuid());
}

void FrontendSessionWorker::socketConnected()
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

void FrontendSessionWorker::socketReadyRead()
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
        const qsizetype scanStart = qMax(consumedBytes, inboundScanOffset);
        const qsizetype newline = inboundBuffer.indexOf('\n', scanStart);
        if (newline < 0)
        {
            inboundScanOffset = inboundBuffer.size();
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
        inboundScanOffset = newline;

        const qsizetype payloadEnd = newline > consumedBytes && inboundBuffer.at(newline - 1) == '\r'
                                         ? newline - 1
                                         : newline;
        const qsizetype payloadBytes = payloadEnd - consumedBytes;
        if (static_cast<std::size_t>(payloadBytes) > maximumFrameBytes) {
            rejectOversizedFrame();
            return;
        }

        ++receivedFrames;
        if (payloadBytes == 0)
        {
            consumedBytes = newline + 1;
            inboundScanOffset = consumedBytes;
            continue;
        }
        const sdk::ReceiveResult result = connection.receive(
            std::string_view(inboundBuffer.constData() + consumedBytes,
                             static_cast<std::size_t>(payloadBytes)));
        consumedBytes = newline + 1;
        inboundScanOffset = consumedBytes;
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
    const qsizetype bufferedBytes = inboundBuffer.size() - inboundOffset;
    if (!hasCompleteInboundFrame() && bufferedBytes > 0
        && static_cast<std::size_t>(bufferedBytes - 1) > maximumFrameBytes) {
        rejectOversizedFrame();
        return;
    }
    if (connection.isOpen()
        && (socket.bytesAvailable() > 0 || hasCompleteInboundFrame()))
        scheduleSocketRead();
}

void FrontendSessionWorker::clearInbound() noexcept
{
    inboundBuffer.clear();
    inboundOffset = 0;
    inboundScanOffset = 0;
}

bool FrontendSessionWorker::hasCompleteInboundFrame() const noexcept
{
    const qsizetype scanStart = qMax(inboundOffset, inboundScanOffset);
    const qsizetype newline = inboundBuffer.indexOf('\n', scanStart);
    inboundScanOffset = newline >= 0 ? newline : inboundBuffer.size();
    return newline >= 0;
}

void FrontendSessionWorker::compactInbound() noexcept
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
    inboundScanOffset = qMax(qsizetype{0}, inboundScanOffset - inboundOffset);
    inboundOffset = 0;
}

void FrontendSessionWorker::rememberIncompleteThreadReadAttempt(
    const std::string& threadId)
{
    // The UI has one active selection. Retain the newest bounded recovery
    // identity if pathological rapid selection has left more unresolved
    // omitted IDs than presentation can track.
    if (!attemptedIncompleteThreadReads.contains(threadId)
        && attemptedIncompleteThreadReads.size()
               >= static_cast<std::size_t>(
                   detail::maximumCoalescedPresentationIdentities))
        attemptedIncompleteThreadReads.clear();
    attemptedIncompleteThreadReads.insert_or_assign(
        threadId, incompleteReadRecoveryEpoch);
}

void FrontendSessionWorker::scheduleSocketRead()
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

void FrontendSessionWorker::reconcileIncompleteThreadReadAttempts()
{
    const bool threadListComplete = currentState.threadList().value
                                    && currentState.threadList().value->complete;
    const bool boundedStateOmitsThreads = synchronizedStateOmitsThreads(currentState);
    const auto resolved = [&](const std::string& id) {
        const auto* thread = currentState.thread(id);
        return (thread && thread->fullyLoaded)
               || (!thread && threadListComplete && !boundedStateOmitsThreads);
    };
    std::erase_if(attemptedIncompleteThreadReads,
                  [&resolved](const auto& entry) {
                      return resolved(entry.first);
                  });
}

void FrontendSessionWorker::beginArchivedThreadRefresh()
{
    if (currentLifecycle != Lifecycle::Ready || archivedThreadListInFlight
        || archivedThreadDiscoveryTerminal())
        return;
    requestArchivedThreadPage(connectionGeneration, std::nullopt);
}

void FrontendSessionWorker::requestArchivedThreadPage(std::uint64_t generation,
                                                std::optional<std::string> cursor)
{
    if (generation != connectionGeneration || currentLifecycle != Lifecycle::Ready
        || archivedThreadDiscoveryTerminal() || archivedThreadListInFlight)
        return;
    const std::string cursorIdentity = cursor.value_or(std::string{});
    if (archivedThreadListCursors.size() >= maximumArchivedThreadListPages
        || !archivedThreadListCursors.insert(cursorIdentity).second) {
        finishArchivedThreadRefresh(
            ArchivedThreadDiscoveryStatus::CompleteWithTruncation,
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
                finishArchivedThreadRefresh(
                    ArchivedThreadDiscoveryStatus::Failed,
                    operationError(result.error,
                                   QStringLiteral("Archived threads could not be restored")));
                return;
            }
            if (result.value->nextCursor) {
                requestArchivedThreadPage(generation, result.value->nextCursor);
                return;
            }
            finishArchivedThreadRefresh(ArchivedThreadDiscoveryStatus::Complete);
        });
    if (const auto error = submissionError(submission,
                                           QStringLiteral("Archived thread listing could not be submitted"))) {
        archivedThreadListInFlight = false;
        finishArchivedThreadRefresh(ArchivedThreadDiscoveryStatus::Failed, *error);
    }
}

void FrontendSessionWorker::finishArchivedThreadRefresh(ArchivedThreadDiscoveryStatus status,
                                                  QString diagnostic)
{
    archivedThreadListInFlight = false;
    archivedThreadListStatus = status;
    if (!diagnostic.isEmpty())
        reportDiagnostic(std::move(diagnostic));

    detail::StateUpdateScope scope;
    scope.allThreadsAffected = true;
    scope.allInspectorsAffected = true;
    scope.allSidebarThreadsAffected = true;
    scope.sidebarAffected = true;
    scope.hasPresentationChange = true;
    emit stateChanged(scope);
}

void FrontendSessionWorker::beginModelCatalogRefresh()
{
    if (currentLifecycle != Lifecycle::Ready || modelListInFlight || modelListComplete)
        return;
    requestModelCatalogPage(connectionGeneration, std::nullopt);
}

void FrontendSessionWorker::requestModelCatalogPage(std::uint64_t generation,
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

void FrontendSessionWorker::finishModelCatalogRefresh(QString diagnostic)
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

void FrontendSessionWorker::socketDisconnected()
{
    if (connection.isOpen()) {
        if (localShutdown || !automaticReconnectEnabled)
            connection.transportDisconnected();
        else
            connection.transportDisconnected(sdk::TransportError{"Unix backend disconnected", true});
    }
    connection = Connection{};
    clearOutbound();
    clearInbound();
    receiveContinuationScheduled = false;
    threadReadsInFlight.clear();
    attemptedIncompleteThreadReads.clear();
    if (!localShutdown) {
        if (recordPreReadyTransportFailure())
            return;
        if (currentLifecycle != Lifecycle::Failed)
            setLifecycle(Lifecycle::Disconnected);
        scheduleReconnect();
    }
}

void FrontendSessionWorker::socketFailed(QLocalSocket::LocalSocketError)
{
    if (localShutdown)
        return;
    if (connection.isOpen()) {
        if (!automaticReconnectEnabled)
            connection.transportDisconnected();
        else
            connection.transportDisconnected(sdk::TransportError{socket.errorString().toStdString(), true});
    }
    connection = Connection{};
    clearOutbound();
    clearInbound();
    receiveContinuationScheduled = false;
    threadReadsInFlight.clear();
    attemptedIncompleteThreadReads.clear();
    if (recordPreReadyTransportFailure())
        return;
    if (automaticReconnectEnabled && currentLifecycle != Lifecycle::Failed)
        setLifecycle(Lifecycle::Failed, socket.errorString());
    scheduleReconnect();
}

void FrontendSessionWorker::handleConnectionStateChange(const sdk::ConnectionStateChange& change)
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

void FrontendSessionWorker::reportDiagnostic(QString message)
{
    if (message.isEmpty() || diagnosticDetail == message)
        return;
    diagnosticDetail = std::move(message);
    emit statusChanged();
}

void FrontendSessionWorker::failWithoutReconnect(QString reason)
{
    // Retrying the same stream after a protocol/state rejection only replays
    // the offending suffix. The explicit reconnect action remains available
    // after the underlying incompatibility is corrected.
    automaticReconnectEnabled = false;
    reconnectTimer.stop();
    clearOutbound();
    setLifecycle(Lifecycle::Failed, std::move(reason));
}

void FrontendSessionWorker::scheduleReconnect()
{
    if (localShutdown || !automaticReconnectEnabled || reconnectTimer.isActive())
        return;
    reconnectTimer.start(reconnectDelayMs);
    reconnectDelayMs = std::min(reconnectDelayMs * 2, maximumReconnectDelayMs);
}

void FrontendSessionWorker::retryConnection()
{
    if (localShutdown)
        return;
    if (socket.state() != QLocalSocket::UnconnectedState) {
        scheduleReconnect();
        return;
    }
    startConnection();
}

void FrontendSessionWorker::resetReconnectPolicy()
{
    automaticReconnectEnabled = true;
    reconnectTimer.stop();
    reconnectDelayMs = initialReconnectDelayMs;
    consecutivePreReadyDisconnects = 0;
    synchronizedCurrentConnection = false;
    preReadyFailureRecordedCurrentConnection = false;
}

bool FrontendSessionWorker::recordPreReadyTransportFailure()
{
    if (localShutdown || synchronizedCurrentConnection)
        return false;
    if (!automaticReconnectEnabled)
        return true;
    if (preReadyFailureRecordedCurrentConnection)
        return false;

    preReadyFailureRecordedCurrentConnection = true;
    ++consecutivePreReadyDisconnects;
    if (consecutivePreReadyDisconnects < maximumConsecutivePreReadyDisconnects)
        return false;

    failWithoutReconnect(
        QStringLiteral("Backend connection failed before synchronization completed on %1 consecutive connections")
            .arg(consecutivePreReadyDisconnects));
    return true;
}

FrontendSessionWorker::SendResult FrontendSessionWorker::send(OutboundMessage&& message)
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

FrontendSessionWorker::SendResult FrontendSessionWorker::sendToTransport(OutboundMessage&& message,
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

FrontendSessionWorker::SendResult FrontendSessionWorker::acceptOutbound(OutboundMessage&& message,
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

FrontendSessionWorker::DrainResult FrontendSessionWorker::drainOutbound(const OutboundWriter& writer) noexcept
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

void FrontendSessionWorker::socketBytesWritten(qint64)
{
    drainSocketWrites();
}

void FrontendSessionWorker::drainSocketWrites()
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

void FrontendSessionWorker::scheduleOutboundDrain()
{
    if (!pendingWrites.empty()
        && !outboundDrainTimer.isActive())
        outboundDrainTimer.start(outboundDrainRetryMs);
}

void FrontendSessionWorker::clearOutbound() noexcept
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

void FrontendSessionWorker::closeTransport(QString reason) noexcept
{
    clearOutbound();
    if (!reason.isEmpty())
        detail = std::move(reason);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.abort();
}

void FrontendSessionWorker::setLifecycle(Lifecycle value, QString newDetail)
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
