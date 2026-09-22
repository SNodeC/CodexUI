// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/WorkerLogic.h"
#include "codex/ui/NodeGraphUiAdapter.h"

#include <QApplication>
#include <QAccessible>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QPersistentModelIndex>
#include <QScrollBar>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace codexui::codex {
namespace {

using nodegraph::NodeKind;
using nodegraph::NodeRef;
using nodegraph::NodeState;

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << message << '\n';
  return false;
}

bool changed(middle::ConversationView::ReconciliationResult result) {
  return result == middle::ConversationView::ReconciliationResult::Changed;
}

bool finishStaging(middle::ConversationView &view) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (view.structuralStagingActive() && elapsed.elapsed() < 3000) {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  return !view.structuralStagingActive();
}

NodeState state(std::string id, std::string type = {}, std::string text = {}) {
  NodeState value;
  value.fields.emplace("id", std::move(id));
  if (!type.empty())
    value.fields.emplace("type", std::move(type));
  if (!text.empty())
    value.fields.emplace("text", std::move(text));
  return value;
}

std::optional<middle::ConversationDelta>
projectDelta(ui::NodeGraphUiAdapter &adapter, const NodeRef &thread,
             std::initializer_list<NodeRef> items, bool structural) {
  const std::vector<NodeRef> values(items);
  return adapter.conversationDelta(thread, values, structural);
}

struct Fixture {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  int turnSerial = 0;

  Fixture() {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "thread-ui"}, state("thread-ui"));
    static_cast<void>(write.finish());
  }

  void appendTurn(std::string answerText) {
    const int serial = turnSerial++;
    const std::string turnId = "turn-" + std::to_string(serial);
    const std::string promptId = "prompt-" + std::to_string(serial);
    const std::string answerId = "answer-" + std::to_string(serial);
    auto write = graph.write();
    NodeRef turn = write.upsert({NodeKind::Turn, turnId}, state(turnId));
    NodeRef prompt = write.upsert(
        {NodeKind::Item, promptId},
        state(promptId, "userMessage", "prompt " + std::to_string(serial)));
    NodeRef answer =
        write.upsert({NodeKind::Item, answerId},
                     state(answerId, "agentMessage", std::move(answerText)));
    write.setParent(thread, turn);
    write.setParent(turn, prompt);
    write.setParent(turn, answer);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, prompt);
    static_cast<void>(write.finish());
  }
};

QModelIndex firstPaintedIndex(middle::ConversationView &view) {
  for (int y = 0; y < view.viewport()->height(); ++y) {
    const QModelIndex index =
        view.indexAt(QPoint(view.viewport()->width() / 2, y));
    if (index.isValid())
      return index;
  }
  return {};
}

bool oldUiConsumesAdapterSnapshotsAtomically() {
  Fixture fixture;
  for (int index = 0; index < 24; ++index)
    fixture.appendTurn("answer " + std::to_string(index));

  ui::NodeGraphUiAdapter adapter(fixture.graph);
  middle::ConversationView view;
  view.resize(760, 560);
  view.show();
  QApplication::processEvents();

  const auto initial = adapter.conversation(fixture.thread);
  if (!require(initial.has_value(), "initial adapter read failed") ||
      !require(changed(view.reconcile(*initial)),
               "initial UI reconciliation was empty"))
    return false;
  QApplication::processEvents();

  if (!require(view.conversationModel()->rowCount() == 48,
               "selected history was not indexed in one reconciliation") ||
      !require(view.findChildren<QWidget *>(
                       QStringLiteral("conversationCardPlaceholder"))
                   .empty(),
               "old UI unexpectedly retained graph placeholders") ||
      !require(view.verticalScrollBar()->value() ==
                   view.verticalScrollBar()->maximum(),
               "initial following position is not the final bottom"))
    return false;

  int owners = 0;
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row)
    if (view.conversationModel()
            ->index(row)
            .data(middle::ConversationItemModel::TurnRootRole)
            .toBool())
      ++owners;
  if (!require(owners == 24, "not every turn has exactly one owning card"))
    return false;

  fixture.appendTurn("new following answer");
  const auto appended = adapter.conversation(fixture.thread);
  if (!require(appended.has_value(), "appended adapter read failed") ||
      !require(changed(view.reconcile(*appended)),
               "new cards were not presented"))
    return false;
  QApplication::processEvents();
  return require(view.conversationModel()->rowCount() == 50,
                 "new cards failed to enter the item view immediately") &&
         require(view.verticalScrollBar()->value() ==
                     view.verticalScrollBar()->maximum(),
                 "following update did not settle at its final bottom");
}

bool pausedViewportKeepsItsPaintedAnchor() {
  Fixture fixture;
  for (int index = 0; index < 30; ++index)
    fixture.appendTurn(std::string(180, static_cast<char>('a' + index % 20)));
  ui::NodeGraphUiAdapter adapter(fixture.graph);
  middle::ConversationView view;
  view.resize(760, 520);
  view.show();
  const auto initial = adapter.conversation(fixture.thread);
  if (!initial || !changed(view.reconcile(*initial)))
    return false;
  QApplication::processEvents();

  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
  QApplication::processEvents();
  const QModelIndex anchor = firstPaintedIndex(view);
  if (!require(anchor.isValid(), "paused viewport has no painted anchor"))
    return false;
  const std::string key =
      anchor.data(middle::ConversationItemModel::StableKeyRole)
          .toString()
          .toStdString();
  const int y = view.visualRect(anchor).top();

  fixture.appendTurn("offscreen tail");
  const auto appended = adapter.conversation(fixture.thread);
  if (!appended || !changed(view.reconcile(*appended)))
    return false;
  QApplication::processEvents();

  const QModelIndex retained = view.conversationModel()->indexForStableKey(key);
  return require(retained.isValid() && view.visualRect(retained).top() == y,
                 "paused incoming tail moved the painted anchor");
}

