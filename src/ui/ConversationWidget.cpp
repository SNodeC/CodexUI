// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ConversationWidget.h"

#include "ui/AnchoredTurnSurface.h"
#include "ui/UpcomingTurnDock.h"

#include <ai/openai/codex/frontend/Messages.h>
#include <ai/openai/codex/frontend/client/State.h>

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSizePolicy>
#include <QStyle>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
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
    QString status;
    QString tail;
    bool truncated = false;
    std::optional<sdk::ItemContentChannel> detailChannel;
    struct DeferredItemText
    {
        sdk::State state;
        ai::openai::codex::typed::ItemId itemId;
        ai::openai::codex::typed::ThreadId threadId;
        ai::openai::codex::typed::TurnId turnId;
        sdk::ItemContentChannel channel = sdk::ItemContentChannel::AgentText;
        std::uint64_t contentRevision = 0;
        std::uint64_t utf8Bytes = 0;
    };
    std::optional<DeferredItemText> deferredDetail;
    std::optional<DeferredItemText> deferredOutput;
};

QString fromUtf8(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString fromUtf8(const std::string& value)
{
    return QString::fromStdString(value);
}

std::string_view itemContent(const sdk::ItemState& item,
                             sdk::ItemContentChannel channel) noexcept
{
    const std::optional<std::string>* content = nullptr;
    switch (channel)
    {
        case sdk::ItemContentChannel::AgentText:
            content = &item.agentText;
            break;
        case sdk::ItemContentChannel::ReasoningText:
            content = &item.reasoningText;
            break;
        case sdk::ItemContentChannel::ReasoningSummary:
            content = &item.reasoningSummary;
            break;
        case sdk::ItemContentChannel::CommandOutput:
            content = &item.commandOutput;
            break;
    }
    return content && *content ? std::string_view(**content) : std::string_view{};
}

std::optional<ActivityPresentation::DeferredItemText> deferredItemText(
    const sdk::State& state,
    const ai::openai::codex::typed::ThreadId& threadId,
    const ai::openai::codex::typed::TurnId& turnId,
    const ai::openai::codex::typed::ItemId& itemId,
    sdk::ItemContentChannel channel)
{
    const auto descriptor = state.itemContentDescriptor(
        threadId, turnId, itemId, channel);
    if (!descriptor || !descriptor->present
        || descriptor->retainedUtf8Bytes == 0)
        return std::nullopt;
    return ActivityPresentation::DeferredItemText{
        state,
        itemId,
        threadId,
        turnId,
        channel,
        descriptor->contentRevision,
        descriptor->retainedUtf8Bytes};
}

std::optional<ActivityPresentation::DeferredItemText> deferredItemText(
    const sdk::State& state,
    const sdk::ItemState& item,
    sdk::ItemContentChannel channel)
{
    const std::string_view content = itemContent(item, channel);
    if (content.empty() || !item.threadId || !item.turnId)
        return std::nullopt;
    auto source = deferredItemText(
        state, *item.threadId, *item.turnId, item.id, channel);
    if (!source
        || source->utf8Bytes != static_cast<std::uint64_t>(content.size()))
        return std::nullopt;
    return source;
}

bool sameDeferredItemText(
    const ActivityPresentation::DeferredItemText& left,
    const ActivityPresentation::DeferredItemText& right) noexcept
{
    return left.contentRevision == right.contentRevision
           && left.itemId == right.itemId
           && left.threadId == right.threadId
           && left.turnId == right.turnId
           && left.channel == right.channel
           && left.utf8Bytes == right.utf8Bytes;
}

QString materializeDeferredItemText(
    const ActivityPresentation::DeferredItemText& source)
{
    const sdk::ItemState* item = source.state.item(
        source.threadId, source.turnId, source.itemId);
    return item ? fromUtf8(itemContent(*item, source.channel)) : QString{};
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
    explicit WrappingLabel(const QString& text, bool markdown = false)
        : markdown(markdown)
    {
        setTextFormat(markdown ? Qt::RichText : Qt::PlainText);
        setWordWrap(true);
        QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        if (markdown) {
            setTextInteractionFlags(Qt::TextBrowserInteraction);
            setOpenExternalLinks(false);
            connect(this, &QLabel::linkActivated, this, [](const QString& target) {
                const QUrl url = QUrl::fromUserInput(target);
                if (url.scheme() == QStringLiteral("https")
                    || url.scheme() == QStringLiteral("http"))
                    (void)QDesktopServices::openUrl(url);
            });
        }
        setContent(text);
    }

    bool setContent(const QString& text)
    {
        if (text == sourceText)
            return false;
        sourceText = text;
        setProperty("sourceText", sourceText);
        if (!markdown) {
            const int previousHeight = preferredHeight();
            heightCache.clear();
            QLabel::setText(text);
            updateGeometry();
            return previousHeight != preferredHeight();
        }

        return renderMarkdownNow();
    }

    [[nodiscard]] const QString& content() const noexcept { return sourceText; }

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
        if (markdown && event->type() == QEvent::FontChange)
            renderMarkdownNow();
        QLabel::changeEvent(event);
    }

private:
    [[nodiscard]] int preferredHeight() const
    {
        const int availableWidth = width();
        return availableWidth > 0 ? heightForWidth(availableWidth) : sizeHint().height();
    }

    bool renderMarkdownNow()
    {
        const int previousHeight = preferredHeight();
        heightCache.clear();
        setTextFormat(Qt::RichText);
        QLabel::setText(safeMarkdownHtml(sourceText, font()));
        updateGeometry();
        setProperty("markdownRenderMode", QStringLiteral("markdown"));
        return previousHeight != preferredHeight();
    }

    static QString safeMarkdownHtml(const QString& markdownText, const QFont& renderFont)
    {
        QTextDocument document;
        document.setDefaultFont(renderFont);
        document.setDefaultStyleSheet(QStringLiteral(
            "a{color:#2f6feb;} code{font-family:monospace;background:#eef1f5;}"
            "pre{font-family:monospace;background:#eef1f5;white-space:pre-wrap;}"));
        document.setMarkdown(
            markdownText,
            QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                | QTextDocument::MarkdownNoHTML);

        struct ImageRange { int start; int length; QString alt; };
        std::vector<ImageRange> images;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
            for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
                const QTextFragment fragment = iterator.fragment();
                if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                    continue;
                const QTextImageFormat format = fragment.charFormat().toImageFormat();
                images.push_back(ImageRange{
                    fragment.position(),
                    fragment.length(),
                    format.property(QTextFormat::ImageAltText).toString()});
            }
        }
        for (auto iterator = images.rbegin(); iterator != images.rend(); ++iterator) {
            QTextCursor cursor(&document);
            cursor.setPosition(iterator->start);
            cursor.setPosition(iterator->start + iterator->length, QTextCursor::KeepAnchor);
            cursor.insertText(iterator->alt.isEmpty()
                ? QStringLiteral("[Image]")
                : QStringLiteral("[Image: %1]").arg(iterator->alt));
        }
        return document.toHtml();
    }

    bool markdown = false;
    QString sourceText;
    mutable QHash<int, int> heightCache;
};

