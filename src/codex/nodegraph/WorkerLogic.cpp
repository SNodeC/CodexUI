// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/WorkerLogic.h"

#include "codex/nodegraph/PromptText.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace codexui::nodegraph {
namespace {

const Value *field(const NodeState &state, std::string_view name) {
  const auto found = state.fields.find(name);
  return found == state.fields.end() ? nullptr : &found->second;
}

std::string stringField(const NodeState &state, std::string_view name) {
  const Value *value = field(state, name);
  const std::string *text = value ? value->asString() : nullptr;
  return text ? *text : std::string{};
}

std::uint64_t unsignedField(const NodeState &state, std::string_view name) {
  const Value *value = field(state, name);
  const std::uint64_t *number = value ? value->asUInt64() : nullptr;
  return number ? *number : 0;
}

const Value *objectField(const Value::Object &object, std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

std::string objectString(const Value::Object &object, std::string_view name) {
  const Value *value = objectField(object, name);
  const std::string *text = value ? value->asString() : nullptr;
  return text ? *text : std::string{};
}

bool isProviderNotice(const DecodedMessage &message) {
  if (message.kind != DecodedMessageKind::ServerNotification)
    return false;
  const std::string_view method = message.method;
  return method == "error" || method == "warning" ||
         method == "guardianWarning" || method == "deprecationNotice" ||
         method == "configWarning" || method == "windows/worldWritableWarning";
}

std::string providerNoticeText(const DecodedMessage &message) {
  std::string text = objectString(message.payload, "message");
  if (text.empty())
    text = objectString(message.payload, "detail");
  if (text.empty()) {
    const Value *error = objectField(message.payload, "error");
    if (const Value::Object *object = error ? error->asObject() : nullptr)
      text = objectString(*object, "message");
  }
  return text.empty() ? message.method : text;
}

bool isLocalPrompt(const NodeState &state) {
  return stringField(state, "type") == "localPrompt";
}

bool hydrationResultIsUsable(NodeGraph::WriteAccess &write,
                             const DecodedMessage &result,
                             const NodeRef &thread) {
  if (!thread || write.find(thread->id()) != thread)
    return false;
  // A successfully reduced correlated result retires its transient operation
  // in the same transaction. If it is still live, the response was stale or
  // mismatched and cannot make this thread ready.
  if (result.expectedNode &&
      write.find(result.expectedNode->id()) == result.expectedNode)
    return false;
  const Value *threadValue = objectField(result.payload, "thread");
  const Value::Object *threadObject =
      threadValue ? threadValue->asObject() : nullptr;
  if (!threadObject ||
      objectString(*threadObject, "id") != thread->id().canonical)
    return false;

  std::unordered_set<const Node *> checkedItems;
  const auto itemIsUsable = [&](const NodeRef &item) {
    if (!item || item->id().kind != NodeKind::Item ||
        !checkedItems.insert(item.get()).second)
      return true;
    const NodeState &state = *write.state(item);
    return isLocalPrompt(state) || !stringField(state, "type").empty();
  };
  for (const NodeRef &turn : write.children(thread)) {
    if (!turn || turn->id().kind != NodeKind::Turn)
      continue;
    for (const NodeRef &item : write.children(turn)) {
      if (!itemIsUsable(item))
        return false;
    }
    for (const NodeRef &root :
         write.related(turn, RelationKind::TurnRootItem)) {
      if (!itemIsUsable(root))
        return false;
    }
  }
  return true;
}

bool isLocalShell(const NodeRef &node) {
  return node && (node->id().canonical.starts_with("local-thread:") ||
                  node->id().canonical.starts_with("local-turn:") ||
                  node->id().canonical.starts_with("local-recovery-thread:") ||
                  node->id().canonical.starts_with("local-recovery-turn:"));
}

Value attachmentSummaries(const std::vector<Attachment> &attachments) {
  Value::Array summaries;
  summaries.reserve(attachments.size());
  for (const Attachment &attachment : attachments) {
    summaries.emplace_back(
        Value::Object{{"path", Value(attachment.path)},
                      {"displayName", Value(attachment.displayName)},
                      {"mimeType", Value(attachment.mimeType)}});
  }
  return Value(std::move(summaries));
}

std::optional<Value::Object> takeObject(Value::Object &source,
                                        std::string_view name) {
  const auto found = source.find(name);
  if (found == source.end() || !found->second.asObject())
    return std::nullopt;
  Value::Object result = std::move(*found->second.asObject());
  source.erase(found);
  return result;
}

std::string takeString(Value::Object &source, std::string_view name) {
  const auto found = source.find(name);
  if (found == source.end() || !found->second.asString())
    return {};
  std::string result = std::move(*found->second.asString());
  source.erase(found);
  return result;
}

NodeRef containingThread(NodeGraph::WriteAccess &write, NodeRef node) {
  while (node && node->id().kind != NodeKind::Thread)
    node = write.parent(node);
  return node;
}

NodeState initialConnectionState() {
  return NodeState{NodeStatus::Unknown,
                   {{"transportState", Value(std::string{})},
                    {"transportDetail", Value(std::string{})},
                    {"connectionGeneration", Value(std::uint64_t{0})},
                    {"connectionId", Value(std::string{})},
                    {"role", Value(std::string{})},
                    {"controllerConnectionId", Value(std::string{})},
                    {"providerGeneration", Value(std::uint64_t{0})},
                    {"providerState", Value(std::string{})},
                    {"providerDetail", Value(std::string{})},
                    {"settings", Value(Value::Object{})}}};
}

NodeRef connectionNode(NodeGraph::WriteAccess &write) {
  if (NodeRef connection = write.find({NodeKind::Connection, "connection"}))
    return connection;
  return write.upsert({NodeKind::Connection, "connection"},
                      initialConnectionState());
}

WorkerGenerations connectionGenerations(NodeGraph::WriteAccess &write) {
  const NodeRef connection = write.find({NodeKind::Connection, "connection"});
  if (!connection)
    return {};
  const std::shared_ptr<const NodeState> state = write.state(connection);
  return {unsignedField(*state, "connectionGeneration"),
          unsignedField(*state, "providerGeneration")};
}

NodeStatus transportStatus(std::string_view state) {
  if (state == "connected")
    return NodeStatus::Connected;
  if (state == "connecting" || state == "retrying")
    return NodeStatus::Pending;
  if (state == "disconnected" || state == "failure")
    return NodeStatus::Disconnected;
  return NodeStatus::Unknown;
}

void clearBridgeFields(NodeGraph::WriteAccess &write, const NodeRef &connection,
                       bool clearProviderGeneration) {
  write.setField(connection, "connectionId", Value(std::string{}));
  write.setField(connection, "role", Value(std::string{}));
  write.setField(connection, "controllerConnectionId", Value(std::string{}));
  if (clearProviderGeneration)
    write.setField(connection, "providerGeneration", Value(std::uint64_t{0}));
  write.setField(connection, "providerState", Value(std::string{}));
  write.setField(connection, "providerDetail", Value(std::string{}));
}

} // namespace

WorkerLogic::WorkerLogic(NodeGraph &graph, ThreadChannels &channels) noexcept
    : graph_(graph), channels_(channels), updater_(graph) {}

ChannelSendStatus WorkerLogic::apply(DecodedMessage message) {
  return applyDetailed(std::move(message)).status;
}

WorkerApplyResult WorkerLogic::applyDetailed(DecodedMessage message) {
  std::optional<UiEffect> noticeEffect;
  if (isProviderNotice(message)) {
    const std::uint64_t serial = nextNoticeSerial_++;
    const std::string severity =
        message.method == "error" ? "error" : "warning";
    const std::string notice = providerNoticeText(message);
    message.payload.insert_or_assign("noticeSerial", Value(serial));
    message.payload.insert_or_assign("noticeText", Value(notice));
    message.payload.insert_or_assign("severity", Value(severity));
    noticeEffect = UiEffect{UiEffectKind::ShowNotice, std::nullopt, notice,
                            Value::Object{{"serial", Value(serial)},
                                          {"severity", Value(severity)}}};
  }

  GraphChange change;
  NodeRef primary;
  {
    auto write = graph_.write();
    if (message.kind == DecodedMessageKind::ServerRequest) {
      const WorkerGenerations current = connectionGenerations(write);
      message.connectionGeneration = current.connection;
      message.providerGeneration = current.provider;
    }
    AppliedMessage applied = updater_.applyInto(write, message);
    primary = std::move(applied.primary);
    change = write.finish();
  }
  forgetRemoved(change);
  // Queue the transient notice ahead of its GraphChanged wake. The graph is
  // already fully committed and unlocked; this FIFO order also prevents the
  // graph fallback from skipping an earlier directly queued notice.
  ChannelSendStatus effectStatus = ChannelSendStatus::Accepted;
  if (noticeEffect)
    effectStatus = channels_.sendUiEffect(*noticeEffect);
  ChannelSendStatus graphStatus = publish(std::move(change));
  if (wakeFailed(graphStatus) && !wakeFailed(effectStatus) &&
      effectStatus != ChannelSendStatus::QueueFull)
    graphStatus = effectStatus;
  return {graphStatus, std::move(primary)};
}

void WorkerLogic::forgetRemoved(const GraphChange &change) {
  for (const NodeRef &removed : change.removed) {
    if (!removed)
      continue;
    if (removed->id().kind == NodeKind::Item &&
        removed->id().canonical.starts_with("local-prompt:"))
      forgetPrompt(removed);
    if (removed->id().kind == NodeKind::Thread) {
      promptQueues_.erase(removed.get());
      promptInFlight_.erase(removed.get());
      for (auto creating = creatingThreads_.begin();
           creating != creatingThreads_.end();) {
        if (creating->second == removed)
          creating = creatingThreads_.erase(creating);
        else
          ++creating;
      }
    }
  }
}

WorkerGenerations WorkerLogic::generations() const {
  auto write = graph_.write();
  const WorkerGenerations current = connectionGenerations(write);
  static_cast<void>(write.finish());
  return current;
}

ChannelSendStatus WorkerLogic::transportEvent(std::string state,
                                              std::string detail) {
  const bool connected = state == "connected";
  const bool clearsBridge = state == "connecting" || state == "retrying" ||
                            state == "disconnected" || state == "failure";
  const bool resetProvider =
      connected || (clearsBridge && state != "connecting");
  std::string resetReason;
  if (connected) {
    resetReason = "A new app-server connection replaced provider state";
  } else if (resetProvider) {
    resetReason =
        detail.empty() ? "The app-server connection was reset" : detail;
  }

  GraphChange change;
  {
    auto write = graph_.write();
    const NodeRef connection = connectionNode(write);
    const std::uint64_t generation =
        connectionGenerations(write).connection + (connected ? 1U : 0U);
    write.setStatus(connection, transportStatus(state));
    write.setField(connection, "transportState", Value(std::move(state)));
    write.setField(connection, "transportDetail",
                   Value(connected ? std::string{} : std::move(detail)));
    write.setField(connection, "connectionGeneration", Value(generation));
    if (connected || clearsBridge)
      clearBridgeFields(write, connection, connected);
    if (resetProvider)
      resetProviderDerived(write, resetReason);
    change = write.finish();
  }
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::bridgeState(
    std::string connectionId, std::string role,
    std::string controllerConnectionId, std::uint64_t providerGeneration,
    std::optional<std::string> providerState, std::string detail) {
  GraphChange change;
  {
    auto write = graph_.write();
    const NodeRef connection = connectionNode(write);
    const std::uint64_t currentProviderGeneration =
        connectionGenerations(write).provider;
    if (providerState && providerGeneration < currentProviderGeneration) {
      change = write.finish();
    } else {
      const bool resetProvider =
          providerState && providerGeneration > currentProviderGeneration;
      const std::string resetReason =
          resetProvider
              ? (detail.empty() ? "The provider generation changed" : detail)
              : std::string{};
      write.setField(connection, "connectionId",
                     Value(std::move(connectionId)));
      write.setField(connection, "role", Value(std::move(role)));
      write.setField(connection, "controllerConnectionId",
                     Value(std::move(controllerConnectionId)));
      if (providerState) {
        write.setField(connection, "providerGeneration",
                       Value(providerGeneration));
        write.setField(connection, "providerState",
                       Value(std::move(*providerState)));
        write.setField(connection, "providerDetail", Value(std::move(detail)));
      }
      if (resetProvider)
        resetProviderDerived(write, resetReason);
      change = write.finish();
    }
  }
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::connectionSettings(Value::Object settings) {
  GraphChange change;
  {
    auto write = graph_.write();
    write.setField(connectionNode(write), "settings",
                   Value(std::move(settings)));
    change = write.finish();
  }
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::threadHydration(const NodeRef &thread,
                                               std::string state,
                                               std::string error) {
  GraphChange change;
  {
    auto write = graph_.write();
    updateThreadHydration(write, thread, std::move(state), std::move(error));
    change = write.finish();
  }
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::completeThreadHydration(DecodedMessage result,
                                                       const NodeRef &thread,
                                                       std::string state,
                                                       std::string error) {
  GraphChange change;
  {
    auto write = graph_.write();
    static_cast<void>(updater_.applyInto(write, result));
    if (state == "ready" && !hydrationResultIsUsable(write, result, thread)) {
      state = "failed";
      if (error.empty())
        error = "Thread hydration returned incomplete item identity";
    }
    updateThreadHydration(write, thread, std::move(state), std::move(error));
    change = write.finish();
  }
  forgetRemoved(change);
  return publish(std::move(change));
}

void WorkerLogic::updateThreadHydration(NodeGraph::WriteAccess &write,
                                        const NodeRef &thread,
                                        std::string state, std::string error) {
  if (!thread || thread->id().kind != NodeKind::Thread ||
      write.find(thread->id()) != thread)
    return;
  write.setField(thread, "hydrationState", Value(std::move(state)));
  const WorkerGenerations current = connectionGenerations(write);
  write.setField(thread, "hydrationConnectionGeneration",
                 Value(current.connection));
  if (error.empty())
    write.eraseField(thread, "hydrationError");
  else
    write.setField(thread, "hydrationError", Value(std::move(error)));
}

std::vector<NodeRef> WorkerLogic::activeAgentChildren(const NodeRef &thread) {
  std::vector<NodeRef> result;
  auto write = graph_.write();
  if (!thread || thread->id().kind != NodeKind::Thread ||
      write.find(thread->id()) != thread) {
    static_cast<void>(write.finish());
    return result;
  }

  std::unordered_set<const Node *> seen;
  for (const NodeRef &turn : write.children(thread)) {
    if (!turn || turn->id().kind != NodeKind::Turn)
      continue;
    for (const NodeRef &item : write.children(turn)) {
      if (!item || item->id().kind != NodeKind::Item)
        continue;
      const std::shared_ptr<const NodeState> state = write.state(item);
      const std::string status = stringField(*state, "status");
      const bool active = state->status == NodeStatus::Pending ||
                          state->status == NodeStatus::Running ||
                          status == "pending" || status == "queued" ||
                          status == "running" || status == "active" ||
                          status == "inProgress";
      if (!active)
        continue;
      for (const NodeRef &child :
           write.related(item, RelationKind::AgentChildThread)) {
        if (child && child->id().kind == NodeKind::Thread &&
            write.find(child->id()) == child && seen.insert(child.get()).second)
          result.emplace_back(child);
      }
    }
  }
  static_cast<void>(write.finish());
  return result;
}

ChannelSendStatus WorkerLogic::showNotice(std::string message) {
  if (message.empty())
    return ChannelSendStatus::Accepted;
  const std::uint64_t serial = nextNoticeSerial_++;
  UiEffect effect{UiEffectKind::ShowNotice,
                  std::nullopt,
                  message,
                  {{"serial", Value(serial)}}};
  const ChannelSendStatus direct = channels_.sendUiEffect(effect);
  if (direct != ChannelSendStatus::QueueFull)
    return direct;

  GraphChange change;
  {
    auto write = graph_.write();
    NodeRef notice = write.upsert({NodeKind::Notice, "local-worker-notice"});
    write.setField(notice, "local", Value(true));
    write.setField(notice, "message", Value(std::move(message)));
    write.setField(notice, "noticeSerial", Value(serial));
    change = write.finish();
  }
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::selectThread(const NodeRef &thread) {
  if (!thread || thread->id().kind != NodeKind::Thread)
    return ChannelSendStatus::Accepted;
  const std::uint64_t serial = nextSelectionSerial_++;
  UiEffect effect{
      UiEffectKind::SelectThread, thread, {}, {{"serial", Value(serial)}}};
  const ChannelSendStatus direct = channels_.sendUiEffect(effect);
  if (direct != ChannelSendStatus::QueueFull)
    return direct;

  GraphChange change;
  {
    auto write = graph_.write();
    if (write.find(thread->id()) != thread)
      return ChannelSendStatus::Accepted;
    NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
    std::array<NodeRef, 1> target{thread};
    write.replaceRelated(runtime, RelationKind::UiSelectionTarget, target);
    write.setField(runtime, "uiSelectionSerial", Value(serial));
    change = write.finish();
  }
  return publish(std::move(change));
}

ChannelSendStatus
WorkerLogic::resolveInteraction(const ProtocolRequestId &requestId,
                                bool accepted, std::string error) {
  return publish(
      updater_.resolveInteraction(requestId, accepted, std::move(error)));
}

ChannelSendStatus WorkerLogic::resolveInteraction(const NodeRef &interaction,
                                                  bool accepted,
                                                  std::string error) {
  return publish(
      updater_.resolveInteraction(interaction, accepted, std::move(error)));
}

ChannelSendStatus
WorkerLogic::rejectInteractionResponse(const NodeRef &interaction,
                                       Value::Object authoredResponse,
                                       std::string error) {
  GraphChange change;
  {
    auto write = graph_.write();
    if (interaction && interaction->id().kind == NodeKind::Interaction &&
        write.find(interaction->id()) == interaction) {
      write.setStatus(interaction, NodeStatus::Failed);
      write.setField(interaction, "error", Value(std::move(error)));
      write.setField(interaction, "retainedResponsePayload",
                     Value(std::move(authoredResponse)));
      for (const NodeRef &target :
           write.related(interaction, RelationKind::InteractionTarget)) {
        NodeRef thread = containingThread(write, target);
        if (!thread)
          continue;
        std::size_t pending = 0;
        for (const NodeRef &candidate :
             write.related(thread, RelationKind::PendingInteraction)) {
          if (candidate &&
              (write.state(candidate)->status == NodeStatus::Pending ||
               write.state(candidate)->status == NodeStatus::Failed))
            ++pending;
        }
        write.setField(thread, "pendingInteractionCount",
                       Value(static_cast<std::uint64_t>(pending)));
      }
    }
    change = write.finish();
  }
  return publish(std::move(change));
}

PromptTransition
WorkerLogic::admitPrompt(NodeAction action,
                         std::optional<std::int64_t> activityAt,
                         std::optional<std::int64_t> admittedAtMs) {
  if (action.kind != NodeActionKind::SubmitPrompt || !action.target) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "The prompt has no current destination thread",
                    {}};
    return {showNotice(std::move(effect.text)), std::nullopt};
  }
  PendingPrompt pending;
  pending.localPrompt = {};
  pending.thread = std::move(action.target);
  pending.promptText = std::move(action.promptText);
  pending.attachments = std::move(action.attachments);
  pending.options = std::move(action.payload);
  pending.creationCorrelation = std::move(action.correlation);
  return admit(std::move(pending), activityAt, admittedAtMs);
}

PromptTransition
WorkerLogic::admitFirstPrompt(RuntimeAction action,
                              std::optional<std::int64_t> activityAt,
                              std::optional<std::int64_t> admittedAtMs) {
  if (action.kind != RuntimeActionKind::CreateThread) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "The new-thread prompt could not be admitted",
                    {}};
    return {showNotice(std::move(effect.text)), std::nullopt};
  }
  PendingPrompt pending;
  pending.createsThread = true;
  pending.creationCorrelation = std::move(action.correlation);
  pending.promptText = std::move(action.promptText);
  pending.attachments = std::move(action.attachments);
  std::optional<Value::Object> options =
      takeObject(action.payload, "threadStart");
  if (std::optional<Value::Object> turnOptions =
          takeObject(action.payload, "turnStart"))
    pending.turnOptions = std::move(*turnOptions);
  pending.requestedName = takeString(action.payload, "requestedName");
  pending.options = options ? std::move(*options) : std::move(action.payload);
  return admit(std::move(pending), activityAt, admittedAtMs);
}

PromptTransition WorkerLogic::admit(PendingPrompt pending,
                                    std::optional<std::int64_t> activityAt,
                                    std::optional<std::int64_t> admittedAtMs) {
  if (pending.promptText.empty() && pending.attachments.empty()) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "Enter a message or attach a file before sending",
                    {}};
    return {showNotice(std::move(effect.text)), std::nullopt};
  }

