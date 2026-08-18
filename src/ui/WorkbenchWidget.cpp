// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/WorkbenchWidget.h"

#include "app/FrontendSession.h"
#include "ui/ConversationWidget.h"
#include "ui/InspectorWidget.h"
#include "ui/InteractiveRequestDialog.h"
#include "ui/SidebarWidget.h"

#include <ai/openai/codex/frontend/client/State.h>

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <variant>

namespace codexui {
namespace {

void setStyleSheetIfChanged(QWidget* widget, const QString& styleSheet)
{
    if (widget->styleSheet() != styleSheet)
        widget->setStyleSheet(styleSheet);
}

std::optional<QString> interactiveResponseValidationError(
    const ai::openai::codex::frontend::client::State& state,
    const InteractiveRequestResponse& response)
{
    const auto source = detail::interactiveRequestSource(state, response.requestId);
    if (!source)
        return QStringLiteral("This request is no longer pending");
    if (source->request.kind != response.kind)
        return QStringLiteral("This request changed; review it and retry");
    if (*source != response.source)
        return QStringLiteral("This request changed; review it and retry");
    const auto safety = detail::interactiveRequestResponseSafety(*source);
    if (safety == InteractiveRequestResponseSafety::Disabled
        || (safety == InteractiveRequestResponseSafety::NegativeOnly
            && !detail::interactiveResponseIsNegative(response)))
        return QStringLiteral("This request is incomplete and cannot be safely answered");
    return std::nullopt;
}

QLabel* label(const QString& text, const char* kind = nullptr)
{
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    if (kind)
        result->setProperty("kind", kind);
    return result;
}

QString plainTooltip(const QString& text)
{
    return Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal);
}

QFrame* dot(const QString& color, int size = 7)
{
    auto* result = new QFrame;
    result->setFixedSize(size, size);
    result->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(color).arg(size / 2));
    return result;
}

QWidget* makeTopBar(QPushButton*& restoreLeft,
                    QPushButton*& restoreRight,
                    QPushButton*& attention,
                    QLabel*& modelStatus,
                    QAction*& reconnectAction)
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
    reconnectAction = menu->addAction(QStringLiteral("Reconnect to app server"));
    menu->addSeparator();
    menu->addAction(QStringLiteral("Command palette is not implemented"));
    QObject::connect(commands, &QPushButton::clicked, commands, [commands, menu] {
        menu->popup(commands->mapToGlobal(QPoint(0, commands->height() + 4)));
    });
    row->addWidget(commands);

    modelStatus = label(QStringLiteral("Model unavailable"));
    modelStatus->setAlignment(Qt::AlignCenter);
    modelStatus->setStyleSheet(QStringLiteral("background:#181c21;border-radius:8px;font-size:12px;font-weight:500;"));
    modelStatus->setFixedSize(210, 32);
    row->addWidget(modelStatus);

    attention = new QPushButton(QStringLiteral("0 requests"));
    attention->setFixedSize(106, 32);
    row->addWidget(attention);

    restoreRight = new QPushButton(QStringLiteral("Show inspector"));
    restoreRight->setProperty("kind", "subtle");
    restoreRight->setFixedHeight(32);
    restoreRight->hide();
    row->addWidget(restoreRight);
    row->addSpacing(70);
    auto* account = label(QStringLiteral("CodexUI"), "muted");
    account->setStyleSheet(QStringLiteral("font-size:12px;font-weight:500;"));
    row->addWidget(account);
    row->addSpacing(35);
    return bar;
}

