// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_THREADPANE_H
#define CODEXUI_CODEX_MIDDLE_THREADPANE_H

#include "codex/nodegraph/Messages.h"

#include <QFrame>
#include <QPoint>

#include <cstddef>
#include <functional>
#include <memory>
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

  // These controls are local shell actions. Operations addressed to a
  // protocol thread always use NodeActions and its already-pinned NodeRef.
  struct Controls {
    std::function<void()> newThread;
    std::function<void()> refresh;
    std::function<void()> hide;
  };

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

  void setControls(Controls controls);
  void setNodeActions(NodeActions actions);
  void refresh(const nodegraph::NodeGraph &graph,
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
  struct GraphScan;
  struct GraphRowRender;
  struct OptimisticThread {
    std::string id;
    std::string title;
    std::string cwd;
    bool failed = false;
    std::string previousId;
  };
  void updateSortButton();
  void toggleExpanded(GraphThreadItem &thread);
  void navigateHierarchy(int key);
  void showContextMenu(const QPoint &position);
  void showContextMenu(const nodegraph::NodeRef &node,
                       std::optional<QPoint> requestedPosition);
  void leaveGraph();
  void scheduleGraphRefresh();
  void scheduleGraphScanPass();
  void runGraphRefresh();
  void applyGraphTopology(GraphTopology topology);
  void scheduleTopologyPass();
  void runTopologyPass();
  void scheduleVisibilityPass();
  void runVisibilityPass();
  void detachRemoved(const nodegraph::GraphChanged &change);
  void dematerialize(GraphThreadItem &item, bool deferred = true);
  void renderGraphRow(GraphThreadItem &item, const GraphRowRender &render);
  [[nodiscard]] GraphThreadItem *graphItem(const QListWidgetItem *item) const;
  [[nodiscard]] GraphThreadItem *
  attachedGraphItem(const nodegraph::NodeRef &node) const;

  Controls controls;
  NodeActions nodeActions;
  SortCriterion sortCriterion = SortCriterion::Recency;
  QToolButton *sortButton = nullptr;
  QListWidget *list = nullptr;
  std::string contextThreadId;
  QMenu *contextMenu = nullptr;
  QTimer *optimisticAnimation = nullptr;
  std::vector<OptimisticThread> optimisticThreads;
  std::vector<GraphThreadItem *> materializedItems;
  const nodegraph::NodeGraph *graph = nullptr;
  nodegraph::NodeRef selectedGraphThread;
  std::string selectedOptimisticThreadId;
  std::unordered_set<const nodegraph::Node *> graphExpandedThreads;
  bool revealSelectedGraphThread = false;
  bool graphRefreshScheduled = false;
  std::unique_ptr<GraphScan> pendingGraphScan;
  std::unique_ptr<GraphTopology> pendingGraphTopology;
  std::size_t topologyCursor = 0;
  int topologySearchCursor = -1;
  bool topologyPassScheduled = false;
  bool visibilityPassScheduled = false;
  int visibilityFirst = -1;
  int visibilityLast = -1;
  int visibilityCursor = -1;
};

} // namespace middle
} // namespace codexui::codex

#endif
