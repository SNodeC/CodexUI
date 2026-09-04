// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolUpdater.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace codexui::nodegraph {
namespace {

const Value *member(const Value::Object &object, std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

const Value::Object *objectMember(const Value::Object &object,
                                  std::string_view name) {
  const Value *value = member(object, name);
  return value ? value->asObject() : nullptr;
}

const Value::Array *arrayMember(const Value::Object &object,
                                std::string_view name) {
  const Value *value = member(object, name);
  return value ? value->asArray() : nullptr;
}

std::string canonicalValue(const Value *value) {
  if (!value)
    return {};
  if (const std::string *text = value->asString())
    return *text;
  if (const std::int64_t *number = value->asInt64())
    return std::to_string(*number);
  if (const std::uint64_t *number = value->asUInt64())
    return std::to_string(*number);
  return {};
}

std::string firstId(const Value::Object &object,
                    std::span<const std::string_view> names) {
  for (const std::string_view name : names) {
    std::string value = canonicalValue(member(object, name));
    if (!value.empty())
      return value;
  }
  return {};
}

std::string scopedCanonical(std::string_view owner,
                            std::string_view protocolId) {
  std::string result = "scope:";
  result += std::to_string(owner.size());
  result += ':';
  result += owner;
  result += ':';
  result += std::to_string(protocolId.size());
  result += ':';
  result += protocolId;
  return result;
}

NodeStatus statusFromValue(const Value *value) {
  const std::string *status = value ? value->asString() : nullptr;
  if (!status)
    return NodeStatus::Unknown;
  if (*status == "pending" || *status == "queued")
    return NodeStatus::Pending;
  if (*status == "running" || *status == "inProgress" || *status == "active")
    return NodeStatus::Running;
  if (*status == "completed" || *status == "complete" || *status == "succeeded")
    return NodeStatus::Completed;
  if (*status == "failed" || *status == "error")
    return NodeStatus::Failed;
  if (*status == "interrupted" || *status == "cancelled")
    return NodeStatus::Interrupted;
  if (*status == "notLoaded")
    return NodeStatus::NotLoaded;
  if (*status == "connected")
    return NodeStatus::Connected;
  if (*status == "disconnected")
    return NodeStatus::Disconnected;
  return NodeStatus::Unknown;
}

void mergeObject(NodeGraph::WriteAccess &write, const NodeRef &node,
                 const Value::Object &object,
                 std::string_view omittedChildField = {}) {
  NodeState next = *write.state(node);
  for (const auto &[key, value] : object) {
    if (!omittedChildField.empty() && key == omittedChildField)
      continue;
    next.fields.insert_or_assign(key, value);
  }
  const NodeStatus status = statusFromValue(member(object, "status"));
  if (status != NodeStatus::Unknown)
    next.status = status;
  write.replaceState(node, std::move(next));
}

std::string retainedProtocolId(NodeGraph::WriteAccess &write,
                               const NodeRef &node) {
  if (!node)
    return {};
  const std::shared_ptr<const NodeState> state = write.state(node);
  const auto found = state->fields.find("protocolId");
  return found == state->fields.end() ? node->id().canonical
                                      : canonicalValue(&found->second);
}

NodeRef ensureTurn(NodeGraph::WriteAccess &write, const NodeRef &thread,
                   std::string_view rawTurnId) {
  if (!thread || thread->id().kind != NodeKind::Thread || rawTurnId.empty())
    return {};
  NodeRef turn =
      write.upsert(scopedTurnNodeId(thread->id().canonical, rawTurnId));
  write.setField(turn, "protocolId", Value(rawTurnId));
  write.setField(turn, "protocolThreadId", Value(thread->id().canonical));
  write.setParent(thread, turn);
  return turn;
}

NodeRef ensureItem(NodeGraph::WriteAccess &write, const NodeRef &turn,
                   std::string_view rawItemId) {
  if (!turn || turn->id().kind != NodeKind::Turn || rawItemId.empty())
    return {};
  NodeRef item = write.upsert(scopedItemNodeId(turn->id(), rawItemId));
  write.setField(item, "protocolId", Value(rawItemId));
  write.setField(item, "protocolTurnId",
                 Value(retainedProtocolId(write, turn)));
  if (NodeRef thread = write.parent(turn))
    write.setField(item, "protocolThreadId", Value(thread->id().canonical));
  write.setParent(turn, item);
  return item;
}

std::optional<std::size_t> indexValue(const Value *value) {
  if (!value)
    return std::nullopt;
  if (const std::uint64_t *index = value->asUInt64())
    return static_cast<std::size_t>(*index);
  if (const std::int64_t *index = value->asInt64(); index && *index >= 0)
    return static_cast<std::size_t>(*index);
  return std::nullopt;
}

void appendIndexedField(NodeGraph::WriteAccess &write, const NodeRef &node,
                        std::string field, std::size_t index,
                        std::string_view suffix) {
  NodeState next = *write.state(node);
  Value &stored = next.fields[std::move(field)];
  if (!stored.isArray())
    stored = Value::Array{};
  Value::Array &parts = *stored.asArray();
  if (parts.size() <= index)
    parts.resize(index + 1);
  std::string combined;
  if (const std::string *existing = parts[index].asString())
    combined = *existing;
  combined.append(suffix);
  parts[index] = Value(std::move(combined));
  write.replaceState(node, std::move(next));
}

void appendSemanticDelta(NodeGraph::WriteAccess &write, const NodeRef &item,
                         std::string_view method,
                         const Value::Object &payload) {
  const std::string delta = canonicalValue(member(payload, "delta"));
  if (delta.empty())
    return;
  if (method == "item/reasoning/summaryTextDelta") {
    appendIndexedField(write, item, "summary",
                       indexValue(member(payload, "summaryIndex")).value_or(0),
                       delta);
    return;
  }
  if (method == "item/reasoning/textDelta") {
    appendIndexedField(write, item, "content",
                       indexValue(member(payload, "contentIndex")).value_or(0),
                       delta);
    return;
  }
  if (method == "item/commandExecution/outputDelta") {
    write.appendStringField(item, "aggregatedOutput", delta);
    return;
  }
  if (method == "item/fileChange/outputDelta") {
    write.appendStringField(item, "output", delta);
    return;
  }
  write.appendStringField(item, "text", delta);
}

std::string addressedId(const Value::Object &payload, NodeKind kind) {
  switch (kind) {
  case NodeKind::Thread: {
    constexpr std::array names{std::string_view("threadId"),
                               std::string_view("conversationId")};
    return firstId(payload, names);
  }
  case NodeKind::Turn: {
    constexpr std::array names{std::string_view("turnId")};
    return firstId(payload, names);
  }
  case NodeKind::Item: {
    constexpr std::array names{std::string_view("itemId"),
                               std::string_view("callId")};
    return firstId(payload, names);
  }
  case NodeKind::Project: {
    constexpr std::array names{std::string_view("projectId")};
    return firstId(payload, names);
  }
  case NodeKind::ThreadSection: {
    constexpr std::array names{std::string_view("sectionId")};
    return firstId(payload, names);
  }
  case NodeKind::Process: {
    constexpr std::array names{std::string_view("processId"),
                               std::string_view("processHandle"),
                               std::string_view("commandId")};
    return firstId(payload, names);
  }
  case NodeKind::RealtimeSession: {
    constexpr std::array names{std::string_view("sessionId")};
    return firstId(payload, names);
  }
  case NodeKind::FilesystemWatch: {
    constexpr std::array names{std::string_view("watchId")};
    return firstId(payload, names);
  }
  default:
    return {};
  }
}

std::string nestedId(const Value::Object &object,
                     std::string_view fallback = {}) {
  constexpr std::array names{
      std::string_view("id"), std::string_view("threadId"),
      std::string_view("turnId"), std::string_view("itemId")};
  std::string result = firstId(object, names);
  return result.empty() ? std::string(fallback) : result;
}

bool beginsWith(std::string_view value, std::string_view prefix) {
  return value.starts_with(prefix);
}

bool isItemDeltaMethod(std::string_view method) {
  return method == "item/agentMessage/delta" || method == "item/plan/delta" ||
         method == "item/reasoning/summaryTextDelta" ||
         method == "item/reasoning/textDelta" ||
         method == "item/commandExecution/outputDelta" ||
         method == "item/fileChange/outputDelta";
}

NodeKind kindForMethod(std::string_view method) {
  if (beginsWith(method, "item/"))
    return NodeKind::Item;
  if (beginsWith(method, "turn/"))
    return NodeKind::Turn;
  if (beginsWith(method, "thread/realtime/"))
    return NodeKind::RealtimeSession;
  if (beginsWith(method, "thread/"))
    return NodeKind::Thread;
  if (beginsWith(method, "project/"))
    return NodeKind::Project;
  if (beginsWith(method, "threadSection/"))
    return NodeKind::ThreadSection;
  if (beginsWith(method, "command/") || beginsWith(method, "process/"))
    return NodeKind::Process;
  if (beginsWith(method, "account/"))
    return NodeKind::Account;
  if (beginsWith(method, "config") || beginsWith(method, "skills/config"))
    return NodeKind::Configuration;
  if (beginsWith(method, "mcpServer/"))
    return NodeKind::McpServer;
  if (beginsWith(method, "fs/"))
    return NodeKind::FilesystemWatch;
  if (method == "error" || method == "warning" || method == "guardianWarning" ||
      method == "deprecationNotice" || method == "configWarning" ||
      method == "windows/worldWritableWarning")
    return NodeKind::Notice;
  return NodeKind::Catalog;
}

std::string catalogKey(std::string_view method) {
  const std::size_t slash = method.find('/');
  return std::string(method.substr(0, slash));
}

NodeRef containingThread(NodeGraph::WriteAccess &write, NodeRef node) {
  while (node && node->id().kind != NodeKind::Thread)
    node = write.parent(node);
  return node;
}

void clearPendingInteractionOwner(NodeGraph::WriteAccess &write,
                                  const NodeRef &interaction) {
  if (NodeRef runtime = write.find({NodeKind::Runtime, "runtime"}))
    write.unrelate(runtime, RelationKind::PendingInteraction, interaction);
  for (const NodeRef &target :
       write.related(interaction, RelationKind::InteractionTarget)) {
    if (NodeRef thread = containingThread(write, target))
      write.unrelate(thread, RelationKind::PendingInteraction, interaction);
  }
}

std::vector<NodeRef> mergeExistingTail(std::vector<NodeRef> first,
                                       const std::vector<NodeRef> &existing) {
  for (const NodeRef &node : existing) {
    if (std::find(first.begin(), first.end(), node) == first.end())
      first.emplace_back(node);
  }
  return first;
}

std::vector<NodeRef> mergeLocalTail(NodeGraph::WriteAccess &write,
                                    std::vector<NodeRef> first,
                                    const std::vector<NodeRef> &existing) {
  for (const NodeRef &node : existing) {
    const std::shared_ptr<const NodeState> state = write.state(node);
    const Value *local = member(state->fields, "local");
    if ((!local || !local->asBool() || !*local->asBool()) ||
        std::find(first.begin(), first.end(), node) != first.end())
      continue;
    first.emplace_back(node);
  }
  return first;
}

bool hasThreadOwner(NodeGraph::WriteAccess &write, const NodeRef &child) {
  for (const NodeRef &candidate : write.orderedNodes()) {
    if (candidate->id().kind != NodeKind::Thread || candidate == child)
      continue;
    for (const RelationKind kind : {RelationKind::StructuralChildThread,
                                    RelationKind::AgentChildThread}) {
      const std::vector<NodeRef> children = write.related(candidate, kind);
      if (std::find(children.begin(), children.end(), child) != children.end())
        return true;
    }
  }
  return false;
}

void clearThreadOwners(NodeGraph::WriteAccess &write, const NodeRef &child) {
  const std::vector<NodeRef> nodes = write.orderedNodes();
  for (const NodeRef &candidate : nodes) {
    if (candidate->id().kind != NodeKind::Thread || candidate == child)
      continue;
    write.unrelate(candidate, RelationKind::StructuralChildThread, child);
    write.unrelate(candidate, RelationKind::AgentChildThread, child);
  }
}

void clearThreadOwnerKind(NodeGraph::WriteAccess &write, const NodeRef &child,
                          RelationKind kind) {
  const std::vector<NodeRef> nodes = write.orderedNodes();
  for (const NodeRef &candidate : nodes) {
    if (candidate->id().kind == NodeKind::Thread && candidate != child)
      write.unrelate(candidate, kind, child);
  }
}

void assignThreadOwner(NodeGraph::WriteAccess &write, const NodeRef &owner,
                       RelationKind kind, const NodeRef &child) {
  if (!owner || !child || owner == child)
    return;
  clearThreadOwners(write, child);
  write.relate(owner, kind, child);
  if (NodeRef runtime = write.find({NodeKind::Runtime, "runtime"})) {
    std::vector<NodeRef> roots =
        write.related(runtime, RelationKind::RootThread);
    const std::size_t before = roots.size();
    roots.erase(std::remove(roots.begin(), roots.end(), child), roots.end());
    if (roots.size() != before)
      write.replaceRelated(runtime, RelationKind::RootThread, roots);
  }
}

void replaceSingleRelation(NodeGraph::WriteAccess &write, const NodeRef &source,
                           RelationKind kind, const NodeRef &target) {
  if (target) {
    std::array<NodeRef, 1> only{target};
    write.replaceRelated(source, kind, only);
  } else {
    write.replaceRelated(source, kind, std::span<const NodeRef>{});
  }
}

void assignProject(NodeGraph::WriteAccess &write, const NodeRef &thread,
                   const Value *projectId) {
  const std::string id = canonicalValue(projectId);
  NodeRef project;
  if (!id.empty())
    project = write.upsert({NodeKind::Project, id});
  replaceSingleRelation(write, thread, RelationKind::ProjectMembership,
                        project);
  write.setField(thread, "projectId", projectId ? *projectId : Value(nullptr));
}

void assignSection(NodeGraph::WriteAccess &write, const NodeRef &thread,
                   const Value *sectionValue) {
  NodeRef section;
  if (const Value::Object *object =
          sectionValue ? sectionValue->asObject() : nullptr) {
    const std::string id = nestedId(*object);
    if (!id.empty()) {
      section = write.upsert({NodeKind::ThreadSection, id});
      mergeObject(write, section, *object);
    }
  } else {
    const std::string id = canonicalValue(sectionValue);
    if (!id.empty())
      section = write.upsert({NodeKind::ThreadSection, id});
  }
  replaceSingleRelation(write, thread, RelationKind::SectionMembership,
                        section);
  write.setField(thread, "section",
                 sectionValue ? *sectionValue : Value(nullptr));
}

std::vector<std::string> agentChildIds(const Value::Object &item) {
  std::vector<std::string> children;
  std::string child = canonicalValue(member(item, "agentThreadId"));
  if (!child.empty())
    children.emplace_back(std::move(child));
  const Value::Array *receivers = arrayMember(item, "receiverThreadIds");
  if (!receivers)
    return children;
  for (const Value &receiver : *receivers) {
    child = canonicalValue(&receiver);
    if (!child.empty() &&
        std::find(children.begin(), children.end(), child) == children.end())
      children.emplace_back(std::move(child));
  }
  return children;
}

bool isUserMessage(const Value::Object &item) {
  return canonicalValue(member(item, "type")) == "userMessage";
}

void correlateLocalPrompt(NodeGraph::WriteAccess &write,
                          const NodeRef &authoritative,
                          const Value::Object &item) {
  if (!authoritative || !isUserMessage(item))
    return;
  const std::string clientId = canonicalValue(member(item, "clientId"));
  if (clientId.empty())
    return;
  NodeRef runtime = write.find({NodeKind::Runtime, "runtime"});
  if (!runtime)
    return;
  for (const NodeRef &local :
       write.related(runtime, RelationKind::PendingPrompt)) {
    if (!local || local->id().kind != NodeKind::Item)
      continue;
    const std::shared_ptr<const NodeState> state = write.state(local);
    const auto found = state->fields.find("clientUserMessageId");
    if (found == state->fields.end() || !found->second.asString() ||
        *found->second.asString() != clientId)
      continue;
    std::array<NodeRef, 1> alias{local};
    write.replaceRelated(authoritative, RelationKind::PromptMaterialization,
                         alias);
    break;
  }
}

void updateLoadedHistoryItemCount(NodeGraph::WriteAccess &write,
                                  const NodeRef &thread) {
  if (!thread)
    return;
  std::unordered_set<const Node *> loaded;
  for (const NodeRef &turn : write.children(thread)) {
    if (!turn || turn->id().kind != NodeKind::Turn)
      continue;
    for (const NodeRef &item : write.children(turn)) {
      if (item && item->id().kind == NodeKind::Item)
        loaded.insert(item.get());
    }
    for (const NodeRef &root :
         write.related(turn, RelationKind::TurnRootItem)) {
      if (root && root->id().kind == NodeKind::Item)
        loaded.insert(root.get());
    }
  }
  write.setField(thread, "historyLoadedItemCount",
                 Value(static_cast<std::uint64_t>(loaded.size())));
}

NodeId realtimeSessionNodeId(std::string_view threadId) {
  return NodeId{NodeKind::RealtimeSession,
                scopedCanonical(threadId, "current")};
}

NodeId realtimeItemNodeId(const NodeRef &session, std::string_view itemId) {
  return NodeId{NodeKind::Item,
                scopedCanonical(session->id().canonical, itemId)};
}

void removeContained(NodeGraph::WriteAccess &write, const NodeRef &node) {
  for (const NodeRef &child : write.children(node)) {
    removeContained(write, child);
    write.remove(child);
  }
}

NodeRef ensureRealtimeSession(NodeGraph::WriteAccess &write,
                              const NodeRef &thread) {
  if (!thread)
    return {};
  NodeRef session = write.upsert(realtimeSessionNodeId(thread->id().canonical));
  write.setField(session, "protocolThreadId", Value(thread->id().canonical));
  write.setParent(thread, session);
  if (write.state(session)->status == NodeStatus::Unknown) {
    write.setStatus(session, NodeStatus::Running);
    write.setField(session, "active", Value(true));
    write.setField(session, "lifecycle", Value("running"));
  }
  return session;
}

NodeRef ensureRealtimeItem(NodeGraph::WriteAccess &write,
                           const NodeRef &session, std::string_view itemId) {
  if (!session || itemId.empty())
    return {};
  NodeRef item = write.upsert(realtimeItemNodeId(session, itemId));
  write.setField(item, "protocolId", Value(itemId));
  const std::shared_ptr<const NodeState> sessionState = write.state(session);
  if (const Value *threadId = member(sessionState->fields, "protocolThreadId"))
    write.setField(item, "protocolThreadId", *threadId);
  if (const Value *sessionId = member(sessionState->fields, "protocolId"))
    write.setField(item, "realtimeSessionId", *sessionId);
  write.setField(item, "realtime", Value(true));
  write.setParent(session, item);
  return item;
}

void appendArrayValue(NodeGraph::WriteAccess &write, const NodeRef &node,
                      std::string key, Value value) {
  NodeState next = *write.state(node);
  Value &stored = next.fields[std::move(key)];
  if (!stored.isArray())
    stored = Value::Array{};
  stored.asArray()->emplace_back(std::move(value));
  write.replaceState(node, std::move(next));
}

void updateRoleTranscript(NodeGraph::WriteAccess &write, const NodeRef &session,
                          std::string_view role, std::string_view text,
                          bool append, bool completed) {
  NodeState next = *write.state(session);
  Value &stored = next.fields["transcripts"];
  if (!stored.isObject())
    stored = Value::Object{};
  Value::Object &transcripts = *stored.asObject();
  Value &roleText = transcripts[std::string(role)];
  std::string combined;
  if (append) {
    if (const std::string *current = roleText.asString())
      combined = *current;
    combined.append(text);
  } else {
    combined = std::string(text);
  }
  roleText = Value(std::move(combined));

  if (completed) {
    Value &completion = next.fields["transcriptCompleted"];
    if (!completion.isObject())
      completion = Value::Object{};
    completion.asObject()->insert_or_assign(std::string(role), Value(true));
  }
  write.replaceState(session, std::move(next));
}

std::uint64_t unsignedField(const NodeState &state, std::string_view name) {
  const Value *value = member(state.fields, name);
  if (const std::uint64_t *number = value ? value->asUInt64() : nullptr)
    return *number;
  if (const std::int64_t *number = value ? value->asInt64() : nullptr;
      number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return 0;
}

NodeRef currentConnection(NodeGraph::WriteAccess &write) {
  return write.upsert({NodeKind::Connection, "connection"});
}

std::string connectionIncarnation(NodeGraph::WriteAccess &write,
                                  const NodeRef &connection) {
  const std::shared_ptr<const NodeState> state = write.state(connection);
  return "connection:" +
         std::to_string(unsignedField(*state, "connectionGeneration")) +
         ":provider:" +
         std::to_string(unsignedField(*state, "providerGeneration"));
}

NodeRef ensureConnectionScopedNode(NodeGraph::WriteAccess &write, NodeKind kind,
                                   std::string_view rawId) {
  if (rawId.empty())
    return {};
  NodeRef connection = currentConnection(write);
  const std::shared_ptr<const NodeState> connectionState =
      write.state(connection);
  NodeRef node = write.upsert(
      {kind, scopedCanonical(connectionIncarnation(write, connection), rawId)});
  write.setField(node, "protocolId", Value(rawId));
  write.setField(
      node, "connectionGeneration",
      Value(unsignedField(*connectionState, "connectionGeneration")));
  write.setField(node, "providerGeneration",
                 Value(unsignedField(*connectionState, "providerGeneration")));
  if (kind == NodeKind::Process)
    write.relate(connection, RelationKind::ProcessOwner, node);
  return node;
}

NodeRef ensureProcess(NodeGraph::WriteAccess &write,
                      const Value::Object &payload) {
  constexpr std::array names{std::string_view("processId"),
                             std::string_view("processHandle"),
                             std::string_view("commandId")};
  return ensureConnectionScopedNode(write, NodeKind::Process,
                                    firstId(payload, names));
}

NodeRef ensureWatch(NodeGraph::WriteAccess &write,
                    const Value::Object &payload) {
  return ensureConnectionScopedNode(write, NodeKind::FilesystemWatch,
                                    canonicalValue(member(payload, "watchId")));
}

} // namespace

NodeId scopedTurnNodeId(std::string_view threadId, std::string_view turnId) {
  return NodeId{NodeKind::Turn, scopedCanonical(threadId, turnId)};
}

NodeId scopedItemNodeId(const NodeId &turnNodeId, std::string_view itemId) {
  return NodeId{NodeKind::Item, scopedCanonical(turnNodeId.canonical, itemId)};
}

std::string protocolCanonicalId(const NodeState &state, const NodeRef &node) {
  const auto found = state.fields.find("protocolId");
  if (found != state.fields.end()) {
    const std::string retained = canonicalValue(&found->second);
    if (!retained.empty())
      return retained;
  }
  return node ? node->id().canonical : std::string{};
}

std::string ProtocolRequestId::canonical() const {
  if (const std::int64_t *number = std::get_if<std::int64_t>(&value))
    return "number:" + std::to_string(*number);
  return "string:" + std::get<std::string>(value);
}

ProtocolUpdater::ProtocolUpdater(NodeGraph &graph) noexcept : graph_(&graph) {}

ApplyResult ProtocolUpdater::apply(DecodedMessage message) {
  const ProtocolDirection direction = catalogDirection(message.kind);
  const auto descriptor = findProtocolMethod(direction, message.method);
  if (!descriptor) {
    auto write = graph_->write();
    applyUnknown(write, message);
    applyThreadActivity(write, message);
    return ApplyResult{
        false, MessageDisposition::GraphUpdate, write.finish(), {}};
  }

  if (descriptor->get().disposition ==
      MessageDisposition::IntentionallyStateNeutral) {
    return ApplyResult{true,
                       descriptor->get().disposition,
                       GraphChange{graph_->publishedRevision(), {}, {}},
                       {}};
  }

  auto write = graph_->write();
  NodeRef primary;
  switch (descriptor->get().disposition) {
  case MessageDisposition::WorkerOperationResult:
    primary = applyOperation(write, message);
    break;
  case MessageDisposition::ReverseInteraction:
    primary = applyInteraction(write, message);
    break;
  case MessageDisposition::GraphUpdate:
  case MessageDisposition::TypedUiEffect:
    applyGraphUpdate(write, message);
    break;
  case MessageDisposition::IntentionallyStateNeutral:
    break;
  }
  applyThreadActivity(write, message);
  return ApplyResult{true, descriptor->get().disposition, write.finish(),
                     std::move(primary)};
}

void ProtocolUpdater::applyThreadActivity(NodeGraph::WriteAccess &write,
                                          const DecodedMessage &message) {
  if (!message.activityAt)
    return;

  NodeRef thread;
  std::string threadId = addressedId(message.payload, NodeKind::Thread);
  if (threadId.empty()) {
    if (const Value::Object *object = objectMember(message.payload, "thread"))
      threadId = nestedId(*object);
  }
  if (!threadId.empty())
    thread = write.find({NodeKind::Thread, threadId});

  if (!thread)
    return;

  std::unordered_set<const Node *> visited;
  while (thread && visited.insert(thread.get()).second) {
    const std::shared_ptr<const NodeState> state = write.state(thread);
    const Value *existing = member(state->fields, "localActivityAt");
    const std::int64_t *signedValue = existing ? existing->asInt64() : nullptr;
    const std::uint64_t *unsignedValue =
        existing ? existing->asUInt64() : nullptr;
    const bool newer =
        (!signedValue && !unsignedValue) ||
        (signedValue && *message.activityAt > *signedValue) ||
        (unsignedValue && *message.activityAt >= 0 &&
         static_cast<std::uint64_t>(*message.activityAt) > *unsignedValue);
    if (newer)
      write.setField(thread, "localActivityAt", Value(*message.activityAt));

    NodeRef owner;
    for (const NodeRef &candidate : write.orderedNodes()) {
      if (!candidate || candidate->id().kind != NodeKind::Thread ||
          candidate == thread)
        continue;
      for (const RelationKind relation : {RelationKind::StructuralChildThread,
                                          RelationKind::AgentChildThread}) {
        const std::vector<NodeRef> children =
            write.related(candidate, relation);
        if (std::find(children.begin(), children.end(), thread) !=
            children.end()) {
          owner = candidate;
          break;
        }
      }
      if (owner)
        break;
    }
    thread = std::move(owner);
  }
}

GraphChange
ProtocolUpdater::resolveInteraction(const ProtocolRequestId &requestId,
                                    bool accepted, std::string error) {
  auto write = graph_->write();
  NodeRef interaction =
      write.find({NodeKind::Interaction, requestId.canonical()});
  if (!interaction)
    return write.finish();
  if (!accepted) {
    write.setStatus(interaction, NodeStatus::Failed);
    write.setField(interaction, "error", Value(std::move(error)));
    return write.finish();
  }
  write.remove(interaction);
  return write.finish();
}

GraphChange ProtocolUpdater::resolveInteraction(const NodeRef &interaction,
                                                bool accepted,
                                                std::string error) {
  auto write = graph_->write();
  if (!interaction || interaction->id().kind != NodeKind::Interaction)
    return write.finish();
  const NodeRef current = write.find(interaction->id());
  if (current != interaction)
    return write.finish();
  if (!accepted) {
    write.setStatus(interaction, NodeStatus::Failed);
    write.setField(interaction, "error", Value(std::move(error)));
    return write.finish();
  }
  write.remove(interaction);
  return write.finish();
}

ProtocolDirection
ProtocolUpdater::catalogDirection(DecodedMessageKind kind) const noexcept {
  switch (kind) {
  case DecodedMessageKind::ClientRequest:
  case DecodedMessageKind::ClientResult:
  case DecodedMessageKind::ClientError:
    return ProtocolDirection::ClientRequest;
  case DecodedMessageKind::ServerRequest:
    return ProtocolDirection::ServerRequest;
  case DecodedMessageKind::ServerNotification:
    return ProtocolDirection::ServerNotification;
  case DecodedMessageKind::ClientNotification:
    return ProtocolDirection::ClientNotification;
  }
  return ProtocolDirection::ServerNotification;
}

NodeRef ProtocolUpdater::applyOperation(NodeGraph::WriteAccess &write,
                                        const DecodedMessage &message) {
  if (message.kind == DecodedMessageKind::ClientNotification) {
    NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
    write.setField(runtime, "initialized", Value(true));
    write.setField(runtime, "lastMethod", Value(message.method));
    return runtime;
  }

  const std::string operationId = message.requestId
                                      ? message.requestId->canonical()
                                      : "uncorrelated:" + message.method;
  if (message.kind == DecodedMessageKind::ClientRequest) {
    if (NodeRef previous = write.find({NodeKind::Operation, operationId}))
      write.remove(previous);
    NodeRef operation = write.upsert({NodeKind::Operation, operationId});
    write.setField(operation, "method", Value(message.method));
    write.setField(operation, "requestPayload", Value(message.payload));
    write.setStatus(operation, NodeStatus::Pending);
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    const std::string turnId = addressedId(message.payload, NodeKind::Turn);
    const std::string itemId = addressedId(message.payload, NodeKind::Item);
    NodeRef thread;
    if (!threadId.empty())
      thread = write.upsert({NodeKind::Thread, threadId});
    NodeRef turn;
    if (!turnId.empty())
      turn = ensureTurn(write, thread, turnId);
    NodeRef target;
    if (beginsWith(message.method, "command/exec") ||
        beginsWith(message.method, "process/")) {
      target = ensureProcess(write, message.payload);
      if (target) {
        mergeObject(write, target, message.payload);
        if (message.method == "command/exec" ||
            message.method == "process/spawn")
          write.setStatus(target, NodeStatus::Running);
      }
    } else if (message.method == "fs/watch" || message.method == "fs/unwatch") {
      target = ensureWatch(write, message.payload);
      if (target) {
        mergeObject(write, target, message.payload);
        if (message.method == "fs/watch")
          write.setStatus(target, NodeStatus::Pending);
      }
    } else if (!itemId.empty())
      target = ensureItem(write, turn, itemId);
    if (!target && turn)
      target = turn;
    if (!target && thread)
      target = thread;
    if (target)
      write.relate(operation, RelationKind::OperationTarget, target);
    return operation;
  }

  NodeRef operation = write.find({NodeKind::Operation, operationId});
  if (!operation) {
    // Tests and bridge integrations may deliver an already-correlated result
    // without asking the graph to expose its transient request. Exact worker
    // callbacks always supply expectedNode, in which case absence means stale.
    if (!message.expectedNode &&
        message.kind == DecodedMessageKind::ClientResult)
      applyGraphUpdate(write, message);
    return {};
  }
  if (message.expectedNode && message.expectedNode != operation)
    return {};
  const std::shared_ptr<const NodeState> operationState =
      write.state(operation);
  const Value *storedMethod = nullptr;
  if (const auto found = operationState->fields.find("method");
      found != operationState->fields.end())
    storedMethod = &found->second;
  if (operationState->status != NodeStatus::Pending || !storedMethod ||
      !storedMethod->asString() || *storedMethod->asString() != message.method)
    return {};

  if (message.kind == DecodedMessageKind::ClientResult) {
    DecodedMessage correlated = message;
    const auto request = operationState->fields.find("requestPayload");
    if (request != operationState->fields.end()) {
      if (const Value::Object *requestObject = request->second.asObject()) {
        for (const auto &[key, value] : *requestObject)
          correlated.payload.try_emplace(key, value);
      }
    }
    applyGraphUpdate(write, correlated);
  }
  // Operations model only work that is currently pending. Results/errors are
  // applied to current state and then the operation is retired in this same
  // graph transaction; retaining terminal operations would be an event log.
  write.remove(operation);
  return {};
}

NodeRef ProtocolUpdater::applyInteraction(NodeGraph::WriteAccess &write,
                                          const DecodedMessage &message) {
  if (!message.requestId)
    throw std::invalid_argument("a server request requires an id");
  if (NodeRef previous =
          write.find({NodeKind::Interaction, message.requestId->canonical()}))
    write.remove(previous);
  NodeRef interaction =
      write.upsert({NodeKind::Interaction, message.requestId->canonical()});
  clearPendingInteractionOwner(write, interaction);
  const std::vector<NodeRef> previousTargets =
      write.related(interaction, RelationKind::InteractionTarget);
  for (const NodeRef &previous : previousTargets)
    write.unrelate(interaction, RelationKind::InteractionTarget, previous);
  NodeState state;
  state.status = NodeStatus::Pending;
  state.fields.emplace("method", Value(message.method));
  state.fields.emplace("payload", Value(message.payload));
  state.fields.emplace("requestId", Value(message.requestId->canonical()));
  write.replaceState(interaction, std::move(state));

  const std::string threadId = addressedId(message.payload, NodeKind::Thread);
  const std::string turnId = addressedId(message.payload, NodeKind::Turn);
  const std::string itemId = addressedId(message.payload, NodeKind::Item);
  NodeRef thread;
  NodeRef turn;
  NodeRef item;
  if (!threadId.empty())
    thread = write.upsert({NodeKind::Thread, threadId});
  if (!turnId.empty())
    turn = ensureTurn(write, thread, turnId);
  if (!itemId.empty())
    item = ensureItem(write, turn, itemId);
  NodeRef target;
  if (item)
    target = item;
  else if (turn)
    target = turn;
  else if (thread)
    target = thread;
  if (target) {
    write.relate(interaction, RelationKind::InteractionTarget, target);
    if (NodeRef owner = containingThread(write, target))
      write.relate(owner, RelationKind::PendingInteraction, interaction);
  }
  NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
  write.relate(runtime, RelationKind::PendingInteraction, interaction);
  return interaction;
}

bool ProtocolUpdater::applyRealtimeUpdate(NodeGraph::WriteAccess &write,
                                          const DecodedMessage &message) {
  const std::string_view method = message.method;
  if (!beginsWith(method, "thread/realtime/"))
    return false;

  const std::string threadId = addressedId(message.payload, NodeKind::Thread);
  if (threadId.empty())
    return true;
  NodeRef thread = write.upsert({NodeKind::Thread, threadId});
  NodeRef session = ensureRealtimeSession(write, thread);

  if (method == "thread/realtime/started") {
    const std::string sessionId =
        canonicalValue(member(message.payload, "realtimeSessionId"));
    const std::shared_ptr<const NodeState> previous = write.state(session);
    const std::string previousId =
        canonicalValue(member(previous->fields, "protocolId"));
    if (previous->status != NodeStatus::Running || previousId != sessionId) {
      removeContained(write, session);
      NodeState next;
      next.status = NodeStatus::Running;
      next.fields.emplace("protocolThreadId", Value(threadId));
      next.fields.emplace("active", Value(true));
      next.fields.emplace("lifecycle", Value("running"));
      if (!sessionId.empty())
        next.fields.emplace("protocolId", Value(sessionId));
      for (const auto &[key, value] : message.payload)
        next.fields.insert_or_assign(key, value);
      write.replaceState(session, std::move(next));
    } else {
      mergeObject(write, session, message.payload);
      write.setField(session, "active", Value(true));
      write.setField(session, "lifecycle", Value("running"));
      write.setStatus(session, NodeStatus::Running);
    }
    return true;
  }

  if (method == "thread/realtime/itemAdded") {
    const Value *value = member(message.payload, "item");
    const Value::Object *object = value ? value->asObject() : nullptr;
    const std::string itemSessionId =
        object ? canonicalValue(member(*object, "realtimeSessionId"))
               : std::string{};
    const std::string currentSessionId =
        canonicalValue(member(write.state(session)->fields, "protocolId"));
    if (!itemSessionId.empty() && !currentSessionId.empty() &&
        itemSessionId != currentSessionId)
      return true;
    const std::string itemId = object ? nestedId(*object) : std::string{};
    if (NodeRef realtimeItem = ensureRealtimeItem(write, session, itemId)) {
      mergeObject(write, realtimeItem, *object);
    } else if (value) {
      appendArrayValue(write, session, "unaddressedItems", *value);
    }
    return true;
  }

  if (method == "thread/realtime/item/started" ||
      method == "thread/realtime/item/completed") {
    const Value::Object *object = objectMember(message.payload, "item");
    const std::string itemSessionId =
        object ? canonicalValue(member(*object, "realtimeSessionId"))
               : std::string{};
    const std::string currentSessionId =
        canonicalValue(member(write.state(session)->fields, "protocolId"));
    if (!itemSessionId.empty() && !currentSessionId.empty() &&
        itemSessionId != currentSessionId)
      return true;
    std::string itemId = object ? nestedId(*object) : std::string{};
    if (itemId.empty())
      itemId = addressedId(message.payload, NodeKind::Item);
    if (NodeRef realtimeItem = ensureRealtimeItem(write, session, itemId)) {
      if (object)
        mergeObject(write, realtimeItem, *object);
      const NodeStatus supplied =
          object ? statusFromValue(member(*object, "status"))
                 : NodeStatus::Unknown;
      if (method == "thread/realtime/item/started") {
        if (supplied == NodeStatus::Unknown)
          write.setStatus(realtimeItem, NodeStatus::Running);
      } else if (supplied == NodeStatus::Unknown) {
        write.setStatus(realtimeItem, NodeStatus::Completed);
      }
    }
    return true;
  }

  if (method == "thread/realtime/item/transcript/delta") {
    const std::string itemId = addressedId(message.payload, NodeKind::Item);
    if (NodeRef realtimeItem = ensureRealtimeItem(write, session, itemId)) {
      const std::string delta =
          canonicalValue(member(message.payload, "delta"));
      if (!delta.empty())
        write.appendStringField(realtimeItem, "transcript", delta);
    }
    return true;
  }

  if (method == "thread/realtime/transcript/delta" ||
      method == "thread/realtime/transcript/done") {
    const std::string role = canonicalValue(member(message.payload, "role"));
    const bool done = method == "thread/realtime/transcript/done";
    const std::string text =
        canonicalValue(member(message.payload, done ? "text" : "delta"));
    updateRoleTranscript(write, session, role.empty() ? "unknown" : role, text,
                         !done, done);
    return true;
  }

  if (method == "thread/realtime/outputAudio/delta") {
    const Value *audio = member(message.payload, "audio");
    const Value::Object *audioObject = audio ? audio->asObject() : nullptr;
    const std::string itemId =
        audioObject ? canonicalValue(member(*audioObject, "itemId"))
                    : std::string{};
    NodeRef target = ensureRealtimeItem(write, session, itemId);
    if (!target)
      target = session;
    if (audio)
      appendArrayValue(write, target, "outputAudioChunks", *audio);
    return true;
  }

  if (method == "thread/realtime/sdp") {
    if (const Value *sdp = member(message.payload, "sdp"))
      write.setField(session, "sdp", *sdp);
    return true;
  }

  if (method == "thread/realtime/error") {
    if (const Value *error = member(message.payload, "message"))
      appendArrayValue(write, session, "errors", *error);
    write.setStatus(session, NodeStatus::Failed);
    write.setField(session, "active", Value(false));
    write.setField(session, "lifecycle", Value("failed"));
    return true;
  }

  if (method == "thread/realtime/closed") {
    if (const Value *reason = member(message.payload, "reason"))
      write.setField(session, "closedReason", *reason);
    if (write.state(session)->status != NodeStatus::Failed)
      write.setStatus(session, NodeStatus::Completed);
    write.setField(session, "active", Value(false));
    write.setField(session, "lifecycle", Value("closed"));
    return true;
  }

  return true;
}

void ProtocolUpdater::applyGraphUpdate(NodeGraph::WriteAccess &write,
                                       const DecodedMessage &message) {
  const std::string_view method = message.method;

  if (applyRealtimeUpdate(write, message))
    return;

  if (method == "command/exec/outputDelta" || method == "process/outputDelta") {
    NodeRef process = ensureProcess(write, message.payload);
    if (!process)
      return;
    const std::string stream =
        canonicalValue(member(message.payload, "stream"));
    const std::string delta =
        canonicalValue(member(message.payload, "deltaBase64"));
    if (!delta.empty())
      write.appendStringField(
          process, (stream.empty() ? "output" : stream) + "Base64", delta);
    if (const Value *capReached = member(message.payload, "capReached"))
      write.setField(process,
                     (stream.empty() ? "output" : stream) + "CapReached",
                     *capReached);
    write.setStatus(process, NodeStatus::Running);
    return;
  }

  if (method == "process/exited") {
    if (NodeRef process = ensureProcess(write, message.payload)) {
      mergeObject(write, process, message.payload);
      write.setStatus(process, NodeStatus::Completed);
    }
    return;
  }

  if (method == "fs/changed") {
    if (NodeRef watch = ensureWatch(write, message.payload)) {
      mergeObject(write, watch, message.payload);
      write.setStatus(watch, NodeStatus::Running);
    }
    return;
  }

  if (message.kind == DecodedMessageKind::ClientResult &&
      (beginsWith(method, "command/exec") || beginsWith(method, "process/") ||
       method == "fs/watch" || method == "fs/unwatch")) {
    if (method == "fs/watch" || method == "fs/unwatch") {
      if (NodeRef watch = ensureWatch(write, message.payload)) {
        if (method == "fs/unwatch")
          write.remove(watch);
        else {
          mergeObject(write, watch, message.payload);
          write.setStatus(watch, NodeStatus::Running);
        }
      }
      return;
    }
    if (NodeRef process = ensureProcess(write, message.payload)) {
      mergeObject(write, process, message.payload);
      if (method == "command/exec")
        write.setStatus(process, NodeStatus::Completed);
      else if (method == "process/spawn")
        write.setStatus(process, NodeStatus::Running);
      else if (method == "command/exec/terminate" || method == "process/kill")
        write.setStatus(process, NodeStatus::Interrupted);
    }
    return;
  }

  if (method == "serverRequest/resolved") {
    const Value *requestId = member(message.payload, "requestId");
    const std::string canonical = canonicalValue(requestId);
    if (!canonical.empty()) {
      NodeRef interaction =
          write.find({NodeKind::Interaction, requestId && requestId->asString()
                                                 ? "string:" + canonical
                                                 : "number:" + canonical});
      if (interaction &&
          (!message.expectedNode || message.expectedNode == interaction))
        write.remove(interaction);
    }
    return;
  }

  if (method == "thread/deleted" ||
      (message.kind == DecodedMessageKind::ClientResult &&
       method == "thread/delete")) {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      if (NodeRef node = write.find({NodeKind::Thread, id}))
        removeThread(write, node);
    }
    return;
  }

  if (method == "thread/archived" || method == "thread/unarchived" ||
      method == "thread/closed" ||
      (message.kind == DecodedMessageKind::ClientResult &&
       (method == "thread/archive" || method == "thread/unarchive"))) {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      if (method == "thread/archived" || method == "thread/archive") {
        write.setField(thread, "archived", Value(true));
        write.setField(thread, "lifecycle", Value("archived"));
      } else if (method == "thread/unarchived" ||
                 method == "thread/unarchive") {
        write.setField(thread, "archived", Value(false));
        write.setField(thread, "lifecycle", Value("unarchived"));
        admitRootThread(write, thread, false);
      } else {
        write.setField(thread, "lifecycle", Value("closed"));
        write.setStatus(thread, NodeStatus::NotLoaded);
      }
    }
    return;
  }

