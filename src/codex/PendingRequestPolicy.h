// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PENDINGREQUESTPOLICY_H
#define CODEXUI_CODEX_PENDINGREQUESTPOLICY_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace codexui::codex {

struct PendingRequestDescriptor {
  std::string id;
  std::string kind;
  std::string threadId;
  std::uint64_t generation = 0;
  nlohmann::json raw = nlohmann::json::object();

  bool operator==(const PendingRequestDescriptor &) const = default;
};

struct PendingRequestResponse {
  nlohmann::json result = nlohmann::json::object();
  nlohmann::json error = nullptr;
};

class PendingRequestPolicy final {
public:
  PendingRequestPolicy() = delete;

  [[nodiscard]] static std::string title(std::string_view kind);
  [[nodiscard]] static std::string dialogTitle(std::string_view kind);
  [[nodiscard]] static std::string detail(std::string_view requestId,
                                          std::string_view threadId,
                                          const nlohmann::json &request);

  [[nodiscard]] static bool
  supportsDirectAccept(std::string_view kind) noexcept;
  [[nodiscard]] static std::string directAcceptLabel(std::string_view kind);

  [[nodiscard]] static PendingRequestResponse
  responseForSubmission(std::string_view kind, const nlohmann::json &request,
                        std::string decision = {},
                        nlohmann::json input = nullptr);
  [[nodiscard]] static PendingRequestResponse
  negativeResponse(std::string_view kind, const nlohmann::json &request);
  [[nodiscard]] static PendingRequestResponse
  positiveResponse(std::string_view kind, const nlohmann::json &request);
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PENDINGREQUESTPOLICY_H
