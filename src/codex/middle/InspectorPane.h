// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H
#define CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H

#include "codex/nodegraph/Messages.h"
#include "codex/ui/UiViewState.h"

#include <QByteArray>
#include <QFrame>
#include <QString>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QStackedWidget;
class QTabWidget;
class QVBoxLayout;

namespace codexui::codex {

class DiffViewer;

namespace middle {

// The legacy snapshot entry point remains a compatibility oracle. Graph mode
// extracts only the currently visible tab's render values from the one shared
// NodeGraph and never participates in app-server state ownership.
class InspectorPane final : public QFrame {
public:
  using RequestAction = std::function<void(const std::string &)>;

  explicit InspectorPane(QWidget *parent = nullptr);

  void setHideAction(std::function<void()> hide);
  void setRequestActions(RequestAction review, RequestAction accept,
                         RequestAction reject);
  void refresh(const ui::InspectorSnapshot &snapshot);
  void refresh(nodegraph::NodeGraph &graph,
               nodegraph::NodeRef selectedThread = {});
  void graphChanged(const nodegraph::GraphChanged &change);
  void appendProtocolFrame(const nlohmann::json &frame);

  [[nodiscard]] QTabWidget *tabs() const noexcept { return inspectorTabs; }

private:
  QFrame *agentFrame(const ui::InspectorAgentRow &agent,
                     std::string_view threadId);
  QFrame *requestFrame(const ui::InspectorRequestRow &request);
  void refreshCurrentTab();
  [[nodiscard]] bool
  graphChangeAffectsCurrentTab(const nodegraph::GraphChanged &change);
  void scheduleGraphRefresh();
  void runGraphRefresh();
  void cancelGraphRowRenders();
  void renderGraphPlan(ui::InspectorPlanSnapshot snapshot);
  void renderGraphAgents(ui::InspectorAgentsSnapshot snapshot);
  void renderGraphRequests(ui::InspectorRequestsSnapshot snapshot);
  void scheduleGraphPlanRender();
  void scheduleGraphAgentsRender();
  void scheduleGraphRequestsRender();
  void runGraphPlanRender();
  void runGraphAgentsRender();
  void runGraphRequestsRender();
  void refreshPlan();
  void refreshAgents();
  void refreshChanges();
  void refreshRequests();
  void refreshState();
  void refreshProtocolStats();
  void renderPlan(const ui::InspectorPlanSnapshot &snapshot);
  void renderAgents(const ui::InspectorAgentsSnapshot &snapshot);
  void renderChanges(const ui::InspectorChangesSnapshot &snapshot);
  void renderRequests(const ui::InspectorRequestsSnapshot &snapshot);
  void renderGraphState(QString value);
  void renderGraphProtocol(QString log, QString statistics);
  void showProtocolTail();
  void restoreProtocolScroll(bool followsTail, int pausedValue);

  std::optional<ui::InspectorSnapshot> currentSnapshot;
  nodegraph::NodeGraph *graph = nullptr;
  nodegraph::NodeRef selectedGraphThread;
  RequestAction reviewRequest;
  RequestAction acceptRequest;
  RequestAction rejectRequest;
  std::function<void()> hideAction;

  QTabWidget *inspectorTabs = nullptr;
  QStackedWidget *infoStack = nullptr;
  QWidget *planContent = nullptr;
  QVBoxLayout *planLayout = nullptr;
  QScrollArea *planScroll = nullptr;
  QWidget *agentsContent = nullptr;
  QVBoxLayout *agentsLayout = nullptr;
  QScrollArea *agentsScroll = nullptr;
  QWidget *requestsContent = nullptr;
  QVBoxLayout *requestsLayout = nullptr;
  QScrollArea *requestsScroll = nullptr;
  DiffViewer *diffViewer = nullptr;
  QPlainTextEdit *stateView = nullptr;
  QPlainTextEdit *protocolLog = nullptr;
  QLabel *protocolStats = nullptr;

  std::optional<ui::InspectorPlanSnapshot> planSnapshot;
  std::optional<ui::InspectorAgentsSnapshot> agentsSnapshot;
  std::unordered_set<std::string> expandedAgents;
  std::optional<ui::InspectorRequestsSnapshot> requestsSnapshot;
  std::optional<ui::InspectorChangesSnapshot> changesSnapshot;
  std::optional<ui::InspectorPlanSnapshot> pendingPlanSnapshot;
  std::optional<ui::InspectorAgentsSnapshot> pendingAgentsSnapshot;
  std::optional<ui::InspectorRequestsSnapshot> pendingRequestsSnapshot;
  std::unordered_set<const nodegraph::Node *> activeGraphDependencies;
  QByteArray stateSnapshot;
  QByteArray protocolStatsSnapshot;
  std::deque<QString> protocolLines;
  std::uint64_t observedSequence = 0;
  bool protocolFollowsTail = true;
  bool mutatingProtocolLog = false;
  bool graphRefreshScheduled = false;
  bool planRenderScheduled = false;
  bool agentsRenderScheduled = false;
  bool requestsRenderScheduled = false;
  bool planRenderClearing = false;
  bool agentsRenderClearing = false;
  bool requestsRenderClearing = false;
  std::size_t planRenderCursor = 0;
  std::size_t agentsRenderCursor = 0;
  std::size_t requestsRenderCursor = 0;
  std::size_t agentsPreservedRows = 0;
  std::size_t requestsPreservedRows = 0;
  int planScrollValue = 0;
  int agentsScrollValue = 0;
  int requestsScrollValue = 0;
  int graphDependenciesTab = -1;
  int graphDependenciesInfoPage = -1;
  int protocolPausedScrollValue = 0;
  std::uint64_t protocolScrollRevision = 0;
};

} // namespace middle
} // namespace codexui::codex

#endif
