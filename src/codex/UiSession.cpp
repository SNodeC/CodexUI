// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/UiSession.h"

#include "codex/PresentationModel.h"
#include "codex/PresentationProtocol.h"
#include "codex/PresentationStatus.h"
#include "codex/middle/ConversationProjection.h"
#include "codex/middle/PromptCoordinator.h"
#include "codex/ui/UiViewProjection.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace codexui::codex {
namespace {

constexpr std::string_view DraftThreadId = "draft:new-thread";

std::int64_t systemClockMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(key);
  return found != object.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

std::string safeMessage(const nlohmann::json &value) {
  std::string message = stringValue(value, "message");
  if (message.empty())
    message = stringValue(value, "detail");
  if (!message.empty())
    return message;
  const auto error = value.find("error");
  return error != value.end() && error->is_object()
             ? stringValue(*error, "message")
             : std::string{};
}

bool isThreadNotFoundResult(const nlohmann::json &result) {
  if (result.value("ok", false))
    return false;
  std::string message =
      safeMessage(result.value("error", nlohmann::json::object()));
  std::ranges::transform(message, message.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return message.find("thread") != std::string::npos &&
         message.find("not found") != std::string::npos;
}

bool isTransientCancellation(const nlohmann::json &result) {
  return !result.value("ok", false) &&
         result.value("error", nlohmann::json::object())
             .value("transient", false);
}

std::optional<std::string> resultTurnId(const nlohmann::json &result) {
  const nlohmann::json scope = result.value("scope", nlohmann::json::object());
  std::string id = stringValue(scope, "turnId");
  if (!id.empty())
    return id;
  const nlohmann::json data = result.value("data", nlohmann::json::object());
  id = stringValue(data, "turnId");
  if (!id.empty())
    return id;
  const nlohmann::json turn = data.value("turn", nlohmann::json::object());
  id = stringValue(turn, "id");
  return id.empty() ? std::nullopt
                    : std::optional<std::string>{std::move(id)};
}

std::string trimAscii(std::string value) {
  const auto whitespace = [](unsigned char character) {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\f' || character == '\v';
  };
  const auto first = std::ranges::find_if_not(value, whitespace);
  if (first == value.end())
    return {};
  const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace);
  return std::string(first, last.base());
}

} // namespace

class UiSession::Impl final {
public:
  enum class Hydration { NotHydrated, InFlight, Hydrated, Failed };
  enum class SettingsHydration {
    Unknown,
    WaitingForRead,
    InFlight,
    Hydrated,
    Failed,
  };

  struct ThreadRuntimeState {
    Hydration hydration = Hydration::NotHydrated;
    SettingsHydration settingsHydration = SettingsHydration::Unknown;
    std::uint64_t readRevision = 0;
    bool operationReady = false;
    bool resumeInFlight = false;
    std::unordered_set<std::uint64_t> recoveryAttemptedSubmissions;

    void resetForConnection() noexcept {
      hydration = Hydration::NotHydrated;
      settingsHydration = SettingsHydration::Unknown;
      readRevision = 0;
      operationReady = false;
      resumeInFlight = false;
    }
  };

  struct HistoryWindow {
    std::size_t requested =
        middle::ConversationProjection::DefaultAuthoritativeItemLimit;
    std::size_t effective =
        middle::ConversationProjection::DefaultAuthoritativeItemLimit;
    std::size_t lastAuthoritativeCount = 0;
  };

  Impl(PresentationClient client, std::string defaultWorkspace,
       UiSession::Clock clock)
      : client(std::move(client)),
        defaultWorkspace(std::move(defaultWorkspace)),
        clock(clock ? std::move(clock) : UiSession::Clock{
                                             systemClockMilliseconds}),
        alive(std::make_shared<bool>(true)) {}

  ~Impl() { *alive = false; }

  [[nodiscard]] std::int64_t now() const { return clock(); }
  [[nodiscard]] std::int64_t nowSeconds() const { return now() / 1000; }

  void changed() {
    if (changedHandler)
      changedHandler();
  }

  void showNotice(std::string message, bool error = true) {
    if (message.empty())
      return;
    notices.push_back({nextNoticeId++, std::move(message), error});
    changed();
  }

  void scheduleWakeup(std::int64_t atMilliseconds) {
    if (!nextWakeupAt || atMilliseconds < *nextWakeupAt) {
      nextWakeupAt = atMilliseconds;
      if (wakeupHandler)
        wakeupHandler(atMilliseconds);
    }
  }

  [[nodiscard]] bool providerReady() const {
    const ConnectionPresentation &connection = model.connection();
    return connection.connected && connection.providerState == "ready";
  }

  [[nodiscard]] bool canControlProvider() const {
    return providerReady() && model.connection().role == "controller";
  }

  void resetRuntimeForConnection() {
    resolvingRequests.clear();
    deferredPromptDispatch.clear();
    for (auto &[threadId, runtime] : runtimeByThread) {
      static_cast<void>(threadId);
      runtime.resetForConnection();
    }
  }

  void hydrateProvider() {
    if (!providerReady())
      return;
    client.execute("threads.list", nlohmann::json::object());
    client.execute("models.list", nlohmann::json::object());
    ensureThreadHydrated(selectedThreadId);
    ensureThreadSettingsHydrated(selectedThreadId);
    for (const std::string &threadId : prompts.queuedThreadIds()) {
      if (threadId == DraftThreadId) {
        if (newThreadIntent)
          startThreadForDraft();
      } else {
        schedulePromptDispatch(threadId);
      }
    }
    client.execute("permission-profiles.list", {{"cwd", defaultWorkspace}});
  }

