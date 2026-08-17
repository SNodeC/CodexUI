// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_WORKBENCHWIDGET_H
#define CODEXUI_UI_WORKBENCHWIDGET_H

#include "ui/InteractiveRequestDialog.h"

#include <QWidget>
#include <QString>
#include <QStringList>

#include <optional>

class QFrame;
class QAction;
class QLabel;
class QPushButton;
class QSplitter;

namespace codexui {

class ConversationWidget;
class FrontendSession;
class InspectorWidget;
class SidebarWidget;

class WorkbenchWidget : public QWidget
{
public:
    explicit WorkbenchWidget(FrontendSession& frontendSession, QWidget* parent = nullptr);

private:
    enum class PendingAction { None, SendExistingThread, SendNewThread, InterruptTurn };

    void refreshLifecycle();
    void scheduleStateRefresh(const QStringList& affectedThreadIds,
                              bool allThreadsAffected,
                              bool inspectorAffected);
    void refreshState(bool refreshSelectedPresentation = true, bool refreshInspector = true);
    void refreshControls();
    void refreshControllerStatus();
    void selectThread(const QString& threadId);
    void selectProjectedAgentThread(const QString& threadId);
    void beginNewThread();
    void sendPrompt(const QString& prompt);
    void stopActiveTurn();
    void ensureController();
    void executePendingAction();
    void startNewThread(const QString& prompt);
    void resumeThread(const QString& threadId, const QString& prompt);
    void startTurn(const QString& threadId, const QString& prompt);
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
    QFrame* codexStatusDot = nullptr;
    QLabel* modelStatus = nullptr;
    QLabel* threadContextStatus = nullptr;
    QLabel* agentActivityStatus = nullptr;
    QLabel* synchronizationStatus = nullptr;
    QLabel* controllerStatus = nullptr;
    QLabel* attentionStatus = nullptr;
    QPushButton* attentionButton = nullptr;
    QAction* reconnectAction = nullptr;
    InteractiveRequestDialog* interactiveRequestDialog = nullptr;
    QString selectedThreadId;
    QString projectedAgentThreadId;
    QString newThreadIdAwaitingState;
    QString pendingPrompt;
    QString pendingThreadId;
    QString pendingTurnId;
    PendingAction pendingAction = PendingAction::None;
    bool newThreadDraft = false;
    bool controllerAcquireInFlight = false;
    bool threadStartInFlight = false;
    bool threadResumeInFlight = false;
    bool turnStartInFlight = false;
    bool interruptInFlight = false;
    bool controllerUnavailable = false;
    std::optional<InteractiveRequestResponse> pendingInteractiveResponse;
    std::string activeInteractiveRequestId;
    bool requestControllerAcquireInFlight = false;
    bool requestResponseInFlight = false;
    bool stateRefreshPending = false;
    bool selectedPresentationRefreshPending = false;
    bool inspectorRefreshPending = false;
};

} // namespace codexui

#endif // CODEXUI_UI_WORKBENCHWIDGET_H
