// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_MAINWINDOW_H
#define CODEXUI_UI_MAINWINDOW_H

#include <QMainWindow>

namespace codexui {

class FrontendSession;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(FrontendSession& frontendSession, QWidget* parent = nullptr);
};

} // namespace codexui

#endif // CODEXUI_UI_MAINWINDOW_H
