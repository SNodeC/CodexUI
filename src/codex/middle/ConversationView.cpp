// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationView.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractSlider>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {

#if QT_CONFIG(accessibility)
namespace {

void notifyBusyChange(QWidget &widget, bool wasBusy, bool isBusy) {
  if (wasBusy == isBusy)
    return;
  QAccessible::State changed;
  changed.busy = true;
  QAccessibleStateChangeEvent event(&widget, changed);
  QAccessible::updateAccessibility(&event);
}

} // namespace
#endif

class ConversationLoadingOverlay final : public QWidget {
public:
  static constexpr int SpinnerDelayMilliseconds = 500;
  static constexpr int SpinnerAnimationMilliseconds = 33;
  static constexpr int SpinnerDiameter = 30;
  static constexpr int SpinnerStrokeWidth = 3;

  enum class State {
    Loading,
    Failed,
  };

  explicit ConversationLoadingOverlay(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("conversationStagingOverlay"));
    setAccessibleName(QStringLiteral("Loading conversation"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);

    spinnerDelay_.setSingleShot(true);
    spinnerDelay_.setTimerType(Qt::PreciseTimer);
    spinnerDelay_.setInterval(SpinnerDelayMilliseconds);
    connect(&spinnerDelay_, &QTimer::timeout, this, [this] {
      if (!isVisible())
        return;
      spinnerVisible_ = true;
      if (UiStyle::animationsEnabled(*this))
        spinnerAnimation_.start();
      update(spinnerRect().adjusted(-2, -2, 2, 2).toAlignedRect());
    });

    spinnerAnimation_.setTimerType(Qt::PreciseTimer);
    spinnerAnimation_.setObjectName(
        QStringLiteral("conversationSpinnerAnimationTimer"));
    spinnerAnimation_.setInterval(SpinnerAnimationMilliseconds);
    connect(&spinnerAnimation_, &QTimer::timeout, this, [this] {
      if (!UiStyle::animationsEnabled(*this)) {
        spinnerAnimation_.stop();
        return;
      }
      phaseDegrees_ = (phaseDegrees_ - 18 + 360) % 360;
      update(spinnerRect().adjusted(-2, -2, 2, 2).toAlignedRect());
    });

    hide();
  }

  void begin() {
    const bool wasBusy = isBusy();
    state_ = State::Loading;
    presentation::setAccessibleNameIfChanged(
        *this, QStringLiteral("Loading conversation"));
    spinnerDelay_.stop();
    spinnerAnimation_.stop();
    spinnerVisible_ = false;
    phaseDegrees_ = 90;
    show();
    raise();
    update();
    spinnerDelay_.start();
#if QT_CONFIG(accessibility)
    notifyBusyChange(*this, wasBusy, isBusy());
#endif
  }

  void fail() {
    const bool wasBusy = isBusy();
    state_ = State::Failed;
    spinnerDelay_.stop();
    spinnerAnimation_.stop();
    spinnerVisible_ = false;
    presentation::setAccessibleNameIfChanged(
        *this, QStringLiteral("Conversation unavailable"));
    show();
    raise();
    update();
#if QT_CONFIG(accessibility)
    notifyBusyChange(*this, wasBusy, isBusy());
#endif
  }

  [[nodiscard]] bool isBusy() const noexcept {
    return state_ == State::Loading && isVisible();
  }

  void finish() {
    const bool wasBusy = isBusy();
    spinnerDelay_.stop();
    spinnerAnimation_.stop();
    spinnerVisible_ = false;
    hide();
#if QT_CONFIG(accessibility)
    notifyBusyChange(*this, wasBusy, isBusy());
#endif
  }

protected:
  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (!event || event->type() != QEvent::StyleChange ||
        state_ != State::Loading || !spinnerVisible_ || !isVisible())
      return;
    if (UiStyle::animationsEnabled(*this))
      spinnerAnimation_.start();
    else
      spinnerAnimation_.stop();
  }

  void paintEvent(QPaintEvent *event) override {
    QPainter painter(this);
    painter.setClipRegion(event->region());
    painter.fillRect(rect(),
                     QColor(QString::fromLatin1(UiStyle::appBackground)));
    if (state_ == State::Failed) {
      painter.setPen(QColor(QString::fromLatin1(UiStyle::secondary)));
      painter.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("Conversation unavailable"));
      return;
    }
    if (!spinnerVisible_)
      return;

    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF ring = spinnerRect();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QString::fromLatin1(UiStyle::divider)),
                        SpinnerStrokeWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(ring);
    painter.setPen(QPen(QColor(QString::fromLatin1(UiStyle::secondary)),
                        SpinnerStrokeWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, phaseDegrees_ * 16, 105 * 16);
  }

private:
  [[nodiscard]] QRectF spinnerRect() const {
    const int paintedCenterlineDiameter = SpinnerDiameter - SpinnerStrokeWidth;
    QRectF ring(0.0, 0.0, paintedCenterlineDiameter, paintedCenterlineDiameter);
    ring.moveCenter(QRectF(rect()).center());
    return ring;
  }

  QTimer spinnerDelay_;
  QTimer spinnerAnimation_;
  State state_ = State::Loading;
  bool spinnerVisible_ = false;
  int phaseDegrees_ = 90;
};

#if QT_CONFIG(accessibility)
namespace {

ConversationView *conversationViewFor(QWidget *widget) {
  for (; widget; widget = widget->parentWidget())
    if (auto *view = dynamic_cast<ConversationView *>(widget))
      return view;
  return nullptr;
}

class ConversationAccessible final : public QAccessibleWidget,
                                     public QAccessibleSelectionInterface {
public:
  ConversationAccessible(QWidget *widget, QAccessible::Role role)
      : QAccessibleWidget(widget, role) {}

  void *interface_cast(QAccessible::InterfaceType type) override {
    return role() == QAccessible::List &&
                   type == QAccessible::SelectionInterface
               ? static_cast<QAccessibleSelectionInterface *>(this)
               : QAccessibleWidget::interface_cast(type);
  }

  int childCount() const override {
    return projectsChildren() ? physicalChildren().size()
                              : QAccessibleWidget::childCount();
  }

  QAccessibleInterface *child(int index) const override {
    if (!projectsChildren())
      return QAccessibleWidget::child(index);
    QWidget *childWidget = physicalChildren().value(index);
    return childWidget ? QAccessible::queryAccessibleInterface(childWidget)
                       : nullptr;
  }

  int indexOfChild(const QAccessibleInterface *child) const override {
    return projectsChildren()
               ? physicalChildren().indexOf(
                     child ? qobject_cast<QWidget *>(child->object()) : nullptr)
               : QAccessibleWidget::indexOfChild(child);
  }

  QAccessibleInterface *focusChild() const override {
    ConversationView *owner = conversationViewFor(widget());
    if (owner && owner->hasFocus()) {
      if (role() == QAccessible::List)
        return itemForIndex(owner->currentIndex());
      if (role() == QAccessible::Pane) {
        QAccessibleInterface *list = child(0);
        QAccessibleInterface *row = list ? list->focusChild() : nullptr;
        return row ? row : list;
      }
    }
    return QAccessibleWidget::focusChild();
  }

  QAccessible::State state() const override {
    QAccessible::State result = QAccessibleWidget::state();
    if (role() == QAccessible::List) {
      const auto *history = widget()->findChild<QPushButton *>(
          QStringLiteral("conversationLoadMore"), Qt::FindDirectChildrenOnly);
      result.busy = history && history->isVisible() && !history->isEnabled();
      return result;
    }
    if (role() == QAccessible::StatusBar) {
      const auto *overlay =
          dynamic_cast<const ConversationLoadingOverlay *>(widget());
      result.busy = overlay && overlay->isBusy();
      return result;
    }
    if (role() != QAccessible::ListItem)
      return result;
    ConversationView *owner = conversationViewFor(widget());
    auto *card = qobject_cast<ConversationCard *>(widget());
    if (!owner || !card || card->parentWidget() != owner->viewport()) {
      result.offscreen = result.invisible = true;
      return result;
    }
    const QModelIndex index = owner->conversationModel()->indexForStableKey(
        stableKey(card->data().key));
    const bool exposed = index.isValid() && !card->isHidden();
    const bool interactive = exposed && owner->isVisible() &&
                             owner->isEnabled() && card->isEnabled();
    result.focusable = result.selectable = interactive;
    result.selected = exposed && owner->selectionModel()->isSelected(index);
    result.focused =
        exposed && owner->hasFocus() && owner->currentIndex() == index;
    result.offscreen = !card->geometry().intersects(owner->viewport()->rect());
    result.invisible = result.invisible || !exposed || result.offscreen;
    return result;
  }

  int selectedItemCount() const override { return selectedItems().size(); }

  QList<QAccessibleInterface *> selectedItems() const override {
    QList<QAccessibleInterface *> result;
    if (ConversationView *owner = conversationViewFor(widget()))
      for (const QModelIndex &index : owner->selectionModel()->selectedRows())
        if (QAccessibleInterface *item = itemForIndex(index))
          result.push_back(item);
    return result;
  }

  bool select(QAccessibleInterface *item) override {
    return setItemSelected(item, true);
  }
  bool unselect(QAccessibleInterface *item) override {
    return setItemSelected(item, false);
  }
  bool selectAll() override { return false; }
  bool clear() override {
    ConversationView *owner = conversationViewFor(widget());
    if (!owner || !owner->isVisible() || !owner->isEnabled())
      return false;
    owner->selectionModel()->clearSelection();
    return !owner->selectionModel()->hasSelection();
  }

private:
  bool projectsChildren() const {
    return role() == QAccessible::Pane || role() == QAccessible::List;
  }

  QModelIndex indexForCard(const ConversationCard *card) const {
    ConversationView *owner = conversationViewFor(widget());
    return owner && card ? owner->conversationModel()->indexForStableKey(
                               stableKey(card->data().key))
                         : QModelIndex{};
  }

  QList<QWidget *> physicalChildren() const {
    QList<QWidget *> result;
    ConversationView *owner = conversationViewFor(widget());
    if (!owner)
      return result;
    if (role() == QAccessible::Pane) {
      result.push_back(owner->viewport());
      for (QScrollBar *bar :
           {owner->horizontalScrollBar(), owner->verticalScrollBar()})
        if (bar->isVisible())
          result.push_back(bar->parentWidget());
      if (QWidget *corner = owner->cornerWidget();
          corner && corner->isVisible())
        result.push_back(corner);
      return result;
    }
    if (role() != QAccessible::List || widget() != owner->viewport())
      return result;
    QList<ConversationCard *> cards;
    for (QWidget *childWidget : widget()->findChildren<QWidget *>(
             QString{}, Qt::FindDirectChildrenOnly)) {
      if (dynamic_cast<ConversationLoadingOverlay *>(childWidget) &&
          !childWidget->isHidden())
        return {childWidget};
      if (auto *card = qobject_cast<ConversationCard *>(childWidget)) {
        if (!card->isHidden() && indexForCard(card).isValid())
          cards.push_back(card);
      } else if (!childWidget->isHidden() &&
                 (childWidget->objectName() ==
                      QStringLiteral("conversationLoadMore") ||
                  childWidget->objectName() ==
                      QStringLiteral("conversationEmpty"))) {
        result.push_back(childWidget);
      }
    }
    std::ranges::sort(
        cards, std::ranges::less{},
        [this](ConversationCard *card) { return indexForCard(card).row(); });
    for (ConversationCard *card : cards)
      result.push_back(card);
    return result;
  }

  QAccessibleInterface *itemForIndex(const QModelIndex &index) const {
    if (!index.isValid())
      return nullptr;
    for (QWidget *childWidget : physicalChildren())
      if (auto *card = qobject_cast<ConversationCard *>(childWidget);
          card && indexForCard(card) == index)
        return QAccessible::queryAccessibleInterface(card);
    return nullptr;
  }

  QModelIndex cardIndex(QAccessibleInterface *item) const {
    auto *card =
        item ? qobject_cast<ConversationCard *>(item->object()) : nullptr;
    return role() == QAccessible::List && card && !card->isHidden() &&
                   card->parentWidget() == widget()
               ? indexForCard(card)
               : QModelIndex{};
  }

  bool setItemSelected(QAccessibleInterface *item, bool selected) {
    ConversationView *owner = conversationViewFor(widget());
    const QModelIndex index = cardIndex(item);
    auto *card =
        item ? qobject_cast<ConversationCard *>(item->object()) : nullptr;
    if (!owner || !owner->isVisible() || !owner->isEnabled() || !card ||
        !card->isEnabled() || !index.isValid())
      return false;
    owner->selectionModel()->select(
        index,
        selected
            ? QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
            : QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
    return owner->selectionModel()->isSelected(index) == selected;
  }
};

QAccessibleInterface *conversationAccessibleFactory(const QString &,
                                                    QObject *object) {
  auto *widget = qobject_cast<QWidget *>(object);
  if (!widget)
    return nullptr;
  ConversationView *owner = conversationViewFor(widget);
  if (!owner)
    return nullptr;
  if (widget == owner)
    return new ConversationAccessible(widget, QAccessible::Pane);
  if (widget == owner->viewport())
    return new ConversationAccessible(widget, QAccessible::List);
  if (qobject_cast<ConversationCard *>(widget))
    return new ConversationAccessible(widget, QAccessible::ListItem);
  if (dynamic_cast<ConversationLoadingOverlay *>(widget))
    return new ConversationAccessible(widget, QAccessible::StatusBar);
  return nullptr;
}

} // namespace
#endif

namespace {

constexpr int CardSpacing = 8;
constexpr int CardFrameExtent = 2;
constexpr int TurnSurfaceBottomPadding = 10;
constexpr int HistoryButtonHeight = 32;
constexpr int NestedCardIndent = 12;
constexpr int NativeScrollLineStep = 20;
constexpr int EstimatedCardHeight = 112;
constexpr int MinimumEstimatedCardHeight = 44;

ConversationCard *owningCard(const ConversationView &view, QWidget *widget) {
  if (!widget || !view.viewport()->isAncestorOf(widget))
    return nullptr;
  for (; widget && widget != view.viewport(); widget = widget->parentWidget())
    if (auto *card = qobject_cast<ConversationCard *>(widget))
      return card;
  return nullptr;
}

bool collapsedByDefault(CardKind kind) noexcept {
  return kind == CardKind::AgentActivity || kind == CardKind::Reasoning ||
         kind == CardKind::Plan || kind == CardKind::GenericActivity;
}

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
#if QT_CONFIG(accessibility)
  static const bool factoryInstalled =
      (QAccessible::installFactory(conversationAccessibleFactory), true);
  static_cast<void>(factoryInstalled);
#endif
  setObjectName(QStringLiteral("conversationScroll"));
  setAccessibleName(QStringLiteral("Conversation pane"));
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  // Rows own arrow/page navigation; their real child controls own Tab order.
  // Letting QAbstractItemView also consume Tab advances the current row while
  // leaving focus on a different card.
  setTabKeyNavigation(false);
  setModel(model_);
  connect(model_, &QAbstractItemModel::dataChanged, this,
          [this](const QModelIndex &first, const QModelIndex &last,
                 const QList<int> &) {
            if (!applying_)
              return;
            for (int rowIndex = first.row(); rowIndex <= last.row();
                 ++rowIndex) {
              const ConversationItemModel::Row *row = model_->row(rowIndex);
              if (!row || materializedCards_.contains(row->stableKey))
                continue;
              auto thread = threadPresentations_.find(row->card.threadId);
              if (thread == threadPresentations_.end())
                continue;
              const auto retained = thread->second.cards.find(row->stableKey);
              if (retained != thread->second.cards.end())
                retained->second.height.reset();
            }
          });
  verticalScrollBar()->setSingleStep(NativeScrollLineStep);
  viewport()->setAutoFillBackground(false);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);

