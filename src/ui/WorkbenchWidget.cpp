// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/WorkbenchWidget.h"

#include "app/FrontendSession.h"
#include "ui/ConversationWidget.h"
#include "ui/InspectorWidget.h"
#include "ui/SidebarWidget.h"

#include <ai/openai/codex/frontend/client/State.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace codexui {
namespace {

QLabel* label(const QString& text, const char* kind = nullptr)
{
    auto* result = new QLabel(text);
    if (kind)
        result->setProperty("kind", kind);
    return result;
}

QFrame* dot(const QString& color, int size = 7)
{
    auto* result = new QFrame;
    result->setFixedSize(size, size);
    result->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(color).arg(size / 2));
    return result;
}

QWidget* makeTopBar(QPushButton*& restoreLeft, QPushButton*& restoreRight)
{
    auto* bar = new QFrame;
    bar->setObjectName(QStringLiteral("topBar"));
    bar->setStyleSheet(QStringLiteral("QFrame#topBar{background:#13161a;}"));
    bar->setFixedHeight(56);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(20, 0, 18, 0);
    row->setSpacing(12);

    auto* brand = label(QStringLiteral("CODEX WORKBENCH"), "title");
    brand->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;"));
    row->addWidget(brand);
    row->addSpacing(18);
    auto* workspace = label(QStringLiteral("Workspace  /  AISuite"), "muted");
    workspace->setStyleSheet(QStringLiteral("font-size:13px;font-weight:500;"));
    row->addWidget(workspace);

    restoreLeft = new QPushButton(QStringLiteral("Show threads"));
    restoreLeft->setProperty("kind", "subtle");
    restoreLeft->setFixedHeight(32);
    restoreLeft->hide();
    row->addSpacing(12);
    row->addWidget(restoreLeft);
    row->addStretch();

    auto* commands = new QPushButton(QStringLiteral("Commands"));
    commands->setFixedSize(132, 32);
    auto* menu = new QMenu(commands);
    menu->addAction(QStringLiteral("Command palette unavailable in Q1"));
    QObject::connect(commands, &QPushButton::clicked, commands, [commands, menu] {
        menu->popup(commands->mapToGlobal(QPoint(0, commands->height() + 4)));
    });
    row->addWidget(commands);

    auto* model = new QLabel(QStringLiteral("gpt-5.6 · high · local"));
    model->setAlignment(Qt::AlignCenter);
    model->setStyleSheet(QStringLiteral("background:#181c21;border-radius:8px;font-size:12px;font-weight:500;"));
    model->setFixedSize(210, 32);
    row->addWidget(model);

    auto* attention = new QLabel(QStringLiteral("2 attention"));
    attention->setAlignment(Qt::AlignCenter);
    attention->setStyleSheet(QStringLiteral("background:#3d2912;color:#f5a83b;border-radius:8px;font-size:11px;font-weight:600;"));
    attention->setFixedSize(106, 32);
    row->addWidget(attention);

    restoreRight = new QPushButton(QStringLiteral("Show inspector"));
    restoreRight->setProperty("kind", "subtle");
    restoreRight->setFixedHeight(32);
    restoreRight->hide();
    row->addWidget(restoreRight);
    row->addSpacing(70);
    auto* account = label(QStringLiteral("Volker"), "muted");
    account->setStyleSheet(QStringLiteral("font-size:12px;font-weight:500;"));
    row->addWidget(account);
    row->addSpacing(35);
    return bar;
}

