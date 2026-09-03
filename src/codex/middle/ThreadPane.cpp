// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"

#include "codex/PresentationStatus.h"
#include "codex/ui/QtNodeAttachment.h"
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
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
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
  std::function<void()> viewportChanged;

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

  void resizeEvent(QResizeEvent *event) override {
    QListWidget::resizeEvent(event);
    if (viewportChanged)
      viewportChanged();
  }

  void showEvent(QShowEvent *event) override {
    QListWidget::showEvent(event);
    if (viewportChanged)
      viewportChanged();
  }

  void scrollContentsBy(int dx, int dy) override {
    QListWidget::scrollContentsBy(dx, dy);
    if (viewportChanged)
      viewportChanged();
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

const nodegraph::Value *graphField(const nodegraph::NodeState &state,
                                   std::string_view name) {
  const auto found = state.fields.find(name);
  return found == state.fields.end() ? nullptr : &found->second;
}

std::string graphString(const nodegraph::Value *value) {
  if (!value)
    return {};
  if (const std::string *string = value->asString())
    return *string;
  if (const auto *object = value->asObject()) {
    const auto type = object->find("type");
    if (type != object->end())
      return graphString(&type->second);
  }
  return {};
}

std::optional<std::int64_t> graphTimestamp(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const std::int64_t *number = value->asInt64())
    return *number;
  if (const std::uint64_t *number = value->asUInt64()) {
    if (*number <=
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return static_cast<std::int64_t>(*number);
  }
  if (const double *number = value->asDouble())
    return static_cast<std::int64_t>(*number);
  return std::nullopt;
}

bool graphBool(const nodegraph::Value *value) {
  const bool *boolean = value ? value->asBool() : nullptr;
  return boolean && *boolean;
}

std::string graphStatus(const nodegraph::NodeState &state) {
  if (std::string status = graphString(graphField(state, "status"));
      !status.empty())
    return status;
  switch (state.status) {
  case nodegraph::NodeStatus::Pending:
    return "pending";
  case nodegraph::NodeStatus::Running:
    return "running";
  case nodegraph::NodeStatus::Completed:
    return "completed";
  case nodegraph::NodeStatus::Failed:
    return "failed";
  case nodegraph::NodeStatus::Interrupted:
    return "interrupted";
  case nodegraph::NodeStatus::NotLoaded:
    return "notLoaded";
  case nodegraph::NodeStatus::Connected:
    return "connected";
  case nodegraph::NodeStatus::Disconnected:
    return "disconnected";
  case nodegraph::NodeStatus::Unknown:
    return {};
  }
  return {};
}

} // namespace

struct ThreadPane::GraphThreadItem final : public QListWidgetItem {
  ~GraphThreadItem() override { detachNode(); }

  void setNode(nodegraph::NodeRef next) {
    if (node == next)
      return;
    detachNode();
    node = std::move(next);
    if (!node)
      return;
    attachment = std::make_unique<ui::QtNodeAttachment>();
    void *attached = node->uiAttachment();
    Q_ASSERT(attached == nullptr);
    if (!attached)
      node->setUiAttachment(attachment.get());
    else
      attachment.reset();
  }

  void detachNode() {
    if (node && attachment && node->uiAttachment() == attachment.get())
      node->setUiAttachment(nullptr);
    attachment.reset();
    node.reset();
    parent.reset();
  }

  nodegraph::NodeRef node;
  nodegraph::NodeRef parent;
  std::unique_ptr<ui::QtNodeAttachment> attachment;
  std::string localId;
  std::string localTitle;
  std::string localCwd;
  std::size_t depth = 0;
  std::uint64_t renderedRevision = 0;
  std::size_t renderedPending = 0;
  bool hasChildren = false;
  bool expanded = false;
  bool optimistic = false;
  bool optimisticFailed = false;
};

struct ThreadPane::GraphTopology final {
  struct Row final {
    nodegraph::NodeRef node;
    nodegraph::NodeRef parent;
    std::string localId;
    std::string localTitle;
    std::string localCwd;
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = false;
    bool optimistic = false;
    bool optimisticFailed = false;
    std::string previousLocalId;
  };

  std::vector<Row> rows;
  std::string selectedId;
};

