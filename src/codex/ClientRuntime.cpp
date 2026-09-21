// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ClientRuntime.h"

#include "codex/Configuration.h"
#include "codex/InternalProtocolOperations.h"
#include "codex/NodeGraphJson.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/WorkerMailboxReceiver.h"
#include "codex/nodegraph/PromptText.h"
#include "codex/nodegraph/WorkerLogic.h"

#include <ai/openai/codex/frontend/CodexBridge.h>
#include <ai/openai/codex/frontend/client/ClientConnection.h>
#include <ai/openai/codex/frontend/client/StreamSocketContextFactory.h>
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
#include <ai/openai/codex/frontend/client/WebSocketClient.h>
#endif
#include <ai/openai/codex/protocol/RuntimePaths.h>
#include <core/EventReceiver.h>
#include <core/SNodeC.h>
#include <core/socket/State.h>
#include <core/socket/stream/ClientFlowController.h>
#include <core/timer/Timer.h>
#include <net/in/stream/legacy/SocketClient.h>
#include <net/in6/stream/legacy/SocketClient.h>
#include <net/un/stream/legacy/SocketClient.h>
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
#include <net/in/stream/tls/SocketClient.h>
#include <net/in6/stream/tls/SocketClient.h>
#endif
#if defined(CODEXUI_CODEX_FRONTEND_RFCOMM)
#include <net/rc/stream/legacy/SocketClient.h>
#include <net/rc/stream/tls/SocketClient.h>
#endif
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
#include <web/http/client/Request.h>
#endif
#include <utils/Timeval.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace codexui::codex {
namespace {

namespace codex = ai::openai::codex;
namespace client = ai::openai::codex::frontend::client;
using nodegraph::exactStringFromValue;
using nodegraph::unsignedIntegerFromValue;
using nodegraph::valueMember;

nodegraph::Value::Object decodedObject(const nlohmann::json &value) {
  if (value.is_object())
    return objectFromJson(value);
  return {{"value", valueFromJson(value)}};
}

std::string jsonString(const nlohmann::json &object, std::string_view key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(std::string(key));
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

std::string resultError(const nlohmann::json &raw) {
  if (!raw.is_object())
    return "Codex operation failed";
  const auto error = raw.find("error");
  if (error != raw.end() && error->is_object()) {
    const std::string message = jsonString(*error, "message");
    if (!message.empty())
      return message;
  }
  const std::string message = jsonString(raw, "message");
  return message.empty() ? "Codex operation failed" : message;
}

std::optional<std::int64_t> threadActivityAt(std::string_view method) noexcept {
  if (method == "thread/read" || method == "thread/resume" ||
      method == "thread/turns/list" || method == "thread/items/list")
    return std::nullopt;
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t wallClockMilliseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

nlohmann::json jsonObject(nodegraph::Value::Object object) {
  return jsonFromValue(nodegraph::Value(std::move(object)));
}

std::optional<nodegraph::ProtocolRequestId>
requestIdMember(const nlohmann::json &object, std::string_view key) {
  if (!object.is_object())
    return std::nullopt;
  const auto found = object.find(std::string(key));
  if (found == object.end() || found->is_null())
    return std::nullopt;
  try {
    return requestIdFromJson(*found);
  } catch (...) {
    return std::nullopt;
  }
}

std::string boundedProtocolMetadata(std::string result,
                                    std::size_t maximumBytes = 160) {
  for (char &character : result) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || byte == 0x7fU)
      character = ' ';
  }
  if (result.size() > maximumBytes) {
    result.resize(maximumBytes);
    result += "...";
  }
  return result;
}

std::string boundedProtocolMetadata(const nlohmann::json &value,
                                    std::size_t maximumBytes = 160) {
  if (value.is_string())
    return boundedProtocolMetadata(value.get<std::string>(), maximumBytes);
  if (value.is_number_unsigned())
    return boundedProtocolMetadata(std::to_string(value.get<std::uint64_t>()),
                                   maximumBytes);
  if (value.is_number_integer())
    return boundedProtocolMetadata(std::to_string(value.get<std::int64_t>()),
                                   maximumBytes);
  return {};
}

bool credentialShapedProtocolText(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const unsigned char character : value)
    lowered.push_back(static_cast<char>(std::tolower(character)));
  constexpr std::array markers{
      std::string_view("authorization"), std::string_view("bearer "),
      std::string_view("password"),      std::string_view("secret"),
      std::string_view("token="),        std::string_view("token:"),
      std::string_view("cookie"),        std::string_view("credential"),
      std::string_view("api_key"),       std::string_view("apikey"),
      std::string_view("-----begin"),    std::string_view("github_pat_"),
      std::string_view("ghp_"),          std::string_view("xoxb-"),
      std::string_view("xoxp-"),         std::string_view("xoxa-")};
  if (std::ranges::any_of(markers, [&lowered](std::string_view marker) {
        return lowered.find(marker) != std::string::npos;
      }))
    return true;
  if (lowered.find("sk-") != std::string::npos)
    return true;
  const std::size_t jwt = value.find("eyJ");
  if (jwt != std::string_view::npos) {
    const std::size_t firstDot = value.find('.', jwt);
    if (firstDot != std::string_view::npos &&
        value.find('.', firstDot + 1) != std::string_view::npos)
      return true;
  }
  return false;
}

std::string protocolIdentifierMetadata(const nlohmann::json &value) {
  std::string result = boundedProtocolMetadata(value);
  return credentialShapedProtocolText(result) ? "<redacted-id>"
                                              : std::move(result);
}

std::string safeProtocolErrorText(std::string_view raw) {
  if (credentialShapedProtocolText(raw) || raw.find('/') != std::string::npos ||
      raw.find('\\') != std::string::npos ||
      raw.find('`') != std::string::npos || raw.find('$') != std::string::npos)
    return "[redacted error detail]";
  return boundedProtocolMetadata(std::string(raw), 240);
}

std::string safeProtocolError(const nlohmann::json &message) {
  const auto error = message.find("error");
  if (error == message.end() || !error->is_object())
    return {};
  const auto detail = error->find("message");
  if (detail == error->end() || !detail->is_string())
    return {};
  return safeProtocolErrorText(detail->get_ref<const std::string &>());
}

struct DiagnosticRequestId final {
  std::string key;
  std::string display;
};

std::optional<DiagnosticRequestId>
diagnosticRequestId(const nlohmann::json &message) {
  if (!message.is_object())
    return std::nullopt;
  const auto found = message.find("id");
  if (found == message.end() || found->is_null())
    return std::nullopt;
  if (found->is_string()) {
    const std::string &value = found->get_ref<const std::string &>();
    if (value.size() <= 160 && !credentialShapedProtocolText(value))
      return DiagnosticRequestId{"string:" + value, value};
    // Keep the chronology visible without retaining a secret or allowing a
    // lossy digest collision to associate a response with the wrong request.
    return DiagnosticRequestId{{}, "<oversized-or-sensitive-id>"};
  }
  if (found->is_number_unsigned()) {
    const std::string value = std::to_string(found->get<std::uint64_t>());
    return DiagnosticRequestId{"unsigned:" + value, value};
  }
  if (found->is_number_integer()) {
    const std::string value = std::to_string(found->get<std::int64_t>());
    return DiagnosticRequestId{"signed:" + value, value};
  }
  return std::nullopt;
}

std::string protocolErrorCode(const nlohmann::json &message) {
  const auto error = message.find("error");
  if (error == message.end() || !error->is_object())
    return {};
  const auto code = error->find("code");
  return code == error->end() ? std::string{}
                              : boundedProtocolMetadata(*code, 48);
}

std::string_view nodeActionDiagnosticSubject(nodegraph::NodeActionKind kind) {
  using enum nodegraph::NodeActionKind;
  switch (kind) {
  case Hydrate:
  case Reload:
    return "thread/turns/list";
  case LoadHistory:
    return "thread/turns/list";
  case Rename:
    return "thread/name/set";
  case Fork:
    return "thread/fork";
  case Archive:
    return "thread/archive";
  case Unarchive:
    return "thread/unarchive";
  case Delete:
    return "thread/delete";
  case SubmitPrompt:
    return "turn/start";
  case InterruptTurn:
    return "turn/interrupt";
  case ResolveInteraction:
    return "serverRequest/respond";
  case PromptMaterialized:
    return "local/prompt/materialized";
  case UiDetached:
    return "local/ui/detached";
  }
  return "local/node-action";
}

std::string_view
runtimeActionDiagnosticSubject(nodegraph::RuntimeActionKind kind) {
  using enum nodegraph::RuntimeActionKind;
  switch (kind) {
  case RefreshThreads:
  case LoadMoreThreads:
    return "thread/list";
  case CreateThread:
    return "thread/start";
  case Connect:
    return "connection/connect";
  case Disconnect:
    return "connection/disconnect";
  case Reconnect:
    return "connection/reconnect";
  case ConfigureConnection:
    return "connection/configure";
  case ClaimController:
    return "connection/controller/claim";
  case ReleaseController:
    return "connection/controller/release";
  case RefreshCatalogs:
    return "catalog/refresh";
  }
  return "local/runtime-action";
}

std::string protocolMutationAuthority(std::string_view method,
                                      bool fromAppServer, bool request,
                                      bool notification, bool response,
                                      bool success,
                                      bool responseObservedInterveningFrame) {
  if (!fromAppServer) {
    // An outbound response resolves one retained reverse interaction. Outbound
    // requests and initialized do not themselves publish provider facts.
    return response ? "remove" : "none";
  }
  if (request)
    return "merge";
  if (!success)
    return "none";
  if (notification) {
    constexpr std::array removedNotifications{
        std::string_view("thread/deleted"),
        std::string_view("serverRequest/resolved"),
        std::string_view("thread/goal/cleared")};
    if (std::ranges::find(removedNotifications, method) !=
        removedNotifications.end())
      return "remove";
    const auto descriptor = nodegraph::findProtocolMethod(
        nodegraph::ProtocolDirection::ServerNotification, method);
    if (!descriptor || descriptor->get().disposition !=
                           nodegraph::MessageDisposition::GraphUpdate)
      return "none";
    // These messages invalidate or report transient provider facilities, but
    // do not themselves author current graph facts.
    if (method == "skills/changed" ||
        method == "mcpServer/event/stream/notification")
      return "none";
    if (method == "thread/name/updated" || method == "thread/goal/updated" ||
        method == "thread/queue/changed" ||
        method == "thread/project/updated" ||
        method == "thread/tokenUsage/updated" ||
        method == "turn/diff/updated" || method == "turn/plan/updated" ||
        method == "item/fileChange/patchUpdated" ||
        method == "account/updated" || method == "account/rateLimits/updated" ||
        method == "app/list/updated" ||
        method == "remoteControl/status/changed" ||
        method == "turn/moderationMetadata" ||
        method == "model/safetyBuffering/updated" ||
        method == "thread/realtime/sdp")
      return "replace";
    return "merge";
  }
  if (!response)
    return "none";
  if (method == "thread/read")
    return responseObservedInterveningFrame ? "merge" : "replace";
  constexpr std::array mergedResults{std::string_view("thread/list"),
                                     std::string_view("thread/start"),
                                     std::string_view("thread/resume"),
                                     std::string_view("thread/fork"),
                                     std::string_view("thread/turns/list"),
                                     std::string_view("thread/items/list"),
                                     std::string_view("turn/start")};
  if (std::ranges::find(mergedResults, method) != mergedResults.end())
    return "merge";
  constexpr std::array replacedResults{
      std::string_view("thread/queue/list"),
      std::string_view("thread/backgroundTerminals/list"),
      std::string_view("thread/timeline/list"),
      std::string_view("thread/realtime/listVoices"),
      std::string_view("project/list"),
      std::string_view("project/read"),
      std::string_view("threadSection/list"),
      std::string_view("skills/list"),
      std::string_view("hooks/list"),
      std::string_view("plugin/list"),
      std::string_view("plugin/read"),
      std::string_view("plugin/installed"),
      std::string_view("app/read"),
      std::string_view("app/list"),
      std::string_view("app/installed"),
      std::string_view("model/list"),
      std::string_view("modelProvider/capabilities/read"),
      std::string_view("experimentalFeature/list"),
      std::string_view("permissionProfile/list"),
      std::string_view("collaborationMode/list"),
      std::string_view("mcpServerStatus/list"),
      std::string_view("config/read"),
      std::string_view("configRequirements/read"),
      std::string_view("account/read"),
      std::string_view("account/rateLimits/read"),
      std::string_view("account/usage/read"),
      std::string_view("account/workspaceMessages/read"),
      std::string_view("windowsSandbox/readiness")};
  if (std::ranges::find(replacedResults, method) != replacedResults.end())
    return "replace";
  // The current protocol contains additional read/list/get families which do
  // not need bespoke UI handling. Their successful results still replace the
  // addressed current catalog or domain value, matching the original
  // Inspector contract.
  if (method.ends_with("/list") || method.ends_with("/read") ||
      method.ends_with("/get"))
    return "replace";
  return "none";
}

class ProtocolDiagnosticEmitter final {
public:
  void setGenerations(nodegraph::WorkerGenerations generations) {
    if (generationsKnown_ && generations != generations_) {
      clientRequests_.clear();
      serverRequests_.clear();
      clientRequestOrder_.clear();
      serverRequestOrder_.clear();
    }
    generations_ = generations;
    generationsKnown_ = true;
  }

  void observeLifecycle(std::string_view state, std::string_view detail,
                        nodegraph::ThreadChannels &channels) {
    nodegraph::Value::Object fields{
        {"direction", nodegraph::Value("transport event")},
        {"source", nodegraph::Value("CodexBridge")},
        {"authority", nodegraph::Value("none")},
        {"subject", nodegraph::Value("connection.lifecycle")},
        {"state",
         nodegraph::Value(boundedProtocolMetadata(std::string(state)))}};
    if (state == "failure" || state == "disconnected") {
      fields.emplace("outcome", nodegraph::Value("ERROR"));
      if (!detail.empty())
        fields.emplace("error",
                       nodegraph::Value(safeProtocolErrorText(detail)));
    }
    deliver(std::move(fields), channels);
  }

