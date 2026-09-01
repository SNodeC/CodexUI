// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"

#include "codex/PresentationStatus.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractItemView>
#include <QActionGroup>
#include <QCollator>
#include <QDateTime>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int ContextMenuRole = Qt::UserRole + 1;
constexpr int DepthRole = Qt::UserRole + 2;
constexpr int HasChildrenRole = Qt::UserRole + 3;
constexpr int ExpandedRole = Qt::UserRole + 4;
constexpr int ParentIdRole = Qt::UserRole + 5;
constexpr int OptimisticRole = Qt::UserRole + 6;
constexpr int OptimisticFailedRole = Qt::UserRole + 7;
constexpr int ChildIndent = 16;
constexpr int DisclosureWidth = 16;
constexpr int DisclosureExtent = 24;

class ThreadListWidget final : public QListWidget {
public:
  std::function<void(const std::string &)> toggleExpansion;
  std::function<void(int)> navigateHierarchy;

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
    return QListWidget::selectionCommand(index, event);
  }

  void mousePressEvent(QMouseEvent *event) override {
    QListWidgetItem *item = itemAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && item &&
        item->data(HasChildrenRole).toBool()) {
      QWidget *row = itemWidget(item);
      QWidget *indicator = row ? row->findChild<QWidget *>(
                                     QStringLiteral("threadExpansionIndicator"))
                               : nullptr;
      const QRect indicatorRect =
          indicator
              ? QRect(indicator->mapTo(viewport(), QPoint()), indicator->size())
                    .adjusted(-(DisclosureExtent - DisclosureWidth) / 2, 0,
                              (DisclosureExtent - DisclosureWidth) / 2, 0)
              : QRect{};
      if (indicatorRect.contains(event->position().toPoint())) {
        if (toggleExpansion)
          toggleExpansion(item->data(Qt::UserRole).toString().toStdString());
        event->accept();
        return;
      }
    }
    QListWidget::mousePressEvent(event);
  }

  void keyPressEvent(QKeyEvent *event) override {
    if ((event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) &&
        navigateHierarchy) {
      navigateHierarchy(event->key());
      event->accept();
      return;
    }
    QListWidget::keyPressEvent(event);
  }
};

class ThreadItemDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem effective = option;
    if (index.data(ContextMenuRole).toBool())
      effective.state |= QStyle::State_MouseOver;
    QStyledItemDelegate::paint(painter, effective, index);
    if (!index.data(OptimisticRole).toBool())
      return;

    const QRectF bounds = QRectF(option.rect).adjusted(1.0, 4.0, -1.0, -4.0);
    const bool failed = index.data(OptimisticFailedRole).toBool();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(failed ? QColor(QStringLiteral("#fff0f2"))
                             : QColor(QStringLiteral("#fff7e8")));
    painter->setPen(QPen(failed ? QColor(QStringLiteral("#efb8c0"))
                                : QColor(QStringLiteral("#dca45a")),
                         1.0));
    painter->drawRoundedRect(bounds, 8.0, 8.0);
    if (!failed) {
      constexpr qint64 HalfCycleMilliseconds = 900;
      const qint64 phase =
          QDateTime::currentMSecsSinceEpoch() % (2 * HalfCycleMilliseconds);
      const qreal position = phase <= HalfCycleMilliseconds
                                 ? qreal(phase) / HalfCycleMilliseconds
                                 : qreal(2 * HalfCycleMilliseconds - phase) /
                                       HalfCycleMilliseconds;
      const qreal center = bounds.left() + position * bounds.width();
      const qreal radius = std::max(24.0, bounds.width() * 0.22);
      QLinearGradient sweep(center - radius, 0.0, center + radius, 0.0);
      sweep.setColorAt(0.0, QColor(220, 164, 90, 0));
      sweep.setColorAt(0.5, QColor(236, 188, 112, 105));
      sweep.setColorAt(1.0, QColor(220, 164, 90, 0));
      QPainterPath clip;
      clip.addRoundedRect(bounds, 8.0, 8.0);
      painter->setClipPath(clip);
      painter->fillRect(bounds, sweep);
    }
    painter->restore();
  }
};

