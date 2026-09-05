// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTextCursor>

#include <iostream>
#include <string>

namespace codexui::codex::middle {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

void sendKey(codexui::ExpandingPromptEditor &editor, int key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers);
  QCoreApplication::sendEvent(&editor, &event);
}

bool promptKeyboardAndFocusContract() {
  codexui::ExpandingPromptEditor editor;
  editor.resize(480, 80);
  editor.show();
  editor.setFocus();
  QCoreApplication::processEvents();
  int submissions = 0;
  QObject::connect(&editor, &codexui::ExpandingPromptEditor::submitRequested,
                   [&] { ++submissions; });
  editor.setPlainText(QStringLiteral("prompt"));
  editor.moveCursor(QTextCursor::End);
  sendKey(editor, Qt::Key_Return);
  bool result = expect(submissions == 1 && editor.hasFocus(),
                       "Return submits without losing prompt focus");
  editor.setPlainText(QStringLiteral("prompt"));
  editor.moveCursor(QTextCursor::End);
  sendKey(editor, Qt::Key_Return, Qt::ShiftModifier);
  result &= expect(submissions == 1 &&
                       editor.toPlainText() == QStringLiteral("prompt\n"),
                   "Shift+Return inserts a newline without submission");
  result &= expect(editor.accessibleName() == QStringLiteral("Message Codex"),
                   "the prompt retains its accessible identity");
  return result;
}

ui::ThreadListRow row(std::string id, std::string title,
                      std::string status = {}) {
  ui::ThreadListRow result;
  result.id = std::move(id);
  result.title = std::move(title);
  result.status = std::move(status);
  result.cwd = "/workspace";
  return result;
}

bool threadPaneSnapshotAndActionContract() {
  ThreadPane pane;
  pane.resize(320, 520);
  ui::ThreadListSnapshot snapshot;
  snapshot.selectedThreadId = "child";
  snapshot.providerReady = true;
  snapshot.canControl = true;
  ui::ThreadListRow root = row("root", "Root", "running");
  root.children.push_back(row("child", "Child", "completed"));
  snapshot.roots.push_back(std::move(root));

  std::string selected;
  ThreadPane::Actions actions;
  actions.select = [&](const std::string &id) { selected = id; };
  pane.setActions(std::move(actions));
  pane.refresh(snapshot);
  pane.show();
  QCoreApplication::processEvents();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  bool result = expect(list && list->count() == 2,
                       "a selected child retains its root hierarchy");
  if (list) {
    selected.clear();
    list->setCurrentRow(0);
    list->setCurrentRow(1);
    QCoreApplication::processEvents();
    result &= expect(selected == "child",
                     "the established action API emits the canonical ID");
  }

  pane.beginOptimisticThread("draft:new-thread", "New thread", "/workspace");
  pane.refresh(snapshot);
  QCoreApplication::processEvents();
  result &= expect(list && list->count() == 3,
                   "the optimistic row appears through normal refresh");
  pane.confirmOptimisticThread("draft:new-thread");
  pane.refresh(snapshot);
  QCoreApplication::processEvents();
  result &= expect(list && list->count() == 2,
                   "confirming the draft removes only its optimistic row");
  return result;
}

VisibleCardData card(CardKind kind, std::string item,
                     CardPayload payload) {
  VisibleCardData result;
  result.key = AuthoritativeItemKey{"thread", "turn", item};
  result.kind = kind;
  result.threadId = "thread";
  result.turnId = "turn";
  result.itemId = std::move(item);
  result.payload = std::move(payload);
  return result;
}

bool conversationOwnershipAndAtomicReconcileContract() {
  ConversationView view;
  view.resize(820, 620);
  view.show();
  ConversationSnapshot snapshot;
  snapshot.threadId = "thread";
  TurnSection section;
  section.key = "turn";
  section.turnId = "turn";
  section.cards.push_back(
      card(CardKind::UserMessage, "user", UserMessageData{"Question", {}}));
  section.rootCardKey = section.cards.front().key;
  section.cards.push_back(card(CardKind::AgentMessage, "agent",
                              AgentMessageData{"Answer", true}));
  snapshot.sections.push_back(std::move(section));
  const bool changed = view.reconcile(snapshot);
  QCoreApplication::processEvents();

  ConversationCard *owner = nullptr;
  ConversationCard *answer = nullptr;
  for (ConversationCard *candidate : view.findChildren<ConversationCard *>()) {
    if (candidate->property("turnContainer").toBool())
      owner = candidate;
    if (const auto *agent =
            std::get_if<AgentMessageData>(&candidate->data().payload);
        agent && agent->text == "Answer")
      answer = candidate;
  }
  bool nested = false;
  for (QWidget *parent = answer ? answer->parentWidget() : nullptr; parent;
       parent = parent->parentWidget())
    if (parent == owner) {
      nested = true;
      break;
    }
  bool result = expect(changed && owner && answer && nested,
                       "one reconcile exposes a complete parented turn");
  result &= expect(!view.reconcile(snapshot),
                   "repeating identical visible state is a no-op");
  return result;
}

bool completeMiddleSurfaceRetainsPaneAndHeadingBehavior() {
  MiddleRegionWidget region;
  region.resize(1500, 850);
  region.show();
  region.setThreadHeading(QStringLiteral("Thread title"),
                          QStringLiteral("/workspace"),
                          QStringLiteral("Last activity: 12:00"),
                          QStringLiteral("running"),
                          QStringLiteral("active"));
  QCoreApplication::processEvents();
  bool result = expect(region.sidebarVisible() && region.inspectorVisible(),
                       "the complete three-pane workspace starts visible");
  result &= expect(region.findChild<QLabel *>(
                           QStringLiteral("conversationTitle")) != nullptr,
                   "the established conversation heading remains present");
  region.showSidebar(false);
  region.showInspector(false);
  QCoreApplication::processEvents();
  result &= expect(!region.sidebarVisible() && !region.inspectorVisible(),
                   "pane visibility remains user-controlled");
  region.showSidebar(true);
  region.showInspector(true);
  QCoreApplication::processEvents();
  result &= expect(region.sidebarVisible() && region.inspectorVisible(),
                   "hidden panes restore without reconstructing the shell");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  const bool passed = promptKeyboardAndFocusContract() &&
                      threadPaneSnapshotAndActionContract() &&
                      conversationOwnershipAndAtomicReconcileContract() &&
                      completeMiddleSurfaceRetainsPaneAndHeadingBehavior();
  if (passed)
    std::cout << "Established UI/UX compatibility tests passed\n";
  return passed ? 0 : 1;
}
