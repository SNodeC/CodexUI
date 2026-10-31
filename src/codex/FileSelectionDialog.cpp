// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "codex/FileSelectionDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeDatabase>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
namespace codexui::codex {
    FileSelectionDialog::FileSelectionDialog(
        QString initialDirectory, QWidget* parent, QString targetTitle, QStringList choices, Filesystem filesystem, bool files)
        : QDialog(parent)
        , filesystem(std::move(filesystem))
        , serverDirectory(initialDirectory.isEmpty() ? QStringLiteral("/") : initialDirectory)
        , localDirectory(QDir::homePath())
        , files(files) {
        setModal(true);
        setWindowTitle(files ? "Attach files" : targetTitle.isEmpty() ? "Select app-server workspace" : "Change workspace…");
        setMinimumWidth(360);
        resize(640, 480);
        auto* root = new QVBoxLayout(this);
        auto* heading = new QLabel(windowTitle());
        heading->setProperty("kind", "heading");
        root->addWidget(heading);
        if (!targetTitle.isEmpty()) {
            auto* target = new QLabel(targetTitle);
            target->setTextFormat(Qt::PlainText);
            root->addWidget(target);
        }
        if (files) {
            source = new QComboBox;
            source->setObjectName("selectionSource");
            source->setAccessibleName("Browse location");
            source->setToolTip("Choose which computer to browse.");
            source->addItems({"This computer", "App-server"});
            source->setCurrentIndex(1);
            root->addWidget(source);
        }
        status = new QLabel;
        status->setWordWrap(true);
        status->setAccessibleName("Selection location and status");
        root->addWidget(status);
        auto* path = new QComboBox;
        path->setObjectName("workspaceRoots");
        path->setEditable(true);
        path->addItems(choices);
        location = path->lineEdit();
        location->setObjectName("codexWorkspaceDirectory");
        location->setAccessibleName(files ? "File or directory path" : "Proposed workspace");
        location->setText(serverDirectory);
        auto* navigation = new QHBoxLayout;
        auto* up = new QPushButton(QIcon::fromTheme("go-up", style()->standardIcon(QStyle::SP_ArrowUp)), {});
        auto* open = new QPushButton(QIcon::fromTheme("go-jump", style()->standardIcon(QStyle::SP_ArrowForward)), {});
        up->setAccessibleName("Open parent directory");
        open->setAccessibleName("Open entered directory");
        up->setToolTip(up->accessibleName());
        open->setToolTip(open->accessibleName());
        navigation->addWidget(up);
        navigation->addWidget(path, 1);
        navigation->addWidget(open);
        root->addLayout(navigation);
        entries = new QTreeWidget;
        entries->setAccessibleName("Directories and files at selected location");
        entries->setHeaderLabels({"Name", "Type"});
        entries->header()->setStretchLastSection(false);
        entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        entries->header()->setSectionResizeMode(1, QHeaderView::Interactive);
        entries->setRootIsDecorated(false);
        entries->setAlternatingRowColors(true);
        entries->setUniformRowHeights(true);
        entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
        entries->setContextMenuPolicy(Qt::CustomContextMenu);
        entries->setSelectionMode(files ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
        root->addWidget(entries, 1);
        auto* footer = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        acceptButton = footer->button(QDialogButtonBox::Ok);
        acceptButton->setProperty("kind", "primary");
        root->addWidget(footer);
        connect(footer, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(footer, &QDialogButtonBox::accepted, this, [this] {
            if (this->files && !localFiles() && this->filesystem) {
                const auto serial = ++request;
                busy = true;
                updateActions();
                this->filesystem(this,
                                 "fs/getMetadata",
                                 {{"path", selectedFiles().first().toStdString()}},
                                 [this, serial](nlohmann::json metadata, std::string error) {
                                     if (serial == request) {
                                         busy = false;
                                         if (error.empty() && metadata.value("isFile", false)) {
                                             accept();
                                         } else {
                                             status->setText(error.empty() ? "Selected file is no longer available."
                                                                           : QString::fromStdString(error));
                                             updateActions();
                                         }
                                     }
                                 });
            } else {
                accept();
            }
        });
        acceptButton->setDefault(true);
        connect(path, &QComboBox::currentTextChanged, this, [this] {
            ++request;
            busy = false;
            entries->clear();
            status->setText("Choose Open directory to browse the entered path.");
            updateActions();
        });
        if (files) {
            connect(source, &QComboBox::currentIndexChanged, this, [this, path, choices] {
                path->clear();
                if (!localFiles()) {
                    path->addItems(choices);
                }
                location->setText(localFiles() ? localDirectory : serverDirectory);
                loadDirectory();
            });
        }
        connect(open, &QPushButton::clicked, this, [this] {
            loadDirectory();
        });
        connect(up, &QPushButton::clicked, this, [this] {
            location->setText(QDir::cleanPath(location->text() + "/.."));
            loadDirectory();
        });
        connect(entries, &QTreeWidget::itemSelectionChanged, this, [this] {
            updateActions();
        });
        connect(entries, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* entry) {
            activateEntry(entry);
        });
        connect(entries, &QTreeWidget::customContextMenuRequested, this, [this, up](const QPoint& point) {
            auto* item = entries->itemAt(point);
            if (item && !item->isSelected()) {
                entries->setCurrentItem(item);
            }
            QMenu menu(this);
            auto* use = menu.addAction(item && item->data(0, Qt::UserRole + 1).toBool() ? "Open directory" : "Use selection", this, [this] {
                activateEntry(entries->currentItem());
            });
            use->setEnabled(item && !busy);
            menu.addAction(up->icon(), "Parent directory", up, &QPushButton::click);
            menu.addAction(QIcon::fromTheme("view-refresh"), "Refresh", this, &FileSelectionDialog::loadDirectory);
            auto* hidden = menu.addAction("Show hidden files");
            hidden->setCheckable(true);
            hidden->setChecked(showHidden);
            connect(hidden, &QAction::toggled, this, [this](bool value) {
                showHidden = value;
                loadDirectory();
            });
            menu.addSeparator();
            menu.addAction(QIcon::fromTheme("edit-copy"), "Copy path", this, [this] {
                QStringList paths;
                for (auto* item : entries->selectedItems()) {
                    paths.push_back(item->data(0, Qt::UserRole).toString());
                }
                QApplication::clipboard()->setText(paths.isEmpty() ? location->text() : paths.join('\n'));
            });
            menu.exec(entries->viewport()->mapToGlobal(point));
        });
        loadDirectory();
        location->setFocus();
        location->selectAll();
    }
    void FileSelectionDialog::activateEntry(QTreeWidgetItem* entry) {
        if (entry && !busy) {
            if (entry->data(0, Qt::UserRole + 1).toBool()) {
                location->setText(entry->data(0, Qt::UserRole).toString());
                loadDirectory();
            } else if (files) {
                acceptButton->click();
            }
        }
    }
    void FileSelectionDialog::loadDirectory() {
        const auto serial = ++request;
        const bool local = localFiles();
        const auto directory = location->text().trimmed();
        if (!directory.isEmpty()) {
            (local ? localDirectory : serverDirectory) = directory;
            entries->clear();
            busy = true;
            status->setText(local ? "This computer — loading files" : "App-server filesystem — loading files");
            updateActions();
            const auto complete = [this, serial, directory](nlohmann::json result, std::string error) {
                if (serial == request) {
                    busy = false;
                    status->setText(error.empty() ? localFiles() ? "This computer. Uploads remain in: " + serverDirectory
                                                                 : "App-server filesystem. Paths are used directly."
                                                  : QString::fromStdString(error));
                    const bool single = error.empty() && result.value("isFile", false);
                    if (single) {
                        (localFiles() ? localDirectory : serverDirectory) = QFileInfo(directory).path();
                    }
                    auto listed =
                        single ? nlohmann::json::array({{{"fileName", QFileInfo(directory).fileName().toStdString()}, {"isFile", true}}})
                        : error.empty() ? result.value("entries", nlohmann::json::array())
                                        : nlohmann::json::array();
                    auto& items = listed.get_ref<nlohmann::json::array_t&>();
                    std::ranges::sort(items, [](const auto& left, const auto& right) {
                        const bool a = left.value("isDirectory", false), b = right.value("isDirectory", false);
                        return a != b ? a
                                      : QString::localeAwareCompare(QString::fromStdString(left.value("fileName", "")),
                                                                    QString::fromStdString(right.value("fileName", ""))) < 0;
                    });
                    const auto parent = QDir::cleanPath(directory + "/..");
                    if (error.empty() && parent != QDir::cleanPath(directory)) {
                        auto* row = new QTreeWidgetItem(entries, {"..", "Parent directory"});
                        row->setIcon(0, QIcon::fromTheme("go-up", style()->standardIcon(QStyle::SP_ArrowUp)));
                        row->setData(0, Qt::UserRole, parent);
                        row->setData(0, Qt::UserRole + 1, true);
                    }
                    for (const auto& item : listed) {
                        const bool folder = item.value("isDirectory", false);
                        const auto name = QString::fromStdString(item.value("fileName", ""));
                        if ((folder || (files && item.value("isFile", false))) && name != "." && name != ".." &&
                            (showHidden || !name.startsWith('.'))) {
                            const auto mime = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
                            auto* row = new QTreeWidgetItem(entries, {name, folder ? "Folder" : mime.comment()});
                            row->setIcon(0,
                                         QIcon::fromTheme(folder ? "folder" : mime.iconName(),
                                                          style()->standardIcon(folder ? QStyle::SP_DirIcon : QStyle::SP_FileIcon)));
                            row->setData(0, Qt::UserRole, single ? directory : QDir::cleanPath(directory + '/' + name));
                            row->setData(0, Qt::UserRole + 1, folder);
                            row->setToolTip(0, row->data(0, Qt::UserRole).toString());
                            row->setToolTip(1, row->text(1));
                            row->setSelected(single);
                        }
                    }
                    updateActions();
                }
            };
            if (local) {
                pool.start([this, directory, complete] {
                    nlohmann::json result = {{"entries", nlohmann::json::array()}};
                    const QDir folder(directory);
                    for (const auto& file : folder.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                                 QDir::DirsFirst | QDir::Name)) {
                        result["entries"].push_back(
                            {{"fileName", file.fileName().toStdString()}, {"isDirectory", file.isDir()}, {"isFile", file.isFile()}});
                    }
                    const bool single = QFileInfo(directory).isFile();
                    const auto error = single || (folder.exists() && folder.isReadable()) ? "" : "Cannot read this computer's directory.";
                    if (single) {
                        result = {{"isFile", true}};
                    }
                    QMetaObject::invokeMethod(
                        this,
                        [complete, result, error] {
                            complete(result, error);
                        },
                        Qt::QueuedConnection);
                });
            } else if (filesystem) {
                filesystem(this,
                           files ? "fs/getMetadata" : "fs/readDirectory",
                           {{"path", directory.toStdString()}},
                           [this, directory, serial, complete](nlohmann::json result, std::string error) {
                               if (files && error.empty() && !result.value("isFile", false) && serial == request) {
                                   filesystem(this, "fs/readDirectory", {{"path", directory.toStdString()}}, complete);
                               } else {
                                   complete(result, error);
                               }
                           });
            } else {
                complete({}, "App-server browsing is unavailable. You can still enter a workspace path.");
            }
        } else {
            ++request;
            busy = false;
            status->setText("Enter an absolute app-server directory and choose Open directory.");
            updateActions();
        }
    }
    QString FileSelectionDialog::selectedDirectory() const {
        QString value = location->text().trimmed();
        if (!files && !entries->selectedItems().isEmpty()) {
            value = entries->selectedItems().first()->data(0, Qt::UserRole).toString();
        }
        return value;
    }
    QStringList FileSelectionDialog::selectedFiles() const {
        QStringList paths;
        for (const auto* entry : entries->selectedItems()) {
            if (!entry->data(0, Qt::UserRole + 1).toBool()) {
                paths.push_back(entry->data(0, Qt::UserRole).toString());
            }
        }
        return paths;
    }
    bool FileSelectionDialog::localFiles() const {
        return files && source->currentIndex() == 0;
    }
    QString FileSelectionDialog::uploadDirectory() const {
        return serverDirectory;
    }
    void FileSelectionDialog::updateActions() {
        acceptButton->setText(files                                  ? localFiles() ? "Upload and attach" : "Attach selected files"
                              : windowTitle() == "Change workspace…" ? "Change workspace"
                                                                     : "Select workspace");
        acceptButton->setEnabled(!busy &&
                                 (files ? !selectedFiles().isEmpty() && !serverDirectory.isEmpty() : !selectedDirectory().isEmpty()));
    }
} // namespace codexui::codex
