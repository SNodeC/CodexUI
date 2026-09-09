// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"

#include <QApplication>
#include <QElapsedTimer>
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
  result &= expect(initialWidgets > 0 && initialWidgets <= 48,
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
  ConversationCard *visible = materializedCard(view, visibleIdentity.first);
  const QModelIndex visibleIndex =
      view.conversationModel()->indexForStableKey(visibleIdentity.first);
  VisibleCardData visibleUpdate =
      *view.conversationModel()->card(visibleIndex.row());
  std::get<AgentMessageData>(visibleUpdate.payload).text +=
      std::string(240, 'x');
  const qulonglong offscreenBefore =
      view.property("targetedOffscreenCardUpdates").toULongLong();
  const auto visibleImpact =
      view.applyCardPresentation(std::move(visibleUpdate));
  settle();
  result &= expect(
      visible && materializedCard(view, visibleIdentity.first) == visible &&
          visibleImpact == PresentationImpact::GeometryChanged &&
          view.property("targetedOffscreenCardUpdates").toULongLong() ==
              offscreenBefore,
      "one visible stream update mutates only its retained row");
  result &= expect(firstVisible(view).second == visibleIdentity.second,
                   "visible height change preserves the exact painted anchor");
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

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  const bool result =
      viewportProportionalFoundation() && atomicPagingAndFollowingArrival();
  if (result)
    std::cout << "Conversation virtualization tests passed\n";
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