bool promptMorphPreservesExactTargetAndWidget() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef providerTurn;
  NodeRef prompt;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "thread-prompt"},
                          state("thread-prompt"));
    turn = write.upsert({NodeKind::Turn, "turn-prompt"}, state("turn-prompt"));
    NodeState promptState = state("local-prompt", "localPrompt", "hello");
    promptState.fields.emplace("submissionId", std::uint64_t{41});
    promptState.fields.emplace("dispatchState", "inFlight");
    prompt =
        write.upsert({NodeKind::Item, "local-prompt"}, std::move(promptState));
    write.setParent(thread, turn);
    write.setParent(turn, prompt);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, prompt);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  middle::ConversationView view;
  view.resize(700, 480);
  view.show();
  int acknowledgements = 0;
  NodeRef acknowledged;
  view.setPromptMaterializedAction([&](NodeRef target) {
    ++acknowledgements;
    acknowledged = std::move(target);
    return true;
  });
  auto snapshot = adapter.conversation(thread);
  if (!snapshot || !changed(view.reconcile(*snapshot)))
    return false;
  if (!require(finishStaging(view),
               "local prompt did not finish its staged presentation"))
    return false;
  const auto before = view.findChildren<middle::ConversationCard *>();
  if (!require(before.size() == 1, "local prompt did not render once") ||
      !require(acknowledgements == 0,
               "local prompt acknowledged before authoritative identity"))
    return false;
  middle::ConversationCard *stable = before.front();
  middle::MarkdownTextView *const body =
      stable->findChild<middle::MarkdownTextView *>();
  QTextDocument *const document = body ? body->document() : nullptr;
  const std::string promptKey =
      middle::stableKey(middle::LocalPromptKey{41});
  const QPersistentModelIndex persistentIndex(
      view.conversationModel()->indexForStableKey(promptKey));
  QAccessibleInterface *const accessibleCard =
      QAccessible::queryAccessibleInterface(stable);
  if (!require(body && document && persistentIndex.isValid() && accessibleCard,
               "local prompt interaction identities were unavailable"))
    return false;
  view.setCurrentIndex(persistentIndex);
  view.selectionModel()->select(persistentIndex,
                                QItemSelectionModel::ClearAndSelect |
                                    QItemSelectionModel::Rows);
  QTextCursor selection(document);
  selection.setPosition(0);
  selection.setPosition(
      std::min(3, std::max(0, document->characterCount() - 1)),
      QTextCursor::KeepAnchor);
  body->setTextCursor(selection);
  body->setFocus(Qt::TabFocusReason);
  QApplication::processEvents();
  const QString selectedText = body->textCursor().selectedText();

  NodeRef authoritative;
  {
    auto write = graph.write();
    write.setField(prompt, "dispatchState", "awaitingMaterialization");
    providerTurn =
        write.upsert({NodeKind::Turn, "provider-turn-prompt"},
                     state("provider-turn-prompt"));
    authoritative =
        write.upsert({NodeKind::Item, "authoritative-prompt"},
                     state("authoritative-prompt", "userMessage", "hello"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{41});
    write.setParent(thread, providerTurn);
    write.setParent(providerTurn, authoritative);
    write.relate(providerTurn, nodegraph::RelationKind::TurnRootItem,
                 authoritative);
    write.relate(authoritative, nodegraph::RelationKind::PromptMaterialization,
                 prompt);
    static_cast<void>(write.finish());
  }
  const auto materialized =
      projectDelta(adapter, thread, {authoritative, prompt}, true);
  if (!require(materialized.has_value(),
               "prompt morph did not produce a structural projection"))
    return false;
  if (!require(view.applyConversationDelta(*materialized).has_value(),
               "prompt morph structural projection was rejected"))
    return false;
  QApplication::processEvents();
  const auto after = view.findChildren<middle::ConversationCard *>();
  if (!require(after.size() == 1, "prompt morph created a duplicate card") ||
      !require(after.front() == stable, "prompt morph replaced its widget") ||
      !require(stable->findChild<middle::MarkdownTextView *>() == body &&
                   body->document() == document &&
                   body->textCursor().selectedText() == selectedText &&
                   QApplication::focusWidget() == body &&
                   persistentIndex.isValid() &&
                   view.currentIndex() == persistentIndex &&
                   view.selectionModel()->isSelected(persistentIndex) &&
                   QAccessible::queryAccessibleInterface(stable) ==
                       accessibleCard,
               "prompt promotion replaced interaction, document, model, or "
               "accessibility identity") ||
      !require(acknowledgements == 1,
               "prompt morph did not acknowledge exactly once") ||
      !require(acknowledged == prompt,
               "prompt morph discarded its exact NodeRef target"))
    return false;
  if (!require(after.front()->data().target == authoritative,
               "prompt morph did not adopt its authoritative NodeRef"))
    return false;

  const auto replayed = view.applyConversationDelta(*materialized);
  if (!require(replayed.has_value() && acknowledgements == 1,
               "unchanged prompt projection was rejected or acknowledged "
               "twice"))
    return false;

  const int promotedTop = stable->mapTo(view.viewport(), QPoint{}).y();
  {
    auto write = graph.write();
    write.remove(prompt);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.conversation(thread);
  if (!require(snapshot.has_value(),
               "local retirement did not project the authoritative card"))
    return false;
  if (!require(view.reconcile(*snapshot) !=
                   middle::ConversationItemModel::StructuralChangeResult::Rejected,
               "local retirement snapshot was rejected"))
    return false;
  QApplication::processEvents();
  const auto retired = view.findChildren<middle::ConversationCard *>();
  bool result = require(retired.size() == 1 && retired.front() == stable,
                        "local retirement replaced the promoted QWidget");
  result &= require(retired.size() == 1 &&
                        retired.front()->data().target == authoritative,
                    "local retirement did not transfer the action target");
  result &= require(retired.size() == 1 &&
                        retired.front()->mapTo(view.viewport(), QPoint{}).y() ==
                            promotedTop,
                    "local retirement moved the promoted card");
  result &= require(
      retired.size() == 1 &&
          retired.front()->findChild<middle::MarkdownTextView *>() == body &&
          body->document() == document &&
          body->textCursor().selectedText() == selectedText &&
          QApplication::focusWidget() == body && persistentIndex.isValid() &&
          view.currentIndex() == persistentIndex &&
          view.selectionModel()->isSelected(persistentIndex) &&
          QAccessible::queryAccessibleInterface(retired.front()) ==
              accessibleCard,
      "local retirement replaced promoted interaction or accessibility state");
  result &= require(acknowledgements == 1,
                    "local retirement acknowledged the prompt again");
  return result;
}

bool coalescedPromptMaterializationAcknowledgesAnUnseenLocalRow() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef local;
  NodeRef authoritative;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "coalesced-prompt-thread"},
                          state("coalesced-prompt-thread"));
    NodeRef localTurn =
        write.upsert({NodeKind::Turn, "coalesced-local-turn"},
                     state("coalesced-local-turn"));
    NodeRef providerTurn =
        write.upsert({NodeKind::Turn, "coalesced-provider-turn"},
                     state("coalesced-provider-turn"));
    NodeState localState =
        state("coalesced-local", "localPrompt", "Coalesced prompt");
    localState.fields.emplace("submissionId", std::uint64_t{91});
    localState.fields.emplace("dispatchState", "awaitingMaterialization");
    local = write.upsert({NodeKind::Item, "coalesced-local"},
                         std::move(localState));
    authoritative =
        write.upsert({NodeKind::Item, "coalesced-authoritative"},
                     state("coalesced-authoritative", "userMessage",
                           "Coalesced prompt"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{91});
    write.setParent(thread, localTurn);
    write.setParent(thread, providerTurn);
    write.setParent(localTurn, local);
    write.setParent(providerTurn, authoritative);
    write.relate(localTurn, nodegraph::RelationKind::TurnRootItem, local);
    write.relate(providerTurn, nodegraph::RelationKind::TurnRootItem,
                 authoritative);
    write.relate(authoritative,
                 nodegraph::RelationKind::PromptMaterialization, local);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  middle::ConversationView view;
  middle::ConversationSnapshot empty;
  empty.threadId = "coalesced-prompt-thread";
  if (!changed(view.reconcile(std::move(empty))) || !finishStaging(view))
    return false;

  int acknowledgements = 0;
  NodeRef acknowledged;
  view.setPromptMaterializedAction([&](NodeRef target) {
    ++acknowledgements;
    acknowledged = std::move(target);
    return true;
  });
  const auto delta =
      projectDelta(adapter, thread, {authoritative, local}, true);
  if (!require(delta && delta->rows.size() == 1 &&
                   delta->removals == std::vector<NodeRef>{local},
               "coalesced materialization did not project one authoritative "
               "row and one hidden local removal"))
    return false;
  const auto applied = view.applyConversationDelta(*delta);
  const middle::ConversationItemModel::Row *row =
      view.conversationModel()->row(0);
  if (!require(applied.has_value() &&
                   view.conversationModel()->rowCount() == 1 && row &&
                   row->card.kind == middle::CardKind::UserMessage &&
                   row->card.target == authoritative &&
                   acknowledgements == 1 && acknowledged == local,
               "an exact delta did not acknowledge its unseen local prompt"))
    return false;

  middle::ConversationView replacementView;
  int replacementAcknowledgements = 0;
  NodeRef replacementAcknowledged;
  replacementView.setPromptMaterializedAction([&](NodeRef target) {
    ++replacementAcknowledgements;
    replacementAcknowledged = std::move(target);
    return true;
  });
  const auto replacement = adapter.conversation(thread);
  if (!require(replacement.has_value(),
               "coalesced materialization had no authoritative snapshot"))
    return false;
  const auto replaced = replacementView.reconcile(*replacement);
  const middle::ConversationItemModel::Row *replacementRow =
      replacementView.conversationModel()->row(0);
  if (!require(
      replaced != middle::ConversationItemModel::StructuralChangeResult::Rejected &&
          replacementView.conversationModel()->rowCount() == 1 &&
          replacementRow &&
          replacementRow->card.kind == middle::CardKind::UserMessage &&
          replacementRow->card.target == authoritative &&
          replacementAcknowledgements == 1 && replacementAcknowledged == local,
      "an authoritative replacement did not acknowledge its unseen local "
      "prompt"))
    return false;
  return require(
      replacementView.reconcile(*replacement) ==
              middle::ConversationItemModel::StructuralChangeResult::Unchanged &&
          replacementAcknowledgements == 1,
      "an unchanged authoritative replacement acknowledged the prompt twice");
}