  const std::uint64_t submission = nextSubmissionId_++;
  const std::string suffix = std::to_string(submission);
  pending.clientUserMessageId = "codexui-" + suffix;
  if (pending.createsThread && pending.creationCorrelation.empty())
    pending.creationCorrelation = "worker-draft:" + suffix;

  GraphChange change;
  NodeRef selectedDraft;
  std::optional<PromptCommand> command;
  bool invalidTarget = false;
  std::string invalidTargetReason;
  {
    auto write = graph_.write();
    if (pending.createsThread) {
      if (const auto creating =
              creatingThreads_.find(pending.creationCorrelation);
          creating != creatingThreads_.end() && creating->second &&
          write.find(creating->second->id()) == creating->second) {
        pending.thread = creating->second;
        pending.createsThread = false;
        pending.options = std::move(pending.turnOptions);
      }
    }
    if (pending.createsThread) {
      NodeState threadState;
      threadState.status = NodeStatus::Pending;
      threadState.fields = {{"type", Value("localThread")},
                            {"local", Value(true)}};
      if (!pending.requestedName.empty())
        threadState.fields.emplace("name", Value(pending.requestedName));
      pending.thread = write.upsert(
          {NodeKind::Thread, "local-thread:" + suffix}, std::move(threadState));
      creatingThreads_.insert_or_assign(pending.creationCorrelation,
                                        pending.thread);
      NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
      std::vector<NodeRef> roots =
          write.related(runtime, RelationKind::RootThread);
      roots.erase(std::remove(roots.begin(), roots.end(), pending.thread),
                  roots.end());
      roots.insert(roots.begin(), pending.thread);
      write.replaceRelated(runtime, RelationKind::RootThread, roots);
      selectedDraft = pending.thread;
    } else {
      const NodeRef current = write.find(pending.thread->id());
      if (current != pending.thread ||
          pending.thread->id().kind != NodeKind::Thread) {
        invalidTarget = true;
        invalidTargetReason = "The destination thread is no longer available";
      } else {
        const Value *recoveryOnly =
            field(*write.state(current), "recoveryOnly");
        if (recoveryOnly && recoveryOnly->asBool() && *recoveryOnly->asBool()) {
          invalidTarget = true;
          invalidTargetReason =
              "Restore this unsent prompt before sending it again";
        }
        const std::string hydration =
            stringField(*write.state(current), "hydrationState");
        if (!invalidTarget &&
            (hydration == "loading" || hydration == "failed" ||
             (write.state(current)->status == NodeStatus::NotLoaded &&
              hydration != "ready"))) {
          invalidTarget = true;
          invalidTargetReason =
              hydration == "failed"
                  ? "Reload this thread before sending the preserved prompt"
                  : "Wait for this thread to finish loading before sending";
        }
      }
    }

    if (invalidTarget) {
      // Queue admission already moved the user's draft off Qt-main. Preserve
      // it as an explicit failed local prompt if its target disappeared or
      // became recovery-only before the worker consumed the command.
      NodeState threadState;
      threadState.status = NodeStatus::Failed;
      threadState.fields = {{"type", Value("localRecoveryThread")},
                            {"local", Value(true)},
                            {"recoveryOnly", Value(true)},
                            {"name", Value("Unsent prompt")}};
      pending.thread =
          write.upsert({NodeKind::Thread, "local-recovery-thread:" + suffix},
                       std::move(threadState));
      NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
      std::vector<NodeRef> roots =
          write.related(runtime, RelationKind::RootThread);
      roots.insert(roots.begin(), pending.thread);
      write.replaceRelated(runtime, RelationKind::RootThread, roots);
      selectedDraft = pending.thread;
    }

    if (!pending.thread) {
      change = write.finish();
    } else {
      NodeRef turn = activeTurn(write, pending.thread);
      const bool startsTurn = !turn;
      if (!turn) {
        NodeState turnState;
        turnState.status =
            invalidTarget ? NodeStatus::Failed : NodeStatus::Pending;
        turnState.fields = {
            {"type", Value(invalidTarget ? "localRecoveryTurn" : "localTurn")},
            {"local", Value(true)}};
        turn = write.upsert(
            {NodeKind::Turn,
             (invalidTarget ? "local-recovery-turn:" : "local-turn:") + suffix},
            std::move(turnState));
        write.setParent(pending.thread, turn);
      }

      NodeState promptState;
      promptState.status =
          invalidTarget ? NodeStatus::Failed : NodeStatus::Pending;
      promptState.fields = {
          {"type", Value("localPrompt")},
          {"local", Value(true)},
          {"submissionId", Value(submission)},
          {"clientUserMessageId", Value(pending.clientUserMessageId)},
          {"authoredText", Value(pending.promptText)},
          {"text", Value(composePromptMarkdown(pending.promptText,
                                               pending.attachments))},
          {"attachments", attachmentSummaries(pending.attachments)},
          {"dispatchState", Value(invalidTarget ? "failed" : "queued")},
          {"startsTurn", Value(startsTurn)},
          {"createsThread", Value(pending.createsThread)},
          {"threadId", Value(pending.thread->id().canonical)}};
      if (!pending.creationCorrelation.empty())
        promptState.fields.emplace("creationCorrelation",
                                   Value(pending.creationCorrelation));
      if (pending.createsThread) {
        promptState.fields.emplace("threadStartOptions",
                                   Value(pending.options));
        promptState.fields.emplace("turnStartOptions",
                                   Value(pending.turnOptions));
        if (!pending.requestedName.empty())
          promptState.fields.emplace("requestedName",
                                     Value(pending.requestedName));
      }
      if (invalidTarget) {
        promptState.fields.emplace("error", Value(invalidTargetReason));
        promptState.fields.emplace("requiresExplicitRecovery", Value(true));
      }
      if (admittedAtMs)
        promptState.fields.emplace("admittedAtMs", Value(*admittedAtMs));
      if (!startsTurn)
        promptState.fields.emplace(
            "expectedTurnId",
            Value(protocolCanonicalId(*write.state(turn), turn)));
      pending.localPrompt = write.upsert(
          {NodeKind::Item, "local-prompt:" + suffix}, std::move(promptState));
      write.setParent(turn, pending.localPrompt);
      NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
      write.relate(runtime, RelationKind::PendingPrompt, pending.localPrompt);
      write.relate(pending.thread, RelationKind::PendingPrompt,
                   pending.localPrompt);
      if (activityAt)
        advancePromptActivity(write, pending.thread, *activityAt);
      if (!invalidTarget) {
        const NodeRef ownerThread = pending.thread;
        promptQueues_[pending.thread.get()].emplace_back(std::move(pending));
        command = takeNextPrompt(write, ownerThread);
      }
      change = write.finish();
    }
  }

