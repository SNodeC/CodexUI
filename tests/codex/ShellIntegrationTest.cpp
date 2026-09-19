// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include <core/SNodeC.h>
#include <utils/Config.h>

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/PendingRequestDialog.h"
#include "codex/ShellWidget.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/ThreadPane.h"
#include "codex/nodegraph/WorkerLogic.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/UiStyle.h"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

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

  static void deliverGraphChanged(FrontendSession &session,
                                  const nodegraph::GraphChanged &change) {
    if (session.graphChangedHandler)
      session.graphChangedHandler(change);
  }
};

namespace {

using namespace codexui::nodegraph;

class AnimationDurationStyle final : public QProxyStyle {
public:
  explicit AnimationDurationStyle(int duration) : duration_(duration) {}

  int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                const QWidget *widget = nullptr,
                QStyleHintReturn *returnData = nullptr) const override {
    return hint == SH_Widget_Animation_Duration
               ? duration_
               : QProxyStyle::styleHint(hint, option, widget, returnData);
  }

private:
  int duration_;
};

class StyleChangeProbe final : public QObject {
public:
  int styleChanges = 0;

protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event && event->type() == QEvent::StyleChange)
      ++styleChanges;
    return false;
  }
};

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

PendingRequestDescriptor dialogRequest(PendingRequestKind kind,
                                       nlohmann::json raw) {
  PendingRequestDescriptor request;
  request.kind = kind;
  request.threadId = "thread-a";
  request.raw = std::move(raw);
  request.availability = PendingRequestAvailability::Actionable;
  return request;
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

void applyScrollableThreadContent(WorkerLogic &worker, std::string threadId,
                                  std::string suffix) {
  const std::string turnId = "scroll-turn-" + suffix;
  const std::string itemId = "scroll-item-" + suffix;
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       {{"threadId", Value(threadId)},
        {"turn", Value(Value::Object{{"id", Value(turnId)},
                                     {"status", Value("completed")}})}}}));
  std::string text;
  for (int line = 0; line < 200; ++line)
    text += "scrollable history line " + std::to_string(line) + '\n';
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "item/started",
       std::nullopt,
       {{"threadId", Value(std::move(threadId))},
        {"turnId", Value(turnId)},
        {"item", Value(Value::Object{{"id", Value(itemId)},
                                     {"type", Value("agentMessage")},
                                     {"text", Value(std::move(text))}})}}}));
}

void makeReady(WorkerLogic &worker) {
  static_cast<void>(worker.transportEvent("connected"));
  static_cast<void>(worker.bridgeState("test-controller", "controller",
                                       "test-controller", 1, "ready"));
}

void markThreadReady(FrontendSession &session, WorkerLogic &worker,
                     std::string_view id) {
  NodeRef thread;
  {
    auto read = FrontendSessionTestPeer::graph(session).tryRead();
    thread = read ? read->find({NodeKind::Thread, std::string(id)}) : NodeRef{};
  }
  if (thread)
    static_cast<void>(worker.threadHydration(thread, "ready"));
}

QTreeWidgetItem *threadItem(QTreeWidget *tree, std::string_view id,
                            QTreeWidgetItem *parent = nullptr) {
  const int count = parent ? parent->childCount()
                    : tree ? tree->topLevelItemCount()
                           : 0;
  for (int row = 0; row < count; ++row) {
    QTreeWidgetItem *item =
        parent ? parent->child(row) : tree->topLevelItem(row);
    if (item && item->data(0, Qt::UserRole).toString().toStdString() == id)
      return item;
    if (QTreeWidgetItem *found = threadItem(tree, id, item))
      return found;
  }
  return nullptr;
}

bool selectThread(QTreeWidget *list, std::string_view id) {
  QTreeWidgetItem *item = threadItem(list, id);
  if (!item)
    return false;
  list->setCurrentItem(item);
  spin();
  return list->currentItem() == item;
}

QAccessibleInterface *threadAccessible(QTreeWidget *tree,
                                       QTreeWidgetItem *item) {
  if (!tree || !item)
    return nullptr;
  std::vector<int> path;
  for (QTreeWidgetItem *current = item; current; current = current->parent())
    path.push_back(current->parent() ? current->parent()->indexOfChild(current)
                                     : tree->indexOfTopLevelItem(current));
  QAccessibleInterface *accessible =
      QAccessible::queryAccessibleInterface(tree);
  for (auto index = path.rbegin(); accessible && index != path.rend(); ++index)
    accessible = accessible->child(*index);
  return accessible;
}

QString threadAccessibleText(QTreeWidget *tree, QTreeWidgetItem *item,
                             QAccessible::Text kind) {
  QAccessibleInterface *accessible = threadAccessible(tree, item);
  return accessible ? accessible->text(kind) : QString{};
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
  const auto findMaterialized = [&]() -> middle::ConversationCard * {
    for (middle::ConversationCard *card :
         shell.findChildren<middle::ConversationCard *>()) {
      const auto *agent =
          std::get_if<middle::AgentMessageData>(&card->data().payload);
      if (agent && agent->text == message)
        return card;
    }
    return nullptr;
  };
  if (middle::ConversationCard *card = findMaterialized())
    return card;
  auto *view = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  if (!view)
    return nullptr;
  QModelIndex target;
  for (int row = 0; row < view->conversationModel()->rowCount(); ++row) {
    const middle::VisibleCardData *candidate =
        view->conversationModel()->card(row);
    const auto *agent =
        candidate ? std::get_if<middle::AgentMessageData>(&candidate->payload)
                  : nullptr;
    if (agent && agent->text == message) {
      target = view->conversationModel()->index(row);
      break;
    }
  }
  const QRect visible =
      view->visualRect(target).intersected(view->viewport()->rect());
  if (!target.isValid() || visible.isEmpty())
    return nullptr;
  const QPoint position = visible.center();
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(position),
                    QPointF(position), view->viewport()->mapToGlobal(position),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &press);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position),
                      QPointF(position),
                      view->viewport()->mapToGlobal(position), Qt::LeftButton,
                      Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &release);
  QCoreApplication::processEvents();
  return findMaterialized();
}

void graphNotificationsPrecedeRetirementRelease(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);

  std::size_t notifications = 0;
  bool ordinaryObserved = false;
  bool rescanObserved = false;
  bool observedAfterRescan = false;
  bool queuedGraphReadableAfterRescan = false;
  NodeRef ordinaryRemoved;
  NodeRef coalescedRemoved;
  session.setGraphChangedHandler([&](const GraphChanged &changed) {
    ++notifications;
    rescanObserved = rescanObserved || changed.rescanRequired;
    for (const NodeRef &node : changed.removed) {
      if (!node)
        continue;
      if (node->id() == NodeId{NodeKind::Thread, "ordinary-removal"}) {
        ordinaryRemoved = node;
        ordinaryObserved = true;
      } else if (node->id() == NodeId{NodeKind::Thread, "coalesced-removal"}) {
        coalescedRemoved = node;
        observedAfterRescan = rescanObserved && !changed.rescanRequired;
      }
    }
    for (const NodeRef &node : changed.affected) {
      if (!node || node->id() != NodeId{NodeKind::Thread, "coalesced-removal"})
        continue;
      auto read = session.nodeGraph().tryRead();
      queuedGraphReadableAfterRescan =
          rescanObserved && read && read->contains(node) &&
          stringFieldEquals(read->state(node), "name",
                            "queued-before-retirement");
    }
    session.acknowledgeUiDetached(changed.removed);
    return true;
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
  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "thread/deleted",
                                  std::nullopt,
                                  {{"threadId", Value("ordinary-removal")}}}));
  require(
      spinUntil([&] { return channels.qtToWorkerSizeApprox() != 0; }),
      "Qt receives removal and queues its typed retirement acknowledgement");

  std::vector<QtToWorkerMessage> commands = takeQtMessages(channels);
  NodeRef ordinaryAcknowledgement;
  for (QtToWorkerMessage &command : commands) {
    if (auto *action = std::get_if<NodeAction>(&command);
        action && action->kind == NodeActionKind::UiDetached)
      ordinaryAcknowledgement = std::move(action->target);
  }
  require(ordinaryObserved && ordinaryRemoved == ordinary &&
              ordinaryAcknowledgement == ordinary,
          "Qt observes the removed node before acknowledging its retirement");
  static_cast<void>(
      worker.acknowledgeUiDetached(std::move(ordinaryAcknowledgement)));
  {
    auto read = session.nodeGraph().tryRead();
    require(read && read->retiredCount() == 0,
            "the worker releases ordinary retirement only after the Qt "
            "acknowledgement");
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

  GraphChange queuedBeforeRetirement;
  {
    auto write = FrontendSessionTestPeer::graph(session).write();
    write.setField(coalesced, "name", "queued-before-retirement");
    queuedBeforeRetirement = write.finish();
  }
  require(channels.sendGraphChanged(std::move(queuedBeforeRetirement)) ==
              ChannelSendStatus::Accepted,
          "an older graph change is queued before the coalesced retirement");

  std::size_t fillerCount = 0;
  for (;;) {
    ProtocolDiagnostic filler{{{"sequence", Value(fillerCount)}}, {}};
    const ChannelSendStatus status =
        channels.sendProtocolDiagnostic(filler);
    if (status == ChannelSendStatus::QueueFull)
      break;
    require(status == ChannelSendStatus::Accepted,
            "ordinary worker-to-Qt filler is admitted normally");
    ++fillerCount;
  }
  require(fillerCount + ThreadChannels::WorkerToQtReservedSlots + 1 ==
              ThreadChannels::WorkerToQtCapacity,
          "worker mailbox saturation preserves the terminal slot");

  const ChannelSendStatus coalescedStatus =
      worker.apply({DecodedMessageKind::ServerNotification,
                    "thread/deleted",
                    std::nullopt,
                    {{"threadId", Value("coalesced-removal")}}});
  require(coalescedStatus == ChannelSendStatus::CoalescedRescan,
          "a saturated graph notification becomes an explicit rescan");
  FrontendSessionTestPeer::drainWorkerMessages(session);
  if (!queuedGraphReadableAfterRescan)
    FrontendSessionTestPeer::drainWorkerMessages(session);
  require(observedAfterRescan && queuedGraphReadableAfterRescan &&
              channels.qtToWorkerSizeApprox() == 0 &&
              channels.workerToQtSizeApprox() != 0,
          "the rescan precedes an exact retirement callback, while an older "
          "graph callback can still read "
          "the retired node while acknowledgement waits for the backlog");
  while (channels.workerToQtSizeApprox() != 0 || channels.rescanPending())
    FrontendSessionTestPeer::drainWorkerMessages(session);
  require(channels.qtToWorkerSizeApprox() != 0,
          "Qt acknowledges the retirement after the stale backlog");

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
          "rescan retirement carries the stable NodeRef through the "
          "acknowledgement");
  static_cast<void>(
      worker.acknowledgeUiDetached(std::move(coalescedAcknowledgement)));
  {
    auto read = session.nodeGraph().tryRead();
    require(read && read->retiredCount() == 0 && !read->contains(coalesced) &&
                coalesced,
            "coalesced retirement leaves graph membership only after Qt "
            "observation while external stable references remain safe");
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

  std::unordered_set<Node *> observed;
  std::size_t largestBatch = 0;
  std::uint64_t authorityRevision = 0;
  bool authorityBoundaryPreserved = true;
  NodeRef laterOrdinary;
  bool laterOrdinaryMisclassified = false;
  session.setGraphChangedHandler([&](const GraphChanged &changed) {
    if (changed.removed.empty())
      return true;
    largestBatch = std::max(largestBatch, changed.removed.size());
    for (const NodeRef &node : changed.removed) {
      if (!node)
        continue;
      if (node == laterOrdinary) {
        laterOrdinaryMisclassified = laterOrdinaryMisclassified ||
                                     changed.providerAuthorityRevision != 0;
      } else {
        observed.insert(node.get());
        authorityBoundaryPreserved =
            authorityBoundaryPreserved &&
            changed.providerAuthorityRevision == authorityRevision;
      }
    }
    session.acknowledgeUiDetached(changed.removed);
    return true;
  });

  GraphChange removal;
  {
    auto write = graph.write();
    const NodeRef connection =
        write.upsert({NodeKind::Connection, "connection"});
    authorityRevision = write.revision() + 1;
    write.setField(connection, "providerAuthorityRevision",
                   Value(authorityRevision));
    for (const NodeRef &node : nodes)
      write.remove(node);
    removal = write.finish();
  }
  removal.providerAuthorityRevision = authorityRevision;
  require(channels.sendGraphChanged(std::move(removal)) ==
              ChannelSendStatus::CoalescedRescan,
          "an oversized mass removal requests an explicit graph rescan even "
          "when the worker mailbox has space");

  FrontendSessionTestPeer::drainWorkerMessages(session);
  require(!observed.empty() && observed.size() <= 64 && largestBatch <= 64,
          "one Qt pass observes only a bounded retirement batch");

  {
    auto write = graph.write();
    laterOrdinary =
        write.upsert({NodeKind::Thread, "ordinary-after-provider-reset"});
    static_cast<void>(write.finish());
  }
  GraphChange laterRemoval;
  {
    auto write = graph.write();
    write.remove(laterOrdinary);
    laterRemoval = write.finish();
  }
  require(channels.sendGraphChanged(std::move(laterRemoval)) ==
              ChannelSendStatus::Accepted,
          "a later ordinary retirement remains directly deliverable");

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
  require(observed.size() == NodeCount && acknowledgements == NodeCount + 1 &&
              largestBatch <= 64 && authorityBoundaryPreserved &&
              !laterOrdinaryMisclassified && read && read->retiredCount() == 0,
          "sliced mass retirement observes and acknowledges every NodeRef "
          "against its authority boundary with a bounded Qt graph read");
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
  markThreadReady(session, worker, "removed-selected");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] {
            return threadItem(list, "removed-selected") != nullptr;
          }) &&
              selectThread(list, "removed-selected") && spinUntil([&] {
                return agentMessageCard(shell, "remove this card") != nullptr;
              }),
          "selected-removal fixture materializes its selected row and card");
  static_cast<void>(takeQtMessages(channels)); // discard hydration

  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "thread/deleted",
                                  std::nullopt,
                                  {{"threadId", Value("removed-selected")}}}));
  require(spinUntil([&] { return channels.qtToWorkerSizeApprox() != 0; }),
          "selected removal clears Qt projections and queues retirement "
          "acknowledgements");
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
  require(released != 0 && read && read->retiredCount() == 0 &&
              !threadItem(list, "removed-selected") &&
              !agentMessageCard(shell, "remove this card"),
          "deferred Qt work retains no released selected-thread NodeRef");
}

void settingsDraftsRetireWithProviderAndThreadIncarnations(
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
  auto *approval =
      shell.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  require(newThread && approval && list,
          "settings lifetime fixture exposes draft controls");
  if (!newThread || !approval || !list)
    return;
  newThread->click();
  approval->setCurrentIndex(approval->findData(QStringLiteral("never")));
  require(approval->currentData() == QStringLiteral("never"),
          "a pre-admission settings draft records its authored value");

  static_cast<void>(worker.bridgeState("test-controller", "controller",
                                       "test-controller", 2, "ready",
                                       "empty provider replaced"));
  FrontendSessionTestPeer::drainWorkerMessages(session);
  require(spinUntil([&] {
            return threadItem(list, "draft:new-thread") &&
                   approval->currentData() == QStringLiteral("default");
          }),
          "a zero-thread provider replacement resets settings without losing "
          "the unsent new-thread draft or deferring invalidation");

  applyThread(worker, "settings-a", "Settings A");
  applyThread(worker, "settings-b", "Settings B");
  markThreadReady(session, worker, "settings-a");
  markThreadReady(session, worker, "settings-b");
  require(spinUntil([&] {
            return threadItem(list, "settings-a") &&
                   threadItem(list, "settings-b");
          }) &&
              selectThread(list, "settings-a") && spinUntil([&] {
                return approval->currentData() == QStringLiteral("default");
              }),
          "canonical settings threads replace the abandoned empty draft");

  approval->setCurrentIndex(approval->findData(QStringLiteral("never")));
  require(selectThread(list, "settings-b") && spinUntil([&] {
            return approval->currentData() == QStringLiteral("default");
          }),
          "settings draft switches to the second canonical thread");
  approval->setCurrentIndex(approval->findData(QStringLiteral("untrusted")));
  require(selectThread(list, "settings-a") && spinUntil([&] {
            return approval->currentData() == QStringLiteral("never");
          }) &&
              selectThread(list, "settings-b") && spinUntil([&] {
                return approval->currentData() == QStringLiteral("untrusted");
              }),
          "ordinary A-to-B-to-A switching preserves each authored draft");

  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "thread/deleted",
                                  std::nullopt,
                                  {{"threadId", Value("settings-a")}}}));
  require(spinUntil([&] { return !threadItem(list, "settings-a"); }),
          "explicit removal retires an unselected thread incarnation");
  applyThread(worker, "settings-a", "Recreated settings A");
  markThreadReady(session, worker, "settings-a");
  require(
      spinUntil([&] { return threadItem(list, "settings-a"); }) &&
          selectThread(list, "settings-a") && spinUntil([&] {
            return approval->currentData() == QStringLiteral("default");
          }),
      "reusing an explicitly removed canonical id starts with fresh settings");

  approval->setCurrentIndex(approval->findData(QStringLiteral("never")));
  require(selectThread(list, "settings-b"),
          "provider-reset fixture retains a draft for its second thread");
  static_cast<void>(worker.bridgeState("test-controller", "controller",
                                       "test-controller", 3, "ready",
                                       "provider replaced"));
  spin(40);
  applyThread(worker, "settings-a", "Provider 3 settings A");
  applyThread(worker, "settings-b", "Provider 3 settings B");
  markThreadReady(session, worker, "settings-a");
  markThreadReady(session, worker, "settings-b");
  require(spinUntil([&] {
            return threadItem(list, "settings-a") &&
                   threadItem(list, "settings-b");
          }) &&
              selectThread(list, "settings-a") && spinUntil([&] {
                return approval->currentData() == QStringLiteral("default");
              }) &&
              selectThread(list, "settings-b") && spinUntil([&] {
                return approval->currentData() == QStringLiteral("default");
              }),
          "provider replacement retires all drafts before canonical ids are "
          "reused");
  static_cast<void>(takeQtMessages(channels));
}

