// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once

#include "codex/nodegraph/Value.h"
#include <nlohmann/json_fwd.hpp>

namespace codexui::codex {

enum class TimeUnit { Seconds, Milliseconds, DurationMilliseconds };
struct ProtocolTime {
  std::string field;
  std::optional<std::int64_t> value;
  TimeUnit unit = TimeUnit::Seconds;
  bool unavailableZero = false;
  bool operator==(const ProtocolTime &) const = default;
};
using ProtocolTimes = std::vector<ProtocolTime>;

// Schema paths only; bounded extraction, without copying payloads or retaining
// graph locks. Missing fields are absent, explicit nulls remain unavailable.
ProtocolTimes protocolTimes(const nodegraph::Value::Object &fields,
                            std::string_view type);
ProtocolTimes protocolTimes(const nlohmann::json &body, std::string_view method,
                            bool response);
ProtocolTime protocolTime(std::string field, std::optional<std::int64_t> value,
                          bool unavailableZero = false);

} // namespace codexui::codex
