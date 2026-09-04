// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/ProtocolCatalog.h"

#include <algorithm>
#include <chrono>
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

const Value *objectField(const Value::Object *object, std::string_view key) {
  if (!object)
    return nullptr;
  const auto found = object->find(key);
  return found == object->end() ? nullptr : &found->second;
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
  require(graphUpdates == 75, "75 server notifications update graph state");
  require(operations == 158,
          "157 requests plus initialized are worker operations");
  require(interactions == 11, "all server requests are interactions");
  require(
      effects == 6,
      "six provider notices update graph state and request a typed UI effect");
  require(neutral == 2, "two internal raw-response events are neutral");
  require(!findProtocolMethod(ProtocolDirection::ServerNotification,
                              "unknown/method"),
          "unknown methods are not silently classified");
}

void everyKnownMethodDispatches() {
  std::uint64_t ordinal = 0;
  for (const MethodDescriptor &descriptor : protocolMethods()) {
    ++ordinal;
    NodeGraph graph;
    ProtocolUpdater updater(graph);
    const std::string suffix = std::to_string(ordinal);
    DecodedMessage message;
    message.kind = decodedKind(descriptor.direction);
    message.method = std::string(descriptor.method);
    message.payload = {
        {"semanticMarker", Value("marker-" + suffix)},
        {"threadId", Value("thread-" + suffix)},
        {"turnId", Value("turn-" + suffix)},
        {"itemId", Value("item-" + suffix)},
        {"targetItemId", Value("target-" + suffix)},
        {"reviewId", Value("review-" + suffix)},
        {"projectId", Value("project-" + suffix)},
        {"processId", Value("process-" + suffix)},
        {"watchId", Value("watch-" + suffix)},
        {"sessionId", Value("session-" + suffix)},
        {"realtimeSessionId", Value("realtime-" + suffix)},
        {"subscriptionId", Value("subscription-" + suffix)},
        {"importId", Value("import-" + suffix)},
        {"name", Value("name-" + suffix)},
        {"delta", Value("delta-" + suffix)},
        {"deltaBase64", Value("ZGVsdGE=")},
        {"stream", Value("stdout")},
        {"status", Value("running")},
        {"thread", Value(Value::Object{{"id", Value("thread-" + suffix)},
                                       {"turns", Value(Value::Array{})}})},
        {"turn", Value(Value::Object{{"id", Value("turn-" + suffix)},
                                     {"items", Value(Value::Array{})}})},
        {"item", Value(Value::Object{
                     {"id", Value("item-" + suffix)},
                     {"type", Value("agentMessage")},
                     {"realtimeSessionId", Value("realtime-" + suffix)}})},
        {"run", Value(Value::Object{{"id", Value("hook-" + suffix)},
                                    {"status", Value("running")}})},
        {"data", Value(Value::Array{Value(
                     Value::Object{{"id", Value("entry-" + suffix)},
                                   {"name", Value("entry-" + suffix)}})})},
        {"requestId", Value(std::int64_t{9000})}};
    if (descriptor.direction == ProtocolDirection::ClientRequest ||
        descriptor.direction == ProtocolDirection::ServerRequest)
      message.requestId.emplace(std::int64_t(ordinal));

    if (descriptor.direction == ProtocolDirection::ServerNotification &&
        descriptor.method == "thread/deleted") {
      auto write = graph.write();
      static_cast<void>(write.upsert({NodeKind::Thread, "thread-" + suffix}));
      static_cast<void>(write.finish());
    }
    if (descriptor.direction == ProtocolDirection::ServerNotification &&
        descriptor.method == "serverRequest/resolved") {
      static_cast<void>(updater.apply(
          {DecodedMessageKind::ServerRequest,
           "item/commandExecution/requestApproval", ProtocolRequestId(9000),
           Value::Object{{"threadId", Value("thread-" + suffix)}}}));
    }

    const std::uint64_t before = graph.publishedRevision();
    const ApplyResult result = updater.apply(std::move(message));
    require(result.knownMethod, "known catalog method dispatches as known");
    require(result.disposition == descriptor.disposition,
            "dispatch returns the catalog disposition");
    if (descriptor.direction == ProtocolDirection::ClientRequest) {
      auto read = graph.tryRead();
      const auto state = result.primary ? read->state(result.primary) : nullptr;
      const Value *requestPayload = field(state, "requestPayload");
      const Value *marker =
          objectField(requestPayload ? requestPayload->asObject() : nullptr,
                      "semanticMarker");
      require(
          result.primary && result.primary->id().kind == NodeKind::Operation &&
              state && state->status == NodeStatus::Pending && marker &&
              marker->asString() && *marker->asString() == "marker-" + suffix,
          "every client request retains its payload in one pending "
          "operation");
      read.reset();
      const ApplyResult completed = updater.apply(
          {DecodedMessageKind::ClientResult, std::string(descriptor.method),
           ProtocolRequestId(static_cast<std::int64_t>(ordinal)),
           Value::Object{{"resultMarker", Value("result-" + suffix)}},
           result.primary});
      read = graph.tryRead();
      require(
          completed.knownMethod &&
              !read->find({NodeKind::Operation,
                           ProtocolRequestId(static_cast<std::int64_t>(ordinal))
                               .canonical()}),
          "every client result consumes its exact request correlation");
    } else if (descriptor.direction == ProtocolDirection::ServerRequest) {
      auto read = graph.tryRead();
      const auto state = result.primary ? read->state(result.primary) : nullptr;
      const Value *payload = field(state, "payload");
      const Value *marker = objectField(payload ? payload->asObject() : nullptr,
                                        "semanticMarker");
      require(
          result.primary &&
              result.primary->id().kind == NodeKind::Interaction && state &&
              state->status == NodeStatus::Pending && marker &&
              marker->asString() && *marker->asString() == "marker-" + suffix &&
              !read->related(result.primary, RelationKind::InteractionTarget)
                   .empty(),
          "every server request retains its payload and target relation");
      read.reset();
      const NodeRef interaction = result.primary;
      static_cast<void>(updater.resolveInteraction(interaction, true));
      read = graph.tryRead();
      require(!read->find(interaction->id()),
              "every reverse interaction resolves by exact NodeRef");
    } else if (descriptor.disposition ==
               MessageDisposition::IntentionallyStateNeutral) {
      require(result.change.empty(), "state-neutral method changes no nodes");
      require(graph.publishedRevision() == before,
              "state-neutral method does not publish a revision");
    } else {
      require(!result.change.empty() && graph.publishedRevision() > before,
              "state-bearing notification publishes concrete state: " +
                  std::string(descriptor.method));
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

void streamedTextIsBoundedAndReportsOmission() {
  constexpr std::size_t RetainedTailBytes = 192 * 1024;
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  Value::Array items{Value(Value::Object{{"id", Value("bounded-agent")},
                                         {"type", Value("agentMessage")}}),
                     Value(Value::Object{{"id", Value("bounded-reasoning")},
                                         {"type", Value("reasoning")}})};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("bounded-stream-thread")},
                {"turns", Value(Value::Array{Value(Value::Object{
                              {"id", Value("bounded-stream-turn")},
                              {"items", Value(std::move(items))}})})}})}}}));

  std::string firstChunk;
  firstChunk.reserve(300001);
  for (std::size_t index = 0; index < 100000; ++index)
    firstChunk.append("\xE2\x82\xAC");
  firstChunk.push_back('x');
  const std::size_t firstChunkBytes = firstChunk.size();
  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerNotification,
                     "item/agentMessage/delta", std::nullopt,
                     Value::Object{{"threadId", Value("bounded-stream-thread")},
                                   {"turnId", Value("bounded-stream-turn")},
                                   {"itemId", Value("bounded-agent")},
                                   {"delta", Value(std::move(firstChunk))}}}));

  const auto retentionCounts = [](const std::shared_ptr<const NodeState> &state,
                                  std::string_view retainedField) {
    const Value *retention = field(state, "textRetention");
    const Value *entry =
        objectField(retention ? retention->asObject() : nullptr, retainedField);
    const Value *discarded =
        objectField(entry ? entry->asObject() : nullptr, "discardedBytes");
    const Value *retained =
        objectField(entry ? entry->asObject() : nullptr, "retainedBytes");
    return std::pair{
        discarded && discarded->asUInt64() ? *discarded->asUInt64() : 0U,
        retained && retained->asUInt64() ? *retained->asUInt64() : 0U};
  };

  {
    auto read = graph.tryRead();
    const NodeRef item = findItem(*read, "bounded-stream-thread",
                                  "bounded-stream-turn", "bounded-agent");
    const auto state = read->state(item);
    const std::string *text = field(state, "text")->asString();
    const auto [discarded, retained] = retentionCounts(state, "text");
    require(text && !text->empty() && text->size() <= RetainedTailBytes &&
                (static_cast<unsigned char>(text->front()) & 0xc0U) != 0x80U &&
                text->back() == 'x' && retained == text->size() &&
                discarded + retained == firstChunkBytes,
            "large agent deltas retain a UTF-8-aligned bounded tail and exact "
            "omission metadata");
  }

  std::string secondChunk(100 * 1024, 'z');
  const std::size_t totalAgentBytes = firstChunkBytes + secondChunk.size();
  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerNotification,
                     "item/agentMessage/delta", std::nullopt,
                     Value::Object{{"threadId", Value("bounded-stream-thread")},
                                   {"turnId", Value("bounded-stream-turn")},
                                   {"itemId", Value("bounded-agent")},
                                   {"delta", Value(std::move(secondChunk))}}}));
  {
    auto read = graph.tryRead();
    const auto state =
        read->state(findItem(*read, "bounded-stream-thread",
                             "bounded-stream-turn", "bounded-agent"));
    const std::string *text = field(state, "text")->asString();
    const auto [discarded, retained] = retentionCounts(state, "text");
    require(text && text->size() <= RetainedTailBytes &&
                retained == text->size() &&
                discarded + retained == totalAgentBytes,
            "successive stream truncations accumulate exact omitted bytes "
            "without growing current node state");
  }

  std::string reasoningChunk(270 * 1024, 'r');
  const std::size_t reasoningBytes = reasoningChunk.size();
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification,
       "item/reasoning/summaryTextDelta", std::nullopt,
       Value::Object{{"threadId", Value("bounded-stream-thread")},
                     {"turnId", Value("bounded-stream-turn")},
                     {"itemId", Value("bounded-reasoning")},
                     {"summaryIndex", Value(std::uint64_t{0})},
                     {"delta", Value(std::move(reasoningChunk))}}}));
  {
    auto read = graph.tryRead();
    const auto state =
        read->state(findItem(*read, "bounded-stream-thread",
                             "bounded-stream-turn", "bounded-reasoning"));
    const Value::Array *summary = field(state, "summary")->asArray();
    const std::string *text =
        summary && !summary->empty() ? summary->front().asString() : nullptr;
    const auto [discarded, retained] = retentionCounts(state, "summary");
    require(summary && summary->size() == 1 && text &&
                text->size() <= RetainedTailBytes && retained == text->size() &&
                discarded + retained == reasoningBytes,
            "indexed reasoning streams retain one bounded tail with omission "
            "metadata");
  }
  const std::uint64_t beforeSparseIndex = graph.publishedRevision();
  const ApplyResult sparseIndex =
      updater.apply({DecodedMessageKind::ServerNotification,
                     "item/reasoning/summaryTextDelta", std::nullopt,
                     Value::Object{{"threadId", Value("bounded-stream-thread")},
                                   {"turnId", Value("bounded-stream-turn")},
                                   {"itemId", Value("bounded-reasoning")},
                                   {"summaryIndex", Value(std::uint64_t{5000})},
                                   {"delta", Value("ignored")}}});
  require(sparseIndex.change.empty() &&
              graph.publishedRevision() == beforeSparseIndex,
          "an out-of-range text index cannot allocate an unbounded sparse "
          "array or publish a false change");

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/completed", std::nullopt,
       Value::Object{
           {"threadId", Value("bounded-stream-thread")},
           {"turnId", Value("bounded-stream-turn")},
           {"item", Value(Value::Object{{"id", Value("bounded-agent")},
                                        {"type", Value("agentMessage")},
                                        {"text", Value("complete text")}})}}}));
  {
    auto read = graph.tryRead();
    const auto state =
        read->state(findItem(*read, "bounded-stream-thread",
                             "bounded-stream-turn", "bounded-agent"));
    require(field(state, "text") && field(state, "text")->asString() &&
                *field(state, "text")->asString() == "complete text" &&
                !field(state, "textRetention"),
            "authoritative completion replaces the stream tail and clears its "
            "obsolete truncation notice");
  }
}