  void onPresentationFrame(const nlohmann::json &event) {
    if (protocolFrameObserver)
      protocolFrameObserver(event);
    const std::string kind = stringValue(event, "kind");
    const std::string action = stringValue(event, "action");
    const std::string correlationId = stringValue(event, "correlationId");
    const bool staleReadResult =
        kind == "result" && action == "thread.read" &&
        !correlationId.empty() &&
        staleReadResultCorrelations.erase(correlationId) > 0;
    if (!staleReadResult)
      model.applyEvent(event);

    const ConnectionPresentation &connection = model.connection();
    if (connection.generation != observedConnectionGeneration) {
      observedConnectionGeneration = connection.generation;
      resetRuntimeForConnection();
    }
    if (connection.providerGeneration != observedProviderGeneration) {
      observedProviderGeneration = connection.providerGeneration;
      resetRuntimeForConnection();
    }

    const std::string type = stringValue(event, "type");
    const nlohmann::json data =
        event.value("data", nlohmann::json::object());
    const nlohmann::json scope =
        event.value("scope", nlohmann::json::object());
    const std::string eventThreadId = stringValue(scope, "threadId");
    const bool hydrationResult =
        kind == "result" && presentation::isThreadHydrationAction(action);
    if (!eventThreadId.empty() && !hydrationResult)
      model.noteThreadActivity(eventThreadId, nowSeconds());
    if (kind == "event" && type == "pending-request.removed") {
      const auto requestId = scope.find("requestId");
      if (requestId != scope.end() && !requestId->is_null())
        resolvingRequests.erase(requestId->dump());
    }
    if (kind == "event" && type == "connection.provider" &&
        stringValue(data, "state") == "disconnected")
      resetRuntimeForConnection();

    if (kind == "result" && !event.value("ok", false) &&
        action != "turn.start" && action != "turn.steer" &&
        action != "thread.read" && action != "thread.resume") {
      const std::string message =
          safeMessage(event.value("error", nlohmann::json::object()));
      showNotice(message.empty() ? "Codex operation failed" : message);
    } else if (kind == "event" && type == "notice.added") {
      const nlohmann::json notice =
          data.value("notice", nlohmann::json::object());
      const std::string message = safeMessage(notice);
      if (!message.empty())
        showNotice(message, stringValue(data, "severity") == "error");
    } else if (kind == "event" && type == "system.diagnostic") {
      const std::string message = safeMessage(data);
      if (!message.empty())
        showNotice("Protocol diagnostic: " + message);
    } else if (kind == "event" && type == "connection.lifecycle" &&
               (stringValue(data, "state") == "failure" ||
                stringValue(data, "state") == "disconnected")) {
      const std::string detail = stringValue(data, "detail");
      if (!detail.starts_with("local-"))
        showNotice(detail.empty() ? "Codex bridge disconnected" : detail);
    }

    if (kind == "event" &&
        ((type == "connection.provider" &&
          stringValue(data, "state") == "ready") ||
         (type == "connection.bridge" &&
          stringValue(data, "state") == "opened" && providerReady())))
      hydrateProvider();
    if (kind == "event" && type == "connection.controller" &&
        providerReady() && model.connection().role == "controller") {
      ensureThreadSettingsHydrated(selectedThreadId);
      for (const std::string &threadId : prompts.queuedThreadIds())
        schedulePromptDispatch(threadId);
    }

    if (type == "thread.removed" && !eventThreadId.empty()) {
      prompts.clearThread(eventThreadId);
      runtimeByThread.erase(eventThreadId);
      historyWindows.erase(eventThreadId);
      if (selectedThreadId == eventThreadId) {
        selectedThreadId.clear();
        effects.push_back(UiEffect::ClearComposerDraft);
      }
    } else if (!eventThreadId.empty()) {
      if (const ThreadPresentation *thread = model.thread(eventThreadId))
        prompts.reconcile(eventThreadId, *thread, now());
    }

    if (!staleReadResult && kind == "result" && action == "thread.read" &&
        event.value("ok", false))
      hydrateHistoricalChildren(eventThreadId);
    else if (kind == "event" && type == "agents.activity.upsert")
      hydrateHistoricalChildren(eventThreadId);

    changed();
  }

  void noteThreadActivity(const std::string &threadId) {
    model.noteThreadActivity(threadId, nowSeconds());
    changed();
  }

  void hydrateHistoricalChildren(const std::string &parentThreadId,
                                 bool retryFailed = false) {
    const ThreadPresentation *thread = model.thread(parentThreadId);
    if (!thread)
      return;
    for (const std::string &childThreadId : thread->childThreadOrder) {
      const ChildThreadOwnership *ownership =
          model.childOwnership(childThreadId);
      if (!ownership || ownership->parentThreadId != parentThreadId)
        continue;
      const auto agent = thread->agents.find(ownership->agentId);
      if (agent == thread->agents.end() ||
          !isActiveStatus(agent->second.status))
        continue;
      const auto runtime = runtimeByThread.find(childThreadId);
      const bool failed = runtime != runtimeByThread.end() &&
                          runtime->second.hydration == Hydration::Failed;
      if (!failed || retryFailed)
        readThread(childThreadId, failed);
    }
  }

  void selectThread(std::string threadId) {
    if (threadId.empty())
      return;
    if (threadId == selectedThreadId) {
      hydrateThreadForSelection(threadId);
      return;
    }
    if (optimisticThread && optimisticThread->key == DraftThreadId &&
        prompts.submissions(std::string(DraftThreadId)).empty())
      optimisticThread.reset();
    selectedThreadId = std::move(threadId);
    newThreadIntent = false;
    newThreadOptions = nlohmann::json::object();
    newThreadName.clear();
    newThreadWorkspace.clear();
    historyWindows.try_emplace(selectedThreadId);
    hydrateThreadForSelection(selectedThreadId);
    changed();
  }

