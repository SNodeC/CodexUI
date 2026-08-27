// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PresentationModel.h"

#include "codex/PresentationProtocol.h"
#include "codex/PresentationStatus.h"

#include <algorithm>

namespace codexui::codex {
namespace {

constexpr std::size_t MaximumRetainedTelemetry = 256;
constexpr std::size_t MaximumIndexedTextParts = 4096;

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto iterator = object.find(key);
  return iterator != object.end() && iterator->is_string()
             ? iterator->get<std::string>()
             : std::string{};
}

nlohmann::json memberValue(const nlohmann::json &object, const char *key,
                           nlohmann::json fallback = nullptr) {
  if (!object.is_object())
    return fallback;
  const auto iterator = object.find(key);
  return iterator == object.end() ? std::move(fallback) : *iterator;
}

bool boolValue(const nlohmann::json &object, const char *key,
               bool fallback = false) {
  if (!object.is_object())
    return fallback;
  const auto iterator = object.find(key);
  return iterator != object.end() && iterator->is_boolean()
             ? iterator->get<bool>()
             : fallback;
}

void updateTimestamp(const nlohmann::json &object, const char *key,
                     std::optional<std::int64_t> &target) {
  if (!object.is_object())
    return;
  const auto iterator = object.find(key);
  if (iterator != object.end() && iterator->is_number_integer())
    target = iterator->get<std::int64_t>();
}

std::string statusValue(const nlohmann::json &value) {
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_object())
    return stringValue(value, "type");
  return {};
}

std::string requestKey(const nlohmann::json &value) {
  return value.is_null() ? std::string{} : value.dump();
}

void appendUnique(std::vector<std::string> &values, const std::string &value,
                  std::size_t maximum) {
  if (value.empty() ||
      std::find(values.begin(), values.end(), value) != values.end())
    return;
  if (values.size() == maximum)
    values.erase(values.begin());
  values.push_back(value);
}

void retainRepositoryHints(ThreadPresentation &thread,
                           const nlohmann::json &item) {
  const std::string type = stringValue(item, "type");
  if (type == "commandExecution")
    appendUnique(thread.commandCwds, stringValue(item, "cwd"), 64);
  if (type != "fileChange")
    return;
  const auto changes = item.find("changes");
  if (changes == item.end() || !changes->is_array())
    return;
  for (const auto &change : *changes)
    appendUnique(thread.changedPaths, stringValue(change, "path"), 512);
}

bool isSpawnActivity(const nlohmann::json &activity) {
  const std::string type = stringValue(activity, "type");
  if (type == "subAgentActivity")
    return true;
  if (type != "collabAgentToolCall")
    return false;
  const std::string tool = stringValue(activity, "tool");
  return tool == "spawn_agent" || tool == "spawnAgent" ||
         tool == "spawn_agents_on_csv" || tool == "spawnAgentsOnCsv";
}

std::string childThreadIdentity(const nlohmann::json &activity) {
  const std::string childThreadId = stringValue(activity, "agentThreadId");
  if (!childThreadId.empty())
    return childThreadId;
  const nlohmann::json receivers =
      memberValue(activity, "receiverThreadIds", nlohmann::json::array());
  if (receivers.is_array() && receivers.size() == 1 &&
      receivers.front().is_string())
    return receivers.front().get<std::string>();
  return {};
}

std::string agentIdentity(const nlohmann::json &activity,
                          const nlohmann::json &scope) {
  const std::string childThreadId = childThreadIdentity(activity);
  if (!childThreadId.empty())
    return childThreadId;
  const std::string itemId = stringValue(scope, "itemId");
  return itemId.empty() ? stringValue(activity, "id") : itemId;
}

void mergePreservingCompleteness(nlohmann::json &target,
                                 const nlohmann::json &update) {
  if (!target.is_object() || !update.is_object()) {
    if (!update.is_null() || target.is_null())
      target = update;
    return;
  }
  for (const auto &[key, value] : update.items()) {
    auto current = target.find(key);
    if (current == target.end()) {
      target[key] = value;
    } else if (current->is_object() && value.is_object()) {
      mergePreservingCompleteness(*current, value);
    } else if (!value.is_null() || current->is_null()) {
      *current = value;
    }
  }
}