  loadMore_ =
      new QPushButton(QStringLiteral("Load more activities"), viewport());
  loadMore_->setObjectName(QStringLiteral("conversationLoadMore"));
  loadMore_->setProperty("kind", "history");
  loadMore_->setFixedHeight(HistoryButtonHeight);
  loadMore_->hide();
  connect(loadMore_, &QPushButton::clicked, this, [this] {
    const auto action = loadMoreAction_;
    if (action)
      action();
  });

  empty_ = makeEmptyLabel(viewport());
  empty_->setObjectName(QStringLiteral("conversationEmpty"));
  emptyMessage_ = empty_->text();
  viewport()->setAccessibleName(QStringLiteral("Conversation"));

  // Rich rows prepared for an atomic thread/paging reveal are never parented
  // into the visible or accessible viewport until their final geometry is
  // known.
  stagingHost_ = new QWidget(this);
  stagingHost_->setObjectName(QStringLiteral("conversationStagingHost"));
  stagingHost_->hide();

  stagingOverlay_ = new ConversationLoadingOverlay(viewport());

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

  resizeFrameTimer_ = new QTimer(this);
  resizeFrameTimer_->setSingleShot(true);
  resizeFrameTimer_->setTimerType(Qt::PreciseTimer);
  resizeFrameTimer_->setInterval(16);
  connect(resizeFrameTimer_, &QTimer::timeout, this, [this] {
    if (interactiveResize_)
      reflowAfterResize(ReflowCause::ResizeFrame);
  });
  resizeSettleTimer_ = new QTimer(this);
  resizeSettleTimer_->setSingleShot(true);
  resizeSettleTimer_->setInterval(120);
  connect(resizeSettleTimer_, &QTimer::timeout, this, [this] {
    if (!interactiveResize_)
      return;
    if (QApplication::mouseButtons().testFlag(Qt::LeftButton)) {
      resizeSettleTimer_->start();
      return;
    }
    endInteractiveResize();
  });

  connect(verticalScrollBar(), &QScrollBar::sliderPressed, this, [this] {
    pausedByCommandOutput_ = false;
    stopFollowingAnimation();
  });
  connect(verticalScrollBar(), &QScrollBar::sliderReleased, this,
          [this] { handleUserScrollValue(verticalScrollBar()->value()); });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
          [this](int action) {
            userActionPending_ = true;
            pausedByCommandOutput_ = false;
            stopFollowingAnimation();
            if (action == QAbstractSlider::SliderSingleStepSub ||
                action == QAbstractSlider::SliderPageStepSub ||
                action == QAbstractSlider::SliderToMinimum)
              mode_ = Mode::Paused;
          });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (!programmaticScroll_ && !applying_ &&
                (verticalScrollBar()->isSliderDown() || userActionPending_))
              handleUserScrollValue(value);
            userActionPending_ = false;
          });
  connect(qApp, &QApplication::focusChanged, this,
          [this](QWidget *previous, QWidget *focused) {
            ConversationCard *const focusedCard = owningCard(*this, focused);
            ConversationCard *departingFocusPin = owningCard(*this, previous);
            if (departingFocusPin == focusedCard || !departingFocusPin ||
                departingFocusPin->isHidden() ||
                departingFocusPin->geometry().intersects(viewport()->rect()))
              departingFocusPin = nullptr;
            if (!focusedCard && !departingFocusPin)
              return;
            layoutMaterializedCards();
            if (departingFocusPin) {
              const auto [first, last] = materializationRows();
              releaseUnneededCards(first, last, departingFocusPin);
            }
            if (focusedCard) {
              const QModelIndex index =
                  model_->indexForStableKey(stableKey(focusedCard->data().key));
              if (index.isValid()) {
                const bool autoScroll = hasAutoScroll();
                setAutoScroll(false);
                selectionModel()->setCurrentIndex(
                    index, QItemSelectionModel::NoUpdate);
                setAutoScroll(autoScroll);
              }
            }
          });

  rebuildHeightIndex();
  updateScrollRange();
}

ConversationView::~ConversationView() {
  QObject::disconnect(qApp, nullptr, this, nullptr);
  cancelStructuralStaging();
  releaseAllCards();
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

void ConversationView::setNoticeAction(
    std::function<void(QString, bool)> action) {
  noticeAction_ = std::move(action);
}

bool ConversationView::retainsTarget(
    const nodegraph::NodeRef &target) const noexcept {
  if (!target)
    return false;
  if (model_->indexForTarget(target).isValid())
    return true;
  for (const auto &[key, card] : stagedCards_) {
    static_cast<void>(key);
    if (card && card->data().target == target)
      return true;
  }
  if (!pendingStructuralSnapshot_)
    return false;
  for (const TurnSection &section : pendingStructuralSnapshot_->sections)
    for (const VisibleCardData &card : section.cards)
      if (card.target == target)
        return true;
  return std::ranges::any_of(
      pendingStructuralSnapshot_->materializedPrompts,
      [&target](const PromptMaterialization &prompt) {
        return prompt.prompt == target;
      });
}

void ConversationView::setReconciliationFinishedAction(
    std::function<void(const std::string &, ReconciliationResult, bool)>
        action) {
  reconciliationFinishedAction_ = std::move(action);
}

void ConversationView::setEmptyMessage(QString message) {
  if (message == emptyMessage_)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  emptyMessage_ = std::move(message);
  empty_->setText(emptyMessage_);
  restoreViewport(anchor, follow);
  viewport()->update();
}

void ConversationView::setPresentationOptions(PresentationOptions options) {
  if (presentationOptions_ == options)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  retainAdmissionDefaults();
  presentationOptions_ = options;
  const bool visibilityChanged =
      model_->setVisibility({options.showReasoning, options.showCodexUpdates});
  if (pendingStructuralSnapshot_) {
    const QScopedValueRollback changingStage(committingStructuralStage_, true);
    clearStagedCards();
    choosePendingStageRows();
    scheduleCardAdmissionPass();
  }
  if (!visibilityChanged)
    return;
  rebuildSectionRanges();
  rebuildHeightIndex();
  restoreViewport(anchor, follow);
  updateMaterialization();
  viewport()->update();
}

ConversationView::ReconciliationResult
ConversationView::reconcile(const ConversationSnapshot &snapshot) {
  if (applying_ || committingStructuralStage_)
    return ReconciliationResult::Rejected;
  if (pendingStructuralSnapshot_)
    cancelStructuralStaging();
  std::vector<nodegraph::NodeRef> acknowledged;
  std::optional<std::string> committedThread;
  const ConversationItemModel::StructuralChangeResult result = reconcileOwned(
      ConversationSnapshot(snapshot), acknowledged, committedThread);
  const bool selectionCommitted = committedThread.has_value();
  publishReconciliation(std::move(committedThread), result, selectionCommitted,
                        std::move(acknowledged));
  return result;
}

void ConversationView::beginThreadSelection(const std::string &threadId,
                                            std::uint64_t incarnation) {
  if (applying_ || threadId.empty())
    return;
  if (incarnation != 0) {
    const auto current = threadPresentations_.find(threadId);
    if (current != threadPresentations_.end() &&
        current->second.incarnation != incarnation)
      forgetThreadPresentation(threadId, current->second.incarnation);
    threadPresentations_[threadId].incarnation = incarnation;
  }
  if (loadingThreadId_ == threadId && stagingOverlay_->isBusy())
    return;
  cancelStructuralStaging();
  loadingThreadId_ = threadId;
  stagingOverlay_->setGeometry(viewport()->rect());
  stagingOverlay_->begin();
}

ConversationItemModel::StructuralChangeResult
ConversationView::reconcileOwned(ConversationSnapshot snapshot,
                                 std::vector<nodegraph::NodeRef> &acknowledged,
                                 std::optional<std::string> &committedThread) {
  const std::string targetThreadId = snapshot.threadId;
  const std::string previousThreadId = threadId_;
  const bool switchedThread = snapshot.threadId != threadId_;
  const Anchor currentAnchor = captureAnchor();
  const ThreadScrollState outgoingState{mode_, currentAnchor,
                                        pausedByCommandOutput_};
  const auto saved = threadPresentations_.find(targetThreadId);
  const ThreadScrollState incomingState =
      switchedThread && saved != threadPresentations_.end()
          ? saved->second.scroll
          : ThreadScrollState{};
  const Anchor targetAnchor =
      switchedThread ? incomingState.anchor : currentAnchor;
  struct PreviousCard {
    CardKind kind = CardKind::GenericActivity;
  };
  std::unordered_map<std::string, PreviousCard> previousCards;
  previousCards.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int row = 0; row < model_->rowCount(); ++row) {
    if (const auto *value = model_->row(row))
      previousCards.emplace(value->stableKey, PreviousCard{value->card.kind});
  }
  std::vector<PromptMaterialization> materializedPrompts =
      std::move(snapshot.materializedPrompts);
  const QScopedValueRollback applying(applying_, true);
  const QSignalBlocker scrollSignals(verticalScrollBar());

  ConversationItemModel::StructuralChangeResult result =
      ConversationItemModel::StructuralChangeResult::Rejected;
  result = model_->reconcile(std::move(snapshot));
  if (result == ConversationItemModel::StructuralChangeResult::Rejected) {
    if (!loadingThreadId_.empty() && loadingThreadId_ == targetThreadId)
      stagingOverlay_->fail();
    return result;
  }
  if (switchedThread) {
    retainAdmissionDefaults();
    stopFollowingAnimation();
    threadPresentations_[targetThreadId].scroll = {};
    threadId_ = targetThreadId;
    mode_ = incomingState.mode;
    pausedByCommandOutput_ = incomingState.pausedByCommandOutput;
    if (!previousThreadId.empty())
      threadPresentations_[previousThreadId].scroll = outgoingState;
  }
  if (result == ConversationItemModel::StructuralChangeResult::Unchanged) {
    if (finishThreadSelection(targetThreadId))
      committedThread = targetThreadId;
    return result;
  }
  for (PromptMaterialization &materialization : materializedPrompts) {
    const std::string key = stableKey(materialization.cardKey);
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    const auto before = previousCards.find(key);
    if (index.isValid() && row && row->card.kind == CardKind::UserMessage &&
        (switchedThread || before == previousCards.end() ||
         before->second.kind == CardKind::LocalPrompt))
      acknowledged.push_back(std::move(materialization.prompt));
  }
  for (auto &[key, card] : stagedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row ||
        card->isCollapsed() == cardCollapsed(row->card, row->stableKey))
      continue;
    card->setCollapsed(cardCollapsed(row->card, row->stableKey));
    static_cast<void>(card->settleHeightForWidth(rowWidth(*row)));
  }
  viewport()->setUpdatesEnabled(false);
  stopFollowingAnimation();
  rebuildSectionRanges();
  updateHistoryControls();

  std::vector<std::string> removeKeys;
  removeKeys.reserve(materializedCards_.size());
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row || !rowPresented(index.row()) ||
        !card->canApply(row->card)) {
      removeKeys.push_back(key);
      continue;
    }
    PresentationImpact impact = PresentationImpact::None;
    if (card->data() != row->card)
      impact = card->applyPresentation(row->card);
    const bool rowGeometryChanged = configureCardForRow(card, *row);
    const int width = rowWidth(*row);
    if (impact == PresentationImpact::GeometryChanged || rowGeometryChanged ||
        !retainedHeight(*row)) {
      const int measuredHeight = card->settleHeightForWidth(width);
      retainCardState(row->card.threadId, key).height =
          HeightRecord{width, measuredHeight};
    }
  }
  for (const std::string &key : removeKeys) {
    const auto found = materializedCards_.find(key);
    if (found == materializedCards_.end())
      continue;
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    releaseCard(key, card);
  }
  if (!switchedThread) {
    for (const auto &[key, previous] : previousCards) {
      const QModelIndex index = model_->indexForStableKey(key);
      const ConversationItemModel::Row *row =
          index.isValid() ? model_->row(index.row()) : nullptr;
      const bool compatible =
          row && (row->card.kind == previous.kind ||
                  (previous.kind == CardKind::LocalPrompt &&
                   row->card.kind == CardKind::UserMessage));
      if (!compatible)
        forgetCardPresentation(targetThreadId, key);
    }
  }
  if (const auto retained = threadPresentations_.find(targetThreadId);
      retained != threadPresentations_.end()) {
    std::vector<std::string> staleKeys;
    for (const auto &[key, state] : retained->second.cards) {
      const QModelIndex index = model_->indexForStableKey(key);
      const ConversationItemModel::Row *row = model_->row(index.row());
      if (!index.isValid() || !row)
        staleKeys.push_back(key);
      else if (state.interaction)
        normalizeRetainedState(row->card, row->nested);
    }
    for (const std::string &key : staleKeys)
      forgetCardPresentation(targetThreadId, key);
  }

  const bool follow = mode_ == Mode::Following;
  rebuildHeightIndex();
  restoreViewport(targetAnchor, follow);
  updateMaterialization();
  restoreAnchor(targetAnchor, follow);
  viewport()->setUpdatesEnabled(true);
  if (finishThreadSelection(targetThreadId))
    committedThread = targetThreadId;
  viewport()->update();
  incrementProperty(this, "graphRefreshPasses");
  return result;
}