  void observeBridge(const nlohmann::json &message,
                     nodegraph::ThreadChannels &channels) {
    const std::string kind = jsonString(message, "kind");
    std::string subject = "bridge.unknown";
    std::string authority = "none";
    if (kind == "bridge.connection")
      subject = "connection.bridge";
    else if (kind == "bridge.controller") {
      subject = "connection.controller";
      authority = "replace";
    } else if (kind == "bridge.provider") {
      subject = "connection.provider";
      authority = "replace";
    } else if (kind == "bridge.diagnostic") {
      subject = "bridge.diagnostic";
    }
    nodegraph::Value::Object fields{
        {"direction", nodegraph::Value("bridge event")},
        {"source", nodegraph::Value("CodexBridge")},
        {"authority", nodegraph::Value(std::move(authority))},
        {"subject", nodegraph::Value(std::move(subject))}};
    for (std::string_view key : {"connectionId", "role", "state", "event"}) {
      const auto found = message.find(std::string(key));
      if (found == message.end())
        continue;
      std::string value = protocolIdentifierMetadata(*found);
      if (!value.empty())
        fields.emplace(std::string(key), nodegraph::Value(std::move(value)));
    }
    if (kind == "bridge.diagnostic") {
      fields.emplace("outcome", nodegraph::Value("ERROR"));
      fields.emplace("errorCategory", nodegraph::Value("bridge"));
      const std::string code =
          protocolIdentifierMetadata(message.value("code", nlohmann::json{}));
      if (!code.empty())
        fields.emplace("errorCode", nodegraph::Value(code));
      const std::string raw = jsonString(message, "message");
      if (!raw.empty())
        fields.emplace("error", nodegraph::Value(safeProtocolErrorText(raw)));
    }
    deliver(std::move(fields), channels);
  }

  void observeLocalRejection(std::string_view subject,
                             std::string_view correlation,
                             const nodegraph::NodeRef &target,
                             std::string_view error,
                             nodegraph::ThreadChannels &channels) {
    nodegraph::Value::Object fields{
        {"direction", nodegraph::Value("local result")},
        {"source", nodegraph::Value("CodexUI")},
        {"authority", nodegraph::Value("none")},
        {"subject",
         nodegraph::Value(boundedProtocolMetadata(std::string(subject), 192))},
        {"outcome", nodegraph::Value("ERROR")},
        {"errorCategory", nodegraph::Value("local-validation")},
        {"error", nodegraph::Value(safeProtocolErrorText(error))}};
    std::string displayedCorrelation = boundedProtocolMetadata(std::string(
        correlation.empty() && target ? std::string_view(target->id().canonical)
                                      : correlation));
    if (credentialShapedProtocolText(displayedCorrelation))
      displayedCorrelation = "<redacted-id>";
    if (!displayedCorrelation.empty())
      fields.emplace("correlation",
                     nodegraph::Value(std::move(displayedCorrelation)));
    if (target) {
      std::string targetId = boundedProtocolMetadata(target->id().canonical);
      if (credentialShapedProtocolText(targetId))
        targetId = "<redacted-id>";
      const std::string scope = targetId;
      fields.emplace("targetId", nodegraph::Value(std::move(targetId)));
      std::string_view scopeKey;
      switch (target->id().kind) {
      case nodegraph::NodeKind::Thread:
        scopeKey = "threadId";
        break;
      case nodegraph::NodeKind::Turn:
        scopeKey = "turnId";
        break;
      case nodegraph::NodeKind::Item:
        scopeKey = "itemId";
        break;
      case nodegraph::NodeKind::Interaction:
        scopeKey = "requestId";
        break;
      default:
        break;
      }
      if (!scopeKey.empty())
        fields.emplace(std::string(scopeKey), nodegraph::Value(scope));
    }
    deliver(std::move(fields), channels);
  }

  void observe(codex::protocol::AppServerDirection direction,
               const nlohmann::json &message,
               nodegraph::ThreadChannels &channels) {
    const nodegraph::WorkerGenerations generations = generations_;
    const bool fromAppServer =
        direction == codex::protocol::AppServerDirection::FromAppServer;
    const std::optional<std::string> wireMethod =
        codex::protocol::jsonRpcMethod(message);
    const std::optional<DiagnosticRequestId> requestId =
        diagnosticRequestId(message);
    const bool request = wireMethod && requestId;
    const bool notification = wireMethod && !requestId;
    const bool response = !wireMethod && requestId;
    const bool success = message.find("error") == message.end();

    nodegraph::Value::Object scopeDetails;
    const nlohmann::json *scope = nullptr;
    if (wireMethod) {
      const auto parameters = message.find("params");
      if (parameters != message.end() && parameters->is_object())
        scope = &*parameters;
    } else {
      const auto result = message.find("result");
      if (result != message.end() && result->is_object())
        scope = &*result;
    }
    if (scope)
      addScope(scopeDetails, *scope);

    std::string method =
        boundedProtocolMetadata(wireMethod.value_or(std::string{}), 192);
    const std::string correlationKey =
        requestId ? requestId->key : std::string{};
    const std::string correlation =
        requestId ? requestId->display : std::string{};
    bool responseObservedInterveningFrame = false;
    if (request && !correlationKey.empty()) {
      auto &requests = fromAppServer ? serverRequests_ : clientRequests_;
      auto &order = fromAppServer ? serverRequestOrder_ : clientRequestOrder_;
      remember(
          requests, order, correlationKey,
          PendingCorrelation{method, scopeDetails, generations, sequence_ + 1});
    } else if (response && !correlationKey.empty()) {
      auto &requests = fromAppServer ? clientRequests_ : serverRequests_;
      const auto correlated = requests.find(correlationKey);
      if (correlated != requests.end()) {
        method = correlated->second.method;
        responseObservedInterveningFrame =
            sequence_ != correlated->second.observedSequence;
        if (correlated->second.generations == generations) {
          for (const auto &[key, value] : correlated->second.scope)
            scopeDetails.try_emplace(key, value);
        }
        requests.erase(correlated);
      }
    }
    if (method.empty())
      method = "<uncorrelated response>";

    std::string directionName;
    if (request)
      directionName = fromAppServer ? "server request" : "client request";
    else if (notification)
      directionName =
          fromAppServer ? "server notification" : "client notification";
    else if (response)
      directionName = fromAppServer
                          ? (success ? "client result" : "client error")
                          : (success ? "server result" : "server error");
    else
      directionName = fromAppServer ? "server frame" : "client frame";

    nodegraph::Value::Object details{
        {"direction", nodegraph::Value(std::move(directionName))},
        {"source", nodegraph::Value(fromAppServer ? "app-server" : "CodexUI")},
        {"authority",
         nodegraph::Value(protocolMutationAuthority(
             method, fromAppServer, request, notification, response, success,
             responseObservedInterveningFrame))},
        {"subject", nodegraph::Value(method)}};
    if (!correlation.empty())
      details.emplace("correlation", nodegraph::Value(correlation));
    if (response)
      details.emplace("outcome", nodegraph::Value(success ? "ok" : "ERROR"));
    if (!success) {
      details.emplace("errorCategory", nodegraph::Value("json-rpc"));
      std::string code = protocolErrorCode(message);
      if (!code.empty())
        details.emplace("errorCode", nodegraph::Value(std::move(code)));
      std::string error = safeProtocolError(message);
      if (!error.empty())
        details.emplace("error", nodegraph::Value(std::move(error)));
    }
    for (auto &[key, value] : scopeDetails)
      details.try_emplace(std::move(key), std::move(value));
    deliver(std::move(details), channels);
  }

private:
  struct PendingCorrelation final {
    std::string method;
    nodegraph::Value::Object scope;
    nodegraph::WorkerGenerations generations;
    std::uint64_t observedSequence = 0;
    std::uint64_t order = 0;
  };

  using CorrelationMap = std::map<std::string, PendingCorrelation, std::less<>>;
  using CorrelationOrder = std::deque<std::pair<std::string, std::uint64_t>>;

  void deliver(nodegraph::Value::Object details,
               nodegraph::ThreadChannels &channels) {
    details.emplace("sequence", nodegraph::Value(++sequence_));
    details.emplace("connectionGeneration",
                    nodegraph::Value(generations_.connection));
    details.emplace("providerGeneration",
                    nodegraph::Value(generations_.provider));
    if (dropped_ != 0 && pending_.empty())
      details.emplace("droppedBefore", nodegraph::Value(dropped_));
    if (pending_.size() >= MaximumDiagnosticBatch) {
      ++dropped_;
      return;
    }
    pending_.push_back(std::move(details));
    if (flushScheduled_)
      return;
    flushScheduled_ = true;
    core::EventReceiver::atNextTick(
        [this, &channels] { flush(channels); });
  }

  void flush(nodegraph::ThreadChannels &channels) {
    flushScheduled_ = false;
    if (pending_.empty())
      return;
    nodegraph::ProtocolDiagnostic diagnostic;
    diagnostic.diagnosticBatch = std::move(pending_);
    pending_.clear();
    const std::size_t delivered = diagnostic.diagnosticBatch.size();
    const nodegraph::ChannelSendStatus status =
        channels.sendProtocolDiagnostic(diagnostic);
    if (status == nodegraph::ChannelSendStatus::QueueFull) {
      dropped_ += delivered;
      return;
    }
    dropped_ = 0;
  }

  void remember(CorrelationMap &requests, CorrelationOrder &order,
                const std::string &key, PendingCorrelation correlation) {
    if (order.size() >= MaximumPendingCorrelations * 2U) {
      CorrelationOrder compacted;
      for (const auto &[orderedKey, serial] : order) {
        const auto current = requests.find(orderedKey);
        if (current != requests.end() && current->second.order == serial)
          compacted.emplace_back(orderedKey, serial);
      }
      order = std::move(compacted);
    }
    while (requests.size() >= MaximumPendingCorrelations &&
           !requests.contains(key) && !order.empty()) {
      const auto [oldestKey, serial] = std::move(order.front());
      order.pop_front();
      const auto oldest = requests.find(oldestKey);
      if (oldest != requests.end() && oldest->second.order == serial)
        requests.erase(oldest);
    }
    correlation.order = ++correlationOrder_;
    order.emplace_back(key, correlation.order);
    requests.insert_or_assign(key, std::move(correlation));
  }

  void addScope(nodegraph::Value::Object &details,
                const nlohmann::json &scope) const {
    constexpr std::array keys{
        std::string_view("threadId"), std::string_view("turnId"),
        std::string_view("itemId"), std::string_view("requestId"),
        std::string_view("processId")};
    for (std::string_view key : keys) {
      const auto found = scope.find(std::string(key));
      if (found == scope.end())
        continue;
      std::string value = protocolIdentifierMetadata(*found);
      if (!value.empty())
        details.emplace(std::string(key), nodegraph::Value(std::move(value)));
    }
    constexpr std::array nested{
        std::pair{std::string_view("thread"), std::string_view("threadId")},
        std::pair{std::string_view("turn"), std::string_view("turnId")},
        std::pair{std::string_view("item"), std::string_view("itemId")}};
    for (const auto &[objectName, idName] : nested) {
      if (details.contains(idName))
        continue;
      const auto object = scope.find(std::string(objectName));
      if (object == scope.end() || !object->is_object())
        continue;
      const auto id = object->find("id");
      if (id == object->end())
        continue;
      std::string value = protocolIdentifierMetadata(*id);
      if (!value.empty())
        details.emplace(std::string(idName),
                        nodegraph::Value(std::move(value)));
    }
  }

  static constexpr std::size_t MaximumPendingCorrelations = 4096;
  static constexpr std::size_t MaximumDiagnosticBatch = 64;
  CorrelationMap clientRequests_;
  CorrelationMap serverRequests_;
  CorrelationOrder clientRequestOrder_;
  CorrelationOrder serverRequestOrder_;
  nodegraph::WorkerGenerations generations_;
  std::uint64_t correlationOrder_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t dropped_ = 0;
  std::vector<nodegraph::Value::Object> pending_;
  bool flushScheduled_ = false;
  bool generationsKnown_ = false;
};

nlohmann::json
promptInput(const std::string &prompt,
            const std::vector<nodegraph::Attachment> &attachments) {
  nlohmann::json input = nlohmann::json::array(
      {{{"type", "text"},
        {"text", nodegraph::composePromptMarkdown(prompt, attachments)},
        {"text_elements", nlohmann::json::array()}}});
  for (const nodegraph::Attachment &attachment : attachments) {
    if (attachment.mimeType.starts_with("image/"))
      input.push_back({{"type", "localImage"}, {"path", attachment.path}});
    else if (attachment.mimeType.starts_with("audio/"))
      input.push_back({{"type", "localAudio"}, {"path", attachment.path}});
  }
  return input;
}

std::optional<nodegraph::ProtocolRequestId>
decodedRequestId(const nlohmann::json &value) {
  if (value.is_null())
    return std::nullopt;
  return requestIdFromJson(value);
}

void applyBridgeState(nodegraph::WorkerLogic &logic,
                      const codex::frontend::CodexBridge &sdk,
                      const nlohmann::json &message) {
  const std::string kind = jsonString(message, "kind");
  if (kind != "bridge.connection" && kind != "bridge.controller" &&
      kind != "bridge.provider")
    return;

  const std::string connectionId = sdk.connectionId().value_or("");
  const std::string controllerId = sdk.controllerConnectionId().value_or("");
  const std::string role =
      sdk.role() ? std::string(codex::protocol::toString(*sdk.role())) : "";
  std::optional<std::string> providerState;
  std::string detail;
  if (kind == "bridge.provider") {
    providerState = jsonString(message, "state");
    detail = jsonString(message, "reason");
  }
  static_cast<void>(logic.bridgeState(
      connectionId, role, controllerId, sdk.providerGeneration(),
      std::move(providerState), std::move(detail)));
}

