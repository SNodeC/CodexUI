// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "ProtocolTiming.h"
#include <algorithm>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace codexui::codex {
namespace {
#include "ProtocolTimingPaths.inc"
constexpr std::size_t MaximumTimes = 128;
constexpr std::size_t MaximumVisits = 2048;

const nodegraph::Value *member(const nodegraph::Value &v,
                               std::string_view key) {
  return v.find(key);
}
const nlohmann::json *member(const nlohmann::json &v, std::string_view key) {
  const auto it = v.is_object() ? v.find(key) : v.end();
  return it == v.end() ? nullptr : &*it;
}
std::optional<std::int64_t> integer(const nodegraph::Value &v) {
  return nodegraph::signedIntegerFromValue(&v);
}
std::optional<std::int64_t> integer(const nlohmann::json &v) {
  if (v.is_number_unsigned()) {
    const auto n = v.get<std::uint64_t>();
    return n <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
               ? std::optional<std::int64_t>(n)
               : std::nullopt;
  }
  return v.is_number_integer() ? std::optional(v.get<std::int64_t>())
                               : std::nullopt;
}
template <class V, class F> void children(const V &v, F visit) {
  if constexpr (std::is_same_v<V, nodegraph::Value>) {
    if (const auto *array = v.asArray())
      for (std::size_t i = 0; i < array->size(); ++i) {
        if (!visit((*array)[i], std::to_string(i)))
          break;
      }
    if (const auto *object = v.asObject())
      for (const auto &[key, child] : *object) {
        if (!visit(child, key))
          break;
      }
  } else if (v.is_array() || v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      const std::string key =
          v.is_object() ? it.key() : std::to_string(it - v.begin());
      if (!visit(*it, key))
        break;
    }
  }
}
template <class V>
void collect(const V &v, std::string_view path, const std::string &prefix,
             bool unavailableZero, ProtocolTimes &out, std::size_t &visits) {
  if (++visits > MaximumVisits || out.size() >= MaximumTimes)
    return;
  if (path.starts_with("@")) {
    for (const auto child : TimingTypes.at(path.substr(1)))
      collect(v, child, prefix, unavailableZero, out, visits);
    return;
  }
  if (path.empty()) {
    out.push_back(protocolTime(prefix, integer(v), unavailableZero));
    return;
  }
  const auto slash = path.find('/');
  const auto key = path.substr(0, slash);
  const auto tail =
      slash == path.npos ? std::string_view{} : path.substr(slash + 1);
  const auto descend = [&](const auto &child, const std::string &name) {
    auto end = std::min(name.size(), std::size_t{192});
    while (end < name.size() && end &&
           (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80)
      --end;
    const auto boundedName =
        name.substr(0, end) + (end < name.size() ? "…" : "");
    collect(child, tail,
            prefix.empty() ? boundedName : prefix + "/" + boundedName,
            unavailableZero, out, visits);
    return visits < MaximumVisits && out.size() < MaximumTimes;
  };
  if (key == "*")
    children(v, descend);
  else if (const auto *child = member(v, key))
    descend(*child, std::string(key));
}
} // namespace

ProtocolTime protocolTime(std::string field, std::optional<std::int64_t> value,
                          bool unavailableZero) {
  const auto name = std::string_view(field).substr(field.find_last_of('/') + 1);
  const auto unit = name == "durationMs" || name == "duration_ms"
                        ? TimeUnit::DurationMilliseconds
                    : name.ends_with("Ms") ? TimeUnit::Milliseconds
                                           : TimeUnit::Seconds;
  return {std::move(field), value, unit, unavailableZero};
}

ProtocolTimes protocolTimes(const nodegraph::Value::Object &fields,
                            std::string_view type) {
  ProtocolTimes out;
  const auto spec = TimingTypes.find(type);
  if (spec == TimingTypes.end())
    return out;
  std::size_t visits = 0;
  for (const auto path : spec->second) {
    // Graph children have their own nodes; project only this object's facts.
    if (path.starts_with("turns/") || path.starts_with("items/"))
      continue;
    const auto slash = path.find('/');
    if (const auto *v = nodegraph::valueMember(fields, path.substr(0, slash)))
      collect(*v,
              slash == path.npos ? std::string_view{} : path.substr(slash + 1),
              std::string(path.substr(0, slash)),
              type == "FsGetMetadataResponse", out, visits);
  }
  return out;
}

ProtocolTimes protocolTimes(const nlohmann::json &body, std::string_view method,
                            bool response) {
  ProtocolTimes out;
  const auto route = TimingMethods.find(std::string(method) +
                                        (response ? ":result" : ":params"));
  if (route == TimingMethods.end())
    return out;
  std::size_t visits = 0;
  for (const auto path : TimingTypes.at(route->second))
    collect(body, path, {}, method == "fs/getMetadata", out, visits);
  if (visits >= MaximumVisits || out.size() >= MaximumTimes)
    out.push_back(protocolTime(
        "Timestamp display limit reached; further entries omitted", {}));
  return out;
}
} // namespace codexui::codex
