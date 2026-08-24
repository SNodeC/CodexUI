// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_GREENFIELD_CODEX_MIDDLE_CONVERSATIONCARDS_H
#define CODEXUI_GREENFIELD_CODEX_MIDDLE_CONVERSATIONCARDS_H

#include "codex/middle/MiddleTypes.h"

#include <QFrame>
#include <QPlainTextEdit>

#include <memory>
#include <optional>

class QLabel;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QVBoxLayout;
class QWheelEvent;

namespace codexui::codex::middle {

class CommandOutputView final : public QPlainTextEdit {
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

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  void measureAtCurrentWidth(bool notifyParent);
  void settleScroll();
  [[nodiscard]] bool isAtBottom() const;

  bool followsLatest_ = true;
  bool programmaticScroll_ = false;
  bool settlingScroll_ = false;
  int preservedScrollValue_ = 0;
  int preferredHeight_ = 0;
};

class ConversationCard : public QFrame {
public:
  explicit ConversationCard(const VisibleCardData &data,
                            QWidget *parent = nullptr);
  ~ConversationCard() override;

  [[nodiscard]] CardKind cardKind() const noexcept;
  [[nodiscard]] const VisibleCardData &data() const noexcept;
  [[nodiscard]] std::optional<CommandOutputView::ScrollState>
  commandOutputScrollState() const;
  void
  restoreCommandOutputScrollState(const CommandOutputView::ScrollState &state);

  // A key and kind identify the persistent widget. apply() updates all card
  // kinds in place and returns false when neither content nor presentation
  // changed. Passing a different key or kind is a programming error.
  bool apply(const VisibleCardData &data);

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] ConversationCard *
createConversationCard(const VisibleCardData &data, QWidget *parent = nullptr);

} // namespace codexui::codex::middle

#endif // CODEXUI_GREENFIELD_CODEX_MIDDLE_CONVERSATIONCARDS_H