bool nonFirstTurnRootPreservesProviderOrderAndNestsOnlyFollowingRows() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef reasoning;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "late-root-thread"},
                          state("late-root-thread"));
    turn = write.upsert({NodeKind::Turn, "late-root-turn"},
                        state("late-root-turn"));
    reasoning = write.upsert(
        {NodeKind::Item, "early-reasoning"},
        state("early-reasoning", "reasoning", "Before the prompt"));
    write.setParent(thread, turn);
    write.setParent(turn, reasoning);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  middle::ConversationView view;
  view.resize(700, 480);
  view.show();
  auto snapshot = adapter.conversation(thread);
  if (!snapshot || !changed(view.reconcile(*snapshot)) || !finishStaging(view))
    return false;
  const qulonglong resets =
      view.conversationModel()->property("modelResetCount").toULongLong();

  NodeRef root;
  NodeRef answer;
  {
    auto write = graph.write();
    root = write.upsert({NodeKind::Item, "late-root"},
                        state("late-root", "userMessage", "Prompt"));
    answer = write.upsert({NodeKind::Item, "late-answer"},
                          state("late-answer", "agentMessage", "Answer"));
    write.setParent(turn, root);
    write.setParent(turn, answer);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    static_cast<void>(write.finish());
  }
  const auto delta = projectDelta(adapter, thread, {answer, root}, true);
  if (!require(delta.has_value(),
               "non-first Turn root did not produce an exact delta"))
    return false;
  if (!require(view.applyConversationDelta(*delta).has_value(),
               "non-first Turn root required an authoritative fallback"))
    return false;

  const auto *first = view.conversationModel()->row(0);
  const auto *second = view.conversationModel()->row(1);
  const auto *third = view.conversationModel()->row(2);
  if (!require(
      first && second && third && first->card.target == reasoning &&
          second->card.target == root && third->card.target == answer &&
          !first->turnRoot && !first->nested && second->turnRoot &&
          !second->nested && !third->turnRoot && third->nested &&
          view.conversationModel()->property("modelResetCount").toULongLong() ==
              resets,
      "provider order or root-relative nesting changed during exact delivery"))
    return false;

  middle::ConversationCard *rootCard = nullptr;
  for (middle::ConversationCard *card :
       view.findChildren<middle::ConversationCard *>())
    if (card->data().target == root)
      rootCard = card;
  QToolButton *const disclosure =
      rootCard ? rootCard->findChild<QToolButton *>(
                     QStringLiteral("cardDisclosureButton"))
               : nullptr;
  if (disclosure)
    disclosure->click();
  QApplication::processEvents();
  return require(
      disclosure && rootCard->isCollapsed() &&
          !view.visualRect(view.conversationModel()->index(0)).isEmpty() &&
          !view.visualRect(view.conversationModel()->index(1)).isEmpty() &&
          view.visualRect(view.conversationModel()->index(2)).isEmpty(),
      "folding a non-first root hid pre-root activity or retained its child");
}

