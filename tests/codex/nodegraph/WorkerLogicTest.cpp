// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/WorkerLogic.h"
#include "../TimingPolicy.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace codexui::nodegraph;

int failures = 0;

void require(bool condition, std::string_view message) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

const Value *field(const std::shared_ptr<const NodeState> &state,
                   std::string_view key) {
  if (!state)
    return nullptr;
  const auto found = state->fields.find(key);
  return found == state->fields.end() ? nullptr : &found->second;
}

bool stringFieldEquals(const std::shared_ptr<const NodeState> &state,
                       std::string_view key, std::string_view expected) {
  const Value *value = field(state, key);
  const std::string *text = value ? value->asString() : nullptr;
  return text && *text == expected;
}

bool unsignedFieldEquals(const std::shared_ptr<const NodeState> &state,
                         std::string_view key, std::uint64_t expected) {
  const Value *value = field(state, key);
  const std::uint64_t *number = value ? value->asUInt64() : nullptr;
  return number && *number == expected;
}

bool boolFieldEquals(const std::shared_ptr<const NodeState> &state,
                     std::string_view key, bool expected) {
  const Value *value = field(state, key);
  const bool *boolean = value ? value->asBool() : nullptr;
  return boolean && *boolean == expected;
}

bool objectStringFieldEquals(const std::shared_ptr<const NodeState> &state,
                             std::string_view key, std::string_view member,
                             std::string_view expected) {
  const Value *value = field(state, key);
  const Value::Object *object = value ? value->asObject() : nullptr;
  if (!object)
    return false;
  const auto found = object->find(member);
  const std::string *text =
      found == object->end() ? nullptr : found->second.asString();
  return text && *text == expected;
}

bool signedFieldEquals(const std::shared_ptr<const NodeState> &state,
                       std::string_view key, std::int64_t expected) {
  const Value *value = field(state, key);
  const std::int64_t *number = value ? value->asInt64() : nullptr;
  return number && *number == expected;
}

std::optional<GraphChanged> takeGraphChanged(ThreadChannels &channels,
                                             std::uint64_t wakeCount = 1) {
  const EventFd::DrainResult wake = channels.drainWorkerToQtWake();
  if (wake.status != EventFd::DrainStatus::Drained || wake.count != wakeCount)
    return std::nullopt;
  WorkerToQtMessage message;
  if (!channels.tryReceiveForQt(message))
    return std::nullopt;
  GraphChanged *changed = std::get_if<GraphChanged>(&message);
  if (!changed)
    return std::nullopt;
  return std::move(*changed);
}

std::vector<WorkerToQtMessage> takeWorkerMessages(ThreadChannels &channels) {
  static_cast<void>(channels.drainWorkerToQtWake());
  std::vector<WorkerToQtMessage> messages;
  WorkerToQtMessage message;
  while (channels.tryReceiveForQt(message)) {
    messages.emplace_back(std::move(message));
    message = WorkerToQtMessage(GraphChanged{});
  }
  return messages;
}

void protocolUpdatesPublishForUnlockedReads() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  const ChannelSendStatus status = logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("worker-thread")},
                                    {"name", Value("Worker thread")}})}}});
  require(status == ChannelSendStatus::Accepted,
          "protocol graph update is admitted to Qt");

  const std::optional<GraphChanged> changed = takeGraphChanged(channels);
  const bool includesThread =
      changed &&
      std::ranges::find_if(changed->affected, [](const NodeRef &node) {
        return node->id() == NodeId{NodeKind::Thread, "worker-thread"};
      }) != changed->affected.end();
  require(changed && changed->revision == graph.publishedRevision() &&
              !changed->rescanRequired && includesThread,
          "protocol update queues its committed revision and addressed "
          "NodeRef");

  auto qtRead = graph.tryRead();
  require(qtRead.has_value(),
          "Qt can try-read immediately after receiving the graph wake");
  if (!qtRead)
    return;
  const NodeRef thread = qtRead->find({NodeKind::Thread, "worker-thread"});
  require(thread && changed && changed->affected.front() == thread &&
              stringFieldEquals(qtRead->state(thread), "name", "Worker thread"),
          "the post-unlock read sees the complete protocol update");
}

void connectionStateAndGenerationsStayCurrent() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);
  std::uint64_t authorityRevision = 0;
  const auto requireProviderReplacement = [&](bool expected,
                                               std::string_view context) {
    const std::optional<GraphChanged> changed = takeGraphChanged(channels);
    const bool replaced = changed &&
                          changed->providerAuthorityRevision != 0;
    require(replaced == expected &&
                (!expected ||
                 (changed->providerAuthorityRevision == changed->revision &&
                  changed->providerAuthorityRevision > authorityRevision)),
            context);
    if (replaced)
      authorityRevision = changed->providerAuthorityRevision;
  };

  require(logic.transportEvent("connecting", "dialing bridge") ==
              ChannelSendStatus::Accepted,
          "connecting state is published");
  requireProviderReplacement(false,
                             "connecting preserves provider authority");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(connection && state && state->status == NodeStatus::Pending &&
                stringFieldEquals(state, "transportState", "connecting") &&
                stringFieldEquals(state, "transportDetail", "dialing bridge") &&
                unsignedFieldEquals(state, "connectionGeneration", 0),
            "connection node retains explicit connecting fields");
  }

  require(logic.transportEvent("connected", "unused") ==
              ChannelSendStatus::Accepted,
          "connected state is published");
  requireProviderReplacement(true,
                             "connected replaces provider authority");
  require(logic.bridgeState("bridge-1", "controller", "bridge-1", 7, "ready",
                            "provider ready") == ChannelSendStatus::Accepted,
          "bridge/provider state is published");
  requireProviderReplacement(true,
                             "a newer provider generation replaces authority");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(
        state && state->status == NodeStatus::Connected &&
            stringFieldEquals(state, "transportState", "connected") &&
            stringFieldEquals(state, "transportDetail", "") &&
            unsignedFieldEquals(state, "connectionGeneration", 1) &&
            unsignedFieldEquals(state, "providerAuthorityRevision",
                                authorityRevision) &&
            stringFieldEquals(state, "connectionId", "bridge-1") &&
            stringFieldEquals(state, "role", "controller") &&
            stringFieldEquals(state, "controllerConnectionId", "bridge-1") &&
            unsignedFieldEquals(state, "providerGeneration", 7) &&
            stringFieldEquals(state, "providerState", "ready") &&
            stringFieldEquals(state, "providerDetail", "provider ready"),
        "connection node holds current transport and bridge facts");
  }

  require(logic.connectionSettings(
              {{"selected", Value("unix")}, {"tls", Value(false)}}) ==
              ChannelSendStatus::Accepted,
          "connection settings are published");
  requireProviderReplacement(false,
                             "connection settings preserve provider authority");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Connected &&
                stringFieldEquals(state, "transportState", "connected") &&
                unsignedFieldEquals(state, "connectionGeneration", 1) &&
                stringFieldEquals(state, "connectionId", "bridge-1") &&
                unsignedFieldEquals(state, "providerGeneration", 7) &&
                stringFieldEquals(state, "providerState", "ready") &&
                objectStringFieldEquals(state, "settings", "selected", "unix"),
            "a settings-only update preserves current transport and provider "
            "facts on the connection node");
  }

  require(logic.bridgeState("bridge-1", "observer", "bridge-2", 7,
                            std::nullopt) == ChannelSendStatus::Accepted,
          "controller identity can change without a provider event");
  requireProviderReplacement(false,
                             "same-generation addressing preserves authority");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(
        state && stringFieldEquals(state, "role", "observer") &&
            stringFieldEquals(state, "controllerConnectionId", "bridge-2") &&
            stringFieldEquals(state, "providerState", "ready") &&
            stringFieldEquals(state, "providerDetail", "provider ready") &&
            stringFieldEquals(state, "transportState", "connected") &&
            objectStringFieldEquals(state, "settings", "selected", "unix"),
        "an addressing-only bridge update preserves transport, settings, and "
        "same-generation provider facts");
  }

  const std::uint64_t beforeStale = graph.publishedRevision();
  require(logic.bridgeState("bridge-1", "controller", "bridge-1", 6,
                            "disconnected",
                            "stale") == ChannelSendStatus::Accepted,
          "stale provider state is handled without failure");
  require(graph.publishedRevision() == beforeStale &&
              channels.workerToQtSizeApprox() == 0 &&
              channels.drainWorkerToQtWake().status ==
                  EventFd::DrainStatus::Empty,
          "stale provider generation cannot publish or wake Qt");

  require(logic.transportEvent("retrying", "retry scheduled") ==
              ChannelSendStatus::Accepted,
          "retrying state is published");
  requireProviderReplacement(true,
                             "retrying replaces provider authority");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(
        state && state->status == NodeStatus::Pending &&
            unsignedFieldEquals(state, "connectionGeneration", 1) &&
            unsignedFieldEquals(state, "providerGeneration", 7) &&
            stringFieldEquals(state, "connectionId", "") &&
            stringFieldEquals(state, "role", "") &&
            stringFieldEquals(state, "controllerConnectionId", "") &&
            stringFieldEquals(state, "providerState", "") &&
            stringFieldEquals(state, "providerDetail", "") &&
            stringFieldEquals(state, "transportDetail", "retry scheduled") &&
            objectStringFieldEquals(state, "settings", "selected", "unix"),
        "a transport-only retry clears bridge facts but preserves both "
        "generations and connection settings");
  }

  require(logic.transportEvent("disconnected", "connection lost") ==
              ChannelSendStatus::Accepted,
          "disconnect state is published");
  requireProviderReplacement(true,
                             "disconnect replaces provider authority");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Disconnected &&
                unsignedFieldEquals(state, "connectionGeneration", 1),
            "disconnect preserves the current connection generation");
  }

  require(logic.transportEvent("connected") == ChannelSendStatus::Accepted,
          "reconnection is published");
  requireProviderReplacement(true,
                             "reconnection replaces provider authority");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Connected &&
                unsignedFieldEquals(state, "connectionGeneration", 2) &&
                unsignedFieldEquals(state, "providerGeneration", 0) &&
                unsignedFieldEquals(state, "providerAuthorityRevision",
                                    authorityRevision) &&
                objectStringFieldEquals(state, "settings", "selected", "unix"),
            "each successful connection advances generation and resets "
            "provider generation without replacing connection settings");
  }

  {
    auto write = graph.write();
    const NodeRef connection = write.find({NodeKind::Connection, "connection"});
    write.setField(connection, "providerGeneration", Value(std::uint64_t{42}));
    static_cast<void>(write.finish());
  }
  const std::uint64_t beforeGenerationRead = graph.publishedRevision();
  require(logic.generations() == WorkerGenerations{2, 42} &&
              graph.publishedRevision() == beforeGenerationRead,
          "generation correlation reads the sole connection node authority "
          "without publishing another revision");
}

void stateNeutralMessagesDoNotWakeQt() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  const std::uint64_t before = graph.publishedRevision();
  const ChannelSendStatus status = logic.apply(
      {DecodedMessageKind::ServerNotification, "rawResponse/completed",
       std::nullopt, Value::Object{{"ignored", Value(true)}}});
  WorkerToQtMessage message;
  require(status == ChannelSendStatus::Accepted &&
              graph.publishedRevision() == before &&
              channels.workerToQtSizeApprox() == 0 &&
              channels.drainWorkerToQtWake().status ==
                  EventFd::DrainStatus::Empty &&
              !channels.tryReceiveForQt(message),
          "explicitly state-neutral protocol handling publishes no revision or "
          "wake");
}

