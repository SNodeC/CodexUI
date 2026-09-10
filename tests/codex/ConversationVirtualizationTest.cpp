// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QHelpEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QThread>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace codexui::codex::middle {
namespace {

bool expect(bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

VisibleCardData message(std::size_t serial, std::string text = {}) {
  const std::string suffix = std::to_string(serial);
  if (text.empty())
    text = "Answer " + suffix;
  return {AuthoritativeItemKey{"virtual-thread", "turn-" + suffix,
                               "item-" + suffix},
          CardKind::AgentMessage,
          "virtual-thread",
          "turn-" + suffix,
          "item-" + suffix,
          AgentMessageData{std::move(text), true}};
}

ConversationSnapshot conversation(std::size_t count,
                                  std::size_t firstSerial = 0) {
  ConversationSnapshot result;
  result.threadId = "virtual-thread";
  result.sections.reserve(count);
  for (std::size_t offset = 0; offset < count; ++offset) {
    VisibleCardData card = message(firstSerial + offset);
    TurnSection section;
    section.key = "section-" + std::to_string(firstSerial + offset);
    section.turnId = card.turnId;
    section.cards.push_back(std::move(card));
    result.sections.push_back(std::move(section));
  }
  return result;
}

ConversationSnapshot pinnedTurnPage(std::size_t firstActivity,
                                    std::size_t activityCount) {
  ConversationSnapshot result;
  result.threadId = "virtual-thread";
  TurnSection section;
  section.key = "shared-turn-section";
  section.turnId = "shared-turn";
  VisibleCardData root{
      AuthoritativeItemKey{"virtual-thread", "shared-turn", "root"},
      CardKind::UserMessage,
      "virtual-thread",
      "shared-turn",
      "root",
      UserMessageData{"Continue"}};
  section.rootCardKey = root.key;
  section.rootPinned = firstActivity != 1;
  section.cards.push_back(std::move(root));
  for (std::size_t serial = firstActivity;
       serial < firstActivity + activityCount; ++serial) {
    const std::string suffix = std::to_string(serial);
    section.cards.push_back(
        {AuthoritativeItemKey{"virtual-thread", "shared-turn",
                              "item-" + suffix},
         CardKind::AgentMessage,
         "virtual-thread",
         "shared-turn",
         "item-" + suffix,
         AgentMessageData{"Answer " + suffix, true}});
  }
  result.sections.push_back(std::move(section));
  return result;
}

void settle(int passes = 4) {
  while (passes-- > 0)
    QApplication::processEvents(QEventLoop::AllEvents, 20);
}

template <typename Predicate>
bool waitUntil(Predicate &&predicate, int timeoutMilliseconds) {
  QElapsedTimer deadline;
  deadline.start();
  while (!predicate() && deadline.elapsed() < timeoutMilliseconds) {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  return predicate();
}

ConversationSnapshot singleMessageConversation(const std::string &threadId,
                                                const std::string &text) {
  VisibleCardData card{
      AuthoritativeItemKey{threadId, "turn", "message"},
      CardKind::AgentMessage,
      threadId,
      "turn",
      "message",
      AgentMessageData{text, true}};
  ConversationSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.sections.push_back(
      {"section", "turn", {std::move(card)}, std::nullopt});
  return snapshot;
}

std::pair<std::string, int> firstVisible(ConversationView &view) {
  for (int y = 0; y < view.viewport()->height(); ++y) {
    const QModelIndex index =
        view.indexAt(QPoint(view.viewport()->width() / 2, y));
    if (!index.isValid())
      continue;
    return {index.data(ConversationItemModel::StableKeyRole)
                .toString()
                .toStdString(),
            view.visualRect(index).top()};
  }
  return {};
}

ConversationCard *materializedCard(ConversationView &view,
                                   const std::string &key) {
  for (ConversationCard *card : view.findChildren<ConversationCard *>())
    if (card->property("conversationAnchorKey").toString().toStdString() == key)
      return card;
  return nullptr;
}

void sendViewportMouse(ConversationView &view, QEvent::Type type,
                       const QPoint &position, Qt::MouseButton button,
                       Qt::MouseButtons buttons) {
  QMouseEvent event(type, QPointF(position), QPointF(position),
                    view.viewport()->mapToGlobal(position), button, buttons,
                    Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &event);
}

bool viewportProportionalFoundation() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  settle();

  ConversationSnapshot snapshot = conversation(10'000);
  bool result = expect(view.reconcile(snapshot),
                       "ten-thousand-row authority is accepted");
  settle();
  const int initialWidgets = view.materializedCardCount();
  result &= expect(view.conversationModel()->rowCount() == 10'000,
                   "the item model indexes all canonical rows");
  result &= expect(initialWidgets <= 48,
                   "initial QWidget count is bounded by the viewport");
  result &= expect(view.findChildren<ConversationCard *>().size() <= 48,
                   "history has no placeholder or hidden QWidget per row");
  result &= expect(view.isAtBottom(),
                   "initial selection reveals a complete following tail");

  const int middle = view.verticalScrollBar()->maximum() / 2;
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(middle);
  settle();
  const auto anchorBefore = firstVisible(view);
  result &= expect(!anchorBefore.first.empty() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "manual navigation establishes a painted paused anchor");
  const int beforeAppendWidgets = view.materializedCardCount();

  ConversationSnapshot prepended = snapshot;
  VisibleCardData inserted = message(10'000);
  TurnSection insertedSection;
  insertedSection.key = "section-10000";
  insertedSection.turnId = inserted.turnId;
  insertedSection.cards.push_back(std::move(inserted));
  prepended.sections.insert(prepended.sections.begin(),
                            std::move(insertedSection));
  result &= expect(view.reconcile(prepended),
                   "a structural insertion reconciles precisely");
  settle();
  const auto anchorAfter = firstVisible(view);
  result &= expect(anchorAfter == anchorBefore,
                   "insertion above preserves identity and exact pixel offset");
  result &= expect(view.horizontalScrollBar()->value() == 0 &&
                       view.materializedCardCount() <=
                           std::max(48, beforeAppendWidgets + 4),
                   "structural insertion preserves both axes and widget bound");

  const QModelIndex offscreen = view.conversationModel()->index(0);
  const std::string offscreenKey =
      offscreen.data(ConversationItemModel::StableKeyRole)
          .toString()
          .toStdString();
  result &= expect(materializedCard(view, offscreenKey) == nullptr,
                   "chosen offscreen row has no QWidget");
  VisibleCardData offscreenUpdate =
      *view.conversationModel()->card(offscreen.row());
  std::get<AgentMessageData>(offscreenUpdate.payload).text += " updated";
  const qulonglong constructionsBefore =
      view.property("conversationCardConstructions").toULongLong();
  const auto offscreenImpact =
      view.applyCardPresentation(std::move(offscreenUpdate));
  settle();
  result &=
      expect(offscreenImpact == PresentationImpact::None &&
                 materializedCard(view, offscreenKey) == nullptr &&
                 view.property("conversationCardConstructions").toULongLong() ==
                     constructionsBefore,
             "offscreen delta performs no QWidget construction");

  const auto visibleIdentity = firstVisible(view);
  const QModelIndex visibleIndex =
      view.conversationModel()->indexForStableKey(visibleIdentity.first);
  VisibleCardData visibleUpdate =
      *view.conversationModel()->card(visibleIndex.row());
  std::get<AgentMessageData>(visibleUpdate.payload).text +=
      std::string(240, 'x');
  const qulonglong offscreenBefore =
      view.property("targetedOffscreenCardUpdates").toULongLong();
  const qulonglong visibleConstructionsBefore =
      view.property("conversationCardConstructions").toULongLong();
  const auto visibleImpact =
      view.applyCardPresentation(std::move(visibleUpdate));
  settle();
  result &= expect(
      materializedCard(view, visibleIdentity.first) == nullptr &&
          visibleImpact == PresentationImpact::GeometryChanged &&
          view.property("targetedOffscreenCardUpdates").toULongLong() ==
              offscreenBefore &&
          view.property("conversationCardConstructions").toULongLong() ==
              visibleConstructionsBefore,
      "one visible stream update invalidates only its passive delegate row");
  result &= expect(firstVisible(view).second == visibleIdentity.second,
                   "visible height change preserves the exact painted anchor");
  return result;
}

bool exactStructuralRowsPreserveTheViewport() {
  nodegraph::NodeGraph graph;
  std::vector<nodegraph::NodeRef> targets;
  nodegraph::NodeRef insertedTarget;
  {
    auto write = graph.write();
    for (int row = 0; row < 10'000; ++row)
      targets.push_back(write.upsert(
          {nodegraph::NodeKind::Item, "exact-row-" + std::to_string(row)}));
    insertedTarget =
        write.upsert({nodegraph::NodeKind::Item, "exact-row-inserted"});
    static_cast<void>(write.finish());
  }

  ConversationSnapshot snapshot = conversation(targets.size());
  for (std::size_t row = 0; row < targets.size(); ++row)
    snapshot.sections[row].cards.front().target = targets[row];
  ConversationView view;
  view.resize(820, 420);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "exact structural row fixture reconciles");
  settle();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  settle();

  const auto anchorBeforeInsert = firstVisible(view);
  VisibleCardData inserted = message(10'001, "Inserted in the middle");
  inserted.target = insertedTarget;
  ConversationRowChange insertion;
  insertion.placement =
      {inserted, "section-inserted", false, false, false, true};
  insertion.previousCardKey = message(7).key;
  insertion.nextCardKey = message(8).key;
  const qulonglong resetsBefore =
      view.conversationModel()->property("modelResetCount").toULongLong();
  const qulonglong identityRebuildsBefore = view.conversationModel()
                                                  ->property(
                                                      "modelIndexRebuildCount")
                                                  .toULongLong();
  const qulonglong sectionRebuildsBefore =
      view.property("conversationSectionRangeRebuilds").toULongLong();
  const qulonglong heightRebuildsBefore =
      view.property("conversationHeightIndexRebuilds").toULongLong();
  const qulonglong structuralRepaintsBefore =
      view.property("targetedStructuralRepaints").toULongLong();
  const qulonglong offscreenRepaintsBefore =
      view.property("targetedStructuralOffscreenRepaintsAvoided")
          .toULongLong();
  result &= expect(view.applyRowChange(std::move(insertion)) &&
                       view.conversationModel()
                               ->indexForTarget(insertedTarget)
                               .row() == 8 &&
                       firstVisible(view) == anchorBeforeInsert,
                   "a middle insertion preserves the exact pixel anchor");

  ConversationRowChange movement;
  movement.placement =
      {message(2), "section-2", false, false, false, true};
  movement.placement.card.target = targets[2];
  movement.previousCardKey =
      view.conversationModel()
          ->row(view.conversationModel()->rowCount() - 1)
          ->card.key;
  const auto anchorBeforeMove = firstVisible(view);
  const bool moved = view.applyRowChange(std::move(movement));
  const auto anchorAfterMove = firstVisible(view);
  result &= expect(moved &&
                       view.conversationModel()->indexForTarget(targets[2])
                               .row() ==
                           view.conversationModel()->rowCount() - 1 &&
                       anchorAfterMove == anchorBeforeMove,
                   "an exact row move preserves the viewport anchor");

  const auto anchorBeforeRemoval = firstVisible(view);
  const bool removed = view.removeCardTarget(targets.front());
  const bool exactBounded =
      removed &&
          !view.conversationModel()->indexForTarget(targets.front()).isValid() &&
          firstVisible(view) == anchorBeforeRemoval &&
          view.conversationModel()->property("modelResetCount").toULongLong() ==
              resetsBefore &&
          view.conversationModel()
                  ->property("modelExactInsertCount")
                  .toULongLong() == 1 &&
          view.conversationModel()
                  ->property("modelExactMoveCount")
                  .toULongLong() == 1 &&
          view.conversationModel()
                  ->property("modelExactRemoveCount")
                  .toULongLong() == 1 &&
          view.conversationModel()
                  ->property("modelIndexRebuildCount")
                  .toULongLong() == identityRebuildsBefore &&
          view.property("conversationSectionRangeRebuilds").toULongLong() ==
              sectionRebuildsBefore &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              heightRebuildsBefore &&
          view.property("targetedStructuralRepaints").toULongLong() ==
              structuralRepaintsBefore &&
          view.property("targetedStructuralOffscreenRepaintsAvoided")
                  .toULongLong() ==
              offscreenRepaintsBefore + 3 &&
          view.materializedCardCount() <= 48;
  result &= expect(
      exactBounded,
      "exact structural operations use narrow model signals and bounded "
      "widgets without rebuilding or repainting ten thousand retained rows");
  return result;
}

bool boundedTailAppendIsViewportProportional() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  bool result = expect(view.reconcile(conversation(10'000)),
                       "bounded-tail fixture reconciles");
  settle();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  settle();
  const auto anchorBefore = firstVisible(view);
  const int horizontalBefore = view.horizontalScrollBar()->value();
  const qulonglong modelRebuilds = view.conversationModel()
                                       ->property("modelIndexRebuildCount")
                                       .toULongLong();
  const qulonglong sectionRebuilds =
      view.property("conversationSectionRangeRebuilds").toULongLong();
  const qulonglong heightRebuilds =
      view.property("conversationHeightIndexRebuilds").toULongLong();
  const int widgetsBefore = view.materializedCardCount();

  ConversationTailCard tail;
  tail.card = message(10'000);
  tail.sectionKey = "section-10000";
  tail.historyActivity = true;
  tail.authoritativeItemCount = 10'001;
  const std::string tailKey = stableKey(tail.card.key);
  result &= expect(view.appendTailCard(std::move(tail)),
                   "canonical tail append was accepted");
  settle();
  const auto anchorAfter = firstVisible(view);
  result &= expect(
      view.conversationModel()->rowCount() == 10'001 &&
          view.conversationModel()->indexForStableKey(tailKey).row() == 10'000 &&
          view.conversationModel()->hiddenAuthoritativeItemCount() == 0,
      "paused tail append did not expand its retained window exactly once");
  result &= expect(anchorAfter == anchorBefore &&
                       view.horizontalScrollBar()->value() == horizontalBefore,
                   "paused tail append did not preserve both "
                   "viewport axes");
  result &= expect(
      view.conversationModel()
                  ->property("modelIndexRebuildCount")
                  .toULongLong() == modelRebuilds &&
          view.property("conversationSectionRangeRebuilds").toULongLong() ==
              sectionRebuilds &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              heightRebuilds &&
          view.materializedCardCount() <= std::max(48, widgetsBefore + 4),
      "one tail append traversed retained indexes or escaped the widget bound");

  ConversationView following;
  following.resize(820, 600);
  following.show();
  result &= expect(following.reconcile(conversation(80)),
                   "following-tail fixture reconciles");
  settle();
  ConversationTailCard followingTail;
  followingTail.card = message(80);
  followingTail.sectionKey = "section-80";
  followingTail.historyActivity = true;
  followingTail.authoritativeItemCount = 81;
  const std::string followingKey = stableKey(followingTail.card.key);
  result &= expect(following.appendTailCard(std::move(followingTail)),
                   "following tail append was accepted");
  settle();
  const QModelIndex finalIndex =
      following.conversationModel()->indexForStableKey(followingKey);
  result &= expect(finalIndex.isValid() && following.isAtBottom() &&
                       following.visualRect(finalIndex).bottom() <=
                           following.viewport()->height(),
                   "following append exposed one complete final card");

  ConversationSnapshot rooted;
  rooted.threadId = "virtual-thread";
  TurnSection rootedSection;
  rootedSection.key = "rooted-section";
  rootedSection.turnId = "rooted-turn";
  for (std::size_t serial = 0; serial < 80; ++serial) {
    VisibleCardData value;
    value.key = AuthoritativeItemKey{"virtual-thread", "rooted-turn",
                                     "rooted-" + std::to_string(serial)};
    value.kind = serial == 0 ? CardKind::UserMessage : CardKind::AgentMessage;
    value.threadId = "virtual-thread";
    value.turnId = "rooted-turn";
    value.itemId = "rooted-" + std::to_string(serial);
    value.payload = serial == 0
                        ? CardPayload{UserMessageData{"Root prompt", {}}}
                        : CardPayload{AgentMessageData{"Nested answer", true}};
    rootedSection.cards.push_back(std::move(value));
  }
  rootedSection.rootCardKey = rootedSection.cards.front().key;
  rooted.sections.push_back(std::move(rootedSection));
  ConversationView rootedView;
  rootedView.resize(820, 600);
  rootedView.show();
  result &= expect(rootedView.reconcile(std::move(rooted)),
                   "rooted bounded-tail fixture reconciles");
  settle();
  const qulonglong rootedRebuilds = rootedView.conversationModel()
                                        ->property("modelIndexRebuildCount")
                                        .toULongLong();
  const auto appendNested = [&rootedView](std::size_t serial) {
    ConversationTailCard nestedTail;
    nestedTail.card.key = AuthoritativeItemKey{
        "virtual-thread", "rooted-turn", "rooted-" + std::to_string(serial)};
    nestedTail.card.kind = CardKind::AgentMessage;
    nestedTail.card.threadId = "virtual-thread";
    nestedTail.card.turnId = "rooted-turn";
    nestedTail.card.itemId = "rooted-" + std::to_string(serial);
    nestedTail.card.payload = AgentMessageData{"Nested tail", true};
    nestedTail.sectionKey = "rooted-section";
    nestedTail.nested = true;
    nestedTail.historyActivity = true;
    nestedTail.authoritativeItemCount = serial + 1;
    return rootedView.appendTailCard(std::move(nestedTail));
  };
  result &= expect(appendNested(80) && appendNested(81),
                   "root-pinned nested tail appends were accepted");
  settle();
  result &= expect(
      rootedView.conversationModel()->rowCount() == 81 &&
          rootedView.conversationModel()
                  ->indexForStableKey(
                      "item:14:virtual-thread11:rooted-turn8:rooted-0")
                  .row() == 0 &&
          rootedView.conversationModel()
                  ->indexForStableKey(
                      "item:14:virtual-thread11:rooted-turn8:rooted-2")
                  .row() == 1 &&
          rootedView.conversationModel()
                  ->property("modelIndexRebuildCount")
                  .toULongLong() == rootedRebuilds &&
          rootedView.isAtBottom(),
      "pinned Turn root trim lost identity, rebuilt history, or stopped "
      "following");
  return result;
}

bool targetedVisibilityChangeIsLocal() {
  ConversationView view;
  view.resize(820, 600);
  view.setPresentationOptions(
      {.showReasoning = true, .showCodexUpdates = false});
  view.show();
  ConversationSnapshot snapshot = conversation(10'000);
  constexpr int TargetRow = 100;
  auto &initial = std::get<AgentMessageData>(
      snapshot.sections[TargetRow].cards.front().payload);
  initial.finalAnswer = false;
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "a long thread with one filtered update reconciles");
  settle();
  const QModelIndex index = view.conversationModel()->index(TargetRow);
  result &= expect(!index.data(ConversationItemModel::PresentedRole).toBool(),
                   "the non-final update begins filtered");

  VisibleCardData finalAnswer = *view.conversationModel()->card(TargetRow);
  std::get<AgentMessageData>(finalAnswer.payload).finalAnswer = true;
  const qulonglong sectionRebuilds =
      view.property("conversationSectionRangeRebuilds").toULongLong();
  const qulonglong indexRebuilds = view.conversationModel()
                                       ->property("modelIndexRebuildCount")
                                       .toULongLong();
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  const auto impact = view.applyCardPresentation(finalAnswer);
  settle();
  const bool localVisibilityPass =
      impact == PresentationImpact::GeometryChanged &&
      index.data(ConversationItemModel::PresentedRole).toBool() &&
      view.property("conversationSectionRangeRebuilds").toULongLong() ==
          sectionRebuilds &&
      view.conversationModel()
              ->property("modelIndexRebuildCount")
              .toULongLong() == indexRebuilds &&
      view.property("conversationCardConstructions").toULongLong() ==
          constructions &&
      view.property("conversationHeightIndexUpdateSteps").toULongLong() <= 15;
  result &= expect(
      localVisibilityPass,
      "final-answer visibility updates only its row and logarithmic height "
      "index");

  const qulonglong commits = view.property("targetedCardCommits").toULongLong();
  result &=
      expect(view.applyCardPresentation(std::move(finalAnswer)) ==
                     PresentationImpact::None &&
                 view.property("targetedCardCommits").toULongLong() == commits,
             "repeated identical final state performs zero presentation "
             "work");
  return result;
}

bool atomicPagingAndFollowingArrival() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  settle();
  ConversationSnapshot initial = conversation(80, 80);
  bool result = expect(view.reconcile(initial), "initial page is visible");
  settle();
  const int rowsBefore = view.conversationModel()->rowCount();
  const int widgetsBefore = view.materializedCardCount();
  const qulonglong resetsBefore =
      view.conversationModel()->property("modelResetCount").toULongLong();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  const auto anchorBeforePage = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !anchorBeforePage.first.empty(),
                   "Load 80 begins from an explicit paused viewport anchor");

  ConversationSnapshot loaded = conversation(160);
  loaded.hasMore = true;
  view.prependHistoryPageStaged(std::move(loaded));
  const bool deferred = view.structuralStagingActive();
  result &= expect(
      deferred ? view.conversationModel()->rowCount() == rowsBefore &&
                     view.materializedCardCount() == widgetsBefore
               : view.conversationModel()->rowCount() == 160 &&
                     view.materializedCardCount() <= widgetsBefore + 4,
      "Load 80 either retains the old frame while preparing visible rows or "
      "commits immediately when its new rows are wholly offscreen");
  QElapsedTimer deadline;
  deadline.start();
  while (view.structuralStagingActive() && deadline.elapsed() < 5000)
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  settle();
  result &= expect(!view.structuralStagingActive() &&
                       view.conversationModel()->rowCount() == 160 &&
                       view.conversationModel()
                               ->property("modelResetCount")
                               .toULongLong() == resetsBefore &&
                       view.materializedCardCount() <= 48,
                   "Load 80 commits one complete virtualized frame without a "
                   "model reset");
  result &= expect(firstVisible(view) == anchorBeforePage,
                   "Load 80 preserves the exact paused row and pixel offset");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  settle();

  ConversationSnapshot appended = conversation(161);
  view.reconcileStaged(std::move(appended));
  deadline.restart();
  while (view.structuralStagingActive() && deadline.elapsed() < 5000)
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  settle();
  const QModelIndex tail = view.conversationModel()->index(160);
  result &=
      expect(view.isAtBottom() && tail.isValid() &&
                 view.visualRect(tail).bottom() <= view.viewport()->height(),
             "following arrival reveals its complete final card");
  return result;
}

bool pinnedTurnPagingPreservesRetainedActivityAnchor() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  ConversationSnapshot initial = pinnedTurnPage(80, 80);
  initial.hasMore = true;
  bool result = expect(view.reconcile(initial),
                       "a bounded page retains its owning turn root");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  const QModelIndex retained =
      view.conversationModel()->indexForStableKey(stableKey(
          AuthoritativeItemKey{"virtual-thread", "shared-turn", "item-80"}));
  const int retainedTop = view.visualRect(retained).top();
  result &= expect(retained.isValid() && retainedTop > 0 &&
                       view.mode() == ConversationView::Mode::Paused,
                   "the first retained activity establishes a paused paging "
                   "anchor below its pinned owner");

  ConversationSnapshot loaded = pinnedTurnPage(1, 159);
  view.prependHistoryPageStaged(std::move(loaded));
  result &= expect(waitUntil([&] { return !view.structuralStagingActive(); },
                             5000),
                   "the pinned-root history page commits");
  settle();
  const QModelIndex retainedAfter =
      view.conversationModel()->indexForStableKey(stableKey(
          AuthoritativeItemKey{"virtual-thread", "shared-turn", "item-80"}));
  result &= expect(retainedAfter.isValid() &&
                       view.visualRect(retainedAfter).top() == retainedTop,
                   "paging anchors the first retained activity rather than "
                   "the root pinned outside the old history window");
  return result;
}

bool historyWindowLivesWithThePresentedThread() {
  ConversationView view;
  bool result = expect(view.historyLimitForThread("history-a", 200) == 80,
                       "a new thread begins with the canonical 80-row window");
  const auto retainedFirst =
      view.requestNextHistoryPage("history-a", 200, true);
  const auto retainedSecond =
      view.requestNextHistoryPage("history-a", 200, true);
  const auto provider = view.requestNextHistoryPage("history-a", 200, true);
  result &= expect(retainedFirst.effectiveLimit == 160 &&
                       !retainedFirst.requestProvider &&
                       retainedSecond.effectiveLimit == 240 &&
                       !retainedSecond.requestProvider &&
                       provider.effectiveLimit == 320 &&
                       provider.requestProvider,
                   "retained pages are consumed before one provider request");
  result &= expect(view.historyLimitForThread("history-b", 500) == 80,
                   "history windows remain independent per thread");
  view.forgetThreadPresentation("history-a");
  result &= expect(view.historyLimitForThread("history-a", 200) == 80,
                   "retiring a thread releases its presentation window");
  return result;
}

bool delayedThreadSelectionSpinner() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  const ConversationSnapshot source =
      singleMessageConversation("spinner-source", "Outgoing conversation");
  bool result = expect(view.reconcile(source),
                       "spinner source conversation reconciles");
  settle();
  std::vector<std::string> committedThreads;
  view.setPresentationCommittedAction(
      [&](const std::string &threadId) { committedThreads.push_back(threadId); });

  view.beginThreadSelection("spinner-slow-target");
  settle();
  auto *overlay = view.findChild<QWidget *>(
      QStringLiteral("conversationStagingOverlay"));
  result &= expect(
      overlay && overlay->isVisible() &&
          view.viewport()->childAt(view.viewport()->rect().center()) == overlay &&
          !overlay->property("spinnerVisible").toBool() &&
          overlay->property("spinnerDelayMilliseconds").toInt() == 500 &&
          overlay->property("spinnerDiameter").toInt() == 30 &&
          overlay->property("spinnerStrokeWidth").toInt() == 3 &&
          view.conversationModel()->indexForStableKey(
                  stableKey(AuthoritativeItemKey{"spinner-source", "turn",
                                                 "message"}))
              .isValid(),
      "thread selection immediately covers the outgoing message viewport "
      "with a blank centered loading surface");
  result &= expect(committedThreads.empty(),
                   "selection does not publish presentation readiness early");

  QElapsedTimer early;
  early.start();
  while (early.elapsed() < 350) {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  result &= expect(overlay && !overlay->property("spinnerVisible").toBool(),
                   "the first half-second of thread loading shows no spinner");
  result &= expect(
      overlay && waitUntil(
                     [overlay] {
                       return overlay->property("spinnerVisible").toBool();
                     },
                     350) &&
          overlay->property("spinnerAnimationActive").toBool(),
      "a slower thread load starts the gray spinner after its delay");
  QRect spinnerPixels;
  if (overlay) {
    const QImage frame = overlay->grab().toImage();
    const QColor background(QStringLiteral("#f6f8fb"));
    for (int y = 0; y < frame.height(); ++y)
      for (int x = 0; x < frame.width(); ++x)
        if (frame.pixelColor(x, y) != background)
          spinnerPixels |= QRect(x, y, 1, 1);
  }
  result &= expect(
      spinnerPixels.width() >= 29 && spinnerPixels.width() <= 31 &&
          spinnerPixels.height() >= 29 && spinnerPixels.height() <= 31 &&
          overlay &&
          (spinnerPixels.center() - overlay->rect().center()).manhattanLength() <=
              2,
      "the painted gray ring is 30 pixels and centered in the message view");
  const qulonglong tick =
      overlay ? overlay->property("spinnerAnimationTick").toULongLong() : 0;
  result &= expect(
      overlay && waitUntil(
                     [overlay, tick] {
                       return overlay->property("spinnerAnimationTick")
                                  .toULongLong() > tick;
                     },
                     150),
      "the visible spinner advances while loading");

  view.reconcileStaged(singleMessageConversation("spinner-slow-target",
                                                 "Incoming conversation"));
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 500) &&
          overlay && !overlay->isVisible() &&
          !overlay->property("spinnerVisible").toBool() &&
          !overlay->property("spinnerAnimationActive").toBool() &&
          view.conversationModel()
              ->indexForStableKey(
                  stableKey(AuthoritativeItemKey{
                      "spinner-slow-target", "turn", "message"}))
              .isValid(),
      "the complete target frame atomically removes and stops the spinner");
  result &= expect(committedThreads ==
                       std::vector<std::string>{"spinner-slow-target"},
                   "the complete target frame publishes readiness once");

  view.beginThreadSelection("spinner-fast-target");
  view.reconcileStaged(singleMessageConversation("spinner-fast-target",
                                                 "Fast conversation"));
  settle();
  result &= expect(
      overlay && !overlay->isVisible() &&
          !overlay->property("spinnerAnimationActive").toBool() &&
          committedThreads ==
              std::vector<std::string>{"spinner-slow-target",
                                       "spinner-fast-target"},
      "a fast staged selection clears and reveals without spinner motion");

  view.beginThreadSelection("spinner-stale-target");
  view.beginThreadSelection("spinner-final-target");
  const qulonglong ignoredBefore =
      view.property("staleThreadStagesIgnored").toULongLong();
  view.reconcileStaged(singleMessageConversation("spinner-stale-target",
                                                 "Stale conversation"));
  result &= expect(
      view.property("staleThreadStagesIgnored").toULongLong() ==
              ignoredBefore + 1 &&
          overlay && overlay->isVisible() && committedThreads.size() == 2,
      "a superseded thread stage cannot reveal or stop the current load");
  view.reconcileStaged(singleMessageConversation("spinner-final-target",
                                                 "Final conversation"));
  settle();
  result &= expect(
      overlay && !overlay->isVisible() &&
          view.conversationModel()
              ->indexForStableKey(
                  stableKey(AuthoritativeItemKey{
                      "spinner-final-target", "turn", "message"}))
              .isValid() &&
          committedThreads ==
              std::vector<std::string>{"spinner-slow-target",
                                       "spinner-fast-target",
                                       "spinner-final-target"},
      "the newest thread identity alone completes the loading surface");
  return result;
}

bool virtualTurnSurfaceAndInteractivePromotion() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "turn-surface";
  VisibleCardData root{AuthoritativeItemKey{"turn-surface", "turn", "root"},
                       CardKind::UserMessage,
                       "turn-surface",
                       "turn",
                       "root",
                       UserMessageData{"Question", {}}};
  VisibleCardData nested{AuthoritativeItemKey{"turn-surface", "turn", "answer"},
                         CardKind::AgentMessage,
                         "turn-surface",
                         "turn",
                         "answer",
                         AgentMessageData{"Answer", true}};
  snapshot.sections.push_back(
      {"turn-section", "turn", {root, nested}, root.key});

  ConversationView view;
  view.resize(820, 600);
  view.show();
  bool result =
      expect(view.reconcile(snapshot), "virtual Turn/You fixture reconciles");
  settle();
  const QModelIndex rootIndex = view.conversationModel()->index(0);
  const QModelIndex nestedIndex = view.conversationModel()->index(1);
  const QRect rootRect = view.visualRect(rootIndex);
  const QRect nestedRect = view.visualRect(nestedIndex);
  result &= expect(rootRect.left() == 0 && nestedRect.left() == 12 &&
                       nestedRect.width() == rootRect.width() - 24 &&
                       nestedRect.top() > rootRect.bottom(),
                   "flat rows retain the established nested turn geometry");
  const QImage painted = view.viewport()->grab().toImage();
  const int sampleY =
      std::clamp(rootRect.bottom() + 3, 0, std::max(0, painted.height() - 1));
  const QColor turnSurface = painted.pixelColor(4, sampleY);
  result &= expect(turnSurface.blue() > turnSurface.red(),
                   "the view paints the continuous blue You turn enclosure");

  const QPoint hover = rootRect.center();
  QMouseEvent move(QEvent::MouseMove, QPointF(hover), QPointF(hover),
                   view.viewport()->mapToGlobal(hover), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &move);
  settle();
  sendViewportMouse(view, QEvent::MouseButtonPress, hover, Qt::LeftButton,
                    Qt::LeftButton);
  sendViewportMouse(view, QEvent::MouseButtonRelease, hover, Qt::LeftButton,
                    Qt::NoButton);
  settle();
  ConversationCard *promoted = materializedCard(view, stableKey(root.key));
  result &= expect(promoted && promoted->property("virtualTurnRoot").toBool() &&
                       promoted->parentWidget() == view.viewport(),
                   "press promotes only the interactive root fragment to a "
                   "real viewport editor");
  result &= expect(view.materializedCardCount() == 1,
                   "interactive promotion remains row-local and bounded");
  return result;
}

bool directTailGrowsTheRetainedTurnSurface() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "tail-growth";
  snapshot.activeTurnId = "active-turn";
  VisibleCardData root{
      LocalPromptKey{771}, CardKind::LocalPrompt, "tail-growth", "active-turn",
      {}, LocalPromptData{771, "Pending question", PromptState::InFlight}};
  TurnSection section;
  section.key = "active-section";
  section.turnId = "active-turn";
  section.cards.push_back(root);
  section.rootCardKey = root.key;
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(820, 360);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "single optimistic Turn fixture reconciles");
  settle();
  ConversationCard *retained = materializedCard(view, stableKey(root.key));
  result &= expect(retained &&
                       retained->property("authoritativeTurnActive").toBool() &&
                       !retained->property("virtualTurnRoot").toBool(),
                   "the optimistic Turn owns its emphasized border on its "
                   "first complete frame");

  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  const qulonglong sectionRebuilds =
      view.property("conversationSectionRangeRebuilds").toULongLong();
  ConversationTailCard tail;
  tail.card = {AuthoritativeItemKey{"tail-growth", "active-turn", "answer"},
               CardKind::AgentMessage,
               "tail-growth",
               "active-turn",
               "answer",
               AgentMessageData{"Final answer", true}};
  tail.sectionKey = "active-section";
  tail.nested = true;
  tail.activeTurn = true;
  tail.historyActivity = true;
  tail.authoritativeItemCount = 2;
  const std::string answerKey = stableKey(tail.card.key);
  result &= expect(view.appendTailCard(std::move(tail)),
                   "the first nested direct-tail card appends");
  settle();

  const QModelIndex rootIndex =
      view.conversationModel()->indexForStableKey(stableKey(root.key));
  const QModelIndex answerIndex =
      view.conversationModel()->indexForStableKey(answerKey);
  const QRect answerRect = view.visualRect(answerIndex);
  const QImage frame = view.viewport()->grab().toImage();
  const QColor grownBorder = frame.pixelColor(
      1, std::clamp(answerRect.center().y(), 0, frame.height() - 1));
  const bool grew =
      retained && retained == materializedCard(view, stableKey(root.key)) &&
          retained->property("virtualTurnRoot").toBool() &&
          !retained->property("authoritativeTurnActive").toBool() &&
          rootIndex.data(ConversationItemModel::ActiveTurnRole).toBool() &&
          answerIndex.isValid() && answerRect.left() == 12 &&
          grownBorder.blue() > grownBorder.red() && grownBorder.red() < 183 &&
          view.property("conversationCardConstructions").toULongLong() ==
              constructions &&
          view.property("conversationSectionRangeRebuilds").toULongLong() ==
              sectionRebuilds;
  result &= expect(
      grew,
      "direct-tail growth retains the root editor and exposes one continuous "
      "emphasized Turn border without rebuilding section indexes");
  return result;
}