  void beginNewThread(UiNewThreadDraft draft) {
    if (newThreadCreationInFlight) {
      showNotice("The current new thread is still being created.", false);
      return;
    }
    prompts.clearThread(std::string(DraftThreadId));
    selectedThreadId.clear();
    newThreadIntent = true;
    newThreadName = std::move(draft.name);
    newThreadWorkspace = draft.workspace.empty()
                             ? defaultWorkspace
                             : std::move(draft.workspace);
    newThreadOptions = nlohmann::json::object();
    if (!draft.baseInstructions.empty())
      newThreadOptions["baseInstructions"] =
          std::move(draft.baseInstructions);
    if (!draft.developerInstructions.empty())
      newThreadOptions["developerInstructions"] =
          std::move(draft.developerInstructions);
    if (draft.ephemeral)
      newThreadOptions["ephemeral"] = true;
    optimisticThread = UiOptimisticThreadView{
        std::string(DraftThreadId), {},
        newThreadName.empty() ? "New thread" : newThreadName,
        newThreadWorkspace, UiOptimisticThreadPhase::Awaiting};
    effects.push_back(UiEffect::ClearComposerDraft);
    effects.push_back(UiEffect::FocusComposer);
    changed();
  }

  void readThread(const std::string &threadId, bool forced = false) {
    if (threadId.empty() || !providerReady())
      return;
    ThreadRuntimeState &runtime = runtimeByThread[threadId];
    if (runtime.resumeInFlight)
      return;
    if (!forced && (runtime.hydration == Hydration::InFlight ||
                    runtime.hydration == Hydration::Hydrated ||
                    runtime.hydration == Hydration::Failed))
      return;
    runtime.hydration = Hydration::InFlight;
    const auto token = alive;
    const std::uint64_t revision = nextReadRevision++;
    runtime.readRevision = revision;
    client.execute(
        "thread.read", {{"threadId", threadId}, {"includeTurns", true}},
        [this, token, threadId,
         revision](const nlohmann::json &result) {
          if (!*token)
            return;
          const auto current = runtimeByThread.find(threadId);
          if (current == runtimeByThread.end() ||
              current->second.readRevision != revision) {
            const std::string correlationId =
                stringValue(result, "correlationId");
            if (!correlationId.empty())
              staleReadResultCorrelations.insert(correlationId);
            return;
          }
          ThreadRuntimeState &runtime = current->second;
          if (result.value("ok", false)) {
            runtime.hydration = Hydration::Hydrated;
            if (runtime.settingsHydration ==
                SettingsHydration::WaitingForRead)
              resumeThreadForSettings(threadId);
            schedulePromptDispatch(threadId);
            return;
          }
          if (isTransientCancellation(result)) {
            runtime.hydration = Hydration::NotHydrated;
            if (runtime.settingsHydration ==
                SettingsHydration::WaitingForRead)
              runtime.settingsHydration = SettingsHydration::Unknown;
            return;
          }
          runtime.hydration = Hydration::Failed;
          if (runtime.settingsHydration == SettingsHydration::WaitingForRead)
            runtime.settingsHydration = SettingsHydration::Failed;
          const std::string message =
              safeMessage(result.value("error", nlohmann::json::object()));
          const std::string displayed =
              message.empty() ? "Thread loading failed" : message;
          static_cast<void>(prompts.failQueued(threadId, displayed));
          showNotice(displayed);
        });
  }

  void ensureThreadSettingsHydrated(const std::string &threadId) {
    if (threadId.empty() || !canControlProvider())
      return;
    ThreadRuntimeState &runtime = runtimeByThread[threadId];
    if (runtime.settingsHydration == SettingsHydration::WaitingForRead ||
        runtime.settingsHydration == SettingsHydration::InFlight ||
        runtime.settingsHydration == SettingsHydration::Hydrated)
      return;
    if (runtime.hydration != Hydration::Hydrated) {
      runtime.settingsHydration = SettingsHydration::WaitingForRead;
      ensureThreadHydrated(threadId);
      return;
    }
    resumeThreadForSettings(threadId);
  }

  void resumeThreadForSettings(const std::string &threadId) {
    ThreadRuntimeState &runtime = runtimeByThread[threadId];
    if (runtime.resumeInFlight)
      return;
    runtime.settingsHydration = SettingsHydration::InFlight;
    const auto token = alive;
    client.execute(
        "thread.resume", {{"threadId", threadId}, {"excludeTurns", true}},
        [this, token, threadId](const nlohmann::json &result) {
          if (!*token)
            return;
          const auto found = runtimeByThread.find(threadId);
          if (found == runtimeByThread.end())
            return;
          ThreadRuntimeState &runtime = found->second;
          if (!result.value("ok", false)) {
            if (isTransientCancellation(result)) {
              runtime.settingsHydration = SettingsHydration::Unknown;
              return;
            }
            runtime.settingsHydration = SettingsHydration::Failed;
            if (selectedThreadId == threadId) {
              const std::string message = safeMessage(
                  result.value("error", nlohmann::json::object()));
              showNotice(message.empty() ? "Thread settings refresh failed"
                                         : message);
            }
          } else {
            runtime.settingsHydration = SettingsHydration::Hydrated;
            runtime.hydration = Hydration::Hydrated;
            runtime.operationReady = true;
          }
          schedulePromptDispatch(threadId);
        });
  }

  void ensureThreadHydrated(const std::string &threadId) {
    if (threadId.empty() || !model.connection().connected)
      return;
    const auto found = runtimeByThread.find(threadId);
    if (found != runtimeByThread.end() &&
        (found->second.hydration == Hydration::Hydrated ||
         found->second.hydration == Hydration::InFlight))
      return;
    readThread(threadId);
  }

  void hydrateThreadForSelection(const std::string &threadId) {
    const auto runtime = runtimeByThread.find(threadId);
    if (runtime != runtimeByThread.end() &&
        runtime->second.hydration == Hydration::Failed)
      readThread(threadId, true);
    else
      ensureThreadHydrated(threadId);
    ensureThreadSettingsHydrated(threadId);
    hydrateHistoricalChildren(threadId, true);
  }

