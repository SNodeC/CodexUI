// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationView.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int CardSpacing = 8;
constexpr int HistoryButtonHeight = 32;
constexpr int NestedCardIndent = 12;
constexpr int NativeScrollLineStep = 20;
constexpr int EstimatedCardHeight = 112;
constexpr int MinimumMaterializationRows = 8;

QLabel *makeEmptyLabel(QWidget *parent) {
  auto *label =
      new QLabel(QStringLiteral("Conversation activity appears here."), parent);
  label->setProperty("kind", "muted");
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}

void incrementProperty(QObject *object, const char *name) {
  object->setProperty(name, object->property(name).toULongLong() + 1);
}

} // namespace

ConversationView::ConversationView(QWidget *parent)
    : QAbstractItemView(parent), model_(new ConversationItemModel(this)) {
  setObjectName(QStringLiteral("conversationScroll"));
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setTabKeyNavigation(true);
  setModel(model_);
  verticalScrollBar()->setSingleStep(NativeScrollLineStep);
  viewport()->setAutoFillBackground(false);

  loadMore_ =
      new QPushButton(QStringLiteral("Load more activities"), viewport());
  loadMore_->setObjectName(QStringLiteral("conversationLoadMore"));
  loadMore_->setProperty("kind", "history");
  loadMore_->setFixedHeight(HistoryButtonHeight);
  loadMore_->hide();
  connect(loadMore_, &QPushButton::clicked, this, [this] {
    if (loadMoreAction_)
      loadMoreAction_();
  });

  empty_ = makeEmptyLabel(viewport());
  emptyMessage_ = empty_->text();

  // Rich rows prepared for an atomic thread/paging reveal are never parented
  // into the visible or accessible viewport until their final geometry is
  // known.
  stagingHost_ = new QWidget;
  stagingHost_->setObjectName(QStringLiteral("conversationStagingHost"));
  stagingHost_->hide();

  stagingOverlay_ =
      new QLabel(QStringLiteral("Loading conversation…"), viewport());
  stagingOverlay_->setObjectName(QStringLiteral("conversationStagingOverlay"));
  stagingOverlay_->setAlignment(Qt::AlignCenter);
  stagingOverlay_->setAutoFillBackground(true);
  stagingOverlay_->hide();

  followAnimation_ = new QVariantAnimation(this);
  followAnimation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(followAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            if (mode_ != Mode::Following || applying_) {
              followAnimation_->stop();
              return;
            }
            setScrollValue(
                std::max(verticalScrollBar()->value(), value.toInt()));
          });
  connect(followAnimation_, &QVariantAnimation::finished, this, [this] {
    if (mode_ == Mode::Following)
      setScrollValue(verticalScrollBar()->maximum());
  });

  connect(verticalScrollBar(), &QScrollBar::sliderPressed, this, [this] {
    sliderDown_ = true;
    pausedByComposerGrowth_ = false;
    stopFollowingAnimation();
  });
  connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this] {
    sliderDown_ = false;
    handleUserScrollValue(verticalScrollBar()->value());
  });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
          [this](int action) {
            userActionPending_ = true;
            pausedByComposerGrowth_ = false;
            stopFollowingAnimation();
            if (action == QAbstractSlider::SliderSingleStepSub ||
                action == QAbstractSlider::SliderPageStepSub ||
                action == QAbstractSlider::SliderToMinimum)
              mode_ = Mode::Paused;
          });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (!programmaticScroll_ && !applying_ &&
                (sliderDown_ || userActionPending_))
              handleUserScrollValue(value);
            userActionPending_ = false;
          });

  rebuildHeightIndex();
  updateScrollRange();
}

ConversationView::~ConversationView() {
  cancelStructuralStaging();
  releaseAllCards();
  delete stagingHost_;
}

void ConversationView::setLoadMoreAction(std::function<void()> action) {
  loadMoreAction_ = std::move(action);
}

void ConversationView::setPromptMaterializedAction(
    std::function<bool(nodegraph::NodeRef)> action) {
  promptMaterializedAction_ = std::move(action);
}

void ConversationView::setPromptRecoveryAction(
    std::function<void(nodegraph::NodeRef)> action) {
  promptRecoveryAction_ = std::move(action);
}

void ConversationView::setEmptyMessage(QString message) {
  if (message == emptyMessage_)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  emptyMessage_ = std::move(message);
  empty_->setText(emptyMessage_);
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  layoutMaterializedCards();
  viewport()->update();
}

void ConversationView::setPresentationOptions(PresentationOptions options) {
  if (presentationOptions_ == options)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  presentationOptions_ = options;
  const bool visibilityChanged =
      model_->setVisibility({options.showReasoning, options.showCodexUpdates});
  if (!visibilityChanged)
    return;
  rebuildHeightIndex();
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  updateMaterialization(false);
  viewport()->update();
}

bool ConversationView::cardVisible(const VisibleCardData &card) const noexcept {
  if (card.kind == CardKind::Reasoning)
    return presentationOptions_.showReasoning;
  if (card.kind != CardKind::AgentMessage)
    return true;
  const auto *message = std::get_if<AgentMessageData>(&card.payload);
  return !message || message->finalAnswer ||
         presentationOptions_.showCodexUpdates;
}

void ConversationView::storeCurrentThreadState() {
  if (threadId_.empty())
    return;
  threadStates_[threadId_] = {mode_, captureAnchor(), pausedByComposerGrowth_};
}

void ConversationView::setThread(const std::string &threadId) {
  if (threadId == threadId_)
    return;
  storeCurrentThreadState();
  stopFollowingAnimation();
  threadId_ = threadId;
  const auto saved = threadStates_.find(threadId_);
  mode_ = saved == threadStates_.end() ? Mode::Following : saved->second.mode;
  pausedByComposerGrowth_ =
      saved != threadStates_.end() && saved->second.pausedByComposerGrowth;
}

