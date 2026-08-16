// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ConversationWidget.h"

#include <ai/openai/codex/frontend/Messages.h>
#include <ai/openai/codex/frontend/client/State.h>

#include <QCryptographicHash>
#include <QEvent>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QEasingCurve>

#include <optional>
#include <variant>
#include <vector>

namespace codexui
{
namespace
{
namespace sdk = ai::openai::codex::frontend::client;
namespace frontend = ai::openai::codex::frontend;

struct ActivityPresentation
{
    QString title;
    QString detail;
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
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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

QFrame* divider()
{
    auto* line = new QFrame;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:#2b3038;"));
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
        return QStringLiteral("#ed6a6a");
    if (normalized.contains(QStringLiteral("complete")) || normalized.contains(QStringLiteral("success")) ||
        normalized == QStringLiteral("done"))
        return QStringLiteral("#40c27d");
    if (normalized.contains(QStringLiteral("progress")) || normalized.contains(QStringLiteral("running")) ||
        normalized.contains(QStringLiteral("active")) || normalized.contains(QStringLiteral("stream")))
        return QStringLiteral("#4f94f5");
    if (normalized.contains(QStringLiteral("interrupt")) || normalized.contains(QStringLiteral("cancel")))
        return QStringLiteral("#f5a83b");
    return QStringLiteral("#949ead");
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
    QStringList details;
    if (message.textTruncated) details.append(QStringLiteral("Retained text is truncated"));
    if (message.contentTruncated) details.append(QStringLiteral("Some original user content is not shown"));
    return details.join(QStringLiteral(" · "));
}

QString pendingRequestDetail(const sdk::State& state, const sdk::ItemState& item)
{
    for (const auto& request : state.pendingRequests())
    {
        if (!request.itemId || *request.itemId != item.id) continue;
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

ActivityPresentation activityPresentation(const sdk::State& state, const sdk::ItemState& item)
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
            if (command->command) result.title = compact(fromUtf8(*command->command), 240);
            if (command->cwd) result.detail = fromUtf8(command->cwd->value);
            if (command->status) result.status = humanize(fromUtf8(*command->status));
            QStringList tail;
            if (command->durationMs) tail.append(QStringLiteral("%1 ms").arg(*command->durationMs));
            if (command->exitCode) tail.append(QStringLiteral("exit %1").arg(*command->exitCode));
            result.tail = tail.join(QStringLiteral(" · "));
            if (item.commandOutput && !item.commandOutput->empty())
            {
                const QString output = QStringLiteral("Output: %1").arg(fromUtf8(*item.commandOutput));
                result.detail = result.detail.isEmpty() ? output : result.detail + QStringLiteral(" · ") + output;
            }
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

void addActivityRow(QVBoxLayout* rows, const ActivityPresentation& item)
{
    auto* line = new QWidget;
    line->setMinimumHeight(38);
    auto* layout = new QHBoxLayout(line);
    layout->setContentsMargins(2, 5, 4, 5);
    layout->setSpacing(8);

    const QString color = statusColor(item.status);
    auto* symbol = textLabel(statusGlyph(item.status));
    symbol->setFixedWidth(14);
    symbol->setAlignment(Qt::AlignTop);
    symbol->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(color));
    layout->addWidget(symbol);

    auto* copy = new QWidget;
    auto* copyLayout = new QVBoxLayout(copy);
    copyLayout->setContentsMargins(0, 0, 0, 0);
    copyLayout->setSpacing(2);
    auto* title = wrappingLabel(item.title);
    title->setStyleSheet(QStringLiteral("font-size:12px;font-weight:500;"));
    copyLayout->addWidget(title);
    if (!item.detail.isEmpty())
    {
        auto* detail = wrappingLabel(compact(item.detail), "meta");
        detail->setToolTip(item.detail);
        copyLayout->addWidget(detail);
    }
    if (item.truncated)
    {
        auto* omitted = textLabel(QStringLiteral("Projected detail is truncated or omitted"), "small");
        omitted->setStyleSheet(QStringLiteral("color:#f5a83b;font-size:9px;"));
        copyLayout->addWidget(omitted);
    }
    layout->addWidget(copy, 1);

    if (!item.tail.isEmpty()) layout->addWidget(textLabel(item.tail, "meta"), 0, Qt::AlignTop);
    auto* state = textLabel(item.status);
    state->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(color));
    layout->addWidget(state, 0, Qt::AlignTop);
    rows->addWidget(line);
}

QFrame* activityCard(const sdk::State& state, const std::vector<const sdk::ItemState*>& items)
{
    auto* card = new QFrame;
    card->setProperty("kind", "panel");
    card->setStyleSheet(QStringLiteral("QFrame{background:#13161a;border-radius:10px;}"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(0);

    auto* header = new QHBoxLayout;
    auto* title = textLabel(QStringLiteral("Activity"));
    title->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;"));
    header->addWidget(title);
    header->addStretch();
    qsizetype plans = 0;
    for (const auto* item : items)
        plans += item->kind.is(frontend::ThreadItemKind::Plan) ? 1 : 0;
    if (plans) header->addWidget(textLabel(QStringLiteral("Plan available"), "small"));
    header->addSpacing(8);
    header->addWidget(
        textLabel(QStringLiteral("%1 activit%2").arg(items.size()).arg(items.size() == 1 ? "y" : "ies"), "small"));
    layout->addLayout(header);
    layout->addSpacing(9);
    layout->addWidget(divider());
    layout->addSpacing(3);
    for (const auto* item : items)
        addActivityRow(layout, activityPresentation(state, *item));
    return card;
}

void addMessage(QVBoxLayout* timeline, const sdk::ItemState& item, bool user)
{
    auto* header = new QHBoxLayout;
    header->addWidget(textLabel(user ? QStringLiteral("YOU") : QStringLiteral("CODEX"), "section"));
    header->addStretch();
    QString statusText = itemStatus(item);
    if (!user)
    {
        const auto semantic = sdk::itemSemanticView(item);
        const auto* agent = semantic ? std::get_if<sdk::AgentMessageSemanticView>(&semantic->details) : nullptr;
        if (agent && agent->phase) statusText += QStringLiteral(" · ") + humanize(fromUtf8(*agent->phase));
    }
    auto* status = textLabel(statusText, "small");
    status->setStyleSheet(QStringLiteral("color:%1;font-size:9px;").arg(statusColor(itemStatus(item))));
    header->addWidget(status);
    timeline->addLayout(header);
    timeline->addSpacing(3);

    const auto userMessage = user ? sdk::userMessageSemanticView(item) : std::nullopt;
    QString content;
    bool missing = false;
    if (user)
    {
        if (!userMessage)
        {
            content = QStringLiteral("User message is unavailable");
            missing = true;
        }
        else if (userMessage->text.empty())
        {
            content = QStringLiteral("User message contains no retained text");
            missing = true;
        }
        else
        {
            content = fromUtf8(userMessage->text);
        }
    }
    else
    {
        content = item.agentText && !item.agentText->empty()
                      ? fromUtf8(*item.agentText)
                      : (item.summary ? fromUtf8(*item.summary) : QString{});
        if (content.isEmpty())
        {
            content = QStringLiteral("No retained message content");
            missing = true;
        }
    }

    QWidget* container = nullptr;
    QVBoxLayout* layout = nullptr;
    if (user)
    {
        auto* card = new QFrame;
        card->setProperty("kind", "raised");
        card->setStyleSheet(QStringLiteral("background:#181c21;border-radius:10px;"));
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
    auto* copy = wrappingLabel(content, missing ? "meta" : "body");
    copy->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(copy);
    QString truncation = userMessage ? userMessageTruncationText(*userMessage) : QString{};
    if (truncation.isEmpty()) truncation = truncationText(item);
    if (!truncation.isEmpty())
    {
        auto* marker = textLabel(truncation, "small");
        marker->setStyleSheet(QStringLiteral("color:#f5a83b;font-size:9px;"));
        layout->addWidget(marker);
    }
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
    addPresentationValue(hash, QByteArray(value.data(), static_cast<qsizetype>(value.size())));
}

void addPresentationValue(QCryptographicHash& hash, bool value)
{
    addPresentationValue(hash, value ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
}

QByteArray conversationPresentationKey(const sdk::State& state, const QString& threadId, bool newThreadDraft)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addPresentationValue(hash, threadId);
    addPresentationValue(hash, newThreadDraft);

    const auto* thread = threadId.isEmpty() ? nullptr : state.thread(threadId.toStdString());
    addPresentationValue(hash, thread != nullptr);
    if (!thread)
        return hash.result();

    addPresentationValue(hash, thread->id.value);
    addPresentationValue(hash, thread->title ? std::string_view(*thread->title) : std::string_view{});
    addPresentationValue(hash, thread->status ? std::string_view(*thread->status) : std::string_view{});
    addPresentationValue(hash, thread->model ? std::string_view(thread->model->value) : std::string_view{});
    addPresentationValue(hash, thread->cwd ? std::string_view(thread->cwd->value) : std::string_view{});
    addPresentationValue(hash, thread->fullyLoaded);
    addPresentationValue(hash, thread->realtime.has_value());
    if (thread->realtime)
    {
        const auto realtime = sdk::realtimeSemanticView(*thread->realtime);
        addPresentationValue(hash, realtime.lifecycle);
        addPresentationValue(hash, QByteArray::number(realtime.itemCount));
        addPresentationValue(hash, realtime.transcriptTruncated);
    }

    for (const auto& turnId : thread->orderedTurns)
    {
        addPresentationValue(hash, turnId.value);
        const auto* turn = state.turn(turnId);
        addPresentationValue(hash, turn != nullptr);
        if (!turn)
            continue;

        addPresentationValue(hash, turn->status.value);
        addPresentationValue(hash, tokenUsageText(*turn));
        addPresentationValue(hash, failureText(*turn));
        for (const auto& itemId : turn->orderedItems)
        {
            const auto* item = state.item(thread->id, turn->id, itemId);
            addPresentationValue(hash, item != nullptr);
            if (!item)
                continue;

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
                const auto* agent = semantic
                                        ? std::get_if<sdk::AgentMessageSemanticView>(&semantic->details)
                                        : nullptr;
                addPresentationValue(hash,
                                     agent && agent->phase ? std::string_view(*agent->phase) : std::string_view{});
            }
            else
            {
                const ActivityPresentation presentation = activityPresentation(state, *item);
                addPresentationValue(hash, presentation.title);
                addPresentationValue(hash, presentation.detail);
                addPresentationValue(hash, presentation.status);
                addPresentationValue(hash, presentation.tail);
                addPresentationValue(hash, presentation.truncated);
            }
        }
    }
    return hash.result();
}

void addEmptyState(QVBoxLayout* timeline, const QString& title, const QString& detail)
{
    auto* empty = new QFrame;
    empty->setProperty("kind", "panel");
    empty->setStyleSheet(QStringLiteral("background:#13161a;border-radius:10px;"));
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

QString segmentStorageKey(const QString& turnId, const QString& segmentId)
{
    return turnId + QChar(0x1f) + segmentId;
}

std::vector<TimelineSegment> timelineSegments(const sdk::State& state,
                                              const sdk::ThreadState& thread,
                                              const sdk::TurnState& turn)
{
    std::vector<TimelineSegment> result;
    std::vector<const sdk::ItemState*> activities;
    const auto flushActivities = [&]
    {
        if (activities.empty())
            return;
        result.push_back({QStringLiteral("activities:") + fromUtf8(activities.front()->id.value), activities, false});
        activities.clear();
    };

    for (const auto& itemId : turn.orderedItems)
    {
        const auto* item = state.item(thread.id, turn.id, itemId);
        if (!item)
        {
            flushActivities();
            result.push_back({QStringLiteral("missing:") + fromUtf8(itemId.value), {}, true});
            continue;
        }
        const bool message = item->kind.is(frontend::ThreadItemKind::UserMessage)
                             || item->kind.is(frontend::ThreadItemKind::AgentMessage);
        if (message)
        {
            flushActivities();
            result.push_back({QStringLiteral("message:") + fromUtf8(item->id.value), {item}, false});
        }
        else
        {
            activities.push_back(item);
        }
    }
    flushActivities();
    if (result.empty())
        result.push_back({QStringLiteral("empty"), {}, false});
    return result;
}

QByteArray segmentPresentationKey(const sdk::State& state, const TimelineSegment& segment)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addPresentationValue(hash, segment.id);
    addPresentationValue(hash, segment.missing);
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
            const ActivityPresentation presentation = activityPresentation(state, *item);
            addPresentationValue(hash, presentation.title);
            addPresentationValue(hash, presentation.detail);
            addPresentationValue(hash, presentation.status);
            addPresentationValue(hash, presentation.tail);
            addPresentationValue(hash, presentation.truncated);
        }
    }
    return hash.result();
}

QWidget* timelineSegmentWidget(const sdk::State& state, const TimelineSegment& segment)
{
    auto* host = new QWidget;
    host->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    if (segment.missing)
    {
        ActivityPresentation omitted{
            QStringLiteral("Unavailable item"),
            QStringLiteral("The ordered item shell is not retained in current State"),
            QStringLiteral("Omitted"),
            {},
            true};
        auto* card = new QFrame;
        card->setProperty("kind", "panel");
        auto* rows = new QVBoxLayout(card);
        rows->setContentsMargins(16, 8, 16, 8);
        addActivityRow(rows, omitted);
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
        addMessage(layout, *item, item->kind.is(frontend::ThreadItemKind::UserMessage));
    }
    else
    {
        layout->addWidget(activityCard(state, segment.items));
        layout->addSpacing(16);
    }
    return host;
}

QWidget* timelineTurnWidget(const sdk::TurnState& turn,
                            qsizetype visibleTurn,
                            QVBoxLayout*& itemLayout,
                            QLabel*& statusLabel)
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
    auto* turnLabel = textLabel(
        QStringLiteral("TURN %1 · %2").arg(visibleTurn).arg(compactId(turn.id.value)), "section");
    turnLabel->setToolTip(fromUtf8(turn.id.value));
    turnHeader->addWidget(turnLabel);
    turnHeader->addStretch();
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

QByteArray turnSummaryPresentationKey(const sdk::State& state,
                                      const sdk::ThreadState& thread,
                                      const sdk::TurnState* turn,
                                      qsizetype index)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addPresentationValue(hash, turn != nullptr);
    if (!turn)
        return hash.result();
    addPresentationValue(hash, turn->id.value);
    addPresentationValue(hash, QByteArray::number(index));
    addPresentationValue(hash, turn->status.value);
    addPresentationValue(hash, tokenUsageText(*turn));
    addPresentationValue(hash, failureText(*turn));
    for (const auto& itemId : turn->orderedItems)
    {
        const auto* item = state.item(thread.id, turn->id, itemId);
        addPresentationValue(hash, item ? item->kind.identity : std::string_view{});
    }
    return hash.result();
}

} // namespace

ConversationWidget::ConversationWidget(QWidget* parent) : QWidget(parent)
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
    contextPath = textLabel(QStringLiteral("No thread selected"), "small");
    contextPath->setStyleSheet(QStringLiteral("color:#949ead;font-size:9px;font-weight:500;"));
    context->addWidget(contextPath);
    context->addStretch();
    root->addLayout(context);
    root->addSpacing(2);
    threadTitle = textLabel(QStringLiteral("No synchronized thread"), "heading");
    root->addWidget(threadTitle);
    threadDetail = textLabel(QStringLiteral("Select a synchronized thread to view its conversation"), "meta");
    root->addWidget(threadDetail);
    root->addSpacing(7);
    root->addWidget(divider());
    root->addSpacing(7);

