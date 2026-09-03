// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolUpdater.h"

#include <array>
#include <charconv>
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
                 const Value::Object &object, std::string_view method) {
  NodeState next = *write.state(node);
  for (const auto &[key, value] : object)
    next.fields.insert_or_assign(key, value);
  next.fields.insert_or_assign("lastMethod", Value(method));
  const NodeStatus status = statusFromValue(member(object, "status"));
  if (status != NodeStatus::Unknown)
    next.status = status;
  write.replaceState(node, std::move(next));
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

bool isDeltaMethod(std::string_view method) {
  return method.ends_with("/delta") || method.ends_with("/outputDelta") ||
         method.ends_with("/progress") ||
         method == "mcpServer/event/stream/notification";
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

std::string payloadDelta(const Value::Object &payload) {
  constexpr std::array names{std::string_view("delta"),
                             std::string_view("text"),
                             std::string_view("output")};
  for (const std::string_view name : names) {
    if (const Value *value = member(payload, name)) {
      if (const std::string *text = value->asString())
        return *text;
    }
  }
  return {};
}

void relateAddressedNodes(NodeGraph::WriteAccess &write,
                          const Value::Object &payload, const NodeRef &thread,
                          const NodeRef &turn, const NodeRef &item) {
  if (thread && turn)
    write.setParent(thread, turn);
  if (turn && item)
    write.setParent(turn, item);
  const std::string childThreadId =
      canonicalValue(member(payload, "childThreadId"));
  if (!childThreadId.empty()) {
    NodeRef child = write.upsert({NodeKind::Thread, childThreadId});
    NodeRef source = item ? item : (turn ? turn : thread);
    if (source)
      write.relate(source, RelationKind::AgentChildThread, child);
  }
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
  write.setField(operation, "payload", Value(message.payload));
  if (message.kind == DecodedMessageKind::ClientRequest) {
    write.setStatus(operation, NodeStatus::Pending);
    return;
  }
  write.setStatus(operation, message.kind == DecodedMessageKind::ClientError
                                 ? NodeStatus::Failed
                                 : NodeStatus::Completed);

  if (message.kind == DecodedMessageKind::ClientResult)
    applyGraphUpdate(write, message);
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
  NodeRef target;
  if (!itemId.empty())
    target = write.upsert({NodeKind::Item, itemId});
  else if (!turnId.empty())
    target = write.upsert({NodeKind::Turn, turnId});
  else if (!threadId.empty())
    target = write.upsert({NodeKind::Thread, threadId});
  if (target)
    write.relate(interaction, RelationKind::InteractionTarget, target);
}

void ProtocolUpdater::applyGraphUpdate(NodeGraph::WriteAccess &write,
                                       const DecodedMessage &message) {
  const std::string_view method = message.method;

  if (method == "thread/deleted") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      if (NodeRef node = write.find({NodeKind::Thread, id}))
        write.remove(node);
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
      if (interaction)
        write.remove(interaction);
    }
    return;
  }

  NodeRef thread;
  NodeRef turn;
  NodeRef item;
  if (const Value::Object *threadObject =
          objectMember(message.payload, "thread"))
    thread = ingestThread(write, *threadObject);
  if (!thread) {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      thread = write.upsert({NodeKind::Thread, id});
      mergeObject(write, thread, message.payload, method);
    }
  }

  if (const Value::Object *turnObject = objectMember(message.payload, "turn"))
    turn = ingestTurn(write, *turnObject, thread);
  if (!turn) {
    const std::string id = addressedId(message.payload, NodeKind::Turn);
    if (!id.empty()) {
      turn = write.upsert({NodeKind::Turn, id});
      mergeObject(write, turn, message.payload, method);
    }
  }

  if (const Value::Object *itemObject = objectMember(message.payload, "item"))
    item = ingestItem(write, *itemObject, turn);
  if (!item) {
    const std::string id = addressedId(message.payload, NodeKind::Item);
    if (!id.empty()) {
      item = write.upsert({NodeKind::Item, id});
      if (isDeltaMethod(method)) {
        const std::string delta = payloadDelta(message.payload);
        if (!delta.empty())
          write.appendStringField(item, std::string(method), delta);
      }
      mergeObject(write, item, message.payload, method);
    }
  }

  relateAddressedNodes(write, message.payload, thread, turn, item);

  constexpr std::array listKeys{std::string_view("threads"),
                                std::string_view("data")};
  if (method == "thread/list" || method == "thread/loaded/list") {
    for (const std::string_view key : listKeys) {
      const Value::Array *threads = arrayMember(message.payload, key);
      if (!threads)
        continue;
      for (const Value &value : *threads) {
        if (const Value::Object *object = value.asObject())
          static_cast<void>(ingestThread(write, *object));
      }
      break;
    }
  }

  if (thread) {
    if (const Value::Array *turns = arrayMember(message.payload, "turns")) {
      for (const Value &value : *turns) {
        if (const Value::Object *object = value.asObject())
          static_cast<void>(ingestTurn(write, *object, thread));
      }
    }
  }

  if (!thread && !turn && !item) {
    const NodeKind kind = kindForMethod(method);
    std::string id = addressedId(message.payload, kind);
    if (id.empty())
      id = kind == NodeKind::Catalog ? catalogKey(method) : std::string(method);
    NodeRef node = write.upsert({kind, std::move(id)});
    if (isDeltaMethod(method)) {
      const std::string delta = payloadDelta(message.payload);
      if (!delta.empty())
        write.appendStringField(node, std::string(method), delta);
    }
    mergeObject(write, node, message.payload, method);
  }
}

