// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationHeightIndex.h"
#include "codex/middle/ConversationItemModel.h"

#include <QAbstractItemView>
#include <QPointer>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QLabel;
class QAbstractButton;
class QEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QTimer;
class QVariantAnimation;
class QMouseEvent;
class QWheelEvent;

namespace codexui::codex::middle {

class ConversationLoadingOverlay;

// Canonical variable-height item view for the conversation. NodeGraph remains
// authoritative; this class owns only Qt indexing, cached row geometry, and
// genuinely local interaction state. QWidget count is bounded by the visible
// viewport plus one viewport of overscan on each side.
class ConversationView final : public QAbstractItemView {
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

  struct HistoryPageRequest {
    std::size_t effectiveLimit = AuthoritativeHistoryPageSize;
    bool requestProvider = false;
  };

  explicit ConversationView(QWidget *parent = nullptr);
  ~ConversationView() override;

  void setLoadMoreAction(std::function<void()> action);
  void
  setPromptMaterializedAction(std::function<bool(nodegraph::NodeRef)> action);
  void setPromptRecoveryAction(std::function<void(nodegraph::NodeRef)> action);
  void setPresentationCommittedAction(
      std::function<void(const std::string &)> action);
  void setEmptyMessage(QString message);
  void setPresentationOptions(PresentationOptions options);
  [[nodiscard]] PresentationOptions presentationOptions() const noexcept {
    return presentationOptions_;
  }

  [[nodiscard]] bool reconcile(const ConversationSnapshot &snapshot);
  // Covers the outgoing message viewport immediately for a different-thread
  // selection. A delayed spinner remains presentation-only; the incoming
  // snapshot still commits through reconcileStaged as one complete frame.
  void beginThreadSelection(const std::string &threadId);
  // Commits a different-thread selection or explicit authority rescan.
  void reconcileStaged(ConversationSnapshot snapshot);
  // Commits a same-thread ordered superset using precise row insertions and
  // row-local value changes; retained rows are never reset or moved.
  void prependHistoryPageStaged(ConversationSnapshot snapshot);
  [[nodiscard]] bool structuralStagingActive() const noexcept {
    return pendingStructuralSnapshot_.has_value();
  }

  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentation(const VisibleCardData &card);
  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentation(VisibleCardData &&card);
  [[nodiscard]] std::optional<PresentationImpact>
  applyPromptMaterialization(PromptMaterialization materialization);
  // Applies one exact non-tail row insertion or movement. Canonical neighbor
  // keys determine the final model row; no complete snapshot is consulted.
  [[nodiscard]] bool applyRowChange(ConversationRowChange change);
  // Removes only the row whose current identity is the exact target NodeRef.
  // A missing target is not treated as a structural authority replacement.
  [[nodiscard]] bool removeCardTarget(const nodegraph::NodeRef &target);
  // Applies one canonical tail insertion without traversing retained model
  // rows. Returns false when the delta is not the exact append shape; the
  // caller then uses exact neighbor placement for that same NodeRef.
  [[nodiscard]] bool appendTailCard(ConversationTailCard tail);

  // History-window and staged-presentation state belong to the item view.
  // Shell supplies current canonical counts but retains no presentation copy.
  [[nodiscard]] std::size_t historyLimitForThread(
      const std::string &threadId, std::size_t authoritativeItemCount);
  [[nodiscard]] HistoryPageRequest requestNextHistoryPage(
      const std::string &threadId, std::size_t authoritativeItemCount,
      bool providerHasMore);
  void forgetThreadPresentation(const std::string &threadId);
  [[nodiscard]] const std::string &presentedThreadId() const noexcept {
    return threadId_;
  }

  void setTrailingSpaceHeight(int height);
  void prepareForLocalPromptAdmission();
  bool forwardWheelEvent(QWheelEvent *event);
  // QSplitter live drags can produce one resize per pointer movement. Keep
  // the panel geometry live while bounding rich-text reflow to display-frame
  // cadence, then reconcile every cached row once the drag finishes.
  void beginInteractiveResize();
  void endInteractiveResize();

  [[nodiscard]] Mode mode() const noexcept { return mode_; }
  [[nodiscard]] Mode modeForThread(const std::string &threadId) const noexcept;
  [[nodiscard]] bool isAtBottom() const noexcept;
  [[nodiscard]] bool dispatchingNativeWheel() const noexcept {
    return dispatchingNativeWheel_;
  }
  [[nodiscard]] int trailingSpaceHeight() const noexcept {
    return trailingSpaceHeight_;
  }
  [[nodiscard]] ConversationItemModel *conversationModel() const noexcept {
    return model_;
  }
  [[nodiscard]] int materializedCardCount() const noexcept {
    return static_cast<int>(materializedCards_.size());
  }

