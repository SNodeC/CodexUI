// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSessionWorker.h"

#include <ai/openai/codex/frontend/Codec.h>
#include <ai/openai/codex/frontend/Messages.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

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

struct FrontendSessionWorkerTestAccess
{
    static void setLifecycle(FrontendSessionWorker& session, FrontendSessionWorker::Lifecycle lifecycle, QString detail = {})
    {
        session.setLifecycle(lifecycle, std::move(detail));
    }

    static void handleConnectionStateChange(
        FrontendSessionWorker& session,
        const ai::openai::codex::frontend::client::ConnectionStateChange& change)
    {
        session.handleConnectionStateChange(change);
    }

    static void reportDiagnostic(FrontendSessionWorker& session, QString message)
    {
        session.reportDiagnostic(std::move(message));
    }

    static bool automaticReconnectEnabled(const FrontendSessionWorker& session)
    {
        return session.automaticReconnectEnabled;
    }

    static int consecutivePreReadyDisconnects(const FrontendSessionWorker& session)
    {
        return session.consecutivePreReadyDisconnects;
    }

    static int maximumConsecutivePreReadyDisconnects()
    {
        return FrontendSessionWorker::maximumConsecutivePreReadyDisconnects;
    }

    static std::size_t maximumFrameBytes(const FrontendSessionWorker& session)
    {
        return session.maximumFrameBytes;
    }

    static void resetReconnectPolicy(FrontendSessionWorker& session)
    {
        session.resetReconnectPolicy();
    }

    static void setInbound(FrontendSessionWorker& session, QByteArray bytes, qsizetype offset)
    {
        session.inboundBuffer = std::move(bytes);
        session.inboundOffset = offset;
        session.inboundScanOffset = offset;
    }

    static void appendInbound(FrontendSessionWorker& session, const QByteArray& bytes)
    {
        session.inboundBuffer.append(bytes);
    }

    static qsizetype inboundScanOffset(const FrontendSessionWorker& session)
    {
        return session.inboundScanOffset;
    }

    static void compactInbound(FrontendSessionWorker& session)
    {
        session.compactInbound();
    }

    static QByteArray inboundBytes(const FrontendSessionWorker& session)
    {
        return session.inboundBuffer;
    }

    static qsizetype inboundOffset(const FrontendSessionWorker& session)
    {
        return session.inboundOffset;
    }

    static bool hasCompleteInboundFrame(const FrontendSessionWorker& session)
    {
        return session.hasCompleteInboundFrame();
    }