void hydrationReadinessIsCurrentGraphState() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);
  static_cast<void>(logic.transportEvent("connected"));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("hydration-thread")},
                                          {"status", Value("notLoaded")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "hydration-thread"});
  }

  require(logic.threadHydration(thread, "loading") ==
              ChannelSendStatus::Accepted,
          "hydration start updates its exact current thread");
  static_cast<void>(takeWorkerMessages(channels));
  require(logic.threadHydration(thread, "failed", "read was rejected") ==
              ChannelSendStatus::Accepted,
          "hydration failure is retained for visible admission gating");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const auto state = read->state(thread);
    require(
        stringFieldEquals(state, "hydrationState", "failed") &&
            stringFieldEquals(state, "hydrationError", "read was rejected") &&
            unsignedFieldEquals(state, "hydrationConnectionGeneration", 1),
        "hydration state and connection generation live on the thread");
  }

  require(logic.threadHydration(thread, "ready") == ChannelSendStatus::Accepted,
          "successful reload makes the same thread admission-ready");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const auto state = read->state(thread);
    require(stringFieldEquals(state, "hydrationState", "ready") &&
                !field(state, "hydrationError"),
            "successful hydration clears the retained failure");
  }
}

void hydrationCompletionUsesExactOperationAuthority() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);
  static_cast<void>(logic.transportEvent("connected"));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("typed-hydration")},
                                          {"status", Value("notLoaded")}})}}}));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "item/plan/delta", std::nullopt,
       Value::Object{{"threadId", Value("typed-hydration")},
                     {"turnId", Value("typed-turn")},
                     {"itemId", Value("typed-plan")},
                     {"delta", Value("newer streamed plan")}}}));
  static_cast<void>(takeWorkerMessages(channels));

  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "typed-hydration"});
  }
  const ProtocolRequestId partialId("partial-hydration");
  WorkerApplyResult partialRequest = logic.applyDetailed(
      {DecodedMessageKind::ClientRequest, "thread/read", partialId,
       Value::Object{{"threadId", Value("typed-hydration")}}});
  static_cast<void>(takeWorkerMessages(channels));
  Value::Object partialTurn{{"id", Value("typed-turn")}};
  Value::Object partialThread{
      {"id", Value("typed-hydration")},
      {"turns", Value(Value::Array{Value(std::move(partialTurn))})}};
  DecodedMessage partialResult{
      DecodedMessageKind::ClientResult, "thread/read", partialId,
      Value::Object{{"thread", Value(std::move(partialThread))}},
      partialRequest.primary};
  require(logic.completeThreadHydration(std::move(partialResult), thread,
                                        "ready") == ChannelSendStatus::Accepted,
          "an exact successful hydration result is reduced atomically");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef partialItem = read->find(scopedItemNodeId(
        scopedTurnNodeId("typed-hydration", "typed-turn"), "typed-plan"));
    const auto state = read->state(thread);
    require(partialItem && !field(read->state(partialItem), "type") &&
                stringFieldEquals(state, "hydrationState", "ready") &&
                !field(state, "hydrationError") &&
                !read->find(partialRequest.primary->id()),
            "partial item presentation state cannot overrule the exact "
            "accepted read outcome");
  }

  require(logic.threadHydration(thread, "loading") ==
              ChannelSendStatus::Accepted,
          "a reload returns the current thread to loading");
  static_cast<void>(takeWorkerMessages(channels));
  const ProtocolRequestId reusedId("reused-hydration");
  WorkerApplyResult staleRequest = logic.applyDetailed(
      {DecodedMessageKind::ClientRequest, "thread/read", reusedId,
       Value::Object{{"threadId", Value("typed-hydration")}}});
  static_cast<void>(takeWorkerMessages(channels));
  WorkerApplyResult currentRequest = logic.applyDetailed(
      {DecodedMessageKind::ClientRequest, "thread/read", reusedId,
       Value::Object{{"threadId", Value("typed-hydration")}}});
  static_cast<void>(takeWorkerMessages(channels));
  require(staleRequest.primary && currentRequest.primary &&
              staleRequest.primary != currentRequest.primary,
          "reusing a request id creates a distinct current Operation");

  const std::uint64_t beforeStale = graph.publishedRevision();
  Value::Object staleThread{{"id", Value("typed-hydration")},
                            {"name", Value("stale name")},
                            {"turns", Value(Value::Array{})}};
  DecodedMessage staleSuccess{
      DecodedMessageKind::ClientResult, "thread/read", reusedId,
      Value::Object{{"thread", Value(std::move(staleThread))}},
      staleRequest.primary};
  require(logic.completeThreadHydration(std::move(staleSuccess), thread,
                                        "ready") == ChannelSendStatus::Accepted,
          "a late success is accepted as an inert worker message");
  DecodedMessage staleError{
      DecodedMessageKind::ClientError, "thread/read", reusedId,
      Value::Object{{"message", Value("late error")}}, staleRequest.primary};
  require(logic.completeThreadHydration(std::move(staleError), thread, "failed",
                                        "late error") ==
              ChannelSendStatus::Accepted,
          "a late error is accepted as an inert worker message");
  {
    auto read = graph.tryRead();
    require(graph.publishedRevision() == beforeStale &&
                takeWorkerMessages(channels).empty() &&
                read->find(currentRequest.primary->id()) ==
                    currentRequest.primary &&
                stringFieldEquals(read->state(thread), "hydrationState",
                                  "loading") &&
                !field(read->state(thread), "hydrationError") &&
                !field(read->state(thread), "name"),
            "late success and error cannot mutate the reused Operation or its "
            "thread hydration state");
  }

  Value::Object currentThread{{"id", Value("typed-hydration")},
                              {"name", Value("current name")},
                              {"turns", Value(Value::Array{})}};
  DecodedMessage currentSuccess{
      DecodedMessageKind::ClientResult, "thread/read", reusedId,
      Value::Object{{"thread", Value(std::move(currentThread))}},
      currentRequest.primary};
  require(logic.completeThreadHydration(std::move(currentSuccess), thread,
                                        "ready") == ChannelSendStatus::Accepted,
          "the exact current success completes hydration");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    require(
        !read->find(currentRequest.primary->id()) &&
            stringFieldEquals(read->state(thread), "name", "current name") &&
            stringFieldEquals(read->state(thread), "hydrationState", "ready") &&
            !field(read->state(thread), "hydrationError"),
        "only the exact current success retires its Operation and makes "
        "the thread ready");
  }

  require(logic.threadHydration(thread, "loading") ==
              ChannelSendStatus::Accepted,
          "a later reload returns to loading");
  static_cast<void>(takeWorkerMessages(channels));
  const ProtocolRequestId failedId("current-hydration-error");
  WorkerApplyResult failedRequest = logic.applyDetailed(
      {DecodedMessageKind::ClientRequest, "thread/read", failedId,
       Value::Object{{"threadId", Value("typed-hydration")}}});
  static_cast<void>(takeWorkerMessages(channels));
  DecodedMessage currentError{
      DecodedMessageKind::ClientError, "thread/read", failedId,
      Value::Object{{"message", Value("read was rejected")}},
      failedRequest.primary};
  require(logic.completeThreadHydration(std::move(currentError), thread,
                                        "failed", "read was rejected") ==
              ChannelSendStatus::Accepted,
          "the exact current error completes hydration as failed");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    require(!read->find(failedRequest.primary->id()) &&
                stringFieldEquals(read->state(thread), "hydrationState",
                                  "failed") &&
                stringFieldEquals(read->state(thread), "hydrationError",
                                  "read was rejected"),
            "only an accepted current error authors visible hydration "
            "failure and its real detail");
  }

  require(logic.threadHydration(thread, "loading") ==
              ChannelSendStatus::Accepted,
          "a contract-checking reload returns to loading");
  static_cast<void>(takeWorkerMessages(channels));
  const ProtocolRequestId mismatchedId("mismatched-hydration-thread");
  WorkerApplyResult mismatchedRequest = logic.applyDetailed(
      {DecodedMessageKind::ClientRequest, "thread/read", mismatchedId,
       Value::Object{{"threadId", Value("typed-hydration")}}});
  static_cast<void>(takeWorkerMessages(channels));
  Value::Object otherThread{{"id", Value("different-thread")},
                            {"turns", Value(Value::Array{})}};
  DecodedMessage mismatchedResult{
      DecodedMessageKind::ClientResult, "thread/read", mismatchedId,
      Value::Object{{"thread", Value(std::move(otherThread))}},
      mismatchedRequest.primary};
  require(logic.completeThreadHydration(std::move(mismatchedResult), thread,
                                        "ready") == ChannelSendStatus::Accepted,
          "a correlated response with the wrong Thread is reduced safely");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    require(
        !read->find(mismatchedRequest.primary->id()) &&
            stringFieldEquals(read->state(thread), "hydrationState",
                              "failed") &&
            stringFieldEquals(read->state(thread), "hydrationError",
                              "Thread hydration returned a mismatched thread"),
        "wire correlation alone cannot make a different Thread payload "
        "ready");
  }
}

void hydrationCompletionWorkIsIndependentOfRetainedItems() {
  constexpr std::size_t Repetitions = 64;
  const auto measure = [](std::size_t itemCount) {
    NodeGraph graph;
    ThreadChannels channels;
    WorkerLogic logic(graph, channels);
    NodeRef thread;
    {
      auto write = graph.write();
      thread = write.upsert({NodeKind::Thread, "hydration-scale-thread"});
      const NodeRef turn = write.upsert(
          scopedTurnNodeId("hydration-scale-thread", "retained-turn"));
      write.setParent(thread, turn);
      std::vector<NodeRef> items;
      items.reserve(itemCount);
      for (std::size_t index = 0; index < itemCount; ++index) {
        NodeState state;
        state.fields = {{"type", Value("agentMessage")},
                        {"protocolId", Value(std::to_string(index))}};
        NodeRef item =
            write.upsert(scopedItemNodeId(turn->id(), std::to_string(index)),
                         std::move(state));
        write.setParent(turn, item);
        items.emplace_back(std::move(item));
      }
      write.replaceChildren(turn, std::move(items));
      static_cast<void>(write.finish());
    }

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t repetition = 0; repetition < Repetitions; ++repetition) {
      const ProtocolRequestId id("hydration-scale-" +
                                 std::to_string(repetition));
      WorkerApplyResult request = logic.applyDetailed(
          {DecodedMessageKind::ClientRequest, "thread/read", id,
           Value::Object{{"threadId", Value("hydration-scale-thread")}}});
      static_cast<void>(logic.completeThreadHydration(
          {DecodedMessageKind::ClientError, "thread/read", id,
           Value::Object{{"message", Value("measured failure")}},
           request.primary},
          thread, "failed", "measured failure"));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    auto read = graph.tryRead();
    const bool valid =
        read &&
        stringFieldEquals(read->state(thread), "hydrationState", "failed") &&
        stringFieldEquals(read->state(thread), "hydrationError",
                          "measured failure");
    return std::pair{valid, elapsed};
  };

  const auto [smallValid, smallElapsed] = measure(2000);
  const auto [largeValid, largeElapsed] = measure(40000);
  require(
      smallValid && largeValid &&
          codexui::testing::timingLimit(
              largeElapsed <= smallElapsed * 4 + std::chrono::milliseconds(10)),
      "hydration completion admission is independent of retained Item "
      "cardinality");
  std::cout
      << "hydration-completion ns (2000 / 40000 retained items): "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(smallElapsed)
             .count()
      << " / "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(largeElapsed)
             .count()
      << '\n';
}