  if (selectedDraft) {
    static_cast<void>(selectThread(selectedDraft));
  }
  if (invalidTarget) {
    static_cast<void>(showNotice(std::move(invalidTargetReason)));
  }
  return {publish(std::move(change)), std::move(command)};
}

void WorkerLogic::advancePromptActivity(NodeGraph::WriteAccess &write,
                                        const NodeRef &target,
                                        std::int64_t proposedActivityAt) {
  std::int64_t activityAt = proposedActivityAt;
  const auto retainMaximum = [&activityAt](const Value *value) {
    if (!value)
      return;
    if (const std::int64_t *number = value->asInt64()) {
      if (*number >= activityAt && *number < INT64_MAX)
        activityAt = *number + 1;
    } else if (const std::uint64_t *number = value->asUInt64()) {
      if (*number >= static_cast<std::uint64_t>(
                         std::max<std::int64_t>(0, activityAt)) &&
          *number < static_cast<std::uint64_t>(INT64_MAX))
        activityAt = static_cast<std::int64_t>(*number) + 1;
    }
  };
  for (const NodeRef &node : write.orderedNodes()) {
    if (!node || node->id().kind != NodeKind::Thread)
      continue;
    const std::shared_ptr<const NodeState> state = write.state(node);
    for (const std::string_view key :
         {std::string_view("updatedAt"), std::string_view("recencyAt"),
          std::string_view("localPromptActivityAt")}) {
      retainMaximum(field(*state, key));
    }
  }

  NodeRef thread = target;
  std::unordered_set<const Node *> visited;
  while (thread && visited.insert(thread.get()).second) {
    write.setField(thread, "localPromptActivityAt", Value(activityAt));
    write.setField(thread, "localActivityAt", Value(activityAt));

    const std::vector<NodeRef> owners =
        write.related(thread, RelationKind::ThreadOwner);
    thread = owners.empty() ? NodeRef{} : owners.front();
  }
}

