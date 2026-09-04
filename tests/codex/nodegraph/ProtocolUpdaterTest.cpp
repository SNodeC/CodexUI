// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/ProtocolCatalog.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
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

DecodedMessageKind decodedKind(ProtocolDirection direction) {
  switch (direction) {
  case ProtocolDirection::ClientRequest:
    return DecodedMessageKind::ClientRequest;
  case ProtocolDirection::ServerRequest:
    return DecodedMessageKind::ServerRequest;
  case ProtocolDirection::ServerNotification:
    return DecodedMessageKind::ServerNotification;
  case ProtocolDirection::ClientNotification:
    return DecodedMessageKind::ClientNotification;
  }
  return DecodedMessageKind::ServerNotification;
}

const Value *field(const std::shared_ptr<const NodeState> &state,
                   std::string_view key) {
  if (!state)
    return nullptr;
  const auto found = state->fields.find(key);
  return found == state->fields.end() ? nullptr : &found->second;
}

std::vector<std::string> canonicalIds(const std::vector<NodeRef> &nodes) {
  std::vector<std::string> result;
  result.reserve(nodes.size());
  for (const NodeRef &node : nodes)
    result.emplace_back(node->id().canonical);
  return result;
}

std::vector<std::string> protocolIds(const NodeGraph::ReadAccess &read,
                                     const std::vector<NodeRef> &nodes) {
  std::vector<std::string> result;
  result.reserve(nodes.size());
  for (const NodeRef &node : nodes)
    result.emplace_back(protocolCanonicalId(*read.state(node), node));
  return result;
}

NodeRef findTurn(const NodeGraph::ReadAccess &read, std::string_view threadId,
                 std::string_view turnId) {
  return read.find(scopedTurnNodeId(threadId, turnId));
}

NodeRef findItem(const NodeGraph::ReadAccess &read, std::string_view threadId,
                 std::string_view turnId, std::string_view itemId) {
  return read.find(
      scopedItemNodeId(scopedTurnNodeId(threadId, turnId), itemId));
}

NodeRef findProtocolNode(const NodeGraph::ReadAccess &read, NodeKind kind,
                         std::string_view protocolId,
                         std::uint64_t connectionGeneration) {
  for (const NodeRef &node : read.orderedNodes()) {
    if (node->id().kind != kind)
      continue;
    const auto state = read.state(node);
    const Value *generation = field(state, "connectionGeneration");
    if (protocolCanonicalId(*state, node) == protocolId && generation &&
        generation->asUInt64() &&
        *generation->asUInt64() == connectionGeneration)
      return node;
  }
  return {};
}

void catalogIsComplete() {
  const auto methods = protocolMethods();
  require(methods.size() == 252, "catalog has all 252 methods");
  require(protocolMethodCount(ProtocolDirection::ClientRequest) == 157,
          "catalog has 157 client requests");
  require(protocolMethodCount(ProtocolDirection::ServerRequest) == 11,
          "catalog has 11 server requests");
  require(protocolMethodCount(ProtocolDirection::ServerNotification) == 83,
          "catalog has 83 server notifications");
  require(protocolMethodCount(ProtocolDirection::ClientNotification) == 1,
          "catalog has one client notification");

  std::set<std::pair<int, std::string_view>> unique;
  std::size_t graphUpdates = 0;
  std::size_t operations = 0;
  std::size_t interactions = 0;
  std::size_t effects = 0;
  std::size_t neutral = 0;
  for (const MethodDescriptor &method : methods) {
    require(!method.method.empty(), "catalog method name is not empty");
    require(unique.emplace(static_cast<int>(method.direction), method.method)
                .second,
            "catalog direction/method key is unique");
    require(findProtocolMethod(method.direction, method.method).has_value(),
            "every catalog method is findable");
    switch (method.disposition) {
    case MessageDisposition::GraphUpdate:
      ++graphUpdates;
      break;
    case MessageDisposition::WorkerOperationResult:
      ++operations;
      break;
    case MessageDisposition::ReverseInteraction:
      ++interactions;
      break;
    case MessageDisposition::TypedUiEffect:
      ++effects;
      break;
    case MessageDisposition::IntentionallyStateNeutral:
      ++neutral;
      break;
    }
  }
  require(graphUpdates == 81, "81 server notifications update the graph");
  require(operations == 158,
          "157 requests plus initialized are worker operations");
  require(interactions == 11, "all server requests are interactions");
  require(effects == 0, "no protocol fact is reduced to only a UI effect");
  require(neutral == 2, "two internal raw-response events are neutral");
  require(!findProtocolMethod(ProtocolDirection::ServerNotification,
                              "unknown/method"),
          "unknown methods are not silently classified");
}

void everyKnownMethodDispatches() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  std::uint64_t ordinal = 0;
  for (const MethodDescriptor &descriptor : protocolMethods()) {
    ++ordinal;
    DecodedMessage message;
    message.kind = decodedKind(descriptor.direction);
    message.method = std::string(descriptor.method);
    message.payload.emplace("threadId",
                            Value("thread-" + std::to_string(ordinal)));
    message.payload.emplace("turnId", Value("turn-" + std::to_string(ordinal)));
    message.payload.emplace("itemId", Value("item-" + std::to_string(ordinal)));
    if (descriptor.direction == ProtocolDirection::ClientRequest ||
        descriptor.direction == ProtocolDirection::ServerRequest)
      message.requestId.emplace(std::int64_t(ordinal));
    const std::uint64_t before = graph.publishedRevision();
    const ApplyResult result = updater.apply(std::move(message));
    require(result.knownMethod, "known catalog method dispatches as known");
    require(result.disposition == descriptor.disposition,
            "dispatch returns the catalog disposition");
    if (descriptor.disposition ==
        MessageDisposition::IntentionallyStateNeutral) {
      require(result.change.empty(), "state-neutral method changes no nodes");
      require(graph.publishedRevision() == before,
              "state-neutral method does not publish a revision");
    }
  }
}

void nestedEntitiesAndStreamsStayCurrent() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  Value::Object item{{"id", Value("item-1")},
                     {"type", Value("agentMessage")},
                     {"status", Value("running")}};
  Value::Object turn{{"id", Value("turn-1")},
                     {"status", Value("inProgress")},
                     {"items", Value(Value::Array{Value(item)})}};
  Value::Object thread{{"id", Value("thread-1")},
                       {"name", Value("Thread one")},
                       {"turns", Value(Value::Array{Value(turn)})}};
  ApplyResult started =
      updater.apply({DecodedMessageKind::ServerNotification, "thread/started",
                     std::nullopt, Value::Object{{"thread", Value(thread)}}});
  require(started.change.revision == 1,
          "one nested notification publishes one graph revision");

  NodeRef threadRef;
  NodeRef turnRef;
  NodeRef itemRef;
  {
    auto read = graph.tryRead();
    require(read.has_value(), "nested graph is readable");
    threadRef = read->find({NodeKind::Thread, "thread-1"});
    turnRef = findTurn(*read, "thread-1", "turn-1");
    itemRef = findItem(*read, "thread-1", "turn-1", "item-1");
    require(threadRef && turnRef && itemRef,
            "nested thread, turn, and item are indexed");
    require(read->parent(turnRef) == threadRef, "turn is linked to thread");
    require(read->parent(itemRef) == turnRef, "item is linked to turn");
    require(read->state(turnRef)->status == NodeStatus::Running,
            "typed status is normalized on the node");
  }

  for (const std::string_view delta : {"hello ", "world"}) {
    static_cast<void>(
        updater.apply({DecodedMessageKind::ServerNotification,
                       "item/agentMessage/delta", std::nullopt,
                       Value::Object{{"threadId", Value("thread-1")},
                                     {"turnId", Value("turn-1")},
                                     {"itemId", Value("item-1")},
                                     {"delta", Value(delta)}}}));
  }
  {
    auto read = graph.tryRead();
    const auto state = read->state(itemRef);
    const Value *stream = field(state, "text");
    require(stream && stream->asString() &&
                *stream->asString() == "hello world",
            "stream fragments append in arrival order on the item");
    require(!field(state, "item/agentMessage/delta"),
            "stream state uses its semantic field instead of a method key");
  }
}

