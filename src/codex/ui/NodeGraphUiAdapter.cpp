// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/NodeGraphUiAdapter.h"

#include "codex/NodeGraphJson.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/UiStatus.h"
#include "codex/nodegraph/ProtocolUpdater.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codexui::codex::ui {
namespace {
using namespace middle;
using nodegraph::boolFromValue;
using nodegraph::exactStringFromValue;
using nodegraph::scalarTextFromValue;
using nodegraph::signedIntegerFromValue;
using nodegraph::valueMember;

UiStatus uiStatus(nodegraph::NodeStatusView status) {
  return statusFromNode(status.semantic, status.unknownText);
}

UiStatus uiStatusFromValue(const nodegraph::Value *value) {
  return uiStatus(nodegraph::nodeStatusView(value));
}

bool graphProviderHasMoreHistory(const nodegraph::NodeState &state) {
  return boolFromValue(valueMember(state, "historyHasMore"));
}

bool isConversationSnapshotMethod(std::string_view method) noexcept {
  return method == "thread/read" || method == "thread/turns/list" ||
         method == "thread/items/list";
}

bool hasPendingHistoryRequest(const nodegraph::NodeGraph::ReadAccess &read,
                              const nodegraph::NodeRef &thread) {
  for (const nodegraph::NodeRef &operation :
       read.related(thread, nodegraph::RelationKind::PendingOperation)) {
    const auto state = read.state(operation);
    if (state && state->status == nodegraph::NodeStatus::Pending &&
        isConversationSnapshotMethod(
            scalarTextFromValue(valueMember(*state, "method"))))
      return true;
  }
  return false;
}

bool fieldChanged(const nodegraph::NodeGraph::ReadAccess &read,
                  const nodegraph::NodeRef &node, std::string_view field,
                  std::uint64_t revision) {
  return node && read.contains(node) &&
         read.fieldChangedRevision(node, field) == revision;
}

UiStatus agentUiStatus(const nodegraph::NodeState &state);

struct PendingPromptPresentation {
  bool awaitingAcknowledgement = false;
  std::optional<std::int64_t> admittedAtMs;
  nodegraph::NodeRef provisionalTurn;
};

PendingPromptPresentation
pendingPromptPresentation(nodegraph::NodeGraph::ReadAccess &read,
                          const nodegraph::NodeRef &thread) {
  PendingPromptPresentation result;
  for (const nodegraph::NodeRef &prompt :
       read.related(thread, nodegraph::RelationKind::PendingPrompt)) {
    if (prompt->id().kind != nodegraph::NodeKind::Item)
      continue;
    const auto state = read.state(prompt);
    if (!state ||
        scalarTextFromValue(valueMember(*state, "type")) != "localPrompt")
      continue;
    const std::string dispatch =
        scalarTextFromValue(valueMember(*state, "dispatchState"));
    const bool awaiting = dispatch == "queued" || dispatch == "dispatching" ||
                          dispatch == "inFlight";
    if (!awaiting)
      continue;
    result.awaitingAcknowledgement = true;
    const auto admittedAt =
        signedIntegerFromValue(valueMember(*state, "admittedAtMs"));
    if (admittedAt &&
        (!result.admittedAtMs || *admittedAt < *result.admittedAtMs))
      result.admittedAtMs = admittedAt;
    if (!result.provisionalTurn &&
        boolFromValue(valueMember(*state, "startsTurn")) &&
        (dispatch == "dispatching" || dispatch == "inFlight") &&
        (state->status == nodegraph::NodeStatus::Pending ||
         state->status == nodegraph::NodeStatus::Running)) {
      const nodegraph::NodeRef turn = read.parent(prompt);
      if (turn && turn->id().kind == nodegraph::NodeKind::Turn &&
          read.parent(turn) == thread)
        result.provisionalTurn = turn;
    }
  }
  return result;
}

std::vector<std::string> graphStrings(const nodegraph::Value *value) {
  std::vector<std::string> result;
  const auto *array = value ? value->asArray() : nullptr;
  if (!array)
    return result;
  result.reserve(array->size());
  for (const nodegraph::Value &entry : *array)
    if (const auto *text = entry.asString(); text && !text->empty())
      result.push_back(*text);
  return result;
}

std::uint64_t graphOmittedTextBytes(const nodegraph::NodeState &state,
                                    std::string_view field) {
  const nodegraph::Value *retention = valueMember(state, "textRetention");
  const auto *retentionObject = retention ? retention->asObject() : nullptr;
  const nodegraph::Value *entry =
      retentionObject ? valueMember(*retentionObject, field) : nullptr;
  const auto *entryObject = entry ? entry->asObject() : nullptr;
  const nodegraph::Value *discarded =
      entryObject ? valueMember(*entryObject, "discardedBytes") : nullptr;
  return nodegraph::unsignedIntegerFromValue(discarded).value_or(0);
}

std::string withTruncationNotice(std::string value, std::uint64_t omitted,
                                 std::string_view subject, bool markdown) {
  if (omitted == 0)
    return value;
  const std::string notice = "Earlier " + std::string(subject) +
                             " was truncated (" + std::to_string(omitted) +
                             " bytes omitted).";
  return markdown ? "> " + notice + "\n\n" + value
                  : '[' + notice + "]\n" + value;
}

std::string graphMessageText(const nodegraph::NodeState &state) {
  std::string result;
  if (const auto *content = valueMember(state, "content");
      content && content->asArray()) {
    for (const nodegraph::Value &entry : *content->asArray()) {
      const auto *object = entry.asObject();
      const std::string text =
          object ? scalarTextFromValue(valueMember(*object, "text"))
                 : std::string{};
      if (text.empty())
        continue;
      if (!result.empty())
        result.push_back('\n');
      result += text;
    }
  }
  return result.empty() ? scalarTextFromValue(valueMember(state, "text"))
                        : result;
}

std::vector<std::string> graphImagePaths(const nodegraph::NodeState &state) {
  std::vector<std::string> result;
  const auto *content = valueMember(state, "content");
  const auto *array = content ? content->asArray() : nullptr;
  if (!array)
    return result;
  for (const nodegraph::Value &entry : *array) {
    const auto *object = entry.asObject();
    if (!object ||
        scalarTextFromValue(valueMember(*object, "type")) != "localImage")
      continue;
    if (std::string path = scalarTextFromValue(valueMember(*object, "path"));
        !path.empty())
      result.push_back(std::move(path));
  }
  return result;
}

std::vector<std::string>
graphLocalPromptImagePaths(const nodegraph::NodeState &state) {
  std::vector<std::string> result;
  const nodegraph::Value *attachments = valueMember(state, "attachments");
  const auto *array = attachments ? attachments->asArray() : nullptr;
  if (!array)
    return result;
  for (const nodegraph::Value &entry : *array) {
    const auto *object = entry.asObject();
    if (!object || !scalarTextFromValue(valueMember(*object, "mimeType"))
                        .starts_with("image/"))
      continue;
    if (std::string path = scalarTextFromValue(valueMember(*object, "path"));
        !path.empty())
      result.emplace_back(std::move(path));
  }
  return result;
}

std::string graphJoinedText(const nodegraph::Value *value) {
  const auto *array = value ? value->asArray() : nullptr;
  if (!array)
    return scalarTextFromValue(value);
  std::string result;
  for (const nodegraph::Value &entry : *array) {
    std::string text = scalarTextFromValue(&entry);
    if (text.empty())
      if (const auto *object = entry.asObject())
        text = scalarTextFromValue(valueMember(*object, "text"));
    if (text.empty())
      continue;
    if (!result.empty())
      result += ", ";
    result += text;
  }
  return result;
}

std::pair<int, int> graphDiffCounts(std::string_view diff) {
  int additions = 0;
  int deletions = 0;
  for (std::size_t offset = 0; offset <= diff.size();) {
    const std::size_t end = diff.find('\n', offset);
    const std::string_view line =
        diff.substr(offset, end == std::string_view::npos ? diff.size() - offset
                                                          : end - offset);
    if (!line.starts_with("+++ ") && !line.starts_with("--- ")) {
      if (line.starts_with('+'))
        ++additions;
      else if (line.starts_with('-'))
        ++deletions;
    }
    if (end == std::string_view::npos)
      break;
    offset = end + 1;
  }
  return {additions, deletions};
}

constexpr std::size_t MaximumGraphActivityDetailBytes = 4000;

class GraphDetailBuilder final {
public:
  void append(std::string_view text) {
    if (text.empty() || truncated_)
      return;
    std::size_t count =
        std::min(text.size(), MaximumGraphActivityDetailBytes - value_.size());
    truncated_ = count < text.size();
    while (count > 0 && count < text.size() &&
           (static_cast<unsigned char>(text[count]) & 0xc0U) == 0x80U)
      --count;
    value_.append(text.substr(0, count));
  }

