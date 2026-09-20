// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/ui/UiStyle.h"
#include "AccessibilityEventProbe.h"

#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPointingDevice>
#include <QPushButton>
#include <QRegion>
#include <QScrollBar>
#include <QStyle>
#include <QStyleFactory>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace codexui::codex::middle {
namespace {

#if defined(__SANITIZE_ADDRESS__)
constexpr qint64 InstrumentedTimingScale = 4;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr qint64 InstrumentedTimingScale = 4;
#else
constexpr qint64 InstrumentedTimingScale = 1;
#endif
#else
constexpr qint64 InstrumentedTimingScale = 1;
#endif

bool expect(bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

bool changed(ConversationView::ReconciliationResult result) {
  return result == ConversationView::ReconciliationResult::Changed;
}

bool admitted(ConversationView::SnapshotDisposition disposition) {
  return disposition == ConversationView::SnapshotDisposition::Admitted;
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

std::optional<PresentationImpact> applyPresentation(ConversationView &view,
                                                    VisibleCardData card) {
  ConversationDelta delta;
  delta.threadId = card.threadId;
  delta.presentations.push_back(std::move(card));
  return view.applyConversationDelta(std::move(delta));
}

bool applyStructural(ConversationView &view, ConversationRowChange change) {
  ConversationDelta delta;
  delta.threadId = change.placement.card.threadId;
  delta.rows.push_back(std::move(change));
  return view.applyConversationDelta(std::move(delta)).has_value();
}

bool removeProjected(ConversationView &view, nodegraph::NodeRef target) {
  ConversationDelta delta;
  delta.threadId = view.presentedThreadId();
  delta.removals.push_back(std::move(target));
  return view.applyConversationDelta(std::move(delta)).has_value();
}

bool appendProjected(ConversationView &view, ConversationRowPlacement tail) {
  ConversationRowChange change;
  change.placement = std::move(tail);
  if (const ConversationItemModel::Row *last = view.conversationModel()->row(
          view.conversationModel()->rowCount() - 1))
    change.previousCardKey = last->card.key;
  ConversationDelta delta;
  delta.threadId = change.placement.card.threadId;
  delta.rows.push_back(std::move(change));
  return view.applyConversationDelta(std::move(delta)).has_value();
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
  section.cards.push_back(std::move(root));
  for (std::size_t serial = firstActivity;
       serial < firstActivity + activityCount; ++serial) {
    const std::string suffix = std::to_string(serial);
    section.cards.push_back(
        {AuthoritativeItemKey{"virtual-thread", "shared-turn",
                              "item-" + suffix},
         CardKind::AgentMessage, "virtual-thread", "shared-turn",
         "item-" + suffix, AgentMessageData{"Answer " + suffix, true}});
  }
  result.sections.push_back(std::move(section));
  return result;
}

void settle(int passes = 4) {
  while (passes-- > 0)
    QApplication::processEvents(QEventLoop::AllEvents, 20);
}

class ViewportPaintRegionProbe final : public QObject {
public:
  explicit ViewportPaintRegionProbe(QWidget *viewport) : viewport_(viewport) {
    viewport_->installEventFilter(this);
  }

  ~ViewportPaintRegionProbe() override { viewport_->removeEventFilter(this); }

  void start() {
    painted_ = {};
    paintEvents_ = 0;
    active_ = true;
  }

  QRegion stop() {
    active_ = false;
    return painted_;
  }

  [[nodiscard]] int paintEvents() const noexcept { return paintEvents_; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (active_ && watched == viewport_ && event->type() == QEvent::Paint) {
      ++paintEvents_;
      painted_ += static_cast<QPaintEvent *>(event)->region();
    }
    return false;
  }

private:
  QWidget *viewport_ = nullptr;
  QRegion painted_;
  int paintEvents_ = 0;
  bool active_ = false;
};

class CommandOutputFirstPaintProbe final : public QObject {
public:
  ~CommandOutputFirstPaintProbe() override {
    if (viewport_)
      viewport_->removeEventFilter(this);
  }

  void watch(CommandOutputView *output) {
    output_ = output;
    viewport_ = output ? output->viewport() : nullptr;
    if (viewport_)
      viewport_->installEventFilter(this);
  }

  [[nodiscard]] bool seen() const noexcept { return seen_; }
  [[nodiscard]] bool followedTail() const noexcept { return followedTail_; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (!seen_ && watched == viewport_ && event->type() == QEvent::Paint) {
      seen_ = true;
      followedTail_ = output_ && output_->followsLatest() &&
                      output_->verticalScrollBar()->maximum() > 0 &&
                      output_->verticalScrollBar()->value() ==
                          output_->verticalScrollBar()->maximum();
    }
    return false;
  }

private:
  QPointer<CommandOutputView> output_;
  QPointer<QWidget> viewport_;
  bool seen_ = false;
  bool followedTail_ = false;
};

class WheelDeliveryProbe final : public QObject {
public:
  explicit WheelDeliveryProbe(QObject *receiver) : receiver_(receiver) {
    receiver_->installEventFilter(this);
  }

  ~WheelDeliveryProbe() override { receiver_->removeEventFilter(this); }

  [[nodiscard]] bool seen() const noexcept { return seen_; }
  [[nodiscard]] ulong timestamp() const noexcept { return timestamp_; }
  [[nodiscard]] Qt::MouseEventSource source() const noexcept { return source_; }
  [[nodiscard]] const QPointingDevice *device() const noexcept {
    return device_;
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched == receiver_ && event->type() == QEvent::Wheel) {
      const auto *wheel = static_cast<QWheelEvent *>(event);
      seen_ = true;
      timestamp_ = wheel->timestamp();
      source_ = wheel->source();
      device_ = wheel->pointingDevice();
    }
    return false;
  }

private:
  QObject *receiver_ = nullptr;
  bool seen_ = false;
  ulong timestamp_ = 0;
  Qt::MouseEventSource source_ = Qt::MouseEventNotSynthesized;
  const QPointingDevice *device_ = nullptr;
};

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
  VisibleCardData card{AuthoritativeItemKey{threadId, "turn", "message"},
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

ConversationSnapshot commandConversation(const std::string &threadId,
                                         std::size_t count) {
  ConversationSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.sections.reserve(count);
  for (std::size_t row = 0; row < count; ++row) {
    const std::string suffix = std::to_string(row);
    VisibleCardData card{
        AuthoritativeItemKey{threadId, "turn-" + suffix, "command-" + suffix},
        CardKind::CommandExecution,
        threadId,
        "turn-" + suffix,
        "command-" + suffix,
        CommandExecutionData{"printf retained", "done", "/tmp", 0, 1},
        nodegraph::NodeStatus::Completed};
    snapshot.sections.push_back(
        {"section-" + suffix, card.turnId, {std::move(card)}, std::nullopt});
  }
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
  for (ConversationCard *card :
       view.viewport()->findChildren<ConversationCard *>(
           QString{}, Qt::FindDirectChildrenOnly))
    if (stableKey(card->data().key) == key)
      return card;
  return nullptr;
}

bool residencyWindowCovered(ConversationView &view) {
  const int height = view.viewport()->height();
  const QRect window = view.viewport()->rect().adjusted(0, -height, 0, height);
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const QModelIndex index = view.conversationModel()->index(row);
    const ConversationItemModel::Row *modelRow =
        view.conversationModel()->row(row);
    if (modelRow && view.visualRect(index).intersects(window) &&
        !materializedCard(view, modelRow->stableKey))
      return false;
  }
  return true;
}

bool viewportCovered(ConversationView &view) {
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const QModelIndex index = view.conversationModel()->index(row);
    const ConversationItemModel::Row *modelRow =
        view.conversationModel()->row(row);
    if (modelRow &&
        view.visualRect(index).intersects(view.viewport()->rect()) &&
        !materializedCard(view, modelRow->stableKey))
      return false;
  }
  return true;
}

bool waitForResidency(ConversationView &view, int timeoutMilliseconds = 3000) {
  return waitUntil(
      [&view] {
        return !view.structuralStagingActive() && residencyWindowCovered(view);
      },
      timeoutMilliseconds);
}

bool viewportProportionalFoundation() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  settle();

  ConversationSnapshot snapshot = conversation(10'000);
  bool result = expect(changed(view.reconcile(snapshot)),
                       "ten-thousand-row authority is accepted");
  result &= expect(waitForResidency(view),
                   "initial overscan admission reaches a bounded fixed point");
  const int initialWidgets = view.materializedCardCount();
  result &= expect(view.conversationModel()->rowCount() == 10'000,
                   "the item model indexes all canonical rows");
  result &= expect(initialWidgets <= 48,
                   "initial QWidget count is bounded by the viewport");
  result &= expect(view.findChildren<ConversationCard *>().size() <= 48,
                   "history has no placeholder or hidden QWidget per row");
  result &= expect(view.isAtBottom(),
                   "initial selection reveals a complete following tail");

  const QModelIndex measuredOffscreen = view.conversationModel()->index(0);
  const std::string offscreenKey =
      measuredOffscreen.data(ConversationItemModel::StableKeyRole)
          .toString()
          .toStdString();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  settle();
  result &= expect(materializedCard(view, offscreenKey) != nullptr,
                   "the future offscreen row first acquires an exact native "
                   "renderer height");

  const int middle = view.verticalScrollBar()->maximum() / 2;
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(middle);
  result &= expect(waitForResidency(view),
                   "paused navigation admits its bounded overscan window");
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
  result &= expect(changed(view.reconcile(prepended)),
                   "a structural insertion reconciles precisely");
  result &= expect(waitForResidency(view),
                   "structural insertion restores bounded overscan residency");
  const auto anchorAfter = firstVisible(view);
  result &= expect(anchorAfter == anchorBefore,
                   "insertion above preserves identity and exact pixel offset");
  result &= expect(view.horizontalScrollBar()->value() == 0 &&
                       view.materializedCardCount() <=
                           std::max(48, beforeAppendWidgets + 4),
                   "structural insertion preserves both axes and widget bound");

  const QModelIndex offscreen =
      view.conversationModel()->indexForStableKey(offscreenKey);
  result &= expect(materializedCard(view, offscreenKey) == nullptr,
                   "chosen offscreen row has no QWidget");
  VisibleCardData offscreenUpdate =
      *view.conversationModel()->card(offscreen.row());
  std::get<AgentMessageData>(offscreenUpdate.payload).text +=
      std::string(1200, 'x');
  const qulonglong constructionsBefore =
      view.property("conversationCardConstructions").toULongLong();
  const auto offscreenImpact =
      applyPresentation(view, std::move(offscreenUpdate));
  settle();
  result &=
      expect(offscreenImpact == PresentationImpact::GeometryChanged &&
                 materializedCard(view, offscreenKey) == nullptr &&
                 firstVisible(view) == anchorBefore &&
                 view.property("conversationCardConstructions").toULongLong() ==
                     constructionsBefore,
             "offscreen content replaces a stale exact height with a bounded "
             "estimate while preserving the viewport anchor");

  const auto visibleIdentity = firstVisible(view);
  const QModelIndex visibleIndex =
      view.conversationModel()->indexForStableKey(visibleIdentity.first);
  ConversationCard *const visibleCard =
      materializedCard(view, visibleIdentity.first);
  VisibleCardData visibleUpdate =
      *view.conversationModel()->card(visibleIndex.row());
  std::get<AgentMessageData>(visibleUpdate.payload).text +=
      std::string(240, 'x');
  const qulonglong offscreenBefore =
      view.property("targetedOffscreenCardUpdates").toULongLong();
  const qulonglong visibleConstructionsBefore =
      view.property("conversationCardConstructions").toULongLong();
  const auto visibleImpact = applyPresentation(view, std::move(visibleUpdate));
  settle();
  result &= expect(
      visibleCard &&
          materializedCard(view, visibleIdentity.first) == visibleCard &&
          visibleImpact == PresentationImpact::GeometryChanged &&
          view.property("targetedOffscreenCardUpdates").toULongLong() ==
              offscreenBefore &&
          view.property("conversationCardConstructions").toULongLong() ==
              visibleConstructionsBefore,
      "one visible stream update keeps and updates its authoritative card");
  result &= expect(firstVisible(view).second == visibleIdentity.second,
                   "visible height change preserves the exact painted anchor");
  const VisibleCardData currentVisible = *view.conversationModel()->card(
      view.conversationModel()->indexForStableKey(visibleIdentity.first).row());
  for (TurnSection &section : prepended.sections)
    for (VisibleCardData &card : section.cards)
      if (stableKey(card.key) == visibleIdentity.first)
        card = currentVisible;
  view.scrollTo(offscreen, QAbstractItemView::PositionAtTop);
  ConversationCard *returned = materializedCard(view, offscreenKey);
  const QRect returnedGeometry = returned ? returned->geometry() : QRect{};
  settle();
  result &= expect(returned && returned->geometry() == returnedGeometry,
                   "returning to an invalidated offscreen row settles its real "
                   "renderer before the frame is exposed");
  const int exactOffscreenExtent =
      view.visualRect(view.conversationModel()->indexForStableKey(offscreenKey))
          .height();

  ConversationSnapshot rescanned = prepended;
  VisibleCardData rescannedCard = *view.conversationModel()->card(
      view.conversationModel()->indexForStableKey(offscreenKey).row());
  std::get<AgentMessageData>(rescannedCard.payload).text +=
      std::string(1200, 'y');
  for (TurnSection &section : rescanned.sections)
    for (VisibleCardData &card : section.cards)
      if (stableKey(card.key) == offscreenKey)
        card = rescannedCard;
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  result &= expect(waitForResidency(view),
                   "the rescan anchor's overscan reaches a fixed point");
  const auto rescanAnchor = firstVisible(view);
  const qulonglong rescanConstructions =
      view.property("conversationCardConstructions").toULongLong();
  result &= expect(materializedCard(view, offscreenKey) == nullptr &&
                       changed(view.reconcile(rescanned)),
                   "a whole-snapshot update changes the released row");
  settle();
  result &= expect(
      materializedCard(view, offscreenKey) == nullptr &&
          firstVisible(view) == rescanAnchor &&
          view.visualRect(
                  view.conversationModel()->indexForStableKey(offscreenKey))
                  .height() != exactOffscreenExtent &&
          view.property("conversationCardConstructions").toULongLong() ==
              rescanConstructions,
      "whole-snapshot reconciliation invalidates only the offscreen scalar "
      "height and preserves the painted anchor");
  const QModelIndex rescannedIndex =
      view.conversationModel()->indexForStableKey(offscreenKey);
  view.scrollTo(rescannedIndex, QAbstractItemView::PositionAtTop);
  ConversationCard *rescannedRenderer = materializedCard(view, offscreenKey);
  const QRect rescannedGeometry =
      rescannedRenderer ? rescannedRenderer->geometry() : QRect{};
  settle();
  result &= expect(
      rescannedRenderer && rescannedRenderer->geometry() == rescannedGeometry,
      "the reconciled offscreen row exposes its exact renderer geometry in "
      "one frame");
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
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "exact structural row fixture reconciles");
  settle();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  result &= expect(waitForResidency(view),
                   "exact-structure fixture starts from settled overscan "
                   "residency");

  const auto anchorBeforeTransaction = firstVisible(view);
  VisibleCardData inserted = message(10'001, "Inserted in the middle");
  inserted.target = insertedTarget;
  ConversationRowChange insertion;
  insertion.placement = {inserted, "section-inserted", false, false, false};
  insertion.previousCardKey = message(7).key;
  insertion.nextCardKey = message(8).key;
  const qulonglong resetsBefore =
      view.conversationModel()->property("modelResetCount").toULongLong();
  const qulonglong identityRebuildsBefore =
      view.conversationModel()
          ->property("modelIndexRebuildCount")
          .toULongLong();
  const qulonglong sectionRebuildsBefore =
      view.property("conversationSectionRangeRebuilds").toULongLong();
  const qulonglong heightRebuildsBefore =
      view.property("conversationHeightIndexRebuilds").toULongLong();
  ViewportPaintRegionProbe paintProbe(view.viewport());
  paintProbe.start();
  ConversationRowChange movement;
  movement.placement = {message(2), "section-2", false, false, false};
  movement.placement.card.target = targets[2];
  movement.previousCardKey = message(9'999).key;
  ConversationDelta transaction;
  transaction.threadId = view.presentedThreadId();
  transaction.rows = {std::move(insertion), std::move(movement)};
  transaction.removals.push_back(targets.front());
  const bool applied =
      view.applyConversationDelta(std::move(transaction)).has_value();
  settle();
  const QRegion offscreenPaint = paintProbe.stop();
  const int offscreenPaintEvents = paintProbe.paintEvents();
  if (offscreenPaintEvents > 2)
    std::cerr << "offscreen structural paint events=" << offscreenPaintEvents
              << " region=" << offscreenPaint.boundingRect().x() << ','
              << offscreenPaint.boundingRect().y() << ' '
              << offscreenPaint.boundingRect().width() << 'x'
              << offscreenPaint.boundingRect().height() << '\n';
  const QModelIndex insertedIndex =
      view.conversationModel()->indexForTarget(insertedTarget);
  const VisibleCardData *beforeInserted =
      view.conversationModel()->card(insertedIndex.row() - 1);
  const VisibleCardData *afterInserted =
      view.conversationModel()->card(insertedIndex.row() + 1);
  result &=
      expect(applied && insertedIndex.isValid() && insertedIndex.row() > 0 &&
                 beforeInserted && beforeInserted->target == targets[7] &&
                 afterInserted && afterInserted->target == targets[8] &&
                 view.conversationModel()->indexForTarget(targets[2]).row() ==
                     view.conversationModel()->rowCount() - 1,
             "one transaction did not commit its exact insert and move order");
  result &= expect(
      !view.conversationModel()->indexForTarget(targets.front()).isValid(),
      "the exact removal retires its target identity");
  result &= expect(firstVisible(view) == anchorBeforeTransaction,
                   "the multi-operation transaction retains its pixel anchor");
  result &= expect(
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
                  .toULongLong() == 1,
      "the transaction emits one exact insert, move, and removal without a "
      "reset");
  result &= expect(
      view.conversationModel()
                  ->property("modelIndexRebuildCount")
                  .toULongLong() == identityRebuildsBefore &&
          view.property("conversationSectionRangeRebuilds").toULongLong() ==
              sectionRebuildsBefore &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              heightRebuildsBefore,
      "exact structural operations preserve identity, section, and height "
      "indexes");
  result &=
      expect(offscreenPaintEvents <= 2,
             "Qt bounds the multi-signal transaction to two coalesced paints");
  result &= expect(offscreenPaint.subtracted(view.viewport()->rect()).isEmpty(),
                   "offscreen transaction paint remains viewport bounded");
  result &= expect(view.materializedCardCount() <= 48,
                   "the exact transaction retains bounded widget residency");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  const QModelIndex visibleIndex =
      view.conversationModel()->indexForTarget(targets[1]);
  const QRect visibleBefore = view.visualRect(visibleIndex);
  paintProbe.start();
  const bool removedVisible = removeProjected(view, targets[1]);
  settle();
  const QRegion visiblePaint = paintProbe.stop();
  const int visiblePaintEvents = paintProbe.paintEvents();
  result &= expect(
      removedVisible && visibleBefore.intersects(view.viewport()->rect()) &&
          visiblePaintEvents >= 1 && visiblePaintEvents <= 2 &&
          visiblePaint.intersects(visibleBefore) &&
          visiblePaint.subtracted(view.viewport()->rect()).isEmpty(),
      "a visible exact removal repaints its viewport-bounded suffix");
  return result;
}

bool rejectedDeltaTransactionsAreAtomic() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef firstTarget;
  nodegraph::NodeRef secondTarget;
  nodegraph::NodeRef conflictingTarget;
  nodegraph::NodeRef promptTarget;
  nodegraph::NodeRef authoritativeTarget;
  nodegraph::NodeRef dependentATarget;
  nodegraph::NodeRef dependentBTarget;
  {
    auto write = graph.write();
    firstTarget =
        write.upsert({nodegraph::NodeKind::Item, "atomic-presentation-first"});
    secondTarget =
        write.upsert({nodegraph::NodeKind::Item, "atomic-presentation-second"});
    conflictingTarget = write.upsert(
        {nodegraph::NodeKind::Item, "atomic-presentation-conflict"});
    promptTarget =
        write.upsert({nodegraph::NodeKind::Item, "atomic-local-prompt"});
    authoritativeTarget = write.upsert(
        {nodegraph::NodeKind::Item, "atomic-authoritative-prompt"});
    dependentATarget =
        write.upsert({nodegraph::NodeKind::Item, "atomic-dependent-a"});
    dependentBTarget =
        write.upsert({nodegraph::NodeKind::Item, "atomic-dependent-b"});
    static_cast<void>(write.finish());
  }

  ConversationSnapshot presentationSnapshot = conversation(2);
  presentationSnapshot.sections[0].cards[0].target = firstTarget;
  presentationSnapshot.sections[1].cards[0].target = secondTarget;
  ConversationView presentationView;
  presentationView.resize(820, 420);
  presentationView.show();
  bool result = expect(
      changed(presentationView.reconcile(std::move(presentationSnapshot))),
      "the atomic presentation fixture reconciles");
  settle();
  const VisibleCardData firstBefore =
      *presentationView.conversationModel()->card(0);
  const VisibleCardData secondBefore =
      *presentationView.conversationModel()->card(1);
  ConversationCard *const firstCard =
      materializedCard(presentationView, stableKey(firstBefore.key));
  ConversationCard *const secondCard =
      materializedCard(presentationView, stableKey(secondBefore.key));
  const QRect firstGeometry = firstCard ? firstCard->geometry() : QRect{};
  const QRect secondGeometry = secondCard ? secondCard->geometry() : QRect{};
  const QImage pixelsBefore = presentationView.viewport()->grab().toImage();
  const qulonglong dataChanges = presentationView.conversationModel()
                                     ->property("modelDataChangeCount")
                                     .toULongLong();
  const qulonglong geometryPasses =
      presentationView.property("conversationLocalGeometryPasses")
          .toULongLong();

  VisibleCardData valid = firstBefore;
  std::get<AgentMessageData>(valid.payload).text = "must not be committed";
  VisibleCardData invalid = secondBefore;
  invalid.target = conflictingTarget;
  ConversationDelta presentations;
  presentations.threadId = presentationView.presentedThreadId();
  presentations.presentations = {std::move(valid), std::move(invalid)};
  const auto presentationResult =
      presentationView.applyConversationDelta(std::move(presentations));
  settle();
  result &= expect(
      !presentationResult &&
          *presentationView.conversationModel()->card(0) == firstBefore &&
          *presentationView.conversationModel()->card(1) == secondBefore &&
          materializedCard(presentationView, stableKey(firstBefore.key)) ==
              firstCard &&
          materializedCard(presentationView, stableKey(secondBefore.key)) ==
              secondCard &&
          (!firstCard || firstCard->geometry() == firstGeometry) &&
          (!secondCard || secondCard->geometry() == secondGeometry) &&
          presentationView.viewport()->grab().toImage() == pixelsBefore &&
          presentationView.conversationModel()
                  ->property("modelDataChangeCount")
                  .toULongLong() == dataChanges &&
          presentationView.property("conversationLocalGeometryPasses")
                  .toULongLong() == geometryPasses,
      "an incompatible later presentation rejects the whole batch without "
      "model, renderer, geometry, pixel, or work changes");

  const std::string thread = "atomic-structural";
  VisibleCardData local{LocalPromptKey{901},
                        CardKind::LocalPrompt,
                        thread,
                        "turn-prompt",
                        "local",
                        LocalPromptData{901, "Atomic prompt"}};
  local.target = promptTarget;
  VisibleCardData other{AuthoritativeItemKey{thread, "turn-other", "other"},
                        CardKind::AgentMessage,
                        thread,
                        "turn-other",
                        "other",
                        AgentMessageData{"Other row", true}};
  other.target = secondTarget;
  ConversationSnapshot structuralSnapshot;
  structuralSnapshot.threadId = thread;
  structuralSnapshot.sections.push_back(
      {"prompt-section", "turn-prompt", {local}, local.key});
  structuralSnapshot.sections.push_back(
      {"other-section", "turn-other", {other}, other.key});
  ConversationView structuralView;
  structuralView.resize(820, 420);
  structuralView.show();
  int acknowledgements = 0;
  nodegraph::NodeRef acknowledged;
  structuralView.setPromptMaterializedAction([&](nodegraph::NodeRef target) {
    ++acknowledgements;
    acknowledged = std::move(target);
    return true;
  });
  result &= expect(changed(structuralView.reconcile(structuralSnapshot)),
                   "the atomic structural fixture reconciles");
  settle();
  const QImage structuralPixels = structuralView.viewport()->grab().toImage();
  const qulonglong exactMoves = structuralView.conversationModel()
                                    ->property("modelExactMoveCount")
                                    .toULongLong();
  const qulonglong exactRemovals = structuralView.conversationModel()
                                       ->property("modelExactRemoveCount")
                                       .toULongLong();

  VisibleCardData promoted = local;
  promoted.kind = CardKind::UserMessage;
  promoted.itemId = "authoritative";
  promoted.payload = UserMessageData{"Atomic prompt", {}};
  promoted.target = authoritativeTarget;
  ConversationRowChange promotion{
      {promoted, "prompt-section", true, false, false}, {}, other.key};
  VisibleCardData conflicting = other;
  conflicting.key = AuthoritativeItemKey{thread, "turn-other", "different-key"};
  ConversationRowChange invalidRow{
      {std::move(conflicting), "other-section", true, false, false},
      promoted.key,
      {}};
  ConversationDelta structural;
  structural.threadId = thread;
  structural.rows = {std::move(promotion), std::move(invalidRow)};
  structural.removals.push_back(promptTarget);
  const auto structuralResult =
      structuralView.applyConversationDelta(std::move(structural));
  settle();
  const QModelIndex localIndex =
      structuralView.conversationModel()->indexForStableKey(
          stableKey(local.key));
  result &= expect(
      !structuralResult && localIndex.isValid() &&
          structuralView.conversationModel()->card(localIndex.row())->kind ==
              CardKind::LocalPrompt &&
          structuralView.conversationModel()->card(localIndex.row())->target ==
              promptTarget &&
          structuralView.conversationModel()->rowCount() == 2 &&
          acknowledgements == 0 &&
          structuralView.viewport()->grab().toImage() == structuralPixels &&
          structuralView.conversationModel()
                  ->property("modelExactMoveCount")
                  .toULongLong() == exactMoves &&
          structuralView.conversationModel()
                  ->property("modelExactRemoveCount")
                  .toULongLong() == exactRemovals,
      "a conflicting later row rejects promotion and retirement without "
      "partial mutation or acknowledgement");

  ConversationSnapshot authoritative;
  authoritative.threadId = thread;
  authoritative.materializedPrompts.push_back({promoted.key, promptTarget});
  authoritative.sections.push_back(
      {"prompt-section", "turn-prompt", {promoted}, promoted.key});
  authoritative.sections.push_back(
      {"other-section", "turn-other", {other}, other.key});
  result &= expect(
      changed(structuralView.reconcile(authoritative)) &&
          acknowledgements == 1 && acknowledged == promptTarget,
      "the subsequent authoritative frame promotes and acknowledges exactly "
      "once");

  VisibleCardData dependentA{
      AuthoritativeItemKey{thread, "dependent-a", "root"},
      CardKind::UserMessage,
      thread,
      "dependent-a",
      "root",
      UserMessageData{"Dependent A", {}}};
  dependentA.target = dependentATarget;
  VisibleCardData dependentB{
      AuthoritativeItemKey{thread, "dependent-b", "root"},
      CardKind::UserMessage,
      thread,
      "dependent-b",
      "root",
      UserMessageData{"Dependent B", {}}};
  dependentB.target = dependentBTarget;
  ConversationDelta unresolvedDependency;
  unresolvedDependency.threadId = thread;
  unresolvedDependency.rows.push_back(
      {{dependentA, "dependent-a-section", true, false, false},
       dependentB.key,
       {}});
  unresolvedDependency.rows.push_back(
      {{dependentB, "dependent-b-section", true, false, false},
       AuthoritativeItemKey{thread, "outside", "before"},
       AuthoritativeItemKey{thread, "outside", "after"}});
  const int rowsBeforeDependency =
      structuralView.conversationModel()->rowCount();
  const qulonglong appendsBeforeDependency =
      structuralView.conversationModel()
          ->property("modelTailAppendCount")
          .toULongLong();
  const QImage pixelsBeforeDependency =
      structuralView.viewport()->grab().toImage();
  result &= expect(
      !structuralView.applyConversationDelta(std::move(unresolvedDependency))
              .has_value() &&
          structuralView.conversationModel()->rowCount() ==
              rowsBeforeDependency &&
          structuralView.conversationModel()
                  ->property("modelTailAppendCount")
                  .toULongLong() == appendsBeforeDependency &&
          structuralView.viewport()->grab().toImage() == pixelsBeforeDependency,
      "an unresolved in-batch neighbor rejects the whole structural plan");

  VisibleCardData unrepresented{
      AuthoritativeItemKey{thread, "old-turn", "old-item"},
      CardKind::AgentMessage,
      thread,
      "old-turn",
      "old-item",
      AgentMessageData{"Outside the resident suffix", true}};
  unrepresented.target = conflictingTarget;
  ConversationDelta outside;
  outside.threadId = thread;
  outside.rows.push_back({{unrepresented, "old-section", false, true, false},
                          AuthoritativeItemKey{thread, "older", "before"},
                          AuthoritativeItemKey{thread, "older", "after"}});
  const int rowsBeforeOutside = structuralView.conversationModel()->rowCount();
  const QImage pixelsBeforeMissingRows =
      structuralView.viewport()->grab().toImage();
  result &= expect(
      !structuralView.applyConversationDelta(std::move(outside)).has_value() &&
          structuralView.conversationModel()->rowCount() == rowsBeforeOutside &&
          structuralView.viewport()->grab().toImage() ==
              pixelsBeforeMissingRows,
      "a structural row with missing canonical neighbors rejects atomically");
  ConversationDelta outsideBeginning;
  outsideBeginning.threadId = thread;
  outsideBeginning.rows.push_back(
      {{unrepresented, "old-section", false, true, false},
       {},
       AuthoritativeItemKey{thread, "older", "unrepresented-next"}});
  result &= expect(
      !structuralView.applyConversationDelta(std::move(outsideBeginning))
              .has_value() &&
          structuralView.conversationModel()->rowCount() == rowsBeforeOutside &&
          structuralView.viewport()->grab().toImage() ==
              pixelsBeforeMissingRows,
      "an absent canonical-beginning row rejects when its successor is "
      "missing");

  const QImage pixelsBeforeMissingLandmarks =
      structuralView.viewport()->grab().toImage();
  VisibleCardData missingYou = unrepresented;
  missingYou.key = AuthoritativeItemKey{thread, "old-turn", "missing-you"};
  missingYou.kind = CardKind::UserMessage;
  missingYou.itemId = "missing-you";
  missingYou.payload = UserMessageData{"Retain this loaded You landmark", {}};
  ConversationDelta missingYouDelta;
  missingYouDelta.threadId = thread;
  missingYouDelta.rows.push_back(
      {{std::move(missingYou), "old-section", false, true, false},
       AuthoritativeItemKey{thread, "older", "before"},
       AuthoritativeItemKey{thread, "older", "after"}});
  result &= expect(
      !structuralView.applyConversationDelta(std::move(missingYouDelta))
              .has_value() &&
          structuralView.conversationModel()->rowCount() == rowsBeforeOutside &&
          structuralView.viewport()->grab().toImage() ==
              pixelsBeforeMissingLandmarks,
      "a disconnected loaded You row requests an authoritative snapshot "
      "instead of disappearing from the complete model");

  VisibleCardData missingRoot = unrepresented;
  missingRoot.key =
      AuthoritativeItemKey{thread, "missing-root-turn", "missing-root"};
  missingRoot.turnId = "missing-root-turn";
  missingRoot.itemId = "missing-root";
  ConversationDelta missingRootDelta;
  missingRootDelta.threadId = thread;
  missingRootDelta.rows.push_back(
      {{std::move(missingRoot), "missing-root-section", true, false, false},
       AuthoritativeItemKey{thread, "older", "before"},
       AuthoritativeItemKey{thread, "older", "after"}});
  result &= expect(
      !structuralView.applyConversationDelta(std::move(missingRootDelta))
              .has_value() &&
          structuralView.conversationModel()->rowCount() == rowsBeforeOutside &&
          structuralView.viewport()->grab().toImage() ==
              pixelsBeforeMissingLandmarks,
      "a disconnected loaded Turn root requests an authoritative snapshot "
      "instead of disappearing from the complete model");

  VisibleCardData promotedOutside = unrepresented;
  promotedOutside.kind = CardKind::UserMessage;
  promotedOutside.payload =
      UserMessageData{"Presentation-promoted You landmark", {}};
  ConversationDelta promotedOutsideDelta;
  promotedOutsideDelta.threadId = thread;
  promotedOutsideDelta.presentations.push_back(promotedOutside);
  result &= expect(
      !structuralView.applyConversationDelta(std::move(promotedOutsideDelta))
              .has_value() &&
          structuralView.conversationModel()->rowCount() == rowsBeforeOutside &&
          structuralView.viewport()->grab().toImage() ==
              pixelsBeforeMissingLandmarks,
      "a presentation-only promotion of a missing row to a You landmark "
      "requests an authoritative snapshot");

  ConversationSnapshot promotedOutsideSnapshot = authoritative;
  promotedOutsideSnapshot.sections.insert(
      promotedOutsideSnapshot.sections.begin(),
      {"old-section", "old-turn", {promotedOutside}, promotedOutside.key});
  result &= expect(
      changed(structuralView.reconcile(std::move(promotedOutsideSnapshot))) &&
          structuralView.conversationModel()
              ->indexForStableKey(stableKey(promotedOutside.key))
              .isValid(),
      "the authoritative replacement retains the presentation-promoted You "
      "landmark");

  return result;
}

bool largeRootReplacementPreservesTheResidentSurface() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef oldRootTarget;
  nodegraph::NodeRef newRootTarget;
  {
    auto write = graph.write();
    oldRootTarget =
        write.upsert({nodegraph::NodeKind::Item, "large-view-old-root"});
    newRootTarget =
        write.upsert({nodegraph::NodeKind::Item, "large-view-new-root"});
    static_cast<void>(write.finish());
  }