QWidget* makeStatusBar(QFrame*& codexStatusDot, QLabel*& synchronizationStatus, QLabel*& controllerStatus)
{
    auto* bar = new QFrame;
    bar->setObjectName(QStringLiteral("customStatusBar"));
    bar->setStyleSheet(QStringLiteral("QFrame#customStatusBar{background:#181c21;}"));
    bar->setFixedHeight(40);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(18, 0, 80, 0);
    row->setSpacing(8);

    codexStatusDot = dot(QStringLiteral("#949ead"));
    row->addWidget(codexStatusDot);
    row->addWidget(label(QStringLiteral("Codex"), "meta"));
    row->addSpacing(22);
    row->addWidget(dot(QStringLiteral("#40c27d")));
    row->addWidget(label(QStringLiteral("Environment"), "meta"));
    row->addSpacing(48);
    row->addWidget(label(QStringLiteral("branch: main"), "meta"));
    row->addSpacing(62);
    row->addWidget(label(QStringLiteral("3 agents running"), "meta"));
    row->addSpacing(24);
    synchronizationStatus = label(QStringLiteral("Disconnected"), "meta");
    synchronizationStatus->setStyleSheet(QStringLiteral("color:#949ead;font-size:10px;font-weight:600;"));
    row->addWidget(synchronizationStatus);
    row->addSpacing(18);
    controllerStatus = label(QStringLiteral("Observer"), "meta");
    controllerStatus->setStyleSheet(QStringLiteral("color:#949ead;font-size:10px;font-weight:600;"));
    row->addWidget(controllerStatus);
    row->addStretch();
    auto* attention = label(QStringLiteral("2 attention"), "meta");
    attention->setStyleSheet(QStringLiteral("color:#f5a83b;font-size:10px;font-weight:600;"));
    row->addWidget(attention);
    return bar;
}

const ai::openai::codex::frontend::client::TurnState*
activeTurn(const ai::openai::codex::frontend::client::State& state,
           const ai::openai::codex::frontend::client::ThreadState* thread)
{
    if (!thread)
        return nullptr;
    for (auto iterator = thread->orderedTurns.rbegin(); iterator != thread->orderedTurns.rend(); ++iterator) {
        const auto* turn = state.turn(*iterator);
        if (turn && turn->active && !turn->terminal)
            return turn;
    }
    return nullptr;
}

} // namespace

WorkbenchWidget::WorkbenchWidget(FrontendSession& session, QWidget* parent)
    : QWidget(parent)
    , frontendSession(session)
{
    setObjectName(QStringLiteral("workbench"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makeTopBar(restoreSidebar, restoreInspector));

    splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);
    sidebar = new SidebarWidget;
    conversation = new ConversationWidget;
    inspector = new InspectorWidget;
    splitter->addWidget(sidebar);
    splitter->addWidget(conversation);
    splitter->addWidget(inspector);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({282, 834, 404});
    layout->addWidget(splitter, 1);
    layout->addWidget(makeStatusBar(codexStatusDot, synchronizationStatus, controllerStatus));

    connect(sidebar, &SidebarWidget::hideRequested, this, [this] { setSidebarVisible(false); });
    connect(inspector, &InspectorWidget::hideRequested, this, [this] { setInspectorVisible(false); });
    connect(restoreSidebar, &QPushButton::clicked, this, [this] { setSidebarVisible(true); });
    connect(restoreInspector, &QPushButton::clicked, this, [this] { setInspectorVisible(true); });
    connect(sidebar, &SidebarWidget::newThreadRequested, this, &WorkbenchWidget::beginNewThread);
    connect(sidebar, &SidebarWidget::threadSelected, this, &WorkbenchWidget::selectThread);
    connect(conversation, &ConversationWidget::sendRequested, this, &WorkbenchWidget::sendPrompt);
    connect(conversation, &ConversationWidget::stopRequested, this, &WorkbenchWidget::stopActiveTurn);
    connect(&frontendSession, &FrontendSession::lifecycleChanged, this, &WorkbenchWidget::refreshLifecycle);
    connect(&frontendSession, &FrontendSession::stateChanged, this, &WorkbenchWidget::refreshState);

    refreshLifecycle();
    refreshState();
}

