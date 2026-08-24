// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/FileSelectionDialog.h"

#include <QAbstractItemView>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeDatabase>
#include <QModelIndex>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

namespace codexui::codex {
namespace {

constexpr int MaximumAttachments = 16;

QLabel *dialogLabel(QString text, const char *kind) {
  auto *label = new QLabel(std::move(text));
  label->setProperty("kind", kind);
  label->setWordWrap(true);
  return label;
}

QString readableSize(std::int64_t bytes) {
  constexpr std::int64_t KiB = 1024;
  constexpr std::int64_t MiB = KiB * 1024;
  if (bytes >= MiB)
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / MiB, 0, 'f',
                                       1);
  if (bytes >= KiB)
    return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / KiB, 0, 'f',
                                       1);
  return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

FileSelectionDialog::FileSelectionDialog(
    Mode mode, QString initialDirectory,
    std::vector<AttachmentDraft> initialAttachments, QWidget *parent)
    : QDialog(parent), mode(mode) {
  setModal(true);
  setWindowTitle(mode == Mode::Workspace ? QStringLiteral("Select workspace")
                                         : QStringLiteral("Attach files"));
  resize(720, mode == Mode::Workspace ? 560 : 680);
  setMinimumSize(560, 460);

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(24, 22, 24, 20);
  root->setSpacing(14);
  root->addWidget(dialogLabel(mode == Mode::Workspace
                                  ? QStringLiteral("Select workspace")
                                  : QStringLiteral("Attach files"),
                              "heading"));
  root->addWidget(dialogLabel(
      mode == Mode::Workspace
          ? QStringLiteral(
                "Choose the directory Codex should use for the new thread.")
          : QStringLiteral(
                "Choose local files to include with the next message."),
      "muted"));

  auto *locationRow = new QHBoxLayout;
  locationRow->setSpacing(8);
  auto *up = new QPushButton(QStringLiteral("Up"));
  up->setProperty("kind", "subtle");
  up->setFixedHeight(34);
  location = new QLineEdit;
  location->setAccessibleName(QStringLiteral("Current directory"));
  auto *go = new QPushButton(QStringLiteral("Go"));
  go->setFixedHeight(34);
  locationRow->addWidget(up);
  locationRow->addWidget(location, 1);
  locationRow->addWidget(go);
  root->addLayout(locationRow);

  fileSystem = new QFileSystemModel(this);
  fileSystem->setFilter(
      mode == Mode::Workspace
          ? QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives
          : QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Drives);
  fileSystem->setRootPath(QStringLiteral("/"));
  browser = new QTreeView;
  browser->setObjectName(QStringLiteral("codexFileBrowser"));
  browser->setModel(fileSystem);
  browser->setRootIsDecorated(false);
  browser->setItemsExpandable(false);
  browser->setSortingEnabled(true);
  browser->sortByColumn(0, Qt::AscendingOrder);
  browser->setSelectionBehavior(QAbstractItemView::SelectRows);
  browser->setSelectionMode(mode == Mode::Workspace
                                ? QAbstractItemView::SingleSelection
                                : QAbstractItemView::ExtendedSelection);
  browser->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  browser->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  browser->header()->setStretchLastSection(false);
  browser->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  browser->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  browser->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  browser->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  root->addWidget(browser, 1);

  if (mode == Mode::Attachments) {
    auto *selectionHeader = new QHBoxLayout;
    selectionHeader->addWidget(
        dialogLabel(QStringLiteral("Selected files"), "title"));
    selectionHeader->addStretch();
    addButton = new QPushButton(QStringLiteral("Add selected"));
    addButton->setFixedHeight(30);
    removeButton = new QPushButton(QStringLiteral("Remove"));
    removeButton->setProperty("kind", "subtle");
    removeButton->setFixedHeight(30);
    selectionHeader->addWidget(addButton);
    selectionHeader->addWidget(removeButton);
    root->addLayout(selectionHeader);

    attachments = new QListWidget;
    attachments->setObjectName(QStringLiteral("codexAttachmentList"));
    attachments->setSelectionMode(QAbstractItemView::ExtendedSelection);
    attachments->setMaximumHeight(128);
    root->addWidget(attachments);
    for (const AttachmentDraft &attachment : initialAttachments) {
      auto *item = new QListWidgetItem(
          QStringLiteral("%1  |  %2")
              .arg(attachment.name, readableSize(attachment.size)));
      item->setData(Qt::UserRole, attachment.path);
      item->setData(Qt::UserRole + 1, attachment.mimeType);
      item->setData(Qt::UserRole + 2,
                    QVariant::fromValue<qlonglong>(attachment.size));
      item->setToolTip(attachment.path);
      attachments->addItem(item);
    }
  }

  errorLabel = dialogLabel({}, "meta");
  errorLabel->setStyleSheet(QStringLiteral("color:#b83a3a;"));
  errorLabel->hide();
  root->addWidget(errorLabel);

  auto *footer = new QHBoxLayout;
  selectionSummary = dialogLabel({}, "meta");
  footer->addWidget(selectionSummary);
  footer->addStretch();
  auto *cancel = new QPushButton(QStringLiteral("Cancel"));
  cancel->setProperty("kind", "subtle");
  cancel->setFixedHeight(34);
  acceptButton = new QPushButton(mode == Mode::Workspace
                                     ? QStringLiteral("Select workspace")
                                     : QStringLiteral("Attach"));
  acceptButton->setProperty("kind", "primary");
  acceptButton->setFixedHeight(34);
  footer->addWidget(cancel);
  footer->addWidget(acceptButton);
  root->addLayout(footer);

  connect(up, &QPushButton::clicked, this, [this] {
    navigateTo(QFileInfo(currentDirectory).dir().absolutePath());
  });
  connect(go, &QPushButton::clicked, this, [this] { navigateFromLocation(); });
  connect(location, &QLineEdit::returnPressed, this,
          [this] { navigateFromLocation(); });
  connect(browser, &QTreeView::doubleClicked, this,
          [this](const QModelIndex &index) {
            const QFileInfo info = fileSystem->fileInfo(index);
            if (info.isDir())
              navigateTo(info.absoluteFilePath());
            else if (this->mode == Mode::Attachments)
              addSelectedFiles();
          });
  connect(browser->selectionModel(), &QItemSelectionModel::selectionChanged,
          this, [this] { updateActions(); });
  if (addButton)
    connect(addButton, &QPushButton::clicked, this,
            [this] { addSelectedFiles(); });
  if (removeButton)
    connect(removeButton, &QPushButton::clicked, this,
            [this] { removeSelectedFiles(); });
  if (attachments)
    connect(attachments, &QListWidget::itemSelectionChanged, this,
            [this] { updateActions(); });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(acceptButton, &QPushButton::clicked, this,
          [this] { acceptSelection(); });

  if (initialDirectory.isEmpty() || !QFileInfo(initialDirectory).isDir())
    initialDirectory = QDir::homePath();
  navigateTo(std::move(initialDirectory));
}

QString FileSelectionDialog::selectedDirectory() const {
  const QModelIndex index = browser->currentIndex();
  if (index.isValid()) {
    const QFileInfo info = fileSystem->fileInfo(index);
    if (info.isDir())
      return info.absoluteFilePath();
  }
  return currentDirectory;
}

std::vector<AttachmentDraft> FileSelectionDialog::selectedAttachments() const {
  std::vector<AttachmentDraft> result;
  if (!attachments)
    return result;
  result.reserve(static_cast<std::size_t>(attachments->count()));
  for (int index = 0; index < attachments->count(); ++index) {
    const QListWidgetItem *item = attachments->item(index);
    result.push_back(AttachmentDraft{
        item->data(Qt::UserRole).toString(),
        QFileInfo(item->data(Qt::UserRole).toString()).fileName(),
        item->data(Qt::UserRole + 1).toString(),
        item->data(Qt::UserRole + 2).toLongLong()});
  }
  return result;
}

void FileSelectionDialog::navigateTo(QString path) {
  const QFileInfo info(path);
  if (!info.exists() || !info.isDir()) {
    errorLabel->setText(
        QStringLiteral("The selected directory does not exist."));
    errorLabel->show();
    return;
  }
  currentDirectory = info.absoluteFilePath();
  location->setText(QDir::toNativeSeparators(currentDirectory));
  browser->clearSelection();
  browser->setCurrentIndex({});
  browser->setRootIndex(fileSystem->index(currentDirectory));
  errorLabel->hide();
  updateActions();
}

void FileSelectionDialog::navigateFromLocation() {
  navigateTo(QDir::fromNativeSeparators(location->text().trimmed()));
}

void FileSelectionDialog::addSelectedFiles() {
  if (!attachments)
    return;
  const QModelIndexList rows = browser->selectionModel()->selectedRows(0);
  QMimeDatabase mimeDatabase;
  for (const QModelIndex &index : rows) {
    const QFileInfo info = fileSystem->fileInfo(index);
    if (!info.isFile())
      continue;
    bool duplicate = false;
    for (int itemIndex = 0; itemIndex < attachments->count(); ++itemIndex) {
      if (attachments->item(itemIndex)->data(Qt::UserRole).toString() ==
          info.absoluteFilePath()) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;
    if (attachments->count() >= MaximumAttachments) {
      errorLabel->setText(
          QStringLiteral("A message can contain at most %1 attachments.")
              .arg(MaximumAttachments));
      errorLabel->show();
      break;
    }
    const QString mime =
        mimeDatabase.mimeTypeForFile(info, QMimeDatabase::MatchContent).name();
    auto *item = new QListWidgetItem(
        QStringLiteral("%1  |  %2")
            .arg(info.fileName(), readableSize(info.size())));
    item->setData(Qt::UserRole, info.absoluteFilePath());
    item->setData(Qt::UserRole + 1, mime);
    item->setData(Qt::UserRole + 2,
                  QVariant::fromValue<qlonglong>(info.size()));
    item->setToolTip(info.absoluteFilePath());
    attachments->addItem(item);
  }
  updateActions();
}

void FileSelectionDialog::removeSelectedFiles() {
  if (!attachments)
    return;
  const QList<QListWidgetItem *> selected = attachments->selectedItems();
  for (QListWidgetItem *item : selected)
    delete attachments->takeItem(attachments->row(item));
  updateActions();
}

void FileSelectionDialog::updateActions() {
  if (mode == Mode::Workspace) {
    selectionSummary->setText(QDir::toNativeSeparators(selectedDirectory()));
    acceptButton->setEnabled(QFileInfo(selectedDirectory()).isDir());
    return;
  }
  const int count = attachments ? attachments->count() : 0;
  selectionSummary->setText(
      count == 1 ? QStringLiteral("1 file selected")
                 : QStringLiteral("%1 files selected").arg(count));
  acceptButton->setEnabled(count > 0);
  if (addButton) {
    const QModelIndexList rows = browser->selectionModel()->selectedRows(0);
    addButton->setEnabled(
        std::any_of(rows.begin(), rows.end(), [this](const QModelIndex &index) {
          return fileSystem->fileInfo(index).isFile();
        }));
  }
  if (removeButton)
    removeButton->setEnabled(attachments &&
                             !attachments->selectedItems().isEmpty());
}

void FileSelectionDialog::acceptSelection() {
  if (mode == Mode::Workspace && !QFileInfo(selectedDirectory()).isDir()) {
    errorLabel->setText(QStringLiteral("Select an existing directory."));
    errorLabel->show();
    return;
  }
  if (mode == Mode::Attachments &&
      (!attachments || attachments->count() == 0)) {
    errorLabel->setText(QStringLiteral("Select at least one file."));
    errorLabel->show();
    return;
  }
  accept();
}

} // namespace codexui::codex
