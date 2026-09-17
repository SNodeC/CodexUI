// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/CurrentProtocolAdapters.h"
#include "codex/nodegraph/ProtocolCatalog.h"

#include <ai/openai/codex/frontend/CodexBridge.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

namespace adapters = codexui::codex::current_protocol;
namespace clientRequests = adapters::client_requests;
namespace requests = adapters::server_requests;
namespace notifications = adapters::server_notifications;
namespace generated = ai::openai::codex::generated;
namespace nodegraph = codexui::nodegraph;

using Bridge = ai::openai::codex::frontend::CodexBridge;
using GeneratedValue = generated::Value;

#define CODEXUI_CAPTURE_CLIENT_REQUEST(Operation, accessor)                    \
  std::string_view(generated::client_requests::Operation::method),
constexpr std::array GeneratedClientRequests{
    AI_OPENAI_CODEX_CLIENT_REQUESTS(CODEXUI_CAPTURE_CLIENT_REQUEST)};
#undef CODEXUI_CAPTURE_CLIENT_REQUEST

#define CODEXUI_CAPTURE_SERVER_REQUEST(Operation, accessor)                    \
  std::string_view(generated::server_requests::Operation::method),
constexpr std::array GeneratedServerRequests{
    AI_OPENAI_CODEX_SERVER_REQUESTS(CODEXUI_CAPTURE_SERVER_REQUEST)};
#undef CODEXUI_CAPTURE_SERVER_REQUEST

#define CODEXUI_CAPTURE_CLIENT_NOTIFICATION(Operation, accessor)               \
  std::string_view(generated::client_notifications::Operation::method),
constexpr std::array GeneratedClientNotifications{
    AI_OPENAI_CODEX_CLIENT_NOTIFICATIONS(CODEXUI_CAPTURE_CLIENT_NOTIFICATION)};
#undef CODEXUI_CAPTURE_CLIENT_NOTIFICATION

#define CODEXUI_CAPTURE_SERVER_NOTIFICATION(Operation, accessor)               \
  std::string_view(generated::server_notifications::Operation::method),
constexpr std::array GeneratedServerNotifications{
    AI_OPENAI_CODEX_SERVER_NOTIFICATIONS(CODEXUI_CAPTURE_SERVER_NOTIFICATION)};
#undef CODEXUI_CAPTURE_SERVER_NOTIFICATION

constexpr std::array CompatibilityClientRequests{
    clientRequests::ThreadItemsList::method,
    clientRequests::ThreadTurnsList::method,
};
constexpr std::array CompatibilityServerRequests{
    requests::CurrentTimeRead::method,
};
constexpr std::array CompatibilityServerNotifications{
    notifications::ModelProviderAuthRecoveryStarted::method,
    notifications::ModelProviderAuthRecoveryCompleted::method,
    notifications::RawResponseItemCompleted::method,
    notifications::RawResponseCompleted::method,
    notifications::ThreadRealtimeItemStarted::method,
    notifications::ThreadRealtimeItemTranscriptDelta::method,
    notifications::ThreadRealtimeItemCompleted::method,
};

// The installed generated header is older than the verified app-server schema
// recorded for this migration.  Keep its client-request delta explicit so the
// catalog is checked by method name rather than only by a self-reported count.
constexpr std::array<std::string_view, 62> VerifiedNewerClientRequests{
    "account/bedrock/discover",
    "account/bedrock/setup",
    "collaborationMode/list",
    "environment/add",
    "environment/info",
    "environment/status",
    "fuzzyFileSearch/sessionStart",
    "fuzzyFileSearch/sessionStop",
    "fuzzyFileSearch/sessionUpdate",
    "getAuthStatus",
    "getConversationSummary",
    "gitDiffToRemote",
    "mcpServer/event/stream/start",
    "mcpServer/event/stream/stop",
    "memory/reset",
    "mock/experimentalMethod",
    "plugin/search",
    "process/kill",
    "process/resizePty",
    "process/spawn",
    "process/writeStdin",
    "project/create",
    "project/delete",
    "project/import",
    "project/list",
    "project/move",
    "project/read",
    "project/update",
    "remoteControl/client/list",
    "remoteControl/client/revoke",
    "remoteControl/disable",
    "remoteControl/enable",
    "remoteControl/pairing/start",
    "remoteControl/pairing/status",
    "remoteControl/status/read",
    "server/diagnostics",
    "thread/backgroundTerminals/clean",
    "thread/backgroundTerminals/list",
    "thread/backgroundTerminals/terminate",
    "thread/decrement_elicitation",
    "thread/increment_elicitation",
    "thread/items/list",
    "thread/memoryMode/set",
    "thread/queue/add",
    "thread/queue/delete",
    "thread/queue/list",
    "thread/queue/reorder",
    "thread/queue/start",
    "thread/queue/update",
    "thread/realtime/appendAudio",
    "thread/realtime/appendSpeech",
    "thread/realtime/appendText",
    "thread/realtime/listVoices",
    "thread/realtime/start",
    "thread/realtime/stop",
    "thread/revert",
    "thread/search",
    "thread/searchOccurrences",
    "thread/settings/update",
    "thread/timeline/list",
    "thread/turns/list",
    "turn/settings/update",
};