void WorkbenchWidget::refreshLifecycle()
{
    using Lifecycle = FrontendSession::Lifecycle;
    QString color = QStringLiteral("#949ead");
    QString title = QStringLiteral("App server disconnected");
    QString detail = frontendSession.statusText();

    switch (frontendSession.lifecycle()) {
        case Lifecycle::Connecting:
        case Lifecycle::Authenticating:
        case Lifecycle::Synchronizing:
            color = QStringLiteral("#4f94f5");
            title = QStringLiteral("Connecting to app server");
            break;
        case Lifecycle::Ready:
            color = QStringLiteral("#40c27d");
            title = QStringLiteral("App server connected");
            detail = QStringLiteral("Local · Unix · synchronized");
            break;
        case Lifecycle::Failed:
            color = QStringLiteral("#f5a83b");
            title = QStringLiteral("App server unavailable");
            break;
        case Lifecycle::Disconnected:
            break;
    }

    sidebar->setConnectionStatus(title, detail, color);
    codexStatusDot->setStyleSheet(QStringLiteral("background:%1;border-radius:3px;").arg(color));
    synchronizationStatus->setText(frontendSession.statusText());
    synchronizationStatus->setToolTip(frontendSession.statusText());
    synchronizationStatus->setStyleSheet(
        QStringLiteral("color:%1;font-size:10px;font-weight:600;").arg(color));

    if (frontendSession.lifecycle() != Lifecycle::Ready) {
        const bool writeWasPending = pendingAction != PendingAction::None || controllerAcquireInFlight
                                     || threadStartInFlight || threadResumeInFlight || turnStartInFlight
                                     || interruptInFlight;
        clearWriteTransients();
        if (writeWasPending)
            showWriteError(QStringLiteral("Backend disconnected before the write completed"));
    }
    refreshControllerStatus();
    refreshControls();
}

void WorkbenchWidget::refreshState()
{
    const auto& state = frontendSession.state();
    const auto threads = state.threads();

    if (!newThreadIdAwaitingState.isEmpty()
        && state.thread(newThreadIdAwaitingState.toStdString()) != nullptr) {
        selectedThreadId = newThreadIdAwaitingState;
        newThreadIdAwaitingState.clear();
        newThreadDraft = false;
    }
    if (!selectedThreadId.isEmpty()
        && state.thread(selectedThreadId.toStdString()) == nullptr)
        selectedThreadId.clear();
    if (!newThreadDraft && selectedThreadId.isEmpty() && !threads.empty())
        selectedThreadId = QString::fromStdString(threads.front().id.value);

    sidebar->setThreads(state, selectedThreadId);

    // ConversationWidget resolves the stable selection against this exact
    // immutable State and never retains backend object addresses.
    conversation->render(state, selectedThreadId, newThreadDraft);

    const auto* selected = selectedThreadId.isEmpty() ? nullptr : state.thread(selectedThreadId.toStdString());
    if (selected && !selected->fullyLoaded)
        frontendSession.loadThread(selectedThreadId);

    controllerUnavailable = controllerUnavailable && !frontendSession.ownsController();
    refreshControllerStatus();
    if (pendingAction != PendingAction::None && frontendSession.ownsController())
        executePendingAction();
    refreshControls();
}

void WorkbenchWidget::selectThread(const QString& threadId)
{
    selectedThreadId = threadId;
    newThreadDraft = false;
    conversation->setWriteStatus({});
    refreshState();
}

void WorkbenchWidget::refreshControls()
{
    const bool ready = frontendSession.lifecycle() == FrontendSession::Lifecycle::Ready;
    const auto& state = frontendSession.state();
    const auto* selected = !newThreadDraft && !selectedThreadId.isEmpty()
                               ? state.thread(selectedThreadId.toStdString())
                               : nullptr;
    const auto* active = activeTurn(state, selected);
    const bool pendingControllerWrite = controllerAcquireInFlight || pendingAction != PendingAction::None;
    const bool promptSubmissionInFlight = threadStartInFlight || threadResumeInFlight || turnStartInFlight;

    sidebar->setNewThreadEnabled(ready && !promptSubmissionInFlight && !pendingControllerWrite);
    conversation->setActionState(ready && (newThreadDraft || selected != nullptr) && active == nullptr
                                     && !promptSubmissionInFlight && !pendingControllerWrite,
                                 ready && selected != nullptr && active != nullptr && !interruptInFlight
                                     && !pendingControllerWrite,
                                 ready && !promptSubmissionInFlight && !controllerAcquireInFlight);
}

