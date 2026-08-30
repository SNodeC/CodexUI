// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UI_UIVIEWSTATE_H
#define CODEXUI_CODEX_UI_UIVIEWSTATE_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace codexui::codex::ui {

// Toolkit-neutral inputs for the concrete thread-list renderer. Expansion,
// sorting, and optimistic rows deliberately remain local to that renderer.
struct ThreadListRow {
  std::string id;
  std::string title;
  std::string cwd;
  std::string status;
  std::optional<std::int64_t> createdAt;
  std::optional<std::int64_t> updatedAt;
  std::optional<std::int64_t> recencyAt;
  std::size_t pending = 0;
  bool archived = false;
  std::vector<ThreadListRow> children;

  bool operator==(const ThreadListRow &) const = default;
};

struct ThreadListSnapshot {
  std::string selectedThreadId;
  bool providerReady = false;
  bool canControl = false;
  std::vector<ThreadListRow> roots;

  bool operator==(const ThreadListSnapshot &) const = default;
};

struct InspectorPlanStep {
  std::string step;
  std::string status;

  bool operator==(const InspectorPlanStep &) const = default;
};

struct InspectorPlan {
  std::string explanation;
  std::vector<InspectorPlanStep> steps;

  bool operator==(const InspectorPlan &) const = default;
};

struct InspectorPlanSnapshot {
  std::string threadId;
  bool threadPresent = false;
  std::optional<InspectorPlan> plan;
  std::optional<std::string> planItem;

  bool operator==(const InspectorPlanSnapshot &) const = default;
};

struct InspectorAgentRow {
  std::string id;
  std::string status;
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

struct InspectorAgentsSnapshot {
  std::string threadId;
  bool threadPresent = false;
  std::vector<InspectorAgentRow> agents;

  bool operator==(const InspectorAgentsSnapshot &) const = default;
};

struct InspectorRequestRow {
  std::string id;
  std::string kind;
  std::string threadContext;
  std::uint64_t generation = 0;
  std::string command;
  std::string reason;
  std::string message;
  std::optional<std::size_t> questionCount;
  bool actionable = false;

  bool operator==(const InspectorRequestRow &) const = default;
};

struct InspectorRequestsSnapshot {
  std::vector<InspectorRequestRow> requests;

  bool operator==(const InspectorRequestsSnapshot &) const = default;
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
  InspectorPlanSnapshot plan;
  InspectorAgentsSnapshot agents;
  InspectorChangesSnapshot changes;
  InspectorRequestsSnapshot requests;
  InspectorStateSnapshot state;

  bool operator==(const InspectorSnapshot &) const = default;
};

} // namespace codexui::codex::ui

#endif // CODEXUI_CODEX_UI_UIVIEWSTATE_H
