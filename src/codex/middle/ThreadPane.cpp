// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"
#include "codex/ui/TimingPresentation.h"

#include "codex/ThreadBrowser.h"
#include "codex/UiStatus.h"
#include "codex/middle/MiddleTypes.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QActionGroup>
#include <QCollator>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMargins>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QProxyStyle>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
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
constexpr int ProjectRowVerticalPadding = 4;
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
constexpr int PendingAnimationIntervalMilliseconds = 32;
constexpr qint64 PendingHalfCycleMilliseconds = 850;

class ThreadTreeStyle final : public QProxyStyle {
public:
  QRect subElementRect(SubElement element, const QStyleOption *option,
                       const QWidget *widget) const override;
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

QColor statusColor(const UiStatus& status, std::size_t pending, bool draft, bool indirectActive) {
    const std::string_view tone = statusTone(status);
    const char* role = UiStyle::threadInactive;
    if (draft || pending != 0) {
        role = UiStyle::statusWarning;
    } else if (indirectActive) {
        role = UiStyle::statusIndirect;
    } else if (tone == "active") {
        role = UiStyle::statusActive;
    } else if (tone == "success") {
        role = UiStyle::statusSuccess;
    } else if (tone == "warning") {
        role = UiStyle::statusWarning;
    } else if (tone == "danger") {
        role = UiStyle::statusDanger;
    }
    return QColor(QString::fromLatin1(role));
}

bool contextMouseEvent(const QEvent* event) {
    return event && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) &&
           static_cast<const QMouseEvent*>(event)->button() == Qt::RightButton;
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
  std::string workspaceChangeUnavailable;
  UiStatus status;
  bool indirectActive = false;
  std::optional<std::int64_t> createdAt;
  ProtocolTimes timing;
  std::optional<std::int64_t> recencyAt;
  std::optional<std::int64_t> lastActivityAt;
  std::size_t pending = 0;
  bool archived = false;
  bool draft = false;
  bool awaitingPrompt = false;
  qint64 animationEpoch = 0;
  ui::ThreadRowKind kind = ui::ThreadRowKind::Thread;
  std::string projectId;
  std::string sectionId;
  nodegraph::NodeRef parentThread;
  std::string details;
  std::int64_t order = 0;
  nlohmann::json query;
  bool pendingPage = false;
  int projectScroll = 0;
#if QT_CONFIG(accessibility)
  QAccessible::Id accessibleId = 0;
#endif
};

// Each row is displayed once. Project bodies project the outer tree's model,
// selection and expansion; only their native viewport geometry is independent.
class ProjectThreadView final : public QTreeView {
public:
    using QTreeView::QTreeView;

protected:
    bool viewportEvent(QEvent* event) override {
        return contextMouseEvent(event) || QTreeView::viewportEvent(event);
    }
    void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {
    }
    void keyPressEvent(QKeyEvent* event) override {
        auto* outer = static_cast<QTreeWidget*>(parentWidget()->parentWidget());
        QModelIndex destination;
        if ((event->key() == Qt::Key_Up && currentIndex() == model()->index(0, 0, rootIndex())) ||
            (event->key() == Qt::Key_Left && currentIndex().parent() == rootIndex() && !isExpanded(currentIndex()))) {
            destination = rootIndex();
        } else if (event->key() == Qt::Key_Down && !indexBelow(currentIndex()).isValid()) {
            destination = outer->indexBelow(rootIndex());
        }
        if (destination.isValid()) {
            outer->setCurrentIndex(destination);
            outer->setFocus();
            event->accept();
        } else {
            QTreeView::keyPressEvent(event);
        }
    }
};

class ThreadTreeWidget final : public QTreeWidget {
public:
    explicit ThreadTreeWidget(ThreadPane* owner)
        : QTreeWidget(owner)
        , owner_(owner)
        , collator_(effectiveLocale()) {
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        connect(this, &QTreeView::expanded, this, &ThreadTreeWidget::scheduleDelayedItemsLayout);
        connect(this, &QTreeView::collapsed, this, &ThreadTreeWidget::scheduleDelayedItemsLayout);
        const auto expansion = [this](const QModelIndex& index) {
            for (auto* body : projectViews_) {
                const QSignalBlocker blocked(body);
                body->setExpanded(index, isExpanded(index));
            }
        };
        connect(this, &QTreeView::expanded, this, expansion);
        connect(this, &QTreeView::collapsed, this, expansion);
        collator_.setCaseSensitivity(Qt::CaseInsensitive);
        collator_.setIgnorePunctuation(true);
        collator_.setNumericMode(true);
    }

    ~ThreadTreeWidget() override {
        blockSignals(true);
        verticalScrollBar()->blockSignals(true);
        qDeleteAll(projectViews_);
        projectViews_.clear();
        clear();
    }

