// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/WorkerLogic.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using namespace codexui::nodegraph;

int failures = 0;

void require(bool condition, std::string_view message) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

const Value *field(const std::shared_ptr<const NodeState> &state,
                   std::string_view key) {
  if (!state)
    return nullptr;
  const auto found = state->fields.find(key);
  return found == state->fields.end() ? nullptr : &found->second;
}

bool stringFieldEquals(const std::shared_ptr<const NodeState> &state,
                       std::string_view key, std::string_view expected) {
  const Value *value = field(state, key);
  const std::string *text = value ? value->asString() : nullptr;
  return text && *text == expected;
}

bool unsignedFieldEquals(const std::shared_ptr<const NodeState> &state,
                         std::string_view key, std::uint64_t expected) {
  const Value *value = field(state, key);
  const std::uint64_t *number = value ? value->asUInt64() : nullptr;
  return number && *number == expected;
}

std::optional<GraphChanged> takeGraphChanged(ThreadChannels &channels,
                                             std::uint64_t wakeCount = 1) {
  const EventFd::DrainResult wake = channels.drainWorkerToQtWake();
  if (wake.status != EventFd::DrainStatus::Drained || wake.count != wakeCount)
    return std::nullopt;
  WorkerToQtMessage message;
  if (!channels.tryReceiveForQt(message))
    return std::nullopt;
  GraphChanged *changed = std::get_if<GraphChanged>(&message);
  if (!changed)
    return std::nullopt;
  return std::move(*changed);
}

void protocolUpdatesPublishForUnlockedReads() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  const ChannelSendStatus status = logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("worker-thread")},
                                    {"name", Value("Worker thread")}})}}});
  require(status == ChannelSendStatus::Accepted,
          "protocol graph update is admitted to Qt");

  const std::optional<GraphChanged> changed = takeGraphChanged(channels);
  const bool includesThread =
      changed && std::ranges::find_if(
                     changed->affected, [](const NodeRef &node) {
                       return node->id() ==
                              NodeId{NodeKind::Thread, "worker-thread"};
                     }) != changed->affected.end();
  require(changed && changed->revision == graph.publishedRevision() &&
              !changed->rescanRequired && includesThread,
          "protocol update queues its committed revision and addressed "
          "NodeRef");

  auto qtRead = graph.tryRead();
  require(qtRead.has_value(),
          "Qt can try-read immediately after receiving the graph wake");
  if (!qtRead)
    return;
  const NodeRef thread = qtRead->find({NodeKind::Thread, "worker-thread"});
  require(thread && changed && changed->affected.front() == thread &&
              stringFieldEquals(qtRead->state(thread), "name", "Worker thread"),
          "the post-unlock read sees the complete protocol update");
}