void ConversationView::publishReconciliation(
    std::optional<std::string> completedThread, ReconciliationResult result,
    bool selectionCommitted, std::vector<nodegraph::NodeRef> acknowledged) {
  const auto finishedAction = reconciliationFinishedAction_;
  const auto materializedAction = promptMaterializedAction_;
  const QPointer<ConversationView> lifetime(this);
  if (completedThread && finishedAction)
    finishedAction(*completedThread, result, selectionCommitted);
  if (!lifetime || !materializedAction)
    return;
  for (nodegraph::NodeRef &target : acknowledged) {
    const bool accepted = materializedAction(std::move(target));
    if (!lifetime || !accepted)
      break;
  }
}

ConversationView::SnapshotDisposition
ConversationView::reconcileStaged(ConversationSnapshot snapshot) {
  return stageSnapshot(std::move(snapshot));
}

ConversationView::SnapshotDisposition
ConversationView::stageSnapshot(ConversationSnapshot snapshot) {
  if (applying_ || committingStructuralStage_)
    return SnapshotDisposition::Retryable;
  if (!loadingThreadId_.empty() && snapshot.threadId != loadingThreadId_) {
    incrementProperty(this, "staleThreadStagesIgnored");
    return SnapshotDisposition::Superseded;
  }
  if (snapshot.threadId != threadId_ && loadingThreadId_.empty())
    beginThreadSelection(snapshot.threadId);
  else {
    cancelStructuralStaging();
    if (!loadingThreadId_.empty() && !stagingOverlay_->isBusy())
      stagingOverlay_->begin();
  }
  pendingStructuralSnapshot_ = std::move(snapshot);
  buildPendingLocations();
  choosePendingStageRows();
  stagingHost_->resize(std::max(0, viewport()->width()),
                       std::max(0, viewport()->height()));
  incrementProperty(this, "structuralStageStarts");
  scheduleCardAdmissionPass();
  return SnapshotDisposition::Admitted;
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
    const auto representedRoot =
        root ? std::ranges::find_if(section.cards,
                                    [&](const auto &card) {
                                      return stableKey(card.key) == *root;
                                    })
             : section.cards.end();
    const std::optional<std::size_t> rootPosition =
        representedRoot == section.cards.end()
            ? std::nullopt
            : std::optional<std::size_t>{static_cast<std::size_t>(
                  std::distance(section.cards.begin(), representedRoot))};
    for (std::size_t cardIndex = 0; cardIndex < section.cards.size();
         ++cardIndex) {
      const std::string key = stableKey(section.cards[cardIndex].key);
      pendingLocations_.emplace(
          key, PendingLocation{sectionIndex, cardIndex,
                               isNestedTurnCard(rootPosition, cardIndex)});
    }
  }
}