void longStreamingDeltasStayBoundedInStateAndCost() {
  constexpr std::size_t MaximumRetainedBytes = 256 * 1024;
  constexpr std::size_t WarmupDeltas = 4097;
  constexpr std::size_t FirstMeasuredDeltas = 1024;
  constexpr std::size_t SecondMeasuredDeltas = 2048;
  const std::string chunk(64, 's');

  NodeGraph graph;
  ProtocolUpdater updater(graph);
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("long-stream-thread")},
                {"turns",
                 Value(Value::Array{Value(Value::Object{
                     {"id", Value("long-stream-turn")},
                     {"items",
                      Value(Value::Array{Value(Value::Object{
                          {"id", Value("long-stream-item")},
                          {"type", Value("agentMessage")}})})}})})}})}}}));
  const std::uint64_t initialRevision = graph.publishedRevision();

  const auto append = [&](std::size_t count) {
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < count; ++index)
      static_cast<void>(updater.apply(
          {DecodedMessageKind::ServerNotification, "item/agentMessage/delta",
           std::nullopt,
           Value::Object{{"threadId", Value("long-stream-thread")},
                         {"turnId", Value("long-stream-turn")},
                         {"itemId", Value("long-stream-item")},
                         {"delta", Value(chunk)}}}));
    return std::chrono::steady_clock::now() - started;
  };

  static_cast<void>(append(WarmupDeltas));
  const auto firstElapsed = append(FirstMeasuredDeltas);
  const auto secondElapsed = append(SecondMeasuredDeltas);
  const auto allowance = firstElapsed * 3 + std::chrono::milliseconds(25);

  auto read = graph.tryRead();
  const NodeRef item = read ? findItem(*read, "long-stream-thread",
                                       "long-stream-turn", "long-stream-item")
                            : NodeRef{};
  const auto state = item ? read->state(item) : nullptr;
  const Value *textValue = field(state, "text");
  const std::string *text = textValue ? textValue->asString() : nullptr;
  const Value *retentionValue = field(state, "textRetention");
  const Value *entry = objectField(
      retentionValue ? retentionValue->asObject() : nullptr, "text");
  const Value *discarded =
      objectField(entry ? entry->asObject() : nullptr, "discardedBytes");
  const std::uint64_t totalBytes =
      static_cast<std::uint64_t>(WarmupDeltas + FirstMeasuredDeltas +
                                 SecondMeasuredDeltas) *
      chunk.size();
  require(
      text && text->size() <= MaximumRetainedBytes && discarded &&
          discarded->asUInt64() &&
          *discarded->asUInt64() + text->size() == totalBytes &&
          graph.publishedRevision() == initialRevision + WarmupDeltas +
                                           FirstMeasuredDeltas +
                                           SecondMeasuredDeltas &&
          secondElapsed <= allowance,
      "long streaming publishes once per input with bounded retained text and "
      "approximately linear steady-state cost");
  std::cout
      << "long-stream steady-state ns: "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(firstElapsed)
             .count()
      << " / "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(secondElapsed)
             .count()
      << '\n';
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

void activeTurnRelationTracksLifecycle() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("active-thread")},
           {"turn", Value(Value::Object{{"id", Value("active-turn")},
                                        {"status", Value("inProgress")}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "active-thread"});
    const NodeRef turn = findTurn(*read, "active-thread", "active-turn");
    require(thread && turn &&
                read->related(thread, RelationKind::ActiveTurn) ==
                    std::vector<NodeRef>{turn},
            "turn start records the current active turn directly");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/status/changed",
       std::nullopt,
       Value::Object{
           {"threadId", Value("active-thread")},
           {"status", Value(Value::Object{{"type", Value("idle")}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "active-thread"});
    require(thread && read->state(thread)->status == NodeStatus::Completed &&
                read->related(thread, RelationKind::ActiveTurn).empty(),
            "object-shaped idle thread status normalizes typed state and "
            "clears the direct active-turn relation");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("active-thread")},
           {"turn", Value(Value::Object{{"id", Value("active-turn")},
                                        {"status", Value("inProgress")}})}}}));

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/completed", std::nullopt,
       Value::Object{
           {"threadId", Value("active-thread")},
           {"turn", Value(Value::Object{{"id", Value("active-turn")}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "active-thread"});
    require(thread && read->related(thread, RelationKind::ActiveTurn).empty(),
            "turn completion clears the direct active-turn relation even "
            "when the payload omits status");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("active-thread")},
           {"turn", Value(Value::Object{{"id", Value("closing-turn")},
                                        {"status", Value("inProgress")}})}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/closed", std::nullopt,
       Value::Object{{"threadId", Value("active-thread")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "active-thread"});
    const Value *status = field(read->state(thread), "status");
    require(thread && read->state(thread)->status == NodeStatus::NotLoaded &&
                status && status->asString() &&
                *status->asString() == "notLoaded" &&
                read->related(thread, RelationKind::ActiveTurn).empty(),
            "closing a thread clears its active turn and converges both "
            "typed and protocol-facing status");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/status/changed",
       std::nullopt,
       Value::Object{
           {"threadId", Value("active-thread")},
           {"status", Value(Value::Object{{"type", Value("systemError")}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "active-thread"});
    require(thread && read->state(thread)->status == NodeStatus::Failed,
            "object-shaped systemError status normalizes to typed failure");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("active-thread")},
           {"turn", Value(Value::Object{{"id", Value("stale-active")},
                                        {"status", Value("inProgress")}})}}}));
  NodeRef staleActive;
  {
    auto read = graph.tryRead();
    staleActive = findTurn(*read, "active-thread", "stale-active");
  }
  Value::Array replacementTurns{Value(Value::Object{
      {"id", Value("replacement-turn")}, {"status", Value("completed")}})};
  const ApplyResult replacement = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read",
       ProtocolRequestId("active-thread-replacement"),
       Value::Object{
           {"thread", Value(Value::Object{
                          {"id", Value("active-thread")},
                          {"turns", Value(std::move(replacementTurns))}})}}});
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "active-thread"});
    require(thread && staleActive &&
                !findTurn(*read, "active-thread", "stale-active") &&
                read->removed(staleActive) &&
                std::ranges::find(replacement.change.removed, staleActive) !=
                    replacement.change.removed.end() &&
                read->related(thread, RelationKind::ActiveTurn).empty(),
            "authoritative history replacement retires an omitted active "
            "turn and clears it as the current action target");
  }
}

void effectiveThreadSettingsConvergeAcrossWireShapes() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/resume",
       ProtocolRequestId("settings-resume"),
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("settings-thread")}})},
           {"model", Value("gpt-current")},
           {"reasoningEffort", Value("high")},
           {"approvalPolicy", Value("never")},
           {"sandbox", Value("workspaceWrite")},
           {"activePermissionProfile", Value("trusted")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "settings-thread"});
    const auto state = read->state(thread);
    require(thread && field(state, "model") &&
                *field(state, "model")->asString() == "gpt-current" &&
                field(state, "reasoningEffort") &&
                *field(state, "reasoningEffort")->asString() == "high" &&
                field(state, "approvalPolicy") &&
                *field(state, "approvalPolicy")->asString() == "never" &&
                field(state, "sandbox") &&
                *field(state, "sandbox")->asString() == "workspaceWrite" &&
                field(state, "activePermissionProfile") &&
                *field(state, "activePermissionProfile")->asString() ==
                    "trusted",
            "thread resume wrapper settings become direct current thread "
            "state");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/settings/updated",
       std::nullopt,
       Value::Object{
           {"threadId", Value("settings-thread")},
           {"threadSettings",
            Value(Value::Object{{"model", Value("gpt-next")},
                                {"effort", Value("medium")},
                                {"sandboxPolicy", Value("readOnly")},
                                {"personality", Value("friendly")}})}}}));
  std::uint64_t firstSettingsRevision = 0;
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "settings-thread"});
    const auto state = read->state(thread);
    const Value *revision = field(state, "settingsRevision");
    firstSettingsRevision =
        revision && revision->asUInt64() ? *revision->asUInt64() : 0;
    require(field(state, "model") &&
                *field(state, "model")->asString() == "gpt-next" &&
                field(state, "effort") &&
                *field(state, "effort")->asString() == "medium" &&
                !field(state, "reasoningEffort") &&
                field(state, "sandboxPolicy") &&
                *field(state, "sandboxPolicy")->asString() == "readOnly" &&
                !field(state, "sandbox") && firstSettingsRevision != 0,
            "modern settings merge sparsely and retire stale legacy aliases");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/settings/updated",
       std::nullopt,
       Value::Object{
           {"threadId", Value("settings-thread")},
           {"threadSettings",
            Value(Value::Object{{"personality", Value(nullptr)}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "settings-thread"});
    const auto state = read->state(thread);
    const Value *latest = field(state, "latestSettingsUpdate");
    const Value::Object *latestObject = latest ? latest->asObject() : nullptr;
    const Value *revision = field(state, "settingsRevision");
    require(field(state, "model") &&
                *field(state, "model")->asString() == "gpt-next" &&
                !field(state, "personality") && latestObject &&
                latestObject->size() == 1 &&
                latestObject->contains("personality") &&
                latestObject->at("personality").isNull() && revision &&
                revision->asUInt64() &&
                *revision->asUInt64() > firstSettingsRevision,
            "sparse settings retain prior facts, merge explicit null, retain "
            "the latest patch, and advance a settings-only revision");
  }
}

void threadItemPagesMaintainScopedContainmentAndOrder() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const ProtocolRequestId firstId("items-page-one");
  const ApplyResult firstRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/items/list", firstId,
       Value::Object{{"threadId", Value("items-thread")},
                     {"turnId", Value("items-turn")},
                     {"sortDirection", Value("desc")}}});
  Value::Array firstPage{
      Value(Value::Object{
          {"turnId", Value("items-turn")},
          {"item", Value(Value::Object{{"id", Value("newer-item")},
                                       {"type", Value("agentMessage")}})}}),
      Value(Value::Object{
          {"turnId", Value("items-turn")},
          {"item", Value(Value::Object{{"id", Value("older-item")},
                                       {"type", Value("agentMessage")}})}})};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/items/list", firstId,
       Value::Object{{"data", Value(std::move(firstPage))},
                     {"nextCursor", Value("older-page")}},
       firstRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef turn = findTurn(*read, "items-thread", "items-turn");
    const auto state = read->state(turn);
    require(turn &&
                protocolIds(*read, read->children(turn)) ==
                    std::vector<std::string>{"older-item", "newer-item"} &&
                field(state, "itemsHistoryHasMore") &&
                *field(state, "itemsHistoryHasMore")->asBool() &&
                field(state, "itemsHistoryNextCursor") &&
                *field(state, "itemsHistoryNextCursor")->asString() ==
                    "older-page",
            "thread/items/list decodes entry containment, canonicalizes a "
            "descending page, and retains its cursor on the addressed turn");
  }

  const ProtocolRequestId secondId("items-page-two");
  const ApplyResult secondRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/items/list", secondId,
       Value::Object{{"threadId", Value("items-thread")},
                     {"turnId", Value("items-turn")},
                     {"cursor", Value("older-page")},
                     {"sortDirection", Value("desc")}}});
  Value::Array secondPage{Value(Value::Object{
      {"turnId", Value("items-turn")},
      {"item", Value(Value::Object{{"id", Value("oldest-item")},
                                   {"type", Value("agentMessage")}})}})};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/items/list", secondId,
       Value::Object{{"data", Value(std::move(secondPage))},
                     {"nextCursor", Value(nullptr)},
                     {"backwardsCursor", Value("newer-page")}},
       secondRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef turn = findTurn(*read, "items-thread", "items-turn");
    const auto state = read->state(turn);
    require(protocolIds(*read, read->children(turn)) ==
                    std::vector<std::string>{"oldest-item", "older-item",
                                             "newer-item"} &&
                field(state, "itemsHistoryHasMore") &&
                !*field(state, "itemsHistoryHasMore")->asBool() &&
                !field(state, "itemsHistoryNextCursor") &&
                field(state, "itemsHistoryBackwardsCursor") &&
                *field(state, "itemsHistoryBackwardsCursor")->asString() ==
                    "newer-page",
            "later item pages prepend without rebuilding or duplicating the "
            "current turn order");
  }
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
                    std::vector<std::string>{"structural-child"} &&
                read->related(child, RelationKind::ThreadOwner) ==
                    std::vector<NodeRef>{parent},
            "parentThreadId creates direct structural and owner relations");
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
                    std::vector<NodeRef>{child} &&
                read->related(child, RelationKind::ThreadOwner) ==
                    std::vector<NodeRef>{parent},
            "agent activity relates both its owner thread and source item to "
            "the stable child thread");
  }
}