class ThreadDisclosureIndicator final : public QWidget {
public:
  explicit ThreadDisclosureIndicator(QWidget *parent = nullptr)
      : QWidget(parent) {
    setObjectName(QStringLiteral("threadExpansionIndicator"));
    setFixedSize(DisclosureWidth, DisclosureExtent);
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

  void setState(bool hasChildren, bool expanded) {
    if (hasChildren_ == hasChildren && expanded_ == expanded)
      return;
    hasChildren_ = hasChildren;
    expanded_ = expanded;
    const QString action = !hasChildren ? QString{}
                           : expanded   ? QStringLiteral("Collapse branch")
                                        : QStringLiteral("Expand branch");
    setAccessibleName(action);
    setToolTip(action);
    setProperty("chevronDirection", !hasChildren ? QString{}
                                    : expanded   ? QStringLiteral("down")
                                                 : QStringLiteral("right"));
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    static_cast<void>(event);
    if (!hasChildren_)
      return;
    UiStyle::drawChevron(this, rect().adjusted(3, 3, -3, -3), isEnabled(),
                         false,
                         expanded_ ? UiStyle::ChevronDirection::Down
                                   : UiStyle::ChevronDirection::Right);
  }

private:
  bool hasChildren_ = false;
  bool expanded_ = false;
};

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QLabel *makeLabel(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QFrame *statusDot() {
  auto *dot = new QFrame;
  dot->setObjectName(QStringLiteral("threadStatusDot"));
  dot->setFixedSize(10, 10);
  return dot;
}

void updateRow(QWidget *row, const std::string &threadId,
               const std::string &threadTitle, const std::string &threadStatus,
               std::size_t requestCount, std::size_t depth, bool hasChildren,
               bool expanded, bool optimistic, bool optimisticFailed) {
  auto *title = row->findChild<QLabel *>(QStringLiteral("threadTitle"));
  auto *dot = row->findChild<QFrame *>(QStringLiteral("threadStatusDot"));
  auto *indent = row->findChild<QWidget *>(QStringLiteral("threadIndent"));
  auto *indicator = static_cast<ThreadDisclosureIndicator *>(
      row->findChild<QWidget *>(QStringLiteral("threadExpansionIndicator")));
  indent->setFixedWidth(static_cast<int>(depth) * ChildIndent);
  indicator->setState(hasChildren, expanded);
  QString titleText = text(threadTitle);
  if (titleText.isEmpty())
    titleText = text(threadId.substr(0, 12));
  if (requestCount != 0)
    titleText.prepend(QStringLiteral("! "));
  title->setText(titleText);
  const PresentationStatus classified = classifyStatus(threadStatus);
  QString color = QString::fromLatin1(UiStyle::threadInactive);
  if (optimistic)
    color = optimisticFailed ? QStringLiteral("#c43d4d")
                             : QStringLiteral("#d17b16");
  else if (requestCount != 0)
    color = QString::fromLatin1(UiStyle::orange);
  else if (classified.kind == StatusKind::Active)
    color = QString::fromLatin1(UiStyle::blue);
  else if (classified.kind == StatusKind::Completed)
    color = QString::fromLatin1(UiStyle::green);
  else if (classified.kind == StatusKind::Failed)
    color = QString::fromLatin1(UiStyle::red);
  dot->setStyleSheet(
      QStringLiteral("background:%1;border-radius:5px;").arg(color));
}

QWidget *createRow() {
  auto *row = new QWidget;
  row->setAttribute(Qt::WA_TransparentForMouseEvents);
  row->setStyleSheet(QStringLiteral("background:transparent;"));
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 2, 0, 2);
  layout->setSpacing(0);
  auto *indent = new QWidget;
  indent->setObjectName(QStringLiteral("threadIndent"));
  indent->setFixedWidth(0);
  indent->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  layout->addWidget(indent);
  auto *indicator = new ThreadDisclosureIndicator;
  layout->addWidget(indicator, 0, Qt::AlignVCenter);
  layout->addSpacing(2);
  layout->addWidget(statusDot(), 0, Qt::AlignVCenter);
  layout->addSpacing(8);
  auto *title = makeLabel({}, "title");
  title->setObjectName(QStringLiteral("threadTitle"));
  title->setWordWrap(false);
  title->setStyleSheet(QStringLiteral("font-weight:500;"));
  layout->addWidget(title, 1);
  return row;
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

std::optional<std::int64_t> timestampFor(const ui::ThreadListRow &thread,
                                         ThreadPane::SortCriterion criterion) {
  if (criterion == ThreadPane::SortCriterion::Created)
    return thread.createdAt;
  if (criterion == ThreadPane::SortCriterion::LastChanged)
    return thread.updatedAt;
  return thread.recencyAt;
}

const ui::ThreadListRow *findThread(const ui::ThreadListRow &row,
                                    std::string_view id) {
  if (row.id == id)
    return &row;
  for (const ui::ThreadListRow &child : row.children) {
    if (const ui::ThreadListRow *found = findThread(child, id))
      return found;
  }
  return nullptr;
}

const ui::ThreadListRow *findThread(
    const std::vector<ui::ThreadListRow> &roots, std::string_view id) {
  for (const ui::ThreadListRow &root : roots) {
    if (const ui::ThreadListRow *found = findThread(root, id))
      return found;
  }
  return nullptr;
}

bool expandAncestors(const ui::ThreadListRow &row, std::string_view id,
                     std::unordered_set<std::string> &expanded) {
  if (row.id == id)
    return true;
  for (const ui::ThreadListRow &child : row.children) {
    if (expandAncestors(child, id, expanded)) {
      expanded.insert(row.id);
      return true;
    }
  }
  return false;
}

} // namespace

ThreadPane::ThreadPane(QWidget *parent) : QFrame(parent) {
  setObjectName(QStringLiteral("sidebar"));
  setStyleSheet(QStringLiteral("QFrame#sidebar{background:#f8fafc;}"));
  setMinimumWidth(220);
  setMaximumWidth(440);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 14, 10, 17);
  layout->setSpacing(0);
  auto *header = new QHBoxLayout;
  header->setContentsMargins(8, 0, 6, 0);
  header->addStrut(24);
  auto *sectionTitle = makeLabel(QStringLiteral("THREADS"), "panelHeader");
  sectionTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  sectionTitle->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  header->addWidget(sectionTitle);
  header->addStretch();
  auto *hide = new QPushButton(QStringLiteral("Hide"));
  hide->setProperty("kind", "subtle");
  hide->setFixedSize(52, 24);
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
  create->setFixedHeight(36);
  create->setStyleSheet(QStringLiteral(
      "QPushButton{background:#ffffff;color:#2f6feb;border:1px solid #bfd3f9;"
      "border-radius:8px;text-align:left;padding-left:14px;font-weight:600;}"
      "QPushButton:hover{background:#e5eeff;border-color:#2f6feb;}"
      "QPushButton:disabled{background:#f6f8fb;color:#98a2b3;"
      "border-color:#d7dee8;}"));
  connect(create, &QPushButton::clicked, this, [this] {
    if (actions.newThread)
      actions.newThread();
  });
  layout->addWidget(create);
  layout->addSpacing(8);

