// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ClientRuntime.h"
#include "codex/Configuration.h"
#include "codex/NodeGraphJson.h"
#include "codex/nodegraph/Messages.h"
#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/ThreadChannels.h"

#include <core/SNodeC.h>
#include <utils/Config.h>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace codexui::codex {
namespace {

using namespace std::chrono_literals;
using namespace nodegraph;

int failures = 0;

Value::Object userInputSubmission(std::string questionId, std::string answer) {
  return {
      {"choice", Value("submit")},
      {"input",
       Value(Value::Object{
           {std::move(questionId),
            Value(Value::Object{{"answers", Value(Value::Array{Value(
                                                std::move(answer))})}})}})}};
}

nlohmann::json userInputQuestions(std::string id) {
  return nlohmann::json::array({{{"id", std::move(id)},
                                 {"question", "Provide a value"},
                                 {"options", nullptr}}});
}

void expect(bool condition, std::string_view message) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

class UnixBridge final {
public:
  UnixBridge() {
    path_ = "/tmp/codexui-runtime-dispatch-" +
            std::to_string(static_cast<long long>(::getpid())) + ".sock";
    static_cast<void>(::unlink(path_.c_str()));
    listener_ =
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_ < 0)
      return;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(address.sun_path))
      return;
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1U);
    if (::bind(listener_, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0) {
      static_cast<void>(::close(listener_));
      listener_ = -1;
    }
  }

  ~UnixBridge() {
    if (client_ >= 0)
      static_cast<void>(::close(client_));
    if (listener_ >= 0)
      static_cast<void>(::close(listener_));
    static_cast<void>(::unlink(path_.c_str()));
  }

  UnixBridge(const UnixBridge &) = delete;
  UnixBridge &operator=(const UnixBridge &) = delete;

  [[nodiscard]] bool valid() const noexcept { return listener_ >= 0; }
  [[nodiscard]] const std::string &path() const noexcept { return path_; }

  bool acceptClient(std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd event{listener_, POLLIN, 0};
      const int ready = ::poll(&event, 1, 20);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready <= 0)
        continue;
      client_ =
          ::accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (client_ >= 0)
        return true;
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        return false;
    }
    return false;
  }

  bool send(nlohmann::json message, std::chrono::milliseconds timeout = 2s) {
    std::string encoded = message.dump();
    encoded.push_back('\n');
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (offset < encoded.size() &&
           std::chrono::steady_clock::now() < deadline) {
      const ssize_t written =
          ::write(client_, encoded.data() + offset, encoded.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR)
        continue;
      if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return false;
      pollfd event{client_, POLLOUT, 0};
      static_cast<void>(::poll(&event, 1, 10));
    }
    return offset == encoded.size();
  }

  std::optional<nlohmann::json>
  receive(std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (const std::size_t newline = buffered_.find('\n');
          newline != std::string::npos) {
        std::string line = buffered_.substr(0, newline);
        buffered_.erase(0, newline + 1U);
        try {
          return nlohmann::json::parse(line);
        } catch (...) {
          return std::nullopt;
        }
      }

      std::array<char, 16U * 1024U> incoming{};
      const ssize_t received =
          ::read(client_, incoming.data(), incoming.size());
      if (received > 0) {
        buffered_.append(incoming.data(), static_cast<std::size_t>(received));
        continue;
      }
      if (received == 0)
        return std::nullopt;
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        return std::nullopt;
      pollfd event{client_, POLLIN, 0};
      static_cast<void>(::poll(&event, 1, 10));
    }
    return std::nullopt;
  }

  std::optional<nlohmann::json>
  receiveAppServer(std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      std::optional<nlohmann::json> envelope = receive(remaining);
      if (!envelope)
        return std::nullopt;
      if (envelope->value("kind", std::string{}) == "appserver" &&
          envelope->contains("payload"))
        return envelope->at("payload");
    }
    return std::nullopt;
  }

  bool reply(const nlohmann::json &request, nlohmann::json result) {
    return send({{"kind", "appserver"},
                 {"connectionId", "runtime-test"},
                 {"role", "controller"},
                 {"seq", nextSequence_++},
                 {"payload",
                  {{"jsonrpc", "2.0"},
                   {"id", request.at("id")},
                   {"result", std::move(result)}}}});
  }

  bool replyError(const nlohmann::json &request, int code,
                  std::string message) {
    return send(
        {{"kind", "appserver"},
         {"connectionId", "runtime-test"},
         {"role", "controller"},
         {"seq", nextSequence_++},
         {"payload",
          {{"jsonrpc", "2.0"},
           {"id", request.at("id")},
           {"error", {{"code", code}, {"message", std::move(message)}}}}}});
  }

  bool appServerRequest(std::string id, std::string method,
                        nlohmann::json parameters,
                        std::string role = "controller") {
    return send({{"kind", "appserver"},
                 {"connectionId", "runtime-test"},
                 {"role", std::move(role)},
                 {"seq", nextSequence_++},
                 {"payload",
                  {{"jsonrpc", "2.0"},
                   {"id", std::move(id)},
                   {"method", std::move(method)},
                   {"params", std::move(parameters)}}}});
  }

  bool appServerNotification(std::string method, nlohmann::json parameters) {
    return send({{"kind", "appserver"},
                 {"connectionId", "runtime-test"},
                 {"role", "controller"},
                 {"seq", nextSequence_++},
                 {"payload",
                  {{"jsonrpc", "2.0"},
                   {"method", std::move(method)},
                   {"params", std::move(parameters)}}}});
  }

  bool setRole(std::string role, std::string controllerConnectionId) {
    if (!send({{"kind", "bridge.connection"},
               {"event", "opened"},
               {"connectionId", "runtime-test"},
               {"role", std::move(role)},
               {"seq", nextSequence_++}}))
      return false;
    return send({{"kind", "bridge.controller"},
                 {"controllerConnectionId", std::move(controllerConnectionId)},
                 {"seq", nextSequence_++}});
  }

  bool setProviderGeneration(std::uint64_t generation) {
    return send({{"kind", "bridge.provider"},
                 {"state", "ready"},
                 {"providerGeneration", generation},
                 {"seq", nextSequence_++}});
  }

private:
  std::string path_;
  int listener_ = -1;
  int client_ = -1;
  std::uint64_t nextSequence_ = 4;
  std::string buffered_;
};

class RunningRuntime final {
public:
  explicit RunningRuntime(Configuration &configuration)
      : configuration_(configuration) {}

  ~RunningRuntime() { stop(); }

  RunningRuntime(const RunningRuntime &) = delete;
  RunningRuntime &operator=(const RunningRuntime &) = delete;

  void start() {
    worker_ = std::thread([this] {
      result_.store(runClientRuntime(configuration_, graph_, channels_, false),
                    std::memory_order_release);
    });
  }

  void stop() {
    if (!worker_.joinable())
      return;
    ShutdownRequest shutdown;
    for (int attempt = 0; attempt != 1000; ++attempt) {
      if (messageAdmitted(channels_.sendShutdown(shutdown)))
        break;
      std::this_thread::sleep_for(1ms);
    }
    worker_.join();
  }

  NodeGraph &graph() noexcept { return graph_; }
  ThreadChannels &channels() noexcept { return channels_; }

  std::optional<std::chrono::nanoseconds> workerCpuTime() {
    if (!worker_.joinable())
      return std::nullopt;
    clockid_t clock{};
    if (::pthread_getcpuclockid(worker_.native_handle(), &clock) != 0)
      return std::nullopt;
    timespec value{};
    if (::clock_gettime(clock, &value) != 0)
      return std::nullopt;
    return std::chrono::seconds(value.tv_sec) +
           std::chrono::nanoseconds(value.tv_nsec);
  }

  void drainNotifications() {
    static_cast<void>(channels_.drainWorkerToQtWake());
    WorkerToQtMessage message;
    while (channels_.tryReceiveForQt(message))
      message = WorkerStopped{};
  }

  std::vector<UiEffect> takeUiEffects() {
    std::vector<UiEffect> effects;
    static_cast<void>(channels_.drainWorkerToQtWake());
    WorkerToQtMessage message;
    while (channels_.tryReceiveForQt(message)) {
      if (UiEffect *effect = std::get_if<UiEffect>(&message))
        effects.emplace_back(std::move(*effect));
      message = WorkerStopped{};
    }
    return effects;
  }

  std::vector<UiEffect> takeProtocolDiagnostics() {
    std::vector<UiEffect> diagnostics;
    for (UiEffect &effect : takeUiEffects())
      if (effect.kind == UiEffectKind::ProtocolDiagnostic)
        diagnostics.emplace_back(std::move(effect));
    return diagnostics;
  }

private:
  Configuration &configuration_;
  NodeGraph graph_;
  ThreadChannels channels_;
  std::thread worker_;
  std::atomic<int> result_{-1};
};

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

NodeRef findNode(NodeGraph &graph, NodeId id) {
  NodeRef found;
  static_cast<void>(waitUntil([&] {
    std::optional<NodeGraph::ReadAccess> read = graph.tryRead();
    if (!read)
      return false;
    found = read->find(id);
    return static_cast<bool>(found);
  }));
  return found;
}

NodeRef findRetiredNode(NodeGraph &graph, const NodeId &id) {
  NodeRef found;
  static_cast<void>(waitUntil([&] {
    std::optional<NodeGraph::ReadAccess> read = graph.tryRead();
    if (!read)
      return false;
    for (std::size_t index = 0; index < read->retiredCount(); ++index) {
      const NodeRef node = read->retiredAt(index);
      if (node && node->id() == id) {
        found = node;
        return true;
      }
    }
    return false;
  }));
  return found;
}

bool operationPending(NodeGraph &graph, const nlohmann::json &requestId) {
  const ProtocolRequestId id = requestIdFromJson(requestId);
  std::optional<NodeGraph::ReadAccess> read = graph.tryRead();
  if (!read)
    return false;
  const NodeRef operation = read->find({NodeKind::Operation, id.canonical()});
  return operation && read->state(operation)->status == NodeStatus::Pending;
}

bool operationTargets(NodeGraph &graph, const nlohmann::json &requestId,
                      const NodeRef &target) {
  const ProtocolRequestId id = requestIdFromJson(requestId);
  std::optional<NodeGraph::ReadAccess> read = graph.tryRead();
  if (!read)
    return false;
  const NodeRef operation = read->find({NodeKind::Operation, id.canonical()});
  return operation && read->related(target, RelationKind::PendingOperation) ==
                          std::vector<NodeRef>{operation};
}

bool operationRetired(NodeGraph &graph, const nlohmann::json &requestId) {
  const ProtocolRequestId id = requestIdFromJson(requestId);
  std::optional<NodeGraph::ReadAccess> read = graph.tryRead();
  return read && !read->find({NodeKind::Operation, id.canonical()});
}

bool interactionRetired(NodeGraph &graph, const NodeRef &interaction) {
  if (!interaction)
    return false;
  std::optional<NodeGraph::ReadAccess> read = graph.tryRead();
  if (!read || read->find(interaction->id()))
    return false;
  for (std::size_t index = 0; index < read->retiredCount(); ++index)
    if (read->retiredAt(index) == interaction)
      return true;
  return false;
}

bool sendAction(ThreadChannels &channels, NodeAction action) {
  return messageAdmitted(channels.sendNodeAction(action));
}

bool sendAction(ThreadChannels &channels, RuntimeAction action) {
  return messageAdmitted(channels.sendRuntimeAction(action));
}

nlohmann::json listedThread() {
  return {{"id", "runtime-thread"},
          {"name", "Runtime dispatch"},
          {"status", "idle"},
          {"historyMode", "legacy"},
          {"createdAt", 1},
          {"updatedAt", 2},
          {"turns", nlohmann::json::array()}};
}

bool rejectedAutomaticCurrentTimeResponseIsTerminal(UnixBridge &bridge,
                                                    RunningRuntime &runtime) {
  constexpr std::string_view RequestId = "clock-before-identity";
  if (!bridge.appServerRequest(std::string(RequestId), "currentTime/read",
                               {{"threadId", "pre-identity-thread"}}))
    return false;

  // The transport is attached, but CodexBridge has not received its
  // bridge.connection identity yet. Its sender therefore deterministically
  // rejects the automatic response without disconnecting the worker.
  const NodeId interactionId{
      NodeKind::Interaction,
      ProtocolRequestId(std::string(RequestId)).canonical()};
  const NodeRef retired = findRetiredNode(runtime.graph(), interactionId);
  std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
  const bool terminal = retired && read && !read->find(interactionId);
  read.reset();
  return terminal && !bridge.receiveAppServer(100ms);
}