    [[nodiscard]] ThreadTreeItem* threadItem(QTreeWidgetItem* item) const {
        return static_cast<ThreadTreeItem*>(item);
    }
    [[nodiscard]] ThreadTreeItem* threadItem(const QModelIndex& index) const {
        return index.isValid() ? threadItem(itemFromIndex(index)) : nullptr;
    }
    [[nodiscard]] QTreeView* rowView(const QModelIndex& index) const {
        QTreeView* result = const_cast<ThreadTreeWidget*>(this);
        QModelIndex root = index;
        while (root.parent().isValid()) {
            root = root.parent();
        }
        if (root != index) {
            for (auto* body : projectViews_) {
                if (body->rootIndex() == root) {
                    result = body;
                }
            }
        }
        return result;
    }
    [[nodiscard]] QRect visualRect(const QModelIndex& index) const override {
        auto* view = rowView(index);
        QRect result = view == this ? QTreeWidget::visualRect(index) : view->visualRect(index);
        if (view != this && result.isValid()) {
            result.moveTopLeft(view->viewport()->mapTo(viewport(), result.topLeft()));
        }
        return result;
    }
    [[nodiscard]] QModelIndex indexAt(const QPoint& point) const override {
        QModelIndex result = QTreeWidget::indexAt(point);
        for (auto* body : projectViews_) {
            if (body->geometry().contains(point)) {
                result = body->indexAt(body->viewport()->mapFrom(viewport(), point));
            }
        }
        return result;
    }
    [[nodiscard]] QRect rowRect(const QModelIndex& index) const {
        QRect result = visualRect(index);
        auto* view = rowView(index);
        if (result.isValid()) {
            result.setLeft(view->viewport()->mapTo(viewport(), QPoint{}).x());
            result.setWidth(view->viewport()->width());
        }
        return result;
    }
    [[nodiscard]] int compactRowHeight() const {
        return std::max(DisclosureHitExtent, fontMetrics().height() + 2 * ProjectRowVerticalPadding);
    }
    [[nodiscard]] int projectBodyHeight(const QModelIndex& project) const {
        int count = 0;
        const std::function<void(const QModelIndex&)> countRows = [&](const QModelIndex& parent) {
            for (int row = 0; row < model()->rowCount(parent) && count < UiStyle::projectVisibleRows; ++row) {
                const QModelIndex index = model()->index(row, 0, parent);
                ++count;
                if (isExpanded(index)) {
                    countRows(index);
                }
            }
        };
        countRows(project);
        return count * compactRowHeight();
    }
    [[nodiscard]] QMargins projectCardInsets(const QModelIndex& index) const {
        const auto* item = threadItem(index);
        const bool project =
            item && !item->parent() && item->kind == ui::ThreadRowKind::Project && item->isExpanded() && item->childCount();
        return {0, project ? UiStyle::projectCardEdgePadding : 0, 0, 0};
    }
    void updateRow(const QModelIndex& index) {
        auto* view = rowView(index);
        QRect rect = view == this ? QTreeWidget::visualRect(index) : view->visualRect(index);
        rect.setLeft(0);
        rect.setWidth(view->viewport()->width());
        view->viewport()->update(rect);
    }
    void updateViewports() {
        viewport()->update();
        for (auto* body : projectViews_) {
            body->viewport()->update();
        }
    }
    template <typename Function>
    void forEachVisibleRow(Function function) const {
        QList<QTreeView*> views{const_cast<ThreadTreeWidget*>(this)};
        for (auto* body : projectViews_) {
            views.append(body);
        }
        for (auto* view : views) {
            QModelIndex index = view == this ? QTreeWidget::indexAt(QPoint(viewport()->width() / 2, 0))
                                             : view->indexAt(QPoint(view->viewport()->width() / 2, 0));
            while (index.isValid() && view->visualRect(index).top() < view->viewport()->height()) {
                if (rowRect(index).intersects(viewport()->rect())) {
                    function(index);
                }
                index = view->indexBelow(index);
            }
        }
    }
    void doItemsLayout() override {
        for (int row = 0; row < topLevelItemCount(); ++row) {
            auto* item = threadItem(topLevelItem(row));
            setRowHidden(row, {}, false);
            const std::function<void(QTreeWidgetItem*)> hideChildren = [&](QTreeWidgetItem* parent) {
                for (int child = 0; child < parent->childCount(); ++child) {
                    const bool hidden = parent == item && item->kind == ui::ThreadRowKind::Project && item->isExpanded();
                    const auto parentIndex = indexFromItem(parent);
                    if (isRowHidden(child, parentIndex) != hidden) {
                        setRowHidden(child, parentIndex, hidden);
                    }
                    hideChildren(parent->child(child));
                }
            };
            hideChildren(item);
        }
        QTreeWidget::doItemsLayout();
        positionProjectViews();
    }
    void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override {
        QModelIndex root = index;
        while (root.parent().isValid()) {
            root = root.parent();
        }
        const auto* item = threadItem(root);
        QTreeWidget::scrollTo(item && item->kind == ui::ThreadRowKind::Project ? root : index, hint);
        positionProjectViews();
        if (auto* view = rowView(index); view != this) {
            view->scrollTo(index, hint);
        }
    }
    void positionProjectViews();
    void setActiveThread(ThreadTreeItem* item) {
        if (std::exchange(activeThreadIndex_, item ? indexFromItem(item) : QModelIndex{}) != activeThreadIndex_) {
            updateViewports();
        }
    }
  [[nodiscard]] QModelIndex activeCardIndex() const {
    QModelIndex index = activeThreadIndex_;
    while (index.parent().isValid())
      index = index.parent();
    const auto *root = threadItem(index);
    return root && (root->kind == ui::ThreadRowKind::Project ||
                    root->kind == ui::ThreadRowKind::Section)
               ? index : QModelIndex(activeThreadIndex_);
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
  void activateItem(ThreadTreeItem *item) { owner_->activateBrowserRow(item); }
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
    if (left.kind != right.kind)
      return left.kind < right.kind;
    if (left.kind == ui::ThreadRowKind::Section)
      return alphaBefore(left.title, left.id, right.title, right.id);
    if ((left.kind == ui::ThreadRowKind::Project &&
         owner_->browserOptions().manualProjects) ||
        (left.kind == ui::ThreadRowKind::Thread && !left.sectionId.empty() &&
         owner_->browserOptions().manualSections))
      return left.order != right.order ? left.order < right.order
                                       : left.id < right.id;
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
  bool viewportEvent(QEvent* event) override {
      bool handled = contextMouseEvent(event);
      if (!handled && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)) {
          auto* mouse = static_cast<QMouseEvent*>(event);
          const QModelIndex index = QTreeWidget::indexAt(mouse->position().toPoint());
          const auto* item = threadItem(index);
          QStyleOptionViewItem option;
          initViewItemOption(&option);
          option.rect = QTreeWidget::visualRect(index);
          option.rect.setLeft(option.rect.left() - indentation());
          option.rect.setWidth(indentation());
          const bool disclosure =
              style()->subElementRect(QStyle::SE_TreeViewDisclosureItem, &option, this).contains(mouse->position().toPoint());
          if (item && item->kind == ui::ThreadRowKind::Project && item->childCount() && mouse->button() == Qt::LeftButton &&
              (event->type() == QEvent::MouseButtonPress ? disclosure : !disclosure)) {
              // Qt omits disclosure hit testing when this view's children are hidden.
              setExpanded(index, !isExpanded(index));
              event->accept();
              handled = true;
          }
      }
      return handled || QTreeWidget::viewportEvent(event);
  }

  void keyPressEvent(QKeyEvent* event) override {
      const QModelIndex current = currentIndex();
      const auto* item = threadItem(current);
      if ((event->key() == Qt::Key_Down || event->key() == Qt::Key_Right) && item && item->kind == ui::ThreadRowKind::Project &&
          isExpanded(current) && item->childCount()) {
          const QModelIndex child = model()->index(0, 0, current);
          setCurrentIndex(child);
          scrollTo(child);
          rowView(child)->setFocus();
          event->accept();
      } else {
          QTreeWidget::keyPressEvent(event);
      }
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
    doItemsLayout();
    owner_->updateAnimationTimer();
    owner_->requestMoreNearListEnd();
    if (owner_->providerReady && owner_->actions.refreshGroups)
      owner_->actions.refreshGroups();
  }

  void scrollContentsBy(int dx, int dy) override {
      QTreeWidget::scrollContentsBy(dx, dy);
      positionProjectViews();
  }

  void resizeEvent(QResizeEvent *event) override {
    QTreeWidget::resizeEvent(event);
    positionProjectViews();
    owner_->updateAnimationTimer();
    if (event->oldSize().isValid() && isVisible()) {
        owner_->requestMoreNearListEnd();
    }
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

  // A lifetime-safe projection of the active conversation, not keyboard focus.
  QList<ProjectThreadView*> projectViews_;
  QPersistentModelIndex activeThreadIndex_;
  ThreadPane *owner_ = nullptr;
  QCollator collator_;
};

namespace {

QRect ThreadTreeStyle::subElementRect(SubElement element,
                                      const QStyleOption *option,
                                      const QWidget *widget) const {
    QRect native = QProxyStyle::subElementRect(element, option, widget);
    if (element != SE_TreeViewDisclosureItem || !option)
      return native;
    if (const auto *tree = dynamic_cast<const ThreadTreeWidget *>(widget)) {
      const QModelIndex index = tree->QTreeWidget::indexAt(
          QPoint(tree->viewport()->width() / 2, option->rect.center().y()));
      native = native.marginsRemoved(tree->projectCardInsets(index));
      const auto* item = tree->threadItem(index);
      if (item && item->kind == ui::ThreadRowKind::Project && item->isExpanded() && item->childCount()) {
          native.setHeight(tree->compactRowHeight());
      }
    }
    const int verticalInset =
        std::max(0, (native.height() - DisclosureHitExtent) / 2);
    // Keep the disclosure target centered in the actual row.
    return option->direction == Qt::RightToLeft
               ? native.adjusted(-DisclosureHitTrailingExtension,
                                 verticalInset,
                                 -DisclosureHitLeadingInset,
                                 -verticalInset)
               : native.adjusted(DisclosureHitLeadingInset,
                                 verticalInset,
                                 DisclosureHitTrailingExtension,
                                 -verticalInset);
  }

} // namespace

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
  return kind == right.kind && draft != right.draft
             ? draft
             : owner->rowBefore(*this, right);
}

