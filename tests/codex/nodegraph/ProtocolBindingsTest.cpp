// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/InternalProtocolOperations.h"
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

namespace generated = ai::openai::codex::generated;
namespace clientRequests = generated::client_requests;
namespace requests = generated::server_requests;
namespace notifications = generated::server_notifications;
namespace internalNotifications =
    codexui::codex::internal_protocol::server_notifications;
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

constexpr std::array LegacyClientRequests{
    std::string_view("getAuthStatus"),
    std::string_view("getConversationSummary"),
    std::string_view("gitDiffToRemote"),
};

constexpr std::array InternalServerNotifications{
    internalNotifications::RawResponseItemCompleted::method,
    internalNotifications::RawResponseCompleted::method,
};

// These assertions deliberately couple this integration test to the installed
// generated schema.  An AISuite protocol update must therefore be reconciled
// with the explicit CodexUI compatibility surface and the graph catalog.
static_assert(GeneratedClientRequests.size() == 159);
static_assert(GeneratedServerRequests.size() == 11);
static_assert(GeneratedServerNotifications.size() == 81);
static_assert(GeneratedClientNotifications.size() == 1);
static_assert(LegacyClientRequests.size() == 3);
static_assert(InternalServerNotifications.size() == 2);

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
concept RequiredTypedParams =
    Operation::paramsRequired &&
    std::derived_from<typename Operation::Params, GeneratedValue> &&
    std::constructible_from<typename Operation::Params, nlohmann::json>;

static_assert(BridgeServerRequest<requests::CurrentTimeRead>);
static_assert(RequiredTypedParams<requests::CurrentTimeRead>);
static_assert(
    std::derived_from<requests::CurrentTimeRead::Response, GeneratedValue>);

static_assert(BridgeClientRequest<clientRequests::ThreadItemsList>);
static_assert(RequiredTypedParams<clientRequests::ThreadItemsList>);
static_assert(std::derived_from<clientRequests::ThreadItemsList::Response,
                                GeneratedValue>);

static_assert(BridgeClientRequest<clientRequests::ThreadTurnsList>);
static_assert(RequiredTypedParams<clientRequests::ThreadTurnsList>);
static_assert(std::derived_from<clientRequests::ThreadTurnsList::Response,
                                GeneratedValue>);

static_assert(
    BridgeServerNotification<notifications::ModelProviderAuthRecoveryStarted>);
static_assert(BridgeServerNotification<
              notifications::ModelProviderAuthRecoveryCompleted>);
static_assert(
    BridgeServerNotification<internalNotifications::RawResponseItemCompleted>);
static_assert(
    BridgeServerNotification<internalNotifications::RawResponseCompleted>);
static_assert(
    BridgeServerNotification<notifications::ThreadRealtimeItemStarted>);
static_assert(
    BridgeServerNotification<notifications::ThreadRealtimeItemTranscriptDelta>);
static_assert(
    BridgeServerNotification<notifications::ThreadRealtimeItemCompleted>);

static_assert(
    RequiredTypedParams<notifications::ModelProviderAuthRecoveryStarted>);
static_assert(
    RequiredTypedParams<notifications::ModelProviderAuthRecoveryCompleted>);
static_assert(
    RequiredTypedParams<internalNotifications::RawResponseItemCompleted>);
static_assert(RequiredTypedParams<internalNotifications::RawResponseCompleted>);
static_assert(RequiredTypedParams<notifications::ThreadRealtimeItemStarted>);
static_assert(
    RequiredTypedParams<notifications::ThreadRealtimeItemTranscriptDelta>);
static_assert(RequiredTypedParams<notifications::ThreadRealtimeItemCompleted>);

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
      hasUniqueMethods(LegacyClientRequests) &&
          hasUniqueMethods(InternalServerNotifications) &&
          hasDisjointMethods(GeneratedClientRequests, LegacyClientRequests) &&
          hasDisjointMethods(GeneratedServerNotifications,
                             InternalServerNotifications),
      "legacy and internal methods are unique additions to generated types");

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
  passed &= expect(catalogContainsAll(LegacyClientRequests, ClientRequest,
                                      "legacy client request") &&
                       catalogContainsAll(InternalServerNotifications,
                                          ServerNotification,
                                          "internal server notification"),
                   "catalog classifies every explicit compatibility method");

  passed &= expect(
      catalogDirectionEqualsUnion(ServerRequest, GeneratedServerRequests, {}),
      "11 server requests exactly match generated types");
  passed &=
      expect(catalogDirectionEqualsUnion(ClientRequest, GeneratedClientRequests,
                                         LegacyClientRequests),
             "162 client requests exactly match generated plus legacy methods");
  passed &= expect(
      catalogDirectionEqualsUnion(ServerNotification,
                                  GeneratedServerNotifications,
                                  InternalServerNotifications),
      "83 server notifications exactly match generated plus internal methods");
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
      internalNotifications::RawResponseItemCompleted::method,
      internalNotifications::RawResponseCompleted::method,
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
                "generated and internal methods match the wire protocol");
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
  passed &=
      expect(notificationPayloadRoundTrips<
                 internalNotifications::RawResponseItemCompleted>(
                 {{"threadId", "thread-1"},
                  {"turnId", "turn-1"},
                  {"item", {{"type", "message"}, {"id", "response-item-1"}}}}),
             "raw-response-item Params retain nested payloads");
  passed &= expect(notificationPayloadRoundTrips<
                       internalNotifications::RawResponseCompleted>(
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

bool testCodexBridgeDispatchesGeneratedAndInternalOperations() {
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
  CODEXUI_TEST_REGISTER_NOTIFICATION(ThreadRealtimeItemStarted)
  CODEXUI_TEST_REGISTER_NOTIFICATION(ThreadRealtimeItemTranscriptDelta)
  CODEXUI_TEST_REGISTER_NOTIFICATION(ThreadRealtimeItemCompleted)
#undef CODEXUI_TEST_REGISTER_NOTIFICATION

#define CODEXUI_TEST_REGISTER_INTERNAL_NOTIFICATION(OperationName)             \
  bridge.onServerNotification<internalNotifications::OperationName>(           \
      [&notificationCount](internalNotifications::OperationName::Params &) {   \
        ++notificationCount;                                                   \
      });
  CODEXUI_TEST_REGISTER_INTERNAL_NOTIFICATION(RawResponseItemCompleted)
  CODEXUI_TEST_REGISTER_INTERNAL_NOTIFICATION(RawResponseCompleted)
#undef CODEXUI_TEST_REGISTER_INTERNAL_NOTIFICATION

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
      internalNotifications::RawResponseItemCompleted::method,
      internalNotifications::RawResponseCompleted::method,
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
                "CodexBridge dispatches generated and internal operations") &&
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
  passed &= testCodexBridgeDispatchesGeneratedAndInternalOperations();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
