// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/NodeGraphJson.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/TurnSettingsPolicy.h"
#include "codex/UiStatus.h"
#include "codex/nodegraph/ProtocolUpdater.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>

namespace codexui::codex::ui {
namespace {

using nodegraph::NodeId;
using nodegraph::NodeKind;
using nodegraph::NodeRef;
using nodegraph::NodeState;

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << message << '\n';
  return false;
}

template <typename T>
const T *pageValue(const InspectorPageSnapshot &page,
                   std::size_t valueIndex = 0) {
  for (const InspectorRow &row : page.rows)
    if (const auto *value = std::get_if<T>(&row.value)) {
      if (valueIndex == 0)
        return value;
      --valueIndex;
    }
  return nullptr;
}

template <typename T>
std::size_t pageValueCount(const InspectorPageSnapshot &page) {
  return static_cast<std::size_t>(std::ranges::count_if(
      page.rows, [](const InspectorRow &row) {
        return std::holds_alternative<T>(row.value);
      }));
}

NodeState itemState(std::string id, std::string type, std::string text) {
  NodeState state;
  state.fields.emplace("id", std::move(id));
  state.fields.emplace("type", std::move(type));
  state.fields.emplace("text", std::move(text));
  return state;
}

bool projectsCanonicalTurnStructureAndRoot() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef firstTurn;
  NodeRef secondTurn;
  NodeRef firstPrompt;
  NodeRef answer;
  NodeRef secondPrompt;
  {
    auto write = graph.write();
    NodeState threadState;
    threadState.fields.emplace("id", "thread-1");
    thread =
        write.upsert({NodeKind::Thread, "thread-1"}, std::move(threadState));

    NodeState turnOneState;
    turnOneState.fields.emplace("id", "turn-1");
    firstTurn =
        write.upsert({NodeKind::Turn, "turn-1"}, std::move(turnOneState));
    firstPrompt =
        write.upsert({NodeKind::Item, "prompt-1"},
                     itemState("prompt-1", "userMessage", "first prompt"));
    answer =
        write.upsert({NodeKind::Item, "answer-1"},
                     itemState("answer-1", "agentMessage", "first answer"));

    NodeState turnTwoState;
    turnTwoState.fields.emplace("id", "turn-2");
    secondTurn =
        write.upsert({NodeKind::Turn, "turn-2"}, std::move(turnTwoState));
    secondPrompt =
        write.upsert({NodeKind::Item, "prompt-2"},
                     itemState("prompt-2", "userMessage", "second prompt"));

    write.setParent(thread, firstTurn);
    write.setParent(firstTurn, firstPrompt);
    write.setParent(firstTurn, answer);
    write.relate(firstTurn, nodegraph::RelationKind::TurnRootItem, firstPrompt);
    write.setParent(thread, secondTurn);
    write.setParent(secondTurn, secondPrompt);
    write.relate(secondTurn, nodegraph::RelationKind::TurnRootItem,
                 secondPrompt);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto result = adapter.conversation(thread);
  if (!require(result.has_value(), "adapter projection was unavailable") ||
      !require(result->threadId == "thread-1", "wrong projected thread") ||
      !require(result->sections.size() == 2, "wrong turn count") ||
      !require(result->sections[0].cards.size() == 2,
               "wrong first-turn card count") ||
      !require(result->sections[1].cards.size() == 1,
               "wrong second-turn card count") ||
      !require(result->sections[0].key == "turn:8:thread-16:turn-1",
               "the established stable section identity changed") ||
      !require(result->sections[0].rootCardKey.has_value(),
               "first turn lost its canonical root") ||
      !require(result->sections[0].cards[0].key ==
                   *result->sections[0].rootCardKey,
               "first root is not the owning card"))
    return false;

  const auto *prompt = std::get_if<middle::UserMessageData>(
      &result->sections[0].cards[0].payload);
  const auto *agent = std::get_if<middle::AgentMessageData>(
      &result->sections[0].cards[1].payload);
  return require(prompt && prompt->text == "first prompt",
                 "user card projection changed") &&
         require(agent && agent->text == "first answer",
                 "agent card projection changed");
}

bool projectsEveryLoadedItemInCanonicalOrder() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef prompt;
  {
    auto write = graph.write();
    NodeState threadState;
    threadState.fields.emplace("id", "thread-limit");
    thread = write.upsert({NodeKind::Thread, "thread-limit"},
                          std::move(threadState));
    NodeState turnState;
    turnState.fields.emplace("id", "turn-limit");
    turn = write.upsert({NodeKind::Turn, "turn-limit"}, std::move(turnState));
    write.setParent(thread, turn);
    prompt = write.upsert({NodeKind::Item, "root"},
                          itemState("root", "userMessage", "owning prompt"));
    write.setParent(turn, prompt);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, prompt);
    for (int index = 0; index < 5; ++index) {
      const std::string id = "answer-" + std::to_string(index);
      NodeRef item =
          write.upsert({NodeKind::Item, id}, itemState(id, "agentMessage", id));
      write.setParent(turn, item);
    }
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto result = adapter.conversation(thread);
  return require(result.has_value(), "complete projection unavailable") &&
         require(!result->hasMore,
                 "complete projection invented provider continuation") &&
         require(result->sections.size() == 1,
                 "complete projection lost its turn") &&
         require(result->sections[0].cards.size() == 6,
                 "complete projection omitted loaded graph children") &&
         require(result->sections[0].cards.front().key ==
                     *result->sections[0].rootCardKey,
                 "the canonical root does not own the turn") &&
         require(result->sections[0].cards.back().itemId == "answer-4",
                 "complete projection changed canonical child order");
}

bool completeHistoryRetainsCanonicalItemOrder() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef root;
  NodeRef olderSteering;
  NodeRef cutoffSteering;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "semantic-window-thread"});
    turn = write.upsert({NodeKind::Turn, "semantic-window-turn"});
    write.setField(turn, "id", "semantic-window-turn");
    write.setParent(thread, turn);
    root = write.upsert(
        {NodeKind::Item, "semantic-window-root"},
        itemState("semantic-window-root", "userMessage", "Opening prompt"));
    write.setParent(turn, root);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);

    const auto appendActivity = [&](int serial) {
      const std::string id =
          "semantic-window-activity-" + std::to_string(serial);
      const NodeRef activity =
          write.upsert({NodeKind::Item, id}, itemState(id, "agentMessage", id));
      write.setParent(turn, activity);
    };
    for (int serial = 0; serial < 3; ++serial)
      appendActivity(serial);
    olderSteering =
        write.upsert({NodeKind::Item, "semantic-window-older-steering"},
                     itemState("semantic-window-older-steering", "userMessage",
                               "Keep this older acknowledged steering prompt"));
    write.setParent(turn, olderSteering);
    for (int serial = 3; serial < 7; ++serial)
      appendActivity(serial);
    cutoffSteering =
        write.upsert({NodeKind::Item, "semantic-window-cutoff-steering"},
                     itemState("semantic-window-cutoff-steering", "userMessage",
                               "Own the newest activity segment"));
    write.setParent(turn, cutoffSteering);
    for (int serial = 7; serial < 19; ++serial)
      appendActivity(serial);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto projected = adapter.conversation(thread);
  if (!require(projected && projected->sections.size() == 1,
               "complete history projection was unavailable"))
    return false;

  const auto &section = projected->sections.front();
  const auto olderSteeringPosition =
      std::ranges::find_if(section.cards, [&](const auto &card) {
        return card.target == olderSteering;
      });
  const auto cutoffSteeringPosition =
      std::ranges::find_if(section.cards, [&](const auto &card) {
        return card.target == cutoffSteering;
      });
  const auto laterActivityPosition =
      std::ranges::find_if(section.cards, [](const auto &card) {
        return card.itemId == "semantic-window-activity-11";
      });
  return require(section.cards.size() == 22,
                 "complete history projection omitted loaded rows") &&
         require(section.rootCardKey && section.cards.front().target == root,
                 "complete history projection lost its Turn root") &&
         require(olderSteeringPosition != section.cards.end() &&
                     cutoffSteeringPosition != section.cards.end() &&
                     laterActivityPosition != section.cards.end() &&
                     olderSteeringPosition < cutoffSteeringPosition &&
                     cutoffSteeringPosition < laterActivityPosition,
                 "complete history projection reordered a loaded row") &&
         require(!projected->hasMore,
                 "loaded rows were mistaken for provider continuation");
}

bool completeHistoryProjectionScalesLinearly() {
  struct Fixture {
    NodeRef thread;
    std::size_t itemCount = 0;
    std::size_t userCount = 0;
  };

  nodegraph::NodeGraph graph;
  std::array<Fixture, 2> fixtures;
  {
    auto write = graph.write();
    const std::array<std::size_t, 2> counts{10'000, 40'000};
    for (std::size_t fixtureIndex = 0; fixtureIndex < counts.size();
         ++fixtureIndex) {
      Fixture &fixture = fixtures[fixtureIndex];
      fixture.itemCount = counts[fixtureIndex];
      const std::string prefix =
          "semantic-scale-" + std::to_string(fixture.itemCount);
      fixture.thread =
          write.upsert({NodeKind::Thread, prefix + "-thread"});
      const NodeRef turn = write.upsert({NodeKind::Turn, prefix + "-turn"});
      write.setField(turn, "id", prefix + "-turn");
      write.setParent(fixture.thread, turn);
      NodeRef root;
      for (std::size_t serial = 0; serial < fixture.itemCount; ++serial) {
        const bool user = serial % 257 == 0;
        const std::string id = prefix + "-item-" + std::to_string(serial);
        const NodeRef item = write.upsert(
            {NodeKind::Item, id},
            itemState(id, user ? "userMessage" : "agentMessage", id));
        write.setParent(turn, item);
        if (user) {
          ++fixture.userCount;
          if (!root)
            root = item;
        }
      }
      write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    }
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  std::array<std::chrono::microseconds, 2> medians;
  bool result = true;
  for (std::size_t fixtureIndex = 0; fixtureIndex < fixtures.size();
       ++fixtureIndex) {
    const Fixture &fixture = fixtures[fixtureIndex];
    std::array<std::chrono::microseconds, 3> samples;
    std::optional<middle::ConversationSnapshot> projected;
    for (std::chrono::microseconds &sample : samples) {
      const auto started = std::chrono::steady_clock::now();
      projected = adapter.conversation(fixture.thread);
      sample = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started);
    }
    std::ranges::sort(samples);
    medians[fixtureIndex] = samples[1];

    const auto *section = projected && projected->sections.size() == 1
                              ? &projected->sections.front()
                              : nullptr;
    const std::size_t projectedUsers =
        section ? static_cast<std::size_t>(std::ranges::count_if(
                      section->cards,
                      [](const auto &card) {
                        return card.kind == middle::CardKind::UserMessage;
                      }))
                : 0;
    result &= require(
        section && projectedUsers == fixture.userCount &&
            section->cards.size() == fixture.itemCount,
        "large complete projection omitted a loaded graph row");
  }

  std::cout << "complete conversation projection us (10k/40k): "
            << medians[0].count() << " / " << medians[1].count() << '\n';
  return result &&
         require(medians[1] <= std::chrono::seconds(1),
                 "40k complete projection exceeded its absolute bound") &&
         require(medians[1] <= medians[0] * 8 + std::chrono::milliseconds(10),
                 "complete projection scaled beyond its linear allowance");
}

bool projectsCanonicalAgentActivityLifecycle() {
  struct LifecycleCase {
    const char *id;
    const char *kind;
    nodegraph::NodeStatus graphStatus = nodegraph::NodeStatus::Unknown;
    const char *fieldStatus = nullptr;
    nodegraph::NodeStatus expected;
  };
  const LifecycleCase cases[] = {
      {"started", "started", nodegraph::NodeStatus::Unknown, nullptr,
       nodegraph::NodeStatus::Running},
      {"progress", "progress", nodegraph::NodeStatus::Unknown, nullptr,
       nodegraph::NodeStatus::Running},
      {"completed", "completed", nodegraph::NodeStatus::Unknown, nullptr,
       nodegraph::NodeStatus::Completed},
      {"failed", "failed", nodegraph::NodeStatus::Unknown, nullptr,
       nodegraph::NodeStatus::Failed},
      {"interrupted", "interrupted", nodegraph::NodeStatus::Unknown, nullptr,
       nodegraph::NodeStatus::Interrupted},
      {"published-running", "completed", nodegraph::NodeStatus::Running,
       "failed", nodegraph::NodeStatus::Running},
      {"field-running", "", nodegraph::NodeStatus::Unknown, "inProgress",
       nodegraph::NodeStatus::Unknown},
      {"missing", "", nodegraph::NodeStatus::Unknown, nullptr,
       nodegraph::NodeStatus::Unknown},
  };

  nodegraph::NodeGraph graph;
  NodeRef thread;
  std::vector<NodeRef> items;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "agent-lifecycle-thread"});
    const NodeRef turn = write.upsert({NodeKind::Turn, "agent-lifecycle-turn"});
    write.setParent(thread, turn);
    for (const LifecycleCase &value : cases) {
      NodeState state;
      state.status = value.graphStatus;
      state.fields.emplace("id", value.id);
      state.fields.emplace("type", "subAgentActivity");
      state.fields.emplace("tool", "spawn_agent");
      if (*value.kind)
        state.fields.emplace("kind", value.kind);
      if (value.fieldStatus)
        state.fields.emplace("status", value.fieldStatus);
      NodeRef item = write.upsert({NodeKind::Item, value.id}, std::move(state));
      write.setParent(turn, item);
      items.push_back(std::move(item));
    }
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto delta = adapter.conversationDelta(thread, items, false);
  bool result = require(delta.has_value(),
                        "agent lifecycle delta was unavailable");
  for (std::size_t index = 0; index < std::size(cases); ++index) {
    const middle::VisibleCardData *card =
        delta && index < delta->presentations.size()
            ? &delta->presentations[index]
            : nullptr;
    const auto *activity =
        card ? std::get_if<middle::AgentActivityData>(&card->payload) : nullptr;
    result &=
        require(activity && card->status.semantic == cases[index].expected,
                "agent activity lost its canonical lifecycle");
  }
  return result;
}

