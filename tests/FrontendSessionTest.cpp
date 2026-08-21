// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <ai/openai/codex/frontend/Codec.h>
#include <ai/openai/codex/frontend/Messages.h>

#include <QCoreApplication>

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace codexui {

struct FrontendSessionTestAccess
{
    static void setLifecycle(FrontendSession& session, FrontendSession::Lifecycle lifecycle, QString detail = {})
    {
        session.setLifecycle(lifecycle, std::move(detail));
    }

    static void handleConnectionStateChange(
        FrontendSession& session,
        const ai::openai::codex::frontend::client::ConnectionStateChange& change)
    {
        session.handleConnectionStateChange(change);
    }

    static void reportDiagnostic(FrontendSession& session, QString message)
    {
        session.reportDiagnostic(std::move(message));
    }

    static bool automaticReconnectEnabled(const FrontendSession& session)
    {
        return session.automaticReconnectEnabled;
    }

    static int consecutivePreReadyDisconnects(const FrontendSession& session)
    {
        return session.consecutivePreReadyDisconnects;
    }

    static int maximumConsecutivePreReadyDisconnects()
    {
        return FrontendSession::maximumConsecutivePreReadyDisconnects;
    }

    static std::size_t maximumFrameBytes(const FrontendSession& session)
    {
        return session.maximumFrameBytes;
    }

    static void resetReconnectPolicy(FrontendSession& session)
    {
        session.resetReconnectPolicy();
    }

    static void setInbound(FrontendSession& session, QByteArray bytes, qsizetype offset)
    {
        session.inboundBuffer = std::move(bytes);
        session.inboundOffset = offset;
    }

    static void compactInbound(FrontendSession& session)
    {
        session.compactInbound();
    }

    static QByteArray inboundBytes(const FrontendSession& session)
    {
        return session.inboundBuffer;
    }

    static qsizetype inboundOffset(const FrontendSession& session)
    {
        return session.inboundOffset;
    }

    static bool hasCompleteInboundFrame(const FrontendSession& session)
    {
        return session.hasCompleteInboundFrame();
    }

    static void prepareReconnectReset(FrontendSession& session)
    {
        session.automaticReconnectEnabled = false;
        session.reconnectDelayMs = FrontendSession::maximumReconnectDelayMs;
        session.reconnectTimer.start(60'000);
    }

    static void installConnectionWithTerminalClose(FrontendSession& session, bool& closeObserved)
    {
        session.connection = session.client->openConnection({
            [](FrontendSession::OutboundMessage) {
                return FrontendSession::SendResult{
                    ai::openai::codex::frontend::client::SendStatus::Accepted,
                    std::nullopt};
            },
            [&session, &closeObserved](std::string) {
                closeObserved = true;
                ai::openai::codex::frontend::client::Error terminalError;
                terminalError.message = "terminal close callback";
                terminalError.retryable = false;
                session.handleConnectionStateChange(
                    {ai::openai::codex::frontend::client::ConnectionState::Connecting,
                     ai::openai::codex::frontend::client::ConnectionState::Closed,
                     terminalError});
            },
        });
    }

    static ai::openai::codex::frontend::client::SendResult
    acceptOutbound(FrontendSession& session,
                   std::string compactJson,
                   qint64 socketBufferedBytes,
                   const std::function<qint64(const char*, qint64)>& writer)
    {
        const std::size_t serializedBytes = compactJson.size();
        return session.acceptOutbound(
            {ai::openai::codex::frontend::client::OutboundKind::Command,
             std::move(compactJson),
             serializedBytes,
             false},
            socketBufferedBytes,
            writer);
    }

    static ai::openai::codex::frontend::client::SendResult
    sendToTransport(FrontendSession& session,
                    std::string compactJson,
                    bool transportConnected,
                    qint64 socketBufferedBytes,
                    const std::function<qint64(const char*, qint64)>& writer)
    {
        const std::size_t serializedBytes = compactJson.size();
        ai::openai::codex::frontend::client::OutboundMessage message{
            ai::openai::codex::frontend::client::OutboundKind::Command,
            std::move(compactJson),
            serializedBytes,
            false};
        return session.sendToTransport(
            std::move(message), transportConnected, socketBufferedBytes, writer);
    }

    static bool drainOutbound(FrontendSession& session,
                              const std::function<qint64(const char*, qint64)>& writer)
    {
        const FrontendSession::DrainResult result = session.drainOutbound(writer);
        return result != FrontendSession::DrainResult::Failed
               && result != FrontendSession::DrainResult::Reset;
    }

    static std::string pendingWire(const FrontendSession& session)
    {
        std::string wire;
        for (const FrontendSession::PendingWrite& pending : session.pendingWrites) {
            const qint64 frameSize = static_cast<qint64>(pending.frame.size());
            if (pending.offset < 0 || pending.offset > frameSize)
                return "<invalid pending-write offset>";
            const std::size_t offset = static_cast<std::size_t>(pending.offset);
            wire.append(pending.frame.data() + offset, pending.frame.size() - offset);
        }
        return wire;
    }

    static qint64 pendingWriteBytes(const FrontendSession& session)
    {
        return session.pendingWriteBytes;
    }

    static qint64 maximumBufferedOutboundBytes()
    {
        return FrontendSession::maximumBufferedOutboundBytes;
    }

    static void clearOutbound(FrontendSession& session)
    {
        session.clearOutbound();
    }

    static bool outboundDrainIsScheduled(const FrontendSession& session)
    {
        return session.outboundDrainTimer.isActive();
    }

    static ai::openai::codex::frontend::client::SendResult
    send(FrontendSession& session, ai::openai::codex::frontend::client::OutboundMessage& message)
    {
        return session.send(std::move(message));
    }

    static bool outboundClearIsDeferred(const FrontendSession& session)
    {
        return session.outboundClearPending && session.pendingWriteBytes > 0;
    }

    static void disconnectTransport(FrontendSession& session)
    {
        session.preReadyFailureRecordedCurrentConnection = false;
        session.socketDisconnected();
        session.reconnectTimer.stop();
    }

    static void failTransport(FrontendSession& session, bool newConnectionAttempt = true)
    {
        if (newConnectionAttempt)
            session.preReadyFailureRecordedCurrentConnection = false;
        session.socketFailed(QLocalSocket::ConnectionRefusedError);
        session.reconnectTimer.stop();
    }

    static bool synchronizeWithCapturedTransport(
        FrontendSession& session,
        std::vector<ai::openai::codex::frontend::client::OutboundMessage>& messages)
    {
        namespace frontend = ai::openai::codex::frontend;
        namespace sdk = frontend::client;

        session.connection = session.client->openConnection({
            [&messages](FrontendSession::OutboundMessage message) {
                messages.push_back(std::move(message));
                return FrontendSession::SendResult{sdk::SendStatus::Accepted, std::nullopt};
            },
            [](std::string) {},
        });
        session.connection.transportConnected();

        const frontend::Json state{
            {"backendRevision", std::uint64_t{1}},
            {"lifecycle", "ready"},
            {"diagnostics",
             {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
            {"sessions", frontend::Json::array()},
            {"threadList",
             {{"hasLoadedPage", true},
              {"complete", true},
              {"pagesLoaded", std::uint64_t{1}}}},
            {"threads", frontend::Json::array()},
            {"pendingRequests", frontend::Json::array()},
            {"codexExtensions", frontend::Json::array()},
            {"omittedCodexExtensions", std::uint64_t{0}},
            {"journal",
             {{"oldestReplayableAfter", std::uint64_t{0}},
              {"currentSequence", std::uint64_t{0}}}},
            {"sequenceExhausted", false},
        };
        return session.connection
                   .receive(frontend::ServerMessage{frontend::Welcome{
                       "archived-refresh-test",
                       frontend::SessionRole::Observer,
                       frontend::SequenceNumber{0},
                       frontend::SyncMode::Snapshot}})
                   .accepted
            && session.connection
                   .receive(frontend::ServerMessage{
                       frontend::Snapshot{frontend::SequenceNumber{0}, state}})
                   .accepted
            && session.connection
                   .receive(frontend::ServerMessage{
                       frontend::SyncComplete{frontend::SequenceNumber{0}}})
                   .accepted;
    }

    static bool receive(FrontendSession& session,
                        ai::openai::codex::frontend::ServerMessage message)
    {
        return session.connection.receive(std::move(message)).accepted;
    }

    static void receiveWire(FrontendSession& session, QByteArray wire)
    {
        session.inboundBuffer = std::move(wire);
        session.inboundOffset = 0;
        session.socketReadyRead();
    }

    static void beginArchivedThreadRefresh(FrontendSession& session)
    {
        session.beginArchivedThreadRefresh();
    }

    static bool archivedThreadListInFlight(const FrontendSession& session)
    {
        return session.archivedThreadListInFlight;
    }

    static std::size_t archivedThreadCursorCount(const FrontendSession& session)
    {
        return session.archivedThreadListCursors.size();
    }
};

} // namespace codexui

namespace {

namespace frontend = ai::openai::codex::frontend;
namespace sdk = ai::openai::codex::frontend::client;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

bool testPeerCredentials()
{
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        std::cerr << "could not create the Unix peer-credential fixture\n";
        return false;
    }

    const uid_t currentUser = ::geteuid();
    const uid_t differentUser = currentUser == std::numeric_limits<uid_t>::max() ? currentUser - 1 : currentUser + 1;
    bool passed = true;
    passed &= expect(!codexui::detail::unixPeerCredentialError(sockets[0], currentUser),
                     "the connected process UID must be accepted");
    passed &= expect(codexui::detail::unixPeerCredentialError(sockets[0], differentUser).has_value(),
                     "a different process UID must be rejected");
    passed &= expect(codexui::detail::unixPeerCredentialError(-1, currentUser).has_value(),
                     "an invalid socket descriptor must fail closed");

    ::close(sockets[0]);
    ::close(sockets[1]);
    return passed;
}

bool testScopedItemPresentationChanges()
{
    sdk::StateUpdate turnUpdate;
    turnUpdate.changes.push_back(sdk::TurnUpsertedChange{
        ai::openai::codex::typed::TurnId{"ambiguous-or-missing-turn"}});
    const auto unresolvedTurn = codexui::detail::stateUpdateScope(turnUpdate);

    sdk::StateUpdate scopedUpdate;
    scopedUpdate.changes.push_back(sdk::ItemUpsertedChange{
        ai::openai::codex::typed::ItemId{"duplicate-item"},
        ai::openai::codex::typed::ThreadId{"target-thread"},
        ai::openai::codex::typed::TurnId{"target-turn"}});
    const auto scoped = codexui::detail::stateUpdateScope(scopedUpdate);

    sdk::StateUpdate streamedUpdate;
    streamedUpdate.changes.push_back(
        sdk::ItemContentReplacedChange{ai::openai::codex::typed::ItemId{"streamed-item"},
                                       sdk::ItemContentChannel::AgentText,
                                       ai::openai::codex::typed::ThreadId{"target-thread"},
                                       ai::openai::codex::typed::TurnId{"target-turn"}});
    streamedUpdate.changes.push_back(
        sdk::ItemContentReplacedChange{ai::openai::codex::typed::ItemId{"streamed-item"},
                                       sdk::ItemContentChannel::AgentText,
                                       ai::openai::codex::typed::ThreadId{"target-thread"},
                                       ai::openai::codex::typed::TurnId{"target-turn"}});
    streamedUpdate.changes.push_back(
        sdk::CursorAdvancedChange{ai::openai::codex::frontend::SequenceNumber{42}});
    const auto streamed = codexui::detail::stateUpdateScope(streamedUpdate);

    sdk::StateUpdate partiallyScopedUpdate;
    partiallyScopedUpdate.changes.push_back(
        sdk::ItemContentReplacedChange{ai::openai::codex::typed::ItemId{"partial-item"},
                                       sdk::ItemContentChannel::AgentText,
                                       ai::openai::codex::typed::ThreadId{"target-thread"},
                                       std::nullopt});
    const auto partiallyScoped = codexui::detail::stateUpdateScope(partiallyScopedUpdate);

    sdk::StateUpdate mixedUpdate = streamedUpdate;
    mixedUpdate.changes.push_back(sdk::ItemUpsertedChange{
        ai::openai::codex::typed::ItemId{"structural-item"},
        ai::openai::codex::typed::ThreadId{"target-thread"},
        ai::openai::codex::typed::TurnId{"target-turn"}});
    const auto mixed = codexui::detail::stateUpdateScope(mixedUpdate);

    sdk::StateUpdate unscopedUpdate;
    unscopedUpdate.changes.push_back(
        sdk::ItemContentReplacedChange{ai::openai::codex::typed::ItemId{"duplicate-item"},
                                       sdk::ItemContentChannel::AgentText,
                                       std::nullopt,
                                       std::nullopt});
    const auto unscoped = codexui::detail::stateUpdateScope(unscopedUpdate);

    sdk::StateUpdate replacementUpdate;
    replacementUpdate.changes.push_back(sdk::StateReplacedChange{});
    const auto replacement = codexui::detail::stateUpdateScope(replacementUpdate);

    sdk::StateUpdate cursorUpdate;
    cursorUpdate.changes.push_back(
        sdk::CursorAdvancedChange{ai::openai::codex::frontend::SequenceNumber{43}});
    const auto cursor = codexui::detail::stateUpdateScope(cursorUpdate);

    bool passed = expect(unresolvedTurn.affectedThreadIds.empty()
                             && unresolvedTurn.fullyAffectedThreadIds.empty()
                             && unresolvedTurn.affectedInspectorThreadIds.empty()
                             && unresolvedTurn.allThreadsAffected
                             && unresolvedTurn.allInspectorsAffected
                             && !unresolvedTurn.sidebarAffected
                             && unresolvedTurn.hasPresentationChange,
                         "a turn upsert without a unique parent lookup must conservatively refresh all threads");
    passed &= expect(scoped.affectedThreadIds == QStringList{QStringLiteral("target-thread")}
                             && scoped.fullyAffectedThreadIds
                                    == QStringList{QStringLiteral("target-thread")}
                             && scoped.affectedInspectorThreadIds
                                    == QStringList{QStringLiteral("target-thread")}
                             && !scoped.allThreadsAffected && !scoped.allInspectorsAffected
                             && !scoped.sidebarAffected && scoped.hasPresentationChange,
                         "a scoped item upsert must refresh its canonical conversation and Inspector");
    passed &= expect(streamed.affectedThreadIds == QStringList{QStringLiteral("target-thread")}
                         && streamed.fullyAffectedThreadIds.empty()
                         && streamed.affectedInspectorThreadIds.empty()
                         && streamed.affectedItemContents
                                == std::vector<codexui::detail::StateUpdateScope::ItemContentIdentity>{
                                    {QStringLiteral("target-thread"),
                                     QStringLiteral("target-turn"),
                                     QStringLiteral("streamed-item")}}
                         && !streamed.allThreadsAffected && !streamed.allInspectorsAffected
                         && !streamed.sidebarAffected && streamed.hasPresentationChange,
                     "streamed item content must carry its exact composite identity");
    passed &= expect(partiallyScoped.affectedThreadIds
                             == QStringList{QStringLiteral("target-thread")}
                         && partiallyScoped.fullyAffectedThreadIds
                                == QStringList{QStringLiteral("target-thread")}
                         && partiallyScoped.affectedItemContents.empty()
                         && !partiallyScoped.allThreadsAffected,
                     "partially scoped item content must require bounded full thread reconciliation");
    passed &= expect(mixed.fullyAffectedThreadIds
                             == QStringList{QStringLiteral("target-thread")}
                         && mixed.affectedItemContents.size() == 1
                         && !mixed.allThreadsAffected,
                     "a structural change mixed with exact content must require full thread reconciliation");
    passed &= expect(unscoped.affectedThreadIds.empty() && unscoped.allThreadsAffected
                         && unscoped.fullyAffectedThreadIds.empty()
                         && unscoped.affectedItemContents.empty()
                         && unscoped.affectedInspectorThreadIds.empty()
                         && unscoped.allInspectorsAffected && !unscoped.sidebarAffected
                         && unscoped.hasPresentationChange,
                     "an unscoped item change must conservatively refresh all thread-bound presentations");
    passed &= expect(replacement.allThreadsAffected && replacement.allInspectorsAffected
                         && replacement.sidebarAffected && replacement.hasPresentationChange,
                     "a State replacement must conservatively refresh every presentation");
    passed &= expect(!cursor.allThreadsAffected && !cursor.allInspectorsAffected
                         && !cursor.sidebarAffected && cursor.hasPresentationChange,
                     "a cursor-only update must dispatch its revision without dirtying broad presentation");
    return passed;
}

bool testLifecycleAndDiagnostics()
{
    int lifecycleChanges = 0;
    int statusChanges = 0;
    bool reconnectCloseObserved = false;
    codexui::FrontendSession session;
    QObject::connect(&session, &codexui::FrontendSession::lifecycleChanged, [&lifecycleChanges] { ++lifecycleChanges; });
    QObject::connect(&session, &codexui::FrontendSession::statusChanged, [&statusChanges] { ++statusChanges; });

    codexui::FrontendSessionTestAccess::setLifecycle(session, codexui::FrontendSession::Lifecycle::Ready);
    codexui::FrontendSessionTestAccess::setLifecycle(session, codexui::FrontendSession::Lifecycle::Ready);
    bool passed = expect(lifecycleChanges == 1, "an identical lifecycle and detail must not emit a duplicate transition");

    codexui::FrontendSessionTestAccess::reportDiagnostic(session, QStringLiteral("projection diagnostic"));
    codexui::FrontendSessionTestAccess::reportDiagnostic(session, QStringLiteral("projection diagnostic"));
    codexui::FrontendSessionTestAccess::setLifecycle(session, codexui::FrontendSession::Lifecycle::Ready);
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Ready
                         && session.statusText() == QStringLiteral("projection diagnostic")
                         && lifecycleChanges == 1 && statusChanges == 1,
                     "an error diagnostic must update status once without changing a ready lifecycle");

    sdk::Error retryableError;
    retryableError.message = "temporary backend failure";
    retryableError.retryable = true;
    const sdk::ConnectionStateChange retryableChange{
        sdk::ConnectionState::Ready, sdk::ConnectionState::Disconnected, retryableError};
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(session, retryableChange);
    const int retryableSignalCount = lifecycleChanges;
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(session, retryableChange);
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && lifecycleChanges == retryableSignalCount && retryableSignalCount == 2,
                     "a retryable connection error must produce one failed transition and retain automatic reconnect");

    sdk::Error terminalError;
    terminalError.message = "terminal protocol failure";
    terminalError.retryable = false;
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(
        session,
        {sdk::ConnectionState::Disconnected, sdk::ConnectionState::Closed, terminalError});
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && !codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && lifecycleChanges == 3,
                     "a nonretryable connection error must produce one failed transition and disable automatic reconnect");
    codexui::FrontendSessionTestAccess::handleConnectionStateChange(
        session,
        {sdk::ConnectionState::Closed, sdk::ConnectionState::Disconnected, std::nullopt});
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && lifecycleChanges == 3,
                     "a following physical close must preserve the terminal failure");

    codexui::FrontendSessionTestAccess::prepareReconnectReset(session);
    codexui::FrontendSessionTestAccess::installConnectionWithTerminalClose(session, reconnectCloseObserved);
    session.reconnectToBackend();
    passed &= expect(reconnectCloseObserved
                         && codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session),
                     "the public reconnect path must override a terminal old-transport close callback");
    return passed;
}