void ConversationView::choosePendingStageRows() {
  pendingStructuralCardKeys_.clear();
  if (!pendingStructuralSnapshot_ || pendingLocations_.empty())
    return;

  std::vector<std::string> keys;
  keys.reserve(pendingLocations_.size());
  for (const TurnSection &section : pendingStructuralSnapshot_->sections) {
    bool childrenPresented = true;
    if (section.rootCardKey) {
      const std::string rootKey = stableKey(*section.rootCardKey);
      const auto root = std::ranges::find_if(
          section.cards, [&rootKey](const VisibleCardData &card) {
            return stableKey(card.key) == rootKey;
          });
      if (root != section.cards.end())
        childrenPresented = !cardCollapsed(*root, rootKey);
    }
    for (const VisibleCardData &card : section.cards) {
      const std::string key = stableKey(card.key);
      const PendingLocation *location = pendingLocation(key);
      if (model_->isPresented(card) &&
          (!location || !location->nested || childrenPresented))
        keys.push_back(key);
    }
  }

  const int minimumExtent = MinimumEstimatedCardHeight + CardSpacing;
  const std::size_t budget = static_cast<std::size_t>(
      (std::max(1, viewport()->height()) + minimumExtent - 1) / minimumExtent +
      2);
  std::size_t first = keys.size() > budget ? keys.size() - budget : 0;
  std::optional<Anchor> anchor;
  if (pendingStructuralSnapshot_->threadId == threadId_ &&
      mode_ == Mode::Paused) {
    anchor = captureAnchor();
  } else if (const auto saved = threadPresentations_.find(
                 pendingStructuralSnapshot_->threadId);
             saved != threadPresentations_.end() &&
             saved->second.scroll.mode == Mode::Paused) {
    anchor = saved->second.scroll.anchor;
  }
  if (anchor) {
    const auto found = std::ranges::find(keys, anchor->stableKey);
    if (found != keys.end()) {
      const std::size_t anchorIndex =
          static_cast<std::size_t>(std::distance(keys.begin(), found));
      const int visiblePrefix = std::max(0, anchor->pixelOffset);
      const std::size_t preceding = static_cast<std::size_t>(
          (visiblePrefix + minimumExtent - 1) / minimumExtent +
          (visiblePrefix > 0 ? 1 : 0));
      first = anchorIndex > preceding ? anchorIndex - preceding : 0;
      if (first + budget > keys.size())
        first = keys.size() > budget ? keys.size() - budget : 0;
    }
  }
  const std::size_t last = std::min(keys.size(), first + budget);
  for (std::size_t index = last; index-- > first;) {
    const std::string &key = keys[index];
    VisibleCardData *card = pendingCard(key);
    if (!card)
      continue;
    const auto retained = materializedCards_.find(key);
    if (retained != materializedCards_.end() &&
        retained->second->canApply(*card))
      continue;
    if (stagedCards_.contains(key))
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

void ConversationView::scheduleCardAdmissionPass() {
  if (cardAdmissionPassScheduled_)
    return;
  cardAdmissionPassScheduled_ = true;
  QTimer::singleShot(1, Qt::PreciseTimer, this, [this] {
    cardAdmissionPassScheduled_ = false;
    runCardAdmissionPass();
  });
}

void ConversationView::runCardAdmissionPass() {
  if (!pendingStructuralSnapshot_) {
    if (!loadingThreadId_.empty() || applying_ || materializing_ ||
        adjustingScrollRange_ || interactiveResize_)
      return;
    updateMaterialization(true);
    return;
  }
  while (!pendingStructuralCardKeys_.empty()) {
    std::string key = std::move(pendingStructuralCardKeys_.back());
    pendingStructuralCardKeys_.pop_back();
    VisibleCardData *data = pendingCard(key);
    const PendingLocation *location = pendingLocation(key);
    if (!data || !location || stagedCards_.contains(key))
      continue;
    const int width = std::max(
        0, viewport()->width() - (location->nested ? 2 * NestedCardIndent : 0));
    ConversationCard *card =
        createCard(*data, stagingHost_, key, width, cardCollapsed(*data, key),
                   location->nested);
    static_cast<void>(card->settleHeightForWidth(width));
    card->setViewportVisible(false);
    card->hide();
    stagedCards_.emplace(key, card);
    incrementProperty(this, "structuralStageCardPasses");
    scheduleCardAdmissionPass();
    return;
  }

  ConversationSnapshot completed = std::move(*pendingStructuralSnapshot_);
  const std::string targetThreadId = completed.threadId;
  pendingStructuralSnapshot_.reset();
  pendingLocations_.clear();
  std::vector<nodegraph::NodeRef> acknowledged;
  std::optional<std::string> committedThread;
  ConversationItemModel::StructuralChangeResult result;
  {
    const QScopedValueRollback committing(committingStructuralStage_, true);
    result =
        reconcileOwned(std::move(completed), acknowledged, committedThread);
    clearStagedCards();
  }
  if (result == ConversationItemModel::StructuralChangeResult::Changed)
    incrementProperty(this, "structuralStageCommits");
  if (nextOverscanAdmissionRow() >= 0)
    scheduleCardAdmissionPass();
  publishReconciliation(targetThreadId, result, committedThread.has_value(),
                        std::move(acknowledged));
}

void ConversationView::clearStagedCards() {
  while (!stagedCards_.empty()) {
    auto card = stagedCards_.extract(stagedCards_.begin());
    delete card.mapped();
  }
}

void ConversationView::cancelStructuralStaging() {
  const QScopedValueRollback committing(committingStructuralStage_, true);
  pendingStructuralSnapshot_.reset();
  pendingLocations_.clear();
  pendingStructuralCardKeys_.clear();
  clearStagedCards();
}

bool ConversationView::finishThreadSelection(const std::string &threadId) {
  if (!loadingThreadId_.empty() && !threadId.empty() &&
      loadingThreadId_ != threadId)
    return false;
  if (loadingThreadId_.empty() && !stagingOverlay_->isVisible())
    return false;
  loadingThreadId_.clear();
  stagingOverlay_->finish();
  return true;
}

std::optional<PresentationImpact>
ConversationView::applyConversationDelta(ConversationDelta delta) {
  if (applying_ || committingStructuralStage_)
    return std::nullopt;
  const bool stagedThread =
      pendingStructuralSnapshot_ &&
      pendingStructuralSnapshot_->threadId == delta.threadId;
  const bool presentationOnly = !delta.presentations.empty();
  const bool structural = !delta.rows.empty() || !delta.removals.empty();
  if ((!stagedThread && delta.threadId != threadId_) ||
      (pendingStructuralSnapshot_ && structural) ||
      (presentationOnly && structural))
    return std::nullopt;
  if (!presentationOnly && !structural)
    return PresentationImpact::None;

  if (presentationOnly) {
    const Mode pendingModeBefore = modeForThread(delta.threadId);
    std::unordered_set<std::string> keys;
    keys.reserve(delta.presentations.size());
    for (const VisibleCardData &card : delta.presentations) {
      const std::string key = stableKey(card.key);
      if (key.empty() || card.threadId != delta.threadId ||
          !keys.insert(key).second) {
        return std::nullopt;
      }
      if (stagedThread) {
        if (const VisibleCardData *pending = pendingCard(key)) {
          if (pending->key != card.key || pending->kind != card.kind ||
              pending->target != card.target) {
            return std::nullopt;
          }
          if (const auto staged = stagedCards_.find(key);
              staged != stagedCards_.end() && !staged->second->canApply(card)) {
            return std::nullopt;
          }
        }
      }
      if (delta.threadId == threadId_) {
        const ConversationItemModel::CardUpdateResult admission =
            model_->cardUpdateResult(card);
        if (admission == ConversationItemModel::CardUpdateResult::Missing) {
          if (!stagedThread || !pendingCard(key)) {
            return std::nullopt;
          }
          continue;
        }
        if (admission == ConversationItemModel::CardUpdateResult::Incompatible) {
          return std::nullopt;
        }
        if (ConversationCard *visible = cardForStableKey(key);
            visible && !visible->canApply(card)) {
          return std::nullopt;
        }
      }
    }

    PresentationImpact aggregateImpact = PresentationImpact::None;
    bool pendingMembershipChanged = false;
    const Anchor presentationAnchor = captureAnchor();
    const bool followedBefore = mode_ == Mode::Following;
    int damageTop = viewport()->height();
    bool geometryChanged = false;
    const QScopedValueRollback applying(applying_, true);
    for (VisibleCardData &card : delta.presentations) {
      const std::string key = stableKey(card.key);
      if (stagedThread) {
        if (VisibleCardData *pending = pendingCard(key)) {
          const bool wasPresented = model_->isPresented(*pending);
          const PendingLocation *location = pendingLocation(key);
          if (const auto staged = stagedCards_.find(key);
              staged != stagedCards_.end()) {
            const PresentationImpact impact =
                staged->second->applyPresentation(card);
            if (impact == PresentationImpact::GeometryChanged) {
              const int width = std::max(0, viewport()->width() -
                                                (location && location->nested
                                                     ? 2 * NestedCardIndent
                                                     : 0));
              static_cast<void>(staged->second->settleHeightForWidth(width));
            }
          }
          normalizeRetainedState(card, location && location->nested);
          *pending = card;
          pendingMembershipChanged =
              pendingMembershipChanged ||
              wasPresented != model_->isPresented(*pending);
        }
      }
      if (delta.threadId == threadId_ &&
          model_->indexForStableKey(key).isValid()) {
        const auto impact = applyCardPresentationOwned(
            std::move(card), damageTop, geometryChanged);
        if (!impact) {
          return std::nullopt;
        }
        aggregateImpact = std::max(aggregateImpact, *impact);
      }
    }
    if (stagedThread && (pendingMembershipChanged ||
                         pendingModeBefore != modeForThread(delta.threadId))) {
      clearStagedCards();
      choosePendingStageRows();
      scheduleCardAdmissionPass();
    }
    if (geometryChanged) {
      {
        const QSignalBlocker blocked(verticalScrollBar());
        restoreViewport(presentationAnchor, followedBefore);
      }
      updateMaterialization();
      damageTop = std::clamp(damageTop, 0, viewport()->height());
      viewport()->update(QRect(0, damageTop, viewport()->width(),
                               viewport()->height() - damageTop));
    }
    return aggregateImpact;
  }

  const Anchor anchor = captureAnchor();
  std::unordered_map<std::string, bool> rootChildrenPresented;
  for (const nodegraph::NodeRef &target : delta.removals) {
    const QModelIndex index = model_->indexForTarget(target);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (index.isValid() && row && row->turnRoot)
      rootChildrenPresented.emplace(row->sectionKey,
                                    !cardCollapsed(row->card, row->stableKey));
  }
  std::vector<nodegraph::NodeRef> acknowledged;
  std::vector<PromptMaterialization> materializedPrompts =
      std::move(delta.materializedPrompts);
  std::unordered_set<std::string> promptTransitions;
  promptTransitions.reserve(materializedPrompts.size());
  for (const PromptMaterialization &materialization : materializedPrompts) {
    const std::string key = stableKey(materialization.cardKey);
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || (row && row->card.kind == CardKind::LocalPrompt))
      promptTransitions.insert(key);
  }
  {
    const QScopedValueRollback applying(applying_, true);
    const QSignalBlocker scrollSignals(verticalScrollBar());
    const auto plan = model_->applyStructuralDelta(delta.rows, delta.removals);
    if (!plan)
      return std::nullopt;

    bool changed = false;
    for (const ConversationItemModel::StructuralDeltaPlan::Operation
             &operation : plan->operations) {
      if (!operation.changed)
        continue;
      changed = true;
      const bool retiresIdentity =
          operation.kind == ConversationItemModel::StructuralDeltaPlan::
                                Operation::Kind::Remove ||
          operation.kind == ConversationItemModel::StructuralDeltaPlan::
                                Operation::Kind::ReplaceRoot;
      if (retiresIdentity) {
        if (const auto found = materializedCards_.find(operation.oldStableKey);
            found != materializedCards_.end()) {
          ConversationCard *card = found->second;
          materializedCards_.erase(found);
          releaseCard(operation.oldStableKey, card);
        }
        forgetCardPresentation(delta.threadId, operation.oldStableKey);
      }

      if (!operation.stableKey.empty()) {
        const QModelIndex index =
            model_->indexForStableKey(operation.stableKey);
        const ConversationItemModel::Row *row = model_->row(index.row());
        if (index.isValid() && row) {
          normalizeRetainedState(row->card, row->nested);
        }
      }
    }

    for (PromptMaterialization &materialization : materializedPrompts) {
      const std::string key = stableKey(materialization.cardKey);
      const bool established =
          std::ranges::any_of(plan->operations, [&](const auto &operation) {
            return operation.changed && operation.stableKey == key;
          });
      const QModelIndex index = model_->indexForStableKey(key);
      const ConversationItemModel::Row *row = model_->row(index.row());
      if (promptTransitions.contains(key) && established && index.isValid() &&
          row && row->card.kind == CardKind::UserMessage)
        acknowledged.push_back(std::move(materialization.prompt));
    }

    const bool historyChromeChanged =
        model_->setProviderHasMore(delta.providerHasMore);
    updateHistoryControls();
    if (changed) {
      stopFollowingAnimation();
      finishStructuralDelta(anchor, plan->operations, rootChildrenPresented);
    } else if (historyChromeChanged) {
      const bool follow = mode_ == Mode::Following;
      restoreViewport(anchor, follow);
      updateMaterialization();
      restoreAnchor(anchor, follow);
      viewport()->update();
    }
  }

  publishReconciliation(std::nullopt, ReconciliationResult::Changed, false,
                        std::move(acknowledged));
  return PresentationImpact::None;
}
void ConversationView::finishStructuralDelta(
    const Anchor &anchor,
    std::span<const ConversationItemModel::StructuralDeltaPlan::Operation>
        operations,
    const std::unordered_map<std::string, bool> &rootChildrenPresented) {
  const bool emitsStructuralSignal =
      std::ranges::any_of(operations, [](const auto &operation) {
        return operation.changed &&
               (operation.kind != ConversationItemModel::StructuralDeltaPlan::
                                      Operation::Kind::Place ||
                operation.source != operation.destination);
      });
  std::unordered_map<std::string, SectionRange> previousSections;
  std::unordered_set<std::string> touchedSections;
  std::unordered_set<std::string> affectedKeys;
  std::unordered_set<std::string> childrenVisibilityChanged;
  std::unordered_map<std::string, int> preferredRows;
  const auto touchSection = [&](const std::string &section, int preferred) {
    if (section.empty())
      return;
    if (touchedSections.insert(section).second) {
      if (const auto found = sectionRanges_.find(section);
          found != sectionRanges_.end())
        previousSections.emplace(section, found->second);
      preferredRows.emplace(section, preferred);
    } else if (preferred >= 0) {
      int &retained = preferredRows[section];
      retained = retained < 0 ? preferred : std::min(retained, preferred);
    }
  };
  for (const ConversationItemModel::StructuralDeltaPlan::Operation &operation :
       operations) {
    if (!operation.changed)
      continue;
    touchSection(operation.oldSection, operation.source);
    touchSection(operation.section, operation.destination);
    affectedKeys.insert(operation.oldStableKey);
    affectedKeys.insert(operation.stableKey);

    if (operation.kind == ConversationItemModel::StructuralDeltaPlan::
                              Operation::Kind::ReplaceRoot)
      continue;
    if (operation.source < 0) {
      const std::array<int, 1> inserted{0};
      heights_.insert(static_cast<std::size_t>(operation.destination),
                      inserted);
    } else if (operation.destination < 0) {
      heights_.remove(static_cast<std::size_t>(operation.source), 1);
    } else if (operation.source != operation.destination) {
      heights_.move(static_cast<std::size_t>(operation.source), 1,
                    static_cast<std::size_t>(operation.destination));
    }
  }

  const auto nearSectionRow = [this](const std::string &section,
                                     int preferred) {
    if (preferred >= 0 && preferred < model_->rowCount()) {
      const ConversationItemModel::Row *candidate = model_->row(preferred);
      if (candidate && candidate->sectionKey == section)
        return preferred;
    }
    for (const int neighbor : {preferred - 1, preferred + 1}) {
      if (neighbor < 0 || neighbor >= model_->rowCount())
        continue;
      const ConversationItemModel::Row *candidate = model_->row(neighbor);
      if (candidate && candidate->sectionKey == section)
        return neighbor;
    }
    return -1;
  };
  const auto updateSection = [&](const std::string &section, int preferred) {
    if (section.empty())
      return;
    const auto previous = previousSections.find(section);
    SectionRange replacement =
        previous == previousSections.end() ? SectionRange{} : previous->second;
    bool rescan = false;
    for (const ConversationItemModel::StructuralDeltaPlan::Operation
             &operation : operations) {
      if (!operation.changed ||
          operation.kind != ConversationItemModel::StructuralDeltaPlan::
                                Operation::Kind::ReplaceRoot ||
          operation.oldSection != section || operation.section != section)
        continue;
      if (replacement.root == operation.oldStableKey)
        replacement.root = operation.stableKey;
      if (replacement.first == operation.oldStableKey)
        replacement.first = operation.stableKey;
      if (replacement.last == operation.oldStableKey)
        replacement.last = operation.stableKey;
    }
    const auto validRow = [this, &section](const std::string &key,
                                           bool requireRoot) {
      const std::optional<int> position = modelSectionRow(key);
      const ConversationItemModel::Row *row =
          position ? model_->row(*position) : nullptr;
      return row && row->sectionKey == section &&
             (!requireRoot || row->turnRoot);
    };
    if (!replacement.root.empty() && !validRow(replacement.root, true))
      replacement.root.clear();
    if (!replacement.first.empty() &&
        (!validRow(replacement.first, false) ||
         !rowPresented(*modelSectionRow(replacement.first)))) {
      replacement.first.clear();
      rescan = true;
    }
    if (!replacement.last.empty() &&
        (!validRow(replacement.last, false) ||
         !rowPresented(*modelSectionRow(replacement.last)))) {
      replacement.last.clear();
      rescan = true;
    }

    for (const ConversationItemModel::StructuralDeltaPlan::Operation
             &operation : operations) {
      if (!operation.changed)
        continue;
      if (previous != previousSections.end() && operation.source >= 0 &&
          operation.destination >= 0 &&
          operation.source != operation.destination &&
          operation.oldSection == section &&
          (previous->second.first == operation.oldStableKey ||
           previous->second.last == operation.oldStableKey))
        rescan = true;
      if (operation.section != section || operation.stableKey.empty())
        continue;
      const QModelIndex changedIndex =
          model_->indexForStableKey(operation.stableKey);
      const ConversationItemModel::Row *changed =
          model_->row(changedIndex.row());
      if (!changedIndex.isValid() || !changed || changed->sectionKey != section)
        continue;
      if (changed->turnRoot)
        replacement.root = operation.stableKey;
      if (!rowPresented(changedIndex.row())) {
        if (previous != previousSections.end() &&
            (previous->second.first == operation.oldStableKey ||
             previous->second.last == operation.oldStableKey))
          rescan = true;
        continue;
      }
      const std::optional<int> first = modelSectionRow(replacement.first);
      const std::optional<int> last = modelSectionRow(replacement.last);
      if (!first || changedIndex.row() < *first)
        replacement.first = operation.stableKey;
      if (!last || changedIndex.row() > *last)
        replacement.last = operation.stableKey;
    }

    if (const auto beforeChildren = rootChildrenPresented.find(section);
        beforeChildren != rootChildrenPresented.end()) {
      const std::optional<int> root = modelSectionRow(replacement.root);
      const ConversationItemModel::Row *rootRow =
          root ? model_->row(*root) : nullptr;
      const bool afterChildren =
          !rootRow || !cardCollapsed(rootRow->card, rootRow->stableKey);
      if (beforeChildren->second != afterChildren) {
        childrenVisibilityChanged.insert(section);
        rescan = true;
      }
    }

    if (rescan) {
      rebuildSectionRange(section, nearSectionRow(section, preferred));
      return;
    }
    if (replacement.first.empty() && replacement.root.empty()) {
      sectionRanges_.erase(section);
    } else {
      sectionRanges_.insert_or_assign(section, std::move(replacement));
    }
  };
  for (const std::string &section : touchedSections)
    updateSection(section, preferredRows.at(section));

  for (const std::string &section : touchedSections) {
    const auto before = previousSections.find(section);
    const auto after = sectionRanges_.find(section);
    if (before != previousSections.end()) {
      affectedKeys.insert(before->second.first);
      affectedKeys.insert(before->second.last);
      affectedKeys.insert(before->second.root);
    }
    if (after != sectionRanges_.end()) {
      affectedKeys.insert(after->second.first);
      affectedKeys.insert(after->second.last);
      affectedKeys.insert(after->second.root);
    }
    const std::string beforeRoot =
        before == previousSections.end() ? std::string{} : before->second.root;
    const std::string afterRoot =
        after == sectionRanges_.end() ? std::string{} : after->second.root;
    if (beforeRoot.empty() != afterRoot.empty() ||
        childrenVisibilityChanged.contains(section)) {
      const auto range = sectionRanges_.find(section);
      const std::optional<int> member =
          range == sectionRanges_.end()
              ? std::nullopt
              : modelSectionRow(!range->second.first.empty()
                                    ? range->second.first
                                    : range->second.root);
      if (!member)
        continue;
      int first = *member;
      while (first > 0 && model_->row(first - 1)->sectionKey == section)
        --first;
      for (int rowIndex = first; rowIndex < model_->rowCount(); ++rowIndex) {
        const ConversationItemModel::Row *row = model_->row(rowIndex);
        if (!row || row->sectionKey != section)
          break;
        affectedKeys.insert(row->stableKey);
      }
    }
  }

  const auto refreshExtent = [this](const std::string &key) {
    if (key.empty())
      return;
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row)
      return;
    if (!rowPresented(index.row())) {
      static_cast<void>(
          heights_.setHeight(static_cast<std::size_t>(index.row()), 0));
      return;
    }
    int cardHeight = estimatedCardHeight(*row);
    if (const HeightRecord *height = retainedHeight(*row))
      cardHeight = height->height;
    static_cast<void>(
        heights_.setHeight(static_cast<std::size_t>(index.row()),
                           std::max(1, cardHeight) + rowSpacing(index.row())));
  };
  for (const std::string &key : affectedKeys)
    refreshExtent(key);

  std::vector<std::string> released;
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row || !rowPresented(index.row()) ||
        !card->canApply(row->card)) {
      released.push_back(key);
      continue;
    }
    if (!affectedKeys.contains(key))
      continue;
    if (card->data() != row->card)
      static_cast<void>(card->applyPresentation(row->card));
    configureCardForRow(card, *row);
    const int height = card->settleHeightForWidth(rowWidth(*row));
    retainCardState(row->card.threadId, key).height =
        HeightRecord{rowWidth(*row), height};
    static_cast<void>(heights_.setHeight(static_cast<std::size_t>(index.row()),
                                         height + rowSpacing(index.row())));
  }
  for (const std::string &key : released) {
    const auto found = materializedCards_.find(key);
    if (found == materializedCards_.end())
      continue;
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    const std::string ownerThread = card->data().threadId;
    const QModelIndex retainedIndex = model_->indexForStableKey(key);
    const ConversationItemModel::Row *retainedRow =
        retainedIndex.isValid() ? model_->row(retainedIndex.row()) : nullptr;
    const bool forget = !retainedRow || (rowPresented(retainedIndex.row()) &&
                                         !card->canApply(retainedRow->card));
    releaseCard(key, card);
    if (forget)
      forgetCardPresentation(ownerThread, key);
  }

  empty_->setVisible(model_->rowCount() == 0);
  setProperty("conversationHeightIndexUpdateSteps",
              static_cast<qulonglong>(heights_.lastUpdateSteps()));
  const bool follow = mode_ == Mode::Following;
  restoreViewport(anchor, follow);
  updateMaterialization();
  restoreAnchor(anchor, follow);
  const bool touchesTurnSurface =
      std::ranges::any_of(touchedSections, [&](const std::string &section) {
        const auto before = previousSections.find(section);
        const auto after = sectionRanges_.find(section);
        return (before != previousSections.end() &&
                hasTurnSurface(before->second)) ||
               (after != sectionRanges_.end() && hasTurnSurface(after->second));
      });
  if (!emitsStructuralSignal || touchesTurnSurface)
    viewport()->update();

  incrementProperty(this, "graphRefreshPasses");
}

void ConversationView::setHistoryRequestPending(const std::string &threadId,
                                                bool pending) {
  bool &current = threadPresentations_[threadId].historyRequestPending;
  if (current == pending)
    return;
  current = pending;
  if (threadId_ == threadId)
    updateHistoryControls();
}

void ConversationView::setProviderHasMore(const std::string &threadId,
                                          bool hasMore) {
  if (threadId != threadId_ || !model_->setProviderHasMore(hasMore))
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  updateHistoryControls();
  {
    const QSignalBlocker blocked(verticalScrollBar());
    restoreViewport(anchor, follow);
  }
  updateMaterialization();
  viewport()->update();
}

void ConversationView::forgetThreadPresentation(const std::string &threadId,
                                                std::uint64_t incarnation) {
  const auto current = threadPresentations_.find(threadId);
  if (incarnation != 0 && (current == threadPresentations_.end() ||
                           current->second.incarnation != incarnation))
    return;
  if (pendingStructuralSnapshot_ &&
      pendingStructuralSnapshot_->threadId == threadId)
    cancelStructuralStaging();
  if (loadingThreadId_ == threadId) {
    loadingThreadId_.clear();
    stagingOverlay_->finish();
  }
  if (threadId_ == threadId) {
    stopFollowingAnimation();
    releaseAllCards();
    pausedByCommandOutput_ = false;
    threadId_.clear();
    mode_ = Mode::Following;
    static_cast<void>(model_->replaceConversation({}));
    rebuildSectionRanges();
    rebuildHeightIndex();
    loadMore_->hide();
    empty_->show();
    updateScrollRange();
    setScrollValue(0);
    viewport()->update();
  }
  threadPresentations_.erase(threadId);
}