void mergeExplicitMembers(nlohmann::json &target,
                          const nlohmann::json &update) {
  if (!target.is_object() || !update.is_object()) {
    target = update;
    return;
  }
  for (const auto &[key, value] : update.items()) {
    auto current = target.find(key);
    if (current != target.end() && current->is_object() && value.is_object())
      mergeExplicitMembers(*current, value);
    else
      target[key] = value;
  }
}

void appendText(nlohmann::json &item, const char *field,
                const nlohmann::json &params) {
  const std::string delta = stringValue(params, "delta");
  if (delta.empty())
    return;
  nlohmann::json &stored = item[field];
  if (!stored.is_string())
    stored = "";
  std::string &existing = stored.get_ref<std::string &>();
  existing += delta;
}

void appendIndexedText(nlohmann::json &item, const char *field,
                       const nlohmann::json &params, const char *indexField) {
  const auto index = params.find(indexField);
  const bool hasIndex = index != params.end() && index->is_number_integer() &&
                        index->get<std::int64_t>() >= 0;
  const std::size_t position = hasIndex ? index->get<std::size_t>() : 0;
  if (position >= MaximumIndexedTextParts)
    return;
  nlohmann::json &parts = item[field];
  if (!parts.is_array())
    parts = nlohmann::json::array();
  while (parts.size() <= position)
    parts.push_back("");
  if (!parts[position].is_string())
    parts[position] = "";
  std::string delta = stringValue(params, "delta");
  if (delta.empty())
    delta = stringValue(params, "text");
  std::string &existing = parts[position].get_ref<std::string &>();
  existing += delta;
}

void applyDomainAuthority(
    std::unordered_map<std::string, nlohmann::json> &domains,
    const std::string &type, const nlohmann::json &data,
    const std::string &authority) {
  if (authority == "none")
    return;
  if (authority == "remove") {
    domains.erase(type);
    return;
  }
  if (authority == "replace" || !domains.contains(type)) {
    domains[type] = data;
    return;
  }
  if (type == "thread.settings.changed") {
    // A null setting explicitly restores the app-server default. Preserve it
    // instead of treating it as an incomplete presentation update.
    mergeExplicitMembers(domains[type], data);
    return;
  }
  mergePreservingCompleteness(domains[type], data);
}

} // namespace

void PresentationModel::applyEvent(const nlohmann::json &event) noexcept {
  try {
    applyValidatedEvent(event);
  } catch (...) {
    // Presentation mutation is an untrusted-data boundary. No malformed event
    // may escape through Qt dispatch.
  }
}