void WorkbenchWidget::refreshControllerStatus()
{
    QString text = QStringLiteral("Observer");
    QString color = QStringLiteral("#949ead");
    QString tooltip = QStringLiteral("Writes acquire controller ownership when needed");
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready) {
        tooltip = QStringLiteral("Controller state is unavailable while disconnected");
    } else if (frontendSession.ownsController()) {
        text = QStringLiteral("Controller");
        color = QStringLiteral("#40c27d");
        tooltip = QStringLiteral("This frontend owns controller");
    } else if (controllerUnavailable) {
        text = QStringLiteral("Controller unavailable");
        color = QStringLiteral("#f5a83b");
        tooltip = QStringLiteral("The last controller acquisition was rejected");
    } else {
        const auto& projection = frontendSession.state().controller();
        if (projection.value && projection.value->present)
            tooltip = QStringLiteral("Another frontend currently owns controller");
    }
    controllerStatus->setText(text);
    controllerStatus->setToolTip(tooltip);
    controllerStatus->setStyleSheet(
        QStringLiteral("color:%1;font-size:10px;font-weight:600;").arg(color));
}

void WorkbenchWidget::beginNewThread()
{
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready || threadStartInFlight)
        return;
    selectedThreadId.clear();
    newThreadDraft = true;
    newThreadIdAwaitingState.clear();
    conversation->setWriteStatus({});
    refreshState();
    conversation->focusComposer();
}

void WorkbenchWidget::sendPrompt(const QString& prompt)
{
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready || prompt.trimmed().isEmpty()
        || threadStartInFlight || threadResumeInFlight || turnStartInFlight || controllerAcquireInFlight
        || pendingAction != PendingAction::None)
        return;

    const auto& state = frontendSession.state();
    const auto* selected = selectedThreadId.isEmpty() ? nullptr : state.thread(selectedThreadId.toStdString());
    if (!newThreadDraft && (!selected || activeTurn(state, selected)))
        return;

    pendingAction = newThreadDraft ? PendingAction::SendNewThread : PendingAction::SendExistingThread;
    pendingPrompt = prompt;
    pendingThreadId = newThreadDraft ? QString{} : selectedThreadId;
    pendingTurnId.clear();
    conversation->setWriteStatus(QStringLiteral("Preparing write…"));
    ensureController();
}

void WorkbenchWidget::stopActiveTurn()
{
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready || interruptInFlight
        || controllerAcquireInFlight || pendingAction != PendingAction::None || selectedThreadId.isEmpty())
        return;
    const auto& state = frontendSession.state();
    const auto* selected = state.thread(selectedThreadId.toStdString());
    const auto* turn = activeTurn(state, selected);
    if (!turn)
        return;

    pendingAction = PendingAction::InterruptTurn;
    pendingThreadId = selectedThreadId;
    pendingTurnId = QString::fromStdString(turn->id.value);
    pendingPrompt.clear();
    conversation->setWriteStatus(QStringLiteral("Preparing interrupt…"));
    ensureController();
}

void WorkbenchWidget::ensureController()
{
    if (frontendSession.ownsController()) {
        executePendingAction();
        return;
    }
    if (controllerAcquireInFlight)
        return;

    controllerAcquireInFlight = true;
    controllerUnavailable = false;
    conversation->setWriteStatus(QStringLiteral("Acquiring controller…"));
    refreshControllerStatus();
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.acquireController([self](const QString& error) {
        if (!self)
            return;
        self->controllerAcquireInFlight = false;
        if (self->frontendSession.ownsController()) {
            self->controllerUnavailable = false;
            self->executePendingAction();
        } else if (!error.isEmpty()) {
            self->pendingAction = PendingAction::None;
            self->pendingPrompt.clear();
            self->pendingThreadId.clear();
            self->pendingTurnId.clear();
            self->controllerUnavailable = true;
            self->showWriteError(error);
        } else {
            self->conversation->setWriteStatus(QStringLiteral("Waiting for controller state…"));
        }
        self->refreshControllerStatus();
        self->refreshControls();
    });
    if (immediateError) {
        controllerAcquireInFlight = false;
        pendingAction = PendingAction::None;
        pendingPrompt.clear();
        pendingThreadId.clear();
        pendingTurnId.clear();
        controllerUnavailable = true;
        showWriteError(*immediateError);
        refreshControllerStatus();
        refreshControls();
    }
}

