// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H

#include "codex/middle/ConversationPresentation.h"
#include "codex/middle/MiddleTypes.h"

#include <QFrame>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextEdit>

#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QPaintEvent;
class QKeyEvent;
class QResizeEvent;
class QTimer;
class QTextDocument;
class QVBoxLayout;
class QWheelEvent;

namespace codexui::codex::middle {

enum class PresentationImpact { None, PaintOnly, GeometryChanged };

class MarkdownTextView final : public QTextBrowser {
  Q_OBJECT

public:
  explicit MarkdownTextView(
      const QString &markdown,
      std::shared_ptr<QTextDocument> preparedDocument = {},
      int initialWidth = 0,
      QWidget *parent = nullptr,
      bool preserveSoftLineBreaks = false);
  ~MarkdownTextView() override;

  bool setContent(const QString &markdown);
  [[nodiscard]] const QString &markdownSource() const noexcept;
  [[nodiscard]] std::shared_ptr<QTextDocument> sharedDocument() const;
  [[nodiscard]] bool hasSelectedText() const;
  [[nodiscard]] int selectionStart() const;
  [[nodiscard]] QString selectedText() const;
  void setSelection(int start, int length);
  [[nodiscard]] int heightForWidth(int width) const override;
  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

protected:
  void keyPressEvent(QKeyEvent *event) override;

private:
  void configureDocument();
  void refreshPreferredHeight(int documentWidth) const;

  std::shared_ptr<QTextDocument> document_;
  QString markdown_;
  QString renderedMarkdown_;
  presentation::MarkdownTailState markdownTail_;
  bool preserveSoftLineBreaks_ = false;
  mutable int preferredDocumentWidth_ = 0;
  mutable int preferredHeight_ = 0;
};

class ContentSizedTextView : public QTextEdit {
public:
  explicit ContentSizedTextView(int maximumContentHeight,
                                QWidget *parent = nullptr);

  bool setContent(const QString &content);
  [[nodiscard]] bool retainsWheelGesture(QWheelEvent *event);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  void wheelEvent(QWheelEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  [[nodiscard]] bool measureAtCurrentWidth(bool notifyParent);
  [[nodiscard]] bool contentHeightCapped() const noexcept;

private:
  [[nodiscard]] bool setPreferredContentHeight(int height, bool notifyParent);
  int preferredHeight_ = 0;
  bool pinScrollToStart_ = false;
  bool wheelGestureActive_ = false;
  bool wheelGestureDecided_ = false;
  bool wheelGestureOwned_ = false;
};

class CommandOutputView final : public QTextEdit {
  Q_OBJECT

public:
  struct ScrollState {
    bool followsLatest = true;
    int value = 0;

    friend bool operator==(const ScrollState &, const ScrollState &) = default;
  };

  explicit CommandOutputView(const QString &output, QWidget *parent = nullptr);

  [[nodiscard]] ScrollState scrollState() const;
  [[nodiscard]] bool followsLatest() const noexcept;
  [[nodiscard]] bool isHeightCapped() const noexcept;
  [[nodiscard]] bool retainsWheelGesture(QWheelEvent *event);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

  // Returns false for a true no-op. Programmatic document/range changes do
  // not alter the user's follow/paused choice.
  bool setOutput(const QString &output);
  void restoreScrollState(const ScrollState &state);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  [[nodiscard]] bool measureAtCurrentWidth(bool notifyParent);
  [[nodiscard]] bool setPreferredContentHeight(int height, bool notifyParent);
  void settleScroll();
  void scheduleScrollSettlement();
  [[nodiscard]] bool isAtBottom() const;
  [[nodiscard]] bool outputRequiresMaximumHeight(const QString &output) const;

  bool followsLatest_ = true;
  bool programmaticScroll_ = false;
  bool settlingScroll_ = false;
  bool scrollSettlementPending_ = false;
  bool userScrollActive_ = false;
  bool wheelGestureActive_ = false;
  bool wheelGestureDecided_ = false;
  bool wheelGestureOwned_ = false;
  int preferredHeight_ = 0;
  int preservedScrollValue_ = 0;
  QString currentOutput_;
};

class ConversationCard : public QFrame {
  Q_OBJECT

public:
  explicit ConversationCard(const VisibleCardData &data,
                            QWidget *parent = nullptr,
                            bool commandInitiallyCollapsed = true,
                            bool imageInitiallyCollapsed = true,
                            bool fileChangesInitiallyCollapsed = true,
                            int initialWidth = 0,
                            std::shared_ptr<QTextDocument> markdownDocument = {},
                            std::optional<bool> collapsedOverride = {});
  ~ConversationCard() override;

  [[nodiscard]] CardKind cardKind() const noexcept;
  [[nodiscard]] const VisibleCardData &data() const noexcept;
  [[nodiscard]] std::shared_ptr<QTextDocument> markdownDocument() const;
  [[nodiscard]] bool isCollapsed() const noexcept;
  void setCollapsed(bool collapsed);
  bool setAuthoritativeTurnActive(bool active);
  // Select the established nested-card presentation for a child, or clear it
  // when the card becomes a turn root or a standalone activity.
  void setNestedPresentation(bool nested);
  // In a virtualized turn the view paints the continuous outer You surface;
  // the root card keeps only its content and interaction geometry.
  void setVirtualTurnRootPresentation(bool fragmented);
  // ConversationView uses this to pause local feedback timers while a card is
  // not painted.
  void setViewportVisible(bool visible);
  [[nodiscard]] std::optional<CommandOutputView::ScrollState>
  commandOutputScrollState() const;
  void
  restoreCommandOutputScrollState(const CommandOutputView::ScrollState &state);

  [[nodiscard]] bool canApply(const VisibleCardData &data) const noexcept;

  // A key identifies the persistent widget. apply() updates matching card
  // kinds in place and also performs the one supported semantic transition
  // from an admitted local prompt to its authoritative user message.
  bool apply(const VisibleCardData &data);
  PresentationImpact applyPresentation(const VisibleCardData &data);

signals:
  void foldRequested(bool collapsed);
  void recoveryRequested();

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] ConversationCard *
createConversationCard(const VisibleCardData &data, QWidget *parent = nullptr,
                       bool commandInitiallyCollapsed = true,
                       bool imageInitiallyCollapsed = true,
                       bool fileChangesInitiallyCollapsed = true,
                       int initialWidth = 0,
                       std::shared_ptr<QTextDocument> markdownDocument = {},
                       std::optional<bool> collapsedOverride = {});

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H
