// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_CONNECTIONDIALOG_H
#define CODEXUI_CODEX_CONNECTIONDIALOG_H

#include <nlohmann/json.hpp>

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace codexui::codex {

class ConnectionDialog final : public QDialog {
public:
  explicit ConnectionDialog(nlohmann::json settings, QWidget *parent = nullptr);

  [[nodiscard]] nlohmann::json selection() const;

private:
  void loadTransport();
  void acceptSelection();

  nlohmann::json settings;
  QComboBox *transport = nullptr;
  QLineEdit *address = nullptr;
  QSpinBox *port = nullptr;
  QLineEdit *webSocketPath = nullptr;
  QLabel *addressLabel = nullptr;
  QLabel *portLabel = nullptr;
  QLabel *webSocketPathLabel = nullptr;
  QLabel *tlsNotice = nullptr;
  QLabel *errorLabel = nullptr;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_CONNECTIONDIALOG_H