  void indent(int depth) {
    for (int index = 0; index < depth; ++index)
      append("  ");
  }

  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  [[nodiscard]] std::string finish() && {
    if (truncated_)
      value_ += "\n\n[Activity details truncated]";
    return std::move(value_);
  }

private:
  std::string value_;
  bool truncated_ = false;
};

void appendGraphDetail(GraphDetailBuilder &builder,
                       const nodegraph::Value &value, int depth) {
  if (builder.truncated())
    return;
  if (value.isNull()) {
    builder.append("none");
    return;
  }
  if (const auto *boolean = value.asBool()) {
    builder.append(*boolean ? "true" : "false");
    return;
  }
  if (const auto *number = value.asInt64()) {
    builder.append(std::to_string(*number));
    return;
  }
  if (const auto *number = value.asUInt64()) {
    builder.append(std::to_string(*number));
    return;
  }
  if (const auto *number = value.asDouble()) {
    builder.append(std::to_string(*number));
    return;
  }
  if (const auto *text = value.asString()) {
    builder.append(*text);
    return;
  }
  if (depth >= 4) {
    builder.append("nested detail omitted");
    return;
  }
  if (const auto *array = value.asArray()) {
    if (array->empty()) {
      builder.append("none");
      return;
    }
    for (const nodegraph::Value &entry : *array) {
      builder.append("\n");
      builder.indent(depth + 1);
      builder.append("- ");
      appendGraphDetail(builder, entry, depth + 1);
      if (builder.truncated())
        return;
    }
    return;
  }
  const auto *object = value.asObject();
  if (!object || object->empty()) {
    builder.append("none");
    return;
  }
  for (const auto &[key, entry] : *object) {
    builder.append("\n");
    builder.indent(depth + 1);
    builder.append(key);
    builder.append(": ");
    appendGraphDetail(builder, entry, depth + 1);
    if (builder.truncated())
      return;
  }
}

std::string graphDisplayDetail(const nodegraph::NodeState &state) {
  GraphDetailBuilder builder;
  for (const auto &[key, value] : state.fields) {
    builder.append(key);
    builder.append(": ");
    appendGraphDetail(builder, value, 0);
    if (builder.truncated())
      break;
    builder.append("\n");
  }
  return std::move(builder).finish();
}

CardKind graphCardKind(const nodegraph::NodeState &state) {
  const std::string type = scalarTextFromValue(valueMember(state, "type"));
  if (type == "localPrompt")
    return CardKind::LocalPrompt;
  if (type == "userMessage")
    return CardKind::UserMessage;
  if (type == "agentMessage")
    return CardKind::AgentMessage;
  if (type == "commandExecution")
    return CardKind::CommandExecution;
  if (type == "collabAgentToolCall" || type == "subAgentActivity")
    return CardKind::AgentActivity;
  if (type == "reasoning")
    return CardKind::Reasoning;
  if (type == "fileChange")
    return CardKind::FileChanges;
  if (type == "imageGeneration" || type == "imageView")
    return CardKind::ImageGeneration;
  if (type == "plan" && !graphMessageText(state).empty())
    return CardKind::Plan;
  return CardKind::GenericActivity;
}

VisibleCardData graphCardData(const nodegraph::NodeRef &item,
                              std::string threadId, std::string turnId,
                              const nodegraph::NodeState &state,
                              std::string_view threadCwd = {}) {
  const std::string itemId =
      item ? nodegraph::protocolCanonicalId(state, item) : std::string{};
  const std::string type = scalarTextFromValue(valueMember(state, "type"));
  const CardKind kind = graphCardKind(state);
  VisibleCardData result{AuthoritativeItemKey{threadId, turnId, itemId},
                         kind,
                         std::move(threadId),
                         std::move(turnId),
                         itemId,
                         GenericActivityData{},
                         {},
                         {}};

  result.status = statusFromState(state);
  if (result.kind == CardKind::AgentActivity)
    result.status = agentUiStatus(state);
  else if (result.kind == CardKind::ImageGeneration && type == "imageView")
    result.status = nodegraph::NodeStatus::Completed;
  else if (result.kind == CardKind::UserMessage ||
           result.kind == CardKind::AgentMessage ||
           result.kind == CardKind::LocalPrompt)
    result.status = {};
  switch (result.kind) {
  case CardKind::UserMessage: {
    result.payload =
        UserMessageData{graphMessageText(state), graphImagePaths(state)};
    break;
  }
  case CardKind::AgentMessage:
    result.payload = AgentMessageData{
        withTruncationNotice(graphMessageText(state),
                             graphOmittedTextBytes(state, "text"),
                             "Codex response", true),
        scalarTextFromValue(valueMember(state, "phase")) == "final_answer"};
    break;
  case CardKind::CommandExecution: {
    std::string outputField = "aggregatedOutput";
    std::string output =
        scalarTextFromValue(valueMember(state, "aggregatedOutput"));
    if (output.empty()) {
      outputField = "output";
      output = scalarTextFromValue(valueMember(state, "output"));
    }
    output = withTruncationNotice(std::move(output),
                                  graphOmittedTextBytes(state, outputField),
                                  "command output", false);
    if (!terminalOutputHasVisibleText(output))
      output.clear();
    const auto exit = signedIntegerFromValue(valueMember(state, "exitCode"));
    auto duration = signedIntegerFromValue(valueMember(state, "durationMs"));
    if (!duration)
      duration = signedIntegerFromValue(valueMember(state, "duration_ms"));
    result.payload = CommandExecutionData{
        scalarTextFromValue(valueMember(state, "command")), std::move(output),
        scalarTextFromValue(valueMember(state, "cwd")),
        exit ? std::optional<int>(static_cast<int>(*exit)) : std::nullopt,
        duration};
    break;
  }
  case CardKind::AgentActivity:
    result.payload = AgentActivityData{
        scalarTextFromValue(valueMember(state, "tool")),
        scalarTextFromValue(valueMember(state, "kind")),
        scalarTextFromValue(valueMember(state, "prompt")),
        scalarTextFromValue(valueMember(state, "resultText")),
        graphStrings(valueMember(state, "receiverThreadIds")),
        scalarTextFromValue(valueMember(state, "model")),
        scalarTextFromValue(valueMember(state, "reasoningEffort")),
        scalarTextFromValue(valueMember(state, "agentThreadId")),
        scalarTextFromValue(valueMember(state, "agentPath")),
        scalarTextFromValue(valueMember(state, "senderThreadId"))};
    break;
  case CardKind::Reasoning:
    result.payload = ReasoningData{withTruncationNotice(
        graphJoinedText(valueMember(state, "summary")),
        graphOmittedTextBytes(state, "summary"), "reasoning", true)};
    break;
  case CardKind::FileChanges: {
    std::string cwd = scalarTextFromValue(valueMember(state, "cwd"));
    if (cwd.empty())
      cwd = threadCwd;
    FileChangesData projected{{}, std::move(cwd)};
    const auto *changes = valueMember(state, "changes");
    const auto *array = changes ? changes->asArray() : nullptr;
    if (array) {
      projected.changes.reserve(array->size());
      for (const nodegraph::Value &value : *array) {
        const auto *change = value.asObject();
        if (!change)
          continue;
        FileChangeData entry{scalarTextFromValue(valueMember(*change, "path")),
                             scalarTextFromValue(valueMember(*change, "kind")),
                             std::nullopt, std::nullopt};
        const auto additions =
            signedIntegerFromValue(valueMember(*change, "additions"));
        const auto deletions =
            signedIntegerFromValue(valueMember(*change, "deletions"));
        if (additions && deletions && *additions >= 0 && *deletions >= 0) {
          entry.additions = static_cast<int>(std::min<std::int64_t>(
              *additions, std::numeric_limits<int>::max()));
          entry.deletions = static_cast<int>(std::min<std::int64_t>(
              *deletions, std::numeric_limits<int>::max()));
        } else if (const nodegraph::Value *diffValue =
                       valueMember(*change, "diff");
                   diffValue && diffValue->asString() &&
                   !diffValue->asString()->empty()) {
          const std::string &diff = *diffValue->asString();
          const auto [additions, deletions] = graphDiffCounts(diff);
          entry.additions = additions;
          entry.deletions = deletions;
        }
        projected.changes.push_back(std::move(entry));
      }
    }
    result.payload = std::move(projected);
    break;
  }
  case CardKind::ImageGeneration: {
    std::string path = scalarTextFromValue(valueMember(state, "path"));
    if (path.empty())
      path = scalarTextFromValue(valueMember(state, "savedPath"));
    if (path.empty())
      path = scalarTextFromValue(valueMember(state, "saved_path"));
    std::string prompt =
        scalarTextFromValue(valueMember(state, "revisedPrompt"));
    if (prompt.empty())
      prompt = scalarTextFromValue(valueMember(state, "revised_prompt"));
    result.payload = ImageGenerationData{std::move(path), std::move(prompt)};
    break;
  }
  case CardKind::Plan:
    result.payload =
        PlanData{{},
                 {},
                 withTruncationNotice(graphMessageText(state),
                                      graphOmittedTextBytes(state, "text"),
                                      "plan text", true)};
    break;
  case CardKind::GenericActivity:
    result.payload = GenericActivityData{type, graphDisplayDetail(state)};
    break;
  case CardKind::LocalPrompt: {
    const std::string dispatch =
        scalarTextFromValue(valueMember(state, "dispatchState"));
    PromptState promptState = PromptState::Queued;
    if (dispatch == "dispatching" || dispatch == "inFlight")
      promptState = PromptState::InFlight;
    else if (dispatch == "awaitingMaterialization")
      promptState = PromptState::Accepted;
    else if (dispatch == "failed" || dispatch == "uncertain")
      promptState = PromptState::Failed;
    const std::int64_t rawId =
        signedIntegerFromValue(valueMember(state, "submissionId")).value_or(0);
    const std::uint64_t submissionId =
        rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
    result.key = LocalPromptKey{submissionId};
    result.itemId.clear();
    result.payload = LocalPromptData{
        submissionId,
        scalarTextFromValue(valueMember(state, "text")),
        promptState,
        valueMember(state, "showPendingAnimation")
            ? boolFromValue(valueMember(state, "showPendingAnimation"))
            : false,
        scalarTextFromValue(valueMember(state, "error")),
        graphLocalPromptImagePaths(state),
        signedIntegerFromValue(valueMember(state, "admittedAtMs")),
        boolFromValue(valueMember(state, "requiresExplicitRecovery"))};
    break;
  }
  }
  result.target = item;
  return result;
}

struct MaterializedPrompt {
  nodegraph::NodeRef prompt;
  std::optional<std::uint64_t> submissionId;
};

std::optional<MaterializedPrompt>
materializedPrompt(nodegraph::NodeGraph::ReadAccess &read,
                   const nodegraph::NodeRef &item) {
  if (!item)
    return std::nullopt;
  std::optional<MaterializedPrompt> fallback;
  for (const nodegraph::NodeRef &prompt :
       read.related(item, nodegraph::RelationKind::PromptMaterialization)) {
    if (prompt->id().kind != nodegraph::NodeKind::Item)
      continue;
    const auto state = read.state(prompt);
    if (!state ||
        scalarTextFromValue(valueMember(*state, "type")) != "localPrompt")
      continue;
    const auto rawSubmission =
        signedIntegerFromValue(valueMember(*state, "submissionId"));
    MaterializedPrompt candidate{
        prompt, rawSubmission && *rawSubmission >= 0
                    ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(
                          *rawSubmission)}
                    : std::nullopt};
    if (candidate.submissionId)
      return candidate;
    if (!fallback)
      fallback = std::move(candidate);
  }
  return fallback;
}

std::optional<CardKey>
conversationCardKey(nodegraph::NodeGraph::ReadAccess &read,
                    const nodegraph::NodeRef &item, std::string_view threadId,
                    std::string_view turnId, bool hiddenPrompt = false) {
  if (!read.live(item) ||
      item->id().kind != nodegraph::NodeKind::Item)
    return std::nullopt;
  const auto state = read.state(item);
  if (!state)
    return std::nullopt;
  const std::string type = scalarTextFromValue(valueMember(*state, "type"));
  if (type == "localPrompt") {
    if (hiddenPrompt)
      return std::nullopt;
    const std::int64_t rawId =
        signedIntegerFromValue(valueMember(*state, "submissionId")).value_or(0);
    return LocalPromptKey{rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId)};
  }
  if (graphCardKind(*state) == CardKind::UserMessage) {
    if (const auto materialized = materializedPrompt(read, item);
        materialized && materialized->submissionId)
      return LocalPromptKey{*materialized->submissionId};
    const auto submission =
        signedIntegerFromValue(valueMember(*state, "localSubmissionId"));
    if (submission && *submission >= 0)
      return LocalPromptKey{static_cast<std::uint64_t>(*submission)};
  }
  return AuthoritativeItemKey{std::string(threadId), std::string(turnId),
                              nodegraph::protocolCanonicalId(*state, item)};
}

bool isHiddenMaterializedPrompt(const nodegraph::NodeGraph::ReadAccess &read,
                                const nodegraph::NodeRef &prompt) {
  if (!read.live(prompt))
    return false;
  const auto state = read.state(prompt);
  return state &&
         scalarTextFromValue(valueMember(*state, "type")) == "localPrompt" &&
         read.hasIncomingRelation(
             prompt, nodegraph::RelationKind::PromptMaterialization);
}

nodegraph::NodeRef conversationTurnRoot(
    const nodegraph::NodeGraph::ReadAccess &read,
                                        const nodegraph::NodeRef &turn) {
  const auto roots = read.related(turn, nodegraph::RelationKind::TurnRootItem);
  return roots.empty() ? nodegraph::NodeRef{} : roots.front();
}

nodegraph::NodeRef
conversationActiveTurn(nodegraph::NodeGraph::ReadAccess &read,
                       const nodegraph::NodeRef &thread) {
  for (const nodegraph::NodeRef &turn :
       read.related(thread, nodegraph::RelationKind::ActiveTurn))
    if (turn->id().kind == nodegraph::NodeKind::Turn &&
        read.parent(turn) == thread)
      return turn;
  return pendingPromptPresentation(read, thread).provisionalTurn;
}

VisibleCardData conversationCardData(nodegraph::NodeGraph::ReadAccess &read,
                                     const nodegraph::NodeRef &item,
                                     std::string threadId, std::string turnId,
                                     const nodegraph::NodeState &state,
                                     std::string_view threadCwd = {}) {
  const std::optional<CardKey> key =
      conversationCardKey(read, item, threadId, turnId);
  VisibleCardData result = graphCardData(item, std::move(threadId),
                                         std::move(turnId), state, threadCwd);
  if (key)
    result.key = *key;
  return result;
}

void updateAgentStatus(UiStatus &current, UiStatus candidate) {
  if (candidate.empty() ||
      (isTerminalTurnStatus(current) && isActiveStatus(candidate)))
    return;
  current = std::move(candidate);
}

UiStatus agentUiStatus(const nodegraph::NodeState &state) {
  return uiStatus(nodegraph::agentActivityStatus(state));
}

std::string threadRowPresentationKey(const nodegraph::NodeRef &thread,
                                  const nodegraph::NodeState &state) {
  const std::string correlation =
      exactStringFromValue(valueMember(state, "creationCorrelation"));
  return correlation.empty()
             ? "thread:" + std::to_string(thread->incarnation())
             : ui::threadPresentationKey(thread->id().canonical, correlation);
}

std::string settingString(const nodegraph::Value *value,
                          std::string_view fallback = {}) {
  const std::string result = exactStringFromValue(value);
  return result.empty() ? std::string(fallback) : result;
}

std::string settingSandbox(const nodegraph::Value *value) {
  std::string type = settingString(value);
  if (const auto *object = value ? value->asObject() : nullptr)
    type = settingString(valueMember(*object, "type"));
  if (type == "readOnly")
    return "read-only";
  if (type == "workspaceWrite")
    return "workspace-write";
  if (type == "dangerFullAccess")
    return "danger-full-access";
  if (type == "externalSandbox")
    return "external";
  if (type == "read-only" || type == "workspace-write" ||
      type == "danger-full-access" || type == "external")
    return type;
  return std::string(DefaultTurnSetting);
}