void delayedRetirementCannotEraseRecreatedThread(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "reused-thread", "Old incarnation");
  applyScrollableThreadContent(worker, "reused-thread", "old");
  markThreadReady(session, worker, "reused-thread");

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  auto *approval =
      shell.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  auto *view = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  middle::ComposerPane *composer = nullptr;
  for (QWidget *widget : shell.findChildren<QWidget *>())
    if (auto *candidate = dynamic_cast<middle::ComposerPane *>(widget)) {
      composer = candidate;
      break;
    }
  require(list && approval && view && composer &&
              spinUntil([&] { return threadItem(list, "reused-thread"); }) &&
              selectThread(list, "reused-thread"),
          "delayed-retirement fixture binds its original incarnation");
  if (!list || !approval || !view || !composer)
    return;
  approval->setCurrentIndex(approval->findData(QStringLiteral("never")));
  static_cast<void>(spinUntil(
      [&] { return view->verticalScrollBar()->maximum() > 0; }, 3000));
  view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
  view->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
  require(view->modeForThread("reused-thread") ==
              middle::ConversationView::Mode::Paused,
          "the original incarnation owns paused viewport state");
  static_cast<void>(takeQtMessages(channels));

  NodeRef original;
  GraphChange removal;
  {
    auto write = graph.write();
    original = write.find({NodeKind::Thread, "reused-thread"});
    write.remove(original);
    removal = write.finish();
  }
  NodeRef replacement;
  GraphChange recreation;
  {
    NodeState state;
    state.fields = {{"name", Value("New incarnation")},
                    {"cwd", Value("/tmp")},
                    {"hydrationState", Value("ready")},
                    {"approvalPolicy", Value("on-request")}};
    auto write = graph.write();
    replacement =
        write.upsert({NodeKind::Thread, "reused-thread"}, std::move(state));
    recreation = write.finish();
  }
  FrontendSessionTestPeer::deliverGraphChanged(
      session, GraphChanged{recreation.revision, {}, {}, true, {}, 0});
  applyScrollableThreadContent(worker, "reused-thread", "new");
  require(spinUntil([&] {
            return approval->currentData() == QStringLiteral("on-request");
          }) &&
              original && replacement && original != replacement &&
              view->modeForThread("reused-thread") ==
                  middle::ConversationView::Mode::Following,
          "the latest-state rescan binds a fresh node and presentation state");

  approval->setCurrentIndex(approval->findData(QStringLiteral("untrusted")));
  static_cast<void>(spinUntil(
      [&] { return view->verticalScrollBar()->maximum() > 0; }, 3000));
  view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
  view->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
  const std::vector<QtToWorkerMessage> reboundMessages =
      takeQtMessages(channels);
  const std::size_t hydrationCount =
      std::ranges::count_if(reboundMessages, [&](const auto &message) {
        const auto *action = std::get_if<NodeAction>(&message);
        return action && action->kind == NodeActionKind::Hydrate &&
               action->target == replacement;
      });

  FrontendSessionTestPeer::deliverGraphChanged(
      session, GraphChanged{removal.revision, std::move(removal.affected),
                            std::move(removal.removed), false,
                            std::move(removal.childListsChanged), 0});
  spin();
  require(
      hydrationCount == 1 &&
          composer->turnSettings().touched(TurnSettingField::Approval) &&
          composer->turnSettings().values()[TurnSettingField::Approval] ==
              "untrusted" &&
          view->modeForThread("reused-thread") ==
              middle::ConversationView::Mode::Paused &&
          list->currentItem() == threadItem(list, "reused-thread"),
      "a delayed old removal preserves the selected replacement's settings, "
      "viewport state, and single hydration");
}

void slicedProviderRetirementPreservesFreshSelection(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "sliced-reuse", "Provider one");
  applyScrollableThreadContent(worker, "sliced-reuse", "provider-one");
  markThreadReady(session, worker, "sliced-reuse");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  auto *approval =
      shell.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  auto *view = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  middle::ComposerPane *composer = nullptr;
  for (QWidget *widget : shell.findChildren<QWidget *>())
    if (auto *candidate = dynamic_cast<middle::ComposerPane *>(widget)) {
      composer = candidate;
      break;
    }
  require(list && approval && view && composer &&
              spinUntil([&] { return threadItem(list, "sliced-reuse"); }) &&
              selectThread(list, "sliced-reuse"),
          "sliced provider fixture binds its original selected thread");
  if (!list || !approval || !view || !composer)
    return;
  static_cast<void>(takeQtMessages(channels));

  {
    auto write = graph.write();
    for (std::size_t index = 0; index < 640; ++index)
      static_cast<void>(write.upsert(
          {NodeKind::Item, "provider-retired-" + std::to_string(index)}));
    static_cast<void>(write.finish());
  }
  require(worker.bridgeState("test-controller", "controller", "test-controller",
                             2, "ready", "sliced provider replacement") ==
              ChannelSendStatus::CoalescedRescan,
          "an oversized provider replacement enters bounded rescan delivery");
  FrontendSessionTestPeer::drainWorkerMessages(session);

  applyThread(worker, "sliced-reuse", "Provider two");
  applyScrollableThreadContent(worker, "sliced-reuse", "provider-two");
  markThreadReady(session, worker, "sliced-reuse");
  NodeRef replacement;
  {
    auto read = graph.tryRead();
    replacement =
        read ? read->find({NodeKind::Thread, "sliced-reuse"}) : NodeRef{};
  }
  FrontendSessionTestPeer::drainWorkerMessages(session);
  for (int pass = 0;
       pass < 32 &&
       (!replacement || composer->turnSettings().context().threadIncarnation !=
                            replacement->incarnation());
       ++pass)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
  require(replacement &&
              composer->turnSettings().context().threadIncarnation ==
                  replacement->incarnation() &&
              approval->currentData() == QStringLiteral("default") &&
              view->modeForThread("sliced-reuse") ==
                  middle::ConversationView::Mode::Following,
          "the fresh same-id thread binds before later retirement slices");

  approval->setCurrentIndex(approval->findData(QStringLiteral("untrusted")));
  static_cast<void>(spinUntil(
      [&] { return view->verticalScrollBar()->maximum() > 0; }, 3000));
  view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
  view->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
  std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  const std::size_t hydrationCount =
      std::ranges::count_if(messages, [&](const auto &message) {
        const auto *action = std::get_if<NodeAction>(&message);
        return action && action->kind == NodeActionKind::Hydrate &&
               action->target == replacement;
      });
  std::size_t retirements = 0;
  const auto releaseRetirements = [&](std::vector<QtToWorkerMessage> batch) {
    for (QtToWorkerMessage &message : batch) {
      auto *action = std::get_if<NodeAction>(&message);
      if (!action || action->kind != NodeActionKind::UiDetached)
        continue;
      ++retirements;
      static_cast<void>(
          worker.acknowledgeUiDetached(std::move(action->target)));
    }
  };
  releaseRetirements(std::move(messages));
  for (int pass = 0; pass < 32; ++pass) {
    FrontendSessionTestPeer::drainWorkerMessages(session);
    releaseRetirements(takeQtMessages(channels));
    auto read = graph.tryRead();
    if (read && read->retiredCount() == 0)
      break;
  }
  const bool retained =
      hydrationCount == 1 && retirements > 512 &&
      composer->turnSettings().touched(TurnSettingField::Approval) &&
      composer->turnSettings().values()[TurnSettingField::Approval] ==
          "untrusted" &&
      view->modeForThread("sliced-reuse") ==
          middle::ConversationView::Mode::Paused &&
      list->currentItem() == threadItem(list, "sliced-reuse");
  if (!retained)
    std::cerr << "sliced provider: hydrations=" << hydrationCount
              << " retirements=" << retirements << " touched="
              << composer->turnSettings().touched(TurnSettingField::Approval)
              << " approval="
              << composer->turnSettings().values()[TurnSettingField::Approval]
              << " paused="
              << (view->modeForThread("sliced-reuse") ==
                  middle::ConversationView::Mode::Paused)
              << " selected="
              << (list->currentItem() == threadItem(list, "sliced-reuse"))
              << '\n';
  require(
      retained,
      "late provider-retirement slices preserve one fresh binding, authored "
      "settings, and viewport state");
}

void splitterHandleDrivesInteractiveConversationResize(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  spin();
  auto *splitter =
      shell.findChild<QSplitter *>(QStringLiteral("workspaceSplitter"));
  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  QWidget *handle = splitter ? splitter->handle(1) : nullptr;
  require(splitter && conversation && handle,
          "workspace exposes its splitter and conversation resize surface");
  if (!handle || !conversation)
    return;

  const QPoint local = handle->rect().center();
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(local),
                    handle->mapToGlobal(local), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QApplication::sendEvent(handle, &press);
  const bool activated =
      conversation->property("conversationInteractiveResizeActive").toBool();
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local),
                      QPointF(local), handle->mapToGlobal(local),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(handle, &release);
  require(activated &&
              !conversation->property("conversationInteractiveResizeActive")
                   .toBool() &&
              conversation->property("conversationInteractiveResizeSettlements")
                      .toULongLong() == 1,
          "splitter press and release bracket one interactive resize burst");
}