  auto *toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(4, 0, 4, 6);
  auto *refresh = new QPushButton(QStringLiteral("Refresh"));
  refresh->setProperty("kind", "subtle");
  refresh->setFixedHeight(28);
  connect(refresh, &QPushButton::clicked, this, [this] {
    if (actions.refresh)
      actions.refresh();
  });
  toolbar->addWidget(refresh);
  toolbar->addStretch();
  sortButton = new UiStyle::ChevronToolButton;
  sortButton->setObjectName(QStringLiteral("threadSortButton"));
  sortButton->setProperty("kind", "subtle");
  sortButton->setProperty("codexChevron", true);
  sortButton->setPopupMode(QToolButton::InstantPopup);
  sortButton->setFixedHeight(28);
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
  addSortAction(QStringLiteral("Alphanumeric"), SortCriterion::Alphanumeric);
  addSortAction(QStringLiteral("Created"), SortCriterion::Created);
  addSortAction(QStringLiteral("Last changed"), SortCriterion::LastChanged);
  QAction *recent =
      addSortAction(QStringLiteral("Recent"), SortCriterion::Recency);
  recent->setChecked(true);
  sortButton->setMenu(sortMenu);
  sortButton->setToolTip(QStringLiteral("Sort threads"));
  updateSortButton();
  toolbar->addWidget(sortButton);
  layout->addLayout(toolbar);

