// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ConversationWidget.h"

#include "ui/UpcomingTurnDock.h"

#include <ai/openai/codex/frontend/Messages.h>
#include <ai/openai/codex/frontend/client/State.h>

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QEvent>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSizePolicy>
#include <QStyle>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QEasingCurve>

#include <algorithm>
#include <optional>
#include <functional>
#include <variant>
#include <vector>

namespace codexui
{
namespace
{
namespace sdk = ai::openai::codex::frontend::client;
namespace frontend = ai::openai::codex::frontend;

constexpr qsizetype maximumRenderedTimelineTurns = 32;
constexpr qsizetype maximumRenderedTimelineItems = 256;
constexpr std::size_t maximumActivityItemsPerSegment = 16;
constexpr qsizetype largeMessageEditorThreshold = 64 * 1024;
constexpr int largeMessageEditorHeight = 240;

struct ActivityPresentation
{
    QString title;
    QString detail;
    QString output;
    QString status;
    QString tail;
    bool truncated = false;
};

QString fromUtf8(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString fromUtf8(const std::string& value)
{
    return QString::fromStdString(value);
}

QString humanize(QString value)
{
    value.replace(QLatin1Char('_'), QLatin1Char(' '));
    value.replace(QLatin1Char('-'), QLatin1Char(' '));
    for (qsizetype index = 1; index < value.size(); ++index)
    {
        if (value.at(index).isUpper() && value.at(index - 1).isLower())
        {
            value.insert(index, QLatin1Char(' '));
            ++index;
        }
    }
    if (!value.isEmpty()) value[0] = value.at(0).toUpper();
    return value;
}

QString compact(const QString& value, qsizetype maximum = 500)
{
    if (value.size() <= maximum) return value;
    return value.left(maximum).trimmed() + QStringLiteral("…");
}

QString plainTooltip(const QString& value)
{
    return Qt::convertFromPlainText(value, Qt::WhiteSpaceNormal);
}

QString singleLinePreview(QString value, qsizetype maximum = 180)
{
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return compact(value.simplified(), maximum);
}

QString compactId(const std::string& id)
{
    const QString value = fromUtf8(id);
    return value.size() > 12 ? value.left(6) + QChar(0x2026) + value.right(5) : value;
}

QLabel* textLabel(const QString& text, const char* kind = nullptr)
{
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    if (kind) result->setProperty("kind", kind);
    return result;
}

class WrappingLabel final : public QLabel
{
public:
    explicit WrappingLabel(const QString& text) : QLabel(text)
    {
        setTextFormat(Qt::PlainText);
        setWordWrap(true);
        QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    void setContent(const QString& text)
    {
        if (text == QLabel::text())
            return;
        heightCache.clear();
        QLabel::setText(text);
        updateGeometry();
    }

    int heightForWidth(int width) const override
    {
        const auto found = heightCache.constFind(width);
        if (found != heightCache.cend())
            return *found;
        const int height = QLabel::heightForWidth(width);
        heightCache.insert(width, height);
        return height;
    }

protected:
    void changeEvent(QEvent* event) override
    {
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            heightCache.clear();
        QLabel::changeEvent(event);
    }

private:
    mutable QHash<int, int> heightCache;
};

QLabel* wrappingLabel(const QString& text, const char* kind = nullptr)
{
    auto* result = new WrappingLabel(text);
    if (kind) result->setProperty("kind", kind);
    return result;
}

QWidget* messageContentWidget(const QString& text)
{
    if (text.size() <= largeMessageEditorThreshold)
    {
        auto* result = static_cast<WrappingLabel*>(wrappingLabel(text, "body"));
        result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return result;
    }

    auto* result = new QPlainTextEdit;
    result->setProperty("kind", "body");
    result->setReadOnly(true);
    result->setUndoRedoEnabled(false);
    result->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    result->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    result->setFixedHeight(largeMessageEditorHeight);
    result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    result->setPlainText(text);
    return result;
}

QString messageContentText(const QWidget* content)
{
    if (const auto* label = qobject_cast<const QLabel*>(content))
        return label->text();
    if (const auto* editor = qobject_cast<const QPlainTextEdit*>(content))
        return editor->toPlainText();
    return {};
}

void setMessageContentText(QWidget* content, const QString& text)
{
    if (auto* label = dynamic_cast<WrappingLabel*>(content))
        label->setContent(text);
    else if (auto* editor = qobject_cast<QPlainTextEdit*>(content); editor && editor->toPlainText() != text)
        editor->setPlainText(text);
}

QWidget* ensureMessageContentWidget(QVBoxLayout* layout, QWidget* content, const QString& text)
{
    const bool needsEditor = text.size() > largeMessageEditorThreshold;
    const bool hasEditor = qobject_cast<QPlainTextEdit*>(content) != nullptr;
    if (needsEditor == hasEditor)
        return content;

    QWidget* replacement = messageContentWidget(text);
    replacement->setObjectName(QStringLiteral("conversationMessageContent"));
    delete layout->replaceWidget(content, replacement);
    content->hide();
    content->deleteLater();
    return replacement;
}

QFrame* divider()
{
    auto* line = new QFrame;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:#d7dee8;"));
    return line;
}

QWidget* badge(const QString& text, const QString& background, const QString& foreground, int width = 0,
               int height = 22)
{
    auto* frame = new QFrame;
    frame->setFixedSize(width > 0 ? width : qMax(58, text.size() * 7 + 18), height);
    frame->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(background).arg(height > 20 ? 6 : 5));
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* copy = textLabel(text);
    copy->setAlignment(Qt::AlignCenter);
    copy->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(foreground));
    layout->addWidget(copy);
    return frame;
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0))
    {
        if (QLayout* child = item->layout())
            clearLayout(child);
        else
            delete item->widget();
        delete item;
    }
}

QString statusColor(const QString& status)
{
    const QString normalized = status.toLower();
    if (normalized.contains(QStringLiteral("fail")) || normalized.contains(QStringLiteral("error")))
        return QStringLiteral("#b83a3a");
    if (normalized.contains(QStringLiteral("complete")) || normalized.contains(QStringLiteral("success")) ||
        normalized == QStringLiteral("done"))
        return QStringLiteral("#23845a");
    if (normalized.contains(QStringLiteral("progress")) || normalized.contains(QStringLiteral("running")) ||
        normalized.contains(QStringLiteral("active")) || normalized.contains(QStringLiteral("stream")))
        return QStringLiteral("#2f6feb");
    if (normalized.contains(QStringLiteral("interrupt")) || normalized.contains(QStringLiteral("cancel")))
        return QStringLiteral("#a76812");
    return QStringLiteral("#667085");
}

QString statusGlyph(const QString& status)
{
    const QString normalized = status.toLower();
    if (normalized.contains(QStringLiteral("fail")) || normalized.contains(QStringLiteral("error")))
        return QStringLiteral("×");
    if (normalized.contains(QStringLiteral("complete")) || normalized.contains(QStringLiteral("success")) ||
        normalized == QStringLiteral("done"))
        return QStringLiteral("✓");
    if (normalized.contains(QStringLiteral("progress")) || normalized.contains(QStringLiteral("running")) ||
        normalized.contains(QStringLiteral("active")) || normalized.contains(QStringLiteral("stream")))
        return QStringLiteral("●");
    return QStringLiteral("•");
}

QString knownKindTitle(frontend::ThreadItemKind kind)
{
    switch (kind)
    {
        case frontend::ThreadItemKind::AgentMessage:
            return QStringLiteral("Agent message");
        case frontend::ThreadItemKind::CollabAgentToolCall:
            return QStringLiteral("Agent collaboration");
        case frontend::ThreadItemKind::CommandExecution:
            return QStringLiteral("Command execution");
        case frontend::ThreadItemKind::ContextCompaction:
            return QStringLiteral("Context compaction");
        case frontend::ThreadItemKind::DynamicToolCall:
            return QStringLiteral("Tool call");
        case frontend::ThreadItemKind::EnteredReviewMode:
            return QStringLiteral("Entered review mode");
        case frontend::ThreadItemKind::ExitedReviewMode:
            return QStringLiteral("Exited review mode");
        case frontend::ThreadItemKind::FileChange:
            return QStringLiteral("File changes");
        case frontend::ThreadItemKind::HookPrompt:
            return QStringLiteral("Hook prompt");
        case frontend::ThreadItemKind::ImageGeneration:
            return QStringLiteral("Image generation");
        case frontend::ThreadItemKind::ImageView:
            return QStringLiteral("Image viewed");
        case frontend::ThreadItemKind::McpToolCall:
            return QStringLiteral("MCP tool call");
        case frontend::ThreadItemKind::Plan:
            return QStringLiteral("Plan");
        case frontend::ThreadItemKind::Reasoning:
            return QStringLiteral("Reasoning");
        case frontend::ThreadItemKind::Sleep:
            return QStringLiteral("Wait");
        case frontend::ThreadItemKind::SubAgentActivity:
            return QStringLiteral("Subagent activity");
        case frontend::ThreadItemKind::UserMessage:
            return QStringLiteral("User message");
        case frontend::ThreadItemKind::WebSearch:
            return QStringLiteral("Web search");
    }
    return QStringLiteral("Item");
}

QString itemStatus(const sdk::ItemState& item)
{
    return item.status && !item.status->empty() ? humanize(fromUtf8(*item.status)) : QStringLiteral("Recorded");
}

QString truncationText(const sdk::ItemState& item)
{
    if (!item.contentTruncated && !item.truncated && item.omittedFields.empty()) return {};
    if (item.droppedContentBytes && *item.droppedContentBytes > 0)
        return QStringLiteral("Content truncated · %1 bytes omitted").arg(*item.droppedContentBytes);
    return QStringLiteral("Content truncated or omitted by the synchronized state");
}

QString userMessageTruncationText(const sdk::UserMessageSemanticView& message)
{
    // Non-text user-input details are intentionally outside this text-only
    // presentation. Their omission must not label complete retained prompt
    // text as truncated.
    return message.textTruncated ? QStringLiteral("Retained text is truncated") : QString{};
}

struct MessagePresentation
{
    QString status;
    QString statusColor;
    QString content;
    QString truncation;
    bool missing = false;
};

MessagePresentation messagePresentation(const sdk::ItemState& item, bool user)
{
    MessagePresentation result;
    const QString itemStatusText = itemStatus(item);
    result.status = itemStatusText;
    result.statusColor = statusColor(itemStatusText);

    if (!user)
    {
        const auto semantic = sdk::itemSemanticView(item);
        const auto* agent = semantic ? std::get_if<sdk::AgentMessageSemanticView>(&semantic->details) : nullptr;
        if (agent && agent->phase)
            result.status += QStringLiteral(" · ") + humanize(fromUtf8(*agent->phase));
    }

    const auto userMessage = user ? sdk::userMessageSemanticView(item) : std::nullopt;
    if (user)
    {
        if (!userMessage)
        {
            result.content = QStringLiteral("User message is unavailable");
            result.missing = true;
        }
        else if (userMessage->text.empty())
        {
            result.content = QStringLiteral("User message contains no retained text");
            result.missing = true;
        }
        else
        {
            result.content = fromUtf8(userMessage->text);
        }
    }
    else
    {
        result.content = item.agentText && !item.agentText->empty()
                             ? fromUtf8(*item.agentText)
                             : (item.summary ? fromUtf8(*item.summary) : QString{});
        if (result.content.isEmpty())
        {
            result.content = QStringLiteral("No retained message content");
            result.missing = true;
        }
    }

    // A valid typed user-message view is the authoritative statement about
    // retained text. Generic item-detail bounds may describe unrelated
    // metadata and must not turn a complete prompt into a truncation warning.
    result.truncation = userMessage ? userMessageTruncationText(*userMessage)
                                    : truncationText(item);
    return result;
}