bool ConversationView::reconcile(const ConversationSnapshot &snapshot) {
  if (!committingStructuralStage_ && pendingStructuralSnapshot_)
    cancelStructuralStaging();
  return reconcileOwned(ConversationSnapshot(snapshot));
}

bool ConversationView::reconcileOwned(ConversationSnapshot snapshot) {
  const bool switchedThread = snapshot.threadId != threadId_;
  const Anchor currentAnchor = captureAnchor();
  setThread(snapshot.threadId);
  Anchor targetAnchor = currentAnchor;
  if (switchedThread) {
    const auto saved = threadStates_.find(snapshot.threadId);
    targetAnchor =
        saved == threadStates_.end() ? Anchor{} : saved->second.anchor;
  }
  const bool follow = mode_ == Mode::Following;

  std::unordered_map<std::string, CardKind> previousKinds;
  previousKinds.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int row = 0; row < model_->rowCount(); ++row) {
    if (const auto *value = model_->row(row))
      previousKinds.emplace(value->stableKey, value->card.kind);
  }

  const QScopedValueRollback applying(applying_, true);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  viewport()->setUpdatesEnabled(false);
  stopFollowingAnimation();

  const bool changed = model_->reconcile(std::move(snapshot));
  loadMore_->setVisible(model_->hasMore());
  empty_->setVisible(model_->rowCount() == 0);

  std::vector<std::string> removeKeys;
  removeKeys.reserve(materializedCards_.size());
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row || !row->presented ||
        !card->canApply(row->card)) {
      removeKeys.push_back(key);
      continue;
    }
    if (card->data() != row->card) {
      if (card->applyPresentation(row->card) ==
          PresentationImpact::GeometryChanged)
        heightCache_.erase(key);
    }
    configureCardForRow(card, *row);
  }
  for (const std::string &key : removeKeys) {
    const auto found = materializedCards_.find(key);
    if (found == materializedCards_.end())
      continue;
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    releaseCard(key, card);
  }

  rebuildHeightIndex();
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(targetAnchor);
  updateMaterialization(false);

  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(targetAnchor);
  layoutMaterializedCards();
  storeCurrentThreadState();

  std::vector<nodegraph::NodeRef> acknowledged;
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const auto *row = model_->row(rowIndex);
    if (!row || row->card.kind != CardKind::UserMessage || !row->card.target)
      continue;
    const auto before = previousKinds.find(row->stableKey);
    if (before != previousKinds.end() &&
        before->second == CardKind::LocalPrompt)
      acknowledged.push_back(row->card.target);
  }

  viewport()->setUpdatesEnabled(true);
  viewport()->update();
  if (changed)
    incrementProperty(this, "graphRefreshPasses");
  for (nodegraph::NodeRef &target : acknowledged)
    if (promptMaterializedAction_ &&
        !promptMaterializedAction_(std::move(target)))
      break;
  return changed;
}

void ConversationView::reconcileStaged(ConversationSnapshot snapshot) {
  cancelStructuralStaging();
  pendingStructuralSnapshot_ = std::move(snapshot);
  buildPendingLocations();
  choosePendingStageRows();
  pendingStructuralCardIndex_ = 0;
  stagingHost_->resize(std::max(0, viewport()->width()),
                       std::max(0, viewport()->height()));
  if (pendingStructuralSnapshot_->threadId != threadId_) {
    stagingOverlay_->setGeometry(viewport()->rect());
    stagingOverlay_->show();
    stagingOverlay_->raise();
  }
  incrementProperty(this, "structuralStageStarts");
  if (pendingStructuralCardKeys_.empty()) {
    runStructuralStagePass();
    return;
  }
  scheduleStructuralStagePass();
}

void ConversationView::buildPendingLocations() {
  pendingLocations_.clear();
  if (!pendingStructuralSnapshot_)
    return;
  for (std::size_t sectionIndex = 0;
       sectionIndex < pendingStructuralSnapshot_->sections.size();
       ++sectionIndex) {
    TurnSection &section = pendingStructuralSnapshot_->sections[sectionIndex];
    std::optional<std::string> root;
    if (section.rootCardKey)
      root = stableKey(*section.rootCardKey);
    const bool representedRoot =
        root && std::ranges::any_of(section.cards, [&](const auto &card) {
          return stableKey(card.key) == *root;
        });
    for (std::size_t cardIndex = 0; cardIndex < section.cards.size();
         ++cardIndex) {
      const std::string key = stableKey(section.cards[cardIndex].key);
      const bool isRoot = representedRoot && key == *root;
      pendingLocations_.emplace(
          key,
          PendingLocation{
              sectionIndex, cardIndex, representedRoot && !isRoot, isRoot,
              isRoot && pendingStructuralSnapshot_->activeTurnId &&
                  section.turnId == *pendingStructuralSnapshot_->activeTurnId});
    }
  }
}

void ConversationView::choosePendingStageRows() {
  pendingStructuralCardKeys_.clear();
  if (!pendingStructuralSnapshot_ || pendingLocations_.empty())
    return;

  std::vector<std::string> keys;
  keys.reserve(pendingLocations_.size());
  for (const TurnSection &section : pendingStructuralSnapshot_->sections)
    for (const VisibleCardData &card : section.cards)
      keys.push_back(stableKey(card.key));

  const int viewportRows =
      std::max(MinimumMaterializationRows,
               std::max(1, viewport()->height()) / EstimatedCardHeight + 2);
  const std::size_t budget = static_cast<std::size_t>(viewportRows * 3);
  std::size_t center = keys.empty() ? 0 : keys.size() - 1;
  if (pendingStructuralSnapshot_->threadId == threadId_ &&
      mode_ == Mode::Paused) {
    const Anchor anchor = captureAnchor();
    const auto found = std::ranges::find(keys, anchor.stableKey);
    if (found != keys.end())
      center = static_cast<std::size_t>(std::distance(keys.begin(), found));
  } else if (const auto saved =
                 threadStates_.find(pendingStructuralSnapshot_->threadId);
             saved != threadStates_.end() &&
             saved->second.mode == Mode::Paused) {
    const auto found = std::ranges::find(keys, saved->second.anchor.stableKey);
    if (found != keys.end())
      center = static_cast<std::size_t>(std::distance(keys.begin(), found));
  }
  const std::size_t first = center > budget / 2 ? center - budget / 2 : 0;
  const std::size_t last = std::min(keys.size(), first + budget);
  for (std::size_t index = first; index < last; ++index) {
    const std::string &key = keys[index];
    VisibleCardData *card = pendingCard(key);
    if (!card || !cardVisible(*card))
      continue;
    const auto retained = materializedCards_.find(key);
    if (retained != materializedCards_.end() &&
        retained->second->canApply(*card))
      continue;
    pendingStructuralCardKeys_.push_back(key);
  }
}