bool testPreReadyReconnectBound()
{
    codexui::FrontendSession session;
    const int maximum = codexui::FrontendSessionTestAccess::maximumConsecutivePreReadyDisconnects();
    bool passed = true;
    for (int attempt = 1; attempt < maximum; ++attempt) {
        codexui::FrontendSessionTestAccess::disconnectTransport(session);
        passed &= expect(codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                             && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(session) == attempt,
                         "a bounded number of pre-synchronization disconnects remains retryable");
    }

    codexui::FrontendSessionTestAccess::disconnectTransport(session);
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && !codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(session) == maximum
                         && session.statusText().contains(QStringLiteral("before synchronization completed")),
                     "repeated pre-synchronization disconnects stop at a visible terminal boundary");

    codexui::FrontendSessionTestAccess::resetReconnectPolicy(session);
    std::vector<sdk::OutboundMessage> messages;
    passed &= expect(
        codexui::FrontendSessionTestAccess::synchronizeWithCapturedTransport(session, messages)
            && session.lifecycle() == codexui::FrontendSession::Lifecycle::Ready
            && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(session) == 0,
        "the real SDK synchronization callback must reset the pre-ready retry budget");
    codexui::FrontendSessionTestAccess::disconnectTransport(session);
    passed &= expect(codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(session) == 0,
                     "a disconnect after synchronization does not consume the pre-ready retry budget");

    codexui::FrontendSession failedConnectSession;
    for (int attempt = 1; attempt < maximum; ++attempt) {
        codexui::FrontendSessionTestAccess::failTransport(failedConnectSession);
        passed &= expect(
            codexui::FrontendSessionTestAccess::automaticReconnectEnabled(failedConnectSession)
                && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(failedConnectSession) == attempt,
            "a bounded number of failed pre-synchronization connection attempts remains retryable");
        codexui::FrontendSessionTestAccess::failTransport(failedConnectSession, false);
        passed &= expect(
            codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(failedConnectSession) == attempt,
            "multiple failure signals for one connection attempt consume the retry budget only once");
    }
    codexui::FrontendSessionTestAccess::failTransport(failedConnectSession);
    passed &= expect(
        failedConnectSession.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
            && !codexui::FrontendSessionTestAccess::automaticReconnectEnabled(failedConnectSession)
            && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(failedConnectSession) == maximum,
        "repeated failed connection attempts stop at the same visible terminal boundary");
    return passed;
}