void graphNotificationSaturationCoalesces() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  std::size_t admitted = 0;
  bool reachedLimit = false;
  bool statusesAccepted = true;
  for (std::size_t attempt = 0; attempt <= ThreadChannels::WorkerToQtCapacity;
       ++attempt) {
    ProtocolDiagnostic diagnostic{
        Value::Object{{"sequence", Value(static_cast<std::int64_t>(attempt))}},
        {}};
    const ChannelSendStatus status =
        channels.sendProtocolDiagnostic(diagnostic);
    if (status == ChannelSendStatus::QueueFull) {
      reachedLimit = true;
      break;
    }
    statusesAccepted =
        statusesAccepted && status == ChannelSendStatus::Accepted;
    ++admitted;
  }
  require(statusesAccepted && reachedLimit &&
              admitted + ThreadChannels::WorkerToQtReservedSlots ==
                  ThreadChannels::WorkerToQtCapacity,
          "ordinary worker messages preserve critical and terminal slots");

  const ChannelSendStatus status = logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("coalesced-thread")}})}}});
  require(
      status == ChannelSendStatus::CoalescedRescan && messageAdmitted(status) &&
          !wakeFailed(status) && channels.rescanPending() &&
          channels.workerToQtSizeApprox() == admitted,
      "committed graph update coalesces visibly when its queue is saturated");

  const std::uint64_t revision = graph.publishedRevision();
  const std::optional<GraphChanged> rescan =
      takeGraphChanged(channels, admitted + 1);
  require(rescan && rescan->rescanRequired && rescan->revision == revision &&
              !channels.rescanPending(),
          "saturated graph update wakes Qt with the latest rescan revision");
  auto read = graph.tryRead();
  require(read && read->find({NodeKind::Thread, "coalesced-thread"}),
          "coalescing never loses the already-committed current graph state");
}

void uiDetachAcknowledgementIsRevisionNeutral() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  NodeRef node;
  {
    auto write = graph.write();
    node = write.upsert({NodeKind::Item, "retired-item"});
    static_cast<void>(write.finish());
  }
  GraphChange removal;
  {
    auto write = graph.write();
    write.remove(node);
    removal = write.finish();
  }
  const std::uint64_t removalRevision = graph.publishedRevision();
  std::weak_ptr<Node> lifetime = node;
  NodeRef acknowledgement = removal.removed.front();
  removal.affected.clear();
  removal.removed.clear();
  node.reset();

  {
    auto read = graph.tryRead();
    require(read && read->retiredCount() == 1 && !lifetime.expired(),
            "removed node remains pinned until Qt detaches it");
  }
  const ChannelSendStatus status =
      logic.acknowledgeUiDetached(std::move(acknowledgement));
  WorkerToQtMessage message;
  require(status == ChannelSendStatus::Accepted &&
              graph.publishedRevision() == removalRevision &&
              channels.drainWorkerToQtWake().status ==
                  EventFd::DrainStatus::Empty &&
              !channels.tryReceiveForQt(message),
          "UiDetached releases retirement without a revision or notification");
  {
    auto read = graph.tryRead();
    require(read && read->retiredCount() == 0,
            "UiDetached removes the graph retirement pin");
  }
  require(lifetime.expired(),
          "retired node dies after the acknowledgement releases its last pin");
}

void authoritativeUiStateSurvivesQueueSaturation() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);
  NodeRef first;
  NodeRef second;
  {
    auto write = graph.write();
    first = write.upsert({NodeKind::Thread, "selection-first"});
    second = write.upsert({NodeKind::Thread, "selection-second"});
    static_cast<void>(write.finish());
  }
  while (true) {
    ProtocolDiagnostic filler{Value::Object{{"subject", Value("filler")}},
                              {}};
    if (channels.sendProtocolDiagnostic(filler) ==
        ChannelSendStatus::QueueFull)
      break;
  }

  require(logic.showNotice("latest visible failure") ==
              ChannelSendStatus::CoalescedRescan,
          "a saturated notice becomes current graph state and an explicit "
          "rescan");
  require(logic.selectThread(first) == ChannelSendStatus::CoalescedRescan,
          "a saturated selection remains authoritative graph state");
  require(logic.selectThread(second) == ChannelSendStatus::CoalescedRescan,
          "a later saturated selection replaces the authoritative target");
  {
    auto read = graph.tryRead();
    const NodeRef notice =
        read->find({NodeKind::Notice, "local-worker-notice"});
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    require(notice &&
                stringFieldEquals(read->state(notice), "message",
                                  "latest visible failure") &&
                runtime &&
                read->related(runtime, RelationKind::UiSelectionTarget) ==
                    std::vector<NodeRef>{second},
            "Qt reconstructs the latest notice and exact stable selection "
            "target from the sole authoritative graph state");
  }
  require(logic.sendWorkerStopped("terminal") == ChannelSendStatus::Accepted,
          "terminal delivery still uses the final reserved slot");
}

void reverseInteractionResolutionUpdatesTheGraph() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  const WorkerApplyResult first =
      logic.applyDetailed({DecodedMessageKind::ServerRequest,
                           "item/commandExecution/requestApproval",
                           ProtocolRequestId("approval-1"),
                           {{"threadId", Value("thread-1")},
                            {"turnId", Value("turn-1")},
                            {"itemId", Value("item-1")}}});
  require(takeGraphChanged(channels).has_value(),
          "reverse interaction creation wakes Qt");

  require(first.primary && logic.resolveInteraction(first.primary) ==
                               ChannelSendStatus::Accepted,
          "accepted reverse interaction is resolved");
  const std::optional<GraphChanged> removed = takeGraphChanged(channels);
  require(removed && removed->removed.size() == 1 &&
              removed->removed.front()->id().kind == NodeKind::Interaction,
          "accepted response removes the pending interaction atomically");
  if (removed && !removed->removed.empty())
    static_cast<void>(logic.acknowledgeUiDetached(removed->removed.front()));

  const WorkerApplyResult second =
      logic.applyDetailed({DecodedMessageKind::ServerRequest,
                           "item/fileChange/requestApproval",
                           ProtocolRequestId("approval-2"),
                           {{"threadId", Value("thread-1")}}});
  require(takeGraphChanged(channels).has_value(),
          "second reverse interaction creation wakes Qt");
  require(second.primary &&
              logic.failInteractionResponse(second.primary,
                                            "transport rejected response") ==
                  ChannelSendStatus::Accepted,
          "rejected response failure is recorded");
  require(takeGraphChanged(channels).has_value(),
          "rejected response failure wakes Qt");
  auto read = graph.tryRead();
  const NodeRef failed = second.primary;
  const auto state = read && failed ? read->state(failed) : nullptr;
  const NodeRef thread =
      read ? read->find({NodeKind::Thread, "thread-1"}) : NodeRef{};
  const NodeRef runtime =
      read ? read->find({NodeKind::Runtime, "runtime"}) : NodeRef{};
  require(
      state && state->status == NodeStatus::Failed &&
          stringFieldEquals(state, "error", "transport rejected response") &&
          read && thread && runtime &&
          read->related(failed, RelationKind::InteractionTarget) ==
              std::vector<NodeRef>{thread} &&
          read->related(runtime, RelationKind::PendingInteraction) ==
              std::vector<NodeRef>{failed} &&
          read->related(thread, RelationKind::PendingInteraction).empty(),
      "failed response remains visible with its concrete error and "
      "derives unresolved attention from sole runtime membership and target "
      "ancestry");
  read.reset();

  const Value::Object authoredResponse{
      {"choice", Value("submit")},
      {"input", Value(Value::Object{
                    {"question",
                     Value(Value::Object{
                         {"answers", Value(Value::Array{Value("yes")})}})}})}};
  require(logic.failInteractionResponse(
              failed, "controller changed before delivery", authoredResponse) ==
              ChannelSendStatus::Accepted,
          "an authored reverse response can be retained after rejection");
  static_cast<void>(takeGraphChanged(channels));
  read = graph.tryRead();
  const auto retainedState = read && failed ? read->state(failed) : nullptr;
  const Value *retained = field(retainedState, "retainedResponsePayload");
  const Value *responseRevision = field(retainedState, "responseRevision");
  const std::uint64_t firstResponseRevision =
      responseRevision && responseRevision->asUInt64()
          ? *responseRevision->asUInt64()
          : 0;
  require(retained && retained->asObject() &&
              *retained->asObject() == authoredResponse &&
              firstResponseRevision != 0 &&
              stringFieldEquals(retainedState, "error",
                                "controller changed before delivery") &&
              read && runtime &&
              read->related(runtime, RelationKind::PendingInteraction) ==
                  std::vector<NodeRef>{failed} &&
              read->related(thread, RelationKind::PendingInteraction).empty(),
          "the failed interaction owns the exact authored response for a "
          "manual retry without automatic resend or losing thread attention");
  read.reset();

  require(logic.failInteractionResponse(
              failed, "controller changed before delivery", authoredResponse) ==
              ChannelSendStatus::Accepted,
          "an identical failed retry is acknowledged authoritatively");
  require(takeGraphChanged(channels).has_value(),
          "an identical failed retry publishes its acknowledgement");
  read = graph.tryRead();
  const auto repeatedState = read && failed ? read->state(failed) : nullptr;
  responseRevision = field(repeatedState, "responseRevision");
  require(responseRevision && responseRevision->asUInt64() &&
              *responseRevision->asUInt64() > firstResponseRevision,
          "response acknowledgement advances when payload and error repeat");
  read.reset();

  require(logic.resolveInteraction(failed) == ChannelSendStatus::Accepted,
          "exact failed interaction can be retired before id reuse");
  require(takeGraphChanged(channels).has_value(),
          "exact interaction retirement is published");
  static_cast<void>(logic.apply({DecodedMessageKind::ServerRequest,
                                 "item/fileChange/requestApproval",
                                 ProtocolRequestId("approval-2"),
                                 {{"threadId", Value("thread-1")}}}));
  require(takeGraphChanged(channels).has_value(),
          "replacement request with the same wire id is published");
  read = graph.tryRead();
  const NodeRef replacement =
      read ? read->find({NodeKind::Interaction,
                         ProtocolRequestId("approval-2").canonical()})
           : NodeRef{};
  require(replacement && replacement != failed,
          "wire-id reuse creates a distinct exact interaction");
  read.reset();

  require(logic.failInteractionResponse(failed, "stale response",
                                        authoredResponse) ==
              ChannelSendStatus::Accepted,
          "stale exact response rejection is safely ignored");
  require(takeWorkerMessages(channels).empty(),
          "stale rejection publishes no graph or UI traffic");
  read = graph.tryRead();
  const auto replacementState =
      read && replacement ? read->state(replacement) : nullptr;
  require(replacementState && replacementState->status == NodeStatus::Pending &&
              !field(replacementState, "retainedResponsePayload") &&
              !field(replacementState, "responseRevision") &&
              !field(replacementState, "error"),
          "stale rejection cannot mutate the current same-id request");
}