  bool submitPrompt(UiPromptDraft draft) {
    draft.text = trimAscii(std::move(draft.text));
    if (draft.text.empty())
      return false;
    if (!canControlProvider()) {
      showNotice("Codex is not ready for a controlled turn. Your message was "
                 "not sent.");
      return false;
    }
    draft.text = middle::promptWithFileLinks(std::move(draft.text),
                                             draft.attachments);
    const bool selectedNewThreadDraft =
        draft.visiblySelectedThreadId == DraftThreadId && newThreadIntent;
    if (!draft.visiblySelectedThreadId.empty() &&
        draft.visiblySelectedThreadId != selectedThreadId &&
        !selectedNewThreadDraft) {
      if (!model.thread(draft.visiblySelectedThreadId)) {
        showNotice("The visibly selected thread is no longer available. Your "
                   "message was not sent.");
        return false;
      }
      selectThread(draft.visiblySelectedThreadId);
    }

    std::string destination = selectedThreadId;
    const ThreadPresentation *thread = model.thread(destination);
    if (destination.empty()) {
      if (!newThreadIntent) {
        showNotice("No destination thread is selected. Your message was not "
                   "sent; select a thread or use New thread.");
        effects.push_back(UiEffect::FocusComposer);
        return false;
      }
      destination = DraftThreadId;
      thread = nullptr;
    } else if (!thread) {
      showNotice("The selected thread is no longer available. Your message "
                 "was not sent.");
      return false;
    }

    if (destination != DraftThreadId) {
      const auto runtime = runtimeByThread.find(destination);
      if (runtime != runtimeByThread.end() &&
          runtime->second.hydration == Hydration::Failed) {
        showNotice("Thread loading failed. Reload the thread before sending; "
                   "your message was not sent.");
        effects.push_back(UiEffect::FocusComposer);
        return false;
      }
    }

    const auto activeTurn =
        destination == DraftThreadId
            ? std::optional<std::string>{}
            : model.activeTurnId(destination);
    static_cast<void>(prompts.admit(
        destination, std::move(draft.text), std::move(draft.attachments),
        std::move(draft.turnStartOptions), thread, activeTurn, now()));
    effects.push_back(UiEffect::PrepareLocalPromptAdmission);
    if (destination == DraftThreadId) {
      pendingThreadStartOptions = std::move(draft.threadStartOptions);
      pendingThreadWorkspace = draft.workspace.empty()
                                   ? newThreadWorkspace
                                   : std::move(draft.workspace);
      changed();
      startThreadForDraft();
    } else {
      changed();
      schedulePromptDispatch(destination);
    }
    return true;
  }

  void startThreadForDraft() {
    if (!canControlProvider() || newThreadCreationInFlight ||
        prompts.submissions(std::string(DraftThreadId)).empty())
      return;
    newThreadCreationInFlight = true;
    nlohmann::json options = pendingThreadStartOptions;
    options.update(newThreadOptions);
    options["cwd"] = pendingThreadWorkspace.empty()
                         ? (newThreadWorkspace.empty() ? defaultWorkspace
                                                       : newThreadWorkspace)
                         : pendingThreadWorkspace;
    const std::string requestedName = newThreadName;
    const auto token = alive;
    client.execute(
        "thread.create", std::move(options),
        [this, token, requestedName](const nlohmann::json &result) {
          if (!*token)
            return;
          newThreadCreationInFlight = false;
          if (!result.value("ok", false)) {
            if (isTransientCancellation(result)) {
              changed();
              return;
            }
            const std::string message =
                safeMessage(result.value("error", nlohmann::json::object()));
            const std::string error =
                message.empty() ? "Thread creation failed" : message;
            std::vector<std::uint64_t> ids;
            for (const auto &submission :
                 prompts.submissions(std::string(DraftThreadId)))
              ids.push_back(submission.id);
            for (const std::uint64_t id : ids)
              static_cast<void>(
                  prompts.fail(std::string(DraftThreadId), id, error));
            if (optimisticThread)
              optimisticThread->phase = UiOptimisticThreadPhase::Failed;
            showNotice(error);
            return;
          }
          const std::string threadId = stringValue(
              result.value("data", nlohmann::json::object())
                  .value("thread", nlohmann::json::object()),
              "id");
          if (threadId.empty()) {
            const std::string error =
                "Thread creation returned no thread identifier";
            std::vector<std::uint64_t> ids;
            for (const auto &submission :
                 prompts.submissions(std::string(DraftThreadId)))
              ids.push_back(submission.id);
            for (const std::uint64_t id : ids)
              static_cast<void>(
                  prompts.fail(std::string(DraftThreadId), id, error));
            if (optimisticThread)
              optimisticThread->phase = UiOptimisticThreadPhase::Failed;
            showNotice(error);
            return;
          }
          if (!prompts.reassignThread(std::string(DraftThreadId), threadId)) {
            if (optimisticThread)
              optimisticThread->phase = UiOptimisticThreadPhase::Failed;
            showNotice("Could not attach the draft prompts to the created "
                       "thread.");
            return;
          }
          ThreadRuntimeState &runtime = runtimeByThread[threadId];
          runtime.hydration = Hydration::Hydrated;
          runtime.settingsHydration = SettingsHydration::Hydrated;
          runtime.operationReady = true;
          if (optimisticThread) {
            optimisticThread->threadId = threadId;
          }
          const bool viewingDraft = selectedThreadId.empty() && newThreadIntent;
          if (viewingDraft) {
            selectedThreadId = threadId;
            newThreadIntent = false;
          }
          newThreadOptions = nlohmann::json::object();
          newThreadName.clear();
          newThreadWorkspace.clear();
          pendingThreadStartOptions = nlohmann::json::object();
          pendingThreadWorkspace.clear();
          if (!requestedName.empty())
            client.execute("thread.rename",
                           {{"threadId", threadId},
                            {"name", requestedName}});
          changed();
          schedulePromptDispatch(threadId);
        });
  }

  void schedulePromptDispatch(const std::string &threadId) {
    if (threadId.empty())
      return;
    deferredPromptDispatch.insert(threadId);
    scheduleWakeup(now());
  }