bool acknowledgedSteeringMovesAboveFollowingActivity() {
  ConversationSnapshot snapshot = conversation(12);
  TurnSection active;
  active.key = "steering-section";
  active.turnId = "steering-turn";
  VisibleCardData root{
      AuthoritativeItemKey{"virtual-thread", "steering-turn", "root"},
      CardKind::UserMessage,
      "virtual-thread",
      "steering-turn",
      "root",
      UserMessageData{"Opening prompt"}};
  active.rootCardKey = root.key;
  active.cards.push_back(std::move(root));
  VisibleCardData steering{LocalPromptKey{812},
                           CardKind::UserMessage,
                           "virtual-thread",
                           "steering-turn",
                           "provider-steering",
                           UserMessageData{"Acknowledged steering"}};
  const std::string steeringKey = stableKey(steering.key);
  active.cards.push_back(std::move(steering));
  snapshot.activeTurnId = active.turnId;
  snapshot.sections.push_back(std::move(active));

  ConversationView view;
  view.resize(820, 360);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "acknowledged steering fixture reconciles");
  settle();
  const QModelIndex steeringIndex =
      view.conversationModel()->indexForStableKey(steeringKey);
  const QRect before = view.visualRect(steeringIndex);
  const VisibleCardData *settledSteering =
      view.conversationModel()->card(steeringIndex.row());
  result &= expect(steeringIndex.isValid() && settledSteering &&
                       settledSteering->kind == CardKind::UserMessage &&
                       view.mode() == ConversationView::Mode::Following &&
                       before.bottom() <= view.viewport()->height(),
                   "the settled steering card begins at the followed tail");

  ConversationTailCard activity;
  activity.card = {
      AuthoritativeItemKey{"virtual-thread", "steering-turn", "after-steer"},
      CardKind::AgentMessage,
      "virtual-thread",
      "steering-turn",
      "after-steer",
      AgentMessageData{"Activity caused by the steering prompt", false}};
  activity.sectionKey = "steering-section";
  activity.nested = true;
  activity.activeTurn = true;
  activity.historyActivity = true;
  const std::string activityKey = stableKey(activity.card.key);
  result &= expect(view.appendTailCard(std::move(activity)),
                   "post-steering activity appends through the bounded path");
  settle();

  const QRect after = view.visualRect(steeringIndex);
  const QModelIndex activityIndex =
      view.conversationModel()->indexForStableKey(activityKey);
  result &= expect(
      view.conversationModel()->indexForStableKey(steeringKey) ==
              steeringIndex &&
          after.top() < before.top() && activityIndex.isValid() &&
          after.bottom() < view.visualRect(activityIndex).top() &&
          view.visualRect(activityIndex).bottom() <= view.viewport()->height() &&
          view.mode() == ConversationView::Mode::Following,
      "an acknowledged steering card moves upward when following activity "
      "arrives instead of remaining pinned to the viewport bottom");
  return result;
}