VisibleCardData *ConversationView::pendingCard(const std::string &key) {
  if (!pendingStructuralSnapshot_)
    return nullptr;
  const auto found = pendingLocations_.find(key);
  if (found == pendingLocations_.end())
    return nullptr;
  const PendingLocation &location = found->second;
  if (location.section >= pendingStructuralSnapshot_->sections.size())
    return nullptr;
  TurnSection &section = pendingStructuralSnapshot_->sections[location.section];
  return location.card < section.cards.size() ? &section.cards[location.card]
                                              : nullptr;
}

const ConversationView::PendingLocation *
ConversationView::pendingLocation(const std::string &key) const {
  const auto found = pendingLocations_.find(key);
  return found == pendingLocations_.end() ? nullptr : &found->second;
}

void ConversationView::scheduleStructuralStagePass() {
  if (structuralStagePassScheduled_ || !pendingStructuralSnapshot_)
    return;
  structuralStagePassScheduled_ = true;
  QTimer::singleShot(1, Qt::PreciseTimer, this, [this] {
    structuralStagePassScheduled_ = false;
    runStructuralStagePass();
  });
}

void ConversationView::runStructuralStagePass() {
  if (!pendingStructuralSnapshot_)
    return;
  while (pendingStructuralCardIndex_ < pendingStructuralCardKeys_.size()) {
    const std::string key =
        pendingStructuralCardKeys_[pendingStructuralCardIndex_++];
    VisibleCardData *data = pendingCard(key);
    const PendingLocation *location = pendingLocation(key);
    if (!data || !location)
      continue;
    ConversationCard *card = createCard(*data, stagingHost_, key);
    card->setNestedPresentation(location->nested);
    card->setAuthoritativeTurnActive(location->activeTurn);
    const int width = std::max(
        0, viewport()->width() - (location->nested ? 2 * NestedCardIndent : 0));
    const int height = measureCard(card, width);
    card->hide();
    stagedCards_.insert_or_assign(key, card);
    stagedHeights_.insert_or_assign(key, height);
    incrementProperty(this, "structuralStageCardPasses");
    scheduleStructuralStagePass();
    return;
  }

  ConversationSnapshot completed = std::move(*pendingStructuralSnapshot_);
  pendingStructuralSnapshot_.reset();
  pendingStructuralCardKeys_.clear();
  pendingStructuralCardIndex_ = 0;
  pendingLocations_.clear();
  const QScopedValueRollback committing(committingStructuralStage_, true);
  static_cast<void>(reconcileOwned(std::move(completed)));
  for (auto &[key, card] : stagedCards_) {
    static_cast<void>(key);
    delete card;
  }
  stagedCards_.clear();
  stagedHeights_.clear();
  stagingOverlay_->hide();
  incrementProperty(this, "structuralStageCommits");
}

void ConversationView::cancelStructuralStaging() {
  pendingStructuralSnapshot_.reset();
  pendingLocations_.clear();
  pendingStructuralCardKeys_.clear();
  pendingStructuralCardIndex_ = 0;
  for (auto &[key, card] : stagedCards_) {
    static_cast<void>(key);
    delete card;
  }
  stagedCards_.clear();
  stagedHeights_.clear();
  stagingOverlay_->hide();
}

std::optional<PresentationImpact>
ConversationView::applyCardPresentation(const VisibleCardData &card) {
  return applyCardPresentationOwned(VisibleCardData(card));
}

std::optional<PresentationImpact>
ConversationView::applyCardPresentation(VisibleCardData &&card) {
  return applyCardPresentationOwned(std::move(card));
}