  void dispatchNextPrompt(const std::string &threadId) {
    if (threadId.empty() || !canControlProvider())
      return;
    auto runtime = runtimeByThread.find(threadId);
    if (runtime != runtimeByThread.end() &&
        runtime->second.settingsHydration == SettingsHydration::InFlight)
      return;
    const auto submissions = prompts.submissions(threadId);
    if (std::ranges::none_of(
            submissions, [](const middle::PromptSubmission &submission) {
              return submission.state == middle::PromptState::Queued;
            }))
      return;
    if (runtime != runtimeByThread.end() && runtime->second.resumeInFlight)
      return;
    if (runtime == runtimeByThread.end() ||
        runtime->second.hydration != Hydration::Hydrated) {
      ensureThreadHydrated(threadId);
      return;
    }
    if (prompts.hasInFlight(threadId))
      return;
    const ThreadPresentation *thread = model.thread(threadId);
    if (!runtime->second.operationReady && thread &&
        thread->status == "notLoaded") {
      resumePromptQueue(threadId);
      return;
    }
    const auto dispatch =
        prompts.beginNext(threadId, model.activeTurnId(threadId));
    if (dispatch)
      dispatchPrompt(*dispatch);
  }

  void dispatchPrompt(middle::PromptDispatch dispatch) {
    nlohmann::json input = nlohmann::json::array(
        {{{"type", "text"},
          {"text", dispatch.prompt},
          {"text_elements", nlohmann::json::array()}}});
    for (const AttachmentDraft &attachment : dispatch.attachments) {
      if (attachment.mimeType.starts_with("image/"))
        input.push_back({{"type", "localImage"},
                         {"path", attachment.path}});
      else if (attachment.mimeType.starts_with("audio/"))
        input.push_back({{"type", "localAudio"},
                         {"path", attachment.path}});
    }
    const std::string threadId = dispatch.threadId;
    const std::uint64_t submissionId = dispatch.id;
    const auto token = alive;
    auto completed = [this, token, threadId,
                      submissionId](const nlohmann::json &result) {
      if (*token)
        completePrompt(threadId, submissionId, result);
    };
    if (dispatch.expectedTurnId) {
      client.execute("turn.steer",
                     {{"threadId", dispatch.threadId},
                      {"expectedTurnId", *dispatch.expectedTurnId},
                      {"clientUserMessageId", dispatch.clientUserMessageId},
                      {"input", std::move(input)}},
                     std::move(completed));
    } else {
      dispatch.turnOptions["clientUserMessageId"] =
          dispatch.clientUserMessageId;
      dispatch.turnOptions["threadId"] = dispatch.threadId;
      dispatch.turnOptions["input"] = std::move(input);
      client.execute("turn.start", std::move(dispatch.turnOptions),
                     std::move(completed));
    }
  }

  void resumePromptQueue(const std::string &threadId) {
    ThreadRuntimeState &runtime = runtimeByThread[threadId];
    if (runtime.resumeInFlight || !canControlProvider())
      return;
    runtime.resumeInFlight = true;
    const auto token = alive;
    client.execute(
        "thread.resume", {{"threadId", threadId}},
        [this, token, threadId](const nlohmann::json &result) {
          if (!*token)
            return;
          const auto found = runtimeByThread.find(threadId);
          if (found == runtimeByThread.end())
            return;
          ThreadRuntimeState &runtime = found->second;
          runtime.resumeInFlight = false;
          if (!result.value("ok", false)) {
            if (isTransientCancellation(result)) {
              runtime.hydration = Hydration::NotHydrated;
              runtime.settingsHydration = SettingsHydration::Unknown;
              runtime.operationReady = false;
              return;
            }
            runtime.settingsHydration = SettingsHydration::Failed;
            const std::string message =
                safeMessage(result.value("error", nlohmann::json::object()));
            const std::string displayed =
                message.empty() ? "Thread resume failed" : message;
            static_cast<void>(prompts.failQueued(threadId, displayed));
            showNotice(displayed);
            return;
          }
          runtime.hydration = Hydration::Hydrated;
          runtime.settingsHydration = SettingsHydration::Hydrated;
          runtime.operationReady = true;
          schedulePromptDispatch(threadId);
        });
  }

  void completePrompt(const std::string &threadId,
                      std::uint64_t submissionId,
                      const nlohmann::json &result) {
    if (isTransientCancellation(result)) {
      if (prompts.requeue(threadId, submissionId)) {
        if (auto runtime = runtimeByThread.find(threadId);
            runtime != runtimeByThread.end()) {
          runtime->second.hydration = Hydration::NotHydrated;
          runtime->second.operationReady = false;
        }
        changed();
      }
      return;
    }
    if (attemptThreadRecovery(threadId, submissionId, result))
      return;
    const auto runtime = runtimeByThread.find(threadId);
    if (runtime != runtimeByThread.end())
      runtime->second.recoveryAttemptedSubmissions.erase(submissionId);
    if (result.value("ok", false)) {
      if (runtime != runtimeByThread.end())
        runtime->second.operationReady = true;
      static_cast<void>(prompts.acknowledge(
          threadId, submissionId, resultTurnId(result), now()));
      scheduleAcceptedTransition(threadId, submissionId);
      if (optimisticThread && optimisticThread->threadId == threadId)
        optimisticThread->phase = UiOptimisticThreadPhase::Confirmed;
    } else {
      const std::string message =
          safeMessage(result.value("error", nlohmann::json::object()));
      const std::string displayed =
          message.empty() ? "Submission failed" : message;
      static_cast<void>(prompts.fail(threadId, submissionId, displayed));
      if (optimisticThread && optimisticThread->threadId == threadId)
        optimisticThread->phase = UiOptimisticThreadPhase::Failed;
      showNotice(message.empty() ? "Turn submission failed" : message);
    }
    changed();
    schedulePromptDispatch(threadId);
  }