bool boundedOptimisticPromptAdoptsProviderOrderInPlace() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef root;
  NodeRef local;
  NodeRef answer;
  {
    auto write = graph.write();
    NodeState threadState;
    thread = write.upsert({NodeKind::Thread, "bounded-prompt-thread"},
                          std::move(threadState));
    turn = write.upsert({NodeKind::Turn, "bounded-prompt-turn"});
    write.setField(turn, "id", "bounded-prompt-turn");
    root = write.upsert(
        {NodeKind::Item, "bounded-prompt-root"},
        itemState("bounded-prompt-root", "userMessage", "Opening prompt"));
    NodeState localState =
        itemState("bounded-local-steering", "localPrompt", "Steer here");
    localState.fields.emplace("local", true);
    localState.fields.emplace("submissionId", std::uint64_t{77});
    localState.fields.emplace("dispatchState", "dispatching");
    localState.fields.emplace("startsTurn", false);
    local = write.upsert({NodeKind::Item, "bounded-local-steering"},
                         std::move(localState));
    answer = write.upsert(
        {NodeKind::Item, "bounded-prompt-answer"},
        itemState("bounded-prompt-answer", "agentMessage", "First answer"));
    write.setParent(thread, turn);
    write.setParent(turn, root);
    write.setParent(turn, local);
    write.setParent(turn, answer);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    write.relate(thread, nodegraph::RelationKind::PendingPrompt, local);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto optimistic = adapter.conversation(thread);
  const middle::CardKey rootKey = middle::AuthoritativeItemKey{
      "bounded-prompt-thread", "bounded-prompt-turn", "bounded-prompt-root"};
  const middle::CardKey steeringKey = middle::LocalPromptKey{77};
  const middle::CardKey answerKey = middle::AuthoritativeItemKey{
      "bounded-prompt-thread", "bounded-prompt-turn", "bounded-prompt-answer"};
  if (!require(optimistic && optimistic->sections.size() == 1 &&
                   optimistic->sections.front().cards.size() == 3,
               "bounded optimistic steering projection was unavailable") ||
      !require(optimistic->sections.front().cards[0].key == rootKey &&
                   optimistic->sections.front().cards[1].key == steeringKey &&
                   optimistic->sections.front().cards[2].key == answerKey,
               "bounded projection moved an optimistic steering prompt out "
               "of its canonical sibling slot"))
    return false;

  NodeRef authoritative;
  {
    auto write = graph.write();
    write.setField(local, "dispatchState", "awaitingMaterialization");
    authoritative =
        write.upsert({NodeKind::Item, "bounded-authoritative-steering"},
                     itemState("bounded-authoritative-steering", "userMessage",
                               "Steer here"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{77});
    write.setParent(turn, authoritative);
    write.relate(authoritative, nodegraph::RelationKind::PromptMaterialization,
                 local);
    write.replaceChildren(
        turn, std::array<NodeRef, 4>{root, local, answer, authoritative});
    static_cast<void>(write.finish());
  }

  const auto materialized = adapter.conversation(thread);
  return require(materialized && materialized->sections.size() == 1 &&
                     materialized->sections.front().cards.size() == 3,
                 "bounded steering materialization was unavailable") &&
         require(materialized->sections.front().cards[0].key == rootKey &&
                     materialized->sections.front().cards[1].key == answerKey &&
                     materialized->sections.front().cards[2].key ==
                         steeringKey,
                 "steering materialization did not adopt provider row order") &&
         require(materialized->sections.front().cards[2].kind ==
                         middle::CardKind::UserMessage &&
                     materialized->sections.front().cards[2].target ==
                         authoritative,
                 "steering materialization did not adopt its authoritative "
                 "target through the stable presentation identity");
}

bool controllerAndObserverShareProviderConversationOrder() {
  nodegraph::NodeGraph controllerGraph;
  nodegraph::NodeGraph observerGraph;
  const auto prepare = [](nodegraph::NodeGraph &graph, bool controller) {
    auto write = graph.write();
    NodeRef thread =
        write.upsert({NodeKind::Thread, "shared-provider-order-thread"});
    NodeRef turn = write.upsert(nodegraph::scopedTurnNodeId(
        "shared-provider-order-thread", "shared-provider-order-turn"));
    write.setField(turn, "protocolId", "shared-provider-order-turn");
    write.setField(turn, "protocolThreadId", "shared-provider-order-thread");
    NodeRef root = write.upsert(
        nodegraph::scopedItemNodeId(turn->id(), "provider-root"),
        itemState("provider-root", "userMessage", "Opening prompt"));
    NodeRef activity = write.upsert(
        nodegraph::scopedItemNodeId(turn->id(), "provider-activity"),
        itemState("provider-activity", "agentMessage", "Provider activity"));
    write.setField(root, "protocolId", "provider-root");
    write.setField(root, "protocolTurnId", "shared-provider-order-turn");
    write.setField(activity, "protocolId", "provider-activity");
    write.setField(activity, "protocolTurnId", "shared-provider-order-turn");
    write.setParent(thread, turn);
    write.setParent(turn, root);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    if (controller) {
      NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
      NodeState localState =
          itemState("private-local-prompt", "localPrompt", "Steer here");
      localState.fields.emplace("local", true);
      localState.fields.emplace("submissionId", std::uint64_t{404});
      localState.fields.emplace("clientUserMessageId", "shared-client-id");
      localState.fields.emplace("dispatchState", "awaitingMaterialization");
      localState.fields.emplace("startsTurn", false);
      NodeRef local = write.upsert({NodeKind::Item, "private-local-prompt"},
                                   std::move(localState));
      write.setParent(turn, local);
      write.relate(runtime, nodegraph::RelationKind::PendingPrompt, local);
      write.relate(thread, nodegraph::RelationKind::PendingPrompt, local);
    }
    write.setParent(turn, activity);
    static_cast<void>(write.finish());
    return thread;
  };

  const NodeRef controllerThread = prepare(controllerGraph, true);
  const NodeRef observerThread = prepare(observerGraph, false);
  nodegraph::ProtocolUpdater controllerUpdater(controllerGraph);
  nodegraph::ProtocolUpdater observerUpdater(observerGraph);
  const auto materialization = [] {
    return nodegraph::DecodedMessage{
        nodegraph::DecodedMessageKind::ServerNotification, "item/started",
        std::nullopt,
        nodegraph::Value::Object{
            {"threadId", nodegraph::Value("shared-provider-order-thread")},
            {"turnId", nodegraph::Value("shared-provider-order-turn")},
            {"item",
             nodegraph::Value(nodegraph::Value::Object{
                 {"id", nodegraph::Value("provider-steering")},
                 {"type", nodegraph::Value("userMessage")},
                 {"clientId", nodegraph::Value("shared-client-id")},
                 {"text", nodegraph::Value("Steer here")}})}}};
  };
  static_cast<void>(controllerUpdater.apply(materialization()));
  static_cast<void>(observerUpdater.apply(materialization()));

  NodeGraphUiAdapter controllerAdapter(controllerGraph);
  NodeGraphUiAdapter observerAdapter(observerGraph);
  const auto signature = [](const middle::ConversationSnapshot &snapshot) {
    std::vector<std::string> result;
    for (const middle::TurnSection &section : snapshot.sections) {
      std::optional<std::size_t> rootPosition;
      if (section.rootCardKey) {
        const auto root = std::ranges::find_if(
            section.cards, [&](const middle::VisibleCardData &card) {
              return card.key == *section.rootCardKey;
            });
        if (root != section.cards.end())
          rootPosition = static_cast<std::size_t>(root - section.cards.begin());
      }
      for (std::size_t index = 0; index < section.cards.size(); ++index) {
        const middle::VisibleCardData &card = section.cards[index];
        const bool root = rootPosition && *rootPosition == index;
        result.push_back(
            std::to_string(static_cast<int>(card.kind)) + ':' + card.itemId +
            ':' + (root ? "root"
                       : (middle::isNestedTurnCard(rootPosition, index)
                              ? "nested"
                              : "standalone")));
      }
    }
    return result;
  };
  const auto controllerNotification =
      controllerAdapter.conversation(controllerThread);
  const auto observerNotification =
      observerAdapter.conversation(observerThread);
  const std::vector<std::string> notificationOrder{
      std::to_string(static_cast<int>(middle::CardKind::UserMessage)) +
          ":provider-root:root",
      std::to_string(static_cast<int>(middle::CardKind::AgentMessage)) +
          ":provider-activity:nested",
      std::to_string(static_cast<int>(middle::CardKind::UserMessage)) +
          ":provider-steering:nested"};
  const std::vector<std::string> controllerOrder =
      controllerNotification ? signature(*controllerNotification)
                             : std::vector<std::string>{};
  if (!require(controllerNotification && observerNotification,
               "controller/observer notification projections were unavailable") ||
      !require(controllerOrder == notificationOrder,
               "the controller did not project provider notification order") ||
      !require(signature(*observerNotification) == notificationOrder,
               "the observer did not project provider notification order"))
    return false;

  const auto applySnapshot = [](nodegraph::ProtocolUpdater &updater,
                                std::string requestId) {
    const nodegraph::ProtocolRequestId id(std::move(requestId));
    const nodegraph::ApplyResult request = updater.apply(
        {nodegraph::DecodedMessageKind::ClientRequest, "thread/read", id,
         nodegraph::Value::Object{
             {"threadId",
              nodegraph::Value("shared-provider-order-thread")}}});
    nodegraph::Value::Array items{
        nodegraph::Value(nodegraph::Value::Object{
            {"id", nodegraph::Value("provider-root")},
            {"type", nodegraph::Value("userMessage")}}),
        nodegraph::Value(nodegraph::Value::Object{
            {"id", nodegraph::Value("provider-activity")},
            {"type", nodegraph::Value("agentMessage")}}),
        nodegraph::Value(nodegraph::Value::Object{
            {"id", nodegraph::Value("replacement-activity")},
            {"type", nodegraph::Value("agentMessage")}}),
        nodegraph::Value(nodegraph::Value::Object{
            {"id", nodegraph::Value("provider-steering")},
            {"type", nodegraph::Value("userMessage")},
            {"clientId", nodegraph::Value("shared-client-id")},
            {"text", nodegraph::Value("Steer here")}})};
    nodegraph::Value::Object turn{
        {"id", nodegraph::Value("shared-provider-order-turn")},
        {"items", nodegraph::Value(std::move(items))}};
    nodegraph::Value::Object thread{
        {"id", nodegraph::Value("shared-provider-order-thread")},
        {"turns", nodegraph::Value(nodegraph::Value::Array{
                      nodegraph::Value(std::move(turn))})}};
    nodegraph::DecodedMessage result{
        nodegraph::DecodedMessageKind::ClientResult, "thread/read", id,
        nodegraph::Value::Object{
            {"thread", nodegraph::Value(std::move(thread))}}};
    result.expectedNode = request.primary;
    static_cast<void>(updater.apply(std::move(result)));
  };
  applySnapshot(controllerUpdater, "controller-provider-order-snapshot");
  applySnapshot(observerUpdater, "observer-provider-order-snapshot");

  const auto controllerSnapshot =
      controllerAdapter.conversation(controllerThread);
  const auto observerSnapshot = observerAdapter.conversation(observerThread);
  return require(controllerSnapshot && observerSnapshot,
                 "controller/observer snapshot projections were unavailable") &&
         require(signature(*controllerSnapshot) ==
                         signature(*observerSnapshot) &&
                     signature(*controllerSnapshot) ==
                         std::vector<std::string>{
                             notificationOrder[0], notificationOrder[1],
                             std::to_string(static_cast<int>(
                                 middle::CardKind::AgentMessage)) +
                                 ":replacement-activity:nested",
                             notificationOrder[2]},
                 "a private optimistic prompt changed provider snapshot order "
                 "or root-relative nesting");
}

bool loadedSteeringPromptsKeepCanonicalOrder() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef root;
  NodeRef older;
  NodeRef newer;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "bounded-steering-thread"});
    turn = write.upsert({NodeKind::Turn, "bounded-steering-turn"});
    write.setField(turn, "id", "bounded-steering-turn");
    write.setParent(thread, turn);
    root = write.upsert(
        {NodeKind::Item, "bounded-steering-root"},
        itemState("bounded-steering-root", "userMessage", "Opening prompt"));
    write.setParent(turn, root);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);

    const auto appendActivity = [&](int index) {
      const std::string id =
          "bounded-steering-activity-" + std::to_string(index);
      NodeRef activity =
          write.upsert({NodeKind::Item, id}, itemState(id, "agentMessage", id));
      write.setParent(turn, activity);
    };
    for (int index = 0; index < 7; ++index)
      appendActivity(index);

    NodeState olderState = itemState("bounded-steering-older", "localPrompt",
                                     "Populate the plan tab");
    olderState.fields.emplace("local", true);
    olderState.fields.emplace("submissionId", std::uint64_t{101});
    olderState.fields.emplace("dispatchState", "inFlight");
    olderState.fields.emplace("startsTurn", false);
    older = write.upsert({NodeKind::Item, "bounded-steering-older"},
                         std::move(olderState));
    write.setParent(turn, older);
    write.relate(thread, nodegraph::RelationKind::PendingPrompt, older);

    for (int index = 7; index < 79; ++index)
      appendActivity(index);

    NodeState newerState =
        itemState("bounded-steering-newer", "localPrompt", "New steering");
    newerState.fields.emplace("local", true);
    newerState.fields.emplace("submissionId", std::uint64_t{102});
    newerState.fields.emplace("dispatchState", "inFlight");
    newerState.fields.emplace("startsTurn", false);
    newer = write.upsert({NodeKind::Item, "bounded-steering-newer"},
                         std::move(newerState));
    write.setParent(turn, newer);
    write.relate(thread, nodegraph::RelationKind::PendingPrompt, newer);
    for (int index = 79; index < 83; ++index)
      appendActivity(index);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto projected = adapter.conversation(thread);
  if (!require(projected && projected->sections.size() == 1,
               "complete multi-steering projection was unavailable"))
    return false;
  const auto &cards = projected->sections.front().cards;
  const auto olderPosition = std::ranges::find_if(cards, [](const auto &card) {
    return card.key == middle::CardKey{middle::LocalPromptKey{101}};
  });
  const auto newerPosition = std::ranges::find_if(cards, [](const auto &card) {
    return card.key == middle::CardKey{middle::LocalPromptKey{102}};
  });
  const auto firstRetainedActivity =
      std::ranges::find_if(cards, [](const auto &card) {
        return card.itemId == "bounded-steering-activity-75";
      });
  const auto firstPostNewer = std::ranges::find_if(cards, [](const auto &card) {
    return card.itemId == "bounded-steering-activity-79";
  });
  return require(cards.size() == 86 && olderPosition != cards.end() &&
                     newerPosition != cards.end() &&
                     firstRetainedActivity != cards.end() &&
                     firstPostNewer != cards.end(),
                 "complete projection lost a loaded steering row") &&
         require(olderPosition < firstRetainedActivity &&
                     firstRetainedActivity < newerPosition &&
                     newerPosition < firstPostNewer,
                 "complete projection changed steering submission order");
}

bool projectsCanonicalRowsThroughOneDelta() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef root;
  NodeRef tail;
  {
    auto write = graph.write();
    NodeState threadState;
    threadState.fields.emplace("historyHasMore", true);
    thread =
        write.upsert({NodeKind::Thread, "tail-thread"}, std::move(threadState));
    NodeState turnState;
    turnState.fields.emplace("id", "tail-turn");
    turnState.status = nodegraph::NodeStatus::Running;
    turn = write.upsert({NodeKind::Turn, "tail-turn"}, std::move(turnState));
    root = write.upsert({NodeKind::Item, "tail-root"},
                        itemState("tail-root", "userMessage", "Question"));
    tail = write.upsert({NodeKind::Item, "tail-answer"},
                        itemState("tail-answer", "agentMessage", "Answer"));
    write.setParent(thread, turn);
    write.setParent(turn, root);
    write.setParent(turn, tail);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    write.relate(thread, nodegraph::RelationKind::ActiveTurn, turn);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const std::array<NodeRef, 1> tailItems{tail};
  const auto projected = adapter.conversationDelta(thread, tailItems, true);
  const middle::ConversationRowChange *tailRow =
      projected && projected->rows.size() == 1 ? &projected->rows.front()
                                               : nullptr;
  const std::array<NodeRef, 1> rootItems{root};
  const auto rootProjection =
      adapter.conversationDelta(thread, rootItems, true);
  const middle::ConversationRowChange *rootRow =
      rootProjection && rootProjection->rows.size() == 1
          ? &rootProjection->rows.front()
          : nullptr;
  const auto snapshot = adapter.conversation(thread);
  const middle::TurnSection *section =
      snapshot && snapshot->sections.size() == 1 ? &snapshot->sections.front()
                                                 : nullptr;
  return require(projected.has_value(),
                 "canonical last item was not projected") &&
         require(tailRow && tailRow->placement.card.target == tail &&
                     !tailRow->placement.turnRoot &&
                     tailRow->placement.nested &&
                     tailRow->placement.activeTurn && !tailRow->nextCardKey,
                 "delta projection lost exact tail placement") &&
         require(projected->providerHasMore,
                 "delta projection lost provider history continuation") &&
         require(rootRow && rootRow->placement.turnRoot &&
                     !rootRow->placement.nested &&
                     rootRow->nextCardKey.has_value(),
                 "the same delta contract lost non-tail root placement") &&
         require(section && section->cards.size() == 2 &&
                     section->cards.front() == rootRow->placement.card &&
                     section->cards.back() == tailRow->placement.card &&
                     section->key == rootRow->placement.sectionKey &&
                     section->key == tailRow->placement.sectionKey &&
                     snapshot->activeTurnId == section->turnId,
                 "full and exact projection paths disagreed on card, section, "
                 "or active-turn presentation facts");
}