void agentChildAggregatesTrackEveryReferencingItem() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  for (const std::string_view id :
       {"shared-agent-child", "replacement-agent-child"}) {
    static_cast<void>(updater.apply(
        {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
         Value::Object{{"thread", Value(Value::Object{{"id", Value(id)}})}}}));
  }

  const auto agentItem = [](std::string id, std::string childId) {
    return Value(Value::Object{{"id", Value(std::move(id))},
                               {"type", Value("subAgentActivity")},
                               {"agentThreadId", Value(std::move(childId))}});
  };
  Value::Object owner{
      {"id", Value("agent-owner")},
      {"turns",
       Value(Value::Array{Value(Value::Object{
           {"id", Value("agent-owner-turn")},
           {"items",
            Value(Value::Array{
                agentItem("first-agent-item", "shared-agent-child"),
                agentItem("second-agent-item", "shared-agent-child")})}})})}};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(std::move(owner))}}}));

  NodeRef firstItem;
  NodeRef secondItem;
  NodeRef sharedChild;
  NodeRef replacementChild;
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef ownerThread = read->find({NodeKind::Thread, "agent-owner"});
    firstItem =
        findItem(*read, "agent-owner", "agent-owner-turn", "first-agent-item");
    secondItem =
        findItem(*read, "agent-owner", "agent-owner-turn", "second-agent-item");
    sharedChild = read->find({NodeKind::Thread, "shared-agent-child"});
    replacementChild =
        read->find({NodeKind::Thread, "replacement-agent-child"});
    const auto roots = read->related(runtime, RelationKind::RootThread);
    require(ownerThread && firstItem && secondItem && sharedChild &&
                replacementChild &&
                read->related(firstItem, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{sharedChild} &&
                read->related(secondItem, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{sharedChild} &&
                read->related(ownerThread, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{sharedChild} &&
                read->related(sharedChild, RelationKind::ThreadOwner) ==
                    std::vector<NodeRef>{ownerThread} &&
                std::ranges::find(roots, sharedChild) == roots.end(),
            "multiple items share one owner-level agent-child relation");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/completed", std::nullopt,
       Value::Object{{"threadId", Value("agent-owner")},
                     {"turnId", Value("agent-owner-turn")},
                     {"item", agentItem("first-agent-item",
                                        "replacement-agent-child")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef ownerThread = read->find({NodeKind::Thread, "agent-owner"});
    const auto roots = read->related(runtime, RelationKind::RootThread);
    require(ownerThread && firstItem && secondItem && sharedChild &&
                replacementChild &&
                read->related(firstItem, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{replacementChild} &&
                read->related(secondItem, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{sharedChild} &&
                read->related(ownerThread, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{replacementChild, sharedChild} &&
                read->related(sharedChild, RelationKind::ThreadOwner) ==
                    std::vector<NodeRef>{ownerThread} &&
                read->related(replacementChild, RelationKind::ThreadOwner) ==
                    std::vector<NodeRef>{ownerThread} &&
                std::ranges::find(roots, sharedChild) == roots.end() &&
                std::ranges::find(roots, replacementChild) == roots.end(),
            "reassigning one of multiple items preserves the aggregate edge "
            "still referenced by its sibling item");
  }

  const ProtocolRequestId firstReadId("agent-owner-first-read");
  const ApplyResult firstRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/read", firstReadId,
       Value::Object{{"threadId", Value("agent-owner")}}});
  Value::Object retainedOwner{
      {"id", Value("agent-owner")},
      {"turns",
       Value(Value::Array{Value(Value::Object{
           {"id", Value("agent-owner-turn")},
           {"items", Value(Value::Array{agentItem(
                         "second-agent-item", "shared-agent-child")})}})})}};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read", firstReadId,
       Value::Object{{"thread", Value(std::move(retainedOwner))}},
       firstRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef ownerThread = read->find({NodeKind::Thread, "agent-owner"});
    const auto roots = read->related(runtime, RelationKind::RootThread);
    require(firstItem && read->removed(firstItem) &&
                !read->find(firstItem->id()) && ownerThread && secondItem &&
                read->find(secondItem->id()) == secondItem && sharedChild &&
                replacementChild &&
                read->related(ownerThread, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{sharedChild} &&
                read->related(sharedChild, RelationKind::ThreadOwner) ==
                    std::vector<NodeRef>{ownerThread} &&
                read->related(replacementChild, RelationKind::ThreadOwner)
                    .empty() &&
                std::ranges::find(roots, sharedChild) == roots.end() &&
                std::ranges::find(roots, replacementChild) != roots.end(),
            "removing one referencing item keeps the shared owner aggregate "
            "and releases only its unreferenced child");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/completed", std::nullopt,
       Value::Object{{"threadId", Value("agent-owner")},
                     {"turnId", Value("agent-owner-turn")},
                     {"item", agentItem("second-agent-item",
                                        "replacement-agent-child")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef ownerThread = read->find({NodeKind::Thread, "agent-owner"});
    const auto roots = read->related(runtime, RelationKind::RootThread);
    require(ownerThread && secondItem && sharedChild && replacementChild &&
                read->related(secondItem, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{replacementChild} &&
                read->related(ownerThread, RelationKind::AgentChildThread) ==
                    std::vector<NodeRef>{replacementChild} &&
                read->related(sharedChild, RelationKind::ThreadOwner).empty() &&
                read->related(replacementChild, RelationKind::ThreadOwner) ==
                    std::vector<NodeRef>{ownerThread} &&
                std::ranges::find(roots, sharedChild) != roots.end() &&
                std::ranges::find(roots, replacementChild) == roots.end(),
            "reassigning the final referencing item replaces only its exact "
            "owner aggregate and promotes the released child");
  }

  const ProtocolRequestId emptyReadId("agent-owner-empty-read");
  const ApplyResult emptyRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/read", emptyReadId,
       Value::Object{{"threadId", Value("agent-owner")}}});
  Value::Object emptyOwner{{"id", Value("agent-owner")},
                           {"turns", Value(Value::Array{Value(Value::Object{
                                         {"id", Value("agent-owner-turn")},
                                         {"items", Value(Value::Array{})}})})}};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read", emptyReadId,
       Value::Object{{"thread", Value(std::move(emptyOwner))}},
       emptyRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const NodeRef ownerThread = read->find({NodeKind::Thread, "agent-owner"});
    const auto roots = read->related(runtime, RelationKind::RootThread);
    require(secondItem && read->removed(secondItem) &&
                !read->find(secondItem->id()) && ownerThread &&
                replacementChild &&
                read->related(ownerThread, RelationKind::AgentChildThread)
                    .empty() &&
                read->related(replacementChild, RelationKind::ThreadOwner)
                    .empty() &&
                std::ranges::find(roots, replacementChild) != roots.end(),
            "removing the final referencing item clears the aggregate and "
            "promotes its released child");
  }
}

void forkRelationsFollowTheCurrentSource() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  for (const std::string_view id : {"fork-source-a", "fork-source-b"}) {
    static_cast<void>(updater.apply(
        {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
         Value::Object{{"thread", Value(Value::Object{{"id", Value(id)}})}}}));
  }
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/fork",
       ProtocolRequestId("initial-fork"),
       Value::Object{
           {"thread",
            Value(Value::Object{{"id", Value("changing-fork")},
                                {"forkedFromId", Value("fork-source-a")}})}}}));

  NodeRef fork;
  {
    auto read = graph.tryRead();
    const NodeRef sourceA = read->find({NodeKind::Thread, "fork-source-a"});
    fork = read->find({NodeKind::Thread, "changing-fork"});
    require(sourceA && fork &&
                read->related(sourceA, RelationKind::ForkChildThread) ==
                    std::vector<NodeRef>{fork},
            "a fork starts with one source-to-child relation");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{{"id", Value("changing-fork")},
                                {"forkedFromId", Value("fork-source-b")}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef sourceA = read->find({NodeKind::Thread, "fork-source-a"});
    const NodeRef sourceB = read->find({NodeKind::Thread, "fork-source-b"});
    require(read->find({NodeKind::Thread, "changing-fork"}) == fork &&
                read->related(sourceA, RelationKind::ForkChildThread).empty() &&
                read->related(sourceB, RelationKind::ForkChildThread) ==
                    std::vector<NodeRef>{fork},
            "fork reassignment removes the old source-to-child direction "
            "before adding the new source");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{{"thread", Value(Value::Object{
                                    {"id", Value("changing-fork")},
                                    {"forkedFromId", Value(nullptr)}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef sourceA = read->find({NodeKind::Thread, "fork-source-a"});
    const NodeRef sourceB = read->find({NodeKind::Thread, "fork-source-b"});
    require(read->find({NodeKind::Thread, "changing-fork"}) == fork &&
                read->related(sourceA, RelationKind::ForkChildThread).empty() &&
                read->related(sourceB, RelationKind::ForkChildThread).empty(),
            "clearing forkedFromId removes the prior source relation while "
            "preserving the fork NodeRef");
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

void hookRunsKeepNestedIdentityAndCurrentOwnership() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const auto notify = [&](std::string method, Value::Object payload) {
    return updater.apply({DecodedMessageKind::ServerNotification,
                          std::move(method), std::nullopt, std::move(payload)});
  };

  const ApplyResult firstStarted = notify(
      "hook/started",
      Value::Object{
          {"threadId", Value("hook-thread")},
          {"turnId", Value("hook-turn")},
          {"run", Value(Value::Object{{"id", Value("hook-run-a")},
                                      {"eventName", Value("preToolUse")},
                                      {"handlerType", Value("command")},
                                      {"status", Value("running")},
                                      {"startedAt", Value(std::int64_t{100})},
                                      {"entries", Value(Value::Array{})}})}});
  const ApplyResult secondStarted = notify(
      "hook/started",
      Value::Object{{"threadId", Value("hook-thread")},
                    {"run", Value(Value::Object{
                                {"id", Value("hook-run-b")},
                                {"eventName", Value("postToolUse")},
                                {"handlerType", Value("mcpTool")},
                                {"status", Value("running")},
                                {"startedAt", Value(std::int64_t{110})}})}});

  NodeRef first;
  NodeRef second;
  std::uint64_t secondStartedRevision = 0;
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "hook-thread"});
    const NodeRef turn = findTurn(*read, "hook-thread", "hook-turn");
    first = read->find({NodeKind::Hook, "hook-run-a"});
    second = read->find({NodeKind::Hook, "hook-run-b"});
    const auto firstState = read->state(first);
    const auto secondState = read->state(second);
    secondStartedRevision = read->changedRevision(second);
    require(first && second && first != second && thread && turn &&
                read->parent(first) == turn && read->parent(turn) == thread &&
                read->parent(second) == thread,
            "nested run ids preserve concurrent hook nodes under their "
            "optional turn or thread owner");
    require(firstState->status == NodeStatus::Running &&
                secondState->status == NodeStatus::Running &&
                protocolCanonicalId(*firstState, first) == "hook-run-a" &&
                protocolCanonicalId(*secondState, second) == "hook-run-b" &&
                field(firstState, "eventName") &&
                *field(firstState, "eventName")->asString() == "preToolUse" &&
                field(firstState, "threadId") &&
                *field(firstState, "threadId")->asString() == "hook-thread" &&
                field(firstState, "turnId") &&
                *field(firstState, "turnId")->asString() == "hook-turn",
            "hook starts retain the current nested summary and protocol "
            "addressing fields");
    require(std::ranges::find(firstStarted.change.affected, first) !=
                    firstStarted.change.affected.end() &&
                std::ranges::find(secondStarted.change.affected, second) !=
                    secondStarted.change.affected.end(),
            "each hook start reports its independently affected run");
  }

  const ApplyResult firstCompleted = notify(
      "hook/completed",
      Value::Object{
          {"threadId", Value("hook-thread")},
          {"turnId", Value("hook-turn")},
          {"run",
           Value(Value::Object{
               {"id", Value("hook-run-a")},
               {"eventName", Value("preToolUse")},
               {"status", Value("completed")},
               {"completedAt", Value(std::int64_t{145})},
               {"durationMs", Value(std::int64_t{45})},
               {"statusMessage", Value("accepted")},
               {"entries", Value(Value::Array{Value(Value::Object{
                               {"kind", Value("context")},
                               {"text", Value("current output")}})})}})}});
  {
    auto read = graph.tryRead();
    const NodeRef currentFirst = read->find({NodeKind::Hook, "hook-run-a"});
    const NodeRef currentSecond = read->find({NodeKind::Hook, "hook-run-b"});
    const auto firstState = read->state(currentFirst);
    const auto secondState = read->state(currentSecond);
    const Value *entries = field(firstState, "entries");
    require(
        currentFirst == first && currentSecond == second &&
            firstState->status == NodeStatus::Completed &&
            field(firstState, "startedAt") &&
            *field(firstState, "startedAt")->asInt64() == 100 &&
            field(firstState, "completedAt") &&
            *field(firstState, "completedAt")->asInt64() == 145 &&
            field(firstState, "lastMethod") &&
            *field(firstState, "lastMethod")->asString() == "hook/completed" &&
            entries && entries->asArray() && entries->asArray()->size() == 1,
        "hook completion updates the same run with its latest fields "
        "while retaining still-current start facts");
    require(secondState->status == NodeStatus::Running &&
                read->changedRevision(second) == secondStartedRevision &&
                std::ranges::find(firstCompleted.change.affected, second) ==
                    firstCompleted.change.affected.end(),
            "completing one hook run does not conflate or rewrite a concurrent "
            "run");
  }

  static_cast<void>(notify(
      "hook/completed",
      Value::Object{{"threadId", Value("hook-thread")},
                    {"run", Value(Value::Object{
                                {"id", Value("hook-run-b")},
                                {"status", Value("blocked")},
                                {"statusMessage",
                                 Value("policy prevented execution")}})}}));
  static_cast<void>(notify(
      "hook/completed",
      Value::Object{{"threadId", Value("hook-thread")},
                    {"run", Value(Value::Object{
                                {"id", Value("hook-run-c")},
                                {"completedAt", Value(std::int64_t{200})}})}}));
  {
    auto read = graph.tryRead();
    const NodeRef blocked = read->find({NodeKind::Hook, "hook-run-b"});
    const NodeRef completed = read->find({NodeKind::Hook, "hook-run-c"});
    require(
        blocked == second &&
            read->state(blocked)->status == NodeStatus::Failed && completed &&
            read->state(completed)->status == NodeStatus::Completed &&
            field(read->state(completed), "status") &&
            *field(read->state(completed), "status")->asString() == "completed",
        "hook terminal status is normalized while the raw current status "
        "remains available on the node");
  }

  const std::uint64_t beforeInvalid = graph.publishedRevision();
  const ApplyResult invalid =
      notify("hook/started",
             Value::Object{
                 {"threadId", Value("hook-thread")},
                 {"run", Value(Value::Object{{"status", Value("running")}})}});
  {
    auto read = graph.tryRead();
    require(invalid.change.empty() && read->revision() == beforeInvalid &&
                !read->find({NodeKind::Hook, "hook/started"}),
            "a hook notification without nested run.id is explicitly "
            "state-neutral instead of collapsing unrelated runs");
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
    write.relate(turn, RelationKind::TurnRootItem, local);
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
  require(read->related(read->parent(authoritative),
                        RelationKind::TurnRootItem) ==
                  std::vector<NodeRef>{authoritative} &&
              read->related(read->parent(local), RelationKind::TurnRootItem) ==
                  std::vector<NodeRef>{local},
          "inbound materialization roots the provider Turn without changing "
          "the still-unacknowledged optimistic Turn ownership");
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
  const ApplyResult streamUpdate =
      updater.apply({DecodedMessageKind::ServerNotification,
                     "item/agentMessage/delta", std::nullopt,
                     Value::Object{{"threadId", Value("history-thread")},
                                   {"turnId", Value("long-turn")},
                                   {"itemId", Value("activity-81")},
                                   {"delta", Value("streamed")}}});
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "history-thread"});
    const Value *loaded = field(read->state(thread), "historyLoadedItemCount");
    require(read->changedRevision(thread) > threadRevision && loaded &&
                loaded->asUInt64() && *loaded->asUInt64() == 84 &&
                std::ranges::find(streamUpdate.change.affected, thread) ==
                    streamUpdate.change.affected.end(),
            "an existing-item stream advances its aggregate correlation "
            "revision without recomputing history metadata or scheduling a "
            "thread-row render");
  }

  Value::Array retainedSuffix{
      Value(Value::Object{{"id", Value("later-steering")},
                          {"type", Value("userMessage")}}),
      Value(Value::Object{{"id", Value("latest-activity")},
                          {"type", Value("agentMessage")}})};
  Value::Object suffixTurn{{"id", Value("long-turn")},
                           {"items", Value(std::move(retainedSuffix))}};
  Value::Object suffixThread{
      {"id", Value("history-thread")},
      {"turns", Value(Value::Array{Value(std::move(suffixTurn))})}};
  const ApplyResult suffixReplacement = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read",
       ProtocolRequestId("suffix-history"),
       Value::Object{{"thread", Value(std::move(suffixThread))}}});
  NodeRef retainedTurn;
  NodeRef steering;
  NodeRef latest;
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "history-thread"});
    retainedTurn = findTurn(*read, "history-thread", "long-turn");
    steering = findItem(*read, "history-thread", "long-turn", "later-steering");
    latest = findItem(*read, "history-thread", "long-turn", "latest-activity");
    const Value *loaded = field(read->state(thread), "historyLoadedItemCount");
    require(read->children(retainedTurn) ==
                    std::vector<NodeRef>{steering, latest} &&
                read->related(retainedTurn, RelationKind::TurnRootItem) ==
                    std::vector<NodeRef>{steering} &&
                !read->find(openingPrompt->id()) &&
                read->removed(openingPrompt) &&
                std::ranges::find(suffixReplacement.change.removed,
                                  openingPrompt) !=
                    suffixReplacement.change.removed.end(),
            "an authoritative suffix retires its omitted provider items and "
            "selects the first retained user message as the current root");
    require(loaded && loaded->asUInt64() && *loaded->asUInt64() == 2,
            "loaded history count includes only current authoritative items");
  }

  const ApplyResult removedHistory = updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("history-thread")}}});
  require(std::ranges::find(removedHistory.change.removed, retainedTurn) !=
                  removedHistory.change.removed.end() &&
              std::ranges::find(removedHistory.change.removed, steering) !=
                  removedHistory.change.removed.end() &&
              std::ranges::find(removedHistory.change.removed, latest) !=
                  removedHistory.change.removed.end(),
          "removing a hydrated thread retires all remaining current history");

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

