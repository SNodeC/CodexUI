// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/WorkbenchWidget.h"

#include "app/FrontendSession.h"
#include "ui/ConversationWidget.h"
#include "ui/InspectorWidget.h"
#include "ui/InteractiveRequestDialog.h"
#include "ui/SidebarWidget.h"
#include "ui/ThreadSetupDialog.h"
#include "ui/UpcomingTurnDock.h"

#include <ai/openai/codex/frontend/client/State.h>

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QFrame>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
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

std::string toUtf8(const QString& value)
{
    const QByteArray encoded = value.toUtf8();
    return std::string(encoded.constData(), static_cast<std::size_t>(encoded.size()));
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
                    QLabel*& workspace,
                    QPushButton*& attention,
                    QPushButton*& reconnect)
{
    auto* bar = new QFrame;
    bar->setObjectName(QStringLiteral("topBar"));
    bar->setStyleSheet(QStringLiteral(
        "QFrame#topBar{background:#ffffff;border-bottom:1px solid #d7dee8;}"));
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
    row->addSpacing(18);

    workspace = label(QStringLiteral("No workspace"), "muted");
    workspace->setObjectName(QStringLiteral("workspaceBreadcrumb"));
    workspace->setStyleSheet(QStringLiteral("color:#667085;font-size:12px;font-weight:500;"));
    row->addWidget(workspace);
    row->addStretch();

    reconnect = new QPushButton(QStringLiteral("Reconnect"));
    reconnect->setProperty("kind", "subtle");
    reconnect->setFixedHeight(32);
    reconnect->hide();
    row->addWidget(reconnect);

    attention = new QPushButton(QStringLiteral("0 requests"));
    attention->setFixedSize(106, 32);
    row->addWidget(attention);

    restoreRight = new QPushButton(QStringLiteral("Show inspector"));
    restoreRight->setProperty("kind", "subtle");
    restoreRight->setFixedHeight(32);
    restoreRight->hide();
    row->addWidget(restoreRight);
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
    bar->setStyleSheet(QStringLiteral(
        "QFrame#customStatusBar{background:#f8fafc;border-top:1px solid #d7dee8;}"));
    bar->setFixedHeight(40);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(18, 0, 80, 0);
    row->setSpacing(8);

    codexStatusDot = dot(QStringLiteral("#98a2b3"));
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
    synchronizationStatus->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;font-weight:600;"));
    row->addWidget(synchronizationStatus);
    row->addSpacing(18);
    controllerStatus = label(QStringLiteral("Observer"), "meta");
    controllerStatus->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;font-weight:600;"));
    row->addWidget(controllerStatus);
    row->addStretch();
    attentionStatus = label(QStringLiteral("0 requests"), "meta");
    attentionStatus->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;font-weight:600;"));
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
        const auto* turn = state.turn(thread->id, *iterator);
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
        if (const auto* turn = state.turn(thread->id, *iterator))
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
    layout->addWidget(makeTopBar(restoreSidebar,
                                 restoreInspector,
                                 workspaceBreadcrumb,
                                 attentionButton,
                                 reconnectButton));

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
    connect(inspector, &InspectorWidget::historicalTurnCloseRequested, this, [this] {
        selectedInspectorTurnId.clear();
        inspector->render(frontendSession.state(),
                          selectedThreadId,
                          frontendSession.lifecycle() == FrontendSession::Lifecycle::Ready,
                          frontendSession.statusText());
    });
    connect(restoreSidebar, &QPushButton::clicked, this, [this] { setSidebarVisible(true); });
    connect(restoreInspector, &QPushButton::clicked, this, [this] { setInspectorVisible(true); });
    connect(attentionButton, &QPushButton::clicked, interactiveRequestDialog, &InteractiveRequestDialog::present);
    connect(reconnectButton, &QPushButton::clicked, &frontendSession, &FrontendSession::reconnectToBackend);
    connect(sidebar, &SidebarWidget::newThreadRequested, this, &WorkbenchWidget::beginNewThread);
    connect(sidebar, &SidebarWidget::threadSelected, this, &WorkbenchWidget::selectThread);
    connect(sidebar, &SidebarWidget::threadActionRequested,
            this, &WorkbenchWidget::handleThreadAction);
    connect(inspector, &InspectorWidget::selectionChanged, this, [this] { refreshState(); });
    connect(inspector, &InspectorWidget::threadOpenRequested, this, &WorkbenchWidget::selectProjectedAgentThread);
    connect(conversation, &ConversationWidget::sendRequested, this, &WorkbenchWidget::sendPrompt);
    connect(conversation, &ConversationWidget::stopRequested, this, &WorkbenchWidget::stopActiveTurn);
    connect(conversation, &ConversationWidget::turnDetailsRequested, this,
            [this](const QString& turnId) {
                selectedInspectorTurnId = turnId;
                inspector->render(frontendSession.state(),
                                  selectedThreadId,
                                  frontendSession.lifecycle() == FrontendSession::Lifecycle::Ready,
                                  frontendSession.statusText(),
                                  selectedInspectorTurnId);
                inspector->showInfo();
            });
    connect(&frontendSession, &FrontendSession::lifecycleChanged, this, &WorkbenchWidget::refreshLifecycle);
    connect(&frontendSession, &FrontendSession::statusChanged, this, &WorkbenchWidget::refreshLifecycle);
    connect(&frontendSession, &FrontendSession::stateChanged, this, &WorkbenchWidget::scheduleStateRefresh);
    connect(&frontendSession, &FrontendSession::modelCatalogChanged, this, [this] {
        conversation->setModelCatalog(frontendSession.modelCatalog());
    });

    conversation->setModelCatalog(frontendSession.modelCatalog());
    refreshLifecycle();
    refreshState();
}

