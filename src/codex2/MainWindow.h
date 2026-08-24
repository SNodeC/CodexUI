// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX2_MAINWINDOW_H
#define CODEXUI_CODEX2_MAINWINDOW_H

#include <QMainWindow>

namespace codexui::codex2 {

class FrontendSession;

class MainWindow final : public QMainWindow {
public:
  explicit MainWindow(FrontendSession &session, QWidget *parent = nullptr);
};

} // namespace codexui::codex2

#endif // CODEXUI_CODEX2_MAINWINDOW_H
