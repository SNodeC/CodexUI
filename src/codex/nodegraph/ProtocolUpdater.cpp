// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolUpdater.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace codexui::nodegraph {
namespace {

constexpr std::size_t MaximumRetainedStreamBytes = 256 * 1024;
constexpr std::size_t RetainedStreamTailBytes = 192 * 1024;
constexpr std::size_t MaximumIndexedTextParts = 4096;

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

bool isLocalPrompt(NodeGraph::WriteAccess &write, const NodeRef &node) {
  if (!node || node->id().kind != NodeKind::Item)
    return false;
  const std::shared_ptr<const NodeState> state = write.state(node);
  const Value *type = member(state->fields, "type");
  return type && type->asString() && *type->asString() == "localPrompt";
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
  if (const Value::Object *object = value ? value->asObject() : nullptr)
    value = member(*object, "type");
  const std::string *status = value ? value->asString() : nullptr;
  if (!status)
    return NodeStatus::Unknown;
  if (*status == "pending" || *status == "queued")
    return NodeStatus::Pending;
  if (*status == "running" || *status == "inProgress" || *status == "active")
    return NodeStatus::Running;
  if (*status == "completed" || *status == "complete" ||
      *status == "succeeded" || *status == "idle")
    return NodeStatus::Completed;
  if (*status == "failed" || *status == "error" || *status == "systemError")
    return NodeStatus::Failed;
  if (*status == "blocked")
    return NodeStatus::Failed;
  if (*status == "interrupted" || *status == "cancelled" ||
      *status == "stopped")
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
                 std::string_view omittedChildField = {},
                 std::optional<std::uint64_t> preserveChangesAfter = {}) {
  NodeState next = *write.state(node);
  for (const auto &[key, value] : object) {
    if (!omittedChildField.empty() && key == omittedChildField)
      continue;
    if (preserveChangesAfter &&
        write.fieldChangedRevision(node, key) > *preserveChangesAfter)
      continue;
    next.fields.insert_or_assign(key, value);
  }
  if (const Value *status = member(object, "status");
      status && (!preserveChangesAfter ||
                 write.statusChangedRevision(node) <= *preserveChangesAfter))
    next.status = statusFromValue(status);
  write.replaceState(node, std::move(next));
}

std::uint64_t unsignedValue(const Value *value) noexcept {
  if (const std::uint64_t *number = value ? value->asUInt64() : nullptr)
    return *number;
  if (const std::int64_t *number = value ? value->asInt64() : nullptr;
      number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return 0;
}

std::size_t utf8TailStart(std::string_view value,
                          std::size_t retainedBytes) noexcept {
  if (value.size() <= retainedBytes)
    return 0;
  std::size_t start = value.size() - retainedBytes;
  while (start < value.size() &&
         (static_cast<unsigned char>(value[start]) & 0xc0U) == 0x80U)
    ++start;
  return start;
}

Value::Object *textRetention(NodeState &state) {
  Value &retention = state.fields["textRetention"];
  if (!retention.isObject())
    retention = Value::Object{};
  return retention.asObject();
}

const Value::Object *textRetention(const NodeState &state) {
  const Value *retention = member(state.fields, "textRetention");
  return retention ? retention->asObject() : nullptr;
}

bool hasTextRetention(const NodeState &state, std::string_view field) {
  const Value::Object *retention = textRetention(state);
  return retention && member(*retention, field);
}

void clearTextRetention(NodeState &state, std::string_view field) {
  Value *retentionValue = nullptr;
  if (auto found = state.fields.find("textRetention");
      found != state.fields.end())
    retentionValue = &found->second;
  Value::Object *retention =
      retentionValue ? retentionValue->asObject() : nullptr;
  if (!retention)
    return;
  retention->erase(std::string(field));
  if (retention->empty())
    state.fields.erase("textRetention");
}

void updateTextRetention(NodeState &state, std::string_view field,
                         std::size_t retainedBytes,
                         std::size_t newlyDiscardedBytes) {
  Value::Object *retention = textRetention(state);
  Value &entryValue = (*retention)[std::string(field)];
  if (!entryValue.isObject())
    entryValue = Value::Object{};
  Value::Object &entry = *entryValue.asObject();
  const std::uint64_t previous = unsignedValue(member(entry, "discardedBytes"));
  const std::uint64_t increment =
      static_cast<std::uint64_t>(newlyDiscardedBytes);
  const std::uint64_t discarded =
      increment > std::numeric_limits<std::uint64_t>::max() - previous
          ? std::numeric_limits<std::uint64_t>::max()
          : previous + increment;
  entry.insert_or_assign("discardedBytes", Value(discarded));
  entry.insert_or_assign("retainedBytes",
                         Value(static_cast<std::uint64_t>(retainedBytes)));
}

void boundScalarText(NodeState &state, std::string_view field) {
  const auto found = state.fields.find(field);
  std::string *value =
      found == state.fields.end() ? nullptr : found->second.asString();
  if (!value)
    return;
  std::size_t discarded = 0;
  if (value->size() > MaximumRetainedStreamBytes) {
    discarded = utf8TailStart(*value, RetainedStreamTailBytes);
    value->erase(0, discarded);
  }
  if (discarded != 0 || hasTextRetention(state, field))
    updateTextRetention(state, field, value->size(), discarded);
}

void boundIndexedText(NodeState &state, std::string_view field) {
  const auto found = state.fields.find(field);
  Value::Array *parts =
      found == state.fields.end() ? nullptr : found->second.asArray();
  if (!parts)
    return;

  std::uint64_t total = 0;
  for (const Value &part : *parts) {
    const std::string *text = part.asString();
    if (!text)
      continue;
    const std::uint64_t bytes = static_cast<std::uint64_t>(text->size());
    total = bytes > std::numeric_limits<std::uint64_t>::max() - total
                ? std::numeric_limits<std::uint64_t>::max()
                : total + bytes;
  }

  std::size_t discarded = 0;
  if (total > MaximumRetainedStreamBytes) {
    std::uint64_t toDiscard = total - RetainedStreamTailBytes;
    for (Value &part : *parts) {
      std::string *text = part.asString();
      if (!text || text->empty() || toDiscard == 0)
        continue;
      std::size_t count = 0;
      if (toDiscard >= text->size()) {
        count = text->size();
      } else {
        count = utf8TailStart(*text, text->size() -
                                         static_cast<std::size_t>(toDiscard));
      }
      text->erase(0, count);
      toDiscard -= std::min<std::uint64_t>(toDiscard, count);
      discarded += count;
    }
  }

  std::size_t retained = 0;
  for (const Value &part : *parts)
    if (const std::string *text = part.asString())
      retained += text->size();
  if (discarded != 0 || hasTextRetention(state, field))
    updateTextRetention(state, field, retained, discarded);
}

void boundRetainedItemText(
    NodeGraph::WriteAccess &write, const NodeRef &item,
    const Value::Object &incoming,
    std::optional<std::uint64_t> preserveChangesAfter = {}) {
  NodeState next = *write.state(item);
  for (const std::string_view field :
       {std::string_view("text"), std::string_view("output"),
        std::string_view("aggregatedOutput"), std::string_view("summary"),
        std::string_view("content")}) {
    if (incoming.contains(field) &&
        (!preserveChangesAfter ||
         write.fieldChangedRevision(item, field) <= *preserveChangesAfter))
      clearTextRetention(next, field);
  }

  const std::string type = canonicalValue(member(next.fields, "type"));
  if (type == "commandExecution") {
    boundScalarText(next, "aggregatedOutput");
    boundScalarText(next, "output");
  } else if (type == "agentMessage" || type == "plan") {
    boundScalarText(next, "text");
  } else if (type == "reasoning") {
    boundIndexedText(next, "summary");
    boundIndexedText(next, "content");
  } else if (type == "fileChange") {
    boundScalarText(next, "output");
  } else if (type != "userMessage") {
    boundScalarText(next, "text");
    boundScalarText(next, "output");
    boundScalarText(next, "aggregatedOutput");
    boundIndexedText(next, "summary");
    boundIndexedText(next, "content");
  }
  write.replaceState(item, std::move(next));
}

void appendBoundedStringField(NodeGraph::WriteAccess &write,
                              const NodeRef &node, std::string field,
                              std::string_view suffix) {
  NodeState next = *write.state(node);
  Value &stored = next.fields[field];
  if (!stored.isString())
    stored = std::string{};
  std::string &existing = *stored.asString();

  std::size_t discarded = 0;
  if (suffix.size() > MaximumRetainedStreamBytes) {
    const std::size_t start = utf8TailStart(suffix, RetainedStreamTailBytes);
    discarded = existing.size() + start;
    existing.assign(suffix.substr(start));
  } else {
    existing.append(suffix);
    if (existing.size() > MaximumRetainedStreamBytes) {
      discarded = utf8TailStart(existing, RetainedStreamTailBytes);
      existing.erase(0, discarded);
    }
  }
  const std::size_t retained = existing.size();
  if (discarded != 0 || hasTextRetention(next, field))
    updateTextRetention(next, field, retained, discarded);
  write.replaceState(node, std::move(next));
}

void mergeSparseRateLimitFields(Value::Object &current,
                                const Value::Object &patch) {
  for (const auto &[key, value] : patch) {
    const auto found = current.find(key);
    const Value::Object *patchObject = value.asObject();
    if (found != current.end() && patchObject) {
      if (Value::Object *currentObject = found->second.asObject()) {
        mergeSparseRateLimitFields(*currentObject, *patchObject);
        continue;
      }
    }
    current.insert_or_assign(key, value);
  }
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
  NodeId id = scopedTurnNodeId(thread->id().canonical, rawTurnId);
  NodeRef turn = write.find(id);
  if (turn) {
    const std::shared_ptr<const NodeState> current = write.state(turn);
    if (canonicalValue(member(current->fields, "protocolId")) != rawTurnId ||
        canonicalValue(member(current->fields, "protocolThreadId")) !=
            thread->id().canonical) {
      NodeState next = *current;
      next.fields.insert_or_assign("protocolId", Value(rawTurnId));
      next.fields.insert_or_assign("protocolThreadId",
                                   Value(thread->id().canonical));
      write.replaceState(turn, std::move(next));
    }
  } else {
    NodeState next;
    next.fields = {{"protocolId", Value(rawTurnId)},
                   {"protocolThreadId", Value(thread->id().canonical)}};
    turn = write.upsert(std::move(id), std::move(next));
  }
  write.setParent(thread, turn);
  return turn;
}

bool isActiveTurn(const NodeState &state) {
  const std::string status = canonicalValue(member(state.fields, "status"));
  return state.status == NodeStatus::Running || status == "active" ||
         status == "running" || status == "inProgress";
}

void updateActiveTurn(NodeGraph::WriteAccess &write, const NodeRef &thread,
                      const NodeRef &turn) {
  if (!thread || !turn)
    return;
  if (isActiveTurn(*write.state(turn))) {
    const std::array<NodeRef, 1> active{turn};
    write.replaceRelated(thread, RelationKind::ActiveTurn, active);
    return;
  }
  write.unrelate(thread, RelationKind::ActiveTurn, turn);
}

void refreshActiveTurn(NodeGraph::WriteAccess &write, const NodeRef &thread) {
  NodeRef active;
  const std::vector<NodeRef> turns = write.children(thread);
  for (auto iterator = turns.rbegin(); iterator != turns.rend(); ++iterator) {
    if (*iterator && (*iterator)->id().kind == NodeKind::Turn &&
        isActiveTurn(*write.state(*iterator))) {
      active = *iterator;
      break;
    }
  }
  if (active) {
    const std::array<NodeRef, 1> current{std::move(active)};
    write.replaceRelated(thread, RelationKind::ActiveTurn, current);
  } else {
    write.replaceRelated(thread, RelationKind::ActiveTurn,
                         std::span<const NodeRef>{});
  }
}

void mergeEffectiveThreadSettings(NodeGraph::WriteAccess &write,
                                  const NodeRef &thread,
                                  const Value::Object &settings) {
  if (!thread)
    return;
  if (settings.contains("effort"))
    write.eraseField(thread, "reasoningEffort");
  if (settings.contains("reasoningEffort"))
    write.eraseField(thread, "effort");
  if (settings.contains("sandboxPolicy"))
    write.eraseField(thread, "sandbox");
  if (settings.contains("sandbox"))
    write.eraseField(thread, "sandboxPolicy");
  for (const auto &[key, value] : settings) {
    if (value.isNull())
      write.eraseField(thread, key);
    else
      write.setField(thread, key, value);
  }
}

void mergeThreadResultSettings(NodeGraph::WriteAccess &write,
                               const NodeRef &thread,
                               const Value::Object &result) {
  Value::Object settings;
  for (const std::string_view key :
       {std::string_view("approvalPolicy"),
        std::string_view("approvalsReviewer"), std::string_view("cwd"),
        std::string_view("instructionSources"), std::string_view("model"),
        std::string_view("modelProvider"), std::string_view("reasoningEffort"),
        std::string_view("sandbox"), std::string_view("serviceTier"),
        std::string_view("activePermissionProfile")}) {
    if (const Value *value = member(result, key))
      settings.emplace(key, *value);
  }
  mergeEffectiveThreadSettings(write, thread, settings);
}

NodeRef ensureItem(NodeGraph::WriteAccess &write, const NodeRef &turn,
                   std::string_view rawItemId) {
  if (!turn || turn->id().kind != NodeKind::Turn || rawItemId.empty())
    return {};
  NodeId id = scopedItemNodeId(turn->id(), rawItemId);
  NodeRef item = write.find(id);
  const std::string turnId = retainedProtocolId(write, turn);
  const NodeRef thread = write.parent(turn);
  const std::string threadId = thread ? thread->id().canonical : std::string{};
  if (item) {
    const std::shared_ptr<const NodeState> current = write.state(item);
    const bool protocolChanged =
        canonicalValue(member(current->fields, "protocolId")) != rawItemId ||
        canonicalValue(member(current->fields, "protocolTurnId")) != turnId ||
        (!threadId.empty() &&
         canonicalValue(member(current->fields, "protocolThreadId")) !=
             threadId);
    if (protocolChanged) {
      NodeState next = *current;
      next.fields.insert_or_assign("protocolId", Value(rawItemId));
      next.fields.insert_or_assign("protocolTurnId", Value(turnId));
      if (!threadId.empty())
        next.fields.insert_or_assign("protocolThreadId", Value(threadId));
      write.replaceState(item, std::move(next));
    }
  } else {
    NodeState next;
    next.fields = {{"protocolId", Value(rawItemId)},
                   {"protocolTurnId", Value(turnId)}};
    if (!threadId.empty())
      next.fields.emplace("protocolThreadId", Value(threadId));
    item = write.upsert(std::move(id), std::move(next));
  }
  write.setParent(turn, item);
  return item;
}

NodeRef applyHookNotification(NodeGraph::WriteAccess &write,
                              const DecodedMessage &message) {
  const Value::Object *run = objectMember(message.payload, "run");
  const std::string runId =
      run ? canonicalValue(member(*run, "id")) : std::string{};
  if (runId.empty())
    return {};

  NodeRef hook = write.upsert({NodeKind::Hook, runId});
  mergeObject(write, hook, *run);
  write.setField(hook, "protocolId", Value(runId));
  write.setField(hook, "lastMethod", Value(message.method));

  const Value *threadValue = member(message.payload, "threadId");
  const Value *turnValue = member(message.payload, "turnId");
  const std::string threadId = canonicalValue(threadValue);
  const std::string turnId = canonicalValue(turnValue);
  if (threadId.empty()) {
    write.eraseField(hook, "threadId");
    write.eraseField(hook, "protocolThreadId");
  } else {
    write.setField(hook, "threadId", Value(threadId));
    write.setField(hook, "protocolThreadId", Value(threadId));
  }
  if (turnId.empty()) {
    write.eraseField(hook, "turnId");
    write.eraseField(hook, "protocolTurnId");
  } else {
    write.setField(hook, "turnId", Value(turnId));
    write.setField(hook, "protocolTurnId", Value(turnId));
  }

  if (!threadId.empty()) {
    NodeRef thread = write.upsert({NodeKind::Thread, threadId});
    NodeRef owner = thread;
    if (!turnId.empty())
      if (NodeRef turn = ensureTurn(write, thread, turnId))
        owner = std::move(turn);
    write.setParent(owner, hook);
  }

  const Value *rawStatus = member(*run, "status");
  NodeStatus status = statusFromValue(rawStatus);
  if (status == NodeStatus::Unknown) {
    const bool started = message.method == "hook/started";
    status = started ? NodeStatus::Running : NodeStatus::Completed;
    if (!rawStatus)
      write.setField(hook, "status", Value(started ? "running" : "completed"));
  }
  write.setStatus(hook, status);
  return hook;
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
  if (index >= MaximumIndexedTextParts)
    return;
  NodeState next = *write.state(node);
  Value &stored = next.fields[field];
  if (!stored.isArray())
    stored = Value::Array{};
  Value::Array &parts = *stored.asArray();
  if (parts.size() <= index)
    parts.resize(index + 1);
  if (!parts[index].isString())
    parts[index] = std::string{};
  parts[index].asString()->append(suffix);
  boundIndexedText(next, field);
  write.replaceState(node, std::move(next));
}

void appendSemanticDelta(NodeGraph::WriteAccess &write, const NodeRef &item,
                         std::string_view method,
                         const Value::Object &payload) {
  if (method == "item/reasoning/summaryPartAdded") {
    appendIndexedField(write, item, "summary",
                       indexValue(member(payload, "summaryIndex")).value_or(0),
                       {});
    return;
  }
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
    appendBoundedStringField(write, item, "aggregatedOutput", delta);
    return;
  }
  if (method == "item/fileChange/outputDelta") {
    appendBoundedStringField(write, item, "output", delta);
    return;
  }
  appendBoundedStringField(write, item, "text", delta);
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
         method == "item/reasoning/summaryPartAdded" ||
         method == "item/reasoning/textDelta" ||
         method == "item/commandExecution/outputDelta" ||
         method == "item/fileChange/outputDelta";
}

bool isProviderNoticeMethod(std::string_view method) {
  return method == "error" || method == "warning" ||
         method == "guardianWarning" || method == "deprecationNotice" ||
         method == "configWarning" || method == "windows/worldWritableWarning";
}

std::string mcpServerNodeKey(std::string_view method,
                             const Value::Object &payload) {
  if (method == "mcpServer/event/stream/notification") {
    const std::string subscription =
        canonicalValue(member(payload, "subscriptionId"));
    return subscription.empty() ? std::string(method)
                                : scopedCanonical("subscription", subscription);
  }

  const std::string name = canonicalValue(member(payload, "name"));
  if (name.empty())
    return std::string(method);
  const std::string threadId = canonicalValue(member(payload, "threadId"));
  return scopedCanonical(threadId.empty() ? std::string_view("global")
                                          : std::string_view(threadId),
                         name);
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

struct CatalogEntitySeed final {
  NodeKind kind = NodeKind::CatalogEntry;
  const Value::Object *fields = nullptr;
  std::string scope;
};

std::string catalogEntityId(const Value::Object &fields) {
  constexpr std::array names{
      std::string_view("id"),         std::string_view("model"),
      std::string_view("key"),        std::string_view("name"),
      std::string_view("path"),       std::string_view("connectorId"),
      std::string_view("runtimeName")};
  return firstId(fields, names);
}

void appendDirectCatalogEntries(std::vector<CatalogEntitySeed> &entries,
                                const Value::Array *values, NodeKind kind,
                                std::string_view scope = {}) {
  if (!values)
    return;
  entries.reserve(entries.size() + values->size());
  for (const Value &value : *values) {
    if (const Value::Object *object = value.asObject())
      entries.push_back({kind, object, std::string(scope)});
  }
}

// Catalog envelopes retain cursors/errors on their Catalog node. Concrete
// rows live as ordered child nodes so declared entity kinds are not opaque
// blobs and stable NodeRefs survive ordinary refreshes.
bool applyNaturalCatalogSnapshot(NodeGraph::WriteAccess &write,
                                 const DecodedMessage &message) {
  const std::string_view method = message.method;
  const bool appNotification =
      message.kind == DecodedMessageKind::ServerNotification &&
      method == "app/list/updated";
  if (message.kind != DecodedMessageKind::ClientResult && !appNotification)
    return false;

  std::string_view catalogId;
  NodeKind directKind = NodeKind::CatalogEntry;
  const Value::Array *directEntries = nullptr;
  std::vector<CatalogEntitySeed> entries;

  if (method == "model/list") {
    catalogId = "model";
    directEntries = arrayMember(message.payload, "data");
  } else if (method == "permissionProfile/list") {
    catalogId = "permissionProfile";
    directKind = NodeKind::PermissionProfile;
    directEntries = arrayMember(message.payload, "data");
  } else if (method == "experimentalFeature/list") {
    catalogId = "experimentalFeature";
    directEntries = arrayMember(message.payload, "data");
  } else if (method == "collaborationMode/list") {
    catalogId = "collaborationMode";
    directEntries = arrayMember(message.payload, "data");
  } else if (method == "mcpServerStatus/list") {
    catalogId = "mcpServer";
    directKind = NodeKind::McpServer;
    directEntries = arrayMember(message.payload, "data");
  } else if (method == "app/list" || appNotification) {
    catalogId = "app";
    directKind = NodeKind::App;
    directEntries = arrayMember(message.payload, "data");
    if (appNotification && !directEntries)
      return false;
  } else if (method == "skills/list") {
    catalogId = "skills";
    if (const Value::Array *groups = arrayMember(message.payload, "data")) {
      for (const Value &value : *groups) {
        const Value::Object *group = value.asObject();
        if (!group)
          continue;
        appendDirectCatalogEntries(entries, arrayMember(*group, "skills"),
                                   NodeKind::Skill,
                                   canonicalValue(member(*group, "cwd")));
      }
    }
  } else if (method == "hooks/list") {
    catalogId = "hooks";
    if (const Value::Array *groups = arrayMember(message.payload, "data")) {
      for (const Value &value : *groups) {
        const Value::Object *group = value.asObject();
        if (!group)
          continue;
        appendDirectCatalogEntries(entries, arrayMember(*group, "hooks"),
                                   NodeKind::Hook,
                                   canonicalValue(member(*group, "cwd")));
      }
    }
  } else if (method == "plugin/list") {
    catalogId = "plugin";
    if (const Value::Array *marketplaces =
            arrayMember(message.payload, "marketplaces")) {
      for (const Value &value : *marketplaces) {
        const Value::Object *marketplace = value.asObject();
        if (!marketplace)
          continue;
        std::string scope = canonicalValue(member(*marketplace, "name"));
        if (scope.empty())
          scope = canonicalValue(member(*marketplace, "path"));
        appendDirectCatalogEntries(entries,
                                   arrayMember(*marketplace, "plugins"),
                                   NodeKind::Plugin, scope);
      }
    }
  } else {
    return false;
  }

  appendDirectCatalogEntries(entries, directEntries, directKind);
  NodeRef catalog = write.upsert({NodeKind::Catalog, std::string(catalogId)});
  const std::vector<NodeRef> previous = write.children(catalog);
  mergeObject(write, catalog, message.payload);
  write.setField(catalog, "lastMethod", Value(std::string(method)));
  write.eraseField(catalog, "stale");
  write.eraseField(catalog, "invalidatedBy");

  const bool continuation =
      !canonicalValue(member(message.payload, "cursor")).empty();
  std::vector<NodeRef> ordered =
      continuation ? previous : std::vector<NodeRef>{};
  ordered.reserve(ordered.size() + entries.size());
  std::unordered_set<const Node *> retained;
  retained.reserve(ordered.size() + entries.size());
  for (const NodeRef &entry : ordered)
    retained.insert(entry.get());

  std::size_t anonymous = 0;
  for (const CatalogEntitySeed &seed : entries) {
    if (!seed.fields)
      continue;
    std::string protocolId = catalogEntityId(*seed.fields);
    if (protocolId.empty())
      protocolId = "anonymous:" + std::to_string(anonymous++);
    std::string owner(catalogId);
    if (!seed.scope.empty()) {
      owner += ':';
      owner += seed.scope;
    }
    NodeState state;
    state.status = statusFromValue(member(*seed.fields, "status"));
    state.fields = *seed.fields;
    state.fields.insert_or_assign("protocolId", Value(protocolId));
    state.fields.insert_or_assign("catalog", Value(std::string(catalogId)));
    if (!seed.scope.empty())
      state.fields.insert_or_assign("catalogScope", Value(seed.scope));
    NodeId id{seed.kind, scopedCanonical(owner, protocolId)};
    NodeRef entry = write.find(id);
    if (entry)
      write.replaceState(entry, std::move(state));
    else
      entry = write.upsert(std::move(id), std::move(state));
    if (retained.insert(entry.get()).second)
      ordered.emplace_back(std::move(entry));
  }

  write.replaceChildren(catalog, ordered);
  if (!continuation) {
    std::vector<NodeRef> omitted;
    omitted.reserve(previous.size());
    for (const NodeRef &entry : previous)
      if (entry && !retained.contains(entry.get()))
        omitted.emplace_back(entry);
    write.removeMany(omitted);
  }
  return true;
}

bool applyAutoApprovalReview(NodeGraph::WriteAccess &write,
                             const DecodedMessage &message) {
  const bool started = message.method == "item/autoApprovalReview/started";
  const bool completed = message.method == "item/autoApprovalReview/completed";
  if (!started && !completed)
    return false;

  const std::string threadId = addressedId(message.payload, NodeKind::Thread);
  const std::string turnId = addressedId(message.payload, NodeKind::Turn);
  if (threadId.empty() || turnId.empty())
    return true;
  NodeRef thread = write.upsert({NodeKind::Thread, threadId});
  NodeRef turn = ensureTurn(write, thread, turnId);

  const std::string targetId =
      canonicalValue(member(message.payload, "targetItemId"));
  NodeRef target = ensureItem(write, turn, targetId);
  std::string reviewId = canonicalValue(member(message.payload, "reviewId"));
  if (reviewId.empty() && !targetId.empty())
    reviewId = "auto-review:" + targetId;
  if (reviewId.empty()) {
    write.setField(turn, "lastAutoApprovalReview", Value(message.payload));
    return true;
  }

  NodeRef review = ensureItem(write, turn, reviewId);
  mergeObject(write, review, message.payload);
  write.setField(review, "type", Value("autoApprovalReview"));
  write.setField(review, "phase", Value(started ? "started" : "completed"));
  write.setField(review, "protocolId", Value(reviewId));
  write.setStatus(review,
                  started ? NodeStatus::Running : NodeStatus::Completed);
  if (target) {
    const std::array<NodeRef, 1> targets{target};
    write.replaceRelated(review, RelationKind::ReviewTarget, targets);
  } else {
    write.replaceRelated(review, RelationKind::ReviewTarget,
                         std::span<const NodeRef>{});
  }
  return true;
}

NodeRef containingThread(NodeGraph::WriteAccess &write, NodeRef node) {
  while (node && node->id().kind != NodeKind::Thread)
    node = write.parent(node);
  return node;
}

void refreshPendingInteractionCount(NodeGraph::WriteAccess &write,
                                    const NodeRef &thread) {
  if (!thread)
    return;
  std::size_t pending = 0;
  for (const NodeRef &interaction :
       write.related(thread, RelationKind::PendingInteraction)) {
    if (interaction &&
        (write.state(interaction)->status == NodeStatus::Pending ||
         write.state(interaction)->status == NodeStatus::Failed))
      ++pending;
  }
  write.setField(thread, "pendingInteractionCount",
                 Value(static_cast<std::uint64_t>(pending)));
}

void clearPendingInteractionOwner(NodeGraph::WriteAccess &write,
                                  const NodeRef &interaction) {
  if (NodeRef runtime = write.find({NodeKind::Runtime, "runtime"}))
    write.unrelate(runtime, RelationKind::PendingInteraction, interaction);
  for (const NodeRef &target :
       write.related(interaction, RelationKind::InteractionTarget)) {
    if (NodeRef thread = containingThread(write, target)) {
      write.unrelate(thread, RelationKind::PendingInteraction, interaction);
      refreshPendingInteractionCount(write, thread);
    }
  }
}

NodeRef newestInteractionForRequest(NodeGraph::WriteAccess &write,
                                    std::string_view requestId) {
  const auto &nodes = write.orderedNodes();
  for (auto node = nodes.rbegin(); node != nodes.rend(); ++node) {
    if (!*node || (*node)->id().kind != NodeKind::Interaction)
      continue;
    if (canonicalValue(member(write.state(*node)->fields, "requestId")) ==
        requestId)
      return *node;
  }
  return {};
}

bool interactionGenerationDiffers(const NodeState &state,
                                  const DecodedMessage &message) {
  if (message.connectionGeneration) {
    const Value *stored = member(state.fields, "connectionGeneration");
    if (!stored || unsignedValue(stored) != *message.connectionGeneration)
      return true;
  }
  if (message.providerGeneration) {
    const Value *stored = member(state.fields, "providerGeneration");
    if (!stored || unsignedValue(stored) != *message.providerGeneration)
      return true;
  }
  return false;
}

std::string scopedInteractionKey(const ProtocolRequestId &requestId,
                                 const DecodedMessage &message) {
  const std::string generation =
      "connection:" + std::to_string(message.connectionGeneration.value_or(0)) +
      ":provider:" + std::to_string(message.providerGeneration.value_or(0));
  return "request:" + scopedCanonical(generation, requestId.canonical());
}

std::vector<NodeRef> mergeExistingTail(std::vector<NodeRef> first,
                                       const std::vector<NodeRef> &existing) {
  std::unordered_set<const Node *> seen;
  seen.reserve(first.size() + existing.size());
  for (const NodeRef &node : first)
    if (node)
      seen.insert(node.get());
  for (const NodeRef &node : existing) {
    if (node && seen.insert(node.get()).second)
      first.emplace_back(node);
  }
  return first;
}

void removeContained(NodeGraph::WriteAccess &write, const NodeRef &node);

bool isExplicitLocalOptimistic(NodeGraph::WriteAccess &write,
                               const NodeRef &node) {
  if (!node)
    return false;
  const NodeState &state = *write.state(node);
  const Value *local = member(state.fields, "local");
  if (!local || !local->asBool() || !*local->asBool())
    return false;
  const std::string type = canonicalValue(member(state.fields, "type"));
  return type == "localPrompt" || type == "localTurn" ||
         type == "localThread" || type == "localRecoveryTurn" ||
         type == "localRecoveryThread";
}

void collectLocalOptimisticItems(NodeGraph::WriteAccess &write,
                                 const NodeRef &node,
                                 std::vector<NodeRef> &items,
                                 std::unordered_set<const Node *> &visited) {
  if (!node || !visited.insert(node.get()).second)
    return;
  if (node->id().kind == NodeKind::Item &&
      isExplicitLocalOptimistic(write, node)) {
    items.emplace_back(node);
    return;
  }
  for (const NodeRef &child : write.children(node))
    collectLocalOptimisticItems(write, child, items, visited);
  if (node->id().kind == NodeKind::Turn) {
    for (const NodeRef &root : write.related(node, RelationKind::TurnRootItem))
      collectLocalOptimisticItems(write, root, items, visited);
  }
}

NodeRef preserveLocalOptimisticTail(NodeGraph::WriteAccess &write,
                                    const NodeRef &thread,
                                    const NodeRef &omitted) {
  if (!thread || thread->id().kind != NodeKind::Thread || !omitted)
    return {};
  std::vector<NodeRef> prompts;
  std::unordered_set<const Node *> visited;
  collectLocalOptimisticItems(write, omitted, prompts, visited);
  if (prompts.empty())
    return {};

  NodeState state;
  state.status = NodeStatus::Running;
  state.fields = {{"type", Value("localTurn")},
                  {"local", Value(true)},
                  {"replacementCarrier", Value(true)}};
  const std::string carrierId =
      "local-turn:replacement:" +
      scopedCanonical(thread->id().canonical,
                      std::to_string(write.revision() + 1) + ':' +
                          omitted->id().canonical);
  NodeRef carrier = write.upsert({NodeKind::Turn, carrierId}, std::move(state));
  write.setParent(thread, carrier);
  for (const NodeRef &prompt : prompts) {
    if (omitted->id().kind == NodeKind::Turn)
      write.unrelate(omitted, RelationKind::TurnRootItem, prompt);
    write.setParent(carrier, prompt);
  }
  return carrier;
}

std::vector<NodeRef> replaceAuthoritativeChildren(
    NodeGraph::WriteAccess &write, const NodeRef &parent,
    std::vector<NodeRef> authoritative, const std::vector<NodeRef> &existing) {
  std::unordered_set<const Node *> retained;
  retained.reserve(authoritative.size() + existing.size());
  for (const NodeRef &node : authoritative)
    if (node)
      retained.insert(node.get());

  for (const NodeRef &node : existing) {
    if (!node || retained.contains(node.get()) ||
        write.find(node->id()) != node)
      continue;
    if (isExplicitLocalOptimistic(write, node)) {
      retained.insert(node.get());
      authoritative.emplace_back(node);
      continue;
    }
    if (NodeRef carrier = preserveLocalOptimisticTail(write, parent, node)) {
      retained.insert(carrier.get());
      authoritative.emplace_back(std::move(carrier));
    }
    removeContained(write, node);
    if (write.find(node->id()) == node)
      write.remove(node);
  }
  return authoritative;
}

bool hasThreadOwner(NodeGraph::WriteAccess &write, const NodeRef &child) {
  return !write.related(child, RelationKind::ThreadOwner).empty();
}

void clearThreadOwners(NodeGraph::WriteAccess &write, const NodeRef &child) {
  const std::vector<NodeRef> owners =
      write.related(child, RelationKind::ThreadOwner);
  for (const NodeRef &owner : owners) {
    write.unrelate(owner, RelationKind::StructuralChildThread, child);
    write.unrelate(owner, RelationKind::AgentChildThread, child);
    write.unrelate(child, RelationKind::ThreadOwner, owner);
  }
}

void clearStructuralThreadOwners(NodeGraph::WriteAccess &write,
                                 const NodeRef &child) {
  const std::vector<NodeRef> owners =
      write.related(child, RelationKind::ThreadOwner);
  for (const NodeRef &owner : owners) {
    const std::vector<NodeRef> structural =
        write.related(owner, RelationKind::StructuralChildThread);
    if (std::ranges::find(structural, child) == structural.end())
      continue;
    write.unrelate(owner, RelationKind::StructuralChildThread, child);
    const std::vector<NodeRef> agentChildren =
        write.related(owner, RelationKind::AgentChildThread);
    if (std::ranges::find(agentChildren, child) == agentChildren.end())
      write.unrelate(child, RelationKind::ThreadOwner, owner);
  }
}

void assignThreadOwner(NodeGraph::WriteAccess &write, const NodeRef &owner,
                       RelationKind kind, const NodeRef &child) {
  if (!owner || !child || owner == child)
    return;
  clearThreadOwners(write, child);
  write.relate(owner, kind, child);
  write.relate(child, RelationKind::ThreadOwner, owner);
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
  std::unordered_set<std::string> seen;
  std::string child = canonicalValue(member(item, "agentThreadId"));
  if (!child.empty() && seen.insert(child).second)
    children.emplace_back(std::move(child));
  const Value::Array *receivers = arrayMember(item, "receiverThreadIds");
  if (!receivers)
    return children;
  for (const Value &receiver : *receivers) {
    child = canonicalValue(&receiver);
    if (!child.empty() && seen.insert(child).second)
      children.emplace_back(std::move(child));
  }
  return children;
}

std::vector<NodeRef> referencedAgentChildren(NodeGraph::WriteAccess &write,
                                             const NodeRef &thread) {
  std::vector<NodeRef> referenced;
  if (!thread)
    return referenced;

  std::unordered_set<const Node *> seenItems;
  std::unordered_set<const Node *> seenChildren;
  for (const NodeRef &turn : write.children(thread)) {
    if (!turn || turn->id().kind != NodeKind::Turn)
      continue;
    std::vector<NodeRef> items = write.children(turn);
    items = mergeExistingTail(std::move(items),
                              write.related(turn, RelationKind::TurnRootItem));
    for (const NodeRef &item : items) {
      if (!item || item->id().kind != NodeKind::Item ||
          !seenItems.insert(item.get()).second)
        continue;
      for (const NodeRef &child :
           write.related(item, RelationKind::AgentChildThread)) {
        if (child && seenChildren.insert(child.get()).second)
          referenced.emplace_back(child);
      }
    }
  }
  return referenced;
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
    if (const auto submission = state->fields.find("submissionId");
        submission != state->fields.end())
      write.setField(authoritative, "localSubmissionId", submission->second);
    std::array<NodeRef, 1> alias{local};
    write.replaceRelated(authoritative, RelationKind::PromptMaterialization,
                         alias);
    // A steering prompt and its provider item share one Turn. The provider
    // item may arrive after activity caused by that prompt, but the visible
    // You card must retain the exact slot where the user submitted it.
    const NodeRef localTurn = write.parent(local);
    if (localTurn && write.parent(authoritative) == localTurn) {
      std::vector<NodeRef> ordered = write.children(localTurn);
      ordered.erase(std::remove(ordered.begin(), ordered.end(), authoritative),
                    ordered.end());
      if (const auto position = std::ranges::find(ordered, local);
          position != ordered.end())
        ordered.insert(std::next(position), authoritative);
      else
        ordered.push_back(authoritative);
      write.replaceChildren(localTurn, ordered);
    }
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
      if (item && item->id().kind == NodeKind::Item &&
          !isLocalPrompt(write, item))
        loaded.insert(item.get());
    }
    for (const NodeRef &root :
         write.related(turn, RelationKind::TurnRootItem)) {
      if (root && root->id().kind == NodeKind::Item &&
          !isLocalPrompt(write, root))
        loaded.insert(root.get());
    }
  }
  write.setField(thread, "historyLoadedItemCount",
                 Value(static_cast<std::uint64_t>(loaded.size())));
}

