// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_SHELLWIDGET_H
#define CODEXUI_CODEX_SHELLWIDGET_H

#include "codex/FileSelectionDialog.h"
#include "codex/PresentationModel.h"

#include <QWidget>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

class QFrame;
class QAction;
class QEvent;
class QGridLayout;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTabWidget;
class QTimer;
class QToolButton;
class QVariantAnimation;
class QVBoxLayout;

namespace codexui {
class ExpandingPromptEditor;
}

namespace codexui::codex {

class FrontendSession;
class DiffViewer;
class ShellWidgetScrollTest;
class TurnSettingsWidget;

class ShellWidget final : public QWidget {
public:
  explicit ShellWidget(FrontendSession &session, QWidget *parent = nullptr);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  friend class ShellWidgetScrollTest;

  enum class PendingPromptStatus { Awaiting, Acknowledged, Failed };
  enum class ThreadHydrationState { NotHydrated, ReadInFlight, Hydrated };

  struct PendingPrompt {
    std::uint64_t id = 0;
    QString prompt;
    std::vector<AttachmentDraft> attachments;
    nlohmann::json turnOptions = nlohmann::json::object();
    PendingPromptStatus status = PendingPromptStatus::Awaiting;
    bool dispatched = false;
    bool readinessRetryAttempted = false;
    qint64 admittedAtMilliseconds = 0;
    qint64 acknowledgedAtMilliseconds = 0;
    bool completionRefreshScheduled = false;
    std::string materializedIdentity;
    QString error;
    std::unordered_set<std::string> knownUserMessageIds;
  };

  struct ConversationScrollAnchor {
    QString key;
    int viewportOffset = 0;
    int absoluteValue = 0;
  };

  enum RefreshArea : std::uint32_t {
    RefreshNone = 0,
    RefreshThreads = 1U << 0U,
    RefreshConversation = 1U << 1U,
    RefreshInspector = 1U << 2U,
    RefreshState = 1U << 3U,
    RefreshProtocolStats = 1U << 4U,
    RefreshTurnSettings = 1U << 5U,
    RefreshStatus = 1U << 6U,
    RefreshAll = (1U << 7U) - 1U,
  };

  void handleEvent(const nlohmann::json &event);
  void scheduleRefresh(std::uint32_t areas = RefreshAll);
  void refresh();
  void refreshThreads();
  void refreshConversation();
  void refreshConversationItems();
  [[nodiscard]] bool refreshConversationItem(const std::string &key,
                                             const std::string &turnId,
                                             const std::string &itemId,
                                             bool &changed);
  void refreshInspector();
  void refreshStateInspector();
  void refreshProtocolStats();
  void showProtocolTail();
  void refreshStatus();
  void refreshTurnSettings();
  [[nodiscard]] std::string visiblySelectedThreadId() const;
  void addConversationTrailingSpace();
  void updateComposerDockHeight(int height);
  void scheduleConversationFollowLatest();
  void scrollConversationToLatest(bool smoothly = true);
  void stopConversationScrollAnimation();
  [[nodiscard]] ConversationScrollAnchor
  captureConversationScrollAnchor() const;
  void restoreConversationScrollAnchor(const ConversationScrollAnchor &anchor);
  void scheduleConversationPausedAnchorRestore();
  void settleConversationScroll(bool followLatest,
                                ConversationScrollAnchor anchor, bool smoothly);
  void appendProtocolFrame(const nlohmann::json &frame);
  void hydrateHistoricalAgents();
  void showNotice(QString message, bool error = true);
  [[nodiscard]] std::uint32_t
  refreshAreasForEvent(const nlohmann::json &event) const;
  [[nodiscard]] std::string
  conversationItemFingerprint(const ItemPresentation &item) const;
  [[nodiscard]] QString
  pendingPromptAnchorKey(const std::string &threadId,
                         std::uint64_t submissionId) const;
  void scheduleAcknowledgementCompletion(const std::string &threadId,
                                         PendingPrompt &submission);

  void selectThread(std::string threadId);
  void beginNewThread();
  void requestThreads();
  void requestModels();
  void readThread(const std::string &threadId);
  void ensureThreadHydrated(const std::string &threadId);
  [[nodiscard]] bool threadIsHydrated(const std::string &threadId) const;
  [[nodiscard]] bool threadRequiresResume(const std::string &threadId) const;
  void renameThread(const std::string &threadId);
  void forkThread(const std::string &threadId);
  void toggleThreadArchive(const std::string &threadId);
  void deleteThread(const std::string &threadId);
  void submitPrompt();
  void submitPromptToThread(std::string threadId, std::uint64_t submissionId,
                            std::string prompt, nlohmann::json options,
                            std::vector<AttachmentDraft> attachments);
  void dispatchNextPrompt(const std::string &threadId);
  void startThreadForPendingPrompts();
  void resetComposer();
  void refreshComposerEnabledState();
  void completePromptSubmission(const std::string &threadId,
                                std::uint64_t submissionId,
                                const nlohmann::json &result);
  [[nodiscard]] bool attemptPromptThreadRecovery(const std::string &threadId,
                                                 std::uint64_t submissionId,
                                                 const nlohmann::json &result);
  void reconcileAcknowledgedPrompts(const std::string &threadId);
  [[nodiscard]] std::unordered_set<std::string>
  materializedUserMessageIds(const std::string &threadId) const;
  void chooseAttachments();
  void refreshAttachments();
  void scheduleComposerLayout();
  void refreshComposerLayout();
  void interruptActiveTurn();
  void respondToFirstPending(bool approve);
  void reviewPending(const std::string &requestKey);
  void rejectPending(const std::string &requestKey);