namespace {

    QString displayTitle(const ThreadTreeItem& item) {
        QString result = text(item.title);
        if (result.isEmpty())
            result = text(item.id.substr(0, 12));
        if (item.pending != 0)
            result.prepend(QStringLiteral("! "));
        return item.kind == ui::ThreadRowKind::Section && !item.parent() ? QStringLiteral("Section · ") + result : result;
    }

int itemLevel(const ThreadTreeItem *item) {
  int level = 1;
  for (const QTreeWidgetItem *parent = item ? item->parent() : nullptr; parent;
       parent = parent->parent())
    ++level;
  return level;
}

QString itemDescription(const ThreadTreeItem& item) {
    QStringList details{displayTitle(item)};
    if (item.kind != ui::ThreadRowKind::Thread) {
        if (item.kind != ui::ThreadRowKind::Page) {
            details.push_back(QStringLiteral("Threads: %1")
                                  .arg(isActiveStatus(item.status)                                ? QStringLiteral("active")
                                       : item.status.semantic == nodegraph::NodeStatus::Completed ? QStringLiteral("loaded")
                                                                                                  : QStringLiteral("not loaded")));
        }
        details.push_back(text(item.details));
    } else {
        details << QStringLiteral("Status: %1").arg(text(displayStatus(item.status)))
                << QStringLiteral("Workspace: %1").arg(item.cwd.empty() ? QStringLiteral("Unknown") : text(item.cwd))
                << QStringLiteral("Recent turn: %1").arg(activityText(item.recencyAt))
                << QStringLiteral("Created: %1").arg(activityText(item.createdAt))
                << QStringLiteral("Last activity: %1").arg(activityText(item.lastActivityAt))
                << QStringLiteral("Level %1").arg(itemLevel(&item));
        if (item.pending != 0) {
            details.push_back(QStringLiteral("%1 pending request(s)").arg(item.pending));
        }
        if (item.draft) {
            details.push_back(QStringLiteral("Draft thread"));
        }
        if (item.awaitingPrompt) {
            details.push_back(QStringLiteral("Waiting for prompt to enter conversation"));
        }
        if (const auto* parent = item.parent() ? static_cast<const ThreadTreeItem*>(item.parent()) : nullptr) {
            details.push_back(QStringLiteral("Parent: %1").arg(displayTitle(*parent)));
        }
        details << QStringLiteral("Server timestamps:\n") + ui::timingDetails(item.timing);
    }
    if (item.indirectActive) {
        details.push_back(QStringLiteral("Indirect activity: running descendant"));
    }
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

  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
      const auto* item = tree_->threadItem(index);
      int height = ThreadRowHeight;
      if (item) {
          const auto* outer = item;
          while (outer->parent()) {
              outer = tree_->threadItem(outer->parent());
          }
          if (outer->kind == ui::ThreadRowKind::Project && outer->isExpanded() && outer->childCount()) {
              height = tree_->compactRowHeight();
              if (item == outer) {
                  height += 2 * UiStyle::projectCardEdgePadding + tree_->projectBodyHeight(index);
              }
          }
      }
      return {0, height};
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    const ThreadTreeItem *item = tree_->threadItem(index);
    if (!item)
      return;
    const QRect row = tree_->rowRect(index);
    auto* view = tree_->rowView(index);
    painter->save();
    painter->translate(view->viewport()->mapFrom(tree_->viewport(), QPoint{}));
    QStyleOptionViewItem effective(option);
    effective.rect = row;
    if (tree_->isContextHighlighted(item))
      effective.state |= QStyle::State_MouseOver;
    const auto *outer = item;
    while (outer->parent())
      outer = tree_->threadItem(outer->parent());
    const bool grouped = outer->kind == ui::ThreadRowKind::Project ||
                         outer->kind == ui::ThreadRowKind::Section;
    const bool activeCard = tree_->activeCardIndex() ==
                            (grouped ? tree_->indexFromItem(outer) : index);
    const bool selected = effective.state.testFlag(QStyle::State_Selected);
    const bool hovered = effective.state.testFlag(QStyle::State_MouseOver);
    const QColor rowBackground(QString::fromLatin1(
        selected ? UiStyle::threadSelected : hovered ? UiStyle::hover : UiStyle::panel));
    QRect contentRow = row.marginsRemoved(tree_->projectCardInsets(index));
    if (item == outer && outer->kind == ui::ThreadRowKind::Project && outer->isExpanded() && outer->childCount()) {
        contentRow.setHeight(tree_->compactRowHeight());
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (!grouped && item->kind != ui::ThreadRowKind::Page) {
      painter->setPen(
          QColor(QString::fromLatin1(activeCard ? UiStyle::activeThreadBorder
                                     : selected ? UiStyle::threadSelectedBorder
                                     : hovered ? UiStyle::dividerStrong
                                               : UiStyle::divider)));
      painter->setBrush(rowBackground);
      painter->drawRoundedRect(QRectF(row).adjusted(0.5, 3.5, -0.5, -3.5), 8,
                               8);
    }
    if (!grouped && item->kind == ui::ThreadRowKind::Page)
      painter->fillRect(row, QColor(QString::fromLatin1(UiStyle::sidebar)));
    if (grouped) {
      const auto *last = outer;
      while (last->isExpanded() && last->childCount())
        last = tree_->threadItem(last->child(last->childCount() - 1));
      QRectF surface = tree_->rowRect(tree_->indexFromItem(outer));
      if (outer->kind != ui::ThreadRowKind::Project) {
          surface.setBottom(tree_->rowRect(tree_->indexFromItem(last)).bottom());
      }
      surface.adjust(0.5, 3.5, -0.5, -3.5);
      painter->setClipRect(row, Qt::IntersectClip);
      painter->setPen(QColor(QString::fromLatin1(
          activeCard ? UiStyle::activeGroupBorder : UiStyle::groupBorder)));
      painter->setBrush(QColor(QString::fromLatin1(
          activeCard ? UiStyle::activeGroupSurface : UiStyle::panel)));
      painter->drawRoundedRect(surface, 8.0, 8.0);
      QPainterPath clip;
      clip.addRoundedRect(surface.adjusted(1, 1, -1, -1), 7, 7);
      painter->setClipPath(clip, Qt::IntersectClip);
      if (activeCard && item == outer)
          painter->fillRect(contentRow.adjusted(2, 0, -2, 0), QColor(QString::fromLatin1(UiStyle::activeGroupHeaderSurface)));
      else if (selected || hovered)
        painter->fillRect(contentRow.adjusted(2, 0, -2, 0), rowBackground);
    }

    const bool feedback = item->draft || item->awaitingPrompt;
    const QRectF feedbackSurface = QRectF(row).adjusted(1.0, 4.0, -1.0, -4.0);
    if (feedback) {
      painter->setPen(QPen(
          QColor(QString::fromLatin1(item->draft ? UiStyle::draftThreadBorder
                                                 : UiStyle::pendingPromptBorderStrong)),
          item->draft ? 1.0 : 1.5));
      painter->setBrush(QColor(QString::fromLatin1(
          item->draft ? UiStyle::draftThreadSurface : UiStyle::pendingPromptSurface)));
      if (grouped)
        painter->fillRect(row.adjusted(2, 0, -2, 0), painter->brush());
      else
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
      QColor edge(QString::fromLatin1(item->draft ? UiStyle::draftThreadBorder
                                                  : UiStyle::pendingPromptBorderStrong));
      QColor middle(QString::fromLatin1(
          item->draft ? UiStyle::draftThreadSweep : UiStyle::pendingPromptBorderStrong));
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
                            QRectF(contentRow).center().y());
    painter->setPen(Qt::NoPen);
    painter->setBrush(statusColor(item->status, item->pending, item->draft, item->indirectActive));
    if (item->kind != ui::ThreadRowKind::Page) {
        painter->drawEllipse(dotCenter, StatusRadius, StatusRadius);
    }

    const QFont font = titleFont(option.font, *item);
    painter->setFont(font);
    painter->setPen(QColor(QString::fromLatin1(UiStyle::primary)));
    const QRect titleRect(contentLeft + 24, contentRow.top(),
                          row.right() - contentLeft - 31, contentRow.height());
    const QString title = QFontMetrics(font).elidedText(
        displayTitle(*item).simplified(), Qt::ElideRight, titleRect.width());
    painter->drawText(titleRect,
                      Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                      title);
    if (item->childCount() != 0) {
      const QRect indicator(itemRect.left() - ThreadIndent +
                                DisclosureIndicatorOffset,
                            contentRow.top(), ThreadIndent, contentRow.height());
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
          QPen(QColor(QString::fromLatin1(UiStyle::threadFocusBorder)), 1.0));
      painter->drawRoundedRect(QRectF(row).adjusted(0.5, 3.5, -0.5, -3.5), 8.0,
                               8.0);
    }
    painter->restore();
    painter->restore();
  }

private:
  static QFont titleFont(QFont font, const ThreadTreeItem &item) {
    font.setWeight(item.kind == ui::ThreadRowKind::Thread
                       ? QFont::Medium : QFont::DemiBold);
    return font;
  }

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
    result.focused = visible && tree_->rowView(tree_->indexFromItem(current))->hasFocus() && tree_->currentItem() == current;
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
    const auto index = tree_->indexFromItem(current);
    auto* view = tree_->rowView(index);
    const QRect bounds(view->viewport()->mapTo(tree_->viewport(), QPoint{}), view->viewport()->size());
    const QRect local = tree_->rowRect(index).intersected(bounds).intersected(tree_->viewport()->rect());
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
      tree_->scrollTo(tree_->indexFromItem(current));
      tree_->rowView(tree_->indexFromItem(current))->setFocus(Qt::OtherFocusReason);
      if (action == QAccessibleActionInterface::pressAction() &&
          current->kind != ui::ThreadRowKind::Thread)
        tree_->activateItem(current);
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
    return view && current && view->rowView(view->indexFromItem(current))->hasFocus() && itemIsVisible(*view, current)
               ? view->accessibleItem(current)
               : nullptr;
  }
  int selectedItemCount() const override {
    return selectedItems().size();
  }
  QList<QAccessibleInterface *> selectedItems() const override {
    QList<QAccessibleInterface *> result;
    if (tree()) {
      for (const auto& index : tree()->selectionModel()->selectedRows()) {
        if (!index.parent().isValid()) {
          result.push_back(tree()->accessibleItem(tree()->threadItem(index)));
        }
      }
    }
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

// Viewport focus resolves to the same stable logical row interfaces as the tree.
class ProjectThreadAccessible final : public QAccessibleWidget {
public:
    explicit ProjectThreadAccessible(ProjectThreadView* body)
        : QAccessibleWidget(body, QAccessible::Pane) {
    }
    QAccessibleInterface* focusChild() const override {
        auto* body = static_cast<ProjectThreadView*>(object());
        auto* tree = static_cast<ThreadTreeWidget*>(body->parentWidget()->parentWidget());
        return body->hasFocus() ? tree->accessibleItem(tree->threadItem(body->currentIndex())) : nullptr;
    }
};

QAccessibleInterface* threadAccessibleFactory(const QString&, QObject* object) {
    auto* tree = dynamic_cast<ThreadTreeWidget*>(object);
    auto* body = dynamic_cast<ProjectThreadView*>(object);
    return tree ? static_cast<QAccessibleInterface*>(new ThreadTreeAccessible(tree)) : body ? new ProjectThreadAccessible(body) : nullptr;
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

void ThreadTreeWidget::positionProjectViews() {
    projectViews_.removeIf([this](ProjectThreadView* body) {
        const QModelIndex root = body->rootIndex();
        const QRect card = QTreeWidget::visualRect(root);
        const bool retire = !root.isValid() || !isExpanded(root) || (!card.intersects(viewport()->rect()) && !body->hasFocus());
        if (retire) {
            delete body;
        }
        return retire;
    });
    QModelIndex root = QTreeWidget::indexAt(QPoint(viewport()->width() / 2, 0));
    while (root.isValid() && QTreeWidget::visualRect(root).top() < viewport()->height()) {
        const auto* item = threadItem(root);
        if (!root.parent().isValid() && item->kind == ui::ThreadRowKind::Project && isExpanded(root) && item->childCount()) {
            auto found = std::find_if(projectViews_.begin(), projectViews_.end(), [&](auto* body) {
                return body->rootIndex() == root;
            });
            ProjectThreadView* body = found == projectViews_.end() ? nullptr : *found;
            if (!body) {
                body = new ProjectThreadView(viewport());
                const QSignalBlocker blocked(body);
                body->setObjectName(QStringLiteral("projectThreadBody"));
                body->setModel(model());
                body->setSelectionModel(selectionModel());
                body->setItemDelegate(itemDelegate());
                body->setRootIndex(root);
                body->setHeaderHidden(true);
                body->setIndentation(ThreadIndent);
                body->setSelectionBehavior(selectionBehavior());
                body->setMouseTracking(true);
                body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                body->setStyle(style());
                body->setStyleSheet(
                    QStringLiteral("QTreeView#projectThreadBody{background:transparent;border:0;padding-left:%1px;outline:0;}")
                        .arg(ThreadIndent));
                connect(body, &QTreeView::expanded, this, [this](const QModelIndex& index) {
                    setExpanded(index, true);
                });
                connect(body, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
                    setExpanded(index, false);
                });
                connect(body, &QTreeView::activated, this, [this](const QModelIndex& index) {
                    owner_->activateBrowserRow(threadItem(index));
                });
                connect(body, &QTreeView::clicked, this, [this](const QModelIndex& index) {
                    if (threadItem(index)->kind == ui::ThreadRowKind::Page) {
                        owner_->activateBrowserRow(threadItem(index));
                    }
                });
                body->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(body, &QTreeView::customContextMenuRequested, this, [this, body](const QPoint& point) {
                    owner_->showContextMenu(body->viewport()->mapTo(viewport(), point));
                });
                connect(body->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, body](int value) {
                    if (auto* project = threadItem(body->rootIndex())) {
                        project->projectScroll = value;
                    }
                    owner_->updateAnimationTimer();
                    owner_->requestMoreNearListEnd();
                });
                projectViews_.append(body);
            }
            const std::function<void(const QModelIndex&)> projectExpansion = [&](const QModelIndex& parent) {
                for (int row = 0; row < model()->rowCount(parent); ++row) {
                    const QModelIndex child = model()->index(row, 0, parent);
                    body->setExpanded(child, isExpanded(child));
                    if (isExpanded(child)) {
                        projectExpansion(child);
                    }
                }
            };
            const QSignalBlocker expansion(body);
            projectExpansion(root);
            QRect bounds = QTreeWidget::visualRect(root);
            bounds.setLeft(1);
            bounds.setRight(viewport()->width() - 2);
            bounds.setTop(bounds.top() + UiStyle::projectCardEdgePadding + compactRowHeight());
            bounds.setHeight(projectBodyHeight(root));
            const QSignalBlocker scrolling(body->verticalScrollBar());
            const int offset = item->projectScroll;
            body->setGeometry(bounds);
            body->setAccessibleName(text(item->title));
            body->show();
            body->doItemsLayout();
            body->verticalScrollBar()->setValue(offset);
            threadItem(root)->projectScroll = body->verticalScrollBar()->value();
        }
        root = QTreeWidget::indexBelow(root);
    }
}

