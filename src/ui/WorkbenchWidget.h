// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_WORKBENCHWIDGET_H
#define CODEXUI_UI_WORKBENCHWIDGET_H

#include "ui/InteractiveRequestDialog.h"
#include "ui/ConversationWidget.h"
#include "ui/ThreadSetupDialog.h"
#include "ui/UpcomingTurnDock.h"

#include <QWidget>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class QFrame;
class QLabel;
class QPushButton;
class QSplitter;

namespace codexui {

namespace detail {
struct StateUpdateScope;

[[nodiscard]] constexpr bool shouldClearMissingSelectedThread(
    bool ready,
    bool threadDiscoveryTerminal,
    bool awaitingSelectedThread,
    std::size_t omittedThreads,
    bool authoritativelyRemoved) noexcept
{
    return authoritativelyRemoved
           || (ready && threadDiscoveryTerminal && !awaitingSelectedThread
               && omittedThreads == 0);
}

[[nodiscard]] inline bool shouldRetryProjectedSelectionAfterReady(
    bool becameReady,
    const QString& selectedThreadId,
    const QString& projectedAgentThreadId) noexcept
{
    return becameReady && !selectedThreadId.isEmpty()
           && selectedThreadId == projectedAgentThreadId;
}
}

class ConversationWidget;
class FrontendSession;
class InspectorWidget;
class SidebarWidget;
enum class ThreadAction;

class WorkbenchWidget : public QWidget
{
public:
    explicit WorkbenchWidget(FrontendSession& frontendSession, QWidget* parent = nullptr);
    ~WorkbenchWidget() override;

private:
    enum class PendingAction {
        None,
        OpenThread,
        SendExistingThread,
        SteerActiveTurn,
        InterruptTurn,
        CreateThread,
        RenameThread,
        ForkThread,
        ResumeWithOptions,
        ArchiveThread,
        UnarchiveThread,
        DeleteThread,
    };

    struct SubmittedTurnSettings {
        QString threadId;
        QString turnId;
        UpcomingTurnDraft draft;
    };

    struct PreparedTurnSubmission {
        QString userPrompt;
        QString effectivePrompt;
        QStringList imagePaths;
        QList<AttachmentInfo> attachments;
        AttachmentStagingLeasePtr stagingLease;
    };

    struct RetainedAttachmentStaging {
        QString registryId;
        QString threadId;
        QString turnId;
        AttachmentStagingLeasePtr lease;
    };

    struct TurnSubmissionPreparationRequest {
        PendingAction action = PendingAction::None;
        QString threadId;
        QString turnId;
        QString prompt;
        QList<AttachmentInfo> attachments;
        QString workspace;
        UpcomingTurnDraft settings;
        std::uint64_t expectedSelectionGeneration = 0;
    };

    struct TurnSubmissionPreparationOutcome {
        AttachmentPreparation preparation;
        QString error;
        bool success = false;
    };

    void refreshLifecycle();
    void scheduleStateRefresh(const detail::StateUpdateScope& scope);
    void refreshState(bool refreshSelectedPresentation = true,
                      bool refreshInspector = true,
                      bool refreshSidebar = true,
                      const ConversationContentUpdates* exactContentChanges = nullptr,
                      const QStringList* sidebarThreadChanges = nullptr);
    void refreshControls();
    void refreshControllerStatus();
    [[nodiscard]] bool writeOperationBusy() const noexcept;
    void selectThread(const QString& threadId);
    void selectProjectedAgentThread(const QString& threadId);
    void beginNewThread();
    void handleThreadAction(const QString& threadId, ThreadAction action);
    void showRenameThreadDialog(const QString& threadId);
    void showForkThreadDialog(const QString& threadId);
    void showResumeWithOptionsDialog(const QString& threadId);
    void showDeleteThreadConfirmation(const QString& threadId);
    void sendPrompt(const QString& prompt, bool steerRequested);
    void stopActiveTurn();
    void maybeResumeSelectedThread();
    void reconcileAutomaticResumeState();
    void ensureController();
    void executePendingAction();
    void startNewThread(const NewThreadSetup& setup, std::uint64_t expectedSelectionGeneration);
    void forkThread(const QString& threadId,
                    const ForkThreadSetup& setup,
                    std::uint64_t expectedSelectionGeneration);
    void resumeThreadWithOptions(const QString& threadId,
                                 const ResumeWithOptionsSetup& setup,
                                 std::uint64_t expectedSelectionGeneration);
    void mutateThread(PendingAction action, const QString& threadId, const QString& value = {});
    void beginTurnSubmissionPreparation(TurnSubmissionPreparationRequest request);
    void cancelAttachmentPreparation() noexcept;
    void finishTurnSubmissionPreparation(TurnSubmissionPreparationRequest request,
                                         TurnSubmissionPreparationOutcome outcome,
                                         std::uint64_t preparationGeneration);
    [[nodiscard]] std::optional<PreparedTurnSubmission> preparedTurnSubmission(
        const QString& prompt,
        const QList<AttachmentInfo>& attachments,
        AttachmentPreparation preparation);
    void resumeThread(const QString& threadId,
                      const PreparedTurnSubmission& submission,
                      const UpcomingTurnDraft& settings);
    void resumeThreadForOpen(const QString& threadId, std::uint64_t expectedSelectionGeneration);
    void startTurn(const QString& threadId,
                   const PreparedTurnSubmission& submission,
                   const UpcomingTurnDraft& settings);
    void steerTurn(const QString& threadId,
                   const QString& turnId,
                   const PreparedTurnSubmission& submission);
    void recoverAttachmentStaging();
    [[nodiscard]] bool retainAttachmentStaging(const QString& threadId,
                                               const QString& turnId,
                                               const AttachmentStagingLeasePtr& lease,
                                               QString* errorMessage = nullptr);
    void releaseAttachmentStaging(const AttachmentStagingLeasePtr& lease);
    void reconcileAttachmentStaging();
    void reconcileSubmittedTurnSettings();
    void interruptTurn(const QString& threadId, const QString& turnId);
    void showWriteError(const QString& error);
    void clearWriteTransients();
    void submitInteractiveResponse(InteractiveRequestResponse response);
    void ensureInteractiveController();
    void performInteractiveResponse();
    void clearInteractiveTransients(const QString& error = {});
    void setSidebarVisible(bool visible);
    void setInspectorVisible(bool visible);