class StreamingMessageView final : public QTextEdit
{
public:
    explicit StreamingMessageView(const QString& text)
        : sourceText(text)
        , sourceUtf8Bytes(static_cast<std::uint64_t>(text.toUtf8().size()))
    {
        measurementDocument = new QTextDocument(this);
        QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setReadOnly(true);
        setUndoRedoEnabled(false);
        setAcceptRichText(false);
        setFrameStyle(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setStyleSheet(QStringLiteral("QTextEdit{background:transparent;border:0;padding:0;}"));
        viewport()->setAutoFillBackground(false);
        document()->setDocumentMargin(0.0);
        document()->setDefaultFont(font());
        document()->setPlainText(text);
        measurementDocument->setDocumentMargin(0.0);
        measurementDocument->setDefaultFont(font());
        measurementDocument->setPlainText(text);
        setProperty("sourceUtf8Bytes", static_cast<qulonglong>(sourceUtf8Bytes));
        setProperty("streamAppendCount", 0);
        setProperty("fullReplacementCount", 0);
        setProperty("geometryInvalidationCount", 0);
        setProperty("sourceMaterializationCount", 0);
        setProperty("markdownRenderMode", QStringLiteral("streaming-plain"));
    }

    [[nodiscard]] const QString& content() const noexcept { return sourceText; }
    [[nodiscard]] std::uint64_t utf8Bytes() const noexcept { return sourceUtf8Bytes; }

    bool replaceContent(const QString& text)
    {
        if (text == sourceText)
            return false;
        if (text.startsWith(sourceText))
        {
            const auto applied = applyAppend(
                sourceUtf8Bytes, 0, text.mid(sourceText.size()));
            return applied.value_or(false);
        }
        const int previousHeight = preferredHeight();
        sourceText = text;
        sourceUtf8Bytes = static_cast<std::uint64_t>(text.toUtf8().size());
        document()->setPlainText(text);
        measurementDocument->setPlainText(text);
        heightCache.clear();
        const bool geometryChanged = previousHeight != preferredHeight();
        setProperty("sourceUtf8Bytes", static_cast<qulonglong>(sourceUtf8Bytes));
        setProperty("fullReplacementCount", property("fullReplacementCount").toULongLong() + 1);
        if (geometryChanged)
            invalidateGeometry();
        return geometryChanged;
    }

    std::optional<bool> applyAppend(std::uint64_t baseContentBytes,
                                    std::uint64_t discardPrefixBytes,
                                    const QString& delta)
    {
        if (baseContentBytes != sourceUtf8Bytes || discardPrefixBytes > sourceUtf8Bytes)
            return std::nullopt;

        const int previousHeight = preferredHeight();
        const QByteArray deltaUtf8 = delta.toUtf8();
        if (discardPrefixBytes == 0)
        {
            sourceText.append(delta);
            QTextCursor cursor(document());
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(delta);
            QTextCursor measurementCursor(measurementDocument);
            measurementCursor.movePosition(QTextCursor::End);
            measurementCursor.insertText(delta);
        }
        else
        {
            const QByteArray previousUtf8 = sourceText.toUtf8();
            const QByteArray nextUtf8 = previousUtf8.mid(
                static_cast<qsizetype>(discardPrefixBytes)) + deltaUtf8;
            sourceText = QString::fromUtf8(nextUtf8);
            document()->setPlainText(sourceText);
            measurementDocument->setPlainText(sourceText);
        }
        sourceUtf8Bytes = baseContentBytes - discardPrefixBytes
                          + static_cast<std::uint64_t>(deltaUtf8.size());
        heightCache.clear();
        const bool geometryChanged = previousHeight != preferredHeight();
        setProperty("sourceUtf8Bytes", static_cast<qulonglong>(sourceUtf8Bytes));
        setProperty("streamAppendCount", property("streamAppendCount").toULongLong() + 1);
        if (geometryChanged)
            invalidateGeometry();
        return geometryChanged;
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        const auto found = heightCache.constFind(width);
        if (found != heightCache.cend())
            return *found;
        const qreal textWidth = qMax(1, width);
        if (measurementDocument->textWidth() != textWidth)
            measurementDocument->setTextWidth(textWidth);
        const int height = qCeil(measurementDocument->size().height());
        heightCache.insert(width, height);
        return height;
    }

    QSize sizeHint() const override
    {
        const int preferredWidth = width() > 0 ? width() : 480;
        return QSize(preferredWidth, heightForWidth(preferredWidth));
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        // This view grows with its document; the enclosing conversation owns
        // vertical navigation.
        event->ignore();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QTextEdit::resizeEvent(event);
        const qreal textWidth = qMax(1, viewport()->width());
        if (document()->textWidth() != textWidth)
            document()->setTextWidth(textWidth);
    }

    void changeEvent(QEvent* event) override
    {
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        {
            document()->setDefaultFont(font());
            if (measurementDocument)
                measurementDocument->setDefaultFont(font());
            heightCache.clear();
            invalidateGeometry();
        }
        QTextEdit::changeEvent(event);
    }

private:
    void invalidateGeometry()
    {
        setProperty(
            "geometryInvalidationCount",
            property("geometryInvalidationCount").toULongLong() + 1);
        updateGeometry();
    }

    [[nodiscard]] int preferredHeight() const
    {
        const int availableWidth = width();
        return availableWidth > 0 ? heightForWidth(availableWidth) : sizeHint().height();
    }

    QString sourceText;
    std::uint64_t sourceUtf8Bytes = 0;
    QTextDocument* measurementDocument = nullptr;
    mutable QHash<int, int> heightCache;
};

QLabel* wrappingLabel(const QString& text, const char* kind = nullptr)
{
    auto* result = new WrappingLabel(text);
    if (kind) result->setProperty("kind", kind);
    return result;
}

QWidget* messageContentWidget(const QString& text, bool streaming)
{
    if (text.size() <= largeMessageEditorThreshold && streaming)
    {
        auto* result = new StreamingMessageView(text);
        result->setProperty("kind", "body");
        return result;
    }
    if (text.size() <= largeMessageEditorThreshold)
    {
        auto* result = new WrappingLabel(text, true);
        result->setProperty("kind", "body");
        result->setProperty("sourceUtf8Bytes", static_cast<qulonglong>(text.toUtf8().size()));
        result->setProperty("sourceMaterializationCount", 0);
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
    result->setProperty("sourceUtf8Bytes", static_cast<qulonglong>(text.toUtf8().size()));
    result->setProperty("streamAppendCount", 0);
    result->setProperty("fullReplacementCount", 0);
    result->setProperty("sourceMaterializationCount", 0);
    result->setProperty("markdownRenderMode", QStringLiteral("large-plain"));
    return result;
}

QString messageContentText(QWidget* content)
{
    content->setProperty(
        "sourceMaterializationCount",
        content->property("sourceMaterializationCount").toULongLong() + 1);
    if (const auto* label = dynamic_cast<const WrappingLabel*>(content))
        return label->content();
    if (const auto* streaming = dynamic_cast<const StreamingMessageView*>(content))
        return streaming->content();
    if (const auto* editor = qobject_cast<const QPlainTextEdit*>(content))
        return editor->toPlainText();
    return {};
}

qsizetype messageContentSize(const QWidget* content)
{
    if (const auto* label = dynamic_cast<const WrappingLabel*>(content))
        return label->content().size();
    if (const auto* streaming = dynamic_cast<const StreamingMessageView*>(content))
        return streaming->content().size();
    if (const auto* editor = qobject_cast<const QPlainTextEdit*>(content))
        return qMax(0, editor->document()->characterCount() - 1);
    return 0;
}

std::uint64_t messageContentUtf8Bytes(const QWidget* content)
{
    if (const auto* streaming = dynamic_cast<const StreamingMessageView*>(content))
        return streaming->utf8Bytes();
    return content->property("sourceUtf8Bytes").toULongLong();
}

bool setMessageContentText(QWidget* content,
                           const QString& text)
{
    if (auto* label = dynamic_cast<WrappingLabel*>(content))
    {
        const bool geometryChanged = label->setContent(text);
        label->setProperty("sourceUtf8Bytes", static_cast<qulonglong>(text.toUtf8().size()));
        return geometryChanged;
    }
    if (auto* streamingView = dynamic_cast<StreamingMessageView*>(content))
        return streamingView->replaceContent(text);
    else if (auto* editor = qobject_cast<QPlainTextEdit*>(content); editor && editor->toPlainText() != text)
    {
        editor->setPlainText(text);
        editor->setProperty("sourceUtf8Bytes", static_cast<qulonglong>(text.toUtf8().size()));
        editor->setProperty("fullReplacementCount", editor->property("fullReplacementCount").toULongLong() + 1);
        return false;
    }
    return false;
}

std::optional<bool> appendMessageContent(QWidget* content,
                                         std::uint64_t baseContentBytes,
                                         std::uint64_t discardPrefixBytes,
                                         const QString& delta)
{
    if (auto* streamingView = dynamic_cast<StreamingMessageView*>(content))
        return streamingView->applyAppend(baseContentBytes, discardPrefixBytes, delta);

    auto* editor = qobject_cast<QPlainTextEdit*>(content);
    if (!editor)
        return std::nullopt;
    const std::uint64_t currentBytes = editor->property("sourceUtf8Bytes").toULongLong();
    if (currentBytes != baseContentBytes || discardPrefixBytes > currentBytes)
        return std::nullopt;

    const QByteArray deltaUtf8 = delta.toUtf8();

    if (discardPrefixBytes == 0)
    {
        QTextCursor cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(delta);
    }
    else
    {
        const QByteArray previousUtf8 = editor->toPlainText().toUtf8();
        editor->setPlainText(
            QString::fromUtf8(previousUtf8.mid(static_cast<qsizetype>(discardPrefixBytes))
                              + deltaUtf8));
    }
    const std::uint64_t nextBytes = baseContentBytes - discardPrefixBytes
                                    + static_cast<std::uint64_t>(deltaUtf8.size());
    editor->setProperty("sourceUtf8Bytes", static_cast<qulonglong>(nextBytes));
    editor->setProperty("streamAppendCount", editor->property("streamAppendCount").toULongLong() + 1);
    return false;
}

bool messageContentWidgetMatches(const QWidget* content,
                                 qsizetype textSize,
                                 bool streaming)
{
    const bool needsEditor = textSize > largeMessageEditorThreshold;
    const bool hasEditor = qobject_cast<const QPlainTextEdit*>(content) != nullptr;
    const bool needsStreamingView = !needsEditor && streaming;
    const bool hasStreamingView = dynamic_cast<const StreamingMessageView*>(content) != nullptr;
    const bool hasMarkdownView = dynamic_cast<const WrappingLabel*>(content) != nullptr;
    return (needsEditor && hasEditor)
           || (needsStreamingView && hasStreamingView)
           || (!needsEditor && !needsStreamingView && hasMarkdownView);
}

QWidget* ensureMessageContentWidget(QVBoxLayout* layout,
                                    QWidget* content,
                                    const QString& text,
                                    bool streaming)
{
    if (messageContentWidgetMatches(content, text.size(), streaming))
        return content;

    QWidget* replacement = messageContentWidget(text, streaming);
    replacement->setObjectName(QStringLiteral("conversationMessageContent"));
    replacement->setProperty(
        "sourceMaterializationCount",
        content->property("sourceMaterializationCount"));
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
    bool streaming = false;
};

bool streamingMessageStatus(const QString& status)
{
    const QString normalized = status.toLower();
    return normalized == QStringLiteral("started")
           || normalized == QStringLiteral("unknown")
           || normalized.contains(QStringLiteral("progress"))
           || normalized.contains(QStringLiteral("running"))
           || normalized.contains(QStringLiteral("active"))
           || normalized.contains(QStringLiteral("stream"));
}

bool turnStreamsMessages(const sdk::TurnState& turn) noexcept
{
    return !turn.terminal && (turn.active || turn.connectionInvalidated);
}

MessagePresentation messagePresentationMetadata(const sdk::ItemState& item,
                                                bool user,
                                                bool turnStreaming)
{
    MessagePresentation result;
    const QString itemStatusText = itemStatus(item);
    result.status = itemStatusText;
    result.statusColor = statusColor(itemStatusText);
    result.streaming = !user
                       && (turnStreaming || streamingMessageStatus(itemStatusText));

    if (!user)
    {
        const auto semantic = sdk::itemSemanticView(item);
        const auto* agent = semantic ? std::get_if<sdk::AgentMessageSemanticView>(&semantic->details) : nullptr;
        if (agent && agent->phase)
            result.status += QStringLiteral(" · ") + humanize(fromUtf8(*agent->phase));
    }

    const auto userMessage = user ? sdk::userMessageSemanticView(item) : std::nullopt;
    result.truncation = userMessage ? userMessageTruncationText(*userMessage)
                                    : truncationText(item);
    return result;
}

MessagePresentation messagePresentation(const sdk::ItemState& item,
                                        bool user,
                                        bool turnStreaming)
{
    MessagePresentation result = messagePresentationMetadata(
        item, user, turnStreaming);
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

    return result;
}

bool applyMessageMetadata(QLabel* status,
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

    bool geometryChanged = false;
    if (truncation->text() != presentation.truncation)
    {
        truncation->setText(presentation.truncation);
        geometryChanged = truncation->isVisible();
    }
    const bool truncationVisible = !presentation.truncation.isEmpty();
    geometryChanged = geometryChanged || truncation->isVisible() != truncationVisible;
    truncation->setVisible(truncationVisible);
    return geometryChanged;
}

bool applyMessagePresentation(QLabel* status,
                              QWidget* content,
                              QLabel* truncation,
                              const MessagePresentation& presentation)
{
    const bool metadataGeometryChanged = applyMessageMetadata(
        status, content, truncation, presentation);
    return setMessageContentText(content, presentation.content)
           || metadataGeometryChanged;
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
                                          bool includeOutput = true,
                                          bool includeReasoningContent = true)
{
    ActivityPresentation result;
    result.title = item.kind.known ? knownKindTitle(*item.kind.known) : QStringLiteral("Unknown item");
    result.status = itemStatus(item);
    result.truncated = item.truncated || item.contentTruncated || !item.omittedFields.empty();
    if (!item.kind.is(frontend::ThreadItemKind::Reasoning)
        && item.summary && !item.summary->empty())
        result.detail = fromUtf8(*item.summary);

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
    if (includeOutput)
        result.deferredOutput = deferredItemText(
            state, item, sdk::ItemContentChannel::CommandOutput);

    if (includeReasoningContent && item.kind.is(frontend::ThreadItemKind::Reasoning))
    {
        if (item.reasoningSummary && !item.reasoningSummary->empty())
        {
            result.detailChannel = sdk::ItemContentChannel::ReasoningSummary;
            result.deferredDetail = deferredItemText(
                state, item, *result.detailChannel);
        }
        else if (item.reasoningText && !item.reasoningText->empty())
        {
            result.detailChannel = sdk::ItemContentChannel::ReasoningText;
            result.deferredDetail = deferredItemText(
                state, item, *result.detailChannel);
        }
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

QToolButton* disclosureButton(bool expanded,
                              const QString& accessibleName,
                              bool activityRow = false)
{
    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("activityDisclosure"));
    button->setProperty("activityRow", activityRow);
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
        "QToolButton#activityDisclosure[activityRow=\"true\"]{padding-left:4px;padding-right:0;"
        "padding-top:0;padding-bottom:0;}"
        "QToolButton#activityDisclosure:hover{background:#f1f5fb;border-color:#d7dee8;}"
        "QToolButton#activityDisclosure:pressed{background:#e9eff7;border-color:#d7dee8;}"
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
    output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit#conversationActivityOutput{font-family:monospace;font-size:11px;"
        "background:#fbfcfe;border:1px solid #e1e7ef;border-radius:6px;padding:6px;}"
        "QAbstractScrollArea::corner{background:transparent;}"
        "QScrollBar:vertical{background:transparent;border:0;width:8px;margin:2px;}"
        "QScrollBar::handle:vertical{background:#b9c4d2;min-height:28px;border-radius:3px;}"
        "QScrollBar::handle:vertical:hover{background:#98a2b3;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{background:transparent;border:0;height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
        "QScrollBar:horizontal{background:transparent;border:0;height:8px;margin:2px;}"
        "QScrollBar::handle:horizontal{background:#b9c4d2;min-width:28px;border-radius:3px;}"
        "QScrollBar::handle:horizontal:hover{background:#98a2b3;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{background:transparent;border:0;width:0;}"
        "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}"));
    output->setPlainText(text);
    output->setProperty("sourceUtf8Bytes", static_cast<qulonglong>(text.toUtf8().size()));
    output->setProperty("streamAppendCount", 0);
    output->setProperty("fullReplacementCount", 0);
    return output;
}

class ActivityDetails final : public QWidget
{
public:
    bool replaceDeferredDetail(
        std::optional<ActivityPresentation::DeferredItemText> source)
    {
        const bool previouslyAvailable = hasDetail();
        const bool contentChanged = source.has_value() != deferredDetail.has_value()
                                    || (source && deferredDetail
                                        && !sameDeferredItemText(
                                            *source, *deferredDetail));
        deferredDetail = std::move(source);
        deferredDetailDirty = deferredDetail.has_value()
                              && (deferredDetailDirty || contentChanged);
        detailBytes = deferredDetail ? deferredDetail->utf8Bytes : 0;
        if (deferredDetail)
            detailChannel = deferredDetail->channel;
        else
            detailChannel.reset();
        setProperty(
            "deferredDetailBytes",
            static_cast<qulonglong>(detailBytes));
        return previouslyAvailable != hasDetail();
    }