void applyMessagePresentation(QLabel* status,
                              QWidget* content,
                              QLabel* truncation,
                              const MessagePresentation& presentation)
{
    if (status->text() != presentation.status)
        status->setText(presentation.status);
    const QString statusStyle =
        QStringLiteral("color:%1;font-size:9px;").arg(presentation.statusColor);
    if (status->styleSheet() != statusStyle)
        status->setStyleSheet(statusStyle);

    const QString kind = presentation.missing ? QStringLiteral("meta") : QStringLiteral("body");
    if (content->property("kind").toString() != kind)
    {
        content->setProperty("kind", kind);
        content->style()->unpolish(content);
        content->style()->polish(content);
    }
    setMessageContentText(content, presentation.content);

    if (truncation->text() != presentation.truncation)
        truncation->setText(presentation.truncation);
    truncation->setVisible(!presentation.truncation.isEmpty());
}

QString pendingRequestDetail(const sdk::State& state, const sdk::ItemState& item)
{
    for (const auto& request : state.pendingRequests())
    {
        if (!request.threadId || !request.turnId || !request.itemId || !item.threadId || !item.turnId
            || *request.threadId != *item.threadId || *request.turnId != *item.turnId
            || *request.itemId != item.id)
            continue;
        const auto view = sdk::pendingRequestPresentation(request);
        QString result = QStringLiteral("Awaiting %1").arg(humanize(fromUtf8(frontend::toString(view.kind))));
        if (view.fileChangeCount)
            result += QStringLiteral(" · %1 file changes").arg(*view.fileChangeCount);
        else if (view.parsedCommandCount)
            result += QStringLiteral(" · %1 commands").arg(*view.parsedCommandCount);
        if (view.truncated) result += QStringLiteral(" · details omitted");
        return result;
    }
    return {};
}

ActivityPresentation activityPresentation(const sdk::State& state,
                                          const sdk::ItemState& item,
                                          bool includeOutput = true)
{
    ActivityPresentation result;
    result.title = item.kind.known ? knownKindTitle(*item.kind.known) : QStringLiteral("Unknown item");
    result.status = itemStatus(item);
    result.truncated = item.truncated || item.contentTruncated || !item.omittedFields.empty();
    if (item.summary && !item.summary->empty()) result.detail = fromUtf8(*item.summary);

    const auto semantic = sdk::itemSemanticView(item);
    if (semantic)
    {
        if (const auto* command = std::get_if<sdk::CommandExecutionSemanticView>(&semantic->details))
        {
            if (command->command)
            {
                const QString fullCommand = fromUtf8(*command->command);
                result.title = singleLinePreview(fullCommand, 240);
                result.detail = QStringLiteral("Command:\n%1").arg(fullCommand);
            }
            if (command->cwd)
            {
                const QString cwd = QStringLiteral("Working directory:\n%1")
                                        .arg(fromUtf8(command->cwd->value));
                result.detail = result.detail.isEmpty() ? cwd
                                                        : result.detail + QStringLiteral("\n\n") + cwd;
            }
            if (command->status) result.status = humanize(fromUtf8(*command->status));
            QStringList tail;
            if (command->durationMs) tail.append(QStringLiteral("%1 ms").arg(*command->durationMs));
            if (command->exitCode) tail.append(QStringLiteral("exit %1").arg(*command->exitCode));
            result.tail = tail.join(QStringLiteral(" · "));
        }
        else if (const auto* changes = std::get_if<sdk::FileChangeSemanticView>(&semantic->details))
        {
            if (changes->changeCount)
                result.title = QStringLiteral("Changed %1 file entries").arg(*changes->changeCount);
            if (changes->status) result.status = humanize(fromUtf8(*changes->status));
            qsizetype redacted = 0;
            qsizetype omitted = 0;
            for (const auto& change : changes->changes)
            {
                redacted += change.pathRedacted ? 1 : 0;
                omitted += change.diffOmitted ? 1 : 0;
            }
            QStringList detail;
            if (redacted) detail.append(QStringLiteral("%1 paths redacted").arg(redacted));
            if (omitted) detail.append(QStringLiteral("%1 diffs omitted").arg(omitted));
            if (changes->changesTruncated) detail.append(QStringLiteral("change list truncated"));
            if (!detail.isEmpty()) result.detail = detail.join(QStringLiteral(" · "));
        }
        else if (const auto* tool = std::get_if<sdk::ToolCallSemanticView>(&semantic->details))
        {
            QStringList identity;
            if (tool->server) identity.append(fromUtf8(*tool->server));
            if (tool->nameSpace) identity.append(fromUtf8(*tool->nameSpace));
            if (tool->tool) identity.append(fromUtf8(*tool->tool));
            if (!identity.isEmpty()) result.title = identity.join(QStringLiteral(" · "));
            if (tool->status) result.status = humanize(fromUtf8(*tool->status));
            if (tool->hasResult)
            {
                const QString retained =
                    *tool->hasResult ? QStringLiteral("Result retained") : QStringLiteral("No retained result");
                result.detail = result.detail.isEmpty() ? retained : result.detail + QStringLiteral(" · ") + retained;
            }
        }
        else if (const auto* search = std::get_if<sdk::WebSearchSemanticView>(&semantic->details))
        {
            if (search->query) result.detail = fromUtf8(*search->query);
        }
        else if (const auto* collab = std::get_if<sdk::CollabAgentToolCallSemanticView>(&semantic->details))
        {
            if (collab->tool) result.title = QStringLiteral("Agent · %1").arg(fromUtf8(*collab->tool));
            if (collab->status) result.status = humanize(fromUtf8(*collab->status));
            QStringList detail;
            if (collab->receiverCount) detail.append(QStringLiteral("%1 receivers").arg(*collab->receiverCount));
            if (collab->agentStateCount) detail.append(QStringLiteral("%1 agent states").arg(*collab->agentStateCount));
            if (collab->senderThreadId)
                detail.append(QStringLiteral("from %1").arg(compactId(collab->senderThreadId->value)));
            if (!detail.isEmpty()) result.detail = detail.join(QStringLiteral(" · "));
        }
        else if (const auto* plan = std::get_if<sdk::PlanSemanticView>(&semantic->details))
        {
            if (plan->text) result.detail = fromUtf8(*plan->text);
            result.truncated = result.truncated || plan->textTruncated;
        }
        else if (const auto* agent = std::get_if<sdk::SubAgentActivitySemanticView>(&semantic->details))
        {
            if (agent->agentPath) result.title = fromUtf8(*agent->agentPath);
            QStringList detail;
            if (agent->kind) detail.append(humanize(fromUtf8(*agent->kind)));
            if (agent->agentThreadId)
                detail.append(QStringLiteral("thread %1").arg(compactId(agent->agentThreadId->value)));
            result.detail = detail.join(QStringLiteral(" · "));
        }
        result.truncated = result.truncated || semantic->truncated || !semantic->omittedFields.empty();
    }

    // Command execution is the common source, but file-change and future typed
    // activities may also carry the canonical command-output channel.
    if (includeOutput && item.commandOutput && !item.commandOutput->empty())
        result.output = fromUtf8(*item.commandOutput);

    if (item.kind.is(frontend::ThreadItemKind::Reasoning))
    {
        if (item.reasoningSummary && !item.reasoningSummary->empty())
            result.detail = fromUtf8(*item.reasoningSummary);
        else if (item.reasoningText && !item.reasoningText->empty())
            result.detail = fromUtf8(*item.reasoningText);
    }

    if (!item.kind.known)
    {
        result.detail = humanize(fromUtf8(item.kind.identity)) +
                        (result.detail.isEmpty() ? QString{} : QStringLiteral(" · ") + result.detail);
    }

    const QString pending = pendingRequestDetail(state, item);
    if (!pending.isEmpty())
    {
        result.detail = result.detail.isEmpty() ? pending : result.detail + QStringLiteral(" · ") + pending;
        result.status = QStringLiteral("Awaiting input");
    }
    return result;
}

QToolButton* disclosureButton(bool expanded, const QString& accessibleName)
{
    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("activityDisclosure"));
    button->setAutoRaise(true);
    button->setCheckable(true);
    button->setArrowType(Qt::NoArrow);
    button->setIcon(button->style()->standardIcon(expanded ? QStyle::SP_ArrowDown
                                                           : QStyle::SP_ArrowRight));
    button->setIconSize(QSize(12, 12));
    button->setChecked(expanded);
    button->setToolTip(expanded ? QStringLiteral("Collapse") : QStringLiteral("Expand"));
    button->setAccessibleName(accessibleName);
    button->setFixedSize(22, 22);
    button->setStyleSheet(QStringLiteral(
        "QToolButton#activityDisclosure{background:transparent;border:1px solid transparent;"
        "border-radius:5px;padding:0;}"
        "QToolButton#activityDisclosure:hover{background:#f1f5fb;border-color:#d7dee8;}"
        "QToolButton#activityDisclosure:pressed,QToolButton#activityDisclosure:checked{"
        "background:#e5eeff;border-color:#bfd3f9;}"
        "QToolButton#activityDisclosure:focus{border:1px solid #2f6feb;}"));
    return button;
}

void setDisclosureState(QToolButton* disclosure, QWidget* details, bool expanded)
{
    disclosure->setChecked(expanded);
    disclosure->setIcon(disclosure->style()->standardIcon(expanded ? QStyle::SP_ArrowDown
                                                                   : QStyle::SP_ArrowRight));
    disclosure->setToolTip(expanded ? QStringLiteral("Collapse") : QStringLiteral("Expand"));
    details->setVisible(expanded);
}

QPlainTextEdit* activityOutputWidget(const QString& text)
{
    auto* output = new QPlainTextEdit;
    output->setObjectName(QStringLiteral("conversationActivityOutput"));
    output->setReadOnly(true);
    output->setUndoRedoEnabled(false);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setMinimumHeight(96);
    output->setMaximumHeight(240);
    output->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    output->setStyleSheet(QStringLiteral("font-family:monospace;font-size:11px;"));
    output->setPlainText(text);
    output->setProperty("activityOutputText", text);
    return output;
}

void updateActivityOutput(QPlainTextEdit* output, const QString& text)
{
    if (!output)
        return;
    const bool followsEnd = output->verticalScrollBar()->maximum()
                            - output->verticalScrollBar()->value() <= 2;
    const int previousScroll = output->verticalScrollBar()->value();
    const QString previous = output->property("activityOutputText").toString();
    if (previous != text)
    {
        if (!previous.isEmpty() && text.startsWith(previous))
        {
            QTextCursor cursor = output->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(text.mid(previous.size()));
        }
        else
        {
            output->setPlainText(text);
        }
    }
    output->setProperty("activityOutputText", text);
    output->setVisible(!text.isEmpty());
    output->verticalScrollBar()->setValue(
        followsEnd ? output->verticalScrollBar()->maximum()
                   : qMin(previousScroll, output->verticalScrollBar()->maximum()));
}

