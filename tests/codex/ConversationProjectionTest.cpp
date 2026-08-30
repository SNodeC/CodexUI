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

bool testTurnRootSurvivesHistoryPaging() {
  ThreadPresentation thread;
  thread.id = "thread-long-turn";
  addTurn(thread, "turn-long");
  appendItem(
      thread, "turn-long",
      item("root-user",
           {{"type", "userMessage"},
            {"content", {{{"type", "text"}, {"text", "Root prompt"}}}}}));
  for (int index = 0; index < 45; ++index)
    appendItem(thread, "turn-long",
               item("before-steer-" + std::to_string(index),
                    {{"type", "agentMessage"}, {"text", "activity"}}));
  appendItem(thread, "turn-long",
             item("steering-user",
                  {{"type", "userMessage"},
                   {"content", {{{"type", "text"},
                                  {"text", "Later steering prompt"}}}}}));
  for (int index = 0; index < 45; ++index)
    appendItem(thread, "turn-long",
               item("after-steer-" + std::to_string(index),
                    {{"type", "agentMessage"}, {"text", "activity"}}));

  const AuthoritativeItemKey rootKey{thread.id, "turn-long", "root-user"};
  const AuthoritativeItemKey steeringKey{thread.id, "turn-long",
                                          "steering-user"};
  const ConversationSnapshot limited =
      ConversationProjection::project(thread, {}, 80, 100);
  bool result = expect(
      limited.sections.size() == 1 &&
          limited.sections.front().rootCardKey == CardKey{rootKey} &&
          limited.sections.front().cards.front().key == CardKey{rootKey} &&
          limited.find(rootKey) && limited.find(steeringKey) &&
          limited.cardKeys().size() == 81 &&
          limited.hiddenAuthoritativeItemCount == 11 && limited.hasMore,
      "a current long turn pins its real root outside the activity budget");

  thread.turns.at("turn-long").status = "completed";
  const ConversationSnapshot completed =
      ConversationProjection::project(thread, {}, 80, 101);
  result &= expect(
      completed.sections.front().rootCardKey == CardKey{rootKey} &&
          completed.find(rootKey) && completed.find(steeringKey) &&
          completed.hiddenAuthoritativeItemCount == 11,
      "turn completion cannot release the retained activity's root");

  const ConversationSnapshot loaded =
      ConversationProjection::project(thread, {}, 160, 102);
  result &= expect(
      loaded.sections.size() == 1 &&
          loaded.sections.front().rootCardKey == CardKey{rootKey} &&
          std::ranges::count(loaded.cardKeys(), CardKey{rootKey}) == 1 &&
          loaded.find(steeringKey) && !loaded.hasMore &&
          loaded.hiddenAuthoritativeItemCount == 0,
      "loading older activity retains one stable root and ordinary paging semantics");

  const auto indexed = indexAuthoritativeItems(thread.id, &thread);
  result &= expect(
      indexed.turnRootUserMessagePositions.at("turn-long") == 0,
      "a cold-loaded thread indexes its root without local prompt state");

  ThreadPresentation rootOnlyHidden;
  rootOnlyHidden.id = "thread-root-only-hidden";
  addTurn(rootOnlyHidden, "turn");
  appendItem(
      rootOnlyHidden, "turn",
      item("root",
           {{"type", "userMessage"},
            {"content", {{{"type", "text"}, {"text", "Root"}}}}}));
  appendItem(rootOnlyHidden, "turn",
             item("answer", {{"type", "agentMessage"},
                              {"text", "Answer"}}));
  const ConversationSnapshot rootOnlyPinned =
      ConversationProjection::project(rootOnlyHidden, {}, 1, 103);
  result &= expect(
      rootOnlyPinned.cardKeys().size() == 2 &&
          rootOnlyPinned.hiddenAuthoritativeItemCount == 0 &&
          !rootOnlyPinned.hasMore,
      "pinning the only earlier root leaves no hidden activity to load");

  ThreadPresentation conflicting;
  conflicting.id = "thread-unique-root";
  PromptCoordinator prompts;
  const auto localId = prompts.admit(
      conflicting.id, QStringLiteral("Locally admitted start"), {},
      nlohmann::json::object(), nullptr, std::nullopt, 200);
  result &= expect(prompts.beginNext(conflicting.id).has_value() &&
                       prompts.acknowledge(conflicting.id, localId,
                                           std::string("turn"), 201),
                   "a locally admitted turn start reaches acknowledgment");
  addTurn(conflicting, "turn");
  appendItem(
      conflicting, "turn",
      item("authoritative-root",
           {{"type", "userMessage"},
            {"content", {{{"type", "text"},
                           {"text", "Different authoritative prompt"}}}}}));
  prompts.reconcile(conflicting.id, conflicting, 202);
  const ConversationSnapshot uniqueRoot = ConversationProjection::project(
      conflicting, prompts.submissions(conflicting.id), 80, 202);
  const AuthoritativeItemKey authoritativeRoot{conflicting.id, "turn",
                                                "authoritative-root"};
  result &= expect(
      uniqueRoot.sections.size() == 1 &&
          uniqueRoot.sections.front().rootCardKey ==
              CardKey{authoritativeRoot} &&
          uniqueRoot.find(LocalPromptKey{localId}),
      "an unmatched local start cannot compete with an existing authoritative root");
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
  prompts.reconcile(first.id, first, 199);
  result &= expect(prompts.submission(first.id, firstId)->state ==
                           PromptState::InFlight &&
                       !prompts.submission(first.id, firstId)->materializedItem,
                   "events and elapsed time cannot manufacture an ack");

  result &= expect(prompts.acknowledge(first.id, firstId, "turn-2", 200),
                   "the matching completion acknowledges the in-flight prompt");
  prompts.reconcile(first.id, first, 200);
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
  auto compactedItems = indexAuthoritativeItems(first.id, &first);
  prompts.reconcile(first.id, compactedItems, 700);
  accepted = prompts.submission(first.id, firstId);
  const ConversationSnapshot compacted = ConversationProjection::project(
      compactedItems, &first, prompts.submissions(first.id), 80, 701);
  result &= expect(
      !accepted && prompts.submissions(first.id).empty() &&
          compacted.find(LocalPromptKey{firstId}) &&
          compacted.find(LocalPromptKey{firstId})->kind ==
              CardKind::UserMessage &&
          !compacted.find(AuthoritativeItemKey{first.id, "turn-2", "user-new"}),
      "submission cleanup retains the compact local visual identity alias");
  auto retainedAliasItems = indexAuthoritativeItems(first.id, &first);
  prompts.reconcile(first.id, retainedAliasItems, 701);
  const ConversationSnapshot retainedAlias = ConversationProjection::project(
      retainedAliasItems, &first, prompts.submissions(first.id), 80, 702);
  result &=
      expect(retainedAlias.find(LocalPromptKey{firstId}) &&
                 retainedAlias.find(LocalPromptKey{firstId})->kind ==
                     CardKind::UserMessage,
             "a later projection reapplies the retained visual identity alias");
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
  beforeHydration.reconcile(retained.id, retained, 401);
  const ConversationSnapshot atTail = ConversationProjection::project(
      retained, beforeHydration.submissions(retained.id), 80, 401);
  const auto keys = atTail.cardKeys();
  result &=
      expect(keys.size() == 3 && keys.back() == CardKey{LocalPromptKey{tailId}},
             "a pre-hydration prompt stays after retained history");

  PromptCoordinator recovering;
  ThreadPresentation empty;
  empty.id = "thread-recovering";
  const auto recoveringId =
      recovering.admit(empty.id, QStringLiteral("retry after resume"), {},
                       nlohmann::json::object(), &empty, std::nullopt, 500);
  result &= expect(recovering.beginNext(empty.id).has_value() &&
                       recovering.requeue(empty.id, recoveringId),
                   "an empty-thread dispatch can return to hydration");
  ThreadPresentation recovered = baseThread(empty.id);
  const auto awaitingHydration = ConversationProjection::project(
      recovered, recovering.submissions(empty.id), 80, 501);
  result &= expect(
      awaitingHydration.cardKeys().back() ==
          CardKey{LocalPromptKey{recoveringId}},
      "a requeued prompt returns to the unresolved retained-history tail");
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
  prompts.reconcile(thread.id, thread, 501);
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

bool testFirstResponseOrderIsAdmissionStable() {
  ThreadPresentation reasoningFirst;
  reasoningFirst.id = "thread-reasoning-first";
  PromptCoordinator prompts;
  const auto promptId = prompts.admit(
      reasoningFirst.id, QStringLiteral("new prompt"), {},
      nlohmann::json::object(), &reasoningFirst, std::nullopt, 600);
  const auto dispatch = prompts.beginNext(reasoningFirst.id);
  bool result =
      expect(dispatch.has_value(), "an empty-thread prompt begins dispatch");
  if (!dispatch)
    return false;

  addTurn(reasoningFirst, "turn-new");
  appendItem(reasoningFirst, "turn-new",
             item("reasoning", {{"type", "reasoning"},
                                {"summary", nlohmann::json::array()}}));
  prompts.reconcile(reasoningFirst.id, reasoningFirst, 601);
  const AuthoritativeItemKey reasoningKey{reasoningFirst.id, "turn-new",
                                          "reasoning"};
  const auto beforeUser = ConversationProjection::project(
      reasoningFirst, prompts.submissions(reasoningFirst.id), 80, 601);
  result &=
      expect(beforeUser.cardKeys() ==
                 std::vector<CardKey>{LocalPromptKey{promptId}, reasoningKey},
             "reasoning arriving first remains after its admitted prompt");

  appendItem(reasoningFirst, "turn-new",
             item("user-new",
                  {{"type", "userMessage"},
                   {"clientId", dispatch->clientUserMessageId},
                   {"content", {{{"type", "text"}, {"text", "new prompt"}}}}}));
  prompts.reconcile(reasoningFirst.id, reasoningFirst, 602);
  const auto materialized = ConversationProjection::project(
      reasoningFirst, prompts.submissions(reasoningFirst.id), 80, 602);
  result &=
      expect(materialized.cardKeys() ==
                 std::vector<CardKey>{LocalPromptKey{promptId}, reasoningKey},
             "early user-message materialization cannot invert the cards");

  result &= expect(prompts.acknowledge(reasoningFirst.id, promptId,
                                       std::string("turn-new"), 700),
                   "the reasoning-first prompt is acknowledged");
  prompts.reconcile(reasoningFirst.id, reasoningFirst, 700);
  const auto transitioning = ConversationProjection::project(
      reasoningFirst, prompts.submissions(reasoningFirst.id), 80, 700);
  result &=
      expect(transitioning.cardKeys() ==
                 std::vector<CardKey>{LocalPromptKey{promptId}, reasoningKey},
             "the animated-to-blue transition retains prompt order");

  auto compactedItems =
      indexAuthoritativeItems(reasoningFirst.id, &reasoningFirst);
  prompts.reconcile(reasoningFirst.id, compactedItems, 1200);
  const auto compacted = ConversationProjection::project(
      compactedItems, &reasoningFirst, prompts.submissions(reasoningFirst.id),
      80, 1200);
  const VisibleCardData *bluePrompt = compacted.find(LocalPromptKey{promptId});
  result &= expect(
      compacted.cardKeys() ==
              std::vector<CardKey>{LocalPromptKey{promptId}, reasoningKey} &&
          bluePrompt && bluePrompt->kind == CardKind::UserMessage,
      "the compact blue card retains the original admission boundary");

  ThreadPresentation continued = baseThread("thread-continued");
  PromptCoordinator continuedPrompts;
  const auto continuedId = continuedPrompts.admit(
      continued.id, QStringLiteral("continued prompt"), {},
      nlohmann::json::object(), &continued, std::nullopt, 750);
  const auto continuedDispatch = continuedPrompts.beginNext(continued.id);
  result &= expect(continuedDispatch.has_value(),
                   "a continued-thread prompt begins dispatch");
  if (!continuedDispatch)
    return false;
  addTurn(continued, "turn-continued");
  appendItem(
      continued, "turn-continued",
      item("reasoning-continued",
           {{"type", "reasoning"}, {"summary", nlohmann::json::array()}}));
  appendItem(
      continued, "turn-continued",
      item("user-continued",
           {{"type", "userMessage"},
            {"clientId", continuedDispatch->clientUserMessageId},
            {"content", {{{"type", "text"}, {"text", "continued prompt"}}}}}));
  continuedPrompts.reconcile(continued.id, continued, 751);
  result &=
      expect(continuedPrompts.acknowledge(continued.id, continuedId,
                                          std::string("turn-continued"), 800),
             "the continued-thread prompt is acknowledged");
  auto continuedItems = indexAuthoritativeItems(continued.id, &continued);
  continuedPrompts.reconcile(continued.id, continuedItems, 1300);
  const auto continuedCompacted = ConversationProjection::project(
      continuedItems, &continued, continuedPrompts.submissions(continued.id),
      80, 1300);
  const auto continuedKeys = continuedCompacted.cardKeys();
  const auto continuedPrompt =
      std::ranges::find(continuedKeys, CardKey{LocalPromptKey{continuedId}});
  const auto continuedReasoning = std::ranges::find(
      continuedKeys,
      CardKey{AuthoritativeItemKey{continued.id, "turn-continued",
                                   "reasoning-continued"}});
  result &= expect(
      continuedPrompt != continuedKeys.end() &&
          continuedReasoning != continuedKeys.end() &&
          continuedPrompt < continuedReasoning,
      "a continued-thread blue card cannot move below earlier reasoning");

  ThreadPresentation userFirst;
  userFirst.id = "thread-user-first";
  PromptCoordinator ordinaryPrompts;
  const auto ordinaryId = ordinaryPrompts.admit(
      userFirst.id, QStringLiteral("ordinary prompt"), {},
      nlohmann::json::object(), &userFirst, std::nullopt, 800);
  const auto ordinaryDispatch = ordinaryPrompts.beginNext(userFirst.id);
  result &= expect(ordinaryDispatch.has_value(),
                   "the user-first prompt begins dispatch");
  if (!ordinaryDispatch)
    return false;
  addTurn(userFirst, "turn-ordinary");
  appendItem(
      userFirst, "turn-ordinary",
      item("user-ordinary",
           {{"type", "userMessage"},
            {"clientId", ordinaryDispatch->clientUserMessageId},
            {"content", {{{"type", "text"}, {"text", "ordinary prompt"}}}}}));
  appendItem(
      userFirst, "turn-ordinary",
      item("reasoning-ordinary",
           {{"type", "reasoning"}, {"summary", nlohmann::json::array()}}));
  ordinaryPrompts.reconcile(userFirst.id, userFirst, 801);
  const auto userBeforeReasoning = ConversationProjection::project(
      userFirst, ordinaryPrompts.submissions(userFirst.id), 80, 801);
  result &= expect(userBeforeReasoning.cardKeys() ==
                       std::vector<CardKey>{
                           LocalPromptKey{ordinaryId},
                           AuthoritativeItemKey{userFirst.id, "turn-ordinary",
                                                "reasoning-ordinary"}},
                   "the ordinary user-first event order remains unchanged");
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
  prompts.reconcile(thread.id, thread, 1020);
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
  prompts.reconcile(thread.id, thread, 1021);
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
          waiting.sections[1].cards.size() == 2 &&
          waiting.sections[1].rootCardKey == CardKey{LocalPromptKey{firstId}},
      "acknowledged duplicates share one turn while steering stays nested");

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
  result &= expect(trimTrailingEmptyLines(QStringView{
                       QStringLiteral("first\nsecond\n\n \t\r\n")}) ==
                       QStringLiteral("first\nsecond"),
                   "trailing empty terminal lines are removed");
  result &= expect(trimTrailingEmptyLines(
                       QStringView{QStringLiteral("  meaningful spacing  ")}) ==
                       QStringLiteral("  meaningful spacing  "),
                   "spacing on a non-empty final line is retained");
  result &= expect(
      trimTrailingEmptyLines(QStringView{QStringLiteral(" \t\r\n")}).isEmpty(),
      "an entirely empty-line display normalizes to zero lines");
  return result;
}

