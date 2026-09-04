// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H
#define CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H

#include "codex/middle/MiddleTypes.h"
#include "codex/nodegraph/NodeGraph.h"

#include <cstddef>
#include <optional>

namespace codexui::codex::ui {

// Narrow compatibility seam between the canonical shared graph and the
// established Qt UI contract.  It owns no nodes and retains no projected
// state.  A successful call holds one short graph read, returns plain render
// values, and releases the graph before any QWidget code runs.
class NodeGraphUiAdapter final {
public:
  struct ConversationOptions {
    bool showReasoning = true;
    bool showCodexUpdates = true;
  };

  explicit NodeGraphUiAdapter(const nodegraph::NodeGraph &graph) noexcept;

  [[nodiscard]] std::optional<middle::ConversationSnapshot>
  conversation(const nodegraph::NodeRef &thread, std::size_t itemLimit,
               ConversationOptions options) const;

  [[nodiscard]] std::optional<middle::VisibleCardData>
  card(const nodegraph::NodeRef &thread, const nodegraph::NodeRef &item,
       ConversationOptions options) const;

private:
  const nodegraph::NodeGraph *graph_;
};

} // namespace codexui::codex::ui

#endif // CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H