void lateResultsCannotRecreateDeletedTargets() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const ProtocolRequestId requestId("late-rename");

  const ApplyResult request = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/name/set", requestId,
       Value::Object{{"threadId", Value("deleted-target")},
                     {"name", Value("stale name")}}});
  require(static_cast<bool>(request.primary),
          "an addressed rename request exposes its pending operation");
  {
    auto read = graph.tryRead();
    require(read->find({NodeKind::Thread, "deleted-target"}) &&
                read->related(request.primary, RelationKind::OperationTarget) ==
                    std::vector<NodeRef>{
                        read->find({NodeKind::Thread, "deleted-target"})},
            "an addressed rename records its original operation target");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("deleted-target")}}}));
  {
    auto read = graph.tryRead();
    require(
        !read->find({NodeKind::Thread, "deleted-target"}) &&
            read->find(request.primary->id()) == request.primary &&
            read->related(request.primary, RelationKind::OperationTarget)
                .empty(),
        "deleting the rename target unlinks it while retaining the in-flight "
        "operation for correlation");
  }

  const ApplyResult late =
      updater.apply({DecodedMessageKind::ClientResult, "thread/name/set",
                     requestId, Value::Object{}, request.primary});
  {
    auto read = graph.tryRead();
    require(!read->find({NodeKind::Thread, "deleted-target"}) &&
                !read->find(request.primary->id()) &&
                std::ranges::find(late.change.removed, request.primary) !=
                    late.change.removed.end(),
            "a late mutation result retires without recreating its deleted "
            "original target");
  }
}

void exactRequestTargetsOverridePayloadAddressingAndLifetime() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("exact-target")},
                                          {"name", Value("Original")}})}}}));

  NodeRef exactTarget;
  {
    auto read = graph.tryRead();
    exactTarget = read->find({NodeKind::Thread, "exact-target"});
  }
  const ProtocolRequestId requestId("exact-target-read");
  DecodedMessage request{
      DecodedMessageKind::ClientRequest, "thread/read", requestId,
      Value::Object{{"threadId", Value("exact-target")},
                    {"turnId", Value("payload-decoy-turn")},
                    {"itemId", Value("payload-decoy-item")}}};
  request.requestTarget = exactTarget;
  const ApplyResult admitted = updater.apply(std::move(request));
  {
    auto read = graph.tryRead();
    const NodeId decoyTurn =
        scopedTurnNodeId("exact-target", "payload-decoy-turn");
    require(
        admitted.primary && exactTarget &&
            read->related(admitted.primary, RelationKind::OperationTarget) ==
                std::vector<NodeRef>{exactTarget} &&
            !read->find(decoyTurn) &&
            !read->find(scopedItemNodeId(decoyTurn, "payload-decoy-item")),
        "a client request retains its exact action NodeRef instead of "
        "reconstructing a more-specific target from payload fields");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("exact-target")}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("exact-target")},
                                          {"name", Value("Replacement")}})}}}));
  const ApplyResult late = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read", requestId,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("exact-target")},
                                          {"name", Value("Late stale")},
                                          {"turns", Value(Value::Array{})}})}},
       admitted.primary});
  {
    auto read = graph.tryRead();
    const NodeRef replacement = read->find({NodeKind::Thread, "exact-target"});
    const Value *name =
        replacement ? field(read->state(replacement), "name") : nullptr;
    require(replacement && replacement != exactTarget &&
                read->removed(exactTarget) && name && name->asString() &&
                *name->asString() == "Replacement" &&
                !read->find(admitted.primary->id()) &&
                std::ranges::find(late.change.removed, admitted.primary) !=
                    late.change.removed.end(),
            "removing an exact target prevents its late response from "
            "mutating a replacement node with the same canonical id");
  }
}