void threadActivityAndPromptOrderingStayInTheGraph() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("parent")},
                                          {"updatedAt", Value(10)},
                                          {"recencyAt", Value(30)}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("child")},
                                          {"parentThreadId", Value("parent")},
                                          {"updatedAt", Value(20)},
                                          {"recencyAt", Value(20)}})}}}));
  static_cast<void>(takeWorkerMessages(channels));

  NodeRef parent;
  NodeRef child;
  {
    auto read = graph.tryRead();
    parent = read->find({NodeKind::Thread, "parent"});
    child = read->find({NodeKind::Thread, "child"});
  }
  NodeAction first{child, NodeActionKind::SubmitPrompt};
  first.promptText = "promote child root";
  PromptTransition admitted = logic.admitPrompt(std::move(first), 20);
  const NodeRef localPrompt =
      admitted.command ? admitted.command->localPrompt : NodeRef{};
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    require(
        signedFieldEquals(read->state(child), "localPromptActivityAt", 31) &&
            signedFieldEquals(read->state(parent), "localPromptActivityAt",
                              31) &&
            signedFieldEquals(read->state(parent), "localActivityAt", 31),
        "prompt admission advances beyond every provider sort key and "
        "propagates to its visible root group");
  }

  static_cast<void>(logic.failPrompt(localPrompt, "turn/start rejected"));
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    require(field(read->state(child), "localPromptActivityAt") == nullptr &&
                field(read->state(parent), "localPromptActivityAt") == nullptr,
            "a rejected turn admission removes its optimistic Recent value");
  }

  static_cast<void>(logic.applyDetailed(
      {DecodedMessageKind::ServerNotification,
       "turn/started",
       std::nullopt,
       Value::Object{
           {"threadId", Value("child")},
           {"turn", Value(Value::Object{{"id", Value("activity-turn")}})}},
       {},
       40}));
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    require(signedFieldEquals(read->state(child), "localActivityAt", 40) &&
                signedFieldEquals(read->state(parent), "localActivityAt", 40) &&
                field(read->state(parent), "localPromptActivityAt") == nullptr,
            "meaningful decoded traffic advances heading activity without "
            "creating prompt ordering state");
  }

  static_cast<void>(logic.applyDetailed(
      {DecodedMessageKind::ClientResult,
       "thread/read",
       ProtocolRequestId("hydration"),
       Value::Object{{"thread", Value(Value::Object{{"id", Value("child")}})}},
       {},
       std::nullopt}));
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    require(signedFieldEquals(read->state(parent), "localActivityAt", 40),
            "selection-driven hydration does not count as live activity");
  }
}