bool testUserMessageImages() {
  ThreadPresentation thread = baseThread("image-thread");
  appendItem(thread, "turn-1",
             item("user-images",
                  {{"type", "userMessage"},
                   {"content",
                    {{{"type", "text"}, {"text", "image prompt"}},
                     {{"type", "localImage"}, {"path", "/tmp/first.png"}},
                     {{"type", "localImage"}, {"path", "/tmp/second.jpg"}}}}}));
  appendItem(thread, "turn-1",
             item("user-image-only",
                  {{"type", "userMessage"},
                   {"content",
                    {{{"type", "localImage"}, {"path", "/tmp/only.png"}}}}}));

  const ConversationSnapshot authoritative = ConversationProjection::project(
      thread, {}, ConversationProjection::DefaultAuthoritativeItemLimit, 10);
  const auto &cards = authoritative.sections.front().cards;
  const auto *mixed = std::get_if<UserMessageData>(&cards[2].payload);
  const auto *imageOnly = std::get_if<UserMessageData>(&cards[3].payload);
  bool result = expect(
      mixed && mixed->text == QStringLiteral("image prompt") &&
          mixed->imagePaths == QStringList{QStringLiteral("/tmp/first.png"),
                                           QStringLiteral("/tmp/second.jpg")},
      "authoritative user messages retain text and local image paths");
  result &= expect(imageOnly && imageOnly->text.isEmpty() &&
                       imageOnly->imagePaths ==
                           QStringList{QStringLiteral("/tmp/only.png")},
                   "an image-only user message remains presentable");

  PromptSubmission pending;
  pending.id = 41;
  pending.threadId = thread.id;
  pending.prompt = QStringLiteral("pending image");
  pending.state = PromptState::InFlight;
  pending.attachments = {
      {QStringLiteral("/tmp/pending.png"), QStringLiteral("pending.png"),
       QStringLiteral("image/png"), 10},
      {QStringLiteral("/tmp/note.txt"), QStringLiteral("note.txt"),
       QStringLiteral("text/plain"), 10}};
  const std::array submissions{pending};
  const ConversationSnapshot local = ConversationProjection::project(
      thread, submissions,
      ConversationProjection::DefaultAuthoritativeItemLimit, 10);
  const auto *localCard = local.find(LocalPromptKey{41});
  const auto *localPrompt =
      localCard ? std::get_if<LocalPromptData>(&localCard->payload) : nullptr;
  result &=
      expect(localPrompt && localPrompt->imagePaths ==
                                QStringList{QStringLiteral("/tmp/pending.png")},
             "temporary prompts expose only their image attachment paths");

  ThreadPresentation replacement = baseThread("replacement-thread");
  addTurn(replacement, "turn-image");
  PromptCoordinator prompts;
  const auto submissionId = prompts.admit(
      replacement.id, QStringLiteral("replacement image"),
      {{QStringLiteral("/tmp/replacement.png"),
        QStringLiteral("replacement.png"), QStringLiteral("image/png"), 10}},
      nlohmann::json::object(), &replacement, std::nullopt, 100);
  const auto dispatch = prompts.beginNext(replacement.id);
  result &=
      expect(dispatch && prompts.acknowledge(replacement.id, submissionId,
                                             std::string("turn-image"), 200),
             "image prompt receives a real acknowledgement");
  appendItem(
      replacement, "turn-image",
      item("authoritative-image",
           {{"type", "userMessage"},
            {"clientId", dispatch ? dispatch->clientUserMessageId : ""},
            {"content",
             {{{"type", "text"}, {"text", "replacement image"}},
              {{"type", "localImage"}, {"path", "/tmp/replacement.png"}}}}}));
  prompts.reconcile(replacement.id, replacement, 200);
  prompts.reconcile(replacement.id, replacement, 800);
  const ConversationSnapshot replaced = ConversationProjection::project(
      replacement, prompts.submissions(replacement.id), 80, 800);
  const VisibleCardData *replacedCard = replaced.find(AuthoritativeItemKey{
      replacement.id, "turn-image", "authoritative-image"});
  const auto *replacedMessage =
      replacedCard ? std::get_if<UserMessageData>(&replacedCard->payload)
                   : nullptr;
  result &= expect(
      replacedCard && replacedCard->kind == CardKind::UserMessage &&
          replacedMessage &&
          replacedMessage->imagePaths ==
              QStringList{QStringLiteral("/tmp/replacement.png")} &&
          !prompts.submission(replacement.id, submissionId),
      "authoritative image presentation survives local payload compaction");
  return result;
}

