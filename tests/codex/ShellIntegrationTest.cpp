// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include <core/SNodeC.h>
#include <utils/Config.h>

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/PendingRequestDialog.h"
#include "codex/ShellWidget.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ThreadPane.h"
#include "codex/nodegraph/WorkerLogic.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace codexui::codex {

// The production API intentionally exposes no graph writer or mailbox
// consumer to Qt. This friend is confined to this integration test and acts
// as the worker side of that boundary without adding a third execution path.
class FrontendSessionTestPeer final {
public:
  static nodegraph::NodeGraph &graph(FrontendSession &session) {
    return session.graph;
  }

  static nodegraph::ThreadChannels &channels(FrontendSession &session) {
    return session.channels;
  }

  static void drainWorkerMessages(FrontendSession &session) {
    session.drainWorkerMessages();
  }
};

namespace {

using namespace codexui::nodegraph;

int failures = 0;

void require(bool condition, std::string_view message) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

void spin(int milliseconds = 20) {
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  } while (timer.elapsed() < std::max(milliseconds, 1));
}

bool spinUntil(const std::function<bool()> &predicate,
               int timeoutMilliseconds = 1000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    spin(2);
  return predicate();
}

const Value *field(const std::shared_ptr<const NodeState> &state,
                   std::string_view name) {
  if (!state)
    return nullptr;
  const auto found = state->fields.find(name);
  return found == state->fields.end() ? nullptr : &found->second;
}

bool stringFieldEquals(const std::shared_ptr<const NodeState> &state,
                       std::string_view name, std::string_view expected) {
  const Value *value = field(state, name);
  return value && value->asString() && *value->asString() == expected;
}

std::vector<QtToWorkerMessage> takeQtMessages(ThreadChannels &channels) {
  static_cast<void>(channels.drainQtToWorkerWake());
  std::vector<QtToWorkerMessage> messages;
  QtToWorkerMessage message;
  while (channels.tryReceiveForWorker(message)) {
    messages.emplace_back(std::move(message));
    message = ShutdownRequest{};
  }
  return messages;
}

void applyThread(WorkerLogic &worker, std::string id,
                 std::string name = "Graph thread") {
  static_cast<void>(worker.apply(DecodedMessage{
      DecodedMessageKind::ServerNotification,
      "thread/started",
      std::nullopt,
      {{"thread", Value(Value::Object{{"id", Value(std::move(id))},
                                      {"name", Value(std::move(name))},
                                      {"cwd", Value("/tmp")}})}}}));
}

void makeReady(WorkerLogic &worker) {
  static_cast<void>(worker.transportEvent("connected"));
  static_cast<void>(worker.bridgeState("test-controller", "controller",
                                       "test-controller", 1, "ready"));
}

QListWidgetItem *threadItem(QListWidget *list, std::string_view id) {
  if (!list)
    return nullptr;
  for (int row = 0; row < list->count(); ++row) {
    QListWidgetItem *item = list->item(row);
    if (item && item->data(Qt::UserRole).toString().toStdString() == id)
      return item;
  }
  return nullptr;
}

bool selectThread(QListWidget *list, std::string_view id) {
  QListWidgetItem *item = threadItem(list, id);
  if (!item)
    return false;
  list->setCurrentItem(item);
  spin();
  return list->currentItem() == item;
}

bool submit(codexui::ExpandingPromptEditor *editor, const QString &prompt) {
  if (!editor || !editor->isEnabled())
    return false;
  editor->setPlainText(prompt);
  return QMetaObject::invokeMethod(editor, "submitRequested",
                                   Qt::DirectConnection);
}

middle::ConversationCard *localPromptCard(ShellWidget &shell,
                                          std::string_view prompt) {
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    auto *card = dynamic_cast<middle::ConversationCard *>(widget);
    if (!card)
      continue;
    const auto *local =
        std::get_if<middle::LocalPromptData>(&card->data().payload);
    if (local && local->prompt == prompt)
      return card;
  }
  return nullptr;
}

middle::ConversationCard *agentMessageCard(ShellWidget &shell,
                                           std::string_view message) {
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    auto *card = dynamic_cast<middle::ConversationCard *>(widget);
    if (!card)
      continue;
    const auto *agent =
        std::get_if<middle::AgentMessageData>(&card->data().payload);
    if (agent && agent->text == message)
      return card;
  }
  return nullptr;
}

void graphNotificationsDetachBeforeRetirement(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);

  std::size_t notifications = 0;
  bool ordinaryDetached = false;
  bool detachedDuringRescan = false;
  NodeRef ordinaryRemoved;
  NodeRef coalescedRemoved;
  int ordinaryAttachment = 1;
  int coalescedAttachment = 2;
  session.setGraphChangedHandler([&](const GraphChanged &changed) {
    ++notifications;
    for (const NodeRef &node : changed.removed) {
      if (!node)
        continue;
      if (node->uiAttachment() == &ordinaryAttachment) {
        ordinaryRemoved = node;
        node->setUiAttachment(nullptr);
        ordinaryDetached = true;
      } else if (node->uiAttachment() == &coalescedAttachment) {
        coalescedRemoved = node;
        node->setUiAttachment(nullptr);
        detachedDuringRescan = changed.rescanRequired;
      }
    }
  });

  applyThread(worker, "ordinary-removal");
  spin();
  NodeRef ordinary;
  {
    auto read = session.nodeGraph().tryRead();
    ordinary =
        read ? read->find({NodeKind::Thread, "ordinary-removal"}) : NodeRef{};
  }
  require(ordinary != nullptr,
          "worker update is visible through the shared graph");
  if (ordinary)
    ordinary->setUiAttachment(&ordinaryAttachment);
  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "thread/deleted",
                                  std::nullopt,
                                  {{"threadId", Value("ordinary-removal")}}}));
  require(
      spinUntil([&] { return channels.qtToWorkerSizeApprox() != 0; }),
      "Qt receives removal and queues its typed detachment acknowledgement");

  std::vector<QtToWorkerMessage> commands = takeQtMessages(channels);
  NodeRef ordinaryAcknowledgement;
  for (QtToWorkerMessage &command : commands) {
    if (auto *action = std::get_if<NodeAction>(&command);
        action && action->kind == NodeActionKind::UiDetached)
      ordinaryAcknowledgement = std::move(action->target);
  }
  require(ordinaryDetached && ordinaryRemoved == ordinary &&
              ordinaryAcknowledgement == ordinary,
          "Qt clears a removed node attachment before acknowledging it");
  static_cast<void>(
      worker.acknowledgeUiDetached(std::move(ordinaryAcknowledgement)));
  {
    auto read = session.nodeGraph().tryRead();
    require(read && read->retiredNodes().empty(),
            "the worker releases ordinary retirement only after UiDetached");
  }

  applyThread(worker, "coalesced-removal");
  spin();
  NodeRef coalesced;
  {
    auto read = session.nodeGraph().tryRead();
    coalesced =
        read ? read->find({NodeKind::Thread, "coalesced-removal"}) : NodeRef{};
  }
  require(coalesced != nullptr, "coalescing fixture has a live shared node");
  if (coalesced)
    coalesced->setUiAttachment(&coalescedAttachment);

  std::size_t fillerCount = 0;
  for (;;) {
    UiEffect filler;
    filler.text = "fill-" + std::to_string(fillerCount);
    const ChannelSendStatus status = channels.sendUiEffect(filler);
    if (status == ChannelSendStatus::QueueFull)
      break;
    require(status == ChannelSendStatus::Accepted,
            "ordinary worker-to-Qt filler is admitted normally");
    ++fillerCount;
  }
  require(fillerCount + ThreadChannels::WorkerToQtReservedSlots ==
              ThreadChannels::WorkerToQtCapacity,
          "worker mailbox saturation preserves critical and terminal slots");

  const ChannelSendStatus coalescedStatus =
      worker.apply({DecodedMessageKind::ServerNotification,
                    "thread/deleted",
                    std::nullopt,
                    {{"threadId", Value("coalesced-removal")}}});
  require(coalescedStatus == ChannelSendStatus::CoalescedRescan,
          "a saturated graph notification becomes an explicit rescan");
  FrontendSessionTestPeer::drainWorkerMessages(session);
  require(detachedDuringRescan && channels.qtToWorkerSizeApprox() == 0 &&
              channels.workerToQtSizeApprox() != 0,
          "Qt detaches a rescan retirement but defers its acknowledgement "
          "until every older queued notification has drained");
  while (channels.workerToQtSizeApprox() != 0 || channels.rescanPending())
    FrontendSessionTestPeer::drainWorkerMessages(session);
  require(channels.qtToWorkerSizeApprox() != 0,
          "Qt acknowledges the detached retirement after the stale backlog");

  commands = takeQtMessages(channels);
  NodeRef coalescedAcknowledgement;
  for (QtToWorkerMessage &command : commands) {
    if (auto *action = std::get_if<NodeAction>(&command);
        action && action->kind == NodeActionKind::UiDetached &&
        action->target == coalesced)
      coalescedAcknowledgement = std::move(action->target);
  }
  require(coalescedRemoved == coalesced &&
              coalescedAcknowledgement == coalesced,
          "rescan retirement carries the stable NodeRef through UiDetached");
  static_cast<void>(
      worker.acknowledgeUiDetached(std::move(coalescedAcknowledgement)));
  {
    auto read = session.nodeGraph().tryRead();
    require(read && read->retiredNodes().empty(),
            "coalesced retirement is released after Qt detachment");
  }
  require(notifications >= 4,
          "eventfd delivery exposes committed changes and synthesized rescan");
}

