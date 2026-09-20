// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationHeightIndex.h"
#include "codex/middle/ConversationItemModel.h"

#include <QAbstractItemView>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QLabel;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QTimer;
class QVariantAnimation;
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

  enum class SnapshotDisposition {
    Admitted,
    Retryable,
    Superseded,
  };

  using ReconciliationResult = ConversationItemModel::StructuralChangeResult;

  explicit ConversationView(QWidget *parent = nullptr);
  ~ConversationView() override;

  void setLoadMoreAction(std::function<void()> action);
  void
  setPromptMaterializedAction(std::function<bool(nodegraph::NodeRef)> action);
  void setPromptRecoveryAction(std::function<void(nodegraph::NodeRef)> action);
  void setNoticeAction(std::function<void(QString, bool)> action);
  void setReconciliationFinishedAction(
      std::function<void(const std::string &, ReconciliationResult, bool)>
          action);
  void setEmptyMessage(QString message);
  void setPresentationOptions(PresentationOptions options);
  [[nodiscard]] PresentationOptions presentationOptions() const noexcept {
    return presentationOptions_;
  }

  [[nodiscard]] ReconciliationResult
  reconcile(const ConversationSnapshot &snapshot);
  // Covers the outgoing message viewport immediately for a different-thread
  // selection. A delayed spinner remains presentation-only; the incoming
  // snapshot still commits through reconcileStaged as one complete frame.
  void beginThreadSelection(const std::string &threadId,
                            std::uint64_t incarnation = 0);
  // Commits a different-thread selection or explicit authority rescan.
  // Admitted snapshots complete asynchronously. A newer admitted snapshot
  // supersedes pending work without publishing a completion for the old work.
  [[nodiscard]] SnapshotDisposition
  reconcileStaged(ConversationSnapshot snapshot);
  [[nodiscard]] bool structuralStagingActive() const noexcept {
    return pendingStructuralSnapshot_.has_value() || committingStructuralStage_;
  }

  // Applies one adapter-projected transaction. Structural placement,
  // presentation updates, prompt acknowledgement, exact removal, and the
  // bounded tail fast path are deliberately not separate public contracts.
  [[nodiscard]] std::optional<PresentationImpact>
  applyConversationDelta(ConversationDelta delta);

  // Provider pagination remains graph-owned; the view only projects whether
  // the current request is pending.
  void setHistoryRequestPending(const std::string &threadId, bool pending);
  void setProviderHasMore(const std::string &threadId, bool hasMore);
  void forgetThreadPresentation(const std::string &threadId,
                                std::uint64_t incarnation = 0);
  [[nodiscard]] const std::string &presentedThreadId() const noexcept {
    return threadId_;
  }
  [[nodiscard]] bool
  retainsTarget(const nodegraph::NodeRef &target) const noexcept;

  // QSplitter live drags can produce one resize per pointer movement. Keep
  // the panel geometry live while bounding rich-text reflow to display-frame
  // cadence, then reconcile every cached row once the drag finishes.
  void beginInteractiveResize();
  void endInteractiveResize();

  [[nodiscard]] Mode mode() const noexcept { return mode_; }
  [[nodiscard]] Mode modeForThread(const std::string &threadId) const noexcept;
  [[nodiscard]] bool isAtBottom() const noexcept;
  [[nodiscard]] const ConversationItemModel *
  conversationModel() const noexcept {
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
  bool event(QEvent *event) override;
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
  void selectionChanged(const QItemSelection &selected,
                        const QItemSelection &deselected) override;
  void currentChanged(const QModelIndex &current,
                      const QModelIndex &previous) override;
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  friend class ConversationAccessible;
  friend class ConversationAccessibleItem;

  void revealRow(const QModelIndex &index, ScrollHint hint, bool userInitiated);

  struct Anchor {
    std::string stableKey;
    int pixelOffset = 0;
    int absoluteValue = 0;
  };

  struct ThreadScrollState {
    Mode mode = Mode::Following;
    Anchor anchor;
    bool pausedByCommandOutput = false;
  };

  struct HeightRecord {
    int width = 0;
    int height = 0;
  };

  struct RetainedCardState {
    std::optional<HeightRecord> height;
    std::optional<bool> collapsed;
    std::unique_ptr<ConversationCard::State> interaction;
  };

  struct ThreadPresentationState {
    std::uint64_t incarnation = 0;
    ThreadScrollState scroll;
    bool historyRequestPending = false;
    std::unordered_map<std::string, RetainedCardState> cards;
  };

  struct PendingLocation {
    std::size_t section = 0;
    std::size_t card = 0;
    bool nested = false;
  };

  struct SectionRange {
    std::string first;
    std::string last;
    std::string root;
  };

  [[nodiscard]] ConversationItemModel::StructuralChangeResult
  reconcileOwned(ConversationSnapshot snapshot,
                 std::vector<nodegraph::NodeRef> &acknowledged,
                 std::optional<std::string> &committedThread);
  void publishReconciliation(std::optional<std::string> completedThread,
                             ReconciliationResult result,
                             bool selectionCommitted,
                             std::vector<nodegraph::NodeRef> acknowledged);
  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentationOwned(VisibleCardData card, int &damageTop,
                             bool &geometryChanged);
  void updateHistoryControls();
  void finishStructuralDelta(
      const Anchor &anchor,
      std::span<const ConversationItemModel::StructuralDeltaPlan::Operation>
          operations,
      const std::unordered_map<std::string, bool> &rootChildrenPresented);
  [[nodiscard]] Anchor captureAnchor() const;
  void restoreAnchor(const Anchor &anchor, bool follow = false);
  void restoreViewport(const Anchor &anchor, bool follow = false);
  void setScrollValue(int value);
  void stopFollowingAnimation();
  void handleCommandOutputFollowLatest(const std::string &ownerThread,
                                       bool followsLatest);
  [[nodiscard]] bool
  hasDetachedCommandOutput(const std::string &ownerThread) const;
  void animateToBottom(int previousValue);
  void handleUserScrollValue(int value);
  void rebuildHeightIndex();
  void rebuildSectionRanges();
  void rebuildSectionRange(const std::string &sectionKey, int nearRow);
  void updateSectionRangeForPresentationChange(int row, bool wasPresented);
  [[nodiscard]] int
  estimatedCardHeight(const ConversationItemModel::Row &row) const;
  [[nodiscard]] bool cardCollapsed(const VisibleCardData &card,
                                   const std::string &stableKey) const;
  void retainAdmissionDefaults();
  void normalizeRetainedState(const VisibleCardData &card, bool nested);
  [[nodiscard]] int rowWidth(const ConversationItemModel::Row &row) const;
  [[nodiscard]] const RetainedCardState *
  retainedCardState(const std::string &threadId,
                    const std::string &stableKey) const;
  [[nodiscard]] const HeightRecord *
  retainedHeight(const ConversationItemModel::Row &row) const;
  [[nodiscard]] RetainedCardState &
  retainCardState(const std::string &threadId, const std::string &stableKey);
  void forgetCardPresentation(const std::string &threadId,
                              const std::string &stableKey);
  [[nodiscard]] bool rowPresented(int row) const;
  [[nodiscard]] static bool hasTurnSurface(const SectionRange &section);
  [[nodiscard]] int rowSpacing(int row) const;
  [[nodiscard]] int rowSpacing(int row, const SectionRange *section) const;
  [[nodiscard]] std::optional<int>
  modelSectionRow(const std::string &stableKey) const;
  [[nodiscard]] QRect rowRect(int row) const;
  [[nodiscard]] bool updateMeasuredHeight(int row, int cardHeight,
                                          bool preserveAnchor);
  [[nodiscard]] bool setMeasuredHeight(int row, int cardHeight);
  void updateScrollRange();
  enum class ReflowCause { ResizeFrame, ResizeExact, Environment };
  void reflowAfterResize(ReflowCause cause);
  void scheduleInteractiveResizeReflow();
  [[nodiscard]] int leadingChromeHeight() const noexcept;
  [[nodiscard]] qint64 naturalContentHeight() const noexcept;

  void updateMaterialization(bool admitOverscan = false);
  [[nodiscard]] std::pair<int, int>
  materializationRows(int overscanViewports = 1) const;
  [[nodiscard]] int nextOverscanAdmissionRow() const;
  [[nodiscard]] bool materializeRow(int row);
  void releaseUnneededCards(int firstRow, int lastRow,
                            ConversationCard *deferredEventReceiver = nullptr);
  void releaseCard(const std::string &key, ConversationCard *card,
                   ConversationCard *deferredEventReceiver = nullptr);
  void releaseAllCards();
  void layoutMaterializedCards();
  void updateFocusDecoration(const QModelIndex &index);
  [[nodiscard]] ConversationCard *
  cardForStableKey(const std::string &key) const;
  bool configureCardForRow(ConversationCard *card,
                           const ConversationItemModel::Row &row);
  [[nodiscard]] ConversationCard *createCard(const VisibleCardData &data,
                                             QWidget *parent,
                                             const std::string &key, int width,
                                             bool collapsed, bool nested);
  void setCardCollapsed(const std::string &key, ConversationCard *card,
                        bool collapsed);

  void buildPendingLocations();
  void choosePendingStageRows();
  [[nodiscard]] SnapshotDisposition
  stageSnapshot(ConversationSnapshot snapshot);
  [[nodiscard]] VisibleCardData *pendingCard(const std::string &key);
  [[nodiscard]] const PendingLocation *
  pendingLocation(const std::string &key) const;
  void scheduleCardAdmissionPass();
  void runCardAdmissionPass();
  void clearStagedCards();
  void cancelStructuralStaging();
  [[nodiscard]] bool finishThreadSelection(const std::string &threadId);

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
  std::function<void(QString, bool)> noticeAction_;
  std::function<void(const std::string &, ReconciliationResult, bool)>
      reconciliationFinishedAction_;

  std::unordered_map<std::string, ConversationCard *> materializedCards_;
  std::unordered_map<std::string, ConversationCard *> stagedCards_;
  std::unordered_map<std::string, SectionRange> sectionRanges_;
  std::unordered_map<std::string, ThreadPresentationState> threadPresentations_;

  PresentationOptions presentationOptions_;
  std::string threadId_;
  QString emptyMessage_;
  std::string loadingThreadId_;
  std::optional<ConversationSnapshot> pendingStructuralSnapshot_;
  std::unordered_map<std::string, PendingLocation> pendingLocations_;
  std::vector<std::string> pendingStructuralCardKeys_;

  Mode mode_ = Mode::Following;
  bool applying_ = false;
  bool programmaticScroll_ = false;
  bool userActionPending_ = false;
  bool pausedByCommandOutput_ = false;
  bool materializing_ = false;
  bool adjustingScrollRange_ = false;
  bool cardAdmissionPassScheduled_ = false;
  bool committingStructuralStage_ = false;
  bool interactiveResize_ = false;
  bool geometryEnvironmentReflowPending_ = false;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
