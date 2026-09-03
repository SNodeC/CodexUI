// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/ProtocolCatalog.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

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
    const Value *stream =
        field(read->state(itemRef), "item/agentMessage/delta");
    require(stream && stream->asString() &&
                *stream->asString() == "hello world",
            "stream fragments append in arrival order on the item");
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
                                   {"nextCursor", Value("next")}}});
  require(result.change.revision == request.change.revision + 1,
          "correlated result is one later atomic revision");
  {
    auto read = graph.tryRead();
    require(read->state(operation)->status == NodeStatus::Completed,
            "successful result completes the same operation node");
    require(read->find({NodeKind::Thread, "listed-1"}) &&
                read->find({NodeKind::Thread, "listed-2"}),
            "thread list result materializes its current entities");
  }

  const ProtocolRequestId failedId(22);
  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientRequest, "thread/read", failedId,
                     Value::Object{{"threadId", Value("missing")}}}));
  static_cast<void>(
      updater.apply({DecodedMessageKind::ClientError, "thread/read", failedId,
                     Value::Object{{"code", Value(-32001)},
                                   {"message", Value("overloaded")}}}));
  {
    auto read = graph.tryRead();
    NodeRef failed = read->find({NodeKind::Operation, failedId.canonical()});
    require(failed && read->state(failed)->status == NodeStatus::Failed,
            "failed result marks its correlated operation failed");
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
                targets.front()->id().canonical == "item-approval",
            "interaction directly relates to its addressed item");
  }

  GraphChange rejected =
      updater.resolveInteraction(interactionId, false, "bridge rejected");
  require(!rejected.empty(), "rejected response updates interaction state");
  {
    auto read = graph.tryRead();
    require(read->state(interaction)->status == NodeStatus::Failed,
            "rejected response remains visibly failed");
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
  {
    auto read = graph.tryRead();
    removed = read->find({NodeKind::Thread, "delete-thread"});
  }
  ApplyResult result = updater.apply(
      {DecodedMessageKind::ServerNotification, "thread/deleted", std::nullopt,
       Value::Object{{"threadId", Value("delete-thread")}}});
  require(result.change.removed.size() == 1 &&
              result.change.removed.front() == removed,
          "thread deletion queues its stable removed reference");
  {
    auto read = graph.tryRead();
    require(read->removed(removed), "removed node is marked removed");
    require(read->children(removed).empty(),
            "removed thread is unlinked from child turns");
    NodeRef turn = read->find({NodeKind::Turn, "delete-turn"});
    require(turn && !read->parent(turn),
            "surviving turn no longer points at removed thread");
  }
}

} // namespace

int main() {
  catalogIsComplete();
  everyKnownMethodDispatches();
  nestedEntitiesAndStreamsStayCurrent();
  resultsAndListsCorrelate();
  interactionsAndRemovalKeepLifetime();
  unknownAndNeutralAreIsolated();
  deletionUnlinksWholeGraph();

  if (failures != 0) {
    std::cerr << failures << " nodegraph protocol assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "codexui protocol updater tests passed\n";
  return EXIT_SUCCESS;
}
