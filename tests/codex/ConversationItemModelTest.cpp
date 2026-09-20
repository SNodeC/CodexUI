// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationItemModel.h"
#include "codex/middle/ConversationHeightIndex.h"
#include "codex/nodegraph/NodeGraph.h"

#include <QCoreApplication>
#include <QPersistentModelIndex>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace codexui::codex::middle {
namespace {

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

struct SignalLog {
  struct Range {
    int first = -1;
    int last = -1;

    bool operator==(const Range &) const = default;
  };
  struct Move {
    int first = -1;
    int last = -1;
    int destination = -1;

    bool operator==(const Move &) const = default;
  };
  std::vector<Range> inserted;
  std::vector<Range> removed;
  std::vector<Move> moved;
  std::vector<Range> changed;
  std::vector<QList<int>> changedRoles;
  int resets = 0;

  explicit SignalLog(ConversationItemModel &model) {
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                     [this](const QModelIndex &, int first, int last) {
                       inserted.push_back({first, last});
                     });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                     [this](const QModelIndex &, int first, int last) {
                       removed.push_back({first, last});
                     });
    QObject::connect(&model, &QAbstractItemModel::rowsMoved, &model,
                     [this](const QModelIndex &, int first, int last,
                            const QModelIndex &, int destination) {
                       moved.push_back({first, last, destination});
                     });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [this](const QModelIndex &first, const QModelIndex &last,
                            const QList<int> &roles) {
                       changed.push_back({first.row(), last.row()});
                       changedRoles.push_back(roles);
                     });
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model,
                     [this] { ++resets; });
  }

  void clear() {
    inserted.clear();
    removed.clear();
    moved.clear();
    changed.clear();
    changedRoles.clear();
    resets = 0;
  }
};

VisibleCardData card(std::string key, nodegraph::NodeRef target,
                     std::string text = {}) {
  VisibleCardData result;
  result.key = AuthoritativeItemKey{"thread", "turn", key};
  result.kind = CardKind::AgentMessage;
  result.threadId = "thread";
  result.turnId = "turn";
  result.itemId = key;
  result.payload = AgentMessageData{std::move(text), true};
  result.target = std::move(target);
  return result;
}

ConversationSnapshot snapshot(std::vector<VisibleCardData> cards,
                              std::string threadId = "thread") {
  ConversationSnapshot result;
  result.threadId = std::move(threadId);
  if (!cards.empty()) {
    TurnSection section;
    section.key = "section";
    section.turnId = "turn";
    section.rootCardKey = cards.front().key;
    section.cards = std::move(cards);
    result.sections.push_back(std::move(section));
  }
  return result;
}

ConversationRowPlacement placement(VisibleCardData value,
                                   bool turnRoot = false) {
  ConversationRowPlacement result;
  result.card = std::move(value);
  result.sectionKey = "section";
  result.turnRoot = turnRoot;
  result.nested = !turnRoot;
  return result;
}

bool applyPlacement(ConversationItemModel &model,
                    ConversationRowPlacement value,
                    std::optional<CardKey> previous = {},
                    std::optional<CardKey> next = {},
                    std::vector<nodegraph::NodeRef> removals = {}) {
  std::vector<ConversationRowChange> rows{
      {std::move(value), std::move(previous), std::move(next)}};
  return model.applyStructuralDelta(rows, removals).has_value();
}

bool applyRemovals(ConversationItemModel &model,
                   std::vector<nodegraph::NodeRef> removals) {
  std::vector<ConversationRowChange> rows;
  return model.applyStructuralDelta(rows, removals).has_value();
}

bool changed(ConversationItemModel::StructuralChangeResult result) {
  return result == ConversationItemModel::StructuralChangeResult::Changed;
}