void accountFacetsConvergeAndRateLimitPatchesStaySparse() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  const ProtocolRequestId accountReadId("account-read");
  const ApplyResult accountRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "account/read", accountReadId, {}});
  const Value::Object fullAccount{
      {"account", Value(Value::Object{{"type", Value("chatgpt")},
                                      {"email", Value("person@example.com")},
                                      {"planType", Value("plus")}})},
      {"requiresOpenaiAuth", Value(false)}};
  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientResult, "account/read",
                     accountReadId, fullAccount, accountRequest.primary}));

  NodeRef accountNode;
  {
    auto read = graph.tryRead();
    accountNode = read->find({NodeKind::Account, "account"});
    require(accountNode && !read->find({NodeKind::Account, "account/read"}),
            "account/read materializes the one current account facet");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "account/updated", std::nullopt,
       Value::Object{{"authMode", Value("chatgpt")},
                     {"planType", Value("team")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef current = read->find({NodeKind::Account, "account"});
    const auto state = current ? read->state(current) : nullptr;
    const Value *account = field(state, "account");
    const Value *email =
        objectField(account ? account->asObject() : nullptr, "email");
    const Value *requiresAuth = field(state, "requiresOpenaiAuth");
    const Value *authMode = field(state, "authMode");
    const Value *planType = field(state, "planType");
    require(current == accountNode &&
                !read->find({NodeKind::Account, "account/updated"}) && email &&
                email->asString() &&
                *email->asString() == "person@example.com" && requiresAuth &&
                requiresAuth->asBool() && !*requiresAuth->asBool() &&
                authMode && authMode->asString() &&
                *authMode->asString() == "chatgpt" && planType &&
                planType->asString() && *planType->asString() == "team",
            "account/updated sparsely augments the same account/read node");
  }

  const ProtocolRequestId rateReadId("rate-limits-read");
  const ApplyResult rateRequest =
      updater.apply({DecodedMessageKind::ClientRequest,
                     "account/rateLimits/read",
                     rateReadId,
                     {}});
  const Value::Object fullRateLimits{
      {"accountId", Value("account-1")},
      {"rateLimitUpsell",
       Value(Value::Object{{"message", Value("upgrade available")}})},
      {"rateLimitResetCredits",
       Value(Value::Object{{"availableCount", Value(2)}})},
      {"rateLimitsByLimitId",
       Value(Value::Object{
           {"secondary-limit",
            Value(Value::Object{{"limitName", Value("secondary")}})}})},
      {"rateLimits",
       Value(Value::Object{
           {"limitId", Value("old-limit")},
           {"limitName", Value("preserve")},
           {"planType", Value("plus")},
           {"primary", Value(Value::Object{{"usedPercent", Value(25)},
                                           {"resetsAt", Value(1000)},
                                           {"windowDurationMins", Value(300)},
                                           {"futurePrimary", Value("keep")}})},
           {"secondary", Value(Value::Object{{"usedPercent", Value(10)},
                                             {"resetsAt", Value(2000)}})},
           {"futureOld", Value(1)}})}};
  static_cast<void>(updater.apply({DecodedMessageKind::ClientResult,
                                   "account/rateLimits/read", rateReadId,
                                   fullRateLimits, rateRequest.primary}));

  NodeRef rateLimitsNode;
  {
    auto read = graph.tryRead();
    rateLimitsNode = read->find({NodeKind::Account, "rate-limits"});
    require(rateLimitsNode &&
                !read->find({NodeKind::Account, "account/rateLimits/read"}),
            "rate-limit reads materialize one current rate-limit facet");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "account/rateLimits/updated",
       std::nullopt,
       Value::Object{
           {"rateLimits",
            Value(Value::Object{
                {"limitId", Value("new-limit")},
                {"primary", Value(Value::Object{{"usedPercent", Value(40)}})},
                {"futureNew", Value(2)}})}}}));
  {
    auto read = graph.tryRead();
    const NodeRef current = read->find({NodeKind::Account, "rate-limits"});
    const auto state = current ? read->state(current) : nullptr;
    const Value *limitsValue = field(state, "rateLimits");
    const Value::Object *limits =
        limitsValue ? limitsValue->asObject() : nullptr;
    const Value *primaryValue = objectField(limits, "primary");
    const Value::Object *primary =
        primaryValue ? primaryValue->asObject() : nullptr;
    const Value *secondaryValue = objectField(limits, "secondary");
    const Value::Object *secondary =
        secondaryValue ? secondaryValue->asObject() : nullptr;
    const Value *limitId = objectField(limits, "limitId");
    const Value *limitName = objectField(limits, "limitName");
    const Value *planType = objectField(limits, "planType");
    const Value *usedPercent = objectField(primary, "usedPercent");
    const Value *resetsAt = objectField(primary, "resetsAt");
    const Value *duration = objectField(primary, "windowDurationMins");
    const Value *futurePrimary = objectField(primary, "futurePrimary");
    const Value *secondaryPercent = objectField(secondary, "usedPercent");
    require(
        current == rateLimitsNode &&
            !read->find({NodeKind::Account, "account/rateLimits/updated"}) &&
            limitId && limitId->asString() &&
            *limitId->asString() == "new-limit" && limitName &&
            limitName->asString() && *limitName->asString() == "preserve" &&
            planType && planType->asString() &&
            *planType->asString() == "plus" && usedPercent &&
            usedPercent->asInt64() && *usedPercent->asInt64() == 40 &&
            resetsAt && resetsAt->asInt64() && *resetsAt->asInt64() == 1000 &&
            duration && duration->asInt64() && *duration->asInt64() == 300 &&
            futurePrimary && futurePrimary->asString() &&
            *futurePrimary->asString() == "keep" && secondaryPercent &&
            secondaryPercent->asInt64() && *secondaryPercent->asInt64() == 10 &&
            objectField(limits, "futureOld") &&
            objectField(limits, "futureNew") &&
            field(state, "rateLimitUpsell") &&
            field(state, "rateLimitResetCredits") &&
            field(state, "rateLimitsByLimitId"),
        "a sparse rate-limit notification patches present nested fields "
        "without clearing full-read metadata");
  }

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "account/rateLimits/updated",
       std::nullopt,
       Value::Object{{"rateLimits",
                      Value(Value::Object{{"limitName", Value(nullptr)}})}}}));
  {
    auto read = graph.tryRead();
    const auto state = read->state(rateLimitsNode);
    const Value *limitsValue = field(state, "rateLimits");
    const Value::Object *limits =
        limitsValue ? limitsValue->asObject() : nullptr;
    const Value *limitName = objectField(limits, "limitName");
    const Value *primaryValue = objectField(limits, "primary");
    const Value::Object *primary =
        primaryValue ? primaryValue->asObject() : nullptr;
    const Value *usedPercent = objectField(primary, "usedPercent");
    require(limitName && limitName->isNull() && usedPercent &&
                usedPercent->asInt64() && *usedPercent->asInt64() == 40 &&
                field(state, "rateLimitResetCredits"),
            "an explicit null clears only its rate-limit field while omitted "
            "current fields remain intact");
  }

  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientResult, "account/usage/read",
                     std::nullopt, Value::Object{{"usage", Value(7)}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "account/workspaceMessages/read",
       std::nullopt, Value::Object{{"messages", Value(Value::Array{})}}}));
  {
    auto read = graph.tryRead();
    require(
        read->find({NodeKind::Account, "account"}) == accountNode &&
            read->find({NodeKind::Account, "rate-limits"}) == rateLimitsNode &&
            read->find({NodeKind::Account, "account/usage/read"}) &&
            read->find({NodeKind::Account, "account/workspaceMessages/read"}),
        "usage and workspace-message snapshots remain separate account "
        "facets");
  }

  const ProtocolRequestId logoutId("account-logout");
  const ApplyResult logoutRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "account/logout", logoutId, {}});
  static_cast<void>(updater.apply({DecodedMessageKind::ClientResult,
                                   "account/logout",
                                   logoutId,
                                   {},
                                   logoutRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef current = read->find({NodeKind::Account, "account"});
    const auto state = current ? read->state(current) : nullptr;
    require(
        current == accountNode &&
            !read->find({NodeKind::Account, "account/logout"}) &&
            field(state, "account") && field(state, "account")->isNull() &&
            field(state, "authMode") && field(state, "authMode")->isNull() &&
            field(state, "planType") && field(state, "planType")->isNull() &&
            field(state, "requiresOpenaiAuth") &&
            field(state, "requiresOpenaiAuth")->asBool() &&
            !*field(state, "requiresOpenaiAuth")->asBool() &&
            field(state, "lastMethod") &&
            field(state, "lastMethod")->asString() &&
            *field(state, "lastMethod")->asString() == "account/logout",
        "logout clears the canonical account facet while preserving the "
        "provider authentication requirement");
  }
}

void successfulRefreshesRetireInvalidationsAndConfigWritesInvalidate() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                   "skills/changed",
                                   std::nullopt,
                                   {}}));
  static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                   "app/list/updated",
                                   std::nullopt,
                                   {}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "skills/list", std::nullopt,
       Value::Object{{"data", Value(Value::Array{Value("skill")})}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "app/list", std::nullopt,
       Value::Object{{"data", Value(Value::Array{Value("app")})}}}));

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "project/changed", std::nullopt,
       Value::Object{{"projectId", Value("project-refresh")}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "project/read", std::nullopt,
       Value::Object{
           {"project", Value(Value::Object{{"id", Value("project-refresh")},
                                           {"name", Value("Current")}})}}}));

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/queue/changed",
       std::nullopt, Value::Object{{"threadId", Value("refresh-thread")}}}));
  const ProtocolRequestId queueId("queue-refresh");
  const ApplyResult queueRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/queue/list", queueId,
       Value::Object{{"threadId", Value("refresh-thread")}}});
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/queue/list", queueId,
       Value::Object{{"data", Value(Value::Array{Value("queued")})}},
       queueRequest.primary}));

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/reverted", std::nullopt,
       Value::Object{{"threadId", Value("refresh-thread")}}}));
  const ProtocolRequestId readId("history-refresh");
  const ApplyResult readRequest =
      updater.apply({DecodedMessageKind::ClientRequest, "thread/read", readId,
                     Value::Object{{"threadId", Value("refresh-thread")},
                                   {"includeTurns", Value(true)}}});
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read", readId,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("refresh-thread")},
                                          {"turns", Value(Value::Array{})}})}},
       readRequest.primary}));

  {
    auto read = graph.tryRead();
    const auto skills = read->state(read->find({NodeKind::Catalog, "skills"}));
    const auto apps = read->state(read->find({NodeKind::Catalog, "app"}));
    const auto project =
        read->state(read->find({NodeKind::Project, "project-refresh"}));
    const auto thread =
        read->state(read->find({NodeKind::Thread, "refresh-thread"}));
    require(!field(skills, "stale") && !field(skills, "invalidatedBy") &&
                !field(apps, "stale") && !field(apps, "invalidatedBy") &&
                !field(project, "stale") && !field(thread, "queueStale") &&
                !field(thread, "historyStale"),
            "successful snapshots retire their matching invalidation facts");
  }

  const Value::Object firstConfig{
      {"config", Value(Value::Object{{"model", Value("old-model")}})},
      {"origins", Value(Value::Object{})}};
  static_cast<void>(updater.apply({DecodedMessageKind::ClientResult,
                                   "config/read", std::nullopt, firstConfig}));
  NodeRef configuration;
  {
    auto read = graph.tryRead();
    configuration = read->find({NodeKind::Configuration, "config/read"});
  }

  const ProtocolRequestId writeId("config-write");
  const ApplyResult writeRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "config/value/write", writeId,
       Value::Object{{"keyPath", Value("model")},
                     {"value", Value("new-model")}}});
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "config/value/write", writeId,
       Value::Object{{"status", Value("ok")}, {"version", Value("2")}},
       writeRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef current =
        read->find({NodeKind::Configuration, "config/read"});
    const auto state = current ? read->state(current) : nullptr;
    require(current == configuration && field(state, "stale") &&
                field(state, "stale")->asBool() &&
                *field(state, "stale")->asBool() &&
                field(state, "invalidatedBy") &&
                field(state, "invalidatedBy")->asString() &&
                *field(state, "invalidatedBy")->asString() ==
                    "config/value/write" &&
                !read->find({NodeKind::Configuration, "config/value/write"}),
            "config writes invalidate the canonical effective snapshot");
  }

  const Value::Object refreshedConfig{
      {"config", Value(Value::Object{{"model", Value("new-model")}})},
      {"origins", Value(Value::Object{})}};
  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientResult, "config/read",
                     std::nullopt, refreshedConfig}));
  {
    auto read = graph.tryRead();
    const NodeRef current =
        read->find({NodeKind::Configuration, "config/read"});
    const auto state = current ? read->state(current) : nullptr;
    const Value *configValue = field(state, "config");
    const Value *model =
        objectField(configValue ? configValue->asObject() : nullptr, "model");
    require(current == configuration && !field(state, "stale") &&
                !field(state, "invalidatedBy") && model && model->asString() &&
                *model->asString() == "new-model",
            "a fresh config read replaces and validates the same singleton");
  }

  const ProtocolRequestId batchId("config-batch-write");
  const ApplyResult batchRequest =
      updater.apply({DecodedMessageKind::ClientRequest, "config/batchWrite",
                     batchId, Value::Object{{"edits", Value(Value::Array{})}}});
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "config/batchWrite", batchId,
       Value::Object{{"status", Value("ok")}, {"version", Value("3")}},
       batchRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef current =
        read->find({NodeKind::Configuration, "config/read"});
    const auto state = current ? read->state(current) : nullptr;
    require(
        current == configuration && field(state, "stale") &&
            field(state, "stale")->asBool() &&
            *field(state, "stale")->asBool() && field(state, "invalidatedBy") &&
            field(state, "invalidatedBy")->asString() &&
            *field(state, "invalidatedBy")->asString() == "config/batchWrite" &&
            !read->find({NodeKind::Configuration, "config/batchWrite"}),
        "batch config writes invalidate that same effective snapshot");
  }
}

void catalogResultsMaterializeNaturalEntityKinds() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const auto result = [&](std::string method, Value::Object payload) {
    return updater.apply({DecodedMessageKind::ClientResult, std::move(method),
                          std::nullopt, std::move(payload)});
  };

  static_cast<void>(result(
      "model/list",
      {{"data", Value(Value::Array{
                    Value(Value::Object{{"id", Value("model-a")},
                                        {"displayName", Value("Model A")}}),
                    Value(Value::Object{{"id", Value("model-b")},
                                        {"displayName", Value("Model B")}})})},
       {"nextCursor", Value("models-next")}}));
  static_cast<void>(
      result("permissionProfile/list",
             {{"data", Value(Value::Array{Value(Value::Object{
                           {"id", Value("trusted")},
                           {"description", Value("Trusted profile")},
                           {"allowed", Value(true)}})})}}));
  static_cast<void>(
      result("skills/list",
             {{"data", Value(Value::Array{Value(Value::Object{
                           {"cwd", Value("/workspace")},
                           {"skills", Value(Value::Array{Value(Value::Object{
                                          {"name", Value("review")},
                                          {"path", Value("/workspace/review")},
                                          {"enabled", Value(true)}})})}})})}}));
  static_cast<void>(
      result("hooks/list",
             {{"data", Value(Value::Array{Value(Value::Object{
                           {"cwd", Value("/workspace")},
                           {"hooks", Value(Value::Array{Value(Value::Object{
                                         {"key", Value("after-turn")},
                                         {"eventName", Value("afterAgent")},
                                         {"enabled", Value(true)}})})}})})}}));
  static_cast<void>(
      result("plugin/list",
             {{"marketplaces",
               Value(Value::Array{Value(Value::Object{
                   {"name", Value("local")},
                   {"plugins", Value(Value::Array{Value(Value::Object{
                                   {"id", Value("plugin-a")},
                                   {"name", Value("Plugin A")}})})}})})}}));
  static_cast<void>(result(
      "app/list",
      {{"data", Value(Value::Array{Value(Value::Object{
                    {"id", Value("app-a")}, {"name", Value("App A")}})})}}));
  static_cast<void>(result("mcpServerStatus/list",
                           {{"data", Value(Value::Array{Value(Value::Object{
                                         {"name", Value("server-a")},
                                         {"status", Value("connected")}})})}}));

  NodeRef retainedModel;
  {
    auto read = graph.tryRead();
    const NodeRef models = read->find({NodeKind::Catalog, "model"});
    const NodeRef profiles =
        read->find({NodeKind::Catalog, "permissionProfile"});
    const NodeRef skills = read->find({NodeKind::Catalog, "skills"});
    const NodeRef hooks = read->find({NodeKind::Catalog, "hooks"});
    const NodeRef plugins = read->find({NodeKind::Catalog, "plugin"});
    const NodeRef apps = read->find({NodeKind::Catalog, "app"});
    const NodeRef servers = read->find({NodeKind::Catalog, "mcpServer"});
    const auto modelChildren = read->children(models);
    retainedModel = modelChildren.size() == 2 ? modelChildren[1] : NodeRef{};
    require(models && profiles && skills && hooks && plugins && apps &&
                servers && modelChildren.size() == 2 &&
                modelChildren[0]->id().kind == NodeKind::CatalogEntry &&
                read->children(profiles).size() == 1 &&
                read->children(profiles)[0]->id().kind ==
                    NodeKind::PermissionProfile &&
                read->children(skills).size() == 1 &&
                read->children(skills)[0]->id().kind == NodeKind::Skill &&
                read->children(hooks).size() == 1 &&
                read->children(hooks)[0]->id().kind == NodeKind::Hook &&
                read->children(plugins).size() == 1 &&
                read->children(plugins)[0]->id().kind == NodeKind::Plugin &&
                read->children(apps).size() == 1 &&
                read->children(apps)[0]->id().kind == NodeKind::App &&
                read->children(servers).size() == 1 &&
                read->children(servers)[0]->id().kind == NodeKind::McpServer &&
                field(read->state(models), "nextCursor") &&
                field(read->state(read->children(skills)[0]), "catalogScope"),
            "catalog envelopes retain paging facts while concrete ordered "
            "children use every declared natural entity kind");
  }

  static_cast<void>(result(
      "model/list",
      {{"data",
        Value(Value::Array{
            Value(Value::Object{{"id", Value("model-b")},
                                {"displayName", Value("Model B2")}}),
            Value(Value::Object{{"id", Value("model-c")},
                                {"displayName", Value("Model C")}})})}}));
  {
    auto read = graph.tryRead();
    const NodeRef models = read->find({NodeKind::Catalog, "model"});
    const auto children = read->children(models);
    const auto retainedState =
        retainedModel ? read->state(retainedModel) : nullptr;
    require(children.size() == 2,
            "authoritative catalog replacement has two current rows");
    require(!children.empty() && children[0] == retainedModel,
            "authoritative catalog replacement preserves stable retained "
            "NodeRefs and first-to-last provider order");
    require(field(retainedState, "displayName") &&
                field(retainedState, "displayName")->asString() &&
                *field(retainedState, "displayName")->asString() == "Model B2",
            "authoritative catalog replacement refreshes retained row state");
    require(!read->find({NodeKind::CatalogEntry, "scope:5:model:7:model-a"}),
            "authoritative catalog replacement retires omitted entity rows");
  }
}

