// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationProjection.h"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

// The Inspector is the production owner of structured turn plans. Keep the
// complete Conversation projection available for a one-line policy reversal.
constexpr bool projectStructuredPlansInConversation = false;

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto value = object.find(key);
  return value != object.end() && value->is_string() ? value->get<std::string>()
                                                     : std::string{};
}

std::optional<std::int64_t> integerValue(const nlohmann::json &object,
                                   const char *key) {
  if (!object.is_object())
    return std::nullopt;
  const auto value = object.find(key);
  if (value == object.end() || !value->is_number_integer())
    return std::nullopt;
  return value->get<std::int64_t>();
}

std::optional<std::string> optionalText(const nlohmann::json &object,
                                    const char *key) {
  if (!object.is_object())
    return std::nullopt;
  const auto value = object.find(key);
  if (value == object.end() || !value->is_string())
    return std::nullopt;
  return value->get<std::string>();
}

std::uint64_t omittedTextBytes(const ItemPresentation &item,
                               const char *field) {
  const auto value =
      std::find_if(item.textRetention.begin(), item.textRetention.end(),
      [field](const TextRetentionPresentation &entry) {
        return entry.field == field;
      });
  return value == item.textRetention.end() ? 0 : value->discardedBytes;
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

std::pair<int, int> unifiedDiffCounts(std::string_view diff) {
  int additions = 0;
  int deletions = 0;
  for (std::size_t offset = 0; offset <= diff.size();) {
    const std::size_t end = diff.find('\n', offset);
    const std::string_view line =
        diff.substr(offset, end == std::string_view::npos ? diff.size() - offset
                                                          : end - offset);
    if (line.starts_with("+++ ") || line.starts_with("--- ")) {
      if (end == std::string_view::npos)
        break;
      offset = end + 1;
      continue;
    }
    if (line.starts_with('+'))
      ++additions;
    else if (line.starts_with('-'))
      ++deletions;
    if (end == std::string_view::npos)
      break;
    offset = end + 1;
  }
  return {additions, deletions};
}

std::string messageText(const nlohmann::json &item) {
  const std::string type = stringValue(item, "type");
  if (type == "agentMessage" || type == "plan")
    return stringValue(item, "text");
  if (type != "userMessage")
    return {};

  std::string result;
  const auto content = item.find("content");
  if (content != item.end() && content->is_array()) {
    for (const nlohmann::json &entry : *content) {
      const std::string value = stringValue(entry, "text");
      if (value.empty())
        continue;
      if (!result.empty())
        result.push_back('\n');
      result += value;
    }
  }
  if (result.empty()) {
    const std::string fallback = stringValue(item, "text");
    if (!fallback.empty())
      result = fallback;
  }
  return result;
}

std::vector<std::string> messageImagePaths(const nlohmann::json &item) {
  std::vector<std::string> result;
  const auto content = item.find("content");
  if (content == item.end() || !content->is_array())
    return result;
  for (const nlohmann::json &entry : *content) {
    if (stringValue(entry, "type") != "localImage")
      continue;
    const std::string path = stringValue(entry, "path");
    if (!path.empty())
      result.push_back(path);
  }
  return result;
}

std::vector<std::string> localImagePaths(const PromptSubmission &submission) {
  std::vector<std::string> result;
  for (const AttachmentDraft &attachment : submission.attachments)
    if (attachment.mimeType.starts_with("image/"))
      result.push_back(attachment.path);
  return result;
}

std::string joinedStrings(const nlohmann::json &value) {
  if (!value.is_array())
    return {};
  std::string result;
  for (const nlohmann::json &entry : value) {
    if (!entry.is_string())
      continue;
    if (!result.empty())
      result += ", ";
    result += entry.get<std::string>();
  }
  return result;
}

std::vector<std::string> stringList(const nlohmann::json &value) {
  std::vector<std::string> result;
  if (!value.is_array())
    return result;
  for (const nlohmann::json &entry : value)
    if (entry.is_string())
      result.push_back(entry.get<std::string>());
  return result;
}

bool hasStructuredPlan(const TurnPresentation &turn) {
  if (!turn.plan.is_object())
    return false;
  const auto steps = turn.plan.find("steps");
  return !stringValue(turn.plan, "explanation").empty() ||
         (steps != turn.plan.end() && steps->is_array() && !steps->empty());
}

PlanData structuredPlan(const TurnPresentation &turn) {
  PlanData result;
  result.explanation = stringValue(turn.plan, "explanation");
  const auto steps = turn.plan.find("steps");
  if (steps == turn.plan.end() || !steps->is_array())
    return result;
  result.steps.reserve(steps->size());
  for (const nlohmann::json &step : *steps) {
    const std::string value = stringValue(step, "step");
    if (!value.empty())
      result.steps.push_back({value, stringValue(step, "status")});
  }
  return result;
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

VisibleCardData authoritativeCard(const AuthoritativeItemKey &identity,
                                  const ItemPresentation &presentation,
                                  CardKey visualKey) {
  const nlohmann::json &item = presentation.raw;
  const std::string type = stringValue(item, "type");
  VisibleCardData result{std::move(visualKey), CardKind::GenericActivity,
      identity.threadId,    identity.turnId,
                         identity.itemId,      GenericActivityData{type, item}};

  if (type == "userMessage") {
    result.kind = CardKind::UserMessage;
    result.payload =
        UserMessageData{messageText(item), messageImagePaths(item)};
  } else if (type == "agentMessage") {
    result.kind = CardKind::AgentMessage;
    result.payload = AgentMessageData{
        withTruncationNotice(messageText(item),
                             omittedTextBytes(presentation, "text"),
                             "Codex response", true),
        stringValue(item, "phase") == "final_answer"};
  } else if (type == "commandExecution") {
    result.kind = CardKind::CommandExecution;
    const char *outputField = "aggregatedOutput";
    std::string output = stringValue(item, "aggregatedOutput");
    if (output.empty()) {
      outputField = "output";
      output = stringValue(item, "output");
    }
    output = withTruncationNotice(output,
                                  omittedTextBytes(presentation, outputField),
                                  "command output", false);
    if (!terminalOutputHasVisibleText(output))
      output.clear();
    std::optional<int> exitCode;
    const auto rawExitCode = item.find("exitCode");
    if (rawExitCode != item.end() && rawExitCode->is_number_integer())
      exitCode = rawExitCode->get<int>();
    std::optional<std::int64_t> duration = integerValue(item, "durationMs");
    if (!duration)
      duration = integerValue(item, "duration_ms");
    result.payload = CommandExecutionData{
        stringValue(item, "command"), output,   stringValue(item, "status"),
        stringValue(item, "cwd"),     exitCode, duration};
  } else if (type == "collabAgentToolCall" || type == "subAgentActivity") {
    result.kind = CardKind::AgentActivity;
    result.payload = AgentActivityData{
        stringValue(item, "tool"),
        stringValue(item, "status"),
        stringValue(item, "kind"),
        stringValue(item, "prompt"),
        stringValue(item, "resultText"),
        stringList(item.value("receiverThreadIds", nlohmann::json::array())),
        stringValue(item, "model"),
        stringValue(item, "reasoningEffort"),
        stringValue(item, "agentThreadId"),
        stringValue(item, "agentPath"),
        stringValue(item, "senderThreadId")};
  } else if (type == "reasoning") {
    result.kind = CardKind::Reasoning;
    result.payload = ReasoningData{withTruncationNotice(
            joinedStrings(item.value("summary", nlohmann::json::array())),
        omittedTextBytes(presentation, "summary"), "reasoning", true)};
  } else if (type == "fileChange") {
    result.kind = CardKind::FileChanges;
    const nlohmann::json changes =
        item.value("changes", nlohmann::json::array());
    FileChangesData projected{stringValue(item, "status"), {}};
    if (changes.is_array()) {
      projected.changes.reserve(changes.size());
      for (const nlohmann::json &change : changes) {
        FileChangeData entry{stringValue(change, "path"),
                             stringValue(change, "kind"), std::nullopt,
                             std::nullopt};
        if (const auto diff = optionalText(change, "diff")) {
          const auto [additions, deletions] = unifiedDiffCounts(*diff);
          entry.additions = additions;
          entry.deletions = deletions;
        }
        projected.changes.push_back(std::move(entry));
      }
    }
    result.payload = std::move(projected);
  } else if (type == "imageGeneration" || type == "imageView") {
    std::string path = stringValue(item, "path");
    if (path.empty())
      path = stringValue(item, "savedPath");
    if (path.empty())
      path = stringValue(item, "saved_path");
    std::string revisedPrompt = stringValue(item, "revisedPrompt");
    if (revisedPrompt.empty())
      revisedPrompt = stringValue(item, "revised_prompt");
    result.kind = CardKind::ImageGeneration;
    result.payload =
        ImageGenerationData{path, stringValue(item, "status"), revisedPrompt};
  } else if (type == "plan") {
    const std::string plan = withTruncationNotice(
        messageText(item), omittedTextBytes(presentation, "text"), "plan text",
        true);
    if (!plan.empty()) {
      result.kind = CardKind::Plan;
      result.payload = PlanData{{}, {}, plan};
    }
  }
  return result;
}

struct ProjectedNode {
  std::size_t position = 0;
  std::uint64_t tieBreaker = 0;
  std::string sectionKey;
  std::string turnId;
  VisibleCardData card;
  bool turnRoot = false;
};

std::optional<std::size_t> admissionBoundaryPosition(
    const std::optional<AuthoritativeItemKey> &admissionAnchor,
    bool admissionAtStart, const AuthoritativeItemIndex &authoritativeItems) {
  if (admissionAnchor) {
    const auto anchor = authoritativeItems.position(*admissionAnchor);
    if (anchor)
      return (*anchor + 1) * 2;
  }
  if (admissionAtStart)
    return 0;
  return std::nullopt;
}

std::size_t submissionPosition(
    const PromptSubmission &submission,
    const AuthoritativeItemIndex &authoritativeItems,
    std::optional<std::size_t> materializedIndex = std::nullopt) {
  const auto admitted = admissionBoundaryPosition(submission.admissionAnchor,
                                                  submission.admissionAtStart,
                                                  authoritativeItems);
  if (admitted)
    return *admitted;
  if (materializedIndex)
    return *materializedIndex * 2 + 1;
  // A queued pre-hydration prompt has no committed boundary yet. Until
  // reconcile establishes one, keep it at the tail of retained history.
  return authoritativeItems.ordered.size() * 2 + 2;
}

} // namespace

ConversationSnapshot ConversationProjection::project(
    const AuthoritativeItemIndex &authoritativeItems,
    const ThreadPresentation *authoritativeThread,
    std::span<const PromptSubmission> localSubmissions,
    std::size_t authoritativeItemLimit, std::int64_t nowMilliseconds) {
  ConversationSnapshot result;
  result.threadId = authoritativeItems.threadId;

  const std::size_t firstVisible =
      authoritativeItems.ordered.size() > authoritativeItemLimit
          ? authoritativeItems.ordered.size() - authoritativeItemLimit
          : 0;
  std::unordered_set<std::string> representedTurns;
  for (std::size_t index = firstVisible;
       index < authoritativeItems.ordered.size(); ++index)
    representedTurns.insert(authoritativeItems.ordered[index].key.turnId);

  // Roots are structural context, not activity-window budget. Pin the real
  // opening userMessage for every turn represented by the retained suffix.
  // This keeps long active and completed turns owned by the same You card and
  // prevents a later steering message from becoming an inferred root.
  std::set<std::size_t> pinnedRootIndexes;
  for (const std::string &turnId : representedTurns) {
    const auto root =
        authoritativeItems.turnRootUserMessagePositions.find(turnId);
    if (root != authoritativeItems.turnRootUserMessagePositions.end() &&
        root->second < firstVisible)
      pinnedRootIndexes.insert(root->second);
  }
  result.hiddenAuthoritativeItemCount = firstVisible - pinnedRootIndexes.size();
  result.hasMore = result.hiddenAuthoritativeItemCount > 0;

  std::map<AuthoritativeItemKey, const PromptSubmission *> bindings;
  for (const PromptSubmission &submission : localSubmissions)
    if (submission.materializedItem)
      bindings.emplace(*submission.materializedItem, &submission);

  std::vector<ProjectedNode> nodes;
  nodes.reserve(authoritativeItems.ordered.size() - firstVisible +
                pinnedRootIndexes.size() + localSubmissions.size());
  for (std::size_t index = 0; index < authoritativeItems.ordered.size();
       ++index) {
    if (index < firstVisible && !pinnedRootIndexes.contains(index))
      continue;
    const AuthoritativeItem &item = authoritativeItems.ordered[index];
    const auto binding = bindings.find(item.key);
    if (binding != bindings.end() &&
        binding->second->localCardVisible(nowMilliseconds))
      continue;
    CardKey visualKey =
        item.promptAlias ? CardKey{item.promptAlias->key} : CardKey{item.key};
    if (binding != bindings.end())
      visualKey = LocalPromptKey{binding->second->id};
    std::size_t position = index * 2 + 1;
    std::uint64_t tieBreaker = 0;
    if (binding != bindings.end()) {
      position =
          submissionPosition(*binding->second, authoritativeItems, index);
      tieBreaker = binding->second->admissionOrdinal;
    } else if (item.promptAlias) {
      const auto admitted = admissionBoundaryPosition(
          item.promptAlias->admissionAnchor, !item.promptAlias->admissionAnchor,
          authoritativeItems);
      position = admitted.value_or(position);
      tieBreaker = item.promptAlias->admissionOrdinal;
    }
    VisibleCardData card =
        authoritativeCard(item.key, *item.presentation, std::move(visualKey));
    const auto root =
        authoritativeItems.turnRootUserMessagePositions.find(item.key.turnId);
    const bool turnRoot =
        root != authoritativeItems.turnRootUserMessagePositions.end() &&
        root->second == index;
    nodes.push_back({position, tieBreaker,
                     sectionComponent("turn:", authoritativeItems.threadId,
                                      item.key.turnId),
                     item.key.turnId, std::move(card), turnRoot});
  }

  if (projectStructuredPlansInConversation && authoritativeThread) {
    std::unordered_map<std::string, std::size_t> firstItemIndexes;
    std::unordered_map<std::string, std::size_t> lastItemIndexes;
    for (std::size_t index = 0; index < authoritativeItems.ordered.size();
         ++index) {
      firstItemIndexes.try_emplace(authoritativeItems.ordered[index].key.turnId,
                                   index);
      lastItemIndexes[authoritativeItems.ordered[index].key.turnId] = index;
    }
    std::unordered_map<std::string, std::optional<std::size_t>> nextItemIndexes;
    std::optional<std::size_t> nextItemIndex;
    for (auto turnId = authoritativeThread->turnOrder.rbegin();
         turnId != authoritativeThread->turnOrder.rend(); ++turnId) {
      nextItemIndexes.emplace(*turnId, nextItemIndex);
      const auto first = firstItemIndexes.find(*turnId);
      if (first != firstItemIndexes.end())
        nextItemIndex = first->second;
    }

    for (const std::string &turnId : authoritativeThread->turnOrder) {
      const auto turn = authoritativeThread->turns.find(turnId);
      if (turn == authoritativeThread->turns.end() ||
          !hasStructuredPlan(turn->second))
        continue;

      const auto last = lastItemIndexes.find(turnId);
      const std::optional<std::size_t> lastItemIndex =
          last == lastItemIndexes.end()
              ? std::nullopt
              : std::optional<std::size_t>{last->second};
      const auto next = nextItemIndexes.find(turnId);
      const std::optional<std::size_t> followingItemIndex =
          next == nextItemIndexes.end() ? std::nullopt : next->second;
      if (lastItemIndex && *lastItemIndex < firstVisible)
        continue;
      const std::size_t position =
          lastItemIndex        ? *lastItemIndex * 2 + 2
          : followingItemIndex ? *followingItemIndex * 2
                               : authoritativeItems.ordered.size() * 2 + 2;
      nodes.push_back(
          {position,
           0,
           sectionComponent("turn:", authoritativeItems.threadId, turnId),
           turnId,
           {TurnPlanKey{authoritativeItems.threadId, turnId},
            CardKind::Plan,
            authoritativeItems.threadId,
            turnId,
            {},
            structuredPlan(turn->second)},
           false});
    }
  }

  for (const PromptSubmission &submission : localSubmissions) {
    if (!submission.localCardVisible(nowMilliseconds))
      continue;
    std::optional<std::size_t> materializedIndex;
    if (submission.materializedItem)
      materializedIndex =
          authoritativeItems.position(*submission.materializedItem);
    const std::size_t position =
        submissionPosition(submission, authoritativeItems, materializedIndex);

    bool knownTurn = false;
    if (authoritativeThread && submission.expectedTurnId) {
      const auto turn =
          authoritativeThread->turns.find(*submission.expectedTurnId);
      knownTurn = turn != authoritativeThread->turns.end();
    }
    const std::string turnId =
        submission.expectedTurnId.value_or(std::string{});
    const std::string sectionKey =
        knownTurn
            ? sectionComponent("turn:", authoritativeItems.threadId, turnId)
            : "pending:" + std::to_string(submission.id);
    VisibleCardData card{LocalPromptKey{submission.id},
                         CardKind::LocalPrompt,
                         authoritativeItems.threadId,
                         turnId,
                         {},
                         LocalPromptData{submission.id, submission.prompt,
                                         submission.state == PromptState::Queued
                                             ? PromptState::InFlight
                                             : submission.state,
                                         submission.acceptedAtMilliseconds,
                                         submission.error,
                                         localImagePaths(submission)}};
    const bool authoritativeRootExists =
        !turnId.empty() &&
        authoritativeItems.turnRootUserMessagePositions.contains(turnId);
    bool turnRoot =
        (!knownTurn || submission.startsTurn) && !authoritativeRootExists;
    if (submission.materializedItem) {
      const auto root = authoritativeItems.turnRootUserMessagePositions.find(
          submission.materializedItem->turnId);
      const auto materialized =
          authoritativeItems.position(*submission.materializedItem);
      turnRoot =
          root != authoritativeItems.turnRootUserMessagePositions.end() &&
                 materialized && root->second == *materialized;
    }
    nodes.push_back({position, submission.admissionOrdinal, sectionKey, turnId,
                     std::move(card), turnRoot});
  }

  std::ranges::sort(nodes,
                    [](const ProjectedNode &left, const ProjectedNode &right) {
                      if (left.position != right.position)
                        return left.position < right.position;
                      return left.tieBreaker < right.tieBreaker;
                    });

  // Aggregate by section key rather than merely grouping adjacent nodes. This
  // guarantees one structural section for each represented turn.
  std::map<std::string, std::size_t> sectionIndexes;
  for (ProjectedNode &node : nodes) {
    auto section = sectionIndexes.find(node.sectionKey);
    if (section == sectionIndexes.end()) {
      const std::size_t index = result.sections.size();
      sectionIndexes.emplace(node.sectionKey, index);
      result.sections.push_back(
          {node.sectionKey, node.turnId, {}, std::nullopt});
      section = sectionIndexes.find(node.sectionKey);
    }
    TurnSection &projectedSection = result.sections[section->second];
    if (node.turnRoot)
      projectedSection.rootCardKey = node.card.key;
    projectedSection.cards.push_back(std::move(node.card));
  }
  return result;
}

} // namespace codexui::codex::middle
