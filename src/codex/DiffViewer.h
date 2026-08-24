// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_DIFFVIEWER_H
#define CODEXUI_CODEX_DIFFVIEWER_H

#include <QString>
#include <QWidget>

#include <vector>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace codexui::codex {

struct DiffFilePresentation {
  QString path;
  QString kind;
  QString diff;
};

class DiffViewer final : public QWidget {
public:
  explicit DiffViewer(QWidget *parent = nullptr);

  void setChanges(QString liveDiff,
                  std::vector<DiffFilePresentation> retainedChanges);

private:
  struct FileDiff {
    QString path;
    QString kind;
    QString content;
    int additions = 0;
    int deletions = 0;
  };

  static std::vector<FileDiff> parseUnifiedDiff(const QString &diff);
  void showSelectedFile();
  void showExpanded();

  QLabel *summary = nullptr;
  QLabel *authority = nullptr;
  QListWidget *files = nullptr;
  QPlainTextEdit *diff = nullptr;
  QPushButton *copyButton = nullptr;
  QPushButton *expandButton = nullptr;
  std::vector<FileDiff> fileDiffs;
  QByteArray contentFingerprint;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_DIFFVIEWER_H
