// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H
#define CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace codexui::codex::middle {

inline constexpr std::int64_t PendingAnimationDelayMilliseconds = 1000;
inline constexpr std::size_t AuthoritativeHistoryPageSize = 80;

struct AuthoritativeItemKey {
  std::string threadId;
  std::string turnId;
  std::string itemId;

  auto operator<=>(const AuthoritativeItemKey &) const = default;
};

// Submission identifiers are process-wide and deliberately independent of a
// thread identifier. A locally admitted prompt therefore keeps its visual key
// when a new-thread draft receives its authoritative server thread id.
struct LocalPromptKey {
  std::uint64_t submissionId = 0;

  auto operator<=>(const LocalPromptKey &) const = default;
};

struct TurnPlanKey {
  std::string threadId;
  std::string turnId;

  auto operator<=>(const TurnPlanKey &) const = default;
};

using CardKey = std::variant<AuthoritativeItemKey, TurnPlanKey, LocalPromptKey>;

[[nodiscard]] std::string stableKey(const CardKey &key);
[[nodiscard]] bool terminalOutputHasVisibleText(std::string_view output);
[[nodiscard]] std::string trimUnicodeWhitespace(std::string_view text);
[[nodiscard]] std::string trimTrailingEmptyLines(std::string_view text);

enum class PromptState { Queued, InFlight, Accepted, Failed };

enum class CardKind {
  UserMessage,
  AgentMessage,
  CommandExecution,
  AgentActivity,
  Reasoning,
  FileChanges,
  ImageGeneration,
  Plan,
  GenericActivity,
  LocalPrompt,
};

struct UserMessageData {
  std::string text;
  std::vector<std::string> imagePaths;

  bool operator==(const UserMessageData &) const = default;
};

struct AgentMessageData {
  std::string text;
  bool finalAnswer = false;

  bool operator==(const AgentMessageData &) const = default;
};

struct CommandExecutionData {
  std::string command;
  std::string output;
  std::string status;
  std::string cwd;
  std::optional<int> exitCode;
  std::optional<std::int64_t> durationMilliseconds;

  bool operator==(const CommandExecutionData &) const = default;
};

struct AgentActivityData {
  std::string tool;
  std::string status;
  std::string kind;
  std::string prompt;
  std::string resultText;
  std::vector<std::string> receivers;
  std::string model;
  std::string reasoningEffort;
  std::string childThreadId;
  std::string agentPath;
  std::string senderThreadId;

  bool operator==(const AgentActivityData &) const = default;
};

struct ReasoningData {
  std::string summary;

  bool operator==(const ReasoningData &) const = default;
};

struct FileChangeData {
  std::string path;
  std::string kind;
  std::optional<int> additions;
  std::optional<int> deletions;

  bool operator==(const FileChangeData &) const = default;
};

struct FileChangesData {
  std::string status;
  std::vector<FileChangeData> changes;

  bool operator==(const FileChangesData &) const = default;
};

struct ImageGenerationData {
  std::string path;
  std::string status;
  std::string revisedPrompt;

  bool operator==(const ImageGenerationData &) const = default;
};

struct PlanStepData {
  std::string text;
  std::string status;

  bool operator==(const PlanStepData &) const = default;
};

struct PlanData {
  std::string explanation;
  std::vector<PlanStepData> steps;
  std::string legacyText;

  bool operator==(const PlanData &) const = default;
};

struct GenericActivityData {
  std::string type;
  nlohmann::json raw = nlohmann::json::object();
  std::string status;

  bool operator==(const GenericActivityData &) const = default;
};

struct LocalPromptData {
  std::uint64_t submissionId = 0;
  std::string prompt;
  PromptState state = PromptState::Queued;
  bool showPendingAnimation = false;
  std::string error;
  std::vector<std::string> imagePaths;

  bool operator==(const LocalPromptData &) const = default;
};

using CardPayload =
    std::variant<UserMessageData, AgentMessageData, CommandExecutionData,
                 AgentActivityData, ReasoningData, FileChangesData, PlanData,
                 ImageGenerationData, GenericActivityData, LocalPromptData>;

struct VisibleCardData {
  CardKey key;
  CardKind kind = CardKind::GenericActivity;
  std::string threadId;
  std::string turnId;
  std::string itemId;
  CardPayload payload = GenericActivityData{};

  bool operator==(const VisibleCardData &) const = default;
};

// A section is a structural, visually transparent turn container. It contains
// only data that can affect the conversation presentation; turn lifecycle
// metadata belongs to the authoritative model and inspector.
struct TurnSection {
  std::string key;
  std::string turnId;
  std::vector<VisibleCardData> cards;
  // The projection, which sees the complete authoritative turn, identifies
  // its actual opening prompt. Rendering must never infer ownership from the
  // first user message that happens to survive history paging.
  std::optional<CardKey> rootCardKey;

  bool operator==(const TurnSection &) const = default;
};

struct ConversationSnapshot {
  std::string threadId;
  std::vector<TurnSection> sections;
  std::size_t hiddenAuthoritativeItemCount = 0;
  bool hasMore = false;
  std::optional<std::string> activeTurnId;

  [[nodiscard]] std::vector<CardKey> cardKeys() const;
  [[nodiscard]] const VisibleCardData *find(const CardKey &key) const noexcept;

  bool operator==(const ConversationSnapshot &) const = default;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H
