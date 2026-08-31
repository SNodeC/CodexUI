// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UISESSION_H
#define CODEXUI_CODEX_UISESSION_H

#include "codex/AttachmentDraft.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/PresentationClient.h"
#include "codex/middle/MiddleTypes.h"
#include "codex/ui/UiViewState.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace codexui::codex {

struct UiNotice {
  std::uint64_t id = 0;
  std::string message;
  bool error = true;

  bool operator==(const UiNotice &) const = default;
};

struct UiNewThreadDraft {
  std::string workspace;
  std::string name;
  std::string baseInstructions;
  std::string developerInstructions;
  bool ephemeral = false;
};

struct UiPromptDraft {
  std::string text;
  std::vector<AttachmentDraft> attachments;
  nlohmann::json turnStartOptions = nlohmann::json::object();
  nlohmann::json threadStartOptions = nlohmann::json::object();
  std::string workspace;
  std::string visiblySelectedThreadId;
};

struct UiSettingsView {
  std::string identity;
  nlohmann::json canonical = nlohmann::json::object();
  nlohmann::json modelCatalog = nlohmann::json::array();
  nlohmann::json permissionProfiles = nlohmann::json::array();
  std::uint64_t settingsRevision = 0;
  nlohmann::json settingsUpdate = nlohmann::json::object();

  bool operator==(const UiSettingsView &) const = default;
};

struct UiPendingRequestView {
  std::string id;
  std::string kind;
  std::string threadId;
  std::uint64_t generation = 0;
  nlohmann::json raw = nlohmann::json::object();
  std::string title;
  std::string detail;
  std::string directAcceptLabel;
  bool supportsDirectAccept = false;
  bool actionable = false;

  bool operator==(const UiPendingRequestView &) const = default;
};

struct UiStatusView {
  bool connected = false;
  bool retrying = false;
  std::string role;
  std::string providerState;
  std::string selectedTransport;
  std::string workspace;
  bool activeTurn = false;
  std::size_t selectedPending = 0;
  std::size_t totalPending = 0;
  bool canSubmit = false;
  bool canEditSettings = false;
  nlohmann::json connectionSettings = nlohmann::json::object();

  bool operator==(const UiStatusView &) const = default;
};

enum class UiConversationMode { NoSelection, NewThread, Thread };

struct UiConversationView {
  UiConversationMode mode = UiConversationMode::NoSelection;
  std::string key;
  std::string title;
  std::string workspace;
  std::string status;
  std::string statusTone;
  std::optional<std::int64_t> lastActivityAt;
  std::string emptyMessage;
  middle::ConversationSnapshot snapshot;

  bool operator==(const UiConversationView &) const = default;
};

enum class UiOptimisticThreadPhase { Awaiting, Confirmed, Failed };

struct UiOptimisticThreadView {
  std::string key;
  std::string threadId;
  std::string title;
  std::string workspace;
  UiOptimisticThreadPhase phase = UiOptimisticThreadPhase::Awaiting;

  bool operator==(const UiOptimisticThreadView &) const = default;
};

struct UiSessionView {
  std::string selectedThreadId;
  bool newThreadIntent = false;
  ui::ThreadListSnapshot threads;
  UiConversationView conversation;
  ui::InspectorSnapshot inspector;
  UiSettingsView settings;
  UiStatusView status;
  std::optional<UiOptimisticThreadView> optimisticThread;
  std::vector<UiPendingRequestView> pendingRequests;
  std::optional<UiPendingRequestView> selectedPendingRequest;

  bool operator==(const UiSessionView &) const = default;
};

enum class UiEffect {
  ClearComposerDraft,
  FocusComposer,
  PrepareLocalPromptAdmission,
};

// Authoritative UI/UX state owner.  It is called on the existing GUI thread in
// this refactor.  The class itself is toolkit-neutral and talks downward only
// through PresentationClient's generic presentation-protocol API.
class UiSession final {
public:
  using Clock = std::function<std::int64_t()>;
  using ChangedHandler = std::function<void()>;
  using WakeupHandler = std::function<void(std::int64_t)>;
  using ProtocolFrameObserver =
      std::function<void(const nlohmann::json &)>;

  explicit UiSession(PresentationClient client, std::string defaultWorkspace,
                     Clock clock = {});
  ~UiSession();

  UiSession(const UiSession &) = delete;
  UiSession &operator=(const UiSession &) = delete;

  void setChangedHandler(ChangedHandler handler);
  void setWakeupHandler(WakeupHandler handler);
  void setProtocolFrameObserver(ProtocolFrameObserver observer);

  void onPresentationFrame(const nlohmann::json &frame);
  void noteThreadActivity(const std::string &threadId);
  void notePromptActivity(const std::string &threadId);
  void tick();

  [[nodiscard]] std::string conversationKey() const;
  [[nodiscard]] const UiSessionView &
  refreshView(bool conversationFollowing, std::string draftWorkspace = {});
  [[nodiscard]] std::vector<UiNotice> takeNotices();
  [[nodiscard]] std::vector<UiEffect> takeEffects();

  void refreshThreads();
  void connectTransport();
  void disconnectTransport();
  void reconnectTransport();
  void configureConnection(nlohmann::json settings);
  void toggleController();

  void selectThread(std::string threadId);
  void reloadThread(const std::string &threadId);
  void beginNewThread(UiNewThreadDraft draft);
  void renameThread(const std::string &threadId, std::string name);
  void forkThread(const std::string &threadId);
  void toggleThreadArchive(const std::string &threadId);
  void deleteThread(const std::string &threadId);

  [[nodiscard]] bool submitPrompt(UiPromptDraft draft);
  void interruptTurn();
  void loadEarlierConversation();

  [[nodiscard]] bool
  resolvePending(UiPendingRequestView request,
                 PendingRequestResponse response);

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_UISESSION_H