  list = new ThreadListWidget;
  auto *threadList = static_cast<ThreadListWidget *>(list);
  threadList->toggleExpansion = [this](const std::string &id) {
    toggleExpanded(id);
  };
  threadList->navigateHierarchy = [this](int key) { navigateHierarchy(key); };
  list->setObjectName(QStringLiteral("threadList"));
  list->setItemDelegate(new ThreadItemDelegate(list));
  optimisticAnimation = new QTimer(list);
  optimisticAnimation->setObjectName(
      QStringLiteral("optimisticThreadAnimation"));
  optimisticAnimation->setInterval(32);
  connect(optimisticAnimation, &QTimer::timeout, list, [this] {
    if (std::ranges::any_of(
            optimisticThreads,
            [](const OptimisticThread &thread) { return !thread.failed; }))
      list->viewport()->update();
  });
  list->setSelectionMode(QAbstractItemView::SingleSelection);
  list->setContextMenuPolicy(Qt::CustomContextMenu);
  list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list->setTextElideMode(Qt::ElideRight);
  list->setStyleSheet(QStringLiteral(
      "QListWidget#threadList{background:transparent;border:0;outline:0;}"
      "QListWidget#threadList::item{min-height:30px;background:#ffffff;"
      "border:1px solid #d7dee8;border-radius:8px;margin:3px 0;"
      "padding:2px 8px;color:#344054;}"
      "QListWidget#threadList::item:hover{background:#f1f5fb;"
      "border-color:#b9c4d2;}"
      "QListWidget#threadList::item:selected{background:#e5eeff;"
      "border-color:#bfd3f9;color:#1d2633;font-weight:600;}"));
  connect(list, &QListWidget::itemSelectionChanged, this, [this] {
    if (actions.select) {
      const std::string id = visiblySelectedThreadId();
      if (!id.empty())
        actions.select(id);
    }
  });
  connect(list, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint &position) { showContextMenu(position); });
  layout->addWidget(list);
}

void ThreadPane::setActions(Actions next) { actions = std::move(next); }

void ThreadPane::beginOptimisticThread(std::string id, std::string title,
                                       std::string cwd) {
  std::erase_if(optimisticThreads, [&id](const OptimisticThread &thread) {
    return thread.id == id;
  });
  optimisticThreads.insert(
      optimisticThreads.begin(),
      OptimisticThread{std::move(id), std::move(title), std::move(cwd), false});
  optimisticAnimation->start();
  visibleSnapshot.reset();
}