void applicationFontChangeRegeneratesUiGeometry(Configuration &configuration) {
  const QFont originalFont = QApplication::font();
  const QString originalStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  {
    FrontendSession session(configuration);
    ShellWidget shell(session);
    shell.resize(1500, 850);
    shell.show();
    spin();

    auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
        QStringLiteral("upcomingPromptEditor"));
    auto *brandTitle =
        shell.findChild<QLabel *>(QStringLiteral("codexBrandTitle"));
    auto *topBar = shell.findChild<QFrame *>(QStringLiteral("topBar"));
    auto *statusBar =
        shell.findChild<QFrame *>(QStringLiteral("customStatusBar"));
    auto *conversation =
        shell.findChild<QFrame *>(QStringLiteral("conversation"));
    require(editor && brandTitle && topBar && statusBar && conversation,
            "the shell exposes its font-dependent and statically styled "
            "surfaces");
    if (editor && brandTitle && topBar && statusBar && conversation) {
      const QImage topPixels = topBar->grab().toImage();
      const QImage statusPixels = statusBar->grab().toImage();
      const QImage conversationPixels = conversation->grab().toImage();
      require(
          topPixels.pixelColor(topPixels.width() - 2, 2) ==
                  QColor(QString::fromLatin1(codexui::UiStyle::panel)) &&
              topPixels.pixelColor(topPixels.width() - 2,
                                   topPixels.height() - 1) ==
                  QColor(QString::fromLatin1(codexui::UiStyle::divider)) &&
              statusPixels.pixelColor(statusPixels.width() - 2,
                                      statusPixels.height() - 2) ==
                  QColor(QString::fromLatin1(codexui::UiStyle::raised)) &&
              statusPixels.pixelColor(statusPixels.width() - 2, 0) ==
                  QColor(QString::fromLatin1(codexui::UiStyle::divider)) &&
              conversationPixels.pixelColor(2, 2) ==
                  QColor(QString::fromLatin1(codexui::UiStyle::appBackground)),
          "centralizing shell chrome preserves its exact background and edge "
          "pixels");

      QString longDraft;
      for (int line = 0; line < 30; ++line)
        longDraft += QStringLiteral("font-dependent line\n");
      editor->setPlainText(longDraft);
      shell.activateWindow();
      editor->setFocus(Qt::OtherFocusReason);
      spin();
      require(QApplication::focusWidget() == editor &&
                  editor->height() == editor->maximumHeight() &&
                  editor->verticalScrollBar()->maximum() > 0,
              "the font-change fixture begins focused at the old scrollable "
              "editor maximum");
      const QString baselineStyleSheet = qApp->styleSheet();
      const int baselineMaximumHeight = editor->maximumHeight();
      QPointer<codexui::ExpandingPromptEditor> editorIdentity(editor);
      QPointer<QLabel> titleIdentity(brandTitle);
      StyleChangeProbe styleProbe;
      editor->installEventFilter(&styleProbe);

      QFont enlarged = originalFont;
      const qreal pointSize = QFontInfo(originalFont).pointSizeF();
      enlarged.setPointSizeF((pointSize > 0.0 ? pointSize : 10.0) + 2.0);
      QApplication::setFont(enlarged);
      const QString expectedStyleSheet =
          codexui::UiStyle::applicationStyleSheet();
      const bool regenerated = spinUntil(
          [&] { return qApp->styleSheet() == expectedStyleSheet; }, 500);
      require(regenerated && expectedStyleSheet != baselineStyleSheet,
              "one application-font event regenerates the shared stylesheet");
      if (!regenerated)
        qApp->setStyleSheet(expectedStyleSheet);
      spin();

      const QString enlargedStyleSheet = qApp->styleSheet();
      const int expectedEditorMaximum =
          editor->fontMetrics().lineSpacing() *
              codexui::ExpandingPromptEditor::maximumVisibleLineCount() +
          10;
      require(editor->maximumHeight() == expectedEditorMaximum &&
                  editor->height() == expectedEditorMaximum &&
                  expectedEditorMaximum > baselineMaximumHeight &&
                  editor->verticalScrollBar()->maximum() > 0,
              "the existing editor remeasurement uses its current font for "
              "both maximum and visible height");
      const int titlePixelSize = QFontInfo(brandTitle->font()).pixelSize();
      QFont largerTitleFont = brandTitle->font();
      largerTitleFont.setPixelSize(titlePixelSize + 1);
      require(titlePixelSize > 0 &&
                  QFontMetrics(brandTitle->font()).height() <= 36 &&
                  QFontMetrics(largerTitleFont).height() > 36,
              "the brand title is the largest current-font size that fits its "
              "unchanged 36-pixel lockup");
      require(editorIdentity == editor && titleIdentity == brandTitle &&
                  QApplication::focusWidget() == editor &&
                  styleProbe.styleChanges > 0,
              "font regeneration retains the same focused editor and brand "
              "objects");

      spin();
      const QRect settledEditorGeometry = editor->geometry();
      const QRect settledTitleGeometry = brandTitle->geometry();
      const QImage settledTitlePixels = brandTitle->grab().toImage();
      spin();
      require(editor->geometry() == settledEditorGeometry &&
                  brandTitle->geometry() == settledTitleGeometry &&
                  brandTitle->grab().toImage() == settledTitlePixels,
              "the real font transition settles before the no-op probe");
      styleProbe.styleChanges = 0;
      const QRect editorGeometry = editor->geometry();
      const QRect titleGeometry = brandTitle->geometry();
      const QImage titlePixels = brandTitle->grab().toImage();
      QEvent identicalFontEvent(QEvent::ApplicationFontChange);
      QApplication::sendEvent(qApp, &identicalFontEvent);
      spin();
      require(qApp->styleSheet() == enlargedStyleSheet &&
                  styleProbe.styleChanges == 0 &&
                  editor->geometry() == editorGeometry &&
                  brandTitle->geometry() == titleGeometry &&
                  brandTitle->grab().toImage() == titlePixels &&
                  editorIdentity == editor && titleIdentity == brandTitle &&
                  QApplication::focusWidget() == editor,
              "an identical application-font event is a semantic pixel, "
              "geometry, focus, identity, and stylesheet no-op");
      editor->removeEventFilter(&styleProbe);
    }
  }
  QApplication::setFont(originalFont);
  qApp->setStyleSheet(originalStyleSheet);
  spin();
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
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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

  static_cast<void>(spinUntil(
      [&] { return channels.qtToWorkerSizeApprox() != 0; }, 3000));
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
  require(released && read && read->retiredCount() == 0 &&
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

  RuntimeAction naturallyAuthored{RuntimeActionKind::ConfigureConnection};
  naturallyAuthored.payload = {{"transport", Value("invalid-for-fixture")}};
  require(session.sendRuntimeAction(naturallyAuthored) ==
              ChannelSendStatus::Accepted,
          "the frontend admits a naturally authored action without requiring "
          "widgets to manufacture protocol correlation");
  const EventFd::DrainResult correlatedWake = channels.drainQtToWorkerWake();
  const bool gotCorrelated = channels.tryReceiveForWorker(received);
  const RuntimeAction *correlatedAction =
      gotCorrelated ? std::get_if<RuntimeAction>(&received) : nullptr;
  require(correlatedWake.status == EventFd::DrainStatus::Drained &&
              correlatedWake.count == 1 && correlatedAction &&
              correlatedAction->correlation.starts_with("ui-action-") &&
              !channels.tryReceiveForWorker(received),
          "FrontendSession assigns one bounded correlation before enqueueing "
          "a real UI action");

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

  NodeAction materialized;
  materialized.target = target;
  materialized.kind = NodeActionKind::PromptMaterialized;
  NodeAction duplicateMaterialized = materialized;
  require(session.sendNodeAction(materialized) == ChannelSendStatus::Accepted &&
              !materialized.target &&
              session.sendNodeAction(duplicateMaterialized) ==
                  ChannelSendStatus::Accepted &&
              !duplicateMaterialized.target,
          "idempotent materialization acknowledgement remains durably "
          "admitted when the bounded mailbox is saturated");

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

  FrontendSessionTestPeer::drainWorkerMessages(session);
  const EventFd::DrainResult acknowledgementWake =
      channels.drainQtToWorkerWake();
  const bool gotAcknowledgement = channels.tryReceiveForWorker(received);
  const NodeAction *acknowledgement =
      gotAcknowledgement ? std::get_if<NodeAction>(&received) : nullptr;
  require(acknowledgementWake.status == EventFd::DrainStatus::Drained &&
              acknowledgementWake.count == 1 && acknowledgement &&
              acknowledgement->kind == NodeActionKind::PromptMaterialized &&
              acknowledgement->target == target &&
              !channels.tryReceiveForWorker(received),
          "mailbox recovery emits one exact materialization acknowledgement "
          "after duplicate saturated admissions");
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
  markThreadReady(session, worker, "traffic-thread");
  auto *threadList =
      shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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
    const NodeRef turn =
        read ? read->find(scopedTurnNodeId("traffic-thread", "traffic-turn"))
             : NodeRef{};
    const Value *activity =
        thread ? field(read->state(thread), "localActivityAt") : nullptr;
    if (turn)
      loadedItems = read->childCount(turn);
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

void conversationPresentationBurstIsFrameBounded(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  constexpr int ItemCount = 24;
  makeReady(worker);
  applyThread(worker, "bounded-stream-thread", "Bounded stream thread");
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       {{"threadId", Value("bounded-stream-thread")},
        {"turn", Value(Value::Object{{"id", Value("bounded-stream-turn")},
                                     {"status", Value("inProgress")}})}}}));
  for (int index = 0; index < ItemCount; ++index) {
    static_cast<void>(worker.apply(
        {DecodedMessageKind::ServerNotification,
         "item/started",
         std::nullopt,
         {{"threadId", Value("bounded-stream-thread")},
          {"turnId", Value("bounded-stream-turn")},
          {"item",
           Value(Value::Object{
               {"id", Value("bounded-stream-item-" + std::to_string(index))},
               {"type", Value("agentMessage")},
               {"text", Value("initial-" + std::to_string(index))}})}}}));
  }
  markThreadReady(session, worker, "bounded-stream-thread");

  auto *threadList =
      shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] {
            return threadItem(threadList, "bounded-stream-thread") != nullptr;
          }),
          "the bounded streaming fixture reaches the thread pane");
  require(selectThread(threadList, "bounded-stream-thread"),
          "the bounded streaming fixture binds the conversation view");
  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  require(conversation && spinUntil([&] {
            return conversation->conversationModel()->rowCount() == ItemCount &&
                   !conversation->structuralStagingActive() &&
                   conversation->viewport()->updatesEnabled();
          }),
          "the bounded streaming fixture exposes its complete initial model");
  if (!conversation)
    return;

  std::vector<NodeRef> items;
  items.reserve(ItemCount);
  {
    auto read = graph.tryRead();
    for (int index = 0; read && index < ItemCount; ++index) {
      items.push_back(read->find(scopedItemNodeId(
          scopedTurnNodeId("bounded-stream-thread", "bounded-stream-turn"),
          "bounded-stream-item-" + std::to_string(index))));
    }
  }
  require(items.size() == ItemCount &&
              std::ranges::all_of(items,
                                  [](const NodeRef &item) { return !!item; }),
          "the bounded streaming fixture resolves every exact item target");
  if (items.size() != ItemCount ||
      !std::ranges::all_of(items, [](const NodeRef &item) { return !!item; }))
    return;

  // Let selection/staging timers become fully idle before measuring the
  // presentation scheduler itself.
  spin(80);
  const qulonglong threadRoutesBefore =
      shell.property("threadPaneRoutes").toULongLong();
  const qulonglong inspectorRoutesBefore =
      shell.property("inspectorRoutes").toULongLong();
  const qulonglong shellCommitsBefore =
      shell.property("shellRenderCommits").toULongLong();
  const qulonglong constructionsBefore =
      conversation->property("conversationCardConstructions").toULongLong();
  shell.setProperty("conversationPresentationRowsProcessed", qulonglong{0});
  shell.setProperty("conversationPresentationMaxRowsPerPass", qulonglong{0});
  shell.setProperty("conversationPresentationDeferredPasses", qulonglong{0});

  bool admitted = true;
  for (int index = 0; index < ItemCount; ++index) {
    GraphChange change;
    {
      auto write = graph.write();
      write.setField(items[static_cast<std::size_t>(index)], "text",
                     Value("final-" + std::to_string(index)));
      change = write.finish();
    }
    admitted = messageAdmitted(channels.sendGraphChanged(std::move(change))) &&
               admitted;
  }
  require(admitted, "every distinct presentation delta enters the Qt queue");

  const bool finalStatePresented = spinUntil(
      [&] {
        if (conversation->conversationModel()->rowCount() != ItemCount)
          return false;
        for (int row = 0; row < ItemCount; ++row) {
          const middle::VisibleCardData *card =
              conversation->conversationModel()->card(row);
          const auto *message =
              card ? std::get_if<middle::AgentMessageData>(&card->payload)
                   : nullptr;
          if (!message || message->text != "final-" + std::to_string(row))
            return false;
        }
        return true;
      },
      2000);
  require(finalStatePresented,
          "a multi-frame presentation burst reaches every latest graph value");

  // One already-scheduled timer may have become redundant as the final pass
  // emptied the queue. Measure only after that timer has had time to fire.
  spin(40);
  const qulonglong idleCommits =
      shell.property("paneCommitInvocations").toULongLong();
  spin(80);
  require(
      shell.property("conversationPresentationRowsProcessed").toULongLong() ==
              ItemCount &&
          shell.property("conversationPresentationMaxRowsPerPass")
                  .toULongLong() <=
              shell.property("conversationPresentationRowsPerPassBudget")
                  .toULongLong() &&
          shell.property("conversationPresentationDeferredPasses")
                  .toULongLong() >= 2,
      "ordinary conversation projection is capped per GUI frame");
  require(shell.property("paneCommitInvocations").toULongLong() == idleCommits,
          "an empty presentation queue schedules no idle pane commits");
  require(
      shell.property("threadPaneRoutes").toULongLong() == threadRoutesBefore &&
          shell.property("inspectorRoutes").toULongLong() ==
              inspectorRoutesBefore &&
          shell.property("shellRenderCommits").toULongLong() ==
              shellCommitsBefore &&
          conversation->property("conversationCardConstructions")
                  .toULongLong() == constructionsBefore,
      "stream coalescing leaves unrelated panes and QWidget population alone");
}

void graphBackedShellPreservesDraftsAndPrompts(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  AnimationDurationStyle normalMotion(100);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "shell-thread", "Shared graph thread");
  markThreadReady(session, worker, "shell-thread");
  require(spinUntil([&] {
            auto *list =
                shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
            return threadItem(list, "shell-thread") != nullptr;
          }),
          "the existing thread widget materializes from shared graph nodes");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  if (list)
    list->setStyle(&normalMotion);
  require(selectThread(list, "shell-thread"),
          "selecting the graph-backed row binds the existing conversation");
  static_cast<void>(takeQtMessages(channels)); // Hydrate is tested elsewhere.

  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "thread/status/changed",
       std::nullopt,
       {{"threadId", Value("shell-thread")}, {"status", Value("cancelled")}}}));
  auto *conversationState =
      shell.findChild<QLabel *>(QStringLiteral("conversationState"));
  require(spinUntil([&] {
            return conversationState &&
                   conversationState->text() == QStringLiteral("interrupted") &&
                   conversationState->property("tone").toString() ==
                       QStringLiteral("warning");
          }),
          "the conversation heading consumes the canonical status tone");

  channels.failNextWorkerToQtWakeForTest();
  const ChannelSendStatus wakeFailure = worker.apply(
      {DecodedMessageKind::ServerNotification, "thread/name/updated",
       std::nullopt,
       Value::Object{{"threadId", Value("shell-thread")},
                     {"threadName", Value("Wake-recovered thread")}}});
  const bool wakeRecovered = spinUntil([&] {
    QTreeWidgetItem *item = threadItem(list, "shell-thread");
    return channels.workerToQtSizeApprox() == 0 &&
           threadAccessibleText(list, item, QAccessible::Name) ==
               QStringLiteral("Wake-recovered thread");
  });
  require(wakeFailure == ChannelSendStatus::AcceptedWakeFailed &&
              deliveryGuaranteed(wakeFailure) && wakeFailed(wakeFailure) &&
              wakeRecovered,
          "Qt's bounded recovery drain renders a graph update after a failed "
          "worker wake");

  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  auto *approval =
      shell.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  if (approval)
    approval->setCurrentIndex(approval->findData(QStringLiteral("untrusted")));
  const QString exact =
      QStringLiteral("  graph prompt stays exact\n\nincluding blank lines\n\n");
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
  const auto authoredApproval = prompt.payload.find("approvalPolicy");
  require(promptCount == 1 && prompt.target && approval &&
              approval->currentData() == QStringLiteral("untrusted") &&
              prompt.target->id() == NodeId{NodeKind::Thread, "shell-thread"} &&
              prompt.promptText == exact.toStdString() &&
              authoredApproval != prompt.payload.end() &&
              authoredApproval->second.asString() &&
              *authoredApproval->second.asString() == "untrusted" && editor &&
              editor->toPlainText().isEmpty(),
          "the composer emits one typed prompt with exact authored settings "
          "and blank lines");

  PromptTransition transition = worker.admitPrompt(
      std::move(prompt), std::nullopt, QDateTime::currentMSecsSinceEpoch());
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
  QTimer *pendingAnimation =
      card ? card->findChild<QTimer *>(QStringLiteral("pendingAnimationTimer"))
           : nullptr;
  require(card && pendingAnimation && !pendingAnimation->isActive(),
          "the optimistically inserted Turn/You card begins calm");
  require(spinUntil([&] { return pendingAnimation->isActive(); }, 1500),
          "the unacknowledged Turn/You card starts feedback at its fixed "
          "one-second admission deadline");

  editor->setPlainText(QStringLiteral("unsent editor draft"));
  static_cast<void>(worker.apply({DecodedMessageKind::ClientResult,
                                  "model/list",
                                  ProtocolRequestId("catalog-refresh"),
                                  {{"models", Value(Value::Array{})}}}));
  spin(40);
  require(
      editor->toPlainText() == QStringLiteral("unsent editor draft") &&
          localPromptCard(shell, exact.toStdString()) == card &&
          pendingAnimation->isActive(),
      "unrelated graph updates preserve local editor text and card identity");

  auto *threadAnimation =
      shell.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  require(threadAnimation && threadAnimation->isActive(),
          "the selected thread card shares the Turn/You pending animation");
  static_cast<void>(worker.completePrompt(localPrompt, true, {}, "shell-turn"));
  spin(60);
  require(!pendingAnimation->isActive() && !threadAnimation->isActive(),
          "the same prompt acknowledgement stops both card animations");
}

void initialHydrationRetainsAllLoadedRowsWithBoundedResidency(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "bounded-history", "Bounded history");
  const ProtocolRequestId seedHistoryId("shell-history-seed");
  const WorkerApplyResult seedHistory =
      worker.applyDetailed({DecodedMessageKind::ClientRequest,
                            "thread/turns/list",
                            seedHistoryId,
                            {{"threadId", Value("bounded-history")}}});
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ClientResult,
       "thread/turns/list",
       seedHistoryId,
       {{"data", Value(Value::Array{})}, {"nextCursor", Value("older-page")}},
       seedHistory.primary}));
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       {{"threadId", Value("bounded-history")},
        {"turn", Value(Value::Object{{"id", Value("bounded-turn")},
                                     {"status", Value("completed")}})}}}));
  for (std::size_t index = 0; index < 100; ++index) {
    const std::string id = "bounded-item-" + std::to_string(index);
    const std::string type = index == 0 ? "userMessage" : "agentMessage";
    static_cast<void>(
        worker.apply({DecodedMessageKind::ServerNotification,
                      "item/started",
                      std::nullopt,
                      {{"threadId", Value("bounded-history")},
                       {"turnId", Value("bounded-turn")},
                       {"item", Value(Value::Object{{"id", Value(id)},
                                                    {"type", Value(type)},
                                                    {"text", Value(id)}})}}}));
  }

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] { return threadItem(list, "bounded-history"); }),
          "bounded history appears in the established thread pane");
  require(selectThread(list, "bounded-history"),
          "bounded history can be selected");
  static_cast<void>(takeQtMessages(channels));
  require(shell.findChildren<middle::ConversationCard *>().empty(),
          "partial pre-hydration history creates no conversation QWidget");

  markThreadReady(session, worker, "bounded-history");
  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QAbstractScrollArea *>(
          QStringLiteral("conversationScroll")));
  require(conversation && spinUntil([&] {
            return conversation->structuralStagingActive() ||
                   conversation->conversationModel()->rowCount() == 100;
          }, 3000),
          "large initial history either stages rich visible rows or commits a "
          "complete resident frame immediately");
  require(!conversation || !conversation->structuralStagingActive() ||
              conversation->conversationModel()->rowCount() == 0,
          "hidden preparation leaves the prior complete model exposed");
  const qulonglong stageStarts =
      conversation->property("structuralStageStarts").toULongLong();
  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "item/agentMessage/delta",
                                  std::nullopt,
                                  {{"threadId", Value("bounded-history")},
                                   {"turnId", Value("bounded-turn")},
                                   {"itemId", Value("bounded-item-99")},
                                   {"delta", Value(" latest")}}}));
  require(spinUntil(
              [&] {
                return conversation->conversationModel()->rowCount() == 100 &&
                       !conversation->structuralStagingActive();
              },
              2000),
          "the first atomic model frame contains every loaded graph item");
  require(conversation->materializedCardCount() <= 48 &&
              shell
                  .findChildren<QWidget *>(
                      QStringLiteral("conversationCardPlaceholder"))
                  .empty(),
          "the complete 100-row frame keeps QWidget work viewport "
          "proportional");
  require(spinUntil([&] {
            const middle::VisibleCardData *tail =
                conversation->conversationModel()->card(
                    conversation->conversationModel()->rowCount() - 1);
            const auto *agent =
                tail ? std::get_if<middle::AgentMessageData>(&tail->payload)
                     : nullptr;
            return agent && agent->text == "bounded-item-99 latest";
          }),
          "the live delta reaches its exact indexed tail row");
  middle::ConversationCard *latestCard = nullptr;
  const bool latestMaterialized = spinUntil([&] {
    latestCard = agentMessageCard(shell, "bounded-item-99 latest");
    return latestCard != nullptr;
  });
  require(conversation->property("structuralStageStarts").toULongLong() ==
                  stageStarts &&
              latestMaterialized,
          "a live canonical update patches the hidden target without "
          "restarting or starving structural staging");

  QPushButton *loadMore =
      shell.findChild<QPushButton *>(QStringLiteral("conversationLoadMore"));
  require(loadMore && loadMore->isVisible() &&
              loadMore->text() == QStringLiteral("Load earlier activities"),
          "provider continuation exposes one provider-owned history action");

  NodeRef historyThread;
  {
    auto read = session.nodeGraph().tryRead();
    historyThread =
        read ? read->find({NodeKind::Thread, "bounded-history"}) : NodeRef{};
  }
  DecodedMessage pendingRequest{DecodedMessageKind::ClientRequest,
                                "thread/turns/list",
                                ProtocolRequestId("shell-history-pending"),
                                {{"threadId", Value("bounded-history")}}};
  pendingRequest.requestTarget = historyThread;
  const WorkerApplyResult pending =
      worker.applyDetailed(std::move(pendingRequest));
  require(historyThread && pending.primary && spinUntil([&] {
            return loadMore && loadMore->isVisible() &&
                   !loadMore->isEnabled() &&
                   loadMore->text() ==
                       QStringLiteral("Loading earlier activities");
          }),
          "the exact graph Operation disables the existing history control");