NodeRef WorkerLogic::activeTurn(NodeGraph::WriteAccess &write,
                                const NodeRef &thread) const {
  const std::vector<NodeRef> indexed =
      write.related(thread, RelationKind::ActiveTurn);
  if (!indexed.empty() && indexed.front() &&
      indexed.front()->id().kind == NodeKind::Turn)
    return indexed.front();
  return {};
}

std::optional<PromptCommand>
WorkerLogic::takeNextPrompt(NodeGraph::WriteAccess &write,
                            const NodeRef &thread) {
  if (!thread || promptInFlight_.contains(thread.get()))
    return std::nullopt;
  auto queue = promptQueues_.find(thread.get());
  while (queue != promptQueues_.end() && !queue->second.empty()) {
    PendingPrompt pending = std::move(queue->second.front());
    queue->second.pop_front();
    if (write.find(pending.localPrompt->id()) != pending.localPrompt)
      continue;

    PromptCommand command;
    command.kind = pending.createsThread ? PromptCommandKind::CreateThread
                                         : PromptCommandKind::StartTurn;
    command.localPrompt = pending.localPrompt;
    command.thread = thread;
    command.clientUserMessageId = std::move(pending.clientUserMessageId);
    command.promptText = std::move(pending.promptText);
    command.attachments = std::move(pending.attachments);
    command.options = std::move(pending.options);
    command.turnOptions = std::move(pending.turnOptions);
    command.requestedName = std::move(pending.requestedName);
    command.creationCorrelation = std::move(pending.creationCorrelation);

    if (!pending.createsThread) {
      if (NodeRef running = activeTurn(write, thread)) {
        command.kind = PromptCommandKind::SteerTurn;
        command.expectedTurnId =
            protocolCanonicalId(*write.state(running), running);
        NodeRef oldParent = write.parent(command.localPrompt);
        write.setParent(running, command.localPrompt);
        if (oldParent && oldParent != running && isLocalShell(oldParent) &&
            write.children(oldParent).empty())
          write.remove(oldParent);
        write.setField(command.localPrompt, "startsTurn", Value(false));
        write.setField(command.localPrompt, "expectedTurnId",
                       Value(command.expectedTurnId));
      } else {
        write.setField(command.localPrompt, "startsTurn", Value(true));
        write.eraseField(command.localPrompt, "expectedTurnId");
      }
    }
    write.setField(command.localPrompt, "dispatchState", Value("dispatching"));
    promptInFlight_.insert_or_assign(thread.get(), command.localPrompt);
    if (queue->second.empty())
      promptQueues_.erase(queue);
    return command;
  }
  if (queue != promptQueues_.end())
    promptQueues_.erase(queue);
  return std::nullopt;
}