bool testGeneratedImageProjection() {
  ThreadPresentation thread = baseThread("generated-image-thread");
  appendItem(thread, "turn-1",
             item("generated-image",
                  {{"type", "imageGeneration"},
                   {"status", "completed"},
                   {"savedPath", "/tmp/generated.png"},
                   {"revisedPrompt", "A restrained CodexUI color proposal"},
                   {"result", std::string(100000, 'A')}}));
  appendItem(thread, "turn-1",
             item("image-view", {{"type", "imageView"},
                                 {"path", "/tmp/review.png"}}));

  const ConversationSnapshot snapshot = ConversationProjection::project(
      thread, {}, ConversationProjection::DefaultAuthoritativeItemLimit, 10);
  const VisibleCardData *card = snapshot.find(
      AuthoritativeItemKey{thread.id, "turn-1", "generated-image"});
  const auto *image =
      card ? std::get_if<ImageGenerationData>(&card->payload) : nullptr;
  const VisibleCardData *viewCard = snapshot.find(
      AuthoritativeItemKey{thread.id, "turn-1", "image-view"});
  const auto *viewImage =
      viewCard ? std::get_if<ImageGenerationData>(&viewCard->payload) : nullptr;
  bool result = expect(
      card && card->kind == CardKind::ImageGeneration && image &&
          image->path == QStringLiteral("/tmp/generated.png") &&
          image->status == QStringLiteral("completed") &&
          image->revisedPrompt ==
              QStringLiteral("A restrained CodexUI color proposal"),
      "generated images project their saved path without exposing base64");
  result &= expect(viewCard && viewCard->kind == CardKind::ImageGeneration &&
                       viewImage &&
                       viewImage->path == QStringLiteral("/tmp/review.png") &&
                       viewImage->status.isEmpty() &&
                       viewImage->revisedPrompt.isEmpty(),
                   "image-view items reuse the local image presentation");
  return result;
}

