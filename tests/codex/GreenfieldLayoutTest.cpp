// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PresentationModel.h"
#include "codex/PresentationProtocol.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QApplication>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QThread>
#include <QToolButton>
#include <QWheelEvent>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace codexui::codex::middle {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

bool hasLabelContaining(const QWidget &root, const QString &text) {
  for (const QLabel *label : root.findChildren<QLabel *>()) {
    if (label->text().contains(text))
      return true;
  }
  return false;
}

void spin(int milliseconds = 0) {
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (milliseconds > 0)
      QThread::msleep(1);
  } while (timer.elapsed() < milliseconds);
}

VisibleCardData textCard(const std::string &thread, int index) {
  const std::string turn = index < 15 ? "turn-1" : "turn-2";
  const std::string item = "item-" + std::to_string(index);
  return {AuthoritativeItemKey{thread, turn, item},
          CardKind::AgentMessage,
          thread,
          turn,
          item,
          AgentMessageData{
              QStringLiteral("A materialized response line %1 with enough "
                             "content to occupy normal card height.")
                  .arg(index),
              false}};
}

ConversationSnapshot longConversation(const std::string &thread) {
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections = {{"turn-one", "turn-1", {}}, {"turn-two", "turn-2", {}}};
  for (int index = 0; index < 30; ++index)
    snapshot.sections[index < 15 ? 0 : 1].cards.push_back(
        textCard(thread, index));
  return snapshot;
}

QWheelEvent wheelFor(QWidget *target, int pixelDelta) {
  const QPointF local(target->rect().center());
  return QWheelEvent(local, target->mapToGlobal(local.toPoint()), QPoint(),
                     QPoint(0, pixelDelta), Qt::NoButton, Qt::NoModifier,
                     Qt::ScrollUpdate, false);
}

std::vector<std::string> threadOrder(const ThreadPane &pane) {
  const auto *list =
      pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  std::vector<std::string> result;
  if (!list)
    return result;
  result.reserve(static_cast<std::size_t>(list->count()));
  for (int row = 0; row < list->count(); ++row)
    result.push_back(
        list->item(row)->data(Qt::UserRole).toString().toStdString());
  return result;
}

bool testOverlayGeometryAndRegionRouting() {
  MiddleRegionWidget region;
  bool result =
      expect(region.composer().extraOverlayHeight() == 0 &&
                 region.conversation().trailingSpaceHeight() == 0,
             "composer construction reports no pre-canonical trailing space");
  region.resize(1500, 820);
  region.show();
  spin(20);

  QSplitter *splitter = region.splitterWidget();
  result &= expect(splitter->count() == 3 && splitter->handleWidth() == 8,
                   "middle region keeps the three-pane splitter geometry");
  result &= expect(splitter->widget(0)->minimumWidth() == 220 &&
                       splitter->widget(0)->maximumWidth() == 440 &&
                       splitter->widget(1)->minimumWidth() == 480 &&
                       splitter->widget(2)->minimumWidth() == 300 &&
                       splitter->widget(2)->maximumWidth() == 520,
                   "pane width constraints match the visual contract");

  ConversationView &view = region.conversation();
  view.reconcile(longConversation("layout-thread"));
  spin(20);
  const QRect viewGeometry = view.geometry();
  const QRect viewportGeometry = view.viewport()->geometry();
  const int canonical = region.composer().canonicalReserveHeight();
  result &=
      expect(canonical > 0 &&
                 region.composer().canonicalReserve()->height() == canonical,
             "composer establishes one compact canonical reserve");

  QString longPrompt;
  for (int line = 0; line < 14; ++line)
    longPrompt += QStringLiteral("A deliberately long prompt line %1 that "
                                 "grows the editor upward.\n")
                      .arg(line);
  region.composer().promptEditor()->setPlainText(longPrompt);
  spin(30);
  const int extra = region.composer().extraOverlayHeight();
  result &= expect(extra > 0 && view.trailingSpaceHeight() == extra,
                   "prompt growth is mirrored by exact trailing scroll space");
  result &=
      expect(view.geometry() == viewGeometry &&
                 view.viewport()->geometry() == viewportGeometry &&
                 region.composer().canonicalReserve()->height() == canonical,
             "prompt growth overlays without shifting the message viewport");
  region.composer().clearDraft();
  spin(30);
  result &= expect(
      region.composer().extraOverlayHeight() == 0 &&
          view.trailingSpaceHeight() == 0 && view.geometry() == viewGeometry &&
          view.viewport()->geometry() == viewportGeometry,
      "prompt contraction restores canonical layout and removes space");

  ComposerPane::Actions rejected;
  rejected.submit = [](QString, std::vector<AttachmentDraft>) { return false; };
  region.composer().setActions(std::move(rejected));
  region.composer().promptEditor()->setPlainText(
      QStringLiteral("must survive rejected admission"));
  QMetaObject::invokeMethod(region.composer().promptEditor(), "submitRequested",
                            Qt::DirectConnection);
  result &= expect(region.composer().promptEditor()->toPlainText() ==
                       QStringLiteral("must survive rejected admission"),
                   "rejected admission preserves the complete composer draft");
  ComposerPane::Actions accepted;
  accepted.submit = [](QString, std::vector<AttachmentDraft>) { return true; };
  region.composer().setActions(std::move(accepted));
  QMetaObject::invokeMethod(region.composer().promptEditor(), "submitRequested",
                            Qt::DirectConnection);
  result &= expect(region.composer().promptEditor()->toPlainText().isEmpty(),
                   "successful local admission clears the draft exactly once");

  result &= expect(view.isAtBottom(), "conversation begins at the bottom");
  QWheelEvent overLeftHandle = wheelFor(splitter->handle(1), 180);
  result &=
      expect(region.routeScrollEvent(splitter->handle(1), &overLeftHandle) &&
                 view.mode() == ConversationView::Mode::Paused,
             "the left middle splitter handle routes wheel input");
  QWheelEvent overRightHandle = wheelFor(splitter->handle(2), 180);
  const int beforeRight = view.verticalScrollBar()->value();
  result &=
      expect(region.routeScrollEvent(splitter->handle(2), &overRightHandle) &&
                 view.verticalScrollBar()->value() < beforeRight,
             "the right middle splitter handle routes wheel input");
  return result;
}

