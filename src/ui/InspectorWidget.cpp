// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/InspectorWidget.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

namespace codexui {
namespace {

QLabel* textLabel(const QString& text, const char* kind = nullptr)
{
    auto* result = new QLabel(text);
    if (kind)
        result->setProperty("kind", kind);
    return result;
}

QFrame* divider()
{
    auto* line = new QFrame;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:#2b3038;"));
    return line;
}

QFrame* dot(const QString& color, int size = 7)
{
    auto* result = new QFrame;
    result->setFixedSize(size, size);
    result->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(color).arg(size / 2));
    return result;
}

QWidget* agentRow(int indent, const QString& title, const QString& subtitle,
                  const QString& color, const QString& state = {})
{
    auto* widget = new QWidget;
    widget->setFixedHeight(subtitle.isEmpty() ? 32 : 48);
    auto* row = new QHBoxLayout(widget);
    row->setContentsMargins(indent, 0, 4, 0);
    row->setSpacing(8);
    if (indent > 0) {
        auto* branch = textLabel(indent > 30 ? QStringLiteral("└─") : QStringLiteral("├─"));
        branch->setFixedWidth(18);
        branch->setStyleSheet(QStringLiteral("color:#2b3038;font-size:12px;"));
        row->addWidget(branch);
    }
    row->addWidget(dot(color, indent == 0 ? 8 : 7), 0, Qt::AlignTop);
    auto* copy = new QVBoxLayout;
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(2);
    auto* name = textLabel(title);
    name->setStyleSheet(QStringLiteral("font-size:%1px;font-weight:%2;")
                            .arg(indent == 0 ? 13 : 12).arg(indent == 0 ? 600 : 500));
    copy->addWidget(name);
    if (!subtitle.isEmpty())
        copy->addWidget(textLabel(subtitle, "meta"));
    row->addLayout(copy, 1);
    if (!state.isEmpty()) {
        auto* stateLabel = textLabel(state);
        stateLabel->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:500;").arg(color));
        row->addWidget(stateLabel, 0, Qt::AlignTop);
    }
    return widget;
}

void addFact(QGridLayout* grid, int row, const QString& name, const QString& value, bool running = false)
{
    grid->addWidget(textLabel(name, "meta"), row, 0);
    auto* valueLabel = textLabel(value);
    valueLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;font-weight:500;")
                                  .arg(running ? QStringLiteral("#40c27d") : QStringLiteral("#e8edf2")));
    grid->addWidget(valueLabel, row, 1);
}

QWidget* placeholderView(const QString& title, const QString& copy)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 18, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(textLabel(title.toUpper(), "section"));
    auto* card = new QFrame;
    card->setProperty("kind", "raised");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 14, 14, 14);
    auto* body = textLabel(copy, "muted");
    body->setWordWrap(true);
    cardLayout->addWidget(body);
    layout->addWidget(card);
    layout->addStretch();
    return page;
}

