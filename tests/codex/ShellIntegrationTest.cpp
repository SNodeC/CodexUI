// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include <core/SNodeC.h>
#include <utils/Config.h>

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/PresentationProtocol.h"
#include "codex/ShellWidget.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QTabWidget>
#include <QThread>
#include <QWheelEvent>

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

  static void deliver(FrontendSession &session, nlohmann::json frame) {
    session.receiveMessage(std::move(frame));
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
                      std::string status = "completed") {
  return {{"id", std::move(id)},
          {"name", std::move(name)},
          {"cwd", "/tmp/codexui-shell-test"},
          {"status", std::move(status)},
          {"turns", nlohmann::json::array()}};
}

nlohmann::json threadWithAgentMessage(std::string id, std::string name,
                                      std::string message) {
  nlohmann::json value = thread(std::move(id), std::move(name));
  value["turns"] = nlohmann::json::array(
      {{{"id", "current-turn"},
        {"status", "completed"},
        {"items", nlohmann::json::array({{{"id", "current-message"},
                                          {"type", "agentMessage"},
                                          {"phase", "final_answer"},
                                          {"text", std::move(message)}}})}}});
  return value;
}

nlohmann::json threadWithPlanAndAgent(std::string id, std::string name) {
  nlohmann::json value = thread(std::move(id), std::move(name));
  value["turns"] = nlohmann::json::array(
      {{{"id", "turn-a"},
        {"status", "completed"},
        {"items",
         nlohmann::json::array({{{"id", "plan-a"},
                                 {"type", "plan"},
                                 {"text", "retained plan marker"}},
                                {{"id", "agent-a"},
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

bool runShellFlow(FrontendSession &session, PresentationPeer &peer) {
  ShellWidget shell(session);
  shell.resize(1500, 850);
  shell.show();
  spin(10);
  bool result = true;

  auto *conversation = dynamic_cast<middle::ConversationView *>(
      shell.findChild<QWidget *>(QStringLiteral("conversationScroll")));
  result &= expect(conversation, "the shell owns the conversation viewport");
  if (!conversation)
    return false;
  const QPointF wheelPosition(conversation->viewport()->rect().center());
  QWheelEvent wheel(
      wheelPosition,
      conversation->viewport()->mapToGlobal(wheelPosition.toPoint()), QPoint(),
      QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(conversation->viewport(), &wheel);
  result &= expect(wheel.isAccepted(),
                   "native wheel delivery crosses the shell router once");

  std::uint64_t sequence = 1;
  result &= peer.send(
      presentation::event(sequence++, 1, "connection.lifecycle",
                          {{"state", "connected"}}, Authority::Merge));
  result &= peer.send(presentation::event(sequence++, 1, "connection.bridge",
                                          {{"state", "opened"},
                                           {"connectionId", "test-controller"},
                                           {"role", "controller"}},
                                          Authority::Merge));
  result &= peer.send(presentation::event(
      sequence++, 1, "thread.upsert", {{"thread", thread("thread-a", "A")}},
      Authority::Merge, {{"threadId", "thread-a"}}));
  result &= peer.send(presentation::event(
      sequence++, 1, "thread.upsert", {{"thread", thread("thread-b", "B")}},
      Authority::Merge, {{"threadId", "thread-b"}}));
  spin(10);
  peer.discard(); // bridge bootstrap operations are outside this scenario.

  auto *list = shell.findChild<QListWidget *>(QStringLiteral("threadList"));
  auto *editor = shell.findChild<codexui::ExpandingPromptEditor *>(
      QStringLiteral("upcomingPromptEditor"));
  result &= expect(selectThread(list, "thread-a"),
                   "the visible A row becomes the prompt destination");
  const auto readA = peer.waitFor("thread.read", "thread-a");
  result &= expect(readA.has_value(), "selecting A requests hydration");
  if (!readA)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 1, "thread.read",
      readA->value("correlationId", std::string{}), true,
      {{"thread", threadWithPlanAndAgent("thread-a", "A")}}, Authority::Replace,
      {{"threadId", "thread-a"}}));
  spin(10);

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
      {{"threadId", "thread-a"}, {"turnId", "turn-a"}, {"itemId", "user-a1"}}));
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
  const auto readB = peer.waitFor("thread.read", "thread-b");
  result &= expect(readB.has_value(), "selecting B requests its own hydration");
  if (!readB)
    return false;
  result &= peer.send(
      presentation::result(sequence++, 1, "thread.read",
                           readB->value("correlationId", std::string{}), true,
                           {{"thread", thread("thread-b", "B")}},
                           Authority::Replace, {{"threadId", "thread-b"}}));
  spin(10);
  result &= expect(submit(editor, QStringLiteral("prompt B1")),
                   "B1 is admitted while A1 is in flight");
  const auto startB = peer.waitFor("turn.start", "thread-b");
  result &=
      expect(startB.has_value(), "different threads dispatch independently");

  result &= peer.send(presentation::result(
      sequence++, 1, "turn.start",
      startA->value("correlationId", std::string{}), true,
      {{"turn", {{"id", "turn-a"}, {"status", "inProgress"}}}},
      Authority::Merge, {{"threadId", "thread-a"}, {"turnId", "turn-a"}}));
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
  auto *inspector = shell.findChild<QFrame *>(QStringLiteral("inspector"));
  auto *inspectorTabs = inspector ? inspector->findChild<QTabWidget *>(
                                        QString{}, Qt::FindDirectChildrenOnly)
                                  : nullptr;
  result &= expect(
      inspector && inspectorTabs &&
          hasPresentedText(*inspector, QStringLiteral("retained plan marker")),
      "A's retained plan is present immediately after return");
  if (inspectorTabs) {
    inspectorTabs->setCurrentIndex(1);
    spin();
  }
  result &= expect(
      inspector &&
          hasPresentedText(*inspector, QStringLiteral("retained agent marker")),
      "A's retained agent detail survives thread navigation");

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
  const auto readC2 = peer.waitFor("thread.read", "thread-c");
  result &= expect(readC2.has_value(),
                   "the new connection owns a fresh hydration read");
  if (!readC2)
    return false;
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
  spin(10);
  result &= expect(hasAgentMessage(shell, QStringLiteral("current C marker")),
                   "a late successful stale read cannot replace newer cards");
  peer.discard();
  result &= expect(submit(editor, QStringLiteral("prompt C1")),
                   "C remains hydrated after the stale read callback");
  const auto startC = peer.waitFor("turn.start", "thread-c");
  result &= expect(startC.has_value() && !peer.has("thread.read"),
                   "a stale read cannot overwrite newer hydration state");

  result &= peer.send(presentation::result(
      sequence++, 2, "turn.start",
      startB ? startB->value("correlationId", std::string{}) : std::string{},
      false, {{"code", -32001}, {"message", "transport cancelled"}},
      Authority::None, {{"threadId", "thread-b"}}));
  spin(10);
  result &= expect(selectThread(list, "thread-b"),
                   "B remains selectable after reconnection");
  const middle::LocalPromptData *cancelled =
      localPrompt(shell, QStringLiteral("prompt B1"));
  result &= expect(cancelled && cancelled->state == middle::PromptState::Failed,
                   "an exact terminal callback after reconnect is never lost");

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
  result &=
      expect(selectThread(list, "thread-b") && selectThread(list, "thread-d"),
             "thread navigation remains available during recovery");
  result &=
      expect(!peer.waitFor("thread.read", "thread-d", 100).has_value() &&
                 !peer.waitFor("turn.steer", "thread-d", 100).has_value(),
             "an in-flight resume gates navigation hydration and dispatch");
  result &= peer.send(
      presentation::result(sequence++, 2, "thread.resume",
                           secondResumeD->value("correlationId", std::string{}),
                           true, {{"thread", thread("thread-d", "D")}},
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

  peer.discard();
  result &= peer.send(presentation::event(
      sequence++, 2, "thread.upsert", {{"thread", thread("thread-f", "F")}},
      Authority::Merge, {{"threadId", "thread-f"}}));
  spin(5);
  result &= expect(selectThread(list, "thread-f"),
                   "F is selected for dispatch/disconnect coverage");
  const auto readF = peer.waitFor("thread.read", "thread-f");
  result &= expect(readF.has_value(), "F is hydrated before admission");
  if (!readF)
    return false;
  result &= peer.send(
      presentation::result(sequence++, 2, "thread.read",
                           readF->value("correlationId", std::string{}), true,
                           {{"thread", thread("thread-f", "F")}},
                           Authority::Replace, {{"threadId", "thread-f"}}));
  spin(5);
  result &= expect(submit(editor, QStringLiteral("prompt F1")),
                   "F1 is admitted while connected");
  FrontendSessionTestPeer::deliver(
      session,
      presentation::event(sequence++, 2, "connection.lifecycle",
                          {{"state", "disconnected"}}, Authority::Merge));
  spin(10);
  const middle::LocalPromptData *disconnectedF1 =
      localPrompt(shell, QStringLiteral("prompt F1"));
  result &= expect(
      disconnectedF1 &&
          !peer.waitFor("turn.start", "thread-f", 100).has_value(),
      "a disconnect crossing the zero-delay timer keeps F1 pending and unsent");
  FrontendSessionTestPeer::deliver(
      session, presentation::event(sequence++, 2, "connection.lifecycle",
                                   {{"state", "connected"}}, Authority::Merge));
  FrontendSessionTestPeer::deliver(
      session, presentation::event(sequence++, 2, "connection.bridge",
                                   {{"state", "opened"},
                                    {"connectionId", "test-controller-3"},
                                    {"role", "controller"}},
                                   Authority::Merge));
  const auto startF = peer.waitFor("turn.start", "thread-f");
  result &= expect(startF.has_value(),
                   "the same queued F1 dispatches after reconnect opens");
  if (!startF)
    return false;
  result &= peer.send(presentation::result(
      sequence++, 2, "turn.start",
      startF->value("correlationId", std::string{}), true,
      {{"turn", {{"id", "turn-f"}, {"status", "inProgress"}}}},
      Authority::Merge, {{"threadId", "thread-f"}, {"turnId", "turn-f"}}));
  return result;
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
  const bool result = codexui::codex::runShellFlow(session, peer);
  if (result)
    std::cout << "Shell integration test passed\n";
  return result ? 0 : 1;
}
