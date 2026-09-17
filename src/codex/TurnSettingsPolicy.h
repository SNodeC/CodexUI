// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_TURNSETTINGSPOLICY_H
#define CODEXUI_CODEX_TURNSETTINGSPOLICY_H

#include <nlohmann/json.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace codexui::codex {

inline constexpr char DefaultTurnSetting[] = "default";

enum class TurnSettingField : std::uint8_t {
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

inline constexpr std::size_t TurnSettingFieldCount =
    static_cast<std::size_t>(TurnSettingField::Count);
enum class TurnSettingsScope : std::uint8_t { Thread, Turn };
using TurnSettingMask = std::bitset<TurnSettingFieldCount>;
using TurnSettingEpochs = std::array<std::uint64_t, TurnSettingFieldCount>;

struct TurnSettingValues final {
  TurnSettingValues() {
    fields.fill(std::string(DefaultTurnSetting));
    fields[static_cast<std::size_t>(TurnSettingField::Workspace)].clear();
  }

  [[nodiscard]] const std::string &operator[](TurnSettingField field) const {
    return fields[static_cast<std::size_t>(field)];
  }
  [[nodiscard]] std::string &operator[](TurnSettingField field) {
    return fields[static_cast<std::size_t>(field)];
  }

  std::array<std::string, TurnSettingFieldCount> fields;
  bool operator==(const TurnSettingValues &) const = default;
};

struct TurnSettingChoice final {
  std::string value;
  std::string label;
  std::string description;

  bool operator==(const TurnSettingChoice &) const = default;
};

struct TurnSettingModel final {
  TurnSettingChoice choice;
  std::vector<std::string> reasoningEfforts;
  std::string defaultReasoningEffort;
  std::vector<TurnSettingChoice> serviceTiers;
  std::string defaultServiceTier;
  bool isDefault = false;
  bool supportsPersonality = true;

  bool operator==(const TurnSettingModel &) const = default;
};

// One lock-free, toolkit-neutral transaction from NodeGraph to settings policy.
// `acknowledged` retains the last provider acknowledgement epoch per field so
// coalesced updates clear exactly the authored values they acknowledge.
struct TurnSettingsContext final {
  std::string identity;
  std::uint64_t providerAuthorityRevision = 0;
  std::uint64_t threadIncarnation = 0;
  TurnSettingValues canonical;
  TurnSettingEpochs acknowledged{};
  std::vector<TurnSettingModel> models;
  std::vector<TurnSettingChoice> permissionProfiles;

  bool operator==(const TurnSettingsContext &) const = default;
};

// Sole native owner of local setting drafts, compatibility transitions and
// start-option serialization. It is deliberately independent of QObject and
// widgets so controls can only project and edit this value state.
class TurnSettingsPolicy final {
public:
  [[nodiscard]] bool setContext(TurnSettingsContext context);
  void change(TurnSettingField field, std::string value);
  void promote(std::string_view sourceIdentity,
               std::string_view canonicalIdentity,
               std::uint64_t canonicalIncarnation);
  void forget(std::string_view identity);
  void forget(std::string_view identity, std::uint64_t incarnation);

  [[nodiscard]] const TurnSettingValues &values() const noexcept;
  [[nodiscard]] bool touched(TurnSettingField field) const noexcept;
  [[nodiscard]] const TurnSettingsContext &context() const noexcept {
    return context_;
  }
  [[nodiscard]] const TurnSettingModel *selectedModel() const noexcept;
  [[nodiscard]] bool personalityEnabled() const noexcept;
  [[nodiscard]] bool networkEnabled() const noexcept;

  [[nodiscard]] std::string workspace(std::string_view fallback) const;
  [[nodiscard]] nlohmann::json startOptions(TurnSettingsScope scope) const;

private:
  struct Draft final {
    TurnSettingValues values;
    TurnSettingMask touched;
    TurnSettingEpochs acknowledged{};
    std::uint64_t threadIncarnation = 0;
  };

  [[nodiscard]] Draft *draft() noexcept;
  [[nodiscard]] const Draft *draft() const noexcept;
  void normalize(Draft &draft) const;

  TurnSettingsContext context_;
  std::unordered_map<std::string, Draft> drafts_;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_TURNSETTINGSPOLICY_H
