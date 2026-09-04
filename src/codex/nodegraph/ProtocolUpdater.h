// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_PROTOCOLUPDATER_H
#define CODEXUI_CODEX_NODEGRAPH_PROTOCOLUPDATER_H

#include "codex/nodegraph/NodeGraph.h"
#include "codex/nodegraph/ProtocolCatalog.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace codexui::nodegraph {

enum class DecodedMessageKind : std::uint8_t {
  ClientRequest,
  ClientResult,
  ClientError,
  ServerRequest,
  ServerNotification,
  ClientNotification,
};

struct ProtocolRequestId final {
  std::variant<std::int64_t, std::string> value;

  ProtocolRequestId(std::int64_t id) : value(id) {}
  ProtocolRequestId(std::string id) : value(std::move(id)) {}
  ProtocolRequestId(const char *id) : value(std::string(id ? id : "")) {}

  [[nodiscard]] std::string canonical() const;
  bool operator==(const ProtocolRequestId &) const = default;
};

// This is already decoded. The nodegraph never parses or emits app-server JSON.
// Client results/errors carry the correlated request method supplied by the
// worker because the wire response itself has no method member.
struct DecodedMessage final {
  DecodedMessageKind kind = DecodedMessageKind::ServerNotification;
  std::string method;
  std::optional<ProtocolRequestId> requestId;
  Value::Object payload;
  // Worker callbacks retain the exact request/interaction node they created.
  // Supplying it prevents a late response from mutating a newer node after a
  // provider reuses the same JSON-RPC id.
  NodeRef expectedNode;
  // Supplied by the CodexUI worker for meaningful thread-scoped traffic.
  // Tests and other headless users need no clock; absence leaves activity
  // unchanged. Hydration, global, and catalog traffic deliberately omit it.
  std::optional<std::int64_t> activityAt;
  // JSON-RPC request ids are connection-scoped. WorkerLogic supplies both
  // generations for server requests so a replacement provider can reuse a
  // wire id without replacing a retained recovery interaction.
  std::optional<std::uint64_t> connectionGeneration;
  std::optional<std::uint64_t> providerGeneration;
};

struct ApplyResult final {
  bool knownMethod = false;
  MessageDisposition disposition =
      MessageDisposition::IntentionallyStateNeutral;
  GraphChange change;
  // The exact newly-created Operation or Interaction, when applicable.
  NodeRef primary;
};

struct AppliedMessage final {
  bool knownMethod = false;
  MessageDisposition disposition =
      MessageDisposition::IntentionallyStateNeutral;
  NodeRef primary;
};

// Provider turn and item identifiers are only unique inside their protocol
// owners. These helpers are the one canonical encoding used at graph lookup
// boundaries. Locally-created provisional nodes keep their existing IDs.
[[nodiscard]] NodeId scopedTurnNodeId(std::string_view threadId,
                                      std::string_view turnId);
[[nodiscard]] NodeId scopedItemNodeId(const NodeId &turnNodeId,
                                      std::string_view itemId);

// Returns the raw provider identifier retained in node state, falling back to
// the node's canonical ID for globally-addressed and local nodes.
[[nodiscard]] std::string protocolCanonicalId(const NodeState &state,
                                              const NodeRef &node);

class ProtocolUpdater final {
public:
  explicit ProtocolUpdater(NodeGraph &graph) noexcept;

  [[nodiscard]] ApplyResult apply(DecodedMessage message);
  // WorkerLogic uses this concrete transaction form when a request result
  // must update its operation and associated local prompt/hydration state in
  // the same graph revision. The caller owns and finishes the write access.
  [[nodiscard]] AppliedMessage applyInto(NodeGraph::WriteAccess &write,
                                         const DecodedMessage &message);

  // Called on the worker after CodexBridge accepts the matching server-request
  // response. This is a lifecycle operation, not a Qt callback.
  [[nodiscard]] GraphChange
  resolveInteraction(const ProtocolRequestId &requestId, bool accepted,
                     std::string error = {});
  [[nodiscard]] GraphChange resolveInteraction(const NodeRef &interaction,
                                               bool accepted,
                                               std::string error = {});

private:
  [[nodiscard]] ProtocolDirection
  catalogDirection(DecodedMessageKind kind) const noexcept;
  [[nodiscard]] NodeRef applyOperation(NodeGraph::WriteAccess &write,
                                       const DecodedMessage &message);
  [[nodiscard]] NodeRef applyInteraction(NodeGraph::WriteAccess &write,
                                         const DecodedMessage &message);
  void applyGraphUpdate(NodeGraph::WriteAccess &write,
                        const DecodedMessage &message,
                        std::optional<std::uint64_t> preserveChangesAfter = {});
  [[nodiscard]] bool applyRealtimeUpdate(NodeGraph::WriteAccess &write,
                                         const DecodedMessage &message);
  void applyUnknown(NodeGraph::WriteAccess &write,
                    const DecodedMessage &message);
  void applyThreadActivity(NodeGraph::WriteAccess &write,
                           const DecodedMessage &message);

  [[nodiscard]] NodeRef
  ingestThread(NodeGraph::WriteAccess &write, const Value::Object &object,
               std::string_view fallbackId = {}, bool replaceTurns = false,
               std::optional<std::uint64_t> preserveChangesAfter = {});
  [[nodiscard]] NodeRef
  ingestTurn(NodeGraph::WriteAccess &write, const Value::Object &object,
             const NodeRef &thread, std::string_view fallbackId = {},
             bool replaceItems = false,
             std::optional<std::uint64_t> preserveChangesAfter = {},
             bool updateCurrentRelation = true);
  [[nodiscard]] NodeRef
  ingestItem(NodeGraph::WriteAccess &write, const Value::Object &object,
             const NodeRef &turn, std::string_view fallbackId = {},
             std::optional<std::uint64_t> preserveChangesAfter = {});
  void admitRootThread(NodeGraph::WriteAccess &write, const NodeRef &thread,
                       bool prepend);
  void replaceThreadList(NodeGraph::WriteAccess &write,
                         const Value::Array &threads);
  void removeThread(NodeGraph::WriteAccess &write, const NodeRef &thread);

  NodeGraph *graph_;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_PROTOCOLUPDATER_H
