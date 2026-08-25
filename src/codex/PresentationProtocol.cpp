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
  if (!value.is_object())
    return false;
  const auto protocol = value.find("protocol");
  if (protocol == value.end() || !protocol->is_string() ||
      protocol->get_ref<const nlohmann::json::string_t &>() != ProtocolName)
    return false;
  const auto version = value.find("version");
  if (version == value.end() || !version->is_number_unsigned() ||
      version->get_ref<const nlohmann::json::number_unsigned_t &>() !=
          ProtocolVersion)
    return false;
  const auto kind = value.find("kind");
  if (kind == value.end() || !kind->is_string())
    return false;
  const std::string &kindValue =
      kind->get_ref<const nlohmann::json::string_t &>();
  const auto stringField = [&value](const char *name) {
    const auto member = value.find(name);
    return member != value.end() && member->is_string() &&
           !member->get_ref<const nlohmann::json::string_t &>().empty();
  };
  if (kindValue == "command") {
    const auto data = value.find("data");
    return stringField("action") && data != value.end() && data->is_object();
  }
  if (kindValue != "event" && kindValue != "result")
    return false;
  const auto sequence = value.find("sequence");
  const auto generation = value.find("generation");
  const auto authority = value.find("authority");
  if (sequence == value.end() || !sequence->is_number_unsigned() ||
      generation == value.end() || !generation->is_number_unsigned() ||
      authority == value.end() || !authority->is_string())
    return false;
  const std::string &authorityValue =
      authority->get_ref<const nlohmann::json::string_t &>();
  if (authorityValue != "none" && authorityValue != "merge" &&
      authorityValue != "replace" && authorityValue != "remove")
    return false;
  const auto scope = value.find("scope");
  if (scope != value.end() && !scope->is_object())
    return false;
  if (kindValue == "event") {
    const auto data = value.find("data");
    return stringField("type") && data != value.end() && data->is_object();
  }
  const auto ok = value.find("ok");
  if (!stringField("action") || !stringField("correlationId") ||
      ok == value.end() || !ok->is_boolean())
    return false;
  return ok->get_ref<const nlohmann::json::boolean_t &>()
             ? value.contains("data") && !value.contains("error")
             : value.contains("error") && !value.contains("data");
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
