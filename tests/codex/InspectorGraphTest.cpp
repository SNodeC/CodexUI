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

nodegraph::UiEffect protocolDiagnostic(std::uint64_t sequence,
                                       std::string direction,
                                       std::string subject,
                                       std::string authority) {
  return {nodegraph::UiEffectKind::ProtocolDiagnostic,
          std::nullopt,
          {},
          {{"sequence", nodegraph::Value(sequence)},
           {"connectionGeneration", nodegraph::Value(std::uint64_t{3})},
           {"providerGeneration", nodegraph::Value(std::uint64_t{7})},
           {"direction", nodegraph::Value(std::move(direction))},
           {"subject", nodegraph::Value(std::move(subject))},
           {"source", nodegraph::Value("app-server")},
           {"authority", nodegraph::Value(std::move(authority))}}};
}

bool directGraphRenderingIsLazyAndCurrent() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef agentItem;
  nodegraph::NodeRef fileChangeItem;
  nodegraph::NodeRef interaction;
  nodegraph::NodeRef pendingStatisticInteraction;
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

    nodegraph::NodeState configurationState;
    configurationState.fields = {
        {"safeMode", nodegraph::Value("workspace-write")},
        {"apiToken", nodegraph::Value("sk-state-secret")},
        {"failed:sk-secret-object-key", nodegraph::Value("safe value")},
        {"environment",
         nodegraph::Value(
             nodegraph::Value::Array{nodegraph::Value(nodegraph::Value::Object{
                 {"name", nodegraph::Value("OPENAI_API_KEY")},
                 {"value", nodegraph::Value("sk-nested-secret")}})})}};
    static_cast<void>(
        write.upsert({nodegraph::NodeKind::Configuration, "config"},
                     std::move(configurationState)));

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
               {"message",
                nodegraph::Value("Deferred request " + std::to_string(index))},
               {"threadId",
                nodegraph::Value(index == 0 ? "failed:sk-thread-secret"
                                            : "thread-graph")}})}};
      const nodegraph::NodeRef extraInteraction =
          write.upsert({nodegraph::NodeKind::Interaction,
                        index == 0 ? "string:failed:sk-state-request-secret"
                                   : "zz-request-" + std::to_string(index)},
                       std::move(extraInteractionState));
      if (index == 0)
        pendingStatisticInteraction = extraInteraction;
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
          state->toPlainText().contains(QStringLiteral("Graph thread")) &&
          state->toPlainText().contains(QStringLiteral("workspace-write")) &&
          state->toPlainText().contains(
              QStringLiteral("Pending interactions (metadata only)")) &&
          !state->toPlainText().contains(QStringLiteral("sk-state-secret")) &&
          !state->toPlainText().contains(QStringLiteral("sk-nested-secret")) &&
          !state->toPlainText().contains(
              QStringLiteral("sk-state-request-secret")) &&
          !state->toPlainText().contains(QStringLiteral("sk-thread-secret")) &&
          !state->toPlainText().contains(
              QStringLiteral("sk-secret-object-key")) &&
          state->toPlainText().contains(
              QStringLiteral("<redacted identifier>")),
      "State preserves useful current graph inspection while recursively "
      "redacting secrets");

  auto *stack = pane.findChild<QStackedWidget *>(QStringLiteral("infoStack"));
  if (stack)
    stack->setCurrentIndex(0);
  auto *protocolChoice =
      pane.findChild<QPushButton *>(QStringLiteral("protocolInfoChoice"));
  nodegraph::UiEffect request =
      protocolDiagnostic(1, "client request", "thread/read", "none");
  request.details.emplace("threadId", nodegraph::Value("thread-graph"));
  request.details.emplace("correlation", nodegraph::Value("request-17"));
  pane.appendProtocolDiagnostic(request);
  nodegraph::UiEffect response =
      protocolDiagnostic(2, "client result", "thread/read", "replace");
  response.details.emplace("threadId", nodegraph::Value("thread-graph"));
  response.details.emplace("correlation", nodegraph::Value("request-17"));
  response.details.emplace("outcome", nodegraph::Value("ok"));
  pane.appendProtocolDiagnostic(response);
  nodegraph::UiEffect failure =
      protocolDiagnostic(4, "client error", "thread/name/set", "none");
  failure.details.emplace("outcome", nodegraph::Value("ERROR"));
  failure.details.emplace("errorCategory", nodegraph::Value("json-rpc"));
  failure.details.emplace("errorCode", nodegraph::Value("-32001"));
  failure.details.emplace("error", nodegraph::Value("rename rejected"));
  pane.appendProtocolDiagnostic(failure);
  for (std::uint64_t sequence = 5; sequence != 90; ++sequence)
    pane.appendProtocolDiagnostic(protocolDiagnostic(
        sequence, "server notification", "turn/outputText/delta", "merge"));
  pane.appendProtocolDiagnostic(
      protocolDiagnostic(90, "server notification", "skills/changed", "none"));
  if (protocolChoice)
    protocolChoice->click();
  spin(20);
  auto *protocol =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("protocolInfoLog"));
  auto *protocolStats =
      pane.findChild<QLabel *>(QStringLiteral("protocolInfoStats"));
  const QString protocolText = protocol ? protocol->toPlainText() : QString{};
  result &= expect(
      protocolText.indexOf(QStringLiteral("client request")) <
              protocolText.indexOf(QStringLiteral("client result")) &&
          protocolText.contains(QStringLiteral("#1")) &&
          protocolText.contains(QStringLiteral("g3")) &&
          protocolText.contains(QStringLiteral("p7")) &&
          protocolText.contains(QStringLiteral("authority=replace")) &&
          protocolText.contains(QStringLiteral("threadId=thread-graph")) &&
          protocolText.contains(QStringLiteral("correlation=request-17")) &&
          protocolText.contains(QStringLiteral("SEQUENCE GAP")) &&
          protocolText.contains(QStringLiteral("error=rename rejected")) &&
          protocolText.contains(QStringLiteral("error-code=-32001")) &&
          !protocolText.contains(QStringLiteral("legacy.raw.frame")) &&
          !protocolText.contains(QStringLiteral("payload-must-not-render")) &&
          !protocolText.contains(QStringLiteral("unknown-payload")),
      "Protocol preserves bounded chronological metadata, semantic scope, "
      "correlation, gaps, and errors without raw payloads");
  result &= expect(
      protocolStats && protocolStats->text().contains(QStringLiteral(
                           "threads 3  |  models 0  |  turns 1  |  items "
                           "32  |  pending 96  |  telemetry 1")),
      "Protocol statistics preserve global threads/models and selected-thread "
      "turn/item plus reverse-request counts");
  nodegraph::GraphChange resolvedStatistic;
  {
    auto write = graph.write();
    write.setStatus(pendingStatisticInteraction,
                    nodegraph::NodeStatus::Completed);
    resolvedStatistic = write.finish();
  }
  pane.graphChanged(notification(std::move(resolvedStatistic)));
  spin(40);
  result &= expect(protocolStats && protocolStats->text().contains(
                                        QStringLiteral("pending 95")),
                   "Protocol pending statistics refresh when a reverse "
                   "interaction resolves");
  auto *authority =
      pane.findChild<QLabel *>(QStringLiteral("protocolInfoAuthority"));
  result &= expect(authority && authority->text().contains(
                                    QStringLiteral("Non-authoritative")),
                   "Protocol identifies its history as non-authoritative");
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

