// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H

#include "codex/middle/ConversationCards.h"

#include <QAbstractScrollArea>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QLabel;
class QEvent;
class QPushButton;
class QSpacerItem;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;
class QWheelEvent;

namespace codexui::codex::middle {

// The conversation has one projection path and one geometry owner.  Its
// content is positioned directly in QAbstractScrollArea's viewport, so every
// reconciliation can update the layout, range, and stable anchor in one
// synchronous transaction.
class ConversationView final : public QAbstractScrollArea {
public:
  enum class Mode { Following, Paused };

  struct PresentationOptions {
    bool showReasoning = true;
    bool showCodexUpdates = true;
    bool commandsInitiallyExpanded = false;
    bool imagesInitiallyExpanded = false;
    bool fileChangesInitiallyExpanded = false;

    bool operator==(const PresentationOptions &) const = default;
  };

  explicit ConversationView(QWidget *parent = nullptr);
  ~ConversationView() override;

  void setLoadMoreAction(std::function<void()> action);
  void
  setPromptMaterializedAction(std::function<bool(nodegraph::NodeRef)> action);
  void setPromptRecoveryAction(std::function<void(nodegraph::NodeRef)> action);
  void setEmptyMessage(QString message);
  void setPresentationOptions(PresentationOptions options);
  [[nodiscard]] PresentationOptions presentationOptions() const noexcept {
    return presentationOptions_;
  }

  // Returns false for a typed projection no-op.  Existing cards are mutated by
  // key; first render and later updates use this same reconciliation path.
  bool reconcile(const ConversationSnapshot &snapshot);

  // Structural changes which introduce many rich cards are prepared under a
  // hidden Qt parent in bounded event-loop slices, then committed through the
  // ordinary reconcile contract in one visible transaction. Existing cards
  // remain retained throughout Load-more staging.
  void reconcileStaged(ConversationSnapshot snapshot);
  [[nodiscard]] bool structuralStagingActive() const noexcept {
    return pendingStructuralSnapshot_.has_value();
  }

  // Applies one already-materialized card without constructing or traversing
  // a complete conversation snapshot. A disengaged result requests the
  // structural reconcile path because the card is absent, hidden, or changed
  // identity/kind.
  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentation(const VisibleCardData &card);

  // Extra composer height is represented after the final card, while the
  // viewport itself keeps its canonical geometry.
  void setTrailingSpaceHeight(int height);

  // A local admission may resume a pause caused solely by composer growth.
  // Explicit user-owned scrolling remains paused.
  void prepareForLocalPromptAdmission();

  // Used by the middle-region chrome and adjacent splitter handles.  Nested
  // scrollable controls should consume their own event before this is called.
  bool forwardWheelEvent(QWheelEvent *event);

  [[nodiscard]] Mode mode() const noexcept { return mode_; }
  [[nodiscard]] Mode modeForThread(const std::string &threadId) const noexcept;
  [[nodiscard]] bool isAtBottom() const noexcept;
  [[nodiscard]] bool dispatchingNativeWheel() const noexcept {
    return dispatchingNativeWheel_;
  }
  [[nodiscard]] int trailingSpaceHeight() const noexcept {
    return trailingSpaceHeight_;
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  struct Anchor {
    std::string stableKey;
    int pixelOffset = 0;
    int absoluteValue = 0;
  };

  struct ThreadScrollState {
    Mode mode = Mode::Following;
    Anchor anchor;
    bool pausedByComposerGrowth = false;
  };

  class TurnSectionWidget;

  bool reconcile(ConversationSnapshot snapshot, bool force,
                 bool settleFollowImmediately);
  [[nodiscard]] bool tryReconcileSingleInsertion(
      ConversationSnapshot &snapshot, bool settleFollowImmediately);
  [[nodiscard]] bool cardVisible(const VisibleCardData &card) const noexcept;
  void setThread(const std::string &threadId);
  void setCardCollapsed(const std::string &key, ConversationCard *card,
                        bool collapsed);
  [[nodiscard]] Anchor captureAnchor() const;
  void restoreAnchor(const Anchor &anchor);
  void storeCurrentThreadState();
  void setScrollValue(int value);
  void stopFollowingAnimation();
  void animateToBottom(int previousValue);
  void recomputeCardGeometries(
      const std::vector<ConversationCard *> &changedCards);
  [[nodiscard]] int settleCardGeometry(ConversationCard *card, int width);
  void recomputeAppendedNestedCardGeometry(
      ConversationCard *card, ConversationCard *turnContainer,
      TurnSectionWidget *section, int previousNestedHeight,
      bool previousNestedVisible, int previousContainerHeight,
      int previousSectionHeight);
  void recomputeAppendedSectionGeometry(ConversationCard *card,
                                        TurnSectionWidget *section,
                                        int sectionTop);
  void settlePaintOnlyCard(ConversationCard *card);
  void scheduleStructuralStagePass();
  void runStructuralStagePass();
  void cancelStructuralStaging();
  [[nodiscard]] ConversationCard *createRetainedCard(
      const VisibleCardData &data, QWidget *parent, const std::string &key);
  [[nodiscard]] VisibleCardData *pendingCard(const std::string &key);
  void recomputeGeometry();
  void positionContent();
  void handleUserScrollValue(int value);
  [[nodiscard]] bool applyWheel(QWheelEvent *event);
  [[nodiscard]] ConversationCard *
  cardForStableKey(const std::string &stableKey) const;

  QWidget *content_ = nullptr;
  QWidget *stagingHost_ = nullptr;
  QLabel *stagingOverlay_ = nullptr;
  QVBoxLayout *contentLayout_ = nullptr;
  QPushButton *loadMore_ = nullptr;
  QSpacerItem *trailingSpace_ = nullptr;
  QLabel *empty_ = nullptr;
  QVariantAnimation *followAnimation_ = nullptr;
  std::function<void()> loadMoreAction_;
  std::function<bool(nodegraph::NodeRef)> promptMaterializedAction_;
  std::function<void(nodegraph::NodeRef)> promptRecoveryAction_;

  ConversationSnapshot snapshot_;
  std::string threadId_;
  std::unordered_map<std::string, TurnSectionWidget *> sections_;
  std::unordered_map<std::string, ConversationCard *> cards_;
  std::unordered_map<std::string, ConversationCard *> stagedCards_;
  std::vector<std::string> displayedSectionKeys_;
  std::vector<std::string> displayedCardKeys_;
  std::unordered_map<std::string, ThreadScrollState> threadStates_;
  std::unordered_map<std::string, CommandOutputView::ScrollState>
      commandOutputStates_;
  std::unordered_map<std::string, bool> cardCollapsedStates_;
  PresentationOptions presentationOptions_;
  std::optional<ConversationSnapshot> pendingStructuralSnapshot_;
  std::vector<std::string> pendingStructuralCardKeys_;
  std::size_t pendingStructuralCardIndex_ = 0;

  Mode mode_ = Mode::Following;
  int trailingSpaceHeight_ = 0;
  int naturalContentHeight_ = 0;
  int contentHeight_ = 0;
  QString emptyMessage_;
  bool applying_ = false;
  bool programmaticScroll_ = false;
  bool sliderDown_ = false;
  bool userActionPending_ = false;
  bool pausedByComposerGrowth_ = false;
  bool dispatchingNativeWheel_ = false;
  bool structuralStagePassScheduled_ = false;
  bool committingStructuralStage_ = false;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
