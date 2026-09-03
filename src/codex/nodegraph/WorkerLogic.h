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
  [[nodiscard]] WorkerGenerations generations() const noexcept;

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

  [[nodiscard]] ChannelSendStatus
  resolveInteraction(const ProtocolRequestId &requestId, bool accepted,
                     std::string error = {});
  [[nodiscard]] ChannelSendStatus resolveInteraction(const NodeRef &interaction,
                                                     bool accepted,
                                                     std::string error = {});

  // RuntimeAction::CreateThread uses the explicit payload shape
  // {threadStart: Object, turnStart: Object, requestedName: String}.
  // NodeAction::SubmitPrompt payload is the exact extra turn/start options.
  [[nodiscard]] PromptTransition admitPrompt(NodeAction action);
  [[nodiscard]] PromptTransition admitFirstPrompt(RuntimeAction action);
  [[nodiscard]] ChannelSendStatus
  attachCreatedThread(PromptCommand &command, std::string threadId,
                      NodeState providerState = {});
  [[nodiscard]] ChannelSendStatus
  markPromptDispatched(const NodeRef &localPrompt,
                       const ProtocolRequestId &requestId);
  [[nodiscard]] PromptTransition
  completePrompt(const NodeRef &localPrompt, bool accepted,
                 std::string error = {},
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
  struct ConnectionCurrentState final {
    std::string transportState;
    std::string transportDetail;
    std::uint64_t connectionGeneration = 0;
    std::string connectionId;
    std::string role;
    std::string controllerConnectionId;
    std::uint64_t providerGeneration = 0;
    std::string providerState;
    std::string providerDetail;
    Value::Object settings;
  };

  struct PendingPrompt final {
    NodeRef localPrompt;
    NodeRef thread;
    std::string clientUserMessageId;
    std::string promptText;
    std::vector<Attachment> attachments;
    Value::Object options;
    Value::Object turnOptions;
    std::string requestedName;
    bool createsThread = false;
  };

  [[nodiscard]] ChannelSendStatus publish(GraphChange change);
  [[nodiscard]] ChannelSendStatus publishConnection(bool resetProvider,
                                                    std::string resetReason);
  [[nodiscard]] NodeState connectionNodeState() const;
  [[nodiscard]] PromptTransition admit(PendingPrompt pending);
  [[nodiscard]] std::optional<PromptCommand>
  takeNextPrompt(NodeGraph::WriteAccess &write, const NodeRef &thread);
  [[nodiscard]] NodeRef activeTurn(NodeGraph::WriteAccess &write,
                                   const NodeRef &thread) const;
  void resetProviderDerived(NodeGraph::WriteAccess &write,
                            std::string_view reason);
  void forgetPrompt(const NodeRef &localPrompt);
  void clearBridgeState(bool clearProviderGeneration);

  NodeGraph &graph_;
  ThreadChannels &channels_;
  ProtocolUpdater updater_;
  ConnectionCurrentState connection_;
  std::uint64_t nextSubmissionId_ = 1;
  std::unordered_map<const Node *, std::deque<PendingPrompt>> promptQueues_;
  std::unordered_map<const Node *, NodeRef> promptInFlight_;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_WORKERLOGIC_H