    static void prepareReconnectReset(FrontendSessionWorker& session)
    {
        session.automaticReconnectEnabled = false;
        session.reconnectDelayMs = FrontendSessionWorker::maximumReconnectDelayMs;
        session.reconnectTimer.start(60'000);
    }

    static void installConnectionWithTerminalClose(FrontendSessionWorker& session, bool& closeObserved)
    {
        session.connection = session.client->openConnection({
            [](FrontendSessionWorker::OutboundMessage) {
                return FrontendSessionWorker::SendResult{
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
    acceptOutbound(FrontendSessionWorker& session,
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
    sendToTransport(FrontendSessionWorker& session,
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

    static bool drainOutbound(FrontendSessionWorker& session,
                              const std::function<qint64(const char*, qint64)>& writer)
    {
        const FrontendSessionWorker::DrainResult result = session.drainOutbound(writer);
        return result != FrontendSessionWorker::DrainResult::Failed
               && result != FrontendSessionWorker::DrainResult::Reset;
    }

    static std::string pendingWire(const FrontendSessionWorker& session)
    {
        std::string wire;
        for (const FrontendSessionWorker::PendingWrite& pending : session.pendingWrites) {
            const qint64 frameSize = static_cast<qint64>(pending.frame.size());
            if (pending.offset < 0 || pending.offset > frameSize)
                return "<invalid pending-write offset>";
            const std::size_t offset = static_cast<std::size_t>(pending.offset);
            wire.append(pending.frame.data() + offset, pending.frame.size() - offset);
        }
        return wire;
    }

    static qint64 pendingWriteBytes(const FrontendSessionWorker& session)
    {
        return session.pendingWriteBytes;
    }

    static qint64 maximumBufferedOutboundBytes()
    {
        return FrontendSessionWorker::maximumBufferedOutboundBytes;
    }

    static void clearOutbound(FrontendSessionWorker& session)
    {
        session.clearOutbound();
    }

    static bool outboundDrainIsScheduled(const FrontendSessionWorker& session)
    {
        return session.outboundDrainTimer.isActive();
    }

    static ai::openai::codex::frontend::client::SendResult
    send(FrontendSessionWorker& session, ai::openai::codex::frontend::client::OutboundMessage& message)
    {
        return session.send(std::move(message));
    }

    static bool outboundClearIsDeferred(const FrontendSessionWorker& session)
    {
        return session.outboundClearPending && session.pendingWriteBytes > 0;
    }

    static void disconnectTransport(FrontendSessionWorker& session)
    {
        session.preReadyFailureRecordedCurrentConnection = false;
        session.socketDisconnected();
        session.reconnectTimer.stop();
    }

    static void failTransport(FrontendSessionWorker& session, bool newConnectionAttempt = true)
    {
        if (newConnectionAttempt)
            session.preReadyFailureRecordedCurrentConnection = false;
        session.socketFailed(QLocalSocket::ConnectionRefusedError);
        session.reconnectTimer.stop();
    }

    static bool synchronizeWithCapturedTransport(
        FrontendSessionWorker& session,
        std::vector<ai::openai::codex::frontend::client::OutboundMessage>& messages,
        ai::openai::codex::frontend::Json threads =
            ai::openai::codex::frontend::Json::array(),
        std::size_t omittedThreads = 0)
    {
        namespace frontend = ai::openai::codex::frontend;
        namespace sdk = frontend::client;

        session.connection = session.client->openConnection({
            [&messages](FrontendSessionWorker::OutboundMessage message) {
                messages.push_back(std::move(message));
                return FrontendSessionWorker::SendResult{sdk::SendStatus::Accepted, std::nullopt};
            },
            [](std::string) {},
        });
        session.connection.transportConnected();

        // ThreadReadStateEffects is a required observed mechanism. It must not
        // be mixed into Hello's representation-capability request list.
        if (messages.empty())
            return false;
        const auto decodedHello = frontend::Codec::decodeClient(
            std::string_view(messages.front().compactJson));
        const auto* hello = decodedHello
            ? std::get_if<frontend::Hello>(&decodedHello.value())
            : nullptr;
        if (!hello || !hello->capabilities
            || std::ranges::find(*hello->capabilities,
                                 frontend::FrontendCapability::ThreadReadStateEffects)
                   != hello->capabilities->end())
            return false;

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
            {"threads", std::move(threads)},
            {"pendingRequests", frontend::Json::array()},
            {"codexExtensions", frontend::Json::array()},
            {"omittedCodexExtensions", std::uint64_t{0}},
            {"capacityProvenance",
             {{"omittedThreads", omittedThreads},
              {"truncated", omittedThreads > 0}}},
            {"journal",
             {{"oldestReplayableAfter", std::uint64_t{0}},
              {"currentSequence", std::uint64_t{0}}}},
            {"sequenceExhausted", false},
        };
        const frontend::FrontendCapability threadReadStateEffects =
            frontend::FrontendCapability::ThreadReadStateEffects;
        return session.connection
                   .receive(frontend::ServerMessage{frontend::Welcome{
                       "archived-refresh-test",
                       frontend::SessionRole::Observer,
                       frontend::SequenceNumber{0},
                       frontend::SyncMode::Snapshot,
                       frontend::Json::object(),
                       frontend::CapabilityAdvertisement{
                           {threadReadStateEffects},
                           {threadReadStateEffects},
                           {threadReadStateEffects},
                           frontend::Json::object()}}})
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

    static bool rejectsMissingThreadReadStateEffects(
        FrontendSessionWorker& session,
        std::vector<ai::openai::codex::frontend::client::OutboundMessage>& messages)
    {
        namespace frontend = ai::openai::codex::frontend;
        namespace sdk = frontend::client;

        session.connection = session.client->openConnection({
            [&messages](FrontendSessionWorker::OutboundMessage message) {
                messages.push_back(std::move(message));
                return FrontendSessionWorker::SendResult{
                    sdk::SendStatus::Accepted, std::nullopt};
            },
            [](std::string) {},
        });
        session.connection.transportConnected();
        const auto result = session.connection.receive(
            frontend::ServerMessage{frontend::Welcome{
                "missing-thread-read-effects",
                frontend::SessionRole::Observer,
                frontend::SequenceNumber{0},
                frontend::SyncMode::Snapshot,
                frontend::Json::object(),
                frontend::CapabilityAdvertisement{
                    {}, {}, {}, frontend::Json::object()}}});
        return !result.accepted
            && session.currentLifecycle == FrontendSessionWorker::Lifecycle::Failed
            && !session.automaticReconnectEnabled;
    }

    static bool receive(FrontendSessionWorker& session,
                        ai::openai::codex::frontend::ServerMessage message)
    {
        return session.connection.receive(std::move(message)).accepted;
    }

    static void publishStateUpdate(
        FrontendSessionWorker& session,
        const ai::openai::codex::frontend::client::StateUpdate& update)
    {
        session.handleStateUpdate(update);
    }

    static void receiveWire(FrontendSessionWorker& session, QByteArray wire)
    {
        session.inboundBuffer = std::move(wire);
        session.inboundOffset = 0;
        session.socketReadyRead();
    }

    static void beginArchivedThreadRefresh(FrontendSessionWorker& session)
    {
        session.beginArchivedThreadRefresh();
    }

    static bool archivedThreadListInFlight(const FrontendSessionWorker& session)
    {
        return session.archivedThreadListInFlight;
    }

    static std::size_t archivedThreadCursorCount(const FrontendSessionWorker& session)
    {
        return session.archivedThreadListCursors.size();
    }
};

struct FrontendSessionFacadeTestAccess
{
    static void enqueueState(FrontendSession& session,
                             std::uint64_t generation,
                             detail::StateUpdateScope scope)
    {
        session.enqueueStateForTest(generation, std::move(scope));
    }

    static void enqueueStatus(FrontendSession& session,
                              std::uint64_t generation,
                              QString status)
    {
        session.enqueueStatusForTest(generation, std::move(status));
    }

    static void enqueueStatus(FrontendSession& session,
                              std::uint64_t generation,
                              FrontendSession::Lifecycle lifecycle,
                              QString status)
    {
        session.enqueueStatusForTest(
            generation, lifecycle, std::move(status));
    }

    static void enqueueLifecycle(FrontendSession& session,
                                 std::uint64_t generation,
                                 FrontendSession::Lifecycle lifecycle,
                                 QString status)
    {
        session.enqueueLifecycleForTest(
            generation, lifecycle, std::move(status));
    }

    static void enqueueModels(
        FrontendSession& session,
        std::uint64_t generation,
        std::vector<ai::openai::codex::typed::Model> models)
    {
        session.enqueueModelsForTest(generation, std::move(models));
    }

    static std::size_t pendingStateCount(const FrontendSession& session)
    {
        return session.pendingStateCountForTest();
    }

    static std::size_t pendingControlCount(const FrontendSession& session)
    {
        return session.pendingControlCountForTest();
    }

    static std::size_t postedWakeCount(const FrontendSession& session)
    {
        return session.postedWakeCountForTest();
    }

    static bool workerAffinityValidated(const FrontendSession& session)
    {
        return session.workerAffinityValidatedForTest();
    }

    static void trackOperation(FrontendSession& session,
                               FrontendSession::OperationCompletion completion)
    {
        session.trackOperationForTest(std::move(completion));
    }

    static void completeOperation(
        FrontendSession& session,
        std::uint64_t generation,
        FrontendSession::OperationCompletion completion,
        QString error)
    {
        session.completeOperationForTest(
            generation, std::move(completion), std::move(error));
    }
};

} // namespace codexui

namespace {

namespace frontend = ai::openai::codex::frontend;
namespace sdk = ai::openai::codex::frontend::client;
namespace typed = ai::openai::codex::typed;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

frontend::Json threadReadStateEffect(std::string_view authority,
                                     bool sourcePartial = false,
                                     std::uint64_t omittedTurns = 0,
                                     std::uint64_t omittedItems = 0)
{
    const bool responseTruncated = omittedTurns != 0 || omittedItems != 0;
    return frontend::Json{
        {"scope", "thread"},
        {"authority", authority},
        {"truncation",
         {{"sourcePartial", sourcePartial},
          {"responseTruncated", responseTruncated},
          {"responseOmittedTurns", omittedTurns},
          {"responseOmittedItems", omittedItems}}},
    };
}

frontend::Json threadReadBody(std::string_view threadId, bool fullyLoaded)
{
    return frontend::Json{
        {"id", threadId},
        {"fullyLoaded", fullyLoaded},
        {"turns", frontend::Json::array()},
        {"extensions", frontend::Json::object()},
    };
}

frontend::Json negotiatedThreadReadResult(std::string_view threadId,
                                          std::string_view authority,
                                          bool sourcePartial = false)
{
    if (authority == "absent") {
        return frontend::Json{
            {"threadId", threadId},
            {"stateEffect", threadReadStateEffect(authority)},
        };
    }
    const bool fullyLoaded = authority == "replace";
    return frontend::Json{
        {"thread", threadReadBody(threadId, fullyLoaded)},
        {"stateEffect",
         threadReadStateEffect(authority, sourcePartial)},
    };
}

bool negotiatedThreadReadRequested(const frontend::Json& command)
{
    return command.value("threadReadStateEffectVersion", 0) == 1;
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

    sdk::StateUpdate appendedUpdate;
    appendedUpdate.changes.push_back(
        sdk::ItemContentAppendedChange{
            ai::openai::codex::typed::ItemId{"streamed-item"},
            sdk::ItemContentChannel::ReasoningText,
            ai::openai::codex::typed::ThreadId{"target-thread"},
            ai::openai::codex::typed::TurnId{"target-turn"},
            17,
            3,
            std::string{"exact \xF0\x9F\x98\x80 bytes"},
        });
    const auto appended = codexui::detail::stateUpdateScope(appendedUpdate);

    sdk::StateUpdate oversizedAppendUpdate;
    oversizedAppendUpdate.changes.push_back(
        sdk::ItemContentAppendedChange{
            ai::openai::codex::typed::ItemId{"oversized-item"},
            sdk::ItemContentChannel::CommandOutput,
            ai::openai::codex::typed::ThreadId{"target-thread"},
            ai::openai::codex::typed::TurnId{"target-turn"},
            0,
            0,
            std::string(
                static_cast<std::size_t>(
                    codexui::detail::maximumCoalescedContentDeltaBytes + 1),
                'x'),
        });
    const auto oversizedAppend =
        codexui::detail::stateUpdateScope(oversizedAppendUpdate);

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

    sdk::StateUpdate threadUpdate;
    threadUpdate.changes.push_back(
        sdk::ThreadUpsertedChange{ai::openai::codex::typed::ThreadId{"target-thread"}});
    const auto threadScoped = codexui::detail::stateUpdateScope(threadUpdate);

    sdk::StateUpdate removedThreadUpdate;
    removedThreadUpdate.changes.push_back(
        sdk::ThreadRemovedChange{ai::openai::codex::typed::ThreadId{"removed-thread"}});
    const auto removedThreadScoped =
        codexui::detail::stateUpdateScope(removedThreadUpdate);

    sdk::StateUpdate cursorUpdate;
    cursorUpdate.changes.push_back(
        sdk::CursorAdvancedChange{ai::openai::codex::frontend::SequenceNumber{43}});
    const auto cursor = codexui::detail::stateUpdateScope(cursorUpdate);

    sdk::StateUpdate oversizedIdentityUpdate;
    for (int index = 0;
         index <= codexui::detail::maximumCoalescedPresentationIdentities;
         ++index) {
        oversizedIdentityUpdate.changes.push_back(
            sdk::ThreadUpsertedChange{ai::openai::codex::typed::ThreadId{
                "thread-" + std::to_string(index)}});
    }
    const auto boundedIdentities =
        codexui::detail::stateUpdateScope(oversizedIdentityUpdate);

    bool passed = expect(unresolvedTurn.affectedThreadIds.empty()
                             && unresolvedTurn.fullyAffectedThreadIds.empty()
                             && unresolvedTurn.affectedInspectorThreadIds.empty()
                             && unresolvedTurn.allThreadsAffected
                             && unresolvedTurn.allInspectorsAffected
                             && unresolvedTurn.allSidebarThreadsAffected
                             && unresolvedTurn.sidebarAffected
                             && unresolvedTurn.hasPresentationChange,
                         "a turn upsert without a unique parent lookup must conservatively refresh all threads");
    passed &= expect(scoped.affectedThreadIds == QStringList{QStringLiteral("target-thread")}
                             && scoped.fullyAffectedThreadIds.empty()
                             && scoped.affectedInspectorThreadIds
                                    == QStringList{QStringLiteral("target-thread")}
                             && !scoped.allThreadsAffected && !scoped.allInspectorsAffected
                             && !scoped.sidebarAffected && scoped.hasPresentationChange,
                         "a scoped item upsert must reconcile its canonical conversation and Inspector without invalidating retained widgets");
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
    passed &= expect(
        appended.affectedItemContents.size() == 1
            && appended.affectedItemContents.front().channel
                   == sdk::ItemContentChannel::ReasoningText
            && appended.affectedItemContents.front().append
            && appended.affectedItemContents.front().append->baseContentBytes == 17
            && appended.affectedItemContents.front().append->discardPrefixBytes == 3
            && appended.affectedItemContents.front().append->deltaUtf8
                   == QByteArray("exact \xF0\x9F\x98\x80 bytes")
            && appended.coalescedContentDeltaBytes
                   == static_cast<std::uint64_t>(
                       QByteArray("exact \xF0\x9F\x98\x80 bytes").size()),
        "an authoritative append change must retain its channel and exact UTF-8 byte contract");
    passed &= expect(
        oversizedAppend.affectedItemContents.size() == 1
            && oversizedAppend.affectedItemContents.front().channel
                   == sdk::ItemContentChannel::CommandOutput
            && !oversizedAppend.affectedItemContents.front().append
            && oversizedAppend.coalescedContentDeltaBytes == 0,
        "an oversized append hint must degrade to an authoritative replacement without entering the GUI mailbox");
    passed &= expect(mixed.fullyAffectedThreadIds.empty()
                         && mixed.affectedItemContents.size() == 1
                         && !mixed.allThreadsAffected,
                     "a structural item change mixed with exact content must retain bounded widget reconciliation");
    passed &= expect(unscoped.affectedThreadIds.empty() && unscoped.allThreadsAffected
                         && unscoped.fullyAffectedThreadIds.empty()
                         && unscoped.affectedItemContents.empty()
                         && unscoped.affectedInspectorThreadIds.empty()
                         && unscoped.allInspectorsAffected && !unscoped.sidebarAffected
                         && unscoped.hasPresentationChange,
                     "an unscoped item change must conservatively refresh all thread-bound presentations");
    passed &= expect(replacement.allThreadsAffected && replacement.allInspectorsAffected
                         && replacement.allSidebarThreadsAffected
                         && replacement.sidebarAffected && replacement.hasPresentationChange,
                     "a State replacement must conservatively refresh every presentation");
    passed &= expect(threadScoped.affectedThreadIds
                             == QStringList{QStringLiteral("target-thread")}
                         && threadScoped.fullyAffectedThreadIds
                                == QStringList{QStringLiteral("target-thread")}
                         && threadScoped.affectedInspectorThreadIds
                                == QStringList{QStringLiteral("target-thread")}
                         && threadScoped.affectedSidebarThreadIds
                                == QStringList{QStringLiteral("target-thread")}
                         && !threadScoped.allThreadsAffected
                         && !threadScoped.allInspectorsAffected
                         && !threadScoped.allSidebarThreadsAffected
                         && threadScoped.sidebarAffected,
                     "a thread upsert must target only its conversation, Inspector dependencies, and Sidebar row");
    passed &= expect(
        removedThreadScoped.affectedThreadIds
                == QStringList{QStringLiteral("removed-thread")}
            && removedThreadScoped.fullyAffectedThreadIds
                   == QStringList{QStringLiteral("removed-thread")}
            && removedThreadScoped.removedThreadIds
                   == QStringList{QStringLiteral("removed-thread")}
            && removedThreadScoped.affectedSidebarThreadIds
                   == QStringList{QStringLiteral("removed-thread")}
            && removedThreadScoped.affectedInspectorThreadIds
                   == QStringList{QStringLiteral("removed-thread")},
        "an authoritative thread removal must preserve its exact identity through the GUI scope");
    passed &= expect(!cursor.allThreadsAffected && !cursor.allInspectorsAffected
                         && !cursor.allSidebarThreadsAffected
                         && !cursor.sidebarAffected && cursor.hasPresentationChange,
                     "a cursor-only update must dispatch its revision without dirtying broad presentation");
    passed &= expect(boundedIdentities.allThreadsAffected
                         && boundedIdentities.allInspectorsAffected
                         && boundedIdentities.allSidebarThreadsAffected
                         && boundedIdentities.affectedThreadIds.empty()
                         && boundedIdentities.fullyAffectedThreadIds.empty()
                         && boundedIdentities.affectedInspectorThreadIds.empty()
                         && boundedIdentities.affectedSidebarThreadIds.empty(),
                     "an oversized identity batch must stop at the presentation bound and degrade to full refreshes");
    return passed;
}

bool testLifecycleAndDiagnostics()
{
    int lifecycleChanges = 0;
    int statusChanges = 0;
    bool reconnectCloseObserved = false;
    codexui::FrontendSessionWorker session;
    QObject::connect(&session, &codexui::FrontendSessionWorker::lifecycleChanged, [&lifecycleChanges] { ++lifecycleChanges; });
    QObject::connect(&session, &codexui::FrontendSessionWorker::statusChanged, [&statusChanges] { ++statusChanges; });

    codexui::FrontendSessionWorkerTestAccess::setLifecycle(session, codexui::FrontendSessionWorker::Lifecycle::Ready);
    codexui::FrontendSessionWorkerTestAccess::setLifecycle(session, codexui::FrontendSessionWorker::Lifecycle::Ready);
    bool passed = expect(lifecycleChanges == 1, "an identical lifecycle and detail must not emit a duplicate transition");

    codexui::FrontendSessionWorkerTestAccess::reportDiagnostic(session, QStringLiteral("projection diagnostic"));
    codexui::FrontendSessionWorkerTestAccess::reportDiagnostic(session, QStringLiteral("projection diagnostic"));
    codexui::FrontendSessionWorkerTestAccess::setLifecycle(session, codexui::FrontendSessionWorker::Lifecycle::Ready);
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Ready
                         && session.statusText() == QStringLiteral("projection diagnostic")
                         && lifecycleChanges == 1 && statusChanges == 1,
                     "an error diagnostic must update status once without changing a ready lifecycle");

    sdk::Error retryableError;
    retryableError.message = "temporary backend failure";
    retryableError.retryable = true;
    const sdk::ConnectionStateChange retryableChange{
        sdk::ConnectionState::Ready, sdk::ConnectionState::Disconnected, retryableError};
    codexui::FrontendSessionWorkerTestAccess::handleConnectionStateChange(session, retryableChange);
    const int retryableSignalCount = lifecycleChanges;
    codexui::FrontendSessionWorkerTestAccess::handleConnectionStateChange(session, retryableChange);
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
                         && codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                         && lifecycleChanges == retryableSignalCount && retryableSignalCount == 2,
                     "a retryable connection error must produce one failed transition and retain automatic reconnect");

    sdk::Error terminalError;
    terminalError.message = "terminal protocol failure";
    terminalError.retryable = false;
    codexui::FrontendSessionWorkerTestAccess::handleConnectionStateChange(
        session,
        {sdk::ConnectionState::Disconnected, sdk::ConnectionState::Closed, terminalError});
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
                         && !codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                         && lifecycleChanges == 3,
                     "a nonretryable connection error must produce one failed transition and disable automatic reconnect");
    codexui::FrontendSessionWorkerTestAccess::handleConnectionStateChange(
        session,
        {sdk::ConnectionState::Closed, sdk::ConnectionState::Disconnected, std::nullopt});
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
                         && lifecycleChanges == 3,
                     "a following physical close must preserve the terminal failure");

    codexui::FrontendSessionWorkerTestAccess::prepareReconnectReset(session);
    codexui::FrontendSessionWorkerTestAccess::installConnectionWithTerminalClose(session, reconnectCloseObserved);
    session.reconnectToBackend();
    passed &= expect(reconnectCloseObserved
                         && codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session),
                     "the public reconnect path must override a terminal old-transport close callback");
    return passed;
}

bool testPreReadyReconnectBound()
{
    codexui::FrontendSessionWorker session;
    const int maximum = codexui::FrontendSessionWorkerTestAccess::maximumConsecutivePreReadyDisconnects();
    bool passed = true;
    for (int attempt = 1; attempt < maximum; ++attempt) {
        codexui::FrontendSessionWorkerTestAccess::disconnectTransport(session);
        passed &= expect(codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                             && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(session) == attempt,
                         "a bounded number of pre-synchronization disconnects remains retryable");
    }

    codexui::FrontendSessionWorkerTestAccess::disconnectTransport(session);
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
                         && !codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                         && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(session) == maximum
                         && session.statusText().contains(QStringLiteral("before synchronization completed")),
                     "repeated pre-synchronization disconnects stop at a visible terminal boundary");

    codexui::FrontendSessionWorkerTestAccess::resetReconnectPolicy(session);
    std::vector<sdk::OutboundMessage> messages;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, messages)
            && session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Ready
            && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(session) == 0,
        "the real SDK synchronization callback must reset the pre-ready retry budget");
    codexui::FrontendSessionWorkerTestAccess::disconnectTransport(session);
    passed &= expect(codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                         && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(session) == 0,
                     "a disconnect after synchronization does not consume the pre-ready retry budget");