QPlainTextEdit* ensureActivityOutput(QWidget* details, QVBoxLayout* layout)
{
    if (!details || !layout)
        return nullptr;
    auto* output = details->findChild<QPlainTextEdit*>(QStringLiteral("conversationActivityOutput"));
    const QString text = details->property("activityOutputText").toString();
    if (!output && !text.isEmpty())
    {
        auto* heading = details->findChild<QLabel*>(
            QStringLiteral("conversationActivityOutputHeading"));
        if (!heading)
        {
            heading = textLabel(QStringLiteral("Output"), "small");
            heading->setObjectName(QStringLiteral("conversationActivityOutputHeading"));
            heading->setStyleSheet(QStringLiteral("font-size:10px;font-weight:600;color:#475467;"));
            const auto* incomplete = details->findChild<QLabel*>(
                QStringLiteral("conversationActivityIncomplete"));
            const int headingPosition = incomplete ? layout->indexOf(incomplete) : layout->count();
            layout->insertWidget(headingPosition, heading);
        }
        output = activityOutputWidget(text);
        const auto* incomplete = details->findChild<QLabel*>(QStringLiteral("conversationActivityIncomplete"));
        const int position = incomplete ? layout->indexOf(incomplete) : layout->count();
        layout->insertWidget(position, output);
    }
    return output;
}

void addActivityRow(QVBoxLayout* rows,
                    const QString& itemId,
                    const ActivityPresentation& item,
                    bool expanded,
                    const std::function<void()>& layoutChanged = {})
{
    auto* line = new QWidget;
    line->setObjectName(QStringLiteral("conversationActivityRow"));
    line->setProperty("itemId", itemId);
    line->setMinimumHeight(38);
    auto* lineLayout = new QVBoxLayout(line);
    lineLayout->setContentsMargins(2, 5, 4, 5);
    lineLayout->setSpacing(5);

    auto* summary = new QWidget;
    auto* layout = new QHBoxLayout(summary);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    const bool hasDetails = !item.detail.isEmpty() || !item.output.isEmpty() || item.truncated;
    auto* disclosure = disclosureButton(
        expanded,
        QStringLiteral("Activity details: %1").arg(item.title));
    disclosure->setEnabled(hasDetails);
    disclosure->setVisible(hasDetails);
    layout->addWidget(disclosure, 0, Qt::AlignTop);

    const QString color = statusColor(item.status);
    auto* symbol = textLabel(statusGlyph(item.status));
    symbol->setObjectName(QStringLiteral("conversationActivitySymbol"));
    symbol->setFixedWidth(14);
    symbol->setAlignment(Qt::AlignTop);
    symbol->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(color));
    layout->addWidget(symbol);

    auto* title = wrappingLabel(item.title);
    title->setObjectName(QStringLiteral("conversationActivityTitle"));
    title->setToolTip(plainTooltip(item.title));
    title->setStyleSheet(QStringLiteral("font-size:12px;font-weight:500;"));
    layout->addWidget(title, 1);

    auto* tail = textLabel(item.tail, "meta");
    tail->setObjectName(QStringLiteral("conversationActivityTail"));
    tail->setVisible(!item.tail.isEmpty());
    layout->addWidget(tail, 0, Qt::AlignTop);
    auto* state = textLabel(item.status);
    state->setObjectName(QStringLiteral("conversationActivityStatus"));
    state->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(color));
    layout->addWidget(state, 0, Qt::AlignTop);
    lineLayout->addWidget(summary);

    auto* details = new QWidget;
    details->setObjectName(QStringLiteral("conversationActivityDetails"));
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(42, 0, 4, 4);
    detailsLayout->setSpacing(6);
    if (!item.detail.isEmpty())
    {
        auto* detail = wrappingLabel(item.detail, "meta");
        detail->setObjectName(QStringLiteral("conversationActivityDetail"));
        detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailsLayout->addWidget(detail);
    }
    if (!item.output.isEmpty())
    {
        details->setProperty("activityOutputText", item.output);
        if (expanded)
        {
            auto* heading = textLabel(QStringLiteral("Output"), "small");
            heading->setObjectName(QStringLiteral("conversationActivityOutputHeading"));
            heading->setStyleSheet(QStringLiteral("font-size:10px;font-weight:600;color:#475467;"));
            detailsLayout->addWidget(heading);
            detailsLayout->addWidget(activityOutputWidget(item.output));
        }
    }
    if (item.truncated)
    {
        auto* omitted = textLabel(QStringLiteral("Canonical activity detail is incomplete"), "small");
        omitted->setObjectName(QStringLiteral("conversationActivityIncomplete"));
        omitted->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
        detailsLayout->addWidget(omitted);
    }
    setDisclosureState(disclosure, details, expanded && hasDetails);
    lineLayout->addWidget(details);
    QObject::connect(disclosure, &QToolButton::clicked, line,
                     [disclosure, details, detailsLayout, layoutChanged](bool)
                     {
                         const bool next = !details->isVisible();
                         if (next)
                             ensureActivityOutput(details, detailsLayout);
                         setDisclosureState(disclosure, details, next);
                         if (layoutChanged)
                             layoutChanged();
                     });
    rows->addWidget(line);
}

bool updateActivityRow(QWidget* row, const ActivityPresentation& item)
{
    if (!row)
        return false;
    auto* title = dynamic_cast<WrappingLabel*>(
        row->findChild<QLabel*>(QStringLiteral("conversationActivityTitle")));
    auto* symbol = row->findChild<QLabel*>(QStringLiteral("conversationActivitySymbol"));
    auto* tail = row->findChild<QLabel*>(QStringLiteral("conversationActivityTail"));
    auto* status = row->findChild<QLabel*>(QStringLiteral("conversationActivityStatus"));
    auto* details = row->findChild<QWidget*>(QStringLiteral("conversationActivityDetails"));
    auto* disclosure = row->findChild<QToolButton*>(QStringLiteral("activityDisclosure"));
    if (!title || !symbol || !tail || !status || !details || !disclosure)
        return false;
    auto* detailsLayout = qobject_cast<QVBoxLayout*>(details->layout());
    if (!detailsLayout)
        return false;

    title->setContent(item.title);
    title->setToolTip(plainTooltip(item.title));
    disclosure->setAccessibleName(QStringLiteral("Activity details: %1").arg(item.title));
    symbol->setText(statusGlyph(item.status));
    symbol->setStyleSheet(
        QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(statusColor(item.status)));
    tail->setText(item.tail);
    tail->setVisible(!item.tail.isEmpty());
    status->setText(item.status);
    status->setStyleSheet(
        QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(statusColor(item.status)));

    auto* detail = dynamic_cast<WrappingLabel*>(
        details->findChild<QLabel*>(QStringLiteral("conversationActivityDetail")));
    if (!detail && !item.detail.isEmpty())
    {
        detail = static_cast<WrappingLabel*>(wrappingLabel({}, "meta"));
        detail->setObjectName(QStringLiteral("conversationActivityDetail"));
        detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailsLayout->insertWidget(0, detail);
    }
    if (detail)
    {
        detail->setContent(item.detail);
        detail->setVisible(!item.detail.isEmpty());
    }

    details->setProperty("activityOutputText", item.output);
    auto* output = details->findChild<QPlainTextEdit*>(QStringLiteral("conversationActivityOutput"));
    if (!output && !details->isHidden() && !item.output.isEmpty())
        output = ensureActivityOutput(details, detailsLayout);
    if (output)
        updateActivityOutput(output, item.output);
    if (auto* outputHeading = details->findChild<QLabel*>(
            QStringLiteral("conversationActivityOutputHeading")))
        outputHeading->setVisible(!item.output.isEmpty());

    auto* incomplete = details->findChild<QLabel*>(QStringLiteral("conversationActivityIncomplete"));
    if (!incomplete && item.truncated)
    {
        incomplete = textLabel(QStringLiteral("Canonical activity detail is incomplete"), "small");
        incomplete->setObjectName(QStringLiteral("conversationActivityIncomplete"));
        incomplete->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
        detailsLayout->addWidget(incomplete);
    }
    if (incomplete)
        incomplete->setVisible(item.truncated);

    const bool hasDetails = !item.detail.isEmpty() || !item.output.isEmpty() || item.truncated;
    disclosure->setEnabled(hasDetails);
    disclosure->setVisible(hasDetails);
    if (!hasDetails)
        setDisclosureState(disclosure, details, false);

    // A wrapping label can become shorter during an in-place canonical update.
    // Invalidate the nested layouts synchronously so the fixed timeline host does
    // not retain their previous height until a platform-specific layout event.
    detailsLayout->invalidate();
    detailsLayout->activate();
    details->updateGeometry();
    if (QLayout* rowLayout = row->layout())
    {
        rowLayout->invalidate();
        rowLayout->activate();
    }
    row->updateGeometry();
    return true;
}

QFrame* activityCard(const sdk::State& state,
                     const std::vector<const sdk::ItemState*>& items,
                     bool typedPlanAvailable,
                     bool expanded,
                     const QSet<QString>& expandedItems,
                     const std::function<void()>& layoutChanged)
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("conversationActivityCard"));
    card->setProperty("kind", "panel");
    card->setStyleSheet(QStringLiteral(
        "QFrame#conversationActivityCard{background:#ffffff;border:1px solid #d7dee8;border-radius:10px;}"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(0);

    auto* header = new QHBoxLayout;
    auto* disclosure = disclosureButton(expanded, QStringLiteral("Activity group"));
    header->addWidget(disclosure);
    auto* title = textLabel(QStringLiteral("Activity"));
    title->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;"));
    header->addWidget(title);
    header->addStretch();
    const bool legacyPlanAvailable = std::ranges::any_of(items, [](const sdk::ItemState* item) {
        return item && item->kind.is(frontend::ThreadItemKind::Plan);
    });
    auto* planAvailable = textLabel(QStringLiteral("Plan available"), "small");
    planAvailable->setObjectName(QStringLiteral("conversationActivityPlanAvailable"));
    planAvailable->setVisible(typedPlanAvailable || legacyPlanAvailable);
    header->addWidget(planAvailable);
    header->addSpacing(8);
    auto* count = textLabel(
        QStringLiteral("%1 activit%2").arg(items.size()).arg(items.size() == 1 ? "y" : "ies"),
        "small");
    count->setObjectName(QStringLiteral("conversationActivityCount"));
    header->addWidget(count);
    layout->addLayout(header);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("conversationActivityBody"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 9, 0, 0);
    bodyLayout->setSpacing(3);
    bodyLayout->addWidget(divider());
    auto* rows = new QVBoxLayout;
    rows->setObjectName(QStringLiteral("conversationActivityRows"));
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(4);
    for (const auto* item : items)
    {
        const QString itemId = fromUtf8(item->id.value);
        addActivityRow(rows,
                       itemId,
                       activityPresentation(state, *item),
                       expandedItems.contains(itemId),
                       layoutChanged);
    }
    bodyLayout->addLayout(rows);
    setDisclosureState(disclosure, body, expanded);
    layout->addWidget(body);
    QObject::connect(disclosure, &QToolButton::clicked, card,
                     [disclosure, body, layoutChanged](bool)
                     {
                         const bool next = !body->isVisible();
                         setDisclosureState(disclosure, body, next);
                         if (layoutChanged)
                             layoutChanged();
                     });
    return card;
}

void addMessage(QVBoxLayout* timeline, const sdk::ItemState& item, bool user)
{
    const MessagePresentation presentation = messagePresentation(item, user);
    auto* header = new QHBoxLayout;
    header->addWidget(textLabel(user ? QStringLiteral("YOU") : QStringLiteral("CODEX"), "section"));
    header->addStretch();
    auto* status = textLabel({}, "small");
    status->setObjectName(QStringLiteral("conversationMessageStatus"));
    header->addWidget(status);
    timeline->addLayout(header);
    timeline->addSpacing(3);

    QWidget* container = nullptr;
    QVBoxLayout* layout = nullptr;
    if (user)
    {
        auto* card = new QFrame;
        card->setObjectName(QStringLiteral("conversationUserMessageCard"));
        card->setProperty("kind", "raised");
        card->setStyleSheet(QStringLiteral(
            "QFrame#conversationUserMessageCard{background:#f8fafc;"
            "border:1px solid #d7dee8;border-radius:10px;}"));
        layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 12, 16, 12);
        layout->setSpacing(5);
        container = card;
    }
    else
    {
        container = new QWidget;
        layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
    }
    auto* copy = messageContentWidget(presentation.content);
    copy->setObjectName(QStringLiteral("conversationMessageContent"));
    layout->addWidget(copy);
    auto* marker = textLabel({}, "small");
    marker->setObjectName(QStringLiteral("conversationMessageTruncation"));
    marker->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
    layout->addWidget(marker);
    applyMessagePresentation(status, copy, marker, presentation);
    timeline->addWidget(container);
    timeline->addSpacing(16);
}

