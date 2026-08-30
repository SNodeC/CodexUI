// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include <core/SNodeC.h>
#include <utils/Config.h>

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/PendingRequestDialog.h"
#include "codex/PresentationProtocol.h"
#include "codex/ShellWidget.h"
#include "codex/middle/ConversationCards.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace codexui::codex {

class FrontendSessionTestPeer final {
public:
  static int takeClientDescriptor(FrontendSession &session) {
    return std::exchange(session.clientDescriptor, -1);
  }
};

namespace {

using presentation::Authority;

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

void spin(int milliseconds = 0) {
  milliseconds = std::max(milliseconds, 20);
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (milliseconds > 0)
      QThread::msleep(1);
  } while (timer.elapsed() < milliseconds);
}

class PresentationPeer final {
public:
  explicit PresentationPeer(int descriptor) : descriptor_(descriptor) {}
  ~PresentationPeer() {
    if (descriptor_ >= 0)
      ::close(descriptor_);
  }

  PresentationPeer(const PresentationPeer &) = delete;
  PresentationPeer &operator=(const PresentationPeer &) = delete;

  bool send(const nlohmann::json &frame) {
    std::string encoded = frame.dump();
    encoded.push_back('\n');
    std::size_t offset = 0;
    QElapsedTimer timer;
    timer.start();
    while (offset < encoded.size() && timer.elapsed() < 1000) {
      const ssize_t written = ::write(descriptor_, encoded.data() + offset,
                                      encoded.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
      } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        spin(1);
      } else {
        return false;
      }
    }
    spin(2);
    return offset == encoded.size();
  }

  std::optional<nlohmann::json> waitFor(std::string_view action,
                                        std::string_view threadId = {},
                                        int timeoutMilliseconds = 1000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds) {
      pump();
      const auto found = std::find_if(
          frames_.begin(), frames_.end(), [&](const nlohmann::json &frame) {
            if (frame.value("action", std::string{}) != action)
              return false;
            if (threadId.empty())
              return true;
            const nlohmann::json data =
                frame.value("data", nlohmann::json::object());
            return data.value("threadId", std::string{}) == threadId;
          });
      if (found != frames_.end()) {
        nlohmann::json result = std::move(*found);
        frames_.erase(found);
        return result;
      }
      spin(1);
    }
    return std::nullopt;
  }

  bool has(std::string_view action) {
    pump();
    return std::ranges::any_of(frames_, [&](const nlohmann::json &frame) {
      return frame.value("action", std::string{}) == action;
    });
  }

  void discard() {
    pump();
    frames_.clear();
  }

private:
  void pump() {
    char buffer[8192];
    for (;;) {
      const ssize_t count = ::read(descriptor_, buffer, sizeof(buffer));
      if (count > 0) {
        incoming_.append(buffer, static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        std::cerr << "peer read failed: " << std::strerror(errno) << '\n';
      break;
    }
    for (;;) {
      const std::size_t newline = incoming_.find('\n');
      if (newline == std::string::npos)
        break;
      const std::string line = incoming_.substr(0, newline);
      incoming_.erase(0, newline + 1);
      if (!line.empty())
        frames_.push_back(nlohmann::json::parse(line));
    }
  }

  int descriptor_ = -1;
  std::string incoming_;
  std::deque<nlohmann::json> frames_;
};

nlohmann::json thread(std::string id, std::string name,
                      std::string status = "idle",
                      std::string activeTurnId = {}) {
  nlohmann::json turns = nlohmann::json::array();
  if (status == "active") {
    if (activeTurnId.empty())
      activeTurnId = id + "-turn";
    turns.push_back({{"id", std::move(activeTurnId)},
                     {"status", "inProgress"},
                     {"items", nlohmann::json::array()}});
  }
  return {{"id", std::move(id)},
          {"name", std::move(name)},
          {"cwd", "/tmp/codexui-shell-test"},
          {"status", std::move(status)},
          {"turns", std::move(turns)}};
}

nlohmann::json threadWithAgentMessage(std::string id, std::string name,
                                      std::string message) {
  const std::string turnId = id + "-turn";
  const std::string messageId = id + "-message";
  nlohmann::json value = thread(std::move(id), std::move(name));
  value["turns"] = nlohmann::json::array(
      {{{"id", turnId},
        {"status", "completed"},
        {"items", nlohmann::json::array({{{"id", messageId},
                                          {"type", "agentMessage"},
                                          {"phase", "final_answer"},
                                          {"text", std::move(message)}}})}}});
  return value;
}

nlohmann::json threadWithPlanAndAgent(std::string id, std::string name) {
  const std::string turnId = id + "-turn";
  const std::string planId = id + "-plan";
  const std::string agentId = id + "-agent";
  nlohmann::json value = thread(std::move(id), std::move(name));
  value["turns"] = nlohmann::json::array(
      {{{"id", turnId},
        {"status", "completed"},
        {"items",
         nlohmann::json::array({{{"id", planId},
                                 {"type", "plan"},
                                 {"text", "retained plan marker"}},
                                {{"id", agentId},
                                 {"type", "subAgentActivity"},
                                 {"status", "completed"},
                                 {"prompt", "retained agent marker"}}})}}});
  return value;
}

bool selectThread(QListWidget *list, std::string_view id) {
  if (!list)
    return false;
  for (int row = 0; row < list->count(); ++row) {
    QListWidgetItem *item = list->item(row);
    if (item && item->data(Qt::UserRole).toString().toStdString() == id) {
      list->setCurrentRow(row);
      spin(2);
      return true;
    }
  }
  return false;
}

bool submit(codexui::ExpandingPromptEditor *editor, const QString &prompt) {
  if (!editor || !editor->isEnabled())
    return false;
  editor->setPlainText(prompt);
  return QMetaObject::invokeMethod(editor, "submitRequested",
                                   Qt::DirectConnection);
}

const middle::LocalPromptData *localPrompt(ShellWidget &shell,
                                           const QString &prompt) {
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    auto *card = dynamic_cast<middle::ConversationCard *>(widget);
    if (!card)
      continue;
    const auto *local =
        std::get_if<middle::LocalPromptData>(&card->data().payload);
    if (local && local->prompt == prompt)
      return local;
  }
  return nullptr;
}