bool testStableIdentityAndExactSignals() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef first;
  nodegraph::NodeRef second;
  nodegraph::NodeRef third;
  {
    auto write = graph.write();
    first = write.upsert({nodegraph::NodeKind::Item, "turn-a/item"});
    second = write.upsert({nodegraph::NodeKind::Item, "turn-b/item"});
    third = write.upsert({nodegraph::NodeKind::Item, "turn-c/item"});
    static_cast<void>(write.finish());
  }

  ConversationItemModel model;
  SignalLog log(model);
  bool result = require(changed(model.replaceConversation(
                            snapshot({card("same-wire-id-a", first, "one"),
                                      card("same-wire-id-b", second, "two")}))),
                        "initial authority was not accepted");
  result &=
      require(log.resets == 1 && log.inserted.empty() && model.rowCount() == 2,
              "initial thread did not use one model reset");
  result &= require(model.indexForTarget(first).row() == 0 &&
                        model.indexForTarget(second).row() == 1 &&
                        !model.indexForTarget(third).isValid(),
                    "NodeRef targeting did not resolve the exact graph node");
  const qulonglong indexRebuilds =
      model.property("modelIndexRebuildCount").toULongLong();

  log.clear();
  result &= require(
      model.replaceConversation(
          snapshot({card("same-wire-id-a", first, "one"),
                    card("same-wire-id-b", second, "two")})) ==
              ConversationItemModel::StructuralChangeResult::Unchanged &&
          log.resets == 0 && log.inserted.empty() && log.removed.empty() &&
          log.moved.empty() && log.changed.empty(),
      "identical model state emitted presentation work");
  result &= require(model.property("modelIndexRebuildCount").toULongLong() ==
                        indexRebuilds,
                    "identical model state rebuilt stable indexes");

  log.clear();
  result &= require(
      model.reconcile(snapshot({card("same-wire-id-a", first, "one"),
                                card("same-wire-id-a", first, "duplicate")})) ==
              ConversationItemModel::StructuralChangeResult::Rejected &&
          model.rowCount() == 2 && model.indexForTarget(first).row() == 0 &&
          model.indexForTarget(second).row() == 1 && log.resets == 0 &&
          log.inserted.empty() && log.removed.empty() && log.moved.empty() &&
          log.changed.empty(),
      "a rejected duplicate authority left model identity and signals intact");

  log.clear();
  result &= require(
      model.reconcile(snapshot({card("same-wire-id-a", first, "one"),
                                card("same-wire-id-b", first, "duplicate")})) ==
              ConversationItemModel::StructuralChangeResult::Rejected &&
          model.rowCount() == 2 && model.indexForTarget(first).row() == 0 &&
          model.indexForTarget(second).row() == 1 && log.resets == 0 &&
          log.inserted.empty() && log.removed.empty() && log.moved.empty() &&
          log.changed.empty(),
      "duplicate target identity was not rejected atomically");

  log.clear();
  result &= require(
      model.reconcile(snapshot({card("same-wire-id-a", second, "one"),
                                card("same-wire-id-b", first, "two")})) ==
              ConversationItemModel::StructuralChangeResult::Changed &&
          model.indexForTarget(first).row() == 1 &&
          model.indexForTarget(second).row() == 0 &&
          model.indexForStableKey("item:6:thread4:turn14:same-wire-id-a")
                  .row() == 0 &&
          model.indexForStableKey("item:6:thread4:turn14:same-wire-id-b")
                  .row() == 1 &&
          log.resets == 0 && log.inserted.empty() && log.removed.empty() &&
          log.moved.empty() && log.changed.size() == 2 &&
          log.changedRoles[0].contains(
              ConversationItemModel::TargetIdentityRole) &&
          log.changedRoles[1].contains(
              ConversationItemModel::TargetIdentityRole),
      "a target swap did not preserve both identity indexes");

  log.clear();
  result &= require(
      model.reconcile(snapshot({card("same-wire-id-a", first, "one"),
                                card("same-wire-id-b", second, "two")})) ==
              ConversationItemModel::StructuralChangeResult::Changed &&
          model.indexForTarget(first).row() == 0 &&
          model.indexForTarget(second).row() == 1,
      "restoring swapped targets lost an identity index");

  log.clear();
  result &=
      require(model.updateCard(card("same-wire-id-b", second, "streamed")) ==
                      ConversationItemModel::CardUpdateResult::Changed &&
                  log.changed.size() == 1 && log.changed.front().first == 1 &&
                  log.changed.front().last == 1 &&
                  log.changedRoles.front().contains(
                      ConversationItemModel::PresentationRole) &&
                  log.changedRoles.front().contains(Qt::AccessibleTextRole),
              "one streamed card did not emit one exact dataChanged range");
  result &= require(model.property("modelIndexRebuildCount").toULongLong() ==
                        indexRebuilds,
                    "one streamed card traversed and rebuilt stable indexes");
  log.clear();
  result &=
      require(model.updateCard(card("same-wire-id-b", second, "streamed")) ==
                      ConversationItemModel::CardUpdateResult::Unchanged &&
                  log.changed.empty(),
              "repeated streamed card emitted presentation work");

  log.clear();
  result &= require(
      model.updateCard(card("same-wire-id-b", third, "retargeted")) ==
              ConversationItemModel::CardUpdateResult::Incompatible &&
          model.indexForTarget(second).row() == 1 &&
          !model.indexForTarget(third).isValid() && log.changed.empty(),
      "a presentation-only update changed authoritative target identity");

  log.clear();
  result &=
      require(applyPlacement(model, placement(card("inserted", third, "three")),
                             model.card(0)->key, model.card(1)->key) &&
                  log.inserted.size() == 1 && log.inserted.front().first == 1 &&
                  log.inserted.front().last == 1 && log.resets == 0,
              "middle insertion did not use beginInsertRows/endInsertRows");

  log.clear();
  result &=
      require(applyPlacement(
                  model, placement(card("same-wire-id-b", second, "streamed")),
                  model.card(0)->key, model.card(1)->key) &&
                  log.moved.size() == 1 && log.moved.front().first == 2 &&
                  log.moved.front().last == 2 &&
                  log.moved.front().destination == 1 && log.resets == 0,
              "actual reordering did not use beginMoveRows/endMoveRows");

  log.clear();
  result &= require(
      !applyPlacement(
          model,
          placement(card("duplicate-tail-target", first, "duplicate target")),
          model.card(model.rowCount() - 1)->key) &&
          model.rowCount() == 3 && model.indexForTarget(first).row() == 0 &&
          log.inserted.empty(),
      "a tail append stole an already represented target identity");

  log.clear();
  result &= require(applyRemovals(model, {third}) && log.removed.size() == 1 &&
                        log.removed.front().first == 2 &&
                        log.removed.front().last == 2 && log.resets == 0,
                    "removal did not use beginRemoveRows/endRemoveRows");
  result &= require(
      model.indexForTarget(second).row() == 1 &&
          model.indexForStableKey("item:6:thread4:turn14:same-wire-id-b")
                  .row() == 1 &&
          model.property("modelIndexRebuildCount").toULongLong() ==
              indexRebuilds,
      "exact structure changes rebuilt or lost stable identity indexes");

  log.clear();
  result &=
      require(changed(model.replaceConversation(snapshot({}, "replacement"))) &&
                  log.resets == 1 && model.rowCount() == 0,
              "genuine thread replacement did not use a model reset");
  return result;
}

bool testAllLoadedReconciliationInsertsOnlyMissingRanges() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef root;
  nodegraph::NodeRef middle;
  nodegraph::NodeRef tail;
  {
    auto write = graph.write();
    root = write.upsert({nodegraph::NodeKind::Item, "page/root"});
    middle = write.upsert({nodegraph::NodeKind::Item, "page/middle"});
    tail = write.upsert({nodegraph::NodeKind::Item, "page/tail"});
    static_cast<void>(write.finish());
  }

  ConversationSnapshot initial =
      snapshot({card("root", root, "root"), card("tail", tail, "tail")});
  initial.hasMore = true;
  ConversationItemModel model;
  bool result = require(changed(model.replaceConversation(std::move(initial))),
                        "history page fixture was not accepted");
  SignalLog log(model);

  ConversationSnapshot expanded =
      snapshot({card("root", root, "root"), card("middle", middle, "middle"),
                card("tail", tail, "tail")});
  expanded.hasMore = true;
  result &= require(
      changed(model.reconcile(std::move(expanded))) && log.resets == 0 &&
          log.inserted.size() == 1 && log.inserted.front().first == 1 &&
          log.inserted.front().last == 1 && model.rowCount() == 3 &&
          model.indexForTarget(root).row() == 0 &&
          model.indexForTarget(middle).row() == 1 &&
          model.indexForTarget(tail).row() == 2,
      "history expansion did not insert only the missing range after its "
      "pinned root");

  log.clear();
  ConversationSnapshot reordered =
      snapshot({card("root", root, "root"), card("tail", tail, "tail"),
                card("middle", middle, "middle")});
  reordered.hasMore = true;
  result &= require(
      model.reconcile(std::move(reordered)) ==
              ConversationItemModel::StructuralChangeResult::Changed &&
          log.inserted.empty() && log.moved.size() == 1 &&
          log.moved.front().first == 2 && log.moved.front().destination == 1 &&
          log.removed.empty() && log.changed.size() == 2 &&
          log.changed[0].first == 1 && log.changed[1].first == 2 &&
          log.changedRoles[0] ==
              QList<int>{ConversationItemModel::LastInTurnRole} &&
          log.changedRoles[1] ==
              QList<int>{ConversationItemModel::LastInTurnRole} &&
          log.resets == 0,
      "history and ordinary authority did not share the exact "
      "ordered reconciliation path");
  return result;
}