bool testReceiveRejectionPreservesPreciseError()
{
    codexui::FrontendSession session;
    std::vector<sdk::OutboundMessage> messages;
    bool passed = expect(
        codexui::FrontendSessionTestAccess::synchronizeWithCapturedTransport(session, messages),
        "the receive-rejection fixture must reach synchronized State");

    const auto duplicateWelcome = frontend::Codec::serializeServer(
        frontend::ServerMessage{frontend::Welcome{
            "duplicate-session",
            frontend::SessionRole::Observer,
            frontend::SequenceNumber{0},
            frontend::SyncMode::Snapshot}});
    passed &= expect(duplicateWelcome.hasValue(),
                     "the duplicate-Welcome rejection fixture must encode");
    if (!duplicateWelcome)
        return false;

    codexui::FrontendSessionTestAccess::receiveWire(
        session, QByteArray::fromStdString(duplicateWelcome.value() + '\n'));
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && !codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && session.statusText() == QStringLiteral("unexpected or duplicate Welcome")
                         && !session.statusText().contains(
                             QStringLiteral("frontend server message was rejected"),
                             Qt::CaseInsensitive),
                     "socketReadyRead must preserve the precise SDK lifecycle error instead of the generic receive rejection");
    codexui::FrontendSessionTestAccess::failTransport(session, false);
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && !codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && session.statusText() == QStringLiteral("unexpected or duplicate Welcome")
                         && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(session) == 0,
                     "the socket error following a terminal SDK rejection must preserve its precise reason and retry budget");
    codexui::FrontendSessionTestAccess::disconnectTransport(session);
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Failed
                         && !codexui::FrontendSessionTestAccess::automaticReconnectEnabled(session)
                         && session.statusText() == QStringLiteral("unexpected or duplicate Welcome")
                         && codexui::FrontendSessionTestAccess::consecutivePreReadyDisconnects(session) == 0,
                     "the physical disconnect following a terminal SDK rejection must preserve its precise reason and retry budget");
    return passed;
}