void PresentationModel::applyValidatedEvent(const nlohmann::json &event) {
  if (!presentation::isPresentationFrame(event))
    return;

  const std::string kind = presentation::stringMember(event, "kind");
  const auto generationMember = event.find("generation");
  const std::uint64_t generation =
      generationMember != event.end() && generationMember->is_number_unsigned()
          ? generationMember->get<std::uint64_t>()
          : 0;
  if (connectionState.generation != 0 && generation != 0 &&
      generation < connectionState.generation)
    return;
  if (generation > connectionState.generation) {
    connectionState.generation = generation;
    lastSequence = 0;
    pendingRequests.clear();
  }
  const auto sequenceMember = event.find("sequence");
  const std::uint64_t sequence =
      sequenceMember != event.end() && sequenceMember->is_number_unsigned()
          ? sequenceMember->get<std::uint64_t>()
          : 0;
  if (sequence != 0) {
    if (sequence <= lastSequence)
      return;
    lastSequence = sequence;
  }
  const nlohmann::json data =
      presentation::member(event, "data", nlohmann::json::object());
  const nlohmann::json scope =
      presentation::member(event, "scope", nlohmann::json::object());
  if (kind == "result") {
    if (!boolValue(event, "ok"))
      return;
    const std::string action = presentation::stringMember(event, "action");
    if (action == "threads.list") {
      const nlohmann::json threads =
          memberValue(data, "threads", nlohmann::json::array());
      mergeThreadList(threads);
    } else if (action == "thread.read") {
      const nlohmann::json thread =
          memberValue(data, "thread", nlohmann::json::object());
      ThreadPresentation &hydrated =
          upsertThread(thread, stringValue(event, "authority") == "replace");
      correlateAgentThread(hydrated.id);
    } else if (action == "thread.create" || action == "thread.resume" ||
               action == "thread.fork") {
      upsertThread(memberValue(data, "thread", nlohmann::json::object()),
                   false);
    } else if (action == "turn.start") {
      const std::string threadId = stringValue(scope, "threadId");
      const auto thread = threads.find(threadId);
      if (thread != threads.end()) {
        TurnPresentation &turn = upsertTurn(
            thread->second,
            memberValue(data, "turn", nlohmann::json::object()), false);
        if (isActiveStatus(turn.status))
          thread->second.status = "active";
      }
    } else if (action == "models.list") {
      const nlohmann::json listedModels =
          memberValue(data, "models", nlohmann::json::array());
      if (listedModels.is_array())
        models = listedModels;
    } else {
      retainDomainEvent("operation." + action, data, scope,
                        presentation::stringMember(event, "authority"));
    }
    return;
  }

  if (kind != "event")
    return;

  const std::string type = presentation::stringMember(event, "type");
  if (presentation::stringMember(event, "authority") == "none") {
    if (retainedTelemetry.size() == MaximumRetainedTelemetry)
      retainedTelemetry.erase(retainedTelemetry.begin());
    retainedTelemetry.push_back(TelemetryPresentation{
        sequence, generation, type, data, scope});
  }
  if (type == "connection.lifecycle") {
    connectionState.generation =
        generation;
    const std::string lifecycle = stringValue(data, "state");
    if (lifecycle == "connected") {
      connectionState.connected = true;
      connectionState.retrying = false;
      connectionState.detail.clear();
    } else if (lifecycle == "connecting" || lifecycle == "retrying") {
      connectionState.connected = false;
      connectionState.retrying = true;
      connectionState.connectionId.clear();
      connectionState.role.clear();
      connectionState.controllerConnectionId.clear();
      connectionState.detail = stringValue(data, "detail");
      pendingRequests.clear();
    } else if (lifecycle == "disconnected" || lifecycle == "failure") {
      connectionState.connected = false;
      connectionState.retrying = false;
      connectionState.connectionId.clear();
      connectionState.role.clear();
      connectionState.controllerConnectionId.clear();
      connectionState.detail = stringValue(data, "detail");
      pendingRequests.clear();
    }
    return;
  }
  if (type == "connection.bridge") {
    connectionState.connectionId = stringValue(data, "connectionId");
    connectionState.role = stringValue(data, "role");
    return;
  }
  if (type == "connection.controller") {
    connectionState.controllerConnectionId =
        stringValue(data, "controllerConnectionId");
    if (!connectionState.connectionId.empty())
      connectionState.role =
          connectionState.controllerConnectionId == connectionState.connectionId
              ? "controller"
              : "observer";
    return;
  }
  if (type == "connection.provider") {
    const auto providerGeneration = data.find("generation");
    if (providerGeneration == data.end() ||
        !providerGeneration->is_number_unsigned())
      return;
    const std::uint64_t incoming = providerGeneration->get<std::uint64_t>();
    if (incoming < connectionState.providerGeneration)
      return;
    const std::string state = stringValue(data, "state");
    if ((connectionState.providerGeneration != 0 &&
         incoming > connectionState.providerGeneration) ||
        state == "disconnected")
      clearProviderState();
    connectionState.providerGeneration = incoming;
    connectionState.providerState = state;
    connectionState.providerDetail = stringValue(data, "reason");
    return;
  }
  if (type == "connection.settings.changed") {
    connectionState.settings = data;
    return;
  }
  if (type == "thread.upsert") {
    upsertThread(memberValue(data, "thread", nlohmann::json::object()), false);
    return;
  }
  if (type == "thread.name.changed") {
    const auto thread = threads.find(stringValue(scope, "threadId"));
    if (thread != threads.end() && data.contains("name") &&
        data["name"].is_string()) {
      thread->second.title = data["name"].get<std::string>();
      thread->second.raw["name"] = data["name"];
    }
    return;
  }
  if (type == "thread.status.changed") {
    const auto thread = threads.find(stringValue(scope, "threadId"));
    if (thread != threads.end()) {
      thread->second.status = statusValue(memberValue(data, "status"));
      thread->second.raw["status"] = memberValue(data, "status");
      correlateAgentThread(thread->first);
    }
    return;
  }
  if (type == "thread.lifecycle") {
    const auto thread = threads.find(stringValue(scope, "threadId"));
    if (thread != threads.end()) {
      thread->second.status = stringValue(data, "state");
      const std::string lifecycle = stringValue(data, "state");
      if (lifecycle == "archived")
        thread->second.archived = true;
      else if (lifecycle == "unarchived")
        thread->second.archived = false;
      thread->second.raw["presentationLifecycle"] = lifecycle;
    }
    return;
  }
  if (type == "thread.removed") {
    removeThread(stringValue(scope, "threadId"));
    return;
  }

  if (type == "pending-request.upsert") {
    const auto id = data.find("requestId");
    if (id == data.end() || id->is_null())
      return;
    const std::string key = requestKey(*id);
    pendingRequests[key] = PendingRequestPresentation{
        key, stringValue(data, "category"), stringValue(scope, "threadId"),
        generation, memberValue(data, "request")};
    return;
  }
  if (type == "pending-request.removed") {
    const auto id = scope.find("requestId");
    if (id != scope.end())
      pendingRequests.erase(requestKey(*id));
    return;
  }

  const std::string threadId = stringValue(scope, "threadId");
  if (threadId.empty()) {
    retainDomainEvent(type, data, scope,
                      presentation::stringMember(event, "authority"));
    return;
  }
  auto threadIterator = threads.find(threadId);
  if (threadIterator == threads.end()) {
    nlohmann::json minimal{{"id", threadId}};
    upsertThread(minimal, false);
    threadIterator = threads.find(threadId);
    if (threadIterator == threads.end())
      return;
  }
  ThreadPresentation &thread = threadIterator->second;

  retainDomainEvent(type, data, scope,
                    presentation::stringMember(event, "authority"));

  if (type == "thread.settings.changed" && data.is_object()) {
    thread.latestSettingsUpdate = data.value("threadSettings", data);
    ++thread.settingsRevision;
  }

  if (type == "turn.upsert") {
    nlohmann::json turn =
        memberValue(data, "turn", nlohmann::json::object());
    const std::string lifecycle = stringValue(data, "lifecycle");
    const std::string embeddedStatus =
        statusValue(memberValue(turn, "status"));
    if (lifecycle == "completed" &&
        !isTerminalTurnStatus(embeddedStatus))
      turn["status"] = "completed";
    else if (lifecycle == "started" && embeddedStatus.empty())
      turn["status"] = "inProgress";
    TurnPresentation &updated = upsertTurn(thread, turn, false);
    if (lifecycle == "started" && isActiveStatus(updated.status))
      thread.status = "active";
    correlateAgentThread(threadId);
    return;
  }
  if (type == "plan.replaced") {
    const std::string turnId = stringValue(scope, "turnId");
    nlohmann::json minimalTurn{{"id", turnId}};
    TurnPresentation &turn = upsertTurn(thread, minimalTurn, false);
    turn.plan = {
        {"explanation", memberValue(data, "explanation")},
        {"steps", memberValue(data, "steps", nlohmann::json::array())}};
    return;
  }
  if (type == "conversation.item.upsert") {
    const std::string turnId = stringValue(scope, "turnId");
    if (data.contains("item")) {
      nlohmann::json minimalTurn{{"id", turnId}};
      TurnPresentation &turn = upsertTurn(thread, minimalTurn, false);
      upsertItem(thread, turn, data["item"], true);
      correlateAgentThread(threadId);
    }
    return;
  }
  if (type == "agents.activity.upsert") {
    upsertAgentActivity(
        thread, scope,
        memberValue(data, "activity", nlohmann::json::object()));
    return;
  }
  if (type == "conversation.reasoning.part-added") {
    if (ItemPresentation *item = findItem(scope)) {
      const auto index = data.find("summaryIndex");
      if (index != data.end() && index->is_number_integer() &&
          index->get<std::int64_t>() >= 0 &&
          index->get<std::size_t>() < MaximumIndexedTextParts) {
        nlohmann::json &parts = item->raw["summary"];
        if (!parts.is_array())
          parts = nlohmann::json::array();
        while (parts.size() <= index->get<std::size_t>())
          parts.push_back("");
      }
    }
    return;
  }
  if (type == "conversation.file-change.output-appended") {
    if (ItemPresentation *item = findItem(scope)) {
      nlohmann::json delta{{"delta", stringValue(data, "delta")}};
      appendText(item->raw, "output", delta);
    }
    return;
  }
  if (type == "conversation.file-change.patch-replaced") {
    if (ItemPresentation *item = findItem(scope)) {
      item->raw["changes"] =
          memberValue(data, "changes", nlohmann::json::array());
      retainRepositoryHints(thread, item->raw);
    }
    return;
  }
  if (type == "conversation.mcp.progress") {
    if (ItemPresentation *item = findItem(scope)) {
      nlohmann::json &progress = item->raw["progress"];
      if (!progress.is_array())
        progress = nlohmann::json::array();
      if (progress.size() < MaximumIndexedTextParts)
        progress.push_back(stringValue(data, "message"));
    }
    return;
  }
  if (type != "conversation.item.append")
    return;

  nlohmann::json identity = scope;
  identity["delta"] = stringValue(data, "text");
  ItemPresentation *item = findItem(identity);
  if (!item)
    return;
  const std::string field = stringValue(data, "field");
  if (field == "summary")
    appendIndexedText(item->raw, "summary", data, "summaryIndex");
  else if (field == "content")
    appendIndexedText(item->raw, "content", data, "contentIndex");
  else if (!field.empty())
    appendText(item->raw, field.c_str(), identity);
}

