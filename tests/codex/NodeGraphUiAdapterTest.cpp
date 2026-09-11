// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/NodeGraphUiAdapter.h"

#include <cstdlib>
#include <iostream>
#include <string>

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
    thread = write.upsert({NodeKind::Thread, "thread-1"},
                          std::move(threadState));

    NodeState turnOneState;
    turnOneState.fields.emplace("id", "turn-1");
    firstTurn = write.upsert({NodeKind::Turn, "turn-1"},
                             std::move(turnOneState));
    firstPrompt = write.upsert(
        {NodeKind::Item, "prompt-1"},
        itemState("prompt-1", "userMessage", "first prompt"));
    answer = write.upsert(
        {NodeKind::Item, "answer-1"},
        itemState("answer-1", "agentMessage", "first answer"));

    NodeState turnTwoState;
    turnTwoState.fields.emplace("id", "turn-2");
    secondTurn = write.upsert({NodeKind::Turn, "turn-2"},
                              std::move(turnTwoState));
    secondPrompt = write.upsert(
        {NodeKind::Item, "prompt-2"},
        itemState("prompt-2", "userMessage", "second prompt"));

    write.setParent(thread, firstTurn);
    write.setParent(firstTurn, firstPrompt);
    write.setParent(firstTurn, answer);
    write.relate(firstTurn, nodegraph::RelationKind::TurnRootItem,
                 firstPrompt);
    write.setParent(thread, secondTurn);
    write.setParent(secondTurn, secondPrompt);
    write.relate(secondTurn, nodegraph::RelationKind::TurnRootItem,
                 secondPrompt);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto result = adapter.conversation(thread, 80);
  if (!require(result.has_value(), "adapter projection was unavailable") ||
      !require(result->threadId == "thread-1", "wrong projected thread") ||
      !require(result->sections.size() == 2, "wrong turn count") ||
      !require(result->sections[0].cards.size() == 2,
               "wrong first-turn card count") ||
      !require(result->sections[1].cards.size() == 1,
               "wrong second-turn card count") ||
      !require(result->sections[0].key ==
                   "turn:8:thread-16:turn-1",
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

bool limitsHistoryButPinsTheOwningPrompt() {
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
    turn = write.upsert({NodeKind::Turn, "turn-limit"},
                        std::move(turnState));
    write.setParent(thread, turn);
    prompt = write.upsert(
        {NodeKind::Item, "root"},
        itemState("root", "userMessage", "owning prompt"));
    write.setParent(turn, prompt);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, prompt);
    for (int index = 0; index < 5; ++index) {
      const std::string id = "answer-" + std::to_string(index);
      NodeRef item = write.upsert(
          {NodeKind::Item, id}, itemState(id, "agentMessage", id));
      write.setParent(turn, item);
    }
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto result = adapter.conversation(thread, 2);
  return require(result.has_value(), "bounded projection unavailable") &&
         require(result->hasMore, "bounded projection lost Load More") &&
         require(result->hiddenAuthoritativeItemCount == 3,
                 "wrong hidden item count") &&
         require(result->sections.size() == 1,
                 "bounded projection lost its turn") &&
         require(result->sections[0].cards.size() == 3,
                 "root was not pinned beside retained suffix") &&
         require(result->sections[0].rootPinned,
                 "bounded projection did not identify the retained owner") &&
         require(result->sections[0].cards.front().key ==
                     *result->sections[0].rootCardKey,
                 "pinned root does not own the turn");
}

bool boundedOptimisticPromptKeepsItsCanonicalSlot() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef root;
  NodeRef local;
  NodeRef answer;
  {
    auto write = graph.write();
    NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{2});
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
  const auto optimistic = adapter.conversation(thread, 80);
  const middle::CardKey rootKey = middle::AuthoritativeItemKey{
      "bounded-prompt-thread", "bounded-prompt-turn",
      "bounded-prompt-root"};
  const middle::CardKey steeringKey = middle::LocalPromptKey{77};
  const middle::CardKey answerKey = middle::AuthoritativeItemKey{
      "bounded-prompt-thread", "bounded-prompt-turn",
      "bounded-prompt-answer"};
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
    authoritative = write.upsert(
        {NodeKind::Item, "bounded-authoritative-steering"},
        itemState("bounded-authoritative-steering", "userMessage",
                  "Steer here"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{77});
    write.setParent(turn, authoritative);
    write.relate(authoritative,
                 nodegraph::RelationKind::PromptMaterialization, local);
    write.replaceChildren(
        turn, std::array<NodeRef, 4>{root, local, authoritative, answer});
    write.setField(thread, "historyLoadedItemCount", std::uint64_t{3});
    static_cast<void>(write.finish());
  }

  const auto materialized = adapter.conversation(thread, 80);
  return require(materialized && materialized->sections.size() == 1 &&
                     materialized->sections.front().cards.size() == 3,
                 "bounded steering materialization was unavailable") &&
         require(materialized->sections.front().cards[0].key == rootKey &&
                     materialized->sections.front().cards[1].key ==
                         steeringKey &&
                     materialized->sections.front().cards[2].key == answerKey,
                 "steering materialization changed the projected row order") &&
         require(materialized->sections.front().cards[1].kind ==
                         middle::CardKind::UserMessage &&
                     materialized->sections.front().cards[1].target == local,
                 "steering materialization did not preserve its stable local "
                 "row while awaiting UI acknowledgement");
}

