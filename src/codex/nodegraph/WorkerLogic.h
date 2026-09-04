// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_WORKERLOGIC_H
#define CODEXUI_CODEX_NODEGRAPH_WORKERLOGIC_H

#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/ThreadChannels.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace codexui::nodegraph {

struct WorkerGenerations final {
  std::uint64_t connection = 0;
  std::uint64_t provider = 0;

  bool operator==(const WorkerGenerations &) const = default;
};

struct WorkerApplyResult final {
  ChannelSendStatus status = ChannelSendStatus::Accepted;
  NodeRef primary;
};

enum class PromptCommandKind : std::uint8_t {
  CreateThread,
  StartTurn,
  SteerTurn,
};

// One concrete command handed to ClientRuntime for one direct CodexBridge
// call. Large newly-authored input is moved, never copied, from the Qt action
// through the worker's bounded pending queue into this command.
struct PromptCommand final {
  PromptCommandKind kind = PromptCommandKind::StartTurn;
  NodeRef localPrompt;
  NodeRef thread;
  std::string clientUserMessageId;
  std::string promptText;
  std::vector<Attachment> attachments;
  Value::Object options;
  std::string expectedTurnId;

  // CreateThread only: attachCreatedThread changes kind to StartTurn and
  // moves turnOptions into options after thread/start returns.
  Value::Object turnOptions;
  std::string requestedName;
  std::string creationCorrelation;
};

struct PromptTransition final {
  ChannelSendStatus status = ChannelSendStatus::Accepted;
  std::optional<PromptCommand> command;
};

// Application logic hosted by the existing SNode.C worker. CodexBridge gives
// it already-decoded messages; it publishes current graph state and wakes Qt.
class WorkerLogic final {
public:
  WorkerLogic(NodeGraph &graph, ThreadChannels &channels) noexcept;
  WorkerLogic(const WorkerLogic &) = delete;
  WorkerLogic &operator=(const WorkerLogic &) = delete;

  [[nodiscard]] ChannelSendStatus apply(DecodedMessage message);
  [[nodiscard]] WorkerApplyResult applyDetailed(DecodedMessage message);
  [[nodiscard]] WorkerGenerations generations() const;

  // A successful transport connection begins a new connection generation.
  // Retry and disconnect events retain that generation so late bridge/provider
  // events can still be recognized as belonging to the current connection.
  [[nodiscard]] ChannelSendStatus transportEvent(std::string state,
                                                 std::string detail = {});

  // CodexBridge supplies its complete current addressing/provider facts. A
  // stale provider generation cannot replace newer provider state.
  [[nodiscard]] ChannelSendStatus bridgeState(
      std::string connectionId, std::string role,
      std::string controllerConnectionId, std::uint64_t providerGeneration,
      std::optional<std::string> providerState, std::string detail = {});

  [[nodiscard]] ChannelSendStatus connectionSettings(Value::Object settings);

  [[nodiscard]] ChannelSendStatus threadHydration(const NodeRef &thread,
                                                  std::string state,
                                                  std::string error = {});
  [[nodiscard]] ChannelSendStatus
  completeThreadHydration(DecodedMessage result, const NodeRef &thread,
                          std::string state, std::string error = {});
  [[nodiscard]] std::vector<NodeRef> activeAgentChildren(const NodeRef &thread);
  [[nodiscard]] ChannelSendStatus showNotice(std::string message);
  [[nodiscard]] ChannelSendStatus selectThread(const NodeRef &thread);

  [[nodiscard]] ChannelSendStatus
  resolveInteraction(const ProtocolRequestId &requestId, bool accepted,
                     std::string error = {});
  [[nodiscard]] ChannelSendStatus resolveInteraction(const NodeRef &interaction,
                                                     bool accepted,
                                                     std::string error = {});
  [[nodiscard]] ChannelSendStatus
  rejectInteractionResponse(const NodeRef &interaction,
                            Value::Object authoredResponse, std::string error);