bool selectionFocusAndOneGesturePromotion() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  ConversationSnapshot snapshot = conversation(200);
  bool result =
      expect(view.reconcile(snapshot), "interaction-state fixture reconciles");
  settle();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
  const auto identity = firstVisible(view);
  const QModelIndex index =
      view.conversationModel()->indexForStableKey(identity.first);
  const QPoint hover =
      view.visualRect(index).intersected(view.viewport()->rect()).center();
  QMouseEvent move(QEvent::MouseMove, QPointF(hover), QPointF(hover),
                   view.viewport()->mapToGlobal(hover), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &move);
  settle();
  result &= expect(materializedCard(view, identity.first) == nullptr,
                   "passive Markdown hover constructs no editor");
  sendViewportMouse(view, QEvent::MouseButtonPress, hover, Qt::LeftButton,
                    Qt::LeftButton);
  sendViewportMouse(view, QEvent::MouseButtonRelease, hover, Qt::LeftButton,
                    Qt::NoButton);
  settle();
  ConversationCard *card = materializedCard(view, identity.first);
  MarkdownTextView *body = card ? card->findChild<MarkdownTextView *>() : nullptr;
  result &= expect(card && body,
                   "one press promotes selectable Markdown to its real card");
  if (!body)
    return false;
  body->setSelection(0, 6);
  const QString selected = body->selectedText();

  VisibleCardData streamed = *view.conversationModel()->card(index.row());
  std::get<AgentMessageData>(streamed.payload).text += " streamed suffix";
  result &= expect(view.applyCardPresentation(std::move(streamed)).has_value(),
                   "streaming targets the promoted row");
  settle();
  result &= expect(body->selectedText() == selected,
                   "Markdown selection survives in-place streaming");
  body->setFocus(Qt::OtherFocusReason);
  QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
  QApplication::sendEvent(body, &copy);
  result &= expect(QApplication::clipboard()->text() == selected,
                   "the promoted Markdown keeps native selection copying");

  body->clearFocus();
  if (QWidget *focused = QApplication::focusWidget())
    focused->clearFocus();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  result &= expect(materializedCard(view, identity.first) == nullptr,
                   "an unfocused editor is released outside bounded overscan");
  view.scrollTo(index, QAbstractItemView::PositionAtTop);
  settle();
  const QPoint restoredHover = view.visualRect(index).center();
  sendViewportMouse(view, QEvent::MouseButtonPress, restoredHover,
                    Qt::LeftButton, Qt::LeftButton);
  sendViewportMouse(view, QEvent::MouseButtonRelease, restoredHover,
                    Qt::LeftButton, Qt::NoButton);
  settle();
  card = materializedCard(view, identity.first);
  body = card ? card->findChild<MarkdownTextView *>() : nullptr;
  result &=
      expect(body && body->selectedText() == selected,
             "selection is restored after virtualized release and return");
  result &= expect(
      index.data(Qt::AccessibleTextRole).toString().contains("streamed suffix"),
      "the passive model exposes current card content to accessibility");

  ConversationSnapshot foldedSnapshot;
  foldedSnapshot.threadId = "folded-promotion";
  VisibleCardData reasoning{
      AuthoritativeItemKey{"folded-promotion", "turn", "reasoning"},
      CardKind::Reasoning,
      "folded-promotion",
      "turn",
      "reasoning",
      ReasoningData{"Expanded by the same pointer gesture"}};
  foldedSnapshot.sections.push_back(
      {"folded-section", "turn", {reasoning}, std::nullopt});
  ConversationView foldedView;
  foldedView.resize(620, 320);
  foldedView.show();
  result &= expect(foldedView.reconcile(foldedSnapshot),
                   "folded delegate fixture reconciles");
  settle();
  const QRect foldedRect =
      foldedView.visualRect(foldedView.conversationModel()->index(0));
  const QPoint disclosurePoint(foldedRect.right() - 16, foldedRect.top() + 22);
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(disclosurePoint),
                    QPointF(disclosurePoint),
                    foldedView.viewport()->mapToGlobal(disclosurePoint),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(foldedView.viewport(), &press);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(disclosurePoint),
                      QPointF(disclosurePoint),
                      foldedView.viewport()->mapToGlobal(disclosurePoint),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(foldedView.viewport(), &release);
  settle();
  ConversationCard *expanded =
      materializedCard(foldedView, stableKey(reasoning.key));
  result &= expect(expanded && !expanded->isCollapsed(),
                   "a press-triggered promotion preserves one-gesture "
                   "disclosure activation");
  foldedView.activateWindow();
  settle();
  foldedView.setFocus(Qt::TabFocusReason);
  foldedView.setCurrentIndex(foldedView.conversationModel()->index(0));
  result &=
      expect(foldedView.hasFocus() && foldedView.currentIndex().row() == 0,
             "keyboard current-row focus remains visibly owned by the "
             "item view");
  return result;
}