void localPromptsAreGraphNodesAndDispatchPerThread() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  for (const std::string_view id : {"prompt-thread", "independent-thread"}) {
    static_cast<void>(logic.apply(
        {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
         Value::Object{{"thread", Value(Value::Object{{"id", Value(id)}})}}}));
    static_cast<void>(takeWorkerMessages(channels));
  }
  NodeRef thread;
  NodeRef independent;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "prompt-thread"});
    independent = read->find({NodeKind::Thread, "independent-thread"});
  }

  NodeAction first;
  first.target = thread;
  first.kind = NodeActionKind::SubmitPrompt;
  first.promptText = std::string(4096, 'p');
  first.promptText.front() = 'A';
  first.attachments.push_back({"/tmp/prompt.png", "prompt.png", "image/png",
                               std::vector<std::uint8_t>(4096, 7)});
  first.attachments.push_back(
      {"/tmp/report #?.txt", "report[1].txt", "text/plain", std::nullopt});
  first.payload.emplace("model", Value("gpt-current"));
  const char *const textStorage = first.promptText.data();
  const std::uint8_t *const bytesStorage =
      first.attachments.front().bytes->data();
  PromptTransition admitted = logic.admitPrompt(std::move(first), 10, 10'000);
  require(admitted.command &&
              admitted.command->kind == PromptCommandKind::StartTurn &&
              admitted.command->thread == thread &&
              admitted.command->promptText.data() == textStorage &&
              admitted.command->attachments.front().bytes->data() ==
                  bytesStorage,
          "first prompt moves its exact large text and attachment storage into "
          "a start-turn command");
  const NodeRef firstPrompt =
      admitted.command ? admitted.command->localPrompt : NodeRef{};
  const std::string firstClientId =
      admitted.command ? admitted.command->clientUserMessageId : std::string{};
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef localTurn = read->parent(firstPrompt);
    const auto state = read->state(firstPrompt);
    const Value *text = field(state, "text");
    const Value *dispatch = field(state, "dispatchState");
    require(
        firstPrompt && localTurn &&
            localTurn->id().canonical.starts_with("local-turn:") &&
            read->parent(localTurn) == thread && text && text->asString() &&
            text->asString()->starts_with(admitted.command->promptText) &&
            text->asString()->ends_with("Attached files:\n- [report\\[1\\].txt]"
                                        "(file:///tmp/report%20%23%3F.txt)") &&
            dispatch && dispatch->asString() &&
            *dispatch->asString() == "dispatching" &&
            boolFieldEquals(state, "showPendingAnimation", false) &&
            read->related(runtime, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{firstPrompt} &&
            read->related(thread, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{firstPrompt} &&
            read->related(localTurn, RelationKind::TurnRootItem) ==
                std::vector<NodeRef>{firstPrompt},
        "admission stores one directly-related local prompt with the "
        "same safe file Markdown sent on the wire and makes its You card the "
        "owning turn root");
  }

  NodeAction queued;
  queued.target = thread;
  queued.kind = NodeActionKind::SubmitPrompt;
  queued.promptText = "second exact prompt";
  PromptTransition second = logic.admitPrompt(std::move(queued), 11, 11'000);
  require(!second.command,
          "a second prompt for the same thread remains queued while one "
          "request is in flight");
  static_cast<void>(takeWorkerMessages(channels));

  NodeAction parallel;
  parallel.target = independent;
  parallel.kind = NodeActionKind::SubmitPrompt;
  parallel.promptText = "independent prompt";
  PromptTransition independentAdmission =
      logic.admitPrompt(std::move(parallel), 12, 12'000);
  require(independentAdmission.command &&
              independentAdmission.command->thread == independent,
          "different threads dispatch independently");
  static_cast<void>(takeWorkerMessages(channels));

  require(logic.markPromptDispatched(firstPrompt,
                                     ProtocolRequestId("turn-request-1")) ==
              ChannelSendStatus::Accepted,
          "the direct bridge request marks only its exact local prompt");
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("prompt-thread")},
           {"turn", Value(Value::Object{{"id", Value("authoritative-turn")},
                                        {"status", Value("inProgress")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));

  PromptTransition completed =
      logic.completePrompt(firstPrompt, true, {}, "authoritative-turn");
  bool steeringKeptFirstTurnOrder = false;
  if (completed.command) {
    auto read = graph.tryRead();
    steeringKeptFirstTurnOrder =
        field(read->state(completed.command->localPrompt), "sortActivityAt") ==
            nullptr &&
        signedFieldEquals(read->state(thread), "localPromptActivityAt", 10);
  }
  require(completed.command &&
              completed.command->kind == PromptCommandKind::SteerTurn &&
              completed.command->expectedTurnId == "authoritative-turn" &&
              completed.command->promptText == "second exact prompt" &&
              steeringKeptFirstTurnOrder,
          "a successful request releases exactly the next same-thread prompt "
          "as a steer command without treating steering as a newer turn");
  static_cast<void>(takeWorkerMessages(channels));

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
       Value::Object{{"threadId", Value("prompt-thread")},
                     {"turnId", Value("authoritative-turn")},
                     {"item", Value(Value::Object{
                                  {"id", Value("user-item")},
                                  {"type", Value("userMessage")},
                                  {"clientId", Value(firstClientId)}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef authoritative;
  {
    auto read = graph.tryRead();
    authoritative = read->find(scopedItemNodeId(
        scopedTurnNodeId("prompt-thread", "authoritative-turn"), "user-item"));
    require(
        authoritative &&
            read->related(authoritative, RelationKind::PromptMaterialization) ==
                std::vector<NodeRef>{firstPrompt} &&
            read->related(read->parent(authoritative),
                          RelationKind::TurnRootItem) ==
                std::vector<NodeRef>{authoritative} &&
            read->state(firstPrompt)->status == NodeStatus::Running &&
            stringFieldEquals(read->state(firstPrompt), "dispatchState",
                              "awaitingMaterialization") &&
            boolFieldEquals(read->state(firstPrompt), "showPendingAnimation",
                            false),
        "matching authoritative clientId directly relates the user item "
        "to its active local visual identity and transfers canonical "
        "turn-root ownership without overriding the retained UI deadline");
  }

  const ChannelSendStatus removed = logic.promptMaterialized(firstPrompt);
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const std::vector<NodeRef> pending =
        read->related(runtime, RelationKind::PendingPrompt);
    require(
        removed == ChannelSendStatus::Accepted &&
            !read->find(firstPrompt->id()) && authoritative &&
            read->related(authoritative, RelationKind::PromptMaterialization)
                .empty() &&
            std::ranges::find(pending, firstPrompt) == pending.end(),
        "Qt materialization acknowledgement removes the local node and "
        "all of its graph relations");
  }
}

void lateStartResultKeepsProviderItemOrder() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("provider-order-thread")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "provider-order-thread"});
  }

  NodeAction action{thread, NodeActionKind::SubmitPrompt};
  action.promptText = "Opening prompt";
  PromptTransition admitted = logic.admitPrompt(std::move(action));
  if (!admitted.command) {
    require(false, "provider-order fixture did not admit its opening prompt");
    return;
  }
  const NodeRef localPrompt = admitted.command->localPrompt;
  const std::string clientId = admitted.command->clientUserMessageId;
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.markPromptDispatched(
      localPrompt, ProtocolRequestId("provider-order-request")));
  static_cast<void>(takeWorkerMessages(channels));

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("provider-order-thread")},
           {"turn", Value(Value::Object{
                        {"id", Value("provider-order-turn")},
                        {"status", Value("inProgress")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
       Value::Object{
           {"threadId", Value("provider-order-thread")},
           {"turnId", Value("provider-order-turn")},
           {"item", Value(Value::Object{
                        {"id", Value("provider-reasoning")},
                        {"type", Value("reasoning")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
       Value::Object{
           {"threadId", Value("provider-order-thread")},
           {"turnId", Value("provider-order-turn")},
           {"item", Value(Value::Object{
                        {"id", Value("provider-user")},
                        {"type", Value("userMessage")},
                        {"clientId", Value(clientId)},
                        {"text", Value("Opening prompt")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));

  PromptTransition completed = logic.completePrompt(
      localPrompt, true, {}, std::string("provider-order-turn"));
  static_cast<void>(takeWorkerMessages(channels));
  require(!completed.command,
          "provider-order fixture unexpectedly dispatched another prompt");

  auto read = graph.tryRead();
  const NodeRef turn =
      read->find(scopedTurnNodeId("provider-order-thread", "provider-order-turn"));
  const NodeRef reasoning = read->find(scopedItemNodeId(
      turn->id(), "provider-reasoning"));
  const NodeRef authoritative =
      read->find(scopedItemNodeId(turn->id(), "provider-user"));
  require(turn && reasoning && authoritative &&
              read->children(turn) ==
                  std::vector<NodeRef>{reasoning, authoritative, localPrompt},
          "a late start result rewrote provider item order around its private "
          "optimistic prompt");
  require(read->related(turn, RelationKind::TurnRootItem) ==
                  std::vector<NodeRef>{authoritative} &&
              read->related(authoritative,
                            RelationKind::PromptMaterialization) ==
                  std::vector<NodeRef>{localPrompt},
          "provider-order preservation lost authoritative root or visual "
          "identity correlation");
}

void earlyMaterializationWaitsForTheExactRequestResult() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{{"id", Value("early-materialization")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "early-materialization"});
  }

  NodeAction first{thread, NodeActionKind::SubmitPrompt};
  first.promptText = "first request";
  PromptTransition admitted = logic.admitPrompt(std::move(first));
  require(admitted.command.has_value(), "the first request is dispatched");
  if (!admitted.command)
    return;
  const NodeRef firstPrompt = admitted.command->localPrompt;
  static_cast<void>(takeWorkerMessages(channels));

  NodeAction second{thread, NodeActionKind::SubmitPrompt};
  second.promptText = "second request";
  require(!logic.admitPrompt(std::move(second)).command,
          "the second request waits behind the first request result");
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.markPromptDispatched(
      firstPrompt, ProtocolRequestId("early-result")));
  static_cast<void>(takeWorkerMessages(channels));

  require(logic.promptMaterialized(firstPrompt) == ChannelSendStatus::Accepted,
          "an early authoritative widget handoff is acknowledged");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef retained = read->find(firstPrompt->id());
    const Value *materialized =
        retained ? field(read->state(retained), "uiMaterialized") : nullptr;
    require(retained == firstPrompt && materialized && materialized->asBool() &&
                *materialized->asBool(),
            "the current local node retains the dispatch slot until its exact "
            "JSON-RPC result");
  }

  PromptTransition completed = logic.completePrompt(firstPrompt, true);
  static_cast<void>(takeWorkerMessages(channels));
  require(completed.command &&
              completed.command->promptText == "second request",
          "the exact result advances one queued prompt after early "
          "materialization");
  {
    auto read = graph.tryRead();
    require(!read->find(firstPrompt->id()),
            "the handed-off local node retires with the completed request");
  }
}

void turnStartResultMakesTheAcceptedTurnActiveBeforeQueueAdvance() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("result-before-event")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "result-before-event"});
  }

  NodeAction first{thread, NodeActionKind::SubmitPrompt};
  first.promptText = "start";
  PromptTransition admitted = logic.admitPrompt(std::move(first));
  static_cast<void>(takeWorkerMessages(channels));
  NodeAction second{thread, NodeActionKind::SubmitPrompt};
  second.promptText = "queued steering";
  second.payload.emplace("model", "must-not-steer");
  second.payload.emplace("summary", "must-not-steer");
  static_cast<void>(logic.admitPrompt(std::move(second)));
  static_cast<void>(takeWorkerMessages(channels));

  PromptTransition next = logic.completePrompt(admitted.command->localPrompt,
                                               true, {}, "turn-from-result");
  static_cast<void>(takeWorkerMessages(channels));
  require(next.command && next.command->kind == PromptCommandKind::SteerTurn &&
              next.command->expectedTurnId == "turn-from-result" &&
              next.command->options.empty(),
          "an accepted turn/start result marks its turn active before the "
          "next exact queued prompt is selected without start settings");
  {
    auto read = graph.tryRead();
    const NodeRef turn =
        read->find(scopedTurnNodeId("result-before-event", "turn-from-result"));
    require(turn && read->state(turn)->status == NodeStatus::Running,
            "the result-created turn remains current until terminal traffic");
  }
}

void combinedResultsPublishOneAtomicGraphChange() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{{"id", Value("atomic-result-thread")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "atomic-result-thread"});
  }

  NodeAction promptAction{thread, NodeActionKind::SubmitPrompt};
  promptAction.promptText = "atomic prompt";
  PromptTransition admitted = logic.admitPrompt(std::move(promptAction));
  require(admitted.command.has_value(),
          "atomic prompt result setup admits the exact prompt");
  if (!admitted.command)
    return;
  const NodeRef localPrompt = admitted.command->localPrompt;
  static_cast<void>(takeWorkerMessages(channels));

  const ProtocolRequestId turnRequestId("atomic-turn-result");
  WorkerApplyResult turnRequest = logic.applyDetailed(
      {DecodedMessageKind::ClientRequest, "turn/start", turnRequestId,
       Value::Object{{"threadId", Value("atomic-result-thread")}}});
  require(turnRequest.primary &&
              turnRequest.primary->id() ==
                  NodeId{NodeKind::Operation, turnRequestId.canonical()},
          "turn request exposes its exact correlated operation");
  static_cast<void>(takeWorkerMessages(channels));

  const std::uint64_t beforePromptResult = graph.publishedRevision();
  Value::Array resultItems{Value(Value::Object{{"id", Value("reasoning")},
                                               {"type", Value("reasoning")},
                                               {"status", Value("running")}})};
  Value::Object resultTurn{{"id", Value("atomic-turn")},
                           {"status", Value("inProgress")},
                           {"items", Value(std::move(resultItems))}};
  DecodedMessage turnResult{
      DecodedMessageKind::ClientResult, "turn/start", turnRequestId,
      Value::Object{{"turn", Value(std::move(resultTurn))}},
      turnRequest.primary};
  PromptTransition completed = logic.completePromptResult(
      std::move(turnResult), localPrompt, true, {}, "atomic-turn");
  const std::vector<WorkerToQtMessage> promptMessages =
      takeWorkerMessages(channels);
  require(!completed.command &&
              graph.publishedRevision() == beforePromptResult + 1 &&
              promptMessages.size() == 1 &&
              std::holds_alternative<GraphChanged>(promptMessages.front()) &&
              std::get<GraphChanged>(promptMessages.front()).revision ==
                  graph.publishedRevision(),
          "operation retirement and prompt acceptance publish exactly one "
          "graph revision and notification");
  {
    auto read = graph.tryRead();
    const NodeRef turn =
        read->find(scopedTurnNodeId("atomic-result-thread", "atomic-turn"));
    const NodeRef reasoning = read->find(scopedItemNodeId(
        scopedTurnNodeId("atomic-result-thread", "atomic-turn"), "reasoning"));
    require(!read->find(turnRequest.primary->id()) && turn && reasoning &&
                read->children(turn) ==
                    std::vector<NodeRef>{localPrompt, reasoning} &&
                read->related(turn, RelationKind::TurnRootItem) ==
                    std::vector<NodeRef>{localPrompt} &&
                stringFieldEquals(read->state(localPrompt), "dispatchState",
                                  "awaitingMaterialization") &&
                read->changedRevision(turn) == read->revision() &&
                read->changedRevision(localPrompt) == read->revision(),
            "the single accepted-result revision contains the retired "
            "operation, acknowledged prompt, and starting prompt ordered "
            "ahead of provider reasoning with an explicit owning You root");
  }

  const ProtocolRequestId readRequestId("atomic-read-result");
  WorkerApplyResult readRequest = logic.applyDetailed(
      {DecodedMessageKind::ClientRequest, "thread/read", readRequestId,
       Value::Object{{"threadId", Value("atomic-result-thread")}}});
  require(static_cast<bool>(readRequest.primary),
          "thread/read exposes its exact correlated operation");
  static_cast<void>(takeWorkerMessages(channels));
  const std::uint64_t beforeReadResult = graph.publishedRevision();
  Value::Object hydratedThread{{"id", Value("atomic-result-thread")},
                               {"name", Value("Hydrated atomically")}};
  DecodedMessage readResult{
      DecodedMessageKind::ClientResult, "thread/read", readRequestId,
      Value::Object{{"thread", Value(std::move(hydratedThread))}},
      readRequest.primary};
  require(logic.completeThreadHydration(std::move(readResult), thread,
                                        "ready") == ChannelSendStatus::Accepted,
          "combined hydration result is admitted");
  const std::vector<WorkerToQtMessage> hydrationMessages =
      takeWorkerMessages(channels);
  require(graph.publishedRevision() == beforeReadResult + 1 &&
              hydrationMessages.size() == 1 &&
              std::holds_alternative<GraphChanged>(hydrationMessages.front()) &&
              std::get<GraphChanged>(hydrationMessages.front()).revision ==
                  graph.publishedRevision(),
          "thread/read replacement and hydration readiness publish exactly "
          "one graph revision and notification");
  {
    auto read = graph.tryRead();
    require(
        !read->find(readRequest.primary->id()) &&
            stringFieldEquals(read->state(thread), "name",
                              "Hydrated atomically") &&
            stringFieldEquals(read->state(thread), "hydrationState", "ready") &&
            read->changedRevision(thread) == read->revision(),
        "the one hydration revision exposes both provider state and "
        "local readiness after retiring its operation");
  }
}

void deletingAnAdmittedPromptPreservesExplicitRecovery() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{{"id", Value("deleted-prompt-thread")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "deleted-prompt-thread"});
  }

  NodeAction action{thread, NodeActionKind::SubmitPrompt};
  action.promptText = "keep this exact admitted prompt";
  PromptTransition admitted = logic.admitPrompt(std::move(action));
  require(admitted.command.has_value(),
          "prompt is admitted before its destination is deleted");
  if (!admitted.command)
    return;
  const NodeRef prompt = admitted.command->localPrompt;
  NodeRef formerTurn;
  {
    auto read = graph.tryRead();
    formerTurn = read->parent(prompt);
  }
  static_cast<void>(takeWorkerMessages(channels));

  const ProtocolRequestId requestId("deleted-prompt-result");
  DecodedMessage requestMessage{
      DecodedMessageKind::ClientRequest, "turn/start", requestId,
      Value::Object{{"threadId", Value("deleted-prompt-thread")}}};
  requestMessage.requestTarget = prompt;
  WorkerApplyResult request = logic.applyDetailed(std::move(requestMessage));
  require(static_cast<bool>(request.primary),
          "admitted prompt request retains its exact operation");
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.markPromptDispatched(prompt, requestId));
  static_cast<void>(takeWorkerMessages(channels));

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("deleted-prompt-thread")}}}));
  const std::vector<WorkerToQtMessage> deletionMessages =
      takeWorkerMessages(channels);
  NodeRef recoveryThread;
  NodeRef recoveryTurn;
  {
    auto read = graph.tryRead();
    recoveryTurn = read->parent(prompt);
    recoveryThread = recoveryTurn ? read->parent(recoveryTurn) : NodeRef{};
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const Value *requiresRecovery =
        field(read->state(prompt), "requiresExplicitRecovery");
    require(
        deletionMessages.size() == 1 &&
            std::holds_alternative<GraphChanged>(deletionMessages.front()) &&
            !read->find(thread->id()) && !read->find(formerTurn->id()) &&
            read->find(prompt->id()) == prompt && recoveryThread &&
            recoveryTurn &&
            recoveryThread->id().canonical.starts_with(
                "local-recovery-thread:removed:") &&
            stringFieldEquals(read->state(prompt), "text",
                              "keep this exact admitted prompt") &&
            stringFieldEquals(read->state(prompt), "dispatchState",
                              "uncertain") &&
            requiresRecovery && requiresRecovery->asBool() &&
            *requiresRecovery->asBool() && request.primary &&
            read->find(request.primary->id()) == request.primary &&
            read->related(runtime, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{prompt} &&
            read->related(recoveryThread, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{prompt},
        "thread deletion removes the former owners while reparenting the "
        "same authored NodeRef and text to explicit recovery");
  }

  const std::uint64_t beforeLateResult = graph.publishedRevision();
  PromptTransition late = logic.completePromptResult(
      {DecodedMessageKind::ClientResult, "turn/start", requestId,
       Value::Object{
           {"threadId", Value("deleted-prompt-thread")},
           {"turn", Value(Value::Object{{"id", Value("late-deleted-turn")},
                                        {"status", Value("inProgress")}})}},
       request.primary},
      prompt, true, {}, "late-deleted-turn");
  const std::vector<WorkerToQtMessage> lateMessages =
      takeWorkerMessages(channels);
  require(!late.command && graph.publishedRevision() == beforeLateResult + 1 &&
              lateMessages.size() == 1 &&
              std::holds_alternative<GraphChanged>(lateMessages.front()),
          "late exact result publishes only its operation retirement");
  {
    auto read = graph.tryRead();
    require(!read->find(request.primary->id()) &&
                !read->find({NodeKind::Thread, "deleted-prompt-thread"}) &&
                !read->find(scopedTurnNodeId("deleted-prompt-thread",
                                             "late-deleted-turn")) &&
                read->find(prompt->id()) == prompt &&
                read->parent(prompt) == recoveryTurn &&
                read->parent(recoveryTurn) == recoveryThread &&
                read->state(prompt)->status == NodeStatus::Failed &&
                stringFieldEquals(read->state(prompt), "text",
                                  "keep this exact admitted prompt") &&
                stringFieldEquals(read->state(prompt), "dispatchState",
                                  "uncertain"),
            "a late result cannot recreate the deleted destination or "
            "silently acknowledge its recovered prompt");
  }
}

void activeAgentChildrenAreCurrentAndDeduplicated() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);
  NodeRef parent;
  NodeRef activeChild;
  {
    auto write = graph.write();
    parent = write.upsert({NodeKind::Thread, "agent-parent"});
    NodeRef turn = write.upsert({NodeKind::Turn, "agent-parent-turn"});
    NodeRef pending = write.upsert({NodeKind::Item, "pending-agent-item"});
    NodeRef running = write.upsert({NodeKind::Item, "running-agent-item"});
    NodeRef fieldActive =
        write.upsert({NodeKind::Item, "field-active-agent-item"});
    NodeRef completed = write.upsert({NodeKind::Item, "completed-agent-item"});
    NodeRef unrelated = write.upsert({NodeKind::Item, "unrelated-agent-item"});
    activeChild = write.upsert({NodeKind::Thread, "active-agent-child"});
    const NodeRef fieldActiveChild =
        write.upsert({NodeKind::Thread, "field-active-agent-child"});
    NodeRef inactiveChild =
        write.upsert({NodeKind::Thread, "inactive-agent-child"});
    write.setParent(parent, turn);
    for (const NodeRef &item :
         {pending, running, fieldActive, completed, unrelated})
      write.setParent(turn, item);
    write.setStatus(pending, NodeStatus::Pending);
    write.setStatus(running, NodeStatus::Running);
    write.setField(fieldActive, "status", Value("inProgress"));
    write.setStatus(completed, NodeStatus::Completed);
    write.relate(pending, RelationKind::AgentChildThread, activeChild);
    write.relate(running, RelationKind::AgentChildThread, activeChild);
    write.relate(fieldActive, RelationKind::AgentChildThread, fieldActiveChild);
    write.relate(completed, RelationKind::AgentChildThread, inactiveChild);
    write.relate(unrelated, RelationKind::StructuralChildThread, inactiveChild);
    static_cast<void>(write.finish());
  }

  const std::uint64_t before = graph.publishedRevision();
  const std::vector<NodeRef> children = logic.activeAgentChildren(parent);
  require(children == std::vector<NodeRef>{activeChild} &&
              graph.publishedRevision() == before &&
              channels.workerToQtSizeApprox() == 0,
          "activeAgentChildren trusts typed live-item status, deduplicates "
          "NodeRefs, and is revision-neutral");
}