bool hasAgentMessage(ShellWidget &shell, const QString &message) {
  for (QWidget *widget : shell.findChildren<QWidget *>()) {
    auto *card = dynamic_cast<middle::ConversationCard *>(widget);
    if (!card)
      continue;
    const auto *agent =
        std::get_if<middle::AgentMessageData>(&card->data().payload);
    if (agent && agent->text == message)
      return true;
  }
  return false;
}

bool hasPresentedText(QWidget &root, const QString &marker) {
  return std::ranges::any_of(
      root.findChildren<QLabel *>(), [&marker](QLabel *label) {
        return label &&
               (label->text().contains(marker) ||
                label->property("markdownSource").toString().contains(marker));
      });
}

struct ShellFlow {
  PresentationPeer &peer;
  ShellWidget shell;
  QListWidget *list;
  codexui::ExpandingPromptEditor *editor;
  std::uint64_t sequence = 1;
  std::uint64_t generation = 1;
  std::string startBCorrelation;

  ShellFlow(FrontendSession &session, PresentationPeer &peer)
      : peer(peer), shell(session) {
    shell.resize(1500, 850);
    shell.show();
    spin(10);
    list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
    editor = shell.findChild<codexui::ExpandingPromptEditor *>(
        QStringLiteral("upcomingPromptEditor"));
  }

  bool completeSettingsRefresh(const std::string &threadId) {
    const auto request = peer.waitFor("thread.resume", threadId);
    const std::string message =
        threadId + " receives its metadata-only settings refresh";
    if (!expect(request.has_value(), message.c_str()) || !request)
      return false;
    return peer.send(presentation::result(
        sequence++, generation, "thread.resume",
        request->value("correlationId", std::string{}), true,
        {{"thread", {{"id", threadId}}}}, Authority::Merge,
        {{"threadId", threadId}}));
  }

  bool verifyHydrationAndNavigation();
  bool verifyPromptLifecycle();
  bool verifyReconnectHydration();
  bool verifyTerminalCallback();
  bool verifyBoundedChildHydration();
  bool verifyNotFoundRecovery();
  bool verifyFailedHydration();
  bool verifyOptimisticNewThread();
  bool verifyPendingResolutionBoundary();

  bool run() {
    return verifyHydrationAndNavigation() && verifyPromptLifecycle() &&
           verifyReconnectHydration() && verifyTerminalCallback() &&
           verifyBoundedChildHydration() && verifyNotFoundRecovery() &&
           verifyFailedHydration() && verifyOptimisticNewThread() &&
           verifyPendingResolutionBoundary();
  }
};