bool testInboundFrameCapacityTracksSdk()
{
    codexui::FrontendSession session;
    const sdk::ClientOptions defaults;
    return expect(
        codexui::FrontendSessionTestAccess::maximumFrameBytes(session) == defaults.maximumInboundMessageBytes
            && codexui::FrontendSessionTestAccess::maximumFrameBytes(session) > 16U * 1024U * 1024U,
        "the Qt JSONL receiver must accept the SDK's complete provider-derived server-message range");
}

std::vector<frontend::Json>
capturedCommands(const std::vector<sdk::OutboundMessage>& messages,
                 std::string_view method = {})
{
    std::vector<frontend::Json> result;
    for (const sdk::OutboundMessage& message : messages) {
        if (message.kind != sdk::OutboundKind::Command)
            continue;
        frontend::Json wire = frontend::Json::parse(message.compactJson, nullptr, false);
        if (!wire.is_discarded()
            && (method.empty() || wire.value("method", std::string{}) == method))
            result.push_back(std::move(wire));
    }
    return result;
}

frontend::Json modelListEntry(std::string id,
                              std::string model,
                              std::string displayName,
                              bool hidden = false)
{
    return frontend::Json{
        {"defaultReasoningEffort", "medium"},
        {"description", "FrontendSession model catalogue fixture"},
        {"displayName", std::move(displayName)},
        {"hidden", hidden},
        {"id", std::move(id)},
        {"isDefault", false},
        {"model", std::move(model)},
        {"supportedReasoningEfforts",
         frontend::Json::array(
             {{{"description", "Balanced"}, {"reasoningEffort", "medium"}}})},
    };
}