const std::vector<std::string> &
PresentationModel::threadOrder() const noexcept {
  return orderedThreads;
}

const ThreadPresentation *
PresentationModel::thread(const std::string &threadId) const noexcept {
  const auto iterator = threads.find(threadId);
  return iterator == threads.end() ? nullptr : &iterator->second;
}

std::optional<std::string>
PresentationModel::activeTurnId(const std::string &threadId) const {
  const ThreadPresentation *value = thread(threadId);
  if (!value)
    return std::nullopt;
  if (!value->status.empty() && !isActiveStatus(value->status))
    return std::nullopt;
  for (auto iterator = value->turnOrder.rbegin();
       iterator != value->turnOrder.rend(); ++iterator) {
    const auto turn = value->turns.find(*iterator);
    if (turn != value->turns.end() && isActiveStatus(turn->second.status))
      return turn->first;
  }
  return std::nullopt;
}

std::size_t PresentationModel::pendingRequestCount() const noexcept {
  return pendingRequests.size();
}

std::size_t PresentationModel::pendingRequestCount(
    const std::string &threadId) const noexcept {
  return static_cast<std::size_t>(
      std::count_if(pendingRequests.begin(), pendingRequests.end(),
                    [&threadId](const auto &entry) {
                      return entry.second.threadId == threadId;
                    }));
}