void specializedNotificationFamiliesKeepCurrentSemantics() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const auto notify = [&](std::string method, Value::Object payload) {
    return updater.apply({DecodedMessageKind::ServerNotification,
                          std::move(method), std::nullopt, std::move(payload)});
  };

  static_cast<void>(notify(
      "item/started",
      {{"threadId", Value("semantic-notifications")},
       {"turnId", Value("turn-1")},
       {"item", Value(Value::Object{{"id", Value("target-1")},
                                    {"type", Value("commandExecution")}})}}));
  static_cast<void>(
      notify("item/autoApprovalReview/started",
             {{"threadId", Value("semantic-notifications")},
              {"turnId", Value("turn-1")},
              {"targetItemId", Value("target-1")},
              {"reviewId", Value("review-1")},
              {"action", Value(Value::Object{{"type", Value("command")}})},
              {"review", Value(Value::Object{{"status", Value("pending")}})}}));

  NodeRef review;
  NodeRef target;
  {
    auto read = graph.tryRead();
    const NodeRef turn = findTurn(*read, "semantic-notifications", "turn-1");
    review = findItem(*read, "semantic-notifications", "turn-1", "review-1");
    target = findItem(*read, "semantic-notifications", "turn-1", "target-1");
    const auto state = review ? read->state(review) : nullptr;
    require(turn && review && target && state &&
                state->status == NodeStatus::Running && field(state, "type") &&
                field(state, "type")->asString() &&
                *field(state, "type")->asString() == "autoApprovalReview" &&
                read->related(review, RelationKind::ReviewTarget) ==
                    std::vector<NodeRef>{target},
            "auto-review start creates one concrete review item related to "
            "its exact target item");
  }
  static_cast<void>(notify(
      "item/autoApprovalReview/completed",
      {{"threadId", Value("semantic-notifications")},
       {"turnId", Value("turn-1")},
       {"targetItemId", Value("target-1")},
       {"reviewId", Value("review-1")},
       {"decisionSource", Value("guardian")},
       {"review", Value(Value::Object{{"status", Value("approved")}})}}));
  {
    auto read = graph.tryRead();
    const NodeRef current =
        findItem(*read, "semantic-notifications", "turn-1", "review-1");
    const auto state = current ? read->state(current) : nullptr;
    require(current == review && state &&
                state->status == NodeStatus::Completed &&
                field(state, "decisionSource") &&
                read->related(current, RelationKind::ReviewTarget) ==
                    std::vector<NodeRef>{target},
            "auto-review completion updates the stable review and preserves "
            "its target relation");
  }

  static_cast<void>(notify("item/reasoning/summaryPartAdded",
                           {{"threadId", Value("semantic-notifications")},
                            {"turnId", Value("turn-1")},
                            {"itemId", Value("reasoning-1")},
                            {"summaryIndex", Value(2)}}));
  static_cast<void>(notify("item/reasoning/summaryTextDelta",
                           {{"threadId", Value("semantic-notifications")},
                            {"turnId", Value("turn-1")},
                            {"itemId", Value("reasoning-1")},
                            {"summaryIndex", Value(2)},
                            {"delta", Value("third part")}}));
  {
    auto read = graph.tryRead();
    const NodeRef reasoning =
        findItem(*read, "semantic-notifications", "turn-1", "reasoning-1");
    const auto state = reasoning ? read->state(reasoning) : nullptr;
    const Value *summary = field(state, "summary");
    const Value::Array *parts = summary ? summary->asArray() : nullptr;
    require(parts && parts->size() == 3 && (*parts)[2].asString() &&
                *(*parts)[2].asString() == "third part",
            "summary part boundaries materialize their indexed empty slot "
            "before later text deltas append");
  }

  static_cast<void>(notify("autoApprovalReview/strictReviewRequired",
                           {{"threadId", Value("semantic-notifications")},
                            {"turnId", Value("turn-1")},
                            {"startedAtMs", Value(15)}}));
  static_cast<void>(notify("thread/environment/connected",
                           {{"threadId", Value("semantic-notifications")},
                            {"environmentId", Value("environment-1")}}));
  static_cast<void>(notify("thread/environment/disconnected",
                           {{"threadId", Value("semantic-notifications")},
                            {"environmentId", Value("environment-1")},
                            {"reason", Value("closed")}}));
  static_cast<void>(
      notify("thread/compacted", {{"threadId", Value("semantic-notifications")},
                                  {"turnId", Value("turn-1")}}));
  static_cast<void>(
      notify("modelProvider/authRecoveryStarted",
             {{"provider", Value("openai")}, {"attempt", Value(2)}}));
  static_cast<void>(
      notify("modelProvider/authRecoveryCompleted",
             {{"provider", Value("openai")}, {"recovered", Value(true)}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread =
        read->find({NodeKind::Thread, "semantic-notifications"});
    const NodeRef turn = findTurn(*read, "semantic-notifications", "turn-1");
    const NodeRef provider = read->find({NodeKind::Catalog, "modelProvider"});
    const auto threadState = thread ? read->state(thread) : nullptr;
    const auto turnState = turn ? read->state(turn) : nullptr;
    const auto providerState = provider ? read->state(provider) : nullptr;
    require(threadState && turnState && providerState &&
                field(turnState, "strictReviewRequired") &&
                field(turnState, "strictReviewRequired")->asBool() &&
                *field(turnState, "strictReviewRequired")->asBool() &&
                field(threadState, "environmentConnected") &&
                field(threadState, "environmentConnected")->asBool() &&
                !*field(threadState, "environmentConnected")->asBool() &&
                field(threadState, "environmentStatus") &&
                field(threadState, "compacted") &&
                field(threadState, "lastCompactedTurnId") &&
                field(providerState, "authRecoveryActive") &&
                field(providerState, "authRecoveryActive")->asBool() &&
                !*field(providerState, "authRecoveryActive")->asBool() &&
                field(providerState, "recovered") &&
                providerState->status == NodeStatus::Completed,
            "review escalation, environment, compaction, and provider-auth "
            "families retain explicit current lifecycle facts");
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

void keyedRuntimeNotificationsKeepIndependentCurrentState() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  const auto importResults = [](std::string_view itemType) {
    return Value::Array{
        Value(Value::Object{{"itemType", Value(itemType)},
                            {"successes", Value(Value::Array{})},
                            {"failures", Value(Value::Array{})}})};
  };
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification,
       "externalAgentConfig/import/progress", std::nullopt,
       Value::Object{{"importId", Value("import-a")},
                     {"itemTypeResults", Value(importResults("SKILLS"))}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification,
       "externalAgentConfig/import/progress", std::nullopt,
       Value::Object{{"importId", Value("import-b")},
                     {"itemTypeResults", Value(importResults("CONFIG"))}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification,
       "externalAgentConfig/import/completed", std::nullopt,
       Value::Object{{"importId", Value("import-a")},
                     {"itemTypeResults", Value(importResults("PLUGINS"))}}}));

  const auto fuzzyFiles = [](std::string_view path) {
    return Value::Array{
        Value(Value::Object{{"file_name", Value(path)},
                            {"match_type", Value("file")},
                            {"path", Value(path)},
                            {"root", Value("/workspace")},
                            {"score", Value(std::uint64_t{1})}})};
  };
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "fuzzyFileSearch/sessionUpdated",
       std::nullopt,
       Value::Object{{"sessionId", Value("search-a")},
                     {"query", Value("alpha")},
                     {"files", Value(fuzzyFiles("alpha.cpp"))}}}));
  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerNotification,
                     "fuzzyFileSearch/sessionUpdated", std::nullopt,
                     Value::Object{{"sessionId", Value("search-b")},
                                   {"query", Value("beta")},
                                   {"files", Value(fuzzyFiles("beta.cpp"))}}}));
  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerNotification,
                     "fuzzyFileSearch/sessionCompleted", std::nullopt,
                     Value::Object{{"sessionId", Value("search-a")}}}));

  static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                   "account/login/completed", std::nullopt,
                                   Value::Object{{"loginId", Value("login-a")},
                                                 {"success", Value(true)},
                                                 {"error", Value(nullptr)}}}));
  static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                   "account/login/completed", std::nullopt,
                                   Value::Object{{"loginId", Value("login-b")},
                                                 {"success", Value(false)},
                                                 {"error", Value("denied")}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "account/login/completed",
       std::nullopt,
       Value::Object{{"loginId", Value(nullptr)},
                     {"success", Value(true)},
                     {"error", Value(nullptr)},
                     {"onboardingEntrypoint", Value("life_sciences")}}}));

  auto read = graph.tryRead();
  const NodeRef importA =
      read->find({NodeKind::ExternalAgentImport, "import-a"});
  const NodeRef importB =
      read->find({NodeKind::ExternalAgentImport, "import-b"});
  const auto importAState = importA ? read->state(importA) : nullptr;
  const auto importBState = importB ? read->state(importB) : nullptr;
  const Value *importAResults = field(importAState, "itemTypeResults");
  const Value *importBResults = field(importBState, "itemTypeResults");
  const auto firstItemType = [](const Value *results) -> std::string {
    const Value::Array *items = results ? results->asArray() : nullptr;
    const Value::Object *item =
        items && !items->empty() ? items->front().asObject() : nullptr;
    const auto found =
        item ? item->find("itemType") : Value::Object::const_iterator{};
    const Value *type = item && found != item->end() ? &found->second : nullptr;
    return type && type->asString() ? *type->asString() : std::string{};
  };
  require(importA && importB && importA != importB && importAState &&
              importBState && importAState->status == NodeStatus::Completed &&
              importBState->status == NodeStatus::Running &&
              firstItemType(importAResults) == "PLUGINS" &&
              firstItemType(importBResults) == "CONFIG",
          "concurrent external-agent imports remain distinct by importId and "
          "completion updates only the addressed import");

  const NodeRef searchA =
      findProtocolNode(*read, NodeKind::FuzzyFileSearchSession, "search-a", 0);
  const NodeRef searchB =
      findProtocolNode(*read, NodeKind::FuzzyFileSearchSession, "search-b", 0);
  const auto searchAState = searchA ? read->state(searchA) : nullptr;
  const auto searchBState = searchB ? read->state(searchB) : nullptr;
  const Value *searchAQuery = field(searchAState, "query");
  const Value *searchBQuery = field(searchBState, "query");
  const Value *searchAFiles = field(searchAState, "files");
  require(searchA && searchB && searchA != searchB && searchAState &&
              searchBState && searchAState->status == NodeStatus::Completed &&
              searchBState->status == NodeStatus::Running && searchAQuery &&
              searchAQuery->asString() &&
              *searchAQuery->asString() == "alpha" && searchBQuery &&
              searchBQuery->asString() && *searchBQuery->asString() == "beta" &&
              searchAFiles && searchAFiles->asArray() &&
              searchAFiles->asArray()->size() == 1,
          "fuzzy-search completion addresses one connection-scoped session "
          "and retains its prior query and result snapshot");

  NodeRef loginA;
  NodeRef loginB;
  NodeRef nullLogin;
  std::size_t loginCount = 0;
  for (const NodeRef &node : read->orderedNodes()) {
    if (node->id().kind != NodeKind::LoginAttempt)
      continue;
    ++loginCount;
    const auto state = read->state(node);
    const Value *protocolId = field(state, "protocolId");
    if (protocolId && protocolId->asString() &&
        *protocolId->asString() == "login-a")
      loginA = node;
    else if (protocolId && protocolId->asString() &&
             *protocolId->asString() == "login-b")
      loginB = node;
    else if (protocolId && protocolId->isNull())
      nullLogin = node;
  }
  const auto loginAState = loginA ? read->state(loginA) : nullptr;
  const auto loginBState = loginB ? read->state(loginB) : nullptr;
  const auto nullLoginState = nullLogin ? read->state(nullLogin) : nullptr;
  const Value *nullProtocolId = field(nullLoginState, "protocolId");
  require(loginCount == 3 && loginA && loginB && nullLogin &&
              loginA != loginB && loginA != nullLogin && loginB != nullLogin &&
              loginAState->status == NodeStatus::Completed &&
              loginBState->status == NodeStatus::Failed &&
              nullLoginState->status == NodeStatus::Completed &&
              nullProtocolId && nullProtocolId->isNull(),
          "login completions retain independent valued loginId attempts and "
          "an explicit nullable-loginId attempt without collisions");
}