bool testModelCatalogRefresh()
{
    codexui::FrontendSession session;
    std::vector<sdk::OutboundMessage> outbound;
    int catalogueSignals = 0;
    QObject::connect(&session, &codexui::FrontendSession::modelCatalogChanged,
                     [&catalogueSignals] { ++catalogueSignals; });

    bool passed = expect(
        codexui::FrontendSessionTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the model-catalogue fixture must reach a synchronized SDK connection");
    std::vector<frontend::Json> commands = capturedCommands(outbound, "model.list");
    passed &= expect(commands.size() == 1
                         && commands.front().value("params", frontend::Json::object())
                                == frontend::Json{{"includeHidden", false}, {"limit", 100}},
                     "synchronization must request the first bounded visible-model page");
    if (commands.size() != 1 || !commands.front().contains("requestId"))
        return false;

    const std::string firstRequestId = commands.front()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                firstRequestId,
                frontend::Json{{"data",
                                frontend::Json::array({modelListEntry(
                                    "preset-alpha", "model-alpha", "Alpha")})},
                               {"nextCursor", "model-page-2"}})}),
        "the first model catalogue page must be accepted");
    commands = capturedCommands(outbound, "model.list");
    passed &= expect(commands.size() == 2 && session.modelCatalog().empty()
                         && catalogueSignals == 0
                         && commands.back().value("params", frontend::Json::object())
                                == frontend::Json{{"cursor", "model-page-2"},
                                                  {"includeHidden", false},
                                                  {"limit", 100}},
                     "a continuation page must not publish a partial catalogue");
    if (commands.size() != 2 || !commands.back().contains("requestId"))
        return false;

    const std::string secondRequestId = commands.back()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                secondRequestId,
                frontend::Json{{"data",
                                frontend::Json::array(
                                    {modelListEntry("preset-hidden", "model-hidden", "Hidden", true),
                                     modelListEntry("preset-alpha-duplicate", "model-alpha", "Duplicate"),
                                     modelListEntry("preset-beta", "model-beta", "Beta")})}})}),
        "the terminal model catalogue page must be accepted");
    const auto& catalogue = session.modelCatalog();
    passed &= expect(catalogue.size() == 2 && catalogue[0].model.value == "model-alpha"
                         && catalogue[0].displayName == "Alpha"
                         && catalogue[1].model.value == "model-beta"
                         && catalogue[1].displayName == "Beta" && catalogueSignals == 0,
                     "terminal publication must retain first-seen visible slugs in exact page order");
    QCoreApplication::processEvents();
    passed &= expect(catalogueSignals == 1,
                     "terminal model catalogue publication must emit exactly one queued change signal");
    return passed;
}

bool testModelCatalogRefreshFailureIsDiagnosed()
{
    codexui::FrontendSession session;
    std::vector<sdk::OutboundMessage> outbound;
    int catalogueSignals = 0;
    QObject::connect(&session, &codexui::FrontendSession::modelCatalogChanged,
                     [&catalogueSignals] { ++catalogueSignals; });

    bool passed = expect(
        codexui::FrontendSessionTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the model-catalogue failure fixture must reach a synchronized SDK connection");
    const std::vector<frontend::Json> commands = capturedCommands(outbound, "model.list");
    if (!expect(commands.size() == 1 && commands.front().contains("requestId"),
                "the failure fixture must capture one model-list request"))
        return false;

    const std::string requestId = commands.front()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::failure(
                requestId,
                frontend::CommandError{frontend::ErrorCode::InternalError,
                                       "model listing failed"})}),
        "the model-catalogue failure response must be accepted");
    QCoreApplication::processEvents();
    passed &= expect(session.modelCatalog().empty() && catalogueSignals == 0,
                     "a failed model listing must not publish a partial or synthetic catalogue");
    passed &= expect(session.statusText().contains(QStringLiteral("model listing failed")),
                     "a failed model listing must surface its diagnostic");
    return passed;
}

bool testArchivedThreadRefresh()
{
    codexui::FrontendSession session;
    std::vector<sdk::OutboundMessage> outbound;
    int discoverySignals = 0;
    std::optional<codexui::detail::StateUpdateScope> discoveryScope;
    QObject::connect(
        &session,
        &codexui::FrontendSession::stateChanged,
        [&session, &discoverySignals, &discoveryScope](
            const codexui::detail::StateUpdateScope& scope) {
            if (!session.archivedThreadDiscoveryComplete())
                return;
            ++discoverySignals;
            discoveryScope = scope;
        });

    bool passed = expect(
        codexui::FrontendSessionTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the archived-thread fixture must reach a synchronized SDK connection");
    std::vector<frontend::Json> commands = capturedCommands(outbound, "thread.list");
    passed &= expect(session.lifecycle() == codexui::FrontendSession::Lifecycle::Ready
                         && !session.archivedThreadDiscoveryComplete()
                         && codexui::FrontendSessionTestAccess::archivedThreadListInFlight(session)
                         && commands.size() == 1,
                     "synchronization must start exactly one incomplete archived-thread discovery request");
    if (commands.size() != 1)
        return false;

    const frontend::Json& first = commands.front();
    passed &= expect(first.value("method", std::string{}) == "thread.list"
                         && first.contains("requestId") && first["requestId"].is_string()
                         && first.value("params", frontend::Json::object())
                                == frontend::Json{{"archived", true}, {"limit", 100}},
                     "the first discovery page must request archived threads with the bounded page size and no cursor");
    if (!first.contains("requestId") || !first["requestId"].is_string())
        return false;

    const std::string firstRequestId = first["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                firstRequestId,
                frontend::Json{{"threads", frontend::Json::array()},
                               {"nextCursor", "archived-page-2"}})}),
        "the first archived-thread page response must be accepted");
    commands = capturedCommands(outbound, "thread.list");
    passed &= expect(!session.archivedThreadDiscoveryComplete()
                         && codexui::FrontendSessionTestAccess::archivedThreadListInFlight(session)
                         && discoverySignals == 0 && commands.size() == 2,
                     "a continuation cursor must keep discovery incomplete and submit exactly one next page");
    if (commands.size() != 2)
        return false;

    const frontend::Json& second = commands.back();
    passed &= expect(second.value("method", std::string{}) == "thread.list"
                         && second.contains("requestId") && second["requestId"].is_string()
                         && second.value("params", frontend::Json::object())
                                == frontend::Json{{"archived", true},
                                                  {"cursor", "archived-page-2"},
                                                  {"limit", 100}},
                     "the second discovery page must preserve the archived filter and exact opaque cursor");
    if (!second.contains("requestId") || !second["requestId"].is_string())
        return false;

    const std::string secondRequestId = second["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                secondRequestId,
                frontend::Json{{"threads", frontend::Json::array()}})}),
        "the terminal archived-thread page response must be accepted");
    passed &= expect(session.archivedThreadDiscoveryComplete()
                         && !codexui::FrontendSessionTestAccess::archivedThreadListInFlight(session)
                         && codexui::FrontendSessionTestAccess::archivedThreadCursorCount(session) == 2
                         && discoverySignals == 1 && discoveryScope
                         && discoveryScope->allThreadsAffected
                         && discoveryScope->allInspectorsAffected
                         && discoveryScope->sidebarAffected
                         && discoveryScope->hasPresentationChange,
                     "the terminal page must publish completion and one conservative presentation refresh");

    const std::size_t outboundAtCompletion = outbound.size();
    codexui::FrontendSessionTestAccess::beginArchivedThreadRefresh(session);
    passed &= expect(outbound.size() == outboundAtCompletion && discoverySignals == 1,
                     "completed archived-thread discovery must not restart or emit duplicate completion refreshes");
    return passed;
}