  constexpr int Count = 10'000;
  const std::string thread = "large-root-view-thread";
  const std::string turn = "large-root-view-turn";
  const std::string sectionKey = "large-root-view-section";
  VisibleCardData oldRoot{
      LocalPromptKey{991},
      CardKind::LocalPrompt,
      thread,
      turn,
      "local-root",
      LocalPromptData{991, "Stable root", PromptState::InFlight}};
  oldRoot.target = oldRootTarget;
  TurnSection section;
  section.key = sectionKey;
  section.turnId = turn;
  section.rootCardKey = oldRoot.key;
  section.cards.reserve(Count);
  section.cards.push_back(oldRoot);
  for (int row = 1; row < Count; ++row) {
    const std::string item = "suffix-" + std::to_string(row);
    section.cards.push_back(
        {AuthoritativeItemKey{thread, turn, item}, CardKind::AgentMessage,
         thread, turn, item,
         AgentMessageData{"Stable retained Markdown " + item, true}});
  }
  const std::string lastKey = stableKey(section.cards.back().key);
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.activeTurnId = turn;
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(820, 420);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "the 10,000-row root-replacement fixture reconciles");
  settle();
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  result &= expect(waitForResidency(view),
                   "root-replacement fixture settles its overscan baseline");
  const auto anchorBefore = firstVisible(view);
  const QModelIndex anchorIndex =
      view.conversationModel()->indexForStableKey(anchorBefore.first);
  const QPersistentModelIndex persistentAnchor(anchorIndex);
  const QPersistentModelIndex persistentLast(
      view.conversationModel()->indexForStableKey(lastKey));
  ConversationCard *const retainedCard =
      materializedCard(view, anchorBefore.first);
  MarkdownTextView *const retainedBody =
      retainedCard ? retainedCard->findChild<MarkdownTextView *>() : nullptr;
  QTextDocument *const retainedDocument =
      retainedBody ? retainedBody->document() : nullptr;
  if (anchorIndex.isValid()) {
    view.setCurrentIndex(anchorIndex);
    view.selectionModel()->select(anchorIndex,
                                  QItemSelectionModel::ClearAndSelect |
                                      QItemSelectionModel::Rows);
  }
  if (retainedBody && retainedDocument) {
    QTextCursor selection(retainedDocument);
    selection.setPosition(0);
    selection.setPosition(
        std::min(6, std::max(0, retainedDocument->characterCount() - 1)),
        QTextCursor::KeepAnchor);
    retainedBody->setTextCursor(selection);
    retainedBody->setFocus(Qt::TabFocusReason);
  }
  settle();
  const QWidget *const focusBefore = QApplication::focusWidget();
  const auto transactionAnchor = firstVisible(view);
  const QTextCursor selectionBefore =
      retainedBody ? retainedBody->textCursor() : QTextCursor{};
  const QRect geometryBefore =
      retainedCard ? retainedCard->geometry() : QRect{};
  const QImage pixelsBefore = view.viewport()->grab().toImage();
  const int cardsBefore = view.materializedCardCount();
  const qsizetype documentsBefore =
      view.findChildren<MarkdownTextView *>().size();
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  const qulonglong releases =
      view.property("conversationRowsReleased").toULongLong();
  const qulonglong materializationPasses =
      view.property("conversationMaterializationPasses").toULongLong();
  const qulonglong geometryPasses =
      view.property("conversationLocalGeometryPasses").toULongLong();
  const qulonglong graphPasses =
      view.property("graphRefreshPasses").toULongLong();
  const qulonglong sectionRebuilds =
      view.property("conversationSectionRangeRebuilds").toULongLong();
  const qulonglong heightRebuilds =
      view.property("conversationHeightIndexRebuilds").toULongLong();
  const qulonglong modelRebuilds = view.conversationModel()
                                       ->property("modelIndexRebuildCount")
                                       .toULongLong();
  const qulonglong modelResets =
      view.conversationModel()->property("modelResetCount").toULongLong();
  const qulonglong sectionRows =
      view.conversationModel()
          ->property("modelSectionStructureRowsTouched")
          .toULongLong();
  const qulonglong exactInserts =
      view.conversationModel()->property("modelExactInsertCount").toULongLong();
  const qulonglong exactRemovals =
      view.conversationModel()->property("modelExactRemoveCount").toULongLong();
  int falseAcknowledgements = 0;
  view.setPromptMaterializedAction([&](nodegraph::NodeRef) {
    ++falseAcknowledgements;
    return true;
  });

  VisibleCardData newRoot{AuthoritativeItemKey{thread, turn, "new-root"},
                          CardKind::UserMessage,
                          thread,
                          turn,
                          "new-root",
                          UserMessageData{"Stable root", {}}};
  newRoot.target = newRootTarget;
  ConversationDelta delta;
  delta.threadId = thread;
  delta.removals.push_back(oldRootTarget);
  delta.rows.push_back({{newRoot, sectionKey, true, false, true},
                        {},
                        AuthoritativeItemKey{thread, turn, "suffix-1"}});
  const auto applied = view.applyConversationDelta(std::move(delta));
  settle();

  const QModelIndex replacement =
      view.conversationModel()->indexForTarget(newRootTarget);
  result &= expect(
      applied && replacement.row() == 0 &&
          replacement.data(ConversationItemModel::TurnRootRole).toBool() &&
          !view.conversationModel()->indexForTarget(oldRootTarget).isValid() &&
          view.conversationModel()->rowCount() == Count &&
          falseAcknowledgements == 0,
      "root A was not retired for B exactly, or emitted a false prompt "
      "acknowledgement");
  result &= expect(
      persistentAnchor.isValid() && persistentAnchor == anchorIndex &&
          persistentLast.isValid() && persistentLast.row() == Count - 1 &&
          persistentLast.data(ConversationItemModel::StableKeyRole)
                  .toString()
                  .toStdString() == lastKey &&
          persistentAnchor.data(ConversationItemModel::NestedCardRole).toBool(),
      "root replacement changed a loaded model identity or role");
  result &= expect(
      retainedCard && retainedBody &&
          materializedCard(view, anchorBefore.first) == retainedCard &&
          retainedCard->geometry() == geometryBefore &&
          retainedBody->document() == retainedDocument &&
          retainedBody->textCursor().anchor() == selectionBefore.anchor() &&
          retainedBody->textCursor().position() == selectionBefore.position() &&
          QApplication::focusWidget() == focusBefore &&
          view.currentIndex() == persistentAnchor &&
          view.selectionModel()->isSelected(persistentAnchor),
      "root replacement changed the resident renderer, document, focus, or "
      "selection");
  result &= expect(firstVisible(view) == transactionAnchor,
                   "root replacement preserves the exact viewport anchor");
  result &= expect(view.viewport()->grab().toImage() == pixelsBefore,
                   "offscreen root replacement preserves settled pixels");
  result &= expect(view.materializedCardCount() == cardsBefore,
                   "root replacement preserves resident-card count");
  result &=
      expect(view.findChildren<MarkdownTextView *>().size() == documentsBefore,
             "root replacement preserves resident-document count");
  result &=
      expect(view.property("conversationCardConstructions").toULongLong() ==
                 constructions,
             "root replacement constructs no unrelated resident card");
  result &= expect(view.property("conversationRowsReleased").toULongLong() ==
                       releases,
                   "root replacement releases no unrelated loaded card");
  result &= expect(
      view.property("graphRefreshPasses").toULongLong() == graphPasses + 1 &&
          view.property("conversationMaterializationPasses").toULongLong() ==
              materializationPasses + 1 &&
          view.property("conversationLocalGeometryPasses").toULongLong() ==
              geometryPasses &&
          view.property("conversationSectionRangeRebuilds").toULongLong() ==
              sectionRebuilds &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              heightRebuilds &&
          view.conversationModel()
                  ->property("modelIndexRebuildCount")
                  .toULongLong() == modelRebuilds &&
          view.conversationModel()->property("modelResetCount").toULongLong() ==
              modelResets &&
          view.conversationModel()
                      ->property("modelSectionStructureRowsTouched")
                      .toULongLong() -
                  sectionRows <=
              7 &&
          view.conversationModel()
                  ->property("modelExactInsertCount")
                  .toULongLong() == exactInserts + 1 &&
          view.conversationModel()
                  ->property("modelExactRemoveCount")
                  .toULongLong() == exactRemovals + 1,
      "root replacement did not settle once with bounded model and geometry "
      "work");
  return result;
}

bool rootReplacementRecomputesFoldedSectionGeometry() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef oldRootTarget;
  nodegraph::NodeRef newRootTarget;
  {
    auto write = graph.write();
    oldRootTarget = write.upsert({nodegraph::NodeKind::Item, "fold-old-root"});
    newRootTarget = write.upsert({nodegraph::NodeKind::Item, "fold-new-root"});
    static_cast<void>(write.finish());
  }

  const std::string thread = "fold-root-thread";
  const std::string turn = "fold-root-turn";
  const std::string sectionKey = "fold-root-section";
  VisibleCardData oldRoot{AuthoritativeItemKey{thread, turn, "old-root"},
                          CardKind::UserMessage,
                          thread,
                          turn,
                          "old-root",
                          UserMessageData{"Fold this turn", {}}};
  oldRoot.target = oldRootTarget;
  TurnSection section;
  section.key = sectionKey;
  section.turnId = turn;
  section.rootCardKey = oldRoot.key;
  section.cards.push_back(oldRoot);
  for (int serial = 0; serial < 4; ++serial) {
    const std::string item = "child-" + std::to_string(serial);
    section.cards.push_back({AuthoritativeItemKey{thread, turn, item},
                             CardKind::AgentMessage, thread, turn, item,
                             AgentMessageData{"Answer " + item, true}});
  }
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.activeTurnId = turn;
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(820, 720);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "the folded-root replacement fixture reconciles");
  settle();
  ConversationCard *const oldCard =
      materializedCard(view, stableKey(oldRoot.key));
  QToolButton *const disclosure =
      oldCard ? oldCard->findChild<QToolButton *>(
                    QStringLiteral("cardDisclosureButton"))
              : nullptr;
  if (disclosure)
    disclosure->click();
  settle();
  bool childrenHidden = oldCard && oldCard->isCollapsed();
  for (int row = 1; row < view.conversationModel()->rowCount(); ++row)
    childrenHidden =
        childrenHidden &&
        view.visualRect(view.conversationModel()->index(row)).isEmpty();
  result &= expect(disclosure && childrenHidden,
                   "folding the old root hides every nested row");

  const qulonglong heightRebuilds =
      view.property("conversationHeightIndexRebuilds").toULongLong();
  VisibleCardData newRoot{AuthoritativeItemKey{thread, turn, "new-root"},
                          CardKind::UserMessage,
                          thread,
                          turn,
                          "new-root",
                          UserMessageData{"Replacement root", {}}};
  newRoot.target = newRootTarget;
  ConversationDelta delta;
  delta.threadId = thread;
  delta.removals.push_back(oldRootTarget);
  delta.rows.push_back({{newRoot, sectionKey, true, false, true},
                        {},
                        AuthoritativeItemKey{thread, turn, "child-0"}});
  const auto applied = view.applyConversationDelta(std::move(delta));
  settle();

  ConversationCard *const newCard =
      materializedCard(view, stableKey(newRoot.key));
  bool childrenVisible = true;
  int previousBottom = -1;
  for (int row = 1; row < view.conversationModel()->rowCount(); ++row) {
    const QModelIndex index = view.conversationModel()->index(row);
    const QRect geometry = view.visualRect(index);
    childrenVisible =
        childrenVisible && geometry.height() > 0 &&
        index.data(ConversationItemModel::NestedCardRole).toBool() &&
        geometry.top() > previousBottom;
    previousBottom = geometry.bottom();
  }
  result &= expect(
      applied && view.conversationModel()->rowCount() == 5 &&
          view.conversationModel()->indexForTarget(newRootTarget).row() == 0 &&
          !view.conversationModel()->indexForTarget(oldRootTarget).isValid() &&
          newCard && !newCard->isCollapsed() && childrenVisible &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              heightRebuilds,
      "a distinct expanded root did not restore every nested scalar extent");
  return result;
}

bool prefixGeometryChangeRepaintsFollowingTurnSurface() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef prefixTarget;
  {
    auto write = graph.write();
    prefixTarget = write.upsert({nodegraph::NodeKind::Item, "surface-prefix"});
    static_cast<void>(write.finish());
  }
  const std::string thread = "surface-shift-thread";
  VisibleCardData prefix{AuthoritativeItemKey{thread, "prefix-turn", "prefix"},
                         CardKind::AgentMessage,
                         thread,
                         "prefix-turn",
                         "prefix",
                         AgentMessageData{"Short prefix", true}};
  prefix.target = prefixTarget;
  VisibleCardData root{AuthoritativeItemKey{thread, "painted-turn", "root"},
                       CardKind::UserMessage,
                       thread,
                       "painted-turn",
                       "root",
                       UserMessageData{"Question", {}}};
  VisibleCardData child{AuthoritativeItemKey{thread, "painted-turn", "child"},
                        CardKind::AgentMessage,
                        thread,
                        "painted-turn",
                        "child",
                        AgentMessageData{"Answer", true}};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back(
      {"prefix-section", "prefix-turn", {prefix}, std::nullopt});
  snapshot.sections.push_back(
      {"painted-section", "painted-turn", {root, child}, root.key});

  ConversationView view;
  view.resize(820, 520);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "the shifted-surface fixture reconciles");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  const QModelIndex rootIndex =
      view.conversationModel()->indexForStableKey(stableKey(root.key));
  const QModelIndex childIndex =
      view.conversationModel()->indexForStableKey(stableKey(child.key));
  const QRect oldRoot = view.visualRect(rootIndex);
  const QRect oldChild = view.visualRect(childIndex);
  const QPoint oldBottomBorder(1, oldChild.bottom() + 9);
  result &= expect(oldRoot.isValid() && oldChild.isValid() &&
                       view.viewport()->rect().contains(oldBottomBorder) &&
                       view.mode() == ConversationView::Mode::Paused,
                   "the following Turn surface begins visible and paused");

  VisibleCardData tallPrefix = prefix;
  std::get<AgentMessageData>(tallPrefix.payload).text =
      "A taller prefix\n\nParagraph";
  ConversationRowChange update;
  update.placement = {std::move(tallPrefix), "prefix-section", false, false,
                      false};
  update.nextCardKey = root.key;
  ViewportPaintRegionProbe paintProbe(view.viewport());
  paintProbe.start();
  const bool applied = applyStructural(view, std::move(update));
  settle();
  const QRegion painted = paintProbe.stop();
  const QRect newRoot = view.visualRect(rootIndex);
  const QRect newChild = view.visualRect(childIndex);
  const QPoint newBottomBorder(1, newChild.bottom() + 9);
  if (!painted.contains(oldBottomBorder) ||
      (view.viewport()->rect().contains(newBottomBorder) &&
       !painted.contains(newBottomBorder)))
    std::cerr << "shifted Turn damage=" << painted.boundingRect().x() << ','
              << painted.boundingRect().y() << ' '
              << painted.boundingRect().width() << 'x'
              << painted.boundingRect().height()
              << " oldBottom=" << oldBottomBorder.y()
              << " newBottom=" << newBottomBorder.y() << '\n';
  result &= expect(
      applied && newRoot.top() > oldRoot.top() &&
          newChild.top() > oldChild.top() &&
          painted.contains(oldBottomBorder) &&
          (!view.viewport()->rect().contains(newBottomBorder) ||
           painted.contains(newBottomBorder)),
      "a prefix extent change did not repaint the old and new Turn enclosure");
  return result;
}

bool rightAnchoredCoalescedRowsMatchCanonicalOrder() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef rootTarget;
  nodegraph::NodeRef rightTarget;
  nodegraph::NodeRef movedTarget;
  nodegraph::NodeRef insertedTarget;
  nodegraph::NodeRef replacementTarget;
  {
    auto write = graph.write();
    rootTarget = write.upsert({nodegraph::NodeKind::Item, "boundary-root"});
    rightTarget = write.upsert({nodegraph::NodeKind::Item, "boundary-right"});
    movedTarget = write.upsert({nodegraph::NodeKind::Item, "boundary-moved"});
    insertedTarget =
        write.upsert({nodegraph::NodeKind::Item, "boundary-inserted"});
    replacementTarget =
        write.upsert({nodegraph::NodeKind::Item, "boundary-replacement"});
    static_cast<void>(write.finish());
  }

  const std::string thread = "boundary-thread";
  const std::string turn = "boundary-turn";
  const std::string sectionKey = "boundary-section";
  const auto agent = [&](std::string id, nodegraph::NodeRef target) {
    VisibleCardData card{AuthoritativeItemKey{thread, turn, id},
                         CardKind::AgentMessage,
                         thread,
                         turn,
                         id,
                         AgentMessageData{id, true}};
    card.target = std::move(target);
    return card;
  };
  VisibleCardData root{AuthoritativeItemKey{thread, turn, "root"},
                       CardKind::UserMessage,
                       thread,
                       turn,
                       "root",
                       UserMessageData{"Pinned root", {}}};
  root.target = rootTarget;
  VisibleCardData right = agent("right", rightTarget);
  VisibleCardData moved = agent("moved", movedTarget);
  VisibleCardData inserted = agent("inserted", insertedTarget);

  ConversationSnapshot initial;
  initial.threadId = thread;
  initial.sections.push_back(
      {sectionKey, turn, {root, right, moved}, root.key});
  ConversationView view;
  view.resize(820, 480);
  view.show();
  bool result = expect(changed(view.reconcile(initial)),
                       "the right-anchored coalescing fixture reconciles");
  settle();
  ConversationCard *const movedCard =
      materializedCard(view, stableKey(moved.key));
  const qulonglong resets =
      view.conversationModel()->property("modelResetCount").toULongLong();
  const qulonglong inserts =
      view.conversationModel()->property("modelExactInsertCount").toULongLong();
  const qulonglong moves =
      view.conversationModel()->property("modelExactMoveCount").toULongLong();

  ConversationDelta delta;
  delta.threadId = thread;
  delta.rows.push_back(
      {{moved, sectionKey, false, true, false}, root.key, inserted.key});
  delta.rows.push_back(
      {{inserted, sectionKey, false, true, false}, moved.key, right.key});
  result &= expect(view.applyConversationDelta(std::move(delta)).has_value(),
                   "a right-anchored changed component commits atomically");
  settle();

  const std::array<CardKey, 4> expected{root.key, moved.key, inserted.key,
                                        right.key};
  for (int row = 0; result && row < static_cast<int>(expected.size()); ++row) {
    const VisibleCardData *card = view.conversationModel()->card(row);
    result &=
        expect(card && card->key == expected[static_cast<std::size_t>(row)],
               "a right-anchored delta diverged from canonical order");
  }
  result &= expect(
      view.conversationModel()->rowCount() == 4 &&
          view.conversationModel()->property("modelResetCount").toULongLong() ==
              resets &&
          view.conversationModel()
                  ->property("modelExactInsertCount")
                  .toULongLong() == inserts + 1 &&
          view.conversationModel()
                  ->property("modelExactMoveCount")
                  .toULongLong() == moves + 1 &&
          materializedCard(view, stableKey(moved.key)) == movedCard,
      "right-anchored repair preserves the renderer while using "
      "one exact insert and move");

  ConversationView tailView;
  tailView.resize(820, 480);
  tailView.show();
  ConversationSnapshot tailInitial;
  tailInitial.threadId = thread;
  tailInitial.sections.push_back({sectionKey, turn, {root, right}, root.key});
  result &= expect(changed(tailView.reconcile(tailInitial)),
                   "the replacement-tail fixture reconciles");
  settle();
  const qulonglong tailAppends = tailView.conversationModel()
                                     ->property("modelTailAppendCount")
                                     .toULongLong();
  VisibleCardData replacement = agent("replacement", replacementTarget);
  ConversationDelta tailDelta;
  tailDelta.threadId = thread;
  tailDelta.removals.push_back(rightTarget);
  tailDelta.rows.push_back(
      {{replacement, sectionKey, false, true, false}, root.key, {}});
  result &= expect(
      tailView.applyConversationDelta(std::move(tailDelta)).has_value() &&
          tailView.conversationModel()->rowCount() == 2 &&
          tailView.conversationModel()->card(1)->key == replacement.key &&
          tailView.conversationModel()
                  ->property("modelTailAppendCount")
                  .toULongLong() == tailAppends + 1,
      "a replacement tail follows its represented canonical predecessor");
  return result;
}

bool boundedTailAppendIsViewportProportional() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  bool result = expect(changed(view.reconcile(conversation(10'000))),
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

  ConversationRowPlacement tail;
  tail.card = message(10'000);
  tail.sectionKey = "section-10000";
  const std::string tailKey = stableKey(tail.card.key);
  result &= expect(appendProjected(view, std::move(tail)),
                   "canonical tail append was accepted");
  settle();
  const auto anchorAfter = firstVisible(view);
  result &= expect(
      view.conversationModel()->rowCount() == 10'001 &&
          view.conversationModel()->indexForStableKey(tailKey).row() == 10'000,
      "paused tail append retained every loaded identity exactly once");
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
  result &= expect(changed(following.reconcile(conversation(80))),
                   "following-tail fixture reconciles");
  settle();
  ConversationRowPlacement followingTail;
  followingTail.card = message(80);
  followingTail.sectionKey = "section-80";
  const std::string followingKey = stableKey(followingTail.card.key);
  result &= expect(appendProjected(following, std::move(followingTail)),
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
  result &= expect(changed(rootedView.reconcile(std::move(rooted))),
                   "rooted bounded-tail fixture reconciles");
  settle();
  const qulonglong rootedRebuilds = rootedView.conversationModel()
                                        ->property("modelIndexRebuildCount")
                                        .toULongLong();
  const auto appendNested = [&rootedView](std::size_t serial) {
    ConversationRowPlacement nestedTail;
    nestedTail.card.key = AuthoritativeItemKey{
        "virtual-thread", "rooted-turn", "rooted-" + std::to_string(serial)};
    nestedTail.card.kind = CardKind::AgentMessage;
    nestedTail.card.threadId = "virtual-thread";
    nestedTail.card.turnId = "rooted-turn";
    nestedTail.card.itemId = "rooted-" + std::to_string(serial);
    nestedTail.card.payload = AgentMessageData{"Nested tail", true};
    nestedTail.sectionKey = "rooted-section";
    nestedTail.nested = true;
    return appendProjected(rootedView, std::move(nestedTail));
  };
  result &= expect(appendNested(80) && appendNested(81),
                   "root-pinned nested tail appends were accepted");
  settle();
  result &=
      expect(rootedView.conversationModel()->rowCount() == 82 &&
                 rootedView.conversationModel()
                         ->indexForStableKey(
                             "item:14:virtual-thread11:rooted-turn8:rooted-0")
                         .row() == 0 &&
                 rootedView.conversationModel()
                         ->indexForStableKey(
                             "item:14:virtual-thread11:rooted-turn8:rooted-2")
                         .row() == 2 &&
                 rootedView.conversationModel()
                         ->property("modelIndexRebuildCount")
                         .toULongLong() == rootedRebuilds &&
                 rootedView.isAtBottom(),
             "complete Turn membership lost identity, rebuilt history, or "
             "stopped following");
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
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "a long thread with one filtered update reconciles");
  settle();
  result &= expect(waitForResidency(view),
                   "visibility fixture reaches its admission fixed point");
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
  const auto impact = applyPresentation(view, finalAnswer);
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
  if (!localVisibilityPass)
    std::cerr
        << "targeted visibility diagnostics: impact="
        << (impact ? static_cast<int>(*impact) : -1) << " presented="
        << index.data(ConversationItemModel::PresentedRole).toBool()
        << " section_rebuilds="
        << view.property("conversationSectionRangeRebuilds").toULongLong()
        << '/' << sectionRebuilds << " index_rebuilds="
        << view.conversationModel()
               ->property("modelIndexRebuildCount")
               .toULongLong()
        << '/' << indexRebuilds << " constructions="
        << view.property("conversationCardConstructions").toULongLong() << '/'
        << constructions << " height_steps="
        << view.property("conversationHeightIndexUpdateSteps").toULongLong()
        << '\n';
  result &= expect(
      localVisibilityPass,
      "final-answer visibility updates only its row and logarithmic height "
      "index");

  const qulonglong commits = view.property("targetedCardCommits").toULongLong();
  result &=
      expect(applyPresentation(view, std::move(finalAnswer)) ==
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
  bool result =
      expect(changed(view.reconcile(initial)), "initial page is visible");
  settle();
  const int rowsBefore = view.conversationModel()->rowCount();
  const qulonglong resetsBefore =
      view.conversationModel()->property("modelResetCount").toULongLong();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  const auto anchorBeforePage = firstVisible(view);
  const int widgetsBefore = view.materializedCardCount();
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !anchorBeforePage.first.empty(),
                   "Load 80 begins from an explicit paused viewport anchor");

  ConversationSnapshot loaded = conversation(160);
  loaded.hasMore = true;
  static_cast<void>(view.reconcileStaged(std::move(loaded)));
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
  result &= expect(
      !view.structuralStagingActive() &&
          view.conversationModel()->rowCount() == 160 &&
          view.conversationModel()->property("modelResetCount").toULongLong() ==
              resetsBefore &&
          view.materializedCardCount() <= 48,
      "Load 80 commits one complete virtualized frame without a "
      "model reset");
  result &= expect(firstVisible(view) == anchorBeforePage,
                   "Load 80 preserves the exact paused row and pixel offset");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  settle();

  ConversationSnapshot appended = conversation(161);
  static_cast<void>(view.reconcileStaged(std::move(appended)));
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
  bool result = expect(changed(view.reconcile(initial)),
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
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  bool publicationObserved = false;
  qulonglong publicationConstructions = 0;
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId, ConversationView::ReconciliationResult,
          bool) {
        if (threadId != "virtual-thread")
          return;
        publicationObserved = true;
        publicationConstructions =
            view.property("conversationCardConstructions").toULongLong();
      });
  static_cast<void>(view.reconcileStaged(std::move(loaded)));
  result &=
      expect(waitUntil([&] { return !view.structuralStagingActive(); }, 5000),
             "the pinned-root history page commits");
  settle();
  const QModelIndex retainedAfter =
      view.conversationModel()->indexForStableKey(stableKey(
          AuthoritativeItemKey{"virtual-thread", "shared-turn", "item-80"}));
  result &= expect(retainedAfter.isValid() &&
                       view.visualRect(retainedAfter).top() == retainedTop,
                   "paging anchors the first retained activity rather than "
                   "the root pinned outside the old history window");
  result &= expect(publicationObserved,
                   "the positive-offset Turn-child anchor is observed at "
                   "publication");
  result &= expect(publicationConstructions == constructions,
                   "the positive-offset Turn-child anchor publishes without "
                   "synchronous fallback construction");
  return result;
}

bool removedInactiveAnchorUsesBoundedVisibleFallback() {
  ConversationView view;
  view.resize(820, 420);
  view.show();
  ConversationSnapshot target = conversation(200);
  bool result =
      expect(changed(view.reconcile(target)) && waitForResidency(view),
             "the removed-anchor target reaches stable residency");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  view.scrollTo(view.conversationModel()->index(100),
                QAbstractItemView::PositionAtTop);
  settle();
  const auto removedAnchor = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !removedAnchor.first.empty(),
                   "the inactive target records a paused middle anchor");
  result &= expect(changed(view.reconcile(singleMessageConversation(
                       "removed-anchor-source", "Source"))),
                   "switching away retains the paused target viewport state");

  std::erase_if(target.sections, [&](const TurnSection &section) {
    return !section.cards.empty() &&
           stableKey(section.cards.front().key) == removedAnchor.first;
  });
  const std::string targetThread = target.threadId;
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  bool publicationObserved = false;
  bool publicationCovered = false;
  qulonglong publicationConstructions = 0;
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId, ConversationView::ReconciliationResult,
          bool) {
        if (threadId != targetThread)
          return;
        publicationObserved = true;
        publicationCovered = viewportCovered(view);
        publicationConstructions =
            view.property("conversationCardConstructions").toULongLong();
      });
  result &= expect(admitted(view.reconcileStaged(std::move(target))),
                   "the anchor-retiring authority snapshot is admitted");
  result &= expect(waitUntil([&] { return publicationObserved; }, 3000),
                   "the anchor-retiring snapshot reaches publication");
  const qulonglong synchronousConstructions =
      publicationConstructions - constructions;
  const qulonglong frameBudget = static_cast<qulonglong>(
      (std::max(1, view.viewport()->height()) + 51) / 52 + 2);
  result &= expect(publicationCovered,
                   "the absent-anchor fallback publishes a complete viewport");
  result &= expect(view.mode() == ConversationView::Mode::Paused,
                   "the absent-anchor fallback preserves paused mode");
  result &=
      expect(!view.conversationModel()
                  ->indexForStableKey(removedAnchor.first)
                  .isValid(),
             "the absent-anchor fallback does not resurrect the retired row");
  result &= expect(
      synchronousConstructions > 0 && synchronousConstructions <= frameBudget,
      "an absent inactive anchor uses the visible-only bounded correctness "
      "fallback rather than staging another geometry authority");
  result &= expect(waitForResidency(view),
                   "the anchor fallback resumes one-card overscan admission");
  return result;
}

bool loadedYouLandmarksStayAccessibleAndVirtualized() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "semantic-landmark-thread";
  TurnSection section;
  section.key = "semantic-landmark-section";
  section.turnId = "semantic-landmark-turn";
  VisibleCardData root{
      AuthoritativeItemKey{snapshot.threadId, section.turnId, "root"},
      CardKind::UserMessage,
      snapshot.threadId,
      section.turnId,
      "root",
      UserMessageData{"Opening You landmark"}};
  VisibleCardData steering{
      AuthoritativeItemKey{snapshot.threadId, section.turnId, "steering"},
      CardKind::UserMessage,
      snapshot.threadId,
      section.turnId,
      "steering",
      UserMessageData{"Older acknowledged You landmark"}};
  section.rootCardKey = root.key;
  section.cards.push_back(root);
  section.cards.push_back(steering);
  for (int serial = 0; serial < 80; ++serial) {
    const std::string suffix = std::to_string(serial);
    section.cards.push_back(
        {AuthoritativeItemKey{snapshot.threadId, section.turnId,
                              "activity-" + suffix},
         CardKind::AgentMessage, snapshot.threadId, section.turnId,
         "activity-" + suffix, AgentMessageData{"Activity " + suffix, true}});
  }
  snapshot.sections.push_back(std::move(section));
  snapshot.hasMore = true;

  ConversationView view;
  view.resize(760, 360);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "semantic You landmark fixture reconciles");
  settle();
  const std::string rootKey = stableKey(root.key);
  const std::string steeringKey = stableKey(steering.key);
  const QModelIndex rootIndex =
      view.conversationModel()->indexForStableKey(rootKey);
  const QModelIndex steeringIndex =
      view.conversationModel()->indexForStableKey(steeringKey);
  result &= expect(
      rootIndex.isValid() && steeringIndex.isValid() &&
          view.conversationModel()->rowCount() == 82 &&
          steeringIndex.data(Qt::AccessibleTextRole)
              .toString()
              .contains(QStringLiteral("Older acknowledged You landmark")) &&
          view.visualRect(rootIndex).height() > 0 &&
          view.visualRect(steeringIndex).height() > 0 &&
          view.materializedCardCount() <= 48,
      "offscreen You landmarks lost model identity, accessible content, "
      "scalar geometry, or bounded residency");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  result &=
      expect(view.mode() == ConversationView::Mode::Paused &&
                 view.conversationModel()->rowCount() == 82,
             "scrolling preserves complete loaded membership without changing "
             "authority");
  ConversationCard *materialized = nullptr;
  for (ConversationCard *candidate : view.findChildren<ConversationCard *>())
    if (stableKey(candidate->data().key) == steeringKey)
      materialized = candidate;
  result &= expect(
      materialized && materialized->data() == steering &&
          view.visualRect(steeringIndex).intersects(view.viewport()->rect()) &&
          view.materializedCardCount() <= 48,
      "scrolling to a retained You landmark did not materialize its exact "
      "authoritative ConversationCard");

  ConversationSnapshot other = conversation(8);
  result &= expect(changed(view.reconcile(std::move(other))) &&
                       changed(view.reconcile(std::move(snapshot))),
                   "a thread round trip did not restore loaded You landmarks");
  settle();
  return result &&
         expect(view.conversationModel()
                        ->indexForStableKey(steeringKey)
                        .isValid() &&
                    view.materializedCardCount() <= 48,
                "thread restoration lost the You landmark or residency bound");
}

