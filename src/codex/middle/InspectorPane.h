// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H
#define CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H

#include "codex/nodegraph/Messages.h"

#include <QByteArray>
#include <QFrame>
#include <QString>

#include <cstddef>
#include <cstdint>
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

// Short-lived render inputs extracted from one successful non-blocking graph
// read. They contain only the values required by the active inspector tab and
// are never retained as application authority.
struct InspectorPlanStepRender final {
  std::string step;
  std::string status;
  bool operator==(const InspectorPlanStepRender &) const = default;
};

struct InspectorPlanRender final {
  std::string explanation;
  std::vector<InspectorPlanStepRender> steps;
  bool operator==(const InspectorPlanRender &) const = default;
};

struct InspectorPlanData final {
  std::string threadId;
  bool threadPresent = false;
  std::optional<InspectorPlanRender> plan;
  std::optional<std::string> planItem;
  bool operator==(const InspectorPlanData &) const = default;
};

struct InspectorAgentRender final {
  std::string id;
  std::string status;
  std::string childThreadId;
  std::string agentPath;
  std::string tool;
  std::string model;
  std::string reasoningEffort;
  std::string prompt;
  std::string resultText;
  std::string senderThreadId;
  std::vector<std::string> receiverThreadIds;
  bool operator==(const InspectorAgentRender &) const = default;
};

struct InspectorAgentsData final {
  std::string threadId;
  bool threadPresent = false;
  std::vector<InspectorAgentRender> agents;
  bool operator==(const InspectorAgentsData &) const = default;
};

struct InspectorRequestRender final {
  std::string id;
  std::string kind;
  std::string threadContext;
  std::uint64_t generation = 0;
  std::string command;
  std::string reason;
  std::string message;
  std::optional<std::size_t> questionCount;
  bool actionable = false;
  bool operator==(const InspectorRequestRender &) const = default;
};

struct InspectorRequestsData final {
  std::vector<InspectorRequestRender> requests;
  bool operator==(const InspectorRequestsData &) const = default;
};

struct InspectorChangesData final {
  std::string threadId;
  std::string cwd;
  std::vector<std::string> commandCwds;
  std::vector<std::string> changedPaths;
  bool operator==(const InspectorChangesData &) const = default;
};

class InspectorPane final : public QFrame {
public:
  using RequestAction = std::function<void(const std::string &)>;

  explicit InspectorPane(QWidget *parent = nullptr);

  void setHideAction(std::function<void()> hide);
  void setRequestActions(RequestAction review, RequestAction accept,
                         RequestAction reject);
  void refresh(const nodegraph::NodeGraph &graph,
               nodegraph::NodeRef selectedThread = {});
  void graphChanged(const nodegraph::GraphChanged &change);

  [[nodiscard]] QTabWidget *tabs() const noexcept { return inspectorTabs; }

private:
  QFrame *agentFrame(const InspectorAgentRender &agent,
                     std::string_view threadId);
  QFrame *requestFrame(const InspectorRequestRender &request);
  void refreshCurrentTab();
  [[nodiscard]] bool
  graphChangeAffectsCurrentTab(const nodegraph::GraphChanged &change);
  void scheduleGraphRefresh();
  void runGraphRefresh();
  void cancelGraphRowRenders();
  void renderGraphPlan(InspectorPlanData snapshot);
  void renderGraphAgents(InspectorAgentsData snapshot);
  void renderGraphRequests(InspectorRequestsData snapshot);
  void scheduleGraphPlanRender();
  void scheduleGraphAgentsRender();
  void scheduleGraphRequestsRender();
  void runGraphPlanRender();
  void runGraphAgentsRender();
  void runGraphRequestsRender();
  void renderChanges(const InspectorChangesData &snapshot);
  void renderGraphState(QString value);
  void renderGraphProtocol(QString log, QString statistics);

  const nodegraph::NodeGraph *graph = nullptr;
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

  std::optional<InspectorPlanData> planSnapshot;
  std::optional<InspectorAgentsData> agentsSnapshot;
  std::unordered_set<std::string> expandedAgents;
  std::optional<InspectorRequestsData> requestsSnapshot;
  std::optional<InspectorChangesData> changesSnapshot;
  std::optional<InspectorPlanData> pendingPlanSnapshot;
  std::optional<InspectorAgentsData> pendingAgentsSnapshot;
  std::optional<InspectorRequestsData> pendingRequestsSnapshot;
  std::unordered_set<const nodegraph::Node *> activeGraphDependencies;
  QByteArray stateSnapshot;
  QByteArray protocolStatsSnapshot;
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
};

} // namespace middle
} // namespace codexui::codex

#endif