  if (method == "thread/goal/updated" || method == "thread/goal/cleared") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      if (method == "thread/goal/cleared") {
        write.setField(thread, "goal", Value(nullptr));
        write.setField(thread, "goalTurnId", Value(nullptr));
      } else {
        if (const Value *goal = member(message.payload, "goal"))
          write.setField(thread, "goal", *goal);
        if (const Value *turnId = member(message.payload, "turnId"))
          write.setField(thread, "goalTurnId", *turnId);
      }
    }
    return;
  }

  if (method == "thread/queue/changed" || method == "thread/reverted") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      write.setField(thread,
                     method == "thread/queue/changed" ? "queueStale"
                                                      : "historyStale",
                     Value(true));
    }
    return;
  }

  if (method == "skills/changed" || method == "app/list/updated") {
    NodeRef catalog = write.upsert(
        {NodeKind::Catalog, method == "skills/changed" ? "skills" : "app"});
    write.setField(catalog, "stale", Value(true));
    write.setField(catalog, "invalidatedBy", Value(method));
    mergeObject(write, catalog, message.payload);
    return;
  }

  if (method == "project/changed") {
    const std::string id = addressedId(message.payload, NodeKind::Project);
    if (!id.empty()) {
      NodeRef project = write.upsert({NodeKind::Project, id});
      mergeObject(write, project, message.payload);
      write.setField(project, "stale", Value(true));
    }
    return;
  }

  if (method == "thread/project/updated") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      assignProject(write, thread, member(message.payload, "projectId"));
    }
    return;
  }

  if (message.kind == DecodedMessageKind::ClientResult &&
      method == "thread/section/move") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      assignSection(write, thread, member(message.payload, "sectionId"));
      if (const Value *enteredAt = member(message.payload, "sectionEnteredAt"))
        write.setField(thread, "sectionEnteredAt", *enteredAt);
    }
    return;
  }

  if (method == "thread/status/changed") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      if (const Value *status = member(message.payload, "status")) {
        write.setField(thread, "status", *status);
        const NodeStatus normalized = statusFromValue(status);
        if (normalized != NodeStatus::Unknown)
          write.setStatus(thread, normalized);
      }
    }
    return;
  }

  if (method == "thread/name/updated") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      if (const Value *name = member(message.payload, "threadName"))
        write.setField(thread, "name", *name);
    }
    return;
  }

  if (message.kind == DecodedMessageKind::ClientResult &&
      beginsWith(method, "project/")) {
    if (method == "project/list") {
      if (const Value::Array *projects = arrayMember(message.payload, "data")) {
        for (const Value &value : *projects) {
          const Value::Object *object = value.asObject();
          const std::string id = object ? nestedId(*object) : std::string{};
          if (!id.empty()) {
            NodeRef project = write.upsert({NodeKind::Project, id});
            mergeObject(write, project, *object);
          }
        }
      }
      return;
    }
    std::string id;
    const Value::Object *object = objectMember(message.payload, "project");
    if (object)
      id = nestedId(*object);
    if (id.empty())
      id = addressedId(message.payload, NodeKind::Project);
    if (id.empty())
      return;
    if (method == "project/delete") {
      if (NodeRef project = write.find({NodeKind::Project, id}))
        write.remove(project);
      return;
    }
    NodeRef project = write.upsert({NodeKind::Project, id});
    if (object)
      mergeObject(write, project, *object);
    else
      mergeObject(write, project, message.payload);
    return;
  }

  if (message.kind == DecodedMessageKind::ClientResult &&
      beginsWith(method, "threadSection/")) {
    if (method == "threadSection/list") {
      if (const Value::Array *sections = arrayMember(message.payload, "data")) {
        for (const Value &value : *sections) {
          const Value::Object *object = value.asObject();
          const std::string id = object ? nestedId(*object) : std::string{};
          if (!id.empty()) {
            NodeRef section = write.upsert({NodeKind::ThreadSection, id});
            mergeObject(write, section, *object);
          }
        }
      }
      return;
    }
    std::string id;
    const Value::Object *object = objectMember(message.payload, "section");
    if (object)
      id = nestedId(*object);
    if (id.empty())
      id = addressedId(message.payload, NodeKind::ThreadSection);
    if (id.empty())
      return;
    if (method == "threadSection/delete") {
      if (NodeRef section = write.find({NodeKind::ThreadSection, id}))
        write.remove(section);
      return;
    }
    NodeRef section = write.upsert({NodeKind::ThreadSection, id});
    if (object)
      mergeObject(write, section, *object);
    else
      mergeObject(write, section, message.payload);
    return;
  }

  if (message.kind == DecodedMessageKind::ClientResult &&
      method == "thread/list") {
    const Value::Array *threads = arrayMember(message.payload, "data");
    if (!threads)
      threads = arrayMember(message.payload, "threads");
    if (threads)
      replaceThreadList(write, *threads);
    return;
  }

  if (message.kind == DecodedMessageKind::ClientResult &&
      method == "thread/turns/list") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (id.empty())
      return;
    NodeRef thread = write.upsert({NodeKind::Thread, id});
    const std::vector<NodeRef> previous = write.children(thread);
    std::vector<NodeRef> page;
    const Value::Array *turns = arrayMember(message.payload, "data");
    if (!turns)
      turns = arrayMember(message.payload, "turns");
    if (turns) {
      page.reserve(turns->size());
      for (const Value &value : *turns) {
        const Value::Object *turnObject = value.asObject();
        if (!turnObject)
          continue;
        NodeRef turn = ingestTurn(write, *turnObject, thread, {}, true);
        if (turn && std::find(page.begin(), page.end(), turn) == page.end())
          page.emplace_back(std::move(turn));
      }
      write.replaceChildren(thread,
                            mergeExistingTail(std::move(page), previous));
    }

    const std::string nextCursor =
        canonicalValue(member(message.payload, "nextCursor"));
    write.setField(thread, "historyHasMore", Value(!nextCursor.empty()));
    if (nextCursor.empty())
      write.eraseField(thread, "historyNextCursor");
    else
      write.setField(thread, "historyNextCursor", Value(nextCursor));
    updateLoadedHistoryItemCount(write, thread);
    return;
  }

  if (isItemDeltaMethod(method)) {
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    const std::string turnId = addressedId(message.payload, NodeKind::Turn);
    const std::string itemId = addressedId(message.payload, NodeKind::Item);
    NodeRef thread;
    NodeRef turn;
    NodeRef item;
    bool historyMembershipMayChange = false;
    if (!threadId.empty())
      thread = write.upsert({NodeKind::Thread, threadId});
    if (!turnId.empty()) {
      const NodeId turnNodeId = scopedTurnNodeId(threadId, turnId);
      const NodeRef previousTurn = thread ? write.find(turnNodeId) : NodeRef{};
      const NodeRef previousParent =
          previousTurn ? write.parent(previousTurn) : NodeRef{};
      turn = ensureTurn(write, thread, turnId);
      if (turn) {
        historyMembershipMayChange |= previousParent != thread;
      }
    }
    if (!itemId.empty()) {
      const NodeId itemNodeId = turn ? scopedItemNodeId(turn->id(), itemId)
                                     : NodeId{NodeKind::Item, {}};
      const NodeRef previousItem = turn ? write.find(itemNodeId) : NodeRef{};
      const NodeRef previousParent =
          previousItem ? write.parent(previousItem) : NodeRef{};
      item = ensureItem(write, turn, itemId);
      if (item) {
        historyMembershipMayChange |= previousParent != turn;
        appendSemanticDelta(write, item, method, message.payload);
      }
    }
    if (historyMembershipMayChange)
      updateLoadedHistoryItemCount(write, thread);
    return;
  }

  NodeRef thread;
  NodeRef turn;
  NodeRef item;
  bool historyMembershipMayChange = false;
  if (const Value::Object *threadObject =
          objectMember(message.payload, "thread")) {
    historyMembershipMayChange = arrayMember(*threadObject, "turns") != nullptr;
    const bool replaceTurns =
        message.kind == DecodedMessageKind::ClientResult &&
        method == "thread/read";
    thread = ingestThread(write, *threadObject, {}, replaceTurns);
    if (thread && (method == "thread/started" ||
                   (message.kind == DecodedMessageKind::ClientResult &&
                    (method == "thread/start" || method == "thread/resume" ||
                     method == "thread/fork" || method == "thread/read"))))
      admitRootThread(write, thread, true);
  } else {
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    if (!threadId.empty())
      thread = write.upsert({NodeKind::Thread, threadId});
  }

  if (const Value::Object *turnObject = objectMember(message.payload, "turn")) {
    const std::string id = nestedId(*turnObject);
    const NodeRef previousTurn =
        id.empty() || !thread
            ? NodeRef{}
            : write.find(scopedTurnNodeId(thread->id().canonical, id));
    const NodeRef previousParent =
        previousTurn ? write.parent(previousTurn) : NodeRef{};
    historyMembershipMayChange |=
        arrayMember(*turnObject, "items") != nullptr ||
        (thread && previousParent != thread);
    turn = ingestTurn(write, *turnObject, thread);
    if (turn && method == "turn/started" &&
        write.state(turn)->status == NodeStatus::Unknown)
      write.setStatus(turn, NodeStatus::Running);
    if (turn && method == "turn/completed" &&
        statusFromValue(member(*turnObject, "status")) == NodeStatus::Unknown)
      write.setStatus(turn, NodeStatus::Completed);
  } else {
    const std::string id = addressedId(message.payload, NodeKind::Turn);
    if (!id.empty()) {
      const NodeRef previousTurn =
          thread ? write.find(scopedTurnNodeId(thread->id().canonical, id))
                 : NodeRef{};
      const NodeRef previousParent =
          previousTurn ? write.parent(previousTurn) : NodeRef{};
      turn = ensureTurn(write, thread, id);
      if (turn) {
        historyMembershipMayChange |= previousParent != thread;
      }
    }
  }

  if (const Value::Object *itemObject = objectMember(message.payload, "item")) {
    const std::string id = nestedId(*itemObject);
    const NodeRef previousItem =
        id.empty() || !turn ? NodeRef{}
                            : write.find(scopedItemNodeId(turn->id(), id));
    const NodeRef previousParent =
        previousItem ? write.parent(previousItem) : NodeRef{};
    historyMembershipMayChange |= turn && previousParent != turn;
    item = ingestItem(write, *itemObject, turn);
    if (item) {
      if (const Value *startedAt = member(message.payload, "startedAtMs"))
        write.setField(item, "startedAtMs", *startedAt);
      if (const Value *completedAt = member(message.payload, "completedAtMs"))
        write.setField(item, "completedAtMs", *completedAt);
    }
    if (item && method == "item/started" &&
        write.state(item)->status == NodeStatus::Unknown)
      write.setStatus(item, NodeStatus::Running);
    if (item && method == "item/completed" &&
        statusFromValue(member(*itemObject, "status")) == NodeStatus::Unknown)
      write.setStatus(item, NodeStatus::Completed);
  } else {
    const std::string id = addressedId(message.payload, NodeKind::Item);
    if (!id.empty()) {
      const NodeRef previousItem =
          turn ? write.find(scopedItemNodeId(turn->id(), id)) : NodeRef{};
      const NodeRef previousParent =
          previousItem ? write.parent(previousItem) : NodeRef{};
      item = ensureItem(write, turn, id);
      if (item) {
        historyMembershipMayChange |= previousParent != turn;
      }
    }
  }

  if (method == "turn/plan/updated" && turn) {
    if (const Value *explanation = member(message.payload, "explanation"))
      write.setField(turn, "planExplanation", *explanation);
    if (const Value *plan = member(message.payload, "plan"))
      write.setField(turn, "plan", *plan);
    if (historyMembershipMayChange)
      updateLoadedHistoryItemCount(write, thread);
    return;
  }

  if (method == "turn/diff/updated" && turn) {
    if (const Value *diff = member(message.payload, "diff"))
      write.setField(turn, "diff", *diff);
    if (historyMembershipMayChange)
      updateLoadedHistoryItemCount(write, thread);
    return;
  }

  if (!thread && !turn && !item) {
    const NodeKind kind = kindForMethod(method);
    std::string id = addressedId(message.payload, kind);
    if (id.empty())
      id = kind == NodeKind::Catalog ? catalogKey(method) : std::string(method);
    NodeRef node = write.upsert({kind, std::move(id)});
    if (method == "command/exec/outputDelta" ||
        method == "process/outputDelta") {
      const std::string delta =
          canonicalValue(member(message.payload, "delta"));
      if (!delta.empty())
        write.appendStringField(node, "output", delta);
    } else {
      mergeObject(write, node, message.payload);
    }
    return;
  }

  if (objectMember(message.payload, "thread") ||
      objectMember(message.payload, "turn") ||
      objectMember(message.payload, "item")) {
    if (historyMembershipMayChange)
      updateLoadedHistoryItemCount(write, thread);
    return;
  }

  NodeRef addressed = item ? item : (turn ? turn : thread);
  if (addressed)
    mergeObject(write, addressed, message.payload);
  if (historyMembershipMayChange)
    updateLoadedHistoryItemCount(write, thread);
}

