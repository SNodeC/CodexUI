// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H
#define CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H

#include <nlohmann/json.hpp>

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace codexui::codex::middle {

inline constexpr qint64 AcknowledgementTransitionMilliseconds = 500;
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
[[nodiscard]] bool terminalOutputHasVisibleText(QStringView output);
[[nodiscard]] QString trimTrailingEmptyLines(QStringView text);

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
  QString text;
  QStringList imagePaths;

  bool operator==(const UserMessageData &) const = default;
};

struct AgentMessageData {
  QString text;
  bool finalAnswer = false;

  bool operator==(const AgentMessageData &) const = default;
};

struct CommandExecutionData {
  QString command;
  QString output;
  QString status;
  QString cwd;
  std::optional<int> exitCode;
  std::optional<qint64> durationMilliseconds;

  bool operator==(const CommandExecutionData &) const = default;
};

struct AgentActivityData {
  QString tool;
  QString status;
  QString kind;
  QString prompt;
  QString resultText;
  QStringList receivers;
  QString model;
  QString reasoningEffort;
  QString childThreadId;
  QString agentPath;
  QString senderThreadId;

  bool operator==(const AgentActivityData &) const = default;
};

struct ReasoningData {
  QString summary;

  bool operator==(const ReasoningData &) const = default;
};

struct FileChangeData {
  QString path;
  QString kind;
  std::optional<int> additions;
  std::optional<int> deletions;

  bool operator==(const FileChangeData &) const = default;
};

struct FileChangesData {
  QString status;
  std::vector<FileChangeData> changes;

  bool operator==(const FileChangesData &) const = default;
};

struct ImageGenerationData {
  QString path;
  QString status;
  QString revisedPrompt;

  bool operator==(const ImageGenerationData &) const = default;
};

struct PlanStepData {
  QString text;
  QString status;

  bool operator==(const PlanStepData &) const = default;
};

struct PlanData {
  QString explanation;
  std::vector<PlanStepData> steps;
  QString legacyText;

  bool operator==(const PlanData &) const = default;
};

struct GenericActivityData {
  QString type;
  nlohmann::json raw = nlohmann::json::object();

  bool operator==(const GenericActivityData &) const = default;
};

struct LocalPromptData {
  std::uint64_t submissionId = 0;
  QString prompt;
  PromptState state = PromptState::Queued;
  qint64 acceptedAtMilliseconds = 0;
  QString error;
  QStringList imagePaths;

  [[nodiscard]] bool
  acceptedTransitionActive(qint64 nowMilliseconds) const noexcept {
    return state == PromptState::Accepted && acceptedAtMilliseconds > 0 &&
           nowMilliseconds >= acceptedAtMilliseconds &&
           nowMilliseconds - acceptedAtMilliseconds <
               AcknowledgementTransitionMilliseconds;
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

  bool operator==(const VisibleCardData &) const = default;
};

// A section is a structural, visually transparent turn container. It contains
// only data that can affect the conversation presentation; turn lifecycle
// metadata belongs to the authoritative model and inspector.
struct TurnSection {
  std::string key;
  std::string turnId;
  std::vector<VisibleCardData> cards;

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
