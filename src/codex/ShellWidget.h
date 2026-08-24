// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_SHELLWIDGET_H
#define CODEXUI_CODEX_SHELLWIDGET_H

#include "codex/PresentationModel.h"

#include <QWidget>

#include <cstdint>
#include <string>
#include <unordered_set>

class QFrame;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTabWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace codexui {
class ExpandingPromptEditor;
}

namespace codexui::codex {

class FrontendSession;
class TurnSettingsWidget;

class ShellWidget final : public QWidget {
public:
  explicit ShellWidget(FrontendSession &session, QWidget *parent = nullptr);

private:
  void handleEvent(const nlohmann::json &event);
  void scheduleRefresh();
  void refresh();
  void refreshThreads();
  void refreshConversation();
  void refreshInspector();
  void refreshStateInspector();
  void refreshProtocolStats();
  void refreshStatus();
  void refreshTurnSettings();
  void appendProtocolFrame(const nlohmann::json &frame);
  void hydrateHistoricalAgents();
  void showNotice(QString message, bool error = true);

  void selectThread(std::string threadId);
  void beginNewThread();
  void requestThreads();
  void requestModels();
  void requestEnvironment();
  void readSelectedThread();
  void renameSelectedThread();
  void forkSelectedThread();
  void toggleSelectedThreadArchive();
  void deleteSelectedThread();
  void submitPrompt();
  void submitPromptToThread(std::string threadId, std::string prompt,
                            nlohmann::json options = nlohmann::json::object());
  void interruptActiveTurn();
  void respondToFirstPending(bool approve);
  void reviewPending(const std::string &requestKey);
  void rejectPending(const std::string &requestKey);

  FrontendSession &session;
  PresentationModel model;
  std::string selectedThreadId;
  bool localNewThreadIntent = false;
  bool environmentRequested = false;

  QLabel *connectionLabel = nullptr;
  QLabel *controllerLabel = nullptr;
  QLabel *workspaceBreadcrumb = nullptr;
  QLabel *threadContextStatus = nullptr;
  QLabel *agentActivityStatus = nullptr;
  QLabel *conversationTitle = nullptr;
  QLabel *conversationMeta = nullptr;
  QLabel *emptyConversation = nullptr;
  QLabel *noticeLabel = nullptr;
  QFrame *connectionStatusDot = nullptr;
  QFrame *bottomConnectionStatusDot = nullptr;
  QFrame *noticeBar = nullptr;
  QFrame *sidebar = nullptr;
  QFrame *inspector = nullptr;
  QSplitter *splitter = nullptr;
  QListWidget *threadList = nullptr;
  QWidget *conversationContent = nullptr;
  QVBoxLayout *conversationLayout = nullptr;
  QScrollArea *conversationScroll = nullptr;
  QTabWidget *inspectorTabs = nullptr;
  QWidget *planContent = nullptr;
  QVBoxLayout *planLayout = nullptr;
  QWidget *agentsContent = nullptr;
  QVBoxLayout *agentsLayout = nullptr;
  QWidget *changesContent = nullptr;
  QVBoxLayout *changesLayout = nullptr;
  QWidget *requestsContent = nullptr;
  QVBoxLayout *requestsLayout = nullptr;
  QLabel *protocolStats = nullptr;
  QPlainTextEdit *protocolLog = nullptr;
  QPlainTextEdit *stateView = nullptr;
  codexui::ExpandingPromptEditor *promptEditor = nullptr;
  TurnSettingsWidget *turnSettings = nullptr;
  QPushButton *sendButton = nullptr;
  QPushButton *interruptButton = nullptr;
  QPushButton *controllerButton = nullptr;
  QPushButton *attentionButton = nullptr;
  QPushButton *reconnectButton = nullptr;
  QPushButton *restoreSidebarButton = nullptr;
  QPushButton *restoreInspectorButton = nullptr;
  QToolButton *threadActionsButton = nullptr;
  QPushButton *approveButton = nullptr;
  QPushButton *denyButton = nullptr;
  QTimer *refreshTimer = nullptr;
  std::uint64_t observedPresentationSequence = 0;
  std::unordered_set<std::string> requestedAgentThreads;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_SHELLWIDGET_H
