// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/WorkerLogic.h"

#include <algorithm>
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

  require(logic.transportEvent("connecting", "dialing bridge") ==
              ChannelSendStatus::Accepted,
          "connecting state is published");
  require(takeGraphChanged(channels).has_value(),
          "connecting state wakes Qt once");
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
  require(takeGraphChanged(channels).has_value(),
          "connected state wakes Qt once");
  require(logic.bridgeState("bridge-1", "controller", "bridge-1", 7, "ready",
                            "provider ready") == ChannelSendStatus::Accepted,
          "bridge/provider state is published");
  require(takeGraphChanged(channels).has_value(),
          "bridge/provider state wakes Qt once");
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
  require(takeGraphChanged(channels).has_value(),
          "connection settings wake Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    const Value *settings = field(state, "settings");
    const Value::Object *object = settings ? settings->asObject() : nullptr;
    bool selectedUnix = false;
    if (object) {
      const auto selected = object->find("selected");
      selectedUnix = selected != object->end() && selected->second.asString() &&
                     *selected->second.asString() == "unix";
    }
    require(selectedUnix,
            "connection node retains current selectable transport settings");
  }

  require(logic.bridgeState("bridge-1", "observer", "bridge-2", 7,
                            std::nullopt) == ChannelSendStatus::Accepted,
          "controller identity can change without a provider event");
  require(takeGraphChanged(channels).has_value(),
          "controller identity change wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(
        state && stringFieldEquals(state, "role", "observer") &&
            stringFieldEquals(state, "controllerConnectionId", "bridge-2") &&
            stringFieldEquals(state, "providerState", "ready") &&
            stringFieldEquals(state, "providerDetail", "provider ready"),
        "unrelated bridge events preserve same-generation provider facts");
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
  require(takeGraphChanged(channels).has_value(),
          "retrying state wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Pending &&
                unsignedFieldEquals(state, "connectionGeneration", 1) &&
                unsignedFieldEquals(state, "providerGeneration", 7) &&
                stringFieldEquals(state, "connectionId", "") &&
                stringFieldEquals(state, "role", "") &&
                stringFieldEquals(state, "controllerConnectionId", "") &&
                stringFieldEquals(state, "providerState", "") &&
                stringFieldEquals(state, "providerDetail", "") &&
                stringFieldEquals(state, "transportDetail", "retry scheduled"),
            "retry clears bridge facts but preserves both generations");
  }

  require(logic.transportEvent("disconnected", "connection lost") ==
              ChannelSendStatus::Accepted,
          "disconnect state is published");
  require(takeGraphChanged(channels).has_value(),
          "disconnect state wakes Qt once");
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
  require(takeGraphChanged(channels).has_value(), "reconnection wakes Qt once");
  {
    auto read = graph.tryRead();
    const NodeRef connection =
        read ? read->find({NodeKind::Connection, "connection"}) : NodeRef{};
    const auto state = read && connection ? read->state(connection) : nullptr;
    require(state && state->status == NodeStatus::Connected &&
                unsignedFieldEquals(state, "connectionGeneration", 2) &&
                unsignedFieldEquals(state, "providerGeneration", 0),
            "each successful connection advances generation and resets "
            "provider generation");
  }
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

void graphNotificationSaturationCoalesces() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  std::size_t admitted = 0;
  bool reachedLimit = false;
  bool statusesAccepted = true;
  for (std::size_t attempt = 0; attempt <= ThreadChannels::WorkerToQtCapacity;
       ++attempt) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "fill-" + std::to_string(attempt),
                    {}};
    const ChannelSendStatus status = channels.sendUiEffect(effect);
    if (status == ChannelSendStatus::QueueFull) {
      reachedLimit = true;
      break;
    }
    statusesAccepted =
        statusesAccepted && status == ChannelSendStatus::Accepted;
    ++admitted;
  }
  require(statusesAccepted && reachedLimit &&
              admitted + 1 == ThreadChannels::WorkerToQtCapacity,
          "ordinary worker messages fill to the reserved queue slot");

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
    require(read && read->retiredNodes().size() == 1 && !lifetime.expired(),
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
    require(read && read->retiredNodes().empty(),
            "UiDetached removes the graph retirement pin");
  }
  require(lifetime.expired(),
          "retired node dies after the acknowledgement releases its last pin");
}

