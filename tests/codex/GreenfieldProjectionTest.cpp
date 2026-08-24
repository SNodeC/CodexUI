// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationProjection.h"
#include "codex/middle/PromptCoordinator.h"

#include <QString>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

namespace codexui::codex::middle {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

ItemPresentation item(std::string id, nlohmann::json raw) {
  return ItemPresentation{std::move(id), std::move(raw), {}};
}

void appendItem(ThreadPresentation &thread, const std::string &turnId,
                ItemPresentation presentation) {
  TurnPresentation &turn = thread.turns.at(turnId);
  turn.itemOrder.push_back(presentation.id);
  turn.items.emplace(presentation.id, std::move(presentation));
}

ThreadPresentation baseThread(std::string id) {
  ThreadPresentation thread;
  thread.id = std::move(id);
  thread.turnOrder = {"turn-1"};
  TurnPresentation turn;
  turn.id = "turn-1";
  turn.status = "completed";
  thread.turns.emplace(turn.id, std::move(turn));
  appendItem(thread, "turn-1",
             item("user-old",
                  {{"type", "userMessage"},
                   {"content", {{{"type", "text"}, {"text", "old prompt"}}}}}));
  appendItem(thread, "turn-1",
             item("answer-old", {{"type", "agentMessage"},
                                 {"phase", "final_answer"},
                                 {"text", "old answer"}}));
  return thread;
}

void addTurn(ThreadPresentation &thread, const std::string &turnId,
             std::string status = "inProgress") {
  thread.turnOrder.push_back(turnId);
  TurnPresentation turn;
  turn.id = turnId;
  turn.status = std::move(status);
  thread.turns.emplace(turn.id, std::move(turn));
}

const VisibleCardData *cardForSubmission(const ConversationSnapshot &snapshot,
                                         std::uint64_t id) {
  return snapshot.find(LocalPromptKey{id});
}

bool testCanonicalGroupingAndProjection() {
  ThreadPresentation thread = baseThread("thread-a");
  addTurn(thread, "turn-2");
  appendItem(thread, "turn-2",
             item("command", {{"type", "commandExecution"},
                              {"command", "true"},
                              {"status", "completed"},
                              {"aggregatedOutput", " \n\t\x1b[0m"}}));

  const ConversationSnapshot snapshot = ConversationProjection::project(
      thread, {}, ConversationProjection::DefaultAuthoritativeItemLimit, 10);
  bool result = expect(snapshot.sections.size() == 2,
                       "one transparent section is projected per turn");
  result &= expect(snapshot.sections[0].turnId == "turn-1" &&
                       snapshot.sections[0].cards.size() == 2 &&
                       snapshot.sections[1].turnId == "turn-2" &&
                       snapshot.sections[1].cards.size() == 1,
                   "thread, turn, and item order are retained");
  const auto *command =
      std::get_if<CommandExecutionData>(&snapshot.sections[1].cards[0].payload);
  result &= expect(command && command->output.isEmpty(),
                   "non-presentable command output is projected as absent");
  result &= expect(std::holds_alternative<AuthoritativeItemKey>(
                       snapshot.sections[0].cards[0].key) &&
                       stableKey(snapshot.sections[0].cards[0].key) !=
                           stableKey(snapshot.sections[0].cards[1].key),
                   "authoritative cards have typed collision-free stable keys");

  const ConversationSnapshot limited =
      ConversationProjection::project(thread, {}, 1, 10);
  result &=
      expect(limited.hasMore && limited.hiddenAuthoritativeItemCount == 2 &&
                 limited.cardKeys().size() == 1,
             "history limit is based only on authoritative items");

  ThreadPresentation emptyPlan = baseThread("thread-empty-plan");
  appendItem(emptyPlan, "turn-1",
             item("empty-plan", {{"type", "plan"}, {"text", ""}}));
  const ConversationSnapshot emptyPlanSnapshot =
      ConversationProjection::project(emptyPlan, {}, 80, 10);
  const VisibleCardData &emptyPlanCard =
      emptyPlanSnapshot.sections.front().cards.back();
  const auto *generic =
      std::get_if<GenericActivityData>(&emptyPlanCard.payload);
  result &= expect(emptyPlanCard.kind == CardKind::GenericActivity && generic &&
                       generic->type == QStringLiteral("plan"),
                   "an empty plan retains the generic raw-data fallback");
  return result;
}

bool testQueueIsolationAndRealAcknowledgement() {
  ThreadPresentation first = baseThread("thread-a");
  ThreadPresentation second = baseThread("thread-b");
  PromptCoordinator prompts;
  const auto firstId =
      prompts.admit(first.id, QStringLiteral("same"), {},
                    nlohmann::json::object(), &first, std::nullopt, 100);
  const auto secondId =
      prompts.admit(second.id, QStringLiteral("other"), {},
                    nlohmann::json::object(), &second, std::nullopt, 101);

  const auto firstDispatch = prompts.beginNext(first.id);
  bool result = expect(firstDispatch && firstDispatch->id == firstId,
                       "the first queued prompt begins dispatch");
  result &= expect(!prompts.beginNext(first.id),
                   "a thread has at most one in-flight prompt");
  const auto secondDispatch = prompts.beginNext(second.id);
  result &= expect(secondDispatch && secondDispatch->id == secondId,
                   "different threads have independent in-flight queues");

  addTurn(first, "turn-2");
  appendItem(
      first, "turn-2",
      item("user-new", {{"type", "userMessage"},
                        {"content", {{{"type", "text"}, {"text", "same"}}}}}));
  prompts.reconcile(first.id, first);
  result &= expect(prompts.submission(first.id, firstId)->state ==
                           PromptState::InFlight &&
                       !prompts.submission(first.id, firstId)->materializedItem,
                   "events and elapsed time cannot manufacture an ack");

  result &= expect(prompts.acknowledge(first.id, firstId, "turn-2", 200),
                   "the matching completion acknowledges the in-flight prompt");
  prompts.reconcile(first.id, first);
  const PromptSubmission *accepted = prompts.submission(first.id, firstId);
  result &= expect(accepted && accepted->state == PromptState::Accepted &&
                       accepted->materializedItem &&
                       accepted->materializedItem->itemId == "user-new",
                   "an acknowledged prompt binds to its authoritative item");

  const ConversationSnapshot transitioning = ConversationProjection::project(
      first, prompts.submissions(first.id), 80, 699);
  const VisibleCardData *local = cardForSubmission(transitioning, firstId);
  result &= expect(local && local->kind == CardKind::LocalPrompt,
                   "the accepted presentation transition lasts 500ms");
  const ConversationSnapshot materialized = ConversationProjection::project(
      first, prompts.submissions(first.id), 80, 700);
  const VisibleCardData *authoritative =
      cardForSubmission(materialized, firstId);
  result &=
      expect(authoritative && authoritative->kind == CardKind::UserMessage &&
                 authoritative->itemId == "user-new",
             "the authoritative user item assumes the local stable key");
  result &= expect(stableKey(local->key) == stableKey(authoritative->key),
                   "materialization does not change the visual identity");
  prompts.compactResolved("thread-a", 700);
  accepted = prompts.submission(first.id, firstId);
  const ConversationSnapshot compacted = ConversationProjection::project(
      first, prompts.submissions(first.id), 80, 701);
  result &= expect(
      accepted && accepted->prompt.isEmpty() && accepted->attachments.empty() &&
          accepted->clientUserMessageId.empty() &&
          accepted->turnOptions.empty() &&
          compacted.find(LocalPromptKey{firstId}) &&
          compacted.find(LocalPromptKey{firstId})->kind ==
              CardKind::UserMessage,
      "resolved aliases release dispatch payload and retain identity");
  return result;
}

bool testDispatchChoiceAndPreHydrationTail() {
  ThreadPresentation thread = baseThread("thread-dispatch");
  PromptCoordinator prompts;
  const auto id = prompts.admit(
      thread.id, QStringLiteral("queued while active"), {},
      nlohmann::json::object(), &thread, std::string("turn-1"), 300);
  const auto dispatch = prompts.beginNext(thread.id, std::nullopt);
  bool result =
      expect(dispatch && dispatch->id == id && !dispatch->expectedTurnId,
             "dispatch-time state replaces a stale admission turn");

  PromptCoordinator beforeHydration;
  const auto tailId = beforeHydration.admit(
      "thread-tail", QStringLiteral("after retained history"), {},
      nlohmann::json::object(), nullptr, std::nullopt, 400);
  ThreadPresentation retained = baseThread("thread-tail");
  beforeHydration.reconcile(retained.id, retained);
  const ConversationSnapshot atTail = ConversationProjection::project(
      retained, beforeHydration.submissions(retained.id), 80, 401);
  const auto keys = atTail.cardKeys();
  result &=
      expect(keys.size() == 3 && keys.back() == CardKey{LocalPromptKey{tailId}},
             "a pre-hydration prompt stays after retained history");
  return result;
}

bool testClientIdentityBindsBeforeAcknowledgement() {
  ThreadPresentation thread = baseThread("thread-client-id");
  PromptCoordinator prompts;
  const auto id =
      prompts.admit(thread.id, QStringLiteral("identity matched"), {},
                    nlohmann::json::object(), &thread, std::nullopt, 500);
  const auto dispatch = prompts.beginNext(thread.id);
  bool result = expect(dispatch && !dispatch->clientUserMessageId.empty(),
                       "every dispatch carries a stable client message id");
  if (!dispatch)
    return false;

  addTurn(thread, "turn-client");
  appendItem(
      thread, "turn-client",
      item("user-client",
           {{"type", "userMessage"},
            {"clientId", dispatch->clientUserMessageId},
            {"content", {{{"type", "text"}, {"text", "identity matched"}}}}}));
  prompts.reconcile(thread.id, thread);
  const PromptSubmission *pending = prompts.submission(thread.id, id);
  result &= expect(pending && pending->state == PromptState::InFlight &&
                       pending->materializedItem &&
                       pending->materializedItem->itemId == "user-client",
                   "client identity binds without manufacturing an ack");
  const ConversationSnapshot snapshot = ConversationProjection::project(
      thread, prompts.submissions(thread.id), 80, 501);
  result &= expect(
      snapshot.cardKeys().size() == 3 && snapshot.find(LocalPromptKey{id}) &&
          snapshot.find(LocalPromptKey{id})->kind == CardKind::LocalPrompt,
      "early materialization keeps one awaiting visual card");
  result &= expect(prompts.fail(thread.id, id, QStringLiteral("rejected")),
                   "the exact terminal callback can fail a bound prompt");
  const ConversationSnapshot failed = ConversationProjection::project(
      thread, prompts.submissions(thread.id), 80, 502);
  const VisibleCardData *failedCard = failed.find(LocalPromptKey{id});
  const auto *failedPrompt =
      failedCard ? std::get_if<LocalPromptData>(&failedCard->payload) : nullptr;
  result &= expect(failed.cardKeys().size() == 3 && failedPrompt &&
                       failedPrompt->state == PromptState::Failed &&
                       failedPrompt->error == QStringLiteral("rejected"),
                   "a failure remains explicit after early materialization");
  return result;
}

bool testAnchoredDuplicatePrompts() {
  ThreadPresentation thread = baseThread("thread-duplicates");
  PromptCoordinator prompts;
  const auto firstId =
      prompts.admit(thread.id, QStringLiteral("repeat"), {},
                    nlohmann::json::object(), &thread, std::nullopt, 1000);
  const auto secondId =
      prompts.admit(thread.id, QStringLiteral("repeat"), {},
                    nlohmann::json::object(), &thread, std::nullopt, 1001);

  bool result = expect(prompts.beginNext(thread.id).has_value(),
                       "first duplicate dispatches");
  result &= expect(prompts.acknowledge(thread.id, firstId, "turn-2", 1010),
                   "first duplicate is acknowledged by id");
  result &= expect(prompts.beginNext(thread.id, "turn-2").has_value(),
                   "second duplicate dispatches only after first ack");
  result &= expect(prompts.acknowledge(thread.id, secondId, "turn-2", 1020),
                   "second duplicate is acknowledged by id");

  addTurn(thread, "turn-2");
  appendItem(thread, "turn-2",
             item("repeat-1",
                  {{"type", "userMessage"},
                   {"content", {{{"type", "text"}, {"text", "repeat"}}}}}));
  prompts.reconcile(thread.id, thread);
  const ConversationSnapshot partiallyMaterialized =
      ConversationProjection::project(thread, prompts.submissions(thread.id),
                                      80, 1600);
  const auto partialKeys = partiallyMaterialized.cardKeys();
  const auto materializedFirst =
      std::ranges::find(partialKeys, CardKey{LocalPromptKey{firstId}});
  const auto waitingSecond =
      std::ranges::find(partialKeys, CardKey{LocalPromptKey{secondId}});
  result &= expect(materializedFirst != partialKeys.end() &&
                       waitingSecond != partialKeys.end() &&
                       materializedFirst < waitingSecond,
                   "partial materialization cannot invert prompt order");

  appendItem(thread, "turn-2",
             item("repeat-2",
                  {{"type", "userMessage"},
                   {"content", {{{"type", "text"}, {"text", "repeat"}}}}}));
  prompts.reconcile(thread.id, thread);
  const PromptSubmission *first = prompts.submission(thread.id, firstId);
  const PromptSubmission *second = prompts.submission(thread.id, secondId);
  result &= expect(
      first && second && first->materializedItem && second->materializedItem &&
          first->materializedItem->itemId == "repeat-1" &&
          second->materializedItem->itemId == "repeat-2",
      "identical prompts bind in admission order without collision");

  const ConversationSnapshot waiting = ConversationProjection::project(
      thread, prompts.submissions(thread.id), 80, 1021);
  const auto keys = waiting.cardKeys();
  const auto firstPosition =
      std::ranges::find(keys, CardKey{LocalPromptKey{firstId}});
  const auto secondPosition =
      std::ranges::find(keys, CardKey{LocalPromptKey{secondId}});
  result &=
      expect(firstPosition != keys.end() && secondPosition != keys.end() &&
                 firstPosition < secondPosition,
             "same-anchor local prompts retain admission order");
  result &= expect(
      waiting.sections.size() == 2 && waiting.sections[1].turnId == "turn-2" &&
          waiting.sections[1].cards.size() == 2,
      "acknowledged duplicates share one authoritative turn section");

  PromptCoordinator moved;
  const auto draftId =
      moved.admit("", QStringLiteral("draft"), {}, nlohmann::json::object(),
                  nullptr, std::nullopt, 1);
  result &= expect(moved.reassignThread("", "assigned") &&
                       moved.submission("assigned", draftId) &&
                       stableKey(LocalPromptKey{draftId}) ==
                           stableKey(CardKey{LocalPromptKey{draftId}}),
                   "new-thread assignment preserves the local prompt key");
  return result;
}

bool testCommandOutputVisibility() {
  bool result = expect(!terminalOutputHasVisibleText(QStringView{}),
                       "empty output is not visible");
  result &= expect(
      !terminalOutputHasVisibleText(QStringView{QStringLiteral(" \n\t")}),
      "whitespace output is not visible");
  result &= expect(!terminalOutputHasVisibleText(
                       QStringView{QStringLiteral("\x1b[0m\x1b]0;title\x07")}),
                   "ANSI and control output is not visible");
  result &= expect(
      terminalOutputHasVisibleText(QStringView{QStringLiteral("done\n")}),
      "printable command output is visible");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main() {
  using namespace codexui::codex::middle;
  bool result = testCanonicalGroupingAndProjection();
  result &= testQueueIsolationAndRealAcknowledgement();
  result &= testDispatchChoiceAndPreHydrationTail();
  result &= testClientIdentityBindsBeforeAcknowledgement();
  result &= testAnchoredDuplicatePrompts();
  result &= testCommandOutputVisibility();
  if (result)
    std::cout << "Greenfield projection tests passed\n";
  return result ? 0 : 1;
}
