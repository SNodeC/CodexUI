// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/NodeGraphJson.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace codexui::codex {
namespace {

nodegraph::Value::Array arrayFromJson(const nlohmann::json &value) {
  nodegraph::Value::Array result;
  result.reserve(value.size());
  for (const nlohmann::json &element : value)
    result.emplace_back(valueFromJson(element));
  return result;
}

nodegraph::Value::Object convertObject(const nlohmann::json &value) {
  nodegraph::Value::Object result;
  for (auto member = value.cbegin(); member != value.cend(); ++member)
    result.emplace(member.key(), valueFromJson(member.value()));
  return result;
}

} // namespace

nodegraph::Value valueFromJson(const nlohmann::json &value) {
  if (value.is_null())
    return nullptr;
  if (value.is_boolean())
    return value.get<nlohmann::json::boolean_t>();
  if (value.is_number_unsigned())
    return value.get<nlohmann::json::number_unsigned_t>();
  if (value.is_number_integer())
    return value.get<nlohmann::json::number_integer_t>();
  if (value.is_number_float())
    return value.get<nlohmann::json::number_float_t>();
  if (value.is_string())
    return value.get<nlohmann::json::string_t>();
  if (value.is_array())
    return arrayFromJson(value);
  if (value.is_object())
    return convertObject(value);
  throw std::invalid_argument("unsupported JSON value for the node graph");
}

nodegraph::Value::Object objectFromJson(const nlohmann::json &value) {
  if (!value.is_object())
    throw std::invalid_argument("node graph payload must be a JSON object");
  return convertObject(value);
}

nodegraph::ProtocolRequestId requestIdFromJson(const nlohmann::json &value) {
  if (value.is_string())
    return nodegraph::ProtocolRequestId(value.get<nlohmann::json::string_t>());
  if (value.is_number_unsigned()) {
    const std::uint64_t numeric =
        value.get<nlohmann::json::number_unsigned_t>();
    if (numeric >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::out_of_range(
          "JSON-RPC request id exceeds signed 64-bit range");
    return nodegraph::ProtocolRequestId(static_cast<std::int64_t>(numeric));
  }
  if (value.is_number_integer())
    return nodegraph::ProtocolRequestId(
        value.get<nlohmann::json::number_integer_t>());
  throw std::invalid_argument(
      "JSON-RPC request id must be a string or integer");
}

nlohmann::json jsonFromValue(const nodegraph::Value &value) {
  if (value.isNull())
    return nullptr;
  if (const bool *boolean = value.asBool())
    return *boolean;
  if (const std::int64_t *integer = value.asInt64())
    return *integer;
  if (const std::uint64_t *integer = value.asUInt64())
    return *integer;
  if (const double *number = value.asDouble())
    return *number;
  if (const std::string *string = value.asString())
    return *string;
  if (const nodegraph::Value::Array *array = value.asArray()) {
    nlohmann::json::array_t result;
    result.reserve(array->size());
    for (const nodegraph::Value &element : *array)
      result.emplace_back(jsonFromValue(element));
    return nlohmann::json(std::move(result));
  }
  if (const nodegraph::Value::Object *object = value.asObject()) {
    nlohmann::json::object_t result;
    for (const auto &[key, element] : *object)
      result.emplace(key, jsonFromValue(element));
    return nlohmann::json(std::move(result));
  }
  throw std::logic_error("node graph Value holds no supported alternative");
}

nlohmann::json
jsonFromRequestId(const nodegraph::ProtocolRequestId &requestId) {
  return std::visit([](const auto &value) -> nlohmann::json { return value; },
                    requestId.value);
}

} // namespace codexui::codex
