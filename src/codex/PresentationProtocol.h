// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PRESENTATIONPROTOCOL_H
#define CODEXUI_CODEX_PRESENTATIONPROTOCOL_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace codexui::codex::presentation {

inline constexpr std::string_view ProtocolName = "codexui.presentation";
inline constexpr std::uint32_t ProtocolVersion = 1;

[[nodiscard]] inline constexpr bool
isThreadHydrationAction(std::string_view action) noexcept {
  return action == "thread.read" || action == "thread.resume";
}

enum class Authority {
  None,
  Merge,
  Replace,
  Remove,
};

[[nodiscard]] std::string_view authorityName(Authority authority) noexcept;

[[nodiscard]] nlohmann::json
command(std::string action, nlohmann::json data = nlohmann::json::object(),
        std::string correlationId = {});

[[nodiscard]] nlohmann::json
result(std::uint64_t sequence, std::uint64_t generation, std::string action,
       std::string correlationId, bool ok, nlohmann::json data,
       Authority authority = Authority::None,
       nlohmann::json scope = nlohmann::json::object());

[[nodiscard]] nlohmann::json
event(std::uint64_t sequence, std::uint64_t generation, std::string type,
      nlohmann::json data = nlohmann::json::object(),
      Authority authority = Authority::None,
      nlohmann::json scope = nlohmann::json::object());

[[nodiscard]] bool isPresentationFrame(const nlohmann::json &value) noexcept;
[[nodiscard]] std::string stringMember(const nlohmann::json &value,
                                       const char *name);
[[nodiscard]] nlohmann::json member(const nlohmann::json &value,
                                    const char *name,
                                    nlohmann::json fallback = nullptr);

} // namespace codexui::codex::presentation

#endif // CODEXUI_CODEX_PRESENTATIONPROTOCOL_H