bool establishProvider(UnixBridge &bridge, RunningRuntime &runtime) {
  RuntimeAction configure;
  configure.kind = RuntimeActionKind::ConfigureConnection;
  configure.payload = {{"transport", Value("unix")},
                       {"path", Value(bridge.path())}};
  if (!sendAction(runtime.channels(), std::move(configure)))
    return false;
  if (!bridge.acceptClient())
    return false;

  expect(rejectedAutomaticCurrentTimeResponseIsTerminal(bridge, runtime),
         "a rejected automatic current-time response retires its interaction "
         "instead of exposing an unresolvable UI action");

  if (!bridge.send({{"kind", "bridge.connection"},
                    {"event", "opened"},
                    {"connectionId", "runtime-test"},
                    {"role", "controller"},
                    {"seq", 1}}) ||
      !bridge.send({{"kind", "bridge.controller"},
                    {"controllerConnectionId", "runtime-test"},
                    {"seq", 2}}) ||
      !bridge.send({{"kind", "bridge.provider"},
                    {"state", "ready"},
                    {"providerGeneration", 1},
                    {"seq", 3}}))
    return false;

  std::unordered_map<std::string, std::size_t> methods;
  for (int count = 0; count != 4; ++count) {
    std::optional<nlohmann::json> request = bridge.receiveAppServer();
    if (!request || !request->contains("id"))
      return false;
    const std::string method = request->value("method", std::string{});
    ++methods[method];
    nlohmann::json result{{"data", nlohmann::json::array()}};
    if (method == "thread/list") {
      const bool firstThreadPage = methods[method] == 1;
      expect(request->at("params").value("sortKey", std::string{}) ==
                     "recency_at" &&
                 request->at("params").value("sortDirection", std::string{}) ==
                     "desc" &&
                 request->at("params").value("limit", 0) == 100 &&
                 request->at("params").value("useStateDbOnly", false) ==
                     firstThreadPage,
             "initial thread hydration requests a fast DB page followed by "
             "the repair page");
      result["data"].push_back(listedThread());
      result["nextCursor"] = nullptr;
    }
    if (!bridge.reply(*request, std::move(result)))
      return false;
  }
  return methods ==
             std::unordered_map<std::string, std::size_t>{
                 {"thread/list", 2},
                 {"model/list", 1},
                 {"permissionProfile/list", 1}} &&
         static_cast<bool>(
             findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"}));
}

std::string diagnosticField(const UiEffect &effect, std::string_view key) {
  const auto found = effect.details.find(key);
  if (found == effect.details.end() || !found->second.asString())
    return {};
  return *found->second.asString();
}

void protocolDiagnosticsPreserveMetadataWithoutPayloads(
    UnixBridge &bridge, RunningRuntime &runtime) {
  const std::vector<UiEffect> initialDiagnostics =
      runtime.takeProtocolDiagnostics();
  expect(std::ranges::any_of(initialDiagnostics,
                             [](const UiEffect &effect) {
                               return diagnosticField(effect, "subject") ==
                                      "connection.lifecycle";
                             }) &&
             std::ranges::any_of(initialDiagnostics,
                                 [](const UiEffect &effect) {
                                   return diagnosticField(effect, "subject") ==
                                              "connection.provider" &&
                                          diagnosticField(
                                              effect, "authority") == "replace";
                                 }),
         "transport lifecycle and bridge provider diagnostics remain visible");
  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  if (!thread)
    return;

  NodeAction rename{thread, NodeActionKind::Rename};
  rename.payload = {{"name", Value("authored-name-must-not-appear")}};
  expect(sendAction(runtime.channels(), std::move(rename)),
         "diagnostic rename enters the typed mailbox");
  const std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(request &&
             request->value("method", std::string{}) == "thread/name/set",
         "diagnostic fixture receives the direct request");
  if (!request)
    return;
  expect(bridge.replyError(*request, -32041, "rename rejected safely"),
         "diagnostic fixture receives a benign JSON-RPC error");

  std::vector<UiEffect> diagnostics;
  expect(waitUntil([&] {
           std::vector<UiEffect> batch = runtime.takeProtocolDiagnostics();
           diagnostics.insert(diagnostics.end(),
                              std::make_move_iterator(batch.begin()),
                              std::make_move_iterator(batch.end()));
           return std::ranges::any_of(diagnostics, [](const UiEffect &effect) {
             return diagnosticField(effect, "direction") == "client error" &&
                    diagnosticField(effect, "subject") == "thread/name/set";
           });
         }),
         "request and response diagnostics cross the typed worker queue");

  const UiEffect *sent = nullptr;
  const UiEffect *failed = nullptr;
  for (const UiEffect &effect : diagnostics) {
    if (diagnosticField(effect, "subject") != "thread/name/set")
      continue;
    if (diagnosticField(effect, "direction") == "client request")
      sent = &effect;
    if (diagnosticField(effect, "direction") == "client error")
      failed = &effect;
  }
  expect(sent && failed && diagnosticField(*sent, "source") == "CodexUI" &&
             diagnosticField(*sent, "authority") == "none" &&
             diagnosticField(*sent, "threadId") == "runtime-thread" &&
             diagnosticField(*failed, "source") == "app-server" &&
             diagnosticField(*failed, "authority") == "none" &&
             diagnosticField(*failed, "threadId") == "runtime-thread" &&
             diagnosticField(*failed, "outcome") == "ERROR" &&
             diagnosticField(*failed, "errorCategory") == "json-rpc" &&
             diagnosticField(*failed, "errorCode") == "-32041" &&
             diagnosticField(*failed, "error") == "rename rejected safely" &&
             diagnosticField(*sent, "correlation") ==
                 diagnosticField(*failed, "correlation"),
         "diagnostics preserve direction, source, semantic authority, scope, "
         "correlation, and safe errors");

  for (const UiEffect &effect : diagnostics) {
    const std::string rendered = diagnosticField(effect, "subject") +
                                 diagnosticField(effect, "error") +
                                 diagnosticField(effect, "threadId");
    expect(rendered.find("authored-name-must-not-appear") == std::string::npos,
           "diagnostics never copy an authored request payload");
  }

  RuntimeAction refresh{RuntimeActionKind::RefreshThreads};
  expect(sendAction(runtime.channels(), std::move(refresh)),
         "secret-error fixture enters the typed mailbox");
  const std::optional<nlohmann::json> secretRequest = bridge.receiveAppServer();
  expect(secretRequest &&
             secretRequest->value("method", std::string{}) == "thread/list",
         "secret-error fixture receives the direct request");
  if (secretRequest) {
    expect(secretRequest->at("params").value("useStateDbOnly", false),
           "the foreground diagnostic request uses the fast DB path");
    expect(bridge.replyError(*secretRequest, -32042,
                             "Bearer sk-runtime-secret eyJabc.def.ghi"),
           "secret-shaped error reaches the runtime");
    std::vector<UiEffect> secretDiagnostics;
    expect(waitUntil([&] {
             std::vector<UiEffect> batch = runtime.takeProtocolDiagnostics();
             secretDiagnostics.insert(secretDiagnostics.end(),
                                      std::make_move_iterator(batch.begin()),
                                      std::make_move_iterator(batch.end()));
             return std::ranges::any_of(
                 secretDiagnostics, [](const UiEffect &effect) {
                   return diagnosticField(effect, "direction") ==
                              "client error" &&
                          diagnosticField(effect, "subject") == "thread/list";
                 });
           }),
           "secret-shaped error produces bounded metadata");
    bool redactedSecretError = false;
    for (const UiEffect &effect : secretDiagnostics) {
      if (diagnosticField(effect, "subject") != "thread/list" ||
          diagnosticField(effect, "direction") != "client error")
        continue;
      redactedSecretError =
          diagnosticField(effect, "error") == "[redacted error detail]";
    }
    expect(redactedSecretError,
           "credential-shaped protocol error detail is redacted");
    const std::optional<nlohmann::json> repairRequest =
        bridge.receiveAppServer();
    expect(repairRequest &&
               repairRequest->value("method", std::string{}) == "thread/list" &&
               !repairRequest->at("params").value("useStateDbOnly", true),
           "a failed fast DB request still falls back to repair scanning");
    if (repairRequest)
      expect(bridge.reply(*repairRequest, {{"data", nlohmann::json::array()},
                                           {"nextCursor", nullptr}}),
             "repair fallback completes after the fast-path failure");
  }

  const auto readAuthority = [&](bool insertInterveningFrame) {
    runtime.drainNotifications();
    NodeAction reload{thread, NodeActionKind::Reload};
    expect(sendAction(runtime.channels(), std::move(reload)),
           "paginated diagnostic reload enters the typed mailbox");
    const std::optional<nlohmann::json> resumeRequest =
        bridge.receiveAppServer();
    expect(resumeRequest &&
               resumeRequest->value("method", std::string{}) ==
                   "thread/resume" &&
               resumeRequest->at("params").value("excludeTurns", false),
           "diagnostic reload establishes metadata-only live state first");
    if (!resumeRequest)
      return std::string{};
    expect(bridge.reply(*resumeRequest, nlohmann::json::object()),
           "metadata-only settings refresh response is delivered");
    const std::optional<nlohmann::json> readRequest = bridge.receiveAppServer();
    expect(readRequest && readRequest->value("method", std::string{}) ==
                              "thread/turns/list",
           "diagnostic reload pages history after metadata resume");
    if (!readRequest)
      return std::string{};
    expect(readRequest->at("params").value("itemsView", std::string{}) ==
               "summary",
           "thread hydration separates bounded turn summaries from items");
    if (insertInterveningFrame) {
      expect(bridge.appServerNotification(
                 "thread/name/updated",
                 {{"threadId", "runtime-thread"},
                  {"name", "Changed while pagination was pending"}}),
             "intervening provider delta is delivered before pagination");
    }
    expect(bridge.reply(*readRequest, {{"data", listedThread().at("turns")},
                                       {"nextCursor", nullptr}}),
           "turn summary diagnostic response is delivered");

    const ProtocolRequestId readOperation =
        requestIdFromJson(readRequest->at("id"));
    expect(waitUntil([&] {
             const std::optional<NodeGraph::ReadAccess> read =
                 runtime.graph().tryRead();
             if (!read)
               return false;
             const NodeRef current =
                 read->find({NodeKind::Thread, "runtime-thread"});
             if (!current)
               return false;
             const auto state = read->state(current);
             return !read->find(
                        {NodeKind::Operation, readOperation.canonical()}) &&
                    exactStringFromValue(
                        valueMember(*state, "hydrationState")) == "ready" &&
                    !valueMember(*state, "hydrationError");
           }),
           "the exact bridge turn page retires its Operation and makes the "
           "same thread ready without a synthetic failure");

    std::string authority;
    expect(waitUntil([&] {
             for (UiEffect &effect : runtime.takeProtocolDiagnostics()) {
               if (diagnosticField(effect, "direction") == "client result" &&
                   diagnosticField(effect, "subject") == "thread/turns/list")
                 authority = diagnosticField(effect, "authority");
             }
             return !authority.empty();
           }),
           "thread/turns/list result retains its diagnostic authority");
    return authority;
  };
  expect(readAuthority(true) == "merge",
         "an intervening provider frame retains paginated merge authority");
  expect(readAuthority(false) == "merge",
         "an immediately correlated page retains paginated merge authority");

  runtime.drainNotifications();
  expect(
      bridge.appServerNotification("skills/changed", nlohmann::json::object()),
      "state-neutral catalog invalidation is delivered");
  expect(bridge.appServerNotification("thread/goal/cleared",
                                      {{"threadId", "runtime-thread"}}),
         "authoritative removal notification is delivered");
  std::vector<UiEffect> notificationDiagnostics;
  expect(waitUntil([&] {
           std::vector<UiEffect> batch = runtime.takeProtocolDiagnostics();
           notificationDiagnostics.insert(
               notificationDiagnostics.end(),
               std::make_move_iterator(batch.begin()),
               std::make_move_iterator(batch.end()));
           return std::ranges::any_of(notificationDiagnostics,
                                      [](const UiEffect &effect) {
                                        return diagnosticField(effect,
                                                               "subject") ==
                                               "skills/changed";
                                      }) &&
                  std::ranges::any_of(
                      notificationDiagnostics, [](const UiEffect &effect) {
                        return diagnosticField(effect, "subject") ==
                               "thread/goal/cleared";
                      });
         }),
         "notification diagnostics preserve semantic authority");
  const auto authorityFor = [&](std::string_view subject) {
    const auto found = std::ranges::find_if(
        notificationDiagnostics, [subject](const UiEffect &effect) {
          return diagnosticField(effect, "subject") == subject;
        });
    return found == notificationDiagnostics.end()
               ? std::string{}
               : diagnosticField(*found, "authority");
  };
  expect(authorityFor("skills/changed") == "none" &&
             authorityFor("thread/goal/cleared") == "remove",
         "state-neutral and removal notifications remain distinguishable");

  runtime.drainNotifications();
  expect(bridge.appServerRequest("diagnostic-clock", "currentTime/read",
                                 {{"threadId", "runtime-thread"}}),
         "reverse-request diagnostic fixture is delivered");
  const std::optional<nlohmann::json> automaticClockResponse =
      bridge.receiveAppServer();
  expect(automaticClockResponse &&
             !automaticClockResponse->contains("method") &&
             automaticClockResponse->value("id", std::string{}) ==
                 "diagnostic-clock",
         "reverse-request diagnostic fixture consumes its automatic response");
  std::vector<UiEffect> reverseDiagnostics;
  expect(waitUntil([&] {
           std::vector<UiEffect> batch = runtime.takeProtocolDiagnostics();
           reverseDiagnostics.insert(reverseDiagnostics.end(),
                                     std::make_move_iterator(batch.begin()),
                                     std::make_move_iterator(batch.end()));
           return std::ranges::any_of(
               reverseDiagnostics, [](const UiEffect &effect) {
                 return diagnosticField(effect, "direction") ==
                            "server result" &&
                        diagnosticField(effect, "correlation") ==
                            "diagnostic-clock";
               });
         }),
         "reverse request and automatic response are both diagnosed");
  const UiEffect *reverseRequest = nullptr;
  const UiEffect *reverseResult = nullptr;
  for (const UiEffect &effect : reverseDiagnostics) {
    if (diagnosticField(effect, "correlation") != "diagnostic-clock")
      continue;
    if (diagnosticField(effect, "direction") == "server request")
      reverseRequest = &effect;
    if (diagnosticField(effect, "direction") == "server result")
      reverseResult = &effect;
  }
  expect(reverseRequest && reverseResult &&
             diagnosticField(*reverseRequest, "authority") == "merge" &&
             diagnosticField(*reverseResult, "authority") == "remove" &&
             diagnosticField(*reverseRequest, "threadId") == "runtime-thread" &&
             diagnosticField(*reverseRequest, "correlation") ==
                 diagnosticField(*reverseResult, "correlation"),
         "reverse interaction diagnostics preserve scope and correlation");

  runtime.drainNotifications();
  constexpr std::string_view SensitiveRequestId =
      "failed:sk-sensitive-correlation";
  expect(bridge.appServerRequest(std::string(SensitiveRequestId),
                                 "currentTime/read",
                                 {{"threadId", "runtime-thread"}}),
         "sensitive request-id diagnostic fixture is delivered");
  const std::optional<nlohmann::json> sensitiveClockResponse =
      bridge.receiveAppServer();
  expect(sensitiveClockResponse &&
             !sensitiveClockResponse->contains("method") &&
             sensitiveClockResponse->value("id", std::string{}) ==
                 SensitiveRequestId,
         "sensitive request-id fixture consumes its automatic response");
  std::vector<UiEffect> sensitiveIdDiagnostics;
  expect(waitUntil([&] {
           std::vector<UiEffect> batch = runtime.takeProtocolDiagnostics();
           sensitiveIdDiagnostics.insert(sensitiveIdDiagnostics.end(),
                                         std::make_move_iterator(batch.begin()),
                                         std::make_move_iterator(batch.end()));
           return std::ranges::any_of(
               sensitiveIdDiagnostics, [](const UiEffect &effect) {
                 return diagnosticField(effect, "direction") ==
                            "server result" &&
                        diagnosticField(effect, "correlation") ==
                            "<oversized-or-sensitive-id>";
               });
         }),
         "sensitive request ids remain visible only as redacted chronology");
  for (const UiEffect &effect : sensitiveIdDiagnostics) {
    std::string visibleMetadata;
    for (const auto &[key, value] : effect.details) {
      visibleMetadata += key;
      if (value.asString())
        visibleMetadata += *value.asString();
    }
    expect(visibleMetadata.find(SensitiveRequestId) == std::string::npos,
           "sensitive request ids are neither retained nor correlated");
  }
  runtime.drainNotifications();
}

void directNodeActionsUseOneCorrelatedRequest(UnixBridge &bridge,
                                              RunningRuntime &runtime) {
  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  expect(static_cast<bool>(thread), "provider hydration creates action target");
  if (!thread)
    return;

  NodeAction missingName{thread, NodeActionKind::Rename};
  missingName.correlation = "missing-name-action";
  expect(sendAction(runtime.channels(), std::move(missingName)),
         "a missing-name rename can enter the typed mailbox");
  expect(!bridge.receiveAppServer(100ms),
         "worker validation blocks a missing rename name");

  NodeAction emptyName{thread, NodeActionKind::Rename};
  emptyName.payload = {{"name", Value(" \t\n")}};
  emptyName.correlation = "blank-name-action";
  expect(sendAction(runtime.channels(), std::move(emptyName)),
         "a blank-name rename can enter the typed mailbox");
  expect(!bridge.receiveAppServer(100ms),
         "worker validation blocks an empty rename name");
  std::vector<UiEffect> localRejections;
  expect(waitUntil([&] {
           std::vector<UiEffect> batch = runtime.takeProtocolDiagnostics();
           localRejections.insert(localRejections.end(),
                                  std::make_move_iterator(batch.begin()),
                                  std::make_move_iterator(batch.end()));
           return std::ranges::count_if(
                      localRejections, [](const UiEffect &effect) {
                        return diagnosticField(effect, "direction") ==
                                   "local result" &&
                               diagnosticField(effect, "subject") ==
                                   "thread/name/set";
                      }) >= 2;
         }),
         "local validation failures reach the bounded Protocol chronology");
  const auto localRenameRejection = [&localRejections](
                                        std::string_view correlation) {
    return std::ranges::any_of(localRejections, [correlation](
                                                    const UiEffect &effect) {
      return diagnosticField(effect, "direction") == "local result" &&
             diagnosticField(effect, "subject") == "thread/name/set" &&
             diagnosticField(effect, "authority") == "none" &&
             diagnosticField(effect, "outcome") == "ERROR" &&
             diagnosticField(effect, "errorCategory") == "local-validation" &&
             diagnosticField(effect, "correlation") == correlation &&
             diagnosticField(effect, "threadId") == "runtime-thread" &&
             diagnosticField(effect, "targetId") == "runtime-thread" &&
             diagnosticField(effect, "error") ==
                 "A non-empty thread name is required";
    });
  };
  expect(localRenameRejection("missing-name-action") &&
             localRenameRejection("blank-name-action"),
         "local rejection diagnostics preserve action correlation and safe "
         "error metadata without sending a wire operation");

  NodeAction rename{thread, NodeActionKind::Rename};
  rename.payload = {{"name", Value("Renamed once")},
                    {"turnId", Value("payload-decoy-turn")},
                    {"itemId", Value("payload-decoy-item")}};
  expect(sendAction(runtime.channels(), std::move(rename)),
         "rename action enters the worker mailbox");
  std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(
      request && request->value("method", std::string{}) == "thread/name/set" &&
          request->at("params").value("threadId", std::string{}) ==
              "runtime-thread" &&
          request->at("params").value("name", std::string{}) == "Renamed once",
      "rename action emits one addressed CodexBridge request");
  if (!request)
    return;
  expect(waitUntil([&] {
           return operationPending(runtime.graph(), request->at("id"));
         }),
         "emitted request has one pending graph correlation");
  expect(operationTargets(runtime.graph(), request->at("id"), thread),
         "the operation retains the exact NodeRef selected by the UI action");
  {
    std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
    const NodeId decoyTurn =
        scopedTurnNodeId("runtime-thread", "payload-decoy-turn");
    expect(read && !read->find(decoyTurn) &&
               !read->find(scopedItemNodeId(decoyTurn, "payload-decoy-item")),
           "payload identifiers cannot synthesize a replacement operation "
           "target when an exact action target was supplied");
  }
  expect(!bridge.receiveAppServer(100ms),
         "non-idempotent rename is not dual-sent");
  expect(bridge.reply(*request, nlohmann::json::object()),
         "matching rename response is delivered");
  expect(waitUntil([&] {
           return operationRetired(runtime.graph(), request->at("id"));
         }),
         "matching response retires the exact pending operation");

  NodeAction archive{thread, NodeActionKind::Archive};
  expect(sendAction(runtime.channels(), std::move(archive)),
         "archive action enters the worker mailbox");
  request = bridge.receiveAppServer();
  expect(request &&
             request->value("method", std::string{}) == "thread/archive" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread",
         "archive action emits one addressed CodexBridge request");
  if (request) {
    expect(!bridge.receiveAppServer(100ms),
           "non-idempotent archive is not dual-sent");
    expect(bridge.reply(*request, nlohmann::json::object()),
           "archive response is delivered");
    expect(waitUntil([&] {
             return operationRetired(runtime.graph(), request->at("id"));
           }),
           "archive response correlates to and retires its operation");
  }
  runtime.drainNotifications();
}

void remainingUiCommandFamiliesUseExactWirePaths(UnixBridge &bridge,
                                                 RunningRuntime &runtime) {
  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  expect(static_cast<bool>(thread),
         "remaining UI command coverage has a stable thread target");
  if (!thread)
    return;

  NodeRef wrongHistoryTarget;
  {
    auto write = runtime.graph().write();
    NodeState wrongTargetState;
    wrongTargetState.fields = {{"type", Value("agentMessage")},
                               {"threadId", Value("runtime-thread")}};
    wrongHistoryTarget = write.upsert({NodeKind::Item, "wrong-history-target"},
                                      std::move(wrongTargetState));
    write.setField(thread, "historyNextCursor", Value("graph-history-cursor"));
    static_cast<void>(write.finish());
  }
  NodeAction wrongHistory{wrongHistoryTarget, NodeActionKind::LoadHistory};
  expect(sendAction(runtime.channels(), std::move(wrongHistory)),
         "a malformed history target reaches typed validation");
  expect(!bridge.receiveAppServer(100ms),
         "history loading rejects a non-Thread target before wire dispatch");

  NodeAction history{thread, NodeActionKind::LoadHistory};
  history.payload = {{"cursor", Value("payload-decoy-cursor")},
                     {"limit", Value(std::uint64_t{23})}};
  expect(sendAction(runtime.channels(), std::move(history)),
         "history paging enters the typed worker mailbox");
  std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(request &&
             request->value("method", std::string{}) == "thread/turns/list" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread" &&
             request->at("params").value("cursor", std::string{}) ==
                 "graph-history-cursor" &&
             request->at("params").value("limit", 0) == 80 &&
             request->at("params").value("sortDirection", std::string{}) ==
                 "desc" &&
             request->at("params").value("itemsView", std::string{}) ==
                 "summary",
         "Load More derives one scoped page from authoritative graph state");
  if (request) {
    expect(operationTargets(runtime.graph(), request->at("id"), thread),
           "history paging preserves its exact thread NodeRef");
    NodeAction duplicate{thread, NodeActionKind::LoadHistory};
    expect(sendAction(runtime.channels(), std::move(duplicate)),
           "a duplicate history action reaches the worker");
    expect(!bridge.receiveAppServer(100ms),
           "the live graph operation is the history single-flight gate");
    expect(bridge.replyError(*request, -32044, "temporary history failure"),
           "history paging exposes a retryable provider failure");
    expect(waitUntil([&] {
             return operationRetired(runtime.graph(), request->at("id"));
           }),
           "history paging failure retires its exact operation");

    NodeAction retry{thread, NodeActionKind::LoadHistory};
    retry.payload = {{"cursor", Value("different-decoy-cursor")},
                     {"limit", Value(std::uint64_t{7})}};
    expect(sendAction(runtime.channels(), std::move(retry)),
           "a failed history operation can be retried");
    request = bridge.receiveAppServer();
    expect(request &&
               request->value("method", std::string{}) == "thread/turns/list" &&
               request->at("params").value("cursor", std::string{}) ==
                   "graph-history-cursor" &&
               request->at("params").value("limit", 0) == 80,
           "history retry preserves the same authoritative page demand");
    if (request)
      expect(
          bridge.reply(*request,
                       {{"data", nlohmann::json::array(
                                     {{{"id", "paged-runtime-turn"},
                                       {"items", nlohmann::json::array()}}})},
                        {"nextCursor", nullptr}}),
          "the retried history page decodes its typed result");
    std::optional<nlohmann::json> itemPage = bridge.receiveAppServer();
    expect(itemPage &&
               itemPage->value("method", std::string{}) ==
                   "thread/items/list" &&
               itemPage->at("params").value("threadId", std::string{}) ==
                   "runtime-thread" &&
               itemPage->at("params").value("turnId", std::string{}) ==
                   "paged-runtime-turn" &&
               itemPage->at("params").value("limit", 0) == 80 &&
               itemPage->at("params").value("sortDirection", std::string{}) ==
                   "desc",
           "turn summaries schedule one bounded item page");
    if (itemPage)
      expect(bridge.reply(*itemPage, {{"data", nlohmann::json::array()},
                                      {"nextCursor", "older-items"}}),
             "item pagination retains its continuation");
    itemPage = bridge.receiveAppServer();
    expect(itemPage &&
               itemPage->value("method", std::string{}) ==
                   "thread/items/list" &&
               itemPage->at("params").value("cursor", std::string{}) ==
                   "older-items",
           "item pagination continues through the provider cursor");
    if (itemPage)
      expect(bridge.reply(*itemPage, {{"data", nlohmann::json::array()},
                                      {"nextCursor", nullptr}}),
             "the final item page completes bounded hydration");
  }
  expect(!bridge.receiveAppServer(100ms),
         "history paging is never duplicated on the wire");

  NodeAction fork{thread, NodeActionKind::Fork};
  expect(sendAction(runtime.channels(), std::move(fork)),
         "fork enters the typed worker mailbox");
  request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "thread/fork" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread" &&
             request->at("params").value("excludeTurns", false),
         "fork encodes one request addressed by the supplied NodeRef");
  if (request) {
    expect(operationTargets(runtime.graph(), request->at("id"), thread),
           "fork preserves its exact thread NodeRef");
    expect(bridge.replyError(*request, -32043, "focused fork rejection"),
           "fork decodes a typed provider error");
    expect(waitUntil([&] {
             return operationRetired(runtime.graph(), request->at("id"));
           }),
           "fork error retires its correlated operation");
  }
  expect(!bridge.receiveAppServer(100ms), "fork is never dual-sent");

  const auto archivedIs = [&](bool expected) {
    std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
    if (!read || read->find(thread->id()) != thread)
      return false;
    const auto state = read->state(thread);
    const auto archived = state->fields.find("archived");
    return archived != state->fields.end() && archived->second.asBool() &&
           *archived->second.asBool() == expected;
  };
  expect(bridge.appServerNotification("thread/archived",
                                      {{"threadId", "runtime-thread"}}) &&
             waitUntil([&] { return archivedIs(true); }),
         "unarchive coverage starts from authoritative archived state");
  NodeAction unarchive{thread, NodeActionKind::Unarchive};
  expect(sendAction(runtime.channels(), std::move(unarchive)),
         "unarchive enters the typed worker mailbox");
  request = bridge.receiveAppServer();
  expect(request &&
             request->value("method", std::string{}) == "thread/unarchive" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread",
         "unarchive encodes one request addressed by the supplied NodeRef");
  if (request) {
    expect(operationTargets(runtime.graph(), request->at("id"), thread),
           "unarchive preserves its exact thread NodeRef");
    expect(bridge.reply(*request, nlohmann::json::object()),
           "unarchive decodes its typed result");
    expect(waitUntil([&] {
             return operationRetired(runtime.graph(), request->at("id"));
           }),
           "unarchive result retires its correlated operation");
  }
  expect(!bridge.receiveAppServer(100ms), "unarchive is never dual-sent");
  expect(bridge.appServerNotification("thread/unarchived",
                                      {{"threadId", "runtime-thread"}}) &&
             waitUntil([&] { return archivedIs(false); }),
         "authoritative unarchive restores the thread for prompt coverage");

  RuntimeAction create{RuntimeActionKind::CreateThread};
  create.correlation = "wire-create-correlation";
  create.promptText = "Create and send exactly once";
  create.attachments.push_back(
      {"/tmp/wire-image.png", "wire-image.png", "image/png", std::nullopt});
  create.payload = {
      {"requestedName", Value("Unsupported ephemeral name")},
      {"threadStart",
       Value(Value::Object{
           {"cwd", Value("/tmp/wire-create")},
           {"baseInstructions", Value("Ephemeral base instructions")},
           {"developerInstructions", Value("Ephemeral developer instructions")},
           {"ephemeral", Value(true)}})},
      {"turnStart",
       Value(Value::Object{{"approvalPolicy", Value("on-request")}})}};
  expect(sendAction(runtime.channels(), std::move(create)),
         "new-thread prompt enters the typed worker mailbox");
  request = bridge.receiveAppServer();
  expect(
      request && request->value("method", std::string{}) == "thread/start" &&
          request->at("params").value("cwd", std::string{}) ==
              "/tmp/wire-create" &&
          request->at("params").value("baseInstructions", std::string{}) ==
              "Ephemeral base instructions" &&
          request->at("params").value("developerInstructions", std::string{}) ==
              "Ephemeral developer instructions" &&
          request->at("params").value("ephemeral", false) &&
          !request->at("params").contains("historyMode"),
      "Create Thread keeps ephemeral instructions and leaves app-server's "
      "legacy history default authoritative");
  if (!request) {
    runtime.drainNotifications();
    return;
  }
  expect(bridge.reply(*request, {{"thread",
                                  {{"id", "wire-created-thread"},
                                   {"name", "Wire created"},
                                   {"status", "idle"},
                                   {"turns", nlohmann::json::array()}}}}),
         "thread/start decodes the canonical created thread");

  request = bridge.receiveAppServer();
  const nlohmann::json input =
      request && request->contains("params")
          ? request->at("params").value("input", nlohmann::json::array())
          : nlohmann::json::array();
  expect(request && request->value("method", std::string{}) == "turn/start" &&
             request->at("params").value("threadId", std::string{}) ==
                 "wire-created-thread" &&
             request->at("params").value("approvalPolicy", std::string{}) ==
                 "on-request" &&
             input.size() == 2 &&
             input.at(0).value("type", std::string{}) == "text" &&
             input.at(0)
                     .value("text", std::string{})
                     .find("Create and send exactly once") !=
                 std::string::npos &&
             input.at(1).value("type", std::string{}) == "localImage" &&
             input.at(1).value("path", std::string{}) == "/tmp/wire-image.png",
         "the first ephemeral prompt skips metadata updates and moves text, "
         "options, and attachment into one turn/start request");
  if (request) {
    expect(bridge.reply(*request, {{"turn",
                                    {{"id", "wire-created-turn"},
                                     {"status", "inProgress"},
                                     {"items", nlohmann::json::array()}}}}),
           "turn/start decodes its canonical turn result");
  }
  expect(!bridge.receiveAppServer(100ms),
         "the first new-thread prompt is never dual-sent");

  const NodeRef created =
      findNode(runtime.graph(), {NodeKind::Thread, "wire-created-thread"});
  const NodeRef active =
      findNode(runtime.graph(),
               scopedTurnNodeId("wire-created-thread", "wire-created-turn"));
  expect(created && active,
         "created-thread command results retain natural thread and turn nodes");
  if (created && active) {
    NodeAction steer{created, NodeActionKind::SubmitPrompt};
    steer.promptText = "Steer the exact active turn";
    steer.payload.emplace("model", "must-not-steer");
    steer.payload.emplace("summary", "must-not-steer");
    expect(sendAction(runtime.channels(), std::move(steer)),
           "active-turn steering enters the typed worker mailbox");
    request = bridge.receiveAppServer();
    expect(request && request->value("method", std::string{}) == "turn/steer" &&
               request->at("params").value("threadId", std::string{}) ==
                   "wire-created-thread" &&
               request->at("params").value("expectedTurnId", std::string{}) ==
                   "wire-created-turn" &&
               !request->at("params").contains("model") &&
               !request->at("params").contains("summary"),
           "Submit while active encodes one turn/steer without start settings");
    if (request)
      expect(bridge.reply(*request, {{"turnId", "wire-created-turn"}}),
             "turn/steer decodes its typed result");
    expect(!bridge.receiveAppServer(100ms), "steering is never dual-sent");
  }

  expect(bridge.appServerNotification("turn/completed",
                                      {{"threadId", "wire-created-thread"},
                                       {"turn",
                                        {{"id", "wire-created-turn"},
                                         {"status", "completed"},
                                         {"items", nlohmann::json::array()}}}}),
         "created active turn receives its authoritative completion");

  if (created) {
    NodeAction remove{created, NodeActionKind::Delete};
    expect(sendAction(runtime.channels(), std::move(remove)),
           "delete enters the typed worker mailbox");
    request = bridge.receiveAppServer();
    expect(request &&
               request->value("method", std::string{}) == "thread/delete" &&
               request->at("params").value("threadId", std::string{}) ==
                   "wire-created-thread",
           "delete encodes one request addressed by the supplied NodeRef");
    if (request) {
      expect(operationTargets(runtime.graph(), request->at("id"), created),
             "delete preserves its exact thread NodeRef");
      expect(bridge.reply(*request, nlohmann::json::object()),
             "delete decodes its typed result");
      expect(waitUntil([&] {
               return operationRetired(runtime.graph(), request->at("id"));
             }),
             "delete result retires its correlated operation");
    }
    expect(!bridge.receiveAppServer(100ms), "delete is never dual-sent");
    expect(bridge.appServerNotification(
               "thread/deleted", {{"threadId", "wire-created-thread"}}) &&
               waitUntil([&] {
                 std::optional<NodeGraph::ReadAccess> read =
                     runtime.graph().tryRead();
                 return read && !read->find(created->id());
               }),
           "authoritative delete retires the exact created thread");
  }
  runtime.drainNotifications();
}