    codexui::FrontendSessionWorker failedConnectSession;
    for (int attempt = 1; attempt < maximum; ++attempt) {
        codexui::FrontendSessionWorkerTestAccess::failTransport(failedConnectSession);
        passed &= expect(
            codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(failedConnectSession)
                && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(failedConnectSession) == attempt,
            "a bounded number of failed pre-synchronization connection attempts remains retryable");
        codexui::FrontendSessionWorkerTestAccess::failTransport(failedConnectSession, false);
        passed &= expect(
            codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(failedConnectSession) == attempt,
            "multiple failure signals for one connection attempt consume the retry budget only once");
    }
    codexui::FrontendSessionWorkerTestAccess::failTransport(failedConnectSession);
    passed &= expect(
        failedConnectSession.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
            && !codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(failedConnectSession)
            && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(failedConnectSession) == maximum,
        "repeated failed connection attempts stop at the same visible terminal boundary");
    return passed;
}

bool testReceiveRejectionPreservesPreciseError()
{
    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> messages;
    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, messages),
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

    codexui::FrontendSessionWorkerTestAccess::receiveWire(
        session, QByteArray::fromStdString(duplicateWelcome.value() + '\n'));
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
                         && !codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                         && session.statusText() == QStringLiteral("unexpected or duplicate Welcome")
                         && !session.statusText().contains(
                             QStringLiteral("frontend server message was rejected"),
                             Qt::CaseInsensitive),
                     "socketReadyRead must preserve the precise SDK lifecycle error instead of the generic receive rejection");
    codexui::FrontendSessionWorkerTestAccess::failTransport(session, false);
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
                         && !codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                         && session.statusText() == QStringLiteral("unexpected or duplicate Welcome")
                         && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(session) == 0,
                     "the socket error following a terminal SDK rejection must preserve its precise reason and retry budget");
    codexui::FrontendSessionWorkerTestAccess::disconnectTransport(session);
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Failed
                         && !codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(session)
                         && session.statusText() == QStringLiteral("unexpected or duplicate Welcome")
                         && codexui::FrontendSessionWorkerTestAccess::consecutivePreReadyDisconnects(session) == 0,
                     "the physical disconnect following a terminal SDK rejection must preserve its precise reason and retry budget");
    return passed;
}

bool testInboundFrameCapacityTracksSdk()
{
    codexui::FrontendSessionWorker session;
    const sdk::ClientOptions defaults;
    return expect(
        codexui::FrontendSessionWorkerTestAccess::maximumFrameBytes(session) == defaults.maximumInboundMessageBytes
            && codexui::FrontendSessionWorkerTestAccess::maximumFrameBytes(session) > 16U * 1024U * 1024U,
        "the Qt JSONL receiver must accept the SDK's complete provider-derived server-message range");
}

std::vector<frontend::Json>
capturedCommands(const std::vector<sdk::OutboundMessage>& messages,
                 std::string_view method);

