// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/UiViewProjection.h"

#include "codex/PresentationModel.h"
#include "codex/PresentationStatus.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace codexui::codex::ui {
namespace {

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

std::string effectivePlanStepStatus(const std::string &stepStatus,
                                    const std::string &turnStatus,
                                    const std::string &threadStatus) {
  if (!isActiveStatus(stepStatus))
    return stepStatus;
  StatusKind outcome = classifyStatus(turnStatus).kind;
  if (outcome != StatusKind::Completed && outcome != StatusKind::Failed &&
      outcome != StatusKind::Interrupted)
    outcome = classifyStatus(threadStatus).kind;
  if (outcome == StatusKind::Completed)
    return "completed";
  if (outcome == StatusKind::Failed)
    return "failed";
  if (outcome == StatusKind::Interrupted)
    return "interrupted";
  return stepStatus;
}

std::optional<ThreadListRow> projectThread(
    const PresentationModel &model, const std::string &threadId,
    const std::unordered_map<std::string, std::size_t> &pendingByThread,
    std::unordered_set<std::string> &visited) {
  if (!visited.insert(threadId).second)
    return std::nullopt;
  const ThreadPresentation *thread = model.thread(threadId);
  if (!thread)
    return std::nullopt;

  ThreadListRow row;
  row.id = thread->id;
  row.title = thread->title;
  row.cwd = thread->cwd;
  row.status = thread->status;
  row.createdAt = thread->createdAt;
  row.updatedAt = thread->updatedAt;
  row.recencyAt = thread->recencyAt;
  if (const auto pending = pendingByThread.find(threadId);
      pending != pendingByThread.end())
    row.pending = pending->second;
  row.archived = thread->archived;
  row.children.reserve(thread->childThreadOrder.size());
  for (const std::string &childId : thread->childThreadOrder) {
    if (auto child = projectThread(model, childId, pendingByThread, visited))
      row.children.push_back(std::move(*child));
  }
  return row;
}

InspectorPlanSnapshot projectPlan(const PresentationModel &model,
                                  const std::string &threadId) {
  InspectorPlanSnapshot result;
  result.threadId = threadId;
  const ThreadPresentation *thread = model.thread(threadId);
  result.threadPresent = thread != nullptr;
  if (!thread)
    return result;

  for (auto id = thread->turnOrder.rbegin(); id != thread->turnOrder.rend();
       ++id) {
    const auto turn = thread->turns.find(*id);
    if (turn == thread->turns.end())
      continue;
    if (turn->second.plan.is_object() && turn->second.plan.contains("steps")) {
      InspectorPlan plan;
      plan.explanation = stringValue(turn->second.plan, "explanation");
      for (const auto &step :
           turn->second.plan.value("steps", nlohmann::json::array())) {
        const std::string status = stringValue(step, "status");
        plan.steps.push_back(
            {stringValue(step, "step"),
             effectivePlanStepStatus(status, turn->second.status,
                                     thread->status)});
      }
      result.plan = std::move(plan);
      break;
    }
    for (auto itemId = turn->second.itemOrder.rbegin();
         itemId != turn->second.itemOrder.rend(); ++itemId) {
      const auto item = turn->second.items.find(*itemId);
      if (item != turn->second.items.end() &&
          stringValue(item->second.raw, "type") == "plan") {
        result.planItem = stringValue(item->second.raw, "text");
        break;
      }
    }
    if (result.planItem)
      break;
  }
  return result;
}

InspectorAgentsSnapshot projectAgents(const PresentationModel &model,
                                      const std::string &threadId) {
  InspectorAgentsSnapshot result;
  result.threadId = threadId;
  const ThreadPresentation *thread = model.thread(threadId);
  result.threadPresent = thread != nullptr;
  if (!thread)
    return result;

  result.agents.reserve(thread->agentOrder.size());
  for (const std::string &id : thread->agentOrder) {
    const auto agent = thread->agents.find(id);
    if (agent == thread->agents.end())
      continue;
    InspectorAgentRow row;
    row.id = id;
    row.status = agent->second.status;
    row.childThreadId = agent->second.childThreadId;
    row.agentPath = stringValue(agent->second.raw, "agentPath");
    row.tool = stringValue(agent->second.raw, "tool");
    row.model = stringValue(agent->second.raw, "model");
    row.reasoningEffort = stringValue(agent->second.raw, "reasoningEffort");
    row.prompt = stringValue(agent->second.raw, "prompt");
    row.resultText = stringValue(agent->second.raw, "resultText");
    row.senderThreadId = stringValue(agent->second.raw, "senderThreadId");
    const auto receivers = agent->second.raw.find("receiverThreadIds");
    if (receivers != agent->second.raw.end() && receivers->is_array()) {
      for (const auto &receiver : *receivers) {
        if (receiver.is_string())
          row.receiverThreadIds.push_back(receiver.get<std::string>());
      }
    }
    result.agents.push_back(std::move(row));
  }
  return result;
}

InspectorRequestsSnapshot
projectRequests(const PresentationModel &model,
                const std::function<bool(std::string_view)> &requestEligible) {
  InspectorRequestsSnapshot result;
  result.requests.reserve(model.pendingRequestCount());
  for (const auto &[id, request] : model.pendingRequestPresentations()) {
    InspectorRequestRow row;
    row.id = id;
    row.kind = request.kind;
    row.threadContext = request.threadId;
    if (const ThreadPresentation *thread = model.thread(request.threadId);
        thread && !thread->title.empty())
      row.threadContext = thread->title;
    row.generation = request.generation;
    row.command = stringValue(request.raw, "command");
    row.reason = stringValue(request.raw, "reason");
    row.message = stringValue(request.raw, "message");
    const auto questions = request.raw.find("questions");
    if (questions != request.raw.end() && questions->is_array())
      row.questionCount = questions->size();
    row.actionable = requestEligible && requestEligible(id);
    result.requests.push_back(std::move(row));
  }
  return result;
}

InspectorChangesSnapshot projectChanges(const PresentationModel &model,
                                        const std::string &threadId) {
  InspectorChangesSnapshot result;
  result.threadId = threadId;
  if (const ThreadPresentation *thread = model.thread(threadId)) {
    result.cwd = thread->cwd;
    result.commandCwds = thread->commandCwds;
    result.changedPaths = thread->changedPaths;
  }
  return result;
}

InspectorStateSnapshot projectState(const PresentationModel &model,
                                    const std::string &threadId) {
  InspectorStateSnapshot result;
  nlohmann::json domains = nlohmann::json::object();
  for (const auto &[name, value] : model.globalDomains())
    domains[name] = value;
  nlohmann::json pending = nlohmann::json::object();
  for (const auto &[id, request] : model.pendingRequestPresentations())
    pending[id] = {{"category", request.kind},
                   {"threadId", request.threadId},
                   {"generation", request.generation}};
  result.state = {{"models", model.modelCatalog()},
                  {"pendingRequests", std::move(pending)},
                  {"domains", std::move(domains)}};
  result.threadCount = model.threadOrder().size();
  result.modelCount = model.modelCatalog().size();
  result.pendingRequestCount = model.pendingRequestCount();
  result.telemetryCount = model.telemetry().size();
  if (const ThreadPresentation *thread = model.thread(threadId)) {
    result.selectedThreadTurnCount = thread->turnOrder.size();
    for (const auto &[id, turn] : thread->turns) {
      static_cast<void>(id);
      result.selectedThreadItemCount += turn.itemOrder.size();
    }
  }
  return result;
}

} // namespace

