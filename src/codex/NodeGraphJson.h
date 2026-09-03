// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPHJSON_H
#define CODEXUI_CODEX_NODEGRAPHJSON_H

#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/Value.h"

#include <nlohmann/json.hpp>

namespace codexui::codex {

// These functions adapt an already-decoded worker-thread DOM. They do not
// parse or encode JSON text and must not be called from the Qt main thread.
[[nodiscard]] nodegraph::Value valueFromJson(const nlohmann::json &value);
[[nodiscard]] nodegraph::Value::Object
objectFromJson(const nlohmann::json &value);
[[nodiscard]] nodegraph::ProtocolRequestId
requestIdFromJson(const nlohmann::json &value);

// Reverse conversion is for worker-thread CodexBridge calls only.
[[nodiscard]] nlohmann::json jsonFromValue(const nodegraph::Value &value);
[[nodiscard]] nlohmann::json
jsonFromRequestId(const nodegraph::ProtocolRequestId &requestId);

} // namespace codexui::codex

#endif // CODEXUI_CODEX_NODEGRAPHJSON_H