bool testIncompleteThreadReadIsBounded()
{
    codexui::FrontendSessionWorker incompatibleSession;
    std::vector<sdk::OutboundMessage> incompatibleOutbound;
    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::rejectsMissingThreadReadStateEffects(
            incompatibleSession, incompatibleOutbound),
        "a backend without required thread-read State effects must fail the handshake without reconnecting");

    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> outbound;
    frontend::Json threads = frontend::Json::array({
        frontend::Json{{"id", "partial-thread"},
                       {"fullyLoaded", false},
                       {"turns", frontend::Json::array()},
                       {"extensions", frontend::Json::object()}},
        frontend::Json{{"id", "retry-thread"},
                       {"fullyLoaded", false},
                       {"turns", frontend::Json::array()},
                       {"extensions", frontend::Json::object()}},
        frontend::Json{{"id", "complete-thread"},
                       {"fullyLoaded", true},
                       {"turns", frontend::Json::array()},
                       {"extensions", frontend::Json::object()}},
    });
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(
            session, outbound, std::move(threads)),
        "the incomplete-thread recovery fixture must reach synchronized State");
    outbound.clear();

    session.loadThread(QStringLiteral("complete-thread"));
    // Without explicit omission provenance, absence from a complete thread
    // list remains authoritative and must not trigger a speculative read.
    session.loadThread(QStringLiteral("missing-thread"));
    session.loadThread(QStringLiteral("partial-thread"));
    session.loadThread(QStringLiteral("partial-thread"));
    std::vector<frontend::Json> reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 1
            && negotiatedThreadReadRequested(reads.front())
            && reads.front().value("params", frontend::Json::object())
                   == frontend::Json{{"threadId", "partial-thread"},
                                     {"includeTurns", true}},
        "only an incomplete retained thread may request one negotiated authoritative full read");
    if (reads.size() != 1 || !reads.front().contains("requestId"))
        return false;

    const auto publishThreadCompleteness = [&session](std::uint64_t sequence,
                                                       bool fullyLoaded) {
        frontend::FrontendEvent update{
            frontend::SequenceNumber{sequence},
            "thread.updated",
            frontend::Json{{"thread",
                            {{"id", "partial-thread"},
                             {"fullyLoaded", fullyLoaded}}}},
        };
        return codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::EventBatch{
                update.sequence, update.sequence, {std::move(update)}}});
    };
    passed &= expect(
        publishThreadCompleteness(1, true)
            && publishThreadCompleteness(2, false),
        "intermediate State updates around an outstanding thread read must be accepted");
    session.loadThread(QStringLiteral("partial-thread"));
    reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 1,
        "State reconciliation must not release in-flight thread-read ownership before its operation completes");

    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                reads.front()["requestId"].get<std::string>(),
                negotiatedThreadReadResult(
                    "partial-thread", "merge", true))}),
        "the partial-thread Merge result must be accepted");
    session.loadThread(QStringLiteral("partial-thread"));
    reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 1,
        "a successful acknowledgement must remain de-duplicated until authoritative State completes the thread");

    sdk::StateUpdate regressingReplacement;
    regressingReplacement.state = session.state();
    regressingReplacement.changes.push_back(sdk::StateReplacedChange{});
    codexui::FrontendSessionWorkerTestAccess::publishStateUpdate(
        session, regressingReplacement);
    session.loadThread(QStringLiteral("partial-thread"));
    session.loadThread(QStringLiteral("partial-thread"));
    reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 2,
        "a later replacement revision that regresses retained history must earn exactly one new automatic read");
    if (reads.size() != 2 || !reads.back().contains("requestId"))
        return false;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                reads.back()["requestId"].get<std::string>(),
                negotiatedThreadReadResult(
                    "partial-thread", "merge", true))}),
        "the replacement-epoch recovery acknowledgement must be accepted");
    session.loadThread(QStringLiteral("partial-thread"));
    reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 2,
        "an unchanged incomplete replacement epoch must remain bounded after its successful read");

    session.loadThread(QStringLiteral("partial-thread"), true);
    reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 3,
        "an explicit user retry may re-read a still-incomplete thread without enabling an automatic loop");
    if (reads.size() != 3 || !reads.back().contains("requestId"))
        return false;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                reads.back()["requestId"].get<std::string>(),
                negotiatedThreadReadResult(
                    "partial-thread", "merge", true))}),
        "the explicit incomplete-thread retry acknowledgement must be accepted");

    session.loadThread(QStringLiteral("retry-thread"));
    reads = capturedCommands(outbound, "thread.read");
    if (!expect(reads.size() == 4 && reads.back().contains("requestId"),
                "a different incomplete thread must receive its own bounded read"))
        return false;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::failure(
                reads.back()["requestId"].get<std::string>(),
                frontend::CommandError{frontend::ErrorCode::CapacityExceeded,
                                       "thread read fence was overtaken"})}),
        "the capacity-limited recovery read must be accepted as an operation response");
    sdk::StateUpdate liveRetryThreadUpdate;
    liveRetryThreadUpdate.state = session.state();
    liveRetryThreadUpdate.changes.push_back(
        sdk::ThreadUpsertedChange{
            ai::openai::codex::typed::ThreadId{"retry-thread"}});
    codexui::FrontendSessionWorkerTestAccess::publishStateUpdate(
        session, liveRetryThreadUpdate);
    session.loadThread(QStringLiteral("retry-thread"));
    reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 4,
        "a failed automatic read must consume the current replacement epoch instead of polling after live updates");

    session.loadThread(QStringLiteral("retry-thread"), true);
    reads = capturedCommands(outbound, "thread.read");
    passed &= expect(
        reads.size() == 5 && reads.back().contains("requestId"),
        "an explicit retry may re-read a capacity-limited recovery without enabling automatic polling");

    codexui::FrontendSessionWorker authoritySession;
    std::vector<sdk::OutboundMessage> authorityOutbound;
    frontend::Json authorityThreads = frontend::Json::array({
        frontend::Json{{"id", "replace-thread"},
                       {"fullyLoaded", false},
                       {"turns", frontend::Json::array()},
                       {"extensions", frontend::Json::object()}},
    });
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(
            authoritySession,
            authorityOutbound,
            std::move(authorityThreads),
            1),
        "the negotiated authority fixture must reach synchronized State");
    std::optional<codexui::detail::StateUpdateScope> authorityScope;
    QObject::connect(
        &authoritySession,
        &codexui::FrontendSessionWorker::stateChanged,
        [&authorityScope](const auto& scope) { authorityScope = scope; });
    authorityOutbound.clear();
    authoritySession.loadThread(QStringLiteral("replace-thread"));
    std::vector<frontend::Json> authorityReads = capturedCommands(
        authorityOutbound, "thread.read");
    if (!expect(authorityReads.size() == 1
                    && negotiatedThreadReadRequested(authorityReads.front())
                    && authorityReads.front().contains("requestId"),
                "the complete authority fixture must negotiate one Replace read"))
        return false;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            authoritySession,
            frontend::ServerMessage{frontend::Response::success(
                authorityReads.front()["requestId"].get<std::string>(),
                negotiatedThreadReadResult(
                    "replace-thread", "replace"))}),
        "the authoritative Replace result must be accepted");
    const auto* replaced = authoritySession.state().thread("replace-thread");
    authoritySession.loadThread(QStringLiteral("replace-thread"));
    passed &= expect(
        replaced && replaced->fullyLoaded
            && capturedCommands(authorityOutbound, "thread.read").size() == 1,
        "Replace must complete the cached thread and suppress further recovery reads");

    authorityScope.reset();
    authoritySession.loadThread(QStringLiteral("absent-thread"));
    authorityReads = capturedCommands(authorityOutbound, "thread.read");
    if (!expect(authorityReads.size() == 2
                    && authorityReads.back().contains("requestId"),
                "an omitted identity must receive one negotiated absence check"))
        return false;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            authoritySession,
            frontend::ServerMessage{frontend::Response::success(
                authorityReads.back()["requestId"].get<std::string>(),
                negotiatedThreadReadResult(
                    "absent-thread", "absent"))})
            && authoritySession.state().thread("absent-thread") == nullptr
            && authorityScope
            && authorityScope->removedThreadIds
                   == QStringList{QStringLiteral("absent-thread")},
        "Absent must publish one exact removal tombstone before completion");

    codexui::FrontendSessionWorker omittedSession;
    std::vector<sdk::OutboundMessage> omittedOutbound;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(
            omittedSession,
            omittedOutbound,
            frontend::Json::array(),
            1),
        "the omitted-thread recovery fixture must reach synchronized State");
    passed &= expect(
        omittedSession.state().capacityProvenance()
            && omittedSession.state().capacityProvenance()->omittedThreads == 1,
        "the recovery fixture must expose its bounded snapshot omission provenance");
    omittedOutbound.clear();
    omittedSession.loadThread(QStringLiteral("omitted-thread"));
    omittedSession.loadThread(QStringLiteral("omitted-thread"));
    std::vector<frontend::Json> omittedReads = capturedCommands(
        omittedOutbound, "thread.read");
    passed &= expect(
        omittedReads.size() == 1
            && omittedReads.front().value("params", frontend::Json::object())
                   == frontend::Json{{"threadId", "omitted-thread"},
                                     {"includeTurns", true}},
        "a selected ID absent from an explicitly bounded snapshot must receive one recovery read");
    if (omittedReads.size() != 1 || !omittedReads.front().contains("requestId"))
        return false;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            omittedSession,
            frontend::ServerMessage{frontend::Response::success(
                omittedReads.front()["requestId"].get<std::string>(),
                negotiatedThreadReadResult(
                    "omitted-thread", "merge", true))}),
        "the omitted-thread Merge result must be accepted");
    omittedSession.loadThread(QStringLiteral("omitted-thread"));
    omittedReads = capturedCommands(omittedOutbound, "thread.read");
    passed &= expect(
        omittedReads.size() == 1,
        "a successful missing-thread recovery must remain bounded until State resolves the omission");
    omittedSession.loadThread(QStringLiteral("omitted-thread"), true);
    omittedReads = capturedCommands(omittedOutbound, "thread.read");
    passed &= expect(
        omittedReads.size() == 2,
        "an explicit user retry may verify a still-omitted identity without enabling automatic polling");

    const auto projectedThreads = [] {
        return frontend::Json::array({
            frontend::Json{{"id", "projected-thread"},
                           {"fullyLoaded", false},
                           {"turns", frontend::Json::array()},
                           {"extensions", frontend::Json::object()}},
        });
    };
    codexui::FrontendSessionWorker reconnectSession;
    std::vector<sdk::OutboundMessage> firstConnectionOutbound;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(
            reconnectSession,
            firstConnectionOutbound,
            projectedThreads()),
        "the projected-selection fixture must synchronize its first connection");
    firstConnectionOutbound.clear();
    reconnectSession.loadThread(QStringLiteral("projected-thread"), true);
    std::vector<frontend::Json> firstConnectionReads = capturedCommands(
        firstConnectionOutbound, "thread.read");
    if (!expect(firstConnectionReads.size() == 1
                    && firstConnectionReads.front().contains("requestId"),
                "a projected incomplete selection must issue one read on its first Ready boundary"))
        return false;
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            reconnectSession,
            frontend::ServerMessage{frontend::Response::success(
                firstConnectionReads.front()["requestId"].get<std::string>(),
                negotiatedThreadReadResult(
                    "projected-thread", "merge", true))}),
        "the first projected-selection read acknowledgement must be accepted");

    codexui::FrontendSessionWorkerTestAccess::disconnectTransport(
        reconnectSession);
    std::vector<sdk::OutboundMessage> secondConnectionOutbound;
    const bool disconnectedForRetry =
        reconnectSession.lifecycle()
                == codexui::FrontendSessionWorker::Lifecycle::Failed
        && codexui::FrontendSessionWorkerTestAccess::automaticReconnectEnabled(
            reconnectSession);
    const bool synchronizedAgain =
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(
            reconnectSession,
            secondConnectionOutbound,
            projectedThreads());
    passed &= expect(
        disconnectedForRetry && synchronizedAgain
            && reconnectSession.lifecycle()
                   == codexui::FrontendSessionWorker::Lifecycle::Ready,
        "the projected-selection fixture must disconnect and synchronize a new Ready connection");
    secondConnectionOutbound.clear();
    // Workbench invokes the explicit retry once when the retained projected
    // selection crosses the new Ready boundary. Ordinary presentation refreshes
    // can immediately follow it and must not submit duplicates.
    reconnectSession.loadThread(QStringLiteral("projected-thread"), true);
    reconnectSession.loadThread(QStringLiteral("projected-thread"));
    reconnectSession.loadThread(QStringLiteral("projected-thread"));
    const std::vector<frontend::Json> secondConnectionReads = capturedCommands(
        secondConnectionOutbound, "thread.read");
    passed &= expect(
        secondConnectionReads.size() == 1,
        "a projected selection retained across disconnect and a new Ready connection must issue exactly one recovery read");
    return passed;
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
        {"description", "FrontendSessionWorker model catalogue fixture"},
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
    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> outbound;
    int catalogueSignals = 0;
    QObject::connect(&session, &codexui::FrontendSessionWorker::modelCatalogChanged,
                     [&catalogueSignals] { ++catalogueSignals; });

    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, outbound),
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
        codexui::FrontendSessionWorkerTestAccess::receive(
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
        codexui::FrontendSessionWorkerTestAccess::receive(
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
    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> outbound;
    int catalogueSignals = 0;
    QObject::connect(&session, &codexui::FrontendSessionWorker::modelCatalogChanged,
                     [&catalogueSignals] { ++catalogueSignals; });

    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the model-catalogue failure fixture must reach a synchronized SDK connection");
    const std::vector<frontend::Json> commands = capturedCommands(outbound, "model.list");
    if (!expect(commands.size() == 1 && commands.front().contains("requestId"),
                "the failure fixture must capture one model-list request"))
        return false;

    const std::string requestId = commands.front()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
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
    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> outbound;
    int discoverySignals = 0;
    std::optional<codexui::detail::StateUpdateScope> discoveryScope;
    QObject::connect(
        &session,
        &codexui::FrontendSessionWorker::stateChanged,
        [&session, &discoverySignals, &discoveryScope](
            const codexui::detail::StateUpdateScope& scope) {
            if (!session.archivedThreadDiscoveryComplete())
                return;
            ++discoverySignals;
            discoveryScope = scope;
        });

    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the archived-thread fixture must reach a synchronized SDK connection");
    std::vector<frontend::Json> commands = capturedCommands(outbound, "thread.list");
    passed &= expect(session.lifecycle() == codexui::FrontendSessionWorker::Lifecycle::Ready
                         && !session.archivedThreadDiscoveryComplete()
                         && codexui::FrontendSessionWorkerTestAccess::archivedThreadListInFlight(session)
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
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                firstRequestId,
                frontend::Json{{"threads", frontend::Json::array()},
                               {"nextCursor", "archived-page-2"}})}),
        "the first archived-thread page response must be accepted");
    commands = capturedCommands(outbound, "thread.list");
    passed &= expect(!session.archivedThreadDiscoveryComplete()
                         && codexui::FrontendSessionWorkerTestAccess::archivedThreadListInFlight(session)
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
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                secondRequestId,
                frontend::Json{{"threads", frontend::Json::array()}})}),
        "the terminal archived-thread page response must be accepted");
    passed &= expect(session.archivedThreadDiscoveryComplete()
                         && !codexui::FrontendSessionWorkerTestAccess::archivedThreadListInFlight(session)
                         && codexui::FrontendSessionWorkerTestAccess::archivedThreadCursorCount(session) == 2
                         && discoverySignals == 1 && discoveryScope
                         && discoveryScope->allThreadsAffected
                         && discoveryScope->allInspectorsAffected
                         && discoveryScope->allSidebarThreadsAffected
                         && discoveryScope->sidebarAffected
                         && discoveryScope->hasPresentationChange,
                     "the terminal page must publish completion and one conservative presentation refresh");

    const std::size_t outboundAtCompletion = outbound.size();
    codexui::FrontendSessionWorkerTestAccess::beginArchivedThreadRefresh(session);
    passed &= expect(outbound.size() == outboundAtCompletion && discoverySignals == 1,
                     "completed archived-thread discovery must not restart or emit duplicate completion refreshes");
    return passed;
}