bool ShellFlow::verifyHydrationAndNavigation() {
  bool result = true;

  result &= peer.send(
      presentation::event(sequence++, 1, "connection.lifecycle",
                          {{"state", "connected"}}, Authority::Merge));
  result &= peer.send(presentation::event(sequence++, 1, "connection.bridge",
                                          {{"state", "opened"},
                                           {"connectionId", "test-controller"},
                                           {"role", "controller"}},
                                          Authority::Merge));
  result &= peer.send(presentation::event(
      sequence++, 1, "connection.provider",
      {{"generation", std::uint64_t{1}}, {"state", "ready"}},
      Authority::Replace));
  result &= peer.send(presentation::event(
      sequence++, 1, "thread.upsert", {{"thread", thread("thread-a", "A")}},
      Authority::Merge, {{"threadId", "thread-a"}}));
  result &= peer.send(presentation::event(
      sequence++, 1, "thread.upsert", {{"thread", thread("thread-b", "B")}},
      Authority::Merge, {{"threadId", "thread-b"}}));
  spin(10);
  peer.discard(); // bridge bootstrap operations are outside this scenario.

  result &= expect(selectThread(list, "thread-a"),
                   "the visible A row becomes the prompt destination");
  const auto readA = peer.waitFor("thread.read", "thread-a");
  result &= expect(readA.has_value(), "selecting A requests hydration");
  if (!readA)
    return false;
  result &= expect(selectThread(list, "thread-b"),
                   "B can be selected while A is still hydrating");
  const auto readB = peer.waitFor("thread.read", "thread-b");
  result &= expect(readB.has_value(), "selecting B requests its own hydration");
  if (!readB)
    return false;
  result &= peer.send(presentation::event(
      sequence++, 1, "thread.upsert",
      {{"thread", thread("thread-a", "A", "notLoaded")}}, Authority::Merge,
      {{"threadId", "thread-a"}}));
  result &= peer.send(presentation::result(
      sequence++, 1, "thread.read",
      readA->value("correlationId", std::string{}), true,
      {{"thread", threadWithPlanAndAgent("thread-a", "A")}}, Authority::Replace,
      {{"threadId", "thread-a"}}));
  result &= completeSettingsRefresh("thread-a");
  result &= peer.send(
      presentation::result(sequence++, 1, "thread.read",
                           readB->value("correlationId", std::string{}), true,
                           {{"thread", thread("thread-b", "B")}},
                           Authority::Replace, {{"threadId", "thread-b"}}));
  result &= completeSettingsRefresh("thread-b");
  spin(10);
  result &= expect(selectThread(list, "thread-a"),
                   "A can be selected again after background hydration");
  spin(10);
  auto *inspector = shell.findChild<QFrame *>(QStringLiteral("inspector"));
  auto *inspectorTabs = inspector ? inspector->findChild<QTabWidget *>(
                                        QString{}, Qt::FindDirectChildrenOnly)
                                  : nullptr;
  result &= expect(
      inspector && inspectorTabs &&
          hasPresentedText(*inspector, QStringLiteral("retained plan marker")),
      "A's retained plan survives background hydration and navigation");
  if (inspectorTabs) {
    inspectorTabs->setCurrentIndex(1);
    spin();
  }
  result &= expect(
      inspector &&
          hasPresentedText(*inspector, QStringLiteral("retained agent marker")),
      "A's retained agent detail survives background hydration and navigation");
  return result;
}

bool ShellFlow::verifyPromptLifecycle() {
  bool result = true;

  result &= expect(submit(editor, QStringLiteral("prompt A1")),
                   "A1 is admitted through the real composer");
  const auto startA = peer.waitFor("turn.start", "thread-a");
  result &= expect(startA.has_value() && !peer.has("thread.create"),
                   "A1 starts on selected A and never creates a new thread");
  if (!startA)
    return false;
  const nlohmann::json startAData =
      startA->value("data", nlohmann::json::object());
  const std::string clientId =
      startAData.value("clientUserMessageId", std::string{});
  result &= expect(!clientId.empty(),
                   "turn.start carries the prompt correlation identity");

  result &= expect(submit(editor, QStringLiteral("prompt A2")),
                   "A2 remains independently editable while A1 awaits ack");
  spin(20);
  result &= expect(!peer.has("turn.steer"),
                   "A2 waits behind the one in-flight operation for A");

  result &= peer.send(presentation::event(
      sequence++, 1, "conversation.item.upsert",
      {{"item",
        {{"id", "user-a1"},
         {"type", "userMessage"},
         {"clientId", clientId},
         {"content", {{{"type", "text"}, {"text", "prompt A1"}}}}}}},
      Authority::Merge,
      {{"threadId", "thread-a"},
       {"turnId", "turn-a-live"},
       {"itemId", "user-a1"}}));
  spin(10);
  const middle::LocalPromptData *beforeAck =
      localPrompt(shell, QStringLiteral("prompt A1"));
  result &=
      expect(beforeAck && beforeAck->state == middle::PromptState::InFlight,
             "materialization alone cannot acknowledge A1");

  editor->setPlainText(QStringLiteral("unsent shared draft"));
  result &= expect(selectThread(list, "thread-b"),
                   "B can be selected while A remains active");
  result &= expect(editor->toPlainText() ==
                       QStringLiteral("unsent shared draft"),
                   "thread navigation retains the shared composer draft");
  result &= expect(!peer.waitFor("thread.read", "thread-b", 100).has_value(),
                   "returning to hydrated B does not reread its history");
  result &= expect(submit(editor, QStringLiteral("prompt B1")),
                   "B1 is admitted while A1 is in flight");
  result &= expect(
      list && list->item(0) &&
          list->item(0)->data(Qt::UserRole).toString().toStdString() ==
              "thread-b" &&
          list->currentItem() == list->item(0),
      "real prompt admission immediately promotes B under Recent");
  const auto startB = peer.waitFor("turn.start", "thread-b");
  result &=
      expect(startB.has_value(), "different threads dispatch independently");
  if (startB)
    startBCorrelation =
        startB->value("correlationId", std::string{});

  result &= peer.send(presentation::result(
      sequence++, 1, "turn.start",
      startA->value("correlationId", std::string{}), true,
      {{"turn", {{"id", "turn-a-live"}, {"status", "inProgress"}}}},
      Authority::Merge,
      {{"threadId", "thread-a"}, {"turnId", "turn-a-live"}}));
  const auto steerA = peer.waitFor("turn.steer", "thread-a");
  result &=
      expect(steerA.has_value(), "A1's real background ack releases queued A2");
  result &= expect(
      list && list->currentItem() &&
          list->currentItem()->data(Qt::UserRole).toString().toStdString() ==
              "thread-b",
      "a background acknowledgment does not change selection");

  peer.discard();
  result &= expect(selectThread(list, "thread-a"),
                   "switching back restores A's retained prompt state");
  spin(10);
  result &= expect(
      !peer.waitFor("thread.read", "thread-a", 100).has_value(),
      "switching back to hydrated A does not issue a destructive reread");
  const middle::LocalPromptData *accepted =
      localPrompt(shell, QStringLiteral("prompt A1"));
  result &= expect(accepted && accepted->state == middle::PromptState::Accepted,
                   "only the correlated turn result acknowledges A1");
  return result;
}

