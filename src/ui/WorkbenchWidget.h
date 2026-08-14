// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_WORKBENCHWIDGET_H
#define CODEXUI_UI_WORKBENCHWIDGET_H

#include <QWidget>
#include <QString>

class QFrame;
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
    void refreshState();
    void refreshControls();
    void refreshControllerStatus();
    void selectThread(const QString& threadId);
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
    QLabel* synchronizationStatus = nullptr;
    QLabel* controllerStatus = nullptr;
    QString selectedThreadId;
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
};

} // namespace codexui

#endif // CODEXUI_UI_WORKBENCHWIDGET_H
