// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H

#include "codex/middle/MiddleTypes.h"

#include <QRect>
#include <QString>
#include <QStringView>
#include <QTextBrowser>
#include <QToolButton>

#include <string_view>

class QFocusEvent;
class QEvent;
class QLabel;
class QPaintEvent;
class QTextDocument;
class QTimer;
class QVariantAnimation;
class QWidget;

namespace codexui::codex::middle {

namespace presentation {

struct MarkdownTailState {
  qsizetype sourceOffset = -1;
  int documentPosition = -1;

  [[nodiscard]] bool valid() const noexcept {
    return sourceOffset >= 0 && documentPosition >= 0;
  }
};

} // namespace presentation

// The one native Markdown widget used by every surface that presents Markdown.
// It retains the canonical source for copy/reconciliation and owns the sole Qt
// document update and height-for-width policy.
class MarkdownTextView final : public QTextBrowser {
  Q_OBJECT

public:
  explicit MarkdownTextView(const QString &markdown, int initialWidth = 0,
                            QWidget *parent = nullptr,
                            bool preserveSoftLineBreaks = false);

  bool setContent(const QString &markdown);
  bool setPreparedContent(const QString &markdown, const QString &html);
  void invalidateGeometryEnvironment();
  [[nodiscard]] const QString &markdownSource() const noexcept;
  [[nodiscard]] presentation::MarkdownTailState markdownTailState() const;
  [[nodiscard]] int heightForWidth(int width) const override;
  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

private:
  void configureDocument();
  void refreshPreferredHeight(int documentWidth) const;

  QString markdown_;
  QString renderedMarkdown_;
  presentation::MarkdownTailState markdownTail_;
  bool preserveSoftLineBreaks_ = false;
  mutable bool preparedLayoutDisabled_ = false;
  mutable int preferredDocumentWidth_ = 0;
  mutable int preferredHeight_ = 0;
};

namespace presentation {

[[nodiscard]] QString prepareMarkdownHtml(const QString &markdown);

struct CardHeaderMetrics final {
  // Keep the Codex Update header aligned with the card header controls.
  static constexpr int LineHeight = 24;
  static constexpr int RichStatusTrailingMargin = 12;
  static constexpr int CopyControlWidth = 16;
  static constexpr int DisclosureControlWidth = 14;
  static constexpr int CopyDisclosureSpacing = 4;
};

class CopyButton final : public QToolButton {
public:
  explicit CopyButton(QString accessibleName, QWidget *parent = nullptr);
  void copyText(const QString &text, bool markdown);

protected:
  void changeEvent(QEvent *event) override;
  void focusInEvent(QFocusEvent *event) override;
  void focusOutEvent(QFocusEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  void showCopiedFeedback();
  void finishFeedback();

  QVariantAnimation *morph_ = nullptr;
  QTimer *hold_ = nullptr;
  qreal morphProgress_ = 0.0;
  bool returningToCopy_ = false;
  bool feedbackActive_ = false;
  bool keyboardFocusVisible_ = false;
};

class DisclosureButton final : public QToolButton {
  Q_OBJECT

public:
  DisclosureButton(QString expandAccessibleName, QString collapseAccessibleName,
                   QWidget *parent = nullptr);

  void setExpanded(bool expanded);
  [[nodiscard]] bool isExpanded() const noexcept { return expanded_; }

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  QString expandAccessibleName_;
  QString collapseAccessibleName_;
  bool expanded_ = false;
};

// Pure display-value helpers shared by conversation presentation surfaces.
// They own no state.
[[nodiscard]] QString statusLabel(const UiStatus &status);
// Apply the shared semantic tone without QObject-property style invalidation.
// Empty status is represented by an empty, hidden label on every surface.
void setLabelTone(QLabel &label, std::string_view tone);
void applyStatusLabel(QLabel &label, const UiStatus &status);
// Qt 6.6 emits accessibility events even for identical property assignments.
// Keep the supported-version contract silent for semantic no-ops.
void setAccessibleNameIfChanged(QWidget &widget, QString name);
void setAccessibleDescriptionIfChanged(QWidget &widget, QString description);
// Announce a completed semantic action through the strongest API provided by
// the supported Qt version. The widget remains the sole accessible object.
void announce(QWidget &widget, const QString &message);
// Single newlines are line breaks; blank lines separate paragraphs. Native
// prose uses Qt's in-paragraph separator instead of its Markdown hard breaks.
// Canonical source retained for Copy and protocol reconciliation is unchanged.
[[nodiscard]] QString userMessageMarkdown(
    QStringView source, bool nativeLineBreaks = false);
[[nodiscard]] QString planMarkdown(const PlanData &plan);
[[nodiscard]] QString agentMetadata(const AgentActivityData &activity,
                                    const UiStatus &status);
[[nodiscard]] QString fileChangesText(const FileChangesData &changes);
[[nodiscard]] QString genericActivityTitle(const GenericActivityData &activity);

// A streamed Markdown document has one mutable trailing block while all
// preceding blocks are already final. These helpers preserve the Qt Markdown
// dialect while replacing only that tail when it is independently reparsable.
void replaceMarkdownDocument(QTextDocument &document, const QString &markdown,
                             MarkdownTailState &tailState);
[[nodiscard]] MarkdownTailState markdownTailState(const QTextDocument &document,
                                                  QStringView markdown);
[[nodiscard]] bool
markdownAppendTailIsIndependent(QStringView next,
                                const MarkdownTailState &tailState);
[[nodiscard]] bool appendMarkdownDocument(QTextDocument &document,
                                          QStringView previous,
                                          QStringView next,
                                          MarkdownTailState &tailState);

} // namespace presentation
} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H
