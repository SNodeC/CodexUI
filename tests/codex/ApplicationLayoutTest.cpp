// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/GitDiffProvider.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/ThreadPane.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/QtNodeAttachment.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <git2.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

class LayoutRequestCounter final : public QObject {
public:
  int count = 0;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    static_cast<void>(watched);
    if (event->type() == QEvent::LayoutRequest)
      ++count;
    return false;
  }
};

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

std::string utf8(const QString &value) { return value.toUtf8().toStdString(); }

nodegraph::NodeStatus graphStatus(std::string_view status) {
  if (status == "active" || status == "inProgress" || status == "running" ||
      status == "started")
    return nodegraph::NodeStatus::Running;
  if (status == "idle" || status == "completed")
    return nodegraph::NodeStatus::Completed;
  if (status == "failed" || status == "systemError")
    return nodegraph::NodeStatus::Failed;
  if (status == "interrupted")
    return nodegraph::NodeStatus::Interrupted;
  if (status == "notLoaded")
    return nodegraph::NodeStatus::NotLoaded;
  return nodegraph::NodeStatus::Unknown;
}

void addTimestamp(nodegraph::Value::Object &fields, std::string name,
                  const std::optional<std::int64_t> &value) {
  if (value)
    fields.emplace(std::move(name), nodegraph::Value(*value));
}

nodegraph::NodeRef findNode(const nodegraph::NodeGraph &graph,
                            nodegraph::NodeKind kind,
                            std::string_view canonical) {
  auto read = graph.tryRead();
  return read ? read->find({kind, std::string(canonical)})
              : nodegraph::NodeRef{};
}

void refresh(ThreadPane &pane, const nodegraph::NodeGraph &graph,
             std::string_view selectedThreadId = {}) {
  pane.refresh(graph, selectedThreadId.empty()
                          ? nodegraph::NodeRef{}
                          : findNode(graph, nodegraph::NodeKind::Thread,
                                     selectedThreadId));
  for (int pass = 0; pass < 3; ++pass)
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

void refresh(InspectorPane &pane, const nodegraph::NodeGraph &graph,
             std::string_view selectedThreadId = {}) {
  pane.refresh(graph, selectedThreadId.empty()
                          ? nodegraph::NodeRef{}
                          : findNode(graph, nodegraph::NodeKind::Thread,
                                     selectedThreadId));
  for (int pass = 0; pass < 3; ++pass)
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

nodegraph::UiEffect protocolDiagnostic(std::uint64_t sequence,
                                       std::string direction,
                                       std::string subject,
                                       std::string authority = "merge") {
  return {nodegraph::UiEffectKind::ProtocolDiagnostic,
          std::nullopt,
          {},
          {{"sequence", nodegraph::Value(sequence)},
           {"connectionGeneration", nodegraph::Value(std::uint64_t{2})},
           {"providerGeneration", nodegraph::Value(std::uint64_t{5})},
           {"direction", nodegraph::Value(std::move(direction))},
           {"subject", nodegraph::Value(std::move(subject))},
           {"source", nodegraph::Value("app-server")},
           {"authority", nodegraph::Value(std::move(authority))}}};
}

nodegraph::NodeRef addThread(nodegraph::NodeGraph::WriteAccess &write,
                             std::string id, std::string name = {},
                             std::string status = {},
                             std::optional<std::int64_t> createdAt = {},
                             std::optional<std::int64_t> updatedAt = {},
                             std::optional<std::int64_t> recencyAt = {}) {
  nodegraph::NodeState state;
  state.status = graphStatus(status);
  state.fields = {{"name", nodegraph::Value(std::move(name))},
                  {"status", nodegraph::Value(std::move(status))}};
  addTimestamp(state.fields, "createdAt", createdAt);
  addTimestamp(state.fields, "updatedAt", updatedAt);
  addTimestamp(state.fields, "recencyAt", recencyAt);
  std::optional<std::int64_t> lastActivityAt = createdAt;
  for (const std::optional<std::int64_t> candidate : {updatedAt, recencyAt})
    if (candidate && (!lastActivityAt || *candidate > *lastActivityAt))
      lastActivityAt = candidate;
  addTimestamp(state.fields, "lastActivityAt", lastActivityAt);
  return write.upsert({nodegraph::NodeKind::Thread, std::move(id)},
                      std::move(state));
}

nodegraph::NodeRef runtimeWithRoots(nodegraph::NodeGraph::WriteAccess &write,
                                    std::span<const nodegraph::NodeRef> roots) {
  const nodegraph::NodeRef runtime =
      write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
  write.replaceRelated(runtime, nodegraph::RelationKind::RootThread, roots);
  nodegraph::NodeState connectionState;
  connectionState.status = nodegraph::NodeStatus::Connected;
  connectionState.fields = {{"providerState", nodegraph::Value("ready")},
                            {"transportState", nodegraph::Value("connected")},
                            {"role", nodegraph::Value("controller")}};
  static_cast<void>(
      write.upsert({nodegraph::NodeKind::Connection, "connection"},
                   std::move(connectionState)));
  return runtime;
}

std::string requestMethod(std::string_view kind) {
  if (kind == "command-approval")
    return "item/commandExecution/requestApproval";
  if (kind == "file-change-approval")
    return "item/fileChange/requestApproval";
  if (kind == "permissions-approval")
    return "permissions/requestApproval";
  if (kind == "mcp-elicitation")
    return "mcpServer/elicitation/request";
  if (kind == "legacy-patch-approval")
    return "applyPatchApproval";
  if (kind == "legacy-command-approval")
    return "execCommandApproval";
  return "item/tool/requestUserInput";
}

nodegraph::NodeRef addInteraction(nodegraph::NodeGraph::WriteAccess &write,
                                  const nodegraph::NodeRef &runtime,
                                  const nodegraph::NodeRef &target,
                                  std::string id, std::string_view kind,
                                  nodegraph::Value::Object payload = {}) {
  nodegraph::NodeState state;
  state.status = nodegraph::NodeStatus::Pending;
  state.fields = {{"method", nodegraph::Value(requestMethod(kind))},
                  {"payload", nodegraph::Value(std::move(payload))}};
  const nodegraph::NodeRef interaction = write.upsert(
      {nodegraph::NodeKind::Interaction, std::move(id)}, std::move(state));
  write.relate(interaction, nodegraph::RelationKind::InteractionTarget, target);
  write.relate(target, nodegraph::RelationKind::PendingInteraction,
               interaction);
  write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
               interaction);
  return interaction;
}
void sendPromptKey(codexui::ExpandingPromptEditor &editor, int key,
                   Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                   bool autoRepeat = false) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), autoRepeat, 1);
  QCoreApplication::sendEvent(&editor, &event);
}

bool testPromptKeyboardSubmission() {
  codexui::ExpandingPromptEditor editor;
  editor.resize(480, 80);
  editor.show();
  editor.setFocus();
  QCoreApplication::processEvents();

  int submissions = 0;
  QObject::connect(&editor, &codexui::ExpandingPromptEditor::submitRequested,
                   [&submissions] { ++submissions; });
  const auto resetDraft = [&editor] {
    editor.setPlainText(QStringLiteral("draft"));
    editor.moveCursor(QTextCursor::End);
  };

  bool result =
      expect(editor.accessibleName() == QStringLiteral("Message Codex") &&
                 editor.accessibleDescription().contains(
                     QStringLiteral("Shift+Enter")),
             "the prompt editor exposes its name and keyboard hint");

  resetDraft();
  sendPromptKey(editor, Qt::Key_Return);
  result &=
      expect(submissions == 1, "Return submits the focused prompt editor");
  resetDraft();
  sendPromptKey(editor, Qt::Key_Enter, Qt::KeypadModifier);
  result &= expect(submissions == 2,
                   "keypad Enter submits the focused prompt editor");
  resetDraft();
  sendPromptKey(editor, Qt::Key_Return, Qt::ControlModifier);
  result &= expect(submissions == 3,
                   "Control+Enter remains a prompt submission alias");
  resetDraft();
  sendPromptKey(editor, Qt::Key_Return, Qt::MetaModifier);
  result &= expect(submissions == 4, "Meta+Enter is a prompt submission alias");

  resetDraft();
  sendPromptKey(editor, Qt::Key_Return, Qt::ShiftModifier);
  result &= expect(submissions == 4 &&
                       editor.toPlainText() == QStringLiteral("draft\n"),
                   "Shift+Enter inserts a newline without submitting");
  resetDraft();
  sendPromptKey(editor, Qt::Key_Return,
                Qt::ControlModifier | Qt::ShiftModifier);
  result &= expect(
      submissions == 4 && editor.toPlainText() == QStringLiteral("draft\n"),
      "Shift takes precedence over the Control+Enter submission alias");

  resetDraft();
  sendPromptKey(editor, Qt::Key_Return, Qt::AltModifier);
  result &= expect(submissions == 4, "Alt+Enter does not submit a prompt");
  resetDraft();
  sendPromptKey(editor, Qt::Key_Return, Qt::ControlModifier, true);
  result &= expect(submissions == 4 &&
                       editor.toPlainText() == QStringLiteral("draft"),
                   "an auto-repeated Enter chord neither submits nor inserts");

  resetDraft();
  QInputMethodEvent preedit(QStringLiteral("candidate"), {});
  QCoreApplication::sendEvent(&editor, &preedit);
  sendPromptKey(editor, Qt::Key_Return);
  result &= expect(submissions == 4,
                   "Enter does not submit while IME preedit is active");
  QInputMethodEvent commit;
  commit.setCommitString(QStringLiteral("candidate"));
  QCoreApplication::sendEvent(&editor, &commit);
  resetDraft();
  sendPromptKey(editor, Qt::Key_Return);
  result &= expect(submissions == 5,
                   "Enter submits again after IME composition completes");

  editor.setPlainText(QStringLiteral("one\ntwo\nthree\nfour"));
  QCoreApplication::processEvents();
  editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->maximum());
  result &= expect(
      editor.verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff &&
          editor.verticalScrollBar()->value() ==
              editor.verticalScrollBar()->minimum(),
      "a fully visible multiline draft has no hidden empty-line scroll tail");

  editor.clear();
  editor.resize(280, codexui::ExpandingPromptEditor::compactHeight());
  QCoreApplication::processEvents();
  const int compactWidth = 170;
  QString boundary;
  while (boundary.size() < 100 &&
         !editor.requiresExpandedLayout(compactWidth)) {
    boundary += QLatin1Char('W');
    editor.setPlainText(boundary);
  }
  const QString beforeBoundary = boundary.chopped(1);
  editor.setPlainText(beforeBoundary);
  const bool beforeExpands = editor.requiresExpandedLayout(compactWidth);
  editor.setPlainText(boundary);
  QCoreApplication::processEvents();
  const qreal liveWidth = editor.document()->textWidth();
  int liveLayoutChanges = 0;
  const QMetaObject::Connection layoutConnection =
      QObject::connect(editor.document()->documentLayout(),
                       &QAbstractTextDocumentLayout::documentSizeChanged,
                       &editor, [&liveLayoutChanges] { ++liveLayoutChanges; });
  const bool boundaryExpands = editor.requiresExpandedLayout(compactWidth);
  QObject::disconnect(layoutConnection);
  result &= expect(!beforeBoundary.isEmpty(),
                   "the compact probe discovers a nonempty wrap boundary");
  result &= expect(!beforeExpands,
                   "the character before the wrap boundary remains compact");
  result &= expect(boundaryExpands,
                   "the first wrapped character enters multiline mode");
  result &= expect(editor.document()->textWidth() == liveWidth,
                   "compact layout probing preserves the live document width");
  result &=
      expect(liveLayoutChanges == 0,
             "compact layout probing does not relay out the visible document");
  return result;
}

bool commitPath(git_repository *repository, const char *path) {
  git_index *index = nullptr;
  if (git_repository_index(&index, repository) < 0)
    return false;
  const bool indexed =
      git_index_add_bypath(index, path) == 0 && git_index_write(index) == 0;
  git_oid treeId{};
  const bool wroteTree = indexed && git_index_write_tree(&treeId, index) == 0;
  git_index_free(index);
  if (!wroteTree)
    return false;
  git_tree *tree = nullptr;
  git_signature *signature = nullptr;
  if (git_tree_lookup(&tree, repository, &treeId) < 0 ||
      git_signature_now(&signature, "CodexUI Test", "codexui@example.invalid") <
          0) {
    git_tree_free(tree);
    git_signature_free(signature);
    return false;
  }
  git_oid commitId{};
  git_reference *head = nullptr;
  git_commit *parent = nullptr;
  if (git_repository_head(&head, repository) == 0)
    git_commit_lookup(&parent, repository, git_reference_target(head));
  const git_commit *parents[] = {parent};
  const bool committed =
      git_commit_create(&commitId, repository, "HEAD", signature, signature,
                        nullptr, "path baseline", tree, parent ? 1 : 0,
                        parent ? parents : nullptr) == 0;
  git_commit_free(parent);
  git_reference_free(head);
  git_signature_free(signature);
  git_tree_free(tree);
  return committed;
}

bool hasLabelContaining(const QWidget &root, const QString &text) {
  for (const QLabel *label : root.findChildren<QLabel *>()) {
    if (label->text().contains(text))
      return true;
  }
  return false;
}