void ConversationView::updateHistoryControls() {
  const bool hasMore = model_->hasMore();
  const auto history = threadPresentations_.find(threadId_);
  const bool pending = history != threadPresentations_.end() &&
                       history->second.historyRequestPending;
  const bool wasBusy = loadMore_->isVisible() && !loadMore_->isEnabled();
  QString label = QStringLiteral("Load more activities");
  QString toolTip;
  QString description;
  if (pending) {
    label = toolTip = QStringLiteral("Loading earlier activities");
    description = QStringLiteral("A history page request is in progress");
  } else if (hasMore) {
    label = QStringLiteral("Load earlier activities");
    toolTip = description = QStringLiteral("Earlier activities are available");
  }
  if (loadMore_->text() != label)
    loadMore_->setText(label);
  if (loadMore_->toolTip() != toolTip)
    loadMore_->setToolTip(toolTip);
  presentation::setAccessibleDescriptionIfChanged(*loadMore_, description);
  loadMore_->setEnabled(hasMore && !pending);
  loadMore_->setVisible(hasMore);
#if QT_CONFIG(accessibility)
  notifyBusyChange(*viewport(), wasBusy,
                   loadMore_->isVisible() && !loadMore_->isEnabled());
#endif
  empty_->setVisible(model_->rowCount() == 0);
}

std::optional<PresentationImpact>
ConversationView::applyCardPresentationOwned(VisibleCardData card,
                                             int &damageTop,
                                             bool &geometryChanged) {
  const std::string key = stableKey(card.key);
  if (card.threadId != threadId_)
    return std::nullopt;
  const QModelIndex index = model_->indexForStableKey(key);
  const ConversationItemModel::Row *before = model_->row(index.row());
  if (!index.isValid() || !before)
    return std::nullopt;
  const QScopedValueRollback applying(applying_, true);
  const bool wasPresented = rowPresented(index.row());
  const std::string ownerThread = before->card.threadId;
  const std::string sectionKey = before->sectionKey;
  std::optional<SectionRange> oldSection;
  if (const auto found = sectionRanges_.find(sectionKey);
      found != sectionRanges_.end())
    oldSection = found->second;
  QRect presentationDamage = rowRect(index.row());
  if (oldSection) {
    const std::optional<int> root = modelSectionRow(oldSection->root);
    const std::optional<int> last = modelSectionRow(oldSection->last);
    if (root && last)
      presentationDamage =
          presentationDamage.united(rowRect(*root)).united(rowRect(*last));
  }
  const bool paintedInViewport =
      wasPresented && rowRect(index.row()).intersects(viewport()->rect());
  const bool offscreenContentMayReflow =
      before->card.kind != card.kind || before->card.payload != card.payload;
  ConversationCard *visibleCard = cardForStableKey(key);
  if (visibleCard && !visibleCard->canApply(card))
    return std::nullopt;
  const bool cardNeedsLivePresentation =
      visibleCard &&
      (!visibleCard->isHidden() ||
       owningCard(*this, QApplication::focusWidget()) == visibleCard);

  const ConversationItemModel::CardUpdateResult result =
      model_->updateCard(std::move(card));
  if (result == ConversationItemModel::CardUpdateResult::Missing ||
      result == ConversationItemModel::CardUpdateResult::Incompatible)
    return std::nullopt;
  if (result == ConversationItemModel::CardUpdateResult::Unchanged)
    return PresentationImpact::None;

  const ConversationItemModel::Row *after = model_->row(index.row());
  if (after)
    normalizeRetainedState(after->card, after->nested);
  PresentationImpact impact = PresentationImpact::None;
  if (cardNeedsLivePresentation && after)
    impact = visibleCard->applyPresentation(after->card);
  const bool presentationChanged =
      after && rowPresented(index.row()) != wasPresented;
  if (presentationChanged) {
    if (auto thread = threadPresentations_.find(ownerThread);
        thread != threadPresentations_.end()) {
      if (auto retained = thread->second.cards.find(key);
          retained != thread->second.cards.end())
        retained->second.height.reset();
    }
    updateSectionRangeForPresentationChange(index.row(), wasPresented);
    const auto nextSectionFound = sectionRanges_.find(sectionKey);
    const SectionRange *nextSection = nextSectionFound == sectionRanges_.end()
                                          ? nullptr
                                          : &nextSectionFound->second;

    std::unordered_set<int> affectedRows{index.row()};
    if (oldSection) {
      if (const std::optional<int> row = modelSectionRow(oldSection->root))
        affectedRows.insert(*row);
      if (const std::optional<int> row = modelSectionRow(oldSection->last))
        affectedRows.insert(*row);
    }
    if (nextSection) {
      if (const std::optional<int> row = modelSectionRow(nextSection->root))
        affectedRows.insert(*row);
      if (const std::optional<int> row = modelSectionRow(nextSection->last))
        affectedRows.insert(*row);
    }

    for (const int affectedRow : affectedRows) {
      const ConversationItemModel::Row *affected = model_->row(affectedRow);
      if (!affected || affected->sectionKey != sectionKey)
        continue;
      if (!rowPresented(affectedRow)) {
        static_cast<void>(
            heights_.setHeight(static_cast<std::size_t>(affectedRow), 0));
        continue;
      }

      const int previousExtent =
          heights_.height(static_cast<std::size_t>(affectedRow));
      int cardHeight = 0;
      if (previousExtent > 0) {
        cardHeight =
            std::max(1, previousExtent -
                            rowSpacing(affectedRow,
                                       oldSection ? &*oldSection : nullptr));
      } else if (const HeightRecord *height = retainedHeight(*affected)) {
        cardHeight = height->height;
      } else {
        cardHeight = estimatedCardHeight(*affected);
      }
      static_cast<void>(heights_.setHeight(
          static_cast<std::size_t>(affectedRow),
          std::max(1, cardHeight) + rowSpacing(affectedRow, nextSection)));
    }

    if (!rowPresented(index.row()) && visibleCard) {
      materializedCards_.erase(key);
      releaseCard(key, visibleCard);
      visibleCard = nullptr;
    }

    for (const int affectedRow : affectedRows) {
      const ConversationItemModel::Row *affected = model_->row(affectedRow);
      if (!affected || !rowPresented(affectedRow) || !affected->turnRoot)
        continue;
      ConversationCard *rootCard = cardForStableKey(affected->stableKey);
      if (!rootCard)
        continue;
      configureCardForRow(rootCard, *affected);
      const int height = rootCard->settleHeightForWidth(rowWidth(*affected));
      retainCardState(affected->card.threadId, affected->stableKey).height =
          HeightRecord{rowWidth(*affected), height};
      static_cast<void>(
          heights_.setHeight(static_cast<std::size_t>(affectedRow),
                             height + rowSpacing(affectedRow, nextSection)));
    }

    impact = PresentationImpact::GeometryChanged;
    geometryChanged = true;
    incrementProperty(this, "conversationLocalGeometryPasses");
    setProperty("conversationHeightIndexUpdateSteps",
                static_cast<qulonglong>(heights_.lastUpdateSteps()));
    if (nextSection) {
      const std::optional<int> root = modelSectionRow(nextSection->root);
      const std::optional<int> last = modelSectionRow(nextSection->last);
      if (root && last)
        presentationDamage =
            presentationDamage.united(rowRect(*root)).united(rowRect(*last));
    }
    damageTop = std::min(damageTop, presentationDamage.top());
  } else if (visibleCard && impact == PresentationImpact::GeometryChanged) {
    const int height = visibleCard->settleHeightForWidth(rowWidth(*after));
    geometryChanged = setMeasuredHeight(index.row(), height) || geometryChanged;
    damageTop = std::min(damageTop, rowRect(index.row()).top());
  } else if (visibleCard && impact == PresentationImpact::PaintOnly) {
    visibleCard->update();
  } else if (after && paintedInViewport) {
    geometryChanged = true;
    damageTop = std::min(damageTop, rowRect(index.row()).top());
    impact = PresentationImpact::GeometryChanged;
  } else if (after && offscreenContentMayReflow) {
    if (auto thread = threadPresentations_.find(ownerThread);
        thread != threadPresentations_.end()) {
      if (auto retained = thread->second.cards.find(key);
          retained != thread->second.cards.end())
        retained->second.height.reset();
    }
    const int estimate =
        std::max(1, estimatedCardHeight(*after)) + rowSpacing(index.row());
    if (heights_.setHeight(static_cast<std::size_t>(index.row()), estimate)) {
      incrementProperty(this, "conversationLocalGeometryPasses");
      setProperty("conversationHeightIndexUpdateSteps",
                  static_cast<qulonglong>(heights_.lastUpdateSteps()));
      geometryChanged = true;
      damageTop = std::min(damageTop, rowRect(index.row()).top());
      impact = PresentationImpact::GeometryChanged;
    } else {
      impact = PresentationImpact::PaintOnly;
    }
  }

  if (!visibleCard && !paintedInViewport)
    incrementProperty(this, "targetedOffscreenCardUpdates");
  incrementProperty(this, "graphRefreshPasses");
  incrementProperty(this, "targetedCardCommits");
  return impact;
}

const ConversationView::RetainedCardState *
ConversationView::retainedCardState(const std::string &threadId,
                                    const std::string &stableKey) const {
  const auto thread = threadPresentations_.find(threadId);
  if (thread == threadPresentations_.end())
    return nullptr;
  const auto card = thread->second.cards.find(stableKey);
  return card == thread->second.cards.end() ? nullptr : &card->second;
}

const ConversationView::HeightRecord *
ConversationView::retainedHeight(const ConversationItemModel::Row &row) const {
  const RetainedCardState *retained =
      retainedCardState(row.card.threadId, row.stableKey);
  return retained && retained->height &&
                 retained->height->width == rowWidth(row)
             ? &*retained->height
             : nullptr;
}

ConversationView::RetainedCardState &
ConversationView::retainCardState(const std::string &threadId,
                                  const std::string &stableKey) {
  return threadPresentations_[threadId].cards[stableKey];
}

void ConversationView::forgetCardPresentation(const std::string &threadId,
                                              const std::string &stableKey) {
  const auto thread = threadPresentations_.find(threadId);
  if (thread == threadPresentations_.end())
    return;
  thread->second.cards.erase(stableKey);
  handleCommandOutputFollowLatest(threadId, true);
}

int ConversationView::estimatedCardHeight(
    const ConversationItemModel::Row &row) const {
  const VisibleCardData &card = row.card;
  if (cardCollapsed(card, row.stableKey) && card.kind != CardKind::LocalPrompt)
    return MinimumEstimatedCardHeight;
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

bool ConversationView::cardCollapsed(const VisibleCardData &card,
                                     const std::string &stableKey) const {
  if (const RetainedCardState *retained =
          retainedCardState(card.threadId, stableKey);
      retained && retained->collapsed)
    return *retained->collapsed;
  if (card.kind == CardKind::CommandExecution)
    return !presentationOptions_.commandsInitiallyExpanded;
  if (card.kind == CardKind::ImageGeneration)
    return !presentationOptions_.imagesInitiallyExpanded;
  if (card.kind == CardKind::FileChanges)
    return !presentationOptions_.fileChangesInitiallyExpanded;
  return collapsedByDefault(card.kind);
}

void ConversationView::retainAdmissionDefaults() {
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row || (row->card.kind != CardKind::CommandExecution &&
                 row->card.kind != CardKind::ImageGeneration &&
                 row->card.kind != CardKind::FileChanges))
      continue;
    const std::string key = row->stableKey;
    if (const RetainedCardState *retained =
            retainedCardState(row->card.threadId, key);
        retained && retained->collapsed)
      continue;
    retainCardState(row->card.threadId, key).collapsed =
        cardCollapsed(row->card, row->stableKey);
  }
}

void ConversationView::normalizeRetainedState(const VisibleCardData &card,
                                              bool nested) {
  const std::string key = stableKey(card.key);
  if (card.threadId == threadId_ && cardForStableKey(key))
    return;
  const auto thread = threadPresentations_.find(card.threadId);
  if (thread == threadPresentations_.end())
    return;
  const auto retained = thread->second.cards.find(key);
  if (retained == thread->second.cards.end() || !retained->second.interaction)
    return;
  const bool releasedDetachedOwner = ConversationCard::normalizeState(
      *retained->second.interaction, card, nested);
  if (retained->second.interaction->empty())
    retained->second.interaction.reset();
  if (releasedDetachedOwner)
    handleCommandOutputFollowLatest(card.threadId, true);
}

int ConversationView::rowWidth(const ConversationItemModel::Row &row) const {
  return std::max(0, viewport()->width() -
                         (row.nested ? 2 * NestedCardIndent : 0));
}

bool ConversationView::rowPresented(int rowIndex) const {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !row->presented)
    return false;
  if (!row->nested)
    return true;
  const auto section = sectionRanges_.find(row->sectionKey);
  if (section == sectionRanges_.end() || section->second.root.empty())
    return true;
  const std::optional<int> rootIndex = modelSectionRow(section->second.root);
  const ConversationItemModel::Row *rootRow =
      rootIndex ? model_->row(*rootIndex) : nullptr;
  return !rootRow || !cardCollapsed(rootRow->card, rootRow->stableKey);
}

void ConversationView::rebuildSectionRanges() {
  sectionRanges_.clear();
  sectionRanges_.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row)
      continue;
    if (row->turnRoot)
      sectionRanges_[row->sectionKey].root = row->stableKey;
  }
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row || !rowPresented(rowIndex))
      continue;
    SectionRange &range = sectionRanges_[row->sectionKey];
    if (range.first.empty())
      range.first = row->stableKey;
    range.last = row->stableKey;
  }
  incrementProperty(this, "conversationSectionRangeRebuilds");
}

