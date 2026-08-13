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

QWidget* makeStatusBar(QFrame*& codexStatusDot, QLabel*& synchronizationStatus)
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
    row->addStretch();
    auto* attention = label(QStringLiteral("2 attention"), "meta");
    attention->setStyleSheet(QStringLiteral("color:#f5a83b;font-size:10px;font-weight:600;"));
    row->addWidget(attention);
    return bar;
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
    layout->addWidget(makeStatusBar(codexStatusDot, synchronizationStatus));

    connect(sidebar, &SidebarWidget::hideRequested, this, [this] { setSidebarVisible(false); });
    connect(inspector, &InspectorWidget::hideRequested, this, [this] { setInspectorVisible(false); });
    connect(restoreSidebar, &QPushButton::clicked, this, [this] { setSidebarVisible(true); });
    connect(restoreInspector, &QPushButton::clicked, this, [this] { setInspectorVisible(true); });
    connect(sidebar, &SidebarWidget::threadSelected, this, &WorkbenchWidget::selectThread);
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
}

void WorkbenchWidget::refreshState()
{
    const auto& state = frontendSession.state();
    const auto threads = state.threads();

    if (!selectedThreadId.isEmpty()
        && state.thread(selectedThreadId.toStdString()) == nullptr)
        selectedThreadId.clear();
    if (selectedThreadId.isEmpty() && !threads.empty())
        selectedThreadId = QString::fromStdString(threads.front().id.value);

    sidebar->setThreads(state, selectedThreadId);

    const auto* selected = selectedThreadId.isEmpty()
                               ? nullptr
                               : state.thread(selectedThreadId.toStdString());
    if (!selected) {
        conversation->setThreadIdentity({}, {}, {});
        return;
    }

    const QString title = selected->title && !selected->title->empty()
                              ? QString::fromStdString(*selected->title)
                              : QString::fromStdString(selected->id.value);
    QStringList metadata;
    if (selected->status && !selected->status->empty())
        metadata.append(QString::fromStdString(*selected->status));
    if (selected->model)
        metadata.append(QString::fromStdString(selected->model->value));
    if (selected->cwd)
        metadata.append(QString::fromStdString(selected->cwd->value));
    if (metadata.isEmpty())
        metadata.append(QStringLiteral("Synchronized thread identity"));
    metadata.append(QStringLiteral("details below remain demo data"));

    conversation->setThreadIdentity(QString::fromStdString(selected->id.value), title,
                                    metadata.join(QStringLiteral(" · ")));
}

void WorkbenchWidget::selectThread(const QString& threadId)
{
    selectedThreadId = threadId;
    refreshState();
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
