// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/CurrentProtocolAdapters.h"

#include <ai/openai/codex/frontend/CodexBridge.h>

#include <array>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

namespace adapters = codexui::codex::current_protocol;
namespace requests = adapters::server_requests;
namespace notifications = adapters::server_notifications;

using Bridge = ai::openai::codex::frontend::CodexBridge;
using GeneratedValue = ai::openai::codex::generated::Value;

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
  bridge.onServerNotification<notifications::OperationName>(                  \
      [&notificationCount](notifications::OperationName::Params &) {          \
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
  accepted &= bridge.receive(
      {{"kind", "appserver"},
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
    accepted &= bridge.receive(
        {{"kind", "appserver"},
         {"payload", {{"jsonrpc", "2.0"}, {"method", method},
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
  passed &= testExactMethods();
  passed &= testCurrentTimeRequestAndResponse();
  passed &= testNotificationPayloads();
  passed &= testCodexBridgeDispatchesCompatibilityOperations();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