bool testArchivedThreadRefreshFailureIsTerminal()
{
    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> outbound;

    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the archived-thread failure fixture must reach a synchronized SDK connection");
    const std::vector<frontend::Json> commands = capturedCommands(outbound, "thread.list");
    if (!expect(commands.size() == 1 && commands.front().contains("requestId"),
                "the failure fixture must capture one archived-thread request"))
        return false;

    int stateSignals = 0;
    QObject::connect(&session, &codexui::FrontendSessionWorker::stateChanged,
                     [&stateSignals](const codexui::detail::StateUpdateScope&) {
                         ++stateSignals;
                     });
    const std::string requestId = commands.front()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::failure(
                requestId,
                frontend::CommandError{frontend::ErrorCode::InternalError,
                                       "archived listing failed"})}),
        "the archived-thread failure response must be accepted");
    passed &= expect(!session.archivedThreadDiscoveryComplete()
                         && session.archivedThreadDiscoveryTerminal()
                         && session.archivedThreadDiscoveryStatus()
                                == codexui::FrontendSessionWorker::ArchivedThreadDiscoveryStatus::Failed
                         && !codexui::FrontendSessionWorkerTestAccess::archivedThreadListInFlight(session),
                     "a failed archived-thread request must stop in-flight work as a terminal failure without claiming a complete result");
    passed &= expect(stateSignals == 1,
                     "a terminal archived-thread failure must unblock presentation reconciliation");
    passed &= expect(session.statusText().contains(QStringLiteral("archived listing failed")),
                     "a failed archived-thread request must still surface its diagnostic");
    return passed;
}

bool testArchivedThreadPaginationTruncationIsTerminal()
{
    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> outbound;
    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, outbound),
        "the archived-thread truncation fixture must synchronize");
    std::vector<frontend::Json> commands = capturedCommands(outbound, "thread.list");
    if (!expect(commands.size() == 1 && commands.front().contains("requestId"),
                "the truncation fixture must capture the first archived-thread request"))
        return false;

    const std::string firstRequestId = commands.front()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                firstRequestId,
                frontend::Json{{"threads", frontend::Json::array()},
                               {"nextCursor", "repeated-cursor"}})}),
        "the first truncated archived-thread page must be accepted");
    commands = capturedCommands(outbound, "thread.list");
    if (!expect(commands.size() == 2 && commands.back().contains("requestId"),
                "the truncation fixture must request the repeated-cursor page once"))
        return false;

    int stateSignals = 0;
    QObject::connect(&session, &codexui::FrontendSessionWorker::stateChanged,
                     [&stateSignals](const codexui::detail::StateUpdateScope&) {
                         ++stateSignals;
                     });
    const std::string secondRequestId = commands.back()["requestId"].get<std::string>();
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::receive(
            session,
            frontend::ServerMessage{frontend::Response::success(
                secondRequestId,
                frontend::Json{{"threads", frontend::Json::array()},
                               {"nextCursor", "repeated-cursor"}})}),
        "the repeated archived-thread cursor response must be accepted");
    passed &= expect(!session.archivedThreadDiscoveryComplete()
                         && session.archivedThreadDiscoveryTerminal()
                         && session.archivedThreadDiscoveryStatus()
                                == codexui::FrontendSessionWorker::ArchivedThreadDiscoveryStatus::CompleteWithTruncation
                         && !codexui::FrontendSessionWorkerTestAccess::archivedThreadListInFlight(session)
                         && stateSignals == 1,
                     "a repeated pagination cursor must terminate discovery as a truncated result and unblock reconciliation");
    passed &= expect(session.statusText().contains(QStringLiteral("invalid pagination boundary")),
                     "truncated archived-thread discovery must explain its pagination boundary");
    return passed;
}