void ProtocolUpdater::applyUnknown(NodeGraph::WriteAccess &write,
                                   const DecodedMessage &message) {
  const ProtocolDirection direction = catalogDirection(message.kind);
  const std::string id =
      std::to_string(static_cast<unsigned>(direction)) + ":" + message.method;
  NodeState state;
  state.fields.emplace("method", Value(message.method));
  state.fields.emplace("payload", Value(message.payload));
  state.fields.emplace("direction",
                       Value(static_cast<std::uint64_t>(direction)));
  NodeRef unknown = write.upsert({NodeKind::UnknownProtocol, id});
  write.replaceState(unknown, std::move(state));
}

NodeRef ProtocolUpdater::ingestThread(NodeGraph::WriteAccess &write,
                                      const Value::Object &object,
                                      std::string_view fallbackId,
                                      bool replaceTurns) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty())
    return {};
  NodeRef thread = write.upsert({NodeKind::Thread, id});
  mergeObject(write, thread, object, "turns");

  if (const Value *projectId = member(object, "projectId"))
    assignProject(write, thread, projectId);
  if (const Value *section = member(object, "section"))
    assignSection(write, thread, section);

  if (const Value *parentValue = member(object, "parentThreadId")) {
    const std::string parentId = canonicalValue(parentValue);
    if (!parentId.empty() && parentId != id) {
      NodeRef parent = write.upsert({NodeKind::Thread, parentId});
      assignThreadOwner(write, parent, RelationKind::StructuralChildThread,
                        thread);
    } else if (parentValue->isNull() || parentId.empty()) {
      const std::vector<NodeRef> nodes = write.orderedNodes();
      for (const NodeRef &candidate : nodes) {
        if (candidate->id().kind == NodeKind::Thread)
          write.unrelate(candidate, RelationKind::StructuralChildThread,
                         thread);
      }
    }
  }

  if (const Value *forkValue = member(object, "forkedFromId")) {
    clearThreadOwnerKind(write, thread, RelationKind::ForkChildThread);
    const std::string forkedFromId = canonicalValue(forkValue);
    if (!forkedFromId.empty() && forkedFromId != id) {
      NodeRef source = write.upsert({NodeKind::Thread, forkedFromId});
      write.relate(source, RelationKind::ForkChildThread, thread);
    }
  }

  if (const Value::Array *turns = arrayMember(object, "turns")) {
    std::vector<NodeRef> order;
    order.reserve(turns->size());
    for (const Value &value : *turns) {
      if (const Value::Object *turnObject = value.asObject()) {
        NodeRef turn = ingestTurn(write, *turnObject, thread, {}, replaceTurns);
        if (turn && std::find(order.begin(), order.end(), turn) == order.end())
          order.emplace_back(std::move(turn));
      }
    }
    if (replaceTurns)
      write.replaceChildren(thread, mergeLocalTail(write, std::move(order),
                                                   write.children(thread)));
    else if (!order.empty())
      write.replaceChildren(
          thread, mergeExistingTail(std::move(order), write.children(thread)));
  }
  return thread;
}