    void clearDeferredDetail()
    {
        deferredDetail.reset();
        deferredDetailDirty = false;
        detailBytes = 0;
        detailChannel.reset();
        setProperty("deferredDetailBytes", 0ULL);
    }

    bool ensureDeferredDetail(QVBoxLayout* layout)
    {
        if (!layout || !deferredDetail || !deferredDetailDirty)
            return false;

        const QString text = materializeDeferredItemText(*deferredDetail);
        auto* detail = findChild<QWidget*>(
            QStringLiteral("conversationActivityDetail"));
        auto* streaming = dynamic_cast<StreamingMessageView*>(detail);
        const bool compatible = streaming
                                && detail->property("activityContentChannel").toInt()
                                       == static_cast<int>(deferredDetail->channel);
        bool geometryChanged = false;
        if (!compatible)
        {
            auto* replacement = new StreamingMessageView(text);
            replacement->setObjectName(
                QStringLiteral("conversationActivityDetail"));
            replacement->setProperty("kind", "meta");
            replacement->setProperty(
                "activityContentChannel",
                static_cast<int>(deferredDetail->channel));
            replacement->style()->unpolish(replacement);
            replacement->style()->polish(replacement);
            if (detail)
            {
                delete layout->replaceWidget(detail, replacement);
                detail->hide();
                detail->deleteLater();
            }
            else
            {
                layout->insertWidget(0, replacement);
            }
            geometryChanged = true;
        }
        else
        {
            geometryChanged = streaming->replaceContent(text);
        }
        if (auto* current = findChild<QWidget*>(
                QStringLiteral("conversationActivityDetail"));
            current && current->isHidden())
        {
            current->show();
            geometryChanged = true;
        }
        deferredDetailDirty = false;
        ++detailMaterializationCount;
        setProperty(
            "detailMaterializationCount",
            static_cast<qulonglong>(detailMaterializationCount));
        return geometryChanged;
    }

    [[nodiscard]] bool acceptsDetailAppend(
        sdk::ItemContentChannel channel,
        std::uint64_t baseContentBytes,
        std::uint64_t discardPrefixBytes) const noexcept
    {
        return baseContentBytes == detailBytes
               && discardPrefixBytes <= detailBytes
               && (!detailChannel || *detailChannel == channel);
    }

    void recordMaterializedDetailSource(
        ActivityPresentation::DeferredItemText source)
    {
        detailBytes = source.utf8Bytes;
        detailChannel = source.channel;
        deferredDetail = std::move(source);
        deferredDetailDirty = false;
        setProperty(
            "deferredDetailBytes",
            static_cast<qulonglong>(detailBytes));
    }

    bool replaceOutput(
        std::optional<ActivityPresentation::DeferredItemText> source)
    {
        const bool previouslyAvailable = hasOutput();
        const bool contentChanged = source.has_value() != deferredOutput.has_value()
                                    || (source && deferredOutput
                                        && !sameDeferredItemText(
                                            *source, *deferredOutput));
        deferredOutput = std::move(source);
        outputBytes = deferredOutput ? deferredOutput->utf8Bytes : 0;
        deferredOutputDirty = deferredOutput.has_value()
                              && (deferredOutputDirty || contentChanged);
        setProperty("deferredOutputBytes", static_cast<qulonglong>(outputBytes));
        if (!deferredOutput && outputEditor)
        {
            outputEditor->hide();
            if (outputHeading)
                outputHeading->hide();
        }
        return previouslyAvailable != hasOutput();
    }

    bool materializeOutput(QVBoxLayout* layout)
    {
        if (!layout || !deferredOutput || !deferredOutputDirty)
            return false;
        const QString text = materializeDeferredItemText(*deferredOutput);
        bool geometryChanged = false;
        if (!outputEditor)
        {
            if (!outputHeading)
            {
                outputHeading = textLabel(QStringLiteral("Output"), "small");
                outputHeading->setObjectName(
                    QStringLiteral("conversationActivityOutputHeading"));
                outputHeading->setStyleSheet(
                    QStringLiteral("font-size:10px;font-weight:600;color:#475467;"));
                const auto* incomplete = findChild<QLabel*>(
                    QStringLiteral("conversationActivityIncomplete"));
                const int headingPosition = incomplete
                                                ? layout->indexOf(incomplete)
                                                : layout->count();
                layout->insertWidget(headingPosition, outputHeading);
            }
            outputEditor = activityOutputWidget(text);
            const auto* incomplete = findChild<QLabel*>(
                QStringLiteral("conversationActivityIncomplete"));
            const int position = incomplete
                                     ? layout->indexOf(incomplete)
                                     : layout->count();
            layout->insertWidget(position, outputEditor);
            geometryChanged = true;
        }
        else
        {
            const bool followsEnd = outputEditor->verticalScrollBar()->maximum()
                                    - outputEditor->verticalScrollBar()->value() <= 2;
            const int previousScroll = outputEditor->verticalScrollBar()->value();
            geometryChanged = setMessageContentText(outputEditor, text);
            outputEditor->verticalScrollBar()->setValue(
                followsEnd ? outputEditor->verticalScrollBar()->maximum()
                           : qMin(previousScroll,
                                  outputEditor->verticalScrollBar()->maximum()));
        }
        outputEditor->show();
        if (outputHeading)
            outputHeading->show();
        deferredOutputDirty = false;
        ++outputMaterializationCount;
        setProperty(
            "outputMaterializationCount",
            static_cast<qulonglong>(outputMaterializationCount));
        return geometryChanged;
    }

    void recordMaterializedOutputSource(
        ActivityPresentation::DeferredItemText source)
    {
        outputBytes = source.utf8Bytes;
        deferredOutput = std::move(source);
        deferredOutputDirty = false;
        setProperty(
            "deferredOutputBytes",
            static_cast<qulonglong>(outputBytes));
    }

    std::optional<bool> applyOutputAppend(std::uint64_t baseContentBytes,
                                          std::uint64_t discardPrefixBytes,
                                          const QString& delta)
    {
        if (!outputEditor || deferredOutputDirty
            || baseContentBytes != outputBytes
            || discardPrefixBytes > outputBytes)
            return std::nullopt;

        const QByteArray deltaUtf8 = delta.toUtf8();
        const bool followsEnd = outputEditor->verticalScrollBar()->maximum()
                                - outputEditor->verticalScrollBar()->value() <= 2;
        const int previousScroll = outputEditor->verticalScrollBar()->value();
        const auto applied = appendMessageContent(
            outputEditor, baseContentBytes, discardPrefixBytes, delta);
        if (!applied)
            return std::nullopt;
        outputBytes = baseContentBytes - discardPrefixBytes
                      + static_cast<std::uint64_t>(deltaUtf8.size());
        outputEditor->verticalScrollBar()->setValue(
            followsEnd ? outputEditor->verticalScrollBar()->maximum()
                       : qMin(previousScroll, outputEditor->verticalScrollBar()->maximum()));
        return *applied;
    }

    QPlainTextEdit* ensureOutput(QVBoxLayout* layout)
    {
        if (!layout || !hasOutput())
            return outputEditor;
        static_cast<void>(materializeOutput(layout));
        return outputEditor;
    }

    [[nodiscard]] bool hasDetail() const noexcept
    {
        if (detailBytes != 0)
            return true;
        const auto* detail = findChild<QWidget*>(
            QStringLiteral("conversationActivityDetail"));
        return detail && !detail->isHidden();
    }
    [[nodiscard]] bool hasOutput() const noexcept { return outputBytes != 0; }
    [[nodiscard]] std::uint64_t retainedDetailBytes() const noexcept
    {
        return detailBytes;
    }
    [[nodiscard]] std::uint64_t retainedOutputBytes() const noexcept { return outputBytes; }
    [[nodiscard]] QPlainTextEdit* output() const noexcept { return outputEditor; }
    [[nodiscard]] QLabel* heading() const noexcept { return outputHeading; }

private:
    std::optional<ActivityPresentation::DeferredItemText> deferredDetail;
    std::optional<ActivityPresentation::DeferredItemText> deferredOutput;
    std::optional<sdk::ItemContentChannel> detailChannel;
    std::uint64_t detailBytes = 0;
    std::uint64_t outputBytes = 0;
    std::uint64_t detailMaterializationCount = 0;
    std::uint64_t outputMaterializationCount = 0;
    bool deferredDetailDirty = false;
    bool deferredOutputDirty = false;
    QPlainTextEdit* outputEditor = nullptr;
    QLabel* outputHeading = nullptr;
};

QWidget* activityDetailWidget(const ActivityPresentation& presentation)
{
    QWidget* detail = nullptr;
    if (presentation.detailChannel)
    {
        auto* streaming = new StreamingMessageView(presentation.detail);
        streaming->setProperty(
            "activityContentChannel",
            static_cast<int>(*presentation.detailChannel));
        detail = streaming;
    }
    else
    {
        detail = wrappingLabel(presentation.detail, "meta");
    }
    detail->setObjectName(QStringLiteral("conversationActivityDetail"));
    detail->setProperty("kind", "meta");
    detail->style()->unpolish(detail);
    detail->style()->polish(detail);
    return detail;
}

bool updateActivityDetail(ActivityDetails* details,
                          QVBoxLayout* layout,
                          const ActivityPresentation& presentation)
{
    if (!details || !layout)
        return false;
    if (presentation.deferredDetail)
    {
        bool changed = details->replaceDeferredDetail(
            presentation.deferredDetail);
        if (details->isVisible())
            changed = details->ensureDeferredDetail(layout) || changed;
        return changed;
    }
    details->clearDeferredDetail();
    QWidget* detail = details->findChild<QWidget*>(
        QStringLiteral("conversationActivityDetail"));
    if (presentation.detail.isEmpty())
    {
        const bool changed = detail && !detail->isHidden();
        if (detail)
            detail->hide();
        return changed;
    }

    const auto expectedChannel = presentation.detailChannel;
    const auto* streaming = dynamic_cast<StreamingMessageView*>(detail);
    const bool compatible = expectedChannel
                                ? streaming
                                      && detail->property("activityContentChannel").toInt()
                                             == static_cast<int>(*expectedChannel)
                                : detail && !streaming;
    bool geometryChanged = false;
    if (!compatible)
    {
        QWidget* replacement = activityDetailWidget(presentation);
        if (detail)
        {
            delete layout->replaceWidget(detail, replacement);
            detail->hide();
            detail->deleteLater();
        }
        else
        {
            layout->insertWidget(0, replacement);
        }
        detail = replacement;
        geometryChanged = true;
    }
    else if (auto* streamingDetail = dynamic_cast<StreamingMessageView*>(detail))
    {
        geometryChanged = streamingDetail->replaceContent(presentation.detail);
    }
    else if (auto* label = dynamic_cast<WrappingLabel*>(detail))
    {
        geometryChanged = label->setContent(presentation.detail);
    }
    if (detail->isHidden())
    {
        detail->show();
        geometryChanged = true;
    }
    return geometryChanged;
}

bool updateActivityDisclosureAvailability(QWidget* row)
{
    if (!row)
        return false;
    auto* details = dynamic_cast<ActivityDetails*>(
        row->findChild<QWidget*>(QStringLiteral("conversationActivityDetails")));
    auto* disclosure = row->findChild<QToolButton*>(QStringLiteral("activityDisclosure"));
    if (!details || !disclosure)
        return false;
    const auto* incomplete = details->findChild<QLabel*>(
        QStringLiteral("conversationActivityIncomplete"));
    const bool hasDetails = details->hasDetail() || details->hasOutput()
                            || (incomplete && !incomplete->isHidden());
    bool changed = disclosure->isVisible() != hasDetails;
    if (auto* prefix = row->findChild<QWidget*>(
            QStringLiteral("conversationActivityPrefix")))
    {
        changed = changed || prefix->width() != (hasDetails ? 31 : 14);
        prefix->setFixedWidth(hasDetails ? 31 : 14);
    }
    if (auto* leadingLayout = row->findChild<QHBoxLayout*>(
            QStringLiteral("conversationActivityLeadingLayout")))
    {
        changed = changed || leadingLayout->spacing() != (hasDetails ? 0 : 6);
        leadingLayout->setSpacing(hasDetails ? 0 : 6);
    }
    disclosure->setEnabled(hasDetails);
    disclosure->setVisible(hasDetails);
    if (!hasDetails)
        setDisclosureState(disclosure, details, false);
    return changed;
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
    summary->setObjectName(QStringLiteral("conversationActivitySummary"));
    auto* layout = new QHBoxLayout(summary);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    const bool hasDetails = !item.detail.isEmpty()
                            || item.deferredDetail.has_value()
                            || item.deferredOutput.has_value()
                            || item.truncated;
    const QString color = statusColor(item.status);
    auto* prefix = new QWidget;
    prefix->setObjectName(QStringLiteral("conversationActivityPrefix"));
    prefix->setFixedSize(hasDetails ? 31 : 14, 22);

    auto* symbol = textLabel(statusGlyph(item.status));
    symbol->setParent(prefix);
    symbol->setObjectName(QStringLiteral("conversationActivitySymbol"));
    symbol->setFixedSize(14, 22);
    symbol->move(0, 0);
    symbol->setAlignment(Qt::AlignCenter);
    symbol->setAttribute(Qt::WA_TransparentForMouseEvents);
    symbol->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(color));