void incrementLoadedHistoryItemCount(NodeGraph::WriteAccess &write,
                                     const NodeRef &thread, std::size_t added) {
  if (!thread || added == 0)
    return;
  const Value *current =
      member(write.state(thread)->fields, "historyLoadedItemCount");
  std::uint64_t count = 0;
  if (const std::uint64_t *number = current ? current->asUInt64() : nullptr)
    count = *number;
  else if (const std::int64_t *number = current ? current->asInt64() : nullptr;
           number && *number >= 0)
    count = static_cast<std::uint64_t>(*number);
  else {
    updateLoadedHistoryItemCount(write, thread);
    return;
  }
  write.setField(thread, "historyLoadedItemCount",
                 Value(count + static_cast<std::uint64_t>(added)));
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
  std::unordered_set<const Node *> seen;
  seen.insert(node.get());
  std::vector<NodeRef> pending = write.children(node);
  for (const NodeRef &root : write.related(node, RelationKind::TurnRootItem))
    pending.emplace_back(root);

  std::vector<NodeRef> owned;
  while (!pending.empty()) {
    NodeRef current = std::move(pending.back());
    pending.pop_back();
    if (!current || !seen.insert(current.get()).second ||
        write.find(current->id()) != current)
      continue;
    owned.emplace_back(current);
    for (const NodeRef &child : write.children(current))
      pending.emplace_back(child);
    for (const NodeRef &root :
         write.related(current, RelationKind::TurnRootItem))
      pending.emplace_back(root);
  }
  std::ranges::reverse(owned);
  write.removeMany(owned);
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
  const std::string retentionField = "transcripts/" + std::string(role);
  if (!append)
    clearTextRetention(next, retentionField);
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
  std::size_t discarded = 0;
  if (combined.size() > MaximumRetainedStreamBytes) {
    discarded = utf8TailStart(combined, RetainedStreamTailBytes);
    combined.erase(0, discarded);
  }
  roleText = Value(std::move(combined));
  if (discarded != 0 || hasTextRetention(next, retentionField))
    updateTextRetention(next, retentionField, roleText.asString()->size(),
                        discarded);

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

bool applyExternalAgentImportUpdate(NodeGraph::WriteAccess &write,
                                    const DecodedMessage &message) {
  const std::string_view method = message.method;
  const bool importResult =
      message.kind == DecodedMessageKind::ClientResult &&
      (method == "externalAgentConfig/import" ||
       method == "externalAgentConfig/import/recordHistory");
  const bool progress = method == "externalAgentConfig/import/progress";
  const bool completed =
      method == "externalAgentConfig/import/completed" ||
      (importResult && method == "externalAgentConfig/import/recordHistory");
  if (!importResult && !progress && !completed)
    return false;

  const std::string importId =
      canonicalValue(member(message.payload, "importId"));
  if (importId.empty())
    return true;

  NodeRef import = write.upsert({NodeKind::ExternalAgentImport, importId});
  mergeObject(write, import, message.payload);
  write.setField(import, "protocolId", Value(importId));
  write.setField(import, "lastMethod", Value(message.method));
  write.setStatus(import,
                  completed ? NodeStatus::Completed : NodeStatus::Running);
  write.setField(import, "lifecycle",
                 Value(completed ? "completed" : "running"));
  return true;
}

bool applyFuzzyFileSearchSessionUpdate(NodeGraph::WriteAccess &write,
                                       const DecodedMessage &message) {
  const std::string_view method = message.method;
  const bool notification = method == "fuzzyFileSearch/sessionUpdated" ||
                            method == "fuzzyFileSearch/sessionCompleted";
  const bool result = message.kind == DecodedMessageKind::ClientResult &&
                      (method == "fuzzyFileSearch/sessionStart" ||
                       method == "fuzzyFileSearch/sessionUpdate" ||
                       method == "fuzzyFileSearch/sessionStop");
  if (!notification && !result)
    return false;

  const std::string sessionId =
      canonicalValue(member(message.payload, "sessionId"));
  if (sessionId.empty())
    return true;

  NodeRef session = ensureConnectionScopedNode(
      write, NodeKind::FuzzyFileSearchSession, sessionId);
  mergeObject(write, session, message.payload);
  write.setField(session, "lastMethod", Value(message.method));
  const bool completed = method == "fuzzyFileSearch/sessionCompleted";
  const bool stopped = method == "fuzzyFileSearch/sessionStop";
  write.setStatus(session, completed ? NodeStatus::Completed
                           : stopped ? NodeStatus::Interrupted
                                     : NodeStatus::Running);
  write.setField(session, "lifecycle",
                 Value(completed ? "completed"
                       : stopped ? "stopped"
                                 : "running"));
  return true;
}

std::string loginAttemptNodeKey(const Value *loginId) {
  if (!loginId || loginId->isNull())
    return "login-id:none";
  if (const std::string *id = loginId->asString())
    return scopedCanonical("login-id", *id);
  return "login-id:invalid";
}

bool applyAccountLoginUpdate(NodeGraph::WriteAccess &write,
                             const DecodedMessage &message) {
  const bool completed = message.method == "account/login/completed";
  const bool startResult = message.kind == DecodedMessageKind::ClientResult &&
                           message.method == "account/login/start";
  const bool cancelResult = message.kind == DecodedMessageKind::ClientResult &&
                            message.method == "account/login/cancel";
  if (!completed && !startResult && !cancelResult)
    return false;

  const Value *loginId = member(message.payload, "loginId");
  NodeRef attempt =
      write.upsert({NodeKind::LoginAttempt, loginAttemptNodeKey(loginId)});
  mergeObject(write, attempt, message.payload);
  if (loginId)
    write.setField(attempt, "protocolId", *loginId);
  write.setField(attempt, "lastMethod", Value(message.method));
  if (completed) {
    const Value *success = member(message.payload, "success");
    if (!success || !success->asBool())
      return true;
    write.setStatus(attempt, *success->asBool() ? NodeStatus::Completed
                                                : NodeStatus::Failed);
    write.setField(attempt, "lifecycle",
                   Value(*success->asBool() ? "completed" : "failed"));
  } else if (cancelResult) {
    const std::string status =
        canonicalValue(member(message.payload, "status"));
    write.setStatus(attempt, status == "canceled" ? NodeStatus::Interrupted
                                                  : NodeStatus::Completed);
    write.setField(attempt, "lifecycle",
                   Value(status.empty() ? "cancelled" : status));
  } else {
    write.setStatus(attempt, NodeStatus::Running);
    write.setField(attempt, "lifecycle", Value("running"));
  }
  return true;
}

bool applyAccountUpdate(NodeGraph::WriteAccess &write,
                        const DecodedMessage &message) {
  const bool readResult = message.kind == DecodedMessageKind::ClientResult &&
                          message.method == "account/read";
  const bool logoutResult = message.kind == DecodedMessageKind::ClientResult &&
                            message.method == "account/logout";
  const bool notification =
      message.kind == DecodedMessageKind::ServerNotification &&
      message.method == "account/updated";
  if (!readResult && !logoutResult && !notification)
    return false;

  NodeRef account = write.upsert({NodeKind::Account, "account"});
  if (logoutResult) {
    NodeState next;
    const std::shared_ptr<const NodeState> previous = write.state(account);
    if (const Value *requiresAuth =
            member(previous->fields, "requiresOpenaiAuth"))
      next.fields.emplace("requiresOpenaiAuth", *requiresAuth);
    next.fields.emplace("account", Value(nullptr));
    next.fields.emplace("authMode", Value(nullptr));
    next.fields.emplace("planType", Value(nullptr));
    next.fields.emplace("lastMethod", Value(message.method));
    write.replaceState(account, std::move(next));
    return true;
  }
  mergeObject(write, account, message.payload);
  write.setField(account, "lastMethod", Value(message.method));
  return true;
}

bool applyAccountRateLimitsUpdate(NodeGraph::WriteAccess &write,
                                  const DecodedMessage &message) {
  const bool readResult = message.kind == DecodedMessageKind::ClientResult &&
                          message.method == "account/rateLimits/read";
  const bool notification =
      message.kind == DecodedMessageKind::ServerNotification &&
      message.method == "account/rateLimits/updated";
  if (!readResult && !notification)
    return false;

  NodeRef rateLimits = write.upsert({NodeKind::Account, "rate-limits"});
  NodeState next = readResult ? NodeState{} : *write.state(rateLimits);
  if (readResult)
    next.fields = message.payload;
  else
    mergeSparseRateLimitFields(next.fields, message.payload);
  next.fields.insert_or_assign("lastMethod", Value(message.method));
  write.replaceState(rateLimits, std::move(next));
  return true;
}

bool applyConfigurationUpdate(NodeGraph::WriteAccess &write,
                              const DecodedMessage &message) {
  if (message.kind != DecodedMessageKind::ClientResult)
    return false;

  const bool readResult = message.method == "config/read";
  const bool writeResult = message.method == "config/value/write" ||
                           message.method == "config/batchWrite";
  if (!readResult && !writeResult)
    return false;

  NodeRef configuration =
      write.upsert({NodeKind::Configuration, "config/read"});
  if (readResult) {
    NodeState next;
    next.fields = message.payload;
    next.fields.insert_or_assign("lastMethod", Value(message.method));
    write.replaceState(configuration, std::move(next));
    return true;
  }

  // A successful write confirms that the prior effective snapshot is no
  // longer authoritative, but its result does not contain enough information
  // to reconstruct layered effective config safely.
  write.setField(configuration, "stale", Value(true));
  write.setField(configuration, "invalidatedBy", Value(message.method));
  write.setField(configuration, "lastMethod", Value(message.method));
  return true;
}

void applyAddressedTurnError(NodeGraph::WriteAccess &write,
                             const Value::Object &payload) {
  const std::string threadId = addressedId(payload, NodeKind::Thread);
  if (threadId.empty())
    return;

  NodeRef thread = write.upsert({NodeKind::Thread, threadId});
  const std::string turnId = addressedId(payload, NodeKind::Turn);
  NodeRef turn = ensureTurn(write, thread, turnId);
  const Value *error = member(payload, "error");
  const Value *willRetry = member(payload, "willRetry");
  for (const NodeRef &target : {thread, turn}) {
    if (!target)
      continue;
    if (error)
      write.setField(target, "error", *error);
    if (willRetry)
      write.setField(target, "willRetry", *willRetry);
  }
  if (turn)
    write.setField(thread, "errorTurnId", Value(turnId));
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
  auto write = graph_->write();
  AppliedMessage applied = applyInto(write, message);
  return ApplyResult{applied.knownMethod, applied.disposition, write.finish(),
                     std::move(applied.primary)};
}

AppliedMessage ProtocolUpdater::applyInto(NodeGraph::WriteAccess &write,
                                          const DecodedMessage &message) {
  const auto touchAddressedThread = [&] {
    if (!write.hasPendingChanges())
      return;
    std::string threadId = addressedId(message.payload, NodeKind::Thread);
    if (threadId.empty()) {
      if (const Value::Object *object = objectMember(message.payload, "thread"))
        threadId = nestedId(*object);
    }
    if (!threadId.empty()) {
      if (NodeRef thread = write.find({NodeKind::Thread, threadId}))
        write.touchRevision(thread);
    }
  };
  const ProtocolDirection direction = catalogDirection(message.kind);
  const auto descriptor = findProtocolMethod(direction, message.method);
  if (!descriptor) {
    applyUnknown(write, message);
    applyThreadActivity(write, message);
    touchAddressedThread();
    return AppliedMessage{false, MessageDisposition::GraphUpdate, {}};
  }

  if (descriptor->get().disposition ==
      MessageDisposition::IntentionallyStateNeutral) {
    return AppliedMessage{true, descriptor->get().disposition, {}};
  }

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
  touchAddressedThread();
  return AppliedMessage{true, descriptor->get().disposition,
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

    const std::vector<NodeRef> owners =
        write.related(thread, RelationKind::ThreadOwner);
    thread = owners.empty() ? NodeRef{} : owners.front();
  }
}

GraphChange
ProtocolUpdater::resolveInteraction(const ProtocolRequestId &requestId,
                                    bool accepted, std::string error) {
  auto write = graph_->write();
  NodeRef interaction =
      newestInteractionForRequest(write, requestId.canonical());
  if (!interaction)
    return write.finish();
  if (!accepted) {
    write.setStatus(interaction, NodeStatus::Failed);
    write.setField(interaction, "error", Value(std::move(error)));
    for (const NodeRef &target :
         write.related(interaction, RelationKind::InteractionTarget))
      refreshPendingInteractionCount(write, containingThread(write, target));
    return write.finish();
  }
  clearPendingInteractionOwner(write, interaction);
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
    for (const NodeRef &target :
         write.related(interaction, RelationKind::InteractionTarget))
      refreshPendingInteractionCount(write, containingThread(write, target));
    return write.finish();
  }
  clearPendingInteractionOwner(write, interaction);
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
    if (message.method == "thread/read")
      write.setField(operation, "requestTargetRevision",
                     Value(write.revision() + 1));
    write.setStatus(operation, NodeStatus::Pending);
    NodeRef target;
    const bool exactTargetSupplied = static_cast<bool>(message.requestTarget);
    if (exactTargetSupplied) {
      if (write.find(message.requestTarget->id()) == message.requestTarget)
        target = message.requestTarget;
    } else {
      const std::string threadId =
          addressedId(message.payload, NodeKind::Thread);
      const std::string turnId = addressedId(message.payload, NodeKind::Turn);
      const std::string itemId = addressedId(message.payload, NodeKind::Item);
      NodeRef thread;
      if (!threadId.empty())
        thread = write.upsert({NodeKind::Thread, threadId});
      NodeRef turn;
      if (!turnId.empty())
        turn = ensureTurn(write, thread, turnId);
      if (beginsWith(message.method, "command/exec") ||
          beginsWith(message.method, "process/")) {
        target = ensureProcess(write, message.payload);
        if (target) {
          mergeObject(write, target, message.payload);
          if (message.method == "command/exec" ||
              message.method == "process/spawn")
            write.setStatus(target, NodeStatus::Running);
        }
      } else if (message.method == "fs/watch" ||
                 message.method == "fs/unwatch") {
        target = ensureWatch(write, message.payload);
        if (target) {
          mergeObject(write, target, message.payload);
          if (message.method == "fs/watch")
            write.setStatus(target, NodeStatus::Pending);
        }
      } else if (!itemId.empty()) {
        target = ensureItem(write, turn, itemId);
      }
      if (!target && turn)
        target = turn;
      if (!target && thread)
        target = thread;
    }
    if (target) {
      write.relate(operation, RelationKind::OperationTarget, target);
    }
    if (target || exactTargetSupplied)
      write.setField(operation, "hadOperationTarget", Value(true));
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

  const Value *hadOperationTarget =
      member(operationState->fields, "hadOperationTarget");
  if (hadOperationTarget && hadOperationTarget->asBool() &&
      *hadOperationTarget->asBool() &&
      write.related(operation, RelationKind::OperationTarget).empty()) {
    // The target was deleted after this request was sent. Retire the exact
    // operation without allowing any late result payload to recreate it.
    write.remove(operation);
    return {};
  }

  if (message.kind == DecodedMessageKind::ClientResult) {
    DecodedMessage correlated = message;
    const auto request = operationState->fields.find("requestPayload");
    if (request != operationState->fields.end()) {
      if (const Value::Object *requestObject = request->second.asObject()) {
        for (const auto &[key, value] : *requestObject)
          correlated.payload.try_emplace(key, value);
      }
    }
    std::optional<std::uint64_t> preserveChangesAfter;
    if (message.method == "thread/read") {
      const Value *started =
          member(operationState->fields, "requestTargetRevision");
      const std::uint64_t *revision = started ? started->asUInt64() : nullptr;
      if (revision)
        preserveChangesAfter = *revision;
    }
    applyGraphUpdate(write, correlated, preserveChangesAfter);
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
  const std::string requestId = message.requestId->canonical();
  std::string interactionKey = requestId;
  if (NodeRef previous = write.find({NodeKind::Interaction, requestId})) {
    const std::shared_ptr<const NodeState> previousState =
        write.state(previous);
    const Value *recovery = member(previousState->fields, "recoveryOnly");
    const bool recoveryOnly =
        recovery && recovery->asBool() && *recovery->asBool();
    if (recoveryOnly || interactionGenerationDiffers(*previousState, message)) {
      interactionKey = scopedInteractionKey(*message.requestId, message);
    } else {
      clearPendingInteractionOwner(write, previous);
      write.remove(previous);
    }
  }
  if (NodeRef previous = write.find({NodeKind::Interaction, interactionKey})) {
    clearPendingInteractionOwner(write, previous);
    write.remove(previous);
  }
  NodeRef interaction =
      write.upsert({NodeKind::Interaction, std::move(interactionKey)});
  clearPendingInteractionOwner(write, interaction);
  const std::vector<NodeRef> previousTargets =
      write.related(interaction, RelationKind::InteractionTarget);
  for (const NodeRef &previous : previousTargets)
    write.unrelate(interaction, RelationKind::InteractionTarget, previous);
  NodeState state;
  state.status = NodeStatus::Pending;
  state.fields.emplace("method", Value(message.method));
  state.fields.emplace("payload", Value(message.payload));
  state.fields.emplace("requestId", Value(requestId));
  if (message.connectionGeneration)
    state.fields.emplace("connectionGeneration",
                         Value(*message.connectionGeneration));
  if (message.providerGeneration)
    state.fields.emplace("providerGeneration",
                         Value(*message.providerGeneration));
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
    if (NodeRef owner = containingThread(write, target)) {
      write.relate(owner, RelationKind::PendingInteraction, interaction);
      refreshPendingInteractionCount(write, owner);
    }
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
        appendBoundedStringField(write, realtimeItem, "transcript", delta);
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

void ProtocolUpdater::applyGraphUpdate(
    NodeGraph::WriteAccess &write, const DecodedMessage &message,
    std::optional<std::uint64_t> preserveChangesAfter) {
  const std::string_view method = message.method;

  if (isProviderNoticeMethod(method)) {
    NodeRef notice = write.upsert({NodeKind::Notice, "provider-notice"});
    write.setField(notice, "method", Value(std::string(method)));
    write.setField(notice, "severity",
                   Value(method == "error" ? "error" : "warning"));
    mergeObject(write, notice, message.payload);
    write.setStatus(notice, method == "error" ? NodeStatus::Failed
                                              : NodeStatus::Completed);
    if (method == "error")
      applyAddressedTurnError(write, message.payload);
    return;
  }

  if (applyNaturalCatalogSnapshot(write, message) ||
      applyAutoApprovalReview(write, message))
    return;

  if (method == "autoApprovalReview/strictReviewRequired") {
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    const std::string turnId = addressedId(message.payload, NodeKind::Turn);
    if (!threadId.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, threadId});
      NodeRef target =
          turnId.empty() ? thread : ensureTurn(write, thread, turnId);
      mergeObject(write, target, message.payload);
      write.setField(target, "strictReviewRequired", Value(true));
    }
    return;
  }

  if (method == "thread/environment/connected" ||
      method == "thread/environment/disconnected") {
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    if (!threadId.empty()) {
      const bool connected = method == "thread/environment/connected";
      NodeRef thread = write.upsert({NodeKind::Thread, threadId});
      mergeObject(write, thread, message.payload);
      write.setField(thread, "environmentConnected", Value(connected));
      write.setField(thread, "environmentStatus",
                     Value(connected ? "connected" : "disconnected"));
    }
    return;
  }

  if (method == "modelProvider/authRecoveryStarted" ||
      method == "modelProvider/authRecoveryCompleted") {
    const bool active = method == "modelProvider/authRecoveryStarted";
    NodeRef provider = write.upsert({NodeKind::Catalog, "modelProvider"});
    mergeObject(write, provider, message.payload);
    write.setField(provider, "authRecoveryActive", Value(active));
    write.setField(provider, "authRecoveryStatus",
                   Value(active ? "recovering" : "completed"));
    write.setField(provider, "lastMethod", Value(std::string(method)));
    write.setStatus(provider,
                    active ? NodeStatus::Running : NodeStatus::Completed);
    return;
  }

  if (method == "thread/compacted") {
    const std::string threadId = addressedId(message.payload, NodeKind::Thread);
    if (!threadId.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, threadId});
      mergeObject(write, thread, message.payload);
      write.setField(thread, "compacted", Value(true));
      if (const Value *turnId = member(message.payload, "turnId"))
        write.setField(thread, "lastCompactedTurnId", *turnId);
    }
    return;
  }

  if (applyExternalAgentImportUpdate(write, message) ||
      applyFuzzyFileSearchSessionUpdate(write, message) ||
      applyAccountLoginUpdate(write, message) ||
      applyAccountUpdate(write, message) ||
      applyAccountRateLimitsUpdate(write, message) ||
      applyConfigurationUpdate(write, message))
    return;

  if (method.starts_with("mcpServer/")) {
    NodeRef server = write.upsert(
        {NodeKind::McpServer, mcpServerNodeKey(method, message.payload)});
    const std::string protocolId =
        method == "mcpServer/event/stream/notification"
            ? canonicalValue(member(message.payload, "subscriptionId"))
            : canonicalValue(member(message.payload, "name"));
    if (!protocolId.empty())
      write.setField(server, "protocolId", Value(protocolId));
    write.setField(server, "lastMethod", Value(std::string(method)));
    mergeObject(write, server, message.payload);
    return;
  }

  if (method == "hook/started" || method == "hook/completed") {
    static_cast<void>(applyHookNotification(write, message));
    return;
  }

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
      appendBoundedStringField(write, process,
                               (stream.empty() ? "output" : stream) + "Base64",
                               delta);
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
      const std::string typedRequestId = requestId && requestId->asString()
                                             ? "string:" + canonical
                                             : "number:" + canonical;
      NodeRef interaction;
      if (message.expectedNode &&
          write.find(message.expectedNode->id()) == message.expectedNode &&
          canonicalValue(member(write.state(message.expectedNode)->fields,
                                "requestId")) == typedRequestId) {
        interaction = message.expectedNode;
      } else if (!message.expectedNode) {
        interaction = newestInteractionForRequest(write, typedRequestId);
      }
      if (interaction &&
          (!message.expectedNode || message.expectedNode == interaction)) {
        clearPendingInteractionOwner(write, interaction);
        write.remove(interaction);
      }
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
        write.setField(thread, "status", Value("notLoaded"));
        write.setStatus(thread, NodeStatus::NotLoaded);
        write.replaceRelated(thread, RelationKind::ActiveTurn,
                             std::span<const NodeRef>{});
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

  if (message.kind == DecodedMessageKind::ClientResult &&
      method == "thread/queue/list") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (!id.empty()) {
      NodeRef thread = write.upsert({NodeKind::Thread, id});
      mergeObject(write, thread, message.payload);
      write.eraseField(thread, "queueStale");
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
        write.setStatus(thread, normalized);
        if (normalized != NodeStatus::Running)
          write.replaceRelated(thread, RelationKind::ActiveTurn,
                               std::span<const NodeRef>{});
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
            write.eraseField(project, "stale");
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
    write.eraseField(project, "stale");
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
      method == "thread/items/list") {
    const std::string id = addressedId(message.payload, NodeKind::Thread);
    if (id.empty())
      return;
    NodeRef thread = write.upsert({NodeKind::Thread, id});
    std::vector<std::pair<NodeRef, std::vector<NodeRef>>> pages;
    std::unordered_map<const Node *, std::size_t> pageIndexes;
    std::vector<std::unordered_set<const Node *>> pageItems;
    if (const Value::Array *entries = arrayMember(message.payload, "data")) {
      pages.reserve(entries->size());
      pageItems.reserve(entries->size());
      for (const Value &value : *entries) {
        const Value::Object *entry = value.asObject();
        const Value::Object *itemObject =
            entry ? objectMember(*entry, "item") : nullptr;
        const std::string turnId =
            entry ? canonicalValue(member(*entry, "turnId")) : std::string{};
        if (!itemObject || turnId.empty())
          continue;
        NodeRef turn = ensureTurn(write, thread, turnId);
        NodeRef item = ingestItem(write, *itemObject, turn);
        if (!item)
          continue;
        auto [position, inserted] =
            pageIndexes.emplace(turn.get(), pages.size());
        if (inserted) {
          pages.emplace_back(turn, std::vector<NodeRef>{});
          pageItems.emplace_back();
        }
        const std::size_t page = position->second;
        if (pageItems[page].insert(item.get()).second)
          pages[page].second.emplace_back(std::move(item));
      }
    }

    const std::string direction =
        canonicalValue(member(message.payload, "sortDirection"));
    for (auto &[turn, page] : pages) {
      if (direction == "desc")
        std::reverse(page.begin(), page.end());
      const std::vector<NodeRef> existing = write.children(turn);
      if (direction == "asc")
        write.replaceChildren(turn, mergeExistingTail(existing, page));
      else
        write.replaceChildren(turn,
                              mergeExistingTail(std::move(page), existing));
    }

    NodeRef cursorOwner = thread;
    const std::string requestedTurn =
        canonicalValue(member(message.payload, "turnId"));
    if (!requestedTurn.empty())
      cursorOwner = ensureTurn(write, thread, requestedTurn);
    const std::string nextCursor =
        canonicalValue(member(message.payload, "nextCursor"));
    write.setField(cursorOwner, "itemsHistoryHasMore",
                   Value(!nextCursor.empty()));
    if (nextCursor.empty())
      write.eraseField(cursorOwner, "itemsHistoryNextCursor");
    else
      write.setField(cursorOwner, "itemsHistoryNextCursor", Value(nextCursor));
    const std::string backwardsCursor =
        canonicalValue(member(message.payload, "backwardsCursor"));
    if (backwardsCursor.empty())
      write.eraseField(cursorOwner, "itemsHistoryBackwardsCursor");
    else
      write.setField(cursorOwner, "itemsHistoryBackwardsCursor",
                     Value(backwardsCursor));
    updateLoadedHistoryItemCount(write, thread);
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
      std::unordered_set<const Node *> seen;
      seen.reserve(turns->size());
      for (const Value &value : *turns) {
        const Value::Object *turnObject = value.asObject();
        if (!turnObject)
          continue;
        NodeRef turn = ingestTurn(write, *turnObject, thread, {}, true);
        if (turn && seen.insert(turn.get()).second)
          page.emplace_back(std::move(turn));
      }
      write.replaceChildren(thread,
                            mergeExistingTail(std::move(page), previous));
    }

    refreshActiveTurn(write, thread);

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
    bool newItemMembership = false;
    if (!threadId.empty())
      thread = write.upsert({NodeKind::Thread, threadId});
    if (!turnId.empty())
      turn = ensureTurn(write, thread, turnId);
    if (!itemId.empty()) {
      const NodeId itemNodeId = turn ? scopedItemNodeId(turn->id(), itemId)
                                     : NodeId{NodeKind::Item, {}};
      const NodeRef previousItem = turn ? write.find(itemNodeId) : NodeRef{};
      const NodeRef previousParent =
          previousItem ? write.parent(previousItem) : NodeRef{};
      item = ensureItem(write, turn, itemId);
      if (item) {
        newItemMembership = previousParent != turn;
        appendSemanticDelta(write, item, method, message.payload);
      }
    }
    if (newItemMembership)
      incrementLoadedHistoryItemCount(write, thread, 1);
    return;
  }

  NodeRef thread;
  NodeRef turn;
  NodeRef item;
  bool historyMembershipMayChange = false;
  bool historyMembershipRequiresRecount = false;
  std::size_t newHistoryItems = 0;
  const auto refreshHistoryCount = [&] {
    if (historyMembershipRequiresRecount)
      updateLoadedHistoryItemCount(write, thread);
    else
      incrementLoadedHistoryItemCount(write, thread, newHistoryItems);
  };
  if (const Value::Object *threadObject =
          objectMember(message.payload, "thread")) {
    const bool authoritativeHistoryResult =
        message.kind == DecodedMessageKind::ClientResult &&
        (method == "thread/rollback" || method == "thread/revert");
    historyMembershipMayChange = authoritativeHistoryResult ||
                                 arrayMember(*threadObject, "turns") != nullptr;
    historyMembershipRequiresRecount = historyMembershipMayChange;
    const bool replaceTurns =
        message.kind == DecodedMessageKind::ClientResult &&
        (method == "thread/read" || authoritativeHistoryResult);
    thread = ingestThread(write, *threadObject, {}, replaceTurns,
                          method == "thread/read" ? preserveChangesAfter
                                                  : std::nullopt);
    if (authoritativeHistoryResult && thread) {
      const std::string turnsCursor =
          canonicalValue(member(message.payload, "turnsBackwardsCursor"));
      write.setField(thread, "historyHasMore", Value(!turnsCursor.empty()));
      if (turnsCursor.empty())
        write.eraseField(thread, "historyNextCursor");
      else
        write.setField(thread, "historyNextCursor", Value(turnsCursor));
      const std::string itemsCursor =
          canonicalValue(member(message.payload, "itemsBackwardsCursor"));
      if (itemsCursor.empty())
        write.eraseField(thread, "itemsHistoryBackwardsCursor");
      else
        write.setField(thread, "itemsHistoryBackwardsCursor",
                       Value(itemsCursor));
      write.eraseField(thread, "historyStale");
    }
    if (thread && (method == "thread/started" ||
                   (message.kind == DecodedMessageKind::ClientResult &&
                    (method == "thread/start" || method == "thread/resume" ||
                     method == "thread/fork" || method == "thread/read"))))
      admitRootThread(write, thread, true);
    if (thread && message.kind == DecodedMessageKind::ClientResult &&
        method == "thread/read" && replaceTurns) {
      const Value *includeTurns = member(message.payload, "includeTurns");
      if (includeTurns && includeTurns->asBool() && *includeTurns->asBool())
        write.eraseField(thread, "historyStale");
    }
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
    historyMembershipRequiresRecount |=
        arrayMember(*turnObject, "items") != nullptr;
    turn = ingestTurn(write, *turnObject, thread);
    if (turn && method == "turn/started" &&
        write.state(turn)->status == NodeStatus::Unknown) {
      write.setStatus(turn, NodeStatus::Running);
      write.setField(turn, "status", Value("running"));
    }
    if (turn && method == "turn/completed" &&
        statusFromValue(member(*turnObject, "status")) == NodeStatus::Unknown) {
      write.setStatus(turn, NodeStatus::Completed);
      write.setField(turn, "status", Value("completed"));
    }
    updateActiveTurn(write, thread, turn);
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
    if (turn && previousParent != turn) {
      historyMembershipMayChange = true;
      ++newHistoryItems;
    }
    item = ingestItem(write, *itemObject, turn);
    if (item) {
      if (const Value *startedAt = member(message.payload, "startedAtMs"))
        write.setField(item, "startedAtMs", *startedAt);
      if (const Value *completedAt = member(message.payload, "completedAtMs"))
        write.setField(item, "completedAtMs", *completedAt);
    }
    if (item && method == "item/started" &&
        write.state(item)->status == NodeStatus::Unknown) {
      write.setStatus(item, NodeStatus::Running);
      write.setField(item, "status", Value("running"));
    }
    if (item && method == "item/completed" &&
        statusFromValue(member(*itemObject, "status")) == NodeStatus::Unknown) {
      write.setStatus(item, NodeStatus::Completed);
      write.setField(item, "status", Value("completed"));
    }
  } else {
    const std::string id = addressedId(message.payload, NodeKind::Item);
    if (!id.empty()) {
      const NodeRef previousItem =
          turn ? write.find(scopedItemNodeId(turn->id(), id)) : NodeRef{};
      const NodeRef previousParent =
          previousItem ? write.parent(previousItem) : NodeRef{};
      item = ensureItem(write, turn, id);
      if (item) {
        if (previousParent != turn) {
          historyMembershipMayChange = true;
          ++newHistoryItems;
        }
      }
    }
  }

  if (thread && message.kind == DecodedMessageKind::ClientResult &&
      (method == "thread/start" || method == "thread/resume" ||
       method == "thread/fork")) {
    mergeThreadResultSettings(write, thread, message.payload);
  }
  if (thread && method == "thread/settings/updated") {
    if (const Value::Object *settings =
            objectMember(message.payload, "threadSettings")) {
      mergeEffectiveThreadSettings(write, thread, *settings);
      write.setField(thread, "latestSettingsUpdate", Value(*settings));
      write.setField(thread, "settingsRevision", Value(write.revision() + 1));
    }
  }

  if (method == "turn/plan/updated" && turn) {
    if (const Value *explanation = member(message.payload, "explanation"))
      write.setField(turn, "planExplanation", *explanation);
    if (const Value *plan = member(message.payload, "plan"))
      write.setField(turn, "plan", *plan);
    if (historyMembershipMayChange)
      refreshHistoryCount();
    return;
  }

  if (method == "turn/diff/updated" && turn) {
    if (const Value *diff = member(message.payload, "diff"))
      write.setField(turn, "diff", *diff);
    if (historyMembershipMayChange)
      refreshHistoryCount();
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
        appendBoundedStringField(write, node, "output", delta);
    } else {
      mergeObject(write, node, message.payload);
    }
    return;
  }

  if (objectMember(message.payload, "thread") ||
      objectMember(message.payload, "turn") ||
      objectMember(message.payload, "item")) {
    if (historyMembershipMayChange)
      refreshHistoryCount();
    return;
  }

  NodeRef addressed = item ? item : (turn ? turn : thread);
  if (addressed)
    mergeObject(write, addressed, message.payload);
  if (historyMembershipMayChange)
    refreshHistoryCount();
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