void connectionStateAndGenerationsStayCurrent() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  require(logic.transportEvent("connecting", "dialing bridge") ==
              ChannelSendStatus::Accepted,
          "connecting state is published");
  require(takeGraphChanged(channels).has_value(),
          "connecting state wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(connection && state && state->status == NodeStatus::Pending &&
                stringFieldEquals(state, "transportState", "connecting") &&
                stringFieldEquals(state, "transportDetail", "dialing bridge") &&
                unsignedFieldEquals(state, "connectionGeneration", 0),
            "connection node retains explicit connecting fields");
  }

  require(logic.transportEvent("connected", "unused") ==
              ChannelSendStatus::Accepted,
          "connected state is published");
  require(takeGraphChanged(channels).has_value(),
          "connected state wakes Qt once");
  require(logic.bridgeState("bridge-1", "controller", "bridge-1", 7, "ready",
                            "provider ready") == ChannelSendStatus::Accepted,
          "bridge/provider state is published");
  require(takeGraphChanged(channels).has_value(),
          "bridge/provider state wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(
        state && state->status == NodeStatus::Connected &&
            stringFieldEquals(state, "transportState", "connected") &&
            stringFieldEquals(state, "transportDetail", "") &&
            unsignedFieldEquals(state, "connectionGeneration", 1) &&
            stringFieldEquals(state, "connectionId", "bridge-1") &&
            stringFieldEquals(state, "role", "controller") &&
            stringFieldEquals(state, "controllerConnectionId", "bridge-1") &&
            unsignedFieldEquals(state, "providerGeneration", 7) &&
            stringFieldEquals(state, "providerState", "ready") &&
            stringFieldEquals(state, "providerDetail", "provider ready"),
        "connection node holds current transport and bridge facts");
  }

  require(logic.connectionSettings(
              {{"selected", Value("unix")}, {"tls", Value(false)}}) ==
              ChannelSendStatus::Accepted,
          "connection settings are published");
  require(takeGraphChanged(channels).has_value(),
          "connection settings wake Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    const Value *settings = field(state, "settings");
    const Value::Object *object = settings ? settings->asObject() : nullptr;
    bool selectedUnix = false;
    if (object) {
      const auto selected = object->find("selected");
      selectedUnix = selected != object->end() && selected->second.asString() &&
                     *selected->second.asString() == "unix";
    }
    require(selectedUnix,
            "connection node retains current selectable transport settings");
  }

  require(logic.bridgeState("bridge-1", "observer", "bridge-2", 7,
                            std::nullopt) == ChannelSendStatus::Accepted,
          "controller identity can change without a provider event");
  require(takeGraphChanged(channels).has_value(),
          "controller identity change wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && stringFieldEquals(state, "role", "observer") &&
                stringFieldEquals(state, "controllerConnectionId",
                                  "bridge-2") &&
                stringFieldEquals(state, "providerState", "ready") &&
                stringFieldEquals(state, "providerDetail", "provider ready"),
            "unrelated bridge events preserve same-generation provider facts");
  }

  const std::uint64_t beforeStale = graph.publishedRevision();
  require(logic.bridgeState("bridge-1", "controller", "bridge-1", 6,
                            "disconnected",
                            "stale") == ChannelSendStatus::Accepted,
          "stale provider state is handled without failure");
  require(graph.publishedRevision() == beforeStale &&
              channels.workerToQtSizeApprox() == 0 &&
              channels.drainWorkerToQtWake().status ==
                  EventFd::DrainStatus::Empty,
          "stale provider generation cannot publish or wake Qt");

  require(logic.transportEvent("retrying", "retry scheduled") ==
              ChannelSendStatus::Accepted,
          "retrying state is published");
  require(takeGraphChanged(channels).has_value(),
          "retrying state wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Pending &&
                unsignedFieldEquals(state, "connectionGeneration", 1) &&
                unsignedFieldEquals(state, "providerGeneration", 7) &&
                stringFieldEquals(state, "connectionId", "") &&
                stringFieldEquals(state, "role", "") &&
                stringFieldEquals(state, "controllerConnectionId", "") &&
                stringFieldEquals(state, "providerState", "") &&
                stringFieldEquals(state, "providerDetail", "") &&
                stringFieldEquals(state, "transportDetail", "retry scheduled"),
            "retry clears bridge facts but preserves both generations");
  }

  require(logic.transportEvent("disconnected", "connection lost") ==
              ChannelSendStatus::Accepted,
          "disconnect state is published");
  require(takeGraphChanged(channels).has_value(),
          "disconnect state wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Disconnected &&
                unsignedFieldEquals(state, "connectionGeneration", 1),
            "disconnect preserves the current connection generation");
  }

  require(logic.transportEvent("connected") == ChannelSendStatus::Accepted,
          "reconnection is published");
  require(takeGraphChanged(channels).has_value(), "reconnection wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Connected &&
                unsignedFieldEquals(state, "connectionGeneration", 2) &&
                unsignedFieldEquals(state, "providerGeneration", 0),
            "each successful connection advances generation and resets "
            "provider generation");
  }
}

