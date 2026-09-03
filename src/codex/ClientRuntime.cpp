// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ClientRuntime.h"

#include "codex/Configuration.h"
#include "codex/CurrentProtocolAdapters.h"
#include "codex/NodeGraphJson.h"
#include "codex/PresentationProtocol.h"
#include "codex/ProtocolNormalizer.h"
#include "codex/WorkerMailboxReceiver.h"
#include "codex/ipc/SNodeSocketPairEndpoint.h"
#include "codex/nodegraph/WorkerLogic.h"

#include <ai/openai/codex/frontend/CodexBridge.h>
#include <ai/openai/codex/frontend/client/ClientConnection.h>
#include <ai/openai/codex/frontend/client/StreamSocketContextFactory.h>
#if defined(CODEXUI_CODEX_FRONTEND_WEBSOCKET)
#include <ai/openai/codex/frontend/client/WebSocketClient.h>
#endif
#include <ai/openai/codex/protocol/JsonLineFramer.h>
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
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace codexui::codex {
namespace {

namespace codex = ai::openai::codex;
namespace client = ai::openai::codex::frontend::client;

constexpr std::size_t MaximumIpcReadBytesPerEvent = 256U * 1024U;

nodegraph::Value::Object decodedObject(const nlohmann::json &value) {
  if (value.is_object())
    return objectFromJson(value);
  return {{"value", valueFromJson(value)}};
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
  const std::string kind = presentation::stringMember(message, "kind");
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
    providerState = presentation::stringMember(message, "state");
    detail = presentation::stringMember(message, "reason");
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

template <typename Operation>
void dispatchRequest(codex::frontend::CodexBridge &sdk,
                     const nlohmann::json &parameters, std::string action,
                     std::string correlationId, ProtocolNormalizer &normalizer,
                     nodegraph::WorkerLogic &workerLogic) {
  struct RequestPublication final {
    bool requestPublished = false;
    std::optional<nodegraph::DecodedMessage> synchronousResult;
  };

  auto publication = std::make_shared<RequestPublication>();
  const std::uint64_t startedAtSequence = normalizer.sequence();
  const std::string requestId = sdk.request<Operation>(
      typename Operation::Params{parameters},
      [action = std::move(action), correlationId = std::move(correlationId),
       context = parameters, startedAtSequence, publication, &normalizer,
       &workerLogic](typename Operation::Response &response) mutable {
        const bool ok = response.ok();
        const nlohmann::json &raw = response.getRaw();
        const nlohmann::json payload =
            ok ? response.getPayload()
               : (raw.is_object() && raw.contains("error") ? raw["error"]
                                                           : raw);
        nodegraph::DecodedMessage decoded{
            ok ? nodegraph::DecodedMessageKind::ClientResult
               : nodegraph::DecodedMessageKind::ClientError,
            std::string(Operation::method),
            decodedRequestId(response.jsonRpcId()), decodedObject(payload)};
        if (publication->requestPublished)
          static_cast<void>(workerLogic.apply(std::move(decoded)));
        else
          publication->synchronousResult.emplace(std::move(decoded));
        normalizer.operationResult(std::move(action), std::move(correlationId),
                                   std::move(context), response.getRaw(),
                                   startedAtSequence);
      });
  static_cast<void>(workerLogic.apply(nodegraph::DecodedMessage{
      nodegraph::DecodedMessageKind::ClientRequest,
      std::string(Operation::method), nodegraph::ProtocolRequestId(requestId),
      decodedObject(parameters)}));
  publication->requestPublished = true;
  if (publication->synchronousResult)
    static_cast<void>(
        workerLogic.apply(std::move(*publication->synchronousResult)));
}

} // namespace

int runClientRuntime(int socketPairDescriptor, Configuration &configuration,
                     nodegraph::NodeGraph &graph,
                     nodegraph::ThreadChannels &channels, bool connectBridge) {
  using StreamFactory = client::StreamSocketContextFactory;

  const std::size_t maximumFrameBytes = configuration.maximumFrameBytes();

  auto *ipcEndpoint = ipc::SNodeSocketPairEndpoint::create(
      socketPairDescriptor, DefaultMaximumWriteQueueBytes,
      MaximumIpcReadBytesPerEvent);
  if (!ipcEndpoint)
    return 1;

  codex::protocol::JsonLineFramer ipcFramer(maximumFrameBytes);
  codex::frontend::CodexBridge sdk({});
  nodegraph::WorkerLogic workerLogic(graph, channels);

  const auto sendToQt = [&ipcEndpoint,
                         maximumFrameBytes](const nlohmann::json &message) {
    if (!ipcEndpoint)
      return false;
    try {
      return ipcEndpoint->send(
          codex::protocol::JsonLineFramer::encode(message, maximumFrameBytes));
    } catch (...) {
      return false;
    }
  };

  ProtocolNormalizer normalizer(sendToQt);

  std::function<void()> requestReconnect;
  std::function<void()> requestShutdown;
  normalizer.setDeliveryFailureHandler([&requestShutdown] {
    if (requestShutdown)
      requestShutdown();
  });

  std::string expectedDisconnectReason;
  bool desiredConnected = connectBridge;
  client::ClientConnection connection(
      sdk,
      client::ClientConnectionCallbacks{
          .onConnected =
              [&normalizer, &workerLogic] {
                static_cast<void>(workerLogic.transportEvent("connected"));
                normalizer.transportEvent("connected");
              },
          .onDisconnected =
              [&normalizer, &expectedDisconnectReason, &desiredConnected,
               &workerLogic] {
                std::string reason =
                    std::exchange(expectedDisconnectReason, {});
                static_cast<void>(workerLogic.transportEvent(
                    desiredConnected ? "retrying" : "disconnected", reason));
                normalizer.transportEvent(desiredConnected ? "retrying"
                                                           : "disconnected",
                                          std::move(reason));
              },
          .onFailure =
              [&normalizer, &workerLogic](std::string reason) {
                static_cast<void>(
                    workerLogic.transportEvent("failure", reason));
                normalizer.transportEvent("failure", std::move(reason));
              }});

  sdk.onRawJson([&normalizer,
                 &workerLogic](codex::protocol::AppServerDirection direction,
                               const nlohmann::json &message) {
    if (direction == codex::protocol::AppServerDirection::FromAppServer) {
      normalizer.observeRawInbound(message);
      return;
    }
    const std::optional<std::string> method =
        codex::protocol::jsonRpcMethod(message);
    if (method && *method == "initialized") {
      const auto parameters = message.find("params");
      const nlohmann::json payload =
          parameters == message.end() ? nlohmann::json::object() : *parameters;
      static_cast<void>(workerLogic.apply(nodegraph::DecodedMessage{
          nodegraph::DecodedMessageKind::ClientNotification, *method,
          std::nullopt, decodedObject(payload)}));
    }
  });
  sdk.onBridgeEvent(
      [&normalizer, &workerLogic, &sdk](const nlohmann::json &message) {
        applyBridgeState(workerLogic, sdk, message);
        normalizer.bridgeEvent(message);
      });

#define CODEXUI_REGISTER_SERVER_REQUEST(OperationName, methodName)             \
  sdk.on##OperationName(                                                       \
      [&normalizer, &workerLogic](                                             \
          codex::generated::server_requests::OperationName::Params &request) { \
        static_cast<void>(workerLogic.apply(nodegraph::DecodedMessage{         \
            nodegraph::DecodedMessageKind::ServerRequest,                      \
            std::string(                                                       \
                codex::generated::server_requests::OperationName::method),     \
            decodedRequestId(request.jsonRpcId()),                             \
            decodedObject(request.getPayload())}));                            \
        normalizer.serverRequest(                                              \
            codex::generated::server_requests::OperationName::method,          \
            request.jsonRpcId(), request.getPayload());                        \
      });
  AI_OPENAI_CODEX_SERVER_REQUESTS(CODEXUI_REGISTER_SERVER_REQUEST)
#undef CODEXUI_REGISTER_SERVER_REQUEST

#define CODEXUI_REGISTER_SERVER_NOTIFICATION(OperationName, methodName)        \
  sdk.on##OperationName([&normalizer, &workerLogic](                           \
                            codex::generated::server_notifications::           \
                                OperationName::Params &notification) {         \
    static_cast<void>(workerLogic.apply(nodegraph::DecodedMessage{             \
        nodegraph::DecodedMessageKind::ServerNotification,                     \
        std::string(                                                           \
            codex::generated::server_notifications::OperationName::method),    \
        std::nullopt, decodedObject(notification.getPayload())}));             \
    normalizer.serverNotification(                                             \
        codex::generated::server_notifications::OperationName::method,         \
        notification.getPayload());                                            \
  });
  AI_OPENAI_CODEX_SERVER_NOTIFICATIONS(CODEXUI_REGISTER_SERVER_NOTIFICATION)
