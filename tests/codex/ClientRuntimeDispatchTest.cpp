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
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace codexui::codex {
namespace {

using namespace std::chrono_literals;
using namespace nodegraph;

int failures = 0;

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

  void drainNotifications() {
    static_cast<void>(channels_.drainWorkerToQtWake());
    WorkerToQtMessage message;
    while (channels_.tryReceiveForQt(message))
      message = WorkerStopped{};
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
    for (const NodeRef &node : read->retiredNodes()) {
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
  return operation && read->related(operation, RelationKind::OperationTarget) ==
                          std::vector<NodeRef>{target};
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
  for (const NodeRef &retired : read->retiredNodes())
    if (retired == interaction)
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
  return {{"id", "runtime-thread"}, {"name", "Runtime dispatch"},
          {"status", "idle"},       {"createdAt", 1},
          {"updatedAt", 2},         {"turns", nlohmann::json::array()}};
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
  for (int count = 0; count != 3; ++count) {
    std::optional<nlohmann::json> request = bridge.receiveAppServer();
    if (!request || !request->contains("id"))
      return false;
    const std::string method = request->value("method", std::string{});
    ++methods[method];
    nlohmann::json result{{"data", nlohmann::json::array()}};
    if (method == "thread/list") {
      result["data"].push_back(listedThread());
      result["nextCursor"] = nullptr;
    }
    if (!bridge.reply(*request, std::move(result)))
      return false;
  }
  return methods ==
             std::unordered_map<std::string, std::size_t>{
                 {"thread/list", 1},
                 {"model/list", 1},
                 {"permissionProfile/list", 1}} &&
         static_cast<bool>(
             findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"}));
}

void directNodeActionsUseOneCorrelatedRequest(UnixBridge &bridge,
                                              RunningRuntime &runtime) {
  const NodeRef thread =
      findNode(runtime.graph(), {NodeKind::Thread, "runtime-thread"});
  expect(static_cast<bool>(thread), "provider hydration creates action target");
  if (!thread)
    return;

  NodeAction missingName{thread, NodeActionKind::Rename};
  expect(sendAction(runtime.channels(), std::move(missingName)),
         "a missing-name rename can enter the typed mailbox");
  expect(!bridge.receiveAppServer(100ms),
         "worker validation blocks a missing rename name");

  NodeAction emptyName{thread, NodeActionKind::Rename};
  emptyName.payload = {{"name", Value(" \t\n")}};
  expect(sendAction(runtime.channels(), std::move(emptyName)),
         "a blank-name rename can enter the typed mailbox");
  expect(!bridge.receiveAppServer(100ms),
         "worker validation blocks an empty rename name");

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

void runtimeRefreshActionsHaveExactRequestCardinality(UnixBridge &bridge,
                                                      RunningRuntime &runtime) {
  RuntimeAction refresh{RuntimeActionKind::RefreshThreads};
  refresh.payload = {{"limit", Value(std::uint64_t{17})}};
  expect(sendAction(runtime.channels(), std::move(refresh)),
         "thread refresh enters the worker mailbox");
  std::optional<nlohmann::json> request = bridge.receiveAppServer();
  expect(request && request->value("method", std::string{}) == "thread/list" &&
             request->at("params").value("limit", 0) == 17,
         "thread refresh emits exactly the requested typed operation");
  if (request)
    expect(bridge.reply(*request,
                        {{"data", nlohmann::json::array({listedThread()})},
                         {"nextCursor", nullptr}}),
           "thread refresh response is delivered");
  expect(!bridge.receiveAppServer(100ms),
         "one refresh action does not duplicate thread/list");

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
    resolve.payload = {{"decision", Value("accept")}};
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

  expect(bridge.appServerRequest("input-runtime", "item/tool/requestUserInput",
                                 {{"threadId", "runtime-thread"},
                                  {"turnId", "runtime-turn"},
                                  {"itemId", "runtime-question"},
                                  {"questions", nlohmann::json::array()}}),
         "user-input request reaches CodexBridge");
  const NodeRef input = findNode(
      runtime.graph(),
      {NodeKind::Interaction, ProtocolRequestId("input-runtime").canonical()});
  expect(static_cast<bool>(input),
         "reverse user input is represented by its interaction node");
  if (input) {
    NodeAction answer{input, NodeActionKind::ResolveInteraction};
    answer.payload = {
        {"answers", Value(Value::Object{
                        {"question-1", Value(Value::Array{Value("yes")})}})}};
    expect(sendAction(runtime.channels(), std::move(answer)),
           "user-input response action enters the worker mailbox");
    const std::optional<nlohmann::json> response = bridge.receiveAppServer();
    expect(response &&
               response->value("id", std::string{}) == "input-runtime" &&
               response->at("result").at("answers").at("question-1") ==
                   nlohmann::json::array({"yes"}),
           "user-input response moves only newly authored answers to the wire");
    expect(!bridge.receiveAppServer(100ms),
           "user-input response is never dual-sent");
    expect(
        waitUntil([&] { return interactionRetired(runtime.graph(), input); }),
        "user-input response removes the exact interaction once");
  }
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
  expect(bridge.appServerRequest("observer-input", "item/tool/requestUserInput",
                                 {{"threadId", "runtime-thread"},
                                  {"turnId", "observer-turn"},
                                  {"itemId", "observer-question"},
                                  {"questions", nlohmann::json::array()}},
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
    response.payload = {
        {"answers", Value(Value::Object{{"question", Value("claimed")}})}};
    expect(sendAction(runtime.channels(), std::move(response)),
           "new controller admits the earlier observer request response");
    const std::optional<nlohmann::json> wire = bridge.receiveAppServer();
    expect(wire && wire->value("id", std::string{}) == "observer-input" &&
               wire->at("result").at("answers").at("question") == "claimed",
           "current controller role, not receipt-time role, authorizes the "
           "exact response");
  }

  expect(bridge.appServerRequest("lost-control-input",
                                 "item/tool/requestUserInput",
                                 {{"threadId", "runtime-thread"},
                                  {"turnId", "lost-control-turn"},
                                  {"itemId", "lost-control-question"},
                                  {"questions", nlohmann::json::array()}}),
         "second reverse request arrives while controlled");
  const NodeRef lostControl = findNode(
      runtime.graph(), {NodeKind::Interaction,
                        ProtocolRequestId("lost-control-input").canonical()});
  expect(bridge.setRole("observer", "another-connection") &&
             waitUntil([&] { return graphRoleIs("observer"); }),
         "controller loss is visible before queued action consumption");
  if (lostControl) {
    NodeAction rejected{lostControl, NodeActionKind::ResolveInteraction};
    rejected.payload = {
        {"answers", Value(Value::Object{{"question", Value("preserved")}})}};
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
                    retained->second.asObject()->contains("answers");
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
    retry.payload = {
        {"answers", Value(Value::Object{{"question", Value("preserved")}})}};
    expect(sendAction(runtime.channels(), std::move(retry)),
           "the user can explicitly resubmit the preserved response");
    const std::optional<nlohmann::json> wire = bridge.receiveAppServer();
    expect(wire && wire->value("id", std::string{}) == "lost-control-input" &&
               wire->at("result").at("answers").at("question") == "preserved" &&
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

  expect(bridge.appServerRequest("generation-input",
                                 "item/tool/requestUserInput",
                                 {{"threadId", "runtime-thread"},
                                  {"turnId", "generation-turn"},
                                  {"itemId", "generation-question"},
                                  {"questions", nlohmann::json::array()}}),
         "generation-change request reaches the current graph");
  const NodeRef generationInput = findNode(
      runtime.graph(), {NodeKind::Interaction,
                        ProtocolRequestId("generation-input").canonical()});
  expect(bridge.setProviderGeneration(2),
         "a new provider generation reaches the runtime");
  std::size_t refreshes = 0;
  while (refreshes != 3) {
    const std::optional<nlohmann::json> refresh = bridge.receiveAppServer();
    if (!refresh)
      break;
    nlohmann::json result{{"data", nlohmann::json::array()}};
    if (refresh->value("method", std::string{}) == "thread/list")
      result["nextCursor"] = nullptr;
    if (bridge.reply(*refresh, std::move(result)))
      ++refreshes;
  }
  expect(refreshes == 3 && generationInput && waitUntil([&] {
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
    response.payload = {
        {"answers", Value(Value::Object{{"question", Value("generation")}})}};
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
                    retained->second.asObject()->contains("answers");
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
                std::move(fileChangeRequest), {{"decision", Value("accept")}});
  expect(response && response->contains("result") &&
             response->at("result") == nlohmann::json{{"decision", "accept"}},
         "file-change approval encodes the authored decision exactly");

  nlohmann::json elicitationRequest = address;
  elicitationRequest["serverName"] = "test-mcp";
  elicitationRequest["message"] = "Choose a value";
  elicitationRequest["requestedSchema"] = nlohmann::json{{"type", "object"}};
  response =
      roundTrip("mcp-runtime", "mcpServer/elicitation/request",
                std::move(elicitationRequest),
                {{"decision", Value("accept")},
                 {"content", Value(Value::Object{{"choice", Value("safe")}})},
                 {"_meta", Value(Value::Object{{"source", Value("qt")}})}});
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
                std::move(permissionsRequest),
                {{"decision", Value("accept")}, {"scope", Value("turn")}});
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
  response = roundTrip(
      "dynamic-tool-runtime", "item/tool/call", std::move(dynamicToolRequest),
      {{"contentItems", Value(Value::Array{Value(Value::Object{
                            {"type", Value("inputText")},
                            {"text", Value("Request declined by user")}})})},
       {"success", Value(false)}});
  expect(response && response->contains("result") &&
             response->at("result") ==
                 nlohmann::json{{"contentItems",
                                 nlohmann::json::array(
                                     {{{"type", "inputText"},
                                       {"text", "Request declined by user"}}})},
                                {"success", false}},
         "dynamic-tool response encodes one explicit failure content item");

  response =
      roundTrip("auth-refresh-runtime", "account/chatgptAuthTokens/refresh",
                {{"reason", "expired"}}, {});
  expect(response && response->contains("error") &&
             response->at("error").value("code", 0) == -32601 &&
             response->at("error").value("message", std::string{}) ==
                 "CodexUI does not support authentication token refresh",
         "authentication refresh emits the explicit unsupported error");

  response = roundTrip("attestation-runtime", "attestation/generate",
                       {{"challenge", "challenge-value"}}, {});
  expect(response && response->contains("error") &&
             response->at("error").value("code", 0) == -32601 &&
             response->at("error").value("message", std::string{}) ==
                 "CodexUI does not support attestation generation",
         "attestation generation emits the explicit unsupported error");

  const Value::Object deniedDecision{
      {"denied",
       Value(Value::Object{{"rejection", Value("Denied by focused test")}})}};
  response = roundTrip("apply-patch-runtime", "applyPatchApproval",
                       {{"conversationId", "runtime-thread"},
                        {"callId", "reverse-apply-patch"},
                        {"fileChanges", nlohmann::json::object()}},
                       {{"decision", Value(deniedDecision)}});
  expect(response && response->contains("result") &&
             response->at("result") ==
                 nlohmann::json{
                     {"decision",
                      {{"denied", {{"rejection", "Denied by focused test"}}}}}},
         "apply-patch approval preserves an authored structured decision");

  response = roundTrip("exec-command-runtime", "execCommandApproval",
                       {{"conversationId", "runtime-thread"},
                        {"callId", "reverse-exec"},
                        {"command", nlohmann::json::array({"printf", "test"})},
                        {"cwd", "/tmp"}},
                       {{"decision", Value("acceptForSession")}});
  expect(response && response->contains("result") &&
             response->at("result") ==
                 nlohmann::json{{"decision", "approved_for_session"}},
         "exec-command approval maps the authored session decision exactly");

  runtime.drainNotifications();
}

void unknownInboundIsRetainedWithoutCorruptingKnownState(
    UnixBridge &bridge, RunningRuntime &runtime) {
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
    codexui::codex::directNodeActionsUseOneCorrelatedRequest(bridge, runtime);
    codexui::codex::runtimeRefreshActionsHaveExactRequestCardinality(bridge,
                                                                     runtime);
    codexui::codex::reverseInteractionsRespondOnceWithAuthoredData(bridge,
                                                                   runtime);
    codexui::codex::remainingReverseRequestFamiliesRoundTripExactlyOnce(
        bridge, runtime);
    codexui::codex::unknownInboundIsRetainedWithoutCorruptingKnownState(
        bridge, runtime);
    codexui::codex::workerRevalidatesCurrentAuthorityAndRetainsResponses(
        bridge, runtime);
  }
  runtime.stop();

  if (codexui::codex::failures != 0) {
    std::cerr << codexui::codex::failures
              << " client-runtime dispatch assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "ClientRuntime typed dispatch integration test passed\n";
  return EXIT_SUCCESS;
}
