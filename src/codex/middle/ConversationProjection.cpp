// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationProjection.h"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace codexui::codex::middle {
namespace {

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto value = object.find(key);
  return value != object.end() && value->is_string() ? value->get<std::string>()
                                                     : std::string{};
}

QString messageText(const nlohmann::json &item) {
  const std::string type = stringValue(item, "type");
  if (type == "agentMessage" || type == "plan")
    return text(stringValue(item, "text"));
  if (type != "userMessage")
    return {};

  QStringList parts;
  const auto content = item.find("content");
  if (content != item.end() && content->is_array()) {
    for (const nlohmann::json &entry : *content) {
      const std::string value = stringValue(entry, "text");
      if (!value.empty())
        parts.push_back(text(value));
    }
  }
  if (parts.empty()) {
    const std::string fallback = stringValue(item, "text");
    if (!fallback.empty())
      parts.push_back(text(fallback));
  }
  return parts.join(QStringLiteral("\n"));
}

QStringList messageImagePaths(const nlohmann::json &item) {
  QStringList result;
  const auto content = item.find("content");
  if (content == item.end() || !content->is_array())
    return result;
  for (const nlohmann::json &entry : *content) {
    if (stringValue(entry, "type") != "localImage")
      continue;
    const std::string path = stringValue(entry, "path");
    if (!path.empty())
      result.push_back(text(path));
  }
  return result;
}

QStringList localImagePaths(const PromptSubmission &submission) {
  QStringList result;
  for (const AttachmentDraft &attachment : submission.attachments)
    if (attachment.mimeType.startsWith(QStringLiteral("image/")))
      result.push_back(attachment.path);
  return result;
}

QString joinedStrings(const nlohmann::json &value) {
  if (!value.is_array())
    return {};
  QStringList result;
  for (const nlohmann::json &entry : value)
    if (entry.is_string())
      result.push_back(text(entry.get<std::string>()));
  return result.join(QStringLiteral(", "));
}

QStringList stringList(const nlohmann::json &value) {
  QStringList result;
  if (!value.is_array())
    return result;
  for (const nlohmann::json &entry : value)
    if (entry.is_string())
      result.push_back(text(entry.get<std::string>()));
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
  VisibleCardData result{
      std::move(visualKey), CardKind::GenericActivity,
      identity.threadId,    identity.turnId,
      identity.itemId,      GenericActivityData{text(type), item}};

  if (type == "userMessage") {
    result.kind = CardKind::UserMessage;
    result.payload =
        UserMessageData{messageText(item), messageImagePaths(item)};
  } else if (type == "agentMessage") {
    result.kind = CardKind::AgentMessage;
    result.payload = AgentMessageData{
        messageText(item), stringValue(item, "phase") == "final_answer"};
  } else if (type == "commandExecution") {
    result.kind = CardKind::CommandExecution;
    QString output = text(stringValue(item, "aggregatedOutput"));
    if (output.isEmpty())
      output = text(stringValue(item, "output"));
    if (!terminalOutputHasVisibleText(output))
      output.clear();
    std::optional<int> exitCode;
    const auto rawExitCode = item.find("exitCode");
    if (rawExitCode != item.end() && rawExitCode->is_number_integer())
      exitCode = rawExitCode->get<int>();
    result.payload =
        CommandExecutionData{text(stringValue(item, "command")), output,
                             text(stringValue(item, "status")),
                             text(stringValue(item, "cwd")), exitCode};
  } else if (type == "collabAgentToolCall" || type == "subAgentActivity") {
    result.kind = CardKind::AgentActivity;
    result.payload = AgentActivityData{
        text(stringValue(item, "tool")),
        text(stringValue(item, "status")),
        text(stringValue(item, "kind")),
        text(stringValue(item, "prompt")),
        text(stringValue(item, "resultText")),
        stringList(item.value("receiverThreadIds", nlohmann::json::array()))};
  } else if (type == "reasoning") {
    result.kind = CardKind::Reasoning;
    result.payload = ReasoningData{
        joinedStrings(item.value("summary", nlohmann::json::array()))};
  } else if (type == "fileChange") {
    result.kind = CardKind::FileChanges;
    const nlohmann::json changes =
        item.value("changes", nlohmann::json::array());
    result.payload = FileChangesData{
        text(stringValue(item, "status")),
        changes.is_array() ? static_cast<int>(changes.size()) : 0, changes};
  } else if (type == "plan") {
    const QString plan = messageText(item);
    if (!plan.isEmpty()) {
      result.kind = CardKind::Plan;
      result.payload = PlanData{plan};
    }
  }
  return result;
}

struct OrderedItem {
  AuthoritativeItemKey key;
  const ItemPresentation *presentation = nullptr;
};