bool testPromptRetargetIsAtomic() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef promptTarget;
  nodegraph::NodeRef messageTarget;
  nodegraph::NodeRef occupiedTarget;
  {
    auto write = graph.write();
    promptTarget = write.upsert({nodegraph::NodeKind::Item, "prompt"});
    messageTarget = write.upsert({nodegraph::NodeKind::Item, "message"});
    occupiedTarget = write.upsert({nodegraph::NodeKind::Item, "occupied"});
    static_cast<void>(write.finish());
  }

  VisibleCardData prompt;
  prompt.key = LocalPromptKey{7};
  prompt.kind = CardKind::LocalPrompt;
  prompt.threadId = "thread";
  prompt.turnId = "turn";
  prompt.payload = LocalPromptData{7, "Question", PromptState::InFlight};
  prompt.target = promptTarget;
  VisibleCardData occupied = card("occupied", occupiedTarget, "Existing");

  ConversationItemModel model;
  bool result = require(changed(model.replaceConversation(
                            snapshot({prompt, std::move(occupied)}))),
                        "prompt retarget fixture was not accepted");
  SignalLog log(model);

  VisibleCardData promoted;
  promoted.key = LocalPromptKey{7};
  promoted.kind = CardKind::UserMessage;
  promoted.threadId = "thread";
  promoted.turnId = "turn";
  promoted.itemId = "message";
  promoted.payload = UserMessageData{"Question", {}};
  promoted.target = occupiedTarget;
  result &= require(!applyPlacement(model, placement(promoted, true), {},
                                    model.card(1)->key) &&
                        model.indexForTarget(promptTarget).row() == 0 &&
                        model.card(0)->kind == CardKind::LocalPrompt &&
                        log.inserted.empty() && log.removed.empty() &&
                        log.moved.empty() && log.changed.empty(),
                    "a rejected prompt retarget partially mutated the row");

  promoted.target = messageTarget;
  ConversationRowPlacement invalid = placement(promoted, true);
  invalid.nested = true;
  result &= require(
      !applyPlacement(model, std::move(invalid), {}, model.card(1)->key) &&
          model.indexForTarget(promptTarget).row() == 0 &&
          model.card(0)->kind == CardKind::LocalPrompt &&
          log.inserted.empty() && log.removed.empty() && log.moved.empty() &&
          log.changed.empty(),
      "an invalid prompt placement partially mutated the row");

  result &= require(applyPlacement(model, placement(promoted, true), {},
                                   model.card(1)->key) &&
                        !model.indexForTarget(promptTarget).isValid() &&
                        model.indexForTarget(messageTarget).row() == 0 &&
                        model.card(0)->kind == CardKind::UserMessage,
                    "validated prompt promotion did not retarget atomically");
  return result;
}

bool testVisibilityAndLargeModelRemainDataOnly() {
  ConversationItemModel model;
  ConversationSnapshot data;
  data.threadId = "large";
  TurnSection section;
  section.key = "large-section";
  section.turnId = "large-turn";
  constexpr int Count = 10000;
  section.cards.reserve(Count);
  for (int index = 0; index < Count; ++index) {
    VisibleCardData row;
    row.key = AuthoritativeItemKey{"large", "large-turn",
                                   "item-" + std::to_string(index)};
    row.kind = index % 2 == 0 ? CardKind::Reasoning : CardKind::AgentMessage;
    row.threadId = "large";
    row.turnId = "large-turn";
    row.itemId = "item-" + std::to_string(index);
    row.payload = row.kind == CardKind::Reasoning
                      ? CardPayload{ReasoningData{"summary"}}
                      : CardPayload{AgentMessageData{"update", false}};
    section.cards.push_back(std::move(row));
  }
  section.rootCardKey = section.cards.front().key;
  data.sections.push_back(std::move(section));
  bool result = require(changed(model.replaceConversation(std::move(data))) &&
                            model.rowCount() == Count,
                        "ten-thousand-row model was not indexed");
  SignalLog log(model);
  result &= require(
      model.setVisibility({false, false}) &&
          !model.data(model.index(0, 0), ConversationItemModel::PresentedRole)
               .toBool() &&
          log.changed.size() == 1 && log.changed.front().first == 0 &&
          log.changed.front().last == Count - 1,
      "visibility did not produce one precise contiguous role change");
  log.clear();
  result &= require(!model.setVisibility({false, false}) && log.changed.empty(),
                    "identical visibility emitted work");
  VisibleCardData inserted;
  inserted.key = AuthoritativeItemKey{"large", "large-turn", "inserted"};
  inserted.kind = CardKind::AgentMessage;
  inserted.threadId = "large";
  inserted.turnId = "large-turn";
  inserted.itemId = "inserted";
  inserted.payload = AgentMessageData{"inserted", true};
  ConversationRowPlacement insertedPlacement;
  insertedPlacement.card = std::move(inserted);
  insertedPlacement.sectionKey = "large-section";
  insertedPlacement.nested = true;
  const qulonglong indexRebuilds =
      model.property("modelIndexRebuildCount").toULongLong();
  const qulonglong sectionRows =
      model.property("modelSectionStructureRowsTouched").toULongLong();
  result &= require(
      applyPlacement(model, std::move(insertedPlacement),
                     model.card(Count / 2 - 1)->key,
                     model.card(Count / 2)->key) &&
          model.rowCount() == Count + 1 &&
          model.property("modelIndexRebuildCount").toULongLong() ==
              indexRebuilds &&
          model.property("modelSectionStructureRowsTouched").toULongLong() -
                  sectionRows <=
              7,
      "a middle insert traversed the ten-thousand-row Turn section");
  return result;
}