template <typename Client>
void configureStreamClient(Client &configuredClient, bool disabled) {
  configuredClient.getConfig()->Instance::setDisabled(disabled);
  configuredClient.getConfig()->Connection::setReadTimeout(
      utils::Timeval({0, 0}));
  configuredClient.getConfig()->Connection::setWriteTimeout(
      utils::Timeval({0, 0}));
  configuredClient.getConfig()->Connection::setMaximumWriteQueueBytes(
      DefaultMaximumWriteQueueBytes);
}

struct RequestOutcome final {
  bool ok = false;
  std::optional<nodegraph::ProtocolRequestId> requestId;
  nodegraph::Value::Object payload;
  std::string error;
  std::string threadId;
  std::string turnId;
  std::string nextCursor;
  bool stale = false;
};

std::vector<std::string>
turnIdsFromPage(const nodegraph::Value::Object &payload) {
  std::vector<std::string> result;
  const nodegraph::Value *data = valueMember(payload, "data");
  const nodegraph::Value::Array *turns = data ? data->asArray() : nullptr;
  if (!turns)
    return result;
  result.reserve(turns->size());
  for (const nodegraph::Value &turn : *turns) {
    const nodegraph::Value::Object *object = turn.asObject();
    const std::string id =
        object ? exactStringFromValue(valueMember(*object, "id"))
               : std::string{};
    if (!id.empty())
      result.push_back(id);
  }
  return result;
}

void identifyResultEntities(RequestOutcome &outcome) {
  outcome.threadId =
      exactStringFromValue(valueMember(outcome.payload, "threadId"));
  outcome.turnId = exactStringFromValue(valueMember(outcome.payload, "turnId"));
  outcome.nextCursor =
      exactStringFromValue(valueMember(outcome.payload, "nextCursor"));
  if (const nodegraph::Value *thread = valueMember(outcome.payload, "thread")) {
    if (const nodegraph::Value::Object *object = thread->asObject())
      if (outcome.threadId.empty())
        outcome.threadId = exactStringFromValue(valueMember(*object, "id"));
  }
  if (const nodegraph::Value *turn = valueMember(outcome.payload, "turn")) {
    if (const nodegraph::Value::Object *object = turn->asObject())
      if (outcome.turnId.empty())
        outcome.turnId = exactStringFromValue(valueMember(*object, "id"));
  }
}

template <typename Operation, typename Published, typename Completed>
std::string dispatchRequestHandled(codex::frontend::CodexBridge &sdk,
                                   nlohmann::json parameters,
                                   nodegraph::WorkerLogic &workerLogic,
                                   nodegraph::NodeRef requestTarget,
                                   Published published, Completed completed) {
  struct RequestPublication final {
    bool requestPublished = false;
    nodegraph::NodeRef operation;
    std::optional<RequestOutcome> synchronousResult;
  };

  auto publication = std::make_shared<RequestPublication>();
  const auto expectedGenerations = workerLogic.generations();
  auto process = [&workerLogic, expectedGenerations, publication,
                  completed =
                      std::move(completed)](RequestOutcome outcome) mutable {
    if (workerLogic.generations() != expectedGenerations)
      return;
    nodegraph::DecodedMessage decoded{
        outcome.ok ? nodegraph::DecodedMessageKind::ClientResult
                   : nodegraph::DecodedMessageKind::ClientError,
        std::string(Operation::method),
        outcome.requestId,
        std::move(outcome.payload),
        publication->operation,
        threadActivityAt(Operation::method),
        {},
        {},
        {}};
    completed(std::move(outcome), std::move(decoded));
  };
  const std::string requestId = sdk.request<Operation>(
      typename Operation::Params{parameters},
      [publication, process](typename Operation::Response &response) mutable {
        const bool ok = response.ok();
        const nlohmann::json &raw = response.getRaw();
        const nlohmann::json payload =
            ok ? response.getPayload()
               : (raw.is_object() && raw.contains("error") ? raw["error"]
                                                           : raw);
        RequestOutcome outcome{ok, decodedRequestId(response.jsonRpcId()),
                               decodedObject(payload),
                               ok ? std::string{} : resultError(raw),
                               {}, {}, {}, false};
        identifyResultEntities(outcome);
        if (publication->requestPublished)
          process(std::move(outcome));
        else
          publication->synchronousResult.emplace(std::move(outcome));
      });
  const nodegraph::ProtocolRequestId typedRequestId(requestId);
  nodegraph::DecodedMessage decodedRequest{
      nodegraph::DecodedMessageKind::ClientRequest,
      std::string(Operation::method),
      typedRequestId,
      decodedObject(parameters),
      {},
      threadActivityAt(Operation::method),
      {},
      {},
      {}};
  decodedRequest.requestTarget = std::move(requestTarget);
  nodegraph::WorkerApplyResult applied =
      workerLogic.applyDetailed(std::move(decodedRequest));
  publication->operation = std::move(applied.primary);
  published(typedRequestId);
  publication->requestPublished = true;
  if (publication->synchronousResult)
    process(std::move(*publication->synchronousResult));
  return requestId;
}

template <typename Operation, typename Published, typename Completed>
std::string dispatchRequest(codex::frontend::CodexBridge &sdk,
                            nlohmann::json parameters,
                            nodegraph::WorkerLogic &workerLogic,
                            nodegraph::NodeRef requestTarget,
                            Published published, Completed completed) {
  return dispatchRequestHandled<Operation>(
      sdk, std::move(parameters), workerLogic, std::move(requestTarget),
      std::move(published),
      [&workerLogic, completed = std::move(completed)](
          RequestOutcome outcome, nodegraph::DecodedMessage decoded) mutable {
        const nodegraph::NodeRef expected = decoded.expectedNode;
        const nodegraph::WorkerApplyResult applied =
            workerLogic.applyDetailed(std::move(decoded));
        outcome.stale = expected && applied.primary != expected;
        completed(std::move(outcome));
      });
}

template <typename Operation, typename Completed>
std::string
dispatchRequest(codex::frontend::CodexBridge &sdk, nlohmann::json parameters,
                nodegraph::WorkerLogic &workerLogic,
                nodegraph::NodeRef requestTarget, Completed completed) {
  return dispatchRequest<Operation>(
      sdk, std::move(parameters), workerLogic, std::move(requestTarget),
      [](const nodegraph::ProtocolRequestId &) {}, std::move(completed));
}

template <typename Operation>
std::string dispatchRequest(codex::frontend::CodexBridge &sdk,
                            nlohmann::json parameters,
                            nodegraph::WorkerLogic &workerLogic,
                            nodegraph::NodeRef requestTarget = {}) {
  return dispatchRequest<Operation>(sdk, std::move(parameters), workerLogic,
                                    std::move(requestTarget),
                                    [](RequestOutcome) {});
}

template <typename Operation>
bool respondToPendingRequest(codex::frontend::CodexBridge &sdk,
                             const typename Operation::Params &request,
                             const PendingRequestResponse &response) {
  if (const auto *error = std::get_if<std::string>(&response))
    return sdk.respondError<Operation>(request, -32601, *error, nullptr);
  const typename Operation::Response typedResponse(
      std::get<nlohmann::json>(response));
  return sdk.respond<Operation>(request, typedResponse);
}

} // namespace

int runClientRuntime(Configuration &configuration, nodegraph::NodeGraph &graph,
                     nodegraph::ThreadChannels &channels, bool connectBridge) {
  using StreamFactory = client::StreamSocketContextFactory;

  const std::size_t maximumFrameBytes = configuration.maximumFrameBytes();
  nodegraph::WorkerLogic workerLogic(graph, channels);

  using ServerRequestParams = std::variant<
      codex::generated::server_requests::CommandExecutionRequestApproval::
          Params,
      codex::generated::server_requests::FileChangeRequestApproval::Params,
      codex::generated::server_requests::ToolRequestUserInput::Params,
      codex::generated::server_requests::McpServerElicitationRequest::Params,
      codex::generated::server_requests::PermissionsRequestApproval::Params,
      codex::generated::server_requests::DynamicToolCall::Params,
      codex::generated::server_requests::ChatgptAuthTokensRefresh::Params,
      codex::generated::server_requests::AttestationGenerate::Params,
      codex::generated::server_requests::ApplyPatchApproval::Params,
      codex::generated::server_requests::ExecCommandApproval::Params>;
  struct PendingServerRequest final {
    nodegraph::ProtocolRequestId requestId;
    nodegraph::WorkerGenerations generations;
    std::string method;
    ServerRequestParams request;
  };

  std::unordered_map<nodegraph::NodeRef, PendingServerRequest>
      pendingServerRequests;
  std::unordered_set<nodegraph::NodeRef> pendingHydrations;
  std::unordered_set<nodegraph::NodeRef> interactiveHydrations;
  std::unordered_set<nodegraph::NodeRef> pendingHistoricalHydrations;
  std::unordered_set<nodegraph::NodeId, nodegraph::NodeIdHash>
      historicalHydrationVisited;
  std::deque<nodegraph::NodeRef> historicalHydrationQueue;
  bool historicalHydrationPumpActive = false;
  struct ItemPage final {
    nodegraph::NodeRef thread;
    std::string threadId;
    std::string turnId;
    std::string cursor;
  };
  std::deque<ItemPage> itemPageQueue;
  std::size_t pendingItemPages = 0;
  std::unordered_set<nodegraph::NodeRef> resumedPromptAdmissions;
  std::unordered_map<nodegraph::NodeRef, nodegraph::PromptCommand>
      promptsWaitingForHydration;
  bool threadListPending = false;
  bool threadListRepairPending = false;
  bool threadListLoadMoreRequested = false;
  std::uint64_t threadListCycle = 0;
  std::string threadListNextCursor;
  std::unordered_set<std::string> threadListSeenCursors;
  nlohmann::json threadListBaseParameters = nlohmann::json::object();
  bool modelListPending = false;
  bool permissionProfilesPending = false;

  const auto clearTransientState = [&] {
    pendingServerRequests.clear();
    pendingHydrations.clear();
    interactiveHydrations.clear();
    pendingHistoricalHydrations.clear();
    historicalHydrationVisited.clear();
    historicalHydrationQueue.clear();
    historicalHydrationPumpActive = false;
    itemPageQueue.clear();
    pendingItemPages = 0;
    resumedPromptAdmissions.clear();
    promptsWaitingForHydration.clear();
    threadListPending = false;
    threadListRepairPending = false;
    threadListLoadMoreRequested = false;
    ++threadListCycle;
    threadListNextCursor.clear();
    threadListSeenCursors.clear();
    threadListBaseParameters = nlohmann::json::object();
    modelListPending = false;
    permissionProfilesPending = false;
  };

  const auto showNotice = [&workerLogic](std::string message) {
    static_cast<void>(workerLogic.showNotice(std::move(message)));
  };

  codex::frontend::CodexBridge sdk({});
  ProtocolDiagnosticEmitter protocolDiagnostics;
  const auto rejectNodeAction =
      [&channels, &protocolDiagnostics, &showNotice,
       &workerLogic](const nodegraph::NodeAction &action, std::string message) {
        protocolDiagnostics.setGenerations(workerLogic.generations());
        protocolDiagnostics.observeLocalRejection(
            nodeActionDiagnosticSubject(action.kind), action.correlation,
            action.target, message, channels);
        showNotice(std::move(message));
      };
  const auto rejectRuntimeAction =
      [&channels, &protocolDiagnostics, &showNotice, &workerLogic](
          const nodegraph::RuntimeAction &action, std::string message) {
        protocolDiagnostics.setGenerations(workerLogic.generations());
        protocolDiagnostics.observeLocalRejection(
            runtimeActionDiagnosticSubject(action.kind), action.correlation, {},
            message, channels);
        showNotice(std::move(message));
      };

  std::function<void()> requestReconnect;
  std::function<void()> requestShutdown;
  std::function<void()> hydrateProvider;
  std::function<void(std::string)> hydrateHistoricalChildren;

  std::string expectedDisconnectReason;
  bool desiredConnected = connectBridge;
  client::ClientConnection connection(
      sdk,
      client::ClientConnectionCallbacks{
          .onConnected =
              [&channels, &clearTransientState, &protocolDiagnostics,
               &workerLogic] {
                clearTransientState();
                static_cast<void>(workerLogic.transportEvent("connected"));
                protocolDiagnostics.setGenerations(workerLogic.generations());
                protocolDiagnostics.observeLifecycle("connected", {}, channels);
              },
          .onDisconnected =
              [&channels, &clearTransientState, &expectedDisconnectReason,
               &desiredConnected, &protocolDiagnostics, &workerLogic] {
                clearTransientState();
                std::string reason =
                    std::exchange(expectedDisconnectReason, {});
                const std::string state =
                    desiredConnected ? "retrying" : "disconnected";
                static_cast<void>(workerLogic.transportEvent(state, reason));
                protocolDiagnostics.setGenerations(workerLogic.generations());
                protocolDiagnostics.observeLifecycle(state, reason, channels);
              },
          .onFailure =
              [&channels, &clearTransientState, &protocolDiagnostics,
               &workerLogic](std::string reason) {
                clearTransientState();
                const std::string diagnosticReason = reason;
                static_cast<void>(
                    workerLogic.transportEvent("failure", std::move(reason)));
                protocolDiagnostics.setGenerations(workerLogic.generations());
                protocolDiagnostics.observeLifecycle(
                    "failure", diagnosticReason, channels);
              }});

  sdk.onRawJson([&channels, &protocolDiagnostics,
                 &workerLogic](codex::protocol::AppServerDirection direction,
                               const nlohmann::json &message) {
    // Observe the already-decoded envelope once and immediately reduce it to
    // bounded metadata. No raw payload crosses to Qt or survives this call.
    protocolDiagnostics.observe(direction, message, channels);
    const std::optional<std::string> method =
        codex::protocol::jsonRpcMethod(message);
    if (!method)
      return;

    if (direction == codex::protocol::AppServerDirection::ToAppServer &&
        *method == "initialized") {
      const auto parameters = message.find("params");
      const nlohmann::json payload =
          parameters == message.end() ? nlohmann::json::object() : *parameters;
      static_cast<void>(workerLogic.applyDetailed(nodegraph::DecodedMessage{
          nodegraph::DecodedMessageKind::ClientNotification,
          *method,
          std::nullopt,
          decodedObject(payload),
          {},
          {},
          {},
          {},
          {}}));
      return;
    }

    if (direction != codex::protocol::AppServerDirection::FromAppServer)
      return;

    const std::optional<nodegraph::ProtocolRequestId> requestId =
        requestIdMember(message, "id");
    if (requestId)
      return; // CodexBridge rejects unregistered requests on this receive pass.
    if (nodegraph::findProtocolMethod(
            nodegraph::ProtocolDirection::ServerNotification, *method))
      return; // The registered typed callback is the sole known-message path.

    const auto parameters = message.find("params");
    const nlohmann::json payload =
        parameters == message.end() ? nlohmann::json::object() : *parameters;
    static_cast<void>(workerLogic.applyDetailed(nodegraph::DecodedMessage{
        nodegraph::DecodedMessageKind::ServerNotification,
        *method,
        std::nullopt,
        decodedObject(payload),
        {},
        {},
        {},
        {},
        {}}));
  });
  sdk.onBridgeEvent([&channels, &clearTransientState, &hydrateProvider,
                     &protocolDiagnostics, &workerLogic,
                     &sdk](const nlohmann::json &message) {
    const std::uint64_t before = workerLogic.generations().provider;
    const std::string kind = jsonString(message, "kind");
    applyBridgeState(workerLogic, sdk, message);
    const auto after = workerLogic.generations();
    protocolDiagnostics.setGenerations(after);
    protocolDiagnostics.observeBridge(message, channels);
    if (after.provider != before)
      clearTransientState();
    if (kind == "bridge.provider" && sdk.providerReady() && hydrateProvider)
      hydrateProvider();
  });

  const auto registerServerRequest = [&]<typename Operation>() {
    sdk.onServerRequest<Operation>([&pendingServerRequests, &workerLogic](
                                       typename Operation::Params &request) {
      const auto requestId = decodedRequestId(request.jsonRpcId());
      if (!requestId)
        return;
      for (auto entry = pendingServerRequests.begin();
           entry != pendingServerRequests.end();) {
        if (entry->second.requestId == *requestId)
          entry = pendingServerRequests.erase(entry);
        else
          ++entry;
      }
      nodegraph::WorkerApplyResult applied =
          workerLogic.applyDetailed(nodegraph::DecodedMessage{
              nodegraph::DecodedMessageKind::ServerRequest,
              std::string(Operation::method),
              requestId,
              decodedObject(request.getPayload()),
              {},
              threadActivityAt(Operation::method),
              {},
              {},
              {}});
      if (!applied.primary)
        return;
      pendingServerRequests.emplace(
          applied.primary,
          PendingServerRequest{*requestId, workerLogic.generations(),
                               std::string(Operation::method),
                               ServerRequestParams(request)});
    });
  };

  using namespace codex::generated::server_requests;
  registerServerRequest.template operator()<CommandExecutionRequestApproval>();
  registerServerRequest.template operator()<FileChangeRequestApproval>();
  registerServerRequest.template operator()<ToolRequestUserInput>();
  registerServerRequest.template operator()<McpServerElicitationRequest>();
  registerServerRequest.template operator()<PermissionsRequestApproval>();
  registerServerRequest.template operator()<DynamicToolCall>();
  registerServerRequest.template operator()<ChatgptAuthTokensRefresh>();
  registerServerRequest.template operator()<AttestationGenerate>();
  registerServerRequest.template operator()<ApplyPatchApproval>();
  registerServerRequest.template operator()<ExecCommandApproval>();

#define CODEXUI_REGISTER_SERVER_NOTIFICATION(OperationName, methodName)        \
  sdk.on##OperationName([&hydrateHistoricalChildren, &pendingServerRequests,   \
                         &workerLogic](                                        \
                            codex::generated::server_notifications::           \
                                OperationName::Params &notification) {         \
    nodegraph::NodeRef expected;                                               \
    const bool resolved =                                                      \
        std::string_view(                                                      \
            codex::generated::server_notifications::OperationName::method) ==  \
        "serverRequest/resolved";                                              \
    if (resolved) {                                                            \
      const auto id = requestIdMember(notification.getPayload(), "requestId"); \
      if (id) {                                                                \
        for (const auto &[node, pending] : pendingServerRequests) {            \
          if (pending.requestId == *id &&                                      \
              pending.generations == workerLogic.generations()) {              \
            expected = node;                                                   \
            break;                                                             \
          }                                                                    \
        }                                                                      \
      }                                                                        \
    }                                                                          \
    if (resolved && !expected)                                                 \
      return;                                                                  \
    static_cast<void>(workerLogic.applyDetailed(nodegraph::DecodedMessage{     \
        nodegraph::DecodedMessageKind::ServerNotification,                     \
        std::string(                                                           \
            codex::generated::server_notifications::OperationName::method),    \
        std::nullopt, decodedObject(notification.getPayload()), expected,      \
        threadActivityAt(                                                      \
            codex::generated::server_notifications::OperationName::method),    \
        {}, {}, {}}));                                                         \
    constexpr std::string_view appliedMethod =                                 \
        codex::generated::server_notifications::OperationName::method;         \
    if (hydrateHistoricalChildren && (appliedMethod == "item/started" ||       \
                                      appliedMethod == "item/completed")) {    \
      const std::string threadId =                                             \
          jsonString(notification.getPayload(), "threadId");                   \
      if (!threadId.empty())                                                   \
        hydrateHistoricalChildren(threadId);                                   \
    }                                                                          \
    if (expected)                                                              \
      pendingServerRequests.erase(expected);                                   \
  });
  AI_OPENAI_CODEX_SERVER_NOTIFICATIONS(CODEXUI_REGISTER_SERVER_NOTIFICATION)
