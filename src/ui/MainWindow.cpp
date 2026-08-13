// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/MainWindow.h"

#include <QLabel>
#include <QStatusBar>

namespace codexui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("CodexUI");
    resize(1024, 720);

    auto* placeholder = new QLabel("Not connected", this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);

    statusBar()->showMessage("Ready");
}

} // namespace codexui