ChannelSendStatus WorkerLogic::attachCreatedThread(PromptCommand &command,
                                                   std::string threadId,
                                                   NodeState providerState) {
  NodeRef authoritative;
  GraphChange change;
  bool attached = false;
  {
    auto write = graph_.write();
    attached = attachCreatedThread(write, command, std::move(threadId),
                                   std::move(providerState), authoritative);
    change = write.finish();
  }
  forgetRemoved(change);
  const ChannelSendStatus status = publish(std::move(change));
  if (!attached)
    return status;

  command.thread = authoritative;
  command.kind = PromptCommandKind::StartTurn;
  command.expectedTurnId.clear();
  command.options = std::move(command.turnOptions);
  static_cast<void>(selectThread(authoritative));
  return status;
}

ChannelSendStatus WorkerLogic::completeCreatedThread(DecodedMessage result,
                                                     PromptCommand &command,
                                                     std::string threadId,
                                                     NodeState providerState) {
  NodeRef authoritative;
  GraphChange change;
  bool attached = false;
  {
    auto write = graph_.write();
    static_cast<void>(updater_.applyInto(write, result));
    attached = attachCreatedThread(write, command, std::move(threadId),
                                   std::move(providerState), authoritative);
    change = write.finish();
  }
  forgetRemoved(change);
  const ChannelSendStatus status = publish(std::move(change));
  if (!attached)
    return status;

  command.thread = authoritative;
  command.kind = PromptCommandKind::StartTurn;
  command.expectedTurnId.clear();
  command.options = std::move(command.turnOptions);
  static_cast<void>(selectThread(authoritative));
  return status;
}