#if QT_CONFIG(accessibility)
  QAccessibleInterface *historyAccessibility =
      loadMore ? QAccessible::queryAccessibleInterface(loadMore) : nullptr;
  QAccessibleInterface *conversationListAccessibility =
      conversation
          ? QAccessible::queryAccessibleInterface(conversation->viewport())
          : nullptr;
  require(historyAccessibility && historyAccessibility->state().disabled &&
              conversationListAccessibility &&
              conversationListAccessibility->state().busy,
          "the Shell history control and list expose graph-derived pending "
          "semantics");
#endif
  static_cast<void>(
      worker.apply({DecodedMessageKind::ClientError,
                    "thread/turns/list",
                    ProtocolRequestId("shell-history-pending"),
                    {{"code", Value(-32001)}, {"message", Value("retry")}},
                    pending.primary}));
  require(spinUntil([&] {
            return loadMore && loadMore->isEnabled() &&
                   loadMore->text() ==
                       QStringLiteral("Load earlier activities");
          }),
          "retiring the same graph Operation re-enables the same control");
#if QT_CONFIG(accessibility)
  historyAccessibility =
      loadMore ? QAccessible::queryAccessibleInterface(loadMore) : nullptr;
  require(historyAccessibility && !historyAccessibility->state().disabled &&
              conversationListAccessibility &&
              !conversationListAccessibility->state().busy,
          "history retry readiness clears the native disabled and list busy "
          "states");
#endif
  const qulonglong pagingResetsBefore = conversation->conversationModel()
                                            ->property("modelResetCount")
                                            .toULongLong();
  if (loadMore)
    loadMore->click();
  const std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  const std::size_t loadHistoryActions =
      std::ranges::count_if(messages, [&](const QtToWorkerMessage &message) {
        const auto *action = std::get_if<NodeAction>(&message);
        return action && action->kind == NodeActionKind::LoadHistory &&
               action->target == historyThread;
      });
  require(loadHistoryActions == 1 &&
              conversation->conversationModel()->rowCount() == 100 &&
              !conversation->structuralStagingActive(),
          "Load Earlier sends one provider action without changing loaded "
          "model membership");

  const ProtocolRequestId olderPageId("shell-history-older-page");
  DecodedMessage olderPageRequest{DecodedMessageKind::ClientRequest,
                                  "thread/turns/list",
                                  olderPageId,
                                  {{"threadId", Value("bounded-history")},
                                   {"cursor", Value("older-page")}}};
  olderPageRequest.requestTarget = historyThread;
  const WorkerApplyResult olderPage =
      worker.applyDetailed(std::move(olderPageRequest));
  Value::Array olderItems{
      Value(Value::Object{{"id", Value("bounded-older-root")},
                          {"type", Value("userMessage")},
                          {"text", Value("Earlier loaded prompt")}})};
  Value::Array olderTurns{
      Value(Value::Object{{"id", Value("bounded-older-turn")},
                          {"items", Value(std::move(olderItems))}})};
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ClientResult,
       "thread/turns/list",
       olderPageId,
       {{"data", Value(std::move(olderTurns))}, {"nextCursor", Value(nullptr)}},
       olderPage.primary}));
  require(spinUntil(
              [&] {
                return conversation->conversationModel()->rowCount() == 101 &&
                       !conversation->structuralStagingActive();
              },
              2000),
          "the provider page prepends its loaded row in one complete frame");
  require(conversation->materializedCardCount() <= 48 && loadMore &&
              !loadMore->isVisible(),
          "the final provider page preserves bounded QWidget residency and "
          "retires its continuation control");
  require(conversation->conversationModel()
                  ->property("modelResetCount")
                  .toULongLong() == pagingResetsBefore,
          "the provider page extends the current model without an authority "
          "reset");
}

void completedLiveAgentAppearsWithoutThreadReselection(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "live-final", "Live final");
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       {{"threadId", Value("live-final")},
        {"turn", Value(Value::Object{
                     {"id", Value("live-turn")},
                     {"items", Value(Value::Array{Value(Value::Object{
                                   {"id", Value("live-prompt")},
                                   {"type", Value("userMessage")},
                                   {"text", Value("Prompt")}})})}})}}}));
  markThreadReady(session, worker, "live-final");

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] { return threadItem(list, "live-final"); }) &&
              selectThread(list, "live-final"),
          "the live completion fixture selects its hydrated thread");
  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QAbstractScrollArea *>(
          QStringLiteral("conversationScroll")));
  require(conversation, "the live completion fixture owns a conversation");
  if (!conversation)
    return;
  auto options = conversation->presentationOptions();
  options.showCodexUpdates = false;
  conversation->setPresentationOptions(options);

  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "item/started",
       std::nullopt,
       {{"threadId", Value("live-final")},
        {"turnId", Value("live-turn")},
        {"item", Value(Value::Object{{"id", Value("live-response")},
                                     {"type", Value("agentMessage")},
                                     {"phase", Value("final_answer")}})}}}));
  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "item/agentMessage/delta",
                                  std::nullopt,
                                  {{"threadId", Value("live-final")},
                                   {"turnId", Value("live-turn")},
                                   {"itemId", Value("live-response")},
                                   {"delta", Value("Visible immediately")}}}));
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "item/completed",
       std::nullopt,
       {{"threadId", Value("live-final")},
        {"turnId", Value("live-turn")},
        {"item",
         Value(Value::Object{{"id", Value("live-response")},
                             {"type", Value("agentMessage")},
                             {"phase", Value("final_answer")},
                             {"text", Value("Visible immediately")}})}}}));
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/completed",
       std::nullopt,
       {{"threadId", Value("live-final")},
        {"turn", Value(Value::Object{{"id", Value("live-turn")}})}}}));

  require(spinUntil(
              [&] {
                middle::ConversationCard *card =
                    agentMessageCard(shell, "Visible immediately");
                return card && !card->isHidden();
              },
              1000),
          "a completed live response becomes visible without thread "
          "reselection");
}

void threadSwitchStagesTheCompleteReplacement(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  const auto addConversation = [&worker](std::string threadId,
                                         std::string title, std::string itemId,
                                         std::string text) {
    applyThread(worker, threadId, title);
    static_cast<void>(worker.apply(
        {DecodedMessageKind::ServerNotification,
         "turn/started",
         std::nullopt,
         {{"threadId", Value(threadId)},
          {"turn", Value(Value::Object{{"id", Value(threadId + "-turn")},
                                       {"status", Value("completed")}})}}}));
    static_cast<void>(worker.apply(
        {DecodedMessageKind::ServerNotification,
         "item/started",
         std::nullopt,
         {{"threadId", Value(threadId)},
          {"turnId", Value(threadId + "-turn")},
          {"item", Value(Value::Object{{"id", Value(std::move(itemId))},
                                       {"type", Value("agentMessage")},
                                       {"text", Value(std::move(text))}})}}}));
  };
  addConversation("staged-a", "Complete A", "a-item", "complete A card");
  addConversation("staged-b", "Hydrating B", "b-item", "partial B card");
  markThreadReady(session, worker, "staged-a");

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  auto *heading =
      shell.findChild<QLabel *>(QStringLiteral("conversationTitle"));
  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  require(spinUntil([&] {
            return threadItem(list, "staged-a") && threadItem(list, "staged-b");
          }),
          "staged-switch fixture exposes both canonical rows");
  require(selectThread(list, "staged-a"),
          "staged-switch fixture selects the complete source");
  static_cast<void>(takeQtMessages(channels));
  middle::ConversationCard *source = nullptr;
  require(spinUntil([&] {
            source = agentMessageCard(shell, "complete A card");
            return source && conversation &&
                   source->isVisibleTo(conversation->viewport()) &&
                   !conversation->structuralStagingActive() && heading &&
                   heading->text() == "Complete A";
          }),
          "the source conversation is complete before switching");

  require(selectThread(list, "staged-b"),
          "the hydrating replacement becomes the visible row selection");
  static_cast<void>(takeQtMessages(channels));
  auto *loading = conversation
                      ? conversation->findChild<QWidget *>(
                            QStringLiteral("conversationStagingOverlay"))
                      : nullptr;
  const QImage blankLoadingFrame =
      loading ? loading->grab().toImage() : QImage{};
  spin(350);
  require(conversation && loading && loading->isVisible() &&
              conversation->viewport()->childAt(
                  conversation->viewport()->rect().center()) == loading &&
              loading->grab().toImage() == blankLoadingFrame &&
              agentMessageCard(shell, "complete A card") == source &&
              !agentMessageCard(shell, "partial B card") && heading &&
              heading->text() == "Complete A",
          "a hydrating replacement immediately covers the outgoing message "
          "surface and exposes no partial provider cards or early spinner");
  require(spinUntil(
              [loading, blankLoadingFrame] {
                return loading &&
                       loading->grab().toImage() != blankLoadingFrame;
              },
              300),
          "a thread still loading after half a second shows the centered "
          "bounded spinner");

  middle::VisibleCardData duplicate{
      middle::AuthoritativeItemKey{"staged-b", "staged-b-turn", "duplicate"},
      middle::CardKind::AgentMessage,
      "staged-b",
      "staged-b-turn",
      "duplicate",
      middle::AgentMessageData{"Invalid duplicate", true}};
  middle::ConversationSnapshot rejected;
  rejected.threadId = "staged-b";
  rejected.sections.push_back(
      {"duplicate-a", "staged-b-turn", {duplicate}, std::nullopt});
  rejected.sections.push_back(
      {"duplicate-b", "staged-b-turn", {duplicate}, std::nullopt});
  require(conversation->reconcileStaged(std::move(rejected)) ==
              middle::ConversationView::SnapshotDisposition::Admitted,
          "the invalid selected-thread snapshot is admitted for validation");
  auto *notice =
      shell.findChild<QFrame *>(QStringLiteral("conversationNoticeBar"));
  const auto noticeText = [notice] {
    const auto labels =
        notice ? notice->findChildren<QLabel *>() : QList<QLabel *>{};
    return labels.empty() ? QString{} : labels.front()->text();
  };
  require(spinUntil([&] {
            return loading && loading->isVisible() &&
                   loading->accessibleName() ==
                       QStringLiteral("Conversation unavailable") &&
                   notice && notice->isVisible() &&
                   noticeText() ==
                       QStringLiteral("Conversation data could not be "
                                      "presented; select Reload to retry.");
          }),
          "terminal snapshot rejection stops Busy state, keeps the outgoing "
          "frame covered, and exposes one failure notice");
  const qulonglong paneCommitsAfterRejection =
      shell.property("paneCommitInvocations").toULongLong();
  spin(80);
  require(shell.property("paneCommitInvocations").toULongLong() ==
              paneCommitsAfterRejection,
          "terminal snapshot rejection does not start a pane retry loop");

  markThreadReady(session, worker, "staged-b");
  require(spinUntil([&] {
            return agentMessageCard(shell, "partial B card") &&
                   !agentMessageCard(shell, "complete A card") && heading &&
                   heading->text() == "Hydrating B" && loading &&
                   !loading->isVisible();
          }),
          "readiness replaces the staged surface once with the complete "
          "incoming conversation, matching heading, and no running spinner");
}

void threadSwitchPreservesNestedInspectorPaging(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  const auto steps = [](std::string prefix) {
    Value::Array steps;
    steps.reserve(180);
    for (int index = 0; index < 180; ++index)
      steps.emplace_back(Value::Object{
          {"step", Value(prefix + std::to_string(index))},
          {"status", Value(index % 2 == 0 ? "completed" : "pending")}});
    return steps;
  };
  applyThread(worker, "paged-plan", "Paged plan");
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       {{"threadId", Value("paged-plan")},
        {"turn", Value(Value::Object{{"id", Value("paged-plan-old-turn")},
                                     {"status", Value("completed")}})}}}));
  static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                  "turn/plan/updated",
                                  std::nullopt,
                                  {{"threadId", Value("paged-plan")},
                                   {"turnId", Value("paged-plan-old-turn")},
                                   {"plan", Value(steps("A step "))}}}));
  markThreadReady(session, worker, "paged-plan");

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] { return threadItem(list, "paged-plan"); }),
          "paginated Inspector switch fixture exposes its thread");
  require(selectThread(list, "paged-plan"),
          "paginated Inspector switch fixture selects the old incarnation");
  auto *pane = dynamic_cast<middle::InspectorPane *>(
      shell.findChild<QFrame *>(QStringLiteral("inspector")));
  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  auto *rows = pane ? pane->findChild<QAbstractScrollArea *>(
                          QStringLiteral("inspectorPlanRows"))
                    : nullptr;
  const auto visibleStep = [rows](const QString &text) {
    if (!rows)
      return false;
    for (QFrame *frame : rows->findChildren<QFrame *>(
             QStringLiteral("inspectorPlanStepFrame"))) {
      QLabel *label =
          frame->findChild<QLabel *>(QStringLiteral("planStepDescription"));
      if (label && label->text() == text && !frame->isHidden() &&
          frame->geometry().intersects(rows->viewport()->rect()))
        return true;
    }
    return false;
  };
  if (rows)
    rows->verticalScrollBar()->setValue(rows->verticalScrollBar()->maximum());
  require(spinUntil([&] { return visibleStep(QStringLiteral("A step 179")); }),
          "thread A reaches its paginated final Plan row");
  require(pane && pane->rowRequest(ui::InspectorProjection::Plan).first > 0,
          "the old incarnation retains a nonzero Plan page before replacement");

  // A same-canonical replacement lets Inspector refresh immediately while the
  // conversation still presents that canonical id. Removing this callback
  // ensures no later conversation-stage notification can mask a lost nested
  // RowViewport demand.
  if (conversation)
    conversation->setReconciliationFinishedAction({});
  NodeRef original;
  {
    auto write = graph.write();
    original = write.find({NodeKind::Thread, "paged-plan"});
    write.remove(original);
    static_cast<void>(write.finish());
  }
  NodeRef replacement;
  GraphChange recreation;
  {
    NodeState threadState;
    threadState.fields = {{"name", Value("Paged plan replacement")},
                          {"cwd", Value("/tmp")},
                          {"hydrationState", Value("ready")}};
    NodeState turnState;
    turnState.status = NodeStatus::Completed;
    turnState.fields = {{"plan", Value(steps("B step "))}};
    auto write = graph.write();
    replacement =
        write.upsert({NodeKind::Thread, "paged-plan"}, std::move(threadState));
    const NodeRef turn = write.upsert({NodeKind::Turn, "paged-plan-new-turn"},
                                      std::move(turnState));
    write.setParent(replacement, turn);
    recreation = write.finish();
  }

  const qulonglong routes = shell.property("inspectorRoutes").toULongLong();
  FrontendSessionTestPeer::deliverGraphChanged(
      session, GraphChanged{recreation.revision, {}, {}, true, {}, 0});
  require(
      original && replacement && original != replacement && rows &&
          spinUntil([&] { return rows->verticalScrollBar()->value() == 0; }),
      "the coalesced same-canonical bind resets the old Plan offset");
  require(
      spinUntil([&] {
        return visibleStep(QStringLiteral("B step 0")) &&
               shell.property("inspectorRoutes").toULongLong() > routes;
      }),
      "the nested Inspector demand loads the replacement's first Plan page");
  require(!visibleStep(QStringLiteral("A step 179")),
          "the replacement exposes no stale old-incarnation Plan renderer");
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
      shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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

  NodeRef selectedThread;
  {
    auto read = FrontendSessionTestPeer::graph(session).tryRead();
    selectedThread =
        read ? read->find({NodeKind::Thread, "inactive-thread"}) : NodeRef{};
  }
  static_cast<void>(worker.threadHydration(selectedThread, "loading"));
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  auto *send =
      shell.findChild<QPushButton *>(QStringLiteral("composerSendButton"));
  if (editor)
    editor->setPlainText(QStringLiteral("steer while history is loading"));
  require(spinUntil([&] {
            return send && send->text() == QStringLiteral("Steer") &&
                   send->isEnabled();
          }),
          "a controller can steer a known active turn with non-empty text "
          "while unrelated history hydration is still loading");
  if (send)
    send->click();
  const auto steeringActions = takeQtMessages(channels);
  const auto steering =
      std::ranges::find_if(steeringActions, [](const QtToWorkerMessage &entry) {
        const auto *action = std::get_if<NodeAction>(&entry);
        return action && action->kind == NodeActionKind::SubmitPrompt;
      });
  require(steering != steeringActions.end() &&
              std::get<NodeAction>(*steering).target == selectedThread &&
              std::get<NodeAction>(*steering).promptText ==
                  "steer while history is loading" &&
              editor && editor->toPlainText().isEmpty(),
          "clicking the enabled Steer control admits exactly one targeted "
          "prompt while hydration is loading");

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
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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