    auto* disclosure = disclosureButton(
        expanded,
        QStringLiteral("Activity details: %1").arg(item.title),
        true);
    disclosure->setParent(prefix);
    disclosure->move(9, 0);
    disclosure->setEnabled(hasDetails);
    disclosure->setVisible(hasDetails);
    symbol->raise();

    auto* title = wrappingLabel(item.title);
    title->setObjectName(QStringLiteral("conversationActivityTitle"));
    title->setToolTip(plainTooltip(item.title));
    title->setStyleSheet(QStringLiteral("font-size:12px;font-weight:500;"));
    // Align the first wrapped text line with the 22 px status/disclosure
    // controls.  A top content inset keeps later lines flowing downward
    // instead of vertically centering the complete multiline label.
    const int titleTopInset = qMax(0, (disclosure->height() - title->fontMetrics().height()) / 2);
    title->setContentsMargins(0, titleTopInset, 0, 0);
    title->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    title->setMinimumHeight(disclosure->height());

    // Keep a native 22 px hit target without letting that hit target define
    // the visible columns.  Its right-pointing chevron begins at the same x as
    // titles on rows without details, and the following title retains the
    // header's measured seven-pixel visible gap.
    auto* leading = new QWidget;
    auto* leadingLayout = new QHBoxLayout(leading);
    leadingLayout->setObjectName(QStringLiteral("conversationActivityLeadingLayout"));
    leadingLayout->setContentsMargins(0, 0, 0, 0);
    leadingLayout->setSpacing(hasDetails ? 0 : 6);
    leadingLayout->addWidget(prefix, 0, Qt::AlignTop);
    leadingLayout->addWidget(title, 1);
    layout->addWidget(leading, 1);

    auto* tail = textLabel(item.tail, "meta");
    tail->setObjectName(QStringLiteral("conversationActivityTail"));
    tail->setFixedHeight(disclosure->height());
    tail->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(tail, 0, Qt::AlignTop);
    tail->setVisible(!item.tail.isEmpty());
    auto* state = textLabel(item.status);
    state->setObjectName(QStringLiteral("conversationActivityStatus"));
    state->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(color));
    state->setFixedHeight(disclosure->height());
    state->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(state, 0, Qt::AlignTop);
    lineLayout->addWidget(summary);

    auto* details = new ActivityDetails;
    details->setObjectName(QStringLiteral("conversationActivityDetails"));
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(42, 0, 4, 4);
    detailsLayout->setSpacing(6);
    if (item.deferredDetail)
    {
        details->replaceDeferredDetail(item.deferredDetail);
        if (expanded)
            details->ensureDeferredDetail(detailsLayout);
    }
    else if (!item.detail.isEmpty())
    {
        detailsLayout->addWidget(activityDetailWidget(item));
    }
    if (item.deferredOutput)
    {
        details->replaceOutput(item.deferredOutput);
        if (expanded)
            details->ensureOutput(detailsLayout);
    }
    if (item.truncated)
    {
        auto* omitted = textLabel(QStringLiteral("Canonical activity detail is incomplete"), "small");
        omitted->setObjectName(QStringLiteral("conversationActivityIncomplete"));
        omitted->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
        detailsLayout->addWidget(omitted);
    }
    lineLayout->addWidget(details);
    setDisclosureState(disclosure, details, expanded && hasDetails);
    QObject::connect(disclosure, &QToolButton::clicked, line,
                     [disclosure, details, detailsLayout, layoutChanged](bool)
                     {
                         const bool next = !details->isVisible();
                         if (next)
                         {
                             details->ensureDeferredDetail(detailsLayout);
                             details->ensureOutput(detailsLayout);
                         }
                         setDisclosureState(disclosure, details, next);
                         if (layoutChanged)
                             layoutChanged();
                     });
    rows->addWidget(line);
}

bool updateActivityRowMetadata(QWidget* row,
                               const ActivityPresentation& item,
                               bool contentGeometryChanged,
                               bool* geometryChanged)
{
    if (!row)
        return false;
    auto* title = dynamic_cast<WrappingLabel*>(
        row->findChild<QLabel*>(QStringLiteral("conversationActivityTitle")));
    auto* symbol = row->findChild<QLabel*>(QStringLiteral("conversationActivitySymbol"));
    auto* tail = row->findChild<QLabel*>(QStringLiteral("conversationActivityTail"));
    auto* status = row->findChild<QLabel*>(QStringLiteral("conversationActivityStatus"));
    auto* details = dynamic_cast<ActivityDetails*>(
        row->findChild<QWidget*>(QStringLiteral("conversationActivityDetails")));
    auto* disclosure = row->findChild<QToolButton*>(QStringLiteral("activityDisclosure"));
    if (!title || !symbol || !tail || !status || !details || !disclosure)
        return false;
    auto* detailsLayout = qobject_cast<QVBoxLayout*>(details->layout());
    if (!detailsLayout)
        return false;

    bool changed = contentGeometryChanged || title->setContent(item.title);
    title->setToolTip(plainTooltip(item.title));
    disclosure->setAccessibleName(QStringLiteral("Activity details: %1").arg(item.title));
    symbol->setText(statusGlyph(item.status));
    symbol->setStyleSheet(
        QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(statusColor(item.status)));
    if (tail->text() != item.tail)
        tail->setText(item.tail);
    const bool tailVisible = !item.tail.isEmpty();
    changed = changed || tail->isVisible() != tailVisible;
    tail->setVisible(tailVisible);
    status->setText(item.status);
    status->setStyleSheet(
        QStringLiteral("color:%1;font-size:9px;font-weight:600;").arg(statusColor(item.status)));

    auto* incomplete = details->findChild<QLabel*>(QStringLiteral("conversationActivityIncomplete"));
    if (!incomplete && item.truncated)
    {
        incomplete = textLabel(QStringLiteral("Canonical activity detail is incomplete"), "small");
        incomplete->setObjectName(QStringLiteral("conversationActivityIncomplete"));
        incomplete->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
        detailsLayout->addWidget(incomplete);
        changed = true;
    }
    if (incomplete)
    {
        changed = changed || incomplete->isVisible() != item.truncated;
        incomplete->setVisible(item.truncated);
    }

    changed = updateActivityDisclosureAvailability(row) || changed;

    if (changed)
    {
        detailsLayout->invalidate();
        details->updateGeometry();
        if (QLayout* rowLayout = row->layout())
            rowLayout->invalidate();
        row->updateGeometry();
    }
    if (geometryChanged)
        *geometryChanged = changed;
    return true;
}

bool updateActivityRow(QWidget* row,
                       const ActivityPresentation& item,
                       bool* geometryChanged)
{
    auto* details = row
                        ? dynamic_cast<ActivityDetails*>(row->findChild<QWidget*>(
                              QStringLiteral("conversationActivityDetails")))
                        : nullptr;
    auto* detailsLayout = details ? qobject_cast<QVBoxLayout*>(details->layout()) : nullptr;
    if (!details || !detailsLayout)
        return false;

    bool contentGeometryChanged = updateActivityDetail(details, detailsLayout, item);
    contentGeometryChanged = details->replaceOutput(item.deferredOutput)
                             || contentGeometryChanged;
    if (details->isVisible())
    {
        contentGeometryChanged = details->ensureDeferredDetail(detailsLayout)
                                 || contentGeometryChanged;
        if (details->hasOutput())
        {
            contentGeometryChanged = details->materializeOutput(detailsLayout)
                                     || contentGeometryChanged;
        }
    }
    return updateActivityRowMetadata(
        row, item, contentGeometryChanged, geometryChanged);
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
    layout->addLayout(header);
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
    header->addWidget(planAvailable);
    planAvailable->setVisible(typedPlanAvailable || legacyPlanAvailable);
    header->addSpacing(8);
    auto* count = textLabel(
        QStringLiteral("%1 activit%2").arg(items.size()).arg(items.size() == 1 ? "y" : "ies"),
        "small");
    count->setObjectName(QStringLiteral("conversationActivityCount"));
    header->addWidget(count);
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
    layout->addWidget(body);
    setDisclosureState(disclosure, body, expanded);
    QObject::connect(disclosure, &QToolButton::clicked, card,
                     [disclosure, body, layoutChanged](bool)
                     {
                         const bool next = !body->isVisible();
                         if (next)
                         {
                             for (auto* candidate : body->findChildren<QWidget*>(
                                      QStringLiteral("conversationActivityDetails")))
                             {
                                 auto* details = dynamic_cast<ActivityDetails*>(candidate);
                                 if (!details)
                                     continue;
                                 if (details->isHidden())
                                     continue;
                                 auto* detailsLayout = qobject_cast<QVBoxLayout*>(
                                     details->layout());
                                 details->ensureDeferredDetail(detailsLayout);
                                 details->ensureOutput(detailsLayout);
                             }
                         }
                         setDisclosureState(disclosure, body, next);
                         if (layoutChanged)
                             layoutChanged();
                     });
    return card;
}

