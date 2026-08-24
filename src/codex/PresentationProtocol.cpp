// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PresentationProtocol.h"

#include <utility>

namespace codexui::codex::presentation {
namespace {

nlohmann::json baseFrame(std::string kind) {
  return {{"protocol", ProtocolName},
          {"version", ProtocolVersion},
          {"kind", std::move(kind)}};
}

void addAuthorityAndScope(nlohmann::json &frame, Authority authority,
                          nlohmann::json scope) {
  frame["authority"] = authorityName(authority);
  if (scope.is_object() && !scope.empty())
    frame["scope"] = std::move(scope);
}

} // namespace

std::string_view authorityName(Authority authority) noexcept {
  switch (authority) {
  case Authority::Merge:
    return "merge";
  case Authority::Replace:
    return "replace";
  case Authority::Remove:
    return "remove";
  case Authority::None:
    return "none";
  }
  return "none";
}

nlohmann::json command(std::string action, nlohmann::json data,
                       std::string correlationId) {
  nlohmann::json frame = baseFrame("command");
  frame["action"] = std::move(action);
  frame["data"] = std::move(data);
  if (!correlationId.empty())
    frame["correlationId"] = std::move(correlationId);
  return frame;
}

nlohmann::json result(std::uint64_t sequence, std::uint64_t generation,
                      std::string action, std::string correlationId, bool ok,
                      nlohmann::json data, Authority authority,
                      nlohmann::json scope) {
  nlohmann::json frame = baseFrame("result");
  frame["sequence"] = sequence;
  frame["generation"] = generation;
  frame["action"] = std::move(action);
  frame["correlationId"] = std::move(correlationId);
  frame["ok"] = ok;
  frame[ok ? "data" : "error"] = std::move(data);
  addAuthorityAndScope(frame, authority, std::move(scope));
  return frame;
}

nlohmann::json event(std::uint64_t sequence, std::uint64_t generation,
                     std::string type, nlohmann::json data, Authority authority,
                     nlohmann::json scope) {
  nlohmann::json frame = baseFrame("event");
  frame["sequence"] = sequence;
  frame["generation"] = generation;
  frame["type"] = std::move(type);
  frame["data"] = std::move(data);
  addAuthorityAndScope(frame, authority, std::move(scope));
  return frame;
}

bool isPresentationFrame(const nlohmann::json &value) noexcept {
  return value.is_object() && stringMember(value, "protocol") == ProtocolName &&
         value.value("version", 0U) == ProtocolVersion;
}

std::string stringMember(const nlohmann::json &value, const char *name) {
  if (!value.is_object())
    return {};
  const auto iterator = value.find(name);
  return iterator != value.end() && iterator->is_string()
             ? iterator->get<std::string>()
             : std::string{};
}

nlohmann::json member(const nlohmann::json &value, const char *name,
                      nlohmann::json fallback) {
  if (!value.is_object())
    return fallback;
  const auto iterator = value.find(name);
  return iterator == value.end() ? std::move(fallback) : *iterator;
}

} // namespace codexui::codex::presentation
