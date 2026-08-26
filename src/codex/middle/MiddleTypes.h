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

using CardKey = std::variant<AuthoritativeItemKey, LocalPromptKey>;

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

  bool operator==(const CommandExecutionData &) const = default;
};

struct AgentActivityData {
  QString tool;
  QString status;
  QString kind;
  QString prompt;
  QString resultText;
  QStringList receivers;

  bool operator==(const AgentActivityData &) const = default;
};

struct ReasoningData {
  QString summary;

  bool operator==(const ReasoningData &) const = default;
};

struct FileChangesData {
  QString status;
  int pathCount = 0;
  nlohmann::json changes = nlohmann::json::array();

  // The conversation card shows only status and path count. Diff contents are
  // owned by the Changes inspector and must not turn a visually identical
  // conversation projection into a layout mutation.
  bool operator==(const FileChangesData &other) const {
    return status == other.status && pathCount == other.pathCount;
  }
};

struct PlanData {
  QString text;

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
  int attachmentCount = 0;
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
                 GenericActivityData, LocalPromptData>;

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

  [[nodiscard]] std::vector<CardKey> cardKeys() const;
  [[nodiscard]] const VisibleCardData *find(const CardKey &key) const noexcept;

  bool operator==(const ConversationSnapshot &) const = default;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_MIDDLETYPES_H