void addMessage(QVBoxLayout* timeline,
                const sdk::ItemState& item,
                bool user,
                bool turnStreaming)
{
    const MessagePresentation presentation = messagePresentation(
        item, user, turnStreaming);
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
    auto* copy = messageContentWidget(presentation.content, presentation.streaming);
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

bool addItemContentIdentity(QCryptographicHash& hash,
                            const sdk::State& state,
                            const sdk::ItemState& item,
                            sdk::ItemContentChannel channel)
{
    if (!item.threadId || !item.turnId)
    {
        addPresentationValue(hash, false);
        return false;
    }
    const auto descriptor = state.itemContentDescriptor(
        *item.threadId, *item.turnId, item.id, channel);
    addPresentationValue(hash, descriptor.has_value());
    if (!descriptor)
        return false;
    addPresentationValue(hash, descriptor->present);
    addPresentationValue(
        hash,
        QByteArray::number(static_cast<qulonglong>(descriptor->retainedUtf8Bytes)));
    addPresentationValue(
        hash,
        QByteArray::number(static_cast<qulonglong>(descriptor->contentRevision)));
    return true;
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
    bool earlierItemsOmitted = false;
};

QString segmentStorageKey(const QString& turnId, const QString& segmentId)
{
    return turnId + QChar(0x1f) + segmentId;
}

std::vector<TimelineSegment> timelineSegments(const sdk::State& state,
                                              const sdk::ThreadState& thread,
                                              const sdk::TurnState& turn,
                                              qsizetype firstItem,
                                              qsizetype endItem)
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
    const qsizetype scanEnd = qMin(
        qMax(scanStart, endItem),
        static_cast<qsizetype>(turn.orderedItems.size()));
    qsizetype activityBucket = -1;
    for (qsizetype index = scanStart; index < scanEnd; ++index)
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

std::vector<TimelineSegment> timelineSegments(const sdk::State& state,
                                              const sdk::ThreadState& thread,
                                              const sdk::TurnState& turn,
                                              qsizetype firstItem)
{
    return timelineSegments(
        state,
        thread,
        turn,
        firstItem,
        static_cast<qsizetype>(turn.orderedItems.size()));
}

qsizetype timelineItemCount(const TimelineSegment& segment)
{
    return qMax<qsizetype>(1, static_cast<qsizetype>(segment.items.size()));
}

TimelineWindow latestTimelineWindow(const sdk::State& state, const sdk::ThreadState& thread)
{
    TimelineWindow result;
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
    result.earlierItemsOmitted = !result.turns.empty()
                                 && (result.turns.front().turnNumber > 1
                                     || result.turns.front().firstItem > 0);
    // Keep this presentation-only count a bounded lower bound. Computing the
    // exact retained count required an all-turn scan on every live item event.
    result.totalItems = result.renderedItems
                        + (result.earlierItemsOmitted ? 1 : 0);
    return result;
}

bool incompleteStateContainsRenderedTimeline(
    const sdk::State& state,
    const sdk::ThreadState& thread,
    const QStringList& renderedTurnIds,
    const QHash<QString, QPair<qsizetype, qsizetype>>& renderedTurnItemRanges,
    const QHash<QString, QStringList>& renderedSegmentIds,
    const QHash<QString, QStringList>& renderedSegmentItemIds,
    qsizetype& inspectedItems)
{
    inspectedItems = 0;
    constexpr qsizetype maximumRecoveryInspectedItems =
        maximumRenderedTimelineItems
        + (static_cast<qsizetype>(maximumActivityItemsPerSegment) - 1)
              * maximumRenderedTimelineTurns;
    for (const QString& renderedTurnId : renderedTurnIds)
    {
        const sdk::TurnState* retainedTurn = state.turn(
            thread.id,
            ai::openai::codex::typed::TurnId{
                renderedTurnId.toStdString()});
        if (!retainedTurn)
            return false;

        const auto range = renderedTurnItemRanges.constFind(renderedTurnId);
        if (range == renderedTurnItemRanges.cend()
            || range->first < 0 || range->second < range->first
            || range->second
                   > static_cast<qsizetype>(retainedTurn->orderedItems.size()))
            return false;
        const qsizetype activityWidth =
            static_cast<qsizetype>(maximumActivityItemsPerSegment);
        const qsizetype scanStart =
            range->first - range->first % activityWidth;
        const qsizetype inspectedRange = range->second - scanStart;
        if (inspectedRange < 0
            || inspectedRange
                   > maximumRecoveryInspectedItems - inspectedItems)
            return false;
        inspectedItems += inspectedRange;

        const std::vector<TimelineSegment> retainedSegments =
            timelineSegments(
                state,
                thread,
                *retainedTurn,
                range->first,
                range->second);
        QHash<QString, QSet<QString>> retainedItemsBySegment;
        retainedItemsBySegment.reserve(
            static_cast<qsizetype>(retainedSegments.size()));
        for (const TimelineSegment& segment : retainedSegments)
        {
            QSet<QString> retainedItemIds;
            retainedItemIds.reserve(
                static_cast<qsizetype>(segment.items.size()));
            for (const sdk::ItemState* item : segment.items)
            {
                if (item)
                    retainedItemIds.insert(fromUtf8(item->id.value));
            }
            retainedItemsBySegment.insert(
                segment.id, std::move(retainedItemIds));
        }
        for (const QString& renderedSegmentId :
             renderedSegmentIds.value(renderedTurnId))
        {
            const auto retained =
                retainedItemsBySegment.constFind(renderedSegmentId);
            if (retained == retainedItemsBySegment.cend())
                return false;
            const QString storage = segmentStorageKey(
                renderedTurnId, renderedSegmentId);
            for (const QString& renderedItemId :
                 renderedSegmentItemIds.value(storage))
            {
                if (!retained->contains(renderedItemId))
                    return false;
            }
        }
    }
    return true;
}

QByteArray segmentPresentationKey(const sdk::State& state,
                                  const TimelineSegment& segment,
                                  bool typedPlanAvailable,
                                  bool turnStreaming,
                                  bool threadFullyLoaded)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addPresentationValue(hash, segment.id);
    addPresentationValue(hash, segment.missing);
    addPresentationValue(hash, typedPlanAvailable);
    addPresentationValue(hash, turnStreaming);
    // A bounded backend snapshot can preserve an empty turn shell while
    // omitting its descendants. Completeness affects only that empty-state
    // presentation; populated segments should keep their stable identity.
    if (segment.items.empty())
        addPresentationValue(hash, threadFullyLoaded);
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
                // User text has no append channel and is normally immutable;
                // hash it directly so an equal-length authoritative repair is
                // never mistaken for unchanged content.
                addPresentationValue(hash, message->text);
                addPresentationValue(hash, userMessageTruncationText(*message));
            }
        }
        else if (item->kind.is(frontend::ThreadItemKind::AgentMessage))
        {
            const std::string_view content = item->agentText && !item->agentText->empty()
                                                 ? std::string_view(*item->agentText)
                                                 : (item->summary
                                                        ? std::string_view(*item->summary)
                                                        : std::string_view{});
            if (!item->agentText || item->agentText->empty()
                || !addItemContentIdentity(
                    hash, state, *item, sdk::ItemContentChannel::AgentText))
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
            const bool reasoning = item->kind.is(frontend::ThreadItemKind::Reasoning);
            const ActivityPresentation presentation = activityPresentation(
                state, *item, false, !reasoning);
            addPresentationValue(hash, presentation.title);
            addPresentationValue(hash, presentation.detail);
            if (!addItemContentIdentity(
                    hash, state, *item, sdk::ItemContentChannel::CommandOutput)
                && item->commandOutput)
                addPresentationValue(hash, std::string_view(*item->commandOutput));
            if (reasoning)
            {
                if (!addItemContentIdentity(
                        hash, state, *item, sdk::ItemContentChannel::ReasoningText)
                    && item->reasoningText)
                    addPresentationValue(hash, std::string_view(*item->reasoningText));
                if (!addItemContentIdentity(
                        hash, state, *item, sdk::ItemContentChannel::ReasoningSummary)
                    && item->reasoningSummary)
                    addPresentationValue(hash, std::string_view(*item->reasoningSummary));
            }
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
                               bool turnStreaming,
                               bool threadFullyLoaded,
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
        ActivityPresentation omitted;
        omitted.title = QStringLiteral("Unavailable item");
        omitted.detail = QStringLiteral(
            "The ordered item shell is not retained in current State");
        omitted.status = QStringLiteral("Omitted");
        omitted.truncated = true;
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
        addEmptyState(
            layout,
            threadFullyLoaded ? QStringLiteral("No items in this turn")
                              : QStringLiteral("Conversation history incomplete"),
            threadFullyLoaded
                ? QStringLiteral("The synchronized turn currently has no retained items.")
                : QStringLiteral(
                    "Some turns or items are unavailable in the current synchronized view."));
    }
    else if (segment.items.size() == 1
             && (segment.items.front()->kind.is(frontend::ThreadItemKind::UserMessage)
                 || segment.items.front()->kind.is(frontend::ThreadItemKind::AgentMessage)))
    {
        const auto* item = segment.items.front();
        const bool user = item->kind.is(frontend::ThreadItemKind::UserMessage);
        host->setProperty("messageUser", user);
        addMessage(layout, *item, user, turnStreaming);
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

std::optional<std::uint64_t> exactAppendResultBytes(
    const ConversationContentAppend& append) noexcept
{
    if (append.discardPrefixBytes > append.baseContentBytes
        || append.baseContentBytes - append.discardPrefixBytes
               > std::numeric_limits<std::uint64_t>::max()
                     - append.deltaUtf8Bytes)
        return std::nullopt;
    return append.baseContentBytes - append.discardPrefixBytes
           + append.deltaUtf8Bytes;
}

bool applyExactActivityAppend(const sdk::State& state,
                              const ai::openai::codex::typed::ThreadId& threadId,
                              QWidget* row,
                              const ConversationContentUpdate& update,
                              bool* geometryChanged,
                              bool* mayShrink)
{
    if (!row || !update.append)
        return false;
    const ConversationContentAppend& append = *update.append;
    auto* details = dynamic_cast<ActivityDetails*>(
        row->findChild<QWidget*>(QStringLiteral("conversationActivityDetails")));
    auto* detailsLayout = details ? qobject_cast<QVBoxLayout*>(details->layout()) : nullptr;
    if (!details || !detailsLayout)
        return false;

    const auto expectedBytes = exactAppendResultBytes(append);
    if (!expectedBytes)
        return false;
    auto source = deferredItemText(
        state,
        threadId,
        ai::openai::codex::typed::TurnId{update.turnId.toStdString()},
        ai::openai::codex::typed::ItemId{update.itemId.toStdString()},
        update.channel);
    if (!source || source->utf8Bytes != *expectedBytes)
        return false;

    bool contentGeometryChanged = false;
    if (update.channel == sdk::ItemContentChannel::CommandOutput)
    {
        if (!details->isVisible())
        {
            if (details->retainedOutputBytes() != append.baseContentBytes
                || append.discardPrefixBytes > append.baseContentBytes)
                return false;
            contentGeometryChanged = details->replaceOutput(source);
        }
        else
        {
            const auto applied = details->applyOutputAppend(
                append.baseContentBytes, append.discardPrefixBytes, append.delta);
            if (!applied)
                return false;
            contentGeometryChanged = *applied;
            details->recordMaterializedOutputSource(std::move(*source));
        }
    }
    else if ((update.channel == sdk::ItemContentChannel::ReasoningText
              || update.channel == sdk::ItemContentChannel::ReasoningSummary))
    {
        if (!details->acceptsDetailAppend(
                update.channel,
                append.baseContentBytes,
                append.discardPrefixBytes))
            return false;
        if (!details->isVisible())
        {
            contentGeometryChanged = details->replaceDeferredDetail(source);
        }
        else
        {
            auto* detail = dynamic_cast<StreamingMessageView*>(
                details->findChild<QWidget*>(QStringLiteral("conversationActivityDetail")));
            if (!detail && append.baseContentBytes == 0
                && append.discardPrefixBytes == 0)
            {
                contentGeometryChanged = details->replaceDeferredDetail(source);
                contentGeometryChanged = details->ensureDeferredDetail(detailsLayout)
                                         || contentGeometryChanged;
            }
            else
            {
                if (!detail
                    || detail->property("activityContentChannel").toInt()
                           != static_cast<int>(update.channel))
                    return false;
                const auto applied = detail->applyAppend(
                    append.baseContentBytes, append.discardPrefixBytes, append.delta);
                if (!applied)
                    return false;
                contentGeometryChanged = *applied;
                details->recordMaterializedDetailSource(std::move(*source));
            }
        }
    }
    else
    {
        return false;
    }

    contentGeometryChanged = updateActivityDisclosureAvailability(row)
                             || contentGeometryChanged;
    if (contentGeometryChanged)
    {
        detailsLayout->invalidate();
        details->updateGeometry();
        if (QLayout* rowLayout = row->layout())
            rowLayout->invalidate();
        row->updateGeometry();
    }
    if (geometryChanged)
        *geometryChanged = contentGeometryChanged;
    if (mayShrink)
        *mayShrink = append.discardPrefixBytes > append.deltaUtf8Bytes;
    return true;
}

bool updateTimelineActivitySegment(QWidget* host,
                                   const sdk::State& state,
                                   const TimelineSegment& segment,
                                   bool typedPlanAvailable,
                                   const ConversationContentUpdates* exactContentChanges,
                                   bool* geometryChanged,
                                   bool* mayShrink,
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
    bool anyGeometryChanged = false;
    bool anyMayShrink = false;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto* item = segment.items.at(index);
        bool rowGeometryChanged = false;
        bool rowMayShrink = false;
        bool handledExactly = false;
        if (exactContentChanges)
        {
            for (const ConversationContentUpdate& update : *exactContentChanges)
            {
                if (update.itemId != fromUtf8(item->id.value))
                    continue;
                if (item->threadId)
                    handledExactly = applyExactActivityAppend(
                        state, *item->threadId, rows.at(index), update,
                        &rowGeometryChanged, &rowMayShrink);
                if (!handledExactly)
                    break;
            }
            if (std::none_of(
                    exactContentChanges->cbegin(), exactContentChanges->cend(),
                    [item](const ConversationContentUpdate& update)
                    { return update.itemId == fromUtf8(item->id.value); }))
                continue;
        }
        if (!handledExactly)
        {
            if (!updateActivityRow(
                    rows.at(index), activityPresentation(state, *item),
                    &rowGeometryChanged))
                return false;
            // A full authoritative replacement may shorten any wrapping
            // detail or hide output/truncation UI.
            rowMayShrink = rowGeometryChanged;
        }
        anyGeometryChanged = anyGeometryChanged || rowGeometryChanged;
        anyMayShrink = anyMayShrink || rowMayShrink;
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
        anyGeometryChanged = true;
    }
    const QString countText = QStringLiteral("%1 activit%2")
                                  .arg(segment.items.size())
                                  .arg(segment.items.size() == 1 ? "y" : "ies");
    anyGeometryChanged = anyGeometryChanged || count->text() != countText;
    count->setText(countText);
    const bool nextPlanVisible = typedPlanAvailable
                                 || std::ranges::any_of(
                                     segment.items, [](const sdk::ItemState* item) {
                                         return item && item->kind.is(frontend::ThreadItemKind::Plan);
                                     });
    anyGeometryChanged = anyGeometryChanged
                         || planAvailable->isVisible() != nextPlanVisible;
    planAvailable->setVisible(nextPlanVisible);
    if (anyGeometryChanged)
    {
        rowsLayout->invalidate();
        if (QLayout* hostLayout = host->layout())
            hostLayout->invalidate();
        host->updateGeometry();
    }
    if (geometryChanged)
        *geometryChanged = anyGeometryChanged;
    if (mayShrink)
        *mayShrink = anyMayShrink;
    return true;
}