bool WorkerLogic::attachCreatedThread(NodeGraph::WriteAccess &write,
                                      PromptCommand &command,
                                      std::string threadId,
                                      NodeState providerState,
                                      NodeRef &authoritative) {
  if (command.kind != PromptCommandKind::CreateThread || threadId.empty() ||
      !command.thread || !command.localPrompt)
    return false;

  NodeRef draft = write.find(command.thread->id());
  if (draft != command.thread ||
      write.find(command.localPrompt->id()) != command.localPrompt)
    return false;
  const std::shared_ptr<const NodeState> draftState = write.state(draft);
  authoritative = write.find({NodeKind::Thread, threadId});
  if (!authoritative)
    authoritative = write.upsert({NodeKind::Thread, std::move(threadId)},
                                 std::move(providerState));
  for (const std::string_view key :
       {std::string_view("localActivityAt"),
        std::string_view("localPromptActivityAt")}) {
    if (const Value *value = field(*draftState, key))
      write.setField(authoritative, std::string(key), *value);
  }

  const std::vector<NodeRef> draftChildren = write.children(draft);
  for (const NodeRef &child : draftChildren)
    write.setParent(authoritative, child);
  const std::vector<NodeRef> prompts =
      write.related(draft, RelationKind::PendingPrompt);
  for (const NodeRef &prompt : prompts) {
    write.relate(authoritative, RelationKind::PendingPrompt, prompt);
    write.setField(prompt, "threadId", Value(authoritative->id().canonical));
  }

  if (NodeRef runtime = write.find({NodeKind::Runtime, "runtime"})) {
    std::vector<NodeRef> roots =
        write.related(runtime, RelationKind::RootThread);
    roots.erase(std::remove(roots.begin(), roots.end(), draft), roots.end());
    roots.erase(std::remove(roots.begin(), roots.end(), authoritative),
                roots.end());
    roots.insert(roots.begin(), authoritative);
    write.replaceRelated(runtime, RelationKind::RootThread, roots);
  }

  if (auto queued = promptQueues_.find(draft.get());
      queued != promptQueues_.end()) {
    std::deque<PendingPrompt> moved = std::move(queued->second);
    promptQueues_.erase(queued);
    std::deque<PendingPrompt> &destination = promptQueues_[authoritative.get()];
    for (PendingPrompt &entry : moved) {
      entry.thread = authoritative;
      destination.emplace_back(std::move(entry));
    }
  }
  if (auto inFlight = promptInFlight_.find(draft.get());
      inFlight != promptInFlight_.end()) {
    NodeRef prompt = std::move(inFlight->second);
    promptInFlight_.erase(inFlight);
    promptInFlight_.insert_or_assign(authoritative.get(), std::move(prompt));
  }
  if (!command.creationCorrelation.empty()) {
    const auto creating = creatingThreads_.find(command.creationCorrelation);
    if (creating != creatingThreads_.end() && creating->second == draft)
      creatingThreads_.erase(creating);
  }
  write.remove(draft);
  return true;
}

ChannelSendStatus
WorkerLogic::markPromptDispatched(const NodeRef &localPrompt,
                                  const ProtocolRequestId &requestId) {
  GraphChange change;
  {
    auto write = graph_.write();
    if (!localPrompt || write.find(localPrompt->id()) != localPrompt)
      return ChannelSendStatus::Accepted;
    write.setField(localPrompt, "dispatchState", Value("inFlight"));
    write.setField(localPrompt, "requestId", Value(requestId.canonical()));
    write.setStatus(localPrompt, NodeStatus::Running);
    change = write.finish();
  }
  return publish(std::move(change));
}

PromptTransition
WorkerLogic::completePrompt(const NodeRef &localPrompt, bool accepted,
                            std::string error,
                            std::optional<std::string> turnId) {
  GraphChange change;
  std::optional<PromptCommand> next;
  {
    auto write = graph_.write();
    next = completePrompt(write, localPrompt, accepted, std::move(error),
                          std::move(turnId));
    change = write.finish();
  }
  forgetRemoved(change);
  return {publish(std::move(change)), std::move(next)};
}

PromptTransition WorkerLogic::completePromptResult(
    DecodedMessage result, const NodeRef &localPrompt, bool accepted,
    std::string error, std::optional<std::string> turnId) {
  GraphChange change;
  std::optional<PromptCommand> next;
  {
    auto write = graph_.write();
    bool requiresRecovery = false;
    if (localPrompt && write.find(localPrompt->id()) == localPrompt) {
      const std::shared_ptr<const NodeState> state = write.state(localPrompt);
      const Value *value = field(*state, "requiresExplicitRecovery");
      requiresRecovery = value && value->asBool() && *value->asBool();
    }
    if (requiresRecovery) {
      // Deletion or a provider-generation reset has already made delivery
      // uncertain. Retire only the exact operation; a late result must not
      // recreate the removed destination or silently acknowledge the prompt.
      if (result.expectedNode &&
          write.find(result.expectedNode->id()) == result.expectedNode)
        write.remove(result.expectedNode);
      forgetPrompt(localPrompt);
    } else {
      static_cast<void>(updater_.applyInto(write, result));
      next = completePrompt(write, localPrompt, accepted, std::move(error),
                            std::move(turnId));
    }
    change = write.finish();
  }
  forgetRemoved(change);
  return {publish(std::move(change)), std::move(next)};
}