bool passiveAndInteractivePresentationShareExactGeometry() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "geometry-invariant";
  TurnSection section;
  section.key = "geometry-section";
  section.turnId = "turn";
  for (int row = 0; row < 50; ++row) {
    std::string markdown;
    const int paragraphs = row == 25 ? 22 : 3 + row % 5;
    for (int paragraph = 0; paragraph < paragraphs; ++paragraph)
      markdown += std::string(100, 'W') + "\n\n";
    section.cards.push_back(
        {AuthoritativeItemKey{"geometry-invariant", "turn",
                              "update-" + std::to_string(row)},
         CardKind::AgentMessage,
         "geometry-invariant",
         "turn",
         "update-" + std::to_string(row),
         AgentMessageData{std::move(markdown), false}});
  }
  const VisibleCardData update = section.cards[25];
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(620, 360);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "geometry-invariant fixture reconciles");
  settle();
  const QModelIndex index = view.conversationModel()->index(25);
  view.scrollTo(index, QAbstractItemView::PositionAtCenter);
  settle();
  result &= expect(materializedCard(view, stableKey(update.key)) == nullptr,
                   "geometry-invariant row begins in passive presentation");
  const int passiveHeight = view.visualRect(index).height();
  view.setCurrentIndex(index);
  settle();
  const int interactiveHeight = view.visualRect(index).height();
  if (interactiveHeight != passiveHeight)
    std::cerr << "geometry mismatch: passive=" << passiveHeight
              << " interactive=" << interactiveHeight << '\n';
  result &= expect(materializedCard(view, stableKey(update.key)) &&
                       interactiveHeight == passiveHeight,
                   "passive Markdown and its interactive widget have exact "
                   "row-height parity");
  return result;
}

