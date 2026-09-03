// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationView.h"

#include "codex/middle/ConversationCards.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int CardSpacing = 8;
constexpr int NativeScrollLineStep = 20;
constexpr int MaxCardOperationsPerPass = 8;

class MeasuredCardPlaceholder final : public QWidget {
public:
  MeasuredCardPlaceholder(const std::string &key, int height, QWidget *parent)
      : QWidget(parent) {
    setObjectName(QStringLiteral("conversationCardPlaceholder"));
    setProperty("conversationAnchorKey", QString::fromStdString(key));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMeasuredHeight(height);
  }

  void setMeasuredHeight(int height) {
    height_ = std::max(0, height);
    setFixedHeight(height_);
  }

private:
  int height_ = 0;
};

int initialCardHeight(const VisibleCardData &card) {
  switch (card.kind) {
  case CardKind::UserMessage:
  case CardKind::AgentMessage:
  case CardKind::LocalPrompt:
    return 88;
  case CardKind::CommandExecution:
  case CardKind::AgentActivity:
  case CardKind::Reasoning:
  case CardKind::FileChanges:
  case CardKind::ImageGeneration:
  case CardKind::Plan:
  case CardKind::GenericActivity:
    return 58;
  }
  return 72;
}

QLabel *makeEmptyLabel() {
  auto *label =
      new QLabel(QStringLiteral("Conversation activity appears here."));
  label->setProperty("kind", "muted");
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}

} // namespace

class ConversationView::TurnSectionWidget final : public QWidget {
public:
  struct CardSlot {
    std::string key;
    QWidget *item = nullptr;
    int measuredHeight = 0;
    bool projectionVisible = true;
    bool newlyAdded = false;
  };

  explicit TurnSectionWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    cards = new QVBoxLayout(this);
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(CardSpacing);
  }

  QVBoxLayout *cards = nullptr;
  std::vector<CardSlot> cardSlots;
  std::string rootKey;
};

ConversationView::ConversationView(QWidget *parent)
    : QAbstractScrollArea(parent) {
  setObjectName(QStringLiteral("conversationScroll"));
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
  verticalScrollBar()->setSingleStep(NativeScrollLineStep);
  viewport()->setAutoFillBackground(false);

  content_ = new QWidget(viewport());
  content_->setObjectName(QStringLiteral("conversationContent"));
  content_->setAttribute(Qt::WA_StyledBackground, false);
  content_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  content_->installEventFilter(this);

  contentLayout_ = new QVBoxLayout(content_);
  contentLayout_->setContentsMargins(0, 0, 0, 0);
  contentLayout_->setSpacing(CardSpacing);
  contentLayout_->setAlignment(Qt::AlignTop);

  loadMore_ = new QPushButton(QStringLiteral("Load more activities"), content_);
  loadMore_->setProperty("kind", "history");
  loadMore_->setFixedHeight(32);
  loadMore_->hide();
  connect(loadMore_, &QPushButton::clicked, this, [this] {
    if (loadMoreAction_)
      loadMoreAction_();
  });
  contentLayout_->addWidget(loadMore_, 0, Qt::AlignHCenter);

  empty_ = makeEmptyLabel();
  emptyMessage_ = empty_->text();
  empty_->setParent(content_);
  contentLayout_->addWidget(empty_);
  trailingSpace_ =
      new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
  contentLayout_->addItem(trailingSpace_);

  followAnimation_ = new QVariantAnimation(this);
  followAnimation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(followAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            if (mode_ != Mode::Following || applying_) {
              followAnimation_->stop();
              return;
            }
            // Never let a retargeted animation move an already-following view
            // backwards.
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
                action == QAbstractSlider::SliderToMinimum) {
              mode_ = Mode::Paused;
            }
          });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            positionContent();
            scheduleVisibilityPass();
            if (programmaticScroll_ || applying_)
              return;
            if (sliderDown_ || userActionPending_) {
              handleUserScrollValue(value);
            }
            userActionPending_ = false;
          });

  recomputeGeometry();
}

void ConversationView::setLoadMoreAction(std::function<void()> action) {
  loadMoreAction_ = std::move(action);
}

void ConversationView::setEmptyMessage(QString message) {
  if (message == emptyMessage_)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  emptyMessage_ = std::move(message);
  empty_->setText(emptyMessage_);
  recomputeGeometry();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  applying_ = false;
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
}

void ConversationView::setPresentationOptions(PresentationOptions options) {
  if (presentationOptions_ == options)
    return;
  presentationOptions_ = options;
  static_cast<void>(reconcile(snapshot_, true, true));
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
  return reconcile(snapshot, false, false);
}