  FrontendSession &session;
  PresentationModel model;
  std::string selectedThreadId;
  bool localNewThreadIntent = false;
  nlohmann::json newThreadDraftOptions = nlohmann::json::object();
  QString newThreadDraftName;
  QString newThreadDraftWorkspace;
  std::vector<AttachmentDraft> attachmentDrafts;
  std::uint64_t attachmentRevision = 0;
  std::unordered_map<std::string, std::deque<PendingPrompt>> pendingPrompts;
  std::deque<PendingPrompt> newThreadPendingPrompts;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      materializedPromptItemIds;
  std::unordered_map<std::string, QString> promptAnchorKeys;
  std::unordered_map<std::string, ThreadHydrationState> threadHydration;
  std::unordered_set<std::string> operationReadyThreads;
  std::uint64_t nextPendingPromptId = 1;
  bool newThreadCreationInFlight = false;

  QLabel *controllerLabel = nullptr;
  QLabel *workspaceBreadcrumb = nullptr;
  QLabel *threadContextStatus = nullptr;
  QLabel *agentActivityStatus = nullptr;
  QLabel *conversationTitle = nullptr;
  QLabel *conversationMeta = nullptr;
  QLabel *emptyConversation = nullptr;
  QLabel *noticeLabel = nullptr;
  QFrame *connectionStatusDot = nullptr;
  QFrame *noticeBar = nullptr;
  QFrame *sidebar = nullptr;
  QFrame *conversationRegion = nullptr;
  QFrame *inspector = nullptr;
  QSplitter *splitter = nullptr;
  QListWidget *threadList = nullptr;
  QWidget *conversationContent = nullptr;
  QWidget *conversationTrailingSpace = nullptr;
  QWidget *composerReserve = nullptr;
  QVBoxLayout *conversationLayout = nullptr;
  QScrollArea *conversationScroll = nullptr;
  QTabWidget *inspectorTabs = nullptr;
  QTabWidget *infoTabs = nullptr;
  QWidget *planContent = nullptr;
  QVBoxLayout *planLayout = nullptr;
  QWidget *agentsContent = nullptr;
  QVBoxLayout *agentsLayout = nullptr;
  DiffViewer *diffViewer = nullptr;
  QWidget *requestsContent = nullptr;
  QVBoxLayout *requestsLayout = nullptr;
  QLabel *protocolStats = nullptr;
  QPlainTextEdit *protocolLog = nullptr;
  QPlainTextEdit *stateView = nullptr;
  codexui::ExpandingPromptEditor *promptEditor = nullptr;
  TurnSettingsWidget *turnSettings = nullptr;
  QWidget *composerBody = nullptr;
  QGridLayout *composerGrid = nullptr;
  QPushButton *sendButton = nullptr;
  QToolButton *attachmentButton = nullptr;
  QFrame *attachmentPanel = nullptr;
  QScrollArea *attachmentListScroll = nullptr;
  QVBoxLayout *attachmentListLayout = nullptr;
  QPushButton *interruptButton = nullptr;
  QPushButton *controllerButton = nullptr;
  QPushButton *requestButton = nullptr;
  QToolButton *connectionButton = nullptr;
  QAction *connectAction = nullptr;
  QAction *disconnectAction = nullptr;
  QAction *reconnectAction = nullptr;
  QPushButton *restoreSidebarButton = nullptr;
  QPushButton *restoreInspectorButton = nullptr;
  QPushButton *approveButton = nullptr;
  QPushButton *denyButton = nullptr;
  QTimer *refreshTimer = nullptr;
  QVariantAnimation *conversationScrollAnimation = nullptr;
  std::uint64_t observedPresentationSequence = 0;
  std::uint32_t pendingRefreshAreas = RefreshAll;
  bool composerExpanded = false;
  bool composerActive = false;
  bool composerLayoutRefreshPending = false;
  bool conversationFollowsLatest = true;
  bool conversationScrollRebuilding = false;
  bool conversationScrollProgrammatic = false;
  bool conversationSpacerAdjusting = false;
  bool conversationFollowScrollPending = false;
  bool conversationUserScrollPending = false;
  bool conversationUserScrollInteraction = false;
  bool conversationSmoothFollowRequested = false;
  int conversationSmoothScrollFloor = 0;
  ConversationScrollAnchor conversationPausedAnchor;
  bool conversationPausedAnchorValid = false;
  bool conversationPausedAnchorRestorePending = false;
  int composerCanonicalHeight = 0;
  int conversationTrailingSpaceHeight = 0;
  std::uint64_t conversationSpacerRevision = 0;
  std::uint64_t conversationScrollSettlementRevision = 0;
  std::size_t conversationItemLimit = 80;
  bool conversationRebuildPending = true;
  std::unordered_map<std::string, QWidget *> conversationCards;
  std::unordered_map<std::string, std::string> conversationCardFingerprints;
  std::unordered_map<std::string, std::pair<bool, int>>
      commandOutputScrollStates;
  std::unordered_map<std::string, std::pair<std::string, std::string>>
      dirtyConversationItems;
  std::deque<QString> protocolLines;
  std::unordered_set<std::string> requestedAgentThreads;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_SHELLWIDGET_H
