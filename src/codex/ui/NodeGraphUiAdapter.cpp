// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/NodeGraphUiAdapter.h"

#include "codex/UiStatus.h"
#include "codex/nodegraph/ProtocolUpdater.h"

#include <algorithm>
#include <cctype>
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

const nodegraph::Value *graphField(const nodegraph::NodeState &state,
                                   std::string_view name) {
  const auto found = state.fields.find(name);
  return found == state.fields.end() ? nullptr : &found->second;
}

const nodegraph::Value *graphMember(const nodegraph::Value::Object &object,
                                    std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

std::string graphString(const nodegraph::Value *value) {
  if (!value)
    return {};
  if (const auto *text = value->asString())
    return *text;
  if (const auto *number = value->asInt64())
    return std::to_string(*number);
  if (const auto *number = value->asUInt64())
    return std::to_string(*number);
  return {};
}

bool graphBool(const nodegraph::Value *value) {
  if (!value)
    return false;
  if (const auto *boolean = value->asBool())
    return *boolean;
  return false;
}

std::optional<std::size_t> graphSize(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const auto *number = value->asUInt64();
      number && *number <= std::numeric_limits<std::size_t>::max())
    return static_cast<std::size_t>(*number);
  if (const auto *number = value->asInt64(); number && *number >= 0)
    return static_cast<std::size_t>(*number);
  return std::nullopt;
}

bool graphProviderHasMoreHistory(const nodegraph::NodeState &state) {
  if (graphBool(graphField(state, "historyHasMore")))
    return true;
  if (!graphString(graphField(state, "historyNextCursor")).empty())
    return true;
  const nodegraph::Value *history = graphField(state, "history");
  const auto *object = history ? history->asObject() : nullptr;
  return object && (graphBool(graphMember(*object, "hasMore")) ||
                    !graphString(graphMember(*object, "nextCursor")).empty());
}

std::string graphStatus(const nodegraph::NodeState &state) {
  if (const nodegraph::Value *value = graphField(state, "status")) {
    if (const auto *object = value->asObject())
      return graphString(graphMember(*object, "type"));
    if (std::string status = graphString(value); !status.empty())
      return status;
  }
  switch (state.status) {
  case nodegraph::NodeStatus::Pending:
    return "pending";
  case nodegraph::NodeStatus::Running:
    return "inProgress";
  case nodegraph::NodeStatus::Completed:
    return "completed";
  case nodegraph::NodeStatus::Failed:
    return "failed";
  case nodegraph::NodeStatus::Interrupted:
    return "interrupted";
  case nodegraph::NodeStatus::NotLoaded:
    return "notLoaded";
  case nodegraph::NodeStatus::Connected:
    return "connected";
  case nodegraph::NodeStatus::Disconnected:
    return "disconnected";
  case nodegraph::NodeStatus::Unknown:
    return {};
  }
  return {};
}

bool graphTurnIsActive(const nodegraph::NodeState &state) {
  if (state.status == nodegraph::NodeStatus::Running)
    return true;
  // Once Send has been admitted, its provisional local turn is the user's
  // active Turn/You surface even before the app-server returns the canonical
  // turn id. Do not extend this optimistic presentation to provider-owned
  // pending history.
  return state.status == nodegraph::NodeStatus::Pending &&
         graphBool(graphField(state, "local"));
}

std::optional<std::int64_t> graphInteger(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const auto *number = value->asInt64())
    return *number;
  if (const auto *number = value->asUInt64();
      number && *number <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
    return static_cast<std::int64_t>(*number);
  return std::nullopt;
}

std::vector<std::string> graphStrings(const nodegraph::Value *value) {
  std::vector<std::string> result;
  const auto *array = value ? value->asArray() : nullptr;
  if (!array)
    return result;
  result.reserve(array->size());
  for (const nodegraph::Value &entry : *array)
    if (const auto *text = entry.asString())
      result.push_back(*text);
  return result;
}

std::uint64_t graphOmittedTextBytes(const nodegraph::NodeState &state,
                                    std::string_view field) {
  const nodegraph::Value *retention = graphField(state, "textRetention");
  const auto *retentionObject = retention ? retention->asObject() : nullptr;
  const nodegraph::Value *entry =
      retentionObject ? graphMember(*retentionObject, field) : nullptr;
  const auto *entryObject = entry ? entry->asObject() : nullptr;
  const nodegraph::Value *discarded =
      entryObject ? graphMember(*entryObject, "discardedBytes") : nullptr;
  if (const auto *number = discarded ? discarded->asUInt64() : nullptr)
    return *number;
  if (const auto *number = discarded ? discarded->asInt64() : nullptr;
      number && *number >= 0)
    return static_cast<std::uint64_t>(*number);
  return 0;
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
  if (const auto *content = graphField(state, "content");
      content && content->asArray()) {
    for (const nodegraph::Value &entry : *content->asArray()) {
      const auto *object = entry.asObject();
      const std::string text =
          object ? graphString(graphMember(*object, "text")) : std::string{};
      if (text.empty())
        continue;
      if (!result.empty())
        result.push_back('\n');
      result += text;
    }
  }
  return result.empty() ? graphString(graphField(state, "text")) : result;
}

std::vector<std::string> graphImagePaths(const nodegraph::NodeState &state) {
  std::vector<std::string> result;
  const auto *content = graphField(state, "content");
  const auto *array = content ? content->asArray() : nullptr;
  if (!array)
    return result;
  for (const nodegraph::Value &entry : *array) {
    const auto *object = entry.asObject();
    if (!object || graphString(graphMember(*object, "type")) != "localImage")
      continue;
    if (std::string path = graphString(graphMember(*object, "path"));
        !path.empty())
      result.push_back(std::move(path));
  }
  return result;
}

std::vector<std::string>
graphLocalPromptImagePaths(const nodegraph::NodeState &state) {
  std::vector<std::string> result;
  const nodegraph::Value *attachments = graphField(state, "attachments");
  const auto *array = attachments ? attachments->asArray() : nullptr;
  if (!array)
    return result;
  for (const nodegraph::Value &entry : *array) {
    const auto *object = entry.asObject();
    if (!object ||
        !graphString(graphMember(*object, "mimeType")).starts_with("image/"))
      continue;
    if (std::string path = graphString(graphMember(*object, "path"));
        !path.empty())
      result.emplace_back(std::move(path));
  }
  return result;
}