const ConnectionPresentation &PresentationModel::connection() const noexcept {
  return connectionState;
}

const nlohmann::json &PresentationModel::modelCatalog() const noexcept {
  return models;
}

const std::unordered_map<std::string, nlohmann::json> &
PresentationModel::globalDomains() const noexcept {
  return retainedGlobalDomains;
}

const std::vector<TelemetryPresentation> &
PresentationModel::telemetry() const noexcept {
  return retainedTelemetry;
}

const std::unordered_map<std::string, PendingRequestPresentation> &
PresentationModel::pendingRequestPresentations() const noexcept {
  return pendingRequests;
}

void PresentationModel::mergeThreadList(const nlohmann::json &listedThreads) {
  if (!listedThreads.is_array())
    return;

  std::vector<std::string> listedIds;
  listedIds.reserve(listedThreads.size());
  for (const auto &raw : listedThreads) {
    const std::string id = stringValue(raw, "id");
    if (id.empty())
      continue;
    upsertThread(raw, false);
    listedIds.push_back(id);
  }

  for (const std::string &id : listedIds)
    std::erase(orderedThreads, id);
  orderedThreads.insert(orderedThreads.begin(), listedIds.begin(),
                        listedIds.end());
}

ThreadPresentation &PresentationModel::upsertThread(const nlohmann::json &raw,
                                                    bool replaceTurns) {
  const std::string id = stringValue(raw, "id");
  if (id.empty()) {
    static ThreadPresentation ignored;
    return ignored;
  }
  auto [iterator, inserted] = threads.try_emplace(id);
  ThreadPresentation &result = iterator->second;
  if (inserted) {
    result.id = id;
    orderedThreads.insert(orderedThreads.begin(), id);
  }
  const std::string previousThreadStatus = result.status;
  std::unordered_map<std::string, std::string> terminalTurnStatuses;
  if (replaceTurns) {
    for (const auto &[turnId, turn] : result.turns) {
      if (isTerminalTurnStatus(turn.status))
        terminalTurnStatuses.emplace(turnId, turn.status);
    }
  }
  nlohmann::json threadFields = raw;
  threadFields.erase("turns");
  if (replaceTurns)
    result.raw = std::move(threadFields);
  else
    mergePreservingCompleteness(result.raw, threadFields);
  const std::string name = stringValue(raw, "name");
  const std::string preview = stringValue(raw, "preview");
  if (!name.empty())
    result.title = name;
  else if (!preview.empty())
    result.title = preview.substr(0, 80);
  else if (result.title.empty())
    result.title = id.empty() ? "Untitled thread" : id.substr(0, 12);
  if (!preview.empty())
    result.preview = preview;
  const std::string cwd = stringValue(raw, "cwd");
  if (!cwd.empty())
    result.cwd = cwd;
  const auto status = raw.find("status");
  if (status != raw.end())
    result.status = statusValue(*status);
  updateTimestamp(raw, "createdAt", result.createdAt);
  updateTimestamp(raw, "updatedAt", result.updatedAt);
  updateTimestamp(raw, "recencyAt", result.recencyAt);
  result.archived = boolValue(raw, "archived", result.archived);

  const auto turns = raw.find("turns");
  if (turns != raw.end() && turns->is_array()) {
    if (replaceTurns) {
      result.turnOrder.clear();
      result.turns.clear();
      result.agentOrder.clear();
      result.agents.clear();
      result.commandCwds.clear();
      result.changedPaths.clear();
    }
    for (const auto &turn : *turns)
      upsertTurn(result, turn, replaceTurns);
    if (replaceTurns) {
      for (const auto &[turnId, terminalStatus] : terminalTurnStatuses) {
        const auto turn = result.turns.find(turnId);
        if (turn != result.turns.end() && isActiveStatus(turn->second.status)) {
          turn->second.status = terminalStatus;
          turn->second.raw["status"] = terminalStatus;
        }
      }
    }
    const bool containsActiveTurn =
        std::ranges::any_of(result.turns, [](const auto &entry) {
          return isActiveStatus(entry.second.status);
        });
    if (!containsActiveTurn && isActiveStatus(result.status) &&
        classifyStatus(previousThreadStatus).kind == StatusKind::Completed) {
      result.status = previousThreadStatus;
      result.raw["status"] = previousThreadStatus;
    }
  }
  return result;
}

