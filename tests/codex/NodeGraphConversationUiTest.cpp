// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/ui/NodeGraphUiAdapter.h"

#include <QApplication>
#include <QDateTime>
#include <QScrollBar>
#include <QTimer>

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

  NodeRef authoritative;
  {
    auto write = graph.write();
    write.setField(prompt, "dispatchState", "awaitingMaterialization");
    authoritative = write.upsert(
        {NodeKind::Item, "authoritative-prompt"},
        state("authoritative-prompt", "userMessage", "hello"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{41});
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
  if (!require(acknowledgements == 1,
               "unchanged prompt projection acknowledged twice"))
    return false;

  const int promotedTop = stable->mapTo(view.viewport(), QPoint{}).y();
  {
    auto write = graph.write();
    write.remove(prompt);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.conversation(thread, 80, {true, true});
  if (!require(snapshot.has_value(),
               "local retirement did not project the authoritative card"))
    return false;
  static_cast<void>(view.reconcile(*snapshot));
  QApplication::processEvents();
  const auto retired = view.findChildren<middle::ConversationCard *>();
  bool result = require(retired.size() == 1 && retired.front() == stable,
                        "local retirement replaced the promoted QWidget");
  result &= require(retired.size() == 1 &&
                        retired.front()->data().target == authoritative,
                    "local retirement did not transfer the action target");
  result &= require(retired.size() == 1 &&
                        retired.front()
                                ->mapTo(view.viewport(), QPoint{})
                                .y() == promotedTop,
                    "local retirement moved the promoted card");
  result &= require(acknowledgements == 1,
                    "local retirement acknowledged the prompt again");
  return result;
}

bool steeringMorphKeepsItsSlotThroughRetirement() {
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
    turn = write.upsert({NodeKind::Turn, "turn-steering"},
                        state("turn-steering"));
    root = write.upsert({NodeKind::Item, "root-steering"},
                        state("root-steering", "userMessage", "Start"));
    NodeState local = state("local-steering", "localPrompt", "Steer here");
    local.fields.emplace("submissionId", std::uint64_t{72});
    local.fields.emplace("dispatchState", "inFlight");
    local.fields.emplace("admittedAtMs",
                         QDateTime::currentMSecsSinceEpoch() - 1500);
    local.fields.emplace("showPendingAnimation", false);
    local.fields.emplace("startsTurn", false);
    steering = write.upsert({NodeKind::Item, "local-steering"},
                            std::move(local));
    progress = write.upsert(
        {NodeKind::Item, "later-progress"},
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
  auto snapshot = adapter.conversation(thread, 80, {true, true});
  if (!snapshot || !view.reconcile(*snapshot))
    return false;
  QApplication::processEvents();
  const auto findCard = [&view](const std::string &key) {
    for (middle::ConversationCard *card :
         view.findChildren<middle::ConversationCard *>())
      if (card->property("conversationAnchorKey").toString().toStdString() ==
          key)
        return card;
    return static_cast<middle::ConversationCard *>(nullptr);
  };
  middle::ConversationCard *stable =
      findCard(middle::stableKey(middle::LocalPromptKey{72}));
  middle::ConversationCard *progressCard = findCard(middle::stableKey(
      middle::AuthoritativeItemKey{"thread-steering", "turn-steering",
                                   "later-progress"}));
  if (!require(stable && progressCard &&
                   stable->mapTo(view.viewport(), QPoint{}).y() <
                       progressCard->mapTo(view.viewport(), QPoint{}).y(),
               "steering did not begin ahead of its later activity"))
    return false;

  {
    auto write = graph.write();
    write.setField(steering, "dispatchState", "awaitingMaterialization");
    write.setField(steering, "showPendingAnimation", false);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.conversation(thread, 80, {true, true});
  if (!snapshot)
    return false;
  static_cast<void>(view.reconcile(*snapshot));
  QApplication::processEvents();
  QTimer *animation = stable->findChild<QTimer *>(
      QStringLiteral("pendingAnimationTimer"));
  if (!require(animation && animation->isActive() &&
                   stable->data().kind == middle::CardKind::LocalPrompt &&
                   acknowledgements == 0,
               "accepted steering stopped while awaiting its authoritative "
               "user item"))
    return false;

  NodeRef authoritative;
  {
    auto write = graph.write();
    authoritative = write.upsert(
        {NodeKind::Item, "provider-steering"},
        state("provider-steering", "userMessage", "Steer here"));
    write.setField(authoritative, "localSubmissionId", std::uint64_t{72});
    write.setParent(turn, authoritative);
    write.relate(authoritative,
                 nodegraph::RelationKind::PromptMaterialization, steering);
    write.replaceChildren(
        turn, std::array<NodeRef, 4>{root, steering, authoritative, progress});
    static_cast<void>(write.finish());
  }
  snapshot = adapter.conversation(thread, 80, {true, true});
  if (!snapshot)
    return false;
  static_cast<void>(view.reconcile(*snapshot));
  QApplication::processEvents();
  const int promotedTop = stable->mapTo(view.viewport(), QPoint{}).y();
  if (!require(stable->data().kind == middle::CardKind::UserMessage &&
                   animation && !animation->isActive() &&
                   acknowledgements == 1 && promotedTop <
                       progressCard->mapTo(view.viewport(), QPoint{}).y(),
               "authoritative steering materialization did not stop its "
               "animation in the original submitted slot"))
    return false;

  {
    auto write = graph.write();
    write.remove(steering);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.conversation(thread, 80, {true, true});
  if (!snapshot)
    return false;
  static_cast<void>(view.reconcile(*snapshot));
  QApplication::processEvents();
  return require(
      findCard(middle::stableKey(middle::LocalPromptKey{72})) == stable &&
          stable->data().target == authoritative &&
          stable->mapTo(view.viewport(), QPoint{}).y() == promotedTop &&
          promotedTop < progressCard->mapTo(view.viewport(), QPoint{}).y() &&
          acknowledgements == 1,
      "steering retirement recreated, moved, or reordered its stable card");
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
        nodegraph::Value::Array{nodegraph::Value(nodegraph::Value::Object{
            {"path", "src/file.cpp"}, {"kind", "update"}})});
    changes = write.upsert({NodeKind::Item, "files"},
                           std::move(changesState));
    write.setParent(thread, turn);
    write.setParent(turn, changes);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  auto snapshot = adapter.conversation(thread, 80, {true, true});
  if (!require(snapshot && snapshot->sections.size() == 1 &&
                   snapshot->sections.front().cards.size() == 1,
               "file changes were not projected from the owning thread"))
    return false;
  const auto *inherited = std::get_if<middle::FileChangesData>(
      &snapshot->sections.front().cards.front().payload);
  if (!require(inherited && inherited->cwd == "/workspace/thread",
               "relative file changes did not inherit the thread workspace"))
    return false;

  {
    auto write = graph.write();
    write.setField(changes, "cwd", "/workspace/item");
    static_cast<void>(write.finish());
  }
  auto projected = adapter.card(thread, changes, {true, true});
  const auto *specific =
      projected
          ? std::get_if<middle::FileChangesData>(&projected->payload)
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
  if (!oldUiConsumesAdapterSnapshotsAtomically() ||
      !pausedViewportKeepsItsPaintedAnchor() ||
      !promptMorphPreservesExactTargetAndWidget() ||
      !steeringMorphKeepsItsSlotThroughRetirement() ||
      !fileChangesUseCanonicalWorkspace())
    return EXIT_FAILURE;
  std::cout << "NodeGraph conversation UI tests passed\n";
  return EXIT_SUCCESS;
}