struct ThreadPane::GraphRowRender final {
  std::uint64_t revision = 0;
  std::string id;
  std::string title;
  std::string cwd;
  std::string status;
  std::optional<std::int64_t> lastActivityAt;
  std::string parentTitle;
  std::size_t pending = 0;
};

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
    if (controls.hide)
      controls.hide();
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
    if (controls.newThread)
      controls.newThread();
  });
  layout->addWidget(create);
  layout->addSpacing(8);

  auto *toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(4, 0, 4, 6);
  auto *refresh = new QPushButton(QStringLiteral("Refresh"));
  refresh->setProperty("kind", "subtle");
  refresh->setFixedHeight(28);
  connect(refresh, &QPushButton::clicked, this, [this] {
    if (controls.refresh)
      controls.refresh();
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
  threadList->viewportChanged = [this] {
    if (graph)
      scheduleVisibilityPass();
  };
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
    const std::string id = visiblySelectedThreadId();
    if (id.empty())
      return;
    if (nodegraph::NodeRef node = visiblySelectedThread();
        node && nodeActions.select)
      nodeActions.select(node);
  });
  connect(list, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint &position) { showContextMenu(position); });
  layout->addWidget(list);
}

ThreadPane::~ThreadPane() { leaveGraph(); }

void ThreadPane::setControls(Controls next) { controls = std::move(next); }

void ThreadPane::setNodeActions(NodeActions next) {
  nodeActions = std::move(next);
}

void ThreadPane::beginOptimisticThread(std::string id, std::string title,
                                       std::string cwd) {
  std::erase_if(optimisticThreads, [&id](const OptimisticThread &thread) {
    return thread.id == id;
  });
  optimisticThreads.insert(
      optimisticThreads.begin(),
      OptimisticThread{std::move(id), std::move(title), std::move(cwd), false});
  selectedOptimisticThreadId = optimisticThreads.front().id;
  optimisticAnimation->start();
  if (graph)
    scheduleGraphRefresh();
}