bool ConversationView::reconcile(const ConversationSnapshot &snapshot,
                                 bool force, bool settleFollowImmediately) {
  if (!force && snapshot == snapshot_ && snapshot.threadId == threadId_)
    return false;

  const bool switchedThread = snapshot.threadId != threadId_;
  if (switchedThread)
    setThread(snapshot.threadId);

  Anchor anchor = captureAnchor();
  if (switchedThread) {
    const auto saved = threadStates_.find(threadId_);
    if (saved != threadStates_.end()) {
      mode_ = saved->second.mode;
      anchor = saved->second.anchor;
    } else {
      mode_ = Mode::Following;
      pausedByComposerGrowth_ = false;
      anchor = {};
    }
  }
  const bool follow = mode_ == Mode::Following;
  const auto visibleOutputFootprint = [this] {
    int height = 0;
    for (const auto &[key, card] : cards_) {
      static_cast<void>(key);
      auto *output = dynamic_cast<CommandOutputView *>(
          card->findChild<QTextEdit *>(QStringLiteral("commandOutputView")));
      if (output && output->isVisibleTo(card))
        height += output->height();
    }
    return height;
  };
  const int outputFootprintBefore = visibleOutputFootprint();

  stopFollowingAnimation();
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  bool visualChange = switchedThread || snapshot != snapshot_;
  const bool showLoadMore = snapshot.hasMore;
  if (loadMore_->isVisible() != showLoadMore) {
    loadMore_->setVisible(showLoadMore);
    visualChange = true;
  }
  if (showLoadMore) {
    const std::size_t page = std::min(AuthoritativeHistoryPageSize,
                                      snapshot.hiddenAuthoritativeItemCount);
    const QString label = QStringLiteral("Load %1 more activities")
                              .arg(static_cast<qulonglong>(page));
    if (loadMore_->text() != label) {
      loadMore_->setText(label);
      visualChange = true;
    }
    loadMore_->setToolTip(QStringLiteral("%1 earlier activities are retained")
                              .arg(static_cast<qulonglong>(
                                  snapshot.hiddenAuthoritativeItemCount)));
  }

  struct DesiredSection {
    TurnSectionWidget *widget = nullptr;
  };
  std::unordered_map<std::string, DesiredSection> desiredSections;
  std::vector<std::string> desiredSectionKeys;
  desiredSectionKeys.reserve(snapshot.sections.size());
  std::vector<std::string> displayedKeys;
  const auto retainCommandOutputState = [this](const std::string &key,
                                               ConversationCard *card) {
    const auto state = card ? card->commandOutputScrollState() : std::nullopt;
    if (state && !state->followsLatest)
      commandOutputStates_[key] = *state;
    else
      commandOutputStates_.erase(key);
  };

  std::unordered_map<std::string, TurnSectionWidget::CardSlot> retainedSlots;
  for (const auto &[sectionKey, section] : sections_) {
    static_cast<void>(sectionKey);
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
      retainedSlots.emplace(slot.key, std::move(slot));
    section->cardSlots.clear();
  }

  std::size_t newCardCount = 0;
  for (const TurnSection &section : snapshot.sections)
    for (const VisibleCardData &card : section.cards)
      if (!retainedSlots.contains(stableKey(card.key)))
        ++newCardCount;
  const bool smallIncrementalAppend =
      !switchedThread && newCardCount > 0 &&
      newCardCount <= static_cast<std::size_t>(MaxCardOperationsPerPass);

  for (const TurnSection &sectionData : snapshot.sections) {
    desiredSectionKeys.push_back(sectionData.key);
    TurnSectionWidget *section = nullptr;
    const auto existingSection = sections_.find(sectionData.key);
    const bool newSection = existingSection == sections_.end();
    if (newSection) {
      section = new TurnSectionWidget(content_);
      section->setProperty("turnSectionKey",
                           QString::fromStdString(sectionData.key));
      sections_.emplace(sectionData.key, section);
      visualChange = true;
    } else {
      section = existingSection->second;
    }
    section->setProperty("turnId", QString::fromStdString(sectionData.turnId));
    section->rootKey =
        sectionData.rootCardKey ? stableKey(*sectionData.rootCardKey) : "";
    section->cardSlots.reserve(sectionData.cards.size());
    for (const VisibleCardData &cardData : sectionData.cards) {
      const std::string key = stableKey(cardData.key);
      TurnSectionWidget::CardSlot slot;
      if (auto retained = retainedSlots.find(key);
          retained != retainedSlots.end()) {
        slot = std::move(retained->second);
        retainedSlots.erase(retained);
      } else {
        slot.key = key;
        slot.measuredHeight = initialCardHeight(cardData);
        slot.item =
            new MeasuredCardPlaceholder(key, slot.measuredHeight, section);
        slot.newlyAdded = smallIncrementalAppend;
        visualChange = true;
      }
      slot.projectionVisible = cardVisible(cardData);
      if (auto *placeholder =
              dynamic_cast<MeasuredCardPlaceholder *>(slot.item))
        placeholder->setMeasuredHeight(
            slot.projectionVisible ? slot.measuredHeight : 0);
      if (slot.item->isHidden() == slot.projectionVisible)
        slot.item->setVisible(slot.projectionVisible);
      section->cardSlots.push_back(std::move(slot));
    }
    desiredSections.emplace(sectionData.key, DesiredSection{section});

    const auto appendDisplayed = [&](const std::string &key) {
      const auto cardData = std::ranges::find_if(
          sectionData.cards, [&key](const VisibleCardData &candidate) {
            return stableKey(candidate.key) == key;
          });
      if (cardData != sectionData.cards.end() && cardVisible(*cardData))
        displayedKeys.push_back(key);
    };
    if (!section->rootKey.empty())
      appendDisplayed(section->rootKey);
    for (const VisibleCardData &cardData : sectionData.cards) {
      const std::string key = stableKey(cardData.key);
      if (key != section->rootKey && cardVisible(cardData))
        displayedKeys.push_back(key);
    }
  }
  const bool appendedVisibleCards =
      displayedKeys.size() > displayedCardKeys_.size() &&
      std::equal(displayedCardKeys_.begin(), displayedCardKeys_.end(),
                 displayedKeys.begin());
  visualChange = visualChange || displayedKeys != displayedCardKeys_ ||
                 desiredSectionKeys != displayedSectionKeys_;

  for (const std::string &key : desiredSectionKeys) {
    TurnSectionWidget *section = desiredSections.at(key).widget;
    arrangeSection(section);
    const bool sectionVisible = std::ranges::any_of(
        section->cardSlots, [](const TurnSectionWidget::CardSlot &slot) {
          return slot.projectionVisible;
        });
    if (section->isHidden() == sectionVisible) {
      section->setVisible(sectionVisible);
      visualChange = true;
    }
  }

  // Desired layouts have now pulled retained items out of obsolete roots.
  // Save the only widget-local state that survives materialization and release
  // before deleting whatever remains outside the new projection.
  for (auto &[key, slot] : retainedSlots) {
    if (auto *card = dynamic_cast<ConversationCard *>(slot.item)) {
      retainCommandOutputState(key, card);
      cards_.erase(key);
    }
    delete slot.item;
    visualChange = true;
  }

  for (const auto &[key, section] : sections_) {
    static_cast<void>(key);
    contentLayout_->removeWidget(section);
  }
  for (std::size_t index = 0; index < desiredSectionKeys.size(); ++index)
    contentLayout_->insertWidget(
        1 + static_cast<int>(index),
        desiredSections.at(desiredSectionKeys[index]).widget);

  for (auto iterator = sections_.begin(); iterator != sections_.end();) {
    if (desiredSections.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    delete iterator->second;
    iterator = sections_.erase(iterator);
    visualChange = true;
  }
  displayedSectionKeys_ = std::move(desiredSectionKeys);

  const bool empty = displayedKeys.empty();
  if (empty_->isVisible() != empty) {
    empty_->setVisible(empty);
    visualChange = true;
  }
  displayedCardKeys_ = std::move(displayedKeys);
  snapshot_ = snapshot;

  recomputeGeometry();
  if (follow) {
    if (switchedThread || appendedVisibleCards || settleFollowImmediately) {
      setScrollValue(verticalScrollBar()->maximum());
    } else {
      // Reflow above the viewport must preserve the same painted card/pixel
      // first. Smooth following starts only after that stable transaction.
      restoreAnchor(anchor);
    }
  } else {
    restoreAnchor(anchor);
  }
  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  const bool materializationChange = runVisibilityPass();
  const bool outputGrew = visibleOutputFootprint() > outputFootprintBefore;
  if (follow && outputGrew)
    setScrollValue(verticalScrollBar()->maximum());
  viewport()->update();

  if (follow && !switchedThread && !outputGrew && !appendedVisibleCards &&
      !settleFollowImmediately) {
    const int stableValue = verticalScrollBar()->value();
    if (verticalScrollBar()->maximum() > stableValue + 3)
      animateToBottom(stableValue);
    else
      setScrollValue(verticalScrollBar()->maximum());
  }
  storeCurrentThreadState();
  return visualChange || materializationChange;
}

void ConversationView::arrangeSection(TurnSectionWidget *section) {
  if (!section)
    return;

  TurnSectionWidget::CardSlot *rootSlot = nullptr;
  if (!section->rootKey.empty()) {
    const auto root = std::ranges::find_if(
        section->cardSlots, [section](const TurnSectionWidget::CardSlot &slot) {
          return slot.key == section->rootKey;
        });
    if (root != section->cardSlots.end())
      rootSlot = &*root;
  }
  auto *prompt =
      rootSlot ? dynamic_cast<ConversationCard *>(rootSlot->item) : nullptr;

  std::unordered_set<ConversationCard *> obsoleteOwners;
  for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
    for (QWidget *parent = slot.item->parentWidget(); parent;
         parent = parent->parentWidget()) {
      auto *owner = dynamic_cast<ConversationCard *>(parent);
      if (!owner)
        continue;
      if (owner != prompt)
        obsoleteOwners.insert(owner);
      break;
    }
  }
  for (ConversationCard *owner : obsoleteOwners)
    owner->setNestedItems({});

  // A formerly materialized root can be replaced by its placeholder in the
  // same pass. Ensure no other card keeps an obsolete nested layout first.
  for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
    auto *card = dynamic_cast<ConversationCard *>(slot.item);
    if (!card || card == prompt)
      continue;
    if (card->property("turnContainer").toBool())
      card->setNestedItems({});
    card->setProperty("turnContainer", false);
    card->setAuthoritativeTurnActive(false);
  }

  if (prompt) {
    std::vector<QWidget *> nestedItems;
    nestedItems.reserve(section->cardSlots.size() - 1);
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
      if (&slot != rootSlot)
        nestedItems.push_back(slot.item);
    prompt->setProperty("nestedConversationCard", false);
    prompt->setProperty("turnContainer", true);
    prompt->setNestedItems(nestedItems);
    const bool active = snapshot_.activeTurnId &&
                        section->property("turnId").toString().toStdString() ==
                            *snapshot_.activeTurnId;
    prompt->setAuthoritativeTurnActive(active);
    if (section->cards->indexOf(prompt) != 0)
      section->cards->insertWidget(0, prompt);
    return;
  }

  std::vector<TurnSectionWidget::CardSlot *> ordered;
  ordered.reserve(section->cardSlots.size());
  if (rootSlot)
    ordered.push_back(rootSlot);
  for (TurnSectionWidget::CardSlot &slot : section->cardSlots)
    if (&slot != rootSlot)
      ordered.push_back(&slot);
  for (std::size_t position = 0; position < ordered.size(); ++position) {
    QWidget *item = ordered[position]->item;
    if (auto *card = dynamic_cast<ConversationCard *>(item)) {
      if (card->property("turnContainer").toBool())
        card->setNestedItems({});
      card->setProperty("nestedConversationCard", false);
      card->setProperty("turnContainer", false);
      card->setAuthoritativeTurnActive(false);
      card->setMinimumHeight(0);
    }
    if (section->cards->indexOf(item) != static_cast<int>(position))
      section->cards->insertWidget(static_cast<int>(position), item);
  }
}

