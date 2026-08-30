// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PRESENTATIONSTATUS_H
#define CODEXUI_CODEX_PRESENTATIONSTATUS_H

#include <cctype>
#include <string>
#include <string_view>

namespace codexui::codex {

enum class StatusKind {
  Unknown,
  Active,
  Completed,
  Failed,
  Interrupted,
  Pending,
  NotLoaded,
};

struct PresentationStatus {
  StatusKind kind;
  std::string_view text;
  std::string_view tone;
};

constexpr PresentationStatus classifyStatus(std::string_view status) noexcept {
  if (status == "active" || status == "inProgress" || status == "running" ||
      status == "started")
    return {StatusKind::Active, "running", "active"};
  if (status == "completed" || status == "idle")
    return {StatusKind::Completed, "completed", "success"};
  if (status == "failed" || status == "systemError")
    return {StatusKind::Failed, "failed", "danger"};
  if (status == "interrupted")
    return {StatusKind::Interrupted, "interrupted", "warning"};
  if (status == "pending")
    return {StatusKind::Pending, "pending", {}};
  if (status == "notLoaded")
    return {StatusKind::NotLoaded, "not loaded", {}};
  return {StatusKind::Unknown, status.empty() ? "unknown" : status,
          std::string_view{}};
}

inline std::string displayStatus(std::string_view status) {
  const PresentationStatus classified = classifyStatus(status);
  if (classified.kind != StatusKind::Unknown || status.empty())
    return std::string(classified.text);

  std::string result;
  result.reserve(status.size() + 4);
  bool pendingSpace = false;
  for (std::size_t index = 0; index < status.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(status[index]);
    if (std::isspace(character) || character == '-' || character == '_' ||
        character == '.' || character == '/') {
      pendingSpace = !result.empty();
      continue;
    }
    const unsigned char previous =
        index == 0 ? 0 : static_cast<unsigned char>(status[index - 1]);
    const unsigned char next =
        index + 1 == status.size()
            ? 0
            : static_cast<unsigned char>(status[index + 1]);
    const bool upper = std::isupper(character);
    const bool boundary =
        upper && (std::islower(previous) || std::isdigit(previous) ||
                  (std::isupper(previous) && std::islower(next)));
    if ((pendingSpace || boundary) && !result.empty() && result.back() != ' ')
      result.push_back(' ');
    result.push_back(static_cast<char>(std::tolower(character)));
    pendingSpace = false;
  }
  return result.empty() ? std::string("unknown") : result;
}

constexpr bool isActiveStatus(std::string_view status) noexcept {
  return classifyStatus(status).kind == StatusKind::Active;
}

constexpr bool isTerminalTurnStatus(std::string_view status) noexcept {
  const StatusKind kind = classifyStatus(status).kind;
  return kind == StatusKind::Completed || kind == StatusKind::Failed ||
         kind == StatusKind::Interrupted;
}

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PRESENTATIONSTATUS_H