void connectionCapabilitiesRefreshOpenThreadActions(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "capability-thread", "Capability thread");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] { return threadItem(list, "capability-thread"); }),
          "the capability fixture renders its exact thread row");
  const auto openMenu = [&]() -> QMenu * {
    QTreeWidgetItem *item = threadItem(list, "capability-thread");
    if (!item)
      return nullptr;
    const QPoint point = list->visualItemRect(item).center();
    QMetaObject::invokeMethod(list, "customContextMenuRequested",
                              Qt::DirectConnection, Q_ARG(QPoint, point));
    spin(10);
    return qobject_cast<QMenu *>(QApplication::activePopupWidget());
  };
  const auto actionNamed = [](QMenu *menu, QStringView label) -> QAction * {
    if (!menu)
      return nullptr;
    for (QAction *action : menu->actions())
      if (action && action->text() == label)
        return action;
    return nullptr;
  };

  QPointer<QMenu> controllerMenu = openMenu();
  QAction *controllerRename = actionNamed(controllerMenu, u"Rename");
  const qulonglong routesBefore =
      shell.property("threadPaneRoutes").toULongLong();
  require(controllerMenu && controllerRename && controllerRename->isEnabled(),
          "controller capability enables the existing Rename action");
  static_cast<void>(worker.bridgeState("test-observer", "observer",
                                       "test-controller", 1, "ready"));
  require(spinUntil([&] {
            return (!controllerMenu || !controllerMenu->isVisible()) &&
                   shell.property("threadPaneRoutes").toULongLong() >
                       routesBefore;
          }),
          "a Connection role transition refreshes ThreadPane and closes its "
          "stale enabled menu");
  QMenu *observerMenu = openMenu();
  QAction *observerReload = actionNamed(observerMenu, u"Reload");
  QAction *observerRename = actionNamed(observerMenu, u"Rename");
  require(observerMenu && observerReload && observerReload->isEnabled() &&
              observerRename && !observerRename->isEnabled(),
          "the fresh menu derives read and control capabilities from the "
          "same Connection projection");
  if (observerMenu)
    observerMenu->close();
}

void forkActionsExposeLineageAndAdvancedOptions(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();

  makeReady(worker);
  applyThread(worker, "fork-source", "Original (fork 1)");
  applyThread(worker, "existing-child", "Original (fork 1.1)");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] { return threadItem(list, "fork-source"); }),
          "fork fixture appears in the real thread list");
  static_cast<void>(takeQtMessages(channels));

  const auto openAction = [&](QStringView label) -> QAction * {
    const QPoint point =
        list->visualItemRect(threadItem(list, "fork-source")).center();
    QMetaObject::invokeMethod(list, "customContextMenuRequested",
                              Qt::DirectConnection, Q_ARG(QPoint, point));
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return nullptr;
    for (QAction *action : menu->actions())
      if (action && action->text() == label)
        return action;
    return nullptr;
  };

  QAction *quick = openAction(u"Quick fork");
  require(quick && openAction(u"Fork with options…"),
          "thread context menu exposes Quick fork and Fork with options");
  if (QWidget *popup = QApplication::activePopupWidget())
    popup->close();
  quick = openAction(u"Quick fork");
  if (quick)
    quick->trigger();
  std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  const NodeAction *quickFork = nullptr;
  for (const QtToWorkerMessage &message : messages) {
    const auto *action = std::get_if<NodeAction>(&message);
    if (action && action->kind == NodeActionKind::Fork)
      quickFork = action;
  }
  const auto quickName = quickFork ? quickFork->payload.find("requestedName")
                                   : Value::Object::const_iterator{};
  require(quickFork && quickName != quickFork->payload.end() &&
              quickName->second.asString() &&
              *quickName->second.asString() == "Original (fork 1.2)" &&
              !quickFork->payload.contains("cwd"),
          "Quick fork sends only the correct next nested chosen name");

  bool suggestedNameVisible = false;
  bool nameDisabledForEphemeral = false;
  QAction *advanced = openAction(u"Fork with options…");
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    const auto lineEdits = dialog->findChildren<QLineEdit *>();
    const auto plainEdits = dialog->findChildren<QPlainTextEdit *>();
    QLineEdit *workspace = nullptr;
    QLineEdit *name = nullptr;
    for (QLineEdit *edit : lineEdits) {
      if (edit->text() == QStringLiteral("/tmp"))
        workspace = edit;
      else
        name = edit;
    }
    suggestedNameVisible =
        name && name->text() == QStringLiteral("Original (fork 1.2)");
    if (workspace)
      workspace->setText(QStringLiteral("/adjusted-workspace"));
    if (name)
      name->setText(QStringLiteral("Chosen advanced fork"));
    if (plainEdits.size() >= 2) {
      plainEdits[0]->setPlainText(QStringLiteral("Adjusted base"));
      plainEdits[1]->setPlainText(QStringLiteral("Adjusted developer"));
    }
    if (auto *ephemeral = dialog->findChild<QCheckBox *>()) {
      ephemeral->setChecked(true);
      nameDisabledForEphemeral =
          name && !name->isEnabled() &&
          name->text() == QStringLiteral("Chosen advanced fork");
    }
    dialog->accept();
  });
  if (advanced)
    advanced->trigger();
  messages = takeQtMessages(channels);
  const NodeAction *advancedFork = nullptr;
  for (const QtToWorkerMessage &message : messages) {
    const auto *action = std::get_if<NodeAction>(&message);
    if (action && action->kind == NodeActionKind::Fork)
      advancedFork = action;
  }
  const auto hasString = [&](std::string_view key, std::string_view value) {
    if (!advancedFork)
      return false;
    const auto found = advancedFork->payload.find(key);
    return found != advancedFork->payload.end() && found->second.asString() &&
           *found->second.asString() == value;
  };
  const auto ephemeral = advancedFork ? advancedFork->payload.find("ephemeral")
                                      : Value::Object::const_iterator{};
  require(advanced && suggestedNameVisible && nameDisabledForEphemeral &&
              advancedFork &&
              !advancedFork->payload.contains("requestedName") &&
              hasString("cwd", "/adjusted-workspace") &&
              hasString("baseInstructions", "Adjusted base") &&
              hasString("developerInstructions", "Adjusted developer") &&
              ephemeral != advancedFork->payload.end() &&
              ephemeral->second.asBool() && *ephemeral->second.asBool(),
          "Temporary Fork with options disables its name while retaining "
          "workspace and instruction fields");
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
    markThreadReady(session, worker, threadId);
  };

  makeReady(worker);
  applyConversation("selected-thread", "selected-turn", "selected-item",
                    "Selected original");
  applyConversation("background-thread", "background-turn", "background-item",
                    "Background original");
  require(spinUntil([&] {
            auto *list =
                shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
            return threadItem(list, "selected-thread") &&
                   threadItem(list, "background-thread");
          }),
          "both selected and background graph threads reach the real shell");

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  const qulonglong selectionPaneCommits =
      shell.property("paneCommitInvocations").toULongLong();
  const qulonglong selectionInspectorRoutes =
      shell.property("inspectorRoutes").toULongLong();
  require(selectThread(list, "selected-thread"),
          "the selected conversation is bound before filtering deltas");
  static_cast<void>(takeQtMessages(channels));
  require(spinUntil([&] {
            return agentMessageCard(shell, "Selected original") != nullptr;
          }),
          "the selected conversation materializes its visible agent card");
  require(
      spinUntil([&] {
        auto *title =
            shell.findChild<QLabel *>(QStringLiteral("conversationTitle"));
        auto *view = dynamic_cast<middle::ConversationView *>(
            shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
        return title && title->text() == QStringLiteral("selected-thread") &&
               view && view->presentedThreadId() == "selected-thread" &&
               shell.property("paneCommitInvocations").toULongLong() >
                   selectionPaneCommits &&
               shell.property("inspectorRoutes").toULongLong() >
                   selectionInspectorRoutes;
      }),
      "selected pane and chrome reach their observable committed state");
  middle::ConversationCard *selectedCard =
      agentMessageCard(shell, "Selected original");
  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  QTreeWidgetItem *const selectedRow = threadItem(list, "selected-thread");
  QTreeWidgetItem *const backgroundRow = threadItem(list, "background-thread");
  const QPersistentModelIndex selectedRowIndex(
      list->indexFromItem(selectedRow));
  const QPersistentModelIndex backgroundRowIndex(
      list->indexFromItem(backgroundRow));
  const int selectedRowPosition = list->indexOfTopLevelItem(selectedRow);
  const int backgroundRowPosition = list->indexOfTopLevelItem(backgroundRow);

  NodeRef selectedItem;
  NodeRef selectedTurn;
  NodeRef backgroundItem;
  NodeRef selectedThread;
  {
    auto read = graph.tryRead();
    selectedThread =
        read ? read->find({NodeKind::Thread, "selected-thread"}) : NodeRef{};
    selectedItem =
        read ? read->find(scopedItemNodeId(
                   scopedTurnNodeId("selected-thread", "selected-turn"),
                   "selected-item"))
             : NodeRef{};
    selectedTurn = read ? read->parent(selectedItem) : NodeRef{};
    backgroundItem =
        read ? read->find(scopedItemNodeId(
                   scopedTurnNodeId("background-thread", "background-turn"),
                   "background-item"))
             : NodeRef{};
  }
  require(selectedThread && selectedTurn && selectedItem && backgroundItem,
          "the test resolves the selected thread and both scoped items");
  if (!selectedThread || !selectedTurn || !selectedItem || !backgroundItem ||
      !selectedCard)
    return;

  // Selection publishes the staged conversation before its frame-coalesced
  // Inspector/chrome completion. Measure unrelated graph work only after that
  // one selection transaction has reached its existing commit boundary.
  spin(40);

  const qulonglong threadRoutesBefore =
      shell.property("threadPaneRoutes").toULongLong();
  const qulonglong conversationRoutesBefore =
      shell.property("conversationRoutes").toULongLong();
  const qulonglong targetedConversationRoutesBefore =
      shell.property("targetedConversationRoutes").toULongLong();
  const qulonglong inspectorRoutesBefore =
      shell.property("inspectorRoutes").toULongLong();
  const qulonglong shellCommitsBefore =
      shell.property("shellRenderCommits").toULongLong();
  const qulonglong conversationPassesBefore =
      conversation ? conversation->property("graphRefreshPasses").toULongLong()
                   : 0;
  const qulonglong conversationGeometryBefore =
      conversation
          ? conversation->property("conversationGeometryPasses").toULongLong()
          : 0;
  const qulonglong conversationLocalGeometryBefore =
      conversation ? conversation->property("conversationLocalGeometryPasses")
                         .toULongLong()
                   : 0;

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
              agentMessageCard(shell, "Selected current") == nullptr &&
              shell.property("threadPaneRoutes").toULongLong() ==
                  threadRoutesBefore &&
              shell.property("conversationRoutes").toULongLong() ==
                  conversationRoutesBefore &&
              shell.property("inspectorRoutes").toULongLong() ==
                  inspectorRoutesBefore &&
              shell.property("shellRenderCommits").toULongLong() ==
                  shellCommitsBefore &&
              threadItem(list, "selected-thread") == selectedRow &&
              threadItem(list, "background-thread") == backgroundRow &&
              selectedRowIndex == list->indexFromItem(selectedRow) &&
              backgroundRowIndex == list->indexFromItem(backgroundRow) &&
              list->indexOfTopLevelItem(selectedRow) == selectedRowPosition &&
              list->indexOfTopLevelItem(backgroundRow) ==
                  backgroundRowPosition &&
              list->currentItem() == selectedRow &&
              (!conversation ||
               conversation->property("graphRefreshPasses").toULongLong() ==
                   conversationPassesBefore),
          "a background-only graph delta performs no selected conversation "
          "refresh, pane route, item replacement/reordering, or shell render");

  require(messageAdmitted(
              channels.sendGraphChanged(std::move(withheldSelectedChange))),
          "the withheld selected change is admitted for comparison");
  require(spinUntil([&] {
            const auto *agent = std::get_if<middle::AgentMessageData>(
                &selectedCard->data().payload);
            return agent && agent->text == "Selected current";
          }),
          "a selected-item graph delta refreshes the existing card");
  require(
      shell.property("conversationRoutes").toULongLong() ==
              conversationRoutesBefore + 1 &&
          shell.property("targetedConversationRoutes").toULongLong() ==
              targetedConversationRoutesBefore + 1 &&
          conversation &&
          conversation->property("graphRefreshPasses").toULongLong() >
              conversationPassesBefore &&
          conversation->property("conversationGeometryPasses").toULongLong() ==
              conversationGeometryBefore &&
          conversation->property("conversationLocalGeometryPasses")
                  .toULongLong() == conversationLocalGeometryBefore &&
          shell.property("threadPaneRoutes").toULongLong() ==
              threadRoutesBefore &&
          shell.property("inspectorRoutes").toULongLong() ==
              inspectorRoutesBefore &&
          shell.property("shellRenderCommits").toULongLong() ==
              shellCommitsBefore,
      "a selected message routes only to ConversationView and leaves thread, "
      "Inspector, and shell-chrome boundaries untouched");

  const int rowsBeforeTail =
      conversation ? conversation->conversationModel()->rowCount() : 0;
  const qulonglong structuralAppendsBefore =
      conversation->conversationModel()
          ->property("modelTailAppendCount")
          .toULongLong();
  const qulonglong modelRebuildsBefore =
      conversation ? conversation->conversationModel()
                         ->property("modelIndexRebuildCount")
                         .toULongLong()
                   : 0;
  const qulonglong sectionRebuildsBefore =
      conversation ? conversation->property("conversationSectionRangeRebuilds")
                         .toULongLong()
                   : 0;
  static_cast<void>(worker.apply(
      {DecodedMessageKind::ServerNotification,
       "item/started",
       std::nullopt,
       {{"threadId", Value("selected-thread")},
        {"turnId", Value("selected-turn")},
        {"item", Value(Value::Object{{"id", Value("selected-tail")},
                                     {"type", Value("agentMessage")},
                                     {"text", Value("Selected tail")}})}}}));
  require(spinUntil([&] {
            if (!conversation ||
                conversation->conversationModel()->rowCount() !=
                    rowsBeforeTail + 1)
              return false;
            const middle::VisibleCardData *tail =
                conversation->conversationModel()->card(rowsBeforeTail);
            return tail && tail->itemId == "selected-tail";
          }),
          "one canonical selected-thread tail item reaches the item view");
  require(
      conversation->conversationModel()
                  ->property("modelTailAppendCount")
                  .toULongLong() == structuralAppendsBefore + 1 &&
          conversation->conversationModel()
                  ->property("modelIndexRebuildCount")
                  .toULongLong() == modelRebuildsBefore &&
          conversation->property("conversationSectionRangeRebuilds")
                  .toULongLong() == sectionRebuildsBefore &&
          shell.property("threadPaneRoutes").toULongLong() ==
              threadRoutesBefore &&
          shell.property("inspectorRoutes").toULongLong() ==
              inspectorRoutesBefore &&
          shell.property("shellRenderCommits").toULongLong() ==
              shellCommitsBefore,
      "one canonical tail insertion uses the bounded structural path without "
      "reindexing history or waking unrelated panes");

  const qulonglong targetedThreadRoutesBefore =
      shell.property("targetedThreadPaneRoutes").toULongLong();
  GraphChange rowChange;
  {
    auto write = graph.write();
    write.setField(selectedThread, "cwd", Value("/tmp/targeted-row"));
    rowChange = write.finish();
  }
  require(messageAdmitted(channels.sendGraphChanged(std::move(rowChange))),
          "the exact thread-row change is admitted");
  require(spinUntil([&] {
            QTreeWidgetItem *item = threadItem(list, "selected-thread");
            return item && item->toolTip(0).contains(
                               QStringLiteral("/tmp/targeted-row"));
          }),
          "the exact displayed thread row receives its changed workspace");
  require(
      shell.property("targetedThreadPaneRoutes").toULongLong() ==
              targetedThreadRoutesBefore + 1 &&
          threadItem(list, "selected-thread") == selectedRow &&
          selectedRowIndex == list->indexFromItem(selectedRow) &&
          list->indexOfTopLevelItem(selectedRow) == selectedRowPosition &&
          list->currentItem() == selectedRow &&
          conversation->property("conversationGeometryPasses").toULongLong() ==
              conversationGeometryBefore,
      "a non-sort thread field patches only its row without topology or "
      "conversation geometry work");

  static_cast<void>(takeQtMessages(channels));
  NodeRef localPrompt;
  GraphChange localPromptChange;
  {
    auto write = graph.write();
    NodeState local;
    local.status = NodeStatus::Pending;
    local.fields = {{"id", Value("local-materialization")},
                    {"type", Value("localPrompt")},
                    {"text", Value("Materialize me")},
                    {"submissionId", Value(std::uint64_t{808})},
                    {"dispatchState", Value("awaitingMaterialization")},
                    {"local", Value(true)}};
    localPrompt = write.upsert({NodeKind::Item, "local-materialization"},
                               std::move(local));
    write.setParent(selectedTurn, localPrompt);
    write.relate(selectedThread, RelationKind::PendingPrompt, localPrompt);
    localPromptChange = write.finish();
  }
  require(
      messageAdmitted(channels.sendGraphChanged(std::move(localPromptChange))),
      "the local materialization row is admitted to Qt");
  const std::string localKey = middle::stableKey(middle::LocalPromptKey{808});
  require(spinUntil([&] {
            return conversation->conversationModel()
                ->indexForStableKey(localKey)
                .isValid();
          }),
          "the local prompt reaches its stable conversation row");
  middle::ConversationCard *localCard = nullptr;
  for (middle::ConversationCard *card :
       conversation->findChildren<middle::ConversationCard *>())
    if (middle::stableKey(card->data().key) == localKey)
      localCard = card;
  const int materializationRowsBefore =
      conversation->conversationModel()->rowCount();
  const qulonglong promptSectionRebuildsBefore =
      conversation->property("conversationSectionRangeRebuilds").toULongLong();

  NodeRef authoritativePrompt;
  GraphChange materializationChange;
  {
    auto write = graph.write();
    NodeState authoritative;
    authoritative.status = NodeStatus::Completed;
    authoritative.fields = {{"id", Value("provider-materialization")},
                            {"type", Value("userMessage")},
                            {"text", Value("Materialize me")},
                            {"localSubmissionId", Value(std::uint64_t{808})}};
    authoritativePrompt = write.upsert(
        {NodeKind::Item, "provider-materialization"}, std::move(authoritative));
    write.setParent(selectedTurn, authoritativePrompt);
    write.relate(authoritativePrompt, RelationKind::PromptMaterialization,
                 localPrompt);
    materializationChange = write.finish();
  }
  require(messageAdmitted(
              channels.sendGraphChanged(std::move(materializationChange))),
          "the authoritative prompt materialization is admitted to Qt");
  require(spinUntil([&] {
            const QModelIndex index =
                conversation->conversationModel()->indexForStableKey(localKey);
            const middle::VisibleCardData *card =
                conversation->conversationModel()->card(index.row());
            return index.isValid() && card &&
                   card->kind == middle::CardKind::UserMessage &&
                   card->target == authoritativePrompt;
          }),
          "the authoritative prompt morphs the exact local row");
  const std::vector<QtToWorkerMessage> materializationActions =
      takeQtMessages(channels);
  const bool exactAcknowledgement =
      std::ranges::count_if(materializationActions, [&](const auto &message) {
        const auto *action = std::get_if<NodeAction>(&message);
        return action && action->kind == NodeActionKind::PromptMaterialized &&
               action->target == localPrompt;
      }) == 1;
  require(
      exactAcknowledgement &&
          conversation->conversationModel()->rowCount() ==
              materializationRowsBefore &&
          conversation->property("conversationSectionRangeRebuilds")
                  .toULongLong() == promptSectionRebuildsBefore &&
          (!localCard || middle::stableKey(localCard->data().key) == localKey),
      "prompt materialization targets one stable row and exact acknowledgement "
      "without structural reconciliation");

  const qulonglong retirementResetsBefore = conversation->conversationModel()
                                                ->property("modelResetCount")
                                                .toULongLong();
  const qulonglong retirementRemovalsBefore =
      conversation->conversationModel()
          ->property("modelExactRemoveCount")
          .toULongLong();
  const qulonglong retirementRoutesBefore =
      shell.property("targetedConversationRoutes").toULongLong();
  GraphChange promptRetirement;
  {
    auto write = graph.write();
    write.remove(localPrompt);
    promptRetirement = write.finish();
  }
  require(
      messageAdmitted(channels.sendGraphChanged(std::move(promptRetirement))),
      "the acknowledged prompt retirement is admitted to Qt");
  require(spinUntil([&] {
            const QModelIndex index =
                conversation->conversationModel()->indexForStableKey(localKey);
            const middle::VisibleCardData *card =
                conversation->conversationModel()->card(index.row());
            return index.isValid() && card &&
                   card->target == authoritativePrompt &&
                   shell.property("targetedConversationRoutes").toULongLong() ==
                       retirementRoutesBefore + 1;
          }),
          "retiring the prompt preserves its authoritative row as an exact "
          "structural no-op");
  require(conversation->conversationModel()
                      ->property("modelResetCount")
                      .toULongLong() == retirementResetsBefore &&
              conversation->conversationModel()
                      ->property("modelExactRemoveCount")
                      .toULongLong() == retirementRemovalsBefore,
          "prompt retirement neither resets nor removes the authoritative "
          "conversation row");

  NodeRef insertedItem;
  const qulonglong exactInsertsBefore = conversation->conversationModel()
                                            ->property("modelExactInsertCount")
                                            .toULongLong();
  const qulonglong exactMovesBefore = conversation->conversationModel()
                                          ->property("modelExactMoveCount")
                                          .toULongLong();
  const qulonglong structuralResetsBefore = conversation->conversationModel()
                                                ->property("modelResetCount")
                                                .toULongLong();
  GraphChange middleInsertion;
  {
    auto write = graph.write();
    NodeState inserted;
    inserted.status = NodeStatus::Running;
    inserted.fields = {{"id", Value("middle-structural-item")},
                       {"type", Value("agentMessage")},
                       {"text", Value("Middle structural item")}};
    insertedItem = write.upsert({NodeKind::Item, "middle-structural-item"},
                                std::move(inserted));
    write.setParent(selectedTurn, insertedItem);
    write.replaceChildren(selectedTurn,
                          std::array<NodeRef, 3>{selectedItem, insertedItem,
                                                 authoritativePrompt});
    middleInsertion = write.finish();
  }
  require(
      messageAdmitted(channels.sendGraphChanged(std::move(middleInsertion))),
      "a canonical middle insertion is admitted to Qt");
  const bool insertedExactly = spinUntil([&] {
    return conversation->conversationModel()
                   ->indexForTarget(insertedItem)
                   .row() == 1 &&
           conversation->conversationModel()
                   ->property("modelExactInsertCount")
                   .toULongLong() == exactInsertsBefore + 1 &&
           conversation->conversationModel()
                   ->property("modelExactMoveCount")
                   .toULongLong() == exactMovesBefore;
  });
  require(insertedExactly,
          "the graph middle insertion reaches its exact model row");

  GraphChange rowMove;
  {
    auto write = graph.write();
    write.replaceChildren(
        selectedTurn, std::array<NodeRef, 3>{selectedItem, authoritativePrompt,
                                             insertedItem});
    rowMove = write.finish();
  }
  require(messageAdmitted(channels.sendGraphChanged(std::move(rowMove))),
          "a canonical row reorder is admitted to Qt");
  const bool movedExactly = spinUntil([&] {
    return conversation->conversationModel()
                   ->indexForTarget(insertedItem)
                   .row() == 2 &&
           conversation->conversationModel()
                   ->property("modelExactMoveCount")
                   .toULongLong() == exactMovesBefore + 1;
  });
  require(movedExactly, "the graph reorder reaches the exact moved model row");
  require(conversation->conversationModel()
                  ->property("modelResetCount")
                  .toULongLong() == structuralResetsBefore,
          "middle insertion and movement use no conversation model reset");

  NodeRef coalescedFirst;
  NodeRef coalescedSecond;
  GraphChange firstCoalescedInsertion;
  GraphChange secondCoalescedInsertion;
  {
    auto write = graph.write();
    NodeState state;
    state.status = NodeStatus::Running;
    state.fields = {{"id", Value("coalesced-first")},
                    {"type", Value("agentMessage")},
                    {"text", Value("Coalesced first")}};
    coalescedFirst =
        write.upsert({NodeKind::Item, "coalesced-first"}, std::move(state));
    write.setParent(selectedTurn, coalescedFirst);
    write.replaceChildren(selectedTurn, std::array<NodeRef, 4>{
                                            selectedItem, coalescedFirst,
                                            authoritativePrompt, insertedItem});
    firstCoalescedInsertion = write.finish();
  }
  {
    auto write = graph.write();
    NodeState state;
    state.status = NodeStatus::Running;
    state.fields = {{"id", Value("coalesced-second")},
                    {"type", Value("agentMessage")},
                    {"text", Value("Coalesced second")}};
    coalescedSecond =
        write.upsert({NodeKind::Item, "coalesced-second"}, std::move(state));
    write.setParent(selectedTurn, coalescedSecond);
    write.replaceChildren(
        selectedTurn,
        std::array<NodeRef, 5>{selectedItem, coalescedSecond, coalescedFirst,
                               authoritativePrompt, insertedItem});
    secondCoalescedInsertion = write.finish();
  }
  require(messageAdmitted(
              channels.sendGraphChanged(std::move(firstCoalescedInsertion))) &&
              messageAdmitted(channels.sendGraphChanged(
                  std::move(secondCoalescedInsertion))),
          "two structural transactions queue before one presentation commit");
  require(spinUntil([&] {
            return conversation->conversationModel()
                           ->indexForTarget(coalescedSecond)
                           .row() == 1 &&
                   conversation->conversationModel()
                           ->indexForTarget(coalescedFirst)
                           .row() == 2;
          }) &&
              conversation->conversationModel()
                      ->property("modelExactInsertCount")
                      .toULongLong() == exactInsertsBefore + 3 &&
              conversation->conversationModel()
                      ->property("modelResetCount")
                      .toULongLong() == structuralResetsBefore,
          "coalesced structural identities retain exact final order without a "
          "snapshot fallback or model reset");

  ui::NodeGraphUiAdapter adapter(graph);
  auto divergent = adapter.conversation(selectedThread);
  const middle::CardKey canonicalFirstKey = middle::AuthoritativeItemKey{
      "selected-thread", "selected-turn", "coalesced-first"};
  bool divergenceInjected = false;
  if (divergent) {
    for (middle::TurnSection &section : divergent->sections) {
      for (middle::VisibleCardData &card : section.cards) {
        if (card.target != coalescedFirst)
          continue;
        card.target = backgroundItem;
        divergenceInjected = true;
      }
    }
  }
  const QModelIndex divergentIndex =
      conversation->conversationModel()->indexForStableKey(
          middle::stableKey(canonicalFirstKey));
  require(divergent && divergenceInjected &&
              conversation->reconcile(*divergent) ==
                  middle::ConversationView::ReconciliationResult::Changed &&
              divergentIndex.isValid() &&
              conversation->conversationModel()
                      ->card(divergentIndex.row())
                      ->target == backgroundItem,
          "the incremental-rejection fixture diverges one authoritative "
          "target without changing graph authority");
  if (!divergent || !divergenceInjected)
    return;

  const qulonglong fallbackStageStartsBefore =
      conversation->property("structuralStageStarts").toULongLong();
  const qulonglong fallbackTargetedRoutesBefore =
      shell.property("targetedConversationRoutes").toULongLong();
  const qulonglong fallbackRoutesBefore =
      shell.property("conversationRoutes").toULongLong();
  GraphChange rejectedIncremental;
  {
    auto write = graph.write();
    write.replaceChildren(
        selectedTurn,
        std::array<NodeRef, 5>{selectedItem, coalescedFirst, coalescedSecond,
                               authoritativePrompt, insertedItem});
    rejectedIncremental = write.finish();
  }
  require(messageAdmitted(
              channels.sendGraphChanged(std::move(rejectedIncremental))),
          "the structurally divergent graph change is admitted to Qt");
  require(
      spinUntil([&] {
        const QModelIndex first =
            conversation->conversationModel()->indexForStableKey(
                middle::stableKey(canonicalFirstKey));
        const QModelIndex second =
            conversation->conversationModel()->indexForTarget(coalescedSecond);
        const middle::VisibleCardData *card =
            conversation->conversationModel()->card(first.row());
        return first.isValid() && second.isValid() && first.row() == 1 &&
               second.row() == 2 && card && card->target == coalescedFirst &&
               !conversation->structuralStagingActive();
      }),
      "one authoritative replacement repairs a rejected incremental "
      "transaction");
  require(conversation->property("structuralStageStarts").toULongLong() ==
                  fallbackStageStartsBefore + 1 &&
              shell.property("targetedConversationRoutes").toULongLong() ==
                  fallbackTargetedRoutesBefore &&
              shell.property("conversationRoutes").toULongLong() ==
                  fallbackRoutesBefore + 1,
          "incremental rejection performs one full replacement and is not "
          "counted as a targeted commit");
  const qulonglong paneCommitsAfterFallback =
      shell.property("paneCommitInvocations").toULongLong();
  spin(80);
  require(shell.property("paneCommitInvocations").toULongLong() ==
              paneCommitsAfterFallback,
          "incremental rejection does not enter a pane retry loop");

  QPointer<QWidget> removedWidget = selectedCard;
  const qulonglong exactRemovalsBefore = conversation->conversationModel()
                                             ->property("modelExactRemoveCount")
                                             .toULongLong();
  const qulonglong removalResetsBefore = conversation->conversationModel()
                                             ->property("modelResetCount")
                                             .toULongLong();
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
            return removedWidget.isNull() &&
                   conversation->conversationModel()
                           ->property("modelExactRemoveCount")
                           .toULongLong() == exactRemovalsBefore + 1;
          }),
          "removed refs always remove matching selected widgets even when "
          "the change needs no structural refresh");
  require(conversation->conversationModel()
                  ->property("modelResetCount")
                  .toULongLong() == removalResetsBefore,
          "an exact selected-row removal does not reset the conversation");
}

