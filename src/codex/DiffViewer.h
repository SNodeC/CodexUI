// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_DIFFVIEWER_H
#define CODEXUI_CODEX_DIFFVIEWER_H

#include "codex/GitDiffProvider.h"

#include <QPointer>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace codexui::codex {

class GitDiffReviewWindow;

class DiffViewer final : public QWidget {
public:
  explicit DiffViewer(QWidget *parent = nullptr);

  void setRepositoryContext(QString threadId, QString workspace,
                            QStringList commandDirectories,
                            QStringList changedPaths);
  void refreshRepository();
  [[nodiscard]] const GitDiffSnapshot &currentSnapshot() const noexcept;

private:
  void applySnapshot(const GitDiffSnapshot &snapshot);
  void showSelectedFile();
  void openReview();
  void updateFileWatches();
  [[nodiscard]] QString selectedPath() const;
  [[nodiscard]] QStringList repositoryCandidates() const;

  GitDiffProvider *provider = nullptr;
  QFileSystemWatcher *fileWatcher = nullptr;
  QTimer *refreshTimer = nullptr;
  QTimer *repositoryTimer = nullptr;
  QString workspace;
  QString threadId;
  QStringList commandDirectories;
  QStringList changedPaths;
  QStringList persistedRepositoryRoots;
  QString selectedRepository;
  GitDiffSnapshot snapshot;
  QByteArray snapshotFingerprint;
  QComboBox *scope = nullptr;
  QComboBox *repositories = nullptr;
  QPushButton *hiddenRepositories = nullptr;
  QLabel *summary = nullptr;
  QLabel *authority = nullptr;
  QLabel *selectedFile = nullptr;
  QListWidget *files = nullptr;
  QPlainTextEdit *diff = nullptr;
  QPushButton *copyButton = nullptr;
  QPushButton *reviewButton = nullptr;
  QPointer<GitDiffReviewWindow> reviewWindow;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_DIFFVIEWER_H
