// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/NodeGraphUiAdapter.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>

#include <iostream>
#include <string>

namespace codexui::codex {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

nodegraph::NodeRef addActivity(nodegraph::NodeGraph::WriteAccess &write,
                               const nodegraph::NodeRef &turn,
                               std::string id, std::string kind,
                               std::string childId, std::string status,
                               std::string tool = {}) {
  nodegraph::NodeState state;
  state.status = status == "completed"
                     ? nodegraph::NodeStatus::Completed
                     : status == "interrupted"
                           ? nodegraph::NodeStatus::Interrupted
                           : nodegraph::NodeStatus::Running;
  state.fields = {{"type", nodegraph::Value("subAgentActivity")},
                  {"kind", nodegraph::Value(std::move(kind))},
                  {"agentThreadId", nodegraph::Value(std::move(childId))},
                  {"status", nodegraph::Value(std::move(status))},
                  {"tool", nodegraph::Value(std::move(tool))}};
  const nodegraph::NodeRef item = write.upsert(
      {nodegraph::NodeKind::Item, std::move(id)}, std::move(state));
  write.setParent(turn, item);
  return item;
}

bool logicalAgentsRemainDeduplicated() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef owner;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef childOne;
  {
    auto write = graph.write();
    owner = write.upsert({nodegraph::NodeKind::Thread, "owner"});
    turn = write.upsert({nodegraph::NodeKind::Turn, "turn"});
    write.setParent(owner, turn);
    childOne = write.upsert({nodegraph::NodeKind::Thread, "child-one"},
                            {nodegraph::NodeStatus::Running, {}});
    const nodegraph::NodeRef started =
        addActivity(write, turn, "start", "started", "child-one",
                    "inProgress", "spawn_agent");
    write.relate(started, nodegraph::RelationKind::AgentChildThread,
                 childOne);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  auto snapshot = adapter.inspector(owner);
  bool result = expect(snapshot && snapshot->agents.agents.size() == 1 &&
                           snapshot->agents.agents.front().childThreadId ==
                               "child-one",
                       "a child start creates one logical Agent row");

  {
    auto write = graph.write();
    static_cast<void>(
        addActivity(write, turn, "progress", "progress", "child-one",
                    "inProgress"));
    static_cast<void>(
        addActivity(write, turn, "replay", "started", "child-one",
                    "inProgress", "spawn_agent"));
    static_cast<void>(
        addActivity(write, turn, "complete", "completed", "child-one",
                    "completed"));
    write.setStatus(childOne, nodegraph::NodeStatus::Completed);
    write.setField(childOne, "status", "completed");
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner);
  result &= expect(snapshot && snapshot->agents.agents.size() == 1 &&
                       snapshot->agents.agents.front().status == "completed",
                   "start, progress, replay and completion remain one row");

  {
    auto write = graph.write();
    static_cast<void>(addActivity(write, turn, "stale", "progress",
                                  "child-one", "inProgress"));
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner);
  result &= expect(snapshot && snapshot->agents.agents.front().status ==
                                    "completed",
                   "a stale active update cannot overwrite terminal status");

  {
    auto write = graph.write();
    nodegraph::NodeState ordinary;
    ordinary.fields = {
        {"type", nodegraph::Value("collabAgentToolCall")},
        {"tool", nodegraph::Value("send_message")},
        {"receiverThreadIds",
         nodegraph::Value(nodegraph::Value::Array{
             nodegraph::Value("not-a-spawn")})}};
    const nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "ordinary-collaboration"},
        std::move(ordinary));
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner);
  result &= expect(snapshot && snapshot->agents.agents.size() == 1,
                   "a non-spawn collaboration call creates no Agent row");

  {
    auto write = graph.write();
    nodegraph::NodeState spawn;
    spawn.status = nodegraph::NodeStatus::Running;
    spawn.fields = {
        {"type", nodegraph::Value("collabAgentToolCall")},
        {"tool", nodegraph::Value("spawn_agents_on_csv")},
        {"receiverThreadIds",
         nodegraph::Value(nodegraph::Value::Array{
             nodegraph::Value("child-two"),
             nodegraph::Value("child-two")})}};
    const nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "multi-spawn"}, std::move(spawn));
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner);
  result &= expect(snapshot && snapshot->agents.agents.size() == 2 &&
                       snapshot->agents.agents[0].childThreadId == "child-one" &&
                       snapshot->agents.agents[1].childThreadId == "child-two",
                   "two distinct children produce two stable ordered rows");

  {
    auto write = graph.write();
    static_cast<void>(addActivity(write, turn, "interrupt", "interrupted",
                                  "child-one", "interrupted"));
    write.setStatus(childOne, nodegraph::NodeStatus::Interrupted);
    write.setField(childOne, "status", "interrupted");
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner);
  result &= expect(snapshot && snapshot->agents.agents.size() == 2 &&
                       snapshot->agents.agents[0].childThreadId == "child-one" &&
                       snapshot->agents.agents[0].status == "interrupted" &&
                       snapshot->agents.agents[1].childThreadId == "child-two",
                   "interruption patches the existing row without reordering");
  return result;
}

