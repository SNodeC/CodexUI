// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_WORKBENCHWIDGET_H
#define CODEXUI_UI_WORKBENCHWIDGET_H

#include "ui/InteractiveRequestDialog.h"
#include "ui/ThreadSetupDialog.h"
#include "ui/UpcomingTurnDock.h"

#include <QWidget>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <cstdint>

class QFrame;
class QLabel;
class QPushButton;
class QSplitter;

namespace codexui {

namespace detail {
struct StateUpdateScope;
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

private:
    enum class PendingAction {
        None,
        OpenThread,
        SendExistingThread,
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

    void refreshLifecycle();
    void scheduleStateRefresh(const detail::StateUpdateScope& scope);
    void refreshState(bool refreshSelectedPresentation = true,
                      bool refreshInspector = true,
                      bool refreshSidebar = true,
                      const QHash<QString, QStringList>* exactContentChanges = nullptr);
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
    void sendPrompt(const QString& prompt);
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
    void resumeThread(const QString& threadId, const QString& prompt, const UpcomingTurnDraft& settings);
    void resumeThreadForOpen(const QString& threadId, std::uint64_t expectedSelectionGeneration);
    void startTurn(const QString& threadId, const QString& prompt, const UpcomingTurnDraft& settings);
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
    QString selectedInspectorTurnId;
    QString projectedAgentThreadId;
    QString newThreadIdAwaitingState;
    QString pendingPrompt;
    QString pendingThreadId;
    QString pendingTurnId;
    QString pendingThreadValue;
    std::optional<NewThreadSetup> pendingNewThreadSetup;
    std::optional<ForkThreadSetup> pendingForkThreadSetup;
    std::optional<ResumeWithOptionsSetup> pendingResumeSetup;
    UpcomingTurnDraft pendingTurnDraft;
    std::optional<SubmittedTurnSettings> submittedTurnSettings;
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
    QHash<QString, QStringList> selectedContentRefreshPending;
    bool inspectorRefreshPending = false;
    bool sidebarRefreshPending = false;
};

} // namespace codexui

#endif // CODEXUI_UI_WORKBENCHWIDGET_H