void stateNeutralMessagesDoNotWakeQt() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  const std::uint64_t before = graph.publishedRevision();
  const ChannelSendStatus status = logic.apply(
      {DecodedMessageKind::ServerNotification, "rawResponse/completed",
       std::nullopt, Value::Object{{"ignored", Value(true)}}});
  WorkerToQtMessage message;
  require(status == ChannelSendStatus::Accepted &&
              graph.publishedRevision() == before &&
              channels.workerToQtSizeApprox() == 0 &&
              channels.drainWorkerToQtWake().status ==
                  EventFd::DrainStatus::Empty &&
              !channels.tryReceiveForQt(message),
          "explicitly state-neutral protocol handling publishes no revision or "
          "wake");
}

void graphNotificationSaturationCoalesces() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  std::size_t admitted = 0;
  bool reachedLimit = false;
  bool statusesAccepted = true;
  for (std::size_t attempt = 0; attempt <= ThreadChannels::WorkerToQtCapacity;
       ++attempt) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "fill-" + std::to_string(attempt),
                    {}};
    const ChannelSendStatus status = channels.sendUiEffect(effect);
    if (status == ChannelSendStatus::QueueFull) {
      reachedLimit = true;
      break;
    }
    statusesAccepted =
        statusesAccepted && status == ChannelSendStatus::Accepted;
    ++admitted;
  }
  require(statusesAccepted && reachedLimit &&
              admitted + 1 == ThreadChannels::WorkerToQtCapacity,
          "ordinary worker messages fill to the reserved queue slot");

  const ChannelSendStatus status = logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("coalesced-thread")}})}}});
  require(
      status == ChannelSendStatus::CoalescedRescan && messageAdmitted(status) &&
          !wakeFailed(status) && channels.rescanPending() &&
          channels.workerToQtSizeApprox() == admitted,
      "committed graph update coalesces visibly when its queue is saturated");

  const std::uint64_t revision = graph.publishedRevision();
  const std::optional<GraphChanged> rescan =
      takeGraphChanged(channels, admitted + 1);
  require(rescan && rescan->rescanRequired && rescan->revision == revision &&
              !channels.rescanPending(),
          "saturated graph update wakes Qt with the latest rescan revision");
  auto read = graph.tryRead();
  require(read && read->find({NodeKind::Thread, "coalesced-thread"}),
          "coalescing never loses the already-committed current graph state");
}

void uiDetachAcknowledgementIsRevisionNeutral() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  NodeRef node;
  {
    auto write = graph.write();
    node = write.upsert({NodeKind::Item, "retired-item"});
    static_cast<void>(write.finish());
  }
  GraphChange removal;
  {
    auto write = graph.write();
    write.remove(node);
    removal = write.finish();
  }
  const std::uint64_t removalRevision = graph.publishedRevision();
  std::weak_ptr<Node> lifetime = node;
  NodeRef acknowledgement = removal.removed.front();
  removal.affected.clear();
  removal.removed.clear();
  node.reset();

  {
    auto read = graph.tryRead();
    require(read && read->retiredNodes().size() == 1 && !lifetime.expired(),
            "removed node remains pinned until Qt detaches it");
  }
  const ChannelSendStatus status =
      logic.acknowledgeUiDetached(std::move(acknowledgement));
  WorkerToQtMessage message;
  require(status == ChannelSendStatus::Accepted &&
              graph.publishedRevision() == removalRevision &&
              channels.drainWorkerToQtWake().status ==
                  EventFd::DrainStatus::Empty &&
              !channels.tryReceiveForQt(message),
          "UiDetached releases retirement without a revision or notification");
  {
    auto read = graph.tryRead();
    require(read && read->retiredNodes().empty(),
            "UiDetached removes the graph retirement pin");
  }
  require(lifetime.expired(),
          "retired node dies after the acknowledgement releases its last pin");
}