bool establishedAgentsWidgetContractIsRetained() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef owner;
  {
    auto write = graph.write();
    owner = write.upsert({nodegraph::NodeKind::Thread, "widget-owner"});
    const nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "widget-turn"});
    write.setParent(owner, turn);
    static_cast<void>(addActivity(write, turn, "widget-agent", "started",
                                  "widget-child", "inProgress",
                                  "spawn_agent"));
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot = adapter.inspector(owner);
  middle::InspectorPane pane;
  pane.resize(440, 700);
  pane.show();
  pane.refresh(*snapshot);
  QCoreApplication::processEvents();

  bool result = expect(
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
          .empty(),
      "the hidden Agents tab constructs no Agent row widgets");
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  const auto frames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
  result &= expect(frames.size() == 1,
                   "activating Agents materializes the current logical row");
  if (!frames.empty()) {
    QFrame *stableFrame = frames.front();
    auto *content = frames.front()->findChild<QWidget *>(
        QStringLiteral("agentCardContent"));
    auto *disclosure = frames.front()->findChild<QToolButton *>(
        QStringLiteral("agentDisclosureButton"));
    result &= expect(content && disclosure && !content->isVisible(),
                     "the established Agent row starts collapsed");
    if (disclosure)
      disclosure->click();
    QCoreApplication::processEvents();
    result &= expect(content && content->isVisible(),
                     "the established disclosure behavior is retained");
    const qulonglong constructions =
        pane.property("agentRowConstructions").toULongLong();
    ui::InspectorSnapshot updated = *snapshot;
    updated.agents.agents.front().status = "completed";
    pane.refresh(updated);
    QCoreApplication::processEvents();
    const auto updatedFrames =
        pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
    auto *updatedContent = stableFrame->findChild<QWidget *>(
        QStringLiteral("agentCardContent"));
    result &= expect(updatedFrames.size() == 1 &&
                         updatedFrames.front() == stableFrame &&
                         updatedContent && updatedContent->isVisible() &&
                         pane.property("agentRowConstructions").toULongLong() ==
                             constructions,
                     "an Agent state change patches the stable expanded row "
                     "without rebuilding the Agents surface");
    const qulonglong patches =
        pane.property("agentRowPatches").toULongLong();
    pane.refresh(updated);
    QCoreApplication::processEvents();
    result &= expect(pane.property("agentRowPatches").toULongLong() == patches,
                     "repeating identical Agent state performs zero row work");
  }
  return result;
}

bool planAndRequestUpdatesRetainUnaffectedRows() {
  ui::InspectorSnapshot snapshot;
  snapshot.plan.threadId = "stable-inspector";
  snapshot.plan.threadPresent = true;
  snapshot.plan.plan = ui::InspectorPlan{
      "Stable explanation",
      {{"first stable step", "in_progress"},
       {"second stable step", "pending"}}};
  snapshot.agents.threadId = "stable-inspector";
  snapshot.agents.threadPresent = true;
  snapshot.requests.requests = {
      {"request-one", "command-approval", "stable-inspector", 1,
       "one", {}, {}, std::nullopt, true},
      {"request-two", "command-approval", "stable-inspector", 1,
       "two", {}, {}, std::nullopt, true}};

  middle::InspectorPane pane;
  pane.resize(440, 700);
  pane.show();
  pane.refresh(snapshot);
  QCoreApplication::processEvents();
  auto planFrames = pane.findChildren<QFrame *>(
      QStringLiteral("inspectorPlanStepFrame"));
  QFrame *firstPlan = nullptr;
  QFrame *secondPlan = nullptr;
  for (QFrame *frame : planFrames) {
    if (frame->property("planStep").toString() ==
        QStringLiteral("first stable step"))
      firstPlan = frame;
    if (frame->property("planStep").toString() ==
        QStringLiteral("second stable step"))
      secondPlan = frame;
  }
  const qulonglong planConstructions =
      pane.property("planRowConstructions").toULongLong();
  snapshot.plan.plan->steps.front().status = "completed";
  pane.refresh(snapshot);
  QCoreApplication::processEvents();
  planFrames = pane.findChildren<QFrame *>(
      QStringLiteral("inspectorPlanStepFrame"));
  bool result = expect(
      planFrames.contains(firstPlan) && planFrames.contains(secondPlan) &&
          pane.property("planRowConstructions").toULongLong() ==
              planConstructions,
      "a Plan status update retains both stable rows and patches only the "
      "changed presentation");

  pane.tabs()->setCurrentIndex(3);
  QCoreApplication::processEvents();
  auto requestFrames = pane.findChildren<QFrame *>(
      QStringLiteral("inspectorRequestFrame"));
  QFrame *requestOne = nullptr;
  QFrame *requestTwo = nullptr;
  for (QFrame *frame : requestFrames) {
    if (frame->property("requestId").toString() ==
        QStringLiteral("request-one"))
      requestOne = frame;
    if (frame->property("requestId").toString() ==
        QStringLiteral("request-two"))
      requestTwo = frame;
  }
  const qulonglong requestConstructions =
      pane.property("requestRowConstructions").toULongLong();
  snapshot.requests.requests.front().actionable = false;
  pane.refresh(snapshot);
  QCoreApplication::processEvents();
  requestFrames = pane.findChildren<QFrame *>(
      QStringLiteral("inspectorRequestFrame"));
  result &= expect(
      requestFrames.contains(requestOne) && requestFrames.contains(requestTwo) &&
          pane.property("requestRowConstructions").toULongLong() ==
              requestConstructions,
      "a Request state update retains both stable request rows without a "
      "whole-tab rebuild");
  return result;
}