NodeRef ProtocolUpdater::ingestThread(
    NodeGraph::WriteAccess &write, const Value::Object &object,
    std::string_view fallbackId, bool replaceTurns,
    std::optional<std::uint64_t> preserveChangesAfter) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty())
    return {};
  NodeRef thread = write.upsert({NodeKind::Thread, id});
  const std::string previousForkSourceId =
      canonicalValue(member(write.state(thread)->fields, "forkedFromId"));
  const auto acceptsField = [&](std::string_view field) {
    return !preserveChangesAfter ||
           write.fieldChangedRevision(thread, field) <= *preserveChangesAfter;
  };
  mergeObject(write, thread, object, "turns", preserveChangesAfter);

  if (const Value *projectId = member(object, "projectId");
      projectId && acceptsField("projectId"))
    assignProject(write, thread, projectId);
  if (const Value *section = member(object, "section");
      section && acceptsField("section"))
    assignSection(write, thread, section);

  if (const Value *parentValue = member(object, "parentThreadId");
      parentValue && acceptsField("parentThreadId")) {
    const std::string parentId = canonicalValue(parentValue);
    if (!parentId.empty() && parentId != id) {
      NodeRef parent = write.upsert({NodeKind::Thread, parentId});
      assignThreadOwner(write, parent, RelationKind::StructuralChildThread,
                        thread);
    } else if (parentValue->isNull() || parentId.empty()) {
      // A provider-null structural parent does not contradict the direct
      // spawn relation retained from the owning thread's agent activity.
      // Clearing every owner here promoted a selected child to a new root and
      // displaced its real root thread in ThreadPane after thread/read.
      clearStructuralThreadOwners(write, thread);
    }
  }

  if (const Value *forkValue = member(object, "forkedFromId");
      forkValue && acceptsField("forkedFromId")) {
    const std::string forkedFromId = canonicalValue(forkValue);
    if (!previousForkSourceId.empty() && previousForkSourceId != forkedFromId) {
      if (NodeRef previousSource =
              write.find({NodeKind::Thread, previousForkSourceId}))
        write.unrelate(previousSource, RelationKind::ForkChildThread, thread);
    }
    if (!forkedFromId.empty() && forkedFromId != id) {
      NodeRef source = write.upsert({NodeKind::Thread, forkedFromId});
      write.relate(source, RelationKind::ForkChildThread, thread);
    }
  }

  if (const Value::Array *turns = arrayMember(object, "turns")) {
    std::vector<NodeRef> order;
    order.reserve(turns->size());
    std::unordered_set<const Node *> ordered;
    ordered.reserve(turns->size());
    for (const Value &value : *turns) {
      if (const Value::Object *turnObject = value.asObject()) {
        const std::string turnId = nestedId(*turnObject);
        NodeRef turn = ingestTurn(write, *turnObject, thread, {}, replaceTurns,
                                  preserveChangesAfter, true);
        if (turn && ordered.insert(turn.get()).second)
          order.emplace_back(std::move(turn));
      }
    }
    if (replaceTurns)
      write.replaceChildren(
          thread, replaceAuthoritativeChildren(write, thread, std::move(order),
                                               write.children(thread)));
    else if (!order.empty())
      write.replaceChildren(
          thread, mergeExistingTail(std::move(order), write.children(thread)));
    refreshActiveTurn(write, thread);
    if (replaceTurns)
      reconcileAgentChildRelations(write, thread);
  }
  return thread;
}

