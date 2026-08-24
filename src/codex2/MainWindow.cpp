// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex2/MainWindow.h"

#ifdef CODEXUI_DEVELOPMENT_HARNESS
#include "codex2/WorkbenchWidget.h"
#else
#include "codex2/ShellWidget.h"
#endif
#include "ui/UiStyle.h"

#include <QApplication>
#include <QFont>

namespace codexui::codex2 {

MainWindow::MainWindow(FrontendSession &session, QWidget *parent)
    : QMainWindow(parent) {
#ifdef CODEXUI_DEVELOPMENT_HARNESS
  setWindowTitle(QStringLiteral("CodexUI - codex2 Harness"));
#else
  setWindowTitle(QStringLiteral("CodexUI"));
#endif
  setMinimumSize(1100, 700);
  resize(1536, 960);

  QFont font(QStringLiteral("Inter"));
  font.setPixelSize(12);
  qApp->setFont(font);
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
#ifdef CODEXUI_DEVELOPMENT_HARNESS
  setCentralWidget(new WorkbenchWidget(session, this));
#else
  setCentralWidget(new ShellWidget(session, this));
#endif
}

} // namespace codexui::codex2
