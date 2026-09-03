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
    turnRef = read->find({NodeKind::Turn, "turn-1"});
    itemRef = read->find({NodeKind::Item, "item-1"});
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
    const NodeRef item = read->find({NodeKind::Item, "spawn-item"});
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
    const auto plan = read->state(read->find({NodeKind::Item, "plan-item"}));
    const auto reasoning =
        read->state(read->find({NodeKind::Item, "reasoning-item"}));
    const auto command =
        read->state(read->find({NodeKind::Item, "command-item"}));
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
    const NodeRef turnTwo = read->find({NodeKind::Turn, "turn-two"});
    require(canonicalIds(read->children(thread)) ==
                    std::vector<std::string>{"turn-two", "turn-one"} &&
                canonicalIds(read->children(turnTwo)) ==
                    std::vector<std::string>{"item-two", "item-one"},
            "thread/read publishes exact turn and item order in one revision");
  }
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
    const NodeRef turn = read->find({NodeKind::Turn, "long-turn"});
    openingPrompt = read->find({NodeKind::Item, "opening-prompt"});
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
  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "item/agentMessage/delta",
       std::nullopt,
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
    const NodeRef turn = read->find({NodeKind::Turn, "long-turn"});
    const NodeRef steering = read->find({NodeKind::Item, "later-steering"});
    const Value *loaded = field(read->state(thread), "historyLoadedItemCount");
    require(read->children(turn) ==
                    std::vector<NodeRef>{
                        steering,
                        read->find({NodeKind::Item, "latest-activity"})} &&
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
                    removedHistory.change.removed.end(), openingPrompt) !=
              removedHistory.change.removed.end(),
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
    const NodeRef turn = read->find({NodeKind::Turn, "newer-turn"});
    const NodeRef root = read->find({NodeKind::Item, "newer-root"});
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
    require(canonicalIds(read->children(thread)) ==
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
  const ApplyResult failedRequest = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/read", failedId,
       Value::Object{{"threadId", Value("missing")}}});
  const ApplyResult failedResult = updater.apply(
      {DecodedMessageKind::ClientError, "thread/read", failedId,
       Value::Object{{"code", Value(-32001)},
                     {"message", Value("overloaded")}},
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

  const ApplyResult first = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/read", reused,
       Value::Object{{"threadId", Value("old-thread")}}});
  const ApplyResult second = updater.apply(
      {DecodedMessageKind::ClientRequest, "thread/list", reused,
       Value::Object{{"limit", Value(10)}}});
  require(first.primary && second.primary && first.primary != second.primary &&
              std::ranges::find(second.change.removed, first.primary) !=
                  second.change.removed.end(),
          "reusing a request id replaces rather than mutates its old "
          "Operation NodeRef");

  const std::uint64_t beforeLate = graph.publishedRevision();
  const ApplyResult late = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/read", reused,
       Value::Object{{"thread", Value(Value::Object{
                                         {"id", Value("late-thread")}})}},
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
  const ApplyResult current = updater.apply(
      {DecodedMessageKind::ClientResult, "thread/list", reused,
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
    require(staleResolution.empty() &&
                read->find({NodeKind::Interaction,
                            interactionId.canonical()}) ==
                    newInteraction.primary,
            "an exact response for the retired interaction cannot resolve its "
            "replacement");
  }
  const GraphChange exactResolution =
      updater.resolveInteraction(newInteraction.primary, true);
  require(std::ranges::find(exactResolution.removed,
                            newInteraction.primary) !=
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
                targets.front()->id().canonical == "item-approval",
            "interaction directly relates to its addressed item");
    const NodeRef turn = read->find({NodeKind::Turn, "turn-approval"});
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
    require(thread &&
                read->related(thread, RelationKind::PendingInteraction) ==
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
    require(thread && runtime &&
                read->related(thread, RelationKind::PendingInteraction)
                    .empty() &&
                read->related(runtime, RelationKind::PendingInteraction)
                    .empty(),
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

  static_cast<void>(updater.apply(
      {DecodedMessageKind::ServerNotification, "future/newAlternative",
       std::nullopt,
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
    removedTurn = read->find({NodeKind::Turn, "delete-turn"});
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
    NodeRef turn = read->find({NodeKind::Turn, "delete-turn"});
    require(!turn && read->removed(removedTurn),
            "contained turns are removed rather than retained as orphans");
  }
}

} // namespace

int main() {
  catalogIsComplete();
  everyKnownMethodDispatches();
  nestedEntitiesAndStreamsStayCurrent();
  rootOrderAndThreadHierarchyAreExplicit();
  semanticDeltasAndHydratedOrderStayCurrent();
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