void reverseInteractionResolutionUpdatesTheGraph() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerRequest, "item/commandExecution/requestApproval",
       ProtocolRequestId("approval-1"),
       {{"threadId", Value("thread-1")},
        {"turnId", Value("turn-1")},
        {"itemId", Value("item-1")}}}));
  require(takeGraphChanged(channels).has_value(),
          "reverse interaction creation wakes Qt");

  require(logic.resolveInteraction(ProtocolRequestId("approval-1"), true) ==
              ChannelSendStatus::Accepted,
          "accepted reverse interaction is resolved");
  const std::optional<GraphChanged> removed = takeGraphChanged(channels);
  require(removed && removed->removed.size() == 1 &&
              removed->removed.front()->id().kind == NodeKind::Interaction,
          "accepted response removes the pending interaction atomically");
  if (removed && !removed->removed.empty())
    static_cast<void>(
        logic.acknowledgeUiDetached(removed->removed.front()));

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerRequest, "item/fileChange/requestApproval",
       ProtocolRequestId("approval-2"),
       {{"threadId", Value("thread-1")}}}));
  require(takeGraphChanged(channels).has_value(),
          "second reverse interaction creation wakes Qt");
  require(logic.resolveInteraction(ProtocolRequestId("approval-2"), false,
                                   "transport rejected response") ==
              ChannelSendStatus::Accepted,
          "rejected response failure is recorded");
  require(takeGraphChanged(channels).has_value(),
          "rejected response failure wakes Qt");
  auto read = graph.tryRead();
  const NodeRef failed =
      read ? read->find({NodeKind::Interaction,
                         ProtocolRequestId("approval-2").canonical()})
           : NodeRef{};
  const auto state = read && failed ? read->state(failed) : nullptr;
  require(state && state->status == NodeStatus::Failed &&
              stringFieldEquals(state, "error", "transport rejected response"),
          "failed response remains visible with its concrete error");
}

void workerStoppedDeliveryIsExplicit() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  require(logic.sendWorkerStopped("normal shutdown") ==
              ChannelSendStatus::Accepted,
          "WorkerStopped is admitted normally");
  const EventFd::DrainResult wake = channels.drainWorkerToQtWake();
  WorkerToQtMessage message;
  require(wake.status == EventFd::DrainStatus::Drained && wake.count == 1 &&
              channels.tryReceiveForQt(message) &&
              std::holds_alternative<WorkerStopped>(message) &&
              std::get<WorkerStopped>(message).reason == "normal shutdown",
          "WorkerStopped arrives as the typed terminal message");

  ThreadChannels closedChannels;
  WorkerLogic closedLogic(graph, closedChannels);
  closedChannels.close();
  const ChannelSendStatus failedWake =
      closedLogic.sendWorkerStopped("wake already closed");
  WorkerToQtMessage admittedMessage;
  require(
      failedWake == ChannelSendStatus::AcceptedWakeFailed &&
          messageAdmitted(failedWake) && wakeFailed(failedWake) &&
          closedChannels.tryReceiveForQt(admittedMessage) &&
          std::holds_alternative<WorkerStopped>(admittedMessage) &&
          std::get<WorkerStopped>(admittedMessage).reason ==
              "wake already closed",
      "WorkerStopped reports an admitted-but-failed wake without hiding it");
}

} // namespace

int main() {
  protocolUpdatesPublishForUnlockedReads();
  connectionStateAndGenerationsStayCurrent();
  stateNeutralMessagesDoNotWakeQt();
  graphNotificationSaturationCoalesces();
  uiDetachAcknowledgementIsRevisionNeutral();
  reverseInteractionResolutionUpdatesTheGraph();
  workerStoppedDeliveryIsExplicit();

  if (failures != 0) {
    std::cerr << failures << " worker logic assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "codexui worker logic tests passed\n";
  return EXIT_SUCCESS;
}