std::string graphJoinedText(const nodegraph::Value *value) {
  const auto *array = value ? value->asArray() : nullptr;
  if (!array)
    return graphString(value);
  std::string result;
  for (const nodegraph::Value &entry : *array) {
    std::string text = graphString(&entry);
    if (text.empty())
      if (const auto *object = entry.asObject())
        text = graphString(graphMember(*object, "text"));
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

constexpr std::size_t MaximumGraphActivityDetailCharacters = 4000;

class GraphDetailBuilder final {
public:
  void append(std::string_view text) {
    if (text.empty() || truncated_)
      return;
    const std::size_t remaining =
        MaximumGraphActivityDetailCharacters - value_.size();
    if (text.size() <= remaining) {
      value_.append(text);
      return;
    }
    value_.append(text.substr(0, remaining));
    truncated_ = true;
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

bool graphHasStructuredPlan(const nodegraph::NodeState &state) {
  if (!graphString(graphField(state, "planExplanation")).empty())
    return true;
  const nodegraph::Value *plan = graphField(state, "plan");
  if (!plan)
    return false;
  if (const auto *steps = plan->asArray())
    return !steps->empty();
  const auto *object = plan->asObject();
  if (!object)
    return false;
  if (!graphString(graphMember(*object, "explanation")).empty())
    return true;
  const nodegraph::Value *steps = graphMember(*object, "steps");
  return steps && steps->asArray() && !steps->asArray()->empty();
}

PlanData graphPlanData(const nodegraph::NodeState &state) {
  PlanData result;
  result.explanation = graphString(graphField(state, "planExplanation"));
  const nodegraph::Value *plan = graphField(state, "plan");
  const nodegraph::Value::Array *steps = plan ? plan->asArray() : nullptr;
  if (const auto *object = plan ? plan->asObject() : nullptr) {
    if (result.explanation.empty())
      result.explanation = graphString(graphMember(*object, "explanation"));
    const nodegraph::Value *nested = graphMember(*object, "steps");
    steps = nested ? nested->asArray() : nullptr;
  }
  if (!steps)
    return result;
  result.steps.reserve(steps->size());
  for (const nodegraph::Value &entry : *steps) {
    const auto *object = entry.asObject();
    if (!object)
      continue;
    std::string text = graphString(graphMember(*object, "step"));
    if (text.empty())
      text = graphString(graphMember(*object, "text"));
    if (!text.empty())
      result.steps.push_back(
          {std::move(text), graphString(graphMember(*object, "status"))});
  }
  return result;
}

CardKind graphCardKind(const nodegraph::NodeState &state) {
  const std::string type = graphString(graphField(state, "type"));
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
  if ((type == "plan" && !graphMessageText(state).empty()) ||
      graphHasStructuredPlan(state))
    return CardKind::Plan;
  return CardKind::GenericActivity;
}

VisibleCardData graphCardData(const nodegraph::NodeRef &item,
                              std::string threadId, std::string turnId,
                              const nodegraph::NodeState &state,
                              std::string_view threadCwd = {}) {
  const std::string itemId =
      item ? nodegraph::protocolCanonicalId(state, item) : std::string{};
  const std::string type = graphString(graphField(state, "type"));
  GenericActivityData generic;
  generic.type = type;
  generic.status = graphStatus(state);
  generic.displayDetail = graphDisplayDetail(state);
  VisibleCardData result{AuthoritativeItemKey{threadId, turnId, itemId},
                         CardKind::GenericActivity,
                         std::move(threadId),
                         std::move(turnId),
                         itemId,
                         std::move(generic)};

  result.kind = graphCardKind(state);
  switch (result.kind) {
  case CardKind::UserMessage: {
    result.payload =
        UserMessageData{graphMessageText(state), graphImagePaths(state)};
    if (const auto submission =
            graphInteger(graphField(state, "localSubmissionId"));
        submission && *submission >= 0)
      result.key = LocalPromptKey{static_cast<std::uint64_t>(*submission)};
    break;
  }
  case CardKind::AgentMessage:
    result.payload = AgentMessageData{
        withTruncationNotice(graphMessageText(state),
                             graphOmittedTextBytes(state, "text"),
                             "Codex response", true),
        graphString(graphField(state, "phase")) == "final_answer"};
    break;
  case CardKind::CommandExecution: {
    std::string outputField = "aggregatedOutput";
    std::string output = graphString(graphField(state, "aggregatedOutput"));
    if (output.empty()) {
      outputField = "output";
      output = graphString(graphField(state, "output"));
    }
    output = withTruncationNotice(std::move(output),
                                  graphOmittedTextBytes(state, outputField),
                                  "command output", false);
    if (!terminalOutputHasVisibleText(output))
      output.clear();
    const auto exit = graphInteger(graphField(state, "exitCode"));
    auto duration = graphInteger(graphField(state, "durationMs"));
    if (!duration)
      duration = graphInteger(graphField(state, "duration_ms"));
    result.payload = CommandExecutionData{
        graphString(graphField(state, "command")),
        std::move(output),
        graphStatus(state),
        graphString(graphField(state, "cwd")),
        exit ? std::optional<int>(static_cast<int>(*exit)) : std::nullopt,
        duration};
    break;
  }
  case CardKind::AgentActivity:
    result.payload =
        AgentActivityData{graphString(graphField(state, "tool")),
                          graphStatus(state),
                          graphString(graphField(state, "kind")),
                          graphString(graphField(state, "prompt")),
                          graphString(graphField(state, "resultText")),
                          graphStrings(graphField(state, "receiverThreadIds")),
                          graphString(graphField(state, "model")),
                          graphString(graphField(state, "reasoningEffort")),
                          graphString(graphField(state, "agentThreadId")),
                          graphString(graphField(state, "agentPath")),
                          graphString(graphField(state, "senderThreadId"))};
    break;
  case CardKind::Reasoning:
    result.payload = ReasoningData{withTruncationNotice(
        graphJoinedText(graphField(state, "summary")),
        graphOmittedTextBytes(state, "summary"), "reasoning", true)};
    break;
  case CardKind::FileChanges: {
    std::string cwd = graphString(graphField(state, "cwd"));
    if (cwd.empty())
      cwd = threadCwd;
    FileChangesData projected{graphStatus(state), {}, std::move(cwd)};
    const auto *changes = graphField(state, "changes");
    const auto *array = changes ? changes->asArray() : nullptr;
    if (array) {
      projected.changes.reserve(array->size());
      for (const nodegraph::Value &value : *array) {
        const auto *change = value.asObject();
        if (!change)
          continue;
        FileChangeData entry{graphString(graphMember(*change, "path")),
                             graphString(graphMember(*change, "kind")),
                             std::nullopt, std::nullopt};
        if (std::string diff = graphString(graphMember(*change, "diff"));
            !diff.empty()) {
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
    std::string path = graphString(graphField(state, "path"));
    if (path.empty())
      path = graphString(graphField(state, "savedPath"));
    if (path.empty())
      path = graphString(graphField(state, "saved_path"));
    std::string prompt = graphString(graphField(state, "revisedPrompt"));
    if (prompt.empty())
      prompt = graphString(graphField(state, "revised_prompt"));
    result.payload = ImageGenerationData{
        std::move(path), type == "imageView" ? "completed" : graphStatus(state),
        std::move(prompt)};
    break;
  }
  case CardKind::Plan:
    if (graphHasStructuredPlan(state))
      result.payload = graphPlanData(state);
    else
      result.payload =
          PlanData{{},
                   {},
                   withTruncationNotice(graphMessageText(state),
                                        graphOmittedTextBytes(state, "text"),
                                        "plan text", true)};
    if (item && item->id().kind == nodegraph::NodeKind::Turn) {
      result.key = TurnPlanKey{result.threadId, result.turnId};
      result.itemId.clear();
    }
    break;
  case CardKind::GenericActivity:
    break;
  case CardKind::LocalPrompt: {
    const std::string dispatch =
        graphString(graphField(state, "dispatchState"));
    PromptState promptState = PromptState::Queued;
    if (dispatch == "dispatching" || dispatch == "inFlight")
      promptState = PromptState::InFlight;
    else if (dispatch == "awaitingMaterialization")
      promptState = PromptState::Accepted;
    else if (dispatch == "failed" || dispatch == "uncertain")
      promptState = PromptState::Failed;
    const std::int64_t rawId =
        graphInteger(graphField(state, "submissionId")).value_or(0);
    const std::uint64_t submissionId =
        rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
    result.key = LocalPromptKey{submissionId};
    result.itemId.clear();
    result.payload = LocalPromptData{
        submissionId,
        graphString(graphField(state, "text")),
        promptState,
        graphField(state, "showPendingAnimation")
            ? graphBool(graphField(state, "showPendingAnimation"))
            : false,
        graphString(graphField(state, "error")),
        graphLocalPromptImagePaths(state),
        graphInteger(graphField(state, "admittedAtMs")),
        graphBool(graphField(state, "requiresExplicitRecovery"))};
    break;
  }
  }
  switch (result.kind) {
  case CardKind::CommandExecution:
  case CardKind::AgentActivity:
  case CardKind::Reasoning:
  case CardKind::FileChanges:
  case CardKind::ImageGeneration:
  case CardKind::Plan:
  case CardKind::GenericActivity:
    result.activeWork = state.status == nodegraph::NodeStatus::Pending ||
                        state.status == nodegraph::NodeStatus::Running;
    break;
  case CardKind::UserMessage:
  case CardKind::AgentMessage:
  case CardKind::LocalPrompt:
    break;
  }
  result.target = item;
  return result;
}

bool terminalStatus(std::string_view status) {
  const StatusKind kind = classifyStatus(status).kind;
  return kind == StatusKind::Completed || kind == StatusKind::Failed ||
         kind == StatusKind::Interrupted;
}

void updateAgentStatus(std::string &current, std::string candidate) {
  if (candidate.empty() ||
      (terminalStatus(current) && isActiveStatus(candidate)))
    return;
  current = std::move(candidate);
}

bool spawnAgentTool(std::string_view tool) {
  return tool == "spawn_agent" || tool == "spawnAgent" ||
         tool == "spawn_agents_on_csv" || tool == "spawnAgentsOnCsv";
}

std::string agentActivityStatus(const nodegraph::NodeState &state) {
  const std::string kind = graphString(graphField(state, "kind"));
  if (kind == "completed" || kind == "interrupted" || kind == "failed")
    return kind;
  if (kind == "interacted")
    return {};
  if (std::string status = graphString(graphField(state, "status"));
      !status.empty())
    return status;
  const std::string published = graphStatus(state);
  if (terminalStatus(published))
    return published;
  if (kind == "started" || kind == "progress")
    return "inProgress";
  return published;
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

std::string requestKind(std::string_view method) {
  if (method == "item/commandExecution/requestApproval")
    return "command-approval";
  if (method == "item/fileChange/requestApproval")
    return "file-change-approval";
  if (method == "item/tool/requestUserInput")
    return "user-input";
  if (method == "mcpServer/elicitation/request")
    return "mcp-elicitation";
  if (method == "item/permissions/requestApproval")
    return "permissions-approval";
  if (method == "item/tool/call")
    return "dynamic-tool-call";
  if (method == "account/chatgptAuthTokens/refresh")
    return "authentication-refresh";
  if (method == "attestation/generate")
    return "attestation";
  if (method == "applyPatchApproval")
    return "legacy-patch-approval";
  if (method == "execCommandApproval")
    return "legacy-command-approval";
  return "unsupported";
}

void appendUniqueBounded(std::vector<std::string> &values, std::string value,
                         std::size_t maximum) {
  if (value.empty() ||
      std::ranges::find(values, value) != values.end())
    return;
  if (values.size() == maximum)
    values.erase(values.begin());
  values.emplace_back(std::move(value));
}

std::string_view nodeKindName(nodegraph::NodeKind kind) {
  using nodegraph::NodeKind;
  switch (kind) {
  case NodeKind::Runtime: return "Runtime";
  case NodeKind::Connection: return "Connection";
  case NodeKind::Thread: return "Thread";
  case NodeKind::Turn: return "Turn";
  case NodeKind::Item: return "Item";
  case NodeKind::Interaction: return "Interaction";
  case NodeKind::Operation: return "Operation";
  case NodeKind::Catalog: return "Catalog";
  case NodeKind::CatalogEntry: return "CatalogEntry";
  case NodeKind::Account: return "Account";
  case NodeKind::Configuration: return "Configuration";
  case NodeKind::PermissionProfile: return "PermissionProfile";
  case NodeKind::Skill: return "Skill";
  case NodeKind::Hook: return "Hook";
  case NodeKind::Plugin: return "Plugin";
  case NodeKind::App: return "App";
  case NodeKind::McpServer: return "McpServer";
  case NodeKind::Project: return "Project";
  case NodeKind::ThreadSection: return "ThreadSection";
  case NodeKind::Process: return "Process";
  case NodeKind::RealtimeSession: return "RealtimeSession";
  case NodeKind::FilesystemWatch: return "FilesystemWatch";
  case NodeKind::ExternalAgentImport: return "ExternalAgentImport";
  case NodeKind::FuzzyFileSearchSession: return "FuzzyFileSearchSession";
  case NodeKind::LoginAttempt: return "LoginAttempt";
  case NodeKind::Notice: return "Notice";
  case NodeKind::UnknownProtocol: return "UnknownProtocol";
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
    result[key] = sensitiveStateField(key)
                      ? nlohmann::json("<redacted>")
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
  if (!read || !read->contains(thread) || read->removed(thread))
    return std::nullopt;
  const auto state = read->state(thread);
  if (!state)
    return std::nullopt;
  const auto timestamp = [&state](std::string_view field) {
    return graphInteger(graphField(*state, field));
  };
  ThreadListRow row;
  row.id = thread->id().canonical;
  row.title = graphString(graphField(*state, "name"));
  if (row.title.empty())
    row.title = graphString(graphField(*state, "preview"));
  if (row.title.empty())
    row.title = row.id.substr(0, std::min<std::size_t>(12, row.id.size()));
  row.cwd = graphString(graphField(*state, "cwd"));
  row.status = graphStatus(*state);
  row.createdAt = timestamp("createdAt");
  row.updatedAt = timestamp("updatedAt");
  row.recencyAt = timestamp("recencyAt");
  for (const std::string_view field : {
           std::string_view("lastActivityAt"), std::string_view("updatedAt"),
           std::string_view("recencyAt"),
           std::string_view("localActivityAt"),
           std::string_view("localPromptActivityAt")}) {
    const std::optional<std::int64_t> candidate = timestamp(field);
    if (candidate && (!row.lastActivityAt || *candidate > *row.lastActivityAt))
      row.lastActivityAt = candidate;
  }
  row.pending =
      graphSize(graphField(*state, "pendingInteractionCount")).value_or(0);
  row.archived = graphBool(graphField(*state, "archived"));
  return row;
}

std::optional<ThreadListSnapshot>
NodeGraphUiAdapter::threads(const nodegraph::NodeRef &selectedThread) const {
  if (!graph_)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;

  ThreadListSnapshot result;
  if (selectedThread && read->contains(selectedThread) &&
      !read->removed(selectedThread))
    result.selectedThreadId = selectedThread->id().canonical;

  const nodegraph::NodeRef connection =
      read->find({nodegraph::NodeKind::Connection, "connection"});
  if (connection) {
    const auto state = read->state(connection);
    if (state) {
      const std::string provider =
          graphString(graphField(*state, "providerState"));
      const std::string transport =
          graphString(graphField(*state, "transportState"));
      const bool connected =
          state->status == nodegraph::NodeStatus::Connected ||
          transport == "connected";
      result.providerReady = connected && provider == "ready";
      result.canControl =
          result.providerReady &&
          graphString(graphField(*state, "role")) == "controller";
    }
  }

  std::vector<nodegraph::NodeRef> allThreads;
  std::unordered_set<const nodegraph::Node *> childThreads;
  for (const nodegraph::NodeRef &node : read->orderedNodes()) {
    if (!node || node->id().kind != nodegraph::NodeKind::Thread ||
        read->removed(node))
      continue;
    allThreads.push_back(node);
    for (const nodegraph::RelationKind kind :
         {nodegraph::RelationKind::StructuralChildThread,
          nodegraph::RelationKind::AgentChildThread,
          nodegraph::RelationKind::ForkChildThread})
      for (const nodegraph::NodeRef &child : read->related(node, kind))
        if (child && read->contains(child) && !read->removed(child) &&
            child->id().kind == nodegraph::NodeKind::Thread)
          childThreads.insert(child.get());
  }

  const auto timestamp = [](const nodegraph::NodeState &state,
                            std::string_view field) {
    return graphInteger(graphField(state, field));
  };
  std::unordered_set<const nodegraph::Node *> emitted;
  const auto buildRow = [&](const auto &self,
                            const nodegraph::NodeRef &node) -> ThreadListRow {
    ThreadListRow row;
    if (!node || !read->contains(node) || read->removed(node) ||
        !emitted.insert(node.get()).second)
      return row;
    const auto state = read->state(node);
    if (!state)
      return row;
    row.id = node->id().canonical;
    row.title = graphString(graphField(*state, "name"));
    if (row.title.empty())
      row.title = graphString(graphField(*state, "preview"));
    if (row.title.empty())
      row.title = row.id.substr(0, std::min<std::size_t>(12, row.id.size()));
    row.cwd = graphString(graphField(*state, "cwd"));
    row.status = graphStatus(*state);
    row.createdAt = timestamp(*state, "createdAt");
    row.updatedAt = timestamp(*state, "updatedAt");
    row.recencyAt = timestamp(*state, "recencyAt");
    for (const std::string_view field : {
             std::string_view("lastActivityAt"),
             std::string_view("updatedAt"), std::string_view("recencyAt"),
             std::string_view("localActivityAt"),
             std::string_view("localPromptActivityAt")}) {
      const std::optional<std::int64_t> candidate = timestamp(*state, field);
      if (candidate &&
          (!row.lastActivityAt || *candidate > *row.lastActivityAt))
        row.lastActivityAt = candidate;
    }
    row.pending = graphSize(graphField(*state, "pendingInteractionCount"))
                      .value_or(0);
    row.archived = graphBool(graphField(*state, "archived"));
    std::unordered_set<const nodegraph::Node *> localChildren;
    for (const nodegraph::RelationKind kind :
         {nodegraph::RelationKind::StructuralChildThread,
          nodegraph::RelationKind::AgentChildThread,
          nodegraph::RelationKind::ForkChildThread}) {
      for (const nodegraph::NodeRef &child : read->related(node, kind)) {
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
NodeGraphUiAdapter::conversationInfo(
    const nodegraph::NodeRef &thread) const {
  if (!graph_ || !thread)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->contains(thread) || read->removed(thread) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;
  const auto state = read->state(thread);
  if (!state)
    return std::nullopt;

  ConversationInfo result;
  if (const auto count =
          graphSize(graphField(*state, "historyLoadedItemCount"))) {
    result.authoritativeItemCount = *count;
  } else {
    const std::size_t turnCount = read->childCount(thread);
    for (std::size_t turnIndex = 0; turnIndex < turnCount; ++turnIndex) {
      const nodegraph::NodeRef turn = read->childAt(thread, turnIndex);
      if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
        continue;
      const std::size_t itemCount = read->childCount(turn);
      for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
        const nodegraph::NodeRef item = read->childAt(turn, itemIndex);
        if (!item || item->id().kind != nodegraph::NodeKind::Item)
          continue;
        const auto itemState = read->state(item);
        if (itemState &&
            graphString(graphField(*itemState, "type")) != "localPrompt")
          ++result.authoritativeItemCount;
      }
    }
  }

  const std::string hydration =
      graphString(graphField(*state, "hydrationState"));
  const bool local = graphBool(graphField(*state, "local"));
  const bool recoveryOnly = graphBool(graphField(*state, "recoveryOnly"));
  result.readyForDisplay = hydration == "ready" || local || recoveryOnly;
  result.hydrationFailed = hydration == "failed";
  result.providerHasMore = graphProviderHasMoreHistory(*state);
  return result;
}

std::optional<InspectorSnapshot>
NodeGraphUiAdapter::inspector(
    const nodegraph::NodeRef &selectedThread,
    InspectorProjection projection) const {
  if (!graph_)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read)
    return std::nullopt;

  nodegraph::NodeRef thread;
  if (selectedThread &&
      selectedThread->id().kind == nodegraph::NodeKind::Thread &&
      read->contains(selectedThread) && !read->removed(selectedThread))
    thread = selectedThread;

  InspectorSnapshot result;
  const bool wantPlan = projection == InspectorProjection::All ||
                        projection == InspectorProjection::Plan;
  const bool wantAgents = projection == InspectorProjection::All ||
                          projection == InspectorProjection::Agents;
  const bool wantChanges = projection == InspectorProjection::All ||
                           projection == InspectorProjection::Changes;
  const bool wantRequests = projection == InspectorProjection::All ||
                            projection == InspectorProjection::Requests;
  const bool wantState = projection == InspectorProjection::All ||
                         projection == InspectorProjection::State;
  result.plan.threadId = thread ? thread->id().canonical : std::string{};
  result.plan.threadPresent = static_cast<bool>(thread);
  result.agents.threadId = result.plan.threadId;
  result.agents.threadPresent = result.plan.threadPresent;
  result.changes.threadId = result.plan.threadId;

  const nodegraph::NodeRef connection =
      wantRequests || wantState
          ? read->find({nodegraph::NodeKind::Connection, "connection"})
          : nodegraph::NodeRef{};
  bool canControl = false;
  std::uint64_t generation = 0;
  if (connection) {
    const auto state = read->state(connection);
    const std::string transport =
        graphString(graphField(*state, "transportState"));
    const std::string provider =
        graphString(graphField(*state, "providerState"));
    canControl =
        (transport == "connected" ||
         state->status == nodegraph::NodeStatus::Connected) &&
        provider == "ready" &&
        graphString(graphField(*state, "role")) == "controller";
    generation = graphInteger(graphField(*state, "connectionGeneration"))
                     .value_or(graphInteger(
                                   graphField(*state, "providerGeneration"))
                                   .value_or(0));
  }

  std::map<std::string, std::size_t, std::less<>> kindCounts;
  nlohmann::json domains = nlohmann::json::array();
  std::size_t omittedDomains = 0;
  if (wantState)
    for (const nodegraph::NodeRef &node : read->orderedNodes()) {
    if (!node || read->removed(node))
      continue;
    ++kindCounts[std::string(nodeKindName(node->id().kind))];
    if (node->id().kind == nodegraph::NodeKind::Thread)
      ++result.state.threadCount;
    else if (node->id().kind == nodegraph::NodeKind::UnknownProtocol)
      ++result.state.telemetryCount;
    else if (node->id().kind == nodegraph::NodeKind::Catalog &&
             node->id().canonical == "model") {
      const auto state = read->state(node);
      const nodegraph::Value *data = graphField(*state, "data");
      if (const auto *models = data ? data->asArray() : nullptr)
        result.state.modelCount = models->size();
    }

    const bool domain = node->id().kind != nodegraph::NodeKind::Thread &&
                        node->id().kind != nodegraph::NodeKind::Turn &&
                        node->id().kind != nodegraph::NodeKind::Item &&
                        node->id().kind != nodegraph::NodeKind::Interaction &&
                        node->id().kind != nodegraph::NodeKind::Operation &&
                        node->id().kind !=
                            nodegraph::NodeKind::UnknownProtocol;
    if (!domain)
      continue;
    if (domains.size() >= 96) {
      ++omittedDomains;
      continue;
    }
    const auto state = read->state(node);
    domains.push_back({{"kind", nodeKindName(node->id().kind)},
                       {"id", node->id().canonical},
                       {"status", graphStatus(*state)},
                       {"changedRevision", read->changedRevision(node)},
                       {"fields", safeStateObject(state->fields)}});
    }

  const nodegraph::NodeRef runtime =
      read->find({nodegraph::NodeKind::Runtime, "runtime"});
  nlohmann::json pendingMetadata = nlohmann::json::array();
  if (runtime && (wantRequests || wantState)) {
    for (const nodegraph::NodeRef &interaction :
         read->related(runtime,
                       nodegraph::RelationKind::PendingInteraction)) {
      if (!interaction || read->removed(interaction) ||
          interaction->id().kind != nodegraph::NodeKind::Interaction)
        continue;
      const auto state = read->state(interaction);
      if (state->status != nodegraph::NodeStatus::Pending &&
          state->status != nodegraph::NodeStatus::Failed)
        continue;

      InspectorRequestRow row;
      row.id = interaction->id().canonical;
      const std::string method = graphString(graphField(*state, "method"));
      row.kind = requestKind(method);
      row.generation = generation;
      const bool recoveryOnly = graphBool(graphField(*state, "recoveryOnly"));
      row.actionable = canControl && !recoveryOnly;
      const nodegraph::Value *payloadValue = graphField(*state, "payload");
      const auto *payload = payloadValue ? payloadValue->asObject() : nullptr;
      if (payload) {
        row.command = graphString(graphMember(*payload, "command"));
        row.reason = graphString(graphMember(*payload, "reason"));
        row.message = graphString(graphMember(*payload, "message"));
        row.threadContext = graphString(graphMember(*payload, "threadId"));
        if (const nodegraph::Value *questions =
                graphMember(*payload, "questions");
            questions && questions->asArray())
          row.questionCount = questions->asArray()->size();
      }
      if (row.message.empty())
        row.message = graphString(graphField(*state, "error"));
      if (row.threadContext.empty()) {
        for (nodegraph::NodeRef target : read->related(
                 interaction, nodegraph::RelationKind::InteractionTarget)) {
          for (std::size_t depth = 0; target && depth < 16;
               ++depth, target = read->parent(target)) {
            if (target->id().kind != nodegraph::NodeKind::Thread)
              continue;
            row.threadContext = target->id().canonical;
            const auto targetState = read->state(target);
            if (std::string title =
                    graphString(graphField(*targetState, "name"));
                !title.empty())
              row.threadContext = std::move(title);
            break;
          }
          if (!row.threadContext.empty())
            break;
        }
      }
      if (wantRequests)
        result.requests.requests.push_back(row);
      ++result.state.pendingRequestCount;
      if (pendingMetadata.size() < 64)
        pendingMetadata.push_back(
            {{"id", interaction->id().canonical},
             {"method", method},
             {"category", row.kind},
             {"thread", row.threadContext},
             {"status", graphStatus(*state)}});
    }
  }

  if (thread) {
    const auto threadState = read->state(thread);
    if (wantChanges)
      result.changes.cwd = graphString(graphField(*threadState, "cwd"));
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
        const std::string type = graphString(graphField(*state, "type"));
        if (wantChanges && type == "commandExecution") {
          appendUniqueBounded(result.changes.commandCwds,
                              graphString(graphField(*state, "cwd")), 64);
        } else if (wantChanges && type == "fileChange") {
          appendUniqueBounded(result.changes.commandCwds,
                              graphString(graphField(*state, "cwd")), 64);
          const nodegraph::Value *changes = graphField(*state, "changes");
          if (const auto *array = changes ? changes->asArray() : nullptr)
            for (const nodegraph::Value &change : *array)
              if (const auto *object = change.asObject())
                appendUniqueBounded(
                    result.changes.changedPaths,
                    graphString(graphMember(*object, "path")), 512);
        }
      }
    }

    const std::string threadStatus = graphStatus(*threadState);
    if (wantPlan)
      for (std::size_t offset = 0;
         offset < read->childCount(thread) && !result.plan.plan &&
         !result.plan.planItem;
         ++offset) {
      const nodegraph::NodeRef turn = read->childAt(
          thread, read->childCount(thread) - offset - 1);
      if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
        continue;
      const auto turnState = read->state(turn);
      const nodegraph::Value *planValue = graphField(*turnState, "plan");
      const nodegraph::Value::Array *steps =
          planValue ? planValue->asArray() : nullptr;
      const nodegraph::Value *explanation =
          graphField(*turnState, "planExplanation");
      bool structured = steps != nullptr;
      if (const auto *object = planValue ? planValue->asObject() : nullptr) {
        const nodegraph::Value *nested = graphMember(*object, "steps");
        structured = nested != nullptr;
        steps = nested ? nested->asArray() : nullptr;
        if (graphString(explanation).empty())
          explanation = graphMember(*object, "explanation");
      }
      if (structured) {
        InspectorPlan plan;
        plan.explanation = graphString(explanation);
        if (steps) {
          plan.steps.reserve(steps->size());
          for (const nodegraph::Value &entry : *steps) {
            const auto *object = entry.asObject();
            if (!object) {
              plan.steps.emplace_back();
              continue;
            }
            const std::string status =
                graphString(graphMember(*object, "status"));
            plan.steps.push_back(
                {graphString(graphMember(*object, "step")),
                 effectivePlanStepStatus(status, graphStatus(*turnState),
                                         threadStatus)});
          }
        }
        result.plan.plan = std::move(plan);
        break;
      }
      for (std::size_t itemOffset = 0;
           itemOffset < read->childCount(turn); ++itemOffset) {
        const nodegraph::NodeRef item = read->childAt(
            turn, read->childCount(turn) - itemOffset - 1);
        if (!item || item->id().kind != nodegraph::NodeKind::Item)
          continue;
        const auto state = read->state(item);
        if (graphString(graphField(*state, "type")) == "plan") {
          result.plan.planItem = graphString(graphField(*state, "text"));
          break;
        }
      }
    }

    if (wantAgents) {
    struct LogicalAgent final {
      InspectorAgentRow row;
      std::string status;
    };
    std::vector<LogicalAgent> logicalAgents;
    std::unordered_map<std::string, std::size_t> logicalIndexes;
    for (std::size_t turnIndex = 0; turnIndex < read->childCount(thread);
         ++turnIndex) {
      const nodegraph::NodeRef turn = read->childAt(thread, turnIndex);
      if (!turn || turn->id().kind != nodegraph::NodeKind::Turn)
        continue;
      const auto turnState = read->state(turn);
      for (std::size_t itemIndex = 0; itemIndex < read->childCount(turn);
           ++itemIndex) {
        const nodegraph::NodeRef item = read->childAt(turn, itemIndex);
        if (!item || item->id().kind != nodegraph::NodeKind::Item)
          continue;
        const auto state = read->state(item);
        const std::string type = graphString(graphField(*state, "type"));
        if (type != "subAgentActivity" && type != "collabAgentToolCall")
          continue;
        const std::string activityKind =
            graphString(graphField(*state, "kind"));
        const bool canCreate =
            type == "subAgentActivity"
                ? (activityKind.empty() || activityKind == "started")
                : spawnAgentTool(graphString(graphField(*state, "tool")));

        struct SourceChild final {
          std::string key;
          std::string id;
          nodegraph::NodeRef thread;
          std::string status;
          std::string result;
          bool canonical = true;
        };
        std::vector<SourceChild> children;
        std::unordered_map<std::string, std::size_t> childIndexes;
        const auto addChild = [&](std::string id,
                                  nodegraph::NodeRef childThread = {},
                                  std::string status = {},
                                  std::string childResult = {}) {
          if (id.empty())
            return;
          const std::string key = "child\n" + id;
          const auto [found, inserted] =
              childIndexes.try_emplace(key, children.size());
          if (inserted) {
            children.push_back({key, std::move(id), std::move(childThread),
                                std::move(status), std::move(childResult),
                                true});
            return;
          }
          SourceChild &child = children.at(found->second);
          if (childThread)
            child.thread = std::move(childThread);
          updateAgentStatus(child.status, std::move(status));
          if (!childResult.empty())
            child.result = std::move(childResult);
        };

        for (const nodegraph::NodeRef &child : read->related(
                 item, nodegraph::RelationKind::AgentChildThread))
          if (child && child->id().kind == nodegraph::NodeKind::Thread)
            addChild(child->id().canonical, child);
        addChild(graphString(graphField(*state, "agentThreadId")));
        const std::vector<std::string> receivers =
            graphStrings(graphField(*state, "receiverThreadIds"));
        for (const std::string &receiver : receivers)
          addChild(receiver);
        if (const nodegraph::Value *statesValue =
                graphField(*state, "agentsStates")) {
          if (const auto *states = statesValue->asObject()) {
            for (const auto &[id, value] : *states) {
              const auto *childState = value.asObject();
              addChild(id, {},
                       childState
                           ? graphString(graphMember(*childState, "status"))
                           : std::string{},
                       childState
                           ? graphString(graphMember(*childState, "message"))
                           : std::string{});
            }
          }
        }
        if (children.empty() && canCreate) {
          std::string id = nodegraph::protocolCanonicalId(*state, item);
          if (id.empty())
            id = item->id().canonical;
          children.push_back({"source\n" + item->id().canonical,
                              std::move(id), {}, {}, {}, false});
        }

        for (SourceChild &child : children) {
          auto found = logicalIndexes.find(child.key);
          if (found == logicalIndexes.end()) {
            if (!canCreate)
              continue;
            LogicalAgent logical;
            logical.row.id = child.id;
            if (child.canonical)
              logical.row.childThreadId = child.id;
            found = logicalIndexes
                        .emplace(child.key, logicalAgents.size())
                        .first;
            logicalAgents.push_back(std::move(logical));
          }
          LogicalAgent &logical = logicalAgents.at(found->second);
          const bool carriesFields = type == "subAgentActivity" || canCreate;
          if (carriesFields) {
            const auto update = [](std::string &target, std::string value) {
              if (!value.empty())
                target = std::move(value);
            };
            update(logical.row.agentPath,
                   graphString(graphField(*state, "agentPath")));
            update(logical.row.tool,
                   graphString(graphField(*state, "tool")));
            update(logical.row.model,
                   graphString(graphField(*state, "model")));
            update(logical.row.reasoningEffort,
                   graphString(graphField(*state, "reasoningEffort")));
            update(logical.row.prompt,
                   graphString(graphField(*state, "prompt")));
            update(logical.row.resultText,
                   graphString(graphField(*state, "resultText")));
            update(logical.row.senderThreadId,
                   graphString(graphField(*state, "senderThreadId")));
            if (!receivers.empty())
              logical.row.receiverThreadIds = receivers;
            std::string activityStatus = agentActivityStatus(*state);
            if (child.canonical && terminalStatus(graphStatus(*turnState)) &&
                isActiveStatus(activityStatus) && child.status.empty())
              activityStatus = "notLoaded";
            updateAgentStatus(logical.status, std::move(activityStatus));
          }
          updateAgentStatus(logical.status, child.status);

          nodegraph::NodeRef childThread = child.thread;
          if (!childThread && child.canonical)
            childThread = read->find(
                {nodegraph::NodeKind::Thread, child.id});
          if (childThread && read->contains(childThread) &&
              !read->removed(childThread)) {
            const auto childState = read->state(childThread);
            updateAgentStatus(logical.status, graphStatus(*childState));
            for (std::size_t childTurnOffset = 0;
                 childTurnOffset < read->childCount(childThread) &&
                 logical.row.resultText.empty();
                 ++childTurnOffset) {
              const nodegraph::NodeRef childTurn = read->childAt(
                  childThread,
                  read->childCount(childThread) - childTurnOffset - 1);
              if (!childTurn)
                continue;
              for (std::size_t childItemOffset = 0;
                   childItemOffset < read->childCount(childTurn);
                   ++childItemOffset) {
                const nodegraph::NodeRef childItem = read->childAt(
                    childTurn,
                    read->childCount(childTurn) - childItemOffset - 1);
                if (!childItem)
                  continue;
                const auto childItemState = read->state(childItem);
                if (graphString(graphField(*childItemState, "type")) !=
                    "agentMessage")
                  continue;
                const std::string value =
                    graphString(graphField(*childItemState, "text"));
                if (!value.empty()) {
                  logical.row.resultText = value;
                  break;
                }
              }
            }
          }
          if (!child.result.empty())
            logical.row.resultText = child.result;
          logical.row.status = logical.status;
        }
      }
    }
    result.agents.agents.reserve(logicalAgents.size());
    for (LogicalAgent &logical : logicalAgents)
      result.agents.agents.push_back(std::move(logical.row));
    }
  }

  nlohmann::json selected = nullptr;
  if (wantState && thread) {
    const auto state = read->state(thread);
    selected = {{"id", thread->id().canonical},
                {"status", graphStatus(*state)},
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
    result.state.state =
        {{"sharedNodeGraph",
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

std::optional<VisibleCardData>
NodeGraphUiAdapter::card(const nodegraph::NodeRef &thread,
                         const nodegraph::NodeRef &item) const {
  if (!graph_ || !thread || !item)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->contains(thread) || !read->contains(item) ||
      read->removed(thread) || read->removed(item) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;
  nodegraph::NodeRef turn = read->parent(item);
  if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
      read->parent(turn) != thread)
    return std::nullopt;
  const auto state = read->state(item);
  const auto turnState = read->state(turn);
  const auto threadState = read->state(thread);
  if (!state || !turnState || !threadState)
    return std::nullopt;
  return graphCardData(item, thread->id().canonical,
                       nodegraph::protocolCanonicalId(*turnState, turn), *state,
                       graphString(graphField(*threadState, "cwd")));
}

std::optional<PromptMaterialization> NodeGraphUiAdapter::promptMaterialization(
    const nodegraph::NodeRef &thread, const nodegraph::NodeRef &item) const {
  if (!graph_ || !thread || !item)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->contains(thread) || !read->contains(item) ||
      read->removed(thread) || read->removed(item) ||
      thread->id().kind != nodegraph::NodeKind::Thread ||
      item->id().kind != nodegraph::NodeKind::Item)
    return std::nullopt;

  const nodegraph::NodeRef turn = read->parent(item);
  if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
      read->parent(turn) != thread)
    return std::nullopt;
  const auto state = read->state(item);
  const auto turnState = read->state(turn);
  const auto threadState = read->state(thread);
  if (!state || !turnState || !threadState ||
      graphCardKind(*state) != CardKind::UserMessage)
    return std::nullopt;

  for (const nodegraph::NodeRef &prompt :
       read->related(item, nodegraph::RelationKind::PromptMaterialization)) {
    if (!prompt || !read->contains(prompt) || read->removed(prompt) ||
        prompt->id().kind != nodegraph::NodeKind::Item)
      continue;
    const auto promptState = read->state(prompt);
    const nodegraph::NodeRef promptTurn = read->parent(prompt);
    if (!promptState || !promptTurn || read->parent(promptTurn) != thread ||
        graphString(graphField(*promptState, "type")) != "localPrompt" ||
        graphString(graphField(*promptState, "dispatchState")) !=
            "awaitingMaterialization")
      continue;
    const auto submissionId =
        graphInteger(graphField(*promptState, "submissionId"));
    if (!submissionId || *submissionId < 0)
      continue;

    VisibleCardData card = graphCardData(
        item, thread->id().canonical,
        nodegraph::protocolCanonicalId(*turnState, turn), *state,
        graphString(graphField(*threadState, "cwd")));
    card.key = LocalPromptKey{static_cast<std::uint64_t>(*submissionId)};
    return PromptMaterialization{std::move(card), prompt};
  }
  return std::nullopt;
}

std::optional<ConversationRowChange>
NodeGraphUiAdapter::rowChange(const nodegraph::NodeRef &thread,
                              const nodegraph::NodeRef &item) const {
  if (!graph_ || !thread || !item)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->contains(thread) || !read->contains(item) ||
      read->removed(thread) || read->removed(item) ||
      thread->id().kind != nodegraph::NodeKind::Thread ||
      item->id().kind != nodegraph::NodeKind::Item)
    return std::nullopt;

  const nodegraph::NodeRef turn = read->parent(item);
  if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
      read->parent(turn) != thread)
    return std::nullopt;
  const auto itemState = read->state(item);
  const auto turnState = read->state(turn);
  const auto threadState = read->state(thread);
  if (!itemState || !turnState || !threadState)
    return std::nullopt;

  const std::string threadId = thread->id().canonical;
  const auto turnId = [&](const nodegraph::NodeRef &owner) {
    const auto state = owner ? read->state(owner) : nullptr;
    return state ? nodegraph::protocolCanonicalId(*state, owner)
                 : std::string{};
  };
  const auto readyPrompts = [&](const nodegraph::NodeRef &owner) {
    std::unordered_set<const nodegraph::Node *> result;
    const std::size_t count = read->childCount(owner);
    for (std::size_t index = 0; index < count; ++index) {
      const nodegraph::NodeRef candidate = read->childAt(owner, index);
      if (!candidate || !read->contains(candidate) || read->removed(candidate))
        continue;
      for (const nodegraph::NodeRef &prompt : read->related(
               candidate, nodegraph::RelationKind::PromptMaterialization)) {
        if (!prompt || !read->contains(prompt) || read->removed(prompt))
          continue;
        const auto state = read->state(prompt);
        if (state && graphString(graphField(*state, "type")) == "localPrompt" &&
            graphString(graphField(*state, "dispatchState")) ==
                "awaitingMaterialization")
          result.insert(prompt.get());
      }
    }
    return result;
  };
  const auto projectedKey =
      [&](const nodegraph::NodeRef &candidate,
          const nodegraph::NodeRef &owner,
          const std::unordered_set<const nodegraph::Node *> &hiddenPrompts)
      -> std::optional<CardKey> {
    if (!candidate || !read->contains(candidate) || read->removed(candidate) ||
        candidate->id().kind != nodegraph::NodeKind::Item)
      return std::nullopt;
    const auto state = read->state(candidate);
    if (!state)
      return std::nullopt;
    const std::string type = graphString(graphField(*state, "type"));
    if (type == "localPrompt") {
      if (hiddenPrompts.contains(candidate.get()))
        return std::nullopt;
      const std::int64_t rawId =
          graphInteger(graphField(*state, "submissionId")).value_or(0);
      return LocalPromptKey{rawId < 0 ? 0
                                     : static_cast<std::uint64_t>(rawId)};
    }
    if (graphCardKind(*state) == CardKind::UserMessage) {
      const auto submission =
          graphInteger(graphField(*state, "localSubmissionId"));
      if (submission && *submission >= 0)
        return LocalPromptKey{static_cast<std::uint64_t>(*submission)};
    }
    return AuthoritativeItemKey{
        threadId, turnId(owner),
        nodegraph::protocolCanonicalId(*state, candidate)};
  };

  const auto hiddenInTurn = readyPrompts(turn);
  const std::optional<CardKey> itemKey =
      projectedKey(item, turn, hiddenInTurn);
  if (!itemKey)
    return std::nullopt;

  std::optional<CardKey> previous;
  std::optional<CardKey> next;
  const std::size_t itemCount = read->childCount(turn);
  std::size_t itemIndex = itemCount;
  for (std::size_t index = 0; index < itemCount; ++index) {
    if (read->childAt(turn, index) == item) {
      itemIndex = index;
      break;
    }
  }
  if (itemIndex == itemCount)
    return std::nullopt;
  for (std::size_t offset = itemIndex; offset > 0 && !previous; --offset)
    previous = projectedKey(read->childAt(turn, offset - 1), turn,
                            hiddenInTurn);
  for (std::size_t index = itemIndex + 1; index < itemCount && !next; ++index)
    next = projectedKey(read->childAt(turn, index), turn, hiddenInTurn);

  const std::size_t turnCount = read->childCount(thread);
  std::size_t turnIndex = turnCount;
  for (std::size_t index = 0; index < turnCount; ++index) {
    if (read->childAt(thread, index) == turn) {
      turnIndex = index;
      break;
    }
  }
  if (turnIndex == turnCount)
    return std::nullopt;
  for (std::size_t offset = turnIndex; offset > 0 && !previous; --offset) {
    const nodegraph::NodeRef owner = read->childAt(thread, offset - 1);
    if (!owner || owner->id().kind != nodegraph::NodeKind::Turn)
      continue;
    const auto hidden = readyPrompts(owner);
    for (std::size_t child = read->childCount(owner);
         child > 0 && !previous; --child)
      previous =
          projectedKey(read->childAt(owner, child - 1), owner, hidden);
  }
  for (std::size_t ownerIndex = turnIndex + 1;
       ownerIndex < turnCount && !next; ++ownerIndex) {
    const nodegraph::NodeRef owner = read->childAt(thread, ownerIndex);
    if (!owner || owner->id().kind != nodegraph::NodeKind::Turn)
      continue;
    const auto hidden = readyPrompts(owner);
    for (std::size_t child = 0;
         child < read->childCount(owner) && !next; ++child)
      next = projectedKey(read->childAt(owner, child), owner, hidden);
  }

  const auto roots =
      read->related(turn, nodegraph::RelationKind::TurnRootItem);
  const nodegraph::NodeRef root =
      !roots.empty() && roots.front() && read->contains(roots.front()) &&
              !read->removed(roots.front())
          ? roots.front()
          : nodegraph::NodeRef{};
  bool activeTurn = graphTurnIsActive(*turnState);
  if (!activeTurn) {
    const auto active =
        read->related(thread, nodegraph::RelationKind::ActiveTurn);
    activeTurn = std::ranges::find(active, turn) != active.end();
  }

  ConversationRowChange result;
  result.placement.card = graphCardData(
      item, threadId, turnId(turn), *itemState,
      graphString(graphField(*threadState, "cwd")));
  result.placement.sectionKey =
      sectionComponent("turn:", threadId, turnId(turn));
  result.placement.turnRoot = root == item;
  result.placement.nested = root && root != item;
  result.placement.activeTurn = activeTurn;
  result.placement.historyActivity =
      graphString(graphField(*itemState, "type")) != "localPrompt";
  result.previousCardKey = std::move(previous);
  result.nextCardKey = std::move(next);
  return result;
}

std::optional<ConversationTailCard>
NodeGraphUiAdapter::tailCard(const nodegraph::NodeRef &thread,
                             const nodegraph::NodeRef &item) const {
  if (!graph_ || !thread || !item)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->contains(thread) || !read->contains(item) ||
      read->removed(thread) || read->removed(item) ||
      thread->id().kind != nodegraph::NodeKind::Thread ||
      item->id().kind != nodegraph::NodeKind::Item)
    return std::nullopt;

  const nodegraph::NodeRef turn = read->parent(item);
  if (!turn || turn->id().kind != nodegraph::NodeKind::Turn ||
      read->parent(turn) != thread)
    return std::nullopt;
  const std::size_t turnCount = read->childCount(thread);
  const std::size_t itemCount = read->childCount(turn);
  if (turnCount == 0 || itemCount == 0 ||
      read->childAt(thread, turnCount - 1) != turn ||
      read->childAt(turn, itemCount - 1) != item)
    return std::nullopt;

  // Prompt materialization deliberately reuses the local visual key and
  // action target. The complete projection owns that uncommon alias handoff.
  if (!read->related(item, nodegraph::RelationKind::PromptMaterialization)
           .empty())
    return std::nullopt;

  const auto state = read->state(item);
  const auto turnState = read->state(turn);
  const auto threadState = read->state(thread);
  if (!state || !turnState || !threadState)
    return std::nullopt;
  const auto authoritativeCount =
      graphSize(graphField(*threadState, "historyLoadedItemCount"));
  if (!authoritativeCount)
    return std::nullopt;

  const std::string threadId = thread->id().canonical;
  const std::string turnId = nodegraph::protocolCanonicalId(*turnState, turn);
  const auto roots = read->related(turn, nodegraph::RelationKind::TurnRootItem);
  const nodegraph::NodeRef root = !roots.empty() && roots.front() &&
                                          read->contains(roots.front()) &&
                                          !read->removed(roots.front())
                                      ? roots.front()
                                      : nodegraph::NodeRef{};
  const bool turnRoot = root == item;
  bool activeTurn = graphTurnIsActive(*turnState);
  if (!activeTurn) {
    const auto active =
        read->related(thread, nodegraph::RelationKind::ActiveTurn);
    activeTurn = std::ranges::find(active, turn) != active.end();
  }

  ConversationTailCard result;
  result.card = graphCardData(item, threadId, turnId, *state,
                              graphString(graphField(*threadState, "cwd")));
  result.sectionKey = sectionComponent("turn:", threadId, turnId);
  result.turnRoot = turnRoot;
  result.nested = root && !turnRoot;
  result.activeTurn = activeTurn;
  result.historyActivity =
      graphString(graphField(*state, "type")) != "localPrompt";
  result.authoritativeItemCount = *authoritativeCount;
  result.providerHasMore = graphProviderHasMoreHistory(*threadState);
  return result;
}

std::optional<ConversationSnapshot>
NodeGraphUiAdapter::conversation(const nodegraph::NodeRef &thread,
                                 std::size_t itemLimit) const {
  if (!graph_ || !thread)
    return std::nullopt;
  auto read = graph_->tryRead();
  if (!read || !read->contains(thread) || read->removed(thread) ||
      thread->id().kind != nodegraph::NodeKind::Thread)
    return std::nullopt;

  struct TurnInput {
    nodegraph::NodeRef turn;
    std::shared_ptr<const nodegraph::NodeState> state;
    std::string id;
    nodegraph::NodeRef root;
    std::vector<nodegraph::NodeRef> items;
  };

  const auto threadState = read->state(thread);
  if (!threadState)
    return std::nullopt;
  const std::string threadCwd = graphString(graphField(*threadState, "cwd"));
  itemLimit = std::max<std::size_t>(1, itemLimit);
  const std::optional<std::size_t> retainedAuthoritativeCount =
      graphSize(graphField(*threadState, "historyLoadedItemCount"));

  std::vector<TurnInput> turns;
  std::unordered_map<const nodegraph::Node *, std::size_t> turnPositions;
  std::size_t itemCount = retainedAuthoritativeCount.value_or(0);
  const std::size_t turnCount = read->childCount(thread);
  turns.reserve(turnCount);
  for (std::size_t turnIndex = 0; turnIndex < turnCount; ++turnIndex) {
    nodegraph::NodeRef turn = read->childAt(thread, turnIndex);
    if (!turn || !read->contains(turn) || read->removed(turn) ||
        turn->id().kind != nodegraph::NodeKind::Turn)
      continue;
    const auto state = read->state(turn);
    if (!state)
      continue;
    TurnInput input;
    input.turn = turn;
    input.state = state;
    input.id = nodegraph::protocolCanonicalId(*state, turn);
    const auto roots =
        read->related(turn, nodegraph::RelationKind::TurnRootItem);
    if (!roots.empty() && roots.front() && read->contains(roots.front()) &&
        !read->removed(roots.front()))
      input.root = roots.front();

    turnPositions.emplace(turn.get(), turns.size());
    turns.push_back(std::move(input));
  }

  std::unordered_set<const nodegraph::Node *> boundedAuthoritativeItems;
  if (retainedAuthoritativeCount) {
    std::size_t remaining = itemLimit;
    for (std::size_t turnOffset = turns.size(); turnOffset > 0 && remaining > 0;
         --turnOffset) {
      TurnInput &input = turns[turnOffset - 1];
      const std::size_t childCount = read->childCount(input.turn);
      for (std::size_t itemOffset = childCount;
           itemOffset > 0 && remaining > 0; --itemOffset) {
        nodegraph::NodeRef item = read->childAt(input.turn, itemOffset - 1);
        if (!item || !read->contains(item) || read->removed(item) ||
            item->id().kind != nodegraph::NodeKind::Item)
          continue;
        const auto state = read->state(item);
        if (!state || graphString(graphField(*state, "type")) == "localPrompt")
          continue;
        input.items.push_back(item);
        boundedAuthoritativeItems.insert(item.get());
        --remaining;
      }
      std::ranges::reverse(input.items);
    }

    // User-authored optimistic/recovery prompts are explicitly protected from
    // history paging. The worker maintains this narrow relation, so retaining
    // them does not require scanning all historical items.
    for (const nodegraph::NodeRef &prompt :
         read->related(thread, nodegraph::RelationKind::PendingPrompt)) {
      if (!prompt || !read->contains(prompt) || read->removed(prompt) ||
          prompt->id().kind != nodegraph::NodeKind::Item)
        continue;
      const nodegraph::NodeRef turn = read->parent(prompt);
      const auto position = turnPositions.find(turn.get());
      if (position == turnPositions.end())
        continue;
      std::vector<nodegraph::NodeRef> &items = turns[position->second].items;
      if (std::ranges::find(items, prompt) == items.end())
        items.push_back(prompt);
    }
  } else {
    for (TurnInput &input : turns) {
      const std::size_t childCount = read->childCount(input.turn);
      input.items.reserve(childCount);
      for (std::size_t itemIndex = 0; itemIndex < childCount; ++itemIndex) {
        nodegraph::NodeRef item = read->childAt(input.turn, itemIndex);
        if (!item || !read->contains(item) || read->removed(item) ||
            item->id().kind != nodegraph::NodeKind::Item)
          continue;
        input.items.push_back(item);
        const auto itemState = read->state(item);
        if (itemState && graphString(graphField(*itemState, "type")) !=
                             "localPrompt")
          ++itemCount;
      }
    }
  }

  const std::size_t skip = itemCount > itemLimit ? itemCount - itemLimit : 0;
  std::size_t visited = 0;

  ConversationSnapshot result;
  result.threadId = thread->id().canonical;
  std::size_t pinnedRoots = 0;
  if (retainedAuthoritativeCount) {
    for (TurnInput &input : turns) {
      if (input.items.empty() || !input.root ||
          boundedAuthoritativeItems.contains(input.root.get()))
        continue;
      if (std::ranges::find(input.items, input.root) == input.items.end())
        input.items.insert(input.items.begin(), input.root);
      const auto rootState = read->state(input.root);
      if (skip != 0 && read->parent(input.root) == input.turn && rootState &&
          graphString(graphField(*rootState, "type")) != "localPrompt")
        ++pinnedRoots;
    }
  } else {
    std::unordered_set<const nodegraph::Node *> representedTurns;
    std::size_t authoritativeIndex = 0;
    for (const TurnInput &input : turns) {
      for (const nodegraph::NodeRef &item : input.items) {
        const auto state = read->state(item);
        if (!state || graphString(graphField(*state, "type")) == "localPrompt")
          continue;
        if (authoritativeIndex++ >= skip)
          representedTurns.insert(input.turn.get());
      }
    }
    authoritativeIndex = 0;
    for (const TurnInput &input : turns) {
      for (const nodegraph::NodeRef &item : input.items) {
        const auto state = read->state(item);
        if (!state || graphString(graphField(*state, "type")) == "localPrompt")
          continue;
        if (authoritativeIndex < skip && item == input.root &&
            representedTurns.contains(input.turn.get()))
          ++pinnedRoots;
        ++authoritativeIndex;
      }
    }
  }
  result.hiddenAuthoritativeItemCount =
      pinnedRoots < skip ? skip - pinnedRoots : 0;
  result.hasMore = skip != 0 || graphProviderHasMoreHistory(*threadState);

  const auto activeTurns =
      read->related(thread, nodegraph::RelationKind::ActiveTurn);
  if (!activeTurns.empty() && activeTurns.front() &&
      read->contains(activeTurns.front()) && !read->removed(activeTurns.front())) {
    const auto state = read->state(activeTurns.front());
    if (state)
      result.activeTurnId =
          nodegraph::protocolCanonicalId(*state, activeTurns.front());
  }

  for (TurnInput &input : turns) {
    TurnSection section;
    section.key =
        sectionComponent("turn:", result.threadId, input.id);
    section.turnId = input.id;
    section.rootPinned = retainedAuthoritativeCount && input.root &&
                         !input.items.empty() &&
                         !boundedAuthoritativeItems.contains(input.root.get());
    bool rootAdded = false;
    std::unordered_set<const nodegraph::Node *> readyPrompts;
    for (const nodegraph::NodeRef &candidate : input.items) {
      for (const nodegraph::NodeRef &prompt : read->related(
               candidate, nodegraph::RelationKind::PromptMaterialization)) {
        if (!prompt || !read->contains(prompt) || read->removed(prompt))
          continue;
        const auto promptState = read->state(prompt);
        if (promptState &&
            graphString(graphField(*promptState, "type")) == "localPrompt" &&
            graphString(graphField(*promptState, "dispatchState")) ==
                "awaitingMaterialization")
          readyPrompts.insert(prompt.get());
      }
    }

    const auto append = [&](const nodegraph::NodeRef &item, bool root) {
      if (!item || !read->contains(item) || read->removed(item))
        return;
      auto state = read->state(item);
      if (!state)
        return;
      if (graphString(graphField(*state, "type")) == "localPrompt" &&
          readyPrompts.contains(item.get()))
        return;

      nodegraph::NodeRef projectedNode = item;
      nodegraph::NodeRef actionTarget = item;
      std::shared_ptr<const nodegraph::NodeState> projectedState = state;
      std::optional<std::uint64_t> promptVisualId;
      const auto promptRelations =
          read->related(item, nodegraph::RelationKind::PromptMaterialization);
      for (const nodegraph::NodeRef &prompt : promptRelations) {
        if (!prompt || !read->contains(prompt) || read->removed(prompt) ||
            prompt->id().kind != nodegraph::NodeKind::Item)
          continue;
        const auto promptState = read->state(prompt);
        if (!promptState ||
            graphString(graphField(*promptState, "type")) != "localPrompt")
          continue;
        const std::int64_t rawId =
            graphInteger(graphField(*promptState, "submissionId")).value_or(0);
        promptVisualId =
            rawId < 0 ? 0 : static_cast<std::uint64_t>(rawId);
        actionTarget = prompt;
        if (graphString(graphField(*promptState, "dispatchState")) !=
            "awaitingMaterialization")
          return;
        break;
      }

      VisibleCardData card = graphCardData(projectedNode, result.threadId,
                                           input.id, *projectedState,
                                           threadCwd);
      if (promptVisualId)
        card.key = LocalPromptKey{*promptVisualId};
      card.target = std::move(actionTarget);
      if (root) {
        section.rootCardKey = card.key;
        rootAdded = true;
      }
      section.cards.push_back(std::move(card));
    };

    if (input.root && read->parent(input.root) != input.turn)
      append(input.root, true);

    for (const nodegraph::NodeRef &item : input.items) {
      const auto itemState = read->state(item);
      const bool localPrompt =
          itemState &&
          graphString(graphField(*itemState, "type")) == "localPrompt";
      const bool selected = retainedAuthoritativeCount || localPrompt ||
                            visited++ >= skip;
      const bool root = item == input.root;
      if (!selected && !root)
        continue;
      if (!selected && root)
        section.rootPinned = true;
      append(item, root);
    }

    if (!section.cards.empty()) {
      if (!rootAdded && input.root) {
        const auto rootState = read->state(input.root);
        if (rootState) {
          const std::string rootId =
              nodegraph::protocolCanonicalId(*rootState, input.root);
          section.rootCardKey = AuthoritativeItemKey{
              result.threadId, input.id, rootId};
        }
      }
      result.sections.push_back(std::move(section));
    }

    if (!result.activeTurnId && graphTurnIsActive(*input.state))
      result.activeTurnId = input.id;
  }

  return result;
}


} // namespace codexui::codex::ui
