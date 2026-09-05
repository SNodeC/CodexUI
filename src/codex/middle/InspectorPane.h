// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H
#define CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H

#include "codex/ui/UiViewState.h"
#include "codex/nodegraph/Messages.h"

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
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QLabel;
class QPlainTextEdit;
class QShowEvent;
class QStackedWidget;
class QTabWidget;
class QVBoxLayout;

namespace codexui::codex {

class DiffViewer;

namespace middle {

// The inspector owns only presentation snapshots.  It never clears a visible
// tab in response to an unrelated frame and never participates in app-server
// state ownership.
class InspectorPane final : public QFrame {
public:
  using RequestAction = std::function<void(const std::string &)>;

  explicit InspectorPane(QWidget *parent = nullptr);

  void setHideAction(std::function<void()> hide);
  void setRefreshRequestedAction(std::function<void()> refresh);
  void setRequestActions(RequestAction review, RequestAction accept,
                         RequestAction reject);
  void refresh(const ui::InspectorSnapshot &snapshot);
  void appendProtocolFrame(const nlohmann::json &frame);
  void appendProtocolDiagnostic(const nodegraph::UiEffect &effect);

  [[nodiscard]] QTabWidget *tabs() const noexcept { return inspectorTabs; }

protected:
  void showEvent(QShowEvent *event) override;

private:
  QFrame *agentFrame(const ui::InspectorAgentRow &agent);
  void patchAgentFrame(QFrame *frame, const ui::InspectorAgentRow &agent);
  QFrame *planStepFrame(const ui::InspectorPlanStep &step);
  void patchPlanStepFrame(QFrame *frame,
                          const ui::InspectorPlanStep &step);
  QFrame *requestFrame(const ui::InspectorRequestRow &request);
  void patchRequestFrame(QFrame *frame,
                         const ui::InspectorRequestRow &request);
  void refreshCurrentTab();
  void refreshPlan();
  void refreshAgents();
  void refreshChanges();
  void refreshRequests();
  void refreshState();
  void refreshProtocolStats();
  void showProtocolTail();
  void restoreProtocolScroll(bool followsTail, int pausedValue);

  std::optional<ui::InspectorSnapshot> currentSnapshot;
  RequestAction reviewRequest;
  RequestAction acceptRequest;
  RequestAction rejectRequest;
  std::function<void()> hideAction;
  std::function<void()> refreshRequested;

  QTabWidget *inspectorTabs = nullptr;
  QStackedWidget *infoStack = nullptr;
  QWidget *planContent = nullptr;
  QVBoxLayout *planLayout = nullptr;
  QWidget *agentsContent = nullptr;
  QVBoxLayout *agentsLayout = nullptr;
  QWidget *requestsContent = nullptr;
  QVBoxLayout *requestsLayout = nullptr;
  DiffViewer *diffViewer = nullptr;
  QPlainTextEdit *stateView = nullptr;
  QPlainTextEdit *protocolLog = nullptr;
  QLabel *protocolStats = nullptr;

  std::optional<ui::InspectorPlanSnapshot> planSnapshot;
  std::unordered_map<std::string, QFrame *> planFrames;
  std::unordered_map<std::string, ui::InspectorPlanStep> renderedPlanSteps;
  QWidget *planExplanation = nullptr;
  QWidget *planMessage = nullptr;
  std::optional<ui::InspectorAgentsSnapshot> agentsSnapshot;
  std::unordered_map<std::string, QFrame *> agentFrames;
  std::unordered_map<std::string, ui::InspectorAgentRow> renderedAgentRows;
  QWidget *agentsMessage = nullptr;
  std::unordered_set<std::string> expandedAgents;
  std::optional<ui::InspectorRequestsSnapshot> requestsSnapshot;
  std::unordered_map<std::string, QFrame *> requestFrames;
  std::unordered_map<std::string, ui::InspectorRequestRow> renderedRequests;
  QWidget *requestsMessage = nullptr;
  QByteArray stateSnapshot;
  QByteArray protocolStatsSnapshot;
  std::deque<QString> protocolLines;
  std::uint64_t observedSequence = 0;
  std::size_t protocolTelemetryCount = 0;
  bool protocolFollowsTail = true;
  bool mutatingProtocolLog = false;
  int protocolPausedScrollValue = 0;
  std::uint64_t protocolScrollRevision = 0;
};

} // namespace middle
} // namespace codexui::codex

#endif
