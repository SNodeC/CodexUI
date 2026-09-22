// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"

#include "codex/UiStatus.h"
#include "codex/middle/MiddleTypes.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QActionGroup>
#include <QCollator>
#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int ThreadRowHeight = 40;
constexpr int ThreadIndent = 16;
constexpr int ContentOffset = 2;
constexpr int DisclosureStatusSpacing = 2;
constexpr int StatusRadius = 5;
constexpr int StatusCenterOffset = 12;
constexpr int DisclosureIndicatorOffset =
    ContentOffset + StatusCenterOffset - DisclosureStatusSpacing - StatusRadius;
constexpr int DisclosureHitLeadingInset = 5;
constexpr int DisclosureHitTrailingExtension = 13;
constexpr int DisclosureHitExtent = 24;
constexpr int DisclosureHitVerticalInset =
    (ThreadRowHeight - DisclosureHitExtent) / 2;
constexpr int PendingAnimationIntervalMilliseconds = 32;
constexpr qint64 PendingHalfCycleMilliseconds = 850;

class ThreadTreeStyle final : public QProxyStyle {
public:
  QRect subElementRect(SubElement element, const QStyleOption *option,
                       const QWidget *widget) const override {
    const QRect native = QProxyStyle::subElementRect(element, option, widget);
    if (element != SE_TreeViewDisclosureItem || !option)
      return native;
    // Preserve the former control's target while Qt owns the interaction.
    return option->direction == Qt::RightToLeft
               ? native.adjusted(-DisclosureHitTrailingExtension,
                                 DisclosureHitVerticalInset,
                                 -DisclosureHitLeadingInset,
                                 -DisclosureHitVerticalInset)
               : native.adjusted(DisclosureHitLeadingInset,
                                 DisclosureHitVerticalInset,
                                 DisclosureHitTrailingExtension,
                                 -DisclosureHitVerticalInset);
  }
};

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString activityText(const std::optional<std::int64_t> &timestamp) {
  if (!timestamp)
    return QStringLiteral("Unknown");
  const QDateTime activity =
      QDateTime::fromSecsSinceEpoch(*timestamp).toLocalTime();
  const QDateTime now = QDateTime::currentDateTime();
  return activity.date() == now.date()
             ? activity.toString(QStringLiteral("HH:mm:ss"))
             : activity.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QColor statusColor(const UiStatus &status, std::size_t pending, bool draft) {
  if (draft || pending != 0)
    return QColor(QString::fromLatin1(UiStyle::orange));
  const std::string_view tone = statusTone(status);
  if (tone == "active")
    return QColor(QString::fromLatin1(UiStyle::blue));
  if (tone == "success")
    return QColor(QString::fromLatin1(UiStyle::green));
  if (tone == "warning")
    return QColor(QString::fromLatin1(UiStyle::orange));
  if (tone == "danger")
    return QColor(QString::fromLatin1(UiStyle::red));
  return QColor(QString::fromLatin1(UiStyle::threadInactive));
}

QString itemDescription(const ThreadTreeItem &item);

} // namespace

class ThreadTreeItem final : public QTreeWidgetItem {
public:
  ThreadTreeItem(ThreadTreeWidget *owner, std::string key);
  ~ThreadTreeItem() override;
  bool operator<(const QTreeWidgetItem &other) const override;
  QVariant data(int column, int role) const override;

  ThreadTreeWidget *owner = nullptr;
  std::string id;
  std::string presentationKey;
  nodegraph::NodeRef target;
  std::string title;
  std::string cwd;
  UiStatus status;
  std::optional<std::int64_t> createdAt;
  std::optional<std::int64_t> recencyAt;
  std::optional<std::int64_t> lastActivityAt;
  std::size_t pending = 0;
  bool archived = false;
  bool draft = false;
  bool awaitingPrompt = false;
  qint64 animationEpoch = 0;
#if QT_CONFIG(accessibility)
  QAccessible::Id accessibleId = 0;
#endif
};

class ThreadTreeWidget final : public QTreeWidget {
public:
  explicit ThreadTreeWidget(ThreadPane *owner)
      : QTreeWidget(owner), owner_(owner), collator_(effectiveLocale()) {
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    collator_.setCaseSensitivity(Qt::CaseInsensitive);
    collator_.setIgnorePunctuation(true);
    collator_.setNumericMode(true);
  }

  ~ThreadTreeWidget() override {
    blockSignals(true);
    verticalScrollBar()->blockSignals(true);
    while (topLevelItemCount() != 0)
      deleteItem(threadItem(topLevelItem(0)));
  }

  [[nodiscard]] ThreadTreeItem *threadItem(QTreeWidgetItem *item) const {
    return static_cast<ThreadTreeItem *>(item);
  }
  [[nodiscard]] ThreadTreeItem *threadItem(const QModelIndex &index) const {
    return index.isValid() ? threadItem(itemFromIndex(index)) : nullptr;
  }
  [[nodiscard]] QRect rowRect(const QModelIndex &index) const {
    QRect result = visualRect(index);
    result.setLeft(viewport()->rect().left());
    result.setRight(viewport()->rect().right());
    return result;
  }
  void deleteItem(ThreadTreeItem *item) {
    if (QTreeWidgetItem *parent = item->parent())
      static_cast<void>(parent->takeChild(parent->indexOfChild(item)));
    else if (const int index = indexOfTopLevelItem(item); index >= 0)
      static_cast<void>(takeTopLevelItem(index));
    delete item;
  }
  [[nodiscard]] bool isContextHighlighted(const ThreadTreeItem *item) const {
    return item && item->presentationKey == owner_->contextPresentationKey;
  }
  void restoreViewportY(ThreadTreeItem *item, int y) {
    if (!item)
      return;
    const QRect itemRect = visualItemRect(item);
    if (!itemRect.isValid() || itemRect.isEmpty())
      return;
    const int movement = itemRect.top() - y;
    const int scrollMovement =
        verticalScrollMode() == QAbstractItemView::ScrollPerPixel
            ? movement
            : qRound(qreal(movement) / ThreadRowHeight);
    verticalScrollBar()->setValue(verticalScrollBar()->value() +
                                  scrollMovement);
  }
  template <typename Row>
  [[nodiscard]] bool rowBefore(const Row &left, const Row &right) const {
    const auto criterion = owner_->currentSortCriterion();
    if (criterion == ThreadPane::SortCriterion::Alphanumeric)
      return alphaBefore(left.title, left.id, right.title, right.id);
    const bool created = criterion == ThreadPane::SortCriterion::Created;
    return timestampBefore(created ? left.createdAt : left.recencyAt, left.id,
                           created ? right.createdAt : right.recencyAt,
                           right.id);
  }

#if QT_CONFIG(accessibility)
  [[nodiscard]] QAccessibleInterface *accessibleItem(ThreadTreeItem *item);
  [[nodiscard]] QAccessibleInterface *accessibleItemForEvent(
      ThreadTreeItem *item);
  void retireAccessible(ThreadTreeItem *item);
#endif

protected:
  QItemSelectionModel::SelectionFlags
  selectionCommand(const QModelIndex &index,
                   const QEvent *event = nullptr) const override {
    if (event && (event->type() == QEvent::MouseButtonPress ||
                  event->type() == QEvent::MouseButtonRelease)) {
      const auto *mouse = static_cast<const QMouseEvent *>(event);
      if (mouse->button() == Qt::RightButton)
        return QItemSelectionModel::NoUpdate;
    }
    return QTreeWidget::selectionCommand(index, event);
  }

  void drawBranches(QPainter *, const QRect &,
                    const QModelIndex &) const override {}

  void currentChanged(const QModelIndex &current,
                      const QModelIndex &previous) override;
  void selectionChanged(const QItemSelection &selected,
                        const QItemSelection &deselected) override;

  void changeEvent(QEvent *event) override {
    QTreeWidget::changeEvent(event);
    if (event && event->type() == QEvent::StyleChange)
      owner_->updateAnimationTimer(true);
  }