void scopedProviderIdentityCannotCrossParents() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  const auto startItem = [&](std::string threadId, std::string turnId,
                             std::string marker) {
    static_cast<void>(updater.apply(
        {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
         Value::Object{
             {"threadId", Value(threadId)},
             {"turnId", Value(turnId)},
             {"item", Value(Value::Object{{"id", Value("shared-item")},
                                          {"marker", Value(marker)},
                                          {"status", Value("running")}})}}}));
  };
  startItem("thread-a", "shared-turn", "from-a");
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("thread-b")},
           {"turn", Value(Value::Object{{"id", Value("shared-turn")},
                                        {"status", Value("running")}})}}}));
  startItem("thread-b", "shared-turn", "from-b");
  startItem("thread-a", "second-turn", "from-a-second");

  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerNotification,
                     "item/agentMessage/delta", std::nullopt,
                     Value::Object{{"threadId", Value("thread-b")},
                                   {"turnId", Value("shared-turn")},
                                   {"itemId", Value("shared-item")},
                                   {"delta", Value("only-b")}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/completed", std::nullopt,
       Value::Object{
           {"threadId", Value("thread-b")},
           {"turnId", Value("shared-turn")},
           {"item", Value(Value::Object{{"id", Value("shared-item")},
                                        {"marker", Value("final-b")}})}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/completed", std::nullopt,
       Value::Object{
           {"threadId", Value("thread-b")},
           {"turn", Value(Value::Object{{"id", Value("shared-turn")}})}}}));

  auto read = graph.tryRead();
  const NodeRef turnA = findTurn(*read, "thread-a", "shared-turn");
  const NodeRef turnB = findTurn(*read, "thread-b", "shared-turn");
  const NodeRef itemA =
      findItem(*read, "thread-a", "shared-turn", "shared-item");
  const NodeRef itemB =
      findItem(*read, "thread-b", "shared-turn", "shared-item");
  const NodeRef secondTurnItem =
      findItem(*read, "thread-a", "second-turn", "shared-item");
  require(turnA && turnB && turnA != turnB && itemA && itemB &&
              secondTurnItem && itemA != itemB && itemA != secondTurnItem,
          "raw turn and item IDs are scoped by every containing parent");
  require(read->parent(turnA)->id().canonical == "thread-a" &&
              read->parent(turnB)->id().canonical == "thread-b" &&
              read->parent(itemA) == turnA && read->parent(itemB) == turnB,
          "same raw IDs never reparent a node from another thread");
  require(protocolCanonicalId(*read->state(turnB), turnB) == "shared-turn" &&
              protocolCanonicalId(*read->state(itemB), itemB) ==
                  "shared-item" &&
              field(read->state(itemA), "text") == nullptr &&
              field(read->state(itemB), "text") &&
              *field(read->state(itemB), "text")->asString() == "only-b",
          "scoped nodes retain raw boundary IDs and isolate stream updates");
  require(read->state(itemA)->status == NodeStatus::Running &&
              read->state(itemB)->status == NodeStatus::Completed &&
              read->state(turnB)->status == NodeStatus::Completed,
          "completion notifications advance existing Running nodes");
  require(!read->find({NodeKind::Turn, "shared-turn"}) &&
              !read->find({NodeKind::Item, "shared-item"}),
          "provider-scoped entities never leak into raw global indexes");
  require(scopedTurnNodeId("a", "b:c") != scopedTurnNodeId("a:b", "c") &&
              scopedItemNodeId(scopedTurnNodeId("a", "b:c"), "d:e") !=
                  scopedItemNodeId(scopedTurnNodeId("a:b", "c"), "e"),
          "length-prefixed scope encoding is collision-safe");
}

