// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ConnectionDialog.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace codexui::codex {
namespace {

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto iterator = object.find(key);
  return iterator != object.end() && iterator->is_string()
             ? iterator->get<std::string>()
             : std::string{};
}

QLabel *dialogLabel(QString value, const char *kind) {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setWordWrap(true);
  return label;
}

} // namespace

ConnectionDialog::ConnectionDialog(nlohmann::json settings, QWidget *parent)
    : QDialog(parent), settings(std::move(settings)) {
  setModal(true);
  setWindowTitle(QStringLiteral("Bridge connection"));
  resize(520, 410);
  setMinimumWidth(460);

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(24, 22, 24, 20);
  root->setSpacing(14);
  root->addWidget(dialogLabel(QStringLiteral("Bridge connection"), "heading"));
  root->addWidget(dialogLabel(
      QStringLiteral(
          "Command-line and SNode.C configuration provide the startup "
          "defaults. These values override the current CodexUI session only."),
      "muted"));

  auto *form = new QFormLayout;
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(12);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  transport = new QComboBox;
  const nlohmann::json available =
      this->settings.value("available", nlohmann::json::array());
  if (available.is_array()) {
    for (const auto &entry : available) {
      transport->addItem(text(stringValue(entry, "label")),
                         text(stringValue(entry, "key")));
    }
  }
  const QString selected = text(stringValue(this->settings, "selected"));
  const int selectedIndex = transport->findData(selected);
  if (selectedIndex >= 0)
    transport->setCurrentIndex(selectedIndex);
  form->addRow(QStringLiteral("Transport"), transport);

  address = new QLineEdit;
  addressLabel = new QLabel;
  form->addRow(addressLabel, address);
  port = new QSpinBox;
  port->setRange(1, 65535);
  portLabel = new QLabel(QStringLiteral("Port"));
  form->addRow(portLabel, port);
  webSocketPath = new QLineEdit;
  webSocketPathLabel = new QLabel(QStringLiteral("WebSocket path"));
  form->addRow(webSocketPathLabel, webSocketPath);
  root->addLayout(form);

  tlsNotice =
      dialogLabel(QStringLiteral("TLS certificates and verification continue "
                                 "to use the effective SNode.C configuration."),
                  "meta");
  root->addWidget(tlsNotice);
  errorLabel = dialogLabel({}, "meta");
  errorLabel->setStyleSheet(QStringLiteral("color:#982f3d;"));
  errorLabel->hide();
  root->addWidget(errorLabel);
  root->addStretch();

  auto *footer = new QHBoxLayout;
  footer->addStretch();
  auto *cancel = new QPushButton(QStringLiteral("Cancel"));
  cancel->setProperty("kind", "subtle");
  cancel->setFixedHeight(34);
  auto *apply = new QPushButton(QStringLiteral("Apply and connect"));
  apply->setProperty("kind", "primary");
  apply->setFixedHeight(34);
  footer->addWidget(cancel);
  footer->addWidget(apply);
  root->addLayout(footer);

  connect(transport, &QComboBox::currentIndexChanged, this,
          [this] { loadTransport(); });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(apply, &QPushButton::clicked, this, [this] { acceptSelection(); });
  loadTransport();
}

nlohmann::json ConnectionDialog::selection() const {
  const QString key = transport->currentData().toString();
  const nlohmann::json available =
      settings.value("available", nlohmann::json::array());
  std::string kind;
  for (const auto &entry : available) {
    if (text(stringValue(entry, "key")) == key) {
      kind = stringValue(entry, "kind");
      break;
    }
  }
  nlohmann::json result{{"transport", key.toStdString()}};
  if (kind == "unix") {
    result["path"] = address->text().trimmed().toStdString();
  } else if (kind == "rfcomm") {
    result["address"] = address->text().trimmed().toStdString();
    result["channel"] = port->value();
  } else {
    result["host"] = address->text().trimmed().toStdString();
    result["port"] = port->value();
    if (kind == "websocket")
      result["webSocketPath"] = webSocketPath->text().trimmed().toStdString();
  }
  return result;
}

void ConnectionDialog::loadTransport() {
  const QString key = transport->currentData().toString();
  const nlohmann::json available =
      settings.value("available", nlohmann::json::array());
  nlohmann::json selected = nlohmann::json::object();
  for (const auto &entry : available) {
    if (text(stringValue(entry, "key")) == key) {
      selected = entry;
      break;
    }
  }
  const std::string kind = stringValue(selected, "kind");
  const bool network = kind == "network" || kind == "websocket";
  const bool bluetooth = kind == "rfcomm";
  addressLabel->setText(kind == "unix" ? QStringLiteral("Socket path")
                        : bluetooth    ? QStringLiteral("Bluetooth address")
                                       : QStringLiteral("Host"));
  address->setText(text(kind == "unix" ? stringValue(selected, "path")
                        : bluetooth    ? stringValue(selected, "address")
                                       : stringValue(selected, "host")));
  portLabel->setText(bluetooth ? QStringLiteral("Channel")
                               : QStringLiteral("Port"));
  port->setRange(bluetooth ? 1 : 1, bluetooth ? 30 : 65535);
  port->setValue(selected.value(bluetooth ? "channel" : "port", 1));
  port->setVisible(network || bluetooth);
  portLabel->setVisible(network || bluetooth);
  const bool webSocket = kind == "websocket";
  webSocketPath->setText(text(stringValue(selected, "webSocketPath")));
  webSocketPath->setVisible(webSocket);
  webSocketPathLabel->setVisible(webSocket);
  tlsNotice->setVisible(selected.value("tls", false));
  errorLabel->hide();
}

void ConnectionDialog::acceptSelection() {
  const nlohmann::json selected = selection();
  const std::string kind = [&] {
    const nlohmann::json available =
        settings.value("available", nlohmann::json::array());
    for (const auto &entry : available) {
      if (stringValue(entry, "key") == stringValue(selected, "transport"))
        return stringValue(entry, "kind");
    }
    return std::string{};
  }();
  const QString endpoint =
      text(stringValue(selected, kind == "unix"     ? "path"
                                 : kind == "rfcomm" ? "address"
                                                    : "host"));
  if (endpoint.trimmed().isEmpty()) {
    errorLabel->setText(QStringLiteral("Enter a connection endpoint."));
    errorLabel->show();
    return;
  }
  if (kind == "websocket" &&
      !text(stringValue(selected, "webSocketPath")).startsWith('/')) {
    errorLabel->setText(
        QStringLiteral("The WebSocket path must start with '/'."));
    errorLabel->show();
    return;
  }
  accept();
}

} // namespace codexui::codex
