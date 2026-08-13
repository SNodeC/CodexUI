// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/SidebarWidget.h"

#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <functional>

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
    result->setFixedHeight(attention ? 26 : 18);
    return result;
}

class ThreadRow final : public QFrame
{
public:
    ThreadRow(QString title, QString details, QString extra, QString color, int height, QWidget* parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("threadRow"));
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(height);
        setProperty("selected", false);
        dotColor = std::move(color);

        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(12, 7, 10, 7);
        row->setSpacing(10);
        dot = new QFrame;
        dot->setFixedSize(8, 8);
        dot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(dotColor));
        row->addWidget(dot, 0, Qt::AlignTop);

        auto* content = new QVBoxLayout;
        content->setContentsMargins(0, 0, 0, 0);
        content->setSpacing(3);
        auto* titleLabel = textLabel(title, "title");
        titleLabel->setStyleSheet(QStringLiteral("font-size:13px;font-weight:500;"));
        content->addWidget(titleLabel);
        if (!details.isEmpty()) {
            detailsLabel = textLabel(details, "meta");
            content->addWidget(detailsLabel);
        }
        if (!extra.isEmpty())
            content->addWidget(textLabel(extra, "meta"));
        row->addLayout(content, 1);
        updateStyle();
    }

    void setSelected(bool value)
    {
        selected = value;
        updateStyle();
    }

    void setAttentionText(bool value)
    {
        if (value && detailsLabel)
            detailsLabel->setStyleSheet(QStringLiteral("color:#f5a83b;font-size:11px;font-weight:600;"));
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

private:
    void updateStyle()
    {
        const auto background = selected ? QStringLiteral("#1a2940")
                                         : hovered ? QStringLiteral("#181c21") : QStringLiteral("transparent");
        setStyleSheet(QStringLiteral("QFrame#threadRow{background:%1;border-radius:8px;}").arg(background));
    }

    QFrame* dot = nullptr;
    QLabel* detailsLabel = nullptr;
    QString dotColor;
    bool selected = false;
    bool hovered = false;
};

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

    auto* newThread = new QPushButton(QStringLiteral("+  New thread"));
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
    auto* items = new QVBoxLayout(list);
    items->setContentsMargins(0, 0, 0, 0);
    items->setSpacing(4);
    items->addWidget(section(QStringLiteral("ACTIVE")));

    QList<ThreadRow*> rows;
    auto addRow = [&](const QString& title, const QString& detail, const QString& extra, const QString& color, int height) {
        auto* row = new ThreadRow(title, detail, extra, color, height);
        rows.append(row);
        items->addWidget(row);
        return row;
    };

    auto* active = addRow(QStringLiteral("A1.8 protocol lifecycle"),
                          QStringLiteral("Running · 3 agents · 4m"),
                          QStringLiteral("3 files changed"), QStringLiteral("#40c27d"), 76);
    active->setSelected(true);
    items->addWidget(section(QStringLiteral("NEEDS ATTENTION"), true));
    auto* attention = addRow(QStringLiteral("CI investigation"), QStringLiteral("Approval required"), {},
                             QStringLiteral("#f5a83b"), 54);
    attention->setAttentionText(true);
    items->addSpacing(8);
    addRow(QStringLiteral("FrontendService review"), QStringLiteral("Completed · 18m ago"), {},
           QStringLiteral("#40c27d"), 54);
    items->addSpacing(26);
    items->addWidget(section(QStringLiteral("REVIEWS")));
    addRow(QStringLiteral("PR #13 final audit"), QStringLiteral("Completed"), {},
           QStringLiteral("transparent"), 52);
    items->addSpacing(26);
    items->addWidget(section(QStringLiteral("ARCHIVED")));
    auto* archived = textLabel(QStringLiteral("12 threads"), "muted");
    archived->setContentsMargins(30, 6, 0, 0);
    archived->setFixedHeight(42);
    items->addWidget(archived);
    items->addStretch();

    for (auto* row : rows) {
        row->clicked = [rows](ThreadRow* selected) {
            for (auto* candidate : rows)
                candidate->setSelected(candidate == selected);
        };
    }

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
    auto* serverDot = new QFrame;
    serverDot->setFixedSize(8, 8);
    serverDot->setStyleSheet(QStringLiteral("background:#40c27d;border-radius:4px;"));
    serverRow->addWidget(serverDot, 0, Qt::AlignTop);
    auto* serverCopy = new QVBoxLayout;
    serverCopy->setSpacing(3);
    auto* connected = textLabel(QStringLiteral("App server connected"), "meta");
    connected->setStyleSheet(QStringLiteral("color:#e8edf2;font-size:11px;font-weight:500;"));
    serverCopy->addWidget(connected);
    serverCopy->addWidget(textLabel(QStringLiteral("Local · stdio"), "meta"));
    serverRow->addLayout(serverCopy, 1);
    root->addLayout(serverRow);

    connect(hide, &QPushButton::clicked, this, &SidebarWidget::hideRequested);
}

} // namespace codexui