  void showEvent(QShowEvent *event) override {
    QTreeWidget::showEvent(event);
    owner_->updateAnimationTimer();
    owner_->requestMoreNearListEnd();
  }

  void resizeEvent(QResizeEvent *event) override {
    QTreeWidget::resizeEvent(event);
    owner_->updateAnimationTimer();
    if (event->oldSize().isValid())
      owner_->requestMoreNearListEnd();
  }

  void hideEvent(QHideEvent *event) override {
    QTreeWidget::hideEvent(event);
    owner_->updateAnimationTimer();
  }

private:
  static QLocale effectiveLocale() {
    return QLocale::system().language() == QLocale::C
               ? QLocale(QLocale::English)
               : QLocale::system();
  }

  [[nodiscard]] bool alphaBefore(std::string_view leftTitle,
                                 std::string_view leftId,
                                 std::string_view rightTitle,
                                 std::string_view rightId) const {
    const QString left = text(leftTitle).trimmed();
    const QString right = text(rightTitle).trimmed();
    const bool leftNumeric = !left.isEmpty() && left.front().isDigit();
    const bool rightNumeric = !right.isEmpty() && right.front().isDigit();
    if (leftNumeric != rightNumeric)
      return leftNumeric;
    const int compared = collator_.compare(left, right);
    return compared == 0 ? leftId < rightId : compared < 0;
  }

  [[nodiscard]] static bool timestampBefore(
      const std::optional<std::int64_t> &left, std::string_view leftId,
      const std::optional<std::int64_t> &right, std::string_view rightId) {
    if (left != right) {
      if (!left || !right)
        return left.has_value();
      return *left > *right;
    }
    return leftId < rightId;
  }

  ThreadPane *owner_ = nullptr;
  QCollator collator_;
};

ThreadTreeItem::ThreadTreeItem(ThreadTreeWidget *tree, std::string key)
    : owner(tree), presentationKey(std::move(key)) {
  setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
}

ThreadTreeItem::~ThreadTreeItem() {
#if QT_CONFIG(accessibility)
  if (owner)
    owner->retireAccessible(this);
#endif
}

bool ThreadTreeItem::operator<(const QTreeWidgetItem &other) const {
  const auto &right = static_cast<const ThreadTreeItem &>(other);
  return draft != right.draft ? draft : owner->rowBefore(*this, right);
}

namespace {

QString displayTitle(const ThreadTreeItem &item) {
  QString result = text(item.title);
  if (result.isEmpty())
    result = text(item.id.substr(0, 12));
  if (item.pending != 0)
    result.prepend(QStringLiteral("! "));
  return result;
}

int itemLevel(const ThreadTreeItem *item) {
  int level = 1;
  for (const QTreeWidgetItem *parent = item ? item->parent() : nullptr; parent;
       parent = parent->parent())
    ++level;
  return level;
}

QString itemDescription(const ThreadTreeItem &item) {
  QStringList details{
      displayTitle(item),
      QStringLiteral("Status: %1").arg(text(displayStatus(item.status))),
      QStringLiteral("Workspace: %1")
          .arg(item.cwd.empty() ? QStringLiteral("Unknown") : text(item.cwd)),
      QStringLiteral("Recent turn: %1").arg(activityText(item.recencyAt)),
      QStringLiteral("Created: %1").arg(activityText(item.createdAt)),
      QStringLiteral("Last activity: %1")
          .arg(activityText(item.lastActivityAt)),
      QStringLiteral("Level %1").arg(itemLevel(&item))};
  if (item.pending != 0)
    details.push_back(
        QStringLiteral("%1 pending request(s)").arg(item.pending));
  if (item.draft)
    details.push_back(QStringLiteral("Draft thread"));
  if (item.awaitingPrompt)
    details.push_back(
        QStringLiteral("Waiting for prompt to enter conversation"));
  if (const auto *parent =
          item.parent() ? static_cast<const ThreadTreeItem *>(item.parent())
                        : nullptr)
    details.push_back(QStringLiteral("Parent: %1").arg(displayTitle(*parent)));
  return details.join(QLatin1Char('\n'));
}

bool itemIsVisible(const ThreadTreeWidget &tree, const ThreadTreeItem *item) {
  return item && tree.isVisible() && tree.visualItemRect(item).isValid();
}

bool itemCanAnimate(const ThreadTreeItem &item, qint64 now) {
  if (item.draft)
    return item.animationEpoch > 0;
  return item.awaitingPrompt && item.animationEpoch > 0 &&
         now >= item.animationEpoch + PendingAnimationDelayMilliseconds;
}

class ThreadItemDelegate final : public QStyledItemDelegate {
public:
  explicit ThreadItemDelegate(ThreadTreeWidget *tree)
      : QStyledItemDelegate(tree), tree_(tree) {}