bool testTruthfulActivityProjection() {
  ThreadPresentation thread = baseThread("activity-thread");
  TurnPresentation &turn = thread.turns.at("turn-1");
  turn.plan = {
      {"explanation", "Keep the conversation chronology compact"},
      {"steps",
       nlohmann::json::array(
           {{{"step", "Inspect protocol data"}, {"status", "completed"}},
            {{"step", "Render the cards"}, {"status", "inProgress"}}})}};
  appendItem(thread, "turn-1",
             item("text-plan", {{"type", "plan"},
                                {"text", "A textual plan-mode response"}}));
  appendItem(thread, "turn-1",
             item("empty-reasoning", {{"type", "reasoning"},
                                      {"summary", nlohmann::json::array()}}));
  appendItem(thread, "turn-1",
             item("command", {{"type", "commandExecution"},
                              {"command", "true"},
                              {"status", "completed"},
                              {"durationMs", 2400}}));
  appendItem(
      thread, "turn-1",
      item("files",
           {{"type", "fileChange"},
            {"status", "completed"},
            {"changes",
             nlohmann::json::array(
                 {{{"path", "src/card.cpp"},
                   {"kind", "update"},
                   {"diff", "--- a/src/card.cpp\n+++ b/src/card.cpp\n-old\n"
                            "+new\n++++literal\n+extra\n"}},
                  {{"path", "tests/card.cpp"},
                   {"kind", "add"},
                   {"diff",
                    "--- /dev/null\n+++ b/tests/card.cpp\n+test\n"}}})}}));
  appendItem(thread, "turn-1",
             item("agent", {{"type", "subAgentActivity"},
                            {"status", "inProgress"},
                            {"agentThreadId", "child-thread"},
                            {"model", "gpt-current"},
                            {"reasoningEffort", "medium"},
                            {"senderThreadId", "activity-thread"}}));

  const ConversationSnapshot snapshot = ConversationProjection::project(
      thread, {}, ConversationProjection::DefaultAuthoritativeItemLimit, 10);
  const VisibleCardData *structured =
      snapshot.find(TurnPlanKey{thread.id, "turn-1"});
  const VisibleCardData *textual =
      snapshot.find(AuthoritativeItemKey{thread.id, "turn-1", "text-plan"});
  const VisibleCardData *reasoning = snapshot.find(
      AuthoritativeItemKey{thread.id, "turn-1", "empty-reasoning"});
  const VisibleCardData *command =
      snapshot.find(AuthoritativeItemKey{thread.id, "turn-1", "command"});
  const VisibleCardData *files =
      snapshot.find(AuthoritativeItemKey{thread.id, "turn-1", "files"});
  const VisibleCardData *agent =
      snapshot.find(AuthoritativeItemKey{thread.id, "turn-1", "agent"});
  const auto *textPlan =
      textual ? std::get_if<PlanData>(&textual->payload) : nullptr;
  const auto *execution =
      command ? std::get_if<CommandExecutionData>(&command->payload) : nullptr;
  const auto *fileData =
      files ? std::get_if<FileChangesData>(&files->payload) : nullptr;
  const auto *agentData =
      agent ? std::get_if<AgentActivityData>(&agent->payload) : nullptr;

  bool result = expect(
      !structured,
      "structured plan state remains Inspector-only in production projection");
  result &=
      expect(textPlan &&
                 textPlan->legacyText ==
                     QStringLiteral("A textual plan-mode response"),
             "textual plan items remain supported conversation content");
  const auto *reasoningData =
      reasoning ? std::get_if<ReasoningData>(&reasoning->payload) : nullptr;
  result &= expect(reasoningData && reasoningData->summary.isEmpty(),
                   "reasoning remains a stable progress card without a public summary");
  result &= expect(execution && execution->durationMilliseconds == 2400,
                   "command duration is retained when supplied");
  result &= expect(
      fileData && fileData->changes.size() == 2 &&
          fileData->changes[0].additions == 3 &&
          fileData->changes[0].deletions == 1 &&
          fileData->changes[1].additions == 1 &&
          fileData->changes[1].deletions == 0,
      "file-change rows and unified-diff counts are projected truthfully");
  result &= expect(
      agentData && agentData->childThreadId == QStringLiteral("child-thread") &&
          agentData->model == QStringLiteral("gpt-current") &&
          agentData->reasoningEffort == QStringLiteral("medium") &&
          agentData->senderThreadId == QStringLiteral("activity-thread"),
      "available agent identity and execution settings are retained");
  return result;
}

