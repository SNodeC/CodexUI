// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ConversationWidget.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
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

QWidget* badge(const QString& text, const QString& background, const QString& foreground,
               int width, int height = 22)
{
    auto* frame = new QFrame;
    frame->setFixedSize(width, height);
    frame->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(background).arg(height > 20 ? 6 : 5));
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* copy = textLabel(text);
    copy->setAlignment(Qt::AlignCenter);
    copy->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(foreground));
    layout->addWidget(copy);
    return frame;
}

QFrame* userPrompt()
{
    auto* card = new QFrame;
    card->setProperty("kind", "raised");
    card->setFixedHeight(70);
    card->setStyleSheet(QStringLiteral("background:#181c21;border-radius:10px;"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(4);
    auto* copy = textLabel(QStringLiteral("Implement the remaining protocol lifecycle work. Keep the public API minimal,\n"
                                          "preserve ABI where possible, and verify the final architecture."), "body");
    copy->setWordWrap(true);
    copy->setStyleSheet(QStringLiteral("font-size:13px;line-height:20px;"));
    layout->addWidget(copy);
    return card;
}

void addActivityRow(QVBoxLayout* rows, const QString& glyph, const QString& glyphColor,
                    const QString& title, const QString& detail = {}, const QString& tail = {},
                    QWidget* status = nullptr)
{
    auto* line = new QWidget;
    line->setFixedHeight(34);
    auto* layout = new QHBoxLayout(line);
    layout->setContentsMargins(2, 0, 4, 0);
    layout->setSpacing(8);
    auto* symbol = textLabel(glyph);
    symbol->setFixedWidth(14);
    symbol->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(glyphColor));
    layout->addWidget(symbol);
    auto* titleLabel = textLabel(title);
    titleLabel->setStyleSheet(QStringLiteral("font-size:12px;font-weight:500;"));
    layout->addWidget(titleLabel);
    if (status)
        layout->addWidget(status);
    if (!detail.isEmpty()) {
        layout->addSpacing(18);
        auto* detailLabel = textLabel(detail, "meta");
        detailLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(detailLabel, 1);
    } else {
        layout->addStretch();
    }
    if (!tail.isEmpty())
        layout->addWidget(textLabel(tail, "meta"));
    rows->addWidget(line);
}

QFrame* activityCard()
{
    auto* card = new QFrame;
    card->setProperty("kind", "panel");
    card->setStyleSheet(QStringLiteral("QFrame{background:#13161a;border-radius:10px;}"));
    card->setFixedHeight(292);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 21, 16, 0);
    layout->setSpacing(0);

    auto* header = new QHBoxLayout;
    auto* title = textLabel(QStringLiteral("Activity"));
    title->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;"));
    header->addWidget(title);
    header->addStretch();
    auto* progress = new QProgressBar;
    progress->setRange(0, 6);
    progress->setValue(3);
    progress->setTextVisible(false);
    progress->setFixedSize(132, 6);
    progress->setStyleSheet(QStringLiteral("QProgressBar{background:#181c21;border:0;border-radius:3px;}"
                                           "QProgressBar::chunk{background:#7a63e0;border-radius:3px;}"));
    header->addWidget(progress);
    header->addSpacing(8);
    header->addWidget(textLabel(QStringLiteral("Plan 3 / 6"), "small"));
    header->addSpacing(6);
    header->addWidget(textLabel(QStringLiteral("24 actions"), "small"));
    layout->addLayout(header);
    layout->addSpacing(9);
    layout->addWidget(divider());
    layout->addSpacing(3);

    addActivityRow(layout, QStringLiteral("✓"), QStringLiteral("#40c27d"), QStringLiteral("Reasoning"),
                   QStringLiteral("Mapped terminal-state ownership and cancellation paths"));
    addActivityRow(layout, QStringLiteral("✓"), QStringLiteral("#40c27d"), QStringLiteral("Read 7 files"));
    addActivityRow(layout, QStringLiteral("↳"), QStringLiteral("#4f94f5"),
                   QStringLiteral("Aristotle · Architecture audit"), {}, {},
                   badge(QStringLiteral("complete"), QStringLiteral("#143321"), QStringLiteral("#40c27d"), 74, 20));
    addActivityRow(layout, QStringLiteral("↳"), QStringLiteral("#4f94f5"),
                   QStringLiteral("Curie · Test analysis"), {}, {},
                   badge(QStringLiteral("running"), QStringLiteral("#14263d"), QStringLiteral("#4f94f5"), 66, 20));
    addActivityRow(layout, QStringLiteral("✓"), QStringLiteral("#40c27d"),
                   QStringLiteral("cmake --build build-debug --parallel 8"), {}, QStringLiteral("18.4s · exit 0"));
    addActivityRow(layout, QStringLiteral("✓"), QStringLiteral("#40c27d"),
                   QStringLiteral("Changed 3 files"), {}, QStringLiteral("+74  −21"));
    addActivityRow(layout, QStringLiteral("●"), QStringLiteral("#4f94f5"),
                   QStringLiteral("Turing · Build investigation"));
    layout->addStretch();
    return card;
}

