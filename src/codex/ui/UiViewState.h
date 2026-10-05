// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UI_UIVIEWSTATE_H
#define CODEXUI_CODEX_UI_UIVIEWSTATE_H

#include "codex/PendingRequestPolicy.h"
#include "codex/middle/MiddleTypes.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace codexui::codex::ui {

struct TokenUsageText {
  std::string summary;
  std::string details;
  bool operator==(const TokenUsageText &) const = default;
};

struct TokenUsageSnapshot {
  TokenUsageText thread;
  TokenUsageText overall;
  bool operator==(const TokenUsageSnapshot &) const = default;
};

enum class InspectorProjection {
  Plan,
  Agents,
  Changes,
  Requests,
  State,
  Protocol,
  Timing
};
inline constexpr std::size_t MaximumInspectorRows = 50;

struct InspectorRowRequest final {
  std::size_t first = 0;
  std::size_t count = MaximumInspectorRows;
  std::string anchorKey;
  std::string focusedKey;
  std::vector<std::string> retainedKeys;
  nodegraph::NodeRef timingTarget{};
};

inline std::string threadPresentationKey(std::string_view threadId,
                                         std::string_view correlation) {
  return correlation.empty() ? std::string(threadId)
                             : "creation:" + std::string(correlation);
}

// Toolkit-neutral inputs for the concrete thread-list renderer. Expansion
// and optimistic rows deliberately remain local to that renderer.
enum class ThreadGrouping { Projects, Sections, Ungrouped };
enum class ThreadRowKind { Project, Section, Thread, Page };
struct ThreadBrowserOptions {
  ThreadGrouping grouping = ThreadGrouping::Projects;
  std::string title;
  int archived = 0; // 0 active, 1 archived, 2 all
  bool manualSections = false;
  bool manualProjects = false;
};

struct ThreadGroup {
  std::string id;
  std::string label; // Display only; fields["name"] retains the authored name.
  nodegraph::NodeRef target;
  nlohmann::json fields;

  bool operator==(const ThreadGroup &) const = default;
};

struct ThreadListRow {
  std::string id;
  std::string presentationKey;
  nodegraph::NodeRef target;
  std::string title;
  std::string cwd;
  UiStatus status;
  std::optional<std::int64_t> createdAt;
  std::optional<std::int64_t> updatedAt;
  std::optional<std::int64_t> recencyAt;
  std::optional<std::int64_t> lastActivityAt;
  std::optional<std::int64_t> pendingPromptAdmittedAtMs;
  std::size_t pending = 0;
  bool awaitingPromptConversation = false;
  bool archived = false;
  std::vector<ThreadListRow> children;
  ProtocolTimes timing{};
  ThreadRowKind kind = ThreadRowKind::Thread;
  std::string projectId;
  std::string sectionId;
  nodegraph::NodeRef parentThread;
  std::string details;
  std::int64_t order = 0;
  nlohmann::json query;
  bool pendingPage = false;
  std::string appearanceColor;

  bool operator==(const ThreadListRow &) const = default;
};

struct ThreadListSnapshot {
  std::string selectedThreadId;
  bool providerReady = false;
  bool canControl = false;
  std::vector<ThreadListRow> roots;
  std::vector<ThreadGroup> projects;
  std::vector<ThreadGroup> sections;
  std::string serverIdentity;
  bool groupingAvailable = false;

  bool operator==(const ThreadListSnapshot &) const = default;
};

struct InspectorAgentRow {
  UiStatus status;
  std::string childThreadId;
  std::string agentPath;
  std::string tool;
  std::string model;
  std::string reasoningEffort;
  std::string prompt;
  std::string resultText;
  std::string senderThreadId;
  std::vector<std::string> receiverThreadIds;

  bool operator==(const InspectorAgentRow &) const = default;
};

struct InspectorMarkdownRow {
  std::string text;
  std::string accessibleName;
  std::string preparedHtml;

  bool operator==(const InspectorMarkdownRow &other) const {
    return text == other.text && accessibleName == other.accessibleName;
  }
};

using InspectorRowValue =
    std::variant<InspectorMarkdownRow, middle::PlanStepData, InspectorAgentRow,
                 PendingRequestDescriptor>;

struct InspectorRow {
  std::string key;
  InspectorRowValue value;

  bool operator==(const InspectorRow &) const = default;
};

struct InspectorPageSnapshot {
  std::size_t first = 0;
  std::size_t total = 0;
  std::uint64_t orderRevision = 0;
  std::string emptyMessage;
  std::optional<std::size_t> anchorIndex;
  std::optional<std::size_t> focusedIndex;
  std::vector<InspectorRow> rows;
  std::optional<InspectorRow> focused;
  std::optional<std::vector<std::string>> validRetainedKeys;

  bool operator==(const InspectorPageSnapshot &) const = default;
};

struct PendingRequestsSummary {
  std::size_t total = 0;
  std::vector<PendingRequestDescriptor> candidates;
};

struct InspectorChangesSnapshot {
  std::string threadId;
  std::string cwd;
  std::vector<std::string> commandCwds;
  std::vector<std::string> changedPaths;

  bool operator==(const InspectorChangesSnapshot &) const = default;
};

struct InspectorStateSnapshot {
  nlohmann::json state = nlohmann::json::object();
  std::size_t threadCount = 0;
  std::size_t modelCount = 0;
  std::size_t selectedThreadTurnCount = 0;
  std::size_t selectedThreadItemCount = 0;
  std::size_t pendingRequestCount = 0;
  std::size_t telemetryCount = 0;

  bool operator==(const InspectorStateSnapshot &) const = default;
};

struct InspectorSnapshot {
  std::uint64_t threadIncarnation = 0;
  InspectorPageSnapshot plan;
  InspectorPageSnapshot agents;
  InspectorChangesSnapshot changes;
  InspectorPageSnapshot requests;
  InspectorStateSnapshot state;
  ProtocolTimes timing{};
  std::string timingTitle{};

  bool operator==(const InspectorSnapshot &) const = default;
};

} // namespace codexui::codex::ui

#endif // CODEXUI_CODEX_UI_UIVIEWSTATE_H