#undef CODEXUI_REGISTER_SERVER_NOTIFICATION

  using CurrentTimeRead = current_protocol::server_requests::CurrentTimeRead;
  sdk.onServerRequest<CurrentTimeRead>(
      [&normalizer, &workerLogic, &sdk](CurrentTimeRead::Params &request) {
        const auto requestId = requestIdFromJson(request.jsonRpcId());
        static_cast<void>(workerLogic.apply(nodegraph::DecodedMessage{
            nodegraph::DecodedMessageKind::ServerRequest,
            std::string(CurrentTimeRead::method), requestId,
            decodedObject(request.getPayload())}));
        const std::int64_t currentTime =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        const CurrentTimeRead::Response response(
            nlohmann::json{{"currentTimeAt", currentTime}});
        const bool accepted = sdk.respond<CurrentTimeRead>(request, response);
        static_cast<void>(workerLogic.resolveInteraction(
            requestId, accepted,
            accepted ? std::string{}
                     : "CodexBridge rejected current-time response"));
        if (!accepted)
          normalizer.transportEvent(
              "failure", "current-time server response was rejected");
      });

#define CODEXUI_REGISTER_CURRENT_NOTIFICATION(OperationName)                   \
  sdk.onServerNotification<                                                    \
      current_protocol::server_notifications::OperationName>(                  \
      [&workerLogic](                                                          \
          current_protocol::server_notifications::OperationName::Params        \
              &notification) {                                                 \
        static_cast<void>(workerLogic.apply(nodegraph::DecodedMessage{         \
            nodegraph::DecodedMessageKind::ServerNotification,                 \
            std::string(current_protocol::server_notifications::               \
                            OperationName::method),                            \
            std::nullopt, decodedObject(notification.getPayload())}));         \
      });
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ModelProviderAuthRecoveryStarted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ModelProviderAuthRecoveryCompleted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(RawResponseItemCompleted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(RawResponseCompleted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ThreadRealtimeItemStarted)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ThreadRealtimeItemTranscriptDelta)
  CODEXUI_REGISTER_CURRENT_NOTIFICATION(ThreadRealtimeItemCompleted)