std::optional<PresentationImpact>
ConversationView::applyCardPresentationOwned(VisibleCardData card) {
  const std::string key = stableKey(card.key);
  if (VisibleCardData *pending = pendingCard(key);
      pending && pendingStructuralSnapshot_ &&
      card.threadId == pendingStructuralSnapshot_->threadId) {
    const auto staged = stagedCards_.find(key);
    if (staged != stagedCards_.end()) {
      if (staged->second->canApply(card)) {
        const PresentationImpact impact =
            staged->second->applyPresentation(card);
        if (impact == PresentationImpact::GeometryChanged) {
          const PendingLocation *location = pendingLocation(key);
          const int width = std::max(
              0, viewport()->width() -
                     (location && location->nested ? 2 * NestedCardIndent : 0));
          stagedHeights_.insert_or_assign(key,
                                          measureCard(staged->second, width));
        }
      } else {
        delete staged->second;
        stagedCards_.erase(staged);
        stagedHeights_.erase(key);
      }
    }
    *pending = card;
    if (!model_->indexForStableKey(key).isValid())
      return PresentationImpact::None;
  }

  if (card.threadId != threadId_)
    return std::nullopt;
  const QModelIndex index = model_->indexForStableKey(key);
  const ConversationItemModel::Row *before = model_->row(index.row());
  if (!index.isValid() || !before)
    return std::nullopt;
  if (before->card == card)
    return PresentationImpact::None;

  const bool wasPresented = before->presented;
  const bool becomingAuthoritative =
      before->card.kind == CardKind::LocalPrompt &&
      card.kind == CardKind::UserMessage && card.target;
  nodegraph::NodeRef authoritativeTarget =
      becomingAuthoritative ? card.target : nodegraph::NodeRef{};
  ConversationCard *visibleCard = cardForStableKey(key);
  PresentationImpact impact = PresentationImpact::None;
  if (visibleCard) {
    if (!visibleCard->canApply(card))
      return std::nullopt;
    impact = visibleCard->applyPresentation(card);
  }

  const ConversationItemModel::CardUpdateResult result =
      model_->updateCard(std::move(card));
  if (result == ConversationItemModel::CardUpdateResult::Missing ||
      result == ConversationItemModel::CardUpdateResult::Incompatible)
    return std::nullopt;
  if (result == ConversationItemModel::CardUpdateResult::Unchanged)
    return PresentationImpact::None;

  const ConversationItemModel::Row *after = model_->row(index.row());
  const bool presentationChanged = after && after->presented != wasPresented;
  if (presentationChanged) {
    if (after->presented) {
      static_cast<void>(
          heights_.setHeight(static_cast<std::size_t>(index.row()),
                             estimatedCardHeight(after->card) + CardSpacing));
    } else {
      static_cast<void>(
          heights_.setHeight(static_cast<std::size_t>(index.row()), 0));
      if (visibleCard) {
        materializedCards_.erase(key);
        releaseCard(key, visibleCard);
        visibleCard = nullptr;
      }
    }
    updateScrollRange();
    updateMaterialization(true);
  } else if (visibleCard && impact == PresentationImpact::GeometryChanged) {
    const int height = measureCard(visibleCard, rowWidth(*after));
    static_cast<void>(updateMeasuredHeight(index.row(), height, true));
  } else if (visibleCard && impact == PresentationImpact::PaintOnly) {
    visibleCard->update();
  }

  if (visibleCard)
    incrementProperty(this, "targetedVisibleCardUpdates");
  else
    incrementProperty(this, "targetedOffscreenCardUpdates");
  incrementProperty(this, "graphRefreshPasses");
  incrementProperty(this, "targetedCardCommits");
  storeCurrentThreadState();
  if (becomingAuthoritative && promptMaterializedAction_)
    static_cast<void>(
        promptMaterializedAction_(std::move(authoritativeTarget)));
  return impact;
}

int ConversationView::estimatedCardHeight(const VisibleCardData &card) const {
  switch (card.kind) {
  case CardKind::CommandExecution:
    return 156;
  case CardKind::FileChanges:
  case CardKind::ImageGeneration:
  case CardKind::Plan:
    return 136;
  case CardKind::UserMessage:
  case CardKind::LocalPrompt:
    return 92;
  default:
    return EstimatedCardHeight;
  }
}

int ConversationView::rowWidth(const ConversationItemModel::Row &row) const {
  return std::max(0, viewport()->width() -
                         (row.nested ? 2 * NestedCardIndent : 0));
}

void ConversationView::rebuildHeightIndex() {
  std::vector<int> extents;
  extents.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row || !row->presented) {
      extents.push_back(0);
      continue;
    }
    const int width = rowWidth(*row);
    int height = 0;
    if (const auto staged = stagedHeights_.find(row->stableKey);
        staged != stagedHeights_.end()) {
      height = staged->second;
      heightCache_.insert_or_assign(row->stableKey,
                                    HeightRecord{width, height});
    } else if (const auto cached = heightCache_.find(row->stableKey);
               cached != heightCache_.end() && cached->second.width == width) {
      height = cached->second.height;
    } else {
      height = estimatedCardHeight(row->card);
    }
    extents.push_back(std::max(1, height) + CardSpacing);
  }
  heights_.assign(extents);
  setProperty("conversationHeightIndexRebuilds",
              static_cast<qulonglong>(heights_.rebuildCount()));
}

int ConversationView::leadingChromeHeight() const noexcept {
  if (!model_)
    return 0;
  if (model_->hasMore())
    return HistoryButtonHeight + CardSpacing;
  if (model_->rowCount() == 0 && empty_)
    return std::max(28, empty_->sizeHint().height()) + CardSpacing;
  return 0;
}

qint64 ConversationView::naturalContentHeight() const noexcept {
  return static_cast<qint64>(leadingChromeHeight()) + heights_.totalHeight() +
         trailingSpaceHeight_;
}

void ConversationView::updateScrollRange() {
  if (!model_ || !viewport())
    return;
  const int viewportHeight = std::max(0, viewport()->height());
  const qint64 maximum64 =
      std::max<qint64>(0, naturalContentHeight() - viewportHeight);
  const int maximum = static_cast<int>(std::min<qint64>(INT_MAX, maximum64));
  verticalScrollBar()->setPageStep(viewportHeight);
  verticalScrollBar()->setRange(0, maximum);
  horizontalScrollBar()->setPageStep(viewport()->width());
  horizontalScrollBar()->setRange(0, 0);

  const int leading = leadingChromeHeight();
  if (model_->hasMore() && loadMore_) {
    const int width = std::min(std::max(180, loadMore_->sizeHint().width()),
                               std::max(0, viewport()->width()));
    loadMore_->setGeometry(
        (viewport()->width() - width) / 2 - horizontalScrollBar()->value(),
        -verticalScrollBar()->value(), width, HistoryButtonHeight);
  }
  if (model_->rowCount() == 0 && empty_) {
    empty_->setGeometry(0, -verticalScrollBar()->value(),
                        std::max(0, viewport()->width()),
                        std::max(0, leading - CardSpacing));
  }
  if (stagingOverlay_)
    stagingOverlay_->setGeometry(viewport()->rect());
}

QRect ConversationView::rowRect(int rowIndex) const {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !row->presented || rowIndex < 0 ||
      static_cast<std::size_t>(rowIndex) >= heights_.size())
    return {};
  const int extent = heights_.height(static_cast<std::size_t>(rowIndex));
  if (extent <= 0)
    return {};
  const qint64 contentTop = static_cast<qint64>(leadingChromeHeight()) +
                            heights_.top(static_cast<std::size_t>(rowIndex));
  const qint64 viewportTop = contentTop - verticalScrollBar()->value();
  const int x = row->nested ? NestedCardIndent : 0;
  return {x - horizontalScrollBar()->value(),
          static_cast<int>(std::clamp<qint64>(viewportTop, INT_MIN, INT_MAX)),
          rowWidth(*row), std::max(1, extent - CardSpacing)};
}