void itemHistoryHydrationHasAnExactConcurrencyCeiling(UnixBridge &bridge,
                                                      RunningRuntime &runtime) {
  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  expect(static_cast<bool>(thread),
         "bounded item hydration has a stable thread target");
  if (!thread)
    return;
  {
    auto write = runtime.graph().write();
    write.setField(thread, "historyNextCursor", Value("bounded-items-page"));
    static_cast<void>(write.finish());
  }
  expect(sendAction(runtime.channels(),
                    NodeAction{thread, NodeActionKind::LoadHistory}),
         "bounded item hydration enters the worker mailbox");
  const std::optional<nlohmann::json> turnPage = bridge.receiveAppServer();
  expect(turnPage &&
             turnPage->value("method", std::string{}) == "thread/turns/list",
         "bounded item hydration starts from one turn page");
  if (!turnPage)
    return;
  nlohmann::json turns = nlohmann::json::array();
  for (int index = 0; index < 10; ++index)
    turns.push_back({{"id", "bounded-turn-" + std::to_string(index)},
                     {"items", nlohmann::json::array()}});
  expect(bridge.reply(*turnPage,
                      {{"data", std::move(turns)}, {"nextCursor", nullptr}}),
         "ten turn summaries are delivered in one bounded page");

  std::vector<nlohmann::json> firstWave;
  for (int index = 0; index < 8; ++index) {
    const std::optional<nlohmann::json> request = bridge.receiveAppServer();
    expect(request &&
               request->value("method", std::string{}) == "thread/items/list",
           "the first item-hydration wave contains eight item pages");
    if (request)
      firstWave.push_back(*request);
  }
  expect(firstWave.size() == 8,
         "the native item scheduler fills its exact eight-request ceiling");
  expect(!bridge.receiveAppServer(100ms),
         "a ninth item page waits while eight requests remain in flight");
  if (firstWave.size() != 8)
    return;

  expect(bridge.reply(firstWave[0], {{"data", nlohmann::json::array()},
                                     {"nextCursor", nullptr}}),
         "one completed item page releases one scheduler slot");
  const std::optional<nlohmann::json> ninth = bridge.receiveAppServer();
  expect(ninth && ninth->value("method", std::string{}) == "thread/items/list",
         "one completion admits exactly the ninth item page");
  expect(bridge.reply(firstWave[1], {{"data", nlohmann::json::array()},
                                     {"nextCursor", nullptr}}),
         "a second completed item page releases the final queued turn");
  const std::optional<nlohmann::json> tenth = bridge.receiveAppServer();
  expect(tenth && tenth->value("method", std::string{}) == "thread/items/list",
         "the second completion admits exactly the tenth item page");
  for (std::size_t index = 2; index < firstWave.size(); ++index)
    expect(bridge.reply(firstWave[index], {{"data", nlohmann::json::array()},
                                           {"nextCursor", nullptr}}),
           "the remaining first-wave item page completes");
  if (ninth)
    expect(bridge.reply(*ninth, {{"data", nlohmann::json::array()},
                                 {"nextCursor", nullptr}}),
           "the ninth item page completes");
  if (tenth)
    expect(bridge.reply(*tenth, {{"data", nlohmann::json::array()},
                                 {"nextCursor", nullptr}}),
           "the tenth item page completes");
  expect(!bridge.receiveAppServer(100ms),
         "item hydration emits no extra pages after the ten turns complete");
  runtime.drainNotifications();
}

