// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"

#include <QApplication>
#include <QElapsedTimer>
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

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  const bool result = viewportProportionalFoundation() &&
                      atomicPagingAndFollowingArrival() &&
                      virtualTurnSurfaceAndInteractivePromotion();
  if (result)
    std::cout << "Conversation virtualization tests passed\n";
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