QString tokenCounts(const QString& name, const std::optional<sdk::TokenCountsView>& counts)
{
    if (!counts) return {};
    QStringList values;
    if (counts->totalTokens) values.append(QStringLiteral("%1 total").arg(*counts->totalTokens));
    if (counts->inputTokens) values.append(QStringLiteral("%1 in").arg(*counts->inputTokens));
    if (counts->outputTokens) values.append(QStringLiteral("%1 out").arg(*counts->outputTokens));
    if (counts->cachedInputTokens) values.append(QStringLiteral("%1 cached").arg(*counts->cachedInputTokens));
    if (counts->reasoningOutputTokens)
        values.append(QStringLiteral("%1 reasoning").arg(*counts->reasoningOutputTokens));
    return values.isEmpty() ? QString{} : name + QStringLiteral(" ") + values.join(QStringLiteral(" / "));
}

QString tokenUsageText(const sdk::TurnState& turn)
{
    const auto usage = sdk::tokenUsageView(turn);
    if (!usage) return {};
    QStringList parts;
    const QString last = tokenCounts(QStringLiteral("last"), usage->last);
    const QString total = tokenCounts(QStringLiteral("total"), usage->total);
    if (!last.isEmpty()) parts.append(last);
    if (!total.isEmpty()) parts.append(total);
    if (usage->modelContextWindowPresent)
        parts.append(usage->modelContextWindow ? QStringLiteral("context %1").arg(*usage->modelContextWindow)
                                               : QStringLiteral("context unavailable"));
    if (usage->truncated) parts.append(QStringLiteral("usage details omitted"));
    return parts.join(QStringLiteral(" · "));
}

QString failureText(const sdk::TurnState& turn)
{
    const auto failure = sdk::failureView(turn);
    if (!failure) return {};
    QStringList details;
    if (failure->message) details.append(fromUtf8(*failure->message));
    if (failure->additionalDetails) details.append(fromUtf8(*failure->additionalDetails));
    if (failure->codexErrorCategory)
        details.append(humanize(fromUtf8(*failure->codexErrorCategory)));
    else if (failure->unknownErrorDiscriminator)
        details.append(QStringLiteral("Backend error: %1").arg(fromUtf8(*failure->unknownErrorDiscriminator)));
    if (failure->httpStatusCode) details.append(QStringLiteral("HTTP %1").arg(*failure->httpStatusCode));
    if (failure->nonSteerableTurnKind)
        details.append(QStringLiteral("turn kind %1").arg(fromUtf8(*failure->nonSteerableTurnKind)));
    if (failure->redacted) details.append(QStringLiteral("sensitive detail redacted"));
    if (failure->decodingOmitted) details.append(QStringLiteral("additional detail omitted"));
    return details.isEmpty() ? QStringLiteral("Turn failed")
                             : QStringLiteral("Turn failed · ") + details.join(QStringLiteral(" · "));
}

void addPresentationValue(QCryptographicHash& hash, const QByteArray& value)
{
    hash.addData(QByteArray::number(value.size()));
    hash.addData(QByteArrayLiteral(":"));
    hash.addData(value);
}

void addPresentationValue(QCryptographicHash& hash, const QString& value)
{
    addPresentationValue(hash, value.toUtf8());
}

void addPresentationValue(QCryptographicHash& hash, std::string_view value)
{
    hash.addData(QByteArray::number(value.size()));
    hash.addData(QByteArrayLiteral(":"));
    hash.addData(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
}

void addPresentationValue(QCryptographicHash& hash, bool value)
{
    addPresentationValue(hash, value ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
}

void addEmptyState(QVBoxLayout* timeline, const QString& title, const QString& detail)
{
    auto* empty = new QFrame;
    empty->setObjectName(QStringLiteral("conversationEmptyState"));
    empty->setProperty("kind", "panel");
    empty->setStyleSheet(QStringLiteral(
        "QFrame#conversationEmptyState{background:#ffffff;border:1px solid #d7dee8;border-radius:10px;}"));
    auto* layout = new QVBoxLayout(empty);
    layout->setContentsMargins(20, 24, 20, 24);
    layout->setSpacing(5);
    auto* heading = textLabel(title);
    heading->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;"));
    layout->addWidget(heading);
    auto* copy = wrappingLabel(detail, "meta");
    layout->addWidget(copy);
    timeline->addWidget(empty);
    timeline->addSpacing(16);
}

struct TimelineSegment
{
    QString id;
    std::vector<const sdk::ItemState*> items;
    bool missing = false;
};

struct ActivityExpansionState
{
    bool groupExpanded = true;
    QSet<QString> expandedItems;
};

ActivityExpansionState activityExpansionState(const QWidget* segment)
{
    ActivityExpansionState result;
    if (!segment)
        return result;
    if (const auto* body = segment->findChild<QWidget*>(QStringLiteral("conversationActivityBody")))
        result.groupExpanded = !body->isHidden();
    for (const auto* row : segment->findChildren<QWidget*>(QStringLiteral("conversationActivityRow")))
    {
        const auto* details = row->findChild<QWidget*>(QStringLiteral("conversationActivityDetails"));
        if (details && !details->isHidden())
            result.expandedItems.insert(row->property("itemId").toString());
    }
    return result;
}

struct TimelineEntry
{
    const sdk::TurnState* turn = nullptr;
    qsizetype turnNumber = 0;
    TimelineSegment segment;
};

struct TimelineTurnSlice
{
    const sdk::TurnState* turn = nullptr;
    qsizetype turnNumber = 0;
    qsizetype firstItem = 0;
};

struct TimelineWindow
{
    std::vector<TimelineTurnSlice> turns;
    qsizetype renderedItems = 0;
    qsizetype totalItems = 0;
};

QString segmentStorageKey(const QString& turnId, const QString& segmentId)
{
    return turnId + QChar(0x1f) + segmentId;
}

std::vector<TimelineSegment> timelineSegments(const sdk::State& state,
                                              const sdk::ThreadState& thread,
                                              const sdk::TurnState& turn,
                                              qsizetype firstItem)
{
    std::vector<TimelineSegment> result;
    if (turn.orderedItems.empty())
    {
        result.push_back({QStringLiteral("empty"), {}, false});
        return result;
    }

    std::vector<const sdk::ItemState*> activities;
    QString activityId;
    const auto flushActivities = [&]
    {
        if (!activities.empty())
            result.push_back({QStringLiteral("activities:") + activityId, activities, false});
        activities.clear();
        activityId.clear();
    };

    const qsizetype activityWidth = static_cast<qsizetype>(maximumActivityItemsPerSegment);
    // Stable ordinal buckets keep activity-card identities from shifting on
    // every append while inspecting at most one partial bucket before the window.
    const qsizetype scanStart = firstItem - firstItem % activityWidth;
    qsizetype activityBucket = -1;
    for (qsizetype index = scanStart; index < static_cast<qsizetype>(turn.orderedItems.size()); ++index)
    {
        const auto& itemId = turn.orderedItems.at(index);
        const qsizetype itemBucket = index / activityWidth;
        if (itemBucket != activityBucket)
        {
            flushActivities();
            activityBucket = itemBucket;
        }
        const auto* item = state.item(thread.id, turn.id, itemId);
        if (!item)
        {
            flushActivities();
            if (index >= firstItem)
                result.push_back({QStringLiteral("missing:") + fromUtf8(itemId.value), {}, true});
            continue;
        }
        const bool message = item->kind.is(frontend::ThreadItemKind::UserMessage)
                             || item->kind.is(frontend::ThreadItemKind::AgentMessage);
        if (message)
        {
            flushActivities();
            if (index >= firstItem)
                result.push_back({QStringLiteral("message:") + fromUtf8(item->id.value), {item}, false});
        }
        else
        {
            if (activityId.isEmpty())
                activityId = fromUtf8(item->id.value);
            if (index >= firstItem)
                activities.push_back(item);
        }
    }
    flushActivities();
    return result;
}

qsizetype timelineItemCount(const TimelineSegment& segment)
{
    return qMax<qsizetype>(1, static_cast<qsizetype>(segment.items.size()));
}

TimelineWindow latestTimelineWindow(const sdk::State& state, const sdk::ThreadState& thread)
{
    TimelineWindow result;
    // Count from ordered IDs only; item lookup and presentation stay bounded
    // to the selected tail below.
    for (const auto& turnId : thread.orderedTurns)
    {
        const auto* turn = state.turn(thread.id, turnId);
        if (turn)
            result.totalItems += qMax<qsizetype>(1, static_cast<qsizetype>(turn->orderedItems.size()));
    }

    qsizetype remainingItems = maximumRenderedTimelineItems;
    for (qsizetype index = static_cast<qsizetype>(thread.orderedTurns.size());
         index > 0 && remainingItems > 0
         && static_cast<qsizetype>(result.turns.size()) < maximumRenderedTimelineTurns;
         --index)
    {
        const auto* turn = state.turn(thread.id, thread.orderedTurns.at(index - 1));
        if (!turn)
            continue;
        const qsizetype itemCount = qMax<qsizetype>(1, static_cast<qsizetype>(turn->orderedItems.size()));
        const qsizetype selectedItems = qMin(itemCount, remainingItems);
        const qsizetype firstItem = turn->orderedItems.empty()
                                         ? 0
                                         : static_cast<qsizetype>(turn->orderedItems.size()) - selectedItems;
        result.turns.insert(result.turns.begin(), {turn, index, firstItem});
        result.renderedItems += selectedItems;
        remainingItems -= selectedItems;
    }
    return result;
}

QByteArray segmentPresentationKey(const sdk::State& state,
                                  const TimelineSegment& segment,
                                  bool typedPlanAvailable)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addPresentationValue(hash, segment.id);
    addPresentationValue(hash, segment.missing);
    addPresentationValue(hash, typedPlanAvailable);
    for (const auto* item : segment.items)
    {
        addPresentationValue(hash, item != nullptr);
        if (!item)
            continue;
        addPresentationValue(hash, item->id.value);
        addPresentationValue(hash, item->kind.identity);
        addPresentationValue(hash, itemStatus(*item));
        addPresentationValue(hash, truncationText(*item));
        if (item->kind.is(frontend::ThreadItemKind::UserMessage))
        {
            const auto message = sdk::userMessageSemanticView(*item);
            addPresentationValue(hash, message.has_value());
            if (message)
            {
                addPresentationValue(hash, message->text);
                addPresentationValue(hash, userMessageTruncationText(*message));
            }
        }
        else if (item->kind.is(frontend::ThreadItemKind::AgentMessage))
        {
            const QString content = item->agentText && !item->agentText->empty()
                                        ? fromUtf8(*item->agentText)
                                        : (item->summary ? fromUtf8(*item->summary) : QString{});
            addPresentationValue(hash, content);
            const auto semantic = sdk::itemSemanticView(*item);
            const auto* agent = semantic ? std::get_if<sdk::AgentMessageSemanticView>(&semantic->details) : nullptr;
            addPresentationValue(hash,
                                 agent && agent->phase ? std::string_view(*agent->phase) : std::string_view{});
        }
        else
        {
            // A command-output channel may be several MiB. Do not convert and
            // hash its complete text merely to discover that an immutable item
            // revision changed; exact content updates explicitly bypass an
            // equal key during reconciliation below.
            const ActivityPresentation presentation = activityPresentation(state, *item, false);
            addPresentationValue(hash, presentation.title);
            addPresentationValue(hash, presentation.detail);
            addPresentationValue(
                hash,
                QByteArray::number(static_cast<qulonglong>(
                    item->commandOutput ? item->commandOutput->size() : 0)));
            if (item->stamp)
                addPresentationValue(
                    hash,
                    QByteArray::number(static_cast<qulonglong>(item->stamp->generation)));
            else if (item->commandOutput)
                // Compatibility states without a source stamp cannot provide
                // a cheaper authoritative revision identity.
                addPresentationValue(hash, std::string_view(*item->commandOutput));
            addPresentationValue(hash, presentation.status);
            addPresentationValue(hash, presentation.tail);
            addPresentationValue(hash, presentation.truncated);
        }
    }
    return hash.result();
}

QWidget* timelineSegmentWidget(const sdk::State& state,
                               const TimelineSegment& segment,
                               bool typedPlanAvailable,
                               const ActivityExpansionState& activityExpansion,
                               const std::function<void()>& layoutChanged)
{
    auto* host = new QWidget;
    host->setObjectName(QStringLiteral("conversationSegment"));
    host->setProperty("segmentId", segment.id);
    host->setProperty("timelineItemCount", timelineItemCount(segment));
    host->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    if (segment.missing)
    {
        ActivityPresentation omitted{
            QStringLiteral("Unavailable item"),
            QStringLiteral("The ordered item shell is not retained in current State"),
            {},
            QStringLiteral("Omitted"),
            {},
            true};
        auto* card = new QFrame;
        card->setProperty("kind", "panel");
        auto* rows = new QVBoxLayout(card);
        rows->setContentsMargins(16, 8, 16, 8);
        addActivityRow(rows, QString{}, omitted, false, layoutChanged);
        layout->addWidget(card);
        layout->addSpacing(16);
    }
    else if (segment.items.empty())
    {
        addEmptyState(layout, QStringLiteral("No items in this turn"),
                      QStringLiteral("The synchronized turn currently has no retained items."));
    }
    else if (segment.items.size() == 1
             && (segment.items.front()->kind.is(frontend::ThreadItemKind::UserMessage)
                 || segment.items.front()->kind.is(frontend::ThreadItemKind::AgentMessage)))
    {
        const auto* item = segment.items.front();
        const bool user = item->kind.is(frontend::ThreadItemKind::UserMessage);
        host->setProperty("messageUser", user);
        addMessage(layout, *item, user);
    }
    else
    {
        layout->addWidget(activityCard(state,
                                       segment.items,
                                       typedPlanAvailable,
                                       activityExpansion.groupExpanded,
                                       activityExpansion.expandedItems,
                                       layoutChanged));
        layout->addSpacing(16);
    }
    return host;
}

bool updateTimelineActivitySegment(QWidget* host,
                                   const sdk::State& state,
                                   const TimelineSegment& segment,
                                   bool typedPlanAvailable,
                                   const QStringList* exactChangedItemIds,
                                   const std::function<void()>& layoutChanged)
{
    if (!host || segment.missing || segment.items.empty())
        return false;
    if (segment.items.size() == 1
        && (segment.items.front()->kind.is(frontend::ThreadItemKind::UserMessage)
            || segment.items.front()->kind.is(frontend::ThreadItemKind::AgentMessage)))
        return false;

    auto* rowsLayout = host->findChild<QVBoxLayout*>(QStringLiteral("conversationActivityRows"));
    auto* count = host->findChild<QLabel*>(QStringLiteral("conversationActivityCount"));
    auto* planAvailable = host->findChild<QLabel*>(QStringLiteral("conversationActivityPlanAvailable"));
    if (!rowsLayout || !count || !planAvailable
        || static_cast<std::size_t>(rowsLayout->count()) > segment.items.size())
        return false;
    std::vector<QWidget*> rows;
    rows.reserve(static_cast<std::size_t>(rowsLayout->count()));
    for (int index = 0; index < rowsLayout->count(); ++index)
    {
        QWidget* row = rowsLayout->itemAt(index)->widget();
        if (!row || row->objectName() != QStringLiteral("conversationActivityRow"))
            return false;
        rows.push_back(row);
    }
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto* item = segment.items.at(index);
        if (!item || rows.at(index)->property("itemId").toString() != fromUtf8(item->id.value))
            return false;
    }
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto* item = segment.items.at(index);
        if (exactChangedItemIds
            && !exactChangedItemIds->contains(fromUtf8(item->id.value)))
            continue;
        if (!updateActivityRow(rows.at(index), activityPresentation(state, *item)))
            return false;
    }
    for (std::size_t index = rows.size(); index < segment.items.size(); ++index)
    {
        const auto* item = segment.items.at(index);
        if (!item)
            return false;
        addActivityRow(rowsLayout,
                       fromUtf8(item->id.value),
                       activityPresentation(state, *item),
                       false,
                       layoutChanged);
    }
    count->setText(QStringLiteral("%1 activit%2")
                       .arg(segment.items.size())
                       .arg(segment.items.size() == 1 ? "y" : "ies"));
    planAvailable->setVisible(typedPlanAvailable
                              || std::ranges::any_of(segment.items, [](const sdk::ItemState* item) {
                                     return item && item->kind.is(frontend::ThreadItemKind::Plan);
                                 }));
    rowsLayout->invalidate();
    rowsLayout->activate();
    if (QLayout* hostLayout = host->layout())
    {
        hostLayout->invalidate();
        hostLayout->activate();
    }
    host->updateGeometry();
    return true;
}