bool semanticRootsRemainTheirOwnViewportAnchors() {
  const auto localPromptPage = [](std::size_t prefixCount) {
    ConversationSnapshot snapshot;
    snapshot.threadId = "semantic-local-anchor";
    for (std::size_t index = 0; index < prefixCount; ++index) {
      const std::string suffix = std::to_string(index);
      VisibleCardData history{
          AuthoritativeItemKey{snapshot.threadId, "history-turn-" + suffix,
                               "history-item-" + suffix},
          CardKind::AgentMessage,
          snapshot.threadId,
          "history-turn-" + suffix,
          "history-item-" + suffix,
          AgentMessageData{"Earlier answer " + suffix, true}};
      snapshot.sections.push_back({"history-section-" + suffix,
                                   history.turnId,
                                   {std::move(history)},
                                   std::nullopt});
    }
    VisibleCardData prompt{
        LocalPromptKey{991}, CardKind::LocalPrompt,
        snapshot.threadId,   "active-turn",
        "local-prompt",      LocalPromptData{991, "Pending question"}};
    VisibleCardData activity{
        AuthoritativeItemKey{snapshot.threadId, "active-turn", "activity"},
        CardKind::AgentMessage,
        snapshot.threadId,
        "active-turn",
        "activity",
        AgentMessageData{"Streaming answer", false}};
    TurnSection active;
    active.key = "active-section";
    active.turnId = "active-turn";
    active.rootCardKey = prompt.key;
    active.cards = {std::move(prompt), std::move(activity)};
    snapshot.sections.push_back(std::move(active));
    for (std::size_t index = 0; index < 30; ++index) {
      const std::string suffix = std::to_string(index);
      VisibleCardData tail{AuthoritativeItemKey{snapshot.threadId,
                                                "tail-turn-" + suffix,
                                                "tail-item-" + suffix},
                           CardKind::AgentMessage,
                           snapshot.threadId,
                           "tail-turn-" + suffix,
                           "tail-item-" + suffix,
                           AgentMessageData{"Later answer " + suffix, true}};
      snapshot.sections.push_back({"tail-section-" + suffix,
                                   tail.turnId,
                                   {std::move(tail)},
                                   std::nullopt});
    }
    return snapshot;
  };

  ConversationView localView;
  localView.resize(820, 420);
  localView.show();
  bool result = expect(changed(localView.reconcile(localPromptPage(0))),
                       "the local-prompt anchor fixture reconciles");
  localView.verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMinimum);
  settle();
  const auto localAnchor = firstVisible(localView);
  result &= expect(localAnchor.first == stableKey(LocalPromptKey{991}),
                   "the visible optimistic prompt owns its viewport anchor");
  result &= expect(changed(localView.reconcile(localPromptPage(8))),
                   "history can be inserted before an optimistic prompt");
  settle();
  result &=
      expect(firstVisible(localView) == localAnchor,
             "history insertion does not replace the local-prompt anchor");
  localView.resize(670, 420);
  settle();
  result &= expect(firstVisible(localView).first == localAnchor.first,
                   "width reflow preserves the local-prompt semantic anchor");

  const auto isolatedPinnedRootPage = [](std::size_t prefixCount) {
    ConversationSnapshot snapshot = conversation(prefixCount, 10'000);
    snapshot.threadId = "isolated-pinned-anchor";
    for (TurnSection &section : snapshot.sections) {
      section.turnId = "history-" + section.turnId;
      for (VisibleCardData &card : section.cards) {
        card.threadId = snapshot.threadId;
        card.turnId = section.turnId;
        card.key =
            AuthoritativeItemKey{snapshot.threadId, card.turnId, card.itemId};
      }
    }
    VisibleCardData root{
        AuthoritativeItemKey{snapshot.threadId, "retained-turn", "root"},
        CardKind::UserMessage,
        snapshot.threadId,
        "retained-turn",
        "root",
        UserMessageData{"Retained owner"}};
    TurnSection retained;
    retained.key = "retained-section";
    retained.turnId = "retained-turn";
    retained.rootCardKey = root.key;
    retained.cards.push_back(std::move(root));
    snapshot.sections.push_back(std::move(retained));
    for (std::size_t index = 0; index < 30; ++index) {
      const std::string suffix = std::to_string(index);
      VisibleCardData tail{
          AuthoritativeItemKey{snapshot.threadId, "after-turn-" + suffix,
                               "after-item-" + suffix},
          CardKind::AgentMessage,
          snapshot.threadId,
          "after-turn-" + suffix,
          "after-item-" + suffix,
          AgentMessageData{"After retained root " + suffix, true}};
      snapshot.sections.push_back({"after-section-" + suffix,
                                   tail.turnId,
                                   {std::move(tail)},
                                   std::nullopt});
    }
    return snapshot;
  };

  ConversationView pinnedView;
  pinnedView.resize(820, 420);
  pinnedView.show();
  result &= expect(changed(pinnedView.reconcile(isolatedPinnedRootPage(0))),
                   "the isolated retained-root fixture reconciles");
  pinnedView.verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMinimum);
  settle();
  const auto pinnedAnchor = firstVisible(pinnedView);
  result &= expect(pinnedAnchor.first ==
                       stableKey(AuthoritativeItemKey{"isolated-pinned-anchor",
                                                      "retained-turn", "root"}),
                   "the isolated retained root is visibly anchored");
  result &= expect(changed(pinnedView.reconcile(isolatedPinnedRootPage(8))),
                   "history can be inserted before an isolated retained root");
  settle();
  result &= expect(
      firstVisible(pinnedView) == pinnedAnchor,
      "a retained root never borrows an anchor from another turn section");

  const auto tallPinnedPage = [](std::size_t first, std::size_t count) {
    ConversationSnapshot snapshot = pinnedTurnPage(first, count);
    std::string tallPrompt;
    for (int line = 0; line < 80; ++line)
      tallPrompt += "A deliberately tall retained prompt line.\n";
    std::get<UserMessageData>(snapshot.sections.front().cards.front().payload)
        .text = std::move(tallPrompt);
    return snapshot;
  };
  ConversationView tallView;
  tallView.resize(520, 300);
  tallView.show();
  result &= expect(changed(tallView.reconcile(tallPinnedPage(80, 40))),
                   "the tall retained-root fixture reconciles");
  tallView.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  const auto tallAnchor = firstVisible(tallView);
  const QModelIndex firstActivity =
      tallView.conversationModel()->indexForStableKey(stableKey(
          AuthoritativeItemKey{"virtual-thread", "shared-turn", "item-80"}));
  result &= expect(
      tallAnchor.first == stableKey(AuthoritativeItemKey{
                              "virtual-thread", "shared-turn", "root"}) &&
          firstActivity.isValid() &&
          tallView.visualRect(firstActivity).top() >=
              tallView.viewport()->height(),
      "the first retained activity begins below the tall visible root");
  result &= expect(changed(tallView.reconcile(tallPinnedPage(1, 119))),
                   "history can expand beneath the tall retained root");
  settle();
  result &= expect(
      firstVisible(tallView) == tallAnchor,
      "an offscreen activity never replaces its visible retained-root anchor");
  return result;
}

bool providerPaginationControlReflectsGraphState() {
  ConversationView control;
  control.resize(820, 420);
  control.show();
  ConversationSnapshot available = conversation(1);
  available.hasMore = true;
  bool result = expect(changed(control.reconcile(std::move(available))),
                       "the history control fixture reconciles");
  settle();
  auto *button =
      control.findChild<QPushButton *>(QStringLiteral("conversationLoadMore"));
  int invocations = 0;
  control.setLoadMoreAction([&] { ++invocations; });
#if QT_CONFIG(accessibility)
  QAccessibleInterface *initialAccessible =
      button ? QAccessible::queryAccessibleInterface(button) : nullptr;
  QAccessibleActionInterface *initialActions =
      initialAccessible ? initialAccessible->actionInterface() : nullptr;
  if (initialActions)
    initialActions->doAction(QAccessibleActionInterface::pressAction());
#else
  if (button)
    button->click();
#endif
  result &= expect(waitUntil([&] { return invocations == 1; }, 500),
                   "the native accessible history press completes");
  result &= expect(button && button->isVisible() && button->isEnabled() &&
#if QT_CONFIG(accessibility)
                       initialActions &&
                       initialActions->actionNames().contains(
                           QAccessibleActionInterface::pressAction()) &&
#endif
                       invocations == 1,
                   "available history exposes one enabled native accessible "
                   "action");
  control.setHistoryRequestPending("virtual-thread", true);
  settle();
  const QRect pendingGeometry = button ? button->geometry() : QRect{};
  const QString pendingText = button ? button->text() : QString{};
#if QT_CONFIG(accessibility)
  QAccessibleInterface *accessible =
      button ? QAccessible::queryAccessibleInterface(button) : nullptr;
  QAccessibleInterface *listAccessible =
      QAccessible::queryAccessibleInterface(control.viewport());
  const QAccessible::State pendingState =
      listAccessible ? listAccessible->state() : QAccessible::State{};
#endif
  if (button)
    button->click();
  control.setHistoryRequestPending("virtual-thread", true);
  settle();
  result &= expect(
      button && button->isVisible() && !button->isEnabled() &&
          button->text() == QStringLiteral("Loading earlier activities") &&
          button->geometry() == pendingGeometry &&
          button->text() == pendingText && invocations == 1,
      "pending history is disabled and a repeated semantic no-op preserves "
      "its pixels and action count");
#if QT_CONFIG(accessibility)
  result &= expect(accessible, "the history control has an accessible object");
  result &= expect(pendingState.busy,
                   "the graph-pending history control is accessibly busy");
  result &= expect(accessible && accessible->state().disabled,
                   "the graph-pending history control is accessibly disabled");
  result &=
      expect(accessible && accessible->text(QAccessible::Name) ==
                               QStringLiteral("Loading earlier activities"),
             "the graph-pending history control has one accessible name");
#endif
  control.setHistoryRequestPending("unpresented-thread", true);
  result &= expect(button && !button->isEnabled(),
                   "another thread cannot clear the presented busy state");
  control.setHistoryRequestPending("virtual-thread", false);
  settle();
#if QT_CONFIG(accessibility)
  accessible = button ? QAccessible::queryAccessibleInterface(button) : nullptr;
#endif
  result &= expect(button && button->isEnabled() &&
                       button->text().startsWith(QStringLiteral("Load "))
#if QT_CONFIG(accessibility)
                       && accessible && listAccessible &&
                       !listAccessible->state().busy
#endif
                   ,
                   "retiring the graph Operation restores the same control");
  return result;
}

bool inactiveAuthorityReplacementRetiresCardState() {
  ConversationView view;
  view.resize(820, 420);
  view.show();
  const std::string commandKey =
      stableKey(AuthoritativeItemKey{"retained-a", "turn-0", "command-0"});
  bool result =
      expect(changed(view.reconcile(commandConversation("retained-a", 1))),
             "the retained-state source thread reconciles");
  settle();
  ConversationCard *command = materializedCard(view, commandKey);
  result &= expect(command && command->isCollapsed(),
                   "the source command begins at its configured default");
  if (command)
    command->setCollapsed(false);
  result &= expect(changed(view.reconcile(singleMessageConversation(
                       "retained-b", "Other conversation"))),
                   "switching away captures the command interaction state");

  ConversationSnapshot retiredA;
  retiredA.threadId = "retained-a";
  result &= expect(changed(view.reconcile(retiredA)),
                   "an inactive-thread authority replacement retires the row");
  result &=
      expect(changed(view.reconcile(singleMessageConversation(
                 "retained-b", "Other conversation"))),
             "the other thread can be restored after inactive retirement");
  result &=
      expect(changed(view.reconcile(commandConversation("retained-a", 1))),
             "the retired command identity can later be reintroduced");
  settle();
  command = materializedCard(view, commandKey);
  result &= expect(command && command->isCollapsed(),
                   "a removed inactive-thread key cannot resurrect stale "
                   "interaction state");
  return result;
}

bool threadIncarnationOwnsCompletePresentationState() {
  const std::string thread = "reused-presentation";
  const std::string key =
      stableKey(AuthoritativeItemKey{thread, "turn-0", "command-0"});
  ConversationView view;
  view.resize(820, 420);
  view.show();
  view.beginThreadSelection(thread, 10);
  bool result = expect(changed(view.reconcile(commandConversation(thread, 1))),
                       "the original presentation incarnation reconciles");
  settle();
  ConversationCard *oldCard = materializedCard(view, key);
  result &= expect(oldCard && oldCard->isCollapsed(),
                   "the original command begins at its canonical default");
  if (oldCard)
    oldCard->setCollapsed(false);
  QPointer<ConversationCard> retiredCard(oldCard);

  view.beginThreadSelection(thread, 11);
  result &= expect(changed(view.reconcile(commandConversation(thread, 1))),
                   "the same canonical id accepts a new node incarnation");
  settle();
  ConversationCard *newCard = materializedCard(view, key);
  result &=
      expect(retiredCard.isNull() && newCard && newCard->isCollapsed(),
             "the replacement cannot inherit the old renderer or fold state");
  if (newCard)
    newCard->setCollapsed(false);
  view.forgetThreadPresentation(thread, 10);
  settle();
  result &= expect(
      materializedCard(view, key) == newCard && newCard &&
          !newCard->isCollapsed() && view.conversationModel()->rowCount() == 1,
      "a delayed old retirement cannot erase or replace the new card");
  view.forgetThreadPresentation(thread, 11);
  result &=
      expect(view.conversationModel()->rowCount() == 0 &&
                 view.presentedThreadId().empty(),
             "the exact replacement retirement clears its complete state");
  return result;
}

bool disclosureDefaultsAreIndependentOfResidency() {
  const std::string thread = "disclosure-admission";
  ConversationSnapshot canonical = commandConversation(thread, 100);
  ConversationView early;
  ConversationView late;
  for (ConversationView *view : {&early, &late}) {
    view->resize(820, 420);
    view->show();
  }
  bool result = expect(changed(early.reconcile(canonical)) &&
                           changed(late.reconcile(canonical)),
                       "matching disclosure fixtures reconcile");
  const std::string firstKey =
      stableKey(AuthoritativeItemKey{thread, "turn-0", "command-0"});
  early.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  ConversationCard *firstEarly = materializedCard(early, firstKey);
  result &= expect(firstEarly && firstEarly->isCollapsed() &&
                       materializedCard(late, firstKey) == nullptr,
                   "only one fixture materializes the admitted first row");

  auto expanded = early.presentationOptions();
  expanded.commandsInitiallyExpanded = true;
  result &=
      expect(changed(late.reconcile(
                 singleMessageConversation("disclosure-other", "Other"))),
             "switching away freezes defaults for never-materialized rows");
  early.setPresentationOptions(expanded);
  late.setPresentationOptions(expanded);
  result &= expect(changed(late.reconcile(canonical)),
                   "the disclosure fixture returns after its default changes");
  early.verticalScrollBar()->setValue(early.verticalScrollBar()->maximum());
  early.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  late.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  firstEarly = materializedCard(early, firstKey);
  ConversationCard *firstLate = materializedCard(late, firstKey);
  const QModelIndex earlyIndex =
      early.conversationModel()->indexForStableKey(firstKey);
  const QModelIndex lateIndex =
      late.conversationModel()->indexForStableKey(firstKey);
  result &=
      expect(firstEarly && firstLate && firstEarly->isCollapsed() &&
                 firstLate->isCollapsed() &&
                 early.visualRect(earlyIndex).height() ==
                     late.visualRect(lateIndex).height(),
             "an admitted disclosure default is identical whether the row was "
             "resident before the preference changed");

  ConversationSnapshot enlarged = commandConversation(thread, 101);
  ConversationRowPlacement tail;
  tail.card = enlarged.sections.back().cards.front();
  tail.sectionKey = enlarged.sections.back().key;
  const std::string newKey = stableKey(tail.card.key);
  result &= expect(appendProjected(late, std::move(tail)),
                   "a command admitted under the new preference appends");
  late.scrollTo(late.conversationModel()->indexForStableKey(newKey),
                QAbstractItemView::PositionAtTop);
  settle();
  ConversationCard *newCommand = materializedCard(late, newKey);
  result &= expect(newCommand && !newCommand->isCollapsed(),
                   "a newly admitted command uses the current default");

  ConversationView staged;
  staged.resize(820, 420);
  staged.show();
  static_cast<void>(
      staged.reconcileStaged(commandConversation("disclosure-staged", 100)));
  auto stagedExpanded = staged.presentationOptions();
  stagedExpanded.commandsInitiallyExpanded = true;
  staged.setPresentationOptions(stagedExpanded);
  result &= expect(
      waitUntil([&staged] { return !staged.structuralStagingActive(); }, 3000),
      "the disclosure snapshot commits after its preference changes");
  staged.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  const std::string stagedKey = stableKey(
      AuthoritativeItemKey{"disclosure-staged", "turn-0", "command-0"});
  ConversationCard *stagedCommand = materializedCard(staged, stagedKey);
  result &= expect(
      stagedCommand && !stagedCommand->isCollapsed(),
      "all cards in one staged commit use its single admission-time default");
  result &= expect(early.materializedCardCount() <= 48 &&
                       late.materializedCardCount() <= 48 &&
                       staged.materializedCardCount() <= 48,
                   "disclosure authority does not expand widget residency");
  return result;
}

bool pendingVisibilityUsesTheCurrentPolicy() {
  const std::string thread = "pending-visibility";
  ConversationView view;
  view.resize(820, 420);
  view.show();
  bool result = expect(
      changed(view.reconcile(singleMessageConversation(thread, "Visible"))),
      "the pending-visibility fixture presents an unaffected current row");

  auto options = view.presentationOptions();
  options.showReasoning = false;
  view.setPresentationOptions(options);
  ConversationSnapshot pending;
  pending.threadId = thread;
  for (int row = 0; row < 80; ++row) {
    const std::string suffix = std::to_string(row);
    VisibleCardData reasoning{
        AuthoritativeItemKey{thread, "turn-" + suffix, "reasoning-" + suffix},
        CardKind::Reasoning,
        thread,
        "turn-" + suffix,
        "reasoning-" + suffix,
        ReasoningData{"Reasoning " + suffix}};
    pending.sections.push_back({"section-" + suffix,
                                reasoning.turnId,
                                {std::move(reasoning)},
                                std::nullopt});
  }

  const qulonglong stages =
      view.property("structuralStageCardPasses").toULongLong();
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  bool publicationObserved = false;
  qulonglong publicationConstructions = 0;
  view.setReconciliationFinishedAction(
      [&](const std::string &completedThread,
          ConversationView::ReconciliationResult, bool) {
        if (completedThread != thread)
          return;
        publicationObserved = viewportCovered(view);
        publicationConstructions =
            view.property("conversationCardConstructions").toULongLong();
      });
  result &= expect(admitted(view.reconcileStaged(std::move(pending))),
                   "the filtered pending frame is admitted");
  options.showReasoning = true;
  view.setPresentationOptions(options);
  result &= expect(
      waitUntil([&] { return publicationObserved; }, 3000) &&
          view.property("structuralStageCardPasses").toULongLong() > stages &&
          publicationConstructions == constructions,
      "the pending frame uses the new visibility policy even when no current "
      "row changes presentation");
  return result;
}

bool tallViewportPublishesOneFrameThenAdmitsOverscan() {
  ConversationView view;
  view.resize(820, 1600);
  view.show();
  const qulonglong stagePassesBefore =
      view.property("structuralStageCardPasses").toULongLong();
  const qulonglong constructionsBefore =
      view.property("conversationCardConstructions").toULongLong();
  bool publicationObserved = false;
  bool publicationValid = false;
  bool publicationCovered = false;
  bool stagingHostEmptyAtPublication = false;
  int publishedCardCount = 0;
  int publishedAccessibleChildren = 0;
  qulonglong publishedStagePasses = 0;
  qulonglong publishedConstructions = 0;
  qulonglong publishedAdmissions = 0;
  std::pair<std::string, int> publishedAnchor;
  QWidget *publishedFocus = nullptr;
  QImage publishedPixels;
  std::vector<std::pair<QPointer<ConversationCard>, QRect>> publishedCards;
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId,
          ConversationView::ReconciliationResult reconciliation,
          bool selectionCommitted) {
        if (threadId != "tall-stage")
          return;
        publicationObserved = true;
        publicationValid = changed(reconciliation) && selectionCommitted;
        publicationCovered = viewportCovered(view);
        publishedCardCount = view.materializedCardCount();
        publishedStagePasses =
            view.property("structuralStageCardPasses").toULongLong() -
            stagePassesBefore;
        publishedConstructions =
            view.property("conversationCardConstructions").toULongLong();
        publishedAdmissions =
            view.property("conversationCardAdmissionPasses").toULongLong();
        publishedAnchor = firstVisible(view);
        publishedFocus = QApplication::focusWidget();
        publishedPixels = view.viewport()->grab().toImage();
        for (ConversationCard *card :
             view.viewport()->findChildren<ConversationCard *>(
                 QString{}, Qt::FindDirectChildrenOnly)) {
          if (!card->isHidden())
            publishedCards.emplace_back(card, card->geometry());
        }
        if (QAccessibleInterface *list =
                QAccessible::queryAccessibleInterface(view.viewport()))
          publishedAccessibleChildren = list->childCount();
        if (QWidget *host = view.findChild<QWidget *>(
                QStringLiteral("conversationStagingHost"),
                Qt::FindDirectChildrenOnly))
          stagingHostEmptyAtPublication =
              host->findChildren<ConversationCard *>(QString{},
                                                     Qt::FindDirectChildrenOnly)
                  .isEmpty();
      });
  ConversationSnapshot snapshot = commandConversation("tall-stage", 200);
  static_cast<void>(view.reconcileStaged(std::move(snapshot)));
  QWidget *stagingHost = view.findChild<QWidget *>(
      QStringLiteral("conversationStagingHost"), Qt::FindDirectChildrenOnly);
  ConversationCard *stagedCard = nullptr;
  bool result = expect(
      waitUntil(
          [&] {
            if (!view.structuralStagingActive() || !stagingHost)
              return false;
            const auto cards = stagingHost->findChildren<ConversationCard *>(
                QString{}, Qt::FindDirectChildrenOnly);
            stagedCard = cards.empty() ? nullptr : cards.front();
            return stagedCard != nullptr;
          },
          2000),
      "the tall snapshot exposes one hidden future renderer while staging");
  if (!stagedCard)
    return false;
  QPointer<ConversationCard> stagedIdentity(stagedCard);
  const int stagedWidth = stagedCard->width();
  const int stagedHeight = stagedCard->height();
  view.resize(960, 1800);
  result &=
      expect(view.structuralStagingActive() && stagedIdentity == stagedCard &&
                 stagedCard->parentWidget() == stagingHost &&
                 stagedCard->isHidden() && stagedCard->width() != stagedWidth,
             "a resize reflows the same hidden future renderer without "
             "publishing it");
  const qulonglong environmentReflows =
      view.property("conversationGeometryEnvironmentReflows").toULongLong();
  view.setStyleSheet(QStringLiteral("* { font-size: 32pt; }"));
  QApplication::sendPostedEvents(&view, QEvent::MetaCall);
  result &= expect(
      view.structuralStagingActive() &&
          view.conversationModel()->rowCount() == 0 &&
          stagedIdentity == stagedCard &&
          stagedCard->parentWidget() == stagingHost && stagedCard->isHidden() &&
          stagedCard->height() != stagedHeight &&
          view.property("conversationGeometryEnvironmentReflows")
                  .toULongLong() == environmentReflows + 1,
      "an environment reflow remeasures the same hidden future renderer "
      "without publishing it");
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 3000),
      "the tall collapsed-card snapshot completes staging");
  const std::size_t stageBudget = static_cast<std::size_t>(
      (std::max(1, view.viewport()->height()) + 51) / 52 + 2);
  result &= expect(
      publicationObserved && publicationValid && publicationCovered &&
          stagingHostEmptyAtPublication && publishedCardCount > 0 &&
          publishedStagePasses >= static_cast<qulonglong>(publishedCardCount) &&
          publishedStagePasses <= stageBudget &&
          publishedConstructions == constructionsBefore,
      "publication contains one complete visible frame built by the bounded "
      "hidden stage without synchronous fallback construction");
  result &= expect(waitForResidency(view),
                   "deferred admission completes the tall overscan window");

  const QRect overscan = view.viewport()->rect().adjusted(
      0, -view.viewport()->height(), 0, view.viewport()->height());
  int expectedResidents = 0;
  bool residencyComplete = true;
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const QModelIndex index = view.conversationModel()->index(row);
    if (!view.visualRect(index).intersects(overscan))
      continue;
    ++expectedResidents;
    const auto *modelRow = view.conversationModel()->row(row);
    residencyComplete = residencyComplete && modelRow &&
                        materializedCard(view, modelRow->stableKey) != nullptr;
  }
  const qulonglong deferredConstructions =
      view.property("conversationCardConstructions").toULongLong() -
      publishedConstructions;
  const qulonglong deferredAdmissions =
      view.property("conversationCardAdmissionPasses").toULongLong() -
      publishedAdmissions;
  const bool publishedCardsStable =
      std::ranges::all_of(publishedCards, [](const auto &entry) {
        return entry.first && !entry.first->isHidden() &&
               entry.first->geometry() == entry.second;
      });
  int settledAccessibleChildren = 0;
  if (QAccessibleInterface *list =
          QAccessible::queryAccessibleInterface(view.viewport()))
    settledAccessibleChildren = list->childCount();
  result &= expect(
      residencyComplete && view.materializedCardCount() >= expectedResidents &&
          view.materializedCardCount() <= expectedResidents + 2 &&
          expectedResidents <= 100 && deferredConstructions > 0 &&
          deferredConstructions == deferredAdmissions &&
          view.materializedCardCount() > publishedCardCount &&
          publishedCardsStable && firstVisible(view) == publishedAnchor &&
          QApplication::focusWidget() == publishedFocus &&
          settledAccessibleChildren == publishedAccessibleChildren &&
          view.viewport()->grab().toImage() == publishedPixels,
      "one-card admission fills hidden overscan without moving pixels, "
      "replacing visible cards, or changing focus or accessibility");
  view.setStyleSheet({});
  return result;
}

bool stagedSnapshotsAreDeferredAndLatestWins() {
  ConversationView view;
  view.resize(820, 420);
  view.show();
  std::vector<std::string> completions;
  bool stageReturned = false;
  bool callbackBeforeReturn = false;
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId, ConversationView::ReconciliationResult,
          bool) {
        callbackBeforeReturn = !stageReturned;
        completions.push_back(threadId);
      });

  ConversationSnapshot empty;
  empty.threadId = "deferred-empty";
  const auto emptyDisposition = view.reconcileStaged(std::move(empty));
  stageReturned = true;
  bool result = expect(
      admitted(emptyDisposition) && view.structuralStagingActive() &&
          completions.empty(),
      "an admitted zero-card snapshot remains pending until the event loop");
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 1000) &&
          !callbackBeforeReturn &&
          completions == std::vector<std::string>{"deferred-empty"},
      "zero-card completion is asynchronous and published exactly once");

  const qulonglong resetsBefore =
      view.conversationModel()->property("modelResetCount").toULongLong();
  stageReturned = false;
  result &= expect(admitted(view.reconcileStaged(conversation(200))) &&
                       view.structuralStagingActive(),
                   "the first same-mailbox snapshot is admitted");
  result &= expect(
      admitted(view.reconcileStaged(
          singleMessageConversation("virtual-thread", "Newest authority"))) &&
          view.structuralStagingActive() &&
          view.conversationModel()->rowCount() == 0,
      "newer authority supersedes pending work before either frame commits");
  stageReturned = true;
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 1000) &&
          completions ==
              std::vector<std::string>{"deferred-empty", "virtual-thread"} &&
          view.conversationModel()->rowCount() == 1 &&
          view.conversationModel()->property("modelResetCount").toULongLong() ==
              resetsBefore + 1,
      "only the newest same-thread snapshot publishes one complete frame");
  const VisibleCardData *presented = view.conversationModel()->card(0);
  result &=
      expect(presented && std::get<AgentMessageData>(presented->payload).text ==
                              "Newest authority",
             "superseded rows never become the presented model authority");
  return result;
}

bool unchangedStageResumesDeferredOverscanAdmission() {
  ConversationView view;
  view.resize(820, 420);
  view.show();
  ConversationSnapshot snapshot = conversation(200);
  const std::string expectedThreadId = snapshot.threadId;
  bool result = expect(changed(view.reconcile(snapshot)),
                       "the unchanged-stage fixture publishes its first frame");

  bool publicationObserved = false;
  bool publicationUnchanged = false;
  bool overscanIncompleteAtPublication = false;
  qulonglong constructionsAtPublication = 0;
  qulonglong admissionsAtPublication = 0;
  view.setReconciliationFinishedAction(
      [&](const std::string &completedThread,
          ConversationView::ReconciliationResult reconciliation, bool) {
        if (completedThread != expectedThreadId)
          return;
        publicationObserved = true;
        publicationUnchanged =
            reconciliation == ConversationView::ReconciliationResult::Unchanged;
        overscanIncompleteAtPublication = !residencyWindowCovered(view);
        constructionsAtPublication =
            view.property("conversationCardConstructions").toULongLong();
        admissionsAtPublication =
            view.property("conversationCardAdmissionPasses").toULongLong();
      });
  result &= expect(admitted(view.reconcileStaged(std::move(snapshot))),
                   "an identical authority snapshot enters the existing stage");
  result &= expect(waitUntil([&] { return publicationObserved; }, 3000),
                   "the identical stage reaches its publication boundary");
  result &= expect(publicationUnchanged,
                   "the identical stage is a semantic model no-op");
  result &= expect(overscanIncompleteAtPublication,
                   "the unchanged stage publishes before hidden overscan");
  result &= expect(waitForResidency(view),
                   "the unchanged stage resumes deferred overscan admission");
  const qulonglong deferredConstructions =
      view.property("conversationCardConstructions").toULongLong() -
      constructionsAtPublication;
  const qulonglong deferredAdmissions =
      view.property("conversationCardAdmissionPasses").toULongLong() -
      admissionsAtPublication;
  result &= expect(
      deferredConstructions > 0 && deferredConstructions == deferredAdmissions,
      "resumed overscan admission constructs exactly one card per pass");
  return result;
}

bool delayedThreadSelectionSpinner() {
  ConversationView view;
  view.resize(820, 600);
  view.show();
  const ConversationSnapshot source =
      singleMessageConversation("spinner-source", "Outgoing conversation");
  bool result = expect(changed(view.reconcile(source)),
                       "spinner source conversation reconciles");
  settle();
  std::vector<std::string> committedThreads;
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId,
          ConversationView::ReconciliationResult outcome,
          bool selectionCommitted) {
        if (outcome != ConversationView::ReconciliationResult::Rejected &&
            selectionCommitted)
          committedThreads.push_back(threadId);
      });

  view.beginThreadSelection("spinner-slow-target");
  settle();
  auto *overlay =
      view.findChild<QWidget *>(QStringLiteral("conversationStagingOverlay"));
  const auto paintedSpinnerBounds = [overlay] {
    QRect pixels;
    if (!overlay)
      return pixels;
    const QImage frame = overlay->grab().toImage();
    const QColor background(QStringLiteral("#f6f8fb"));
    for (int y = 0; y < frame.height(); ++y)
      for (int x = 0; x < frame.width(); ++x)
        if (frame.pixelColor(x, y) != background)
          pixels |= QRect(x, y, 1, 1);
    return pixels;
  };
  result &= expect(
      overlay && overlay->isVisible() &&
          view.viewport()->childAt(view.viewport()->rect().center()) ==
              overlay &&
          paintedSpinnerBounds().isEmpty() &&
          view.conversationModel()
              ->indexForStableKey(stableKey(
                  AuthoritativeItemKey{"spinner-source", "turn", "message"}))
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
  result &= expect(overlay && paintedSpinnerBounds().isEmpty(),
                   "the first half-second of thread loading shows no spinner");
  result &=
      expect(overlay && waitUntil(
                            [&paintedSpinnerBounds] {
                              return !paintedSpinnerBounds().isEmpty();
                            },
                            350),
             "a slower thread load starts the gray spinner after its delay");
  const QRect spinnerPixels = paintedSpinnerBounds();
  qreal spinnerDpr = 1.0;
  if (overlay) {
    const QImage frame = overlay->grab().toImage();
    spinnerDpr = frame.devicePixelRatio();
  }
  const qreal expectedSpinnerExtent = 30.0 * spinnerDpr;
  const qreal extentTolerance = std::ceil(spinnerDpr);
  const QPointF expectedSpinnerCenter =
      overlay ? QRectF(overlay->rect()).center() * spinnerDpr : QPointF{};
  const QPointF paintedSpinnerCenter = QRectF(spinnerPixels).center();
  result &= expect(
      overlay &&
          std::abs(spinnerPixels.width() - expectedSpinnerExtent) <=
              extentTolerance &&
          std::abs(spinnerPixels.height() - expectedSpinnerExtent) <=
              extentTolerance &&
          std::abs(paintedSpinnerCenter.x() - expectedSpinnerCenter.x()) +
                  std::abs(paintedSpinnerCenter.y() -
                           expectedSpinnerCenter.y()) <=
              std::ceil(2.0 * spinnerDpr),
      "the painted gray ring is 30 logical pixels and physically centered");
  const QImage spinnerFrame = overlay ? overlay->grab().toImage() : QImage{};
  result &=
      expect(overlay && waitUntil(
                            [overlay, spinnerFrame] {
                              return overlay->grab().toImage() != spinnerFrame;
                            },
                            150),
             "the visible spinner advances while loading");

  static_cast<void>(view.reconcileStaged(singleMessageConversation(
      "spinner-slow-target", "Incoming conversation")));
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 500) &&
          overlay && !overlay->isVisible() &&
          view.conversationModel()
              ->indexForStableKey(stableKey(AuthoritativeItemKey{
                  "spinner-slow-target", "turn", "message"}))
              .isValid(),
      "the complete target frame atomically removes and stops the spinner");
  result &= expect(committedThreads ==
                       std::vector<std::string>{"spinner-slow-target"},
                   "the complete target frame publishes readiness once");

  view.beginThreadSelection("spinner-fast-target");
  static_cast<void>(view.reconcileStaged(
      singleMessageConversation("spinner-fast-target", "Fast conversation")));
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 500) &&
          overlay && !overlay->isVisible() &&
          committedThreads == std::vector<std::string>{"spinner-slow-target",
                                                       "spinner-fast-target"},
      "a fast staged selection clears and reveals without spinner motion");

  view.beginThreadSelection("spinner-stale-target");
  view.beginThreadSelection("spinner-final-target");
  const qulonglong ignoredBefore =
      view.property("staleThreadStagesIgnored").toULongLong();
  static_cast<void>(view.reconcileStaged(
      singleMessageConversation("spinner-stale-target", "Stale conversation")));
  result &= expect(
      view.property("staleThreadStagesIgnored").toULongLong() ==
              ignoredBefore + 1 &&
          overlay && overlay->isVisible() && committedThreads.size() == 2,
      "a superseded thread stage cannot reveal or stop the current load");
  static_cast<void>(view.reconcileStaged(
      singleMessageConversation("spinner-final-target", "Final conversation")));
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 500) &&
          overlay && !overlay->isVisible() &&
          view.conversationModel()
              ->indexForStableKey(stableKey(AuthoritativeItemKey{
                  "spinner-final-target", "turn", "message"}))
              .isValid() &&
          committedThreads == std::vector<std::string>{"spinner-slow-target",
                                                       "spinner-fast-target",
                                                       "spinner-final-target"},
      "the newest thread identity alone completes the loading surface");
  return result;
}

