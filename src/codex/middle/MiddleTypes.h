// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H
#define CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H

#include "codex/UiStatus.h"
#include "codex/nodegraph/NodeGraph.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace codexui::codex::middle {

inline constexpr std::int64_t PendingAnimationDelayMilliseconds = 1000;
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

using CardKey = std::variant<AuthoritativeItemKey, LocalPromptKey>;

[[nodiscard]] std::string stableKey(const CardKey &key);
[[nodiscard]] bool terminalOutputHasVisibleText(std::string_view output);
[[nodiscard]] std::string trimUnicodeWhitespace(std::string_view text);
[[nodiscard]] bool
hasTextAfterTrimmingTrailingEmptyLines(std::string_view text);
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
  std::string cwd;
  std::optional<int> exitCode;
  std::optional<std::int64_t> durationMilliseconds;

  bool operator==(const CommandExecutionData &) const = default;
};

struct AgentActivityData {
  std::string tool;
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
  std::vector<FileChangeData> changes;
  // Relative provider paths are resolved against the owning thread's current
  // workspace only when the user explicitly asks the desktop to open them.
  std::string cwd;

  bool operator==(const FileChangesData &) const = default;
};

struct ImageGenerationData {
  std::string path;
  std::string revisedPrompt;

  bool operator==(const ImageGenerationData &) const = default;
};

struct PlanStepData {
  std::string text;
  UiStatus status;

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
  // Adapter-owned readable tree: 4000 UTF-8 bytes plus truncation notice.
  std::string displayDetail;

  bool operator==(const GenericActivityData &) const = default;
};

struct LocalPromptData {
  std::uint64_t submissionId = 0;
  std::string prompt;
  PromptState state = PromptState::Queued;
  std::string error;
  std::vector<std::string> imagePaths;
  std::optional<std::int64_t> admittedAtMs;
  bool requiresExplicitRecovery = false;

  bool awaitingConversation() const noexcept {
    return state != PromptState::Failed;
  }
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
  UiStatus status;
  // Stable action/lifetime identity supplied by the adapter. Widgets retain
  // it but never inspect graph state through it.
  nodegraph::NodeRef target;

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
  // its actual opening prompt. Rendering must never infer ownership from row
  // position.
  std::optional<CardKey> rootCardKey;

  bool operator==(const TurnSection &) const = default;
};

[[nodiscard]] inline bool
isNestedTurnCard(std::optional<std::size_t> rootPosition,
                 std::size_t cardPosition) noexcept {
  return rootPosition && cardPosition > *rootPosition;
}

// Exact placement facts for one conversation row. They carry no authority:
// the NodeRef target and all values are read from NodeGraph under one short
// lock, then consumed by Qt after the lock has been released.
struct ConversationRowPlacement {
  VisibleCardData card;
  std::string sectionKey;
  bool turnRoot = false;
  bool nested = false;
  bool activeTurn = false;
};

// One canonical row plus its immediate presented neighbors. The neighboring
// keys are positioning facts only; NodeGraph remains the source of both the
// row values and their order.
struct ConversationRowChange {
  ConversationRowPlacement placement;
  std::optional<CardKey> previousCardKey;
  std::optional<CardKey> nextCardKey;
};

struct PromptMaterialization {
  CardKey cardKey;
  nodegraph::NodeRef prompt;

  bool operator==(const PromptMaterialization &) const = default;
};

// One lock-free value transaction from the graph projection to the item view.
// Presentation-only changes remain separately budgetable from structural
// placement. Each structural row carries final canonical neighbors, and rows
// whose changed neighbors depend on one another are dependency-ordered;
// removals are exact graph identities.
struct ConversationDelta {
  std::string threadId;
  std::vector<VisibleCardData> presentations;
  std::vector<ConversationRowChange> rows;
  std::vector<nodegraph::NodeRef> removals;
  std::vector<PromptMaterialization> materializedPrompts;
  bool providerHasMore = false;
};

struct ConversationSnapshot {
  std::string threadId;
  std::vector<TurnSection> sections;
  std::vector<PromptMaterialization> materializedPrompts;
  // Provider continuation only. Every item already loaded in NodeGraph is
  // represented in sections; QWidget residency remains independently bounded.
  bool hasMore = false;
  std::optional<std::string> activeTurnId;

  [[nodiscard]] std::vector<CardKey> cardKeys() const;
  [[nodiscard]] const VisibleCardData *find(const CardKey &key) const noexcept;

  bool operator==(const ConversationSnapshot &) const = default;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H