#undef CODEXUI_REGISTER_SERVER_NOTIFICATION

  using CurrentTimeRead = codex::generated::server_requests::CurrentTimeRead;
  sdk.onServerRequest<CurrentTimeRead>([&pendingServerRequests, &showNotice,
                                        &workerLogic, &sdk](
                                           CurrentTimeRead::Params &request) {
    const auto requestId = requestIdFromJson(request.jsonRpcId());
    for (auto entry = pendingServerRequests.begin();
         entry != pendingServerRequests.end();) {
      if (entry->second.requestId == requestId)
        entry = pendingServerRequests.erase(entry);
      else
        ++entry;
    }
    nodegraph::WorkerApplyResult applied = workerLogic.applyDetailed(
        nodegraph::DecodedMessage{nodegraph::DecodedMessageKind::ServerRequest,
                                  std::string(CurrentTimeRead::method),
                                  requestId,
                                  decodedObject(request.getPayload()),
                                  {},
                                  threadActivityAt(CurrentTimeRead::method),
                                  {},
                                  {},
                                  {}});
    const std::int64_t currentTime =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const CurrentTimeRead::Response response(
        nlohmann::json{{"currentTimeAt", currentTime}});
    const bool accepted = sdk.respond<CurrentTimeRead>(request, response);
    // This interaction is handled entirely by the worker and has no
    // retained typed request for a later UI response.  Whether the
    // transport accepted the automatic reply or not, it is terminal and
    // must never remain as an apparently actionable interaction node.
    if (applied.primary)
      static_cast<void>(workerLogic.resolveInteraction(applied.primary));
    if (!accepted)
      showNotice("Current-time server response was rejected");
  });

#define CODEXUI_REGISTER_INTERNAL_NOTIFICATION(OperationName)                  \
  sdk.onServerNotification<                                                    \
      internal_protocol::server_notifications::OperationName>(                 \
      [&workerLogic](                                                          \
          internal_protocol::server_notifications::OperationName::Params       \
              &notification) {                                                 \
        static_cast<void>(workerLogic.applyDetailed(nodegraph::DecodedMessage{ \
            nodegraph::DecodedMessageKind::ServerNotification,                 \
            std::string(internal_protocol::server_notifications::              \
                            OperationName::method),                            \
            std::nullopt,                                                      \
            decodedObject(notification.getPayload()),                          \
            {},                                                                \
            threadActivityAt(internal_protocol::server_notifications::         \
                                 OperationName::method),                       \
            {},                                                                \
            {},                                                                \
            {}}));                                                             \
      });
  CODEXUI_REGISTER_INTERNAL_NOTIFICATION(RawResponseItemCompleted)
  CODEXUI_REGISTER_INTERNAL_NOTIFICATION(RawResponseCompleted)
#undef CODEXUI_REGISTER_INTERNAL_NOTIFICATION

  const auto publishTransportEvent = [&channels, &clearTransientState,
                                      &protocolDiagnostics,
                                      &workerLogic](std::string state,
                                                    std::string detail = {}) {
    if (state == "retrying" || state == "disconnected" || state == "failure")
      clearTransientState();
    const std::string diagnosticState = state;
    const std::string diagnosticDetail = detail;
    static_cast<void>(
        workerLogic.transportEvent(std::move(state), std::move(detail)));
    protocolDiagnostics.setGenerations(workerLogic.generations());
    protocolDiagnostics.observeLifecycle(diagnosticState, diagnosticDetail,
                                         channels);
  };

  net::un::stream::legacy::SocketClient<StreamFactory,
                                        client::ClientConnection &, std::size_t>
      unixClient("codex-ui-unix", connection, std::size_t(maximumFrameBytes));
  unixClient.getConfig()->Remote::setSunPath(
      codex::protocol::defaultFrontendSocketPath());
  configureStreamClient(unixClient, false);

  net::in::stream::legacy::SocketClient<StreamFactory,
                                        client::ClientConnection &, std::size_t>
      ipv4Client("codex-ui-ipv4", connection, std::size_t(maximumFrameBytes));
  configureStreamClient(ipv4Client, true);
  ipv4Client.getConfig()->Remote::setHost("127.0.0.1");

  net::in6::stream::legacy::SocketClient<
      StreamFactory, client::ClientConnection &, std::size_t>
      ipv6Client("codex-ui-ipv6", connection, std::size_t(maximumFrameBytes));
  configureStreamClient(ipv6Client, true);
  ipv6Client.getConfig()->Remote::setHost("::1");

#if defined(CODEXUI_CODEX_FRONTEND_TLS)
  net::in::stream::tls::SocketClient<StreamFactory, client::ClientConnection &,
                                     std::size_t>
      tlsIpv4Client("codex-ui-tls-ipv4", connection,
                    std::size_t(maximumFrameBytes));
  configureStreamClient(tlsIpv4Client, true);
  tlsIpv4Client.getConfig()->Remote::setHost("127.0.0.1");

  net::in6::stream::tls::SocketClient<StreamFactory, client::ClientConnection &,
                                      std::size_t>
      tlsIpv6Client("codex-ui-tls-ipv6", connection,
                    std::size_t(maximumFrameBytes));
  configureStreamClient(tlsIpv6Client, true);
  tlsIpv6Client.getConfig()->Remote::setHost("::1");
#endif

#if defined(CODEXUI_CODEX_FRONTEND_RFCOMM)
  net::rc::stream::legacy::SocketClient<StreamFactory,
                                        client::ClientConnection &, std::size_t>
      rfcommClient("codex-ui-rfcomm", connection,
                   std::size_t(maximumFrameBytes));
  configureStreamClient(rfcommClient, true);

  net::rc::stream::tls::SocketClient<StreamFactory, client::ClientConnection &,
                                     std::size_t>
      rfcommTlsClient("codex-ui-rfcomm-tls", connection,
                      std::size_t(maximumFrameBytes));
  configureStreamClient(rfcommTlsClient, true);
#endif

#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
  client::linkWebSocketClient();
  std::string currentWebSocketEndpoint = configuration.webSocketEndpoint();
  auto webSocketBinding =
      std::make_shared<client::WebSocketBinding>(connection, maximumFrameBytes);
  const auto beginWebSocket =
      [webSocketBinding, &currentWebSocketEndpoint](
          const std::shared_ptr<web::http::client::MasterRequest> &request) {
        webSocketBinding->beginUpgrade(request, currentWebSocketEndpoint);
      };
  const auto endWebSocket =
      [webSocketBinding](
          const std::shared_ptr<web::http::client::MasterRequest> &request) {
        webSocketBinding->httpDisconnected(request);
      };

  client::WebSocketHttpClient<net::in::stream::legacy::SocketClient>
      webSocketIpv4Client("codex-ui-websocket-ipv4", beginWebSocket,
                          endWebSocket, webSocketBinding);
  configureStreamClient(webSocketIpv4Client, true);
  webSocketIpv4Client.getConfig()->Remote::setHost("127.0.0.1");

  client::WebSocketHttpClient<net::in6::stream::legacy::SocketClient>
      webSocketIpv6Client("codex-ui-websocket-ipv6", beginWebSocket,
                          endWebSocket, webSocketBinding);
  configureStreamClient(webSocketIpv6Client, true);
  webSocketIpv6Client.getConfig()->Remote::setHost("::1");

#if defined(CODEXUI_CODEX_FRONTEND_TLS)
  client::WebSocketHttpClient<net::in::stream::tls::SocketClient> wssIpv4Client(
      "codex-ui-wss-ipv4", beginWebSocket, endWebSocket, webSocketBinding);
  configureStreamClient(wssIpv4Client, true);
  wssIpv4Client.getConfig()->Remote::setHost("127.0.0.1");

  client::WebSocketHttpClient<net::in6::stream::tls::SocketClient>
      wssIpv6Client("codex-ui-wss-ipv6", beginWebSocket, endWebSocket,
                    webSocketBinding);
  configureStreamClient(wssIpv6Client, true);
  wssIpv6Client.getConfig()->Remote::setHost("::1");
