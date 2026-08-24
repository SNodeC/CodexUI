// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_WORKBENCHWIDGET_H
#define CODEXUI_CODEX_WORKBENCHWIDGET_H

#include "codex/PresentationModel.h"

#include <QWidget>

#include <cstdint>
#include <string>
#include <unordered_set>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace codexui {
class ExpandingPromptEditor;
}

namespace codexui::codex {

class FrontendSession;

class WorkbenchWidget final : public QWidget {
public:
  explicit WorkbenchWidget(FrontendSession &session, QWidget *parent = nullptr);

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
  void appendProtocolFrame(const nlohmann::json &frame);
  void hydrateHistoricalAgents();

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
  void submitPromptToThread(std::string threadId, std::string prompt);
  void interruptActiveTurn();
  void respondToFirstPending(bool approve);

  FrontendSession &session;
  PresentationModel model;
  std::string selectedThreadId;
  bool localNewThreadIntent = false;
  bool environmentRequested = false;

  QLabel *connectionLabel = nullptr;
  QLabel *controllerLabel = nullptr;
  QLabel *attentionLabel = nullptr;
  QLabel *conversationTitle = nullptr;
  QLabel *conversationMeta = nullptr;
  QLabel *emptyConversation = nullptr;
  QListWidget *threadList = nullptr;
  QWidget *conversationContent = nullptr;
  QVBoxLayout *conversationLayout = nullptr;
  QScrollArea *conversationScroll = nullptr;
  QTabWidget *inspectorTabs = nullptr;
  QWidget *planContent = nullptr;
  QVBoxLayout *planLayout = nullptr;
  QWidget *agentsContent = nullptr;
  QVBoxLayout *agentsLayout = nullptr;
  QWidget *requestsContent = nullptr;
  QVBoxLayout *requestsLayout = nullptr;
  QLabel *protocolStats = nullptr;
  QPlainTextEdit *protocolLog = nullptr;
  QPlainTextEdit *stateView = nullptr;
  codexui::ExpandingPromptEditor *promptEditor = nullptr;
  QPushButton *sendButton = nullptr;
  QPushButton *interruptButton = nullptr;
  QPushButton *controllerButton = nullptr;
  QToolButton *threadActionsButton = nullptr;
  QPushButton *approveButton = nullptr;
  QPushButton *denyButton = nullptr;
  QTimer *refreshTimer = nullptr;
  std::uint64_t observedPresentationSequence = 0;
  std::unordered_set<std::string> requestedAgentThreads;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_WORKBENCHWIDGET_H