void conversationSnapshotsShareOneAdmissionAuthority(UnixBridge &bridge,
                                                     RunningRuntime &runtime) {
  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  expect(static_cast<bool>(thread),
         "snapshot single-flight coverage has a stable thread target");
  if (!thread)
    return;

  {
    auto write = runtime.graph().write();
    write.setField(thread, "historyHasMore", Value(true));
    write.setField(thread, "historyNextCursor", Value("read-overlap-cursor"));
    static_cast<void>(write.finish());
  }

  expect(sendAction(runtime.channels(),
                    NodeAction{thread, NodeActionKind::Reload}),
         "reload enters the worker before competing Load More coverage");
  const std::optional<nlohmann::json> resumeRequest = bridge.receiveAppServer();
  expect(resumeRequest &&
             resumeRequest->value("method", std::string{}) == "thread/resume" &&
             resumeRequest->at("params").value("excludeTurns", false),
         "reload owns one metadata-only resume before its history page");
  expect(sendAction(runtime.channels(),
                    NodeAction{thread, NodeActionKind::LoadHistory}),
         "Load More reaches admission while reload resume is pending");
  expect(!bridge.receiveAppServer(100ms),
         "reload resume suppresses a competing history page");
  if (resumeRequest) {
    expect(bridge.reply(*resumeRequest, nlohmann::json::object()),
           "metadata-only reload resume completes");
    const std::optional<nlohmann::json> readRequest = bridge.receiveAppServer();
    expect(readRequest &&
               readRequest->value("method", std::string{}) ==
                   "thread/turns/list" &&
               readRequest->at("params").value("itemsView", std::string{}) ==
                   "summary",
           "reload owns one bounded conversation snapshot");
    if (readRequest)
      expect(bridge.reply(*readRequest, {{"data", nlohmann::json::array()},
                                         {"nextCursor", nullptr}}),
             "the sole reload page completes");
  }

  {
    auto write = runtime.graph().write();
    write.setField(thread, "historyHasMore", Value(true));
    write.setField(thread, "historyNextCursor", Value("page-overlap-cursor"));
    static_cast<void>(write.finish());
  }
  expect(sendAction(runtime.channels(),
                    NodeAction{thread, NodeActionKind::LoadHistory}),
         "Load More enters the worker before competing reload coverage");
  const std::optional<nlohmann::json> pageRequest = bridge.receiveAppServer();
  expect(pageRequest &&
             pageRequest->value("method", std::string{}) == "thread/turns/list",
         "Load More owns one paginated conversation snapshot");
  expect(sendAction(runtime.channels(),
                    NodeAction{thread, NodeActionKind::Reload}),
         "reload reaches admission while thread/turns/list is pending");
  expect(!bridge.receiveAppServer(100ms),
         "thread/turns/list suppresses a competing reload snapshot");
  if (pageRequest)
    expect(bridge.reply(*pageRequest, {{"data", nlohmann::json::array()},
                                       {"nextCursor", nullptr}}),
           "the sole paginated snapshot completes");
  runtime.drainNotifications();
}