  bool attemptThreadRecovery(const std::string &threadId,
                             std::uint64_t submissionId,
                             const nlohmann::json &result) {
    if (!isThreadNotFoundResult(result))
      return false;
    const auto found = runtimeByThread.find(threadId);
    if (found == runtimeByThread.end())
      return false;
    ThreadRuntimeState &runtime = found->second;
    if (!runtime.recoveryAttemptedSubmissions.insert(submissionId).second)
      return false;
    if (!prompts.requeue(threadId, submissionId))
      return false;
    runtime.hydration = Hydration::NotHydrated;
    runtime.operationReady = false;
    changed();
    runtime.resumeInFlight = true;
    const auto token = alive;
    client.execute(
        "thread.resume", {{"threadId", threadId}},
        [this, token, threadId](const nlohmann::json &resumeResult) {
          if (!*token)
            return;
          const auto found = runtimeByThread.find(threadId);
          if (found == runtimeByThread.end())
            return;
          ThreadRuntimeState &runtime = found->second;
          runtime.resumeInFlight = false;
          if (!resumeResult.value("ok", false)) {
            if (isTransientCancellation(resumeResult)) {
              runtime.hydration = Hydration::NotHydrated;
              runtime.settingsHydration = SettingsHydration::Unknown;
              runtime.operationReady = false;
              return;
            }
            runtime.settingsHydration = SettingsHydration::Failed;
            const std::string message = safeMessage(
                resumeResult.value("error", nlohmann::json::object()));
            const std::string displayed =
                message.empty() ? "Thread recovery failed" : message;
            static_cast<void>(prompts.failQueued(threadId, displayed));
            showNotice(displayed);
            return;
          }
          runtime.hydration = Hydration::Hydrated;
          runtime.settingsHydration = SettingsHydration::Hydrated;
          runtime.operationReady = true;
          schedulePromptDispatch(threadId);
        });
    return true;
  }

  void scheduleAcceptedTransition(const std::string &threadId,
                                  std::uint64_t submissionId) {
    const middle::PromptSubmission *submission =
        prompts.submission(threadId, submissionId);
    if (!submission || submission->state != middle::PromptState::Accepted)
      return;
    const std::int64_t deadline =
        submission->acceptedAtMilliseconds +
        middle::AcknowledgementTransitionMilliseconds;
    acceptedTransitionDeadlines[{threadId, submissionId}] = deadline;
    scheduleWakeup(deadline);
  }

  void tick() {
    nextWakeupAt.reset();
    const auto deferred = std::exchange(deferredPromptDispatch,
                                        std::set<std::string>{});
    for (const std::string &threadId : deferred)
      dispatchNextPrompt(threadId);

    const std::int64_t current = now();
    bool projectionChanged = false;
    for (auto iterator = acceptedTransitionDeadlines.begin();
         iterator != acceptedTransitionDeadlines.end();) {
      if (iterator->second > current) {
        scheduleWakeup(iterator->second);
        ++iterator;
        continue;
      }
      const auto &[threadId, submissionId] = iterator->first;
      const middle::PromptSubmission *submission =
          prompts.submission(threadId, submissionId);
      if (submission && submission->state == middle::PromptState::Accepted &&
          submission->acceptedTransitionActive(current)) {
        iterator->second = submission->acceptedAtMilliseconds +
                           middle::AcknowledgementTransitionMilliseconds;
        scheduleWakeup(iterator->second);
        ++iterator;
        continue;
      }
      if (const ThreadPresentation *thread = model.thread(threadId))
        prompts.reconcile(threadId, *thread, current);
      iterator = acceptedTransitionDeadlines.erase(iterator);
      projectionChanged = true;
    }
    if (projectionChanged)
      changed();
  }

  [[nodiscard]] bool isPendingActionable(
      const std::string &requestKey) const {
    const auto request = model.pendingRequestPresentations().find(requestKey);
    return canControlProvider() &&
           request != model.pendingRequestPresentations().end() &&
           request->second.generation == model.connection().generation &&
           !resolvingRequests.contains(requestKey);
  }

  [[nodiscard]] UiPendingRequestView pendingView(
      const PendingRequestPresentation &request) const {
    return {request.id,
            request.kind,
            request.threadId,
            request.generation,
            request.raw,
            PendingRequestPolicy::title(request.kind),
            PendingRequestPolicy::detail(request.id, request.threadId,
                                         request.raw),
            PendingRequestPolicy::directAcceptLabel(request.kind),
            PendingRequestPolicy::supportsDirectAccept(request.kind),
            isPendingActionable(request.id)};
  }

  bool resolvePending(UiPendingRequestView request,
                      PendingRequestResponse response) {
    const auto current = model.pendingRequestPresentations().find(request.id);
    if (!canControlProvider() ||
        current == model.pendingRequestPresentations().end() ||
        current->second.generation != model.connection().generation ||
        current->second.generation != request.generation ||
        current->second.kind != request.kind ||
        current->second.threadId != request.threadId ||
        current->second.raw != request.raw ||
        resolvingRequests.contains(request.id)) {
      showNotice("The pending request is no longer actionable.", false);
      return false;
    }
    const nlohmann::json nativeId =
        nlohmann::json::parse(request.id, nullptr, false);
    if (nativeId.is_discarded()) {
      showNotice("The pending request has an invalid identity.");
      return false;
    }
    resolvingRequests.insert(request.id);
    if (!client.respond(nativeId, std::move(response.result),
                        std::move(response.error))) {
      resolvingRequests.erase(request.id);
      showNotice("The pending response could not be sent.");
      return false;
    }
    if (!current->second.threadId.empty())
      model.noteThreadActivity(current->second.threadId, nowSeconds());
    changed();
    return true;
  }

  UiSettingsView projectSettings() const {
    UiSettingsView result;
    result.identity = "no-thread";
    if (const ThreadPresentation *thread = model.thread(selectedThreadId)) {
      result.identity = thread->id;
      result.settingsUpdate = thread->latestSettingsUpdate;
      result.settingsRevision = thread->settingsRevision;
      result.canonical = thread->raw;
      const auto settings = thread->domains.find("thread.settings.changed");
      if (settings != thread->domains.end() && settings->second.is_object()) {
        nlohmann::json update = settings->second;
        if (update.contains("threadSettings") &&
            update["threadSettings"].is_object())
          update = update["threadSettings"];
        if (update.contains("effort"))
          result.canonical.erase("reasoningEffort");
        if (update.contains("sandboxPolicy"))
          result.canonical.erase("sandbox");
        result.canonical.merge_patch(update);
      }
    } else if (newThreadIntent) {
      result.identity = DraftThreadId;
      result.canonical["cwd"] = newThreadWorkspace.empty()
                                     ? defaultWorkspace
                                     : newThreadWorkspace;
    } else {
      result.canonical["cwd"] = defaultWorkspace;
    }
    const auto profiles =
        model.globalDomains().find("operation.permission-profiles.list");
    if (profiles != model.globalDomains().end())
      result.permissionProfiles = profiles->second;
    result.modelCatalog = model.modelCatalog();
    return result;
  }

