// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"

#include "codex/UiStatus.h"
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
  std::function<void(QListWidgetItem *)> toggleExpansion;
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
          toggleExpansion(item);
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
  const UiStatus classified = classifyStatus(threadStatus);
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

std::optional<std::size_t> graphCount(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const std::uint64_t *number = value->asUInt64()) {
    return *number > std::numeric_limits<std::size_t>::max()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(*number);
  }
  if (const std::int64_t *number = value->asInt64(); number && *number >= 0)
    return static_cast<std::size_t>(*number);
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
    attachment->binding = this;
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
  GraphThreadItem *selectedItem = nullptr;
};

struct ThreadPane::GraphScan final {
  struct ReadThread final {
    nodegraph::NodeRef node;
    nodegraph::NodeRef parent;
    std::vector<nodegraph::NodeRef> children;
    std::string title;
    std::optional<std::int64_t> createdAt;
    std::optional<std::int64_t> updatedAt;
    std::optional<std::int64_t> recencyAt;
    std::optional<std::int64_t> localPromptActivityAt;
  };

  struct Visit final {
    nodegraph::NodeRef node;
    nodegraph::NodeRef parent;
  };

  struct PendingThread final {
    std::size_t position = 0;
    std::size_t structuralCursor = 0;
    std::size_t structuralCount = 0;
    std::size_t agentCursor = 0;
    std::size_t agentCount = 0;
    std::unordered_set<const nodegraph::Node *> children;
  };

  struct MergeHead final {
    std::size_t current = 0;
    std::size_t end = 0;
  };

  struct AppendFrame final {
    nodegraph::NodeRef node;
    nodegraph::NodeRef parent;
    std::size_t depth = 0;
    std::size_t childCursor = 0;
    bool emitted = false;
    bool expanded = false;
  };

  enum class Phase {
    Roots,
    Threads,
    PruneExpanded,
    ExpandSelection,
    SortChunks,
    MergeInitialize,
    Merge,
    OptimisticRows,
    Rows,
  };