bool ShellFlow::verifyReconnectHydration() {
  bool result = true;

  result &= peer.send(presentation::event(
      sequence++, 1, "thread.upsert", {{"thread", thread("thread-c", "C")}},
      Authority::Merge, {{"threadId", "thread-c"}}));
  spin(5);
  result &= expect(selectThread(list, "thread-c"),
                   "C is selected for hydration supersession coverage");
  const auto readC1 = peer.waitFor("thread.read", "thread-c");
  result &= expect(readC1.has_value(), "C issues its first hydration read");
  if (!readC1)
    return false;
  result &= expect(submit(editor, QStringLiteral("prompt C queued across restart")),
                   "a prompt can queue behind C's in-flight hydration");
  result &= expect(!peer.waitFor("turn.start", "thread-c", 100).has_value(),
                   "the queued prompt waits for authoritative hydration");

  result &= peer.send(presentation::event(
      sequence++, 1, "connection.provider",
      {{"generation", std::uint64_t{1}}, {"state", "disconnected"}},
      Authority::Replace));
  spin(10);
  const auto *status =
      shell.findChild<QLabel *>(QStringLiteral("globalStatusLabel"));
  result &= expect(status && status->text() == QStringLiteral("Provider unavailable"),
                   "provider loss cannot leave the shell visibly Ready");
  peer.discard();
  result &= expect(submit(editor, QStringLiteral("provider unavailable")),
                   "the editable composer reaches the guarded admission boundary");
  spin(5);
  result &= expect(editor->toPlainText() == QStringLiteral("provider unavailable") &&
                       !peer.has("turn.start") && !peer.has("turn.steer"),
                   "provider loss rejects a stale hidden-thread destination without clearing the draft");

  generation = 2;
  result &= peer.send(presentation::event(sequence++, 2, "connection.lifecycle",
                                          {{"state", "disconnected"}},
                                          Authority::Merge));
  result &= peer.send(presentation::event(sequence++, 2, "connection.lifecycle",
                                          {{"state", "connected"}},
                                          Authority::Merge));
  result &=
      peer.send(presentation::event(sequence++, 2, "connection.bridge",
                                    {{"state", "opened"},
                                     {"connectionId", "test-controller-2"},
                                     {"role", "controller"}},
                                    Authority::Merge));
  result &= peer.send(presentation::event(
      sequence++, 2, "connection.provider",
      {{"generation", std::uint64_t{2}}, {"state", "ready"}},
      Authority::Replace));
  const auto reconnectedList = peer.waitFor("threads.list");
  result &= expect(reconnectedList.has_value(),
                   "the ready provider requests a fresh authoritative list");
  if (!reconnectedList)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "threads.list",
      reconnectedList->value("correlationId", std::string{}), true,
      {{"threads",
        nlohmann::json::array({thread("thread-a", "A"),
                               thread("thread-b", "B"),
                               thread("thread-c", "C")})}},
      Authority::Merge));
  const auto readC2 = peer.waitFor("thread.read", "thread-c");
  result &= expect(readC2.has_value(),
                   "the new connection owns a fresh hydration read");
  if (!readC2)
    return false;
  const auto readB2 = peer.waitFor("thread.read", "thread-b");
  result &= expect(readB2.has_value(),
                   "the restart also rehydrates B's interrupted prompt queue");
  if (!readB2)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "thread.read",
      readB2->value("correlationId", std::string{}), true,
      {{"thread", thread("thread-b", "B")}}, Authority::Replace,
      {{"threadId", "thread-b"}}));
  const auto resumedStartB = peer.waitFor("turn.start", "thread-b");
  result &= expect(resumedStartB.has_value(),
                   "B's interrupted in-flight prompt is reissued after hydration");
  if (!resumedStartB)
    return false;
  startBCorrelation =
      resumedStartB->value("correlationId", std::string{});
  result &= peer.send(presentation::result(
      sequence++, 2, "thread.read",
      readC2->value("correlationId", std::string{}), true,
      {{"thread", threadWithAgentMessage("thread-c", "C", "current C marker")}},
      Authority::Replace, {{"threadId", "thread-c"}}));
  result &= peer.send(
      presentation::result(sequence++, 2, "thread.read",
                           readC1->value("correlationId", std::string{}), true,
                           {{"thread", thread("thread-c", "stale C")}},
                           Authority::Replace, {{"threadId", "thread-c"}}));
  result &= completeSettingsRefresh("thread-c");
  const auto resumedStartC = peer.waitFor("turn.start", "thread-c");
  result &= expect(
      resumedStartC.has_value(),
      "a transient provider restart preserves and dispatches C's queued prompt");
  if (!resumedStartC)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.start",
      resumedStartC->value("correlationId", std::string{}), true,
      {{"turn", {{"id", "turn-c-reconnected"}, {"status", "completed"}}}},
      Authority::Replace,
      {{"threadId", "thread-c"}, {"turnId", "turn-c-reconnected"}}));
  spin(10);
  const middle::LocalPromptData *preserved =
      localPrompt(shell, QStringLiteral("prompt C queued across restart"));
  result &= expect(preserved && preserved->state == middle::PromptState::Accepted,
                   "the reconnected turn result acknowledges the preserved prompt");
  result &= expect(hasAgentMessage(shell, QStringLiteral("current C marker")),
                   "a late successful stale read cannot replace newer cards");
  peer.discard();
  result &= expect(submit(editor, QStringLiteral("prompt C1")),
                   "C remains hydrated after the stale read callback");
  const auto startC = peer.waitFor("turn.start", "thread-c");
  result &= expect(startC.has_value() && !peer.has("thread.read"),
                   "a stale read cannot overwrite newer hydration state");
  return result;
}

