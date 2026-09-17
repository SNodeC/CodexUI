// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H
#define CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H

#include "codex/TurnSettingsPolicy.h"
#include "codex/middle/MiddleTypes.h"
#include "codex/nodegraph/Messages.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/UiViewState.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace codexui::codex::ui {

// Narrow compatibility seam between the canonical shared graph and the
// established Qt UI contract.  It owns no nodes and retains no projected
// state.  A successful call holds one short graph read, returns plain render
// values, and releases the graph before any QWidget code runs.
class NodeGraphUiAdapter final {
public:
  static constexpr std::size_t MaximumConversationDeltaItems = 64;

  struct ConversationInfo {
    bool readyForDisplay = false;
    bool hydrationFailed = false;
    bool providerHasMore = false;
    bool historyRequestPending = false;
  };

  struct ConversationRoute {
    bool affected = false;
    bool structural = false;
    bool authorityReplacement = false;
    std::vector<nodegraph::NodeRef> items;
    std::optional<bool> historyRequestPending;
  };

  explicit NodeGraphUiAdapter(const nodegraph::NodeGraph &graph) noexcept;

  [[nodiscard]] std::optional<middle::ConversationSnapshot>
  conversation(const nodegraph::NodeRef &thread) const;

  [[nodiscard]] std::optional<ConversationInfo>
  conversationInfo(const nodegraph::NodeRef &thread) const;

  // Classifies one graph notification at the graph boundary. Shell only
  // coalesces the returned identities and never derives ancestry, placement,
  // prompt aliases, or authority-replacement policy itself.
  [[nodiscard]] ConversationRoute
  conversationRoute(const nodegraph::GraphChanged &change,
                    const nodegraph::NodeRef &thread) const;

  // Projects a coalesced bounded set through one graph read. Presentation-only
  // values and canonically ordered structural rows share graphCardData and the
  // same stable identity policy used by complete snapshots.
  [[nodiscard]] std::optional<middle::ConversationDelta>
  conversationDelta(const nodegraph::NodeRef &thread,
                    std::span<const nodegraph::NodeRef> items,
                    bool structural) const;

  [[nodiscard]] std::optional<ThreadListSnapshot>
  threads(const nodegraph::NodeRef &selectedThread) const;

  [[nodiscard]] std::optional<ThreadListRow>
  threadRow(const nodegraph::NodeRef &thread) const;

  [[nodiscard]] std::optional<InspectorPageSnapshot>
  pendingRequests(const InspectorRowRequest &request = {}) const;

  [[nodiscard]] std::optional<PendingRequestsSummary>
  pendingRequestSummary(
      std::string_view selectedThreadId,
      std::span<const nodegraph::NodeRef> localTargets = {}) const;

  [[nodiscard]] std::optional<PendingRequestDescriptor>
  pendingRequest(const nodegraph::NodeRef &target, bool *busy = nullptr) const;

  [[nodiscard]] std::optional<TurnSettingsContext>
  turnSettings(const nodegraph::NodeRef &thread,
               std::string_view draftIdentity = {},
               std::string_view draftWorkspace = {}) const;

  [[nodiscard]] std::optional<InspectorSnapshot>
  inspector(const nodegraph::NodeRef &selectedThread,
            InspectorProjection projection,
            const InspectorRowRequest &request = {}) const;

  // Routes graph notifications through the same dependency policy that owns
  // the corresponding projection. Shell does not reconstruct graph ancestry
  // or presentation dependencies.
  [[nodiscard]] bool
  inspectorAffected(const nodegraph::GraphChanged &change,
                    const nodegraph::NodeRef &selectedThread,
                    InspectorProjection projection) const;

private:
  [[nodiscard]] static std::optional<middle::ConversationRowChange>
  projectRowChange(std::optional<nodegraph::NodeGraph::ReadAccess> &read,
                   const nodegraph::NodeRef &thread,
                   const nodegraph::NodeRef &item, std::string_view threadCwd,
                   const nodegraph::NodeRef &activeTurn);

  const nodegraph::NodeGraph *graph_;
};

} // namespace codexui::codex::ui

#endif // CODEXUI_CODEX_UI_NODEGRAPHUIADAPTER_H
