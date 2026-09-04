// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ClientRuntime.h"

#include "codex/Configuration.h"
#include "codex/CurrentProtocolAdapters.h"
#include "codex/NodeGraphJson.h"
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
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
  if (method == "thread/read" || method == "thread/resume")
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

const nodegraph::Value *valueMember(const nodegraph::Value::Object &object,
                                    std::string_view key) {
  const auto found = object.find(key);
  return found == object.end() ? nullptr : &found->second;
}

std::string valueString(const nodegraph::Value::Object &object,
                        std::string_view key) {
  const nodegraph::Value *value = valueMember(object, key);
  const std::string *string = value ? value->asString() : nullptr;
  return string ? *string : std::string{};
}

std::optional<std::uint64_t>
valueUnsigned(const nodegraph::Value::Object &object, std::string_view key) {
  const nodegraph::Value *value = valueMember(object, key);
  if (const std::uint64_t *number = value ? value->asUInt64() : nullptr)
    return *number;
  if (const std::int64_t *number = value ? value->asInt64() : nullptr;
      number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return std::nullopt;
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
};

void identifyResultEntities(RequestOutcome &outcome) {
  outcome.threadId = valueString(outcome.payload, "threadId");
  outcome.turnId = valueString(outcome.payload, "turnId");
  if (const nodegraph::Value *thread = valueMember(outcome.payload, "thread")) {
    if (const nodegraph::Value::Object *object = thread->asObject())
      if (outcome.threadId.empty())
        outcome.threadId = valueString(*object, "id");
  }
  if (const nodegraph::Value *turn = valueMember(outcome.payload, "turn")) {
    if (const nodegraph::Value::Object *object = turn->asObject())
      if (outcome.turnId.empty())
        outcome.turnId = valueString(*object, "id");
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
        threadActivityAt(Operation::method)};
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
                               ok ? std::string{} : resultError(raw)};
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
      threadActivityAt(Operation::method)};
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
        static_cast<void>(workerLogic.applyDetailed(std::move(decoded)));
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
  std::unordered_set<nodegraph::NodeRef> pendingHistoryLoads;
  std::unordered_set<nodegraph::NodeRef> resumedPromptAdmissions;
  std::unordered_map<nodegraph::NodeRef, nodegraph::PromptCommand>
      promptsWaitingForHydration;
  bool threadListPending = false;
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
    pendingHistoryLoads.clear();
    resumedPromptAdmissions.clear();
    promptsWaitingForHydration.clear();
    threadListPending = false;
    modelListPending = false;
    permissionProfilesPending = false;
  };

  const auto showNotice = [&workerLogic](std::string message) {
    static_cast<void>(workerLogic.showNotice(std::move(message)));
  };

  codex::frontend::CodexBridge sdk({});

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
              [&clearTransientState, &workerLogic] {
                clearTransientState();
                static_cast<void>(workerLogic.transportEvent("connected"));
              },
          .onDisconnected =
              [&clearTransientState, &expectedDisconnectReason,
               &desiredConnected, &workerLogic] {
                clearTransientState();
                std::string reason =
                    std::exchange(expectedDisconnectReason, {});
                static_cast<void>(workerLogic.transportEvent(
                    desiredConnected ? "retrying" : "disconnected", reason));
              },
          .onFailure =
              [&clearTransientState, &workerLogic](std::string reason) {
                clearTransientState();
                static_cast<void>(
                    workerLogic.transportEvent("failure", std::move(reason)));
              }});

  sdk.onRawJson([&workerLogic](codex::protocol::AppServerDirection direction,
                               const nlohmann::json &message) {
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
          {}}));
      return;
    }

    if (direction != codex::protocol::AppServerDirection::FromAppServer)
      return;

    const std::optional<nodegraph::ProtocolRequestId> requestId =
        requestIdMember(message, "id");
    const nodegraph::DecodedMessageKind kind =
        requestId ? nodegraph::DecodedMessageKind::ServerRequest
                  : nodegraph::DecodedMessageKind::ServerNotification;
    const nodegraph::ProtocolDirection catalogDirection =
        requestId ? nodegraph::ProtocolDirection::ServerRequest
                  : nodegraph::ProtocolDirection::ServerNotification;
    if (nodegraph::findProtocolMethod(catalogDirection, *method))
      return; // The registered typed callback is the sole known-message path.

    const auto parameters = message.find("params");
    const nlohmann::json payload =
        parameters == message.end() ? nlohmann::json::object() : *parameters;
    static_cast<void>(workerLogic.applyDetailed(nodegraph::DecodedMessage{
        kind, *method, requestId, decodedObject(payload), {}}));
  });
  sdk.onBridgeEvent([&clearTransientState, &hydrateProvider, &workerLogic,
                     &sdk](const nlohmann::json &message) {
    const std::uint64_t before = workerLogic.generations().provider;
    const std::string kind = jsonString(message, "kind");
    applyBridgeState(workerLogic, sdk, message);
    const auto after = workerLogic.generations();
    if (after.provider != before)
      clearTransientState();
    if (kind == "bridge.provider" && sdk.providerReady() && hydrateProvider)
      hydrateProvider();
  });

  const auto registerServerRequest = [&]<typename Operation>() {
    sdk.onServerRequest<Operation>([&pendingServerRequests, &sdk, &workerLogic](
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
              threadActivityAt(Operation::method)});
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
            codex::generated::server_notifications::OperationName::method)})); \
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

  using CurrentTimeRead = current_protocol::server_requests::CurrentTimeRead;
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
                                  threadActivityAt(CurrentTimeRead::method)});
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
      static_cast<void>(
          workerLogic.resolveInteraction(applied.primary, true, {}));
    if (!accepted)
      showNotice("Current-time server response was rejected");
  });