void ThreadTreeWidget::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QAbstractItemView::currentChanged(current, previous);
    if (allColumnsShowFocus()) {
        for (const auto& index : {previous, current}) {
            if (index.isValid()) {
                updateRow(index);
            }
        }
    }
#if QT_CONFIG(accessibility)
    if (!signalsBlocked() && current.isValid() && rowView(current)->hasFocus())
        sendThreadEvent(accessibleItemForEvent(threadItem(current)), QAccessible::Focus);
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
               QString::fromLatin1(UiStyle::newThreadText),
               QString::fromLatin1(UiStyle::newThreadBorder),
               QString::fromLatin1(UiStyle::newThreadHoverSurface),
               QString::fromLatin1(UiStyle::newThreadHoverBorder),
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
        addSortAction(QStringLiteral("Recent"), SortCriterion::Recency)},
       {SortCriterion::SectionOrder,
        addSortAction(QStringLiteral("Section order"),
                      SortCriterion::SectionOrder)}}};
  sortButton->setMenu(sortMenu);
  sortButton->setToolTip(QStringLiteral("Sort threads"));
  sortActions.back().action->setToolTip(
      QStringLiteral("Server section order. In All, active threads precede "
                     "archived threads, each in its server order."));
  updateSortButton();
  toolbar->addWidget(sortButton);
  groupButton = new UiStyle::ChevronToolButton;
  groupButton->setObjectName(QStringLiteral("threadGroupingButton"));
  groupButton->setText(QStringLiteral("Projects"));
  groupButton->setAccessibleName(QStringLiteral("Group threads"));
  groupButton->setToolTip(QStringLiteral("Group threads"));
  groupButton->setProperty("kind", "subtle");
  groupButton->setProperty("codexChevron", true);
  groupButton->setMinimumHeight(28);
  groupButton->setPopupMode(QToolButton::InstantPopup);
  auto *groupsMenu = new QMenu(groupButton);
  auto *groupModes = new QActionGroup(groupsMenu);
  for (const auto &[label, mode] : std::array{
           std::pair{QStringLiteral("Projects → Sections"),
                     ui::ThreadGrouping::Projects},
           std::pair{QStringLiteral("Sections"), ui::ThreadGrouping::Sections},
           std::pair{QStringLiteral("Ungrouped"),
                     ui::ThreadGrouping::Ungrouped}}) {
    auto *action = groupsMenu->addAction(label, this, [this, mode] {
      browser.grouping = mode;
      changeBrowser();
    });
    action->setCheckable(true);
    action->setData(static_cast<int>(mode));
    groupModes->addAction(action);
  }
  auto *manual = groupsMenu->addAction(QStringLiteral("Manual project order"));
  manual->setCheckable(true);
  connect(manual, &QAction::toggled, this, [this](bool enabled) {
    browser.manualProjects = enabled;
    changeBrowser();
  });
  groupsMenu->addSeparator();
  groupsMenu->addAction(QStringLiteral("New project…"), this, [this] {
    if (canControl)
      editGroup(true);
  });
  groupsMenu->addAction(QStringLiteral("New section…"), this, [this] {
    if (canControl)
      editGroup(false);
  });
  groupsMenu->addAction(QStringLiteral("Refresh groups"), this, [this] {
    if (actions.refreshGroups)
      actions.refreshGroups();
  });
  for (const auto &[label, method] : std::array{
           std::pair{QStringLiteral("Load more projects"), "project/list"},
           std::pair{QStringLiteral("Load more sections"),
                     "threadSection/list"}})
    groupsMenu->addAction(label, this, [this, method] {
      if (actions.browse)
        actions.browse(
            {{"method", method}, {"parameters", nlohmann::json::object()}});
    });
  groupButton->setMenu(groupsMenu);
  connect(groupsMenu, &QMenu::aboutToShow, this,
          [this, groupsMenu, manual, groupModes] {
    for (auto *action : groupModes->actions())
      action->setChecked(action->data().toInt() ==
                         static_cast<int>(browser.grouping));
    QSignalBlocker blocked(manual);
    manual->setChecked(browser.manualProjects);
    for (auto *action : groupsMenu->actions())
      if (action->text().startsWith(QStringLiteral("New ")))
        action->setEnabled(canControl && groupingAvailable);
  });
  toolbar->insertWidget(0, groupButton);
  layout->addLayout(toolbar);
  auto *filters = new QHBoxLayout;
  auto *search = new QLineEdit;
  search->setObjectName(QStringLiteral("threadTitleSearch"));
  search->setPlaceholderText(QStringLiteral("Search titles · Enter"));
  search->setAccessibleName(QStringLiteral("Search thread titles"));
  search->setMinimumWidth(0);
  search->setClearButtonEnabled(true);
  auto *archive = new UiStyle::ChevronComboBox;
  archive->setObjectName(QStringLiteral("threadArchiveFilter"));
  archive->addItems({QStringLiteral("Active"), QStringLiteral("Archived"),
                     QStringLiteral("All")});
  archive->setAccessibleName(QStringLiteral("Thread archive filter"));
  connect(search, &QLineEdit::returnPressed, this, [this, search] {
    browser.title = search->text().trimmed().toUtf8().toStdString();
    changeBrowser();
  });
  connect(search, &QLineEdit::textChanged, this, [this](const QString &value) {
    if (value.isEmpty() && !browser.title.empty()) {
      browser.title.clear();
      changeBrowser();
    }
  });
  connect(archive, &QComboBox::currentIndexChanged, this, [this](int value) {
    browser.archived = value;
    changeBrowser();
  });
  filters->addWidget(search, 1);
  filters->addWidget(archive);
  layout->addLayout(filters);
  layout->addSpacing(8);

  tree = new ThreadTreeWidget(this);
  tree->setObjectName(QStringLiteral("threadList"));
  tree->setAccessibleName(QStringLiteral("Threads"));
  tree->setHeaderHidden(true);
  tree->setIndentation(ThreadIndent);
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
  tree->setStyleSheet(QStringLiteral(
      "QTreeWidget#threadList{background:transparent;border:0;outline:0;"
      "show-decoration-selected:0;}"
      "QTreeWidget#threadList::item{background:transparent;border:0;"
      "min-height:30px;}"));

  connect(tree, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
            const auto *selected = tree->threadItem(current);
            if (!selected || selected->kind != ui::ThreadRowKind::Thread ||
                !selected->target ||
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
  connect(tree, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem *item) {
            activateBrowserRow(tree->threadItem(item));
          });
  connect(tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
    if (tree->threadItem(item)->kind == ui::ThreadRowKind::Page)
      activateBrowserRow(tree->threadItem(item));
  });
  connect(tree, &QTreeWidget::itemExpanded, this,
          [this](QTreeWidgetItem *item) {
            updateAnimationTimer();
            const auto kind = tree->threadItem(item)->kind;
            if (!tree->signalsBlocked() &&
                (kind == ui::ThreadRowKind::Project ||
                 kind == ui::ThreadRowKind::Section)) {
              QSettings().setValue(
                  settingsKey() + QStringLiteral("/expanded/") +
                      text(tree->threadItem(item)->presentationKey),
                  true);
              activateBrowserRow(tree->threadItem(item));
            }
#if QT_CONFIG(accessibility)
            sendThreadExpansionEvent(
                tree->accessibleItemForEvent(tree->threadItem(item)));
#endif
          });
  connect(tree, &QTreeWidget::itemCollapsed, this,
          [this](QTreeWidgetItem *item) {
            updateAnimationTimer();
            const auto kind = tree->threadItem(item)->kind;
            if (kind == ui::ThreadRowKind::Project ||
                kind == ui::ThreadRowKind::Section)
              QSettings().setValue(
                  settingsKey() + QStringLiteral("/expanded/") +
                      text(tree->threadItem(item)->presentationKey),
                  false);
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

QAbstractScrollArea* ThreadPane::scrollArea() const {
    return tree;
}

QString ThreadPane::settingsKey() const {
  return QStringLiteral("threads/") +
         QString::fromLatin1(
             QCryptographicHash::hash(QByteArray::fromStdString(serverIdentity),
                                      QCryptographicHash::Sha256)
                 .toHex());
}

void ThreadPane::changeBrowser() {
  QSettings settings;
  settings.setValue(settingsKey() + "/grouping",
                    static_cast<int>(browser.grouping));
  settings.setValue(settingsKey() + "/manualProjects", browser.manualProjects);
  settings.setValue(settingsKey() + "/sort", static_cast<int>(sortCriterion));
  groupButton->setText(browser.grouping == ui::ThreadGrouping::Projects
                           ? QStringLiteral("Projects")
                       : browser.grouping == ui::ThreadGrouping::Sections
                           ? QStringLiteral("Sections")
                           : QStringLiteral("Ungrouped"));
  if (actions.browserChanged)
    actions.browserChanged();
  if (actions.browse) {
    nlohmann::json parameters = nlohmann::json::object();
    parameters["archived"] = browser.archived == 1;
    if (!browser.title.empty())
      parameters["searchTerm"] = browser.title;
    actions.browse({{"parameters", parameters},
                    {"refresh", true},
                    {"reset", true},
                    {"includeArchived", browser.archived == 2}});
    if (browser.archived == 2) {
      parameters["archived"] = true;
      actions.browse({{"parameters", parameters}, {"refresh", true}});
    }
    // Existing expanded groups are refreshed with the new filters by the next
    // projection's page rows. No network call is made from a paint operation.
  }
}

void ThreadPane::activateBrowserRow(ThreadTreeItem *item) {
  if (!item || item->query.is_null() || item->pendingPage || !actions.browse ||
      !providerReady)
    return;
  if (item->kind == ui::ThreadRowKind::Page) {
    // A failed page is retried only by an explicit click, never by a refresh
    // loop.
    actions.browse(item->query);
  } else {
    for (int i = 0; i < item->childCount(); ++i) {
      auto *child = tree->threadItem(item->child(i));
      if (child->kind == ui::ThreadRowKind::Page && child->details.empty()) {
        activateBrowserRow(child);
      }
    }
  }
}

void ThreadPane::editGroup(bool project, const std::string &id, bool readOnly) {
  const auto &source = project ? projects : sections;
  const auto found = std::ranges::find(source, id, &ui::ThreadGroup::id);
  const nlohmann::json fields =
      found == source.end() ? nlohmann::json::object() : found->fields;
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("threadGroupDialog"));
  dialog.setWindowTitle(project ? QStringLiteral("Project")
                                : QStringLiteral("Section"));
  dialog.resize(520, project ? 480 : 300);
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  auto *name = new QLineEdit(text(fields.value("name", std::string{})));
  name->setObjectName(QStringLiteral("groupName"));
  name->setReadOnly(readOnly);
  form->addRow(QStringLiteral("Name"), name);
  if (readOnly && !id.empty()) {
    auto *identifier = new QLineEdit(text(id));
    identifier->setReadOnly(true);
    identifier->setObjectName(QStringLiteral("groupId"));
    form->addRow(QStringLiteral("ID"), identifier);
  }
  QPlainTextEdit *roots = nullptr;
  QPlainTextEdit *description = nullptr;
  QLineEdit *icon = nullptr;
  QLineEdit *color = nullptr;
  if (project) {
    roots = new UiStyle::DialogTextEdit;
    roots->setObjectName(QStringLiteral("projectRoots"));
    roots->setProperty("kind", "dialogEditor");
    roots->setReadOnly(readOnly);
    roots->setMaximumHeight(90);
    QStringList paths;
    for (const auto &root : fields.value("roots", nlohmann::json::array()))
      paths.push_back(text(root.value("path", std::string{})));
    roots->setPlainText(paths.join('\n'));
    roots->setPlaceholderText(
        QStringLiteral("Absolute workspace paths, one per line"));
    form->addRow(QStringLiteral("Roots"), roots);
    description = new UiStyle::DialogTextEdit;
    description->setObjectName(QStringLiteral("projectDescription"));
    description->setProperty("kind", "dialogEditor");
    description->setReadOnly(readOnly);
    const auto metadata = fields.value("metadata", nlohmann::json::object());
    description->setPlainText(
        text(metadata.value(ProjectDescriptionKey, std::string{})));
    form->addRow(QStringLiteral("Description"), description);
    layout->addWidget(UiStyle::makeLabel(
        QStringLiteral(
            "Descriptions organise work; they are not model instructions."),
        "muted"));
    if (readOnly) {
      auto *details = new QPlainTextEdit(text(fields.dump(2)));
      details->setReadOnly(true);
      details->setAccessibleName(
          QStringLiteral("Project metadata and timestamps"));
      form->addRow(QStringLiteral("Metadata"), details);
    }
  } else {
    const auto appearance =
        fields.contains("appearance") && fields["appearance"].is_object()
            ? fields["appearance"]
            : nlohmann::json::object();
    const auto component = [&](const char *key) {
      const auto found = appearance.find(key);
      return found != appearance.end() && found->is_string()
                 ? text(found->get<std::string>())
                 : QString{};
    };
    icon = new QLineEdit(component("icon"));
    color = new QLineEdit(component("color"));
    icon->setReadOnly(readOnly);
    color->setReadOnly(readOnly);
    form->addRow(QStringLiteral("Icon"), icon);
    form->addRow(QStringLiteral("Colour"), color);
    layout->addWidget(UiStyle::makeLabel(
        QStringLiteral(
            "Sections are shared across projects. Changes apply everywhere."),
        "muted"));
  }
  layout->addLayout(form, 1);
  auto *error = UiStyle::makeLabel({}, "muted");
  error->setWordWrap(true);
  layout->addWidget(error);
  auto *buttons = new QDialogButtonBox(readOnly ? QDialogButtonBox::Close
                                                : QDialogButtonBox::Save |
                                                      QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    if (!canControl || !actions.manage)
      return;
    if (name->text().trimmed().isEmpty()) {
      error->setText(QStringLiteral("Enter a name."));
      return;
    }
    nlohmann::json parameters{{"name", name->text().trimmed().toStdString()}};
    if (project) {
      parameters["roots"] = nlohmann::json::array();
      for (const auto &path :
           roots->toPlainText().split('\n', Qt::SkipEmptyParts)) {
        if (!QDir::isAbsolutePath(path.trimmed())) {
          error->setText(
              QStringLiteral("Workspace roots must be absolute paths."));
          return;
        }
        parameters["roots"].push_back({{"path", path.trimmed().toStdString()}});
      }
      const auto value = description->toPlainText().toStdString();
      if (id.empty()) {
        parameters["metadata"] = {{ProjectDescriptionKey, value}};
        parameters["idempotencyKey"] =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
      } else {
        parameters["projectId"] = id;
        parameters["description"] = value;
      }
    } else {
      if (!id.empty())
        parameters["sectionId"] = id;
      parameters["appearance"] = {
          {"icon", icon->text().isEmpty()
                       ? nlohmann::json(nullptr)
                       : nlohmann::json(icon->text().toStdString())},
          {"color", color->text().isEmpty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(color->text().toStdString())}};
    }
    using enum nodegraph::RuntimeActionKind;
    actions.manage(project ? (id.empty() ? CreateProject : UpdateProject)
                           : (id.empty() ? CreateSection : UpdateSection),
                   std::move(parameters));
    dialog.accept();
  });
  dialog.exec();
}