bool projectsExactPromptMaterialization() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef localTurn;
  NodeRef providerTurn;
  NodeRef prompt;
  NodeRef authoritative;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "prompt-thread"});
    localTurn = write.upsert({NodeKind::Turn, "local-prompt-turn"});
    write.setField(localTurn, "id", "local-prompt-turn");
    providerTurn = write.upsert({NodeKind::Turn, "provider-prompt-turn"});
    write.setField(providerTurn, "id", "provider-prompt-turn");
    NodeState local = itemState("local-prompt", "localPrompt", "hello");
    local.fields.emplace("submissionId", std::uint64_t{91});
    local.fields.emplace("dispatchState", "inFlight");
    prompt = write.upsert({NodeKind::Item, "local-prompt"}, std::move(local));
    authoritative =
        write.upsert({NodeKind::Item, "provider-prompt"},
                     itemState("provider-prompt", "userMessage", "hello"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{91});
    write.setParent(thread, localTurn);
    write.setParent(thread, providerTurn);
    write.setParent(localTurn, prompt);
    write.relate(localTurn, nodegraph::RelationKind::TurnRootItem, prompt);
    write.setParent(providerTurn, authoritative);
    write.relate(providerTurn, nodegraph::RelationKind::TurnRootItem,
                 authoritative);
    write.relate(authoritative, nodegraph::RelationKind::PromptMaterialization,
                 prompt);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const std::array<NodeRef, 2> changed{authoritative, prompt};
  const auto result = adapter.conversationDelta(thread, changed, true);
  const middle::ConversationRowChange *row =
      result && result->rows.size() == 1 ? &result->rows.front() : nullptr;
  const auto snapshot = adapter.conversation(thread);
  const middle::VisibleCardData *snapshotCard =
      snapshot && snapshot->sections.size() == 1 &&
              snapshot->sections.front().cards.size() == 1
          ? &snapshot->sections.front().cards.front()
          : nullptr;
  return require(result.has_value(),
                 "early exact prompt materialization was not projected") &&
         require(row && row->placement.card.key ==
                            middle::CardKey{middle::LocalPromptKey{91}},
                 "early prompt materialization changed the stable local key") &&
         require(row->placement.card.kind == middle::CardKind::UserMessage &&
                     row->placement.card.target == authoritative &&
                     row->placement.turnRoot &&
                     result->removals == std::vector<NodeRef>{prompt},
                 "one delta did not move the stable presentation from its "
                 "provisional Turn to the authoritative provider Turn") &&
         require(snapshotCard && *snapshotCard == row->placement.card,
                 "full and exact projection paths disagreed on promoted "
                 "prompt identity or presentation");
}

bool projectsExactRowPlacementAndNeighbors() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef first;
  NodeRef moved;
  NodeRef last;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "row-thread"});
    turn = write.upsert({NodeKind::Turn, "row-turn"});
    write.setField(turn, "id", "row-turn");
    first = write.upsert({NodeKind::Item, "row-first"},
                         itemState("first", "userMessage", "first"));
    moved = write.upsert({NodeKind::Item, "row-moved"},
                         itemState("moved", "agentMessage", "moved"));
    last = write.upsert({NodeKind::Item, "row-last"},
                        itemState("last", "agentMessage", "last"));
    write.setParent(thread, turn);
    write.setParent(turn, first);
    write.setParent(turn, moved);
    write.setParent(turn, last);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, first);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const std::array<NodeRef, 1> changed{moved};
  auto delta = adapter.conversationDelta(thread, changed, true);
  const middle::ConversationRowChange *placement =
      delta && delta->rows.size() == 1 ? &delta->rows.front() : nullptr;
  const middle::CardKey firstKey =
      middle::AuthoritativeItemKey{"row-thread", "row-turn", "row-first"};
  const middle::CardKey lastKey =
      middle::AuthoritativeItemKey{"row-thread", "row-turn", "row-last"};
  bool result = require(
      placement && placement->placement.card.target == moved &&
          placement->placement.sectionKey == "turn:10:row-thread8:row-turn" &&
          placement->placement.nested && !placement->placement.turnRoot &&
          placement->previousCardKey == firstKey &&
          placement->nextCardKey == lastKey,
      "one row projection lost its exact placement or neighbors");

  {
    auto write = graph.write();
    write.replaceChildren(turn, std::array<NodeRef, 3>{first, last, moved});
    static_cast<void>(write.finish());
  }
  delta = adapter.conversationDelta(thread, changed, true);
  placement = delta && delta->rows.size() == 1 ? &delta->rows.front() : nullptr;
  result &= require(placement && placement->previousCardKey == lastKey &&
                        !placement->nextCardKey,
                    "a canonical move did not update the exact row neighbors");
  {
    auto write = graph.write();
    const auto busy = adapter.conversationDelta(thread, changed, true);
    result &=
        require(!busy, "row projection distinguishes graph contention from an "
                       "authoritative removal");
    static_cast<void>(write.finish());
  }
  return result;
}

bool ordersStructuralRowsByCanonicalDependencies() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef left;
  NodeRef right;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "ordered-delta-thread"});
    turn = write.upsert({NodeKind::Turn, "ordered-delta-turn"});
    write.setField(turn, "id", "ordered-delta-turn");
    left = write.upsert({NodeKind::Item, "ordered-left"},
                        itemState("ordered-left", "userMessage", "left"));
    right = write.upsert({NodeKind::Item, "ordered-right"},
                         itemState("ordered-right", "agentMessage", "right"));
    write.setParent(thread, turn);
    write.setParent(turn, left);
    write.setParent(turn, right);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, left);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto initial = adapter.conversation(thread);
  NodeRef first;
  NodeRef second;
  NodeRef third;
  {
    auto write = graph.write();
    first = write.upsert({NodeKind::Item, "ordered-first"},
                         itemState("ordered-first", "agentMessage", "first"));
    second =
        write.upsert({NodeKind::Item, "ordered-second"},
                     itemState("ordered-second", "agentMessage", "second"));
    third = write.upsert({NodeKind::Item, "ordered-third"},
                         itemState("ordered-third", "agentMessage", "third"));
    write.setParent(turn, first);
    write.setParent(turn, second);
    write.setParent(turn, third);
    write.replaceChildren(
        turn, std::array<NodeRef, 5>{left, first, second, third, right});
    static_cast<void>(write.finish());
  }

  const std::array<NodeRef, 3> reverseDependencies{second, third, first};
  const auto delta =
      adapter.conversationDelta(thread, reverseDependencies, true);
  const auto complete = adapter.conversation(thread);
  const std::array<NodeRef, 3> canonical{first, second, third};
  bool result =
      require(initial && initial->sections.size() == 1 &&
                  initial->sections.front().cards.size() == 2,
              "ordered-delta fixture lost its initial retained anchors") &&
      require(delta && delta->rows.size() == canonical.size() &&
                  delta->removals.empty(),
              "ordered-delta projection did not retain all changed rows") &&
      require(complete && complete->sections.size() == 1 &&
                  complete->sections.front().cards.size() == 5,
              "ordered-delta full projection was unavailable");
  for (std::size_t index = 0; result && index < canonical.size(); ++index) {
    const middle::ConversationRowChange &row = delta->rows[index];
    const middle::VisibleCardData &snapshotCard =
        complete->sections.front().cards[index + 1];
    result &= require(row.placement.card.target == canonical[index],
                      "structural rows retained caller order instead of "
                      "canonical dependency order");
    result &= require(row.placement.card == snapshotCard,
                      "incremental and complete projection disagreed on a "
                      "dependency-ordered row");
  }
  return result;
}

bool routesRelationsIncrementallyAndTurnReordersAuthoritatively() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef firstTurn;
  NodeRef secondTurn;
  NodeRef firstRoot;
  NodeRef secondRoot;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "route-thread"});
    firstTurn = write.upsert({NodeKind::Turn, "route-turn-1"});
    secondTurn = write.upsert({NodeKind::Turn, "route-turn-2"});
    firstRoot = write.upsert({NodeKind::Item, "route-root-1"},
                             itemState("route-root-1", "userMessage", "one"));
    secondRoot = write.upsert({NodeKind::Item, "route-root-2"},
                              itemState("route-root-2", "userMessage", "two"));
    write.setParent(thread, firstTurn);
    write.setParent(thread, secondTurn);
    write.setParent(firstTurn, firstRoot);
    write.setParent(secondTurn, secondRoot);
    write.relate(firstTurn, nodegraph::RelationKind::TurnRootItem, firstRoot);
    write.relate(secondTurn, nodegraph::RelationKind::TurnRootItem, secondRoot);
    write.relate(thread, nodegraph::RelationKind::ActiveTurn, firstTurn);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  nodegraph::GraphChange graphChange;
  {
    auto write = graph.write();
    const std::array active{secondTurn};
    write.replaceRelated(thread, nodegraph::RelationKind::ActiveTurn, active);
    graphChange = write.finish();
  }
  nodegraph::GraphChanged changed{graphChange.revision,
                                  std::move(graphChange.affected),
                                  std::move(graphChange.removed), false,
                                  std::move(graphChange.childListsChanged)};
  auto route = adapter.conversationRoute(changed, thread);
  bool result = require(
      route.affected && route.structural && !route.authorityReplacement &&
          route.items.size() == 2 &&
          std::ranges::find(route.items, firstRoot) != route.items.end() &&
          std::ranges::find(route.items, secondRoot) != route.items.end(),
      "an ActiveTurn relation change replaced the conversation instead of "
      "updating the two authoritative roots");

  {
    auto write = graph.write();
    const std::array reordered{secondTurn, firstTurn};
    write.replaceChildren(thread, reordered);
    graphChange = write.finish();
  }
  nodegraph::GraphChanged reordered{graphChange.revision,
                                    std::move(graphChange.affected),
                                    std::move(graphChange.removed), false,
                                    std::move(graphChange.childListsChanged)};
  NodeRef session;
  nodegraph::GraphChanged sessionChanged;
  {
    auto write = graph.write();
    session = write.upsert({NodeKind::RealtimeSession, "route-session"});
    write.setParent(thread, session);
    graphChange = write.finish();
  }
  sessionChanged = {graphChange.revision, std::move(graphChange.affected),
                    std::move(graphChange.removed), false,
                    std::move(graphChange.childListsChanged)};
  {
    auto write = graph.write();
    const std::array active{firstTurn};
    write.replaceRelated(thread, nodegraph::RelationKind::ActiveTurn, active);
    graphChange = write.finish();
  }
  changed = {graphChange.revision, std::move(graphChange.affected),
             std::move(graphChange.removed), false,
             std::move(graphChange.childListsChanged)};
  route = adapter.conversationRoute(reordered, thread);
  result &= require(route.affected && route.structural &&
                        route.authorityReplacement && route.items.empty(),
                    "a retained Turn reorder masked by later child and "
                    "relation revisions was not routed through authoritative "
                    "ordering");
  route = adapter.conversationRoute(sessionChanged, thread);
  result &= require(!route.affected && !route.structural &&
                        !route.authorityReplacement && route.items.empty(),
                    "a later non-Turn Thread child event was mistaken for "
                    "conversation topology");
  route = adapter.conversationRoute(changed, thread);
  result &=
      require(route.affected && route.structural && !route.authorityReplacement,
              "the relation revision after a reorder did not remain "
              "an exact incremental update");

  NodeRef childThread;
  {
    auto write = graph.write();
    childThread = write.upsert({NodeKind::Thread, "route-child-thread"});
    write.relate(thread, nodegraph::RelationKind::AgentChildThread,
                 childThread);
    graphChange = write.finish();
  }
  changed = {graphChange.revision, std::move(graphChange.affected),
             std::move(graphChange.removed), false,
             std::move(graphChange.childListsChanged)};
  route = adapter.conversationRoute(changed, thread);
  result &= require(!route.affected && !route.structural &&
                        !route.authorityReplacement && route.items.empty(),
                    "a child-thread aggregate relation was mistaken for "
                    "conversation topology");

  NodeRef otherThread;
  {
    auto write = graph.write();
    otherThread = write.upsert({NodeKind::Thread, "route-other-thread"});
    write.setParent(otherThread, firstTurn);
    graphChange = write.finish();
  }
  changed = {graphChange.revision, std::move(graphChange.affected),
             std::move(graphChange.removed), false,
             std::move(graphChange.childListsChanged)};
  route = adapter.conversationRoute(changed, thread);
  result &= require(route.affected && route.structural &&
                        route.authorityReplacement && route.items.empty(),
                    "a Turn moved out of the selected Thread left stale "
                    "conversation rows");

  return result;
}

bool preservesReadinessActivityAndProviderPaginationSemantics() {
  nodegraph::NodeGraph graph;
  NodeRef runtime;
  NodeRef thread;
  {
    auto write = graph.write();
    runtime = write.upsert({NodeKind::Runtime, "runtime"});
    NodeState connection;
    connection.status = nodegraph::NodeStatus::Disconnected;
    connection.fields = {{"transportState", "disconnected"},
                         {"providerState", "ready"},
                         {"role", "controller"}};
    static_cast<void>(write.upsert({NodeKind::Connection, "connection"},
                                   std::move(connection)));
    NodeState threadState;
    threadState.fields = {{"name", "Provider thread"},
                          {"localNameOverlay", "Chosen thread"},
                          {"creationCorrelation", "qt-draft:test"},
                          {"updatedAt", std::int64_t{4}},
                          {"recencyAt", std::int64_t{6}},
                          {"lastActivityAt", std::int64_t{5}},
                          {"localActivityAt", std::int64_t{9}},
                          {"localPromptActivityAt", std::int64_t{8}},
                          {"hydrationState", "loading"},
                          {"historyHasMore", true}};
    thread = write.upsert({NodeKind::Thread, "compatibility-thread"},
                          std::move(threadState));
    const NodeRef turn = write.upsert({NodeKind::Turn, "compatibility-turn"});
    write.setParent(thread, turn);
    const NodeRef provider =
        write.upsert({NodeKind::Item, "provider-item"},
                     itemState("provider-item", "agentMessage", "provider"));
    write.setParent(turn, provider);
    NodeState local = itemState("local-item", "localPrompt", "local");
    local.fields.emplace("dispatchState", "inFlight");
    local.fields.emplace("admittedAtMs", std::int64_t{1234});
    const NodeRef localPrompt =
        write.upsert({NodeKind::Item, "local-item"}, std::move(local));
    write.setParent(turn, localPrompt);
    write.relate(thread, nodegraph::RelationKind::PendingPrompt, localPrompt);
    write.relate(runtime, nodegraph::RelationKind::RootThread, thread);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto info = adapter.conversationInfo(thread);
  const auto threads = adapter.threads(thread);
  bool result =
      require(info && !info->readyForDisplay && !info->hydrationFailed &&
                  info->providerHasMore,
              "hydration or provider pagination semantics changed") &&
      require(threads && !threads->providerReady && !threads->canControl,
              "disconnected transport exposed ready thread controls") &&
      require(threads && threads->roots.size() == 1 &&
                  threads->roots.front().presentationKey ==
                      "creation:qt-draft:test" &&
                  threads->roots.front().title == "Chosen thread" &&
                  threads->roots.front().recencyAt ==
                      std::optional<std::int64_t>{8} &&
                  threads->roots.front().lastActivityAt ==
                      std::optional<std::int64_t>{9} &&
                  threads->roots.front().awaitingPromptAcknowledgement &&
                  threads->roots.front().pendingPromptAdmittedAtMs ==
                      std::optional<std::int64_t>{1234},
              "presentation identity, chosen-name, Recent, activity, or "
              "prompt lifecycle projection changed");

  return result;
}

bool projectsHistoryRequestLifecycleFromTheGraph() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  {
    auto write = graph.write();
    NodeState threadState;
    threadState.fields = {{"hydrationState", "ready"},
                          {"historyHasMore", true}};
    thread = write.upsert({NodeKind::Thread, "history-pending-thread"},
                          std::move(threadState));
    static_cast<void>(write.finish());
  }
  NodeGraphUiAdapter adapter(graph);
  bool result = true;

  nodegraph::GraphChange graphChange;
  NodeRef unrelated;
  {
    auto write = graph.write();
    NodeState state;
    state.status = nodegraph::NodeStatus::Pending;
    state.fields = {
        {"method", "thread/name/set"},
        {"requestPayload",
         nodegraph::Value::Object{{"threadId", "history-pending-thread"}}}};
    unrelated = write.upsert({NodeKind::Operation, "unrelated-operation"},
                             std::move(state));
    write.relate(thread, nodegraph::RelationKind::PendingOperation, unrelated);
    graphChange = write.finish();
  }
  nodegraph::GraphChanged changed{graphChange.revision,
                                  std::move(graphChange.affected),
                                  std::move(graphChange.removed), false,
                                  std::move(graphChange.childListsChanged)};
  auto route = adapter.conversationRoute(changed, thread);
  auto info = adapter.conversationInfo(thread);
  result &= require(!route.historyRequestPending && info &&
                        !info->historyRequestPending,
                    "an unrelated operation became history busy state");

  NodeRef history;
  {
    auto write = graph.write();
    write.remove(unrelated);
    NodeState state;
    state.status = nodegraph::NodeStatus::Pending;
    state.fields = {
        {"method", "thread/read"},
        {"requestPayload",
         nodegraph::Value::Object{{"threadId", "history-pending-thread"}}}};
    history = write.upsert({NodeKind::Operation, "history-operation"},
                           std::move(state));
    write.relate(thread, nodegraph::RelationKind::PendingOperation, history);
    graphChange = write.finish();
  }
  changed = {graphChange.revision, std::move(graphChange.affected),
             std::move(graphChange.removed), false,
             std::move(graphChange.childListsChanged)};
  route = adapter.conversationRoute(changed, thread);
  info = adapter.conversationInfo(thread);
  result &= require(route.historyRequestPending == std::optional<bool>{true} &&
                        !route.affected && !route.structural && info &&
                        info->historyRequestPending,
                    "a live thread/read snapshot did not disable Load More");

  NodeRef replacement;
  {
    auto write = graph.write();
    write.remove(history);
    NodeState state;
    state.status = nodegraph::NodeStatus::Pending;
    state.fields = {
        {"method", "thread/turns/list"},
        {"requestPayload",
         nodegraph::Value::Object{{"threadId", "history-pending-thread"}}}};
    replacement =
        write.upsert({NodeKind::Operation, "replacement-history-operation"},
                     std::move(state));
    write.relate(thread, nodegraph::RelationKind::PendingOperation,
                 replacement);
    graphChange = write.finish();
  }
  changed = {graphChange.revision, std::move(graphChange.affected),
             std::move(graphChange.removed), false,
             std::move(graphChange.childListsChanged)};
  route = adapter.conversationRoute(changed, thread);
  info = adapter.conversationInfo(thread);
  result &= require(route.historyRequestPending == std::optional<bool>{true} &&
                        info && info->historyRequestPending,
                    "same-transaction Operation replacement exposed an "
                    "intermediate idle state");

  {
    auto write = graph.write();
    write.remove(replacement);
    graphChange = write.finish();
  }
  changed = {graphChange.revision, std::move(graphChange.affected),
             std::move(graphChange.removed), false,
             std::move(graphChange.childListsChanged)};
  route = adapter.conversationRoute(changed, thread);
  info = adapter.conversationInfo(thread);
  result &= require(route.historyRequestPending == std::optional<bool>{false} &&
                        info && !info->historyRequestPending,
                    "retiring the exact history Operation did not re-enable "
                    "Load More");
  return result;
}