std::vector<TurnSettingModel>
settingModels(nodegraph::NodeGraph::ReadAccess &read) {
  std::vector<TurnSettingModel> result;
  const nodegraph::NodeRef catalog =
      read.find({nodegraph::NodeKind::Catalog, "model"});
  if (!catalog)
    return result;
  for (const nodegraph::NodeRef &entry : read.children(catalog)) {
    const auto state = read.state(entry);
    const nodegraph::Value *hidden = valueMember(*state, "hidden");
    if (hidden && hidden->asBool() && *hidden->asBool())
      continue;
    std::string id = settingString(valueMember(*state, "model"));
    if (id.empty())
      id = settingString(valueMember(*state, "id"));
    if (id.empty())
      continue;
    TurnSettingModel model;
    model.choice.value = id;
    model.choice.label = settingString(valueMember(*state, "displayName"));
    if (model.choice.label.empty())
      model.choice.label = id;
    model.choice.description =
        settingString(valueMember(*state, "description"));
    model.isDefault = boolFromValue(valueMember(*state, "isDefault"));
    model.defaultReasoningEffort =
        settingString(valueMember(*state, "defaultReasoningEffort"));
    model.defaultServiceTier =
        settingString(valueMember(*state, "defaultServiceTier"));
    std::unordered_set<std::string> effortIds;
    if (const nodegraph::Value *supported =
            valueMember(*state, "supportedReasoningEfforts");
        supported && supported->asArray()) {
      for (const nodegraph::Value &option : *supported->asArray()) {
        const auto *fields = option.asObject();
        const std::string effort =
            fields ? settingString(valueMember(*fields, "reasoningEffort"))
                   : std::string{};
        if (!effort.empty() && effortIds.insert(effort).second)
          model.reasoningEfforts.push_back(effort);
      }
    }
    std::unordered_set<std::string> tierIds;
    if (const nodegraph::Value *tiers = valueMember(*state, "serviceTiers");
        tiers && tiers->asArray()) {
      for (const nodegraph::Value &tier : *tiers->asArray()) {
        const auto *fields = tier.asObject();
        const std::string tierId =
            fields ? settingString(valueMember(*fields, "id")) : std::string{};
        if (tierId.empty() || !tierIds.insert(tierId).second)
          continue;
        std::string label = settingString(valueMember(*fields, "name"));
        model.serviceTiers.push_back(
            {tierId, label.empty() ? tierId : std::move(label),
             settingString(valueMember(*fields, "description"))});
      }
    }
    if (const nodegraph::Value *tiers =
            valueMember(*state, "additionalSpeedTiers");
        tiers && tiers->asArray()) {
      for (const nodegraph::Value &tier : *tiers->asArray()) {
        const std::string value = settingString(&tier);
        if (!value.empty() && tierIds.insert(value).second)
          model.serviceTiers.push_back({value, {}, {}});
      }
    }
    if (const nodegraph::Value *personality =
            valueMember(*state, "supportsPersonality");
        personality && personality->asBool())
      model.supportsPersonality = *personality->asBool();
    result.push_back(std::move(model));
  }
  return result;
}

std::vector<TurnSettingChoice>
settingPermissionProfiles(nodegraph::NodeGraph::ReadAccess &read) {
  std::vector<TurnSettingChoice> result;
  const nodegraph::NodeRef catalog =
      read.find({nodegraph::NodeKind::Catalog, "permissionProfile"});
  if (!catalog)
    return result;
  for (const nodegraph::NodeRef &entry : read.children(catalog)) {
    const auto state = read.state(entry);
    const nodegraph::Value *allowed = valueMember(*state, "allowed");
    if (allowed && allowed->asBool() && !*allowed->asBool())
      continue;
    const std::string id = settingString(valueMember(*state, "id"));
    if (!id.empty())
      result.push_back(
          {id, {}, settingString(valueMember(*state, "description"))});
  }
  return result;
}

bool canControlRequests(nodegraph::NodeGraph::ReadAccess &read) {
  const nodegraph::NodeRef connection =
      read.find({nodegraph::NodeKind::Connection, "connection"});
  if (!connection)
    return false;
  const auto state = read.state(connection);
  return state && state->status == nodegraph::NodeStatus::Connected &&
         scalarTextFromValue(valueMember(*state, "providerState")) == "ready" &&
         scalarTextFromValue(valueMember(*state, "role")) == "controller";
}

nodegraph::NodeRef
interactionThread(nodegraph::NodeGraph::ReadAccess &read,
                  const nodegraph::NodeRef &interaction) {
  const std::size_t targetCount = read.relatedCount(
      interaction, nodegraph::RelationKind::InteractionTarget);
  for (std::size_t index = 0; index < targetCount; ++index)
    for (nodegraph::NodeRef target = read.relatedAt(
             interaction, nodegraph::RelationKind::InteractionTarget, index);
         target; target = read.parent(target))
      if (target->id().kind == nodegraph::NodeKind::Thread)
        return target;
  return {};
}

std::string requestThreadId(const nodegraph::NodeState &state,
                            const nodegraph::NodeRef &exactThread) {
  if (exactThread)
    return exactThread->id().canonical;
  const nodegraph::Value *payload = valueMember(state, "payload");
  const auto *object = payload ? payload->asObject() : nullptr;
  return object ? scalarTextFromValue(valueMember(*object, "threadId"))
                : std::string{};
}

PendingRequestDescriptor
projectPendingRequest(nodegraph::NodeGraph::ReadAccess &read,
                      const nodegraph::NodeRef &interaction, bool canControl) {
  const auto state = read.state(interaction);
  PendingRequestDescriptor request;
  request.target = interaction;
  request.method = scalarTextFromValue(valueMember(*state, "method"));
  request.kind = PendingRequestPolicy::kindForMethod(request.method);
  request.error = scalarTextFromValue(valueMember(*state, "error"));
  request.responseRevision = nodegraph::unsignedIntegerFromValue(
                                 valueMember(*state, "responseRevision"))
                                 .value_or(0);

  const nodegraph::Value *payload = valueMember(*state, "payload");
  if (payload)
    request.raw = jsonFromValue(*payload);

  const nodegraph::NodeRef thread = interactionThread(read, interaction);
  request.threadId = requestThreadId(*state, thread);
  if (thread) {
    const auto threadState = read.state(thread);
    request.threadTitle =
        scalarTextFromValue(valueMember(*threadState, "name"));
    if (request.threadTitle.empty())
      request.threadTitle =
          scalarTextFromValue(valueMember(*threadState, "title"));
  }

  const nodegraph::Value *retained =
      valueMember(*state, "retainedResponsePayload");
  if (const auto *object = retained ? retained->asObject() : nullptr) {
    PendingRequestSubmission submission;
    submission.choice = scalarTextFromValue(valueMember(*object, "choice"));
    if (const nodegraph::Value *input = valueMember(*object, "input"))
      submission.input = jsonFromValue(*input);
    if (const nodegraph::Value *metadata = valueMember(*object, "metadata"))
      submission.metadata = jsonFromValue(*metadata);
    if (!submission.choice.empty())
      request.retainedSubmission = std::move(submission);
  }

  const bool recoveryOnly = boolFromValue(valueMember(*state, "recoveryOnly"));
  request.availability = recoveryOnly ? PendingRequestAvailability::RecoveryOnly
                         : canControl ? PendingRequestAvailability::Actionable
                                      : PendingRequestAvailability::Unavailable;
  return request;
}

template <typename Integer>
std::optional<Integer> numericSuffix(std::string_view key,
                                     std::string_view prefix) {
  if (!key.starts_with(prefix))
    return std::nullopt;
  Integer result = 0;
  const char *first = key.data() + prefix.size();
  const char *last = key.data() + key.size();
  const auto parsed = std::from_chars(first, last, result);
  return parsed.ec == std::errc{} && parsed.ptr == last
             ? std::optional<Integer>{result} : std::nullopt;
}

std::optional<std::size_t>
requestPosition(nodegraph::NodeGraph::ReadAccess &read,
                const nodegraph::NodeRef &runtime, std::string_view key) {
  const auto incarnation = numericSuffix<std::uint64_t>(key, "request:");
  if (!incarnation)
    return std::nullopt;
  std::size_t first = 0;
  std::size_t last = read.relatedCount(
      runtime, nodegraph::RelationKind::PendingInteraction);
  while (first < last) {
    const std::size_t middle = first + (last - first) / 2;
    const nodegraph::NodeRef candidate = read.relatedAt(
        runtime, nodegraph::RelationKind::PendingInteraction, middle);
    if (candidate->incarnation() < *incarnation)
      first = middle + 1;
    else
      last = middle;
  }
  const nodegraph::NodeRef found = read.relatedAt(
      runtime, nodegraph::RelationKind::PendingInteraction, first);
  return found && found->incarnation() == *incarnation
             ? std::optional<std::size_t>{first}
             : std::nullopt;
}

bool requestTargetsThread(nodegraph::NodeGraph::ReadAccess &read,
                          const nodegraph::NodeRef &interaction,
                          const nodegraph::NodeState &state,
                          std::string_view selectedThreadId) {
  return requestThreadId(state, interactionThread(read, interaction)) ==
         selectedThreadId;
}

template <typename Visitor>
void forEachPendingRequestThread(nodegraph::NodeGraph::ReadAccess &read,
                                 Visitor &&visit) {
  const nodegraph::NodeRef runtime =
      read.find({nodegraph::NodeKind::Runtime, "runtime"});
  if (!runtime)
    return;
  const std::size_t count = read.relatedCount(
      runtime, nodegraph::RelationKind::PendingInteraction);
  for (std::size_t index = 0; index < count; ++index) {
    const nodegraph::NodeRef interaction = read.relatedAt(
        runtime, nodegraph::RelationKind::PendingInteraction, index);
    if (const nodegraph::NodeRef thread =
            interactionThread(read, interaction))
      visit(thread);
  }
}

std::unordered_map<const nodegraph::Node *, std::size_t>
pendingRequestCounts(nodegraph::NodeGraph::ReadAccess &read) {
  std::unordered_map<const nodegraph::Node *, std::size_t> result;
  forEachPendingRequestThread(
      read, [&](const nodegraph::NodeRef &thread) { ++result[thread.get()]; });
  return result;
}

int pendingRequestRank(nodegraph::NodeGraph::ReadAccess &read,
                       const nodegraph::NodeRef &interaction,
                       const nodegraph::NodeState &state, bool canControl,
                       std::string_view selectedThreadId) {
  const nodegraph::Value *retained =
      valueMember(state, "retainedResponsePayload");
  const auto *object = retained ? retained->asObject() : nullptr;
  const bool hasRetained =
      object && !scalarTextFromValue(valueMember(*object, "choice")).empty();
  const PendingRequestAvailability availability =
      boolFromValue(valueMember(state, "recoveryOnly"))
          ? PendingRequestAvailability::RecoveryOnly
          : canControl ? PendingRequestAvailability::Actionable
                       : PendingRequestAvailability::Unavailable;
  return PendingRequestPolicy::attentionRank(
      availability, hasRetained,
      requestTargetsThread(read, interaction, state, selectedThreadId));
}

void boundInspectorWindow(InspectorPageSnapshot &page, std::size_t count) {
  page.first = std::min(page.first, page.total);
  if (page.total == 0 || count == 0)
    return;
  count = std::min(count, page.total);
  if (page.first == page.total)
    page.first = page.total - count;
  if (!page.anchorIndex)
    return;
  if (*page.anchorIndex < page.first)
    page.first = *page.anchorIndex;
  else if (*page.anchorIndex - page.first >= count)
    page.first = *page.anchorIndex - count + 1;
}

template <typename Project>
void projectInspectorWindow(InspectorPageSnapshot &page,
                            const InspectorRowRequest &request,
                            Project &&project) {
  page.first = request.first;
  const std::size_t count = std::min(request.count, MaximumInspectorRows);
  boundInspectorWindow(page, count);
  const std::size_t end =
      page.first + std::min(count, page.total - page.first);
  page.rows.reserve(end - page.first);
  for (std::size_t row = page.first; row < end; ++row)
    page.rows.push_back(project(row));
  if (page.focusedIndex &&
      (*page.focusedIndex < page.first || *page.focusedIndex >= end))
    page.focused = project(*page.focusedIndex);
}