void massRetirementIsSliced(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);

  constexpr std::size_t NodeCount = 640;
  std::vector<NodeRef> nodes;
  nodes.reserve(NodeCount);
  {
    auto write = graph.write();
    for (std::size_t index = 0; index < NodeCount; ++index) {
      nodes.emplace_back(write.upsert(
          {NodeKind::Item, "mass-retired-" + std::to_string(index)}));
    }
    static_cast<void>(write.finish());
  }

  int attachment = 1;
  for (const NodeRef &node : nodes)
    node->setUiAttachment(&attachment);

  std::unordered_set<Node *> detached;
  std::size_t largestBatch = 0;
  session.setGraphChangedHandler([&](const GraphChanged &changed) {
    if (!changed.rescanRequired)
      return;
    largestBatch = std::max(largestBatch, changed.removed.size());
    for (const NodeRef &node : changed.removed) {
      if (node && node->uiAttachment() != nullptr) {
        node->setUiAttachment(nullptr);
        detached.insert(node.get());
      }
    }
  });

  GraphChange removal;
  {
    auto write = graph.write();
    for (const NodeRef &node : nodes)
      write.remove(node);
    removal = write.finish();
  }
  require(channels.sendGraphChanged(std::move(removal)) ==
              ChannelSendStatus::CoalescedRescan,
          "an oversized mass removal requests an explicit graph rescan even "
          "when the worker mailbox has space");

  FrontendSessionTestPeer::drainWorkerMessages(session);
  require(!detached.empty() && detached.size() <= 64 && largestBatch <= 64,
          "one Qt pass detaches only a bounded retirement batch");

  std::size_t acknowledgements = 0;
  for (std::size_t pass = 0; pass < 128; ++pass) {
    FrontendSessionTestPeer::drainWorkerMessages(session);
    std::vector<QtToWorkerMessage> commands = takeQtMessages(channels);
    for (QtToWorkerMessage &command : commands) {
      auto *action = std::get_if<NodeAction>(&command);
      if (!action || action->kind != NodeActionKind::UiDetached)
        continue;
      ++acknowledgements;
      static_cast<void>(
          worker.acknowledgeUiDetached(std::move(action->target)));
    }
    auto read = graph.tryRead();
    if (read && read->retiredCount() == 0 &&
        channels.workerToQtSizeApprox() == 0 &&
        channels.qtToWorkerSizeApprox() == 0)
      break;
  }

  auto read = graph.tryRead();
  require(detached.size() == NodeCount && acknowledgements == NodeCount &&
              largestBatch <= 64 && read && read->retiredCount() == 0,
          "sliced mass retirement detaches and acknowledges every NodeRef "
          "exactly once without an unbounded Qt graph read");
}

void selectedRemovalUnbindsBeforeWorkerRetirement(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "removed-selected", "Removed selected");
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "item/started",
       std::nullopt,
       {{"threadId", Value("removed-selected")},
        {"turnId", Value("removed-turn")},
        {"item", Value(Value::Object{{"id", Value("removed-item")},
                                     {"type", Value("agentMessage")},
                                     {"text", Value("remove this card")}})}}}));
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(
      spinUntil(
          [&] { return threadItem(list, "removed-selected") != nullptr; }) &&
          selectThread(list, "removed-selected") && spinUntil([&] {
            return agentMessageCard(shell, "remove this card") != nullptr;
          }),
      "selected-removal fixture materializes a graph-attached row and card");
  static_cast<void>(takeQtMessages(channels)); // discard hydration

  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "thread/deleted",
                                  std::nullopt,
                                  {{"threadId", Value("removed-selected")}}}));
  require(
      spinUntil([&] { return channels.qtToWorkerSizeApprox() != 0; }),
      "selected removal detaches Qt and queues retirement acknowledgements");
  std::vector<QtToWorkerMessage> acknowledgements = takeQtMessages(channels);
  std::size_t released = 0;
  for (QtToWorkerMessage &message : acknowledgements) {
    auto *action = std::get_if<NodeAction>(&message);
    if (!action || action->kind != NodeActionKind::UiDetached)
      continue;
    static_cast<void>(worker.acknowledgeUiDetached(std::move(action->target)));
    ++released;
  }
  spin(60); // exercises every deferred binding/render after releaseRetired()

  auto read = graph.tryRead();
  require(released != 0 && read && read->retiredNodes().empty() &&
              !threadItem(list, "removed-selected") &&
              !agentMessageCard(shell, "remove this card"),
          "deferred Qt work retains no released selected-thread NodeRef");
}

void removedAffectedOptimisticRetryDoesNotReadReleasedNode(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  spin(30);

  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  if (newThread)
    newThread->click();
  spin(30);
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  require(newThread && threadItem(list, "draft:new-thread") && editor &&
              submit(editor, QStringLiteral("retain optimistic selection")),
          "same-transaction removal fixture starts an optimistic creation");
  static_cast<void>(takeQtMessages(channels));

  GraphChange affectedAndRemoved;
  NodeRef removedPrompt;
  {
    auto write = graph.write();
    NodeRef thread = write.upsert({NodeKind::Thread, "transient-thread"});
    NodeRef turn = write.upsert({NodeKind::Turn, "transient-turn"});
    NodeState state;
    state.status = NodeStatus::Pending;
    state.fields = {{"type", Value("localPrompt")},
                    {"local", Value(true)},
                    {"createsThread", Value(true)},
                    {"submissionId", Value(std::uint64_t{99})},
                    {"dispatchState", Value("queued")}};
    removedPrompt = write.upsert({NodeKind::Item, "transient-local-prompt"},
                                 std::move(state));
    write.setParent(thread, turn);
    write.setParent(turn, removedPrompt);
    write.remove(removedPrompt);
    affectedAndRemoved = write.finish();
  }
  require(std::ranges::find(affectedAndRemoved.affected, removedPrompt) !=
                  affectedAndRemoved.affected.end() &&
              std::ranges::find(affectedAndRemoved.removed, removedPrompt) !=
                  affectedAndRemoved.removed.end() &&
              messageAdmitted(
                  channels.sendGraphChanged(std::move(affectedAndRemoved))),
          "one notification may carry the same stable ref as affected and "
          "removed");

  // Force every graph-reading handler onto its non-blocking Qt retry path.
  // FrontendSession can still collect the stable removed ref and defer graph
  // membership release until its typed acknowledgement reaches the worker.
  auto contended = graph.write();
  FrontendSessionTestPeer::drainWorkerMessages(session);
  static_cast<void>(contended.finish());

  std::vector<QtToWorkerMessage> acknowledgements = takeQtMessages(channels);
  bool released = false;
  for (QtToWorkerMessage &message : acknowledgements) {
    auto *action = std::get_if<NodeAction>(&message);
    if (!action || action->kind != NodeActionKind::UiDetached ||
        action->target != removedPrompt)
      continue;
    static_cast<void>(worker.acknowledgeUiDetached(std::move(action->target)));
    released = true;
  }
  spin(80); // executes the delayed reconciliation after releaseRetired()

  auto read = graph.tryRead();
  require(released && read && read->retiredNodes().empty() &&
              !read->find(removedPrompt->id()) &&
              threadItem(list, "draft:new-thread") == list->currentItem(),
          "delayed optimistic reconciliation validates graph membership "
          "before reading a released affected-and-removed ref");
}

void typedActionsAreExactOnceAndBounded(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  applyThread(worker, "typed-action-target");
  spin();

  NodeRef target;
  {
    auto read = session.nodeGraph().tryRead();
    target = read ? read->find({NodeKind::Thread, "typed-action-target"})
                  : NodeRef{};
  }

  NodeAction authored;
  authored.target = target;
  authored.kind = NodeActionKind::SubmitPrompt;
  authored.promptText = "  exact authored prompt  ";
  authored.attachments.push_back(
      {"/tmp/exact.bin", "exact.bin", "application/octet-stream",
       std::vector<std::uint8_t>{0, 1, 2, 127, 254, 255}});
  authored.payload.emplace("model", Value("current-model"));
  authored.correlation = "typed-once";
  const char *promptStorage = authored.promptText.data();
  const std::uint8_t *attachmentStorage =
      authored.attachments.front().bytes->data();
  require(session.sendNodeAction(authored) == ChannelSendStatus::Accepted &&
              !authored.target && authored.promptText.empty() &&
              authored.attachments.empty(),
          "typed admission moves newly-authored data out of Qt exactly once");

  const EventFd::DrainResult exactWake = channels.drainQtToWorkerWake();
  QtToWorkerMessage received;
  const bool gotOne = channels.tryReceiveForWorker(received);
  const NodeAction *receivedAction =
      gotOne ? std::get_if<NodeAction>(&received) : nullptr;
  require(exactWake.status == EventFd::DrainStatus::Drained &&
              exactWake.count == 1 && receivedAction &&
              receivedAction->target == target &&
              receivedAction->kind == NodeActionKind::SubmitPrompt &&
              receivedAction->promptText == "  exact authored prompt  " &&
              receivedAction->promptText.data() == promptStorage &&
              receivedAction->attachments.front().bytes->data() ==
                  attachmentStorage &&
              receivedAction->correlation == "typed-once" &&
              !channels.tryReceiveForWorker(received),
          "one Qt action produces one FIFO payload and one wake");

  std::size_t admissions = 0;
  for (;;) {
    RuntimeAction filler;
    filler.kind = RuntimeActionKind::RefreshThreads;
    filler.correlation = "filler-" + std::to_string(admissions);
    const ChannelSendStatus status = session.sendRuntimeAction(filler);
    if (status == ChannelSendStatus::QueueFull)
      break;
    require(status == ChannelSendStatus::Accepted,
            "ordinary typed filler is admitted normally");
    ++admissions;
  }
  require(admissions + 1 == ThreadChannels::QtToWorkerCapacity,
          "Qt mailbox reserves one bounded slot for shutdown");

  NodeAction rejected;
  rejected.target = target;
  rejected.kind = NodeActionKind::SubmitPrompt;
  rejected.promptText = "retain this input";
  rejected.attachments.push_back(
      {"/tmp/retained.txt", "retained.txt", "text/plain", std::nullopt});
  const NodeAction unchanged = rejected;
  require(session.sendNodeAction(rejected) == ChannelSendStatus::QueueFull &&
              rejected == unchanged,
          "queue saturation rejects visibly without consuming user input");

  const EventFd::DrainResult saturatedWake = channels.drainQtToWorkerWake();
  std::size_t drained = 0;
  bool onlyFillers = true;
  while (channels.tryReceiveForWorker(received)) {
    const RuntimeAction *filler = std::get_if<RuntimeAction>(&received);
    onlyFillers = onlyFillers && filler &&
                  filler->correlation == "filler-" + std::to_string(drained);
    ++drained;
  }
  require(saturatedWake.status == EventFd::DrainStatus::Drained &&
              saturatedWake.count == admissions && drained == admissions &&
              onlyFillers,
          "a rejected non-idempotent action adds no payload and no wake");
}