bool hasButtonText(const QWidget &root, const QString &text) {
  for (const QPushButton *button : root.findChildren<QPushButton *>()) {
    if (button->text() == text)
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

void populateLongConversation(nodegraph::NodeGraph &graph,
                              nodegraph::NodeRef &thread,
                              std::string threadId) {
  auto write = graph.write();
  thread =
      addThread(write, std::move(threadId), "Long conversation", "completed");
  const nodegraph::NodeRef firstTurn =
      write.upsert({nodegraph::NodeKind::Turn, "turn-1"});
  const nodegraph::NodeRef secondTurn =
      write.upsert({nodegraph::NodeKind::Turn, "turn-2"});
  write.setParent(thread, firstTurn);
  write.setParent(thread, secondTurn);
  for (int index = 0; index < 30; ++index) {
    nodegraph::NodeState state;
    state.status = nodegraph::NodeStatus::Completed;
    state.fields = {
        {"type", nodegraph::Value("agentMessage")},
        {"phase", nodegraph::Value("final_answer")},
        {"text",
         nodegraph::Value(
             utf8(QStringLiteral("A materialized response line %1 with enough "
                                 "content to occupy normal card height.")
                      .arg(index)))}};
    const nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "item-" + std::to_string(index)},
        std::move(state));
    write.setParent(index < 15 ? firstTurn : secondTurn, item);
  }
  static_cast<void>(write.finish());
}
QWheelEvent wheelFor(QWidget *target, int pixelDelta,
                     Qt::ScrollPhase phase = Qt::ScrollUpdate) {
  const QPointF local(target->rect().center());
  return QWheelEvent(local, target->mapToGlobal(local.toPoint()), QPoint(),
                     QPoint(0, pixelDelta), Qt::NoButton, Qt::NoModifier, phase,
                     false);
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

bool testOverlayGeometryAndRegionRouting() {
  MiddleRegionWidget region;
  bool result =
      expect(region.composer().extraOverlayHeight() == 0 &&
                 region.conversation().trailingSpaceHeight() == 0,
             "composer construction reports no pre-canonical trailing space");
  region.resize(1500, 820);
  region.show();
  region.setThreadHeading(
      QStringLiteral("Thread title"), QStringLiteral("/workspace"),
      QStringLiteral("Last activity: 14:15:51"), QStringLiteral("completed"),
      QStringLiteral("success"));
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

  auto *threadHeaderDivider = splitter->widget(0)->findChild<QFrame *>(
      QStringLiteral("threadHeaderDivider"));
  auto *conversationHeaderDivider = splitter->widget(1)->findChild<QFrame *>(
      QStringLiteral("conversationHeaderDivider"));
  auto *conversationTitle = splitter->widget(1)->findChild<QLabel *>(
      QStringLiteral("conversationTitle"));
  auto *conversationMetadata = splitter->widget(1)->findChild<QLabel *>(
      QStringLiteral("conversationMetadata"));
  auto *conversationTrailingMetadata = splitter->widget(1)->findChild<QLabel *>(
      QStringLiteral("conversationTrailingMetadata"));
  auto *conversationState = splitter->widget(1)->findChild<QLabel *>(
      QStringLiteral("conversationState"));
  auto *reasoningToggle = splitter->widget(1)->findChild<QToolButton *>(
      QStringLiteral("conversationReasoningToggle"));
  auto *updatesToggle = splitter->widget(1)->findChild<QToolButton *>(
      QStringLiteral("conversationUpdatesToggle"));
  auto *commandFoldingToggle = splitter->widget(1)->findChild<QToolButton *>(
      QStringLiteral("conversationCommandFoldingToggle"));
  auto *imageFoldingToggle = splitter->widget(1)->findChild<QToolButton *>(
      QStringLiteral("conversationImageFoldingToggle"));
  const auto paneRect = [](QWidget *widget, QWidget *pane) {
    return QRect(widget->mapTo(pane, QPoint()), widget->size());
  };
  const QRect threadDividerRect =
      threadHeaderDivider ? paneRect(threadHeaderDivider, splitter->widget(0))
                          : QRect{};
  const QRect conversationDividerRect =
      conversationHeaderDivider
          ? paneRect(conversationHeaderDivider, splitter->widget(1))
          : QRect{};
  result &= expect(
      threadHeaderDivider && conversationHeaderDivider &&
          threadDividerRect.left() == 10 &&
          threadDividerRect.right() == splitter->widget(0)->width() - 11 &&
          conversationDividerRect.left() == 10 &&
          conversationDividerRect.right() == splitter->widget(1)->width() - 11,
      "Threads and Conversation header dividers share the 10 px inset");
  result &= expect(
      conversationTitle && conversationMetadata &&
          conversationTrailingMetadata && conversationState &&
          conversationMetadata->geometry().left() >
              conversationTitle->geometry().right() &&
          conversationTrailingMetadata->geometry().right() <
              conversationState->geometry().left() &&
          conversationState->geometry().right() >=
              conversationState->parentWidget()->width() - 16 &&
          conversationTrailingMetadata->text() ==
              QStringLiteral("Last activity: 14:15:51") &&
          conversationTrailingMetadata->property("tone").toString() ==
              QStringLiteral("strong") &&
          conversationState->text() == QStringLiteral("completed") &&
          conversationState->property("tone").toString() ==
              QStringLiteral("success") &&
          conversationState->width() >=
              conversationState->fontMetrics().horizontalAdvance(
                  conversationState->text()) &&
          conversationTrailingMetadata->width() >=
              conversationTrailingMetadata->fontMetrics().horizontalAdvance(
                  conversationTrailingMetadata->text()) &&
          std::abs((conversationMetadata->geometry().top() +
                    conversationMetadata->contentsMargins().top() +
                    conversationMetadata->fontMetrics().ascent()) -
                   (conversationTitle->geometry().top() +
                    conversationTitle->contentsMargins().top() +
                    conversationTitle->fontMetrics().ascent())) <= 1,
      "thread title metadata align by baseline and activity aligns right");
  result &= expect(
      reasoningToggle && updatesToggle && commandFoldingToggle &&
          imageFoldingToggle && !reasoningToggle->isChecked() &&
          updatesToggle->isChecked() && commandFoldingToggle->isChecked() &&
          imageFoldingToggle->isChecked() &&
          reasoningToggle->text().isEmpty() &&
          updatesToggle->text().isEmpty() &&
          commandFoldingToggle->text().isEmpty() &&
          imageFoldingToggle->text().isEmpty() &&
          reasoningToggle->accessibleName() ==
              QStringLiteral("Show reasoning cards") &&
          commandFoldingToggle->accessibleName() ==
              QStringLiteral("New command cards start expanded") &&
          imageFoldingToggle->accessibleName() ==
              QStringLiteral("New image cards start expanded"),
      "Conversation header exposes the three canonical default presentation "
      "controls");
  if (reasoningToggle && commandFoldingToggle) {
    reasoningToggle->click();
    commandFoldingToggle->click();
    const auto options = region.conversation().presentationOptions();
    const QSettings persisted;
    result &= expect(
        options.showReasoning && options.showCodexUpdates &&
            !options.commandsInitiallyExpanded &&
            reasoningToggle->accessibleName() ==
                QStringLiteral("Hide reasoning cards") &&
            commandFoldingToggle->accessibleName() ==
                QStringLiteral("New command cards start collapsed") &&
            persisted.value(QStringLiteral("conversation/showReasoning"), false)
                .toBool() &&
            !persisted
                 .value(
                     QStringLiteral("conversation/commandsInitiallyExpanded"),
                     true)
                 .toBool(),
        "Conversation presentation controls update the view and persistent "
        "settings together");
    reasoningToggle->click();
    commandFoldingToggle->click();
  }

  ConversationView &view = region.conversation();
  nodegraph::NodeGraph conversationGraph;
  nodegraph::NodeRef conversationThread;
  populateLongConversation(conversationGraph, conversationThread,
                           "layout-thread");
  view.bindGraph(conversationGraph, conversationThread);
  spin(20);
  const QRect viewGeometry = view.geometry();
  const QRect viewportGeometry = view.viewport()->geometry();
  auto *notice =
      region.findChild<QFrame *>(QStringLiteral("conversationNoticeBar"));
  auto *dismissNotice = notice ? notice->findChild<QPushButton *>() : nullptr;
  region.showNotice(QStringLiteral("Transient interaction notice"), false);
  spin(10);
  const auto regionRect = [&region](QWidget *widget) {
    return QRect(widget->mapTo(&region, QPoint()), widget->size());
  };
  result &=
      expect(notice && notice->isVisible() && dismissNotice &&
                 view.geometry() == viewGeometry &&
                 view.viewport()->geometry() == viewportGeometry &&
                 regionRect(notice).intersects(regionRect(&view)),
             "transient interaction notice overlays without shifting messages");
  if (dismissNotice)
    dismissNotice->click();
  spin(10);
  result &=
      expect(notice && notice->isHidden() && view.geometry() == viewGeometry &&
                 view.viewport()->geometry() == viewportGeometry,
             "dismissing the notice preserves message geometry");
  const int canonical = region.composer().canonicalReserveHeight();
  result &=
      expect(canonical > 0 &&
                 region.composer().canonicalReserve()->height() == canonical,
             "composer establishes one compact canonical reserve");

  QFrame *boundary = nullptr;
  QFrame *composerSurface = nullptr;
  for (QFrame *frame : region.composer().findChildren<QFrame *>()) {
    const QString kind = frame->property("kind").toString();
    if (kind == QStringLiteral("standardDivider"))
      boundary = frame;
    else if (kind == QStringLiteral("composer"))
      composerSurface = frame;
  }
  TurnSettingsWidget *settings = region.composer().turnSettings();
  auto *sendButton = region.composer().findChild<QPushButton *>(
      QStringLiteral("composerSendButton"));
  const auto overlayRect = [&](QWidget *widget) {
    return QRect(widget->mapTo(&region.composer(), QPoint()), widget->size());
  };
  const auto settingsToComposerGap = [&] {
    return composerSurface ? overlayRect(composerSurface).top() -
                                 overlayRect(settings).bottom() - 1
                           : -1;
  };
  const auto settingsToEditorGap = [&] {
    return region.composer()
               .promptEditor()
               ->mapTo(&region.composer(), QPoint())
               .y() -
           overlayRect(settings).bottom() - 1;
  };
  const auto stableComposerGeometry = [&] {
    if (!boundary || !composerSurface)
      return false;
    const QRect boundaryRect = overlayRect(boundary);
    const QRect settingsRect = overlayRect(settings);
    const QRect composerRect = overlayRect(composerSurface);
    return boundaryRect.top() == 8 && boundaryRect.height() == 1 &&
           settingsRect.top() - boundaryRect.bottom() - 1 == 8 &&
           settings->height() == settings->sizeHint().height() &&
           settingsToComposerGap() == 8 && boundaryRect.left() == 0 &&
           boundaryRect.right() == region.composer().width() - 1 &&
           settingsRect.left() == 10 && composerRect.left() == 10 &&
           settingsRect.right() == region.composer().width() - 11 &&
           composerRect.right() == region.composer().width() - 11 &&
           boundaryRect.width() == composerRect.width() + 20;
  };
  const auto finalCardBottom = [&] {
    int bottom = -1;
    for (QFrame *frame : view.findChildren<QFrame *>()) {
      if (!frame->property("conversationAnchorKey").toString().isEmpty() &&
          frame->isVisible())
        bottom = std::max(
            bottom,
            frame->mapTo(view.viewport(), QPoint(0, frame->height())).y());
    }
    return bottom;
  };
  result &= expect(
      boundary && composerSurface &&
          region.composer().testAttribute(Qt::WA_StyledBackground) &&
          UiStyle::applicationStyleSheet().contains(
              QStringLiteral("QWidget#composerOverlay")) &&
          UiStyle::applicationStyleSheet().contains(
              QStringLiteral("QLabel[tone=\"success\"]")) &&
          stableComposerGeometry(),
      "compact composer has an opaque surface and canonical section gaps");
  region.composer().setCanSubmit(true);
  result &= expect(sendButton && !sendButton->isEnabled(),
                   "an empty prompt cannot activate Send");
  region.composer().promptEditor()->setPlainText(QStringLiteral("draft"));
  spin(10);
  result &= expect(sendButton && sendButton->isEnabled(),
                   "non-blank input activates Send when admission is ready");
  region.composer().promptEditor()->setFocus();
  spin(10);
  result &= expect(composerSurface->property("focused").toBool(),
                   "prompt focus activates the canonical composer focus state");
  QString submittedPrompt;
  ComposerPane::Actions exactSubmission;
  exactSubmission.submit = [&submittedPrompt](QString prompt,
                                              std::vector<AttachmentDraft>) {
    submittedPrompt = std::move(prompt);
    return false;
  };
  region.composer().setActions(std::move(exactSubmission));
  const QString exactPrompt = QStringLiteral("  indented Markdown\n\n");
  region.composer().promptEditor()->setPlainText(exactPrompt);
  QMetaObject::invokeMethod(region.composer().promptEditor(), "submitRequested",
                            Qt::DirectConnection);
  result &= expect(submittedPrompt == exactPrompt,
                   "submission validates whitespace without rewriting it");
  region.composer().clearDraft();
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  result &= expect(finalCardBottom() == view.viewport()->height(),
                   "compact bottom has no scroll-owned trailing gap");
  const QRect compactOverlayGeometry = region.composer().geometry();
  const QRect compactBoundaryGeometry = boundary->geometry();
  view.verticalScrollBar()->setValue(
      std::max(0, view.verticalScrollBar()->maximum() - 80));
  spin(10);
  result &= expect(region.composer().geometry() == compactOverlayGeometry &&
                       boundary->geometry() == compactBoundaryGeometry,
                   "history scrolling leaves the composer boundary fixed");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  const int compactEditorGap = settingsToEditorGap();
  region.composer().setActiveTurn(true);
  spin(20);
  result &= expect(stableComposerGeometry() &&
                       settingsToEditorGap() == compactEditorGap,
                   "active-turn controls retain the compact composer gaps");

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
  result &= expect(
      view.verticalScrollBar()->property("composerBottomInset").toInt() ==
              extra &&
          view.verticalScrollBar()->styleSheet().contains(
              QStringLiteral("margin:2px 2px %1px 2px").arg(extra + 2)),
      "prompt growth shortens the visible message scrollbar track");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  result &= expect(
      stableComposerGeometry() && settingsToEditorGap() == compactEditorGap &&
          view.viewport()->height() - finalCardBottom() == extra &&
          view.geometry() == viewGeometry &&
          view.viewport()->geometry() == viewportGeometry &&
          region.composer().canonicalReserve()->height() == canonical,
      "prompt growth keeps gaps fixed without shifting the message viewport");
  region.composer().setAttachments(
      {{"/tmp/layout-diagnostic.png", "layout-diagnostic.png", "image/png"}});
  spin(30);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  result &= expect(stableComposerGeometry() &&
                       region.composer().extraOverlayHeight() > extra &&
                       view.viewport()->height() - finalCardBottom() ==
                           region.composer().extraOverlayHeight() &&
                       view.trailingSpaceHeight() ==
                           region.composer().extraOverlayHeight(),
                   "attachments retain the canonical settings-to-composer gap");
  region.composer().clearDraft();
  spin(30);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  result &= expect(
      region.composer().extraOverlayHeight() == 0 &&
          view.trailingSpaceHeight() == 0 && view.geometry() == viewGeometry &&
          view.verticalScrollBar()->property("composerBottomInset").toInt() ==
              0 &&
          view.verticalScrollBar()->styleSheet().isEmpty() &&
          view.viewport()->geometry() == viewportGeometry &&
          finalCardBottom() == view.viewport()->height() &&
          stableComposerGeometry() && settingsToEditorGap() == compactEditorGap,
      "prompt contraction restores canonical layout, gaps, and trailing space");
  region.composer().setActiveTurn(false);
  spin(20);
  result &= expect(view.isAtBottom(), "conversation begins at the bottom");
  region.composer().setAttentionRequest(
      QStringLiteral("Command approval requested"),
      QStringLiteral("Command: gh auth status  |  Reason: Verify GitHub "
                     "authentication"),
      true, QStringLiteral("Accept"));
  region.composer().setAttentionVisible(true);
  spin(20);
  result &= expect(
      hasLabelContaining(region.composer(),
                         QStringLiteral("Command approval requested")) &&
          hasLabelContaining(region.composer(),
                             QStringLiteral("Command: gh auth status")) &&
          hasButtonText(region.composer(), QStringLiteral("Reject")) &&
          hasButtonText(region.composer(), QStringLiteral("Accept")),
      "composer attention requests show details and direct semantic actions");
  region.composer().setAttentionVisible(false);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);

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

  QString oversizedPrompt;
  for (int line = 0; line < 30; ++line)
    oversizedPrompt +=
        QStringLiteral("scroll-owned prompt line %1\n").arg(line);
  region.composer().promptEditor()->setPlainText(oversizedPrompt);
  spin(20);
  auto *promptScroll = region.composer().promptEditor()->verticalScrollBar();
  promptScroll->setValue(promptScroll->minimum());
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  const int conversationBeforePromptWheel = view.verticalScrollBar()->value();
  QWheelEvent promptRoute =
      wheelFor(region.composer().promptEditor(), 120, Qt::ScrollBegin);
  result &= expect(
      !region.routeScrollEvent(region.composer().promptEditor(), &promptRoute),
      "the prompt editor retains its own wheel origin");
  QWheelEvent promptNative =
      wheelFor(region.composer().promptEditor(), 120, Qt::ScrollUpdate);
  QCoreApplication::sendEvent(region.composer().promptEditor(), &promptNative);
  result &=
      expect(view.verticalScrollBar()->value() == conversationBeforePromptWheel,
             "prompt overscroll cannot move the conversation");
  QWheelEvent settingsWheel = wheelFor(settings, 120, Qt::ScrollBegin);
  result &= expect(region.routeScrollEvent(settings, &settingsWheel) &&
                       view.verticalScrollBar()->value() ==
                           conversationBeforePromptWheel,
                   "settings-originated scrolling is consumed locally");
  region.composer().clearDraft();
  spin(20);

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

bool testStableComposerLayoutRequests() {
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  bool result = true;
  {
    MiddleRegionWidget region;
    region.resize(1500, 820);
    region.show();
    spin(20);

    LayoutRequestCounter composerLayoutRequests;
    region.composer().installEventFilter(&composerLayoutRequests);
    spin(80);
    result =
        expect(composerLayoutRequests.count <= 1,
               "stable composer geometry does not perpetually request layout");
  }
  qApp->setStyleSheet(QString{});
  return result;
}

bool testThreadSelectionProjection() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef threadA;
  nodegraph::NodeRef threadB;
  {
    auto write = graph.write();
    threadA = addThread(write, "thread-a", "A", "idle");
    threadB = addThread(write, "thread-b", "B", "active");
    const std::vector roots{threadA, threadB};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  ThreadPane pane;
  pane.resize(340, 620);
  pane.show();
  refresh(pane, graph, "thread-a");
  bool result = expect(pane.visiblySelectedThreadId() == "thread-a",
                       "thread selection is projected from Shell state");
  refresh(pane, graph, "draft:new-thread");
  result &= expect(pane.visiblySelectedThreadId().empty(),
                   "a New Thread draft cannot retain an old visible row");

  {
    auto write = graph.write();
    write.replaceRelated(threadA,
                         nodegraph::RelationKind::StructuralChildThread,
                         std::span(&threadB, 1));
    const std::array roots{threadA};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }
  refresh(pane, graph, "thread-b");
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  QListWidgetItem *selected = list ? list->currentItem() : nullptr;
  result &=
      expect(selected &&
                 selected->data(Qt::UserRole).toString() ==
                     QStringLiteral("thread-b") &&
                 pane.visiblySelectedThreadId() == "thread-b",
             "navigating to a nested thread reveals and selects it beneath its "
             "parent");
  QWidget *row = selected && list ? list->itemWidget(selected) : nullptr;
  auto *title =
      row ? row->findChild<QLabel *>(QStringLiteral("threadTitle")) : nullptr;
  auto *dot = row ? row->findChild<QFrame *>(QStringLiteral("threadStatusDot"))
                  : nullptr;
  auto *rowLayout = row ? qobject_cast<QHBoxLayout *>(row->layout()) : nullptr;
  auto *sortButton =
      pane.findChild<QToolButton *>(QStringLiteral("threadSortButton"));
  QListWidgetItem *parentItem = threadItem(list, "thread-a");
  QWidget *parentRow =
      list && parentItem ? list->itemWidget(parentItem) : nullptr;
  QWidget *disclosure = parentRow
                            ? parentRow->findChild<QWidget *>(
                                  QStringLiteral("threadExpansionIndicator"))
                            : nullptr;
  auto *parentDot =
      parentRow
          ? parentRow->findChild<QFrame *>(QStringLiteral("threadStatusDot"))
          : nullptr;
  const QString selectedAccessible =
      selected ? selected->data(Qt::AccessibleTextRole).toString() : QString{};
  const QString parentAccessible =
      parentItem ? parentItem->data(Qt::AccessibleTextRole).toString()
                 : QString{};
  result &= expect(
      selected && selected->sizeHint().height() == 40 && rowLayout &&
          rowLayout->contentsMargins() == QMargins(0, 2, 0, 2) &&
          rowLayout->spacing() == 0 && title && dot &&
          dot->size() == QSize(10, 10) && rowLayout->indexOf(dot) >= 0 &&
          disclosure && disclosure->size() == QSize(16, 24) &&
          disclosure->geometry().left() == 0 && parentDot &&
          parentDot->geometry().left() - disclosure->geometry().right() - 1 ==
              2 &&
          disclosure->property("chevronDirection").toString() ==
              QStringLiteral("down") &&
          sortButton &&
          dynamic_cast<UiStyle::ChevronToolButton *>(sortButton) &&
          sortButton->property("codexChevron").toBool() &&
          title->property("kind").toString() == QStringLiteral("title") &&
          selected->data(Qt::DisplayRole).toString().isEmpty() &&
          selectedAccessible.contains(QStringLiteral("B, running")) &&
          selectedAccessible.contains(QStringLiteral("level 2")) &&
          parentAccessible.contains(QStringLiteral("A")) &&
          parentAccessible.contains(QStringLiteral("expanded")) &&
          !title->wordWrap() &&
          title->textInteractionFlags().testFlag(Qt::TextSelectableByMouse) &&
          selected->toolTip().contains(QStringLiteral("Workspace:")) &&
          selected->toolTip().contains(QStringLiteral("Status: running")) &&
          selected->toolTip().contains(QStringLiteral("Last activity:")) &&
          selected->toolTip().contains(QStringLiteral("Parent: A")),
      "compact thread cards retain their status dot and expose canonical "
      "details through hover and accessibility");
  refresh(pane, graph, "thread-a");
  bool childPresent = false;
  if (list) {
    for (int index = 0; index < list->count(); ++index) {
      childPresent |= list->item(index)->data(Qt::UserRole).toString() ==
                      QStringLiteral("thread-b");
    }
  }
  result &=
      expect(childPresent && pane.visiblySelectedThreadId() == "thread-a",
             "a child thread remains nested while its parent is selected");
  nodegraph::GraphChange removed;
  {
    auto write = graph.write();
    write.remove(threadB);
    removed = write.finish();
  }
  pane.graphChanged(
      {removed.revision, removed.affected, removed.removed, false});
  refresh(pane, graph, "thread-a");
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

bool testThreadRuntimeStatusColors() {
  nodegraph::NodeGraph graph;
  const std::vector<std::pair<std::string, std::string>> statuses{
      {"thread-not-loaded", "notLoaded"},
      {"thread-completed", "idle"},
      {"thread-running", "active"},
      {"thread-failed", "systemError"},
  };
  {
    auto write = graph.write();
    std::vector<nodegraph::NodeRef> roots;
    for (const auto &[id, status] : statuses)
      roots.push_back(addThread(write, id, id, status));
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  ThreadPane pane;
  pane.resize(340, 620);
  pane.show();
  refresh(pane, graph, "thread-completed");
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  const std::vector<std::pair<std::string, const char *>> expected{
      {"thread-not-loaded", UiStyle::threadInactive},
      {"thread-completed", UiStyle::green},
      {"thread-running", UiStyle::blue},
      {"thread-failed", UiStyle::red},
  };
  bool result = true;
  for (const auto &[id, color] : expected) {
    QListWidgetItem *item = threadItem(list, id);
    QWidget *row = item && list ? list->itemWidget(item) : nullptr;
    auto *dot =
        row ? row->findChild<QFrame *>(QStringLiteral("threadStatusDot"))
            : nullptr;
    const std::string message =
        id + " uses its canonical app-server runtime-state color";
    result &=
        expect(dot && dot->styleSheet().contains(QString::fromLatin1(color)),
               message.c_str());
  }
  return result;
}

bool testIncrementalThreadSettings() {
  TurnSettingsWidget settings;
  const nlohmann::json models =
      nlohmann::json::array({{{"model", "gpt-a"}, {"displayName", "A"}},
                             {{"model", "gpt-b"}, {"displayName", "B"}}});
  settings.setContext("thread-a",
                      {{"model", "gpt-a"}, {"approvalPolicy", "never"}}, models,
                      nlohmann::json::array());
  auto *model = settings.findChild<QComboBox *>(QStringLiteral("codexModel"));
  auto *approval =
      settings.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  auto *personality =
      settings.findChild<QComboBox *>(QStringLiteral("codexPersonality"));
  auto *access =
      settings.findChild<QComboBox *>(QStringLiteral("codexSandbox"));
  auto *network =
      settings.findChild<QComboBox *>(QStringLiteral("codexNetwork"));
  auto *permissionProfile =
      settings.findChild<QComboBox *>(QStringLiteral("codexPermissionProfile"));
  if (!model || !approval || !personality || !access || !network ||
      !permissionProfile)
    return expect(false, "thread settings controls are discoverable");

  bool canonicalSettingsStyle = settings.styleSheet().isEmpty();
  for (const QLabel *label : settings.findChildren<QLabel *>()) {
    if (label->text() == QStringLiteral("Model"))
      canonicalSettingsStyle &= label->property("kind") == "settingLabel" &&
                                label->styleSheet().isEmpty();
  }

  model->setCurrentIndex(model->findData(QStringLiteral("gpt-b")));
  settings.setContext(
      "thread-a", {{"model", "gpt-a"}, {"approvalPolicy", "on-request"}},
      models, nlohmann::json::array(), 1, {{"approvalPolicy", "on-request"}});
  bool result = expect(canonicalSettingsStyle,
                       "thread settings use canonical application styling");
  result &= expect(UiStyle::humanizeLabel(QStringLiteral("xhigh")) ==
                       QStringLiteral("Extra high"),
                   "the fallback reasoning effort uses a human-readable label");
  result &= expect(
      model->currentData().toString() == QStringLiteral("gpt-b") &&
          approval->currentData().toString() == QStringLiteral("on-request"),
      "a partial authoritative update preserves unrelated pending settings");

  settings.setContext("thread-a",
                      {{"model", "gpt-b"}, {"approvalPolicy", "on-request"}},
                      models, nlohmann::json::array(), 2,
                      {{"model", "gpt-b"}, {"approvalPolicy", "on-request"}});
  result &= expect(!settings.turnStartOptions().contains("model") &&
                       !settings.turnStartOptions().contains("approvalPolicy"),
                   "authoritative settings clear their pending overrides");

  settings.setContext("thread-b",
                      {{"model", "gpt-a"},
                       {"reasoningEffort", "medium"},
                       {"personality", "friendly"},
                       {"sandboxPolicy",
                        {{"type", "workspaceWrite"}, {"networkAccess", false}}},
                       {"approvalPolicy", "never"},
                       {"approvalsReviewer", "user"},
                       {"cwd", "/workspace"},
                       {"activePermissionProfile", {{"id", "managed"}}},
                       {"serviceTier", "priority"},
                       {"summary", "concise"},
                       {"collaborationMode", {{"mode", "default"}}}},
                      models, nlohmann::json::array());
  result &= expect(model->currentData().toString() == QStringLiteral("gpt-a"),
                   "thread selection restores that thread's retained value");
  result &=
      expect(settings.turnStartOptions() ==
                     nlohmann::json{{"collaborationMode",
                                     {{"mode", "default"},
                                      {"settings",
                                       {{"model", "gpt-a"},
                                        {"developer_instructions", nullptr},
                                        {"reasoning_effort", "medium"}}}}}} &&
                 settings.threadStartOptions().empty(),
             "untouched settings emit only the displayed collaboration mode");
  result &= expect(access->isEnabled() && network->isEnabled(),
                   "a permission preset does not lock its effective access "
                   "controls");

  settings.setContext("full-access-thread",
                      {{"sandboxPolicy", {{"type", "dangerFullAccess"}}},
                       {"activePermissionProfile", {{"id", ":full-access"}}}},
                      nlohmann::json::array(),
                      {{"data", nlohmann::json::array({{{"id", ":full-access"},
                                                        {"allowed", true}}})}});
  result &=
      expect(access->isEnabled() && !network->isEnabled() &&
                 network->currentData().toString() == QStringLiteral("enabled"),
             "only logically redundant network selection is disabled");

  settings.setContext(
      "workspace-profile-thread",
      {{"sandboxPolicy",
        {{"type", "workspaceWrite"}, {"networkAccess", false}}},
       {"activePermissionProfile", {{"id", ":workspace"}}}},
      nlohmann::json::array(),
      {{"data", nlohmann::json::array(
                    {{{"id", ":workspace"}, {"allowed", true}},
                     {{"id", ":read-only"}, {"allowed", true}},
                     {{"id", ":danger-full-access"}, {"allowed", true}}})}});
  result &= expect(
      permissionProfile->itemText(permissionProfile->findData(
          QStringLiteral(":workspace"))) == QStringLiteral("Workspace") &&
          permissionProfile->itemText(permissionProfile->findData(
              QStringLiteral(":read-only"))) == QStringLiteral("Read only") &&
          permissionProfile->itemText(permissionProfile->findData(
              QStringLiteral(":danger-full-access"))) ==
              QStringLiteral("Full access"),
      "built-in permission profiles have user-facing labels");

  access->setCurrentIndex(
      access->findData(QStringLiteral("danger-full-access")));
  const nlohmann::json explicitAccessTurn = settings.turnStartOptions();
  const nlohmann::json explicitAccessThread = settings.threadStartOptions();
  result &= expect(
      permissionProfile->currentData().toString() ==
              QStringLiteral("default") &&
          !explicitAccessTurn.contains("permissions") &&
          explicitAccessTurn.value("sandboxPolicy", nlohmann::json(nullptr)) ==
              nlohmann::json({{"type", "dangerFullAccess"}}) &&
          !explicitAccessThread.contains("permissions") &&
          explicitAccessThread.value("sandbox", nlohmann::json(nullptr)) ==
              nlohmann::json("danger-full-access"),
      "an explicit access choice replaces the active permission profile");

  settings.setContext("individual-overrides-thread",
                      {{"model", "gpt-a"},
                       {"approvalPolicy", "never"},
                       {"personality", "friendly"},
                       {"sandboxPolicy",
                        {{"type", "workspaceWrite"}, {"networkAccess", false}}},
                       {"activePermissionProfile", {{"id", ":workspace"}}}},
                      models,
                      {{"data", nlohmann::json::array({{{"id", ":workspace"},
                                                        {"allowed", true}}})}});
  model->setCurrentIndex(model->findData(QStringLiteral("gpt-b")));
  approval->setCurrentIndex(approval->findData(QStringLiteral("on-request")));
  personality->setCurrentIndex(
      personality->findData(QStringLiteral("pragmatic")));
  const nlohmann::json individualOverrides = settings.turnStartOptions();
  result &= expect(
      permissionProfile->currentData().toString() ==
              QStringLiteral(":workspace") &&
          individualOverrides.value("model", "") == "gpt-b" &&
          individualOverrides.value("approvalPolicy", "") == "on-request" &&
          individualOverrides.value("personality", "") == "pragmatic" &&
          !individualOverrides.contains("sandboxPolicy") &&
          !individualOverrides.contains("permissions"),
      "supported individual settings override retained thread values without "
      "discarding its permission profile");

  return result;
}

bool testThreadHierarchyExpansionAndNavigation() {
  nodegraph::NodeGraph graph;
  {
    auto write = graph.write();
    const nodegraph::NodeRef rootZ = addThread(write, "root-z", "Z root");
    const nodegraph::NodeRef rootA = addThread(write, "root-a", "A root");
    const nodegraph::NodeRef childZ = addThread(write, "child-z", "Z child");
    const nodegraph::NodeRef childA = addThread(write, "child-a", "A child");
    const nodegraph::NodeRef grandchild =
        addThread(write, "grandchild", "Nested child");
    const std::array rootChildren{childZ, childA};
    write.replaceRelated(rootA, nodegraph::RelationKind::StructuralChildThread,
                         rootChildren);
    write.replaceRelated(childZ, nodegraph::RelationKind::StructuralChildThread,
                         std::span(&grandchild, 1));
    const std::array roots{rootZ, rootA};
    const nodegraph::NodeRef runtime = runtimeWithRoots(write, roots);
    addInteraction(write, runtime, grandchild, "nested-request", "user-input",
                   {{"message", nodegraph::Value("Review nested work")}});
    static_cast<void>(write.finish());
  }

  ThreadPane pane;
  pane.setSortCriterion(ThreadPane::SortCriterion::Alphanumeric);
  std::string selectedThread;
  int selections = 0;
  ThreadPane::NodeActions actions;
  actions.select = [&](const nodegraph::NodeRef &node) {
    selectedThread = node ? node->id().canonical : std::string{};
    ++selections;
    refresh(pane, graph, selectedThread);
  };
  pane.setNodeActions(std::move(actions));
  pane.resize(340, 620);
  pane.show();
  refresh(pane, graph, selectedThread);
  spin(20);

  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  QListWidgetItem *rootA = threadItem(list, "root-a");
  QWidget *rootRow = list && rootA ? list->itemWidget(rootA) : nullptr;
  QWidget *rootDisclosure =
      rootRow ? rootRow->findChild<QWidget *>(
                    QStringLiteral("threadExpansionIndicator"))
              : nullptr;
  bool result = expect(
      list &&
          threadOrder(pane) == std::vector<std::string>{"root-a", "root-z"} &&
          rootA && rootA->data(Qt::UserRole + 2).toInt() == 0 &&
          rootDisclosure && rootDisclosure->size() == QSize(16, 24) &&
          rootDisclosure->property("chevronDirection").toString() ==
              QStringLiteral("right") &&
          pane.visiblySelectedThreadId().empty(),
      "thread branches default to a canonical collapsed disclosure without "
      "selecting a hidden descendant");
  if (!list || !rootA)
    return false;

  const auto clickExpansion = [list](QListWidgetItem *item) {
    QWidget *row = item ? list->itemWidget(item) : nullptr;
    QWidget *indicator = row ? row->findChild<QWidget *>(
                                   QStringLiteral("threadExpansionIndicator"))
                             : nullptr;
    const QPoint position =
        indicator
            ? indicator->mapTo(list->viewport(), indicator->rect().center())
            : QPoint{};
    QMouseEvent press(QEvent::MouseButtonPress, position,
                      list->viewport()->mapToGlobal(position), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &press);
    spin();
  };

  clickExpansion(rootA);
  QListWidgetItem *childZ = threadItem(list, "child-z");
  QListWidgetItem *childA = threadItem(list, "child-a");
  rootA = threadItem(list, "root-a");
  rootRow = rootA ? list->itemWidget(rootA) : nullptr;
  rootDisclosure = rootRow ? rootRow->findChild<QWidget *>(
                                 QStringLiteral("threadExpansionIndicator"))
                           : nullptr;
  result &= expect(
      threadOrder(pane) == std::vector<std::string>{"root-a", "child-z",
                                                    "child-a", "root-z"} &&
          childZ && childZ->data(Qt::UserRole + 2).toInt() == 1 && childA &&
          rootDisclosure &&
          rootDisclosure->property("chevronDirection").toString() ==
              QStringLiteral("down") &&
          pane.visiblySelectedThreadId().empty() && selections == 0,
      "expanding a root reveals its ordered children and updates the "
      "canonical disclosure");
  if (!childZ)
    return false;

  selectedThread = "grandchild";
  refresh(pane, graph, selectedThread);
  spin();
  QListWidgetItem *grandchild = threadItem(list, "grandchild");
  QWidget *grandchildRow =
      list && grandchild ? list->itemWidget(grandchild) : nullptr;
  QLabel *grandchildTitle =
      grandchildRow
          ? grandchildRow->findChild<QLabel *>(QStringLiteral("threadTitle"))
          : nullptr;
  result &= expect(
      threadOrder(pane) == std::vector<std::string>{"root-a", "child-z",
                                                    "grandchild", "child-a",
                                                    "root-z"} &&
          grandchild && grandchild->data(Qt::UserRole + 2).toInt() == 2 &&
          grandchildTitle && grandchildTitle->text().startsWith("! ") &&
          pane.visiblySelectedThreadId() == "grandchild" && selections == 0,
      "nested navigation expands only its ancestor path and restores nesting, "
      "requests, and selection");
  if (!grandchild)
    return false;

  childZ = threadItem(list, "child-z");
  clickExpansion(childZ);
  result &= expect(
      threadOrder(pane) == std::vector<std::string>{"root-a", "child-z",
                                                    "child-a", "root-z"} &&
          pane.visiblySelectedThreadId().empty() && selections == 0,
      "collapsing a nested parent hides descendants without selecting it");
  childZ = threadItem(list, "child-z");
  clickExpansion(childZ);
  result &= expect(
      threadOrder(pane) == std::vector<std::string>{"root-a", "child-z",
                                                    "grandchild", "child-a",
                                                    "root-z"} &&
          pane.visiblySelectedThreadId() == "grandchild" && selections == 0,
      "expanding restores arbitrary nesting and projected child selection");

  rootA = threadItem(list, "root-a");
  clickExpansion(rootA);
  result &= expect(threadOrder(pane) ==
                           std::vector<std::string>{"root-a", "root-z"} &&
                       selections == 0,
                   "collapsing a root hides its complete descendant subtree");
  rootA = threadItem(list, "root-a");
  clickExpansion(rootA);

  childZ = threadItem(list, "child-z");
  list->setCurrentItem(childZ);
  spin();
  const int beforeKeyboard = selections;
  QKeyEvent rightToChild(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  QApplication::sendEvent(list, &rightToChild);
  spin();
  result &=
      expect(selectedThread == "grandchild" &&
                 pane.visiblySelectedThreadId() == "grandchild" &&
                 selections == beforeKeyboard + 1,
             "Right navigates from an expanded parent to its first child");
  QKeyEvent leftToParent(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
  QApplication::sendEvent(list, &leftToParent);
  spin();
  result &= expect(selectedThread == "child-z" &&
                       pane.visiblySelectedThreadId() == "child-z" &&
                       selections == beforeKeyboard + 2,
                   "Left navigates from a nested child to its parent");
  QKeyEvent leftCollapse(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
  QApplication::sendEvent(list, &leftCollapse);
  spin();
  result &= expect(
      threadOrder(pane) == std::vector<std::string>{"root-a", "child-z",
                                                    "child-a", "root-z"} &&
          pane.visiblySelectedThreadId() == "child-z" &&
          selections == beforeKeyboard + 2,
      "Left collapses an expanded parent without changing selection");
  QKeyEvent rightExpand(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  QApplication::sendEvent(list, &rightExpand);
  spin();
  result &=
      expect(threadOrder(pane) ==
                     std::vector<std::string>{"root-a", "child-z", "grandchild",
                                              "child-a", "root-z"} &&
                 pane.visiblySelectedThreadId() == "child-z" &&
                 selections == beforeKeyboard + 2,
             "Right expands a collapsed parent without changing selection");
  return result;
}

bool testThreadAlphanumericSort() {
  nodegraph::NodeGraph graph;
  {
    auto write = graph.write();
    const std::vector roots{addThread(write, "alpha", "Alpha"),
                            addThread(write, "ten", "10 Release"),
                            addThread(write, "two", "2 Review"),
                            addThread(write, "one", "1 Setup"),
                            addThread(write, "beta", "beta")};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }
  ThreadPane pane;
  pane.setSortCriterion(ThreadPane::SortCriterion::Alphanumeric);
  refresh(pane, graph, "two");
  const std::vector<std::string> order = threadOrder(pane);
  const bool correct = order == std::vector<std::string>(
                                    {"one", "two", "ten", "alpha", "beta"}) &&
                       pane.visiblySelectedThreadId() == "two";
  if (!correct) {
    std::cerr << "Observed alphanumeric order:";
    for (const std::string &id : order)
      std::cerr << ' ' << id;
    std::cerr << "; selected=" << pane.visiblySelectedThreadId() << '\n';
  }
  return expect(correct,
                "Alphanumeric sorting is natural and preserves selection");
}

bool testThreadCreatedSort() {
  nodegraph::NodeGraph graph;
  {
    auto write = graph.write();
    const std::vector roots{addThread(write, "old", {}, {}, 10),
                            addThread(write, "missing"),
                            addThread(write, "new", {}, {}, 30),
                            addThread(write, "middle", {}, {}, 20)};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }
  ThreadPane pane;
  pane.setSortCriterion(ThreadPane::SortCriterion::Created);
  refresh(pane, graph);
  return expect(threadOrder(pane) == std::vector<std::string>(
                                         {"new", "middle", "old", "missing"}),
                "Created sorting is newest first with missing values last");
}

bool testThreadLastChangedSort() {
  nodegraph::NodeGraph graph;
  {
    auto write = graph.write();
    const std::vector roots{addThread(write, "first", "Renamed", {}, {}, 20),
                            addThread(write, "second", {}, {}, {}, 10),
                            addThread(write, "third", {}, {}, {}, 30)};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }
  ThreadPane pane;
  pane.setSortCriterion(ThreadPane::SortCriterion::LastChanged);
  refresh(pane, graph);
  return expect(threadOrder(pane) ==
                    std::vector<std::string>({"third", "first", "second"}),
                "Last changed sorting uses retained updated timestamps");
}

bool testThreadRecencySort() {
  nodegraph::NodeGraph graph;
  {
    auto write = graph.write();
    const std::vector roots{addThread(write, "older", {}, {}, {}, {}, 10),
                            addThread(write, "recent", {}, {}, {}, {}, 30),
                            addThread(write, "middle", {}, {}, {}, {}, 20)};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }
  ThreadPane pane;
  refresh(pane, graph, "older");
  return expect(
      pane.currentSortCriterion() == ThreadPane::SortCriterion::Recency &&
          threadOrder(pane) ==
              std::vector<std::string>({"recent", "middle", "older"}) &&
          pane.visiblySelectedThreadId() == "older",
      "Recent is the default and preserves selection");
}

bool testThreadLastActivityRetention() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef tracked;
  nodegraph::NodeRef updatedOnly;
  {
    auto write = graph.write();
    tracked = addThread(write, "tracked", {}, {}, {}, 20, 30);
    updatedOnly = addThread(write, "updated-only", {}, {}, {}, 25);
    const std::array roots{tracked, updatedOnly};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }
  const auto integerField = [&graph](const nodegraph::NodeRef &node,
                                     std::string_view name) {
    std::optional<std::int64_t> result;
    auto read = graph.tryRead();
    if (!read)
      return result;
    const auto state = read->state(node);
    if (!state)
      return result;
    const auto found = state->fields.find(name);
    if (found != state->fields.end()) {
      if (const auto *value = found->second.asInt64())
        result = *value;
      else if (const auto *value = found->second.asUInt64())
        result = static_cast<std::int64_t>(*value);
    }
    return result;
  };

  bool result =
      expect(integerField(tracked, "lastActivityAt") == 30,
             "provider recency and update timestamps seed activity by maximum");
  result &= expect(integerField(updatedOnly, "lastActivityAt") == 25,
                   "provider update timestamp seeds activity without recency");

  {
    auto write = graph.write();
    write.setField(tracked, "localActivityAt", nodegraph::Value(40));
    write.setField(tracked, "recencyAt", nodegraph::Value(35));
    static_cast<void>(write.finish());
  }
  result &= expect(integerField(tracked, "localActivityAt") == 40 &&
                       integerField(tracked, "updatedAt") == 20 &&
                       integerField(tracked, "recencyAt") == 35,
                   "live activity remains independent from provider sort keys");
  return result;
}

bool testPromptActivityNaturallyOrdersThreads() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef older;
  nodegraph::NodeRef recent;
  {
    auto write = graph.write();
    older = addThread(write, "older", "Older", {}, 10, 10, 10);
    recent = addThread(write, "recent", "Recent", {}, 30, 30, 30);
    const std::array roots{older, recent};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  ThreadPane pane;
  refresh(pane, graph, "older");
  bool result =
      expect(threadOrder(pane) == std::vector<std::string>({"recent", "older"}),
             "provider recency initially determines thread order");

  {
    auto write = graph.write();
    write.setField(older, "localActivityAt", nodegraph::Value(40));
    write.setField(older, "localPromptActivityAt", nodegraph::Value(40));
    static_cast<void>(write.finish());
  }
  refresh(pane, graph, "older");
  result &=
      expect(threadOrder(pane) == std::vector<std::string>({"older", "recent"}),
             "prompt activity immediately updates natural Recent ordering");
  pane.setSortCriterion(ThreadPane::SortCriterion::LastChanged);
  spin();
  result &=
      expect(threadOrder(pane) == std::vector<std::string>({"older", "recent"}),
             "the same activity updates natural Last changed ordering");
  pane.setSortCriterion(ThreadPane::SortCriterion::Created);
  spin();
  result &=
      expect(threadOrder(pane) == std::vector<std::string>({"recent", "older"}),
             "prompt activity does not affect Created ordering");

  pane.setSortCriterion(ThreadPane::SortCriterion::Recency);
  spin();
  {
    auto write = graph.write();
    write.setField(recent, "localActivityAt", nodegraph::Value(41));
    write.setField(recent, "localPromptActivityAt", nodegraph::Value(41));
    static_cast<void>(write.finish());
  }
  refresh(pane, graph, "recent");
  result &= expect(
      threadOrder(pane) == std::vector<std::string>({"recent", "older"}),
      "a later prompt moves its thread first without losing prior activity");
  {
    auto write = graph.write();
    write.setField(older, "recencyAt", nodegraph::Value(20));
    static_cast<void>(write.finish());
  }
  refresh(pane, graph, "recent");
  auto read = graph.tryRead();
  const auto olderState = read ? read->state(older) : nullptr;
  const nodegraph::Value *localPrompt = nullptr;
  if (olderState) {
    const auto found = olderState->fields.find("localPromptActivityAt");
    if (found != olderState->fields.end())
      localPrompt = &found->second;
  }
  result &= expect(
      localPrompt && localPrompt->asInt64() && *localPrompt->asInt64() == 40 &&
          threadOrder(pane) == std::vector<std::string>({"recent", "older"}),
      "stale provider timestamps cannot undo newer local ordering");
  return result;
}
bool testOptimisticThreadRowLifecycle() {
  nodegraph::NodeGraph graph;
  {
    auto write = graph.write();
    static_cast<void>(runtimeWithRoots(write, {}));
    static_cast<void>(write.finish());
  }
  ThreadPane pane;
  pane.resize(320, 520);
  pane.show();
  pane.beginOptimisticThread("draft:new-thread", "Draft title",
                             "/workspace/draft");
  refresh(pane, graph, "draft:new-thread");
  spin();

  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  auto *animation =
      pane.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  QListWidgetItem *draft = threadItem(list, "draft:new-thread");
  bool result = expect(
      draft && pane.visiblySelectedThreadId() == "draft:new-thread" &&
          draft->data(Qt::UserRole + 6).toBool() &&
          !draft->data(Qt::UserRole + 7).toBool() && animation &&
          animation->isActive(),
      "a new-thread intent immediately presents one selected animated row");
  if (!draft)
    return false;

  {
    auto write = graph.write();
    const nodegraph::NodeRef created =
        addThread(write, "thread-created", "Created title", "idle");
    write.setField(created, "cwd", nodegraph::Value("/workspace/created"));
    const std::array roots{created};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }
  pane.promoteOptimisticThread("draft:new-thread", "thread-created");
  refresh(pane, graph, "thread-created");
  spin();
  QListWidgetItem *promoted = threadItem(list, "thread-created");
  result &=
      expect(promoted == draft && promoted->data(Qt::UserRole + 6).toBool() &&
                 pane.visiblySelectedThreadId() == "thread-created" &&
                 animation->isActive(),
             "thread/start rekeys the existing row without replacing its item "
             "or animation");

  pane.beginOptimisticThread("draft:second", "Second draft",
                             "/workspace/second");
  refresh(pane, graph, "draft:second");
  spin();
  QListWidgetItem *second = threadItem(list, "draft:second");
  result &= expect(second && threadItem(list, "thread-created") == draft &&
                       animation->isActive(),
                   "a second draft can animate while the first created thread "
                   "still awaits acknowledgment");

  pane.confirmOptimisticThread("thread-created");
  refresh(pane, graph, "draft:second");
  spin();
  result &= expect(threadItem(list, "thread-created") == draft &&
                       !draft->data(Qt::UserRole + 6).toBool() &&
                       !pane.isOptimisticThread("thread-created") &&
                       threadItem(list, "draft:second") == second &&
                       second->data(Qt::UserRole + 6).toBool() &&
                       animation->isActive(),
                   "acknowledging one new thread canonicalizes only that row");

  pane.failOptimisticThread("draft:second");
  refresh(pane, graph, "draft:second");
  spin();
  result &= expect(
      threadItem(list, "draft:second") == second &&
          second->data(Qt::UserRole + 7).toBool() && !animation->isActive(),
      "a failed new thread retains its row and stops only its animation");
  return result;
}

bool testThreadRowReorderOwnership() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef nodeA;
  nodegraph::NodeRef nodeB;
  {
    auto write = graph.write();
    nodeA = addThread(write, "thread-a", "A");
    nodeB = addThread(write, "thread-b", "B");
    const std::array roots{nodeA, nodeB};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  ThreadPane pane;
  int selectedByUser = 0;
  ThreadPane::NodeActions actions;
  actions.select = [&](const nodegraph::NodeRef &) { ++selectedByUser; };
  pane.setNodeActions(std::move(actions));
  pane.setSortCriterion(ThreadPane::SortCriterion::Alphanumeric);
  pane.resize(320, 500);
  pane.show();
  refresh(pane, graph, "thread-a");
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
  QPointer<QWidget> stableThreadARow = list->itemWidget(threadA);
  QPointer<QWidget> originalRow = list->itemWidget(threadB);

  {
    auto write = graph.write();
    write.setStatus(nodeB, nodegraph::NodeStatus::Completed);
    write.setField(nodeB, "status", nodegraph::Value("completed"));
    static_cast<void>(write.finish());
  }
  refresh(pane, graph, "thread-a");
  result &= expect(stableThreadARow == list->itemWidget(threadA) &&
                       originalRow == list->itemWidget(threadB),
                   "content-only refreshes preserve thread row widgets");

  {
    auto write = graph.write();
    write.setField(nodeA, "name", nodegraph::Value("Z"));
    static_cast<void>(write.finish());
  }
  refresh(pane, graph, "thread-a");
  QPointer<QWidget> movedRow = list->itemWidget(threadB);
  result &= expect(movedRow && originalRow != movedRow,
                   "moving an item never reattaches its deferred-delete row");
  if (!movedRow || originalRow == movedRow)
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

bool testThreadPaneDirectGraphBinding() {
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> roots;
  nodegraph::NodeRef runtime;
  {
    auto write = graph.write();
    runtime = write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
    for (int index = 0; index < 48; ++index) {
      const std::string id = QStringLiteral("thread-%1")
                                 .arg(index, 2, 10, QLatin1Char('0'))
                                 .toStdString();
      nodegraph::NodeState state;
      state.status = nodegraph::NodeStatus::Completed;
      state.fields.emplace("name", nodegraph::Value("Graph " + id));
      state.fields.emplace("cwd", nodegraph::Value("/workspace/" + id));
      state.fields.emplace("createdAt", nodegraph::Value(index));
      state.fields.emplace("updatedAt", nodegraph::Value(index));
      state.fields.emplace("recencyAt", nodegraph::Value(index));
      roots.emplace_back(
          write.upsert({nodegraph::NodeKind::Thread, id}, std::move(state)));
    }
    write.replaceRelated(runtime, nodegraph::RelationKind::RootThread, roots);

    // This history is intentionally absent from RootThread. Pending badge
    // rendering must stay proportional to the visible thread candidates, not
    // to unrelated protocol history retained by the shared graph.
    const nodegraph::NodeRef background =
        write.upsert({nodegraph::NodeKind::Thread, "background-history"});
    const nodegraph::NodeRef backgroundTurn =
        write.upsert({nodegraph::NodeKind::Turn, "background-turn"});
    write.setParent(background, backgroundTurn);
    std::vector<nodegraph::NodeRef> backgroundItems;
    backgroundItems.reserve(2048);
    for (int index = 0; index < 2048; ++index) {
      backgroundItems.emplace_back(
          write.upsert({nodegraph::NodeKind::Item,
                        "background-item-" + std::to_string(index)}));
    }
    write.replaceChildren(backgroundTurn, backgroundItems);
    for (int index = 0; index < 256; ++index) {
      nodegraph::NodeState pendingState;
      pendingState.status = nodegraph::NodeStatus::Pending;
      const nodegraph::NodeRef interaction =
          write.upsert({nodegraph::NodeKind::Interaction,
                        "string:background-pending-" + std::to_string(index)},
                       std::move(pendingState));
      write.relate(interaction, nodegraph::RelationKind::InteractionTarget,
                   backgroundItems[static_cast<std::size_t>(index * 8)]);
      write.relate(background, nodegraph::RelationKind::PendingInteraction,
                   interaction);
      write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                   interaction);
    }
    static_cast<void>(write.finish());
  }

  bool result = true;
  {
    ThreadPane pane;
    int nodeSelections = 0;
    nodegraph::NodeRef selectedByNodeAction;
    ThreadPane::NodeActions nodeActions;
    nodeActions.select = [&](const nodegraph::NodeRef &node) {
      ++nodeSelections;
      selectedByNodeAction = node;
    };
    pane.setNodeActions(std::move(nodeActions));
    pane.setSortCriterion(ThreadPane::SortCriterion::Alphanumeric);
    pane.resize(320, 220);
    pane.show();

    // Calling the entry point while the writer holds the graph proves that Qt
    // only schedules a try-read; it never waits synchronously.
    {
      auto write = graph.write();
      pane.refresh(graph, roots.front());
      static_cast<void>(write.finish());
    }
    spin(100);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
    std::size_t materialized = 0;
    for (const nodegraph::NodeRef &node : roots) {
      auto *attachment =
          static_cast<ui::QtNodeAttachment *>(node->uiAttachment());
      if (attachment && attachment->widget)
        ++materialized;
    }
    result &= expect(
        list && list->count() == static_cast<int>(roots.size()) &&
            pane.visiblySelectedThread() == roots.front() && materialized > 0 &&
            materialized < roots.size(),
        "ThreadPane reads one shared graph and materializes only its visible "
        "rows plus overscan");
    if (!list)
      return false;

    list->setCurrentRow(1);
    result &= expect(nodeSelections == 1 && selectedByNodeAction == roots[1],
                     "graph-bound selection dispatches one pinned NodeRef");

    nodegraph::NodeRef pendingInteraction;
    nodegraph::GraphChange pendingChange;
    {
      auto write = graph.write();
      nodegraph::NodeState state;
      state.status = nodegraph::NodeStatus::Pending;
      pendingInteraction =
          write.upsert({nodegraph::NodeKind::Interaction, "string:pending"},
                       std::move(state));
      write.relate(pendingInteraction,
                   nodegraph::RelationKind::InteractionTarget, roots[1]);
      write.relate(roots[1], nodegraph::RelationKind::PendingInteraction,
                   pendingInteraction);
      write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                   pendingInteraction);
      pendingChange = write.finish();
    }
    pane.graphChanged(nodegraph::GraphChanged{pendingChange.revision,
                                              pendingChange.affected,
                                              pendingChange.removed, false});
    spin(40);
    QListWidgetItem *pendingItem = threadItem(list, "thread-01");
    QWidget *pendingRow = pendingItem ? list->itemWidget(pendingItem) : nullptr;
    QLabel *pendingTitle =
        pendingRow
            ? pendingRow->findChild<QLabel *>(QStringLiteral("threadTitle"))
            : nullptr;
    result &= expect(pendingTitle && pendingTitle->text().startsWith("! "),
                     "pending graph interactions update the visible thread "
                     "badge without a copied view model");

    QPointer<QWidget> stablePendingRow = pendingRow;
    nodegraph::GraphChange resolvedChange;
    {
      auto write = graph.write();
      write.remove(pendingInteraction);
      resolvedChange = write.finish();
    }
    pane.graphChanged(nodegraph::GraphChanged{resolvedChange.revision,
                                              resolvedChange.affected,
                                              resolvedChange.removed, false});
    spin(40);
    pendingItem = threadItem(list, "thread-01");
    pendingRow = pendingItem ? list->itemWidget(pendingItem) : nullptr;
    pendingTitle =
        pendingRow
            ? pendingRow->findChild<QLabel *>(QStringLiteral("threadTitle"))
            : nullptr;
    result &=
        expect(pendingItem && stablePendingRow == pendingRow && pendingTitle &&
                   !pendingTitle->text().startsWith("! "),
               "interaction-only resolution clears the visible badge without "
               "rebuilding thread topology across large unrelated history");

    auto *removedAttachment =
        static_cast<ui::QtNodeAttachment *>(roots.front()->uiAttachment());
    QPointer<QWidget> removedWidget =
        removedAttachment ? removedAttachment->widget : nullptr;
    nodegraph::GraphChange removedChange;
    {
      auto write = graph.write();
      write.remove(roots.front());
      removedChange = write.finish();
    }
    pane.graphChanged(nodegraph::GraphChanged{removedChange.revision,
                                              removedChange.affected,
                                              removedChange.removed, false});
    result &= expect(
        roots.front()->uiAttachment() == nullptr && removedWidget.isNull() &&
            threadItem(list, "thread-00") == nullptr,
        "removed nodes synchronously detach and destroy their row before "
        "UiDetached acknowledgement");
  }

  result &= expect(
      std::ranges::all_of(roots,
                          [](const nodegraph::NodeRef &node) {
                            return node->uiAttachment() == nullptr;
                          }),
      "destroying ThreadPane clears every remaining opaque node attachment");
  return result;
}

bool testLargeThreadTopologyKeepsQtHeartbeatAlive() {
  constexpr int ThreadCount = 1536;
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> roots;
  roots.reserve(ThreadCount);
  {
    auto write = graph.write();
    for (int index = 0; index < ThreadCount; ++index) {
      const std::string suffix = std::to_string(index);
      nodegraph::NodeState state;
      state.status = nodegraph::NodeStatus::Completed;
      state.fields = {{"name", nodegraph::Value("Large thread " + suffix)},
                      {"status", nodegraph::Value("completed")},
                      {"recencyAt", nodegraph::Value(index)}};
      roots.emplace_back(
          write.upsert({nodegraph::NodeKind::Thread, "large-thread-" + suffix},
                       std::move(state)));
    }
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  ThreadPane pane;
  pane.resize(320, 220);
  pane.show();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  std::uint64_t heartbeatCount = 0;
  std::uint64_t partialHeartbeatCount = 0;
  QTimer heartbeat;
  heartbeat.setInterval(0);
  QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
    ++heartbeatCount;
    if (list && list->count() > 0 && list->count() < ThreadCount)
      ++partialHeartbeatCount;
  });
  heartbeat.start();

  pane.refresh(graph, roots.front());
  bool result = expect(list && list->count() == 0,
                       "large topology refresh schedules instead of "
                       "constructing all placeholders synchronously");
  QElapsedTimer deadline;
  deadline.start();
  while (list && list->count() != ThreadCount && deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  spin(50);
  heartbeat.stop();

  QListWidgetItem *requestedSelection =
      threadItem(list, roots.front()->id().canonical);
  result &= expect(
      list && requestedSelection && list->currentItem() == requestedSelection &&
          pane.visiblySelectedThread() == roots.front(),
      "large initial reconciliation restores the exact requested NodeRef "
      "selection after its row is sliced in last");

  std::uint64_t reorderHeartbeatCount = 0;
  std::uint64_t partialReorderHeartbeatCount = 0;
  QTimer reorderHeartbeat;
  reorderHeartbeat.setInterval(0);
  QObject::connect(&reorderHeartbeat, &QTimer::timeout, &reorderHeartbeat, [&] {
    ++reorderHeartbeatCount;
    if (!list || list->count() != ThreadCount)
      return;
    const std::string first =
        list->item(0)->data(Qt::UserRole).toString().toStdString();
    const std::string last = list->item(ThreadCount - 1)
                                 ->data(Qt::UserRole)
                                 .toString()
                                 .toStdString();
    if (first == "large-thread-0" && last != "large-thread-1535")
      ++partialReorderHeartbeatCount;
  });
  reorderHeartbeat.start();

  pane.setSortCriterion(ThreadPane::SortCriterion::Alphanumeric);
  deadline.restart();
  while (list &&
         (list->count() != ThreadCount ||
          list->item(0)->data(Qt::UserRole).toString() !=
              QStringLiteral("large-thread-0") ||
          list->item(ThreadCount - 1)->data(Qt::UserRole).toString() !=
              QStringLiteral("large-thread-1535")) &&
         deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  spin(50);
  reorderHeartbeat.stop();

  requestedSelection = threadItem(list, roots.front()->id().canonical);
  result &= expect(
      list && list->count() == ThreadCount &&
          list->item(0)->data(Qt::UserRole).toString() ==
              QStringLiteral("large-thread-0") &&
          list->item(ThreadCount - 1)->data(Qt::UserRole).toString() ==
              QStringLiteral("large-thread-1535") &&
          reorderHeartbeatCount > 1 && partialReorderHeartbeatCount > 0 &&
          requestedSelection && list->currentItem() == requestedSelection &&
          pane.visiblySelectedThread() == roots.front(),
      "reverse large-list reorder yields Qt ticks between bounded moves and "
      "preserves the requested selection");

  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  std::size_t materialized = 0;
  for (const nodegraph::NodeRef &node : roots) {
    const auto *attachment =
        static_cast<const ui::QtNodeAttachment *>(node->uiAttachment());
    if (attachment && attachment->widget)
      ++materialized;
  }
  result &= expect(
      list && list->count() == ThreadCount && heartbeatCount > 1 &&
          partialHeartbeatCount > 0 && materialized > 0 && materialized < 32,
      "large thread topology reconciliation is sliced across Qt passes while "
      "only viewport rows materialize");
  return result;
}

bool testLargeThreadScanReplacesAnObsoleteRevision() {
  constexpr int ThreadCount = 1024;
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> roots;
  roots.reserve(ThreadCount);
  {
    auto write = graph.write();
    for (int index = 0; index < ThreadCount; ++index) {
      const std::string suffix = std::to_string(index);
      roots.emplace_back(addThread(write, "revision-old-" + suffix,
                                   "Old revision " + suffix, "completed", {},
                                   {}, index));
    }
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  const nodegraph::NodeRef selected = roots[ThreadCount / 2];
  ThreadPane pane;
  pane.resize(320, 220);
  pane.show();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  pane.refresh(graph, selected);

  bool replaced = false;
  bool staleTopologyObserved = false;
  int eventTicks = 0;
  int replacementTick = 0;
  nodegraph::NodeRef latest;
  QTimer revisionChanger;
  revisionChanger.setInterval(0);
  QObject::connect(&revisionChanger, &QTimer::timeout, &revisionChanger, [&] {
    ++eventTicks;
    if (!replaced && eventTicks >= 4 && list && list->count() == 0) {
      const nodegraph::NodeRef retired = roots.front();
      auto write = graph.write();
      write.remove(retired);
      latest = addThread(write, "revision-newest", "Newest revision",
                         "completed", {}, {}, ThreadCount + 1000);
      std::vector<nodegraph::NodeRef> replacementRoots(roots.begin() + 1,
                                                       roots.end());
      replacementRoots.emplace_back(latest);
      const nodegraph::NodeRef runtime =
          write.find({nodegraph::NodeKind::Runtime, "runtime"});
      write.replaceRelated(runtime, nodegraph::RelationKind::RootThread,
                           replacementRoots);
      const nodegraph::GraphChange replacement = write.finish();
      pane.graphChanged(nodegraph::GraphChanged{replacement.revision,
                                                replacement.affected,
                                                replacement.removed, false});
      replaced = true;
      replacementTick = eventTicks;
    }
    if (replaced && list && list->count() > 0 &&
        list->item(0)->data(Qt::UserRole).toString() !=
            QStringLiteral("revision-newest"))
      staleTopologyObserved = true;
  });
  revisionChanger.start();

  QElapsedTimer deadline;
  deadline.start();
  while ((!replaced || !list || list->count() != ThreadCount ||
          list->item(0)->data(Qt::UserRole).toString() !=
              QStringLiteral("revision-newest")) &&
         deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  spin(30);
  revisionChanger.stop();

  QListWidgetItem *selectedItem = threadItem(list, selected->id().canonical);
  return expect(
      replaced && replacementTick >= 4 && !staleTopologyObserved && list &&
          list->count() == ThreadCount &&
          list->item(0)->data(Qt::UserRole).toString() ==
              QStringLiteral("revision-newest") &&
          !threadItem(list, "revision-old-0") && latest &&
          latest->uiAttachment() != nullptr && selectedItem &&
          list->currentItem() == selectedItem &&
          pane.visiblySelectedThread() == selected,
      "an in-flight scan discards its obsolete revision before exposing rows "
      "and restores selection from the replacement revision");
}

bool testThreadScanCompletesUnderContinuousGraphChanges() {
  constexpr int ThreadCount = 768;
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> roots;
  roots.reserve(ThreadCount);
  nodegraph::NodeRef unrelated;
  {
    auto write = graph.write();
    for (int index = 0; index < ThreadCount; ++index) {
      const std::string suffix = std::to_string(index);
      roots.emplace_back(addThread(write, "churn-thread-" + suffix,
                                   "Churn thread " + suffix, "completed", {},
                                   {}, index));
    }
    static_cast<void>(runtimeWithRoots(write, roots));
    unrelated =
        write.upsert({nodegraph::NodeKind::Item, "unrelated-stream-item"});
    static_cast<void>(write.finish());
  }

  ThreadPane pane;
  pane.resize(320, 220);
  pane.show();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  bool result = true;

  // Force the first scheduled read to encounter the writer. The retry must be
  // deferred rather than spinning a zero-delay timer while the lock is held.
  {
    auto write = graph.write();
    pane.refresh(graph, roots.front());
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    result &=
        expect(pane.graphReadRetryCount() > 0 && list && list->count() == 0,
               "ThreadPane records nonblocking graph-lock contention and "
               "leaves the current UI untouched");
    static_cast<void>(write.finish());
  }

  int churnTicks = 0;
  int contendedNotifications = 0;
  QTimer churn;
  churn.setInterval(0);
  QObject::connect(&churn, &QTimer::timeout, &churn, [&] {
    ++churnTicks;
    nodegraph::GraphChange change;
    {
      auto write = graph.write();
      if (churnTicks % 2 == 0) {
        write.setField(unrelated, "streamSequence",
                       nodegraph::Value(churnTicks));
      } else {
        // This is a real Thread notification, but does not change roots,
        // hierarchy, presentation, or sorting. It must not request topology
        // work or cancel the topology currently making progress.
        write.setField(roots.front(), "streamSequence",
                       nodegraph::Value(churnTicks));
      }
      change = write.finish();
    }
    // Model the next worker transaction already owning the graph when Qt
    // handles the previous notification. Classification contention must not
    // turn state-only churn into topology invalidation.
    {
      auto nextWorkerWrite = graph.write();
      pane.graphChanged(nodegraph::GraphChanged{
          change.revision, change.affected, change.removed, false});
      ++contendedNotifications;
      static_cast<void>(nextWorkerWrite.finish());
    }
  });
  churn.start();

  QElapsedTimer deadline;
  deadline.start();
  while (pane.completedTopologyCount() == 0 && deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  const bool completedDuringChurn =
      pane.completedTopologyCount() > 0 && list && list->count() == ThreadCount;
  churn.stop();

  spin(30);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

  result &= expect(
      completedDuringChurn && churnTicks > 8 && contendedNotifications > 8 &&
          pane.completedTopologyCount() == 1,
      "continuous unrelated and Thread state revisions cannot starve a scan, "
      "and non-presentation fields request no follow-up topology");
  result &= expect(
      pane.maximumGraphScanWorkObserved() <= 64 &&
          pane.maximumTopologyWorkObserved() <= 32 &&
          pane.maximumVisibilityWorkObserved() <= 12 &&
          pane.materializedRowCount() > 0 && pane.materializedRowCount() < 32,
      "ThreadPane instrumentation proves graph, topology, and visible-widget "
      "work stay within their fixed per-pass and viewport bounds");

  QListWidgetItem *visibleItem = nullptr;
  if (list)
    for (int row = 0; row < list->count(); ++row)
      if (list->itemWidget(list->item(row))) {
        visibleItem = list->item(row);
        break;
      }
  nodegraph::NodeRef visibleNode;
  if (visibleItem) {
    const std::string id =
        visibleItem->data(Qt::UserRole).toString().toStdString();
    const auto found = std::ranges::find_if(
        roots, [&id](const nodegraph::NodeRef &candidate) {
          return candidate && candidate->id().canonical == id;
        });
    if (found != roots.end())
      visibleNode = *found;
  }
  QWidget *visibleRow = visibleItem && list ? list->itemWidget(visibleItem)
                                            : nullptr;
  QLabel *visibleTitle = visibleRow
                             ? visibleRow->findChild<QLabel *>(
                                   QStringLiteral("threadTitle"))
                             : nullptr;
  const qulonglong topologyBeforeRowPatch =
      pane.property("graphTopologyScansStarted").toULongLong();
  const qulonglong rowPatchesBefore =
      pane.property("rowPresentationUpdates").toULongLong();
  const qulonglong suppressionsBefore =
      pane.property("wholePaneUpdateSuppressions").toULongLong();
  if (visibleNode) {
    nodegraph::GraphChange renamed;
    {
      auto write = graph.write();
      write.setField(visibleNode, "name", "One locally patched row");
      renamed = write.finish();
    }
    pane.graphChanged(nodegraph::GraphChanged{
        renamed.revision, renamed.affected, renamed.removed, false});
    spin(40);
  }
  result &= expect(
      visibleNode && visibleTitle &&
          visibleTitle->text() == QStringLiteral("One locally patched row") &&
          pane.property("graphTopologyScansStarted").toULongLong() ==
              topologyBeforeRowPatch &&
          pane.property("rowPresentationUpdates").toULongLong() ==
              rowPatchesBefore + 1 &&
          pane.property("wholePaneUpdateSuppressions").toULongLong() ==
              suppressionsBefore,
      "a visible non-sort Thread name change patches exactly one row without "
      "a topology scan or whole-list update suppression");

  QListWidgetItem *firstItem = list ? list->item(0) : nullptr;
  if (firstItem && list) {
    const QPoint position = list->visualItemRect(firstItem).center();
    const std::uint64_t retriesBefore = pane.contextMenuReadRetryCount();
    {
      auto write = graph.write();
      QContextMenuEvent contextMenuEvent(
          QContextMenuEvent::Mouse, position,
          list->viewport()->mapToGlobal(position));
      QApplication::sendEvent(list->viewport(), &contextMenuEvent);
      result &= expect(
          pane.contextMenuReadRetryCount() == retriesBefore + 1,
          "a contended context-menu read schedules a bounded nonzero retry");
      static_cast<void>(write.finish());
    }
    spin(40);
    if (QWidget *popup = QApplication::activePopupWidget())
      popup->close();
  } else {
    result &= expect(false, "the churn test retains a row for context actions");
  }
  return result;
}

bool testThreadRelationChangeDuringValidationRestartsCandidate() {
  constexpr int ChildCount = 512;
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef parentX;
  nodegraph::NodeRef parentY;
  std::vector<nodegraph::NodeRef> children;
  children.reserve(ChildCount);
  {
    auto write = graph.write();
    parentX = addThread(write, "validation-parent-x", "Parent X");
    parentY = addThread(write, "validation-parent-y", "Parent Y");
    for (int index = 0; index < ChildCount; ++index) {
      children.emplace_back(
          addThread(write, "validation-child-" + std::to_string(index),
                    "Validation child " + std::to_string(index)));
    }
    write.replaceRelated(
        parentX, nodegraph::RelationKind::StructuralChildThread, children);
    const std::array roots{parentX, parentY};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  const nodegraph::NodeRef movedChild = children.front();
  ThreadPane pane;
  pane.resize(320, 220);
  pane.show();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  pane.refresh(graph, parentY);

  bool movedBetweenValidationPasses = false;
  bool notificationDeliveredAfterValidationTurn = false;
  bool discardedBeforeNotification = false;
  std::optional<nodegraph::GraphChange> delayedNotification;
  QTimer mover;
  mover.setInterval(0);
  QObject::connect(&mover, &QTimer::timeout, &mover, [&] {
    if (movedBetweenValidationPasses) {
      if (delayedNotification && pane.discardedTopologyCount() > 0) {
        discardedBeforeNotification = true;
        pane.graphChanged(nodegraph::GraphChanged{
            delayedNotification->revision, delayedNotification->affected,
            delayedNotification->removed, false});
        delayedNotification.reset();
        notificationDeliveredAfterValidationTurn = true;
      }
      return;
    }
    if (pane.topologyValidationPassCount() == 0 ||
        pane.completedTopologyCount() != 0)
      return;
    {
      auto write = graph.write();
      write.unrelate(parentX, nodegraph::RelationKind::StructuralChildThread,
                     movedChild);
      write.relate(parentY, nodegraph::RelationKind::StructuralChildThread,
                   movedChild);
      delayedNotification = write.finish();
    }
    movedBetweenValidationPasses = true;
  });
  mover.start();

  QElapsedTimer deadline;
  deadline.start();
  while ((!notificationDeliveredAfterValidationTurn ||
          pane.completedTopologyCount() == 0) &&
         deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  mover.stop();

  const std::uint64_t completedBeforeReveal = pane.completedTopologyCount();
  pane.refresh(graph, movedChild);
  QListWidgetItem *movedItem = nullptr;
  deadline.restart();
  while (deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
    movedItem = threadItem(list, movedChild->id().canonical);
    if (pane.completedTopologyCount() > completedBeforeReveal && movedItem &&
        movedItem->data(Qt::UserRole + 5).toString() ==
            QStringLiteral("validation-parent-y"))
      break;
  }
  spin(30);

  int childRows = 0;
  if (list) {
    for (int row = 0; row < list->count(); ++row) {
      if (list->item(row)->data(Qt::UserRole).toString() ==
          QString::fromStdString(movedChild->id().canonical))
        ++childRows;
    }
  }
  const bool correct = movedBetweenValidationPasses &&
                       notificationDeliveredAfterValidationTurn &&
                       discardedBeforeNotification && movedItem &&
                       childRows == 1 &&
                       movedItem->data(Qt::UserRole + 5).toString() ==
                           QStringLiteral("validation-parent-y") &&
                       pane.visiblySelectedThread() == movedChild &&
                       pane.maximumTopologyWorkObserved() <= 32;
  return expect(
      correct,
      "a child reassigned between bounded relation-validation chunks is "
      "published once under its current canonical parent");
}

bool testRemovalBeforeMaterializationReleasesSelection() {
  constexpr int ThreadCount = 512;
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> roots;
  roots.reserve(ThreadCount);
  {
    auto write = graph.write();
    for (int index = 0; index < ThreadCount; ++index) {
      const std::string suffix = std::to_string(index);
      roots.emplace_back(addThread(write, "unmaterialized-" + suffix,
                                   "Unmaterialized " + suffix, "completed", {},
                                   {}, index));
    }
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  nodegraph::NodeRef selected = roots.front();
  nodegraph::NodeRef placeholder = roots.back();
  std::weak_ptr<nodegraph::Node> selectedLifetime = selected;
  std::weak_ptr<nodegraph::Node> placeholderLifetime = placeholder;
  ThreadPane pane;
  pane.resize(320, 220);
  pane.show();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  pane.refresh(graph, selected);

  bool removed = false;
  bool removedBeforeMaterialization = false;
  bool detachedSynchronously = false;
  bool releasedAfterAcknowledgement = false;
  int eventTicks = 0;
  QTimer remover;
  remover.setInterval(0);
  QObject::connect(&remover, &QTimer::timeout, &remover, [&] {
    ++eventTicks;
    QListWidgetItem *placeholderItem =
        placeholder ? threadItem(list, placeholder->id().canonical) : nullptr;
    if (removed || eventTicks < 4 || !list || list->count() == 0 ||
        list->count() >= ThreadCount || !placeholderItem ||
        threadItem(list, selected->id().canonical))
      return;

    {
      const nodegraph::NodeRef retiringSelection = selected;
      const nodegraph::NodeRef retiringPlaceholder = placeholder;
      const auto *attachment = static_cast<const ui::QtNodeAttachment *>(
          retiringPlaceholder->uiAttachment());
      removedBeforeMaterialization =
          attachment && !attachment->widget && list->currentItem() == nullptr;
      nodegraph::GraphChange removal;
      {
        auto write = graph.write();
        write.remove(retiringSelection);
        write.remove(retiringPlaceholder);
        removal = write.finish();
      }
      pane.graphChanged(nodegraph::GraphChanged{
          removal.revision, removal.affected, removal.removed, false});
      detachedSynchronously = retiringSelection->uiAttachment() == nullptr &&
                              retiringPlaceholder->uiAttachment() == nullptr &&
                              pane.visiblySelectedThreadId().empty();
      {
        const std::array<nodegraph::NodeRef, 2> acknowledged{
            retiringSelection, retiringPlaceholder};
        auto write = graph.write();
        write.releaseRetired(acknowledged);
        static_cast<void>(write.finish());
      }
      std::erase_if(roots, [&](const nodegraph::NodeRef &node) {
        return node == retiringSelection || node == retiringPlaceholder;
      });
      selected.reset();
      placeholder.reset();
      removal.affected.clear();
      removal.removed.clear();
    }
    releasedAfterAcknowledgement =
        selectedLifetime.expired() && placeholderLifetime.expired();
    removed = true;
  });
  remover.start();

  QElapsedTimer deadline;
  deadline.start();
  while ((!removed || !list || list->count() != ThreadCount - 2) &&
         deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  spin(30);
  remover.stop();

  return expect(
      removed && eventTicks >= 4 && removedBeforeMaterialization &&
          detachedSynchronously && releasedAfterAcknowledgement && list &&
          list->count() == ThreadCount - 2 &&
          pane.visiblySelectedThreadId().empty(),
      "removal before widget materialization detaches its placeholder, "
      "cancels a not-yet-inserted selection, and permits safe retirement");
}

bool testDestroyThreadPaneWithQueuedTopologyPasses() {
  constexpr int ThreadCount = 512;
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> roots;
  roots.reserve(ThreadCount);
  {
    auto write = graph.write();
    for (int index = 0; index < ThreadCount; ++index) {
      const std::string suffix = std::to_string(index);
      roots.emplace_back(addThread(write, "destroy-queued-" + suffix,
                                   "Destroy queued " + suffix, "completed", {},
                                   {}, index));
    }
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  QPointer<ThreadPane> pane = new ThreadPane;
  pane->resize(320, 220);
  pane->show();
  QPointer<QListWidget> list =
      pane->findChild<QListWidget *>(QStringLiteral("threadList"));
  pane->refresh(graph, roots.front());

  bool destroyedDuringPartialTopology = false;
  bool hadAttachedPlaceholders = false;
  QTimer destroyer;
  destroyer.setInterval(0);
  QObject::connect(&destroyer, &QTimer::timeout, &destroyer, [&] {
    if (!pane || !list || list->count() == 0 || list->count() >= ThreadCount)
      return;
    for (const nodegraph::NodeRef &node : roots)
      hadAttachedPlaceholders =
          hadAttachedPlaceholders || node->uiAttachment() != nullptr;
    destroyedDuringPartialTopology = true;
    destroyer.stop();
    delete pane.data();
  });
  destroyer.start();

  QElapsedTimer deadline;
  deadline.start();
  while (pane && deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  destroyer.stop();
  spin(50);

  const bool allAttachmentsCleared =
      std::ranges::all_of(roots, [](const nodegraph::NodeRef &node) {
        return node->uiAttachment() == nullptr;
      });
  if (pane)
    delete pane.data();
  return expect(
      destroyedDuringPartialTopology && hadAttachedPlaceholders && !pane &&
          !list && allAttachmentsCleared,
      "destroying ThreadPane during a partial topology clears every opaque "
      "attachment and cancels all queued Qt passes");
}

bool testNestedCommandScrollOwnership() {
  MiddleRegionWidget region;
  region.resize(1500, 820);
  region.show();
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  populateLongConversation(graph, thread, "command-thread");
  QString output;
  for (int line = 0; line < 100; ++line)
    output += QStringLiteral("command output line %1\n").arg(line);
  QString command;
  for (int line = 0; line < 30; ++line)
    command += QStringLiteral("command argument line %1\n").arg(line);
  {
    auto write = graph.write();
    nodegraph::NodeState state;
    state.status = nodegraph::NodeStatus::Running;
    state.fields = {{"type", nodegraph::Value("commandExecution")},
                    {"command", nodegraph::Value(utf8(command))},
                    {"aggregatedOutput", nodegraph::Value(utf8(output))},
                    {"status", nodegraph::Value("inProgress")}};
    const nodegraph::NodeRef item =
        write.upsert({nodegraph::NodeKind::Item, "command"}, std::move(state));
    write.setParent(write.find({nodegraph::NodeKind::Turn, "turn-2"}), item);
    static_cast<void>(write.finish());
  }
  region.conversation().bindGraph(graph, thread);
  spin(30);

  CommandOutputView *commandOutput = nullptr;
  ContentSizedTextView *commandText = nullptr;
  for (QWidget *widget : region.findChildren<QWidget *>())
    if (auto *candidate = dynamic_cast<CommandOutputView *>(widget)) {
      commandOutput = candidate;
    } else if (auto *candidate = dynamic_cast<ContentSizedTextView *>(widget);
               candidate &&
               candidate->objectName() == QStringLiteral("commandTextView")) {
      commandText = candidate;
    }
  bool result = expect(
      commandOutput && commandOutput->verticalScrollBar()->maximum() > 0 &&
          commandText && commandText->verticalScrollBar()->maximum() > 0,
      "long command and output own real nested scrollbars");
  if (!commandOutput || !commandText)
    return false;

  auto verifyBoundaryOwnership = [&](ContentSizedTextView *view,
                                     const char *description) {
    QScrollBar *inner = view->verticalScrollBar();
    QScrollBar *outer = region.conversation().verticalScrollBar();

    inner->setValue(inner->maximum() / 2);
    outer->setValue(outer->maximum());
    spin();
    QWheelEvent begin = wheelFor(view, 0, Qt::ScrollBegin);
    region.routeScrollEvent(view, &begin);
    QWheelEvent firstUpdate = wheelFor(view, 120, Qt::ScrollUpdate);
    bool passed =
        expect(!region.routeScrollEvent(view, &firstUpdate), description);

    inner->setValue(inner->minimum());
    const int outerBeforeOverscroll = outer->value();
    QWheelEvent sameGesture = wheelFor(view, 120, Qt::ScrollUpdate);
    passed &=
        expect(!region.routeScrollEvent(view, &sameGesture) &&
                   outer->value() == outerBeforeOverscroll,
               "a gesture reaching the top cannot leak to the conversation");
    QWheelEvent end = wheelFor(view, 0, Qt::ScrollEnd);
    region.routeScrollEvent(view, &end);

    QWheelEvent freshAtTop = wheelFor(view, 120, Qt::ScrollBegin);
    passed &=
        expect(region.routeScrollEvent(view, &freshAtTop) &&
                   outer->value() < outerBeforeOverscroll,
               "a fresh outward gesture at the top scrolls the conversation");
    QWheelEvent topEnd = wheelFor(view, 0, Qt::ScrollEnd);
    region.routeScrollEvent(view, &topEnd);

    inner->setValue(inner->maximum() / 2);
    outer->setValue(outer->minimum());
    QWheelEvent downBegin = wheelFor(view, -120, Qt::ScrollBegin);
    passed &= expect(!region.routeScrollEvent(view, &downBegin), description);
    inner->setValue(inner->maximum());
    const int outerBeforeBottomOverscroll = outer->value();
    QWheelEvent sameDownGesture = wheelFor(view, -120, Qt::ScrollUpdate);
    passed &=
        expect(!region.routeScrollEvent(view, &sameDownGesture) &&
                   outer->value() == outerBeforeBottomOverscroll,
               "a gesture reaching the bottom cannot leak to the conversation");
    QWheelEvent downEnd = wheelFor(view, 0, Qt::ScrollEnd);
    region.routeScrollEvent(view, &downEnd);

    QWheelEvent freshAtBottom = wheelFor(view, -120, Qt::ScrollBegin);
    passed &= expect(
        region.routeScrollEvent(view, &freshAtBottom) &&
            outer->value() > outerBeforeBottomOverscroll,
        "a fresh outward gesture at the bottom scrolls the conversation");
    QWheelEvent bottomEnd = wheelFor(view, 0, Qt::ScrollEnd);
    region.routeScrollEvent(view, &bottomEnd);

    inner->setValue(inner->maximum() / 2);
    outer->setValue(outer->maximum());
    const int outerBeforeMouseWheel = outer->value();
    QWheelEvent innerNotch = wheelFor(view, 120, Qt::NoScrollPhase);
    passed &= expect(!region.routeScrollEvent(view, &innerNotch) &&
                         outer->value() == outerBeforeMouseWheel,
                     "a mouse-wheel notch scrolls a movable nested view");
    inner->setValue(inner->minimum());
    QWheelEvent boundaryNotch = wheelFor(view, 120, Qt::NoScrollPhase);
    passed &=
        expect(region.routeScrollEvent(view, &boundaryNotch) &&
                   outer->value() < outerBeforeMouseWheel,
               "a mouse-wheel notch at the boundary scrolls the conversation");
    return passed;
  };

  result &= verifyBoundaryOwnership(commandText,
                                    "command text owns a scrollable gesture");
  result &= verifyBoundaryOwnership(commandOutput,
                                    "command output owns a scrollable gesture");
  return result;
}

bool testInfoViewerLayout() {
  nodegraph::NodeGraph graph;
  {
    auto write = graph.write();
    static_cast<void>(write.upsert({nodegraph::NodeKind::Runtime, "runtime"}));
    for (int index = 0; index < 90; ++index) {
      nodegraph::NodeState state;
      state.status = index == 0 ? nodegraph::NodeStatus::Failed
                                : nodegraph::NodeStatus::Pending;
      state.fields = {
          {"method",
           nodegraph::Value("protocol/test/" + std::to_string(index))},
          {"payload", nodegraph::Value(nodegraph::Value::Object{
                          {"private", nodegraph::Value("must-not-render")}})}};
      static_cast<void>(
          write.upsert({nodegraph::NodeKind::Operation,
                        "fixture-operation:" + std::to_string(index)},
                       std::move(state)));
    }
    nodegraph::NodeState unknownState;
    unknownState.fields = {
        {"method", nodegraph::Value("future/protocol/method")},
        {"direction", nodegraph::Value(std::uint64_t{2})},
        {"payload", nodegraph::Value(nodegraph::Value::Object{
                        {"private", nodegraph::Value("must-not-render")}})}};
    static_cast<void>(write.upsert(
        {nodegraph::NodeKind::UnknownProtocol, "2:future/protocol/method"},
        std::move(unknownState)));
    static_cast<void>(write.finish());
  }

  InspectorPane inspector;
  inspector.resize(420, 700);
  inspector.show();
  inspector.refresh(graph);
  inspector.tabs()->setCurrentIndex(4);
  auto *infoStack =
      inspector.findChild<QStackedWidget *>(QStringLiteral("infoStack"));
  auto *protocolChoice =
      inspector.findChild<QPushButton *>(QStringLiteral("protocolInfoChoice"));
  auto *protocol =
      inspector.findChild<QPlainTextEdit *>(QStringLiteral("protocolInfoLog"));
  auto *state =
      inspector.findChild<QPlainTextEdit *>(QStringLiteral("stateInfoView"));
  auto *statistics =
      inspector.findChild<QLabel *>(QStringLiteral("protocolInfoStats"));
  bool result =
      expect(infoStack && protocolChoice && protocol && state && statistics,
             "Info exposes State and Protocol through choice navigation");
  if (!infoStack || !protocolChoice || !protocol || !state || !statistics)
    return false;
  const auto inspectorScrolls = inspector.findChildren<QScrollArea *>();
  result &= expect(
      inspectorScrolls.size() == 3 &&
          std::ranges::all_of(
              inspectorScrolls,
              [](QScrollArea *scroll) {
                return scroll &&
                       scroll->property("kind") == "inspectorScroll" &&
                       scroll->verticalScrollBarPolicy() ==
                           Qt::ScrollBarAsNeeded &&
                       scroll->verticalScrollBar()
                           ->property("kind")
                           .toString()
                           .isEmpty() &&
                       scroll->verticalScrollBar()->styleSheet().isEmpty();
              }),
      "Plan, Agents, and Requests inherit the canonical application "
      "scrollbar");
  for (std::uint64_t sequence = 1; sequence <= 90; ++sequence)
    inspector.appendProtocolDiagnostic(
        protocolDiagnostic(sequence, "server notification",
                           "protocol/test/" + std::to_string(sequence)));
  protocolChoice->click();
  spin(20);
  result &=
      expect(protocol->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded &&
                 state->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
             "both Info viewers use the common as-needed scrollbar policy");
  result &= expect(
      protocol->verticalScrollBar()->property("kind").toString().isEmpty() &&
          state->verticalScrollBar()->property("kind").toString().isEmpty() &&
          protocol->verticalScrollBar()->styleSheet().isEmpty() &&
          state->verticalScrollBar()->styleSheet().isEmpty(),
      "both Info viewer scrollbars inherit the shared visual style");
  result &= expect(
      protocol->toPlainText().contains(QStringLiteral("protocol/test/1")) &&
          protocol->toPlainText().contains(QStringLiteral("#90")) &&
          protocol->toPlainText().contains(
              QStringLiteral("server notification")) &&
          !protocol->toPlainText().contains(QStringLiteral("must-not-render")),
      "Protocol shows chronological metadata without retaining payloads");
  QScrollBar *protocolScroll = protocol->verticalScrollBar();
  result &= expect(protocolScroll->maximum() > 0 &&
                       protocolScroll->value() == protocolScroll->maximum(),
                   "Protocol follows new frames while already at the tail");
  protocolScroll->setValue(protocolScroll->maximum() / 3);
  spin();
  const int pausedValue = protocolScroll->value();
  inspector.appendProtocolDiagnostic(protocolDiagnostic(
      91, "server notification", "protocol/test/visible-update"));
  spin(20);
  result &=
      expect(protocolScroll->value() == pausedValue,
             "a visible Protocol update preserves a user-paused position");
  infoStack->setCurrentIndex(0);
  inspector.appendProtocolDiagnostic(protocolDiagnostic(
      92, "server notification", "protocol/test/hidden-update"));
  protocolChoice->click();
  spin(20);
  result &=
      expect(protocolScroll->value() == pausedValue &&
                 protocol->toPlainText().contains(
                     QStringLiteral("protocol/test/hidden-update")),
             "Protocol refresh preserves its paused position across tabs");
  // Drive an explicit user-like move to the tail even when a transient page
  // relayout has clamped the preserved paused value to its current maximum.
  protocolScroll->setValue(protocolScroll->minimum());
  protocolScroll->setValue(protocolScroll->maximum());
  inspector.appendProtocolDiagnostic(protocolDiagnostic(
      93, "server notification", "protocol/test/following-update"));
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
  const QString previousStyleSheet = qApp->styleSheet();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef ownerThread;
  nodegraph::NodeRef runtime;
  {
    auto write = graph.write();
    ownerThread = addThread(write, "owner-thread", "Original title");
    const nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "turn-one"});
    write.setParent(ownerThread, turn);

    nodegraph::Value::Array receivers{nodegraph::Value("receiver-one"),
                                      nodegraph::Value("receiver-two")};
    nodegraph::NodeState firstAgent;
    firstAgent.status = nodegraph::NodeStatus::Running;
    firstAgent.fields = {
        {"type", nodegraph::Value("subAgentActivity")},
        {"status", nodegraph::Value("inProgress")},
        {"agentPath", nodegraph::Value("/root/lifecycle_review")},
        {"agentThreadId", nodegraph::Value("child-thread")},
        {"resultText",
         nodegraph::Value(
             "No blocking Inspector findings.\n\n"
             "* Typed snapshots cover all rendered Plan, Agent, and Request "
             "fields, with default equality and optional first-render state.\n"
             "* Rendering now uses those typed projections directly.\n"
             "* Request thread titles participate in equality, so rename-only "
             "changes invalidate correctly.\n"
             "* optional size correctly distinguishes absent questions from a "
             "visible zero questions.\n"
             "* The added Application Layout regression is minimal and well "
             "targeted: render the original title, rename without changing the "
             "request, refresh, then require the new label and reject the old "
             "one.\n"
             "* Application Layout tests pass offscreen.\n\n"
             "No files were edited.")},
        {"senderThreadId", nodegraph::Value("sender-thread")},
        {"receiverThreadIds", nodegraph::Value(std::move(receivers))}};
    const nodegraph::NodeRef first = write.upsert(
        {nodegraph::NodeKind::Item, "agent-one"}, std::move(firstAgent));
    write.setParent(turn, first);

    nodegraph::NodeState secondAgent;
    secondAgent.status = nodegraph::NodeStatus::Completed;
    secondAgent.fields = {
        {"type", nodegraph::Value("subAgentActivity")},
        {"status", nodegraph::Value("completed")},
        {"agentPath", nodegraph::Value("/root/hierarchy_ui_review")},
        {"agentThreadId", nodegraph::Value("child-thread-two")},
        {"resultText",
         nodegraph::Value(
             "No blocking Git snapshot issues found.\n\n"
             "* Add defaulted equality to the file and snapshot records.\n"
             "* Replace both retained snapshot hashes with optional typed "
             "snapshots so an initial empty result still renders.\n"
             "* Preserve the current snapshot fallback and repository context "
             "behavior.")}};
    const nodegraph::NodeRef second = write.upsert(
        {nodegraph::NodeKind::Item, "agent-two"}, std::move(secondAgent));
    write.setParent(turn, second);

    const std::array roots{ownerThread};
    runtime = runtimeWithRoots(write, roots);
    addInteraction(write, runtime, ownerThread, "request-one", "user-input",
                   {{"message", nodegraph::Value("Choose an option")},
                    {"questions", nodegraph::Value(nodegraph::Value::Array{
                                      nodegraph::Value(1), nodegraph::Value(2),
                                      nodegraph::Value(3)})}});
    static_cast<void>(write.finish());
  }
  InspectorPane inspector;
  inspector.resize(420, 700);
  inspector.show();
  refresh(inspector, graph, "owner-thread");
  inspector.tabs()->setCurrentIndex(1);
  spin(20);
  bool result = expect(
      hasLabelContaining(
          inspector,
          QStringLiteral("thread child-thread  |  sender sender-thread  |  "
                         "receivers receiver-one, receiver-two")),
      "Agents show child, sender, and receiver thread identities");
  QLabel *agentStatus = nullptr;
  for (QLabel *label : inspector.findChildren<QLabel *>()) {
    if (label->text() == QStringLiteral("running")) {
      agentStatus = label;
      break;
    }
  }
  result &= expect(agentStatus && agentStatus->property("tone") == "active",
                   "running agent status uses the canonical active tone");
  auto *agentResult =
      inspector.findChild<QLabel *>(QStringLiteral("agentResult"));
  QWidget *agentContent = agentResult ? agentResult->parentWidget() : nullptr;
  auto *agentFrame = agentContent
                         ? qobject_cast<QFrame *>(agentContent->parentWidget())
                         : nullptr;
  auto *agentTitle =
      agentFrame ? agentFrame->findChild<QLabel *>(QStringLiteral("agentTitle"))
                 : nullptr;
  auto *agentName =
      agentFrame ? agentFrame->findChild<QLabel *>(QStringLiteral("agentName"))
                 : nullptr;
  auto *agentCopy = agentFrame ? agentFrame->findChild<QToolButton *>(
                                     QStringLiteral("agentCopyButton"))
                               : nullptr;
  auto *agentDisclosure = agentFrame
                              ? agentFrame->findChild<QToolButton *>(
                                    QStringLiteral("agentDisclosureButton"))
                              : nullptr;
  result &= expect(
      agentContent && !agentContent->isVisible() && agentDisclosure &&
          agentDisclosure->accessibleName() == QStringLiteral("Expand agent"),
      "agent cards initially retain their content collapsed");
  if (agentDisclosure) {
    agentDisclosure->click();
    spin();
  }
  if (agentCopy) {
    agentCopy->click();
    spin();
  }
  result &= expect(agentCopy &&
                       QApplication::clipboard()->text().contains(
                           QStringLiteral("No blocking Inspector findings.")),
                   "agent copy actions retain the complete agent content");
  const int statusBottom =
      agentStatus && agentFrame
          ? agentStatus->mapTo(agentFrame, QPoint()).y() + agentStatus->height()
          : 0;
  const int headingBottom = std::max(
      {agentTitle && agentFrame
           ? agentTitle->mapTo(agentFrame, QPoint()).y() + agentTitle->height()
           : 0,
       agentName && agentFrame
           ? agentName->mapTo(agentFrame, QPoint()).y() + agentName->height()
           : 0,
       statusBottom,
       agentCopy && agentFrame
           ? agentCopy->mapTo(agentFrame, QPoint()).y() + agentCopy->height()
           : 0,
       agentDisclosure && agentFrame
           ? agentDisclosure->mapTo(agentFrame, QPoint()).y() +
                 agentDisclosure->height()
           : 0});
  const int resultTop = agentResult && agentFrame
                            ? agentResult->mapTo(agentFrame, QPoint()).y()
                            : 0;
  const int resultHeightForWidth =
      agentResult ? agentResult->heightForWidth(agentResult->width()) : -1;
  result &= expect(agentStatus && agentStatus->width() > 0 &&
                       !agentStatus->visibleRegion().isEmpty(),
                   "agent status occupies the card heading instead of "
                   "leaving an invisible gap");
  result &= expect(
      agentTitle && agentTitle->text() == QStringLiteral("Agent") &&
          agentTitle->property("kind").toString() == QStringLiteral("title") &&
          agentTitle->contentsMargins().bottom() == 0 && agentName &&
          agentName->text() == QStringLiteral("lifecycle_review") &&
          agentName->property("kind").toString() == QStringLiteral("code") &&
          agentName->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored &&
          agentName->toolTip() == QStringLiteral("/root/lifecycle_review") &&
          agentStatus &&
          agentStatus->geometry().left() > agentName->geometry().left() &&
          agentCopy && agentDisclosure &&
          agentCopy->geometry().left() > agentStatus->geometry().left() &&
          agentDisclosure->geometry().left() > agentCopy->geometry().left() &&
          agentDisclosure->accessibleName() ==
              QStringLiteral("Collapse agent") &&
          !hasLabelContaining(inspector,
                              QStringLiteral("/root/lifecycle_review")),
      "agent cards show identity then status, copy, and disclosure actions "
      "while retaining the full path as a tooltip");
  const auto baseline = [agentFrame](QLabel *label) {
    return label && agentFrame ? label->mapTo(agentFrame, QPoint()).y() +
                                     label->contentsMargins().top() +
                                     label->fontMetrics().ascent()
                               : -1000;
  };
  result &= expect(
      std::abs(baseline(agentTitle) - baseline(agentName)) <= 1 &&
          std::abs(baseline(agentTitle) - baseline(agentStatus)) <= 1 &&
          agentFrame && agentFrame->parentWidget() &&
          agentFrame->geometry().right() <= agentFrame->parentWidget()->width(),
      "agent heading text shares one baseline and the card remains within the "
      "available Inspector width");
  result &= expect(
      agentFrame && agentFrame->layout() && agentResult &&
          resultTop - headingBottom <= agentFrame->layout()->spacing() &&
          agentResult->alignment().testFlag(Qt::AlignTop) &&
          resultHeightForWidth >= 0 &&
          agentResult->height() >= resultHeightForWidth - 1 &&
          agentResult->height() <= resultHeightForWidth + 1,
      "long agent Markdown follows visible metadata without surplus height");
  const auto hasNativeBlackFrame = [](QScrollBar *scrollBar) {
    if (!scrollBar)
      return true;
    const QImage rendered = scrollBar->grab().toImage();
    for (int y = 0; y < rendered.height(); ++y) {
      for (int x = 0; x < rendered.width(); ++x) {
        const QColor pixel = rendered.pixelColor(x, y);
        if (pixel.red() < 16 && pixel.green() < 16 && pixel.blue() < 16)
          return true;
      }
    }
    return false;
  };
  auto *agentsScroll = qobject_cast<QScrollArea *>(inspector.tabs()->widget(1));
  QScrollBar *agentsScrollBar =
      agentsScroll ? agentsScroll->verticalScrollBar() : nullptr;
  if (agentsScroll)
    agentsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  spin(20);
  result &= expect(agentsScrollBar && agentsScrollBar->isVisible() &&
                       agentsScrollBar->width() == 8 &&
                       !hasNativeBlackFrame(agentsScrollBar),
                   "a visible Inspector scrollbar renders with the canonical "
                   "frameless style");
  auto *compactDiff =
      inspector.findChild<QPlainTextEdit *>(QStringLiteral("codexDiffText"));
  QStringList diffLines;
  for (int line = 0; line < 80; ++line)
    diffLines << QStringLiteral("+%1 a deliberately long changed line for "
                                "scrollbar verification")
                     .arg(line);
  inspector.tabs()->setCurrentIndex(2);
  spin(20);
  if (compactDiff)
    compactDiff->setPlainText(diffLines.join(QLatin1Char('\n')));
  spin(20);
  QScrollBar *diffVertical =
      compactDiff ? compactDiff->verticalScrollBar() : nullptr;
  QScrollBar *diffHorizontal =
      compactDiff ? compactDiff->horizontalScrollBar() : nullptr;
  result &= expect(
      diffVertical && diffHorizontal && diffVertical->isVisible() &&
          diffHorizontal->isVisible() && diffVertical->width() == 8 &&
          diffHorizontal->height() == 8 && !hasNativeBlackFrame(diffVertical) &&
          !hasNativeBlackFrame(diffHorizontal),
      "Changes preview scrollbars retain overview rendering without native "
      "frames");
  inspector.tabs()->setCurrentIndex(3);
  spin(20);
  result &=
      expect(hasLabelContaining(inspector, QStringLiteral("User input")) &&
                 hasLabelContaining(inspector,
                                    QStringLiteral("thread Original title")) &&
                 hasLabelContaining(inspector, QStringLiteral("3 questions")),
             "Requests show their thread title and retained question count");
  {
    auto write = graph.write();
    write.setField(ownerThread, "name", nodegraph::Value("Renamed title"));
    static_cast<void>(write.finish());
  }
  refresh(inspector, graph, "owner-thread");
  spin(20);
  result &= expect(
      hasLabelContaining(inspector, QStringLiteral("thread Renamed title")) &&
          !hasLabelContaining(inspector,
                              QStringLiteral("thread Original title")),
      "Requests update their thread label after a thread rename");
  QFrame *requestFrame = nullptr;
  for (QFrame *frame : inspector.findChildren<QFrame *>()) {
    if (frame->property("tone") == "warning") {
      requestFrame = frame;
      break;
    }
  }
  QPushButton *rejectButton = nullptr;
  QPushButton *reviewButton = nullptr;
  for (QPushButton *button : inspector.findChildren<QPushButton *>()) {
    if (button->text() == QStringLiteral("Reject"))
      rejectButton = button;
    else if (button->text() == QStringLiteral("Review"))
      reviewButton = button;
  }
  result &= expect(
      requestFrame && rejectButton && reviewButton &&
          rejectButton->property("kind") == "destructive" &&
          reviewButton->property("kind") == "request",
      "complex pending requests use warning surfaces and a review action");
  {
    auto write = graph.write();
    addInteraction(
        write, runtime, ownerThread, "request-two", "command-approval",
        {{"command", nodegraph::Value("gh auth status")},
         {"reason", nodegraph::Value("Verify GitHub authentication")},
         {"cwd", nodegraph::Value("/home/voc/projects/drafts")}});
    static_cast<void>(write.finish());
  }
  refresh(inspector, graph, "owner-thread");
  spin(20);
  QPushButton *acceptButton = nullptr;
  for (QPushButton *button : inspector.findChildren<QPushButton *>()) {
    if (button->text() == QStringLiteral("Accept")) {
      acceptButton = button;
      break;
    }
  }
  result &= expect(
      acceptButton &&
          acceptButton->property("kind").toString() ==
              QStringLiteral("request") &&
          hasLabelContaining(inspector,
                             QStringLiteral("Command: gh auth status")) &&
          hasLabelContaining(
              inspector,
              QStringLiteral("Reason: Verify GitHub authentication")),
      "simple approval requests show decision details and direct accept");
  qApp->setStyleSheet(previousStyleSheet);
  return result;
}

bool testTerminalPlanStatusReconciliation() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  {
    auto write = graph.write();
    thread = addThread(write, "plan-thread", {}, "active");
    nodegraph::NodeState turnState;
    turnState.status = nodegraph::NodeStatus::Running;
    turnState.fields = {
        {"status", nodegraph::Value("inProgress")},
        {"planExplanation",
         nodegraph::Value("Lifecycle [plan](https://example.com)")},
        {"plan", nodegraph::Value(nodegraph::Value::Array{
                     nodegraph::Value(nodegraph::Value::Object{
                         {"step", nodegraph::Value("Active step")},
                         {"status", nodegraph::Value("inProgress")}}),
                     nodegraph::Value(nodegraph::Value::Object{
                         {"step", nodegraph::Value("Pending step")},
                         {"status", nodegraph::Value("pending")}})})}};
    const nodegraph::NodeRef turn = write.upsert(
        {nodegraph::NodeKind::Turn, "plan-turn"}, std::move(turnState));
    write.setParent(thread, turn);
    const std::array roots{thread};
    static_cast<void>(runtimeWithRoots(write, roots));
    static_cast<void>(write.finish());
  }

  InspectorPane inspector;
  refresh(inspector, graph, "plan-thread");
  const auto hasExactLabel = [&inspector](const QString &value) {
    return std::ranges::any_of(
        inspector.findChildren<QLabel *>(),
        [&value](const QLabel *label) { return label->text() == value; });
  };
  bool result = expect(hasExactLabel(QStringLiteral("running")) &&
                           hasExactLabel(QStringLiteral("pending")),
                       "active plans preserve running and pending statuses");
  const auto markdownLabels = inspector.findChildren<QLabel *>();
  result &=
      expect(std::ranges::any_of(
                 markdownLabels,
                 [](const QLabel *label) {
                   return label->textFormat() == Qt::RichText &&
                          label->text().contains(QStringLiteral("href=")) &&
                          label->textInteractionFlags().testFlag(
                              Qt::LinksAccessibleByKeyboard);
                 }),
             "Inspector Markdown links are keyboard accessible");

  const auto setThreadStatus = [&](nodegraph::NodeStatus nodeStatus,
                                   const char *status) {
    auto write = graph.write();
    write.setStatus(thread, nodeStatus);
    write.setField(thread, "status", nodegraph::Value(status));
    static_cast<void>(write.finish());
    refresh(inspector, graph, "plan-thread");
  };
  setThreadStatus(nodegraph::NodeStatus::Completed, "completed");
  result &= expect(!hasExactLabel(QStringLiteral("running")) &&
                       hasExactLabel(QStringLiteral("completed")) &&
                       hasExactLabel(QStringLiteral("pending")),
                   "a terminal thread reconciles stale running to completed "
                   "without changing pending");
  setThreadStatus(nodegraph::NodeStatus::Failed, "failed");
  result &= expect(hasExactLabel(QStringLiteral("failed")) &&
                       hasExactLabel(QStringLiteral("pending")),
                   "a failed thread reconciles stale running to failed");
  setThreadStatus(nodegraph::NodeStatus::Interrupted, "interrupted");
  result &=
      expect(hasExactLabel(QStringLiteral("interrupted")) &&
                 hasExactLabel(QStringLiteral("pending")),
             "an interrupted thread reconciles stale running to interrupted");
  return result;
}

bool testGitDiffScopes() {
  QTemporaryDir repositoryDirectory;
  if (!expect(repositoryDirectory.isValid(),
              "Git diff test creates a temporary workspace"))
    return false;
  GitDiffProvider provider;
  git_repository *repository = nullptr;
  if (!expect(git_repository_init(
                  &repository, repositoryDirectory.path().toUtf8().constData(),
                  0) == 0,
              "Git diff test initializes an in-process repository"))
    return false;
  QFile file(repositoryDirectory.filePath(QStringLiteral("notes.txt")));
  if (!expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "Git diff test creates an untracked file")) {
    git_repository_free(repository);
    return false;
  }
  file.write("first line\nsecond line\n");
  file.close();

  GitDiffSnapshot received;
  bool ready = false;
  QObject::connect(&provider, &GitDiffProvider::snapshotReady,
                   [&received, &ready](const GitDiffSnapshot &snapshot) {
                     received = snapshot;
                     ready = true;
                   });
  const auto request =
      [&](const QString &workspace, const QStringList &directories,
          const QStringList &paths, const QString &selectedRepository,
          GitDiffScope scope, bool includeHiddenRepositories = false) {
        ready = false;
        provider.request(workspace, directories, paths, selectedRepository,
                         includeHiddenRepositories, scope,
                         GitDiffContext::Compact);
        QElapsedTimer timeout;
        timeout.start();
        while (!ready && timeout.elapsed() < 3000)
          spin(1);
        return ready;
      };

  bool result = expect(
      request(repositoryDirectory.path(), {}, {}, {}, GitDiffScope::Unstaged) &&
          received.repository && received.error.isEmpty() &&
          received.files.size() == 1 &&
          received.files.front().status == QStringLiteral("Untracked") &&
          received.files.front().patch.contains(QStringLiteral("+first line")),
      "Unstaged scope includes untracked file content");

  git_index *index = nullptr;
  if (git_repository_index(&index, repository) == 0) {
    git_index_add_bypath(index, "notes.txt");
    git_index_write(index);
    git_index_free(index);
  }
  result &= expect(
      request(repositoryDirectory.path(), {}, {}, {}, GitDiffScope::Staged) &&
          received.files.size() == 1 &&
          received.files.front().status == QStringLiteral("Added"),
      "Staged scope compares the index with HEAD");
  result &= expect(
      request(repositoryDirectory.path(), {}, {}, {},
              GitDiffScope::Uncommitted) &&
          received.files.size() == 1 &&
          received.files.front().patch.contains(QStringLiteral("+second line")),
      "Since-HEAD scope combines index and worktree state");

  QTemporaryDir ordinaryDirectory;
  result &=
      expect(ordinaryDirectory.isValid() &&
                 request(ordinaryDirectory.path(), {}, {}, {},
                         GitDiffScope::Unstaged) &&
                 !received.repository &&
                 received.error.contains(QStringLiteral("Git repository")),
             "ordinary folders expose an explicit non-repository state");

  QTemporaryDir multiWorkspace;
  const QString firstRoot = multiWorkspace.filePath(QStringLiteral("first"));
  const QString secondRoot = multiWorkspace.filePath(QStringLiteral("second"));
  const QString hiddenRoot =
      multiWorkspace.filePath(QStringLiteral(".hidden/repository"));
  git_repository *firstRepository = nullptr;
  git_repository *secondRepository = nullptr;
  git_repository *hiddenRepository = nullptr;
  git_repository_init(&firstRepository, firstRoot.toUtf8().constData(), 0);
  git_repository_init(&secondRepository, secondRoot.toUtf8().constData(), 0);
  QDir().mkpath(hiddenRoot);
  git_repository_init(&hiddenRepository, hiddenRoot.toUtf8().constData(), 0);
  for (const QString &root : {firstRoot, secondRoot, hiddenRoot}) {
    QFile shared(QDir(root).filePath(QStringLiteral("shared.txt")));
    if (shared.open(QIODevice::WriteOnly | QIODevice::Truncate))
      shared.write("shared path\n");
  }
  QFile firstOnly(QDir(firstRoot).filePath(QStringLiteral("first-only.txt")));
  if (firstOnly.open(QIODevice::WriteOnly | QIODevice::Truncate))
    firstOnly.write("first repository\n");
  firstOnly.close();
  result &= expect(
      request(multiWorkspace.path(),
              {firstRoot, firstRoot, hiddenRoot, secondRoot},
              {QStringLiteral("shared.txt")}, {}, GitDiffScope::Unstaged) &&
          received.repositoryRoots.size() == 2 && received.files.size() == 3 &&
          !received.repositoryRoots.contains(QDir::cleanPath(hiddenRoot)),
      "duplicate directories are deduplicated, hidden roots are excluded, and "
      "ambiguous paths retain visible matches");
  result &= expect(
      request(multiWorkspace.path(), {firstRoot, secondRoot, hiddenRoot},
              {QStringLiteral("shared.txt")}, {}, GitDiffScope::Unstaged,
              true) &&
          received.repositoryRoots.size() == 3 && received.files.size() == 4 &&
          received.repositoryRoots.contains(QDir::cleanPath(hiddenRoot)),
      "the explicit hidden-repository option includes hidden candidates");
  result &= expect(
      request(multiWorkspace.path(), {firstRoot, secondRoot},
              {QStringLiteral("shared.txt")}, firstRoot,
              GitDiffScope::Unstaged) &&
          received.repositoryRoots.size() == 2 && received.files.size() == 2 &&
          received.files.front().repositoryRoot == QDir::cleanPath(firstRoot),
      "repository selection filters files without losing the candidate set");
  result &= expect(
      request(multiWorkspace.path(), {firstRoot, secondRoot},
              {QStringLiteral("first-only.txt")}, {}, GitDiffScope::Unstaged) &&
          received.repositoryRoots == QStringList{QDir::cleanPath(firstRoot)} &&
          received.files.size() == 2,
      "a unique relative path resolves one repository and includes all of its "
      "changes");
  result &=
      expect(request(multiWorkspace.path(), {firstRoot, secondRoot},
                     {QDir(secondRoot).filePath(QStringLiteral("shared.txt"))},
                     {}, GitDiffScope::Unstaged) &&
                 received.repositoryRoots ==
                     QStringList{QDir::cleanPath(secondRoot)} &&
                 received.files.size() == 1,
             "an absolute path resolves only its owning repository");
  result &= expect(
      request(multiWorkspace.path(), {firstRoot, secondRoot},
              {QStringLiteral("not-applied-yet.txt")},
              QStringLiteral("/stale/repository"), GitDiffScope::Unstaged) &&
          received.repositoryRoots.size() == 2 && received.files.size() == 3,
      "an unmatched early path and stale selection safely fall back to all "
      "candidate repositories");
  const QString priorityPath = QStringLiteral("priority.txt");
  QFile firstPriority(QDir(firstRoot).filePath(priorityPath));
  QFile secondPriority(QDir(secondRoot).filePath(priorityPath));
  const bool priorityFiles =
      firstPriority.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
      firstPriority.write("baseline\n") > 0;
  firstPriority.close();
  const bool secondPriorityFile =
      secondPriority.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
      secondPriority.write("baseline\n") > 0;
  secondPriority.close();
  const bool priorityCommitted = priorityFiles && secondPriorityFile &&
                                 commitPath(firstRepository, "priority.txt") &&
                                 commitPath(secondRepository, "priority.txt");
  if (firstPriority.open(QIODevice::WriteOnly | QIODevice::Truncate))
    firstPriority.write("changed\n");
  firstPriority.close();
  result &= expect(
      priorityCommitted &&
          request(multiWorkspace.path(), {firstRoot, secondRoot},
                  {priorityPath}, {}, GitDiffScope::Unstaged) &&
          received.repositoryRoots == QStringList{QDir::cleanPath(firstRoot)} &&
          received.files.size() == 3 &&
          std::any_of(received.files.begin(), received.files.end(),
                      [&](const GitDiffFile &file) {
                        return file.path == priorityPath &&
                               file.status == QStringLiteral("Modified");
                      }),
      "a currently changed path is preferred over the same clean tracked path");
  const QString secondCleanPath = QStringLiteral("second-clean.txt");
  QFile secondClean(QDir(secondRoot).filePath(secondCleanPath));
  const bool secondCleanCreated =
      secondClean.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
      secondClean.write("clean unique path\n") > 0;
  secondClean.close();
  result &= expect(
      secondCleanCreated && commitPath(secondRepository, "second-clean.txt") &&
          request(multiWorkspace.path(), {firstRoot, secondRoot},
                  {priorityPath, secondCleanPath}, {},
                  GitDiffScope::Unstaged) &&
          received.repositoryRoots.size() == 2 && received.files.size() == 4,
      "changed-file preference is applied independently for every hinted path");
  QFile::remove(QDir(firstRoot).filePath(priorityPath));
  result &= expect(
      request(multiWorkspace.path(), {firstRoot, secondRoot}, {priorityPath},
              {}, GitDiffScope::Unstaged) &&
          received.repositoryRoots == QStringList{QDir::cleanPath(firstRoot)} &&
          std::any_of(received.files.begin(), received.files.end(),
                      [&](const GitDiffFile &file) {
                        return file.path == priorityPath &&
                               file.status == QStringLiteral("Deleted");
                      }),
      "a deleted path is resolved from Git state and preferred over a clean "
      "tracked match");
  git_repository_free(firstRepository);
  git_repository_free(secondRepository);
  git_repository_free(hiddenRepository);
  git_repository_free(repository);
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  QTemporaryDir settingsDirectory;
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     settingsDirectory.path());
  using namespace codexui::codex::middle;
  bool result = testPromptKeyboardSubmission();
  result &= testOverlayGeometryAndRegionRouting();
  result &= testThreadSelectionProjection();
  result &= testThreadRuntimeStatusColors();
  result &= testThreadHierarchyExpansionAndNavigation();
  result &= testIncrementalThreadSettings();
  result &= testThreadAlphanumericSort();
  result &= testThreadCreatedSort();
  result &= testThreadLastChangedSort();
  result &= testThreadRecencySort();
  result &= testThreadLastActivityRetention();
  result &= testPromptActivityNaturallyOrdersThreads();
  result &= testOptimisticThreadRowLifecycle();
  result &= testThreadRowReorderOwnership();
  result &= testThreadPaneDirectGraphBinding();
  result &= testLargeThreadTopologyKeepsQtHeartbeatAlive();
  result &= testLargeThreadScanReplacesAnObsoleteRevision();
  result &= testThreadScanCompletesUnderContinuousGraphChanges();
  result &= testThreadRelationChangeDuringValidationRestartsCandidate();
  result &= testRemovalBeforeMaterializationReleasesSelection();
  result &= testDestroyThreadPaneWithQueuedTopologyPasses();
  result &= testNestedCommandScrollOwnership();
  result &= testInfoViewerLayout();
  result &= testInspectorDetailParity();
  result &= testTerminalPlanStatusReconciliation();
  result &= testGitDiffScopes();
  result &= testStableComposerLayoutRequests();
  if (result)
    std::cout << "Application layout tests passed\n";
  return result ? 0 : 1;
}
