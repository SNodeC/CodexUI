// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MAINWINDOW_H
#define CODEXUI_CODEX_MAINWINDOW_H

#include <QMainWindow>

namespace codexui::codex {

class FrontendSession;

class MainWindow final : public QMainWindow {
public:
  explicit MainWindow(FrontendSession &session, QWidget *parent = nullptr);
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_MAINWINDOW_H