bool testTurnSteeringSubmission()
{
    codexui::FrontendSessionWorker session;
    std::vector<sdk::OutboundMessage> outbound;
    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::synchronizeWithCapturedTransport(session, outbound),
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
        codexui::FrontendSessionWorkerTestAccess::receive(
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
        codexui::FrontendSessionWorkerTestAccess::receive(
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
    codexui::FrontendSessionWorker session;
    bool passed = true;

    sdk::OutboundMessage closedMessage{
        sdk::OutboundKind::Command,
        R"({"closed":true})",
        15,
        true};
    auto result = codexui::FrontendSessionWorkerTestAccess::send(session, closedMessage);
    passed &= expect(result.status == sdk::SendStatus::Closed
                         && closedMessage.compactJson.empty(),
                     "a closed transport must reject and scrub the moved outbound message");

    const std::string firstFrame = R"({"first":1})";
    const std::string firstWire = firstFrame + '\n';
    std::string written;
    int writeCalls = 0;
    result = codexui::FrontendSessionWorkerTestAccess::sendToTransport(
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
                         && codexui::FrontendSessionWorkerTestAccess::outboundDrainIsScheduled(session)
                         && written + codexui::FrontendSessionWorkerTestAccess::pendingWire(session) == firstWire,
                     "a positive short write must accept ownership and retain the exact suffix");
    passed &= expect(codexui::FrontendSessionWorkerTestAccess::drainOutbound(
                         session,
                         [&written](const char* bytes, qint64 size) {
                             written.append(bytes, static_cast<std::size_t>(size));
                             return size;
                         })
                         && written == firstWire
                         && !codexui::FrontendSessionWorkerTestAccess::outboundDrainIsScheduled(session)
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == 0,
                     "draining a short write must produce the original frame exactly once");

    const std::size_t maximumFrameSize =
        ai::openai::codex::frontend::DefaultFrontendMaximumInboundMessageBytes;
    const std::string maximumFrame(maximumFrameSize, 'x');
    qint64 maximumWireWritten = 0;
    bool maximumWireValid = true;
    result = codexui::FrontendSessionWorkerTestAccess::sendToTransport(
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
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session)
                                == static_cast<qint64>(maximumFrameSize + 1U) - maximumWireWritten,
                     "a maximum-size SDK frame must retain its exact suffix after a partial socket write");
    passed &= expect(codexui::FrontendSessionWorkerTestAccess::drainOutbound(
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
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == 0,
                     "a partially written maximum-size SDK frame must drain once in exact wire order");

    written.clear();
    writeCalls = 0;
    const std::string secondFrame = R"({"second":2})";
    const std::string thirdFrame = R"({"third":3})";
    const std::string orderedWire = secondFrame + '\n' + thirdFrame + '\n';
    result = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
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
    const auto secondResult = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        thirdFrame,
        0,
        [&secondWriterCalled](const char*, qint64 size) {
            secondWriterCalled = true;
            return size;
        });
    for (int drain = 0;
         drain < 3 && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) > 0;
         ++drain) {
        const qint64 beforeDrain = codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session);
        const bool drained = codexui::FrontendSessionWorkerTestAccess::drainOutbound(
            session,
            [&written](const char* bytes, qint64 size) {
                written.append(bytes, static_cast<std::size_t>(size));
                return size;
            });
        passed &= drained
                  && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) < beforeDrain;
    }
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && secondResult.status == sdk::SendStatus::Accepted
                         && !secondWriterCalled
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == 0
                         && written == orderedWire,
                     "queued frames must preserve exact FIFO order");

    const std::string blockedFrame = R"({"blocked":true})";
    result = codexui::FrontendSessionWorkerTestAccess::sendToTransport(
        session,
        blockedFrame,
        true,
        0,
        [](const char*, qint64) { return qint64{0}; });
    const std::string blockedWire = blockedFrame + '\n';
    const qint64 blockedWireBytes = static_cast<qint64>(blockedWire.size());
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && codexui::FrontendSessionWorkerTestAccess::pendingWire(session) == blockedWire
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == blockedWireBytes
                         && codexui::FrontendSessionWorkerTestAccess::outboundDrainIsScheduled(session)
                         && codexui::FrontendSessionWorkerTestAccess::drainOutbound(
                             session,
                             [](const char*, qint64) { return qint64{0}; })
                         && codexui::FrontendSessionWorkerTestAccess::pendingWire(session) == blockedWire
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == blockedWireBytes,
                     "zero write progress must retain the complete frame for retry");
    codexui::FrontendSessionWorkerTestAccess::clearOutbound(session);
    passed &= expect(!codexui::FrontendSessionWorkerTestAccess::outboundDrainIsScheduled(session),
                     "transport cleanup must cancel a pending drain retry");

    bool capacityWriterCalled = false;
    result = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        R"({"capacity":true})",
        codexui::FrontendSessionWorkerTestAccess::maximumBufferedOutboundBytes(),
        [&capacityWriterCalled](const char*, qint64 size) {
            capacityWriterCalled = true;
            return size;
        });
    passed &= expect(result.status == sdk::SendStatus::Backpressure
                         && result.error && result.error->retryable
                         && !capacityWriterCalled
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == 0,
                     "capacity rejection must be retryable and occur before queue or writer mutation");

    const std::string exactFrame = R"({"exact":true})";
    const qint64 exactFrameBytes = static_cast<qint64>(exactFrame.size() + 1U);
    std::string exactWire;
    result = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        exactFrame,
        codexui::FrontendSessionWorkerTestAccess::maximumBufferedOutboundBytes() - exactFrameBytes,
        [&exactWire](const char* bytes, qint64 size) {
            exactWire.append(bytes, static_cast<std::size_t>(size));
            return size;
        });
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && exactWire == exactFrame + '\n',
                     "an outbound frame that exactly fits the combined cap must be accepted");

    const std::string queuedFrame = R"({"queued":true})";
    result = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        queuedFrame,
        0,
        [](const char*, qint64) { return qint64{0}; });
    const std::string combinedFrame = R"({"combined":true})";
    const qint64 combinedFrameBytes = static_cast<qint64>(combinedFrame.size() + 1U);
    const qint64 expectedQueuedBytes = static_cast<qint64>(queuedFrame.size() + 1U);
    const std::string queuedWire = codexui::FrontendSessionWorkerTestAccess::pendingWire(session);
    const qint64 oneByteOver = codexui::FrontendSessionWorkerTestAccess::maximumBufferedOutboundBytes()
                               - expectedQueuedBytes - combinedFrameBytes + 1;
    const auto combinedRejected = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        combinedFrame,
        oneByteOver,
        [](const char*, qint64 size) { return size; });
    passed &= expect(result.status == sdk::SendStatus::Accepted
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == expectedQueuedBytes
                         && combinedRejected.status == sdk::SendStatus::Backpressure
                         && codexui::FrontendSessionWorkerTestAccess::pendingWire(session) == queuedWire,
                     "the cap must include both Qt-buffered and application-held suffix bytes");
    const auto combinedAccepted = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        combinedFrame,
        oneByteOver - 1,
        [](const char*, qint64 size) { return size; });
    passed &= expect(combinedAccepted.status == sdk::SendStatus::Accepted
                         && codexui::FrontendSessionWorkerTestAccess::pendingWire(session)
                                == queuedWire + combinedFrame + '\n',
                     "combined buffering exactly at the cap must remain admissible");
    codexui::FrontendSessionWorkerTestAccess::clearOutbound(session);

    result = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        R"({"old":true})",
        0,
        [](const char*, qint64 size) { return std::min<qint64>(1, size); });
    codexui::FrontendSessionWorkerTestAccess::disconnectTransport(session);
    std::string newWire;
    const std::string newFrame = R"({"new":true})";
    const auto newResult = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
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
    result = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        R"({"reentrant":true})",
        0,
        [&session, &reentrantClearWasDeferred](const char*, qint64) {
            codexui::FrontendSessionWorkerTestAccess::clearOutbound(session);
            reentrantClearWasDeferred = codexui::FrontendSessionWorkerTestAccess::outboundClearIsDeferred(session);
            return qint64{1};
        });
    passed &= expect(result.status == sdk::SendStatus::Closed
                         && reentrantClearWasDeferred
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == 0,
                     "reentrant transport cleanup must invalidate the in-flight queue access");

    result = codexui::FrontendSessionWorkerTestAccess::acceptOutbound(
        session,
        R"({"failure":true})",
        0,
        [](const char*, qint64) { return qint64{-1}; });
    passed &= expect(result.status == sdk::SendStatus::Failed
                         && codexui::FrontendSessionWorkerTestAccess::pendingWriteBytes(session) == 0,
                     "a negative write must fail without retaining owned frame data");
    return passed;
}

bool testInboundBufferCompaction()
{
    codexui::FrontendSessionWorker session;
    const QByteArray prefix(300 * 1024, 'p');
    const QByteArray tail(300 * 1024, 't');
    const QByteArray backlog = prefix + tail;

    codexui::FrontendSessionWorkerTestAccess::setInbound(session, backlog, 64 * 1024);
    codexui::FrontendSessionWorkerTestAccess::compactInbound(session);
    bool passed = expect(
        codexui::FrontendSessionWorkerTestAccess::inboundBytes(session) == backlog
            && codexui::FrontendSessionWorkerTestAccess::inboundOffset(session) == 64 * 1024,
        "small consumed prefixes must remain as an offset instead of moving a large replay tail");

    codexui::FrontendSessionWorkerTestAccess::setInbound(session, backlog, prefix.size());
    codexui::FrontendSessionWorkerTestAccess::compactInbound(session);
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::inboundBytes(session) == tail
            && codexui::FrontendSessionWorkerTestAccess::inboundOffset(session) == 0,
        "a large consumed prefix must compact once it reaches both the threshold and half the buffer");

    const QByteArray completeAndPartial("done\npartial");
    codexui::FrontendSessionWorkerTestAccess::setInbound(session, completeAndPartial, 0);
    const bool completeAtStart = codexui::FrontendSessionWorkerTestAccess::hasCompleteInboundFrame(session);
    codexui::FrontendSessionWorkerTestAccess::setInbound(session, completeAndPartial, 5);
    passed &= expect(completeAtStart
                         && !codexui::FrontendSessionWorkerTestAccess::hasCompleteInboundFrame(session),
                     "frame detection must ignore newlines in the consumed prefix");

    codexui::FrontendSessionWorkerTestAccess::setInbound(session, tail, tail.size());
    codexui::FrontendSessionWorkerTestAccess::compactInbound(session);
    passed &= expect(codexui::FrontendSessionWorkerTestAccess::inboundBytes(session).isEmpty()
                         && codexui::FrontendSessionWorkerTestAccess::inboundOffset(session) == 0,
                     "a fully consumed inbound buffer must reset without retaining capacity state");

    const QByteArray firstPartial(16 * 1024 * 1024, 'a');
    codexui::FrontendSessionWorkerTestAccess::setInbound(session, firstPartial, 0);
    passed &= expect(
        !codexui::FrontendSessionWorkerTestAccess::hasCompleteInboundFrame(session)
            && codexui::FrontendSessionWorkerTestAccess::inboundScanOffset(session)
                   == firstPartial.size(),
        "a large partial frame must remember the exact prefix already scanned for a terminator");
    const QByteArray secondPartial(1024 * 1024, 'b');
    codexui::FrontendSessionWorkerTestAccess::appendInbound(session, secondPartial);
    passed &= expect(
        !codexui::FrontendSessionWorkerTestAccess::hasCompleteInboundFrame(session)
            && codexui::FrontendSessionWorkerTestAccess::inboundScanOffset(session)
                   == firstPartial.size() + secondPartial.size(),
        "receiving another chunk must scan only the newly appended partial-frame suffix");
    codexui::FrontendSessionWorkerTestAccess::appendInbound(session, QByteArray("\n"));
    passed &= expect(
        codexui::FrontendSessionWorkerTestAccess::hasCompleteInboundFrame(session)
            && codexui::FrontendSessionWorkerTestAccess::inboundScanOffset(session)
                   == firstPartial.size() + secondPartial.size(),
        "the incremental scan cursor must still detect the terminator at the first new byte");
    return passed;
}

