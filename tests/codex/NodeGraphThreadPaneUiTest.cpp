// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ForkNaming.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/NodeGraphUiAdapter.h"

#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>

#include <cstdlib>
#include <iostream>

namespace codexui::codex {
namespace {

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << message << '\n';
  return false;
}

bool forkNamesPreserveTheirLineage() {
  const std::vector<std::string> titles{
      "Original", "Original (fork 1)", "Original (fork 2)",
      "Original (fork 1.1)", "Original (fork 1.3)",
      "Original (fork 1.1.1)"};
  return require(suggestForkName("Original", titles) ==
                     "Original (fork 3)",
                 "a repeated root fork did not use the next root number") &&
         require(suggestForkName("Original (fork 1)", titles) ==
                     "Original (fork 1.2)",
                 "a fork of a fork lost its parent lineage") &&
         require(suggestForkName("Original (fork 1.1)", titles) ==
                     "Original (fork 1.1.2)",
                 "a nested fork did not use its own next child number") &&
         require(suggestForkName("Separate", titles) ==
                     "Separate (fork 1)",
                 "an unrelated thread did not start at fork 1");
}

bool selectedChildRetainsRootAndCanonicalIdentity() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef root;
  nodegraph::NodeRef child;
  {
    auto write = graph.write();
    nodegraph::NodeRef runtime =
        write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
    nodegraph::NodeState rootState;
    rootState.fields.emplace("name", "Root");
    root = write.upsert({nodegraph::NodeKind::Thread, "root"},
                        std::move(rootState));
    nodegraph::NodeState childState;
    childState.fields.emplace("name", "Child");
    child = write.upsert({nodegraph::NodeKind::Thread, "child"},
                         std::move(childState));
    write.relate(runtime, nodegraph::RelationKind::RootThread, root);
    write.relate(root, nodegraph::RelationKind::AgentChildThread, child);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot = adapter.threads(child);
  if (!require(snapshot.has_value(), "thread adapter read failed"))
    return false;

  middle::ThreadPane pane;
  pane.resize(300, 500);
  std::string selected;
  middle::ThreadPane::Actions actions;
  actions.select = [&](const std::string &id) { selected = id; };
  pane.setActions(std::move(actions));
  pane.refresh(*snapshot);
  pane.show();
  QApplication::processEvents();

  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  if (!require(list != nullptr, "thread list widget missing") ||
      !require(list->count() == 2,
               "selected child did not retain its visible root"))
    return false;
  list->setCurrentRow(0);
  QApplication::processEvents();
  selected.clear();
  list->setCurrentRow(1);
  QApplication::processEvents();
  return require(selected == child->id().canonical,
                 "thread UI changed the canonical action identity") &&
         require(list->item(0)->data(Qt::UserRole).toString() ==
                     QStringLiteral("root"),
                 "selecting a child removed its root row");
}

bool sortingAndPromptAnimationAreFixed() {
  middle::ThreadPane pane;
  pane.resize(300, 500);
  ui::ThreadListSnapshot snapshot;
  ui::ThreadListRow older;
  older.id = "older";
  older.title = "20 tasks";
  older.createdAt = 500;
  older.updatedAt = 900;
  older.recencyAt = 10;
  ui::ThreadListRow recent;
  recent.id = "recent";
  recent.title = "Alpha";
  recent.createdAt = 1;
  recent.updatedAt = 2;
  recent.recencyAt = 30;
  ui::ThreadListRow missing;
  missing.id = "missing";
  missing.title = "2 tasks";
  snapshot.roots = {older, recent, missing};

  pane.refresh(snapshot);
  pane.show();
  QApplication::processEvents();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  auto *sortButton =
      pane.findChild<QToolButton *>(QStringLiteral("threadSortButton"));
  if (!require(list && list->count() == 3, "thread list missing") ||
      !require(sortButton && sortButton->menu(), "thread sort control missing") ||
      !require(list->item(0)->data(Qt::UserRole).toString() ==
                   QStringLiteral("recent"),
               "thread rows are not ordered by recencyAt newest first"))
    return false;
  if (!require(list->item(0)->toolTip().contains(
                   QStringLiteral("Recent turn:")) &&
                   list->item(0)->toolTip().contains(
                       QStringLiteral("Created:")),
               "thread hover omits provider recent-turn or creation time"))
    return false;

  QStringList sortLabels;
  for (QAction *action : sortButton->menu()->actions())
    sortLabels.push_back(action->text());
  if (!require(sortLabels == QStringList{QStringLiteral("Alphanumeric"),
                                         QStringLiteral("Created"),
                                         QStringLiteral("Recent")},
               "thread sort menu does not expose exactly the three contracts"))
    return false;

  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Alphanumeric);
  QApplication::processEvents();
  if (!require(list->item(0)->data(Qt::UserRole).toString() ==
                   QStringLiteral("missing"),
               "alphanumeric order is not numeric-aware") ||
      !require(list->item(1)->data(Qt::UserRole).toString() ==
                   QStringLiteral("older"),
               "alphanumeric order did not place 20 after 2") ||
      !require(list->item(2)->data(Qt::UserRole).toString() ==
                   QStringLiteral("recent"),
               "alphanumeric order did not place letters after numeric titles"))
    return false;

  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Created);
  QApplication::processEvents();
  if (!require(list->item(0)->data(Qt::UserRole).toString() ==
                   QStringLiteral("older"),
               "Created order is not newest first") ||
      !require(list->item(1)->data(Qt::UserRole).toString() ==
                   QStringLiteral("recent"),
               "Created order did not keep older dated rows before missing") ||
      !require(list->item(2)->data(Qt::UserRole).toString() ==
                   QStringLiteral("missing"),
               "Created order did not put missing timestamps last"))
    return false;

  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Recency);
  QApplication::processEvents();

  snapshot.roots[0].recencyAt = 31;
  snapshot.roots[0].awaitingPromptAcknowledgement = true;
  snapshot.roots[0].pendingPromptAdmittedAtMs =
      QDateTime::currentMSecsSinceEpoch() - 1500;
  pane.refresh(snapshot);
  QApplication::processEvents();
  auto *animation =
      pane.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  QWidget *row = list->itemWidget(list->item(0));
  auto *title =
      row ? row->findChild<QLabel *>(QStringLiteral("threadTitle")) : nullptr;
  if (!require(list->item(0)->data(Qt::UserRole).toString() ==
                   QStringLiteral("older"),
               "new turn admission did not promote its thread") ||
      !require(title && title->text() == QStringLiteral("20 tasks"),
               "promotion substituted a canonical ID for the chosen name") ||
      !require(animation && animation->isActive(),
               "thread-card animation did not follow the pending prompt"))
    return false;

  snapshot.roots[1].status = "active";
  pane.refresh(snapshot);
  QApplication::processEvents();
  if (!require(animation->isActive(),
               "unrelated thread traffic stopped pending prompt animation"))
    return false;

  snapshot.roots[0].awaitingPromptAcknowledgement = false;
  snapshot.roots[0].pendingPromptAdmittedAtMs.reset();
  pane.refresh(snapshot);
  QApplication::processEvents();
  row = list->itemWidget(list->item(0));
  title =
      row ? row->findChild<QLabel *>(QStringLiteral("threadTitle")) : nullptr;
  return require(!animation->isActive(),
                 "prompt acknowledgement did not stop thread-card animation") &&
         require(title && title->text() == QStringLiteral("20 tasks"),
                 "prompt acknowledgement changed the chosen name");
}

