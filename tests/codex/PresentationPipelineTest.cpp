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
                       thread->cwd == "/workspace",
                   "list, full read, and live events retain one stable thread");
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