InspectorPageSnapshot projectPendingRequests(
    nodegraph::NodeGraph::ReadAccess &read, bool canControl,
    const InspectorRowRequest &window) {
  InspectorPageSnapshot result;
  const nodegraph::NodeRef runtime =
      read.find({nodegraph::NodeKind::Runtime, "runtime"});
  if (!runtime) {
    result.emptyMessage = "No pending requests.";
    return result;
  }
  result.orderRevision = read.pendingInteractionOrderRevision();
  result.total = read.relatedCount(
      runtime, nodegraph::RelationKind::PendingInteraction);
  result.anchorIndex = requestPosition(read, runtime, window.anchorKey);
  result.focusedIndex = requestPosition(read, runtime, window.focusedKey);
  projectInspectorWindow(result, window, [&](std::size_t index) {
    const nodegraph::NodeRef interaction = read.relatedAt(
        runtime, nodegraph::RelationKind::PendingInteraction, index);
    return InspectorRow{"request:" + std::to_string(interaction->incarnation()),
                        projectPendingRequest(read, interaction, canControl)};
  });
  if (result.total == 0)
    result.emptyMessage = "No pending requests.";
  return result;
}

PendingRequestsSummary projectPendingRequestSummary(
    nodegraph::NodeGraph::ReadAccess &read, bool canControl,
    std::string_view selectedThreadId,
    std::span<const nodegraph::NodeRef> localTargets) {
  PendingRequestsSummary result;
  const nodegraph::NodeRef runtime =
      read.find({nodegraph::NodeKind::Runtime, "runtime"});
  if (!runtime)
    return result;
  std::unordered_set<const nodegraph::Node *> local;
  local.reserve(localTargets.size());
  for (const nodegraph::NodeRef &target : localTargets)
    if (target)
      local.insert(target.get());
  int bestRank = std::numeric_limits<int>::max();
  nodegraph::NodeRef best;
  std::size_t bestInsertion = 0;
  for (std::size_t index = 0;
       index < read.relatedCount(runtime,
                                 nodegraph::RelationKind::PendingInteraction);
       ++index) {
    const nodegraph::NodeRef interaction = read.relatedAt(
        runtime, nodegraph::RelationKind::PendingInteraction, index);
    ++result.total;
    if (local.contains(interaction.get())) {
      result.candidates.push_back(
          projectPendingRequest(read, interaction, canControl));
      continue;
    }
    const auto state = read.state(interaction);
    const int rank = pendingRequestRank(read, interaction, *state, canControl,
                                        selectedThreadId);
    if (rank < bestRank) {
      bestRank = rank;
      best = interaction;
      bestInsertion = result.candidates.size();
    }
  }
  if (best)
    result.candidates.insert(
        result.candidates.begin() + static_cast<std::ptrdiff_t>(bestInsertion),
        projectPendingRequest(read, best, canControl));
  return result;
}

void appendUniqueBounded(std::vector<std::string> &values, std::string value,
                         std::size_t maximum) {
  if (value.empty() || std::ranges::find(values, value) != values.end())
    return;
  if (values.size() == maximum)
    values.erase(values.begin());
  values.emplace_back(std::move(value));
}

std::string_view nodeKindName(nodegraph::NodeKind kind) {
  using nodegraph::NodeKind;
  switch (kind) {
  case NodeKind::Runtime:
    return "Runtime";
  case NodeKind::Connection:
    return "Connection";
  case NodeKind::Thread:
    return "Thread";
  case NodeKind::Turn:
    return "Turn";
  case NodeKind::Item:
    return "Item";
  case NodeKind::Interaction:
    return "Interaction";
  case NodeKind::Operation:
    return "Operation";
  case NodeKind::Catalog:
    return "Catalog";
  case NodeKind::CatalogEntry:
    return "CatalogEntry";
  case NodeKind::Account:
    return "Account";
  case NodeKind::Configuration:
    return "Configuration";
  case NodeKind::PermissionProfile:
    return "PermissionProfile";
  case NodeKind::Skill:
    return "Skill";
  case NodeKind::Hook:
    return "Hook";
  case NodeKind::Plugin:
    return "Plugin";
  case NodeKind::App:
    return "App";
  case NodeKind::McpServer:
    return "McpServer";
  case NodeKind::Project:
    return "Project";
  case NodeKind::ThreadSection:
    return "ThreadSection";
  case NodeKind::Process:
    return "Process";
  case NodeKind::RealtimeSession:
    return "RealtimeSession";
  case NodeKind::FilesystemWatch:
    return "FilesystemWatch";
  case NodeKind::ExternalAgentImport:
    return "ExternalAgentImport";
  case NodeKind::FuzzyFileSearchSession:
    return "FuzzyFileSearchSession";
  case NodeKind::LoginAttempt:
    return "LoginAttempt";
  case NodeKind::Notice:
    return "Notice";
  case NodeKind::UnknownProtocol:
    return "UnknownProtocol";
  }
  return "Unknown";
}

bool sensitiveStateField(std::string_view key) {
  std::string normalized;
  normalized.reserve(key.size());
  for (const unsigned char character : key)
    if (std::isalnum(character))
      normalized.push_back(static_cast<char>(std::tolower(character)));
  return normalized == "payload" || normalized == "requestpayload" ||
         normalized == "responsepayload" ||
         normalized == "retainedresponsepayload" || normalized == "raw" ||
         normalized == "private" || normalized == "bytes" ||
         normalized == "command" || normalized == "prompt" ||
         normalized == "input" || normalized == "output" ||
         normalized == "delta" || normalized == "error" ||
         normalized == "message" || normalized == "token" ||
         normalized.ends_with("token") ||
         normalized.find("password") != std::string::npos ||
         normalized.find("secret") != std::string::npos ||
         normalized.find("credential") != std::string::npos ||
         normalized.find("authorization") != std::string::npos ||
         normalized.find("cookie") != std::string::npos ||
         normalized.find("apikey") != std::string::npos ||
         normalized.find("privatekey") != std::string::npos;
}

nlohmann::json safeStateValue(const nodegraph::Value &value, int depth = 0) {
  if (depth > 8)
    return "<nested value>";
  if (value.isNull())
    return nullptr;
  if (const auto *boolean = value.asBool())
    return *boolean;
  if (const auto *number = value.asInt64())
    return *number;
  if (const auto *number = value.asUInt64())
    return *number;
  if (const auto *number = value.asDouble())
    return *number;
  if (const auto *string = value.asString()) {
    if (string->size() > 1024)
      return string->substr(0, 1024) + "...";
    return *string;
  }
  if (const auto *array = value.asArray()) {
    nlohmann::json result = nlohmann::json::array();
    const std::size_t retained = std::min<std::size_t>(array->size(), 32);
    for (std::size_t index = 0; index < retained; ++index)
      result.push_back(safeStateValue(array->at(index), depth + 1));
    if (array->size() > retained)
      result.push_back("<more entries omitted>");
    return result;
  }
  nlohmann::json result = nlohmann::json::object();
  const auto *object = value.asObject();
  std::size_t retained = 0;
  for (const auto &[key, entry] : *object) {
    if (retained++ == 64) {
      result["<more fields>"] = "omitted";
      break;
    }
    result[key] = sensitiveStateField(key) ? nlohmann::json("<redacted>")
                                           : safeStateValue(entry, depth + 1);
  }
  return result;
}

nlohmann::json safeStateObject(const nodegraph::Value::Object &fields) {
  return safeStateValue(nodegraph::Value(fields));
}

std::string sectionComponent(std::string_view prefix, std::string_view threadId,
                             std::string_view suffix) {
  std::string result(prefix);
  result += std::to_string(threadId.size());
  result.push_back(':');
  result.append(threadId);
  result += std::to_string(suffix.size());
  result.push_back(':');
  result.append(suffix);
  return result;
}

