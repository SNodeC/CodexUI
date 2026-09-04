// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/ui/NodeGraphUiAdapter.h"

#include <QApplication>
#include <QScrollBar>

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

NodeState state(std::string id, std::string type = {}, std::string text = {}) {
  NodeState value;
  value.fields.emplace("id", std::move(id));
  if (!type.empty())
    value.fields.emplace("type", std::move(type));
  if (!text.empty())
    value.fields.emplace("text", std::move(text));
  return value;
}

struct Fixture {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  int turnSerial = 0;

  Fixture() {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "thread-ui"},
                          state("thread-ui"));
    static_cast<void>(write.finish());
  }

  void appendTurn(std::string answerText) {
    const int serial = turnSerial++;
    const std::string turnId = "turn-" + std::to_string(serial);
    const std::string promptId = "prompt-" + std::to_string(serial);
    const std::string answerId = "answer-" + std::to_string(serial);
    auto write = graph.write();
    NodeRef turn =
        write.upsert({NodeKind::Turn, turnId}, state(turnId));
    NodeRef prompt = write.upsert(
        {NodeKind::Item, promptId},
        state(promptId, "userMessage", "prompt " + std::to_string(serial)));
    NodeRef answer = write.upsert(
        {NodeKind::Item, answerId},
        state(answerId, "agentMessage", std::move(answerText)));
    write.setParent(thread, turn);
    write.setParent(turn, prompt);
    write.setParent(turn, answer);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, prompt);
    static_cast<void>(write.finish());
  }
};

middle::ConversationCard *firstPaintedCard(middle::ConversationView &view) {
  middle::ConversationCard *result = nullptr;
  int best = std::numeric_limits<int>::max();
  for (middle::ConversationCard *card :
       view.findChildren<middle::ConversationCard *>()) {
    const QPoint top = card->mapTo(view.viewport(), QPoint{});
    if (top.y() + card->height() <= 0 || top.y() >= view.viewport()->height())
      continue;
    if (top.y() < best) {
      best = top.y();
      result = card;
    }
  }
  return result;
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

  const auto initial =
      adapter.conversation(fixture.thread, 80, {true, true});
  if (!require(initial.has_value(), "initial adapter read failed") ||
      !require(view.reconcile(*initial), "initial UI reconciliation was empty"))
    return false;
  QApplication::processEvents();

  const auto cards = view.findChildren<middle::ConversationCard *>();
  if (!require(cards.size() == 48,
               "selected history was not materialized in one reconciliation") ||
      !require(view.findChildren<QWidget *>(
                       QStringLiteral("conversationCardPlaceholder"))
                       .empty(),
               "old UI unexpectedly retained graph placeholders") ||
      !require(view.verticalScrollBar()->value() ==
                   view.verticalScrollBar()->maximum(),
               "initial following position is not the final bottom"))
    return false;

  int owners = 0;
  for (middle::ConversationCard *card : cards)
    if (card->property("turnContainer").toBool())
      ++owners;
  if (!require(owners == 24, "not every turn has exactly one owning card"))
    return false;

  fixture.appendTurn("new following answer");
  const auto appended =
      adapter.conversation(fixture.thread, 80, {true, true});
  if (!require(appended.has_value(), "appended adapter read failed") ||
      !require(view.reconcile(*appended), "new cards were not presented"))
    return false;
  QApplication::processEvents();
  return require(view.findChildren<middle::ConversationCard *>().size() == 50,
                 "new cards failed to appear immediately") &&
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
  const auto initial =
      adapter.conversation(fixture.thread, 80, {true, true});
  if (!initial || !view.reconcile(*initial))
    return false;
  QApplication::processEvents();

  view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
  QApplication::processEvents();
  middle::ConversationCard *anchor = firstPaintedCard(view);
  if (!require(anchor != nullptr, "paused viewport has no painted anchor"))
    return false;
  const std::string key =
      anchor->property("conversationAnchorKey").toString().toStdString();
  const int y = anchor->mapTo(view.viewport(), QPoint{}).y();

  fixture.appendTurn("offscreen tail");
  const auto appended =
      adapter.conversation(fixture.thread, 80, {true, true});
  if (!appended || !view.reconcile(*appended))
    return false;
  QApplication::processEvents();

  for (middle::ConversationCard *card :
       view.findChildren<middle::ConversationCard *>()) {
    if (card->property("conversationAnchorKey").toString().toStdString() != key)
      continue;
    return require(card->mapTo(view.viewport(), QPoint{}).y() == y,
                   "paused incoming tail moved the painted anchor");
  }
  return require(false, "paused incoming tail replaced the anchor widget");
}

bool promptMorphPreservesExactTargetAndWidget() {
  nodegraph::NodeGraph graph;
  NodeRef thread;
  NodeRef turn;
  NodeRef prompt;
  {
    auto write = graph.write();
    thread = write.upsert({NodeKind::Thread, "thread-prompt"},
                          state("thread-prompt"));
    turn = write.upsert({NodeKind::Turn, "turn-prompt"},
                        state("turn-prompt"));
    NodeState promptState = state("local-prompt", "localPrompt", "hello");
    promptState.fields.emplace("submissionId", std::uint64_t{41});
    promptState.fields.emplace("dispatchState", "inFlight");
    prompt = write.upsert({NodeKind::Item, "local-prompt"},
                          std::move(promptState));
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
  view.setPromptMaterializedAction(
      [&](NodeRef target) {
        ++acknowledgements;
        acknowledged = std::move(target);
        return true;
      });
  auto snapshot = adapter.conversation(thread, 80, {true, true});
  if (!snapshot || !view.reconcile(*snapshot))
    return false;
  QApplication::processEvents();
  const auto before = view.findChildren<middle::ConversationCard *>();
  if (!require(before.size() == 1, "local prompt did not render once") ||
      !require(acknowledgements == 0,
               "local prompt acknowledged before authoritative identity"))
    return false;
  middle::ConversationCard *stable = before.front();

  {
    auto write = graph.write();
    write.setField(prompt, "dispatchState", "awaitingMaterialization");
    NodeRef authoritative = write.upsert(
        {NodeKind::Item, "authoritative-prompt"},
        state("authoritative-prompt", "userMessage", "hello"));
    write.setParent(turn, authoritative);
    write.relate(authoritative,
                 nodegraph::RelationKind::PromptMaterialization, prompt);
    write.relate(turn, nodegraph::RelationKind::TurnRootItem, authoritative);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.conversation(thread, 80, {true, true});
  if (!snapshot || !view.reconcile(*snapshot))
    return false;
  QApplication::processEvents();
  const auto after = view.findChildren<middle::ConversationCard *>();
  if (!require(after.size() == 1, "prompt morph created a duplicate card") ||
      !require(after.front() == stable, "prompt morph replaced its widget") ||
      !require(acknowledgements == 1,
               "prompt morph did not acknowledge exactly once") ||
      !require(acknowledged == prompt,
               "prompt morph discarded its exact NodeRef target"))
    return false;

  static_cast<void>(view.reconcile(*snapshot));
  return require(acknowledgements == 1,
                 "unchanged prompt projection acknowledged twice");
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex;
  if (!oldUiConsumesAdapterSnapshotsAtomically() ||
      !pausedViewportKeepsItsPaintedAnchor() ||
      !promptMorphPreservesExactTargetAndWidget())
    return EXIT_FAILURE;
  std::cout << "NodeGraph conversation UI tests passed\n";
  return EXIT_SUCCESS;
}