NodeRef ProtocolUpdater::ingestTurn(NodeGraph::WriteAccess &write,
                                    const Value::Object &object,
                                    const NodeRef &thread,
                                    std::string_view fallbackId,
                                    bool replaceItems) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty() || !thread)
    return {};
  NodeRef turn = ensureTurn(write, thread, id);
  mergeObject(write, turn, object, "items");
  if (const Value::Array *items = arrayMember(object, "items")) {
    std::vector<NodeRef> order;
    order.reserve(items->size());
    for (const Value &value : *items) {
      if (const Value::Object *itemObject = value.asObject()) {
        NodeRef item = ingestItem(write, *itemObject, turn);
        if (item && std::find(order.begin(), order.end(), item) == order.end())
          order.emplace_back(std::move(item));
      }
    }
    if (replaceItems)
      write.replaceChildren(
          turn, mergeLocalTail(write, std::move(order), write.children(turn)));
    else if (!order.empty())
      write.replaceChildren(
          turn, mergeExistingTail(std::move(order), write.children(turn)));
  }
  return turn;
}

NodeRef ProtocolUpdater::ingestItem(NodeGraph::WriteAccess &write,
                                    const Value::Object &object,
                                    const NodeRef &turn,
                                    std::string_view fallbackId) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty() || !turn)
    return {};
  NodeRef item = ensureItem(write, turn, id);
  mergeObject(write, item, object);
  correlateLocalPrompt(write, item, object);
  if (isUserMessage(object) &&
      write.related(turn, RelationKind::TurnRootItem).empty())
    write.relate(turn, RelationKind::TurnRootItem, item);

  const bool hasAgentChildren =
      member(object, "agentThreadId") || member(object, "receiverThreadIds");
  if (hasAgentChildren) {
    const std::vector<std::string> childThreadIds = agentChildIds(object);
    const std::vector<NodeRef> previous =
        write.related(item, RelationKind::AgentChildThread);
    std::vector<NodeRef> children;
    children.reserve(childThreadIds.size());
    for (const std::string &childThreadId : childThreadIds)
      children.emplace_back(write.upsert({NodeKind::Thread, childThreadId}));
    write.replaceRelated(item, RelationKind::AgentChildThread, children);
    NodeRef owner = turn ? write.parent(turn) : NodeRef{};
    if (owner) {
      for (const NodeRef &child : children)
        assignThreadOwner(write, owner, RelationKind::AgentChildThread, child);
    }
    for (const NodeRef &released : previous) {
      if (std::find(children.begin(), children.end(), released) ==
          children.end()) {
        clearThreadOwnerKind(write, released, RelationKind::AgentChildThread);
        if (!hasThreadOwner(write, released))
          admitRootThread(write, released, false);
      }
    }
  }
  return item;
}