bool queuedAdmissionYieldsToThreadSelection() {
  bool result = true;
  {
    ConversationView loading;
    loading.resize(620, 360);
    loading.show();
    result &= expect(changed(loading.reconcile(conversation(240))),
                     "admission-suppression source reconciles");
    loading.verticalScrollBar()->setValue(
        loading.verticalScrollBar()->maximum() / 2);
    result &= expect(waitForResidency(loading),
                     "admission-suppression source reaches stable residency");
    loading.verticalScrollBar()->setValue(
        loading.verticalScrollBar()->minimum());
    const bool admissionQueued = !residencyWindowCovered(loading);
    const qulonglong outgoingConstructions =
        loading.property("conversationCardConstructions").toULongLong();
    loading.beginThreadSelection("loading-target");
    QThread::msleep(3);
    settle();
    result &= expect(
        admissionQueued && !residencyWindowCovered(loading) &&
            loading.property("conversationCardConstructions").toULongLong() ==
                outgoingConstructions,
        "thread loading suppresses queued outgoing overscan admission");
  }

  ConversationView priority;
  priority.resize(620, 360);
  priority.show();
  result &= expect(changed(priority.reconcile(conversation(240))),
                   "admission-priority source reconciles");
  priority.verticalScrollBar()->setValue(
      priority.verticalScrollBar()->maximum() / 2);
  result &= expect(waitForResidency(priority),
                   "admission-priority source reaches stable residency");
  priority.verticalScrollBar()->setValue(
      priority.verticalScrollBar()->minimum());
  const bool admissionQueued = !residencyWindowCovered(priority);
  const qulonglong outgoingConstructions =
      priority.property("conversationCardConstructions").toULongLong();
  const qulonglong stagePasses =
      priority.property("structuralStageCardPasses").toULongLong();
  priority.beginThreadSelection("admission-target");
  result &= expect(
      admissionQueued &&
          admitted(priority.reconcileStaged(singleMessageConversation(
              "admission-target", "Incoming conversation"))) &&
          waitUntil([&priority] { return !priority.structuralStagingActive(); },
                    1000),
      "target structural staging preempts stale generic admission");
  const bool targetResidency = waitForResidency(priority);
  const auto residentCards = priority.findChildren<ConversationCard *>();
  const VisibleCardData *presented = priority.conversationModel()->card(0);
  result &= expect(
      targetResidency && priority.presentedThreadId() == "admission-target" &&
          priority.conversationModel()->rowCount() == 1 && presented &&
          std::get<AgentMessageData>(presented->payload).text ==
              "Incoming conversation" &&
          priority.property("structuralStageCardPasses").toULongLong() ==
              stagePasses + 1 &&
          priority.property("conversationCardConstructions").toULongLong() ==
              outgoingConstructions &&
          !residentCards.empty() &&
          std::ranges::all_of(residentCards,
                              [](ConversationCard *card) {
                                return card->data().threadId ==
                                       "admission-target";
                              }),
      "the target commits from staged cards with no outgoing renderer left");
  return result;
}

bool rejectedStagesPreserveThePresentedFrame() {
  ConversationView view;
  view.resize(820, 420);
  view.show();
  const ConversationSnapshot source = conversation(60);
  bool result = expect(changed(view.reconcile(source)),
                       "rejected-stage source conversation reconciles");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.scrollTo(view.conversationModel()->index(30),
                QAbstractItemView::PositionAtCenter);
  view.setFocus(Qt::OtherFocusReason);
  settle();
  result &= expect(waitForResidency(view),
                   "the rejected-stage source reaches stable residency");

  struct Completion {
    std::string threadId;
    ConversationView::ReconciliationResult result =
        ConversationView::ReconciliationResult::Unchanged;
    bool selectionCommitted = false;
    bool stagingEmpty = false;
  };
  std::vector<Completion> completions;
  QWidget *const stagingHost =
      view.findChild<QWidget *>(QStringLiteral("conversationStagingHost"));
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId,
          ConversationView::ReconciliationResult outcome,
          bool selectionCommitted) {
        completions.push_back(
            {threadId, outcome, selectionCommitted,
             stagingHost && stagingHost
                                ->findChildren<ConversationCard *>(
                                    QString{}, Qt::FindDirectChildrenOnly)
                                .empty()});
      });

  const int rowsBefore = view.conversationModel()->rowCount();
  const auto anchorBefore = firstVisible(view);
  const auto cardsBefore = view.findChildren<ConversationCard *>();
  const std::string centerKey = view.conversationModel()
                                    ->index(30)
                                    .data(ConversationItemModel::StableKeyRole)
                                    .toString()
                                    .toStdString();
  ConversationCard *const centerCard = materializedCard(view, centerKey);
  const QRect centerGeometry = centerCard ? centerCard->geometry() : QRect{};
  QWidget *const focusBefore = QApplication::focusWidget();
  const QImage pixelsBefore = view.viewport()->grab().toImage();
  const qulonglong commitsBefore =
      view.property("structuralStageCommits").toULongLong();

  ConversationSnapshot invalidHistory = conversation(60);
  VisibleCardData duplicate = message(10'001, "duplicate history row");
  TurnSection firstDuplicate{
      "duplicate-section-a", duplicate.turnId, {duplicate}, std::nullopt};
  TurnSection secondDuplicate{
      "duplicate-section-b", duplicate.turnId, {duplicate}, std::nullopt};
  invalidHistory.sections.insert(invalidHistory.sections.begin() + 30,
                                 std::move(firstDuplicate));
  invalidHistory.sections.insert(invalidHistory.sections.begin() + 31,
                                 std::move(secondDuplicate));
  result &=
      expect(admitted(view.reconcileStaged(std::move(invalidHistory))) &&
                 waitUntil([&] { return completions.size() == 1; }, 1000),
             "a duplicate history snapshot reaches one terminal completion");
  settle();
  result &= expect(
      completions.size() == 1 &&
          completions.front().threadId == "virtual-thread" &&
          completions.front().result ==
              ConversationView::ReconciliationResult::Rejected &&
          !completions.front().selectionCommitted &&
          completions.front().stagingEmpty && !view.structuralStagingActive() &&
          view.presentedThreadId() == "virtual-thread" &&
          view.conversationModel()->rowCount() == rowsBefore &&
          firstVisible(view) == anchorBefore &&
          view.findChildren<ConversationCard *>() == cardsBefore &&
          centerCard && materializedCard(view, centerKey) == centerCard &&
          centerCard->geometry() == centerGeometry &&
          QApplication::focusWidget() == focusBefore &&
          view.viewport()->grab().toImage() == pixelsBefore &&
          view.property("structuralStageCommits").toULongLong() ==
              commitsBefore,
      "rejected history changes no model, pixels, object, geometry, anchor, "
      "focus, or commit count and releases every staged card");

  nodegraph::NodeGraph graph;
  nodegraph::NodeRef duplicateTarget;
  {
    auto write = graph.write();
    duplicateTarget =
        write.upsert({nodegraph::NodeKind::Item, "duplicate-target"});
    static_cast<void>(write.finish());
  }
  ConversationSnapshot invalidTargets = conversation(60);
  VisibleCardData firstTarget = message(10'002, "first target row");
  VisibleCardData secondTarget = message(10'003, "second target row");
  firstTarget.target = duplicateTarget;
  secondTarget.target = duplicateTarget;
  invalidTargets.sections.insert(invalidTargets.sections.begin() + 30,
                                 TurnSection{"target-section-a",
                                             firstTarget.turnId,
                                             {std::move(firstTarget)},
                                             std::nullopt});
  invalidTargets.sections.insert(invalidTargets.sections.begin() + 31,
                                 TurnSection{"target-section-b",
                                             secondTarget.turnId,
                                             {std::move(secondTarget)},
                                             std::nullopt});
  result &=
      expect(admitted(view.reconcileStaged(std::move(invalidTargets))) &&
                 waitUntil([&] { return completions.size() == 2; }, 1000),
             "a duplicate target snapshot reaches one terminal completion");
  settle();
  result &= expect(completions.size() == 2 &&
                       completions.back().result ==
                           ConversationView::ReconciliationResult::Rejected &&
                       !completions.back().selectionCommitted &&
                       completions.back().stagingEmpty &&
                       view.conversationModel()->rowCount() == rowsBefore &&
                       firstVisible(view) == anchorBefore &&
                       view.findChildren<ConversationCard *>() == cardsBefore &&
                       QApplication::focusWidget() == focusBefore &&
                       view.viewport()->grab().toImage() == pixelsBefore &&
                       view.property("structuralStageCommits").toULongLong() ==
                           commitsBefore,
                   "duplicate target authority changes no presented state");

  auto *overlay =
      view.findChild<QWidget *>(QStringLiteral("conversationStagingOverlay"));
#if QT_CONFIG(accessibility)
  QAccessibleInterface *failedOverlay =
      overlay ? QAccessible::queryAccessibleInterface(overlay) : nullptr;
  const QAccessible::Id failedOverlayId =
      failedOverlay ? QAccessible::uniqueId(failedOverlay) : 0;
  tests::AccessibilityEventProbe accessibilityEvents;
#endif
  view.beginThreadSelection("rejected-target");
  settle();
#if QT_CONFIG(accessibility)
  accessibilityEvents.clear();
#endif
  ConversationSnapshot rejectedTarget =
      singleMessageConversation("rejected-target", "invalid target");
  rejectedTarget.sections.push_back(rejectedTarget.sections.front());
  result &= expect(
      admitted(view.reconcileStaged(std::move(rejectedTarget))) &&
          waitUntil([&] { return completions.size() == 3; }, 1000),
      "a duplicate selected-thread snapshot reaches one terminal completion");
#if QT_CONFIG(accessibility)
  const auto failedOverlayEvents =
      accessibilityEvents.events(failedOverlayId);
  const bool failedAccessibility =
      failedOverlay && !failedOverlay->state().busy &&
      failedOverlay->text(QAccessible::Name) ==
          QStringLiteral("Conversation unavailable") &&
      accessibilityEvents
              .events(failedOverlayId, QAccessible::StateChanged)
              .size() == 1 &&
      !accessibilityEvents
           .events(failedOverlayId, QAccessible::StateChanged)
           .front()
           .state.busy &&
      accessibilityEvents.events(failedOverlayId, QAccessible::NameChanged)
              .size() == 1 &&
      failedOverlayEvents.size() == 2 &&
      failedOverlayEvents.at(0).type == QAccessible::NameChanged &&
      failedOverlayEvents.at(1).type == QAccessible::StateChanged;
#else
  const bool failedAccessibility = true;
#endif
  result &= expect(
      completions.size() == 3 &&
          completions.back().result ==
              ConversationView::ReconciliationResult::Rejected &&
          !completions.back().selectionCommitted &&
          completions.back().stagingEmpty && overlay && overlay->isVisible() &&
          overlay->accessibleName() ==
              QStringLiteral("Conversation unavailable") &&
          failedAccessibility && view.presentedThreadId() == "virtual-thread" &&
          view.mode() == ConversationView::Mode::Paused &&
          firstVisible(view) == anchorBefore &&
          view.conversationModel()->rowCount() == rowsBefore && centerCard &&
          materializedCard(view, centerKey) == centerCard,
      "a rejected selection keeps the outgoing authority intact and covered");

#if QT_CONFIG(accessibility)
  accessibilityEvents.clear();
#endif
  ConversationSnapshot repeatedRejection =
      singleMessageConversation("rejected-target", "invalid target");
  repeatedRejection.sections.push_back(repeatedRejection.sections.front());
  result &= expect(
      view.reconcile(repeatedRejection) ==
          ConversationView::ReconciliationResult::Rejected,
      "the identical invalid authority remains rejected");
  settle();
#if QT_CONFIG(accessibility)
  result &= expect(
      accessibilityEvents.events(failedOverlayId).isEmpty() && failedOverlay &&
          !failedOverlay->state().busy &&
          failedOverlay->text(QAccessible::Name) ==
              QStringLiteral("Conversation unavailable"),
      "repeating the failed loading state emits no accessibility event");
  accessibilityEvents.clear();
#endif
  view.beginThreadSelection("rejected-target");
  settle();
#if QT_CONFIG(accessibility)
  const auto retryBusy = accessibilityEvents.events(
      failedOverlayId, QAccessible::StateChanged);
  const auto retryEvents = accessibilityEvents.events(failedOverlayId);
  result &= expect(
      retryBusy.size() == 1 && retryBusy.front().changedStates.busy &&
          retryBusy.front().state.busy &&
          accessibilityEvents
                  .events(failedOverlayId, QAccessible::NameChanged)
                  .size() == 1 &&
          retryEvents.size() == 2 &&
          retryEvents.at(0).type == QAccessible::NameChanged &&
          retryEvents.at(1).type == QAccessible::StateChanged,
      "retry publishes one final name and busy transition on the retained "
      "loading status");
  accessibilityEvents.clear();
#endif
  result &= expect(
      admitted(view.reconcileStaged(
          singleMessageConversation("rejected-target", "valid target"))) &&
          waitUntil([&] { return completions.size() == 4; }, 1000) &&
          completions.back().result ==
              ConversationView::ReconciliationResult::Changed &&
          completions.back().selectionCommitted &&
          completions.back().stagingEmpty && overlay && !overlay->isVisible() &&
          view.presentedThreadId() == "rejected-target" &&
          view.modeForThread("virtual-thread") ==
              ConversationView::Mode::Paused,
      "the next valid authority completes the still-pending selection");
#if QT_CONFIG(accessibility)
  const auto retryComplete = accessibilityEvents.events(
      failedOverlayId, QAccessible::StateChanged);
  result &= expect(
      retryComplete.size() == 1 && retryComplete.front().changedStates.busy &&
          !retryComplete.front().state.busy,
      "retry completion publishes one not-busy transition on the retained "
      "loading status");
#endif
  return result;
}

bool stagedCommitRejectsReentrantMutation() {
  ConversationView view;
  view.resize(820, 420);
  view.show();
  bool result = expect(changed(view.reconcile(conversation(2))),
                       "the reentrant fixture reconciles");
  bool signalObserved = false;
  bool stagingReportedActive = false;
  bool nestedDeltaAccepted = true;
  ConversationView::SnapshotDisposition nestedStageDisposition =
      ConversationView::SnapshotDisposition::Admitted;
  ConversationView::ReconciliationResult nestedResult =
      ConversationView::ReconciliationResult::Unchanged;
  QObject::connect(
      view.conversationModel(), &QAbstractItemModel::rowsAboutToBeInserted,
      &view, [&](const QModelIndex &, int, int) {
        signalObserved = true;
        stagingReportedActive = view.structuralStagingActive();
        nestedResult = view.reconcile(conversation(4));
        nestedStageDisposition = view.reconcileStaged(conversation(4));
        ConversationDelta delta;
        delta.threadId = "virtual-thread";
        nestedDeltaAccepted =
            view.applyConversationDelta(std::move(delta)).has_value();
      });

  result &= expect(
      admitted(view.reconcileStaged(conversation(3))) &&
          waitUntil([&view] { return !view.structuralStagingActive(); }, 1000),
      "the outer staged insertion reaches one terminal commit");
  result &= expect(
      signalObserved && stagingReportedActive &&
          nestedResult == ConversationView::ReconciliationResult::Rejected &&
          nestedStageDisposition ==
              ConversationView::SnapshotDisposition::Retryable &&
          !nestedDeltaAccepted && view.conversationModel()->rowCount() == 3,
      "an open model transaction rejects every nested structural mutation");

  signalObserved = false;
  stagingReportedActive = true;
  nestedDeltaAccepted = true;
  nestedStageDisposition = ConversationView::SnapshotDisposition::Admitted;
  nestedResult = ConversationView::ReconciliationResult::Unchanged;
  ConversationRowPlacement directTail;
  directTail.card = message(3);
  directTail.sectionKey = "section-3";
  result &= expect(
      appendProjected(view, std::move(directTail)) && signalObserved &&
          !stagingReportedActive &&
          nestedResult == ConversationView::ReconciliationResult::Rejected &&
          nestedStageDisposition ==
              ConversationView::SnapshotDisposition::Retryable &&
          !nestedDeltaAccepted && view.conversationModel()->rowCount() == 4,
      "a direct delta holds the same mutation guard until its model signals "
      "and geometry commit finish");
  return result;
}

bool stagedCardCleanupRejectsReentrantMutation() {
  ConversationView view;
  view.resize(820, 420);
  view.show();
  bool result = expect(changed(view.reconcile(conversation(2))),
                       "the staged-cleanup fixture reconciles");
  result &= expect(admitted(view.reconcileStaged(conversation(80))),
                   "the staged-cleanup frame is admitted");
  QWidget *const stagingHost = view.findChild<QWidget *>(
      QStringLiteral("conversationStagingHost"), Qt::FindDirectChildrenOnly);
  QPointer<ConversationCard> staged;
  result &= expect(waitUntil(
                       [&] {
                         if (!stagingHost || !view.structuralStagingActive())
                           return false;
                         const auto cards =
                             stagingHost->findChildren<ConversationCard *>(
                                 QString{}, Qt::FindDirectChildrenOnly);
                         if (cards.empty())
                           return false;
                         staged = cards.front();
                         return true;
                       },
                       1000),
                   "one future card exists before staged cleanup");
  if (!staged)
    return false;

  bool destroyed = false;
  bool nestedDeltaAccepted = true;
  ConversationView::SnapshotDisposition nestedStage =
      ConversationView::SnapshotDisposition::Admitted;
  ConversationView::ReconciliationResult nestedReconcile =
      ConversationView::ReconciliationResult::Unchanged;
  QObject::connect(staged, &QObject::destroyed, &view, [&] {
    destroyed = true;
    ConversationDelta delta;
    delta.threadId = "virtual-thread";
    nestedDeltaAccepted =
        view.applyConversationDelta(std::move(delta)).has_value();
    nestedStage = view.reconcileStaged(conversation(3));
    nestedReconcile = view.reconcile(conversation(3));
  });
  const auto replacement = view.reconcileStaged(conversation(3));
  result &= expect(
      admitted(replacement) &&
          waitUntil([&view] { return !view.structuralStagingActive(); },
                    2000) &&
          destroyed && !nestedDeltaAccepted &&
          nestedStage == ConversationView::SnapshotDisposition::Retryable &&
          nestedReconcile == ConversationView::ReconciliationResult::Rejected &&
          view.conversationModel()->rowCount() == 3,
      "superseded staged QObject destruction cannot reenter snapshot, delta, "
      "or model mutation before the newest frame commits");
  return result;
}

bool committedCallbackMayDestroyView() {
  auto *view = new ConversationView;
  QPointer<ConversationView> lifetime(view);
  const auto callbackContinued = std::make_shared<int>(0);
  view->resize(640, 360);
  view->show();
  view->setReconciliationFinishedAction(
      [view, callbackContinued](const std::string &,
                                ConversationView::ReconciliationResult, bool) {
        delete view;
        ++*callbackContinued;
      });
  static_cast<void>(view->reconcileStaged(
      singleMessageConversation("destructive-commit", "Committed")));
  const bool destroyed =
      waitUntil([&lifetime] { return lifetime.isNull(); }, 1000);
  return expect(destroyed && *callbackContinued == 1,
                "presentation publication runs only after staged cleanup and "
                "its callable may safely continue after destroying the view");
}

bool destructiveCompletionSuppressesPromptCallbacks() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef prompt;
  nodegraph::NodeRef authoritative;
  {
    auto write = graph.write();
    prompt = write.upsert({nodegraph::NodeKind::Item, "completion-prompt"});
    authoritative =
        write.upsert({nodegraph::NodeKind::Item, "completion-user"});
    static_cast<void>(write.finish());
  }

  const std::string thread = "destructive-completion-materialization";
  VisibleCardData local{LocalPromptKey{78},
                        CardKind::LocalPrompt,
                        thread,
                        "turn",
                        "local",
                        LocalPromptData{78, "Queued prompt"}};
  local.target = prompt;
  ConversationSnapshot initial;
  initial.threadId = thread;
  initial.sections.push_back({"section", "turn", {local}, local.key});

  auto *view = new ConversationView;
  QPointer<ConversationView> lifetime(view);
  view->resize(640, 360);
  view->show();
  bool result = expect(changed(view->reconcile(initial)),
                       "the destructive completion fixture reconciles");
  const auto completionCallbacks = std::make_shared<int>(0);
  const auto promptCallbacks = std::make_shared<int>(0);
  view->setPromptMaterializedAction([promptCallbacks](nodegraph::NodeRef) {
    ++*promptCallbacks;
    return true;
  });
  view->setReconciliationFinishedAction(
      [view, completionCallbacks](
          const std::string &, ConversationView::ReconciliationResult, bool) {
        ++*completionCallbacks;
        delete view;
      });

  VisibleCardData user{
      LocalPromptKey{78}, CardKind::UserMessage,           thread, "turn",
      "authoritative",    UserMessageData{"Queued prompt"}};
  user.target = authoritative;
  ConversationSnapshot acknowledged;
  acknowledged.threadId = thread;
  acknowledged.materializedPrompts.push_back({user.key, prompt});
  acknowledged.sections.push_back({"section", "turn", {user}, user.key});
  result &= expect(admitted(view->reconcileStaged(std::move(acknowledged))),
                   "the acknowledged prompt snapshot is admitted");
  result &= expect(
      waitUntil([&lifetime] { return lifetime.isNull(); }, 1000) &&
          *completionCallbacks == 1 && *promptCallbacks == 0,
      "owner destruction in completion prevents later prompt publication");
  return result;
}

bool destructivePromptCallbackStopsTheBatch() {
  nodegraph::NodeGraph graph;
  std::array<nodegraph::NodeRef, 2> prompts;
  std::array<nodegraph::NodeRef, 2> authoritative;
  {
    auto write = graph.write();
    for (std::size_t index = 0; index < prompts.size(); ++index) {
      prompts[index] = write.upsert(
          {nodegraph::NodeKind::Item, "batch-prompt-" + std::to_string(index)});
      authoritative[index] = write.upsert(
          {nodegraph::NodeKind::Item, "batch-user-" + std::to_string(index)});
    }
    static_cast<void>(write.finish());
  }

  const std::string thread = "destructive-materialization-batch";
  ConversationSnapshot initial;
  initial.threadId = thread;
  TurnSection localSection{"section", "turn", {}, std::nullopt};
  TurnSection userSection{"section", "turn", {}, std::nullopt};
  for (std::size_t index = 0; index < prompts.size(); ++index) {
    const std::uint64_t localId = 80 + index;
    VisibleCardData local{LocalPromptKey{localId},
                          CardKind::LocalPrompt,
                          thread,
                          "turn",
                          "local-" + std::to_string(index),
                          LocalPromptData{localId, "Queued prompt"}};
    local.target = prompts[index];
    VisibleCardData user{LocalPromptKey{localId},
                         CardKind::UserMessage,
                         thread,
                         "turn",
                         "user-" + std::to_string(index),
                         UserMessageData{"Queued prompt"}};
    user.target = authoritative[index];
    localSection.cards.push_back(std::move(local));
    userSection.cards.push_back(std::move(user));
  }
  localSection.rootCardKey = localSection.cards.front().key;
  userSection.rootCardKey = userSection.cards.front().key;
  initial.sections.push_back(std::move(localSection));

  auto *view = new ConversationView;
  QPointer<ConversationView> lifetime(view);
  view->resize(640, 360);
  view->show();
  bool result = expect(changed(view->reconcile(initial)),
                       "the destructive prompt batch fixture reconciles");
  const auto promptCallbacks = std::make_shared<int>(0);
  view->setPromptMaterializedAction(
      [view, promptCallbacks](nodegraph::NodeRef) {
        ++*promptCallbacks;
        delete view;
        return true;
      });
  ConversationSnapshot acknowledged;
  acknowledged.threadId = thread;
  for (std::size_t index = 0; index < prompts.size(); ++index)
    acknowledged.materializedPrompts.push_back(
        {userSection.cards[index].key, prompts[index]});
  acknowledged.sections.push_back(std::move(userSection));
  result &= expect(admitted(view->reconcileStaged(std::move(acknowledged))),
                   "the prompt batch snapshot is admitted");
  result &=
      expect(waitUntil([&lifetime] { return lifetime.isNull(); }, 1000) &&
                 *promptCallbacks == 1,
             "owner destruction in the first prompt callback stops the batch");
  return result;
}

bool promptCallbackMayDestroyView() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef prompt;
  nodegraph::NodeRef authoritative;
  {
    auto write = graph.write();
    prompt = write.upsert({nodegraph::NodeKind::Item, "destructive-prompt"});
    authoritative =
        write.upsert({nodegraph::NodeKind::Item, "destructive-authoritative"});
    static_cast<void>(write.finish());
  }

  const std::string thread = "destructive-materialization";
  VisibleCardData local{LocalPromptKey{77},
                        CardKind::LocalPrompt,
                        thread,
                        "turn",
                        "local",
                        LocalPromptData{77, "Queued prompt"}};
  local.target = prompt;
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back({"section", "turn", {local}, local.key});

  auto *view = new ConversationView;
  QPointer<ConversationView> lifetime(view);
  view->resize(640, 360);
  view->show();
  bool result = expect(changed(view->reconcile(snapshot)),
                       "the destructive prompt fixture reconciles");
  const auto callbackContinued = std::make_shared<int>(0);
  view->setPromptMaterializedAction(
      [view, callbackContinued](nodegraph::NodeRef) {
        delete view;
        ++*callbackContinued;
        return true;
      });
  VisibleCardData user{
      LocalPromptKey{77}, CardKind::UserMessage,           thread, "turn",
      "authoritative",    UserMessageData{"Queued prompt"}};
  user.target = authoritative;
  ConversationDelta materialization;
  materialization.threadId = thread;
  materialization.materializedPrompts.push_back({LocalPromptKey{77}, prompt});
  materialization.rows.push_back(
      {{std::move(user), "section", true, false, false}, {}, {}});
  materialization.removals.push_back(prompt);
  const auto impact = view->applyConversationDelta(std::move(materialization));
  result &= expect(
      impact.has_value() && lifetime.isNull() && *callbackContinued == 1,
      "prompt publication keeps its callable alive after it destroys the "
      "view");
  return result;
}

bool virtualTurnSurfaceUsesOneRenderer() {
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
  bool result = expect(changed(view.reconcile(snapshot)),
                       "virtual Turn/You fixture reconciles");
  settle();
  const QModelIndex rootIndex = view.conversationModel()->index(0);
  const QModelIndex nestedIndex = view.conversationModel()->index(1);
  const QRect rootRect = view.visualRect(rootIndex);
  const QRect nestedRect = view.visualRect(nestedIndex);
  ConversationCard *const rootCard =
      materializedCard(view, stableKey(root.key));
  ConversationCard *const nestedCard =
      materializedCard(view, stableKey(nested.key));
  result &=
      expect(rootRect.left() == 0 && nestedRect.left() == 12 &&
                 nestedRect.width() == rootRect.width() - 24 &&
                 nestedRect.top() > rootRect.bottom() && rootCard && nestedCard,
             "visible turn rows use authoritative cards at nested geometry");
  const QImage painted = view.viewport()->grab().toImage();
  const int sampleY =
      std::clamp(rootRect.bottom() + 3, 0, std::max(0, painted.height() - 1));
  const QColor turnSurface = painted.pixelColor(4, sampleY);
  result &= expect(turnSurface.blue() > turnSurface.red(),
                   "the view paints the continuous blue You turn enclosure");

  const QRect cardGeometry = rootCard ? rootCard->geometry() : QRect{};
  if (rootCard) {
    QEvent enter(QEvent::Enter);
    QApplication::sendEvent(rootCard, &enter);
    QMouseEvent press(QEvent::MouseButtonPress,
                      QPointF(rootCard->rect().center()),
                      QPointF(rootCard->rect().center()),
                      rootCard->mapToGlobal(rootCard->rect().center()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rootCard, &press);
    QMouseEvent release(QEvent::MouseButtonRelease,
                        QPointF(rootCard->rect().center()),
                        QPointF(rootCard->rect().center()),
                        rootCard->mapToGlobal(rootCard->rect().center()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(rootCard, &release);
  }
  settle();
  result &= expect(
      rootCard && materializedCard(view, stableKey(root.key)) == rootCard &&
          rootCard->geometry() == cardGeometry &&
          rootCard->property("virtualTurnRoot").toBool() &&
          rootCard->parentWidget() == view.viewport(),
      "hover and first press retain the same visual object and geometry");
  result &= expect(view.materializedCardCount() == 2,
                   "the bounded visible turn has exactly its two card widgets");
  return result;
}

bool directTailGrowsTheRetainedTurnSurface() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "tail-growth";
  snapshot.activeTurnId = "active-turn";
  VisibleCardData root{
      LocalPromptKey{771},
      CardKind::LocalPrompt,
      "tail-growth",
      "active-turn",
      {},
      LocalPromptData{771, "Pending question", PromptState::InFlight}};
  TurnSection section;
  section.key = "active-section";
  section.turnId = "active-turn";
  section.cards.push_back(root);
  section.rootCardKey = root.key;
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(820, 360);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
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
  ViewportPaintRegionProbe paintProbe(view.viewport());
  paintProbe.start();
  ConversationRowPlacement tail;
  tail.card = {AuthoritativeItemKey{"tail-growth", "active-turn", "answer"},
               CardKind::AgentMessage,
               "tail-growth",
               "active-turn",
               "answer",
               AgentMessageData{"Final answer", true}};
  tail.sectionKey = "active-section";
  tail.nested = true;
  tail.activeTurn = true;
  const std::string answerKey = stableKey(tail.card.key);
  result &= expect(appendProjected(view, std::move(tail)),
                   "the first nested direct-tail card appends");
  settle();
  const QRegion directTailPaint = paintProbe.stop();

  const QModelIndex rootIndex =
      view.conversationModel()->indexForStableKey(stableKey(root.key));
  const QModelIndex answerIndex =
      view.conversationModel()->indexForStableKey(answerKey);
  const QRect answerRect = view.visualRect(answerIndex);
  const QPoint bottomBorderPoint(answerRect.center().x(),
                                 answerRect.bottom() + 10);
  const QImage frame = view.viewport()->grab().toImage();
  const QColor grownBorder = frame.pixelColor(
      1, std::clamp(answerRect.center().y(), 0, frame.height() - 1));
  result &= expect(retained &&
                       retained == materializedCard(view, stableKey(root.key)),
                   "direct-tail growth retains the root card identity");
  result &= expect(materializedCard(view, answerKey),
                   "direct-tail growth materializes the visible answer card");
  result &= expect(retained && retained->property("virtualTurnRoot").toBool() &&
                       !retained->property("authoritativeTurnActive").toBool(),
                   "the retained root delegates its active Turn border to the "
                   "view after direct-tail growth");
  result &= expect(
      rootIndex.data(ConversationItemModel::ActiveTurnRole).toBool(),
      "direct-tail growth preserves authoritative active-Turn model state");
  result &= expect(answerIndex.isValid() && answerRect.left() == 12,
                   "the direct-tail answer has nested Turn geometry");
  result &= expect(directTailPaint.contains(bottomBorderPoint),
                   "direct-tail growth invalidates the extended Turn border");
  result &=
      expect(grownBorder.blue() > grownBorder.red() && grownBorder.red() < 183,
             "direct-tail growth paints the continuous Turn border");
  result &=
      expect(view.property("conversationCardConstructions").toULongLong() ==
                 constructions + 1,
             "direct-tail growth constructs only its new answer card");
  result &=
      expect(view.property("conversationSectionRangeRebuilds").toULongLong() ==
                 sectionRebuilds,
             "direct-tail growth does not rebuild retained section indexes");

  paintProbe.start();
  ConversationRowPlacement laterTail;
  laterTail.card = {
      AuthoritativeItemKey{"tail-growth", "active-turn", "later-update"},
      CardKind::AgentMessage,
      "tail-growth",
      "active-turn",
      "later-update",
      AgentMessageData{"Later update", false}};
  laterTail.sectionKey = "active-section";
  laterTail.nested = true;
  laterTail.activeTurn = true;
  const std::string laterKey = stableKey(laterTail.card.key);
  result &= expect(appendProjected(view, std::move(laterTail)),
                   "a later nested direct-tail card appends");
  settle();
  const QRegion laterTailPaint = paintProbe.stop();
  const QModelIndex laterIndex =
      view.conversationModel()->indexForStableKey(laterKey);
  const QRect laterRect = view.visualRect(laterIndex);
  result &= expect(laterIndex.isValid(),
                   "the later direct-tail card enters the model");
  result &= expect(
      laterTailPaint.contains(
          QPoint(laterRect.center().x(), laterRect.bottom() + 10)),
      "every later direct-tail card invalidates its new Turn bottom border");
  return result;
}

bool stagedCommandOutputFollowsAfterFinalGeometry() {
  const std::string thread = "staged-command-follow";
  ConversationView view;
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(760, 520);
  view.show();
  bool result = expect(changed(view.reconcile(singleMessageConversation(
                           "command-stage-source", "Source"))),
                       "the command staging source frame reconciles");

  ConversationSnapshot target = commandConversation(thread, 20);
  VisibleCardData &command = target.sections.at(18).cards.front();
  auto &execution = std::get<CommandExecutionData>(command.payload);
  execution.output.clear();
  for (int line = 0; line < 100; ++line)
    execution.output +=
        "retained staged output line " + std::to_string(line) + '\n';
  command.status = nodegraph::NodeStatus::Running;
  const std::string commandKey = stableKey(command.key);
  const qulonglong constructionsBefore =
      view.property("conversationCardConstructions").toULongLong();

  bool publicationObserved = false;
  bool publicationValid = false;
  QPointer<ConversationCard> publishedCard;
  QPointer<CommandOutputView> publishedOutput;
  CommandOutputFirstPaintProbe firstPaint;
  view.setReconciliationFinishedAction(
      [&](const std::string &threadId,
          ConversationView::ReconciliationResult reconciliation,
          bool selectionCommitted) {
        if (threadId != thread)
          return;
        publicationObserved = true;
        publishedCard = materializedCard(view, commandKey);
        publishedOutput = publishedCard
                              ? publishedCard->findChild<CommandOutputView *>(
                                    QStringLiteral("commandOutputView"))
                              : nullptr;
        publicationValid =
            changed(reconciliation) && selectionCommitted && publishedOutput &&
            publishedOutput->followsLatest() &&
            publishedOutput->verticalScrollBar()->maximum() > 0 &&
            view.property("conversationCardConstructions").toULongLong() ==
                constructionsBefore;
        firstPaint.watch(publishedOutput);
      });
  result &= expect(admitted(view.reconcileStaged(std::move(target))),
                   "the expanded command frame enters hidden staging");
  result &= expect(waitUntil([&] { return publicationObserved; }, 3000),
                   "the expanded command frame reaches atomic publication");
  result &= expect(publicationValid,
                   "atomic publication adopts the staged following command");
  result &= expect(waitUntil([&] { return firstPaint.seen(); }, 1000) &&
                       firstPaint.followedTail(),
                   "the first painted staged command frame follows its real "
                   "tail");
  result &= expect(
      publishedCard && publishedOutput &&
          materializedCard(view, commandKey) == publishedCard &&
          publishedCard->findChild<CommandOutputView *>(
              QStringLiteral("commandOutputView")) == publishedOutput &&
          publishedOutput->followsLatest() &&
          publishedOutput->verticalScrollBar()->value() ==
              publishedOutput->verticalScrollBar()->maximum(),
      "event-loop settlement preserves the published command renderer and "
      "follow-tail state");
  return result;
}

bool commandOutputScrollOwnsConversationFollowing() {
  const std::string thread = "command-follow-ownership";
  QString output;
  for (int line = 0; line < 100; ++line)
    output += QStringLiteral("streamed command line %1\n").arg(line);
  VisibleCardData root{AuthoritativeItemKey{thread, "turn", "root"},
                       CardKind::UserMessage,
                       thread,
                       "turn",
                       "root",
                       UserMessageData{"Run it"}};
  VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{
          "run long command", output.toStdString(), "/workspace", {}, {}},
      nodegraph::NodeStatus::Running};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.activeTurnId = "turn";
  snapshot.sections.push_back({"section", "turn", {root, command}, root.key});

  ConversationView view;
  auto options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(760, 300);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "command follow-ownership fixture reconciles");
  settle();
  ConversationCard *commandCard =
      materializedCard(view, stableKey(command.key));
  auto *commandOutput = commandCard
                            ? commandCard->findChild<CommandOutputView *>(
                                  QStringLiteral("commandOutputView"))
                            : nullptr;
  result &= expect(
      commandOutput && commandOutput->verticalScrollBar()->maximum() > 0 &&
          commandOutput->followsLatest() &&
          view.mode() == ConversationView::Mode::Following && view.isAtBottom(),
      "the visible command and conversation begin following");
  if (!commandOutput)
    return false;

  commandOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  settle();
  result &= expect(!commandOutput->followsLatest() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "scrolling command output upward pauses outer following");

  ConversationRowPlacement tail;
  tail.card = {AuthoritativeItemKey{thread, "turn", "answer"},
               CardKind::AgentMessage,
               thread,
               "turn",
               "answer",
               AgentMessageData{std::string(1200, 'x'), true}};
  tail.sectionKey = "section";
  tail.nested = true;
  tail.activeTurn = true;
  const VisibleCardData answer = tail.card;
  result &= expect(appendProjected(view, std::move(tail)),
                   "new tail activity arrives while command output is paused");
  settle();
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !view.isAtBottom(),
                   "paused command output prevents arrival from following");

  commandOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMaximum);
  result &= expect(
      waitUntil(
          [&] {
            return commandOutput->followsLatest() &&
                   view.mode() == ConversationView::Mode::Following &&
                   view.isAtBottom();
          },
          500),
      "returning command output to its bottom resumes outer tail following");

  commandOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  settle();
  const bool ownsSecondPause = !commandOutput->followsLatest() &&
                               view.mode() == ConversationView::Mode::Paused;
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  settle();
  commandOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMaximum);
  settle();
  const bool manualPauseSurvivesCommandRelease =
      commandOutput->followsLatest() &&
      view.mode() == ConversationView::Mode::Paused && !view.isAtBottom();
  result &= expect(ownsSecondPause && manualPauseSurvivesCommandRelease,
                   "direct outer scrolling converts command-owned pause to "
                   "manual pause before the command releases");
  return result;
}