void qtHeartbeatSurvivesLargeInboundTraffic(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "traffic-thread", "Traffic thread");
  auto *threadList =
      shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] {
            return threadItem(threadList, "traffic-thread") != nullptr;
          }),
          "large-traffic fixture materializes the existing thread list");
  require(selectThread(threadList, "traffic-thread"),
          "large-traffic fixture binds the real conversation view");
  static_cast<void>(takeQtMessages(channels)); // discard the hydration action

  std::atomic_bool producerFinished = false;
  std::atomic_bool midpointReady = false;
  std::atomic_bool abortWait = false;
  std::atomic_bool heartbeatObservedAtMidpoint = false;
  std::atomic_size_t coalescedNotifications = 0;
  std::atomic_uint64_t heartbeatCount = 0;
  std::uint64_t heartbeatsWithBacklog = 0;

  QTimer heartbeat;
  heartbeat.setInterval(0);
  QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
    heartbeatCount.fetch_add(1, std::memory_order_relaxed);
    if (channels.workerToQtSizeApprox() != 0 || channels.rescanPending())
      ++heartbeatsWithBacklog;
  });
  heartbeat.start();

  constexpr std::size_t DeltaCount = 4096;
  std::thread producer([&] {
    const auto publish = [&](DecodedMessage message) {
      const ChannelSendStatus status = worker.apply(std::move(message));
      if (status == ChannelSendStatus::CoalescedRescan ||
          status == ChannelSendStatus::CoalescedRescanWakeFailed)
        coalescedNotifications.fetch_add(1, std::memory_order_relaxed);
    };
    publish(
        {DecodedMessageKind::ServerNotification,
         "turn/started",
         std::nullopt,
         {{"threadId", Value("traffic-thread")},
          {"turn", Value(Value::Object{{"id", Value("traffic-turn")},
                                       {"status", Value("inProgress")}})}}});
    publish({DecodedMessageKind::ServerNotification,
             "item/started",
             std::nullopt,
             {{"threadId", Value("traffic-thread")},
              {"turnId", Value("traffic-turn")},
              {"item", Value(Value::Object{{"id", Value("traffic-item")},
                                           {"type", Value("agentMessage")},
                                           {"text", Value("")}})}}});

    for (std::size_t index = 0; index < DeltaCount; ++index) {
      const bool lastItem = index + 1 == DeltaCount;
      publish({DecodedMessageKind::ServerNotification,
               "item/started",
               std::nullopt,
               {{"threadId", Value("traffic-thread")},
                {"turnId", Value("traffic-turn")},
                {"item",
                 Value(Value::Object{
                     {"id", Value("background-item-" + std::to_string(index))},
                     {"type", Value(lastItem ? "agentMessage" : "reasoning")},
                     {"text", Value(lastItem ? "latest visible item" : "")},
                     {"status", Value("running")}})}},
               {},
               static_cast<std::int64_t>(index + 1)});
      publish({DecodedMessageKind::ServerNotification,
               "item/agentMessage/delta",
               std::nullopt,
               {{"threadId", Value("traffic-thread")},
                {"turnId", Value("traffic-turn")},
                {"itemId", Value("traffic-item")},
                {"delta", Value("x")}}});
      if (index == DeltaCount / 2) {
        midpointReady.store(true, std::memory_order_release);
        const std::uint64_t before =
            heartbeatCount.load(std::memory_order_acquire);
        while (!abortWait.load(std::memory_order_acquire) &&
               heartbeatCount.load(std::memory_order_acquire) == before)
          std::this_thread::yield();
        heartbeatObservedAtMidpoint.store(
            heartbeatCount.load(std::memory_order_acquire) != before,
            std::memory_order_release);
      }
    }
    producerFinished.store(true, std::memory_order_release);
  });

  QElapsedTimer deadline;
  deadline.start();
  // Let the worker establish a real saturated backlog before Qt begins
  // pumping events. The worker then pauses at the midpoint until the Qt timer
  // proves it can run while that backlog is being drained.
  while (!midpointReady.load(std::memory_order_acquire) &&
         deadline.elapsed() < 5000)
    std::this_thread::yield();
  while (!producerFinished.load(std::memory_order_acquire) &&
         deadline.elapsed() < 5000)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  abortWait.store(true, std::memory_order_release);
  producer.join();
  const bool drained = spinUntil(
      [&] {
        return channels.workerToQtSizeApprox() == 0 &&
               !channels.rescanPending();
      },
      2000);
  const bool renderedLatest = spinUntil(
      [&] { return agentMessageCard(shell, "latest visible item") != nullptr; },
      2000);
  heartbeat.stop();

  std::string streamedText;
  std::uint64_t loadedItems = 0;
  std::int64_t latestActivity = 0;
  {
    auto read = session.nodeGraph().tryRead();
    const NodeRef item =
        read ? read->find(scopedItemNodeId(
                   scopedTurnNodeId("traffic-thread", "traffic-turn"),
                   "traffic-item"))
             : NodeRef{};
    const Value *textValue = item ? field(read->state(item), "text") : nullptr;
    if (textValue && textValue->asString())
      streamedText = *textValue->asString();
    const NodeRef thread =
        read ? read->find({NodeKind::Thread, "traffic-thread"}) : NodeRef{};
    const Value *count =
        thread ? field(read->state(thread), "historyLoadedItemCount") : nullptr;
    const Value *activity =
        thread ? field(read->state(thread), "localActivityAt") : nullptr;
    if (count && count->asUInt64())
      loadedItems = *count->asUInt64();
    if (activity && activity->asInt64())
      latestActivity = *activity->asInt64();
  }
  require(producerFinished.load(std::memory_order_acquire),
          "the distinct-item producer completes within the responsiveness "
          "budget");
  require(drained, "Qt drains the bounded graph notification backlog");
  require(heartbeatObservedAtMidpoint.load(std::memory_order_acquire) &&
              heartbeatCount.load(std::memory_order_acquire) > 1 &&
              heartbeatsWithBacklog > 0,
          "Qt heartbeat runs while the distinct-item backlog is nonempty");
  require(renderedLatest,
          "the visible existing widget reaches the latest streamed state");
  require(streamedText.size() == DeltaCount && loadedItems == DeltaCount + 1 &&
              latestActivity == static_cast<std::int64_t>(DeltaCount),
          "large inbound traffic leaves complete distinct-item, stream, and "
          "thread-activity state");
  require(coalescedNotifications.load(std::memory_order_acquire) != 0,
          "large inbound traffic uses explicit notification coalescing");
}

void graphBackedShellPreservesDraftsAndPrompts(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "shell-thread", "Shared graph thread");
  require(spinUntil([&] {
            auto *list =
                shell.findChild<QListWidget *>(QStringLiteral("threadList"));
            return threadItem(list, "shell-thread") != nullptr;
          }),
          "the existing thread widget materializes from shared graph nodes");
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(selectThread(list, "shell-thread"),
          "selecting the graph-backed row binds the existing conversation");
  static_cast<void>(takeQtMessages(channels)); // Hydrate is tested elsewhere.

  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  const QString exact = QStringLiteral("  graph prompt stays exact  ");
  const QString trimmed = exact.trimmed();
  require(submit(editor, exact), "the real composer emits its submit action");
  std::vector<QtToWorkerMessage> actions = takeQtMessages(channels);
  NodeAction prompt;
  std::size_t promptCount = 0;
  for (QtToWorkerMessage &message : actions) {
    if (auto *action = std::get_if<NodeAction>(&message);
        action && action->kind == NodeActionKind::SubmitPrompt) {
      prompt = std::move(*action);
      ++promptCount;
    }
  }
  require(promptCount == 1 && prompt.target &&
              prompt.target->id() == NodeId{NodeKind::Thread, "shell-thread"} &&
              prompt.promptText == trimmed.toStdString() && editor &&
              editor->toPlainText().isEmpty(),
          "the composer emits one typed prompt with legacy whitespace "
          "normalization");

  PromptTransition transition = worker.admitPrompt(std::move(prompt));
  const NodeRef localPrompt =
      transition.command ? transition.command->localPrompt : NodeRef{};
  require(
      localPrompt != nullptr,
      "the worker turns an admitted action into the one shared prompt node");
  require(spinUntil([&] {
            return localPromptCard(shell, trimmed.toStdString()) != nullptr;
          }),
          "the visible existing card renders directly from the prompt node");
  middle::ConversationCard *card =
      localPromptCard(shell, trimmed.toStdString());

  editor->setPlainText(QStringLiteral("unsent editor draft"));
  static_cast<void>(worker.apply({DecodedMessageKind::ClientResult,
                                  "model/list",
                                  ProtocolRequestId("catalog-refresh"),
                                  {{"models", Value(Value::Array{})}}}));
  spin(40);
  require(
      editor->toPlainText() == QStringLiteral("unsent editor draft") &&
          localPromptCard(shell, trimmed.toStdString()) == card,
      "unrelated graph updates preserve local editor text and card identity");
}

