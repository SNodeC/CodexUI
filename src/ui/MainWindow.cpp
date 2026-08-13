// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/MainWindow.h"

#include "ui/UiStyle.h"
#include "ui/WorkbenchWidget.h"

#include <QApplication>
#include <QFont>

namespace codexui {

MainWindow::MainWindow(FrontendSession& frontendSession, QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("CodexUI — Codex Workbench"));
    setMinimumSize(1100, 700);
    resize(1536, 960);

    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(12);
    qApp->setFont(font);
    qApp->setStyleSheet(UiStyle::applicationStyleSheet());

    setCentralWidget(new WorkbenchWidget(frontendSession, this));
}

} // namespace codexui