void ConversationView::scheduleVisibilityPass() {
  if (applying_ || visibilityPassScheduled_ || snapshot_.threadId.empty())
    return;
  visibilityPassScheduled_ = true;
  QTimer::singleShot(0, this, [this] {
    visibilityPassScheduled_ = false;
    if (runVisibilityPass())
      scheduleVisibilityPass();
  });
}

bool ConversationView::runVisibilityPass() {
  if (applying_ || !content_ || !viewport() || snapshot_.threadId.empty())
    return false;

  enum class Operation { Materialize, Render, Release };
  struct Candidate {
    TurnSectionWidget *section = nullptr;
    TurnSectionWidget::CardSlot *slot = nullptr;
    const VisibleCardData *data = nullptr;
    Operation operation = Operation::Materialize;
    bool inViewport = false;
    int distance = 0;
  };

  const int scrollTop = verticalScrollBar()->value();
  const int viewportHeight = std::max(1, viewport()->height());
  const QRect visibleRect(0, scrollTop, std::max(1, content_->width()),
                          viewportHeight);
  const QRect materializationRect(0, std::max(0, scrollTop - viewportHeight),
                                  std::max(1, content_->width()),
                                  viewportHeight * 3);
  const QRect retentionRect(0, std::max(0, scrollTop - 2 * viewportHeight),
                            std::max(1, content_->width()), viewportHeight * 5);
  std::vector<Candidate> candidates;

  for (const auto &[sectionKey, section] : sections_) {
    static_cast<void>(sectionKey);
    const QRect sectionRect(section->mapTo(content_, QPoint{}),
                            section->size());
    const bool sectionNear = sectionRect.intersects(materializationRect);
    for (TurnSectionWidget::CardSlot &slot : section->cardSlots) {
      const VisibleCardData *data = dataForStableKey(slot.key);
      if (!data)
        continue;
      QWidget *item = slot.item;
      const QRect itemRect(item->mapTo(content_, QPoint{}), item->size());
      const bool isRoot = slot.key == section->rootKey;
      const bool presented =
          slot.projectionVisible && item->isVisibleTo(content_);
      // Filtered cards are retained only while their containing turn is near;
      // they have no geometry of their own but existing controls rely on their
      // stable folded/output state when the filter is toggled back on.
      const bool wanted =
          slot.newlyAdded ||
          (isRoot ? sectionNear
                  : (presented ? itemRect.intersects(materializationRect)
                               : (!slot.projectionVisible && sectionNear)));
      const bool retained =
          isRoot ? sectionRect.intersects(retentionRect)
                 : (presented ? itemRect.intersects(retentionRect)
                              : sectionRect.intersects(retentionRect));
      const bool inViewport = presented && itemRect.intersects(visibleRect);
      const int center = itemRect.center().y();
      const int distance =
          center < visibleRect.top()
              ? visibleRect.top() - center
              : (center > visibleRect.bottom() ? center - visibleRect.bottom()
                                               : 0);

      if (auto *card = dynamic_cast<ConversationCard *>(item)) {
        QWidget *focus = QApplication::focusWidget();
        const bool ownsFocus =
            focus && (focus == card || card->isAncestorOf(focus));
        if (!card->canApply(*data)) {
          candidates.push_back(
              {section, &slot, data, Operation::Release, inViewport, distance});
        } else if (!retained && !ownsFocus) {
          candidates.push_back(
              {section, &slot, data, Operation::Release, false, distance});
        } else if (wanted) {
          const bool rootShouldBeActive =
              isRoot && snapshot_.activeTurnId &&
              section->property("turnId").toString().toStdString() ==
                  *snapshot_.activeTurnId;
          if (card->data() != *data ||
              card->property("authoritativeTurnActive").toBool() !=
                  rootShouldBeActive)
            candidates.push_back({section, &slot, data, Operation::Render,
                                  inViewport, distance});
        }
      } else if (wanted) {
        candidates.push_back({section, &slot, data, Operation::Materialize,
                              inViewport, distance});
      }
    }
  }

  std::ranges::stable_sort(candidates, [](const Candidate &left,
                                          const Candidate &right) {
    if (left.inViewport != right.inViewport)
      return left.inViewport > right.inViewport;
    const auto geometryStableRender = [](const Candidate &candidate) {
      if (candidate.operation != Operation::Render || !candidate.data ||
          !candidate.slot)
        return false;
      const auto *card = dynamic_cast<ConversationCard *>(candidate.slot->item);
      const auto *before =
          card ? std::get_if<PlanData>(&card->data().payload) : nullptr;
      const auto *after = std::get_if<PlanData>(&candidate.data->payload);
      if (!before || !after || before->explanation != after->explanation ||
          before->legacyText != after->legacyText ||
          before->steps.size() != after->steps.size())
        return false;
      for (std::size_t index = 0; index < before->steps.size(); ++index)
        if (before->steps[index].text != after->steps[index].text)
          return false;
      // Status glyph changes retain the established plan row geometry, so
      // they are the safest work to defer when a frame's budget is full.
      return true;
    };
    if (geometryStableRender(left) != geometryStableRender(right))
      return geometryStableRender(right);
    const auto urgency = [](Operation operation) {
      return operation == Operation::Release ? 1 : 0;
    };
    if (urgency(left.operation) != urgency(right.operation))
      return urgency(left.operation) < urgency(right.operation);
    return left.distance < right.distance;
  });
  if (candidates.empty())
    return false;

  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  const bool followedBottom = follow && isAtBottom();
  const int previousValue = verticalScrollBar()->value();
  stopFollowingAnimation();
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  std::vector<std::pair<ConversationCard *, CommandOutputView::ScrollState>>
      outputRestorations;
  int operations = 0;

  for (const Candidate &candidate : candidates) {
    if (operations >= MaxCardOperationsPerPass)
      break;
    TurnSectionWidget::CardSlot &slot = *candidate.slot;
    if (candidate.operation == Operation::Materialize) {
      auto *placeholder = dynamic_cast<MeasuredCardPlaceholder *>(slot.item);
      if (!placeholder)
        continue;
      auto *card = createConversationCard(
          *candidate.data, candidate.section,
          !presentationOptions_.commandsInitiallyExpanded,
          !presentationOptions_.imagesInitiallyExpanded);
      card->setProperty("conversationAnchorKey",
                        QString::fromStdString(slot.key));
      if (const auto collapsed = cardCollapsedStates_.find(slot.key);
          collapsed != cardCollapsedStates_.end())
        card->setCollapsed(collapsed->second);
      connect(card, &ConversationCard::foldRequested, this,
              [this, key = slot.key, card](bool collapsed) {
                const auto retained = cards_.find(key);
                if (retained != cards_.end() && retained->second == card)
                  setCardCollapsed(key, card, collapsed);
              });
      if (const auto saved = commandOutputStates_.find(slot.key);
          saved != commandOutputStates_.end()) {
        outputRestorations.emplace_back(card, saved->second);
        commandOutputStates_.erase(saved);
      }
      card->setVisible(slot.projectionVisible);
      slot.item = card;
      slot.newlyAdded = false;
      cards_[slot.key] = card;
      delete placeholder;
      ++operations;
      continue;
    }

    auto *card = dynamic_cast<ConversationCard *>(slot.item);
    if (!card)
      continue;
    if (candidate.operation == Operation::Render) {
      card->apply(*candidate.data);
      card->setVisible(slot.projectionVisible);
      ++operations;
      continue;
    }

    const auto outputState = card->commandOutputScrollState();
    if (outputState && !outputState->followsLatest)
      commandOutputStates_[slot.key] = *outputState;
    else
      commandOutputStates_.erase(slot.key);
    if (slot.key == candidate.section->rootKey) {
      card->setNestedItems({});
      if (card->layout()) {
        card->layout()->invalidate();
        card->layout()->activate();
      }
      slot.measuredHeight = std::max(initialCardHeight(*candidate.data),
                                     card->minimumSizeHint().height());
    } else {
      slot.measuredHeight = std::max(1, card->height());
    }
    auto *placeholder = new MeasuredCardPlaceholder(
        slot.key, slot.projectionVisible ? slot.measuredHeight : 0,
        candidate.section);
    placeholder->setVisible(slot.projectionVisible);
    slot.item = placeholder;
    cards_.erase(slot.key);
    delete card;
    ++operations;
  }

  for (const auto &[sectionKey, section] : sections_) {
    static_cast<void>(sectionKey);
    arrangeSection(section);
  }
  recomputeGeometry();
  for (const auto &[card, state] : outputRestorations)
    card->restoreCommandOutputScrollState(state);
  if (followedBottom)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();

  if (follow && !followedBottom) {
    const int stableValue = verticalScrollBar()->value();
    if (verticalScrollBar()->maximum() > stableValue + 3)
      animateToBottom(std::min(previousValue, stableValue));
  }
  if (operations == MaxCardOperationsPerPass)
    scheduleVisibilityPass();
  return operations > 0;
}