void inactiveThreadNeverReactivatesAStaleTurn(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "inactive-thread", "Inactive lifecycle");
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       {{"threadId", Value("inactive-thread")},
        {"turn", Value(Value::Object{{"id", Value("stale-turn")},
                                     {"status", Value("inProgress")}})}}}));
  auto *threadList =
      shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] {
            return threadItem(threadList, "inactive-thread") != nullptr;
          }) &&
              selectThread(threadList, "inactive-thread"),
          "inactive lifecycle fixture selects its graph-backed thread");
  static_cast<void>(takeQtMessages(channels));

  QPushButton *stopButton = nullptr;
  for (QPushButton *button : shell.findChildren<QPushButton *>()) {
    if (button && button->text() == QStringLiteral("Stop")) {
      stopButton = button;
      break;
    }
  }
  require(stopButton && spinUntil([&] { return stopButton->isVisible(); }),
          "the maintained active-turn relation exposes the existing Stop "
          "control");

  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "thread/status/changed",
       std::nullopt,
       {{"threadId", Value("inactive-thread")},
        {"status", Value(Value::Object{{"type", Value("idle")}})}}}));
  require(stopButton && spinUntil([&] { return !stopButton->isVisible(); }),
          "object-shaped idle status hides Stop without rediscovering the "
          "stale Running child");
  if (stopButton)
    stopButton->click();
  const auto idleActions = takeQtMessages(channels);
  require(std::ranges::none_of(
              idleActions,
              [](const QtToWorkerMessage &entry) {
                const auto *action = std::get_if<NodeAction>(&entry);
                return action && action->kind == NodeActionKind::InterruptTurn;
              }),
          "idle thread state cannot emit an interrupt for a stale turn");

  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       {{"threadId", Value("inactive-thread")},
        {"turn", Value(Value::Object{{"id", Value("closing-turn")},
                                     {"status", Value("inProgress")}})}}}));
  require(stopButton && spinUntil([&] { return stopButton->isVisible(); }),
          "a later authoritative active turn restores Stop");
  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "thread/closed",
                                  std::nullopt,
                                  {{"threadId", Value("inactive-thread")}}}));
  require(stopButton && spinUntil([&] { return !stopButton->isVisible(); }),
          "closed lifecycle clears Stop despite retained Running history");
}

void reloadAndReconnectHydrationStayExplicit(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "rehydrate-thread", "Rehydrate me");
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] { return threadItem(list, "rehydrate-thread"); }),
          "rehydration fixture appears in the real thread list");
  require(selectThread(list, "rehydrate-thread"),
          "rehydration fixture can be selected");
  std::vector<QtToWorkerMessage> selection = takeQtMessages(channels);
  const auto selectedHydrates =
      std::ranges::count_if(selection, [](const QtToWorkerMessage &message) {
        const auto *action = std::get_if<NodeAction>(&message);
        return action && action->kind == NodeActionKind::Hydrate;
      });
  require(selectedHydrates == 1,
          "ordinary selection emits one non-forced hydration action");

  NodeRef original;
  {
    auto read = session.nodeGraph().tryRead();
    original =
        read ? read->find({NodeKind::Thread, "rehydrate-thread"}) : NodeRef{};
  }
  static_cast<void>(worker.threadHydration(original, "ready"));
  spin(30);
  static_cast<void>(takeQtMessages(channels));

  const QPoint menuPoint =
      list->visualItemRect(threadItem(list, "rehydrate-thread")).center();
  QMetaObject::invokeMethod(list, "customContextMenuRequested",
                            Qt::DirectConnection, Q_ARG(QPoint, menuPoint));
  spin(20);
  QAction *reload = nullptr;
  for (QMenu *menu : shell.findChildren<QMenu *>()) {
    for (QAction *action : menu->actions()) {
      if (action && action->text() == QStringLiteral("Reload")) {
        reload = action;
        break;
      }
    }
    if (reload)
      break;
  }
  if (reload)
    reload->trigger();
  const std::vector<QtToWorkerMessage> reloadMessages =
      takeQtMessages(channels);
  require(reload &&
              std::ranges::count_if(
                  reloadMessages,
                  [](const QtToWorkerMessage &message) {
                    const auto *action = std::get_if<NodeAction>(&message);
                    return action && action->kind == NodeActionKind::Reload;
                  }) == 1,
          "visible Reload emits one distinct forced-read action");

  static_cast<void>(worker.bridgeState("test-controller", "controller",
                                       "test-controller", 2, "ready",
                                       "provider replaced"));
  spin(40);
  applyThread(worker, "rehydrate-thread", "Recreated thread");
  require(spinUntil([&] {
            auto read = session.nodeGraph().tryRead();
            const NodeRef recreated =
                read ? read->find({NodeKind::Thread, "rehydrate-thread"})
                     : NodeRef{};
            return recreated && recreated != original;
          }),
          "provider reset recreates selected canonical id with a fresh node");
  spin(50);
  const std::vector<QtToWorkerMessage> rebound = takeQtMessages(channels);
  std::size_t automaticHydrates = 0;
  NodeRef automaticTarget;
  for (const QtToWorkerMessage &message : rebound) {
    const auto *action = std::get_if<NodeAction>(&message);
    if (!action || action->kind != NodeActionKind::Hydrate)
      continue;
    ++automaticHydrates;
    automaticTarget = action->target;
  }
  require(automaticHydrates == 1 && automaticTarget &&
              automaticTarget != original &&
              automaticTarget->id() ==
                  NodeId{NodeKind::Thread, "rehydrate-thread"},
          "a recreated selected thread is read once without resending prompts");
}

void backgroundGraphChangesDoNotRefreshSelectedConversation(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  const auto applyConversation = [&](std::string threadId, std::string turnId,
                                     std::string itemId, std::string message) {
    applyThread(worker, threadId, threadId);
    static_cast<void>(worker.apply(
        {DecodedMessageKind::ServerNotification,
         "turn/started",
         std::nullopt,
         {{"threadId", Value(threadId)},
          {"turn", Value(Value::Object{{"id", Value(turnId)},
                                       {"status", Value("inProgress")}})}}}));
    static_cast<void>(worker.apply(
        {DecodedMessageKind::ServerNotification,
         "item/started",
         std::nullopt,
         {{"threadId", Value(threadId)},
          {"turnId", Value(turnId)},
          {"item", Value(Value::Object{{"id", Value(itemId)},
                                       {"type", Value("agentMessage")},
                                       {"text", Value(message)}})}}}));
  };

  makeReady(worker);
  applyConversation("selected-thread", "selected-turn", "selected-item",
                    "Selected original");
  applyConversation("background-thread", "background-turn", "background-item",
                    "Background original");
  require(spinUntil([&] {
            auto *list =
                shell.findChild<QListWidget *>(QStringLiteral("threadList"));
            return threadItem(list, "selected-thread") &&
                   threadItem(list, "background-thread");
          }),
          "both selected and background graph threads reach the real shell");

  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(selectThread(list, "selected-thread"),
          "the selected conversation is bound before filtering deltas");
  static_cast<void>(takeQtMessages(channels));
  require(spinUntil([&] {
            return agentMessageCard(shell, "Selected original") != nullptr;
          }),
          "the selected conversation materializes its visible agent card");
  middle::ConversationCard *selectedCard =
      agentMessageCard(shell, "Selected original");

  NodeRef selectedItem;
  NodeRef backgroundItem;
  {
    auto read = graph.tryRead();
    selectedItem =
        read ? read->find(scopedItemNodeId(
                   scopedTurnNodeId("selected-thread", "selected-turn"),
                   "selected-item"))
             : NodeRef{};
    backgroundItem =
        read ? read->find(scopedItemNodeId(
                   scopedTurnNodeId("background-thread", "background-turn"),
                   "background-item"))
             : NodeRef{};
  }
  require(selectedItem && backgroundItem,
          "the test resolves both scoped conversation items");
  if (!selectedItem || !backgroundItem || !selectedCard)
    return;

  GraphChange withheldSelectedChange;
  {
    auto write = graph.write();
    write.setField(selectedItem, "text", Value("Selected current"));
    withheldSelectedChange = write.finish();
  }

  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "item/agentMessage/delta",
                                  std::nullopt,
                                  {{"threadId", Value("background-thread")},
                                   {"turnId", Value("background-turn")},
                                   {"itemId", Value("background-item")},
                                   {"delta", Value(" delta")}}}));
  spin(60);
  const auto *afterBackground =
      std::get_if<middle::AgentMessageData>(&selectedCard->data().payload);
  require(afterBackground && afterBackground->text == "Selected original" &&
              agentMessageCard(shell, "Selected current") == nullptr,
          "a background-only graph delta performs no selected conversation "
          "refresh or render");

  require(messageAdmitted(
              channels.sendGraphChanged(std::move(withheldSelectedChange))),
          "the withheld selected change is admitted for comparison");
  require(spinUntil([&] {
            const auto *agent = std::get_if<middle::AgentMessageData>(
                &selectedCard->data().payload);
            return agent && agent->text == "Selected current";
          }),
          "a selected-item graph delta refreshes the existing card");

  QPointer<QWidget> removedWidget = selectedCard;
  GraphChange removal;
  {
    auto write = graph.write();
    NodeState state = *write.state(selectedItem);
    state.fields.erase("protocolThreadId");
    state.fields.erase("threadId");
    write.replaceState(selectedItem, std::move(state));
    write.remove(selectedItem);
    removal = write.finish();
  }
  GraphChange backgroundWithRemoval{
      removal.revision, {backgroundItem}, std::move(removal.removed)};
  require(messageAdmitted(
              channels.sendGraphChanged(std::move(backgroundWithRemoval))),
          "the background notification carrying a removed ref is admitted");
  require(spinUntil([&] {
            return selectedItem->uiAttachment() == nullptr &&
                   removedWidget.isNull();
          }),
          "removed refs always detach matching selected widgets even when "
          "the change needs no structural refresh");
}