TurnPresentation &PresentationModel::upsertTurn(ThreadPresentation &thread,
                                                const nlohmann::json &raw,
                                                bool replaceItems) {
  const std::string id = stringValue(raw, "id");
  if (id.empty()) {
    static TurnPresentation ignored;
    return ignored;
  }
  auto [iterator, inserted] = thread.turns.try_emplace(id);
  TurnPresentation &result = iterator->second;
  if (inserted) {
    result.id = id;
    thread.turnOrder.push_back(id);
  }
  nlohmann::json turnFields = raw;
  turnFields.erase("items");
  if (replaceItems)
    result.raw = std::move(turnFields);
  else
    mergePreservingCompleteness(result.raw, turnFields);
  const std::string status = statusValue(memberValue(raw, "status"));
  if (!status.empty() &&
      !(isTerminalTurnStatus(result.status) && isActiveStatus(status)))
    result.status = status;
  if (isTerminalTurnStatus(result.status) && isActiveStatus(status))
    result.raw["status"] = result.status;
  const auto items = raw.find("items");
  if (items != raw.end() && items->is_array()) {
    if (replaceItems) {
      result.itemOrder.clear();
      result.items.clear();
    }
    for (const auto &item : *items)
      upsertItem(thread, result, item);
  }
  return result;
}