void ThreadPane::promoteOptimisticThread(const std::string &draftId,
                                         const std::string &authoritativeId) {
  const auto optimistic =
      std::ranges::find(optimisticThreads, draftId, &OptimisticThread::id);
  if (optimistic == optimisticThreads.end() || authoritativeId.empty() ||
      draftId == authoritativeId)
    return;
  if (auto node = rows.extract(draftId); !node.empty()) {
    node.key() = authoritativeId;
    node.mapped()->setData(Qt::UserRole, text(authoritativeId));
    rows.insert(std::move(node));
  }
  optimistic->id = authoritativeId;
  visibleSnapshot.reset();
}

void ThreadPane::confirmOptimisticThread(const std::string &threadId) {
  if (!isOptimisticThread(threadId))
    return;
  std::erase_if(optimisticThreads, [&threadId](const OptimisticThread &thread) {
    return thread.id == threadId;
  });
  if (std::ranges::none_of(
          optimisticThreads,
          [](const OptimisticThread &thread) { return !thread.failed; }))
    optimisticAnimation->stop();
  visibleSnapshot.reset();
}

void ThreadPane::failOptimisticThread(const std::string &threadId) {
  const auto optimistic =
      std::ranges::find(optimisticThreads, threadId, &OptimisticThread::id);
  if (optimistic == optimisticThreads.end())
    return;
  optimistic->failed = true;
  if (std::ranges::none_of(
          optimisticThreads,
          [](const OptimisticThread &thread) { return !thread.failed; }))
    optimisticAnimation->stop();
  visibleSnapshot.reset();
}

bool ThreadPane::isOptimisticThread(const std::string &threadId) const {
  return std::ranges::any_of(optimisticThreads,
                             [&threadId](const OptimisticThread &thread) {
                               return thread.id == threadId;
                             });
}

void ThreadPane::setSortCriterion(SortCriterion criterion) {
  if (sortCriterion == criterion)
    return;
  sortCriterion = criterion;
  updateSortButton();
  visibleSnapshot.reset();
  if (currentSnapshot)
    refresh(*currentSnapshot);
}

ThreadPane::SortCriterion ThreadPane::currentSortCriterion() const noexcept {
  return sortCriterion;
}

void ThreadPane::updateSortButton() {
  if (!sortButton)
    return;
  QString label;
  switch (sortCriterion) {
  case SortCriterion::Alphanumeric:
    label = QStringLiteral("A–Z");
    break;
  case SortCriterion::Created:
    label = QStringLiteral("Created");
    break;
  case SortCriterion::LastChanged:
    label = QStringLiteral("Changed");
    break;
  case SortCriterion::Recency:
    label = QStringLiteral("Recent");
    break;
  }
  sortButton->setText(QStringLiteral("Sort: %1").arg(label));
  for (QAction *action : sortButton->menu()->actions())
    action->setChecked(action->text() ==
                       (sortCriterion == SortCriterion::Alphanumeric
                            ? QStringLiteral("Alphanumeric")
                        : sortCriterion == SortCriterion::Created
                            ? QStringLiteral("Created")
                        : sortCriterion == SortCriterion::LastChanged
                            ? QStringLiteral("Last changed")
                            : QStringLiteral("Recent")));
}

void ThreadPane::sortRootThreads(std::vector<ui::ThreadListRow> &rows) const {
  QCollator collator(QLocale::system().language() == QLocale::C
                         ? QLocale(QLocale::English)
                         : QLocale::system());
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setIgnorePunctuation(true);
  collator.setNumericMode(true);
  std::sort(rows.begin(), rows.end(),
            [&](const ui::ThreadListRow &left,
                const ui::ThreadListRow &right) {
              if (sortCriterion == SortCriterion::Alphanumeric) {
                const QString leftTitle = text(left.title).trimmed();
                const QString rightTitle = text(right.title).trimmed();
                const bool leftStartsWithNumber =
                    !leftTitle.isEmpty() && leftTitle.front().isDigit();
                const bool rightStartsWithNumber =
                    !rightTitle.isEmpty() && rightTitle.front().isDigit();
                if (leftStartsWithNumber != rightStartsWithNumber)
                  return leftStartsWithNumber;
                const int comparison = collator.compare(leftTitle, rightTitle);
                if (comparison != 0)
                  return comparison < 0;
              } else {
                const auto leftTimestamp = timestampFor(left, sortCriterion);
                const auto rightTimestamp = timestampFor(right, sortCriterion);
                if (leftTimestamp != rightTimestamp) {
                  if (!leftTimestamp)
                    return false;
                  if (!rightTimestamp)
                    return true;
                  return *leftTimestamp > *rightTimestamp;
                }
              }
              return left.id < right.id;
            });
}

