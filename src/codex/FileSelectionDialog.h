// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_FILESELECTIONDIALOG_H
#define CODEXUI_CODEX_FILESELECTIONDIALOG_H

#include "codex/AttachmentDraft.h"

#include <QDialog>
#include <QString>

#include <optional>
#include <vector>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeView;

namespace codexui::codex {

class FileSelectionDialog final : public QDialog {
public:
  enum class Mode { Workspace, Attachments };

  explicit FileSelectionDialog(Mode mode, QString initialDirectory,
                               std::vector<AttachmentDraft> attachments = {},
                               QWidget *parent = nullptr);

  [[nodiscard]] QString selectedDirectory() const;
  [[nodiscard]] std::vector<AttachmentDraft> selectedAttachments() const;

private:
  void navigateTo(QString path);
  void navigateFromLocation();
  void addSelectedFiles();
  void removeSelectedFiles();
  void updateActions();
  void acceptSelection();

  Mode mode;
  QFileSystemModel *fileSystem = nullptr;
  QTreeView *browser = nullptr;
  QLineEdit *location = nullptr;
  QListWidget *attachments = nullptr;
  QLabel *selectionSummary = nullptr;
  QLabel *errorLabel = nullptr;
  QPushButton *addButton = nullptr;
  QPushButton *removeButton = nullptr;
  QPushButton *acceptButton = nullptr;
  QString currentDirectory;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_FILESELECTIONDIALOG_H