bool testArchivedThreadRefreshFailureRemainsIncomplete()
{
    codexui::FrontendSession session;
    std::vector<sdk::OutboundMessage> outbound;

    bool passed = expect(
        codexui::FrontendSessionTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the archived-thread failure fixture must reach a synchronized SDK connection");
    const std::vector<frontend::Json> commands = capturedCommands(outbound, "thread.list");
    if (!expect(commands.size() == 1 && commands.front().contains("requestId"),
                "the failure fixture must capture one archived-thread request"))
        return false;

    int stateSignals = 0;
    QObject::connect(&session, &codexui::FrontendSession::stateChanged,
                     [&stateSignals](const codexui::detail::StateUpdateScope&) {
                         ++stateSignals;
                     });
    const std::string requestId = commands.front()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::failure(
                requestId,
                frontend::CommandError{frontend::ErrorCode::InternalError,
                                       "archived listing failed"})}),
        "the archived-thread failure response must be accepted");
    passed &= expect(!session.archivedThreadDiscoveryComplete()
                         && !codexui::FrontendSessionTestAccess::archivedThreadListInFlight(session),
                     "a failed archived-thread request must stop in-flight work without claiming discovery completed");
    passed &= expect(stateSignals == 0,
                     "a failed archived-thread request must not emit the completion refresh that can clear selection");
    passed &= expect(session.statusText().contains(QStringLiteral("archived listing failed")),
                     "a failed archived-thread request must still surface its diagnostic");
    return passed;
}

bool testTurnSteeringSubmission()
{
    codexui::FrontendSession session;
    std::vector<sdk::OutboundMessage> outbound;
    bool passed = expect(
        codexui::FrontendSessionTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the turn-steering fixture must reach a synchronized SDK connection");

    QString completionError = QStringLiteral("completion not called");
    const auto immediateError = session.steerTurn(
        QStringLiteral("thread-active"),
        QStringLiteral("turn-active"),
        QStringLiteral("focus on the narrow fix"),
        [&completionError](const QString& error) { completionError = error; });
    const std::vector<frontend::Json> commands = capturedCommands(outbound, "turn.steer");
    passed &= expect(!immediateError && commands.size() == 1,
                     "a valid steering prompt must submit exactly one typed turn.steer command");
    if (commands.size() != 1 || !commands.front().contains("requestId"))
        return false;

    const frontend::Json expectedParams{
        {"expectedTurnId", "turn-active"},
        {"input",
         frontend::Json::array({{{"text", "focus on the narrow fix"},
                                 {"text_elements", frontend::Json::array()},
                                 {"type", "text"}}})},
        {"threadId", "thread-active"},
    };
    const frontend::Json actualParams = commands.front().value("params", frontend::Json::object());
    passed &= expect(actualParams == expectedParams,
                     "steering must preserve the canonical thread/turn identities and exact typed text input");

    const std::string requestId = commands.front()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                requestId, frontend::Json{{"turnId", "turn-active"}})}),
        "the matching turn.steer response must be accepted");
    passed &= expect(completionError.isEmpty(),
                     "a matching accepted turn identity must complete steering successfully");

    QString imageCompletion = QStringLiteral("completion not called");
    const auto imageError = session.steerTurn(
        QStringLiteral("thread-active"),
        QStringLiteral("turn-active"),
        {},
        QStringList{QStringLiteral("/tmp/screenshot.png")},
        [&imageCompletion](const QString& error) { imageCompletion = error; });
    const std::vector<frontend::Json> imageCommands = capturedCommands(outbound, "turn.steer");
    passed &= expect(!imageError && imageCommands.size() == 2,
                     "an image-only steer must submit one additional typed turn.steer command");
    if (imageCommands.size() != 2 || !imageCommands.back().contains("requestId"))
        return false;
    const frontend::Json imageParams = imageCommands.back().value(
        "params", frontend::Json::object());
    passed &= expect(
        imageParams.value("input", frontend::Json::array())
            == frontend::Json::array({{{"path", "/tmp/screenshot.png"},
                                       {"type", "localImage"}}}),
        "local image attachments must stay typed instead of being embedded into prompt text");
    const std::string imageRequestId = imageCommands.back()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                imageRequestId, frontend::Json{{"turnId", "turn-active"}})}),
        "the image-only turn.steer response must be accepted");
    passed &= expect(imageCompletion.isEmpty(),
                     "an accepted image-only steer must complete successfully");

    const std::size_t outboundBeforeInvalid = outbound.size();
    const auto missingIdentityError = session.steerTurn(
        QStringLiteral("thread-active"), {}, QStringLiteral("do not send"), [](const QString&) {});
    passed &= expect(missingIdentityError.has_value()
                         && outbound.size() == outboundBeforeInvalid,
                     "steering without the canonical active turn identity must fail before transport submission");
    return passed;
}

