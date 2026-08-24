// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_GREENFIELD_CODEX_MIDDLE_INSPECTORPANE_H
#define CODEXUI_GREENFIELD_CODEX_MIDDLE_INSPECTORPANE_H

#include <QByteArray>
#include <QFrame>
#include <QString>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

class QLabel;
class QPlainTextEdit;
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
  QTabWidget *infoTabs = nullptr;
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

  QByteArray planSnapshot;
  QByteArray agentsSnapshot;
  QByteArray changesSnapshot;
  QByteArray requestsSnapshot;
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