bool ShellFlow::verifyTerminalCallback() {
  bool result = true;
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.start",
      startBCorrelation,
      false, {{"code", -32001}, {"message", "transport cancelled"}},
      Authority::None, {{"threadId", "thread-b"}}));
  spin(10);
  result &= expect(selectThread(list, "thread-b"),
                   "B remains selectable after reconnection");
  const middle::LocalPromptData *cancelled =
      localPrompt(shell, QStringLiteral("prompt B1"));
  result &= expect(cancelled && cancelled->state == middle::PromptState::Failed,
                   "a real failure of the reissued request remains terminal");
  return result;
}

bool ShellFlow::verifyBoundedChildHydration() {
  bool result = true;

  peer.discard();
  result &=
      peer.send(presentation::event(sequence++, 2, "agents.activity.upsert",
                                    {{"activity",
                                      {{"id", "child-failure"},
                                       {"type", "subAgentActivity"},
                                       {"status", "started"},
                                       {"agentThreadId", "child-failure"}}}},
                                    Authority::Merge,
                                    {{"threadId", "thread-b"},
                                     {"turnId", "turn-b"},
                                     {"itemId", "child-failure"}}));
  const auto childRead = peer.waitFor("thread.read", "child-failure");
  result &= expect(childRead.has_value(),
                   "a started historical child is hydrated once");
  if (!childRead)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "thread.read",
      childRead->value("correlationId", std::string{}), false,
      {{"code", -32002}, {"message", "child hydration failed"}},
      Authority::None, {{"threadId", "child-failure"}}));
  spin(10);
  result &=
      expect(!peer.waitFor("thread.read", "child-failure", 100).has_value(),
             "a failed child hydration does not enter an automatic retry loop");

  result &= expect(selectThread(list, "thread-a") &&
                       selectThread(list, "thread-b"),
                   "explicit navigation returns to the failed child's parent");
  const auto retriedChildRead =
      peer.waitFor("thread.read", "child-failure");
  result &= expect(retriedChildRead.has_value(),
                   "explicit parent navigation retries one failed child read");
  if (!retriedChildRead)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "thread.read",
      retriedChildRead->value("correlationId", std::string{}), true,
      {{"thread", threadWithAgentMessage("child-failure", "Child",
                                          "completed child result")}},
      Authority::Replace, {{"threadId", "child-failure"}}));
  spin(10);

  auto *inspector = shell.findChild<QFrame *>(QStringLiteral("inspector"));
  auto *tabs = inspector ? inspector->findChild<QTabWidget *>(
                               QString{}, Qt::FindDirectChildrenOnly)
                         : nullptr;
  if (tabs) {
    tabs->setCurrentIndex(1);
    spin();
  }
  result &= expect(inspector && tabs &&
                       hasPresentedText(*inspector,
                                        QStringLiteral("Completed")) &&
                       !hasPresentedText(*inspector,
                                         QStringLiteral("Running")),
                   "retried child completion replaces the stale Running badge");
  return result;
}

