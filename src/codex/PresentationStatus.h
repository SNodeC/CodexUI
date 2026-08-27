// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PRESENTATIONSTATUS_H
#define CODEXUI_CODEX_PRESENTATIONSTATUS_H

#include <string_view>

namespace codexui::codex {

enum class StatusKind {
  Unknown,
  Active,
  Completed,
  Failed,
  Interrupted,
};

struct PresentationStatus {
  StatusKind kind;
  std::string_view text;
  std::string_view tone;
};

constexpr PresentationStatus classifyStatus(std::string_view status) noexcept {
  if (status == "active" || status == "inProgress" || status == "running" ||
      status == "started")
    return {StatusKind::Active, "Running", "active"};
  if (status == "completed" || status == "idle")
    return {StatusKind::Completed, "Completed", "success"};
  if (status == "failed" || status == "systemError")
    return {StatusKind::Failed, "Failed", "danger"};
  if (status == "interrupted")
    return {StatusKind::Interrupted, "Interrupted", "warning"};
  return {StatusKind::Unknown, status.empty() ? "Unknown" : status,
          std::string_view{}};
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