bool updateTimelineMessageSegment(QWidget* host,
                                  const TimelineSegment& segment,
                                  bool* mayShrink = nullptr)
{
    if (!host || segment.missing || segment.items.size() != 1)
        return false;
    const auto* item = segment.items.front();
    if (!item)
        return false;
    const bool user = item->kind.is(frontend::ThreadItemKind::UserMessage);
    if (!user && !item->kind.is(frontend::ThreadItemKind::AgentMessage))
        return false;
    if (!host->property("messageUser").isValid()
        || host->property("messageUser").toBool() != user)
        return false;

    auto* status = host->findChild<QLabel*>(QStringLiteral("conversationMessageStatus"));
    auto* contentWidget = host->findChild<QWidget*>(QStringLiteral("conversationMessageContent"));
    auto* truncation = host->findChild<QLabel*>(QStringLiteral("conversationMessageTruncation"));
    if (!status || !contentWidget || !truncation)
        return false;

    const QString previousContent = messageContentText(contentWidget);
    const QString previousTruncation = truncation->text();
    const bool previousTruncationVisible = truncation->isVisible();
    const QString previousKind = contentWidget->property("kind").toString();
    const MessagePresentation presentation = messagePresentation(*item, user);
    if (mayShrink)
    {
        const QString nextKind = presentation.missing ? QStringLiteral("meta")
                                                      : QStringLiteral("body");
        *mayShrink = !presentation.content.startsWith(previousContent)
                     || previousKind != nextKind
                     || (previousTruncationVisible
                         && !presentation.truncation.startsWith(previousTruncation));
    }
    auto* contentLayout = qobject_cast<QVBoxLayout*>(contentWidget->parentWidget()->layout());
    if (!contentLayout)
        return false;
    contentWidget = ensureMessageContentWidget(contentLayout, contentWidget, presentation.content);
    applyMessagePresentation(status,
                             contentWidget,
                             truncation,
                             presentation);
    return true;
}

QWidget* timelineTurnWidget(const sdk::TurnState& turn,
                            qsizetype visibleTurn,
                            QVBoxLayout*& itemLayout,
                            QLabel*& turnLabel,
                            QLabel*& statusLabel,
                            std::function<void()> detailsRequested)
{
    auto* host = new QWidget;
    host->setObjectName(QStringLiteral("conversationTurn"));
    host->setProperty("turnId", fromUtf8(turn.id.value));
    host->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (visibleTurn > 1)
    {
        layout->addSpacing(7);
        layout->addWidget(divider());
        layout->addSpacing(12);
    }

    auto* turnHeader = new QHBoxLayout;
    turnLabel = textLabel(QStringLiteral("TURN %1").arg(visibleTurn), "section");
    turnLabel->setToolTip(fromUtf8(turn.id.value));
    turnHeader->addWidget(turnLabel);
    turnHeader->addStretch();
    auto* details = new QPushButton(QStringLiteral("Details"));
    details->setObjectName(QStringLiteral("conversationTurnDetails"));
    details->setProperty("kind", "subtle");
    details->setFixedSize(58, 24);
    QObject::connect(details, &QPushButton::clicked, details,
                     [detailsRequested = std::move(detailsRequested)] { detailsRequested(); });
    turnHeader->addWidget(details);
    const QString status = humanize(fromUtf8(turn.status.value));
    statusLabel = textLabel(status, "small");
    statusLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(statusColor(status)));
    turnHeader->addWidget(statusLabel);
    layout->addLayout(turnHeader);
    layout->addSpacing(10);

    auto* itemHost = new QWidget;
    itemHost->setStyleSheet(QStringLiteral("background:transparent;"));
    itemLayout = new QVBoxLayout(itemHost);
    itemLayout->setContentsMargins(0, 0, 0, 0);
    itemLayout->setSpacing(0);
    layout->addWidget(itemHost);
    return host;
}

QByteArray turnSummaryPresentationKey(const sdk::TurnState* turn,
                                      qsizetype index)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addPresentationValue(hash, turn != nullptr);
    if (!turn)
        return hash.result();
    addPresentationValue(hash, turn->id.value);
    addPresentationValue(hash, QByteArray::number(index));
    addPresentationValue(hash, turn->status.value);
    addPresentationValue(hash, failureText(*turn));
    return hash.result();
}

} // namespace