#undef CODEXUI_REGISTER_CURRENT_NOTIFICATION

  const auto publishTransportEvent =
      [&normalizer, &workerLogic](std::string state, std::string detail = {}) {
        static_cast<void>(workerLogic.transportEvent(state, detail));
        normalizer.transportEvent(std::move(state), std::move(detail));
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
  bool shutdownDraining = false;
  bool eventLoopRunning = false;
  std::function<void()> continueTransition;
  std::function<bool()> terminatingFlowTerminated;
  std::function<void()> pendingSelection;
  std::chrono::steady_clock::time_point transitionDeadline;
  std::chrono::steady_clock::time_point shutdownDrainDeadline;
  std::function<void()> finishShutdownAfterDrain;

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
    normalizer.connectionSettings(std::move(settings));
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
    if (ipcEndpoint)
      ipcEndpoint->close();
    if (eventLoopRunning)
      core::SNodeC::stop();
  };

  finishShutdownAfterDrain = [&] {
    if (shutdownRequested)
      return;
    if (ipcEndpoint && ipcEndpoint->queuedBytes() != 0 &&
        std::chrono::steady_clock::now() < shutdownDrainDeadline) {
      static_cast<void>(core::timer::Timer::singleshotTimer(
          finishShutdownAfterDrain, utils::Timeval({0, 10000})));
      return;
    }
    requestShutdown();
  };

  const auto dispatchCommand = [&](nlohmann::json command) {
    if (!presentation::isPresentationFrame(command) ||
        presentation::stringMember(command, "kind") != "command") {
      publishTransportEvent("failure", "invalid CodexUI presentation command");
      return;
    }

    const std::string action = presentation::stringMember(command, "action");
    const std::string correlationId =
        presentation::stringMember(command, "correlationId");
    const nlohmann::json parameters =
        presentation::member(command, "data", nlohmann::json::object());

    if (!parameters.is_object()) {
      normalizer.operationRejected(
          action, correlationId, -32602,
          "presentation command data must be an object");
      return;
    }

    if (action == "runtime.shutdown") {
      if (!shutdownDraining) {
        shutdownDraining = true;
        normalizer.localOperationResult(action, correlationId, true,
                                        nlohmann::json::object());
        shutdownDrainDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        finishShutdownAfterDrain();
      }
      return;
    }
    if (action == "connection.reconnect") {
      requestReconnect();
      return;
    }
    if (action == "connection.connect") {
      requestConnect();
      return;
    }
    if (action == "connection.disconnect") {
      requestDisconnect();
      return;
    }
    if (action == "connection.configure") {
      if (transitionPending) {
        normalizer.localOperationResult(
            action, correlationId, false,
            {{"code", -32000},
             {"message", "connection transition in progress"}});
        return;
      }
      const std::string transport =
          presentation::stringMember(parameters, "transport");
      std::function<void()> selection;
      const auto networkEndpoint =
          [&]() -> std::optional<std::pair<std::string, std::uint16_t>> {
        const std::string host = presentation::stringMember(parameters, "host");
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
        const std::string path = presentation::stringMember(parameters, "path");
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
        const std::string address =
            presentation::stringMember(parameters, "address");
        const auto channel = parameters.find("channel");
        if (!address.empty() && channel != parameters.end() &&
            channel->is_number_integer() && channel->get<int>() > 0 &&
            channel->get<int>() <= 30) {
          const auto value = static_cast<std::uint8_t>(channel->get<int>());
          if (transport == "rfcomm") {
            selection = [&, address, value] {
              rfcommClient.getConfig()
                  ->Remote::setBtAddress(address)
                  ->setChannel(value);
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
        const std::string path =
            presentation::stringMember(parameters, "webSocketPath");
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
        normalizer.localOperationResult(
            action, correlationId, false,
            {{"code", -32602}, {"message", "invalid connection settings"}});
        return;
      }
      beginTransition(true, std::move(selection), "local-transport-switch");
      normalizer.localOperationResult(action, correlationId, true,
                                      {{"accepted", true}});
      return;
    }
    if (action == "controller.claim") {
      static_cast<void>(sdk.claimController());
      return;
    }
    if (action == "controller.release") {
      static_cast<void>(sdk.releaseController());
      return;
    }

    if (action == "diagnostic.raw.send") {
      const auto message = parameters.find("message");
      if (message == parameters.end() || !sdk.sendRawJson(*message))
        publishTransportEvent("failure", "raw app-server message was rejected");
      return;
    }

    if (action == "pending-request.resolve") {
      const auto requestIdMember = parameters.find("requestId");
      const nlohmann::json requestId = requestIdMember == parameters.end()
                                           ? nlohmann::json(nullptr)
                                           : *requestIdMember;
      nlohmann::json response{{"jsonrpc", "2.0"}, {"id", requestId}};
      if (parameters.contains("error"))
        response["error"] = parameters["error"];
      else
        response["result"] = presentation::member(parameters, "result",
                                                  nlohmann::json::object());
      const bool accepted = !requestId.is_null() && sdk.sendRawJson(response);
      if (!requestId.is_null()) {
        static_cast<void>(workerLogic.resolveInteraction(
            requestIdFromJson(requestId), accepted,
            accepted ? std::string{}
                     : "CodexBridge rejected the server-request response"));
      }
      if (!accepted)
        publishTransportEvent("failure",
                              "server-request response was rejected");
      return;
    }

    using namespace codex::generated::client_requests;
    if (action == "threads.list")
      dispatchRequest<ThreadList>(sdk, parameters, action, correlationId,
                                  normalizer, workerLogic);
    else if (action == "thread.read")
      dispatchRequest<ThreadRead>(sdk, parameters, action, correlationId,
                                  normalizer, workerLogic);
    else if (action == "thread.create")
      dispatchRequest<ThreadStart>(sdk, parameters, action, correlationId,
                                   normalizer, workerLogic);
    else if (action == "thread.resume")
      dispatchRequest<ThreadResume>(sdk, parameters, action, correlationId,
                                    normalizer, workerLogic);
    else if (action == "thread.fork")
      dispatchRequest<ThreadFork>(sdk, parameters, action, correlationId,
                                  normalizer, workerLogic);
    else if (action == "thread.rename")
      dispatchRequest<ThreadSetName>(sdk, parameters, action, correlationId,
                                     normalizer, workerLogic);
    else if (action == "thread.archive")
      dispatchRequest<ThreadArchive>(sdk, parameters, action, correlationId,
                                     normalizer, workerLogic);
    else if (action == "thread.unarchive")
      dispatchRequest<ThreadUnarchive>(sdk, parameters, action, correlationId,
                                       normalizer, workerLogic);
    else if (action == "thread.delete")
      dispatchRequest<ThreadDelete>(sdk, parameters, action, correlationId,
                                    normalizer, workerLogic);
    else if (action == "models.list")
      dispatchRequest<ModelList>(sdk, parameters, action, correlationId,
                                 normalizer, workerLogic);
    else if (action == "model-provider-capabilities.read")
      dispatchRequest<ModelProviderCapabilitiesRead>(
          sdk, parameters, action, correlationId, normalizer, workerLogic);
    else if (action == "account.read")
      dispatchRequest<GetAccount>(sdk, parameters, action, correlationId,
                                  normalizer, workerLogic);
    else if (action == "account.rate-limits.read")
      dispatchRequest<GetAccountRateLimits>(
          sdk, parameters, action, correlationId, normalizer, workerLogic);
    else if (action == "account.token-usage.read")
      dispatchRequest<GetAccountTokenUsage>(
          sdk, parameters, action, correlationId, normalizer, workerLogic);
    else if (action == "config.read")
      dispatchRequest<ConfigRead>(sdk, parameters, action, correlationId,
                                  normalizer, workerLogic);
    else if (action == "permission-profiles.list")
      dispatchRequest<PermissionProfileList>(
          sdk, parameters, action, correlationId, normalizer, workerLogic);
    else if (action == "experimental-features.list")
      dispatchRequest<ExperimentalFeatureList>(
          sdk, parameters, action, correlationId, normalizer, workerLogic);
    else if (action == "skills.list")
      dispatchRequest<SkillsList>(sdk, parameters, action, correlationId,
                                  normalizer, workerLogic);
    else if (action == "hooks.list")
      dispatchRequest<HooksList>(sdk, parameters, action, correlationId,
                                 normalizer, workerLogic);
    else if (action == "plugins.list")
      dispatchRequest<PluginList>(sdk, parameters, action, correlationId,
                                  normalizer, workerLogic);
    else if (action == "apps.list")
      dispatchRequest<AppsList>(sdk, parameters, action, correlationId,
                                normalizer, workerLogic);
    else if (action == "mcp-servers.list")
      dispatchRequest<McpServerStatusList>(
          sdk, parameters, action, correlationId, normalizer, workerLogic);
#define CODEXUI_DISPATCH_PRESENTATION_REQUEST(ActionName, OperationName)       \
  else if (action == ActionName) dispatchRequest<OperationName>(               \
      sdk, parameters, action, correlationId, normalizer, workerLogic);
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.unsubscribe",
                                          ThreadUnsubscribe)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.goal.set", ThreadGoalSet)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.goal.get", ThreadGoalGet)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.goal.clear", ThreadGoalClear)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.metadata.update",
                                          ThreadMetadataUpdate)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.section.move",
                                          ThreadSectionMove)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.compact.start",
                                          ThreadCompactStart)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.shell-command.start",
                                          ThreadShellCommand)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.guardian-denial.approve",
                                          ThreadApproveGuardianDeniedAction)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.rollback", ThreadRollback)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.sections.list",
                                          ThreadSectionList)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.section.create",
                                          ThreadSectionCreate)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.section.update",
                                          ThreadSectionUpdate)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.section.delete",
                                          ThreadSectionDelete)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("threads.loaded.list",
                                          ThreadLoadedList)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("thread.items.inject",
                                          ThreadInjectItems)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("skills.extra-roots.set",
                                          SkillsExtraRootsSet)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("marketplace.add", MarketplaceAdd)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("marketplace.remove",
                                          MarketplaceRemove)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("marketplace.upgrade",
                                          MarketplaceUpgrade)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugins.installed", PluginInstalled)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.read", PluginRead)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.skill.read", PluginSkillRead)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.share.save", PluginShareSave)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.share.targets.update",
                                          PluginShareUpdateTargets)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.shares.list", PluginShareList)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.share.checkout",
                                          PluginShareCheckout)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.share.delete",
                                          PluginShareDelete)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("apps.read", AppsRead)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("apps.installed", AppsInstalled)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.file.read", FsReadFile)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.file.write", FsWriteFile)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.directory.create",
                                          FsCreateDirectory)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.metadata.read",
                                          FsGetMetadata)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.directory.read",
                                          FsReadDirectory)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.remove", FsRemove)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.copy", FsCopy)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.watch", FsWatch)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("filesystem.unwatch", FsUnwatch)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("skills.config.write",
                                          SkillsConfigWrite)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.install", PluginInstall)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("plugin.uninstall", PluginUninstall)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("review.start", ReviewStart)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST(
        "experimental-features.enablement.set",
        ExperimentalFeatureEnablementSet)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("mcp-server.oauth-login.start",
                                          McpServerOauthLogin)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("mcp-servers.refresh",
                                          McpServerRefresh)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("mcp-resource.read", McpResourceRead)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("mcp-server.tool.call",
                                          McpServerToolCall)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("windows-sandbox.setup.start",
                                          WindowsSandboxSetupStart)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("windows-sandbox.readiness",
                                          WindowsSandboxReadiness)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("account.login.start", LoginAccount)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("account.login.cancel",
                                          CancelLoginAccount)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("account.logout", LogoutAccount)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST(
        "account.rate-limit-reset-credit.consume",
        ConsumeAccountRateLimitResetCredit)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("workspace.messages.read",
                                          GetWorkspaceMessages)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("account.credits-nudge-email.send",
                                          SendAddCreditsNudgeEmail)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("feedback.upload", FeedbackUpload)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("command.execute", OneOffCommandExec)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("command.stdin.write",
                                          CommandExecWrite)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("command.terminate",
                                          CommandExecTerminate)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("command.resize", CommandExecResize)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("external-agent-config.detect",
                                          ExternalAgentConfigDetect)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("external-agent-config.import",
                                          ExternalAgentConfigImport)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST(
        "external-agent-config.import-history.record",
        ExternalAgentConfigImportHistoryRecord)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST(
        "external-agent-config.import-histories.read",
        ExternalAgentConfigImportHistoriesRead)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("config.value.write",
                                          ConfigValueWrite)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("config.batch.write",
                                          ConfigBatchWrite)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("config.requirements.read",
                                          ConfigRequirementsRead)
    CODEXUI_DISPATCH_PRESENTATION_REQUEST("workspace.search.start",
                                          FuzzyFileSearch)
