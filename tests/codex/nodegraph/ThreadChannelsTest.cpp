// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ThreadChannels.h"
#include "codex/nodegraph/EventFd.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using codexui::nodegraph::Attachment;
using codexui::nodegraph::ChannelSendStatus;
using codexui::nodegraph::deliveryGuaranteed;
using codexui::nodegraph::EventFd;
using codexui::nodegraph::GraphChange;
using codexui::nodegraph::GraphChanged;
using codexui::nodegraph::messageAdmitted;
using codexui::nodegraph::NodeAction;
using codexui::nodegraph::NodeActionKind;
using codexui::nodegraph::NodeGraph;
using codexui::nodegraph::NodeId;
using codexui::nodegraph::NodeKind;
using codexui::nodegraph::NodeRef;
using codexui::nodegraph::NodeState;
using codexui::nodegraph::NodeStatus;
using codexui::nodegraph::QtToWorkerMessage;
using codexui::nodegraph::RuntimeAction;
using codexui::nodegraph::RuntimeActionKind;
using codexui::nodegraph::ShutdownRequest;
using codexui::nodegraph::ThreadChannels;
using codexui::nodegraph::UiEffect;
using codexui::nodegraph::UiEffectKind;
using codexui::nodegraph::Value;
using codexui::nodegraph::wakeFailed;
using codexui::nodegraph::WorkerStopped;
using codexui::nodegraph::WorkerToQtMessage;

