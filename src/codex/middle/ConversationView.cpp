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
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int CardSpacing = 8;
constexpr int NativeScrollLineStep = 20;

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
  explicit TurnSectionWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    cards = new QVBoxLayout(this);
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(CardSpacing);
  }

  QVBoxLayout *cards = nullptr;
  std::vector<std::string> cardKeys;
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
  bool visualChange = switchedThread;
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
    int position = 0;
    bool insert = false;
    std::vector<std::string> cardKeys;
  };
  struct DesiredCard {
    TurnSectionWidget *section = nullptr;
    const VisibleCardData *data = nullptr;
  };
  std::unordered_map<std::string, DesiredSection> desiredSections;
  std::unordered_map<std::string, DesiredCard> desiredCards;
  std::vector<std::string> desiredSectionKeys;
  desiredSectionKeys.reserve(snapshot.sections.size());
  std::vector<std::string> displayedKeys;
  std::vector<std::pair<ConversationCard *, CommandOutputView::ScrollState>>
      commandOutputRestorations;
  const auto retainCommandOutputState = [this](const std::string &key,
                                               ConversationCard *card) {
    const auto state = card ? card->commandOutputScrollState() : std::nullopt;
    if (state && !state->followsLatest)
      commandOutputStates_[key] = *state;
    else
      commandOutputStates_.erase(key);
  };

  int sectionIndex = 0;
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

    DesiredSection desiredSection{section, sectionIndex++, newSection};
    desiredSection.cardKeys.reserve(sectionData.cards.size());
    int cardIndex = 0;
    for (const VisibleCardData &cardData : sectionData.cards) {
      desiredSection.cardKeys.push_back(stableKey(cardData.key));
      const std::string &key = desiredSection.cardKeys.back();
      if (cardVisible(cardData))
        displayedKeys.push_back(key);
      desiredCards.emplace(key, DesiredCard{section, &cardData});
      ++cardIndex;
    }
    desiredSections.emplace(sectionData.key, std::move(desiredSection));
  }
  const bool appendedVisibleCards =
      displayedKeys.size() > displayedCardKeys_.size() &&
      std::equal(displayedCardKeys_.begin(), displayedCardKeys_.end(),
                 displayedKeys.begin());

  for (std::size_t offset = displayedSectionKeys_.size(); offset > 0;
       --offset) {
    const std::size_t index = offset - 1;
    const std::string &key = displayedSectionKeys_[index];
    const auto desired = desiredSections.find(key);
    if (desired != desiredSections.end() &&
        desired->second.position == static_cast<int>(index))
      continue;
    delete contentLayout_->takeAt(1 + static_cast<int>(index));
    if (desired != desiredSections.end())
      desired->second.insert = true;
    visualChange = true;
  }

  // A prompt card may currently own desired nested cards while itself falls
  // outside the retained window. Detach those children before deleting the
  // obsolete prompt so their stable widgets can move to the transparent
  // section fallback instead of being destroyed with their QObject parent.
  for (const auto &[key, card] : cards_) {
    static_cast<void>(key);
    ConversationCard *owner = nullptr;
    for (QWidget *parent = card->parentWidget(); parent;
         parent = parent->parentWidget()) {
      owner = dynamic_cast<ConversationCard *>(parent);
      if (owner)
        break;
    }
    if (!owner)
      continue;
    const std::string ownerKey =
        owner->property("conversationCardKey").toString().toStdString();
    const auto desiredOwner = desiredCards.find(ownerKey);
    const bool ownerRetained =
        desiredOwner != desiredCards.end() &&
        owner->canApply(*desiredOwner->second.data);
    if (ownerRetained)
      continue;
    const auto desiredCard = desiredCards.find(key);
    QWidget *safeParent = desiredCard == desiredCards.end()
                              ? owner->parentWidget()
                              : desiredCard->second.section;
    card->setParent(safeParent);
  }

  for (auto iterator = cards_.begin(); iterator != cards_.end();) {
    const auto desired = desiredCards.find(iterator->first);
    if (desired != desiredCards.end() &&
        iterator->second->canApply(*desired->second.data)) {
      ++iterator;
      continue;
    }
    retainCommandOutputState(iterator->first, iterator->second);
    delete iterator->second;
    iterator = cards_.erase(iterator);
    visualChange = true;
  }

  for (const TurnSection &sectionData : snapshot.sections) {
    DesiredSection &desiredSection = desiredSections.at(sectionData.key);
    TurnSectionWidget *section = desiredSection.widget;
    int cardIndex = 0;
    for (const VisibleCardData &cardData : sectionData.cards) {
      const std::string &key =
          desiredSection.cardKeys[static_cast<std::size_t>(cardIndex)];

      ConversationCard *card = nullptr;
      const auto existingCard = cards_.find(key);
      if (existingCard != cards_.end()) {
        card = existingCard->second;
        visualChange = card->apply(cardData) || visualChange;
      } else {
        card = createConversationCard(
            cardData, section, !presentationOptions_.commandsInitiallyExpanded,
            !presentationOptions_.imagesInitiallyExpanded);
        card->setProperty("conversationAnchorKey", QString::fromStdString(key));
        if (const auto collapsed = cardCollapsedStates_.find(key);
            collapsed != cardCollapsedStates_.end())
          card->setCollapsed(collapsed->second);
        connect(card, &ConversationCard::foldRequested, this,
                [this, key, card](bool collapsed) {
                  const auto retained = cards_.find(key);
                  if (retained != cards_.end() && retained->second == card)
                    setCardCollapsed(key, card, collapsed);
                });
        if (const auto saved = commandOutputStates_.find(key);
            saved != commandOutputStates_.end()) {
          commandOutputRestorations.emplace_back(card, saved->second);
          commandOutputStates_.erase(saved);
        }
        cards_.emplace(key, card);
        visualChange = true;
      }

      const bool visible = cardVisible(cardData);
      if (card->isHidden() == visible) {
        card->setVisible(visible);
        visualChange = true;
      }
      ++cardIndex;
    }

    ConversationCard *prompt = nullptr;
    if (sectionData.rootCardKey) {
      const auto root = cards_.find(stableKey(*sectionData.rootCardKey));
      if (root != cards_.end())
        prompt = root->second;
    }
    std::vector<ConversationCard *> orderedCards;
    orderedCards.reserve(sectionData.cards.size());
    for (const VisibleCardData &cardData : sectionData.cards) {
      ConversationCard *card = cards_.at(stableKey(cardData.key));
      orderedCards.push_back(card);
    }
    if (prompt) {
      for (ConversationCard *card : orderedCards) {
        if (card != prompt && card->property("turnContainer").toBool())
          card->setNestedCards({});
        if (card != prompt)
          visualChange =
              card->setAuthoritativeTurnActive(false) || visualChange;
        card->setProperty("turnContainer", false);
      }
      std::vector<ConversationCard *> nestedCards;
      nestedCards.reserve(orderedCards.size() - 1);
      for (ConversationCard *card : orderedCards)
        if (card != prompt)
          nestedCards.push_back(card);
      prompt->setProperty("nestedConversationCard", false);
      prompt->setProperty("turnContainer", true);
      prompt->setNestedCards(nestedCards);
      visualChange =
          prompt->setAuthoritativeTurnActive(
              snapshot.activeTurnId &&
              sectionData.turnId == *snapshot.activeTurnId) ||
          visualChange;
      if (section->cards->indexOf(prompt) != 0)
        section->cards->insertWidget(0, prompt);
    } else {
      for (std::size_t position = 0; position < orderedCards.size(); ++position) {
        ConversationCard *card = orderedCards[position];
        if (card->property("turnContainer").toBool())
          card->setNestedCards({});
        card->setProperty("nestedConversationCard", false);
        card->setProperty("turnContainer", false);
        visualChange = card->setAuthoritativeTurnActive(false) || visualChange;
        card->setMinimumHeight(0);
        if (section->cards->indexOf(card) != static_cast<int>(position))
          section->cards->insertWidget(static_cast<int>(position), card);
      }
    }
    section->cardKeys = std::move(desiredSection.cardKeys);
    const bool sectionVisible =
        std::ranges::any_of(sectionData.cards, [this](const auto &card) {
          return cardVisible(card);
        });
    if (section->isHidden() == sectionVisible) {
      section->setVisible(sectionVisible);
      visualChange = true;
    }
    if (desiredSection.insert)
      contentLayout_->insertWidget(1 + desiredSection.position, section);
  }

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
  const bool outputGrew = visibleOutputFootprint() > outputFootprintBefore;
  for (const auto &[card, state] : commandOutputRestorations)
    card->restoreCommandOutputScrollState(state);
  if (follow) {
    if (switchedThread || outputGrew || appendedVisibleCards ||
        settleFollowImmediately) {
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
  return visualChange;
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
  const int visibleHeight =
      std::max(0, viewport()->height() - trailingSpaceHeight_);
  const int visibleTop =
      collapsed
          ? titleTop
          : std::clamp(titleTop, 0,
                       std::max(0, visibleHeight - card->height()));
  setScrollValue(card->mapTo(content_, QPoint{}).y() - visibleTop);

  applying_ = false;
  content_->setUpdatesEnabled(true);
  viewport()->setUpdatesEnabled(true);
  viewport()->update();
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
    ConversationCard *card = cardForStableKey(key);
    if (!card || !card->isVisible())
      continue;
    const int viewportTop = card->mapTo(viewport(), QPoint(0, 0)).y();
    if (viewportTop + card->height() < 0)
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
    if (ConversationCard *card = cardForStableKey(anchor.stableKey)) {
      const int top = card->mapTo(content_, QPoint(0, 0)).y();
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
    card->setMinimumHeight(0);
    card->layout()->invalidate();
    activateCard(card);
    card->updateGeometry();
    cardWidth = std::max(0, cardWidth);
    const int cardHeight =
        card->layout()->hasHeightForWidth()
            ? card->layout()->heightForWidth(cardWidth) +
                  2 * card->frameWidth()
            : card->sizeHint().height();
    card->setMinimumHeight(cardHeight);
    card->resize(cardWidth, cardHeight);
    card->layout()->setGeometry(card->contentsRect());
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
        QStringLiteral("conversationNestedCards"),
        Qt::FindDirectChildrenOnly);
    if (!nested || !nested->layout())
      continue;
    if (card->layout())
      card->layout()->activate();
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
    settleCardHeight(card, card->width());
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

} // namespace codexui::codex::middle
