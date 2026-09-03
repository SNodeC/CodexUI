// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QThread>

#include <iostream>
#include <string>
#include <utility>

namespace codexui::codex::middle {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

void spin(int milliseconds = 0) {
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (milliseconds > 0)
      QThread::msleep(1);
  } while (timer.elapsed() < milliseconds);
}

bool hasLabelContaining(const QWidget &root, const QString &text) {
  for (const QLabel *label : root.findChildren<QLabel *>()) {
    if (label->text().contains(text))
      return true;
  }
  return false;
}

nodegraph::GraphChanged notification(nodegraph::GraphChange change) {
  return {change.revision, std::move(change.affected),
          std::move(change.removed), false};
}

bool directGraphRenderingIsLazyAndCurrent() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.status = nodegraph::NodeStatus::Running;
    threadState.fields = {{"name", nodegraph::Value("Graph thread")},
                          {"cwd", nodegraph::Value("/workspace/graph")}};
    thread = write.upsert({nodegraph::NodeKind::Thread, "thread-graph"},
                          std::move(threadState));

    nodegraph::NodeState turnState;
    turnState.status = nodegraph::NodeStatus::Running;
    turnState.fields = {
        {"planExplanation", nodegraph::Value("Graph-only explanation")},
        {"plan",
         nodegraph::Value(nodegraph::Value::Array{nodegraph::Value(
             nodegraph::Value::Object{{"step", nodegraph::Value("Inspect")},
                                      {"status", nodegraph::Value("pending")}})})}};
    turn = write.upsert({nodegraph::NodeKind::Turn, "turn-graph"},
                        std::move(turnState));
    write.setParent(thread, turn);

    nodegraph::NodeState agentState;
    agentState.status = nodegraph::NodeStatus::Running;
    agentState.fields = {
        {"type", nodegraph::Value("subAgentActivity")},
        {"agentPath", nodegraph::Value("/root/graph_agent")},
        {"agentThreadId", nodegraph::Value("agent-thread")},
        {"prompt", nodegraph::Value("Inspect the shared graph")}};
    const nodegraph::NodeRef agentItem =
        write.upsert({nodegraph::NodeKind::Item, "agent-item"},
                     std::move(agentState));
    write.setParent(turn, agentItem);
    nodegraph::NodeState childState;
    childState.status = nodegraph::NodeStatus::Completed;
    const nodegraph::NodeRef child =
        write.upsert({nodegraph::NodeKind::Thread, "agent-thread"},
                     std::move(childState));
    write.relate(agentItem, nodegraph::RelationKind::AgentChildThread, child);

    nodegraph::NodeState connectionState;
    connectionState.status = nodegraph::NodeStatus::Connected;
    connectionState.fields = {
        {"transportState", nodegraph::Value("connected")},
        {"providerState", nodegraph::Value("ready")},
        {"role", nodegraph::Value("controller")},
        {"providerGeneration", nodegraph::Value(std::uint64_t{7})}};
    static_cast<void>(write.upsert(
        {nodegraph::NodeKind::Connection, "connection"},
        std::move(connectionState)));

    nodegraph::NodeState interactionState;
    interactionState.status = nodegraph::NodeStatus::Pending;
    interactionState.fields = {
        {"method", nodegraph::Value("item/tool/requestUserInput")},
        {"payload",
         nodegraph::Value(nodegraph::Value::Object{
             {"message", nodegraph::Value("Choose from the graph")},
             {"questions",
              nodegraph::Value(nodegraph::Value::Array{
                  nodegraph::Value("first"), nodegraph::Value("second")})}})}};
    const nodegraph::NodeRef interaction = write.upsert(
        {nodegraph::NodeKind::Interaction, "string:request-graph"},
        std::move(interactionState));
    write.relate(interaction, nodegraph::RelationKind::InteractionTarget,
                 thread);

    nodegraph::NodeState operationState;
    operationState.status = nodegraph::NodeStatus::Pending;
    operationState.fields = {
        {"method", nodegraph::Value("thread/read")},
        {"requestPayload",
         nodegraph::Value(nodegraph::Value::Object{
             {"private", nodegraph::Value("payload-must-not-render")}})}};
    static_cast<void>(write.upsert(
        {nodegraph::NodeKind::Operation, "string:operation-graph"},
        std::move(operationState)));

    nodegraph::NodeState unknownState;
    unknownState.fields = {
        {"method", nodegraph::Value("future/method")},
        {"direction", nodegraph::Value(std::uint64_t{2})},
        {"payload",
         nodegraph::Value(nodegraph::Value::Object{
             {"private", nodegraph::Value("unknown-payload")}})}};
    static_cast<void>(write.upsert(
        {nodegraph::NodeKind::UnknownProtocol,
         "2:future/method"},
        std::move(unknownState)));
    static_cast<void>(write.finish());
  }

  InspectorPane pane;
  pane.resize(440, 700);
  pane.appendProtocolFrame({{"kind", "event"},
                            {"type", "legacy.raw.frame"},
                            {"sequence", 1}});
  pane.show();

  // The graph entry point schedules a non-blocking try-read. Running its first
  // pass while a writer owns the lock must only defer the render.
  {
    auto write = graph.write();
    pane.refresh(graph, thread);
    QCoreApplication::processEvents();
    static_cast<void>(write.finish());
  }
  spin(30);

  bool result = expect(
      hasLabelContaining(pane, QStringLiteral("Graph-only explanation")) &&
          hasLabelContaining(pane, QStringLiteral("Inspect")),
      "Plan reads the selected thread after a contended graph pass retries");
  result &= expect(
      !pane.findChild<QLabel *>(QStringLiteral("agentTitle")),
      "an invisible Agents tab performs no widget projection");
  result &= expect(thread->uiAttachment() == nullptr,
                   "the composite Inspector does not claim node attachment");

  pane.tabs()->setCurrentIndex(1);
  spin(20);
  result &= expect(
      hasLabelContaining(pane, QStringLiteral("graph_agent")) &&
          hasLabelContaining(pane, QStringLiteral("completed")),
      "Agents reads current source and child-thread facts on visibility");

  pane.tabs()->setCurrentIndex(3);
  spin(20);
  result &= expect(
      hasLabelContaining(pane, QStringLiteral("Choose from the graph")) &&
          hasLabelContaining(pane, QStringLiteral("2 questions")) &&
          hasLabelContaining(pane, QStringLiteral("thread Graph thread")),
      "Requests reads current pending interactions and target relations");

  pane.tabs()->setCurrentIndex(4);
  auto *stateChoice =
      pane.findChild<QPushButton *>(QStringLiteral("stateInfoChoice"));
  if (stateChoice)
    stateChoice->click();
  spin(20);
  auto *state =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("stateInfoView"));
  result &= expect(
      state && state->toPlainText().contains(QStringLiteral("Shared NodeGraph")) &&
          state->toPlainText().contains(QStringLiteral("thread-graph")) &&
          state->toPlainText().contains(QStringLiteral("Graph thread")),
      "State renders bounded current-node diagnostics without a JSON model");

  auto *stack = pane.findChild<QStackedWidget *>(QStringLiteral("infoStack"));
  if (stack)
    stack->setCurrentIndex(0);
  auto *protocolChoice =
      pane.findChild<QPushButton *>(QStringLiteral("protocolInfoChoice"));
  if (protocolChoice)
    protocolChoice->click();
  spin(20);
  auto *protocol =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("protocolInfoLog"));
  const QString protocolText = protocol ? protocol->toPlainText() : QString{};
  result &= expect(
      protocolText.contains(QStringLiteral("thread/read")) &&
          protocolText.contains(QStringLiteral("future/method")) &&
          !protocolText.contains(QStringLiteral("legacy.raw.frame")) &&
          !protocolText.contains(QStringLiteral("payload-must-not-render")) &&
          !protocolText.contains(QStringLiteral("unknown-payload")),
      "Protocol replaces raw history with current operation/unknown diagnostics");

  nodegraph::GraphChange changed;
  {
    auto write = graph.write();
    write.setField(
        turn, "planExplanation",
        nodegraph::Value("Updated while the Plan tab was invisible"));
    changed = write.finish();
  }
  pane.graphChanged(notification(std::move(changed)));
  spin(10);
  pane.tabs()->setCurrentIndex(0);
  spin(20);
  result &= expect(
      hasLabelContaining(
          pane, QStringLiteral("Updated while the Plan tab was invisible")),
      "a newly visible tab renders the latest node revision exactly once");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  const bool passed =
      codexui::codex::middle::directGraphRenderingIsLazyAndCurrent();
  if (passed)
    std::cout << "Inspector graph tests passed\n";
  return passed ? 0 : 1;
}
