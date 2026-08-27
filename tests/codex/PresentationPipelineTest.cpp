// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PresentationModel.h"
#include "codex/PresentationProtocol.h"
#include "codex/ProtocolNormalizer.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

std::string stringMember(const nlohmann::json &value, const char *name) {
  const auto member = value.find(name);
  return member != value.end() && member->is_string()
             ? member->get<std::string>()
             : std::string{};
}

} // namespace

int main() {
  using codexui::codex::PresentationModel;
  using codexui::codex::ProtocolNormalizer;

  PresentationModel model;
  std::vector<nlohmann::json> frames;
  ProtocolNormalizer normalizer([&](const nlohmann::json &frame) {
    frames.push_back(frame);
    model.applyEvent(frame);
    return true;
  });

  normalizer.transportEvent("connected");
  normalizer.bridgeEvent({{"kind", "bridge.connection"},
                          {"event", "connected"},
                          {"connectionId", "frontend-test"},
                          {"role", "observer"}});
  normalizer.bridgeEvent({{"kind", "bridge.controller"},
                          {"controllerConnectionId", "frontend-test"}});
  normalizer.connectionSettings(
      {{"selected", "ipv6"},
       {"available", nlohmann::json::array({{{"key", "ipv6"},
                                             {"label", "IPv6"},
                                             {"kind", "network"},
                                             {"host", "::1"},
                                             {"port", 4500},
                                             {"tls", false}}})}});

  normalizer.operationResult(
      "threads.list", "list-1", nlohmann::json::object(),
      {{"id", "list-1"},
       {"result",
        {{"data", nlohmann::json::array({{{"id", "thread-1"},
                                          {"preview", "Architecture pipeline"},
                                          {"cwd", "/workspace"},
                                          {"status", {{"type", "idle"}}}}})},
         {"nextCursor", nullptr},
         {"backwardsCursor", nullptr}}}});

  normalizer.operationResult(
      "thread.read", "read-1", {{"threadId", "thread-1"}},
      {{"id", "read-1"},
       {"result",
        {{"thread",
          {{"id", "thread-1"},
           {"preview", "Architecture pipeline"},
           {"cwd", "/workspace"},
           {"status", {{"type", "idle"}}},
           {"turns",
            nlohmann::json::array(
                {{{"id", "turn-1"},
                  {"status", "completed"},
                  {"items", nlohmann::json::array(
                                {{{"id", "user-1"},
                                  {"type", "userMessage"},
                                  {"content",
                                   nlohmann::json::array(
                                       {{{"type", "text"},
                                         {"text", "Inspect"}}})}}})}}})}}}}}});

  normalizer.serverNotification("turn/started",
                                {{"threadId", "thread-1"},
                                 {"turn",
                                  {{"id", "turn-2"},
                                   {"status", "inProgress"},
                                   {"items", nlohmann::json::array()}}}});
  normalizer.serverNotification("item/started",
                                {{"threadId", "thread-1"},
                                 {"turnId", "turn-2"},
                                 {"item",
                                  {{"id", "command-1"},
                                   {"type", "commandExecution"},
                                   {"command", "printf PIPELINE_OK"},
                                   {"cwd", "/workspace"},
                                   {"status", "inProgress"},
                                   {"aggregatedOutput", nullptr},
                                   {"exitCode", nullptr}}}});
  normalizer.serverNotification("item/commandExecution/outputDelta",
                                {{"threadId", "thread-1"},
                                 {"turnId", "turn-2"},
                                 {"itemId", "command-1"},
                                 {"delta", "PIPELINE_OK\n"}});
  normalizer.serverNotification("item/completed",
                                {{"threadId", "thread-1"},
                                 {"turnId", "turn-2"},
                                 {"item",
                                  {{"id", "command-1"},
                                   {"type", "commandExecution"},
                                   {"command", "printf PIPELINE_OK"},
                                   {"cwd", "/workspace"},
                                   {"status", "completed"},
                                   {"aggregatedOutput", "PIPELINE_OK\n"},
                                   {"exitCode", 0}}}});
  normalizer.serverNotification(
      "turn/diff/updated",
      {{"threadId", "thread-1"},
       {"turnId", "turn-2"},
       {"diff", "diff --git a/README.md b/README.md\n+PIPELINE_OK\n"}});
  normalizer.serverNotification(
      "turn/plan/updated",
      {{"threadId", "thread-1"},
       {"turnId", "turn-2"},
       {"explanation", "Keep live inspector state"},
       {"plan", nlohmann::json::array({{{"step", "Retain the plan"},
                                        {"status", "completed"}}})}});
  normalizer.serverNotification("turn/completed",
                                {{"threadId", "thread-1"},
                                 {"turn",
                                  {{"id", "turn-2"},
                                   {"status", "completed"},
                                   {"items", nlohmann::json::array()}}}});

  normalizer.operationResult("thread.read", "read-2",
                             {{"threadId", "thread-1"}},
                             {{"id", "read-2"},
                              {"result",
                               {{"thread",
                                 {{"id", "thread-1"},
                                  {"preview", "Architecture pipeline"},
                                  {"cwd", "/workspace"},
                                  {"status", {{"type", "idle"}}},
                                  {"turns", nlohmann::json::array()}}}}}});

  normalizer.operationResult(
      "thread.resume", "resume-1", {"threadId", "thread-1"},
      {{"id", "resume-1"},
       {"result",
        {{"thread", {{"id", "thread-1"}}},
         {"model", "gpt-current"},
         {"reasoningEffort", "high"},
         {"approvalPolicy", "never"},
         {"sandbox", "workspaceWrite"}}}});
  normalizer.serverNotification(
      "thread/settings/updated",
      {{"threadId", "thread-1"},
       {"threadSettings",
        {{"model", "gpt-current"}, {"personality", "friendly"}}}});
  normalizer.serverNotification(
      "thread/settings/updated",
      {{"threadId", "thread-1"},
       {"threadSettings", {{"personality", nullptr}}}});
  normalizer.serverNotification(
      "thread/settings/updated",
      {{"threadId", "thread-2"},
       {"threadSettings", {{"model", "gpt-background"}}}});

  bool validFrames = !frames.empty();
  std::uint64_t expectedSequence = 1;
  for (const nlohmann::json &frame : frames) {
    validFrames &= codexui::codex::presentation::isPresentationFrame(frame);
    validFrames &= frame.value("sequence", 0ULL) == expectedSequence++;
    validFrames &= frame.value("generation", 0ULL) == 1;
  }

  const auto *thread = model.thread("thread-1");
  const auto *turn = thread == nullptr
                         ? nullptr
                         : [&]() -> const codexui::codex::TurnPresentation * {
    const auto found = thread->turns.find("turn-2");
    return found == thread->turns.end() ? nullptr : &found->second;
  }();
  const auto *item = turn == nullptr
                         ? nullptr
                         : [&]() -> const codexui::codex::ItemPresentation * {
    const auto found = turn->items.find("command-1");
    return found == turn->items.end() ? nullptr : &found->second;
  }();

  bool passed = true;
  passed &= expect(validFrames,
                   "normalizer emits ordered versioned generation frames");
  passed &= expect(
      model.connection().connected &&
          model.connection().connectionId == "frontend-test" &&
          model.connection().role == "controller" &&
          stringMember(model.connection().settings, "selected") == "ipv6",
      "connection, controller, and transport settings form coherent state");
  passed &= expect(thread != nullptr && thread->turnOrder.size() == 2 &&
                       thread->cwd == "/workspace" &&
                       stringMember(thread->raw, "model") == "gpt-current" &&
                       stringMember(thread->raw, "reasoningEffort") == "high",
                   "list, full read, and live events retain one stable thread");
  const auto settings =
      thread == nullptr ? nullptr
                        : &thread->domains.at("thread.settings.changed")
                               .at("threadSettings");
  const auto *background = model.thread("thread-2");
  passed &= expect(
      settings != nullptr && settings->contains("personality") &&
          settings->at("personality").is_null() &&
          thread->settingsRevision == 2 && background != nullptr &&
          background->settingsRevision == 1 &&
          stringMember(background->latestSettingsUpdate, "model") ==
              "gpt-background",
      "settings updates retain explicit defaults and remain thread scoped");
  passed &= expect(turn != nullptr && turn->status == "completed" &&
                       turn->itemOrder.size() == 1,
                   "live turn lifecycle resolves one stable turn");
  passed &= expect(
      item != nullptr &&
          stringMember(item->raw, "command") == "printf PIPELINE_OK" &&
          stringMember(item->raw, "aggregatedOutput") == "PIPELINE_OK\n" &&
          item->raw.value("exitCode", -1) == 0 &&
          stringMember(item->raw, "status") == "completed",
      "command lifecycle retains its authoritative result");
  passed &=
      expect(turn != nullptr &&
                 stringMember(turn->domains.at("turn.diff.changed"), "diff") ==
                     "diff --git a/README.md b/README.md\n+PIPELINE_OK\n",
             "live turn diff is retained in its authoritative turn scope");
  passed &=
      expect(turn != nullptr &&
                 stringMember(turn->plan, "explanation") ==
                     "Keep live inspector state" &&
                 turn->plan.value("steps", nlohmann::json::array()).size() == 1,
             "incomplete thread reads preserve live plan and inspector state");
  passed &= expect(!model.activeTurnId("thread-1").has_value(),
                   "completed stream leaves no active turn");
  passed &= expect(model.thread("thread-1") != nullptr &&
                       model.thread("thread-1")->status == "idle",
                   "completed stream retains idle thread status");

  normalizer.operationResult(
      "thread.read", "stale-active-read", {{"threadId", "thread-1"}},
      {{"id", "stale-active-read"},
       {"result",
        {{"thread",
          {{"id", "thread-1"},
           {"status", {{"type", "active"}}},
           {"turns",
            nlohmann::json::array(
                {{{"id", "turn-1"}, {"status", "completed"}},
                 {{"id", "turn-2"}, {"status", "inProgress"}}})}}}}}});
  thread = model.thread("thread-1");
  turn = thread == nullptr ? nullptr : &thread->turns.at("turn-2");
  passed &= expect(turn != nullptr && turn->status == "completed" &&
                       !model.activeTurnId("thread-1").has_value(),
                   "a stale authoritative read cannot reactivate a completed "
                   "turn");
  passed &= expect(thread != nullptr && thread->status == "idle",
                   "a stale authoritative read cannot restore running thread "
                   "chrome");

  normalizer.serverNotification(
      "turn/started",
      {{"threadId", "thread-1"}, {"turn", {{"id", "turn-3"}}}});
  passed &= expect(model.activeTurnId("thread-1") == "turn-3" &&
                       model.thread("thread-1")->status == "active",
                   "started lifecycle supplies a missing active status");
  normalizer.serverNotification(
      "thread/status/changed",
      {{"threadId", "thread-1"}, {"status", {{"type", "completed"}}}});
  passed &= expect(!model.activeTurnId("thread-1").has_value(),
                   "completed thread chrome vetoes a stale active turn");
  normalizer.serverNotification(
      "turn/completed",
      {{"threadId", "thread-1"}, {"turn", {{"id", "turn-3"}}}});
  passed &= expect(!model.activeTurnId("thread-1").has_value(),
                   "completed lifecycle clears activity without turn status");

  PresentationModel hydratedModel;
  ProtocolNormalizer hydratedNormalizer(
      [&](const nlohmann::json &frame) {
        hydratedModel.applyEvent(frame);
        return true;
      });
  hydratedNormalizer.transportEvent("connected");
  hydratedNormalizer.operationResult(
      "thread.read", "repository-hints", {{"threadId", "repository-thread"}},
      {{"id", "repository-hints"},
       {"result",
        {{"thread",
          {{"id", "repository-thread"},
           {"cwd", "/workspace"},
           {"turns",
            nlohmann::json::array(
                {{{"id", "repository-turn"},
                  {"items",
                   nlohmann::json::array(
                       {{{"id", "repository-command"},
                         {"type", "commandExecution"},
                         {"cwd", "/workspace/project/src"}},
                        {{"id", "repository-change"},
                         {"type", "fileChange"},
                         {"changes",
                          nlohmann::json::array(
                              {{{"path", "lib/example.cpp"}},
                               {{"path", "removed.txt"}}})}}})}}})}}}}}});
  const auto *repositoryThread = hydratedModel.thread("repository-thread");
  passed &= expect(
      repositoryThread != nullptr &&
          repositoryThread->commandCwds ==
              std::vector<std::string>{"/workspace/project/src"} &&
          repositoryThread->changedPaths ==
              std::vector<std::string>{"lib/example.cpp", "removed.txt"},
      "authoritative thread hydration retains compact repository hints");

  PresentationModel orderingModel;
  orderingModel.applyEvent(codexui::codex::presentation::event(
      1, 1, "thread.upsert", {{"thread", {{"id", "retained-a"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "retained-a"}}));
  orderingModel.applyEvent(codexui::codex::presentation::event(
      2, 1, "thread.upsert", {{"thread", {{"id", "retained-b"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "retained-b"}}));
  orderingModel.applyEvent(codexui::codex::presentation::result(
      3, 1, "threads.list", "ordered-threads", true,
      {{"threads",
        nlohmann::json::array({{{"id", "provider-a"}},
                               {{"id", "provider-b"}},
                               {{"id", "provider-a"}}})}},
      codexui::codex::presentation::Authority::Merge));
  passed &= expect(
      orderingModel.threadOrder() ==
          std::vector<std::string>{"provider-a", "provider-b", "retained-b",
                                   "retained-a"},
      "thread discovery preserves provider order and one retained tail");

  PresentationModel ownershipModel;
  ownershipModel.applyEvent(codexui::codex::presentation::result(
      1, 1, "threads.list", "ownership-roots", true,
      {{"threads",
        nlohmann::json::array({{{"id", "parent"}},
                               {{"id", "child-one"}},
                               {{"id", "second-root"}}})}},
      codexui::codex::presentation::Authority::Merge));
  ownershipModel.applyEvent(codexui::codex::presentation::event(
      2, 1, "conversation.item.upsert",
      {{"item",
        {{"id", "spawn-one"},
         {"type", "subAgentActivity"},
         {"status", "started"},
         {"agentThreadId", "child-one"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "parent"},
       {"turnId", "parent-turn"},
       {"itemId", "spawn-one"}}));
  ownershipModel.applyEvent(codexui::codex::presentation::event(
      3, 1, "conversation.item.upsert",
      {{"item",
        {{"id", "spawn-one"},
         {"type", "subAgentActivity"},
         {"status", "started"},
         {"agentThreadId", "child-one"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "parent"},
       {"turnId", "parent-turn"},
       {"itemId", "spawn-one"}}));
  ownershipModel.applyEvent(codexui::codex::presentation::event(
      4, 1, "conversation.item.upsert",
      {{"item",
        {{"id", "spawn-two"},
         {"type", "subAgentActivity"},
         {"status", "started"},
         {"agentThreadId", "child-two"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "parent"},
       {"turnId", "parent-turn"},
       {"itemId", "spawn-two"}}));
  ownershipModel.applyEvent(codexui::codex::presentation::event(
      5, 1, "conversation.item.upsert",
      {{"item",
        {{"id", "spawn-grandchild"},
         {"type", "subAgentActivity"},
         {"status", "started"},
         {"agentThreadId", "grandchild"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "child-one"},
       {"turnId", "child-turn"},
       {"itemId", "spawn-grandchild"}}));
  const auto *childOneOwnership =
      ownershipModel.childOwnership("child-one");
  const auto *grandchildOwnership =
      ownershipModel.childOwnership("grandchild");
  const auto *parent = ownershipModel.thread("parent");
  const auto *childOne = ownershipModel.thread("child-one");
  passed &= expect(
      childOneOwnership && childOneOwnership->parentThreadId == "parent" &&
          childOneOwnership->agentId == "spawn-one" &&
          grandchildOwnership &&
          grandchildOwnership->parentThreadId == "child-one" &&
          parent &&
          parent->childThreadOrder ==
              std::vector<std::string>{"child-one", "child-two"} &&
          childOne && childOne->childThreadOrder ==
                          std::vector<std::string>{"grandchild"} &&
          ownershipModel.threadOrder() ==
              std::vector<std::string>{"parent", "second-root"},
      "ownership is unique, ordered, nested, and excluded from root order");

  ownershipModel.applyEvent(codexui::codex::presentation::event(
      6, 1, "agents.activity.upsert",
      {{"activity",
        {{"id", "peer-interaction"},
         {"type", "subAgentActivity"},
         {"kind", "interacted"},
         {"agentPath", "/root/child-two"},
         {"agentThreadId", "child-two"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "child-one"},
       {"turnId", "child-turn"},
       {"itemId", "peer-interaction"}}));
  parent = ownershipModel.thread("parent");
  childOne = ownershipModel.thread("child-one");
  passed &= expect(
      parent && childOne &&
          parent->childThreadOrder ==
              std::vector<std::string>{"child-one", "child-two"} &&
          childOne->childThreadOrder ==
              std::vector<std::string>{"grandchild"} &&
          ownershipModel.childOwnership("child-two") &&
          ownershipModel.childOwnership("child-two")->parentThreadId ==
              "parent" &&
          !childOne->agents.contains("peer-interaction") &&
          parent->agents.at("spawn-two").status == "started" &&
          stringMember(parent->agents.at("spawn-two").raw, "agentPath") ==
              "/root/child-two",
      "peer interaction cannot reparent a sibling as a nested child");

  ownershipModel.applyEvent(codexui::codex::presentation::event(
      7, 1, "turn.upsert",
      {{"turn", {{"id", "child-turn"}, {"status", "completed"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "child-one"}, {"turnId", "child-turn"}}));
  ownershipModel.applyEvent(codexui::codex::presentation::event(
      8, 1, "conversation.item.upsert",
      {{"item",
        {{"id", "child-answer"},
         {"type", "agentMessage"},
         {"text", "direct child result"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "child-one"},
       {"turnId", "child-turn"},
       {"itemId", "child-answer"}}));
  parent = ownershipModel.thread("parent");
  const auto ownerAgent = parent == nullptr
                              ? nullptr
                              : [&]() -> const codexui::codex::AgentPresentation * {
                                  const auto found =
                                      parent->agents.find("spawn-one");
                                  return found == parent->agents.end()
                                             ? nullptr
                                             : &found->second;
                                }();
  const auto *ownerSourceItem =
      parent ? &parent->turns.at("parent-turn").items.at("spawn-one")
             : nullptr;
  passed &= expect(
      ownerAgent && ownerAgent->status == "completed" &&
          stringMember(ownerAgent->raw, "resultText") ==
              "direct child result" &&
          ownerSourceItem &&
          stringMember(ownerSourceItem->raw, "resultText") ==
              "direct child result" &&
          parent->agents.at("spawn-two").status == "started",
      "child completion and results route only to the indexed owning agent");

  ownershipModel.applyEvent(codexui::codex::presentation::event(
      9, 1, "agents.activity.upsert",
      {{"activity",
        {{"id", "wait-state"},
         {"type", "collabAgentToolCall"},
         {"tool", "wait_agent"},
         {"agentsStates",
          {{"child-one",
            {{"status", "completed"},
             {"message", "state-correlated result"}}}}}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "parent"},
       {"turnId", "parent-turn"},
       {"itemId", "wait-state"}}));
  parent = ownershipModel.thread("parent");
  ownerSourceItem =
      parent ? &parent->turns.at("parent-turn").items.at("spawn-one")
             : nullptr;
  passed &= expect(
      parent &&
          stringMember(parent->agents.at("spawn-one").raw, "resultText") ==
              "state-correlated result" &&
          ownerSourceItem &&
          stringMember(ownerSourceItem->raw, "resultText") ==
              "state-correlated result",
      "agent state results route to the indexed owner and its source item");

  ownershipModel.applyEvent(codexui::codex::presentation::result(
      10, 1, "thread.read", "replace-child", true,
      {{"thread",
        {{"id", "child-one"},
         {"status", {{"type", "idle"}}},
         {"turns",
          nlohmann::json::array(
              {{{"id", "child-turn"},
                {"items",
                 nlohmann::json::array(
                     {{{"id", "spawn-grandchild"},
                       {"type", "subAgentActivity"},
                       {"status", "started"},
                       {"agentThreadId", "grandchild"}}})}}})}}}},
      codexui::codex::presentation::Authority::Replace,
      {{"threadId", "child-one"}}));
  parent = ownershipModel.thread("parent");
  passed &= expect(
      parent && parent->agents.at("spawn-one").status == "idle" &&
          !parent->agents.at("spawn-one").raw.contains("resultText") &&
          ownershipModel.childOwnership("child-one") != nullptr &&
          ownershipModel.childOwnership("grandchild") != nullptr,
      "authoritative child hydration clears stale results without losing ownership");

  ownershipModel.applyEvent(codexui::codex::presentation::result(
      11, 1, "threads.list", "relisted-owned-child", true,
      {{"threads",
        nlohmann::json::array({{{"id", "child-one"}},
                               {{"id", "second-root"}},
                               {{"id", "parent"}}})}},
      codexui::codex::presentation::Authority::Merge));
  passed &= expect(
      ownershipModel.threadOrder() ==
          std::vector<std::string>{"second-root", "parent"},
      "thread relisting cannot reintroduce an owned child as a root");

  ownershipModel.applyEvent(codexui::codex::presentation::result(
      12, 1, "thread.read", "replace-parent", true,
      {{"thread",
        {{"id", "parent"},
         {"turns",
          nlohmann::json::array(
              {{{"id", "replacement-turn"},
                {"items",
                 nlohmann::json::array(
                     {{{"id", "spawn-two"},
                       {"type", "subAgentActivity"},
                       {"status", "started"},
                       {"agentThreadId", "child-two"}},
                      {{"id", "spawn-one"},
                       {"type", "subAgentActivity"},
                       {"status", "completed"},
                       {"agentThreadId", "child-one"}}})}}})}}}},
      codexui::codex::presentation::Authority::Replace,
      {{"threadId", "parent"}}));
  parent = ownershipModel.thread("parent");
  passed &= expect(
      parent &&
          parent->childThreadOrder ==
              std::vector<std::string>{"child-two", "child-one"} &&
          ownershipModel.childOwnership("child-one") &&
          ownershipModel.childOwnership("child-two") &&
          ownershipModel.threadOrder() ==
              std::vector<std::string>{"second-root", "parent"},
      "authoritative parent hydration rebuilds ordered ownership in one pass");

  ownershipModel.applyEvent(codexui::codex::presentation::event(
      13, 1, "thread.removed", nlohmann::json::object(),
      codexui::codex::presentation::Authority::Remove,
      {{"threadId", "child-one"}}));
  parent = ownershipModel.thread("parent");
  passed &= expect(
      ownershipModel.thread("child-one") == nullptr && parent &&
          parent->childThreadOrder ==
              std::vector<std::string>{"child-two"} &&
          ownershipModel.childOwnership("child-one") == nullptr &&
          ownershipModel.childOwnership("grandchild") == nullptr &&
          ownershipModel.threadOrder() ==
              std::vector<std::string>{"second-root", "parent",
                                       "grandchild"},
      "authoritative child removal prunes ownership and promotes surviving descendants");

  ownershipModel.applyEvent(codexui::codex::presentation::event(
      14, 1, "thread.removed", nlohmann::json::object(),
      codexui::codex::presentation::Authority::Remove,
      {{"threadId", "parent"}}));
  passed &= expect(
      ownershipModel.thread("parent") == nullptr &&
          ownershipModel.childOwnership("child-two") == nullptr &&
          ownershipModel.threadOrder() ==
              std::vector<std::string>{"second-root", "child-two",
                                       "grandchild"},
      "authoritative parent removal promotes children in retained root order");

  ownershipModel.applyEvent(codexui::codex::presentation::event(
      15, 1, "connection.provider",
      {{"generation", std::uint64_t{1}}, {"state", "disconnected"}},
      codexui::codex::presentation::Authority::Replace));
  passed &= expect(ownershipModel.threadOrder().empty() &&
                       ownershipModel.thread("child-two") == nullptr &&
                       ownershipModel.childOwnership("child-two") == nullptr,
                   "provider loss clears threads and ownership atomically");

  PresentationModel reconnectOwnershipModel;
  reconnectOwnershipModel.applyEvent(codexui::codex::presentation::result(
      1, 1, "thread.read", "hydrate-owner", true,
      {{"thread",
        {{"id", "hydrated-parent"},
         {"turns",
          nlohmann::json::array(
              {{{"id", "hydrated-turn"},
                {"items",
                 nlohmann::json::array(
                     {{{"id", "hydrated-spawn"},
                       {"type", "subAgentActivity"},
                       {"status", "started"},
                       {"agentThreadId", "hydrated-child"}}})}}})}}}},
      codexui::codex::presentation::Authority::Replace,
      {{"threadId", "hydrated-parent"}}));
  reconnectOwnershipModel.applyEvent(codexui::codex::presentation::result(
      2, 1, "thread.read", "hydrate-child", true,
      {{"thread",
        {{"id", "hydrated-child"},
         {"turns",
          nlohmann::json::array(
              {{{"id", "child-turn"},
                {"status", "completed"},
                {"items",
                 nlohmann::json::array(
                     {{{"id", "nested-spawn"},
                       {"type", "subAgentActivity"},
                       {"status", "started"},
                       {"agentThreadId", "hydrated-grandchild"}},
                      {{"id", "hydrated-result"},
                       {"type", "agentMessage"},
                       {"text", "hydrated answer"}}})}}})}}}},
      codexui::codex::presentation::Authority::Replace,
      {{"threadId", "hydrated-child"}}));
  const auto *hydratedParent =
      reconnectOwnershipModel.thread("hydrated-parent");
  const auto *hydratedSourceItem =
      hydratedParent
          ? &hydratedParent->turns.at("hydrated-turn")
                 .items.at("hydrated-spawn")
          : nullptr;
  passed &= expect(
      hydratedParent &&
          reconnectOwnershipModel.childOwnership("hydrated-child") &&
          reconnectOwnershipModel.childOwnership("hydrated-grandchild") &&
          hydratedParent->agents.at("hydrated-spawn").status == "completed" &&
          stringMember(hydratedParent->agents.at("hydrated-spawn").raw,
                       "resultText") == "hydrated answer" &&
          hydratedSourceItem &&
          stringMember(hydratedSourceItem->raw, "status") == "completed" &&
          stringMember(hydratedSourceItem->raw, "resultText") ==
              "hydrated answer",
      "parent-first and nested child hydration retain direct correlation");
  reconnectOwnershipModel.applyEvent(codexui::codex::presentation::event(
      1, 2, "connection.lifecycle", {{"state", "connected"}},
      codexui::codex::presentation::Authority::Replace));
  reconnectOwnershipModel.applyEvent(codexui::codex::presentation::result(
      2, 2, "threads.list", "post-reconnect-roots", true,
      {{"threads",
        nlohmann::json::array({{{"id", "hydrated-child"}},
                               {{"id", "hydrated-parent"}}})}},
      codexui::codex::presentation::Authority::Merge));
  passed &= expect(
      reconnectOwnershipModel.childOwnership("hydrated-child") &&
          reconnectOwnershipModel.threadOrder() ==
              std::vector<std::string>{"hydrated-parent"},
      "connection-generation reconnect preserves ownership and root filtering");

  PresentationModel reboundOwnershipModel;
  reboundOwnershipModel.applyEvent(codexui::codex::presentation::event(
      1, 1, "thread.upsert", {{"thread", {{"id", "rebind-parent"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "rebind-parent"}}));
  reboundOwnershipModel.applyEvent(codexui::codex::presentation::event(
      2, 1, "conversation.item.upsert",
      {{"item",
        {{"type", "subAgentActivity"},
         {"id", "stable-agent"},
         {"status", "completed"},
         {"resultText", "old result"},
         {"agentThreadId", "old-child"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "rebind-parent"},
       {"turnId", "rebind-turn"},
       {"itemId", "stable-agent"}}));
  reboundOwnershipModel.applyEvent(codexui::codex::presentation::event(
      3, 1, "agents.activity.upsert",
      {{"activity",
        {{"type", "subAgentActivity"},
         {"status", "started"},
         {"agentThreadId", "new-child"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "rebind-parent"},
       {"turnId", "rebind-turn"},
       {"itemId", "stable-agent"}}));
  const auto *rebindParent =
      reboundOwnershipModel.thread("rebind-parent");
  passed &= expect(
      rebindParent &&
          rebindParent->childThreadOrder ==
              std::vector<std::string>{"new-child"} &&
          reboundOwnershipModel.childOwnership("old-child") == nullptr &&
          reboundOwnershipModel.childOwnership("new-child") &&
          reboundOwnershipModel.threadOrder() ==
              std::vector<std::string>{"rebind-parent", "old-child"},
      "rebinding one stable agent detaches and promotes the old child");
  passed &= expect(
      rebindParent &&
          rebindParent->agents.at("stable-agent").childThreadId ==
              "new-child" &&
          rebindParent->agents.at("stable-agent").status == "started" &&
          !rebindParent->agents.at("stable-agent").raw.contains("resultText") &&
          !rebindParent->turns.at("rebind-turn")
               .items.at("stable-agent")
               .raw.contains("resultText"),
      "rebinding one stable agent resets its stale completion and result");
  reboundOwnershipModel.applyEvent(codexui::codex::presentation::event(
      4, 1, "turn.upsert",
      {{"turn", {{"id", "new-child-turn"}, {"status", "completed"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "new-child"}, {"turnId", "new-child-turn"}}));
  reboundOwnershipModel.applyEvent(codexui::codex::presentation::result(
      5, 1, "thread.read", "stale-parent-merge", true,
      {{"thread",
        {{"id", "rebind-parent"},
         {"turns",
          nlohmann::json::array(
              {{{"id", "rebind-turn"},
                {"items",
                 nlohmann::json::array(
                     {{{"id", "stable-agent"},
                       {"type", "subAgentActivity"},
                       {"status", "inProgress"},
                       {"kind", "started"},
                       {"agentThreadId", "new-child"}}})}}})}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "rebind-parent"}}));
  rebindParent = reboundOwnershipModel.thread("rebind-parent");
  const auto *reboundSourceItem =
      rebindParent
          ? &rebindParent->turns.at("rebind-turn").items.at("stable-agent")
          : nullptr;
  passed &= expect(
      rebindParent &&
          rebindParent->agents.at("stable-agent").status == "completed" &&
          reboundSourceItem &&
          stringMember(reboundSourceItem->raw, "status") == "completed",
      "stale merged parent hydration cannot reactivate a completed child agent");
  reboundOwnershipModel.applyEvent(codexui::codex::presentation::event(
      6, 1, "agents.activity.upsert",
      {{"activity",
        {{"type", "subAgentActivity"},
         {"status", "started"},
         {"agentThreadId", "rebind-parent"}}}},
      codexui::codex::presentation::Authority::Merge,
      {{"threadId", "new-child"},
       {"turnId", "cycle-turn"},
       {"itemId", "cycle-agent"}}));
  passed &= expect(
      reboundOwnershipModel.childOwnership("rebind-parent") == nullptr &&
          reboundOwnershipModel.thread("new-child") &&
          reboundOwnershipModel.thread("new-child")
              ->childThreadOrder.empty(),
      "ancestor ownership cycles are rejected without disturbing the tree");

  normalizer.bridgeEvent({{"kind", "bridge.provider"},
                          {"state", "disconnected"},
                          {"providerGeneration", std::uint64_t{1}},
                          {"reason", "test provider restart"}});
  passed &= expect(model.thread("thread-1") == nullptr &&
                       model.connection().providerGeneration == 1 &&
                       model.connection().providerState == "disconnected",
                   "provider loss clears provider-scoped presentation state");
  normalizer.bridgeEvent({{"kind", "bridge.provider"},
                          {"state", "ready"},
                          {"providerGeneration", std::uint64_t{2}}});
  passed &= expect(model.connection().providerGeneration == 2 &&
                       model.connection().providerState == "ready",
                   "a new provider generation is accepted for rehydration");
  return passed ? 0 : 1;
}
