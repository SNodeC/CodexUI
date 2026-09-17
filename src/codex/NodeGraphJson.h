// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPHJSON_H
#define CODEXUI_CODEX_NODEGRAPHJSON_H

#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/Value.h"

#include <nlohmann/json.hpp>

namespace codexui::codex {

// These pure conversions adapt already-decoded values. They neither parse nor
// encode JSON text and retain no graph or protocol state.
[[nodiscard]] nodegraph::Value valueFromJson(const nlohmann::json &value);
[[nodiscard]] nodegraph::Value::Object
objectFromJson(const nlohmann::json &value);
[[nodiscard]] nodegraph::ProtocolRequestId
requestIdFromJson(const nlohmann::json &value);

[[nodiscard]] nlohmann::json jsonFromValue(const nodegraph::Value &value);
[[nodiscard]] nlohmann::json
jsonFromRequestId(const nodegraph::ProtocolRequestId &requestId);

} // namespace codexui::codex

#endif // CODEXUI_CODEX_NODEGRAPHJSON_H