void optimisticDraftUsesOneTypedCreateAction(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  spin(40);

  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  require(newThread != nullptr, "the existing New thread control is available");
  if (!newThread)
    return;
  newThread->click();
  spin(40);

  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  QListWidgetItem *draft = threadItem(list, "draft:new-thread");
  QListWidgetItem *const stableDraft = draft;
  require(draft && list->currentItem() == draft,
          "the local optimistic draft is selected without a mirror model");

  static_cast<void>(worker.connectionSettings(
      {{"selected", Value("unix")}, {"endpoint", Value("local")}}));
  spin(40);
  require(draft && list->currentItem() == draft,
          "an unrelated shared-graph change preserves draft selection");

  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  const QString promptText = QStringLiteral("first exact draft prompt");
  require(submit(editor, promptText),
          "the optimistic draft submits through the real composer");
  std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  RuntimeAction create;
  std::size_t creates = 0;
  for (QtToWorkerMessage &message : messages) {
    if (auto *action = std::get_if<RuntimeAction>(&message);
        action && action->kind == RuntimeActionKind::CreateThread) {
      create = std::move(*action);
      ++creates;
    }
  }
  const auto threadStart = create.payload.find("threadStart");
  const auto turnStart = create.payload.find("turnStart");
  require(
      creates == 1 && create.promptText == promptText.toStdString() &&
          threadStart != create.payload.end() &&
          threadStart->second.asObject() && turnStart != create.payload.end() &&
          turnStart->second.asObject(),
      "the draft emits one typed CreateThread with owned prompt and options");

  PromptTransition transition = worker.admitFirstPrompt(std::move(create));
  require(
      transition.command &&
          transition.command->kind == PromptCommandKind::CreateThread,
      "worker admission creates the local thread/turn/prompt graph atomically");
  spin(60);
  const NodeRef graphDraft =
      transition.command ? transition.command->thread : NodeRef{};
  require(
      graphDraft && list && list->currentItem() == stableDraft &&
          list->currentItem()->data(Qt::UserRole).toString().toStdString() ==
              graphDraft->id().canonical &&
          localPromptCard(shell, promptText.toStdString()),
      "the same optimistic row hands off to the selected shared graph draft");

  if (!transition.command)
    return;
  const NodeRef localPrompt = transition.command->localPrompt;
  require(worker.attachCreatedThread(*transition.command, "created-thread") ==
              ChannelSendStatus::Accepted,
          "the worker attaches the accepted canonical thread exactly once");
  spin(60);
  middle::ThreadPane *threadPane = nullptr;
  for (QWidget *ancestor = list; ancestor && !threadPane;
       ancestor = ancestor->parentWidget())
    threadPane = dynamic_cast<middle::ThreadPane *>(ancestor);
  require(threadItem(list, "created-thread") == stableDraft &&
              list->currentItem() == stableDraft && threadPane &&
              threadPane->isOptimisticThread("created-thread"),
          "the same row is promoted from local to canonical identity");

  static_cast<void>(
      worker.completePrompt(localPrompt, true, {}, "created-turn"));
  spin(60);
  require(threadItem(list, "created-thread") == stableDraft && threadPane &&
              !threadPane->isOptimisticThread("created-thread"),
          "the exact prompt result confirms the canonical row without "
          "replacing its widget item");
}

void emptyOptimisticDraftIsAbandonedOnThreadSelection(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "existing-after-empty-draft", "Existing thread");
  spin(40);

  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  require(newThread && list,
          "empty-draft abandonment fixture exposes the thread controls");
  if (!newThread || !list)
    return;
  newThread->click();
  require(spinUntil([&] {
            return threadItem(list, "draft:new-thread") == list->currentItem();
          }),
          "an empty local New Thread draft starts as the selected row");

  require(selectThread(list, "existing-after-empty-draft") &&
              spinUntil([&] { return !threadItem(list, "draft:new-thread"); }),
          "selecting a real thread abandons and removes an unsubmitted local "
          "draft");

  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  newThread->click();
  require(spinUntil([&] {
            return threadItem(list, "draft:new-thread") == list->currentItem();
          }),
          "abandonment clears creation state so a later New Thread can start");
}

void secondNewThreadIsGuardedWhileCreationIsInFlight(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  spin(40);

  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  require(newThread && list && editor,
          "in-flight creation fixture exposes New Thread and the composer");
  if (!newThread || !list || !editor)
    return;
  newThread->click();
  require(spinUntil([&] {
            QListWidgetItem *draft = threadItem(list, "draft:new-thread");
            return draft && draft == list->currentItem() && draft->isSelected();
          }) &&
              submit(editor, QStringLiteral("one creation in flight")),
          "the first new-thread action is admitted from its optimistic row");

  std::optional<RuntimeAction> create;
  for (QtToWorkerMessage &message : takeQtMessages(channels)) {
    if (auto *action = std::get_if<RuntimeAction>(&message);
        action && action->kind == RuntimeActionKind::CreateThread)
      create = std::move(*action);
  }
  require(create.has_value(),
          "the first submission emits one worker-owned creation action");
  if (!create)
    return;

  bool secondDialogOpened = false;
  QTimer::singleShot(0, &shell, [&secondDialogOpened] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      secondDialogOpened = true;
      dialog->accept();
    }
  });
  newThread->click();
  spin(30);
  require(!secondDialogOpened &&
              threadItem(list, "draft:new-thread") == list->currentItem(),
          "New Thread does not replace the correlation or optimistic row "
          "while creation is in flight");

  PromptTransition transition = worker.admitFirstPrompt(std::move(*create));
  require(transition.command && spinUntil([&] {
            return threadItem(list,
                              transition.command->thread->id().canonical) !=
                   nullptr;
          }),
          "the original in-flight correlation still promotes its exact "
          "optimistic row after a guarded second click");
}

void optimisticCreationDoesNotOverrideLaterNavigation(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "navigation-thread", "Chosen after creation");
  spin(40);

  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  if (!newThread)
    return;
  newThread->click();
  spin(30);

  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  QListWidgetItem *const draft = threadItem(list, "draft:new-thread");
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  require(draft && submit(editor, QStringLiteral("create in background")),
          "navigation race admits the selected optimistic draft");
  std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  std::optional<RuntimeAction> create;
  for (QtToWorkerMessage &message : messages) {
    if (auto *action = std::get_if<RuntimeAction>(&message);
        action && action->kind == RuntimeActionKind::CreateThread)
      create = std::move(*action);
  }
  require(create.has_value() && selectThread(list, "navigation-thread"),
          "the user navigates away before worker creation effects arrive");
  if (!create)
    return;

  PromptTransition transition = worker.admitFirstPrompt(std::move(*create));
  spin(50);
  require(transition.command &&
              list->currentItem() == threadItem(list, "navigation-thread") &&
              threadItem(list, transition.command->thread->id().canonical) ==
                  draft,
          "the delayed local handoff promotes its row without stealing the "
          "newer selection");
  if (!transition.command)
    return;

  const NodeRef localPrompt = transition.command->localPrompt;
  static_cast<void>(
      worker.attachCreatedThread(*transition.command, "background-created"));
  spin(50);
  require(list->currentItem() == threadItem(list, "navigation-thread") &&
              threadItem(list, "background-created") == draft,
          "canonical creation completion preserves both row identity and "
          "later navigation");
  static_cast<void>(
      worker.completePrompt(localPrompt, true, {}, "background-turn"));
  spin(50);
  require(list->currentItem() == threadItem(list, "navigation-thread") &&
              threadItem(list, "background-created") == draft,
          "prompt acknowledgement confirms the background row without a "
          "selection override");
}

void optimisticCreationIgnoresAnotherCreationCorrelation(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  spin(40);

  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  require(newThread != nullptr,
          "the creation-correlation fixture exposes New thread");
  if (!newThread)
    return;
  newThread->click();
  spin(30);

  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  QListWidgetItem *const foregroundDraft = threadItem(list, "draft:new-thread");
  require(foregroundDraft && list->currentItem() == foregroundDraft,
          "the foreground optimistic draft begins selected");
  if (!foregroundDraft)
    return;

  RuntimeAction background;
  background.kind = RuntimeActionKind::CreateThread;
  background.correlation = "different-creation-correlation";
  background.promptText = "background creation prompt";
  background.payload.emplace(
      "threadStart",
      Value(Value::Object{{"cwd", Value("/tmp")},
                          {"name", Value("Different creation")}}));
  background.payload.emplace("turnStart", Value(Value::Object{}));
  PromptTransition transition = worker.admitFirstPrompt(std::move(background));
  require(transition.command.has_value(),
          "the worker admits a distinct correlated background creation");
  if (!transition.command)
    return;

  const std::string backgroundThreadId =
      transition.command->thread->id().canonical;
  require(spinUntil(
              [&] { return threadItem(list, backgroundThreadId) != nullptr; }),
          "the differently correlated worker draft is visible in the list");
  require(list->currentItem() == foregroundDraft &&
              list->currentItem()->data(Qt::UserRole).toString() ==
                  QStringLiteral("draft:new-thread") &&
              threadItem(list, backgroundThreadId) != foregroundDraft,
          "a worker SelectThread for another creation correlation cannot "
          "hijack the foreground optimistic selection");
}