bool stateAndProtocolRemainUsefulBoundedAndRedacted() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  {
    auto write = graph.write();
    nodegraph::NodeState state;
    state.status = nodegraph::NodeStatus::Running;
    state.fields = {{"name", nodegraph::Value("Visible thread")},
                    {"prompt", nodegraph::Value("must-not-leak")},
                    {"status", nodegraph::Value("running")}};
    thread = write.upsert({nodegraph::NodeKind::Thread, "state-thread"},
                          std::move(state));
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot = adapter.inspector(thread);
  bool result = expect(snapshot &&
                           snapshot->state.state.dump().find("must-not-leak") ==
                               std::string::npos &&
                           snapshot->state.state.dump().find("<redacted>") !=
                               std::string::npos,
                       "State preserves useful graph metadata without secrets");

  middle::InspectorPane pane;
  pane.resize(440, 700);
  pane.refresh(*snapshot);
  nodegraph::UiEffect diagnostic;
  diagnostic.kind = nodegraph::UiEffectKind::ProtocolDiagnostic;
  diagnostic.details = {
      {"sequence", nodegraph::Value(std::uint64_t{7})},
      {"direction", nodegraph::Value("server notification")},
      {"subject", nodegraph::Value("item/completed")},
      {"authority", nodegraph::Value("merge")},
      {"threadId", nodegraph::Value("state-thread")},
      {"correlation", nodegraph::Value("corr-7")},
      {"error", nodegraph::Value("Bearer secret-value")}};
  pane.appendProtocolDiagnostic(diagnostic);
  auto *protocol =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("protocolInfoLog"));
  result &= expect(protocol && protocol->toPlainText().isEmpty(),
                   "a hidden Inspector diagnostic performs no QWidget update");

  pane.show();
  pane.tabs()->setCurrentIndex(4);
  auto *protocolChoice =
      pane.findChild<QPushButton *>(QStringLiteral("protocolInfoChoice"));
  if (protocolChoice)
    protocolChoice->click();
  QCoreApplication::processEvents();
  result &= expect(
      protocol &&
          protocol->toPlainText().contains(QStringLiteral("item/completed")) &&
          protocol->toPlainText().contains(QStringLiteral("corr-7")) &&
          protocol->toPlainText().contains(
              QStringLiteral("<redacted sensitive text>")) &&
          !protocol->toPlainText().contains(QStringLiteral("secret-value")),
      "Protocol retains bounded metadata and redacts sensitive text");

  auto *stateChoice =
      pane.findChild<QPushButton *>(QStringLiteral("stateInfoChoice"));
  QPushButton *protocolBack = nullptr;
  for (QPushButton *button : pane.findChildren<QPushButton *>())
    if (button->text().contains(QStringLiteral("Info"))) {
      protocolBack = button;
      break;
    }
  if (protocolBack)
    protocolBack->click();
  if (stateChoice)
    stateChoice->click();
  QCoreApplication::processEvents();
  auto *state =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("stateInfoView"));
  result &= expect(state &&
                       state->toPlainText().contains(
                           QStringLiteral("sharedNodeGraph")) &&
                       state->toPlainText().contains(
                           QStringLiteral("Visible thread")) &&
                       !state->toPlainText().contains(
                           QStringLiteral("must-not-leak")),
                   "the visible State page retains graph inspection content");
  return result;
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  const bool passed =
      codexui::codex::logicalAgentsRemainDeduplicated() &&
      codexui::codex::establishedAgentsWidgetContractIsRetained() &&
      codexui::codex::planAndRequestUpdatesRetainUnaffectedRows() &&
      codexui::codex::stateAndProtocolRemainUsefulBoundedAndRedacted();
  if (passed)
    std::cout << "NodeGraph Inspector UI adapter tests passed\n";
  return passed ? 0 : 1;
}