void ProtocolUpdater::admitRootThread(NodeGraph::WriteAccess &write,
                                      const NodeRef &thread, bool prepend) {
  if (!thread || hasThreadOwner(write, thread))
    return;
  NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
  std::vector<NodeRef> roots = write.related(runtime, RelationKind::RootThread);
  roots.erase(std::remove(roots.begin(), roots.end(), thread), roots.end());
  if (prepend)
    roots.insert(roots.begin(), thread);
  else
    roots.emplace_back(thread);
  write.replaceRelated(runtime, RelationKind::RootThread, roots);
}

void ProtocolUpdater::replaceThreadList(NodeGraph::WriteAccess &write,
                                        const Value::Array &threads) {
  NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
  const std::vector<NodeRef> previous =
      write.related(runtime, RelationKind::RootThread);
  std::vector<NodeRef> listed;
  listed.reserve(threads.size());
  for (const Value &value : threads) {
    const Value::Object *object = value.asObject();
    if (!object)
      continue;
    NodeRef thread = ingestThread(write, *object);
    if (thread &&
        std::find(listed.begin(), listed.end(), thread) == listed.end())
      listed.emplace_back(std::move(thread));
  }

  std::vector<NodeRef> roots;
  roots.reserve(listed.size() + previous.size());
  for (const NodeRef &thread : listed) {
    if (!hasThreadOwner(write, thread))
      roots.emplace_back(thread);
  }
  for (const NodeRef &thread : previous) {
    if (!hasThreadOwner(write, thread) &&
        std::find(roots.begin(), roots.end(), thread) == roots.end())
      roots.emplace_back(thread);
  }
  write.replaceRelated(runtime, RelationKind::RootThread, roots);
}

