// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H
#define CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H

#include "codex/middle/MiddleTypes.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/UiViewState.h"

#include <cstddef>
#include <optional>

namespace codexui::codex::ui {

// Narrow compatibility seam between the canonical shared graph and the
// established Qt UI contract.  It owns no nodes and retains no projected
// state.  A successful call holds one short graph read, returns plain render
// values, and releases the graph before any QWidget code runs.
class NodeGraphUiAdapter final {
public:
  struct ConversationInfo {
    std::size_t authoritativeItemCount = 0;
    bool readyForDisplay = false;
    bool hydrationFailed = false;
    bool providerHasMore = false;
  };

  explicit NodeGraphUiAdapter(const nodegraph::NodeGraph &graph) noexcept;

  [[nodiscard]] std::optional<middle::ConversationSnapshot>
  conversation(const nodegraph::NodeRef &thread,
               std::size_t itemLimit) const;

  [[nodiscard]] std::optional<ConversationInfo>
  conversationInfo(const nodegraph::NodeRef &thread) const;

  [[nodiscard]] std::optional<middle::VisibleCardData>
  card(const nodegraph::NodeRef &thread,
       const nodegraph::NodeRef &item) const;

  // Projects an authoritative user item only when it exactly materializes a
  // still-current local prompt. The returned card retains the LocalPromptKey
  // and prompt NodeRef so Qt can morph and acknowledge that one stable row.
  [[nodiscard]] std::optional<middle::VisibleCardData>
  promptMaterialization(const nodegraph::NodeRef &thread,
                        const nodegraph::NodeRef &item) const;

  // Projects only a canonical last item of the selected thread. It is the
  // bounded structural fast path for ordinary append; any non-tail or prompt
  // alias case returns nullopt and uses complete reconciliation instead.
  [[nodiscard]] std::optional<middle::ConversationTailCard>
  tailCard(const nodegraph::NodeRef &thread,
           const nodegraph::NodeRef &item) const;

  [[nodiscard]] std::optional<ThreadListSnapshot>
  threads(const nodegraph::NodeRef &selectedThread) const;

  [[nodiscard]] std::optional<ThreadListRow>
  threadRow(const nodegraph::NodeRef &thread) const;

  [[nodiscard]] std::optional<InspectorSnapshot>
  inspector(const nodegraph::NodeRef &selectedThread,
            InspectorProjection projection = InspectorProjection::All) const;

private:
  const nodegraph::NodeGraph *graph_;
};

} // namespace codexui::codex::ui

#endif // CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H
