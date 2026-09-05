// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/NodeGraphJson.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

using codexui::codex::jsonFromRequestId;
using codexui::codex::jsonFromValue;
using codexui::codex::objectFromJson;
using codexui::codex::requestIdFromJson;
using codexui::codex::valueFromJson;
using codexui::nodegraph::ProtocolRequestId;
using codexui::nodegraph::Value;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

template <typename Exception, typename Operation>
bool throws(Operation &&operation) {
  try {
    operation();
  } catch (const Exception &) {
    return true;
  } catch (...) {
  }
  return false;
}

void everyDomAlternativeKeepsItsType() {
  const nlohmann::json source = nlohmann::json::object(
      {{"null", nullptr},
       {"boolean", true},
       {"signed", std::numeric_limits<std::int64_t>::min()},
       {"unsigned", std::numeric_limits<std::uint64_t>::max()},
       {"float", 0.125},
       {"string", std::string("embedded\0nul", 12)},
       {"array", nlohmann::json::array(
                     {nullptr, false, std::int64_t{-1}, std::uint64_t{2}, 3.5,
                      "tail", nlohmann::json::object({{"deep", 7}})})},
       {"object", nlohmann::json::object({{"nested", "value"}})}});

  const Value converted = valueFromJson(source);
  const Value::Object *object = converted.asObject();
  expect(object != nullptr && object->size() == source.size(),
         "a JSON object becomes one directly owned Value object");
  if (!object)
    return;

  expect(object->at("null").isNull(), "null retains its alternative");
  expect(object->at("boolean").asBool() && *object->at("boolean").asBool(),
         "boolean retains its alternative");
  expect(object->at("signed").asInt64() &&
             *object->at("signed").asInt64() ==
                 std::numeric_limits<std::int64_t>::min(),
         "signed integer retains its full range");
  expect(object->at("unsigned").asUInt64() &&
             *object->at("unsigned").asUInt64() ==
                 std::numeric_limits<std::uint64_t>::max(),
         "unsigned integer retains its full range");
  expect(object->at("float").asDouble() &&
             *object->at("float").asDouble() == 0.125,
         "floating point retains its alternative");
  expect(object->at("string").asString() &&
             *object->at("string").asString() ==
                 std::string("embedded\0nul", 12),
         "strings retain embedded null bytes");
  expect(object->at("array").asArray() &&
             object->at("array").asArray()->size() == 7 &&
             object->at("array").asArray()->back().asObject(),
         "arrays and nested objects convert recursively");

  const nlohmann::json roundTrip = jsonFromValue(converted);
  expect(roundTrip == source, "the complete nested DOM round-trips by value");
  expect(roundTrip.at("signed").type() ==
                 nlohmann::json::value_t::number_integer &&
             roundTrip.at("unsigned").type() ==
                 nlohmann::json::value_t::number_unsigned &&
             roundTrip.at("float").type() ==
                 nlohmann::json::value_t::number_float,
         "reverse conversion preserves every numeric DOM alternative");
}

void objectPayloadsAreRequiredExplicitly() {
  const nlohmann::json source = nlohmann::json::object(
      {{"threadId", "thread-1"}, {"attempt", std::uint64_t{4}}});
  const Value::Object object = objectFromJson(source);
  expect(object.size() == 2 && object.at("threadId").asString() &&
             *object.at("threadId").asString() == "thread-1" &&
             object.at("attempt").asUInt64() &&
             *object.at("attempt").asUInt64() == 4,
         "the object helper returns a typed nodegraph payload");
  expect(throws<std::invalid_argument>([] {
           static_cast<void>(objectFromJson(nlohmann::json::array()));
         }),
         "the object helper rejects non-object payloads");

  const nlohmann::json binary =
      nlohmann::json::binary({std::uint8_t{1}, std::uint8_t{2}});
  expect(throws<std::invalid_argument>(
             [&] { static_cast<void>(valueFromJson(binary)); }),
         "unsupported binary DOM values are rejected rather than corrupted");
}

void requestIdsAcceptOnlyLosslessStringOrIntegerValues() {
  const ProtocolRequestId text = requestIdFromJson("request-7");
  const ProtocolRequestId negative = requestIdFromJson(std::int64_t{-9});
  const ProtocolRequestId unsignedInRange =
      requestIdFromJson(std::uint64_t{42});

  expect(std::get<std::string>(text.value) == "request-7" &&
             jsonFromRequestId(text).type() == nlohmann::json::value_t::string,
         "string request IDs preserve their value and type");
  expect(std::get<std::int64_t>(negative.value) == -9 &&
             jsonFromRequestId(negative).type() ==
                 nlohmann::json::value_t::number_integer,
         "signed request IDs preserve their value and type");
  expect(std::get<std::int64_t>(unsignedInRange.value) == 42,
         "representable unsigned DOM integers remain exact request IDs");

  expect(throws<std::invalid_argument>(
             [] { static_cast<void>(requestIdFromJson(nullptr)); }) &&
             throws<std::invalid_argument>(
                 [] { static_cast<void>(requestIdFromJson(true)); }) &&
             throws<std::invalid_argument>(
                 [] { static_cast<void>(requestIdFromJson(1.25)); }),
         "null, boolean, and floating request IDs are rejected");
  expect(throws<std::out_of_range>([] {
           static_cast<void>(
               requestIdFromJson(std::numeric_limits<std::uint64_t>::max()));
         }),
         "unsigned request IDs outside ProtocolRequestId range are rejected");
}

} // namespace

int main() {
  everyDomAlternativeKeepsItsType();
  objectPayloadsAreRequiredExplicitly();
  requestIdsAcceptOnlyLosslessStringOrIntegerValues();

  if (failures != 0)
    std::cerr << failures << " node graph JSON assertion(s) failed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
