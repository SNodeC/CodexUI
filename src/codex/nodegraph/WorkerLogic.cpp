// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/WorkerLogic.h"

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

bool isLocalPrompt(const NodeState &state) {
  return stringField(state, "type") == "localPrompt";
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

} // namespace

WorkerLogic::WorkerLogic(NodeGraph &graph, ThreadChannels &channels) noexcept
    : graph_(graph), channels_(channels), updater_(graph) {}

ChannelSendStatus WorkerLogic::apply(DecodedMessage message) {
  return applyDetailed(std::move(message)).status;
}

WorkerApplyResult WorkerLogic::applyDetailed(DecodedMessage message) {
  ApplyResult result = updater_.apply(std::move(message));
  NodeRef primary = std::move(result.primary);
  for (const NodeRef &removed : result.change.removed) {
    if (!removed)
      continue;
    if (removed->id().kind == NodeKind::Item &&
        removed->id().canonical.starts_with("local-prompt:"))
      forgetPrompt(removed);
    if (removed->id().kind == NodeKind::Thread) {
      promptQueues_.erase(removed.get());
      promptInFlight_.erase(removed.get());
    }
  }
  return {publish(std::move(result.change)), std::move(primary)};
}

WorkerGenerations WorkerLogic::generations() const noexcept {
  return {connection_.connectionGeneration, connection_.providerGeneration};
}

ChannelSendStatus WorkerLogic::transportEvent(std::string state,
                                              std::string detail) {
  connection_.transportState = std::move(state);
  bool resetProvider = false;
  std::string resetReason;
  if (connection_.transportState == "connected") {
    ++connection_.connectionGeneration;
    connection_.transportDetail.clear();
    clearBridgeState(true);
    resetProvider = true;
    resetReason = "A new app-server connection replaced provider state";
  } else {
    connection_.transportDetail = std::move(detail);
    if (connection_.transportState == "connecting" ||
        connection_.transportState == "retrying" ||
        connection_.transportState == "disconnected" ||
        connection_.transportState == "failure") {
      clearBridgeState(false);
      if (connection_.transportState != "connecting") {
        resetProvider = true;
        resetReason = connection_.transportDetail.empty()
                          ? "The app-server connection was reset"
                          : connection_.transportDetail;
      }
    }
  }
  return publishConnection(resetProvider, std::move(resetReason));
}

ChannelSendStatus WorkerLogic::bridgeState(
    std::string connectionId, std::string role,
    std::string controllerConnectionId, std::uint64_t providerGeneration,
    std::optional<std::string> providerState, std::string detail) {
  if (providerState && providerGeneration < connection_.providerGeneration)
    return ChannelSendStatus::Accepted;

  const bool resetProvider =
      providerState && providerGeneration > connection_.providerGeneration;
  std::string resetReason;
  if (resetProvider)
    resetReason = detail.empty() ? "The provider generation changed" : detail;

  connection_.connectionId = std::move(connectionId);
  connection_.role = std::move(role);
  connection_.controllerConnectionId = std::move(controllerConnectionId);
  if (providerState && providerGeneration >= connection_.providerGeneration) {
    connection_.providerGeneration = providerGeneration;
    connection_.providerState = std::move(*providerState);
    connection_.providerDetail = std::move(detail);
  }
  return publishConnection(resetProvider, std::move(resetReason));
}

ChannelSendStatus WorkerLogic::connectionSettings(Value::Object settings) {
  connection_.settings = std::move(settings);
  return publishConnection(false, {});
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

PromptTransition WorkerLogic::admitPrompt(NodeAction action) {
  if (action.kind != NodeActionKind::SubmitPrompt || !action.target) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "The prompt has no current destination thread",
                    {}};
    return {channels_.sendUiEffect(effect), std::nullopt};
  }
  PendingPrompt pending;
  pending.localPrompt = {};
  pending.thread = std::move(action.target);
  pending.promptText = std::move(action.promptText);
  pending.attachments = std::move(action.attachments);
  pending.options = std::move(action.payload);
  return admit(std::move(pending));
}