bool multipleDetachedCommandsShareOneOuterPauseCause() {
  const std::string thread = "multiple-command-follow-ownership";
  std::string output;
  for (int line = 0; line < 100; ++line)
    output += "command output line " + std::to_string(line) + '\n';
  const VisibleCardData root{AuthoritativeItemKey{thread, "turn", "root"},
                             CardKind::UserMessage,
                             thread,
                             "turn",
                             "root",
                             UserMessageData{"Run both"}};
  const VisibleCardData first{
      AuthoritativeItemKey{thread, "turn", "command-a"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command-a",
      CommandExecutionData{"first command", output, "/workspace", {}, {}},
      nodegraph::NodeStatus::Running};
  const VisibleCardData second{
      AuthoritativeItemKey{thread, "turn", "command-b"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command-b",
      CommandExecutionData{"second command", output, "/workspace", {}, {}},
      nodegraph::NodeStatus::Running};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.activeTurnId = "turn";
  snapshot.sections.push_back(
      {"section", "turn", {root, first, second}, root.key});

  ConversationView view;
  auto options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(760, 520);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "multiple-command follow fixture reconciles");
  settle();
  ConversationCard *firstCard = materializedCard(view, stableKey(first.key));
  ConversationCard *secondCard = materializedCard(view, stableKey(second.key));
  auto *firstOutput =
      firstCard ? firstCard->findChild<CommandOutputView *>() : nullptr;
  auto *secondOutput =
      secondCard ? secondCard->findChild<CommandOutputView *>() : nullptr;
  result &= expect(firstOutput && secondOutput &&
                       firstOutput->verticalScrollBar()->maximum() > 0 &&
                       secondOutput->verticalScrollBar()->maximum() > 0 &&
                       view.verticalScrollBar()->maximum() > 0,
                   "both command outputs have resident scroll authorities");
  if (!firstOutput || !secondOutput)
    return false;

  firstOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  settle();
  result &= expect(!firstOutput->followsLatest() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "detaching command output pauses the following view");
  firstOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMaximum);
  result &=
      expect(waitUntil(
                 [&] {
                   return firstOutput->followsLatest() &&
                          view.mode() == ConversationView::Mode::Following;
                 },
                 500),
             "the command-owned pause releases after output reattaches");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
  firstOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  firstOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMaximum);
  settle();
  result &= expect(firstOutput->followsLatest() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "a command cannot acquire or release a user-owned outer "
                   "pause");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  settle();

  firstOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  secondOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  settle();
  result &=
      expect(!firstOutput->followsLatest() && !secondOutput->followsLatest() &&
                 view.mode() == ConversationView::Mode::Paused,
             "two detached outputs share one derived outer pause cause");

  VisibleCardData retiredFirst = first;
  auto &retired = std::get<CommandExecutionData>(retiredFirst.payload);
  retired.output = " \n\t\n";
  retiredFirst.status = nodegraph::NodeStatus::Completed;
  result &= expect(applyPresentation(view, retiredFirst).has_value(),
                   "the first detached output retires");
  settle();
  result &= expect(!secondOutput->followsLatest() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "retiring one output cannot resume while another remains "
                   "detached");

  secondOutput->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderToMaximum);
  result &=
      expect(waitUntil(
                 [&] {
                   return secondOutput->followsLatest() &&
                          view.mode() == ConversationView::Mode::Following;
                 },
                 500),
             "the final detached output returning to its tail releases the one "
             "outer pause cause");
  return result;
}

bool offscreenCommandRemovalRetiresInnerAndOuterPause() {
  const std::string thread = "offscreen-command-state";
  VisibleCardData root{AuthoritativeItemKey{thread, "command-turn", "root"},
                       CardKind::UserMessage,
                       thread,
                       "command-turn",
                       "root",
                       UserMessageData{"Run the retained command"}};
  std::string output;
  for (int line = 0; line < 100; ++line)
    output += "old output line " + std::to_string(line) + '\n';
  VisibleCardData command{
      AuthoritativeItemKey{thread, "command-turn", "command"},
      CardKind::CommandExecution,
      thread,
      "command-turn",
      "command",
      CommandExecutionData{
          "run retained command", output, "/workspace", {}, {}},
      nodegraph::NodeStatus::Running};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  for (int row = 0; row < 160; ++row) {
    const std::string id = "filler-" + std::to_string(row);
    snapshot.sections.push_back(
        {"section-" + id,
         "turn-" + id,
         {{AuthoritativeItemKey{thread, "turn-" + id, id},
           CardKind::AgentMessage, thread, "turn-" + id, id,
           AgentMessageData{std::string(180, 'x'), true}}},
         std::nullopt});
  }
  snapshot.sections.push_back(
      {"command-section", "command-turn", {root, command}, root.key});

  ConversationView view;
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(760, 320);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "offscreen command-state fixture reconciles");
  const std::string key = stableKey(command.key);
  ConversationCard *card = nullptr;
  CommandOutputView *inner = nullptr;
  const bool commandReady = waitUntil(
      [&] {
        card = materializedCard(view, key);
        inner = card ? card->findChild<CommandOutputView *>(
                           QStringLiteral("commandOutputView"))
                     : nullptr;
        return inner && inner->verticalScrollBar()->maximum() > 0;
      },
      2000);
  result &= expect(commandReady,
                   "the command has a scrollable resident output owner");
  if (!inner)
    return false;
  result &= expect(card->state().empty(),
                   "following unselected output has no retained interaction "
                   "snapshot");
  inner->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  QTextCursor selection(inner->document());
  selection.setPosition(30);
  selection.setPosition(12, QTextCursor::KeepAnchor);
  inner->setTextCursor(selection);
  inner->setFocus(Qt::TabFocusReason);
  settle();
  result &= expect(!inner->followsLatest() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "inner detachment owns the outer paused state");

  const CommandOutputView::State detachedState = inner->state();
  QTextDocument *detachedDocument = inner->document();
  const auto environmentAnchor = firstVisible(view);
  const qulonglong environmentReflows =
      view.property("conversationGeometryEnvironmentReflows").toULongLong();
  view.setStyleSheet(QStringLiteral("* { font-size: 18pt; }"));
  settle();
  result &=
      expect(materializedCard(view, key) == card &&
                 card->findChild<CommandOutputView *>(
                     QStringLiteral("commandOutputView")) == inner &&
                 inner->document() == detachedDocument && inner->hasFocus() &&
                 inner->state() == detachedState &&
                 firstVisible(view) == environmentAnchor &&
                 view.mode() == ConversationView::Mode::Paused &&
                 view.property("conversationGeometryEnvironmentReflows")
                         .toULongLong() == environmentReflows + 1,
             "font reflow preserves the command renderer, document, reverse "
             "selection, detached follow state, and outer anchor");
  view.setStyleSheet({});
  settle();

  VisibleCardData withoutOutput = command;
  auto &removed = std::get<CommandExecutionData>(withoutOutput.payload);
  removed.output = " \n\t\n";
  withoutOutput.status = nodegraph::NodeStatus::Completed;
  result &= expect(applyPresentation(view, withoutOutput).has_value(),
                   "the resident command accepts output removal");
  settle();
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "removing resident output releases its outer pause");

  result &= expect(applyPresentation(view, command).has_value(),
                   "the command accepts a new output generation");
  result &=
      expect(waitUntil(
                 [&] {
                   card = materializedCard(view, key);
                   inner = card ? card->findChild<CommandOutputView *>(
                                      QStringLiteral("commandOutputView"))
                                : nullptr;
                   return inner && inner->verticalScrollBar()->maximum() > 0;
                 },
                 2000),
             "the new command generation materializes with scrollable output");
  if (!inner)
    return false;
  inner->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  settle();
  result &= expect(!inner->followsLatest() &&
                       view.mode() == ConversationView::Mode::Paused,
                   "the new resident generation can own outer pause");

  QPointer<ConversationCard> released(card);
  inner->clearFocus();
  view.setFocus(Qt::OtherFocusReason);
  view.scrollTo(view.conversationModel()->index(0),
                QAbstractItemView::PositionAtTop);
  result &= expect(waitUntil([&] { return released.isNull(); }, 2000),
                   "the detached command can leave widget residency");

  result &= expect(applyPresentation(view, withoutOutput).has_value(),
                   "the offscreen command accepts output removal");
  settle();
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "removing the offscreen output releases its outer pause");

  VisibleCardData freshOutput = withoutOutput;
  auto &fresh = std::get<CommandExecutionData>(freshOutput.payload);
  fresh.output.clear();
  for (int line = 0; line < 80; ++line)
    fresh.output += "fresh output line " + std::to_string(line) + '\n';
  result &= expect(applyPresentation(view, freshOutput).has_value(),
                   "a fresh offscreen output generation is accepted");
  result &= expect(waitUntil([&] { return materializedCard(view, key); }, 2000),
                   "the command rematerializes after fresh output");
  card = materializedCard(view, key);
  inner = card ? card->findChild<CommandOutputView *>(
                     QStringLiteral("commandOutputView"))
               : nullptr;
  const bool freshStateSettled = waitUntil(
      [&] {
        return inner && inner->followsLatest() &&
               !inner->textCursor().hasSelection() &&
               inner->verticalScrollBar()->value() ==
                   inner->verticalScrollBar()->maximum();
      },
      1000);
  result &= expect(freshStateSettled,
                   "fresh output cannot resurrect detached scroll or "
                   "selection state");

  return result;
}

bool stagedInactiveTruncationCannotResurrectCommandState() {
  const std::string thread = "staged-command-state";
  ConversationSnapshot initial = commandConversation(thread, 1);
  VisibleCardData &initialCard = initial.sections.front().cards.front();
  auto &initialCommand = std::get<CommandExecutionData>(initialCard.payload);
  initialCard.status = nodegraph::NodeStatus::Running;
  initialCommand.output.clear();
  for (int line = 0; line < 100; ++line)
    initialCommand.output +=
        "retained output line " + std::to_string(line) + '\n';
  const std::string key = stableKey(initialCard.key);

  ConversationView view;
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(760, 320);
  view.show();
  bool result = expect(changed(view.reconcile(initial)),
                       "staged command-state fixture reconciles");
  ConversationCard *card = nullptr;
  CommandOutputView *output = nullptr;
  result &=
      expect(waitUntil(
                 [&] {
                   card = materializedCard(view, key);
                   output = card ? card->findChild<CommandOutputView *>(
                                       QStringLiteral("commandOutputView"))
                                 : nullptr;
                   return output && output->verticalScrollBar()->maximum() > 0;
                 },
                 2000),
             "the staged-state command has a scrollable interaction owner");
  if (!output)
    return false;
  output->verticalScrollBar()->triggerAction(
      QAbstractSlider::SliderSingleStepSub);
  QTextCursor selection(output->document());
  selection.setPosition(50);
  selection.setPosition(12, QTextCursor::KeepAnchor);
  output->setTextCursor(selection);
  settle();
  result &=
      expect(!output->followsLatest() && output->textCursor().hasSelection() &&
                 output->textCursor().position() == 12 &&
                 output->textCursor().anchor() == 50 &&
                 view.mode() == ConversationView::Mode::Paused,
             "the command owns detached follow and reverse selection");

  result &= expect(
      changed(
          view.reconcile(singleMessageConversation("staged-source", "Source"))),
      "switching away retains the inactive command interaction authority");
  result &= expect(waitForResidency(view),
                   "the source thread settles before future staging begins");

  ConversationSnapshot staged = commandConversation(thread, 20);
  staged.sections.front().cards.front() = initialCard;
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  bool publicationObserved = false;
  qulonglong constructionsAtPublication =
      std::numeric_limits<qulonglong>::max();
  view.setReconciliationFinishedAction(
      [&](const std::string &completedThread,
          ConversationView::ReconciliationResult, bool) {
        if (completedThread != thread)
          return;
        publicationObserved = true;
        constructionsAtPublication =
            view.property("conversationCardConstructions").toULongLong();
      });
  static_cast<void>(view.reconcileStaged(std::move(staged)));
  QWidget *stagingHost = view.findChild<QWidget *>(
      QStringLiteral("conversationStagingHost"), Qt::FindDirectChildrenOnly);
  QPointer<ConversationCard> future;
  result &=
      expect(waitUntil(
                 [&] {
                   if (!view.structuralStagingActive() || !stagingHost)
                     return false;
                   for (ConversationCard *candidate :
                        stagingHost->findChildren<ConversationCard *>(
                            QString{}, Qt::FindDirectChildrenOnly)) {
                     if (stableKey(candidate->data().key) == key) {
                       future = candidate;
                       return true;
                     }
                   }
                   return false;
                 },
                 2000),
             "the target command is prepared as geometry-only staged state");
  if (!future)
    return false;

  VisibleCardData removed = initialCard;
  auto &removedCommand = std::get<CommandExecutionData>(removed.payload);
  removedCommand.output = "replacement output\n";
  removed.status = nodegraph::NodeStatus::Completed;
  const auto removedImpact = applyPresentation(view, removed);
  VisibleCardData regrown = initialCard;
  auto &regrownCommand = std::get<CommandExecutionData>(regrown.payload);
  regrownCommand.output += "new generation suffix\n";
  const auto regrownImpact = applyPresentation(view, regrown);
  result &= expect(
      removedImpact == PresentationImpact::None &&
          regrownImpact == PresentationImpact::None &&
          view.structuralStagingActive() && !future,
      "pending truncation retires the inactive interaction owner and its "
      "superseded paused-frame renderer exactly once");

  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 3000) &&
          view.mode() == ConversationView::Mode::Following && view.isAtBottom(),
      "committing a cleared command owner restores the target thread's "
      "authoritative follow mode");
  result &=
      expect(publicationObserved && constructionsAtPublication == constructions,
             "the state transition stages its new following frame without "
             "synchronous commit construction");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  result &= expect(
      waitUntil(
          [&] {
            card = materializedCard(view, key);
            output = card ? card->findChild<CommandOutputView *>(
                                QStringLiteral("commandOutputView"))
                          : nullptr;
            return output && output->verticalScrollBar()->maximum() > 0;
          },
          2000),
      "the committed command rematerializes from retained semantic state");
  result &= expect(
      waitUntil(
          [&] {
            return output && output->followsLatest() &&
                   !output->textCursor().hasSelection() &&
                   output->verticalScrollBar()->value() ==
                       output->verticalScrollBar()->maximum();
          },
          1000),
      "a truncated inactive generation cannot resurrect detached scroll or "
      "selection after regrowth");
  return result;
}

bool singleRendererLifecycleAndResidency() {
  ConversationView view;
  view.resize(820, 420);
  view.show();

  ConversationSnapshot snapshot = conversation(240);
  bool result = expect(changed(view.reconcile(snapshot)),
                       "single-renderer residency fixture reconciles");
  result &= expect(waitForResidency(view),
                   "the sole-renderer residency window reaches a fixed point");

  const QRect overscan = view.viewport()->rect().adjusted(
      0, -view.viewport()->height(), 0, view.viewport()->height());
  const auto scalarResidencyExtent = [&view](int row) {
    QRect extent = view.visualRect(view.conversationModel()->index(row));
    if (row + 1 < view.conversationModel()->rowCount()) {
      const QRect next =
          view.visualRect(view.conversationModel()->index(row + 1));
      if (!next.isEmpty())
        extent.setBottom(next.top() - 1);
    }
    return extent;
  };
  int expectedResidents = 0;
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const QModelIndex index = view.conversationModel()->index(row);
    const auto *modelRow = view.conversationModel()->row(row);
    if (!modelRow || !index.data(ConversationItemModel::PresentedRole).toBool())
      continue;
    const bool resident = scalarResidencyExtent(row).intersects(overscan);
    ConversationCard *card = materializedCard(view, modelRow->stableKey);
    if (resident)
      ++expectedResidents;
    result &= expect(
        resident ? card != nullptr : card == nullptr,
        resident ? "every viewport/overscan row owns a ConversationCard"
                 : "rows outside viewport/overscan retain no card widget");
  }

  const auto cards = view.findChildren<ConversationCard *>();
  const auto documents = view.findChildren<MarkdownTextView *>();
  result &=
      expect(view.materializedCardCount() == expectedResidents &&
                 cards.size() == expectedResidents && expectedResidents <= 48 &&
                 documents.size() <= cards.size(),
             "card and Markdown-document residency is viewport-bounded");

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();
  view.scrollTo(view.conversationModel()->index(100),
                QAbstractItemView::PositionAtCenter);
  result &= expect(waitForResidency(view),
                   "a paused seek eventually admits the complete overscan");
  bool exactSeekResidency = true;
  for (int candidate = 0; candidate < view.conversationModel()->rowCount();
       ++candidate) {
    const QModelIndex index = view.conversationModel()->index(candidate);
    const auto *row = view.conversationModel()->row(candidate);
    if (!row || !index.data(ConversationItemModel::PresentedRole).toBool())
      continue;
    const bool expected = scalarResidencyExtent(candidate).intersects(overscan);
    ConversationCard *card = materializedCard(view, row->stableKey);
    QWidget *focused = QApplication::focusWidget();
    const bool focusPin =
        card && focused && (focused == card || card->isAncestorOf(focused));
    exactSeekResidency =
        exactSeekResidency && (expected ? card != nullptr : !card || focusPin);
  }
  result &= expect(exactSeekResidency,
                   "a paused seek converges every viewport/overscan row to "
                   "the sole renderer before releasing old residents");

  const int row = view.indexAt(view.viewport()->rect().center()).row();
  const auto *modelRow = view.conversationModel()->row(row);
  ConversationCard *const retained =
      modelRow ? materializedCard(view, modelRow->stableKey) : nullptr;
  QTextDocument *const document =
      retained && retained->findChild<MarkdownTextView *>()
          ? retained->findChild<MarkdownTextView *>()->document()
          : nullptr;
  const QRect geometry = retained ? retained->geometry() : QRect{};
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  const VisibleCardData unchanged =
      modelRow ? modelRow->card : VisibleCardData{};
  result &= expect(retained && applyPresentation(view, unchanged) ==
                                   PresentationImpact::None,
                   "an identical presentation is a semantic no-op");
  settle();
  result &= expect(
      retained && modelRow &&
          materializedCard(view, modelRow->stableKey) == retained &&
          retained->geometry() == geometry &&
          retained->findChild<MarkdownTextView *>()->document() == document &&
          view.property("conversationCardConstructions").toULongLong() ==
              constructions,
      "a semantic no-op preserves object, document, geometry, and work counts");

  const auto stagedCardsBefore = view.findChildren<ConversationCard *>();
  const QImage stagedPixelsBefore = view.viewport()->grab().toImage();
  QWidget *const stagedFocusBefore = QApplication::focusWidget();
  const qulonglong stagedCardPasses =
      view.property("structuralStageCardPasses").toULongLong();
  const qulonglong stagedMaterializations =
      view.property("conversationMaterializationPasses").toULongLong();
  const qulonglong stagedGeometry =
      view.property("conversationLocalGeometryPasses").toULongLong();
  const qulonglong stagedGraphRefreshes =
      view.property("graphRefreshPasses").toULongLong();
  static_cast<void>(view.reconcileStaged(ConversationSnapshot(snapshot)));
  result &= expect(
      waitUntil([&view] { return !view.structuralStagingActive(); }, 1000),
      "an identical staged snapshot completes");
  settle();
  result &= expect(
      view.findChildren<ConversationCard *>() == stagedCardsBefore &&
          materializedCard(view, modelRow->stableKey) == retained &&
          retained->geometry() == geometry &&
          retained->findChild<MarkdownTextView *>()->document() == document &&
          view.viewport()->grab().toImage() == stagedPixelsBefore &&
          QApplication::focusWidget() == stagedFocusBefore &&
          view.property("conversationMaterializationPasses").toULongLong() ==
              stagedMaterializations &&
          view.property("conversationLocalGeometryPasses").toULongLong() ==
              stagedGeometry &&
          view.property("graphRefreshPasses").toULongLong() ==
              stagedGraphRefreshes,
      "an identical staged replacement preserves pixels, resident objects, "
      "focus, geometry, and visible renderer work");
  result &=
      expect(view.property("structuralStageCardPasses").toULongLong() ==
                 stagedCardPasses,
             "an identical staged preflight constructs no hidden renderer");

  VisibleCardData streamed = unchanged;
  std::get<AgentMessageData>(streamed.payload).text +=
      "\n\nA streamed paragraph with a [link](https://example.com).";
  const auto impact = applyPresentation(view, std::move(streamed));
  settle();
  result &= expect(
      impact == PresentationImpact::GeometryChanged && modelRow &&
          materializedCard(view, modelRow->stableKey) == retained &&
          retained->findChild<MarkdownTextView *>()->document() == document &&
          view.property("conversationCardConstructions").toULongLong() ==
              constructions,
      "streaming updates the same card and native Markdown document in place");

  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  settle();
  const auto topCards = view.findChildren<ConversationCard *>();
  result &= expect(
      static_cast<int>(topCards.size()) <= 48 &&
          std::ranges::all_of(topCards,
                              [&view](ConversationCard *card) {
                                return card->parentWidget() == view.viewport();
                              }),
      "scrolling recycles residency without introducing another renderer");
  return result;
}

bool hiddenResidentPresentationPrecedesScroll() {
  ConversationView view;
  view.resize(820, 420);
  view.show();

  bool result = expect(changed(view.reconcile(conversation(240))),
                       "hidden-resident presentation fixture reconciles");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  result &= expect(waitForResidency(view),
                   "hidden-resident fixture settles its overscan window");

  ConversationCard *hidden = nullptr;
  QModelIndex hiddenIndex;
  for (ConversationCard *candidate :
       view.viewport()->findChildren<ConversationCard *>(
           QString{}, Qt::FindDirectChildrenOnly)) {
    if (!candidate->isHidden())
      continue;
    const QModelIndex index = view.conversationModel()->indexForStableKey(
        stableKey(candidate->data().key));
    if (!index.isValid())
      continue;
    hidden = candidate;
    hiddenIndex = index;
    break;
  }
  result &= expect(hidden && hiddenIndex.isValid(),
                   "the settled window contains a hidden resident card");
  if (!hidden || !hiddenIndex.isValid())
    return false;
  const std::string hiddenKey = stableKey(hidden->data().key);
  QPointer<ConversationCard> hiddenIdentity = hidden;

  VisibleCardData update = *view.conversationModel()->card(hiddenIndex.row());
  const QString expectedMarkdown =
      QString::fromStdString(std::get<AgentMessageData>(update.payload).text) +
      QStringLiteral("\n\nIncoming presentation while hidden.");
  std::get<AgentMessageData>(update.payload).text =
      expectedMarkdown.toStdString();
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
  result &= expect(applyPresentation(view, std::move(update)).has_value(),
                   "the hidden resident accepts its presentation delta");

  const auto residentGeometryMatchesModel = [&view] {
    return std::ranges::all_of(
        view.viewport()->findChildren<ConversationCard *>(
            QString{}, Qt::FindDirectChildrenOnly),
        [&view](ConversationCard *card) {
          const QModelIndex index = view.conversationModel()->indexForStableKey(
              stableKey(card->data().key));
          return index.isValid() && card->geometry() == view.visualRect(index);
        });
  };
  QPointer<MarkdownTextView> body = hidden->findChild<MarkdownTextView *>();
  result &= expect(
      hidden->data() == *view.conversationModel()->card(hiddenIndex.row()) &&
          body && body->markdownSource() == expectedMarkdown &&
          residentGeometryMatchesModel(),
      "every resident card is current and geometrically authoritative before "
      "scrolling");

  view.scrollTo(hiddenIndex, QAbstractItemView::PositionAtCenter);
  result &= expect(
      hiddenIdentity && materializedCard(view, hiddenKey) == hiddenIdentity &&
          hiddenIdentity->data() ==
              *view.conversationModel()->card(hiddenIndex.row()) &&
          body->markdownSource() == expectedMarkdown &&
          view.property("conversationCardConstructions").toULongLong() ==
              constructions,
      "scrolling exposes the already-current resident without renderer work");
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
  VisibleCardData steering{
      LocalPromptKey{812}, CardKind::UserMessage,
      "virtual-thread",    "steering-turn",
      "provider-steering", UserMessageData{"Acknowledged steering"}};
  const std::string steeringKey = stableKey(steering.key);
  active.cards.push_back(std::move(steering));
  snapshot.activeTurnId = active.turnId;
  snapshot.sections.push_back(std::move(active));

  ConversationView view;
  view.resize(820, 360);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
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

  ConversationRowPlacement activity;
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
  const std::string activityKey = stableKey(activity.card.key);
  result &= expect(appendProjected(view, std::move(activity)),
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
          view.visualRect(activityIndex).bottom() <=
              view.viewport()->height() &&
          view.mode() == ConversationView::Mode::Following,
      "an acknowledged steering card moves upward when following activity "
      "arrives instead of remaining pinned to the viewport bottom");
  return result;
}

