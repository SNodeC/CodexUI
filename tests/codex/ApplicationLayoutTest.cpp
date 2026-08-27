// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/GitDiffProvider.h"
#include "codex/PresentationModel.h"
#include "codex/PresentationProtocol.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFrame>
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
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QWheelEvent>

#include <git2.h>

#include <iostream>
#include <optional>
#include <string>
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

bool commitPath(git_repository *repository, const char *path) {
  git_index *index = nullptr;
  if (git_repository_index(&index, repository) < 0)
    return false;
  const bool indexed = git_index_add_bypath(index, path) == 0 &&
                       git_index_write(index) == 0;
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

  auto *threadHeaderDivider =
      splitter->widget(0)->findChild<QFrame *>(
          QStringLiteral("threadHeaderDivider"));
  auto *conversationHeaderDivider =
      splitter->widget(1)->findChild<QFrame *>(
          QStringLiteral("conversationHeaderDivider"));
  auto *conversationTitle =
      splitter->widget(1)->findChild<QLabel *>(
          QStringLiteral("conversationTitle"));
  auto *conversationMetadata =
      splitter->widget(1)->findChild<QLabel *>(
          QStringLiteral("conversationMetadata"));
  const auto paneRect = [](QWidget *widget, QWidget *pane) {
    return QRect(widget->mapTo(pane, QPoint()), widget->size());
  };
  const QRect threadDividerRect =
      threadHeaderDivider
          ? paneRect(threadHeaderDivider, splitter->widget(0))
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
          conversationDividerRect.right() ==
              splitter->widget(1)->width() - 11,
      "Threads and Conversation header dividers share the 10 px inset");
  result &= expect(
      conversationTitle && conversationMetadata &&
          conversationMetadata->geometry().left() >
              conversationTitle->geometry().right() &&
          std::abs(conversationMetadata->geometry().bottom() -
                   conversationTitle->geometry().bottom()) <= 1,
      "thread title and metadata form one baseline-aligned lockup");

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
  const auto overlayRect = [&](QWidget *widget) {
    return QRect(widget->mapTo(&region.composer(), QPoint()), widget->size());
  };
  const auto settingsToComposerGap = [&] {
    return composerSurface ? overlayRect(composerSurface).top() -
                                 overlayRect(settings).bottom() - 1
                           : -1;
  };
  const auto settingsToEditorGap = [&] {
    return region.composer().promptEditor()->mapTo(&region.composer(), QPoint())
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
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  result &= expect(
      stableComposerGeometry() &&
          settingsToEditorGap() == compactEditorGap &&
          view.viewport()->height() - finalCardBottom() == extra &&
          view.geometry() == viewGeometry &&
          view.viewport()->geometry() == viewportGeometry &&
          region.composer().canonicalReserve()->height() == canonical,
      "prompt growth keeps gaps fixed without shifting the message viewport");
  region.composer().setAttachments(
      {{QStringLiteral("/tmp/layout-diagnostic.png"),
        QStringLiteral("layout-diagnostic.png"), QStringLiteral("image/png")}});
  spin(30);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  result &= expect(
      stableComposerGeometry() &&
          region.composer().extraOverlayHeight() > extra &&
          view.viewport()->height() - finalCardBottom() ==
              region.composer().extraOverlayHeight() &&
          view.trailingSpaceHeight() == region.composer().extraOverlayHeight(),
      "attachments retain the canonical settings-to-composer gap");
  region.composer().clearDraft();
  spin(30);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  spin(10);
  result &= expect(
      region.composer().extraOverlayHeight() == 0 &&
          view.trailingSpaceHeight() == 0 && view.geometry() == viewGeometry &&
          view.viewport()->geometry() == viewportGeometry &&
          finalCardBottom() == view.viewport()->height() &&
          stableComposerGeometry() &&
          settingsToEditorGap() == compactEditorGap,
      "prompt contraction restores canonical layout, gaps, and trailing space");
  region.composer().setActiveTurn(false);
  spin(20);

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
    result = expect(
        composerLayoutRequests.count <= 1,
        "stable composer geometry does not perpetually request layout");
  }
  qApp->setStyleSheet(QString{});
  return result;
}

