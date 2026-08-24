// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ProtocolNormalizer.h"

#include <ai/openai/codex/protocol/Envelope.h>
#include <ai/openai/codex/protocol/generated/ProtocolTypes.h>

#include <array>
#include <optional>
#include <utility>

namespace codexui::codex {
namespace {

using ai::openai::codex::protocol::JsonRpcKind;
using presentation::Authority;
using namespace std::string_view_literals;

nlohmann::json errorValue(const nlohmann::json &response) {
  const auto iterator = response.find("error");
  return iterator != response.end()
             ? *iterator
             : nlohmann::json{{"code", -32000},
                              {"message", "operation failed"}};
}

nlohmann::json stableScope(const nlohmann::json &value) {
  nlohmann::json scope = nlohmann::json::object();
  if (!value.is_object())
    return scope;
  constexpr std::array keys{"threadId", "turnId", "itemId", "processId",
                            "requestId"};
  for (const char *key : keys) {
    const auto iterator = value.find(key);
    if (iterator != value.end() && !iterator->is_null())
      scope[key] = *iterator;
  }
  return scope;
}

std::string requestKind(std::string_view method) {
  if (method == "item/commandExecution/requestApproval")
    return "command-approval";
  if (method == "item/fileChange/requestApproval")
    return "file-change-approval";
  if (method == "item/tool/requestUserInput")
    return "user-input";
  if (method == "mcpServer/elicitation/request")
    return "mcp-elicitation";
  if (method == "item/permissions/requestApproval")
    return "permissions-approval";
  if (method == "item/tool/call")
    return "dynamic-tool-call";
  if (method == "account/chatgptAuthTokens/refresh")
    return "authentication-refresh";
  if (method == "attestation/generate")
    return "attestation";
  if (method == "applyPatchApproval")
    return "legacy-patch-approval";
  if (method == "execCommandApproval")
    return "legacy-command-approval";
  return "unsupported";
}

struct EventDescriptor {
  std::string_view type;
  Authority authority = Authority::None;
};

std::optional<EventDescriptor> remainingNotification(std::string_view method) {
  // Every generated notification without a richer reducer above has one stable
  // presentation-domain event name. Native app-server method names stop here.
  static constexpr std::array descriptors{
      std::pair{"thread/reverted"sv,
                EventDescriptor{"thread.reverted", Authority::Merge}},
      std::pair{"skills/changed"sv,
                EventDescriptor{"catalog.skills.invalidated", Authority::None}},
      std::pair{"thread/goal/updated"sv,
                EventDescriptor{"thread.goal.changed", Authority::Replace}},
      std::pair{"thread/goal/cleared"sv,
                EventDescriptor{"thread.goal.removed", Authority::Remove}},
      std::pair{"thread/queue/changed"sv,
                EventDescriptor{"thread.queue.changed", Authority::Replace}},
      std::pair{"project/changed"sv,
                EventDescriptor{"workspace.project.changed", Authority::Merge}},
      std::pair{"thread/project/updated"sv,
                EventDescriptor{"thread.project.changed", Authority::Replace}},
      std::pair{
          "thread/environment/connected"sv,
          EventDescriptor{"thread.environment.connected", Authority::Merge}},
      std::pair{
          "thread/environment/disconnected"sv,
          EventDescriptor{"thread.environment.disconnected", Authority::Merge}},
      std::pair{"thread/settings/updated"sv,
                EventDescriptor{"thread.settings.changed", Authority::Merge}},
      std::pair{"hook/started"sv,
                EventDescriptor{"activity.hook.started", Authority::Merge}},
      std::pair{"hook/completed"sv,
                EventDescriptor{"activity.hook.completed", Authority::Merge}},
      std::pair{"turn/diff/updated"sv,
                EventDescriptor{"turn.diff.changed", Authority::Replace}},
      std::pair{"item/autoApprovalReview/started"sv,
                EventDescriptor{"approval.review.started", Authority::Merge}},
      std::pair{"item/autoApprovalReview/completed"sv,
                EventDescriptor{"approval.review.completed", Authority::Merge}},
      std::pair{
          "autoApprovalReview/strictReviewRequired"sv,
          EventDescriptor{"approval.strict-review.required", Authority::Merge}},
      std::pair{"command/exec/outputDelta"sv,
                EventDescriptor{"terminal.command.output-appended",
                                Authority::Merge}},
      std::pair{"process/outputDelta"sv,
                EventDescriptor{"terminal.process.output-appended",
                                Authority::Merge}},
      std::pair{
          "process/exited"sv,
          EventDescriptor{"terminal.process.completed", Authority::Merge}},
      std::pair{"item/commandExecution/terminalInteraction"sv,
                EventDescriptor{"conversation.command.interaction",
                                Authority::Merge}},
      std::pair{"item/fileChange/outputDelta"sv,
                EventDescriptor{"conversation.file-change.output-appended",
                                Authority::Merge}},
      std::pair{"item/fileChange/patchUpdated"sv,
                EventDescriptor{"conversation.file-change.patch-replaced",
                                Authority::Replace}},
      std::pair{"item/mcpToolCall/progress"sv,
                EventDescriptor{"conversation.mcp.progress", Authority::Merge}},
      std::pair{
          "mcpServer/oauthLogin/completed"sv,
          EventDescriptor{"integration.mcp.login-completed", Authority::Merge}},
      std::pair{
          "mcpServer/startupStatus/updated"sv,
          EventDescriptor{"integration.mcp.status-changed", Authority::Merge}},
      std::pair{"mcpServer/event/stream/notification"sv,
                EventDescriptor{"integration.mcp.event", Authority::None}},
      std::pair{"app/list/updated"sv,
                EventDescriptor{"catalog.apps.changed", Authority::Replace}},
      std::pair{"remoteControl/status/changed"sv,
                EventDescriptor{"connection.remote-control.changed",
                                Authority::Replace}},
      std::pair{"externalAgentConfig/import/progress"sv,
                EventDescriptor{"settings.external-agent-import.progress",
                                Authority::Merge}},
      std::pair{"externalAgentConfig/import/completed"sv,
                EventDescriptor{"settings.external-agent-import.completed",
                                Authority::Merge}},
      std::pair{"fs/changed"sv,
                EventDescriptor{"workspace.files.changed", Authority::Merge}},
      std::pair{"item/reasoning/summaryPartAdded"sv,
                EventDescriptor{"conversation.reasoning.part-added",
                                Authority::Merge}},
      std::pair{"thread/compacted"sv,
                EventDescriptor{"thread.compacted", Authority::Merge}},
      std::pair{"model/rerouted"sv,
                EventDescriptor{"model.rerouted", Authority::Merge}},
      std::pair{
          "model/verification"sv,
          EventDescriptor{"model.verification.changed", Authority::Merge}},
      std::pair{"turn/moderationMetadata"sv,
                EventDescriptor{"turn.moderation.changed", Authority::Replace}},
      std::pair{"model/safetyBuffering/updated"sv,
                EventDescriptor{"model.safety-buffering.changed",
                                Authority::Replace}},
      std::pair{"fuzzyFileSearch/sessionUpdated"sv,
                EventDescriptor{"workspace.search.changed", Authority::Merge}},
      std::pair{
          "fuzzyFileSearch/sessionCompleted"sv,
          EventDescriptor{"workspace.search.completed", Authority::Merge}},
      std::pair{"thread/realtime/started"sv,
                EventDescriptor{"realtime.session.started", Authority::Merge}},
      std::pair{"thread/realtime/itemAdded"sv,
                EventDescriptor{"realtime.item.added", Authority::Merge}},
      std::pair{
          "thread/realtime/transcript/delta"sv,
          EventDescriptor{"realtime.transcript.appended", Authority::Merge}},
      std::pair{
          "thread/realtime/transcript/done"sv,
          EventDescriptor{"realtime.transcript.completed", Authority::Merge}},
      std::pair{"thread/realtime/outputAudio/delta"sv,
                EventDescriptor{"realtime.audio.appended", Authority::Merge}},
      std::pair{"thread/realtime/sdp"sv,
                EventDescriptor{"realtime.session-description.changed",
                                Authority::Replace}},
      std::pair{"thread/realtime/error"sv,
                EventDescriptor{"realtime.session.failed", Authority::Merge}},
      std::pair{"thread/realtime/closed"sv,
                EventDescriptor{"realtime.session.closed", Authority::Merge}},
      std::pair{"windows/worldWritableWarning"sv,
                EventDescriptor{"system.windows-permission.warning",
                                Authority::None}},
      std::pair{"windowsSandbox/setupCompleted"sv,
                EventDescriptor{"system.windows-sandbox.completed",
                                Authority::Merge}},
      std::pair{"account/login/completed"sv,
                EventDescriptor{"account.login.completed", Authority::Merge}},
  };
  for (const auto &[name, descriptor] : descriptors) {
    if (method == name)
      return descriptor;
  }
  return std::nullopt;
}

} // namespace

ProtocolNormalizer::ProtocolNormalizer(Sink sink) : sink(std::move(sink)) {}

void ProtocolNormalizer::transportEvent(std::string_view eventName,
                                        std::string detail) {
  if (eventName == "connected")
    ++connectionGeneration;
  nlohmann::json data{{"state", eventName}};
  if (!detail.empty())
    data["detail"] = std::move(detail);
  emitEvent("connection.lifecycle", std::move(data));
}

void ProtocolNormalizer::connectionSettings(nlohmann::json settings) {
  emitEvent("connection.settings.changed", std::move(settings),
            Authority::Replace);
}

void ProtocolNormalizer::localOperationResult(std::string action,
                                              std::string correlationId,
                                              bool ok, nlohmann::json data) {
  emit(presentation::result(nextSequence++, connectionGeneration,
                            std::move(action), std::move(correlationId), ok,
                            std::move(data)));
}

void ProtocolNormalizer::bridgeEvent(const nlohmann::json &value) {
  const std::string kind = presentation::stringMember(value, "kind");
  if (kind == "bridge.connection") {
    emitEvent("connection.bridge",
              {{"state", value.value("event", std::string{})},
               {"connectionId", value.value("connectionId", std::string{})},
               {"role", value.value("role", std::string{})}});
    return;
  }
  if (kind == "bridge.controller") {
    emitEvent("connection.controller",
              {{"controllerConnectionId",
                presentation::member(value, "controllerConnectionId")}},
              Authority::Replace);
    return;
  }
  if (kind == "bridge.diagnostic") {
    diagnostic("bridge", value.value("code", std::string{}),
               value.value("message", std::string{}),
               value.value("details", nlohmann::json::object()));
    return;
  }
  diagnostic("bridge", "unknown-event", kind, value);
}

void ProtocolNormalizer::serverNotification(std::string_view method,
                                            const nlohmann::json &params) {
  const nlohmann::json scope = stableScope(params);
  if (method == "thread/started") {
    emitEvent("thread.upsert",
              {{"thread", params.value("thread", nlohmann::json::object())}},
              Authority::Merge);
  } else if (method == "thread/status/changed") {
    emitEvent("thread.status.changed",
              {{"status", presentation::member(params, "status")}},
              Authority::Merge, scope);
  } else if (method == "thread/name/updated") {
    emitEvent("thread.name.changed",
              {{"name", presentation::member(params, "threadName")}},
              Authority::Replace, scope);
  } else if (method == "thread/deleted") {
    emitEvent("thread.removed", nlohmann::json::object(), Authority::Remove,
              scope);
  } else if (method == "thread/archived" || method == "thread/unarchived" ||
             method == "thread/closed") {
    const std::string state = method == "thread/archived"     ? "archived"
                              : method == "thread/unarchived" ? "unarchived"
                                                              : "closed";
    emitEvent("thread.lifecycle", {{"state", state}}, Authority::Merge, scope);
  } else if (method == "turn/started" || method == "turn/completed") {
    emitEvent(
        "turn.upsert",
        {{"lifecycle", method == "turn/started" ? "started" : "completed"},
         {"turn", params.value("turn", nlohmann::json::object())}},
        Authority::Merge, scope);
  } else if (method == "turn/plan/updated") {
    emitEvent("plan.replaced",
              {{"explanation", presentation::member(params, "explanation")},
               {"steps", params.value("plan", nlohmann::json::array())}},
              Authority::Replace, scope);
  } else if (method == "item/started" || method == "item/completed") {
    const nlohmann::json item = params.value("item", nlohmann::json::object());
    nlohmann::json itemScope = scope;
    if (!itemScope.contains("itemId") && item.contains("id") &&
        !item["id"].is_null())
      itemScope["itemId"] = item["id"];
    emitEvent(
        "conversation.item.upsert",
        {{"lifecycle", method == "item/started" ? "started" : "completed"},
         {"item", item}},
        Authority::Merge, itemScope);
    const std::string itemType = item.value("type", std::string{});
    if (itemType == "collabAgentToolCall" || itemType == "subAgentActivity") {
      emitEvent(
          "agents.activity.upsert",
          {{"lifecycle", method == "item/started" ? "started" : "completed"},
           {"activity", item}},
          Authority::Merge, itemScope);
    }
  } else if (method == "item/agentMessage/delta" ||
             method == "item/plan/delta" ||
             method == "item/reasoning/summaryTextDelta" ||
             method == "item/reasoning/textDelta" ||
             method == "item/commandExecution/outputDelta") {
    std::string field = "text";
    if (method == "item/commandExecution/outputDelta")
      field = "aggregatedOutput";
    else if (method == "item/reasoning/summaryTextDelta")
      field = "summary";
    else if (method == "item/reasoning/textDelta")
      field = "content";
    nlohmann::json data{{"field", std::move(field)},
                        {"text", params.value("delta", std::string{})}};
    if (params.contains("summaryIndex"))
      data["summaryIndex"] = params["summaryIndex"];
    if (params.contains("contentIndex"))
      data["contentIndex"] = params["contentIndex"];
    emitEvent("conversation.item.append", std::move(data), Authority::Merge,
              scope);
  } else if (method == "serverRequest/resolved") {
    emitEvent("pending-request.removed", nlohmann::json::object(),
              Authority::Remove, scope);
  } else if (method == "error" || method == "warning" ||
             method == "guardianWarning" || method == "configWarning" ||
             method == "deprecationNotice") {
    emitEvent("notice.added",
              {{"severity", method == "error" ? "error" : "warning"},
               {"notice", params}},
              Authority::None, scope);
  } else if (method == "thread/tokenUsage/updated") {
    emitEvent(
        "thread.token-usage.changed",
        {{"tokenUsage", params.value("tokenUsage", nlohmann::json::object())}},
        Authority::Replace, scope);
  } else if (method == "account/updated") {
    emitEvent("account.changed", {{"account", params}}, Authority::Replace);
  } else if (method == "account/rateLimits/updated") {
    emitEvent("account.rate-limits.changed", {{"rateLimits", params}},
              Authority::Replace);
  } else if (const auto descriptor = remainingNotification(method)) {
    emitEvent(std::string(descriptor->type), params, descriptor->authority,
              scope);
  } else {
    diagnostic("appserver", "unmapped-notification", std::string(method));
  }
}

void ProtocolNormalizer::serverRequest(std::string_view method,
                                       const nlohmann::json &requestId,
                                       const nlohmann::json &params) {
  nlohmann::json scope = stableScope(params);
  scope["requestId"] = requestId;
  emitEvent("pending-request.upsert",
            {{"requestId", requestId},
             {"category", requestKind(method)},
             {"request", params}},
            Authority::Merge, std::move(scope));
}

void ProtocolNormalizer::observeRawInbound(const nlohmann::json &message) {
  const auto method = ai::openai::codex::protocol::jsonRpcMethod(message);
  const JsonRpcKind kind =
      ai::openai::codex::protocol::classifyJsonRpc(message);
  if ((kind == JsonRpcKind::Request || kind == JsonRpcKind::Notification) &&
      method && !knownServerMethod(*method))
    diagnostic("appserver", "unknown-method", *method);
}

void ProtocolNormalizer::operationResult(std::string action,
                                         std::string correlationId,
                                         nlohmann::json context,
                                         const nlohmann::json &response) {
  const bool ok = response.is_object() && response.contains("result");
  nlohmann::json data;
  Authority authority = Authority::None;
  nlohmann::json scope = stableScope(context);
  if (ok) {
    const nlohmann::json &value = response["result"];
    if (action == "threads.list") {
      data = {
          {"threads", value.value("data", nlohmann::json::array())},
          {"nextCursor", presentation::member(value, "nextCursor")},
          {"backwardsCursor", presentation::member(value, "backwardsCursor")}};
      authority = Authority::Merge;
    } else if (action == "thread.read") {
      const nlohmann::json thread =
          value.value("thread", nlohmann::json::object());
      data = {{"thread", thread}};
      authority = Authority::Merge;
      const std::string threadId = presentation::stringMember(thread, "id");
      if (!threadId.empty())
        scope["threadId"] = threadId;
    } else if (action == "thread.create" || action == "thread.resume" ||
               action == "thread.fork") {
      data = {{"thread", value.value("thread", nlohmann::json::object())}};
      authority = Authority::Merge;
    } else if (action == "models.list") {
      data = {{"models", value.value("data", nlohmann::json::array())},
              {"nextCursor", presentation::member(value, "nextCursor")}};
      authority = Authority::Replace;
    } else if (action == "model-provider-capabilities.read" ||
               action == "account.read" ||
               action == "account.rate-limits.read" ||
               action == "account.token-usage.read" ||
               action == "config.read" ||
               action == "permission-profiles.list" ||
               action == "experimental-features.list" ||
               action == "skills.list" || action == "hooks.list" ||
               action == "plugins.list" || action == "apps.list" ||
               action == "mcp-servers.list") {
      data = value;
      authority = Authority::Replace;
    } else if (action.ends_with(".list") || action.ends_with(".read") ||
               action.ends_with(".get") || action == "plugins.installed" ||
               action == "apps.installed" ||
               action == "windows-sandbox.readiness") {
      data = value;
      authority = Authority::Replace;
    } else if (action == "turn.start") {
      data = {{"turn", value.value("turn", nlohmann::json::object())}};
      authority = Authority::Merge;
    } else {
      data = value;
    }
  } else {
    data = errorValue(response);
  }
  emit(presentation::result(nextSequence++, connectionGeneration,
                            std::move(action), std::move(correlationId), ok,
                            std::move(data), authority, std::move(scope)));
}

void ProtocolNormalizer::operationRejected(std::string action,
                                           std::string correlationId, int code,
                                           std::string message) {
  emit(presentation::result(nextSequence++, connectionGeneration,
                            std::move(action), std::move(correlationId), false,
                            {{"code", code}, {"message", std::move(message)}}));
}

bool ProtocolNormalizer::emit(nlohmann::json frame) const {
  return sink && sink(frame);
}

bool ProtocolNormalizer::emitEvent(std::string type, nlohmann::json data,
                                   Authority authority, nlohmann::json scope) {
  return emit(presentation::event(nextSequence++, connectionGeneration,
                                  std::move(type), std::move(data), authority,
                                  std::move(scope)));
}

void ProtocolNormalizer::diagnostic(std::string source, std::string code,
                                    std::string message,
                                    nlohmann::json details) {
  emitEvent("system.diagnostic", {{"source", std::move(source)},
                                  {"code", std::move(code)},
                                  {"message", std::move(message)},
                                  {"details", std::move(details)}});
}

bool ProtocolNormalizer::knownServerMethod(std::string_view method) const {
#define CODEXUI_MATCH_SERVER_REQUEST(OperationName, methodName)                \
  if (method ==                                                                \
      ai::openai::codex::generated::server_requests::OperationName::method)    \
    return true;
  AI_OPENAI_CODEX_SERVER_REQUESTS(CODEXUI_MATCH_SERVER_REQUEST)
#undef CODEXUI_MATCH_SERVER_REQUEST
#define CODEXUI_MATCH_SERVER_NOTIFICATION(OperationName, methodName)           \
  if (method == ai::openai::codex::generated::server_notifications::           \
                    OperationName::method)                                     \
    return true;
  AI_OPENAI_CODEX_SERVER_NOTIFICATIONS(CODEXUI_MATCH_SERVER_NOTIFICATION)
#undef CODEXUI_MATCH_SERVER_NOTIFICATION
  return false;
}

} // namespace codexui::codex