QFrame* turnSummary()
{
    auto* strip = new QFrame;
    strip->setProperty("kind", "summary");
    strip->setFixedHeight(36);
    auto* row = new QHBoxLayout(strip);
    row->setContentsMargins(14, 0, 14, 0);
    row->setSpacing(20);
    auto* turn = textLabel(QStringLiteral("TURN 17"), "small");
    turn->setStyleSheet(QStringLiteral("color:#949ead;font-size:9px;font-weight:600;"));
    row->addWidget(turn);
    row->addWidget(badge(QStringLiteral("RUNNING"), QStringLiteral("#143321"), QStringLiteral("#40c27d"), 70));
    row->addSpacing(12);
    for (const auto& item : {QStringLiteral("3 agents"), QStringLiteral("3 files"), QStringLiteral("1 command")}) {
        auto* itemLabel = textLabel(item);
        itemLabel->setStyleSheet(QStringLiteral("font-size:9px;font-weight:500;"));
        row->addWidget(itemLabel);
    }
    row->addStretch();
    row->addWidget(textLabel(QStringLiteral("4m 32s"), "small"));
    return strip;
}

} // namespace

ConversationWidget::ConversationWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("conversation"));
    setStyleSheet(QStringLiteral("QWidget#conversation{background:#0e1013;}"));
    setMinimumWidth(480);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 14, 24, 0);
    root->setSpacing(0);

    auto* context = new QHBoxLayout;
    context->setSpacing(10);
    context->addWidget(badge(QStringLiteral("THREAD"), QStringLiteral("#181c21"), QStringLiteral("#949ead"), 54, 18));
    contextPath = textLabel(QStringLiteral("Waiting for synchronized thread"), "small");
    contextPath->setStyleSheet(QStringLiteral("color:#949ead;font-size:9px;font-weight:500;"));
    context->addWidget(contextPath);
    context->addStretch();
    root->addLayout(context);
    root->addSpacing(2);
    threadTitle = textLabel(QStringLiteral("No synchronized thread"), "heading");
    threadTitle->setTextFormat(Qt::PlainText);
    root->addWidget(threadTitle);
    threadDetail = textLabel(QStringLiteral("Detailed conversation below remains deterministic presentation data"), "meta");
    threadDetail->setTextFormat(Qt::PlainText);
    root->addWidget(threadDetail);
    root->addSpacing(7);
    root->addWidget(divider());
    root->addSpacing(7);
    root->addWidget(turnSummary());
    root->addSpacing(3);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget;
    content->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* conversation = new QVBoxLayout(content);
    conversation->setContentsMargins(0, 0, 0, 16);
    conversation->setSpacing(0);
    conversation->setAlignment(Qt::AlignTop);

    conversation->addWidget(textLabel(QStringLiteral("YOU"), "section"));
    conversation->addSpacing(2);
    conversation->addWidget(userPrompt());
    conversation->addSpacing(14);
    conversation->addWidget(textLabel(QStringLiteral("CODEX"), "section"));
    conversation->addSpacing(0);
    auto* firstResponse = textLabel(QStringLiteral("I'll inspect the lifecycle ownership and existing tests first, then make the smallest\n"
                                                    "coherent change."), "body");
    firstResponse->setWordWrap(true);
    conversation->addWidget(firstResponse);
    conversation->addSpacing(26);
    conversation->addWidget(activityCard());
    conversation->addSpacing(15);
    conversation->addWidget(textLabel(QStringLiteral("CODEX"), "section"));
    conversation->addSpacing(0);
    auto* secondResponse = textLabel(QStringLiteral("The ownership path is now unified. Curie is checking the regression coverage while\n"
                                                     "Turing verifies the build boundary."), "body");
    secondResponse->setWordWrap(true);
    conversation->addWidget(secondResponse);
    conversation->addSpacing(45);

    composer = new QFrame;
    composer->setProperty("kind", "composer");
    composer->setProperty("focused", false);
    composer->setStyleSheet(QStringLiteral("background:#13161a;border:2px solid transparent;border-radius:10px;"));
    composer->setFixedHeight(100);
    auto* composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(16, 12, 12, 10);
    composerLayout->setSpacing(4);
    editor = new QPlainTextEdit;
    editor->setPlaceholderText(QStringLiteral("Ask Codex…"));
    editor->setMaximumBlockCount(20);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editor->installEventFilter(this);
    composerLayout->addWidget(editor, 1);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto* attach = new QPushButton(QStringLiteral("Attach"));
    attach->setProperty("kind", "subtle");
    attach->setStyleSheet(QStringLiteral("text-align:left;padding:0;"));
    attach->setFixedSize(54, 24);
    actions->addWidget(attach);
    actions->addStretch();
    actions->addWidget(textLabel(QStringLiteral("Primary+Enter to send"), "meta"));
    auto* stop = new QPushButton(QStringLiteral("Stop"));
    stop->setProperty("kind", "stop");
    stop->setStyleSheet(QStringLiteral("QPushButton{background:#521a1a;color:#ffd1d1;border-radius:7px;}"
                                       "QPushButton:hover{background:#672222;}"));
    stop->setFixedSize(66, 32);
    actions->addWidget(stop);
    composerLayout->addLayout(actions);
    conversation->addWidget(composer);

    connect(stop, &QPushButton::clicked, this, [stop] {
        stop->setText(stop->text() == QStringLiteral("Stop") ? QStringLiteral("Stopped") : QStringLiteral("Stop"));
    });

    editor->setFocus(Qt::OtherFocusReason);

    scroll->setWidget(content);
    root->addWidget(scroll, 1);
}

void ConversationWidget::setThreadIdentity(const QString& id, const QString& title, const QString& detail)
{
    contextPath->setText(id.isEmpty() ? QStringLiteral("No thread selected") : id);
    contextPath->setToolTip(id);
    threadTitle->setText(title.isEmpty() ? QStringLiteral("No synchronized thread") : title);
    threadTitle->setToolTip(title);
    threadDetail->setText(detail.isEmpty()
                              ? QStringLiteral("Detailed conversation below remains deterministic presentation data")
                              : detail);
    threadDetail->setToolTip(detail);
}

bool ConversationWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editor && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        const auto border = event->type() == QEvent::FocusIn ? QStringLiteral("#4f94f5")
                                                             : QStringLiteral("transparent");
        composer->setStyleSheet(QStringLiteral("background:#13161a;border:2px solid %1;border-radius:10px;").arg(border));
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace codexui
