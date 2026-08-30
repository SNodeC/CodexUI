// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PendingRequestPolicy.h"

#include <utility>
#include <vector>

namespace codexui::codex {
namespace {

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

nlohmann::json memberValue(const nlohmann::json &object, const char *key,
                           nlohmann::json fallback) {
  if (!object.is_object())
    return fallback;
  const auto found = object.find(key);
  return found == object.end() ? fallback : *found;
}

nlohmann::json jsonRpcError(std::string message) {
  return {{"code", -32601}, {"message", std::move(message)}};
}

void appendDetail(std::vector<std::string> &parts, std::string label,
                  std::string value) {
  if (!value.empty())
    parts.push_back(std::move(label) + value);
}

std::string joinDetails(const std::vector<std::string> &parts) {
  std::string result;
  for (const std::string &part : parts) {
    if (!result.empty())
      result += "  |  ";
    result += part;
  }
  return result;
}

} // namespace

std::string PendingRequestPolicy::title(std::string_view kind) {
  if (kind == "command-approval")
    return "Command approval requested";
  if (kind == "file-change-approval")
    return "File-change approval requested";
  if (kind == "permissions-approval")
    return "Permission request";
  if (kind == "user-input")
    return "Codex needs input";
  if (kind == "mcp-elicitation")
    return "MCP server request";
  if (kind == "legacy-patch-approval")
    return "Legacy patch approval";
  if (kind == "legacy-command-approval")
    return "Legacy command approval";
  return "Codex request needs attention";
}

std::string PendingRequestPolicy::dialogTitle(std::string_view kind) {
  if (kind == "command-approval")
    return "Command approval";
  if (kind == "file-change-approval")
    return "File-change approval";
  if (kind == "user-input")
    return "Codex needs input";
  if (kind == "mcp-elicitation")
    return "MCP server request";
  if (kind == "permissions-approval")
    return "Permission request";
  if (kind == "dynamic-tool-call")
    return "Dynamic tool request";
  if (kind == "authentication-refresh")
    return "Authentication refresh";
  if (kind == "attestation")
    return "Attestation request";
  if (kind == "legacy-patch-approval")
    return "Legacy patch approval";
  if (kind == "legacy-command-approval")
    return "Legacy command approval";
  return "Unsupported Codex request";
}

std::string PendingRequestPolicy::detail(std::string_view requestId,
                                         std::string_view threadId,
                                         const nlohmann::json &request) {
  std::vector<std::string> parts;
  appendDetail(parts, "Command: ", stringValue(request, "command"));
  appendDetail(parts, "Reason: ", stringValue(request, "reason"));
  appendDetail(parts, {}, stringValue(request, "message"));
  appendDetail(parts, "Directory: ", stringValue(request, "cwd"));
  appendDetail(parts, "Grant root: ", stringValue(request, "grantRoot"));

  if (request.is_object()) {
    const auto permissions = request.find("permissions");
    if (permissions != request.end() && !permissions->is_null())
      parts.push_back("Permissions: " + permissions->dump(0));
    const auto questions = request.find("questions");
    if (questions != request.end() && questions->is_array())
      parts.push_back(std::to_string(questions->size()) + " questions");
  }

  if (!parts.empty())
    return joinDetails(parts);
  return "Request " + std::string(requestId) + " for thread " +
         std::string(threadId);
}

bool PendingRequestPolicy::supportsDirectAccept(
    std::string_view kind) noexcept {
  return kind == "command-approval" || kind == "file-change-approval" ||
         kind == "permissions-approval" || kind == "legacy-patch-approval" ||
         kind == "legacy-command-approval";
}

std::string PendingRequestPolicy::directAcceptLabel(std::string_view kind) {
  return kind == "permissions-approval" ? "Allow this turn" : "Accept";
}

PendingRequestResponse PendingRequestPolicy::responseForSubmission(
    std::string_view kind, const nlohmann::json &request, std::string decision,
    nlohmann::json input) {
  PendingRequestResponse response;
  if (kind == "command-approval" || kind == "file-change-approval") {
    response.result = {{"decision", std::move(decision)}};
  } else if (kind == "user-input") {
    response.result = {{"answers", std::move(input)}};
  } else if (kind == "mcp-elicitation") {
    const bool acceptsContent = decision == "accept";
    response.result = {{"action", std::move(decision)},
                       {"content", acceptsContent ? std::move(input)
                                                  : nlohmann::json(nullptr)},
                       {"_meta", nullptr}};
  } else if (kind == "permissions-approval") {
    if (decision == "decline") {
      response.error = jsonRpcError("Permission request declined by user");
    } else {
      response.result = {{"permissions", memberValue(request, "permissions",
                                                     nlohmann::json::object())},
                         {"scope", std::move(decision)}};
    }
  } else if (kind == "legacy-patch-approval" ||
             kind == "legacy-command-approval") {
    if (decision == "approved" || decision == "approved_for_session")
      response.result = {{"decision", std::move(decision)}};
    else if (decision == "denied")
      response.result = {
          {"decision", {{"denied", {{"rejection", "Denied by user"}}}}}};
    else
      response.result = {{"decision", "abort"}};
  } else if (kind == "dynamic-tool-call") {
    response.result = {
        {"contentItems",
         nlohmann::json::array(
             {{{"type", "inputText"},
               {"text", "CodexUI does not provide this dynamic tool"}}})},
        {"success", false}};
  } else {
    response.error =
        jsonRpcError("CodexUI does not support this server request");
  }
  return response;
}

PendingRequestResponse
PendingRequestPolicy::negativeResponse(std::string_view kind,
                                       const nlohmann::json &request) {
  if (kind == "command-approval" || kind == "file-change-approval")
    return responseForSubmission(kind, request, "decline");
  if (kind == "mcp-elicitation")
    return responseForSubmission(kind, request, "decline");
  if (kind == "legacy-patch-approval" || kind == "legacy-command-approval")
    return responseForSubmission(kind, request, "denied");
  if (kind == "dynamic-tool-call") {
    PendingRequestResponse response;
    response.result = {
        {"contentItems",
         nlohmann::json::array(
             {{{"type", "inputText"}, {"text", "Request declined by user"}}})},
        {"success", false}};
    return response;
  }

  PendingRequestResponse response;
  response.error = jsonRpcError("Request declined by user");
  return response;
}

PendingRequestResponse
PendingRequestPolicy::positiveResponse(std::string_view kind,
                                       const nlohmann::json &request) {
  if (kind == "command-approval" || kind == "file-change-approval")
    return responseForSubmission(kind, request, "accept");
  if (kind == "permissions-approval")
    return responseForSubmission(kind, request, "turn");
  if (kind == "legacy-patch-approval" || kind == "legacy-command-approval")
    return responseForSubmission(kind, request, "approved");

  PendingRequestResponse response;
  response.error =
      jsonRpcError("CodexUI cannot directly approve this server request");
  return response;
}

} // namespace codexui::codex