QWidget* makeStatusBar(QFrame*& codexStatusDot,
                       QLabel*& threadContextStatus,
                       QLabel*& agentActivityStatus,
                       QLabel*& synchronizationStatus,
                       QLabel*& controllerStatus,
                       QLabel*& attentionStatus)
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
    row->addSpacing(48);
    threadContextStatus = label(QStringLiteral("No thread context"), "meta");
    row->addWidget(threadContextStatus);
    row->addSpacing(62);
    agentActivityStatus = label(QStringLiteral("No agent activity"), "meta");
    row->addWidget(agentActivityStatus);
    row->addSpacing(24);
    synchronizationStatus = label(QStringLiteral("Disconnected"), "meta");
    synchronizationStatus->setStyleSheet(QStringLiteral("color:#949ead;font-size:10px;font-weight:600;"));
    row->addWidget(synchronizationStatus);
    row->addSpacing(18);
    controllerStatus = label(QStringLiteral("Observer"), "meta");
    controllerStatus->setStyleSheet(QStringLiteral("color:#949ead;font-size:10px;font-weight:600;"));
    row->addWidget(controllerStatus);
    row->addStretch();
    attentionStatus = label(QStringLiteral("0 requests"), "meta");
    attentionStatus->setStyleSheet(QStringLiteral("color:#949ead;font-size:10px;font-weight:600;"));
    row->addWidget(attentionStatus);
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