void rootOrderAndThreadHierarchyAreExplicit() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  for (const std::string_view id : {"retained-a", "retained-b"}) {
    static_cast<void>(updater.apply(
        {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
         Value::Object{{"thread", Value(Value::Object{{"id", Value(id)}})}}}));
  }

  const ProtocolRequestId listId("ordered-list");
  Value::Array listed{
      Value(Value::Object{{"id", Value("provider-a")}}),
      Value(Value::Object{{"id", Value("structural-child")},
                          {"parentThreadId", Value("structural-parent")}}),
      Value(Value::Object{{"id", Value("structural-parent")},
                          {"parentThreadId", Value(nullptr)}}),
      Value(Value::Object{{"id", Value("provider-b")}}),
      Value(Value::Object{{"id", Value("provider-a")}})};
  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientResult, "thread/list", listId,
                     Value::Object{{"data", Value(std::move(listed))}}}));

  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef parent = read->find({NodeKind::Thread, "structural-parent"});
    const NodeRef child = read->find({NodeKind::Thread, "structural-child"});
    require(runtime && canonicalIds(
                           read->related(runtime, RelationKind::RootThread)) ==
                           std::vector<std::string>{
                               "provider-a", "structural-parent", "provider-b",
                               "retained-b", "retained-a"},
            "thread/list replaces the provider prefix and preserves one "
            "ordered retained tail");
    require(parent && child &&
                canonicalIds(read->related(
                    parent, RelationKind::StructuralChildThread)) ==
                    std::vector<std::string>{"structural-child"},
            "parentThreadId creates a direct structural thread relation");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/fork",
       ProtocolRequestId("fork-result"),
       Value::Object{
           {"thread", Value(Value::Object{
                          {"id", Value("fork-child")},
                          {"forkedFromId", Value("structural-parent")}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef parent = read->find({NodeKind::Thread, "structural-parent"});
    const NodeRef fork = read->find({NodeKind::Thread, "fork-child"});
    const auto roots = read->related(runtime, RelationKind::RootThread);
    require(!roots.empty() && roots.front() == fork &&
                canonicalIds(
                    read->related(parent, RelationKind::ForkChildThread)) ==
                    std::vector<std::string>{"fork-child"},
            "forkedFromId keeps a fork relation while the fork remains a "
            "visible root");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("agent-child")}})}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
       Value::Object{
           {"threadId", Value("structural-parent")},
           {"turnId", Value("agent-turn")},
           {"item",
            Value(Value::Object{{"id", Value("spawn-item")},
                                {"type", Value("subAgentActivity")},
                                {"agentThreadId", Value("agent-child")}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef parent = read->find({NodeKind::Thread, "structural-parent"});
    const NodeRef item =
        findItem(*read, "structural-parent", "agent-turn", "spawn-item");
    const NodeRef child = read->find({NodeKind::Thread, "agent-child"});
    const auto roots =
        canonicalIds(read->related(runtime, RelationKind::RootThread));
    require(std::find(roots.begin(), roots.end(), "agent-child") == roots.end(),
            "an agent-owned child is removed from canonical root order");
    require(parent && item && child &&
                read->related(parent, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{child} &&
                read->related(item, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{child},
            "agent activity relates both its owner thread and source item to "
            "the stable child thread");
  }
}

void semanticDeltasAndHydratedOrderStayCurrent() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  const auto delta = [&](std::string method, std::string itemId,
                         std::string text, std::string indexName = {},
                         std::uint64_t index = 0) {
    Value::Object payload{{"threadId", Value("semantic-thread")},
                          {"turnId", Value("semantic-turn")},
                          {"itemId", Value(std::move(itemId))},
                          {"delta", Value(std::move(text))}};
    if (!indexName.empty())
      payload.emplace(std::move(indexName), Value(index));
    static_cast<void>(
        updater.apply({DecodedMessageKind::ServerNotification,
                       std::move(method), std::nullopt, std::move(payload)}));
  };
  delta("item/plan/delta", "plan-item", "step one");
  delta("item/reasoning/summaryTextDelta", "reasoning-item", "second",
        "summaryIndex", 1);
  delta("item/reasoning/summaryTextDelta", "reasoning-item", " part",
        "summaryIndex", 1);
  delta("item/reasoning/textDelta", "reasoning-item", "details", "contentIndex",
        0);
  delta("item/commandExecution/outputDelta", "command-item", "line one\n");

  {
    auto read = graph.tryRead();
    const auto plan = read->state(
        findItem(*read, "semantic-thread", "semantic-turn", "plan-item"));
    const auto reasoning = read->state(
        findItem(*read, "semantic-thread", "semantic-turn", "reasoning-item"));
    const auto command = read->state(
        findItem(*read, "semantic-thread", "semantic-turn", "command-item"));
    const Value *text = field(plan, "text");
    const Value *summary = field(reasoning, "summary");
    const Value *content = field(reasoning, "content");
    const Value *output = field(command, "aggregatedOutput");
    require(
        text && text->asString() && *text->asString() == "step one" &&
            summary && summary->asArray() && summary->asArray()->size() == 2 &&
            summary->asArray()->at(1).asString() &&
            *summary->asArray()->at(1).asString() == "second part" && content &&
            content->asArray() && content->asArray()->front().asString() &&
            *content->asArray()->front().asString() == "details" && output &&
            output->asString() && *output->asString() == "line one\n",
        "all item deltas append to their concrete semantic fields");
  }

  Value::Object hydrated{
      {"id", Value("semantic-thread")},
      {"turns",
       Value(Value::Array{
           Value(Value::Object{
               {"id", Value("turn-two")},
               {"items",
                Value(Value::Array{
                    Value(Value::Object{{"id", Value("item-two")}}),
                    Value(Value::Object{{"id", Value("item-one")}})})}}),
           Value(Value::Object{
               {"id", Value("turn-one")},
               {"items", Value(Value::Array{Value(Value::Object{
                             {"id", Value("item-three")}})})}})})}};
  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientResult, "thread/read",
                     ProtocolRequestId("hydrate-order"),
                     Value::Object{{"thread", Value(std::move(hydrated))}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "semantic-thread"});
    const NodeRef turnTwo = findTurn(*read, "semantic-thread", "turn-two");
    require(protocolIds(*read, read->children(thread)) ==
                    std::vector<std::string>{"turn-two", "turn-one"} &&
                protocolIds(*read, read->children(turnTwo)) ==
                    std::vector<std::string>{"item-two", "item-one"},
            "thread/read publishes exact turn and item order in one revision");
  }
}

void realtimeNotificationsMaintainOneCurrentSession() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const auto notify = [&](std::string method, Value::Object payload) {
    return updater.apply({DecodedMessageKind::ServerNotification,
                          std::move(method), std::nullopt, std::move(payload)});
  };

  static_cast<void>(
      notify("thread/realtime/started",
             Value::Object{{"threadId", Value("realtime-thread")},
                           {"realtimeSessionId", Value("session-one")},
                           {"version", Value("v1")}}));
  static_cast<void>(notify(
      "thread/realtime/itemAdded",
      Value::Object{{"threadId", Value("realtime-thread")},
                    {"item", Value(Value::Object{
                                 {"id", Value("raw-item")},
                                 {"type", Value("transcriptSegment")},
                                 {"text", Value("already committed")}})}}));
  static_cast<void>(notify(
      "thread/realtime/item/started",
      Value::Object{{"threadId", Value("realtime-thread")},
                    {"item", Value(Value::Object{
                                 {"id", Value("stream-item")},
                                 {"realtimeSessionId", Value("session-one")},
                                 {"type", Value("transcriptSegment")}})}}));
  for (const std::string_view delta : {"hello ", "world"}) {
    static_cast<void>(
        notify("thread/realtime/item/transcript/delta",
               Value::Object{{"threadId", Value("realtime-thread")},
                             {"itemId", Value("stream-item")},
                             {"delta", Value(delta)}}));
  }
  static_cast<void>(notify("thread/realtime/transcript/delta",
                           Value::Object{{"threadId", Value("realtime-thread")},
                                         {"role", Value("assistant")},
                                         {"delta", Value("draft")}}));
  static_cast<void>(notify("thread/realtime/transcript/delta",
                           Value::Object{{"threadId", Value("realtime-thread")},
                                         {"role", Value("user")},
                                         {"delta", Value("question")}}));
  static_cast<void>(notify("thread/realtime/transcript/done",
                           Value::Object{{"threadId", Value("realtime-thread")},
                                         {"role", Value("assistant")},
                                         {"text", Value("final answer")}}));
  for (const std::string_view data : {"YXVkaW8x", "YXVkaW8y"}) {
    static_cast<void>(notify(
        "thread/realtime/outputAudio/delta",
        Value::Object{
            {"threadId", Value("realtime-thread")},
            {"audio", Value(Value::Object{{"itemId", Value("stream-item")},
                                          {"data", Value(data)},
                                          {"sampleRate", Value(24000)}})}}));
  }
  static_cast<void>(notify("thread/realtime/sdp",
                           Value::Object{{"threadId", Value("realtime-thread")},
                                         {"sdp", Value("current-sdp")}}));
  for (const std::string_view error : {"first error", "second error"}) {
    static_cast<void>(
        notify("thread/realtime/error",
               Value::Object{{"threadId", Value("realtime-thread")},
                             {"message", Value(error)}}));
  }
  static_cast<void>(
      notify("thread/realtime/item/completed",
             Value::Object{
                 {"threadId", Value("realtime-thread")},
                 {"item", Value(Value::Object{
                              {"id", Value("stream-item")},
                              {"realtimeSessionId", Value("session-one")},
                              {"text", Value("authoritative transcript")}})}}));
  static_cast<void>(notify("thread/realtime/closed",
                           Value::Object{{"threadId", Value("realtime-thread")},
                                         {"reason", Value("remote close")}}));

  NodeRef session;
  NodeRef streamItem;
  NodeRef rawItem;
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "realtime-thread"});
    for (const NodeRef &child : read->children(thread)) {
      if (child->id().kind == NodeKind::RealtimeSession)
        session = child;
    }
    require(session && read->parent(session) == thread,
            "realtime started creates one session contained by its thread");
    for (const NodeRef &child : read->children(session)) {
      const std::string id = protocolCanonicalId(*read->state(child), child);
      if (id == "stream-item")
        streamItem = child;
      else if (id == "raw-item")
        rawItem = child;
    }
    const auto sessionState = read->state(session);
    const auto itemState = read->state(streamItem);
    const Value *transcripts = field(sessionState, "transcripts");
    const Value *completion = field(sessionState, "transcriptCompleted");
    const Value *errors = field(sessionState, "errors");
    const Value *audio = field(itemState, "outputAudioChunks");
    require(
        rawItem && streamItem && itemState->status == NodeStatus::Completed &&
            field(itemState, "transcript") &&
            *field(itemState, "transcript")->asString() == "hello world" &&
            audio && audio->asArray() && audio->asArray()->size() == 2,
        "realtime items retain identity, transcript, audio, and completion");
    require(
        transcripts && transcripts->asObject() &&
            transcripts->find("assistant") &&
            *transcripts->find("assistant")->asString() == "final answer" &&
            transcripts->find("user") &&
            *transcripts->find("user")->asString() == "question" &&
            completion && completion->find("assistant") &&
            *completion->find("assistant")->asBool(),
        "role transcripts finalize independently without overwriting peers");
    require(errors && errors->asArray() && errors->asArray()->size() == 2 &&
                field(sessionState, "sdp") &&
                *field(sessionState, "sdp")->asString() == "current-sdp" &&
                sessionState->status == NodeStatus::Failed &&
                field(sessionState, "active") &&
                !*field(sessionState, "active")->asBool(),
            "errors append and close preserves the session's failed status");
  }

  const ApplyResult restarted =
      notify("thread/realtime/started",
             Value::Object{{"threadId", Value("realtime-thread")},
                           {"realtimeSessionId", Value("session-two")},
                           {"version", Value("v2")}});
  {
    auto read = graph.tryRead();
    const auto state = read->state(session);
    require(read->children(session).empty() &&
                std::ranges::find(restarted.change.removed, streamItem) !=
                    restarted.change.removed.end() &&
                std::ranges::find(restarted.change.removed, rawItem) !=
                    restarted.change.removed.end(),
            "a new realtime incarnation removes old current-session items");
    require(state->status == NodeStatus::Running &&
                field(state, "realtimeSessionId") &&
                *field(state, "realtimeSessionId")->asString() ==
                    "session-two" &&
                !field(state, "errors") && !field(state, "transcripts"),
            "new realtime start atomically resets only the current session");
  }
}