bool ShellFlow::verifyNotFoundRecovery() {
  bool result = true;

  peer.discard();
  result &= peer.send(presentation::event(
      sequence++, 2, "thread.upsert", {{"thread", thread("thread-d", "D")}},
      Authority::Merge, {{"threadId", "thread-d"}}));
  spin(5);
  result &= expect(selectThread(list, "thread-d"),
                   "D is selected for thread-not-found recovery coverage");
  const auto readD = peer.waitFor("thread.read", "thread-d");
  result &= expect(readD.has_value(), "D is hydrated before its first prompt");
  if (!readD)
    return false;
  result &= peer.send(
      presentation::result(sequence++, 2, "thread.read",
                           readD->value("correlationId", std::string{}), true,
                           {{"thread", thread("thread-d", "D")}},
                           Authority::Replace, {{"threadId", "thread-d"}}));
  result &= completeSettingsRefresh("thread-d");
  spin(5);
  result &= expect(submit(editor, QStringLiteral("prompt D1")),
                   "D1 is admitted before recovery");
  const auto firstStartD = peer.waitFor("turn.start", "thread-d");
  result &= expect(firstStartD.has_value(), "D1 begins with turn.start");
  if (!firstStartD)
    return false;
  const std::string firstDClientId =
      firstStartD->value("data", nlohmann::json::object())
          .value("clientUserMessageId", std::string{});
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.start",
      firstStartD->value("correlationId", std::string{}), false,
      {{"code", -32004}, {"message", "thread thread-d not found"}},
      Authority::None, {{"threadId", "thread-d"}}));
  const auto firstResumeD = peer.waitFor("thread.resume", "thread-d");
  result &= expect(firstResumeD.has_value(),
                   "thread-not-found triggers one explicit resume");
  if (!firstResumeD)
    return false;
  result &= peer.send(
      presentation::result(sequence++, 2, "thread.resume",
                           firstResumeD->value("correlationId", std::string{}),
                           true, {{"thread", thread("thread-d", "D")}},
                           Authority::Merge, {{"threadId", "thread-d"}}));
  const auto retriedStartD = peer.waitFor("turn.start", "thread-d");
  result &= expect(retriedStartD.has_value() &&
                       retriedStartD->value("data", nlohmann::json::object())
                               .value("clientUserMessageId", std::string{}) ==
                           firstDClientId,
                   "D1 retries once with the same client message identity");
  if (!retriedStartD)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.start",
      retriedStartD->value("correlationId", std::string{}), true,
      {{"turn", {{"id", "turn-d"}, {"status", "inProgress"}}}},
      Authority::Merge, {{"threadId", "thread-d"}, {"turnId", "turn-d"}}));
  spin(5);

  result &= expect(submit(editor, QStringLiteral("prompt D2")),
                   "the prompt after recovery remains dispatchable");
  const auto firstSteerD = peer.waitFor("turn.steer", "thread-d");
  result &= expect(firstSteerD.has_value(),
                   "the next prompt steers the recovered active turn");
  if (!firstSteerD)
    return false;
  const std::string secondDClientId =
      firstSteerD->value("data", nlohmann::json::object())
          .value("clientUserMessageId", std::string{});
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.steer",
      firstSteerD->value("correlationId", std::string{}), false,
      {{"code", -32004}, {"message", "thread thread-d not found"}},
      Authority::None, {{"threadId", "thread-d"}}));
  const auto secondResumeD = peer.waitFor("thread.resume", "thread-d");
  result &= expect(secondResumeD.has_value(),
                   "D2 receives its single bounded recovery attempt");
  if (!secondResumeD)
    return false;
  result &= expect(submit(editor, QStringLiteral("prompt D during recovery")),
                   "another prompt remains admissible during recovery");
  spin(10);
  result &= expect(!peer.waitFor("turn.steer", "thread-d", 100).has_value(),
                   "an in-flight resume gates dispatch");
  result &= peer.send(
      presentation::result(sequence++, 2, "thread.resume",
                           secondResumeD->value("correlationId", std::string{}),
                           true,
                           {{"thread",
                             thread("thread-d", "D", "active", "turn-d")}},
                           Authority::Merge, {{"threadId", "thread-d"}}));
  const auto retriedSteerD = peer.waitFor("turn.steer", "thread-d");
  result &= expect(retriedSteerD.has_value() &&
                       retriedSteerD->value("data", nlohmann::json::object())
                               .value("clientUserMessageId", std::string{}) ==
                           secondDClientId,
                   "D2 retry also preserves its exact identity");
  if (!retriedSteerD)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.steer",
      retriedSteerD->value("correlationId", std::string{}), false,
      {{"code", -32004}, {"message", "thread thread-d not found again"}},
      Authority::None, {{"threadId", "thread-d"}}));
  spin(10);
  const middle::LocalPromptData *failedD2 =
      localPrompt(shell, QStringLiteral("prompt D2"));
  result &= expect(
      failedD2 && failedD2->state == middle::PromptState::Failed &&
          !peer.waitFor("thread.resume", "thread-d", 100).has_value(),
      "a repeated not-found is terminal and cannot start a second recovery");
  const auto postRecoverySteerD = peer.waitFor("turn.steer", "thread-d");
  result &= expect(postRecoverySteerD.has_value(),
                   "the queued prompt dispatches after recovery finishes");
  if (!postRecoverySteerD)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.steer",
      postRecoverySteerD->value("correlationId", std::string{}), true,
      {{"turn", {{"id", "turn-d"}, {"status", "inProgress"}}}},
      Authority::Merge, {{"threadId", "thread-d"}, {"turnId", "turn-d"}}));
  return result;
}

