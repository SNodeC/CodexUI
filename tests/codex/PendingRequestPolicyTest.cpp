// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PendingRequestPolicy.h"

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using codexui::codex::PendingRequestPolicy;
using codexui::codex::PendingRequestResponse;

bool expect(bool condition, std::string_view message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

nlohmann::json error(std::string message) {
  return {{"code", -32601}, {"message", std::move(message)}};
}

nlohmann::json denied() {
  return {{"decision", {{"denied", {{"rejection", "Denied by user"}}}}}};
}

nlohmann::json failedTool(std::string text) {
  return {{"contentItems",
           nlohmann::json::array(
               {{{"type", "inputText"}, {"text", std::move(text)}}})},
          {"success", false}};
}

bool expectResponse(std::string_view name, const PendingRequestResponse &actual,
                    const nlohmann::json &result,
                    const nlohmann::json &responseError = nullptr) {
  const bool matches = actual.result == result && actual.error == responseError;
  if (!matches) {
    std::cerr << "Expected result " << result.dump() << " and error "
              << responseError.dump() << ", got result " << actual.result.dump()
              << " and error " << actual.error.dump() << '\n';
  }
  return expect(matches, name);
}

bool verifyPresentationMetadata() {
  struct TitleCase {
    std::string_view kind;
    std::string_view title;
    std::string_view dialogTitle;
  };
  constexpr std::array titles{
      TitleCase{"command-approval", "Command approval requested",
                "Command approval"},
      TitleCase{"file-change-approval", "File-change approval requested",
                "File-change approval"},
      TitleCase{"user-input", "Codex needs input", "Codex needs input"},
      TitleCase{"mcp-elicitation", "MCP server request", "MCP server request"},
      TitleCase{"permissions-approval", "Permission request",
                "Permission request"},
      TitleCase{"dynamic-tool-call", "Codex request needs attention",
                "Dynamic tool request"},
      TitleCase{"authentication-refresh", "Codex request needs attention",
                "Authentication refresh"},
      TitleCase{"attestation", "Codex request needs attention",
                "Attestation request"},
      TitleCase{"legacy-patch-approval", "Legacy patch approval",
                "Legacy patch approval"},
      TitleCase{"legacy-command-approval", "Legacy command approval",
                "Legacy command approval"},
      TitleCase{"unsupported", "Codex request needs attention",
                "Unsupported Codex request"},
  };

  bool passed = true;
  for (const TitleCase &entry : titles) {
    passed &= expect(PendingRequestPolicy::title(entry.kind) == entry.title &&
                         PendingRequestPolicy::dialogTitle(entry.kind) ==
                             entry.dialogTitle,
                     std::string("titles: ") + std::string(entry.kind));
  }

  constexpr std::array directAcceptKinds{
      "command-approval", "file-change-approval", "permissions-approval",
      "legacy-patch-approval", "legacy-command-approval"};
  for (const std::string_view kind : directAcceptKinds)
    passed &= expect(PendingRequestPolicy::supportsDirectAccept(kind),
                     std::string("direct accept: ") + std::string(kind));
  passed &= expect(!PendingRequestPolicy::supportsDirectAccept("user-input") &&
                       PendingRequestPolicy::directAcceptLabel(
                           "permissions-approval") == "Allow this turn" &&
                       PendingRequestPolicy::directAcceptLabel(
                           "command-approval") == "Accept",
                   "direct-accept capability and labels are exact");

  const nlohmann::json request{{"command", "make test"},
                               {"reason", "review"},
                               {"message", "Need confirmation"},
                               {"cwd", "/repo"},
                               {"grantRoot", "/repo"},
                               {"permissions", {{"network", true}}},
                               {"questions", nlohmann::json::array({1, 2})}};
  const std::string actualDetail =
      PendingRequestPolicy::detail("request-1", "thread-1", request);
  const std::string expectedDetail =
      "Command: make test  |  Reason: review  |  Need confirmation  |  "
      "Directory: /repo  |  Grant root: /repo  |  Permissions: "
      "{\n\"network\": true\n}  |  2 questions";
  if (actualDetail != expectedDetail)
    std::cerr << "Expected detail " << expectedDetail << ", got "
              << actualDetail << '\n';
  passed &=
      expect(actualDetail == expectedDetail,
             "request detail preserves the existing field order and labels");
  passed &= expect(PendingRequestPolicy::detail("request-2", "thread-2",
                                                nlohmann::json::object()) ==
                       "Request request-2 for thread thread-2",
                   "empty request detail uses identity fallback");
  return passed;
}

bool verifySubmissionResponses() {
  bool passed = true;
  const nlohmann::json permissions{{"network", {{"enabled", true}}}};
  const nlohmann::json request{{"permissions", permissions}};
  const nlohmann::json answers{
      {"question", {{"answers", nlohmann::json::array({"yes"})}}}};
  const nlohmann::json content{{"accepted", true}};

  passed &= expectResponse(
      "command submission response",
      PendingRequestPolicy::responseForSubmission(
          "command-approval", nlohmann::json::object(), "cancel"),
      {{"decision", "cancel"}});
  passed &= expectResponse(
      "file-change submission response",
      PendingRequestPolicy::responseForSubmission(
          "file-change-approval", nlohmann::json::object(), "acceptForSession"),
      {{"decision", "acceptForSession"}});
  passed &=
      expectResponse("user-input submission response",
                     PendingRequestPolicy::responseForSubmission(
                         "user-input", nlohmann::json::object(), {}, answers),
                     {{"answers", answers}});
  passed &= expectResponse(
      "MCP accepted submission response",
      PendingRequestPolicy::responseForSubmission(
          "mcp-elicitation", nlohmann::json::object(), "accept", content),
      {{"action", "accept"}, {"content", content}, {"_meta", nullptr}});
  passed &= expectResponse(
      "MCP cancelled submission response",
      PendingRequestPolicy::responseForSubmission(
          "mcp-elicitation", nlohmann::json::object(), "cancel", content),
      {{"action", "cancel"}, {"content", nullptr}, {"_meta", nullptr}});
  passed &=
      expectResponse("permission accepted submission response",
                     PendingRequestPolicy::responseForSubmission(
                         "permissions-approval", request, "session"),
                     {{"permissions", permissions}, {"scope", "session"}});
  passed &= expectResponse("permission declined submission response",
                           PendingRequestPolicy::responseForSubmission(
                               "permissions-approval", request, "decline"),
                           nlohmann::json::object(),
                           error("Permission request declined by user"));
  passed &=
      expectResponse("legacy patch approved-for-session response",
                     PendingRequestPolicy::responseForSubmission(
                         "legacy-patch-approval", nlohmann::json::object(),
                         "approved_for_session"),
                     {{"decision", "approved_for_session"}});
  passed &= expectResponse(
      "legacy patch denied response",
      PendingRequestPolicy::responseForSubmission(
          "legacy-patch-approval", nlohmann::json::object(), "denied"),
      denied());
  passed &= expectResponse(
      "legacy command abort response",
      PendingRequestPolicy::responseForSubmission(
          "legacy-command-approval", nlohmann::json::object(), "abort"),
      {{"decision", "abort"}});
  passed &=
      expectResponse("dynamic-tool unavailable response",
                     PendingRequestPolicy::responseForSubmission(
                         "dynamic-tool-call", nlohmann::json::object()),
                     failedTool("CodexUI does not provide this dynamic tool"));
  for (const std::string_view kind :
       {"authentication-refresh", "attestation", "unsupported"}) {
    passed &=
        expectResponse(std::string(kind) + " unsupported submission response",
                       PendingRequestPolicy::responseForSubmission(
                           kind, nlohmann::json::object()),
                       nlohmann::json::object(),
                       error("CodexUI does not support this server request"));
  }
  return passed;
}

bool verifyCanonicalResponses() {
  struct ResponseCase {
    std::string_view kind;
    nlohmann::json request;
    nlohmann::json positiveResult;
    nlohmann::json positiveError;
    nlohmann::json negativeResult;
    nlohmann::json negativeError;
  };
  const nlohmann::json cannotApprove =
      error("CodexUI cannot directly approve this server request");
  const nlohmann::json declined = error("Request declined by user");
  const nlohmann::json empty = nlohmann::json::object();
  const nlohmann::json permissions{{"network", true}};
  const std::array cases{
      ResponseCase{"command-approval",
                   empty,
                   {{"decision", "accept"}},
                   nullptr,
                   {{"decision", "decline"}},
                   nullptr},
      ResponseCase{"file-change-approval",
                   empty,
                   {{"decision", "accept"}},
                   nullptr,
                   {{"decision", "decline"}},
                   nullptr},
      ResponseCase{"user-input", empty, empty, cannotApprove, empty, declined},
      ResponseCase{
          "mcp-elicitation",
          empty,
          empty,
          cannotApprove,
          {{"action", "decline"}, {"content", nullptr}, {"_meta", nullptr}},
          nullptr},
      ResponseCase{"permissions-approval",
                   {{"permissions", permissions}},
                   {{"permissions", permissions}, {"scope", "turn"}},
                   nullptr,
                   empty,
                   declined},
      ResponseCase{"legacy-patch-approval",
                   empty,
                   {{"decision", "approved"}},
                   nullptr,
                   denied(),
                   nullptr},
      ResponseCase{"legacy-command-approval",
                   empty,
                   {{"decision", "approved"}},
                   nullptr,
                   denied(),
                   nullptr},
      ResponseCase{"dynamic-tool-call", empty, empty, cannotApprove,
                   failedTool("Request declined by user"), nullptr},
      ResponseCase{"authentication-refresh", empty, empty, cannotApprove, empty,
                   declined},
      ResponseCase{"attestation", empty, empty, cannotApprove, empty, declined},
      ResponseCase{"unsupported", empty, empty, cannotApprove, empty, declined},
  };

  bool passed = true;
  for (const ResponseCase &entry : cases) {
    passed &= expectResponse(
        std::string(entry.kind) + " direct positive response",
        PendingRequestPolicy::positiveResponse(entry.kind, entry.request),
        entry.positiveResult, entry.positiveError);
    passed &= expectResponse(
        std::string(entry.kind) + " negative response",
        PendingRequestPolicy::negativeResponse(entry.kind, entry.request),
        entry.negativeResult, entry.negativeError);
  }
  return passed;
}

} // namespace

int main() {
  const bool passed = verifyPresentationMetadata() &&
                      verifySubmissionResponses() && verifyCanonicalResponses();
  return passed ? 0 : 1;
}