void ConversationView::rebuildSectionRange(const std::string &sectionKey,
                                           int nearRow) {
  if (sectionKey.empty())
    return;
  if (nearRow < 0 || nearRow >= model_->rowCount() ||
      model_->row(nearRow)->sectionKey != sectionKey) {
    const auto retained = sectionRanges_.find(sectionKey);
    const std::optional<int> retainedRow =
        retained == sectionRanges_.end()
            ? std::nullopt
            : modelSectionRow(!retained->second.first.empty()
                                  ? retained->second.first
                                  : retained->second.root);
    if (!retainedRow) {
      sectionRanges_.erase(sectionKey);
      return;
    }
    nearRow = *retainedRow;
  }

  int first = nearRow;
  while (first > 0) {
    const ConversationItemModel::Row *candidate = model_->row(first - 1);
    if (!candidate || candidate->sectionKey != sectionKey)
      break;
    --first;
  }
  SectionRange replacement;
  int end = first;
  for (; end < model_->rowCount(); ++end) {
    const ConversationItemModel::Row *candidate = model_->row(end);
    if (!candidate || candidate->sectionKey != sectionKey)
      break;
    if (candidate->turnRoot)
      replacement.root = candidate->stableKey;
  }
  const std::optional<int> rootRow = modelSectionRow(replacement.root);
  const ConversationItemModel::Row *root =
      rootRow ? model_->row(*rootRow) : nullptr;
  const bool childrenPresented =
      !root || !cardCollapsed(root->card, root->stableKey);
  for (int candidateIndex = first; candidateIndex < end; ++candidateIndex) {
    const ConversationItemModel::Row *candidate = model_->row(candidateIndex);
    if (!candidate->presented || (candidate->nested && !childrenPresented))
      continue;
    if (replacement.first.empty())
      replacement.first = candidate->stableKey;
    replacement.last = candidate->stableKey;
  }
  if (replacement.first.empty() && replacement.root.empty()) {
    sectionRanges_.erase(sectionKey);
  } else {
    sectionRanges_.insert_or_assign(sectionKey, std::move(replacement));
  }
}

void ConversationView::updateSectionRangeForPresentationChange(
    int rowIndex, bool wasPresented) {
  const ConversationItemModel::Row *changed = model_->row(rowIndex);
  if (!changed || rowPresented(rowIndex) == wasPresented)
    return;

  if (rowPresented(rowIndex)) {
    SectionRange &range = sectionRanges_[changed->sectionKey];
    const std::optional<int> first = modelSectionRow(range.first);
    const std::optional<int> last = modelSectionRow(range.last);
    if (!first || rowIndex < *first)
      range.first = changed->stableKey;
    if (!last || rowIndex > *last)
      range.last = changed->stableKey;
    if (changed->turnRoot)
      range.root = changed->stableKey;
    return;
  }

  // Hiding a targeted row is uncommon (global visibility changes use the
  // structural rebuild path). Recompute only its canonical turn, never the
  // loaded conversation.
  rebuildSectionRange(changed->sectionKey, rowIndex);
}

int ConversationView::rowSpacing(int rowIndex) const {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row)
    return CardSpacing;
  const auto found = sectionRanges_.find(row->sectionKey);
  return rowSpacing(rowIndex,
                    found == sectionRanges_.end() ? nullptr : &found->second);
}

bool ConversationView::hasTurnSurface(const SectionRange &section) {
  return !section.root.empty() && !section.last.empty() &&
         section.root != section.last;
}

int ConversationView::rowSpacing(int rowIndex,
                                 const SectionRange *section) const {
  if (!section || !hasTurnSurface(*section))
    return CardSpacing;
  const std::optional<int> root = modelSectionRow(section->root);
  const std::optional<int> last = modelSectionRow(section->last);
  if (!root || !last || *last <= *root)
    return CardSpacing;
  if (rowIndex == *root)
    return 14;
  if (rowIndex == *last)
    return CardSpacing + TurnSurfaceBottomPadding;
  return CardSpacing;
}

std::optional<int>
ConversationView::modelSectionRow(const std::string &stableKey) const {
  if (stableKey.empty())
    return std::nullopt;
  const QModelIndex index = model_->indexForStableKey(stableKey);
  return index.isValid() ? std::optional<int>(index.row()) : std::nullopt;
}

void ConversationView::rebuildHeightIndex() {
  std::vector<int> extents;
  extents.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row || !rowPresented(rowIndex)) {
      extents.push_back(0);
      continue;
    }
    const int width = rowWidth(*row);
    int height = 0;
    if (const auto staged = stagedCards_.find(row->stableKey);
        committingStructuralStage_ && staged != stagedCards_.end()) {
      height = staged->second->height();
      retainCardState(row->card.threadId, row->stableKey).height =
          HeightRecord{width, height};
    } else if (const HeightRecord *retained = retainedHeight(*row)) {
      height = retained->height;
    } else {
      height = estimatedCardHeight(*row);
    }
    extents.push_back(std::max(1, height) + rowSpacing(rowIndex));
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
  return static_cast<qint64>(leadingChromeHeight()) + heights_.totalHeight();
}

void ConversationView::updateScrollRange() {
  if (!model_ || !viewport())
    return;
  const QScopedValueRollback adjusting(adjustingScrollRange_, true);
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

void ConversationView::beginInteractiveResize() {
  if (interactiveResize_)
    return;
  interactiveResize_ = true;
  stopFollowingAnimation();
  setProperty("conversationInteractiveResizeActive", true);
}

void ConversationView::endInteractiveResize() {
  if (!interactiveResize_)
    return;
  interactiveResize_ = false;
  resizeFrameTimer_->stop();
  resizeSettleTimer_->stop();
  reflowAfterResize(ReflowCause::ResizeExact);
  setProperty("conversationInteractiveResizeActive", false);
  incrementProperty(this, "conversationInteractiveResizeSettlements");
}

void ConversationView::scheduleInteractiveResizeReflow() {
  if (!resizeFrameTimer_->isActive())
    resizeFrameTimer_->start();
  // This is a safety net for a platform that ends a native mouse grab without
  // delivering the corresponding release to the splitter handle.
  resizeSettleTimer_->start();
}

void ConversationView::reflowAfterResize(ReflowCause cause) {
  const bool invalidateEnvironment = cause == ReflowCause::Environment;
  if (applying_ || materializing_) {
    if (cause == ReflowCause::ResizeFrame)
      scheduleInteractiveResizeReflow();
    return;
  }
  if (invalidateEnvironment)
    stopFollowingAnimation();
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;

  if (invalidateEnvironment) {
    for (auto &presentation : threadPresentations_ | std::views::values)
      for (RetainedCardState &retained :
           presentation.cards | std::views::values)
        retained.height.reset();
  }
  for (auto &[key, card] : stagedCards_) {
    if (invalidateEnvironment)
      card->invalidateGeometryEnvironment();
    const PendingLocation *location = pendingLocation(key);
    static_cast<void>(card->settleHeightForWidth(std::max(
        1, viewport()->width() -
               (location && location->nested ? 2 * NestedCardIndent : 0))));
  }
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row =
        index.isValid() ? model_->row(index.row()) : nullptr;
    if (!index.isValid() || !row)
      continue;
    if (invalidateEnvironment)
      card->invalidateGeometryEnvironment();
    const int width = rowWidth(*row);
    const int height = card->settleHeightForWidth(width);
    retainCardState(row->card.threadId, key).height =
        HeightRecord{width, height};
    if (!invalidateEnvironment)
      static_cast<void>(
          heights_.setHeight(static_cast<std::size_t>(index.row()),
                             height + rowSpacing(index.row())));
  }
  if (invalidateEnvironment)
    rebuildHeightIndex();
  restoreViewport(anchor, follow);
  updateMaterialization();
  viewport()->update();
  if (invalidateEnvironment) {
    incrementProperty(this, "conversationGeometryEnvironmentReflows");
  } else if (cause == ReflowCause::ResizeExact) {
    incrementProperty(this, "conversationInteractiveResizeExactReflows");
  } else {
    incrementProperty(this, "conversationInteractiveResizeFrameReflows");
  }
}

QRect ConversationView::rowRect(int rowIndex) const {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !rowPresented(rowIndex) || rowIndex < 0 ||
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
          rowWidth(*row), std::max(1, extent - rowSpacing(rowIndex))};
}

bool ConversationView::updateMeasuredHeight(int rowIndex, int cardHeight,
                                            bool preserveAnchor) {
  const Anchor anchor = preserveAnchor ? captureAnchor() : Anchor{};
  const bool follow = mode_ == Mode::Following;
  if (!setMeasuredHeight(rowIndex, cardHeight))
    return false;
  updateScrollRange();
  if (follow || preserveAnchor)
    restoreAnchor(anchor, follow);
  else
    layoutMaterializedCards();
  return true;
}

bool ConversationView::setMeasuredHeight(int rowIndex, int cardHeight) {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !rowPresented(rowIndex))
    return false;
  retainCardState(row->card.threadId, row->stableKey).height =
      HeightRecord{rowWidth(*row), cardHeight};
  if (!heights_.setHeight(static_cast<std::size_t>(rowIndex),
                          std::max(1, cardHeight) + rowSpacing(rowIndex)))
    return false;
  incrementProperty(this, "conversationLocalGeometryPasses");
  setProperty("conversationHeightIndexUpdateSteps",
              static_cast<qulonglong>(heights_.lastUpdateSteps()));
  return true;
}

std::pair<int, int>
ConversationView::materializationRows(int overscanViewports) const {
  if (model_->rowCount() == 0 || heights_.empty() ||
      heights_.totalHeight() <= 0)
    return {-1, -1};
  const qint64 scroll = verticalScrollBar()->value();
  const qint64 viewportHeight = std::max(1, viewport()->height());
  const qint64 overscan =
      static_cast<qint64>(std::max(0, overscanViewports)) * viewportHeight;
  const qint64 contentStart =
      std::max<qint64>(0, scroll - leadingChromeHeight() - overscan);
  const qint64 contentEnd = std::min<qint64>(heights_.totalHeight() - 1,
                                             scroll - leadingChromeHeight() +
                                                 viewportHeight - 1 + overscan);
  if (contentEnd < contentStart)
    return {-1, -1};
  const int first = static_cast<int>(heights_.rowAt(contentStart));
  const int last = static_cast<int>(heights_.rowAt(contentEnd));
  return {std::max(0, first), std::min(model_->rowCount() - 1, last)};
}

int ConversationView::nextOverscanAdmissionRow() const {
  const auto [visibleFirst, visibleLast] = materializationRows(0);
  const auto [first, last] = materializationRows();
  if (visibleFirst < 0 || first < 0)
    return -1;
  int closest = -1;
  for (std::size_t rowIndex = heights_.nextRowWithExtent(first);
       rowIndex < heights_.size() && rowIndex <= static_cast<std::size_t>(last);
       rowIndex = heights_.nextRowWithExtent(rowIndex + 1)) {
    const int candidate = static_cast<int>(rowIndex);
    const ConversationItemModel::Row *row = model_->row(candidate);
    if (!row)
      continue;
    const ConversationCard *card = cardForStableKey(row->stableKey);
    const bool current = card && card->data() == row->card;
    if (candidate >= visibleFirst && candidate <= visibleLast) {
      if (!current)
        return candidate;
      continue;
    }
    if (current)
      continue;
    if (candidate < visibleFirst) {
      closest = candidate;
      continue;
    }
    if (closest < 0 || candidate - visibleLast < visibleFirst - closest)
      closest = candidate;
    break;
  }
  return closest;
}

ConversationCard *ConversationView::createCard(const VisibleCardData &data,
                                               QWidget *parent,
                                               const std::string &key,
                                               int width, bool collapsed,
                                               bool nested) {
  QElapsedTimer constructionTimer;
  constructionTimer.start();
  auto *card = new ConversationCard(data, collapsed, parent, width);
  card->setNestedPresentation(nested);
  connect(card, &ConversationCard::foldRequested, this,
          [this, key, card](bool collapsed) {
            const auto retained = materializedCards_.find(key);
            if (retained != materializedCards_.end() &&
                retained->second == card)
              setCardCollapsed(key, card, collapsed);
          });
  connect(card, &ConversationCard::intrinsicGeometryChanged, this,
          [this, key, card] {
            const auto retained = materializedCards_.find(key);
            if (retained == materializedCards_.end() ||
                retained->second != card)
              return;
            const QModelIndex index = model_->indexForStableKey(key);
            if (!index.isValid())
              return;
            const ConversationItemModel::Row *row = model_->row(index.row());
            if (!row)
              return;
            const int height = card->settleHeightForWidth(rowWidth(*row));
            static_cast<void>(updateMeasuredHeight(index.row(), height, true));
          });
  connect(card, &ConversationCard::recoveryRequested, this, [this, key, card] {
    const auto retained = materializedCards_.find(key);
    if (retained == materializedCards_.end() || retained->second != card)
      return;
    const auto action = promptRecoveryAction_;
    nodegraph::NodeRef target = card->data().target;
    if (action && target)
      action(std::move(target));
  });
  connect(card, &ConversationCard::noticeRequested, this,
          [this](QString message, bool error) {
            if (noticeAction_)
              noticeAction_(std::move(message), error);
          });
  if (auto *output = card->findChild<CommandOutputView *>(
          QStringLiteral("commandOutputView"))) {
    connect(output, &CommandOutputView::followLatestChanged, this,
            [this, ownerThread = data.threadId](bool followsLatest) {
              handleCommandOutputFollowLatest(ownerThread, followsLatest);
            });
  }
  card->setProperty("conversationConstructionMicros",
                    constructionTimer.nsecsElapsed() / 1000);
  return card;
}

bool ConversationView::configureCardForRow(
    ConversationCard *card, const ConversationItemModel::Row &row) {
  if (!card)
    return false;
  const auto section = sectionRanges_.find(row.sectionKey);
  const std::optional<int> root = section == sectionRanges_.end()
                                      ? std::nullopt
                                      : modelSectionRow(section->second.root);
  const std::optional<int> last = section == sectionRanges_.end()
                                      ? std::nullopt
                                      : modelSectionRow(section->second.last);
  const bool fragmentedRoot = row.turnRoot && root && last && *last > *root;
  const bool nestedChanged = card->setNestedPresentation(row.nested);
  const bool rootGeometryChanged =
      card->setVirtualTurnRootPresentation(fragmentedRoot);
  card->setAuthoritativeTurnActive(row.turnRoot && row.activeTurn &&
                                   !fragmentedRoot);
  const QModelIndex current = currentIndex();
  card->setShowsKeyboardFocus(
      hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange) &&
      current.isValid() && model_->row(current.row()) == &row);
  return nestedChanged || rootGeometryChanged;
}