  QSize sizeHint(const QStyleOptionViewItem &,
                 const QModelIndex &) const override {
    return {0, ThreadRowHeight};
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    const ThreadTreeItem *item = tree_->threadItem(index);
    if (!item)
      return;
    const QRect row = tree_->rowRect(index);
    QStyleOptionViewItem effective(option);
    effective.rect = row;
    if (tree_->isContextHighlighted(item))
      effective.state |= QStyle::State_MouseOver;
    QStyledItemDelegate::paint(painter, effective, index);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const bool feedback = item->draft || item->awaitingPrompt;
    const QRectF feedbackSurface = QRectF(row).adjusted(1.0, 4.0, -1.0, -4.0);
    if (feedback) {
      painter->setPen(QPen(
          QColor(QString::fromLatin1(item->draft ? UiStyle::orangeBorderStrong
                                                 : UiStyle::blueBorderStrong)),
          item->draft ? 1.0 : 1.5));
      painter->setBrush(QColor(QString::fromLatin1(
          item->draft ? UiStyle::orangeSurface : UiStyle::blueSurface)));
      painter->drawRoundedRect(feedbackSurface, 8.0, 8.0);
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (UiStyle::animationsEnabled(*tree_) && itemCanAnimate(*item, now)) {
      const qint64 started =
          item->draft
              ? item->animationEpoch
              : item->animationEpoch + PendingAnimationDelayMilliseconds;
      const qint64 phase = (now - started) % (2 * PendingHalfCycleMilliseconds);
      const qreal position =
          phase <= PendingHalfCycleMilliseconds
              ? qreal(phase) / PendingHalfCycleMilliseconds
              : qreal(2 * PendingHalfCycleMilliseconds - phase) /
                    PendingHalfCycleMilliseconds;
      const qreal center =
          feedbackSurface.left() + position * feedbackSurface.width();
      const qreal radius = std::max(24.0, feedbackSurface.width() * 0.22);
      QLinearGradient sweep(center - radius, 0.0, center + radius, 0.0);
      QColor edge(QString::fromLatin1(item->draft ? UiStyle::orangeBorderStrong
                                                  : UiStyle::blueBorderStrong));
      QColor middle(QString::fromLatin1(
          item->draft ? UiStyle::orange : UiStyle::blueBorderStrong));
      edge.setAlpha(0);
      middle.setAlpha(105);
      sweep.setColorAt(0.0, edge);
      sweep.setColorAt(0.5, middle);
      sweep.setColorAt(1.0, edge);
      QPainterPath clip;
      clip.addRoundedRect(feedbackSurface, 8.0, 8.0);
      painter->save();
      painter->setClipPath(clip);
      painter->fillRect(feedbackSurface, sweep);
      painter->restore();
    }

    const QRect itemRect = tree_->visualRect(index);
    const int contentLeft = itemRect.left() + ContentOffset;
    const QPointF dotCenter(contentLeft + StatusCenterOffset,
                            QRectF(row).center().y());
    painter->setPen(Qt::NoPen);
    painter->setBrush(statusColor(item->status, item->pending, item->draft));
    painter->drawEllipse(dotCenter, StatusRadius, StatusRadius);

    QFont titleFont = option.font;
    titleFont.setWeight(QFont::Medium);
    painter->setFont(titleFont);
    painter->setPen(QColor(QString::fromLatin1(UiStyle::primary)));
    const QRect titleRect(contentLeft + 24, row.top(),
                          row.right() - contentLeft - 31, row.height());
    const QString title = QFontMetrics(titleFont).elidedText(
        displayTitle(*item), Qt::ElideRight, titleRect.width());
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);
    if (item->childCount() != 0) {
      const QRect indicator(itemRect.left() - ThreadIndent +
                                DisclosureIndicatorOffset,
                            row.top(), ThreadIndent, row.height());
      UiStyle::drawChevron(*painter, indicator.adjusted(3, 3, -3, -3),
                           option.state.testFlag(QStyle::State_Enabled), false,
                           item->isExpanded()
                               ? UiStyle::ChevronDirection::Down
                               : UiStyle::ChevronDirection::Right);
    }
    if (effective.state.testFlag(QStyle::State_HasFocus) &&
        !effective.state.testFlag(QStyle::State_Selected)) {
      painter->setBrush(Qt::NoBrush);
      painter->setPen(
          QPen(QColor(QString::fromLatin1(UiStyle::blueBorder)), 1.0));
      painter->drawRoundedRect(QRectF(row).adjusted(0.5, 3.5, -0.5, -3.5), 8.0,
                               8.0);
    }
    painter->restore();
  }

private:
  ThreadTreeWidget *tree_ = nullptr;
};

#if QT_CONFIG(accessibility)

class ThreadItemAccessible final : public QAccessibleInterface,
                                   public QAccessibleActionInterface {
public:
  ThreadItemAccessible(ThreadTreeWidget *tree, ThreadTreeItem *item)
      : tree_(tree), item_(item) {}

  [[nodiscard]] ThreadTreeWidget *tree() const { return tree_; }
  [[nodiscard]] ThreadTreeItem *item() const {
    return isValid() ? item_ : nullptr;
  }

  bool isValid() const override {
    return tree_ && item_ && item_->owner == tree_;
  }
  QObject *object() const override { return nullptr; }
  QWindow *window() const override {
    return tree_ && tree_->window() ? tree_->window()->windowHandle() : nullptr;
  }
  QAccessible::Role role() const override { return QAccessible::TreeItem; }

  QAccessible::State state() const override {
    QAccessible::State result;
    const ThreadTreeItem *current = item();
    if (!current) {
      result.invalid = true;
      result.invisible = true;
      result.offscreen = true;
      return result;
    }
    result.disabled = !tree_->isEnabled();
    const bool visible = itemIsVisible(*tree_, current);
    result.focusable = result.selectable = !result.disabled && visible;
    result.selected = tree_->selectionModel()->isSelected(
        tree_->indexFromItem(current));
    result.focused =
        visible && tree_->hasFocus() && tree_->currentItem() == current;
    result.expandable = current->childCount() != 0;
    if (result.expandable) {
      result.expanded = current->isExpanded();
      result.collapsed = !result.expanded;
    }
    const QRect bounds = rect();
    result.invisible = !visible || bounds.isEmpty();
    result.offscreen =
        result.invisible ||
        !bounds.intersects(QRect(tree_->viewport()->mapToGlobal(QPoint{}),
                                 tree_->viewport()->size()));
    return result;
  }

  QRect rect() const override {
    const ThreadTreeItem *current = item();
    if (!itemIsVisible(*tree_, current))
      return {};
    const QRect local = tree_->rowRect(tree_->indexFromItem(current));
    return local.isEmpty()
               ? QRect{}
               : QRect(tree_->viewport()->mapToGlobal(local.topLeft()),
                       local.size());
  }

  QString text(QAccessible::Text kind) const override {
    const ThreadTreeItem *current = item();
    if (!current)
      return {};
    if (kind == QAccessible::Name)
      return displayTitle(*current);
    if (kind == QAccessible::Description || kind == QAccessible::Help)
      return itemDescription(*current);
    return {};
  }
  void setText(QAccessible::Text, const QString &) override {}

  QAccessibleInterface *parent() const override {
    ThreadTreeItem *current = item();
    if (!current)
      return nullptr;
    return current->parent()
               ? tree_->accessibleItem(tree_->threadItem(current->parent()))
               : QAccessible::queryAccessibleInterface(tree_);
  }
  int childCount() const override {
    const ThreadTreeItem *current = item();
    return current ? current->childCount() : 0;
  }
  QAccessibleInterface *child(int index) const override {
    ThreadTreeItem *current = item();
    return current && index >= 0 && index < current->childCount()
               ? tree_->accessibleItem(tree_->threadItem(current->child(index)))
               : nullptr;
  }
  int indexOfChild(const QAccessibleInterface *candidate) const override {
    const auto *accessible =
        dynamic_cast<const ThreadItemAccessible *>(candidate);
    ThreadTreeItem *current = item();
    return current && accessible && accessible->tree() == tree_
               ? current->indexOfChild(accessible->item())
               : -1;
  }
  QAccessibleInterface *childAt(int x, int y) const override {
    ThreadTreeItem *current = item();
    if (!current)
      return nullptr;
    ThreadTreeItem *hit = tree_->threadItem(
        tree_->itemAt(tree_->viewport()->mapFromGlobal(QPoint(x, y))));
    while (hit && hit->parent() != current)
      hit = hit->parent() ? tree_->threadItem(hit->parent()) : nullptr;
    return hit ? tree_->accessibleItem(hit) : nullptr;
  }

  void *interface_cast(QAccessible::InterfaceType type) override {
    return type == QAccessible::ActionInterface
               ? static_cast<QAccessibleActionInterface *>(this)
               : nullptr;
  }
  QStringList actionNames() const override {
    ThreadTreeItem *current = item();
    if (!itemIsVisible(*tree_, current) || !tree_->isEnabled())
      return {};
    QStringList result{QAccessibleActionInterface::pressAction(),
                       QAccessibleActionInterface::setFocusAction()};
    if (current->childCount() != 0)
      result.push_back(QAccessibleActionInterface::toggleAction());
    return result;
  }
  void doAction(const QString &action) override {
    ThreadTreeItem *current = item();
    if (!itemIsVisible(*tree_, current) || !tree_->isEnabled())
      return;
    if (action == QAccessibleActionInterface::toggleAction() &&
        current->childCount() != 0) {
      current->setExpanded(!current->isExpanded());
      return;
    }
    if (action == QAccessibleActionInterface::pressAction() ||
        action == QAccessibleActionInterface::setFocusAction()) {
      tree_->setCurrentItem(current);
      current->setSelected(true);
      tree_->setFocus(Qt::OtherFocusReason);
    }
  }
  QStringList keyBindingsForAction(const QString &action) const override {
    if (action == QAccessibleActionInterface::toggleAction())
      return {QStringLiteral("Left/Right")};
    if (action == QAccessibleActionInterface::pressAction())
      return {QStringLiteral("Space")};
    return {};
  }

private:
  QPointer<ThreadTreeWidget> tree_;
  ThreadTreeItem *item_ = nullptr;
};

class ThreadTreeAccessible final : public QAccessibleWidget,
                                   public QAccessibleSelectionInterface {
public:
  explicit ThreadTreeAccessible(ThreadTreeWidget *tree)
      : QAccessibleWidget(tree, QAccessible::Tree) {}

  void *interface_cast(QAccessible::InterfaceType type) override {
    return type == QAccessible::SelectionInterface
               ? static_cast<QAccessibleSelectionInterface *>(this)
               : QAccessibleWidget::interface_cast(type);
  }

  int childCount() const override {
    return tree() ? tree()->topLevelItemCount() : 0;
  }
  QAccessibleInterface *child(int index) const override {
    return tree() && index >= 0 && index < tree()->topLevelItemCount()
               ? tree()->accessibleItem(
                     tree()->threadItem(tree()->topLevelItem(index)))
               : nullptr;
  }
  int indexOfChild(const QAccessibleInterface *candidate) const override {
    const auto *accessible =
        dynamic_cast<const ThreadItemAccessible *>(candidate);
    return tree() && accessible && accessible->tree() == tree()
               ? tree()->indexOfTopLevelItem(accessible->item())
               : -1;
  }
  QAccessibleInterface *childAt(int x, int y) const override {
    if (!tree())
      return nullptr;
    ThreadTreeItem *item = tree()->threadItem(
        tree()->itemAt(tree()->viewport()->mapFromGlobal(QPoint(x, y))));
    while (item && item->parent())
      item = tree()->threadItem(item->parent());
    return item ? tree()->accessibleItem(item) : nullptr;
  }
  QAccessibleInterface *focusChild() const override {
    ThreadTreeWidget *view = tree();
    ThreadTreeItem *current =
        view ? view->threadItem(view->currentItem()) : nullptr;
    return view && view->hasFocus() && itemIsVisible(*view, current)
               ? view->accessibleItem(current)
               : nullptr;
  }
  int selectedItemCount() const override {
    return selectedItems().size();
  }
  QList<QAccessibleInterface *> selectedItems() const override {
    QList<QAccessibleInterface *> result;
    if (tree())
      for (QTreeWidgetItem *item : tree()->selectedItems())
        if (!item->parent())
          result.push_back(tree()->accessibleItem(tree()->threadItem(item)));
    return result;
  }
  bool select(QAccessibleInterface *candidate) override {
    return setSelected(candidate, true);
  }
  bool unselect(QAccessibleInterface *candidate) override {
    return setSelected(candidate, false);
  }
  bool selectAll() override { return false; }
  bool clear() override {
    if (!tree() || !tree()->isEnabled())
      return false;
    const QList<QAccessibleInterface *> selected = selectedItems();
    for (QAccessibleInterface *item : selected)
      static_cast<void>(setSelected(item, false));
    return selectedItems().isEmpty();
  }

private:
  [[nodiscard]] ThreadTreeWidget *tree() const {
    return dynamic_cast<ThreadTreeWidget *>(widget());
  }
  bool setSelected(QAccessibleInterface *candidate, bool selected) {
    auto *accessible = dynamic_cast<ThreadItemAccessible *>(candidate);
    if (!tree() || !tree()->isEnabled() || !accessible ||
        accessible->tree() != tree() || !accessible->item() ||
        accessible->item()->parent())
      return false;
    if (selected && !itemIsVisible(*tree(), accessible->item()))
      return false;
    if (selected)
      tree()->setCurrentItem(accessible->item());
    accessible->item()->setSelected(selected);
    return accessible->item()->isSelected() == selected;
  }
};

QAccessibleInterface *threadAccessibleFactory(const QString &,
                                              QObject *object) {
  auto *tree = dynamic_cast<ThreadTreeWidget *>(object);
  return tree ? new ThreadTreeAccessible(tree) : nullptr;
}

void sendThreadEvent(QAccessibleInterface *interface,
                     QAccessible::Event type) {
  if (!interface)
    return;
  QAccessibleEvent event(interface, type);
  QAccessible::updateAccessibility(&event);
}

QAccessibleInterface *cachedThreadInterface(ThreadTreeItem *item) {
  return item && item->accessibleId != 0
             ? QAccessible::accessibleInterface(item->accessibleId)
             : nullptr;
}

struct ThreadAccessibleState final {
  QAccessible::Id id = 0;
  QString name;
  QString description;
  std::string parentKey;
  bool expandable = false;
  bool expanded = false;
  bool collapsed = false;
};

std::optional<ThreadAccessibleState>
observeThreadAccessibility(ThreadTreeItem *item) {
  if (!cachedThreadInterface(item))
    return std::nullopt;
  const bool expandable = item->childCount() != 0;
  const bool expanded = expandable && item->isExpanded();
  const auto *parent =
      item->parent() ? static_cast<const ThreadTreeItem *>(item->parent())
                     : nullptr;
  return ThreadAccessibleState{
      item->accessibleId, displayTitle(*item), itemDescription(*item),
      parent ? parent->presentationKey : std::string{}, expandable, expanded,
      expandable && !expanded};
}

void notifyThreadAccessibility(ThreadTreeItem *item,
                               const ThreadAccessibleState &before) {
  if (!item || item->accessibleId != before.id)
    return;
  QAccessibleInterface *interface = cachedThreadInterface(item);
  if (!interface)
    return;
  const auto *parent =
      item->parent() ? static_cast<const ThreadTreeItem *>(item->parent())
                     : nullptr;
  const std::string parentKey =
      parent ? parent->presentationKey : std::string{};
  if (parentKey != before.parentKey)
    sendThreadEvent(interface, QAccessible::ParentChanged);
  if (displayTitle(*item) != before.name)
    sendThreadEvent(interface, QAccessible::NameChanged);
  if (itemDescription(*item) != before.description)
    sendThreadEvent(interface, QAccessible::DescriptionChanged);

  const bool expandable = item->childCount() != 0;
  const bool expanded = expandable && item->isExpanded();
  const bool collapsed = expandable && !expanded;
  QAccessible::State changed;
  changed.expandable = expandable != before.expandable;
  changed.expanded = expanded != before.expanded;
  changed.collapsed = collapsed != before.collapsed;
  if (changed.expandable || changed.expanded || changed.collapsed) {
    QAccessibleStateChangeEvent event(interface, changed);
    QAccessible::updateAccessibility(&event);
  }
}

void sendThreadExpansionEvent(QAccessibleInterface *interface) {
  if (!interface)
    return;
  QAccessible::State changed;
  changed.expanded = changed.collapsed = true;
  QAccessibleStateChangeEvent event(interface, changed);
  QAccessible::updateAccessibility(&event);
}

#endif

const ui::ThreadListRow *
findPresentation(const std::vector<ui::ThreadListRow> &roots,
                 std::string_view key) {
  for (const ui::ThreadListRow &row : roots) {
    if (row.presentationKey == key)
      return &row;
    if (const ui::ThreadListRow *found = findPresentation(row.children, key))
      return found;
  }
  return nullptr;
}

} // namespace

