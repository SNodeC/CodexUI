// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/DiffViewer.h"
#include "codex/middle/InspectorPane.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QThread>
#include <QTimer>

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

QLabel *labelContaining(const QWidget &root, const QString &text) {
  for (QLabel *label : root.findChildren<QLabel *>()) {
    if (label->text().contains(text))
      return label;
  }
  return nullptr;
}

void runOneQueuedPass() {
  QEventLoop loop;
  QTimer::singleShot(0, &loop, &QEventLoop::quit);
  loop.exec();
}

nodegraph::GraphChanged notification(nodegraph::GraphChange change) {
  return {change.revision, std::move(change.affected),
          std::move(change.removed), false};
}

bool directGraphRenderingIsLazyAndCurrent() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef agentItem;
  nodegraph::NodeRef fileChangeItem;
  nodegraph::NodeRef interaction;
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.status = nodegraph::NodeStatus::Running;
    threadState.fields = {{"name", nodegraph::Value("Graph thread")},
                          {"cwd", nodegraph::Value("/workspace/graph")}};
    thread = write.upsert({nodegraph::NodeKind::Thread, "thread-graph"},
                          std::move(threadState));

    nodegraph::Value::Array planSteps;
    planSteps.reserve(96);
    for (int index = 0; index < 96; ++index) {
      planSteps.emplace_back(nodegraph::Value::Object{
          {"step", nodegraph::Value("Plan step " + std::to_string(index))},
          {"status", nodegraph::Value(index == 0 ? "pending" : "completed")}});
    }
    nodegraph::NodeState turnState;
    turnState.status = nodegraph::NodeStatus::Running;
    turnState.fields = {
        {"planExplanation", nodegraph::Value("Graph-only explanation")},
        {"plan", nodegraph::Value(std::move(planSteps))}};
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
    agentItem = write.upsert({nodegraph::NodeKind::Item, "agent-item"},
                             std::move(agentState));
    write.setParent(turn, agentItem);
    for (int index = 0; index < 30; ++index) {
      nodegraph::NodeState extraAgentState;
      extraAgentState.status = nodegraph::NodeStatus::Completed;
      extraAgentState.fields = {
          {"type", nodegraph::Value("subAgentActivity")},
          {"agentPath",
           nodegraph::Value("/root/batched_agent_" + std::to_string(index))}};
      const nodegraph::NodeRef extraAgent = write.upsert(
          {nodegraph::NodeKind::Item, "batched-agent-" + std::to_string(index)},
          std::move(extraAgentState));
      write.setParent(turn, extraAgent);
    }
    nodegraph::NodeState childState;
    childState.status = nodegraph::NodeStatus::Completed;
    const nodegraph::NodeRef child = write.upsert(
        {nodegraph::NodeKind::Thread, "agent-thread"}, std::move(childState));
    write.relate(agentItem, nodegraph::RelationKind::AgentChildThread, child);

    nodegraph::NodeState connectionState;
    connectionState.status = nodegraph::NodeStatus::Connected;
    connectionState.fields = {
        {"transportState", nodegraph::Value("connected")},
        {"providerState", nodegraph::Value("ready")},
        {"role", nodegraph::Value("controller")},
        {"providerGeneration", nodegraph::Value(std::uint64_t{7})}};
    static_cast<void>(
        write.upsert({nodegraph::NodeKind::Connection, "connection"},
                     std::move(connectionState)));

    nodegraph::NodeState interactionState;
    interactionState.status = nodegraph::NodeStatus::Pending;
    interactionState.fields = {
        {"method", nodegraph::Value("item/tool/requestUserInput")},
        {"payload", nodegraph::Value(nodegraph::Value::Object{
                        {"message", nodegraph::Value("Choose from the graph")},
                        {"questions", nodegraph::Value(nodegraph::Value::Array{
                                          nodegraph::Value("first"),
                                          nodegraph::Value("second")})}})}};
    nodegraph::NodeState fileChangeState;
    fileChangeState.status = nodegraph::NodeStatus::Completed;
    fileChangeState.fields = {
        {"type", nodegraph::Value("fileChange")},
        {"changes",
         nodegraph::Value(
             nodegraph::Value::Array{nodegraph::Value(nodegraph::Value::Object{
                 {"path", nodegraph::Value("src/original.cpp")}})})}};
    fileChangeItem =
        write.upsert({nodegraph::NodeKind::Item, "file-change-item"},
                     std::move(fileChangeState));
    write.setParent(turn, fileChangeItem);

    interaction =
        write.upsert({nodegraph::NodeKind::Interaction, "string:request-graph"},
                     std::move(interactionState));
    write.relate(interaction, nodegraph::RelationKind::InteractionTarget,
                 thread);
    const nodegraph::NodeRef runtime =
        write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
    write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                 interaction);
    write.relate(thread, nodegraph::RelationKind::PendingInteraction,
                 interaction);
    for (int index = 0; index < 96; ++index) {
      nodegraph::NodeState extraInteractionState;
      extraInteractionState.status = nodegraph::NodeStatus::Pending;
      extraInteractionState.fields = {
          {"method", nodegraph::Value("item/tool/requestUserInput")},
          {"requestId",
           nodegraph::Value("bulk-request-" + std::to_string(index))},
          {"payload",
           nodegraph::Value(nodegraph::Value::Object{
               {"message", nodegraph::Value("Deferred request " +
                                            std::to_string(index))}})}};
      const nodegraph::NodeRef extraInteraction =
          write.upsert({nodegraph::NodeKind::Interaction,
                        "zz-request-" + std::to_string(index)},
                       std::move(extraInteractionState));
      write.relate(extraInteraction, nodegraph::RelationKind::InteractionTarget,
                   thread);
      write.relate(runtime, nodegraph::RelationKind::PendingInteraction,
                   extraInteraction);
      write.relate(thread, nodegraph::RelationKind::PendingInteraction,
                   extraInteraction);
    }

    nodegraph::NodeState operationState;
    operationState.status = nodegraph::NodeStatus::Pending;
    operationState.fields = {
        {"method", nodegraph::Value("thread/read")},
        {"requestPayload",
         nodegraph::Value(nodegraph::Value::Object{
             {"private", nodegraph::Value("payload-must-not-render")}})}};
    static_cast<void>(
        write.upsert({nodegraph::NodeKind::Operation, "string:operation-graph"},
                     std::move(operationState)));
    for (int index = 0; index < 96; ++index) {
      nodegraph::NodeState extraOperationState;
      extraOperationState.status = nodegraph::NodeStatus::Pending;
      extraOperationState.fields = {
          {"method",
           nodegraph::Value("fixture/protocol/" + std::to_string(index))}};
      static_cast<void>(
          write.upsert({nodegraph::NodeKind::Operation,
                        "fixture-operation:" + std::to_string(index)},
                       std::move(extraOperationState)));
    }

    nodegraph::NodeState unknownState;
    unknownState.fields = {
        {"method", nodegraph::Value("future/method")},
        {"direction", nodegraph::Value(std::uint64_t{2})},
        {"payload", nodegraph::Value(nodegraph::Value::Object{
                        {"private", nodegraph::Value("unknown-payload")}})}};
    static_cast<void>(
        write.upsert({nodegraph::NodeKind::UnknownProtocol, "2:future/method"},
                     std::move(unknownState)));
    static_cast<void>(write.finish());
  }

  InspectorPane pane;
  pane.resize(440, 700);
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
          hasLabelContaining(pane, QStringLiteral("Plan step 0")),
      "Plan reads the selected thread after a contended graph pass retries");
  const qsizetype initialPlanRows =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorPlanStep")).size();
  auto *planViewport = qobject_cast<QScrollArea *>(pane.tabs()->widget(0));
  result &= expect(initialPlanRows > 0 && initialPlanRows < 96 &&
                       initialPlanRows <= 48 && planViewport &&
                       planViewport->verticalScrollBar()->maximum() > 0,
                   "Plan materializes only a bounded visible row window");
  if (planViewport) {
    planViewport->verticalScrollBar()->setValue(
        planViewport->verticalScrollBar()->maximum());
    spin(60);
    result &= expect(
        hasLabelContaining(pane, QStringLiteral("Plan step 95")) &&
            pane.findChildren<QFrame *>(QStringLiteral("inspectorPlanStep"))
                    .size() <= 48,
        "scrolling Plan replaces the bounded window with the latest rows");
    planViewport->verticalScrollBar()->setValue(0);
    spin(30);
  }
  result &= expect(!pane.findChild<QLabel *>(QStringLiteral("agentTitle")),
                   "an invisible Agents tab performs no widget projection");
  result &= expect(thread->uiAttachment() == nullptr,
                   "the composite Inspector does not claim node attachment");

  QPointer<QLabel> stablePlan =
      labelContaining(pane, QStringLiteral("Graph-only explanation"));
  nodegraph::NodeRef unrelatedItem;
  nodegraph::GraphChange unrelatedChange;
  {
    auto write = graph.write();
    const nodegraph::NodeRef unrelatedThread =
        write.upsert({nodegraph::NodeKind::Thread, "unrelated-thread"});
    const nodegraph::NodeRef unrelatedTurn =
        write.upsert({nodegraph::NodeKind::Turn, "unrelated-turn"});
    write.setParent(unrelatedThread, unrelatedTurn);
    nodegraph::NodeState unrelatedState;
    unrelatedState.fields = {{"type", nodegraph::Value("agentMessage")},
                             {"text", nodegraph::Value("unrelated")}};
    unrelatedItem = write.upsert({nodegraph::NodeKind::Item, "unrelated-item"},
                                 std::move(unrelatedState));
    write.setParent(unrelatedTurn, unrelatedItem);
    unrelatedChange = write.finish();
  }
  pane.graphChanged(notification(std::move(unrelatedChange)));
  spin(10);
  result &= expect(
      stablePlan &&
          stablePlan ==
              labelContaining(pane, QStringLiteral("Graph-only explanation")),
      "an unrelated thread update preserves Plan widgets");

  pane.tabs()->setCurrentIndex(1);
  runOneQueuedPass();
  runOneQueuedPass();
  const qsizetype firstAgentSlice =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame")).size();
  result &= expect(firstAgentSlice > 0 && firstAgentSlice <= 12,
                   "Agents bounds widget construction per Qt pass");
  spin(80);
  result &= expect(
      hasLabelContaining(pane, QStringLiteral("graph_agent")) &&
          hasLabelContaining(pane, QStringLiteral("completed")),
      "Agents reads current source and child-thread facts on visibility");
  const qsizetype materializedAgents =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame")).size();
  auto *agentsViewport = qobject_cast<QScrollArea *>(pane.tabs()->widget(1));
  result &= expect(materializedAgents > 0 && materializedAgents < 31 &&
                       materializedAgents <= 48 && agentsViewport &&
                       agentsViewport->verticalScrollBar()->maximum() > 0,
                   "Agents retains only its bounded visible row window");
  if (agentsViewport) {
    agentsViewport->verticalScrollBar()->setValue(
        agentsViewport->verticalScrollBar()->maximum());
    spin(40);
    result &= expect(
        hasLabelContaining(pane, QStringLiteral("batched_agent_29")) &&
            pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
                    .size() <= 48,
        "scrolling Agents replaces rather than accumulates materialized rows");
    agentsViewport->verticalScrollBar()->setValue(0);
    spin(40);
  }
  QPointer<QFrame> stableAgent =
      pane.findChild<QFrame *>(QStringLiteral("inspectorAgentFrame"));
  nodegraph::GraphChange agentIrrelevantChange;
  {
    auto write = graph.write();
    write.setField(turn, "planExplanation",
                   nodegraph::Value("Changed outside the Agents projection"));
    agentIrrelevantChange = write.finish();
  }
  pane.graphChanged(notification(std::move(agentIrrelevantChange)));
  spin(20);
  result &= expect(
      stableAgent && stableAgent == pane.findChild<QFrame *>(
                                        QStringLiteral("inspectorAgentFrame")),
      "a revision-irrelevant selected-thread change preserves Agent cards");

  pane.tabs()->setCurrentIndex(2);
  spin(160);
  auto *diffViewer = static_cast<DiffViewer *>(pane.tabs()->widget(2));
  QTimer *diffRefresh = nullptr;
  if (diffViewer) {
    for (QTimer *timer : diffViewer->findChildren<QTimer *>(
             QString{}, Qt::FindDirectChildrenOnly)) {
      if (timer->isSingleShot()) {
        diffRefresh = timer;
        break;
      }
    }
  }
  result &=
      expect(diffRefresh && !diffRefresh->isActive(),
             "the initial Changes refresh settles before relevance checks");
  nodegraph::GraphChange changesIrrelevantChange;
  {
    auto write = graph.write();
    write.setField(agentItem, "prompt",
                   "Inspect the shared graph but not Git context");
    changesIrrelevantChange = write.finish();
  }
  pane.graphChanged(notification(std::move(changesIrrelevantChange)));
  spin(10);
  result &=
      expect(diffRefresh && !diffRefresh->isActive(),
             "an unrelated item delta does not launch a Git diff refresh");
  nodegraph::GraphChange changesRelevantChange;
  {
    auto write = graph.write();
    write.setField(fileChangeItem, "changes",
                   nodegraph::Value(nodegraph::Value::Array{
                       nodegraph::Value(nodegraph::Value::Object{
                           {"path", nodegraph::Value("src/updated.cpp")}})}));
    changesRelevantChange = write.finish();
  }
  pane.graphChanged(notification(std::move(changesRelevantChange)));
  spin(10);
  result &= expect(diffRefresh && diffRefresh->isActive(),
                   "a changed file hint launches one deferred Git refresh");
  spin(130);

  pane.tabs()->setCurrentIndex(3);
  runOneQueuedPass();
  result &= expect(
      pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"))
          .empty(),
      "a large Requests scan yields before publishing a partial revision");
  int requestHeartbeats = 0;
  QTimer requestHeartbeat;
  requestHeartbeat.setInterval(0);
  QObject::connect(&requestHeartbeat, &QTimer::timeout,
                   [&requestHeartbeats] { ++requestHeartbeats; });
  requestHeartbeat.start();
  nodegraph::GraphChange revisedRequest;
  {
    auto write = graph.write();
    write.setField(
        interaction, "payload",
        nodegraph::Value(nodegraph::Value::Object{
            {"message", nodegraph::Value("Choose after request restart")},
            {"questions",
             nodegraph::Value(nodegraph::Value::Array{
                 nodegraph::Value("first"), nodegraph::Value("second")})}}));
    revisedRequest = write.finish();
  }
  pane.graphChanged(notification(std::move(revisedRequest)));
  spin(40);
  requestHeartbeat.stop();
  result &= expect(requestHeartbeats > 2,
                   "bounded Requests extraction yields to the Qt heartbeat");
  result &= expect(
      hasLabelContaining(pane,
                         QStringLiteral("Choose after request restart")) &&
          hasLabelContaining(pane, QStringLiteral("2 questions")) &&
          hasLabelContaining(pane, QStringLiteral("thread Graph thread")),
      "Requests restarts a partial scan and renders only the current graph "
      "revision");
  const qsizetype materializedRequests =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"))
          .size();
  auto *requestsViewport = qobject_cast<QScrollArea *>(pane.tabs()->widget(3));
  result &= expect(materializedRequests > 0 && materializedRequests < 97 &&
                       materializedRequests <= 48 && requestsViewport &&
                       requestsViewport->verticalScrollBar()->maximum() > 0,
                   "Requests retains only its bounded visible row window");
  if (requestsViewport) {
    requestsViewport->verticalScrollBar()->setValue(
        requestsViewport->verticalScrollBar()->maximum());
    spin(40);
    result &= expect(
        hasLabelContaining(pane, QStringLiteral("Deferred request 95")) &&
            pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"))
                    .size() <= 48,
        "scrolling Requests replaces rather than accumulates request cards");
    requestsViewport->verticalScrollBar()->setValue(0);
    spin(40);
  }
  QPointer<QFrame> pendingRequest =
      pane.findChild<QFrame *>(QStringLiteral("inspectorRequestFrame"));
  const bool hadPendingRequest = pendingRequest;
  nodegraph::GraphChange failedChange;
  {
    auto write = graph.write();
    write.setStatus(interaction, nodegraph::NodeStatus::Failed);
    write.setField(interaction, "error",
                   nodegraph::Value("the response was rejected"));
    failedChange = write.finish();
  }
  pane.graphChanged(notification(std::move(failedChange)));
  spin(20);
  auto *failedRequest =
      pane.findChild<QFrame *>(QStringLiteral("inspectorRequestFrame"));
  auto *failedReject = failedRequest
                           ? failedRequest->findChild<QPushButton *>(
                                 QString{}, Qt::FindChildrenRecursively)
                           : nullptr;
  result &= expect(
      hadPendingRequest && failedRequest &&
          hasLabelContaining(*failedRequest,
                             QStringLiteral("Choose after request restart")) &&
          failedReject && failedReject->isEnabled(),
      "a failed interaction remains visible for deliberate "
      "response recovery");

  pane.tabs()->setCurrentIndex(4);
  auto *stateChoice =
      pane.findChild<QPushButton *>(QStringLiteral("stateInfoChoice"));
  if (stateChoice)
    stateChoice->click();
  spin(20);
  auto *state =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("stateInfoView"));
  result &= expect(
      state &&
          state->toPlainText().contains(QStringLiteral("Shared NodeGraph")) &&
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
      "Protocol replaces raw history with current operation/unknown "
      "diagnostics");
  result &= expect(protocol && protocol->verticalScrollBar()->maximum() > 0 &&
                       protocol->verticalScrollBar()->value() ==
                           protocol->verticalScrollBar()->maximum(),
                   "the first current-protocol render follows the log tail");

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