QWidget* agentsView()
{
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget;
    page->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 14, 0, 24);
    layout->setSpacing(0);

    layout->addWidget(textLabel(QStringLiteral("AGENT TREE"), "section"));
    layout->addSpacing(8);
    layout->addWidget(agentRow(0, QStringLiteral("Main agent"), {}, QStringLiteral("#40c27d"), QStringLiteral("Running")));
    layout->addWidget(agentRow(18, QStringLiteral("Aristotle"), QStringLiteral("Architecture audit"),
                               QStringLiteral("#40c27d"), QStringLiteral("✓")));
    layout->addWidget(agentRow(18, QStringLiteral("Curie"), QStringLiteral("Test analysis"),
                               QStringLiteral("#4f94f5"), QStringLiteral("●")));
    layout->addWidget(agentRow(44, QStringLiteral("Turing"), QStringLiteral("Build investigation"),
                               QStringLiteral("#4f94f5")));
    layout->addWidget(agentRow(18, QStringLiteral("Hopper"), QStringLiteral("Documentation · waiting"),
                               QStringLiteral("#949ead")));
    layout->addSpacing(32);
    layout->addWidget(divider());
    layout->addSpacing(16);
    layout->addWidget(textLabel(QStringLiteral("SELECTED AGENT"), "section"));
    layout->addSpacing(9);
    auto* curie = textLabel(QStringLiteral("Curie"));
    curie->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(curie);
    layout->addWidget(textLabel(QStringLiteral("Test analysis"), "muted"));
    layout->addSpacing(28);

    auto* facts = new QGridLayout;
    facts->setContentsMargins(0, 0, 0, 0);
    facts->setHorizontalSpacing(26);
    facts->setVerticalSpacing(9);
    facts->setColumnMinimumWidth(0, 84);
    addFact(facts, 0, QStringLiteral("Model"), QStringLiteral("gpt-5.6"));
    addFact(facts, 1, QStringLiteral("Reasoning"), QStringLiteral("high"));
    addFact(facts, 2, QStringLiteral("Status"), QStringLiteral("Running · 1m 18s"), true);
    layout->addLayout(facts);
    layout->addSpacing(32);
    auto* current = textLabel(QStringLiteral("Current activity"), "section");
    current->setStyleSheet(QStringLiteral("color:#949ead;font-size:10px;font-weight:600;"));
    layout->addWidget(current);
    layout->addSpacing(9);

    auto* activity = new QFrame;
    activity->setProperty("kind", "raised");
    activity->setFixedHeight(74);
    activity->setStyleSheet(QStringLiteral("background:#181c21;border-radius:8px;"));
    auto* activityLayout = new QVBoxLayout(activity);
    activityLayout->setContentsMargins(14, 11, 14, 11);
    activityLayout->setSpacing(6);
    auto* reviewing = textLabel(QStringLiteral("Reviewing lifecycle regression tests"));
    reviewing->setStyleSheet(QStringLiteral("font-size:11px;font-weight:500;"));
    activityLayout->addWidget(reviewing);
    activityLayout->addWidget(textLabel(QStringLiteral("5 actions · 2 files inspected"), "meta"));
    layout->addWidget(activity);
    layout->addSpacing(1);

    auto* open = new QPushButton(QStringLiteral("Open Curie thread                                      ↗"));
    open->setProperty("kind", "agentLink");
    open->setStyleSheet(QStringLiteral("QPushButton{background:#1a2940;color:#4f94f5;border-radius:7px;text-align:left;}"
                                        "QPushButton:hover{background:#223653;}"));
    open->setFixedHeight(34);
    layout->addWidget(open);
    QObject::connect(open, &QPushButton::clicked, open, [open] {
        open->setText(QStringLiteral("Curie thread previewed locally                         ↗"));
    });
    layout->addSpacing(0);
    layout->addWidget(divider());
    layout->addSpacing(16);
    layout->addWidget(textLabel(QStringLiteral("SESSION"), "section"));
    layout->addSpacing(8);
    layout->addWidget(textLabel(QStringLiteral("3 running agents")));
    layout->addSpacing(5);
    layout->addWidget(textLabel(QStringLiteral("1 waiting"), "muted"));
    layout->addStretch();

    auto* attentionRow = new QHBoxLayout;
    attentionRow->addStretch();
    auto* attention = new QLabel(QStringLiteral("2 need attention"));
    attention->setAlignment(Qt::AlignCenter);
    attention->setStyleSheet(QStringLiteral("background:#3d2912;color:#f5a83b;border-radius:7px;font-size:9px;font-weight:600;"));
    attention->setFixedSize(118, 30);
    attentionRow->addWidget(attention);
    layout->addLayout(attentionRow);

    scroll->setWidget(page);
    return scroll;
}

} // namespace

InspectorWidget::InspectorWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("inspector"));
    setStyleSheet(QStringLiteral("QWidget#inspector{background:#13161a;}"));
    setMinimumWidth(300);
    setMaximumWidth(520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 20, 0);
    root->setSpacing(0);

    auto* header = new QHBoxLayout;
    header->addWidget(textLabel(QStringLiteral("INSPECTOR · PREVIEW"), "section"));
    header->addStretch();
    auto* hide = new QPushButton(QStringLiteral("Hide"));
    hide->setProperty("kind", "subtle");
    hide->setFixedSize(58, 24);
    header->addWidget(hide);
    root->addLayout(header);
    root->addSpacing(7);

    auto* tabs = new QTabBar;
    tabs->setExpanding(false);
    tabs->addTab(QStringLiteral("Plan"));
    tabs->addTab(QStringLiteral("Agents"));
    tabs->addTab(QStringLiteral("Changes"));
    tabs->addTab(QStringLiteral("Info"));
    tabs->setCurrentIndex(1);
    root->addWidget(tabs);
    root->addSpacing(7);
    root->addWidget(divider());

    auto* pages = new QStackedWidget;
    pages->addWidget(placeholderView(QStringLiteral("Plan"),
                                     QStringLiteral("Plan 3 / 6\nLifecycle refactoring is in progress.")));
    pages->addWidget(agentsView());
    pages->addWidget(placeholderView(QStringLiteral("Changes"),
                                     QStringLiteral("3 files changed\n+74 additions · −21 deletions")));
    pages->addWidget(placeholderView(QStringLiteral("Info"),
                                     QStringLiteral("A1.8 protocol lifecycle\ngpt-5.6 · high · local")));
    pages->setCurrentIndex(1);
    root->addWidget(pages, 1);

    connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(hide, &QPushButton::clicked, this, &InspectorWidget::hideRequested);
}

} // namespace codexui
