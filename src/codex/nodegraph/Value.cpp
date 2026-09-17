// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/Value.h"

#include <limits>
#include <utility>

namespace codexui::nodegraph {

Value::Value(std::nullptr_t) noexcept : storage_(std::monostate{}) {}

Value::Value(bool value) noexcept : storage_(value) {}

Value::Value(std::int64_t value) noexcept : storage_(value) {}

Value::Value(std::uint64_t value) noexcept : storage_(value) {}

Value::Value(double value) noexcept : storage_(value) {}

Value::Value(const char *value) : storage_(std::string(value ? value : "")) {}

Value::Value(std::string value) : storage_(std::move(value)) {}

Value::Value(std::string_view value) : storage_(std::string(value)) {}

Value::Value(Array value) : storage_(std::move(value)) {}

Value::Value(Object value) : storage_(std::move(value)) {}

bool Value::isNull() const noexcept {
  return std::holds_alternative<std::monostate>(storage_);
}

bool Value::isBool() const noexcept {
  return std::holds_alternative<bool>(storage_);
}

bool Value::isSigned() const noexcept {
  return std::holds_alternative<std::int64_t>(storage_);
}

bool Value::isUnsigned() const noexcept {
  return std::holds_alternative<std::uint64_t>(storage_);
}

bool Value::isDouble() const noexcept {
  return std::holds_alternative<double>(storage_);
}

bool Value::isString() const noexcept {
  return std::holds_alternative<std::string>(storage_);
}

bool Value::isArray() const noexcept {
  return std::holds_alternative<Array>(storage_);
}

bool Value::isObject() const noexcept {
  return std::holds_alternative<Object>(storage_);
}

bool *Value::asBool() noexcept { return std::get_if<bool>(&storage_); }

const bool *Value::asBool() const noexcept {
  return std::get_if<bool>(&storage_);
}

std::int64_t *Value::asInt64() noexcept {
  return std::get_if<std::int64_t>(&storage_);
}

const std::int64_t *Value::asInt64() const noexcept {
  return std::get_if<std::int64_t>(&storage_);
}

std::uint64_t *Value::asUInt64() noexcept {
  return std::get_if<std::uint64_t>(&storage_);
}

const std::uint64_t *Value::asUInt64() const noexcept {
  return std::get_if<std::uint64_t>(&storage_);
}

double *Value::asDouble() noexcept { return std::get_if<double>(&storage_); }

const double *Value::asDouble() const noexcept {
  return std::get_if<double>(&storage_);
}

std::string *Value::asString() noexcept {
  return std::get_if<std::string>(&storage_);
}

const std::string *Value::asString() const noexcept {
  return std::get_if<std::string>(&storage_);
}

Value::Array *Value::asArray() noexcept {
  return std::get_if<Array>(&storage_);
}

const Value::Array *Value::asArray() const noexcept {
  return std::get_if<Array>(&storage_);
}

Value::Object *Value::asObject() noexcept {
  return std::get_if<Object>(&storage_);
}

const Value::Object *Value::asObject() const noexcept {
  return std::get_if<Object>(&storage_);
}

Value *Value::find(std::string_view key) noexcept {
  Object *object = asObject();
  if (!object)
    return nullptr;
  const auto member = object->find(key);
  return member == object->end() ? nullptr : &member->second;
}

const Value *Value::find(std::string_view key) const noexcept {
  const Object *object = asObject();
  if (!object)
    return nullptr;
  const auto member = object->find(key);
  return member == object->end() ? nullptr : &member->second;
}

const Value *valueMember(const Value::Object &object,
                         std::string_view key) noexcept {
  const auto member = object.find(key);
  return member == object.end() ? nullptr : &member->second;
}

std::string exactStringFromValue(const Value *value) {
  const std::string *text = value ? value->asString() : nullptr;
  return text ? *text : std::string{};
}

std::string scalarTextFromValue(const Value *value) {
  if (const std::string *text = value ? value->asString() : nullptr)
    return *text;
  if (const std::int64_t *number = value ? value->asInt64() : nullptr)
    return std::to_string(*number);
  if (const std::uint64_t *number = value ? value->asUInt64() : nullptr)
    return std::to_string(*number);
  return {};
}

bool boolFromValue(const Value *value, bool fallback) noexcept {
  const bool *boolean = value ? value->asBool() : nullptr;
  return boolean ? *boolean : fallback;
}

std::optional<std::int64_t>
signedIntegerFromValue(const Value *value) noexcept {
  if (const std::int64_t *number = value ? value->asInt64() : nullptr)
    return *number;
  if (const std::uint64_t *number = value ? value->asUInt64() : nullptr;
      number && *number <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
    return static_cast<std::int64_t>(*number);
  return std::nullopt;
}

std::optional<std::uint64_t>
unsignedIntegerFromValue(const Value *value) noexcept {
  if (const std::uint64_t *number = value ? value->asUInt64() : nullptr)
    return *number;
  if (const std::int64_t *number = value ? value->asInt64() : nullptr;
      number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return std::nullopt;
}

} // namespace codexui::nodegraph
