// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/MainWindow.h"

#ifdef CODEXUI_DEVELOPMENT_HARNESS
#include "codex/WorkbenchWidget.h"
#else
#include "codex/ShellWidget.h"
#endif
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QFont>

namespace codexui::codex {

MainWindow::MainWindow(FrontendSession &session, QWidget *parent)
    : QMainWindow(parent) {
#ifdef CODEXUI_DEVELOPMENT_HARNESS
  setWindowTitle(QStringLiteral("CodexUI - codex Harness"));
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

} // namespace codexui::codex