void WorkbenchWidget::scheduleStateRefresh(const detail::StateUpdateScope& scope)
{
    const bool currentSelectionAffected = scope.affectedThreadIds.contains(selectedThreadId);
    const bool awaitedSelectionAffected = !newThreadIdAwaitingState.isEmpty()
                                          && scope.affectedThreadIds.contains(newThreadIdAwaitingState);
    const bool selectedAffected = scope.allThreadsAffected || currentSelectionAffected
                                  || awaitedSelectionAffected;
    const bool submittedTurnAffected = !turnThreadIdAwaitingState.isEmpty()
                                       && (scope.allThreadsAffected
                                           || scope.affectedThreadIds.contains(
                                               turnThreadIdAwaitingState));
    const bool automaticResumeAffected = !automaticResumeThreadId.isEmpty()
                                         && (scope.allThreadsAffected
                                             || scope.affectedThreadIds.contains(
                                                 automaticResumeThreadId));
    const bool selectedInspectorAffected = scope.allInspectorsAffected
                                           || scope.affectedInspectorThreadIds.contains(selectedThreadId)
                                           || (!newThreadIdAwaitingState.isEmpty()
                                               && scope.affectedInspectorThreadIds.contains(
                                                   newThreadIdAwaitingState));
    // Keep the factual State revision current without invoking the expensive
    // Inspector projections when none of their selected semantics changed.
    if (!selectedInspectorAffected)
        inspector->updateStateRevision(frontendSession.state().revision());
    if (!selectedAffected && !selectedInspectorAffected && !scope.sidebarAffected
        && !submittedTurnAffected && !automaticResumeAffected)
        return;
    if (selectedAffected)
    {
        selectedPresentationRefreshPending = true;
        const bool requiresFullRefresh = scope.allThreadsAffected || awaitedSelectionAffected
                                         || scope.fullyAffectedThreadIds.contains(selectedThreadId);
        if (requiresFullRefresh)
        {
            selectedPresentationFullRefreshPending = true;
            selectedContentRefreshPending.clear();
        }
        else if (!selectedPresentationFullRefreshPending)
        {
            bool foundExactContent = false;
            for (const auto& identity : scope.affectedItemContents)
            {
                if (identity.threadId != selectedThreadId)
                    continue;
                foundExactContent = true;
                QStringList& itemIds = selectedContentRefreshPending[identity.turnId];
                if (!itemIds.contains(identity.itemId))
                    itemIds.append(identity.itemId);
            }
            // A conversation-affecting update without an exact item identity
            // must retain the existing bounded full reconciliation.
            if (!foundExactContent)
            {
                selectedPresentationFullRefreshPending = true;
                selectedContentRefreshPending.clear();
            }
        }
    }
    inspectorRefreshPending = inspectorRefreshPending || selectedInspectorAffected;
    sidebarRefreshPending = sidebarRefreshPending || scope.sidebarAffected;
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
        const bool refreshSidebar = sidebarRefreshPending;
        const bool exactContentOnly = refreshSelectedPresentation
                                      && !selectedPresentationFullRefreshPending
                                      && !selectedContentRefreshPending.isEmpty();
        QHash<QString, QStringList> exactContentChanges = std::move(selectedContentRefreshPending);
        selectedPresentationRefreshPending = false;
        selectedPresentationFullRefreshPending = false;
        selectedContentRefreshPending.clear();
        inspectorRefreshPending = false;
        sidebarRefreshPending = false;
        if (exactContentOnly && !refreshInspector && !refreshSidebar
            && turnThreadIdAwaitingState.isEmpty() && automaticResumeThreadId.isEmpty()
            && conversation->updateExactMessageContent(
                frontendSession.state(), selectedThreadId, exactContentChanges))
            return;
        refreshState(refreshSelectedPresentation,
                     refreshInspector,
                     refreshSidebar,
                     exactContentOnly ? &exactContentChanges : nullptr);
    });
}

void WorkbenchWidget::refreshLifecycle()
{
    using Lifecycle = FrontendSession::Lifecycle;
    QString color = QStringLiteral("#667085");
    QString title = QStringLiteral("App server disconnected");
    QString detail = frontendSession.statusText();

    switch (frontendSession.lifecycle()) {
        case Lifecycle::Connecting:
        case Lifecycle::Authenticating:
        case Lifecycle::Synchronizing:
            color = QStringLiteral("#2f6feb");
            title = QStringLiteral("Connecting to app server");
            break;
        case Lifecycle::Ready:
            color = QStringLiteral("#23845a");
            title = QStringLiteral("App server connected");
            detail = QStringLiteral("Local · Unix · synchronized");
            break;
        case Lifecycle::Failed:
            color = QStringLiteral("#a76812");
            title = QStringLiteral("App server unavailable");
            break;
        case Lifecycle::Disconnected:
            break;
    }

    const bool reconnectAvailable = frontendSession.lifecycle() == Lifecycle::Disconnected
                                    || frontendSession.lifecycle() == Lifecycle::Failed;
    reconnectButton->setVisible(reconnectAvailable);
    reconnectButton->setEnabled(reconnectAvailable);

    sidebar->setConnectionStatus(title, detail, color);
    setStyleSheetIfChanged(codexStatusDot, QStringLiteral("background:%1;border-radius:3px;").arg(color));
    synchronizationStatus->setText(frontendSession.statusText());
    synchronizationStatus->setToolTip(plainTooltip(frontendSession.statusText()));
    setStyleSheetIfChanged(synchronizationStatus,
                           QStringLiteral("color:%1;font-size:10px;font-weight:600;").arg(color));
    if (frontendSession.lifecycle() != Lifecycle::Ready) {
        threadContextStatus->setText(QStringLiteral("No thread context"));
        threadContextStatus->setToolTip({});
        agentActivityStatus->setText(QStringLiteral("No agent activity"));
        inspector->render(frontendSession.state(), {}, false, frontendSession.statusText());
    }

    if (frontendSession.lifecycle() != Lifecycle::Ready) {
        const bool writeWasPending = pendingAction != PendingAction::None || controllerAcquireInFlight
                                     || threadStartInFlight || threadResumeInFlight || turnStartInFlight
                                     || turnSteerInFlight
                                     || interruptInFlight || threadMutationInFlight;
        clearWriteTransients();
        if (writeWasPending)
            showWriteError(QStringLiteral("Backend disconnected before the write completed"));
        if (requestControllerAcquireInFlight || requestResponseInFlight || pendingInteractiveResponse)
            clearInteractiveTransients(QStringLiteral("Backend disconnected before the response completed"));
    }
    refreshControllerStatus();
    refreshControls();
}