void staleHistoryFailureIsSemanticallyInert(UnixBridge &bridge,
                                            RunningRuntime &runtime) {
  constexpr std::string_view ThreadId = "stale-history-thread";
  constexpr std::string_view ErrorText =
      "late history failure must remain invisible";
  NodeRef thread;
  {
    auto write = runtime.graph().write();
    NodeState state;
    state.fields = {{"historyHasMore", Value(true)},
                    {"historyNextCursor", Value("stale-cursor")}};
    thread = write.upsert({NodeKind::Thread, std::string(ThreadId)},
                          std::move(state));
    static_cast<void>(write.finish());
  }
  static_cast<void>(runtime.takeUiEffects());

  NodeAction action{thread, NodeActionKind::LoadHistory};
  expect(sendAction(runtime.channels(), std::move(action)),
         "stale-history coverage admits one exact action");
  const std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(request &&
             request->value("method", std::string{}) == "thread/turns/list" &&
             operationTargets(runtime.graph(), request->at("id"), thread),
         "stale-history coverage observes the target-owned Operation");
  if (!request)
    return;

  {
    auto write = runtime.graph().write();
    write.remove(thread);
    static_cast<void>(write.finish());
  }
  expect(operationRetired(runtime.graph(), request->at("id")),
         "removing the history target cascades its pending Operation");
  static_cast<void>(runtime.takeUiEffects());
  expect(bridge.replyError(*request, -32055, std::string(ErrorText)),
         "the retired history request receives its late provider failure");
  expect(bridge.appServerNotification(
             "thread/started", {{"thread",
                                 {{"id", "stale-history-barrier"},
                                  {"name", "Stale history barrier"}}}}) &&
             waitUntil([&] {
               return static_cast<bool>(
                   findNode(runtime.graph(),
                            {NodeKind::Thread, "stale-history-barrier"}));
             }),
         "a later wire frame proves the late callback has settled");

  const std::vector<UiEffect> effects = runtime.takeUiEffects();
  expect(std::ranges::any_of(
             effects,
             [](const UiEffect &effect) {
               return effect.kind == UiEffectKind::ProtocolDiagnostic &&
                      diagnosticField(effect, "direction") == "client error" &&
                      diagnosticField(effect, "subject") == "thread/turns/list";
             }),
         "the late history failure remains visible to protocol diagnostics");
  expect(std::ranges::none_of(effects,
                              [&](const UiEffect &effect) {
                                return effect.kind ==
                                           UiEffectKind::ShowNotice &&
                                       effect.text.find(ErrorText) !=
                                           std::string::npos;
                              }),
         "a stale history failure emits no user-visible notice");
  expect(!findNode(runtime.graph(), {NodeKind::Thread, std::string(ThreadId)}),
         "the stale history result cannot recreate its retired thread");
}

void firstPromptAfterForkStartsANewTurn(UnixBridge &bridge,
                                        RunningRuntime &runtime) {
  const NodeRef source =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  expect(static_cast<bool>(source),
         "successful fork coverage has a stable source thread");
  if (!source)
    return;

  NodeAction fork{source, NodeActionKind::Fork};
  fork.payload = {{"requestedName", Value("Runtime thread (fork 1)")},
                  {"cwd", Value("/fork-workspace")},
                  {"baseInstructions", Value("Fork base")},
                  {"developerInstructions", Value("Fork developer")},
                  {"ephemeral", Value(true)}};
  expect(sendAction(runtime.channels(), std::move(fork)),
         "successful fork enters the typed worker mailbox");
  std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "thread/fork" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread" &&
             request->at("params").value("cwd", std::string{}) ==
                 "/fork-workspace" &&
             request->at("params").value("baseInstructions", std::string{}) ==
                 "Fork base" &&
             request->at("params").value("developerInstructions",
                                         std::string{}) == "Fork developer" &&
             request->at("params").value("ephemeral", false) &&
             request->at("params").value("excludeTurns", false) &&
             !request->at("params").contains("requestedName") &&
             !request->at("params").contains("name"),
         "successful fork sends adjustable options but keeps its chosen name "
         "out of thread/fork");
  if (!request)
    return;
  const nlohmann::json forkedThread{{"id", "runtime-fork"},
                                    {"name", "Runtime fork"},
                                    {"forkedFromId", "runtime-thread"},
                                    {"status", "idle"},
                                    {"createdAt", 3},
                                    {"updatedAt", 4},
                                    {"recencyAt", 4},
                                    {"turns", nlohmann::json::array()}};
  expect(bridge.reply(*request, {{"thread", forkedThread}}),
         "successful fork result is delivered");

  NodeRef forked;
  expect(waitUntil([&] {
           forked =
               findNode(runtime.graph(), {NodeKind::Thread, "runtime-fork"});
           if (!forked)
             return false;
           auto read = runtime.graph().tryRead();
           if (!read)
             return false;
           const auto state = read->state(forked);
           const auto overlay = state->fields.find("localNameOverlay");
           return overlay != state->fields.end() &&
                  overlay->second.asString() &&
                  *overlay->second.asString() == "Runtime thread (fork 1)";
         }),
         "successful fork immediately applies its chosen local name");
  expect(static_cast<bool>(forked),
         "successful fork retains its authoritative thread node");
  if (!forked)
    return;
  const std::optional<nlohmann::json> historyPage = bridge.receiveAppServer();
  expect(historyPage &&
             historyPage->value("method", std::string{}) ==
                 "thread/turns/list" &&
             historyPage->at("params").value("threadId", std::string{}) ==
                 "runtime-fork" &&
             historyPage->at("params").value("itemsView", std::string{}) ==
                 "summary",
         "metadata-only fork hydrates copied history through bounded pages");
  NodeAction prompt{forked, NodeActionKind::SubmitPrompt};
  prompt.promptText = "Answer after a successful fork";
  expect(sendAction(runtime.channels(), std::move(prompt)),
         "the first fork prompt enters the typed worker mailbox");
  request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "turn/start" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-fork",
         "the first fork prompt starts immediately without a redundant read or "
         "resume");
  if (!request)
    return;
  expect(bridge.reply(*request, {{"turn",
                                  {{"id", "runtime-fork-turn"},
                                   {"status", "inProgress"},
                                   {"items", nlohmann::json::array()}}}}),
         "the first fork prompt acknowledgement is delivered");
  if (historyPage)
    expect(bridge.reply(*historyPage, {{"data", nlohmann::json::array()},
                                       {"nextCursor", nullptr}}),
           "fork history can complete after its first prompt starts");
  expect(bridge.appServerNotification("turn/completed",
                                      {{"threadId", "runtime-fork"},
                                       {"turn",
                                        {{"id", "runtime-fork-turn"},
                                         {"status", "completed"},
                                         {"items", nlohmann::json::array()}}}}),
         "the fork turn reaches an authoritative terminal state");
  runtime.drainNotifications();
}

void ordinaryThreadPromptStillStartsAndCompletes(UnixBridge &bridge,
                                                 RunningRuntime &runtime) {
  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  expect(static_cast<bool>(thread),
         "ordinary prompt coverage has a stable thread");
  if (!thread)
    return;

  {
    auto write = runtime.graph().write();
    write.setStatus(thread, NodeStatus::Completed);
    write.setField(thread, "status", Value("notLoaded"));
    write.setField(thread, "hydrationState", Value("ready"));
    static_cast<void>(write.finish());
  }
  NodeAction prompt{thread, NodeActionKind::SubmitPrompt};
  prompt.promptText = "Answer an ordinary thread prompt";
  expect(sendAction(runtime.channels(), std::move(prompt)),
         "an ordinary prompt enters the typed worker mailbox");
  std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "turn/start" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread",
         "typed completed state overrides conflicting raw notLoaded text");
  if (!request)
    return;
  expect(bridge.reply(*request, {{"turn",
                                  {{"id", "ordinary-runtime-turn"},
                                   {"status", "inProgress"},
                                   {"items", nlohmann::json::array()}}}}),
         "the ordinary prompt acknowledgement is delivered");
  expect(bridge.appServerNotification("turn/completed",
                                      {{"threadId", "runtime-thread"},
                                       {"turn",
                                        {{"id", "ordinary-runtime-turn"},
                                         {"status", "completed"},
                                         {"items", nlohmann::json::array()}}}}),
         "the ordinary prompt reaches an authoritative terminal state");
  runtime.drainNotifications();

  const auto activeTurnCleared = [&] {
    const std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
    return read && read->related(thread, RelationKind::ActiveTurn).empty();
  };
  expect(waitUntil(activeTurnCleared),
         "the ordinary prompt releases its active-turn relation");

  {
    auto write = runtime.graph().write();
    write.setStatus(thread, NodeStatus::NotLoaded);
    write.setField(thread, "status", Value("completed"));
    write.setField(thread, "hydrationState", Value("ready"));
    static_cast<void>(write.finish());
  }
  NodeAction resumedPrompt{thread, NodeActionKind::SubmitPrompt};
  resumedPrompt.promptText = "Resume from typed thread state";
  expect(sendAction(runtime.channels(), std::move(resumedPrompt)),
         "a typed not-loaded prompt enters the worker mailbox");
  request = bridge.receiveAppServer();
  expect(request &&
             request->value("method", std::string{}) == "thread/resume" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread" &&
             request->at("params").value("excludeTurns", false),
         "typed not-loaded state overrides conflicting raw completed text");
  if (request)
    expect(bridge.reply(*request, nlohmann::json::object()),
           "the typed-state resume acknowledgement is delivered");
  request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "turn/start" &&
             request->at("params").value("threadId", std::string{}) ==
                 "runtime-thread",
         "the resumed prompt continues through one turn/start");
  if (request)
    expect(bridge.reply(*request, {{"turn",
                                    {{"id", "resumed-runtime-turn"},
                                     {"status", "inProgress"},
                                     {"items", nlohmann::json::array()}}}}),
           "the resumed prompt acknowledgement is delivered");
  expect(bridge.appServerNotification("turn/completed",
                                      {{"threadId", "runtime-thread"},
                                       {"turn",
                                        {{"id", "resumed-runtime-turn"},
                                         {"status", "completed"},
                                         {"items", nlohmann::json::array()}}}}),
         "the resumed prompt reaches an authoritative terminal state");
  runtime.drainNotifications();
  expect(waitUntil(activeTurnCleared),
         "the resumed prompt releases its active-turn relation");
  {
    auto write = runtime.graph().write();
    write.setStatus(thread, NodeStatus::Completed);
    write.setField(thread, "status", Value("idle"));
    static_cast<void>(write.finish());
  }
}

