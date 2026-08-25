// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_DIFFVIEWER_H
#define CODEXUI_CODEX_DIFFVIEWER_H

#include "codex/GitDiffProvider.h"

#include <QPointer>
#include <QWidget>

class QComboBox;
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

  void setWorkspace(QString workspace);
  void refreshRepository();

private:
  void applySnapshot(const GitDiffSnapshot &snapshot);
  void showSelectedFile();
  void openReview();
  [[nodiscard]] QString selectedPath() const;

  GitDiffProvider *provider = nullptr;
  QTimer *refreshTimer = nullptr;
  QTimer *repositoryTimer = nullptr;
  QString workspace;
  GitDiffSnapshot snapshot;
  QByteArray snapshotFingerprint;
  QComboBox *scope = nullptr;
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