void ThreadTreeWidget::currentChanged(const QModelIndex &current,
                                      const QModelIndex &previous) {
  QAbstractItemView::currentChanged(current, previous);
  if (allColumnsShowFocus()) {
    if (previous.isValid())
      viewport()->update(rowRect(previous));
    if (current.isValid())
      viewport()->update(rowRect(current));
  }
#if QT_CONFIG(accessibility)
  if (!signalsBlocked() && current.isValid() && hasFocus())
    sendThreadEvent(accessibleItemForEvent(threadItem(current)),
                    QAccessible::Focus);
#endif
}

void ThreadTreeWidget::selectionChanged(const QItemSelection &selected,
                                        const QItemSelection &deselected) {
  QAbstractItemView::selectionChanged(selected, deselected);
#if QT_CONFIG(accessibility)
  if (signalsBlocked())
    return;
  for (const QModelIndex &index : selected.indexes())
    sendThreadEvent(accessibleItemForEvent(threadItem(index)),
                    QAccessible::SelectionAdd);
  for (const QModelIndex &index : deselected.indexes())
    sendThreadEvent(accessibleItemForEvent(threadItem(index)),
                    QAccessible::SelectionRemove);
#endif
}

QVariant ThreadTreeItem::data(int column, int role) const {
  if (column == 0 && role == Qt::UserRole)
    return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
  if (column == 0 && role == Qt::ToolTipRole)
    return itemDescription(*this);
  return QTreeWidgetItem::data(column, role);
}

#if QT_CONFIG(accessibility)
QAccessibleInterface *ThreadTreeWidget::accessibleItem(ThreadTreeItem *item) {
  if (!item || item->owner != this)
    return nullptr;
  if (item->accessibleId != 0) {
    if (QAccessibleInterface *existing =
            QAccessible::accessibleInterface(item->accessibleId))
      return existing;
    item->accessibleId = 0;
  }
  auto *created = new ThreadItemAccessible(this, item);
  item->accessibleId = QAccessible::registerAccessibleInterface(created);
  return created;
}