bool singleRendererInteractionTargetsVisibleChildren() {
  const QString source = QStringLiteral(
      "Select this text and use [the link](https://example.com).");
  ConversationView view;
  view.resize(820, 320);
  view.show();
  bool result = expect(changed(view.reconcile(singleMessageConversation(
                           "interaction", source.toStdString()))),
                       "single-renderer interaction fixture reconciles");
  settle();

  const QModelIndex index = view.conversationModel()->index(0);
  const auto *row = view.conversationModel()->row(0);
  ConversationCard *const card =
      row ? materializedCard(view, row->stableKey) : nullptr;
  auto *body = card ? card->findChild<MarkdownTextView *>() : nullptr;
  auto *copy =
      card ? card->findChild<QToolButton *>(QStringLiteral("cardCopyButton"))
           : nullptr;
  auto *disclosure = card ? card->findChild<QToolButton *>(
                                QStringLiteral("cardDisclosureButton"))
                          : nullptr;
  const QRect initialGeometry = card ? card->geometry() : QRect{};
  result &= expect(card && body && copy && disclosure &&
                       view.visualRect(index) == initialGeometry,
                   "the visible row is interactive before the first gesture");

  if (body) {
    QTextCursor selection(body->document());
    selection.setPosition(6, QTextCursor::KeepAnchor);
    body->setTextCursor(selection);
    body->setFocus(Qt::TabFocusReason);
    QKeyEvent copySelection(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(body, &copySelection);
  }
  settle();
  result &= expect(
      QApplication::clipboard()->text() == QStringLiteral("Select") && card &&
          row && materializedCard(view, row->stableKey) == card &&
          card->geometry() == initialGeometry &&
          body->textCursor().hasSelection(),
      "selection and Ctrl+C stay on the same Markdown object and geometry");

  const auto clickControl = [](QToolButton *control) {
    if (!control)
      return;
    const QPoint local = control->rect().center();
    const QPoint global = control->mapToGlobal(local);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(control, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local),
                        QPointF(global), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(control, &release);
  };
  clickControl(copy);
  settle();
  result &= expect(
      QApplication::clipboard()->text() == source && row &&
          materializedCard(view, row->stableKey) == card && copy &&
          QRect(copy->mapTo(card, QPoint{}), copy->size())
              .intersects(card->contentsRect()),
      "the visible Copy hit geometry activates directly on the authoritative "
      "card");

  QStringList activatedLinks;
  if (body) {
    body->setOpenLinks(false);
    body->setOpenExternalLinks(false);
    QObject::connect(body, &QTextBrowser::anchorClicked, body,
                     [&activatedLinks](const QUrl &url) {
                       activatedLinks.push_back(url.toString());
                     });
  }
  const int linkPosition =
      body ? body->toPlainText().indexOf(QStringLiteral("the link")) + 1 : -1;
  QTextCursor linkCursor = body ? QTextCursor(body->document()) : QTextCursor{};
  QPoint linkPoint;
  bool linkPointerFeedback = false;
  if (body && linkPosition > 0) {
    linkCursor.setPosition(linkPosition);
    const QPoint origin = body->cursorRect(linkCursor).center();
    for (int offset = -2; offset <= body->fontMetrics().averageCharWidth();
         ++offset) {
      const QPoint candidate = origin + QPoint(offset, 0);
      if (body->anchorAt(candidate) == QStringLiteral("https://example.com")) {
        linkPoint = candidate;
        break;
      }
    }
    QMouseEvent move(QEvent::MouseMove, QPointF(linkPoint),
                     QPointF(body->viewport()->mapToGlobal(linkPoint)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(body->viewport(), &move);
    linkPointerFeedback =
        body->viewport()->cursor().shape() == Qt::PointingHandCursor;
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(linkPoint),
                      QPointF(body->viewport()->mapToGlobal(linkPoint)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(body->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(linkPoint),
                        QPointF(body->viewport()->mapToGlobal(linkPoint)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(body->viewport(), &release);
  }
  settle();
  const bool pointerLinkActivated =
      body && !linkPoint.isNull() &&
      body->anchorAt(linkPoint) == QStringLiteral("https://example.com") &&
      linkPointerFeedback &&
      activatedLinks == QStringList{QStringLiteral("https://example.com")};
  if (!pointerLinkActivated)
    std::cerr << "markdown link position=" << linkPosition
              << " point=" << linkPoint.x() << ',' << linkPoint.y()
              << " anchor='"
              << (body ? body->anchorAt(linkPoint).toStdString() : "missing")
              << "' pointing=" << linkPointerFeedback
              << " activations=" << activatedLinks.join('|').toStdString()
              << '\n';
  result &= expect(
      pointerLinkActivated,
      "the sole Markdown widget owns link hit testing, pointer feedback, and "
      "activation");
  if (body && linkPosition > 0) {
    QTextCursor documentStart(body->document());
    documentStart.movePosition(QTextCursor::Start);
    body->setTextCursor(documentStart);
    body->setFocus(Qt::TabFocusReason);
    QKeyEvent nextLink(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(body, &nextLink);
    QKeyEvent activate(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(body, &activate);
  }
  settle();
  const bool keyboardLinkActivated =
      activatedLinks == QStringList{QStringLiteral("https://example.com"),
                                    QStringLiteral("https://example.com")};
  if (!keyboardLinkActivated)
    std::cerr << "markdown keyboard activations="
              << activatedLinks.join('|').toStdString() << '\n';
  result &= expect(
      keyboardLinkActivated,
      "the same Markdown link activates from the keyboard without another "
      "interaction implementation");

  const bool wasCollapsed = card && card->isCollapsed();
  clickControl(disclosure);
  settle();
  result &= expect(
      card && row && materializedCard(view, row->stableKey) == card &&
          card->isCollapsed() != wasCollapsed,
      "disclosure changes state without replacing the interacted-with card");

  const VisibleCardData unchanged = view.conversationModel()->card(0)
                                        ? *view.conversationModel()->card(0)
                                        : VisibleCardData{};
  const QRect collapsedGeometry = card ? card->geometry() : QRect{};
  QWidget *const focusedBeforeNoop = QApplication::focusWidget();
  result &=
      expect(applyPresentation(view, unchanged) == PresentationImpact::None,
             "a focused card accepts an identical update as a no-op");
  settle();
  result &=
      expect(card && row && materializedCard(view, row->stableKey) == card &&
                 card->geometry() == collapsedGeometry &&
                 QApplication::focusWidget() == focusedBeforeNoop,
             "no-op update preserves focused object and exact pixels");
  return result;
}

bool collapsedAgentPhaseSettlesAuthoritativeGeometry() {
  VisibleCardData update{
      AuthoritativeItemKey{"collapsed-phase", "turn", "message"},
      CardKind::AgentMessage,
      "collapsed-phase",
      "turn",
      "message",
      AgentMessageData{"Streaming answer", false}};
  ConversationSnapshot snapshot;
  snapshot.threadId = update.threadId;
  snapshot.sections.push_back({"section", "turn", {update}, std::nullopt});

  ConversationView view;
  view.resize(820, 320);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "collapsed phase fixture reconciles");
  settle();

  const QModelIndex index = view.conversationModel()->index(0);
  const auto *row = view.conversationModel()->row(0);
  ConversationCard *const card =
      row ? materializedCard(view, row->stableKey) : nullptr;
  auto *disclosure = card ? card->findChild<QToolButton *>(
                                QStringLiteral("cardDisclosureButton"))
                          : nullptr;
  if (disclosure)
    disclosure->click();
  settle();
  const QRect before = view.visualRect(index);

  VisibleCardData finalAnswer = update;
  std::get<AgentMessageData>(finalAnswer.payload).finalAnswer = true;
  ConversationCard expected(finalAnswer, true, view.viewport(),
                            card ? card->width() : view.viewport()->width());
  expected.ensurePolished();
  const int expectedHeight = expected.settleHeightForWidth(
      card ? card->width() : view.viewport()->width());
  const auto impact = applyPresentation(view, std::move(finalAnswer));
  settle();
  const QRect after = view.visualRect(index);
  result &= expect(
      card && disclosure && card->isCollapsed() &&
          impact == PresentationImpact::GeometryChanged &&
          materializedCard(view, row->stableKey) == card &&
          after.height() == expectedHeight &&
          card->height() == expectedHeight && after.height() != before.height(),
      "a collapsed update-to-final transition keeps its renderer and settles "
      "the height index to the measured header geometry");
  return result;
}

bool semanticSelectionsSurviveRecycling() {
  const std::string thread = "selection-recycling";
  const VisibleCardData markdown{
      AuthoritativeItemKey{thread, "markdown-turn", "markdown"},
      CardKind::AgentMessage,
      thread,
      "markdown-turn",
      "markdown",
      AgentMessageData{"Select this Markdown payload across residency.\n\n"
                       "Mutable streaming tail.",
                       true}};
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "command-turn", "command"},
      CardKind::CommandExecution,
      thread,
      "command-turn",
      "command",
      CommandExecutionData{
          "printf semantic-command-selection", "done", "/workspace", 0, {}},
      nodegraph::NodeStatus::Completed};
  const VisibleCardData files{
      AuthoritativeItemKey{thread, "files-turn", "files"},
      CardKind::FileChanges,
      thread,
      "files-turn",
      "files",
      FileChangesData{{{"src/semantic-selection.cpp", "update", 4, 1}},
                      "/workspace"},
      nodegraph::NodeStatus::Completed};

  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections = {
      {"markdown-section", "markdown-turn", {markdown}, std::nullopt},
      {"command-section", "command-turn", {command}, std::nullopt},
      {"files-section", "files-turn", {files}, std::nullopt}};
  for (int serial = 0; serial < 180; ++serial) {
    const std::string suffix = std::to_string(serial);
    snapshot.sections.push_back(
        {"filler-section-" + suffix,
         "filler-turn-" + suffix,
         {{AuthoritativeItemKey{thread, "filler-turn-" + suffix,
                                "filler-" + suffix},
           CardKind::AgentMessage, thread, "filler-turn-" + suffix,
           "filler-" + suffix, AgentMessageData{std::string(180, 'x'), true}}},
         std::nullopt});
  }

  ConversationView view;
  ConversationView::PresentationOptions options;
  options.commandsInitiallyExpanded = true;
  options.fileChangesInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(820, 480);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "selection recycling fixture reconciles");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  settle();

  const std::string markdownKey = stableKey(markdown.key);
  const std::string commandKey = stableKey(command.key);
  const std::string filesKey = stableKey(files.key);
  ConversationCard *markdownCard = materializedCard(view, markdownKey);
  ConversationCard *commandCard = materializedCard(view, commandKey);
  ConversationCard *filesCard = materializedCard(view, filesKey);
  auto *markdownText =
      markdownCard ? markdownCard->findChild<MarkdownTextView *>() : nullptr;
  auto *commandText = commandCard ? commandCard->findChild<QTextEdit *>(
                                        QStringLiteral("commandTextView"))
                                  : nullptr;
  auto *filesText = filesCard ? filesCard->findChild<QTextBrowser *>(
                                    QStringLiteral("fileChangesList"))
                              : nullptr;

  if (markdownText) {
    QTextCursor begin(markdownText->document());
    begin.setPosition(0);
    QTextCursor end(markdownText->document());
    end.setPosition(6);
    const QPoint beginPoint = markdownText->cursorRect(begin).center();
    const QPoint endPoint = markdownText->cursorRect(end).center();
    QWidget *surface = markdownText->viewport();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(beginPoint),
                      QPointF(beginPoint), surface->mapToGlobal(beginPoint),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(surface, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(endPoint), QPointF(endPoint),
                     surface->mapToGlobal(endPoint), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(surface, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(endPoint),
                        QPointF(endPoint), surface->mapToGlobal(endPoint),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(surface, &release);
  }
  if (commandText) {
    QTextCursor cursor(commandText->document());
    cursor.setPosition(2);
    cursor.setPosition(10, QTextCursor::KeepAnchor);
    commandText->setTextCursor(cursor);
  }
  if (filesText) {
    QTextCursor cursor(filesText->document());
    cursor.setPosition(4);
    cursor.setPosition(16, QTextCursor::KeepAnchor);
    filesText->setTextCursor(cursor);
  }
  const QTextCursor markdownSelection =
      markdownText ? markdownText->textCursor() : QTextCursor{};
  const QTextCursor commandSelection =
      commandText ? commandText->textCursor() : QTextCursor{};
  const QTextCursor filesSelection =
      filesText ? filesText->textCursor() : QTextCursor{};
  const QString markdownSelectedText = markdownSelection.selectedText();
  const int markdownSelectionPosition = markdownSelection.position();
  const int markdownSelectionAnchor = markdownSelection.anchor();
  const QString filesSelectedText = filesSelection.selectedText();
  result &= expect(markdownSelection.hasSelection() &&
                       commandSelection.hasSelection() &&
                       filesSelection.hasSelection(),
                   "mouse and keyboard text selections are established on "
                   "semantic card controls");
  const auto hasBoundedFingerprints = [](const ConversationCard *card) {
    if (!card)
      return false;
    const ConversationCard::State state = card->state();
    return !state.selections.empty() &&
           std::ranges::all_of(state.selections, [](const auto &selection) {
             return selection.source.length > 0 &&
                    selection.source.digest.size() == 32;
           });
  };
  result &= expect(hasBoundedFingerprints(markdownCard) &&
                       hasBoundedFingerprints(commandCard) &&
                       hasBoundedFingerprints(filesCard),
                   "retained selections store fixed-size semantic source "
                   "fingerprints instead of complete content copies");

  const auto fold = [](ConversationCard *card) {
    QToolButton *const disclosure =
        card ? card->findChild<QToolButton *>(
                   QStringLiteral("cardDisclosureButton"))
             : nullptr;
    if (disclosure)
      disclosure->click();
  };
  fold(markdownCard);
  fold(commandCard);
  fold(filesCard);
  settle();
  result &= expect(markdownCard && markdownCard->isCollapsed() && commandCard &&
                       commandCard->isCollapsed() && filesCard &&
                       filesCard->isCollapsed(),
                   "selected card bodies fold before leaving residency");

  QPointer<ConversationCard> releasedMarkdown(markdownCard);
  QPointer<ConversationCard> releasedCommand(commandCard);
  QPointer<ConversationCard> releasedFiles(filesCard);
  view.setFocus(Qt::OtherFocusReason);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  result &= expect(waitUntil(
                       [&] {
                         return releasedMarkdown.isNull() &&
                                releasedCommand.isNull() &&
                                releasedFiles.isNull();
                       },
                       2000),
                   "offscreen semantic selections do not pin card residency");

  VisibleCardData appendedMarkdown = markdown;
  std::get<AgentMessageData>(appendedMarkdown.payload).text +=
      " Appended streaming text.";
  VisibleCardData replacedCommand = command;
  std::get<CommandExecutionData>(replacedCommand.payload).command =
      "a different command owner";
  VisibleCardData retargetedFiles = files;
  std::get<FileChangesData>(retargetedFiles.payload).cwd = "/a/new/workspace";
  result &= expect(applyPresentation(view, appendedMarkdown).has_value() &&
                       applyPresentation(view, replacedCommand).has_value() &&
                       applyPresentation(view, retargetedFiles).has_value(),
                   "offscreen semantic owners accept append, replacement, "
                   "and link-target-only updates");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  result &= expect(waitUntil(
                       [&] {
                         return materializedCard(view, markdownKey) &&
                                materializedCard(view, commandKey) &&
                                materializedCard(view, filesKey);
                       },
                       2000),
                   "selected cards rematerialize through the bounded window");

  markdownCard = materializedCard(view, markdownKey);
  commandCard = materializedCard(view, commandKey);
  filesCard = materializedCard(view, filesKey);
  result &= expect(
      markdownCard && markdownCard->isCollapsed() && commandCard &&
          commandCard->isCollapsed() && filesCard && filesCard->isCollapsed(),
      "fold state survives release and deferred rematerialization");
  fold(markdownCard);
  fold(commandCard);
  fold(filesCard);
  settle();
  markdownText =
      markdownCard ? markdownCard->findChild<MarkdownTextView *>() : nullptr;
  commandText = commandCard ? commandCard->findChild<QTextEdit *>(
                                  QStringLiteral("commandTextView"))
                            : nullptr;
  filesText = filesCard ? filesCard->findChild<QTextBrowser *>(
                              QStringLiteral("fileChangesList"))
                        : nullptr;
  const QTextCursor restoredMarkdown =
      markdownText ? markdownText->textCursor() : QTextCursor{};
  const QTextCursor restoredCommand =
      commandText ? commandText->textCursor() : QTextCursor{};
  const QTextCursor restoredFiles =
      filesText ? filesText->textCursor() : QTextCursor{};
  const bool selectionsRestored =
      markdownText && commandText && filesText &&
      restoredMarkdown.selectedText() == markdownSelectedText &&
      restoredMarkdown.position() == markdownSelectionPosition &&
      restoredMarkdown.anchor() == markdownSelectionAnchor &&
      !restoredCommand.hasSelection() &&
      restoredFiles.selectedText() == filesSelectedText;
  if (!selectionsRestored)
    std::cerr << "selection restore markdown=" << restoredMarkdown.position()
              << ':' << restoredMarkdown.anchor() << '/'
              << markdownSelectionPosition << ':' << markdownSelectionAnchor
              << " command='" << restoredCommand.selectedText().toStdString()
              << "'/<cleared> files='"
              << restoredFiles.selectedText().toStdString() << "'/'"
              << filesSelectedText.toStdString() << "'\n";
  result &= expect(
      selectionsRestored,
      "folded recycling restores immutable-prefix and link-target selections "
      "after deferred expansion while clearing a replaced semantic owner");

  QPointer<ConversationCard> truncatedCard(markdownCard);
  view.setFocus(Qt::OtherFocusReason);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  result &= expect(waitUntil([&] { return truncatedCard.isNull(); }, 2000),
                   "the selected Markdown card leaves residency again");
  VisibleCardData truncatedMarkdown = appendedMarkdown;
  std::get<AgentMessageData>(truncatedMarkdown.payload).text = "Short";
  VisibleCardData regrownMarkdown = truncatedMarkdown;
  std::get<AgentMessageData>(regrownMarkdown.payload).text =
      std::get<AgentMessageData>(appendedMarkdown.payload).text +
      " Regrown after truncation.";
  result &= expect(applyPresentation(view, truncatedMarkdown).has_value() &&
                       applyPresentation(view, regrownMarkdown).has_value(),
                   "an offscreen owner can truncate and later regrow");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  result &= expect(
      waitUntil([&] { return materializedCard(view, markdownKey); }, 2000),
      "the truncated-and-regrown card rematerializes");
  markdownCard = materializedCard(view, markdownKey);
  markdownText =
      markdownCard ? markdownCard->findChild<MarkdownTextView *>() : nullptr;
  result &= expect(markdownText && !markdownText->textCursor().hasSelection(),
                   "truncation retires selection so later growth cannot "
                   "resurrect it");

  if (markdownText)
    markdownText->setFocus(Qt::TabFocusReason);
  QPointer<ConversationCard> focusedRetirement(markdownCard);
  view.forgetThreadPresentation(thread);
  view.resize(view.width() + 1, view.height());
  settle();
  result &=
      expect(focusedRetirement.isNull() && view.materializedCardCount() == 0 &&
                 view.conversationModel()->rowCount() == 0 &&
                 view.conversationModel()->threadId().empty() &&
                 view.verticalScrollBar()->maximum() == 0 &&
                 view.presentedThreadId().empty(),
             "retiring the presented thread safely removes focused "
             "cards, model rows, and cached geometry across reflow");
  result &= expect(changed(view.reconcile(
                       singleMessageConversation("selection-other", "Other"))),
                   "selection fixture switches after active retirement");
  result &= expect(changed(view.reconcile(snapshot)),
                   "retired selection thread can be presented afresh");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  settle();
  markdownCard = materializedCard(view, markdownKey);
  markdownText =
      markdownCard ? markdownCard->findChild<MarkdownTextView *>() : nullptr;
  result &= expect(markdownText && !markdownText->textCursor().hasSelection(),
                   "thread retirement deletes retained card interaction state");
  return result;
}

bool markdownCompletionCannotRetargetSelection() {
  struct Fixture {
    const char *before;
    const char *after;
    const char *selected;
  };
  const Fixture fixtures[] = {
      {"Start **aaaaaa", "Start **aaaaaa**", "aa"},
      {"Select [link", "Select [link](https://example.com)", "link"},
      {"Select `code", "Select `code`", "code"},
      {"[aaaa][ref]\n\nTail",
       "[aaaa][ref]\n\nTail\n\n[ref]: https://example.com", "aa"},
  };
  const std::string thread = "markdown-completion";
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  for (std::size_t index = 0; index < std::size(fixtures); ++index) {
    const std::string suffix = std::to_string(index);
    VisibleCardData card{AuthoritativeItemKey{thread, "syntax-turn-" + suffix,
                                              "syntax-item-" + suffix},
                         CardKind::AgentMessage,
                         thread,
                         "syntax-turn-" + suffix,
                         "syntax-item-" + suffix,
                         AgentMessageData{fixtures[index].before, true}};
    snapshot.sections.push_back({"syntax-section-" + suffix,
                                 card.turnId,
                                 {std::move(card)},
                                 std::nullopt});
  }
  for (int index = 0; index < 180; ++index) {
    const std::string suffix = std::to_string(index);
    VisibleCardData card{AuthoritativeItemKey{thread, "filler-turn-" + suffix,
                                              "filler-item-" + suffix},
                         CardKind::AgentMessage,
                         thread,
                         "filler-turn-" + suffix,
                         "filler-item-" + suffix,
                         AgentMessageData{std::string(180, 'x'), true}};
    snapshot.sections.push_back({"filler-section-" + suffix,
                                 card.turnId,
                                 {std::move(card)},
                                 std::nullopt});
  }

  ConversationView view;
  view.resize(820, 480);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "Markdown completion fixture reconciles");
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  settle();

  ConversationCard *cards[std::size(fixtures)]{};
  MarkdownTextView *texts[std::size(fixtures)]{};
  bool selectionsReady = true;
  for (std::size_t index = 0; index < std::size(fixtures); ++index) {
    const std::string key =
        stableKey(snapshot.sections[index].cards.front().key);
    cards[index] = materializedCard(view, key);
    texts[index] =
        cards[index] ? cards[index]->findChild<MarkdownTextView *>() : nullptr;
    const QString selected = QString::fromLatin1(fixtures[index].selected);
    const int start =
        texts[index] ? texts[index]->document()->toPlainText().indexOf(selected)
                     : -1;
    if (!texts[index] || start < 0) {
      selectionsReady = false;
      continue;
    }
    QTextCursor cursor(texts[index]->document());
    cursor.setPosition(start);
    cursor.setPosition(start + selected.size(), QTextCursor::KeepAnchor);
    texts[index]->setTextCursor(cursor);
    selectionsReady = selectionsReady &&
                      texts[index]->textCursor().selectedText() == selected;
  }
  result &= expect(selectionsReady,
                   "incomplete Markdown exposes the selected literal text");

  VisibleCardData residentUpdate = snapshot.sections[0].cards.front();
  std::get<AgentMessageData>(residentUpdate.payload).text = fixtures[0].after;
  QPointer<ConversationCard> residentIdentity(cards[0]);
  result &= expect(applyPresentation(view, residentUpdate).has_value() &&
                       residentIdentity == cards[0] && texts[0] &&
                       !texts[0]->textCursor().hasSelection(),
                   "completing repeated emphasis in a resident Markdown "
                   "document cannot retarget the old cursor offsets");

  QPointer<ConversationCard> recycledLink(cards[1]);
  QPointer<ConversationCard> recycledCode(cards[2]);
  view.setFocus(Qt::OtherFocusReason);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  result &= expect(
      waitUntil([&] { return recycledLink.isNull() && recycledCode.isNull(); },
                2000),
      "selected Markdown tails can leave widget residency");
  for (std::size_t index = 1; index < std::size(fixtures); ++index) {
    VisibleCardData update = snapshot.sections[index].cards.front();
    std::get<AgentMessageData>(update.payload).text = fixtures[index].after;
    result &= expect(applyPresentation(view, std::move(update)).has_value(),
                     "offscreen Markdown accepts syntax completion");
  }
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  result &= expect(
      waitUntil(
          [&] {
            for (std::size_t index = 1; index < std::size(fixtures); ++index) {
              const std::string key =
                  stableKey(snapshot.sections[index].cards.front().key);
              ConversationCard *card = materializedCard(view, key);
              MarkdownTextView *text =
                  card ? card->findChild<MarkdownTextView *>() : nullptr;
              if (!text || text->textCursor().hasSelection())
                return false;
            }
            return true;
          },
          2000),
      "link, code, and reference completion cannot resurrect "
      "an offscreen selection at shifted rendered offsets");
  return result;
}

bool keyboardNavigationAndViewIsolation() {
  const std::string thread = "keyboard-navigation";
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  for (int row = 0; row < 8; ++row) {
    const std::string suffix = std::to_string(row);
    const CardKind kind =
        row == 1 ? CardKind::Reasoning : CardKind::AgentMessage;
    const CardPayload payload =
        kind == CardKind::Reasoning
            ? CardPayload{ReasoningData{"hidden reasoning"}}
            : CardPayload{AgentMessageData{"Visible row " + suffix, true}};
    snapshot.sections.push_back(
        {"section-" + suffix,
         "turn-" + suffix,
         {{AuthoritativeItemKey{thread, "turn-" + suffix, "item-" + suffix},
           kind, thread, "turn-" + suffix, "item-" + suffix, payload}},
         std::nullopt});
  }

  ConversationView first;
  ConversationView::PresentationOptions options;
  options.showReasoning = false;
  first.setPresentationOptions(options);
  first.resize(520, 180);
  first.show();
  bool result = expect(changed(first.reconcile(snapshot)),
                       "keyboard navigation fixture reconciles");
  bool hasExpectedDpr = false;
  const qreal expectedDpr =
      qEnvironmentVariable("QT_SCALE_FACTOR").toDouble(&hasExpectedDpr);
  if (hasExpectedDpr)
    result &= expect(std::abs(first.devicePixelRatioF() - expectedDpr) < 0.02,
                     "focus fixture uses the requested device-pixel ratio");
  first.verticalScrollBar()->setValue(first.verticalScrollBar()->minimum());
  settle();
  const QModelIndex firstIndex = first.conversationModel()->index(0);
  QPointer<ConversationCard> initialFirstCard = materializedCard(
      first, firstIndex.data(ConversationItemModel::StableKeyRole)
                 .toString()
                 .toStdString());
  const QPoint mouseSelectionPoint =
      initialFirstCard ? initialFirstCard->rect().center() : QPoint{};
  const auto selectFirstWithMouse = [&] {
    if (!initialFirstCard)
      return;
    first.setFocus(Qt::MouseFocusReason);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(mouseSelectionPoint),
                      QPointF(mouseSelectionPoint),
                      initialFirstCard->mapToGlobal(mouseSelectionPoint),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(initialFirstCard, &press);
    QMouseEvent release(QEvent::MouseButtonRelease,
                        QPointF(mouseSelectionPoint),
                        QPointF(mouseSelectionPoint),
                        initialFirstCard->mapToGlobal(mouseSelectionPoint),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(initialFirstCard, &release);
    settle();
  };
  selectFirstWithMouse();
  first.selectionModel()->clear();
  settle();
  const QRect firstCardGeometry =
      initialFirstCard ? initialFirstCard->geometry() : QRect{};
  const QImage unselectedPixels =
      initialFirstCard ? initialFirstCard->grab().toImage() : QImage{};
  auto *firstList = QAccessible::queryAccessibleInterface(first.viewport());
  QAccessibleInterface *const firstAccessible =
      firstList ? firstList->child(firstIndex.row()) : nullptr;
  const QAccessible::Id firstAccessibleId =
      firstAccessible ? QAccessible::uniqueId(firstAccessible) : 0;
  selectFirstWithMouse();
  const QImage mouseSelectedPixels =
      initialFirstCard ? initialFirstCard->grab().toImage() : QImage{};
  result &= expect(
      initialFirstCard && initialFirstCard->geometry() == firstCardGeometry &&
          mouseSelectedPixels == unselectedPixels &&
          first.selectionModel()->isSelected(firstIndex) && firstAccessible &&
          firstAccessible->state().selected && firstAccessibleId &&
          QAccessible::accessibleInterface(firstAccessibleId) ==
              firstAccessible,
      "visible-card mouse selection changes only semantic and accessible "
      "selection");
  selectFirstWithMouse();
  result &= expect(
      initialFirstCard && initialFirstCard->geometry() == firstCardGeometry &&
          initialFirstCard->grab().toImage() == mouseSelectedPixels &&
          QAccessible::accessibleInterface(firstAccessibleId) ==
              firstAccessible,
      "repeated mouse selection preserves pixels, geometry, and accessible "
      "identity");

  const auto sendKey = [](QWidget &target, int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&target, &event);
    QApplication::processEvents();
  };
  sendKey(first, Qt::Key_Down);
  ConversationCard *selected =
      materializedCard(first, first.conversationModel()
                                  ->index(2)
                                  .data(ConversationItemModel::StableKeyRole)
                                  .toString()
                                  .toStdString());
  result &=
      expect(first.currentIndex().row() == 2 && selected &&
                 first.selectionModel()->isSelected(first.currentIndex()) &&
                 first.visualRect(first.currentIndex())
                     .intersects(first.viewport()->rect()),
             "Down skips a hidden row and selects it semantically");
  sendKey(first, Qt::Key_Up);
  settle();
  const QImage keyboardFocusedPixels =
      initialFirstCard ? initialFirstCard->grab().toImage() : QImage{};
  const bool keyboardFocusVisible =
      first.currentIndex().row() == 0 && initialFirstCard &&
      initialFirstCard->geometry() == firstCardGeometry &&
      keyboardFocusedPixels != mouseSelectedPixels;
  if (!keyboardFocusVisible)
    std::cerr << "keyboard focus diagnostics: current="
              << first.currentIndex().row()
              << " view_focus=" << first.hasFocus() << " keyboard_mode="
              << first.window()->testAttribute(Qt::WA_KeyboardFocusChange)
              << " card=" << !initialFirstCard.isNull() << " geometry="
              << (initialFirstCard &&
                  initialFirstCard->geometry() == firstCardGeometry)
              << " pixels_changed="
              << (initialFirstCard &&
                  keyboardFocusedPixels != mouseSelectedPixels)
              << '\n';
  result &= expect(keyboardFocusVisible,
                   "Up returns to the preceding row with keyboard-only focus "
                   "pixels");
  first.clearFocus();
  settle();
  const QImage unfocusedPixels =
      initialFirstCard ? initialFirstCard->grab().toImage() : QImage{};
  result &= expect(initialFirstCard && unfocusedPixels != keyboardFocusedPixels,
                   "losing view focus removes the keyboard focus pixels");
  selectFirstWithMouse();
  result &= expect(initialFirstCard && initialFirstCard->grab().toImage() ==
                                           mouseSelectedPixels,
                   "mouse refocus does not restore keyboard focus pixels");
  sendKey(first, Qt::Key_Up);
  result &= expect(first.currentIndex().row() == 0,
                   "Up at the first presented row does not wrap");
  sendKey(first, Qt::Key_End);
  result &= expect(first.currentIndex().row() == 7 &&
                       first.visualRect(first.currentIndex())
                           .intersects(first.viewport()->rect()),
                   "End selects and reveals the final presented row");
  sendKey(first, Qt::Key_Down);
  result &= expect(first.currentIndex().row() == 7,
                   "Down at the final presented row does not wrap");
  sendKey(first, Qt::Key_Home);
  result &= expect(first.currentIndex().row() == 0,
                   "Home selects the first presented row");
  sendKey(first, Qt::Key_PageDown);
  const int pageDownRow = first.currentIndex().row();
  result &= expect(pageDownRow > 0 && pageDownRow != 1 &&
                       first.visualRect(first.currentIndex())
                           .intersects(first.viewport()->rect()),
                   "PageDown resolves row spacing through the height index");
  sendKey(first, Qt::Key_PageUp);
  result &= expect(first.currentIndex().row() < pageDownRow &&
                       first.currentIndex().row() != 1,
                   "PageUp resolves the opposite viewport edge through the "
                   "same geometry authority");
  sendKey(first, Qt::Key_Home);

  bool hiddenOverscan = false;
  for (ConversationCard *card : first.findChildren<ConversationCard *>()) {
    const QRect geometry(card->mapTo(first.viewport(), QPoint{}), card->size());
    if (!geometry.intersects(first.viewport()->rect())) {
      hiddenOverscan = true;
      result &= expect(card->isHidden(),
                       "non-focused overscan cards are absent from traversal");
    }
  }
  result &= expect(hiddenOverscan,
                   "the fixture contains hidden resident overscan cards");

  QPointer<ConversationCard> firstCard =
      materializedCard(first, first.conversationModel()
                                  ->index(0)
                                  .data(ConversationItemModel::StableKeyRole)
                                  .toString()
                                  .toStdString());
  QPointer<QToolButton> firstCopy = firstCard
                                        ? firstCard->findChild<QToolButton *>(
                                              QStringLiteral("cardCopyButton"))
                                        : nullptr;
  QPointer<QToolButton> firstDisclosure =
      firstCard ? firstCard->findChild<QToolButton *>(
                      QStringLiteral("cardDisclosureButton"))
                : nullptr;
  if (firstCopy)
    firstCopy->setFocus(Qt::TabFocusReason);
  settle();
  if (QWidget *focused = QApplication::focusWidget()) {
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(focused, &tab);
  }
  settle();
  result &= expect(!first.tabKeyNavigation() && firstDisclosure &&
                       QApplication::focusWidget() == firstDisclosure &&
                       first.currentIndex().row() == 0,
                   "Tab traverses real controls without independently moving "
                   "the item-view current row");

  if (firstCopy)
    firstCopy->setFocus(Qt::TabFocusReason);
  first.verticalScrollBar()->setValue(first.verticalScrollBar()->maximum());
  settle();
  const QRect pinnedGeometry =
      firstCard ? QRect(firstCard->mapTo(first.viewport(), QPoint{}),
                        firstCard->size())
                : QRect{};
  const QPersistentModelIndex pinnedSelection =
      first.selectionModel()->selectedIndexes().isEmpty()
          ? QPersistentModelIndex{}
          : QPersistentModelIndex(
                first.selectionModel()->selectedIndexes().front());
  QPointer<ConversationCard> pinnedCard = firstCard;
  result &= expect(firstCard && firstCopy && firstCopy->hasFocus() &&
                       !firstCard->isHidden() &&
                       !pinnedGeometry.intersects(first.viewport()->rect()) &&
                       !(static_cast<int>(firstCopy->focusPolicy()) &
                         static_cast<int>(Qt::TabFocus)),
                   "a focused offscreen card keeps object focus but is removed "
                   "from subsequent Tab traversal");
  if (QWidget *focused = QApplication::focusWidget()) {
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(focused, &tab);
  }
  const bool pinSurvivedKeyDispatch = !pinnedCard.isNull();
  const bool pinLeftActiveTree =
      pinnedCard && pinnedCard->parentWidget() == &first &&
      materializedCard(first, stableKey(pinnedCard->data().key)) == nullptr;
  QCoreApplication::sendPostedEvents(pinnedCard.data(), QEvent::DeferredDelete);
  settle();
  QWidget *focusedAfterPinned = QApplication::focusWidget();
  ConversationCard *focusedCard = nullptr;
  for (QWidget *candidate = focusedAfterPinned;
       candidate && candidate != first.viewport();
       candidate = candidate->parentWidget()) {
    if ((focusedCard = qobject_cast<ConversationCard *>(candidate)))
      break;
  }
  const QRect focusedGeometry =
      focusedCard ? QRect(focusedCard->mapTo(first.viewport(), QPoint{}),
                          focusedCard->size())
                  : QRect{};
  result &= expect(
      pinSurvivedKeyDispatch && pinLeftActiveTree && focusedCard &&
          focusedCard != pinnedCard.data() && pinnedCard.isNull() &&
          focusedGeometry.intersects(first.viewport()->rect()) &&
          stableKey(focusedCard->data().key) ==
              first.currentIndex()
                  .data(ConversationItemModel::StableKeyRole)
                  .toString()
                  .toStdString() &&
          pinnedSelection.isValid() &&
          first.selectionModel()->selectedIndexes() ==
              QModelIndexList{pinnedSelection},
      "Tab leaves a focused pinned card for a visible card and moves only the "
      "navigation cursor while releasing the old offscreen renderer");
  first.setFocus(Qt::OtherFocusReason);
  first.verticalScrollBar()->setValue(first.verticalScrollBar()->minimum());
  sendKey(first, Qt::Key_Home);
  settle();

  ConversationView second;
  second.setPresentationOptions(options);
  second.resize(520, 180);
  second.show();
  result &= expect(changed(second.reconcile(snapshot)),
                   "a second view presents colliding semantic keys");
  second.verticalScrollBar()->setValue(second.verticalScrollBar()->minimum());
  settle();
  ConversationCard *secondCard =
      materializedCard(second, second.conversationModel()
                                   ->index(2)
                                   .data(ConversationItemModel::StableKeyRole)
                                   .toString()
                                   .toStdString());
  auto *secondBody =
      secondCard ? secondCard->findChild<MarkdownTextView *>() : nullptr;
  if (secondBody)
    secondBody->setFocus(Qt::TabFocusReason);
  settle();
  result &= expect(first.currentIndex().row() == 0 &&
                       second.currentIndex().row() == 2 && secondCard &&
                       second.selectionModel()->selectedIndexes().isEmpty(),
                   "child focus moves only its owning view's navigation "
                   "cursor and does not create semantic selection");
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
         CardKind::AgentMessage, "lazy-scroll-anchor", "turn",
         "update-" + std::to_string(row),
         AgentMessageData{std::move(markdown), false}});
  }
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(620, 360);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "lazy-scroll anchor fixture reconciles");
  settle();
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  settle();

  const auto windowCovered = [&view](const QRect &window) {
    for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
      const QModelIndex index = view.conversationModel()->index(row);
      const ConversationItemModel::Row *modelRow =
          view.conversationModel()->row(row);
      if (!modelRow || !view.visualRect(index).intersects(window))
        continue;
      if (!materializedCard(view, modelRow->stableKey))
        return false;
    }
    return true;
  };
  result &= expect(waitForResidency(view),
                   "the initial hidden overscan reaches a fixed point");
  std::unordered_set<std::string> priorResidents;
  for (ConversationCard *card :
       view.viewport()->findChildren<ConversationCard *>(
           QString{}, Qt::FindDirectChildrenOnly))
    priorResidents.insert(stableKey(card->data().key));
  const qulonglong seekConstructions =
      view.property("conversationCardConstructions").toULongLong();
  QScrollBar *const scrollBar = view.verticalScrollBar();
  scrollBar->setValue(scrollBar->value() < scrollBar->maximum() / 2
                          ? scrollBar->maximum()
                          : scrollBar->minimum());
  int newlyVisibleRows = 0;
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const ConversationItemModel::Row *modelRow =
        view.conversationModel()->row(row);
    if (modelRow &&
        view.visualRect(view.conversationModel()->index(row))
            .intersects(view.viewport()->rect()) &&
        !priorResidents.contains(modelRow->stableKey))
      ++newlyVisibleRows;
  }
  const int height = view.viewport()->height();
  result &= expect(
      windowCovered(view.viewport()->rect()) &&
          !windowCovered(
              view.viewport()->rect().adjusted(0, -height, 0, height)) &&
          view.property("conversationCardConstructions").toULongLong() -
                  seekConstructions <=
              static_cast<qulonglong>(newlyVisibleRows),
      "a disjoint seek synchronously constructs only newly visible rows");
  result &= expect(waitForResidency(view),
                   "the disjoint seek eventually restores hidden overscan");
  const qulonglong warmConstructions =
      view.property("conversationCardConstructions").toULongLong();
  bool immediateViewportCoverage = true;
  for (int step = 0; step < 4; ++step) {
    QScrollBar *bar = view.verticalScrollBar();
    const int next = std::min(bar->maximum(), bar->value() + 60);
    if (next == bar->value())
      break;
    bar->setValue(next);
    immediateViewportCoverage =
        immediateViewportCoverage && windowCovered(view.viewport()->rect());
  }
  result &= expect(
      immediateViewportCoverage &&
          view.property("conversationCardConstructions").toULongLong() ==
              warmConstructions,
      "warmed scrolling materializes every visible row without synchronously "
      "constructing hidden overscan renderers");
  result &= expect(
      waitUntil(
          [&] {
            const int height = view.viewport()->height();
            return windowCovered(
                view.viewport()->rect().adjusted(0, -height, 0, height));
          },
          2000),
      "hidden overscan admission eventually completes around the settled "
      "viewport");

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
  const QPointF local = view.viewport()->rect().center();
  const QPointingDevice *const device =
      QPointingDevice::primaryPointingDevice();
  QWheelEvent wheel(local, view.viewport()->mapToGlobal(local.toPoint()), {},
                    QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false,
                    Qt::MouseEventSynthesizedByApplication, device);
  constexpr ulong WheelTimestamp = 424242;
  wheel.setTimestamp(WheelTimestamp);
  WheelDeliveryProbe delivery(view.viewport());
  wheel.ignore();
  static_cast<void>(QApplication::sendEvent(view.viewport(), &wheel));
  result &= expect(wheel.isAccepted(),
                   "conversation accepts one native wheel gesture");
  result &=
      expect(delivery.seen() && delivery.timestamp() == WheelTimestamp &&
                 delivery.source() == Qt::MouseEventSynthesizedByApplication &&
                 delivery.device() == device,
             "native outer wheel delivery preserves timestamp, source, and "
             "pointing device identity");
  return result;
}