void ConversationView::setCardCollapsed(const std::string &key,
                                        ConversationCard *card,
                                        bool collapsed) {
  if (!card || card->isCollapsed() == collapsed)
    return;

  const int titleTop = card->mapTo(viewport(), QPoint{}).y();
  stopFollowingAnimation();
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  mode_ = Mode::Paused;
  pausedByComposerGrowth_ = false;
  cardCollapsedStates_[key] = collapsed;
  ConversationCard *turnContainer =
      card->property("turnContainer").toBool() ? card : nullptr;
  for (QWidget *parent = card->parentWidget(); !turnContainer && parent;
       parent = parent->parentWidget())
    if (auto *candidate = dynamic_cast<ConversationCard *>(parent);
        candidate && candidate->property("turnContainer").toBool())
      turnContainer = candidate;
  if (turnContainer)
    turnContainer->setMinimumHeight(0);
  card->setCollapsed(collapsed);
  recomputeGeometry();
  for (const auto &[sectionKey, section] : sections_) {
    static_cast<void>(sectionKey);
    const auto slot = std::ranges::find_if(
        section->cardSlots,
        [&key](const TurnSectionWidget::CardSlot &candidate) {
          return candidate.key == key;
        });
    if (slot != section->cardSlots.end() && key != section->rootKey)
      slot->measuredHeight = std::max(1, card->height());
  }
  const int visibleHeight =
      std::max(0, viewport()->height() - trailingSpaceHeight_);
  const int visibleTop =
      collapsed ? titleTop
                : std::clamp(titleTop, 0,
                             std::max(0, visibleHeight - card->height()));
  setScrollValue(card->mapTo(content_, QPoint{}).y() - visibleTop);

  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
  scheduleVisibilityPass();
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

  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  content_->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  if (grew) {
    pausedByComposerGrowth_ =
        pausedByComposerGrowth_ || mode_ == Mode::Following;
    mode_ = Mode::Paused;
  }
  trailingSpaceHeight_ = height;
  QScrollBar *conversationScrollBar = verticalScrollBar();
  conversationScrollBar->setProperty("composerBottomInset", height);
  conversationScrollBar->setStyleSheet(
      height == 0
          ? QString{}
          : QStringLiteral("QScrollBar:vertical{margin:2px 2px %1px 2px;}")
                .arg(height + 2));
  recomputeGeometry();
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

  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
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

bool ConversationView::eventFilter(QObject *watched, QEvent *event) {
  if (watched == content_ && event->type() == QEvent::LayoutRequest &&
      !applying_) {
    Anchor anchor = captureAnchor();
    if (mode_ == Mode::Paused) {
      const auto retained = threadStates_.find(threadId_);
      if (retained != threadStates_.end() &&
          !retained->second.anchor.stableKey.empty())
        anchor = retained->second.anchor;
    }
    const bool follow = mode_ == Mode::Following;
    stopFollowingAnimation();
    applying_ = true;
    viewport()->setUpdatesEnabled(false);
    const QSignalBlocker scrollSignals(verticalScrollBar());
    recomputeGeometry();
    restoreAnchor(anchor);
    applying_ = false;
    viewport()->setUpdatesEnabled(true);
    viewport()->update();
    const int stableValue = verticalScrollBar()->value();
    if (follow && verticalScrollBar()->maximum() > stableValue + 3)
      animateToBottom(stableValue);
    else if (follow)
      setScrollValue(verticalScrollBar()->maximum());
    scheduleVisibilityPass();
    storeCurrentThreadState();
    return true;
  }
  return QAbstractScrollArea::eventFilter(watched, event);
}

void ConversationView::resizeEvent(QResizeEvent *event) {
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  stopFollowingAnimation();
  applying_ = true;
  viewport()->setUpdatesEnabled(false);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  QAbstractScrollArea::resizeEvent(event);
  recomputeGeometry();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  applying_ = false;
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
  scheduleVisibilityPass();
  storeCurrentThreadState();
}

void ConversationView::wheelEvent(QWheelEvent *event) {
  if (!applyWheel(event))
    QAbstractScrollArea::wheelEvent(event);
}

ConversationView::Anchor ConversationView::captureAnchor() const {
  Anchor anchor;
  anchor.absoluteValue = verticalScrollBar()->value();
  for (const std::string &key : displayedCardKeys_) {
    QWidget *item = itemForStableKey(key);
    if (!item || !item->isVisible())
      continue;
    const int viewportTop = item->mapTo(viewport(), QPoint(0, 0)).y();
    if (viewportTop + item->height() < 0)
      continue;
    anchor.stableKey = key;
    // The contract is visual stability. Capture the actual painted offset
    // instead of deriving it from content coordinates while a layout/range
    // transaction may temporarily be between those coordinate systems.
    anchor.pixelOffset = viewportTop;
    break;
  }
  return anchor;
}

void ConversationView::restoreAnchor(const Anchor &anchor) {
  int value = anchor.absoluteValue;
  if (!anchor.stableKey.empty()) {
    if (QWidget *item = itemForStableKey(anchor.stableKey)) {
      const int top = item->mapTo(content_, QPoint(0, 0)).y();
      value = top - anchor.pixelOffset;
    }
  }
  setScrollValue(std::clamp(value, verticalScrollBar()->minimum(),
                            verticalScrollBar()->maximum()));
}

void ConversationView::setScrollValue(int value) {
  value = std::clamp(value, verticalScrollBar()->minimum(),
                     verticalScrollBar()->maximum());
  programmaticScroll_ = true;
  verticalScrollBar()->setValue(value);
  programmaticScroll_ = false;
  positionContent();
  scheduleVisibilityPass();
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

void ConversationView::recomputeGeometry() {
  if (!content_ || !viewport())
    return;
  const int width = std::max(0, viewport()->width());
  trailingSpace_->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
  contentLayout_->invalidate();
  for (const auto &[key, section] : sections_) {
    static_cast<void>(key);
    section->setMinimumHeight(0);
  }

  // Give every nested layout its final width before asking for height.  This
  // makes wrapped labels and command output contribute to the same range
  // transaction as their insertion/update.
  content_->resize(width, std::max(viewport()->height(), contentHeight_));
  contentLayout_->setGeometry(content_->rect());
  for (const auto &[key, section] : sections_) {
    static_cast<void>(key);
    section->layout()->activate();
  }
  const auto activateCard = [](ConversationCard *card) {
    if (!card)
      return;
    if (QWidget *cardContent = card->findChild<QWidget *>(
            QStringLiteral("conversationCardContent"),
            Qt::FindDirectChildrenOnly);
        cardContent && cardContent->layout())
      cardContent->layout()->activate();
    if (card->layout())
      card->layout()->activate();
  };
  const auto settleCardHeight = [&activateCard](ConversationCard *card,
                                                int cardWidth) {
    if (!card || !card->layout())
      return;
    cardWidth = std::max(0, cardWidth);
    card->setMinimumHeight(0);
    // Retained rich text is created and nested in one transaction. Establish
    // its real width before measuring so QLabel cannot reuse pre-nesting
    // document geometry until a later streamed update.
    card->resize(cardWidth, card->height());
    card->layout()->invalidate();
    card->layout()->setGeometry(card->contentsRect());
    activateCard(card);
    card->updateGeometry();
    const int cardHeight =
        card->layout()->hasHeightForWidth()
            ? card->layout()->heightForWidth(cardWidth) + 2 * card->frameWidth()
            : card->sizeHint().height();
    card->setMinimumHeight(cardHeight);
    card->resize(cardWidth, cardHeight);
    card->layout()->setGeometry(card->contentsRect());
    activateCard(card);
  };
  for (const auto &[key, card] : cards_) {
    static_cast<void>(key);
    activateCard(card);
  }
  // Child/subagent threads may have no visible You root. Their cards live
  // directly in a turn section, so settle them at the final section width
  // just as deliberately as cards nested inside a normal turn container.
  for (const auto &[key, card] : cards_) {
    static_cast<void>(key);
    if (card->property("turnContainer").toBool() ||
        card->property("nestedConversationCard").toBool())
      continue;
    const int cardWidth = card->parentWidget()
                              ? card->parentWidget()->contentsRect().width()
                              : card->width();
    settleCardHeight(card, cardWidth);
  }
  // A You turn container adds one real layout depth. Settle that depth in
  // dependency order so newly nested cards reach their final height inside
  // this transaction instead of posting a second visible LayoutRequest.
  for (const auto &[key, card] : cards_) {
    static_cast<void>(key);
    if (!card->property("turnContainer").toBool())
      continue;
    card->setMinimumHeight(0);
    QWidget *nested = card->findChild<QWidget *>(
        QStringLiteral("conversationNestedCards"), Qt::FindDirectChildrenOnly);
    if (!nested || !nested->layout())
      continue;
    const int cardWidth = card->parentWidget()
                              ? card->parentWidget()->contentsRect().width()
                              : card->width();
    card->resize(cardWidth, card->height());
    if (card->layout()) {
      card->layout()->invalidate();
      card->layout()->setGeometry(card->contentsRect());
      card->layout()->activate();
    }
    nested->layout()->activate();
    for (int index = 0; index < nested->layout()->count(); ++index) {
      auto *nestedCard = dynamic_cast<ConversationCard *>(
          nested->layout()->itemAt(index)->widget());
      if (!nestedCard)
        continue;
      const int nestedWidth = nested->contentsRect().width();
      settleCardHeight(nestedCard, nestedWidth);
    }
    nested->layout()->invalidate();
    const int nestedHeight =
        nested->isHidden() ? 0 : nested->layout()->minimumSize().height();
    nested->setFixedHeight(nestedHeight);
    nested->layout()->setGeometry(nested->contentsRect());
    nested->updateGeometry();
    nested->layout()->invalidate();
    nested->layout()->activate();
    settleCardHeight(card, cardWidth);
  }
  for (const auto &[key, section] : sections_) {
    static_cast<void>(key);
    section->layout()->invalidate();
    const int sectionHeight = section->layout()->minimumSize().height();
    section->setMinimumHeight(sectionHeight);
    section->resize(section->width(), sectionHeight);
    section->layout()->setGeometry(section->contentsRect());
    section->updateGeometry();
    section->layout()->activate();
  }
  contentLayout_->invalidate();
  contentLayout_->setGeometry(content_->rect());
  contentLayout_->activate();

  int wanted = contentLayout_->hasHeightForWidth()
                   ? contentLayout_->heightForWidth(width)
                   : contentLayout_->sizeHint().height();
  wanted = std::max(wanted, contentLayout_->minimumSize().height());
  naturalContentHeight_ = wanted;
  trailingSpace_->changeSize(0, trailingSpaceHeight_, QSizePolicy::Minimum,
                             QSizePolicy::Fixed);
  contentLayout_->invalidate();
  wanted += trailingSpaceHeight_;
  contentHeight_ = std::max(viewport()->height(), wanted);
  content_->resize(width, contentHeight_);
  contentLayout_->setGeometry(QRect(0, 0, width, contentHeight_));
  for (const auto &[key, section] : sections_) {
    static_cast<void>(key);
    section->layout()->activate();
  }
  contentLayout_->activate();

  verticalScrollBar()->setPageStep(viewport()->height());
  verticalScrollBar()->setRange(
      0, std::max(0, contentHeight_ - viewport()->height()));
  positionContent();

  for (const auto &[key, card] : cards_) {
    static_cast<void>(key);
    if (QWidget *nested = card->findChild<QWidget *>(
            QStringLiteral("conversationNestedCards"),
            Qt::FindDirectChildrenOnly))
      QCoreApplication::sendPostedEvents(nested, QEvent::LayoutRequest);
    QCoreApplication::sendPostedEvents(card, QEvent::LayoutRequest);
  }
  for (const auto &[key, section] : sections_) {
    static_cast<void>(key);
    QCoreApplication::sendPostedEvents(section, QEvent::LayoutRequest);
  }
  QCoreApplication::sendPostedEvents(content_, QEvent::LayoutRequest);
}

void ConversationView::positionContent() {
  if (content_)
    content_->move(0, -verticalScrollBar()->value());
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
    // An upward wheel/touchpad gesture pauses before any subsequent layout or
    // incoming frame can move the viewport.
    stopFollowingAnimation();
    mode_ = Mode::Paused;
  }
  // Keep Qt's native wheel/touchpad interpretation, but deliver it directly
  // to the scrollbar. Calling QAbstractScrollArea::wheelEvent() here would
  // redispatch through ShellWidget's application event filter, which routes
  // the same gesture back into this method recursively.
  QScrollBar *bar = verticalScrollBar();
  const QPointF local = bar->mapFromGlobal(event->globalPosition().toPoint());
  QWheelEvent forwarded(local, event->globalPosition(), event->pixelDelta(),
                        event->angleDelta(), event->buttons(),
                        event->modifiers(), event->phase(), event->inverted());
  const QScopedValueRollback nativeDispatch(dispatchingNativeWheel_, true);
  QApplication::sendEvent(bar, &forwarded);
  positionContent();
  if (verticalScrollBar()->value() < oldValue)
    mode_ = Mode::Paused;
  if (verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1)
    mode_ = Mode::Following;
  storeCurrentThreadState();
  event->accept();
  return true;
}

ConversationCard *
ConversationView::cardForStableKey(const std::string &key) const {
  const auto card = cards_.find(key);
  return card == cards_.end() ? nullptr : card->second;
}

QWidget *ConversationView::itemForStableKey(const std::string &key) const {
  for (const auto &[sectionKey, section] : sections_) {
    static_cast<void>(sectionKey);
    const auto item = std::ranges::find_if(
        section->cardSlots, [&key](const TurnSectionWidget::CardSlot &slot) {
          return slot.key == key;
        });
    if (item != section->cardSlots.end())
      return item->item;
  }
  return nullptr;
}

const VisibleCardData *
ConversationView::dataForStableKey(const std::string &key) const {
  for (const TurnSection &section : snapshot_.sections)
    for (const VisibleCardData &card : section.cards)
      if (stableKey(card.key) == key)
        return &card;
  return nullptr;
}

} // namespace codexui::codex::middle