bool testThreadSelectionProjection() {
  PresentationModel model;
  model.applyEvent(presentation::event(
      1, 1, "thread.upsert", {{"thread", {{"id", "thread-a"}, {"name", "A"}}}},
      presentation::Authority::Merge, {{"threadId", "thread-a"}}));
  model.applyEvent(presentation::event(
      2, 1, "thread.upsert", {{"thread", {{"id", "thread-b"}, {"name", "B"}}}},
      presentation::Authority::Merge, {{"threadId", "thread-b"}}));

  ThreadPane pane;
  pane.refresh(model, "thread-a");
  bool result = expect(pane.visiblySelectedThreadId() == "thread-a",
                       "thread selection is projected from Shell state");
  pane.refresh(model, "draft:new-thread");
  result &= expect(pane.visiblySelectedThreadId().empty(),
                   "a New Thread draft cannot retain an old visible row");

  model.applyEvent(presentation::event(3, 1, "agents.activity.upsert",
                                       {{"activity",
                                         {{"id", "thread-b"},
                                          {"type", "subAgentActivity"},
                                          {"status", "inProgress"},
                                          {"agentThreadId", "thread-b"}}}},
                                       presentation::Authority::Merge,
                                       {{"threadId", "thread-a"},
                                        {"turnId", "turn-a"},
                                        {"itemId", "thread-b"}}));
  pane.refresh(model, "thread-b");
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  QListWidgetItem *selected = list ? list->currentItem() : nullptr;
  result &= expect(
      selected &&
          selected->data(Qt::UserRole).toString() ==
              QStringLiteral("thread-b") &&
          pane.visiblySelectedThreadId() == "thread-b",
      "a hydrated selected child thread remains visible outside root ordering");
  QWidget *row = selected && list ? list->itemWidget(selected) : nullptr;
  auto *title =
      row ? row->findChild<QLabel *>(QStringLiteral("threadTitle")) : nullptr;
  auto *status =
      row ? row->findChild<QLabel *>(QStringLiteral("threadStatus")) : nullptr;
  auto *dot =
      row ? row->findChild<QFrame *>(QStringLiteral("threadStatusDot"))
          : nullptr;
  auto *rowLayout = row ? qobject_cast<QHBoxLayout *>(row->layout()) : nullptr;
  auto *sortButton =
      pane.findChild<QToolButton *>(QStringLiteral("threadSortButton"));
  result &= expect(
      selected && selected->sizeHint().height() == 54 && rowLayout &&
          rowLayout->contentsMargins() == QMargins(5, 2, 5, 2) &&
          rowLayout->spacing() == 8 && title && status && dot &&
          dot->size() == QSize(10, 10) && rowLayout->indexOf(dot) >= 0 &&
          sortButton &&
          sortButton->property("codexChevron").toBool() &&
          title->property("kind").toString() == QStringLiteral("title") &&
          status->property("kind").toString() == QStringLiteral("meta") &&
          title->textInteractionFlags().testFlag(Qt::TextSelectableByMouse) &&
          status->textInteractionFlags().testFlag(Qt::TextSelectableByMouse),
      "thread cards keep their status dot and shared chevron styling inside "
      "the UI contract");
  pane.refresh(model, "thread-a");
  bool retainedSupplement = false;
  if (list) {
    for (int index = 0; index < list->count(); ++index) {
      retainedSupplement |= list->item(index)->data(Qt::UserRole).toString() ==
                            QStringLiteral("thread-b");
    }
  }
  result &=
      expect(retainedSupplement && pane.visiblySelectedThreadId() == "thread-a",
             "a previously selected retained thread survives navigation");
  model.applyEvent(presentation::event(
      4, 1, "thread.removed", nlohmann::json::object(),
      presentation::Authority::Remove, {{"threadId", "thread-b"}}));
  pane.refresh(model, "thread-a");
  bool retainedAfterRemoval = false;
  if (list) {
    for (int index = 0; index < list->count(); ++index) {
      retainedAfterRemoval |=
          list->item(index)->data(Qt::UserRole).toString() ==
          QStringLiteral("thread-b");
    }
  }
  result &= expect(!retainedAfterRemoval,
                   "an authoritative removal drops a retained thread");
  return result;
}