void turnErrorsUpdateAddressedStateWithoutLosingTheNotice() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const Value::Object retryError{
      {"message", Value("provider disconnected")},
      {"additionalDetails", Value("retrying shortly")},
      {"codexErrorInfo", Value("responseStreamDisconnected")}};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "error", std::nullopt,
       Value::Object{{"threadId", Value("error-thread")},
                     {"turnId", Value("error-turn")},
                     {"error", Value(retryError)},
                     {"willRetry", Value(true)}}}));

  {
    auto read = graph.tryRead();
    const NodeRef notice = read->find({NodeKind::Notice, "provider-notice"});
    const NodeRef thread = read->find({NodeKind::Thread, "error-thread"});
    const NodeRef turn = findTurn(*read, "error-thread", "error-turn");
    const auto noticeState = notice ? read->state(notice) : nullptr;
    const auto threadState = thread ? read->state(thread) : nullptr;
    const auto turnState = turn ? read->state(turn) : nullptr;
    const Value *threadError = field(threadState, "error");
    const Value *turnError = field(turnState, "error");
    const Value *threadRetry = field(threadState, "willRetry");
    const Value *turnRetry = field(turnState, "willRetry");
    require(notice && noticeState &&
                noticeState->status == NodeStatus::Failed && thread && turn &&
                read->parent(turn) == thread && threadError && turnError &&
                *threadError == Value(retryError) &&
                *turnError == Value(retryError) && threadRetry &&
                threadRetry->asBool() && *threadRetry->asBool() && turnRetry &&
                turnRetry->asBool() && *turnRetry->asBool(),
            "error remains a provider notice while atomically updating the "
            "addressed thread and scoped turn error/retry facts");
  }

  const Value::Object finalError{{"message", Value("retry exhausted")}};
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "error", std::nullopt,
       Value::Object{{"threadId", Value("error-thread")},
                     {"turnId", Value("error-turn")},
                     {"error", Value(finalError)},
                     {"willRetry", Value(false)}}}));
  auto read = graph.tryRead();
  const auto threadState =
      read->state(read->find({NodeKind::Thread, "error-thread"}));
  const auto turnState =
      read->state(findTurn(*read, "error-thread", "error-turn"));
  const Value *threadRetry = field(threadState, "willRetry");
  const Value *turnRetry = field(turnState, "willRetry");
  require(field(threadState, "error") &&
              *field(threadState, "error") == Value(finalError) &&
              field(turnState, "error") &&
              *field(turnState, "error") == Value(finalError) && threadRetry &&
              threadRetry->asBool() && !*threadRetry->asBool() && turnRetry &&
              turnRetry->asBool() && !*turnRetry->asBool(),
          "a later terminal turn error replaces current error state and "
          "clears willRetry on exactly the same thread and turn nodes");
}

void mcpServerNotificationsKeepCanonicalScope() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const auto apply = [&](std::string method, Value::Object payload) {
    static_cast<void>(
        updater.apply({DecodedMessageKind::ServerNotification,
                       std::move(method), std::nullopt, std::move(payload)}));
  };

  apply("mcpServer/startupStatus/updated",
        {{"name", Value("server-a")}, {"status", Value("ready")}});
  apply("mcpServer/startupStatus/updated",
        {{"name", Value("server-b")}, {"status", Value("failed")}});
  apply("mcpServer/oauthLogin/completed",
        {{"name", Value("server-a")}, {"success", Value(true)}});
  apply("mcpServer/startupStatus/updated", {{"threadId", Value("thread-one")},
                                            {"name", Value("server-a")},
                                            {"status", Value("starting")}});
  apply("mcpServer/event/stream/notification",
        {{"subscriptionId", Value("subscription-one")},
         {"notification", Value(Value::Object{{"method", Value("first")}})}});
  apply("mcpServer/event/stream/notification",
        {{"subscriptionId", Value("subscription-two")},
         {"notification", Value(Value::Object{{"method", Value("second")}})}});

  auto read = graph.tryRead();
  std::vector<NodeRef> servers;
  for (const NodeRef &node : read->orderedNodes())
    if (node->id().kind == NodeKind::McpServer)
      servers.push_back(node);

  const auto find = [&](std::string_view protocolId,
                        std::string_view threadId = {}) {
    return std::ranges::find_if(servers, [&](const NodeRef &node) {
      const auto state = read->state(node);
      const Value *thread = field(state, "threadId");
      const std::string actualThread =
          thread && thread->asString() ? *thread->asString() : std::string{};
      return protocolCanonicalId(*state, node) == protocolId &&
             actualThread == threadId;
    });
  };
  const auto globalA = find("server-a");
  const auto globalB = find("server-b");
  const auto threadA = find("server-a", "thread-one");
  const auto subscriptionOne = find("subscription-one");
  const auto subscriptionTwo = find("subscription-two");
  require(servers.size() == 5 && globalA != servers.end() &&
              globalB != servers.end() && threadA != servers.end() &&
              subscriptionOne != servers.end() &&
              subscriptionTwo != servers.end(),
          "MCP server and subscription notifications retain distinct "
          "canonical scopes");
  if (globalA != servers.end()) {
    const auto state = read->state(*globalA);
    require(field(state, "success") && field(state, "success")->asBool() &&
                *field(state, "success")->asBool(),
            "OAuth completion updates the matching global MCP server");
  }
  if (globalB != servers.end()) {
    const auto state = read->state(*globalB);
    require(field(state, "status") && field(state, "status")->asString() &&
                *field(state, "status")->asString() == "failed",
            "one MCP server update cannot overwrite another server");
  }
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
    require(
        read->related(thread, RelationKind::PendingInteraction) ==
                std::vector<NodeRef>{interaction} &&
            field(read->state(thread), "pendingInteractionCount") &&
            field(read->state(thread), "pendingInteractionCount")->asUInt64() &&
            *field(read->state(thread), "pendingInteractionCount")
                    ->asUInt64() == 1,
        "the addressed thread directly indexes and counts its pending "
        "interaction");
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
    require(
        thread &&
            read->related(thread, RelationKind::PendingInteraction) ==
                std::vector<NodeRef>{interaction} &&
            field(read->state(thread), "pendingInteractionCount") &&
            field(read->state(thread), "pendingInteractionCount")->asUInt64() &&
            *field(read->state(thread), "pendingInteractionCount")
                    ->asUInt64() == 1,
        "a rejected response remains counted as unresolved thread attention");
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
            read->related(runtime, RelationKind::PendingInteraction).empty() &&
            field(read->state(thread), "pendingInteractionCount") &&
            field(read->state(thread), "pendingInteractionCount")->asUInt64() &&
            *field(read->state(thread), "pendingInteractionCount")
                    ->asUInt64() == 0,
        "interaction removal unlinks both direct pending indexes and keeps "
        "the derived count current");
  }

  const ProtocolRequestId externallyResolvedId("approval-externally-resolved");
  static_cast<void>(
      updater.apply({DecodedMessageKind::ServerRequest,
                     "item/fileChange/requestApproval", externallyResolvedId,
                     Value::Object{{"threadId", Value("thread-approval")}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "serverRequest/resolved",
       std::nullopt,
       Value::Object{{"requestId", Value("approval-externally-resolved")}}}));
  {
    auto read = graph.tryRead();
    const NodeRef thread = read->find({NodeKind::Thread, "thread-approval"});
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    const Value *count = field(read->state(thread), "pendingInteractionCount");
    require(
        thread && runtime && count && count->asUInt64() &&
            *count->asUInt64() == 0 &&
            read->related(thread, RelationKind::PendingInteraction).empty() &&
            read->related(runtime, RelationKind::PendingInteraction).empty(),
        "provider-side request resolution clears the same indexes and "
        "cached thread attention count");
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

void largeThreadDeletionIsNearLinear() {
  const auto measure = [](std::size_t itemCount) {
    NodeGraph graph;
    ProtocolUpdater updater(graph);
    {
      auto write = graph.write();
      NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
      NodeRef thread = write.upsert({NodeKind::Thread, "bulk-delete-thread"});
      NodeRef turn = write.upsert(
          scopedTurnNodeId("bulk-delete-thread", "bulk-delete-turn"));
      write.setParent(thread, turn);
      write.relate(runtime, RelationKind::RootThread, thread);
      for (std::size_t index = 0; index < itemCount; ++index) {
        NodeState state;
        state.fields = {{"type", Value("agentMessage")},
                        {"protocolId", Value(std::to_string(index))}};
        NodeRef item =
            write.upsert(scopedItemNodeId(turn->id(), std::to_string(index)),
                         std::move(state));
        write.setParent(turn, item);
      }
      static_cast<void>(write.finish());
    }

    const auto started = std::chrono::steady_clock::now();
    const ApplyResult result = updater.apply(
        {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
         Value::Object{{"threadId", Value("bulk-delete-thread")}}});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    auto read = graph.tryRead();
    const bool valid = result.change.removed.size() == itemCount + 2 && read &&
                       !read->find({NodeKind::Thread, "bulk-delete-thread"}) &&
                       read->retiredCount() == itemCount + 2;
    return std::pair{valid, elapsed};
  };

  const auto [smallValid, smallElapsed] = measure(1500);
  const auto [largeValid, largeElapsed] = measure(3000);
  require(smallValid && largeValid &&
              largeElapsed <= smallElapsed * 3 + std::chrono::milliseconds(25),
          "large canonical thread deletion scales approximately linearly");
  std::cout
      << "thread-delete ns (1500 / 3000 items): "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(smallElapsed)
             .count()
      << " / "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(largeElapsed)
             .count()
      << '\n';
}

void correlatedThreadReadsPreserveOnlyInterveningLiveState() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("merging-read-thread")},
                                          {"cwd", Value("/old/cwd")}})}}}));

  const ProtocolRequestId mergingReadId("merging-thread-read");
  const ApplyResult mergingRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/read", mergingReadId,
       Value::Object{{"threadId", Value("merging-read-thread")}}});
  require(static_cast<bool>(mergingRequest.primary),
          "correlated merging read exposes its exact operation");

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "turn/started", std::nullopt,
       Value::Object{
           {"threadId", Value("merging-read-thread")},
           {"turn", Value(Value::Object{{"id", Value("live-turn")},
                                        {"status", Value("inProgress")}})}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/plan/delta", std::nullopt,
       Value::Object{{"threadId", Value("merging-read-thread")},
                     {"turnId", Value("live-turn")},
                     {"itemId", Value("live-plan")},
                     {"delta", Value("live plan text")}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/name/updated",
       std::nullopt,
       Value::Object{{"threadId", Value("merging-read-thread")},
                     {"threadName", Value("Live thread name")}}}));
  Value::Array snapshotItems{Value(Value::Object{
      {"id", Value("snapshot-item")}, {"type", Value("agentMessage")}})};
  Value::Object snapshotTurn{{"id", Value("snapshot-turn")},
                             {"items", Value(std::move(snapshotItems))}};
  Value::Array staleLiveItems{
      Value(Value::Object{{"id", Value("live-plan")},
                          {"type", Value("plan")},
                          {"text", Value("stale snapshot text")}})};
  Value::Object staleLiveTurn{{"id", Value("live-turn")},
                              {"status", Value("completed")},
                              {"items", Value(std::move(staleLiveItems))}};
  Value::Array snapshotTurns{Value(std::move(snapshotTurn)),
                             Value(std::move(staleLiveTurn))};
  Value::Object mergingThread{{"id", Value("merging-read-thread")},
                              {"name", Value("Stale snapshot name")},
                              {"cwd", Value("/authoritative/cwd")},
                              {"turns", Value(std::move(snapshotTurns))}};
  DecodedMessage mergingResult{
      DecodedMessageKind::ClientResult, "thread/read", mergingReadId,
      Value::Object{{"thread", Value(std::move(mergingThread))}},
      mergingRequest.primary};
  const ApplyResult merged = updater.apply(std::move(mergingResult));
  {
    auto read = graph.tryRead();
    const NodeRef thread =
        read->find({NodeKind::Thread, "merging-read-thread"});
    const NodeRef liveTurn =
        findTurn(*read, "merging-read-thread", "live-turn");
    const NodeRef livePlan =
        findItem(*read, "merging-read-thread", "live-turn", "live-plan");
    const NodeRef snapshotTurn =
        findTurn(*read, "merging-read-thread", "snapshot-turn");
    const Value *text = field(read->state(livePlan), "text");
    const Value *name = field(read->state(thread), "name");
    const Value *cwd = field(read->state(thread), "cwd");
    const Value *type = field(read->state(livePlan), "type");
    require(!read->find(mergingRequest.primary->id()) && thread && liveTurn &&
                livePlan && snapshotTurn &&
                protocolIds(*read, read->children(thread)) ==
                    std::vector<std::string>{"snapshot-turn", "live-turn"} &&
                read->children(liveTurn) == std::vector<NodeRef>{livePlan} &&
                text && text->asString() &&
                *text->asString() == "live plan text" && type &&
                type->asString() && *type->asString() == "plan" &&
                read->state(liveTurn)->status == NodeStatus::Running && name &&
                name->asString() && *name->asString() == "Live thread name" &&
                cwd && cwd->asString() &&
                *cwd->asString() == "/authoritative/cwd" &&
                std::ranges::find(merged.change.affected, livePlan) !=
                    merged.change.affected.end() &&
                read->changedRevision(livePlan) == merged.change.revision,
            "an intervening live plan mutation preserves newer text while "
            "hydrating its missing authoritative type without replacing newer "
            "top-level or turn state");
  }

  NodeGraph replacingGraph;
  ProtocolUpdater replacingUpdater(replacingGraph);
  static_cast<void>(replacingUpdater.apply(
      {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
       Value::Object{
           {"threadId", Value("replacing-read-thread")},
           {"turnId", Value("old-turn")},
           {"item", Value(Value::Object{{"id", Value("old-item")},
                                        {"type", Value("agentMessage")}})}}}));
  NodeRef replacedTurn;
  NodeRef replacedItem;
  {
    auto read = replacingGraph.tryRead();
    replacedTurn = findTurn(*read, "replacing-read-thread", "old-turn");
    replacedItem =
        findItem(*read, "replacing-read-thread", "old-turn", "old-item");
  }
  const ProtocolRequestId replacingReadId("replacing-thread-read");
  const ApplyResult replacingRequest = replacingUpdater.apply(
      {DecodedMessageKind::ClientRequest, "thread/read", replacingReadId,
       Value::Object{{"threadId", Value("replacing-read-thread")}}});
  require(static_cast<bool>(replacingRequest.primary),
          "correlated replacing read exposes its exact operation");
  static_cast<void>(replacingUpdater.apply(
      {DecodedMessageKind::ServerNotification, "thread/name/updated",
       std::nullopt,
       Value::Object{{"threadId", Value("unrelated-thread")},
                     {"threadName", Value("Unrelated change")}}}));
  Value::Array freshItems{Value(Value::Object{
      {"id", Value("fresh-item")}, {"type", Value("agentMessage")}})};
  Value::Object freshTurn{{"id", Value("fresh-turn")},
                          {"items", Value(std::move(freshItems))}};
  Value::Array freshTurns{Value(std::move(freshTurn))};
  Value::Object replacingThread{{"id", Value("replacing-read-thread")},
                                {"name", Value("Fresh snapshot name")},
                                {"turns", Value(std::move(freshTurns))}};
  DecodedMessage replacingResult{
      DecodedMessageKind::ClientResult, "thread/read", replacingReadId,
      Value::Object{{"thread", Value(std::move(replacingThread))}},
      replacingRequest.primary};
  const ApplyResult replaced =
      replacingUpdater.apply(std::move(replacingResult));
  {
    auto read = replacingGraph.tryRead();
    const NodeRef thread =
        read->find({NodeKind::Thread, "replacing-read-thread"});
    const NodeRef freshTurn =
        findTurn(*read, "replacing-read-thread", "fresh-turn");
    const Value *name = field(read->state(thread), "name");
    require(thread && freshTurn && replacedTurn && replacedItem &&
                read->children(thread) == std::vector<NodeRef>{freshTurn} &&
                !findTurn(*read, "replacing-read-thread", "old-turn") &&
                !read->find(replacedItem->id()) &&
                read->removed(replacedTurn) && read->removed(replacedItem) &&
                std::ranges::find(replaced.change.removed, replacedTurn) !=
                    replaced.change.removed.end() &&
                name && name->asString() &&
                *name->asString() == "Fresh snapshot name",
            "an unrelated thread mutation does not prevent the correlated "
            "read from retiring omitted addressed-thread history");
  }
}

