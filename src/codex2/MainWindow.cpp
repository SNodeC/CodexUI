// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex2/MainWindow.h"

#include "codex2/WorkbenchWidget.h"
#include "ui/UiStyle.h"

#include <QApplication>
#include <QFont>

namespace codexui::codex2 {

MainWindow::MainWindow(FrontendSession &session, QWidget *parent)
    : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("CodexUI - Codex Workbench"));
  setMinimumSize(1100, 700);
  resize(1536, 960);

  QFont font(QStringLiteral("Inter"));
  font.setPixelSize(12);
  qApp->setFont(font);
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  setCentralWidget(new WorkbenchWidget(session, this));
}

} // namespace codexui::codex2