void ThreadPane::appendVisibleThread(
    RenderedThreadList &snapshot, const ui::ThreadListRow &thread,
    const std::string &parentId, std::size_t depth,
    std::unordered_set<std::string> &visited) const {
  if (!visited.insert(thread.id).second)
    return;
  const bool hasChildren = !thread.children.empty();
  const bool expanded = hasChildren && expandedThreads.contains(thread.id);
  snapshot.rows.push_back(
      {thread.id, thread.title, thread.cwd, thread.status,
       thread.lastActivityAt, parentId,
       thread.pending, depth, hasChildren, expanded});
  if (!expanded)
    return;
  for (const ui::ThreadListRow &child : thread.children)
    appendVisibleThread(snapshot, child, thread.id, depth + 1, visited);
}

void ThreadPane::toggleExpanded(const std::string &threadId) {
  if (expandedThreads.contains(threadId))
    expandedThreads.erase(threadId);
  else
    expandedThreads.insert(threadId);
  visibleSnapshot.reset();
  if (currentSnapshot)
    refresh(*currentSnapshot);
}

void ThreadPane::navigateHierarchy(int key) {
  QListWidgetItem *current = list->currentItem();
  if (!current)
    return;
  const std::string id = current->data(Qt::UserRole).toString().toStdString();
  const bool hasChildren = current->data(HasChildrenRole).toBool();
  const bool expanded = current->data(ExpandedRole).toBool();
  if (key == Qt::Key_Right && hasChildren) {
    if (!expanded) {
      toggleExpanded(id);
      return;
    }
    const int nextRow = list->row(current) + 1;
    if (nextRow < list->count() &&
        list->item(nextRow)->data(ParentIdRole).toString().toStdString() == id)
      list->setCurrentRow(nextRow);
    return;
  }
  if (key != Qt::Key_Left)
    return;
  if (hasChildren && expanded) {
    toggleExpanded(id);
    return;
  }
  const QString parentId = current->data(ParentIdRole).toString();
  if (parentId.isEmpty())
    return;
  const auto parent = rows.find(parentId.toStdString());
  if (parent != rows.end())
    list->setCurrentItem(parent->second);
}

void ThreadPane::setContextHighlight(const std::string &threadId,
                                     bool highlighted) {
  const auto found = rows.find(threadId);
  if (found == rows.end())
    return;
  found->second->setData(ContextMenuRole, highlighted);
}

