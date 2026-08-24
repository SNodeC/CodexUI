// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NEWTHREADDIALOG_H
#define CODEXUI_CODEX_NEWTHREADDIALOG_H

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace codexui::codex {

struct NewThreadDraft {
  QString workspace;
  QString name;
  QString baseInstructions;
  QString developerInstructions;
  bool ephemeral = false;
};

class NewThreadDialog final : public QDialog {
public:
  explicit NewThreadDialog(QString initialWorkspace, QWidget *parent = nullptr);

  [[nodiscard]] NewThreadDraft draft() const;

private:
  void chooseWorkspace();
  void acceptDraft();

  QLineEdit *workspace = nullptr;
  QLineEdit *name = nullptr;
  QPlainTextEdit *baseInstructions = nullptr;
  QPlainTextEdit *developerInstructions = nullptr;
  QCheckBox *ephemeral = nullptr;
  QLabel *errorLabel = nullptr;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_NEWTHREADDIALOG_H