bool ConversationView::materializeRow(int rowIndex) {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !rowPresented(rowIndex))
    return false;
  if (ConversationCard *existing = cardForStableKey(row->stableKey)) {
    if (existing->data() == row->card)
      return false;
    if (!existing->canApply(row->card))
      return false;
    const PresentationImpact impact = existing->applyPresentation(row->card);
    configureCardForRow(existing, *row);
    if (impact != PresentationImpact::GeometryChanged)
      return false;
    const int height = existing->settleHeightForWidth(rowWidth(*row));
    retainCardState(row->card.threadId, row->stableKey).height =
        HeightRecord{rowWidth(*row), height};
    return heights_.setHeight(static_cast<std::size_t>(rowIndex),
                              height + rowSpacing(rowIndex));
  }

  ConversationCard *card = nullptr;
  if (const auto staged = stagedCards_.find(row->stableKey);
      committingStructuralStage_ && staged != stagedCards_.end() &&
      staged->second->canApply(row->card)) {
    card = staged->second;
    stagedCards_.erase(staged);
    card->setParent(viewport());
  } else {
    card = createCard(row->card, viewport(), row->stableKey, rowWidth(*row),
                      cardCollapsed(row->card, row->stableKey), row->nested);
    incrementProperty(this, "conversationCardConstructions");
  }
  if (const RetainedCardState *retained =
          retainedCardState(row->card.threadId, row->stableKey);
      retained && retained->interaction)
    card->restoreState(*retained->interaction);
  configureCardForRow(card, *row);
  card->ensurePolished();
  const int height = card->settleHeightForWidth(rowWidth(*row));
  retainCardState(row->card.threadId, row->stableKey).height =
      HeightRecord{rowWidth(*row), height};
  const bool heightChanged = heights_.setHeight(
      static_cast<std::size_t>(rowIndex), height + rowSpacing(rowIndex));
  materializedCards_.emplace(row->stableKey, card);
  const QRect geometry = rowRect(rowIndex);
  const bool visible = geometry.intersects(viewport()->rect());
  card->setGeometry(geometry);
  card->setViewportVisible(visible);
  card->setVisible(visible);
  return heightChanged;
}

void ConversationView::releaseCard(const std::string &key,
                                   ConversationCard *card,
                                   ConversationCard *deferredEventReceiver) {
  if (!card)
    return;
  RetainedCardState &retained = retainCardState(card->data().threadId, key);
  retained.collapsed = card->isCollapsed();
  ConversationCard::State interaction = card->state();
  if (interaction.empty())
    retained.interaction.reset();
  else
    retained.interaction =
        std::make_unique<ConversationCard::State>(std::move(interaction));
  card->setViewportVisible(false);
  if (card == deferredEventReceiver) {
    card->hide();
    card->setParent(this);
    card->deleteLater();
  } else {
    delete card;
  }
  incrementProperty(this, "conversationRowsReleased");
}

void ConversationView::releaseUnneededCards(
    int firstRow, int lastRow, ConversationCard *deferredEventReceiver) {
  ConversationCard *const focusedCard =
      owningCard(*this, QApplication::focusWidget());
  std::vector<std::string> removeKeys;
  for (const auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const bool presented = index.isValid() && rowPresented(index.row());
    if (!presented || ((index.row() < firstRow || index.row() > lastRow) &&
                       card != focusedCard))
      removeKeys.push_back(key);
  }
  for (const std::string &key : removeKeys) {
    auto released = materializedCards_.extract(key);
    if (!released.empty())
      releaseCard(released.key(), released.mapped(), deferredEventReceiver);
  }
}

void ConversationView::releaseAllCards() {
  while (!materializedCards_.empty()) {
    ConversationCard *card =
        materializedCards_.extract(materializedCards_.begin()).mapped();
    delete card;
    incrementProperty(this, "conversationRowsReleased");
  }
}

void ConversationView::updateMaterialization(bool admitOverscan) {
  if (materializing_)
    return;
  const QScopedValueRollback materializing(materializing_, true);
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;

  if (admitOverscan) {
    const int admissionRow = nextOverscanAdmissionRow();
    if (admissionRow < 0)
      return;
    if (materializeRow(admissionRow))
      restoreViewport(anchor, follow);
    incrementProperty(this, "conversationCardAdmissionPasses");
  } else {
    bool heightChanged = false;
    constexpr int MaximumVisibleSettlementPasses = 4;
    for (int pass = 0; pass < MaximumVisibleSettlementPasses; ++pass) {
      incrementProperty(this, "conversationMaterializationPasses");
      const auto [first, last] = materializationRows(0);
      bool passChanged = false;
      if (first >= 0)
        for (std::size_t rowIndex = heights_.nextRowWithExtent(first);
             rowIndex < heights_.size() &&
             rowIndex <= static_cast<std::size_t>(last);
             rowIndex = heights_.nextRowWithExtent(rowIndex + 1))
          passChanged = materializeRow(static_cast<int>(rowIndex)) ||
                        passChanged;
      heightChanged = heightChanged || passChanged;
      if (!passChanged)
        break;
    }
    if (heightChanged)
      restoreViewport(anchor, follow);
    else
      layoutMaterializedCards();
  }

  const auto finalRows = materializationRows();
  releaseUnneededCards(finalRows.first, finalRows.second);
  if (nextOverscanAdmissionRow() >= 0)
    scheduleCardAdmissionPass();
}

void ConversationView::layoutMaterializedCards() {
  const QRect visibleRect = viewport()->rect();
  ConversationCard *const focusedCard =
      owningCard(*this, QApplication::focusWidget());
  for (const auto &[key, card] : materializedCards_) {
    static_cast<void>(key);
    card->setViewportVisible(false);
    if (card != focusedCard)
      card->hide();
  }
  const auto [first, last] = materializationRows();
  if (first < 0)
    return;
  std::size_t rowIndex = heights_.nextRowWithExtent(first);
  qint64 top = rowIndex < heights_.size() ? heights_.top(rowIndex) : 0;
  for (; rowIndex < heights_.size() &&
         rowIndex <= static_cast<std::size_t>(last);
       rowIndex = heights_.nextRowWithExtent(rowIndex + 1)) {
    const ConversationItemModel::Row *row =
        model_->row(static_cast<int>(rowIndex));
    if (!row) {
      top += heights_.height(rowIndex);
      continue;
    }
    const auto found = materializedCards_.find(row->stableKey);
    if (found == materializedCards_.end()) {
      top += heights_.height(rowIndex);
      continue;
    }
    ConversationCard *card = found->second;
    const QRect geometry(row->nested ? NestedCardIndent : 0,
                         static_cast<int>(leadingChromeHeight() + top -
                                          verticalScrollBar()->value()),
                         rowWidth(*row),
                         std::max(0, heights_.height(rowIndex) -
                                         rowSpacing(rowIndex)));
    const bool visible = geometry.intersects(visibleRect);
    const bool ownsFocus = !visible && card == focusedCard;
    if (visible || ownsFocus || !card->isHidden())
      card->setGeometry(geometry);
    card->setViewportVisible(visible);
    card->setVisible(visible || ownsFocus);
    top += heights_.height(rowIndex);
  }
}

ConversationCard *
ConversationView::cardForStableKey(const std::string &key) const {
  const auto found = materializedCards_.find(key);
  return found == materializedCards_.end() ? nullptr : found->second;
}

void ConversationView::updateFocusDecoration(const QModelIndex &index) {
  if (ConversationCard *card =
          cardForStableKey(index.data(ConversationItemModel::StableKeyRole)
                               .toString()
                               .toStdString()))
    card->setShowsKeyboardFocus(
        index.isValid() && currentIndex() == index && hasFocus() &&
        window()->testAttribute(Qt::WA_KeyboardFocusChange));
}

void ConversationView::setCardCollapsed(const std::string &key,
                                        ConversationCard *card,
                                        bool collapsed) {
  if (!card || card->isCollapsed() == collapsed)
    return;
  const QModelIndex index = model_->indexForStableKey(key);
  if (!index.isValid())
    return;
  const QRect geometryBefore = rowRect(index.row());
  Anchor anchor = captureAnchor();
  anchor.stableKey = key;
  anchor.pixelOffset = rowRect(index.row()).top();
  mode_ = Mode::Paused;
  pausedByCommandOutput_ = false;
  stopFollowingAnimation();
  card->setCollapsed(collapsed);
  RetainedCardState &retained = retainCardState(card->data().threadId, key);
  retained.collapsed = collapsed;
  ConversationCard::State interaction = card->state();
  if (interaction.empty())
    retained.interaction.reset();
  else
    retained.interaction =
        std::make_unique<ConversationCard::State>(std::move(interaction));
  const ConversationItemModel::Row *row = model_->row(index.row());
  if (!row)
    return;

  if (row->turnRoot) {
    SectionRange replacement;
    int first = index.row();
    while (first > 0) {
      const ConversationItemModel::Row *candidate = model_->row(first - 1);
      if (!candidate || candidate->sectionKey != row->sectionKey)
        break;
      --first;
    }
    int last = first;
    for (; last < model_->rowCount(); ++last) {
      const ConversationItemModel::Row *candidate = model_->row(last);
      if (!candidate || candidate->sectionKey != row->sectionKey)
        break;
      if (!rowPresented(last))
        continue;
      if (replacement.first.empty())
        replacement.first = candidate->stableKey;
      replacement.last = candidate->stableKey;
      if (candidate->turnRoot)
        replacement.root = candidate->stableKey;
    }
    if (replacement.first.empty() && replacement.root.empty())
      sectionRanges_.erase(row->sectionKey);
    else
      sectionRanges_.insert_or_assign(row->sectionKey, replacement);

    const SectionRange *section = nullptr;
    if (const auto found = sectionRanges_.find(row->sectionKey);
        found != sectionRanges_.end())
      section = &found->second;
    for (int affectedRow = first; affectedRow < last; ++affectedRow) {
      const ConversationItemModel::Row *affected = model_->row(affectedRow);
      if (!affected)
        continue;
      if (!rowPresented(affectedRow)) {
        static_cast<void>(
            heights_.setHeight(static_cast<std::size_t>(affectedRow), 0));
        continue;
      }
      int cardHeight = estimatedCardHeight(*affected);
      if (const HeightRecord *height = retainedHeight(*affected))
        cardHeight = height->height;
      static_cast<void>(heights_.setHeight(
          static_cast<std::size_t>(affectedRow),
          std::max(1, cardHeight) + rowSpacing(affectedRow, section)));
    }
    configureCardForRow(card, *row);
    const int rootHeight = card->settleHeightForWidth(rowWidth(*row));
    retainCardState(row->card.threadId, key).height =
        HeightRecord{rowWidth(*row), rootHeight};
    static_cast<void>(
        heights_.setHeight(static_cast<std::size_t>(index.row()),
                           rootHeight + rowSpacing(index.row(), section)));
    incrementProperty(this, "conversationLocalGeometryPasses");
    setProperty("conversationHeightIndexUpdateSteps",
                static_cast<qulonglong>(heights_.lastUpdateSteps()));
    restoreViewport(anchor);
    updateMaterialization();
  } else {
    const int height = card->settleHeightForWidth(rowWidth(*row));
    static_cast<void>(updateMeasuredHeight(index.row(), height, false));
  }
  restoreAnchor(anchor);
  if (!collapsed) {
    const QRect expanded = rowRect(index.row());
    const int availableBottom = std::max(0, viewport()->height() - 1);
    if (!expanded.isEmpty() && expanded.bottom() > availableBottom)
      setScrollValue(verticalScrollBar()->value() + expanded.bottom() -
                     availableBottom);
  }
  const QRect geometryAfter = rowRect(index.row());
  if (!geometryBefore.isEmpty() || !geometryAfter.isEmpty()) {
    const int firstAffectedY =
        std::clamp(std::min(geometryBefore.isEmpty() ? geometryAfter.top()
                                                     : geometryBefore.top(),
                            geometryAfter.isEmpty() ? geometryBefore.top()
                                                    : geometryAfter.top()) -
                       CardFrameExtent,
                   0, viewport()->height());
    // Every later row changes viewport position when one variable-height row
    // expands or collapses. Real child widgets repaint themselves when moved;
    // invalidate the affected visible suffix only after the final anchor and
    // scroll position have been established.
    viewport()->update(0, firstAffectedY, viewport()->width(),
                       viewport()->height() - firstAffectedY);
  }
}

bool ConversationView::isAtBottom() const noexcept {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
}

ConversationView::Mode
ConversationView::modeForThread(const std::string &threadId) const noexcept {
  if (threadId == threadId_)
    return mode_;
  const auto saved = threadPresentations_.find(threadId);
  return saved == threadPresentations_.end() ? Mode::Following
                                             : saved->second.scroll.mode;
}

ConversationView::Anchor ConversationView::captureAnchor() const {
  Anchor anchor;
  anchor.absoluteValue = verticalScrollBar()->value();
  if (model_->rowCount() == 0 || heights_.empty() ||
      heights_.totalHeight() <= 0)
    return anchor;
  const qint64 contentY =
      std::max<qint64>(0, static_cast<qint64>(verticalScrollBar()->value()) -
                              leadingChromeHeight());
  const std::size_t rowIndex =
      heights_.nextRowWithExtent(heights_.rowAt(contentY));
  if (rowIndex >= heights_.size())
    return anchor;
  const ConversationItemModel::Row *row =
      model_->row(static_cast<int>(rowIndex));
  if (!row)
    return anchor;
  anchor.stableKey = row->stableKey;
  anchor.pixelOffset = rowRect(static_cast<int>(rowIndex)).top();
  if (!row->turnRoot || row->card.kind == CardKind::LocalPrompt ||
      !retainedHeight(*row))
    return anchor;

  // The Turn root decorates the continuous surface that follows it. When that
  // surface is visible, anchor its first child so provider-prepended rows do
  // not move the activity the user was reading.
  for (std::size_t next = heights_.nextRowWithExtent(rowIndex + 1);
       next < heights_.size(); next = heights_.nextRowWithExtent(next + 1)) {
    const ConversationItemModel::Row *activity =
        model_->row(static_cast<int>(next));
    if (!activity || activity->sectionKey != row->sectionKey)
      break;
    const int activityTop = rowRect(static_cast<int>(next)).top();
    if (activityTop >= viewport()->height())
      break;
    anchor.stableKey = activity->stableKey;
    anchor.pixelOffset = activityTop;
    break;
  }
  return anchor;
}