void optimisticDraftUsesOneTypedCreateAction(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  AnimationDurationStyle normalMotion(100);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  spin(40);

  const QString chosenName = QStringLiteral("Chosen UI name");
  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  if (list)
    list->setStyle(&normalMotion);
  QTimer::singleShot(0, &shell, [chosenName] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      for (QLineEdit *editor : dialog->findChildren<QLineEdit *>()) {
        if (editor->placeholderText() == QStringLiteral("Optional thread name"))
          editor->setText(chosenName);
      }
      dialog->accept();
    }
  });
  require(newThread != nullptr, "the existing New thread control is available");
  if (!newThread)
    return;
  newThread->click();
  spin(40);

  auto *approval =
      shell.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  if (approval)
    approval->setCurrentIndex(approval->findData(QStringLiteral("never")));
  QTreeWidgetItem *draft = threadItem(list, "draft:new-thread");
  QTreeWidgetItem *const stableDraft = draft;
  auto *threadAnimation =
      shell.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  require(draft && list->currentItem() == draft && approval &&
              approval->currentData() == QStringLiteral("never") &&
              threadAccessibleText(list, draft, QAccessible::Name) ==
                  chosenName &&
              threadAnimation && threadAnimation->isActive(),
          "dialog Continue selects the named optimistic draft and starts its "
          "animation before the first prompt");

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
  const Value::Object *threadOptions = threadStart == create.payload.end()
                                           ? nullptr
                                           : threadStart->second.asObject();
  const Value::Object *turnOptions = turnStart == create.payload.end()
                                         ? nullptr
                                         : turnStart->second.asObject();
  require(creates == 1 && create.promptText == promptText.toStdString() &&
              threadOptions && turnOptions &&
              threadOptions->contains("approvalPolicy") &&
              threadOptions->at("approvalPolicy").asString() &&
              *threadOptions->at("approvalPolicy").asString() == "never" &&
              turnOptions->contains("approvalPolicy") &&
              turnOptions->at("approvalPolicy").asString() &&
              *turnOptions->at("approvalPolicy").asString() == "never",
          "the draft emits one typed CreateThread with exact authored start "
          "options");

  PromptTransition transition = worker.admitFirstPrompt(
      std::move(create), std::nullopt, QDateTime::currentMSecsSinceEpoch());
  require(
      transition.command &&
          transition.command->kind == PromptCommandKind::CreateThread,
      "worker admission creates the local thread/turn/prompt graph atomically");
  spin(60);
  const NodeRef graphDraft =
      transition.command ? transition.command->thread : NodeRef{};
  require(
      graphDraft && list && list->currentItem() == stableDraft && approval &&
          approval->currentData() == QStringLiteral("never") &&
          list->currentItem()->data(0, Qt::UserRole).toString().toStdString() ==
              graphDraft->id().canonical &&
          localPromptCard(shell, promptText.toStdString()) &&
          threadAccessibleText(list, stableDraft, QAccessible::Name) ==
              chosenName &&
          threadAnimation->isActive(),
      "the same optimistic row and chosen name hand off to the shared graph "
      "draft without stopping its animation");

  if (!transition.command)
    return;
  const NodeRef localPrompt = transition.command->localPrompt;
  require(worker.attachCreatedThread(*transition.command, "created-thread") ==
              ChannelSendStatus::Accepted,
          "the worker attaches the accepted canonical thread exactly once");
  require(spinUntil(
              [&] {
                return threadItem(list, "created-thread") == stableDraft &&
                       approval &&
                       approval->currentData() == QStringLiteral("never") &&
                       list->currentItem() == stableDraft &&
                       threadAccessibleText(list, stableDraft,
                                            QAccessible::Name) == chosenName &&
                       threadAnimation->isActive();
              },
              3000),
          "the same graph-owned row and chosen name survive canonical "
          "attachment with one continuous animation");

  static_cast<void>(
      worker.completePrompt(localPrompt, true, {}, "created-turn"));
  middle::ConversationCard *acceptedCard = nullptr;
  QTimer *acceptedAnimation = nullptr;
  require(spinUntil(
              [&] {
                acceptedCard = localPromptCard(shell, promptText.toStdString());
                acceptedAnimation =
                    acceptedCard ? acceptedCard->findChild<QTimer *>(
                                       QStringLiteral("pendingAnimationTimer"))
                                 : nullptr;
                return threadItem(list, "created-thread") == stableDraft &&
                       acceptedCard && acceptedAnimation &&
                       !acceptedAnimation->isActive() &&
                       !threadAnimation->isActive() &&
                       threadAccessibleText(list, stableDraft,
                                            QAccessible::Name) == chosenName;
              },
              3000),
          "the exact prompt result updates the canonical row without "
          "replacing its widget item or chosen name, and stops pending "
          "feedback");
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
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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

  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  static_cast<void>(takeQtMessages(channels));
  bool visiblySelectedDuringContention = false;
  bool submittedWrongDestination = false;
  {
    auto write = FrontendSessionTestPeer::graph(session).write();
    QTreeWidgetItem *existing = threadItem(list, "existing-after-empty-draft");
    if (existing)
      list->setCurrentItem(existing);
    spin(24);
    visiblySelectedDuringContention = list->currentItem() == existing;
    static_cast<void>(submit(editor, QStringLiteral("must not create")));
    for (const QtToWorkerMessage &message : takeQtMessages(channels)) {
      if (const auto *runtime = std::get_if<RuntimeAction>(&message))
        submittedWrongDestination |=
            runtime->kind == RuntimeActionKind::CreateThread;
      if (const auto *node = std::get_if<NodeAction>(&message))
        submittedWrongDestination |= node->kind == NodeActionKind::SubmitPrompt;
    }
    static_cast<void>(write.finish());
  }
  require(visiblySelectedDuringContention && !submittedWrongDestination,
          "a visibly selected graph row cannot submit the abandoned draft or "
          "another thread while exact binding is contended");
  require(spinUntil([&] {
            return list->currentItem() ==
                       threadItem(list, "existing-after-empty-draft") &&
                   !threadItem(list, "draft:new-thread");
          }),
          "the retrying exact selection binds the still-visible real row and "
          "abandons its draft");

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