void reverseInteractionResolutionUpdatesTheGraph() {
  NodeGraph graph;
  ThreadChannels channels;
  WorkerLogic logic(graph, channels);

  static_cast<void>(logic.apply({DecodedMessageKind::ServerRequest,
                                 "item/commandExecution/requestApproval",
                                 ProtocolRequestId("approval-1"),
                                 {{"threadId", Value("thread-1")},
                                  {"turnId", Value("turn-1")},
                                  {"itemId", Value("item-1")}}}));
  require(takeGraphChanged(channels).has_value(),
          "reverse interaction creation wakes Qt");

  require(logic.resolveInteraction(ProtocolRequestId("approval-1"), true) ==
              ChannelSendStatus::Accepted,
          "accepted reverse interaction is resolved");
  const std::optional<GraphChanged> removed = takeGraphChanged(channels);
  require(removed && removed->removed.size() == 1 &&
              removed->removed.front()->id().kind == NodeKind::Interaction,
          "accepted response removes the pending interaction atomically");
  if (removed && !removed->removed.empty())
    static_cast<void>(logic.acknowledgeUiDetached(removed->removed.front()));

  static_cast<void>(logic.apply({DecodedMessageKind::ServerRequest,
                                 "item/fileChange/requestApproval",
                                 ProtocolRequestId("approval-2"),
                                 {{"threadId", Value("thread-1")}}}));
  require(takeGraphChanged(channels).has_value(),
          "second reverse interaction creation wakes Qt");
  require(logic.resolveInteraction(ProtocolRequestId("approval-2"), false,
                                   "transport rejected response") ==
              ChannelSendStatus::Accepted,
          "rejected response failure is recorded");
  require(takeGraphChanged(channels).has_value(),
          "rejected response failure wakes Qt");
  auto read = graph.tryRead();
  const NodeRef failed =
      read ? read->find({NodeKind::Interaction,
                         ProtocolRequestId("approval-2").canonical()})
           : NodeRef{};
  const auto state = read && failed ? read->state(failed) : nullptr;
  require(state && state->status == NodeStatus::Failed &&
              stringFieldEquals(state, "error", "transport rejected response"),
          "failed response remains visible with its concrete error");
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
  first.payload.emplace("model", Value("gpt-current"));
  const char *const textStorage = first.promptText.data();
  const std::uint8_t *const bytesStorage =
      first.attachments.front().bytes->data();
  PromptTransition admitted = logic.admitPrompt(std::move(first));
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
    require(firstPrompt && localTurn &&
                localTurn->id().canonical.starts_with("local-turn:") &&
                read->parent(localTurn) == thread && text && text->asString() &&
                *text->asString() == admitted.command->promptText && dispatch &&
                dispatch->asString() &&
                *dispatch->asString() == "dispatching" &&
                read->related(runtime, RelationKind::PendingPrompt) ==
                    std::vector<NodeRef>{firstPrompt} &&
                read->related(thread, RelationKind::PendingPrompt) ==
                    std::vector<NodeRef>{firstPrompt},
            "admission stores one directly-related local prompt under a "
            "provisional turn");
  }

  NodeAction queued;
  queued.target = thread;
  queued.kind = NodeActionKind::SubmitPrompt;
  queued.promptText = "second exact prompt";
  PromptTransition second = logic.admitPrompt(std::move(queued));
  require(!second.command,
          "a second prompt for the same thread remains queued while one "
          "request is in flight");
  static_cast<void>(takeWorkerMessages(channels));

  NodeAction parallel;
  parallel.target = independent;
  parallel.kind = NodeActionKind::SubmitPrompt;
  parallel.promptText = "independent prompt";
  PromptTransition independentAdmission =
      logic.admitPrompt(std::move(parallel));
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
  require(completed.command &&
              completed.command->kind == PromptCommandKind::SteerTurn &&
              completed.command->expectedTurnId == "authoritative-turn" &&
              completed.command->promptText == "second exact prompt",
          "a successful request releases exactly the next same-thread prompt "
          "as a steer command");
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
    authoritative = read->find({NodeKind::Item, "user-item"});
    require(
        authoritative &&
            read->related(authoritative, RelationKind::PromptMaterialization) ==
                std::vector<NodeRef>{firstPrompt} &&
            read->state(firstPrompt)->status == NodeStatus::Completed &&
            stringFieldEquals(read->state(firstPrompt), "dispatchState",
                              "materialized"),
        "matching authoritative clientId directly relates the user item "
        "to its local visual identity");
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
  PromptTransition admitted = logic.admitFirstPrompt(std::move(action));
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
  const std::vector<WorkerToQtMessage> admissionMessages =
      takeWorkerMessages(channels);
  require(std::ranges::any_of(
              admissionMessages,
              [&](const auto &message) {
                const UiEffect *effect = std::get_if<UiEffect>(&message);
                return effect && effect->kind == UiEffectKind::SelectThread &&
                       effect->target == std::optional<NodeRef>(draft);
              }),
          "new-thread admission selects its materialized local draft NodeRef");
  {
    auto read = graph.tryRead();
    const NodeRef runtime = read->find({NodeKind::Runtime, "runtime"});
    require(draft && draft->id().canonical.starts_with("local-thread:") &&
                read->related(runtime, RelationKind::RootThread).front() ==
                    draft &&
                read->parent(read->parent(localPrompt)) == draft,
            "the first prompt is immediately renderable under one draft "
            "thread and provisional turn");
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
  const std::vector<WorkerToQtMessage> migrationMessages =
      takeWorkerMessages(channels);
  {
    auto read = graph.tryRead();
    const NodeRef actual = read->find({NodeKind::Thread, "created-thread"});
    require(admitted.command->kind == PromptCommandKind::StartTurn &&
                admitted.command->thread == actual &&
                admitted.command->options.contains("approvalPolicy") &&
                !read->find(draft->id()) &&
                read->parent(read->parent(localPrompt)) == actual &&
                read->related(actual, RelationKind::PendingPrompt) ==
                    std::vector<NodeRef>{localPrompt} &&
                std::ranges::any_of(
                    migrationMessages,
                    [&](const auto &message) {
                      const UiEffect *effect = std::get_if<UiEffect>(&message);
                      return effect &&
                             effect->kind == UiEffectKind::SelectThread &&
                             effect->target == std::optional<NodeRef>(actual);
                    }),
            "migration removes the draft shell, selects the canonical thread, "
            "and changes the retained command to turn/start");
  }

  NodeGraph failedGraph;
  ThreadChannels failedChannels;
  WorkerLogic failedLogic(failedGraph, failedChannels);
  RuntimeAction failingCreate;
  failingCreate.kind = RuntimeActionKind::CreateThread;
  failingCreate.promptText = "first unsent draft";
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
    require(!failed.command && prompts.size() == 2 &&
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
  }
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
    turn = read->find({NodeKind::Turn, "reset-turn"});
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
                     node->id().kind == NodeKind::Item;
    }
    const auto promptState = read->state(localPrompt);
    const Value *recovery = field(promptState, "requiresExplicitRecovery");
    const Value *settings = field(read->state(connection), "settings");
    require(
        read->revision() == before + 1 && resetMessages.size() == 1 &&
            std::holds_alternative<GraphChanged>(resetMessages.front()) &&
            onlyAllowed && runtime && connection && !owner &&
            !read->find({NodeKind::Turn, "reset-turn"}) && localPrompt &&
            recoveryOwner &&
            recoveryOwner->id().canonical.starts_with(
                "local-recovery-thread:") &&
            recoveryTurn->id().canonical.starts_with("local-recovery-turn:") &&
            !read->find({NodeKind::Operation,
                         ProtocolRequestId("pending-catalog").canonical()}) &&
            !read->find({NodeKind::Interaction,
                         ProtocolRequestId("pending").canonical()}) &&
            promptState->status == NodeStatus::Failed &&
            stringFieldEquals(promptState, "dispatchState", "uncertain") &&
            recovery && recovery->asBool() && *recovery->asBool() && settings &&
            settings->asObject() &&
            read->related(runtime, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{localPrompt} &&
            read->related(recoveryOwner, RelationKind::PendingPrompt) ==
                std::vector<NodeRef>{localPrompt},
        "provider reset publishes one complete revision, removes stale "
        "canonical owners and work, preserves settings, and reparents only "
        "explicit prompt recovery state to local shells");
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
    const NodeRef reusedTurn = read->find({NodeKind::Turn, "reset-turn"});
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
  WorkerToQtMessage admittedMessage;
  require(
      failedWake == ChannelSendStatus::AcceptedWakeFailed &&
          messageAdmitted(failedWake) && wakeFailed(failedWake) &&
          closedChannels.tryReceiveForQt(admittedMessage) &&
          std::holds_alternative<WorkerStopped>(admittedMessage) &&
          std::get<WorkerStopped>(admittedMessage).reason ==
              "wake already closed",
      "WorkerStopped reports an admitted-but-failed wake without hiding it");
}

} // namespace

int main() {
  protocolUpdatesPublishForUnlockedReads();
  connectionStateAndGenerationsStayCurrent();
  stateNeutralMessagesDoNotWakeQt();
  graphNotificationSaturationCoalesces();
  uiDetachAcknowledgementIsRevisionNeutral();
  reverseInteractionResolutionUpdatesTheGraph();
  localPromptsAreGraphNodesAndDispatchPerThread();
  firstPromptCreatesAndMigratesOneDraftThread();
  providerGenerationResetIsAtomicAndKeepsOnlyRecoveryPrompts();
  workerStoppedDeliveryIsExplicit();

  if (failures != 0) {
    std::cerr << failures << " worker logic assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "codexui worker logic tests passed\n";
  return EXIT_SUCCESS;
}