const ai::openai::codex::frontend::client::TurnState*
latestTurn(const ai::openai::codex::frontend::client::State& state,
           const ai::openai::codex::frontend::client::ThreadState* thread)
{
    if (!thread)
        return nullptr;
    for (auto iterator = thread->orderedTurns.rbegin(); iterator != thread->orderedTurns.rend(); ++iterator) {
        if (const auto* turn = state.turn(*iterator))
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
    layout->addWidget(makeTopBar(restoreSidebar, restoreInspector, attentionButton, modelStatus, reconnectAction));

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
    layout->addWidget(makeStatusBar(codexStatusDot, threadContextStatus, agentActivityStatus,
                                    synchronizationStatus, controllerStatus, attentionStatus));

    interactiveRequestDialog = new InteractiveRequestDialog(
        [this]() -> const ai::openai::codex::frontend::client::State& { return frontendSession.state(); },
        [this](InteractiveRequestResponse response) { submitInteractiveResponse(std::move(response)); },
        this);

    connect(sidebar, &SidebarWidget::hideRequested, this, [this] { setSidebarVisible(false); });
    connect(inspector, &InspectorWidget::hideRequested, this, [this] { setInspectorVisible(false); });
    connect(restoreSidebar, &QPushButton::clicked, this, [this] { setSidebarVisible(true); });
    connect(restoreInspector, &QPushButton::clicked, this, [this] { setInspectorVisible(true); });
    connect(attentionButton, &QPushButton::clicked, interactiveRequestDialog, &InteractiveRequestDialog::present);
    connect(reconnectAction, &QAction::triggered, &frontendSession, &FrontendSession::reconnectToBackend);
    connect(sidebar, &SidebarWidget::newThreadRequested, this, &WorkbenchWidget::beginNewThread);
    connect(sidebar, &SidebarWidget::threadSelected, this, &WorkbenchWidget::selectThread);
    connect(inspector, &InspectorWidget::selectionChanged, this, [this] { refreshState(); });
    connect(inspector, &InspectorWidget::threadOpenRequested, this, &WorkbenchWidget::selectProjectedAgentThread);
    connect(conversation, &ConversationWidget::sendRequested, this, &WorkbenchWidget::sendPrompt);
    connect(conversation, &ConversationWidget::stopRequested, this, &WorkbenchWidget::stopActiveTurn);
    connect(&frontendSession, &FrontendSession::lifecycleChanged, this, &WorkbenchWidget::refreshLifecycle);
    connect(&frontendSession, &FrontendSession::statusChanged, this, &WorkbenchWidget::refreshLifecycle);
    connect(&frontendSession, &FrontendSession::stateChanged, this, &WorkbenchWidget::scheduleStateRefresh);

    refreshLifecycle();
    refreshState();
}

void WorkbenchWidget::scheduleStateRefresh(const QStringList& affectedThreadIds,
                                           bool allThreadsAffected,
                                           bool inspectorAffected)
{
    const bool selectedAffected = allThreadsAffected
                                  || affectedThreadIds.contains(selectedThreadId)
                                  || (!newThreadIdAwaitingState.isEmpty()
                                      && affectedThreadIds.contains(newThreadIdAwaitingState));
    selectedPresentationRefreshPending = selectedPresentationRefreshPending || selectedAffected;
    inspectorRefreshPending = inspectorRefreshPending || inspectorAffected || selectedAffected;
    if (stateRefreshPending)
        return;
    stateRefreshPending = true;
    // Let adjacent bounded socket batches collapse into one presentation pass
    // while still updating live output at approximately one frame cadence.
    QTimer::singleShot(16, this, [this] {
        if (!stateRefreshPending)
            return;
        stateRefreshPending = false;
        const bool refreshSelectedPresentation = selectedPresentationRefreshPending;
        const bool refreshInspector = inspectorRefreshPending;
        selectedPresentationRefreshPending = false;
        inspectorRefreshPending = false;
        refreshState(refreshSelectedPresentation, refreshInspector);
    });
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

    reconnectAction->setEnabled(frontendSession.lifecycle() == Lifecycle::Disconnected
                                || frontendSession.lifecycle() == Lifecycle::Failed);

    sidebar->setConnectionStatus(title, detail, color);
    setStyleSheetIfChanged(codexStatusDot, QStringLiteral("background:%1;border-radius:3px;").arg(color));
    synchronizationStatus->setText(frontendSession.statusText());
    synchronizationStatus->setToolTip(plainTooltip(frontendSession.statusText()));
    setStyleSheetIfChanged(synchronizationStatus,
                           QStringLiteral("color:%1;font-size:10px;font-weight:600;").arg(color));
    if (frontendSession.lifecycle() != Lifecycle::Ready) {
        modelStatus->setText(QStringLiteral("Model unavailable"));
        modelStatus->setToolTip({});
        threadContextStatus->setText(QStringLiteral("No thread context"));
        threadContextStatus->setToolTip({});
        agentActivityStatus->setText(QStringLiteral("No agent activity"));
        inspector->render(frontendSession.state(), {}, false, frontendSession.statusText());
    }

    if (frontendSession.lifecycle() != Lifecycle::Ready) {
        const bool writeWasPending = pendingAction != PendingAction::None || controllerAcquireInFlight
                                     || threadStartInFlight || threadResumeInFlight || turnStartInFlight
                                     || interruptInFlight;
        clearWriteTransients();
        if (writeWasPending)
            showWriteError(QStringLiteral("Backend disconnected before the write completed"));
        if (requestControllerAcquireInFlight || requestResponseInFlight || pendingInteractiveResponse)
            clearInteractiveTransients(QStringLiteral("Backend disconnected before the response completed"));
    }
    refreshControllerStatus();
    refreshControls();
}

void WorkbenchWidget::refreshState(bool refreshSelectedPresentation, bool refreshInspector)
{
    stateRefreshPending = false;
    selectedPresentationRefreshPending = false;
    inspectorRefreshPending = false;
    const auto& state = frontendSession.state();
    const auto threads = state.threads();
    const bool ready = frontendSession.lifecycle() == FrontendSession::Lifecycle::Ready;
    const bool threadListComplete = state.threadList().value && state.threadList().value->complete;
    const QString previousThreadId = selectedThreadId;
    const bool previousNewThreadDraft = newThreadDraft;

    if (!newThreadIdAwaitingState.isEmpty()
        && state.thread(newThreadIdAwaitingState.toStdString()) != nullptr) {
        selectedThreadId = newThreadIdAwaitingState;
        newThreadIdAwaitingState.clear();
        newThreadDraft = false;
    }
    if (!selectedThreadId.isEmpty()
        && state.thread(selectedThreadId.toStdString()) == nullptr && ready && threadListComplete)
        selectedThreadId.clear();
    if (!newThreadDraft && selectedThreadId.isEmpty() && !threads.empty())
        selectedThreadId = QString::fromStdString(threads.front().id.value);
    const bool selectionChanged = previousThreadId != selectedThreadId
                                  || previousNewThreadDraft != newThreadDraft;
    refreshSelectedPresentation = refreshSelectedPresentation || selectionChanged;
    refreshInspector = refreshInspector || refreshSelectedPresentation;

    sidebar->setThreads(state, selectedThreadId);

    // ConversationWidget resolves the stable selection against this exact
    // immutable State and never retains backend object addresses.
    if (refreshSelectedPresentation)
        conversation->render(state, selectedThreadId, newThreadDraft);

    if (refreshInspector)
        inspector->render(state, newThreadDraft ? QString{} : selectedThreadId,
                          ready, frontendSession.statusText());

    const auto* selected = !newThreadDraft && !selectedThreadId.isEmpty()
                               ? state.thread(selectedThreadId.toStdString())
                               : nullptr;
    if (refreshSelectedPresentation) {
        QStringList modelDetails;
        if (ready && selected && selected->model)
            modelDetails.append(QString::fromStdString(selected->model->value));
        if (ready && selected && selected->modelProvider)
            modelDetails.append(QString::fromStdString(*selected->modelProvider));
        modelStatus->setText(modelDetails.isEmpty() ? QStringLiteral("Model unavailable")
                                                    : modelDetails.join(QStringLiteral(" · ")));
        modelStatus->setToolTip(plainTooltip(modelStatus->text()));

        const QString context = ready && selected && selected->cwd
                                    ? QString::fromStdString(selected->cwd->value)
                                    : QStringLiteral("No thread context");
        threadContextStatus->setText(context.size() > 44
                                         ? context.left(20) + QChar(0x2026) + context.right(20)
                                         : context);
        threadContextStatus->setToolTip(ready && selected && selected->cwd ? plainTooltip(context) : QString{});

        std::size_t agentActivities = 0;
        if (const auto* turn = ready ? latestTurn(state, selected) : nullptr) {
            for (const auto& itemId : turn->orderedItems) {
                const auto* item = state.item(selected->id, turn->id, itemId);
                if (!item)
                    continue;
                const auto semantic = ai::openai::codex::frontend::client::itemSemanticView(*item);
                if (semantic
                    && (std::holds_alternative<
                            ai::openai::codex::frontend::client::SubAgentActivitySemanticView>(semantic->details)
                        || std::holds_alternative<
                            ai::openai::codex::frontend::client::CollabAgentToolCallSemanticView>(semantic->details)))
                    ++agentActivities;
            }
        }
        agentActivityStatus->setText(agentActivities == 0
                                         ? QStringLiteral("No agent activity")
                                         : QStringLiteral("%1 agent activit%2")
                                               .arg(agentActivities)
                                               .arg(agentActivities == 1 ? QStringLiteral("y")
                                                                        : QStringLiteral("ies")));
    }

    const std::size_t attentionCount = state.hasPendingRequestProjection() ? state.pendingRequests().size() : 0;
    const QString attentionText = QStringLiteral("%1 request%2")
                                      .arg(attentionCount)
                                      .arg(attentionCount == 1 ? QString{} : QStringLiteral("s"));
    attentionButton->setText(attentionText);
    attentionStatus->setText(attentionText);
    const bool needsAttention = attentionCount > 0;
    setStyleSheetIfChanged(
        attentionButton,
        needsAttention
            ? QStringLiteral("background:#3d2912;color:#f5a83b;border-radius:8px;font-size:11px;font-weight:600;")
            : QStringLiteral("background:#181c21;color:#949ead;border-radius:8px;font-size:11px;font-weight:600;"));
    setStyleSheetIfChanged(
        attentionStatus,
        QStringLiteral("color:%1;font-size:10px;font-weight:600;")
            .arg(needsAttention ? QStringLiteral("#f5a83b") : QStringLiteral("#949ead")));
    attentionButton->setToolTip(needsAttention ? QStringLiteral("Open pending Codex requests")
                                               : QStringLiteral("Codex has no pending requests"));
    interactiveRequestDialog->synchronize(state);

    if (selected && !selected->fullyLoaded && projectedAgentThreadId != selectedThreadId)
        frontendSession.loadThread(selectedThreadId);

    controllerUnavailable = controllerUnavailable && !frontendSession.ownsController();
    refreshControllerStatus();
    if (pendingAction != PendingAction::None && frontendSession.ownsController())
        executePendingAction();
    if (pendingInteractiveResponse && !requestControllerAcquireInFlight && !requestResponseInFlight
        && frontendSession.ownsController())
        performInteractiveResponse();
    refreshControls();
}

void WorkbenchWidget::selectThread(const QString& threadId)
{
    selectedThreadId = threadId;
    projectedAgentThreadId.clear();
    newThreadDraft = false;
    conversation->setWriteStatus({});
    refreshState();
}

void WorkbenchWidget::selectProjectedAgentThread(const QString& threadId)
{
    selectedThreadId = threadId;
    projectedAgentThreadId = threadId;
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
    const bool pendingControllerWrite = controllerAcquireInFlight || pendingAction != PendingAction::None
                                        || requestControllerAcquireInFlight || requestResponseInFlight;
    const bool promptSubmissionInFlight = threadStartInFlight || threadResumeInFlight || turnStartInFlight;

    sidebar->setNewThreadEnabled(ready && !promptSubmissionInFlight && !pendingControllerWrite);
    conversation->setActionState(ready && (newThreadDraft || (selected != nullptr && selected->fullyLoaded))
                                     && active == nullptr
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
    controllerStatus->setToolTip(plainTooltip(tooltip));
    setStyleSheetIfChanged(controllerStatus,
                           QStringLiteral("color:%1;font-size:10px;font-weight:600;").arg(color));
}

void WorkbenchWidget::submitInteractiveResponse(InteractiveRequestResponse response)
{
    const std::string id = response.requestId.value;
    if (const auto error = interactiveResponseValidationError(frontendSession.state(), response)) {
        interactiveRequestDialog->responseFailed(id, *error);
        return;
    }
    if (requestControllerAcquireInFlight || requestResponseInFlight || pendingInteractiveResponse) {
        interactiveRequestDialog->responseFailed(id, QStringLiteral("Another response is already being submitted"));
        return;
    }
    if (controllerAcquireInFlight || pendingAction != PendingAction::None) {
        interactiveRequestDialog->responseFailed(id, QStringLiteral("Another write is acquiring controller; retry shortly"));
        return;
    }

    pendingInteractiveResponse = std::move(response);
    activeInteractiveRequestId = id;
    ensureInteractiveController();
}

void WorkbenchWidget::ensureInteractiveController()
{
    if (!pendingInteractiveResponse)
        return;
    if (frontendSession.ownsController()) {
        performInteractiveResponse();
        return;
    }

    requestControllerAcquireInFlight = true;
    controllerUnavailable = false;
    const std::string requestId = pendingInteractiveResponse->requestId.value;
    interactiveRequestDialog->setSubmitting(requestId, QStringLiteral("Acquiring controller…"));
    refreshControllerStatus();
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.acquireController([self, requestId](const QString& error) {
        if (!self)
            return;
        self->requestControllerAcquireInFlight = false;
        if (self->frontendSession.ownsController()) {
            self->controllerUnavailable = false;
            self->performInteractiveResponse();
        } else if (!error.isEmpty()) {
            self->pendingInteractiveResponse.reset();
            self->activeInteractiveRequestId.clear();
            self->controllerUnavailable = true;
            self->interactiveRequestDialog->responseFailed(requestId, error);
        } else {
            self->interactiveRequestDialog->setSubmitting(requestId, QStringLiteral("Waiting for controller state…"));
        }
        self->refreshControllerStatus();
        self->refreshControls();
    });
    if (immediateError) {
        requestControllerAcquireInFlight = false;
        pendingInteractiveResponse.reset();
        activeInteractiveRequestId.clear();
        controllerUnavailable = true;
        interactiveRequestDialog->responseFailed(requestId, *immediateError);
        refreshControllerStatus();
        refreshControls();
    }
}

void WorkbenchWidget::performInteractiveResponse()
{
    if (!pendingInteractiveResponse || !frontendSession.ownsController() || requestResponseInFlight)
        return;

    InteractiveRequestResponse response = std::move(*pendingInteractiveResponse);
    pendingInteractiveResponse.reset();
    const std::string requestId = response.requestId.value;
    if (const auto error = interactiveResponseValidationError(frontendSession.state(), response)) {
        activeInteractiveRequestId.clear();
        interactiveRequestDialog->responseFailed(requestId, *error);
        return;
    }

    requestResponseInFlight = true;
    interactiveRequestDialog->setSubmitting(requestId, QStringLiteral("Submitting response…"));
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto completion = [self, requestId](const QString& error) {
        if (!self)
            return;
        self->requestResponseInFlight = false;
        if (error.isEmpty())
            self->interactiveRequestDialog->responseAccepted(requestId);
        else
            self->interactiveRequestDialog->responseFailed(requestId, error);
        self->activeInteractiveRequestId.clear();
        self->refreshControls();
    };

    std::optional<QString> immediateError;
    if (auto* approval = std::get_if<ai::openai::codex::typed::ApprovalDecision>(&response.value)) {
        immediateError = frontendSession.respondApproval(response.requestId, std::move(*approval), completion);
    } else if (auto* patch = std::get_if<ai::openai::codex::typed::ApplyPatchApprovalResponse>(&response.value)) {
        immediateError = frontendSession.respondApplyPatchApproval(response.requestId, std::move(*patch), completion);
    } else if (auto* command = std::get_if<ai::openai::codex::typed::ExecCommandApprovalResponse>(&response.value)) {
        immediateError = frontendSession.respondExecCommandApproval(response.requestId, std::move(*command), completion);
    } else if (auto* answers = std::get_if<std::vector<ai::openai::codex::typed::UserInputAnswer>>(&response.value)) {
        immediateError = frontendSession.respondUserInput(response.requestId, std::move(*answers), completion);
    }
    if (immediateError) {
        requestResponseInFlight = false;
        activeInteractiveRequestId.clear();
        interactiveRequestDialog->responseFailed(requestId, *immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::clearInteractiveTransients(const QString& error)
{
    const std::string requestId = activeInteractiveRequestId;
    pendingInteractiveResponse.reset();
    activeInteractiveRequestId.clear();
    requestControllerAcquireInFlight = false;
    requestResponseInFlight = false;
    if (!requestId.empty())
        interactiveRequestDialog->responseFailed(requestId, error);
}

void WorkbenchWidget::beginNewThread()
{
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready || threadStartInFlight)
        return;
    selectedThreadId.clear();
    projectedAgentThreadId.clear();
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
    if (const auto error = FrontendSession::promptValidationError(prompt)) {
        showWriteError(*error);
        return;
    }

    const auto& state = frontendSession.state();
    const auto* selected = selectedThreadId.isEmpty() ? nullptr : state.thread(selectedThreadId.toStdString());
    if (!newThreadDraft && (!selected || !selected->fullyLoaded || activeTurn(state, selected)))
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
        {
            const auto* thread = frontendSession.state().thread(threadId.toStdString());
            // Resuming an already attached thread can replay its existing item projection.
            if (thread && ai::openai::codex::frontend::client::threadIsIdle(*thread))
                startTurn(threadId, prompt);
            else
                resumeThread(threadId, prompt);
            break;
        }
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
        if (!error.isEmpty()) {
            self->showWriteError(error);
        } else {
            self->conversation->clearPrompt();
            self->conversation->setWriteStatus({});
        }
        self->refreshState();
    });
    if (immediateError) {
        turnStartInFlight = false;
        showWriteError(*immediateError);
        refreshState();
        return;
    }

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