void ConversationView::restoreAnchor(const Anchor &anchor, bool follow) {
  if (follow) {
    setScrollValue(verticalScrollBar()->maximum());
    return;
  }
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
}

void ConversationView::restoreViewport(const Anchor &anchor, bool follow) {
  updateScrollRange();
  restoreAnchor(anchor, follow);
}

void ConversationView::setScrollValue(int value) {
  value = std::clamp(value, verticalScrollBar()->minimum(),
                     verticalScrollBar()->maximum());
  const QScopedValueRollback programmatic(programmaticScroll_, true);
  const bool laysOutThroughSignal = value != verticalScrollBar()->value() &&
                                    !verticalScrollBar()->signalsBlocked();
  verticalScrollBar()->setValue(value);
  if (!laysOutThroughSignal)
    layoutMaterializedCards();
}

void ConversationView::stopFollowingAnimation() {
  if (followAnimation_->state() != QAbstractAnimation::Stopped)
    followAnimation_->stop();
}

void ConversationView::handleCommandOutputFollowLatest(
    const std::string &ownerThread, bool followsLatest) {
  if (ownerThread != threadId_) {
    auto saved = threadPresentations_.find(ownerThread);
    if (!followsLatest || saved == threadPresentations_.end() ||
        !saved->second.scroll.pausedByCommandOutput ||
        hasDetachedCommandOutput(ownerThread))
      return;
    saved->second.scroll.pausedByCommandOutput = false;
    if (saved->second.scroll.mode == Mode::Paused)
      saved->second.scroll.mode = Mode::Following;
    return;
  }
  if (!followsLatest) {
    if (mode_ != Mode::Following && !pausedByCommandOutput_)
      return;
    if (!pausedByCommandOutput_) {
      stopFollowingAnimation();
      mode_ = Mode::Paused;
      pausedByCommandOutput_ = true;
    }
    return;
  }
  if (!pausedByCommandOutput_ || hasDetachedCommandOutput(ownerThread))
    return;
  pausedByCommandOutput_ = false;
  if (mode_ != Mode::Paused)
    return;
  mode_ = Mode::Following;
  if (applying_)
    setScrollValue(verticalScrollBar()->maximum());
  else
    animateToBottom(verticalScrollBar()->value());
}

bool ConversationView::hasDetachedCommandOutput(
    const std::string &ownerThread) const {
  for (const auto &[key, card] : materializedCards_) {
    if (card->data().threadId != ownerThread)
      continue;
    const auto *output = card->findChild<CommandOutputView *>(
        QStringLiteral("commandOutputView"));
    if (output && !output->followsLatest())
      return true;
  }
  const auto thread = threadPresentations_.find(ownerThread);
  if (thread == threadPresentations_.end())
    return false;
  return std::ranges::any_of(thread->second.cards, [&](const auto &entry) {
    const auto &[key, state] = entry;
    return !materializedCards_.contains(key) && state.interaction &&
           state.interaction->commandOutput &&
           !state.interaction->commandOutput->view.followsLatest;
  });
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
  if (distance <= 3 || !UiStyle::animationsEnabled(*this)) {
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
  pausedByCommandOutput_ = false;
  mode_ = value >= verticalScrollBar()->maximum() - 1 ? Mode::Following
                                                      : Mode::Paused;
}

QRect ConversationView::visualRect(const QModelIndex &index) const {
  if (!index.isValid() || index.model() != model_ || index.column() != 0)
    return {};
  return rowRect(index.row());
}

void ConversationView::scrollTo(const QModelIndex &index, ScrollHint hint) {
  if (applying_)
    return;
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
  updateMaterialization();
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
  const bool hadCurrent = currentIndex().isValid();
  int row = hadCurrent ? currentIndex().row() : -1;
  const int count = model_->rowCount();
  int direction = 0;
  switch (cursorAction) {
  case MoveUp:
  case MovePrevious:
    direction = -1;
    if (row >= 0)
      --row;
    break;
  case MoveDown:
  case MoveNext:
    direction = 1;
    if (row >= 0)
      ++row;
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
    if (heights_.empty() || heights_.totalHeight() <= 0)
      return currentIndex();
    direction = cursorAction == MovePageUp ? -1 : 1;
    if (!hadCurrent)
      break;
    const int pageHeight = std::max(1, viewport()->height());
    const int cardHeight = std::max(
        1, heights_.height(static_cast<std::size_t>(row)) - rowSpacing(row));
    if (cardHeight >= pageHeight) {
      row += direction;
      break;
    }
    if (direction > 0) {
      const qint64 boundary =
          heights_.top(static_cast<std::size_t>(row)) + pageHeight;
      int candidate = static_cast<int>(heights_.rowAt(
          std::clamp<qint64>(boundary - 1, 0, heights_.totalHeight() - 1)));
      const qint64 candidateBottom =
          heights_.bottom(static_cast<std::size_t>(candidate)) -
          rowSpacing(candidate);
      if (candidateBottom > boundary && candidate > 0) {
        const std::size_t previous = heights_.previousRowWithExtent(
            static_cast<std::size_t>(candidate - 1));
        candidate =
            previous < heights_.size() ? static_cast<int>(previous) : row;
      }
      row = candidate > row ? candidate : row + 1;
    } else {
      const qint64 boundary =
          heights_.top(static_cast<std::size_t>(row)) + cardHeight - pageHeight;
      int candidate = static_cast<int>(heights_.rowAt(
          std::clamp<qint64>(boundary, 0, heights_.totalHeight() - 1)));
      if (heights_.top(static_cast<std::size_t>(candidate)) < boundary) {
        const std::size_t next =
            heights_.nextRowWithExtent(static_cast<std::size_t>(candidate + 1));
        candidate = next < heights_.size() ? static_cast<int>(next) : row;
      }
      row = candidate < row ? candidate : row - 1;
    }
    break;
  }
  default:
    return currentIndex();
  }
  if (row < 0 && !hadCurrent) {
    row = 0;
    direction = 1;
  }
  if (direction > 0 && row >= 0) {
    const std::size_t next =
        heights_.nextRowWithExtent(static_cast<std::size_t>(row));
    if (next < heights_.size())
      return model_->index(static_cast<int>(next));
  } else if (direction < 0 && row < count) {
    const std::size_t previous = heights_.previousRowWithExtent(
        static_cast<std::size_t>(std::max(0, row)));
    if (previous < heights_.size())
      return model_->index(static_cast<int>(previous));
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
  return !row || !rowPresented(index.row());
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
    for (std::size_t row = heights_.nextRowWithExtent(range.top());
         row < heights_.size() &&
         row <= static_cast<std::size_t>(range.bottom());
         row = heights_.nextRowWithExtent(row + 1))
      region += visualRect(model_->index(static_cast<int>(row)));
  return region;
}

void ConversationView::updateGeometries() {
  if (applying_)
    return;
  updateScrollRange();
  layoutMaterializedCards();
}

void ConversationView::scrollContentsBy(int dx, int dy) {
  if (!programmaticScroll_ && !applying_ && !adjustingScrollRange_) {
    stopFollowingAnimation();
    pausedByCommandOutput_ = false;
    mode_ = isAtBottom() ? Mode::Following : Mode::Paused;
  }
  scrollDirtyRegion(dx, dy);
  viewport()->scroll(dx, dy);
  if (!materializing_ && !adjustingScrollRange_)
    updateMaterialization();
  else
    layoutMaterializedCards();
}

void ConversationView::selectionChanged(const QItemSelection &selected,
                                        const QItemSelection &deselected) {
  QAbstractItemView::selectionChanged(selected, deselected);
#if QT_CONFIG(accessibility)
  const auto notify = [this](const QModelIndex &index,
                             QAccessible::Event type) {
    const auto *row = model_->row(index.row());
    ConversationCard *card = row ? cardForStableKey(row->stableKey) : nullptr;
    if (!card || card->isHidden())
      return;
    if (QAccessibleInterface *interface =
            QAccessible::queryAccessibleInterface(card)) {
      QAccessibleEvent event(interface, type);
      QAccessible::updateAccessibility(&event);
    }
  };
  for (const QModelIndex &index : selected.indexes())
    notify(index, QAccessible::SelectionAdd);
  for (const QModelIndex &index : deselected.indexes())
    notify(index, QAccessible::SelectionRemove);
#endif
}

void ConversationView::currentChanged(const QModelIndex &current,
                                      const QModelIndex &previous) {
  QAbstractItemView::currentChanged(current, previous);
  updateFocusDecoration(previous);
  updateFocusDecoration(current);
#if QT_CONFIG(accessibility)
  const auto *row = model_->row(current.row());
  ConversationCard *card = row ? cardForStableKey(row->stableKey) : nullptr;
  if (hasFocus() && card && !card->isHidden()) {
    if (QAccessibleInterface *interface =
            QAccessible::queryAccessibleInterface(card)) {
      QAccessibleEvent event(interface, QAccessible::Focus);
      QAccessible::updateAccessibility(&event);
    }
  }
#endif
}

void ConversationView::paintEvent(QPaintEvent *event) {
  QPainter painter(viewport());
  painter.fillRect(event->rect(),
                   QColor(QString::fromLatin1(UiStyle::appBackground)));
  if (!heights_.empty() && heights_.totalHeight() > 0) {
    const qint64 firstY = std::max<qint64>(
        0, verticalScrollBar()->value() - leadingChromeHeight() +
               event->rect().top());
    const qint64 lastY =
        std::min<qint64>(heights_.totalHeight() - 1,
                         verticalScrollBar()->value() - leadingChromeHeight() +
                             std::max(0, event->rect().bottom()));
    if (lastY >= firstY) {
      const int first = static_cast<int>(heights_.rowAt(firstY));
      const int last = static_cast<int>(heights_.rowAt(lastY));
      std::unordered_set<std::string> paintedSections;
      painter.setRenderHint(QPainter::Antialiasing);
      painter.setBrush(QColor(QString::fromLatin1(UiStyle::blueSurface)));
      for (std::size_t rowIndex = heights_.nextRowWithExtent(first);
           rowIndex < heights_.size() &&
           rowIndex <= static_cast<std::size_t>(last);
           rowIndex = heights_.nextRowWithExtent(rowIndex + 1)) {
        const ConversationItemModel::Row *row =
            model_->row(static_cast<int>(rowIndex));
        if (!row || !paintedSections.insert(row->sectionKey).second)
          continue;
        const auto section = sectionRanges_.find(row->sectionKey);
        if (section == sectionRanges_.end() || !hasTurnSurface(section->second))
          continue;
        const SectionRange &range = section->second;
        const std::optional<int> root = modelSectionRow(range.root);
        const std::optional<int> sectionLast = modelSectionRow(range.last);
        if (!root || !sectionLast || *sectionLast <= *root)
          continue;
        const ConversationItemModel::Row *rootRow = model_->row(*root);
        const bool active = rootRow && rootRow->activeTurn;
        const qreal top =
            static_cast<qreal>(leadingChromeHeight()) +
            static_cast<qreal>(heights_.top(static_cast<std::size_t>(*root))) -
            verticalScrollBar()->value();
        const qreal bottom =
            static_cast<qreal>(leadingChromeHeight()) +
            static_cast<qreal>(
                heights_.top(static_cast<std::size_t>(*sectionLast))) +
            heights_.height(static_cast<std::size_t>(*sectionLast)) -
            CardSpacing - verticalScrollBar()->value();
        const QRectF surface(0.5, top + 0.5,
                             std::max(0, viewport()->width()) - 1.0,
                             std::max<qreal>(1.0, bottom - top - 1.0));
        painter.setPen(
            QPen(QColor(QString::fromLatin1(active ? UiStyle::activeTurnBorder
                                                   : UiStyle::blueBorder)),
                 active ? 2.0 : 1.0));
        painter.drawRoundedRect(surface, 8.0, 8.0);
      }
    }
  }
}

void ConversationView::mousePressEvent(QMouseEvent *event) {
  window()->setAttribute(Qt::WA_KeyboardFocusChange, false);
  QAbstractItemView::mousePressEvent(event);
  updateFocusDecoration(currentIndex());
}

void ConversationView::resizeEvent(QResizeEvent *event) {
  QAbstractItemView::resizeEvent(event);
  stagingHost_->resize(viewport()->size());
  stagingOverlay_->setGeometry(viewport()->rect());
  if (pendingStructuralSnapshot_ &&
      event->oldSize().height() != event->size().height()) {
    choosePendingStageRows();
    scheduleCardAdmissionPass();
  }
  if (interactiveResize_) {
    // Borders and child widths follow the pointer immediately. Heights remain
    // stable until the coalesced frame pass, preventing every mouse event from
    // synchronously reparsing and measuring the conversation.
    layoutMaterializedCards();
    scheduleInteractiveResizeReflow();
    incrementProperty(this, "conversationInteractiveResizeEvents");
    return;
  }
  stopFollowingAnimation();
  reflowAfterResize(ReflowCause::ResizeExact);
}

void ConversationView::wheelEvent(QWheelEvent *event) {
  QAbstractItemView::wheelEvent(event);
  const int verticalDelta = !event->pixelDelta().isNull()
                                ? event->pixelDelta().y()
                                : event->angleDelta().y();
  if (verticalDelta != 0)
    handleUserScrollValue(verticalScrollBar()->value());
}

bool ConversationView::event(QEvent *event) {
  const bool keyboardInput = event->type() == QEvent::KeyPress;
  const bool focusChange =
      event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut;
  if (keyboardInput)
    window()->setAttribute(Qt::WA_KeyboardFocusChange, true);
  const bool handled = QAbstractItemView::event(event);
  if (keyboardInput || focusChange)
    updateFocusDecoration(currentIndex());
  bool geometryEnvironmentChanged = event->type() == QEvent::FontChange ||
                                    event->type() == QEvent::StyleChange;
  geometryEnvironmentChanged = geometryEnvironmentChanged ||
                               event->type() == QEvent::DevicePixelRatioChange;
  if (geometryEnvironmentChanged && stagingHost_ &&
      !geometryEnvironmentReflowPending_) {
    // Application stylesheet repolish can notify this view before its
    // descendants. Reconcile after that event batch so every renderer cache
    // observes one final effective font/style/DPR.
    geometryEnvironmentReflowPending_ = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
          geometryEnvironmentReflowPending_ = false;
          reflowAfterResize(ReflowCause::Environment);
        },
        Qt::QueuedConnection);
  }
  return handled;
}

} // namespace codexui::codex::middle
