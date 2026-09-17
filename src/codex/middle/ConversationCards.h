// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H

#include "codex/middle/ConversationPresentation.h"
#include "codex/middle/MiddleTypes.h"

#include <QByteArray>
#include <QFrame>
#include <QPlainTextEdit>
#include <QTextEdit>

#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QPaintEvent;
class QTimer;
class QVBoxLayout;
class QWheelEvent;

namespace codexui::codex::middle {

enum class PresentationImpact { None, PaintOnly, GeometryChanged };

class ContentSizedTextView : public QTextEdit {
public:
  explicit ContentSizedTextView(int maximumContentHeight,
                                QWidget *parent = nullptr);

  bool setContent(const QString &content);
  void invalidateGeometryEnvironment();
  void settleWidth(int width);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  [[nodiscard]] bool measureAtCurrentWidth(bool notifyParent);

private:
  [[nodiscard]] bool setPreferredContentHeight(int height, bool notifyParent);
  int preferredHeight_ = 0;
  bool pinScrollToStart_ = false;
};

class CommandOutputView final : public QTextEdit {
  Q_OBJECT

public:
  struct State {
    bool followsLatest = true;
    int value = 0;
    int selectionPosition = -1;
    int selectionAnchor = -1;

    friend bool operator==(const State &, const State &) = default;
  };

  explicit CommandOutputView(QWidget *parent = nullptr);

  [[nodiscard]] State state() const;
  [[nodiscard]] bool followsLatest() const noexcept;
  [[nodiscard]] bool isHeightCapped() const noexcept;
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

  // Returns false for a true no-op. Programmatic document/range changes do
  // not alter the user's follow/paused choice.
  bool setOutput(const QString &output);
  void invalidateGeometryEnvironment();
  void settleWidth(int width);
  void restoreState(const State &state);

signals:
  // False is emitted only for direct scrollbar/wheel interaction. True is
  // also emitted when authoritative content retires a detached output owner;
  // restoring a new instance never claims conversation scroll ownership.
  void followLatestChanged(bool followsLatest);

protected:
  void wheelEvent(QWheelEvent *event) override;

private:
  [[nodiscard]] bool measureAtCurrentWidth(bool notifyParent);
  [[nodiscard]] bool setPreferredContentHeight(int height, bool notifyParent);
  void refreshMaximumHeight();
  void settleScroll();
  void scheduleScrollSettlement();
  void setUserFollowLatest(bool followsLatest);
  [[nodiscard]] bool isAtBottom() const;
  [[nodiscard]] bool outputRequiresMaximumHeight(const QString &output) const;

  bool followsLatest_ = true;
  bool suppressScrollState_ = false;
  bool scrollSettlementPending_ = false;
  int preferredHeight_ = 0;
  int preservedScrollValue_ = 0;
  QString currentOutput_;
};

class ConversationCard : public QFrame {
  Q_OBJECT

public:
  struct ContentFingerprint {
    qsizetype length = 0;
    QByteArray digest;

    friend bool operator==(const ContentFingerprint &,
                           const ContentFingerprint &) = default;
  };

  struct TextSelection {
    enum class Role {
      Title,
      Phase,
      Body,
      MarkdownBody,
      Metadata,
      Detail,
      Command,
      FileChanges,
    };

    Role role = Role::Body;
    int position = 0;
    int anchor = 0;
    int scrollValue = 0;
    ContentFingerprint source;
    presentation::MarkdownTailState markdownTail;
  };

  struct CommandOutputState {
    CommandOutputView::State view;
    ContentFingerprint source;

    friend bool operator==(const CommandOutputState &,
                           const CommandOutputState &) = default;
  };

  struct State {
    CardKind sourceKind = CardKind::GenericActivity;
    std::optional<CommandOutputState> commandOutput;
    std::vector<TextSelection> selections;

    [[nodiscard]] bool empty() const noexcept {
      return !commandOutput && selections.empty();
    }
  };

  explicit ConversationCard(const VisibleCardData &data,
                            bool initiallyCollapsed, QWidget *parent = nullptr,
                            int initialWidth = 0);
  ~ConversationCard() override;

  [[nodiscard]] const VisibleCardData &data() const noexcept;
  [[nodiscard]] bool isCollapsed() const noexcept;
  void setCollapsed(bool collapsed);
  bool setAuthoritativeTurnActive(bool active);
  // Select the established nested-card presentation for a child, or clear it
  // when the card becomes a turn root or a standalone activity.
  bool setNestedPresentation(bool nested);
  // In a virtualized turn the view paints the continuous outer You surface;
  // the root card keeps only its content and interaction geometry.
  bool setVirtualTurnRootPresentation(bool fragmented);
  // ConversationView uses this to pause local feedback timers while a card is
  // not painted.
  void setViewportVisible(bool visible);
  // Retires renderer-local measurements after the effective font, style, or
  // device-pixel ratio changes without replacing this interactive object.
  void invalidateGeometryEnvironment();
  [[nodiscard]] int settleHeightForWidth(int width);
  [[nodiscard]] State state() const;
  void restoreState(const State &state);

  // Reconciles sparse interaction state with the exact semantic source shown
  // by the next presentation. Returns true when a detached command-output
  // owner ended.
  static bool normalizeState(State &state, const VisibleCardData &next,
                             bool nextNested);

  [[nodiscard]] bool canApply(const VisibleCardData &data) const noexcept;

  // A key identifies the persistent widget. Matching card kinds update in
  // place, including the supported local-prompt-to-user transition.
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

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONCARDS_H