bool testLargeRootReplacementRetiresOnlyItsIdentity() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef oldTarget;
  nodegraph::NodeRef newTarget;
  {
    auto write = graph.write();
    oldTarget = write.upsert({nodegraph::NodeKind::Item, "large-old-root"});
    newTarget = write.upsert({nodegraph::NodeKind::Item, "large-new-root"});
    static_cast<void>(write.finish());
  }

  constexpr int Count = 10'000;
  std::vector<VisibleCardData> cards;
  cards.reserve(Count);
  VisibleCardData oldRoot = card("old-root", oldTarget, "Old root");
  oldRoot.kind = CardKind::UserMessage;
  oldRoot.payload = UserMessageData{"Old root", {}};
  cards.push_back(oldRoot);
  for (int row = 1; row < Count; ++row)
    cards.push_back(card("suffix-" + std::to_string(row), {}, "Suffix"));
  const CardKey firstSuffixKey = cards[1].key;

  ConversationItemModel model;
  bool result =
      require(changed(model.replaceConversation(snapshot(std::move(cards)))) &&
                  model.rowCount() == Count,
              "large root-replacement fixture was not accepted");
  const QPersistentModelIndex retiredRoot(model.index(0));
  const std::vector<int> samples{1, Count / 3, Count / 2, Count - 1};
  std::vector<QPersistentModelIndex> retainedSuffixes;
  std::vector<std::string> retainedKeys;
  retainedSuffixes.reserve(samples.size());
  retainedKeys.reserve(samples.size());
  for (const int row : samples) {
    retainedSuffixes.emplace_back(model.index(row));
    retainedKeys.push_back(model.row(row)->stableKey);
  }

  bool validAfterInsert = false;
  bool validAfterRemove = false;
  QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                   [&](const QModelIndex &, int first, int last) {
                     const ConversationItemModel::Row *root = model.row(0);
                     const ConversationItemModel::Row *temporary = model.row(1);
                     validAfterInsert =
                         first == 1 && last == 1 && root && temporary &&
                         root->turnRoot &&
                         root->stableKey == stableKey(oldRoot.key) &&
                         !temporary->turnRoot && temporary->nested;
                   });
  QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                   [&](const QModelIndex &, int first, int last) {
                     const ConversationItemModel::Row *root = model.row(0);
                     validAfterRemove =
                         first == 0 && last == 0 && root && root->turnRoot &&
                         !root->nested &&
                         root->stableKey != stableKey(oldRoot.key);
                   });
  SignalLog log(model);
  const qulonglong indexRebuilds =
      model.property("modelIndexRebuildCount").toULongLong();
  const qulonglong sectionRows =
      model.property("modelSectionStructureRowsTouched").toULongLong();
  const qulonglong exactInserts =
      model.property("modelExactInsertCount").toULongLong();
  const qulonglong exactRemovals =
      model.property("modelExactRemoveCount").toULongLong();

  VisibleCardData newRoot = card("new-root", newTarget, "New root");
  newRoot.kind = CardKind::UserMessage;
  newRoot.payload = UserMessageData{"New root", {}};
  std::vector<ConversationRowChange> rows{
      {placement(newRoot, true), {}, firstSuffixKey}};
  const auto transaction = model.applyStructuralDelta(rows, {&oldTarget, 1});
  const QModelIndex replacement = model.indexForTarget(newTarget);

  result &= require(
      transaction && transaction->operations.size() == 1 &&
          transaction->operations.front().kind ==
              ConversationItemModel::StructuralDeltaPlan::Operation::Kind::
                  ReplaceRoot &&
          replacement.row() == 0 && model.rowCount() == Count &&
          replacement.data(ConversationItemModel::TurnRootRole).toBool() &&
          !replacement.data(ConversationItemModel::NestedCardRole).toBool(),
      "large root replacement did not commit as one typed transaction");
  result &= require(
      validAfterInsert && validAfterRemove && !retiredRoot.isValid() &&
          log.inserted.size() == 1 && log.inserted.front().first == 1 &&
          log.inserted.front().last == 1 && log.removed.size() == 1 &&
          log.removed.front().first == 0 && log.removed.front().last == 0 &&
          log.moved.empty() && log.resets == 0,
      "root replacement exposed a rootless model boundary or retained A's "
      "identity");
  result &= require(model.rowCount() == Count,
                    "a position-preserving root replacement changed row "
                    "membership");
  for (std::size_t sample = 0; sample < samples.size(); ++sample) {
    result &= require(
        retainedSuffixes[sample].isValid() &&
            retainedSuffixes[sample].row() == samples[sample] &&
            model.row(samples[sample])->stableKey == retainedKeys[sample] &&
            model.row(samples[sample])->nested,
        "root replacement changed a retained suffix identity or role");
  }
  result &= require(
      !log.changed.empty() &&
          std::ranges::all_of(log.changed,
                              [](const SignalLog::Range &change) {
                                return change.first == 0 && change.last == 0;
                              }) &&
          std::ranges::any_of(
              log.changedRoles,
              [](const QList<int> &roles) {
                return roles.contains(ConversationItemModel::TurnRootRole) &&
                       roles.contains(ConversationItemModel::NestedCardRole) &&
                       roles.contains(ConversationItemModel::FirstInTurnRole);
              }) &&
          model.property("modelSectionStructureRowsTouched").toULongLong() -
                  sectionRows <=
              7 &&
          model.property("modelIndexRebuildCount").toULongLong() ==
              indexRebuilds &&
          model.property("modelExactInsertCount").toULongLong() ==
              exactInserts + 1 &&
          model.property("modelExactRemoveCount").toULongLong() ==
              exactRemovals + 1,
      "root replacement traversed or structurally changed its retained "
      "suffix");
  return result;
}

bool testNonFirstRootReplacementNeverClaimsSectionFront() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef prefaceTarget;
  nodegraph::NodeRef oldRootTarget;
  nodegraph::NodeRef newRootTarget;
  nodegraph::NodeRef suffixTarget;
  {
    auto write = graph.write();
    prefaceTarget =
        write.upsert({nodegraph::NodeKind::Item, "non-first-preface"});
    oldRootTarget =
        write.upsert({nodegraph::NodeKind::Item, "non-first-old-root"});
    newRootTarget =
        write.upsert({nodegraph::NodeKind::Item, "non-first-new-root"});
    suffixTarget =
        write.upsert({nodegraph::NodeKind::Item, "non-first-suffix"});
    static_cast<void>(write.finish());
  }

  VisibleCardData preface = card("non-first-preface", prefaceTarget);
  VisibleCardData oldRoot = card("non-first-old-root", oldRootTarget);
  oldRoot.kind = CardKind::UserMessage;
  oldRoot.payload = UserMessageData{"Old root", {}};
  VisibleCardData suffix = card("non-first-suffix", suffixTarget);
  ConversationSnapshot initial;
  initial.threadId = "thread";
  initial.sections.push_back(
      {"section", "turn", {preface, oldRoot, suffix}, oldRoot.key});
  ConversationItemModel model;
  bool result = require(changed(model.replaceConversation(std::move(initial))),
                        "non-first root fixture was not accepted");

  const auto structureIsValid = [&] {
    int roots = 0;
    for (int rowIndex = 0; rowIndex < model.rowCount(); ++rowIndex) {
      const ConversationItemModel::Row *row = model.row(rowIndex);
      if (!row || row->firstInTurn != (rowIndex == 0))
        return false;
      if (row->turnRoot) {
        ++roots;
        if (rowIndex != 1 || row->nested)
          return false;
      } else if (row->nested != (rowIndex > 1)) {
        return false;
      }
    }
    return roots == 1;
  };
  bool validAfterInsert = false;
  bool validAfterRemove = false;
  QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                   [&](const QModelIndex &, int, int) {
                     validAfterInsert = structureIsValid();
                   });
  QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                   [&](const QModelIndex &, int, int) {
                     validAfterRemove = structureIsValid();
                   });

  VisibleCardData newRoot = card("non-first-new-root", newRootTarget);
  newRoot.kind = CardKind::UserMessage;
  newRoot.payload = UserMessageData{"New root", {}};
  std::vector<ConversationRowChange> rows{{
      {newRoot, "section", true, false, false}, preface.key, suffix.key}};
  const auto transaction =
      model.applyStructuralDelta(rows, {&oldRootTarget, 1});
  return require(
      transaction && validAfterInsert && validAfterRemove &&
          structureIsValid() && model.indexForTarget(newRootTarget).row() == 1,
      "a non-first root replacement exposed false section-front semantics");
}