bool testThreadAlphanumericSort() {
  PresentationModel model;
  model.applyEvent(presentation::result(
      1, 1, "threads.list", "alpha-threads", true,
      {{"threads",
        nlohmann::json::array({{{"id", "alpha"}, {"name", "Alpha"}},
                               {{"id", "ten"}, {"name", "10 Release"}},
                               {{"id", "two"}, {"name", "2 Review"}},
                               {{"id", "one"}, {"name", "1 Setup"}},
                               {{"id", "beta"}, {"name", "beta"}}})}},
      presentation::Authority::Merge));
  ThreadPane pane;
  pane.setSortCriterion(ThreadPane::SortCriterion::Alphanumeric);
  pane.refresh(model, "two");
  return expect(threadOrder(pane) ==
                        std::vector<std::string>(
                            {"one", "two", "ten", "alpha", "beta"}) &&
                    pane.visiblySelectedThreadId() == "two",
                "Alphanumeric sorting is natural and preserves selection");
}

bool testThreadCreatedSort() {
  PresentationModel model;
  model.applyEvent(presentation::result(
      1, 1, "threads.list", "created-threads", true,
      {{"threads",
        nlohmann::json::array({{{"id", "old"}, {"createdAt", 10}},
                               {{"id", "missing"}},
                               {{"id", "new"}, {"createdAt", 30}},
                               {{"id", "middle"}, {"createdAt", 20}}})}},
      presentation::Authority::Merge));
  ThreadPane pane;
  pane.setSortCriterion(ThreadPane::SortCriterion::Created);
  pane.refresh(model, {});
  return expect(threadOrder(pane) == std::vector<std::string>(
                                         {"new", "middle", "old", "missing"}),
                "Created sorting is newest first with missing values last");
}

bool testThreadLastChangedSort() {
  PresentationModel model;
  model.applyEvent(presentation::result(
      1, 1, "threads.list", "changed-threads", true,
      {{"threads",
        nlohmann::json::array({{{"id", "first"}, {"updatedAt", 20}},
                               {{"id", "second"}, {"updatedAt", 10}},
                               {{"id", "third"}, {"updatedAt", 30}}})}},
      presentation::Authority::Merge));
  model.applyEvent(presentation::event(
      2, 1, "thread.upsert",
      {{"thread", {{"id", "first"}, {"name", "Renamed"}}}},
      presentation::Authority::Merge, {{"threadId", "first"}}));
  ThreadPane pane;
  pane.setSortCriterion(ThreadPane::SortCriterion::LastChanged);
  pane.refresh(model, {});
  return expect(threadOrder(pane) ==
                    std::vector<std::string>({"third", "first", "second"}),
                "Last changed sorting uses retained updated timestamps");
}