void authoritativeReplacementRetiresItemsAndPreservesLocalTail() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const auto item = [](std::string id) {
    return Value(Value::Object{{"id", Value(std::move(id))},
                               {"type", Value("agentMessage")}});
  };
  const auto turn = [&](std::string id, Value::Array items) {
    return Value(Value::Object{{"id", Value(std::move(id))},
                               {"items", Value(std::move(items))}});
  };
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("replace-membership")},
                {"turns",
                 Value(Value::Array{
                     turn("retained-turn",
                          {item("retained-item"), item("omitted-item")}),
                     turn("omitted-turn", {item("omitted-child")})})}})}}}));

  NodeRef omittedItem;
  NodeRef omittedTurn;
  NodeRef omittedChild;
  NodeRef localTurn;
  NodeRef localPrompt;
  {
    auto write = graph.write();
    const NodeRef threadNode =
        write.find({NodeKind::Thread, "replace-membership"});
    omittedItem = write.find(scopedItemNodeId(
        scopedTurnNodeId("replace-membership", "retained-turn"),
        "omitted-item"));
    omittedTurn =
        write.find(scopedTurnNodeId("replace-membership", "omitted-turn"));
    omittedChild = write.find(
        scopedItemNodeId(scopedTurnNodeId("replace-membership", "omitted-turn"),
                         "omitted-child"));
    NodeState localTurnState;
    localTurnState.status = NodeStatus::Pending;
    localTurnState.fields = {{"type", Value("localTurn")},
                             {"local", Value(true)}};
    localTurn = write.upsert({NodeKind::Turn, "local-turn:protected"},
                             std::move(localTurnState));
    NodeState localPromptState;
    localPromptState.status = NodeStatus::Pending;
    localPromptState.fields = {{"type", Value("localPrompt")},
                               {"local", Value(true)},
                               {"text", Value("authored locally")}};
    localPrompt = write.upsert({NodeKind::Item, "local-prompt:protected"},
                               std::move(localPromptState));
    write.setParent(threadNode, localTurn);
    write.setParent(localTurn, localPrompt);
    static_cast<void>(write.finish());
  }

  const ProtocolRequestId requestId("replace-membership-read");
  const ApplyResult request = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/read", requestId,
       Value::Object{{"threadId", Value("replace-membership")}}});
  const ApplyResult replacement = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read", requestId,
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("replace-membership")},
                {"turns", Value(Value::Array{turn(
                              "retained-turn", {item("retained-item")})})}})}},
       request.primary});

  auto read = graph.tryRead();
  const NodeRef threadNode =
      read->find({NodeKind::Thread, "replace-membership"});
  const NodeRef retainedTurn =
      findTurn(*read, "replace-membership", "retained-turn");
  require(threadNode && retainedTurn && omittedItem && omittedTurn &&
              omittedChild && !read->find(omittedItem->id()) &&
              !read->find(omittedTurn->id()) &&
              !read->find(omittedChild->id()) && read->removed(omittedItem) &&
              read->removed(omittedTurn) && read->removed(omittedChild) &&
              std::ranges::find(replacement.change.removed, omittedItem) !=
                  replacement.change.removed.end(),
          "authoritative replacement retires omitted provider items and whole "
          "provider turns instead of leaving resolvable orphans");
  require(read->find(localTurn->id()) == localTurn &&
              read->find(localPrompt->id()) == localPrompt &&
              read->parent(localTurn) == threadNode &&
              read->parent(localPrompt) == localTurn &&
              read->children(threadNode) ==
                  std::vector<NodeRef>{retainedTurn, localTurn},
          "authoritative replacement preserves the exact explicit local "
          "optimistic tail and its stable NodeRefs");
}

void rollbackAndRevertReplaceAuthoritativeHistory() {
  NodeGraph graph;
  ProtocolUpdater updater(graph);
  const auto turn = [](std::string id, std::string itemId) {
    return Value(
        Value::Object{{"id", Value(std::move(id))},
                      {"status", Value("completed")},
                      {"items", Value(Value::Array{Value(Value::Object{
                                    {"id", Value(std::move(itemId))},
                                    {"type", Value("agentMessage")}})})}});
  };
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/started", std::nullopt,
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("history-mutation")},
                {"turns", Value(Value::Array{turn("turn-1", "item-1"),
                                             turn("turn-2", "item-2"),
                                             turn("turn-3", "item-3")})}})}}}));
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/started", std::nullopt,
       Value::Object{
           {"threadId", Value("history-mutation")},
           {"turnId", Value("turn-2")},
           {"item", Value(Value::Object{{"id", Value("item-2-omitted")},
                                        {"type", Value("agentMessage")}})}}}));
  NodeRef removedTurn;
  NodeRef removedItem;
  NodeRef removedRetainedTurnItem;
  {
    auto read = graph.tryRead();
    removedTurn = findTurn(*read, "history-mutation", "turn-3");
    removedItem = findItem(*read, "history-mutation", "turn-3", "item-3");
    removedRetainedTurnItem =
        findItem(*read, "history-mutation", "turn-2", "item-2-omitted");
  }

  const ProtocolRequestId rollbackId("rollback-history");
  const ApplyResult rollbackRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/rollback", rollbackId,
       Value::Object{{"threadId", Value("history-mutation")},
                     {"numTurns", Value(std::uint64_t{1})}}});
  const ApplyResult rolledBack = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/rollback", rollbackId,
       Value::Object{
           {"thread",
            Value(Value::Object{
                {"id", Value("history-mutation")},
                {"turns", Value(Value::Array{turn("turn-1", "item-1"),
                                             turn("turn-2", "item-2")})}})}},
       rollbackRequest.primary});
  NodeRef firstTurn;
  NodeRef secondTurn;
  {
    auto read = graph.tryRead();
    const NodeRef threadNode =
        read->find({NodeKind::Thread, "history-mutation"});
    firstTurn = findTurn(*read, "history-mutation", "turn-1");
    secondTurn = findTurn(*read, "history-mutation", "turn-2");
    require(threadNode && firstTurn && secondTurn &&
                protocolIds(*read, read->children(threadNode)) ==
                    std::vector<std::string>{"turn-1", "turn-2"} &&
                !read->find(removedTurn->id()) &&
                !read->find(removedItem->id()) &&
                !read->find(removedRetainedTurnItem->id()) &&
                read->removed(removedTurn) && read->removed(removedItem) &&
                read->removed(removedRetainedTurnItem) &&
                std::ranges::find(rolledBack.change.removed, removedTurn) !=
                    rolledBack.change.removed.end() &&
                std::ranges::find(rolledBack.change.removed,
                                  removedRetainedTurnItem) !=
                    rolledBack.change.removed.end(),
            "successful rollback replaces returned history and retires every "
            "superseded turn and item");
  }

  const ProtocolRequestId revertId("revert-history");
  const ApplyResult revertRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/revert", revertId,
       Value::Object{{"threadId", Value("history-mutation")},
                     {"beforeTurnId", Value("turn-2")}}});
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ClientResult, "thread/revert", revertId,
       Value::Object{
           {"thread", Value(Value::Object{{"id", Value("history-mutation")},
                                          {"turns", Value(Value::Array{})}})},
           {"turnsBackwardsCursor", Value("retained-prefix-cursor")},
           {"itemsBackwardsCursor", Value("retained-items-cursor")}},
       revertRequest.primary}));
  {
    auto read = graph.tryRead();
    const NodeRef threadNode =
        read->find({NodeKind::Thread, "history-mutation"});
    const auto state = threadNode ? read->state(threadNode) : nullptr;
    require(threadNode && read->children(threadNode).empty() &&
                !read->find(firstTurn->id()) && !read->find(secondTurn->id()) &&
                read->removed(firstTurn) && read->removed(secondTurn) &&
                field(state, "historyHasMore") &&
                field(state, "historyHasMore")->asBool() &&
                *field(state, "historyHasMore")->asBool() &&
                field(state, "historyNextCursor") &&
                field(state, "historyNextCursor")->asString() &&
                *field(state, "historyNextCursor")->asString() ==
                    "retained-prefix-cursor" &&
                field(state, "itemsHistoryBackwardsCursor") &&
                field(state, "itemsHistoryBackwardsCursor")->asString() &&
                *field(state, "itemsHistoryBackwardsCursor")->asString() ==
                    "retained-items-cursor",
            "successful paginated revert unlinks cached history and exposes "
            "only authoritative reload cursors");
  }
}

} // namespace

int main() {
  catalogIsComplete();
  everyKnownMethodDispatches();
  nestedEntitiesAndStreamsStayCurrent();
  streamedTextIsBoundedAndReportsOmission();
  longStreamingDeltasStayBoundedInStateAndCost();
  activeTurnRelationTracksLifecycle();
  effectiveThreadSettingsConvergeAcrossWireShapes();
  threadItemPagesMaintainScopedContainmentAndOrder();
  scopedProviderIdentityCannotCrossParents();
  rootOrderAndThreadHierarchyAreExplicit();
  agentChildAggregatesTrackEveryReferencingItem();
  forkRelationsFollowTheCurrentSource();
  semanticDeltasAndHydratedOrderStayCurrent();
  realtimeNotificationsMaintainOneCurrentSession();
  hookRunsKeepNestedIdentityAndCurrentOwnership();
  graphRelationsInvalidationAndIncarnationsAreExplicit();
  promptMaterializationDoesNotAcknowledgeDelivery();
  turnRootsAndPagedHistoryStayExplicit();
  resultsAndListsCorrelate();
  lateResultsCannotRecreateDeletedTargets();
  exactRequestTargetsOverridePayloadAddressingAndLifetime();
  accountFacetsConvergeAndRateLimitPatchesStaySparse();
  successfulRefreshesRetireInvalidationsAndConfigWritesInvalidate();
  catalogResultsMaterializeNaturalEntityKinds();
  specializedNotificationFamiliesKeepCurrentSemantics();
  reusedWireIdsRequireExactCurrentNodes();
  keyedRuntimeNotificationsKeepIndependentCurrentState();
  turnErrorsUpdateAddressedStateWithoutLosingTheNotice();
  mcpServerNotificationsKeepCanonicalScope();
  interactionsAndRemovalKeepLifetime();
  unknownAndNeutralAreIsolated();
  lifecycleFactsAndRemovalPreserveThreadHierarchy();
  deletionUnlinksWholeGraph();
  largeThreadDeletionIsNearLinear();
  correlatedThreadReadsPreserveOnlyInterveningLiveState();
  authoritativeReplacementRetiresItemsAndPreservesLocalTail();
  rollbackAndRevertReplaceAuthoritativeHistory();

  if (failures != 0) {
    std::cerr << failures << " nodegraph protocol assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "codexui protocol updater tests passed\n";
  return EXIT_SUCCESS;
}
