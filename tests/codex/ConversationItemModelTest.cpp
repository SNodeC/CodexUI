// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationItemModel.h"
#include "codex/middle/ConversationHeightIndex.h"
#include "codex/nodegraph/NodeGraph.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <string>
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
  };
  struct Move {
    int first = -1;
    int last = -1;
    int destination = -1;
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
  bool result = require(
      model.reconcile(snapshot({card("same-wire-id-a", first, "one"),
                                card("same-wire-id-b", second, "two")})),
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
      !model.reconcile(snapshot({card("same-wire-id-a", first, "one"),
                                 card("same-wire-id-b", second, "two")})) &&
          log.resets == 0 && log.inserted.empty() && log.removed.empty() &&
          log.moved.empty() && log.changed.empty(),
      "identical model state emitted presentation work");
  result &= require(model.property("modelIndexRebuildCount").toULongLong() ==
                        indexRebuilds,
                    "identical model state rebuilt stable indexes");

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
      model.reconcile(snapshot({card("same-wire-id-a", first, "one"),
                                card("inserted", third, "three"),
                                card("same-wire-id-b", second, "streamed")})) &&
          log.inserted.size() == 1 && log.inserted.front().first == 1 &&
          log.inserted.front().last == 1 && log.resets == 0,
      "middle insertion did not use beginInsertRows/endInsertRows");

  log.clear();
  result &= require(
      model.reconcile(snapshot({card("inserted", third, "three"),
                                card("same-wire-id-a", first, "one"),
                                card("same-wire-id-b", second, "streamed")})) &&
          log.moved.size() == 1 && log.moved.front().first == 1 &&
          log.moved.front().last == 1 && log.moved.front().destination == 0 &&
          log.resets == 0,
      "actual reordering did not use beginMoveRows/endMoveRows");

  log.clear();
  result &= require(
      model.reconcile(snapshot({card("inserted", third, "three"),
                                card("same-wire-id-b", second, "streamed")})) &&
          log.removed.size() == 1 && log.removed.front().first == 1 &&
          log.removed.front().last == 1 && log.resets == 0,
      "removal did not use beginRemoveRows/endRemoveRows");
  result &= require(
      model.indexForTarget(second).row() == 1 &&
          model.indexForStableKey("item:6:thread4:turn14:same-wire-id-b")
                  .row() == 1,
      "stable and exact target indexes were not rebuilt");

  log.clear();
  result &= require(model.reconcile(snapshot({}, "replacement")) &&
                        log.resets == 1 && model.rowCount() == 0,
                    "genuine thread replacement did not use a model reset");
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
  bool result =
      require(model.reconcile(std::move(data)) && model.rowCount() == Count,
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
        !require(index.lastLookupSteps() <= 15,
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
                        index.lastUpdateSteps() <= 15,
                    "one height update was not exact and logarithmic");
  const std::vector<int> appended{41, 42};
  index.insert(index.size(), appended);
  result &= require(index.size() == Count + 2 && index.height(Count) == 41 &&
                        index.height(Count + 1) == 42 &&
                        index.rebuildCount() == rebuilds,
                    "tail append rebuilt retained height state");

  const std::vector<int> inserted{77};
  index.insert(3, inserted);
  result &=
      require(index.height(3) == 77 && index.rebuildCount() == rebuilds + 1,
              "non-tail insertion did not rebuild exact prefix state");
  index.move(3, 1, 8);
  result &= require(index.height(8) == 77,
                    "height movement did not retain the moved extent");
  index.remove(8, 1);
  result &= require(index.size() == Count + 2,
                    "height removal did not restore the expected row count");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  using namespace codexui::codex::middle;
  bool result = testStableIdentityAndExactSignals();
  result &= testVisibilityAndLargeModelRemainDataOnly();
  result &= testHeightIndexIsBoundedAndExact();
  if (result)
    std::cout << "Conversation item model tests passed\n";
  return result ? 0 : 1;
}
