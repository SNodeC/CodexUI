// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/SidebarWidget.h"

#include <ai/openai/codex/frontend/client/State.h>

#include <QEnterEvent>
#include <QContextMenuEvent>
#include <QFrame>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTextDocument>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace codexui {
namespace {

QLabel* textLabel(const QString& text, const char* kind = nullptr)
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

QLabel* section(const QString& text, bool attention = false)
{
    auto* result = textLabel(text, attention ? "attentionSection" : "section");
    result->setMinimumHeight(attention ? 26 : 18);
    return result;
}

QString boundedRowText(QString value)
{
    constexpr qsizetype maximumCharacters = 512;
    if (value.size() <= maximumCharacters)
        return value;
    value.truncate(maximumCharacters);
    value.append(QChar(0x2026));
    return value;
}

class ThreadRow final : public QFrame
{
public:
    ThreadRow(QString stableId,
              QString title,
              QString details,
              QString color,
              ThreadActionAvailability actions,
              bool running,
              bool attention,
              bool archived,
              QWidget* parent = nullptr)
        : QFrame(parent)
        , stableId(std::move(stableId))
        , actions(actions)
        , running(running)
        , attention(attention)
        , archived(archived)
    {
        setObjectName(QStringLiteral("threadRow"));
        setProperty("threadId", this->stableId);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(64);
        setProperty("selected", false);

        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(12, 7, 10, 7);
        row->setSpacing(10);
        dot = new QFrame;
        dot->setFixedSize(8, 8);
        row->addWidget(dot, 0, Qt::AlignTop);

        auto* content = new QVBoxLayout;
        content->setContentsMargins(0, 0, 0, 0);
        content->setSpacing(4);
        titleLabel = textLabel({}, "title");
        titleLabel->setStyleSheet(QStringLiteral("font-size:13px;font-weight:500;"));
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        content->addWidget(titleLabel);
        detailsLabel = textLabel({}, "meta");
        detailsLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        content->addWidget(detailsLabel);
        row->addLayout(content, 1);
        updatePresentation(std::move(title),
                           std::move(details),
                           std::move(color),
                           this->actions,
                           this->running,
                           this->attention,
                           this->archived);
        updateStyle();
    }

    void updatePresentation(QString title,
                            QString details,
                            QString color,
                            ThreadActionAvailability nextActions,
                            bool nextRunning,
                            bool nextAttention,
                            bool nextArchived)
    {
        actions = nextActions;
        running = nextRunning;
        attention = nextAttention;
        archived = nextArchived;
        if (dotColor != color) {
            dotColor = std::move(color);
        }
        if (fullTitle != title) {
            fullTitle = std::move(title);
            titleLabel->setToolTip(plainTooltip(fullTitle));
            lastAvailableWidth = -1;
        }
        if (fullDetails != details) {
            fullDetails = std::move(details);
            detailsLabel->setToolTip(plainTooltip(fullDetails));
            detailsLabel->setVisible(!fullDetails.isEmpty());
            lastAvailableWidth = -1;
        }
        updateElision(width());
        updateStyle();
    }

    void setSelected(bool value)
    {
        if (selected == value)
            return;
        selected = value;
        updateStyle();
    }

    void setInteractionEnabled(bool enabled)
    {
        if (isEnabled() == enabled)
            return;
        setEnabled(enabled);
        updateStyle();
    }

    [[nodiscard]] const QString& id() const noexcept
    {
        return stableId;
    }

    std::function<void(ThreadRow*)> clicked;
    std::function<void(ThreadRow*, const QPoint&)> contextRequested;

protected:
    void enterEvent(QEnterEvent* event) override
    {
        hovered = true;
        updateStyle();
        QFrame::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        hovered = false;
        updateStyle();
        QFrame::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && clicked)
            clicked(this);
        QFrame::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        if (contextRequested)
            contextRequested(this, event->globalPos());
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
             || event->key() == Qt::Key_Space)
            && clicked)
        {
            clicked(this);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Menu && contextRequested)
        {
            contextRequested(this, mapToGlobal(rect().center()));
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent* event) override
    {
        focused = true;
        updateStyle();
        QFrame::focusInEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        focused = false;
        updateStyle();
        QFrame::focusOutEvent(event);
    }

