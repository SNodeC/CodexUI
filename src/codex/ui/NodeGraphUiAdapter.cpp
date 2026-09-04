// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/NodeGraphUiAdapter.h"

#include "codex/nodegraph/ProtocolUpdater.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
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

bool graphCardVisible(const nodegraph::NodeState &state,
                      const NodeGraphUiAdapter::ConversationOptions &options) {
  const CardKind kind = graphCardKind(state);
  if (kind == CardKind::Reasoning)
    return options.showReasoning;
  if (kind != CardKind::AgentMessage)
    return true;
  return graphString(graphField(state, "phase")) == "final_answer" ||
         options.showCodexUpdates;
}

VisibleCardData graphCardData(const nodegraph::NodeRef &item,
                              std::string threadId, std::string turnId,
                              const nodegraph::NodeState &state) {
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
  case CardKind::UserMessage:
    result.payload =
        UserMessageData{graphMessageText(state), graphImagePaths(state)};
    break;
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
    FileChangesData projected{graphStatus(state), {}};
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
  return result;
}


} // namespace

using namespace middle;


NodeGraphUiAdapter::NodeGraphUiAdapter(
    const nodegraph::NodeGraph &graph) noexcept
    : graph_(&graph) {}

std::optional<VisibleCardData>
NodeGraphUiAdapter::card(const nodegraph::NodeRef &thread,
                         const nodegraph::NodeRef &item,
                         ConversationOptions options) const {
  static_cast<void>(options);
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
  if (!state || !turnState)
    return std::nullopt;
  return graphCardData(item, thread->id().canonical,
                       nodegraph::protocolCanonicalId(*turnState, turn), *state);
}

std::optional<ConversationSnapshot>
NodeGraphUiAdapter::conversation(const nodegraph::NodeRef &thread,
                                 std::size_t itemLimit,
                                 ConversationOptions options) const {
  static_cast<void>(options);
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

  std::vector<TurnInput> turns;
  std::size_t itemCount = 0;
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

    const std::size_t childCount = read->childCount(turn);
    input.items.reserve(childCount);
    for (std::size_t itemIndex = 0; itemIndex < childCount; ++itemIndex) {
      nodegraph::NodeRef item = read->childAt(turn, itemIndex);
      if (!item || !read->contains(item) || read->removed(item) ||
          item->id().kind != nodegraph::NodeKind::Item)
        continue;
      input.items.push_back(item);
      ++itemCount;
    }
    turns.push_back(std::move(input));
  }

  itemLimit = std::max<std::size_t>(1, itemLimit);
  const std::size_t skip = itemCount > itemLimit ? itemCount - itemLimit : 0;
  std::size_t visited = 0;

  ConversationSnapshot result;
  result.threadId = thread->id().canonical;
  result.hiddenAuthoritativeItemCount = skip;
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
    section.key = input.id;
    section.turnId = input.id;
    bool rootAdded = false;

    const auto append = [&](const nodegraph::NodeRef &item, bool root) {
      if (!item || !read->contains(item) || read->removed(item))
        return;
      auto state = read->state(item);
      if (!state)
        return;

      nodegraph::NodeRef projectedNode = item;
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
        if (graphString(graphField(*promptState, "dispatchState")) !=
            "awaitingMaterialization") {
          projectedNode = prompt;
          projectedState = promptState;
        }
        break;
      }

      VisibleCardData card = graphCardData(projectedNode, result.threadId,
                                           input.id, *projectedState);
      if (promptVisualId)
        card.key = LocalPromptKey{*promptVisualId};
      if (root) {
        section.rootCardKey = card.key;
        rootAdded = true;
      }
      section.cards.push_back(std::move(card));
    };

    if (input.root && read->parent(input.root) != input.turn)
      append(input.root, true);

    for (const nodegraph::NodeRef &item : input.items) {
      const bool selected = visited++ >= skip;
      const bool root = item == input.root;
      if (!selected && !root)
        continue;
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