QAccessibleInterface *
ThreadTreeWidget::accessibleItemForEvent(ThreadTreeItem *item) {
  if (!item || (item->accessibleId == 0 && !QAccessible::isActive()))
    return nullptr;
  return accessibleItem(item);
}

void ThreadTreeWidget::retireAccessible(ThreadTreeItem *item) {
  if (!item || item->accessibleId == 0)
    return;
  const QAccessible::Id id = item->accessibleId;
  if (QAccessibleInterface *interface = QAccessible::accessibleInterface(id)) {
    QAccessibleEvent destroyed(interface, QAccessible::ObjectDestroyed);
    QAccessible::updateAccessibility(&destroyed);
    QAccessible::deleteAccessibleInterface(id);
  }
  item->accessibleId = 0;
}
#endif

ThreadPane::ThreadPane(QWidget *parent) : QFrame(parent) {
#if QT_CONFIG(accessibility)
  static const bool factoryInstalled =
      (QAccessible::installFactory(threadAccessibleFactory), true);
  static_cast<void>(factoryInstalled);
#endif
  setObjectName(QStringLiteral("sidebar"));
  setStyleSheet(QStringLiteral("QFrame#sidebar{background:%1;}")
                    .arg(QString::fromLatin1(UiStyle::sidebar)));
  setMaximumWidth(440);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 14, 10, 17);
  layout->setSpacing(0);

  auto *header = new QHBoxLayout;
  header->setContentsMargins(8, 0, 6, 0);
  header->addStrut(24);
  auto *sectionTitle =
      UiStyle::makeLabel(QStringLiteral("THREADS"), "panelHeader");
  sectionTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  header->addWidget(sectionTitle);
  header->addStretch();
  auto *hide = new QPushButton(QStringLiteral("Hide"));
  hide->setProperty("kind", "subtle");
  hide->setMinimumSize(52, 24);
  connect(hide, &QPushButton::clicked, this, [this] {
    if (actions.hide)
      actions.hide();
  });
  header->addWidget(hide);
  layout->addLayout(header);
  auto *headerDivider = new QFrame;
  headerDivider->setObjectName(QStringLiteral("threadHeaderDivider"));
  headerDivider->setProperty("kind", "standardDivider");
  headerDivider->setFixedHeight(1);
  layout->addWidget(headerDivider);
  layout->addSpacing(8);

  auto *create = new QPushButton(QStringLiteral("+  New thread"));
  create->setObjectName(QStringLiteral("threadNewButton"));
  create->setMinimumHeight(36);
  create->setStyleSheet(
      QStringLiteral(
          "QPushButton{background:%1;color:%2;border:1px solid %3;"
          "border-radius:8px;text-align:left;padding-left:14px;font-weight:600;"
          "}"
          "QPushButton:hover{background:%4;border-color:%5;}"
          "QPushButton:disabled{background:%6;color:%7;border-color:%8;}")
          .arg(QString::fromLatin1(UiStyle::panel),
               QString::fromLatin1(UiStyle::blue),
               QString::fromLatin1(UiStyle::blueBorder),
               QString::fromLatin1(UiStyle::blueSelected),
               QString::fromLatin1(UiStyle::blue),
               QString::fromLatin1(UiStyle::appBackground),
               QString::fromLatin1(UiStyle::placeholder),
               QString::fromLatin1(UiStyle::divider)));
  connect(create, &QPushButton::clicked, this, [this] {
    if (actions.newThread)
      actions.newThread();
  });
  layout->addWidget(create);
  layout->addSpacing(8);

  auto *toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(4, 0, 4, 6);
  toolbar->addStretch();
  sortButton = new UiStyle::ChevronToolButton;
  sortButton->setObjectName(QStringLiteral("threadSortButton"));
  sortButton->setProperty("kind", "subtle");
  sortButton->setProperty("codexChevron", true);
  sortButton->setPopupMode(QToolButton::InstantPopup);
  sortButton->setMinimumHeight(28);
  auto *sortMenu = new QMenu(sortButton);
  auto *sortGroup = new QActionGroup(sortMenu);
  sortGroup->setExclusive(true);
  const auto addSortAction = [this, sortMenu, sortGroup](QString label,
                                                         SortCriterion value) {
    QAction *action = sortMenu->addAction(std::move(label));
    action->setCheckable(true);
    sortGroup->addAction(action);
    connect(action, &QAction::triggered, this,
            [this, value] { setSortCriterion(value); });
    return action;
  };
  sortActions = {
      {{SortCriterion::Alphanumeric,
        addSortAction(QStringLiteral("Alphanumeric"),
                      SortCriterion::Alphanumeric)},
       {SortCriterion::Created,
        addSortAction(QStringLiteral("Created"), SortCriterion::Created)},
       {SortCriterion::Recency,
        addSortAction(QStringLiteral("Recent"), SortCriterion::Recency)}}};
  sortButton->setMenu(sortMenu);
  sortButton->setToolTip(QStringLiteral("Sort threads"));
  updateSortButton();
  toolbar->addWidget(sortButton);
  layout->addLayout(toolbar);

  tree = new ThreadTreeWidget(this);
  tree->setObjectName(QStringLiteral("threadList"));
  tree->setAccessibleName(QStringLiteral("Threads"));
  tree->setHeaderHidden(true);
  tree->setColumnCount(1);
  tree->setRootIsDecorated(true);
  tree->setItemsExpandable(true);
  tree->setExpandsOnDoubleClick(true);
  tree->setIndentation(ThreadIndent);
  tree->setUniformRowHeights(true);
  tree->setAnimated(false);
  tree->setSelectionMode(QAbstractItemView::SingleSelection);
  tree->setSelectionBehavior(QAbstractItemView::SelectRows);
  tree->setMouseTracking(true);
  tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  tree->setContextMenuPolicy(Qt::CustomContextMenu);
  tree->setItemDelegate(new ThreadItemDelegate(tree));
  optimisticAnimation = new QTimer(tree);
  optimisticAnimation->setObjectName(
      QStringLiteral("optimisticThreadAnimation"));
  connect(optimisticAnimation, &QTimer::timeout, this,
          [this] { updateAnimationTimer(true); });
  auto *treeStyle = new ThreadTreeStyle;
  treeStyle->setParent(tree);
  tree->setStyle(treeStyle);
  tree->setStyleSheet(
      QStringLiteral(
          "QTreeWidget#threadList{background:transparent;border:0;outline:0;"
          "show-decoration-selected:0;}"
          "QTreeWidget#threadList::item{min-height:30px;background:%1;"
          "border:1px solid %2;border-radius:8px;margin:3px 0;"
          "padding:2px 8px;color:%3;}"
          "QTreeWidget#threadList::item:hover{background:%4;"
          "border-color:%5;}"
          "QTreeWidget#threadList::item:selected{background:%6;"
          "border-color:%7;color:%3;font-weight:600;}")
          .arg(QString::fromLatin1(UiStyle::panel),
               QString::fromLatin1(UiStyle::divider),
               QString::fromLatin1(UiStyle::primary),
               QString::fromLatin1(UiStyle::hover),
               QString::fromLatin1(UiStyle::dividerStrong),
               QString::fromLatin1(UiStyle::blueSelected),
               QString::fromLatin1(UiStyle::blueBorder)));

  connect(tree, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
            const auto *selected = tree->threadItem(current);
            if (!selected || !selected->target ||
                std::exchange(selectionDispatchPending, true))
              return;
            // Selection actions may retire rows. Let Qt finish changing its
            // current index before observers mutate the model.
            QMetaObject::invokeMethod(
                this,
                [this] {
                  selectionDispatchPending = false;
                  const auto visible = visiblySelectedThread();
                  if (actions.select && visible && visible->target)
                    actions.select(visible->target);
                },
                Qt::QueuedConnection);
          });
  connect(tree, &QTreeWidget::customContextMenuRequested, this,
          [this](const QPoint &position) { showContextMenu(position); });
  connect(tree, &QTreeWidget::itemExpanded, this,
          [this](QTreeWidgetItem *item) {
            updateAnimationTimer();
#if QT_CONFIG(accessibility)
            sendThreadExpansionEvent(
                tree->accessibleItemForEvent(tree->threadItem(item)));
#endif
          });
  connect(tree, &QTreeWidget::itemCollapsed, this,
          [this](QTreeWidgetItem *item) {
            updateAnimationTimer();
#if QT_CONFIG(accessibility)
            sendThreadExpansionEvent(
                tree->accessibleItemForEvent(tree->threadItem(item)));
#endif
          });
  connect(tree->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
    if (tree->signalsBlocked())
      return;
    updateAnimationTimer();
    requestMoreNearListEnd();
  });
  layout->addWidget(tree);
}