PromptTransition WorkerLogic::admitFirstPrompt(RuntimeAction action) {
  if (action.kind != RuntimeActionKind::CreateThread) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "The new-thread prompt could not be admitted",
                    {}};
    return {channels_.sendUiEffect(effect), std::nullopt};
  }
  PendingPrompt pending;
  pending.createsThread = true;
  pending.promptText = std::move(action.promptText);
  pending.attachments = std::move(action.attachments);
  std::optional<Value::Object> options =
      takeObject(action.payload, "threadStart");
  if (std::optional<Value::Object> turnOptions =
          takeObject(action.payload, "turnStart"))
    pending.turnOptions = std::move(*turnOptions);
  pending.requestedName = takeString(action.payload, "requestedName");
  pending.options = options ? std::move(*options) : std::move(action.payload);
  return admit(std::move(pending));
}

PromptTransition WorkerLogic::admit(PendingPrompt pending) {
  if (pending.promptText.empty() && pending.attachments.empty()) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "Enter a message or attach a file before sending",
                    {}};
    return {channels_.sendUiEffect(effect), std::nullopt};
  }

  const std::uint64_t submission = nextSubmissionId_++;
  const std::string suffix = std::to_string(submission);
  pending.clientUserMessageId = "codexui-" + suffix;

  GraphChange change;
  NodeRef selectedDraft;
  std::optional<PromptCommand> command;
  bool invalidTarget = false;
  {
    auto write = graph_.write();
    if (pending.createsThread) {
      NodeState threadState;
      threadState.status = NodeStatus::Pending;
      threadState.fields = {{"type", Value("localThread")},
                            {"local", Value(true)}};
      if (!pending.requestedName.empty())
        threadState.fields.emplace("name", Value(pending.requestedName));
      pending.thread = write.upsert(
          {NodeKind::Thread, "local-thread:" + suffix}, std::move(threadState));
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
          pending.thread->id().kind != NodeKind::Thread)
        invalidTarget = true;
    }

    if (invalidTarget || !pending.thread) {
      // An invalid target does not consume the authored data into a graph
      // node. Qt only clears its draft after a successful admission.
      change = write.finish();
    } else {
      NodeRef turn = activeTurn(write, pending.thread);
      const bool startsTurn = !turn;
      if (!turn) {
        NodeState turnState;
        turnState.status = NodeStatus::Pending;
        turnState.fields = {{"type", Value("localTurn")},
                            {"local", Value(true)}};
        turn = write.upsert({NodeKind::Turn, "local-turn:" + suffix},
                            std::move(turnState));
        write.setParent(pending.thread, turn);
      }

      NodeState promptState;
      promptState.status = NodeStatus::Pending;
      promptState.fields = {
          {"type", Value("localPrompt")},
          {"local", Value(true)},
          {"submissionId", Value(submission)},
          {"clientUserMessageId", Value(pending.clientUserMessageId)},
          {"text", Value(pending.promptText)},
          {"attachments", attachmentSummaries(pending.attachments)},
          {"dispatchState", Value("queued")},
          {"startsTurn", Value(startsTurn)},
          {"threadId", Value(pending.thread->id().canonical)}};
      if (!startsTurn)
        promptState.fields.emplace("expectedTurnId",
                                   Value(turn->id().canonical));
      pending.localPrompt = write.upsert(
          {NodeKind::Item, "local-prompt:" + suffix}, std::move(promptState));
      write.setParent(turn, pending.localPrompt);
      NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
      write.relate(runtime, RelationKind::PendingPrompt, pending.localPrompt);
      write.relate(pending.thread, RelationKind::PendingPrompt,
                   pending.localPrompt);
      const NodeRef ownerThread = pending.thread;
      promptQueues_[pending.thread.get()].emplace_back(std::move(pending));
      command = takeNextPrompt(write, ownerThread);
      change = write.finish();
    }
  }

  if (selectedDraft) {
    UiEffect select{UiEffectKind::SelectThread, selectedDraft, {}, {}};
    static_cast<void>(channels_.sendUiEffect(select));
  } else if (invalidTarget) {
    UiEffect effect{UiEffectKind::ShowNotice,
                    std::nullopt,
                    "The destination thread is no longer available",
                    {}};
    return {channels_.sendUiEffect(effect), std::nullopt};
  }
  return {publish(std::move(change)), std::move(command)};
}