bool expect(bool condition, std::string_view message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

NodeId id(NodeKind kind, std::string canonical) {
  return NodeId{kind, std::move(canonical)};
}

NodeRef insertNode(NodeGraph &graph, NodeId nodeId) {
  auto write = graph.write();
  NodeRef node = write.upsert(std::move(nodeId));
  static_cast<void>(write.finish());
  return node;
}

NodeAction richPromptAction(const NodeRef &target) {
  NodeAction action;
  action.target = target;
  action.kind = NodeActionKind::SubmitPrompt;
  action.promptText = std::string(4096, 'p');
  action.attachments = {
      Attachment{"/tmp/image.bin", "image.bin", "application/octet-stream",
                 std::vector<std::uint8_t>{0, 1, 2, 3, 127, 128, 254, 255}},
      Attachment{"/tmp/reference.txt", "reference.txt", "text/plain",
                 std::nullopt},
  };
  action.payload = Value::Object{
      {"draft", "draft-17"},
      {"metadata", Value::Object{{"source", "composer"}, {"version", 3}}},
  };
  action.correlation = "prompt-correlation-17";
  return action;
}

bool hasRequiredFlags(int descriptor) {
  const int statusFlags = ::fcntl(descriptor, F_GETFL);
  const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
  return statusFlags >= 0 && (statusFlags & O_NONBLOCK) != 0 &&
         descriptorFlags >= 0 && (descriptorFlags & FD_CLOEXEC) != 0;
}

bool testEventFd() {
  static_assert(!std::is_copy_constructible_v<EventFd>);
  static_assert(!std::is_copy_assignable_v<EventFd>);
  static_assert(std::is_nothrow_move_constructible_v<EventFd>);
  static_assert(std::is_nothrow_move_assignable_v<EventFd>);

  EventFd wake;
  bool passed = true;
  passed &= expect(wake.valid() && wake.creationError() == 0,
                   "eventfd construction succeeds without an error");
  if (!wake.valid())
    return false;

  const int originalDescriptor = wake.descriptor();
  passed &= expect(hasRequiredFlags(originalDescriptor),
                   "eventfd is non-blocking and close-on-exec");

  const EventFd::DrainResult initiallyEmpty = wake.drain();
  passed &=
      expect(initiallyEmpty.status == EventFd::DrainStatus::Empty &&
                 initiallyEmpty.count == 0 && initiallyEmpty.errorNumber == 0 &&
                 initiallyEmpty.accepted(),
             "empty eventfd drain is non-blocking and successful");

  const EventFd::NotifyResult first = wake.notify();
  const EventFd::NotifyResult second = wake.notify();
  const EventFd::NotifyResult third = wake.notify();
  passed &=
      expect(first.status == EventFd::NotifyStatus::Notified &&
                 second.status == EventFd::NotifyStatus::Notified &&
                 third.status == EventFd::NotifyStatus::Notified &&
                 first.accepted() && second.accepted() && third.accepted(),
             "successive eventfd notifications are admitted");
  const EventFd::DrainResult accumulated = wake.drain();
  passed &= expect(accumulated.status == EventFd::DrainStatus::Drained &&
                       accumulated.count == 3 && accumulated.errorNumber == 0 &&
                       accumulated.accepted(),
                   "one eventfd read drains the accumulated wake count");
  passed &= expect(wake.drain().status == EventFd::DrainStatus::Empty,
                   "drained eventfd immediately reports empty");

  EventFd moved(std::move(wake));
  passed &= expect(!wake.valid() && moved.valid() &&
                       moved.descriptor() == originalDescriptor,
                   "eventfd move construction transfers its one descriptor");
  passed &= expect(
      wake.notify() ==
              EventFd::NotifyResult{EventFd::NotifyStatus::Closed, EBADF} &&
          wake.drain() ==
              EventFd::DrainResult{EventFd::DrainStatus::Closed, 0, EBADF},
      "a moved-from eventfd reports closed operations");

  EventFd assigned;
  const int replacedDescriptor = assigned.descriptor();
  assigned = std::move(moved);
  errno = 0;
  const int replacedState = ::fcntl(replacedDescriptor, F_GETFD);
  const int replacedError = errno;
  passed &= expect(!moved.valid() && assigned.valid() &&
                       assigned.descriptor() == originalDescriptor &&
                       replacedState == -1 && replacedError == EBADF,
                   "eventfd move assignment closes the replaced descriptor");

  assigned.close();
  assigned.close();
  passed &= expect(
      !assigned.valid() &&
          assigned.notify() ==
              EventFd::NotifyResult{EventFd::NotifyStatus::Closed, EBADF} &&
          assigned.drain() ==
              EventFd::DrainResult{EventFd::DrainStatus::Closed, 0, EBADF},
      "closing an eventfd is idempotent and explicitly reported");
  return passed;
}

bool testDescriptorsAndVariantOrder() {
  ThreadChannels channels;
  bool passed = true;
  const int workerToQt = channels.workerToQtEventFd();
  const int qtToWorker = channels.qtToWorkerEventFd();
  passed &=
      expect(channels.valid() && workerToQt >= 0 && qtToWorker >= 0 &&
                 workerToQt != qtToWorker &&
                 workerToQt == channels.workerToQtEventFd() &&
                 qtToWorker == channels.qtToWorkerEventFd(),
             "thread channels expose exactly two stable distinct eventfds");
  passed &= expect(hasRequiredFlags(workerToQt) && hasRequiredFlags(qtToWorker),
                   "both channel eventfds have the required Linux flags");

  NodeGraph graph;
  GraphChange graphChange;
  NodeRef target;
  {
    auto write = graph.write();
    target = write.upsert(
        id(NodeKind::Thread, "thread/order"),
        NodeState{NodeStatus::Pending, {{"title", "Ordered thread"}}});
    graphChange = write.finish();
  }
  const GraphChanged expectedGraphChanged{
      graphChange.revision, graphChange.affected, graphChange.removed, false};
  passed &= expect(channels.sendGraphChanged(std::move(graphChange)) ==
                       ChannelSendStatus::Accepted,
                   "a graph change is admitted worker-to-Qt");

  UiEffect effect{
      UiEffectKind::ShowNotice, target, "focus", {{"reason", "new-thread"}}};
  const UiEffect expectedEffect = effect;
  passed &= expect(channels.sendUiEffect(effect) == ChannelSendStatus::Accepted,
                   "a UI effect is admitted worker-to-Qt");
  WorkerStopped stopped{"normal stop"};
  const WorkerStopped expectedStopped = stopped;
  passed &=
      expect(channels.sendWorkerStopped(stopped) == ChannelSendStatus::Accepted,
             "worker termination is admitted worker-to-Qt");

  const EventFd::DrainResult workerWake = channels.drainWorkerToQtWake();
  passed &= expect(workerWake.status == EventFd::DrainStatus::Drained &&
                       workerWake.count == 3,
                   "worker-to-Qt eventfd accumulates one wake per admission");

  WorkerToQtMessage workerMessage;
  passed &=
      expect(channels.tryReceiveForQt(workerMessage) &&
                 std::holds_alternative<GraphChanged>(workerMessage) &&
                 std::get<GraphChanged>(workerMessage) == expectedGraphChanged,
             "GraphChanged remains first in worker-to-Qt FIFO order");
  passed &= expect(channels.tryReceiveForQt(workerMessage) &&
                       std::holds_alternative<UiEffect>(workerMessage) &&
                       std::get<UiEffect>(workerMessage) == expectedEffect,
                   "UiEffect remains second in worker-to-Qt FIFO order");
  passed &=
      expect(channels.tryReceiveForQt(workerMessage) &&
                 std::holds_alternative<WorkerStopped>(workerMessage) &&
                 std::get<WorkerStopped>(workerMessage) == expectedStopped,
             "WorkerStopped remains third in worker-to-Qt FIFO order");
  passed &= expect(!channels.tryReceiveForQt(workerMessage),
                   "worker-to-Qt queue is empty after ordered drain");

  NodeAction nodeAction = richPromptAction(target);
  const NodeAction expectedNodeAction = nodeAction;
  const char *const promptStorage = nodeAction.promptText.data();
  const std::uint8_t *const attachmentStorage =
      nodeAction.attachments.front().bytes->data();
  passed &=
      expect(channels.sendNodeAction(nodeAction) == ChannelSendStatus::Accepted,
             "a node action is admitted Qt-to-worker");
  passed &=
      expect(!nodeAction.target && nodeAction.promptText.empty() &&
                 nodeAction.attachments.empty() && nodeAction.payload.empty() &&
                 nodeAction.correlation.empty(),
             "admission moves prompt and attachment ownership from Qt");

  RuntimeAction runtimeAction{
      RuntimeActionKind::Reconnect, {{"provider", "local"}}, "runtime-order"};
  const RuntimeAction expectedRuntimeAction = runtimeAction;
  passed &= expect(channels.sendRuntimeAction(runtimeAction) ==
                       ChannelSendStatus::Accepted,
                   "a runtime action is admitted Qt-to-worker");
  ShutdownRequest shutdown;
  passed &=
      expect(channels.sendShutdown(shutdown) == ChannelSendStatus::Accepted,
             "shutdown is admitted Qt-to-worker");

  const EventFd::DrainResult qtWake = channels.drainQtToWorkerWake();
  passed &= expect(qtWake.status == EventFd::DrainStatus::Drained &&
                       qtWake.count == 3,
                   "Qt-to-worker eventfd accumulates one wake per admission");

  QtToWorkerMessage qtMessage;
  const bool receivedNodeAction = channels.tryReceiveForWorker(qtMessage);
  const NodeAction *receivedAction =
      receivedNodeAction ? std::get_if<NodeAction>(&qtMessage) : nullptr;
  passed &= expect(
      receivedAction && *receivedAction == expectedNodeAction &&
          receivedAction->promptText.data() == promptStorage &&
          receivedAction->attachments.front().bytes->data() ==
              attachmentStorage,
      "NodeAction arrives first with moved prompt and attachment storage");
  passed &=
      expect(channels.tryReceiveForWorker(qtMessage) &&
                 std::holds_alternative<RuntimeAction>(qtMessage) &&
                 std::get<RuntimeAction>(qtMessage) == expectedRuntimeAction,
             "RuntimeAction remains second in Qt-to-worker FIFO order");
  passed &= expect(channels.tryReceiveForWorker(qtMessage) &&
                       std::holds_alternative<ShutdownRequest>(qtMessage),
                   "ShutdownRequest remains third in Qt-to-worker FIFO order");
  passed &= expect(!channels.tryReceiveForWorker(qtMessage),
                   "Qt-to-worker queue is empty after ordered drain");
  return passed;
}

bool testQtToWorkerBackpressure() {
  ThreadChannels channels;
  NodeGraph graph;
  const NodeRef target =
      insertNode(graph, id(NodeKind::Thread, "thread/backpressure"));

  bool passed = true;
  std::size_t ordinaryAdmissions = 0;
  bool reachedOrdinaryLimit = false;
  bool allOrdinaryAdmissionsAccepted = true;
  for (std::size_t attempt = 0; attempt <= ThreadChannels::QtToWorkerCapacity;
       ++attempt) {
    RuntimeAction filler{RuntimeActionKind::RefreshThreads,
                         {},
                         "runtime-fill-" + std::to_string(attempt)};
    const ChannelSendStatus status = channels.sendRuntimeAction(filler);
    if (status == ChannelSendStatus::QueueFull) {
      reachedOrdinaryLimit = true;
      break;
    }
    allOrdinaryAdmissionsAccepted =
        allOrdinaryAdmissionsAccepted && status == ChannelSendStatus::Accepted;
    ++ordinaryAdmissions;
  }
  passed &=
      expect(allOrdinaryAdmissionsAccepted && reachedOrdinaryLimit &&
                 ordinaryAdmissions + 1 == ThreadChannels::QtToWorkerCapacity &&
                 channels.qtToWorkerSizeApprox() == ordinaryAdmissions,
             "ordinary Qt actions wake the worker and stop at the reserved "
             "shutdown slot");

  NodeAction rejected = richPromptAction(target);
  const NodeAction expectedRejected = rejected;
  const char *const rejectedPromptStorage = rejected.promptText.data();
  const std::uint8_t *const rejectedAttachmentStorage =
      rejected.attachments.front().bytes->data();
  const ChannelSendStatus rejectedStatus = channels.sendNodeAction(rejected);
  passed &= expect(
      rejectedStatus == ChannelSendStatus::QueueFull &&
          !messageAdmitted(rejectedStatus) && !wakeFailed(rejectedStatus) &&
          rejected == expectedRejected &&
          rejected.promptText.data() == rejectedPromptStorage &&
          rejected.attachments.front().bytes->data() ==
              rejectedAttachmentStorage,
      "a saturated mailbox leaves rejected user input byte-for-byte owned");

  ShutdownRequest shutdown;
  const ChannelSendStatus shutdownStatus = channels.sendShutdown(shutdown);
  passed &= expect(shutdownStatus == ChannelSendStatus::Accepted &&
                       channels.qtToWorkerSizeApprox() ==
                           ThreadChannels::QtToWorkerCapacity,
                   "the reserved slot still admits shutdown exactly once");

  const EventFd::DrainResult wake = channels.drainQtToWorkerWake();
  passed &= expect(wake.status == EventFd::DrainStatus::Drained &&
                       wake.count == ordinaryAdmissions + 1,
                   "rejected actions add no eventfd wake");

  QtToWorkerMessage message;
  bool fifoOrder = true;
  for (std::size_t index = 0; index < ordinaryAdmissions; ++index) {
    const bool received = channels.tryReceiveForWorker(message);
    const RuntimeAction *runtime =
        received ? std::get_if<RuntimeAction>(&message) : nullptr;
    fifoOrder = fifoOrder && runtime &&
                runtime->correlation == "runtime-fill-" + std::to_string(index);
  }
  passed &=
      expect(fifoOrder, "admitted ordinary Qt actions preserve FIFO order");
  passed &= expect(channels.tryReceiveForWorker(message) &&
                       std::holds_alternative<ShutdownRequest>(message) &&
                       !channels.tryReceiveForWorker(message),
                   "shutdown follows all previously admitted ordinary actions");
  return passed;
}

bool testGraphCoalescingAndRetiredLifetime() {
  ThreadChannels channels;
  bool passed = true;

  std::size_t ordinaryAdmissions = 0;
  bool reachedOrdinaryLimit = false;
  bool allOrdinaryAdmissionsAccepted = true;
  for (std::size_t attempt = 0; attempt <= ThreadChannels::WorkerToQtCapacity;
       ++attempt) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "ui-fill-" + std::to_string(attempt),
                    {}};
    const ChannelSendStatus status = channels.sendUiEffect(effect);
    if (status == ChannelSendStatus::QueueFull) {
      reachedOrdinaryLimit = true;
      break;
    }
    allOrdinaryAdmissionsAccepted =
        allOrdinaryAdmissionsAccepted && status == ChannelSendStatus::Accepted;
    ++ordinaryAdmissions;
  }
  passed &=
      expect(allOrdinaryAdmissionsAccepted && reachedOrdinaryLimit &&
                 ordinaryAdmissions + ThreadChannels::WorkerToQtReservedSlots ==
                     ThreadChannels::WorkerToQtCapacity &&
                 channels.workerToQtSizeApprox() == ordinaryAdmissions,
             "ordinary worker notifications preserve critical and terminal "
             "slots");

  UiEffect selection{
      UiEffectKind::SelectThread, std::nullopt, "critical selection", {}};
  passed &= expect(
      channels.sendUiEffect(selection) == ChannelSendStatus::Accepted &&
          channels.workerToQtSizeApprox() == ordinaryAdmissions + 1,
      "critical selection uses its reserved slot under ordinary saturation");

  WorkerStopped stopped{"worker finished while Qt was saturated"};
  passed &= expect(
      channels.sendWorkerStopped(stopped) == ChannelSendStatus::Accepted &&
          channels.workerToQtSizeApprox() == ThreadChannels::WorkerToQtCapacity,
      "the reserved slot still admits WorkerStopped");

  NodeGraph graph;
  NodeRef affected;
  NodeRef removed;
  {
    auto write = graph.write();
    affected = write.upsert(id(NodeKind::Thread, "thread/coalesced"));
    removed = write.upsert(id(NodeKind::Item, "item/removed"));
    static_cast<void>(write.finish());
  }

  GraphChange change;
  {
    auto write = graph.write();
    write.setField(affected, "title", "latest title");
    write.remove(removed);
    change = write.finish();
  }
  const std::uint64_t latestRevision = change.revision;
  passed &= expect(
      change.affected.size() == 1 && change.affected.front() == affected &&
          change.removed.size() == 1 && change.removed.front() == removed,
      "saturated GraphChange names affected and removed NodeRefs");

  std::weak_ptr<codexui::nodegraph::Node> removedLifetime = removed;
  const ChannelSendStatus coalesced =
      channels.sendGraphChanged(std::move(change));
  removed.reset();
  passed &= expect(
      coalesced == ChannelSendStatus::CoalescedRescan &&
          messageAdmitted(coalesced) && !wakeFailed(coalesced) &&
          channels.rescanPending() &&
          channels.workerToQtSizeApprox() == ThreadChannels::WorkerToQtCapacity,
      "full worker mailbox coalesces GraphChanged into an explicit rescan");

  const EventFd::DrainResult wake = channels.drainWorkerToQtWake();
  passed &=
      expect(wake.status == EventFd::DrainStatus::Drained &&
                 wake.count == ordinaryAdmissions + 3,
             "coalesced rescan still wakes Qt without queue payload copies");

  WorkerToQtMessage message;
  passed &=
      expect(channels.tryReceiveForQt(message) &&
                 std::holds_alternative<GraphChanged>(message) &&
                 std::get<GraphChanged>(message).revision == latestRevision &&
                 std::get<GraphChanged>(message).affected.empty() &&
                 std::get<GraphChanged>(message).removed.empty() &&
                 std::get<GraphChanged>(message).rescanRequired &&
                 !channels.rescanPending(),
             "Qt receives the latest synthesized rescan before stale queued "
             "notifications");
  bool fifoOrder = true;
  for (std::size_t index = 0; index < ordinaryAdmissions; ++index) {
    const bool received = channels.tryReceiveForQt(message);
    const UiEffect *effect =
        received ? std::get_if<UiEffect>(&message) : nullptr;
    fifoOrder = fifoOrder && effect &&
                effect->text == "ui-fill-" + std::to_string(index);
  }
  passed &=
      expect(fifoOrder,
             "queued worker notifications preserve FIFO order after rescan");
  passed &=
      expect(channels.tryReceiveForQt(message) &&
                 std::holds_alternative<UiEffect>(message) &&
                 std::get<UiEffect>(message).kind == UiEffectKind::SelectThread,
             "critical selection follows ordinary notifications");
  passed &= expect(channels.tryReceiveForQt(message) &&
                       std::holds_alternative<WorkerStopped>(message) &&
                       std::get<WorkerStopped>(message).reason ==
                           "worker finished while Qt was saturated",
                   "WorkerStopped follows ordinary notifications");
  passed &= expect(!channels.tryReceiveForQt(message),
                   "rescan is synthesized only once");

  NodeRef retired;
  {
    auto read = graph.tryRead();
    if (read && read->retiredNodes().size() == 1)
      retired = read->retiredNodes().front();
    passed &=
        expect(read.has_value() && retired && read->removed(retired) &&
                   retired->id() == id(NodeKind::Item, "item/removed") &&
                   !removedLifetime.expired(),
               "removed node stays reachable after its notification coalesces");
  }

  NodeAction detached;
  detached.target = retired;
  detached.kind = NodeActionKind::UiDetached;
  detached.correlation = "detach-item/removed";
  passed &=
      expect(channels.sendNodeAction(detached) == ChannelSendStatus::Accepted &&
                 !detached.target,
             "Qt acknowledges detachment with the stable retired NodeRef");
  retired.reset();

  const EventFd::DrainResult detachWake = channels.drainQtToWorkerWake();
  passed &= expect(detachWake.status == EventFd::DrainStatus::Drained &&
                       detachWake.count == 1,
                   "UiDetached admission wakes the worker once");
  QtToWorkerMessage detachedMessage;
  const bool receivedDetached = channels.tryReceiveForWorker(detachedMessage);
  NodeAction *detachedAction =
      receivedDetached ? std::get_if<NodeAction>(&detachedMessage) : nullptr;
  NodeRef acknowledged = detachedAction ? detachedAction->target : NodeRef{};
  passed &= expect(detachedAction &&
                       detachedAction->kind == NodeActionKind::UiDetached &&
                       acknowledged && !removedLifetime.expired(),
                   "worker receives the removal pin before retirement release");

  {
    const std::array<NodeRef, 1> released{acknowledged};
    auto write = graph.write();
    write.releaseRetired(released);
    static_cast<void>(write.finish());
  }
  passed &=
      expect(!removedLifetime.expired(),
             "executing UiDetached keeps its NodeRef alive during release");
  acknowledged.reset();
  detachedMessage = ShutdownRequest{};
  passed &=
      expect(removedLifetime.expired(),
             "retired node dies only after graph and command release NodeRefs");
  return passed;
}