#endif
#endif

  std::function<void()> connectSelected;
  std::shared_ptr<core::socket::stream::ClientFlowController> selectedFlow;
  std::function<void()> disableSelected;
  std::string selectedTransport;
  std::string selectedTransportLabel;
  bool transitionPending = false;
  bool shutdownRequested = false;
  bool eventLoopRunning = false;
  std::function<void()> continueTransition;
  std::function<void()> pendingSelection;
  std::chrono::steady_clock::time_point transitionDeadline;

  const auto selectClient = [&](auto &configuredClient, std::string transport,
                                std::string label) {
    if (disableSelected)
      disableSelected();
    configuredClient.getConfig()->Instance::setDisabled(false);
    auto *const clientHandle = &configuredClient;
    auto *const config = configuredClient.getConfig();
    selectedTransport = std::move(transport);
    selectedTransportLabel = std::move(label);
    const std::string connectionLabel = selectedTransportLabel;
    connectSelected = [&, clientHandle, connectionLabel] {
      publishTransportEvent("retrying", "Connecting using " + connectionLabel);
      selectedFlow = clientHandle->connect(
          [&, connectionLabel](const auto &, core::socket::State state) {
            if (state == core::socket::State::OK ||
                state == core::socket::State::DISABLED)
              return;
            const std::string failure = "failed to connect using " +
                                        connectionLabel + ": " + state.what();
            core::EventReceiver::atNextTick([&, flow = selectedFlow, failure] {
              if (eventLoopRunning && !shutdownRequested && flow &&
                  flow == selectedFlow && flow->isTerminated())
                publishTransportEvent("failure", failure);
            });
          });
    };
    disableSelected = [config] { config->Instance::setDisabled(true); };
  };

  const auto connectionSettings = [&] {
    nlohmann::json available = nlohmann::json::array();
    available.push_back({{"key", "unix"},
                         {"label", "Unix socket"},
                         {"kind", "unix"},
                         {"path", unixClient.getConfig()->Remote::getSunPath()},
                         {"tls", false}});
    const auto addNetwork = [&available](const auto &configuredClient,
                                         const char *key, const char *label,
                                         const char *kind, bool tls,
                                         std::string webSocketPath = {}) {
      nlohmann::json entry{
          {"key", key},
          {"label", label},
          {"kind", kind},
          {"host", configuredClient.getConfig()->Remote::getHost()},
          {"port", configuredClient.getConfig()->Remote::getPort()},
          {"tls", tls}};
      if (!webSocketPath.empty())
        entry["webSocketPath"] = std::move(webSocketPath);
      available.push_back(std::move(entry));
    };
    addNetwork(ipv4Client, "ipv4", "IPv4", "network", false);
    addNetwork(ipv6Client, "ipv6", "IPv6", "network", false);
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
    addNetwork(tlsIpv4Client, "tls-ipv4", "IPv4 TLS", "network", true);
    addNetwork(tlsIpv6Client, "tls-ipv6", "IPv6 TLS", "network", true);
#endif
#if defined(CODEXUI_CODEX_FRONTEND_RFCOMM)
    available.push_back(
        {{"key", "rfcomm"},
         {"label", "RFCOMM"},
         {"kind", "rfcomm"},
         {"address", rfcommClient.getConfig()->Remote::getBtAddress()},
         {"channel", rfcommClient.getConfig()->Remote::getChannel()},
         {"tls", false}});
    available.push_back(
        {{"key", "rfcomm-tls"},
         {"label", "RFCOMM TLS"},
         {"kind", "rfcomm"},
         {"address", rfcommTlsClient.getConfig()->Remote::getBtAddress()},
         {"channel", rfcommTlsClient.getConfig()->Remote::getChannel()},
         {"tls", true}});
#endif
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
    addNetwork(webSocketIpv4Client, "websocket-ipv4", "WebSocket IPv4",
               "websocket", false, currentWebSocketEndpoint);
    addNetwork(webSocketIpv6Client, "websocket-ipv6", "WebSocket IPv6",
               "websocket", false, currentWebSocketEndpoint);
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
    addNetwork(wssIpv4Client, "wss-ipv4", "WSS IPv4", "websocket", true,
               currentWebSocketEndpoint);
    addNetwork(wssIpv6Client, "wss-ipv6", "WSS IPv6", "websocket", true,
               currentWebSocketEndpoint);
#endif
#endif
    return nlohmann::json{{"selected", selectedTransport},
                          {"available", std::move(available)}};
  };

  const auto publishConnectionSettings = [&] {
    nlohmann::json settings = connectionSettings();
    static_cast<void>(workerLogic.connectionSettings(decodedObject(settings)));
  };

  continueTransition = [&] {
    if (shutdownRequested || !transitionPending)
      return;
    if ((selectedFlow && !selectedFlow->isTerminated()) ||
        connection.attached()) {
      if (std::chrono::steady_clock::now() >= transitionDeadline) {
        publishTransportEvent("failure", "connection transition timed out");
        requestShutdown();
        return;
      }
      static_cast<void>(core::timer::Timer::singleshotTimer(
          continueTransition, utils::Timeval({0, 10000})));
      return;
    }
    transitionPending = false;
    if (pendingSelection) {
      std::function<void()> selection = std::move(pendingSelection);
      pendingSelection = {};
      selection();
      publishConnectionSettings();
    }
    if (desiredConnected && connectSelected)
      connectSelected();
  };

  const auto beginTransition = [&](bool connectAfterwards,
                                   std::function<void()> selection = {},
                                   std::string disconnectReason = {}) {
    if (shutdownRequested || transitionPending)
      return;
    desiredConnected = connectAfterwards;
    pendingSelection = std::move(selection);
    if (!selectedFlow ||
        (selectedFlow->isTerminated() && !connection.attached())) {
      if (pendingSelection) {
        std::function<void()> selected = std::move(pendingSelection);
        pendingSelection = {};
        selected();
        publishConnectionSettings();
      }
      if (desiredConnected && connectSelected)
        connectSelected();
      return;
    }
    transitionPending = true;
    transitionDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    expectedDisconnectReason =
        connection.attached() ? std::move(disconnectReason) : std::string{};
    connection.disconnect("CodexUI connection transition");
    static_cast<void>(selectedFlow->terminateFlow());
    static_cast<void>(core::timer::Timer::singleshotTimer(
        continueTransition, utils::Timeval({0, 10000})));
  };

  requestReconnect = [&] { beginTransition(true, {}, "local-user-reconnect"); };

  const auto requestConnect = [&] {
    if (shutdownRequested)
      return;
    desiredConnected = true;
    if (transitionPending || connection.attached())
      return;
    if (!selectedFlow || selectedFlow->isTerminated()) {
      if (connectSelected)
        connectSelected();
    }
  };

  const auto requestDisconnect = [&] {
    beginTransition(false, {}, "local-user-disconnect");
  };

  requestShutdown = [&] {
    if (shutdownRequested)
      return;
    shutdownRequested = true;
    transitionPending = false;
    desiredConnected = false;
    connection.shutdown();
    if (selectedFlow)
      static_cast<void>(selectedFlow->terminateFlow());
    if (eventLoopRunning)
      core::SNodeC::stop();
  };

  const auto currentThreadId =
      [&graph](const nodegraph::NodeRef &target) -> std::optional<std::string> {
    if (!target)
      return std::nullopt;
    auto write = graph.write();
    if (write.find(target->id()) != target) {
      static_cast<void>(write.finish());
      return std::nullopt;
    }
    nodegraph::NodeRef current = target;
    while (current && current->id().kind != nodegraph::NodeKind::Thread)
      current = write.parent(current);
    const std::optional<std::string> result =
        current ? std::optional<std::string>(current->id().canonical)
                : std::nullopt;
    static_cast<void>(write.finish());
    return result;
  };

  const auto currentNodeState = [&graph](const nodegraph::NodeRef &target)
      -> std::shared_ptr<const nodegraph::NodeState> {
    if (!target)
      return {};
    auto write = graph.write();
    std::shared_ptr<const nodegraph::NodeState> result;
    if (write.find(target->id()) == target)
      result = write.state(target);
    static_cast<void>(write.finish());
    return result;
  };

  const auto currentNode = [&graph](nodegraph::NodeId id) {
    auto write = graph.write();
    nodegraph::NodeRef result = write.find(id);
    static_cast<void>(write.finish());
    return result;
  };

  const auto conversationSnapshotPending =
      [&graph](const nodegraph::NodeRef &target) {
        if (!target)
          return false;
        auto write = graph.write();
        bool pending = false;
        if (write.find(target->id()) == target) {
          for (const nodegraph::NodeRef &operation : write.related(
                   target, nodegraph::RelationKind::PendingOperation)) {
            const auto state = write.state(operation);
            const std::string method =
                exactStringFromValue(valueMember(*state, "method"));
            if (state->status == nodegraph::NodeStatus::Pending &&
                (method == "thread/read" || method == "thread/resume" ||
                 method == "thread/turns/list")) {
              pending = true;
              break;
            }
          }
        }
        static_cast<void>(write.finish());
        return pending;
      };

  const auto normalizedThreadListParameters = [](nlohmann::json parameters) {
    parameters.erase("cursor");
    parameters.erase("useStateDbOnly");
    parameters["sortKey"] = "recency_at";
    parameters["sortDirection"] = "desc";
    parameters["limit"] = 100;
    return parameters;
  };

  const auto requestThreadListPage =
      [&](nlohmann::json parameters,
          std::function<void(RequestOutcome)> completed) {
        threadListPending = true;
        dispatchRequest<codex::generated::client_requests::ThreadList>(
            sdk, std::move(parameters), workerLogic, {},
            [&,
             completed = std::move(completed)](RequestOutcome outcome) mutable {
              threadListPending = false;
              completed(std::move(outcome));
            });
      };

  std::function<void()> requestMoreThreads;
  requestMoreThreads = [&] {
    if (threadListPending || threadListRepairPending) {
      threadListLoadMoreRequested = true;
      return;
    }
    if (threadListNextCursor.empty())
      return;
    const std::string cursor = threadListNextCursor;
    if (!threadListSeenCursors.insert(cursor).second) {
      threadListNextCursor.clear();
      threadListLoadMoreRequested = false;
      return;
    }
    const std::uint64_t cycle = threadListCycle;
    nlohmann::json parameters = threadListBaseParameters;
    parameters["useStateDbOnly"] = true;
    parameters["cursor"] = cursor;
    requestThreadListPage(
        std::move(parameters), [&, cycle, cursor](RequestOutcome outcome) {
          if (cycle != threadListCycle)
            return;
          if (outcome.stale) {
            threadListSeenCursors.erase(cursor);
            threadListLoadMoreRequested = false;
            return;
          }
          if (!outcome.ok) {
            threadListSeenCursors.erase(cursor);
            threadListLoadMoreRequested = false;
            showNotice(outcome.error);
            return;
          }
          threadListNextCursor = std::move(outcome.nextCursor);
          if (threadListLoadMoreRequested) {
            threadListLoadMoreRequested = false;
            requestMoreThreads();
          }
        });
  };

  const auto requestThreadListRepair = [&](std::uint64_t cycle) {
    if (cycle != threadListCycle || threadListPending)
      return;
    threadListRepairPending = true;
    nlohmann::json parameters = threadListBaseParameters;
    parameters["useStateDbOnly"] = false;
    requestThreadListPage(
        std::move(parameters), [&, cycle](RequestOutcome outcome) {
          if (cycle != threadListCycle)
            return;
          threadListRepairPending = false;
          if (outcome.stale)
            return;
          if (!outcome.ok) {
            showNotice(outcome.error);
            if (threadListLoadMoreRequested) {
              threadListLoadMoreRequested = false;
              requestMoreThreads();
            }
            return;
          }
          threadListNextCursor = std::move(outcome.nextCursor);
          if (threadListLoadMoreRequested) {
            threadListLoadMoreRequested = false;
            requestMoreThreads();
          }
        });
  };

  const auto requestThreadList = [&](nlohmann::json parameters) {
    if (threadListPending)
      return;
    const std::uint64_t cycle = ++threadListCycle;
    threadListRepairPending = true;
    threadListLoadMoreRequested = false;
    threadListNextCursor.clear();
    threadListSeenCursors.clear();
    threadListBaseParameters =
        normalizedThreadListParameters(std::move(parameters));
    nlohmann::json fastParameters = threadListBaseParameters;
    fastParameters["useStateDbOnly"] = true;
    requestThreadListPage(
        std::move(fastParameters), [&, cycle](RequestOutcome outcome) {
          if (cycle != threadListCycle)
            return;
          if (outcome.stale) {
            threadListRepairPending = false;
            return;
          }
          if (!outcome.ok) {
            showNotice(outcome.error);
          } else {
            threadListNextCursor = std::move(outcome.nextCursor);
          }
          const nodegraph::WorkerGenerations expectedGenerations =
              workerLogic.generations();
          static_cast<void>(core::timer::Timer::singleshotTimer(
              [&, cycle, expectedGenerations] {
                if (workerLogic.generations() == expectedGenerations)
                  requestThreadListRepair(cycle);
              },
              utils::Timeval({0, 50000})));
        });
  };

  const auto requestModelList = [&](nlohmann::json parameters) {
    if (modelListPending)
      return;
    modelListPending = true;
    dispatchRequest<codex::generated::client_requests::ModelList>(
        sdk, std::move(parameters), workerLogic, {},
        [&modelListPending, &showNotice](RequestOutcome outcome) {
          modelListPending = false;
          if (!outcome.stale && !outcome.ok)
            showNotice(outcome.error);
        });
  };

  const auto requestPermissionProfiles = [&](nlohmann::json parameters) {
    if (permissionProfilesPending)
      return;
    permissionProfilesPending = true;
    dispatchRequest<codex::generated::client_requests::PermissionProfileList>(
        sdk, std::move(parameters), workerLogic, {},
        [&permissionProfilesPending, &showNotice](RequestOutcome outcome) {
          permissionProfilesPending = false;
          if (!outcome.stale && !outcome.ok)
            showNotice(outcome.error);
        });
  };

  hydrateProvider = [&] {
    requestThreadList(nlohmann::json::object());
    requestModelList(nlohmann::json::object());
    requestPermissionProfiles(nlohmann::json::object());
  };

  std::function<void(nodegraph::PromptCommand)> dispatchPrompt;
  const auto failPromptAndContinue = [&](const nodegraph::NodeRef &localPrompt,
                                         std::string error) {
    nodegraph::PromptTransition transition =
        workerLogic.failPrompt(localPrompt, error);
    showNotice(std::move(error));
    if (transition.command)
      dispatchPrompt(std::move(*transition.command));
  };
  dispatchPrompt = [&](nodegraph::PromptCommand command) {
    using nodegraph::PromptCommandKind;
    if (!command.localPrompt)
      return;
    if (!sdk.providerReady() || !sdk.isController()) {
      failPromptAndContinue(command.localPrompt,
                            "Codex is not ready for a controlled turn");
      return;
    }

    if (command.kind == PromptCommandKind::CreateThread) {
      auto pending =
          std::make_shared<nodegraph::PromptCommand>(std::move(command));
      dispatchRequestHandled<codex::generated::client_requests::ThreadStart>(
          sdk, jsonObject(pending->options), workerLogic, pending->localPrompt,
          [](const nodegraph::ProtocolRequestId &) {},
          [&, pending](RequestOutcome outcome,
                       nodegraph::DecodedMessage decoded) mutable {
            if (!outcome.ok) {
              const std::string error = outcome.error;
              nodegraph::PromptTransition transition =
                  workerLogic.completePromptResult(
                      std::move(decoded), pending->localPrompt, false, error);
              showNotice(error);
              if (transition.command)
                dispatchPrompt(std::move(*transition.command));
              return;
            }
            std::string threadId = std::move(outcome.threadId);
            if (threadId.empty()) {
              constexpr std::string_view MissingThread =
                  "Thread creation returned no thread identifier";
              nodegraph::PromptTransition transition =
                  workerLogic.completePromptResult(std::move(decoded),
                                                   pending->localPrompt, false,
                                                   std::string(MissingThread));
              showNotice(std::string(MissingThread));
              if (transition.command)
                dispatchPrompt(std::move(*transition.command));
              return;
            }
            const std::string requestedName = pending->requestedName;
            const nodegraph::Value *ephemeralValue =
                valueMember(pending->options, "ephemeral");
            const bool ephemeral = ephemeralValue && ephemeralValue->asBool() &&
                                   *ephemeralValue->asBool();
            static_cast<void>(workerLogic.completeCreatedThread(
                std::move(decoded), *pending, threadId));
            if (pending->kind == PromptCommandKind::CreateThread ||
                !pending->thread) {
              failPromptAndContinue(
                  pending->localPrompt,
                  "Could not attach the prompt to the created thread");
              return;
            }
            if (!ephemeral && !requestedName.empty()) {
              dispatchRequest<codex::generated::client_requests::ThreadSetName>(
                  sdk,
                  nlohmann::json{{"threadId", threadId},
                                 {"name", requestedName}},
                  workerLogic, pending->thread,
                  [&showNotice](RequestOutcome renameOutcome) {
                    if (!renameOutcome.stale && !renameOutcome.ok)
                      showNotice(renameOutcome.error);
                  });
            }
            dispatchPrompt(std::move(*pending));
          });
      return;
    }

    if (pendingHydrations.contains(command.thread)) {
      promptsWaitingForHydration.insert_or_assign(command.thread,
                                                  std::move(command));
      return;
    }

    const auto threadState = currentNodeState(command.thread);
    const std::string hydrationState =
        threadState
            ? exactStringFromValue(valueMember(*threadState, "hydrationState"))
            : std::string{};
    if (!threadState || hydrationState == "failed") {
      failPromptAndContinue(
          command.localPrompt,
          hydrationState == "failed"
              ? "Reload this thread before sending the preserved prompt"
              : "The destination thread is no longer available");
      return;
    }
    const bool needsResume =
        threadState->status == nodegraph::NodeStatus::NotLoaded;
    const bool explicitlyResumed =
        resumedPromptAdmissions.erase(command.localPrompt) != 0;
    if (needsResume && !explicitlyResumed) {
      auto pending =
          std::make_shared<nodegraph::PromptCommand>(std::move(command));
      dispatchRequest<codex::generated::client_requests::ThreadResume>(
          sdk,
          nlohmann::json{{"threadId", pending->thread->id().canonical},
                         {"excludeTurns", true}},
          workerLogic, pending->thread,
          [&, pending](RequestOutcome outcome) mutable {
            if (outcome.stale)
              return;
            if (!outcome.ok) {
              failPromptAndContinue(pending->localPrompt,
                                    std::move(outcome.error));
              return;
            }
            resumedPromptAdmissions.insert(pending->localPrompt);
            dispatchPrompt(std::move(*pending));
          });
      return;
    }

    nlohmann::json parameters = jsonObject(command.options);
    parameters["threadId"] = command.thread->id().canonical;
    parameters["clientUserMessageId"] = command.clientUserMessageId;
    parameters["input"] = promptInput(command.promptText, command.attachments);

    const nodegraph::NodeRef localPrompt = command.localPrompt;
    const auto published = [&workerLogic, localPrompt](
                               const nodegraph::ProtocolRequestId &requestId) {
      static_cast<void>(
          workerLogic.markPromptDispatched(localPrompt, requestId));
    };
    const auto completed = [&, localPrompt](RequestOutcome outcome,
                                            nodegraph::DecodedMessage decoded) {
      std::optional<std::string> turnId;
      std::string id = std::move(outcome.turnId);
      if (!id.empty())
        turnId = std::move(id);
      nodegraph::PromptTransition transition = workerLogic.completePromptResult(
          std::move(decoded), localPrompt, outcome.ok, outcome.error,
          std::move(turnId));
      if (!outcome.ok)
        showNotice(outcome.error);
      if (transition.command)
        dispatchPrompt(std::move(*transition.command));
    };

    if (command.kind == PromptCommandKind::SteerTurn) {
      parameters["expectedTurnId"] = command.expectedTurnId;
      dispatchRequestHandled<codex::generated::client_requests::TurnSteer>(
          sdk, std::move(parameters), workerLogic, localPrompt, published,
          completed);
    } else {
      dispatchRequestHandled<codex::generated::client_requests::TurnStart>(
          sdk, std::move(parameters), workerLogic, localPrompt, published,
          completed);
    }
  };

  const auto continuePromptAfterHydration =
      [&](const nodegraph::NodeRef &thread, std::string error,
          bool resumed = false) {
        auto waiting = promptsWaitingForHydration.find(thread);
        if (waiting == promptsWaitingForHydration.end())
          return;
        nodegraph::PromptCommand command = std::move(waiting->second);
        promptsWaitingForHydration.erase(waiting);
        if (!error.empty()) {
          failPromptAndContinue(command.localPrompt, std::move(error));
          return;
        }
        if (resumed)
          resumedPromptAdmissions.insert(command.localPrompt);
        dispatchPrompt(std::move(command));
      };

  const auto hydrationReady = [&](const nodegraph::NodeRef &thread) {
    const auto state = currentNodeState(thread);
    return state &&
           exactStringFromValue(valueMember(*state, "hydrationState")) ==
               "ready" &&
           unsignedIntegerFromValue(
               valueMember(*state, "hydrationConnectionGeneration")) ==
               std::optional<std::uint64_t>(
                   workerLogic.generations().connection);
  };

  constexpr std::size_t MaximumHistoricalHydrations = 8;
  constexpr std::size_t MaximumItemHydrations = 8;
  std::function<void()> pumpHistoricalHydrations;
  std::function<void()> pumpItemHydrations;
  std::function<void(const nodegraph::NodeRef &, bool, bool)>
      startThreadHydration;
  std::function<void(const nodegraph::NodeRef &)> queueActiveAgentChildren;

  const auto turnPageParameters = [](std::string_view threadId,
                                     std::string_view cursor = {}) {
    nlohmann::json parameters{{"threadId", threadId},
                              {"limit", 80},
                              {"sortDirection", "desc"},
                              {"itemsView", "summary"}};
    if (!cursor.empty())
      parameters["cursor"] = cursor;
    return parameters;
  };

  const auto queueItemHydrations =
      [&](const nodegraph::NodeRef &thread, std::string_view threadId,
          const std::vector<std::string> &turnIds) {
        for (const std::string &turnId : turnIds)
          itemPageQueue.push_back(
              {thread, std::string(threadId), turnId, std::string{}});
        if (pumpItemHydrations)
          pumpItemHydrations();
      };

  pumpItemHydrations = [&] {
    while (pendingItemPages < MaximumItemHydrations && !itemPageQueue.empty()) {
      ItemPage page = std::move(itemPageQueue.front());
      itemPageQueue.pop_front();
      if (!page.thread || !currentThreadId(page.thread))
        continue;
      nlohmann::json parameters{{"threadId", page.threadId},
                                {"turnId", page.turnId},
                                {"limit", 80},
                                {"sortDirection", "desc"}};
      if (!page.cursor.empty())
        parameters["cursor"] = page.cursor;
      ++pendingItemPages;
      dispatchRequest<codex::generated::client_requests::ThreadItemsList>(
          sdk, std::move(parameters), workerLogic, page.thread,
          [&, page = std::move(page)](RequestOutcome outcome) mutable {
            --pendingItemPages;
            if (!outcome.stale && outcome.ok && !outcome.nextCursor.empty()) {
              page.cursor = std::move(outcome.nextCursor);
              itemPageQueue.push_back(std::move(page));
            } else if (!outcome.stale && !outcome.ok) {
              showNotice(outcome.error);
            }
            pumpItemHydrations();
          });
    }
  };

  queueActiveAgentChildren = [&](const nodegraph::NodeRef &parent) {
    for (nodegraph::NodeRef child : workerLogic.activeAgentChildren(parent)) {
      if (!child || hydrationReady(child) ||
          pendingHydrations.contains(child) ||
          !historicalHydrationVisited.insert(child->id()).second)
        continue;
      historicalHydrationQueue.emplace_back(std::move(child));
    }
    if (pumpHistoricalHydrations)
      pumpHistoricalHydrations();
  };

  startThreadHydration = [&](const nodegraph::NodeRef &thread, bool interactive,
                             bool force) {
    const std::optional<std::string> threadId = currentThreadId(thread);
    if (!threadId) {
      if (interactive)
        showNotice("The selected thread is no longer available");
      return;
    }
    if (conversationSnapshotPending(thread) ||
        (!force && hydrationReady(thread)))
      return;
    if (interactive)
      interactiveHydrations.insert(thread);
    pendingHydrations.insert(thread);
    static_cast<void>(workerLogic.threadHydration(thread, "loading"));

    const auto completeTurnPage =
        [&, thread, id = *threadId](bool resumeAttempted, bool resumeSucceeded,
                                    RequestOutcome outcome,
                                    nodegraph::DecodedMessage decoded) mutable {
          const std::vector<std::string> turnIds =
              outcome.ok ? turnIdsFromPage(decoded.payload)
                         : std::vector<std::string>{};
          const bool wasHistorical =
              pendingHistoricalHydrations.erase(thread) != 0;
          const bool needsSettings = interactiveHydrations.erase(thread) != 0;
          pendingHydrations.erase(thread);

          if (!outcome.ok) {
            const std::string error = outcome.error.empty()
                                          ? "Thread hydration failed"
                                          : outcome.error;
            static_cast<void>(workerLogic.completeThreadHydration(
                std::move(decoded), thread, "failed", error));
            continuePromptAfterHydration(thread, error);
            if (wasHistorical && pumpHistoricalHydrations)
              pumpHistoricalHydrations();
            return;
          }
          static_cast<void>(workerLogic.completeThreadHydration(
              std::move(decoded), thread, "ready"));
          if (!currentThreadId(thread)) {
            continuePromptAfterHydration(
                thread, "The selected thread is no longer available");
            if (wasHistorical && pumpHistoricalHydrations)
              pumpHistoricalHydrations();
            return;
          }
          queueActiveAgentChildren(thread);
          // Observers retain read-only thread hydration. Controller authority
          // gates mutations and settings resume, not ordinary selection/read.
          if (!needsSettings || !sdk.isController() || resumeAttempted) {
            queueItemHydrations(thread, id, turnIds);
            continuePromptAfterHydration(thread, {}, resumeSucceeded);
            if (wasHistorical && pumpHistoricalHydrations)
              pumpHistoricalHydrations();
            return;
          }
          dispatchRequest<codex::generated::client_requests::ThreadResume>(
              sdk, nlohmann::json{{"threadId", id}, {"excludeTurns", true}},
              workerLogic, thread,
              [&, thread](RequestOutcome resumeOutcome) mutable {
                if (resumeOutcome.stale) {
                  promptsWaitingForHydration.erase(thread);
                  return;
                }
                if (!resumeOutcome.ok)
                  showNotice(resumeOutcome.error.empty()
                                 ? "Thread settings refresh failed"
                                 : resumeOutcome.error);
                // A successful turn page is hydrated even when the
                // controller-only settings resume fails. A later prompt to a
                // provider-notLoaded thread performs its own metadata-only
                // resume.
                continuePromptAfterHydration(thread, {}, resumeOutcome.ok);
              });
          queueItemHydrations(thread, id, turnIds);
          if (wasHistorical && pumpHistoricalHydrations)
            pumpHistoricalHydrations();
        };
    auto requestTurnPage = [&, thread, id = *threadId,
                            completeTurnPage](bool resumeAttempted,
                                              bool resumeSucceeded) mutable {
      dispatchRequestHandled<
          codex::generated::client_requests::ThreadTurnsList>(
          sdk, turnPageParameters(id), workerLogic, thread,
          [](const nodegraph::ProtocolRequestId &) {},
          [completeTurnPage = std::move(completeTurnPage), resumeAttempted,
           resumeSucceeded](RequestOutcome outcome,
                            nodegraph::DecodedMessage decoded) mutable {
            completeTurnPage(resumeAttempted, resumeSucceeded,
                             std::move(outcome), std::move(decoded));
          });
    };
    if (!interactive || !sdk.isController()) {
      requestTurnPage(false, false);
      return;
    }
    dispatchRequest<codex::generated::client_requests::ThreadResume>(
        sdk, nlohmann::json{{"threadId", *threadId}, {"excludeTurns", true}},
        workerLogic, thread,
        [&, thread, requestTurnPage = std::move(requestTurnPage)](
            RequestOutcome outcome) mutable {
          if (outcome.stale) {
            pendingHydrations.erase(thread);
            interactiveHydrations.erase(thread);
            const bool wasHistorical =
                pendingHistoricalHydrations.erase(thread) != 0;
            promptsWaitingForHydration.erase(thread);
            if (wasHistorical && pumpHistoricalHydrations)
              pumpHistoricalHydrations();
            return;
          }
          if (!outcome.ok)
            showNotice(outcome.error.empty() ? "Thread settings refresh failed"
                                             : outcome.error);
          requestTurnPage(true, outcome.ok);
        });
  };

  pumpHistoricalHydrations = [&] {
    if (historicalHydrationPumpActive)
      return;
    historicalHydrationPumpActive = true;
    while (pendingHistoricalHydrations.size() < MaximumHistoricalHydrations &&
           !historicalHydrationQueue.empty()) {
      nodegraph::NodeRef child = std::move(historicalHydrationQueue.front());
      historicalHydrationQueue.pop_front();
      if (!child || hydrationReady(child) || !currentThreadId(child) ||
          pendingHydrations.contains(child))
        continue;
      pendingHistoricalHydrations.insert(child);
      startThreadHydration(child, false, false);
    }
    historicalHydrationPumpActive = false;
  };

  const auto hydrateThread = [&](const nodegraph::NodeRef &thread,
                                 bool force = false) {
    if (!force && hydrationReady(thread))
      return;
    interactiveHydrations.insert(thread);
    startThreadHydration(thread, true, force);
  };

  hydrateHistoricalChildren = [&](std::string threadId) {
    nodegraph::NodeRef parent =
        currentNode({nodegraph::NodeKind::Thread, std::move(threadId)});
    if (parent)
      queueActiveAgentChildren(parent);
  };

  const auto loadHistory = [&](nodegraph::NodeAction action) {
    if (!action.target ||
        action.target->id().kind != nodegraph::NodeKind::Thread) {
      rejectNodeAction(action, "The selected thread is no longer available");
      return;
    }
    const nodegraph::NodeRef thread = action.target;
    const std::shared_ptr<const nodegraph::NodeState> state =
        currentNodeState(thread);
    const std::optional<std::string> threadId = currentThreadId(thread);
    if (!state || !threadId) {
      rejectNodeAction(action, "The selected thread is no longer available");
      return;
    }
    if (conversationSnapshotPending(thread))
      return;
    const std::string cursor =
        exactStringFromValue(valueMember(*state, "historyNextCursor"));
    dispatchRequestHandled<codex::generated::client_requests::ThreadTurnsList>(
        sdk, turnPageParameters(*threadId, cursor), workerLogic, thread,
        [](const nodegraph::ProtocolRequestId &) {},
        [&, thread, id = *threadId](RequestOutcome outcome,
                                    nodegraph::DecodedMessage decoded) {
          const std::vector<std::string> turnIds =
              outcome.ok ? turnIdsFromPage(decoded.payload)
                         : std::vector<std::string>{};
          const nodegraph::NodeRef expected = decoded.expectedNode;
          const nodegraph::WorkerApplyResult applied =
              workerLogic.applyDetailed(std::move(decoded));
          outcome.stale = expected && applied.primary != expected;
          if (!outcome.stale && outcome.ok)
            queueItemHydrations(thread, id, turnIds);
          else if (!outcome.stale)
            showNotice(outcome.error);
        });
  };

  const auto resolveInteraction = [&](nodegraph::NodeAction action) {
    auto reject = [&](std::string message) {
      static_cast<void>(workerLogic.failInteractionResponse(
          action.target, message, std::move(action.payload)));
      rejectNodeAction(action, std::move(message));
    };
    auto found = pendingServerRequests.find(action.target);
    if (found == pendingServerRequests.end() ||
        found->second.generations != workerLogic.generations() ||
        !sdk.providerReady() || !sdk.isController()) {
      reject("The pending request is no longer actionable");
      return;
    }
    const std::shared_ptr<const nodegraph::NodeState> state =
        currentNodeState(action.target);
    if (!state || exactStringFromValue(valueMember(*state, "method")) !=
                      found->second.method) {
      reject("The pending request is no longer actionable");
      return;
    }
    // A bridge sender may synchronously feed serverRequest/resolved back into
    // CodexBridge. Visit a local typed copy so that such reentrancy can erase
    // the map entry without invalidating the request currently being encoded.
    PendingServerRequest pending = found->second;

    PendingRequestSubmission submission;
    submission.choice =
        exactStringFromValue(valueMember(action.payload, "choice"));
    if (const nodegraph::Value *input = valueMember(action.payload, "input"))
      submission.input = jsonFromValue(*input);
    if (const nodegraph::Value *metadata =
            valueMember(action.payload, "metadata"))
      submission.metadata = jsonFromValue(*metadata);
    const PendingRequestKind kind =
        PendingRequestPolicy::kindForMethod(pending.method);
    const std::optional<bool> accepted = std::visit(
        [&](auto &request) -> std::optional<bool> {
          using Request = std::decay_t<decltype(request)>;
          const std::optional<PendingRequestResponse> response =
              PendingRequestPolicy::responseForSubmission(
                  kind, request.getPayload(), submission);
          if (!response)
            return std::nullopt;
          if constexpr (std::is_same_v<
                            Request, CommandExecutionRequestApproval::Params>) {
            return respondToPendingRequest<CommandExecutionRequestApproval>(
                sdk, request, *response);
          } else if constexpr (std::is_same_v<
                                   Request,
                                   FileChangeRequestApproval::Params>) {
            return respondToPendingRequest<FileChangeRequestApproval>(
                sdk, request, *response);
          } else if constexpr (std::is_same_v<Request,
                                              ToolRequestUserInput::Params>) {
            return respondToPendingRequest<ToolRequestUserInput>(sdk, request,
                                                                 *response);
          } else if constexpr (std::is_same_v<
                                   Request,
                                   McpServerElicitationRequest::Params>) {
            return respondToPendingRequest<McpServerElicitationRequest>(
                sdk, request, *response);
          } else if constexpr (std::is_same_v<
                                   Request,
                                   PermissionsRequestApproval::Params>) {
            return respondToPendingRequest<PermissionsRequestApproval>(
                sdk, request, *response);
          } else if constexpr (std::is_same_v<Request,
                                              DynamicToolCall::Params>) {
            return respondToPendingRequest<DynamicToolCall>(sdk, request,
                                                            *response);
          } else if constexpr (std::is_same_v<
                                   Request, ChatgptAuthTokensRefresh::Params>) {
            return respondToPendingRequest<ChatgptAuthTokensRefresh>(
                sdk, request, *response);
          } else if constexpr (std::is_same_v<Request,
                                              AttestationGenerate::Params>) {
            return respondToPendingRequest<AttestationGenerate>(sdk, request,
                                                                *response);
          } else if constexpr (std::is_same_v<Request,
                                              ApplyPatchApproval::Params>) {
            return respondToPendingRequest<ApplyPatchApproval>(sdk, request,
                                                               *response);
          } else if constexpr (std::is_same_v<Request,
                                              ExecCommandApproval::Params>) {
            return respondToPendingRequest<ExecCommandApproval>(sdk, request,
                                                                *response);
          }
        },
        pending.request);

    if (!accepted) {
      reject("The authored pending response is invalid.");
    } else if (*accepted) {
      static_cast<void>(workerLogic.resolveInteraction(action.target));
      pendingServerRequests.erase(action.target);
    } else {
      static_cast<void>(workerLogic.failInteractionResponse(
          action.target, "CodexBridge rejected the server-request response",
          std::move(action.payload)));
      rejectNodeAction(action, "The pending response could not be sent");
    }
  };

  const auto dispatchNodeAction = [&](nodegraph::NodeAction action) {
    using enum nodegraph::NodeActionKind;
    switch (action.kind) {
    case Hydrate:
      hydrateThread(action.target);
      return;
    case Reload:
      hydrateThread(action.target, true);
      return;
    case LoadHistory:
      loadHistory(std::move(action));
      return;
    case Rename:
    case Fork:
    case Archive:
    case Unarchive:
    case Delete: {
      if (!sdk.providerReady() || !sdk.isController()) {
        rejectNodeAction(
            action, "Controller access is unavailable for this thread action");
        return;
      }
      if (!action.target ||
          action.target->id().kind != nodegraph::NodeKind::Thread) {
        rejectNodeAction(action, "The selected thread is no longer available");
        return;
      }
      const std::shared_ptr<const nodegraph::NodeState> threadState =
          currentNodeState(action.target);
      const auto stateFlag = [&threadState](std::string_view name) {
        if (!threadState)
          return false;
        const nodegraph::Value *value = valueMember(threadState->fields, name);
        return value && value->asBool() && *value->asBool();
      };
      if (!threadState || stateFlag("local") || stateFlag("recoveryOnly")) {
        rejectNodeAction(action, "The selected thread is no longer available");
        return;
      }
      const bool archived = stateFlag("archived");
      if ((action.kind == Archive && archived) ||
          (action.kind == Unarchive && !archived)) {
        rejectNodeAction(action, "The thread action is no longer applicable");
        return;
      }
      const std::string threadId =
          nodegraph::protocolCanonicalId(*threadState, action.target);
      if (threadId.empty()) {
        rejectNodeAction(action, "The selected thread is no longer available");
        return;
      }
      if (action.kind == Rename) {
        const nodegraph::Value *nameValue = valueMember(action.payload, "name");
        const std::string *name = nameValue ? nameValue->asString() : nullptr;
        if (!name ||
            name->find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
          rejectNodeAction(action, "A non-empty thread name is required");
          return;
        }
      }
      std::string requestedForkName;
      bool ephemeralFork = false;
      if (action.kind == Fork) {
        const nodegraph::Value *nameValue =
            valueMember(action.payload, "requestedName");
        if (const std::string *name =
                nameValue ? nameValue->asString() : nullptr)
          requestedForkName = *name;
        const nodegraph::Value *ephemeralValue =
            valueMember(action.payload, "ephemeral");
        ephemeralFork = ephemeralValue && ephemeralValue->asBool() &&
                        *ephemeralValue->asBool();
        action.payload.erase("requestedName");
      }
      nlohmann::json parameters = jsonObject(std::move(action.payload));
      parameters["threadId"] = threadId;
      if (action.kind == Fork)
        parameters["excludeTurns"] = true;
      const nodegraph::NodeRef target = std::move(action.target);
      const auto completed = [&showNotice](RequestOutcome outcome) {
        if (!outcome.stale && !outcome.ok)
          showNotice(outcome.error);
      };
      if (action.kind == Rename)
        dispatchRequest<codex::generated::client_requests::ThreadSetName>(
            sdk, std::move(parameters), workerLogic, target, completed);
      else if (action.kind == Fork)
        dispatchRequestHandled<codex::generated::client_requests::ThreadFork>(
            sdk, std::move(parameters), workerLogic, target,
            [](const nodegraph::ProtocolRequestId &) {},
            [&, requestedForkName, ephemeralFork](
                RequestOutcome outcome, nodegraph::DecodedMessage decoded) {
              if (!outcome.ok) {
                static_cast<void>(
                    workerLogic.applyDetailed(std::move(decoded)));
                showNotice(outcome.error);
                return;
              }
              std::string forkId = std::move(outcome.threadId);
              if (forkId.empty()) {
                static_cast<void>(
                    workerLogic.applyDetailed(std::move(decoded)));
                showNotice("Thread fork returned no thread identifier");
                return;
              }
              static_cast<void>(workerLogic.completeFork(
                  std::move(decoded), forkId, requestedForkName));
              nodegraph::NodeRef fork =
                  currentNode({nodegraph::NodeKind::Thread, forkId});
              if (!fork)
                return;
              if (!ephemeralFork && !requestedForkName.empty()) {
                dispatchRequest<
                    codex::generated::client_requests::ThreadSetName>(
                    sdk,
                    nlohmann::json{{"threadId", forkId},
                                   {"name", requestedForkName}},
                    workerLogic, fork,
                    [&showNotice](RequestOutcome renameOutcome) {
                      if (!renameOutcome.stale && !renameOutcome.ok)
                        showNotice(renameOutcome.error);
                    });
              }
              // A metadata-only fork is already live and must not gate its
              // first prompt on retained history. Hydrate that history through
              // the same bounded page path in parallel.
              static_cast<void>(workerLogic.selectThread(fork));
              loadHistory(nodegraph::NodeAction{
                  fork, nodegraph::NodeActionKind::LoadHistory, {}, {}, {}, {}});
            });
      else if (action.kind == Archive)
        dispatchRequest<codex::generated::client_requests::ThreadArchive>(
            sdk, std::move(parameters), workerLogic, target, completed);
      else if (action.kind == Unarchive)
        dispatchRequest<codex::generated::client_requests::ThreadUnarchive>(
            sdk, std::move(parameters), workerLogic, target, completed);
      else
        dispatchRequest<codex::generated::client_requests::ThreadDelete>(
            sdk, std::move(parameters), workerLogic, target, completed);
      return;
    }
    case SubmitPrompt: {
      nodegraph::PromptTransition transition = workerLogic.admitPrompt(
          std::move(action), threadActivityAt("turn/start"),
          wallClockMilliseconds());
      if (transition.command) {
        if (!sdk.providerReady() || !sdk.isController()) {
          failPromptAndContinue(transition.command->localPrompt,
                                "Codex is not ready for a controlled turn");
        } else {
          dispatchPrompt(std::move(*transition.command));
        }
      }
      return;
    }
    case InterruptTurn: {
      if (!sdk.providerReady() || !sdk.isController() || !action.target ||
          action.target->id().kind != nodegraph::NodeKind::Turn) {
        rejectNodeAction(action,
                         "No controlled active turn is available to stop");
        return;
      }
      std::optional<std::pair<std::string, std::string>> currentActiveTurn;
      {
        auto write = graph.write();
        if (write.find(action.target->id()) == action.target &&
            write.state(action.target)->status ==
                nodegraph::NodeStatus::Running) {
          const nodegraph::NodeRef thread = write.parent(action.target);
          if (thread && thread->id().kind == nodegraph::NodeKind::Thread) {
            const std::shared_ptr<const nodegraph::NodeState> threadState =
                write.state(thread);
            const nodegraph::Value *archived =
                valueMember(threadState->fields, "archived");
            const nodegraph::Value *local =
                valueMember(threadState->fields, "local");
            const nodegraph::Value *recoveryOnly =
                valueMember(threadState->fields, "recoveryOnly");
            const bool threadUnavailable =
                (archived && archived->asBool() && *archived->asBool()) ||
                (local && local->asBool() && *local->asBool()) ||
                (recoveryOnly && recoveryOnly->asBool() &&
                 *recoveryOnly->asBool());
            const std::vector<nodegraph::NodeRef> active =
                write.related(thread, nodegraph::RelationKind::ActiveTurn);
            if (!threadUnavailable &&
                std::find(active.begin(), active.end(), action.target) !=
                    active.end()) {
              const std::shared_ptr<const nodegraph::NodeState> turnState =
                  write.state(action.target);
              const nodegraph::Value *turnLocal =
                  valueMember(turnState->fields, "local");
              if (!turnLocal || !turnLocal->asBool() || !*turnLocal->asBool()) {
                currentActiveTurn = std::pair{
                    nodegraph::protocolCanonicalId(*threadState, thread),
                    nodegraph::protocolCanonicalId(*turnState, action.target)};
              }
            }
          }
        }
        static_cast<void>(write.finish());
      }
      if (!currentActiveTurn || currentActiveTurn->first.empty() ||
          currentActiveTurn->second.empty()) {
        rejectNodeAction(action,
                         "No controlled active turn is available to stop");
        return;
      }
      nlohmann::json parameters = jsonObject(std::move(action.payload));
      parameters["threadId"] = currentActiveTurn->first;
      parameters["turnId"] = currentActiveTurn->second;
      dispatchRequest<codex::generated::client_requests::TurnInterrupt>(
          sdk, std::move(parameters), workerLogic, action.target,
          [&showNotice](RequestOutcome outcome) {
            if (!outcome.stale && !outcome.ok)
              showNotice(outcome.error);
          });
      return;
    }
    case ResolveInteraction:
      resolveInteraction(std::move(action));
      return;
    case PromptMaterialized: {
      static_cast<void>(
          workerLogic.promptMaterialized(std::move(action.target)));
      return;
    }
    case UiDetached:
      static_cast<void>(
          workerLogic.acknowledgeUiDetached(std::move(action.target)));
      return;
    }
  };

  const auto configureConnection = [&](nodegraph::RuntimeAction action) {
    if (transitionPending) {
      rejectRuntimeAction(action,
                          "A connection transition is already in progress");
      return;
    }
    const nlohmann::json parameters = jsonObject(std::move(action.payload));
    const std::string transport = jsonString(parameters, "transport");
    std::function<void()> selection;
    const auto networkEndpoint =
        [&]() -> std::optional<std::pair<std::string, std::uint16_t>> {
      const std::string host = jsonString(parameters, "host");
      const auto port = parameters.find("port");
      if (host.empty() || port == parameters.end() ||
          !port->is_number_integer())
        return std::nullopt;
      const std::int64_t value = port->get<std::int64_t>();
      if (value <= 0 || value > 65535)
        return std::nullopt;
      return std::pair{host, static_cast<std::uint16_t>(value)};
    };

    if (transport == "unix") {
      const std::string path = jsonString(parameters, "path");
      if (!path.empty())
        selection = [&, path] {
          unixClient.getConfig()->Remote::setSunPath(path);
          selectClient(unixClient, "unix", "Unix socket");
        };
    } else if (transport == "ipv4") {
      if (const auto endpoint = networkEndpoint())
        selection = [&, endpoint] {
          ipv4Client.getConfig()
              ->Remote::setHost(endpoint->first)
              ->setPort(endpoint->second);
          selectClient(ipv4Client, "ipv4", "IPv4");
        };
    } else if (transport == "ipv6") {
      if (const auto endpoint = networkEndpoint())
        selection = [&, endpoint] {
          ipv6Client.getConfig()
              ->Remote::setHost(endpoint->first)
              ->setPort(endpoint->second);
          selectClient(ipv6Client, "ipv6", "IPv6");
        };
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
    } else if (transport == "tls-ipv4") {
      if (const auto endpoint = networkEndpoint())
        selection = [&, endpoint] {
          tlsIpv4Client.getConfig()
              ->Remote::setHost(endpoint->first)
              ->setPort(endpoint->second);
          selectClient(tlsIpv4Client, "tls-ipv4", "IPv4 TLS");
        };
    } else if (transport == "tls-ipv6") {
      if (const auto endpoint = networkEndpoint())
        selection = [&, endpoint] {
          tlsIpv6Client.getConfig()
              ->Remote::setHost(endpoint->first)
              ->setPort(endpoint->second);
          selectClient(tlsIpv6Client, "tls-ipv6", "IPv6 TLS");
        };
#endif
#if defined(CODEXUI_CODEX_FRONTEND_RFCOMM)
    } else if (transport == "rfcomm" || transport == "rfcomm-tls") {
      const std::string address = jsonString(parameters, "address");
      const auto channel = parameters.find("channel");
      if (!address.empty() && channel != parameters.end() &&
          channel->is_number_integer() && channel->get<int>() > 0 &&
          channel->get<int>() <= 30) {
        const auto value = static_cast<std::uint8_t>(channel->get<int>());
        if (transport == "rfcomm") {
          selection = [&, address, value] {
            rfcommClient.getConfig()->Remote::setBtAddress(address)->setChannel(
                value);
            selectClient(rfcommClient, "rfcomm", "RFCOMM");
          };
        } else {
          selection = [&, address, value] {
            rfcommTlsClient.getConfig()
                ->Remote::setBtAddress(address)
                ->setChannel(value);
            selectClient(rfcommTlsClient, "rfcomm-tls", "RFCOMM TLS");
          };
        }
      }
#endif
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
    } else if (transport == "websocket-ipv4" || transport == "websocket-ipv6"
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
               || transport == "wss-ipv4" || transport == "wss-ipv6"
#endif
    ) {
      const auto endpoint = networkEndpoint();
      const std::string path = jsonString(parameters, "webSocketPath");
      if (endpoint && !path.empty() && path.front() == '/') {
        if (transport == "websocket-ipv4") {
          selection = [&, endpoint, path] {
            currentWebSocketEndpoint = path;
            webSocketIpv4Client.getConfig()
                ->Remote::setHost(endpoint->first)
                ->setPort(endpoint->second);
            selectClient(webSocketIpv4Client, "websocket-ipv4",
                         "WebSocket IPv4");
          };
        } else if (transport == "websocket-ipv6") {
          selection = [&, endpoint, path] {
            currentWebSocketEndpoint = path;
            webSocketIpv6Client.getConfig()
                ->Remote::setHost(endpoint->first)
                ->setPort(endpoint->second);
            selectClient(webSocketIpv6Client, "websocket-ipv6",
                         "WebSocket IPv6");
          };
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
        } else if (transport == "wss-ipv4") {
          selection = [&, endpoint, path] {
            currentWebSocketEndpoint = path;
            wssIpv4Client.getConfig()
                ->Remote::setHost(endpoint->first)
                ->setPort(endpoint->second);
            selectClient(wssIpv4Client, "wss-ipv4", "WSS IPv4");
          };
        } else {
          selection = [&, endpoint, path] {
            currentWebSocketEndpoint = path;
            wssIpv6Client.getConfig()
                ->Remote::setHost(endpoint->first)
                ->setPort(endpoint->second);
            selectClient(wssIpv6Client, "wss-ipv6", "WSS IPv6");
          };
#endif
        }
      }
#endif
    }
    if (!selection) {
      rejectRuntimeAction(action, "Invalid connection settings");
      return;
    }
    beginTransition(true, std::move(selection), "local-transport-switch");
  };

  const auto dispatchRuntimeAction = [&](nodegraph::RuntimeAction action) {
    using enum nodegraph::RuntimeActionKind;
    switch (action.kind) {
    case RefreshThreads:
      requestThreadList(jsonObject(std::move(action.payload)));
      return;
    case LoadMoreThreads:
      requestMoreThreads();
      return;
    case CreateThread: {
      nodegraph::PromptTransition transition = workerLogic.admitFirstPrompt(
          std::move(action), threadActivityAt("thread/start"),
          wallClockMilliseconds());
      if (transition.command) {
        if (!sdk.providerReady() || !sdk.isController()) {
          failPromptAndContinue(transition.command->localPrompt,
                                "Codex is not ready to create a thread");
        } else {
          dispatchPrompt(std::move(*transition.command));
        }
      }
      return;
    }
    case Connect:
      requestConnect();
      return;
    case Disconnect:
      requestDisconnect();
      return;
    case Reconnect:
      requestReconnect();
      return;
    case ConfigureConnection:
      configureConnection(std::move(action));
      return;
    case ClaimController:
      if (!sdk.claimController())
        rejectRuntimeAction(action, "Controller claim was rejected");
      return;
    case ReleaseController:
      if (!sdk.releaseController())
        rejectRuntimeAction(action, "Controller release was rejected");
      return;
    case RefreshCatalogs: {
      nlohmann::json parameters = jsonObject(std::move(action.payload));
      requestModelList(parameters);
      requestPermissionProfiles(std::move(parameters));
      return;
    }
    }
  };
  const bool workerMailboxReady =
      WorkerMailboxReceiver::create(
          channels,
          [&dispatchNodeAction, &dispatchRuntimeAction,
           &requestShutdown](nodegraph::QtToWorkerMessage message) {
            std::visit(
                [&](auto &payload) {
                  using Message = std::decay_t<decltype(payload)>;
                  if constexpr (std::is_same_v<Message, nodegraph::NodeAction>)
                    dispatchNodeAction(std::move(payload));
                  else if constexpr (std::is_same_v<Message,
                                                    nodegraph::RuntimeAction>)
                    dispatchRuntimeAction(std::move(payload));
                  else if (requestShutdown)
                    requestShutdown();
                },
                message);
          },
          [&publishTransportEvent, &requestShutdown](std::string reason) {
            publishTransportEvent("failure", std::move(reason));
            if (requestShutdown)
              requestShutdown();
          }) != nullptr;

  eventLoopRunning = true;
  core::EventReceiver::atNextTick([&] {
    publishTransportEvent("runtime-started");
    if (!workerMailboxReady) {
      publishTransportEvent("failure",
                            "unable to observe the Qt-to-worker eventfd");
      requestShutdown();
      return;
    }
    const std::array disabled{
        unixClient.getConfig()->Instance::getDisabled(),
        ipv4Client.getConfig()->Instance::getDisabled(),
        ipv6Client.getConfig()->Instance::getDisabled(),
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
        tlsIpv4Client.getConfig()->Instance::getDisabled(),
        tlsIpv6Client.getConfig()->Instance::getDisabled(),
#endif
#if defined(CODEXUI_CODEX_FRONTEND_RFCOMM)
        rfcommClient.getConfig()->Instance::getDisabled(),
        rfcommTlsClient.getConfig()->Instance::getDisabled(),
#endif
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
        webSocketIpv4Client.getConfig()->Instance::getDisabled(),
        webSocketIpv6Client.getConfig()->Instance::getDisabled(),
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
        wssIpv4Client.getConfig()->Instance::getDisabled(),
        wssIpv6Client.getConfig()->Instance::getDisabled(),
#endif
#endif
    };
    const std::size_t enabled = static_cast<std::size_t>(
        std::count(disabled.begin(), disabled.end(), false));
    if (enabled != 1) {
      publishTransportEvent(
          "failure",
          "exactly one outgoing bridge transport must be enabled; found " +
              std::to_string(enabled));
      requestShutdown();
      return;
    }

    if (!unixClient.getConfig()->Instance::getDisabled())
      selectClient(unixClient, "unix", "Unix socket");
    else if (!ipv4Client.getConfig()->Instance::getDisabled())
      selectClient(ipv4Client, "ipv4", "IPv4");
    else if (!ipv6Client.getConfig()->Instance::getDisabled())
      selectClient(ipv6Client, "ipv6", "IPv6");
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
    else if (!tlsIpv4Client.getConfig()->Instance::getDisabled())
      selectClient(tlsIpv4Client, "tls-ipv4", "IPv4 TLS");
    else if (!tlsIpv6Client.getConfig()->Instance::getDisabled())
      selectClient(tlsIpv6Client, "tls-ipv6", "IPv6 TLS");
#endif
#if defined(CODEXUI_CODEX_FRONTEND_RFCOMM)
    else if (!rfcommClient.getConfig()->Instance::getDisabled())
      selectClient(rfcommClient, "rfcomm", "RFCOMM");
    else if (!rfcommTlsClient.getConfig()->Instance::getDisabled())
      selectClient(rfcommTlsClient, "rfcomm-tls", "RFCOMM TLS");
#endif
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
    else if (!webSocketIpv4Client.getConfig()->Instance::getDisabled())
      selectClient(webSocketIpv4Client, "websocket-ipv4", "WebSocket IPv4");
    else if (!webSocketIpv6Client.getConfig()->Instance::getDisabled())
      selectClient(webSocketIpv6Client, "websocket-ipv6", "WebSocket IPv6");
#if defined(CODEXUI_CODEX_FRONTEND_TLS)
    else if (!wssIpv4Client.getConfig()->Instance::getDisabled())
      selectClient(wssIpv4Client, "wss-ipv4", "WSS IPv4");
    else if (!wssIpv6Client.getConfig()->Instance::getDisabled())
      selectClient(wssIpv6Client, "wss-ipv6", "WSS IPv6");
#endif
#endif
    publishConnectionSettings();
    if (connectBridge && connectSelected)
      connectSelected();
  });

  const int result = core::SNodeC::start();
  eventLoopRunning = false;
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
  webSocketBinding->shutdown();
#endif
  if (selectedFlow)
    static_cast<void>(selectedFlow->terminateFlow());
  connection.shutdown();
  static_cast<void>(workerLogic.sendWorkerStopped(
      result == 0 ? "SNode.C worker stopped" : "SNode.C worker failed"));
  return result;
}

} // namespace codexui::codex
