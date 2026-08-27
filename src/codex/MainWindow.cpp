// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/MainWindow.h"

#include "codex/ShellWidget.h"
#include "codex/ui/BrandMark.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>

namespace codexui::codex {

MainWindow::MainWindow(FrontendSession &session, QWidget *parent)
    : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("CodexUI"));
  setMinimumSize(1100, 700);
  resize(1536, 960);

  const QIcon applicationIcon = codexui::BrandMark::icon();
  qApp->setWindowIcon(applicationIcon);
  setWindowIcon(applicationIcon);
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  setCentralWidget(new ShellWidget(session, this));
}

} // namespace codexui::codex