bool testWakeFailureAfterAdmission() {
  ThreadChannels channels;
  NodeGraph graph;
  const NodeRef target =
      insertNode(graph, id(NodeKind::Thread, "thread/closed-wake"));
  channels.failNextQtToWorkerWakeForTest();

  NodeAction action = richPromptAction(target);
  const NodeAction expected = action;
  const ChannelSendStatus status = channels.sendNodeAction(action);

  bool passed = true;
  passed &= expect(status == ChannelSendStatus::AcceptedWakeFailed &&
                       messageAdmitted(status) && deliveryGuaranteed(status) &&
                       wakeFailed(status) &&
                       channels.qtToWorkerSizeApprox() == 1 && !action.target &&
                       action.promptText.empty() && action.attachments.empty(),
                   "failed wake reports one admitted payload for bounded "
                   "fallback delivery");
  passed &= expect(channels.drainQtToWorkerWake().status ==
                       EventFd::DrainStatus::Empty,
                   "injected wake failure leaves the eventfd unsignaled");

  QtToWorkerMessage message;
  passed &=
      expect(channels.tryReceiveForWorker(message) &&
                 std::holds_alternative<NodeAction>(message) &&
                 std::get<NodeAction>(message) == expected &&
                 !channels.tryReceiveForWorker(message),
             "wake failure leaves exactly one admitted command in the mailbox");

  ThreadChannels shutdownChannels;
  shutdownChannels.failNextQtToWorkerWakeForTest();
  ShutdownRequest shutdown;
  const ChannelSendStatus shutdownStatus =
      shutdownChannels.sendShutdown(shutdown);
  QtToWorkerMessage shutdownMessage;
  passed &= expect(
      shutdownStatus == ChannelSendStatus::AcceptedWakeFailed &&
          deliveryGuaranteed(shutdownStatus) &&
          shutdownChannels.drainQtToWorkerWake().status ==
              EventFd::DrainStatus::Empty &&
          shutdownChannels.tryReceiveForWorker(shutdownMessage) &&
          std::holds_alternative<ShutdownRequest>(shutdownMessage),
      "an unwoken shutdown remains available to the worker timeout drain");

  ThreadChannels workerChannels;
  workerChannels.failNextWorkerToQtWakeForTest();
  UiEffect effect{UiEffectKind::ShowNotice, {}, "fallback notice", {}};
  const ChannelSendStatus workerStatus = workerChannels.sendUiEffect(effect);
  WorkerToQtMessage workerMessage;
  passed &=
      expect(workerStatus == ChannelSendStatus::AcceptedWakeFailed &&
                 deliveryGuaranteed(workerStatus) && wakeFailed(workerStatus) &&
                 workerChannels.drainWorkerToQtWake().status ==
                     EventFd::DrainStatus::Empty &&
                 workerChannels.tryReceiveForQt(workerMessage) &&
                 std::holds_alternative<UiEffect>(workerMessage),
             "Qt can recover one admitted worker message after a failed wake");

  ThreadChannels closedChannels;
  closedChannels.close();
  NodeAction rejected = richPromptAction(target);
  const NodeAction retained = rejected;
  passed &= expect(
      closedChannels.sendNodeAction(rejected) == ChannelSendStatus::QueueFull &&
          !messageAdmitted(ChannelSendStatus::QueueFull) &&
          !deliveryGuaranteed(ChannelSendStatus::QueueFull) &&
          rejected == retained && closedChannels.qtToWorkerSizeApprox() == 0,
      "a closed channel rejects without consuming user-owned payload");
  return passed;
}