ConversationWidget::ConversationWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("conversation"));
    setStyleSheet(QStringLiteral("QWidget#conversation{background:#f6f8fb;}"));
    setMinimumWidth(480);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 14, 24, 0);
    root->setSpacing(0);

    auto* context = new QHBoxLayout;
    context->setSpacing(10);
    context->addWidget(badge(QStringLiteral("THREAD"), QStringLiteral("#e5eeff"), QStringLiteral("#2f6feb"), 54, 18));
    contextPath = textLabel(QStringLiteral("No thread selected"), "small");
    contextPath->setStyleSheet(QStringLiteral("color:#667085;font-size:9px;font-weight:500;"));
    context->addWidget(contextPath);
    contextPath->hide();
    context->addStretch();
    root->addLayout(context);
    root->addSpacing(2);
    threadTitle = textLabel(QStringLiteral("No synchronized thread"), "heading");
    root->addWidget(threadTitle);
    root->addSpacing(2);
    threadDetail = textLabel(QStringLiteral("Select a synchronized thread to view its conversation"), "meta");
    root->addWidget(threadDetail);
    root->addSpacing(7);
    root->addWidget(divider());
    root->addSpacing(7);

    turnFailure = textLabel({}, "meta");
    turnFailure->setWordWrap(true);
    turnFailure->setStyleSheet(QStringLiteral(
        "background:#fff1f1;color:#b83a3a;border:1px solid #efc4c4;border-radius:6px;"
        "padding:7px 10px;font-size:10px;"));
    turnFailure->hide();
    root->addSpacing(3);
    root->addWidget(turnFailure);
    root->addSpacing(3);

    scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* conversationScroll = scrollArea->verticalScrollBar();
    scrollAnimation = new QPropertyAnimation(conversationScroll, "value", this);
    scrollAnimation->setEasingCurve(QEasingCurve::OutCubic);
    layoutSettleTimer = new QTimer(this);
    layoutSettleTimer->setSingleShot(true);
    layoutSettleTimer->setInterval(16);
    connect(layoutSettleTimer, &QTimer::timeout, this, &ConversationWidget::settleTimelineLayout);
    connect(scrollAnimation, &QPropertyAnimation::finished, this,
            [this] { followingLatest = false; });
    connect(conversationScroll, &QScrollBar::rangeChanged, this,
            [this, conversationScroll](int, int maximum)
            {
                if (pinLatestDuringLayout)
                    conversationScroll->setValue(maximum);
            });
    connect(conversationScroll, &QScrollBar::actionTriggered, this,
            [this, conversationScroll](int)
            {
                scrollAnimation->stop();
                followingLatest = false;
                pendingFollowLatest = false;
                pendingPreviousScroll = conversationScroll->value();
                pendingViewportAnchor.clear();
            });
    connect(conversationScroll, &QScrollBar::sliderPressed, this,
            [this, conversationScroll]
            {
                scrollAnimation->stop();
                followingLatest = false;
                pendingFollowLatest = false;
                pendingPreviousScroll = conversationScroll->value();
                pendingViewportAnchor.clear();
            });
    auto* content = new QWidget;
    content->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* conversation = new QVBoxLayout(content);
    conversation->setContentsMargins(0, 0, 0, 16);
    conversation->setSpacing(0);
    conversation->setAlignment(Qt::AlignTop);

    timelineWindowNotice = new QFrame;
    timelineWindowNotice->setObjectName(QStringLiteral("conversationWindowNotice"));
    timelineWindowNotice->setStyleSheet(
        QStringLiteral("QFrame#conversationWindowNotice{background:#f8fafc;border:1px solid #d7dee8;border-radius:7px;}"));
    auto* timelineWindowLayout = new QVBoxLayout(timelineWindowNotice);
    timelineWindowLayout->setContentsMargins(12, 8, 12, 8);
    timelineWindowDetail = wrappingLabel({}, "meta");
    timelineWindowLayout->addWidget(timelineWindowDetail);
    timelineWindowNotice->hide();
    conversation->addWidget(timelineWindowNotice);

    timelineHost = new QWidget;
    timelineHost->setObjectName(QStringLiteral("conversationTimeline"));
    timelineHost->setProperty("maximumRenderedTurns", maximumRenderedTimelineTurns);
    timelineHost->setProperty("maximumRenderedItems", maximumRenderedTimelineItems);
    timeline = new QVBoxLayout(timelineHost);
    timeline->setContentsMargins(0, 0, 0, 0);
    timeline->setSpacing(0);
    timeline->setAlignment(Qt::AlignTop);
    conversation->addWidget(timelineHost);

    scrollArea->setWidget(content);

    anchoredSurface = new AnchoredTurnSurface;
    upcomingTurnDock = new UpcomingTurnDock;
    anchoredSurface->setConversationWidget(scrollArea);
    anchoredSurface->setUpcomingTurnDock(upcomingTurnDock);
    root->addWidget(anchoredSurface, 1);

    connect(upcomingTurnDock, &UpcomingTurnDock::sendRequested,
            this, &ConversationWidget::sendRequested);
    connect(upcomingTurnDock, &UpcomingTurnDock::stopRequested,
            this, &ConversationWidget::stopRequested);
    connect(upcomingTurnDock, &UpcomingTurnDock::settingsChanged,
            this, &ConversationWidget::upcomingTurnSettingsChanged);

    addEmptyState(timeline, QStringLiteral("No thread selected"),
                  QStringLiteral("Choose a synchronized thread from the sidebar."));
}

void ConversationWidget::setModelCatalog(
    const std::vector<ai::openai::codex::typed::Model>& catalog)
{
    upcomingTurnDock->setModelCatalog(catalog);
}