void graphRelationsInvalidationAndIncarnationsAreExplicit() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  {
    auto write = graph.write();
    NodeRef connection = write.upsert({NodeKind::Connection, "connection"});
    write.setField(connection, "connectionGeneration", Value(1));
    write.setField(connection, "providerGeneration", Value(7));
    static_cast<void>(write.finish());
  }

  const auto notify = [&](std::string method, Value::Object payload) {
    return updater.apply({DecodedMessageKind::ServerNotification,
                          std::move(method), std::nullopt, std::move(payload)});
  };
  static_cast<void>(notify("command/exec/outputDelta",
                           Value::Object{{"processId", Value("reused-process")},
                                         {"stream", Value("stdout")},
                                         {"deltaBase64", Value("Zmlyc3Q=")},
                                         {"capReached", Value(false)}}));
  static_cast<void>(notify(
      "fs/changed",
      Value::Object{{"watchId", Value("reused-watch")},
                    {"changedPaths", Value(Value::Array{Value("/first")})}}));
  {
    auto write = graph.write();
    NodeRef connection = write.find({NodeKind::Connection, "connection"});
    write.setField(connection, "connectionGeneration", Value(2));
    write.setField(connection, "providerGeneration", Value(1));
    static_cast<void>(write.finish());
  }
  static_cast<void>(notify("command/exec/outputDelta",
                           Value::Object{{"processId", Value("reused-process")},
                                         {"stream", Value("stdout")},
                                         {"deltaBase64", Value("c2Vjb25k")},
                                         {"capReached", Value(true)}}));
  static_cast<void>(notify(
      "fs/changed",
      Value::Object{{"watchId", Value("reused-watch")},
                    {"changedPaths", Value(Value::Array{Value("/second")})}}));
  {
    auto read = graph.tryRead();
    const NodeRef firstProcess =
        findProtocolNode(*read, NodeKind::Process, "reused-process", 1);
    const NodeRef secondProcess =
        findProtocolNode(*read, NodeKind::Process, "reused-process", 2);
    const NodeRef firstWatch =
        findProtocolNode(*read, NodeKind::FilesystemWatch, "reused-watch", 1);
    const NodeRef secondWatch =
        findProtocolNode(*read, NodeKind::FilesystemWatch, "reused-watch", 2);
    const NodeRef connection = read->find({NodeKind::Connection, "connection"});
    require(
        firstProcess && secondProcess && firstProcess != secondProcess &&
            firstWatch && secondWatch && firstWatch != secondWatch,
        "connection-scoped process and watch IDs cannot cross incarnations");
    require(
        field(read->state(firstProcess), "stdoutBase64") &&
            *field(read->state(firstProcess), "stdoutBase64")->asString() ==
                "Zmlyc3Q=" &&
            field(read->state(secondProcess), "stdoutBase64") &&
            *field(read->state(secondProcess), "stdoutBase64")->asString() ==
                "c2Vjb25k",
        "same raw process ID retains isolated output per incarnation");
    const auto owned = read->related(connection, RelationKind::ProcessOwner);
    require(std::ranges::find(owned, firstProcess) != owned.end() &&
                std::ranges::find(owned, secondProcess) != owned.end(),
            "the connection directly owns its scoped process nodes");
  }

  Value::Object collabItem{
      {"id", Value("collab-item")},
      {"type", Value("collabAgentToolCall")},
      {"receiverThreadIds",
       Value(Value::Array{Value("receiver-a"), Value("receiver-b")})}};
  Value::Object thread{
      {"id", Value("related-thread")},
      {"projectId", Value("project-a")},
      {"section", Value(Value::Object{{"id", Value("section-a")},
                                      {"name", Value("Section A")}})},
      {"turns", Value(Value::Array{Value(Value::Object{
                    {"id", Value("related-turn")},
                    {"items", Value(Value::Array{Value(collabItem)})}})})}};
  static_cast<void>(notify(
      "thread/started", Value::Object{{"thread", Value(std::move(thread))}}));
  {
    auto read = graph.tryRead();
    const NodeRef owner = read->find({NodeKind::Thread, "related-thread"});
    const NodeRef item =
        findItem(*read, "related-thread", "related-turn", "collab-item");
    const NodeRef project = read->find({NodeKind::Project, "project-a"});
    const NodeRef section = read->find({NodeKind::ThreadSection, "section-a"});
    const NodeRef receiverA = read->find({NodeKind::Thread, "receiver-a"});
    const NodeRef receiverB = read->find({NodeKind::Thread, "receiver-b"});
    require(read->related(owner, RelationKind::ProjectMembership) ==
                    std::vector<NodeRef>{project} &&
                read->related(owner, RelationKind::SectionMembership) ==
                    std::vector<NodeRef>{section},
            "thread descriptors populate direct project and section relations");
    require(read->related(item, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{receiverA, receiverB} &&
                read->related(owner, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{receiverA, receiverB},
            "all receiverThreadIds populate stable child-thread relations");
  }

  static_cast<void>(notify(
      "item/completed",
      Value::Object{
          {"threadId", Value("related-thread")},
          {"turnId", Value("related-turn")},
          {"item",
           Value(Value::Object{{"id", Value("collab-item")},
                               {"type", Value("collabAgentToolCall")},
                               {"receiverThreadIds",
                                Value(Value::Array{Value("receiver-b")})}})}}));
  static_cast<void>(notify("thread/project/updated",
                           Value::Object{{"threadId", Value("related-thread")},
                                         {"projectId", Value("project-b")}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/section/move", std::nullopt,
       Value::Object{{"threadId", Value("related-thread")},
                     {"sectionId", Value("section-b")}}}));
  static_cast<void>(notify(
      "thread/goal/updated",
      Value::Object{{"threadId", Value("related-thread")},
                    {"turnId", Value("related-turn")},
                    {"goal", Value(Value::Object{{"text", Value("ship")}})}}));
  static_cast<void>(
      notify("thread/goal/cleared",
             Value::Object{{"threadId", Value("related-thread")}}));
  static_cast<void>(
      notify("thread/queue/changed",
             Value::Object{{"threadId", Value("related-thread")}}));
  static_cast<void>(notify("skills/changed", Value::Object{}));
  static_cast<void>(notify("project/changed",
                           Value::Object{{"projectId", Value("project-b")},
                                         {"changeType", Value("updated")}}));
  {
    auto read = graph.tryRead();
    const NodeRef owner = read->find({NodeKind::Thread, "related-thread"});
    const NodeRef item =
        findItem(*read, "related-thread", "related-turn", "collab-item");
    const NodeRef receiverB = read->find({NodeKind::Thread, "receiver-b"});
    const NodeRef projectB = read->find({NodeKind::Project, "project-b"});
    const NodeRef sectionB = read->find({NodeKind::ThreadSection, "section-b"});
    const auto ownerState = read->state(owner);
    require(read->related(item, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{receiverB} &&
                read->related(owner, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{receiverB},
            "authoritative receiver replacement removes stale child relations");
    require(read->related(owner, RelationKind::ProjectMembership) ==
                    std::vector<NodeRef>{projectB} &&
                read->related(owner, RelationKind::SectionMembership) ==
                    std::vector<NodeRef>{sectionB},
            "exact project and section assignments replace prior relations");
    require(field(ownerState, "goal") && field(ownerState, "goal")->isNull() &&
                field(ownerState, "goalTurnId") &&
                field(ownerState, "goalTurnId")->isNull() &&
                field(ownerState, "queueStale") &&
                *field(ownerState, "queueStale")->asBool(),
            "goal clearing is known-null and queue changes invalidate state");
    const auto skills = read->state(read->find({NodeKind::Catalog, "skills"}));
    require(field(skills, "stale") && *field(skills, "stale")->asBool() &&
                field(read->state(projectB), "stale") &&
                *field(read->state(projectB), "stale")->asBool(),
            "catalog and project invalidations are explicit current facts");
  }

  static_cast<void>(notify("thread/project/updated",
                           Value::Object{{"threadId", Value("related-thread")},
                                         {"projectId", Value(nullptr)}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/section/move", std::nullopt,
       Value::Object{{"threadId", Value("related-thread")},
                     {"sectionId", Value(nullptr)}}}));
  {
    auto read = graph.tryRead();
    const NodeRef owner = read->find({NodeKind::Thread, "related-thread"});
    require(read->related(owner, RelationKind::ProjectMembership).empty() &&
                read->related(owner, RelationKind::SectionMembership).empty(),
            "nullable project and section updates remove stale membership");
  }
}

void promptMaterializationDoesNotAcknowledgeDelivery() {
  NodeGraph graph;
  NodeRef local;
  {
    auto write = graph.write();
    NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
    NodeRef thread = write.upsert({NodeKind::Thread, "prompt-thread"});
    NodeRef turn = write.upsert({NodeKind::Turn, "local-turn"});
    local = write.upsert({NodeKind::Item, "local-prompt"});
    write.setParent(thread, turn);
    write.setParent(turn, local);
    write.setField(local, "local", Value(true));
    write.setField(local, "clientUserMessageId", Value("client-prompt"));
    write.setField(local, "dispatchState", Value("awaitingResult"));
    write.setStatus(local, NodeStatus::Running);
    write.relate(runtime, RelationKind::PendingPrompt, local);
    static_cast<void>(write.finish());
  }

  ProtocolUpdater updater(graph);
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
       Value::Object{{"threadId", Value("prompt-thread")},
                     {"turnId", Value("provider-turn")},
                     {"item", Value(Value::Object{
                                  {"id", Value("provider-item")},
                                  {"type", Value("userMessage")},
                                  {"clientId", Value("client-prompt")}})}}}));
  auto read = graph.tryRead();
  const NodeRef authoritative =
      findItem(*read, "prompt-thread", "provider-turn", "provider-item");
  const auto localState = read->state(local);
  require(read->related(authoritative, RelationKind::PromptMaterialization) ==
              std::vector<NodeRef>{local},
          "authoritative prompt identity is related to its local node");
  require(localState->status == NodeStatus::Running &&
              field(localState, "dispatchState") &&
              *field(localState, "dispatchState")->asString() ==
                  "awaitingResult",
          "inbound materialization alone never acknowledges outbound delivery");
}