bool testOutboundQueue()
{
    codexui::FrontendSession session;
    bool passed = true;

    sdk::OutboundMessage closedMessage{
        sdk::OutboundKind::Command,
        R"({"closed":true})",
        15,
        true};
    auto result = codexui::FrontendSessionTestAccess::send(session, closedMessage);
    passed &= expect(result.status == sdk::SendStatus::Closed
                         && closedMessage.compactJson.empty(),
                     "a closed transport must reject and scrub the moved outbound message");

    const std::string firstFrame = R"({"first":1})";
    const std::string firstWire = firstFrame + '\n';
    std::string written;
    int writeCalls = 0;
    result = codexui::FrontendSessionTestAccess::sendToTransport(
        session,
        firstFrame,
        true,
        0,
        [&written, &writeCalls](const char* bytes, qint64 size) {
            ++writeCalls;
            const qint64 accepted = writeCalls == 1 ? std::min<qint64>(3, size) : size;
            written.append(bytes, static_cast<std::size_t>(accepted));
            return accepted;
        });
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && codexui::FrontendSessionTestAccess::outboundDrainIsScheduled(session)
                         && written + codexui::FrontendSessionTestAccess::pendingWire(session) == firstWire,
                     "a positive short write must accept ownership and retain the exact suffix");
    passed &= expect(codexui::FrontendSessionTestAccess::drainOutbound(
                         session,
                         [&written](const char* bytes, qint64 size) {
                             written.append(bytes, static_cast<std::size_t>(size));
                             return size;
                         })
                         && written == firstWire
                         && !codexui::FrontendSessionTestAccess::outboundDrainIsScheduled(session)
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == 0,
                     "draining a short write must produce the original frame exactly once");

    const std::size_t maximumFrameSize =
        ai::openai::codex::frontend::DefaultFrontendMaximumInboundMessageBytes;
    const std::string maximumFrame(maximumFrameSize, 'x');
    qint64 maximumWireWritten = 0;
    bool maximumWireValid = true;
    result = codexui::FrontendSessionTestAccess::sendToTransport(
        session,
        maximumFrame,
        true,
        0,
        [&maximumWireWritten, &maximumWireValid](const char* bytes, qint64 size) {
            const qint64 accepted = std::min<qint64>(4093, size);
            for (qint64 index = 0; index < accepted; ++index)
                maximumWireValid = maximumWireValid && bytes[index] == 'x';
            maximumWireWritten += accepted;
            return accepted;
        });
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && maximumWireValid
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session)
                                == static_cast<qint64>(maximumFrameSize + 1U) - maximumWireWritten,
                     "a maximum-size SDK frame must retain its exact suffix after a partial socket write");
    passed &= expect(codexui::FrontendSessionTestAccess::drainOutbound(
                         session,
                         [&maximumWireWritten, &maximumWireValid, maximumFrameSize](
                             const char* bytes, qint64 size) {
                             for (qint64 index = 0; index < size; ++index) {
                                 const auto wireIndex = static_cast<std::size_t>(
                                     maximumWireWritten + index);
                                 const char expected = wireIndex == maximumFrameSize ? '\n' : 'x';
                                 maximumWireValid = maximumWireValid && bytes[index] == expected;
                             }
                             maximumWireWritten += size;
                             return size;
                         })
                         && maximumWireValid
                         && maximumWireWritten == static_cast<qint64>(maximumFrameSize + 1U)
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == 0,
                     "a partially written maximum-size SDK frame must drain once in exact wire order");

    written.clear();
    writeCalls = 0;
    const std::string secondFrame = R"({"second":2})";
    const std::string thirdFrame = R"({"third":3})";
    const std::string orderedWire = secondFrame + '\n' + thirdFrame + '\n';
    result = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        secondFrame,
        0,
        [&written, &writeCalls](const char* bytes, qint64 size) {
            ++writeCalls;
            const qint64 accepted = std::min<qint64>(2, size);
            written.append(bytes, static_cast<std::size_t>(accepted));
            return accepted;
        });
    bool secondWriterCalled = false;
    const auto secondResult = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        thirdFrame,
        0,
        [&secondWriterCalled](const char*, qint64 size) {
            secondWriterCalled = true;
            return size;
        });
    for (int drain = 0;
         drain < 3 && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) > 0;
         ++drain) {
        const qint64 beforeDrain = codexui::FrontendSessionTestAccess::pendingWriteBytes(session);
        const bool drained = codexui::FrontendSessionTestAccess::drainOutbound(
            session,
            [&written](const char* bytes, qint64 size) {
                written.append(bytes, static_cast<std::size_t>(size));
                return size;
            });
        passed &= drained
                  && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) < beforeDrain;
    }
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && secondResult.status == sdk::SendStatus::Accepted
                         && !secondWriterCalled
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == 0
                         && written == orderedWire,
                     "queued frames must preserve exact FIFO order");

    const std::string blockedFrame = R"({"blocked":true})";
    result = codexui::FrontendSessionTestAccess::sendToTransport(
        session,
        blockedFrame,
        true,
        0,
        [](const char*, qint64) { return qint64{0}; });
    const std::string blockedWire = blockedFrame + '\n';
    const qint64 blockedWireBytes = static_cast<qint64>(blockedWire.size());
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && codexui::FrontendSessionTestAccess::pendingWire(session) == blockedWire
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == blockedWireBytes
                         && codexui::FrontendSessionTestAccess::outboundDrainIsScheduled(session)
                         && codexui::FrontendSessionTestAccess::drainOutbound(
                             session,
                             [](const char*, qint64) { return qint64{0}; })
                         && codexui::FrontendSessionTestAccess::pendingWire(session) == blockedWire
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == blockedWireBytes,
                     "zero write progress must retain the complete frame for retry");
    codexui::FrontendSessionTestAccess::clearOutbound(session);
    passed &= expect(!codexui::FrontendSessionTestAccess::outboundDrainIsScheduled(session),
                     "transport cleanup must cancel a pending drain retry");

    bool capacityWriterCalled = false;
    result = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        R"({"capacity":true})",
        codexui::FrontendSessionTestAccess::maximumBufferedOutboundBytes(),
        [&capacityWriterCalled](const char*, qint64 size) {
            capacityWriterCalled = true;
            return size;
        });
    passed &= expect(result.status == sdk::SendStatus::Backpressure
                         && result.error && result.error->retryable
                         && !capacityWriterCalled
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == 0,
                     "capacity rejection must be retryable and occur before queue or writer mutation");

    const std::string exactFrame = R"({"exact":true})";
    const qint64 exactFrameBytes = static_cast<qint64>(exactFrame.size() + 1U);
    std::string exactWire;
    result = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        exactFrame,
        codexui::FrontendSessionTestAccess::maximumBufferedOutboundBytes() - exactFrameBytes,
        [&exactWire](const char* bytes, qint64 size) {
            exactWire.append(bytes, static_cast<std::size_t>(size));
            return size;
        });
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && exactWire == exactFrame + '\n',
                     "an outbound frame that exactly fits the combined cap must be accepted");

    const std::string queuedFrame = R"({"queued":true})";
    result = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        queuedFrame,
        0,
        [](const char*, qint64) { return qint64{0}; });
    const std::string combinedFrame = R"({"combined":true})";
    const qint64 combinedFrameBytes = static_cast<qint64>(combinedFrame.size() + 1U);
    const qint64 expectedQueuedBytes = static_cast<qint64>(queuedFrame.size() + 1U);
    const std::string queuedWire = codexui::FrontendSessionTestAccess::pendingWire(session);
    const qint64 oneByteOver = codexui::FrontendSessionTestAccess::maximumBufferedOutboundBytes()
                               - expectedQueuedBytes - combinedFrameBytes + 1;
    const auto combinedRejected = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        combinedFrame,
        oneByteOver,
        [](const char*, qint64 size) { return size; });
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == expectedQueuedBytes
                         && combinedRejected.status == sdk::SendStatus::Backpressure
                         && codexui::FrontendSessionTestAccess::pendingWire(session) == queuedWire,
                     "the cap must include both Qt-buffered and application-held suffix bytes");
    const auto combinedAccepted = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        combinedFrame,
        oneByteOver - 1,
        [](const char*, qint64 size) { return size; });
    passed &= expect(combinedAccepted.status == sdk::SendStatus::Accepted
                         && codexui::FrontendSessionTestAccess::pendingWire(session)
                                == queuedWire + combinedFrame + '\n',
                     "combined buffering exactly at the cap must remain admissible");
    codexui::FrontendSessionTestAccess::clearOutbound(session);

    result = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        R"({"old":true})",
        0,
        [](const char*, qint64 size) { return std::min<qint64>(1, size); });
    codexui::FrontendSessionTestAccess::disconnectTransport(session);
    std::string newWire;
    const std::string newFrame = R"({"new":true})";
    const auto newResult = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        newFrame,
        0,
        [&newWire](const char* bytes, qint64 size) {
            newWire.append(bytes, static_cast<std::size_t>(size));
            return size;
        });
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && newResult.status == sdk::SendStatus::Accepted
                         && newWire == newFrame + '\n',
                     "transport cleanup must not carry an old suffix into a new connection");

    bool reentrantClearWasDeferred = false;
    result = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        R"({"reentrant":true})",
        0,
        [&session, &reentrantClearWasDeferred](const char*, qint64) {
            codexui::FrontendSessionTestAccess::clearOutbound(session);
            reentrantClearWasDeferred = codexui::FrontendSessionTestAccess::outboundClearIsDeferred(session);
            return qint64{1};
        });
    passed &= expect(result.status == sdk::SendStatus::Closed
                         && reentrantClearWasDeferred
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == 0,
                     "reentrant transport cleanup must invalidate the in-flight queue access");

    result = codexui::FrontendSessionTestAccess::acceptOutbound(
        session,
        R"({"failure":true})",
        0,
        [](const char*, qint64) { return qint64{-1}; });
    passed &= expect(result.status == sdk::SendStatus::Failed
                         && codexui::FrontendSessionTestAccess::pendingWriteBytes(session) == 0,
                     "a negative write must fail without retaining owned frame data");
    return passed;
}