void inactiveThreadStartsInsteadOfSteeringStaleHistory() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("idle-prompt-thread")}})}}}));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("idle-prompt-thread")},
           {"turn", Value(Value::Object{{"id", Value("stale-running-turn")},
                                        {"status", Value("inProgress")}})}}}));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/status/changed",
       std::nullopt,
       Value::Object{
           {"threadId", Value("idle-prompt-thread")},
           {"status", Value(Value::Object{{"type", Value("idle")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));

  NodeRef thread;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "idle-prompt-thread"});
  }
  NodeAction prompt{thread, NodeActionKind::SubmitPrompt};
  prompt.promptText = "new turn after idle";
  const PromptTransition transition = logic.admitPrompt(std::move(prompt));
  require(transition.command &&
              transition.command->kind == PromptCommandKind::StartTurn,
          "prompt admission trusts the maintained active-turn relation and "
          "never steers a stale Running child after the thread becomes idle");
}

void stalePromptTargetsRetainAuthoredInputInRecoveryNodes() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("removed-destination")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef staleThread;
  {
    auto read = graph.tryRead();
    staleThread = read->find({NodeKind::Thread, "removed-destination"});
  }
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("removed-destination")}}}));
  static_cast<void>(takeWorkerMessages(channels));

  NodeAction action{staleThread, NodeActionKind::SubmitPrompt};
  action.promptText = "retain this authored prompt";
  action.attachments.push_back(
      {"/tmp/recovery.png", "recovery.png", "image/png", std::nullopt});
  PromptTransition retained = logic.admitPrompt(std::move(action));
  require(!retained.command,
          "a stale target is never dispatched by canonical id");
  static_cast<void>(takeWorkerMessages(channels));

  auto read = graph.tryRead();
  const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
  const NodeRef notice =
      read->find({NodeKind::Notice, "local-worker-notice"});
  const std::vector<NodeRef> roots =
      read->related(runtime, RelationKind::RootThread);
  const NodeRef recovery =
      roots.empty() || !roots.front()->id().canonical.starts_with(
                           "local-recovery-thread:")
          ? NodeRef{}
          : roots.front();
  const NodeRef turn = recovery && read->childCount(recovery) != 0
                           ? read->childAt(recovery, 0)
                           : NodeRef{};
  const NodeRef prompt =
      turn && read->childCount(turn) != 0 ? read->childAt(turn, 0) : NodeRef{};
  const auto state = prompt ? read->state(prompt) : nullptr;
  const Value *attachments = state ? field(state, "attachments") : nullptr;
  require(notice &&
              stringFieldEquals(read->state(notice), "message",
                                "The destination thread is no longer available") &&
              recovery && turn && prompt && state &&
              state->status == NodeStatus::Failed &&
              stringFieldEquals(state, "text", "retain this authored prompt") &&
              stringFieldEquals(state, "dispatchState", "failed") &&
              attachments && attachments->asArray() &&
              attachments->asArray()->size() == 1 &&
              read->related(runtime, RelationKind::PendingPrompt) ==
                  std::vector<NodeRef>{prompt},
          "worker-side rejection exposes one authoritative notice and keeps "
          "exact authored input in a visible recovery graph node");
}

void recoveryOnlyTargetsCannotDispatchAndRetainAuthoredInput() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  NodeRef recoveryOnlyThread;
  {
    auto write = graph.write();
    NodeState state;
    state.status = NodeStatus::Failed;
    state.fields = {{"type", Value("localRecoveryThread")},
                    {"local", Value(true)},
                    {"recoveryOnly", Value(true)},
                    {"name", Value("Unsent prompt")}};
    recoveryOnlyThread = write.upsert(
        {NodeKind::Thread, "local-recovery-thread:existing"}, std::move(state));
    NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
    write.relate(runtime, RelationKind::RootThread, recoveryOnlyThread);
    static_cast<void>(write.finish());
  }

  NodeAction action{recoveryOnlyThread, NodeActionKind::SubmitPrompt};
  action.promptText = "do not dispatch this recovery text";
  action.attachments.push_back({"/tmp/recovery-again.txt", "recovery-again.txt",
                                "text/plain", std::nullopt});
  PromptTransition retained = logic.admitPrompt(std::move(action));
  require(!retained.command,
          "a live recovery-only target cannot produce a provider command");
  static_cast<void>(takeWorkerMessages(channels));

  auto read = graph.tryRead();
  const NodeRef notice =
      read->find({NodeKind::Notice, "local-worker-notice"});
  NodeRef retainedPrompt;
  for (const NodeRef &node : read->orderedNodes()) {
    if (node->id().kind != NodeKind::Item)
      continue;
    const auto state = read->state(node);
    if (stringFieldEquals(state, "authoredText",
                          "do not dispatch this recovery text")) {
      retainedPrompt = node;
      break;
    }
  }
  const NodeRef retainedTurn =
      retainedPrompt ? read->parent(retainedPrompt) : NodeRef{};
  const NodeRef retainedThread =
      retainedTurn ? read->parent(retainedTurn) : NodeRef{};
  const auto state = retainedPrompt ? read->state(retainedPrompt) : nullptr;
  const Value *attachments = state ? field(state, "attachments") : nullptr;
  const Value *requiresRecovery =
      state ? field(state, "requiresExplicitRecovery") : nullptr;
  require(notice &&
              stringFieldEquals(
                  read->state(notice), "message",
                  "Restore this unsent prompt before sending it again") &&
              retainedPrompt && retainedThread &&
              retainedThread != recoveryOnlyThread &&
              retainedThread->id().canonical.starts_with(
                  "local-recovery-thread:") &&
              state->status == NodeStatus::Failed &&
              stringFieldEquals(state, "dispatchState", "failed") &&
              stringFieldEquals(
                  state, "error",
                  "Restore this unsent prompt before sending it again") &&
              attachments && attachments->asArray() &&
              attachments->asArray()->size() == 1 && requiresRecovery &&
              requiresRecovery->asBool() && *requiresRecovery->asBool(),
          "recovery-only rejection keeps exact authored input in a separate "
          "explicit recovery node");
}

void failedHydrationTargetsRetainAuthoredInput() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);
  NodeRef thread;
  {
    auto write = graph.write();
    NodeState state;
    state.status = NodeStatus::NotLoaded;
    state.fields = {{"hydrationState", Value("failed")},
                    {"hydrationError", Value("history unavailable")}};
    thread =
        write.upsert({NodeKind::Thread, "failed-hydration"}, std::move(state));
    static_cast<void>(write.finish());
  }

  NodeAction action{thread, NodeActionKind::SubmitPrompt};
  action.promptText = "keep after a stale visible frame";
  action.attachments.push_back(
      {"/tmp/retained.txt", "retained.txt", "text/plain", std::nullopt});
  PromptTransition transition = logic.admitPrompt(std::move(action));
  require(!transition.command,
          "failed hydration is revalidated before provider dispatch");
  static_cast<void>(takeWorkerMessages(channels));
  auto read = graph.tryRead();
  NodeRef retainedPrompt;
  for (const NodeRef &node : read->orderedNodes()) {
    if (node->id().kind == NodeKind::Item &&
        stringFieldEquals(read->state(node), "authoredText",
                          "keep after a stale visible frame")) {
      retainedPrompt = node;
      break;
    }
  }
  const auto state = retainedPrompt ? read->state(retainedPrompt) : nullptr;
  const Value *attachments = field(state, "attachments");
  require(retainedPrompt && state->status == NodeStatus::Failed &&
              stringFieldEquals(state, "dispatchState", "failed") &&
              attachments && attachments->asArray() &&
              attachments->asArray()->size() == 1,
          "failed hydration keeps the moved prompt and attachment metadata "
          "in explicit recovery state");
}