bool testThreadRecencySort() {
  PresentationModel model;
  model.applyEvent(presentation::result(
      1, 1, "threads.list", "recent-threads", true,
      {{"threads",
        nlohmann::json::array({{{"id", "older"}, {"recencyAt", 10}},
                               {{"id", "recent"}, {"recencyAt", 30}},
                               {{"id", "middle"}, {"recencyAt", 20}}})}},
      presentation::Authority::Merge));
  ThreadPane pane;
  pane.refresh(model, "older");
  return expect(
      pane.currentSortCriterion() == ThreadPane::SortCriterion::Recency &&
          threadOrder(pane) ==
              std::vector<std::string>({"recent", "middle", "older"}) &&
          pane.visiblySelectedThreadId() == "older",
      "Recent is the default and preserves selection");
}

bool testThreadRowReorderOwnership() {
  PresentationModel model;
  model.applyEvent(presentation::event(
      1, 1, "thread.upsert", {{"thread", {{"id", "thread-a"}, {"name", "A"}}}},
      presentation::Authority::Merge, {{"threadId", "thread-a"}}));
  model.applyEvent(presentation::event(
      2, 1, "thread.upsert", {{"thread", {{"id", "thread-b"}, {"name", "B"}}}},
      presentation::Authority::Merge, {{"threadId", "thread-b"}}));

  ThreadPane pane;
  int selectedByUser = 0;
  ThreadPane::Actions actions;
  actions.select = [&](const std::string &) { ++selectedByUser; };
  pane.setActions(std::move(actions));
  pane.setSortCriterion(ThreadPane::SortCriterion::Alphanumeric);
  pane.resize(320, 500);
  pane.show();
  pane.refresh(model, "thread-a");
  spin(20);
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  QListWidgetItem *threadA = nullptr;
  QListWidgetItem *threadB = nullptr;
  if (list) {
    for (int row = 0; row < list->count(); ++row) {
      if (list->item(row)->data(Qt::UserRole).toString() ==
          QStringLiteral("thread-a")) {
        threadA = list->item(row);
      } else if (list->item(row)->data(Qt::UserRole).toString() ==
                 QStringLiteral("thread-b")) {
        threadB = list->item(row);
      }
    }
  }
  bool result = expect(list && threadA && threadB,
                       "the stable thread row exists before list reordering");
  if (!list || !threadA || !threadB)
    return false;
  const QPoint rightClickPosition = list->visualItemRect(threadB).center();
  QMouseEvent rightClick(QEvent::MouseButtonPress, rightClickPosition,
                         list->viewport()->mapToGlobal(rightClickPosition),
                         Qt::RightButton, Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &rightClick);
  QContextMenuEvent contextMenuEvent(
      QContextMenuEvent::Mouse, rightClickPosition,
      list->viewport()->mapToGlobal(rightClickPosition));
  QApplication::sendEvent(list->viewport(), &contextMenuEvent);
  result &= expect(pane.visiblySelectedThreadId() == "thread-a" &&
                       selectedByUser == 0 &&
                       threadB->data(Qt::UserRole + 1).toBool(),
                   "right-click highlights row actions without selecting a "
                   "thread");
  if (QWidget *popup = QApplication::activePopupWidget())
    popup->close();
  spin();
  result &= expect(!threadB->data(Qt::UserRole + 1).toBool(),
                   "closing row actions clears the native context hover");
  QPointer<QWidget> originalRow = list->itemWidget(threadB);

  model.applyEvent(presentation::result(
      3, 1, "threads.list", "reordered-threads", true,
      {{"threads",
        nlohmann::json::array({{{"id", "thread-a"}, {"name", "Z"}},
                               {{"id", "thread-b"}, {"name", "B"}}})}},
      presentation::Authority::Replace));
  pane.refresh(model, "thread-a");
  QPointer<QWidget> movedRow = list->itemWidget(threadB);
  result &= expect(originalRow && movedRow && originalRow != movedRow,
                   "moving an item never reattaches its deferred-delete row");
  if (!originalRow || !movedRow || originalRow == movedRow)
    return false;

  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  spin(20);
  result &= expect(originalRow.isNull() && movedRow &&
                       list->itemWidget(threadB) == movedRow,
                   "deferred deletion cannot invalidate the moved thread row");
  list->setCurrentItem(threadA);
  list->viewport()->repaint();
  spin(20);
  result &= expect(pane.visiblySelectedThreadId() == "thread-a",
                   "the reordered row remains selectable after repaint");
  return result;
}