// These assertions deliberately couple this integration test to the installed
// generated schema.  An AISuite protocol update must therefore be reconciled
// with the explicit CodexUI compatibility surface and the graph catalog.
static_assert(GeneratedClientRequests.size() == 95);
static_assert(GeneratedServerRequests.size() == 10);
static_assert(GeneratedServerNotifications.size() == 76);
static_assert(GeneratedClientNotifications.size() == 1);
static_assert(CompatibilityClientRequests.size() == 2);
static_assert(CompatibilityServerRequests.size() == 1);
static_assert(CompatibilityServerNotifications.size() == 7);
static_assert(VerifiedNewerClientRequests.size() == 62);

template <typename Operation>
concept BridgeServerRequest =
    requires(Bridge &bridge, Bridge::EventHandler<Operation> handler,
             const typename Operation::Params &request,
             const typename Operation::Response &response) {
      bridge.template onServerRequest<Operation>(std::move(handler));
      {
        bridge.template respond<Operation>(request, response)
      } -> std::same_as<bool>;
    };

template <typename Operation>
concept BridgeClientRequest =
    requires(Bridge &bridge, Bridge::ResponseHandler<Operation> handler,
             const typename Operation::Params &params) {
      {
        bridge.template request<Operation>(params, std::move(handler))
      } -> std::same_as<std::string>;
    };

template <typename Operation>
concept BridgeServerNotification =
    requires(Bridge &bridge, Bridge::EventHandler<Operation> handler) {
      bridge.template onServerNotification<Operation>(std::move(handler));
    };

template <typename Operation>
concept RequiredValueParams =
    Operation::paramsRequired &&
    std::same_as<typename Operation::Params, GeneratedValue> &&
    std::constructible_from<typename Operation::Params, nlohmann::json>;

static_assert(BridgeServerRequest<requests::CurrentTimeRead>);
static_assert(RequiredValueParams<requests::CurrentTimeRead>);
static_assert(
    std::same_as<requests::CurrentTimeRead::Response, GeneratedValue>);

static_assert(BridgeClientRequest<clientRequests::ThreadItemsList>);
static_assert(RequiredValueParams<clientRequests::ThreadItemsList>);
static_assert(
    std::same_as<clientRequests::ThreadItemsList::Response, GeneratedValue>);

static_assert(BridgeClientRequest<clientRequests::ThreadTurnsList>);
static_assert(RequiredValueParams<clientRequests::ThreadTurnsList>);
static_assert(
    std::same_as<clientRequests::ThreadTurnsList::Response, GeneratedValue>);

static_assert(
    BridgeServerNotification<notifications::ModelProviderAuthRecoveryStarted>);
static_assert(BridgeServerNotification<
              notifications::ModelProviderAuthRecoveryCompleted>);
static_assert(
    BridgeServerNotification<notifications::RawResponseItemCompleted>);
static_assert(BridgeServerNotification<notifications::RawResponseCompleted>);
static_assert(
    BridgeServerNotification<notifications::ThreadRealtimeItemStarted>);
static_assert(
    BridgeServerNotification<notifications::ThreadRealtimeItemTranscriptDelta>);
static_assert(
    BridgeServerNotification<notifications::ThreadRealtimeItemCompleted>);

static_assert(
    RequiredValueParams<notifications::ModelProviderAuthRecoveryStarted>);
static_assert(
    RequiredValueParams<notifications::ModelProviderAuthRecoveryCompleted>);
static_assert(RequiredValueParams<notifications::RawResponseItemCompleted>);
static_assert(RequiredValueParams<notifications::RawResponseCompleted>);
static_assert(RequiredValueParams<notifications::ThreadRealtimeItemStarted>);
static_assert(
    RequiredValueParams<notifications::ThreadRealtimeItemTranscriptDelta>);
static_assert(RequiredValueParams<notifications::ThreadRealtimeItemCompleted>);