bool pagingFollowsTheVisibleListEnd() {
  middle::ThreadPane pane;
  pane.resize(300, 500);
  int loadMoreRequests = 0;
  middle::ThreadPane::Actions actions;
  actions.loadMore = [&] { ++loadMoreRequests; };
  pane.setActions(std::move(actions));

  ui::ThreadListSnapshot longSnapshot;
  for (int index = 0; index < 80; ++index) {
    ui::ThreadListRow row;
    row.id = "long-" + std::to_string(index);
    row.title = row.id;
    row.recencyAt = 1000 - index;
    longSnapshot.roots.push_back(std::move(row));
  }
  pane.refresh(longSnapshot);
  pane.show();
  QApplication::processEvents();
  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  if (!require(list && list->verticalScrollBar()->maximum() > 0,
               "long thread page did not produce a scroll range"))
    return false;
  loadMoreRequests = 0;
  list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
  QApplication::processEvents();
  return require(loadMoreRequests > 0,
                 "scrolling to the visible list end did not request the next "
                 "thread page");
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (!codexui::codex::forkNamesPreserveTheirLineage() ||
      !codexui::codex::selectedChildRetainsRootAndCanonicalIdentity() ||
      !codexui::codex::sortingAndPromptAnimationAreFixed() ||
      !codexui::codex::pagingFollowsTheVisibleListEnd())
    return EXIT_FAILURE;
  std::cout << "NodeGraph ThreadPane UI tests passed\n";
  return EXIT_SUCCESS;
}