bool testFileLinksArePartOfTheCanonicalPrompt() {
  const std::vector<AttachmentDraft> attachments{
      {QStringLiteral("/tmp/review notes [final] (2).pdf"),
       QStringLiteral("review notes [final] (2).pdf"),
       QStringLiteral("application/pdf"), 10},
      {QStringLiteral("/tmp/image.png"), QStringLiteral("image.png"),
       QStringLiteral("image/png"), 10},
      {QStringLiteral("/tmp/audio.ogg"), QStringLiteral("audio.ogg"),
       QStringLiteral("audio/ogg"), 10}};
  const QString composed =
      promptWithFileLinks(QStringLiteral("Review this"), attachments);
  const QString expected = QStringLiteral(
      "Review this\n\nAttached files:\n"
      "- [review notes \\[final\\] (2).pdf]"
      "(file:///tmp/review%20notes%20%5Bfinal%5D%20%282%29.pdf)");
  bool result = expect(composed == expected,
                       "ordinary files become escaped durable Markdown links");

  PromptCoordinator prompts;
  const auto id =
      prompts.admit("thread-files", composed, attachments,
                    nlohmann::json::object(), nullptr, std::nullopt, 100);
  const auto dispatch = prompts.beginNext("thread-files");
  result &=
      expect(dispatch && dispatch->id == id && dispatch->prompt == composed,
             "temporary presentation and transport share one prompt");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main() {
  using namespace codexui::codex::middle;
  bool result = testCanonicalGroupingAndProjection();
  result &= testTurnRootSurvivesHistoryPaging();
  result &= testQueueIsolationAndRealAcknowledgement();
  result &= testDispatchChoiceAndPreHydrationTail();
  result &= testClientIdentityBindsBeforeAcknowledgement();
  result &= testFirstResponseOrderIsAdmissionStable();
  result &= testAnchoredDuplicatePrompts();
  result &= testCommandOutputVisibility();
  result &= testUserMessageImages();
  result &= testGeneratedImageProjection();
  result &= testTruthfulActivityProjection();
  result &= testFileLinksArePartOfTheCanonicalPrompt();
  if (result)
    std::cout << "Conversation projection tests passed\n";
  return result ? 0 : 1;
}
