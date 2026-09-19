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

class QLabel;
class QHideEvent;
class QPlainTextEdit;
class QShowEvent;
class QStackedWidget;
class QTabWidget;

namespace codexui::codex {

class DiffViewer;

namespace middle {

class MarkdownTextView;

// The inspector owns only presentation snapshots.  It never clears a visible
// tab in response to an unrelated frame and never participates in app-server
// state ownership.
class InspectorPane final : public QFrame {
public:
  using RequestAction = std::function<void(const nodegraph::NodeRef &)>;

  explicit InspectorPane(QWidget *parent = nullptr);
  ~InspectorPane() override;
  static void prepareMarkdown(ui::InspectorSnapshot &snapshot);

  void setHideAction(std::function<void()> hide);
  void setRefreshRequestedAction(std::function<void()> refresh);
  void setRequestActions(RequestAction review, RequestAction accept,
                         RequestAction reject);
  void refresh(const ui::InspectorSnapshot &snapshot,
               ui::InspectorProjection projection);
  [[nodiscard]] std::optional<ui::InspectorProjection>
  currentProjection() const;
  [[nodiscard]] ui::InspectorRowRequest
  rowRequest(ui::InspectorProjection projection) const;
  void appendProtocolFrame(const nlohmann::json &frame);
  [[nodiscard]] bool
  appendProtocolDiagnostic(const nodegraph::ProtocolDiagnostic &diagnostic);
  void flushProtocolPresentation();
  [[nodiscard]] bool
  retainsTarget(const nodegraph::NodeRef &target) const noexcept;

  [[nodiscard]] QTabWidget *tabs() const noexcept { return inspectorTabs; }

protected:
  void hideEvent(QHideEvent *event) override;
  void showEvent(QShowEvent *event) override;

private:
  class AgentFrame;
  class PlanStepFrame;
  class RequestFrame;
  class RowViewport;

  AgentFrame *agentFrame(const std::string &rowKey);
  RequestFrame *requestFrame();
  void retireThreadPresentation();
  void refreshCurrentTab();
  void refreshChanges();
  void refreshState();
  void refreshProtocolStats();
  void showProtocolTail();
  void restoreProtocolScroll(bool followsTail, int pausedValue);

  std::optional<std::uint64_t> currentThreadIncarnation;
  ui::InspectorChangesSnapshot changes;
  ui::InspectorStateSnapshot state;
  RequestAction reviewRequest;
  RequestAction acceptRequest;
  RequestAction rejectRequest;
  std::function<void()> hideAction;
  std::function<void()> refreshRequested;

  QTabWidget *inspectorTabs = nullptr;
  QStackedWidget *infoStack = nullptr;
  RowViewport *planRows = nullptr;
  RowViewport *agentsRows = nullptr;
  RowViewport *requestRows = nullptr;
  DiffViewer *diffViewer = nullptr;
  QPlainTextEdit *stateView = nullptr;
  QPlainTextEdit *protocolLog = nullptr;
  QLabel *protocolStats = nullptr;

  std::deque<std::string> expandedAgentIds;
  QByteArray stateSnapshot;
  QByteArray protocolStatsSnapshot;
  std::deque<QString> protocolLines;
  std::deque<QString> pendingProtocolLines;
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