bool testThreadedFacadeMailbox()
{
    codexui::FrontendSession session;
    QElapsedTimer wait;
    wait.start();
    while (!codexui::FrontendSessionFacadeTestAccess::workerAffinityValidated(session)
           && wait.elapsed() < 2'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }

    bool passed = expect(
        codexui::FrontendSessionFacadeTestAccess::workerAffinityValidated(session),
        "the frontend engine, Unix socket, and timers must originate on the one worker thread");

    int stateSignals = 0;
    int statusSignals = 0;
    bool callbacksOnGuiThread = true;
    QThread* const guiThread = QThread::currentThread();
    QStringList deliveryOrder;
    std::optional<codexui::detail::StateUpdateScope> deliveredScope;
    QObject::connect(
        &session,
        &codexui::FrontendSession::stateChanged,
        [&stateSignals, &deliveredScope, &callbacksOnGuiThread,
         guiThread](const auto& scope) {
            callbacksOnGuiThread = callbacksOnGuiThread
                                   && QThread::currentThread() == guiThread;
            ++stateSignals;
            deliveredScope = scope;
        });
    QObject::connect(&session,
                     &codexui::FrontendSession::statusChanged,
                     [&statusSignals, &deliveryOrder, &callbacksOnGuiThread,
                      guiThread] {
                         callbacksOnGuiThread =
                             callbacksOnGuiThread
                             && QThread::currentThread() == guiThread;
                         ++statusSignals;
                         deliveryOrder.push_back(QStringLiteral("status"));
                     });
    QObject::connect(
        &session,
        &codexui::FrontendSession::stateChanged,
        [&deliveryOrder](const auto&) {
            deliveryOrder.push_back(QStringLiteral("state"));
        });

    const std::size_t wakesBefore =
        codexui::FrontendSessionFacadeTestAccess::postedWakeCount(session);
    const auto appendScope = [](QString itemId,
                                sdk::ItemContentChannel channel,
                                std::uint64_t base,
                                QByteArray delta) {
        codexui::detail::StateUpdateScope scope;
        scope.affectedThreadIds.push_back(
            QStringLiteral("streaming-thread"));
        scope.affectedItemContents.push_back({
            QStringLiteral("streaming-thread"),
            QStringLiteral("streaming-turn"),
            std::move(itemId),
            channel,
            codexui::detail::StateUpdateScope::ItemContentAppend{
                base,
                0,
                std::move(delta),
            },
        });
        scope.coalescedContentDeltaBytes = static_cast<std::uint64_t>(
            scope.affectedItemContents.front().append->deltaUtf8.size());
        scope.hasPresentationChange = true;
        return scope;
    };
    codexui::FrontendSessionFacadeTestAccess::enqueueState(
        session,
        7,
        appendScope(QStringLiteral("contiguous"),
                    sdk::ItemContentChannel::AgentText,
                    10,
                    QByteArray("ab")));
    codexui::FrontendSessionFacadeTestAccess::enqueueState(
        session,
        7,
        appendScope(QStringLiteral("contiguous"),
                    sdk::ItemContentChannel::AgentText,
                    12,
                    QByteArray("cd")));
    codexui::FrontendSessionFacadeTestAccess::enqueueState(
        session,
        7,
        appendScope(QStringLiteral("contiguous"),
                    sdk::ItemContentChannel::ReasoningText,
                    20,
                    QByteArray("reasoning")));
    codexui::FrontendSessionFacadeTestAccess::enqueueState(
        session,
        7,
        appendScope(QStringLiteral("ambiguous"),
                    sdk::ItemContentChannel::AgentText,
                    0,
                    QByteArray("first")));
    codexui::FrontendSessionFacadeTestAccess::enqueueState(
        session,
        7,
        appendScope(QStringLiteral("ambiguous"),
                    sdk::ItemContentChannel::AgentText,
                    99,
                    QByteArray("second")));
    codexui::FrontendSessionFacadeTestAccess::enqueueState(
        session,
        7,
        appendScope(
            QStringLiteral("oversized"),
            sdk::ItemContentChannel::AgentText,
            0,
            QByteArray(
                static_cast<qsizetype>(
                    codexui::detail::maximumCoalescedContentDeltaBytes + 1),
                'x')));
    {
        codexui::detail::StateUpdateScope scope;
        scope.affectedSidebarThreadIds = {
            QStringLiteral("sidebar-a"), QStringLiteral("sidebar-b")};
        scope.sidebarAffected = true;
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 7, std::move(scope));
    }
    {
        codexui::detail::StateUpdateScope scope;
        scope.affectedThreadIds = {QStringLiteral("removed-thread")};
        scope.fullyAffectedThreadIds = {QStringLiteral("removed-thread")};
        scope.removedThreadIds = {QStringLiteral("removed-thread")};
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 7, std::move(scope));
    }
    {
        codexui::detail::StateUpdateScope scope;
        scope.affectedSidebarThreadIds = {
            QStringLiteral("sidebar-b"), QStringLiteral("sidebar-c")};
        scope.sidebarAffected = true;
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 7, std::move(scope));
    }
    for (int index = 0; index < 1'000; ++index) {
        codexui::detail::StateUpdateScope scope;
        scope.affectedThreadIds.push_back(QStringLiteral("streaming-thread"));
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 7, std::move(scope));
        codexui::FrontendSessionFacadeTestAccess::enqueueStatus(
            session,
            7,
            QStringLiteral("diagnostic-%1").arg(index));
    }
    int successfulCompletions = 0;
    QString successfulCompletionValue;
    codexui::FrontendSessionFacadeTestAccess::completeOperation(
        session,
        7,
        [&successfulCompletions, &successfulCompletionValue,
         &callbacksOnGuiThread, guiThread](const QString& value) {
            callbacksOnGuiThread = callbacksOnGuiThread
                                   && QThread::currentThread() == guiThread;
            ++successfulCompletions;
            successfulCompletionValue = value;
        },
        QStringLiteral("completed"));

    passed &= expect(
        codexui::FrontendSessionFacadeTestAccess::pendingStateCount(session) == 1,
        "interleaved control events must never let more than one full State wait for the GUI");
    passed &= expect(
        codexui::FrontendSessionFacadeTestAccess::pendingControlCount(session)
            == 2,
        "a blocked GUI must retain only the latest replaceable status and the lossless completion");
    passed &= expect(
        codexui::FrontendSessionFacadeTestAccess::postedWakeCount(session)
                - wakesBefore
            == 1,
        "a burst of worker publications must post exactly one GUI wakeup");

    wait.restart();
    while ((codexui::FrontendSessionFacadeTestAccess::pendingStateCount(session) != 0
            || statusSignals != 1 || successfulCompletions != 1)
           && wait.elapsed() < 2'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    passed &= expect(
        stateSignals == 1 && deliveredScope
            && deliveredScope->affectedThreadIds
                   == QStringList{QStringLiteral("streaming-thread"),
                                  QStringLiteral("removed-thread")}
            && deliveredScope->removedThreadIds
                   == QStringList{QStringLiteral("removed-thread")},
        "the one latest State publication must retain the merged presentation scope");
    if (deliveredScope) {
        const auto content = [&deliveredScope](QStringView itemId,
                                                sdk::ItemContentChannel channel)
            -> const codexui::detail::StateUpdateScope::ItemContentIdentity* {
            const auto found = std::find_if(
                deliveredScope->affectedItemContents.cbegin(),
                deliveredScope->affectedItemContents.cend(),
                [itemId, channel](const auto& candidate) {
                    return candidate.itemId == itemId
                           && candidate.channel == channel;
                });
            return found == deliveredScope->affectedItemContents.cend()
                       ? nullptr
                       : &*found;
        };
        const auto* contiguous = content(
            QStringView{u"contiguous"}, sdk::ItemContentChannel::AgentText);
        const auto* otherChannel = content(
            QStringView{u"contiguous"}, sdk::ItemContentChannel::ReasoningText);
        const auto* ambiguous = content(
            QStringView{u"ambiguous"}, sdk::ItemContentChannel::AgentText);
        const auto* oversized = content(
            QStringView{u"oversized"}, sdk::ItemContentChannel::AgentText);
        passed &= expect(
            deliveredScope->affectedItemContents.size() == 4 && contiguous
                && contiguous->append
                && contiguous->append->baseContentBytes == 10
                && contiguous->append->deltaUtf8 == QByteArray("abcd")
                && otherChannel && otherChannel->append
                && otherChannel->append->deltaUtf8 == QByteArray("reasoning")
                && ambiguous && !ambiguous->append && oversized
                && !oversized->append
                && deliveredScope->coalescedContentDeltaBytes == 13,
            "the one-slot mailbox must merge only bounded contiguous same-channel appends and degrade ambiguous or oversized sequences to replacement");
        passed &= expect(
            deliveredScope->sidebarAffected
                && !deliveredScope->allSidebarThreadsAffected
                && deliveredScope->affectedSidebarThreadIds
                       == QStringList{QStringLiteral("sidebar-a"),
                                      QStringLiteral("sidebar-b"),
                                      QStringLiteral("sidebar-c")},
            "the one-slot mailbox must merge and deduplicate targeted Sidebar rows");
    }
    passed &= expect(statusSignals == 1
                         && session.statusText()
                                == QStringLiteral("diagnostic-999"),
                     "replaceable status publications must collapse to the newest value around coalesced State updates");
    passed &= expect(successfulCompletions == 1
                         && successfulCompletionValue
                                == QStringLiteral("completed"),
                     "duplicate provider completion attempts must publish exactly one ordered GUI callback");
    passed &= expect(!deliveryOrder.isEmpty()
                         && deliveryOrder.front() == QStringLiteral("state"),
                     "S-C-S interleaving must publish the newest State before callbacks observe it");

    int shutdownCompletions = 0;
    QString shutdownError;
    std::vector<int> shutdownOrder;
    codexui::FrontendSessionFacadeTestAccess::trackOperation(
        session,
        [&shutdownCompletions, &shutdownError, &shutdownOrder,
         &callbacksOnGuiThread, guiThread](const QString& error) {
            callbacksOnGuiThread = callbacksOnGuiThread
                                   && QThread::currentThread() == guiThread;
            ++shutdownCompletions;
            shutdownOrder.push_back(1);
            shutdownError = error;
        });
    codexui::FrontendSessionFacadeTestAccess::trackOperation(
        session,
        [&shutdownCompletions, &shutdownOrder, &callbacksOnGuiThread,
         guiThread](const QString&) {
            callbacksOnGuiThread = callbacksOnGuiThread
                                   && QThread::currentThread() == guiThread;
            ++shutdownCompletions;
            shutdownOrder.push_back(2);
        });
    session.shutdown();
    session.shutdown();
    passed &= expect(shutdownCompletions == 2 && !shutdownError.isEmpty()
                         && shutdownOrder == std::vector<int>{1, 2},
                     "shutdown must fail every retained operation token exactly once in submission order before joining");
    passed &= expect(callbacksOnGuiThread,
                     "all facade signals and completions must execute on the GUI thread");
    return passed;
}

