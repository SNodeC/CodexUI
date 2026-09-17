// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PENDINGREQUESTPOLICY_H
#define CODEXUI_CODEX_PENDINGREQUESTPOLICY_H

#include "codex/nodegraph/NodeGraph.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace codexui::codex {

enum class PendingRequestKind : std::uint8_t {
  CommandApproval,
  FileChangeApproval,
  UserInput,
  McpElicitation,
  PermissionsApproval,
  DynamicToolCall,
  AuthenticationRefresh,
  Attestation,
  LegacyPatchApproval,
  LegacyCommandApproval,
  Unsupported,
};

enum class PendingRequestAvailability : std::uint8_t {
  Actionable,
  Submitting,
  RecoveryOnly,
  Unavailable,
};

enum class PendingRequestActionTone : std::uint8_t {
  Approve,
  Danger,
  Neutral,
};

struct PendingRequestAction {
  std::string value;
  std::string label;
  PendingRequestActionTone tone = PendingRequestActionTone::Neutral;
  bool requiresInput = false;

  bool operator==(const PendingRequestAction &) const = default;
};

struct PendingRequestControls {
  std::optional<PendingRequestAction> positive;
  std::optional<PendingRequestAction> negative;
  bool directEnabled = false;
  bool reviewEnabled = false;

  bool operator==(const PendingRequestControls &) const = default;
};

// Only data authored by the user belongs here. Protocol-derived request facts
// remain on the target Interaction and are combined with this submission at
// the worker boundary.
struct PendingRequestSubmission {
  std::string choice;
  nlohmann::json input = nullptr;
  nlohmann::json metadata = nullptr;

  bool operator==(const PendingRequestSubmission &) const = default;
};

struct PendingRequestDescriptor {
  nodegraph::NodeRef target;
  std::string method;
  PendingRequestKind kind = PendingRequestKind::Unsupported;
  std::string threadId;
  std::string threadTitle;
  std::uint64_t responseRevision = 0;
  nlohmann::json raw = nlohmann::json::object();
  PendingRequestAvailability availability =
      PendingRequestAvailability::Unavailable;
  std::string error;
  std::optional<PendingRequestSubmission> retainedSubmission;

  bool operator==(const PendingRequestDescriptor &) const = default;
};

// A valid authored submission produces exactly one JSON result or one JSON-RPC
// -32601 error message.
using PendingRequestResponse = std::variant<nlohmann::json, std::string>;

class PendingRequestPolicy final {
public:
  PendingRequestPolicy() = delete;

  [[nodiscard]] static PendingRequestKind
  kindForMethod(std::string_view method) noexcept;
  [[nodiscard]] static std::string_view
  kindToken(PendingRequestKind kind) noexcept;
  [[nodiscard]] static std::string_view title(PendingRequestKind kind) noexcept;
  [[nodiscard]] static std::string_view
  dialogTitle(PendingRequestKind kind) noexcept;
  [[nodiscard]] static std::string
  detail(const PendingRequestDescriptor &request);
  [[nodiscard]] static std::string
  status(const PendingRequestDescriptor &request);
  [[nodiscard]] static nlohmann::json
  disclosure(const PendingRequestDescriptor &request);

  [[nodiscard]] static std::vector<PendingRequestAction>
  actions(const PendingRequestDescriptor &request);
  [[nodiscard]] static PendingRequestControls
  controls(const PendingRequestDescriptor &request);
  [[nodiscard]] static int
  attentionRank(PendingRequestAvailability availability, bool retainedSubmission,
                bool selectedThread) noexcept;
  [[nodiscard]] static std::optional<std::size_t>
  attentionIndex(const std::vector<PendingRequestDescriptor> &requests,
                 std::string_view selectedThreadId);

  // Absence is a locally invalid authored submission and must never reach the
  // wire. An engaged error is an intentional JSON-RPC response.
  [[nodiscard]] static std::optional<PendingRequestResponse>
  responseForSubmission(PendingRequestKind kind, const nlohmann::json &request,
                        PendingRequestSubmission submission);
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PENDINGREQUESTPOLICY_H