bool largeIncomingCommandUsesBoundedFinalWidthLayout() {
  const std::string thread = "bounded-command";
  VisibleCardData root{AuthoritativeItemKey{thread, "turn", "root"},
                       CardKind::UserMessage,
                       thread,
                       "turn",
                       "root",
                       UserMessageData{"Run the command"}};
  ConversationSnapshot snapshot;
  snapshot.threadId = thread;
  snapshot.sections.push_back({"command-section", "turn", {root}, root.key});

  ConversationView view;
  ConversationView::PresentationOptions options = view.presentationOptions();
  options.commandsInitiallyExpanded = true;
  view.setPresentationOptions(options);
  view.resize(760, 480);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "bounded-command fixture reconciles");
  settle();

  std::string output;
  output.reserve(192 * 1024);
  while (output.size() < 192 * 1024)
    output += "0123456789abcdef command output line for bounded layout\n";
  output.resize(192 * 1024);
  ConversationRowPlacement tail;
  tail.card = {
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{
          "printf diagnostic", std::move(output), "/workspace", {}, {}},
      nodegraph::NodeStatus::Running};
  const std::string key = stableKey(tail.card.key);
  tail.sectionKey = "command-section";
  tail.nested = true;
  tail.activeTurn = true;

  QElapsedTimer timer;
  timer.start();
  result &= expect(appendProjected(view, std::move(tail)),
                   "large command appends through the direct-tail path");
  const qint64 appendMicros = timer.nsecsElapsed() / 1000;
  settle();
  ConversationCard *commandCard = materializedCard(view, key);
  CommandOutputView *commandOutput =
      commandCard ? commandCard->findChild<CommandOutputView *>(
                        QStringLiteral("commandOutputView"))
                  : nullptr;
  result &= expect(
      commandOutput && commandOutput->viewport()->width() > 500 &&
          commandOutput->property("boundedOutputMeasurements").toULongLong() >=
              1 &&
          commandOutput->property("fullOutputMeasurements").toULongLong() ==
              0 &&
          commandOutput->document()->characterCount() > 190 * 1024,
      "large command output is retained but bypasses whole-document geometry "
      "at its final row width");
  view.setProperty("largeCommandAppendMicros", appendMicros);
  return result;
}

bool streamingMarkdownKeepsOneDocument() {
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
  snapshot.sections.push_back(
      {"markdown-section", update.turnId, {update}, std::nullopt});

  ConversationView view;
  view.resize(760, 480);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "large Markdown tail fixture reconciles");
  settle();

  const std::string key = stableKey(update.key);
  ConversationCard *const card = materializedCard(view, key);
  MarkdownTextView *const body =
      card ? card->findChild<MarkdownTextView *>() : nullptr;
  QTextDocument *const document = body ? body->document() : nullptr;
  const QString stableFirstBlock =
      document ? document->begin().text() : QString{};
  const qulonglong constructions =
      view.property("conversationCardConstructions").toULongLong();
#if QT_CONFIG(accessibility)
  QAccessibleInterface *const cardAccessible =
      card ? QAccessible::queryAccessibleInterface(card) : nullptr;
  QAccessibleInterface *const markdownAccessible =
      body ? QAccessible::queryAccessibleInterface(body) : nullptr;
  QAccessibleTextInterface *const accessibleText =
      markdownAccessible ? markdownAccessible->textInterface() : nullptr;
  const QAccessible::Id cardAccessibleId =
      cardAccessible ? QAccessible::uniqueId(cardAccessible) : 0;
  const QAccessible::Id markdownAccessibleId =
      markdownAccessible ? QAccessible::uniqueId(markdownAccessible) : 0;
  tests::AccessibilityEventProbe accessibilityEvents;
#endif
  if (body) {
    QTextCursor selection(body->document());
    selection.setPosition(std::min(12, body->document()->characterCount() - 1),
                          QTextCursor::KeepAnchor);
    body->setTextCursor(selection);
    body->setFocus(Qt::TabFocusReason);
  }
#if QT_CONFIG(accessibility)
  settle();
  int accessibleSelectionStart = -1;
  int accessibleSelectionEnd = -1;
  if (accessibleText && accessibleText->selectionCount() == 1)
    accessibleText->selection(0, &accessibleSelectionStart,
                              &accessibleSelectionEnd);
  const auto selectionEvents = accessibilityEvents.events(
      markdownAccessibleId, QAccessible::TextSelectionChanged);
  const auto caretEvents = accessibilityEvents.events(
      markdownAccessibleId, QAccessible::TextCaretMoved);
  int selectionEventOrder = -1;
  int caretEventOrder = -1;
  for (qsizetype event = 0; event < accessibilityEvents.all().size(); ++event) {
    const auto &record = accessibilityEvents.all().at(event);
    if (record.target == markdownAccessibleId &&
        record.type == QAccessible::TextSelectionChanged)
      selectionEventOrder = static_cast<int>(event);
    if (record.target == markdownAccessibleId &&
        record.type == QAccessible::TextCaretMoved)
      caretEventOrder = static_cast<int>(event);
  }
  result &= expect(
      cardAccessibleId != 0 && markdownAccessibleId != 0 && accessibleText &&
          accessibleSelectionStart == 0 && accessibleSelectionEnd == 12 &&
          selectionEvents.size() == 1 && caretEvents.size() == 1 &&
          selectionEventOrder >= 0 && caretEventOrder > selectionEventOrder &&
          accessibilityEvents
              .events(cardAccessibleId, QAccessible::TextSelectionChanged)
              .isEmpty() &&
          accessibilityEvents
              .events(cardAccessibleId, QAccessible::TextCaretMoved)
              .isEmpty(),
      "Markdown selection uses one ordered native stream on its text interface");
  accessibilityEvents.clear();
  if (body)
    body->setTextCursor(body->textCursor());
  settle();
  result &= expect(
      accessibilityEvents.events(markdownAccessibleId).isEmpty() &&
          accessibilityEvents.events(cardAccessibleId).isEmpty(),
      "reapplying the identical Markdown selection emits no event");
  accessibilityEvents.clear();
#endif

  auto &message = std::get<AgentMessageData>(update.payload);
  message.text += "** with a [link](https://example.com).\n\n"
                  "The final paragraph is complete.";
  QElapsedTimer timer;
  timer.start();
  const auto impact = applyPresentation(view, update);
  const qint64 updateMicros = timer.nsecsElapsed() / 1000;
  settle();

  result &= expect(
      impact == PresentationImpact::GeometryChanged && card && body &&
          materializedCard(view, key) == card && body->document() == document &&
          document->begin().text() == stableFirstBlock &&
          body->markdownSource() == QString::fromStdString(message.text) &&
          body->toHtml().contains(QStringLiteral("https://example.com")) &&
          body->textCursor().hasSelection() &&
          view.property("conversationCardConstructions").toULongLong() ==
              constructions,
      "streaming preserves the card, document, stable prefix, link, and "
      "selection");
#if QT_CONFIG(accessibility)
  accessibleSelectionStart = -1;
  accessibleSelectionEnd = -1;
  if (accessibleText && accessibleText->selectionCount() == 1)
    accessibleText->selection(0, &accessibleSelectionStart,
                              &accessibleSelectionEnd);
  const bool markdownMutationObserved =
      std::ranges::any_of(accessibilityEvents.all(),
                          [markdownAccessibleId](const auto &event) {
                            return event.target == markdownAccessibleId &&
                                   (event.type == QAccessible::TextInserted ||
                                    event.type == QAccessible::TextRemoved ||
                                    event.type == QAccessible::TextUpdated);
                          });
  const bool cardMutationObserved =
      std::ranges::any_of(accessibilityEvents.all(),
                          [cardAccessibleId](const auto &event) {
                            return event.target == cardAccessibleId &&
                                   (event.type == QAccessible::TextInserted ||
                                    event.type == QAccessible::TextRemoved ||
                                    event.type == QAccessible::TextUpdated);
                          });
  result &= expect(
      QAccessible::queryAccessibleInterface(body) == markdownAccessible &&
          QAccessible::uniqueId(markdownAccessible) == markdownAccessibleId &&
          accessibleText && accessibleSelectionStart == 0 &&
          accessibleSelectionEnd == 12 &&
          accessibleText->text(0, accessibleText->characterCount())
              .contains(QStringLiteral("The final paragraph is complete.")) &&
          !cardMutationObserved &&
          (!QAccessible::isActive() || markdownMutationObserved) &&
          accessibilityEvents.events(cardAccessibleId).isEmpty(),
      "streaming retains one native Markdown text interface and emits no "
      "duplicate event on the containing ListItem");
  accessibilityEvents.clear();
#endif
  result &= expect(applyPresentation(view, update) == PresentationImpact::None,
                   "identical streamed Markdown is a semantic no-op");
  settle();
#if QT_CONFIG(accessibility)
  result &= expect(
      accessibilityEvents.events(markdownAccessibleId).isEmpty() &&
          accessibilityEvents.events(cardAccessibleId).isEmpty(),
      "identical streamed Markdown emits no accessibility event");
#endif
  result &= expect(updateMicros < 500000,
                   "incremental visible Markdown update stays below 500 ms");
  return result;
}
bool interactiveResizeCoalescesConversationReflow() {
  ConversationSnapshot snapshot;
  snapshot.threadId = "interactive-resize";
  TurnSection section;
  section.key = "resize-section";
  section.turnId = "resize-turn";
  for (int row = 0; row < 320; ++row) {
    std::string text = "Resize row " + std::to_string(row) + " ";
    text.append(220 + row % 80, static_cast<char>('a' + row % 26));
    section.cards.push_back(
        {AuthoritativeItemKey{"interactive-resize", "resize-turn",
                              "resize-" + std::to_string(row)},
         CardKind::AgentMessage, "interactive-resize", "resize-turn",
         "resize-" + std::to_string(row),
         AgentMessageData{std::move(text), false}});
  }
  snapshot.sections.push_back(std::move(section));

  ConversationView view;
  view.resize(620, 360);
  view.show();
  bool result = expect(changed(view.reconcile(std::move(snapshot))),
                       "interactive-resize fixture reconciles");
  settle();
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
  settle();
  const auto retained = firstVisible(view);
  view.setCurrentIndex(view.indexAt(view.viewport()->rect().center()));
  settle();
  const QModelIndex promoted = view.currentIndex();
  const std::string promotedKey =
      promoted.data(ConversationItemModel::StableKeyRole)
          .toString()
          .toStdString();
  ConversationCard *card = materializedCard(view, promotedKey);
  result &= expect(!retained.first.empty() && promoted.isValid() && card,
                   "interactive-resize fixture has an anchored rich card");

  const qulonglong rebuildsBefore =
      view.property("conversationHeightIndexRebuilds").toULongLong();
  const qulonglong framesBefore =
      view.property("conversationInteractiveResizeFrameReflows").toULongLong();
  view.beginInteractiveResize();
  for (int step = 0; step < 80; ++step)
    view.resize(621 + step * 2, 360);
  card = materializedCard(view, promotedKey);
  result &= expect(
      view.property("conversationInteractiveResizeActive").toBool() &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              rebuildsBefore &&
          view.property("conversationInteractiveResizeEvents").toULongLong() >=
              80 &&
          card && card->width() == view.visualRect(promoted).width(),
      "splitter motion updates live width without one full reflow per event");

  result &= expect(
      waitUntil(
          [&] {
            return view.property("conversationInteractiveResizeFrameReflows")
                       .toULongLong() > framesBefore;
          },
          250) &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              rebuildsBefore &&
          view.property("conversationInteractiveResizeFrameReflows")
                  .toULongLong() == framesBefore + 1,
      "one display-frame pass coalesces a burst of splitter resize events");

  view.endInteractiveResize();
  settle();
  const auto retainedAfter = firstVisible(view);
  result &= expect(
      !view.property("conversationInteractiveResizeActive").toBool() &&
          view.property("conversationHeightIndexRebuilds").toULongLong() ==
              rebuildsBefore &&
          view.property("conversationInteractiveResizeSettlements")
                  .toULongLong() == 1 &&
          retainedAfter == retained,
      "splitter release reflows only resident cards and preserves the paused "
      "viewport anchor without rebuilding offscreen scalar estimates");

  const QModelIndex distant = view.conversationModel()->index(300);
  view.scrollTo(distant, QAbstractItemView::PositionAtCenter);
  settle();
  const std::string distantKey =
      distant.data(ConversationItemModel::StableKeyRole)
          .toString()
          .toStdString();
  ConversationCard *distantCard = materializedCard(view, distantKey);
  result &= expect(
      distantCard && distantCard->width() == view.visualRect(distant).width() &&
          distantCard->height() == view.visualRect(distant).height(),
      "an offscreen scalar settles through the sole renderer at "
      "the resized width when it enters residency");
  return result;
}

bool runtimeGeometryEnvironmentKeepsOneInteractiveRenderer() {
  const QString originalStyleSheet = qApp->styleSheet();
  ConversationSnapshot snapshot = conversation(600);
  std::get<AgentMessageData>(snapshot.sections.front().cards.front().payload)
      .text.append(2'000, 'x');

  ConversationView view;
  view.resize(700, 360);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "runtime-geometry fixture reconciles");
  settle();
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  settle();
  const QModelIndex first = view.conversationModel()->index(0);
  const std::string firstKey =
      first.data(ConversationItemModel::StableKeyRole).toString().toStdString();
  const int exactFirstHeight = view.visualRect(first).height();

  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  result &= expect(waitForResidency(view),
                   "runtime geometry starts from settled overscan residency");
  const auto styleAnchor = firstVisible(view);
  const QModelIndex selected = view.indexAt(view.viewport()->rect().center());
  const std::string selectedKey =
      selected.data(ConversationItemModel::StableKeyRole)
          .toString()
          .toStdString();
  ConversationCard *card = materializedCard(view, selectedKey);
  MarkdownTextView *body =
      card ? card->findChild<MarkdownTextView *>() : nullptr;
  QTextDocument *document = body ? body->document() : nullptr;
  if (body) {
    QTextCursor cursor(body->document());
    cursor.setPosition(1);
    cursor.setPosition(std::min(7, body->document()->characterCount() - 1),
                       QTextCursor::KeepAnchor);
    body->setTextCursor(cursor);
    body->setFocus(Qt::TabFocusReason);
  }
  const QString selectedText =
      body ? body->textCursor().selectedText() : QString{};
  const qulonglong reflows =
      view.property("conversationGeometryEnvironmentReflows").toULongLong();

  qApp->setStyleSheet(originalStyleSheet +
                      QStringLiteral("\nQWidget { font-size: 18pt; }"));
  settle();
  result &= expect(waitForResidency(view),
                   "style reflow settles the same bounded residency window");
  card = materializedCard(view, selectedKey);
  body = card ? card->findChild<MarkdownTextView *>() : nullptr;
  result &= expect(
      !styleAnchor.first.empty() && selected.isValid() && card && body &&
          body->document() == document && body->hasFocus() &&
          body->textCursor().selectedText() == selectedText &&
          firstVisible(view) == styleAnchor &&
          materializedCard(view, firstKey) == nullptr &&
          view.visualRect(first).height() != exactFirstHeight &&
          view.property("conversationGeometryEnvironmentReflows")
                  .toULongLong() == reflows + 1 &&
          view.materializedCardCount() <= 48,
      "one application-style pass retires offscreen scalar geometry while "
      "preserving the resident renderer, document, selection, focus, and "
      "paused anchor");

  const auto dprAnchor = firstVisible(view);
  const qulonglong dprReflows =
      view.property("conversationGeometryEnvironmentReflows").toULongLong();
  const qulonglong dprConstructions =
      view.property("conversationCardConstructions").toULongLong();
  QEvent dprChange(QEvent::DevicePixelRatioChange);
  QApplication::sendEvent(&view, &dprChange);
  settle();
  result &= expect(
      materializedCard(view, selectedKey) == card && body && body->hasFocus() &&
          body->document() == document &&
          body->textCursor().selectedText() == selectedText &&
          firstVisible(view) == dprAnchor &&
          view.property("conversationGeometryEnvironmentReflows")
                  .toULongLong() == dprReflows + 1 &&
          view.property("conversationCardConstructions").toULongLong() ==
              dprConstructions,
      "a DPR notification keeps identity and interaction state and performs "
      "one authoritative geometry reconciliation");

  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  settle();
  ConversationCard *returned = materializedCard(view, firstKey);
  result &= expect(returned &&
                       view.visualRect(first).height() == returned->height() &&
                       returned->height() != exactFirstHeight,
                   "a retired offscreen scalar is replaced by the sole "
                   "renderer's exact height when it returns to residency");

  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  settle();
  const qulonglong followReflows =
      view.property("conversationGeometryEnvironmentReflows").toULongLong();
  qApp->setStyleSheet(originalStyleSheet +
                      QStringLiteral("\nQWidget { font-size: 19pt; }"));
  settle();
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom() &&
                       view.property("conversationGeometryEnvironmentReflows")
                               .toULongLong() == followReflows + 1,
                   "runtime reflow retains an explicitly followed bottom in "
                   "one coalesced pass");
  qApp->setStyleSheet(originalStyleSheet);
  settle();

  view.verticalScrollBar()->setValue(view.verticalScrollBar()->minimum());
  settle();
  ConversationCard *retainedFirst = materializedCard(view, firstKey);
  const int retainedExactHeight = retainedFirst ? retainedFirst->height() : 0;
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
  settle();
  result &= expect(retainedFirst &&
                       changed(view.reconcile(singleMessageConversation(
                           "geometry-other", "Other conversation"))) &&
                       changed(view.reconcile(snapshot)),
                   "a plain thread round-trip restores retained geometry");
  settle();
  QModelIndex returnedFirst = view.conversationModel()->index(0);
  result &=
      expect(materializedCard(view, firstKey) == nullptr &&
                 view.visualRect(returnedFirst).height() == retainedExactHeight,
             "thread switching alone preserves an inactive thread's "
             "exact offscreen scalar");

  ConversationSnapshot emptyThread;
  emptyThread.threadId = "geometry-empty";
  result &= expect(changed(view.reconcile(std::move(emptyThread))),
                   "the runtime fixture activates an empty thread");
  qApp->setStyleSheet(originalStyleSheet +
                      QStringLiteral("\nQWidget { font-size: 20pt; }"));
  settle();
  result &= expect(changed(view.reconcile(snapshot)),
                   "the runtime fixture returns after an empty-thread style "
                   "change");
  settle();
  returnedFirst = view.conversationModel()->index(0);
  result &=
      expect(materializedCard(view, firstKey) == nullptr &&
                 view.visualRect(returnedFirst).height() != retainedExactHeight,
             "an empty-thread environment change retires scalar geometry "
             "belonging to inactive threads without materializing cards");
  qApp->setStyleSheet(originalStyleSheet);
  settle();

  QWidget *stagingHost = view.findChild<QWidget *>(
      QStringLiteral("conversationStagingHost"), Qt::FindDirectChildrenOnly);
  result &= expect(stagingHost && stagingHost->parentWidget() == &view &&
                       stagingHost->isHidden(),
                   "the hidden staging renderer inherits the view geometry "
                   "environment without entering the viewport");
  return result;
}