void ThreadPane::promoteOptimisticThread(const std::string &draftId,
                                         const std::string &authoritativeId) {
  const auto optimistic =
      std::ranges::find(optimisticThreads, draftId, &OptimisticThread::id);
  if (optimistic == optimisticThreads.end() || authoritativeId.empty() ||
      draftId == authoritativeId)
    return;
  optimistic->previousId = draftId;
  optimistic->id = authoritativeId;
  if (selectedOptimisticThreadId == draftId)
    selectedOptimisticThreadId = authoritativeId;
  if (graph)
    scheduleGraphRefresh();
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
  if (graph)
    scheduleGraphRefresh();
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
  if (graph)
    scheduleGraphRefresh();
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
  if (graph)
    scheduleGraphRefresh();
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

void ThreadPane::toggleExpanded(const std::string &threadId) {
  GraphThreadItem *binding = nullptr;
  for (int index = 0; index < list->count(); ++index) {
    auto *candidate = graphItem(list->item(index));
    if (candidate && candidate->node &&
        candidate->node->id().canonical == threadId) {
      binding = candidate;
      break;
    }
  }
  if (!binding)
    return;
  const nodegraph::Node *node = binding->node.get();
  if (graphExpandedThreads.contains(node))
    graphExpandedThreads.erase(node);
  else
    graphExpandedThreads.insert(node);
  scheduleGraphRefresh();
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
  for (int index = 0; index < list->count(); ++index) {
    QListWidgetItem *candidate = list->item(index);
    if (candidate->data(Qt::UserRole).toString() == parentId) {
      list->setCurrentItem(candidate);
      break;
    }
  }
}

void ThreadPane::setContextHighlight(const std::string &threadId,
                                     bool highlighted) {
  for (int index = 0; index < list->count(); ++index) {
    QListWidgetItem *candidate = list->item(index);
    if (candidate->data(Qt::UserRole).toString().toStdString() == threadId) {
      candidate->setData(ContextMenuRole, highlighted);
      break;
    }
  }
}

void ThreadPane::refresh(const nodegraph::NodeGraph &nextGraph,
                         nodegraph::NodeRef selectedThread) {
  if (graph != &nextGraph) {
    leaveGraph();
    list->clear();
    graph = &nextGraph;
  }
  if (selectedGraphThread != selectedThread)
    revealSelectedGraphThread = true;
  selectedGraphThread = std::move(selectedThread);
  if (selectedGraphThread)
    selectedOptimisticThreadId.clear();
  scheduleGraphRefresh();
}

void ThreadPane::graphChanged() {
  if (graph)
    scheduleGraphRefresh();
}

void ThreadPane::graphChanged(const nodegraph::GraphChanged &change) {
  if (!graph)
    return;
  detachRemoved(change);
  // All removal work above is complete before the FrontendSession handler can
  // enqueue UiDetached. A reverse-interaction transaction only changes the
  // pending relation/revision of its addressed thread, so visible badges can
  // update without rebuilding the thread topology.
  const auto hasKind = [&change](nodegraph::NodeKind kind,
                                 bool removedOnly = false) {
    const auto matches = [kind](const nodegraph::NodeRef &node) {
      return node && node->id().kind == kind;
    };
    return (!removedOnly && std::ranges::any_of(change.affected, matches)) ||
           std::ranges::any_of(change.removed, matches);
  };
  const bool interactionChanged = hasKind(nodegraph::NodeKind::Interaction);
  const bool topologyChanged =
      change.rescanRequired || hasKind(nodegraph::NodeKind::Runtime, true) ||
      hasKind(nodegraph::NodeKind::Thread, true) ||
      (!interactionChanged && (hasKind(nodegraph::NodeKind::Runtime) ||
                               hasKind(nodegraph::NodeKind::Thread)));
  if (topologyChanged)
    scheduleGraphRefresh();
  else if (interactionChanged)
    scheduleVisibilityPass();
}

void ThreadPane::detachRemoved(const nodegraph::GraphChanged &change) {
  if (change.removed.empty())
    return;
  std::unordered_set<const nodegraph::Node *> removed;
  removed.reserve(change.removed.size());
  for (const nodegraph::NodeRef &node : change.removed)
    removed.insert(node.get());

  const bool signalsWereBlocked = list->blockSignals(true);
  const bool updatesWereEnabled = list->updatesEnabled();
  list->setUpdatesEnabled(false);
  for (int index = list->count() - 1; index >= 0; --index) {
    GraphThreadItem *item = graphItem(list->item(index));
    if (!item || !item->node || !removed.contains(item->node.get()))
      continue;
    graphExpandedThreads.erase(item->node.get());
    if (selectedGraphThread == item->node) {
      selectedGraphThread.reset();
      revealSelectedGraphThread = false;
    }
    if (contextThreadId == item->localId && contextMenu)
      contextMenu->close();
    dematerialize(*item, false);
    static_cast<void>(list->takeItem(index));
    delete item;
  }
  list->setUpdatesEnabled(updatesWereEnabled);
  list->blockSignals(signalsWereBlocked);
}

void ThreadPane::leaveGraph() {
  graphRefreshScheduled = false;
  visibilityPassScheduled = false;
  visibilityFirst = -1;
  visibilityLast = -1;
  visibilityCursor = -1;
  if (!graph)
    return;

  list->blockSignals(true);
  list->clear();
  graphExpandedThreads.clear();
  selectedGraphThread.reset();
  selectedOptimisticThreadId.clear();
  revealSelectedGraphThread = false;
  graph = nullptr;
  list->blockSignals(false);
}

void ThreadPane::scheduleGraphRefresh() {
  if (!graph || graphRefreshScheduled)
    return;
  graphRefreshScheduled = true;
  QTimer::singleShot(0, this, [this] {
    graphRefreshScheduled = false;
    runGraphRefresh();
  });
}

void ThreadPane::runGraphRefresh() {
  if (!graph)
    return;

  struct ReadThread final {
    nodegraph::NodeRef node;
    nodegraph::NodeRef parent;
    std::vector<nodegraph::NodeRef> children;
    std::string title;
    std::optional<std::int64_t> createdAt;
    std::optional<std::int64_t> updatedAt;
    std::optional<std::int64_t> recencyAt;
  };

  auto read = graph->tryRead();
  if (!read) {
    scheduleGraphRefresh();
    return;
  }

  std::vector<ReadThread> threads;
  std::unordered_map<const nodegraph::Node *, std::size_t> positions;
  std::vector<nodegraph::NodeRef> roots;
  if (nodegraph::NodeRef runtime =
          read->find({nodegraph::NodeKind::Runtime, "runtime"})) {
    roots = read->related(runtime, nodegraph::RelationKind::RootThread);
  }

  const auto gather = [&](const auto &self, const nodegraph::NodeRef &node,
                          nodegraph::NodeRef parent) -> void {
    if (!node || node->id().kind != nodegraph::NodeKind::Thread ||
        read->removed(node) || positions.contains(node.get()))
      return;
    const std::size_t position = threads.size();
    positions.emplace(node.get(), position);
    threads.push_back(ReadThread{node, std::move(parent)});

    std::vector<nodegraph::NodeRef> children =
        read->related(node, nodegraph::RelationKind::StructuralChildThread);
    for (nodegraph::NodeRef child :
         read->related(node, nodegraph::RelationKind::AgentChildThread)) {
      if (std::find(children.begin(), children.end(), child) == children.end())
        children.emplace_back(std::move(child));
    }
    threads[position].children = children;
    for (const nodegraph::NodeRef &child : children)
      self(self, child, node);
  };
  for (const nodegraph::NodeRef &root : roots)
    gather(gather, root, {});

  for (ReadThread &thread : threads) {
    if (std::find(roots.begin(), roots.end(), thread.node) == roots.end())
      continue;
    const std::shared_ptr<const nodegraph::NodeState> state =
        read->state(thread.node);
    thread.title = graphString(graphField(*state, "name"));
    if (thread.title.empty())
      thread.title = graphString(graphField(*state, "title"));
    thread.createdAt = graphTimestamp(graphField(*state, "createdAt"));
    thread.updatedAt = graphTimestamp(graphField(*state, "updatedAt"));
    thread.recencyAt = graphTimestamp(graphField(*state, "recencyAt"));
  }
  read.reset();

  std::erase_if(graphExpandedThreads,
                [&positions](const nodegraph::Node *node) {
                  return !positions.contains(node);
                });
  const bool selectedIsPresent =
      selectedGraphThread && positions.contains(selectedGraphThread.get());
  if (selectedIsPresent && revealSelectedGraphThread) {
    auto selected = positions.find(selectedGraphThread.get());
    while (selected != positions.end()) {
      const nodegraph::NodeRef &parent = threads[selected->second].parent;
      if (!parent)
        break;
      graphExpandedThreads.insert(parent.get());
      selected = positions.find(parent.get());
    }
    revealSelectedGraphThread = false;
  }

  const auto timestamp = [this](const ReadThread &thread) {
    if (sortCriterion == SortCriterion::Created)
      return thread.createdAt;
    if (sortCriterion == SortCriterion::LastChanged)
      return thread.updatedAt;
    return thread.recencyAt;
  };
  QCollator collator(QLocale::system().language() == QLocale::C
                         ? QLocale(QLocale::English)
                         : QLocale::system());
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setIgnorePunctuation(true);
  collator.setNumericMode(true);
  std::sort(
      roots.begin(), roots.end(),
      [&](const nodegraph::NodeRef &left, const nodegraph::NodeRef &right) {
        const auto leftPosition = positions.find(left.get());
        const auto rightPosition = positions.find(right.get());
        if (leftPosition == positions.end() || rightPosition == positions.end())
          return left->id().canonical < right->id().canonical;
        const ReadThread &leftThread = threads[leftPosition->second];
        const ReadThread &rightThread = threads[rightPosition->second];
        if (sortCriterion == SortCriterion::Alphanumeric) {
          const QString leftTitle = text(leftThread.title).trimmed();
          const QString rightTitle = text(rightThread.title).trimmed();
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
          const auto leftTimestamp = timestamp(leftThread);
          const auto rightTimestamp = timestamp(rightThread);
          if (leftTimestamp != rightTimestamp) {
            if (!leftTimestamp)
              return false;
            if (!rightTimestamp)
              return true;
            return *leftTimestamp > *rightTimestamp;
          }
        }
        return left->id().canonical < right->id().canonical;
      });

  GraphTopology topology;
  if (selectedIsPresent)
    topology.selectedId = selectedGraphThread->id().canonical;
  else if (std::ranges::any_of(
               optimisticThreads, [this](const OptimisticThread &optimistic) {
                 return optimistic.id == selectedOptimisticThreadId;
               }))
    topology.selectedId = selectedOptimisticThreadId;
  std::unordered_set<const nodegraph::Node *> appended;
  const auto append = [&](const auto &self, const nodegraph::NodeRef &node,
                          nodegraph::NodeRef parent,
                          std::size_t depth) -> void {
    const auto position = positions.find(node.get());
    if (position == positions.end() || !appended.insert(node.get()).second)
      return;
    const ReadThread &thread = threads[position->second];
    const bool hasChildren = !thread.children.empty();
    const bool expanded =
        hasChildren && graphExpandedThreads.contains(node.get());
    topology.rows.push_back({node,
                             std::move(parent),
                             node->id().canonical,
                             {},
                             {},
                             depth,
                             hasChildren,
                             expanded});
    if (!expanded)
      return;
    for (const nodegraph::NodeRef &child : thread.children)
      self(self, child, node, depth + 1);
  };

  for (const OptimisticThread &optimistic : optimisticThreads) {
    const auto thread = std::ranges::find_if(
        threads, [&optimistic](const ReadThread &candidate) {
          return candidate.node->id().canonical == optimistic.id;
        });
    if (thread != threads.end()) {
      appended.insert(thread->node.get());
      topology.rows.push_back({thread->node,
                               {},
                               optimistic.id,
                               {},
                               {},
                               0,
                               false,
                               false,
                               true,
                               optimistic.failed,
                               optimistic.previousId});
    } else {
      topology.rows.push_back({{},
                               {},
                               optimistic.id,
                               optimistic.title,
                               optimistic.cwd,
                               0,
                               false,
                               false,
                               true,
                               optimistic.failed,
                               optimistic.previousId});
    }
  }
  for (const nodegraph::NodeRef &root : roots)
    append(append, root, {}, 0);

  applyGraphTopology(std::move(topology));
}

void ThreadPane::applyGraphTopology(GraphTopology topology) {
  const auto sameItem = [](const GraphThreadItem &item,
                           const GraphTopology::Row &row) {
    return item.node == row.node && item.parent == row.parent &&
           item.localId == row.localId && item.depth == row.depth &&
           item.hasChildren == row.hasChildren &&
           item.expanded == row.expanded && item.optimistic == row.optimistic;
  };
  bool structureChanged =
      list->count() != static_cast<int>(topology.rows.size());
  if (!structureChanged) {
    for (int index = 0; index < list->count(); ++index) {
      const GraphThreadItem *item = graphItem(list->item(index));
      if (!item ||
          !sameItem(*item, topology.rows[static_cast<std::size_t>(index)])) {
        structureChanged = true;
        break;
      }
    }
  }

  list->blockSignals(true);
  if (structureChanged) {
    list->setUpdatesEnabled(false);
    std::vector<GraphThreadItem *> old;
    old.reserve(static_cast<std::size_t>(list->count()));
    while (list->count() != 0) {
      auto *item = graphItem(list->item(0));
      Q_ASSERT(item);
      dematerialize(*item);
      static_cast<void>(list->takeItem(0));
      old.emplace_back(item);
    }

    for (GraphTopology::Row &row : topology.rows) {
      auto existing = std::ranges::find_if(old, [&row](GraphThreadItem *item) {
        return item &&
               ((row.node && item->node == row.node) ||
                (!row.node && !item->node && item->localId == row.localId));
      });
      if (existing == old.end() && row.node) {
        // Promotion from a local optimistic id to its authoritative NodeRef
        // keeps the QListWidgetItem stable.
        existing = std::ranges::find_if(old, [&row](GraphThreadItem *item) {
          return item && !item->node &&
                 (item->localId == row.localId ||
                  (!row.previousLocalId.empty() &&
                   item->localId == row.previousLocalId));
        });
      }

      GraphThreadItem *item = nullptr;
      if (existing != old.end()) {
        item = *existing;
        *existing = nullptr;
      } else {
        item = new GraphThreadItem;
        item->setSizeHint(QSize(0, 40));
      }
      item->setNode(std::move(row.node));
      item->parent = std::move(row.parent);
      item->localId = std::move(row.localId);
      item->localTitle = std::move(row.localTitle);
      item->localCwd = std::move(row.localCwd);
      item->depth = row.depth;
      item->hasChildren = row.hasChildren;
      item->expanded = row.expanded;
      item->optimistic = row.optimistic;
      item->optimisticFailed = row.optimisticFailed;
      item->renderedRevision = 0;

      item->setData(Qt::UserRole, text(item->localId));
      item->setData(Qt::DisplayRole, {});
      item->setData(DepthRole, static_cast<qulonglong>(item->depth));
      item->setData(HasChildrenRole, item->hasChildren);
      item->setData(ExpandedRole, item->expanded);
      item->setData(ParentIdRole, item->parent
                                      ? text(item->parent->id().canonical)
                                      : QString{});
      item->setData(OptimisticRole, item->optimistic);
      item->setData(OptimisticFailedRole, item->optimisticFailed);
      list->addItem(item);
    }
    for (GraphThreadItem *obsolete : old)
      delete obsolete;
    list->setUpdatesEnabled(true);
    visibilityFirst = -1;
    visibilityLast = -1;
    visibilityCursor = -1;
  } else {
    for (int index = 0; index < list->count(); ++index) {
      GraphThreadItem *item = graphItem(list->item(index));
      const GraphTopology::Row &row =
          topology.rows[static_cast<std::size_t>(index)];
      if (item->optimisticFailed != row.optimisticFailed) {
        item->optimisticFailed = row.optimisticFailed;
        item->renderedRevision = 0;
        if (item->attachment)
          item->attachment->renderedRevision = 0;
        item->setData(OptimisticFailedRole, item->optimisticFailed);
      }
    }
  }

  list->clearSelection();
  list->setCurrentRow(-1);
  if (!topology.selectedId.empty()) {
    for (int index = 0; index < list->count(); ++index) {
      auto *item = graphItem(list->item(index));
      if (item && item->localId == topology.selectedId) {
        list->setCurrentItem(item);
        break;
      }
    }
  }
  if (!contextThreadId.empty())
    setContextHighlight(contextThreadId, true);
  list->blockSignals(false);
  scheduleVisibilityPass();
}

void ThreadPane::scheduleVisibilityPass() {
  if (!graph || visibilityPassScheduled)
    return;
  visibilityPassScheduled = true;
  QTimer::singleShot(0, this, [this] {
    visibilityPassScheduled = false;
    runVisibilityPass();
  });
}

void ThreadPane::runVisibilityPass() {
  // Topology runs first after every graph notification. This prevents a stale
  // child item from reading a parent that was just removed and acknowledged.
  if (graphRefreshScheduled || !graph || list->count() == 0)
    return;

  constexpr int OverscanRows = 2;
  constexpr std::size_t MaxWidgetWork = 12;
  int firstVisible = -1;
  int lastVisible = -1;
  if (list->viewport()->isVisible() && list->count() != 0) {
    QModelIndex first = list->indexAt(QPoint(1, 1));
    if (!first.isValid())
      first = list->model()->index(0, 0);
    QModelIndex last =
        list->indexAt(QPoint(1, std::max(0, list->viewport()->height() - 1)));
    if (!last.isValid())
      last = list->model()->index(list->count() - 1, 0);
    firstVisible = first.row();
    lastVisible = last.row();
  }
  const int first =
      firstVisible < 0 ? -1 : std::max(0, firstVisible - OverscanRows);
  const int last =
      lastVisible < 0 ? -1
                      : std::min(list->count() - 1, lastVisible + OverscanRows);
  if (first != visibilityFirst || last != visibilityLast) {
    visibilityFirst = first;
    visibilityLast = last;
    visibilityCursor = first;
  }

  std::size_t work = 0;
  bool moreWork = false;
  for (int index = 0; index < list->count(); ++index) {
    GraphThreadItem *item = graphItem(list->item(index));
    Q_ASSERT(item);
    const bool desired = first >= 0 && index >= first && index <= last;
    if (item->attachment) {
      item->attachment->viewportVisible =
          desired && index >= firstVisible && index <= lastVisible;
      item->attachment->materialization =
          !desired ? ui::NodeMaterialization::Placeholder
          : item->attachment->viewportVisible
              ? ui::NodeMaterialization::ViewportVisible
              : ui::NodeMaterialization::Overscan;
    }
    if (desired || !list->itemWidget(item))
      continue;
    if (work == MaxWidgetWork) {
      moreWork = true;
      continue;
    }
    dematerialize(*item);
    ++work;
  }
  if (work == MaxWidgetWork && moreWork) {
    scheduleVisibilityPass();
    return;
  }
  if (first < 0)
    return;

  struct Candidate final {
    GraphThreadItem *item = nullptr;
    bool hasWidget = false;
  };
  std::vector<Candidate> candidates;
  const int start = visibilityCursor < first || visibilityCursor > last
                        ? first
                        : visibilityCursor;
  for (int index = start; index <= last; ++index) {
    GraphThreadItem *item = graphItem(list->item(index));
    candidates.push_back({item, list->itemWidget(item) != nullptr});
  }

  const bool needsGraphRead =
      std::ranges::any_of(candidates, [](const Candidate &candidate) {
        return candidate.item && candidate.item->node;
      });
  std::optional<nodegraph::NodeGraph::ReadAccess> read;
  if (needsGraphRead) {
    read = graph->tryRead();
    if (!read) {
      scheduleVisibilityPass();
      return;
    }
  }

  std::vector<std::pair<GraphThreadItem *, GraphRowRender>> renders;
  int nextCursor = last + 1;
  for (int offset = 0; offset < static_cast<int>(candidates.size()); ++offset) {
    Candidate &candidate = candidates[static_cast<std::size_t>(offset)];
    GraphThreadItem &item = *candidate.item;
    if (work == MaxWidgetWork) {
      nextCursor = start + offset;
      moreWork = true;
      break;
    }

    GraphRowRender render;
    if (!item.node) {
      const std::uint64_t revision = item.optimisticFailed ? 2 : 1;
      if (candidate.hasWidget && item.renderedRevision == revision)
        continue;
      render.revision = revision;
      render.id = item.localId;
      render.title = item.localTitle;
      render.cwd = item.localCwd;
    } else {
      if (!item.attachment)
        continue;
      std::uint64_t revision = read->changedRevision(item.node);
      if (item.parent)
        revision = std::max(revision, read->changedRevision(item.parent));
      std::size_t pending = 0;
      for (const nodegraph::NodeRef &interaction : read->related(
               item.node, nodegraph::RelationKind::PendingInteraction)) {
        if (!interaction ||
            interaction->id().kind != nodegraph::NodeKind::Interaction)
          continue;
        const std::shared_ptr<const nodegraph::NodeState> interactionState =
            read->state(interaction);
        if (interactionState->status != nodegraph::NodeStatus::Pending)
          continue;
        ++pending;
        revision = std::max(revision, read->changedRevision(interaction));
      }
      if (candidate.hasWidget &&
          item.attachment->renderedRevision == revision &&
          item.renderedPending == pending)
        continue;
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(item.node);
      render.revision = revision;
      render.id = item.node->id().canonical;
      render.title = graphString(graphField(*state, "name"));
      if (render.title.empty())
        render.title = graphString(graphField(*state, "title"));
      render.cwd = graphString(graphField(*state, "cwd"));
      render.status = graphStatus(*state);
      render.lastActivityAt =
          graphTimestamp(graphField(*state, "lastActivityAt"));
      for (const std::string_view field :
           {std::string_view("recencyAt"), std::string_view("updatedAt")}) {
        const auto timestamp = graphTimestamp(graphField(*state, field));
        if (timestamp &&
            (!render.lastActivityAt || *timestamp > *render.lastActivityAt))
          render.lastActivityAt = timestamp;
      }
      render.pending = pending;
      if (item.parent) {
        const std::shared_ptr<const nodegraph::NodeState> parentState =
            read->state(item.parent);
        render.parentTitle = graphString(graphField(*parentState, "name"));
        if (render.parentTitle.empty())
          render.parentTitle = graphString(graphField(*parentState, "title"));
      }
    }
    renders.emplace_back(&item, std::move(render));
    ++work;
  }
  read.reset();

  for (auto &[item, render] : renders)
    renderGraphRow(*item, render);
  visibilityCursor = nextCursor > last ? first : nextCursor;
  if (moreWork)
    scheduleVisibilityPass();
}

void ThreadPane::dematerialize(GraphThreadItem &item, bool deferred) {
  QWidget *widget = list->itemWidget(&item);
  if (widget) {
    list->removeItemWidget(&item);
    if (deferred)
      widget->deleteLater();
    else
      delete widget;
  }
  item.renderedRevision = 0;
  item.renderedPending = 0;
  if (item.attachment) {
    item.attachment->widget.clear();
    item.attachment->renderedRevision = 0;
    item.attachment->viewportVisible = false;
    item.attachment->materialization = ui::NodeMaterialization::Placeholder;
  }
}

void ThreadPane::renderGraphRow(GraphThreadItem &item,
                                const GraphRowRender &render) {
  QWidget *row = list->itemWidget(&item);
  if (!row) {
    row = createRow();
    list->setItemWidget(&item, row);
  }

  const QString title = text(render.title);
  const QString status = text(displayStatus(render.status));
  QStringList accessibleParts{title, status,
                              QStringLiteral("level %1").arg(item.depth + 1)};
  if (item.hasChildren)
    accessibleParts.push_back(item.expanded ? QStringLiteral("expanded")
                                            : QStringLiteral("collapsed"));
  item.setData(Qt::AccessibleTextRole,
               accessibleParts.join(QStringLiteral(", ")));
  QStringList details{title,
                      QStringLiteral("Workspace: %1")
                          .arg(render.cwd.empty() ? QStringLiteral("Unknown")
                                                  : text(render.cwd)),
                      QStringLiteral("Status: %1").arg(status),
                      QStringLiteral("Last activity: %1")
                          .arg(activityText(render.lastActivityAt))};
  if (item.parent) {
    details.push_back(QStringLiteral("Parent: %1")
                          .arg(render.parentTitle.empty()
                                   ? text(item.parent->id().canonical)
                                   : text(render.parentTitle)));
  }
  item.setToolTip(details.join(QLatin1Char('\n')));
  updateRow(row, render.id, render.title, render.status, render.pending,
            item.depth, item.hasChildren, item.expanded, item.optimistic,
            item.optimisticFailed);
  item.renderedRevision = render.revision;
  item.renderedPending = render.pending;
  if (item.attachment) {
    item.attachment->widget = row;
    item.attachment->renderedRevision = render.revision;
  }
}

ThreadPane::GraphThreadItem *
ThreadPane::graphItem(const QListWidgetItem *item) const {
  return dynamic_cast<GraphThreadItem *>(const_cast<QListWidgetItem *>(item));
}

std::string ThreadPane::visiblySelectedThreadId() const {
  const QList<QListWidgetItem *> selected = list->selectedItems();
  return selected.size() == 1 && selected.front()
             ? selected.front()->data(Qt::UserRole).toString().toStdString()
             : std::string{};
}

nodegraph::NodeRef ThreadPane::visiblySelectedThread() const {
  const QList<QListWidgetItem *> selected = list->selectedItems();
  if (selected.size() != 1 || !selected.front())
    return {};
  GraphThreadItem *item = graphItem(selected.front());
  return item ? item->node : nodegraph::NodeRef{};
}

void ThreadPane::showContextMenu(const QPoint &position) {
  if (!graph)
    return;
  QListWidgetItem *listItem = list->itemAt(position);
  GraphThreadItem *item = graphItem(listItem);
  if (!item || !item->node)
    return;
  const nodegraph::NodeRef node = item->node;
  const std::string id = node->id().canonical;

  auto read = graph->tryRead();
  if (!read) {
    QTimer::singleShot(0, this,
                       [this, position] { showContextMenu(position); });
    return;
  }
  const std::shared_ptr<const nodegraph::NodeState> threadState =
      read->state(node);
  const bool archived = graphBool(graphField(*threadState, "archived"));
  bool providerReady = false;
  bool canControl = false;
  if (nodegraph::NodeRef connection =
          read->find({nodegraph::NodeKind::Connection, "connection"})) {
    const std::shared_ptr<const nodegraph::NodeState> connectionState =
        read->state(connection);
    providerReady =
        connectionState->status == nodegraph::NodeStatus::Connected &&
        graphString(graphField(*connectionState, "providerState")) == "ready";
    canControl =
        providerReady &&
        graphString(graphField(*connectionState, "role")) == "controller";
  }
  read.reset();

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
  QAction *reload =
      menu->addAction(QStringLiteral("Reload"), this, [this, node] {
        if (nodeActions.reload)
          nodeActions.reload(node);
      });
  QAction *rename =
      menu->addAction(QStringLiteral("Rename"), this, [this, node] {
        if (nodeActions.rename)
          nodeActions.rename(node);
      });
  QAction *fork = menu->addAction(QStringLiteral("Fork"), this, [this, node] {
    if (nodeActions.fork)
      nodeActions.fork(node);
  });
  QAction *archive = menu->addAction(
      archived ? QStringLiteral("Unarchive") : QStringLiteral("Archive"), this,
      [this, node, archived] {
        if (archived && nodeActions.unarchive)
          nodeActions.unarchive(node);
        else if (!archived && nodeActions.archive)
          nodeActions.archive(node);
      });
  menu->addSeparator();
  QAction *remove =
      menu->addAction(QStringLiteral("Delete"), this, [this, node] {
        if (nodeActions.remove)
          nodeActions.remove(node);
      });
  reload->setEnabled(providerReady);
  rename->setEnabled(canControl);
  fork->setEnabled(canControl);
  archive->setEnabled(canControl);
  remove->setEnabled(canControl);
  menu->popup(list->viewport()->mapToGlobal(position));
}

} // namespace codexui::codex::middle