bool expect(bool condition, std::string_view message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

bool contains(std::span<const std::string_view> methods,
              std::string_view method) {
  return std::ranges::find(methods, method) != methods.end();
}

bool hasUniqueMethods(std::span<const std::string_view> methods) {
  for (std::size_t left = 0; left < methods.size(); ++left) {
    if (std::ranges::find(methods.subspan(left + 1), methods[left]) !=
        methods.end())
      return false;
  }
  return true;
}

bool hasDisjointMethods(
    std::span<const std::string_view> generatedMethods,
    std::span<const std::string_view> compatibilityMethods) {
  return std::ranges::none_of(generatedMethods,
                              [compatibilityMethods](std::string_view method) {
                                return contains(compatibilityMethods, method);
                              });
}

bool catalogContainsAll(std::span<const std::string_view> sourceMethods,
                        nodegraph::ProtocolDirection direction,
                        std::string_view sourceName) {
  for (const std::string_view method : sourceMethods) {
    if (!nodegraph::findProtocolMethod(direction, method)) {
      std::cerr << "Catalog is missing " << sourceName << " method " << method
                << '\n';
      return false;
    }
  }
  return true;
}

bool catalogDirectionEqualsUnion(
    nodegraph::ProtocolDirection direction,
    std::span<const std::string_view> generatedMethods,
    std::span<const std::string_view> compatibilityMethods) {
  if (nodegraph::protocolMethodCount(direction) !=
      generatedMethods.size() + compatibilityMethods.size())
    return false;

  return std::ranges::all_of(
      nodegraph::protocolMethods(),
      [direction, generatedMethods,
       compatibilityMethods](const nodegraph::MethodDescriptor &descriptor) {
        return descriptor.direction != direction ||
               contains(generatedMethods, descriptor.method) ||
               contains(compatibilityMethods, descriptor.method);
      });
}

bool testGeneratedSchemaCatalogCoverage() {
  using enum nodegraph::ProtocolDirection;

  bool passed = true;
  passed &= expect(hasUniqueMethods(GeneratedClientRequests) &&
                       hasUniqueMethods(GeneratedServerRequests) &&
                       hasUniqueMethods(GeneratedServerNotifications) &&
                       hasUniqueMethods(GeneratedClientNotifications),
                   "generated ProtocolTypes macros contain unique methods");
  passed &= expect(
      hasUniqueMethods(CompatibilityClientRequests) &&
          hasUniqueMethods(CompatibilityServerRequests) &&
          hasUniqueMethods(CompatibilityServerNotifications) &&
          hasUniqueMethods(VerifiedNewerClientRequests) &&
          hasDisjointMethods(GeneratedClientRequests,
                             VerifiedNewerClientRequests) &&
          hasDisjointMethods(GeneratedServerRequests,
                             CompatibilityServerRequests) &&
          hasDisjointMethods(GeneratedServerNotifications,
                             CompatibilityServerNotifications),
      "compatibility adapters are unique additions to generated ProtocolTypes");

  passed &=
      expect(std::ranges::all_of(CompatibilityClientRequests,
                                 [](std::string_view method) {
                                   return contains(VerifiedNewerClientRequests,
                                                   method);
                                 }),
             "typed client compatibility adapters belong to the verified "
             "schema delta");

  passed &= expect(
      catalogContainsAll(GeneratedClientRequests, ClientRequest,
                         "generated client request") &&
          catalogContainsAll(GeneratedServerRequests, ServerRequest,
                             "generated server request") &&
          catalogContainsAll(GeneratedServerNotifications, ServerNotification,
                             "generated server notification") &&
          catalogContainsAll(GeneratedClientNotifications, ClientNotification,
                             "generated client notification"),
      "catalog classifies every method in generated ProtocolTypes");
  passed &=
      expect(catalogContainsAll(VerifiedNewerClientRequests, ClientRequest,
                                "verified newer client request"),
             "catalog classifies every newer-schema client request");
  passed &=
      expect(catalogContainsAll(CompatibilityClientRequests, ClientRequest,
                                "compatibility client request") &&
                 catalogContainsAll(CompatibilityServerRequests, ServerRequest,
                                    "compatibility server request") &&
                 catalogContainsAll(CompatibilityServerNotifications,
                                    ServerNotification,
                                    "compatibility server notification"),
             "catalog classifies every explicit CodexUI compatibility adapter");

  passed &=
      expect(catalogDirectionEqualsUnion(ServerRequest, GeneratedServerRequests,
                                         CompatibilityServerRequests),
             "11 server requests exactly match generated types plus adapters");
  passed &= expect(
      catalogDirectionEqualsUnion(ClientRequest, GeneratedClientRequests,
                                  VerifiedNewerClientRequests),
      "157 client requests exactly match generated types plus verified delta");
  passed &= expect(
      catalogDirectionEqualsUnion(ServerNotification,
                                  GeneratedServerNotifications,
                                  CompatibilityServerNotifications),
      "83 server notifications exactly match generated types plus adapters");
  passed &=
      expect(catalogDirectionEqualsUnion(ClientNotification,
                                         GeneratedClientNotifications, {}),
             "one client notification exactly matches generated ProtocolTypes");

  return passed;
}

template <typename Operation>
bool notificationPayloadRoundTrips(nlohmann::json payload) {
  const nlohmann::json envelope{
      {"jsonrpc", "2.0"}, {"method", Operation::method}, {"params", payload}};
  const typename Operation::Params params(envelope);
  return params.jsonRpcMethod() == Operation::method &&
         params.jsonRpcId().is_null() && params.getPayload() == payload;
}

bool testExactMethods() {
  constexpr std::array methods{
      clientRequests::ThreadItemsList::method,
      clientRequests::ThreadTurnsList::method,
      requests::CurrentTimeRead::method,
      notifications::ModelProviderAuthRecoveryStarted::method,
      notifications::ModelProviderAuthRecoveryCompleted::method,
      notifications::RawResponseItemCompleted::method,
      notifications::RawResponseCompleted::method,
      notifications::ThreadRealtimeItemStarted::method,
      notifications::ThreadRealtimeItemTranscriptDelta::method,
      notifications::ThreadRealtimeItemCompleted::method,
  };
  constexpr std::array expected{
      std::string_view("thread/items/list"),
      std::string_view("thread/turns/list"),
      std::string_view("currentTime/read"),
      std::string_view("modelProvider/authRecoveryStarted"),
      std::string_view("modelProvider/authRecoveryCompleted"),
      std::string_view("rawResponseItem/completed"),
      std::string_view("rawResponse/completed"),
      std::string_view("thread/realtime/item/started"),
      std::string_view("thread/realtime/item/transcript/delta"),
      std::string_view("thread/realtime/item/completed"),
  };
  return expect(methods == expected,
                "adapter methods exactly match the current wire protocol");
}

bool testCurrentTimeRequestAndResponse() {
  const nlohmann::json payload{{"threadId", "thread-clock"}};
  const nlohmann::json envelope{{"jsonrpc", "2.0"},
                                {"id", "clock-request-7"},
                                {"method", requests::CurrentTimeRead::method},
                                {"params", payload}};
  const requests::CurrentTimeRead::Params request(envelope);
  const requests::CurrentTimeRead::Response response(
      nlohmann::json{{"currentTimeAt", 1'725'210'123}});

  bool passed = true;
  passed &=
      expect(request.jsonRpcMethod() == requests::CurrentTimeRead::method &&
                 request.jsonRpcId() == "clock-request-7" &&
                 request.getPayload() == payload,
             "current-time Params retain the request id and payload");
  passed &= expect(response.getPayload() ==
                       nlohmann::json{{"currentTimeAt", 1'725'210'123}},
                   "current-time Response exposes the exact result payload");
  return passed;
}

bool testNotificationPayloads() {
  bool passed = true;
  passed &= expect(notificationPayloadRoundTrips<
                       notifications::ModelProviderAuthRecoveryStarted>(
                       {{"provider", "openai"}, {"attempt", 2}}),
                   "auth-recovery-started Params retain their payload");
  passed &= expect(notificationPayloadRoundTrips<
                       notifications::ModelProviderAuthRecoveryCompleted>(
                       {{"provider", "openai"}, {"recovered", true}}),
                   "auth-recovery-completed Params retain their payload");
  passed &= expect(
      notificationPayloadRoundTrips<notifications::RawResponseItemCompleted>(
          {{"threadId", "thread-1"},
           {"turnId", "turn-1"},
           {"item", {{"type", "message"}, {"id", "response-item-1"}}}}),
      "raw-response-item Params retain nested payloads");
  passed &=
      expect(notificationPayloadRoundTrips<notifications::RawResponseCompleted>(
                 {{"threadId", "thread-1"},
                  {"turnId", "turn-1"},
                  {"responseId", "response-1"},
                  {"usage", {{"inputTokens", 12}, {"outputTokens", 4}}}}),
             "raw-response Params retain nested payloads");
  passed &= expect(
      notificationPayloadRoundTrips<notifications::ThreadRealtimeItemStarted>(
          {{"threadId", "thread-rt"}, {"itemId", "item-rt"}}),
      "realtime-item-started Params retain their payload");
  passed &=
      expect(notificationPayloadRoundTrips<
                 notifications::ThreadRealtimeItemTranscriptDelta>(
                 {{"threadId", "thread-rt"},
                  {"itemId", "item-rt"},
                  {"delta", "hello"}}),
             "realtime-item-transcript-delta Params retain their payload");
  passed &= expect(
      notificationPayloadRoundTrips<notifications::ThreadRealtimeItemCompleted>(
          {{"threadId", "thread-rt"}, {"itemId", "item-rt"}}),
      "realtime-item-completed Params retain their payload");
  return passed;
}

bool testCodexBridgeDispatchesCompatibilityOperations() {
  nlohmann::json sent;
  Bridge bridge([&sent](const nlohmann::json &message) {
    sent = message;
    return true;
  });
  bool currentTimeHandled = false;
  std::size_t notificationCount = 0;
  bridge.onServerRequest<requests::CurrentTimeRead>(
      [&](requests::CurrentTimeRead::Params &request) {
        currentTimeHandled = request.jsonRpcId() == "clock-bridge" &&
                             request.getPayload().value(
                                 "threadId", std::string{}) == "thread-bridge";
        const requests::CurrentTimeRead::Response response(
            nlohmann::json{{"currentTimeAt", 1'725'210'123}});
        static_cast<void>(
            bridge.respond<requests::CurrentTimeRead>(request, response));
      });
#define CODEXUI_TEST_REGISTER_NOTIFICATION(OperationName)                      \
  bridge.onServerNotification<notifications::OperationName>(                   \
      [&notificationCount](notifications::OperationName::Params &) {           \
        ++notificationCount;                                                   \
      });
  CODEXUI_TEST_REGISTER_NOTIFICATION(ModelProviderAuthRecoveryStarted)
  CODEXUI_TEST_REGISTER_NOTIFICATION(ModelProviderAuthRecoveryCompleted)
  CODEXUI_TEST_REGISTER_NOTIFICATION(RawResponseItemCompleted)
  CODEXUI_TEST_REGISTER_NOTIFICATION(RawResponseCompleted)
  CODEXUI_TEST_REGISTER_NOTIFICATION(ThreadRealtimeItemStarted)
  CODEXUI_TEST_REGISTER_NOTIFICATION(ThreadRealtimeItemTranscriptDelta)
  CODEXUI_TEST_REGISTER_NOTIFICATION(ThreadRealtimeItemCompleted)
#undef CODEXUI_TEST_REGISTER_NOTIFICATION

  bool accepted = bridge.receive({{"kind", "bridge.connection"},
                                  {"event", "opened"},
                                  {"connectionId", "bridge-test"},
                                  {"role", "controller"}});
  accepted &= bridge.receive({{"kind", "appserver"},
                              {"payload",
                               {{"jsonrpc", "2.0"},
                                {"id", "clock-bridge"},
                                {"method", requests::CurrentTimeRead::method},
                                {"params", {{"threadId", "thread-bridge"}}}}}});

  constexpr std::array notificationMethods{
      notifications::ModelProviderAuthRecoveryStarted::method,
      notifications::ModelProviderAuthRecoveryCompleted::method,
      notifications::RawResponseItemCompleted::method,
      notifications::RawResponseCompleted::method,
      notifications::ThreadRealtimeItemStarted::method,
      notifications::ThreadRealtimeItemTranscriptDelta::method,
      notifications::ThreadRealtimeItemCompleted::method,
  };
  for (const std::string_view method : notificationMethods) {
    accepted &= bridge.receive({{"kind", "appserver"},
                                {"payload",
                                 {{"jsonrpc", "2.0"},
                                  {"method", method},
                                  {"params", {{"marker", method}}}}}});
  }

  const nlohmann::json response =
      sent.value("payload", nlohmann::json::object());
  return expect(accepted && currentTimeHandled && notificationCount == 7,
                "CodexBridge dispatches every compatibility operation") &&
         expect(response.value("id", std::string{}) == "clock-bridge" &&
                    response.value("result", nlohmann::json::object())
                            .value("currentTimeAt", std::int64_t{}) ==
                        1'725'210'123,
                "CodexBridge emits the typed current-time response");
}

} // namespace

int main() {
  bool passed = true;
  passed &= testGeneratedSchemaCatalogCoverage();
  passed &= testExactMethods();
  passed &= testCurrentTimeRequestAndResponse();
  passed &= testNotificationPayloads();
  passed &= testCodexBridgeDispatchesCompatibilityOperations();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