bool testOversizedGraphChangeRequiresRescan() {
  ThreadChannels channels;
  NodeGraph graph;
  GraphChange oversized;
  {
    auto write = graph.write();
    for (std::size_t index = 0;
         index <= ThreadChannels::MaximumDirectGraphReferences; ++index) {
      static_cast<void>(write.upsert(
          id(NodeKind::Item, "oversized/" + std::to_string(index))));
    }
    oversized = write.finish();
  }
  const std::uint64_t revision = oversized.revision;

  bool passed = true;
  passed &= expect(
      channels.sendGraphChanged(std::move(oversized)) ==
              ChannelSendStatus::CoalescedRescan &&
          channels.workerToQtSizeApprox() == 0 && channels.rescanPending(),
      "an oversized committed transaction coalesces even with queue space");
  passed &= expect(channels.drainWorkerToQtWake().count == 1,
                   "an oversized graph rescan produces one eventfd wake");
  WorkerToQtMessage message;
  passed &= expect(
      channels.tryReceiveForQt(message) &&
          std::holds_alternative<GraphChanged>(message) &&
          std::get<GraphChanged>(message).revision == revision &&
          std::get<GraphChanged>(message).rescanRequired &&
          std::get<GraphChanged>(message).affected.empty() &&
          std::get<GraphChanged>(message).removed.empty(),
      "Qt receives only the explicit rescan marker, never an unbounded ref "
      "vector");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testEventFd();
  passed &= testDescriptorsAndVariantOrder();
  passed &= testQtToWorkerBackpressure();
  passed &= testGraphCoalescingAndRetiredLifetime();
  passed &= testOversizedGraphChangeRequiresRescan();
  passed &= testWakeFailureAfterAdmission();
  return passed ? 0 : 1;
}