bool boundedProtocolHistoryAndGraphScanStayResponsive() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  nodegraph::NodeRef revisedOperation;
  nodegraph::NodeRef firstUnknown;
  nodegraph::NodeRef churnNode;
  nodegraph::NodeRef movedSelectedItem;
  nodegraph::NodeRef selectedTurn;
  nodegraph::NodeRef unrelatedTurn;
  {
    auto write = graph.write();
    thread = write.upsert({nodegraph::NodeKind::Thread, "bounded-thread"});
    const nodegraph::NodeRef unrelatedThread =
        write.upsert({nodegraph::NodeKind::Thread, "bounded-unrelated"});
    const nodegraph::NodeRef firstTurn =
        write.upsert({nodegraph::NodeKind::Turn, "bounded-turn-1"});
    selectedTurn = write.upsert({nodegraph::NodeKind::Turn, "bounded-turn-2"});
    unrelatedTurn =
        write.upsert({nodegraph::NodeKind::Turn, "bounded-other-turn"});
    write.setParent(thread, firstTurn);
    write.setParent(thread, selectedTurn);
    write.setParent(unrelatedThread, unrelatedTurn);
    for (int index = 0; index < 80; ++index) {
      nodegraph::NodeState itemState;
      itemState.fields = {
          {"protocolThreadId", nodegraph::Value("bounded-thread")}};
      const nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item, "bounded-item:" + std::to_string(index)},
          std::move(itemState));
      write.setParent(index % 2 == 0 ? firstTurn : selectedTurn, item);
      if (index == 0)
        movedSelectedItem = item;
    }
    for (int index = 0; index < 16; ++index) {
      nodegraph::NodeState unknownState;
      unknownState.fields = {
          {"method",
           nodegraph::Value("unknown/stress/" + std::to_string(index))}};
      const nodegraph::NodeRef unknown =
          write.upsert({nodegraph::NodeKind::UnknownProtocol,
                        "unknown-stress:" + std::to_string(index)},
                       std::move(unknownState));
      if (index == 0)
        firstUnknown = unknown;
    }
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
  auto *statistics =
      pane.findChild<QLabel *>(QStringLiteral("protocolInfoStats"));
  auto *authority =
      pane.findChild<QLabel *>(QStringLiteral("protocolInfoAuthority"));

  runOneQueuedPass();
  bool result = expect(protocol && statistics && statistics->text().isEmpty(),
                       "a large Protocol scan is bounded to more than one "
                       "Qt event-loop pass");

  nodegraph::GraphChange reparented;
  {
    auto write = graph.write();
    write.setParent(unrelatedTurn, movedSelectedItem);
    write.setField(movedSelectedItem, "protocolThreadId",
                   nodegraph::Value("bounded-unrelated"));
    reparented = write.finish();
  }
  pane.graphChanged(notification(std::move(reparented)));

  nodegraph::GraphChange shiftedOrder;
  {
    auto write = graph.write();
    write.remove(firstUnknown);
    nodegraph::NodeState replacementState;
    replacementState.fields = {
        {"method", nodegraph::Value("unknown/stress/replacement")}};
    static_cast<void>(write.upsert(
        {nodegraph::NodeKind::UnknownProtocol, "unknown-stress:replacement"},
        std::move(replacementState)));
    nodegraph::NodeState churnState;
    churnState.fields = {{"method", nodegraph::Value("unrelated/churn")}};
    churnNode =
        write.upsert({nodegraph::NodeKind::Operation, "structural-churn"},
                     std::move(churnState));
    shiftedOrder = write.finish();
  }
  pane.graphChanged(notification(std::move(shiftedOrder)));

  bool completedDuringRevisions = false;
  for (int revision = 0; revision != 100 && !completedDuringRevisions;
       ++revision) {
    nodegraph::GraphChange changed;
    {
      auto write = graph.write();
      write.remove(churnNode);
      nodegraph::NodeState churnState;
      churnState.fields = {
          {"method", nodegraph::Value("unknown/stress/churn/" +
                                      std::to_string(revision))}};
      churnNode = write.upsert({nodegraph::NodeKind::Operation,
                                "structural-churn:" + std::to_string(revision)},
                               std::move(churnState));
      write.setField(revisedOperation, "scanPulse",
                     nodegraph::Value(std::uint64_t(revision)));
      changed = write.finish();
    }
    pane.graphChanged(notification(std::move(changed)));
    runOneQueuedPass();
    completedDuringRevisions = statistics && !statistics->text().isEmpty();
  }
  result &= expect(completedDuringRevisions,
                   "continuous unrelated structural and field revisions do "
                   "not starve a bounded Protocol statistics scan");
  result &= expect(
      statistics &&
          statistics->text().contains(QStringLiteral("turns 2  |  items 79")) &&
          statistics->text().contains(QStringLiteral("unknown 16")),
      "an immutable insertion frontier neither skips nor duplicates surviving "
      "nodes, and selected relation changes restart before publishing mixed "
      "counts");
  spin(120);
  const qulonglong completedScans =
      pane.property("protocolScanCompletions").toULongLong();
  for (int revision = 100; revision != 200; ++revision) {
    nodegraph::GraphChange changed;
    {
      auto write = graph.write();
      write.setField(revisedOperation, "scanPulse",
                     nodegraph::Value(std::uint64_t(revision)));
      changed = write.finish();
    }
    pane.graphChanged(notification(std::move(changed)));
    runOneQueuedPass();
  }
  spin(30);
  result &= expect(
      pane.property("protocolScanCompletions").toULongLong() == completedScans,
      "unrelated field streaming stops after the productive bounded scan "
      "instead of sustaining a zero-delay rescan loop");

  for (std::uint64_t sequence = 1; sequence <= 2050; ++sequence) {
    nodegraph::UiEffect effect = protocolDiagnostic(
        sequence, "server notification",
        "protocol/bounded/" + std::to_string(sequence), "merge");
    if (sequence == 2049)
      effect.details.emplace("droppedBefore",
                             nodegraph::Value(std::uint64_t{3}));
    pane.appendProtocolDiagnostic(effect);
  }
  spin(20);
  const QString boundedText = protocol ? protocol->toPlainText() : QString{};
  result &= expect(
      protocol && protocol->document()->blockCount() <= 2000 &&
          !boundedText.contains(QStringLiteral("protocol/bounded/1  ")) &&
          boundedText.contains(QStringLiteral("protocol/bounded/2050")) &&
          boundedText.contains(QStringLiteral("DROPPED 3 DIAGNOSTICS")) &&
          authority &&
          authority->text().contains(QStringLiteral("metadata-only")),
      "Protocol retains only its newest 2000 metadata lines and keeps the "
      "non-authoritative header outside that bound");

  auto *stateChoice =
      pane.findChild<QPushButton *>(QStringLiteral("stateInfoChoice"));
  if (stateChoice)
    stateChoice->click();
  const qulonglong initialStateCompletions =
      pane.property("stateScanCompletions").toULongLong();
  runOneQueuedPass();
  nodegraph::GraphChange stateReparented;
  {
    auto write = graph.write();
    write.setParent(selectedTurn, movedSelectedItem);
    write.setField(movedSelectedItem, "protocolThreadId",
                   nodegraph::Value("bounded-thread"));
    stateReparented = write.finish();
  }
  pane.graphChanged(notification(std::move(stateReparented)));
  bool stateCompletedDuringStructuralChurn = false;
  for (int revision = 200;
       revision != 300 && !stateCompletedDuringStructuralChurn; ++revision) {
    nodegraph::GraphChange changed;
    {
      auto write = graph.write();
      write.remove(churnNode);
      nodegraph::NodeState churnState;
      churnState.fields = {
          {"method", nodegraph::Value("unknown/state-churn/" +
                                      std::to_string(revision))}};
      churnNode =
          write.upsert({nodegraph::NodeKind::Operation,
                        "structural-state-churn:" + std::to_string(revision)},
                       std::move(churnState));
      changed = write.finish();
    }
    pane.graphChanged(notification(std::move(changed)));
    runOneQueuedPass();
    stateCompletedDuringStructuralChurn =
        pane.property("stateScanCompletions").toULongLong() >
        initialStateCompletions;
  }
  result &= expect(stateCompletedDuringStructuralChurn,
                   "continuous unrelated insert/remove activity cannot "
                   "starve the bounded State scan");
  auto *state =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("stateInfoView"));
  result &=
      expect(state && state->toPlainText().contains(
                          QStringLiteral("turns: 2\n  items: 80")),
             "State restarts a selected hierarchy scan before publishing mixed "
             "item totals");
  if (protocolChoice)
    protocolChoice->click();
  spin(20);

  const QString currentText = protocol ? protocol->toPlainText() : QString{};

  pane.hide();
  pane.appendProtocolDiagnostic(protocolDiagnostic(
      2051, "server notification", "protocol/updated-while-hidden", "none"));
  spin(30);
  result &= expect(protocol && protocol->toPlainText() == currentText,
                   "a hidden Inspector records diagnostics without QWidget "
                   "projection");

  pane.show();
  spin(120);
  const QString shownText = protocol ? protocol->toPlainText() : QString{};
  result &= expect(
      shownText.contains(QStringLiteral("protocol/updated-while-hidden")) &&
          !shownText.contains(QStringLiteral("protocol/bounded/1  ")),
      "showing Protocol resynchronizes the bounded chronological tail");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  const bool passed =
      codexui::codex::middle::directGraphRenderingIsLazyAndCurrent() &&
      codexui::codex::middle::
          boundedProtocolHistoryAndGraphScanStayResponsive();
  if (passed)
    std::cout << "Inspector graph tests passed\n";
  return passed ? 0 : 1;
}