NodeRef
ProtocolUpdater::ingestTurn(NodeGraph::WriteAccess &write,
                            const Value::Object &object, const NodeRef &thread,
                            std::string_view fallbackId, bool replaceItems,
                            std::optional<std::uint64_t> preserveChangesAfter,
                            bool updateCurrentRelation) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty() || !thread)
    return {};
  NodeRef turn = ensureTurn(write, thread, id);
  mergeObject(write, turn, object, "items", preserveChangesAfter);
  if (const Value::Array *items = arrayMember(object, "items")) {
    std::vector<NodeRef> order;
    order.reserve(items->size());
    std::unordered_set<const Node *> ordered;
    ordered.reserve(items->size());
    for (const Value &value : *items) {
      if (const Value::Object *itemObject = value.asObject()) {
        const std::string itemId = nestedId(*itemObject);
        NodeRef item =
            ingestItem(write, *itemObject, turn, {}, preserveChangesAfter);
        if (item && ordered.insert(item.get()).second)
          order.emplace_back(std::move(item));
      }
    }
    if (replaceItems) {
      const std::vector<NodeRef> existing =
          mergeExistingTail(write.children(turn),
                            write.related(turn, RelationKind::TurnRootItem));
      write.replaceChildren(
          turn, replaceAuthoritativeChildren(write, turn, std::move(order),
                                             existing));
    } else if (!order.empty())
      write.replaceChildren(
          turn, mergeExistingTail(std::move(order), write.children(turn)));
    if (replaceItems) {
      NodeRef root;
      for (const NodeRef &candidate : write.children(turn)) {
        if (candidate && isUserMessage(write.state(candidate)->fields)) {
          root = candidate;
          break;
        }
      }
      replaceSingleRelation(write, turn, RelationKind::TurnRootItem, root);
      reconcileAgentChildRelations(write, write.parent(turn));
    }
  }
  if (updateCurrentRelation)
    updateActiveTurn(write, thread, turn);
  return turn;
}