bool testInboundBufferCompaction()
{
    codexui::FrontendSession session;
    const QByteArray prefix(300 * 1024, 'p');
    const QByteArray tail(300 * 1024, 't');
    const QByteArray backlog = prefix + tail;

    codexui::FrontendSessionTestAccess::setInbound(session, backlog, 64 * 1024);
    codexui::FrontendSessionTestAccess::compactInbound(session);
    bool passed = expect(
        codexui::FrontendSessionTestAccess::inboundBytes(session) == backlog
            && codexui::FrontendSessionTestAccess::inboundOffset(session) == 64 * 1024,
        "small consumed prefixes must remain as an offset instead of moving a large replay tail");

    codexui::FrontendSessionTestAccess::setInbound(session, backlog, prefix.size());
    codexui::FrontendSessionTestAccess::compactInbound(session);
    passed &= expect(
        codexui::FrontendSessionTestAccess::inboundBytes(session) == tail
            && codexui::FrontendSessionTestAccess::inboundOffset(session) == 0,
        "a large consumed prefix must compact once it reaches both the threshold and half the buffer");

    const QByteArray completeAndPartial("done\npartial");
    codexui::FrontendSessionTestAccess::setInbound(session, completeAndPartial, 0);
    const bool completeAtStart = codexui::FrontendSessionTestAccess::hasCompleteInboundFrame(session);
    codexui::FrontendSessionTestAccess::setInbound(session, completeAndPartial, 5);
    passed &= expect(completeAtStart
                         && !codexui::FrontendSessionTestAccess::hasCompleteInboundFrame(session),
                     "frame detection must ignore newlines in the consumed prefix");

    codexui::FrontendSessionTestAccess::setInbound(session, tail, tail.size());
    codexui::FrontendSessionTestAccess::compactInbound(session);
    passed &= expect(codexui::FrontendSessionTestAccess::inboundBytes(session).isEmpty()
                         && codexui::FrontendSessionTestAccess::inboundOffset(session) == 0,
                     "a fully consumed inbound buffer must reset without retaining capacity state");
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    return testPeerCredentials() && testScopedItemPresentationChanges() && testLifecycleAndDiagnostics()
               && testPreReadyReconnectBound() && testReceiveRejectionPreservesPreciseError()
               && testInboundFrameCapacityTracksSdk()
               && testModelCatalogRefresh() && testModelCatalogRefreshFailureIsDiagnosed()
               && testArchivedThreadRefresh()
               && testArchivedThreadRefreshFailureRemainsIncomplete()
               && testTurnSteeringSubmission()
               && testOutboundQueue() && testInboundBufferCompaction()
           ? 0
           : 1;
}