bool testReversedSectionRootReplacementsRemainAtomic() {
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> targets;
  {
    auto write = graph.write();
    for (const char *id :
         {"root-a0", "child-a1", "root-b0", "child-b1", "root-a2", "root-b2"})
      targets.push_back(write.upsert({nodegraph::NodeKind::Item, id}));
    static_cast<void>(write.finish());
  }
  const auto makeCard = [&](std::string turn, std::string id,
                            nodegraph::NodeRef target, bool root) {
    VisibleCardData value;
    value.key = AuthoritativeItemKey{"thread", turn, id};
    value.kind = root ? CardKind::UserMessage : CardKind::AgentMessage;
    value.threadId = "thread";
    value.turnId = std::move(turn);
    value.itemId = id;
    value.payload = root ? CardPayload{UserMessageData{id, {}}}
                         : CardPayload{AgentMessageData{id, true}};
    value.target = std::move(target);
    return value;
  };
  VisibleCardData rootA0 = makeCard("turn-a", "root-a0", targets[0], true);
  VisibleCardData childA1 = makeCard("turn-a", "child-a1", targets[1], false);
  VisibleCardData rootB0 = makeCard("turn-b", "root-b0", targets[2], true);
  VisibleCardData childB1 = makeCard("turn-b", "child-b1", targets[3], false);
  VisibleCardData rootA2 = makeCard("turn-a", "root-a2", targets[4], true);
  VisibleCardData rootB2 = makeCard("turn-b", "root-b2", targets[5], true);

  ConversationSnapshot initial;
  initial.threadId = "thread";
  initial.sections.push_back(
      {"section-a", "turn-a", {rootA0, childA1}, rootA0.key});
  initial.sections.push_back(
      {"section-b", "turn-b", {rootB0, childB1}, rootB0.key});
  ConversationItemModel model;
  bool result = require(changed(model.replaceConversation(std::move(initial))),
                        "two-section root fixture was not accepted");
  const QPersistentModelIndex retiredA(model.indexForTarget(targets[0]));
  const QPersistentModelIndex childA(model.indexForTarget(targets[1]));
  const QPersistentModelIndex retiredB(model.indexForTarget(targets[2]));
  const QPersistentModelIndex childB(model.indexForTarget(targets[3]));
  QPersistentModelIndex insertedA;
  QPersistentModelIndex insertedB;
  bool signalSectionsValid = true;
  const auto sectionsValid = [&model] {
    std::unordered_map<std::string, int> roots;
    std::unordered_map<std::string, int> rows;
    for (int rowIndex = 0; rowIndex < model.rowCount(); ++rowIndex) {
      const ConversationItemModel::Row *row = model.row(rowIndex);
      if (!row)
        return false;
      ++rows[row->sectionKey];
      if (row->turnRoot)
        ++roots[row->sectionKey];
    }
    return std::ranges::all_of(rows, [&roots](const auto &entry) {
      const auto root = roots.find(entry.first);
      return root != roots.end() && root->second == 1;
    });
  };
  QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                   [&](const QModelIndex &, int, int) {
                     signalSectionsValid &= sectionsValid();
                     const QModelIndex a = model.indexForTarget(targets[4]);
                     const QModelIndex b = model.indexForTarget(targets[5]);
                     if (a.isValid())
                       insertedA = a;
                     if (b.isValid())
                       insertedB = b;
                   });
  QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                   [&](const QModelIndex &, int, int) {
                     signalSectionsValid &= sectionsValid();
                   });
  SignalLog log(model);
  std::vector<ConversationRowChange> rows{
      {{rootB2, "section-b", true, false, false}, childA1.key, childB1.key},
      {{rootA2, "section-a", true, false, false}, {}, childA1.key},
  };
  const std::vector<nodegraph::NodeRef> removals{targets[0], targets[2]};
  const auto transaction = model.applyStructuralDelta(rows, removals);
  result &= require(
      transaction && transaction->operations.size() == 2 &&
          transaction->operations[0].kind ==
              ConversationItemModel::StructuralDeltaPlan::Operation::Kind::
                  ReplaceRoot &&
          transaction->operations[0].deltaIndex == 1 &&
          transaction->operations[0].removalIndex == 0 &&
          transaction->operations[1].kind ==
              ConversationItemModel::StructuralDeltaPlan::Operation::Kind::
                  ReplaceRoot &&
          transaction->operations[1].deltaIndex == 0 &&
          transaction->operations[1].removalIndex == 1,
      "root replacements were not paired by section independently of delta "
      "order");
  result &= require(
      signalSectionsValid && sectionsValid() && !retiredA.isValid() &&
          !retiredB.isValid() && childA.isValid() && childA.row() == 1 &&
          childB.isValid() && childB.row() == 3 && insertedA.isValid() &&
          insertedA.row() == 0 && insertedB.isValid() && insertedB.row() == 2,
      "two-section replacement exposed an invalid root boundary or changed "
      "a retained identity");
  result &= require(
      log.inserted == std::vector<SignalLog::Range>{{1, 1}, {3, 3}} &&
          log.removed == std::vector<SignalLog::Range>{{0, 0}, {2, 2}} &&
          log.moved.empty() && log.resets == 0,
      "two-section replacement emitted imprecise structural signals");
  return result;
}

bool testAmbiguousRootReplacementRejectsBeforeCommit() {
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> targets;
  {
    auto write = graph.write();
    for (const char *id : {"ambiguous-root", "ambiguous-child",
                           "ambiguous-first", "ambiguous-second"})
      targets.push_back(write.upsert({nodegraph::NodeKind::Item, id}));
    static_cast<void>(write.finish());
  }
  VisibleCardData root = card("ambiguous-root", targets[0]);
  root.kind = CardKind::UserMessage;
  root.payload = UserMessageData{"Root", {}};
  VisibleCardData child = card("ambiguous-child", targets[1]);
  VisibleCardData first = card("ambiguous-first", targets[2]);
  first.kind = CardKind::UserMessage;
  first.payload = UserMessageData{"First", {}};
  VisibleCardData second = card("ambiguous-second", targets[3]);
  second.kind = CardKind::UserMessage;
  second.payload = UserMessageData{"Second", {}};
  ConversationItemModel model;
  bool result =
      require(changed(model.replaceConversation(snapshot({root, child}))),
              "ambiguous root fixture was not accepted");
  const QPersistentModelIndex persistentRoot(model.indexForTarget(targets[0]));
  const QPersistentModelIndex persistentChild(model.indexForTarget(targets[1]));
  SignalLog log(model);
  const qulonglong inserts =
      model.property("modelExactInsertCount").toULongLong();
  const qulonglong removalsBefore =
      model.property("modelExactRemoveCount").toULongLong();
  std::vector<ConversationRowChange> rows{
      {placement(first, true), {}, child.key},
      {placement(second, true), {}, child.key},
  };
  const std::vector<nodegraph::NodeRef> removals{targets[0]};
  result &= require(
      !model.planStructuralDelta(rows, removals) &&
          !model.applyStructuralDelta(rows, removals) &&
          persistentRoot.isValid() && persistentRoot.row() == 0 &&
          persistentChild.isValid() && persistentChild.row() == 1 &&
          model.rowCount() == 2 && log.inserted.empty() &&
          log.removed.empty() && log.moved.empty() && log.changed.empty() &&
          log.resets == 0 &&
          model.property("modelExactInsertCount").toULongLong() == inserts &&
          model.property("modelExactRemoveCount").toULongLong() ==
              removalsBefore,
      "ambiguous replacement roots mutated the model before "
      "atomic rejection");
  return result;
}

