// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolUpdater.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>
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

std::vector<NodeRef> mergeExistingTail(std::vector<NodeRef> first,
                                       const std::vector<NodeRef> &existing) {
  for (const NodeRef &node : existing) {
    if (std::find(first.begin(), first.end(), node) == first.end())
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

std::string agentChildId(const Value::Object &item) {
  std::string child = canonicalValue(member(item, "agentThreadId"));
  if (!child.empty())
    return child;
  const Value::Array *receivers = arrayMember(item, "receiverThreadIds");
  if (!receivers || receivers->size() != 1)
    return {};
  return canonicalValue(&receivers->front());
}

} // namespace

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
    return ApplyResult{false, MessageDisposition::GraphUpdate, write.finish()};
  }

  if (descriptor->get().disposition ==
      MessageDisposition::IntentionallyStateNeutral) {
    return ApplyResult{true, descriptor->get().disposition,
                       GraphChange{graph_->publishedRevision(), {}, {}}};
  }

  auto write = graph_->write();
  switch (descriptor->get().disposition) {
  case MessageDisposition::WorkerOperationResult:
    applyOperation(write, message);
    break;
  case MessageDisposition::ReverseInteraction:
    applyInteraction(write, message);
    break;
  case MessageDisposition::GraphUpdate:
  case MessageDisposition::TypedUiEffect:
    applyGraphUpdate(write, message);
    break;
  case MessageDisposition::IntentionallyStateNeutral:
    break;
  }
  return ApplyResult{true, descriptor->get().disposition, write.finish()};
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

void ProtocolUpdater::applyOperation(NodeGraph::WriteAccess &write,
                                     const DecodedMessage &message) {
  if (message.kind == DecodedMessageKind::ClientNotification) {
    NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
    write.setField(runtime, "initialized", Value(true));
    write.setField(runtime, "lastMethod", Value(message.method));
    return;
  }

  const std::string operationId = message.requestId
                                      ? message.requestId->canonical()
                                      : "uncorrelated:" + message.method;
  NodeRef operation = write.upsert({NodeKind::Operation, operationId});
  write.setField(operation, "method", Value(message.method));
  if (message.kind == DecodedMessageKind::ClientRequest) {
    write.setField(operation, "requestPayload", Value(message.payload));
    write.setStatus(operation, NodeStatus::Pending);
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    const std::string turnId = addressedId(message.payload, NodeKind::Turn);
    const std::string itemId = addressedId(message.payload, NodeKind::Item);
    NodeRef target;
    if (!itemId.empty())
      target = write.upsert({NodeKind::Item, itemId});
    else if (!turnId.empty())
      target = write.upsert({NodeKind::Turn, turnId});
    else if (!threadId.empty())
      target = write.upsert({NodeKind::Thread, threadId});
    if (target)
      write.relate(operation, RelationKind::OperationTarget, target);
    return;
  }
  write.setField(operation,
                 message.kind == DecodedMessageKind::ClientError
                     ? "errorPayload"
                     : "resultPayload",
                 Value(message.payload));
  write.setStatus(operation, message.kind == DecodedMessageKind::ClientError
                                 ? NodeStatus::Failed
                                 : NodeStatus::Completed);

  if (message.kind == DecodedMessageKind::ClientResult) {
    DecodedMessage correlated = message;
    const auto operationState = write.state(operation);
    const auto request = operationState->fields.find("requestPayload");
    if (request != operationState->fields.end()) {
      if (const Value::Object *requestObject = request->second.asObject()) {
        for (const auto &[key, value] : *requestObject)
          correlated.payload.try_emplace(key, value);
      }
    }
    applyGraphUpdate(write, correlated);
  }
}

void ProtocolUpdater::applyInteraction(NodeGraph::WriteAccess &write,
                                       const DecodedMessage &message) {
  if (!message.requestId)
    throw std::invalid_argument("a server request requires an id");
  NodeRef interaction =
      write.upsert({NodeKind::Interaction, message.requestId->canonical()});
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
  if (!turnId.empty()) {
    turn = write.upsert({NodeKind::Turn, turnId});
    if (thread)
      write.setParent(thread, turn);
  }
  if (!itemId.empty()) {
    item = write.upsert({NodeKind::Item, itemId});
    if (turn)
      write.setParent(turn, item);
  }
  NodeRef target;
  if (item)
    target = item;
  else if (turn)
    target = turn;
  else if (thread)
    target = thread;
  if (target)
    write.relate(interaction, RelationKind::InteractionTarget, target);
}