ThreadListSnapshot projectThreadListSnapshot(const PresentationModel &model,
                                             std::string selectedThreadId) {
  ThreadListSnapshot result;
  result.selectedThreadId = std::move(selectedThreadId);
  const ConnectionPresentation &connection = model.connection();
  result.providerReady =
      connection.connected && connection.providerState == "ready";
  result.canControl = result.providerReady && connection.role == "controller";

  std::unordered_map<std::string, std::size_t> pendingByThread;
  pendingByThread.reserve(model.pendingRequestCount());
  for (const auto &[id, request] : model.pendingRequestPresentations()) {
    static_cast<void>(id);
    ++pendingByThread[request.threadId];
  }

  std::unordered_set<std::string> visited;
  visited.reserve(model.threadOrder().size());
  result.roots.reserve(model.threadOrder().size());
  for (const std::string &id : model.threadOrder()) {
    if (auto row = projectThread(model, id, pendingByThread, visited))
      result.roots.push_back(std::move(*row));
  }
  return result;
}

InspectorSnapshot projectInspectorSnapshot(
    const PresentationModel &model, std::string selectedThreadId,
    const std::function<bool(std::string_view)> &requestEligible) {
  InspectorSnapshot result;
  result.plan = projectPlan(model, selectedThreadId);
  result.agents = projectAgents(model, selectedThreadId);
  result.changes = projectChanges(model, selectedThreadId);
  result.requests = projectRequests(model, requestEligible);
  result.state = projectState(model, selectedThreadId);
  return result;
}

} // namespace codexui::codex::ui