bool testMixedStructuralTransactionPreservesPersistentIndexes() {
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> targets;
  {
    auto write = graph.write();
    for (const char *id :
         {"mixed-root", "mixed-a", "mixed-b", "mixed-c", "mixed-x"})
      targets.push_back(write.upsert({nodegraph::NodeKind::Item, id}));
    static_cast<void>(write.finish());
  }
  VisibleCardData root = card("mixed-root", targets[0]);
  root.kind = CardKind::UserMessage;
  root.payload = UserMessageData{"Root", {}};
  VisibleCardData a = card("mixed-a", targets[1]);
  VisibleCardData b = card("mixed-b", targets[2]);
  VisibleCardData c = card("mixed-c", targets[3]);
  VisibleCardData x = card("mixed-x", targets[4]);
  ConversationItemModel model;
  bool result =
      require(changed(model.replaceConversation(snapshot({root, a, b, c}))),
              "mixed structural fixture was not accepted");
  const QPersistentModelIndex persistentRoot(model.indexForTarget(targets[0]));
  const QPersistentModelIndex retiredA(model.indexForTarget(targets[1]));
  const QPersistentModelIndex persistentB(model.indexForTarget(targets[2]));
  const QPersistentModelIndex persistentC(model.indexForTarget(targets[3]));
  QPersistentModelIndex insertedX;
  QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                   [&](const QModelIndex &, int, int) {
                     insertedX = model.indexForTarget(targets[4]);
                   });
  SignalLog log(model);
  std::vector<ConversationRowChange> rows{
      {placement(c), root.key, x.key},
      {placement(x), c.key, b.key},
  };
  const std::vector<nodegraph::NodeRef> removals{targets[1]};
  const auto transaction = model.applyStructuralDelta(rows, removals);
  result &= require(
      transaction && transaction->operations.size() == 3 &&
          model.rowCount() == 4 && persistentRoot.isValid() &&
          persistentRoot.row() == 0 && !retiredA.isValid() &&
          persistentC.isValid() && persistentC.row() == 1 &&
          insertedX.isValid() && insertedX.row() == 2 &&
          persistentB.isValid() && persistentB.row() == 3,
      "mixed transaction did not preserve every retained persistent index");
  result &= require(
      log.removed == std::vector<SignalLog::Range>{{1, 1}} &&
          log.moved == std::vector<SignalLog::Move>{{2, 2, 1}} &&
          log.inserted == std::vector<SignalLog::Range>{{2, 2}} &&
          log.resets == 0,
      "mixed transaction did not emit one exact remove, move, and insert");
  return result;
}

bool testDisconnectedTailCannotDuplicateSectionRoot() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef firstTarget;
  nodegraph::NodeRef lastTarget;
  nodegraph::NodeRef duplicateTarget;
  {
    auto write = graph.write();
    firstTarget = write.upsert({nodegraph::NodeKind::Item, "tail-first-root"});
    lastTarget = write.upsert({nodegraph::NodeKind::Item, "tail-last-root"});
    duplicateTarget =
        write.upsert({nodegraph::NodeKind::Item, "tail-duplicate-root"});
    static_cast<void>(write.finish());
  }

  VisibleCardData first = card("first-root", firstTarget);
  VisibleCardData last = card("last-root", lastTarget);
  ConversationSnapshot data;
  data.threadId = "thread";
  data.sections.push_back({"first-section", "first-turn", {first}, first.key});
  data.sections.push_back({"last-section", "last-turn", {last}, last.key});
  ConversationItemModel model;
  bool result = require(changed(model.replaceConversation(std::move(data))),
                        "disconnected-tail fixture was not accepted");
  SignalLog log(model);
  log.clear();

  VisibleCardData duplicate = card("duplicate-root", duplicateTarget);
  ConversationRowPlacement duplicatePlacement;
  duplicatePlacement.card = std::move(duplicate);
  duplicatePlacement.sectionKey = "first-section";
  duplicatePlacement.turnRoot = true;
  result &=
      require(!applyPlacement(model, std::move(duplicatePlacement), last.key) &&
                  model.rowCount() == 2 &&
                  model.indexForTarget(firstTarget).row() == 0 &&
                  model.indexForTarget(lastTarget).row() == 1 &&
                  !model.indexForTarget(duplicateTarget).isValid() &&
                  log.inserted.empty() && log.removed.empty() &&
                  log.moved.empty() && log.changed.empty() && log.resets == 0,
              "a tail fast path admitted a disconnected second section root");
  return result;
}

bool testExistingRowsCannotChangeStructuralRootIdentity() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef rootTarget;
  nodegraph::NodeRef childTarget;
  nodegraph::NodeRef tailTarget;
  {
    auto write = graph.write();
    rootTarget = write.upsert({nodegraph::NodeKind::Item, "stable-root"});
    childTarget = write.upsert({nodegraph::NodeKind::Item, "stable-child"});
    tailTarget = write.upsert({nodegraph::NodeKind::Item, "stable-tail"});
    static_cast<void>(write.finish());
  }

  VisibleCardData root = card("stable-root", rootTarget, "Question");
  root.kind = CardKind::UserMessage;
  root.payload = UserMessageData{"Question", {}};
  VisibleCardData child = card("stable-child", childTarget, "Answer");
  child.kind = CardKind::UserMessage;
  child.payload = UserMessageData{"Steering", {}};
  VisibleCardData tail = card("stable-tail", tailTarget, "Activity");
  ConversationItemModel model;
  bool result =
      require(changed(model.replaceConversation(snapshot({root, child, tail}))),
              "structural-root fixture was not accepted");
  SignalLog log(model);
  const QPersistentModelIndex persistentRoot = model.indexForTarget(rootTarget);
  const QPersistentModelIndex persistentChild =
      model.indexForTarget(childTarget);
  const QPersistentModelIndex persistentTail = model.indexForTarget(tailTarget);

  ConversationRowPlacement demotedRoot = placement(root);
  ConversationRowPlacement promotedChild = placement(child, true);
  std::vector<ConversationRowChange> rows{
      {std::move(demotedRoot), child.key, tail.key},
      {std::move(promotedChild), {}, root.key},
  };
  result &= require(
      !model.applyStructuralDelta(rows, {}).has_value() &&
          model.rowCount() == 3 && persistentRoot.isValid() &&
          persistentRoot.row() == 0 &&
          persistentRoot.data(ConversationItemModel::TurnRootRole).toBool() &&
          persistentChild.isValid() && persistentChild.row() == 1 &&
          !persistentChild.data(ConversationItemModel::TurnRootRole).toBool() &&
          persistentTail.isValid() && persistentTail.row() == 2 &&
          log.inserted.empty() && log.removed.empty() && log.moved.empty() &&
          log.changed.empty() && log.resets == 0,
      "a Place transaction changed the section's structural-root identity");
  return result;
}