bool ShellFlow::verifyFailedHydration() {
  bool result = true;

  peer.discard();
  result &= peer.send(presentation::event(
      sequence++, 2, "thread.upsert", {{"thread", thread("thread-e", "E")}},
      Authority::Merge, {{"threadId", "thread-e"}}));
  spin(5);
  result &= expect(selectThread(list, "thread-e"),
                   "E is selected for failed-hydration admission coverage");
  const auto readE = peer.waitFor("thread.read", "thread-e");
  result &= expect(readE.has_value(), "E requests its first hydration read");
  if (!readE)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "thread.read",
      readE->value("correlationId", std::string{}), false,
      {{"code", -32005}, {"message", "thread hydration failed"}},
      Authority::None, {{"threadId", "thread-e"}}));
  spin(10);
  result &= expect(submit(editor, QStringLiteral("prompt E1")),
                   "the composer delivers E1 to the admission boundary");
  spin(10);
  result &=
      expect(editor && editor->toPlainText() == QStringLiteral("prompt E1"),
             "failed hydration rejects admission without clearing the draft");
  result &=
      expect(!peer.waitFor("turn.start", "thread-e", 100).has_value() &&
                 !peer.waitFor("thread.read", "thread-e", 100).has_value(),
             "failed hydration cannot send or enter an automatic read loop");
  return result;
}

bool ShellFlow::verifyOptimisticNewThread() {
  bool result = true;
  peer.discard();

  auto *newThread =
      shell.findChild<QPushButton *>(QStringLiteral("threadNewButton"));
  bool dialogOpened = false;
  QTimer::singleShot(0, &shell, [&dialogOpened] {
    if (auto *dialog = qobject_cast<QDialog *>(
            QApplication::activeModalWidget())) {
      dialogOpened = true;
      dialog->accept();
    }
  });
  result &= expect(newThread, "the real New thread action is available");
  if (!newThread)
    return false;
  newThread->click();
  spin(10);

  auto findThreadItem = [this](std::string_view id) -> QListWidgetItem * {
    if (!list)
      return nullptr;
    for (int row = 0; row < list->count(); ++row) {
      QListWidgetItem *item = list->item(row);
      if (item && item->data(Qt::UserRole).toString().toStdString() == id)
        return item;
    }
    return nullptr;
  };
  QListWidgetItem *draft = findThreadItem("draft:new-thread");
  result &= expect(
      dialogOpened && draft && list->currentItem() == draft &&
          draft->data(Qt::UserRole + 6).toBool(),
      "accepting the dialog immediately selects one animated draft row");
  if (!draft)
    return false;

  result &= expect(submit(editor, QStringLiteral("first new-thread prompt")),
                   "the selected optimistic draft admits its first prompt");
  const auto create = peer.waitFor("thread.create");
  result &= expect(create.has_value(),
                   "the optimistic draft dispatches thread.create");
  if (!create)
    return false;
  result &= peer.send(presentation::result(
      sequence++, generation, "thread.create",
      create->value("correlationId", std::string{}), true,
      {{"thread", {{"id", "thread-new"},
                    {"name", "New thread"},
                    {"cwd", "/workspace/new"},
                    {"status", "idle"}}}},
      Authority::Merge, {{"threadId", "thread-new"}}));

  const auto start = peer.waitFor("turn.start", "thread-new");
  result &= expect(start.has_value(),
                   "thread.create promotion dispatches the retained prompt");
  if (!start)
    return false;
  QListWidgetItem *promoted = findThreadItem("thread-new");
  result &= expect(
      promoted == draft && promoted->data(Qt::UserRole + 6).toBool(),
      "thread.create rekeys the same visible item while acknowledgment is pending");

  result &= peer.send(presentation::result(
      sequence++, generation, "turn.start",
      start->value("correlationId", std::string{}), true,
      {{"turn", {{"id", "turn-new"}, {"status", "inProgress"}}}},
      Authority::Merge,
      {{"threadId", "thread-new"}, {"turnId", "turn-new"}}));
  spin(10);
  result &= expect(findThreadItem("thread-new") == draft &&
                       !draft->data(Qt::UserRole + 6).toBool(),
                   "turn acknowledgment canonicalizes the same thread item");
  return result;
}

bool ShellFlow::verifyPendingResolutionBoundary() {
  bool result = true;
  peer.discard();
  auto pending = [this](int id, const char *command) {
    return peer.send(presentation::event(
        sequence++, generation, "pending-request.upsert",
        {{"requestId", id},
         {"category", "command-approval"},
         {"request", {{"command", command}, {"cwd", "/tmp"}}}},
        Authority::Merge,
        {{"threadId", "thread-new"}, {"requestId", id}}));
  };
  auto *accept = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestAcceptButton"));
  auto *reject = shell.findChild<QPushButton *>(
      QStringLiteral("pendingRequestRejectButton"));
  result &= expect(accept && reject,
                   "the selected request exposes typed response actions");
  if (!accept || !reject)
    return false;

  result &= pending(91, "first approval");
  spin(30);
  result &= expect(accept->isEnabled(),
                   "the current controller can answer a current request");
  accept->click();
  accept->click();
  const auto accepted = peer.waitFor("pending-request.resolve");
  result &= expect(
      accepted && accepted->value("data", nlohmann::json::object())
                              .value("requestId", 0) == 91 &&
          accepted->value("data", nlohmann::json::object())
                  .value("result", nlohmann::json::object())
                  .value("decision", std::string{}) == "accept",
      "the first response preserves the native request identity and decision");
  result &= expect(!peer.waitFor("pending-request.resolve", {}, 100).has_value(),
                   "a repeated click cannot resolve the same request twice");
  spin(30);
  result &= expect(!accept->isEnabled() && !reject->isEnabled(),
                   "a resolving request disables all response actions");
  result &= peer.send(presentation::event(
      sequence++, generation, "pending-request.removed",
      nlohmann::json::object(), Authority::Remove,
      {{"threadId", "thread-new"}, {"requestId", 91}}));

  result &= pending(92, "observer approval");
  result &= peer.send(presentation::event(
      sequence++, generation, "connection.controller",
      {{"controllerConnectionId", "different-controller"}},
      Authority::Replace));
  spin(30);
  result &= expect(!accept->isEnabled() && !reject->isEnabled(),
                   "an observer can inspect but cannot answer a request");
  accept->click();
  result &= expect(!peer.waitFor("pending-request.resolve", {}, 100).has_value(),
                   "disabled observer actions emit no response");

  result &= peer.send(presentation::event(
      sequence++, generation, "connection.controller",
      {{"controllerConnectionId", "test-controller-2"}}, Authority::Replace));
  spin(30);
  result &= expect(accept->isEnabled() && reject->isEnabled(),
                   "current controller ownership restores request actions");
  reject->click();
  const auto rejected = peer.waitFor("pending-request.resolve");
  result &= expect(
      rejected && rejected->value("data", nlohmann::json::object())
                          .value("requestId", 0) == 92,
      "the restored controller can resolve the retained request");
  return result;
}