void ProtocolUpdater::removeThread(NodeGraph::WriteAccess &write,
                                   const NodeRef &thread) {
  NodeRef runtime = write.find({NodeKind::Runtime, "runtime"});
  std::vector<NodeRef> roots =
      runtime ? write.related(runtime, RelationKind::RootThread)
              : std::vector<NodeRef>{};
  const auto rootPosition = std::find(roots.begin(), roots.end(), thread);
  const std::size_t insertion = rootPosition == roots.end()
                                    ? roots.size()
                                    : static_cast<std::size_t>(std::distance(
                                          roots.begin(), rootPosition));
  roots.erase(std::remove(roots.begin(), roots.end(), thread), roots.end());

  std::vector<NodeRef> promoted =
      write.related(thread, RelationKind::StructuralChildThread);
  promoted =
      mergeExistingTail(std::move(promoted),
                        write.related(thread, RelationKind::AgentChildThread));

  std::vector<NodeRef> descendants;
  std::unordered_set<const Node *> collected;
  const auto collect = [&](const auto &self, const NodeRef &node) -> void {
    std::vector<NodeRef> contained = write.children(node);
    if (node->id().kind == NodeKind::Turn) {
      contained =
          mergeExistingTail(std::move(contained),
                            write.related(node, RelationKind::TurnRootItem));
    }
    for (const NodeRef &child : contained) {
      if (!collected.insert(child.get()).second)
        continue;
      self(self, child);
      descendants.emplace_back(child);
    }
  };
  collect(collect, thread);
  for (const NodeRef &descendant : descendants)
    write.remove(descendant);
  write.remove(thread);

  std::size_t next = std::min(insertion, roots.size());
  for (const NodeRef &child : promoted) {
    if (hasThreadOwner(write, child) ||
        std::find(roots.begin(), roots.end(), child) != roots.end())
      continue;
    roots.insert(roots.begin() + static_cast<std::ptrdiff_t>(next), child);
    ++next;
  }
  if (runtime)
    write.replaceRelated(runtime, RelationKind::RootThread, roots);
}

} // namespace codexui::nodegraph