bool testTailAppendKeepsEveryLoadedIdentityIndex() {
  ConversationItemModel model;
  ConversationSnapshot data;
  data.threadId = "tail-thread";
  TurnSection section;
  section.key = "tail-section";
  section.turnId = "tail-turn";
  constexpr int Count = 10'000;
  section.cards.reserve(Count);
  for (int position = 0; position < Count; ++position) {
    VisibleCardData value;
    value.key = AuthoritativeItemKey{"tail-thread", "tail-turn",
                                     "item-" + std::to_string(position)};
    value.kind = position == 0 ? CardKind::UserMessage : CardKind::AgentMessage;
    value.threadId = "tail-thread";
    value.turnId = "tail-turn";
    value.itemId = "item-" + std::to_string(position);
    value.payload = position == 0
                        ? CardPayload{UserMessageData{"Question", {}}}
                        : CardPayload{AgentMessageData{"Answer", true}};
    section.cards.push_back(std::move(value));
  }
  section.rootCardKey = section.cards.front().key;
  data.sections.push_back(std::move(section));
  bool result = require(changed(model.replaceConversation(std::move(data))),
                        "tail fixture was not accepted");
  SignalLog log(model);
  const qulonglong rebuilds =
      model.property("modelIndexRebuildCount").toULongLong();

  const auto append = [&](int serial) {
    ConversationRowPlacement tail;
    tail.card.key = AuthoritativeItemKey{"tail-thread", "tail-turn",
                                         "item-" + std::to_string(serial)};
    tail.card.kind = CardKind::AgentMessage;
    tail.card.threadId = "tail-thread";
    tail.card.turnId = "tail-turn";
    tail.card.itemId = "item-" + std::to_string(serial);
    tail.card.payload = AgentMessageData{"Tail", true};
    tail.sectionKey = "tail-section";
    tail.nested = true;
    return applyPlacement(model, std::move(tail),
                          model.card(model.rowCount() - 1)->key);
  };

  result &= require(append(Count), "first tail append failed");
  result &= require(model.rowCount() == Count + 1 &&
                        log.inserted.size() == 1 &&
                        log.inserted.front().first == Count,
                    "first append did not retain every loaded row");

  log.clear();
  result &= require(append(Count + 1), "second tail append failed");
  result &= require(
      model.rowCount() == Count + 2 && log.inserted.size() == 1 &&
          log.inserted.front().first == Count + 1 && log.removed.empty(),
      "tail append removed or failed to index a loaded row");
  result &= require(
      model.property("modelIndexRebuildCount").toULongLong() == rebuilds &&
          model.indexForStableKey("item:11:tail-thread9:tail-turn6:item-0")
                  .row() == 0 &&
          model.indexForStableKey("item:11:tail-thread9:tail-turn6:item-2")
                  .row() == 2 &&
          model.indexForStableKey("item:11:tail-thread9:tail-turn10:item-10001")
                  .row() == Count + 1,
      "tail append rebuilt or lost absolute stable identity indexes");
  return result;
}

 bool testHeightIndexIsBoundedAndExact() {
  constexpr std::size_t Count = 10000;
  std::vector<int> heights(Count);
  for (std::size_t row = 0; row < Count; ++row)
    heights[row] = 20 + static_cast<int>(row % 17);
  ConversationHeightIndex index;
  index.assign(heights);
  bool result =
      require(index.size() == Count, "height index did not retain every row");
  std::vector<qint64> prefix(Count + 1, 0);
  for (std::size_t row = 0; row < Count; ++row)
    prefix[row + 1] = prefix[row] + heights[row];
  for (std::size_t sample = 0; sample < 1000; ++sample) {
    const qint64 y = prefix.back() * static_cast<qint64>(sample) / 1000;
    const auto expected = static_cast<std::size_t>(
        std::upper_bound(prefix.begin(), prefix.end(), y) - prefix.begin() - 1);
    if (!require(index.rowAt(y) == std::min(expected, Count - 1),
                 "position-to-row lookup returned the wrong row") ||
        !require(index.lastLookupSteps() <= 64,
                 "position-to-row lookup exceeded logarithmic steps")) {
      result = false;
      break;
    }
  }

  const std::size_t rebuilds = index.rebuildCount();
  const qint64 totalBefore = index.totalHeight();
  result &= require(index.setHeight(5000, heights[5000] + 91) &&
                        index.totalHeight() == totalBefore + 91 &&
                        index.rebuildCount() == rebuilds &&
                        index.lastUpdateSteps() <= 64,
                    "one height update was not exact and logarithmic");
  const std::vector<int> appended{41, 42};
  index.insert(index.size(), appended);
  result &= require(index.size() == Count + 2 && index.height(Count) == 41 &&
                        index.height(Count + 1) == 42 &&
                        index.rebuildCount() == rebuilds,
                    "tail append rebuilt retained height state");

  const std::vector<int> inserted{77};
  index.insert(3, inserted);
  result &= require(index.height(3) == 77 && index.rebuildCount() == rebuilds &&
                        index.lastUpdateSteps() <= 128,
                    "non-tail insertion was not exact and bounded");
  index.move(3, 1, 8);
  result &= require(index.height(8) == 77 && index.rebuildCount() == rebuilds &&
                        index.lastUpdateSteps() <= 256,
                    "height movement did not retain a bounded moved extent");
  index.remove(8, 1);
  result &=
      require(index.size() == Count + 2 && index.rebuildCount() == rebuilds &&
                  index.lastUpdateSteps() <= 128,
              "height removal was not exact and bounded");

  std::vector<int> sparseHeights(Count, 0);
  sparseHeights.front() = 31;
  sparseHeights.back() = 47;
  ConversationHeightIndex sparse;
  sparse.assign(sparseHeights);
  result &= require(
      sparse.nextRowWithExtent(0) == 0 &&
          sparse.nextRowWithExtent(1) == Count - 1 &&
          sparse.lastLookupSteps() <= 64 &&
          sparse.nextRowWithExtent(Count) == Count &&
          sparse.previousRowWithExtent(Count - 1) == Count - 1 &&
          sparse.previousRowWithExtent(Count - 2) == 0 &&
          sparse.lastLookupSteps() <= 64,
      "zero-height rows were not skipped on logarithmic geometry paths");

  ConversationHeightIndex prefixIndex;
  const std::vector<int> prefixHeights{50, 20, 30, 40};
  prefixIndex.assign(prefixHeights);
  const std::size_t prefixRebuilds = prefixIndex.rebuildCount();
  prefixIndex.remove(0, 1);
  result &=
      require(prefixIndex.size() == 3 && prefixIndex.height(0) == 20 &&
                  prefixIndex.top(1) == 20 && prefixIndex.totalHeight() == 90 &&
                  prefixIndex.rowAt(21) == 1 &&
                  prefixIndex.rebuildCount() == prefixRebuilds,
              "prefix removal was not exact and bounded");
  const std::vector<int> prefixTail{55};
  prefixIndex.insert(prefixIndex.size(), prefixTail);
  result &= require(prefixIndex.size() == 4 && prefixIndex.height(3) == 55 &&
                        prefixIndex.totalHeight() == 145 &&
                        prefixIndex.rebuildCount() == prefixRebuilds,
                    "tail append after prefix removal rebuilt height state");

  ConversationHeightIndex pinnedRoot;
  const std::vector<int> pinnedHeights{60, 20, 30};
  pinnedRoot.assign(pinnedHeights);
  const std::size_t pinnedRebuilds = pinnedRoot.rebuildCount();
  pinnedRoot.remove(1, 1);
  result &= require(pinnedRoot.size() == 2 && pinnedRoot.height(0) == 60 &&
                        pinnedRoot.height(1) == 30 &&
                        pinnedRoot.totalHeight() == 90 &&
                        pinnedRoot.rebuildCount() == pinnedRebuilds,
                    "pinned-root prefix trim was not exact and bounded");
  return result;
}