bool updateTimelineMessageSegment(QWidget* host,
                                  const TimelineSegment& segment,
                                  bool turnStreaming,
                                  bool* geometryChanged,
                                  bool* mayShrink)
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
    const MessagePresentation presentation = messagePresentation(
        *item, user, turnStreaming);
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
    QWidget* previousContentWidget = contentWidget;
    contentWidget = ensureMessageContentWidget(
        contentLayout, contentWidget, presentation.content, presentation.streaming);
    const bool rendererChanged = previousContentWidget != contentWidget;
    if (mayShrink)
        *mayShrink = *mayShrink || rendererChanged;
    const bool presentationGeometryChanged = applyMessagePresentation(
        status, contentWidget, truncation, presentation);
    if (geometryChanged)
        *geometryChanged = rendererChanged || presentationGeometryChanged;
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
    layoutSettleTimer = new QTimer(this);
    layoutSettleTimer->setSingleShot(true);
    layoutSettleTimer->setInterval(16);
    connect(layoutSettleTimer, &QTimer::timeout, this, &ConversationWidget::settleTimelineLayout);
    connect(conversationScroll, &QScrollBar::rangeChanged, this,
            [this, conversationScroll](int, int maximum)
            {
                if (pinLatestDuringLayout || followingLatest)
                    conversationScroll->setValue(maximum);
            });
    connect(conversationScroll, &QScrollBar::actionTriggered, this,
            [this, conversationScroll](int)
            {
                followingLatest = false;
                pendingFollowLatest = false;
                pendingPreviousScroll = conversationScroll->value();
                pendingViewportAnchor.clear();
            });
    connect(conversationScroll, &QScrollBar::sliderPressed, this,
            [this, conversationScroll]
            {
                followingLatest = false;
                pendingFollowLatest = false;
                pendingPreviousScroll = conversationScroll->value();
                pendingViewportAnchor.clear();
            });
    connect(conversationScroll, &QScrollBar::valueChanged, this,
            [this, conversationScroll]
            {
                if (conversationScroll->maximum() - conversationScroll->value() > 72)
                    followingLatest = false;
                requestDeferredPresentationAtTail();
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

bool ConversationWidget::shouldFreezePresentation(const QString& threadId,
                                                   bool newThreadDraft) const
{
    if (threadId.isEmpty() || threadId != renderedThreadId
        || newThreadDraft != renderedNewThreadDraft || pinLatestDuringLayout)
        return false;
    const auto* bar = scrollArea->verticalScrollBar();
    return bar->maximum() - bar->value() > 72;
}

void ConversationWidget::markPresentationDeferred()
{
    deferredPresentationPending = true;
}

void ConversationWidget::requestDeferredPresentationAtTail()
{
    if (!deferredPresentationPending || deferredPresentationRequestScheduled)
        return;
    const auto* bar = scrollArea->verticalScrollBar();
    if (bar->maximum() - bar->value() > 72)
        return;

    deferredPresentationRequestScheduled = true;
    QTimer::singleShot(0, this,
                       [this]
                       {
                           deferredPresentationRequestScheduled = false;
                           if (!deferredPresentationPending)
                               return;
                           const auto* settledBar = scrollArea->verticalScrollBar();
                           if (settledBar->maximum() - settledBar->value() > 72)
                               return;
                           deferredPresentationPending = false;
                           emit latestPresentationRequested();
                       });
}

void ConversationWidget::render(const sdk::State& state,
                                const QString& threadId,
                                bool newThreadDraft,
                                const ConversationContentUpdates* exactContentChanges,
                                bool structurallyAffected)
{
    auto* scrollBar = scrollArea->verticalScrollBar();
    const int previousScroll = scrollBar->value();
    const bool wasNearBottom = scrollBar->maximum() - previousScroll <= 72;
    const bool threadChanged = renderedThreadId != threadId || renderedNewThreadDraft != newThreadDraft;
    const auto* thread = threadId.isEmpty() ? nullptr : state.thread(threadId.toStdString());
    // Capacity provenance is only needed to classify a missing selection.
    // Avoid decoding its complete diagnostic object for every live delta of
    // a thread that is already present.
    const auto capacityProvenance = thread
                                        ? decltype(state.capacityProvenance()){}
                                        : state.capacityProvenance();
    const bool selectedThreadOmitted = !newThreadDraft && !thread
                                       && !threadId.isEmpty()
                                       && capacityProvenance
                                       && capacityProvenance->omittedThreads > 0;
    const bool selectedThreadUnresolved = !newThreadDraft && !thread
                                          && !threadId.isEmpty()
                                          && (selectedThreadOmitted
                                              || (state.threadList().value
                                                  && !state.threadList().value->complete));
    const bool missingThreadPresentationChanged =
        !thread && !threadChanged
        && (renderedThreadFullyLoaded.has_value()
            || renderedSelectedThreadOmitted != selectedThreadOmitted);
    const bool threadCompletenessChanged = thread
                                           && (!renderedThreadFullyLoaded
                                               || *renderedThreadFullyLoaded
                                                      != thread->fullyLoaded);
    const bool structuralReconciliation = structurallyAffected
                                          && !threadChanged
                                          && !threadCompletenessChanged
                                          && thread && !newThreadDraft;
    if (!structuralReconciliation)
    {
        upcomingTurnDock->setCanonicalConfiguration(
            thread ? thread->executionConfiguration
                   : std::optional<sdk::ExecutionConfiguration>{},
            thread ? threadId : QString{},
            newThreadDraft);
    }
    if (!threadChanged && shouldFreezePresentation(threadId, newThreadDraft))
    {
        markPresentationDeferred();
        return;
    }
    // Exact content appends cannot remove turns or items. Apply them before
    // the bounded incomplete-history proof so streaming on a partial thread
    // remains proportional to the changed bytes. A structural publication
    // still has to derive the new segment topology, but must not apply these
    // mutations a second time during that reconciliation.
    const bool exactContentApplied = exactContentChanges
                                     && !threadChanged
                                     && !threadCompletenessChanged
                                     && thread && !newThreadDraft
                                     && updateExactMessageContent(
                                         state, threadId, *exactContentChanges);
    if (exactContentApplied && !structuralReconciliation)
        return;
    // A bounded replacement is not deletion authority. Keep the same-thread
    // widgets until an incomplete publication can account for every rendered
    // descendant; requester-local Merge will make that true, while Replace is
    // necessarily fullyLoaded and exact Absent changes the selection.
    if (!structuralReconciliation)
    {
        const bool renderedTimelineRetained = !renderedTurnIds.isEmpty();
        qsizetype recoveryInspectedItems = 0;
        const bool incompleteTimelineRetained =
            !thread || thread->fullyLoaded
            || incompleteStateContainsRenderedTimeline(
                state,
                *thread,
                renderedTurnIds,
                renderedTurnItemRanges,
                renderedSegmentIds,
                renderedSegmentItemIds,
                recoveryInspectedItems);
        timelineHost->setProperty(
            "recoveryInspectedTimelineItems", recoveryInspectedItems);
        const bool incompleteTimelineRegressed =
            !threadChanged && renderedTimelineRetained
            && (selectedThreadUnresolved
                || !incompleteTimelineRetained);
        if (incompleteTimelineRegressed)
        {
            const QString recovery = QStringLiteral("History recovery pending");
            if (!threadDetail->text().contains(recovery))
            {
                const QString detail = threadDetail->text();
                threadDetail->setText(
                    detail.isEmpty()
                        ? recovery
                        : detail + QStringLiteral(" · ") + recovery);
                threadDetail->setToolTip(threadDetail->text());
            }
            return;
        }
    }
    if (!thread && !threadChanged && !missingThreadPresentationChanged)
        return;
    if (threadChanged)
    {
        deferredPresentationPending = false;
        deferredPresentationRequestScheduled = false;
    }
    const bool followLatest = threadChanged || wasNearBottom || followingLatest;
    const bool exactContentOnly = !structuralReconciliation
                                  && exactContentChanges && !threadChanged
                                  && !threadCompletenessChanged && thread && !newThreadDraft
                                  && !renderedSummaryKey.isEmpty();
    const std::uint64_t generation = ++renderGeneration;
    bool timelineShrank = false;
    bool timelineGeometryChanged = threadChanged;
    if (threadChanged)
        pendingViewportAnchor.clear();
    else if (!followLatest && !layoutSettleTimer->isActive())
        captureTimelineAnchor();
    else if (followLatest)
        pendingViewportAnchor.clear();
    renderedThreadId = threadId;
    renderedNewThreadDraft = newThreadDraft;
    if (thread)
        renderedThreadFullyLoaded = thread->fullyLoaded;
    else
        renderedThreadFullyLoaded.reset();
    renderedSelectedThreadOmitted = selectedThreadOmitted;
    if (threadChanged)
    {
        followingLatest = false;
        pinLatestDuringLayout = true;
        pinLatestGeneration = generation;
        scrollArea->viewport()->setUpdatesEnabled(false);
    }

    const auto clearTimelineState = [this, &timelineShrank, &timelineGeometryChanged]
    {
        timelineShrank = timelineShrank || timeline->count() > 0;
        timelineGeometryChanged = timelineGeometryChanged || timeline->count() > 0;
        renderedTurnIds.clear();
        renderedTurnWidgets.clear();
        renderedTurnLabels.clear();
        renderedTurnStatusLabels.clear();
        renderedTurnItemLayouts.clear();
        renderedTurnItemRanges.clear();
        renderedSegmentIds.clear();
        renderedSegmentItemIds.clear();
        renderedSegmentKeys.clear();
        renderedSegmentWidgets.clear();
        renderedActivityRows.clear();
        renderedActivityRowSegments.clear();
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
        QString pathText;
        QString titleText;
        QString detailText;
        QString emptyTitle;
        QString emptyDetail;
        if (newThreadDraft) {
            pathText = QStringLiteral("New thread draft");
            titleText = QStringLiteral("New conversation");
            detailText = QStringLiteral(
                "A real thread will be created when the first prompt is sent");
            emptyTitle = QStringLiteral("Start a new conversation");
            emptyDetail = QStringLiteral(
                "Type a prompt below. Backend defaults will be used for the new thread.");
        } else if (selectedThreadOmitted) {
            pathText = QStringLiteral("History incomplete");
            titleText = QStringLiteral("Conversation history incomplete");
            detailText = QStringLiteral(
                "This conversation is not available in the current synchronized view.");
            emptyTitle = titleText;
            emptyDetail = detailText;
        } else {
            pathText = QStringLiteral("No thread selected");
            titleText = QStringLiteral("No synchronized thread");
            detailText = QStringLiteral(
                "Select a synchronized thread to view its conversation");
            emptyTitle = QStringLiteral("No thread selected");
            emptyDetail = QStringLiteral(
                "Choose a synchronized thread from the sidebar.");
        }
        contextPath->setText(pathText);
        contextPath->setToolTip({});
        threadTitle->setText(titleText);
        threadTitle->setToolTip({});
        threadDetail->setText(detailText);
        threadDetail->setToolTip({});
        timelineWindowNotice->hide();
        timelineHost->setProperty("renderedTimelineItems", 0);
        timelineHost->setProperty("retainedTimelineItems", 0);
        addEmptyState(timeline, emptyTitle, emptyDetail);
        timelineGeometryChanged = true;
    }
    else
    {
        if (!structuralReconciliation)
        {
            const QString id = fromUtf8(thread->id.value);
            const QString title = thread->title && !thread->title->empty()
                                      ? fromUtf8(*thread->title)
                                      : id;
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
                                .arg(thread->orderedTurns.size() == 1
                                         ? QString{}
                                         : QStringLiteral("s")));
            if (!thread->fullyLoaded)
                metadata.append(QStringLiteral("History incomplete"));
            threadDetail->setText(metadata.join(QStringLiteral(" · ")));
            threadDetail->setToolTip(threadDetail->text());
        }

        const sdk::TurnState* currentTurn = nullptr;
        qsizetype currentIndex = -1;
        std::optional<TimelineWindow> structuralWindow;
        if (structuralReconciliation)
        {
            structuralWindow.emplace(latestTimelineWindow(state, *thread));
            if (!structuralWindow->turns.empty())
                currentTurn = structuralWindow->turns.back().turn;
        }
        else
        {
            for (qsizetype index = 0;
                 index < static_cast<qsizetype>(thread->orderedTurns.size());
                 ++index)
            {
                if (const auto* turn = state.turn(
                        thread->id, thread->orderedTurns.at(index)))
                {
                    currentTurn = turn;
                    currentIndex = index;
                }
            }
        }

        if (!structuralReconciliation)
        {
            const QByteArray summaryKey = exactContentOnly
                                              ? renderedSummaryKey
                                              : turnSummaryPresentationKey(
                                                    currentTurn, currentIndex);
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
        }

        if (!currentTurn)
        {
            timelineWindowNotice->hide();
            timelineHost->setProperty("renderedTimelineItems", 0);
            timelineHost->setProperty("retainedTimelineItems", 0);
            if (threadChanged || threadCompletenessChanged
                || !renderedTurnIds.isEmpty() || timeline->count() == 0)
            {
                clearTimelineState();
                addEmptyState(
                    timeline,
                    thread->fullyLoaded ? QStringLiteral("Ready for the first turn")
                                        : QStringLiteral("Conversation history incomplete"),
                    thread->fullyLoaded
                        ? QStringLiteral("Use the upcoming-turn dock below to start this thread.")
                        : QStringLiteral(
                            "Some turns or items are unavailable in the current synchronized view."));
                timelineGeometryChanged = true;
            }
        }
        else
        {
            const TimelineWindow window = structuralWindow
                                              ? std::move(*structuralWindow)
                                              : latestTimelineWindow(state, *thread);
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
            if (window.earlierItemsOmitted)
            {
                timelineWindowDetail->setText(
                    QStringLiteral("Showing the latest %1 synchronized timeline entries. "
                                   "Earlier entries remain in canonical AISuite State and are not "
                                   "materialized in this live view.")
                        .arg(window.renderedItems));
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

            const auto forgetRenderedActivityRows = [this](const QString& storage)
            {
                for (auto row = renderedActivityRowSegments.begin();
                     row != renderedActivityRowSegments.end();)
                {
                    if (row.value() != storage)
                    {
                        ++row;
                        continue;
                    }
                    renderedActivityRows.remove(row.key());
                    row = renderedActivityRowSegments.erase(row);
                }
            };
            const auto forgetRenderedSegment =
                [this, &forgetRenderedActivityRows](const QString& storage)
            {
                forgetRenderedActivityRows(storage);
                renderedSegmentItemIds.remove(storage);
                renderedSegmentKeys.remove(storage);
                renderedSegmentWidgets.remove(storage);
            };
            const auto removeRenderedTurn = [this,
                                             &forgetRenderedSegment,
                                             &timelineShrank,
                                             &timelineGeometryChanged](const QString& turnId)
            {
                for (const QString& segmentId : renderedSegmentIds.take(turnId))
                {
                    const QString storage = segmentStorageKey(turnId, segmentId);
                    forgetRenderedSegment(storage);
                }
                renderedTurnLabels.remove(turnId);
                renderedTurnStatusLabels.remove(turnId);
                renderedTurnItemLayouts.remove(turnId);
                renderedTurnItemRanges.remove(turnId);
                if (QWidget* widget = renderedTurnWidgets.take(turnId))
                {
                    if (pendingViewportAnchor == widget
                        || (pendingViewportAnchor && widget->isAncestorOf(pendingViewportAnchor)))
                        pendingViewportAnchor.clear();
                    timeline->removeWidget(widget);
                    widget->hide();
                    widget->deleteLater();
                    timelineShrank = true;
                    timelineGeometryChanged = true;
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
                const auto windowSlice = std::ranges::find_if(
                    window.turns,
                    [turn](const TimelineTurnSlice& slice) {
                        return slice.turn == turn;
                    });
                if (windowSlice != window.turns.cend())
                {
                    renderedTurnItemRanges.insert(
                        turnId,
                        qMakePair(
                            windowSlice->firstItem,
                            static_cast<qsizetype>(
                                turn->orderedItems.size())));
                }
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
                    timelineGeometryChanged = true;
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
                const QSet<QString> nextSegmentIds(
                    segmentIds.cbegin(), segmentIds.cend());
                for (const QString& oldId : oldSegmentIds)
                {
                    if (nextSegmentIds.contains(oldId))
                        continue;
                    const QString storage = segmentStorageKey(turnId, oldId);
                    if (QWidget* widget = renderedSegmentWidgets.value(storage))
                    {
                        if (pendingViewportAnchor == widget
                            || (pendingViewportAnchor
                                && widget->isAncestorOf(pendingViewportAnchor)))
                            pendingViewportAnchor.clear();
                        itemLayout->removeWidget(widget);
                        widget->hide();
                        widget->deleteLater();
                        timelineShrank = true;
                        timelineGeometryChanged = true;
                    }
                    forgetRenderedSegment(storage);
                }

                for (qsizetype segmentIndex = 0;
                     segmentIndex < static_cast<qsizetype>(visibleTurn.segments.size());
                     ++segmentIndex)
                {
                    const TimelineSegment* segment = visibleTurn.segments.at(
                        static_cast<std::size_t>(segmentIndex));
                    const QString storage = segmentStorageKey(turnId, segment->id);
                    QStringList itemIds;
                    itemIds.reserve(static_cast<qsizetype>(segment->items.size()));
                    for (const sdk::ItemState* item : segment->items)
                    {
                        if (item)
                            itemIds.append(fromUtf8(item->id.value));
                    }
                    renderedSegmentItemIds.insert(storage, std::move(itemIds));
                    QWidget* oldWidget = renderedSegmentWidgets.value(storage);
                    if (oldWidget)
                        oldWidget->setProperty(
                            "timelineItemCount", timelineItemCount(*segment));
                    if (oldWidget && itemLayout->indexOf(oldWidget) != segmentIndex)
                    {
                        itemLayout->removeWidget(oldWidget);
                        itemLayout->insertWidget(
                            segmentIndex, oldWidget, 0, Qt::AlignTop);
                        timelineGeometryChanged = true;
                    }
                    const ConversationContentUpdates* segmentContentChanges = nullptr;
                    ConversationContentUpdates segmentContentStorage;
                    bool explicitlyAffected = false;
                    if (oldWidget && (exactContentOnly || exactContentApplied))
                    {
                        for (const ConversationContentUpdate& update : *exactContentChanges)
                        {
                            if (update.turnId != turnId)
                                continue;
                            const bool segmentContainsItem = std::any_of(
                                segment->items.cbegin(),
                                segment->items.cend(),
                                [&update](const sdk::ItemState* item)
                                {
                                    return item && update.itemId == fromUtf8(item->id.value);
                                });
                            if (segmentContainsItem)
                                segmentContentStorage.push_back(update);
                        }
                        explicitlyAffected = !segmentContentStorage.empty();
                        if (exactContentOnly && !explicitlyAffected)
                            continue;
                        if (exactContentOnly && explicitlyAffected)
                            segmentContentChanges = &segmentContentStorage;
                    }
                    const bool typedPlanAvailable = turn->plan.has_value();
                    const bool turnStreaming = turnStreamsMessages(*turn);
                    const QByteArray segmentKey = segmentPresentationKey(
                        state,
                        *segment,
                        typedPlanAvailable,
                        turnStreaming,
                        thread->fullyLoaded);
                    if (oldWidget && !explicitlyAffected
                        && renderedSegmentKeys.value(storage) == segmentKey)
                        continue;

                    const bool exactMessageAlreadyApplied =
                        exactContentApplied && explicitlyAffected
                        && segment->items.size() == 1
                        && segment->items.front()
                        && (segment->items.front()->kind.is(
                                frontend::ThreadItemKind::UserMessage)
                            || segment->items.front()->kind.is(
                                frontend::ThreadItemKind::AgentMessage));
                    if (oldWidget && exactMessageAlreadyApplied)
                    {
                        const sdk::ItemState* item = segment->items.front();
                        auto* status = oldWidget->findChild<QLabel*>(
                            QStringLiteral("conversationMessageStatus"));
                        auto* content = oldWidget->findChild<QWidget*>(
                            QStringLiteral("conversationMessageContent"));
                        auto* truncation = oldWidget->findChild<QLabel*>(
                            QStringLiteral("conversationMessageTruncation"));
                        if (status && content && truncation)
                        {
                            const bool user = item->kind.is(
                                frontend::ThreadItemKind::UserMessage);
                            const bool metadataGeometryChanged =
                                applyMessageMetadata(
                                    status,
                                    content,
                                    truncation,
                                    messagePresentationMetadata(
                                        *item, user, turnStreaming));
                            renderedSegmentKeys.insert(storage, segmentKey);
                            timelineShrank = timelineShrank
                                             || metadataGeometryChanged;
                            timelineGeometryChanged =
                                timelineGeometryChanged
                                || metadataGeometryChanged;
                            continue;
                        }
                    }

                    bool messageMayShrink = false;
                    bool messageGeometryChanged = false;
                    if (oldWidget
                        && updateTimelineMessageSegment(
                            oldWidget,
                            *segment,
                            turnStreaming,
                            &messageGeometryChanged,
                            &messageMayShrink))
                    {
                        renderedSegmentKeys.insert(storage, segmentKey);
                        timelineShrank = timelineShrank || messageMayShrink;
                        timelineGeometryChanged = timelineGeometryChanged
                                                  || messageGeometryChanged;
                        continue;
                    }

                    if (oldWidget
                        && updateTimelineActivitySegment(
                            oldWidget,
                            state,
                            *segment,
                            typedPlanAvailable,
                            segmentContentChanges,
                            &messageGeometryChanged,
                            &messageMayShrink,
                            [this] { activityLayoutChanged(); }))
                    {
                        renderedSegmentKeys.insert(storage, segmentKey);
                        timelineShrank = timelineShrank || messageMayShrink;
                        timelineGeometryChanged = timelineGeometryChanged
                                                  || messageGeometryChanged;
                        continue;
                    }

                    const ActivityExpansionState expansion = activityExpansionState(oldWidget);
                    QWidget* newWidget = timelineSegmentWidget(
                        state,
                        *segment,
                        typedPlanAvailable,
                        turnStreaming,
                        thread->fullyLoaded,
                        expansion,
                        [this] { activityLayoutChanged(); });
                    newWidget->setProperty("turnId", turnId);
                    if (oldWidget)
                    {
                        const bool replacesAnchor = pendingViewportAnchor == oldWidget
                                                    || (pendingViewportAnchor
                                                        && oldWidget->isAncestorOf(pendingViewportAnchor));
                        itemLayout->removeWidget(oldWidget);
                        oldWidget->hide();
                        oldWidget->deleteLater();
                        forgetRenderedActivityRows(storage);
                        itemLayout->insertWidget(
                            segmentIndex, newWidget, 0, Qt::AlignTop);
                        if (replacesAnchor)
                            pendingViewportAnchor.clear();
                        timelineShrank = true;
                        timelineGeometryChanged = true;
                    }
                    else
                    {
                        itemLayout->insertWidget(
                            segmentIndex, newWidget, 0, Qt::AlignTop);
                        timelineGeometryChanged = true;
                    }
                    renderedSegmentWidgets.insert(storage, newWidget);
                    renderedSegmentKeys.insert(storage, segmentKey);
                }
                renderedSegmentIds.insert(turnId, segmentIds);
            }
            renderedTurnIds = visibleTurnIds;
        }
    }

    if (timelineGeometryChanged)
        scheduleTimelineLayout(previousScroll, followLatest, threadChanged, timelineShrank);
    else if (followLatest)
        scrollBar->setValue(scrollBar->maximum());
}

bool ConversationWidget::updateExactMessageContent(
    const sdk::State& state,
    const QString& threadId,
    const ConversationContentUpdates& exactContentChanges)
{
    if (threadId.isEmpty() || renderedThreadId != threadId || renderedNewThreadDraft
        || exactContentChanges.empty())
        return false;
    if (shouldFreezePresentation(threadId, false))
    {
        markPresentationDeferred();
        return true;
    }
    auto* scrollBar = scrollArea->verticalScrollBar();
    const int previousScroll = scrollBar->value();
    const bool followLatest = scrollBar->maximum() - previousScroll <= 72
                              || followingLatest;
    if (!followLatest && !layoutSettleTimer->isActive())
        captureTimelineAnchor();
    else if (followLatest)
        pendingViewportAnchor.clear();

    bool timelineShrank = false;
    bool geometryChanged = false;
    for (const ConversationContentUpdate& update : exactContentChanges)
    {
        if (!update.append)
            return false;
        if (!renderedTurnIds.contains(update.turnId))
            continue;

        const QString messageSegmentId = QStringLiteral("message:") + update.itemId;
        const QString messageStorage = segmentStorageKey(update.turnId, messageSegmentId);
        QWidget* messageWidget = renderedSegmentWidgets.value(messageStorage);
        bool contentMayShrink = false;
        bool contentGeometryChanged = false;
        QString affectedStorage;
        if (messageWidget)
        {
            if (messageWidget->property("messageUser").toBool()
                || update.channel != sdk::ItemContentChannel::AgentText)
                return false;
            const ai::openai::codex::typed::ThreadId typedThreadId{
                threadId.toStdString()};
            const ai::openai::codex::typed::TurnId typedTurnId{
                update.turnId.toStdString()};
            const ai::openai::codex::typed::ItemId typedItemId{
                update.itemId.toStdString()};
            const auto expectedBytes = exactAppendResultBytes(*update.append);
            const auto descriptor = state.itemContentDescriptor(
                typedThreadId,
                typedTurnId,
                typedItemId,
                update.channel);
            const auto* turn = state.turn(typedThreadId, typedTurnId);
            const auto* item = state.item(typedThreadId, typedTurnId, typedItemId);
            if (!expectedBytes || !descriptor || !descriptor->present
                || descriptor->retainedUtf8Bytes != *expectedBytes
                || !turn || !item
                || !item->kind.is(frontend::ThreadItemKind::AgentMessage))
                return false;
            auto* content = messageWidget->findChild<QWidget*>(
                QStringLiteral("conversationMessageContent"));
            if (!content)
                return false;
            const bool emptyCanonicalPlaceholder =
                content->property("kind").toString() == QStringLiteral("meta")
                && update.append->baseContentBytes == 0;
            const std::uint64_t currentUtf8Bytes = emptyCanonicalPlaceholder
                                                       ? 0
                                                       : messageContentUtf8Bytes(content);
            if (currentUtf8Bytes != update.append->baseContentBytes
                || update.append->discardPrefixBytes
                       > update.append->baseContentBytes)
                return false;

            auto* contentLayout = qobject_cast<QVBoxLayout*>(
                content->parentWidget()->layout());
            if (!contentLayout)
                return false;
            QWidget* const previousContent = content;
            const bool authoritativeStreaming =
                turnStreamsMessages(*turn)
                || streamingMessageStatus(itemStatus(*item));
            if (!authoritativeStreaming)
            {
                auto* status = messageWidget->findChild<QLabel*>(
                    QStringLiteral("conversationMessageStatus"));
                auto* truncation = messageWidget->findChild<QLabel*>(
                    QStringLiteral("conversationMessageTruncation"));
                if (!status || !truncation)
                    return false;
                const MessagePresentation presentation = messagePresentation(
                    *item, false, false);
                content = ensureMessageContentWidget(
                    contentLayout,
                    content,
                    presentation.content,
                    false);
                const bool rendererChanged = previousContent != content;
                contentGeometryChanged = rendererChanged
                                         || applyMessagePresentation(
                                             status,
                                             content,
                                             truncation,
                                             presentation);
                // Markdown reparsing can reduce its preferred height even
                // when the raw source only grew (for example, when this
                // append closes an emphasis span or fenced block).
                contentMayShrink = true;
            }
            else
            {
                bool mutationGeometryChanged = false;
                if (emptyCanonicalPlaceholder)
                {
                    // The visible copy is explanatory UI text, not canonical
                    // message content. Reset that small placeholder directly
                    // so the first real delta still enters the O(delta) path.
                    if (dynamic_cast<StreamingMessageView*>(content))
                        mutationGeometryChanged = setMessageContentText(
                            content, QString{});
                    else
                        content = ensureMessageContentWidget(
                            contentLayout, content, QString{}, true);
                    if (content->property("kind").toString()
                        != QStringLiteral("body"))
                    {
                        content->setProperty("kind", QStringLiteral("body"));
                        content->style()->unpolish(content);
                        content->style()->polish(content);
                    }
                }
                else if (!dynamic_cast<StreamingMessageView*>(content)
                         && !qobject_cast<QPlainTextEdit*>(content))
                {
                    content = ensureMessageContentWidget(
                        contentLayout,
                        content,
                        messageContentText(content),
                        true);
                }

                const auto applied = appendMessageContent(
                    content,
                    update.append->baseContentBytes,
                    update.append->discardPrefixBytes,
                    update.append->delta);
                if (!applied)
                    return false;
                mutationGeometryChanged = mutationGeometryChanged || *applied;

                // Select the renderer from the post-append size. In
                // particular, the append that crosses 64 KiB performs the
                // one required source materialization immediately; it does
                // not leave the small-message renderer alive until a later
                // event happens to arrive.
                if (!messageContentWidgetMatches(
                        content, messageContentSize(content), true))
                {
                    content = ensureMessageContentWidget(
                        contentLayout,
                        content,
                        messageContentText(content),
                        true);
                }
                contentGeometryChanged = previousContent != content
                                         || mutationGeometryChanged;
                contentMayShrink = update.append->discardPrefixBytes
                                   > update.append->deltaUtf8Bytes;
            }
            affectedStorage = messageStorage;
        }
        else
        {
            const QString activityIdentity = update.turnId
                                             + QChar::Null
                                             + update.itemId;
            QWidget* activityRow = renderedActivityRows.value(activityIdentity);
            if (!activityRow) {
                for (const QString& segmentId : renderedSegmentIds.value(update.turnId))
                {
                    const QString storage = segmentStorageKey(update.turnId, segmentId);
                    QWidget* candidate = renderedSegmentWidgets.value(storage);
                    if (!candidate)
                        continue;
                    const auto rows = candidate->findChildren<QWidget*>(
                        QStringLiteral("conversationActivityRow"));
                    const auto found = std::find_if(
                        rows.cbegin(), rows.cend(), [&update](const QWidget* row) {
                            return row->property("itemId").toString() == update.itemId;
                        });
                    if (found == rows.cend())
                        continue;
                    activityRow = *found;
                    affectedStorage = storage;
                    renderedActivityRows.insert(activityIdentity, activityRow);
                    renderedActivityRowSegments.insert(activityIdentity, storage);
                    break;
                }
            } else {
                affectedStorage = renderedActivityRowSegments.value(activityIdentity);
            }
            if (!activityRow)
                continue;
            if (!applyExactActivityAppend(
                    state,
                    ai::openai::codex::typed::ThreadId{threadId.toStdString()},
                    activityRow,
                    update,
                    &contentGeometryChanged, &contentMayShrink))
                return false;
        }
        renderedSegmentKeys.remove(affectedStorage);
        timelineShrank = timelineShrank || contentMayShrink;
        geometryChanged = geometryChanged || contentGeometryChanged;
    }
    if (geometryChanged)
    {
        // Document and layout repaints are queued. Hide the intermediate old
        // extent until the existing settle pass has resized and pinned the
        // conversation, then expose one final frame.
        if (followLatest && scrollArea->viewport()->updatesEnabled())
            scrollArea->viewport()->setUpdatesEnabled(false);
        scheduleTimelineLayout(previousScroll, followLatest, false, timelineShrank);
    }
    else if (followLatest)
        scrollBar->setValue(scrollBar->maximum());
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
    if (!followLatest && !layoutSettleTimer->isActive())
        captureTimelineAnchor();
    else if (followLatest)
        pendingViewportAnchor.clear();
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
        settleThreadSwitchLayout(pinLatestGeneration, 1);
        return;
    }

    synchronizeTimelineHeight(timelineShrank);
    scrollArea->widget()->layout()->activate();

    auto* bar = scrollArea->verticalScrollBar();
    if (followLatest)
    {
        pendingViewportAnchor.clear();
        followingLatest = !renderedThreadId.isEmpty();
        bar->setValue(bar->maximum());
        if (!scrollArea->viewport()->updatesEnabled())
            scrollArea->viewport()->setUpdatesEnabled(true);
        return;
    }

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
        scrollArea->viewport()->setUpdatesEnabled(true);
    followingLatest = false;
}

void ConversationWidget::settleThreadSwitchLayout(std::uint64_t generation, int remainingPasses)
{
    // An older generation must leave ownership to the newer settle pass. If
    // the current generation was cancelled, however, no later callback owns
    // the viewport freeze and updates must be restored here.
    if (generation != pinLatestGeneration)
        return;
    if (!pinLatestDuringLayout) {
        scrollArea->viewport()->setUpdatesEnabled(true);
        return;
    }
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
    followingLatest = true;
    pinLatestDuringLayout = false;
    pendingViewportAnchor.clear();
    scrollArea->viewport()->setUpdatesEnabled(true);
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

const QList<AttachmentInfo>& ConversationWidget::attachments() const noexcept
{
    return upcomingTurnDock->attachments();
}

QString ConversationWidget::attachmentWorkspace() const
{
    return upcomingTurnDock->attachmentWorkspace();
}

void ConversationWidget::clearAttachmentsIfUnchanged(
    const QList<AttachmentInfo>& submittedAttachments)
{
    upcomingTurnDock->clearAttachmentsIfUnchanged(submittedAttachments);
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
