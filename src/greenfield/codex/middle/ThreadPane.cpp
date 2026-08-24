// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"

#include "codex/PresentationModel.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {
namespace {

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
  dot->setFixedSize(8, 8);
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
  QString color = QStringLiteral("#98a2b3");
  if (requestCount != 0)
    color = QStringLiteral("#a76812");
  else if (thread.status == "active" || thread.status == "inProgress")
    color = QStringLiteral("#2f6feb");
  else if (thread.status == "failed" || thread.status == "systemError")
    color = QStringLiteral("#b83a3a");
  dot->setStyleSheet(
      QStringLiteral("background:%1;border-radius:4px;").arg(color));
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
  header->setContentsMargins(8, 0, 6, 8);
  header->addWidget(makeLabel(QStringLiteral("WORK"), "section"));
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
  layout->addLayout(toolbar);

  list = new QListWidget;
  list->setObjectName(QStringLiteral("threadList"));
  list->setSelectionMode(QAbstractItemView::SingleSelection);
  list->setContextMenuPolicy(Qt::CustomContextMenu);
  list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list->setTextElideMode(Qt::ElideRight);
  list->setStyleSheet(QStringLiteral(
      "QListWidget#threadList{background:transparent;border:0;outline:0;}"
      "QListWidget#threadList::item{min-height:30px;border:0;border-radius:5px;"
      "padding:2px 8px;color:#344054;}"
      "QListWidget#threadList::item:hover{background:#eef3fa;}"
      "QListWidget#threadList::item:selected{background:#e5eeff;"
      "color:#1d2633;font-weight:600;}"));
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

void ThreadPane::refresh(const PresentationModel &model,
                         const std::string &selectedThreadId) {
  currentModel = &model;
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
  const std::string serialized =
      nlohmann::json{{"selected", selectedThreadId}, {"rows", visible}}.dump();
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
      item->setSizeHint(QSize(0, 48));
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
  QMenu menu(list);
  menu.addAction(QStringLiteral("Reload"), this, [this, id] {
    if (actions.reload)
      actions.reload(id);
  });
  const bool canControl = currentModel->connection().connected &&
                          currentModel->connection().role == "controller";
  QAction *rename = menu.addAction(QStringLiteral("Rename"), this, [this, id] {
    if (actions.rename)
      actions.rename(id);
  });
  QAction *fork = menu.addAction(QStringLiteral("Fork"), this, [this, id] {
    if (actions.fork)
      actions.fork(id);
  });
  QAction *archive =
      menu.addAction(thread->archived ? QStringLiteral("Unarchive")
                                      : QStringLiteral("Archive"),
                     this, [this, id] {
                       if (actions.toggleArchive)
                         actions.toggleArchive(id);
                     });
  menu.addSeparator();
  QAction *remove = menu.addAction(QStringLiteral("Delete"), this, [this, id] {
    if (actions.remove)
      actions.remove(id);
  });
  rename->setEnabled(canControl);
  fork->setEnabled(canControl);
  archive->setEnabled(canControl);
  remove->setEnabled(canControl);
  menu.exec(list->viewport()->mapToGlobal(position));
}

} // namespace codexui::codex::middle