ItemPresentation &PresentationModel::upsertItem(ThreadPresentation &thread,
                                                TurnPresentation &turn,
                                                const nlohmann::json &raw,
                                                bool live) {
  const std::string id = stringValue(raw, "id");
  if (id.empty()) {
    static ItemPresentation ignored;
    return ignored;
  }
  auto [iterator, inserted] = turn.items.try_emplace(id);
  ItemPresentation &result = iterator->second;
  if (inserted) {
    result.id = id;
    result.raw = raw;
    turn.itemOrder.push_back(id);
  } else {
    mergePreservingCompleteness(result.raw, raw);
  }
  const std::string type = stringValue(result.raw, "type");
  retainRepositoryHints(thread, result.raw);
  if (type == "subAgentActivity" || type == "collabAgentToolCall") {
    upsertAgentActivity(
        thread,
        {{"threadId", thread.id}, {"turnId", turn.id}, {"itemId", result.id}},
        result.raw, live);
  }
  return result;
}

void PresentationModel::upsertAgentActivity(ThreadPresentation &owner,
                                            const nlohmann::json &scope,
                                            const nlohmann::json &activity,
                                            bool live) {
  const std::string type = stringValue(activity, "type");
  if (type == "collabAgentToolCall" && !isSpawnActivity(activity)) {
    const nlohmann::json states =
        memberValue(activity, "agentsStates", nlohmann::json::object());
    if (!states.is_object())
      return;
    for (const auto &[childThreadId, state] : states.items()) {
      const auto existing = owner.agents.find(childThreadId);
      if (existing == owner.agents.end() || !state.is_object())
        continue;
      const std::string status = stringValue(state, "status");
      const std::string message = stringValue(state, "message");
      if (!status.empty())
        existing->second.status = status;
      if (!message.empty())
        existing->second.raw["resultText"] = message;
      existing->second.raw["agentState"] = state;
      correlateAgentThread(childThreadId);
    }
    return;
  }

  const std::string childThreadId = childThreadIdentity(activity);
  if (type == "collabAgentToolCall" && childThreadId.empty())
    return;

  const std::string id = agentIdentity(activity, scope);
  if (id.empty())
    return;

  auto [iterator, inserted] = owner.agents.try_emplace(id);
  AgentPresentation &agent = iterator->second;
  if (inserted) {
    agent.id = id;
    owner.agentOrder.push_back(id);
  }
  agent.itemId = stringValue(scope, "itemId");
  agent.ownerTurnId = stringValue(scope, "turnId");
  mergePreservingCompleteness(agent.raw, activity);

  if (!childThreadId.empty())
    agent.childThreadId = childThreadId;

  const std::string activityStatus = stringValue(activity, "status");
  const std::string activityKind = stringValue(activity, "kind");
  if (!activityStatus.empty())
    agent.status = activityStatus;
  else if (live && activityKind == "started")
    agent.status = "inProgress";
  else if (!activityKind.empty())
    agent.status = activityKind;

  if (!agent.childThreadId.empty()) {
    auto [child, childInserted] = threads.try_emplace(agent.childThreadId);
    if (childInserted)
      child->second.id = agent.childThreadId;
    child->second.agentThread = true;
    std::erase(orderedThreads, agent.childThreadId);
    correlateAgentThread(agent.childThreadId);
  }
}

