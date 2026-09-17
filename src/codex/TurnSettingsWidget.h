// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_TURNSETTINGSWIDGET_H
#define CODEXUI_CODEX_TURNSETTINGSWIDGET_H

#include "codex/TurnSettingsPolicy.h"

#include <QWidget>

#include <array>

class QComboBox;
class QLineEdit;
class QToolButton;

namespace codexui::codex {

class TurnSettingsWidget final : public QWidget {
public:
  explicit TurnSettingsWidget(TurnSettingsPolicy &settings,
                              QWidget *parent = nullptr);

  void setContext(TurnSettingsContext context);

private:
  void markTouched(TurnSettingField field);
  void render();
  void refreshModels();
  void refreshModelOptions();
  void refreshPermissionProfiles();
  void refreshAccessCompatibility();
  void refreshMoreIndicator();
  [[nodiscard]] QComboBox *combo(TurnSettingField field) const noexcept {
    return combos[static_cast<std::size_t>(field)];
  }

  TurnSettingsPolicy &settings;
  std::array<QComboBox *, TurnSettingFieldCount> combos{};
  QLineEdit *cwd = nullptr;
  QToolButton *more = nullptr;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_TURNSETTINGSWIDGET_H