void ProtocolUpdater::applyUnknown(NodeGraph::WriteAccess &write,
                                   const DecodedMessage &message) {
  const std::string id =
      message.method + "#" + std::to_string(++unknownSequence_);
  NodeState state;
  state.fields.emplace("method", Value(message.method));
  state.fields.emplace("payload", Value(message.payload));
  state.fields.emplace("direction", Value(static_cast<std::uint64_t>(
                                        catalogDirection(message.kind))));
  static_cast<void>(
      write.upsert({NodeKind::UnknownProtocol, id}, std::move(state)));
}

NodeRef ProtocolUpdater::ingestThread(NodeGraph::WriteAccess &write,
                                      const Value::Object &object,
                                      std::string_view fallbackId) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty())
    return {};
  NodeRef thread = write.upsert({NodeKind::Thread, id});
  mergeObject(write, thread, object, "thread/entity");
  if (const Value::Array *turns = arrayMember(object, "turns")) {
    for (const Value &value : *turns) {
      if (const Value::Object *turnObject = value.asObject())
        static_cast<void>(ingestTurn(write, *turnObject, thread));
    }
  }
  return thread;
}

NodeRef ProtocolUpdater::ingestTurn(NodeGraph::WriteAccess &write,
                                    const Value::Object &object,
                                    const NodeRef &thread,
                                    std::string_view fallbackId) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty())
    return {};
  NodeRef turn = write.upsert({NodeKind::Turn, id});
  mergeObject(write, turn, object, "turn/entity");
  if (thread)
    write.setParent(thread, turn);
  if (const Value::Array *items = arrayMember(object, "items")) {
    for (const Value &value : *items) {
      if (const Value::Object *itemObject = value.asObject())
        static_cast<void>(ingestItem(write, *itemObject, turn));
    }
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
  mergeObject(write, item, object, "item/entity");
  if (turn)
    write.setParent(turn, item);
  const std::string childThreadId =
      canonicalValue(member(object, "childThreadId"));
  if (!childThreadId.empty()) {
    NodeRef child = write.upsert({NodeKind::Thread, childThreadId});
    write.relate(item, RelationKind::AgentChildThread, child);
  }
  return item;
}

} // namespace codexui::nodegraph
