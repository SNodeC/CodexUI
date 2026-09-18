// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_MESSAGES_H
#define CODEXUI_CODEX_NODEGRAPH_MESSAGES_H

#include "codex/nodegraph/NodeGraph.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace codexui::nodegraph {

struct GraphChanged final {
  std::uint64_t revision = 0;
  std::vector<NodeRef> affected;
  std::vector<NodeRef> removed;
  bool rescanRequired = false;
  std::vector<ChildListChange> childListsChanged;
  std::uint64_t providerAuthorityRevision = 0;

  bool operator==(const GraphChanged &) const = default;
};

// Bounded, metadata-only diagnostics for the existing Inspector. This is
// explicitly non-authoritative UI history; current state remains in NodeGraph
// and raw protocol payloads never cross the worker/Qt boundary.
struct ProtocolDiagnostic final {
  Value::Object details;
  std::vector<Value::Object> diagnosticBatch;

  bool operator==(const ProtocolDiagnostic &) const = default;
};

struct WorkerStopped final {
  std::string reason;

  bool operator==(const WorkerStopped &) const = default;
};

using WorkerToQtMessage =
    std::variant<GraphChanged, ProtocolDiagnostic, WorkerStopped>;

struct Attachment final {
  std::string path;
  std::string displayName;
  std::string mimeType;
  std::optional<std::vector<std::uint8_t>> bytes;

  bool operator==(const Attachment &) const = default;
};

enum class NodeActionKind : std::uint8_t {
  Hydrate,
  Reload,
  LoadHistory,
  Rename,
  Fork,
  Archive,
  Unarchive,
  Delete,
  SubmitPrompt,
  InterruptTurn,
  ResolveInteraction,
  PromptMaterialized,
  UiDetached,
};

// Only newly authored data crosses from Qt. Existing protocol-derived state is
// read from target on the worker and is never copied into an action.
struct NodeAction final {
  NodeRef target;
  NodeActionKind kind = NodeActionKind::Hydrate;
  std::string promptText;
  std::vector<Attachment> attachments;
  Value::Object payload;
  std::string correlation;

  bool operator==(const NodeAction &) const = default;
};

enum class RuntimeActionKind : std::uint8_t {
  RefreshThreads,
  LoadMoreThreads,
  CreateThread,
  Connect,
  Disconnect,
  Reconnect,
  ConfigureConnection,
  ClaimController,
  ReleaseController,
  RefreshCatalogs,
};

struct RuntimeAction final {
  RuntimeActionKind kind = RuntimeActionKind::RefreshThreads;
  Value::Object payload;
  std::string correlation;
  // CreateThread owns its first prompt until the worker admits and dispatches
  // it. Other runtime actions leave these fields empty.
  std::string promptText;
  std::vector<Attachment> attachments;

  bool operator==(const RuntimeAction &) const = default;
};

struct ShutdownRequest final {
  bool operator==(const ShutdownRequest &) const = default;
};

using QtToWorkerMessage =
    std::variant<NodeAction, RuntimeAction, ShutdownRequest>;

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_MESSAGES_H
