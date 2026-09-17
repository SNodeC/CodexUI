// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PendingRequestPolicy.h"

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using codexui::codex::PendingRequestAvailability;
using codexui::codex::PendingRequestControls;
using codexui::codex::PendingRequestDescriptor;
using codexui::codex::PendingRequestKind;
using codexui::codex::PendingRequestPolicy;
using codexui::codex::PendingRequestSubmission;

bool expect(bool condition, std::string_view message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

PendingRequestDescriptor
descriptor(PendingRequestKind kind,
           nlohmann::json raw = nlohmann::json::object()) {
  PendingRequestDescriptor result;
  result.kind = kind;
  result.threadId = "thread-1";
  result.raw = std::move(raw);
  return result;
}

bool verifyKindsAndPresentation() {
  struct KindCase {
    PendingRequestKind kind;
    std::string_view title;
    std::string_view dialogTitle;
  };
  constexpr std::array cases{
      KindCase{PendingRequestKind::CommandApproval,
               "Command approval requested", "Command approval"},
      KindCase{PendingRequestKind::FileChangeApproval,
               "File-change approval requested", "File-change approval"},
      KindCase{PendingRequestKind::UserInput, "Codex needs input",
               "Codex needs input"},
      KindCase{PendingRequestKind::McpElicitation, "MCP server request",
               "MCP server request"},
      KindCase{PendingRequestKind::PermissionsApproval, "Permission request",
               "Permission request"},
      KindCase{PendingRequestKind::DynamicToolCall,
               "Codex request needs attention", "Dynamic tool request"},
      KindCase{PendingRequestKind::AuthenticationRefresh,
               "Codex request needs attention", "Authentication refresh"},
      KindCase{PendingRequestKind::Attestation, "Codex request needs attention",
               "Attestation request"},
      KindCase{PendingRequestKind::LegacyPatchApproval, "Legacy patch approval",
               "Legacy patch approval"},
      KindCase{PendingRequestKind::LegacyCommandApproval,
               "Legacy command approval", "Legacy command approval"},
      KindCase{PendingRequestKind::Unsupported, "Codex request needs attention",
               "Unsupported Codex request"},
  };

  bool passed = true;
  for (const KindCase &entry : cases) {
    passed &= expect(
        PendingRequestPolicy::title(entry.kind) == entry.title &&
            PendingRequestPolicy::dialogTitle(entry.kind) == entry.dialogTitle,
        std::string("native request titles: ") + std::string(entry.title));
  }

  PendingRequestDescriptor detailed =
      descriptor(PendingRequestKind::CommandApproval,
                 {{"command", "make test"},
                  {"reason", "review"},
                  {"cwd", "/repo"},
                  {"secret", "must not be displayed"},
                  {"permissions", {{"network", true}}},
                  {"questions", nlohmann::json::array({1, 2})}});
  const std::string detail = PendingRequestPolicy::detail(detailed);
  passed &=
      expect(detail.find("Thread: thread-1") != std::string::npos &&
                 detail.find("Command: make test") != std::string::npos &&
                 detail.find("Reason: review") != std::string::npos &&
                 detail.find("Directory: /repo") != std::string::npos &&
                 detail.find("must not be displayed") == std::string::npos,
             "request detail is derived once from the descriptor");
  passed &= expect(PendingRequestPolicy::detail(descriptor(
                       PendingRequestKind::Unsupported)) == "Thread: thread-1",
                   "empty request detail preserves its visible context");

  PendingRequestDescriptor dynamic =
      descriptor(PendingRequestKind::DynamicToolCall,
                 {{"callId", "call-1"},
                  {"namespace", "tools"},
                  {"tool", "lookup"},
                  {"arguments", {{"password", "secret"}}},
                  {"providerPrivate", "hidden"}});
  const nlohmann::json dynamicDisclosure =
      PendingRequestPolicy::disclosure(dynamic);
  passed &= expect(
      dynamicDisclosure.value("callId", std::string{}) == "call-1" &&
          dynamicDisclosure.value("namespace", std::string{}) == "tools" &&
          dynamicDisclosure.value("tool", std::string{}) == "lookup" &&
          !dynamicDisclosure.contains("arguments") &&
          !dynamicDisclosure.contains("providerPrivate") &&
          dynamicDisclosure.contains("additionalDetails"),
      "dynamic-tool disclosure excludes arguments and marks "
      "unknown fields");

  PendingRequestDescriptor mcp =
      descriptor(PendingRequestKind::McpElicitation,
                 {{"serverName", "server"},
                  {"message", "Confirm access"},
                  {"url", "https://example.invalid/confirm"},
                  {"requestedSchema", {{"type", "object"}}},
                  {"_meta", {{"credential", "secret"}}}});
  const nlohmann::json mcpDisclosure = PendingRequestPolicy::disclosure(mcp);
  passed &= expect(mcpDisclosure.contains("serverName") &&
                       mcpDisclosure.contains("message") &&
                       mcpDisclosure.contains("requestedSchema") &&
                       mcpDisclosure.contains("url") &&
                       !mcpDisclosure.contains("_meta"),
                   "MCP disclosure keeps review facts but hides metadata");

  PendingRequestDescriptor user = descriptor(
      PendingRequestKind::UserInput,
      {{"isBlocking", true},
       {"questions", nlohmann::json::array({{{"id", "q"},
                                             {"question", "Secret?"},
                                             {"options", nullptr},
                                             {"isSecret", true}}})}});
  passed &= expect(PendingRequestPolicy::disclosure(user) ==
                           nlohmann::json({{"isBlocking", true}}) &&
                       PendingRequestPolicy::detail(user).find("1 questions") !=
                           std::string::npos,
                   "question content has one specialized rendering");

  PendingRequestDescriptor oversized =
      descriptor(PendingRequestKind::PermissionsApproval,
                 {{"permissions", nlohmann::json::array()}});
  for (int index = 0; index < 1000; ++index)
    oversized.raw["permissions"].push_back(nlohmann::json::object());
  const nlohmann::json bounded = PendingRequestPolicy::disclosure(oversized);
  passed &= expect(
      bounded.contains("permissions") && bounded["permissions"].is_array() &&
          bounded["permissions"].size() < 1000 &&
          bounded.value("additionalDetails", std::string{}) ==
              "Omitted for display",
      "empty containers and depth limits consume the disclosure budget");
  return passed;
}

bool verifyNativeControlsAndRecovery() {
  bool passed = true;
  PendingRequestDescriptor command = descriptor(
      PendingRequestKind::CommandApproval, {{"command", "make test"}});
  PendingRequestControls controls = PendingRequestPolicy::controls(command);
  passed &= expect(controls.positive && controls.positive->value == "accept" &&
                       !controls.negative,
                   "command direct controls use the canonical policy");

  const PendingRequestDescriptor malformedExec =
      descriptor(PendingRequestKind::CommandApproval,
                 {{"command", "make test"},
                  {"proposedExecpolicyAmendment", nlohmann::json::object()}});
  const auto malformedActions = PendingRequestPolicy::actions(malformedExec);
  passed &= expect(
      malformedActions.size() == 1 &&
          malformedActions.front().value == "cancel" &&
          !PendingRequestPolicy::controls(malformedExec).positive,
      "malformed command amendments fail closed without compact approval");

  const PendingRequestDescriptor networkOnly =
      descriptor(PendingRequestKind::CommandApproval,
                 {{"networkApprovalContext",
                   {{"host", "example.test"}, {"protocol", "https"}}}});
  passed &= expect(!PendingRequestPolicy::controls(networkOnly).positive,
                   "network approvals require full review");

  const PendingRequestDescriptor additionalPermissions = descriptor(
      PendingRequestKind::CommandApproval,
      {{"additionalPermissions", {{"network", {{"enabled", true}}}}}});
  passed &= expect(
      PendingRequestPolicy::disclosure(additionalPermissions)
              .contains("additionalPermissions") &&
          !PendingRequestPolicy::controls(additionalPermissions).positive,
      "additional command permissions are disclosed only in full review");

  const PendingRequestDescriptor structuredForm =
      descriptor(PendingRequestKind::McpElicitation,
                 {{"serverName", "server"},
                  {"threadId", "thread-1"},
                  {"message", "Provide a value"},
                  {"mode", "form"},
                  {"requestedSchema",
                   {{"type", "object"},
                    {"properties", {{"answer", {{"type", "string"}}}}}}},
                  {"_meta", {{"codex_approval_kind", "mcp_tool_call"}}}});
  const auto structuredActions = PendingRequestPolicy::actions(structuredForm);
  passed &= expect(
      !structuredActions.empty() && structuredActions.front().requiresInput,
      "a structured MCP form cannot become a message-only approval");

  command.availability = PendingRequestAvailability::Actionable;
  controls = PendingRequestPolicy::controls(command);
  passed &= expect(controls.positive && !controls.negative &&
                       controls.directEnabled && controls.reviewEnabled,
                   "actionable compact controls have one policy projection");
  command.availability = PendingRequestAvailability::Submitting;
  controls = PendingRequestPolicy::controls(command);
  passed &= expect(controls.positive && !controls.negative &&
                       !controls.directEnabled && !controls.reviewEnabled,
                   "submitting controls remain stable and single-flight");
  command.availability = PendingRequestAvailability::RecoveryOnly;
  command.retainedSubmission = PendingRequestSubmission{"accept"};
  command.error = "The provider rejected the response.";
  controls = PendingRequestPolicy::controls(command);
  passed &=
      expect(!controls.positive && !controls.negative &&
                 !controls.directEnabled && controls.reviewEnabled,
             "retained authored input is reviewed without direct actions");
  passed &= expect(
      PendingRequestPolicy::status(command) ==
          "The provider rejected the response.  |  The authored response is "
          "retained for review.",
      "recovery status preserves both the failure and retained response facts");
  command.retainedSubmission.reset();
  passed &= expect(
      PendingRequestPolicy::status(command) ==
          "The provider rejected the response.  |  The request is no longer "
          "actionable.",
      "recovery status never invents authored input that was not retained");
  return passed;
}

bool verifyAttentionPolicy() {
  PendingRequestDescriptor recovery =
      descriptor(PendingRequestKind::CommandApproval);
  recovery.threadId = "selected";
  recovery.availability = PendingRequestAvailability::RecoveryOnly;
  recovery.retainedSubmission = PendingRequestSubmission{"accept"};
  PendingRequestDescriptor actionable = recovery;
  actionable.availability = PendingRequestAvailability::Actionable;
  actionable.retainedSubmission.reset();

  bool passed = true;
  std::vector requests{recovery, actionable};
  passed &= expect(PendingRequestPolicy::attentionIndex(requests, "selected") ==
                       std::optional<std::size_t>(1),
                   "current actionable request outranks selected recovery");

  requests.front().availability = PendingRequestAvailability::Submitting;
  requests.front().retainedSubmission.reset();
  requests.back().threadId = "other";
  passed &= expect(PendingRequestPolicy::attentionIndex(requests, "selected") ==
                       std::optional<std::size_t>(1),
                   "actionable work outranks selected submitting feedback");

  requests = {recovery, recovery};
  requests.back().availability = PendingRequestAvailability::Unavailable;
  requests.back().threadId = "other";
  passed &= expect(PendingRequestPolicy::attentionIndex(requests, "selected") ==
                       std::optional<std::size_t>(0),
                   "selected recovery outranks unavailable remote recovery");

  requests = {actionable, actionable};
  passed &= expect(PendingRequestPolicy::attentionIndex(requests, "selected") ==
                       std::optional<std::size_t>(0),
                   "equal attention candidates preserve graph order");
  passed &= expect(!PendingRequestPolicy::attentionIndex({}, "selected"),
                   "empty pending requests have no attention target");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= verifyKindsAndPresentation();
  passed &= verifyNativeControlsAndRecovery();
  passed &= verifyAttentionPolicy();
  return passed ? 0 : 1;
}