int ConversationView::measureCard(ConversationCard *card, int width) const {
  if (!card || !card->layout())
    return 0;
  width = std::max(1, width);
  card->setMinimumHeight(0);
  card->setMaximumHeight(QWIDGETSIZE_MAX);
  card->resize(width, std::max(1, card->height()));
  if (QWidget *content =
          card->findChild<QWidget *>(QStringLiteral("conversationCardContent"),
                                     Qt::FindDirectChildrenOnly);
      content && content->layout()) {
    content->layout()->invalidate();
    content->layout()->setGeometry(content->contentsRect());
    content->layout()->activate();
  }
  card->layout()->invalidate();
  card->layout()->setGeometry(card->contentsRect());
  card->layout()->activate();
  const int height =
      card->layout()->hasHeightForWidth()
          ? card->layout()->heightForWidth(width) + 2 * card->frameWidth()
          : card->sizeHint().height();
  card->setFixedHeight(std::max(1, height));
  card->resize(width, std::max(1, height));
  card->layout()->setGeometry(card->contentsRect());
  card->layout()->activate();
  QCoreApplication::removePostedEvents(card, QEvent::LayoutRequest);
  return std::max(1, height);
}

bool ConversationView::updateMeasuredHeight(int rowIndex, int cardHeight,
                                            bool preserveAnchor) {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !row->presented)
    return false;
  const Anchor anchor = preserveAnchor ? captureAnchor() : Anchor{};
  const bool follow = mode_ == Mode::Following;
  heightCache_.insert_or_assign(row->stableKey,
                                HeightRecord{rowWidth(*row), cardHeight});
  if (!heights_.setHeight(static_cast<std::size_t>(rowIndex),
                          std::max(1, cardHeight) + CardSpacing))
    return false;
  incrementProperty(this, "conversationLocalGeometryPasses");
  setProperty("conversationHeightIndexUpdateSteps",
              static_cast<qulonglong>(heights_.lastUpdateSteps()));
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else if (preserveAnchor)
    restoreAnchor(anchor);
  layoutMaterializedCards();
  return true;
}

std::pair<int, int> ConversationView::materializationRows() const {
  if (model_->rowCount() == 0 || heights_.empty() ||
      heights_.totalHeight() <= 0)
    return {-1, -1};
  const qint64 scroll = verticalScrollBar()->value();
  const qint64 viewportHeight = std::max(1, viewport()->height());
  const qint64 contentStart =
      std::max<qint64>(0, scroll - leadingChromeHeight() - viewportHeight);
  const qint64 contentEnd =
      std::min<qint64>(heights_.totalHeight() - 1,
                       scroll - leadingChromeHeight() + 2 * viewportHeight);
  if (contentEnd < contentStart)
    return {-1, -1};
  const int first = static_cast<int>(heights_.rowAt(contentStart));
  const int last = static_cast<int>(heights_.rowAt(contentEnd));
  return {std::max(0, first), std::min(model_->rowCount() - 1, last)};
}

ConversationCard *ConversationView::createCard(const VisibleCardData &data,
                                               QWidget *parent,
                                               const std::string &key) {
  ConversationCard *card = createConversationCard(
      data, parent, !presentationOptions_.commandsInitiallyExpanded,
      !presentationOptions_.imagesInitiallyExpanded,
      !presentationOptions_.fileChangesInitiallyExpanded);
  card->setProperty("conversationAnchorKey", QString::fromStdString(key));
  if (const auto collapsed = cardCollapsedStates_.find(key);
      collapsed != cardCollapsedStates_.end())
    card->setCollapsed(collapsed->second);
  if (const auto output = commandOutputStates_.find(key);
      output != commandOutputStates_.end())
    card->restoreCommandOutputScrollState(output->second);
  card->installEventFilter(this);
  for (QWidget *child : card->findChildren<QWidget *>())
    child->installEventFilter(this);
  connect(card, &ConversationCard::foldRequested, this,
          [this, key, card](bool collapsed) {
            const auto retained = materializedCards_.find(key);
            if (retained != materializedCards_.end() &&
                retained->second == card)
              setCardCollapsed(key, card, collapsed);
          });
  connect(card, &ConversationCard::recoveryRequested, this, [this, key, card] {
    const auto retained = materializedCards_.find(key);
    if (retained == materializedCards_.end() || retained->second != card ||
        !promptRecoveryAction_ || !card->data().target)
      return;
    promptRecoveryAction_(card->data().target);
  });
  return card;
}

void ConversationView::configureCardForRow(
    ConversationCard *card, const ConversationItemModel::Row &row) {
  if (!card)
    return;
  card->setProperty("turnContainer", row.turnRoot);
  card->setNestedCards({});
  card->setNestedPresentation(row.nested);
  card->setAuthoritativeTurnActive(row.turnRoot && row.activeTurn);
}

ConversationCard *ConversationView::materializeRow(int rowIndex) {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !row->presented)
    return nullptr;
  if (ConversationCard *retained = cardForStableKey(row->stableKey))
    return retained;

  ConversationCard *card = nullptr;
  if (const auto staged = stagedCards_.find(row->stableKey);
      staged != stagedCards_.end() && staged->second->canApply(row->card)) {
    card = staged->second;
    stagedCards_.erase(staged);
    stagedHeights_.erase(row->stableKey);
  } else {
    card = createCard(row->card, stagingHost_, row->stableKey);
    incrementProperty(this, "conversationCardConstructions");
  }
  card->hide();
  card->setParent(viewport());
  configureCardForRow(card, *row);
  const int height = measureCard(card, rowWidth(*row));
  heightCache_.insert_or_assign(row->stableKey,
                                HeightRecord{rowWidth(*row), height});
  static_cast<void>(heights_.setHeight(static_cast<std::size_t>(rowIndex),
                                       height + CardSpacing));
  materializedCards_.emplace(row->stableKey, card);
  card->setGeometry(rowRect(rowIndex));
  card->show();
  incrementProperty(this, "conversationRowsMaterialized");
  return card;
}

