// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationPresentation.h"

#include "codex/UiStatus.h"
#include "codex/ui/UiStyle.h"

#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cstddef>

namespace codexui::codex::middle::presentation {
namespace {

constexpr qsizetype MaximumGenericActivityCharacters = 4096;
constexpr std::size_t MaximumGenericActivityUtf8Bytes =
    static_cast<std::size_t>(MaximumGenericActivityCharacters) * 4;

constexpr QTextDocument::MarkdownFeatures MarkdownFeatures =
    QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
    QTextDocument::MarkdownNoHTML;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QStringList textList(const std::vector<std::string> &values) {
  QStringList result;
  result.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string &value : values)
    result.push_back(text(value));
  return result;
}

QString displayChangeKind(std::string_view kind) {
  if (kind.empty())
    return QStringLiteral("Changed");
  return UiStyle::humanizeLabel(text(kind));
}

qsizetype lastSimpleMarkdownParagraphStart(QStringView source) {
  qsizetype paragraphStart = 0;
  qsizetype candidateStart = 0;
  qsizetype lineStart = 0;
  while (lineStart <= source.size()) {
    qsizetype lineEnd = source.indexOf(QLatin1Char('\n'), lineStart);
    if (lineEnd < 0)
      lineEnd = source.size();
    QStringView line = source.sliced(lineStart, lineEnd - lineStart);
    if (!line.isEmpty() && line.back() == QLatin1Char('\r'))
      line.chop(1);
    bool blank = true;
    for (QChar character : line) {
      if (!character.isSpace()) {
        blank = false;
        break;
      }
    }
    if (blank) {
      candidateStart = std::min(source.size(), lineEnd + 1);
    } else if (candidateStart > paragraphStart) {
      paragraphStart = candidateStart;
    }
    if (lineEnd == source.size())
      break;
    lineStart = lineEnd + 1;
  }
  return paragraphStart;
}

bool simpleMarkdownParagraphs(QStringView source) {
  qsizetype lineStart = 0;
  while (lineStart <= source.size()) {
    qsizetype lineEnd = source.indexOf(QLatin1Char('\n'), lineStart);
    if (lineEnd < 0)
      lineEnd = source.size();
    QStringView line = source.sliced(lineStart, lineEnd - lineStart);
    if (!line.isEmpty() && line.back() == QLatin1Char('\r'))
      line.chop(1);
    qsizetype indentation = 0;
    while (indentation < line.size() &&
           line.at(indentation) == QLatin1Char(' '))
      ++indentation;
    const QStringView content = line.sliced(indentation);
    const bool heading =
        content.startsWith(QLatin1Char('#')) &&
        (content.size() == 1 || content.at(1).isSpace());
    const bool quote = content.startsWith(QLatin1Char('>'));
    const bool fence = content.startsWith(QLatin1StringView("```")) ||
                       content.startsWith(QLatin1StringView("~~~"));
    const bool unorderedList =
        content.size() >= 2 &&
        (content.at(0) == QLatin1Char('-') ||
         content.at(0) == QLatin1Char('*') ||
         content.at(0) == QLatin1Char('+')) &&
        content.at(1).isSpace();
    qsizetype digits = 0;
    while (digits < content.size() && content.at(digits).isDigit())
      ++digits;
    const bool orderedList =
        digits > 0 && digits + 1 < content.size() &&
        (content.at(digits) == QLatin1Char('.') ||
         content.at(digits) == QLatin1Char(')')) &&
        content.at(digits + 1).isSpace();
    const bool referenceDefinition = content.startsWith(QLatin1Char('['));
    const bool table = content.contains(QLatin1Char('|'));
    if (indentation >= 4 || heading || quote || fence || unorderedList ||
        orderedList || referenceDefinition || table)
      return false;
    if (lineEnd == source.size())
      break;
    lineStart = lineEnd + 1;
  }
  return true;
}

} // namespace

QString statusLabel(std::string_view status) {
  return text(codexui::codex::displayStatus(status));
}

