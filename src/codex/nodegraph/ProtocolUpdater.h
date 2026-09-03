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
};

struct ApplyResult final {
  bool knownMethod = false;
  MessageDisposition disposition =
      MessageDisposition::IntentionallyStateNeutral;
  GraphChange change;
};

class ProtocolUpdater final {
public:
  explicit ProtocolUpdater(NodeGraph &graph) noexcept;

  [[nodiscard]] ApplyResult apply(DecodedMessage message);

  // Called on the worker after CodexBridge accepts the matching server-request
  // response. This is a lifecycle operation, not a Qt callback.
  [[nodiscard]] GraphChange
  resolveInteraction(const ProtocolRequestId &requestId, bool accepted,
                     std::string error = {});

private:
  [[nodiscard]] ProtocolDirection
  catalogDirection(DecodedMessageKind kind) const noexcept;
  void applyOperation(NodeGraph::WriteAccess &write,
                      const DecodedMessage &message);
  void applyInteraction(NodeGraph::WriteAccess &write,
                        const DecodedMessage &message);
  void applyGraphUpdate(NodeGraph::WriteAccess &write,
                        const DecodedMessage &message);
  void applyUnknown(NodeGraph::WriteAccess &write,
                    const DecodedMessage &message);

  [[nodiscard]] NodeRef ingestThread(NodeGraph::WriteAccess &write,
                                     const Value::Object &object,
                                     std::string_view fallbackId = {});
  [[nodiscard]] NodeRef ingestTurn(NodeGraph::WriteAccess &write,
                                   const Value::Object &object,
                                   const NodeRef &thread,
                                   std::string_view fallbackId = {});
  [[nodiscard]] NodeRef ingestItem(NodeGraph::WriteAccess &write,
                                   const Value::Object &object,
                                   const NodeRef &turn,
                                   std::string_view fallbackId = {});

  NodeGraph *graph_;
  std::uint64_t unknownSequence_ = 0;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_PROTOCOLUPDATER_H