void ThreadPane::showGroupMenu(QMenu *menu, ThreadTreeItem *item) {
  using enum nodegraph::RuntimeActionKind;
  const auto invoke = [this, menu](QString label,
                                   nodegraph::RuntimeActionKind kind,
                                   nlohmann::json parameters) {
    auto *action = menu->addAction(
        label, this, [this, kind, parameters = std::move(parameters)] {
          if (canControl && actions.manage)
            actions.manage(kind, parameters);
        });
    action->setEnabled(canControl && groupingAvailable);
  };
  const bool project = item->kind == ui::ThreadRowKind::Project;
  const bool group = item->kind != ui::ThreadRowKind::Thread;
  const std::string id = item->id;
  if (group) {
      menu->addAction(QStringLiteral("New thread here…"),
                      this,
                      [this, projectId = item->projectId, sectionId = item->sectionId] {
                          if (canControl && actions.newGroupedThread) {
                              actions.newGroupedThread(projectId, sectionId);
                          }
                      })
          ->setEnabled(canControl);
      if (!id.empty()) {
          menu->addAction(QStringLiteral("Details…"), this, [this, project, id] {
              editGroup(project, id, true);
          });
          menu->addAction(QStringLiteral("Edit…"),
                          this,
                          [this, project, id] {
                              if (canControl)
                                  editGroup(project, id);
                          })
              ->setEnabled(canControl);
          menu->addAction(QStringLiteral("Delete group…"),
                          this,
                          [this, project, id] {
                              if (canControl && actions.manage &&
                                  QMessageBox::question(this,
                                                        QStringLiteral("Delete group"),
                                                        QStringLiteral("Delete this group? Its threads will be "
                                                                       "retained, not deleted.")) == QMessageBox::Yes)
                                  actions.manage(project ? DeleteProject : DeleteSection, {{project ? "projectId" : "sectionId", id}});
                          })
              ->setEnabled(canControl);
      }
    if (project && !id.empty()) {
      auto *before = menu->addMenu(QStringLiteral("Move project before"));
      before->setEnabled(canControl);
      for (const auto &entry : projects)
        if (entry.id != id)
          before->addAction(
              text(entry.label).replace("&", "&&"), this,
              [this, id, destination = entry.id] {
                if (canControl && actions.manage)
                  actions.manage(
                      MoveProject,
                      {{"projectId", id}, {"beforeProjectId", destination}});
              });
      invoke(QStringLiteral("Move project to end"), MoveProject,
             {{"projectId", id}});
    }
    return;
  }
  if (item->parentThread)
    menu->addAction(QStringLiteral("Go to parent"), this,
                    [this, target = item->parentThread] {
                      if (actions.select)
                        actions.select(target);
                    });
  for (bool isProject : {true, false}) {
    auto *assign = menu->addMenu(isProject ? QStringLiteral("Assign project")
                                           : QStringLiteral("Assign section"));
    assign->setEnabled(canControl && groupingAvailable);
    for (const auto &destination : isProject ? projects : sections)
      assign->addAction(
          text(destination.label).replace("&", "&&"), this,
          [this, isProject, id, destinationId = destination.id] {
            if (canControl && actions.manage)
              actions.manage(
                  isProject ? AssignProject : AssignSection,
                  {{"threadId", id},
                   {isProject ? "projectId" : "sectionId", destinationId}});
          });
  }
  invoke(QStringLiteral("Remove from project"), AssignProject,
         {{"threadId", id}, {"projectId", ""}});
  invoke(QStringLiteral("Remove from section"), AssignSection,
         {{"threadId", id}, {"sectionId", nullptr}});
  if (!item->sectionId.empty()) {
    auto *before =
        menu->addMenu(QStringLiteral("Move before in shared section"));
    before->setEnabled(canControl);
    for (const auto &[key, row] : rows)
      if (row != item && row->kind == ui::ThreadRowKind::Thread &&
          row->sectionId == item->sectionId)
        before->addAction(
            text(row->title), this,
            [this, id, section = item->sectionId, beforeId = row->id] {
              if (canControl && actions.manage)
                actions.manage(AssignSection, {{"threadId", id},
                                               {"sectionId", section},
                                               {"beforeThreadId", beforeId}});
            });
    invoke(QStringLiteral("Move to end of shared section"), AssignSection,
           {{"threadId", id}, {"sectionId", item->sectionId}});
  }
  menu->addSeparator();
}

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
  assign(item->workspaceChangeUnavailable, row.workspaceChangeUnavailable);
  assign(item->status, row.status);
  assign(item->indirectActive, row.indirectActive);
  assign(item->createdAt, row.createdAt);
  assign(item->timing, row.timing);
  assign(item->recencyAt, row.recencyAt);
  assign(item->lastActivityAt, row.lastActivityAt);
  assign(item->pending, row.pending);
  assign(item->archived, row.archived);
  assign(item->kind, row.kind);
  assign(item->projectId, row.projectId);
  assign(item->sectionId, row.sectionId);
  assign(item->details, row.details);
  assign(item->query, row.query);
  assign(item->pendingPage, row.pendingPage);
  assign(item->draft, draft);
  assign(item->awaitingPrompt, row.awaitingPromptConversation);
  assign(item->animationEpoch,
         static_cast<qint64>(draft
                                 ? draftStartedAt.value_or(0)
                                 : row.pendingPromptAdmittedAtMs.value_or(0)));
  if (!changed)
    return false;
  if (item->treeWidget() == tree) {
      tree->updateRow(tree->indexFromItem(item));
  }
