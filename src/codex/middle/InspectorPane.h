// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H
#define CODEXUI_CODEX_MIDDLE_INSPECTORPANE_H

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
#include <vector>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QTabWidget;
class QVBoxLayout;

namespace codexui::codex {

class DiffViewer;
class PresentationModel;

namespace middle {

// The inspector owns only presentation snapshots.  It never clears a visible
// tab in response to an unrelated frame and never participates in app-server
// state ownership.
class InspectorPane final : public QFrame {
public:
  using RequestAction = std::function<void(const std::string &)>;

  explicit InspectorPane(QWidget *parent = nullptr);

  void setHideAction(std::function<void()> hide);
  void setRequestActions(RequestAction review, RequestAction reject);
  void refresh(const PresentationModel &model,
               const std::string &selectedThreadId);
  void appendProtocolFrame(const nlohmann::json &frame);

  [[nodiscard]] QTabWidget *tabs() const noexcept { return inspectorTabs; }

private:
  struct PlanStepSnapshot {
    std::string step;
    std::string status;

    bool operator==(const PlanStepSnapshot &) const = default;
  };
  struct PlanContentSnapshot {
    std::string explanation;
    std::vector<PlanStepSnapshot> steps;

    bool operator==(const PlanContentSnapshot &) const = default;
  };
  struct PlanSnapshot {
    std::string threadId;
    bool threadPresent = false;
    std::optional<PlanContentSnapshot> plan;
    std::optional<std::string> planItem;

    bool operator==(const PlanSnapshot &) const = default;
  };
  struct AgentSnapshot {
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

    bool operator==(const AgentSnapshot &) const = default;
  };
  struct AgentsSnapshot {
    std::string threadId;
    bool threadPresent = false;
    std::vector<AgentSnapshot> agents;

    bool operator==(const AgentsSnapshot &) const = default;
  };
  struct RequestSnapshot {
    std::string id;
    std::string kind;
    std::string threadContext;
    std::uint64_t generation = 0;
    std::string command;
    std::string reason;
    std::string message;
    std::optional<std::size_t> questionCount;

    bool operator==(const RequestSnapshot &) const = default;
  };

  static QFrame *agentFrame(const AgentSnapshot &agent);
  void refreshCurrentTab();
  void refreshPlan();
  void refreshAgents();
  void refreshChanges();
  void refreshRequests();
  void refreshState();
  void refreshProtocolStats();
  void showProtocolTail();
  void restoreProtocolScroll(bool followsTail, int pausedValue);

  const PresentationModel *currentModel = nullptr;
  std::string currentThreadId;
  RequestAction reviewRequest;
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

  std::optional<PlanSnapshot> planSnapshot;
  std::optional<AgentsSnapshot> agentsSnapshot;
  std::optional<std::vector<RequestSnapshot>> requestsSnapshot;
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