std::optional<PromptCommand> WorkerLogic::completePrompt(
    NodeGraph::WriteAccess &write, const NodeRef &localPrompt, bool accepted,
    std::string error, std::optional<std::string> turnId) {
  if (!localPrompt || write.find(localPrompt->id()) != localPrompt)
    return std::nullopt;
  const std::shared_ptr<const NodeState> initialState =
      write.state(localPrompt);
  const Value *recovery = field(*initialState, "requiresExplicitRecovery");
  if (recovery && recovery->asBool() && *recovery->asBool()) {
    forgetPrompt(localPrompt);
    return std::nullopt;
  }
  NodeRef thread = containingThread(write, localPrompt);
  if (!thread)
    return std::nullopt;
  if (auto inFlight = promptInFlight_.find(thread.get());
      inFlight != promptInFlight_.end() && inFlight->second == localPrompt)
    promptInFlight_.erase(inFlight);

  const std::shared_ptr<const NodeState> promptState = write.state(localPrompt);
  const Value *materialized = field(*promptState, "uiMaterialized");
  const bool uiMaterialized =
      materialized && materialized->asBool() && *materialized->asBool();
  const Value *startsTurnValue = field(*promptState, "startsTurn");
  const bool startsTurn = startsTurnValue && startsTurnValue->asBool() &&
                          *startsTurnValue->asBool();

  if (uiMaterialized) {
    NodeRef provisionalTurn = write.parent(localPrompt);
    write.remove(localPrompt);
    if (provisionalTurn && isLocalShell(provisionalTurn) &&
        write.children(provisionalTurn).empty())
      write.remove(provisionalTurn);
    if (thread && isLocalShell(thread) && write.children(thread).empty())
      write.remove(thread);
  } else if (accepted) {
    write.setField(localPrompt, "dispatchState",
                   Value("awaitingMaterialization"));
    write.eraseField(localPrompt, "error");
    write.eraseField(localPrompt, "requiresExplicitRecovery");
    write.setStatus(localPrompt, NodeStatus::Running);
    if (turnId && !turnId->empty()) {
      NodeRef turn =
          write.upsert(scopedTurnNodeId(thread->id().canonical, *turnId));
      write.setField(turn, "protocolId", Value(*turnId));
      write.setField(turn, "protocolThreadId", Value(thread->id().canonical));
      if (!write.parent(turn))
        write.setParent(thread, turn);
      const NodeStatus turnStatus = write.state(turn)->status;
      if (turnStatus == NodeStatus::Unknown ||
          turnStatus == NodeStatus::Pending ||
          turnStatus == NodeStatus::NotLoaded)
        write.setStatus(turn, NodeStatus::Running);
      const NodeStatus currentTurnStatus = write.state(turn)->status;
      if (currentTurnStatus == NodeStatus::Running) {
        const std::array<NodeRef, 1> activeTurn{turn};
        write.replaceRelated(thread, RelationKind::ActiveTurn, activeTurn);
      } else
        write.unrelate(thread, RelationKind::ActiveTurn, turn);
      NodeRef previous = write.parent(localPrompt);
      write.setParent(turn, localPrompt);
      if (startsTurn) {
        NodeRef materializedItem;
        std::vector<NodeRef> ordered = write.children(turn);
        for (const NodeRef &candidate : ordered) {
          if (!candidate || candidate == localPrompt ||
              candidate->id().kind != NodeKind::Item)
            continue;
          const std::vector<NodeRef> prompts =
              write.related(candidate, RelationKind::PromptMaterialization);
          if (std::find(prompts.begin(), prompts.end(), localPrompt) !=
              prompts.end()) {
            materializedItem = candidate;
            break;
          }
        }
        ordered.erase(std::remove(ordered.begin(), ordered.end(), localPrompt),
                      ordered.end());
        if (materializedItem)
          ordered.erase(
              std::remove(ordered.begin(), ordered.end(), materializedItem),
              ordered.end());
        ordered.insert(ordered.begin(), localPrompt);
        if (materializedItem)
          ordered.insert(ordered.begin() + 1, materializedItem);
        write.replaceChildren(turn, ordered);
      }
      if (previous && previous != turn && isLocalShell(previous) &&
          write.children(previous).empty())
        write.remove(previous);
      write.setField(localPrompt, "turnId", Value(*turnId));
    }
  } else {
    const std::string failure =
        error.empty() ? "Prompt submission failed" : std::move(error);
    write.setField(localPrompt, "dispatchState", Value("failed"));
    write.setField(localPrompt, "error", Value(failure));
    write.setField(localPrompt, "requiresExplicitRecovery", Value(true));
    write.setStatus(localPrompt, NodeStatus::Failed);

    // A failed thread/start has no canonical destination for later prompts.
    // Keep every authored draft visible as failed; never turn a queued draft
    // into an automatic request against the local-only thread id.
    if (thread->id().canonical.starts_with("local-thread:")) {
      const std::string correlation =
          stringField(*write.state(localPrompt), "creationCorrelation");
      if (!correlation.empty()) {
        const auto creating = creatingThreads_.find(correlation);
        if (creating != creatingThreads_.end() && creating->second == thread)
          creatingThreads_.erase(creating);
      }
      if (auto queued = promptQueues_.find(thread.get());
          queued != promptQueues_.end()) {
        for (const PendingPrompt &pending : queued->second) {
          if (!pending.localPrompt ||
              write.find(pending.localPrompt->id()) != pending.localPrompt)
            continue;
          write.setField(pending.localPrompt, "dispatchState", Value("failed"));
          write.setField(pending.localPrompt, "error", Value(failure));
          write.setField(pending.localPrompt, "requiresExplicitRecovery",
                         Value(true));
          write.setStatus(pending.localPrompt, NodeStatus::Failed);
        }
        promptQueues_.erase(queued);
      }
    }
  }
  if (accepted || !thread->id().canonical.starts_with("local-thread:"))
    return takeNextPrompt(write, thread);
  return std::nullopt;
}

PromptTransition WorkerLogic::failPrompt(const NodeRef &localPrompt,
                                         std::string error) {
  return completePrompt(localPrompt, false, std::move(error));
}