bool runShellFlow(FrontendSession &session, PresentationPeer &peer) {
  return ShellFlow(session, peer).run();
}

bool verifyPendingRequestTextBoundaries() {
  bool inspected = false;
  bool plainText = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    inspected = true;
    const auto labels = dialog->findChildren<QLabel *>();
    const auto command = std::ranges::find_if(labels, [](QLabel *label) {
      return label && label->text().contains(
                          QStringLiteral("<b>untrusted command</b>"));
    });
    plainText = command != labels.end() &&
                (*command)->textFormat() == Qt::PlainText;
    dialog->reject();
  });
  const PendingRequestPresentation request{
      "unsafe-command", "command-approval", "thread-a", 1,
      {{"command", "<b>untrusted command</b>"}}};
  static_cast<void>(PendingRequestDialog::present(request, nullptr));

  bool escapedLink = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    const auto labels = dialog->findChildren<QLabel *>();
    escapedLink = std::ranges::any_of(labels, [](QLabel *label) {
      return label && label->textFormat() == Qt::RichText &&
             label->text().contains(QStringLiteral("&lt;img")) &&
             !label->text().contains(QStringLiteral("<img"));
    });
    dialog->reject();
  });
  const PendingRequestPresentation elicitation{
      "unsafe-link", "mcp-elicitation", "thread-a", 1,
      {{"url", "https://example.invalid/\"><img src=x>"}}};
  static_cast<void>(PendingRequestDialog::present(elicitation, nullptr));

  return expect(inspected && plainText,
                "request text is always rendered literally") &&
         expect(escapedLink, "the explicit MCP link escapes untrusted markup");
}

bool verifyPermissionRequestDisclosure() {
  const nlohmann::json permissions =
      {{"fileSystem",
        {{"write", nlohmann::json::array({"/tmp/<untrusted>"})},
         {"entries",
          nlohmann::json::array(
              {{{"access", "read"},
                {"path", {{"type", "glob_pattern"}, {"pattern", "*.md"}}}}})}}},
       {"network", {{"enabled", true}}},
       {"futureCapability", {{"mode", "bounded"}}}};
  bool completeDisclosure = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    QStringList displayed;
    for (QLabel *label : dialog->findChildren<QLabel *>()) {
      if (label)
        displayed.push_back(label->text());
    }
    const QString all = displayed.join(QLatin1Char('\n'));
    completeDisclosure =
        all.contains(QStringLiteral("File system / write / 1: "
                                    "/tmp/<untrusted>")) &&
        all.contains(QStringLiteral("Network / enabled: Yes")) &&
        all.contains(
            QStringLiteral("futureCapability / mode: bounded"));
    dialog->accept();
  });
  const PendingRequestPresentation request{
      "permissions", "permissions-approval", "thread-a", 1,
      {{"permissions", permissions}, {"reason", "test disclosure"}}};
  const auto response = PendingRequestDialog::present(request, nullptr);
  return expect(completeDisclosure,
                "permission approval discloses known and future fields") &&
         expect(response && response->error.is_null() &&
                    response->result.value("permissions", nlohmann::json{}) ==
                        permissions &&
                    response->result.value("scope", std::string{}) == "turn",
                "permission approval returns the exact disclosed object");
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  auto *configuration =
      utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
  QApplication application(argc, argv);
  core::SNodeC::init(argc, argv);

  codexui::codex::FrontendSession session(*configuration);
  codexui::codex::PresentationPeer peer(
      codexui::codex::FrontendSessionTestPeer::takeClientDescriptor(session));
  const bool result = codexui::codex::verifyPendingRequestTextBoundaries() &&
                      codexui::codex::verifyPermissionRequestDisclosure() &&
                      codexui::codex::runShellFlow(session, peer);
  if (result)
    std::cout << "Shell integration test passed\n";
  return result ? 0 : 1;
}
