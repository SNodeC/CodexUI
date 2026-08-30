// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PendingRequestPolicy.h"
#include "codex/PresentationProtocol.h"
#include "codex/UiSession.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using codexui::codex::PresentationClient;
using codexui::codex::UiEffect;
using codexui::codex::UiNewThreadDraft;
using codexui::codex::UiPendingRequestView;
using codexui::codex::UiPromptDraft;
using codexui::codex::UiSession;
using codexui::codex::UiConversationMode;
using codexui::codex::presentation::Authority;

struct Request {
  std::string id;
  std::string action;
  nlohmann::json data;
  PresentationClient::Completion completion;
};

struct Response {
  nlohmann::json id;
  nlohmann::json result;
  nlohmann::json error;
};

class FakeBoundary final {
public:
  PresentationClient client() {
    return PresentationClient{
        [this](std::string action, nlohmann::json data,
               PresentationClient::Completion completion) {
          const std::string id = "request-" + std::to_string(nextId++);
          requests.push_back(
              {id, std::move(action), std::move(data), std::move(completion)});
          return id;
        },
        [this](std::string action, nlohmann::json data) {
          commands.emplace_back(std::move(action), std::move(data));
          return true;
        },
        [this](nlohmann::json id, nlohmann::json result,
               nlohmann::json error) {
          responses.push_back(
              {std::move(id), std::move(result), std::move(error)});
          return true;
        }};
  }

  Request *latest(std::string_view action) {
    const auto found = std::find_if(
        requests.rbegin(), requests.rend(), [action](const Request &request) {
          return request.action == action;
        });
    return found == requests.rend() ? nullptr : &*found;
  }

  std::size_t count(std::string_view action) const {
    return static_cast<std::size_t>(std::count_if(
        requests.begin(), requests.end(), [action](const Request &request) {
          return request.action == action;
        }));
  }

  std::uint64_t nextId = 1;
  std::vector<Request> requests;
  std::vector<std::pair<std::string, nlohmann::json>> commands;
  std::vector<Response> responses;
};

bool expect(bool condition, std::string_view message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

nlohmann::json thread(std::string id, std::string title) {
  return {{"id", std::move(id)},
          {"preview", std::move(title)},
          {"cwd", "/workspace"},
          {"status", {{"type", "idle"}}},
          {"turns", nlohmann::json::array()}};
}

void complete(UiSession &session, Request &request, std::uint64_t sequence,
              nlohmann::json data, Authority authority = Authority::None,
              nlohmann::json scope = nlohmann::json::object()) {
  const nlohmann::json result = codexui::codex::presentation::result(
      sequence, 1, request.action, request.id, true, std::move(data),
      authority, std::move(scope));
  if (request.completion)
    request.completion(result);
  session.onPresentationFrame(result);
}

} // namespace