void WorkbenchWidget::executePendingAction()
{
    if (!frontendSession.ownsController() || pendingAction == PendingAction::None)
        return;

    const PendingAction action = pendingAction;
    const QString prompt = pendingPrompt;
    const QString threadId = pendingThreadId;
    const QString turnId = pendingTurnId;
    pendingAction = PendingAction::None;
    pendingPrompt.clear();
    pendingThreadId.clear();
    pendingTurnId.clear();

    switch (action) {
        case PendingAction::SendExistingThread:
            resumeThread(threadId, prompt);
            break;
        case PendingAction::SendNewThread:
            startNewThread(prompt);
            break;
        case PendingAction::InterruptTurn:
            interruptTurn(threadId, turnId);
            break;
        case PendingAction::None:
            break;
    }
}

void WorkbenchWidget::startNewThread(const QString& prompt)
{
    threadStartInFlight = true;
    conversation->setWriteStatus(QStringLiteral("Creating thread…"));
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.startThread(
        [self, prompt](const QString& threadId, const QString& error) {
            if (!self)
                return;
            self->threadStartInFlight = false;
            if (!error.isEmpty()) {
                self->showWriteError(error);
                self->refreshControls();
                return;
            }
            self->newThreadIdAwaitingState = threadId;
            self->startTurn(threadId, prompt);
            self->refreshState();
        });
    if (immediateError) {
        threadStartInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::startTurn(const QString& threadId, const QString& prompt)
{
    turnStartInFlight = true;
    conversation->setWriteStatus(QStringLiteral("Starting turn…"));
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.startTurn(threadId, prompt, [self](const QString& error) {
        if (!self)
            return;
        self->turnStartInFlight = false;
        if (!error.isEmpty())
            self->showWriteError(error);
        else
            self->conversation->setWriteStatus({});
        self->refreshState();
    });
    if (immediateError) {
        turnStartInFlight = false;
        showWriteError(*immediateError);
        refreshState();
        return;
    }

    // The prompt crossed the typed turn.start submission boundary. Canonical
    // State, not this command result, supplies the visible user message.
    conversation->clearPrompt();
    conversation->setWriteStatus(QStringLiteral("Prompt submitted"));
    refreshControls();
}

void WorkbenchWidget::resumeThread(const QString& threadId, const QString& prompt)
{
    threadResumeInFlight = true;
    conversation->setWriteStatus(QStringLiteral("Attaching thread…"));
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.resumeThread(
        threadId,
        [self, prompt](const QString& resumedThreadId, const QString& error) {
            if (!self)
                return;
            self->threadResumeInFlight = false;
            if (!error.isEmpty()) {
                self->showWriteError(error);
                self->refreshControls();
                return;
            }
            self->startTurn(resumedThreadId, prompt);
        });
    if (immediateError) {
        threadResumeInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::interruptTurn(const QString& threadId, const QString& turnId)
{
    interruptInFlight = true;
    conversation->setWriteStatus(QStringLiteral("Stopping turn…"));
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.interruptTurn(threadId, turnId, [self](const QString& error) {
        if (!self)
            return;
        self->interruptInFlight = false;
        if (!error.isEmpty())
            self->showWriteError(error);
        else
            self->conversation->setWriteStatus({});
        self->refreshState();
    });
    if (immediateError) {
        interruptInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::showWriteError(const QString& error)
{
    conversation->setWriteStatus(error.isEmpty() ? QStringLiteral("Write operation failed") : error, true);
}

void WorkbenchWidget::clearWriteTransients()
{
    pendingAction = PendingAction::None;
    pendingPrompt.clear();
    pendingThreadId.clear();
    pendingTurnId.clear();
    newThreadIdAwaitingState.clear();
    controllerAcquireInFlight = false;
    threadStartInFlight = false;
    threadResumeInFlight = false;
    turnStartInFlight = false;
    interruptInFlight = false;
    controllerUnavailable = false;
}

void WorkbenchWidget::setSidebarVisible(bool visible)
{
    sidebar->setVisible(visible);
    restoreSidebar->setVisible(!visible);
    if (visible)
        splitter->setSizes({282, qMax(500, splitter->width() - 694), 404});
}

void WorkbenchWidget::setInspectorVisible(bool visible)
{
    inspector->setVisible(visible);
    restoreInspector->setVisible(!visible);
    if (visible)
        splitter->setSizes({282, qMax(500, splitter->width() - 694), 404});
}

} // namespace codexui