bool controllerAndObserverExactDeltasConverge() {
  nodegraph::NodeGraph controllerGraph;
  nodegraph::NodeGraph observerGraph;
  struct PreparedGraph {
    NodeRef thread;
    NodeRef localPrompt;
  };
  const auto prepare = [](nodegraph::NodeGraph &graph, bool controller) {
    auto write = graph.write();
    NodeRef thread =
        write.upsert({NodeKind::Thread, "exact-provider-order-thread"},
                     state("exact-provider-order-thread"));
    NodeRef turn = write.upsert(nodegraph::scopedTurnNodeId(
        "exact-provider-order-thread", "exact-provider-order-turn"));
    write.setField(turn, "protocolId", "exact-provider-order-turn");
    write.setField(turn, "protocolThreadId", "exact-provider-order-thread");
    NodeRef root = write.upsert(
        nodegraph::scopedItemNodeId(turn->id(), "exact-provider-root"),
        state("exact-provider-root", "userMessage", "Opening prompt"));
    NodeRef activity = write.upsert(
        nodegraph::scopedItemNodeId(turn->id(), "exact-provider-activity"),
        state("exact-provider-activity", "agentMessage", "Activity"));
    write.setField(root, "protocolId", "exact-provider-root");
    write.setField(root, "protocolTurnId", "exact-provider-order-turn");
    write.setField(activity, "protocolId", "exact-provider-activity");
    write.setField(activity, "protocolTurnId", "exact-provider-order-turn");
    write.setParent(thread, turn);
    write.setParent(turn, root);
    write.setParent(turn, activity);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    NodeRef localPrompt;
    if (controller) {
      NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
      NodeState local =
          state("exact-local-prompt", "localPrompt", "Steer here");
      local.fields.emplace("local", true);
      local.fields.emplace("submissionId", std::uint64_t{405});
      local.fields.emplace("clientUserMessageId", "exact-client-id");
      local.fields.emplace("dispatchState", "awaitingMaterialization");
      local.fields.emplace("startsTurn", false);
      localPrompt = write.upsert({NodeKind::Item, "exact-local-prompt"},
                                 std::move(local));
      write.setParent(turn, localPrompt);
      write.relate(runtime, nodegraph::RelationKind::PendingPrompt,
                   localPrompt);
      write.relate(thread, nodegraph::RelationKind::PendingPrompt,
                   localPrompt);
    }
    static_cast<void>(write.finish());
    return PreparedGraph{thread, localPrompt};
  };

  const PreparedGraph controller = prepare(controllerGraph, true);
  const PreparedGraph observer = prepare(observerGraph, false);
  ui::NodeGraphUiAdapter controllerAdapter(controllerGraph);
  ui::NodeGraphUiAdapter observerAdapter(observerGraph);
  middle::ConversationView controllerView;
  middle::ConversationView observerView;
  const auto controllerInitial =
      controllerAdapter.conversation(controller.thread);
  const auto observerInitial = observerAdapter.conversation(observer.thread);
  if (!require(controllerInitial && observerInitial &&
                   changed(controllerView.reconcile(*controllerInitial)) &&
                   changed(observerView.reconcile(*observerInitial)),
               "controller/observer exact-order fixtures did not reconcile"))
    return false;

  int controllerAcknowledgements = 0;
  int observerAcknowledgements = 0;
  NodeRef acknowledged;
  controllerView.setPromptMaterializedAction([&](NodeRef prompt) {
    ++controllerAcknowledgements;
    acknowledged = std::move(prompt);
    return true;
  });
  observerView.setPromptMaterializedAction([&](NodeRef) {
    ++observerAcknowledgements;
    return true;
  });
  const auto notification = [] {
    return nodegraph::DecodedMessage{
        nodegraph::DecodedMessageKind::ServerNotification, "item/started",
        std::nullopt,
        nodegraph::Value::Object{
            {"threadId", nodegraph::Value("exact-provider-order-thread")},
            {"turnId", nodegraph::Value("exact-provider-order-turn")},
            {"item",
             nodegraph::Value(nodegraph::Value::Object{
                 {"id", nodegraph::Value("exact-provider-steering")},
                 {"type", nodegraph::Value("userMessage")},
                 {"clientId", nodegraph::Value("exact-client-id")},
                 {"text", nodegraph::Value("Steer here")}})}}};
  };
  nodegraph::ProtocolUpdater controllerUpdater(controllerGraph);
  nodegraph::ProtocolUpdater observerUpdater(observerGraph);
  const nodegraph::ApplyResult controllerChange =
      controllerUpdater.apply(notification());
  const nodegraph::ApplyResult observerChange =
      observerUpdater.apply(notification());
  const auto route = [](ui::NodeGraphUiAdapter &adapter,
                        const NodeRef &thread,
                        const nodegraph::GraphChange &change) {
    return adapter.conversationRoute(
        {change.revision, change.affected, change.removed, false,
         change.childListsChanged, change.providerAuthorityRevision},
        thread);
  };
  const ui::NodeGraphUiAdapter::ConversationRoute controllerRoute =
      route(controllerAdapter, controller.thread, controllerChange.change);
  const ui::NodeGraphUiAdapter::ConversationRoute observerRoute =
      route(observerAdapter, observer.thread, observerChange.change);
  if (!require(controllerRoute.affected && controllerRoute.structural &&
                   !controllerRoute.authorityReplacement &&
                   observerRoute.affected && observerRoute.structural &&
                   !observerRoute.authorityReplacement,
               "a provider item notification unnecessarily requested a full "
               "conversation replacement"))
    return false;
  const auto controllerDelta = controllerAdapter.conversationDelta(
      controller.thread, controllerRoute.items, controllerRoute.structural);
  const auto observerDelta = observerAdapter.conversationDelta(
      observer.thread, observerRoute.items, observerRoute.structural);
  if (!require(controllerDelta && observerDelta,
               "controller/observer exact notification deltas were unavailable") ||
      !require(controllerView.applyConversationDelta(*controllerDelta) &&
                   observerView.applyConversationDelta(*observerDelta),
               "controller/observer exact notification deltas were rejected"))
    return false;

  const auto signature = [](const middle::ConversationView &view) {
    std::vector<std::string> result;
    for (int index = 0; index < view.conversationModel()->rowCount(); ++index) {
      const middle::ConversationItemModel::Row *row =
          view.conversationModel()->row(index);
      if (!row)
        continue;
      result.push_back(
          row->card.itemId + ':' +
          (row->turnRoot ? "root" : (row->nested ? "nested" : "standalone")));
    }
    return result;
  };
  const std::vector<std::string> expected{
      "exact-provider-root:root", "exact-provider-activity:nested",
      "exact-provider-steering:nested"};
  return require(signature(controllerView) == expected &&
                     signature(observerView) == expected,
                 "controller and observer exact routes diverged from provider "
                 "order or root-relative nesting") &&
         require(controllerAcknowledgements == 1 &&
                     acknowledged == controller.localPrompt &&
                     observerAcknowledgements == 0,
                 "prompt materialization acknowledgement leaked across "
                 "controller/observer ownership");
}