int main() {
  bool passed = true;
  std::int64_t now = 1'000'000;
  FakeBoundary boundary;
  UiSession session(boundary.client(), "/workspace", [&now] { return now; });
  std::size_t changeCount = 0;
  std::optional<std::int64_t> wakeup;
  session.setChangedHandler([&changeCount] { ++changeCount; });
  session.setWakeupHandler(
      [&wakeup](std::int64_t atMilliseconds) { wakeup = atMilliseconds; });

  std::uint64_t sequence = 1;
  session.onPresentationFrame(codexui::codex::presentation::event(
      sequence++, 1, "connection.lifecycle", {{"state", "connected"}},
      Authority::Merge));
  session.onPresentationFrame(codexui::codex::presentation::event(
      sequence++, 1, "connection.bridge",
      {{"state", "opened"},
       {"connectionId", "ui-controller"},
       {"role", "controller"}},
      Authority::Merge));
  session.onPresentationFrame(codexui::codex::presentation::event(
      sequence++, 1, "connection.provider",
      {{"generation", std::uint64_t{1}}, {"state", "ready"}},
      Authority::Replace));

  passed &= expect(boundary.count("threads.list") == 1 &&
                       boundary.count("models.list") == 1 &&
                       boundary.count("permission-profiles.list") == 1,
                   "provider readiness hydrates through the generic boundary");

  session.onPresentationFrame(codexui::codex::presentation::event(
      sequence++, 1, "thread.upsert",
      {{"thread", thread("thread-a", "Boundary thread")}}, Authority::Merge,
      {{"threadId", "thread-a"}}));
  session.selectThread("thread-a");
  Request *read = boundary.latest("thread.read");
  passed &= expect(read && read->data.value("threadId", std::string{}) ==
                               "thread-a" &&
                       read->data.value("includeTurns", false),
                   "selection requests authoritative thread hydration");
  if (!read)
    return 1;
  complete(session, *read, sequence++,
           {{"thread", thread("thread-a", "Boundary thread")}},
           Authority::Replace, {{"threadId", "thread-a"}});

  Request *resume = boundary.latest("thread.resume");
  passed &= expect(resume && resume->data.value("excludeTurns", false),
                   "settings hydration remains a logic-layer operation");
  if (!resume)
    return 1;
  complete(session, *resume, sequence++,
           {{"thread", {{"id", "thread-a"}}}, {"model", "gpt-test"}},
           Authority::Merge, {{"threadId", "thread-a"}});

  const auto &selected = session.refreshView(true, "/workspace");
  passed &= expect(selected.selectedThreadId == "thread-a" &&
                       selected.conversation.mode == UiConversationMode::Thread &&
                       selected.conversation.title == "Boundary thread" &&
                       selected.status.canSubmit &&
                       selected.threads.canControl,
                   "one neutral snapshot projects the selected UI state");

  UiPromptDraft prompt;
  prompt.text = "  inspect the boundary  ";
  prompt.turnStartOptions = {{"model", "gpt-test"}};
  prompt.threadStartOptions = {{"ephemeral", false}};
  prompt.workspace = "/workspace";
  prompt.visiblySelectedThreadId = "thread-a";
  passed &= expect(session.submitPrompt(std::move(prompt)),
                   "prompt admission is accepted by UiSession");
  passed &= expect(wakeup == now,
                   "transport dispatch is deferred without a new scheduler");
  session.tick();
  Request *turnStart = boundary.latest("turn.start");
  const nlohmann::json input =
      turnStart ? turnStart->data.value("input", nlohmann::json::array())
                : nlohmann::json::array();
  passed &= expect(turnStart &&
                       turnStart->data.value("threadId", std::string{}) ==
                           "thread-a" &&
                       input.is_array() && input.size() == 1 &&
                       input[0].value("text", std::string{}) ==
                           "inspect the boundary",
                   "queued prompt becomes the exact protocol turn operation");

  session.onPresentationFrame(codexui::codex::presentation::event(
      sequence++, 1, "pending-request.upsert",
      {{"requestId", 77},
       {"category", "command-approval"},
       {"request", {{"command", "make test"}}}},
      Authority::Merge, {{"threadId", "thread-a"}, {"requestId", 77}}));
  const auto &pendingView = session.refreshView(true, "/workspace");
  passed &= expect(pendingView.selectedPendingRequest &&
                       pendingView.selectedPendingRequest->id == "77" &&
                       pendingView.selectedPendingRequest->actionable &&
                       pendingView.selectedPendingRequest->supportsDirectAccept,
                   "pending capability and eligibility cross the neutral API");
  if (!pendingView.selectedPendingRequest)
    return 1;
  const UiPendingRequestView pending = *pendingView.selectedPendingRequest;
  passed &= expect(session.resolvePending(
                       pending,
                       codexui::codex::PendingRequestPolicy::positiveResponse(
                           pending.kind, pending.raw)) &&
                       boundary.responses.size() == 1 &&
                       boundary.responses.front().id == 77,
                   "typed pending response returns through the same boundary");

  session.onPresentationFrame(codexui::codex::presentation::event(
      sequence++, 1, "pending-request.upsert",
      {{"requestId", 78},
       {"category", "command-approval"},
       {"request", {{"command", "stale"}}}},
      Authority::Merge, {{"threadId", "thread-a"}, {"requestId", 78}}));
  const auto &staleView = session.refreshView(true, "/workspace");
  const auto staleFound = std::find_if(
      staleView.pendingRequests.begin(), staleView.pendingRequests.end(),
      [](const UiPendingRequestView &request) { return request.id == "78"; });
  if (staleFound == staleView.pendingRequests.end())
    return 1;
  const UiPendingRequestView stale = *staleFound;
  session.onPresentationFrame(codexui::codex::presentation::event(
      sequence++, 1, "pending-request.removed", nlohmann::json::object(),
      Authority::Remove, {{"threadId", "thread-a"}, {"requestId", 78}}));
  passed &= expect(
      !session.resolvePending(
          stale, codexui::codex::PendingRequestPolicy::positiveResponse(
                     stale.kind, stale.raw)),
      "stale dialog responses are rejected against the current snapshot");

  session.beginNewThread(UiNewThreadDraft{
      "/workspace/new", "Neutral draft", {}, {}, true});
  const auto effects = session.takeEffects();
  const auto &draft = session.refreshView(true, "/workspace/new");
  passed &= expect(
      draft.newThreadIntent &&
          draft.conversation.mode == UiConversationMode::NewThread &&
          draft.conversation.title == "Neutral draft" &&
          std::find(effects.begin(), effects.end(),
                    UiEffect::ClearComposerDraft) != effects.end() &&
          std::find(effects.begin(), effects.end(), UiEffect::FocusComposer) !=
              effects.end(),
      "new-thread intent exposes state plus narrow renderer effects");
  passed &= expect(changeCount != 0,
                   "state changes notify the existing GUI-thread adapter");

  return passed ? 0 : 1;
}