void turnRootsAndPagedHistoryStayExplicit() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  Value::Array completeItems;
  completeItems.emplace_back(Value::Object{{"id", Value("opening-prompt")},
                                           {"type", Value("userMessage")}});
  for (std::size_t index = 0; index < 82; ++index) {
    completeItems.emplace_back(
        Value::Object{{"id", Value("activity-" + std::to_string(index))},
                      {"type", Value("agentMessage")}});
  }
  completeItems.emplace_back(Value::Object{{"id", Value("later-steering")},
                                           {"type", Value("userMessage")}});
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read",
       ProtocolRequestId("complete-history"),
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("history-thread")},
                {"turns",
                 Value(Value::Array{Value(Value::Object{
                     {"id", Value("long-turn")},
                     {"items", Value(std::move(completeItems))}})})}})}}}));

  NodeRef openingPrompt;
  std::uint64_t threadRevision = 0;
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "history-thread"});
    const NodeRef turn = findTurn(*read, "history-thread", "long-turn");
    openingPrompt =
        findItem(*read, "history-thread", "long-turn", "opening-prompt");
    const Value *loaded = field(read->state(thread), "historyLoadedItemCount");
    require(turn && openingPrompt &&
                read->related(turn, RelationKind::TurnRootItem) ==
                    std::vector<NodeRef>{openingPrompt},
            "a turn directly relates to its authoritative opening prompt");
    require(loaded && loaded->asUInt64() && *loaded->asUInt64() == 84,
            "thread history records the complete loaded item count for local "
            "windowing");
    threadRevision = read->changedRevision(thread);
  }
  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerNotification,
                     "item/agentMessage/delta", std::nullopt,
                     Value::Object{{"threadId", Value("history-thread")},
                                   {"turnId", Value("long-turn")},
                                   {"itemId", Value("activity-81")},
                                   {"delta", Value("streamed")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "history-thread"});
    require(read->changedRevision(thread) == threadRevision,
            "an existing-item stream delta does not touch thread history "
            "metadata");
  }

  Value::Array retainedSuffix{
      Value(Value::Object{{"id", Value("later-steering")},
                          {"type", Value("userMessage")}}),
      Value(Value::Object{{"id", Value("latest-activity")},
                          {"type", Value("agentMessage")}})};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read",
       ProtocolRequestId("suffix-history"),
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("history-thread")},
                {"turns",
                 Value(Value::Array{Value(Value::Object{
                     {"id", Value("long-turn")},
                     {"items", Value(std::move(retainedSuffix))}})})}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "history-thread"});
    const NodeRef turn = findTurn(*read, "history-thread", "long-turn");
    const NodeRef steering =
        findItem(*read, "history-thread", "long-turn", "later-steering");
    const Value *loaded = field(read->state(thread), "historyLoadedItemCount");
    require(read->children(turn) ==
                    std::vector<NodeRef>{
                        steering, findItem(*read, "history-thread", "long-turn",
                                           "latest-activity")} &&
                read->related(turn, RelationKind::TurnRootItem) ==
                    std::vector<NodeRef>{openingPrompt},
            "a retained suffix cannot promote a later steering message over "
            "the known opening prompt");
    require(loaded && loaded->asUInt64() && *loaded->asUInt64() == 3,
            "loaded history count includes a pinned opening prompt outside "
            "the current item suffix");
  }

  const ApplyResult removedHistory = updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("history-thread")}}});
  require(std::find(removedHistory.change.removed.begin(),
                    removedHistory.change.removed.end(),
                    openingPrompt) != removedHistory.change.removed.end(),
          "removing a hydrated thread also removes its pinned opening prompt");

  const ProtocolRequestId firstPage("turn-page-1");
  const ApplyResult firstPageRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/turns/list", firstPage,
       Value::Object{{"threadId", Value("paged-thread")}}});
  Value::Array firstTurns{Value(Value::Object{
      {"id", Value("newer-turn")},
      {"items", Value(Value::Array{
                    Value(Value::Object{{"id", Value("newer-root")},
                                        {"type", Value("userMessage")}}),
                    Value(Value::Object{{"id", Value("newer-steering")},
                                        {"type", Value("userMessage")}})})}})};
  const ApplyResult firstPageResult = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/turns/list", firstPage,
       Value::Object{{"data", Value(std::move(firstTurns))},
                     {"nextCursor", Value("older-page")}},
       firstPageRequest.primary});
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "paged-thread"});
    const NodeRef turn = findTurn(*read, "paged-thread", "newer-turn");
    const NodeRef root =
        findItem(*read, "paged-thread", "newer-turn", "newer-root");
    const NodeRef operation =
        read->find({NodeKind::Operation, firstPage.canonical()});
    const auto state = read->state(thread);
    const Value *hasMore = field(state, "historyHasMore");
    const Value *cursor = field(state, "historyNextCursor");
    const Value *loaded = field(state, "historyLoadedItemCount");
    require(thread && turn && root &&
                read->related(turn, RelationKind::TurnRootItem) ==
                    std::vector<NodeRef>{root},
            "a turns page retains the opening item relation for each turn");
    require(!operation && firstPageRequest.primary &&
                std::ranges::find(firstPageResult.change.removed,
                                  firstPageRequest.primary) !=
                    firstPageResult.change.removed.end(),
            "a turns/list result uses its correlated request scope then "
            "retires the completed operation");
    require(hasMore && hasMore->asBool() && *hasMore->asBool() && cursor &&
                cursor->asString() && *cursor->asString() == "older-page" &&
                loaded && loaded->asUInt64() && *loaded->asUInt64() == 2,
            "a turns page exposes provider continuation and loaded history "
            "size on its thread");
  }

  const ProtocolRequestId lastPage("turn-page-2");
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/turns/list", lastPage,
       Value::Object{{"threadId", Value("paged-thread")},
                     {"cursor", Value("older-page")}}}));
  Value::Array olderTurns{Value(Value::Object{
      {"id", Value("older-turn")},
      {"items",
       Value(Value::Array{Value(Value::Object{
           {"id", Value("older-root")}, {"type", Value("userMessage")}})})}})};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/turns/list", lastPage,
       Value::Object{{"data", Value(std::move(olderTurns))},
                     {"nextCursor", Value(nullptr)}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "paged-thread"});
    const auto state = read->state(thread);
    const Value *hasMore = field(state, "historyHasMore");
    const Value *loaded = field(state, "historyLoadedItemCount");
    require(protocolIds(*read, read->children(thread)) ==
                    std::vector<std::string>{"older-turn", "newer-turn"} &&
                hasMore && hasMore->asBool() && !*hasMore->asBool() &&
                !field(state, "historyNextCursor") && loaded &&
                loaded->asUInt64() && *loaded->asUInt64() == 3,
            "the final turns page clears provider continuation while "
            "retaining accumulated chronological history");
  }
}