std::optional<ThreadListRow>
projectThreadRow(nodegraph::NodeGraph::ReadAccess &read,
                 const nodegraph::NodeRef &thread, std::size_t pending) {
  if (!read.live(thread) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;
  const auto state = read.state(thread);
  if (!state)
    return std::nullopt;
  const auto timestamp = [&state](std::string_view field) {
    return signedIntegerFromValue(valueMember(*state, field));
  };
  ThreadListRow row;
  row.id = thread->id().canonical;
  row.presentationKey = threadRowPresentationKey(thread, *state);
  row.target = thread;
  row.title = scalarTextFromValue(valueMember(*state, "localNameOverlay"));
  if (row.title.empty())
    row.title = scalarTextFromValue(valueMember(*state, "name"));
  if (row.title.empty())
    row.title = scalarTextFromValue(valueMember(*state, "preview"));
  if (row.title.empty())
    row.title = row.id.substr(0, std::min<std::size_t>(12, row.id.size()));
  row.cwd = scalarTextFromValue(valueMember(*state, "cwd"));
  row.status = statusFromState(*state);
  row.createdAt = timestamp("createdAt");
  row.updatedAt = timestamp("updatedAt");
  row.recencyAt = timestamp("recencyAt");
  if (const auto local = timestamp("localPromptActivityAt");
      local && (!row.recencyAt || *local > *row.recencyAt))
    row.recencyAt = local;
  for (const std::string_view field :
       {std::string_view("lastActivityAt"), std::string_view("updatedAt"),
        std::string_view("recencyAt"), std::string_view("localActivityAt"),
        std::string_view("localPromptActivityAt")}) {
    const std::optional<std::int64_t> candidate = timestamp(field);
    if (candidate && (!row.lastActivityAt || *candidate > *row.lastActivityAt))
      row.lastActivityAt = candidate;
  }
  row.pending = pending;
  const PendingPromptPresentation prompt =
      pendingPromptPresentation(read, thread);
  row.awaitingPromptAcknowledgement = prompt.awaitingAcknowledgement;
  row.pendingPromptAdmittedAtMs = prompt.admittedAtMs;
  row.archived = boolFromValue(valueMember(*state, "archived"));
  return row;
}

InspectorPageSnapshot projectPlan(
    nodegraph::NodeGraph::ReadAccess &read,
    const nodegraph::NodeRef &thread,
    const nodegraph::InspectorThreadIndex *index,
    const InspectorRowRequest &request) {
  InspectorPageSnapshot result;
  if (!thread || !index || !index->planSource) {
    result.emptyMessage = thread ? "No plan for this thread."
                                 : "No selected thread.";
    return result;
  }
  const bool legacy =
      index->planSource->id().kind == nodegraph::NodeKind::Item;
  const std::string sourceId =
      std::to_string(index->planSource->incarnation());
  const std::string explanationKey = "plan:" + sourceId + ":explanation";
  const std::string stepPrefix = "plan:step:" + sourceId + ':';
  const auto keyFor = [&](std::size_t row) {
    if (legacy)
      return "plan:" + sourceId + ":legacy";
    if (index->planHasExplanation && row == 0)
      return explanationKey;
    return stepPrefix +
           std::to_string(row - (index->planHasExplanation ? 1 : 0));
  };
  result.total = index->planRowCount;
  result.orderRevision = index->planOrderRevision;
  if (result.total == 0 && legacy)
    result.emptyMessage = "Plan is being prepared.";
  const auto find = [&](const std::string &key) -> std::optional<std::size_t> {
    if (key.empty())
      return std::nullopt;
    if (legacy || key == explanationKey)
      return result.total != 0 && key == keyFor(0)
                 ? std::optional<std::size_t>{0}
                 : std::nullopt;
    const auto sourceIndex = numericSuffix<std::size_t>(key, stepPrefix);
    const std::size_t offset = index->planHasExplanation ? 1 : 0;
    if (!sourceIndex || *sourceIndex >= result.total - offset)
      return std::nullopt;
    return *sourceIndex + offset;
  };
  result.anchorIndex = find(request.anchorKey);
  result.focusedIndex = find(request.focusedKey);
  const auto source = read.state(index->planSource);
  const nodegraph::InspectorPlanView plan =
      nodegraph::inspectorPlanView(*source);
  const UiStatus turnStatus = statusFromState(
      *read.state(legacy ? read.parent(index->planSource) : index->planSource));
  const UiStatus threadStatus = statusFromState(*read.state(thread));
  const auto project = [&](std::size_t row) -> InspectorRow {
    const std::string key = keyFor(row);
    if (!legacy && index->planHasExplanation && row == 0) {
      return InspectorRow{
          key,
          InspectorMarkdownRow{scalarTextFromValue(plan.explanation),
                               "Plan explanation", {}}};
    }
    if (legacy)
      return InspectorRow{
          key,
          InspectorMarkdownRow{scalarTextFromValue(valueMember(*source, "text")),
                               "Plan", {}}};
    const auto *steps = plan.steps->asArray();
    const std::size_t sourceIndex =
        row - (index->planHasExplanation ? 1 : 0);
    const auto *step = steps->at(sourceIndex).asObject();
    if (!step)
      return InspectorRow{key, PlanStepData{}};
    std::string stepText = scalarTextFromValue(valueMember(*step, "step"));
    if (stepText.empty())
      stepText = scalarTextFromValue(valueMember(*step, "text"));
    return InspectorRow{
        key,
        PlanStepData{
            std::move(stepText),
            effectivePlanStepStatus(
                uiStatusFromValue(valueMember(*step, "status")), turnStatus,
                threadStatus)}};
  };
  projectInspectorWindow(result, request, project);
  return result;
}

InspectorPageSnapshot projectAgents(
    nodegraph::NodeGraph::ReadAccess &read,
    const nodegraph::NodeRef &thread,
    const nodegraph::InspectorThreadIndex *index,
    const InspectorRowRequest &request) {
  InspectorPageSnapshot result;
  result.validRetainedKeys.emplace();
  if (!thread || !index || index->agents.empty()) {
    result.emptyMessage = thread ? "No agent activity for this thread."
                                 : "No selected thread.";
    return result;
  }
  result.total = index->agents.size();
  result.orderRevision = index->agentOrderRevision;
  const auto logicalKey = [](std::string_view key) {
    constexpr std::string_view prefix = "agent:";
    return key.starts_with(prefix) ? key.substr(prefix.size()) : key;
  };
  const auto find = [&](const std::string &key) -> std::optional<std::size_t> {
    if (key.empty())
      return std::nullopt;
    const auto found = index->agentPositions.find(std::string(logicalKey(key)));
    return found == index->agentPositions.end()
               ? std::nullopt
                 : std::optional<std::size_t>{found->second};
  };
  const std::size_t retainedCount =
      std::min(request.retainedKeys.size(), MaximumInspectorRows);
  result.validRetainedKeys->reserve(retainedCount);
  for (std::size_t index = 0; index < retainedCount; ++index) {
    const std::string &key = request.retainedKeys[index];
    if (find(key) &&
        std::ranges::find(*result.validRetainedKeys, key) ==
            result.validRetainedKeys->end())
      result.validRetainedKeys->push_back(key);
  }
  result.anchorIndex = find(request.anchorKey);
  result.focusedIndex = find(request.focusedKey);
  const auto project = [&](std::size_t row) -> InspectorRow {
    const nodegraph::InspectorAgentSelector &selector = index->agents[row];
    InspectorAgentRow logical;
    if (selector.childIdVisible)
      logical.childThreadId = selector.childId;
    const auto sourceFor = [&](std::size_t field) {
      const auto &candidates = selector.candidates[field];
      return candidates.empty()
                 ? nodegraph::NodeRef{}
                 : selector.contributors[*candidates.rbegin() / 2];
    };
    const std::array targets{&logical.agentPath, &logical.tool, &logical.model,
                             &logical.reasoningEffort, &logical.prompt,
                             &logical.senderThreadId};
    for (std::size_t index = 0;
         index < nodegraph::InspectorAgentDetailFields.size(); ++index)
      if (const nodegraph::NodeRef source = sourceFor(index))
        *targets[index] = scalarTextFromValue(
            valueMember(*read.state(source),
                        nodegraph::InspectorAgentDetailFields[index]));
    const nodegraph::NodeRef receiver = sourceFor(6);
    if (receiver)
      logical.receiverThreadIds =
          graphStrings(valueMember(*read.state(receiver), "receiverThreadIds"));

    const auto status = [&](std::size_t token) {
      const nodegraph::NodeRef &source = selector.contributors[token / 2];
      const auto sourceState = read.state(source);
      const auto *child =
          nodegraph::reportedAgentState(*sourceState, selector.childId);
      const nodegraph::NodeStatusView childStatus =
          child ? nodegraph::nodeStatusView(valueMember(*child, "status"))
                : nodegraph::NodeStatusView{};
      if (token % 2 != 0)
        return uiStatus(childStatus);
      const auto turnState = read.state(read.parent(source));
      return uiStatus(nodegraph::inspectorAgentActivityStatus(
          *sourceState, turnState.get(), childStatus,
          selector.childIdVisible));
    };
    if (!selector.candidates[8].empty()) {
      logical.status = status(*selector.candidates[8].rbegin());
      if (isActiveStatus(logical.status) &&
          !selector.candidates[9].empty()) {
        const std::size_t nonRunning = *selector.candidates[9].rbegin();
        const UiStatus previous = status(nonRunning);
        if (isTerminalTurnStatus(previous))
          logical.status = previous;
      }
    }
    if (!selector.candidates[7].empty()) {
      const std::size_t token = *selector.candidates[7].rbegin();
      const nodegraph::NodeRef &source = selector.contributors[token / 2];
      const auto sourceState = read.state(source);
      if (token % 2 == 0)
        logical.resultText =
            scalarTextFromValue(valueMember(*sourceState, "resultText"));
      else if (const auto *child =
                   nodegraph::reportedAgentState(*sourceState,
                                                 selector.childId))
        logical.resultText =
            scalarTextFromValue(valueMember(*child, "message"));
    }
    const nodegraph::NodeRef &childThread = selector.exactChild;
    if (childThread) {
      updateAgentStatus(logical.status,
                        statusFromState(*read.state(childThread)));
      if (logical.resultText.empty())
        if (const nodegraph::InspectorThreadIndex *childIndex =
                read.inspectorIndex(childThread);
            childIndex)
          if (const nodegraph::NodeRef latest =
                  childIndex->latestAgentMessage())
            logical.resultText = scalarTextFromValue(
                valueMember(*read.state(latest), "text"));
    }
    return InspectorRow{"agent:" + selector.key, std::move(logical)};
  };
  projectInspectorWindow(result, request, project);
  return result;
}

} // namespace

using namespace middle;

NodeGraphUiAdapter::NodeGraphUiAdapter(
    const nodegraph::NodeGraph &graph) noexcept
    : graph_(&graph) {}

std::optional<ThreadListRow>
NodeGraphUiAdapter::threadRow(const nodegraph::NodeRef &thread) const {
  if (!graph_ || !thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;
  if (!read->live(thread))
    return std::nullopt;
  std::size_t pending = 0;
  forEachPendingRequestThread(*read, [&](const nodegraph::NodeRef &owner) {
    pending += owner == thread;
  });
  return projectThreadRow(*read, thread, pending);
}

std::optional<ThreadListSnapshot>
NodeGraphUiAdapter::threads(const nodegraph::NodeRef &selectedThread,
                            std::uint64_t *graphRevision) const {
  if (!graph_)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;
  if (graphRevision)
    *graphRevision = read->revision();

  const auto pendingCounts = pendingRequestCounts(*read);

  ThreadListSnapshot result;
  if (read->live(selectedThread))
    result.selectedThreadId = selectedThread->id().canonical;

  const nodegraph::NodeRef connection =
      read->find({nodegraph::NodeKind::Connection, "connection"});
  if (connection) {
    const auto state = read->state(connection);
    if (state) {
      const std::string provider =
          scalarTextFromValue(valueMember(*state, "providerState"));
      const bool connected = state->status == nodegraph::NodeStatus::Connected;
      result.providerReady = connected && provider == "ready";
      result.canControl =
          result.providerReady &&
          scalarTextFromValue(valueMember(*state, "role")) == "controller";
    }
  }

  std::vector<nodegraph::NodeRef> allThreads;
  std::unordered_set<const nodegraph::Node *> childThreads;
  for (const nodegraph::NodeRef &node :
       read->orderedNodes(nodegraph::NodeKind::Thread)) {
    allThreads.push_back(node);
    for (const nodegraph::RelationKind kind :
         {nodegraph::RelationKind::StructuralChildThread,
          nodegraph::RelationKind::AgentChildThread,
          nodegraph::RelationKind::ForkChildThread})
      for (std::size_t index = 0; index < read->relatedCount(node, kind);
           ++index) {
        const nodegraph::NodeRef child = read->relatedAt(node, kind, index);
        if (child->id().kind == nodegraph::NodeKind::Thread)
          childThreads.insert(child.get());
      }
  }

  std::unordered_set<const nodegraph::Node *> emitted;
  const auto buildRow = [&](const auto &self,
                            const nodegraph::NodeRef &node) -> ThreadListRow {
    if (!node || !emitted.insert(node.get()).second)
      return {};
    const auto pending = pendingCounts.find(node.get());
    std::optional<ThreadListRow> projected = projectThreadRow(
        *read, node, pending == pendingCounts.end() ? 0 : pending->second);
    if (!projected)
      return {};
    ThreadListRow row = std::move(*projected);
    std::unordered_set<const nodegraph::Node *> localChildren;
    for (const nodegraph::RelationKind kind :
         {nodegraph::RelationKind::StructuralChildThread,
          nodegraph::RelationKind::AgentChildThread,
          nodegraph::RelationKind::ForkChildThread}) {
      for (std::size_t index = 0; index < read->relatedCount(node, kind);
           ++index) {
        const nodegraph::NodeRef child = read->relatedAt(node, kind, index);
        if (!child || child->id().kind != nodegraph::NodeKind::Thread ||
            !localChildren.insert(child.get()).second)
          continue;
        ThreadListRow projected = self(self, child);
        if (!projected.id.empty())
          row.children.push_back(std::move(projected));
      }
    }
    return row;
  };

  std::vector<nodegraph::NodeRef> roots;
  const nodegraph::NodeRef runtime =
      read->find({nodegraph::NodeKind::Runtime, "runtime"});
  if (runtime)
    roots = read->related(runtime, nodegraph::RelationKind::RootThread);
  for (const nodegraph::NodeRef &thread : allThreads)
    if (!childThreads.contains(thread.get()) &&
        std::ranges::find(roots, thread) == roots.end())
      roots.push_back(thread);
  for (const nodegraph::NodeRef &root : roots) {
    ThreadListRow row = buildRow(buildRow, root);
    if (!row.id.empty())
      result.roots.push_back(std::move(row));
  }
  // Malformed or partially paged ownership must not make a canonical thread
  // disappear. Keep any still-unreachable thread as a root until its owner is
  // available.
  for (const nodegraph::NodeRef &thread : allThreads) {
    if (emitted.contains(thread.get()))
      continue;
    ThreadListRow row = buildRow(buildRow, thread);
    if (!row.id.empty())
      result.roots.push_back(std::move(row));
  }
  return result;
}

std::optional<NodeGraphUiAdapter::ConversationInfo>
NodeGraphUiAdapter::conversationInfo(const nodegraph::NodeRef &thread) const {
  if (!graph_ || !thread)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->live(thread) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;
  const auto state = read->state(thread);
  if (!state)
    return std::nullopt;

  ConversationInfo result;
  const std::string hydration =
      scalarTextFromValue(valueMember(*state, "hydrationState"));
  const bool local = boolFromValue(valueMember(*state, "local"));
  const bool recoveryOnly = boolFromValue(valueMember(*state, "recoveryOnly"));
  result.readyForDisplay = hydration == "ready" || local || recoveryOnly;
  result.hydrationFailed = hydration == "failed";
  result.providerHasMore = graphProviderHasMoreHistory(*state);
  result.historyRequestPending = hasPendingHistoryRequest(*read, thread);
  return result;
}

std::optional<InspectorPageSnapshot>
NodeGraphUiAdapter::pendingRequests(const InspectorRowRequest &request) const {
  if (!graph_)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;
  return projectPendingRequests(*read, canControlRequests(*read), request);
}

std::optional<PendingRequestsSummary> NodeGraphUiAdapter::pendingRequestSummary(
    std::string_view selectedThreadId,
    std::span<const nodegraph::NodeRef> localTargets) const {
  if (!graph_)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;
  return projectPendingRequestSummary(*read, canControlRequests(*read),
                                      selectedThreadId, localTargets);
}

std::optional<PendingRequestDescriptor>
NodeGraphUiAdapter::pendingRequest(const nodegraph::NodeRef &target,
                                   bool *busy) const {
  if (busy)
    *busy = false;
  if (!graph_ || !target)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read) {
    if (busy)
      *busy = true;
    return std::nullopt;
  }
  if (!read->live(target))
    return std::nullopt;
  const nodegraph::NodeRef runtime =
      read->find({nodegraph::NodeKind::Runtime, "runtime"});
  if (!runtime ||
      !read->isRelated(runtime, nodegraph::RelationKind::PendingInteraction,
                       target))
    return std::nullopt;
  return projectPendingRequest(*read, target, canControlRequests(*read));
}