void ThreadPane::refresh(const ui::ThreadListSnapshot &input) {
  currentSnapshot = input;
  const ui::ThreadListSnapshot &view = *currentSnapshot;
  const std::string &selectedThreadId = view.selectedThreadId;
  const bool selectionChanged = selectedThreadId != projectedSelectedThreadId;
  projectedSelectedThreadId = selectedThreadId;
  if (selectionChanged) {
    for (const ui::ThreadListRow &root : view.roots)
      if (expandAncestors(root, selectedThreadId, expandedThreads))
        break;
  }
  std::erase_if(expandedThreads, [&view](const std::string &id) {
    const ui::ThreadListRow *thread = findThread(view.roots, id);
    return !thread || thread->children.empty();
  });
  std::vector<ui::ThreadListRow> rootRows = view.roots;
  sortRootThreads(rootRows);

  RenderedThreadList next{selectedThreadId, sortCriterion, {}};
  std::unordered_set<std::string> visited;
  visited.reserve(rootRows.size());
  for (const OptimisticThread &optimisticThread : optimisticThreads) {
    if (const ui::ThreadListRow *thread =
            findThread(view.roots, optimisticThread.id)) {
      next.rows.push_back({thread->id,
                           thread->title,
                           thread->cwd,
                           thread->status,
                           thread->lastActivityAt,
                           {},
                           0,
                           0,
                           false,
                           false,
                           true,
                           optimisticThread.failed});
    } else {
      next.rows.push_back({optimisticThread.id,
                           optimisticThread.title,
                           optimisticThread.cwd,
                           {},
                           {},
                           {},
                           0,
                           0,
                           false,
                           false,
                           true,
                           optimisticThread.failed});
    }
    visited.insert(optimisticThread.id);
  }
  for (const ui::ThreadListRow &row : rootRows)
    appendVisibleThread(next, row, {}, 0, visited);
  if (visibleSnapshot && *visibleSnapshot == next)
    return;
  visibleSnapshot = std::move(next);
  const RenderedThreadList &snapshot = *visibleSnapshot;
  list->blockSignals(true);
  list->setUpdatesEnabled(false);
  // Selection is a projection of selectedThreadId, never retained widget
  // state. This also makes an explicit New Thread draft visibly select no
  // existing row.
  list->clearSelection();
  list->setCurrentRow(-1);

  std::unordered_set<std::string> wanted;
  wanted.reserve(snapshot.rows.size());
  for (const RenderedThreadRow &row : snapshot.rows)
    wanted.insert(row.id);
  for (int index = list->count() - 1; index >= 0; --index) {
    QListWidgetItem *item = list->item(index);
    const std::string id = item->data(Qt::UserRole).toString().toStdString();
    if (wanted.contains(id))
      continue;
    rows.erase(id);
    delete list->takeItem(index);
  }

  std::unordered_map<std::string, int> existingPositions;
  existingPositions.reserve(rows.size());
  int existingIndex = 0;
  for (const RenderedThreadRow &row : snapshot.rows) {
    if (rows.contains(row.id))
      existingPositions.emplace(row.id, existingIndex++);
  }

  std::unordered_set<std::string> moved;
  moved.reserve(rows.size());
  for (int index = list->count() - 1; index >= 0; --index) {
    QListWidgetItem *item = list->item(index);
    const std::string id = item->data(Qt::UserRole).toString().toStdString();
    if (existingPositions.at(id) == index)
      continue;
    // Removing an index widget transfers it into Qt's deferred-deletion path.
    // A changed row receives a fresh widget when its item is reinserted.
    list->removeItemWidget(item);
    list->takeItem(index);
    moved.insert(id);
  }
  int wantedIndex = 0;
  for (const RenderedThreadRow &row : snapshot.rows) {
    auto found = rows.find(row.id);
    if (found == rows.end()) {
      auto *item = new QListWidgetItem;
      item->setSizeHint(QSize(0, 40));
      item->setData(Qt::UserRole, text(row.id));
      list->insertItem(wantedIndex, item);
      list->setItemWidget(item, createRow());
      found = rows.emplace(row.id, item).first;
    } else if (moved.contains(row.id)) {
      list->insertItem(wantedIndex, found->second);
      list->setItemWidget(found->second, createRow());
    }
    QListWidgetItem *item = found->second;
    const QString title = text(row.title);
    const QString status = text(displayStatus(row.status));
    QStringList accessibleParts{title, status,
                                QStringLiteral("level %1").arg(row.depth + 1)};
    if (row.hasChildren)
      accessibleParts.push_back(row.expanded ? QStringLiteral("expanded")
                                             : QStringLiteral("collapsed"));
    item->setData(Qt::DisplayRole, {});
    item->setData(Qt::AccessibleTextRole, accessibleParts.join(", "));
    QStringList details{title,
                        QStringLiteral("Workspace: %1").arg(
                            row.cwd.empty() ? QStringLiteral("Unknown")
                                            : text(row.cwd)),
                        QStringLiteral("Status: %1").arg(status),
                        QStringLiteral("Last activity: %1").arg(
                            activityText(row.lastActivityAt))};
    if (!row.parentId.empty()) {
      const ui::ThreadListRow *parent =
          findThread(currentSnapshot->roots, row.parentId);
      details.push_back(QStringLiteral("Parent: %1").arg(
          parent && !parent->title.empty() ? text(parent->title)
                                           : text(row.parentId)));
    }
    item->setToolTip(details.join(QLatin1Char('\n')));
    item->setData(DepthRole, static_cast<qulonglong>(row.depth));
    item->setData(HasChildrenRole, row.hasChildren);
    item->setData(ExpandedRole, row.expanded);
    item->setData(ParentIdRole, text(row.parentId));
    item->setData(OptimisticRole, row.optimistic);
    item->setData(OptimisticFailedRole, row.optimisticFailed);
    updateRow(list->itemWidget(item), row.id, row.title, row.status,
              row.pending, row.depth, row.hasChildren, row.expanded,
              row.optimistic, row.optimisticFailed);
    if (row.id == contextThreadId)
      setContextHighlight(row.id, true);
    if (row.id == snapshot.selectedThreadId)
      list->setCurrentItem(item);
    ++wantedIndex;
  }
  list->setUpdatesEnabled(true);
  list->blockSignals(false);
}