ChannelSendStatus WorkerLogic::promptMaterialized(const NodeRef &localPrompt) {
  GraphChange change;
  bool retainedForResult = false;
  {
    auto write = graph_.write();
    if (!localPrompt || write.find(localPrompt->id()) != localPrompt)
      return ChannelSendStatus::Accepted;
    NodeRef turn = write.parent(localPrompt);
    NodeRef thread = turn ? write.parent(turn) : NodeRef{};
    const auto inFlight =
        thread ? promptInFlight_.find(thread.get()) : promptInFlight_.end();
    retainedForResult =
        inFlight != promptInFlight_.end() && inFlight->second == localPrompt;
    if (retainedForResult) {
      // The authoritative user item can arrive before the turn request's
      // JSON-RPC result. The widget has moved to that item, but this small
      // current node must keep the per-thread dispatch slot until the exact
      // result advances the queue.
      write.setField(localPrompt, "uiMaterialized", Value(true));
    } else {
      write.remove(localPrompt);
      if (turn && isLocalShell(turn) && write.children(turn).empty())
        write.remove(turn);
      if (thread && isLocalShell(thread) && write.children(thread).empty())
        write.remove(thread);
    }
    change = write.finish();
  }
  if (!retainedForResult)
    forgetPrompt(localPrompt);
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::acknowledgeUiDetached(NodeRef node) {
  GraphChange change;
  {
    const std::array<NodeRef, 1> detached{std::move(node)};
    auto write = graph_.write();
    write.releaseRetired(detached);
    change = write.finish();
  }
  return publish(std::move(change));
}

ChannelSendStatus WorkerLogic::sendWorkerStopped(std::string reason) {
  WorkerStopped stopped{std::move(reason)};
  return channels_.sendWorkerStopped(stopped);
}

ChannelSendStatus WorkerLogic::publish(GraphChange change) {
  return channels_.sendGraphChanged(std::move(change));
}

void WorkerLogic::resetProviderDerived(NodeGraph::WriteAccess &write,
                                       std::string_view reason) {
  const std::vector<NodeRef> nodes = write.orderedNodes();
  std::unordered_set<const Node *> retained;
  std::vector<NodeRef> prompts;
  std::vector<NodeRef> interactions;
  if (NodeRef runtime = write.find({NodeKind::Runtime, "runtime"}))
    retained.insert(runtime.get());
  if (NodeRef connection = write.find({NodeKind::Connection, "connection"}))
    retained.insert(connection.get());

  for (const NodeRef &node : nodes) {
    if (node && node->id().kind == NodeKind::Interaction) {
      interactions.emplace_back(node);
      retained.insert(node.get());
      continue;
    }
    if (!node || node->id().kind != NodeKind::Item ||
        !isLocalPrompt(*write.state(node)))
      continue;
    prompts.emplace_back(node);
    retained.insert(node.get());
  }

  NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
  retained.insert(runtime.get());
  write.setField(runtime, "initialized", Value(false));
  for (const NodeRef &interaction : interactions) {
    write.setStatus(interaction, NodeStatus::Failed);
    write.setField(interaction, "recoveryOnly", Value(true));
    write.setField(interaction, "error",
                   Value(reason.empty()
                             ? "The provider reset before the response was sent"
                             : std::string(reason)));
  }
  std::unordered_map<const Node *, NodeRef> recoveryByFormerThread;
  std::vector<NodeRef> recoveryThreads;
  for (const NodeRef &prompt : prompts) {
    NodeRef formerThread = containingThread(write, prompt);
    NodeRef recoveryThread;
    if (const auto found = recoveryByFormerThread.find(formerThread.get());
        found != recoveryByFormerThread.end()) {
      recoveryThread = found->second;
    } else {
      const std::string recoveryId = std::to_string(nextSubmissionId_++);
      NodeState threadState;
      threadState.status = NodeStatus::Failed;
      threadState.fields = {{"type", Value("localRecoveryThread")},
                            {"local", Value(true)},
                            {"recoveryOnly", Value(true)},
                            {"name", Value("Unsent prompt")}};
      recoveryThread = write.upsert(
          {NodeKind::Thread, "local-recovery-thread:" + recoveryId},
          std::move(threadState));
      recoveryByFormerThread.emplace(formerThread.get(), recoveryThread);
      recoveryThreads.emplace_back(recoveryThread);
    }
    const std::string turnId = std::to_string(nextSubmissionId_++);
    NodeState turnState;
    turnState.status = NodeStatus::Failed;
    turnState.fields = {{"type", Value("localRecoveryTurn")},
                        {"local", Value(true)}};
    NodeRef recoveryTurn =
        write.upsert({NodeKind::Turn, "local-recovery-turn:" + turnId},
                     std::move(turnState));
    write.setParent(recoveryThread, recoveryTurn);
    write.setParent(recoveryTurn, prompt);

    write.setStatus(prompt, NodeStatus::Failed);
    write.setField(prompt, "dispatchState", Value("uncertain"));
    write.setField(prompt, "error",
                   Value(reason.empty() ? "Provider state was reset"
                                        : std::string(reason)));
    write.setField(prompt, "requiresExplicitRecovery", Value(true));
    write.setField(prompt, "threadId", Value(recoveryThread->id().canonical));
    write.eraseField(prompt, "turnId");
    write.eraseField(prompt, "expectedTurnId");
    write.eraseField(prompt, "requestId");
    write.relate(runtime, RelationKind::PendingPrompt, prompt);
    write.relate(recoveryThread, RelationKind::PendingPrompt, prompt);
  }

  // Every provider-derived node, including the former canonical owners of a
  // recovery prompt, is retired. Reused provider ids therefore allocate fresh
  // NodeRefs and cannot inherit local recovery fields.
  for (const NodeRef &node : nodes) {
    if (!retained.contains(node.get()))
      write.remove(node);
  }
  write.replaceRelated(runtime, RelationKind::RootThread, recoveryThreads);
  promptQueues_.clear();
  promptInFlight_.clear();
  creatingThreads_.clear();
}

void WorkerLogic::forgetPrompt(const NodeRef &localPrompt) {
  if (!localPrompt)
    return;
  for (auto iterator = promptInFlight_.begin();
       iterator != promptInFlight_.end();) {
    if (iterator->second == localPrompt)
      iterator = promptInFlight_.erase(iterator);
    else
      ++iterator;
  }
  for (auto iterator = promptQueues_.begin();
       iterator != promptQueues_.end();) {
    std::erase_if(iterator->second, [&](const PendingPrompt &pending) {
      return pending.localPrompt == localPrompt;
    });
    if (iterator->second.empty())
      iterator = promptQueues_.erase(iterator);
    else
      ++iterator;
  }
}

} // namespace codexui::nodegraph