bool bidirectionalLazyMeasurementPreservesNativeScrollMotion() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "lazy-scroll-anchor";
  TurnSection section;
  section.key = "lazy-scroll-section";
  section.turnId = "turn";
  for (int row = 0; row < 240; ++row) {
    std::string markdown;
    const int paragraphs = 1 + row % 9;
    for (int paragraph = 0; paragraph < paragraphs; ++paragraph)
      markdown += std::string(90 + row % 37, 'W') + "\n\n";
    section.cards.push_back(
        {AuthoritativeItemKey{"lazy-scroll-anchor", "turn",
                              "update-" + std::to_string(row)},
         CardKind::AgentMessage,
         "lazy-scroll-anchor",
         "turn",
         "update-" + std::to_string(row),
         AgentMessageData{std::move(markdown), false}});
  }
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(620, 360);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "lazy-scroll anchor fixture reconciles");
  settle();
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  settle();

  const auto verifySteps = [&](QAbstractSlider::SliderAction action,
                               int expectedDelta) {
    for (int step = 0; step < 80; ++step) {
      const auto before = firstVisible(view);
      if (before.first.empty())
        return false;
      const QModelIndex retained =
          view.conversationModel()->indexForStableKey(before.first);
      const int valueBefore = view.verticalScrollBar()->value();
      view.verticalScrollBar()->triggerAction(action);
      settle(1);
      if (view.verticalScrollBar()->value() == valueBefore)
        return true;
      if (!retained.isValid() ||
          view.visualRect(retained).top() - before.second != expectedDelta)
        return false;
    }
    return true;
  };
  result &= expect(verifySteps(QAbstractSlider::SliderSingleStepSub,
                               view.verticalScrollBar()->singleStep()),
                   "upward scrolling preserves exact motion while rows above "
                   "become measured");
  result &= expect(verifySteps(QAbstractSlider::SliderSingleStepAdd,
                               -view.verticalScrollBar()->singleStep()),
                   "downward scrolling preserves exact motion while rows "
                   "below become measured");
  const qulonglong passesBefore =
      view.property("conversationMaterializationPasses").toULongLong();
  const QPointF local = view.viewport()->rect().center();
  QWheelEvent wheel(local, view.viewport()->mapToGlobal(local.toPoint()), {},
                    QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false);
  result &= expect(view.forwardWheelEvent(&wheel),
                   "conversation accepts one native wheel gesture");
  settle(1);
  result &= expect(
      view.property("conversationMaterializationPasses").toULongLong() ==
          passesBefore + 1,
      "one wheel update performs exactly one materialization pass");
  return result;
}