void ConversationWidget::render(const sdk::State& state,
                                const QString& threadId,
                                bool newThreadDraft,
                                const QHash<QString, QStringList>* exactContentChanges)
{
    auto* scrollBar = scrollArea->verticalScrollBar();
    const int previousScroll = scrollBar->value();
    const bool wasNearBottom = scrollBar->maximum() - previousScroll <= 72;
    const bool threadChanged = renderedThreadId != threadId || renderedNewThreadDraft != newThreadDraft;
    const auto* thread = threadId.isEmpty() ? nullptr : state.thread(threadId.toStdString());
    upcomingTurnDock->setCanonicalConfiguration(
        thread ? thread->executionConfiguration
               : std::optional<sdk::ExecutionConfiguration>{},
        thread ? threadId : QString{},
        newThreadDraft);
    if (!thread && !threadChanged && threadId.isEmpty())
        return;
    const bool followLatest = threadChanged || wasNearBottom || followingLatest;
    const bool exactContentOnly = exactContentChanges && !threadChanged && thread && !newThreadDraft
                                  && !renderedSummaryKey.isEmpty();
    const std::uint64_t generation = ++renderGeneration;
    bool timelineShrank = false;
    if (threadChanged)
        pendingViewportAnchor.clear();
    else if (!layoutSettleTimer->isActive())
        captureTimelineAnchor();
    renderedThreadId = threadId;
    renderedNewThreadDraft = newThreadDraft;
    if (threadChanged)
    {
        followingLatest = false;
        pinLatestDuringLayout = true;
        pinLatestGeneration = generation;
        scrollArea->viewport()->setUpdatesEnabled(false);
    }
    else
        scrollAnimation->stop();

    const auto clearTimelineState = [this, &timelineShrank]
    {
        timelineShrank = timelineShrank || timeline->count() > 0;
        renderedTurnIds.clear();
        renderedTurnWidgets.clear();
        renderedTurnLabels.clear();
        renderedTurnStatusLabels.clear();
        renderedTurnItemLayouts.clear();
        renderedSegmentIds.clear();
        renderedSegmentKeys.clear();
        renderedSegmentWidgets.clear();
        clearLayout(timeline);
    };

    if (!thread)
    {
        if (timeline->count() > 0)
            clearTimelineState();
        if (!renderedSummaryKey.isEmpty() || turnFailure->isVisible())
        {
            renderedSummaryKey.clear();
            turnFailure->hide();
        }
        contextPath->setText(newThreadDraft ? QStringLiteral("New thread draft")
                                            : QStringLiteral("No thread selected"));
        contextPath->setToolTip({});
        threadTitle->setText(newThreadDraft ? QStringLiteral("New conversation")
                                            : QStringLiteral("No synchronized thread"));
        threadTitle->setToolTip({});
        threadDetail->setText(newThreadDraft
                                  ? QStringLiteral("A real thread will be created when the first prompt is sent")
                                  : QStringLiteral("Select a synchronized thread to view its conversation"));
        threadDetail->setToolTip({});
        timelineWindowNotice->hide();
        timelineHost->setProperty("renderedTimelineItems", 0);
        timelineHost->setProperty("retainedTimelineItems", 0);
        addEmptyState(timeline,
                      newThreadDraft ? QStringLiteral("Start a new conversation")
                                     : QStringLiteral("No thread selected"),
                      newThreadDraft ? QStringLiteral("Type a prompt below. Backend defaults will be used for the new thread.")
                                     : QStringLiteral("Choose a synchronized thread from the sidebar."));
    }
    else
    {
        const QString id = fromUtf8(thread->id.value);
        const QString title = thread->title && !thread->title->empty() ? fromUtf8(*thread->title) : id;
        threadTitle->setText(title);
        threadTitle->setToolTip(title);
        QStringList metadata;
        if (thread->archived.value_or(false))
            metadata.append(QStringLiteral("Archived"));
        else if (thread->status && !thread->status->empty())
            metadata.append(humanize(fromUtf8(*thread->status)));
        if (thread->ephemeral.value_or(false))
            metadata.append(QStringLiteral("Temporary"));
        metadata.append(QStringLiteral("%1 turn%2")
                            .arg(thread->orderedTurns.size())
                            .arg(thread->orderedTurns.size() == 1 ? QString{} : QStringLiteral("s")));
        if (!thread->fullyLoaded)
            metadata.append(QStringLiteral("Loading conversation…"));
        threadDetail->setText(metadata.join(QStringLiteral(" · ")));
        threadDetail->setToolTip(threadDetail->text());

        const sdk::TurnState* currentTurn = nullptr;
        qsizetype currentIndex = -1;
        for (qsizetype index = 0; index < static_cast<qsizetype>(thread->orderedTurns.size()); ++index)
        {
            if (const auto* turn = state.turn(thread->id, thread->orderedTurns.at(index)))
            {
                currentTurn = turn;
                currentIndex = index;
            }
        }

        const QByteArray summaryKey = exactContentOnly
                                          ? renderedSummaryKey
                                          : turnSummaryPresentationKey(currentTurn, currentIndex);
        if (!exactContentOnly && summaryKey != renderedSummaryKey)
        {
            renderedSummaryKey = summaryKey;
            turnFailure->hide();
            if (currentTurn)
            {
                const QString failure = failureText(*currentTurn);
                if (!failure.isEmpty())
                {
                    turnFailure->setText(failure);
                    turnFailure->setToolTip(failure);
                    turnFailure->show();
                }
            }
        }

        if (!currentTurn)
        {
            timelineWindowNotice->hide();
            timelineHost->setProperty("renderedTimelineItems", 0);
            timelineHost->setProperty("retainedTimelineItems", 0);
            if (threadChanged || !renderedTurnIds.isEmpty() || timeline->count() == 0)
            {
                clearTimelineState();
                addEmptyState(timeline, QStringLiteral("Ready for the first turn"),
                              thread->fullyLoaded
                                  ? QStringLiteral("Use the upcoming-turn dock below to start this thread.")
                                  : QStringLiteral("No turn projection is currently retained for this thread."));
            }
        }
        else
        {
            const TimelineWindow window = latestTimelineWindow(state, *thread);
            std::vector<TimelineEntry> entries;
            entries.reserve(static_cast<std::size_t>(window.renderedItems));
            for (const TimelineTurnSlice& slice : window.turns)
            {
                auto segments = timelineSegments(state, *thread, *slice.turn, slice.firstItem);
                for (TimelineSegment& segment : segments)
                    entries.push_back({slice.turn, slice.turnNumber, std::move(segment)});
            }
            timelineHost->setProperty("renderedTimelineItems", window.renderedItems);
            timelineHost->setProperty("retainedTimelineItems", window.totalItems);
            if (window.renderedItems < window.totalItems)
            {
                timelineWindowDetail->setText(
                    QStringLiteral("Showing the latest %1 of %2 synchronized timeline entries. "
                                   "Earlier entries remain in canonical AISuite State and are not "
                                   "materialized in this live view.")
                        .arg(window.renderedItems)
                        .arg(window.totalItems));
                timelineWindowNotice->show();
            }
            else
            {
                timelineWindowNotice->hide();
            }

            struct VisibleTimelineTurn
            {
                const sdk::TurnState* turn = nullptr;
                qsizetype turnNumber = 0;
                std::vector<const TimelineSegment*> segments;
            };
            std::vector<VisibleTimelineTurn> visibleTurns;
            for (const TimelineEntry& entry : entries)
            {
                if (visibleTurns.empty() || visibleTurns.back().turn != entry.turn)
                    visibleTurns.push_back({entry.turn, entry.turnNumber, {}});
                visibleTurns.back().segments.push_back(&entry.segment);
            }
            QStringList visibleTurnIds;
            visibleTurnIds.reserve(static_cast<qsizetype>(visibleTurns.size()));
            for (const VisibleTimelineTurn& visibleTurn : visibleTurns)
                visibleTurnIds.append(fromUtf8(visibleTurn.turn->id.value));

            const auto removeRenderedTurn = [this, &timelineShrank](const QString& turnId)
            {
                for (const QString& segmentId : renderedSegmentIds.take(turnId))
                {
                    const QString storage = segmentStorageKey(turnId, segmentId);
                    renderedSegmentKeys.remove(storage);
                    renderedSegmentWidgets.remove(storage);
                }
                renderedTurnLabels.remove(turnId);
                renderedTurnStatusLabels.remove(turnId);
                renderedTurnItemLayouts.remove(turnId);
                if (QWidget* widget = renderedTurnWidgets.take(turnId))
                {
                    if (pendingViewportAnchor == widget
                        || (pendingViewportAnchor && widget->isAncestorOf(pendingViewportAnchor)))
                        pendingViewportAnchor.clear();
                    timeline->removeWidget(widget);
                    widget->hide();
                    widget->deleteLater();
                    timelineShrank = true;
                }
            };

            bool compatibleTurns = !threadChanged && timeline->count() == renderedTurnIds.size();
            qsizetype removedTurnPrefix = 0;
            if (compatibleTurns && !renderedTurnIds.isEmpty())
            {
                if (visibleTurnIds.isEmpty())
                {
                    compatibleTurns = false;
                }
                else
                {
                    removedTurnPrefix = renderedTurnIds.indexOf(visibleTurnIds.front());
                    compatibleTurns = removedTurnPrefix >= 0
                                      && renderedTurnIds.size() - removedTurnPrefix <= visibleTurnIds.size();
                    for (qsizetype index = 0;
                         compatibleTurns && index < renderedTurnIds.size() - removedTurnPrefix;
                         ++index)
                    {
                        const QString oldId = renderedTurnIds.at(removedTurnPrefix + index);
                        compatibleTurns = oldId == visibleTurnIds.at(index)
                                          && renderedTurnWidgets.contains(oldId)
                                          && renderedTurnLabels.contains(oldId)
                                          && renderedTurnItemLayouts.contains(oldId)
                                          && renderedTurnStatusLabels.contains(oldId);
                    }
                }
            }
            if (!compatibleTurns)
            {
                clearTimelineState();
            }
            else
            {
                for (qsizetype index = 0; index < removedTurnPrefix; ++index)
                {
                    const QString removed = renderedTurnIds.front();
                    renderedTurnIds.removeFirst();
                    removeRenderedTurn(removed);
                }
            }

            for (const VisibleTimelineTurn& visibleTurn : visibleTurns)
            {
                const auto* turn = visibleTurn.turn;
                const QString turnId = fromUtf8(turn->id.value);
                QVBoxLayout* itemLayout = renderedTurnItemLayouts.value(turnId);
                QLabel* turnLabel = renderedTurnLabels.value(turnId);
                QLabel* statusLabel = renderedTurnStatusLabels.value(turnId);
                if (!renderedTurnWidgets.contains(turnId))
                {
                    auto* turnWidget = timelineTurnWidget(
                        *turn,
                        visibleTurn.turnNumber,
                        itemLayout,
                        turnLabel,
                        statusLabel,
                        [this, turnId] { emit turnDetailsRequested(turnId); });
                    timeline->addWidget(turnWidget, 0, Qt::AlignTop);
                    renderedTurnWidgets.insert(turnId, turnWidget);
                    renderedTurnLabels.insert(turnId, turnLabel);
                    renderedTurnItemLayouts.insert(turnId, itemLayout);
                    renderedTurnStatusLabels.insert(turnId, statusLabel);
                }
                else
                {
                    const QString heading = QStringLiteral("TURN %1").arg(visibleTurn.turnNumber);
                    if (turnLabel && turnLabel->text() != heading)
                        turnLabel->setText(heading);
                    const QString status = humanize(fromUtf8(turn->status.value));
                    if (statusLabel && statusLabel->text() != status)
                    {
                        statusLabel->setText(status);
                        statusLabel->setStyleSheet(
                            QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(statusColor(status)));
                    }
                }

                QStringList segmentIds;
                segmentIds.reserve(static_cast<qsizetype>(visibleTurn.segments.size()));
                for (const TimelineSegment* segment : visibleTurn.segments)
                    segmentIds.append(segment->id);

                const QStringList oldSegmentIds = renderedSegmentIds.value(turnId);
                bool compatibleSegments = true;
                qsizetype removedSegmentPrefix = 0;
                if (!oldSegmentIds.isEmpty())
                {
                    if (segmentIds.isEmpty())
                    {
                        compatibleSegments = false;
                    }
                    else
                    {
                        removedSegmentPrefix = oldSegmentIds.indexOf(segmentIds.front());
                        compatibleSegments = removedSegmentPrefix >= 0
                                             && oldSegmentIds.size() - removedSegmentPrefix <= segmentIds.size();
                        for (qsizetype segmentIndex = 0;
                             compatibleSegments && segmentIndex < oldSegmentIds.size() - removedSegmentPrefix;
                             ++segmentIndex)
                        {
                            const QString oldId = oldSegmentIds.at(removedSegmentPrefix + segmentIndex);
                            const QString storage = segmentStorageKey(turnId, oldId);
                            compatibleSegments = oldId == segmentIds.at(segmentIndex)
                                                 && renderedSegmentWidgets.contains(storage);
                        }
                    }
                }
                if (!compatibleSegments)
                {
                    if (QWidget* turnWidget = renderedTurnWidgets.value(turnId);
                        pendingViewportAnchor && turnWidget
                        && (pendingViewportAnchor == turnWidget
                            || turnWidget->isAncestorOf(pendingViewportAnchor)))
                        pendingViewportAnchor.clear();
                    clearLayout(itemLayout);
                    timelineShrank = timelineShrank || !oldSegmentIds.isEmpty();
                    for (const QString& oldId : oldSegmentIds)
                    {
                        const QString storage = segmentStorageKey(turnId, oldId);
                        renderedSegmentKeys.remove(storage);
                        renderedSegmentWidgets.remove(storage);
                    }
                }
                else
                {
                    if (removedSegmentPrefix > 0 && pendingViewportAnchor)
                    {
                        bool anchorRemoved = false;
                        for (qsizetype segmentIndex = 0; segmentIndex < removedSegmentPrefix; ++segmentIndex)
                        {
                            QWidget* removed = renderedSegmentWidgets.value(
                                segmentStorageKey(turnId, oldSegmentIds.at(segmentIndex)));
                            anchorRemoved = anchorRemoved || pendingViewportAnchor == removed
                                            || (removed && removed->isAncestorOf(pendingViewportAnchor));
                        }
                        if (anchorRemoved && removedSegmentPrefix < oldSegmentIds.size())
                        {
                            QWidget* survivor = renderedSegmentWidgets.value(
                                segmentStorageKey(turnId, oldSegmentIds.at(removedSegmentPrefix)));
                            pendingViewportAnchor = survivor;
                            if (survivor)
                            {
                                pendingViewportAnchorY = scrollArea->viewport()
                                                             ->mapFromGlobal(survivor->mapToGlobal(QPoint{}))
                                                             .y();
                            }
                        }
                    }
                    for (qsizetype segmentIndex = 0; segmentIndex < removedSegmentPrefix; ++segmentIndex)
                    {
                        const QString storage = segmentStorageKey(turnId, oldSegmentIds.at(segmentIndex));
                        if (QWidget* widget = renderedSegmentWidgets.take(storage))
                        {
                            itemLayout->removeWidget(widget);
                            widget->hide();
                            widget->deleteLater();
                            timelineShrank = true;
                        }
                        renderedSegmentKeys.remove(storage);
                    }
                }

                for (const TimelineSegment* segment : visibleTurn.segments)
                {
                    const QString storage = segmentStorageKey(turnId, segment->id);
                    QWidget* oldWidget = renderedSegmentWidgets.value(storage);
                    const QStringList* exactChangedItemIds = nullptr;
                    bool explicitlyAffected = false;
                    if (oldWidget && exactContentOnly)
                    {
                        const auto changedItems = exactContentChanges->constFind(turnId);
                        explicitlyAffected = changedItems != exactContentChanges->cend()
                                             && std::any_of(
                                                 segment->items.cbegin(),
                                                 segment->items.cend(),
                                                 [&changedItems](const sdk::ItemState* item)
                                                 {
                                                     return item
                                                            && changedItems->contains(
                                                                fromUtf8(item->id.value));
                                                 });
                        if (!explicitlyAffected)
                            continue;
                        exactChangedItemIds = &changedItems.value();
                    }
                    const bool typedPlanAvailable = turn->plan.has_value();
                    const QByteArray segmentKey = segmentPresentationKey(
                        state, *segment, typedPlanAvailable);
                    if (oldWidget && !explicitlyAffected
                        && renderedSegmentKeys.value(storage) == segmentKey)
                        continue;

                    bool messageMayShrink = false;
                    if (oldWidget && updateTimelineMessageSegment(oldWidget, *segment, &messageMayShrink))
                    {
                        renderedSegmentKeys.insert(storage, segmentKey);
                        timelineShrank = timelineShrank || messageMayShrink;
                        continue;
                    }

                    if (oldWidget
                        && updateTimelineActivitySegment(
                            oldWidget,
                            state,
                            *segment,
                            typedPlanAvailable,
                            exactChangedItemIds,
                            [this] { activityLayoutChanged(); }))
                    {
                        renderedSegmentKeys.insert(storage, segmentKey);
                        timelineShrank = true;
                        continue;
                    }

                    const ActivityExpansionState expansion = activityExpansionState(oldWidget);
                    QWidget* newWidget = timelineSegmentWidget(
                        state,
                        *segment,
                        typedPlanAvailable,
                        expansion,
                        [this] { activityLayoutChanged(); });
                    newWidget->setProperty("turnId", turnId);
                    if (oldWidget)
                    {
                        const bool replacesAnchor = pendingViewportAnchor == oldWidget
                                                    || (pendingViewportAnchor
                                                        && oldWidget->isAncestorOf(pendingViewportAnchor));
                        const int position = itemLayout->indexOf(oldWidget);
                        itemLayout->removeWidget(oldWidget);
                        oldWidget->hide();
                        oldWidget->deleteLater();
                        itemLayout->insertWidget(position, newWidget, 0, Qt::AlignTop);
                        if (replacesAnchor)
                            pendingViewportAnchor = newWidget;
                        timelineShrank = true;
                    }
                    else
                    {
                        itemLayout->addWidget(newWidget, 0, Qt::AlignTop);
                    }
                    renderedSegmentWidgets.insert(storage, newWidget);
                    renderedSegmentKeys.insert(storage, segmentKey);
                }
                renderedSegmentIds.insert(turnId, segmentIds);
            }
            renderedTurnIds = visibleTurnIds;
        }
    }

    scheduleTimelineLayout(previousScroll, followLatest, threadChanged, timelineShrank);
}