std::string ThreadPane::visiblySelectedThreadId() const {
  const QList<QListWidgetItem *> selected = list->selectedItems();
  return selected.size() == 1 && selected.front()
             ? selected.front()->data(Qt::UserRole).toString().toStdString()
             : std::string{};
}

void ThreadPane::showContextMenu(const QPoint &position) {
  QListWidgetItem *item = list->itemAt(position);
  if (!item || !currentSnapshot)
    return;
  const std::string id = item->data(Qt::UserRole).toString().toStdString();
  const ui::ThreadListRow *thread = findThread(currentSnapshot->roots, id);
  if (!thread)
    return;
  if (contextMenu)
    contextMenu->close();
  contextThreadId = id;
  setContextHighlight(contextThreadId, true);
  auto *menu = new QMenu(list);
  contextMenu = menu;
  connect(menu, &QMenu::aboutToHide, this, [this, menu] {
    if (contextMenu == menu) {
      setContextHighlight(contextThreadId, false);
      contextThreadId.clear();
      contextMenu = nullptr;
    }
    menu->deleteLater();
  });
  QAction *reload = menu->addAction(QStringLiteral("Reload"), this, [this, id] {
    if (actions.reload)
      actions.reload(id);
  });
  const bool providerReady = currentSnapshot->providerReady;
  const bool canControl = currentSnapshot->canControl;
  QAction *rename = menu->addAction(QStringLiteral("Rename"), this, [this, id] {
    if (actions.rename)
      actions.rename(id);
  });
  QAction *fork = menu->addAction(QStringLiteral("Fork"), this, [this, id] {
    if (actions.fork)
      actions.fork(id);
  });
  QAction *archive =
      menu->addAction(thread->archived ? QStringLiteral("Unarchive")
                                       : QStringLiteral("Archive"),
                      this, [this, id] {
                        if (actions.toggleArchive)
                          actions.toggleArchive(id);
                      });
  menu->addSeparator();
  QAction *remove = menu->addAction(QStringLiteral("Delete"), this, [this, id] {
    if (actions.remove)
      actions.remove(id);
  });
  reload->setEnabled(providerReady);
  rename->setEnabled(canControl);
  fork->setEnabled(canControl);
  archive->setEnabled(canControl);
  remove->setEnabled(canControl);
  menu->popup(list->viewport()->mapToGlobal(position));
}

} // namespace codexui::codex::middle