  UiSessionView &refreshView(bool conversationFollowing,
                             std::string draftWorkspace) {
    const std::int64_t current = now();
    const std::string visibleThreadId =
        selectedThreadId.empty() && newThreadIntent
            ? std::string(DraftThreadId)
            : selectedThreadId;
    viewState = UiSessionView{};
    viewState.selectedThreadId = selectedThreadId;
    viewState.newThreadIntent = newThreadIntent;
    viewState.threads =
        ui::projectThreadListSnapshot(model, visibleThreadId);
    viewState.inspector = ui::projectInspectorSnapshot(
        model, selectedThreadId,
        [this](std::string_view requestId) {
          return isPendingActionable(std::string(requestId));
        });
    viewState.settings = projectSettings();
    viewState.optimisticThread = optimisticThread;

    const ThreadPresentation *thread = model.thread(selectedThreadId);
    middle::AuthoritativeItemIndex authoritativeItems =
        middle::indexAuthoritativeItems(visibleThreadId, thread);
    if (thread)
      prompts.reconcile(selectedThreadId, authoritativeItems, current);
    const std::size_t authoritativeCount = authoritativeItems.ordered.size();
    HistoryWindow &history = historyWindows[visibleThreadId];
    if (!conversationFollowing &&
        authoritativeCount > history.lastAuthoritativeCount)
      history.effective +=
          authoritativeCount - history.lastAuthoritativeCount;
    else if (conversationFollowing)
      history.effective = history.requested;
    history.lastAuthoritativeCount = authoritativeCount;
    UiConversationView &conversation = viewState.conversation;
    conversation.key = visibleThreadId;
    conversation.snapshot = middle::ConversationProjection::project(
        authoritativeItems, thread, prompts.submissions(visibleThreadId),
        history.effective, current);
    conversation.snapshot.activeTurnId =
        model.activeTurnId(selectedThreadId);
    if (thread) {
      conversation.mode = UiConversationMode::Thread;
      conversation.title = thread->title;
      conversation.workspace = thread->cwd;
      conversation.status = std::string(classifyStatus(thread->status).text);
      conversation.lastActivityAt = thread->lastActivityAt;
      conversation.emptyMessage = "No materialized activity.";
    } else if (newThreadIntent) {
      conversation.mode = UiConversationMode::NewThread;
      conversation.title = newThreadName.empty() ? "New thread" : newThreadName;
      conversation.workspace =
          draftWorkspace.empty()
              ? (newThreadWorkspace.empty() ? defaultWorkspace
                                            : newThreadWorkspace)
              : std::move(draftWorkspace);
      conversation.emptyMessage = "Send a message to create this thread.";
    } else {
      conversation.mode = UiConversationMode::NoSelection;
      conversation.title = "Select a thread";
      conversation.workspace = "No workspace";
      conversation.emptyMessage = "Conversation activity appears here.";
    }

    const ConnectionPresentation &connection = model.connection();
    UiStatusView &status = viewState.status;
    status.connected = connection.connected;
    status.retrying = connection.retrying;
    status.role = connection.role;
    status.providerState = connection.providerState;
    status.connectionSettings = connection.settings;
    status.workspace = conversation.workspace;
    status.activeTurn =
        model.activeTurnId(selectedThreadId).has_value();
    status.totalPending = model.pendingRequestCount();
    const std::string selectedKey = stringValue(connection.settings, "selected");
    const nlohmann::json available =
        connection.settings.value("available", nlohmann::json::array());
    if (available.is_array()) {
      for (const auto &entry : available) {
        if (stringValue(entry, "key") == selectedKey) {
          status.selectedTransport = stringValue(entry, "label");
          break;
        }
      }
    }
    for (const auto &[id, request] : model.pendingRequestPresentations()) {
      UiPendingRequestView projected = pendingView(request);
      if (request.threadId == selectedThreadId) {
        ++status.selectedPending;
        if (!viewState.selectedPendingRequest)
          viewState.selectedPendingRequest = projected;
      }
      viewState.pendingRequests.push_back(std::move(projected));
    }
    status.canSubmit = canControlProvider();
    status.canEditSettings = status.canSubmit && !status.activeTurn;
    return viewState;
  }

  PresentationClient client;
  std::string defaultWorkspace;
  UiSession::Clock clock;
  std::shared_ptr<bool> alive;
  UiSession::ChangedHandler changedHandler;
  UiSession::WakeupHandler wakeupHandler;
  UiSession::ProtocolFrameObserver protocolFrameObserver;
  std::optional<std::int64_t> nextWakeupAt;

  PresentationModel model;
  middle::PromptCoordinator prompts;
  std::string selectedThreadId;
  bool newThreadIntent = false;
  bool newThreadCreationInFlight = false;
  nlohmann::json newThreadOptions = nlohmann::json::object();
  std::string newThreadName;
  std::string newThreadWorkspace;
  nlohmann::json pendingThreadStartOptions = nlohmann::json::object();
  std::string pendingThreadWorkspace;
  std::optional<UiOptimisticThreadView> optimisticThread;

  std::unordered_map<std::string, ThreadRuntimeState> runtimeByThread;
  std::unordered_set<std::string> resolvingRequests;
  std::unordered_set<std::string> staleReadResultCorrelations;
  std::uint64_t nextReadRevision = 1;
  std::unordered_map<std::string, HistoryWindow> historyWindows;
  std::uint64_t observedConnectionGeneration = 0;
  std::uint64_t observedProviderGeneration = 0;
  std::set<std::string> deferredPromptDispatch;
  std::map<std::pair<std::string, std::uint64_t>, std::int64_t>
      acceptedTransitionDeadlines;