  [[nodiscard]] QRect visualRect(const QModelIndex &index) const override;
  void scrollTo(const QModelIndex &index,
                ScrollHint hint = EnsureVisible) override;
  [[nodiscard]] QModelIndex indexAt(const QPoint &point) const override;

protected:
  [[nodiscard]] QModelIndex
  moveCursor(CursorAction cursorAction,
             Qt::KeyboardModifiers modifiers) override;
  [[nodiscard]] int horizontalOffset() const override;
  [[nodiscard]] int verticalOffset() const override;
  [[nodiscard]] bool isIndexHidden(const QModelIndex &index) const override;
  void setSelection(const QRect &rect,
                    QItemSelectionModel::SelectionFlags command) override;
  [[nodiscard]] QRegion
  visualRegionForSelection(const QItemSelection &selection) const override;
  void updateGeometries() override;
  void scrollContentsBy(int dx, int dy) override;
  bool viewportEvent(QEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;
  void currentChanged(const QModelIndex &current,
                      const QModelIndex &previous) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  struct Anchor {
    std::string stableKey;
    int pixelOffset = 0;
    int absoluteValue = 0;
    int horizontalValue = 0;
  };

  struct ThreadScrollState {
    Mode mode = Mode::Following;
    Anchor anchor;
    bool pausedByComposerGrowth = false;
  };

  struct HistoryWindow {
    std::size_t requested = AuthoritativeHistoryPageSize;
    std::size_t effective = AuthoritativeHistoryPageSize;
    std::size_t lastAuthoritativeCount = 0;
  };

  struct HeightRecord {
    int width = 0;
    int height = 0;
  };

  struct PendingLocation {
    std::size_t section = 0;
    std::size_t card = 0;
    bool nested = false;
    bool root = false;
    bool activeTurn = false;
  };

  struct SectionRange {
    std::string first;
    std::string last;
    std::string root;
    bool active = false;
  };

  struct LabelSelection {
    int ordinal = -1;
    int start = -1;
    int length = 0;
  };

  struct EditSelection {
    int ordinal = -1;
    int position = 0;
    int anchor = 0;
  };

  struct CardInteractionState {
    std::vector<LabelSelection> labels;
    std::vector<EditSelection> edits;
    std::vector<EditSelection> plainEdits;
  };

  enum class SnapshotOperation {
    OrderedReconciliation,
    AuthorityReplacement,
    HistoryPrepend,
  };

  [[nodiscard]] bool reconcileOwned(ConversationSnapshot snapshot,
                                    SnapshotOperation operation);
  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentationOwned(VisibleCardData card,
                             nodegraph::NodeRef materializedPrompt = {});
  void finishExactStructureChange(const Anchor &anchor, bool follow,
                                  int sourceRow, int destinationRow,
                                  std::string changedKey,
                                  std::string oldSection,
                                  std::string newSection);
  [[nodiscard]] bool cardVisible(const VisibleCardData &card) const noexcept;
  void setThread(const std::string &threadId);
  void storeCurrentThreadState();
  [[nodiscard]] Anchor captureAnchor() const;
  [[nodiscard]] Anchor captureHistoryPrependAnchor() const;
  void restoreAnchor(const Anchor &anchor);
  void setScrollValue(int value);
  void stopFollowingAnimation();
  void handleCommandOutputFollowLatest(CommandOutputView *output,
                                       bool followsLatest);
  void animateToBottom(int previousValue);
  void handleUserScrollValue(int value);
  [[nodiscard]] bool applyWheel(QWheelEvent *event);

  void rebuildHeightIndex();
  void rebuildSectionRanges();
  void rebuildSectionRange(const std::string &sectionKey, int nearRow);
  void updateSectionRangeForPresentationChange(int row, bool wasPresented);
  [[nodiscard]] int estimatedCardHeight(const VisibleCardData &card) const;
  [[nodiscard]] int rowWidth(const ConversationItemModel::Row &row) const;
  [[nodiscard]] bool
  rowUsesPassiveDelegate(const ConversationItemModel::Row &row) const;
  [[nodiscard]] bool rowCollapsed(const ConversationItemModel::Row &row) const;
  [[nodiscard]] bool rowPresented(int row) const;
  [[nodiscard]] int rowSpacing(int row) const;
  [[nodiscard]] int rowSpacing(int row, const SectionRange *section) const;
  [[nodiscard]] std::optional<int>
  modelSectionRow(const std::string &stableKey) const;
  [[nodiscard]] QRect rowRect(int row) const;
  [[nodiscard]] int measureCard(ConversationCard *card, int width) const;
  [[nodiscard]] bool updateMeasuredHeight(int row, int cardHeight,
                                          bool preserveAnchor);
  void updateScrollRange();
  void reflowAfterResize(bool exact);
  void scheduleInteractiveResizeReflow();
  [[nodiscard]] int leadingChromeHeight() const noexcept;
  [[nodiscard]] qint64 naturalContentHeight() const noexcept;

  void updateMaterialization(bool preserveAnchor = true);
  [[nodiscard]] std::pair<int, int> materializationRows() const;
  [[nodiscard]] ConversationCard *materializeRow(int row,
                                                 bool forInteraction = false);
  void releaseUnneededCards(int firstRow, int lastRow);
  void releaseCard(const std::string &key, ConversationCard *card);
  void captureCardInteractionState(const std::string &key,
                                   ConversationCard *card,
                                   bool preserveExistingWhenEmpty);
  void restoreCardInteractionState(const std::string &key,
                                   ConversationCard *card);
  void releaseAllCards();
  void layoutMaterializedCards();
  void updateMaterializationProperties();
  [[nodiscard]] ConversationCard *
  cardForStableKey(const std::string &key) const;
  void configureCardForRow(ConversationCard *card,
                           const ConversationItemModel::Row &row);
  [[nodiscard]] ConversationCard *createCard(const VisibleCardData &data,
                                             QWidget *parent,
                                             const std::string &key,
                                             int width, bool collapsed);
  void setCardCollapsed(const std::string &key, ConversationCard *card,
                        bool collapsed);

  void buildPendingLocations();
  void choosePendingStageRows();
  void stageSnapshot(ConversationSnapshot snapshot,
                     SnapshotOperation operation);
  [[nodiscard]] VisibleCardData *pendingCard(const std::string &key);
  [[nodiscard]] const PendingLocation *
  pendingLocation(const std::string &key) const;
  void scheduleStructuralStagePass();
  void runStructuralStagePass();
  void cancelStructuralStaging();
  void finishThreadSelection(const std::string &threadId);

  ConversationItemModel *model_ = nullptr;
  ConversationHeightIndex heights_;
  QLabel *empty_ = nullptr;
  QPushButton *loadMore_ = nullptr;
  QWidget *stagingHost_ = nullptr;
  ConversationLoadingOverlay *stagingOverlay_ = nullptr;
  QVariantAnimation *followAnimation_ = nullptr;
  QTimer *resizeFrameTimer_ = nullptr;
  QTimer *resizeSettleTimer_ = nullptr;

  std::function<void()> loadMoreAction_;
  std::function<bool(nodegraph::NodeRef)> promptMaterializedAction_;
  std::function<void(nodegraph::NodeRef)> promptRecoveryAction_;
  std::function<void(const std::string &)> presentationCommittedAction_;

  std::unordered_map<std::string, ConversationCard *> materializedCards_;
  std::unordered_map<std::string, ConversationCard *> stagedCards_;
  std::unordered_map<std::string, int> stagedHeights_;
  std::unordered_map<std::string, HeightRecord> heightCache_;
  std::unordered_map<std::string, SectionRange> sectionRanges_;
  std::string activeSectionKey_;
  std::unordered_map<std::string, CardInteractionState> cardInteractionStates_;
  std::unordered_map<std::string, bool> cardCollapsedStates_;
  std::unordered_map<std::string, CommandOutputView::ScrollState>
      commandOutputStates_;
  std::unordered_map<std::string, ThreadScrollState> threadStates_;
  std::unordered_map<std::string, HistoryWindow> historyWindows_;

  PresentationOptions presentationOptions_;
  std::string threadId_;
  QString emptyMessage_;
  std::string loadingThreadId_;
  std::optional<ConversationSnapshot> pendingStructuralSnapshot_;
  SnapshotOperation pendingSnapshotOperation_ =
      SnapshotOperation::AuthorityReplacement;
  std::unordered_map<std::string, PendingLocation> pendingLocations_;
  std::vector<std::string> pendingStructuralCardKeys_;
  std::size_t pendingStructuralCardIndex_ = 0;

  Mode mode_ = Mode::Following;
  int trailingSpaceHeight_ = 0;
  bool applying_ = false;
  bool programmaticScroll_ = false;
  bool sliderDown_ = false;
  bool userActionPending_ = false;
  bool pausedByComposerGrowth_ = false;
  QPointer<CommandOutputView> commandOutputPauseOwner_;
  bool dispatchingNativeWheel_ = false;
  bool materializing_ = false;
  bool adjustingScrollRange_ = false;
  bool structuralStagePassScheduled_ = false;
  bool committingStructuralStage_ = false;
  bool interactiveResize_ = false;
  bool preservePointerAnchor_ = false;
  // A synthetic event ignored by a card child can propagate back through the
  // viewport. Stop that propagated event from entering the forwarding path a
  // second time.
  bool forwardingMouseEvent_ = false;
  QPointer<QAbstractButton> forwardedButtonAction_;
  QRect forwardedButtonViewportRect_;
  QPointer<QWidget> forwardedMouseTarget_;
  QPoint forwardedMouseViewportOrigin_;
  QPoint forwardedMouseLocalOrigin_;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