  std::uint64_t revision = 0;
  nodegraph::NodeRef runtime;
  std::size_t rootCount = 0;
  std::size_t rootCursor = 0;
  std::vector<nodegraph::NodeRef> roots;
  std::unordered_set<const nodegraph::Node *> rootSet;
  std::vector<Visit> visits;
  std::size_t visitCursor = 0;
  std::optional<PendingThread> pendingThread;
  std::vector<ReadThread> threads;
  std::unordered_map<const nodegraph::Node *, std::size_t> positions;
  std::unordered_map<std::string, std::size_t> idPositions;
  std::unordered_set<const nodegraph::Node *>::iterator expandedCursor;
  bool expandedCursorInitialized = false;
  nodegraph::NodeRef selectedAncestor;
  bool selectedIsPresent = false;
  std::size_t sortCursor = 0;
  std::size_t mergeInitCursor = 0;
  std::vector<MergeHead> mergeHeap;
  std::vector<nodegraph::NodeRef> sortedRoots;
  std::size_t optimisticCursor = 0;
  std::size_t appendRootCursor = 0;
  std::vector<AppendFrame> appendStack;
  std::unordered_set<const nodegraph::Node *> appended;
  GraphTopology topology;
  Phase phase = Phase::Roots;
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
  threadList->toggleExpansion = [this](QListWidgetItem *item) {
    if (GraphThreadItem *thread = graphItem(item))
      toggleExpanded(*thread);
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

void ThreadPane::toggleExpanded(GraphThreadItem &thread) {
  if (!thread.node)
    return;
  const nodegraph::Node *node = thread.node.get();
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
      if (GraphThreadItem *binding = graphItem(current))
        toggleExpanded(*binding);
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
    if (GraphThreadItem *binding = graphItem(current))
      toggleExpanded(*binding);
    return;
  }
  const QString parentId = current->data(ParentIdRole).toString();
  if (parentId.isEmpty())
    return;
  GraphThreadItem *binding = graphItem(current);
  if (binding && binding->parent) {
    if (GraphThreadItem *parent = attachedGraphItem(binding->parent))
      list->setCurrentItem(parent);
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
  if (topologyChanged) {
    // A partially applied topology is render work only. Discard it whenever a
    // newer structural revision arrives; stable NodeRefs remain authoritative
    // in the graph and the next try-read rebuilds the pending row order.
    pendingGraphScan.reset();
    pendingGraphTopology.reset();
    topologyCursor = 0;
    topologySearchCursor = -1;
    scheduleGraphRefresh();
  } else if (interactionChanged) {
    scheduleVisibilityPass();
  }
}

void ThreadPane::detachRemoved(const nodegraph::GraphChanged &change) {
  if (change.removed.empty())
    return;
  std::vector<GraphThreadItem *> removedItems;
  removedItems.reserve(change.removed.size());
  for (const nodegraph::NodeRef &node : change.removed) {
    if (!node || node->id().kind != nodegraph::NodeKind::Thread)
      continue;
    graphExpandedThreads.erase(node.get());
    if (selectedGraphThread == node) {
      selectedGraphThread.reset();
      revealSelectedGraphThread = false;
    }
    if (GraphThreadItem *item = attachedGraphItem(node))
      removedItems.emplace_back(item);
  }

  const bool signalsWereBlocked = list->blockSignals(true);
  const bool updatesWereEnabled = list->updatesEnabled();
  list->setUpdatesEnabled(false);
  for (GraphThreadItem *item : removedItems) {
    const int index = list->row(item);
    if (index < 0)
      continue;
    if (contextThreadId == item->localId && contextMenu)
      contextMenu->close();
    const bool retainForPromotion =
        item->optimistic &&
        std::ranges::any_of(optimisticThreads, [item](const auto &optimistic) {
          return optimistic.id == item->localId;
        });
    dematerialize(*item, false);
    if (retainForPromotion) {
      // thread/start atomically retires the local draft and publishes the
      // canonical thread. Keep this Qt-owned row as a node-free optimistic
      // placeholder until the scheduled topology pass rebinds it by the exact
      // previous local id.
      item->setNode({});
      item->parent.reset();
      continue;
    }
    static_cast<void>(list->takeItem(index));
    delete item;
  }
  list->setUpdatesEnabled(updatesWereEnabled);
  list->blockSignals(signalsWereBlocked);
}

void ThreadPane::leaveGraph() {
  graphRefreshScheduled = false;
  pendingGraphScan.reset();
  pendingGraphTopology.reset();
  topologyCursor = 0;
  topologySearchCursor = -1;
  topologyPassScheduled = false;
  visibilityPassScheduled = false;
  visibilityFirst = -1;
  visibilityLast = -1;
  visibilityCursor = -1;
  if (contextMenu) {
    QMenu *menu = std::exchange(contextMenu, nullptr);
    menu->close();
    menu->deleteLater();
  }
  contextThreadId.clear();
  if (!graph)
    return;

  list->blockSignals(true);
  materializedItems.clear();
  list->clear();
  graphExpandedThreads.clear();
  selectedGraphThread.reset();
  selectedOptimisticThreadId.clear();
  revealSelectedGraphThread = false;
  graph = nullptr;
  list->blockSignals(false);
}

void ThreadPane::scheduleGraphRefresh() {
  if (!graph)
    return;
  pendingGraphScan.reset();
  pendingGraphTopology.reset();
  topologyCursor = 0;
  topologySearchCursor = -1;
  scheduleGraphScanPass();
}

void ThreadPane::scheduleGraphScanPass() {
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
  constexpr std::size_t MaximumGraphWorkPerPass = 64;
  constexpr std::size_t SortChunkSize = 64;

  if (pendingGraphScan &&
      graph->publishedRevision() != pendingGraphScan->revision) {
    pendingGraphScan.reset();
    scheduleGraphScanPass();
    return;
  }

  const bool needsRead = !pendingGraphScan ||
                         pendingGraphScan->phase == GraphScan::Phase::Roots ||
                         pendingGraphScan->phase == GraphScan::Phase::Threads;
  std::optional<nodegraph::NodeGraph::ReadAccess> read;
  if (needsRead) {
    read = graph->tryRead();
    if (!read) {
      scheduleGraphScanPass();
      return;
    }
  }

  if (!pendingGraphScan) {
    pendingGraphScan = std::make_unique<GraphScan>();
    pendingGraphScan->revision = read->revision();
    pendingGraphScan->runtime =
        read->find({nodegraph::NodeKind::Runtime, "runtime"});
    if (pendingGraphScan->runtime) {
      pendingGraphScan->rootCount = read->relatedCount(
          pendingGraphScan->runtime, nodegraph::RelationKind::RootThread);
      pendingGraphScan->roots.reserve(pendingGraphScan->rootCount);
      pendingGraphScan->rootSet.reserve(pendingGraphScan->rootCount);
      pendingGraphScan->visits.reserve(pendingGraphScan->rootCount);
    }
  } else if (read && read->revision() != pendingGraphScan->revision) {
    pendingGraphScan.reset();
    read.reset();
    scheduleGraphScanPass();
    return;
  }

  GraphScan &scan = *pendingGraphScan;
  const auto timestamp = [this](const GraphScan::ReadThread &thread) {
    if (sortCriterion == SortCriterion::Created)
      return thread.createdAt;
    std::optional<std::int64_t> result =
        sortCriterion == SortCriterion::LastChanged ? thread.updatedAt
                                                    : thread.recencyAt;
    if (thread.localPromptActivityAt &&
        (!result || *thread.localPromptActivityAt > *result))
      result = thread.localPromptActivityAt;
    return result;
  };
  QCollator collator(QLocale::system().language() == QLocale::C
                         ? QLocale(QLocale::English)
                         : QLocale::system());
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setIgnorePunctuation(true);
  collator.setNumericMode(true);
  const auto rootLess = [&](const nodegraph::NodeRef &left,
                            const nodegraph::NodeRef &right) {
    const auto leftPosition = scan.positions.find(left.get());
    const auto rightPosition = scan.positions.find(right.get());
    if (leftPosition == scan.positions.end() ||
        rightPosition == scan.positions.end())
      return left->id().canonical < right->id().canonical;
    const GraphScan::ReadThread &leftThread =
        scan.threads[leftPosition->second];
    const GraphScan::ReadThread &rightThread =
        scan.threads[rightPosition->second];
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
  };
  const auto heapLater = [&](const GraphScan::MergeHead &left,
                             const GraphScan::MergeHead &right) {
    return rootLess(scan.roots[right.current], scan.roots[left.current]);
  };

  std::size_t work = 0;
  while (work < MaximumGraphWorkPerPass) {
    switch (scan.phase) {
    case GraphScan::Phase::Roots:
      while (scan.rootCursor < scan.rootCount &&
             work < MaximumGraphWorkPerPass) {
        nodegraph::NodeRef root =
            read->relatedAt(scan.runtime, nodegraph::RelationKind::RootThread,
                            scan.rootCursor++);
        ++work;
        if (!root || root->id().kind != nodegraph::NodeKind::Thread ||
            read->find(root->id()) != root ||
            !scan.rootSet.insert(root.get()).second)
          continue;
        scan.roots.emplace_back(root);
        scan.visits.push_back({std::move(root), {}});
      }
      if (scan.rootCursor == scan.rootCount) {
        scan.phase = GraphScan::Phase::Threads;
        continue;
      }
      read.reset();
      scheduleGraphScanPass();
      return;

    case GraphScan::Phase::Threads:
      while (work < MaximumGraphWorkPerPass) {
        if (!scan.pendingThread) {
          if (scan.visitCursor == scan.visits.size()) {
            scan.phase = GraphScan::Phase::PruneExpanded;
            break;
          }
          GraphScan::Visit visit = std::move(scan.visits[scan.visitCursor++]);
          ++work;
          if (!visit.node ||
              visit.node->id().kind != nodegraph::NodeKind::Thread ||
              scan.positions.contains(visit.node.get()) ||
              read->find(visit.node->id()) != visit.node)
            continue;

          const std::size_t position = scan.threads.size();
          scan.positions.emplace(visit.node.get(), position);
          scan.idPositions.emplace(visit.node->id().canonical, position);
          scan.threads.push_back(
              GraphScan::ReadThread{visit.node, std::move(visit.parent)});
          GraphScan::ReadThread &thread = scan.threads.back();
          if (scan.rootSet.contains(thread.node.get())) {
            const std::shared_ptr<const nodegraph::NodeState> state =
                read->state(thread.node);
            thread.title = graphString(graphField(*state, "name"));
            if (thread.title.empty())
              thread.title = graphString(graphField(*state, "title"));
            thread.createdAt = graphTimestamp(graphField(*state, "createdAt"));
            thread.updatedAt = graphTimestamp(graphField(*state, "updatedAt"));
            thread.recencyAt = graphTimestamp(graphField(*state, "recencyAt"));
            thread.localPromptActivityAt =
                graphTimestamp(graphField(*state, "localPromptActivityAt"));
          }
          GraphScan::PendingThread pending;
          pending.position = position;
          pending.structuralCount = read->relatedCount(
              thread.node, nodegraph::RelationKind::StructuralChildThread);
          pending.agentCount = read->relatedCount(
              thread.node, nodegraph::RelationKind::AgentChildThread);
          pending.children.reserve(pending.structuralCount +
                                   pending.agentCount);
          scan.pendingThread = std::move(pending);
        }

        GraphScan::PendingThread &pending = *scan.pendingThread;
        GraphScan::ReadThread &thread = scan.threads[pending.position];
        nodegraph::NodeRef child;
        if (pending.structuralCursor < pending.structuralCount) {
          child = read->relatedAt(
              thread.node, nodegraph::RelationKind::StructuralChildThread,
              pending.structuralCursor++);
        } else if (pending.agentCursor < pending.agentCount) {
          child = read->relatedAt(thread.node,
                                  nodegraph::RelationKind::AgentChildThread,
                                  pending.agentCursor++);
        } else {
          scan.pendingThread.reset();
          continue;
        }
        ++work;
        if (child && child->id().kind == nodegraph::NodeKind::Thread &&
            pending.children.insert(child.get()).second) {
          thread.children.emplace_back(child);
          scan.visits.push_back({std::move(child), thread.node});
        }
      }
      if (scan.phase == GraphScan::Phase::Threads) {
        read.reset();
        scheduleGraphScanPass();
        return;
      }
      read.reset();
      continue;

    case GraphScan::Phase::PruneExpanded:
      if (!scan.expandedCursorInitialized) {
        scan.expandedCursor = graphExpandedThreads.begin();
        scan.expandedCursorInitialized = true;
      }
      while (scan.expandedCursor != graphExpandedThreads.end() &&
             work < MaximumGraphWorkPerPass) {
        if (!scan.positions.contains(*scan.expandedCursor))
          scan.expandedCursor = graphExpandedThreads.erase(scan.expandedCursor);
        else
          ++scan.expandedCursor;
        ++work;
      }
      if (scan.expandedCursor == graphExpandedThreads.end()) {
        scan.selectedIsPresent =
            selectedGraphThread &&
            scan.positions.contains(selectedGraphThread.get());
        if (scan.selectedIsPresent && revealSelectedGraphThread)
          scan.selectedAncestor = selectedGraphThread;
        scan.phase = GraphScan::Phase::ExpandSelection;
        continue;
      }
      scheduleGraphScanPass();
      return;

    case GraphScan::Phase::ExpandSelection:
      while (scan.selectedAncestor && work < MaximumGraphWorkPerPass) {
        const auto selected = scan.positions.find(scan.selectedAncestor.get());
        if (selected == scan.positions.end()) {
          scan.selectedAncestor.reset();
          break;
        }
        const nodegraph::NodeRef parent = scan.threads[selected->second].parent;
        if (!parent) {
          scan.selectedAncestor.reset();
          break;
        }
        graphExpandedThreads.insert(parent.get());
        scan.selectedAncestor = parent;
        ++work;
      }
      if (!scan.selectedAncestor) {
        if (scan.selectedIsPresent)
          scan.topology.selectedId = selectedGraphThread->id().canonical;
        else
          scan.topology.selectedId = selectedOptimisticThreadId;
        revealSelectedGraphThread = false;
        scan.phase = GraphScan::Phase::SortChunks;
        continue;
      }
      scheduleGraphScanPass();
      return;

    case GraphScan::Phase::SortChunks: {
      if (scan.sortCursor < scan.roots.size()) {
        const std::size_t end =
            std::min(scan.roots.size(), scan.sortCursor + SortChunkSize);
        std::sort(
            scan.roots.begin() + static_cast<std::ptrdiff_t>(scan.sortCursor),
            scan.roots.begin() + static_cast<std::ptrdiff_t>(end), rootLess);
        work += end - scan.sortCursor;
        scan.sortCursor = end;
      }
      if (scan.sortCursor == scan.roots.size()) {
        scan.mergeHeap.reserve((scan.roots.size() + SortChunkSize - 1) /
                               SortChunkSize);
        scan.sortedRoots.reserve(scan.roots.size());
        scan.phase = GraphScan::Phase::MergeInitialize;
        continue;
      }
      scheduleGraphScanPass();
      return;
    }

    case GraphScan::Phase::MergeInitialize:
      while (scan.mergeInitCursor < scan.roots.size() &&
             work < MaximumGraphWorkPerPass) {
        const std::size_t end =
            std::min(scan.roots.size(), scan.mergeInitCursor + SortChunkSize);
        scan.mergeHeap.push_back({scan.mergeInitCursor, end});
        std::push_heap(scan.mergeHeap.begin(), scan.mergeHeap.end(), heapLater);
        scan.mergeInitCursor = end;
        ++work;
      }
      if (scan.mergeInitCursor == scan.roots.size()) {
        scan.phase = GraphScan::Phase::Merge;
        continue;
      }
      scheduleGraphScanPass();
      return;

    case GraphScan::Phase::Merge:
      while (!scan.mergeHeap.empty() && work < MaximumGraphWorkPerPass) {
        std::pop_heap(scan.mergeHeap.begin(), scan.mergeHeap.end(), heapLater);
        GraphScan::MergeHead head = scan.mergeHeap.back();
        scan.mergeHeap.pop_back();
        scan.sortedRoots.emplace_back(scan.roots[head.current++]);
        if (head.current < head.end) {
          scan.mergeHeap.emplace_back(head);
          std::push_heap(scan.mergeHeap.begin(), scan.mergeHeap.end(),
                         heapLater);
        }
        ++work;
      }
      if (scan.mergeHeap.empty()) {
        scan.phase = GraphScan::Phase::OptimisticRows;
        continue;
      }
      scheduleGraphScanPass();
      return;

    case GraphScan::Phase::OptimisticRows:
      while (scan.optimisticCursor < optimisticThreads.size() &&
             work < MaximumGraphWorkPerPass) {
        const OptimisticThread &optimistic =
            optimisticThreads[scan.optimisticCursor++];
        const auto position = scan.idPositions.find(optimistic.id);
        if (position != scan.idPositions.end()) {
          const nodegraph::NodeRef &node = scan.threads[position->second].node;
          scan.appended.insert(node.get());
          scan.topology.rows.push_back({node,
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
          scan.topology.rows.push_back({{},
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
        ++work;
      }
      if (scan.optimisticCursor == optimisticThreads.size()) {
        scan.phase = GraphScan::Phase::Rows;
        continue;
      }
      scheduleGraphScanPass();
      return;

    case GraphScan::Phase::Rows:
      while (work < MaximumGraphWorkPerPass) {
        if (scan.appendStack.empty()) {
          if (scan.appendRootCursor == scan.sortedRoots.size())
            break;
          scan.appendStack.push_back(
              {scan.sortedRoots[scan.appendRootCursor++], {}, 0});
        }

        GraphScan::AppendFrame &frame = scan.appendStack.back();
        if (!frame.emitted) {
          const auto position = scan.positions.find(frame.node.get());
          if (position == scan.positions.end() ||
              !scan.appended.insert(frame.node.get()).second) {
            scan.appendStack.pop_back();
            ++work;
            continue;
          }
          const GraphScan::ReadThread &thread = scan.threads[position->second];
          const bool hasChildren = !thread.children.empty();
          frame.expanded =
              hasChildren && graphExpandedThreads.contains(frame.node.get());
          scan.topology.rows.push_back({frame.node,
                                        frame.parent,
                                        frame.node->id().canonical,
                                        {},
                                        {},
                                        frame.depth,
                                        hasChildren,
                                        frame.expanded});
          frame.emitted = true;
          ++work;
          if (!frame.expanded)
            scan.appendStack.pop_back();
          continue;
        }

        const auto position = scan.positions.find(frame.node.get());
        const GraphScan::ReadThread &thread = scan.threads[position->second];
        if (frame.childCursor == thread.children.size()) {
          scan.appendStack.pop_back();
          continue;
        }
        const nodegraph::NodeRef child = thread.children[frame.childCursor++];
        scan.appendStack.push_back({child, frame.node, frame.depth + 1});
        ++work;
      }
      if (scan.appendRootCursor != scan.sortedRoots.size() ||
          !scan.appendStack.empty()) {
        scheduleGraphScanPass();
        return;
      }
      GraphTopology topology = std::move(scan.topology);
      pendingGraphScan.reset();
      applyGraphTopology(std::move(topology));
      return;
    }
  }
  scheduleGraphScanPass();
}

void ThreadPane::applyGraphTopology(GraphTopology topology) {
  pendingGraphTopology = std::make_unique<GraphTopology>(std::move(topology));
  topologyCursor = 0;
  topologySearchCursor = -1;
  visibilityFirst = -1;
  visibilityLast = -1;
  visibilityCursor = -1;
  scheduleTopologyPass();
}

void ThreadPane::scheduleTopologyPass() {
  if (!graph || !pendingGraphTopology || topologyPassScheduled)
    return;
  topologyPassScheduled = true;
  QTimer::singleShot(0, this, [this] {
    topologyPassScheduled = false;
    runTopologyPass();
  });
}

void ThreadPane::runTopologyPass() {
  if (!graph || !pendingGraphTopology)
    return;

  constexpr std::size_t MaximumItemsPerPass = 32;
  GraphTopology &topology = *pendingGraphTopology;
  const bool signalsWereBlocked = list->blockSignals(true);
  const bool updatesWereEnabled = list->updatesEnabled();
  list->setUpdatesEnabled(false);
  std::size_t work = 0;

  const auto matches = [](const GraphThreadItem &item,
                          const GraphTopology::Row &row) {
    if (row.node && item.node == row.node)
      return true;
    if (!row.node && !item.node && item.localId == row.localId)
      return true;
    return row.node && !item.node &&
           (item.localId == row.localId ||
            (!row.previousLocalId.empty() &&
             item.localId == row.previousLocalId));
  };

  while (topologyCursor < topology.rows.size() && work < MaximumItemsPerPass) {
    const int destination = static_cast<int>(topologyCursor);
    GraphTopology::Row &row = topology.rows[topologyCursor];
    GraphThreadItem *item = destination < list->count()
                                ? graphItem(list->item(destination))
                                : nullptr;
    if (!item || !matches(*item, row)) {
      int existingIndex = -1;
      if (topologySearchCursor < destination + 1)
        topologySearchCursor = destination + 1;
      while (topologySearchCursor < list->count() &&
             work < MaximumItemsPerPass) {
        const int candidateIndex = topologySearchCursor++;
        GraphThreadItem *candidate = graphItem(list->item(candidateIndex));
        ++work;
        if (candidate && matches(*candidate, row)) {
          existingIndex = candidateIndex;
          item = candidate;
          break;
        }
      }
      if (existingIndex < 0 && topologySearchCursor < list->count())
        break;
      if (existingIndex >= 0) {
        // QListWidget owns the materialized row widget separately from its
        // item. Release it before moving the item so deferred deletion cannot
        // later invalidate a newly attached row at the destination.
        dematerialize(*item, false);
        static_cast<void>(list->takeItem(existingIndex));
        list->insertItem(destination, item);
        // Charge the shifted placeholder span against this pass. Large lists
        // therefore perform one model move, while tiny lists can still finish
        // their reconciliation within the established three Qt turns.
        work = std::min(MaximumItemsPerPass,
                        work + static_cast<std::size_t>(list->count()));
      } else {
        item = new GraphThreadItem;
        item->setSizeHint(QSize(0, 40));
        list->insertItem(destination, item);
      }
      topologySearchCursor = -1;
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
    if (item->attachment)
      item->attachment->renderedRevision = 0;
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
    item->setData(ContextMenuRole,
                  !contextThreadId.empty() && item->localId == contextThreadId);
    if (!topology.selectedId.empty() && item->localId == topology.selectedId)
      topology.selectedItem = item;
    ++topologyCursor;
    ++work;
  }

  while (topologyCursor == topology.rows.size() &&
         list->count() > static_cast<int>(topology.rows.size()) &&
         work < MaximumItemsPerPass) {
    const int index = static_cast<int>(topology.rows.size());
    GraphThreadItem *obsolete = graphItem(list->item(index));
    if (obsolete)
      dematerialize(*obsolete, false);
    static_cast<void>(list->takeItem(index));
    delete obsolete;
    ++work;
  }

  list->setUpdatesEnabled(updatesWereEnabled);
  list->blockSignals(signalsWereBlocked);
  if (topologyCursor != topology.rows.size() ||
      list->count() > static_cast<int>(topology.rows.size())) {
    scheduleTopologyPass();
    return;
  }

  const std::string selectedId = std::move(topology.selectedId);
  GraphThreadItem *selectedItem = topology.selectedItem;
  pendingGraphTopology.reset();
  topologyCursor = 0;
  topologySearchCursor = -1;
  const bool finalSignalsWereBlocked = list->blockSignals(true);
  list->clearSelection();
  list->setCurrentRow(-1);
  if (!selectedId.empty() && selectedItem)
    list->setCurrentItem(selectedItem);
  list->blockSignals(finalSignalsWereBlocked);
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
  if (graphRefreshScheduled || pendingGraphScan || topologyPassScheduled ||
      pendingGraphTopology || !graph || list->count() == 0)
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
  const bool rangeChanged = first != visibilityFirst || last != visibilityLast;
  if (rangeChanged) {
    visibilityFirst = first;
    visibilityLast = last;
    visibilityCursor = first;
  }

  std::size_t work = 0;
  std::unordered_map<GraphThreadItem *, int> desiredRows;
  if (first >= 0) {
    desiredRows.reserve(static_cast<std::size_t>(last - first + 1));
    for (int index = first; index <= last; ++index) {
      GraphThreadItem *item = graphItem(list->item(index));
      Q_ASSERT(item);
      desiredRows.emplace(item, index);
    }
  }

  std::vector<GraphThreadItem *> staleMaterializations;
  staleMaterializations.reserve(materializedItems.size());
  for (GraphThreadItem *item : materializedItems) {
    if (!desiredRows.contains(item))
      staleMaterializations.emplace_back(item);
  }
  for (GraphThreadItem *item : staleMaterializations) {
    if (work == MaxWidgetWork) {
      scheduleVisibilityPass();
      return;
    }
    dematerialize(*item);
    ++work;
  }

  for (const auto &[item, index] : desiredRows) {
    if (item->attachment) {
      item->attachment->viewportVisible =
          index >= firstVisible && index <= lastVisible;
      item->attachment->materialization =
          item->attachment->viewportVisible
              ? ui::NodeMaterialization::ViewportVisible
              : ui::NodeMaterialization::Overscan;
    }
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
  const int end =
      std::min(last, start + static_cast<int>(MaxWidgetWork - work) - 1);
  for (int index = start; index <= end; ++index) {
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
  for (Candidate &candidate : candidates) {
    GraphThreadItem &item = *candidate.item;
    ++work;

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
      const std::shared_ptr<const nodegraph::NodeState> state =
          read->state(item.node);
      std::optional<std::size_t> indexedPending =
          graphCount(graphField(*state, "pendingInteractionCount"));
      std::size_t pending = indexedPending.value_or(0);
      if (!indexedPending) {
        // Compatibility for small hand-built test graphs. Production worker
        // updates maintain the exact derived count on the thread node.
        constexpr std::size_t MaximumFallbackInteractions = 64;
        const std::size_t count = std::min(
            read->relatedCount(item.node,
                               nodegraph::RelationKind::PendingInteraction),
            MaximumFallbackInteractions);
        for (std::size_t index = 0; index < count; ++index) {
          const nodegraph::NodeRef interaction = read->relatedAt(
              item.node, nodegraph::RelationKind::PendingInteraction, index);
          if (!interaction ||
              interaction->id().kind != nodegraph::NodeKind::Interaction)
            continue;
          const std::shared_ptr<const nodegraph::NodeState> interactionState =
              read->state(interaction);
          if (interactionState->status != nodegraph::NodeStatus::Pending &&
              interactionState->status != nodegraph::NodeStatus::Failed)
            continue;
          ++pending;
          revision = std::max(revision, read->changedRevision(interaction));
        }
      }
      if (candidate.hasWidget &&
          item.attachment->renderedRevision == revision &&
          item.renderedPending == pending)
        continue;
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
           {std::string_view("recencyAt"), std::string_view("updatedAt"),
            std::string_view("localActivityAt"),
            std::string_view("localPromptActivityAt")}) {
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
  }
  read.reset();

  for (auto &[item, render] : renders)
    renderGraphRow(*item, render);
  visibilityCursor = end >= last ? first : end + 1;
  if (end < last)
    scheduleVisibilityPass();
}

void ThreadPane::dematerialize(GraphThreadItem &item, bool deferred) {
  std::erase(materializedItems, &item);
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
    if (std::find(materializedItems.begin(), materializedItems.end(), &item) ==
        materializedItems.end())
      materializedItems.emplace_back(&item);
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

ThreadPane::GraphThreadItem *
ThreadPane::attachedGraphItem(const nodegraph::NodeRef &node) const {
  if (!node)
    return nullptr;
  const auto *attachment =
      static_cast<const ui::QtNodeAttachment *>(node->uiAttachment());
  auto *item = attachment ? static_cast<GraphThreadItem *>(attachment->binding)
                          : nullptr;
  return item && item->node == node ? item : nullptr;
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
  showContextMenu(item->node, position);
}

void ThreadPane::showContextMenu(const nodegraph::NodeRef &node,
                                 std::optional<QPoint> requestedPosition) {
  if (!graph || !node)
    return;
  GraphThreadItem *item = attachedGraphItem(node);
  if (!item)
    return;
  const std::string id = node->id().canonical;

  auto read = graph->tryRead();
  if (!read) {
    QTimer::singleShot(0, this,
                       [this, node] { showContextMenu(node, std::nullopt); });
    return;
  }
  if (read->find(node->id()) != node)
    return;
  const std::shared_ptr<const nodegraph::NodeState> threadState =
      read->state(node);
  const bool archived = graphBool(graphField(*threadState, "archived"));
  const bool recoveryOnly = graphBool(graphField(*threadState, "recoveryOnly"));
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
  item->setData(ContextMenuRole, true);
  auto *menu = new QMenu(list);
  contextMenu = menu;
  connect(menu, &QMenu::aboutToHide, this, [this, menu, node] {
    if (contextMenu == menu) {
      if (GraphThreadItem *bound = attachedGraphItem(node))
        bound->setData(ContextMenuRole, false);
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
  reload->setEnabled(providerReady && !recoveryOnly);
  rename->setEnabled(canControl && !recoveryOnly);
  fork->setEnabled(canControl && !recoveryOnly);
  archive->setEnabled(canControl && !recoveryOnly);
  remove->setEnabled(canControl && !recoveryOnly);
  QPoint position =
      requestedPosition.value_or(list->visualItemRect(item).center());
  if (list->itemAt(position) != item)
    position = list->visualItemRect(item).center();
  menu->popup(list->viewport()->mapToGlobal(position));
}

} // namespace codexui::codex::middle