  std::vector<UiNotice> notices;
  std::vector<UiEffect> effects;
  std::uint64_t nextNoticeId = 1;
  UiSessionView viewState;
};

UiSession::UiSession(PresentationClient client, std::string defaultWorkspace,
                     Clock clock)
    : impl(std::make_unique<Impl>(std::move(client),
                                  std::move(defaultWorkspace),
                                  std::move(clock))) {}

UiSession::~UiSession() = default;

void UiSession::setChangedHandler(ChangedHandler handler) {
  impl->changedHandler = std::move(handler);
}

void UiSession::setWakeupHandler(WakeupHandler handler) {
  impl->wakeupHandler = std::move(handler);
  if (impl->wakeupHandler && impl->nextWakeupAt)
    impl->wakeupHandler(*impl->nextWakeupAt);
}

void UiSession::setProtocolFrameObserver(ProtocolFrameObserver observer) {
  impl->protocolFrameObserver = std::move(observer);
}

void UiSession::onPresentationFrame(const nlohmann::json &frame) {
  impl->onPresentationFrame(frame);
}

void UiSession::noteThreadActivity(const std::string &threadId) {
  impl->noteThreadActivity(threadId);
}

void UiSession::tick() { impl->tick(); }

std::string UiSession::conversationKey() const {
  return impl->selectedThreadId.empty() && impl->newThreadIntent
             ? std::string(DraftThreadId)
             : impl->selectedThreadId;
}

const UiSessionView &
UiSession::refreshView(bool conversationFollowing,
                       std::string draftWorkspace) {
  return impl->refreshView(conversationFollowing, std::move(draftWorkspace));
}

std::vector<UiNotice> UiSession::takeNotices() {
  return std::exchange(impl->notices, {});
}

std::vector<UiEffect> UiSession::takeEffects() {
  return std::exchange(impl->effects, {});
}

void UiSession::refreshThreads() {
  if (impl->providerReady())
    impl->client.execute("threads.list", nlohmann::json::object());
}

void UiSession::connectTransport() {
  impl->client.send("connection.connect");
}

void UiSession::disconnectTransport() {
  impl->client.send("connection.disconnect");
}

void UiSession::reconnectTransport() {
  impl->client.send("connection.reconnect");
}

void UiSession::configureConnection(nlohmann::json settings) {
  const auto token = impl->alive;
  impl->client.execute(
      "connection.configure", std::move(settings),
      [implementation = impl.get(), token](const nlohmann::json &result) {
        if (!*token || result.value("ok", false))
          return;
        const std::string message =
            safeMessage(result.value("error", nlohmann::json::object()));
        implementation->showNotice(
            message.empty() ? "Connection configuration failed" : message);
      });
}

void UiSession::toggleController() {
  impl->client.send(impl->model.connection().role == "controller"
                        ? "controller.release"
                        : "controller.claim");
}

void UiSession::selectThread(std::string threadId) {
  impl->selectThread(std::move(threadId));
}

void UiSession::reloadThread(const std::string &threadId) {
  impl->runtimeByThread[threadId].settingsHydration =
      Impl::SettingsHydration::Unknown;
  impl->readThread(threadId, true);
  impl->ensureThreadSettingsHydrated(threadId);
}

void UiSession::beginNewThread(UiNewThreadDraft draft) {
  impl->beginNewThread(std::move(draft));
}

void UiSession::renameThread(const std::string &threadId, std::string name) {
  name = trimAscii(std::move(name));
  if (!impl->canControlProvider() || !impl->model.thread(threadId) ||
      name.empty())
    return;
  impl->client.execute("thread.rename",
                       {{"threadId", threadId}, {"name", std::move(name)}});
}

void UiSession::forkThread(const std::string &threadId) {
  if (threadId.empty() || !impl->canControlProvider())
    return;
  const auto token = impl->alive;
  impl->client.execute(
      "thread.fork", {{"threadId", threadId}},
      [implementation = impl.get(), token](const nlohmann::json &result) {
        if (!*token || !result.value("ok", false))
          return;
        const std::string id = stringValue(
            result.value("data", nlohmann::json::object())
                .value("thread", nlohmann::json::object()),
            "id");
        if (!id.empty())
          implementation->selectThread(id);
      });
}

void UiSession::toggleThreadArchive(const std::string &threadId) {
  if (!impl->canControlProvider())
    return;
  const ThreadPresentation *thread = impl->model.thread(threadId);
  if (!thread)
    return;
  impl->client.execute(thread->archived ? "thread.unarchive"
                                        : "thread.archive",
                       {{"threadId", threadId}});
}

void UiSession::deleteThread(const std::string &threadId) {
  if (!threadId.empty() && impl->canControlProvider())
    impl->client.execute("thread.delete", {{"threadId", threadId}});
}

bool UiSession::submitPrompt(UiPromptDraft draft) {
  return impl->submitPrompt(std::move(draft));
}

void UiSession::interruptTurn() {
  const auto turn = impl->model.activeTurnId(impl->selectedThreadId);
  if (turn)
    impl->client.execute("turn.interrupt",
                         {{"threadId", impl->selectedThreadId},
                          {"turnId", *turn}});
}

void UiSession::loadEarlierConversation() {
  const std::string key = impl->selectedThreadId.empty() &&
                                  impl->newThreadIntent
                              ? std::string(DraftThreadId)
                              : impl->selectedThreadId;
  Impl::HistoryWindow &history = impl->historyWindows[key];
  history.requested +=
      middle::ConversationProjection::DefaultAuthoritativeItemLimit;
  history.effective +=
      middle::ConversationProjection::DefaultAuthoritativeItemLimit;
  impl->changed();
}

bool UiSession::resolvePending(UiPendingRequestView request,
                               PendingRequestResponse response) {
  return impl->resolvePending(std::move(request), std::move(response));
}

} // namespace codexui::codex
