// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX2_TURNSETTINGSWIDGET_H
#define CODEXUI_CODEX2_TURNSETTINGSWIDGET_H

#include <nlohmann/json.hpp>

#include <QWidget>

#include <array>
#include <string>

class QComboBox;
class QLineEdit;
class QMenu;
class QPushButton;

namespace codexui::codex2 {

class TurnSettingsWidget final : public QWidget {
public:
  explicit TurnSettingsWidget(QWidget *parent = nullptr);

  void setContext(std::string identity, const nlohmann::json &canonical,
                  const nlohmann::json &models,
                  const nlohmann::json &permissionProfiles);
  void setControlsEnabled(bool enabled);

  [[nodiscard]] std::string workspace(const std::string &fallback) const;
  [[nodiscard]] nlohmann::json threadStartOptions() const;
  [[nodiscard]] nlohmann::json turnStartOptions() const;

private:
  enum class Field : std::size_t {
    Model,
    Effort,
    Personality,
    Sandbox,
    Network,
    Approval,
    Reviewer,
    Workspace,
    PermissionProfile,
    ServiceTier,
    Summary,
    Collaboration,
    Count,
  };

  void markTouched(Field field);
  void resetFromCanonical(const nlohmann::json &canonical);
  void refreshModels(const nlohmann::json &models);
  void refreshModelOptions();
  void refreshPermissionProfiles(const nlohmann::json &profiles);
  void refreshAccessCompatibility();
  void refreshMoreIndicator();
  [[nodiscard]] bool touched(Field field) const noexcept;
  [[nodiscard]] QString value(const QComboBox *combo) const;
  [[nodiscard]] nlohmann::json sandboxPolicy() const;
  [[nodiscard]] nlohmann::json collaborationMode() const;

  std::string contextIdentity;
  nlohmann::json modelCatalog = nlohmann::json::array();
  std::array<bool, static_cast<std::size_t>(Field::Count)> touchedFields{};

  QComboBox *model = nullptr;
  QComboBox *effort = nullptr;
  QComboBox *personality = nullptr;
  QComboBox *sandbox = nullptr;
  QComboBox *network = nullptr;
  QComboBox *approval = nullptr;
  QComboBox *reviewer = nullptr;
  QLineEdit *cwd = nullptr;
  QComboBox *permissionProfile = nullptr;
  QComboBox *serviceTier = nullptr;
  QComboBox *summary = nullptr;
  QComboBox *collaboration = nullptr;
  QPushButton *more = nullptr;
  QMenu *moreMenu = nullptr;
};

} // namespace codexui::codex2

#endif // CODEXUI_CODEX2_TURNSETTINGSWIDGET_H
