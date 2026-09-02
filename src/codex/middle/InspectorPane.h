// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H
#define CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H

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
#include <unordered_set>
#include <vector>

class QLabel;
class QPlainTextEdit;
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
  void setRequestActions(RequestAction review, RequestAction accept,
                         RequestAction reject);
  void refresh(const ui::InspectorSnapshot &snapshot);
  void appendProtocolFrame(const nlohmann::json &frame);

  [[nodiscard]] QTabWidget *tabs() const noexcept { return inspectorTabs; }

private:
  QFrame *agentFrame(const ui::InspectorAgentRow &agent);
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
  std::optional<ui::InspectorAgentsSnapshot> agentsSnapshot;
  std::unordered_set<std::string> expandedAgents;
  std::optional<ui::InspectorRequestsSnapshot> requestsSnapshot;
  QByteArray stateSnapshot;
  QByteArray protocolStatsSnapshot;
  std::deque<QString> protocolLines;
  std::uint64_t observedSequence = 0;
  bool protocolFollowsTail = true;
  bool mutatingProtocolLog = false;
  int protocolPausedScrollValue = 0;
  std::uint64_t protocolScrollRevision = 0;
};

} // namespace middle
} // namespace codexui::codex

#endif