void runtimeRefreshActionsHaveExactRequestCardinality(UnixBridge &bridge,
                                                      RunningRuntime &runtime) {
  RuntimeAction refresh{RuntimeActionKind::RefreshThreads};
  refresh.payload = {{"limit", Value(std::uint64_t{17})}};
  expect(sendAction(runtime.channels(), std::move(refresh)),
         "thread refresh enters the worker mailbox");
  std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "thread/list" &&
             request->at("params").value("limit", 0) == 100 &&
             request->at("params").value("sortKey", std::string{}) ==
                 "recency_at" &&
             request->at("params").value("sortDirection", std::string{}) ==
                 "desc" &&
             request->at("params").value("useStateDbOnly", false) &&
             !request->at("params").contains("cursor"),
         "thread refresh emits the fast DB-only Recent first page");
  if (request)
    expect(bridge.reply(*request,
                        {{"data", nlohmann::json::array({listedThread()})},
                         {"nextCursor", "discarded-fast-cursor"}}),
           "fast thread refresh page is delivered immediately");
  request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "thread/list" &&
             request->at("params").value("limit", 0) == 100 &&
             request->at("params").value("sortKey", std::string{}) ==
                 "recency_at" &&
             request->at("params").value("sortDirection", std::string{}) ==
                 "desc" &&
             !request->at("params").value("useStateDbOnly", true) &&
             !request->at("params").contains("cursor"),
         "thread refresh follows with one scan-and-repair first page");
  if (request)
    expect(bridge.reply(*request,
                        {{"data", nlohmann::json::array({listedThread()})},
                         {"nextCursor", "recent-page-2"}}),
           "thread repair page establishes the reconciled pagination cursor");
  RuntimeAction loadMore{RuntimeActionKind::LoadMoreThreads};
  expect(sendAction(runtime.channels(), std::move(loadMore)),
         "scroll pagination enters the worker mailbox");
  request = bridge.receiveAppServer();
  expect(
      request && request->value("method", std::string{}) == "thread/list" &&
          request->at("params").value("limit", 0) == 100 &&
          request->at("params").value("sortKey", std::string{}) ==
              "recency_at" &&
          request->at("params").value("sortDirection", std::string{}) ==
              "desc" &&
          request->at("params").value("useStateDbOnly", false) &&
          request->at("params").value("cursor", std::string{}) ==
              "recent-page-2",
      "scroll pagination follows the repaired cursor through the DB-only path");
  if (request)
    expect(bridge.replyError(*request, -32000, "temporary paging failure"),
           "a transient page failure is delivered");
  RuntimeAction retryLoadMore{RuntimeActionKind::LoadMoreThreads};
  expect(sendAction(runtime.channels(), std::move(retryLoadMore)),
         "a failed page cursor can be retried");
  request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "thread/list" &&
             request->at("params").value("cursor", std::string{}) ==
                 "recent-page-2",
         "retry preserves the failed page cursor");
  if (request)
    expect(bridge.reply(*request, {{"data", nlohmann::json::array()},
                                   {"nextCursor", nullptr}}),
           "the retried additional page is delivered");
  expect(!bridge.receiveAppServer(100ms),
         "pagination stops after the requested final page");

  RuntimeAction catalogs{RuntimeActionKind::RefreshCatalogs};
  catalogs.payload = {{"cwd", Value("/tmp/runtime-dispatch")}};
  expect(sendAction(runtime.channels(), std::move(catalogs)),
         "catalog refresh enters the worker mailbox");
  std::unordered_map<std::string, std::size_t> methods;
  for (int count = 0; count != 2; ++count) {
    request = bridge.receiveAppServer();
    if (!request)
      break;
    ++methods[request->value("method", std::string{})];
    expect(request->at("params").value("cwd", std::string{}) ==
               "/tmp/runtime-dispatch",
           "catalog refresh preserves the newly authored payload");
    expect(bridge.reply(*request, {{"data", nlohmann::json::array()}}),
           "catalog response is delivered");
  }
  expect(methods ==
             std::unordered_map<std::string, std::size_t>{
                 {"model/list", 1}, {"permissionProfile/list", 1}},
         "catalog refresh emits one request for each concrete catalog");
  expect(!bridge.receiveAppServer(100ms),
         "catalog operations are not duplicated");
  runtime.drainNotifications();
}

void failedWakeUsesBoundedWorkerRecovery(UnixBridge &bridge,
                                         RunningRuntime &runtime) {
  RuntimeAction refresh{RuntimeActionKind::RefreshThreads};
  refresh.payload = {{"limit", Value(std::uint64_t{9})}};
  runtime.channels().failNextQtToWorkerWakeForTest();
  const ChannelSendStatus status =
      runtime.channels().sendRuntimeAction(refresh);
  expect(status == ChannelSendStatus::AcceptedWakeFailed &&
             deliveryGuaranteed(status) && wakeFailed(status) &&
             runtime.channels().drainQtToWorkerWake().status ==
                 EventFd::DrainStatus::Empty,
         "a failed Qt wake leaves one non-retryable action for timeout "
         "delivery");

  std::optional<nlohmann::json> request = bridge.receiveAppServer(1500ms);
  expect(request && request->value("method", std::string{}) == "thread/list" &&
             request->at("params").value("limit", 0) == 100 &&
             request->at("params").value("sortKey", std::string{}) ==
                 "recency_at" &&
             request->at("params").value("sortDirection", std::string{}) ==
                 "desc" &&
             request->at("params").value("useStateDbOnly", false),
         "the existing worker thread consumes an unwoken action within its "
         "bounded recovery interval using fixed Recent ordering");
  if (request)
    expect(bridge.reply(*request,
                        {{"data", nlohmann::json::array({listedThread()})},
                         {"nextCursor", nullptr}}),
           "the timeout-delivered action completes normally");
  request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "thread/list" &&
             !request->at("params").value("useStateDbOnly", true),
         "the admitted refresh schedules exactly one repair stage");
  if (request)
    expect(bridge.reply(*request,
                        {{"data", nlohmann::json::array({listedThread()})},
                         {"nextCursor", nullptr}}),
           "the repair stage completes after wake recovery");
  expect(!bridge.receiveAppServer(150ms),
         "wake recovery never duplicates either loading stage");
  runtime.drainNotifications();
}

void idleWorkerSleepsBetweenWakeRecoveryChecks(RunningRuntime &runtime) {
  const auto before = runtime.workerCpuTime();
  std::this_thread::sleep_for(350ms);
  const auto after = runtime.workerCpuTime();
  expect(before && after && *after >= *before && *after - *before < 100ms,
         "an idle worker rearms its mailbox timeout instead of zero-timeout "
         "polling");
}

void reverseInteractionsRespondOnceWithAuthoredData(UnixBridge &bridge,
                                                    RunningRuntime &runtime) {
  expect(bridge.appServerRequest("approval-runtime",
                                 "item/commandExecution/requestApproval",
                                 {{"threadId", "runtime-thread"},
                                  {"turnId", "runtime-turn"},
                                  {"itemId", "runtime-command"},
                                  {"command", "printf runtime"},
                                  {"cwd", "/tmp"}}),
         "approval request reaches CodexBridge");
  const NodeRef approval = findNode(
      runtime.graph(), {NodeKind::Interaction,
                        ProtocolRequestId("approval-runtime").canonical()});
  expect(static_cast<bool>(approval),
         "reverse approval is represented by its stable interaction node");
  if (approval) {
    NodeAction resolve{approval, NodeActionKind::ResolveInteraction};
    resolve.payload = {{"choice", Value("accept")}};
    expect(sendAction(runtime.channels(), std::move(resolve)),
           "approval response action enters the worker mailbox");
    const std::optional<nlohmann::json> response = bridge.receiveAppServer();
    expect(response && !response->contains("method") &&
               response->value("id", std::string{}) == "approval-runtime" &&
               response->at("result").value("decision", std::string{}) ==
                   "accept",
           "approval action emits one typed response with the original id");
    expect(!bridge.receiveAppServer(100ms),
           "approval response is never dual-sent");
    expect(waitUntil([&] {
             std::optional<NodeGraph::ReadAccess> read =
                 runtime.graph().tryRead();
             return read &&
                    !read->find(
                        {NodeKind::Interaction,
                         ProtocolRequestId("approval-runtime").canonical()});
           }),
           "accepted response retires the exact interaction");
  }

  expect(bridge.appServerRequest(
             "input-runtime", "item/tool/requestUserInput",
             {{"threadId", "runtime-thread"},
              {"turnId", "runtime-turn"},
              {"itemId", "runtime-question"},
              {"isBlocking", true},
              {"questions", userInputQuestions("question-1")}}),
         "user-input request reaches CodexBridge");
  const NodeRef input = findNode(
      runtime.graph(),
      {NodeKind::Interaction, ProtocolRequestId("input-runtime").canonical()});
  expect(static_cast<bool>(input),
         "reverse user input is represented by its interaction node");
  if (input) {
    NodeAction answer{input, NodeActionKind::ResolveInteraction};
    answer.payload = userInputSubmission("question-1", "yes");
    expect(sendAction(runtime.channels(), std::move(answer)),
           "user-input response action enters the worker mailbox");
    const std::optional<nlohmann::json> response = bridge.receiveAppServer();
    expect(response &&
               response->value("id", std::string{}) == "input-runtime" &&
               response->at("result")
                       .at("answers")
                       .at("question-1")
                       .at("answers") == nlohmann::json::array({"yes"}),
           "user-input response moves only newly authored answers to the wire");
    expect(!bridge.receiveAppServer(100ms),
           "user-input response is never dual-sent");
    expect(
        waitUntil([&] { return interactionRetired(runtime.graph(), input); }),
        "user-input response removes the exact interaction once");
  }
  runtime.drainNotifications();
}

void invalidAuthoredReverseResponseNeverReachesWire(UnixBridge &bridge,
                                                    RunningRuntime &runtime) {
  expect(bridge.appServerRequest(
             "invalid-input-runtime", "item/tool/requestUserInput",
             {{"threadId", "runtime-thread"},
              {"turnId", "runtime-turn"},
              {"itemId", "invalid-runtime-question"},
              {"isBlocking", true},
              {"questions", userInputQuestions("required-question")}}),
         "invalid-submission fixture reaches CodexBridge");
  const NodeRef interaction =
      findNode(runtime.graph(),
               {NodeKind::Interaction,
                ProtocolRequestId("invalid-input-runtime").canonical()});
  expect(static_cast<bool>(interaction),
         "invalid-submission fixture has one exact interaction");
  if (!interaction)
    return;

  const Value::Object malformed =
      userInputSubmission("unexpected-question", "must remain local");
  NodeAction invalid{interaction, NodeActionKind::ResolveInteraction};
  invalid.payload = malformed;
  expect(sendAction(runtime.channels(), std::move(invalid)),
         "malformed authored response enters the typed worker mailbox");
  expect(!bridge.receiveAppServer(200ms),
         "malformed authored response never reaches the wire");
  expect(
      waitUntil([&] {
        std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
        if (!read || read->find(interaction->id()) != interaction)
          return false;
        const auto state = read->state(interaction);
        const auto retained = state->fields.find("retainedResponsePayload");
        const auto revision = state->fields.find("responseRevision");
        const auto error = state->fields.find("error");
        return state->status == NodeStatus::Failed &&
               error != state->fields.end() && error->second.asString() &&
               *error->second.asString() ==
                   "The authored pending response is invalid." &&
               retained != state->fields.end() && retained->second.asObject() &&
               *retained->second.asObject() == malformed &&
               revision != state->fields.end() && revision->second.asUInt64() &&
               *revision->second.asUInt64() != 0;
      }),
      "invalid submission remains exact, recoverable, and authoritatively "
      "acknowledged");

  NodeAction corrected{interaction, NodeActionKind::ResolveInteraction};
  corrected.payload =
      userInputSubmission("required-question", "corrected answer");
  expect(sendAction(runtime.channels(), std::move(corrected)),
         "a deliberate corrected response can reuse the exact interaction");
  const std::optional<nlohmann::json> response = bridge.receiveAppServer();
  expect(response &&
             response->value("id", std::string{}) == "invalid-input-runtime" &&
             response->at("result")
                     .at("answers")
                     .at("required-question")
                     .at("answers") ==
                 nlohmann::json::array({"corrected answer"}) &&
             waitUntil([&] {
               return interactionRetired(runtime.graph(), interaction);
             }),
         "the corrected authored response is sent once and retires the exact "
         "interaction");
  expect(!bridge.receiveAppServer(100ms),
         "invalid-response recovery never duplicates the wire response");
  runtime.drainNotifications();
}