bool boundedProtocolScanYieldsRestartsAndSleepsWhileHidden() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef revisedOperation;
  {
    auto write = graph.write();
    thread = write.upsert({nodegraph::NodeKind::Thread, "bounded-thread"});
    for (int index = 0; index < 4096; ++index) {
      nodegraph::NodeState state;
      state.status = nodegraph::NodeStatus::Pending;
      state.fields = {
          {"method", nodegraph::Value(index == 0 ? "protocol/stale-partial"
                                                 : "protocol/stress/" +
                                                       std::to_string(index))}};
      const nodegraph::NodeRef operation =
          write.upsert({nodegraph::NodeKind::Operation,
                        "bounded-operation:" + std::to_string(index)},
                       std::move(state));
      if (index == 0)
        revisedOperation = operation;
    }
    static_cast<void>(write.finish());
  }

  InspectorPane pane;
  pane.resize(440, 700);
  pane.show();
  pane.refresh(graph, thread);
  pane.tabs()->setCurrentIndex(4);
  auto *protocolChoice =
      pane.findChild<QPushButton *>(QStringLiteral("protocolInfoChoice"));
  if (protocolChoice)
    protocolChoice->click();
  auto *protocol =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("protocolInfoLog"));

  runOneQueuedPass();
  bool result = expect(protocol && protocol->toPlainText().isEmpty(),
                       "a large Protocol scan is bounded to more than one "
                       "Qt event-loop pass");

  int heartbeats = 0;
  QTimer heartbeat;
  heartbeat.setInterval(0);
  QObject::connect(&heartbeat, &QTimer::timeout,
                   [&heartbeats] { ++heartbeats; });
  heartbeat.start();

  nodegraph::GraphChange revised;
  {
    auto write = graph.write();
    write.setField(revisedOperation, "method",
                   nodegraph::Value("protocol/revision-current"));
    revised = write.finish();
  }
  pane.graphChanged(notification(std::move(revised)));
  spin(120);
  heartbeat.stop();
  const QString currentText = protocol ? protocol->toPlainText() : QString{};
  result &= expect(heartbeats > 2,
                   "bounded Protocol extraction yields to the Qt heartbeat");
  result &= expect(
      currentText.contains(QStringLiteral("protocol/revision-current")) &&
          !currentText.contains(QStringLiteral("protocol/stale-partial")),
      "a revision change discards partial Protocol scan state before render");

  pane.hide();
  nodegraph::GraphChange hiddenChange;
  {
    auto write = graph.write();
    write.setField(revisedOperation, "method",
                   nodegraph::Value("protocol/updated-while-hidden"));
    hiddenChange = write.finish();
  }
  pane.graphChanged(notification(std::move(hiddenChange)));
  spin(30);
  result &= expect(protocol && protocol->toPlainText() == currentText,
                   "a hidden Inspector retains only dirty state and performs "
                   "no Protocol projection");

  pane.show();
  spin(120);
  const QString shownText = protocol ? protocol->toPlainText() : QString{};
  result &= expect(
      shownText.contains(QStringLiteral("protocol/updated-while-hidden")) &&
          !shownText.contains(QStringLiteral("protocol/revision-current")),
      "showing a dirty Inspector renders exactly the newest graph revision");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  const bool passed =
      codexui::codex::middle::directGraphRenderingIsLazyAndCurrent() &&
      codexui::codex::middle::
          boundedProtocolScanYieldsRestartsAndSleepsWhileHidden();
  if (passed)
    std::cout << "Inspector graph tests passed\n";
  return passed ? 0 : 1;
}
