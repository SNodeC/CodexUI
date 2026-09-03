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

  bool operator==(const GraphChanged &) const = default;
};

enum class UiEffectKind : std::uint8_t {
  ShowNotice,
  FocusComposer,
  ClearComposerDraft,
  PrepareLocalPromptAdmission,
};

struct UiEffect final {
  UiEffectKind kind = UiEffectKind::ShowNotice;
  std::optional<NodeRef> target;
  std::string text;
  Value::Object details;

  bool operator==(const UiEffect &) const = default;
};

struct WorkerStopped final {
  std::string reason;

  bool operator==(const WorkerStopped &) const = default;
};

using WorkerToQtMessage = std::variant<GraphChanged, UiEffect, WorkerStopped>;

struct Attachment final {
  std::string path;
  std::string displayName;
  std::string mimeType;
  std::optional<std::vector<std::uint8_t>> bytes;

  bool operator==(const Attachment &) const = default;
};

enum class NodeActionKind : std::uint8_t {
  Hydrate,
  Resume,
  LoadHistory,
  Rename,
  Fork,
  Archive,
  Unarchive,
  Delete,
  Compact,
  StartShellCommand,
  ApproveGuardianDenied,
  SetGoal,
  ClearGoal,
  MoveSection,
  UpdateMetadata,
  UpdateSettings,
  SubmitPrompt,
  SteerPrompt,
  InterruptTurn,
  ResolveInteraction,
  UploadFeedback,
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
  CreateThread,
  Connect,
  Disconnect,
  Reconnect,
  ConfigureConnection,
  ClaimController,
  ReleaseController,
  RefreshCatalogs,
  AccountLogin,
  AccountLogout,
  InstallPlugin,
  UninstallPlugin,
};

struct RuntimeAction final {
  RuntimeActionKind kind = RuntimeActionKind::RefreshThreads;
  Value::Object payload;
  std::string correlation;

  bool operator==(const RuntimeAction &) const = default;
};

struct ShutdownRequest final {
  bool operator==(const ShutdownRequest &) const = default;
};

using QtToWorkerMessage =
    std::variant<NodeAction, RuntimeAction, ShutdownRequest>;

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_MESSAGES_H
