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
  const auto result = adapter.conversation(thread, 80, {true, true});
  if (!require(result.has_value(), "adapter projection was unavailable") ||
      !require(result->threadId == "thread-1", "wrong projected thread") ||
      !require(result->sections.size() == 2, "wrong turn count") ||
      !require(result->sections[0].cards.size() == 2,
               "wrong first-turn card count") ||
      !require(result->sections[1].cards.size() == 1,
               "wrong second-turn card count") ||
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
  const auto result = adapter.conversation(thread, 2, {true, true});
  return require(result.has_value(), "bounded projection unavailable") &&
         require(result->hasMore, "bounded projection lost Load More") &&
         require(result->hiddenAuthoritativeItemCount == 4,
                 "wrong hidden item count") &&
         require(result->sections.size() == 1,
                 "bounded projection lost its turn") &&
         require(result->sections[0].cards.size() == 3,
                 "root was not pinned beside retained suffix") &&
         require(result->sections[0].cards.front().key ==
                     *result->sections[0].rootCardKey,
                 "pinned root does not own the turn");
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
         require(result->roots[0].target == root,
                 "canonical root NodeRef changed") &&
         require(result->roots[0].children.size() == 1,
                 "child hierarchy was flattened") &&
         require(result->roots[0].children[0].target == child,
                 "child action target was reconstructed") &&
         require(result->roots[1].target == orphan,
                 "unreachable canonical thread was hidden");
}

} // namespace
} // namespace codexui::codex::ui

int main() {
  using namespace codexui::codex::ui;
  if (!projectsCanonicalTurnStructureAndRoot() ||
      !limitsHistoryButPinsTheOwningPrompt() ||
      !preservesThreadRootsAndExactChildTargets())
    return EXIT_FAILURE;
  std::cout << "NodeGraph UI adapter tests passed\n";
  return EXIT_SUCCESS;
}