bool steeringMorphAdoptsProviderOrderWithoutReplacingItsWidget() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef root;
  NodeRef steering;
  NodeRef progress;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "thread-steering"},
                          state("thread-steering"));
    turn =
        write.upsert({NodeKind::Turn, "turn-steering"}, state("turn-steering"));
    root = write.upsert({NodeKind::Item, "root-steering"},
                        state("root-steering", "userMessage", "Start"));
    NodeState local = state("local-steering", "localPrompt", "Steer here");
    local.fields.emplace("submissionId", std::uint64_t{72});
    local.fields.emplace("dispatchState", "inFlight");
    local.fields.emplace("admittedAtMs",
                         QDateTime::currentMSecsSinceEpoch() - 1500);
    local.fields.emplace("startsTurn", false);
    steering =
        write.upsert({NodeKind::Item, "local-steering"}, std::move(local));
    progress =
        write.upsert({NodeKind::Item, "later-progress"},
                     state("later-progress", "agentMessage", "Later progress"));
    write.setParent(thread, turn);
    write.setParent(turn, root);
    write.setParent(turn, steering);
    write.setParent(turn, progress);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, root);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  middle::ConversationView view;
  view.resize(700, 520);
  view.show();
  int acknowledgements = 0;
  view.setPromptMaterializedAction([&](NodeRef target) {
    ++acknowledgements;
    return target == steering;
  });
  auto snapshot = adapter.conversation(thread);
  if (!snapshot || !changed(view.reconcile(*snapshot)))
    return false;
  QApplication::processEvents();
  const auto findCard = [&view](const std::string &key) {
    for (middle::ConversationCard *card :
         view.findChildren<middle::ConversationCard *>())
      if (middle::stableKey(card->data().key) == key)
        return card;
    return static_cast<middle::ConversationCard *>(nullptr);
  };
  middle::ConversationCard *stable =
      findCard(middle::stableKey(middle::LocalPromptKey{72}));
  const std::string progressKey =
      middle::stableKey(middle::AuthoritativeItemKey{
          "thread-steering", "turn-steering", "later-progress"});
  const QModelIndex progressIndex =
      view.conversationModel()->indexForStableKey(progressKey);
  if (!require(stable && progressIndex.isValid() &&
                   stable->mapTo(view.viewport(), QPoint{}).y() <
                       view.visualRect(progressIndex).top(),
               "steering did not begin ahead of its later activity"))
    return false;

  {
    auto write = graph.write();
    write.setField(steering, "dispatchState", "awaitingMaterialization");
    static_cast<void>(write.finish());
  }
  const auto acknowledgedPrompt =
      projectDelta(adapter, thread, {steering}, false);
  if (!acknowledgedPrompt)
    return false;
  static_cast<void>(view.applyConversationDelta(*acknowledgedPrompt));
  QApplication::processEvents();
  QTimer *animation =
      stable->findChild<QTimer *>(QStringLiteral("pendingAnimationTimer"));
  if (!require(animation && animation->isActive() &&
                   stable->data().kind == middle::CardKind::LocalPrompt &&
                   acknowledgements == 0,
               "successful steering acceptance must keep pending animation "
               "until its authoritative item arrives"))
    return false;

  NodeRef authoritative;
  {
    auto write = graph.write();
    authoritative =
        write.upsert({NodeKind::Item, "provider-steering"},
                     state("provider-steering", "userMessage", "Steer here"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{72});
    write.setParent(turn, authoritative);
    write.relate(authoritative, nodegraph::RelationKind::PromptMaterialization,
                 steering);
    write.replaceChildren(
        turn, std::array<NodeRef, 4>{root, steering, progress, authoritative});
    static_cast<void>(write.finish());
  }
  const auto materialized =
      projectDelta(adapter, thread, {authoritative, steering}, true);
  if (!materialized)
    return false;
  if (!require(view.applyConversationDelta(*materialized).has_value(),
               "provider-ordered steering materialization was rejected"))
    return false;
  QApplication::processEvents();
  const int promotedTop = stable->mapTo(view.viewport(), QPoint{}).y();
  const QModelIndex promotedProgressIndex =
      view.conversationModel()->indexForStableKey(progressKey);
  if (!require(stable->data().kind == middle::CardKind::UserMessage &&
                   animation && !animation->isActive() &&
                   acknowledgements == 1 &&
                   promotedProgressIndex.isValid() &&
                   promotedTop > view.visualRect(promotedProgressIndex).top(),
               "authoritative steering materialization did not stop its "
               "animation while adopting provider order in place"))
    return false;

  {
    auto write = graph.write();
    write.remove(steering);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.conversation(thread);
  if (!snapshot)
    return false;
  if (!require(view.reconcile(*snapshot) !=
                   middle::ConversationItemModel::StructuralChangeResult::Rejected,
               "steering retirement snapshot was rejected"))
    return false;
  QApplication::processEvents();
  const QModelIndex retiredProgressIndex =
      view.conversationModel()->indexForStableKey(progressKey);
  return require(
      findCard(middle::stableKey(middle::LocalPromptKey{72})) == stable &&
          stable->data().target == authoritative &&
          stable->mapTo(view.viewport(), QPoint{}).y() == promotedTop &&
          retiredProgressIndex.isValid() &&
          promotedTop > view.visualRect(retiredProgressIndex).top() &&
          acknowledgements == 1,
      "steering retirement recreated or moved its promoted provider row");
}

bool pendingSteeringFollowsIncomingItemsUntilHistoryEntry() {
  using namespace nodegraph;
  bool result = true;
  for (const bool responseFirst : {false, true}) {
    NodeGraph graph;
    ThreadChannels channels;
    WorkerLogic worker(graph, channels);
    ui::NodeGraphUiAdapter adapter(graph);
    middle::ConversationView view;
    view.resize(760, 900);
    view.show();
    const auto notify = [&](std::string method, Value::Object payload) {
      static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification,
                                      std::move(method),
                                      {},
                                      std::move(payload)}));
    };
    const auto item = [&](std::string id, std::string type,
                          std::string client = {}) {
      Value::Object data{{"id", id},
                         {"type", type},
                         {"text", client.empty() ? id : "same steer"}};
      if (!client.empty())
        data.emplace("clientId", client);
      notify("item/started", {{"threadId", "steering-live"},
                              {"turnId", "turn"},
                              {"item", std::move(data)}});
    };
    notify("thread/started",
           {{"thread", Value::Object{{"id", "steering-live"}}}});
    notify("turn/started",
           {{"threadId", "steering-live"},
            {"turn", Value::Object{{"id", "turn"}, {"status", "inProgress"}}}});
    item("opening", "userMessage");
    NodeRef thread;
    {
      auto read = graph.tryRead();
      thread = read->find({NodeKind::Thread, "steering-live"});
    }
    static_cast<void>(channels.drainWorkerToQtWake());
    WorkerToQtMessage message;
    while (channels.tryReceiveForQt(message)) {
    }
    static_cast<void>(view.reconcile(*adapter.conversation(thread)));
    view.setPromptMaterializedAction([&](NodeRef prompt) {
      return messageAdmitted(worker.promptMaterialized(prompt));
    });
    const auto flush = [&] {
      static_cast<void>(channels.drainWorkerToQtWake());
      std::vector<NodeRef> changedItems;
      bool structural = false;
      while (channels.tryReceiveForQt(message)) {
        if (const auto *event = std::get_if<GraphChanged>(&message)) {
          const auto route = adapter.conversationRoute(*event, thread);
          if (!route.affected)
            continue;
          structural |= route.structural;
          for (const NodeRef &item : route.items)
            if (std::ranges::find(changedItems, item) == changedItems.end())
              changedItems.push_back(item);
        }
      }
      if (!changedItems.empty()) {
        auto delta =
            adapter.conversationDelta(thread, changedItems, structural);
        result &= require(
            delta && view.applyConversationDelta(std::move(*delta)).has_value(),
            "pending-tail protocol traffic must apply incrementally without "
            "a snapshot fallback");
      }
      QApplication::processEvents();
    };
    const auto submit = [&] {
      NodeAction action{thread, NodeActionKind::SubmitPrompt};
      action.promptText = "same steer";
      return worker.admitPrompt(std::move(action), {},
                                QDateTime::currentMSecsSinceEpoch() - 1500);
    };
    auto first = submit();
    if (!first.command)
      return false;
    static_cast<void>(submit());
    flush();
    const auto card = [&](int id) -> middle::ConversationCard * {
      for (auto *widget : view.findChildren<middle::ConversationCard *>())
        if (middle::stableKey(widget->data().key) ==
            "prompt:" + std::to_string(id))
          return widget;
      return nullptr;
    };
    auto *firstCard = card(1);
    auto *secondCard = card(2);
    if (!require(firstCard && secondCard,
                 "both queued steering cards are resident"))
      return false;
    const auto order = [&] {
      std::vector<std::string> actual;
      for (int i = 0; i < view.conversationModel()->rowCount(); ++i) {
        const auto &data = view.conversationModel()->row(i)->card;
        actual.push_back(data.itemId.empty() ? middle::stableKey(data.key)
                                             : data.itemId);
      }
      return actual;
    };
    item("intervening", "agentMessage");
    flush();
    result &=
        require(order() == std::vector<std::string>{"opening", "intervening",
                                                    "prompt:1", "prompt:2"},
                "incoming history precedes all unconsumed steering in "
                "submission order");
    {
      auto read = graph.tryRead();
      const auto *index = read->inspectorIndex(thread);
      result &= require(index && index->latestAgentMessage() &&
                            scalarTextFromValue(valueMember(
                                *read->state(index->latestAgentMessage()),
                                "text")) == "intervening",
                        "insertion before pending prompts updates the "
                        "Inspector's latest message index");
    }
    PromptTransition next;
    if (responseFirst) {
      next =
          worker.completePrompt(first.command->localPrompt, true, {}, "turn");
      flush();
      auto *timer = firstCard->findChild<QTimer *>(
          QStringLiteral("pendingAnimationTimer"));
      result &= require(timer && timer->isActive(),
                        "request acceptance keeps pending steering animated");
    }
    const int beforePromotion = firstCard->y();
    item("steering-1", "userMessage", first.command->clientUserMessageId);
    flush();
    result &= require(
        card(1) == firstCard && firstCard->y() == beforePromotion &&
            firstCard->data().kind == middle::CardKind::UserMessage &&
            !firstCard
                 ->findChild<QTimer *>(QStringLiteral("pendingAnimationTimer"))
                 ->isActive(),
        "authoritative entry ends animation without replacing or relocating "
        "the steering widget");
    if (!responseFirst) {
      next =
          worker.completePrompt(first.command->localPrompt, true, {}, "turn");
      flush();
    }
    if (!next.command)
      return false;
    item("later", "agentMessage");
    flush();
    result &= require(
        order() == std::vector<std::string>{"opening", "intervening",
                                            "steering-1", "later", "prompt:2"},
        "later items overtake only the still-pending steering");
    item("steering-2", "userMessage", next.command->clientUserMessageId);
    flush();
    static_cast<void>(
        worker.completePrompt(next.command->localPrompt, true, {}, "turn"));
    flush();
    const auto settled = order();
    static_cast<void>(view.reconcile(*adapter.conversation(thread)));
    result &= require(order() == settled && card(1) == firstCard &&
                          card(2) == secondCard,
                      "retirement and full snapshot retain authoritative order "
                      "and both widget identities");
    auto failed = submit();
    if (!failed.command)
      return false;
    flush();
    static_cast<void>(
        worker.failPrompt(failed.command->localPrompt, "Rejected"));
    flush();
    auto *failedCard = card(3);
    result &= require(
        failedCard &&
            !failedCard
                 ->findChild<QTimer *>(QStringLiteral("pendingAnimationTimer"))
                 ->isActive() &&
            std::get<middle::LocalPromptData>(failedCard->data().payload)
                .requiresExplicitRecovery,
        "rejection stops pending feedback and preserves explicit recovery");
  }
  return result;
}

