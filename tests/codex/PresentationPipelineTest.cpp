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
  normalizer.serverNotification("turn/completed",
                                {{"threadId", "thread-1"},
                                 {"turn",
                                  {{"id", "turn-2"},
                                   {"status", "completed"},
                                   {"items", nlohmann::json::array()}}}});

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
  passed &= expect(model.connection().connected &&
                       model.connection().connectionId == "frontend-test" &&
                       model.connection().role == "controller",
                   "connection and controller events form coherent state");
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
  passed &= expect(!model.activeTurnId("thread-1").has_value(),
                   "completed stream leaves no active turn");
  return passed ? 0 : 1;
}