void firstPromptCreatesAndMigratesOneDraftThread() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  RuntimeAction action;
  action.kind = RuntimeActionKind::CreateThread;
  action.promptText = "first prompt exactly";
  action.payload = {
      {"threadStart", Value(Value::Object{{"cwd", Value("/workspace")},
                                          {"model", Value("gpt-current")}})},
      {"turnStart",
       Value(Value::Object{{"approvalPolicy", Value("on-request")}})},
      {"requestedName", Value("Named locally")}};
  PromptTransition admitted =
      logic.admitFirstPrompt(std::move(action), 100, 100'000);
  require(admitted.command &&
              admitted.command->kind == PromptCommandKind::CreateThread &&
              admitted.command->requestedName == "Named locally" &&
              admitted.command->options.contains("cwd") &&
              !admitted.command->options.contains("requestedName") &&
              admitted.command->turnOptions.contains("approvalPolicy"),
          "new-thread admission separates exact thread, turn, and rename "
          "parameters");
  const NodeRef draft = admitted.command ? admitted.command->thread : NodeRef{};
  const NodeRef localPrompt =
      admitted.command ? admitted.command->localPrompt : NodeRef{};
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    require(
        draft && draft->id().canonical.starts_with("local-thread:") &&
            read->related(runtime, RelationKind::RootThread).front() == draft &&
            read->related(runtime, RelationKind::UiSelectionTarget) ==
                std::vector<NodeRef>{draft} &&
            read->parent(read->parent(localPrompt)) == draft &&
            stringFieldEquals(read->state(draft), "name", "Named locally") &&
            stringFieldEquals(read->state(draft), "cwd", "/workspace") &&
            stringFieldEquals(read->state(draft), "creationCorrelation",
                              admitted.command->creationCorrelation),
        "the first prompt is immediately renderable under one draft "
        "thread with its immutable identity, chosen name, workspace, and "
        "provisional turn");
  }

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("created-thread")},
                                    {"name", Value("Provider default")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  require(admitted.command &&
              logic.attachCreatedThread(*admitted.command, "created-thread") ==
                  ChannelSendStatus::Accepted,
          "thread/start result migrates the draft prompt atomically");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef actual = read->find({NodeKind::Thread, "created-thread"});
    require(
        admitted.command->kind == PromptCommandKind::StartTurn &&
            admitted.command->thread == actual &&
            admitted.command->options.contains("approvalPolicy") &&
            !read->find(draft->id()) &&
            read->parent(read->parent(localPrompt)) == actual &&
            read->related(actual, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{localPrompt} &&
            stringFieldEquals(read->state(actual), "name", "Named locally") &&
            stringFieldEquals(read->state(actual), "localNameOverlay",
                              "Named locally") &&
            stringFieldEquals(read->state(actual), "cwd", "/workspace") &&
            stringFieldEquals(read->state(actual), "creationCorrelation",
                              admitted.command->creationCorrelation) &&
            read->related(read->find({NodeKind::Runtime, "runtime"}),
                          RelationKind::UiSelectionTarget) ==
                std::vector<NodeRef>{actual},
        "migration removes the draft shell, preserves presentation identity, "
        "name, and workspace, selects the canonical thread, and changes the "
        "retained command to turn/start");
  }
  static_cast<void>(
      logic.completePrompt(localPrompt, true, {}, "created-turn"));
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef actual = read->find({NodeKind::Thread, "created-thread"});
    const Value *sortActivity =
        field(read->state(localPrompt), "sortActivityAt");
    const std::int64_t *timestamp =
        sortActivity ? sortActivity->asInt64() : nullptr;
    require(timestamp &&
                signedFieldEquals(read->state(actual),
                                  "confirmedLocalPromptActivityAt",
                                  *timestamp) &&
                signedFieldEquals(read->state(actual), "localPromptActivityAt",
                                  *timestamp) &&
                stringFieldEquals(read->state(actual), "localNameOverlay",
                                  "Named locally"),
            "turn acknowledgement confirms Recent without dropping the "
            "chosen-name overlay");
  }

  NodeGraph failedGraph;
  ThreadChannels failedChannels;
  WorkerLogic failedLogic(failedGraph, failedChannels);
  RuntimeAction failingCreate;
  failingCreate.kind = RuntimeActionKind::CreateThread;
  failingCreate.correlation = "failing-draft";
  failingCreate.promptText = "first unsent draft";
  failingCreate.payload = {
      {"requestedName", Value("Recover this name")},
      {"threadStart",
       Value(
           Value::Object{{"cwd", Value("/recovery-workspace")},
                         {"baseInstructions", Value("base recovery")},
                         {"developerInstructions", Value("developer recovery")},
                         {"ephemeral", Value(true)}})},
      {"turnStart", Value(Value::Object{{"approvalPolicy", Value("never")}})}};
  PromptTransition firstDraft =
      failedLogic.admitFirstPrompt(std::move(failingCreate));
  require(firstDraft.command.has_value(),
          "failing draft is admitted before its provider failure");
  if (!firstDraft.command)
    return;
  NodeAction laterDraft;
  laterDraft.kind = NodeActionKind::SubmitPrompt;
  laterDraft.target = firstDraft.command->thread;
  laterDraft.promptText = "second unsent draft";
  PromptTransition queuedDraft = failedLogic.admitPrompt(std::move(laterDraft));
  require(!queuedDraft.command,
          "later input queues behind the in-flight draft creation");
  PromptTransition failed = failedLogic.failPrompt(
      firstDraft.command->localPrompt, "thread creation failed");
  {
    auto read = failedGraph.tryRead();
    const std::vector<NodeRef> prompts =
        read->related(firstDraft.command->thread, RelationKind::PendingPrompt);
    const auto firstState = prompts.empty() ? std::shared_ptr<const NodeState>{}
                                            : read->state(prompts.front());
    const Value *threadOptions = field(firstState, "threadStartOptions");
    require(!failed.command &&
                read->state(firstDraft.command->thread)->status ==
                    NodeStatus::Failed &&
                prompts.size() == 2 &&
                std::ranges::all_of(prompts,
                                    [&](const NodeRef &prompt) {
                                      return read->state(prompt)->status ==
                                                 NodeStatus::Failed &&
                                             stringFieldEquals(
                                                 read->state(prompt),
                                                 "dispatchState", "failed");
                                    }),
            "failed thread creation keeps every authored draft visible and "
            "never auto-dispatches one against a local thread id");
    require(firstState &&
                stringFieldEquals(firstState, "requestedName",
                                  "Recover this name") &&
                stringFieldEquals(firstState, "creationCorrelation",
                                  "failing-draft") &&
                threadOptions && threadOptions->asObject() &&
                threadOptions->asObject()->contains("baseInstructions") &&
                threadOptions->asObject()->contains("developerInstructions") &&
                threadOptions->asObject()->contains("ephemeral"),
            "failed creation retains every modal-authored option for explicit "
            "recovery");
  }
}

void creationCorrelationSharesExactlyOneDraft() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  RuntimeAction first;
  first.kind = RuntimeActionKind::CreateThread;
  first.correlation = "draft-token";
  first.promptText = "first correlated prompt";
  first.payload = {
      {"threadStart", Value(Value::Object{{"cwd", Value("/workspace")}})},
      {"turnStart",
       Value(Value::Object{{"approvalPolicy", Value("on-request")}})}};
  PromptTransition admitted = logic.admitFirstPrompt(std::move(first));
  require(admitted.command &&
              admitted.command->kind == PromptCommandKind::CreateThread,
          "the first correlated action owns thread creation");
  if (!admitted.command)
    return;
  const NodeRef draft = admitted.command->thread;
  static_cast<void>(takeWorkerMessages(channels));

  RuntimeAction second;
  second.kind = RuntimeActionKind::CreateThread;
  second.correlation = "draft-token";
  second.promptText = "second correlated prompt";
  second.payload = {
      {"threadStart", Value(Value::Object{{"cwd", Value("/workspace")}})},
      {"turnStart", Value(Value::Object{{"model", Value("gpt-current")}})}};
  PromptTransition queued = logic.admitFirstPrompt(std::move(second));
  require(!queued.command,
          "a second action for the same draft queues behind one creation");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const std::vector<NodeRef> prompts =
        read->related(draft, RelationKind::PendingPrompt);
    require(read->related(runtime, RelationKind::RootThread) ==
                    std::vector<NodeRef>{draft} &&
                prompts.size() == 2,
            "same-correlation actions create one local thread with both "
            "stable prompt nodes");
  }

  require(logic.attachCreatedThread(*admitted.command, "correlated-thread") ==
              ChannelSendStatus::Accepted,
          "the single draft is promoted to its canonical thread");
  static_cast<void>(takeWorkerMessages(channels));
  PromptTransition next = logic.completePrompt(admitted.command->localPrompt,
                                               true, {}, "correlated-turn");
  require(next.command && next.command->thread &&
              next.command->thread->id().canonical == "correlated-thread" &&
              (next.command->kind == PromptCommandKind::StartTurn ||
               next.command->kind == PromptCommandKind::SteerTurn) &&
              next.command->promptText == "second correlated prompt",
          "promotion advances the second prompt only against the canonical "
          "thread without a second thread/start");

  RuntimeAction distinct;
  distinct.kind = RuntimeActionKind::CreateThread;
  distinct.correlation = "other-draft-token";
  distinct.promptText = "independent draft";
  PromptTransition separate = logic.admitFirstPrompt(std::move(distinct));
  require(separate.command &&
              separate.command->kind == PromptCommandKind::CreateThread &&
              separate.command->thread != next.command->thread,
          "a distinct correlation creates an independent draft");
}