bool pendingRequestsPreserveExactGraphAuthority() {
  nodegraph::NodeGraph graph;
  NodeRef selectedThread;
  NodeRef otherThread;
  NodeRef recovery;
  NodeRef actionable;
  {
    auto write = graph.write();
    NodeState connectionState;
    connectionState.status = nodegraph::NodeStatus::Connected;
    connectionState.fields = {{"role", "controller"},
                              {"providerState", "ready"}};
    static_cast<void>(write.upsert({NodeKind::Connection, "connection"},
                                   std::move(connectionState)));
    const NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});

    NodeState selectedState;
    selectedState.fields = {{"name", "Selected title"}};
    selectedThread = write.upsert({NodeKind::Thread, "selected-thread"},
                                  std::move(selectedState));
    NodeState otherState;
    otherState.fields = {{"title", "Other title"}};
    otherThread =
        write.upsert({NodeKind::Thread, "other-thread"}, std::move(otherState));

    NodeState recoveryState;
    recoveryState.status = nodegraph::NodeStatus::Failed;
    recoveryState.fields = {
        {"requestId", "shared-wire-id"},
        {"method", "item/commandExecution/requestApproval"},
        {"payload", nodegraph::Value::Object{{"threadId", "selected-thread"},
                                             {"command", "make old"}}},
        {"connectionGeneration", std::uint64_t{3}},
        {"providerGeneration", std::uint64_t{5}},
        {"responseRevision", std::uint64_t{17}},
        {"recoveryOnly", true},
        {"error", "provider changed"},
        {"retainedResponsePayload",
         nodegraph::Value::Object{
             {"choice", "accept"},
             {"input", nodegraph::Value::Object{}},
             {"metadata", nodegraph::Value::Object{{"source", "user"}}}}}};
    recovery = write.upsert({NodeKind::Interaction, "recovery-node"},
                            std::move(recoveryState));

    NodeState actionableState;
    actionableState.status = nodegraph::NodeStatus::Pending;
    actionableState.fields = {
        {"requestId", "shared-wire-id"},
        {"method", "item/fileChange/requestApproval"},
        {"payload", nodegraph::Value::Object{{"threadId", "other-thread"},
                                             {"reason", "review patch"}}},
        {"connectionGeneration", std::uint64_t{3}},
        {"providerGeneration", std::uint64_t{6}},
        {"responseRevision", std::uint64_t{19}}};
    actionable = write.upsert({NodeKind::Interaction, "actionable-node"},
                              std::move(actionableState));

    write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                 recovery);
    write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                 actionable);
    write.relate(recovery, nodegraph::RelationKind::InteractionTarget,
                 selectedThread);
    write.relate(actionable, nodegraph::RelationKind::InteractionTarget,
                 otherThread);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto projected = adapter.pendingRequests();
  bool passed = true;
  passed &= require(projected && projected->total == 2 &&
                        pageValueCount<PendingRequestDescriptor>(*projected) ==
                            2,
                    "pending request projection preserves both exact nodes");
  if (projected) {
    const PendingRequestDescriptor *old =
        pageValue<PendingRequestDescriptor>(*projected, 0);
    const PendingRequestDescriptor *current =
        pageValue<PendingRequestDescriptor>(*projected, 1);
    passed &=
        require(old && current && old->target == recovery &&
                    current->target == actionable &&
                    recovery != actionable,
                "same wire id remains two distinct exact Interaction targets");
    passed &= require(
        old && old->kind == PendingRequestKind::CommandApproval &&
            old->method == "item/commandExecution/requestApproval" &&
            old->threadId == "selected-thread" &&
            old->threadTitle == "Selected title" &&
            old->responseRevision == 17 &&
            old->availability == PendingRequestAvailability::RecoveryOnly &&
            old->error == "provider changed" && old->retainedSubmission &&
            old->retainedSubmission->choice == "accept" &&
            old->retainedSubmission->input == nlohmann::json::object() &&
            old->retainedSubmission->metadata ==
                nlohmann::json({{"source", "user"}}) &&
            old->raw == nlohmann::json({{"threadId", "selected-thread"},
                                        {"command", "make old"}}),
        "recovery projection retains exact context and authored data");
    passed &= require(
        current && current->kind == PendingRequestKind::FileChangeApproval &&
            current->threadId == "other-thread" &&
            current->threadTitle == "Other title" &&
            current->responseRevision == 19 &&
            current->availability ==
                PendingRequestAvailability::Actionable &&
            !current->retainedSubmission &&
            current->raw == nlohmann::json({{"threadId", "other-thread"},
                                            {"reason", "review patch"}}),
        "actionable projection comes from the current graph node");
  }

  const auto inspector =
      adapter.inspector(selectedThread, InspectorProjection::Requests);
  passed &= require(inspector && projected && inspector->requests == *projected,
                    "Requests Inspector consumes the canonical projection");

  {
    auto write = graph.write();
    const NodeRef connection = write.find({NodeKind::Connection, "connection"});
    write.setField(connection, "providerState", "unavailable");
    write.setField(otherThread, "title", "Renamed other title");
    static_cast<void>(write.finish());
  }
  const auto unavailable = adapter.pendingRequests();
  const PendingRequestDescriptor *renamed =
      unavailable ? pageValue<PendingRequestDescriptor>(*unavailable, 1)
                  : nullptr;
  passed &= require(
      unavailable && unavailable->total == 2 && renamed &&
          renamed->availability ==
              PendingRequestAvailability::Unavailable &&
          renamed->threadTitle == "Renamed other title",
      "connection and thread authorities reproject availability and context");
  return passed;
}

bool pendingRequestOwnershipFollowsExactAncestry() {
  nodegraph::NodeGraph graph;
  NodeRef runtime;
  NodeRef firstThread;
  NodeRef secondThread;
  NodeRef firstTurn;
  NodeRef secondTurn;
  NodeRef target;
  NodeRef interaction;
  {
    auto write = graph.write();
    runtime = write.upsert({NodeKind::Runtime, "runtime"});
    NodeState firstState;
    firstState.fields.emplace("name", "First request owner");
    firstThread = write.upsert({NodeKind::Thread, "request-first"},
                               std::move(firstState));
    NodeState secondState;
    secondState.fields.emplace("name", "Second request owner");
    secondThread = write.upsert({NodeKind::Thread, "request-second"},
                                std::move(secondState));
    firstTurn = write.upsert({NodeKind::Turn, "request-first-turn"});
    secondTurn = write.upsert({NodeKind::Turn, "request-second-turn"});
    target = write.upsert({NodeKind::Item, "request-target"});
    write.setParent(firstThread, firstTurn);
    write.setParent(secondThread, secondTurn);
    write.setParent(firstTurn, target);
    NodeState request;
    request.status = nodegraph::NodeStatus::Pending;
    request.fields = {
        {"method", "item/commandExecution/requestApproval"},
        {"payload",
         nodegraph::Value::Object{{"threadId", "request-first"}}}};
    interaction = write.upsert({NodeKind::Interaction, "moving-request"},
                               std::move(request));
    write.relate(interaction, nodegraph::RelationKind::InteractionTarget,
                 target);
    write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                 interaction);
    static_cast<void>(write.finish());
  }

  const auto notification = [](nodegraph::GraphChange change) {
    return nodegraph::GraphChanged{
        change.revision, std::move(change.affected),
        std::move(change.removed), false,
        std::move(change.childListsChanged)};
  };
  NodeGraphUiAdapter adapter(graph);
  bool passed = true;
  auto firstRow = adapter.threadRow(firstThread);
  auto secondRow = adapter.threadRow(secondThread);
  auto request = adapter.pendingRequest(interaction);
  passed &= require(firstRow && secondRow && firstRow->pending == 1 &&
                        secondRow->pending == 0 && request &&
                        request->threadId == "request-first" &&
                        request->threadTitle == "First request owner",
                    "pending ownership did not begin at exact target ancestry");

  nodegraph::GraphChanged siblingChange;
  {
    auto write = graph.write();
    const NodeRef sibling =
        write.upsert({NodeKind::Item, "unrelated-request-sibling"});
    write.setParent(firstTurn, sibling);
    siblingChange = notification(write.finish());
  }
  passed &= require(
      !adapter.inspectorAffected(siblingChange, firstThread,
                                 InspectorProjection::Requests),
      "an unrelated sibling append invalidated pending request context");

  nodegraph::GraphChanged targetMove;
  {
    auto write = graph.write();
    write.setParent(secondTurn, target);
    targetMove = notification(write.finish());
  }
  firstRow = adapter.threadRow(firstThread);
  secondRow = adapter.threadRow(secondThread);
  request = adapter.pendingRequest(interaction);
  passed &= require(
      adapter.inspectorAffected(targetMove, firstThread,
                                InspectorProjection::Requests) &&
          firstRow && secondRow && firstRow->pending == 0 &&
          secondRow->pending == 1 && request &&
          request->threadId == "request-second" &&
          request->threadTitle == "Second request owner",
      "moving the exact target did not move request context and attention");

  nodegraph::GraphChanged ancestorMove;
  {
    auto write = graph.write();
    write.setParent(firstThread, secondTurn);
    ancestorMove = notification(write.finish());
  }
  firstRow = adapter.threadRow(firstThread);
  secondRow = adapter.threadRow(secondThread);
  passed &= require(
      adapter.inspectorAffected(ancestorMove, secondThread,
                                InspectorProjection::Requests) &&
          firstRow && secondRow && firstRow->pending == 1 &&
          secondRow->pending == 0,
      "moving a target ancestor left pending attention on its former Thread");

  nodegraph::GraphChanged detached;
  {
    auto write = graph.write();
    write.remove(secondTurn);
    detached = notification(write.finish());
  }
  request = adapter.pendingRequest(interaction);
  firstRow = adapter.threadRow(firstThread);
  secondRow = adapter.threadRow(secondThread);
  passed &= require(
      adapter.inspectorAffected(detached, firstThread,
                                InspectorProjection::Requests) &&
          request && request->threadId == "request-first" &&
          request->threadTitle.empty() && firstRow && secondRow &&
          firstRow->pending == 0 && secondRow->pending == 0,
      "ancestor retirement did not detach request context and exact badges");

  NodeRef recreatedFirst;
  NodeRef currentForRecreated;
  {
    auto write = graph.write();
    write.remove(firstThread);
    NodeState threadState;
    threadState.fields.emplace("name", "Recreated request owner");
    recreatedFirst = write.upsert({NodeKind::Thread, "request-first"},
                                  std::move(threadState));
    NodeState currentState;
    currentState.status = nodegraph::NodeStatus::Pending;
    currentState.fields = {
        {"method", "item/commandExecution/requestApproval"},
        {"payload",
         nodegraph::Value::Object{{"threadId", "request-first"}}}};
    currentForRecreated = write.upsert(
        {NodeKind::Interaction, "current-recreated-request"},
        std::move(currentState));
    write.relate(currentForRecreated,
                 nodegraph::RelationKind::InteractionTarget, recreatedFirst);
    write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                 currentForRecreated);
    static_cast<void>(write.finish());
  }
  request = adapter.pendingRequest(interaction);
  firstRow = adapter.threadRow(recreatedFirst);
  passed &= require(
      request && request->threadId == "request-first" &&
          request->threadTitle.empty() && firstRow && firstRow->pending == 1,
      "payload fallback attached retained attention to a reused Thread ID");

  NodeRef replacement;
  {
    auto write = graph.write();
    write.remove(interaction);
    NodeState state;
    state.status = nodegraph::NodeStatus::Pending;
    state.fields = {
        {"method", "item/fileChange/requestApproval"},
        {"payload",
         nodegraph::Value::Object{{"threadId", "request-second"}}}};
    replacement = write.upsert({NodeKind::Interaction, "moving-request"},
                               std::move(state));
    write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                 replacement);
    static_cast<void>(write.finish());
  }
  bool busy = true;
  passed &= require(!adapter.pendingRequest(interaction, &busy) && !busy &&
                        adapter.pendingRequest(replacement),
                    "exact request lookup retargeted a retired incarnation");
  {
    const std::array<NodeRef, 1> retired{interaction};
    auto write = graph.write();
    write.releaseRetired(retired);
    static_cast<void>(write.finish());
  }
  nodegraph::NodeGraph foreignGraph;
  NodeRef foreign;
  {
    auto write = foreignGraph.write();
    foreign = write.upsert({NodeKind::Interaction, "moving-request"});
    static_cast<void>(write.finish());
  }
  passed &= require(!adapter.pendingRequest(interaction, &busy) && !busy &&
                        !adapter.pendingRequest(foreign, &busy) && !busy,
                    "released or foreign exact request lookup was unsafe");
  return passed;
}