NodeRef WorkerLogic::activeTurn(NodeGraph::WriteAccess &write,
                                const NodeRef &thread) const {
  const std::vector<NodeRef> turns = write.children(thread);
  for (auto iterator = turns.rbegin(); iterator != turns.rend(); ++iterator) {
    const NodeRef &turn = *iterator;
    if (!turn || turn->id().kind != NodeKind::Turn)
      continue;
    const std::shared_ptr<const NodeState> state = write.state(turn);
    const std::string protocolStatus = stringField(*state, "status");
    if (state->status == NodeStatus::Running || protocolStatus == "active" ||
        protocolStatus == "running" || protocolStatus == "inProgress")
      return turn;
  }
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

    if (!pending.createsThread) {
      if (NodeRef running = activeTurn(write, thread)) {
        command.kind = PromptCommandKind::SteerTurn;
        command.expectedTurnId = running->id().canonical;
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
  if (command.kind != PromptCommandKind::CreateThread || threadId.empty() ||
      !command.thread || !command.localPrompt)
    return ChannelSendStatus::Accepted;

  NodeRef authoritative;
  GraphChange change;
  {
    auto write = graph_.write();
    NodeRef draft = write.find(command.thread->id());
    if (draft != command.thread ||
        write.find(command.localPrompt->id()) != command.localPrompt)
      return ChannelSendStatus::Accepted;
    authoritative = write.find({NodeKind::Thread, threadId});
    if (!authoritative)
      authoritative = write.upsert({NodeKind::Thread, std::move(threadId)},
                                   std::move(providerState));

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
      std::deque<PendingPrompt> &destination =
          promptQueues_[authoritative.get()];
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
    write.remove(draft);
    change = write.finish();
  }

  command.thread = authoritative;
  command.kind = PromptCommandKind::StartTurn;
  command.expectedTurnId.clear();
  command.options = std::move(command.turnOptions);
  UiEffect select{UiEffectKind::SelectThread, authoritative, {}, {}};
  static_cast<void>(channels_.sendUiEffect(select));
  return publish(std::move(change));
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
    if (!localPrompt || write.find(localPrompt->id()) != localPrompt)
      return {};
    NodeRef thread = containingThread(write, localPrompt);
    if (!thread)
      return {};
    if (auto inFlight = promptInFlight_.find(thread.get());
        inFlight != promptInFlight_.end() && inFlight->second == localPrompt)
      promptInFlight_.erase(inFlight);

    if (accepted) {
      write.setField(localPrompt, "dispatchState",
                     Value("awaitingMaterialization"));
      write.eraseField(localPrompt, "error");
      write.eraseField(localPrompt, "requiresExplicitRecovery");
      write.setStatus(localPrompt, NodeStatus::Running);
      if (turnId && !turnId->empty()) {
        NodeRef turn = write.upsert({NodeKind::Turn, *turnId});
        if (!write.parent(turn))
          write.setParent(thread, turn);
        NodeRef previous = write.parent(localPrompt);
        write.setParent(turn, localPrompt);
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
        if (auto queued = promptQueues_.find(thread.get());
            queued != promptQueues_.end()) {
          for (const PendingPrompt &pending : queued->second) {
            if (!pending.localPrompt ||
                write.find(pending.localPrompt->id()) != pending.localPrompt)
              continue;
            write.setField(pending.localPrompt, "dispatchState",
                           Value("failed"));
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
      next = takeNextPrompt(write, thread);
    change = write.finish();
  }
  return {publish(std::move(change)), std::move(next)};
}

PromptTransition WorkerLogic::failPrompt(const NodeRef &localPrompt,
                                         std::string error) {
  return completePrompt(localPrompt, false, std::move(error));
}

ChannelSendStatus WorkerLogic::promptMaterialized(const NodeRef &localPrompt) {
  GraphChange change;
  {
    auto write = graph_.write();
    if (!localPrompt || write.find(localPrompt->id()) != localPrompt)
      return ChannelSendStatus::Accepted;
    NodeRef turn = write.parent(localPrompt);
    NodeRef thread = turn ? write.parent(turn) : NodeRef{};
    write.remove(localPrompt);
    if (turn && isLocalShell(turn) && write.children(turn).empty())
      write.remove(turn);
    if (thread && isLocalShell(thread) && write.children(thread).empty())
      write.remove(thread);
    change = write.finish();
  }
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

ChannelSendStatus WorkerLogic::publishConnection(bool resetProvider,
                                                 std::string resetReason) {
  NodeState state = connectionNodeState();
  GraphChange change;
  {
    auto write = graph_.write();
    if (resetProvider)
      resetProviderDerived(write, resetReason);
    NodeRef connection =
        write.upsert({NodeKind::Connection, "connection"}, state);
    write.replaceState(connection, std::move(state));
    change = write.finish();
  }
  return publish(std::move(change));
}

void WorkerLogic::resetProviderDerived(NodeGraph::WriteAccess &write,
                                       std::string_view reason) {
  const std::vector<NodeRef> nodes = write.orderedNodes();
  std::unordered_set<const Node *> retained;
  std::vector<NodeRef> prompts;
  if (NodeRef runtime = write.find({NodeKind::Runtime, "runtime"}))
    retained.insert(runtime.get());
  if (NodeRef connection = write.find({NodeKind::Connection, "connection"}))
    retained.insert(connection.get());

  for (const NodeRef &node : nodes) {
    if (!node || node->id().kind != NodeKind::Item ||
        !isLocalPrompt(*write.state(node)))
      continue;
    prompts.emplace_back(node);
    retained.insert(node.get());
  }

  NodeRef runtime = write.upsert({NodeKind::Runtime, "runtime"});
  retained.insert(runtime.get());
  write.setField(runtime, "initialized", Value(false));
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

NodeState WorkerLogic::connectionNodeState() const {
  NodeStatus status = NodeStatus::Unknown;
  if (connection_.transportState == "connected")
    status = NodeStatus::Connected;
  else if (connection_.transportState == "connecting" ||
           connection_.transportState == "retrying")
    status = NodeStatus::Pending;
  else if (connection_.transportState == "disconnected" ||
           connection_.transportState == "failure")
    status = NodeStatus::Disconnected;

  return NodeState{
      status,
      {{"transportState", connection_.transportState},
       {"transportDetail", connection_.transportDetail},
       {"connectionGeneration", connection_.connectionGeneration},
       {"connectionId", connection_.connectionId},
       {"role", connection_.role},
       {"controllerConnectionId", connection_.controllerConnectionId},
       {"providerGeneration", connection_.providerGeneration},
       {"providerState", connection_.providerState},
       {"providerDetail", connection_.providerDetail},
       {"settings", connection_.settings}}};
}

void WorkerLogic::clearBridgeState(bool clearProviderGeneration) {
  connection_.connectionId.clear();
  connection_.role.clear();
  connection_.controllerConnectionId.clear();
  if (clearProviderGeneration)
    connection_.providerGeneration = 0;
  connection_.providerState.clear();
  connection_.providerDetail.clear();
}

} // namespace codexui::nodegraph