void ConversationView::releaseCard(const std::string &key,
                                   ConversationCard *card) {
  if (!card)
    return;
  cardCollapsedStates_.insert_or_assign(key, card->isCollapsed());
  if (const auto state = card->commandOutputScrollState())
    commandOutputStates_.insert_or_assign(key, *state);
  card->setViewportVisible(false);
  delete card;
  incrementProperty(this, "conversationRowsReleased");
}

void ConversationView::releaseUnneededCards(int firstRow, int lastRow) {
  std::unordered_set<std::string> retainedKeys;
  if (firstRow >= 0 && lastRow >= firstRow) {
    retainedKeys.reserve(static_cast<std::size_t>(lastRow - firstRow + 1));
    for (int rowIndex = firstRow; rowIndex <= lastRow; ++rowIndex)
      if (const auto *row = model_->row(rowIndex); row && row->presented)
        retainedKeys.insert(row->stableKey);
  }

  QWidget *focused = QApplication::focusWidget();
  std::vector<std::string> removeKeys;
  for (const auto &[key, card] : materializedCards_) {
    const bool ownsFocus =
        focused && (focused == card || card->isAncestorOf(focused));
    if (!retainedKeys.contains(key) && !ownsFocus)
      removeKeys.push_back(key);
  }
  for (const std::string &key : removeKeys) {
    const auto found = materializedCards_.find(key);
    if (found == materializedCards_.end())
      continue;
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    releaseCard(key, card);
  }
}

void ConversationView::releaseAllCards() {
  for (auto &[key, card] : materializedCards_)
    releaseCard(key, card);
  materializedCards_.clear();
  updateMaterializationProperties();
}

void ConversationView::updateMaterialization(bool preserveAnchor) {
  if (materializing_)
    return;
  const QScopedValueRollback materializing(materializing_, true);
  const Anchor anchor = preserveAnchor ? captureAnchor() : Anchor{};
  const bool follow = mode_ == Mode::Following;

  for (int pass = 0; pass < 2; ++pass) {
    const auto [first, last] = materializationRows();
    const qint64 totalBefore = heights_.totalHeight();
    if (first >= 0)
      for (int rowIndex = first; rowIndex <= last; ++rowIndex)
        static_cast<void>(materializeRow(rowIndex));
    if (heights_.totalHeight() == totalBefore)
      break;
    updateScrollRange();
    if (follow)
      setScrollValue(verticalScrollBar()->maximum());
    else if (preserveAnchor)
      restoreAnchor(anchor);
  }
  const auto [first, last] = materializationRows();
  releaseUnneededCards(first, last);
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else if (preserveAnchor)
    restoreAnchor(anchor);
  layoutMaterializedCards();
  updateMaterializationProperties();
}

void ConversationView::layoutMaterializedCards() {
  const QRect visibleRect = viewport()->rect();
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    if (!index.isValid())
      continue;
    const QRect geometry = rowRect(index.row());
    card->setGeometry(geometry);
    card->setViewportVisible(geometry.intersects(visibleRect));
  }
}

void ConversationView::updateMaterializationProperties() {
  const qulonglong count = static_cast<qulonglong>(materializedCards_.size());
  setProperty("conversationMaterializedCardCount", count);
  setProperty(
      "conversationMaterializedCardPeak",
      std::max(property("conversationMaterializedCardPeak").toULongLong(),
               count));
}

ConversationCard *
ConversationView::cardForStableKey(const std::string &key) const {
  const auto found = materializedCards_.find(key);
  return found == materializedCards_.end() ? nullptr : found->second;
}

void ConversationView::setCardCollapsed(const std::string &key,
                                        ConversationCard *card,
                                        bool collapsed) {
  if (!card || card->isCollapsed() == collapsed)
    return;
  const QModelIndex index = model_->indexForStableKey(key);
  if (!index.isValid())
    return;
  Anchor anchor = captureAnchor();
  anchor.stableKey = key;
  anchor.pixelOffset = rowRect(index.row()).top();
  mode_ = Mode::Paused;
  pausedByComposerGrowth_ = false;
  stopFollowingAnimation();
  cardCollapsedStates_.insert_or_assign(key, collapsed);
  card->setCollapsed(collapsed);
  const ConversationItemModel::Row *row = model_->row(index.row());
  const int height = measureCard(card, rowWidth(*row));
  static_cast<void>(updateMeasuredHeight(index.row(), height, false));
  restoreAnchor(anchor);
  layoutMaterializedCards();
  storeCurrentThreadState();
}

void ConversationView::setTrailingSpaceHeight(int height) {
  height = std::max(0, height);
  if (height == trailingSpaceHeight_)
    return;
  const bool grew = height > trailingSpaceHeight_;
  const Anchor anchor = captureAnchor();
  const int previousValue = verticalScrollBar()->value();
  stopFollowingAnimation();
  if (grew) {
    pausedByComposerGrowth_ =
        pausedByComposerGrowth_ || mode_ == Mode::Following;
    mode_ = Mode::Paused;
  }
  trailingSpaceHeight_ = height;
  verticalScrollBar()->setProperty("composerBottomInset", height);
  verticalScrollBar()->setStyleSheet(
      height == 0
          ? QString{}
          : QStringLiteral("QScrollBar:vertical{margin:2px 2px %1px 2px;}")
                .arg(height + 2));
  updateScrollRange();
  if (mode_ == Mode::Following)
    setScrollValue(verticalScrollBar()->maximum());
  else if (grew && anchor.stableKey.empty())
    setScrollValue(std::min(previousValue, verticalScrollBar()->maximum()));
  else
    restoreAnchor(anchor);
  if (!grew && isAtBottom()) {
    mode_ = Mode::Following;
    pausedByComposerGrowth_ = false;
  }
  layoutMaterializedCards();
  storeCurrentThreadState();
}