public:
    [[nodiscard]] const ThreadActionAvailability& availability() const noexcept { return actions; }
    [[nodiscard]] bool isRunning() const noexcept { return running; }
    [[nodiscard]] bool isArchived() const noexcept { return archived; }
    void setContextOpen(bool value)
    {
        if (contextOpen == value)
            return;
        contextOpen = value;
        updateStyle();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        updateElision(event->size().width());
        QFrame::resizeEvent(event);
    }

private:
    void updateElision(int width)
    {
        const int availableWidth = qMax(0, width - 42);
        if (availableWidth != lastAvailableWidth) {
            lastAvailableWidth = availableWidth;
            const QString title = titleLabel->fontMetrics().elidedText(fullTitle, Qt::ElideRight, availableWidth);
            if (titleLabel->text() != title)
                titleLabel->setText(title);
            if (detailsLabel) {
                const QString details = detailsLabel->fontMetrics().elidedText(fullDetails, Qt::ElideRight, availableWidth);
                if (detailsLabel->text() != details)
                    detailsLabel->setText(details);
            }
        }
    }

    void updateStyle()
    {
        if (!isEnabled()) {
            dot->setStyleSheet(QStringLiteral("background:#98a2b3;border-radius:4px;"));
            setStyleSheet(QStringLiteral(
                "QFrame#threadRow{background:#f6f8fb;border:1px solid #d7dee8;border-radius:9px;}"
                "QFrame#threadRow QLabel[kind=\"title\"],"
                "QFrame#threadRow QLabel[kind=\"meta\"]{color:#98a2b3;}"));
            return;
        }
        QString background = archived ? QStringLiteral("#f6f8fb") : QStringLiteral("#ffffff");
        QString border = QStringLiteral("#d7dee8");
        if (attention && !selected) {
            background = QStringLiteral("#fff6df");
            border = QStringLiteral("#e5c77d");
        }
        if (hovered || contextOpen) {
            background = selected ? QStringLiteral("#d9e7ff") : QStringLiteral("#f1f5fb");
            border = selected ? QStringLiteral("#bfd3f9") : QStringLiteral("#b9c4d2");
        }
        if (selected) {
            background = hovered || contextOpen ? QStringLiteral("#d9e7ff") : QStringLiteral("#e5eeff");
            border = QStringLiteral("#bfd3f9");
        }
        if (focused)
            border = QStringLiteral("#2f6feb");
        const QString effectiveDot = attention ? QStringLiteral("#a76812")
                                               : archived ? QStringLiteral("#98a2b3") : dotColor;
        dot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(effectiveDot));
        setStyleSheet(QStringLiteral(
                          "QFrame#threadRow{background:%1;border:1px solid %2;border-radius:9px;}"
                          "QFrame#threadRow QLabel[kind=\"title\"]{color:%3;}"
                          "QFrame#threadRow QLabel[kind=\"meta\"]{color:#667085;}")
                          .arg(background,
                               border,
                               archived ? QStringLiteral("#667085") : QStringLiteral("#1d2633")));
    }

    QFrame* dot = nullptr;
    QLabel* titleLabel = nullptr;
    QLabel* detailsLabel = nullptr;
    QString stableId;
    QString fullTitle;
    QString fullDetails;
    QString dotColor;
    int lastAvailableWidth = -1;
    bool selected = false;
    bool hovered = false;
    bool focused = false;
    bool contextOpen = false;
    ThreadActionAvailability actions;
    bool running = false;
    bool attention = false;
    bool archived = false;
};

QString threadStatusColor(const std::optional<std::string>& status)
{
    if (!status)
        return QStringLiteral("#98a2b3");
    const QString value = QString::fromStdString(*status).toLower();
    if (value.contains(QStringLiteral("fail")) || value.contains(QStringLiteral("error"))
        || value.contains(QStringLiteral("approval")) || value.contains(QStringLiteral("attention")))
        return QStringLiteral("#a76812");
    if (value.contains(QStringLiteral("running")) || value.contains(QStringLiteral("active"))
        || value.contains(QStringLiteral("complete")))
        return QStringLiteral("#23845a");
    return QStringLiteral("#2f6feb");
}

bool threadMayBeRunning(const ai::openai::codex::frontend::client::State& state,
                        const ai::openai::codex::frontend::client::ThreadState& thread)
{
    for (const auto& turnId : thread.orderedTurns) {
        const auto* turn = state.turn(turnId);
        if (turn && turn->active && !turn->terminal)
            return true;
    }
    return false;
}