void saturatedShellKeepsTheEditorDraft(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "saturated-shell", "Saturated shell");
  require(spinUntil([&] {
            return threadItem(shell.findChild<QListWidget *>(
                                  QStringLiteral("threadList")),
                              "saturated-shell") != nullptr;
          }),
          "saturation fixture renders its shared thread");
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(selectThread(list, "saturated-shell"),
          "saturation fixture selects its graph thread");
  static_cast<void>(takeQtMessages(channels));

  std::size_t admissions = 0;
  for (;;) {
    RuntimeAction filler;
    filler.kind = RuntimeActionKind::RefreshCatalogs;
    filler.correlation = "shell-fill-" + std::to_string(admissions);
    const ChannelSendStatus status = session.sendRuntimeAction(filler);
    if (status == ChannelSendStatus::QueueFull)
      break;
    ++admissions;
  }

  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  const QString retained = QStringLiteral("retain after visible rejection");
  require(submit(editor, retained),
          "the enabled composer reaches the saturated typed boundary");
  spin(20);
  require(editor && editor->toPlainText() == retained,
          "a rejected prompt remains editable in the existing composer");

  const std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  const bool hasPrompt = std::ranges::any_of(messages, [](const auto &message) {
    const auto *action = std::get_if<NodeAction>(&message);
    return action && action->kind == NodeActionKind::SubmitPrompt;
  });
  require(
      messages.size() == admissions && !hasPrompt,
      "visible rejection neither admits nor retries the non-idempotent prompt");
}

void saturatedRenameRetainsAuthoredName(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "rename-retention", "Original name");
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(spinUntil(
              [&] { return threadItem(list, "rename-retention") != nullptr; }),
          "rename-retention fixture renders its shared thread");
  static_cast<void>(takeQtMessages(channels));

  std::size_t fillers = 0;
  for (;;) {
    RuntimeAction filler;
    filler.kind = RuntimeActionKind::RefreshCatalogs;
    filler.correlation = "rename-fill-" + std::to_string(fillers);
    const ChannelSendStatus status = session.sendRuntimeAction(filler);
    if (status == ChannelSendStatus::QueueFull)
      break;
    ++fillers;
  }

  const auto openRename = [&]() -> QAction * {
    const QPoint point =
        list->visualItemRect(threadItem(list, "rename-retention")).center();
    QMetaObject::invokeMethod(list, "customContextMenuRequested",
                              Qt::DirectConnection, Q_ARG(QPoint, point));
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (menu)
      for (QAction *action : menu->actions())
        if (action && action->text() == QStringLiteral("Rename"))
          return action;
    return nullptr;
  };

  const QString exact = QStringLiteral("Exact queued rename");
  QAction *rename = openRename();
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    auto *edit = dialog ? dialog->findChild<QLineEdit *>() : nullptr;
    if (edit) {
      edit->setText(exact);
      dialog->accept();
    }
  });
  if (rename)
    rename->trigger();
  const std::vector<QtToWorkerMessage> rejected = takeQtMessages(channels);
  require(rename && rejected.size() == fillers &&
              std::ranges::none_of(
                  rejected,
                  [](const auto &message) {
                    const auto *action = std::get_if<NodeAction>(&message);
                    return action && action->kind == NodeActionKind::Rename;
                  }),
          "a saturated rename is visibly rejected without being queued");

  bool prefilled = false;
  rename = openRename();
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    auto *edit = dialog ? dialog->findChild<QLineEdit *>() : nullptr;
    prefilled = edit && edit->text() == exact;
    if (dialog)
      dialog->accept();
  });
  if (rename)
    rename->trigger();
  const std::vector<QtToWorkerMessage> admitted = takeQtMessages(channels);
  const auto action = std::ranges::find_if(admitted, [](const auto &message) {
    const auto *candidate = std::get_if<NodeAction>(&message);
    return candidate && candidate->kind == NodeActionKind::Rename;
  });
  bool exactPayload = false;
  if (action != admitted.end()) {
    const auto &renameAction = std::get<NodeAction>(*action);
    const auto name = renameAction.payload.find("name");
    exactPayload = name != renameAction.payload.end() &&
                   name->second.asString() &&
                   *name->second.asString() == exact.toStdString();
  }
  require(rename && prefilled && admitted.size() == 1 && exactPayload,
          "reopening Rename recovers the authored name and deliberately "
          "admits it exactly once");
}

void saturatedWorkerEffectsKeepNewestUiState(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "effect-first", "First selection");
  applyThread(worker, "effect-second", "Newest selection");
  require(spinUntil([&] {
            auto *list =
                shell.findChild<QListWidget *>(QStringLiteral("threadList"));
            return threadItem(list, "effect-first") &&
                   threadItem(list, "effect-second") &&
                   channels.workerToQtSizeApprox() == 0;
          }),
          "effect-saturation fixture renders both shared threads");

  NodeRef first;
  NodeRef second;
  {
    auto read = graph.tryRead();
    first = read->find({NodeKind::Thread, "effect-first"});
    second = read->find({NodeKind::Thread, "effect-second"});
  }
  std::size_t oldNotices = 0;
  while (channels.workerToQtSizeApprox() <
         ThreadChannels::WorkerToQtCapacity -
             ThreadChannels::WorkerToQtReservedSlots) {
    require(worker.showNotice("old notice " + std::to_string(oldNotices)) ==
                ChannelSendStatus::Accepted,
            "ordinary sequenced notice fills only ordinary queue capacity");
    ++oldNotices;
  }
  require(worker.showNotice("newest visible notice") ==
                  ChannelSendStatus::CoalescedRescan &&
              worker.selectThread(first) == ChannelSendStatus::Accepted &&
              worker.selectThread(second) == ChannelSendStatus::CoalescedRescan,
          "saturated effects retain a graph fallback after the critical "
          "selection slot is used");

  require(
      spinUntil(
          [&] {
            auto *list =
                shell.findChild<QListWidget *>(QStringLiteral("threadList"));
            auto *bar = shell.findChild<QFrame *>(
                QStringLiteral("conversationNoticeBar"));
            const auto labels =
                bar ? bar->findChildren<QLabel *>() : QList<QLabel *>{};
            return channels.workerToQtSizeApprox() == 0 &&
                   !channels.rescanPending() && list &&
                   list->currentItem() == threadItem(list, "effect-second") &&
                   !labels.empty() &&
                   labels.front()->text() ==
                       QStringLiteral("newest visible notice");
          },
          3000),
      "Qt ignores older queued effects after reconstructing the newest "
      "sequenced graph fallback");
}

void saturatedInteractionRetainsAuthoredInput(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "retained-input", "Retained input");
  static_cast<void>(worker.applyDetailed(
      {DecodedMessageKind::ServerRequest,
       "item/tool/requestUserInput",
       ProtocolRequestId("retained-question"),
       {{"threadId", Value("retained-input")},
        {"turnId", Value("retained-turn")},
        {"itemId", Value("retained-item")},
        {"questions", Value(Value::Array{Value(Value::Object{
                          {"id", Value("answer")},
                          {"question", Value("What should be retained?")},
                          {"options", Value(Value::Array{})}})})}}}));

  auto *review = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestReviewButton"));
  require(spinUntil([&] {
            return review && review->isVisible() && review->isEnabled();
          }),
          "user-input request exposes the existing Review action");
  if (!review)
    return;

  std::size_t fillers = 0;
  for (;;) {
    RuntimeAction filler;
    filler.kind = RuntimeActionKind::RefreshCatalogs;
    filler.correlation = "input-fill-" + std::to_string(fillers);
    const ChannelSendStatus status = session.sendRuntimeAction(filler);
    if (status == ChannelSendStatus::QueueFull)
      break;
    ++fillers;
  }

  const QString exact = QStringLiteral("exact answer retained at saturation");
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    const auto edits = dialog->findChildren<QLineEdit *>();
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    if (edits.size() == 1 && buttons) {
      edits.front()->setText(exact);
      buttons->button(QDialogButtonBox::Ok)->click();
    }
  });
  review->click();
  spin();
  const std::vector<QtToWorkerMessage> rejected = takeQtMessages(channels);
  require(rejected.size() == fillers &&
              std::ranges::none_of(
                  rejected,
                  [](const auto &message) {
                    const auto *action = std::get_if<NodeAction>(&message);
                    return action &&
                           action->kind == NodeActionKind::ResolveInteraction;
                  }),
          "a saturated response is not queued or retried automatically");

  bool prefilled = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    const auto edits = dialog->findChildren<QLineEdit *>();
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    prefilled = edits.size() == 1 && edits.front()->text() == exact;
    if (buttons)
      buttons->button(QDialogButtonBox::Ok)->click();
  });
  review->click();
  const std::vector<QtToWorkerMessage> admitted = takeQtMessages(channels);
  const auto response = std::ranges::find_if(admitted, [](const auto &message) {
    const auto *action = std::get_if<NodeAction>(&message);
    return action && action->kind == NodeActionKind::ResolveInteraction;
  });
  bool exactPayload = false;
  if (response != admitted.end()) {
    const auto &action = std::get<NodeAction>(*response);
    const auto answers = action.payload.find("answers");
    if (answers != action.payload.end() && answers->second.asObject()) {
      const auto answer = answers->second.asObject()->find("answer");
      if (answer != answers->second.asObject()->end() &&
          answer->second.asObject()) {
        const auto values = answer->second.asObject()->find("answers");
        exactPayload = values != answer->second.asObject()->end() &&
                       values->second.asArray() &&
                       values->second.asArray()->size() == 1 &&
                       values->second.asArray()->front().asString() &&
                       *values->second.asArray()->front().asString() ==
                           exact.toStdString();
      }
    }
  }
  require(prefilled && admitted.size() == 1 && exactPayload,
          "reopening Review recovers the exact authored input and a deliberate "
          "submit admits it once");
}

