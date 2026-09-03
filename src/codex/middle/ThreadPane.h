// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_THREADPANE_H
#define CODEXUI_CODEX_MIDDLE_THREADPANE_H

#include "codex/nodegraph/Messages.h"
#include "codex/ui/UiViewState.h"

#include <QFrame>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QMenu;
class QToolButton;
class QTimer;

namespace codexui::codex {
namespace middle {

class ThreadPane final : public QFrame {
public:
  enum class SortCriterion { Alphanumeric, Created, LastChanged, Recency };

  struct Actions {
    std::function<void()> newThread;
    std::function<void()> refresh;
    std::function<void()> hide;
    std::function<void(const std::string &)> select;
    std::function<void(const std::string &)> reload;
    std::function<void(const std::string &)> rename;
    std::function<void(const std::string &)> fork;
    std::function<void(const std::string &)> toggleArchive;
    std::function<void(const std::string &)> remove;
  };

  // During the graph cutover row actions carry the already-pinned node.  A
  // configured NodeActions callback takes precedence over the legacy
  // canonical-id callback, so an operation is never dispatched twice.
  struct NodeActions {
    std::function<void(const nodegraph::NodeRef &)> select;
    std::function<void(const nodegraph::NodeRef &)> reload;
    std::function<void(const nodegraph::NodeRef &)> rename;
    std::function<void(const nodegraph::NodeRef &)> fork;
    std::function<void(const nodegraph::NodeRef &)> archive;
    std::function<void(const nodegraph::NodeRef &)> unarchive;
    std::function<void(const nodegraph::NodeRef &)> remove;
  };

  explicit ThreadPane(QWidget *parent = nullptr);
  ~ThreadPane() override;

  void setActions(Actions actions);
  void setNodeActions(NodeActions actions);
  void refresh(const ui::ThreadListSnapshot &snapshot);
  // This is the direct shared-graph entry point. The selected NodeRef is local
  // navigation state; protocol-derived row state continues to live only in
  // the graph.
  void refresh(nodegraph::NodeGraph &graph,
               nodegraph::NodeRef selectedThread = {});
  void graphChanged();
  // Removal detachment is synchronous: FrontendSession may acknowledge the
  // removed NodeRefs as soon as this handler returns.
  void graphChanged(const nodegraph::GraphChanged &change);
  void beginOptimisticThread(std::string id, std::string title,
                             std::string cwd);
  void promoteOptimisticThread(const std::string &draftId,
                               const std::string &authoritativeId);
  void confirmOptimisticThread(const std::string &threadId);
  void failOptimisticThread(const std::string &threadId);
  [[nodiscard]] bool isOptimisticThread(const std::string &threadId) const;
  void setSortCriterion(SortCriterion criterion);
  [[nodiscard]] SortCriterion currentSortCriterion() const noexcept;
  [[nodiscard]] std::string visiblySelectedThreadId() const;
  [[nodiscard]] nodegraph::NodeRef visiblySelectedThread() const;

private:
  struct GraphThreadItem;
  struct GraphTopology;
  struct GraphRowRender;
  struct RenderedThreadRow {
    std::string id;
    std::string title;
    std::string cwd;
    std::string status;
    std::optional<std::int64_t> lastActivityAt;
    std::string parentId;
    std::size_t pending = 0;
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = false;
    bool optimistic = false;
    bool optimisticFailed = false;

    bool operator==(const RenderedThreadRow &) const = default;
  };
  struct RenderedThreadList {
    std::string selectedThreadId;
    SortCriterion sortCriterion = SortCriterion::Recency;
    std::vector<RenderedThreadRow> rows;

    bool operator==(const RenderedThreadList &) const = default;
  };
  struct OptimisticThread {
    std::string id;
    std::string title;
    std::string cwd;
    bool failed = false;
  };
  void updateSortButton();
  void sortRootThreads(std::vector<ui::ThreadListRow> &rows) const;
  void appendVisibleThread(RenderedThreadList &snapshot,
                           const ui::ThreadListRow &thread,
                           const std::string &parentId, std::size_t depth,
                           std::unordered_set<std::string> &visited) const;
  void toggleExpanded(const std::string &threadId);
  void navigateHierarchy(int key);
  void setContextHighlight(const std::string &threadId, bool highlighted);
  void showContextMenu(const QPoint &position);
  void showGraphContextMenu(const QPoint &position);
  void leaveGraphMode();
  void scheduleGraphRefresh();
  void runGraphRefresh();
  void applyGraphTopology(GraphTopology topology);
  void scheduleVisibilityPass();
  void runVisibilityPass();
  void detachRemoved(const nodegraph::GraphChanged &change);
  void dematerialize(GraphThreadItem &item, bool deferred = true);
  void renderGraphRow(GraphThreadItem &item, const GraphRowRender &render);
  [[nodiscard]] GraphThreadItem *graphItem(const QListWidgetItem *item) const;

  std::optional<ui::ThreadListSnapshot> currentSnapshot;
  Actions actions;
  NodeActions nodeActions;
  SortCriterion sortCriterion = SortCriterion::Recency;
  QToolButton *sortButton = nullptr;
  QListWidget *list = nullptr;
  std::unordered_map<std::string, QListWidgetItem *> rows;
  std::unordered_set<std::string> expandedThreads;
  std::string projectedSelectedThreadId;
  std::string contextThreadId;
  QMenu *contextMenu = nullptr;
  QTimer *optimisticAnimation = nullptr;
  std::vector<OptimisticThread> optimisticThreads;
  std::optional<RenderedThreadList> visibleSnapshot;

  nodegraph::NodeGraph *graph = nullptr;
  nodegraph::NodeRef selectedGraphThread;
  std::unordered_set<const nodegraph::Node *> graphExpandedThreads;
  bool graphRefreshScheduled = false;
  bool visibilityPassScheduled = false;
  int visibilityFirst = -1;
  int visibilityLast = -1;
  int visibilityCursor = -1;
};

} // namespace middle
} // namespace codexui::codex

#endif