std::optional<TurnSettingsContext>
NodeGraphUiAdapter::turnSettings(const nodegraph::NodeRef &thread,
                                 std::string_view draftIdentity,
                                 std::string_view draftWorkspace) const {
  if (!graph_)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;

  TurnSettingsContext result;
  result.models = settingModels(*read);
  result.permissionProfiles = settingPermissionProfiles(*read);
  if (const nodegraph::NodeRef connection =
          read->find({nodegraph::NodeKind::Connection, "connection"}))
    result.providerAuthorityRevision =
        nodegraph::unsignedIntegerFromValue(
            valueMember(*read->state(connection), "providerAuthorityRevision"))
            .value_or(0);
  if (!thread) {
    result.identity = std::string(draftIdentity);
    result.canonical[TurnSettingField::Workspace] = std::string(draftWorkspace);
    return result;
  }
  if (thread->id().kind != nodegraph::NodeKind::Thread ||
      !read->live(thread))
    return std::nullopt;
  const auto state = read->state(thread);

  result.threadIncarnation = thread->incarnation();
  result.identity = boolFromValue(valueMember(*state, "local"))
                        ? threadRowPresentationKey(thread, *state)
                        : thread->id().canonical;
  const nodegraph::Value *acknowledgements =
      valueMember(*state, "settingsAcknowledgements");
  const auto *acknowledged =
      acknowledgements ? acknowledgements->asObject() : nullptr;
  const auto acknowledge = [&result, acknowledged](TurnSettingField field,
                                                   std::string_view name) {
    result.acknowledged[static_cast<std::size_t>(field)] =
        acknowledged ? nodegraph::unsignedIntegerFromValue(
                           valueMember(*acknowledged, name))
                           .value_or(0)
                     : 0;
  };
  const auto assign = [&result, &state, &acknowledge](
                          TurnSettingField field, std::string_view name,
                          std::string_view authority = {}) {
    result.canonical[field] =
        settingString(valueMember(*state, name), DefaultTurnSetting);
    acknowledge(field, authority.empty() ? name : authority);
  };
  assign(TurnSettingField::Model, "model");
  result.canonical[TurnSettingField::Effort] =
      settingString(valueMember(*state, "reasoningEffort"), DefaultTurnSetting);
  if (result.canonical[TurnSettingField::Effort] == DefaultTurnSetting)
    result.canonical[TurnSettingField::Effort] =
        settingString(valueMember(*state, "effort"), DefaultTurnSetting);
  acknowledge(TurnSettingField::Effort, "effort");
  assign(TurnSettingField::Personality, "personality");
  assign(TurnSettingField::Approval, "approvalPolicy", "approval");
  assign(TurnSettingField::Reviewer, "approvalsReviewer", "reviewer");
  result.canonical[TurnSettingField::Workspace] =
      settingString(valueMember(*state, "cwd"));
  acknowledge(TurnSettingField::Workspace, "cwd");
  assign(TurnSettingField::ServiceTier, "serviceTier");
  assign(TurnSettingField::Summary, "summary");

  const nodegraph::Value *sandbox = valueMember(*state, "sandboxPolicy");
  if (!sandbox)
    sandbox = valueMember(*state, "sandbox");
  result.canonical[TurnSettingField::Sandbox] = settingSandbox(sandbox);
  const auto *sandboxObject = sandbox ? sandbox->asObject() : nullptr;
  const nodegraph::Value *network =
      sandboxObject ? valueMember(*sandboxObject, "networkAccess") : nullptr;
  const bool networkEnabled =
      network && ((network->asBool() && *network->asBool()) ||
                  scalarTextFromValue(network) == "enabled");
  const std::string &access = result.canonical[TurnSettingField::Sandbox];
  result.canonical[TurnSettingField::Network] =
      access == DefaultTurnSetting ? std::string(DefaultTurnSetting)
      : access == "danger-full-access" || networkEnabled ? "enabled"
                                                         : "restricted";
  acknowledge(TurnSettingField::Sandbox, "sandbox");
  acknowledge(TurnSettingField::Network, "sandbox");

  if (const nodegraph::Value *profile =
          valueMember(*state, "activePermissionProfile")) {
    const std::string id =
        profile->asObject()
            ? settingString(valueMember(*profile->asObject(), "id"))
            : settingString(profile);
    if (!id.empty())
      result.canonical[TurnSettingField::PermissionProfile] = id;
  }
  acknowledge(TurnSettingField::PermissionProfile, "permissionProfile");
  if (const nodegraph::Value *collaboration =
          valueMember(*state, "collaborationMode");
      collaboration && collaboration->asObject()) {
    const std::string mode =
        settingString(valueMember(*collaboration->asObject(), "mode"));
    if (!mode.empty())
      result.canonical[TurnSettingField::Collaboration] = mode;
  }
  acknowledge(TurnSettingField::Collaboration, "collaboration");
  return result;
}

std::optional<InspectorSnapshot>
NodeGraphUiAdapter::inspector(const nodegraph::NodeRef &selectedThread,
                              InspectorProjection projection,
                              const InspectorRowRequest &request) const {
  if (!graph_)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;

  nodegraph::NodeRef thread;
  if (selectedThread &&
      selectedThread->id().kind == nodegraph::NodeKind::Thread &&
      read->live(selectedThread))
    thread = selectedThread;

  InspectorSnapshot result;
  const bool wantPlan = projection == InspectorProjection::Plan;
  const bool wantAgents = projection == InspectorProjection::Agents;
  const bool wantChanges = projection == InspectorProjection::Changes;
  const bool wantRequests = projection == InspectorProjection::Requests;
  const bool wantState = projection == InspectorProjection::State;
  result.threadIncarnation = thread ? thread->incarnation() : 0;
  result.changes.threadId = thread ? thread->id().canonical : std::string{};

  std::map<std::string, std::size_t, std::less<>> kindCounts;
  nlohmann::json domains = nlohmann::json::array();
  std::size_t omittedDomains = 0;
  if (wantState) {
    for (int rawKind = static_cast<int>(nodegraph::NodeKind::Runtime);
         rawKind <= static_cast<int>(nodegraph::NodeKind::UnknownProtocol);
         ++rawKind) {
      const auto kind = static_cast<nodegraph::NodeKind>(rawKind);
      const auto &nodes = read->orderedNodes(kind);
      if (!nodes.empty())
        kindCounts.emplace(std::string(nodeKindName(kind)), nodes.size());
      if (kind == nodegraph::NodeKind::Thread)
        result.state.threadCount = nodes.size();
      else if (kind == nodegraph::NodeKind::UnknownProtocol)
        result.state.telemetryCount = nodes.size();
      if (kind == nodegraph::NodeKind::Catalog)
        for (const nodegraph::NodeRef &node : nodes)
          if (node->id().canonical == "model") {
            const auto state = read->state(node);
            const nodegraph::Value *data = valueMember(*state, "data");
            if (const auto *models = data ? data->asArray() : nullptr)
              result.state.modelCount = models->size();
            break;
          }

      const bool domain = kind != nodegraph::NodeKind::Thread &&
                          kind != nodegraph::NodeKind::Turn &&
                          kind != nodegraph::NodeKind::Item &&
                          kind != nodegraph::NodeKind::Interaction &&
                          kind != nodegraph::NodeKind::Operation &&
                          kind != nodegraph::NodeKind::UnknownProtocol;
      if (!domain)
        continue;
      for (const nodegraph::NodeRef &node : nodes) {
        if (domains.size() >= 96) {
          ++omittedDomains;
          continue;
        }
        const auto state = read->state(node);
        domains.push_back(
            {{"kind", nodeKindName(kind)},
             {"id", node->id().canonical},
             {"status", std::string(statusToken(statusFromState(*state)))},
             {"changedRevision", read->changedRevision(node)},
             {"fields", safeStateObject(state->fields)}});
      }
    }
  }

  nlohmann::json pendingMetadata = nlohmann::json::array();
  if (wantRequests || wantState) {
    InspectorRowRequest metadataRequest;
    metadataRequest.count = MaximumInspectorRows;
    InspectorPageSnapshot requests = projectPendingRequests(
        *read, canControlRequests(*read),
        wantRequests ? request : metadataRequest);
    result.state.pendingRequestCount = requests.total;
    if (wantState)
      for (const InspectorRow &row : requests.rows) {
        const auto *pending = std::get_if<PendingRequestDescriptor>(&row.value);
        if (!pending)
          continue;
        pendingMetadata.push_back(
            {{"id", pending->target->id().canonical},
             {"method", pending->method},
             {"category",
              std::string(PendingRequestPolicy::kindToken(pending->kind))},
             {"thread", pending->threadTitle.empty() ? pending->threadId
                                                    : pending->threadTitle},
             {"status", std::string(statusToken(
                            statusFromState(*read->state(pending->target))))}});
      }
    if (wantRequests)
      result.requests = std::move(requests);
  }

  const nodegraph::InspectorThreadIndex *inspectorIndex =
      thread ? read->inspectorIndex(thread) : nullptr;
  if (wantPlan)
    result.plan = projectPlan(*read, thread, inspectorIndex, request);
  if (wantAgents)
    result.agents = projectAgents(*read, thread, inspectorIndex, request);

  if (thread) {
    const auto threadState = read->state(thread);
    if (wantChanges)
      result.changes.cwd =
          scalarTextFromValue(valueMember(*threadState, "cwd"));
    if (wantState)
      result.state.selectedThreadTurnCount = read->childCount(thread);
    if (wantChanges || wantState)
      for (std::size_t turnIndex = 0; turnIndex < read->childCount(thread);
           ++turnIndex) {
        const nodegraph::NodeRef turn = read->childAt(thread, turnIndex);
        if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
          continue;
        if (wantState)
          result.state.selectedThreadItemCount += read->childCount(turn);
        for (std::size_t itemIndex = 0; itemIndex < read->childCount(turn);
             ++itemIndex) {
          const nodegraph::NodeRef item = read->childAt(turn, itemIndex);
          if (!item || item->id().kind != nodegraph::NodeKind::Item)
            continue;
          const auto state = read->state(item);
          const std::string type =
              scalarTextFromValue(valueMember(*state, "type"));
          if (wantChanges && type == "commandExecution") {
            appendUniqueBounded(result.changes.commandCwds,
                                scalarTextFromValue(valueMember(*state, "cwd")),
                                64);
          } else if (wantChanges && type == "fileChange") {
            appendUniqueBounded(result.changes.commandCwds,
                                scalarTextFromValue(valueMember(*state, "cwd")),
                                64);
            const nodegraph::Value *changes = valueMember(*state, "changes");
            if (const auto *array = changes ? changes->asArray() : nullptr)
              for (const nodegraph::Value &change : *array)
                if (const auto *object = change.asObject())
                  appendUniqueBounded(
                      result.changes.changedPaths,
                      scalarTextFromValue(valueMember(*object, "path")), 512);
          }
        }
      }

  }

  nlohmann::json selected = nullptr;
  if (wantState && thread) {
    const auto state = read->state(thread);
    selected = {{"id", thread->id().canonical},
                {"status", std::string(statusToken(statusFromState(*state)))},
                {"changedRevision", read->changedRevision(thread)},
                {"turns", result.state.selectedThreadTurnCount},
                {"items", result.state.selectedThreadItemCount},
                {"fields", safeStateObject(state->fields)}};
    if (const nodegraph::NodeRef parent = read->parent(thread))
      selected["parent"] = parent->id().canonical;
  }
  if (wantState) {
    nlohmann::json counts = nlohmann::json::object();
    for (const auto &[kind, count] : kindCounts)
      counts[kind] = count;
    result.state.state = {
        {"sharedNodeGraph",
         {{"revision", read->revision()},
          {"nodes", read->orderedNodes().size()},
          {"nodeKinds", std::move(counts)},
          {"selectedThread", std::move(selected)},
          {"currentDomains", std::move(domains)},
          {"omittedDomainCount", omittedDomains},
          {"pendingInteractions", std::move(pendingMetadata)}}}};
  }
  return result;
}

bool NodeGraphUiAdapter::inspectorAffected(
    const nodegraph::GraphChanged &change,
    const nodegraph::NodeRef &selectedThread,
    InspectorProjection projection) const {
  if (!graph_)
    return false;
  auto read = graph_->tryRead();
  return read ? inspectorAffected(change, selectedThread, projection, *read)
              : true;
}