bool largeIncomingCommandUsesBoundedFinalWidthLayout() {
  const std::string thread = "bounded-command";
  VisibleCardData root{
      AuthoritativeItemKey{thread, "turn", "root"},
      CardKind::UserMessage,
      thread,
      "turn",
      "root",
      UserMessageData{"Run the command"}};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back(
      {"command-section", "turn", {root}, root.key});

  ConversationView view;
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(760, 480);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "bounded-command fixture reconciles");
  settle();

  std::string output;
  output.reserve(192 * 1024);
  while (output.size() < 192 * 1024)
    output += "0123456789abcdef command output line for bounded layout\n";
  output.resize(192 * 1024);
  ConversationTailCard tail;
  tail.card = {AuthoritativeItemKey{thread, "turn", "command"},
               CardKind::CommandExecution,
               thread,
               "turn",
               "command",
               CommandExecutionData{"printf diagnostic", std::move(output),
                                    "inProgress", "/workspace", {}, {}},
               true};
  const std::string key = stableKey(tail.card.key);
  tail.sectionKey = "command-section";
  tail.nested = true;
  tail.activeTurn = true;
  tail.historyActivity = true;
  tail.authoritativeItemCount = 2;

  QElapsedTimer timer;
  timer.start();
  result &= expect(view.appendTailCard(std::move(tail)),
                   "large command appends through the direct-tail path");
  const qint64 appendMicros = timer.nsecsElapsed() / 1000;
  settle();
  ConversationCard *commandCard = materializedCard(view, key);
  CommandOutputView *commandOutput = commandCard
                                         ? dynamic_cast<CommandOutputView *>(
                                               commandCard->findChild<QPlainTextEdit *>(
                                                   QStringLiteral(
                                                       "commandOutputView")))
                                         : nullptr;
  result &= expect(
      commandOutput && commandOutput->viewport()->width() > 500 &&
          commandOutput->property("boundedOutputMeasurements").toULongLong() >=
              1 &&
          commandOutput->property("fullOutputMeasurements").toULongLong() == 0 &&
          commandOutput->document()->characterCount() > 190 * 1024,
      "large command output is retained but bypasses whole-document geometry "
      "at its final row width");
  view.setProperty("largeCommandAppendMicros", appendMicros);
  return result;
}

