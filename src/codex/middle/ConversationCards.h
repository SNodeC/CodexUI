// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H

#include "codex/middle/MiddleTypes.h"

#include <QFrame>
#include <QTextEdit>

#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QVBoxLayout;
class QWheelEvent;

namespace codexui::codex::middle {

class ContentSizedTextView : public QTextEdit {
public:
  explicit ContentSizedTextView(int maximumContentHeight,
                                QWidget *parent = nullptr);

  bool setContent(const QString &content);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  void resizeEvent(QResizeEvent *event) override;
  void measureAtCurrentWidth(bool notifyParent);

private:
  int preferredHeight_ = 0;
};

class CommandOutputView final : public ContentSizedTextView {
public:
  struct ScrollState {
    bool followsLatest = true;
    int value = 0;

    friend bool operator==(const ScrollState &, const ScrollState &) = default;
  };

  explicit CommandOutputView(const QString &output, QWidget *parent = nullptr);

  [[nodiscard]] ScrollState scrollState() const;
  [[nodiscard]] bool followsLatest() const noexcept;

  // Returns false for a true no-op. Programmatic document/range changes do
  // not alter the user's follow/paused choice.
  bool setOutput(const QString &output);
  void restoreScrollState(const ScrollState &state);

protected:
  void wheelEvent(QWheelEvent *event) override;

private:
  void settleScroll();
  [[nodiscard]] bool isAtBottom() const;

  bool followsLatest_ = true;
  bool programmaticScroll_ = false;
  bool settlingScroll_ = false;
  int preservedScrollValue_ = 0;
  QString currentOutput_;
};

class ConversationCard : public QFrame {
  Q_OBJECT

public:
  explicit ConversationCard(const VisibleCardData &data,
                            QWidget *parent = nullptr,
                            bool commandInitiallyCollapsed = true,
                            bool imageInitiallyCollapsed = true);
  ~ConversationCard() override;

  [[nodiscard]] CardKind cardKind() const noexcept;
  [[nodiscard]] const VisibleCardData &data() const noexcept;
  [[nodiscard]] bool isCollapsed() const noexcept;
  void setCollapsed(bool collapsed);
  void setNestedCards(const std::vector<ConversationCard *> &cards);
  [[nodiscard]] std::optional<CommandOutputView::ScrollState>
  commandOutputScrollState() const;
  void
  restoreCommandOutputScrollState(const CommandOutputView::ScrollState &state);

  [[nodiscard]] bool canApply(const VisibleCardData &data) const noexcept;

  // A key identifies the persistent widget. apply() updates matching card
  // kinds in place and also performs the one supported semantic transition
  // from an admitted local prompt to its authoritative user message.
  bool apply(const VisibleCardData &data);

signals:
  void foldRequested(bool collapsed);

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] ConversationCard *
createConversationCard(const VisibleCardData &data, QWidget *parent = nullptr,
                       bool commandInitiallyCollapsed = true,
                       bool imageInitiallyCollapsed = true);

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H