void providerGenerationResetIsAtomicAndKeepsOnlyRecoveryPrompts() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.connectionSettings(
      {{"selected", Value("unix")}, {"endpoint", Value("local")}}));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.transportEvent("connected"));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(
      logic.bridgeState("bridge", "controller", "bridge", 7, "ready"));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("reset-thread")},
                                    {"name", Value("Provider fact")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("reset-thread")},
           {"turn", Value(Value::Object{{"id", Value("reset-turn")},
                                        {"status", Value("inProgress")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef thread;
  NodeRef turn;
  {
    auto read = graph.tryRead();
    thread = read->find({NodeKind::Thread, "reset-thread"});
    turn = read->find(scopedTurnNodeId("reset-thread", "reset-turn"));
  }
  NodeAction action;
  action.kind = NodeActionKind::SubmitPrompt;
  action.target = thread;
  action.promptText = "recover this exact text";
  PromptTransition prompt = logic.admitPrompt(std::move(action));
  const NodeRef localPrompt =
      prompt.command ? prompt.command->localPrompt : NodeRef{};
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(
      logic.apply({DecodedMessageKind::ClientRequest, "model/list",
                   ProtocolRequestId("pending-catalog"), Value::Object{}}));
  static_cast<void>(takeWorkerMessages(channels));
  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerRequest,
       "item/commandExecution/requestApproval", ProtocolRequestId("pending"),
       Value::Object{{"threadId", Value("reset-thread")}}}));
  static_cast<void>(takeWorkerMessages(channels));
  NodeRef interaction;
  {
    auto read = graph.tryRead();
    interaction = read->find(
        {NodeKind::Interaction, ProtocolRequestId("pending").canonical()});
  }

  const std::uint64_t before = graph.publishedRevision();
  require(logic.bridgeState("bridge", "controller", "bridge", 8, "ready",
                            "provider replaced") == ChannelSendStatus::Accepted,
          "new provider generation is admitted");
  const std::vector<WorkerToQtMessage> resetMessages =
      takeWorkerMessages(channels);
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef connection = read->find({NodeKind::Connection, "connection"});
    const NodeRef owner = read->find({NodeKind::Thread, "reset-thread"});
    const NodeRef recoveryTurn = read->parent(localPrompt);
    const NodeRef recoveryOwner =
        recoveryTurn ? read->parent(recoveryTurn) : NodeRef{};
    bool onlyAllowed = true;
    for (const NodeRef &node : read->orderedNodes()) {
      onlyAllowed &= node->id().kind == NodeKind::Runtime ||
                     node->id().kind == NodeKind::Connection ||
                     node->id().kind == NodeKind::Thread ||
                     node->id().kind == NodeKind::Turn ||
                     node->id().kind == NodeKind::Item ||
                     node->id().kind == NodeKind::Interaction;
    }
    const auto promptState = read->state(localPrompt);
    const Value *recovery = field(promptState, "requiresExplicitRecovery");
    const Value *settings = field(read->state(connection), "settings");
    require(
        read->revision() == before + 1 && resetMessages.size() == 1 &&
            std::holds_alternative<GraphChanged>(resetMessages.front()) &&
            onlyAllowed && runtime && connection && !owner &&
            !read->find(scopedTurnNodeId("reset-thread", "reset-turn")) &&
            localPrompt && recoveryOwner &&
            recoveryOwner->id().canonical.starts_with(
                "local-recovery-thread:") &&
            recoveryTurn->id().canonical.starts_with("local-recovery-turn:") &&
            !read->find({NodeKind::Operation,
                         ProtocolRequestId("pending-catalog").canonical()}) &&
            read->find({NodeKind::Interaction,
                        ProtocolRequestId("pending").canonical()}) ==
                interaction &&
            read->state(interaction)->status == NodeStatus::Failed &&
            stringFieldEquals(read->state(interaction), "error",
                              "provider replaced") &&
            field(read->state(interaction), "recoveryOnly") &&
            field(read->state(interaction), "recoveryOnly")->asBool() &&
            *field(read->state(interaction), "recoveryOnly")->asBool() &&
            read->related(interaction, RelationKind::InteractionTarget)
                .empty() &&
            promptState->status == NodeStatus::Failed &&
            stringFieldEquals(promptState, "dispatchState", "uncertain") &&
            recovery && recovery->asBool() && *recovery->asBool() && settings &&
            settings->asObject() &&
            read->related(runtime, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{localPrompt} &&
            read->related(runtime, RelationKind::PendingInteraction) ==
                std::vector<NodeRef>{interaction} &&
            read->related(recoveryOwner, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{localPrompt},
        "provider reset publishes one complete revision, removes stale "
        "canonical owners and work, preserves settings, and retains only "
        "explicit user-input recovery state");
  }
  require(logic.generations() == WorkerGenerations{1, 8},
          "callback generation snapshot advances with the atomic reset");

  static_cast<void>(logic.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("reset-thread")},
           {"turn", Value(Value::Object{{"id", Value("reset-turn")},
                                        {"status", Value("inProgress")}})}}}));
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef reusedThread = read->find({NodeKind::Thread, "reset-thread"});
    const NodeRef reusedTurn =
        read->find(scopedTurnNodeId("reset-thread", "reset-turn"));
    const auto threadState = read->state(reusedThread);
    const auto turnState = read->state(reusedTurn);
    const NodeRef recoveryOwner = read->parent(read->parent(localPrompt));
    require(
        reusedThread && reusedThread != thread && reusedTurn &&
            reusedTurn != turn && !field(threadState, "local") &&
            !field(threadState, "recoveryOnly") && !field(turnState, "local") &&
            recoveryOwner && recoveryOwner != reusedThread &&
            read->related(reusedThread, RelationKind::PendingPrompt).empty(),
        "reused provider Thread/Turn ids allocate fresh nodes without "
        "inheriting recovery state or stealing the retained prompt");
  }

  const WorkerApplyResult reusedRequest = logic.applyDetailed(
      {DecodedMessageKind::ServerRequest,
       "item/commandExecution/requestApproval", ProtocolRequestId("pending"),
       Value::Object{{"threadId", Value("reset-thread")},
                     {"turnId", Value("reset-turn")}}});
  static_cast<void>(takeWorkerMessages(channels));
  const NodeRef currentInteraction = reusedRequest.primary;
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef reusedThread = read->find({NodeKind::Thread, "reset-thread"});
    const NodeRef reusedTurn =
        read->find(scopedTurnNodeId("reset-thread", "reset-turn"));
    const auto currentState =
        currentInteraction ? read->state(currentInteraction) : nullptr;
    require(currentInteraction && currentInteraction != interaction &&
                currentInteraction->id().canonical !=
                    ProtocolRequestId("pending").canonical() &&
                read->find({NodeKind::Interaction,
                            ProtocolRequestId("pending").canonical()}) ==
                    interaction &&
                read->state(interaction)->status == NodeStatus::Failed &&
                currentState->status == NodeStatus::Pending &&
                unsignedFieldEquals(currentState, "connectionGeneration", 1) &&
                unsignedFieldEquals(currentState, "providerGeneration", 8) &&
                runtime &&
                read->related(runtime, RelationKind::PendingInteraction) ==
                    std::vector<NodeRef>{interaction, currentInteraction} &&
                read->related(currentInteraction,
                              RelationKind::InteractionTarget) ==
                    std::vector<NodeRef>{reusedTurn} &&
                read->parent(reusedTurn) == reusedThread &&
                read->related(reusedThread,
                              RelationKind::PendingInteraction).empty(),
            "a replacement provider can reuse a wire id without replacing the "
            "older recovery interaction or conflating thread attention");
  }

  require(logic.resolveInteraction(currentInteraction) ==
              ChannelSendStatus::Accepted,
          "the current generation interaction resolves by exact NodeRef");
  static_cast<void>(takeWorkerMessages(channels));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    require(read->find(interaction->id()) == interaction &&
                read->state(interaction)->status == NodeStatus::Failed &&
                !read->find(currentInteraction->id()) &&
                runtime &&
                read->related(runtime, RelationKind::PendingInteraction) ==
                    std::vector<NodeRef>{interaction},
            "resolving the new generation leaves the older recovery record "
            "intact and clears only current runtime membership");
  }
}

void providerTurnErrorsKeepTheirAuthoritativeNotice() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  require(logic.apply(
              {DecodedMessageKind::ServerNotification, "error", std::nullopt,
               Value::Object{
                   {"threadId", Value("notice-error-thread")},
                   {"turnId", Value("notice-error-turn")},
                   {"error", Value(Value::Object{
                                 {"message", Value("provider retry failed")}})},
                   {"willRetry", Value(false)}}}) ==
              ChannelSendStatus::Accepted,
          "an addressed provider error publishes normally");
  static_cast<void>(takeWorkerMessages(channels));

  auto read = graph.tryRead();
  const NodeRef notice =
      read->find({NodeKind::Notice, "provider-notice"});
  const NodeRef turn =
      read->find(scopedTurnNodeId("notice-error-thread", "notice-error-turn"));
  const auto state = turn ? read->state(turn) : nullptr;
  const Value *willRetry = field(state, "willRetry");
  require(notice &&
              stringFieldEquals(read->state(notice), "noticeText",
                                "provider retry failed") &&
              turn && field(state, "error") && willRetry && willRetry->asBool() &&
              !*willRetry->asBool(),
          "the worker retains one authoritative notice beside the addressed "
          "turn error facts");
}

void workerStoppedDeliveryIsExplicit() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  require(logic.sendWorkerStopped("normal shutdown") ==
              ChannelSendStatus::Accepted,
          "WorkerStopped is admitted normally");
  const EventFd::DrainResult wake = channels.drainWorkerToQtWake();
  WorkerToQtMessage message;
  require(wake.status == EventFd::DrainStatus::Drained && wake.count == 1 &&
              channels.tryReceiveForQt(message) &&
              std::holds_alternative<WorkerStopped>(message) &&
              std::get<WorkerStopped>(message).reason == "normal shutdown",
          "WorkerStopped arrives as the typed terminal message");

  ThreadChannels closedChannels;
  WorkerLogic closedLogic(graph, closedChannels);
  closedChannels.close();
  const ChannelSendStatus failedWake =
      closedLogic.sendWorkerStopped("wake already closed");
  require(
      failedWake == ChannelSendStatus::QueueFull &&
          !messageAdmitted(failedWake) && !wakeFailed(failedWake) &&
          closedChannels.workerToQtSizeApprox() == 0,
      "WorkerStopped rejects cleanly once both channel directions are closed");
}

} // namespace

int main() {
  protocolUpdatesPublishForUnlockedReads();
  connectionStateAndGenerationsStayCurrent();
  stateNeutralMessagesDoNotWakeQt();
  hydrationReadinessIsCurrentGraphState();
  hydrationCompletionUsesExactOperationAuthority();
  hydrationCompletionWorkIsIndependentOfRetainedItems();
  graphNotificationSaturationCoalesces();
  uiDetachAcknowledgementIsRevisionNeutral();
  authoritativeUiStateSurvivesQueueSaturation();
  reverseInteractionResolutionUpdatesTheGraph();
  threadActivityAndPromptOrderingStayInTheGraph();
  localPromptsAreGraphNodesAndDispatchPerThread();
  lateStartResultKeepsProviderItemOrder();
  earlyMaterializationWaitsForTheExactRequestResult();
  turnStartResultMakesTheAcceptedTurnActiveBeforeQueueAdvance();
  combinedResultsPublishOneAtomicGraphChange();
  deletingAnAdmittedPromptPreservesExplicitRecovery();
  activeAgentChildrenAreCurrentAndDeduplicated();
  inactiveThreadStartsInsteadOfSteeringStaleHistory();
  stalePromptTargetsRetainAuthoredInputInRecoveryNodes();
  recoveryOnlyTargetsCannotDispatchAndRetainAuthoredInput();
  failedHydrationTargetsRetainAuthoredInput();
  firstPromptCreatesAndMigratesOneDraftThread();
  creationCorrelationSharesExactlyOneDraft();
  providerGenerationResetIsAtomicAndKeepsOnlyRecoveryPrompts();
  providerTurnErrorsKeepTheirAuthoritativeNotice();
  workerStoppedDeliveryIsExplicit();

  if (failures != 0) {
    std::cerr << failures << " worker logic assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "codexui worker logic tests passed\n";
  return EXIT_SUCCESS;
}