bool NodeGraphUiAdapter::inspectorAffected(
    const nodegraph::GraphChanged &change,
    const nodegraph::NodeRef &selectedThread,
    InspectorProjection projection,
    const nodegraph::NodeGraph::ReadAccess &access) const {
  if (projection == InspectorProjection::Protocol)
    return false;
  if (change.rescanRequired)
    return true;
  if (projection == InspectorProjection::State) {
    if (!change.childListsChanged.empty())
      return true;
    const auto changesSummary = [&](const nodegraph::NodeRef &node) {
      if (!node)
        return false;
      if (node == selectedThread)
        return true;
      return node->id().kind != nodegraph::NodeKind::Turn &&
             node->id().kind != nodegraph::NodeKind::Item &&
             node->id().kind != nodegraph::NodeKind::Operation;
    };
    return std::ranges::any_of(change.affected, changesSummary) ||
           std::ranges::any_of(change.removed, changesSummary);
  }
  if (change.affected.size() + change.removed.size() > 64 ||
      change.childListsChanged.size() > 64)
    return true;
  const auto *read = &access;

  if (projection == InspectorProjection::Requests) {
    if (read->pendingInteractionOrderRevision() == change.revision)
      return true;
    const auto connectionChanged = [&](const nodegraph::NodeRef &node) {
      if (!node || node->id().kind != nodegraph::NodeKind::Connection)
        return false;
      return !read->live(node) ||
             read->statusChangedRevision(node) == change.revision ||
             fieldChanged(*read, node, "role", change.revision) ||
             fieldChanged(*read, node, "providerState", change.revision);
    };
    if (std::ranges::any_of(change.affected, connectionChanged) ||
        std::ranges::any_of(change.removed, connectionChanged))
      return true;
    const nodegraph::NodeRef runtime =
        read->find({nodegraph::NodeKind::Runtime, "runtime"});
    if (!runtime)
      return false;
    constexpr std::array<std::string_view, 6> RequestFields{
        "method", "payload", "error", "responseRevision",
        "retainedResponsePayload", "recoveryOnly"};
    for (const nodegraph::NodeRef &node : change.affected) {
      if (!node || node->id().kind != nodegraph::NodeKind::Interaction ||
          !read->live(node) ||
          !read->isRelated(runtime,
                           nodegraph::RelationKind::PendingInteraction, node))
        continue;
      if (read->structureChangedRevision(node) == change.revision ||
          std::ranges::any_of(RequestFields, [&](std::string_view field) {
            return fieldChanged(*read, node, field, change.revision);
          }))
        return true;
    }
    std::vector<nodegraph::NodeRef> renamedThreads;
    for (const nodegraph::NodeRef &node : change.affected)
      if (node && node->id().kind == nodegraph::NodeKind::Thread &&
          read->live(node) &&
          (fieldChanged(*read, node, "name", change.revision) ||
           fieldChanged(*read, node, "title", change.revision)))
        renamedThreads.push_back(node);
    if (renamedThreads.empty() && change.childListsChanged.empty())
      return false;
    const std::size_t count = read->relatedCount(
        runtime, nodegraph::RelationKind::PendingInteraction);
    for (std::size_t index = 0; index < count; ++index) {
      const nodegraph::NodeRef interaction = read->relatedAt(
          runtime, nodegraph::RelationKind::PendingInteraction, index);
      const std::size_t targetCount = read->relatedCount(
          interaction, nodegraph::RelationKind::InteractionTarget);
      bool resolved = false;
      for (std::size_t targetIndex = 0;
           targetIndex < targetCount && !resolved; ++targetIndex) {
        for (nodegraph::NodeRef node = read->relatedAt(
                 interaction, nodegraph::RelationKind::InteractionTarget,
                 targetIndex);
             node; node = read->parent(node)) {
          if (node->id().kind == nodegraph::NodeKind::Thread) {
            resolved = true;
            if (std::ranges::find(renamedThreads, node) !=
                renamedThreads.end())
              return true;
            break;
          }
          if (read->structureChangedRevision(node) != change.revision)
            continue;
          const nodegraph::NodeRef parent = read->parent(node);
          if (std::ranges::any_of(
                  change.childListsChanged,
                  [&](const nodegraph::ChildListChange &entry) {
                    return entry.childKind == node->id().kind &&
                           (!parent || entry.owner == parent);
                  }))
            return true;
        }
      }
    }
    return false;
  }

  if (!selectedThread)
    return false;
  if (!read->live(selectedThread))
    return true;
  if (projection == InspectorProjection::Plan ||
      projection == InspectorProjection::Agents) {
    const nodegraph::InspectorThreadIndex *index =
        read->inspectorIndex(selectedThread);
    if (!index)
      return true;
    return (projection == InspectorProjection::Plan
                ? index->planChangedRevision
                : index->agentChangedRevision) == change.revision;
  }

  const auto belongsToSelection = [&](nodegraph::NodeRef node) {
    while (node && node != selectedThread && read->live(node))
      node = read->parent(node);
    return node == selectedThread;
  };
  if (std::ranges::any_of(change.childListsChanged, [&](const auto &entry) {
        return entry.owner && belongsToSelection(entry.owner) &&
               ((entry.owner == selectedThread &&
                 entry.childKind == nodegraph::NodeKind::Turn) ||
                (entry.owner->id().kind == nodegraph::NodeKind::Turn &&
                 entry.childKind == nodegraph::NodeKind::Item));
      }))
    return true;
  for (const nodegraph::NodeRef &node : change.affected) {
    if (!node || !read->contains(node))
      continue;
    if (node == selectedThread) {
      if (fieldChanged(*read, node, "cwd", change.revision) ||
          fieldChanged(*read, node, "workspace", change.revision) ||
          fieldChanged(*read, node, "hydrationState", change.revision))
        return true;
      continue;
    }
    if (node->id().kind != nodegraph::NodeKind::Item ||
        !belongsToSelection(node))
      continue;
    const auto state = read->state(node);
    const std::string type =
        scalarTextFromValue(valueMember(*state, "type"));
    if (type == "fileChange" || type == "commandExecution" ||
        fieldChanged(*read, node, "type", change.revision))
      return true;
  }
  return false;
}

NodeGraphUiAdapter::ConversationRoute
NodeGraphUiAdapter::conversationRoute(const nodegraph::GraphChanged &change,
                                      const nodegraph::NodeRef &thread) const {
  if (!graph_ || !thread)
    return {};
  auto read = graph_->tryRead();
  return read ? conversationRoute(change, thread, *read)
              : ConversationRoute{true, true, true, {}, {}, {}, 0};
}

NodeGraphUiAdapter::ConversationRoute NodeGraphUiAdapter::conversationRoute(
    const nodegraph::GraphChanged &change, const nodegraph::NodeRef &thread,
    const nodegraph::NodeGraph::ReadAccess &access) const {
  ConversationRoute route;
  route.graphRevision = access.revision();
  if (!thread)
    return route;
  if (change.rescanRequired) {
    route.affected = true;
    route.structural = true;
    route.authorityReplacement = true;
    return route;
  }

  const auto *read = &access;

  const std::string &threadId = thread->id().canonical;
  route.graphRevision = change.revision;
  const auto isHistoryOperationForThread = [&](const nodegraph::NodeRef &node) {
    if (!node || node->id().kind != nodegraph::NodeKind::Operation ||
        !read->contains(node))
      return false;
    const auto state = read->state(node);
    if (!state || !isConversationSnapshotMethod(
                      scalarTextFromValue(valueMember(*state, "method"))))
      return false;
    const nodegraph::Value *payload = valueMember(*state, "requestPayload");
    const auto *object = payload ? payload->asObject() : nullptr;
    if (object &&
        scalarTextFromValue(valueMember(*object, "threadId")) == threadId)
      return true;
    if (!read->live(node) || !read->live(thread))
      return false;
    const std::vector<nodegraph::NodeRef> pendingOperations =
        read->related(thread, nodegraph::RelationKind::PendingOperation);
    return std::ranges::find(pendingOperations, node) !=
           pendingOperations.end();
  };
  const bool historyOperationChanged =
      std::ranges::any_of(change.affected, isHistoryOperationForThread) ||
      std::ranges::any_of(change.removed, isHistoryOperationForThread);
  if (historyOperationChanged && read->live(thread))
    route.historyRequestPending = hasPendingHistoryRequest(*read, thread);
  const bool threadTurnsChanged =
      std::ranges::any_of(change.childListsChanged, [&](const auto &changed) {
        return changed.owner == thread &&
               changed.childKind == nodegraph::NodeKind::Turn;
      });
  const bool turnInsertedIntoThread =
      threadTurnsChanged &&
      std::ranges::any_of(change.affected, [&](const auto &node) {
        return node && node->id().kind == nodegraph::NodeKind::Turn &&
               read->structureChangedRevision(node) == change.revision &&
               read->parent(node) == thread;
      });
  if (threadTurnsChanged && !turnInsertedIntoThread) {
    route.affected = true;
    route.structural = true;
    route.authorityReplacement = true;
    return route;
  }
  const auto belongsToThread = [&](const nodegraph::NodeRef &node) {
    if (!node)
      return false;
    nodegraph::NodeRef ancestor = node;
    while ((ancestor = read->parent(ancestor))) {
      if (ancestor == thread)
        return true;
      if (ancestor->id().kind == nodegraph::NodeKind::Thread)
        return false;
    }
    const auto state = read->state(node);
    if (!state)
      return false;
    constexpr std::array<std::string_view, 2> ThreadFields{
        "protocolThreadId", "threadId"};
    return std::ranges::any_of(
        ThreadFields, [&](std::string_view field) {
          return scalarTextFromValue(valueMember(*state, field)) == threadId;
        });
  };
  const auto addItem = [&](const nodegraph::NodeRef &item) {
    if (!item || route.authorityReplacement ||
        std::ranges::find(route.items, item) != route.items.end())
      return;
    route.items.push_back(item);
  };
  const auto routeNode = [&](const nodegraph::NodeRef &node) {
    if (!node)
      return;
    if (node == thread) {
      if (!read->contains(node)) {
        route = {true, true, true, {}, {}, {}, route.graphRevision};
        return;
      }
      if (fieldChanged(*read, node, "historyHasMore", change.revision)) {
        route.affected = true;
        route.providerHasMore = graphProviderHasMoreHistory(*read->state(node));
      }
      if (fieldChanged(*read, node, "hydrationState", change.revision)) {
        route.affected = true;
        route.structural = true;
        route.authorityReplacement = true;
        route.items.clear();
        return;
      }
      if (read->structureChangedRevision(node) == change.revision) {
        for (const nodegraph::NodeRef &candidate : change.affected) {
          if (!candidate ||
              candidate->id().kind != nodegraph::NodeKind::Turn ||
              !belongsToThread(candidate))
            continue;
          route.affected = true;
          route.structural = true;
          addItem(conversationTurnRoot(*read, candidate));
        }
      }
      return;
    }
    if (node->id().kind == nodegraph::NodeKind::Thread ||
        (node->id().kind != nodegraph::NodeKind::Turn &&
         node->id().kind != nodegraph::NodeKind::Item))
      return;

    if (!read->live(node)) {
      if (node->id().kind == nodegraph::NodeKind::Item &&
          belongsToThread(node)) {
        route.affected = true;
        route.structural = true;
        addItem(node);
      }
      return;
    }

    if (!belongsToThread(node)) {
      if (node->id().kind == nodegraph::NodeKind::Item &&
          read->structureChangedRevision(node) == change.revision &&
          !read->parent(node)) {
        route.affected = true;
        route.structural = true;
        route.authorityReplacement = true;
      }
      return;
    }

    if (node->id().kind == nodegraph::NodeKind::Turn) {
      const bool structureChanged =
          read->structureChangedRevision(node) == change.revision;
      const bool statusChanged =
          read->statusChangedRevision(node) == change.revision;
      if (!structureChanged && !statusChanged)
        return;
      route.affected = true;
      route.structural = route.structural || structureChanged || statusChanged;
      addItem(conversationTurnRoot(*read, node));
      return;
    }
    route.affected = true;
    if (read->structureChangedRevision(node) == change.revision)
      route.structural = true;
    addItem(node);
  };

  for (const nodegraph::NodeRef &node : change.affected)
    routeNode(node);
  for (const nodegraph::NodeRef &node : change.removed) {
    if (!node)
      continue;
    if (node == thread)
      return {true, true, true, {}, {}, {}, route.graphRevision};
    if (node->id().kind == nodegraph::NodeKind::Item &&
        belongsToThread(node)) {
      route.affected = true;
      route.structural = true;
      addItem(node);
    } else if (node->id().kind == nodegraph::NodeKind::Turn &&
               belongsToThread(node)) {
      route.affected = true;
      route.structural = true;
      route.authorityReplacement = true;
      route.items.clear();
    }
  }

  return route;
}

