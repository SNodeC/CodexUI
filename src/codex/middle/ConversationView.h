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
class QEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QTimer;
class QVariantAnimation;
class QMouseEvent;
class QWheelEvent;

namespace codexui::codex::middle {

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

  [[nodiscard]] bool reconcile(const ConversationSnapshot &snapshot);
  void reconcileStaged(ConversationSnapshot snapshot);
  [[nodiscard]] bool structuralStagingActive() const noexcept {
    return pendingStructuralSnapshot_.has_value();
  }

  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentation(const VisibleCardData &card);
  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentation(VisibleCardData &&card);

  void setTrailingSpaceHeight(int height);
  void prepareForLocalPromptAdmission();
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
  bool eventFilter(QObject *watched, QEvent *event) override;
  void currentChanged(const QModelIndex &current,
                      const QModelIndex &previous) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
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
    int first = -1;
    int last = -1;
    int root = -1;
    bool active = false;
  };

  [[nodiscard]] bool reconcileOwned(ConversationSnapshot snapshot);
  [[nodiscard]] std::optional<PresentationImpact>
  applyCardPresentationOwned(VisibleCardData card);
  [[nodiscard]] bool cardVisible(const VisibleCardData &card) const noexcept;
  void setThread(const std::string &threadId);
  void storeCurrentThreadState();
  [[nodiscard]] Anchor captureAnchor() const;
  void restoreAnchor(const Anchor &anchor);
  void setScrollValue(int value);
  void stopFollowingAnimation();
  void animateToBottom(int previousValue);
  void handleUserScrollValue(int value);
  [[nodiscard]] bool applyWheel(QWheelEvent *event);

  void rebuildHeightIndex();
  void rebuildSectionRanges();
  [[nodiscard]] int estimatedCardHeight(const VisibleCardData &card) const;
  [[nodiscard]] int rowWidth(const ConversationItemModel::Row &row) const;
  [[nodiscard]] bool
  rowUsesPassiveDelegate(const ConversationItemModel::Row &row) const;
  [[nodiscard]] bool rowCollapsed(const ConversationItemModel::Row &row) const;
  [[nodiscard]] int rowSpacing(int row) const;
  [[nodiscard]] QRect rowRect(int row) const;
  [[nodiscard]] int measureCard(ConversationCard *card, int width) const;
  [[nodiscard]] bool updateMeasuredHeight(int row, int cardHeight,
                                          bool preserveAnchor);
  void updateScrollRange();
  [[nodiscard]] int leadingChromeHeight() const noexcept;
  [[nodiscard]] qint64 naturalContentHeight() const noexcept;

  void updateMaterialization(bool preserveAnchor = true);
  [[nodiscard]] std::pair<int, int> materializationRows() const;
  [[nodiscard]] ConversationCard *materializeRow(int row,
                                                 bool forInteraction = false);
  void releaseUnneededCards(int firstRow, int lastRow);
  void releaseCard(const std::string &key, ConversationCard *card);
  void releaseAllCards();
  void layoutMaterializedCards();
  void updateMaterializationProperties();
  [[nodiscard]] ConversationCard *
  cardForStableKey(const std::string &key) const;
  void configureCardForRow(ConversationCard *card,
                           const ConversationItemModel::Row &row);
  [[nodiscard]] ConversationCard *createCard(const VisibleCardData &data,
                                             QWidget *parent,
                                             const std::string &key);
  void setCardCollapsed(const std::string &key, ConversationCard *card,
                        bool collapsed);

  void buildPendingLocations();
  void choosePendingStageRows();
  [[nodiscard]] VisibleCardData *pendingCard(const std::string &key);
  [[nodiscard]] const PendingLocation *
  pendingLocation(const std::string &key) const;
  void scheduleStructuralStagePass();
  void runStructuralStagePass();
  void cancelStructuralStaging();

  ConversationItemModel *model_ = nullptr;
  ConversationHeightIndex heights_;
  QLabel *empty_ = nullptr;
  QPushButton *loadMore_ = nullptr;
  QWidget *stagingHost_ = nullptr;
  QLabel *stagingOverlay_ = nullptr;
  QVariantAnimation *followAnimation_ = nullptr;

  std::function<void()> loadMoreAction_;
  std::function<bool(nodegraph::NodeRef)> promptMaterializedAction_;
  std::function<void(nodegraph::NodeRef)> promptRecoveryAction_;

  std::unordered_map<std::string, ConversationCard *> materializedCards_;
  std::unordered_map<std::string, ConversationCard *> stagedCards_;
  std::unordered_map<std::string, int> stagedHeights_;
  std::unordered_map<std::string, HeightRecord> heightCache_;
  std::unordered_map<std::string, SectionRange> sectionRanges_;
  std::unordered_map<std::string, bool> cardCollapsedStates_;
  std::unordered_map<std::string, CommandOutputView::ScrollState>
      commandOutputStates_;
  std::unordered_map<std::string, ThreadScrollState> threadStates_;

  PresentationOptions presentationOptions_;
  std::string threadId_;
  QString emptyMessage_;
  std::optional<ConversationSnapshot> pendingStructuralSnapshot_;
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
  bool dispatchingNativeWheel_ = false;
  bool materializing_ = false;
  bool structuralStagePassScheduled_ = false;
  bool committingStructuralStage_ = false;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