void PresentationModel::correlateAgentThread(const std::string &childThreadId) {
  const auto child = threads.find(childThreadId);
  if (child == threads.end())
    return;

  std::string childStatus = child->second.status;
  std::string resultText;
  for (const std::string &turnId : child->second.turnOrder) {
    const auto turn = child->second.turns.find(turnId);
    if (turn == child->second.turns.end())
      continue;
    if (!turn->second.status.empty())
      childStatus = turn->second.status;
    for (const std::string &itemId : turn->second.itemOrder) {
      const auto item = turn->second.items.find(itemId);
      if (item == turn->second.items.end() ||
          stringValue(item->second.raw, "type") != "agentMessage")
        continue;
      const std::string text = stringValue(item->second.raw, "text");
      if (!text.empty())
        resultText = text;
    }
  }

  for (auto &[ownerId, owner] : threads) {
    static_cast<void>(ownerId);
    for (auto &[agentId, agent] : owner.agents) {
      static_cast<void>(agentId);
      if (agent.childThreadId != childThreadId)
        continue;
      if (!childStatus.empty())
        agent.status = childStatus;
      if (!resultText.empty())
        agent.raw["resultText"] = resultText;
      agent.raw["childThreadId"] = childThreadId;
    }
  }
}

void PresentationModel::removeThread(const std::string &threadId) {
  threads.erase(threadId);
  std::erase(orderedThreads, threadId);
}

void PresentationModel::clearProviderState() {
  orderedThreads.clear();
  threads.clear();
  pendingRequests.clear();
  models = nlohmann::json::array();
  retainedGlobalDomains.clear();
}

void PresentationModel::retainDomainEvent(const std::string &type,
                                          const nlohmann::json &data,
                                          const nlohmann::json &scope,
                                          const std::string &authority) {
  const std::string threadId = stringValue(scope, "threadId");
  const std::string turnId = stringValue(scope, "turnId");
  const std::string itemId = stringValue(scope, "itemId");
  if (!itemId.empty()) {
    if (ItemPresentation *item = findItem(scope))
      applyDomainAuthority(item->domains, type, data, authority);
    return;
  }
  if (!turnId.empty()) {
    if (TurnPresentation *turn = findTurn(threadId, turnId))
      applyDomainAuthority(turn->domains, type, data, authority);
    return;
  }
  if (!threadId.empty()) {
    const auto thread = threads.find(threadId);
    if (thread != threads.end())
      applyDomainAuthority(thread->second.domains, type, data, authority);
    return;
  }
  applyDomainAuthority(retainedGlobalDomains, type, data, authority);
}

TurnPresentation *PresentationModel::findTurn(const std::string &threadId,
                                              const std::string &turnId) {
  auto thread = threads.find(threadId);
  if (thread == threads.end())
    return nullptr;
  auto turn = thread->second.turns.find(turnId);
  return turn == thread->second.turns.end() ? nullptr : &turn->second;
}

ItemPresentation *PresentationModel::findItem(const nlohmann::json &params) {
  TurnPresentation *turn =
      findTurn(stringValue(params, "threadId"), stringValue(params, "turnId"));
  if (!turn)
    return nullptr;
  const std::string itemId = stringValue(params, "itemId");
  auto item = turn->items.find(itemId);
  return item == turn->items.end() ? nullptr : &item->second;
}

} // namespace codexui::codex