    FrontendSession& frontendSession;
    QSplitter* splitter = nullptr;
    SidebarWidget* sidebar = nullptr;
    ConversationWidget* conversation = nullptr;
    InspectorWidget* inspector = nullptr;
    QPushButton* restoreSidebar = nullptr;
    QPushButton* restoreInspector = nullptr;
    QLabel* workspaceBreadcrumb = nullptr;
    QFrame* codexStatusDot = nullptr;
    QLabel* threadContextStatus = nullptr;
    QLabel* agentActivityStatus = nullptr;
    QLabel* synchronizationStatus = nullptr;
    QLabel* controllerStatus = nullptr;
    QLabel* attentionStatus = nullptr;
    QPushButton* attentionButton = nullptr;
    QPushButton* reconnectButton = nullptr;
    InteractiveRequestDialog* interactiveRequestDialog = nullptr;
    QString selectedThreadId;
    // Only the current selection matters to presentation reconciliation. One
    // retained identity keeps the 16 ms GUI coalescing window strictly
    // bounded even during a pathological removal burst.
    QString authoritativelyRemovedSelectedThreadId;
    QString selectedInspectorTurnId;
    QString projectedAgentThreadId;
    QString newThreadIdAwaitingState;
    QString pendingPrompt;
    QList<AttachmentInfo> pendingAttachments;
    QString pendingAttachmentWorkspace;
    QString pendingThreadId;
    QString pendingTurnId;
    QString pendingThreadValue;
    std::optional<NewThreadSetup> pendingNewThreadSetup;
    std::optional<ForkThreadSetup> pendingForkThreadSetup;
    std::optional<ResumeWithOptionsSetup> pendingResumeSetup;
    UpcomingTurnDraft pendingTurnDraft;
    std::optional<SubmittedTurnSettings> submittedTurnSettings;
    // Kept across frontend reconnects. Canonical terminal turn state is the
    // only authority that triggers cleanup. Lease destruction itself retains
    // files so closing the UI cannot break a backend turn that remains active.
    QList<RetainedAttachmentStaging> retainedAttachmentStaging;
    QString turnThreadIdAwaitingState;
    QString turnIdAwaitingState;
    QString automaticResumeThreadId;
    QSet<QString> automaticResumeAttemptedThreadIds;
    PendingAction pendingAction = PendingAction::None;
    std::uint64_t selectionGeneration = 0;
    std::uint64_t pendingSelectionGeneration = 0;
    bool controllerAcquireInFlight = false;
    bool threadStartInFlight = false;
    bool threadResumeInFlight = false;
    bool turnStartInFlight = false;
    bool turnSteerInFlight = false;
    bool attachmentPreparationInFlight = false;
    std::uint64_t attachmentPreparationGeneration = 0;
    std::shared_ptr<std::atomic_bool> attachmentPreparationCancellation;
    bool interruptInFlight = false;
    bool threadMutationInFlight = false;
    bool controllerUnavailable = false;
    std::optional<InteractiveRequestResponse> pendingInteractiveResponse;
    std::string activeInteractiveRequestId;
    bool requestControllerAcquireInFlight = false;
    bool requestResponseInFlight = false;
    bool stateRefreshPending = false;
    bool selectedPresentationRefreshPending = false;
    bool selectedPresentationFullRefreshPending = false;
    ConversationContentUpdates selectedContentRefreshPending;
    std::uint64_t selectedContentRefreshPendingBytes = 0;
    bool inspectorRefreshPending = false;
    bool sidebarRefreshPending = false;
    bool sidebarFullRefreshPending = false;
    bool frontendWasReady = false;
    QStringList sidebarThreadRefreshPending;
    QSet<QString> sidebarThreadRefreshPendingSet;
};

} // namespace codexui

#endif // CODEXUI_UI_WORKBENCHWIDGET_H
