// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_GREENFIELD_CODEX_SHELLWIDGET_H
#define CODEXUI_GREENFIELD_CODEX_SHELLWIDGET_H

#include <QWidget>

#include <memory>

namespace codexui::codex {

class FrontendSession;

// Production shell backed by the green-field middle-region implementation.
// The private implementation keeps protocol/application coordination out of
// the visual component interfaces.
class ShellWidget final : public QWidget {
public:
  explicit ShellWidget(FrontendSession &session, QWidget *parent = nullptr);
  ~ShellWidget() override;

  ShellWidget(const ShellWidget &) = delete;
  ShellWidget &operator=(const ShellWidget &) = delete;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace codexui::codex

#endif