NodeRef
ProtocolUpdater::ingestItem(NodeGraph::WriteAccess &write,
                            const Value::Object &object, const NodeRef &turn,
                            std::string_view fallbackId,
                            std::optional<std::uint64_t> preserveChangesAfter) {
  const std::string id = nestedId(object, fallbackId);
  if (id.empty() || !turn)
    return {};
  NodeRef item = ensureItem(write, turn, id);
  mergeObject(write, item, object, {}, preserveChangesAfter);
  boundRetainedItemText(write, item, object, preserveChangesAfter);
  correlateLocalPrompt(write, item, object);
  if (isUserMessage(object)) {
    const std::vector<NodeRef> roots =
        write.related(turn, RelationKind::TurnRootItem);
    bool claimsTurnRoot = roots.empty();
    if (!claimsTurnRoot) {
      const std::vector<NodeRef> prompts =
          write.related(item, RelationKind::PromptMaterialization);
      claimsTurnRoot = std::ranges::any_of(
          prompts, [&write, &roots](const NodeRef &prompt) {
            if (std::ranges::find(roots, prompt) == roots.end())
              return false;
            const Value *dispatch =
                member(write.state(prompt)->fields, "dispatchState");
            return canonicalValue(dispatch) == "awaitingMaterialization";
          });
    }
    if (claimsTurnRoot)
      replaceSingleRelation(write, turn, RelationKind::TurnRootItem, item);
  }

  const bool hasAgentChildren =
      member(object, "agentThreadId") || member(object, "receiverThreadIds");
  if (hasAgentChildren) {
    const std::vector<std::string> childThreadIds =
        agentChildIds(write.state(item)->fields);
    const std::vector<NodeRef> previous =
        write.related(item, RelationKind::AgentChildThread);
    std::vector<NodeRef> children;
    children.reserve(childThreadIds.size());
    for (const std::string &childThreadId : childThreadIds)
      children.emplace_back(write.upsert({NodeKind::Thread, childThreadId}));
    write.replaceRelated(item, RelationKind::AgentChildThread, children);
    NodeRef owner = turn ? write.parent(turn) : NodeRef{};
    if (owner && previous != children)
      reconcileAgentChildRelations(write, owner);
  }
  return item;
}

