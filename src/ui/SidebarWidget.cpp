// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/SidebarWidget.h"

#include <ai/openai/codex/frontend/client/State.h>

#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace codexui {
namespace {

QLabel* textLabel(const QString& text, const char* kind = nullptr)
{
    auto* result = new QLabel(text);
    if (kind)
        result->setProperty("kind", kind);
    return result;
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
    ThreadRow(QString stableId, QString title, QString details, QString color, QWidget* parent = nullptr)
        : QFrame(parent)
        , stableId(std::move(stableId))
    {
        setObjectName(QStringLiteral("threadRow"));
        setProperty("threadId", this->stableId);
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(58);
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
        updatePresentation(std::move(title), std::move(details), std::move(color));
        updateStyle();
    }

    void updatePresentation(QString title, QString details, QString color)
    {
        if (dotColor != color) {
            dotColor = std::move(color);
            dot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(dotColor));
        }
        if (fullTitle != title) {
            fullTitle = std::move(title);
            titleLabel->setToolTip(fullTitle);
            lastAvailableWidth = -1;
        }
        if (fullDetails != details) {
            fullDetails = std::move(details);
            detailsLabel->setToolTip(fullDetails);
            detailsLabel->setVisible(!fullDetails.isEmpty());
            lastAvailableWidth = -1;
        }
        updateElision(width());
    }

    void setSelected(bool value)
    {
        if (selected == value)
            return;
        selected = value;
        updateStyle();
    }

    [[nodiscard]] const QString& id() const noexcept
    {
        return stableId;
    }

    std::function<void(ThreadRow*)> clicked;

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
        const auto background = selected ? QStringLiteral("#1a2940")
                                         : hovered ? QStringLiteral("#181c21") : QStringLiteral("transparent");
        setStyleSheet(QStringLiteral("QFrame#threadRow{background:%1;border-radius:8px;}").arg(background));
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
};

QString threadStatusColor(const std::optional<std::string>& status)
{
    if (!status)
        return QStringLiteral("#949ead");
    const QString value = QString::fromStdString(*status).toLower();
    if (value.contains(QStringLiteral("fail")) || value.contains(QStringLiteral("error"))
        || value.contains(QStringLiteral("approval")) || value.contains(QStringLiteral("attention")))
        return QStringLiteral("#f5a83b");
    if (value.contains(QStringLiteral("running")) || value.contains(QStringLiteral("active"))
        || value.contains(QStringLiteral("complete")))
        return QStringLiteral("#40c27d");
    return QStringLiteral("#4f94f5");
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

SidebarWidget::SidebarWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sidebar"));
    setStyleSheet(QStringLiteral("QWidget#sidebar{background:#13161a;}"));
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
    newThread->setProperty("kind", "primary");
    newThread->setFixedHeight(36);
    newThread->setStyleSheet(QStringLiteral("text-align:left;padding-left:14px;"));
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
    divider->setStyleSheet(QStringLiteral("background:#2b3038;"));
    root->addWidget(divider);
    root->addSpacing(26);

    auto* serverRow = new QHBoxLayout;
    serverRow->setContentsMargins(8, 0, 0, 0);
    serverRow->setSpacing(10);
    serverDot = new QFrame;
    serverDot->setFixedSize(8, 8);
    serverDot->setStyleSheet(QStringLiteral("background:#40c27d;border-radius:4px;"));
    serverRow->addWidget(serverDot, 0, Qt::AlignTop);
    auto* serverCopy = new QVBoxLayout;
    serverCopy->setSpacing(3);
    serverTitle = textLabel(QStringLiteral("Not connected"), "meta");
    serverTitle->setStyleSheet(QStringLiteral("color:#e8edf2;font-size:11px;font-weight:500;"));
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
        QString secondary;
        if (thread.status && !thread.status->empty())
            secondary = boundedRowText(QString::fromStdString(*thread.status));
        if (thread.preview && !thread.preview->empty()) {
            const QString preview = boundedRowText(QString::fromStdString(*thread.preview));
            if (preview != title) {
                if (!secondary.isEmpty())
                    secondary += QStringLiteral(" · ");
                secondary += preview;
            }
        }
        if (secondary.isEmpty())
            secondary = QStringLiteral("Synchronized thread");
        presentations.push_back({id, title, boundedRowText(secondary), threadStatusColor(thread.status)});
    }

    if (threadsRendered && presentations == renderedThreads && selectedThreadId == renderedSelection)
        return;

    bool sameOrder = threadsRendered && presentations.size() == renderedThreads.size();
    for (std::size_t index = 0; sameOrder && index < presentations.size(); ++index)
        sameOrder = presentations[index].id == renderedThreads[index].id;
    if (sameOrder) {
        renderedThreads = presentations;
        renderedSelection = selectedThreadId;
        for (std::size_t index = 0; index < renderedThreads.size(); ++index) {
            auto* item = threadItems->itemAt(static_cast<int>(index + 1));
            auto* row = item ? dynamic_cast<ThreadRow*>(item->widget()) : nullptr;
            if (!row) {
                sameOrder = false;
                break;
            }
            const auto& presentation = renderedThreads[index];
            row->updatePresentation(presentation.title, presentation.details, presentation.color);
            row->setSelected(presentation.id == selectedThreadId);
        }
        if (sameOrder)
            return;
    }
    threadsRendered = true;
    renderedThreads = std::move(presentations);
    renderedSelection = selectedThreadId;

    clearLayout(threadItems);
    threadItems->addWidget(section(QStringLiteral("THREADS")));

    if (renderedThreads.empty()) {
        auto* empty = textLabel(QStringLiteral("No synchronized threads"), "muted");
        empty->setContentsMargins(12, 8, 0, 0);
        threadItems->addWidget(empty);
    } else {
        for (const auto& presentation : renderedThreads) {
            auto* row = new ThreadRow(presentation.id, presentation.title, presentation.details, presentation.color);
            row->setSelected(presentation.id == selectedThreadId);
            row->clicked = [this](ThreadRow* selected) { emit threadSelected(selected->id()); };
            threadItems->addWidget(row);
        }
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

} // namespace codexui