bool testNestedCommandScrollOwnership() {
  MiddleRegionWidget region;
  region.resize(1500, 820);
  region.show();
  ConversationSnapshot snapshot = longConversation("command-thread");
  QString output;
  for (int line = 0; line < 100; ++line)
    output += QStringLiteral("command output line %1\n").arg(line);
  snapshot.sections.back().cards.push_back(
      {AuthoritativeItemKey{"command-thread", "turn-2", "command"},
       CardKind::CommandExecution, "command-thread", "turn-2", "command",
       CommandExecutionData{QStringLiteral("run-command"),
                            output,
                            QStringLiteral("inProgress"),
                            {},
                            std::nullopt}});
  region.conversation().reconcile(snapshot);
  spin(30);

  CommandOutputView *commandOutput = nullptr;
  for (QWidget *widget : region.findChildren<QWidget *>())
    if (auto *candidate = dynamic_cast<CommandOutputView *>(widget)) {
      commandOutput = candidate;
      break;
    }
  bool result =
      expect(commandOutput && commandOutput->verticalScrollBar()->maximum() > 0,
             "long command output owns a real nested scrollbar");
  if (!commandOutput)
    return false;
  commandOutput->verticalScrollBar()->setValue(
      commandOutput->verticalScrollBar()->maximum() / 2);
  spin();
  QWheelEvent owned = wheelFor(commandOutput, 120);
  result &= expect(!region.routeScrollEvent(commandOutput, &owned),
                   "a nested output consumes input while it can scroll");
  commandOutput->verticalScrollBar()->setValue(
      commandOutput->verticalScrollBar()->minimum());
  spin();
  const int outerBefore = region.conversation().verticalScrollBar()->value();
  QWheelEvent handedOff = wheelFor(commandOutput, 120);
  result &= expect(region.routeScrollEvent(commandOutput, &handedOff) &&
                       region.conversation().verticalScrollBar()->value() <
                           outerBefore,
                   "nested output hands input to the message view at its edge");
  return result;
}

bool testInfoViewerLayout() {
  InspectorPane inspector;
  inspector.resize(420, 700);
  inspector.show();
  PresentationModel model;
  inspector.refresh(model, {});
  inspector.tabs()->setCurrentIndex(4);
  auto *infoStack =
      inspector.findChild<QStackedWidget *>(QStringLiteral("infoStack"));
  auto *protocolChoice = inspector.findChild<QPushButton *>(
      QStringLiteral("protocolInfoChoice"));
  auto *protocol =
      inspector.findChild<QPlainTextEdit *>(QStringLiteral("protocolInfoLog"));
  auto *state =
      inspector.findChild<QPlainTextEdit *>(QStringLiteral("stateInfoView"));
  auto *statistics =
      inspector.findChild<QLabel *>(QStringLiteral("protocolInfoStats"));
  bool result = expect(infoStack && protocolChoice && protocol && state && statistics,
                       "Info exposes State and Protocol through choice navigation");
  if (!infoStack || !protocolChoice || !protocol || !state || !statistics)
    return false;
  protocolChoice->click();
  inspector.appendProtocolFrame(
      {{"kind", "event"},
       {"type", "conversation.item.upsert"},
       {"sequence", 1},
       {"generation", 1},
       {"authority", "app-server"},
       {"scope", {{"threadId", "thread"}, {"itemId", "item"}}}});
  inspector.appendProtocolFrame(
      {{"kind", "result"},
       {"action", "thread.read"},
       {"sequence", 2},
       {"generation", 1},
       {"authority", "app-server"},
       {"ok", false},
       {"error", {{"message", "thread hydration failed"}}},
       {"scope", {{"threadId", "thread"}}}});
  for (int sequence = 3; sequence <= 90; ++sequence) {
    inspector.appendProtocolFrame(
        {{"kind", "event"},
         {"type",
          QStringLiteral("protocol.test.%1").arg(sequence).toStdString()},
         {"sequence", sequence},
         {"generation", 1},
         {"authority", "app-server"},
         {"scope", {{"threadId", "thread"}}}});
  }
  inspector.refresh(model, {});
  spin(20);
  result &=
      expect(protocol->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded &&
                 state->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
             "both Info viewers use the common as-needed scrollbar policy");
  result &=
      expect(protocol->verticalScrollBar()->property("kind") == "infoViewer" &&
                 state->verticalScrollBar()->property("kind") == "infoViewer",
             "both Info viewer scrollbars use the shared visual style");
  result &= expect(protocol->toPlainText().contains(
                       QStringLiteral("thread hydration failed")),
                   "failed protocol results retain their error detail");
  QScrollBar *protocolScroll = protocol->verticalScrollBar();
  result &= expect(protocolScroll->maximum() > 0 &&
                       protocolScroll->value() == protocolScroll->maximum(),
                   "Protocol follows new frames while already at the tail");
  protocolScroll->setValue(protocolScroll->maximum() / 3);
  spin();
  const int pausedValue = protocolScroll->value();
  inspector.appendProtocolFrame({{"kind", "event"},
                                 {"type", "protocol.test.visible-append"},
                                 {"sequence", 91},
                                 {"generation", 1},
                                 {"authority", "app-server"}});
  inspector.refresh(model, {});
  spin(20);
  result &=
      expect(protocolScroll->value() == pausedValue,
             "a visible Protocol append preserves a user-paused position");
  infoStack->setCurrentIndex(0);
  inspector.appendProtocolFrame({{"kind", "event"},
                                 {"type", "protocol.test.hidden-append"},
                                 {"sequence", 92},
                                 {"generation", 1},
                                 {"authority", "app-server"}});
  protocolChoice->click();
  spin(20);
  result &=
      expect(protocolScroll->value() == pausedValue,
             "Protocol refresh preserves its paused position across tabs");
  protocolScroll->setValue(protocolScroll->maximum());
  inspector.appendProtocolFrame({{"kind", "event"},
                                 {"type", "protocol.test.following-append"},
                                 {"sequence", 93},
                                 {"generation", 1},
                                 {"authority", "app-server"}});
  spin(20);
  result &=
      expect(protocolScroll->value() == protocolScroll->maximum(),
             "Protocol continues following when an append starts at the tail");
  result &=
      expect(!statistics->text().isEmpty() &&
                 statistics->geometry().top() >= protocol->geometry().bottom(),
             "Protocol statistics are laid out below the expanding log");
  return result;
}