bool accessibilityUsesLogicalRowsAndRealControls() {
#if !QT_CONFIG(accessibility)
  return true;
#else
  ConversationSnapshot snapshot = conversation(200);
  ConversationView view;
  view.resize(700, 360);
  view.show();
  bool result = expect(changed(view.reconcile(snapshot)),
                       "accessibility fixture reconciles");
  result &= expect(waitForResidency(view),
                   "the accessible resident tree reaches a stable window");

  QAccessibleInterface *pane = QAccessible::queryAccessibleInterface(&view);
  QAccessibleInterface *list = pane ? pane->child(0) : nullptr;
  QAccessibleSelectionInterface *selection =
      list ? list->selectionInterface() : nullptr;
  tests::AccessibilityEventProbe events;
  const auto renderer = [](QAccessibleInterface *item) {
    auto *body = item ? item->child(0) : nullptr;
    return body ? qobject_cast<ConversationCard *>(body->object()) : nullptr;
  };
  QList<QWidget *> expectedPaneChildren{view.viewport()};
  for (QScrollBar *bar : {view.horizontalScrollBar(), view.verticalScrollBar()})
    if (bar->isVisible())
      expectedPaneChildren.push_back(bar->parentWidget());
  if (QWidget *corner = view.cornerWidget(); corner && corner->isVisible())
    expectedPaneChildren.push_back(corner);
  bool exactPaneTree =
      pane && pane->childCount() == expectedPaneChildren.size();
  for (int child = 0; exactPaneTree && child < expectedPaneChildren.size();
       ++child) {
    QAccessibleInterface *item = pane->child(child);
    exactPaneTree = item && item->object() == expectedPaneChildren.at(child) &&
                    item->parent() == pane && pane->indexOfChild(item) == child;
  }
  const bool expectsScrollBar = view.horizontalScrollBar()->isVisible() ||
                                view.verticalScrollBar()->isVisible();
  result &= expect(
      pane && pane->role() == QAccessible::Pane && exactPaneTree &&
          !pane->tableInterface() && list && pane->indexOfChild(list) == 0 &&
          list->role() == QAccessible::List && list->parent() == pane &&
          !list->tableInterface() && selection &&
          list->childCount() == view.conversationModel()->rowCount(),
      "the Pane preserves its native physical children and exposes one "
      "logical List independently of widget residency");

  const auto residentCards = view.viewport()->findChildren<ConversationCard *>(
      QString{}, Qt::FindDirectChildrenOnly);
  const auto hiddenResident =
      std::ranges::find_if(residentCards, &QWidget::isHidden);
  ConversationCard *const hiddenCard =
      hiddenResident == residentCards.end() ? nullptr : *hiddenResident;
  QAccessibleInterface *const hiddenItem =
      hiddenCard ? QAccessible::queryAccessibleInterface(hiddenCard) : nullptr;
  const QAccessible::State hiddenState =
      hiddenItem ? hiddenItem->state() : QAccessible::State{};
  result &= expect(
      hiddenItem && list && list->indexOfChild(hiddenItem) == -1 &&
          hiddenState.invisible && !hiddenState.focusable &&
          !hiddenState.selectable && !selection->select(hiddenItem),
      "hidden overscan is neither an accessible child nor selectable through "
      "a directly queried interface");

  bool foundVisible = false;
  bool foundOffscreen = false;
  bool foundCopy = false;
  bool foundScrollBar = false;
  bool foundVirtualCell = false;
  bool symmetricTree = true;
  QWidget *stagingHost = view.findChild<QWidget *>(
      QStringLiteral("conversationStagingHost"), Qt::FindDirectChildrenOnly);
  bool foundStagingObject = false;
  const auto inspect = [&](auto &&self, QAccessibleInterface *interface,
                           int depth) -> void {
    if (!interface || depth > 5)
      return;
    foundVirtualCell = foundVirtualCell || interface->tableInterface() ||
                       interface->tableCellInterface() ||
                       interface->role() == QAccessible::Cell ||
                       interface->role() == QAccessible::Table;
    foundScrollBar =
        foundScrollBar || interface->role() == QAccessible::ScrollBar;
    QWidget *accessibleWidget = qobject_cast<QWidget *>(interface->object());
    foundStagingObject =
        foundStagingObject || (accessibleWidget && stagingHost &&
                               (accessibleWidget == stagingHost ||
                                stagingHost->isAncestorOf(accessibleWidget)));
    foundCopy = foundCopy || (interface->role() == QAccessible::Button &&
                              interface->text(QAccessible::Name) ==
                                  QStringLiteral("Copy card content"));
    for (int child = 0; child < interface->childCount(); ++child) {
      QAccessibleInterface *descendant = interface->child(child);
      symmetricTree =
          symmetricTree && descendant && descendant->parent() == interface &&
          interface->indexOfChild(descendant) == child &&
          interface->child(interface->indexOfChild(descendant)) == descendant;
      self(self, descendant, depth + 1);
    }
  };
  QAccessibleInterface *visibleItem = nullptr;
  QAccessibleInterface *otherItem = nullptr;
  for (int child = 0; list && child < list->childCount(); ++child) {
    QAccessibleInterface *item = list->child(child);
    const QModelIndex index = view.conversationModel()->index(child);
    const QAccessible::State state =
        item ? item->state() : QAccessible::State{};
    const bool offscreen =
        !view.visualRect(index).intersects(view.viewport()->rect());
    result &= expect(
        item && !item->object() && item->role() == QAccessible::ListItem &&
            item->parent() == list && list->indexOfChild(item) == child &&
            index.isValid() && !item->text(QAccessible::Name).isEmpty() &&
            state.selectable && state.offscreen == offscreen &&
            state.invisible == offscreen &&
            item->text(QAccessible::Description)
                .contains(
                    QString::fromStdString("Answer " + std::to_string(child))),
        "each logical item has model order, text and scalar geometry");
    if (!state.offscreen && !visibleItem)
      visibleItem = item;
    else if (!state.offscreen && !otherItem)
      otherItem = item;
    foundVisible = foundVisible || !state.offscreen;
    foundOffscreen = foundOffscreen || state.offscreen;
  }
  inspect(inspect, pane, 0);
  result &=
      expect(foundVisible && foundOffscreen && foundCopy &&
                 foundScrollBar == expectsScrollBar && !foundVirtualCell &&
                 !foundStagingObject && symmetricTree &&
                 list->childCount() == view.conversationModel()->rowCount(),
             "logical rows expose resident controls exactly once, with no "
             "staging objects or duplicate table cells");

  view.setEnabled(false);
  result &= expect(
      visibleItem && visibleItem->state().disabled &&
          !visibleItem->state().focusable &&
          !visibleItem->state().selectable && selection && otherItem &&
          !selection->select(otherItem) && !selection->clear(),
      "a disabled Conversation rejects accessible selection mutations");
  view.setEnabled(true);

  QAccessibleInterface *retainedItem = list ? list->child(0) : nullptr;
  QObject *retainedObject = retainedItem ? retainedItem->object() : nullptr;
  const int retainedCount = list ? list->childCount() : 0;
  result &= expect(view.reconcile(snapshot) ==
                       ConversationView::ReconciliationResult::Unchanged,
                   "an identical accessibility snapshot is a semantic no-op");
  settle();
  result &= expect(
      list && list->childCount() == retainedCount &&
          list->child(0) == retainedItem &&
          list->child(0)->object() == retainedObject,
      "a semantic no-op retains the accessible card objects and tree shape");

  if (!otherItem && list && list->childCount() > 1)
    otherItem = list->child(1);
  auto *currentCard = renderer(visibleItem);
  const QModelIndex current = currentCard
                                  ? view.conversationModel()->indexForStableKey(
                                        stableKey(currentCard->data().key))
                                  : QModelIndex{};
  auto *otherCard = renderer(otherItem);
  const QModelIndex other =
      otherCard ? view.conversationModel()->indexForStableKey(
                      stableKey(otherCard->data().key))
                : QModelIndex{};
  const QAccessible::Id currentId =
      visibleItem ? QAccessible::uniqueId(visibleItem) : 0;
  const QAccessible::Id otherId =
      otherItem ? QAccessible::uniqueId(otherItem) : 0;
  const QAccessible::Id paneId = pane ? QAccessible::uniqueId(pane) : 0;
  view.setCurrentIndex(current);
  view.setFocus(Qt::OtherFocusReason);
  view.selectionModel()->select(
      current, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  settle();
  events.clear();
  if (selection && otherItem)
    static_cast<void>(selection->select(otherItem));
  settle();
  const auto selectedOther = events.events(otherId, QAccessible::SelectionAdd);
  const auto deselectedCurrent =
      events.events(currentId, QAccessible::SelectionRemove);
  int addOrder = -1;
  int removeOrder = -1;
  for (qsizetype event = 0; event < events.all().size(); ++event) {
    const auto &record = events.all().at(event);
    if (record.target == otherId && record.type == QAccessible::SelectionAdd)
      addOrder = static_cast<int>(event);
    if (record.target == currentId &&
        record.type == QAccessible::SelectionRemove)
      removeOrder = static_cast<int>(event);
  }
  result &=
      expect(selectedOther.size() == 1 && deselectedCurrent.size() == 1 &&
                 addOrder >= 0 && removeOrder > addOrder &&
                 selectedOther.front().state.selected &&
                 !deselectedCurrent.front().state.selected,
             "accessible selection emits one event on each exact logical item");
  events.clear();
  view.setCurrentIndex(other);
  settle();
  result &= expect(events.events(otherId, QAccessible::Focus).size() == 1 &&
                       otherItem && otherItem->state().focused,
                   "logical row focus emits once on the exact logical item");
  events.clear();
  view.setCurrentIndex(other);
  if (selection && otherItem)
    static_cast<void>(selection->select(otherItem));
  settle();
  result &= expect(
      events.events(otherId).isEmpty() && events.events(currentId).isEmpty(),
      "identical row focus and selection are accessibility no-ops");
  auto *copy = currentCard ? currentCard->findChild<QToolButton *>(
                                 QStringLiteral("cardCopyButton"))
                           : nullptr;
  const QModelIndexList selectionBeforeControlFocus =
      view.selectionModel()->selectedIndexes();
  if (copy)
    copy->setFocus(Qt::TabFocusReason);
  settle();
  result &= expect(
      copy && list->focusChild() && list->focusChild()->object() == copy &&
          pane->focusChild() && pane->focusChild()->object() == copy &&
          view.selectionModel()->selectedIndexes() ==
              selectionBeforeControlFocus &&
          visibleItem && !visibleItem->state().focused,
      "container focus resolves to the real focused card control without "
      "changing semantic row selection or row focus");
  view.setCurrentIndex(current);
  events.clear();
  view.setFocus(Qt::OtherFocusReason);
  settle();
  result &= expect(
      events.events(paneId, QAccessible::Focus).size() == 1 &&
          events.events(currentId, QAccessible::Focus).isEmpty() && pane &&
          pane->focusChild() == visibleItem && visibleItem->state().focused,
      "native view focus resolves to its unchanged current ListItem");
  events.clear();
  view.setFocus(Qt::OtherFocusReason);
  settle();
  result &= expect(events.events(paneId).isEmpty() &&
                       events.events(currentId).isEmpty(),
                   "refocusing an already focused view is a semantic no-op");
  result &= expect(
      selection && current.isValid() && otherItem &&
          list->focusChild() == visibleItem && visibleItem->state().focused &&
          selection->select(otherItem) && view.currentIndex() == current &&
          selection->selectedItemCount() == 1 &&
          selection->isSelected(selection->selectedItem(0)) &&
          selection->clear() && selection->selectedItemCount() == 0 &&
          view.currentIndex() == current,
      "accessible selection changes selection only and current "
      "row focus resolves to its resident ListItem");

  QModelIndex nonresident;
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const auto *candidate = view.conversationModel()->row(row);
    if (candidate && !materializedCard(view, candidate->stableKey)) {
      nonresident = view.conversationModel()->index(row);
      break;
    }
  }
  const qulonglong constructionsBeforeNonresidentSelection =
      view.property("conversationCardConstructions").toULongLong();
  const int residentsBeforeNonresidentSelection = view.materializedCardCount();
  const int scrollBeforeNonresidentSelection =
      view.verticalScrollBar()->value();
  QList<QAccessible::Id> accessibleObjectsBeforeNonresidentSelection;
  for (int child = 0; list && child < list->childCount(); ++child)
    accessibleObjectsBeforeNonresidentSelection.push_back(
        QAccessible::uniqueId(list->child(child)));
  events.clear();
  if (nonresident.isValid())
    view.selectionModel()->select(
        nonresident, QItemSelectionModel::ClearAndSelect |
                         QItemSelectionModel::Rows);
  settle();
  auto *nonresidentItem = list->child(nonresident.row());
  const auto nonresidentId = QAccessible::uniqueId(nonresidentItem);
  result &= expect(
      nonresident.isValid() && view.selectionModel()->isSelected(nonresident) &&
          selection && selection->selectedItemCount() == 1 &&
          selection->selectedItem(0) == nonresidentItem &&
          events.events(nonresidentId, QAccessible::SelectionAdd).size() == 1 &&
          events.events(nonresidentId, QAccessible::Focus).isEmpty() &&
          view.property("conversationCardConstructions").toULongLong() ==
              constructionsBeforeNonresidentSelection &&
          view.materializedCardCount() == residentsBeforeNonresidentSelection &&
          view.verticalScrollBar()->value() ==
              scrollBeforeNonresidentSelection &&
          view.currentIndex() == current,
      "offscreen selection exposes its logical item and event without "
      "constructing a widget, scrolling or moving focus");
  events.clear();
  view.selectionModel()->clearSelection();
  settle();
  QList<QAccessible::Id> accessibleObjectsAfterNonresidentSelection;
  for (int child = 0; list && child < list->childCount(); ++child)
    accessibleObjectsAfterNonresidentSelection.push_back(
        QAccessible::uniqueId(list->child(child)));
  result &= expect(
      events.events(nonresidentId, QAccessible::SelectionRemove).size() == 1 &&
          accessibleObjectsAfterNonresidentSelection ==
              accessibleObjectsBeforeNonresidentSelection &&
          view.property("conversationCardConstructions").toULongLong() ==
              constructionsBeforeNonresidentSelection &&
          view.materializedCardCount() == residentsBeforeNonresidentSelection &&
          view.verticalScrollBar()->value() == scrollBeforeNonresidentSelection,
      "clearing offscreen selection emits the exact removal without changing "
      "residency");

  if (copy)
    copy->setFocus(Qt::TabFocusReason);
  QScrollBar *const scrollBar = view.verticalScrollBar();
  const int pinnedScroll = scrollBar->value();
  scrollBar->setValue(pinnedScroll < scrollBar->maximum() / 2
                          ? scrollBar->maximum()
                          : scrollBar->minimum());
  const auto accessibleObjects = [list] {
    QList<QAccessible::Id> objects;
    for (int child = 0; list && child < list->childCount(); ++child)
      objects.push_back(QAccessible::uniqueId(list->child(child)));
    return objects;
  };
  const QList<QAccessible::Id> beforeAdmission = accessibleObjects();
  const qulonglong constructionsBeforeAdmission =
      view.property("conversationCardConstructions").toULongLong();
  result &=
      expect(currentCard && !currentCard->isHidden() &&
                 !currentCard->geometry().intersects(view.viewport()->rect()) &&
                 list && list->indexOfChild(visibleItem) >= 0,
             "a focused offscreen card remains a real accessible renderer");
  result &= expect(waitForResidency(view),
                   "hidden overscan admission reaches its fixed point");
  result &= expect(
      view.property("conversationCardConstructions").toULongLong() >
              constructionsBeforeAdmission &&
          accessibleObjects() == beforeAdmission,
      "hidden overscan admission does not replace or extend the accessible "
      "tree");
  scrollBar->setValue(pinnedScroll);
  result &= expect(waitForResidency(view),
                   "accessibility fixture restores stable residency");

  ConversationCard staged(message(0), false, stagingHost,
                          std::max(1, view.viewport()->width()));
  staged.hide();
  QAccessibleInterface *stagedItem =
      QAccessible::queryAccessibleInterface(&staged);
  const QAccessible::State stagedState =
      stagedItem ? stagedItem->state() : QAccessible::State{};
  result &= expect(stagedItem && stagedItem->role() == QAccessible::Client &&
                       stagedItem->parent() != list &&
                       list->indexOfChild(stagedItem) == -1 &&
                       stagedState.invisible && !stagedState.selectable &&
                       !selection->select(stagedItem) &&
                       view.currentIndex() == current,
                   "a staged card with a resident key remains outside the "
                   "List and cannot become an accessibility selection");

  ConversationSnapshot history = snapshot;
  history.hasMore = true;
  result &= expect(changed(view.reconcile(std::move(history))),
                   "the accessibility fixture exposes available history");
  settle();
  result &= expect(
      list && list->childCount() == view.conversationModel()->rowCount() + 1 &&
          list->child(0)->role() == QAccessible::Button &&
          list->child(0)->parent() == list,
      "the visible native history control precedes resident "
      "cards in the List");
  auto *historyButton = view.findChild<QPushButton *>(
      QStringLiteral("conversationLoadMore"));
  QAccessibleInterface *historyAccessible =
      historyButton ? QAccessible::queryAccessibleInterface(historyButton)
                    : nullptr;
  const QAccessible::Id historyId =
      historyAccessible ? QAccessible::uniqueId(historyAccessible) : 0;
  const QAccessible::Id listId = list ? QAccessible::uniqueId(list) : 0;
  events.clear();
  view.setHistoryRequestPending("virtual-thread", true);
  settle();
  const auto historyBusy =
      events.events(historyId, QAccessible::StateChanged);
  const auto listBusy = events.events(listId, QAccessible::StateChanged);
  result &= expect(
      historyBusy.size() == 1 &&
          historyBusy.front().changedStates.disabled &&
          historyBusy.front().state.disabled &&
          !historyBusy.front().changedStates.busy &&
          listBusy.size() == 1 && listBusy.front().changedStates.busy &&
          listBusy.front().state.busy && historyAccessible &&
          historyAccessible->state().disabled &&
          events.events(historyId, QAccessible::NameChanged).size() == 1 &&
          events.events(historyId, QAccessible::DescriptionChanged).size() == 1,
      "history pending publishes native button changes and one custom List "
      "busy change");
  events.clear();
  view.setHistoryRequestPending("virtual-thread", true);
  settle();
  result &= expect(events.events(historyId).isEmpty() &&
                       events.events(listId).isEmpty(),
                   "identical history pending state emits no accessibility event");
  events.clear();
  view.setHistoryRequestPending("virtual-thread", false);
  settle();
  const auto historyAvailable =
      events.events(historyId, QAccessible::StateChanged);
  const auto listAvailable = events.events(listId, QAccessible::StateChanged);
  result &= expect(
      historyAvailable.size() == 1 &&
          historyAvailable.front().changedStates.disabled &&
          !historyAvailable.front().state.disabled &&
          !historyAvailable.front().changedStates.busy &&
          listAvailable.size() == 1 &&
          listAvailable.front().changedStates.busy &&
          !listAvailable.front().state.busy && historyAccessible &&
          !historyAccessible->state().disabled &&
          events.events(historyId, QAccessible::NameChanged).size() == 1 &&
          events.events(historyId, QAccessible::DescriptionChanged).size() == 1,
      "history completion publishes native button changes and one custom List "
      "busy change");
  events.clear();
  view.setHistoryRequestPending("virtual-thread", false);
  settle();
  result &= expect(events.events(historyId).isEmpty() &&
                       events.events(listId).isEmpty(),
      "identical available history state emits no accessibility event");

  ConversationSnapshot emptySnapshot;
  emptySnapshot.threadId = "virtual-thread";
  result &= expect(changed(view.reconcile(std::move(emptySnapshot))),
                   "the accessibility fixture can become empty");
  settle();
  result &= expect(list && list->childCount() == 1 &&
                       list->child(0)->role() == QAccessible::StaticText &&
                       list->child(0)->parent() == list,
                   "an empty conversation exposes its one visible native "
                   "status label");

  QWidget *overlay = view.findChild<QWidget *>(
      QStringLiteral("conversationStagingOverlay"));
  QAccessibleInterface *overlayAccessible =
      overlay ? QAccessible::queryAccessibleInterface(overlay) : nullptr;
  const QAccessible::Id overlayId =
      overlayAccessible ? QAccessible::uniqueId(overlayAccessible) : 0;
  events.clear();
  view.beginThreadSelection("accessible-next-thread");
  settle();
  QAccessibleInterface *loading = list ? list->child(0) : nullptr;
  result &= expect(
      list && list->childCount() == 1 && loading &&
          loading->role() == QAccessible::StatusBar && loading->state().busy &&
          loading->text(QAccessible::Name) ==
              QStringLiteral("Loading conversation"),
      "the visible loading cover replaces obscured rows with one named busy "
      "status object");
  const auto overlayState =
      events.events(overlayId, QAccessible::StateChanged);
  result &= expect(overlayState.size() == 1 &&
                       overlayState.front().changedStates.busy &&
                       overlayState.front().state.busy,
                   "loading publishes one busy transition on its stable status object");
  events.clear();
  view.beginThreadSelection("accessible-next-thread");
  settle();
  result &= expect(events.events(overlayId).isEmpty(),
                   "repeated loading selection is an accessibility no-op");
  ConversationSnapshot loaded;
  loaded.threadId = "accessible-next-thread";
  events.clear();
  result &= expect(changed(view.reconcile(loaded)),
                   "the accessibility target conversation completes loading");
  settle();
  const auto loadedState =
      events.events(overlayId, QAccessible::StateChanged);
  result &= expect(loadedState.size() == 1 &&
                       loadedState.front().changedStates.busy &&
                       !loadedState.front().state.busy && overlay &&
                       !overlay->isVisible(),
                   "loading completion publishes one not-busy transition on "
                   "the retained status object");
  events.clear();
  result &= expect(view.reconcile(loaded) ==
                       ConversationView::ReconciliationResult::Unchanged,
                   "an identical loaded conversation is a semantic no-op");
  settle();
  result &= expect(events.events(overlayId).isEmpty(),
                   "identical loaded state emits no accessibility event");
  return result;
#endif
}

bool accessibilitySurvivesResidencyAndModelChanges() {
#if !QT_CONFIG(accessibility)
  return true;
#else
  auto owner = std::make_unique<ConversationView>();
  auto &view = *owner;
  view.resize(700, 360);
  view.show();
  auto snapshot = conversation(2000, 100);
  bool result =
      expect(changed(view.reconcile(snapshot)) && waitForResidency(view),
             "logical accessibility lifecycle fixture settles");
  auto *list = QAccessible::queryAccessibleInterface(view.viewport());
  if (!list || list->childCount() != 2000)
    return expect(false, "all loaded logical rows are accessible");
  const auto constructions = view.property("conversationCardConstructions");
  const auto measurements = view.property("conversationHeightIndexUpdateSteps");
  const int residents = view.materializedCardCount();
  const int documents = view.findChildren<QTextDocument *>().size();
  QElapsedTimer traversal;
  traversal.start();
  QList<QAccessible::Id> identities;
  for (int row = 0; row < list->childCount(); ++row) {
    auto *item = list->child(row);
    result &=
        expect(item && item->isValid() && list->indexOfChild(item) == row &&
                   item->text(QAccessible::Description)
                       .contains(QString::number(100 + row)),
               "every logical row enumerates in model order with bounded text");
    identities.push_back(QAccessible::uniqueId(item));
  }
  std::cout << "Accessible 2000-row traversal: "
            << traversal.nsecsElapsed() / 1000 << " us\n";
  result &= expect(
      view.property("conversationCardConstructions") == constructions &&
          view.property("conversationHeightIndexUpdateSteps") == measurements &&
          view.materializedCardCount() == residents &&
          view.findChildren<QTextDocument *>().size() == documents,
      "full accessibility enumeration constructs and measures no renderers or "
      "documents");
  auto *first = list->child(0);
  const auto firstId = QAccessible::uniqueId(first);
  const auto selection = view.selectionModel()->selectedRows();
  first->actionInterface()->doAction(
      QAccessibleActionInterface::setFocusAction());
  result &= expect(
      waitForResidency(view) && !first->state().offscreen &&
          first->state().focused && list->focusChild() == first &&
          first->focusChild() == first &&
          view.selectionModel()->selectedRows() == selection,
      "explicit accessible focus reveals the row without altering selection");
  auto *body = first->child(0);
  QPointer<QObject> originalRenderer(body ? body->object() : nullptr);
  auto *card =
      body ? qobject_cast<ConversationCard *>(body->object()) : nullptr;
  auto *copy =
      card ? card->findChild<QToolButton *>(QStringLiteral("cardCopyButton"))
           : nullptr;
  auto *copyInterface = QAccessible::queryAccessibleInterface(copy);
  if (copyInterface && copyInterface->actionInterface())
    copyInterface->actionInterface()->doAction(
        QAccessibleActionInterface::pressAction());
  result &= expect(
      copyInterface &&
          QApplication::clipboard()->text() == QStringLiteral("Answer 100") &&
          body->parent() == first && first->indexOfChild(body) == 0,
      "the logical row exposes the sole real Copy implementation");
  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
  result &= expect(
      waitForResidency(view) && originalRenderer.isNull() &&
          QAccessible::accessibleInterface(firstId) == first &&
          first->childCount() == 0 && first->state().offscreen &&
          list->child(0) == first && list->childCount() == 2000,
      "recycling deletes the renderer but preserves the logical identity");
  tests::AccessibilityEventProbe events;
  const auto updated = message(100, "Changed while offscreen");
  result &= expect(
      applyPresentation(view, updated).has_value() &&
          first->text(QAccessible::Description)
              .contains(QStringLiteral("Changed while offscreen")) &&
          events.events(firstId, QAccessible::DescriptionChanged).size() == 1 &&
          first->childCount() == 0,
      "offscreen updates notify the exact logical row without materializing "
      "it");
  events.clear();
  result &=
      expect(applyPresentation(view, updated) == PresentationImpact::None &&
                 events.events(firstId).isEmpty(),
             "unchanged offscreen content emits no accessibility event");
  first->actionInterface()->doAction(
      QAccessibleActionInterface::setFocusAction());
  result &= expect(waitForResidency(view) && first->childCount() == 1 &&
                       QAccessible::uniqueId(list->child(0)) == firstId,
                   "returning to a recycled row preserves its accessible ID");

  auto prefix = conversation(3, 97);
  snapshot.sections.insert(snapshot.sections.begin(), prefix.sections.begin(),
                           prefix.sections.end());
  result &=
      expect(changed(view.reconcile(snapshot)) && list->child(3) == first,
             "history insertion preserves existing accessible identities");
  std::swap(snapshot.sections[3], snapshot.sections[20]);
  result &= expect(
      changed(view.reconcile(snapshot)) && list->child(20) == first &&
          list->indexOfChild(first) == 20,
      "model moves update position without replacing accessible identity");
  snapshot.sections.erase(snapshot.sections.begin() + 20);
  result &= expect(changed(view.reconcile(snapshot)) &&
                       !QAccessible::accessibleInterface(firstId),
                   "true removal retires the exact accessible ID");

  auto folded = pinnedTurnPage(0, 4);
  auto reasoning = message(9999);
  reasoning.kind = CardKind::Reasoning;
  reasoning.payload = ReasoningData{"Hidden reasoning"};
  folded.sections.push_back(
      {"reasoning-section", reasoning.turnId, {reasoning}, {}});
  result &=
      expect(changed(view.reconcile(folded)), "folding fixture reconciles");
  auto *root = list->child(0);
  auto *nested = list->child(1);
  const auto nestedId = QAccessible::uniqueId(nested);
  root->actionInterface()->doAction(
      QAccessibleActionInterface::setFocusAction());
  auto *rootBody = root->child(0);
  auto *rootCard =
      rootBody ? qobject_cast<ConversationCard *>(rootBody->object()) : nullptr;
  auto *disclosure = rootCard ? rootCard->findChild<QToolButton *>(
                                    QStringLiteral("cardDisclosureButton"))
                              : nullptr;
  auto *disclosureInterface = QAccessible::queryAccessibleInterface(disclosure);
  if (disclosureInterface && disclosureInterface->actionInterface())
    disclosureInterface->actionInterface()->doAction(
        QAccessibleActionInterface::pressAction());
  result &=
      expect(disclosure && rootCard->isCollapsed() &&
                 disclosureInterface->state().collapsed &&
                 nested->state().invisible && !nested->state().selectable &&
                 nested->actionInterface()->actionNames().isEmpty() &&
                 !list->selectionInterface()->select(nested) &&
                 list->childCount() == 6 &&
                 QAccessible::uniqueId(list->child(1)) == nestedId,
             "native disclosure owns folding; hidden logical children retain "
             "identity but cannot act");
  if (disclosureInterface && disclosureInterface->actionInterface())
    disclosureInterface->actionInterface()->doAction(
        QAccessibleActionInterface::pressAction());
  result &= expect(disclosure && !rootCard->isCollapsed() &&
                       nested->state().selectable &&
                       QAccessible::uniqueId(list->child(1)) == nestedId,
                   "unfolding restores interaction on the same logical row");
  auto *reasoningItem = list->child(5);
  const auto reasoningId = QAccessible::uniqueId(reasoningItem);
  auto options = view.presentationOptions();
  options.showReasoning = false;
  events.clear();
  view.setPresentationOptions(options);
  result &= expect(
      reasoningItem->state().invisible && !reasoningItem->state().selectable &&
          reasoningItem->rect().isEmpty() && list->child(5) == reasoningItem &&
          events.events(reasoningId, QAccessible::StateChanged).size() == 1,
      "filtering hides logical rows without removing their identity");
  options.showReasoning = true;
  view.setPresentationOptions(options);
  result &= expect(list->child(5) == reasoningItem &&
                       reasoningItem->state().selectable &&
                       QAccessible::uniqueId(reasoningItem) == reasoningId,
                   "restoring visibility reuses the same accessible row");

  ConversationSnapshot prompt;
  prompt.threadId = "virtual-thread";
  VisibleCardData local{LocalPromptKey{999},
                        CardKind::LocalPrompt,
                        prompt.threadId,
                        "turn-prompt",
                        "local",
                        LocalPromptData{999, "Continue"}};
  prompt.sections.push_back(
      {"prompt-section", "turn-prompt", {local}, local.key});
  result &=
      expect(changed(view.reconcile(prompt)), "optimistic fixture reconciles");
  auto *promptItem = list->child(0);
  const auto promptId = QAccessible::uniqueId(promptItem);
  local.kind = CardKind::UserMessage;
  local.payload = UserMessageData{"Continue"};
  local.itemId = "authoritative";
  prompt.sections.front().cards.front() = local;
  result &= expect(
      changed(view.reconcile(prompt)) && list->child(0) == promptItem &&
          QAccessible::uniqueId(promptItem) == promptId,
      "optimistic acknowledgement preserves the accessible row identity");
  auto switched = conversation(1);
  switched.threadId = "another-thread";
  switched.sections[0].cards[0].threadId = switched.threadId;
  switched.sections[0].cards[0].key =
      AuthoritativeItemKey{switched.threadId, "turn-0", "item-0"};
  result &= expect(
      changed(view.reconcile(switched)) &&
          !QAccessible::accessibleInterface(promptId),
      "thread replacement retires old handles instead of retargeting them");
  result &=
      expect(std::ranges::all_of(identities,
                                 [](QAccessible::Id id) {
                                   return !QAccessible::accessibleInterface(id);
                                 }),
             "all enumerated identities retire with their model rows");
  const auto finalId = QAccessible::uniqueId(list->child(0));
  view.selectionModel()->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
  QObject::connect(view.selectionModel(), &QItemSelectionModel::currentChanged,
                   &view, [&owner] { owner.reset(); });
  list->child(0)->actionInterface()->doAction(
      QAccessibleActionInterface::setFocusAction());
  result &=
      expect(!owner && !QAccessible::accessibleInterface(finalId),
             "teardown during an accessible action retires outstanding IDs "
             "without using a deleted interface");
  return result;
#endif
}

bool collapsedLargeCardsSkipBodyProjection() {
  FileChangesData changes;
  changes.cwd = "/workspace";
  changes.changes.reserve(5'000);
  for (int index = 0; index < 5'000; ++index) {
    changes.changes.push_back(
        {"src/generated/file-" + std::to_string(index) + ".cpp", "update",
         index % 9, index % 4});
  }
  VisibleCardData card{
      AuthoritativeItemKey{"virtual-thread", "large-files", "changes"},
      CardKind::FileChanges,
      "virtual-thread",
      "large-files",
      "changes",
      std::move(changes),
      nodegraph::NodeStatus::Completed};
  ConversationSnapshot snapshot;
  snapshot.threadId = "virtual-thread";
  snapshot.sections.push_back(
      {"large-file-section", "large-files", {card}, std::nullopt});

  ConversationView view;
  view.resize(820, 320);
  view.show();
  static_cast<void>(view.reconcileStaged(std::move(snapshot)));
  bool result =
      expect(waitUntil([&] { return !view.structuralStagingActive(); }, 5000),
             "large collapsed file-change fixture commits through staging");
  settle();
  static_cast<void>(view.viewport()->grab());
  settle();
  const QModelIndex index = view.conversationModel()->index(0);
  ConversationCard *richCard = materializedCard(view, stableKey(card.key));
  auto *fileList = richCard ? richCard->findChild<QTextBrowser *>(
                                  QStringLiteral("fileChangesList"))
                            : nullptr;
  const bool bodySkipped =
      index.isValid() && richCard && richCard->isCollapsed() && fileList &&
      !fileList->isVisibleTo(view.viewport()) &&
      view.visualRect(index).height() == richCard->height() &&
      richCard->height() <= 64 && view.materializedCardCount() <= 1 &&
      richCard->property("fileChangesBodyRebuilds").toULongLong() == 0;
  if (!bodySkipped)
    std::cerr
        << "collapsed large card: valid=" << index.isValid()
        << " height=" << view.visualRect(index).height()
        << " materialized=" << view.materializedCardCount() << " bodyRebuilds="
        << (richCard
                ? richCard->property("fileChangesBodyRebuilds").toULongLong()
                : std::numeric_limits<qulonglong>::max())
        << '\n';
  result &= expect(
      bodySkipped,
      "collapsed large cards paint only their header without converting or "
      "laying out their body");
  QElapsedTimer expansionTimer;
  expansionTimer.start();
  richCard->setCollapsed(false);
  const qint64 expansionMicros = expansionTimer.nsecsElapsed() / 1000;
  const bool boundedExpansion =
      fileList && fileList->document()->blockCount() == 5'000 &&
      richCard->property("fileChangesBodyRebuilds").toULongLong() == 1 &&
      expansionMicros < InstrumentedTimingScale * 100'000;
  if (!boundedExpansion)
    std::cerr << "large file-change expansion us=" << expansionMicros
              << " rebuilds="
              << richCard->property("fileChangesBodyRebuilds").toULongLong()
              << " blocks="
              << (fileList ? fileList->document()->blockCount() : -1) << '\n';
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
      {AuthoritativeItemKey{"deferred", "turn", "user"}, CardKind::UserMessage,
       "deferred", "turn", "user", UserMessageData{large, {}}},
      {AuthoritativeItemKey{"deferred", "turn", "agent"},
       CardKind::AgentMessage, "deferred", "turn", "agent",
       AgentMessageData{large, false}},
      {AuthoritativeItemKey{"deferred", "turn", "command"},
       CardKind::CommandExecution, "deferred", "turn", "command",
       CommandExecutionData{large, large, "/workspace", {}, {}},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{"deferred", "turn", "activity"},
       CardKind::AgentActivity, "deferred", "turn", "activity",
       AgentActivityData{"spawn_agent", {}, large, large},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{"deferred", "turn", "reasoning"},
       CardKind::Reasoning, "deferred", "turn", "reasoning",
       ReasoningData{large}, nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{"deferred", "turn", "plan"}, CardKind::Plan,
       "deferred", "turn", "plan",
       PlanData{large, {{large, nodegraph::NodeStatus::Running}}, {}},
       nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{"deferred", "turn", "image"},
       CardKind::ImageGeneration, "deferred", "turn", "image",
       ImageGenerationData{{}, large}, nodegraph::NodeStatus::Running},
      {AuthoritativeItemKey{"deferred", "turn", "generic"},
       CardKind::GenericActivity, "deferred", "turn", "generic",
       GenericActivityData{"customActivity", large},
       nodegraph::NodeStatus::Running},
      {LocalPromptKey{912},
       CardKind::LocalPrompt,
       "deferred",
       "turn",
       {},
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
          return view->markdownSource().contains(
              QString::fromStdString(marker));
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
          return label->property("kind").toString() !=
                     QStringLiteral("title") &&
                 label->text().contains(QString::fromStdString(marker));
        });
  };

  bool result = true;
  for (VisibleCardData &data : cards) {
    ConversationCard card(data, true, nullptr, 820);
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
  const QString activeApplicationStyle = qApp->style()->objectName();
  qApp->setStyleSheet(codexui::UiStyle::applicationStyleSheet());
  using namespace codexui::codex::middle;
  if (argc == 2 && std::string_view(argv[1]) == "--selection-focus") {
    const QString requestedStyle = qEnvironmentVariable("QT_STYLE_OVERRIDE");
    const bool styleAvailable =
        std::ranges::any_of(QStyleFactory::keys(), [&](const QString &style) {
          return style.compare(requestedStyle, Qt::CaseInsensitive) == 0;
        });
    if (!requestedStyle.isEmpty() && !styleAvailable) {
      std::cout << "SKIP: unavailable Qt style " << requestedStyle.toStdString()
                << '\n';
      return 77;
    }
    if (!requestedStyle.isEmpty() &&
        activeApplicationStyle.compare(requestedStyle, Qt::CaseInsensitive) !=
            0) {
      std::cerr << "FAILED: requested style " << requestedStyle.toStdString()
                << " but Qt activated " << activeApplicationStyle.toStdString()
                << '\n';
      return EXIT_FAILURE;
    }
    return keyboardNavigationAndViewIsolation() ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  bool result = true;
  const auto run = [&result](const char *name, bool (*test)()) {
    const bool passed = test();
    if (!passed)
      std::cerr << "CASE FAILED: " << name << '\n';
    result &= passed;
  };
  run("viewportProportionalFoundation", viewportProportionalFoundation);
  run("exactStructuralRowsPreserveTheViewport",
      exactStructuralRowsPreserveTheViewport);
  run("rejectedDeltaTransactionsAreAtomic", rejectedDeltaTransactionsAreAtomic);
  run("largeRootReplacementPreservesTheResidentSurface",
      largeRootReplacementPreservesTheResidentSurface);
  run("rootReplacementRecomputesFoldedSectionGeometry",
      rootReplacementRecomputesFoldedSectionGeometry);
  run("prefixGeometryChangeRepaintsFollowingTurnSurface",
      prefixGeometryChangeRepaintsFollowingTurnSurface);
  run("rightAnchoredCoalescedRowsMatchCanonicalOrder",
      rightAnchoredCoalescedRowsMatchCanonicalOrder);
  run("boundedTailAppendIsViewportProportional",
      boundedTailAppendIsViewportProportional);
  run("targetedVisibilityChangeIsLocal", targetedVisibilityChangeIsLocal);
  run("atomicPagingAndFollowingArrival", atomicPagingAndFollowingArrival);
  run("pinnedTurnPagingPreservesRetainedActivityAnchor",
      pinnedTurnPagingPreservesRetainedActivityAnchor);
  run("removedInactiveAnchorUsesBoundedVisibleFallback",
      removedInactiveAnchorUsesBoundedVisibleFallback);
  run("loadedYouLandmarksStayAccessibleAndVirtualized",
      loadedYouLandmarksStayAccessibleAndVirtualized);
  run("semanticRootsRemainTheirOwnViewportAnchors",
      semanticRootsRemainTheirOwnViewportAnchors);
  run("providerPaginationControlReflectsGraphState",
      providerPaginationControlReflectsGraphState);
  run("inactiveAuthorityReplacementRetiresCardState",
      inactiveAuthorityReplacementRetiresCardState);
  run("threadIncarnationOwnsCompletePresentationState",
      threadIncarnationOwnsCompletePresentationState);
  run("disclosureDefaultsAreIndependentOfResidency",
      disclosureDefaultsAreIndependentOfResidency);
  run("pendingVisibilityUsesTheCurrentPolicy",
      pendingVisibilityUsesTheCurrentPolicy);
  run("tallViewportPublishesOneFrameThenAdmitsOverscan",
      tallViewportPublishesOneFrameThenAdmitsOverscan);
  run("stagedSnapshotsAreDeferredAndLatestWins",
      stagedSnapshotsAreDeferredAndLatestWins);
  run("unchangedStageResumesDeferredOverscanAdmission",
      unchangedStageResumesDeferredOverscanAdmission);
  run("delayedThreadSelectionSpinner", delayedThreadSelectionSpinner);
  run("queuedAdmissionYieldsToThreadSelection",
      queuedAdmissionYieldsToThreadSelection);
  run("rejectedStagesPreserveThePresentedFrame",
      rejectedStagesPreserveThePresentedFrame);
  run("stagedCommitRejectsReentrantMutation",
      stagedCommitRejectsReentrantMutation);
  run("stagedCardCleanupRejectsReentrantMutation",
      stagedCardCleanupRejectsReentrantMutation);
  run("committedCallbackMayDestroyView", committedCallbackMayDestroyView);
  run("destructiveCompletionSuppressesPromptCallbacks",
      destructiveCompletionSuppressesPromptCallbacks);
  run("destructivePromptCallbackStopsTheBatch",
      destructivePromptCallbackStopsTheBatch);
  run("promptCallbackMayDestroyView", promptCallbackMayDestroyView);
  run("virtualTurnSurfaceUsesOneRenderer", virtualTurnSurfaceUsesOneRenderer);
  run("directTailGrowsTheRetainedTurnSurface",
      directTailGrowsTheRetainedTurnSurface);
  run("stagedCommandOutputFollowsAfterFinalGeometry",
      stagedCommandOutputFollowsAfterFinalGeometry);
  run("commandOutputScrollOwnsConversationFollowing",
      commandOutputScrollOwnsConversationFollowing);
  run("multipleDetachedCommandsShareOneOuterPauseCause",
      multipleDetachedCommandsShareOneOuterPauseCause);
  run("offscreenCommandRemovalRetiresInnerAndOuterPause",
      offscreenCommandRemovalRetiresInnerAndOuterPause);
  run("stagedInactiveTruncationCannotResurrectCommandState",
      stagedInactiveTruncationCannotResurrectCommandState);
  run("singleRendererLifecycleAndResidency",
      singleRendererLifecycleAndResidency);
  run("hiddenResidentPresentationPrecedesScroll",
      hiddenResidentPresentationPrecedesScroll);
  run("acknowledgedSteeringMovesAboveFollowingActivity",
      acknowledgedSteeringMovesAboveFollowingActivity);
  run("singleRendererInteractionTargetsVisibleChildren",
      singleRendererInteractionTargetsVisibleChildren);
  run("collapsedAgentPhaseSettlesAuthoritativeGeometry",
      collapsedAgentPhaseSettlesAuthoritativeGeometry);
  run("semanticSelectionsSurviveRecycling", semanticSelectionsSurviveRecycling);
  run("markdownCompletionCannotRetargetSelection",
      markdownCompletionCannotRetargetSelection);
  run("keyboardNavigationAndViewIsolation", keyboardNavigationAndViewIsolation);
  run("bidirectionalLazyMeasurementPreservesNativeScrollMotion",
      bidirectionalLazyMeasurementPreservesNativeScrollMotion);
  run("largeIncomingCommandUsesBoundedFinalWidthLayout",
      largeIncomingCommandUsesBoundedFinalWidthLayout);
  run("streamingMarkdownKeepsOneDocument", streamingMarkdownKeepsOneDocument);
  run("interactiveResizeCoalescesConversationReflow",
      interactiveResizeCoalescesConversationReflow);
  run("runtimeGeometryEnvironmentKeepsOneInteractiveRenderer",
      runtimeGeometryEnvironmentKeepsOneInteractiveRenderer);
  run("accessibilityUsesLogicalRowsAndRealControls",
      accessibilityUsesLogicalRowsAndRealControls);
  run("accessibilitySurvivesResidencyAndModelChanges",
      accessibilitySurvivesResidencyAndModelChanges);
  run("collapsedLargeCardsSkipBodyProjection",
      collapsedLargeCardsSkipBodyProjection);
  run("collapsedInteractionDefersEveryHeavyCardBody",
      collapsedInteractionDefersEveryHeavyCardBody);
  if (result)
    std::cout << "Conversation virtualization tests passed\n";
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