bool streamingMarkdownReparsesOnlyMutableTail() {
  std::string markdown;
  markdown.reserve(192 * 1024);
  for (int paragraph = 0; paragraph < 2400; ++paragraph) {
    markdown += "Stable paragraph ";
    markdown += std::to_string(paragraph);
    markdown += " remains unchanged while the visible tail streams.\n\n";
  }
  markdown += "Mutable **tail";

  VisibleCardData update{
      AuthoritativeItemKey{"markdown-tail", "turn", "update"},
      CardKind::AgentMessage,
      "markdown-tail",
      "turn",
      "update",
      AgentMessageData{std::move(markdown), false}};
  ConversationSnapshot snapshot;
  snapshot.threadId = update.threadId;
  VisibleCardData sentinel{
      AuthoritativeItemKey{"markdown-tail", "sentinel-turn", "sentinel"},
      CardKind::UserMessage,
      "markdown-tail",
      "sentinel-turn",
      "sentinel",
      UserMessageData{"Keep the keyboard current row separate."}};
  snapshot.sections.push_back(
      {"sentinel-section", sentinel.turnId, {sentinel}, std::nullopt});
  snapshot.sections.push_back(
      {"markdown-section", update.turnId, {update}, std::nullopt});

  ConversationView view;
  view.resize(760, 480);
  view.show();
  bool result =
      expect(view.reconcile(std::move(snapshot)),
             "large Markdown tail fixture reconciles through the delegate");
  settle();
  view.setCurrentIndex(view.conversationModel()->index(0));
  settle();
  const qulonglong rebuildsBefore =
      view.property("conversationDelegateDocumentRebuilds").toULongLong();
  const qulonglong appendsBefore =
      view.property("conversationDelegateIncrementalAppends").toULongLong();

  auto &message = std::get<AgentMessageData>(update.payload);
  message.text += "** with a [link](https://example.com).\n\n"
                  "The final paragraph is complete.";
  QElapsedTimer timer;
  timer.start();
  result &= expect(view.applyCardPresentation(update).has_value(),
                   "the visible Markdown row accepts its streamed suffix");
  const qint64 updateMicros = timer.nsecsElapsed() / 1000;
  settle();
  const QModelIndex index =
      view.conversationModel()->indexForStableKey(stableKey(update.key));
  const int passiveHeight = view.visualRect(index).height();
  result &= expect(
      view.property("conversationDelegateDocumentRebuilds").toULongLong() ==
              rebuildsBefore &&
          view.property("conversationDelegateIncrementalAppends")
                  .toULongLong() == appendsBefore + 1,
      "streaming reparses only the mutable Markdown tail instead of rebuilding "
      "the unchanged document");
  view.setProperty("largeMarkdownTailUpdateMicros", updateMicros);

  const QPoint hover =
      view.visualRect(index).intersected(view.viewport()->rect()).center();
  QMouseEvent move(QEvent::MouseMove, QPointF(hover), QPointF(hover),
                   view.viewport()->mapToGlobal(hover), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &move);
  settle();
  result &= expect(materializedCard(view, stableKey(update.key)) == nullptr,
                   "hovering a very large update performs no QWidget work");
  const qulonglong transfersBefore =
      view.property("conversationDelegateDocumentTransfers").toULongLong();
  QElapsedTimer promotionTimer;
  promotionTimer.start();
  sendViewportMouse(view, QEvent::MouseButtonPress, hover, Qt::LeftButton,
                    Qt::LeftButton);
  sendViewportMouse(view, QEvent::MouseButtonRelease, hover, Qt::LeftButton,
                    Qt::NoButton);
  const qint64 promotionMicros = promotionTimer.nsecsElapsed() / 1000;
  settle();
  ConversationCard *card = materializedCard(view, stableKey(update.key));
  MarkdownTextView *body = card ? card->findChild<MarkdownTextView *>() : nullptr;
  view.setProperty("largeMarkdownPromotionMicros", promotionMicros);
  result &= expect(
      card && body &&
          view.property("conversationDelegateDocumentTransfers")
                  .toULongLong() == transfersBefore + 1 &&
          body->property("markdownSource").toString() ==
              QString::fromStdString(message.text) &&
          body->toHtml().contains(QStringLiteral("https://example.com")) &&
          card->height() == passiveHeight,
      "interaction promotion preserves the complete streamed Markdown, link, "
      "and delegate geometry");
  return result;
}

bool passiveMarkdownHoverKeepsLinkSemanticsWithoutAnEditor() {
  VisibleCardData sentinel{
      AuthoritativeItemKey{"passive-link", "sentinel", "sentinel"},
      CardKind::UserMessage,
      "passive-link",
      "sentinel",
      "sentinel",
      UserMessageData{"Keep current focus separate."}};
  VisibleCardData linked{
      AuthoritativeItemKey{"passive-link", "turn", "linked"},
      CardKind::AgentMessage,
      "passive-link",
      "turn",
      "linked",
      AgentMessageData{"[Docs](https://example.com)", false}};
  ConversationSnapshot snapshot;
  snapshot.threadId = "passive-link";
  snapshot.sections.push_back(
      {"sentinel-section", sentinel.turnId, {sentinel}, std::nullopt});
  snapshot.sections.push_back(
      {"linked-section", linked.turnId, {linked}, std::nullopt});

  ConversationView view;
  view.resize(620, 320);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "passive link fixture reconciles");
  settle();
  view.setCurrentIndex(view.conversationModel()->index(0));
  settle();
  const QModelIndex index =
      view.conversationModel()->indexForStableKey(stableKey(linked.key));
  const QRect row = view.visualRect(index);
  const QPoint anchor(row.left() + 16, row.top() + 46);
  QMouseEvent move(QEvent::MouseMove, QPointF(anchor), QPointF(anchor),
                   view.viewport()->mapToGlobal(anchor), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &move);
  settle();
  result &= expect(
      !materializedCard(view, stableKey(linked.key)) &&
          view.viewport()->cursor().shape() == Qt::PointingHandCursor,
      "a passive Markdown link keeps its pointing cursor without constructing "
      "an editor");

  QHelpEvent tooltip(QEvent::ToolTip, anchor,
                     view.viewport()->mapToGlobal(anchor));
  QApplication::sendEvent(view.viewport(), &tooltip);
  result &= expect(
      QToolTip::text() == QStringLiteral("https://example.com"),
      "a passive Markdown link exposes its established URL tooltip");
  QToolTip::hideText();
  return result;
}

bool outsideTextDragDoesNotReenterTheView() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "virtual-thread";
  VisibleCardData update = message(0, "Selectable update text");
  std::get<AgentMessageData>(update.payload).finalAnswer = false;
  TurnSection section;
  section.key = "update-section";
  section.turnId = update.turnId;
  section.cards.push_back(update);
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(820, 320);
  view.show();
  bool result = expect(view.reconcile(std::move(snapshot)),
                       "pointer-drag update fixture reconciles");
  settle();
  const QModelIndex index = view.conversationModel()->index(0);
  const QRect row = view.visualRect(index);

  // A naturally delivered press on card padding is ignored by the card and
  // propagates to the item view. The view's one-gesture forwarding must not
  // send that same press recursively back through the parent chain.
  const QPoint padding(row.left() + 2, row.center().y());
  sendViewportMouse(view, QEvent::MouseButtonPress, padding, Qt::LeftButton,
                    Qt::LeftButton);
  const QPoint paddingDrag(row.left() + 4, row.center().y() + 2);
  sendViewportMouse(view, QEvent::MouseMove, paddingDrag, Qt::NoButton,
                    Qt::LeftButton);
  sendViewportMouse(view, QEvent::MouseButtonRelease, paddingDrag,
                    Qt::LeftButton, Qt::NoButton);
  settle();

  ConversationCard *card = materializedCard(view, stableKey(update.key));
  MarkdownTextView *body = card ? card->findChild<MarkdownTextView *>() : nullptr;
  result &= expect(card && body && view.currentIndex() == index,
                   "padding press remains a bounded row interaction");
  if (!body)
    return false;

  // Begin in the label's blank area after the glyphs and drag back through
  // the text. This is the real press/move/release path, not setSelection().
  const QPoint start = body->mapTo(
      view.viewport(), QPoint(std::max(1, body->width() - 2),
                              std::max(1, body->fontMetrics().height() / 2)));
  const QPoint finish =
      body->mapTo(view.viewport(),
                  QPoint(1, std::max(1, body->fontMetrics().height() / 2)));
  sendViewportMouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton,
                    Qt::LeftButton);
  sendViewportMouse(view, QEvent::MouseMove, finish, Qt::NoButton,
                    Qt::LeftButton);
  sendViewportMouse(view, QEvent::MouseButtonRelease, finish, Qt::LeftButton,
                    Qt::NoButton);
  settle();
  result &= expect(body->hasSelectedText(),
                   "dragging from outside update glyphs selects text");
  return result;
}