void resultsAndListsCorrelate() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const ProtocolRequestId requestId("list-1");

  ApplyResult request =
      updater.apply({DecodedMessageKind::ClientRequest, "thread/list",
                     requestId, Value::Object{{"limit", Value(20)}}});
  NodeRef operation;
  {
    auto read = graph.tryRead();
    operation = read->find({NodeKind::Operation, requestId.canonical()});
    require(operation && read->state(operation)->status == NodeStatus::Pending,
            "client request creates a pending correlated operation node");
  }

  Value::Array threads{
      Value(Value::Object{{"id", Value("listed-1")}, {"name", Value("First")}}),
      Value(
          Value::Object{{"id", Value("listed-2")}, {"name", Value("Second")}})};
  ApplyResult result =
      updater.apply({DecodedMessageKind::ClientResult, "thread/list", requestId,
                     Value::Object{{"data", Value(std::move(threads))},
                                   {"nextCursor", Value("next")}},
                     operation});
  require(result.change.revision == request.change.revision + 1,
          "correlated result is one later atomic revision");
  {
    auto read = graph.tryRead();
    require(!read->find({NodeKind::Operation, requestId.canonical()}) &&
                std::ranges::find(result.change.removed, operation) !=
                    result.change.removed.end(),
            "successful result applies current state then retires its pending "
            "operation");
    require(read->find({NodeKind::Thread, "listed-1"}) &&
                read->find({NodeKind::Thread, "listed-2"}),
            "thread list result materializes its current entities");
  }

  const ProtocolRequestId failedId(22);
  const ApplyResult failedRequest =
      updater.apply({DecodedMessageKind::ClientRequest, "thread/read", failedId,
                     Value::Object{{"threadId", Value("missing")}}});
  const ApplyResult failedResult = updater.apply(
      {DecodedMessageKind::ClientError, "thread/read", failedId,
       Value::Object{{"code", Value(-32001)}, {"message", Value("overloaded")}},
       failedRequest.primary});
  {
    auto read = graph.tryRead();
    NodeRef failed = read->find({NodeKind::Operation, failedId.canonical()});
    require(!failed && failedRequest.primary &&
                std::ranges::find(failedResult.change.removed,
                                  failedRequest.primary) !=
                    failedResult.change.removed.end(),
            "failed result retires its pending operation instead of keeping "
            "terminal history");
  }
}