void recoveryOnlyPromptRestoresToComposer(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "recovery-source", "Recovery source");
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(spinUntil(
              [&] { return threadItem(list, "recovery-source") != nullptr; }) &&
              selectThread(list, "recovery-source"),
          "recovery fixture selects its original provider thread");
  static_cast<void>(takeQtMessages(channels));

  const QString exact = QStringLiteral("preserve this exact unsent prompt");
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  require(submit(editor, exact),
          "recovery fixture admits the prompt into the typed mailbox");
  std::vector<QtToWorkerMessage> authored = takeQtMessages(channels);
  auto promptAction = std::ranges::find_if(authored, [](const auto &message) {
    const auto *action = std::get_if<NodeAction>(&message);
    return action && action->kind == NodeActionKind::SubmitPrompt;
  });
  if (promptAction == authored.end()) {
    require(false, "recovery fixture receives the admitted prompt action");
    return;
  }
  PromptTransition transition =
      worker.admitPrompt(std::move(std::get<NodeAction>(*promptAction)));
  require(transition.command.has_value(),
          "worker starts one provider-bound prompt command");

  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "thread/deleted",
                                  std::nullopt,
                                  {{"threadId", Value("recovery-source")}}}));
  std::string recoveryId;
  require(spinUntil([&] {
            for (int row = 0; list && row < list->count(); ++row) {
              const std::string id =
                  list->item(row)->data(Qt::UserRole).toString().toStdString();
              if (id.starts_with("local-recovery-thread:")) {
                recoveryId = id;
                return true;
              }
            }
            return false;
          }),
          "thread deletion exposes a recovery-only local thread");
  static_cast<void>(takeQtMessages(channels));
  require(!recoveryId.empty() && selectThread(list, recoveryId),
          "the recovery-only thread can be inspected without hydration");
  const std::vector<QtToWorkerMessage> inspectionActions =
      takeQtMessages(channels);
  const bool hydratedRecovery =
      std::ranges::any_of(inspectionActions, [](const auto &message) {
        const auto *action = std::get_if<NodeAction>(&message);
        return action && action->kind == NodeActionKind::Hydrate;
      });
  auto *send =
      shell.findChild<QPushButton *>(QStringLiteral("composerSendButton"));
  QPushButton *restore = nullptr;
  require(spinUntil([&] {
            restore = shell.findChild<QPushButton *>(
                QStringLiteral("promptRecoveryButton"));
            return restore && restore->isVisible();
          }) &&
              send && !send->isEnabled() && !hydratedRecovery,
          "recovery-only selection disables normal send and shows one explicit "
          "restore action");
  if (!restore)
    return;

  middle::ComposerPane *composer = nullptr;
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    composer = dynamic_cast<middle::ComposerPane *>(widget);
    if (composer)
      break;
  }
  const QString existingDraft =
      QStringLiteral("keep this newer composer draft unchanged");
  const std::vector<AttachmentDraft> existingAttachments{
      {"/tmp/newer-draft.txt", "newer-draft.txt", "text/plain", 23}};
  if (editor)
    editor->setPlainText(existingDraft);
  if (composer)
    composer->setAttachments(existingAttachments);
  restore->click();
  spin(40);
  const std::vector<QtToWorkerMessage> guardedRestore =
      takeQtMessages(channels);
  require(composer && editor && editor->toPlainText() == existingDraft &&
              composer->attachments() == existingAttachments &&
              list->currentItem() == threadItem(list, recoveryId) &&
              restore->isVisible() && guardedRestore.empty(),
          "Restore leaves a newer composer draft and its attachments intact");
  if (!composer || !editor)
    return;
  editor->clear();
  composer->setAttachments({});

  restore->click();
  spin(40);
  const std::vector<QtToWorkerMessage> afterRestore = takeQtMessages(channels);
  const bool sentAutomatically =
      std::ranges::any_of(afterRestore, [](const auto &message) {
        if (const auto *action = std::get_if<NodeAction>(&message))
          return action->kind == NodeActionKind::SubmitPrompt;
        if (const auto *action = std::get_if<RuntimeAction>(&message))
          return action->kind == RuntimeActionKind::CreateThread;
        return false;
      });
  require(editor && editor->toPlainText() == exact &&
              threadItem(list, "draft:new-thread") == list->currentItem() &&
              send && send->isEnabled() && !sentAutomatically,
          "Restore moves the exact text into a new-thread composer draft "
          "without sending it");
}

void providerNoticesReachTheTransientSurface(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "notice-thread", "Notice thread");
  spin(40);

  auto *bar =
      shell.findChild<QFrame *>(QStringLiteral("conversationNoticeBar"));
  auto noticeText = [bar] {
    const auto labels = bar ? bar->findChildren<QLabel *>() : QList<QLabel *>{};
    return labels.empty() ? QString{} : labels.front()->text();
  };

  static_cast<void>(
      worker.apply({DecodedMessageKind::ServerNotification,
                    "warning",
                    std::nullopt,
                    {{"message", Value("Provider warning is visible")}}}));
  require(spinUntil([&] {
            return bar && bar->isVisible() &&
                   noticeText() ==
                       QStringLiteral("Provider warning is visible");
          }) &&
              bar->property("tone").toString() == QStringLiteral("warning"),
          "provider warnings use the canonical non-error notice surface");

  static_cast<void>(
      worker.apply({DecodedMessageKind::ServerNotification,
                    "error",
                    std::nullopt,
                    {{"threadId", Value("notice-thread")},
                     {"message", Value("Provider error is visible")}}}));
  require(spinUntil([&] {
            return noticeText() == QStringLiteral("Provider error is visible");
          }) &&
              bar->property("tone").toString() == QStringLiteral("danger"),
          "provider errors use the canonical error notice surface");

  auto read = graph.tryRead();
  const NodeRef notice =
      read ? read->find({NodeKind::Notice, "provider-notice"}) : NodeRef{};
  const NodeRef thread =
      read ? read->find({NodeKind::Thread, "notice-thread"}) : NodeRef{};
  require(notice && thread &&
              stringFieldEquals(read->state(notice), "method", "error") &&
              !field(read->state(thread), "message"),
          "provider notice state stays isolated from addressed thread facts");
}

void failedHydrationKeepsTheEditorDraft(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "failed-hydration", "Needs reload");
  require(spinUntil([&] {
            return threadItem(shell.findChild<QListWidget *>(
                                  QStringLiteral("threadList")),
                              "failed-hydration") != nullptr;
          }),
          "failed hydration fixture renders its graph thread");
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(selectThread(list, "failed-hydration"),
          "failed hydration fixture selects its destination");
  static_cast<void>(takeQtMessages(channels));
  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "failed-hydration"});
  }
  static_cast<void>(
      worker.threadHydration(thread, "failed", "Hydration failed"));
  spin(40);

  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  auto *send =
      shell.findChild<QPushButton *>(QStringLiteral("composerSendButton"));
  const QString retained = QStringLiteral("retain until Reload succeeds");
  editor->setPlainText(retained);
  spin();
  const bool invoked = QMetaObject::invokeMethod(editor, "submitRequested",
                                                 Qt::DirectConnection);
  const std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  require(invoked && send && !send->isEnabled() &&
              editor->toPlainText() == retained && messages.empty(),
          "failed hydration disables admission without clearing or queueing "
          "the user-owned draft");
}

void reverseInteractionCarriesOnlyAuthoredResponse(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "approval-thread", "Approval thread");
  require(spinUntil([&] {
            return threadItem(shell.findChild<QListWidget *>(
                                  QStringLiteral("threadList")),
                              "approval-thread") != nullptr;
          }),
          "reverse-interaction fixture renders its target thread");
  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  require(selectThread(list, "approval-thread"),
          "pending interaction is scoped to the selected graph thread");
  static_cast<void>(takeQtMessages(channels));

  const WorkerApplyResult request =
      worker.applyDetailed({DecodedMessageKind::ServerRequest,
                            "item/commandExecution/requestApproval",
                            ProtocolRequestId("approval-typed"),
                            {{"threadId", Value("approval-thread")},
                             {"turnId", Value("approval-turn")},
                             {"itemId", Value("approval-item")},
                             {"command", Value("echo secret protocol fact")},
                             {"cwd", Value("/provider/cwd")}}});
  require(request.primary != nullptr,
          "decoded server request creates one pending interaction node");
  auto *accept = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestAcceptButton"));
  require(spinUntil([&] {
            return accept && accept->isVisible() && accept->isEnabled();
          }),
          "controller sees the existing actionable approval UI");
  if (!accept)
    return;
  accept->click();

  const std::vector<QtToWorkerMessage> responses = takeQtMessages(channels);
  const NodeAction *response = nullptr;
  std::size_t responseCount = 0;
  for (const QtToWorkerMessage &message : responses) {
    const auto *candidate = std::get_if<NodeAction>(&message);
    if (candidate && candidate->kind == NodeActionKind::ResolveInteraction) {
      response = candidate;
      ++responseCount;
    }
  }
  const auto decision = response ? response->payload.find("decision")
                                 : Value::Object::const_iterator{};
  require(
      responseCount == 1 && response && response->target == request.primary &&
          response->payload.size() == 1 &&
          decision != response->payload.end() && decision->second.asString() &&
          *decision->second.asString() == "accept" &&
          !response->payload.contains("command") &&
          !response->payload.contains("cwd") && response->promptText.empty() &&
          response->attachments.empty(),
      "approval sends one typed action containing only authored decision data");

  static_cast<void>(worker.resolveInteraction(
      request.primary, false, "CodexBridge rejected the response"));
  spin(40);
  const std::vector<QtToWorkerMessage> automaticRetry =
      takeQtMessages(channels);
  require(automaticRetry.empty() && accept->isVisible() && accept->isEnabled(),
          "a rejected bridge response stays visibly actionable without an "
          "automatic retry");
  accept->click();
  const std::vector<QtToWorkerMessage> deliberateRetry =
      takeQtMessages(channels);
  const std::size_t deliberateCount =
      std::ranges::count_if(deliberateRetry, [&](const auto &message) {
        const auto *candidate = std::get_if<NodeAction>(&message);
        return candidate &&
               candidate->kind == NodeActionKind::ResolveInteraction &&
               candidate->target == request.primary;
      });
  require(deliberateCount == 1,
          "the user can deliberately re-author one response after bridge "
          "rejection");
}