std::optional<ConversationDelta>
NodeGraphUiAdapter::conversationDelta(const nodegraph::NodeRef &thread,
                                      std::span<const nodegraph::NodeRef> items,
                                      bool structural) const {
  if (!graph_ || !thread)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->live(thread) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;
  const auto threadState = read->state(thread);
  if (!threadState)
    return std::nullopt;

  ConversationDelta result;
  result.threadId = thread->id().canonical;
  result.providerHasMore = graphProviderHasMoreHistory(*threadState);
  const std::string threadCwd =
      scalarTextFromValue(valueMember(*threadState, "cwd"));
  const nodegraph::NodeRef activeTurn =
      structural ? conversationActiveTurn(*read, thread) : nodegraph::NodeRef{};

  std::vector<nodegraph::NodeRef> transactionItems;
  transactionItems.reserve(items.size());
  for (const nodegraph::NodeRef &target : items) {
    if (structural && target && read->live(target) &&
        target->id().kind == nodegraph::NodeKind::Turn)
      transactionItems.push_back(conversationTurnRoot(*read, target));
    else
      transactionItems.push_back(target);
  }

  std::unordered_set<const nodegraph::Node *> seen;
  seen.reserve(transactionItems.size());
  std::unordered_set<const nodegraph::Node *> materializedPrompts;
  materializedPrompts.reserve(items.size());
  std::unordered_map<std::string, std::size_t> changedRows;
  changedRows.reserve(items.size());
  for (const nodegraph::NodeRef &item : transactionItems) {
    if (!item || !seen.insert(item.get()).second)
      continue;
    if (!structural) {
      if (!read->live(item) ||
          item->id().kind != nodegraph::NodeKind::Item)
        continue;
      const nodegraph::NodeRef turn = read->parent(item);
      if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
          read->parent(turn) != thread)
        continue;
      const auto state = read->state(item);
      const auto turnState = read->state(turn);
      if (!state || !turnState)
        continue;

      if (!isHiddenMaterializedPrompt(*read, item))
        result.presentations.push_back(conversationCardData(
            *read, item, result.threadId,
            nodegraph::protocolCanonicalId(*turnState, turn), *state,
            threadCwd));
      continue;
    }

    std::optional<ConversationRowChange> row =
        projectRowChange(read, thread, item, threadCwd, activeTurn);
    if (!row) {
      result.removals.push_back(item);
      continue;
    }
    if (const auto prompt = materializedPrompt(*read, item);
        prompt && materializedPrompts.insert(prompt->prompt.get()).second)
      result.materializedPrompts.push_back(
          {row->placement.card.key, prompt->prompt});
    const std::string key = stableKey(row->placement.card.key);
    if (const auto duplicate = changedRows.find(key);
        duplicate != changedRows.end()) {
      result.rows[duplicate->second] = std::move(*row);
    } else {
      changedRows.emplace(key, result.rows.size());
      result.rows.push_back(std::move(*row));
    }
  }

  read.reset();
  if (!structural || result.rows.size() < 2)
    return result;

  std::vector<std::vector<std::size_t>> following(result.rows.size());
  std::vector<std::size_t> predecessors(result.rows.size(), 0);
  const auto relate = [&](std::size_t before, std::size_t after) {
    if (before == after ||
        std::ranges::find(following[before], after) != following[before].end())
      return;
    following[before].push_back(after);
    ++predecessors[after];
  };
  for (std::size_t index = 0; index < result.rows.size(); ++index) {
    if (result.rows[index].previousCardKey) {
      const auto previous =
          changedRows.find(stableKey(*result.rows[index].previousCardKey));
      if (previous != changedRows.end())
        relate(previous->second, index);
    }
    if (result.rows[index].nextCardKey) {
      const auto next =
          changedRows.find(stableKey(*result.rows[index].nextCardKey));
      if (next != changedRows.end())
        relate(index, next->second);
    }
  }

  std::vector<ConversationRowChange> ordered;
  ordered.reserve(result.rows.size());
  std::vector<bool> emitted(result.rows.size(), false);
  while (ordered.size() != result.rows.size()) {
    bool progress = false;
    for (std::size_t index = 0; index < result.rows.size(); ++index) {
      if (emitted[index] || predecessors[index] != 0)
        continue;
      emitted[index] = true;
      progress = true;
      ordered.push_back(std::move(result.rows[index]));
      for (const std::size_t next : following[index])
        --predecessors[next];
    }
    if (!progress)
      return std::nullopt;
  }
  result.rows = std::move(ordered);
  return result;
}

std::optional<ConversationDelta>
NodeGraphUiAdapter::conversationDeltaSince(
    const nodegraph::NodeRef &thread, std::uint64_t afterRevision,
    std::uint64_t *graphRevision) const {
  if (!graph_ || !thread || afterRevision == 0)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->live(thread) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;

  std::vector<nodegraph::NodeRef> changed;
  const std::size_t turnCount = read->childCount(thread);
  changed.reserve(turnCount);
  for (std::size_t turnIndex = 0; turnIndex < turnCount; ++turnIndex) {
    const nodegraph::NodeRef turn = read->childAt(thread, turnIndex);
    if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
      continue;
    const auto turnState = read->state(turn);
    if (!turnState)
      continue;
    if (read->changedRevision(turn) > afterRevision ||
        read->structureChangedRevision(turn) > afterRevision)
      changed.push_back(conversationTurnRoot(*read, turn));
    const std::size_t itemCount = read->childCount(turn);
    for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
      const nodegraph::NodeRef item = read->childAt(turn, itemIndex);
      if (item && item->id().kind == nodegraph::NodeKind::Item &&
          (read->changedRevision(item) > afterRevision ||
           read->structureChangedRevision(item) > afterRevision))
        changed.push_back(item);
    }
  }
  if (graphRevision)
    *graphRevision = read->revision();
  read.reset();
  return conversationDelta(thread, changed, true);
}

std::optional<ConversationRowChange> NodeGraphUiAdapter::projectRowChange(
    std::optional<nodegraph::NodeGraph::ReadAccess> &read,
    const nodegraph::NodeRef &thread, const nodegraph::NodeRef &item,
    std::string_view threadCwd, const nodegraph::NodeRef &activeTurn) {
  if (!read)
    return std::nullopt;
  if (!read->live(thread) || !read->live(item) ||
      thread->id().kind != nodegraph::NodeKind::Thread ||
      item->id().kind != nodegraph::NodeKind::Item)
    return {};

  const nodegraph::NodeRef turn = read->parent(item);
  if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
      read->parent(turn) != thread)
    return {};
  const auto itemState = read->state(item);
  const auto turnState = read->state(turn);
  if (!itemState || !turnState)
    return {};

  const std::string threadId = thread->id().canonical;
  const auto canonicalTurnId = [&](const nodegraph::NodeRef &owner) {
    const auto state = owner ? read->state(owner) : nullptr;
    return state ? nodegraph::protocolCanonicalId(*state, owner)
                 : std::string{};
  };
  const auto projectedKeyAt = [&](const nodegraph::NodeRef &owner,
                                  std::size_t index) -> std::optional<CardKey> {
    const nodegraph::NodeRef candidate = read->childAt(owner, index);
    return conversationCardKey(
        *read, candidate, threadId, canonicalTurnId(owner),
        isHiddenMaterializedPrompt(*read, candidate));
  };

  const std::size_t itemCount = read->childCount(turn);
  const std::optional<std::size_t> itemIndex = read->childIndex(item);
  if (!itemIndex || *itemIndex >= itemCount)
    return {};
  const std::optional<CardKey> itemKey = projectedKeyAt(turn, *itemIndex);
  if (!itemKey)
    return {};

  std::optional<CardKey> previous;
  std::optional<CardKey> next;
  for (std::size_t offset = *itemIndex; offset > 0 && !previous; --offset)
    previous = projectedKeyAt(turn, offset - 1);
  for (std::size_t index = *itemIndex + 1; index < itemCount && !next; ++index)
    next = projectedKeyAt(turn, index);

  const std::size_t turnCount = read->childCount(thread);
  const std::optional<std::size_t> turnIndex = read->childIndex(turn);
  if (!turnIndex || *turnIndex >= turnCount)
    return {};
  for (std::size_t offset = *turnIndex; offset > 0 && !previous; --offset) {
    const nodegraph::NodeRef owner = read->childAt(thread, offset - 1);
    if (!owner || owner->id().kind != nodegraph::NodeKind::Turn)
      continue;
    for (std::size_t child = read->childCount(owner); child > 0 && !previous;
         --child)
      previous = projectedKeyAt(owner, child - 1);
  }
  for (std::size_t ownerIndex = *turnIndex + 1; ownerIndex < turnCount && !next;
       ++ownerIndex) {
    const nodegraph::NodeRef owner = read->childAt(thread, ownerIndex);
    if (!owner || owner->id().kind != nodegraph::NodeKind::Turn)
      continue;
    for (std::size_t child = 0; child < read->childCount(owner) && !next;
         ++child)
      next = projectedKeyAt(owner, child);
  }

  const nodegraph::NodeRef root = conversationTurnRoot(*read, turn);
  const std::optional<std::size_t> rootIndex =
      root && read->parent(root) == turn ? read->childIndex(root)
                                         : std::nullopt;
  const std::string turnId = canonicalTurnId(turn);

  ConversationRowChange result;
  result.placement.card = conversationCardData(*read, item, threadId, turnId,
                                               *itemState, threadCwd);
  result.placement.card.key = *itemKey;
  result.placement.sectionKey = sectionComponent("turn:", threadId, turnId);
  result.placement.turnRoot = root == item;
  result.placement.nested = isNestedTurnCard(rootIndex, *itemIndex);
  result.placement.activeTurn = activeTurn == turn;
  result.previousCardKey = std::move(previous);
  result.nextCardKey = std::move(next);
  return result;
}

std::optional<ConversationSnapshot>
NodeGraphUiAdapter::conversation(const nodegraph::NodeRef &thread,
                                 std::uint64_t *graphRevision) const {
  if (!graph_ || !thread)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->live(thread) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;

  const auto threadState = read->state(thread);
  if (!threadState)
    return std::nullopt;
  if (graphRevision)
    *graphRevision = read->revision();
  const std::string threadCwd =
      scalarTextFromValue(valueMember(*threadState, "cwd"));
  const nodegraph::NodeRef activeTurn = conversationActiveTurn(*read, thread);

  ConversationSnapshot result;
  result.threadId = thread->id().canonical;
  result.hasMore = graphProviderHasMoreHistory(*threadState);

  if (activeTurn) {
    const auto state = read->state(activeTurn);
    if (state)
      result.activeTurnId = nodegraph::protocolCanonicalId(*state, activeTurn);
  }

  std::unordered_set<const nodegraph::Node *> materializedPrompts;
  const std::size_t turnCount = read->childCount(thread);
  result.sections.reserve(turnCount);
  for (std::size_t turnIndex = 0; turnIndex < turnCount; ++turnIndex) {
    const nodegraph::NodeRef turn = read->childAt(thread, turnIndex);
    if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
      continue;
    const auto turnState = read->state(turn);
    if (!turnState)
      continue;
    const std::string turnId =
        nodegraph::protocolCanonicalId(*turnState, turn);
    const nodegraph::NodeRef root = conversationTurnRoot(*read, turn);
    TurnSection section;
    section.key = sectionComponent("turn:", result.threadId, turnId);
    section.turnId = turnId;
    const std::size_t childCount = read->childCount(turn);
    section.cards.reserve(childCount);
    for (std::size_t itemIndex = 0; itemIndex < childCount; ++itemIndex) {
      const nodegraph::NodeRef item = read->childAt(turn, itemIndex);
      if (!item || item->id().kind != nodegraph::NodeKind::Item ||
          !read->live(item))
        continue;
      const auto itemState = read->state(item);
      if (!itemState ||
          (scalarTextFromValue(valueMember(*itemState, "type")) ==
               "localPrompt" &&
           isHiddenMaterializedPrompt(*read, item)))
        continue;
      VisibleCardData card = conversationCardData(*read, item, result.threadId,
                                                  turnId, *itemState,
                                                  threadCwd);
      if (const auto prompt = materializedPrompt(*read, item);
          prompt && materializedPrompts.insert(prompt->prompt.get()).second)
        result.materializedPrompts.push_back({card.key, prompt->prompt});
      if (item == root)
        section.rootCardKey = card.key;
      section.cards.push_back(std::move(card));
    }

    if (!section.cards.empty())
      result.sections.push_back(std::move(section));
  }

  return result;
}

} // namespace codexui::codex::ui