ThreadPane::~ThreadPane() { delete tree; }

void ThreadPane::setActions(Actions next) { actions = std::move(next); }

bool ThreadPane::applyItemPresentation(
    ThreadTreeItem *item, const ui::ThreadListRow &row, bool draft,
    std::optional<std::int64_t> draftStartedAt, bool publishAccessibility) {
#if QT_CONFIG(accessibility)
  std::vector<std::pair<ThreadTreeItem *, ThreadAccessibleState>> observed;
  if (publishAccessibility) {
    if (auto state = observeThreadAccessibility(item))
      observed.emplace_back(item, std::move(*state));
    for (int index = 0; index < item->childCount(); ++index) {
      ThreadTreeItem *child = tree->threadItem(item->child(index));
      if (auto state = observeThreadAccessibility(child))
        observed.emplace_back(child, std::move(*state));
    }
  }
#endif
  bool changed = false;
  const auto assign = [&changed](auto &field, const auto &value) {
    if (field == value)
      return;
    field = value;
    changed = true;
  };
  assign(item->id, row.id);
  assign(item->target, row.target);
  assign(item->title, row.title);
  assign(item->cwd, row.cwd);
  assign(item->status, row.status);
  assign(item->createdAt, row.createdAt);
  assign(item->recencyAt, row.recencyAt);
  assign(item->lastActivityAt, row.lastActivityAt);
  assign(item->pending, row.pending);
  assign(item->archived, row.archived);
  assign(item->draft, draft);
  assign(item->awaitingPrompt, row.awaitingPromptConversation);
  assign(item->animationEpoch,
         static_cast<qint64>(draft
                                 ? draftStartedAt.value_or(0)
                                 : row.pendingPromptAdmittedAtMs.value_or(0)));
  if (!changed)
    return false;
  if (item->treeWidget() == tree)
    tree->viewport()->update(tree->rowRect(tree->indexFromItem(item)));
#if QT_CONFIG(accessibility)
  for (const auto &[observedItem, state] : observed)
    notifyThreadAccessibility(observedItem, state);
#endif
  return true;
}

void ThreadPane::beginOptimisticThread(std::string id,
                                       std::string presentationKey,
                                       std::string title, std::string cwd) {
  if (draftItem && draftItem->presentationKey != presentationKey)
    retireOptimisticThread();
  if (!draftItem) {
    auto *item = new ThreadTreeItem(tree, std::move(presentationKey));
    draftItem = item;
    rows.emplace(item->presentationKey, item);
    tree->insertTopLevelItem(0, item);
  }
  ui::ThreadListRow draft;
  draft.id = std::move(id);
  draft.title = std::move(title);
  draft.cwd = std::move(cwd);
  static_cast<void>(applyItemPresentation(draftItem, draft, true,
                                          QDateTime::currentMSecsSinceEpoch()));
  tree->setCurrentItem(draftItem);
  updateAnimationTimer();
}

void ThreadPane::discardOptimisticThread(const std::string &threadId) {
  if (!draftItem || draftItem->id != threadId)
    return;
  retireOptimisticThread();
  updateAnimationTimer();
}

void ThreadPane::retireOptimisticThread() {
  ThreadTreeItem *retired = std::exchange(draftItem, nullptr);
  rows.erase(retired->presentationKey);
  QSignalBlocker blocked(tree);
  tree->deleteItem(retired);
}

void ThreadPane::setSortCriterion(SortCriterion criterion) {
  if (sortCriterion == criterion)
    return;
  sortCriterion = criterion;
  updateSortButton();
  sortRootItems();
}

ThreadPane::SortCriterion ThreadPane::currentSortCriterion() const noexcept {
  return sortCriterion;
}

void ThreadPane::updateSortButton() {
  for (const auto &[criterion, action] : sortActions) {
    const bool selected = criterion == sortCriterion;
    action->setChecked(selected);
    if (selected)
      sortButton->setText(QStringLiteral("Sort: %1").arg(action->text()));
  }
}

void ThreadPane::sortRootItems() {
  const QModelIndex anchorIndex =
      tree->isVisible()
          ? tree->indexAt(QPoint(tree->viewport()->width() / 2, 0))
          : QModelIndex{};
  ThreadTreeItem *anchorItem = tree->threadItem(anchorIndex);
  const int anchorY =
      anchorIndex.isValid() ? tree->visualRect(anchorIndex).top() : 0;
  QSignalBlocker blocked(tree);
  tree->invisibleRootItem()->sortChildren(0, Qt::AscendingOrder);
  if (tree->isVisible()) {
    tree->doItemsLayout();
    tree->restoreViewportY(anchorItem, anchorY);
  }
  blocked.unblock();
  updateAnimationTimer();
  if (tree->isVisible())
    requestMoreNearListEnd();
}

bool ThreadPane::repositionRootItem(ThreadTreeItem *item) {
  if (!item || item->parent())
    return false;
  const int current = tree->indexOfTopLevelItem(item);
  if (current < 0)
    return false;
  auto *previous = current > 0
                       ? tree->threadItem(tree->topLevelItem(current - 1))
                       : nullptr;
  auto *next = current + 1 < tree->topLevelItemCount()
                   ? tree->threadItem(tree->topLevelItem(current + 1))
                   : nullptr;
  if ((!previous || !(*item < *previous)) &&
      (!next || !(*next < *item)))
    return false;

  const QModelIndex anchorIndex =
      tree->isVisible()
          ? tree->indexAt(QPoint(tree->viewport()->width() / 2, 0))
          : QModelIndex{};
  ThreadTreeItem *anchorItem = tree->threadItem(anchorIndex);
  const int anchorY =
      anchorIndex.isValid() ? tree->visualRect(anchorIndex).top() : 0;
  QSignalBlocker blocked(tree);
  const auto newestRecency =
      tree->threadItem(tree->topLevelItem(0))->recencyAt;
  tree->takeTopLevelItem(current);
  int position = 0;
  while (position < tree->topLevelItemCount()) {
    auto *candidate = tree->threadItem(tree->topLevelItem(position));
    if (candidate && *item < *candidate)
      break;
    ++position;
  }
  tree->insertTopLevelItem(position, item);
  if (tree->isVisible()) {
    tree->doItemsLayout();
    if (position == 0 && sortCriterion == SortCriterion::Recency &&
        item->recencyAt > newestRecency)
      tree->scrollToTop();
    else
      tree->restoreViewportY(anchorItem, anchorY);
  }
  blocked.unblock();
  updateAnimationTimer();
  if (tree->isVisible())
    requestMoreNearListEnd();
  return true;
}