void pendingRequestTextBoundaries() {
  bool inspected = false;
  bool plainText = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    inspected = true;
    const auto labels = dialog->findChildren<QLabel *>();
    const auto command = std::ranges::find_if(labels, [](QLabel *label) {
      return label &&
             label->text().contains(QStringLiteral("<b>untrusted command</b>"));
    });
    plainText =
        command != labels.end() && (*command)->textFormat() == Qt::PlainText;
    dialog->reject();
  });
  const PendingRequestDescriptor command{
      "unsafe-command",
      "command-approval",
      "thread-a",
      1,
      {{"command", "<b>untrusted command</b>"}}};
  static_cast<void>(PendingRequestDialog::present(command, nullptr));

  bool escapedLink = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    escapedLink = std::ranges::any_of(
        dialog->findChildren<QLabel *>(), [](QLabel *label) {
          return label && label->textFormat() == Qt::RichText &&
                 label->text().contains(QStringLiteral("&lt;img")) &&
                 !label->text().contains(QStringLiteral("<img"));
        });
    dialog->reject();
  });
  const PendingRequestDescriptor elicitation{
      "unsafe-link",
      "mcp-elicitation",
      "thread-a",
      1,
      {{"url", "https://example.invalid/\"><img src=x>"}}};
  static_cast<void>(PendingRequestDialog::present(elicitation, nullptr));

  require(inspected && plainText,
          "server-provided request text is rendered literally");
  require(escapedLink, "the explicit MCP link escapes untrusted markup");
}

void pendingRequestValidationRetainsInput() {
  bool incompleteWarning = false;
  bool questionDialogRetained = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    const auto edits = dialog->findChildren<QLineEdit *>();
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    auto *submitButton =
        buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
    if (edits.size() != 2 || !submitButton)
      return;
    edits.front()->setText(QStringLiteral("Retained first answer"));
    QTimer::singleShot(0, [&] {
      auto *warning =
          qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      incompleteWarning = warning && warning->windowTitle() ==
                                         QStringLiteral("Incomplete response");
      if (warning)
        warning->done(QMessageBox::Ok);
    });
    submitButton->click();
    questionDialogRetained =
        dialog->isVisible() &&
        edits.front()->text() == QStringLiteral("Retained first answer");
    edits.back()->setText(QStringLiteral("Second answer"));
    submitButton->click();
  });
  const PendingRequestDescriptor questions{
      "questions",
      "user-input",
      "thread-a",
      1,
      {{"questions",
        nlohmann::json::array({{{"id", "first"},
                                {"question", "First?"},
                                {"options", nlohmann::json::array()}},
                               {{"id", "second"},
                                {"question", "Second?"},
                                {"options", nlohmann::json::array()}}})}}};
  const auto questionResponse =
      PendingRequestDialog::present(questions, nullptr);
  const bool answersPreserved =
      questionResponse &&
      questionResponse->result["answers"]["first"]["answers"] ==
          nlohmann::json::array({"Retained first answer"}) &&
      questionResponse->result["answers"]["second"]["answers"] ==
          nlohmann::json::array({"Second answer"});

  bool invalidJsonWarning = false;
  bool mcpDialogRetained = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    auto *editor = dialog->findChild<QPlainTextEdit *>();
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    auto *submitButton =
        buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
    if (!editor || !submitButton)
      return;
    editor->setPlainText(QStringLiteral("["));
    QTimer::singleShot(0, [&] {
      auto *warning =
          qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      invalidJsonWarning = warning && warning->windowTitle() ==
                                          QStringLiteral("Invalid response");
      if (warning)
        warning->done(QMessageBox::Ok);
    });
    submitButton->click();
    mcpDialogRetained =
        dialog->isVisible() && editor->toPlainText() == QStringLiteral("[");
    editor->setPlainText(QStringLiteral("{\"accepted\":true}"));
    submitButton->click();
  });
  const PendingRequestDescriptor elicitation{
      "elicitation",
      "mcp-elicitation",
      "thread-a",
      1,
      {{"message", "Structured response"},
       {"requestedSchema", {{"type", "object"}}}}};
  const auto mcpResponse = PendingRequestDialog::present(elicitation, nullptr);
  const bool validJsonReturned =
      mcpResponse &&
      mcpResponse->result.value("action", std::string{}) == "accept" &&
      mcpResponse->result["content"] == nlohmann::json({{"accepted", true}});

  require(incompleteWarning && questionDialogRetained && answersPreserved,
          "incomplete questions retain the modal and prior authored answers");
  require(
      invalidJsonWarning && mcpDialogRetained && validJsonReturned,
      "invalid MCP JSON remains editable until valid authored input exists");
}

void permissionRequestDisclosure() {
  const nlohmann::json permissions = {
      {"fileSystem",
       {{"write", nlohmann::json::array({"/tmp/<untrusted>"})},
        {"entries",
         nlohmann::json::array(
             {{{"access", "read"},
               {"path", {{"type", "glob_pattern"}, {"pattern", "*.md"}}}}})}}},
      {"network", {{"enabled", true}}},
      {"futureCapability", {{"mode", "bounded"}}}};
  bool completeDisclosure = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    QStringList displayed;
    for (QLabel *label : dialog->findChildren<QLabel *>())
      if (label)
        displayed.push_back(label->text());
    const QString all = displayed.join(QLatin1Char('\n'));
    completeDisclosure =
        all.contains(QStringLiteral("File system / write / 1: "
                                    "/tmp/<untrusted>")) &&
        all.contains(QStringLiteral("Network / enabled: Yes")) &&
        all.contains(QStringLiteral("futureCapability / mode: bounded"));
    dialog->accept();
  });
  const PendingRequestDescriptor request{
      "permissions",
      "permissions-approval",
      "thread-a",
      1,
      {{"permissions", permissions}, {"reason", "test disclosure"}}};
  const auto response = PendingRequestDialog::present(request, nullptr);
  require(completeDisclosure,
          "permission approval discloses known and future request fields");
  require(response && response->error.is_null() &&
              response->result.value("permissions", nlohmann::json{}) ==
                  permissions &&
              response->result.value("scope", std::string{}) == "turn",
          "permission approval returns the exact disclosed permissions object");
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  auto *configuration =
      utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
  QApplication application(argc, argv);
  core::SNodeC::init(argc, argv);

  using namespace codexui::codex;
  graphNotificationsDetachBeforeRetirement(*configuration);
  massRetirementIsSliced(*configuration);
  selectedRemovalUnbindsBeforeWorkerRetirement(*configuration);
  removedAffectedOptimisticRetryDoesNotReadReleasedNode(*configuration);
  typedActionsAreExactOnceAndBounded(*configuration);
  qtHeartbeatSurvivesLargeInboundTraffic(*configuration);
  graphBackedShellPreservesDraftsAndPrompts(*configuration);
  inactiveThreadNeverReactivatesAStaleTurn(*configuration);
  reloadAndReconnectHydrationStayExplicit(*configuration);
  backgroundGraphChangesDoNotRefreshSelectedConversation(*configuration);
  optimisticDraftUsesOneTypedCreateAction(*configuration);
  emptyOptimisticDraftIsAbandonedOnThreadSelection(*configuration);
  secondNewThreadIsGuardedWhileCreationIsInFlight(*configuration);
  optimisticCreationDoesNotOverrideLaterNavigation(*configuration);
  optimisticCreationIgnoresAnotherCreationCorrelation(*configuration);
  saturatedShellKeepsTheEditorDraft(*configuration);
  saturatedRenameRetainsAuthoredName(*configuration);
  saturatedWorkerEffectsKeepNewestUiState(*configuration);
  saturatedInteractionRetainsAuthoredInput(*configuration);
  recoveryOnlyPromptRestoresToComposer(*configuration);
  providerNoticesReachTheTransientSurface(*configuration);
  failedHydrationKeepsTheEditorDraft(*configuration);
  reverseInteractionCarriesOnlyAuthoredResponse(*configuration);
  pendingRequestTextBoundaries();
  pendingRequestValidationRetainsInput();
  permissionRequestDisclosure();

  if (failures != 0) {
    std::cerr << failures << " shell integration assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "Shell typed nodegraph integration test passed\n";
  return EXIT_SUCCESS;
}
