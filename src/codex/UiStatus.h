// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UISTATUS_H
#define CODEXUI_CODEX_UISTATUS_H

#include "codex/nodegraph/ProtocolUpdater.h"

#include <string>
#include <string_view>

namespace codexui::codex {

// A presentation status owns only forward-compatible unknown text. Known
// protocol aliases collapse to one typed value before reaching a renderer.
struct UiStatus final {
  nodegraph::NodeStatus semantic = nodegraph::NodeStatus::Unknown;
  std::string unknownText;

  UiStatus() = default;
  UiStatus(nodegraph::NodeStatus value) noexcept : semantic(value) {}

  [[nodiscard]] bool empty() const noexcept {
    return semantic == nodegraph::NodeStatus::Unknown && unknownText.empty();
  }

  bool operator==(const UiStatus &) const = default;
};

inline UiStatus statusFromNode(nodegraph::NodeStatus status,
                               std::string_view unknownText = {}) {
  UiStatus result{status};
  if (status == nodegraph::NodeStatus::Unknown)
    result.unknownText = unknownText;
  return result;
}

inline UiStatus statusFromState(const nodegraph::NodeState &state) {
  const std::string_view raw =
      nodegraph::statusTextFromValue(nodegraph::valueMember(state, "status"));
  return statusFromNode(state.status, raw);
}

inline std::string_view statusToken(const UiStatus &status) noexcept {
  switch (status.semantic) {
  case nodegraph::NodeStatus::Running:
    return "running";
  case nodegraph::NodeStatus::Completed:
    return "completed";
  case nodegraph::NodeStatus::Failed:
    return "failed";
  case nodegraph::NodeStatus::Interrupted:
    return "interrupted";
  case nodegraph::NodeStatus::Pending:
    return "pending";
  case nodegraph::NodeStatus::NotLoaded:
    return "notLoaded";
  case nodegraph::NodeStatus::Connected:
    return "connected";
  case nodegraph::NodeStatus::Disconnected:
    return "disconnected";
  case nodegraph::NodeStatus::Unknown:
    return status.unknownText;
  }
  return {};
}

constexpr bool asciiWhitespace(unsigned char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

constexpr bool asciiUpper(unsigned char value) noexcept {
  return value >= 'A' && value <= 'Z';
}

constexpr bool asciiLower(unsigned char value) noexcept {
  return value >= 'a' && value <= 'z';
}

constexpr bool asciiDigit(unsigned char value) noexcept {
  return value >= '0' && value <= '9';
}

inline std::string displayStatus(const UiStatus &status) {
  if (status.semantic != nodegraph::NodeStatus::Unknown) {
    if (status.semantic == nodegraph::NodeStatus::NotLoaded)
      return "not loaded";
    return std::string(statusToken(status));
  }
  const std::string_view raw = status.unknownText;
  if (raw.empty())
    return "unknown";

  std::string result;
  result.reserve(raw.size() + 4);
  bool pendingSpace = false;
  for (std::size_t index = 0; index < raw.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(raw[index]);
    if (asciiWhitespace(character) || character == '-' || character == '_' ||
        character == '.' || character == '/') {
      pendingSpace = !result.empty();
      continue;
    }
    const unsigned char previous =
        index == 0 ? 0 : static_cast<unsigned char>(raw[index - 1]);
    const unsigned char next = index + 1 == raw.size()
                                   ? 0
                                   : static_cast<unsigned char>(raw[index + 1]);
    const bool boundary = asciiUpper(character) &&
                          (asciiLower(previous) || asciiDigit(previous) ||
                           (asciiUpper(previous) && asciiLower(next)));
    if ((pendingSpace || boundary) && !result.empty() && result.back() != ' ')
      result.push_back(' ');
    result.push_back(asciiUpper(character)
                         ? static_cast<char>(character - 'A' + 'a')
                         : static_cast<char>(character));
    pendingSpace = false;
  }
  return result.empty() ? std::string("unknown") : result;
}

inline std::string_view statusTone(const UiStatus &status) noexcept {
  switch (status.semantic) {
  case nodegraph::NodeStatus::Running:
    return "active";
  case nodegraph::NodeStatus::Completed:
  case nodegraph::NodeStatus::Connected:
    return "success";
  case nodegraph::NodeStatus::Failed:
  case nodegraph::NodeStatus::Disconnected:
    return "danger";
  case nodegraph::NodeStatus::Interrupted:
    return "warning";
  case nodegraph::NodeStatus::Unknown:
  case nodegraph::NodeStatus::Pending:
  case nodegraph::NodeStatus::NotLoaded:
    return {};
  }
  return {};
}

constexpr bool isActiveStatus(const UiStatus &status) noexcept {
  return status.semantic == nodegraph::NodeStatus::Running;
}

constexpr bool isWorkingStatus(const UiStatus &status) noexcept {
  return status.semantic == nodegraph::NodeStatus::Pending ||
         status.semantic == nodegraph::NodeStatus::Running;
}

constexpr bool isTerminalTurnStatus(const UiStatus &status) noexcept {
  return status.semantic == nodegraph::NodeStatus::Completed ||
         status.semantic == nodegraph::NodeStatus::Failed ||
         status.semantic == nodegraph::NodeStatus::Interrupted;
}

inline UiStatus effectivePlanStepStatus(const UiStatus &stepStatus,
                                        const UiStatus &turnStatus,
                                        const UiStatus &threadStatus) {
  if (!isActiveStatus(stepStatus))
    return stepStatus;
  const UiStatus &outcome =
      isTerminalTurnStatus(turnStatus) ? turnStatus : threadStatus;
  return isTerminalTurnStatus(outcome) ? outcome : stepStatus;
}

} // namespace codexui::codex

#endif // CODEXUI_CODEX_UISTATUS_H