void workerRevalidatesCurrentAuthorityAndRetainsResponses(
    UnixBridge &bridge, RunningRuntime &runtime) {
  const auto graphRoleIs = [&](std::string_view expected) {
    std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
    if (!read)
      return false;
    const NodeRef connection = read->find({NodeKind::Connection, "connection"});
    if (!connection)
      return false;
    const auto state = read->state(connection);
    const auto role = state->fields.find("role");
    return role != state->fields.end() && role->second.asString() &&
           *role->second.asString() == expected;
  };

  expect(bridge.setRole("observer", "another-connection") &&
             waitUntil([&] { return graphRoleIs("observer"); }),
         "runtime enters observer role before receiving a reverse request");
  expect(
      bridge.appServerRequest("observer-input", "item/tool/requestUserInput",
                              {{"threadId", "runtime-thread"},
                               {"turnId", "observer-turn"},
                               {"itemId", "observer-question"},
                               {"isBlocking", true},
                               {"questions", userInputQuestions("question")}},
                              "observer"),
      "observer-delivered reverse request reaches the graph");
  const NodeRef observerInput = findNode(
      runtime.graph(),
      {NodeKind::Interaction, ProtocolRequestId("observer-input").canonical()});
  expect(static_cast<bool>(observerInput),
         "observer-delivered request has one stable interaction");

  expect(bridge.setRole("controller", "runtime-test") &&
             waitUntil([&] { return graphRoleIs("controller"); }),
         "the same connection can subsequently claim controller");
  if (observerInput) {
    NodeAction response{observerInput, NodeActionKind::ResolveInteraction};
    response.payload = userInputSubmission("question", "claimed");
    expect(sendAction(runtime.channels(), std::move(response)),
           "new controller admits the earlier observer request response");
    const std::optional<nlohmann::json> wire = bridge.receiveAppServer();
    expect(wire && wire->value("id", std::string{}) == "observer-input" &&
               wire->at("result").at("answers").at("question").at("answers") ==
                   nlohmann::json::array({"claimed"}),
           "current controller role, not receipt-time role, authorizes the "
           "exact response");
  }

  expect(bridge.appServerRequest(
             "lost-control-input", "item/tool/requestUserInput",
             {{"threadId", "runtime-thread"},
              {"turnId", "lost-control-turn"},
              {"itemId", "lost-control-question"},
              {"isBlocking", true},
              {"questions", userInputQuestions("question")}}),
         "second reverse request arrives while controlled");
  const NodeRef lostControl = findNode(
      runtime.graph(), {NodeKind::Interaction,
                        ProtocolRequestId("lost-control-input").canonical()});
  expect(bridge.setRole("observer", "another-connection") &&
             waitUntil([&] { return graphRoleIs("observer"); }),
         "controller loss is visible before queued action consumption");
  if (lostControl) {
    NodeAction rejected{lostControl, NodeActionKind::ResolveInteraction};
    rejected.payload = userInputSubmission("question", "preserved");
    expect(sendAction(runtime.channels(), std::move(rejected)),
           "stale Qt response still enters the typed mailbox");
    expect(!bridge.receiveAppServer(200ms),
           "worker revalidation prevents a response after controller loss");
    expect(waitUntil([&] {
             std::optional<NodeGraph::ReadAccess> read =
                 runtime.graph().tryRead();
             if (!read)
               return false;
             const NodeRef current = read->find(lostControl->id());
             if (current != lostControl ||
                 read->state(current)->status != NodeStatus::Failed)
               return false;
             const auto state = read->state(current);
             const auto retained =
                 state->fields.find("retainedResponsePayload");
             return retained != state->fields.end() &&
                    retained->second.asObject() &&
                    *retained->second.asObject() ==
                        userInputSubmission("question", "preserved");
           }),
           "worker rejection keeps the authored answer on the actionable "
           "interaction for manual recovery");
  }

  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  if (thread) {
    NodeAction rename{thread, NodeActionKind::Rename};
    rename.payload = {{"name", Value("must not leave observer")}};
    expect(sendAction(runtime.channels(), std::move(rename)),
           "observer thread mutation enters the typed mailbox");
    expect(!bridge.receiveAppServer(200ms),
           "worker current-role validation blocks provider mutations");
  }
  expect(bridge.setRole("controller", "runtime-test") &&
             waitUntil([&] { return graphRoleIs("controller"); }),
         "runtime restores controller for remaining coverage");
  if (lostControl) {
    NodeAction retry{lostControl, NodeActionKind::ResolveInteraction};
    retry.payload = userInputSubmission("question", "preserved");
    expect(sendAction(runtime.channels(), std::move(retry)),
           "the user can explicitly resubmit the preserved response");
    const std::optional<nlohmann::json> wire = bridge.receiveAppServer();
    expect(wire && wire->value("id", std::string{}) == "lost-control-input" &&
               wire->at("result").at("answers").at("question").at("answers") ==
                   nlohmann::json::array({"preserved"}) &&
               waitUntil([&] {
                 return interactionRetired(runtime.graph(), lostControl);
               }),
           "manual recovery sends exactly once after control is restored");
  }

  expect(bridge.appServerNotification(
             "turn/started",
             {{"threadId", "runtime-thread"},
              {"turn",
               {{"id", "already-finished-turn"}, {"status", "inProgress"}}}}),
         "active turn fixture reaches the worker");
  const NodeRef finishedTurn =
      findNode(runtime.graph(),
               scopedTurnNodeId("runtime-thread", "already-finished-turn"));
  expect(bridge.appServerNotification(
             "turn/completed",
             {{"threadId", "runtime-thread"},
              {"turn",
               {{"id", "already-finished-turn"}, {"status", "completed"}}}}) &&
             waitUntil([&] {
               std::optional<NodeGraph::ReadAccess> read =
                   runtime.graph().tryRead();
               return read && finishedTurn &&
                      read->state(finishedTurn)->status ==
                          NodeStatus::Completed;
             }),
         "turn fixture is terminal before the stale stop action");
  if (finishedTurn) {
    NodeAction stop{finishedTurn, NodeActionKind::InterruptTurn};
    expect(sendAction(runtime.channels(), std::move(stop)),
           "stale stop enters the typed mailbox");
    expect(!bridge.receiveAppServer(200ms),
           "worker revalidation blocks interrupt for a non-active turn");
  }

  const auto threadArchived = [&](bool expected) {
    std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
    if (!read || !thread)
      return false;
    const NodeRef current = read->find(thread->id());
    if (current != thread)
      return false;
    const auto state = read->state(current);
    const auto archived = state->fields.find("archived");
    return archived != state->fields.end() && archived->second.asBool() &&
           *archived->second.asBool() == expected;
  };
  expect(thread &&
             bridge.appServerNotification("thread/archived",
                                          {{"threadId", "runtime-thread"}}) &&
             waitUntil([&] { return threadArchived(true); }),
         "the worker observes an authoritative archive before a queued "
         "duplicate action");
  if (thread) {
    NodeAction duplicateArchive{thread, NodeActionKind::Archive};
    expect(sendAction(runtime.channels(), std::move(duplicateArchive)),
           "a now-stale archive action still enters the typed mailbox");
    expect(!bridge.receiveAppServer(200ms),
           "worker lifecycle revalidation blocks duplicate archive");
  }

  expect(bridge.appServerNotification(
             "turn/started",
             {{"threadId", "runtime-thread"},
              {"turn",
               {{"id", "archived-active-turn"}, {"status", "inProgress"}}}}),
         "an out-of-order active turn can coexist with archived thread state");
  const NodeRef archivedActiveTurn =
      findNode(runtime.graph(),
               scopedTurnNodeId("runtime-thread", "archived-active-turn"));
  if (archivedActiveTurn) {
    NodeAction stop{archivedActiveTurn, NodeActionKind::InterruptTurn};
    stop.payload = {{"turnId", Value("stale-payload-turn")}};
    expect(sendAction(runtime.channels(), std::move(stop)),
           "an archived-thread stop enters the typed mailbox");
    expect(!bridge.receiveAppServer(200ms),
           "worker revalidation prevents archived active state from reaching "
           "CodexBridge");
  }

  expect(bridge.appServerNotification("thread/unarchived",
                                      {{"threadId", "runtime-thread"}}) &&
             waitUntil([&] { return threadArchived(false); }),
         "the authoritative thread returns to unarchived state");
  if (thread) {
    NodeAction duplicateUnarchive{thread, NodeActionKind::Unarchive};
    expect(sendAction(runtime.channels(), std::move(duplicateUnarchive)),
           "a now-stale unarchive action still enters the typed mailbox");
    expect(!bridge.receiveAppServer(200ms),
           "worker lifecycle revalidation blocks duplicate unarchive");
  }
  if (archivedActiveTurn) {
    NodeAction stop{archivedActiveTurn, NodeActionKind::InterruptTurn};
    stop.payload = {{"threadId", Value("stale-payload-thread")},
                    {"turnId", Value("stale-payload-turn")}};
    expect(sendAction(runtime.channels(), std::move(stop)),
           "the current unarchived active turn admits a stop action");
    const std::optional<nlohmann::json> request = bridge.receiveAppServer();
    expect(request &&
               request->value("method", std::string{}) == "turn/interrupt" &&
               request->at("params").value("threadId", std::string{}) ==
                   "runtime-thread" &&
               request->at("params").value("turnId", std::string{}) ==
                   "archived-active-turn",
           "worker addressing replaces stale payload ids with the current "
           "canonical active-turn address");
    if (request)
      expect(bridge.reply(*request, nlohmann::json::object()),
             "the revalidated interrupt response reaches the worker");
  }

  expect(bridge.appServerNotification("thread/started",
                                      {{"thread",
                                        {{"id", "removed-action-thread"},
                                         {"name", "Removed before dispatch"},
                                         {"cwd", "/tmp"}}}}),
         "stale thread-action fixture reaches the graph");
  const NodeRef removedActionThread =
      findNode(runtime.graph(), {NodeKind::Thread, "removed-action-thread"});
  expect(removedActionThread &&
             bridge.appServerNotification(
                 "thread/deleted", {{"threadId", "removed-action-thread"}}) &&
             waitUntil([&] {
               std::optional<NodeGraph::ReadAccess> read =
                   runtime.graph().tryRead();
               return read && !read->find(removedActionThread->id());
             }),
         "the worker retires the exact action target before dispatch");
  if (removedActionThread) {
    NodeAction staleRename{removedActionThread, NodeActionKind::Rename};
    staleRename.payload = {{"name", Value("must not reach provider")}};
    expect(sendAction(runtime.channels(), std::move(staleRename)),
           "a removed target remains safely pinnable in the typed queue");
    expect(!bridge.receiveAppServer(200ms),
           "worker membership revalidation blocks a removed thread target");
  }

  expect(
      bridge.appServerRequest("generation-input", "item/tool/requestUserInput",
                              {{"threadId", "runtime-thread"},
                               {"turnId", "generation-turn"},
                               {"itemId", "generation-question"},
                               {"isBlocking", true},
                               {"questions", userInputQuestions("question")}}),
      "generation-change request reaches the current graph");
  const NodeRef generationInput = findNode(
      runtime.graph(), {NodeKind::Interaction,
                        ProtocolRequestId("generation-input").canonical()});
  expect(bridge.setProviderGeneration(2),
         "a new provider generation reaches the runtime");
  std::size_t refreshes = 0;
  while (refreshes != 4) {
    const std::optional<nlohmann::json> refresh = bridge.receiveAppServer();
    if (!refresh)
      break;
    nlohmann::json result{{"data", nlohmann::json::array()}};
    if (refresh->value("method", std::string{}) == "thread/list")
      result["nextCursor"] = nullptr;
    if (bridge.reply(*refresh, std::move(result)))
      ++refreshes;
  }
  expect(refreshes == 4 && generationInput && waitUntil([&] {
           std::optional<NodeGraph::ReadAccess> read =
               runtime.graph().tryRead();
           if (!read)
             return false;
           const NodeRef current = read->find(generationInput->id());
           if (current != generationInput ||
               read->state(current)->status != NodeStatus::Failed)
             return false;
           const auto state = read->state(current);
           const auto recovery = state->fields.find("recoveryOnly");
           return recovery != state->fields.end() &&
                  recovery->second.asBool() && *recovery->second.asBool();
         }),
         "provider replacement keeps the expired interaction as explicit "
         "response-recovery state");
  if (generationInput) {
    NodeAction response{generationInput, NodeActionKind::ResolveInteraction};
    response.payload = userInputSubmission("question", "generation");
    expect(sendAction(runtime.channels(), std::move(response)),
           "authored response can reach the worker after generation reset");
    expect(!bridge.receiveAppServer(200ms),
           "expired-generation response is never sent to the replacement "
           "provider");
    expect(waitUntil([&] {
             std::optional<NodeGraph::ReadAccess> read =
                 runtime.graph().tryRead();
             if (!read)
               return false;
             const NodeRef current = read->find(generationInput->id());
             if (current != generationInput)
               return false;
             const auto state = read->state(current);
             const auto retained =
                 state->fields.find("retainedResponsePayload");
             return retained != state->fields.end() &&
                    retained->second.asObject() &&
                    *retained->second.asObject() ==
                        userInputSubmission("question", "generation");
           }),
           "generation rejection preserves newly authored response data in "
           "the shared graph");
  }
  runtime.drainNotifications();
}

