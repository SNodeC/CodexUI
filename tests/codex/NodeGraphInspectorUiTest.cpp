// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/InspectorPane.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/NodeGraphUiAdapter.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

#include <git2.h>

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

template <typename Predicate>
bool waitFor(Predicate &&predicate, int timeoutMilliseconds = 3000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeoutMilliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return predicate();
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
  const auto agentsOnly =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  result &= expect(
      agentsOnly && agentsOnly->agents.agents.size() == 2 &&
          !agentsOnly->plan.plan && !agentsOnly->plan.planItem &&
          agentsOnly->state.state.empty() &&
          agentsOnly->requests.requests.empty(),
      "the active Agents projection does not construct State, Plan, or "
      "Requests presentation data");
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

bool changesTabShowsCanonicalThreadRepositoryChanges() {
  QTemporaryDir repositoryDirectory;
  if (!expect(repositoryDirectory.isValid(),
              "creates a repository for the Inspector Changes proof"))
    return false;

  git_repository *repository = nullptr;
  if (!expect(git_repository_init(
                  &repository,
                  repositoryDirectory.path().toUtf8().constData(), 0) == 0,
              "initializes the Inspector Changes proof repository"))
    return false;

  const QString changedPath =
      repositoryDirectory.filePath(QStringLiteral("changed.txt"));
  QFile changedFile(changedPath);
  const QByteArray contents("visible Inspector change\n");
  const bool wrote = changedFile.open(QIODevice::WriteOnly) &&
                     changedFile.write(contents) == contents.size();
  changedFile.close();
  if (!expect(wrote, "creates the real untracked Inspector change")) {
    git_repository_free(repository);
    return false;
  }

  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  const QString parentWorkspace =
      QFileInfo(repositoryDirectory.path()).absolutePath();
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields = {
        {"id", nodegraph::Value("changes-thread")},
        {"cwd", nodegraph::Value(parentWorkspace.toStdString())}};
    thread = write.upsert({nodegraph::NodeKind::Thread, "changes-thread"},
                          std::move(threadState));
    const nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "changes-turn"});
    nodegraph::NodeState fileChanges;
    fileChanges.fields = {
        {"type", nodegraph::Value("fileChange")},
        {"cwd", nodegraph::Value(repositoryDirectory.path().toStdString())},
        {"changes",
         nodegraph::Value(nodegraph::Value::Array{
             nodegraph::Value(nodegraph::Value::Object{
                 {"path", nodegraph::Value("changed.txt")},
                 {"kind", nodegraph::Value("add")}})})}};
    const nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "changes-item"}, std::move(fileChanges));
    write.setParent(thread, turn);
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot =
      adapter.inspector(thread, ui::InspectorProjection::Changes);
  bool result = expect(
      snapshot && snapshot->changes.cwd == parentWorkspace.toStdString() &&
          snapshot->changes.commandCwds ==
              std::vector<std::string>{repositoryDirectory.path().toStdString()} &&
          snapshot->changes.changedPaths ==
              std::vector<std::string>{"changed.txt"},
      "Changes projection retains the parent workspace, item repository, and "
      "changed path");

  middle::InspectorPane pane;
  pane.resize(520, 720);
  pane.show();
  pane.tabs()->setCurrentIndex(2);
  if (snapshot)
    pane.refresh(*snapshot, ui::InspectorProjection::Changes);
  auto *files =
      pane.findChild<QListWidget *>(QStringLiteral("codexDiffFiles"));
  const bool visible = waitFor([&] {
    return files && files->count() == 1 &&
           files->item(0)->text().contains(QStringLiteral("changed.txt"));
  });
  result &= expect(
      visible,
      "the visible Inspector Changes tab renders the real changed file");
  git_repository_free(repository);
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
      codexui::codex::changesTabShowsCanonicalThreadRepositoryChanges() &&
      codexui::codex::stateAndProtocolRemainUsefulBoundedAndRedacted();
  if (passed)
    std::cout << "NodeGraph Inspector UI adapter tests passed\n";
  return passed ? 0 : 1;
}