void contendedUserSelectionKeepsTheLatestVisibleRow(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "selection-a", "Selection A");
  applyThread(worker, "selection-b", "Selection B");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] {
            return threadItem(list, "selection-a") &&
                   threadItem(list, "selection-b");
          }),
          "the contended-selection fixture renders both exact rows");
  nodegraph::NodeRef first;
  nodegraph::NodeRef second;
  {
    auto read = FrontendSessionTestPeer::graph(session).tryRead();
    if (read) {
      first = read->find({NodeKind::Thread, "selection-a"});
      second = read->find({NodeKind::Thread, "selection-b"});
    }
  }
  static_cast<void>(takeQtMessages(channels));
  {
    auto write = FrontendSessionTestPeer::graph(session).write();
    if (QTreeWidgetItem *item = threadItem(list, "selection-a"))
      list->setCurrentItem(item);
    spin(24);
    static_cast<void>(write.finish());
  }
  if (QTreeWidgetItem *item = threadItem(list, "selection-b"))
    list->setCurrentItem(item);
  spin(50);

  middle::ThreadPane *threadPane = nullptr;
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    threadPane = dynamic_cast<middle::ThreadPane *>(widget);
    if (threadPane)
      break;
  }
  const auto visible =
      threadPane ? threadPane->visiblySelectedThread() : std::nullopt;
  const std::vector<QtToWorkerMessage> actions = takeQtMessages(channels);
  std::size_t firstHydrates = 0;
  std::size_t secondHydrates = 0;
  for (const QtToWorkerMessage &message : actions) {
    const auto *action = std::get_if<NodeAction>(&message);
    if (!action || action->kind != NodeActionKind::Hydrate)
      continue;
    firstHydrates += action->target == first;
    secondHydrates += action->target == second;
  }
  require(first && second && visible && visible->target == second &&
              list->currentItem() == threadItem(list, "selection-b") &&
              firstHydrates == 0 && secondHydrates == 1,
          "an older contended selection retry cannot replace the newer "
          "visible exact selection");
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
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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
            QTreeWidgetItem *draft = threadItem(list, "draft:new-thread");
            return draft && draft == list->currentItem() && draft->isSelected();
          }) &&
              submit(editor, QStringLiteral("one creation in flight")),
          "the first new-thread action is admitted from its optimistic row");
  QTreeWidgetItem *const stableDraft = threadItem(list, "draft:new-thread");
  if (!stableDraft)
    return;

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
  if (!transition.command)
    return;
  const std::string localThreadId = transition.command->thread->id().canonical;
  require(threadItem(list, localThreadId) == stableDraft,
          "graph admission replaced the pre-admission thread item");
  static_cast<void>(worker.failPrompt(transition.command->localPrompt,
                                      "thread creation failed"));
  spin(40);
  QTimer *animation =
      shell.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  require(threadItem(list, localThreadId) == stableDraft,
          "graph-owned creation failure replaced its thread row");
  require(threadAccessibleText(list, stableDraft, QAccessible::Description)
              .contains(QStringLiteral("failed")),
          "graph-owned creation failure was not exposed accessibly");
  require(animation && !animation->isActive(),
          "graph-owned creation failure did not stop pending feedback");
}

void providerResetKeepsInFlightCreationGuard(Configuration &configuration) {
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
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  QTimer::singleShot(0, &shell, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  require(newThread && list && editor,
          "provider-reset creation fixture exposes the authored draft UI");
  if (!newThread || !list || !editor)
    return;
  newThread->click();
  require(submit(editor, QStringLiteral("creation survives reset")),
          "the provider-reset fixture admits one creation action");
  QTreeWidgetItem *const stableDraft = threadItem(list, "draft:new-thread");
  const bool createQueued =
      std::ranges::any_of(takeQtMessages(channels), [](const auto &message) {
        const auto *action = std::get_if<RuntimeAction>(&message);
        return action && action->kind == RuntimeActionKind::CreateThread;
      });
  static_cast<void>(worker.bridgeState("test-controller", "controller",
                                       "test-controller", 2, "ready",
                                       "provider changed during admission"));
  spin(30);

  bool secondDialogOpened = false;
  QTimer::singleShot(0, &shell, [&secondDialogOpened] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      secondDialogOpened = true;
      dialog->reject();
    }
  });
  newThread->click();
  spin(20);
  require(createQueued && stableDraft && !secondDialogOpened &&
              threadItem(list, "draft:new-thread") == stableDraft,
          "provider replacement cannot discard an admitted creation's "
          "correlation or permit a competing draft");
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

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  QTreeWidgetItem *const draft = threadItem(list, "draft:new-thread");
  auto *approval =
      shell.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  if (approval)
    approval->setCurrentIndex(approval->findData(QStringLiteral("untrusted")));
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  require(draft && approval &&
              approval->currentData() == QStringLiteral("untrusted") &&
              submit(editor, QStringLiteral("create in background")),
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
  require(
      spinUntil(
          [&] {
            return list->currentItem() ==
                       threadItem(list, "navigation-thread") &&
                   threadItem(list, "background-created") == draft &&
                   threadAccessibleText(list, draft, QAccessible::Description)
                       .contains(
                           QStringLiteral("Awaiting prompt acknowledgement"));
          },
          3000),
      "canonical creation completion preserves both row identity and "
      "later navigation");
  static_cast<void>(
      worker.completePrompt(localPrompt, true, {}, "background-turn"));
  require(
      spinUntil(
          [&] {
            return list->currentItem() ==
                       threadItem(list, "navigation-thread") &&
                   threadItem(list, "background-created") == draft &&
                   !threadAccessibleText(list, draft, QAccessible::Description)
                        .contains(
                            QStringLiteral("Awaiting prompt acknowledgement"));
          },
          3000),
      "prompt acknowledgement confirms the background row without a "
      "selection override");
  require(selectThread(list, "background-created") && approval &&
              approval->currentData() == QStringLiteral("untrusted"),
          "selecting the completed background thread restores its exact "
          "promoted settings draft");
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

  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  QTreeWidgetItem *const foregroundDraft = threadItem(list, "draft:new-thread");
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
              list->currentItem()->data(0, Qt::UserRole).toString() ==
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
            return threadItem(shell.findChild<QTreeWidget *>(
                                  QStringLiteral("threadList")),
                              "saturated-shell") != nullptr;
          }),
          "saturation fixture renders its shared thread");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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

void modalThreadActionCannotOutliveItsExactTarget(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "modal-lifetime", "Modal lifetime");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] { return threadItem(list, "modal-lifetime"); }),
          "the modal-lifetime fixture renders its exact thread");
  if (!threadItem(list, "modal-lifetime"))
    return;
  const QPoint point =
      list->visualItemRect(threadItem(list, "modal-lifetime")).center();
  QMetaObject::invokeMethod(list, "customContextMenuRequested",
                            Qt::DirectConnection, Q_ARG(QPoint, point));
  auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
  QAction *rename = nullptr;
  if (menu)
    for (QAction *action : menu->actions())
      if (action && action->text() == QStringLiteral("Rename")) {
        rename = action;
        break;
      }
  static_cast<void>(takeQtMessages(channels));
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    auto *edit = dialog ? dialog->findChild<QLineEdit *>() : nullptr;
    if (edit)
      edit->setText(QStringLiteral("Must not reach replacement"));
    static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                    "thread/deleted",
                                    std::nullopt,
                                    {{"threadId", Value("modal-lifetime")}}}));
    if (dialog)
      dialog->accept();
  });
  if (rename)
    rename->trigger();
  spin(30);
  const std::vector<QtToWorkerMessage> messages = takeQtMessages(channels);
  require(rename &&
              std::ranges::none_of(
                  messages,
                  [](const QtToWorkerMessage &message) {
                    const auto *action = std::get_if<NodeAction>(&message);
                    return action && action->kind == NodeActionKind::Rename;
                  }),
          "closing a thread inside Rename prevents the modal result from "
          "submitting or retaining an action for the retired target");
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
                shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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
              worker.selectThread(first) ==
                  ChannelSendStatus::CoalescedRescan &&
              worker.selectThread(second) == ChannelSendStatus::CoalescedRescan,
          "saturated graph updates retain one reconstructable fallback");

  for (int pass = 0;
       pass < 1024 &&
       (channels.workerToQtSizeApprox() != 0 || channels.rescanPending());
       ++pass)
    FrontendSessionTestPeer::drainWorkerMessages(session);

  require(
      spinUntil(
          [&] {
            auto *list =
                shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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
        {"isBlocking", Value(true)},
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
  static_cast<void>(takeQtMessages(channels));

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
    const auto choice = action.payload.find("choice");
    const auto input = action.payload.find("input");
    if (choice != action.payload.end() && choice->second.asString() &&
        *choice->second.asString() == "submit" &&
        input != action.payload.end() && input->second.asObject()) {
      const auto answer = input->second.asObject()->find("answer");
      if (answer != input->second.asObject()->end() &&
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
  require(prefilled && admitted.size() == 1 && exactPayload &&
              std::get<NodeAction>(*response).payload.size() == 2,
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
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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
            if (!list)
              return false;
            for (QTreeWidgetItemIterator current(list); *current; ++current) {
              const std::string id =
                  (*current)->data(0, Qt::UserRole).toString().toStdString();
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
            return threadItem(shell.findChild<QTreeWidget *>(
                                  QStringLiteral("threadList")),
                              "failed-hydration") != nullptr;
          }),
          "failed hydration fixture renders its graph thread");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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

void hiddenInspectorRequestsRetireWithTheirExactInteraction(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "hidden-request-thread", "Hidden request thread");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(
      spinUntil([&] { return threadItem(list, "hidden-request-thread"); }) &&
          selectThread(list, "hidden-request-thread"),
      "the hidden-Request fixture binds its exact thread");

  const WorkerApplyResult request =
      worker.applyDetailed({DecodedMessageKind::ServerRequest,
                            "item/commandExecution/requestApproval",
                            ProtocolRequestId("hidden-request"),
                            {{"threadId", Value("hidden-request-thread")},
                             {"turnId", Value("hidden-request-turn")},
                             {"itemId", Value("hidden-request-item")},
                             {"command", Value("make hidden-request")}}});
  middle::InspectorPane *inspector = nullptr;
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    inspector = dynamic_cast<middle::InspectorPane *>(widget);
    if (inspector)
      break;
  }
  require(request.primary && inspector,
          "the hidden-Request fixture creates one exact Interaction");
  if (!request.primary || !inspector)
    return;
  inspector->tabs()->setCurrentIndex(3);
  QPointer<QFrame> requestFrame;
  require(spinUntil([&] {
            const auto frames = inspector->findChildren<QFrame *>(
                QStringLiteral("inspectorRequestFrame"));
            if (frames.size() != 1)
              return false;
            requestFrame = frames.front();
            return true;
          }),
          "the exact Interaction materializes one Inspector Request row");
  inspector->tabs()->setCurrentIndex(0);
  spin(20);
  static_cast<void>(worker.resolveInteraction(request.primary));
  require(spinUntil([&] { return requestFrame.isNull(); }),
          "removing an Interaction retires its Request widget while that tab "
          "is hidden");
  inspector->tabs()->setCurrentIndex(3);
  require(
      inspector->findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"))
          .isEmpty(),
      "activating Requests does not materialize its retained stale page");
  spin(20);
  require(
      inspector->findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"))
          .isEmpty(),
      "returning to Requests cannot flash the retired exact action");
}

void requestAncestorMoveRetainsVisibleInteractionIdentity(
    Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  NodeGraph &graph = FrontendSessionTestPeer::graph(session);
  WorkerLogic worker(graph, channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "request-owner-a", "Request owner A");
  applyThread(worker, "request-owner-b", "Request owner B");
  applyThread(worker, "request-selected-c", "Unrelated selected C");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] {
            return threadItem(list, "request-owner-a") &&
                   threadItem(list, "request-owner-b") &&
                   threadItem(list, "request-selected-c");
          }) &&
              selectThread(list, "request-selected-c"),
          "the Request-ancestry fixture selects an unrelated exact Thread");
  QTreeWidgetItem *selectedItem = list ? list->currentItem() : nullptr;
  static_cast<void>(takeQtMessages(channels));

  const WorkerApplyResult request =
      worker.applyDetailed({DecodedMessageKind::ServerRequest,
                            "item/commandExecution/requestApproval",
                            ProtocolRequestId("moving-request"),
                            {{"threadId", Value("request-owner-a")},
                             {"turnId", Value("moving-request-turn")},
                             {"itemId", Value("moving-request-item")},
                             {"command", Value("make moving-request")}}});
  middle::InspectorPane *inspector = nullptr;
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    inspector = dynamic_cast<middle::InspectorPane *>(widget);
    if (inspector)
      break;
  }
  require(request.primary && inspector,
          "the Request-ancestry fixture creates one exact Interaction");
  if (!request.primary || !inspector || !list || !selectedItem)
    return;
  inspector->tabs()->setCurrentIndex(3);

  QPointer<QFrame> requestFrame;
  QPointer<QPushButton> accept;
  QPointer<QLabel> detail;
  QPointer<QFrame> compactAttention;
  const auto titleHasAttention = [&](std::string_view id) {
    QTreeWidgetItem *item = threadItem(list, id);
    return threadAccessibleText(list, item, QAccessible::Name)
        .startsWith(QStringLiteral("! "));
  };
  require(spinUntil([&] {
            const auto frames = inspector->findChildren<QFrame *>(
                QStringLiteral("inspectorRequestFrame"));
            if (frames.size() != 1)
              return false;
            requestFrame = frames.front();
            accept = requestFrame->findChild<QPushButton *>(
                QStringLiteral("pendingRequestAccept"));
            detail = requestFrame->findChild<QLabel *>(
                QStringLiteral("pendingRequestDetail"));
            auto *compactAccept = shell.findChild<QPushButton *>(
                QStringLiteral("pendingRequestAcceptButton"));
            compactAttention =
                compactAccept
                    ? qobject_cast<QFrame *>(compactAccept->parentWidget())
                    : nullptr;
            return accept && detail && compactAttention &&
                   detail->text().contains(
                       QStringLiteral("Thread: request-owner-a")) &&
                   detail->text().contains(
                       QStringLiteral("Title: Request owner A")) &&
                   compactAttention->accessibleDescription().contains(
                       QStringLiteral("request-owner-a")) &&
                   titleHasAttention("request-owner-a") &&
                   !titleHasAttention("request-owner-b");
          }),
          "exact ancestry supplies the initial Request context and Thread "
          "badge");
  if (!requestFrame || !accept || !detail)
    return;
  shell.activateWindow();
  accept->setFocus(Qt::TabFocusReason);
  require(spinUntil([&] { return accept && accept->hasFocus(); }),
          "the exact visible Request action accepts keyboard focus");

  NodeRef target;
  NodeRef turn;
  NodeRef ownerA;
  NodeRef ownerB;
  {
    auto read = graph.tryRead();
    target = read ? read->relatedAt(request.primary,
                                    RelationKind::InteractionTarget, 0)
                  : NodeRef{};
    turn = read ? read->parent(target) : NodeRef{};
    ownerA = read ? read->parent(turn) : NodeRef{};
    ownerB =
        read ? read->find({NodeKind::Thread, "request-owner-b"}) : NodeRef{};
  }
  require(target && target->id().kind == NodeKind::Item && turn &&
              turn->id().kind == NodeKind::Turn && ownerA && ownerB &&
              ownerA->id().canonical == "request-owner-a",
          "the Request exposes its exact Item, Turn, and Thread ancestry");
  if (!target || !turn || !ownerA || !ownerB)
    return;

