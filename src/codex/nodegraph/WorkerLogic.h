// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_WORKERLOGIC_H
#define CODEXUI_CODEX_NODEGRAPH_WORKERLOGIC_H

#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/ThreadChannels.h"

#include <cstdint>
#include <optional>
#include <string>

namespace codexui::nodegraph {

// Application logic hosted by the existing SNode.C worker. CodexBridge gives
// it already-decoded messages; it publishes current graph state and wakes Qt.
class WorkerLogic final {
public:
  WorkerLogic(NodeGraph &graph, ThreadChannels &channels) noexcept;
  WorkerLogic(const WorkerLogic &) = delete;
  WorkerLogic &operator=(const WorkerLogic &) = delete;

  [[nodiscard]] ChannelSendStatus apply(DecodedMessage message);

  // A successful transport connection begins a new connection generation.
  // Retry and disconnect events retain that generation so late bridge/provider
  // events can still be recognized as belonging to the current connection.
  [[nodiscard]] ChannelSendStatus transportEvent(std::string state,
                                                 std::string detail = {});

  // CodexBridge supplies its complete current addressing/provider facts. A
  // stale provider generation cannot replace newer provider state.
  [[nodiscard]] ChannelSendStatus
  bridgeState(std::string connectionId, std::string role,
              std::string controllerConnectionId,
              std::uint64_t providerGeneration,
              std::optional<std::string> providerState,
              std::string detail = {});

  [[nodiscard]] ChannelSendStatus connectionSettings(Value::Object settings);

  [[nodiscard]] ChannelSendStatus
  resolveInteraction(const ProtocolRequestId &requestId, bool accepted,
                     std::string error = {});

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

  [[nodiscard]] ChannelSendStatus publish(GraphChange change);
  [[nodiscard]] ChannelSendStatus publishConnection();
  [[nodiscard]] NodeState connectionNodeState() const;
  void clearBridgeState(bool clearProviderGeneration);

  NodeGraph &graph_;
  ThreadChannels &channels_;
  ProtocolUpdater updater_;
  ConnectionCurrentState connection_;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_WORKERLOGIC_H