bool projectsOnlyTheExactCanonicalTail() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef root;
  NodeRef tail;
  {
    auto write = graph.write();
    NodeState threadState;
    threadState.fields.emplace("historyLoadedItemCount", std::uint64_t{2});
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
  const auto projected = adapter.tailCard(thread, tail);
  return require(projected.has_value(),
                 "canonical last item was not projected") &&
         require(projected->card.target == tail && !projected->turnRoot &&
                     projected->nested && projected->activeTurn,
                 "tail projection lost exact NodeRef or turn placement") &&
         require(projected->authoritativeItemCount == 2 &&
                     projected->providerHasMore,
                 "tail projection lost authoritative history chrome") &&
         require(!adapter.tailCard(thread, root),
                 "a non-tail item entered the bounded append path");
}

bool projectsExactPromptMaterialization() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef prompt;
  NodeRef authoritative;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "prompt-thread"});
    turn = write.upsert({NodeKind::Turn, "prompt-turn"});
    write.setField(turn, "id", "prompt-turn");
    NodeState local = itemState("local-prompt", "localPrompt", "hello");
    local.fields.emplace("submissionId", std::uint64_t{91});
    local.fields.emplace("dispatchState", "awaitingMaterialization");
    prompt = write.upsert({NodeKind::Item, "local-prompt"}, std::move(local));
    authoritative = write.upsert(
        {NodeKind::Item, "provider-prompt"},
        itemState("provider-prompt", "userMessage", "hello"));
    write.setParent(thread, turn);
    write.setParent(turn, prompt);
    write.setParent(turn, authoritative);
    write.relate(authoritative,
                 nodegraph::RelationKind::PromptMaterialization, prompt);
    static_cast<void>(write.finish());
  }

  NodeGraphUiAdapter adapter(graph);
  const auto result = adapter.promptMaterialization(thread, authoritative);
  return require(result.has_value(),
                 "exact prompt materialization was not projected") &&
         require(result->card.key ==
                     middle::CardKey{middle::LocalPromptKey{91}},
                 "prompt materialization changed the stable local key") &&
         require(result->card.kind == middle::CardKind::UserMessage &&
                     result->card.target == authoritative &&
                     result->prompt == prompt,
                 "prompt materialization conflated authoritative row and "
                 "exact acknowledgement identities") &&
         require(!adapter.promptMaterialization(thread, prompt),
                 "a local prompt was accepted as its own materialization");
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
  auto placement = adapter.rowChange(thread, moved);
  const middle::CardKey firstKey = middle::AuthoritativeItemKey{
      "row-thread", "row-turn", "row-first"};
  const middle::CardKey lastKey = middle::AuthoritativeItemKey{
      "row-thread", "row-turn", "row-last"};
  bool result =
      require(placement && placement->placement.card.target == moved &&
                  placement->placement.sectionKey ==
                      "turn:10:row-thread8:row-turn" &&
                  placement->placement.nested &&
                  !placement->placement.turnRoot &&
                  placement->previousCardKey == firstKey &&
                  placement->nextCardKey == lastKey,
              "one row projection lost its exact placement or neighbors");

  {
    auto write = graph.write();
    write.replaceChildren(turn, std::array<NodeRef, 3>{first, last, moved});
    static_cast<void>(write.finish());
  }
  placement = adapter.rowChange(thread, moved);
  result &= require(placement && placement->previousCardKey == lastKey &&
                        !placement->nextCardKey,
                    "a canonical move did not update the exact row neighbors");
  {
    auto write = graph.write();
    const auto busy = adapter.rowChange(thread, moved);
    result &= require(busy.graphBusy && !busy,
                      "row projection distinguishes graph contention from an "
                      "authoritative removal");
    static_cast<void>(write.finish());
  }
  return result;
}

bool preservesReadinessActivityAndAuthoritativeBudgetSemantics() {
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
    const NodeRef provider = write.upsert(
        {NodeKind::Item, "provider-item"},
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
  return require(info && !info->readyForDisplay &&
                     !info->hydrationFailed && info->providerHasMore &&
                     info->authoritativeItemCount == 1,
                 "hydration or authoritative history budget changed") &&
         require(threads && !threads->providerReady && !threads->canControl,
                 "disconnected transport exposed ready thread controls") &&
         require(threads && threads->roots.size() == 1 &&
                     threads->roots.front().title == "Chosen thread" &&
                     threads->roots.front().recencyAt ==
                         std::optional<std::int64_t>{8} &&
                     threads->roots.front().lastActivityAt ==
                         std::optional<std::int64_t>{9} &&
                     threads->roots.front().awaitingPromptAcknowledgement &&
                     threads->roots.front().pendingPromptAdmittedAtMs ==
                         std::optional<std::int64_t>{1234},
                 "chosen-name, Recent, activity, or prompt lifecycle "
                 "projection changed");
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
    orphan =
        write.upsert({NodeKind::Thread, "orphan"}, std::move(orphanState));
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

} // namespace
} // namespace codexui::codex::ui

int main() {
  using namespace codexui::codex::ui;
  if (!projectsCanonicalTurnStructureAndRoot() ||
      !limitsHistoryButPinsTheOwningPrompt() ||
      !boundedOptimisticPromptKeepsItsCanonicalSlot() ||
      !projectsOnlyTheExactCanonicalTail() ||
      !projectsExactPromptMaterialization() ||
      !projectsExactRowPlacementAndNeighbors() ||
      !preservesThreadRootsAndExactChildTargets() ||
      !preservesReadinessActivityAndAuthoritativeBudgetSemantics())
    return EXIT_FAILURE;
  std::cout << "NodeGraph UI adapter tests passed\n";
  return EXIT_SUCCESS;
}
