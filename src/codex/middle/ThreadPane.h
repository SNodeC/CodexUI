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
#include <set>
#include <string>
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
  // Narrow scheduling instrumentation used by the responsiveness regression
  // tests. The counters describe Qt work, not graph/domain state.
  [[nodiscard]] std::size_t maximumGraphScanWorkObserved() const noexcept;
  [[nodiscard]] std::size_t maximumTopologyWorkObserved() const noexcept;
  [[nodiscard]] std::size_t maximumVisibilityWorkObserved() const noexcept;
  [[nodiscard]] std::size_t materializedRowCount() const noexcept;
  [[nodiscard]] std::uint64_t completedTopologyCount() const noexcept;
  [[nodiscard]] std::uint64_t topologyValidationPassCount() const noexcept;
  [[nodiscard]] std::uint64_t discardedTopologyCount() const noexcept;
  [[nodiscard]] std::uint64_t graphReadRetryCount() const noexcept;
  [[nodiscard]] std::uint64_t contextMenuReadRetryCount() const noexcept;

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
                       std::optional<QPoint> requestedPosition,
                       std::uint64_t retryToken, unsigned retryAttempt);
  void leaveGraph();
  void scheduleGraphRefresh();
  void scheduleGraphScanPass(bool lockContended = false);
  void runGraphRefresh();
  void applyGraphTopology(GraphTopology topology);
  void scheduleTopologyPass(bool lockContended = false);
  void runTopologyPass();
  void scheduleVisibilityPass(bool lockContended = false);
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
  std::set<const nodegraph::Node *> graphExpandedThreads;
  bool revealSelectedGraphThread = false;
  bool graphRefreshAfterCurrent = false;
  bool graphRefreshScheduled = false;
  unsigned graphReadRetryAttempt = 0;
  unsigned topologyReadRetryAttempt = 0;
  unsigned visibilityReadRetryAttempt = 0;
  std::uint64_t graphBindingEpoch = 0;
  std::uint64_t topologyInputEpoch = 0;
  std::uint64_t contextMenuRetryToken = 0;
  std::unique_ptr<GraphScan> pendingGraphScan;
  std::unique_ptr<GraphTopology> pendingGraphTopology;
  std::size_t topologyCursor = 0;
  int topologySearchCursor = -1;
  bool topologyPassScheduled = false;
  bool visibilityPassScheduled = false;
  int visibilityFirst = -1;
  int visibilityLast = -1;
  int visibilityCursor = -1;
  std::size_t maximumGraphScanWork = 0;
  std::size_t maximumTopologyWork = 0;
  std::size_t maximumVisibilityWork = 0;
  std::uint64_t completedTopologies = 0;
  std::uint64_t topologyValidationPasses = 0;
  std::uint64_t discardedTopologies = 0;
  std::uint64_t rowPresentationUpdates = 0;
  std::uint64_t topologyScansStarted = 0;
  std::uint64_t wholePaneUpdateSuppressions = 0;
  bool topologyCommitUpdatesSuppressed = false;
  std::uint64_t graphReadRetries = 0;
  std::uint64_t contextMenuReadRetries = 0;
};

} // namespace middle
} // namespace codexui::codex

#endif