std::vector<OrderedItem> orderedItems(const std::string &threadId,
                                      const ThreadPresentation *thread) {
  std::vector<OrderedItem> result;
  if (!thread)
    return result;
  for (const std::string &turnId : thread->turnOrder) {
    const auto turn = thread->turns.find(turnId);
    if (turn == thread->turns.end())
      continue;
    for (const std::string &itemId : turn->second.itemOrder) {
      const auto item = turn->second.items.find(itemId);
      if (item == turn->second.items.end())
        continue;
      result.push_back(
          {AuthoritativeItemKey{threadId, turnId, itemId}, &item->second});
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
};

std::size_t submissionPosition(
    const PromptSubmission &submission,
    const std::vector<OrderedItem> &authoritativeItems,
    std::optional<std::size_t> materializedIndex = std::nullopt) {
  if (submission.admissionAnchor) {
    const auto anchor = std::ranges::find(
        authoritativeItems, *submission.admissionAnchor, &OrderedItem::key);
    if (anchor != authoritativeItems.end())
      return static_cast<std::size_t>(
                 std::distance(authoritativeItems.begin(), anchor) + 1) *
             2;
  }
  if (materializedIndex)
    return *materializedIndex * 2 + 1;
  // No authoritative tail was known at admission. Until reconcile establishes
  // one, the prompt is a tail item rather than a synthetic history prefix.
  return authoritativeItems.size() * 2 + 2;
}

} // namespace

ConversationSnapshot ConversationProjection::project(
    const std::string &threadId, const ThreadPresentation *authoritativeThread,
    std::span<const PromptSubmission> localSubmissions,
    std::size_t authoritativeItemLimit, qint64 nowMilliseconds) {
  ConversationSnapshot result;
  result.threadId = threadId;

  const std::vector<OrderedItem> authoritativeItems =
      orderedItems(threadId, authoritativeThread);
  result.hiddenAuthoritativeItemCount =
      authoritativeItems.size() > authoritativeItemLimit
          ? authoritativeItems.size() - authoritativeItemLimit
          : 0;
  result.hasMore = result.hiddenAuthoritativeItemCount > 0;
  const std::size_t firstVisible = result.hiddenAuthoritativeItemCount;

  std::map<AuthoritativeItemKey, const PromptSubmission *> bindings;
  for (const PromptSubmission &submission : localSubmissions)
    if (submission.materializedItem)
      bindings.emplace(*submission.materializedItem, &submission);

  std::vector<ProjectedNode> nodes;
  nodes.reserve(authoritativeItems.size() - firstVisible +
                localSubmissions.size());
  for (std::size_t index = firstVisible; index < authoritativeItems.size();
       ++index) {
    const OrderedItem &item = authoritativeItems[index];
    const auto binding = bindings.find(item.key);
    if (binding != bindings.end() &&
        binding->second->localCardVisible(nowMilliseconds))
      continue;
    CardKey visualKey = item.key;
    if (binding != bindings.end())
      visualKey = LocalPromptKey{binding->second->id};
    const std::size_t position =
        binding == bindings.end()
            ? index * 2 + 1
            : submissionPosition(*binding->second, authoritativeItems, index);
    const std::uint64_t tieBreaker =
        binding == bindings.end() ? 0 : binding->second->admissionOrdinal;
    nodes.push_back({position, tieBreaker,
                     sectionComponent("turn:", threadId, item.key.turnId),
                     item.key.turnId,
                     authoritativeCard(item.key, *item.presentation,
                                       std::move(visualKey))});
  }

  for (const PromptSubmission &submission : localSubmissions) {
    if (!submission.localCardVisible(nowMilliseconds))
      continue;
    std::optional<std::size_t> materializedIndex;
    if (submission.materializedItem) {
      const auto materialized = std::ranges::find(
          authoritativeItems, *submission.materializedItem, &OrderedItem::key);
      if (materialized != authoritativeItems.end())
        materializedIndex = static_cast<std::size_t>(
            std::distance(authoritativeItems.begin(), materialized));
    }
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
        knownTurn ? sectionComponent("turn:", threadId, turnId)
                  : "pending:" + std::to_string(submission.id);
    VisibleCardData card{
        LocalPromptKey{submission.id},
        CardKind::LocalPrompt,
        threadId,
        turnId,
        {},
        LocalPromptData{submission.id, submission.prompt,
                        submission.state == PromptState::Queued
                            ? PromptState::InFlight
                            : submission.state,
                        submission.acceptedAtMilliseconds, submission.error,
                        localImagePaths(submission)}};
    nodes.push_back({position, submission.admissionOrdinal, sectionKey, turnId,
                     std::move(card)});
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
      result.sections.push_back({node.sectionKey, node.turnId, {}});
      section = sectionIndexes.find(node.sectionKey);
    }
    result.sections[section->second].cards.push_back(std::move(node.card));
  }
  return result;
}

} // namespace codexui::codex::middle
