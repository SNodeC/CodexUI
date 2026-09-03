// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include <core/SNodeC.h>
#include <utils/Config.h>

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/PendingRequestDialog.h"
#include "codex/ShellWidget.h"
#include "codex/middle/ConversationCards.h"
#include "codex/nodegraph/WorkerLogic.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
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
  require(fillerCount + 1 == ThreadChannels::WorkerToQtCapacity,
          "worker mailbox saturation preserves its terminal-message slot");

  const ChannelSendStatus coalescedStatus =
      worker.apply({DecodedMessageKind::ServerNotification,
                    "thread/deleted",
                    std::nullopt,
                    {{"threadId", Value("coalesced-removal")}}});
  require(coalescedStatus == ChannelSendStatus::CoalescedRescan,
          "a saturated graph notification becomes an explicit rescan");
  require(spinUntil([&] {
            return detachedDuringRescan &&
                   channels.qtToWorkerSizeApprox() != 0 &&
                   channels.workerToQtSizeApprox() == 0;
          }),
          "Qt drains the coalesced rescan and detaches its retired node");

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

  std::atomic_bool producerFinished = false;
  std::atomic_bool midpointReady = false;
  std::atomic_bool abortWait = false;
  std::atomic_bool heartbeatObservedAtMidpoint = false;
  std::atomic_size_t coalescedNotifications = 0;
  std::atomic_uint64_t heartbeatCount = 0;
  std::uint64_t heartbeatsWithBacklog = 0;
  std::size_t deliveredGraphChanges = 0;
  session.setGraphChangedHandler(
      [&](const GraphChanged &) { ++deliveredGraphChanges; });

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
         "thread/started",
         std::nullopt,
         {{"thread", Value(Value::Object{{"id", Value("traffic-thread")}})}}});
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
  heartbeat.stop();

  std::string streamedText;
  {
    auto read = session.nodeGraph().tryRead();
    const NodeRef item =
        read ? read->find({NodeKind::Item, "traffic-item"}) : NodeRef{};
    const Value *textValue = item ? field(read->state(item), "text") : nullptr;
    if (textValue && textValue->asString())
      streamedText = *textValue->asString();
  }
  require(producerFinished.load(std::memory_order_acquire) && drained &&
              heartbeatObservedAtMidpoint.load(std::memory_order_acquire) &&
              heartbeatCount.load(std::memory_order_acquire) > 1 &&
              heartbeatsWithBacklog > 0,
          "Qt heartbeat continues while bounded inbound notifications drain");
  require(streamedText.size() == DeltaCount,
          "large inbound traffic leaves the complete current node state");
  require(deliveredGraphChanges != 0,
          "large inbound traffic delivers graph work to Qt");
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
              prompt.promptText == exact.toStdString() && editor &&
              editor->toPlainText().isEmpty(),
          "the composer emits one typed prompt with exact authored text");

  PromptTransition transition = worker.admitPrompt(std::move(prompt));
  const NodeRef localPrompt =
      transition.command ? transition.command->localPrompt : NodeRef{};
  require(
      localPrompt != nullptr,
      "the worker turns an admitted action into the one shared prompt node");
  require(spinUntil([&] {
            return localPromptCard(shell, exact.toStdString()) != nullptr;
          }),
          "the visible existing card renders directly from the prompt node");
  middle::ConversationCard *card = localPromptCard(shell, exact.toStdString());

  editor->setPlainText(QStringLiteral("unsent editor draft"));
  static_cast<void>(worker.apply({DecodedMessageKind::ClientResult,
                                  "model/list",
                                  ProtocolRequestId("catalog-refresh"),
                                  {{"models", Value(Value::Array{})}}}));
  spin(40);
  require(
      editor->toPlainText() == QStringLiteral("unsent editor draft") &&
          localPromptCard(shell, exact.toStdString()) == card,
      "unrelated graph updates preserve local editor text and card identity");
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
      graphDraft && list && list->currentItem() &&
          list->currentItem()->data(Qt::UserRole).toString().toStdString() ==
              graphDraft->id().canonical &&
          localPromptCard(shell, promptText.toStdString()),
      "the optimistic row hands off to the selected shared graph draft");
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
  typedActionsAreExactOnceAndBounded(*configuration);
  qtHeartbeatSurvivesLargeInboundTraffic(*configuration);
  graphBackedShellPreservesDraftsAndPrompts(*configuration);
  optimisticDraftUsesOneTypedCreateAction(*configuration);
  saturatedShellKeepsTheEditorDraft(*configuration);
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