void WorkbenchWidget::refreshState(bool refreshSelectedPresentation,
                                   bool refreshInspector,
                                   bool refreshSidebar,
                                   const QHash<QString, QStringList>* exactContentChanges)
{
    stateRefreshPending = false;
    selectedPresentationRefreshPending = false;
    selectedPresentationFullRefreshPending = false;
    selectedContentRefreshPending.clear();
    inspectorRefreshPending = false;
    sidebarRefreshPending = false;
    const auto& state = frontendSession.state();
    const auto threads = state.threads();
    const bool ready = frontendSession.lifecycle() == FrontendSession::Lifecycle::Ready;
    const bool threadListComplete = state.threadList().value && state.threadList().value->complete;
    const bool threadDiscoveryComplete = threadListComplete
                                         && frontendSession.archivedThreadDiscoveryComplete();
    const QString previousThreadId = selectedThreadId;

    if (!newThreadIdAwaitingState.isEmpty()
        && state.thread(newThreadIdAwaitingState.toStdString()) != nullptr) {
        selectedThreadId = newThreadIdAwaitingState;
        selectedInspectorTurnId.clear();
        newThreadIdAwaitingState.clear();
    }
    const bool awaitingSelectedThread = !newThreadIdAwaitingState.isEmpty()
        && selectedThreadId == newThreadIdAwaitingState;
    if (!selectedThreadId.isEmpty()
        && state.thread(selectedThreadId.toStdString()) == nullptr && ready && threadDiscoveryComplete
        && !awaitingSelectedThread)
        selectedThreadId.clear();
    if (selectedThreadId.isEmpty() && !threads.empty() && newThreadIdAwaitingState.isEmpty())
        selectedThreadId = QString::fromStdString(threads.front().id.value);
    const bool selectionChanged = previousThreadId != selectedThreadId;
    if (selectionChanged) {
        selectedInspectorTurnId.clear();
        conversation->clearPrompt();
    }
    refreshSelectedPresentation = refreshSelectedPresentation || selectionChanged;
    refreshInspector = refreshInspector || selectionChanged;
    refreshSidebar = refreshSidebar || selectionChanged;

    if (refreshSidebar)
        sidebar->setThreads(state, selectedThreadId, threadDiscoveryComplete);

    // ConversationWidget resolves the stable selection against this exact
    // immutable State and never retains backend object addresses.
    if (refreshSelectedPresentation) {
        conversation->render(state,
                             selectedThreadId,
                             false,
                             selectionChanged ? nullptr : exactContentChanges);
    }
    reconcileSubmittedTurnSettings();

    if (refreshInspector)
        inspector->render(state, selectedThreadId,
                          ready, frontendSession.statusText(), selectedInspectorTurnId);

    const auto* selected = !selectedThreadId.isEmpty()
                               ? state.thread(selectedThreadId.toStdString())
                               : nullptr;
    if (refreshSelectedPresentation) {
        const QString context = ready && selected && selected->cwd
                                    ? QString::fromStdString(selected->cwd->value)
                                    : QStringLiteral("No thread context");
        const QString workspaceName = ready && selected && selected->cwd
            ? QFileInfo(context).fileName()
            : QString{};
        workspaceBreadcrumb->setText(workspaceName.isEmpty()
                                         ? QStringLiteral("No workspace")
                                         : QStringLiteral("Workspace / %1").arg(workspaceName));
        workspaceBreadcrumb->setToolTip(workspaceName.isEmpty() ? QString{} : plainTooltip(context));
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
    const QString attentionText = attentionCount == 0
        ? QStringLiteral("No attention")
        : QStringLiteral("%1 request%2")
              .arg(attentionCount)
              .arg(attentionCount == 1 ? QString{} : QStringLiteral("s"));
    attentionButton->setText(attentionText);
    attentionStatus->setText(attentionText);
    const bool needsAttention = attentionCount > 0;
    setStyleSheetIfChanged(
        attentionButton,
        needsAttention
            ? QStringLiteral("background:#fff6df;color:#a76812;border:1px solid #e5c77d;border-radius:8px;font-size:11px;font-weight:600;")
            : QStringLiteral("background:#f1f5fb;color:#667085;border:1px solid #d7dee8;border-radius:8px;font-size:11px;font-weight:600;"));
    setStyleSheetIfChanged(
        attentionStatus,
        QStringLiteral("color:%1;font-size:10px;font-weight:600;")
            .arg(needsAttention ? QStringLiteral("#a76812") : QStringLiteral("#667085")));
    attentionButton->setToolTip(needsAttention ? QStringLiteral("Open pending Codex requests")
                                               : QStringLiteral("Codex has no pending requests"));
    interactiveRequestDialog->synchronize(state);

    if (selected && !selected->fullyLoaded && projectedAgentThreadId != selectedThreadId)
        frontendSession.loadThread(selectedThreadId);

    reconcileAutomaticResumeState();
    controllerUnavailable = controllerUnavailable && !frontendSession.ownsController();
    refreshControllerStatus();
    maybeResumeSelectedThread();
    if (pendingAction != PendingAction::None && frontendSession.ownsController())
        executePendingAction();
    if (pendingInteractiveResponse && !requestControllerAcquireInFlight && !requestResponseInFlight
        && frontendSession.ownsController())
        performInteractiveResponse();
    refreshControls();
}

void WorkbenchWidget::selectThread(const QString& threadId)
{
    ++selectionGeneration;
    if (automaticResumeThreadId != threadId)
        automaticResumeAttemptedThreadIds.remove(threadId);
    if (!newThreadIdAwaitingState.isEmpty() && threadId != newThreadIdAwaitingState)
        newThreadIdAwaitingState.clear();
    if (selectedThreadId != threadId)
        conversation->clearPrompt();
    if (selectedThreadId != threadId)
        selectedInspectorTurnId.clear();
    selectedThreadId = threadId;
    projectedAgentThreadId.clear();
    conversation->setWriteStatus({});
    refreshState();
}

void WorkbenchWidget::selectProjectedAgentThread(const QString& threadId)
{
    ++selectionGeneration;
    if (automaticResumeThreadId != threadId)
        automaticResumeAttemptedThreadIds.remove(threadId);
    if (!newThreadIdAwaitingState.isEmpty() && threadId != newThreadIdAwaitingState)
        newThreadIdAwaitingState.clear();
    if (selectedThreadId != threadId)
        conversation->clearPrompt();
    if (selectedThreadId != threadId)
        selectedInspectorTurnId.clear();
    selectedThreadId = threadId;
    projectedAgentThreadId = threadId;
    conversation->setWriteStatus({});
    refreshState();
}

void WorkbenchWidget::refreshControls()
{
    const bool ready = frontendSession.lifecycle() == FrontendSession::Lifecycle::Ready;
    const auto& state = frontendSession.state();
    const auto* selected = !selectedThreadId.isEmpty()
                               ? state.thread(selectedThreadId.toStdString())
                               : nullptr;
    const auto* active = activeTurn(state, selected);
    const bool pendingControllerWrite = controllerAcquireInFlight || pendingAction != PendingAction::None
                                        || requestControllerAcquireInFlight || requestResponseInFlight
                                        || threadMutationInFlight;
    const bool promptSubmissionInFlight = threadStartInFlight || threadResumeInFlight
                                          || turnStartInFlight || turnSteerInFlight;
    const bool selectedWritable = selected && selected->fullyLoaded
                                  && !selected->archived.value_or(false);

    sidebar->setNewThreadEnabled(ready && !promptSubmissionInFlight && !pendingControllerWrite);
    sidebar->setThreadInteractionEnabled(ready);
    const bool idleComposerAvailable = ready && selectedWritable && active == nullptr
                                       && !promptSubmissionInFlight && !pendingControllerWrite;
    const bool steerComposerAvailable = ready && selectedWritable && active != nullptr
                                        && !promptSubmissionInFlight && !pendingControllerWrite;
    conversation->setActionState(
        idleComposerAvailable || steerComposerAvailable,
        ready && active != nullptr && !interruptInFlight
            && !promptSubmissionInFlight && !pendingControllerWrite,
        idleComposerAvailable || steerComposerAvailable,
        idleComposerAvailable,
        ready && active != nullptr,
        active != nullptr,
        selectedThreadId,
        active ? QString::fromStdString(active->id.value) : QString{});
}

bool WorkbenchWidget::writeOperationBusy() const noexcept
{
    return pendingAction != PendingAction::None || controllerAcquireInFlight
        || threadStartInFlight || threadResumeInFlight || turnStartInFlight
        || turnSteerInFlight
        || interruptInFlight || threadMutationInFlight
        || requestControllerAcquireInFlight || requestResponseInFlight;
}

void WorkbenchWidget::refreshControllerStatus()
{
    QString text = QStringLiteral("Observer");
    QString color = QStringLiteral("#667085");
    QString tooltip = QStringLiteral("Writes acquire controller ownership when needed");
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready) {
        tooltip = QStringLiteral("Controller state is unavailable while disconnected");
    } else if (frontendSession.ownsController()) {
        text = QStringLiteral("Controller");
        color = QStringLiteral("#23845a");
        tooltip = QStringLiteral("This frontend owns controller");
    } else if (controllerUnavailable) {
        text = QStringLiteral("Controller unavailable");
        color = QStringLiteral("#a76812");
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
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready || writeOperationBusy())
        return;

    auto* dialog = new ThreadSetupDialog(ThreadSetupDialog::Mode::NewThread, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QPointer<ThreadSetupDialog> guarded(dialog);
    connect(dialog, &QDialog::accepted, this, [this, guarded] {
        if (!guarded)
            return;
        if (writeOperationBusy()) {
            showWriteError(QStringLiteral("Another write operation is already in progress"));
            return;
        }
        const auto result = guarded->result();
        const auto* setup = std::get_if<NewThreadSetup>(&result);
        if (!setup)
            return;
        pendingNewThreadSetup = *setup;
        pendingAction = PendingAction::CreateThread;
        pendingSelectionGeneration = selectionGeneration;
        pendingThreadId.clear();
        conversation->setWriteStatus(QStringLiteral("Preparing new thread…"));
        ensureController();
    });
    dialog->open();
}

void WorkbenchWidget::handleThreadAction(const QString& threadId, ThreadAction action)
{
    const auto& state = frontendSession.state();
    const auto* thread = state.thread(threadId.toStdString());
    if (!thread) {
        showWriteError(QStringLiteral("The selected thread is no longer available"));
        return;
    }
    const ThreadActionAvailability available = detail::threadActionAvailability(state, *thread);
    switch (action) {
        case ThreadAction::Open:
            if (available.open)
                selectThread(threadId);
            return;
        case ThreadAction::CopyId:
            QApplication::clipboard()->setText(threadId);
            return;
        case ThreadAction::Rename:
            if (writeOperationBusy())
                return;
            if (available.rename)
                showRenameThreadDialog(threadId);
            return;
        case ThreadAction::Fork:
            if (writeOperationBusy())
                return;
            if (available.fork)
                showForkThreadDialog(threadId);
            return;
        case ThreadAction::ResumeWithOptions:
            if (writeOperationBusy())
                return;
            if (available.resumeWithOptions)
                showResumeWithOptionsDialog(threadId);
            return;
        case ThreadAction::Delete:
            if (writeOperationBusy())
                return;
            if (available.remove)
                showDeleteThreadConfirmation(threadId);
            return;
        case ThreadAction::Interrupt:
        {
            if (writeOperationBusy())
                return;
            if (!available.interrupt)
                return;
            const auto* turn = activeTurn(state, thread);
            if (!turn)
                return;
            pendingAction = PendingAction::InterruptTurn;
            pendingThreadId = threadId;
            pendingTurnId = QString::fromStdString(turn->id.value);
            conversation->setWriteStatus(QStringLiteral("Preparing interrupt…"));
            ensureController();
            return;
        }
        case ThreadAction::Archive:
        case ThreadAction::Unarchive:
        {
            if (writeOperationBusy())
                return;
            const bool allowed = action == ThreadAction::Archive ? available.archive : available.unarchive;
            if (!allowed)
                return;
            pendingAction = action == ThreadAction::Archive ? PendingAction::ArchiveThread
                                                            : PendingAction::UnarchiveThread;
            pendingThreadId = threadId;
            conversation->setWriteStatus(action == ThreadAction::Archive
                                             ? QStringLiteral("Preparing archive…")
                                             : QStringLiteral("Preparing unarchive…"));
            ensureController();
            return;
        }
    }
}

void WorkbenchWidget::showRenameThreadDialog(const QString& threadId)
{
    const auto* thread = frontendSession.state().thread(threadId.toStdString());
    if (!thread)
        return;
    auto* dialog = new QInputDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Rename thread"));
    dialog->setLabelText(QStringLiteral("Thread name"));
    dialog->setTextValue(thread->title ? QString::fromStdString(*thread->title) : QString{});
    dialog->setOkButtonText(QStringLiteral("Rename"));
    const QPointer<QInputDialog> guarded(dialog);
    connect(dialog, &QDialog::accepted, this, [this, guarded, threadId] {
        if (!guarded || guarded->textValue().trimmed().isEmpty())
            return;
        if (writeOperationBusy()) {
            showWriteError(QStringLiteral("Another write operation is already in progress"));
            return;
        }
        pendingAction = PendingAction::RenameThread;
        pendingThreadId = threadId;
        pendingThreadValue = guarded->textValue().trimmed();
        conversation->setWriteStatus(QStringLiteral("Preparing rename…"));
        ensureController();
    });
    dialog->open();
}

void WorkbenchWidget::showForkThreadDialog(const QString& threadId)
{
    const auto* thread = frontendSession.state().thread(threadId.toStdString());
    if (!thread)
        return;
    auto* dialog = new ThreadSetupDialog(ThreadSetupDialog::Mode::ForkThread, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (thread->title)
        dialog->setSuggestedThreadName(QStringLiteral("%1 (fork)").arg(QString::fromStdString(*thread->title)));
    const QPointer<ThreadSetupDialog> guarded(dialog);
    connect(dialog, &QDialog::accepted, this, [this, guarded, threadId] {
        if (!guarded)
            return;
        if (writeOperationBusy()) {
            showWriteError(QStringLiteral("Another write operation is already in progress"));
            return;
        }
        const auto result = guarded->result();
        const auto* setup = std::get_if<ForkThreadSetup>(&result);
        if (!setup)
            return;
        pendingForkThreadSetup = *setup;
        pendingAction = PendingAction::ForkThread;
        pendingSelectionGeneration = selectionGeneration;
        pendingThreadId = threadId;
        conversation->setWriteStatus(QStringLiteral("Preparing fork…"));
        ensureController();
    });
    dialog->open();
}

void WorkbenchWidget::showResumeWithOptionsDialog(const QString& threadId)
{
    auto* dialog = new ThreadSetupDialog(ThreadSetupDialog::Mode::ResumeWithOptions, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QPointer<ThreadSetupDialog> guarded(dialog);
    connect(dialog, &QDialog::accepted, this, [this, guarded, threadId] {
        if (!guarded)
            return;
        if (writeOperationBusy()) {
            showWriteError(QStringLiteral("Another write operation is already in progress"));
            return;
        }
        const auto result = guarded->result();
        const auto* setup = std::get_if<ResumeWithOptionsSetup>(&result);
        if (!setup)
            return;
        pendingResumeSetup = *setup;
        pendingAction = PendingAction::ResumeWithOptions;
        pendingSelectionGeneration = selectionGeneration;
        pendingThreadId = threadId;
        conversation->setWriteStatus(QStringLiteral("Preparing resume…"));
        ensureController();
    });
    dialog->open();
}

void WorkbenchWidget::showDeleteThreadConfirmation(const QString& threadId)
{
    const auto* thread = frontendSession.state().thread(threadId.toStdString());
    if (!thread)
        return;
    const QString name = thread->title && !thread->title->empty()
                             ? QString::fromStdString(*thread->title)
                             : threadId;
    auto* dialog = new QMessageBox(QMessageBox::Warning,
                                   QStringLiteral("Delete thread"),
                                   QStringLiteral("Delete “%1”? This cannot be undone.").arg(name),
                                   QMessageBox::Cancel | QMessageBox::Ok,
                                   this);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->button(QMessageBox::Ok)->setText(QStringLiteral("Delete"));
    connect(dialog, &QMessageBox::finished, this, [this, threadId](int result) {
        if (result != QMessageBox::Ok)
            return;
        if (writeOperationBusy()) {
            showWriteError(QStringLiteral("Another write operation is already in progress"));
            return;
        }
        pendingAction = PendingAction::DeleteThread;
        pendingThreadId = threadId;
        conversation->setWriteStatus(QStringLiteral("Preparing delete…"));
        ensureController();
    });
    dialog->open();
}

void WorkbenchWidget::sendPrompt(const QString& prompt, bool steerRequested)
{
    const QList<AttachmentInfo> attachments = conversation->attachments();
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready
        || (prompt.trimmed().isEmpty() && attachments.isEmpty()) || writeOperationBusy())
        return;
    if (const auto error = FrontendSession::promptValidationError(prompt)) {
        showWriteError(*error);
        return;
    }

    const auto& state = frontendSession.state();
    const auto* selected = selectedThreadId.isEmpty() ? nullptr : state.thread(selectedThreadId.toStdString());
    if (!selected || !selected->fullyLoaded || selected->archived.value_or(false))
        return;

    if (const auto* active = activeTurn(state, selected)) {
        if (!steerRequested) {
            showWriteError(QStringLiteral(
                "A turn started before this prompt was submitted. Edit the draft to send it as a steer."));
            refreshControls();
            return;
        }
        pendingAction = PendingAction::SteerActiveTurn;
        pendingPrompt = prompt;
        pendingAttachments = attachments;
        pendingAttachmentWorkspace = conversation->attachmentWorkspace();
        pendingThreadId = selectedThreadId;
        pendingTurnId = QString::fromStdString(active->id.value);
        pendingTurnDraft = {};
        conversation->setWriteStatus(QStringLiteral("Preparing steer…"));
        ensureController();
        return;
    }

    if (steerRequested) {
        showWriteError(QStringLiteral(
            "The active turn ended before this steer was submitted. Edit the draft to use it for a new turn."));
        refreshControls();
        return;
    }

    const UpcomingTurnDraft settings = conversation->upcomingTurnDraft();
    if (settings.threadIdentity != selectedThreadId) {
        showWriteError(QStringLiteral("Upcoming-turn settings no longer match the selected thread"));
        return;
    }

    pendingAction = PendingAction::SendExistingThread;
    pendingPrompt = prompt;
    pendingAttachments = attachments;
    pendingAttachmentWorkspace = conversation->attachmentWorkspace();
    pendingThreadId = selectedThreadId;
    pendingTurnId.clear();
    pendingTurnDraft = settings;
    conversation->setWriteStatus(QStringLiteral("Preparing write…"));
    ensureController();
}

void WorkbenchWidget::stopActiveTurn()
{
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready || writeOperationBusy()
        || selectedThreadId.isEmpty())
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

void WorkbenchWidget::maybeResumeSelectedThread()
{
    if (frontendSession.lifecycle() != FrontendSession::Lifecycle::Ready
        || selectedThreadId.isEmpty() || writeOperationBusy()
        || automaticResumeAttemptedThreadIds.contains(selectedThreadId))
        return;

    const auto& state = frontendSession.state();
    const auto* thread = state.thread(selectedThreadId.toStdString());
    if (!thread || !thread->fullyLoaded || thread->archived.value_or(false)
        || activeTurn(state, thread)
        || ai::openai::codex::frontend::client::threadIsIdle(*thread))
        return;

    automaticResumeAttemptedThreadIds.insert(selectedThreadId);
    pendingAction = PendingAction::OpenThread;
    pendingThreadId = selectedThreadId;
    pendingSelectionGeneration = selectionGeneration;
    conversation->setWriteStatus(QStringLiteral("Resuming thread…"));
    ensureController();
}

void WorkbenchWidget::reconcileAutomaticResumeState()
{
    if (automaticResumeThreadId.isEmpty())
        return;

    const auto& state = frontendSession.state();
    const auto* thread = state.thread(automaticResumeThreadId.toStdString());
    const bool threadDiscoveryComplete = state.threadList().value
                                         && state.threadList().value->complete
                                         && frontendSession.archivedThreadDiscoveryComplete();
    const bool canonicalResumeObserved = thread
        && (thread->archived.value_or(false) || activeTurn(state, thread)
            || ai::openai::codex::frontend::client::threadIsIdle(*thread));
    if (!canonicalResumeObserved && (thread || !threadDiscoveryComplete))
        return;

    const QString completedThreadId = automaticResumeThreadId;
    automaticResumeThreadId.clear();
    automaticResumeAttemptedThreadIds.remove(completedThreadId);
    threadResumeInFlight = false;
    const bool composerReady = thread && !thread->archived.value_or(false)
                               && !activeTurn(state, thread)
                               && ai::openai::codex::frontend::client::threadIsIdle(*thread);
    if (selectedThreadId == completedThreadId) {
        conversation->setWriteStatus({});
        if (composerReady)
            conversation->focusComposer();
    }
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
            self->pendingAttachments.clear();
            self->pendingAttachmentWorkspace.clear();
            self->pendingThreadId.clear();
            self->pendingTurnId.clear();
            self->pendingThreadValue.clear();
            self->pendingNewThreadSetup.reset();
            self->pendingForkThreadSetup.reset();
            self->pendingResumeSetup.reset();
            self->pendingTurnDraft = {};
            self->pendingSelectionGeneration = 0;
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
        pendingAttachments.clear();
        pendingAttachmentWorkspace.clear();
        pendingThreadId.clear();
        pendingTurnId.clear();
        pendingThreadValue.clear();
        pendingNewThreadSetup.reset();
        pendingForkThreadSetup.reset();
        pendingResumeSetup.reset();
        pendingTurnDraft = {};
        pendingSelectionGeneration = 0;
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
    const QList<AttachmentInfo> attachments = pendingAttachments;
    const QString attachmentWorkspace = pendingAttachmentWorkspace;
    const QString threadId = pendingThreadId;
    const QString turnId = pendingTurnId;
    const QString value = pendingThreadValue;
    const auto newThreadSetup = pendingNewThreadSetup;
    const auto forkSetup = pendingForkThreadSetup;
    const auto resumeSetup = pendingResumeSetup;
    const UpcomingTurnDraft turnSettings = pendingTurnDraft;
    const std::uint64_t expectedSelectionGeneration = pendingSelectionGeneration;
    pendingAction = PendingAction::None;
    pendingPrompt.clear();
    pendingAttachments.clear();
    pendingAttachmentWorkspace.clear();
    pendingThreadId.clear();
    pendingTurnId.clear();
    pendingThreadValue.clear();
    pendingNewThreadSetup.reset();
    pendingForkThreadSetup.reset();
    pendingResumeSetup.reset();
    pendingTurnDraft = {};
    pendingSelectionGeneration = 0;

    switch (action) {
        case PendingAction::OpenThread:
        {
            const auto& state = frontendSession.state();
            const auto* thread = state.thread(threadId.toStdString());
            if (selectionGeneration != expectedSelectionGeneration
                || selectedThreadId != threadId) {
                automaticResumeAttemptedThreadIds.remove(threadId);
                refreshState();
                break;
            }
            if (!thread || !thread->fullyLoaded || thread->archived.value_or(false)
                || activeTurn(state, thread)) {
                automaticResumeAttemptedThreadIds.remove(threadId);
                showWriteError(QStringLiteral("The selected thread changed before it could be resumed"));
                refreshControls();
                break;
            }
            if (ai::openai::codex::frontend::client::threadIsIdle(*thread)) {
                conversation->setWriteStatus({});
                refreshControls();
                break;
            }
            resumeThreadForOpen(threadId, expectedSelectionGeneration);
            break;
        }
        case PendingAction::SendExistingThread:
        {
            const auto& state = frontendSession.state();
            const auto* thread = state.thread(threadId.toStdString());
            if (!thread || !thread->fullyLoaded || thread->archived.value_or(false)
                || activeTurn(state, thread) || turnSettings.threadIdentity != threadId) {
                showWriteError(QStringLiteral("The target thread changed before the prompt could be sent"));
                refreshControls();
                break;
            }
            const auto submission = prepareTurnSubmission(
                threadId, prompt, attachments, attachmentWorkspace);
            if (!submission) {
                refreshControls();
                break;
            }
            // Resuming an already attached thread can replay its existing item projection.
            if (ai::openai::codex::frontend::client::threadIsIdle(*thread))
                startTurn(threadId, *submission, turnSettings);
            else
                resumeThread(threadId, *submission, turnSettings);
            break;
        }
        case PendingAction::SteerActiveTurn:
        {
            const auto& state = frontendSession.state();
            const auto* thread = state.thread(threadId.toStdString());
            const auto* turn = activeTurn(state, thread);
            if (!thread || !thread->fullyLoaded || thread->archived.value_or(false)
                || !turn || QString::fromStdString(turn->id.value) != turnId) {
                showWriteError(QStringLiteral("The target turn changed before it could be steered"));
                refreshControls();
                break;
            }
            const auto submission = prepareTurnSubmission(
                threadId, prompt, attachments, attachmentWorkspace);
            if (!submission) {
                refreshControls();
                break;
            }
            steerTurn(threadId, turnId, *submission);
            break;
        }
        case PendingAction::InterruptTurn:
        {
            const auto& state = frontendSession.state();
            const auto* thread = state.thread(threadId.toStdString());
            const auto* turn = activeTurn(state, thread);
            if (!thread || !turn || QString::fromStdString(turn->id.value) != turnId
                || !detail::threadActionAvailability(state, *thread).interrupt) {
                showWriteError(QStringLiteral("The target turn is no longer interruptible"));
                refreshControls();
                break;
            }
            interruptTurn(threadId, turnId);
            break;
        }
        case PendingAction::CreateThread:
            if (newThreadSetup)
                startNewThread(*newThreadSetup, expectedSelectionGeneration);
            break;
        case PendingAction::RenameThread:
        case PendingAction::ArchiveThread:
        case PendingAction::UnarchiveThread:
        case PendingAction::DeleteThread:
            mutateThread(action, threadId, value);
            break;
        case PendingAction::ForkThread:
            if (forkSetup) {
                const auto& state = frontendSession.state();
                const auto* thread = state.thread(threadId.toStdString());
                if (!thread || !detail::threadActionAvailability(state, *thread).fork) {
                    showWriteError(QStringLiteral("The target thread can no longer be forked"));
                    refreshControls();
                    break;
                }
                forkThread(threadId, *forkSetup, expectedSelectionGeneration);
            }
            break;
        case PendingAction::ResumeWithOptions:
            if (resumeSetup) {
                const auto& state = frontendSession.state();
                const auto* thread = state.thread(threadId.toStdString());
                if (!thread || !detail::threadActionAvailability(state, *thread).resumeWithOptions) {
                    showWriteError(QStringLiteral("The target thread can no longer be resumed with options"));
                    refreshControls();
                    break;
                }
                resumeThreadWithOptions(threadId, *resumeSetup, expectedSelectionGeneration);
            }
            break;
        case PendingAction::None:
            break;
    }
}

void WorkbenchWidget::startNewThread(const NewThreadSetup& setup,
                                     std::uint64_t expectedSelectionGeneration)
{
    threadStartInFlight = true;
    conversation->setWriteStatus(QStringLiteral("Creating thread…"));
    refreshControls();
    ai::openai::codex::typed::ThreadStartParams parameters;
    if (!setup.instructions.baseInstructions.isEmpty())
        parameters.baseInstructions = toUtf8(setup.instructions.baseInstructions);
    if (!setup.instructions.developerInstructions.isEmpty())
        parameters.developerInstructions = toUtf8(setup.instructions.developerInstructions);
    parameters.ephemeral = setup.temporary;
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.startThread(
        std::move(parameters),
        [self, name = setup.name.trimmed(), expectedSelectionGeneration](const QString& threadId,
                                                                         const QString& error) {
            if (!self)
                return;
            self->threadStartInFlight = false;
            if (!error.isEmpty()) {
                self->showWriteError(error);
                self->refreshControls();
                return;
            }
            const bool keepAutomaticSelection = self->selectionGeneration == expectedSelectionGeneration;
            if (keepAutomaticSelection) {
                self->newThreadIdAwaitingState = threadId;
                self->selectedThreadId = threadId;
                self->projectedAgentThreadId.clear();
                self->conversation->clearPrompt();
                self->conversation->clearUpcomingTurnSettings();
                self->conversation->setWriteStatus(QStringLiteral("Thread created"));
            }
            if (!name.isEmpty()) {
                self->threadMutationInFlight = true;
                self->refreshControls();
                const auto renameError = self->frontendSession.renameThread(
                    threadId,
                    name,
                    [self, threadId](const QString& renameFailure) {
                        if (!self)
                            return;
                        self->threadMutationInFlight = false;
                        if (!renameFailure.isEmpty())
                            self->showWriteError(renameFailure);
                        else if (self->selectedThreadId == threadId)
                            self->conversation->setWriteStatus({});
                        self->refreshState();
                    });
                if (renameError) {
                    self->threadMutationInFlight = false;
                    self->showWriteError(*renameError);
                }
            } else if (keepAutomaticSelection) {
                self->conversation->setWriteStatus({});
            }
            self->refreshState();
            if (keepAutomaticSelection)
                self->conversation->focusComposer();
        });
    if (immediateError) {
        threadStartInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

std::optional<WorkbenchWidget::PreparedTurnSubmission>
WorkbenchWidget::prepareTurnSubmission(const QString& threadId,
                                       const QString& prompt,
                                       const QList<AttachmentInfo>& attachments,
                                       const QString& workspace)
{
    AttachmentPreparation preparation;
    QString error;
    if (!AttachmentManager::prepare(
            attachments, workspace, threadId, &preparation, &error)) {
        showWriteError(error);
        return std::nullopt;
    }
    const QString effectivePrompt = AttachmentManager::composePrompt(prompt, preparation);
    if (const auto validationError = FrontendSession::promptValidationError(effectivePrompt)) {
        showWriteError(*validationError);
        return std::nullopt;
    }
    if (effectivePrompt.trimmed().isEmpty() && preparation.imagePaths.isEmpty()) {
        showWriteError(QStringLiteral("A turn requires a prompt or attachment"));
        return std::nullopt;
    }
    return PreparedTurnSubmission{
        prompt, effectivePrompt, preparation.imagePaths, attachments};
}

void WorkbenchWidget::startTurn(const QString& threadId,
                                const PreparedTurnSubmission& submission,
                                const UpcomingTurnDraft& settings)
{
    if (settings.threadIdentity != threadId) {
        showWriteError(QStringLiteral("Upcoming-turn settings no longer match the target thread"));
        return;
    }
    turnStartInFlight = true;
    turnThreadIdAwaitingState.clear();
    turnIdAwaitingState.clear();
    submittedTurnSettings.reset();
    if (selectedThreadId == threadId)
        conversation->setWriteStatus(QStringLiteral("Starting turn…"));
    refreshControls();
    ai::openai::codex::typed::TurnStartParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    parameters.model = settings.model;
    parameters.effort = settings.effort;
    parameters.personality = settings.personality;
    parameters.sandboxPolicy = settings.sandboxPolicy;
    parameters.approvalPolicy = settings.approvalPolicy;
    parameters.approvalsReviewer = settings.approvalsReviewer;
    parameters.cwd = settings.cwd;
    parameters.serviceTier = settings.serviceTier;
    parameters.summary = settings.summary;
    parameters.collaborationMode = settings.collaborationMode;
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.startTurn(
        std::move(parameters),
        submission.effectivePrompt,
        submission.imagePaths,
        [self,
         targetThreadId = threadId,
         submittedPrompt = submission.userPrompt,
         submittedAttachments = submission.attachments,
         submittedSettings = settings](const QString& acceptedTurnId, const QString& error) {
            if (!self)
                return;
            if (!error.isEmpty() || acceptedTurnId.isEmpty()) {
                self->turnStartInFlight = false;
                self->turnThreadIdAwaitingState.clear();
                self->turnIdAwaitingState.clear();
                const QString failure = !error.isEmpty()
                    ? error
                    : QStringLiteral("The backend accepted the turn without returning its identity");
                if (self->selectedThreadId == targetThreadId)
                    self->showWriteError(failure);
                else
                    self->showWriteError(
                        QStringLiteral("Turn in %1 failed: %2").arg(targetThreadId, failure));
            } else {
                self->turnThreadIdAwaitingState = targetThreadId;
                self->turnIdAwaitingState = acceptedTurnId;
                self->submittedTurnSettings = SubmittedTurnSettings{
                    targetThreadId, acceptedTurnId, submittedSettings};
                if (self->selectedThreadId == targetThreadId) {
                    self->conversation->clearPromptIfUnchanged(submittedPrompt);
                    self->conversation->clearAttachmentsIfUnchanged(submittedAttachments);
                    self->conversation->setWriteStatus(QStringLiteral("Waiting for canonical turn state…"));
                }
            }
            self->refreshState();
        });
    if (immediateError) {
        turnStartInFlight = false;
        turnThreadIdAwaitingState.clear();
        turnIdAwaitingState.clear();
        showWriteError(*immediateError);
        refreshState();
        return;
    }

    if (selectedThreadId == threadId)
        conversation->setWriteStatus(QStringLiteral("Prompt submitted"));
    refreshControls();
}

void WorkbenchWidget::steerTurn(const QString& threadId,
                                const QString& turnId,
                                const PreparedTurnSubmission& submission)
{
    turnSteerInFlight = true;
    if (selectedThreadId == threadId)
        conversation->setWriteStatus(QStringLiteral("Steering turn…"));
    refreshControls();

    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.steerTurn(
        threadId,
        turnId,
        submission.effectivePrompt,
        submission.imagePaths,
        [self,
         targetThreadId = threadId,
         submittedPrompt = submission.userPrompt,
         submittedAttachments = submission.attachments](const QString& error) {
            if (!self)
                return;
            self->turnSteerInFlight = false;
            if (!error.isEmpty()) {
                if (self->selectedThreadId == targetThreadId)
                    self->showWriteError(error);
                else
                    self->showWriteError(
                        QStringLiteral("Turn in %1 could not be steered: %2")
                            .arg(targetThreadId, error));
            } else if (self->selectedThreadId == targetThreadId) {
                self->conversation->clearPromptIfUnchanged(submittedPrompt);
                self->conversation->clearAttachmentsIfUnchanged(submittedAttachments);
                self->conversation->setWriteStatus({});
            }
            self->refreshState();
        });
    if (immediateError) {
        turnSteerInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::reconcileSubmittedTurnSettings()
{
    const auto& state = frontendSession.state();
    if (!turnThreadIdAwaitingState.isEmpty() && !turnIdAwaitingState.isEmpty()) {
        const auto* awaitingThread = state.thread(turnThreadIdAwaitingState.toStdString());
        if (awaitingThread) {
            const auto acceptedTurn = std::ranges::find_if(
                awaitingThread->orderedTurns,
                [this](const auto& id) {
                    return QString::fromStdString(id.value) == turnIdAwaitingState;
                });
            if (acceptedTurn != awaitingThread->orderedTurns.end()) {
                const auto* turn = state.turn(awaitingThread->id, *acceptedTurn);
                if (turn) {
                    const QString completedThreadId = turnThreadIdAwaitingState;
                    turnStartInFlight = false;
                    turnThreadIdAwaitingState.clear();
                    turnIdAwaitingState.clear();
                    if (selectedThreadId == completedThreadId)
                        conversation->setWriteStatus({});
                }
            }
        }
    }

    if (!submittedTurnSettings)
        return;
    const auto* thread = state.thread(submittedTurnSettings->threadId.toStdString());
    if (!thread)
        return;
    const auto acceptedTurn = std::ranges::find_if(
        thread->orderedTurns,
        [this](const auto& id) {
            return QString::fromStdString(id.value) == submittedTurnSettings->turnId;
        });
    if (acceptedTurn == thread->orderedTurns.end())
        return;
    const auto* turn = state.turn(thread->id, *acceptedTurn);
    if (!turn
        || !thread->executionConfiguration || !turn->effectiveExecutionConfiguration
        || *thread->executionConfiguration != *turn->effectiveExecutionConfiguration)
        return;

    conversation->acknowledgeSubmittedSettings(submittedTurnSettings->draft);
    submittedTurnSettings.reset();
}

void WorkbenchWidget::resumeThread(const QString& threadId,
                                   const PreparedTurnSubmission& submission,
                                   const UpcomingTurnDraft& settings)
{
    threadResumeInFlight = true;
    if (selectedThreadId == threadId)
        conversation->setWriteStatus(QStringLiteral("Attaching thread…"));
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.resumeThread(
        threadId,
        [self, submission, settings, targetThreadId = threadId](const QString& resumedThreadId,
                                                                const QString& error) {
            if (!self)
                return;
            self->threadResumeInFlight = false;
            if (!error.isEmpty()) {
                if (self->selectedThreadId == targetThreadId)
                    self->showWriteError(error);
                else
                    self->showWriteError(
                        QStringLiteral("Thread %1 could not be resumed: %2")
                            .arg(targetThreadId, error));
                self->refreshControls();
                return;
            }
            self->startTurn(resumedThreadId, submission, settings);
        });
    if (immediateError) {
        threadResumeInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::resumeThreadForOpen(const QString& threadId,
                                          std::uint64_t expectedSelectionGeneration)
{
    threadResumeInFlight = true;
    automaticResumeThreadId = threadId;
    if (selectedThreadId == threadId)
        conversation->setWriteStatus(QStringLiteral("Resuming thread…"));
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.resumeThread(
        threadId,
        [self, targetThreadId = threadId, expectedSelectionGeneration](const QString&,
                                                                       const QString& error) {
            if (!self)
                return;
            // State projection may reach the coalesced UI before the operation
            // completion. In that case canonical reconciliation already
            // finished this resume and the late completion must be a no-op.
            if (self->automaticResumeThreadId != targetThreadId) {
                self->refreshState();
                return;
            }
            if (!error.isEmpty()) {
                self->threadResumeInFlight = false;
                self->automaticResumeThreadId.clear();
                if (self->selectedThreadId == targetThreadId
                    && self->selectionGeneration == expectedSelectionGeneration)
                    self->showWriteError(error);
                else
                    self->showWriteError(
                        QStringLiteral("Thread %1 could not be resumed: %2")
                            .arg(targetThreadId, error));
            } else if (self->selectedThreadId == targetThreadId
                       && self->selectionGeneration == expectedSelectionGeneration) {
                self->conversation->setWriteStatus(
                    QStringLiteral("Waiting for canonical thread state…"));
            }
            self->refreshState();
        });
    if (immediateError) {
        threadResumeInFlight = false;
        automaticResumeThreadId.clear();
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::forkThread(const QString& threadId,
                                 const ForkThreadSetup& setup,
                                 std::uint64_t expectedSelectionGeneration)
{
    threadMutationInFlight = true;
    conversation->setWriteStatus(QStringLiteral("Forking thread…"));
    refreshControls();
    ai::openai::codex::typed::ThreadForkParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    if (!setup.instructions.baseInstructions.isEmpty())
        parameters.baseInstructions = toUtf8(setup.instructions.baseInstructions);
    if (!setup.instructions.developerInstructions.isEmpty())
        parameters.developerInstructions = toUtf8(setup.instructions.developerInstructions);
    parameters.ephemeral = setup.temporary;
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.forkThread(
        std::move(parameters),
        [self, name = setup.name.trimmed(), expectedSelectionGeneration](const QString& forkedThreadId,
                                                                         const QString& error) {
            if (!self)
                return;
            self->threadMutationInFlight = false;
            if (!error.isEmpty()) {
                self->showWriteError(error);
                self->refreshControls();
                return;
            }
            const bool keepAutomaticSelection = self->selectionGeneration == expectedSelectionGeneration;
            if (keepAutomaticSelection) {
                self->newThreadIdAwaitingState = forkedThreadId;
                self->selectedThreadId = forkedThreadId;
                self->projectedAgentThreadId.clear();
                self->conversation->clearPrompt();
                self->conversation->setWriteStatus(QStringLiteral("Thread forked"));
            }
            if (!name.isEmpty()) {
                self->threadMutationInFlight = true;
                self->refreshControls();
                const auto renameError = self->frontendSession.renameThread(
                    forkedThreadId,
                    name,
                    [self, forkedThreadId](const QString& renameFailure) {
                        if (!self)
                            return;
                        self->threadMutationInFlight = false;
                        if (!renameFailure.isEmpty())
                            self->showWriteError(renameFailure);
                        else if (self->selectedThreadId == forkedThreadId)
                            self->conversation->setWriteStatus({});
                        self->refreshState();
                    });
                if (renameError) {
                    self->threadMutationInFlight = false;
                    self->showWriteError(*renameError);
                }
            } else if (keepAutomaticSelection) {
                self->conversation->setWriteStatus({});
            }
            self->refreshState();
        });
    if (immediateError) {
        threadMutationInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::resumeThreadWithOptions(const QString& threadId,
                                              const ResumeWithOptionsSetup& setup,
                                              std::uint64_t expectedSelectionGeneration)
{
    threadMutationInFlight = true;
    conversation->setWriteStatus(QStringLiteral("Resuming thread…"));
    refreshControls();
    ai::openai::codex::typed::ThreadResumeParams parameters;
    parameters.threadId = ai::openai::codex::typed::ThreadId{threadId.toStdString()};
    if (!setup.instructions.baseInstructions.isEmpty())
        parameters.baseInstructions = toUtf8(setup.instructions.baseInstructions);
    if (!setup.instructions.developerInstructions.isEmpty())
        parameters.developerInstructions = toUtf8(setup.instructions.developerInstructions);
    const QPointer<WorkbenchWidget> self(this);
    const auto immediateError = frontendSession.resumeThread(
        std::move(parameters),
        [self, expectedSelectionGeneration](const QString& resumedThreadId, const QString& error) {
            if (!self)
                return;
            self->threadMutationInFlight = false;
            if (!error.isEmpty()) {
                self->showWriteError(error);
            } else if (self->selectionGeneration == expectedSelectionGeneration) {
                self->selectedThreadId = resumedThreadId;
                self->projectedAgentThreadId.clear();
                self->conversation->setWriteStatus({});
                self->conversation->focusComposer();
            } else {
                self->conversation->setWriteStatus({});
            }
            self->refreshState();
        });
    if (immediateError) {
        threadMutationInFlight = false;
        showWriteError(*immediateError);
        refreshControls();
    }
}

void WorkbenchWidget::mutateThread(PendingAction action,
                                   const QString& threadId,
                                   const QString& value)
{
    const auto& state = frontendSession.state();
    const auto* thread = state.thread(threadId.toStdString());
    if (!thread) {
        showWriteError(QStringLiteral("The selected thread is no longer available"));
        return;
    }
    const ThreadActionAvailability available = detail::threadActionAvailability(state, *thread);
    if ((action == PendingAction::ArchiveThread && !available.archive)
        || (action == PendingAction::UnarchiveThread && !available.unarchive)
        || (action == PendingAction::DeleteThread && !available.remove)
        || (action == PendingAction::RenameThread && !available.rename)) {
        showWriteError(QStringLiteral("That thread action is no longer available"));
        return;
    }

    threadMutationInFlight = true;
    refreshControls();
    const QPointer<WorkbenchWidget> self(this);
    const auto completion = [self, action, threadId](const QString& error) {
        if (!self)
            return;
        self->threadMutationInFlight = false;
        if (!error.isEmpty()) {
            self->showWriteError(error);
        } else {
            self->conversation->setWriteStatus({});
        }
        self->refreshState();
    };

    std::optional<QString> immediateError;
    switch (action) {
        case PendingAction::RenameThread:
            immediateError = frontendSession.renameThread(threadId, value, completion);
            break;
        case PendingAction::ArchiveThread:
            immediateError = frontendSession.archiveThread(threadId, completion);
            break;
        case PendingAction::UnarchiveThread:
            immediateError = frontendSession.unarchiveThread(threadId, completion);
            break;
        case PendingAction::DeleteThread:
            immediateError = frontendSession.deleteThread(threadId, completion);
            break;
        default:
            threadMutationInFlight = false;
            return;
    }
    if (immediateError) {
        threadMutationInFlight = false;
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
    pendingAttachments.clear();
    pendingAttachmentWorkspace.clear();
    pendingThreadId.clear();
    pendingTurnId.clear();
    pendingThreadValue.clear();
    pendingNewThreadSetup.reset();
    pendingForkThreadSetup.reset();
    pendingResumeSetup.reset();
    pendingTurnDraft = {};
    submittedTurnSettings.reset();
    turnThreadIdAwaitingState.clear();
    turnIdAwaitingState.clear();
    automaticResumeThreadId.clear();
    automaticResumeAttemptedThreadIds.clear();
    pendingSelectionGeneration = 0;
    newThreadIdAwaitingState.clear();
    controllerAcquireInFlight = false;
    threadStartInFlight = false;
    threadResumeInFlight = false;
    turnStartInFlight = false;
    turnSteerInFlight = false;
    interruptInFlight = false;
    threadMutationInFlight = false;
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