bool testHeightIndexScalarUpdatesPreserveGeometry() {
  ConversationHeightIndex index;
  static_assert(noexcept(index.setHeight(0, 1)));
  bool result = require(!index.setHeight(0, 1) && index.empty() &&
                            index.totalHeight() == 0 &&
                            index.lastUpdateSteps() == 0,
                        "scalar update of an empty height index changed it");
  std::vector<int> heights(65);
  for (std::size_t row = 0; row < heights.size(); ++row)
    heights[row] = row % 5 == 0 ? 0 : 7 + static_cast<int>(row);
  index.assign(heights);
  const auto rebuilds = index.rebuildCount();
  const auto verify = [&] {
    bool exact = true;
    qint64 top = 0;
    for (std::size_t row = 0; row < heights.size(); ++row) {
      const qint64 bottom = top + heights[row];
      exact &= require(index.height(row) == heights[row] &&
                           index.top(row) == top && index.bottom(row) == bottom,
                       "scalar update changed a row height or prefix incorrectly");
      if (heights[row] > 0)
        exact &= require(index.rowAt(top) == row &&
                             index.rowAt(bottom - 1) == row,
                         "scalar update broke extent-boundary lookup");
      top = bottom;
    }
    exact &= require(index.size() == heights.size() &&
                         index.totalHeight() == top &&
                         index.top(index.size()) == top &&
                         index.rebuildCount() == rebuilds,
                     "scalar update changed row count, total or rebuild count");
    if (top == 0)
      exact &= require(index.rowAt(0) == 0,
                       "all-folded height index has no valid zero position");
    return exact;
  };
  const auto update = [&](std::size_t row, int requested) {
    const int expected = std::max(0, requested);
    const bool changed = heights[row] != expected;
    const auto lookupSteps = index.lastLookupSteps();
    bool exact = require(index.setHeight(row, requested) == changed,
                         "scalar height update returned the wrong change flag");
    exact &= require(index.lastLookupSteps() == lookupSteps &&
                         (changed ? index.lastUpdateSteps() > 0 &&
                                        index.lastUpdateSteps() <= 64
                                  : index.lastUpdateSteps() == 0),
                     "scalar update changed lookup/update diagnostic meaning");
    heights[row] = expected;
    exact &= verify();
    if (!exact)
      std::cerr << "height update row=" << row << " requested=" << requested
                << '\n';
    result &= exact;
  };
  result &= verify();
  for (int phase = 0; phase < 2; ++phase) {
    for (std::size_t sample = 0; sample < heights.size(); ++sample) {
      const std::size_t row = (sample * 37) % heights.size();
      for (int requested : {0, std::numeric_limits<int>::min(),
                            std::numeric_limits<int>::max(),
                            std::numeric_limits<int>::max(), 7, 7})
        update(row, requested);
    }
    if (phase == 0) {
      const std::vector<int> inserted{std::numeric_limits<int>::max(), 0, 41};
      index.insert(3, inserted);
      heights.insert(heights.begin() + 3, inserted.begin(), inserted.end());
      result &= verify();
      index.move(2, 4, 10);
      std::rotate(heights.begin() + 2, heights.begin() + 6, heights.begin() + 14);
      result &= verify();
      index.remove(5, 3);
      heights.erase(heights.begin() + 5, heights.begin() + 8);
      result &= verify();
    }
  }
  for (std::size_t row = 0; row < heights.size(); ++row)
    update(row, 0);
  update(heights.size() - 1, std::numeric_limits<int>::max());
  update(0, std::numeric_limits<int>::max());
  const auto steps = index.lastUpdateSteps();
  for (std::size_t row : {index.size(), std::numeric_limits<std::size_t>::max()})
    result &= require(!index.setHeight(row, 1) && index.lastUpdateSteps() == steps,
                      "invalid scalar update changed the index or its diagnostics");
  result &= verify();
  return result;
}

bool testAccessibilityProjectionStopsAtItsVisibleBound() {
  ConversationItemModel model;
  VisibleCardData files;
  files.key = AuthoritativeItemKey{"accessible-thread", "turn", "files"};
  files.kind = CardKind::FileChanges;
  files.threadId = "accessible-thread";
  files.turnId = "turn";
  files.itemId = "files";
  files.payload = FileChangesData{
      {{std::string(100'000, 'x'), "update", 1, 1},
       {"must-not-be-projected-after-the-bound", "update", 1, 1}},
      {}};
  files.status = nodegraph::NodeStatus::Completed;
  bool result =
      require(changed(model.replaceConversation(snapshot({std::move(files)}))),
              "large accessibility fixture was not accepted");
  const QString fileText =
      model.index(0).data(Qt::AccessibleTextRole).toString();
  result &= require(
      fileText.size() <= 8210 && fileText.endsWith(QStringLiteral("…")) &&
          !fileText.contains(QStringLiteral("must-not-be-projected")),
      "file accessibility traversed beyond its bounded text");

  VisibleCardData plan;
  plan.key = AuthoritativeItemKey{"accessible-thread", "turn", "plan"};
  plan.kind = CardKind::Plan;
  plan.threadId = "accessible-thread";
  plan.turnId = "turn";
  plan.itemId = "plan";
  plan.payload = PlanData{std::string(100'000, 'p'),
                          {{"must-not-be-projected-after-the-bound",
                            nodegraph::NodeStatus::Pending}},
                          {}};
  result &=
      require(changed(model.replaceConversation(snapshot({std::move(plan)}))),
              "large plan accessibility fixture was not accepted");
  const QString planText =
      model.index(0).data(Qt::AccessibleTextRole).toString();
  result &= require(
      planText.size() <= 8210 && planText.endsWith(QStringLiteral("…")) &&
          !planText.contains(QStringLiteral("must-not-be-projected")),
      "plan accessibility traversed beyond its bounded text");
  plan = *model.card(0);
  plan.payload = PlanData{"Plan explanation",
                          {{"first", nodegraph::NodeStatus::Pending},
                           {"second", nodegraph::NodeStatus::Running},
                           {"third", nodegraph::NodeStatus::Completed}},
                          {}};
  result &=
      require(model.updateCard(plan) ==
                      ConversationItemModel::CardUpdateResult::Changed &&
                  model.index(0).data(Qt::AccessibleTextRole).toString() ==
                      QStringLiteral("Plan\nPlan explanation\npending: first\n"
                                     "running: second\ncompleted: third"),
              "plan summary exposes every typed step status");
  std::get<PlanData>(plan.payload).steps[0].status =
      nodegraph::NodeStatus::Completed;
  result &= require(
      model.updateCard(plan) ==
              ConversationItemModel::CardUpdateResult::Changed &&
          model.index(0)
              .data(Qt::AccessibleTextRole)
              .toString()
              .contains(QStringLiteral("completed: first")) &&
          model.updateCard(plan) ==
              ConversationItemModel::CardUpdateResult::Unchanged,
      "status-only changes reach summaries and repeated status is a no-op");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  using namespace codexui::codex::middle;
  bool result = testStableIdentityAndExactSignals();
  result &= testAllLoadedReconciliationInsertsOnlyMissingRanges();
  result &= testPromptRetargetIsAtomic();
  result &= testVisibilityAndLargeModelRemainDataOnly();
  result &= testLargeRootReplacementRetiresOnlyItsIdentity();
  result &= testNonFirstRootReplacementNeverClaimsSectionFront();
  result &= testReversedSectionRootReplacementsRemainAtomic();
  result &= testAmbiguousRootReplacementRejectsBeforeCommit();
  result &= testMixedStructuralTransactionPreservesPersistentIndexes();
  result &= testDisconnectedTailCannotDuplicateSectionRoot();
  result &= testExistingRowsCannotChangeStructuralRootIdentity();
  result &= testTailAppendKeepsEveryLoadedIdentityIndex();
  result &= testHeightIndexIsBoundedAndExact();
  result &= testHeightIndexScalarUpdatesPreserveGeometry();
  result &= testAccessibilityProjectionStopsAtItsVisibleBound();
  if (result)
    std::cout << "Conversation item model tests passed\n";
  return result ? 0 : 1;
}