void ConversationView::prepareForLocalPromptAdmission() {
  if (mode_ != Mode::Paused || !pausedByComposerGrowth_)
    return;
  mode_ = Mode::Following;
  pausedByComposerGrowth_ = false;
  storeCurrentThreadState();
}

bool ConversationView::forwardWheelEvent(QWheelEvent *event) {
  return event && applyWheel(event);
}

bool ConversationView::isAtBottom() const noexcept {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
}

ConversationView::Mode
ConversationView::modeForThread(const std::string &threadId) const noexcept {
  if (threadId == threadId_)
    return mode_;
  const auto saved = threadStates_.find(threadId);
  return saved == threadStates_.end() ? Mode::Following : saved->second.mode;
}

ConversationView::Anchor ConversationView::captureAnchor() const {
  Anchor anchor;
  anchor.absoluteValue = verticalScrollBar()->value();
  anchor.horizontalValue = horizontalScrollBar()->value();
  if (model_->rowCount() == 0 || heights_.empty() ||
      heights_.totalHeight() <= 0)
    return anchor;
  const qint64 contentY =
      std::max<qint64>(0, static_cast<qint64>(verticalScrollBar()->value()) -
                              leadingChromeHeight());
  std::size_t rowIndex = heights_.rowAt(contentY);
  while (rowIndex < heights_.size() && heights_.height(rowIndex) == 0)
    ++rowIndex;
  if (rowIndex >= heights_.size())
    return anchor;
  const ConversationItemModel::Row *row =
      model_->row(static_cast<int>(rowIndex));
  if (!row)
    return anchor;
  anchor.stableKey = row->stableKey;
  anchor.pixelOffset = rowRect(static_cast<int>(rowIndex)).top();
  return anchor;
}

void ConversationView::restoreAnchor(const Anchor &anchor) {
  int value = anchor.absoluteValue;
  if (!anchor.stableKey.empty()) {
    const QModelIndex index = model_->indexForStableKey(anchor.stableKey);
    if (index.isValid()) {
      const qint64 top = static_cast<qint64>(leadingChromeHeight()) +
                         heights_.top(static_cast<std::size_t>(index.row()));
      value = static_cast<int>(std::clamp<qint64>(
          top - anchor.pixelOffset, verticalScrollBar()->minimum(),
          verticalScrollBar()->maximum()));
    }
  }
  setScrollValue(value);
  horizontalScrollBar()->setValue(std::clamp(anchor.horizontalValue,
                                             horizontalScrollBar()->minimum(),
                                             horizontalScrollBar()->maximum()));
}

void ConversationView::setScrollValue(int value) {
  value = std::clamp(value, verticalScrollBar()->minimum(),
                     verticalScrollBar()->maximum());
  const QScopedValueRollback programmatic(programmaticScroll_, true);
  verticalScrollBar()->setValue(value);
  layoutMaterializedCards();
}

void ConversationView::stopFollowingAnimation() {
  if (followAnimation_->state() != QAbstractAnimation::Stopped)
    followAnimation_->stop();
}

void ConversationView::animateToBottom(int previousValue) {
  if (mode_ != Mode::Following)
    return;
  const int destination = verticalScrollBar()->maximum();
  const int start =
      std::clamp(std::max(verticalScrollBar()->value(), previousValue),
                 verticalScrollBar()->minimum(), destination);
  const int distance = destination - start;
  stopFollowingAnimation();
  if (distance <= 3) {
    setScrollValue(destination);
    return;
  }
  setScrollValue(start);
  followAnimation_->setDuration(std::clamp(110 + distance / 3, 130, 260));
  followAnimation_->setStartValue(start);
  followAnimation_->setEndValue(destination);
  followAnimation_->start();
}

void ConversationView::handleUserScrollValue(int value) {
  stopFollowingAnimation();
  pausedByComposerGrowth_ = false;
  mode_ = value >= verticalScrollBar()->maximum() - 1 ? Mode::Following
                                                      : Mode::Paused;
  storeCurrentThreadState();
}

bool ConversationView::applyWheel(QWheelEvent *event) {
  if (!event)
    return false;
  const int intent = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                   : event->angleDelta().y();
  if (intent == 0)
    return false;
  pausedByComposerGrowth_ = false;
  const int oldValue = verticalScrollBar()->value();
  if (intent > 0) {
    stopFollowingAnimation();
    mode_ = Mode::Paused;
  }
  QScrollBar *bar = verticalScrollBar();
  const QPointF local = bar->mapFromGlobal(event->globalPosition().toPoint());
  QWheelEvent forwarded(local, event->globalPosition(), event->pixelDelta(),
                        event->angleDelta(), event->buttons(),
                        event->modifiers(), event->phase(), event->inverted());
  const QScopedValueRollback nativeDispatch(dispatchingNativeWheel_, true);
  QApplication::sendEvent(bar, &forwarded);
  if (verticalScrollBar()->value() < oldValue)
    mode_ = Mode::Paused;
  if (isAtBottom())
    mode_ = Mode::Following;
  updateMaterialization(false);
  storeCurrentThreadState();
  event->accept();
  return true;
}

QRect ConversationView::visualRect(const QModelIndex &index) const {
  if (!index.isValid() || index.model() != model_ || index.column() != 0)
    return {};
  return rowRect(index.row());
}

void ConversationView::scrollTo(const QModelIndex &index, ScrollHint hint) {
  const QRect geometry = visualRect(index);
  if (geometry.isEmpty())
    return;
  int target = verticalScrollBar()->value();
  if (hint == PositionAtTop)
    target += geometry.top();
  else if (hint == PositionAtBottom)
    target += geometry.bottom() - viewport()->height() + 1;
  else if (hint == PositionAtCenter)
    target += geometry.center().y() - viewport()->height() / 2;
  else if (geometry.top() < 0)
    target += geometry.top();
  else if (geometry.bottom() >= viewport()->height())
    target += geometry.bottom() - viewport()->height() + 1;
  setScrollValue(target);
  handleUserScrollValue(verticalScrollBar()->value());
  updateMaterialization(false);
}