bool testInspectorDetailParity() {
  PresentationModel model;
  model.applyEvent(presentation::event(
      1, 1, "thread.upsert", {{"thread", {{"id", "owner-thread"}}}},
      presentation::Authority::Merge, {{"threadId", "owner-thread"}}));
  model.applyEvent(presentation::event(
      2, 1, "agents.activity.upsert",
      {{"activity",
        {{"id", "agent-one"},
         {"type", "subAgentActivity"},
         {"status", "inProgress"},
         {"agentThreadId", "child-thread"},
         {"senderThreadId", "sender-thread"},
         {"receiverThreadIds",
          nlohmann::json::array({"receiver-one", "receiver-two"})}}}},
      presentation::Authority::Merge,
      {{"threadId", "owner-thread"},
       {"turnId", "turn-one"},
       {"itemId", "agent-one"}}));
  model.applyEvent(presentation::event(
      3, 1, "pending-request.upsert",
      {{"requestId", "request-one"},
       {"category", "userInput"},
       {"request",
        {{"message", "Choose an option"},
         {"questions", nlohmann::json::array({1, 2, 3})}}}},
      presentation::Authority::Merge,
      {{"threadId", "owner-thread"}, {"requestId", "request-one"}}));

  InspectorPane inspector;
  inspector.resize(420, 700);
  inspector.show();
  inspector.refresh(model, "owner-thread");
  inspector.tabs()->setCurrentIndex(1);
  spin(20);
  bool result = expect(
      hasLabelContaining(
          inspector,
          QStringLiteral("thread child-thread  |  sender sender-thread  |  "
                         "receivers receiver-one, receiver-two")),
      "Agents show child, sender, and receiver thread identities");
  inspector.tabs()->setCurrentIndex(3);
  spin(20);
  result &= expect(hasLabelContaining(inspector, QStringLiteral("3 questions")),
                   "Requests show their retained question count");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  bool result = testOverlayGeometryAndRegionRouting();
  result &= testThreadSelectionProjection();
  result &= testThreadAlphanumericSort();
  result &= testThreadCreatedSort();
  result &= testThreadLastChangedSort();
  result &= testThreadRecencySort();
  result &= testThreadRowReorderOwnership();
  result &= testNestedCommandScrollOwnership();
  result &= testInfoViewerLayout();
  result &= testInspectorDetailParity();
  if (result)
    std::cout << "Greenfield layout tests passed\n";
  return result ? 0 : 1;
}
