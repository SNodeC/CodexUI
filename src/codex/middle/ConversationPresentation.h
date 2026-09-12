// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H

#include "codex/middle/MiddleTypes.h"

#include <QRect>
#include <QString>
#include <QStringView>

#include <string_view>

class QTextDocument;

namespace codexui::codex::middle::presentation {

struct CardHeaderMetrics final {
  // The delegate-painted Codex Update header is the established optical
  // reference for both passive and materialized cards.
  static constexpr int LineHeight = 24;
  static constexpr int HorizontalInset = 12;
  static constexpr int StatusWidth = 145;
  static constexpr int CopyInkLeftFromRight = 43;
  static constexpr int StatusToCopyInkGap = 17;
  static constexpr int RichStatusTrailingMargin = 12;
  static constexpr int CopyControlWidth = 16;
  static constexpr int DisclosureControlWidth = 14;
  static constexpr int CopyDisclosureSpacing = 4;
};

[[nodiscard]] QRect cardDisclosureIndicator(const QRect &control,
                                            bool expanded);

struct MarkdownTailState {
  qsizetype sourceOffset = -1;
  int documentPosition = -1;

  [[nodiscard]] bool valid() const noexcept {
    return sourceOffset >= 0 && documentPosition >= 0;
  }
};

// Pure display-value helpers shared by the passive delegate and the rich card
// editor. They own no state and do not decide which renderer a row uses.
[[nodiscard]] QString statusLabel(std::string_view status);
// User-authored prompt newlines are intentional visual line breaks. Preserve
// them in the Markdown presentation without changing the canonical source
// retained for copy or protocol reconciliation.
[[nodiscard]] QString userMessageMarkdown(QStringView source);
[[nodiscard]] QString planMarkdown(const PlanData &plan);
[[nodiscard]] QString agentMetadata(const AgentActivityData &activity);
[[nodiscard]] QString fileChangesText(const FileChangesData &changes);
[[nodiscard]] QString genericActivityTitle(const GenericActivityData &activity);
[[nodiscard]] QString
boundedGenericActivityDetail(const GenericActivityData &activity);

// A streamed Markdown document has one mutable trailing block while all
// preceding blocks are already final. These helpers preserve the Qt Markdown
// dialect while replacing only that tail when it is independently reparsable.
void replaceMarkdownDocument(QTextDocument &document, const QString &markdown,
                             MarkdownTailState &tailState);
[[nodiscard]] MarkdownTailState
markdownTailState(const QTextDocument &document, QStringView markdown);
[[nodiscard]] bool appendMarkdownDocument(QTextDocument &document,
                                          QStringView previous,
                                          QStringView next,
                                          MarkdownTailState &tailState);

} // namespace codexui::codex::middle::presentation

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H
