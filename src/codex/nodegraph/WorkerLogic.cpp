// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/WorkerLogic.h"

#include <array>
#include <utility>

namespace codexui::nodegraph {

WorkerLogic::WorkerLogic(NodeGraph &graph, ThreadChannels &channels) noexcept
    : graph_(graph), channels_(channels), updater_(graph) {}

ChannelSendStatus WorkerLogic::apply(DecodedMessage message) {
  ApplyResult result = updater_.apply(std::move(message));
  return publish(std::move(result.change));
}

ChannelSendStatus WorkerLogic::transportEvent(std::string state,
                                              std::string detail) {
  connection_.transportState = std::move(state);
  if (connection_.transportState == "connected") {
    ++connection_.connectionGeneration;
    connection_.transportDetail.clear();
    clearBridgeState(true);
  } else {
    connection_.transportDetail = std::move(detail);
    if (connection_.transportState == "connecting" ||
        connection_.transportState == "retrying" ||
        connection_.transportState == "disconnected" ||
        connection_.transportState == "failure") {
      clearBridgeState(false);
    }
  }
  return publishConnection();
}

ChannelSendStatus WorkerLogic::bridgeState(std::string connectionId,
                                           std::string role,
                                           std::string controllerConnectionId,
                                           std::uint64_t providerGeneration,
                                           std::optional<std::string> providerState,
                                           std::string detail) {
  if (providerState && providerGeneration < connection_.providerGeneration)
    return ChannelSendStatus::Accepted;

  connection_.connectionId = std::move(connectionId);
  connection_.role = std::move(role);
  connection_.controllerConnectionId = std::move(controllerConnectionId);
  if (providerState && providerGeneration >= connection_.providerGeneration) {
    connection_.providerGeneration = providerGeneration;
    connection_.providerState = std::move(*providerState);
    connection_.providerDetail = std::move(detail);
  }
  return publishConnection();
}

ChannelSendStatus WorkerLogic::connectionSettings(Value::Object settings) {
  connection_.settings = std::move(settings);
  return publishConnection();
}

ChannelSendStatus
WorkerLogic::resolveInteraction(const ProtocolRequestId &requestId,
                                bool accepted, std::string error) {
  return publish(
      updater_.resolveInteraction(requestId, accepted, std::move(error)));
}

ChannelSendStatus WorkerLogic::acknowledgeUiDetached(NodeRef node) {
  GraphChange change;
  {
    const std::array<NodeRef, 1> detached{std::move(node)};
    auto write = graph_.write();
    write.releaseRetired(detached);
    change = write.finish();
  }
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::sendWorkerStopped(std::string reason) {
  WorkerStopped stopped{std::move(reason)};
  return channels_.sendWorkerStopped(stopped);
}

ChannelSendStatus WorkerLogic::publish(GraphChange change) {
  return channels_.sendGraphChanged(std::move(change));
}

ChannelSendStatus WorkerLogic::publishConnection() {
  NodeState state = connectionNodeState();
  GraphChange change;
  {
    auto write = graph_.write();
    NodeRef connection =
        write.upsert({NodeKind::Connection, "connection"}, state);
    write.replaceState(connection, std::move(state));
    change = write.finish();
  }
  return publish(std::move(change));
}

NodeState WorkerLogic::connectionNodeState() const {
  NodeStatus status = NodeStatus::Unknown;
  if (connection_.transportState == "connected")
    status = NodeStatus::Connected;
  else if (connection_.transportState == "connecting" ||
           connection_.transportState == "retrying")
    status = NodeStatus::Pending;
  else if (connection_.transportState == "disconnected" ||
           connection_.transportState == "failure")
    status = NodeStatus::Disconnected;

  return NodeState{
      status,
      {{"transportState", connection_.transportState},
       {"transportDetail", connection_.transportDetail},
       {"connectionGeneration", connection_.connectionGeneration},
       {"connectionId", connection_.connectionId},
       {"role", connection_.role},
       {"controllerConnectionId", connection_.controllerConnectionId},
       {"providerGeneration", connection_.providerGeneration},
       {"providerState", connection_.providerState},
       {"providerDetail", connection_.providerDetail},
       {"settings", connection_.settings}}};
}

void WorkerLogic::clearBridgeState(bool clearProviderGeneration) {
  connection_.connectionId.clear();
  connection_.role.clear();
  connection_.controllerConnectionId.clear();
  if (clearProviderGeneration)
    connection_.providerGeneration = 0;
  connection_.providerState.clear();
  connection_.providerDetail.clear();
}

} // namespace codexui::nodegraph