bool activeTurnDeltaKeepsModelAuthorityAndWidgets() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef firstTurn;
  NodeRef secondTurn;
  NodeRef firstRoot;
  NodeRef secondRoot;
  NodeRef firstAnswer;
  NodeRef secondAnswer;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "active-thread"},
                          state("active-thread"));
    firstTurn =
        write.upsert({NodeKind::Turn, "active-turn-1"}, state("active-turn-1"));
    secondTurn =
        write.upsert({NodeKind::Turn, "active-turn-2"}, state("active-turn-2"));
    firstRoot = write.upsert(
        {NodeKind::Item, "active-root-1"},
        state("active-root-1", "userMessage", "First prompt"));
    secondRoot = write.upsert(
        {NodeKind::Item, "active-root-2"},
        state("active-root-2", "userMessage", "Second prompt"));
    firstAnswer = write.upsert(
        {NodeKind::Item, "active-answer-1"},
        state("active-answer-1", "agentMessage", "First answer"));
    secondAnswer = write.upsert(
        {NodeKind::Item, "active-answer-2"},
        state("active-answer-2", "agentMessage", "Second answer"));
    write.setParent(thread, firstTurn);
    write.setParent(thread, secondTurn);
    write.setParent(firstTurn, firstRoot);
    write.setParent(firstTurn, firstAnswer);
    write.setParent(secondTurn, secondRoot);
    write.setParent(secondTurn, secondAnswer);
    write.relate(firstTurn, nodegraph::RelationKind::TurnRootItem, firstRoot);
    write.relate(secondTurn, nodegraph::RelationKind::TurnRootItem, secondRoot);
    write.relate(thread, nodegraph::RelationKind::ActiveTurn, firstTurn);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  middle::ConversationView view;
  view.resize(760, 720);
  view.show();
  const auto initial = adapter.conversation(thread);
  if (!require(initial && changed(view.reconcile(*initial)),
               "active-Turn fixture did not reconcile"))
    return false;
  QApplication::processEvents();

  const auto cardForTarget = [&view](const NodeRef &target) {
    for (middle::ConversationCard *card :
         view.findChildren<middle::ConversationCard *>())
      if (card->data().target == target)
        return card;
    return static_cast<middle::ConversationCard *>(nullptr);
  };
  QModelIndex firstIndex = view.conversationModel()->indexForTarget(firstRoot);
  QModelIndex secondIndex =
      view.conversationModel()->indexForTarget(secondRoot);
  middle::ConversationCard *firstCard = cardForTarget(firstRoot);
  middle::ConversationCard *secondCard = cardForTarget(secondRoot);
  const QRect firstGeometry = view.visualRect(firstIndex);
  const QRect secondGeometry = view.visualRect(secondIndex);
  const auto turnBorder = [&view](const NodeRef &answer) {
    const QModelIndex index = view.conversationModel()->indexForTarget(answer);
    const QImage frame = view.viewport()->grab().toImage();
    const int y = std::clamp(view.visualRect(index).center().y(), 0,
                             frame.height() - 1);
    return frame.pixelColor(1, y);
  };
  const QColor firstBorderBefore = turnBorder(firstAnswer);
  const QColor secondBorderBefore = turnBorder(secondAnswer);
  if (!require(firstIndex.data(middle::ConversationItemModel::ActiveTurnRole)
                       .toBool() &&
                   !secondIndex
                        .data(middle::ConversationItemModel::ActiveTurnRole)
                        .toBool() &&
                   firstCard && secondCard,
               "initial ActiveTurn relation did not own exactly the first "
               "Turn presentation"))
    return false;

  nodegraph::GraphChange graphChange;
  {
    auto write = graph.write();
    write.replaceRelated(thread, nodegraph::RelationKind::ActiveTurn,
                         std::array{secondTurn});
    graphChange = write.finish();
  }
  const nodegraph::GraphChanged notification{
      graphChange.revision, std::move(graphChange.affected),
      std::move(graphChange.removed), false,
      std::move(graphChange.childListsChanged)};
  const ui::NodeGraphUiAdapter::ConversationRoute route =
      adapter.conversationRoute(notification, thread);
  if (!require(route.affected && route.structural &&
                   !route.authorityReplacement && route.items.size() == 2 &&
                   std::ranges::find(route.items, firstRoot) !=
                       route.items.end() &&
                   std::ranges::find(route.items, secondRoot) !=
                       route.items.end(),
               "ActiveTurn routing did not carry both authoritative roots"))
    return false;
  auto delta = adapter.conversationDelta(thread, route.items, route.structural);
  if (!require(delta && view.applyConversationDelta(std::move(*delta)),
               "the authoritative ActiveTurn delta was rejected"))
    return false;
  QApplication::processEvents();

  firstIndex = view.conversationModel()->indexForTarget(firstRoot);
  secondIndex = view.conversationModel()->indexForTarget(secondRoot);
  int activeRoots = 0;
  for (int row = 0; row < view.conversationModel()->rowCount(); ++row) {
    const QModelIndex index = view.conversationModel()->index(row);
    if (index.data(middle::ConversationItemModel::TurnRootRole).toBool() &&
        index.data(middle::ConversationItemModel::ActiveTurnRole).toBool())
      ++activeRoots;
  }
  const QColor firstBorderAfter = turnBorder(firstAnswer);
  const QColor secondBorderAfter = turnBorder(secondAnswer);
  return require(
      !firstIndex.data(middle::ConversationItemModel::ActiveTurnRole).toBool() &&
          secondIndex.data(middle::ConversationItemModel::ActiveTurnRole)
              .toBool() &&
          activeRoots == 1 && cardForTarget(firstRoot) == firstCard &&
          cardForTarget(secondRoot) == secondCard &&
          view.visualRect(firstIndex) == firstGeometry &&
          view.visualRect(secondIndex) == secondGeometry &&
          firstBorderBefore.red() < firstBorderAfter.red() &&
          secondBorderAfter.red() < secondBorderBefore.red(),
      "ActiveTurn authority remained duplicated or replaced/moved its cards");
}