void reusedWireIdsRequireExactCurrentNodes() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const ProtocolRequestId reused("reused-request");

  const ApplyResult first =
      updater.apply({DecodedMessageKind::ClientRequest, "thread/read", reused,
                     Value::Object{{"threadId", Value("old-thread")}}});
  const ApplyResult second =
      updater.apply({DecodedMessageKind::ClientRequest, "thread/list", reused,
                     Value::Object{{"limit", Value(10)}}});
  require(first.primary && second.primary && first.primary != second.primary &&
              std::ranges::find(second.change.removed, first.primary) !=
                  second.change.removed.end(),
          "reusing a request id replaces rather than mutates its old "
          "Operation NodeRef");

  const std::uint64_t beforeLate = graph.publishedRevision();
  const ApplyResult late = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read", reused,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("late-thread")}})}},
       first.primary});
  {
    auto read = graph.tryRead();
    require(late.change.empty() && late.change.revision == beforeLate &&
                read->find({NodeKind::Operation, reused.canonical()}) ==
                    second.primary &&
                !read->find({NodeKind::Thread, "late-thread"}),
            "a late result retaining the replaced NodeRef cannot mutate the "
            "new operation or graph");
  }

  Value::Array currentThreads{
      Value(Value::Object{{"id", Value("current-thread")}})};
  const ApplyResult current =
      updater.apply({DecodedMessageKind::ClientResult, "thread/list", reused,
                     Value::Object{{"data", Value(std::move(currentThreads))}},
                     second.primary});
  {
    auto read = graph.tryRead();
    require(read->find({NodeKind::Thread, "current-thread"}) &&
                !read->find({NodeKind::Operation, reused.canonical()}) &&
                std::ranges::find(current.change.removed, second.primary) !=
                    current.change.removed.end(),
            "the exact current operation accepts its result and is pruned");
  }

  const ProtocolRequestId interactionId("reused-interaction");
  const ApplyResult oldInteraction = updater.apply(
      {DecodedMessageKind::ServerRequest,
       "item/commandExecution/requestApproval", interactionId,
       Value::Object{{"threadId", Value("old-interaction-thread")}}});
  const ApplyResult newInteraction = updater.apply(
      {DecodedMessageKind::ServerRequest, "item/fileChange/requestApproval",
       interactionId,
       Value::Object{{"threadId", Value("new-interaction-thread")}}});
  require(oldInteraction.primary && newInteraction.primary &&
              oldInteraction.primary != newInteraction.primary &&
              std::ranges::find(newInteraction.change.removed,
                                oldInteraction.primary) !=
                  newInteraction.change.removed.end(),
          "reusing a server-request id creates a distinct Interaction "
          "NodeRef");
  const GraphChange staleResolution =
      updater.resolveInteraction(oldInteraction.primary, true);
  {
    auto read = graph.tryRead();
    require(
        staleResolution.empty() &&
            read->find({NodeKind::Interaction, interactionId.canonical()}) ==
                newInteraction.primary,
        "an exact response for the retired interaction cannot resolve its "
        "replacement");
  }
  const GraphChange exactResolution =
      updater.resolveInteraction(newInteraction.primary, true);
  require(std::ranges::find(exactResolution.removed, newInteraction.primary) !=
              exactResolution.removed.end(),
          "the exact current interaction resolves normally");
}

void interactionsAndRemovalKeepLifetime() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const ProtocolRequestId interactionId("approval-1");
  ApplyResult request =
      updater.apply({DecodedMessageKind::ServerRequest,
                     "item/commandExecution/requestApproval", interactionId,
                     Value::Object{{"threadId", Value("thread-approval")},
                                   {"turnId", Value("turn-approval")},
                                   {"itemId", Value("item-approval")}}});
  require(request.disposition == MessageDisposition::ReverseInteraction,
          "approval is a reverse interaction");

  NodeRef interaction;
  {
    auto read = graph.tryRead();
    interaction =
        read->find({NodeKind::Interaction, interactionId.canonical()});
    require(interaction &&
                read->state(interaction)->status == NodeStatus::Pending,
            "reverse request creates a pending interaction node");
    const auto targets =
        read->related(interaction, RelationKind::InteractionTarget);
    require(targets.size() == 1 &&
                protocolCanonicalId(*read->state(targets.front()),
                                    targets.front()) == "item-approval",
            "interaction directly relates to its addressed item");
    const NodeRef turn = findTurn(*read, "thread-approval", "turn-approval");
    const NodeRef thread = read->find({NodeKind::Thread, "thread-approval"});
    require(turn && thread && read->parent(targets.front()) == turn &&
                read->parent(turn) == thread,
            "interaction targets retain their addressed containment chain");
    require(read->related(thread, RelationKind::PendingInteraction) ==
                std::vector<NodeRef>{interaction},
            "the addressed thread directly indexes its pending interaction");
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    require(runtime &&
                read->related(runtime, RelationKind::PendingInteraction) ==
                    std::vector<NodeRef>{interaction},
            "runtime directly indexes the complete pending interaction set");
  }

  GraphChange rejected =
      updater.resolveInteraction(interactionId, false, "bridge rejected");
  require(!rejected.empty(), "rejected response updates interaction state");
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "thread-approval"});
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    require(read->state(interaction)->status == NodeStatus::Failed,
            "rejected response remains visibly failed");
    require(thread && read->related(thread, RelationKind::PendingInteraction) ==
                          std::vector<NodeRef>{interaction},
            "a rejected response remains discoverable for thread recovery");
    require(runtime &&
                read->related(runtime, RelationKind::PendingInteraction) ==
                    std::vector<NodeRef>{interaction},
            "a rejected response remains in the runtime interaction set");
  }

  GraphChange accepted = updater.resolveInteraction(interactionId, true);
  require(accepted.removed.size() == 1 &&
              accepted.removed.front() == interaction,
          "accepted response removes and notifies with the same NodeRef");
  {
    auto read = graph.tryRead();
    require(!read->find({NodeKind::Interaction, interactionId.canonical()}),
            "resolved interaction leaves canonical indexes");
    require(read->retiredNodes().size() == 1,
            "removed interaction stays reachable for Qt detachment");
    const NodeRef thread = read->find({NodeKind::Thread, "thread-approval"});
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    require(
        thread && runtime &&
            read->related(thread, RelationKind::PendingInteraction).empty() &&
            read->related(runtime, RelationKind::PendingInteraction).empty(),
        "interaction removal unlinks both direct pending indexes");
  }
}