  // RuntimeAction::CreateThread uses the explicit payload shape
  // {threadStart: Object, turnStart: Object, requestedName: String}.
  // NodeAction::SubmitPrompt payload is the exact extra turn/start options.
  [[nodiscard]] PromptTransition
  admitPrompt(NodeAction action,
              std::optional<std::int64_t> activityAt = std::nullopt,
              std::optional<std::int64_t> admittedAtMs = std::nullopt);
  [[nodiscard]] PromptTransition
  admitFirstPrompt(RuntimeAction action,
                   std::optional<std::int64_t> activityAt = std::nullopt,
                   std::optional<std::int64_t> admittedAtMs = std::nullopt);
  [[nodiscard]] ChannelSendStatus
  attachCreatedThread(PromptCommand &command, std::string threadId,
                      NodeState providerState = {});
  [[nodiscard]] ChannelSendStatus
  completeCreatedThread(DecodedMessage result, PromptCommand &command,
                        std::string threadId, NodeState providerState = {});
  [[nodiscard]] ChannelSendStatus
  markPromptDispatched(const NodeRef &localPrompt,
                       const ProtocolRequestId &requestId);
  [[nodiscard]] PromptTransition
  completePrompt(const NodeRef &localPrompt, bool accepted,
                 std::string error = {},
                 std::optional<std::string> turnId = std::nullopt);
  [[nodiscard]] PromptTransition
  completePromptResult(DecodedMessage result, const NodeRef &localPrompt,
                       bool accepted, std::string error = {},
                       std::optional<std::string> turnId = std::nullopt);
  [[nodiscard]] PromptTransition failPrompt(const NodeRef &localPrompt,
                                            std::string error);
  [[nodiscard]] ChannelSendStatus
  promptMaterialized(const NodeRef &localPrompt);

  // Qt has already cleared the opaque attachment and destroyed its QWidget.
  // Releasing this recovery pin is deliberately revision-neutral.
  [[nodiscard]] ChannelSendStatus acknowledgeUiDetached(NodeRef node);

  [[nodiscard]] ChannelSendStatus sendWorkerStopped(std::string reason);

private:
  struct PendingPrompt final {
    NodeRef localPrompt;
    NodeRef thread;
    std::string clientUserMessageId;
    std::string promptText;
    std::vector<Attachment> attachments;
    Value::Object options;
    Value::Object turnOptions;
    std::string requestedName;
    std::string creationCorrelation;
    bool createsThread = false;
  };

  [[nodiscard]] ChannelSendStatus publish(GraphChange change);
  void forgetRemoved(const GraphChange &change);
  void updateThreadHydration(NodeGraph::WriteAccess &write,
                             const NodeRef &thread, std::string state,
                             std::string error);
  [[nodiscard]] bool attachCreatedThread(NodeGraph::WriteAccess &write,
                                         PromptCommand &command,
                                         std::string threadId,
                                         NodeState providerState,
                                         NodeRef &authoritative);
  [[nodiscard]] std::optional<PromptCommand>
  completePrompt(NodeGraph::WriteAccess &write, const NodeRef &localPrompt,
                 bool accepted, std::string error,
                 std::optional<std::string> turnId);
  [[nodiscard]] PromptTransition
  admit(PendingPrompt pending, std::optional<std::int64_t> activityAt,
        std::optional<std::int64_t> admittedAtMs);
  [[nodiscard]] std::optional<PromptCommand>
  takeNextPrompt(NodeGraph::WriteAccess &write, const NodeRef &thread);
  [[nodiscard]] NodeRef activeTurn(NodeGraph::WriteAccess &write,
                                   const NodeRef &thread) const;
  void resetProviderDerived(NodeGraph::WriteAccess &write,
                            std::string_view reason);
  void advancePromptActivity(NodeGraph::WriteAccess &write,
                             const NodeRef &thread,
                             std::int64_t proposedActivityAt);
  void forgetPrompt(const NodeRef &localPrompt);

  NodeGraph &graph_;
  ThreadChannels &channels_;
  ProtocolUpdater updater_;
  std::uint64_t nextSubmissionId_ = 1;
  std::uint64_t nextNoticeSerial_ = 1;
  std::uint64_t nextSelectionSerial_ = 1;
  std::unordered_map<const Node *, std::deque<PendingPrompt>> promptQueues_;
  std::unordered_map<const Node *, NodeRef> promptInFlight_;
  std::unordered_map<std::string, NodeRef> creatingThreads_;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_WORKERLOGIC_H