void ThreadPane::refresh(const ui::ThreadListSnapshot &snapshot) {
  const QModelIndex anchorIndex =
      tree->isVisible()
          ? tree->indexAt(QPoint(tree->viewport()->width() / 2, 0))
          : QModelIndex{};
  const ThreadTreeItem *anchorItem = tree->threadItem(anchorIndex);
  const std::string anchorKey =
      anchorItem ? anchorItem->presentationKey : std::string{};
  const nodegraph::NodeRef anchorTarget =
      anchorItem ? anchorItem->target : nodegraph::NodeRef{};
  const int anchorY =
      anchorIndex.isValid() ? tree->visualRect(anchorIndex).top() : 0;
  bool structureChanged = false;
  bool revealNewest = false;
  if (contextMenu) {
    const ui::ThreadListRow *context =
        findPresentation(snapshot.roots, contextPresentationKey);
    if (!context || context->target != contextTarget ||
        context->archived != contextArchived ||
        providerReady != snapshot.providerReady ||
        canControl != snapshot.canControl)
      contextMenu->close();
  }
  providerReady = snapshot.providerReady;
  canControl = snapshot.canControl;

  const auto *currentBefore = tree->threadItem(tree->currentItem());
  const bool hadCurrent = currentBefore != nullptr;
  const std::string currentKey =
      currentBefore ? currentBefore->presentationKey : std::string{};
  const nodegraph::NodeRef currentTarget =
      currentBefore ? currentBefore->target : nodegraph::NodeRef{};
#if QT_CONFIG(accessibility)
  const QList<QTreeWidgetItem *> selectedBeforeItems = tree->selectedItems();
  const auto *selectedBefore =
      selectedBeforeItems.isEmpty()
          ? nullptr
          : tree->threadItem(selectedBeforeItems.constFirst());
  const std::string selectedKey =
      selectedBefore ? selectedBefore->presentationKey : std::string{};
  std::vector<std::pair<std::string, ThreadAccessibleState>>
      observedAccessibility;
  for (const auto &[key, item] : rows)
    if (auto state = observeThreadAccessibility(item))
      observedAccessibility.emplace_back(key, std::move(*state));
#endif
  std::unordered_set<std::string> wanted;
  wanted.reserve(rows.size() + snapshot.roots.size() + 1);
  ThreadTreeItem *selected = nullptr;
  bool selectedReparented = false;
  bool currentReparented = false;
  const bool initialPopulation = rows.empty();
  QList<QTreeWidgetItem *> initialRoots;
  if (initialPopulation)
    initialRoots.reserve(static_cast<qsizetype>(snapshot.roots.size()));

  const auto eraseSubtree = [this, &structureChanged](ThreadTreeItem *root) {
    structureChanged = true;
    std::vector<ThreadTreeItem *> pending{root};
    while (!pending.empty()) {
      ThreadTreeItem *item = pending.back();
      pending.pop_back();
      for (int index = 0; index < item->childCount(); ++index)
        pending.push_back(tree->threadItem(item->child(index)));
      rows.erase(item->presentationKey);
      if (draftItem == item)
        draftItem = nullptr;
    }
    tree->deleteItem(root);
  };

  const auto place = [this, &structureChanged](ThreadTreeItem *item,
                                               QTreeWidgetItem *parent,
                                               int position) {
    QTreeWidgetItem *positioned =
        parent ? parent->child(position) : tree->topLevelItem(position);
    if (positioned == item)
      return false;
    structureChanged = true;
    QTreeWidgetItem *currentParent = item->parent();
    const bool parentChanged = currentParent != parent;
    const int currentPosition =
        currentParent                ? currentParent->indexOfChild(item)
        : item->treeWidget() == tree ? tree->indexOfTopLevelItem(item)
                                     : -1;
    if (currentParent)
      currentParent->takeChild(currentPosition);
    else if (currentPosition >= 0)
      tree->takeTopLevelItem(currentPosition);
    if (parent)
      parent->insertChild(position, item);
    else
      tree->insertTopLevelItem(position, item);
    return parentChanged;
  };

  std::function<void(const ui::ThreadListRow &, QTreeWidgetItem *, int, bool)>
      reconcile;
  reconcile = [&](const ui::ThreadListRow &row, QTreeWidgetItem *parent,
                  int position, bool ancestorReparented) {
    if (row.presentationKey.empty() ||
        !wanted.insert(row.presentationKey).second)
      return;
    auto found = rows.find(row.presentationKey);
    ThreadTreeItem *item = found == rows.end() ? nullptr : found->second;
    if (!item) {
      item = new ThreadTreeItem(tree, row.presentationKey);
      rows.emplace(row.presentationKey, item);
    }
    if (!parent && position == 0 && sortCriterion == SortCriterion::Recency) {
      const auto *newest = tree->threadItem(tree->topLevelItem(0));
      revealNewest =
          newest && newest != item && row.recencyAt > newest->recencyAt;
    }
    bool reparented = false;
    if (initialPopulation && !parent)
      initialRoots.push_back(item);
    else
      reparented = place(item, parent, position);
    if (item->presentationKey == currentKey && item->target == currentTarget)
      currentReparented = reparented || ancestorReparented;
    if (item == draftItem) {
      draftItem = nullptr;
    }
    static_cast<void>(applyItemPresentation(item, row, false, {}, false));
    if (row.id == snapshot.selectedThreadId) {
      selected = item;
      selectedReparented = reparented || ancestorReparented;
    }
    for (int child = 0; child < static_cast<int>(row.children.size()); ++child)
      reconcile(row.children[static_cast<std::size_t>(child)], item, child,
                reparented || ancestorReparented);
  };

  std::vector<const ui::ThreadListRow *> roots;
  roots.reserve(snapshot.roots.size());
  for (const ui::ThreadListRow &row : snapshot.roots)
    roots.push_back(&row);
  std::ranges::stable_sort(roots, [this](const auto *left, const auto *right) {
    return tree->rowBefore(*left, *right);
  });

  QSignalBlocker blocked(tree);
  const bool retainDraft =
      draftItem &&
      !findPresentation(snapshot.roots, draftItem->presentationKey);
  int rootPosition = retainDraft ? 1 : 0;
  if (retainDraft) {
    wanted.insert(draftItem->presentationKey);
    static_cast<void>(place(draftItem, nullptr, 0));
  }
  for (const ui::ThreadListRow *row : roots)
    reconcile(*row, nullptr, rootPosition++, false);
  if (!initialRoots.isEmpty()) {
    tree->addTopLevelItems(initialRoots);
    structureChanged = true;
  }

  std::vector<ThreadTreeItem *> staleRoots;
  for (const auto &[key, item] : rows) {
    if (wanted.contains(key))
      continue;
    const auto *parent =
        item->parent() ? tree->threadItem(item->parent()) : nullptr;
    if (!parent || wanted.contains(parent->presentationKey))
      staleRoots.push_back(item);
  }
  for (ThreadTreeItem *item : staleRoots)
    eraseSubtree(item);

  if (!selected && !currentKey.empty()) {
    const auto retained = rows.find(currentKey);
    if (retained != rows.end() && retained->second->target == currentTarget) {
      selected = retained->second;
      selectedReparented = currentReparented;
    }
  }
  if (!selected && draftItem)
    selected = draftItem;
  if (selected) {
    if (!hadCurrent || selected->presentationKey != currentKey ||
        selectedReparented)
      for (QTreeWidgetItem *ancestor = selected->parent(); ancestor;
           ancestor = ancestor->parent())
        ancestor->setExpanded(true);
    if (tree->currentItem() != selected)
      tree->setCurrentItem(selected);
  } else if (tree->currentItem()) {
    tree->setCurrentItem(nullptr);
    tree->clearSelection();
  }
  const bool sameSelection =
      (!hadCurrent && !selected) ||
      (hadCurrent && selected && selected->presentationKey == currentKey &&
       selected->target == currentTarget && !selectedReparented);
  if (tree->isVisible() && structureChanged) {
    // QAbstractItemView defers scrollbar geometry; settle a structural refresh
    // before restoring its stable semantic anchor or requesting another page.
    tree->doItemsLayout();
    const auto retainedAnchor = rows.find(anchorKey);
    if (revealNewest)
      tree->scrollToTop();
    else if (sameSelection && retainedAnchor != rows.end() &&
             retainedAnchor->second->target == anchorTarget)
      tree->restoreViewportY(retainedAnchor->second, anchorY);
  }
  blocked.unblock();
#if QT_CONFIG(accessibility)
  const auto retainedByKey = [this](const std::string &key) {
    const auto found = rows.find(key);
    return found != rows.end() ? found->second : nullptr;
  };
  for (const auto &[key, state] : observedAccessibility) {
    const auto found = rows.find(key);
    if (found != rows.end())
      notifyThreadAccessibility(found->second, state);
  }
  const QList<QTreeWidgetItem *> selectedAfterItems = tree->selectedItems();
  ThreadTreeItem *const selectedAfter =
      selectedAfterItems.isEmpty()
          ? nullptr
          : tree->threadItem(selectedAfterItems.constFirst());
  ThreadTreeItem *const selectedBeforeRetained =
      retainedByKey(selectedKey);
  if (selectedAfter != selectedBeforeRetained) {
    sendThreadEvent(tree->accessibleItemForEvent(selectedAfter),
                    QAccessible::SelectionAdd);
    sendThreadEvent(tree->accessibleItemForEvent(selectedBeforeRetained),
                    QAccessible::SelectionRemove);
  }
  ThreadTreeItem *const currentAfter = tree->threadItem(tree->currentItem());
  ThreadTreeItem *const currentBeforeRetained =
      retainedByKey(currentKey);
  if (tree->hasFocus() && currentAfter &&
      currentAfter != currentBeforeRetained)
    sendThreadEvent(tree->accessibleItemForEvent(currentAfter),
                    QAccessible::Focus);
#endif
  updateAnimationTimer();
  if (tree->isVisible() && (structureChanged || !sameSelection))
    requestMoreNearListEnd();
}