bool testFacadeReplaceableControlCoalescing()
{
    codexui::FrontendSession session;
    const auto model = [](std::string id) {
        typed::Model result;
        result.id = typed::ModelId{id};
        result.model = typed::ModelId{id};
        result.displayName = std::move(id);
        return result;
    };
    const auto publish = [&session, &model](
                             std::uint64_t generation,
                             codexui::FrontendSession::Lifecycle lifecycle,
                             QString status,
                             std::string modelId) {
        codexui::FrontendSessionFacadeTestAccess::enqueueStatus(
            session, generation, lifecycle, std::move(status));
        codexui::FrontendSessionFacadeTestAccess::enqueueModels(
            session, generation, {model(std::move(modelId))});
    };

    int statusSignals = 0;
    int lifecycleSignals = 0;
    int modelSignals = 0;
    std::vector<int> completionOrder;
    bool completionSnapshotsCorrect = true;
    QObject::connect(&session,
                     &codexui::FrontendSession::statusChanged,
                     [&statusSignals] { ++statusSignals; });
    QObject::connect(&session,
                     &codexui::FrontendSession::lifecycleChanged,
                     [&lifecycleSignals] { ++lifecycleSignals; });
    QObject::connect(&session,
                     &codexui::FrontendSession::modelCatalogChanged,
                     [&modelSignals] { ++modelSignals; });

    publish(7,
            codexui::FrontendSession::Lifecycle::Disconnected,
            QStringLiteral("pre-old"),
            "model-pre-old");
    publish(8,
            codexui::FrontendSession::Lifecycle::Disconnected,
            QStringLiteral("pre-current"),
            "model-pre-current");
    publish(7,
            codexui::FrontendSession::Lifecycle::Disconnected,
            QStringLiteral("pre-stale"),
            "model-pre-stale");

    codexui::FrontendSessionFacadeTestAccess::enqueueLifecycle(
        session,
        8,
        codexui::FrontendSession::Lifecycle::Connecting,
        QStringLiteral("connecting"));
    publish(8,
            codexui::FrontendSession::Lifecycle::Connecting,
            QStringLiteral("mid-old"),
            "model-mid-old");
    publish(8,
            codexui::FrontendSession::Lifecycle::Connecting,
            QStringLiteral("mid-current"),
            "model-mid-current");
    codexui::FrontendSessionFacadeTestAccess::completeOperation(
        session,
        8,
        [&session, &completionOrder,
         &completionSnapshotsCorrect](const QString&) {
            completionOrder.push_back(1);
            completionSnapshotsCorrect = completionSnapshotsCorrect
                && session.lifecycle()
                    == codexui::FrontendSession::Lifecycle::Connecting
                && session.statusText() == QStringLiteral("mid-current")
                && session.modelCatalog().size() == 1
                && session.modelCatalog().front().model.value
                    == "model-mid-current";
        },
        QString{});

    publish(8,
            codexui::FrontendSession::Lifecycle::Connecting,
            QStringLiteral("post-old"),
            "model-post-old");
    publish(8,
            codexui::FrontendSession::Lifecycle::Connecting,
            QStringLiteral("post-current"),
            "model-post-current");
    codexui::FrontendSessionFacadeTestAccess::completeOperation(
        session,
        8,
        [&session, &completionOrder,
         &completionSnapshotsCorrect](const QString&) {
            completionOrder.push_back(2);
            completionSnapshotsCorrect = completionSnapshotsCorrect
                && session.lifecycle()
                    == codexui::FrontendSession::Lifecycle::Connecting
                && session.statusText() == QStringLiteral("post-current")
                && session.modelCatalog().size() == 1
                && session.modelCatalog().front().model.value
                    == "model-post-current";
        },
        QString{});

    codexui::FrontendSessionFacadeTestAccess::enqueueLifecycle(
        session,
        8,
        codexui::FrontendSession::Lifecycle::Ready,
        QStringLiteral("ready"));

    bool passed = expect(
        codexui::FrontendSessionFacadeTestAccess::pendingControlCount(session)
            == 10,
        "replaceable controls must remain bounded to one status and one model per lifecycle/completion segment");
    QCoreApplication::processEvents();
    passed &= expect(
        completionOrder == std::vector<int>{1, 2}
            && completionSnapshotsCorrect,
        "operation completions must remain FIFO barriers and observe the preceding status and model publications");
    passed &= expect(
        lifecycleSignals == 2
            && session.lifecycle()
                == codexui::FrontendSession::Lifecycle::Ready,
        "semantically distinct lifecycle transitions must never be coalesced");
    passed &= expect(
        statusSignals == 3
            && session.statusText() == QStringLiteral("ready"),
        "each barrier segment must publish only its highest-generation latest status");
    passed &= expect(
        modelSignals == 3 && session.modelCatalog().size() == 1
            && session.modelCatalog().front().model.value
                == "model-post-current",
        "each barrier segment must publish only its highest-generation latest model catalogue");
    session.shutdown();
    return passed;
}

bool testImmediateFacadeShutdown()
{
    QTemporaryDir isolatedRuntime;
    const QByteArray previousRuntime = qgetenv("XDG_RUNTIME_DIR");
    if (!isolatedRuntime.isValid())
        return expect(false, "immediate-shutdown test requires an isolated runtime directory");
    qputenv("XDG_RUNTIME_DIR", isolatedRuntime.path().toUtf8());

    bool passed = true;
    for (int iteration = 0; iteration < 32; ++iteration) {
        codexui::FrontendSession session;
        int completionCount = 0;
        codexui::FrontendSessionFacadeTestAccess::trackOperation(
            session,
            [&completionCount](const QString&) { ++completionCount; });
        // This deliberately races the worker's first construction/attach. A
        // stop observed before attach must discard pending worker commands;
        // the facade owns the one terminal completion for their gates.
        session.connectToBackend();
        session.shutdown();
        passed &= expect(
            completionCount == 1,
            "immediate facade shutdown must join safely and complete retained operations exactly once");
    }
    if (previousRuntime.isNull())
        qunsetenv("XDG_RUNTIME_DIR");
    else
        qputenv("XDG_RUNTIME_DIR", previousRuntime);
    return passed;
}

bool testFacadeGenerationGating()
{
    codexui::FrontendSession session;
    int statusSignals = 0;
    int completionSignals = 0;
    QObject::connect(&session,
                     &codexui::FrontendSession::statusChanged,
                     [&statusSignals] { ++statusSignals; });

    codexui::FrontendSessionFacadeTestAccess::enqueueStatus(
        session, 3, QStringLiteral("current generation"));
    QCoreApplication::processEvents();
    codexui::FrontendSessionFacadeTestAccess::enqueueStatus(
        session, 2, QStringLiteral("stale generation"));
    codexui::FrontendSessionFacadeTestAccess::completeOperation(
        session,
        1,
        [&completionSignals](const QString&) { ++completionSignals; },
        QString{});
    QCoreApplication::processEvents();

    const bool passed = expect(
        session.statusText() == QStringLiteral("current generation")
            && statusSignals == 1 && completionSignals == 1,
        "stale lifecycle controls must not regress a newer connection generation while operation completions remain lossless");
    session.shutdown();
    return passed;
}

bool testFacadeScopeBound()
{
    codexui::FrontendSession session;
    std::optional<codexui::detail::StateUpdateScope> delivered;
    QObject::connect(
        &session,
        &codexui::FrontendSession::stateChanged,
        [&delivered](const auto& scope) { delivered = scope; });

    {
        codexui::detail::StateUpdateScope scope;
        scope.affectedThreadIds.push_back(QStringLiteral("removed-thread"));
        scope.fullyAffectedThreadIds.push_back(QStringLiteral("removed-thread"));
        scope.removedThreadIds.push_back(QStringLiteral("removed-thread"));
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 1, std::move(scope));
    }
    for (int index = 0; index < 1'100; ++index) {
        codexui::detail::StateUpdateScope scope;
        scope.affectedThreadIds.push_back(QStringLiteral("thread"));
        scope.affectedItemContents.push_back({
            QStringLiteral("thread"),
            QStringLiteral("turn"),
            QStringLiteral("item-%1").arg(index),
            sdk::ItemContentChannel::AgentText,
            std::nullopt,
        });
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 1, std::move(scope));
    }
    QCoreApplication::processEvents();
    bool passed = expect(
        delivered && delivered->allThreadsAffected
            && delivered->affectedThreadIds.empty()
            && delivered->affectedItemContents.empty()
            && delivered->removedThreadIds
                   == QStringList{QStringLiteral("removed-thread")},
        "a blocked GUI must degrade an unbounded exact-scope burst to one bounded full refresh while retaining exact removals");

    delivered.reset();
    {
        codexui::detail::StateUpdateScope removed;
        removed.affectedThreadIds.push_back(
            QStringLiteral("remove-then-upsert"));
        removed.removedThreadIds.push_back(
            QStringLiteral("remove-then-upsert"));
        removed.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 1, std::move(removed));

        codexui::detail::StateUpdateScope upserted;
        upserted.affectedThreadIds.push_back(
            QStringLiteral("remove-then-upsert"));
        upserted.fullyAffectedThreadIds.push_back(
            QStringLiteral("remove-then-upsert"));
        upserted.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 1, std::move(upserted));
    }
    QCoreApplication::processEvents();
    passed &= expect(
        delivered && delivered->removedThreadIds.empty(),
        "a newer exact upsert must supersede a coalesced removal tombstone");

    delivered.reset();
    for (int index = 0;
         index <= codexui::detail::maximumCoalescedPresentationIdentities;
         ++index) {
        codexui::detail::StateUpdateScope scope;
        const QString threadId = QStringLiteral("removed-%1").arg(index);
        scope.affectedThreadIds.push_back(threadId);
        scope.removedThreadIds.push_back(threadId);
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 1, std::move(scope));
    }
    QCoreApplication::processEvents();
    passed &= expect(
        delivered && delivered->allThreadsAffected
            && delivered->removedThreadIdsOverflowed
            && delivered->removedThreadIds.size()
                   == codexui::detail::maximumCoalescedPresentationIdentities,
        "a removal burst must expose bounded tombstone overflow so the selected identity can be verified explicitly");

    delivered.reset();
    for (int index = 0;
         index <= codexui::detail::maximumCoalescedPresentationIdentities;
         ++index) {
        codexui::detail::StateUpdateScope scope;
        scope.affectedSidebarThreadIds.push_back(
            QStringLiteral("sidebar-%1").arg(index));
        scope.sidebarAffected = true;
        scope.hasPresentationChange = true;
        codexui::FrontendSessionFacadeTestAccess::enqueueState(
            session, 1, std::move(scope));
    }
    QCoreApplication::processEvents();
    passed &= expect(delivered && delivered->sidebarAffected
                         && delivered->allSidebarThreadsAffected
                         && delivered->affectedSidebarThreadIds.empty(),
                     "a blocked GUI must bound targeted Sidebar identities and let full-refresh dominance clear them");
    session.shutdown();
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    return testPeerCredentials() && testScopedItemPresentationChanges() && testLifecycleAndDiagnostics()
               && testPreReadyReconnectBound() && testReceiveRejectionPreservesPreciseError()
               && testInboundFrameCapacityTracksSdk()
               && testIncompleteThreadReadIsBounded()
               && testModelCatalogRefresh() && testModelCatalogRefreshFailureIsDiagnosed()
               && testArchivedThreadRefresh()
               && testArchivedThreadRefreshFailureIsTerminal()
               && testArchivedThreadPaginationTruncationIsTerminal()
               && testTurnSteeringSubmission()
               && testOutboundQueue() && testInboundBufferCompaction()
               && testThreadedFacadeMailbox()
               && testFacadeReplaceableControlCoalescing()
               && testImmediateFacadeShutdown()
               && testFacadeGenerationGating() && testFacadeScopeBound()
           ? 0
           : 1;
}