bool coalescedCanonicalFrontMatchesFullProjection() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef retained;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "front-block-thread"},
                          state("front-block-thread"));
    turn = write.upsert({NodeKind::Turn, "front-block-turn"},
                        state("front-block-turn"));
    retained = write.upsert(
        {NodeKind::Item, "front-block-retained"},
        state("front-block-retained", "agentMessage", "Retained tail"));
    write.setParent(thread, turn);
    write.setParent(turn, retained);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  middle::ConversationView incremental;
  incremental.resize(700, 480);
  incremental.show();
  const auto initial = adapter.conversation(thread);
  if (!require(initial && changed(incremental.reconcile(*initial)),
               "front-block fixture did not reconcile its retained row"))
    return false;

  NodeRef first;
  NodeRef second;
  {
    auto write = graph.write();
    first = write.upsert({NodeKind::Item, "front-block-first"},
                         state("front-block-first", "userMessage", "New root"));
    second = write.upsert(
        {NodeKind::Item, "front-block-second"},
        state("front-block-second", "agentMessage", "New nested row"));
    write.setParent(turn, first);
    write.setParent(turn, second);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, first);
    write.replaceChildren(turn,
                          std::array<NodeRef, 3>{first, second, retained});
    static_cast<void>(write.finish());
  }

  const auto delta = projectDelta(adapter, thread, {second, first}, true);
  const auto complete = adapter.conversation(thread);
  if (!require(delta && delta->rows.size() == 2 && complete &&
                   complete->sections.size() == 1 &&
                   complete->sections.front().cards.size() == 3,
               "front-block projections were unavailable") ||
      !require(incremental.applyConversationDelta(*delta).has_value(),
               "a dependency-ordered canonical front block was rejected"))
    return false;

  const middle::TurnSection &expected = complete->sections.front();
  bool result =
      require(incremental.conversationModel()->rowCount() ==
                  static_cast<int>(expected.cards.size()),
              "incremental front insertion silently lost a changed row");
  for (int rowIndex = 0;
       result && rowIndex < incremental.conversationModel()->rowCount();
       ++rowIndex) {
    const middle::ConversationItemModel::Row *row =
        incremental.conversationModel()->row(rowIndex);
    const middle::VisibleCardData &card =
        expected.cards[static_cast<std::size_t>(rowIndex)];
    result &=
        require(row && row->card == card && row->sectionKey == expected.key &&
                    row->turnRoot == (expected.rootCardKey &&
                                      card.key == *expected.rootCardKey) &&
                    row->nested == (expected.rootCardKey &&
                                    card.key != *expected.rootCardKey),
                "incremental and complete front-block projections diverged");
  }
  return result;
}