void ProtocolUpdater::reconcileAgentChildRelations(
    NodeGraph::WriteAccess &write, const NodeRef &thread) {
  if (!thread)
    return;
  const std::vector<NodeRef> previous =
      write.related(thread, RelationKind::AgentChildThread);
  const std::vector<NodeRef> referenced =
      referencedAgentChildren(write, thread);

  for (const NodeRef &child : referenced)
    assignThreadOwner(write, thread, RelationKind::AgentChildThread, child);
  write.replaceRelated(thread, RelationKind::AgentChildThread, referenced);

  const std::vector<NodeRef> structural =
      write.related(thread, RelationKind::StructuralChildThread);
  for (const NodeRef &released : previous) {
    if (std::find(referenced.begin(), referenced.end(), released) !=
        referenced.end())
      continue;
    if (std::find(structural.begin(), structural.end(), released) ==
        structural.end())
      write.unrelate(released, RelationKind::ThreadOwner, thread);
    if (!hasThreadOwner(write, released))
      admitRootThread(write, released, false);
  }
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
  std::unordered_set<const Node *> listedSet;
  listedSet.reserve(threads.size());
  for (const Value &value : threads) {
    const Value::Object *object = value.asObject();
    if (!object)
      continue;
    NodeRef thread = ingestThread(write, *object);
    if (thread && listedSet.insert(thread.get()).second)
      listed.emplace_back(std::move(thread));
  }

  std::vector<NodeRef> roots;
  roots.reserve(listed.size() + previous.size());
  std::unordered_set<const Node *> rootSet;
  rootSet.reserve(listed.size() + previous.size());
  for (const NodeRef &thread : listed) {
    if (!hasThreadOwner(write, thread) && rootSet.insert(thread.get()).second)
      roots.emplace_back(thread);
  }
  for (const NodeRef &thread : previous) {
    if (!hasThreadOwner(write, thread) && rootSet.insert(thread.get()).second)
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

  std::vector<NodeRef> localPrompts;
  for (const NodeRef &descendant : descendants) {
    if (isLocalPrompt(write, descendant))
      localPrompts.emplace_back(descendant);
  }

  NodeRef recoveryThread;
  if (!localPrompts.empty()) {
    if (!runtime)
      runtime = write.upsert({NodeKind::Runtime, "runtime"});
    const std::string recoverySuffix =
        std::to_string(write.revision() + 1) + ':' + thread->id().canonical;
    NodeState recoveryState;
    recoveryState.status = NodeStatus::Failed;
    recoveryState.fields = {{"type", Value("localRecoveryThread")},
                            {"local", Value(true)},
                            {"recoveryOnly", Value(true)},
                            {"name", Value("Unsent prompt")}};
    recoveryThread = write.upsert(
        {NodeKind::Thread, "local-recovery-thread:removed:" + recoverySuffix},
        std::move(recoveryState));

    std::size_t promptIndex = 0;
    for (const NodeRef &prompt : localPrompts) {
      NodeState turnState;
      turnState.status = NodeStatus::Failed;
      turnState.fields = {{"type", Value("localRecoveryTurn")},
                          {"local", Value(true)}};
      NodeRef recoveryTurn = write.upsert(
          {NodeKind::Turn, "local-recovery-turn:removed:" + recoverySuffix +
                               ':' + std::to_string(promptIndex++)},
          std::move(turnState));
      write.setParent(recoveryThread, recoveryTurn);
      write.setParent(recoveryTurn, prompt);
      write.setStatus(prompt, NodeStatus::Failed);
      write.setField(prompt, "dispatchState", Value("uncertain"));
      write.setField(prompt, "error",
                     Value("The destination thread was removed"));
      write.setField(prompt, "requiresExplicitRecovery", Value(true));
      write.setField(prompt, "threadId", Value(recoveryThread->id().canonical));
      write.setField(prompt, "startsTurn", Value(true));
      write.eraseField(prompt, "turnId");
      write.eraseField(prompt, "expectedTurnId");
      write.eraseField(prompt, "requestId");
      write.eraseField(prompt, "uiMaterialized");
      write.relate(runtime, RelationKind::PendingPrompt, prompt);
      write.relate(recoveryThread, RelationKind::PendingPrompt, prompt);
    }
  }

  std::vector<NodeRef> removedDescendants;
  removedDescendants.reserve(descendants.size());
  for (const NodeRef &descendant : descendants)
    if (!isLocalPrompt(write, descendant))
      removedDescendants.emplace_back(descendant);
  write.removeMany(removedDescendants);
  write.remove(thread);

  std::size_t next = std::min(insertion, roots.size());
  if (recoveryThread) {
    roots.insert(roots.begin() + static_cast<std::ptrdiff_t>(next),
                 recoveryThread);
    ++next;
  }
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