void clearLayout(QLayout* layout)
{
    while (auto* item = layout->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

namespace detail {

ThreadActionAvailability
threadActionAvailability(const ai::openai::codex::frontend::client::State& state,
                         const ai::openai::codex::frontend::client::ThreadState& thread)
{
    const bool running = threadMayBeRunning(state, thread);
    bool hasInterruptibleTurn = false;
    for (const auto& turnId : thread.orderedTurns) {
        const auto* turn = state.turn(turnId);
        hasInterruptibleTurn = hasInterruptibleTurn || (turn && turn->active && !turn->terminal);
    }

    ThreadActionAvailability result;
    const bool archived = thread.archived.value_or(false);
    const bool fullyActionable = thread.fullyLoaded;
    result.fork = fullyActionable;
    result.interrupt = hasInterruptibleTurn;
    result.resumeWithOptions = !running && fullyActionable;
    result.remove = !running && fullyActionable;
    result.archive = !running && fullyActionable && thread.archived.has_value() && !archived;
    result.unarchive = !running && archived;
    return result;
}

} // namespace detail

SidebarWidget::SidebarWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sidebar"));
    setStyleSheet(QStringLiteral("QWidget#sidebar{background:#f8fafc;}"));
    setMinimumWidth(220);
    setMaximumWidth(440);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 14, 10, 17);
    root->setSpacing(0);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(8, 0, 6, 8);
    header->addWidget(section(QStringLiteral("WORK")));
    header->addStretch();
    auto* hide = new QPushButton(QStringLiteral("Hide"));
    hide->setProperty("kind", "subtle");
    hide->setFixedSize(52, 24);
    header->addWidget(hide);
    root->addLayout(header);

    newThread = new QPushButton(QStringLiteral("+  New thread"));
    newThread->setFixedHeight(36);
    newThread->setStyleSheet(QStringLiteral(
        "QPushButton{background:#ffffff;color:#2f6feb;border:1px solid #bfd3f9;border-radius:8px;"
        "text-align:left;padding-left:14px;font-weight:600;}"
        "QPushButton:hover{background:#e5eeff;border-color:#2f6feb;}"
        "QPushButton:disabled{background:#f6f8fb;color:#98a2b3;border-color:#d7dee8;}"));
    root->addWidget(newThread);
    root->addSpacing(18);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* list = new QWidget;
    list->setStyleSheet(QStringLiteral("background:transparent;"));
    threadItems = new QVBoxLayout(list);
    threadItems->setContentsMargins(0, 0, 0, 0);
    threadItems->setSpacing(5);
    threadItems->setSizeConstraint(QLayout::SetMinAndMaxSize);
    threadItems->addWidget(section(QStringLiteral("THREADS")));
    threadItems->addWidget(textLabel(QStringLiteral("Waiting for synchronized state…"), "muted"));
    threadItems->addStretch();

    scroll->setWidget(list);
    root->addWidget(scroll, 1);

    auto* divider = new QFrame;
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background:#d7dee8;"));
    root->addWidget(divider);
    root->addSpacing(26);

    auto* serverRow = new QHBoxLayout;
    serverRow->setContentsMargins(8, 0, 0, 0);
    serverRow->setSpacing(10);
    serverDot = new QFrame;
    serverDot->setFixedSize(8, 8);
    serverDot->setStyleSheet(QStringLiteral("background:#23845a;border-radius:4px;"));
    serverRow->addWidget(serverDot, 0, Qt::AlignTop);
    auto* serverCopy = new QVBoxLayout;
    serverCopy->setSpacing(3);
    serverTitle = textLabel(QStringLiteral("Not connected"), "meta");
    serverTitle->setStyleSheet(QStringLiteral("color:#1d2633;font-size:11px;font-weight:500;"));
    serverCopy->addWidget(serverTitle);
    serverDetail = textLabel(QStringLiteral("Unix frontend"), "meta");
    serverCopy->addWidget(serverDetail);
    serverRow->addLayout(serverCopy, 1);
    root->addLayout(serverRow);

    connect(hide, &QPushButton::clicked, this, &SidebarWidget::hideRequested);
    connect(newThread, &QPushButton::clicked, this, &SidebarWidget::newThreadRequested);
}