bool collapsedLargeCardsSkipBodyProjection() {
  FileChangesData changes;
  changes.status = "completed";
  changes.cwd = "/workspace";
  changes.changes.reserve(5'000);
  for (int index = 0; index < 5'000; ++index) {
    changes.changes.push_back({"src/generated/file-" +
                                   std::to_string(index) + ".cpp",
                               "update", index % 9, index % 4});
  }
  VisibleCardData card{
      AuthoritativeItemKey{"virtual-thread", "large-files", "changes"},
      CardKind::FileChanges,
      "virtual-thread",
      "large-files",
      "changes",
      std::move(changes)};
  ConversationSnapshot snapshot;
  snapshot.threadId = "virtual-thread";
  snapshot.sections.push_back(
      {"large-file-section", "large-files", {card}, std::nullopt});

  ConversationView view;
  view.resize(820, 320);
  view.show();
  bool result =
      expect(view.reconcile(std::move(snapshot)),
             "large collapsed file-change fixture reconciles");
  settle();
  static_cast<void>(view.viewport()->grab());
  settle();
  const QModelIndex index = view.conversationModel()->index(0);
  ConversationCard *richCard = materializedCard(view, stableKey(card.key));
  const bool bodySkipped =
      index.isValid() && view.visualRect(index).height() == 46 &&
      view.materializedCardCount() <= 1 && richCard &&
      richCard->property("fileChangesBodyRebuilds").toULongLong() == 0 &&
      view.property("conversationDelegateDocumentRebuilds").toULongLong() ==
          0;
  if (!bodySkipped)
    std::cerr << "collapsed large card: valid=" << index.isValid()
              << " height=" << view.visualRect(index).height()
              << " materialized=" << view.materializedCardCount()
              << " bodyRebuilds="
              << (richCard ? richCard->property("fileChangesBodyRebuilds")
                                     .toULongLong()
                           : std::numeric_limits<qulonglong>::max())
              << " documents="
              << view.property("conversationDelegateDocumentRebuilds")
                     .toULongLong()
              << '\n';
  result &= expect(
      bodySkipped,
      "collapsed large cards paint only their header without converting or "
      "laying out their body");
  QElapsedTimer expansionTimer;
  expansionTimer.start();
  richCard->setCollapsed(false);
  const qint64 expansionMicros = expansionTimer.nsecsElapsed() / 1000;
  auto *fileList = richCard->findChild<QPlainTextEdit *>(
      QStringLiteral("fileChangesList"));
  const bool boundedExpansion =
      fileList && fileList->blockCount() == 5'000 &&
      richCard->property("fileChangesBodyRebuilds").toULongLong() == 1 &&
      expansionMicros < 100'000;
  if (!boundedExpansion)
    std::cerr << "large file-change expansion us=" << expansionMicros
              << " internal="
              << richCard->property("fileChangesBodyBuildMicros").toLongLong()
              << " text="
              << (fileList ? fileList->property("fileChangesSetTextMicros")
                                 .toLongLong()
                           : -1)
              << " links="
              << (fileList
                      ? fileList->property("fileChangesFormatLinksMicros")
                            .toLongLong()
                      : -1)
              << " measure="
              << (fileList ? fileList->property("fileChangesMeasureMicros")
                                 .toLongLong()
                           : -1)
              << '\n';
  result &= expect(
      boundedExpansion,
      "expanding a large file-change card creates one block-oriented document "
      "without one widget per path");
  return result;
}

bool collapsedInteractionDefersEveryHeavyCardBody() {
  const std::string large(20'000, 'x');
  const std::string marker = "latest-deferred-body";
  std::vector<VisibleCardData> cards{
      {AuthoritativeItemKey{"deferred", "turn", "user"},
       CardKind::UserMessage, "deferred", "turn", "user",
       UserMessageData{large, {}}},
      {AuthoritativeItemKey{"deferred", "turn", "agent"},
       CardKind::AgentMessage, "deferred", "turn", "agent",
       AgentMessageData{large, false}},
      {AuthoritativeItemKey{"deferred", "turn", "command"},
       CardKind::CommandExecution, "deferred", "turn", "command",
       CommandExecutionData{large, large, "inProgress", "/workspace", {}}},
      {AuthoritativeItemKey{"deferred", "turn", "activity"},
       CardKind::AgentActivity, "deferred", "turn", "activity",
       AgentActivityData{"spawn_agent", "inProgress", {}, large, large}},
      {AuthoritativeItemKey{"deferred", "turn", "reasoning"},
       CardKind::Reasoning, "deferred", "turn", "reasoning",
       ReasoningData{large}},
      {TurnPlanKey{"deferred", "turn"}, CardKind::Plan, "deferred", "turn",
       {}, PlanData{large, {{large, "inProgress"}}, {}}},
      {AuthoritativeItemKey{"deferred", "turn", "image"},
       CardKind::ImageGeneration, "deferred", "turn", "image",
       ImageGenerationData{{}, "inProgress", large}},
      {AuthoritativeItemKey{"deferred", "turn", "generic"},
       CardKind::GenericActivity, "deferred", "turn", "generic",
       GenericActivityData{"customActivity", "inProgress", large}},
      {LocalPromptKey{912}, CardKind::LocalPrompt, "deferred", "turn", {},
       LocalPromptData{912, large, PromptState::InFlight, 0, {}, {}}}};

  auto appendMarker = [&marker](VisibleCardData &card) {
    std::visit(
        [&marker](auto &payload) {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, UserMessageData>)
            payload.text += marker;
          else if constexpr (std::is_same_v<Payload, AgentMessageData>)
            payload.text += marker;
          else if constexpr (std::is_same_v<Payload, CommandExecutionData>)
            payload.output += marker;
          else if constexpr (std::is_same_v<Payload, AgentActivityData>)
            payload.resultText += marker;
          else if constexpr (std::is_same_v<Payload, ReasoningData>)
            payload.summary += marker;
          else if constexpr (std::is_same_v<Payload, PlanData>)
            payload.explanation += marker;
          else if constexpr (std::is_same_v<Payload, ImageGenerationData>)
            payload.revisedPrompt += marker;
          else if constexpr (std::is_same_v<Payload, GenericActivityData>)
            payload.displayDetail = marker + payload.displayDetail;
          else if constexpr (std::is_same_v<Payload, LocalPromptData>)
            payload.prompt += marker;
        },
        card.payload);
  };
  auto bodyContains = [&marker](ConversationCard &card) {
    const auto markdown = card.findChildren<MarkdownTextView *>();
    if (std::ranges::any_of(markdown, [&marker](MarkdownTextView *view) {
          return view->markdownSource().contains(QString::fromStdString(marker));
        }))
      return true;
    const auto textEdits = card.findChildren<QTextEdit *>();
    if (std::ranges::any_of(textEdits, [&marker](QTextEdit *view) {
          return view->toPlainText().contains(QString::fromStdString(marker));
        }))
      return true;
    const auto plainEdits = card.findChildren<QPlainTextEdit *>();
    if (std::ranges::any_of(plainEdits, [&marker](QPlainTextEdit *view) {
          return view->toPlainText().contains(QString::fromStdString(marker));
        }))
      return true;
    return std::ranges::any_of(
        card.findChildren<QLabel *>(), [&marker](QLabel *label) {
          return label->property("kind").toString() != QStringLiteral("title") &&
                 label->text().contains(QString::fromStdString(marker));
        });
  };

  bool result = true;
  for (VisibleCardData &data : cards) {
    ConversationCard card(data, nullptr, true, true, true, 820, {}, true);
    result &= expect(
        card.isCollapsed() &&
            card.property("conversationBodyProjectionDeferred").toBool() &&
            !bodyContains(card),
        "collapsed interaction constructs no hidden heavy body");
    VisibleCardData latest = data;
    appendMarker(latest);
    result &= expect(
        card.applyPresentation(latest) == PresentationImpact::PaintOnly &&
            card.property("conversationBodyProjectionDeferred").toBool() &&
            !bodyContains(card),
        "collapsed lifecycle updates only the card header surface");
    card.setCollapsed(false);
    result &= expect(
        !card.property("conversationBodyProjectionDeferred").toBool() &&
            card.property("conversationDeferredBodyBuilds").toULongLong() ==
                1 &&
            bodyContains(card),
        "expansion projects the latest deferred body exactly once");
  }
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  using namespace codexui::codex::middle;
  const bool result = viewportProportionalFoundation() &&
                      exactStructuralRowsPreserveTheViewport() &&
                      boundedTailAppendIsViewportProportional() &&
                      targetedVisibilityChangeIsLocal() &&
                      atomicPagingAndFollowingArrival() &&
                      pinnedTurnPagingPreservesRetainedActivityAnchor() &&
                      historyWindowLivesWithThePresentedThread() &&
                      delayedThreadSelectionSpinner() &&
                      virtualTurnSurfaceAndInteractivePromotion() &&
                      directTailGrowsTheRetainedTurnSurface() &&
                      acknowledgedSteeringMovesAboveFollowingActivity() &&
                      passiveAndInteractivePresentationShareExactGeometry() &&
                      bidirectionalLazyMeasurementPreservesNativeScrollMotion() &&
                      largeIncomingCommandUsesBoundedFinalWidthLayout() &&
                      streamingMarkdownReparsesOnlyMutableTail() &&
                      passiveMarkdownHoverKeepsLinkSemanticsWithoutAnEditor() &&
                      selectionFocusAndOneGesturePromotion() &&
                      outsideTextDragDoesNotReenterTheView() &&
                      collapsedLargeCardsSkipBodyProjection() &&
                      collapsedInteractionDefersEveryHeavyCardBody();
  if (result)
    std::cout << "Conversation virtualization tests passed\n";
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
