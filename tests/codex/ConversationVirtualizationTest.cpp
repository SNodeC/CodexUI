// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollBar>

#include <algorithm>
#include <cstdlib>
#include <iostream>
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

void settle(int passes = 4) {
  while (passes-- > 0)
    QApplication::processEvents(QEventLoop::AllEvents, 20);
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
  const std::string tailKey = stableKey(tail.card.key);
  result &= expect(view.appendTailCard(std::move(tail), 10'000),
                   "canonical tail append was accepted");
  settle();
  const auto anchorAfter = firstVisible(view);
  result &= expect(
      view.conversationModel()->rowCount() == 10'000 &&
          view.conversationModel()->indexForStableKey(tailKey).row() == 9'999 &&
          view.conversationModel()->hiddenAuthoritativeItemCount() == 1 &&
          view.conversationModel()->hasMore(),
      "bounded tail append did not retain the exact suffix and history chrome");
  result &= expect(anchorAfter == anchorBefore &&
                       view.horizontalScrollBar()->value() == horizontalBefore,
                   "bounded tail append and prefix trim did not preserve both "
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
  const std::string followingKey = stableKey(followingTail.card.key);
  result &= expect(following.appendTailCard(std::move(followingTail), 80),
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
  rootedView.verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMinimum);
  rootedView.verticalScrollBar()->setValue(
      rootedView.verticalScrollBar()->maximum() / 2);
  settle();
  const auto rootedAnchor = firstVisible(rootedView);
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
    return rootedView.appendTailCard(std::move(nestedTail), 80);
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
          firstVisible(rootedView) == rootedAnchor,
      "pinned Turn root trim lost identity, rebuilt history, or moved anchor");
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

  ConversationSnapshot loaded = conversation(160);
  loaded.hasMore = true;
  view.reconcileStaged(std::move(loaded));
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
                       view.materializedCardCount() <= 48,
                   "Load 80 commits one complete virtualized frame");

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
  ConversationCard *promoted = materializedCard(view, stableKey(root.key));
  result &= expect(promoted && promoted->property("virtualTurnRoot").toBool() &&
                       promoted->parentWidget() == view.viewport(),
                   "hover promotes only the interactive root fragment to a "
                   "real viewport editor");
  result &= expect(view.materializedCardCount() == 1,
                   "interactive promotion remains row-local and bounded");
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
  ConversationCard *card = materializedCard(view, identity.first);
  QLabel *body = nullptr;
  if (card) {
    for (QLabel *label : card->findChildren<QLabel *>())
      if (label->property("markdownSource").isValid()) {
        body = label;
        break;
      }
  }
  result &= expect(card && body,
                   "hover promotes selectable Markdown to its real card");
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
  QMouseEvent restoredMove(QEvent::MouseMove, QPointF(restoredHover),
                           QPointF(restoredHover),
                           view.viewport()->mapToGlobal(restoredHover),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &restoredMove);
  settle();
  card = materializedCard(view, identity.first);
  body = nullptr;
  if (card)
    for (QLabel *label : card->findChildren<QLabel *>())
      if (label->property("markdownSource").isValid()) {
        body = label;
        break;
      }
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
  QLabel *body = nullptr;
  if (card)
    for (QLabel *label : card->findChildren<QLabel *>())
      if (label->property("markdownSource").isValid()) {
        body = label;
        break;
      }
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

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  const bool result = viewportProportionalFoundation() &&
                      boundedTailAppendIsViewportProportional() &&
                      targetedVisibilityChangeIsLocal() &&
                      atomicPagingAndFollowingArrival() &&
                      virtualTurnSurfaceAndInteractivePromotion() &&
                      selectionFocusAndOneGesturePromotion() &&
                      outsideTextDragDoesNotReenterTheView();
  if (result)
    std::cout << "Conversation virtualization tests passed\n";
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