bool pendingRequestProjectionWorkIsQuantitativelyBounded() {
  struct Measurement final {
    bool valid = false;
    std::chrono::steady_clock::duration projection{};
    std::chrono::steady_clock::duration paging{};
    std::chrono::steady_clock::duration unrelated{};
    std::chrono::steady_clock::duration hierarchy{};
  };
  const auto measure = [](std::size_t count) {
    nodegraph::NodeGraph graph;
    NodeRef runtime;
    NodeRef thread;
    NodeRef firstTurn;
    NodeRef secondTurn;
    NodeRef movable;
    std::vector<NodeRef> requests;
    requests.reserve(count);
    {
      auto write = graph.write();
      runtime = write.upsert({NodeKind::Runtime, "runtime"});
      thread = write.upsert({NodeKind::Thread, "request-scale-thread"});
      firstTurn = write.upsert({NodeKind::Turn, "request-scale-turn-a"});
      secondTurn = write.upsert({NodeKind::Turn, "request-scale-turn-b"});
      movable = write.upsert({NodeKind::Item, "request-scale-movable"});
      write.setParent(thread, firstTurn);
      write.setParent(thread, secondTurn);
      write.setParent(firstTurn, movable);
      for (std::size_t index = 0; index < count; ++index) {
        NodeState state;
        state.status = nodegraph::NodeStatus::Pending;
        state.fields = {
            {"method", "item/commandExecution/requestApproval"},
            {"payload", nodegraph::Value::Object{
                            {"threadId", "request-scale-thread"}}}};
        NodeRef request = write.upsert(
            {NodeKind::Interaction,
             "request-scale-" + std::to_string(index)},
            std::move(state));
        write.relate(request, nodegraph::RelationKind::InteractionTarget,
                     thread);
        requests.push_back(std::move(request));
      }
      write.replaceRelated(runtime,
                           nodegraph::RelationKind::PendingInteraction,
                           requests);
      static_cast<void>(write.finish());
    }
    NodeGraphUiAdapter adapter(graph);
    Measurement result;
    auto started = std::chrono::steady_clock::now();
    const auto row = adapter.threadRow(thread);
    const auto rows = adapter.threads(thread);
    result.projection = std::chrono::steady_clock::now() - started;
    result.valid = row && row->pending == count && rows &&
                   rows->roots.size() == 1 &&
                   rows->roots.front().pending == count;

    started = std::chrono::steady_clock::now();
    for (std::size_t sample = 0; sample < 32; ++sample) {
      InspectorRowRequest request;
      request.first = (count - MaximumInspectorRows) * sample / 31;
      const auto page = adapter.pendingRequests(request);
      result.valid =
          result.valid && page && page->total == count &&
          page->first == request.first &&
          page->rows.size() == MaximumInspectorRows;
    }
    result.paging = std::chrono::steady_clock::now() - started;

    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
      nodegraph::GraphChange change;
      {
        auto write = graph.write();
        write.setField(movable, "value", iteration % 2);
        change = write.finish();
      }
      nodegraph::GraphChanged notification{
          change.revision, std::move(change.affected),
          std::move(change.removed), false,
          std::move(change.childListsChanged)};
      started = std::chrono::steady_clock::now();
      result.valid =
          result.valid &&
          !adapter.inspectorAffected(notification, thread,
                                     InspectorProjection::Requests);
      result.unrelated += std::chrono::steady_clock::now() - started;
    }

    nodegraph::GraphChange move;
    {
      auto write = graph.write();
      write.setParent(secondTurn, movable);
      move = write.finish();
    }
    nodegraph::GraphChanged notification{
        move.revision, std::move(move.affected), std::move(move.removed), false,
        std::move(move.childListsChanged)};
    started = std::chrono::steady_clock::now();
    result.valid =
        result.valid &&
        !adapter.inspectorAffected(notification, thread,
                                   InspectorProjection::Requests);
    result.hierarchy = std::chrono::steady_clock::now() - started;
    return result;
  };

  const Measurement small = measure(2'000);
  const Measurement large = measure(10'000);
  std::cout << "pending request ns (projection, paging, unrelated, hierarchy; "
               "2k/10k): "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   small.projection)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   large.projection)
                   .count()
            << ", "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(small.paging)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(large.paging)
                   .count()
            << ", "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   small.unrelated)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   large.unrelated)
                   .count()
            << ", "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   small.hierarchy)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   large.hierarchy)
                   .count()
            << '\n';
  return require(
      small.valid && large.valid &&
          large.projection <=
              small.projection * 8 + std::chrono::milliseconds(30) &&
          large.paging <= small.paging * 6 + std::chrono::milliseconds(30) &&
          large.unrelated <=
              small.unrelated * 5 + std::chrono::milliseconds(10) &&
          large.hierarchy <=
              small.hierarchy * 8 + std::chrono::milliseconds(30),
      "pending Request projection and dependency work exceeded its bounds");
}

bool preservesThreadRootsAndExactChildTargets() {
  nodegraph::NodeGraph graph;
  NodeRef runtime;
  NodeRef root;
  NodeRef child;
  NodeRef orphan;
  {
    auto write = graph.write();
    runtime = write.upsert({NodeKind::Runtime, "runtime"});
    NodeState rootState;
    rootState.fields.emplace("name", "Root thread");
    root = write.upsert({NodeKind::Thread, "root"}, std::move(rootState));
    NodeState childState;
    childState.fields.emplace("name", "Child agent");
    child = write.upsert({NodeKind::Thread, "child"}, std::move(childState));
    NodeState orphanState;
    orphanState.fields.emplace("name", "Paged orphan");
    orphan = write.upsert({NodeKind::Thread, "orphan"}, std::move(orphanState));
    write.relate(runtime, nodegraph::RelationKind::RootThread, root);
    write.relate(root, nodegraph::RelationKind::AgentChildThread, child);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto result = adapter.threads(child);
  return require(result.has_value(), "thread projection unavailable") &&
         require(result->selectedThreadId == "child",
                 "selected child identity was lost") &&
         require(result->roots.size() == 2,
                 "root or unreachable paged thread disappeared") &&
         require(result->roots[0].id == root->id().canonical,
                 "canonical root identity changed") &&
         require(result->roots[0].children.size() == 1,
                 "child hierarchy was flattened") &&
         require(result->roots[0].children[0].id == child->id().canonical,
                 "canonical child identity changed") &&
         require(result->roots[1].id == orphan->id().canonical,
                 "unreachable canonical thread was hidden");
}

bool rejectsRetiredThreadIncarnationsAtEveryProjectionBoundary() {
  nodegraph::NodeGraph graph;
  NodeRef runtime;
  NodeRef first;
  {
    auto write = graph.write();
    runtime = write.upsert({NodeKind::Runtime, "runtime"});
    NodeState state;
    state.fields.emplace("name", "First lifetime");
    first = write.upsert({NodeKind::Thread, "stable-thread"},
                         std::move(state));
    write.relate(runtime, nodegraph::RelationKind::RootThread, first);
    static_cast<void>(write.finish());
  }
  NodeGraphUiAdapter adapter(graph);
  const auto firstRow = adapter.threadRow(first);
  const std::uint64_t firstIncarnation = first->incarnation();

  {
    auto write = graph.write();
    write.remove(first);
    static_cast<void>(write.finish());
  }
  NodeRef second;
  {
    auto write = graph.write();
    NodeState state;
    state.fields.emplace("name", "Second lifetime");
    second = write.upsert({NodeKind::Thread, "stable-thread"},
                          std::move(state));
    write.relate(runtime, nodegraph::RelationKind::RootThread, second);
    static_cast<void>(write.finish());
  }

  const auto staleList = adapter.threads(first);
  const auto staleInspector =
      adapter.inspector(first, InspectorProjection::Plan);
  const auto secondRow = adapter.threadRow(second);
  bool passed =
      require(firstRow && secondRow && first != second &&
                  firstIncarnation != second->incarnation() &&
                  firstRow->presentationKey != secondRow->presentationKey,
              "same canonical thread ID received a fresh presentation key") &&
      require(!adapter.threadRow(first),
              "a retired exact thread still projected a row") &&
      require(staleList && staleList->selectedThreadId.empty() &&
                  staleList->roots.size() == 1 &&
                  staleList->roots.front().target == second,
              "a retired selection retargeted the replacement thread") &&
      require(staleInspector && staleInspector->threadIncarnation == 0 &&
                  staleInspector->plan.total == 0 &&
                  staleInspector->agents.total == 0 &&
                  staleInspector->changes.threadId.empty(),
              "a retired exact thread leaked into Inspector projections");

  {
    const std::array<NodeRef, 1> retired{first};
    auto write = graph.write();
    write.releaseRetired(retired);
    static_cast<void>(write.finish());
  }
  const auto releasedList = adapter.threads(first);
  const auto releasedInspector =
      adapter.inspector(first, InspectorProjection::Plan);
  passed &= require(!adapter.threadRow(first) && releasedList &&
                        releasedList->selectedThreadId.empty() &&
                        releasedList->roots.size() == 1 &&
                        releasedList->roots.front().target == second &&
                        releasedInspector &&
                        releasedInspector->threadIncarnation == 0,
                    "released retirement made stale projection unsafe");
  return passed;
}

bool provisionalTurnRequiresTypedAdmittedPrompt() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef prompt;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "provisional-thread"});
    NodeState turnState;
    turnState.fields.emplace("id", "provisional-turn");
    turn = write.upsert({NodeKind::Turn, "provisional-turn"},
                        std::move(turnState));
    NodeState promptState =
        itemState("provisional-prompt", "localPrompt", "hello");
    promptState.status = nodegraph::NodeStatus::Pending;
    promptState.fields.emplace("local", true);
    promptState.fields.emplace("submissionId", std::uint64_t{1});
    promptState.fields.emplace("dispatchState", "inFlight");
    promptState.fields.emplace("startsTurn", true);
    prompt = write.upsert({NodeKind::Item, "provisional-prompt"},
                          std::move(promptState));
    write.setParent(thread, turn);
    write.setParent(turn, prompt);
    write.relate(thread, nodegraph::RelationKind::PendingPrompt, prompt);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto initial = adapter.conversation(thread);
  const std::array<NodeRef, 1> changed{prompt};
  const auto initialDelta = adapter.conversationDelta(thread, changed, true);
  const middle::ConversationRowChange *initialRow =
      initialDelta && initialDelta->rows.size() == 1
          ? &initialDelta->rows.front()
          : nullptr;
  bool passed =
      require(initial && initial->activeTurnId == "provisional-turn" &&
                  initialRow && initialRow->placement.activeTurn,
              "a typed admitted starts-turn prompt did not expose "
              "its provisional turn");
  {
    auto read = graph.tryRead();
    passed &= require(
        read &&
            read->related(thread, nodegraph::RelationKind::ActiveTurn).empty(),
        "provisional activity fabricated an authoritative ActiveTurn relation");
  }

  const auto mutate = [&](nodegraph::NodeStatus status, std::string dispatch,
                          bool startsTurn) {
    auto write = graph.write();
    write.setStatus(prompt, status);
    write.setField(prompt, "dispatchState", std::move(dispatch));
    write.setField(prompt, "startsTurn", startsTurn);
    static_cast<void>(write.finish());
  };
  const auto expectsActive = [&](bool expected, const char *message) {
    const auto snapshot = adapter.conversation(thread);
    passed &= require(
        snapshot && snapshot->activeTurnId.has_value() == expected, message);
  };
  mutate(nodegraph::NodeStatus::Pending, "queued", true);
  expectsActive(false, "a merely queued prompt became a provisional turn");
  mutate(nodegraph::NodeStatus::Unknown, "dispatching", true);
  expectsActive(false, "raw dispatch state bypassed typed prompt status");
  mutate(nodegraph::NodeStatus::Running, "dispatching", false);
  expectsActive(false, "a steering prompt became a provisional turn");
  mutate(nodegraph::NodeStatus::Running, "dispatching", true);
  expectsActive(true, "a running starts-turn prompt lost provisional activity");
  mutate(nodegraph::NodeStatus::Running, "awaitingMaterialization", true);
  expectsActive(false,
                "an acknowledged prompt remained a provisional active turn");
  return passed;
}