    turnSummary = new QFrame;
    turnSummary->setProperty("kind", "summary");
    turnSummaryLayout = new QVBoxLayout(turnSummary);
    turnSummaryLayout->setContentsMargins(14, 7, 14, 7);
    turnSummaryLayout->setSpacing(3);
    turnSummary->hide();
    root->addWidget(turnSummary);
    turnFailure = textLabel({}, "meta");
    turnFailure->setWordWrap(true);
    turnFailure->setStyleSheet(QStringLiteral("background:#32191b;color:#ffb3b3;border-radius:6px;"
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
    connect(scrollAnimation, &QPropertyAnimation::finished, this,
            [this] { followingLatest = false; });
    connect(conversationScroll, &QScrollBar::rangeChanged, this,
            [this, conversationScroll](int, int maximum)
            {
                if (pinLatestDuringLayout)
                    conversationScroll->setValue(maximum);
            });
    connect(conversationScroll, &QScrollBar::actionTriggered, this,
            [this](int)
            {
                scrollAnimation->stop();
                followingLatest = false;
            });
    connect(conversationScroll, &QScrollBar::sliderPressed, this,
            [this]
            {
                scrollAnimation->stop();
                followingLatest = false;
            });
    auto* content = new QWidget;
    content->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* conversation = new QVBoxLayout(content);
    conversation->setContentsMargins(0, 0, 0, 16);
    conversation->setSpacing(0);
    conversation->setAlignment(Qt::AlignTop);

    timelineHost = new QWidget;
    timeline = new QVBoxLayout(timelineHost);
    timeline->setContentsMargins(0, 0, 0, 0);
    timeline->setSpacing(0);
    timeline->setAlignment(Qt::AlignTop);
    conversation->addWidget(timelineHost);

    composer = new QFrame;
    composer->setProperty("kind", "composer");
    composer->setProperty("focused", false);
    composer->setStyleSheet(QStringLiteral("background:#13161a;border:2px solid transparent;border-radius:10px;"));
    composer->setFixedHeight(100);
    auto* composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(16, 12, 12, 10);
    composerLayout->setSpacing(4);
    editor = new QPlainTextEdit;
    editor->setPlaceholderText(QStringLiteral("Message Codex"));
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editor->installEventFilter(this);
    composerLayout->addWidget(editor, 1);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto* attach = new QPushButton(QStringLiteral("Attach"));
    attach->setProperty("kind", "subtle");
    attach->setStyleSheet(QStringLiteral("text-align:left;padding:0;"));
    attach->setFixedSize(54, 24);
    attach->setEnabled(false);
    attach->setToolTip(QStringLiteral("File attachments are outside the Q4 interactive core"));
    actions->addWidget(attach);
    actions->addStretch();
    composerStatus = textLabel(
        QStringLiteral("%1 to send")
            .arg(QKeySequence(Qt::CTRL | Qt::Key_Enter).toString(QKeySequence::NativeText)),
        "meta");
    actions->addWidget(composerStatus);
    send = new QPushButton(QStringLiteral("Send"));
    send->setProperty("kind", "primary");
    send->setFixedSize(66, 32);
    send->setEnabled(false);
    actions->addWidget(send);
    stop = new QPushButton(QStringLiteral("Stop"));
    stop->setProperty("kind", "stop");
    stop->setFixedSize(66, 32);
    stop->setEnabled(false);
    actions->addWidget(stop);
    composerLayout->addLayout(actions);
    conversation->addWidget(composer);

    connect(editor, &QPlainTextEdit::textChanged, this, &ConversationWidget::updateSendEnabled);
    connect(send, &QPushButton::clicked, this, [this] {
        if (send->isEnabled())
            emit sendRequested(editor->toPlainText());
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        if (stop->isEnabled())
            emit stopRequested();
    });

    editor->setFocus(Qt::OtherFocusReason);
    scrollArea->setWidget(content);
    root->addWidget(scrollArea, 1);

    addEmptyState(timeline, QStringLiteral("No thread selected"),
                  QStringLiteral("Choose a synchronized thread from the sidebar."));
}

void ConversationWidget::render(const sdk::State& state, const QString& threadId, bool newThreadDraft)
{
    const QByteArray presentationKey = conversationPresentationKey(state, threadId, newThreadDraft);
    if (presentationKey == renderedPresentationKey)
        return;

    auto* scrollBar = scrollArea->verticalScrollBar();
    const int previousScroll = scrollBar->value();
    const bool wasNearBottom = scrollBar->maximum() - previousScroll <= 72;
    const bool threadChanged = renderedThreadId != threadId || renderedNewThreadDraft != newThreadDraft;
    const bool followLatest = threadChanged || wasNearBottom || followingLatest;
    const std::uint64_t generation = ++renderGeneration;
    renderedThreadId = threadId;
    renderedNewThreadDraft = newThreadDraft;
    renderedPresentationKey = presentationKey;
    if (threadChanged)
    {
        followingLatest = false;
        pinLatestDuringLayout = true;
        pinLatestGeneration = generation;
        QTimer::singleShot(250, this,
                           [this, generation]
                           {
                               if (generation != pinLatestGeneration)
                                   return;
                               synchronizeTimelineHeight();
                               scrollArea->widget()->layout()->activate();
                               scrollArea->widget()->adjustSize();
                               auto* latestBar = scrollArea->verticalScrollBar();
                               latestBar->setValue(latestBar->maximum());
                               pinLatestDuringLayout = false;
                           });
    }

    const auto clearTimelineState = [this]
    {
        renderedTurnIds.clear();
        renderedTurnWidgets.clear();
        renderedTurnStatusLabels.clear();
        renderedTurnItemLayouts.clear();
        renderedSegmentIds.clear();
        renderedSegmentKeys.clear();
        renderedSegmentWidgets.clear();
        clearLayout(timeline);
    };

    const auto* thread = threadId.isEmpty() ? nullptr : state.thread(threadId.toStdString());
    if (!thread)
    {
        if (timeline->count() > 0)
            clearTimelineState();
        if (!renderedSummaryKey.isEmpty() || turnSummary->isVisible() || turnFailure->isVisible())
        {
            clearLayout(turnSummaryLayout);
            renderedSummaryKey.clear();
            turnSummary->hide();
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
        contextPath->setText(id);
        contextPath->setToolTip(id);
        threadTitle->setText(title);
        threadTitle->setToolTip(title);
        QStringList metadata;
        if (thread->status && !thread->status->empty()) metadata.append(humanize(fromUtf8(*thread->status)));
        if (thread->model) metadata.append(fromUtf8(thread->model->value));
        if (thread->cwd) metadata.append(fromUtf8(thread->cwd->value));
        if (thread->realtime)
        {
            const auto realtime = sdk::realtimeSemanticView(*thread->realtime);
            metadata.append(QStringLiteral("Realtime %1 · %2 items")
                                .arg(humanize(fromUtf8(realtime.lifecycle)))
                                .arg(realtime.itemCount));
            if (realtime.transcriptTruncated) metadata.append(QStringLiteral("realtime transcript truncated"));
        }
        metadata.append(thread->fullyLoaded ? QStringLiteral("Conversation synchronized")
                                            : QStringLiteral("Conversation projection is partial"));
        threadDetail->setText(metadata.join(QStringLiteral(" · ")));
        threadDetail->setToolTip(threadDetail->text());

        const sdk::TurnState* currentTurn = nullptr;
        qsizetype currentIndex = -1;
        for (qsizetype index = 0; index < static_cast<qsizetype>(thread->orderedTurns.size()); ++index)
        {
            if (const auto* turn = state.turn(thread->orderedTurns.at(index)))
            {
                currentTurn = turn;
                currentIndex = index;
            }
        }

        const QByteArray summaryKey = turnSummaryPresentationKey(state, *thread, currentTurn, currentIndex);
        if (summaryKey != renderedSummaryKey)
        {
            renderedSummaryKey = summaryKey;
            clearLayout(turnSummaryLayout);
            turnSummary->hide();
            turnFailure->hide();
            if (currentTurn)
            {
                turnSummary->show();
                auto* summaryRow = new QHBoxLayout;
                summaryRow->setContentsMargins(0, 0, 0, 0);
                summaryRow->setSpacing(14);
                auto* identity = textLabel(
                    QStringLiteral("TURN %1 · %2").arg(currentIndex + 1).arg(compactId(currentTurn->id.value)), "small");
                identity->setToolTip(fromUtf8(currentTurn->id.value));
                summaryRow->addWidget(identity);
                const QString turnStatus = humanize(fromUtf8(currentTurn->status.value));
                summaryRow->addWidget(badge(turnStatus.toUpper(), QStringLiteral("#181c21"), statusColor(turnStatus)));
                summaryRow->addSpacing(4);

                qsizetype commands = 0;
                qsizetype changes = 0;
                qsizetype subagents = 0;
                for (const auto& itemId : currentTurn->orderedItems)
                {
                    const auto* item = state.item(thread->id, currentTurn->id, itemId);
                    if (!item) continue;
                    commands += item->kind.is(frontend::ThreadItemKind::CommandExecution) ? 1 : 0;
                    changes += item->kind.is(frontend::ThreadItemKind::FileChange) ? 1 : 0;
                    subagents += item->kind.is(frontend::ThreadItemKind::SubAgentActivity) ? 1 : 0;
                }
                summaryRow->addWidget(
                    textLabel(QStringLiteral("%1 items").arg(currentTurn->orderedItems.size()), "small"));
                if (commands) summaryRow->addWidget(textLabel(QStringLiteral("%1 commands").arg(commands), "small"));
                if (changes)
                    summaryRow->addWidget(textLabel(QStringLiteral("%1 file-change items").arg(changes), "small"));
                if (subagents)
                    summaryRow->addWidget(textLabel(QStringLiteral("%1 subagent items").arg(subagents), "small"));
                summaryRow->addStretch();
                turnSummaryLayout->addLayout(summaryRow);
                const QString usage = tokenUsageText(*currentTurn);
                if (!usage.isEmpty())
                {
                    auto* tokens = wrappingLabel(usage, "small");
                    tokens->setToolTip(usage);
                    turnSummaryLayout->addWidget(tokens);
                }
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
            if (threadChanged || !renderedTurnIds.isEmpty() || timeline->count() == 0)
            {
                clearTimelineState();
                addEmptyState(timeline, QStringLiteral("No conversation loaded"),
                              thread->fullyLoaded
                                  ? QStringLiteral("This synchronized thread does not contain any turns.")
                                  : QStringLiteral("No turn projection is currently retained for this thread."));
            }
        }
        else
        {
            std::vector<const sdk::TurnState*> visibleTurns;
            QStringList visibleTurnIds;
            for (const auto& turnId : thread->orderedTurns)
            {
                const auto* turn = state.turn(turnId);
                if (!turn)
                    continue;
                visibleTurns.push_back(turn);
                visibleTurnIds.append(fromUtf8(turn->id.value));
            }

            bool compatibleTurns = !threadChanged && timeline->count() == renderedTurnIds.size()
                                   && renderedTurnIds.size() <= visibleTurnIds.size();
            for (qsizetype index = 0; compatibleTurns && index < renderedTurnIds.size(); ++index)
                compatibleTurns = renderedTurnIds.at(index) == visibleTurnIds.at(index)
                                  && renderedTurnWidgets.contains(renderedTurnIds.at(index));
            if (!compatibleTurns)
                clearTimelineState();

            for (qsizetype index = 0; index < static_cast<qsizetype>(visibleTurns.size()); ++index)
            {
                const auto* turn = visibleTurns.at(index);
                const QString turnId = visibleTurnIds.at(index);
                QVBoxLayout* itemLayout = renderedTurnItemLayouts.value(turnId);
                QLabel* statusLabel = renderedTurnStatusLabels.value(turnId);
                if (!renderedTurnWidgets.contains(turnId))
                {
                    auto* turnWidget = timelineTurnWidget(*turn, index + 1, itemLayout, statusLabel);
                    timeline->addWidget(turnWidget);
                    renderedTurnWidgets.insert(turnId, turnWidget);
                    renderedTurnItemLayouts.insert(turnId, itemLayout);
                    renderedTurnStatusLabels.insert(turnId, statusLabel);
                }
                else
                {
                    const QString status = humanize(fromUtf8(turn->status.value));
                    if (statusLabel && statusLabel->text() != status)
                    {
                        statusLabel->setText(status);
                        statusLabel->setStyleSheet(
                            QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(statusColor(status)));
                    }
                }

                const auto segments = timelineSegments(state, *thread, *turn);
                QStringList segmentIds;
                segmentIds.reserve(static_cast<qsizetype>(segments.size()));
                for (const auto& segment : segments)
                    segmentIds.append(segment.id);

                const QStringList oldSegmentIds = renderedSegmentIds.value(turnId);
                bool compatibleSegments = oldSegmentIds.size() <= segmentIds.size();
                for (qsizetype segmentIndex = 0;
                     compatibleSegments && segmentIndex < oldSegmentIds.size();
                     ++segmentIndex)
                {
                    const QString storage = segmentStorageKey(turnId, oldSegmentIds.at(segmentIndex));
                    compatibleSegments = oldSegmentIds.at(segmentIndex) == segmentIds.at(segmentIndex)
                                         && renderedSegmentWidgets.contains(storage);
                }
                if (!compatibleSegments)
                {
                    clearLayout(itemLayout);
                    for (const QString& oldId : oldSegmentIds)
                    {
                        const QString storage = segmentStorageKey(turnId, oldId);
                        renderedSegmentKeys.remove(storage);
                        renderedSegmentWidgets.remove(storage);
                    }
                }

                for (qsizetype segmentIndex = 0; segmentIndex < static_cast<qsizetype>(segments.size()); ++segmentIndex)
                {
                    const TimelineSegment& segment = segments.at(segmentIndex);
                    const QString storage = segmentStorageKey(turnId, segment.id);
                    const QByteArray segmentKey = segmentPresentationKey(state, segment);
                    QWidget* oldWidget = renderedSegmentWidgets.value(storage);
                    if (oldWidget && renderedSegmentKeys.value(storage) == segmentKey)
                        continue;

                    QWidget* newWidget = timelineSegmentWidget(state, segment);
                    if (oldWidget)
                    {
                        const int position = itemLayout->indexOf(oldWidget);
                        itemLayout->removeWidget(oldWidget);
                        oldWidget->hide();
                        oldWidget->deleteLater();
                        itemLayout->insertWidget(position, newWidget);
                    }
                    else
                    {
                        itemLayout->addWidget(newWidget);
                    }
                    renderedSegmentWidgets.insert(storage, newWidget);
                    renderedSegmentKeys.insert(storage, segmentKey);
                }
                renderedSegmentIds.insert(turnId, segmentIds);
            }
            renderedTurnIds = visibleTurnIds;
        }
    }

    synchronizeTimelineHeight();
    scrollArea->widget()->layout()->activate();
    scrollArea->widget()->adjustSize();
    QTimer::singleShot(0, this,
                       [this, previousScroll, followLatest, threadChanged, generation]
                       {
                           if (generation != renderGeneration)
                               return;
                           synchronizeTimelineHeight();
                           scrollArea->widget()->layout()->activate();
                           scrollArea->widget()->adjustSize();
                           QTimer::singleShot(0, this,
                                              [this, previousScroll, followLatest, threadChanged, generation]
                                              {
                                                  if (generation != renderGeneration)
                                                      return;
                                                  synchronizeTimelineHeight();
                                                  scrollArea->widget()->layout()->activate();
                                                  scrollArea->widget()->adjustSize();
                                                  auto* bar = scrollArea->verticalScrollBar();
                                                  if (threadChanged)
                                                  {
                                                      scrollAnimation->stop();
                                                      bar->setValue(bar->maximum());
                                                  }
                                                  else if (followLatest)
                                                  {
                                                      scrollAnimation->stop();
                                                      const int distance = bar->maximum() - bar->value();
                                                      if (distance <= 0 || renderedThreadId.isEmpty())
                                                      {
                                                          followingLatest = false;
                                                          bar->setValue(bar->maximum());
                                                      }
                                                      else
                                                      {
                                                          followingLatest = true;
                                                          scrollAnimation->setDuration(qBound(90, distance, 220));
                                                          scrollAnimation->setStartValue(bar->value());
                                                          scrollAnimation->setEndValue(bar->maximum());
                                                          scrollAnimation->start();
                                                      }
                                                  }
                                                  else
                                                  {
                                                      followingLatest = false;
                                                      bar->setValue(qMin(previousScroll, bar->maximum()));
                                                  }
                                              });
                       });
}

void ConversationWidget::synchronizeTimelineHeight()
{
    timeline->invalidate();
    const int height = timeline->minimumSize().height();
    timelineHost->setFixedHeight(qMax(0, height));
}

void ConversationWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const std::uint64_t generation = renderGeneration;
    QTimer::singleShot(0, this,
                       [this, generation]
                       {
                           if (generation != renderGeneration)
                               return;
                           synchronizeTimelineHeight();
                           scrollArea->widget()->layout()->activate();
                           scrollArea->widget()->adjustSize();
                       });
}

void ConversationWidget::clearPrompt()
{
    editor->clear();
}

void ConversationWidget::focusComposer()
{
    editor->setFocus(Qt::OtherFocusReason);
}

void ConversationWidget::setActionState(bool sendAllowed, bool stopAllowed, bool editorAllowed)
{
    sendContextAllowed = sendAllowed;
    editor->setEnabled(editorAllowed);
    stop->setEnabled(stopAllowed);
    updateSendEnabled();
}

void ConversationWidget::setWriteStatus(const QString& text, bool error)
{
    if (text.isEmpty()) {
        composerStatus->setText(
            QStringLiteral("%1 to send")
                .arg(QKeySequence(Qt::CTRL | Qt::Key_Enter).toString(QKeySequence::NativeText)));
        composerStatus->setToolTip({});
        composerStatus->setStyleSheet({});
        return;
    }
    composerStatus->setText(compact(text, 100));
    composerStatus->setToolTip(text);
    composerStatus->setStyleSheet(error ? QStringLiteral("color:#ed6a6a;font-size:10px;")
                                        : QStringLiteral("color:#949ead;font-size:10px;"));
}

void ConversationWidget::updateSendEnabled()
{
    send->setEnabled(sendContextAllowed && editor->isEnabled() && !editor->toPlainText().trimmed().isEmpty());
}

bool ConversationWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editor && event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
        if (enter && modifiers == Qt::ControlModifier)
        {
            if (!key->isAutoRepeat() && send->isEnabled())
                emit sendRequested(editor->toPlainText());
            return true;
        }
    }
    if (watched == editor && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
    {
        const auto border =
            event->type() == QEvent::FocusIn ? QStringLiteral("#4f94f5") : QStringLiteral("transparent");
        composer->setStyleSheet(
            QStringLiteral("background:#13161a;border:2px solid %1;border-radius:10px;").arg(border));
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace codexui