#if QT_CONFIG(accessibility)
  QAccessibleInterface *frameInterface =
      QAccessible::queryAccessibleInterface(requestFrame);
  QAccessibleInterface *buttonInterface =
      QAccessible::queryAccessibleInterface(accept);
  const QAccessible::Id frameAccessibleId =
      frameInterface ? QAccessible::uniqueId(frameInterface) : 0;
  const QAccessible::Id buttonAccessibleId =
      buttonInterface ? QAccessible::uniqueId(buttonInterface) : 0;
#endif
  spin(80);
  const qulonglong paneCommitsBefore =
      shell.property("paneCommitInvocations").toULongLong();
  const qulonglong threadRoutesBefore =
      shell.property("threadPaneRoutes").toULongLong();
  const qulonglong targetedThreadRoutesBefore =
      shell.property("targetedThreadPaneRoutes").toULongLong();
  const qulonglong inspectorRoutesBefore =
      shell.property("inspectorRoutes").toULongLong();
  const qulonglong conversationRoutesBefore =
      shell.property("conversationRoutes").toULongLong();
  const qulonglong shellCommitsBefore =
      shell.property("shellRenderCommits").toULongLong();

  GraphChange moved;
  {
    auto write = graph.write();
    write.setParent(ownerB, turn);
    moved = write.finish();
  }
  require(messageAdmitted(channels.sendGraphChanged(std::move(moved))),
          "the exact Request ancestor move reaches the shell");
  require(spinUntil([&] {
            return requestFrame && accept && detail &&
                   detail->text().contains(
                       QStringLiteral("Thread: request-owner-b")) &&
                   detail->text().contains(
                       QStringLiteral("Title: Request owner B")) &&
                   !detail->text().contains(
                       QStringLiteral("Thread: request-owner-a")) &&
                   !titleHasAttention("request-owner-a") &&
                   titleHasAttention("request-owner-b");
          }),
          "moving the exact ancestor transfers Request context and badge");

  const auto frames = inspector->findChildren<QFrame *>(
      QStringLiteral("inspectorRequestFrame"));
  QPushButton *compactAcceptAfter = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestAcceptButton"));
  require(frames.size() == 1 && frames.front() == requestFrame &&
              requestFrame->findChild<QPushButton *>(
                  QStringLiteral("pendingRequestAccept")) == accept &&
              compactAcceptAfter &&
              compactAcceptAfter->parentWidget() == compactAttention &&
              compactAttention->accessibleDescription().contains(
                  QStringLiteral("request-owner-b")) &&
              !compactAttention->accessibleDescription().contains(
                  QStringLiteral("request-owner-a")) &&
              accept->hasFocus() && list->currentItem() == selectedItem &&
              selectedItem->isSelected(),
          "ancestry movement preserves both Request surfaces, action focus, "
          "and unrelated Thread selection");
#if QT_CONFIG(accessibility)
  frameInterface = QAccessible::accessibleInterface(frameAccessibleId);
  buttonInterface = QAccessible::accessibleInterface(buttonAccessibleId);
  require(frameAccessibleId != 0 && buttonAccessibleId != 0 && frameInterface &&
              frameInterface->object() == requestFrame &&
              frameInterface->text(QAccessible::Description)
                  .contains(QStringLiteral("request-owner-b")) &&
              !frameInterface->text(QAccessible::Description)
                   .contains(QStringLiteral("request-owner-a")) &&
              buttonInterface && buttonInterface->object() == accept &&
              buttonInterface->role() == QAccessible::Button &&
              buttonInterface->text(QAccessible::Name) ==
                  QStringLiteral("Accept") &&
              buttonInterface->state().focused,
          "ancestry movement preserves accessible object identity and "
          "updates only its authoritative context");
#endif
  require(shell.property("paneCommitInvocations").toULongLong() ==
                  paneCommitsBefore + 1 &&
              shell.property("threadPaneRoutes").toULongLong() ==
                  threadRoutesBefore + 1 &&
              shell.property("targetedThreadPaneRoutes").toULongLong() ==
                  targetedThreadRoutesBefore &&
              shell.property("inspectorRoutes").toULongLong() ==
                  inspectorRoutesBefore + 1 &&
              shell.property("conversationRoutes").toULongLong() ==
                  conversationRoutesBefore &&
              shell.property("shellRenderCommits").toULongLong() ==
                  shellCommitsBefore + 1,
          "one Request ancestry change takes one coalesced full Thread route, "
          "one Inspector route, and one chrome commit only");
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
            return threadItem(shell.findChild<QTreeWidget *>(
                                  QStringLiteral("threadList")),
                              "approval-thread") != nullptr;
          }),
          "reverse-interaction fixture renders its target thread");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
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
  const auto choice = response ? response->payload.find("choice")
                               : Value::Object::const_iterator{};
  require(
      responseCount == 1 && response && response->target == request.primary &&
          response->payload.size() == 1 && choice != response->payload.end() &&
          choice->second.asString() && *choice->second.asString() == "accept" &&
          !response->payload.contains("command") &&
          !response->payload.contains("cwd") && response->promptText.empty() &&
          response->attachments.empty(),
      "approval sends one typed action containing only authored decision data");

  if (response)
    static_cast<void>(worker.failInteractionResponse(
        request.primary, "CodexBridge rejected the response",
        response->payload));
  spin(40);
  const std::vector<QtToWorkerMessage> automaticRetry =
      takeQtMessages(channels);
  auto *review = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestReviewButton"));
  require(automaticRetry.empty() && review && review->isVisible() &&
              review->isEnabled() && !accept->isVisible(),
          "a rejected bridge response preserves Review without an automatic "
          "retry or a misleading direct action");
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    auto *buttons = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
    if (buttons)
      buttons->button(QDialogButtonBox::Ok)->click();
  });
  if (review)
    review->click();
  const std::vector<QtToWorkerMessage> deliberateRetry =
      takeQtMessages(channels);
  std::optional<NodeAction> retry;
  const std::size_t deliberateCount =
      std::ranges::count_if(deliberateRetry, [&](const auto &message) {
        const auto *candidate = std::get_if<NodeAction>(&message);
        const bool matches =
            candidate &&
            candidate->kind == NodeActionKind::ResolveInteraction &&
            candidate->target == request.primary;
        if (matches)
          retry = *candidate;
        return matches;
      });
  require(deliberateCount == 1 && retry && response &&
              retry->payload == response->payload,
          "Review deliberately resubmits the exact retained response once "
          "after bridge rejection");
  require(spinUntil([&] { return review && !review->isEnabled(); }),
          "a retained retry stays single-flight until the worker acknowledges "
          "its outcome");
  if (review)
    review->click();
  spin(20);
  require(takeQtMessages(channels).empty(),
          "a retained retry cannot be admitted twice before authoritative "
          "acknowledgement");
  if (retry)
    static_cast<void>(worker.failInteractionResponse(
        request.primary, "CodexBridge rejected the retried response",
        retry->payload));
  require(spinUntil([&] { return review && review->isEnabled(); }),
          "the authoritative retry failure restores the retained response for "
          "deliberate recovery");
}

void compactAttentionKeepsExactVisibleTarget(Configuration &configuration) {
  FrontendSession session(configuration);
  ThreadChannels &channels = FrontendSessionTestPeer::channels(session);
  WorkerLogic worker(FrontendSessionTestPeer::graph(session), channels);
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  makeReady(worker);
  applyThread(worker, "attention-identity", "Attention identity");
  auto *list = shell.findChild<QTreeWidget *>(QStringLiteral("threadList"));
  require(spinUntil([&] {
            return threadItem(list, "attention-identity") != nullptr;
          }) &&
              selectThread(list, "attention-identity"),
          "attention identity fixture selects its target thread");
  static_cast<void>(takeQtMessages(channels));

  const WorkerApplyResult first =
      worker.applyDetailed({DecodedMessageKind::ServerRequest,
                            "item/commandExecution/requestApproval",
                            ProtocolRequestId("attention-first"),
                            {{"threadId", Value("attention-identity")},
                             {"command", Value("first visible command")}}});
  const WorkerApplyResult second =
      worker.applyDetailed({DecodedMessageKind::ServerRequest,
                            "item/commandExecution/requestApproval",
                            ProtocolRequestId("attention-second"),
                            {{"threadId", Value("attention-identity")},
                             {"command", Value("second visible command")}}});
  auto *accept = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestAcceptButton"));
  QWidget *attention = accept ? accept->parentWidget() : nullptr;
  require(first.primary && second.primary && spinUntil([&] {
            return accept && accept->isVisible() && accept->isEnabled() &&
                   attention &&
                   attention->accessibleDescription().contains(
                       QStringLiteral("first visible command"));
          }),
          "compact attention initially presents the first exact request");
  if (!first.primary || !second.primary || !accept || !attention)
    return;

  shell.activateWindow();
  accept->setFocus(Qt::TabFocusReason);
  require(
      spinUntil([&] { return shell.isActiveWindow() && accept->hasFocus(); }),
      "the visible request action accepts keyboard focus");
  static_cast<void>(takeQtMessages(channels));
  static_cast<void>(worker.resolveInteraction(first.primary));

  // Until the queued graph change is rendered, the pixels still describe the
  // first request. Its exact NodeRef has ended, so activation must be a no-op;
  // it must never fall through to the newly ranked second request.
  accept->click();
  const std::vector<QtToWorkerMessage> raced = takeQtMessages(channels);
  require(std::ranges::none_of(raced,
                               [](const auto &message) {
                                 const auto *action =
                                     std::get_if<NodeAction>(&message);
                                 return action &&
                                        action->kind ==
                                            NodeActionKind::ResolveInteraction;
                               }),
          "activation during an attention change never targets a request the "
          "user has not seen");

  auto *review = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestReviewButton"));
  auto *reject = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestRejectButton"));
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  require(spinUntil([&] {
            return attention->accessibleDescription().contains(
                       QStringLiteral("second visible command")) &&
                   editor && editor->hasFocus();
          }) &&
              !accept->hasFocus() && (!review || !review->hasFocus()) &&
              (!reject || !reject->hasFocus()),
          "semantic request replacement transfers focus before controls "
          "retarget");

  accept->click();
  const std::vector<QtToWorkerMessage> visibleResponse =
      takeQtMessages(channels);
  require(std::ranges::count_if(
              visibleResponse,
              [&](const auto &message) {
                const auto *action = std::get_if<NodeAction>(&message);
                return action &&
                       action->kind == NodeActionKind::ResolveInteraction &&
                       action->target == second.primary;
              }) == 1,
          "after rendering, the compact action targets the exact visible "
          "replacement request");
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
  const PendingRequestDescriptor command =
      dialogRequest(PendingRequestKind::CommandApproval,
                    {{"command", "<b>untrusted command</b>"}});
  static_cast<void>(PendingRequestDialog::present(command, nullptr));

  bool plainUrl = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    plainUrl = std::ranges::any_of(
        dialog->findChildren<QLabel *>(), [](QLabel *label) {
          return label && label->textFormat() == Qt::PlainText &&
                 label->text().contains(QStringLiteral(
                     "https://example.invalid/\"><img src=x>")) &&
                 !label->openExternalLinks();
        });
    dialog->reject();
  });
  const PendingRequestDescriptor elicitation =
      dialogRequest(PendingRequestKind::McpElicitation,
                    {{"url", "https://example.invalid/\"><img src=x>"}});
  static_cast<void>(PendingRequestDialog::present(elicitation, nullptr));

  require(inspected && plainText,
          "server-provided request text is rendered literally");
  require(plainUrl,
          "the bounded MCP URL is literal text rather than active markup");
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
  const PendingRequestDescriptor questions = dialogRequest(
      PendingRequestKind::UserInput,
      {{"questions",
        nlohmann::json::array({{{"id", "first"},
                                {"question", "First?"},
                                {"options", nlohmann::json::array()}},
                               {{"id", "second"},
                                {"question", "Second?"},
                                {"options", nlohmann::json::array()}}})}});
  const auto questionResponse =
      PendingRequestDialog::present(questions, nullptr);
  const bool answersPreserved =
      questionResponse && questionResponse->choice == "submit" &&
      questionResponse->input["first"]["answers"] ==
          nlohmann::json::array({"Retained first answer"}) &&
      questionResponse->input["second"]["answers"] ==
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
  const PendingRequestDescriptor elicitation =
      dialogRequest(PendingRequestKind::McpElicitation,
                    {{"serverName", "test-server"},
                     {"threadId", "thread-a"},
                     {"message", "Structured response"},
                     {"mode", "form"},
                     {"requestedSchema", {{"type", "object"}}}});
  const auto mcpResponse = PendingRequestDialog::present(elicitation, nullptr);
  const bool validJsonReturned =
      mcpResponse && mcpResponse->choice == "accept" &&
      mcpResponse->input == nlohmann::json({{"accepted", true}});

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
  const PendingRequestDescriptor request = dialogRequest(
      PendingRequestKind::PermissionsApproval,
      {{"permissions", permissions}, {"reason", "test disclosure"}});
  const auto response = PendingRequestDialog::present(request, nullptr);
  require(completeDisclosure,
          "permission approval discloses known and future request fields");
  require(response && response->choice == "decline" &&
              response->input.is_null() && response->metadata.is_null(),
          "an unknown or mixed permission shape remains visible but cannot be "
          "approved");
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  auto *configuration =
      utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
  QApplication application(argc, argv);
  core::SNodeC::init(argc, argv);

  using namespace codexui::codex;
  graphNotificationsPrecedeRetirementRelease(*configuration);
  massRetirementIsSliced(*configuration);
  selectedRemovalUnbindsBeforeWorkerRetirement(*configuration);
  settingsDraftsRetireWithProviderAndThreadIncarnations(*configuration);
  delayedRetirementCannotEraseRecreatedThread(*configuration);
  slicedProviderRetirementPreservesFreshSelection(*configuration);
  applicationFontChangeRegeneratesUiGeometry(*configuration);
  splitterHandleDrivesInteractiveConversationResize(*configuration);
  removedAffectedOptimisticRetryDoesNotReadReleasedNode(*configuration);
  typedActionsAreExactOnceAndBounded(*configuration);
  qtHeartbeatSurvivesLargeInboundTraffic(*configuration);
  conversationPresentationBurstIsFrameBounded(*configuration);
  graphBackedShellPreservesDraftsAndPrompts(*configuration);
  initialHydrationRetainsAllLoadedRowsWithBoundedResidency(*configuration);
  completedLiveAgentAppearsWithoutThreadReselection(*configuration);
  threadSwitchStagesTheCompleteReplacement(*configuration);
  threadSwitchPreservesNestedInspectorPaging(*configuration);
  inactiveThreadNeverReactivatesAStaleTurn(*configuration);
  reloadAndReconnectHydrationStayExplicit(*configuration);
  connectionCapabilitiesRefreshOpenThreadActions(*configuration);
  forkActionsExposeLineageAndAdvancedOptions(*configuration);
  backgroundGraphChangesDoNotRefreshSelectedConversation(*configuration);
  optimisticDraftUsesOneTypedCreateAction(*configuration);
  emptyOptimisticDraftIsAbandonedOnThreadSelection(*configuration);
  contendedUserSelectionKeepsTheLatestVisibleRow(*configuration);
  secondNewThreadIsGuardedWhileCreationIsInFlight(*configuration);
  providerResetKeepsInFlightCreationGuard(*configuration);
  optimisticCreationDoesNotOverrideLaterNavigation(*configuration);
  optimisticCreationIgnoresAnotherCreationCorrelation(*configuration);
  saturatedShellKeepsTheEditorDraft(*configuration);
  saturatedRenameRetainsAuthoredName(*configuration);
  modalThreadActionCannotOutliveItsExactTarget(*configuration);
  saturatedWorkerEffectsKeepNewestUiState(*configuration);
  saturatedInteractionRetainsAuthoredInput(*configuration);
  recoveryOnlyPromptRestoresToComposer(*configuration);
  providerNoticesReachTheTransientSurface(*configuration);
  failedHydrationKeepsTheEditorDraft(*configuration);
  hiddenInspectorRequestsRetireWithTheirExactInteraction(*configuration);
  requestAncestorMoveRetainsVisibleInteractionIdentity(*configuration);
  reverseInteractionCarriesOnlyAuthoredResponse(*configuration);
  compactAttentionKeepsExactVisibleTarget(*configuration);
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