QModelIndex ConversationView::indexAt(const QPoint &point) const {
  if (!viewport()->rect().contains(point) || heights_.empty())
    return {};
  const qint64 contentY = static_cast<qint64>(point.y()) +
                          verticalScrollBar()->value() - leadingChromeHeight();
  if (contentY < 0 || contentY >= heights_.totalHeight())
    return {};
  const int rowIndex = static_cast<int>(heights_.rowAt(contentY));
  const QRect geometry = rowRect(rowIndex);
  return geometry.contains(point) ? model_->index(rowIndex) : QModelIndex{};
}

QModelIndex ConversationView::moveCursor(CursorAction cursorAction,
                                         Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  int row = currentIndex().isValid() ? currentIndex().row() : -1;
  const int count = model_->rowCount();
  int direction = 0;
  switch (cursorAction) {
  case MoveUp:
  case MovePrevious:
    direction = -1;
    break;
  case MoveDown:
  case MoveNext:
    direction = 1;
    break;
  case MoveHome:
    row = 0;
    direction = 1;
    break;
  case MoveEnd:
    row = count - 1;
    direction = -1;
    break;
  case MovePageUp:
  case MovePageDown: {
    const int y = cursorAction == MovePageUp ? 0 : viewport()->height() - 1;
    const QModelIndex page = indexAt(QPoint(viewport()->width() / 2, y));
    if (page.isValid())
      row = page.row();
    direction = cursorAction == MovePageUp ? -1 : 1;
    break;
  }
  default:
    return currentIndex();
  }
  if (row < 0)
    row = direction < 0 ? count - 1 : 0;
  while (row >= 0 && row < count) {
    if (!isIndexHidden(model_->index(row)))
      return model_->index(row);
    row += direction;
  }
  return currentIndex();
}

int ConversationView::horizontalOffset() const {
  return horizontalScrollBar()->value();
}

int ConversationView::verticalOffset() const {
  return verticalScrollBar()->value();
}

bool ConversationView::isIndexHidden(const QModelIndex &index) const {
  const ConversationItemModel::Row *row = model_->row(index.row());
  return !row || !row->presented;
}

void ConversationView::setSelection(
    const QRect &rect, QItemSelectionModel::SelectionFlags command) {
  if (!selectionModel())
    return;
  const QModelIndex topLeft = indexAt(rect.topLeft());
  const QModelIndex bottomRight = indexAt(rect.bottomRight());
  if (!topLeft.isValid() || !bottomRight.isValid())
    return;
  selectionModel()->select(
      QItemSelection(model_->index(std::min(topLeft.row(), bottomRight.row())),
                     model_->index(std::max(topLeft.row(), bottomRight.row()))),
      command);
}

QRegion ConversationView::visualRegionForSelection(
    const QItemSelection &selection) const {
  QRegion region;
  for (const QItemSelectionRange &range : selection)
    for (int row = range.top(); row <= range.bottom(); ++row)
      region += visualRect(model_->index(row));
  return region;
}

void ConversationView::updateGeometries() {
  if (applying_)
    return;
  updateScrollRange();
  layoutMaterializedCards();
}

void ConversationView::scrollContentsBy(int dx, int dy) {
  static_cast<void>(dx);
  static_cast<void>(dy);
  if (!materializing_)
    updateMaterialization(false);
  layoutMaterializedCards();
  viewport()->update();
}

bool ConversationView::eventFilter(QObject *watched, QEvent *event) {
  auto *widget = qobject_cast<QWidget *>(watched);
  ConversationCard *card = nullptr;
  for (QWidget *candidate = widget; candidate && candidate != viewport();
       candidate = candidate->parentWidget()) {
    if ((card = qobject_cast<ConversationCard *>(candidate)))
      break;
  }
  if (card && event->type() == QEvent::FocusIn) {
    const std::string key =
        card->property("conversationAnchorKey").toString().toStdString();
    const QModelIndex index = model_->indexForStableKey(key);
    if (index.isValid())
      setCurrentIndex(index);
  }
  if (card && event->type() == QEvent::LayoutRequest && !applying_ &&
      !materializing_) {
    const std::string key =
        card->property("conversationAnchorKey").toString().toStdString();
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (index.isValid() && row && cardForStableKey(key) == card) {
      const int height = measureCard(card, rowWidth(*row));
      static_cast<void>(updateMeasuredHeight(index.row(), height, true));
      return true;
    }
  }
  return QAbstractItemView::eventFilter(watched, event);
}

void ConversationView::paintEvent(QPaintEvent *event) {
  QPainter painter(viewport());
  painter.setClipRegion(event->region());
  if (hasFocus() && currentIndex().isValid()) {
    QStyleOptionFocusRect option;
    option.initFrom(this);
    option.rect = visualRect(currentIndex()).adjusted(1, 1, -1, -1);
    option.state |= QStyle::State_KeyboardFocusChange;
    style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
  }
}

void ConversationView::resizeEvent(QResizeEvent *event) {
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  stopFollowingAnimation();
  QAbstractItemView::resizeEvent(event);
  stagingHost_->resize(viewport()->size());
  stagingOverlay_->setGeometry(viewport()->rect());
  rebuildHeightIndex();
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row)
      continue;
    const int height = measureCard(card, rowWidth(*row));
    heightCache_.insert_or_assign(key, HeightRecord{rowWidth(*row), height});
    static_cast<void>(heights_.setHeight(static_cast<std::size_t>(index.row()),
                                         height + CardSpacing));
  }
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  updateMaterialization(false);
  viewport()->update();
  storeCurrentThreadState();
}

void ConversationView::wheelEvent(QWheelEvent *event) {
  if (!applyWheel(event))
    QAbstractItemView::wheelEvent(event);
}

} // namespace codexui::codex::middle