#undef CODEXUI_DISPATCH_PRESENTATION_REQUEST
    else if (action == "turn.start") dispatchRequest<TurnStart>(
        sdk, parameters, action, correlationId, normalizer, workerLogic);
    else if (action == "turn.steer") dispatchRequest<TurnSteer>(
        sdk, parameters, action, correlationId, normalizer, workerLogic);
    else if (action == "turn.interrupt") dispatchRequest<TurnInterrupt>(
        sdk, parameters, action, correlationId, normalizer, workerLogic);
    else normalizer.operationRejected(
        action, correlationId, -32601,
        "unsupported CodexUI presentation action");
  };

  const bool workerMailboxReady =
      WorkerMailboxReceiver::create(channels, [&workerLogic, &requestShutdown,
                                               &channels](
                                                  nodegraph::QtToWorkerMessage
                                                      message) {
        std::visit(
            [&](auto &payload) {
              using Message = std::decay_t<decltype(payload)>;
              if constexpr (std::is_same_v<Message, nodegraph::NodeAction>) {
                if (payload.kind == nodegraph::NodeActionKind::UiDetached) {
                  static_cast<void>(workerLogic.acknowledgeUiDetached(
                      std::move(payload.target)));
                  return;
                }
                nodegraph::UiEffect rejected{
                    nodegraph::UiEffectKind::ShowNotice,
                    payload.target,
                    "Typed user action is disabled during graph comparison",
                    {}};
                static_cast<void>(channels.sendUiEffect(rejected));
              } else if constexpr (std::is_same_v<Message,
                                                  nodegraph::RuntimeAction>) {
                nodegraph::UiEffect rejected{
                    nodegraph::UiEffectKind::ShowNotice,
                    std::nullopt,
                    "Typed runtime action is disabled during graph "
                    "comparison",
                    {}};
                static_cast<void>(channels.sendUiEffect(rejected));
              } else if (requestShutdown) {
                requestShutdown();
              }
            },
            message);
      }) != nullptr;

  ipcEndpoint->setOnData([&](const char *data, std::size_t size) {
    try {
      const bool accepted = ipcFramer.consume(
          std::string_view(data, size), dispatchCommand,
          [&publishTransportEvent, &requestShutdown](std::string message) {
            publishTransportEvent("failure", std::move(message));
            requestShutdown();
          });
      if (!accepted)
        requestShutdown();
    } catch (const std::exception &exception) {
      publishTransportEvent(
          "failure", std::string("presentation command dispatch failed: ") +
                         exception.what());
      requestShutdown();
    } catch (...) {
      publishTransportEvent("failure", "presentation command dispatch failed");
      requestShutdown();
    }
  });
  ipcEndpoint->setOnError(
      [&publishTransportEvent, &requestShutdown](int errorNumber) {
        publishTransportEvent("failure", std::string("socketpair failure: ") +
                                             std::to_string(errorNumber));
        requestShutdown();
      });
  ipcEndpoint->setOnClosed([&ipcEndpoint, &requestShutdown] {
    ipcEndpoint = nullptr;
    requestShutdown();
  });

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