void remainingReverseRequestFamiliesRoundTripExactlyOnce(
    UnixBridge &bridge, RunningRuntime &runtime) {
  const auto roundTrip =
      [&](std::string id, std::string method, nlohmann::json requestPayload,
          Value::Object responsePayload) -> std::optional<nlohmann::json> {
    expect(bridge.appServerRequest(id, method, std::move(requestPayload)),
           method + " reaches CodexBridge");
    const NodeRef interaction =
        findNode(runtime.graph(),
                 {NodeKind::Interaction, ProtocolRequestId(id).canonical()});
    expect(static_cast<bool>(interaction),
           method + " creates its stable interaction node");
    if (!interaction)
      return std::nullopt;

    NodeAction resolve{interaction, NodeActionKind::ResolveInteraction};
    resolve.payload = std::move(responsePayload);
    expect(sendAction(runtime.channels(), std::move(resolve)),
           method + " typed response enters the worker mailbox");
    std::optional<nlohmann::json> response = bridge.receiveAppServer();
    expect(response && response->value("id", std::string{}) == id,
           method + " response preserves the original JSON-RPC id");
    expect(!bridge.receiveAppServer(100ms),
           method + " response is never dual-sent");
    expect(waitUntil([&] {
             return interactionRetired(runtime.graph(), interaction);
           }),
           method + " response removes the exact interaction once");
    return response;
  };

  const nlohmann::json address{{"threadId", "runtime-thread"},
                               {"turnId", "reverse-turn"}};

  nlohmann::json fileChangeRequest = address;
  fileChangeRequest["itemId"] = "reverse-file-change";
  fileChangeRequest["reason"] = "write the focused test";
  std::optional<nlohmann::json> response =
      roundTrip("file-change-runtime", "item/fileChange/requestApproval",
                std::move(fileChangeRequest), {{"choice", Value("accept")}});
  expect(response && response->contains("result") &&
             response->at("result") == nlohmann::json{{"decision", "accept"}},
         "file-change approval encodes the authored decision exactly");

  nlohmann::json elicitationRequest = address;
  elicitationRequest["serverName"] = "test-mcp";
  elicitationRequest["message"] = "Choose a value";
  elicitationRequest["mode"] = "form";
  elicitationRequest["requestedSchema"] = nlohmann::json{{"type", "object"}};
  response =
      roundTrip("mcp-runtime", "mcpServer/elicitation/request",
                std::move(elicitationRequest),
                {{"choice", Value("accept")},
                 {"input", Value(Value::Object{{"choice", Value("safe")}})},
                 {"metadata", Value(Value::Object{{"source", Value("qt")}})}});
  expect(response && response->contains("result") &&
             response->at("result") ==
                 nlohmann::json{{"action", "accept"},
                                {"content", {{"choice", "safe"}}},
                                {"_meta", {{"source", "qt"}}}},
         "MCP elicitation encodes action, content, and metadata exactly");

  const nlohmann::json requestedPermissions{
      {"fileSystem", {{"read", nlohmann::json::array({"/workspace"})}}},
      {"network", {{"enabled", false}}}};
  nlohmann::json permissionsRequest = address;
  permissionsRequest["itemId"] = "reverse-permissions";
  permissionsRequest["permissions"] = requestedPermissions;
  response =
      roundTrip("permissions-runtime", "item/permissions/requestApproval",
                std::move(permissionsRequest), {{"choice", Value("turn")}});
  expect(response && response->contains("result") &&
             response->at("result") ==
                 nlohmann::json{{"permissions", requestedPermissions},
                                {"scope", "turn"}},
         "permission approval returns the exact requested permission object "
         "and authored scope");

  nlohmann::json dynamicToolRequest = address;
  dynamicToolRequest["callId"] = "reverse-dynamic-tool";
  dynamicToolRequest["tool"] = "unsupported_tool";
  dynamicToolRequest["arguments"] = nlohmann::json{{"value", 7}};
  response = roundTrip("dynamic-tool-runtime", "item/tool/call",
                       std::move(dynamicToolRequest),
                       {{"choice", Value("unavailable")}});
  expect(response && response->contains("result") &&
             response->at("result") ==
                 nlohmann::json{
                     {"contentItems",
                      nlohmann::json::array(
                          {{{"type", "inputText"},
                            {"text",
                             "CodexUI does not provide this dynamic tool"}}})},
                     {"success", false}},
         "dynamic-tool response encodes one explicit failure content item");

  response = roundTrip(
      "auth-refresh-runtime", "account/chatgptAuthTokens/refresh",
      {{"reason", "unauthorized"}}, {{"choice", Value("unsupported")}});
  expect(response && response->contains("error") &&
             response->at("error").value("code", 0) == -32601 &&
             response->at("error").value("message", std::string{}) ==
                 "CodexUI does not support authentication token refresh",
         "authentication refresh emits the explicit unsupported error");

  response = roundTrip("attestation-runtime", "attestation/generate",
                       {{"challenge", "challenge-value"}},
                       {{"choice", Value("unsupported")}});
  expect(response && response->contains("error") &&
             response->at("error").value("code", 0) == -32601 &&
             response->at("error").value("message", std::string{}) ==
                 "CodexUI does not support attestation generation",
         "attestation generation emits the explicit unsupported error");

  response = roundTrip("apply-patch-runtime", "applyPatchApproval",
                       {{"conversationId", "runtime-thread"},
                        {"callId", "reverse-apply-patch"},
                        {"fileChanges", nlohmann::json::object()}},
                       {{"choice", Value("denied")}});
  expect(
      response && response->contains("result") &&
          response->at("result") ==
              nlohmann::json{{"decision",
                              {{"denied", {{"rejection", "Denied by user"}}}}}},
      "apply-patch approval derives the typed denial from authored intent");

  response = roundTrip("exec-command-runtime", "execCommandApproval",
                       {{"conversationId", "runtime-thread"},
                        {"callId", "reverse-exec"},
                        {"command", nlohmann::json::array({"printf", "test"})},
                        {"cwd", "/tmp"}},
                       {{"choice", Value("approved_for_session")}});
  expect(response && response->contains("result") &&
             response->at("result") ==
                 nlohmann::json{{"decision", "approved_for_session"}},
         "exec-command approval maps the authored session decision exactly");

  runtime.drainNotifications();
}

void unknownInboundIsRetainedWithoutCorruptingKnownState(
    UnixBridge &bridge, RunningRuntime &runtime) {
  constexpr std::string_view RequestMethod = "future/runtime-request";
  expect(bridge.appServerRequest(
             "future-runtime-request", std::string(RequestMethod),
             {{"threadId", "runtime-thread"}, {"futureValue", 23}}),
         "unknown inbound request reaches CodexBridge");
  const std::optional<nlohmann::json> rejection = bridge.receiveAppServer();
  expect(rejection && !rejection->contains("method") &&
             rejection->value("id", std::string{}) ==
                 "future-runtime-request" &&
             rejection->contains("error") &&
             rejection->at("error").value("code", 0) == -32601,
         "CodexBridge automatically rejects an unregistered server request");
  {
    std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
    expect(read &&
               !read->find(
                   {NodeKind::Interaction,
                    ProtocolRequestId("future-runtime-request").canonical()}),
           "unknown server request creates no phantom UI interaction");
  }
  expect(!bridge.receiveAppServer(100ms),
         "unknown server request is rejected exactly once");

  constexpr std::string_view Method = "future/runtime-event";
  expect(bridge.appServerNotification(
             std::string(Method), {{"threadId", "runtime-thread"},
                                   {"name", "must-not-overwrite-known-thread"},
                                   {"futureValue", 17}}),
         "unknown inbound notification reaches CodexBridge's raw hook");

  const std::string unknownId = std::to_string(static_cast<unsigned>(
                                    ProtocolDirection::ServerNotification)) +
                                ":" + std::string(Method);
  const NodeRef unknown =
      findNode(runtime.graph(), {NodeKind::UnknownProtocol, unknownId});
  expect(static_cast<bool>(unknown),
         "unknown inbound notification is retained in the shared graph");

  std::optional<NodeGraph::ReadAccess> read = runtime.graph().tryRead();
  const NodeRef thread =
      read ? read->find({NodeKind::Thread, "runtime-thread"}) : NodeRef{};
  const auto unknownState = read && unknown ? read->state(unknown) : nullptr;
  const auto threadState = read && thread ? read->state(thread) : nullptr;
  const auto unknownMethod = unknownState ? unknownState->fields.find("method")
                                          : Value::Object::const_iterator{};
  const auto knownName = threadState ? threadState->fields.find("name")
                                     : Value::Object::const_iterator{};
  expect(unknownState && unknownMethod != unknownState->fields.end() &&
             unknownMethod->second.asString() &&
             *unknownMethod->second.asString() == Method,
         "unknown graph state identifies the unrecognized method");
  expect(threadState && knownName != threadState->fields.end() &&
             knownName->second.asString() &&
             *knownName->second.asString() == "Runtime dispatch",
         "unknown addressed fields cannot mutate a known thread");
  runtime.drainNotifications();
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  auto *configuration =
      utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
  core::SNodeC::init(argc, argv);

  codexui::codex::UnixBridge bridge;
  codexui::codex::expect(bridge.valid(),
                         "test bridge creates a private Unix listener");
  if (!bridge.valid())
    return EXIT_FAILURE;

  codexui::codex::RunningRuntime runtime(*configuration);
  runtime.start();
  const bool ready = codexui::codex::establishProvider(bridge, runtime);
  codexui::codex::expect(
      ready, "runtime connects and performs one initial provider hydration");
  if (ready) {
    codexui::codex::idleWorkerSleepsBetweenWakeRecoveryChecks(runtime);
    codexui::codex::protocolDiagnosticsPreserveMetadataWithoutPayloads(bridge,
                                                                       runtime);
    codexui::codex::directNodeActionsUseOneCorrelatedRequest(bridge, runtime);
    codexui::codex::remainingUiCommandFamiliesUseExactWirePaths(bridge,
                                                                runtime);
    codexui::codex::itemHistoryHydrationHasAnExactConcurrencyCeiling(bridge,
                                                                     runtime);
    codexui::codex::conversationSnapshotsShareOneAdmissionAuthority(bridge,
                                                                    runtime);
    codexui::codex::staleHistoryFailureIsSemanticallyInert(bridge, runtime);
    codexui::codex::ordinaryThreadPromptStillStartsAndCompletes(bridge,
                                                                runtime);
    codexui::codex::firstPromptAfterForkStartsANewTurn(bridge, runtime);
    codexui::codex::runtimeRefreshActionsHaveExactRequestCardinality(bridge,
                                                                     runtime);
    codexui::codex::failedWakeUsesBoundedWorkerRecovery(bridge, runtime);
    codexui::codex::reverseInteractionsRespondOnceWithAuthoredData(bridge,
                                                                   runtime);
    codexui::codex::invalidAuthoredReverseResponseNeverReachesWire(bridge,
                                                                   runtime);
    codexui::codex::remainingReverseRequestFamiliesRoundTripExactlyOnce(
        bridge, runtime);
    codexui::codex::unknownInboundIsRetainedWithoutCorruptingKnownState(
        bridge, runtime);
    codexui::codex::workerRevalidatesCurrentAuthorityAndRetainsResponses(
        bridge, runtime);
  }
  runtime.channels().failNextQtToWorkerWakeForTest();
  const auto shutdownStarted = std::chrono::steady_clock::now();
  runtime.stop();
  codexui::codex::expect(
      std::chrono::steady_clock::now() - shutdownStarted <
          std::chrono::seconds(2),
      "an unwoken ShutdownRequest is consumed without hanging worker join");

  if (codexui::codex::failures != 0) {
    std::cerr << codexui::codex::failures
              << " client-runtime dispatch assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "ClientRuntime typed dispatch integration test passed\n";
  return EXIT_SUCCESS;
}
