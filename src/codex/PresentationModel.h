// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PRESENTATIONMODEL_H
#define CODEXUI_CODEX_PRESENTATIONMODEL_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace codexui::codex {

struct ItemPresentation {
  std::string id;
  nlohmann::json raw = nlohmann::json::object();
  std::unordered_map<std::string, nlohmann::json> domains;
};

struct TurnPresentation {
  std::string id;
  std::string status;
  std::vector<std::string> itemOrder;
  std::unordered_map<std::string, ItemPresentation> items;
  nlohmann::json plan = nlohmann::json::object();
  nlohmann::json raw = nlohmann::json::object();
  std::unordered_map<std::string, nlohmann::json> domains;
};

struct AgentPresentation {
  std::string id;
  std::string itemId;
  std::string ownerTurnId;
  std::string childThreadId;
  std::string status;
  nlohmann::json raw = nlohmann::json::object();
};

struct ThreadPresentation {
  std::string id;
  std::string title;
  std::string preview;
  std::string cwd;
  std::string status;
  std::vector<std::string> turnOrder;
  std::unordered_map<std::string, TurnPresentation> turns;
  nlohmann::json raw = nlohmann::json::object();
  std::unordered_map<std::string, nlohmann::json> domains;
  std::vector<std::string> agentOrder;
  std::unordered_map<std::string, AgentPresentation> agents;
  bool archived = false;
  bool agentThread = false;
};

struct PendingRequestPresentation {
  std::string id;
  std::string kind;
  std::string threadId;
  std::uint64_t generation = 0;
  nlohmann::json raw;
};

struct ConnectionPresentation {
  bool connected = false;
  bool retrying = false;
  std::uint64_t generation = 0;
  std::string connectionId;
  std::string role;
  std::string controllerConnectionId;
  std::string detail;
  nlohmann::json settings = nlohmann::json::object();
};

struct TelemetryPresentation {
  std::uint64_t sequence = 0;
  std::uint64_t generation = 0;
  std::string type;
  nlohmann::json data = nlohmann::json::object();
  nlohmann::json scope = nlohmann::json::object();
};

class PresentationModel final {
public:
  void applyEvent(const nlohmann::json &event);

  [[nodiscard]] const std::vector<std::string> &threadOrder() const noexcept;
  [[nodiscard]] const ThreadPresentation *
  thread(const std::string &threadId) const noexcept;
  [[nodiscard]] std::optional<std::string>
  activeTurnId(const std::string &threadId) const;
  [[nodiscard]] std::size_t pendingRequestCount() const noexcept;
  [[nodiscard]] std::size_t
  pendingRequestCount(const std::string &threadId) const noexcept;
  [[nodiscard]] const ConnectionPresentation &connection() const noexcept;
  [[nodiscard]] const nlohmann::json &modelCatalog() const noexcept;
  [[nodiscard]] const std::unordered_map<std::string, nlohmann::json> &
  globalDomains() const noexcept;
  [[nodiscard]] const std::vector<TelemetryPresentation> &
  telemetry() const noexcept;
  [[nodiscard]] const std::unordered_map<std::string,
                                         PendingRequestPresentation> &
  pendingRequestPresentations() const noexcept;

private:
  void mergeThreadList(const nlohmann::json &listedThreads);
  ThreadPresentation &upsertThread(const nlohmann::json &raw,
                                   bool replaceTurns);
  TurnPresentation &upsertTurn(ThreadPresentation &thread,
                               const nlohmann::json &raw, bool replaceItems);
  ItemPresentation &upsertItem(ThreadPresentation &thread,
                               TurnPresentation &turn,
                               const nlohmann::json &raw, bool live = false);
  void upsertAgentActivity(ThreadPresentation &owner,
                           const nlohmann::json &scope,
                           const nlohmann::json &activity, bool live = true);
  void correlateAgentThread(const std::string &childThreadId);
  void removeThread(const std::string &threadId);
  void retainDomainEvent(const std::string &type, const nlohmann::json &data,
                         const nlohmann::json &scope,
                         const std::string &authority);
  TurnPresentation *findTurn(const std::string &threadId,
                             const std::string &turnId);
  ItemPresentation *findItem(const nlohmann::json &params);

  std::vector<std::string> orderedThreads;
  std::unordered_map<std::string, ThreadPresentation> threads;
  std::unordered_map<std::string, PendingRequestPresentation> pendingRequests;
  ConnectionPresentation connectionState;
  nlohmann::json models = nlohmann::json::array();
  std::unordered_map<std::string, nlohmann::json> retainedGlobalDomains;
  std::vector<TelemetryPresentation> retainedTelemetry;
  std::uint64_t lastSequence = 0;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PRESENTATIONMODEL_H