bool testThreadSelectionProjection() {
  PresentationModel model;
  model.applyEvent(presentation::event(
      1, 1, "thread.upsert", {{"thread", {{"id", "thread-a"}, {"name", "A"}}}},
      presentation::Authority::Merge, {{"threadId", "thread-a"}}));
  model.applyEvent(presentation::event(
      2, 1, "thread.upsert",
      {{"thread", {{"id", "thread-b"},
                    {"name", "B"},
                    {"status", {{"type", "active"}}}}}},
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
          dynamic_cast<UiStyle::ChevronToolButton *>(sortButton) &&
          sortButton->property("codexChevron").toBool() &&
          title->property("kind").toString() == QStringLiteral("title") &&
          status->property("kind").toString() == QStringLiteral("meta") &&
          status->property("tone").toString() == QStringLiteral("active") &&
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

bool testIncrementalThreadSettings() {
  TurnSettingsWidget settings;
  const nlohmann::json models =
      nlohmann::json::array({{{"model", "gpt-a"}, {"displayName", "A"}},
                             {{"model", "gpt-b"}, {"displayName", "B"}}});
  settings.setContext("thread-a",
                      {{"model", "gpt-a"}, {"approvalPolicy", "never"}},
                      models, nlohmann::json::array());
  auto *model = settings.findChild<QComboBox *>(QStringLiteral("codexModel"));
  auto *approval =
      settings.findChild<QComboBox *>(QStringLiteral("codexApproval"));
  auto *access =
      settings.findChild<QComboBox *>(QStringLiteral("codexSandbox"));
  auto *network =
      settings.findChild<QComboBox *>(QStringLiteral("codexNetwork"));
  if (!model || !approval || !access || !network)
    return expect(false, "thread settings controls are discoverable");

  model->setCurrentIndex(model->findData(QStringLiteral("gpt-b")));
  settings.setContext(
      "thread-a",
      {{"model", "gpt-a"}, {"approvalPolicy", "on-request"}}, models,
      nlohmann::json::array(), 1, {{"approvalPolicy", "on-request"}});
  bool result = expect(
      model->currentData().toString() == QStringLiteral("gpt-b") &&
          approval->currentData().toString() == QStringLiteral("on-request"),
      "a partial authoritative update preserves unrelated pending settings");

  settings.setContext(
      "thread-a",
      {{"model", "gpt-b"}, {"approvalPolicy", "on-request"}}, models,
      nlohmann::json::array(), 2,
      {{"model", "gpt-b"}, {"approvalPolicy", "on-request"}});
  result &= expect(!settings.turnStartOptions().contains("model") &&
                       !settings.turnStartOptions().contains("approvalPolicy"),
                   "authoritative settings clear their pending overrides");

  settings.setContext(
      "thread-b",
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
  result &= expect(settings.turnStartOptions().empty() &&
                       settings.threadStartOptions().empty(),
                   "all untouched thread settings produce no overrides");
  result &= expect(access->isEnabled() && network->isEnabled(),
                   "a permission preset does not lock its effective access "
                   "controls");

  settings.setContext(
      "full-access-thread",
      {{"sandboxPolicy", {{"type", "dangerFullAccess"}}},
       {"activePermissionProfile", {{"id", ":full-access"}}}},
      nlohmann::json::array(),
      {{"data", nlohmann::json::array(
                    {{{"id", ":full-access"}, {"allowed", true}}})}});
  result &= expect(access->isEnabled() && !network->isEnabled() &&
                       network->currentData().toString() ==
                           QStringLiteral("enabled"),
                   "only logically redundant network selection is disabled");

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
  QPointer<QWidget> stableThreadARow = list->itemWidget(threadA);
  QPointer<QWidget> originalRow = list->itemWidget(threadB);

  model.applyEvent(presentation::event(
      3, 1, "thread.status.changed", {{"status", "completed"}},
      presentation::Authority::Merge, {{"threadId", "thread-b"}}));
  pane.refresh(model, "thread-a");
  result &= expect(stableThreadARow == list->itemWidget(threadA) &&
                       originalRow == list->itemWidget(threadB),
                   "content-only refreshes preserve thread row widgets");

  model.applyEvent(presentation::result(
      4, 1, "threads.list", "reordered-threads", true,
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
  QWheelEvent boundary = wheelFor(commandOutput, 120);
  result &= expect(!region.routeScrollEvent(commandOutput, &boundary) &&
                       region.conversation().verticalScrollBar()->value() ==
                           outerBefore,
                   "nested output retains input at its scroll boundary");
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
      1, 1, "thread.upsert",
      {{"thread", {{"id", "owner-thread"}, {"name", "Original title"}}}},
      presentation::Authority::Merge, {{"threadId", "owner-thread"}}));
  model.applyEvent(presentation::event(
      2, 1, "agents.activity.upsert",
      {{"activity",
        {{"id", "agent-one"},
         {"type", "subAgentActivity"},
         {"status", "inProgress"},
         {"agentThreadId", "child-thread"},
         {"resultText",
          "Agent result summary.\n\n* First rendered finding.\n* Second "
          "rendered finding."},
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
  QLabel *agentStatus = nullptr;
  for (QLabel *label : inspector.findChildren<QLabel *>()) {
    if (label->text() == QStringLiteral("Running")) {
      agentStatus = label;
      break;
    }
  }
  result &= expect(agentStatus && agentStatus->property("tone") == "active",
                   "running agent status uses the canonical active tone");
  auto *agentResult =
      inspector.findChild<QLabel *>(QStringLiteral("agentResult"));
  result &= expect(
      agentResult && agentResult->alignment().testFlag(Qt::AlignTop),
      "agent Markdown starts at the top of any surplus result-label height");
  inspector.tabs()->setCurrentIndex(3);
  spin(20);
  result &= expect(
      hasLabelContaining(inspector, QStringLiteral("thread Original title")) &&
          hasLabelContaining(inspector, QStringLiteral("3 questions")),
      "Requests show their thread title and retained question count");
  model.applyEvent(presentation::event(
      4, 1, "thread.name.changed", {{"name", "Renamed title"}},
      presentation::Authority::Replace, {{"threadId", "owner-thread"}}));
  inspector.refresh(model, "owner-thread");
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
  QPushButton *denyButton = nullptr;
  QPushButton *reviewButton = nullptr;
  for (QPushButton *button : inspector.findChildren<QPushButton *>()) {
    if (button->text() == QStringLiteral("Deny"))
      denyButton = button;
    else if (button->text() == QStringLiteral("Review"))
      reviewButton = button;
  }
  result &= expect(
      requestFrame && denyButton && reviewButton &&
          denyButton->property("kind") == "destructive" &&
          reviewButton->property("kind") == "request",
      "pending requests use warning surfaces and semantic actions");
  return result;
}

bool testGitDiffScopes() {
  QTemporaryDir repositoryDirectory;
  if (!expect(repositoryDirectory.isValid(),
              "Git diff test creates a temporary workspace"))
    return false;
  GitDiffProvider provider;
  git_repository *repository = nullptr;
  if (!expect(git_repository_init(&repository,
                                  repositoryDirectory.path().toUtf8().constData(),
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
  const auto request = [&](const QString &workspace,
                           const QStringList &directories,
                           const QStringList &paths,
                           const QString &selectedRepository,
                           GitDiffScope scope,
                           bool includeHiddenRepositories = false) {
    ready = false;
    provider.request(workspace, directories, paths, selectedRepository,
                     includeHiddenRepositories, scope, GitDiffContext::Compact);
    QElapsedTimer timeout;
    timeout.start();
    while (!ready && timeout.elapsed() < 3000)
      spin(1);
    return ready;
  };

  bool result = expect(request(repositoryDirectory.path(), {}, {}, {},
                              GitDiffScope::Unstaged) &&
                           received.repository && received.error.isEmpty() &&
                           received.files.size() == 1 &&
                           received.files.front().status ==
                               QStringLiteral("Untracked") &&
                           received.files.front().patch.contains(
                               QStringLiteral("+first line")),
                       "Unstaged scope includes untracked file content");

  git_index *index = nullptr;
  if (git_repository_index(&index, repository) == 0) {
    git_index_add_bypath(index, "notes.txt");
    git_index_write(index);
    git_index_free(index);
  }
  result &= expect(request(repositoryDirectory.path(), {}, {}, {},
                           GitDiffScope::Staged) &&
                       received.files.size() == 1 &&
                       received.files.front().status ==
                           QStringLiteral("Added"),
                   "Staged scope compares the index with HEAD");
  result &= expect(request(repositoryDirectory.path(), {}, {}, {},
                           GitDiffScope::Uncommitted) &&
                       received.files.size() == 1 &&
                       received.files.front().patch.contains(
                           QStringLiteral("+second line")),
                   "Since-HEAD scope combines index and worktree state");

  QTemporaryDir ordinaryDirectory;
  result &= expect(ordinaryDirectory.isValid() &&
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
      "duplicate directories are deduplicated, hidden roots are excluded, and ambiguous paths retain visible matches");
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
              {QStringLiteral("first-only.txt")}, {},
              GitDiffScope::Unstaged) &&
          received.repositoryRoots == QStringList{QDir::cleanPath(firstRoot)} &&
          received.files.size() == 2,
      "a unique relative path resolves one repository and includes all of its changes");
  result &= expect(
      request(multiWorkspace.path(), {firstRoot, secondRoot},
              {QDir(secondRoot).filePath(QStringLiteral("shared.txt"))}, {},
              GitDiffScope::Unstaged) &&
          received.repositoryRoots ==
              QStringList{QDir::cleanPath(secondRoot)} &&
          received.files.size() == 1,
      "an absolute path resolves only its owning repository");
  result &= expect(
      request(multiWorkspace.path(), {firstRoot, secondRoot},
              {QStringLiteral("not-applied-yet.txt")},
              QStringLiteral("/stale/repository"), GitDiffScope::Unstaged) &&
          received.repositoryRoots.size() == 2 && received.files.size() == 3,
      "an unmatched early path and stale selection safely fall back to all candidate repositories");
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
  const bool priorityCommitted =
      priorityFiles && secondPriorityFile &&
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
      "a deleted path is resolved from Git state and preferred over a clean tracked match");
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
  using namespace codexui::codex::middle;
  bool result = testOverlayGeometryAndRegionRouting();
  result &= testThreadSelectionProjection();
  result &= testIncrementalThreadSettings();
  result &= testThreadAlphanumericSort();
  result &= testThreadCreatedSort();
  result &= testThreadLastChangedSort();
  result &= testThreadRecencySort();
  result &= testThreadRowReorderOwnership();
  result &= testNestedCommandScrollOwnership();
  result &= testInfoViewerLayout();
  result &= testInspectorDetailParity();
  result &= testGitDiffScopes();
  result &= testStableComposerLayoutRequests();
  if (result)
    std::cout << "Application layout tests passed\n";
  return result ? 0 : 1;
}