bool ConversationWidget::updateExactMessageContent(
    const sdk::State& state,
    const QString& threadId,
    const QHash<QString, QStringList>& exactContentChanges)
{
    if (threadId.isEmpty() || renderedThreadId != threadId || renderedNewThreadDraft
        || exactContentChanges.isEmpty())
        return false;
    const auto* thread = state.thread(threadId.toStdString());
    if (!thread)
        return false;

    struct PendingMessageUpdate
    {
        QString storage;
        QWidget* widget = nullptr;
        TimelineSegment segment;
        QByteArray presentationKey;
    };
    std::vector<PendingMessageUpdate> updates;
    for (auto turnIterator = exactContentChanges.cbegin();
         turnIterator != exactContentChanges.cend();
         ++turnIterator)
    {
        const ai::openai::codex::typed::TurnId turnIdentity{turnIterator.key().toStdString()};
        const auto* turn = state.turn(thread->id, turnIdentity);
        if (!turn)
            return false;
        const bool turnVisible = renderedTurnIds.contains(turnIterator.key());
        for (const QString& itemId : turnIterator.value())
        {
            const ai::openai::codex::typed::ItemId itemIdentity{itemId.toStdString()};
            const auto* item = state.item(thread->id, turn->id, itemIdentity);
            if (!item)
                return false;
            const bool user = item->kind.is(frontend::ThreadItemKind::UserMessage);
            const bool agent = item->kind.is(frontend::ThreadItemKind::AgentMessage);
            if (!user && !agent)
            {
                if (turnVisible)
                    return false;
                continue;
            }
            if (!turnVisible)
                continue;

            const QString segmentId = QStringLiteral("message:") + itemId;
            if (!renderedSegmentIds.value(turnIterator.key()).contains(segmentId))
                continue;
            const QString storage = segmentStorageKey(turnIterator.key(), segmentId);
            QWidget* widget = renderedSegmentWidgets.value(storage);
            if (!widget || !widget->property("messageUser").isValid()
                || widget->property("messageUser").toBool() != user)
                return false;
            TimelineSegment segment{segmentId, {item}, false};
            updates.push_back(
                {storage,
                 widget,
                 segment,
                 segmentPresentationKey(state, segment, turn->plan.has_value())});
        }
    }

    if (updates.empty())
        return true;
    auto* scrollBar = scrollArea->verticalScrollBar();
    const int previousScroll = scrollBar->value();
    const bool followLatest = scrollBar->maximum() - previousScroll <= 72
                              || followingLatest;
    if (!layoutSettleTimer->isActive())
        captureTimelineAnchor();
    scrollAnimation->stop();

    bool timelineShrank = false;
    for (PendingMessageUpdate& update : updates)
    {
        bool messageMayShrink = false;
        if (!updateTimelineMessageSegment(update.widget, update.segment, &messageMayShrink))
            return false;
        renderedSegmentKeys.insert(update.storage, update.presentationKey);
        timelineShrank = timelineShrank || messageMayShrink;
    }
    scheduleTimelineLayout(previousScroll, followLatest, false, timelineShrank);
    return true;
}

void ConversationWidget::scheduleTimelineLayout(int previousScroll,
                                                bool followLatest,
                                                bool threadChanged,
                                                bool timelineShrank)
{
    if (!layoutSettleTimer->isActive())
    {
        pendingPreviousScroll = previousScroll;
        layoutSettleTimer->start();
    }
    pendingFollowLatest = pendingFollowLatest || followLatest;
    pendingThreadChanged = pendingThreadChanged || threadChanged;
    pendingTimelineShrink = pendingTimelineShrink || timelineShrank;
}

void ConversationWidget::activityLayoutChanged()
{
    auto* bar = scrollArea->verticalScrollBar();
    const int previousScroll = bar->value();
    const bool followLatest = bar->maximum() - previousScroll <= 72
                              || followingLatest;
    if (!layoutSettleTimer->isActive())
        captureTimelineAnchor();
    scrollAnimation->stop();
    scheduleTimelineLayout(previousScroll, followLatest, false, true);
}

void ConversationWidget::captureTimelineAnchor()
{
    pendingViewportAnchor.clear();
    auto* viewport = scrollArea->viewport();
    const auto captureIfVisible = [this, viewport](QWidget* candidate)
    {
        if (!candidate)
            return false;
        const int y = viewport->mapFromGlobal(candidate->mapToGlobal(QPoint{})).y();
        if (y >= viewport->height() || y + candidate->height() <= 0)
            return false;
        pendingViewportAnchor = candidate;
        pendingViewportAnchorY = y;
        return true;
    };
    for (const QString& turnId : renderedTurnIds)
    {
        for (const QString& segmentId : renderedSegmentIds.value(turnId))
        {
            if (captureIfVisible(renderedSegmentWidgets.value(segmentStorageKey(turnId, segmentId))))
                return;
        }
        QWidget* turn = renderedTurnWidgets.value(turnId);
        if (captureIfVisible(turn))
            return;
    }
}

void ConversationWidget::settleTimelineLayout()
{
    const int previousScroll = pendingPreviousScroll;
    const bool followLatest = pendingFollowLatest;
    const bool threadChanged = pendingThreadChanged;
    const bool timelineShrank = pendingTimelineShrink;
    pendingFollowLatest = false;
    pendingThreadChanged = false;
    pendingTimelineShrink = false;

    if (threadChanged || pinLatestDuringLayout)
    {
        scrollAnimation->stop();
        followingLatest = false;
        settleThreadSwitchLayout(pinLatestGeneration, 2);
        return;
    }

    synchronizeTimelineHeight(timelineShrank);
    scrollArea->widget()->layout()->activate();
    scrollArea->widget()->adjustSize();

    auto* bar = scrollArea->verticalScrollBar();
    bar->setValue(qMin(previousScroll, bar->maximum()));
    if (pendingViewportAnchor)
    {
        auto* viewport = scrollArea->viewport();
        const int settledY = viewport->mapFromGlobal(pendingViewportAnchor->mapToGlobal(QPoint{})).y();
        const int correction = settledY - pendingViewportAnchorY;
        bar->setValue(qBound(0, bar->value() + correction, bar->maximum()));
    }
    pendingViewportAnchor.clear();
    if (!scrollArea->viewport()->updatesEnabled())
    {
        scrollArea->viewport()->setUpdatesEnabled(true);
        scrollArea->viewport()->update();
    }
    if (!followLatest)
    {
        scrollAnimation->stop();
        followingLatest = false;
        return;
    }

    scrollAnimation->stop();
    const int distance = bar->maximum() - bar->value();
    if (distance <= 0 || renderedThreadId.isEmpty())
    {
        followingLatest = false;
        bar->setValue(bar->maximum());
        return;
    }
    followingLatest = true;
    scrollAnimation->setDuration(qBound(90, distance, 220));
    scrollAnimation->setStartValue(bar->value());
    scrollAnimation->setEndValue(bar->maximum());
    scrollAnimation->start();
}

void ConversationWidget::settleThreadSwitchLayout(std::uint64_t generation, int remainingPasses)
{
    if (generation != pinLatestGeneration || !pinLatestDuringLayout)
        return;
    synchronizeTimelineHeight(true);
    scrollArea->widget()->layout()->activate();
    scrollArea->widget()->adjustSize();
    if (remainingPasses > 0)
    {
        QTimer::singleShot(0, this,
                           [this, generation, remainingPasses]
                           {
                               settleThreadSwitchLayout(generation, remainingPasses - 1);
                           });
        return;
    }

    auto* bar = scrollArea->verticalScrollBar();
    bar->setValue(bar->maximum());
    QTimer::singleShot(100, this,
                       [this, generation]
                       {
                           if (generation != pinLatestGeneration || !pinLatestDuringLayout)
                               return;
                           synchronizeTimelineHeight(true);
                           scrollArea->widget()->layout()->activate();
                           scrollArea->widget()->adjustSize();
                           auto* settledBar = scrollArea->verticalScrollBar();
                           settledBar->setValue(settledBar->maximum());
                           scrollArea->viewport()->setUpdatesEnabled(true);
                           scrollArea->viewport()->update();
                           QTimer::singleShot(100, this,
                                              [this, generation]
                                              {
                                                  if (generation != pinLatestGeneration
                                                      || !pinLatestDuringLayout)
                                                      return;
                                                  synchronizeTimelineHeight(true);
                                                  scrollArea->widget()->layout()->activate();
                                                  scrollArea->widget()->adjustSize();
                                                  auto* finalBar = scrollArea->verticalScrollBar();
                                                  finalBar->setValue(finalBar->maximum());
                                                  pinLatestDuringLayout = false;
                                                  pendingViewportAnchor.clear();
                                              });
                       });
}

void ConversationWidget::synchronizeTimelineHeight(bool allowShrink)
{
    if (allowShrink)
    {
        timelineHost->setMinimumHeight(0);
        timelineHost->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    timeline->invalidate();
    timeline->activate();
    const int width = timelineHost->contentsRect().width();
    const int preferredHeight = width > 0 && timeline->hasHeightForWidth()
                                    ? timeline->heightForWidth(width)
                                    : timeline->sizeHint().height();
    const int target = qMax(0, qMax(timeline->minimumSize().height(), preferredHeight));
    if (allowShrink || target > timelineHost->height())
        timelineHost->setFixedHeight(target);
}

void ConversationWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (resizeLayoutPending)
        return;
    resizeLayoutPending = true;
    QTimer::singleShot(0, this,
                       [this]
                       {
                           synchronizeTimelineHeight();
                           scrollArea->widget()->layout()->activate();
                           scrollArea->widget()->adjustSize();
                           QTimer::singleShot(0, this,
                                              [this]
                                              {
                                                  synchronizeTimelineHeight();
                                                  scrollArea->widget()->layout()->activate();
                                                  scrollArea->widget()->adjustSize();
                                                  resizeLayoutPending = false;
                                              });
                       });
}

void ConversationWidget::clearPrompt()
{
    upcomingTurnDock->clearPrompt();
}

void ConversationWidget::clearPromptIfUnchanged(const QString& submittedPrompt)
{
    upcomingTurnDock->clearPromptIfUnchanged(submittedPrompt);
}

void ConversationWidget::focusComposer()
{
    upcomingTurnDock->focusPrompt();
}

UpcomingTurnDraft ConversationWidget::upcomingTurnDraft() const
{
    return upcomingTurnDock->draft();
}

void ConversationWidget::clearUpcomingTurnSettings()
{
    upcomingTurnDock->clearTouchedSettings();
}

void ConversationWidget::acknowledgeSubmittedSettings(const UpcomingTurnDraft& submitted)
{
    upcomingTurnDock->acknowledgeSubmittedSettings(submitted);
}

void ConversationWidget::setActionState(bool primaryAllowed,
                                        bool stopAllowed,
                                        bool editorAllowed,
                                        bool settingsAllowed,
                                        bool stopVisible,
                                        bool steerMode,
                                        const QString& actionThreadIdentity,
                                        const QString& activeTurnIdentity)
{
    upcomingTurnDock->setActionState(
        primaryAllowed,
        stopAllowed,
        editorAllowed,
        settingsAllowed,
        stopVisible,
        steerMode,
        actionThreadIdentity,
        activeTurnIdentity);
}

void ConversationWidget::setWriteStatus(const QString& text, bool error)
{
    upcomingTurnDock->setStatus(text, error);
}

} // namespace codexui