void SidebarWidget::setThreads(const ai::openai::codex::frontend::client::State& state,
                               const QString& selectedThreadId)
{
    std::vector<ThreadPresentation> presentations;
    const auto threads = state.threads();
    presentations.reserve(threads.size());
    for (const auto& thread : threads) {
        const QString id = QString::fromStdString(thread.id.value);
        const QString title = boundedRowText(
            thread.title && !thread.title->empty() ? QString::fromStdString(*thread.title) : id);
        const bool running = threadMayBeRunning(state, thread);
        const bool archived = thread.archived.value_or(false);
        QStringList secondaryParts;
        if (archived)
            secondaryParts.append(QStringLiteral("Archived"));
        else if (!thread.fullyLoaded)
            secondaryParts.append(QStringLiteral("Loading"));
        else if (running)
            secondaryParts.append(QStringLiteral("Running"));
        else if (!ai::openai::codex::frontend::client::threadIsIdle(thread))
            secondaryParts.append(QStringLiteral("Ready to resume"));
        else
            secondaryParts.append(QStringLiteral("Idle"));
        secondaryParts.append(thread.orderedTurns.empty()
                                  ? QStringLiteral("Ready for first turn")
                                  : QStringLiteral("%1 turn%2")
                                        .arg(thread.orderedTurns.size())
                                        .arg(thread.orderedTurns.size() == 1
                                                 ? QString{}
                                                 : QStringLiteral("s")));
        if (thread.ephemeral.value_or(false))
            secondaryParts.append(QStringLiteral("Temporary"));
        const QString secondary = secondaryParts.join(QStringLiteral(" · "));
        bool attention = false;
        if (state.hasPendingRequestProjection()) {
            for (const auto& request : state.pendingRequests())
                attention = attention || (request.threadId && request.threadId->value == thread.id.value);
        }
        presentations.push_back({id,
                                 title,
                                 boundedRowText(secondary),
                                 threadStatusColor(thread.status),
                                 detail::threadActionAvailability(state, thread),
                                 running,
                                 attention,
                                 archived});
    }
    std::stable_partition(presentations.begin(), presentations.end(),
                          [](const ThreadPresentation& presentation) {
                              return !presentation.archived;
                          });

    if (threadsRendered && presentations == renderedThreads && selectedThreadId == renderedSelection)
        return;

    bool sameOrder = threadsRendered && presentations.size() == renderedThreads.size();
    for (std::size_t index = 0; sameOrder && index < presentations.size(); ++index) {
        sameOrder = presentations[index].id == renderedThreads[index].id
                    && presentations[index].archived == renderedThreads[index].archived;
    }
    if (sameOrder) {
        renderedThreads = presentations;
        renderedSelection = selectedThreadId;
        std::size_t index = 0;
        for (int itemIndex = 0; itemIndex < threadItems->count(); ++itemIndex) {
            auto* item = threadItems->itemAt(itemIndex);
            auto* row = item ? dynamic_cast<ThreadRow*>(item->widget()) : nullptr;
            if (!row)
                continue;
            if (index >= renderedThreads.size()
                || row->id() != renderedThreads[index].id) {
                sameOrder = false;
                break;
            }
            const auto& presentation = renderedThreads[index++];
            row->updatePresentation(presentation.title,
                                    presentation.details,
                                    presentation.color,
                                    presentation.actions,
                                    presentation.running,
                                    presentation.attention,
                                    presentation.archived);
            row->setSelected(presentation.id == selectedThreadId);
            row->setInteractionEnabled(threadInteractionEnabled);
        }
        sameOrder = sameOrder && index == renderedThreads.size();
        if (sameOrder)
            return;
    }
    threadsRendered = true;
    renderedThreads = std::move(presentations);
    renderedSelection = selectedThreadId;

    clearLayout(threadItems);

    if (renderedThreads.empty()) {
        threadItems->addWidget(section(QStringLiteral("ACTIVE")));
        auto* empty = textLabel(QStringLiteral("No synchronized threads"), "muted");
        empty->setContentsMargins(12, 8, 0, 0);
        threadItems->addWidget(empty);
    } else {
        const auto addGroup = [this, &selectedThreadId](const QString& heading, bool archived) {
            const bool hasRows = std::ranges::any_of(
                renderedThreads,
                [archived](const auto& presentation) {
                    return presentation.archived == archived;
                });
            if (!hasRows)
                return;
            threadItems->addWidget(section(heading));
            for (const auto& presentation : renderedThreads) {
                if (presentation.archived != archived)
                    continue;
            auto* row = new ThreadRow(presentation.id,
                                      presentation.title,
                                      presentation.details,
                                      presentation.color,
                                      presentation.actions,
                                      presentation.running,
                                      presentation.attention,
                                      presentation.archived);
            row->setSelected(presentation.id == selectedThreadId);
            row->setInteractionEnabled(threadInteractionEnabled);
            row->clicked = [this](ThreadRow* selected) { emit threadSelected(selected->id()); };
            row->contextRequested = [this](ThreadRow* source, const QPoint& globalPosition) {
                source->setContextOpen(true);
                auto* menu = new QMenu(this);
                const QString stableId = source->id();
                const QPointer<ThreadRow> guardedSource(source);
                const ThreadActionAvailability available = source->availability();
                const bool running = source->isRunning();
                const bool archived = source->isArchived();
                const auto add = [menu, this, stableId](const QString& text,
                                                        ThreadAction action,
                                                        bool enabled = true) {
                    QAction* item = menu->addAction(text);
                    item->setEnabled(enabled);
                    connect(item, &QAction::triggered, this,
                            [this, stableId, action] {
                                emit threadActionRequested(stableId, action);
                            });
                    return item;
                };
                add(QStringLiteral("Open"), ThreadAction::Open, available.open);
                add(QStringLiteral("Rename…"), ThreadAction::Rename, available.rename);
                add(QStringLiteral("Fork…"), ThreadAction::Fork, available.fork);
                menu->addSeparator();
                if (running)
                    add(QStringLiteral("Interrupt"), ThreadAction::Interrupt, available.interrupt);
                else
                    add(QStringLiteral("Resume with options…\tadvanced"),
                        ThreadAction::ResumeWithOptions,
                        available.resumeWithOptions);
                menu->addSeparator();
                if (archived)
                    add(QStringLiteral("Unarchive"), ThreadAction::Unarchive, available.unarchive);
                else
                    add(running ? QStringLiteral("Archive\trunning") : QStringLiteral("Archive"),
                        ThreadAction::Archive,
                        available.archive);
                QAction* remove = add(running ? QStringLiteral("Delete…\trunning")
                                              : QStringLiteral("Delete…"),
                                      ThreadAction::Delete,
                                      available.remove);
                QPixmap destructiveIcon = style()->standardIcon(QStyle::SP_TrashIcon).pixmap(16, 16);
                if (!destructiveIcon.isNull()) {
                    QPainter painter(&destructiveIcon);
                    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                    painter.fillRect(destructiveIcon.rect(), QColor(QStringLiteral("#b83a3a")));
                }
                remove->setIcon(QIcon(destructiveIcon));
                menu->addSeparator();
                QMenu* moreMenu = menu->addMenu(QStringLiteral("More"));
                QAction* copyId = moreMenu->addAction(QStringLiteral("Copy thread ID"));
                connect(copyId, &QAction::triggered, this,
                        [this, stableId] { emit threadActionRequested(stableId, ThreadAction::CopyId); });
                connect(menu, &QMenu::aboutToHide, this,
                        [guardedSource, menu] {
                            if (guardedSource)
                                guardedSource->setContextOpen(false);
                            menu->deleteLater();
                        });
                menu->popup(globalPosition);
            };
            threadItems->addWidget(row);
            }
            threadItems->addSpacing(12);
        };
        addGroup(QStringLiteral("ACTIVE"), false);
        addGroup(QStringLiteral("ARCHIVED"), true);
    }
    threadItems->addStretch();
}

void SidebarWidget::setConnectionStatus(const QString& title, const QString& connectionDetail, const QString& color)
{
    serverTitle->setText(title);
    serverDetail->setText(connectionDetail);
    serverDot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(color));
}

void SidebarWidget::setNewThreadEnabled(bool enabled)
{
    newThread->setEnabled(enabled);
}

void SidebarWidget::setThreadInteractionEnabled(bool enabled)
{
    if (threadInteractionEnabled == enabled)
        return;
    threadInteractionEnabled = enabled;
    for (int index = 0; index < threadItems->count(); ++index) {
        auto* item = threadItems->itemAt(index);
        if (auto* row = item ? dynamic_cast<ThreadRow*>(item->widget()) : nullptr)
            row->setInteractionEnabled(enabled);
    }
}

} // namespace codexui