#if QT_CONFIG(accessibility)
  for (const auto &[observedItem, state] : observed)
    notifyThreadAccessibility(observedItem, state);
#endif
  return true;
}

void ThreadPane::beginOptimisticThread(const ui::ThreadListRow &draft) {
  if (draftItem && draftItem->presentationKey != draft.presentationKey)
    retireOptimisticThread();
  if (!draftItem) {
    auto *item = new ThreadTreeItem(tree, draft.presentationKey);
    draftItem = item;
    rows.emplace(item->presentationKey, item);
    tree->insertTopLevelItem(0, item);
  }
  static_cast<void>(applyItemPresentation(draftItem, draft, true,
                                          QDateTime::currentMSecsSinceEpoch()));
  static_cast<void>(repositionRootItem(draftItem));
  tree->setCurrentItem(draftItem);
  tree->setActiveThread(draftItem);
  updateAnimationTimer();
}

std::optional<ui::ThreadListRow> ThreadPane::optimisticThread() const {
  if (!draftItem)
    return {};
  ui::ThreadListRow row;
  row.id = draftItem->id;
  row.presentationKey = draftItem->presentationKey;
  row.title = draftItem->title;
  row.cwd = draftItem->cwd;
  row.createdAt = row.recencyAt = draftItem->animationEpoch / 1000;
  row.projectId = draftItem->projectId;
  row.sectionId = draftItem->sectionId;
  return row;
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
  browser.manualSections = criterion == SortCriterion::SectionOrder;
  updateSortButton();
  sortRootItems();
  if (actions.browserChanged)
    changeBrowser();
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
  projects = snapshot.projects;
  sections = snapshot.sections;
  groupingAvailable = snapshot.groupingAvailable;
  if (serverIdentity != snapshot.serverIdentity) {
    serverIdentity = snapshot.serverIdentity;
    const QSettings settings;
    browser.grouping = static_cast<ui::ThreadGrouping>(std::clamp(
        settings.value(settingsKey() + "/grouping", 0).toInt(), 0, 2));
    browser.manualProjects =
        settings.value(settingsKey() + "/manualProjects", false).toBool();
    sortCriterion = static_cast<SortCriterion>(
        std::clamp(settings.value(settingsKey() + "/sort", 2).toInt(), 0, 3));
    browser.manualSections = sortCriterion == SortCriterion::SectionOrder;
    updateSortButton();
    if (actions.browserChanged)
      actions.browserChanged();
  }
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
  groupButton->setText(!groupingAvailable ? QStringLiteral("Groups unavailable")
                       : browser.grouping == ui::ThreadGrouping::Projects
                           ? QStringLiteral("Projects")
                       : browser.grouping == ui::ThreadGrouping::Sections
                           ? QStringLiteral("Sections")
                           : QStringLiteral("Ungrouped"));

  const auto *currentBefore = tree->threadItem(tree->currentItem());
  const bool hadCurrent = currentBefore != nullptr;
  const std::string currentKey =
      currentBefore ? currentBefore->presentationKey : std::string{};
  const nodegraph::NodeRef currentTarget =
      currentBefore ? currentBefore->target : nodegraph::NodeRef{};
#if QT_CONFIG(accessibility)
  const QModelIndexList selectedBeforeItems = tree->selectionModel()->selectedRows();
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
    const bool createdItem = !item;
    if (!item) {
      item = new ThreadTreeItem(tree, row.presentationKey);
      rows.emplace(row.presentationKey, item);
    }
    if (!parent && position == 0 && sortCriterion == SortCriterion::Recency) {
      const auto *newest = tree->threadItem(tree->topLevelItem(0));
      revealNewest =
          newest && newest != item && row.recencyAt > newest->recencyAt;
    }
    if (item == draftItem && row.target) {
        draftItem = nullptr;
    }
    static_cast<void>(applyItemPresentation(item, row, item == draftItem,
                                            item->animationEpoch, false));
    const bool reparented = place(item, parent, position);
    if (item->presentationKey == currentKey && item->target == currentTarget) {
        currentReparented = reparented || ancestorReparented;
    }
    // These describe placement, not a targeted status/title presentation.
    item->parentThread = row.parentThread;
    item->order = row.order;
    if (row.kind == ui::ThreadRowKind::Thread &&
        row.id == snapshot.selectedThreadId) {
      selected = item;
      selectedReparented = reparented || ancestorReparented;
    }
    std::vector<const ui::ThreadListRow *> children;
    for (const auto &child : row.children)
      children.push_back(&child);
    if (row.kind != ui::ThreadRowKind::Thread)
      std::ranges::stable_sort(children,
                               [this](const auto *left, const auto *right) {
                                 return tree->rowBefore(*left, *right);
                               });
    for (std::size_t child = 0; child < children.size(); ++child)
      reconcile(*children[child], item, static_cast<int>(child),
                reparented || ancestorReparented);
    if (createdItem && row.kind != ui::ThreadRowKind::Page) {
        item->setExpanded(row.kind == ui::ThreadRowKind::Thread ||
                          QSettings().value(settingsKey() + "/expanded/" + text(item->presentationKey), false).toBool());
    }
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
  int rootPosition = 0;
  if (retainDraft)
    wanted.insert(draftItem->presentationKey);
  for (const ui::ThreadListRow *row : roots) {
    if (retainDraft && !draftItem->parent() &&
        rootPosition == tree->indexOfTopLevelItem(draftItem))
      ++rootPosition;
    reconcile(*row, nullptr, rootPosition++, false);
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

  // Capture the snapshot's active thread before the keyboard-focus fallback.
  // A local draft remains the active conversation until it is materialized.
  tree->setActiveThread(selected ? selected : draftItem);

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
  if (tree->isVisible() && (structureChanged || !sameSelection)) {
      // QAbstractItemView defers scrollbar geometry; settle a structural refresh
      // before restoring its stable semantic anchor or requesting another page.
      tree->doItemsLayout();
      const auto retainedAnchor = rows.find(anchorKey);
      if (revealNewest) {
          tree->scrollToTop();
      } else if (sameSelection && retainedAnchor != rows.end() && retainedAnchor->second->target == anchorTarget) {
          tree->restoreViewportY(retainedAnchor->second, anchorY);
      }
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
  const QModelIndexList selectedAfterItems = tree->selectionModel()->selectedRows();
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
  if (currentAfter && tree->rowView(tree->indexFromItem(currentAfter))->hasFocus() &&
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
  tree->forEachVisibleRow([&](const QModelIndex& index) {
      const ThreadTreeItem* item = tree->threadItem(index);
      if (itemCanAnimate(*item, now)) {
          active = true;
          if (repaint) {
              tree->updateRow(index);
          }
      } else if (item->awaitingPrompt && item->animationEpoch > 0) {
          const qint64 deadline = item->animationEpoch + PendingAnimationDelayMilliseconds;
          earliest = std::min(earliest, deadline - now);
      }
  });
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
  if (actions.browse) {
      tree->forEachVisibleRow([&](const QModelIndex& index) {
          auto* item = tree->threadItem(index);
          if (item->kind == ui::ThreadRowKind::Page && !item->pendingPage && item->details.empty()) {
              activateBrowserRow(item);
          }
      });
      return;
  }
  if (!actions.loadMore || tree->topLevelItemCount() == 0)
    return;
  const QScrollBar *scroll = tree->verticalScrollBar();
  const int threshold = std::max(48, scroll->pageStep() / 2);
  if (scroll->maximum() == 0 ||
      scroll->value() >= scroll->maximum() - threshold)
    actions.loadMore();
}

std::optional<ThreadPane::VisibleThread> ThreadPane::visiblySelectedThread() const {
    const QModelIndexList selected = tree->selectionModel()->selectedRows();
    std::optional<VisibleThread> result;
    if (selected.size() == 1) {
        const auto* item = tree->threadItem(selected.front());
        if (item->kind == ui::ThreadRowKind::Thread) {
            result = VisibleThread{item->id, item->presentationKey, item->target};
        }
    }
    return result;
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
  tree->updateRow(tree->indexFromItem(found->second));
}

void ThreadPane::showContextMenu(const QPoint &position) {
  auto *item = tree->threadItem(tree->itemAt(position));
  if (!item || item->kind == ui::ThreadRowKind::Page)
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
  if (item->kind != ui::ThreadRowKind::Thread) {
    showGroupMenu(menu, item);
    menu->popup(tree->viewport()->mapToGlobal(position));
    return;
  }
  showGroupMenu(menu, item);
  const auto addAction = [this, menu, target](QString label,
                                              ThreadAction Actions::*member) {
    return menu->addAction(std::move(label), this, [this, target, member] {
      const ThreadAction &action = actions.*member;
      if (action)
        action(target);
    });
  };
  QAction *reload = addAction(QStringLiteral("Reload"), &Actions::reload);
  addAction(QStringLiteral("Timing details"), &Actions::timing);
  QAction* workspace = addAction(QStringLiteral("Change workspace…"), &Actions::changeWorkspace);
  workspace->setEnabled(canControl && target && item->workspaceChangeUnavailable.empty());
  workspace->setToolTip(target ? text(item->workspaceChangeUnavailable) : QStringLiteral("An existing thread is required."));
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