bool ThreadPane::applyRowPresentation(const ui::ThreadListRow &row) {
  const auto found = rows.find(row.presentationKey);
  if (found == rows.end() || !row.target)
    return false;
  auto *item = found->second;
  if (item->draft || !item->target || item->target != row.target)
    return false;
  const std::string priorTitle = item->title;
  const auto priorCreated = item->createdAt;
  const auto priorRecency = item->recencyAt;
  const bool changed = applyItemPresentation(item, row);
  if (!changed)
    return true;
  const bool orderingChanged =
      !item->parent() &&
      (sortCriterion == SortCriterion::Alphanumeric ? priorTitle != item->title
       : sortCriterion == SortCriterion::Created
           ? priorCreated != item->createdAt
           : priorRecency != item->recencyAt);
  const bool repositioned = orderingChanged && repositionRootItem(item);
  if (contextMenu && item->presentationKey == contextPresentationKey &&
      item->archived != contextArchived)
    contextMenu->close();
  if (!repositioned)
    updateAnimationTimer();
  return true;
}

void ThreadPane::updateAnimationTimer(bool repaint) {
  if (!tree->isVisible() || !UiStyle::animationsEnabled(*tree)) {
    optimisticAnimation->stop();
    return;
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  qint64 earliest = std::numeric_limits<qint64>::max();
  bool active = false;
  QModelIndex index = tree->indexAt(QPoint(1, 1));
  if (!index.isValid())
    index = tree->indexAt(QPoint(tree->viewport()->width() / 2, 1));
  while (index.isValid()) {
    const QRect rect = tree->visualRect(index);
    if (rect.top() >= tree->viewport()->height())
      break;
    const ThreadTreeItem *item = tree->threadItem(index);
    if (itemCanAnimate(*item, now)) {
      active = true;
      if (repaint)
        tree->viewport()->update(tree->rowRect(index));
    } else if (item->awaitingPrompt && item->animationEpoch > 0) {
      const qint64 deadline =
          item->animationEpoch + PendingAnimationDelayMilliseconds;
      earliest = std::min(earliest, deadline - now);
    }
    index = tree->indexBelow(index);
  }
  if (active) {
    if (!optimisticAnimation->isActive() ||
        optimisticAnimation->isSingleShot() ||
        optimisticAnimation->interval() !=
            PendingAnimationIntervalMilliseconds) {
      optimisticAnimation->setSingleShot(false);
      optimisticAnimation->start(PendingAnimationIntervalMilliseconds);
    }
  } else if (earliest != std::numeric_limits<qint64>::max()) {
    const int delay = static_cast<int>(
        std::clamp<qint64>(earliest, 1, std::numeric_limits<int>::max()));
    if (!optimisticAnimation->isActive() ||
        !optimisticAnimation->isSingleShot() ||
        optimisticAnimation->remainingTime() > delay + 1) {
      optimisticAnimation->setSingleShot(true);
      optimisticAnimation->start(delay);
    }
  } else {
    optimisticAnimation->stop();
  }
}

void ThreadPane::requestMoreNearListEnd() {
  if (!actions.loadMore || tree->topLevelItemCount() == 0)
    return;
  const QScrollBar *scroll = tree->verticalScrollBar();
  const int threshold = std::max(48, scroll->pageStep() / 2);
  if (scroll->maximum() == 0 ||
      scroll->value() >= scroll->maximum() - threshold)
    actions.loadMore();
}

std::optional<ThreadPane::VisibleThread>
ThreadPane::visiblySelectedThread() const {
  const QList<QTreeWidgetItem *> selected = tree->selectedItems();
  if (selected.size() != 1 || !selected.front())
    return std::nullopt;
  const ThreadTreeItem *item = tree->threadItem(selected.front());
  return VisibleThread{item->id, item->presentationKey, item->target};
}

bool ThreadPane::retainsTarget(
    const nodegraph::NodeRef &target) const noexcept {
  if (!target)
    return false;
  if (contextTarget == target)
    return true;
  return std::ranges::any_of(rows, [&target](const auto &entry) {
    return entry.second && entry.second->target == target;
  });
}

void ThreadPane::updateContextRow(const std::string &presentationKey) {
  const auto found = rows.find(presentationKey);
  if (found == rows.end())
    return;
  tree->viewport()->update(tree->rowRect(tree->indexFromItem(found->second)));
}

void ThreadPane::showContextMenu(const QPoint &position) {
  auto *item = tree->threadItem(tree->itemAt(position));
  if (!item || !item->target)
    return;
  if (contextMenu)
    contextMenu->close();
  contextPresentationKey = item->presentationKey;
  contextTarget = item->target;
  contextArchived = item->archived;
  auto *menu = new QMenu(tree);
  contextMenu = menu;
  updateContextRow(contextPresentationKey);
  connect(menu, &QMenu::aboutToHide, this, [this, menu] {
    if (contextMenu == menu) {
      const std::string key = std::exchange(contextPresentationKey, {});
      contextTarget.reset();
      contextArchived = false;
      contextMenu = nullptr;
      updateContextRow(key);
    }
    menu->deleteLater();
  });
  const nodegraph::NodeRef target = item->target;
  const auto addAction = [this, menu, target](QString label,
                                              ThreadAction Actions::*member) {
    return menu->addAction(std::move(label), this, [this, target, member] {
      const ThreadAction &action = actions.*member;
      if (action)
        action(target);
    });
  };
  QAction *reload = addAction(QStringLiteral("Reload"), &Actions::reload);
  QAction *rename = addAction(QStringLiteral("Rename"), &Actions::rename);
  QAction *fork = addAction(QStringLiteral("Quick fork"), &Actions::fork);
  QAction *forkWithOptions = addAction(QStringLiteral("Fork with options…"),
                                       &Actions::forkWithOptions);
  QAction *archive = addAction(item->archived ? QStringLiteral("Unarchive")
                                              : QStringLiteral("Archive"),
                               &Actions::toggleArchive);
  menu->addSeparator();
  QAction *remove = addAction(QStringLiteral("Delete"), &Actions::remove);
  reload->setEnabled(providerReady);
  for (QAction *action : {rename, fork, forkWithOptions, archive, remove})
    action->setEnabled(canControl);
  menu->popup(tree->viewport()->mapToGlobal(position));
}

} // namespace codexui::codex::middle