bool structuredPlanPresenceAndEffectiveStatusAreCanonical() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef legacyPlan;
  {
    auto write = graph.write();
    NodeState threadState;
    threadState.status = nodegraph::NodeStatus::Failed;
    threadState.fields.emplace("status", "failed");
    thread =
        write.upsert({NodeKind::Thread, "plan-thread"}, std::move(threadState));
    NodeState turnState;
    turnState.status = nodegraph::NodeStatus::Completed;
    turnState.fields = {
        {"id", "plan-turn"},
        {"status", "completed"},
        {"planExplanation", ""},
        {"plan", nodegraph::Value::Array{
                     nodegraph::Value(nodegraph::Value::Object{
                         {"step", "running step"}, {"status", "inProgress"}}),
                     nodegraph::Value(nodegraph::Value::Object{
                         {"step", "pending step"}, {"status", "queued"}})}}};
    turn = write.upsert({NodeKind::Turn, "plan-turn"}, std::move(turnState));
    write.setParent(thread, turn);
    legacyPlan = write.upsert(
        {NodeKind::Item, "legacy-plan"},
        itemState("legacy-plan", "plan", "legacy fallback"));
    write.setParent(turn, legacyPlan);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  auto snapshot = adapter.inspector(thread, InspectorProjection::Plan);
  const middle::PlanStepData *running =
      snapshot ? pageValue<middle::PlanStepData>(snapshot->plan, 0) : nullptr;
  const middle::PlanStepData *pending =
      snapshot ? pageValue<middle::PlanStepData>(snapshot->plan, 1) : nullptr;
  bool passed = require(
      snapshot && snapshot->plan.total == 2 && running && pending &&
          running->status.semantic ==
              nodegraph::NodeStatus::Completed &&
          pending->status.semantic ==
              nodegraph::NodeStatus::Pending,
      "structured Plan did not use typed turn outcome or preserve pending");

  {
    auto write = graph.write();
    write.setField(
        turn, "plan",
        nodegraph::Value::Object{
            {"explanation", "Nested explanation"},
            {"steps", nodegraph::Value::Array{
                          nodegraph::Value(nodegraph::Value::Object{
                              {"step", "nested step"},
                              {"status", "completed"}}),
                          nodegraph::Value("malformed step"),
                          nodegraph::Value(nodegraph::Value::Object{})}}});
    write.setField(turn, "planExplanation", "");
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(thread, InspectorProjection::Plan);
  const InspectorMarkdownRow *nestedExplanation =
      snapshot ? pageValue<InspectorMarkdownRow>(snapshot->plan, 0) : nullptr;
  const middle::PlanStepData *nestedStep =
      snapshot ? pageValue<middle::PlanStepData>(snapshot->plan, 0) : nullptr;
  passed &= require(snapshot && snapshot->plan.total == 4 &&
                        nestedExplanation && nestedStep &&
                        nestedExplanation->text == "Nested explanation" &&
                        nestedStep->text == "nested step" &&
                        pageValueCount<middle::PlanStepData>(snapshot->plan) ==
                            3,
                    "object-form Plan lost nested or positional step data");

  {
    auto write = graph.write();
    write.setField(turn, "plan", nodegraph::Value::Array{});
    write.setField(turn, "planExplanation", "");
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(thread, InspectorProjection::Plan);
  passed &= require(snapshot && snapshot->plan.total == 0 &&
                        snapshot->plan.emptyMessage.empty(),
                    "an explicitly empty structured Plan exposed legacy data");

  {
    auto write = graph.write();
    write.setField(turn, "plan",
                   nodegraph::Value::Object{
                       {"steps", nodegraph::Value::Array{}}});
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(thread, InspectorProjection::Plan);
  passed &= require(snapshot && snapshot->plan.total == 0 &&
                        snapshot->plan.emptyMessage.empty(),
                    "an object-form empty Plan exposed legacy data");

  {
    auto write = graph.write();
    write.setField(turn, "plan", nodegraph::Value::Object{});
    write.setField(turn, "planExplanation", nullptr);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(thread, InspectorProjection::Plan);
  const InspectorMarkdownRow *legacy =
      snapshot ? pageValue<InspectorMarkdownRow>(snapshot->plan) : nullptr;
  passed &= require(snapshot && snapshot->plan.total == 1 && legacy &&
                        legacy->text == "legacy fallback",
                    "malformed Plan fields suppressed the legacy fallback");

  {
    auto write = graph.write();
    write.setField(legacyPlan, "text", "");
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(thread, InspectorProjection::Plan);
  passed &= require(snapshot && snapshot->plan.total == 0 &&
                        snapshot->plan.emptyMessage ==
                            "Plan is being prepared.",
                    "an empty legacy Plan lost its preparing state");
  return passed;
}

bool projectsBoundedInspectorPagesFromCanonicalGraph() {
  constexpr std::size_t RowCount = 121;
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  std::vector<NodeRef> agents;
  std::vector<NodeRef> requests;
  {
    auto write = graph.write();
    NodeState connection;
    connection.status = nodegraph::NodeStatus::Connected;
    connection.fields = {{"role", "controller"}, {"providerState", "ready"}};
    static_cast<void>(write.upsert({NodeKind::Connection, "connection"},
                                   std::move(connection)));
    const NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
    thread = write.upsert({NodeKind::Thread, "paged-thread"});
    nodegraph::Value::Array steps;
    steps.reserve(RowCount);
    for (std::size_t row = 0; row < RowCount; ++row)
      steps.emplace_back(nodegraph::Value::Object{
          {"step", "plan-" + std::to_string(row)}, {"status", "pending"}});
    NodeState turnState;
    turnState.fields = {{"plan", std::move(steps)}};
    turn = write.upsert({NodeKind::Turn, "paged-turn"},
                        std::move(turnState));
    write.setParent(thread, turn);
    agents.reserve(RowCount);
    requests.reserve(RowCount);
    for (std::size_t row = 0; row < RowCount; ++row) {
      NodeState agent;
      agent.fields = {{"type", "subAgentActivity"},
                      {"kind", "started"},
                      {"agentThreadId", "agent-" + std::to_string(row)},
                      {"prompt", "agent-" + std::to_string(row)}};
      NodeRef item = write.upsert(
          {NodeKind::Item, "paged-agent-" + std::to_string(row)},
          std::move(agent));
      write.setParent(turn, item);
      agents.emplace_back(std::move(item));

      NodeState request;
      request.status = nodegraph::NodeStatus::Pending;
      request.fields = {
          {"method", "item/commandExecution/requestApproval"},
          {"payload",
           nodegraph::Value::Object{{"threadId", "paged-thread"},
                                    {"command", "request-" +
                                                    std::to_string(row)}}}};
      NodeRef interaction = write.upsert(
          {NodeKind::Interaction, "paged-request-" + std::to_string(row)},
          std::move(request));
      write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                   interaction);
      requests.emplace_back(std::move(interaction));
    }
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto page = [&](InspectorProjection projection,
                        const InspectorRowRequest &request) {
    const auto snapshot = adapter.inspector(thread, projection, request);
    if (!snapshot)
      return InspectorPageSnapshot{};
    switch (projection) {
    case InspectorProjection::Plan:
      return snapshot->plan;
    case InspectorProjection::Agents:
      return snapshot->agents;
    case InspectorProjection::Requests:
      return snapshot->requests;
    case InspectorProjection::Changes:
    case InspectorProjection::State:
    case InspectorProjection::Protocol:
      return InspectorPageSnapshot{};
    }
    return InspectorPageSnapshot{};
  };
  const auto label = [](const InspectorRow &row) {
    if (const auto *plan = std::get_if<middle::PlanStepData>(&row.value))
      return plan->text;
    if (const auto *agent = std::get_if<InspectorAgentRow>(&row.value))
      return agent->prompt;
    if (const auto *request =
            std::get_if<PendingRequestDescriptor>(&row.value))
      return request->target ? request->target->id().canonical : std::string{};
    return std::string{};
  };
  const auto expected = [](InspectorProjection projection, std::size_t row) {
    const char *prefix = projection == InspectorProjection::Plan
                             ? "plan-"
                         : projection == InspectorProjection::Agents
                             ? "agent-"
                             : "paged-request-";
    return std::string(prefix) + std::to_string(row);
  };
  const auto name = [](InspectorProjection projection) {
    return projection == InspectorProjection::Plan
               ? "Plan"
           : projection == InspectorProjection::Agents ? "Agents"
                                                       : "Requests";
  };
  const auto keyOwnsVariant = [](InspectorProjection projection,
                                 const InspectorRow &row) {
    if (projection == InspectorProjection::Plan)
      return std::holds_alternative<middle::PlanStepData>(row.value)
                 ? row.key.starts_with("plan:step:")
                 : std::holds_alternative<InspectorMarkdownRow>(row.value) &&
                       row.key.starts_with("plan:") &&
                       !row.key.starts_with("plan:step:");
    if (projection == InspectorProjection::Agents)
      return std::holds_alternative<InspectorAgentRow>(row.value) &&
             row.key.starts_with("agent:");
    return std::holds_alternative<PendingRequestDescriptor>(row.value) &&
           row.key.starts_with("request:");
  };
  bool passed = true;
  std::array<std::array<std::string, 3>, 3> savedKeys;
  constexpr std::array projections{InspectorProjection::Plan,
                                   InspectorProjection::Agents,
                                   InspectorProjection::Requests};
  for (std::size_t section = 0; section < projections.size(); ++section) {
    const InspectorProjection projection = projections[section];
    const InspectorPageSnapshot first = page(projection, {});
    const InspectorPageSnapshot middle = page(projection, {52, 13});
    const InspectorPageSnapshot end = page(projection, {117, 50});
    const bool bounded =
        first.total == RowCount && first.first == 0 &&
        first.rows.size() == MaximumInspectorRows &&
        label(first.rows.front()) == expected(projection, 0) &&
        label(first.rows.back()) == expected(projection, 49) &&
        middle.first == 52 && middle.rows.size() == 13 &&
        label(middle.rows.front()) == expected(projection, 52) &&
        label(middle.rows.back()) == expected(projection, 64) &&
        end.first == 117 && end.rows.size() == 4 &&
        label(end.rows.front()) == expected(projection, 117) &&
        label(end.rows.back()) == expected(projection, 120) &&
        std::ranges::all_of(first.rows, [&](const InspectorRow &row) {
          return keyOwnsVariant(projection, row);
        });
    if (!bounded) {
      std::cerr << name(projection)
                << " real Inspector paging is not bounded and exact\n";
      passed = false;
    }
    if (projection == InspectorProjection::Agents) {
      InspectorRowRequest retained;
      retained.retainedKeys.push_back(first.rows.front().key);
      retained.retainedKeys.push_back(first.rows.front().key);
      for (std::size_t row = 1; row < MaximumInspectorRows - 1; ++row)
        retained.retainedKeys.push_back(first.rows[row].key);
      retained.retainedKeys.push_back(first.rows.back().key);
      const InspectorPageSnapshot validated = page(projection, retained);
      passed &= require(
          validated.validRetainedKeys &&
              validated.validRetainedKeys->size() ==
                  MaximumInspectorRows - 1 &&
              std::ranges::find(*validated.validRetainedKeys,
                                first.rows.back().key) ==
                  validated.validRetainedKeys->end(),
          "Agent retained-state validation is deduplicated and bounded");
    }

    const InspectorPageSnapshot keys = page(projection, {80, 31});
    if (keys.rows.size() == 31) {
      savedKeys[section] = {keys.rows[0].key, keys.rows[5].key,
                            keys.rows[30].key};
      InspectorRowRequest anchored{0, 10, savedKeys[section][0],
                                   savedKeys[section][2]};
      const InspectorPageSnapshot pinned = page(projection, anchored);
      const bool containsAnchor = std::ranges::any_of(
          pinned.rows, [&](const InspectorRow &row) {
            return row.key == savedKeys[section][0];
          });
      const bool duplicatesFocus = std::ranges::any_of(
          pinned.rows, [&](const InspectorRow &row) {
            return row.key == savedKeys[section][2];
          });
      const bool exact =
          pinned.first == 71 && pinned.anchorIndex == 80 &&
          pinned.focusedIndex == 110 && pinned.rows.size() == 10 &&
          containsAnchor && pinned.focused &&
          pinned.focused->key == savedKeys[section][2] && !duplicatesFocus &&
          label(*pinned.focused) == expected(projection, 110) &&
          keyOwnsVariant(projection, *pinned.focused);
      if (!exact) {
        std::cerr << name(projection)
                  << " anchor/focus projection is not exact\n";
        passed = false;
      }
      const InspectorPageSnapshot above =
          page(projection, {100, 10, savedKeys[section][0], {}});
      const InspectorPageSnapshot zero =
          page(projection, {100, 0, savedKeys[section][0], {}});
      passed &= require(above.first == 80 && above.rows.size() == 10 &&
                            above.rows.front().key == savedKeys[section][0],
                        "an anchor above the requested Inspector window did "
                        "not become its first row");
      passed &= require(zero.first == 100 && zero.rows.empty() &&
                            zero.anchorIndex == 80,
                        "a zero-count Inspector request moved its window or "
                        "projected rows");
    } else {
      std::cerr << name(projection) << " could not capture paging keys\n";
      passed = false;
    }
  }

  const InspectorPageSnapshot planKeys = page(InspectorProjection::Plan, {});
  if (!planKeys.rows.empty()) {
    std::string overflow = planKeys.rows.front().key;
    overflow.replace(overflow.find_last_of(':') + 1, std::string::npos,
                     std::to_string(
                         std::numeric_limits<std::size_t>::max()));
    {
      auto write = graph.write();
      write.setField(turn, "planExplanation", "Explanation");
      static_cast<void>(write.finish());
    }
    InspectorRowRequest request;
    request.count = 2;
    request.focusedKey = std::move(overflow);
    const InspectorPageSnapshot protectedPlan =
        page(InspectorProjection::Plan, request);
    passed &= require(!protectedPlan.focusedIndex && !protectedPlan.focused &&
                          protectedPlan.rows.size() == 2 &&
                          protectedPlan.rows.front().key !=
                              planKeys.rows.front().key &&
                          protectedPlan.rows.back().key ==
                              planKeys.rows.front().key &&
                          keyOwnsVariant(InspectorProjection::Plan,
                                         protectedPlan.rows.front()) &&
                          keyOwnsVariant(InspectorProjection::Plan,
                                         protectedPlan.rows.back()),
                      "Plan shape changes reused a key for another renderer "
                      "or wrapped a maximal key into a valid row");
  }

  {
    auto write = graph.write();
    nodegraph::Value::Array steps;
    for (std::size_t row = 0; row < 63; ++row)
      steps.emplace_back(nodegraph::Value::Object{
          {"step", "plan-" + std::to_string(row)}, {"status", "pending"}});
    write.setField(turn, "plan", std::move(steps));
    write.setField(turn, "planExplanation", "");
    std::vector<NodeRef> removed;
    removed.insert(removed.end(), agents.begin() + 63, agents.end());
    removed.insert(removed.end(), requests.begin() + 63, requests.end());
    write.removeMany(removed);
    static_cast<void>(write.finish());
  }
  for (std::size_t section = 0; section < projections.size(); ++section) {
    const InspectorProjection projection = projections[section];
    InspectorRowRequest stale{117, 50, savedKeys[section][2],
                              savedKeys[section][2]};
    const InspectorPageSnapshot shrunk = page(projection, stale);
    const bool populated =
        shrunk.total == 63 && shrunk.first == 13 &&
        shrunk.rows.size() == 50 && !shrunk.anchorIndex &&
        !shrunk.focusedIndex && !shrunk.focused &&
        label(shrunk.rows.front()) == expected(projection, 13) &&
        label(shrunk.rows.back()) == expected(projection, 62);
    if (!populated) {
      std::cerr << name(projection)
                << " stale window produced a blank or inexact page\n";
      passed = false;
    }
  }
  return passed;
}

bool settingsDraftLifetimeFollowsSemanticIncarnations() {
  nodegraph::NodeGraph graph;
  NodeRef local;
  NodeRef promoted;
  NodeRef ordinary;
  std::uint64_t authorityRevision = 0;
  {
    auto write = graph.write();
    const NodeRef connection =
        write.upsert({NodeKind::Connection, "connection"});
    authorityRevision = write.revision() + 1;
    write.setField(connection, "providerAuthorityRevision",
                   authorityRevision);
    NodeState localState;
    localState.fields = {{"local", true}, {"creationCorrelation", "draft-a"}};
    local = write.upsert({NodeKind::Thread, "local-a"}, std::move(localState));
    NodeState promotedState;
    promotedState.fields = {{"creationCorrelation", "draft-a"}};
    promoted = write.upsert({NodeKind::Thread, "thread-a"},
                            std::move(promotedState));
    ordinary = write.upsert({NodeKind::Thread, "thread-b"});
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto localContext = adapter.turnSettings(local);
  const auto promotedContext = adapter.turnSettings(promoted);
  const auto ordinaryContext = adapter.turnSettings(ordinary);
  const auto draftContext =
      adapter.turnSettings({}, "creation:new-draft", "/tmp");
  bool passed = true;
  passed &= require(localContext && localContext->identity == "creation:draft-a",
                    "local settings use their creation incarnation");
  passed &= require(promotedContext && promotedContext->identity == "thread-a",
                    "authoritative settings use canonical thread identity");
  passed &= require(ordinaryContext && ordinaryContext->identity == "thread-b",
                    "ordinary settings use canonical thread identity");
  passed &= require(
      localContext && promotedContext && ordinaryContext && draftContext &&
          localContext->providerAuthorityRevision == authorityRevision &&
          promotedContext->providerAuthorityRevision == authorityRevision &&
          ordinaryContext->providerAuthorityRevision == authorityRevision &&
          draftContext->providerAuthorityRevision == authorityRevision &&
          localContext->threadIncarnation == local->incarnation() &&
          promotedContext->threadIncarnation == promoted->incarnation() &&
          ordinaryContext->threadIncarnation == ordinary->incarnation() &&
          draftContext->threadIncarnation == 0,
      "settings project provider authority and exact graph incarnations");

  TurnSettingsPolicy policy;
  TurnSettingsContext destination;
  destination.identity = "thread-a";
  destination.providerAuthorityRevision = authorityRevision;
  destination.threadIncarnation = promoted->incarnation();
  destination.canonical[TurnSettingField::Approval] = "on-request";
  static_cast<void>(policy.setContext(destination));
  policy.change(TurnSettingField::Approval, "untrusted");

  TurnSettingsContext source;
  source.identity = "creation:draft-a";
  source.providerAuthorityRevision = authorityRevision;
  source.threadIncarnation = local->incarnation();
  static_cast<void>(policy.setContext(source));
  policy.change(TurnSettingField::Approval, "never");
  policy.change(TurnSettingField::Summary, "concise");
  policy.promote(source.identity, destination.identity,
                 destination.threadIncarnation);
  static_cast<void>(policy.setContext(destination));
  passed &= require(policy.context().identity == "thread-a" &&
                        policy.values()[TurnSettingField::Approval] == "never" &&
                        policy.values()[TurnSettingField::Summary] == "concise" &&
                        policy.touched(TurnSettingField::Approval) &&
                        policy.touched(TurnSettingField::Summary),
                    "promotion moves the exact source draft over stale destination state");

  TurnSettingsPolicy removedPromotion;
  static_cast<void>(removedPromotion.setContext(source));
  removedPromotion.change(TurnSettingField::Approval, "never");
  removedPromotion.promote(source.identity, destination.identity,
                           destination.threadIncarnation);
  removedPromotion.forget(destination.identity,
                          destination.threadIncarnation);
  static_cast<void>(removedPromotion.setContext(destination));
  passed &= require(
      !removedPromotion.touched(TurnSettingField::Approval) &&
          removedPromotion.values()[TurnSettingField::Approval] ==
              "on-request",
      "exact retirement releases a promoted draft before its first projection");

  TurnSettingsContext other;
  other.identity = "thread-b";
  other.providerAuthorityRevision = authorityRevision;
  other.threadIncarnation = ordinary->incarnation();
  other.canonical[TurnSettingField::Approval] = "on-request";
  static_cast<void>(policy.setContext(other));
  policy.change(TurnSettingField::Approval, "untrusted");
  static_cast<void>(policy.setContext(destination));
  passed &= require(policy.values()[TurnSettingField::Approval] == "never",
                    "ordinary thread switching preserves the authored draft");

  TurnSettingsContext reincarnated = destination;
  ++reincarnated.threadIncarnation;
  static_cast<void>(policy.setContext(reincarnated));
  passed &= require(!policy.touched(TurnSettingField::Approval) &&
                        policy.values()[TurnSettingField::Approval] ==
                            "on-request",
                    "reusing a canonical id starts from the new incarnation");
  policy.change(TurnSettingField::Approval, "untrusted");
  policy.forget(destination.identity, destination.threadIncarnation);
  static_cast<void>(policy.setContext(reincarnated));
  passed &= require(policy.touched(TurnSettingField::Approval) &&
                        policy.values()[TurnSettingField::Approval] ==
                            "untrusted",
                    "a delayed old retirement cannot erase a new incarnation");
  policy.forget(reincarnated.identity, reincarnated.threadIncarnation);
  static_cast<void>(policy.setContext(reincarnated));
  passed &= require(!policy.touched(TurnSettingField::Approval) &&
                        policy.values()[TurnSettingField::Approval] ==
                            "on-request",
                    "the exact incarnation retirement releases its draft");
  TurnSettingsContext replacement = other;
  ++replacement.providerAuthorityRevision;
  static_cast<void>(policy.setContext(replacement));
  passed &= require(!policy.touched(TurnSettingField::Approval) &&
                        policy.values()[TurnSettingField::Approval] ==
                            "on-request",
                    "a new provider authority retires every retained settings draft");
  static_cast<void>(policy.setContext(other));
  passed &= require(
      policy.context().providerAuthorityRevision ==
              replacement.providerAuthorityRevision &&
          !policy.touched(TurnSettingField::Approval) &&
          policy.values()[TurnSettingField::Approval] == "on-request",
      "an older provider projection cannot restore a retired settings draft");
  return passed;
}

bool frontendPresentationContractIsShared() {
  std::ifstream input(CODEXUI_PRESENTATION_CONTRACT_PATH);
  if (!require(input.good(),
               "shared frontend presentation fixture is readable"))
    return false;

  nlohmann::json contract;
  try {
    input >> contract;
  } catch (const std::exception &error) {
    std::cerr << "shared frontend presentation fixture did not parse: "
              << error.what() << '\n';
    return false;
  }

  bool passed = require(contract.value("schemaVersion", 0) == 5,
                        "shared presentation schema version changed");
  const auto check = [&passed](bool condition, const std::string &message) {
    passed &= require(condition, message.c_str());
  };
  const auto normalized = [](const std::string &raw) {
    const nodegraph::Value value(raw);
    return statusFromNode(nodegraph::nodeStatusFromValue(&value), raw);
  };

  for (const auto &group : contract.at("statusCases")) {
    const auto &expected = group.at("expected");
    for (const auto &rawValue : group.at("inputs")) {
      const std::string raw = rawValue.get<std::string>();
      const nodegraph::Value value(raw);
      NodeState state;
      state.status = nodegraph::nodeStatusFromValue(&value);
      state.fields.emplace("status", value);
      const UiStatus status = statusFromState(state);
      const std::string prefix =
          "status contract " + group.at("id").get<std::string>() + ": ";
      check(status.semantic == nodegraph::nodeStatusFromText(
                                   expected.at("semantic").get<std::string>()),
            prefix + "semantic");
      check(statusToken(status) == expected.at("token").get<std::string>(),
            prefix + "token");
      check(displayStatus(status) == expected.at("display").get<std::string>(),
            prefix + "display");
      check(statusTone(status) == expected.at("tone").get<std::string>(),
            prefix + "tone");
      check(isActiveStatus(status) == expected.at("active").get<bool>(),
            prefix + "active");
      check(isWorkingStatus(status) == expected.at("working").get<bool>(),
            prefix + "working");
      check(isTerminalTurnStatus(status) == expected.at("terminal").get<bool>(),
            prefix + "terminal");
    }
  }

  for (const auto &entry : contract.at("planCases")) {
    const UiStatus status = effectivePlanStepStatus(
        normalized(entry.at("step").get<std::string>()),
        normalized(entry.at("turn").get<std::string>()),
        normalized(entry.at("thread").get<std::string>()));
    const auto &expected = entry.at("expected");
    const std::string prefix =
        "plan contract " + entry.at("id").get<std::string>() + ": ";
    check(status.semantic == nodegraph::nodeStatusFromText(
                                 expected.at("semantic").get<std::string>()),
          prefix + "semantic");
    check(statusToken(status) == expected.at("token").get<std::string>(),
          prefix + "token");
  }

  for (const auto &entry : contract.at("lifecycleCases")) {
    nodegraph::NodeGraph graph;
    nodegraph::ProtocolUpdater updater(graph);
    const std::string id = entry.at("id").get<std::string>();
    nodegraph::Value::Object thread{{"id", id}};
    if (entry.contains("initialThreadStatus"))
      thread.emplace("status",
                     entry.at("initialThreadStatus").get<std::string>());
    static_cast<void>(
        updater.apply({nodegraph::DecodedMessageKind::ServerNotification,
                       "thread/started",
                       std::nullopt,
                       {{"thread", nodegraph::Value(std::move(thread))}}}));
    const std::string entity = entry.at("entity").get<std::string>();
    if (entity != "turn")
      static_cast<void>(updater.apply(
          {nodegraph::DecodedMessageKind::ServerNotification,
           "turn/started",
           std::nullopt,
           {{"threadId", id},
            {"turn",
             nodegraph::Value(nodegraph::Value::Object{{"id", "turn"}})}}}));
    nodegraph::Value::Object value{{"id", entity == "item" ? "item" : "turn"}};
    if (entry.contains("status"))
      value.emplace("status", entry.at("status").get<std::string>());
    if (entity == "item")
      value.emplace("type", "agentMessage");
    nodegraph::Value::Object payload{{"threadId", id}};
    std::string method =
        entity + '/' + entry.at("lifecycle").get<std::string>();
    if (entity == "thread") {
      method = "thread/status/changed";
      payload.emplace("status", entry.at("status").get<std::string>());
    } else if (entity == "turn") {
      payload.emplace("turn", nodegraph::Value(std::move(value)));
    } else {
      payload.emplace("turnId", "turn");
      payload.emplace("item", nodegraph::Value(std::move(value)));
    }
    static_cast<void>(
        updater.apply({nodegraph::DecodedMessageKind::ServerNotification,
                       std::move(method), std::nullopt, std::move(payload)}));
    auto read = graph.tryRead();
    const NodeRef threadNode = read->find({NodeKind::Thread, id});
    const NodeRef node =
        entity == "thread" ? threadNode
        : entity == "turn"
            ? read->find(nodegraph::scopedTurnNodeId(id, "turn"))
            : read->find(nodegraph::scopedItemNodeId(
                  nodegraph::scopedTurnNodeId(id, "turn"), "item"));
    const std::string prefix = "lifecycle contract " + id + ": ";
    check(threadNode && node, prefix + "graph identity");
    if (!threadNode || !node)
      continue;
    const UiStatus status = statusFromState(*read->state(node));
    const auto &expected = entry.at("expected");
    check(status.semantic == nodegraph::nodeStatusFromText(
                                 expected.at("semantic").get<std::string>()),
          prefix + "semantic");
    check(statusToken(status) == expected.at("token").get<std::string>(),
          prefix + "token");
    if (entry.contains("expectedThreadStatus"))
      check(statusToken(statusFromState(*read->state(threadNode))) ==
                entry.at("expectedThreadStatus").get<std::string>(),
            prefix + "thread status");
    if (entry.contains("expectedActiveTurnId")) {
      const NodeRef active =
          read->relatedAt(threadNode, nodegraph::RelationKind::ActiveTurn, 0);
      const std::string expectedActive =
          entry.at("expectedActiveTurnId").get<std::string>();
      check(expectedActive.empty()
                ? !active
                : active && nodegraph::protocolCanonicalId(
                                *read->state(active), active) == expectedActive,
            prefix + "active turn");
    }
  }

  for (const auto &entry : contract.at("planReplacementCases")) {
    nodegraph::NodeGraph graph;
    nodegraph::ProtocolUpdater updater(graph);
    const std::string id = entry.at("id").get<std::string>();
    static_cast<void>(updater.apply(
        {nodegraph::DecodedMessageKind::ServerNotification,
         "thread/started",
         std::nullopt,
         {{"thread",
           nodegraph::Value(nodegraph::Value::Object{{"id", id}})}}}));
    static_cast<void>(updater.apply(
        {nodegraph::DecodedMessageKind::ServerNotification,
         "turn/started",
         std::nullopt,
         {{"threadId", id},
          {"turn",
           nodegraph::Value(nodegraph::Value::Object{{"id", "turn"}})}}}));
    static_cast<void>(updater.apply(
        {nodegraph::DecodedMessageKind::ServerNotification,
         "turn/plan/updated",
         std::nullopt,
         {{"threadId", id},
          {"turnId", "turn"},
          {"explanation", entry.at("initialExplanation").get<std::string>()},
          {"plan", nodegraph::Value(nodegraph::Value::Array{nodegraph::Value(
                       nodegraph::Value::Object{{"step", "obsolete step"},
                                                {"status", "pending"}})})}}}));
    const nodegraph::Value replacement =
        entry.at("replacementPlan").is_array()
            ? nodegraph::Value(nodegraph::Value::Array{})
            : nodegraph::Value(nodegraph::Value::Object{{"unexpected", true}});
    static_cast<void>(updater.apply(
        {nodegraph::DecodedMessageKind::ServerNotification,
         "turn/plan/updated",
         std::nullopt,
         {{"threadId", id}, {"turnId", "turn"}, {"plan", replacement}}}));
    NodeRef threadNode;
    {
      auto read = graph.tryRead();
      threadNode = read->find({NodeKind::Thread, id});
    }
    NodeGraphUiAdapter adapter(graph);
    const auto snapshot =
        adapter.inspector(threadNode, InspectorProjection::Plan);
    const std::string prefix = "plan replacement contract " + id + ": ";
    check(snapshot && snapshot->plan.emptyMessage.empty(),
          prefix + "explicit replacement presence");
    if (snapshot) {
      const InspectorMarkdownRow *explanation =
          pageValue<InspectorMarkdownRow>(snapshot->plan);
      check((explanation ? explanation->text : std::string{}) ==
                entry.at("expectedExplanation").get<std::string>(),
            prefix + "explanation");
      check(pageValueCount<middle::PlanStepData>(snapshot->plan) ==
                entry.at("expectedStepCount").get<std::size_t>(),
            prefix + "steps");
    }
  }

  for (const auto &entry : contract.at("childLifecycleCases")) {
    nodegraph::NodeGraph graph;
    NodeRef parent;
    NodeRef child;
    {
      auto write = graph.write();
      parent = write.upsert({NodeKind::Thread, "parent"});
      const NodeRef turn = write.upsert({NodeKind::Turn, "turn"});
      NodeState activityState;
      activityState.status = nodegraph::NodeStatus::Running;
      activityState.fields = {{"type", "subAgentActivity"},
                              {"status", "running"},
                              {"agentThreadId", "child"}};
      const NodeRef activity =
          write.upsert({NodeKind::Item, "activity"}, std::move(activityState));
      NodeState childState;
      childState.status = nodegraph::NodeStatus::Running;
      childState.fields = {{"status", "running"}};
      child = write.upsert({NodeKind::Thread, "child"}, std::move(childState));
      write.setParent(parent, turn);
      write.setParent(turn, activity);
      write.relate(activity, nodegraph::RelationKind::AgentChildThread, child);
      static_cast<void>(write.finish());
    }
    nodegraph::ProtocolUpdater updater(graph);
    static_cast<void>(
        updater.apply({nodegraph::DecodedMessageKind::ServerNotification,
                       "thread/" + entry.at("lifecycle").get<std::string>(),
                       std::nullopt,
                       {{"threadId", "child"}}}));
    NodeGraphUiAdapter adapter(graph);
    const auto snapshot =
        adapter.inspector(parent, InspectorProjection::Agents);
    const InspectorAgentRow *agent =
        snapshot ? pageValue<InspectorAgentRow>(snapshot->agents) : nullptr;
    const std::string prefix =
        "child lifecycle contract " + entry.at("id").get<std::string>() + ": ";
    check(snapshot && snapshot->agents.total == 1 && agent,
          prefix + "agent presence");
    if (agent)
      check(statusToken(agent->status) ==
                entry.at("expectedAgentStatus").get<std::string>(),
            prefix + "agent status");
  }

  for (const auto &entry : contract.at("agentStatusCases")) {
    nodegraph::NodeGraph graph;
    NodeRef parent;
    {
      auto write = graph.write();
      parent = write.upsert({NodeKind::Thread, "parent"});
      const NodeRef turn = write.upsert({NodeKind::Turn, "turn"});
      const std::string id = entry.at("id").get<std::string>();
      NodeState activityState;
      activityState.fields = {
          {"type", "collabAgentToolCall"},
          {"tool", "spawn_agent"},
          {"kind", entry.at("kind").get<std::string>()},
          {"receiverThreadIds", nodegraph::Value(nodegraph::Value::Array{
                                    nodegraph::Value(id + "-child")})}};
      if (entry.contains("status")) {
        const std::string status = entry.at("status").get<std::string>();
        activityState.status = nodegraph::nodeStatusFromText(status);
        activityState.fields.emplace("status", status);
      }
      const NodeRef activity =
          write.upsert({NodeKind::Item, id}, std::move(activityState));
      write.setParent(parent, turn);
      write.setParent(turn, activity);
      static_cast<void>(write.finish());
    }
    NodeGraphUiAdapter adapter(graph);
    const auto snapshot =
        adapter.inspector(parent, InspectorProjection::Agents);
    const InspectorAgentRow *agent =
        snapshot ? pageValue<InspectorAgentRow>(snapshot->agents) : nullptr;
    const std::string prefix =
        "agent status contract " + entry.at("id").get<std::string>() + ": ";
    check(snapshot && snapshot->agents.total == 1 && agent,
          prefix + "agent presence");
    if (agent)
      check(statusToken(agent->status) ==
                entry.at("expectedAgentStatus").get<std::string>(),
            prefix + "agent status");
  }

  constexpr std::array SettingFields{
      std::pair{std::string_view("model"), TurnSettingField::Model},
      std::pair{std::string_view("effort"), TurnSettingField::Effort},
      std::pair{std::string_view("personality"), TurnSettingField::Personality},
      std::pair{std::string_view("sandbox"), TurnSettingField::Sandbox},
      std::pair{std::string_view("network"), TurnSettingField::Network},
      std::pair{std::string_view("approval"), TurnSettingField::Approval},
      std::pair{std::string_view("reviewer"), TurnSettingField::Reviewer},
      std::pair{std::string_view("cwd"), TurnSettingField::Workspace},
      std::pair{std::string_view("permissionProfile"),
                TurnSettingField::PermissionProfile},
      std::pair{std::string_view("serviceTier"), TurnSettingField::ServiceTier},
      std::pair{std::string_view("summary"), TurnSettingField::Summary},
      std::pair{std::string_view("collaboration"),
                TurnSettingField::Collaboration}};
  const auto settingField = [&](std::string_view name) {
    for (const auto &[candidate, field] : SettingFields)
      if (candidate == name)
        return field;
    return TurnSettingField::Count;
  };
  const auto touchedSettings = [&](const TurnSettingsPolicy &policy) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto &[name, field] : SettingFields)
      if (policy.touched(field))
        result.push_back(name);
    return result;
  };
  const auto settingCatalog = [](const TurnSettingsPolicy &policy) {
    nlohmann::json models = nlohmann::json::array();
    nlohmann::json modelLabels = nlohmann::json::array();
    nlohmann::json modelDescriptions = nlohmann::json::array();
    nlohmann::json modelDefaults = nlohmann::json::array();
    nlohmann::json personalitySupport = nlohmann::json::array();
    nlohmann::json defaultEfforts = nlohmann::json::array();
    nlohmann::json defaultTiers = nlohmann::json::array();
    nlohmann::json profiles = nlohmann::json::array();
    nlohmann::json profileDescriptions = nlohmann::json::array();
    for (const TurnSettingModel &model : policy.context().models) {
      models.push_back(model.choice.value);
      modelLabels.push_back(model.choice.label);
      modelDescriptions.push_back(model.choice.description);
      modelDefaults.push_back(model.isDefault);
      personalitySupport.push_back(model.supportsPersonality);
      defaultEfforts.push_back(model.defaultReasoningEffort);
      defaultTiers.push_back(model.defaultServiceTier);
    }
    for (const TurnSettingChoice &profile :
         policy.context().permissionProfiles) {
      profiles.push_back(profile.value);
      profileDescriptions.push_back(profile.description);
    }
    const TurnSettingModel *selected = policy.selectedModel();
    nlohmann::json efforts = nlohmann::json::array();
    nlohmann::json tiers = nlohmann::json::array();
    nlohmann::json tierLabels = nlohmann::json::array();
    nlohmann::json tierDescriptions = nlohmann::json::array();
    if (selected) {
      for (const std::string &effort : selected->reasoningEfforts)
        efforts.push_back(effort);
      for (const TurnSettingChoice &tier : selected->serviceTiers) {
        tiers.push_back(tier.value);
        tierLabels.push_back(tier.label);
        tierDescriptions.push_back(tier.description);
      }
    }
    return nlohmann::json{
        {"models", std::move(models)},
        {"modelLabels", std::move(modelLabels)},
        {"modelDescriptions", std::move(modelDescriptions)},
        {"modelDefaults", std::move(modelDefaults)},
        {"modelPersonalitySupport", std::move(personalitySupport)},
        {"defaultEfforts", std::move(defaultEfforts)},
        {"defaultServiceTiers", std::move(defaultTiers)},
        {"efforts", std::move(efforts)},
        {"serviceTiers", std::move(tiers)},
        {"serviceTierLabels", std::move(tierLabels)},
        {"serviceTierDescriptions", std::move(tierDescriptions)},
        {"permissionProfiles", std::move(profiles)},
        {"permissionProfileDescriptions", std::move(profileDescriptions)},
        {"selectedModel", selected ? selected->choice.value : std::string{}},
        {"personalityEnabled", policy.personalityEnabled()}};
  };

  for (const auto &entry : contract.at("turnSettingsCases")) {
    nodegraph::NodeGraph graph;
    nodegraph::ProtocolUpdater updater(graph);
    const auto catalogResult = [&updater](const std::string &method,
                                          const nlohmann::json &rows) {
      const nodegraph::ProtocolRequestId requestId("fixture:" + method);
      const nodegraph::ApplyResult request =
          updater.apply({nodegraph::DecodedMessageKind::ClientRequest,
                         method,
                         requestId,
                         {}});
      static_cast<void>(updater.apply(
          {nodegraph::DecodedMessageKind::ClientResult, method, requestId,
           objectFromJson(nlohmann::json{{"data", rows}}), request.primary}));
    };
    catalogResult("model/list", entry.at("catalogs").at("models"));
    catalogResult("permissionProfile/list",
                  entry.at("catalogs").at("permissionProfiles"));
    for (const auto &thread : entry.at("threads"))
      static_cast<void>(
          updater.apply({nodegraph::DecodedMessageKind::ServerNotification,
                         "thread/started", std::nullopt,
                         objectFromJson(nlohmann::json{
                             {"thread", thread.at("canonical")}})}));

    NodeGraphUiAdapter adapter(graph);
    TurnSettingsPolicy policy;
    std::unordered_map<std::string, TurnSettingEpochs> observedAcknowledgements;
    const auto &newThread = entry.at("newThread");
    const auto projectedNew =
        adapter.turnSettings({}, newThread.at("key").get<std::string>(),
                             newThread.at("cwd").get<std::string>());
    check(projectedNew &&
              projectedNew->identity ==
                  newThread.at("key").get<std::string>() &&
              projectedNew->canonical[TurnSettingField::Workspace] ==
                  newThread.at("cwd").get<std::string>(),
          "settings contract new-thread projection");

    nodegraph::NodeRef selectedThread;
    std::string selectedId;
    const auto select = [&](const std::string &id) {
      auto read = graph.tryRead();
      selectedThread = read->find({nodegraph::NodeKind::Thread, id});
      read.reset();
      const auto context = adapter.turnSettings(selectedThread);
      check(context.has_value(), "settings contract selected projection");
      if (context)
        static_cast<void>(policy.setContext(*context));
      selectedId = id;
    };
    const auto change = [&](const nlohmann::json &authored) {
      const TurnSettingField field =
          settingField(authored.at(0).get<std::string>());
      check(field != TurnSettingField::Count,
            "settings contract field is declared");
      if (field != TurnSettingField::Count)
        static_cast<void>(
            policy.change(field, authored.at(1).get<std::string>()));
    };
    const auto verify = [&](const nlohmann::json &expected) {
      const std::string prefix =
          "settings contract " + entry.at("id").get<std::string>() + ": ";
      if (expected.contains("identity"))
        check(policy.context().identity ==
                  expected.at("identity").get<std::string>(),
              prefix + "identity");
      if (expected.contains("values")) {
        for (const auto &[name, value] : expected.at("values").items()) {
          const TurnSettingField field = settingField(name);
          check(field != TurnSettingField::Count &&
                    policy.values()[field] == value.get<std::string>(),
                prefix + "value " + name);
        }
      }
      if (expected.contains("touched"))
        check(touchedSettings(policy) == expected.at("touched"),
              prefix + "touched");
      nlohmann::json acknowledged = nlohmann::json::array();
      TurnSettingEpochs &observed =
          observedAcknowledgements[policy.context().identity];
      for (const auto &[name, field] : SettingFields) {
        const std::size_t fieldIndex = static_cast<std::size_t>(field);
        if (policy.context().acknowledged[fieldIndex] > observed[fieldIndex])
          acknowledged.push_back(name);
        observed[fieldIndex] = std::max(
            observed[fieldIndex], policy.context().acknowledged[fieldIndex]);
      }
      if (expected.contains("acknowledged")) {
        check(acknowledged == expected.at("acknowledged"),
              prefix + "acknowledged");
      }
      if (expected.contains("catalog"))
        check(settingCatalog(policy) == expected.at("catalog"),
              prefix + "catalog");
      if (expected.contains("threadOptions"))
        check(policy.startOptions(codex::TurnSettingsScope::Thread) ==
                  expected.at("threadOptions"),
              prefix + "thread options");
      if (expected.contains("turnOptions"))
        check(policy.startOptions(codex::TurnSettingsScope::Turn) ==
                  expected.at("turnOptions"),
              prefix + "turn options");
    };

    for (const auto &step : entry.at("steps")) {
      if (step.contains("select"))
        select(step.at("select").get<std::string>());
      if (step.contains("change"))
        change(step.at("change"));
      for (const auto &authored :
           step.value("changes", nlohmann::json::array()))
        change(authored);
      nlohmann::json updates = step.value("updates", nlohmann::json::array());
      if (step.contains("update"))
        updates.insert(updates.begin(), step.at("update"));
      for (const auto &update : updates)
        static_cast<void>(updater.apply(
            {nodegraph::DecodedMessageKind::ServerNotification,
             "thread/settings/updated", std::nullopt,
             objectFromJson(nlohmann::json{{"threadId", selectedId},
                                           {"threadSettings", update}})}));
      if (!updates.empty())
        select(selectedId);
      if (step.contains("expect"))
        verify(step.at("expect"));
    }

    TurnSettingsContext replay = policy.context();
    static_cast<void>(policy.change(TurnSettingField::Approval, "never"));
    static_cast<void>(policy.setContext(replay));
    check(policy.touched(TurnSettingField::Approval) &&
              policy.values()[TurnSettingField::Approval] == "never",
          "settings policy preserves edits across a repeated context");
    const std::size_t approval =
        static_cast<std::size_t>(TurnSettingField::Approval);
    if (replay.acknowledged[approval] != 0)
      --replay.acknowledged[approval];
    static_cast<void>(policy.setContext(std::move(replay)));
    check(policy.touched(TurnSettingField::Approval) &&
              policy.values()[TurnSettingField::Approval] == "never",
          "settings policy rejects an older acknowledgement");
    replay = policy.context();
    replay.canonical[TurnSettingField::Approval] = "on-request";
    static_cast<void>(policy.setContext(std::move(replay)));
    check(policy.touched(TurnSettingField::Approval) &&
              policy.values()[TurnSettingField::Approval] == "never",
          "settings policy preserves authored values across snapshots");
    replay = policy.context();
    replay.canonical[TurnSettingField::Approval] = "never";
    replay.acknowledged[approval] += 2;
    static_cast<void>(policy.setContext(std::move(replay)));
    check(!policy.touched(TurnSettingField::Approval) &&
              policy.values()[TurnSettingField::Approval] == "never",
          "settings policy applies the field acknowledgement");
    replay = policy.context();
    static_cast<void>(policy.change(TurnSettingField::Approval, "untrusted"));
    replay.canonical[TurnSettingField::Approval] = "on-request";
    ++replay.acknowledged[approval];
    static_cast<void>(policy.setContext(std::move(replay)));
    check(policy.touched(TurnSettingField::Approval) &&
              policy.values()[TurnSettingField::Approval] == "untrusted",
          "settings policy preserves an edit newer than acknowledgement");
  }

  const auto actionTone = [](PendingRequestActionTone tone) {
    switch (tone) {
    case PendingRequestActionTone::Approve:
      return "approve";
    case PendingRequestActionTone::Danger:
      return "danger";
    case PendingRequestActionTone::Neutral:
      return "neutral";
    }
    return "neutral";
  };
  const auto fixtureRaw = [](const nlohmann::json &entry) {
    nlohmann::json raw = entry.at("raw");
    for (const auto &repeat : entry.value("repeats", nlohmann::json::array())) {
      nlohmann::json *target = &raw;
      const nlohmann::json &path = repeat.at("path");
      for (std::size_t index = 0; index + 1 < path.size(); ++index)
        target = &(*target)[path.at(index).get<std::string>()];
      const std::string key = path.back().get<std::string>();
      const std::size_t count = repeat.at("count").get<std::size_t>();
      if (repeat.contains("text")) {
        const std::string text = repeat.at("text").get<std::string>();
        std::string value;
        value.reserve(text.size() * count);
        for (std::size_t index = 0; index < count; ++index)
          value += text;
        (*target)[key] = std::move(value);
      } else {
        (*target)[key] = nlohmann::json::array();
        for (std::size_t index = 0; index < count; ++index)
          (*target)[key].push_back(repeat.at("element"));
      }
    }
    return raw;
  };
  for (const auto &entry : contract.at("pendingRequestCases")) {
    const std::string id = entry.at("id").get<std::string>();
    const PendingRequestKind kind = PendingRequestPolicy::kindForMethod(
        entry.at("method").get<std::string>());
    const std::string prefix = "pending request contract " + id + ": ";
    check(PendingRequestPolicy::kindToken(kind) ==
              entry.at("expectedKind").get<std::string>(),
          prefix + "kind");
    PendingRequestDescriptor request;
    request.kind = kind;
    request.raw = fixtureRaw(entry);
    nlohmann::json actions = nlohmann::json::array();
    for (const PendingRequestAction &candidate :
         PendingRequestPolicy::actions(request))
      actions.push_back({{"value", candidate.value},
                         {"label", candidate.label},
                         {"tone", actionTone(candidate.tone)},
                         {"requiresInput", candidate.requiresInput}});
    check(actions == entry.at("expectedActions"), prefix + "actions");

    for (const auto &attempt : entry.at("submissions")) {
      PendingRequestSubmission submission;
      submission.choice = attempt.at("choice").get<std::string>();
      if (attempt.contains("input"))
        submission.input = attempt.at("input");
      if (attempt.contains("metadata"))
        submission.metadata = attempt.at("metadata");
      const std::optional<PendingRequestResponse> response =
          PendingRequestPolicy::responseForSubmission(kind, request.raw,
                                                      std::move(submission));
      nlohmann::json actual = nullptr;
      if (response) {
        if (const auto *result = std::get_if<nlohmann::json>(&*response))
          actual = nlohmann::json{{"result", *result}};
        else
          actual =
              nlohmann::json{{"error",
                              {{"code", -32601},
                               {"message", std::get<std::string>(*response)}}}};
      }
      check(actual == attempt.at("expected"),
            prefix + "response " + attempt.at("id").get<std::string>());
    }
  }
  for (const auto &entry : contract.at("pendingRequestBoundaryCases")) {
    const PendingRequestKind kind = PendingRequestPolicy::kindForMethod(
        entry.at("method").get<std::string>());
    const nlohmann::json raw = fixtureRaw(entry);
    PendingRequestDescriptor request;
    request.kind = kind;
    request.raw = raw;
    nlohmann::json actions = nlohmann::json::array();
    for (const PendingRequestAction &candidate :
         PendingRequestPolicy::actions(request))
      actions.push_back(candidate.value);
    const std::string prefix =
        "pending boundary contract " + entry.at("id").get<std::string>() + ": ";
    check(actions == entry.at("expectedActionValues"), prefix + "actions");
    for (const auto &choice : entry.at("rejectedChoices"))
      check(!PendingRequestPolicy::responseForSubmission(
                kind, raw, {choice.get<std::string>()}),
            prefix + "reject " + choice.get<std::string>());
  }

  NodeState typedAuthority;
  typedAuthority.status = nodegraph::NodeStatus::Completed;
  typedAuthority.fields.emplace("status", "inProgress");
  check(statusFromState(typedAuthority).semantic ==
            nodegraph::NodeStatus::Completed,
        "typed NodeState status overrides conflicting raw status");
  return passed;
}

} // namespace
} // namespace codexui::codex::ui

int main() {
  using namespace codexui::codex::ui;
  bool passed = true;
  passed &= projectsCanonicalTurnStructureAndRoot();
  passed &= projectsEveryLoadedItemInCanonicalOrder();
  passed &= completeHistoryRetainsCanonicalItemOrder();
  passed &= completeHistoryProjectionScalesLinearly();
  passed &= projectsCanonicalAgentActivityLifecycle();
  passed &= boundedOptimisticPromptAdoptsProviderOrderInPlace();
  passed &= controllerAndObserverShareProviderConversationOrder();
  passed &= loadedSteeringPromptsKeepCanonicalOrder();
  passed &= projectsCanonicalRowsThroughOneDelta();
  passed &= projectsExactPromptMaterialization();
  passed &= projectsExactRowPlacementAndNeighbors();
  passed &= ordersStructuralRowsByCanonicalDependencies();
  passed &= routesRelationsIncrementallyAndTurnReordersAuthoritatively();
  passed &= preservesThreadRootsAndExactChildTargets();
  passed &= rejectsRetiredThreadIncarnationsAtEveryProjectionBoundary();
  passed &= preservesReadinessActivityAndProviderPaginationSemantics();
  passed &= projectsHistoryRequestLifecycleFromTheGraph();
  passed &= pendingRequestsPreserveExactGraphAuthority();
  passed &= pendingRequestOwnershipFollowsExactAncestry();
  passed &= pendingRequestProjectionWorkIsQuantitativelyBounded();
  passed &= provisionalTurnRequiresTypedAdmittedPrompt();
  passed &= structuredPlanPresenceAndEffectiveStatusAreCanonical();
  passed &= projectsBoundedInspectorPagesFromCanonicalGraph();
  passed &= settingsDraftLifetimeFollowsSemanticIncarnations();
  passed &= frontendPresentationContractIsShared();
  if (passed)
    std::cout << "NodeGraph UI adapter tests passed\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