bool fileChangesUseCanonicalWorkspace() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef changes;
  {
    auto write = graph.write();
    NodeState threadState = state("thread-files");
    threadState.fields.emplace("cwd", "/workspace/thread");
    thread = write.upsert({NodeKind::Thread, "thread-files"},
                          std::move(threadState));
    turn = write.upsert({NodeKind::Turn, "turn-files"}, state("turn-files"));
    NodeState changesState = state("files", "fileChange");
    changesState.fields.emplace(
        "changes",
        nodegraph::Value::Array{nodegraph::Value(
            nodegraph::Value::Object{{"path", "src/file.cpp"},
                                     {"kind", "update"},
                                     {"additions", std::int64_t{7}},
                                     {"deletions", std::int64_t{3}},
                                     {"diff", "+different fallback\n"}})});
    changes = write.upsert({NodeKind::Item, "files"}, std::move(changesState));
    write.setParent(thread, turn);
    write.setParent(turn, changes);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  auto snapshot = adapter.conversation(thread);
  if (!require(snapshot && snapshot->sections.size() == 1 &&
                   snapshot->sections.front().cards.size() == 1,
               "file changes were not projected from the owning thread"))
    return false;
  const auto *inherited = std::get_if<middle::FileChangesData>(
      &snapshot->sections.front().cards.front().payload);
  if (!require(inherited && inherited->cwd == "/workspace/thread" &&
                   inherited->changes.size() == 1 &&
                   inherited->changes.front().additions == 7 &&
                   inherited->changes.front().deletions == 3,
               "relative file changes did not inherit the thread workspace "
               "and canonical diff counts"))
    return false;

  {
    auto write = graph.write();
    write.setField(changes, "cwd", "/workspace/item");
    static_cast<void>(write.finish());
  }
  auto projected = projectDelta(adapter, thread, {changes}, false);
  const middle::VisibleCardData *projectedCard =
      projected && projected->presentations.size() == 1
          ? &projected->presentations.front()
          : nullptr;
  const auto *specific =
      projectedCard
          ? std::get_if<middle::FileChangesData>(&projectedCard->payload)
          : nullptr;
  return require(specific && specific->cwd == "/workspace/item",
                 "an item-specific file workspace did not override the "
                 "thread workspace");
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex;
  bool result = true;
  const auto run = [&result](const char *name, bool (*test)()) {
    const bool passed = test();
    if (!passed)
      std::cerr << "CASE FAILED: " << name << '\n';
    result &= passed;
  };
  run("oldUiConsumesAdapterSnapshotsAtomically",
      oldUiConsumesAdapterSnapshotsAtomically);
  run("pausedViewportKeepsItsPaintedAnchor",
      pausedViewportKeepsItsPaintedAnchor);
  run("promptMorphPreservesExactTargetAndWidget",
      promptMorphPreservesExactTargetAndWidget);
  run("coalescedPromptMaterializationAcknowledgesAnUnseenLocalRow",
      coalescedPromptMaterializationAcknowledgesAnUnseenLocalRow);
  run("nonFirstTurnRootPreservesProviderOrderAndNestsOnlyFollowingRows",
      nonFirstTurnRootPreservesProviderOrderAndNestsOnlyFollowingRows);
  run("controllerAndObserverExactDeltasConverge",
      controllerAndObserverExactDeltasConverge);
  run("steeringMorphAdoptsProviderOrderWithoutReplacingItsWidget",
      steeringMorphAdoptsProviderOrderWithoutReplacingItsWidget);
  run("pendingSteeringFollowsIncomingItemsUntilHistoryEntry",
      pendingSteeringFollowsIncomingItemsUntilHistoryEntry);
  run("activeTurnDeltaKeepsModelAuthorityAndWidgets",
      activeTurnDeltaKeepsModelAuthorityAndWidgets);
  run("coalescedCanonicalFrontMatchesFullProjection",
      coalescedCanonicalFrontMatchesFullProjection);
  run("fileChangesUseCanonicalWorkspace", fileChangesUseCanonicalWorkspace);
  if (!result)
    return EXIT_FAILURE;
  std::cout << "NodeGraph conversation UI tests passed\n";
  return EXIT_SUCCESS;
}