QString planMarkdown(const PlanData &plan) {
  if (!plan.legacyText.empty())
    return text(plan.legacyText);
  QStringList rows;
  if (!plan.explanation.empty())
    rows << text(plan.explanation);
  if (!plan.steps.empty() && !rows.empty())
    rows << QString{};
  for (const PlanStepData &step : plan.steps) {
    const QString marker = step.status == "completed"    ? QStringLiteral("✓")
                           : step.status == "inProgress" ? QStringLiteral("◉")
                                                         : QStringLiteral("○");
    rows << QStringLiteral("%1 %2  ").arg(marker, text(step.text));
  }
  return rows.join(QLatin1Char('\n'));
}

QString agentMetadata(const AgentActivityData &activity) {
  QStringList metadata;
  if (!activity.tool.empty())
    metadata << text(activity.tool);
  if (activity.status.empty() && !activity.kind.empty())
    metadata << statusLabel(activity.kind);
  if (!activity.receivers.empty())
    metadata << textList(activity.receivers).join(QStringLiteral(", "));
  if (!activity.model.empty())
    metadata << text(activity.model);
  if (!activity.reasoningEffort.empty())
    metadata << text(activity.reasoningEffort);
  if (!activity.childThreadId.empty())
    metadata << QStringLiteral("thread %1").arg(text(activity.childThreadId));
  if (!activity.agentPath.empty())
    metadata << text(activity.agentPath);
  if (!activity.senderThreadId.empty())
    metadata << QStringLiteral("sender %1").arg(text(activity.senderThreadId));
  return metadata.join(QStringLiteral("  |  "));
}

QString fileChangesText(const FileChangesData &changes) {
  QStringList rows;
  for (const FileChangeData &change : changes.changes) {
    if (change.path.empty())
      continue;
    QString row = QStringLiteral("%1  ·  %2")
                      .arg(text(change.path), displayChangeKind(change.kind));
    if (change.additions && change.deletions)
      row += QStringLiteral("  +%1 −%2")
                 .arg(*change.additions)
                 .arg(*change.deletions);
    rows << row;
  }
  return rows.join(QLatin1Char('\n'));
}

QString genericActivityTitle(const GenericActivityData &activity) {
  return activity.type.empty() ? QStringLiteral("Activity")
                               : UiStyle::humanizeLabel(text(activity.type));
}

QString boundedGenericActivityDetail(const GenericActivityData &activity) {
  const std::size_t byteCount = std::min(
      activity.displayDetail.size(), MaximumGenericActivityUtf8Bytes);
  QString rendered = QString::fromUtf8(
      activity.displayDetail.data(), static_cast<qsizetype>(byteCount));
  if (byteCount == activity.displayDetail.size() &&
      rendered.size() <= MaximumGenericActivityCharacters)
    return rendered;
  rendered.truncate(MaximumGenericActivityCharacters);
  return rendered + QStringLiteral("\n\n[Activity details truncated]");
}

MarkdownTailState markdownTailState(const QTextDocument &document,
                                    QStringView markdown) {
  if (markdown.isEmpty())
    return {};
  const qsizetype tail = lastSimpleMarkdownParagraphStart(markdown);
  if (!simpleMarkdownParagraphs(markdown.sliced(tail)))
    return {};
  const QTextBlock lastBlock = document.lastBlock();
  return lastBlock.isValid() ? MarkdownTailState{tail, lastBlock.position()}
                             : MarkdownTailState{};
}

void replaceMarkdownDocument(QTextDocument &document, const QString &markdown,
                             MarkdownTailState &tailState) {
  document.setMarkdown(markdown, MarkdownFeatures);
  tailState = markdownTailState(document, QStringView(markdown));
}

bool appendMarkdownDocument(QTextDocument &document, QStringView previous,
                            QStringView next,
                            MarkdownTailState &tailState) {
  if (!tailState.valid() || !next.startsWith(previous))
    return false;
  const QStringView reparsedTail = next.sliced(tailState.sourceOffset);
  if (!simpleMarkdownParagraphs(reparsedTail))
    return false;
  QTextCursor cursor(&document);
  cursor.setPosition(tailState.documentPosition);
  cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  cursor.insertMarkdown(reparsedTail.toString(), MarkdownFeatures);
  tailState = markdownTailState(document, next);
  return true;
}

} // namespace codexui::codex::middle::presentation
