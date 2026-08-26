// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"

#include "codex/PresentationModel.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractItemView>
#include <QActionGroup>
#include <QCollator>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QStyleOptionToolButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int ContextMenuRole = Qt::UserRole + 1;

class ThreadListWidget final : public QListWidget {
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
  }
};

class ChevronToolButton final : public QToolButton {
protected:
  void paintEvent(QPaintEvent *event) override {
    QToolButton::paintEvent(event);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    const QRect contents =
        style()->subElementRect(QStyle::SE_ToolButtonLayoutItem, &option, this);
    const int indicatorWidth =
        style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, this);
    const QRect indicator(contents.right() - std::max(12, indicatorWidth),
                          contents.top(), std::max(12, indicatorWidth),
                          contents.height());
    UiStyle::drawChevron(
        this, indicator, option.state & QStyle::State_Enabled,
        option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus));
  }
};

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString displayStatus(const std::string &status) {
  if (status == "inProgress" || status == "active")
    return QStringLiteral("Running");
  if (status == "completed" || status == "idle")
    return QStringLiteral("Completed");
  if (status == "failed" || status == "systemError")
    return QStringLiteral("Failed");
  return status.empty() ? QStringLiteral("Unknown") : text(status);
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

void updateRow(QWidget *row, const ThreadPresentation &thread,
               std::size_t requestCount) {
  auto *title = row->findChild<QLabel *>(QStringLiteral("threadTitle"));
  auto *status = row->findChild<QLabel *>(QStringLiteral("threadStatus"));
  auto *dot = row->findChild<QFrame *>(QStringLiteral("threadStatusDot"));
  QString titleText = text(thread.title);
  if (titleText.isEmpty())
    titleText = text(thread.id.substr(0, 12));
  if (requestCount != 0)
    titleText.prepend(QStringLiteral("! "));
  title->setText(titleText);
  status->setText(displayStatus(thread.status));
  QString color = QStringLiteral("#cacccf");
  if (requestCount != 0)
    color = QStringLiteral("#a85d0c");
  else if (thread.status == "active" || thread.status == "inProgress")
    color = QStringLiteral("#2f6feb");
  else if (thread.status == "failed" || thread.status == "systemError")
    color = QStringLiteral("#c43d4d");
  dot->setStyleSheet(
      QStringLiteral("background:%1;border-radius:5px;").arg(color));
}

QWidget *createRow() {
  auto *row = new QWidget;
  row->setAttribute(Qt::WA_TransparentForMouseEvents);
  row->setStyleSheet(QStringLiteral("background:transparent;"));
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(5, 2, 5, 2);
  layout->setSpacing(8);
  layout->addWidget(statusDot());
  auto *copy = new QVBoxLayout;
  copy->setContentsMargins(0, 0, 0, 0);
  copy->setSpacing(1);
  auto *title = makeLabel({}, "title");
  title->setObjectName(QStringLiteral("threadTitle"));
  title->setStyleSheet(QStringLiteral("font-weight:500;"));
  auto *status = makeLabel({}, "meta");
  status->setObjectName(QStringLiteral("threadStatus"));
  copy->addWidget(title);
  copy->addWidget(status);
  layout->addLayout(copy, 1);
  return row;
}

std::optional<std::int64_t> timestampFor(const ThreadPresentation &thread,
                                         ThreadPane::SortCriterion criterion) {
  if (criterion == ThreadPane::SortCriterion::Created)
    return thread.createdAt;
  if (criterion == ThreadPane::SortCriterion::LastChanged)
    return thread.updatedAt;
  return thread.recencyAt;
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
  headerDivider->setProperty("kind", "standardDivider");
  headerDivider->setFixedHeight(1);
  layout->addWidget(headerDivider);
  layout->addSpacing(8);

  auto *create = new QPushButton(QStringLiteral("+  New thread"));
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
  sortButton = new ChevronToolButton;
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
  list->setObjectName(QStringLiteral("threadList"));
  list->setItemDelegate(new ThreadItemDelegate(list));
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

void ThreadPane::setSortCriterion(SortCriterion criterion) {
  if (sortCriterion == criterion)
    return;
  sortCriterion = criterion;
  updateSortButton();
  visibleSnapshot.clear();
  if (currentModel)
    refresh(*currentModel, projectedSelectedThreadId);
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

void ThreadPane::sortVisibleThreads(std::vector<std::string> &ids,
                                    const PresentationModel &model) const {
  QCollator collator(QLocale::system().language() == QLocale::C
                         ? QLocale(QLocale::English)
                         : QLocale::system());
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setIgnorePunctuation(true);
  collator.setNumericMode(true);
  std::sort(ids.begin(), ids.end(),
            [&](const std::string &leftId, const std::string &rightId) {
              const ThreadPresentation *left = model.thread(leftId);
              const ThreadPresentation *right = model.thread(rightId);
              if (!left || !right)
                return leftId < rightId;
              if (sortCriterion == SortCriterion::Alphanumeric) {
                const QString leftTitle = text(left->title).trimmed();
                const QString rightTitle = text(right->title).trimmed();
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
                const auto leftTimestamp = timestampFor(*left, sortCriterion);
                const auto rightTimestamp = timestampFor(*right, sortCriterion);
                if (leftTimestamp != rightTimestamp) {
                  if (!leftTimestamp)
                    return false;
                  if (!rightTimestamp)
                    return true;
                  return *leftTimestamp > *rightTimestamp;
                }
              }
              return leftId < rightId;
            });
}

void ThreadPane::setContextHighlight(const std::string &threadId,
                                     bool highlighted) {
  const auto found = rows.find(threadId);
  if (found == rows.end())
    return;
  found->second->setData(ContextMenuRole, highlighted);
}

void ThreadPane::refresh(const PresentationModel &model,
                         const std::string &selectedThreadId) {
  currentModel = &model;
  projectedSelectedThreadId = selectedThreadId;
  const std::vector<std::string> &authoritativeOrder = model.threadOrder();
  std::erase_if(retainedVisibleThreads, [&](const std::string &id) {
    return !model.thread(id) ||
           std::find(authoritativeOrder.begin(), authoritativeOrder.end(),
                     id) != authoritativeOrder.end();
  });
  if (!selectedThreadId.empty() && model.thread(selectedThreadId) &&
      std::find(authoritativeOrder.begin(), authoritativeOrder.end(),
                selectedThreadId) == authoritativeOrder.end() &&
      std::find(retainedVisibleThreads.begin(), retainedVisibleThreads.end(),
                selectedThreadId) == retainedVisibleThreads.end()) {
    retainedVisibleThreads.insert(retainedVisibleThreads.begin(),
                                  selectedThreadId);
  }
  std::vector<std::string> visibleOrder = retainedVisibleThreads;
  visibleOrder.insert(visibleOrder.end(), authoritativeOrder.begin(),
                      authoritativeOrder.end());
  sortVisibleThreads(visibleOrder, model);
  nlohmann::json visible = nlohmann::json::array();
  for (const std::string &id : visibleOrder) {
    const ThreadPresentation *thread = model.thread(id);
    if (!thread)
      continue;
    visible.push_back({{"id", id},
                       {"title", thread->title},
                       {"cwd", thread->cwd},
                       {"status", thread->status},
                       {"pending", model.pendingRequestCount(id)}});
  }
  const std::string serialized = nlohmann::json{
      {"selected", selectedThreadId},
      {"sort", static_cast<int>(sortCriterion)},
      {"rows", visible}}.dump();
  const QByteArray next(serialized.data(),
                        static_cast<qsizetype>(serialized.size()));
  if (next == visibleSnapshot)
    return;
  visibleSnapshot = next;
  list->blockSignals(true);
  list->setUpdatesEnabled(false);
  // Selection is a projection of selectedThreadId, never retained widget
  // state. This also makes an explicit New Thread draft visibly select no
  // existing row.
  list->clearSelection();
  list->setCurrentRow(-1);
  std::unordered_set<std::string> retained;
  int wantedIndex = 0;
  for (const std::string &id : visibleOrder) {
    const ThreadPresentation *thread = model.thread(id);
    if (!thread)
      continue;
    retained.insert(id);
    QListWidgetItem *item = nullptr;
    const auto found = rows.find(id);
    if (found == rows.end()) {
      item = new QListWidgetItem;
      item->setSizeHint(QSize(0, 54));
      item->setData(Qt::UserRole, text(id));
      list->insertItem(wantedIndex, item);
      list->setItemWidget(item, createRow());
      rows[id] = item;
    } else {
      item = found->second;
      const int currentIndex = list->row(item);
      if (currentIndex != wantedIndex) {
        // Removing an index widget transfers it into Qt's deferred-deletion
        // path. It must never be attached again after moving the item.
        list->removeItemWidget(item);
        item = list->takeItem(currentIndex);
        list->insertItem(wantedIndex, item);
        list->setItemWidget(item, createRow());
        rows[id] = item;
      }
    }
    item->setToolTip(text(thread->cwd));
    updateRow(list->itemWidget(item), *thread, model.pendingRequestCount(id));
    if (id == contextThreadId)
      setContextHighlight(id, true);
    if (id == selectedThreadId)
      list->setCurrentItem(item);
    ++wantedIndex;
  }
  for (auto it = rows.begin(); it != rows.end();) {
    if (retained.contains(it->first)) {
      ++it;
      continue;
    }
    delete list->takeItem(list->row(it->second));
    it = rows.erase(it);
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
  if (!item || !currentModel)
    return;
  const std::string id = item->data(Qt::UserRole).toString().toStdString();
  const ThreadPresentation *thread = currentModel->thread(id);
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
  menu->addAction(QStringLiteral("Reload"), this, [this, id] {
    if (actions.reload)
      actions.reload(id);
  });
  const bool canControl = currentModel->connection().connected &&
                          currentModel->connection().role == "controller";
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
  QAction *remove =
      menu->addAction(QStringLiteral("Delete"), this, [this, id] {
        if (actions.remove)
          actions.remove(id);
      });
  rename->setEnabled(canControl);
  fork->setEnabled(canControl);
  archive->setEnabled(canControl);
  remove->setEnabled(canControl);
  menu->popup(list->viewport()->mapToGlobal(position));
}

} // namespace codexui::codex::middle