void unknownAndNeutralAreIsolated() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("known-thread")},
                                          {"name", Value("Known")}})}}}));
  NodeRef known;
  std::shared_ptr<const NodeState> before;
  {
    auto read = graph.tryRead();
    known = read->find({NodeKind::Thread, "known-thread"});
    before = read->state(known);
  }

  const std::uint64_t beforeNeutral = graph.publishedRevision();
  ApplyResult neutral = updater.apply(
      {DecodedMessageKind::ServerNotification, "rawResponse/completed",
       std::nullopt, Value::Object{{"secret", Value("not retained")}}});
  require(neutral.knownMethod && neutral.change.empty() &&
              graph.publishedRevision() == beforeNeutral,
          "known raw-response event is explicitly state-neutral");

  ApplyResult unknown =
      updater.apply({DecodedMessageKind::ServerNotification,
                     "future/newAlternative", std::nullopt,
                     Value::Object{{"threadId", Value("known-thread")},
                                   {"newData", Value(7)}}});
  require(!unknown.knownMethod && !unknown.change.empty(),
          "unknown alternative is retained in its own changed node");
  {
    auto read = graph.tryRead();
    require(read->state(known) == before,
            "unknown alternative cannot corrupt addressed known state");
    bool foundUnknown = false;
    for (const NodeRef &node : read->orderedNodes()) {
      if (node->id().kind == NodeKind::UnknownProtocol)
        foundUnknown = true;
    }
    require(foundUnknown, "unknown alternative has a discoverable node");
  }

  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerNotification,
                     "future/newAlternative", std::nullopt,
                     Value::Object{{"threadId", Value("known-thread")},
                                   {"future", Value("latest")}}}));
  {
    auto read = graph.tryRead();
    std::size_t unknownCount = 0;
    for (const NodeRef &node : read->orderedNodes())
      unknownCount += node->id().kind == NodeKind::UnknownProtocol ? 1U : 0U;
    require(unknownCount == 1 && read->state(known) == before,
            "repeated unknown alternatives replace current fallback state "
            "without creating a journal or mutating known state");
  }
}

void lifecycleFactsAndRemovalPreserveThreadHierarchy() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  Value::Array threads{
      Value(Value::Object{{"id", Value("lifecycle-parent")}}),
      Value(Value::Object{{"id", Value("lifecycle-child")},
                          {"parentThreadId", Value("lifecycle-parent")}}),
      Value(Value::Object{{"id", Value("lifecycle-grandchild")},
                          {"parentThreadId", Value("lifecycle-child")}})};
  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientResult, "thread/list",
                     ProtocolRequestId("lifecycle-list"),
                     Value::Object{{"data", Value(std::move(threads))}}}));

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/archived", std::nullopt,
       Value::Object{{"threadId", Value("lifecycle-parent")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef parent = read->find({NodeKind::Thread, "lifecycle-parent"});
    const auto state = read->state(parent);
    const Value *archived = field(state, "archived");
    require(
        archived && archived->asBool() && *archived->asBool() &&
            canonicalIds(read->related(runtime, RelationKind::RootThread)) ==
                std::vector<std::string>{"lifecycle-parent"},
        "archive is retained as a lifecycle fact without deleting the "
        "visible thread");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/closed", std::nullopt,
       Value::Object{{"threadId", Value("lifecycle-parent")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef parent = read->find({NodeKind::Thread, "lifecycle-parent"});
    require(parent && read->state(parent)->status == NodeStatus::NotLoaded,
            "thread/closed marks provider loading state without removal");
  }

  const ApplyResult deleted = updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("lifecycle-parent")}}});
  require(deleted.change.removed.size() == 1,
          "deleting a thread does not delete independently retained child "
          "threads");
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef child = read->find({NodeKind::Thread, "lifecycle-child"});
    const NodeRef grandchild =
        read->find({NodeKind::Thread, "lifecycle-grandchild"});
    require(canonicalIds(read->related(runtime, RelationKind::RootThread)) ==
                    std::vector<std::string>{"lifecycle-child"} &&
                child && grandchild &&
                read->related(child, RelationKind::StructuralChildThread) ==
                    std::vector<NodeRef>{grandchild},
            "parent removal promotes direct children and preserves their "
            "descendant hierarchy");
  }
}

void deletionUnlinksWholeGraph() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("delete-thread")},
           {"turn", Value(Value::Object{{"id", Value("delete-turn")}})}}}));
  NodeRef removed;
  NodeRef removedTurn;
  {
    auto read = graph.tryRead();
    removed = read->find({NodeKind::Thread, "delete-thread"});
    removedTurn = findTurn(*read, "delete-thread", "delete-turn");
  }
  ApplyResult result = updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("delete-thread")}}});
  require(
      result.change.removed.size() == 2 &&
          std::find(result.change.removed.begin(), result.change.removed.end(),
                    removed) != result.change.removed.end() &&
          std::find(result.change.removed.begin(), result.change.removed.end(),
                    removedTurn) != result.change.removed.end(),
      "thread deletion queues stable references for its whole contained "
      "lifecycle");
  {
    auto read = graph.tryRead();
    require(read->removed(removed), "removed node is marked removed");
    require(read->children(removed).empty(),
            "removed thread is unlinked from child turns");
    NodeRef turn = findTurn(*read, "delete-thread", "delete-turn");
    require(!turn && read->removed(removedTurn),
            "contained turns are removed rather than retained as orphans");
  }
}

} // namespace

int main() {
  catalogIsComplete();
  everyKnownMethodDispatches();
  nestedEntitiesAndStreamsStayCurrent();
  scopedProviderIdentityCannotCrossParents();
  rootOrderAndThreadHierarchyAreExplicit();
  semanticDeltasAndHydratedOrderStayCurrent();
  realtimeNotificationsMaintainOneCurrentSession();
  graphRelationsInvalidationAndIncarnationsAreExplicit();
  promptMaterializationDoesNotAcknowledgeDelivery();
  turnRootsAndPagedHistoryStayExplicit();
  resultsAndListsCorrelate();
  reusedWireIdsRequireExactCurrentNodes();
  interactionsAndRemovalKeepLifetime();
  unknownAndNeutralAreIsolated();
  lifecycleFactsAndRemovalPreserveThreadHierarchy();
  deletionUnlinksWholeGraph();

  if (failures != 0) {
    std::cerr << failures << " nodegraph protocol assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "codexui protocol updater tests passed\n";
  return EXIT_SUCCESS;
}
