// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H

#include "codex/middle/ConversationCards.h"
#include "codex/nodegraph/NodeGraph.h"

#include <QAbstractScrollArea>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class QLabel;
class QEvent;
class QPushButton;
class QSpacerItem;
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

  // The view retains only the selected thread's loaded history window and its
  // structural NodeRefs. Cards in that bounded window materialize once;
  // subsequent protocol-derived projection reads remain visible-card-only.
  void bindGraph(const nodegraph::NodeGraph &graph,
                 nodegraph::NodeRef selectedThread);
  void graphChanged(std::span<const nodegraph::NodeRef> removed = {});
  void graphChangedDeferred(std::span<const nodegraph::NodeRef> removed = {});
  void graphChangedDeferred(std::span<const nodegraph::NodeRef> affected,
                            std::span<const nodegraph::NodeRef> removed);
  void detachRemovedNodes(std::span<const nodegraph::NodeRef> removed);

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

  enum class AtomicCover { None, Loading, FrozenFrame };

  struct ThreadScrollState {
    Mode mode = Mode::Following;
    Anchor anchor;
    bool pausedByComposerGrowth = false;
    std::size_t graphRequestedHistoryLimit = AuthoritativeHistoryPageSize;
    std::size_t graphHistoryLimit = AuthoritativeHistoryPageSize;
    std::size_t graphKnownItemCount = 0;
    std::string graphNewestItemKey;
  };

  class GraphViewportGeometry;
  class TurnSectionWidget;

  void setThread(const std::string &threadId);
  void setCardCollapsed(const std::string &key, ConversationCard *card,
                        bool collapsed);
  [[nodiscard]] Anchor captureAnchor() const;
  void restoreAnchor(const Anchor &anchor);
  void storeCurrentThreadState();
  void setScrollValue(int value);
  void stopFollowingAnimation();
  void restoreRequestedGraphHistoryLimit();
  void animateToBottom(int previousValue);
  void recomputeGeometry(
      const std::vector<TurnSectionWidget *> *affectedSections = nullptr);
  void arrangeSection(TurnSectionWidget *section);
  void clearGraph(bool preserveAtomicTransition = false);
  void scheduleGraphRefresh();
  void scheduleGraphContentionRetry();
  void scheduleVisibilityContentionRetry();
  void scheduleRetiredGeometryCleanup();
  void publishRetiredGeometryCleanupMetrics();
  void runGraphRefresh();
  [[nodiscard]] bool reconcileGraphViewport();
  void detachGraphWidgets(std::span<const nodegraph::NodeRef> removed = {});
  void updateGraphChrome();
  void scheduleVisibilityPass();
  void resetGraphVisibilityScan();
  [[nodiscard]] bool runVisibilityPass();
  [[nodiscard]] bool runGraphVisibilityPass();
  void beginAtomicMaterialization(
      bool fullCommit, AtomicCover cover = AtomicCover::None);
  void finishBulkMaterializationIfReady();
  void positionContent();
  void handleUserScrollValue(int value);
  [[nodiscard]] bool applyWheel(QWheelEvent *event);
  [[nodiscard]] ConversationCard *
  cardForStableKey(const std::string &stableKey) const;
  [[nodiscard]] QWidget *itemForStableKey(const std::string &stableKey) const;

  QWidget *content_ = nullptr;
  QLabel *atomicTransitionOverlay_ = nullptr;
  QVBoxLayout *contentLayout_ = nullptr;
  QPushButton *loadMore_ = nullptr;
  QWidget *graphLeadingPlaceholder_ = nullptr;
  QWidget *graphTrailingPlaceholder_ = nullptr;
  QLabel *empty_ = nullptr;
  QVariantAnimation *followAnimation_ = nullptr;
  std::function<void()> loadMoreAction_;
  std::function<bool(nodegraph::NodeRef)> promptMaterializedAction_;
  std::function<void(nodegraph::NodeRef)> promptRecoveryAction_;

  const nodegraph::NodeGraph *graph_ = nullptr;
  nodegraph::NodeRef graphThread_;
  std::unique_ptr<GraphViewportGeometry> graphGeometry_;
  std::vector<TurnSectionWidget *> graphSections_;
  std::size_t graphRequestedHistoryLimit_ = AuthoritativeHistoryPageSize;
  std::size_t graphHistoryLimit_ = AuthoritativeHistoryPageSize;
  std::size_t graphKnownItemCount_ = 0;
  std::string graphNewestItemKey_;
  std::size_t graphHiddenItemCount_ = 0;
  std::size_t graphWindowItemCount_ = 0;
  bool graphProviderHasMore_ = false;
  std::size_t visibilitySectionCursor_ = 0;
  std::size_t visibilitySlotCursor_ = 0;
  std::size_t visibilitySlotsRemaining_ = 0;
  int visibilityScanScrollTop_ = -1;
  int visibilityScanViewportHeight_ = -1;
  int visibilityScanViewportWidth_ = -1;
  int visibilityScanContentHeight_ = -1;
  std::optional<std::pair<std::size_t, std::size_t>>
      immediateVisibilityStart_;
  std::vector<nodegraph::NodeRef> immediateMaterializationNodes_;
  std::string threadId_;
  std::unordered_map<std::string, ThreadScrollState> threadStates_;
  std::unordered_map<std::string, CommandOutputView::ScrollState>
      commandOutputStates_;
  std::unordered_map<std::string, bool> cardCollapsedStates_;
  std::optional<Anchor> pendingGraphAnchorRestore_;
  PresentationOptions presentationOptions_;

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
  bool graphRefreshScheduled_ = false;
  bool visibilityPassScheduled_ = false;
  bool retiredGeometryCleanupScheduled_ = false;
  bool bulkMaterializationUpdatesSuppressed_ = false;
  bool atomicMaterializationFullCommit_ = false;
  bool graphHydrationSettled_ = true;
  bool graphViewportReconciliationPending_ = false;
  std::optional<Anchor> atomicMaterializationAnchor_;
  std::vector<TurnSectionWidget *> atomicMaterializationSections_;
  std::size_t graphPassCardOperations_ = 0;
  std::uint64_t graphBindingEpoch_ = 0;
  std::uint64_t graphRefreshPasses_ = 0;
  std::uint64_t geometryPasses_ = 0;
  std::uint64_t fullGeometryPasses_ = 0;
  int geometryWidthCorrectionDepth_ = 0;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONVIEW_H