#define CODEXUI_REGISTER_CURRENT_NOTIFICATION(OperationName)                   \
  sdk.onServerNotification<                                                    \
      current_protocol::server_notifications::OperationName>(                  \
      [&workerLogic](                                                          \
          current_protocol::server_notifications::OperationName::Params        \
              &notification) {                                                 \
        static_cast<void>(workerLogic.applyDetailed(nodegraph::DecodedMessage{ \
            nodegraph::DecodedMessageKind::ServerNotification,                 \
            std::string(current_protocol::server_notifications::               \
                            OperationName::method),                            \
            std::nullopt,                                                      \
            decodedObject(notification.getPayload()),                          \
            {},                                                                \
            threadActivityAt(current_protocol::server_notifications::          \
                                 OperationName::method)}));                    \
      });
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ModelProviderAuthRecoveryStarted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ModelProviderAuthRecoveryCompleted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(RawResponseItemCompleted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(RawResponseCompleted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ThreadRealtimeItemStarted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ThreadRealtimeItemTranscriptDelta)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ThreadRealtimeItemCompleted)
#undef CODEXUI_REGISTER_CURRENT_NOTIFICATION

  const auto publishTransportEvent = [&clearTransientState,
                                      &workerLogic](std::string state,
                                                    std::string detail = {}) {
    if (state == "retrying" || state == "disconnected" || state == "failure")
      clearTransientState();
    static_cast<void>(
        workerLogic.transportEvent(std::move(state), std::move(detail)));
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
  std::function<void()> terminateSelected;
  std::function<bool()> selectedFlowTerminated;
  std::function<void()> disableSelected;
  std::string selectedTransport;
  std::string selectedTransportLabel;
  bool transitionPending = false;
  bool shutdownRequested = false;
  bool eventLoopRunning = false;
  std::function<void()> continueTransition;
  std::function<bool()> terminatingFlowTerminated;
  std::function<void()> pendingSelection;
  std::chrono::steady_clock::time_point transitionDeadline;

  const auto selectClient = [&](auto &configuredClient, std::string transport,
                                std::string label) {
    if (disableSelected)
      disableSelected();
    configuredClient.getConfig()->Instance::setDisabled(false);
    auto *const clientHandle = &configuredClient;
    auto *const flow = configuredClient.getFlowController();
    auto *const config = configuredClient.getConfig();
    selectedTransport = std::move(transport);
    selectedTransportLabel = std::move(label);
    const std::string connectionLabel = selectedTransportLabel;
    connectSelected = [&, clientHandle, flow, connectionLabel] {
      publishTransportEvent("retrying", "Connecting using " + connectionLabel);
      clientHandle->connect([&, flow, connectionLabel](
                                const auto &, core::socket::State state) {
        if (state == core::socket::State::OK ||
            state == core::socket::State::DISABLED)
          return;
        const std::string failure =
            "failed to connect using " + connectionLabel + ": " + state.what();
        core::EventReceiver::atNextTick([&, flow, failure] {
          if (eventLoopRunning && !shutdownRequested && flow->isTerminated())
            publishTransportEvent("failure", failure);
        });
      });
    };
    terminateSelected = [flow] { static_cast<void>(flow->terminateFlow()); };
    selectedFlowTerminated = [flow] { return flow->isTerminated(); };
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
    if ((terminatingFlowTerminated && !terminatingFlowTerminated()) ||
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
    terminatingFlowTerminated = {};
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
    terminatingFlowTerminated = selectedFlowTerminated;
    if (!terminateSelected ||
        ((!terminatingFlowTerminated || terminatingFlowTerminated()) &&
         !connection.attached())) {
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
    terminateSelected();
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
    if (!selectedFlowTerminated || selectedFlowTerminated()) {
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
    if (terminateSelected)
      terminateSelected();
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

  const auto requestThreadList = [&](nlohmann::json parameters) {
    if (threadListPending)
      return;
    threadListPending = true;
    dispatchRequest<codex::generated::client_requests::ThreadList>(
        sdk, std::move(parameters), workerLogic, {},
        [&threadListPending, &showNotice](RequestOutcome outcome) {
          threadListPending = false;
          if (!outcome.ok)
            showNotice(outcome.error);
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
          if (!outcome.ok)
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
          if (!outcome.ok)
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
            static_cast<void>(workerLogic.completeCreatedThread(
                std::move(decoded), *pending, threadId));
            if (pending->kind == PromptCommandKind::CreateThread ||
                !pending->thread) {
              failPromptAndContinue(
                  pending->localPrompt,
                  "Could not attach the prompt to the created thread");
              return;
            }
            if (!requestedName.empty()) {
              dispatchRequest<codex::generated::client_requests::ThreadSetName>(
                  sdk,
                  nlohmann::json{{"threadId", threadId},
                                 {"name", requestedName}},
                  workerLogic, pending->thread,
                  [&showNotice](RequestOutcome renameOutcome) {
                    if (!renameOutcome.ok)
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
        threadState ? valueString(threadState->fields, "hydrationState")
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
        threadState &&
        (threadState->status == nodegraph::NodeStatus::NotLoaded ||
         valueString(threadState->fields, "status") == "notLoaded");
    const bool explicitlyResumed =
        resumedPromptAdmissions.erase(command.localPrompt) != 0;
    if (needsResume && !explicitlyResumed) {
      auto pending =
          std::make_shared<nodegraph::PromptCommand>(std::move(command));
      dispatchRequest<codex::generated::client_requests::ThreadResume>(
          sdk, nlohmann::json{{"threadId", pending->thread->id().canonical}},
          workerLogic, pending->thread,
          [&, pending](RequestOutcome outcome) mutable {
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
    return state && valueString(state->fields, "hydrationState") == "ready" &&
           valueUnsigned(state->fields, "hydrationConnectionGeneration") ==
               std::optional<std::uint64_t>(
                   workerLogic.generations().connection);
  };

  constexpr std::size_t MaximumHistoricalHydrations = 8;
  std::function<void()> pumpHistoricalHydrations;
  std::function<void(const nodegraph::NodeRef &, bool, bool)>
      startThreadHydration;
  std::function<void(const nodegraph::NodeRef &)> queueActiveAgentChildren;

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
    if (!force && hydrationReady(thread))
      return;
    if (interactive)
      interactiveHydrations.insert(thread);
    if (!pendingHydrations.insert(thread).second)
      return;
    static_cast<void>(workerLogic.threadHydration(thread, "loading"));

    dispatchRequestHandled<codex::generated::client_requests::ThreadRead>(
        sdk, nlohmann::json{{"threadId", *threadId}, {"includeTurns", true}},
        workerLogic, thread, [](const nodegraph::ProtocolRequestId &) {},
        [&, thread, id = *threadId](RequestOutcome outcome,
                                    nodegraph::DecodedMessage decoded) {
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
          if (!needsSettings || !sdk.isController()) {
            continuePromptAfterHydration(thread, {});
            if (wasHistorical && pumpHistoricalHydrations)
              pumpHistoricalHydrations();
            return;
          }
          dispatchRequest<codex::generated::client_requests::ThreadResume>(
              sdk, nlohmann::json{{"threadId", id}, {"excludeTurns", true}},
              workerLogic, thread,
              [&, thread](RequestOutcome resumeOutcome) mutable {
                if (!resumeOutcome.ok)
                  showNotice(resumeOutcome.error.empty()
                                 ? "Thread settings refresh failed"
                                 : resumeOutcome.error);
                // A successful thread/read is hydrated even when the
                // controller-only settings resume fails. A later prompt to a
                // provider-notLoaded thread performs its own bounded resume.
                continuePromptAfterHydration(thread, {}, resumeOutcome.ok);
              });
          if (wasHistorical && pumpHistoricalHydrations)
            pumpHistoricalHydrations();
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
    const std::optional<std::string> threadId = currentThreadId(action.target);
    if (!threadId) {
      showNotice("The selected thread is no longer available");
      return;
    }
    if (!pendingHistoryLoads.insert(action.target).second)
      return;
    nlohmann::json parameters = jsonObject(std::move(action.payload));
    parameters["threadId"] = *threadId;
    if (!parameters.contains("cursor")) {
      if (const auto state = currentNodeState(action.target)) {
        const std::string cursor =
            valueString(state->fields, "historyNextCursor");
        if (!cursor.empty())
          parameters["cursor"] = cursor;
      }
    }
    if (!parameters.contains("limit"))
      parameters["limit"] = 80;
    if (!parameters.contains("sortDirection"))
      parameters["sortDirection"] = "desc";
    if (!parameters.contains("itemsView"))
      parameters["itemsView"] = "full";
    const nodegraph::NodeRef thread = std::move(action.target);
    dispatchRequest<current_protocol::client_requests::ThreadTurnsList>(
        sdk, std::move(parameters), workerLogic, thread,
        [&, thread](RequestOutcome outcome) {
          pendingHistoryLoads.erase(thread);
          if (!outcome.ok)
            showNotice(outcome.error);
        });
  };

  const auto resolveInteraction = [&](nodegraph::NodeAction action) {
    auto reject = [&](std::string message) {
      static_cast<void>(workerLogic.rejectInteractionResponse(
          action.target, std::move(action.payload), message));
      showNotice(std::move(message));
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
    if (!state ||
        valueString(state->fields, "method") != found->second.method) {
      reject("The pending request is no longer actionable");
      return;
    }
    // A bridge sender may synchronously feed serverRequest/resolved back into
    // CodexBridge. Visit a local typed copy so that such reentrancy can erase
    // the map entry without invalidating the request currently being encoded.
    PendingServerRequest pending = found->second;

    std::string decision = valueString(action.payload, "decision");
    if (decision.empty())
      decision = valueString(action.payload, "action");
    const nodegraph::Value *authoredDecision =
        valueMember(action.payload, "decision");
    const std::string scope = valueString(action.payload, "scope");
    const nodegraph::Value *answers = valueMember(action.payload, "answers");
    const nodegraph::Value *content = valueMember(action.payload, "content");
    const nodegraph::Value *meta = valueMember(action.payload, "_meta");
    bool accepted = false;

    std::visit(
        [&](auto &request) {
          using Request = std::decay_t<decltype(request)>;
          if constexpr (std::is_same_v<
                            Request, CommandExecutionRequestApproval::Params>) {
            CommandExecutionRequestApproval::Response response(
                nlohmann::json{{"decision", decision}});
            accepted =
                sdk.respond<CommandExecutionRequestApproval>(request, response);
          } else if constexpr (std::is_same_v<
                                   Request,
                                   FileChangeRequestApproval::Params>) {
            FileChangeRequestApproval::Response response(
                nlohmann::json{{"decision", decision}});
            accepted =
                sdk.respond<FileChangeRequestApproval>(request, response);
          } else if constexpr (std::is_same_v<Request,
                                              ToolRequestUserInput::Params>) {
            ToolRequestUserInput::Response response(nlohmann::json{
                {"answers", answers ? jsonFromValue(*answers)
                                    : nlohmann::json::object()}});
            accepted = sdk.respond<ToolRequestUserInput>(request, response);
          } else if constexpr (std::is_same_v<
                                   Request,
                                   McpServerElicitationRequest::Params>) {
            const bool acceptsContent = decision == "accept";
            McpServerElicitationRequest::Response response(
                nlohmann::json{{"action", decision},
                               {"content", acceptsContent && content
                                               ? jsonFromValue(*content)
                                               : nlohmann::json(nullptr)},
                               {"_meta", meta ? jsonFromValue(*meta)
                                              : nlohmann::json(nullptr)}});
            accepted =
                sdk.respond<McpServerElicitationRequest>(request, response);
          } else if constexpr (std::is_same_v<
                                   Request,
                                   PermissionsRequestApproval::Params>) {
            if (scope.empty() || scope == "decline" || decision == "decline") {
              accepted = sdk.respondError<PermissionsRequestApproval>(
                  request, -32601, "Permission request declined by user");
            } else {
              nlohmann::json responsePayload{
                  {"permissions",
                   request.getPayload().is_object() &&
                           request.getPayload().contains("permissions")
                       ? request.getPayload()["permissions"]
                       : nlohmann::json::object()},
                  {"scope", scope}};
              PermissionsRequestApproval::Response response(
                  std::move(responsePayload));
              accepted =
                  sdk.respond<PermissionsRequestApproval>(request, response);
            }
          } else if constexpr (std::is_same_v<Request,
                                              DynamicToolCall::Params>) {
            const nodegraph::Value *contentItems =
                valueMember(action.payload, "contentItems");
            const nodegraph::Value *success =
                valueMember(action.payload, "success");
            const std::string message =
                valueString(action.payload, "message").empty()
                    ? "CodexUI does not provide this dynamic tool"
                    : valueString(action.payload, "message");
            DynamicToolCall::Response response(nlohmann::json{
                {"contentItems",
                 contentItems ? jsonFromValue(*contentItems)
                              : nlohmann::json::array({{{"type", "inputText"},
                                                        {"text", message}}})},
                {"success",
                 success && success->asBool() ? *success->asBool() : false}});
            accepted = sdk.respond<DynamicToolCall>(request, response);
          } else if constexpr (std::is_same_v<
                                   Request, ChatgptAuthTokensRefresh::Params>) {
            accepted = sdk.respondError<ChatgptAuthTokensRefresh>(
                request, -32601,
                "CodexUI does not support authentication token refresh");
          } else if constexpr (std::is_same_v<Request,
                                              AttestationGenerate::Params>) {
            accepted = sdk.respondError<AttestationGenerate>(
                request, -32601,
                "CodexUI does not support attestation generation");
          } else if constexpr (std::is_same_v<Request,
                                              ApplyPatchApproval::Params>) {
            nlohmann::json reviewDecision = "abort";
            if (authoredDecision && authoredDecision->isObject())
              reviewDecision = jsonFromValue(*authoredDecision);
            else if (decision == "accept" || decision == "approved")
              reviewDecision = "approved";
            else if (decision == "acceptForSession" ||
                     decision == "approved_for_session")
              reviewDecision = "approved_for_session";
            else if (decision == "decline" || decision == "denied")
              reviewDecision =
                  nlohmann::json{{"denied", {{"rejection", "Denied by user"}}}};
            ApplyPatchApproval::Response response(
                nlohmann::json{{"decision", std::move(reviewDecision)}});
            accepted = sdk.respond<ApplyPatchApproval>(request, response);
          } else if constexpr (std::is_same_v<Request,
                                              ExecCommandApproval::Params>) {
            nlohmann::json reviewDecision = "abort";
            if (authoredDecision && authoredDecision->isObject())
              reviewDecision = jsonFromValue(*authoredDecision);
            else if (decision == "accept" || decision == "approved")
              reviewDecision = "approved";
            else if (decision == "acceptForSession" ||
                     decision == "approved_for_session")
              reviewDecision = "approved_for_session";
            else if (decision == "decline" || decision == "denied")
              reviewDecision =
                  nlohmann::json{{"denied", {{"rejection", "Denied by user"}}}};
            ExecCommandApproval::Response response(
                nlohmann::json{{"decision", std::move(reviewDecision)}});
            accepted = sdk.respond<ExecCommandApproval>(request, response);
          }
        },
        pending.request);

    if (accepted) {
      static_cast<void>(
          workerLogic.resolveInteraction(action.target, true, {}));
      pendingServerRequests.erase(action.target);
    } else {
      static_cast<void>(workerLogic.rejectInteractionResponse(
          action.target, std::move(action.payload),
          "CodexBridge rejected the server-request response"));
      showNotice("The pending response could not be sent");
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
        showNotice("Controller access is unavailable for this thread action");
        return;
      }
      if (!action.target ||
          action.target->id().kind != nodegraph::NodeKind::Thread) {
        showNotice("The selected thread is no longer available");
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
        showNotice("The selected thread is no longer available");
        return;
      }
      const bool archived = stateFlag("archived");
      if ((action.kind == Archive && archived) ||
          (action.kind == Unarchive && !archived)) {
        showNotice("The thread action is no longer applicable");
        return;
      }
      const std::string threadId =
          nodegraph::protocolCanonicalId(*threadState, action.target);
      if (threadId.empty()) {
        showNotice("The selected thread is no longer available");
        return;
      }
      if (action.kind == Rename) {
        const nodegraph::Value *nameValue = valueMember(action.payload, "name");
        const std::string *name = nameValue ? nameValue->asString() : nullptr;
        if (!name ||
            name->find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
          showNotice("A non-empty thread name is required");
          return;
        }
      }
      nlohmann::json parameters = jsonObject(std::move(action.payload));
      parameters["threadId"] = threadId;
      const nodegraph::NodeRef target = std::move(action.target);
      const auto completed = [&showNotice](RequestOutcome outcome) {
        if (!outcome.ok)
          showNotice(outcome.error);
      };
      if (action.kind == Rename)
        dispatchRequest<codex::generated::client_requests::ThreadSetName>(
            sdk, std::move(parameters), workerLogic, target, completed);
      else if (action.kind == Fork)
        dispatchRequest<codex::generated::client_requests::ThreadFork>(
            sdk, std::move(parameters), workerLogic, target,
            [&](RequestOutcome outcome) {
              if (!outcome.ok) {
                showNotice(outcome.error);
                return;
              }
              std::string forkId = std::move(outcome.threadId);
              if (forkId.empty())
                return;
              nodegraph::NodeRef fork =
                  currentNode({nodegraph::NodeKind::Thread, forkId});
              if (!fork)
                return;
              static_cast<void>(workerLogic.selectThread(fork));
              hydrateThread(fork);
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
        showNotice("No controlled active turn is available to stop");
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
        showNotice("No controlled active turn is available to stop");
        return;
      }
      nlohmann::json parameters = jsonObject(std::move(action.payload));
      parameters["threadId"] = currentActiveTurn->first;
      parameters["turnId"] = currentActiveTurn->second;
      dispatchRequest<codex::generated::client_requests::TurnInterrupt>(
          sdk, std::move(parameters), workerLogic, action.target,
          [&showNotice](RequestOutcome outcome) {
            if (!outcome.ok)
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
      showNotice("A connection transition is already in progress");
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
      showNotice("Invalid connection settings");
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
        showNotice("Controller claim was rejected");
      return;
    case ReleaseController:
      if (!sdk.releaseController())
        showNotice("Controller release was rejected");
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
  if (terminateSelected)
    terminateSelected();
  connection.shutdown();
  static_cast<void>(workerLogic.sendWorkerStopped(
      result == 0 ? "SNode.C worker stopped" : "SNode.C worker failed"));
  return result;
}

} // namespace codexui::codex