void ProtocolUpdater::applyGraphUpdate(NodeGraph::WriteAccess &write,
                                       const DecodedMessage &message) {
  const std::string_view method = message.method;

  if (method == "serverRequest/resolved") {
    const Value *requestId = member(message.payload, "requestId");
    const std::string canonical = canonicalValue(requestId);
    if (!canonical.empty()) {
      NodeRef interaction =
          write.find({NodeKind::Interaction, requestId && requestId->asString()
                                                 ? "string:" + canonical
                                                 : "number:" + canonical});
      if (interaction)
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
      method == "thread/list") {
    const Value::Array *threads = arrayMember(message.payload, "data");
    if (!threads)
      threads = arrayMember(message.payload, "threads");
    if (threads)
      replaceThreadList(write, *threads);
    return;
  }

  if (isItemDeltaMethod(method)) {
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    const std::string turnId = addressedId(message.payload, NodeKind::Turn);
    const std::string itemId = addressedId(message.payload, NodeKind::Item);
    NodeRef thread;
    NodeRef turn;
    NodeRef item;
    if (!threadId.empty())
      thread = write.upsert({NodeKind::Thread, threadId});
    if (!turnId.empty()) {
      turn = write.upsert({NodeKind::Turn, turnId});
      if (thread)
        write.setParent(thread, turn);
    }
    if (!itemId.empty()) {
      item = write.upsert({NodeKind::Item, itemId});
      if (turn)
        write.setParent(turn, item);
      appendSemanticDelta(write, item, method, message.payload);
    }
    return;
  }

  NodeRef thread;
  NodeRef turn;
  NodeRef item;
  if (const Value::Object *threadObject =
          objectMember(message.payload, "thread")) {
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
    turn = ingestTurn(write, *turnObject, thread);
    if (turn && method == "turn/started" &&
        write.state(turn)->status == NodeStatus::Unknown)
      write.setStatus(turn, NodeStatus::Running);
    if (turn && method == "turn/completed" &&
        write.state(turn)->status == NodeStatus::Unknown)
      write.setStatus(turn, NodeStatus::Completed);
  } else {
    const std::string id = addressedId(message.payload, NodeKind::Turn);
    if (!id.empty()) {
      turn = write.upsert({NodeKind::Turn, id});
      if (thread)
        write.setParent(thread, turn);
    }
  }

  if (const Value::Object *itemObject = objectMember(message.payload, "item")) {
    item = ingestItem(write, *itemObject, turn);
    if (const Value *startedAt = member(message.payload, "startedAtMs"))
      write.setField(item, "startedAtMs", *startedAt);
    if (const Value *completedAt = member(message.payload, "completedAtMs"))
      write.setField(item, "completedAtMs", *completedAt);
    if (method == "item/started" &&
        write.state(item)->status == NodeStatus::Unknown)
      write.setStatus(item, NodeStatus::Running);
    if (method == "item/completed" &&
        write.state(item)->status == NodeStatus::Unknown)
      write.setStatus(item, NodeStatus::Completed);
  } else {
    const std::string id = addressedId(message.payload, NodeKind::Item);
    if (!id.empty()) {
      item = write.upsert({NodeKind::Item, id});
      if (turn)
        write.setParent(turn, item);
    }
  }

  if (method == "turn/plan/updated" && turn) {
    if (const Value *explanation = member(message.payload, "explanation"))
      write.setField(turn, "planExplanation", *explanation);
    if (const Value *plan = member(message.payload, "plan"))
      write.setField(turn, "plan", *plan);
    return;
  }

  if (method == "turn/diff/updated" && turn) {
    if (const Value *diff = member(message.payload, "diff"))
      write.setField(turn, "diff", *diff);
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
      objectMember(message.payload, "item"))
    return;

  NodeRef addressed = item ? item : (turn ? turn : thread);
  if (addressed)
    mergeObject(write, addressed, message.payload);
}

void ProtocolUpdater::applyUnknown(NodeGraph::WriteAccess &write,
                                   const DecodedMessage &message) {
  const ProtocolDirection direction = catalogDirection(message.kind);
  const std::string id = std::to_string(static_cast<unsigned>(direction)) +
                         ":" + message.method;
  NodeState state;
  state.fields.emplace("method", Value(message.method));
  state.fields.emplace("payload", Value(message.payload));
  state.fields.emplace(
      "direction", Value(static_cast<std::uint64_t>(direction)));
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
      write.replaceChildren(thread, order);
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
  if (id.empty())
    return {};
  NodeRef turn = write.upsert({NodeKind::Turn, id});
  mergeObject(write, turn, object, "items");
  if (thread)
    write.setParent(thread, turn);
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
      write.replaceChildren(turn, order);
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
  if (id.empty())
    return {};
  NodeRef item = write.upsert({NodeKind::Item, id});
  mergeObject(write, item, object);
  if (turn)
    write.setParent(turn, item);

  const std::string childThreadId = agentChildId(object);
  if (!childThreadId.empty()) {
    NodeRef child = write.upsert({NodeKind::Thread, childThreadId});
    const std::vector<NodeRef> previous =
        write.related(item, RelationKind::AgentChildThread);
    std::array<NodeRef, 1> onlyChild{child};
    write.replaceRelated(item, RelationKind::AgentChildThread, onlyChild);
    NodeRef owner = turn ? write.parent(turn) : NodeRef{};
    if (owner)
      assignThreadOwner(write, owner, RelationKind::AgentChildThread, child);
    for (const NodeRef &released : previous) {
      if (released != child) {
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
  const auto collect = [&](const auto &self, const NodeRef &node) -> void {
    for (const NodeRef &child : write.children(node)) {
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
