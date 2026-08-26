// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextLayout>
#include <QThread>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace codexui::codex::middle {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

void spin(int milliseconds = 0) {
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (milliseconds > 0)
      QThread::msleep(1);
  } while (timer.elapsed() < milliseconds);
}

VisibleCardData agentCard(const std::string &threadId,
                          const std::string &turnId, int index,
                          QString text = {}) {
  const std::string itemId = "agent-" + std::to_string(index);
  if (text.isEmpty())
    text = QStringLiteral("Codex output line %1 with enough text to wrap a "
                          "little in the viewport.")
               .arg(index);
  return {AuthoritativeItemKey{threadId, turnId, itemId},
          CardKind::AgentMessage,
          threadId,
          turnId,
          itemId,
          AgentMessageData{std::move(text), index % 3 == 0}};
}

ConversationSnapshot conversation(const std::string &threadId, int count) {
  ConversationSnapshot result;
  result.threadId = threadId;
  TurnSection first{"turn:" + threadId + ":1", "turn-1", {}};
  TurnSection second{"turn:" + threadId + ":2", "turn-2", {}};
  for (int index = 0; index < count; ++index)
    (index < count / 2 ? first : second)
        .cards.push_back(agentCard(
            threadId, index < count / 2 ? "turn-1" : "turn-2", index));
  result.sections.push_back(std::move(first));
  result.sections.push_back(std::move(second));
  return result;
}

ConversationCard *card(ConversationView &view, const std::string &key) {
  for (QWidget *widget : view.findChildren<QWidget *>()) {
    auto *candidate = dynamic_cast<ConversationCard *>(widget);
    if (!candidate)
      continue;
    if (candidate->property("conversationAnchorKey").toString() ==
        QString::fromStdString(key))
      return candidate;
  }
  return nullptr;
}

std::pair<std::string, int> firstVisible(ConversationView &view) {
  std::vector<ConversationCard *> cards;
  for (QWidget *widget : view.findChildren<QWidget *>())
    if (auto *candidate = dynamic_cast<ConversationCard *>(widget))
      cards.push_back(candidate);
  std::ranges::sort(cards, [&view](QWidget *left, QWidget *right) {
    return left->mapTo(view.viewport(), QPoint{}).y() <
           right->mapTo(view.viewport(), QPoint{}).y();
  });
  for (ConversationCard *candidate : cards) {
    const int top = candidate->mapTo(view.viewport(), QPoint{}).y();
    if (top + candidate->height() >= 0)
      return {
          candidate->property("conversationAnchorKey").toString().toStdString(),
          top};
  }
  return {};
}

void wheel(ConversationView &view, int pixelDelta) {
  const QPointF local(view.viewport()->rect().center());
  QWheelEvent event(local, view.viewport()->mapToGlobal(local.toPoint()),
                    QPoint(), QPoint(0, pixelDelta), Qt::NoButton,
                    Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(view.viewport(), &event);
  spin();
}

void mouseWheelNotch(ConversationView &view, int angleDelta) {
  const QPointF local(view.viewport()->rect().center());
  QWheelEvent event(local, view.viewport()->mapToGlobal(local.toPoint()),
                    QPoint(), QPoint(0, angleDelta), Qt::NoButton,
                    Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(view.viewport(), &event);
  spin();
}

bool testFollowPauseAndStableAnchor() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationSnapshot snapshot = conversation("thread-a", 34);
  bool result = expect(view.reconcile(snapshot), "initial projection renders");
  spin();
  QScrollArea nativeReference;
  nativeReference.setWidgetResizable(true);
  auto *nativeContent = new QWidget;
  nativeContent->setMinimumHeight(5000);
  nativeReference.setWidget(nativeContent);
  nativeReference.resize(view.size());
  nativeReference.show();
  spin();
  result &=
      expect(view.verticalScrollBar()->singleStep() ==
                 nativeReference.verticalScrollBar()->singleStep(),
             "conversation line-step matches the previous native QScrollArea");
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "a new thread starts following at its real bottom");

  const int oldValue = view.verticalScrollBar()->value();
  snapshot.sections.back().cards.push_back(agentCard("thread-a", "turn-2", 34));
  result &= expect(view.reconcile(snapshot), "a new card materializes");
  int previous = view.verticalScrollBar()->value();
  bool monotonic = previous >= oldValue;
  QElapsedTimer animation;
  animation.start();
  while (animation.elapsed() < 400 && !view.isAtBottom()) {
    spin(8);
    const int current = view.verticalScrollBar()->value();
    monotonic = monotonic && current >= previous;
    previous = current;
  }
  result &= expect(monotonic && view.isAtBottom(),
                   "follow animation is monotonic and reaches the new bottom");

  const int beforeWheelNotch = view.verticalScrollBar()->value();
  mouseWheelNotch(view, 120);
  result &= expect(
      view.mode() == ConversationView::Mode::Paused && !view.isAtBottom() &&
          beforeWheelNotch - view.verticalScrollBar()->value() ==
              std::min(beforeWheelNotch,
                       view.verticalScrollBar()->singleStep() *
                           std::max(1, QApplication::wheelScrollLines())),
      "native mouse-wheel handling uses the configured line "
      "distance and "
      "pauses following immediately");
  const auto anchor = firstVisible(view);
  result &=
      expect(!anchor.first.empty(), "paused view has a visible card anchor");

  // Reflow a card above the anchor and append another card in one projection.
  const auto anchorPosition =
      std::ranges::find_if(snapshot.sections.front().cards,
                           [&anchor](const VisibleCardData &candidate) {
                             return stableKey(candidate.key) == anchor.first;
                           });
  if (anchorPosition != snapshot.sections.front().cards.begin() &&
      anchorPosition != snapshot.sections.front().cards.end()) {
    auto &message = std::get<AgentMessageData>((anchorPosition - 1)->payload);
    message.text += QStringLiteral(
        "\nA reflowing upstream update.\nA second line.\nA third line.");
  }
  snapshot.sections.back().cards.push_back(agentCard("thread-a", "turn-2", 35));
  result &= expect(view.reconcile(snapshot),
                   "paused incoming changes still materialize");
  spin();
  const auto after = firstVisible(view);
  result &= expect(after.first == anchor.first &&
                       std::abs(after.second - anchor.second) <= 1,
                   "paused reconciliation preserves key and pixel anchor");
  result &=
      expect(card(view, stableKey(snapshot.sections.back().cards.back().key)),
             "paused mode never withholds a later card");

  const int unchangedValue = view.verticalScrollBar()->value();
  const auto unchangedAnchor = firstVisible(view);
  result &= expect(!view.reconcile(snapshot),
                   "an identical visible projection is a true no-op");
  spin();
  result &= expect(view.verticalScrollBar()->value() == unchangedValue &&
                       firstVisible(view) == unchangedAnchor,
                   "a no-op changes neither scroll nor visible geometry");

  while (!view.isAtBottom())
    wheel(view, -300);
  view.verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
  spin();
  const auto pageStepAnchor = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       !pageStepAnchor.first.empty(),
                   "a scrollbar page action pauses at its resulting anchor");
  auto &upstream = std::get<AgentMessageData>(
      snapshot.sections.front().cards.front().payload);
  upstream.text += QStringLiteral(
      "\nTrack-action upstream reflow.\nSecond line.\nThird line.");
  result &= expect(view.reconcile(snapshot),
                   "page-action coverage applies an upstream reflow");
  spin();
  const auto afterPageStepReflow = firstVisible(view);
  result &= expect(
      afterPageStepReflow.first == pageStepAnchor.first &&
          std::abs(afterPageStepReflow.second - pageStepAnchor.second) <= 1,
      "page-action scroll ownership survives later reflow");
  return result;
}

bool testThreadLocalScrollAndComposerExtent() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationSnapshot first = conversation("thread-a", 30);
  ConversationSnapshot second = conversation("thread-b", 26);
  view.reconcile(first);
  spin();
  wheel(view, 220);
  const auto saved = firstVisible(view);
  bool result = expect(view.mode() == ConversationView::Mode::Paused,
                       "first thread is paused before switching");

  view.reconcile(second);
  spin();
  result &= expect(view.mode() == ConversationView::Mode::Following &&
                       view.isAtBottom(),
                   "a new thread does not inherit another thread's pause");
  view.reconcile(first);
  spin();
  const auto restored = firstVisible(view);
  result &= expect(view.mode() == ConversationView::Mode::Paused &&
                       restored.first == saved.first &&
                       std::abs(restored.second - saved.second) <= 1,
                   "switching back restores that thread's own visual anchor");

  const int beforeExtent = view.verticalScrollBar()->maximum();
  const int beforeValue = view.verticalScrollBar()->value();
  view.setTrailingSpaceHeight(137);
  spin();
  result &=
      expect(view.trailingSpaceHeight() == 137 &&
                 view.verticalScrollBar()->maximum() == beforeExtent + 137 &&
                 view.verticalScrollBar()->value() == beforeValue &&
                 view.mode() == ConversationView::Mode::Paused,
             "composer growth adds exact scroll extent without moving content");
  while (!view.isAtBottom())
    wheel(view, -240);
  result &= expect(view.mode() == ConversationView::Mode::Following,
                   "reaching the extended bottom restores following");
  view.setTrailingSpaceHeight(0);
  spin();
  result &=
      expect(view.isAtBottom() && view.trailingSpaceHeight() == 0,
             "composer contraction removes extent and accepts bottom clamp");
  return result;
}

bool testPromptAdmissionFollowOwnership() {
  ConversationView view;
  view.resize(620, 340);
  view.show();
  ConversationSnapshot snapshot = conversation("prompt-follow", 30);
  view.reconcile(snapshot);
  spin();

  view.setTrailingSpaceHeight(120);
  bool result = expect(view.mode() == ConversationView::Mode::Paused,
                       "composer growth preserves the painted viewport");
  view.prepareForLocalPromptAdmission();
  VisibleCardData pending{
      LocalPromptKey{1001},
      CardKind::LocalPrompt,
      "prompt-follow",
      {},
      {},
      LocalPromptData{1001,
                      QStringLiteral("a newly admitted pending prompt"),
                      PromptState::InFlight,
                      0,
                      {}}};
  snapshot.sections.back().cards.push_back(pending);
  view.reconcile(snapshot);
  view.setTrailingSpaceHeight(0);
  QElapsedTimer follow;
  follow.start();
  while (follow.elapsed() < 400 && !view.isAtBottom())
    spin(8);
  ConversationCard *pendingCard = card(view, stableKey(pending.key));
  result &= expect(
      view.mode() == ConversationView::Mode::Following && view.isAtBottom() &&
          pendingCard &&
          pendingCard->mapTo(view.viewport(), QPoint{}).y() +
                  pendingCard->height() <=
              view.viewport()->height(),
      "composer-owned pause resumes and reveals the complete admitted prompt");

  wheel(view, 180);
  const auto userAnchor = firstVisible(view);
  view.setTrailingSpaceHeight(120);
  view.prepareForLocalPromptAdmission();
  VisibleCardData later = pending;
  later.key = LocalPromptKey{1002};
  std::get<LocalPromptData>(later.payload).submissionId = 1002;
  std::get<LocalPromptData>(later.payload).prompt =
      QStringLiteral("must not displace a user-owned reading position");
  snapshot.sections.back().cards.push_back(later);
  view.reconcile(snapshot);
  view.setTrailingSpaceHeight(0);
  spin(40);
  const auto retainedAnchor = firstVisible(view);
  result &=
      expect(view.mode() == ConversationView::Mode::Paused &&
                 retainedAnchor.first == userAnchor.first &&
                 std::abs(retainedAnchor.second - userAnchor.second) <= 1,
             "local admission never overrides an explicit user scroll pause");
  return result;
}

bool testMutableCardsAndCommandOutput() {
  const std::string thread = "card-thread";
  TurnSection section{"turn:cards", "turn", {}};
  section.cards = {
      {AuthoritativeItemKey{thread, "turn", "user"}, CardKind::UserMessage,
       thread, "turn", "user",
       UserMessageData{QStringLiteral("hello **Markdown**\n\n| Value | Rating "
                                      "|\n|---|---|\n| State | 10 |")}},
      {AuthoritativeItemKey{thread, "turn", "agent"}, CardKind::AgentMessage,
       thread, "turn", "agent",
       AgentMessageData{QStringLiteral("answer"), false}},
      {AuthoritativeItemKey{thread, "turn", "command"},
       CardKind::CommandExecution, thread, "turn", "command",
       CommandExecutionData{QStringLiteral("printf test\n\n \t"),
                            QStringLiteral(" \n\t"),
                            QStringLiteral("inProgress"),
                            {},
                            std::nullopt}},
      {AuthoritativeItemKey{thread, "turn", "activity"},
       CardKind::AgentActivity, thread, "turn", "activity",
       AgentActivityData{QStringLiteral("tool"),
                         QStringLiteral("inProgress"),
                         {},
                         QStringLiteral("prompt"),
                         {},
                         {}}},
      {AuthoritativeItemKey{thread, "turn", "reasoning"}, CardKind::Reasoning,
       thread, "turn", "reasoning", ReasoningData{QStringLiteral("summary")}},
      {AuthoritativeItemKey{thread, "turn", "files"}, CardKind::FileChanges,
       thread, "turn", "files",
       FileChangesData{
           QStringLiteral("inProgress"),
           {{QStringLiteral("src/card.cpp"), QStringLiteral("update"), 2, 1}}}},
      {AuthoritativeItemKey{thread, "turn", "plan"}, CardKind::Plan, thread,
       "turn", "plan",
       PlanData{
           QStringLiteral("Keep the card compact"),
           {{QStringLiteral("Inspect data"), QStringLiteral("completed")},
            {QStringLiteral("Render cards"), QStringLiteral("inProgress")}},
           {}}},
      {AuthoritativeItemKey{thread, "turn", "generic"},
       CardKind::GenericActivity, thread, "turn", "generic",
       GenericActivityData{QStringLiteral("custom activity"),
                           {{"detail", "initial"}}}},
      {LocalPromptKey{77},
       CardKind::LocalPrompt,
       thread,
       {},
       {},
       LocalPromptData{77,
                       QStringLiteral("pending\n\nAttached files:\n"
                                      "- [report.pdf](file:///tmp/report.pdf)"),
                       PromptState::InFlight,
                       0,
                       {}}},
  };
  ConversationSnapshot snapshot{thread, {section}, 0, false};
  ConversationView view;
  view.resize(650, 520);
  view.show();
  view.reconcile(snapshot);
  spin();

  std::unordered_map<std::string, ConversationCard *> identities;
  for (const auto &value : snapshot.sections.front().cards)
    identities[stableKey(value.key)] = card(view, stableKey(value.key));
  auto containsLabelText = [](QWidget *parent, const QString &needle) {
    return std::ranges::any_of(
        parent->findChildren<QLabel *>(), [&needle](QLabel *label) {
          return label->text().contains(needle) ||
                 label->property("markdownSource").toString().contains(needle);
        });
  };
  auto *commandCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "command"}})];
  auto *output = dynamic_cast<CommandOutputView *>(
      commandCard->findChild<QTextEdit *>(QStringLiteral("commandOutputView")));
  auto *commandText = dynamic_cast<ContentSizedTextView *>(
      commandCard->findChild<QTextEdit *>(QStringLiteral("commandTextView")));
  bool result = expect(output && output->isHidden(),
                       "empty-line command output has no black surface");
  auto *userCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "user"}})];
  const auto userLabels = userCard->findChildren<QLabel *>();
  result &=
      expect(std::ranges::any_of(
                 userLabels,
                 [](QLabel *label) {
                   return label->property("markdownSource").toString() ==
                              QStringLiteral(
                                  "hello **Markdown**\n\n| Value | Rating |\n"
                                  "|---|---|\n| State | 10 |") &&
                          label->textFormat() == Qt::RichText &&
                          label->text().contains(QStringLiteral("<table"));
                 }),
             "authoritative user messages render GitHub Markdown tables");
  auto *filesCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "files"}})];
  auto *planCard = identities[stableKey(
      CardKey{AuthoritativeItemKey{thread, "turn", "plan"}})];
  result &=
      expect(containsLabelText(
                 filesCard, QStringLiteral("src/card.cpp  ·  Update  +2 −1")) &&
                 containsLabelText(filesCard, QStringLiteral("+2 −1")),
             "file-change cards show paths, kinds, and truthful diff counts");
  result &= expect(
      containsLabelText(planCard, QStringLiteral("Keep the card compact")) &&
          containsLabelText(planCard, QStringLiteral("✓ Inspect data")) &&
          containsLabelText(planCard, QStringLiteral("◉ Render cards")),
      "structured plan cards show explanation and step status");
  result &= expect(
      commandText &&
          commandText->toPlainText() == QStringLiteral("printf test") &&
          commandText->height() < commandText->maximumHeight() &&
          commandText->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
      "short command text trims empty lines and uses its content height");
  auto *pendingCard = identities[stableKey(CardKey{LocalPromptKey{77}})];
  result &= expect(
      std::ranges::any_of(
          pendingCard->findChildren<QLabel *>(),
          [](QLabel *label) {
            return label->property("markdownSource")
                       .toString()
                       .contains(QStringLiteral(
                           "[report.pdf](file:///tmp/report.pdf)")) &&
                   label->textFormat() == Qt::RichText;
          }),
      "pending prompts render file links before authoritative replacement");

  auto &cards = snapshot.sections.front().cards;
  std::get<UserMessageData>(cards[0].payload).text +=
      QStringLiteral(" updated");
  std::get<AgentMessageData>(cards[1].payload).text +=
      QStringLiteral(" updated");
  auto &command = std::get<CommandExecutionData>(cards[2].payload);
  command.output =
      QString(120, QLatin1Char('x')) + QStringLiteral("\nvisible\n\n \t");
  command.status = QStringLiteral("completed");
  std::get<AgentActivityData>(cards[3].payload).resultText =
      QStringLiteral("result");
  std::get<ReasoningData>(cards[4].payload).summary += QStringLiteral(" more");
  std::get<FileChangesData>(cards[5].payload)
      .changes.push_back(
          {QStringLiteral("tests/card.cpp"), QStringLiteral("add"), 3, 0});
  std::get<PlanData>(cards[6].payload).steps[1].status =
      QStringLiteral("completed");
  auto &generic = std::get<GenericActivityData>(cards[7].payload);
  generic.type = QStringLiteral("updated custom activity");
  generic.raw["detail"] = "updated";
  std::get<LocalPromptData>(cards[8].payload).state = PromptState::Failed;
  std::get<LocalPromptData>(cards[8].payload).error = QStringLiteral("error");
  result &=
      expect(view.reconcile(snapshot), "all card types accept visible updates");
  const int immediateOuterRange = view.verticalScrollBar()->maximum();
  const int immediateCommandHeight = commandCard->height();
  const int immediatePreferredOutputHeight = output->sizeHint().height();
  spin();
  result &=
      expect(view.verticalScrollBar()->maximum() == immediateOuterRange &&
                 commandCard->height() == immediateCommandHeight &&
                 output->sizeHint().height() == immediatePreferredOutputHeight,
             "command output has no delayed outer geometry settlement");
  for (const auto &value : cards)
    result &= expect(card(view, stableKey(value.key)) ==
                         identities[stableKey(value.key)],
                     "same-key same-kind card updates in place");
  result &=
      expect(!output->isHidden() && output->minimumHeight() == 0 &&
                 output->maximumHeight() == 220 &&
                 output->toPlainText().endsWith(QStringLiteral("visible")) &&
                 output->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
             "visible output trims empty lines and grows with the 220px cap");

  QString longOutput;
  for (int line = 0; line < 80; ++line)
    longOutput += QStringLiteral("line %1 with terminal output\n").arg(line);
  output->setOutput(longOutput);
  view.resize(650, 520);
  spin();
  result &= expect(output->verticalScrollBar()->maximum() > 0 &&
                       output->followsLatest(),
                   "long command output exposes its own scrollbar and follows");

  // A scrollbar move immediately after an output update is user-owned.  It
  // must not be overwritten by a deferred follow-latest settlement.
  output->setOutput(longOutput + QStringLiteral("new output before gesture\n"));
  const int immediateGestureValue = output->verticalScrollBar()->maximum() / 3;
  output->verticalScrollBar()->setValue(immediateGestureValue);
  spin();
  result &=
      expect(!output->followsLatest() &&
                 output->verticalScrollBar()->value() == immediateGestureValue,
             "an immediate inner-scroll gesture supersedes following");

  output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum() /
                                        2);
  spin();
  const int preserved = output->verticalScrollBar()->value();
  output->setOutput(longOutput + QStringLiteral("one more line\n"));
  spin();
  result &= expect(!output->followsLatest() &&
                       output->verticalScrollBar()->value() == preserved,
                   "paused command output preserves its inner scroll value");
  output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());
  spin();
  result &= expect(output->followsLatest(),
                   "inner output following resumes at its real bottom");

  command.output = QStringLiteral("\x1b]0;terminal title\x07\x1b[0m \n\t");
  result &= expect(view.reconcile(snapshot),
                   "non-presentable replacement updates the command card");
  const int hiddenOuterRange = view.verticalScrollBar()->maximum();
  const int hiddenCommandHeight = commandCard->height();
  result &= expect(output->isHidden(),
                   "non-presentable replacement removes the black surface");
  spin();
  result &=
      expect(output->isHidden() &&
                 view.verticalScrollBar()->maximum() == hiddenOuterRange &&
                 commandCard->height() == hiddenCommandHeight,
             "hidden command output causes no delayed outer reflow");
  return result;
}

bool testInitialCommandGeometrySettlement() {
  const std::string thread = "initial-command-thread";
  QString output;
  for (int word = 0; word < 32; ++word)
    output += QStringLiteral("width-sensitive-output ");
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{QStringLiteral("printf output"),
                           output,
                           QStringLiteral("completed"),
                           {},
                           0}};
  ConversationSnapshot snapshot{
      thread, {{"turn:initial-command", "turn", {command}}}, 0, false};

  ConversationView view;
  view.resize(650, 520);
  view.show();
  spin();
  bool result = expect(view.reconcile(snapshot),
                       "initial visible command output is inserted");
  ConversationCard *commandCard = card(view, stableKey(command.key));
  auto *outputView = commandCard ? dynamic_cast<CommandOutputView *>(
                                       commandCard->findChild<QTextEdit *>(
                                           QStringLiteral("commandOutputView")))
                                 : nullptr;
  result &= expect(commandCard && outputView && !outputView->isHidden() &&
                       outputView->height() < outputView->maximumHeight(),
                   "initial output is visible and below its height cap");
  if (!commandCard || !outputView)
    return false;
  const int immediateRange = view.verticalScrollBar()->maximum();
  const int immediateCardHeight = commandCard->height();
  const int immediateOutputHeight = outputView->height();
  const int immediateHint = outputView->sizeHint().height();
  spin();
  result &= expect(view.verticalScrollBar()->maximum() == immediateRange &&
                       commandCard->height() == immediateCardHeight &&
                       outputView->height() == immediateOutputHeight &&
                       outputView->sizeHint().height() == immediateHint,
                   "initial wrapped output has no delayed geometry settlement");

  const int glyphWidth = std::max(
      1, outputView->fontMetrics().horizontalAdvance(QLatin1Char('W')));
  const int charactersPerLine =
      std::max(1, outputView->viewport()->width() / glyphWidth);
  auto &execution = std::get<CommandExecutionData>(
      snapshot.sections.front().cards.front().payload);
  execution.output = QString(charactersPerLine + 1, QLatin1Char('W'));
  result &= expect(view.reconcile(snapshot),
                   "single logical output line changes to two visual lines");
  spin();
  const QTextBlock wrappedBlock = outputView->document()->firstBlock();
  result &= expect(
      wrappedBlock.layout() && wrappedBlock.layout()->lineCount() == 2 &&
          outputView->verticalScrollBar()->maximum() == 0 &&
          outputView->viewport()->height() >=
              static_cast<int>(
                  std::ceil(outputView->document()->size().height())),
      "two visual output lines are fully visible without inner scrolling");
  return result;
}

bool testBottomAnchoredCommandOutputGrowth() {
  const std::string thread = "bottom-anchored-output";
  ConversationSnapshot snapshot = conversation(thread, 14);
  VisibleCardData command{
      AuthoritativeItemKey{thread, "turn-2", "live-command"},
      CardKind::CommandExecution,
      thread,
      "turn-2",
      "live-command",
      CommandExecutionData{QStringLiteral("run live command"),
                           {},
                           QStringLiteral("inProgress"),
                           {},
                           std::nullopt}};
  snapshot.sections.back().cards.push_back(command);

  ConversationView view;
  view.resize(620, 360);
  view.show();
  view.reconcile(snapshot);
  spin();
  ConversationCard *commandCard = card(view, stableKey(command.key));
  auto *metadata =
      commandCard
          ? commandCard->findChild<QLabel *>(QStringLiteral("commandMetadata"))
          : nullptr;
  auto *output = commandCard ? dynamic_cast<CommandOutputView *>(
                                   commandCard->findChild<QTextEdit *>(
                                       QStringLiteral("commandOutputView")))
                             : nullptr;
  bool result =
      expect(commandCard && metadata && output && output->isHidden() &&
                 view.isAtBottom() && metadata->property("tone") == "active",
             "live command starts with a hidden zero-line output");
  if (!commandCard || !metadata || !output)
    return false;
  const int metadataBottomBefore =
      metadata->mapTo(view.viewport(), QPoint(0, metadata->height())).y();

  auto &live = std::get<CommandExecutionData>(
      snapshot.sections.back().cards.back().payload);
  live.output = QStringLiteral(
      "first wrapped output line with enough words to use real width\n"
      "second output line\nthird output line\n\n");
  result &= expect(view.reconcile(snapshot), "live output becomes visible");
  const int metadataBottomAfter =
      metadata->mapTo(view.viewport(), QPoint(0, metadata->height())).y();
  result &= expect(!output->isHidden() && output->height() > 2 * 20 &&
                       output->height() == output->sizeHint().height() &&
                       metadataBottomAfter == metadataBottomBefore &&
                       view.isAtBottom(),
                   "multiline output takes its needed height and grows upward");

  QString cappedOutput;
  for (int line = 0; line < 80; ++line)
    cappedOutput += QStringLiteral("scrollable line %1\n").arg(line);
  live.output = cappedOutput;
  result &= expect(view.reconcile(snapshot), "live output reaches its cap");
  result &= expect(
      output->height() == 220 && output->verticalScrollBar()->maximum() > 0 &&
          metadata->mapTo(view.viewport(), QPoint(0, metadata->height())).y() ==
              metadataBottomBefore,
      "capped output keeps its scrollbar and fixed card bottom");
  return result;
}

bool testCommandOutputStateAcrossNavigation() {
  const std::string thread = "command-navigation-thread";
  QString output;
  for (int line = 0; line < 80; ++line)
    output += QStringLiteral("retained line %1\n").arg(line);
  const VisibleCardData command{
      AuthoritativeItemKey{thread, "turn", "command"},
      CardKind::CommandExecution,
      thread,
      "turn",
      "command",
      CommandExecutionData{QStringLiteral("produce output"),
                           output,
                           QStringLiteral("completed"),
                           {},
                           0}};
  const ConversationSnapshot commandThread{
      thread, {{"turn:command-navigation", "turn", {command}}}, 0, false};

  ConversationView view;
  view.resize(650, 520);
  view.show();
  view.reconcile(commandThread);
  spin();
  ConversationCard *commandCard = card(view, stableKey(command.key));
  auto *initialOutput = commandCard
                            ? dynamic_cast<CommandOutputView *>(
                                  commandCard->findChild<QTextEdit *>(
                                      QStringLiteral("commandOutputView")))
                            : nullptr;
  bool result =
      expect(initialOutput && initialOutput->verticalScrollBar()->maximum() > 0,
             "navigation test has independently scrollable output");
  if (!initialOutput)
    return false;
  const int pausedValue = initialOutput->verticalScrollBar()->maximum() / 3;
  initialOutput->verticalScrollBar()->setValue(pausedValue);
  spin();
  result &= expect(!initialOutput->followsLatest(),
                   "command output is paused before thread navigation");

  view.reconcile(conversation("other-thread", 8));
  spin();
  view.reconcile(commandThread);
  spin();
  commandCard = card(view, stableKey(command.key));
  auto *restoredOutput = commandCard
                             ? dynamic_cast<CommandOutputView *>(
                                   commandCard->findChild<QTextEdit *>(
                                       QStringLiteral("commandOutputView")))
                             : nullptr;
  result &=
      expect(restoredOutput && !restoredOutput->followsLatest() &&
                 restoredOutput->verticalScrollBar()->value() == pausedValue,
             "thread navigation restores paused command output state");
  return result;
}

bool testPendingPromptAnimation() {
  VisibleCardData pending{
      LocalPromptKey{901},
      CardKind::LocalPrompt,
      "prompt-thread",
      {},
      {},
      LocalPromptData{
          901, QStringLiteral("pending prompt"), PromptState::InFlight, 0, {}}};
  ConversationCard card(pending);
  card.resize(560, 92);
  card.show();
  spin(40);
  const QImage first = card.grab().toImage();
  spin(110);
  const QImage second = card.grab().toImage();
  bool result =
      expect(first != second,
             "an unacknowledged prompt visibly animates its blue sweep");

  auto &accepted = std::get<LocalPromptData>(pending.payload);
  accepted.state = PromptState::Accepted;
  accepted.acceptedAtMilliseconds = QDateTime::currentMSecsSinceEpoch();
  result &= expect(card.apply(pending),
                   "the real acknowledged state updates the pending card");
  spin(560);
  const QImage settled = card.grab().toImage();
  spin(100);
  result &= expect(settled == card.grab().toImage(),
                   "the acknowledgment transition stops after 500ms");
  return result;
}

bool testMessageImagePresentation() {
  QTemporaryDir directory;
  const QString path = directory.filePath(QStringLiteral("sample.png"));
  QImage source(640, 360, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(QStringLiteral("#2f6feb")));
  bool result = expect(directory.isValid() && source.save(path),
                       "image test fixture is a real readable image");

  VisibleCardData message{
      AuthoritativeItemKey{"images", "turn", "message"},
      CardKind::UserMessage,
      "images",
      "turn",
      "message",
      UserMessageData{QStringLiteral("attached image"), {path}}};
  auto *card = new ConversationCard(message);
  card->show();
  spin();
  auto *thumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  const QPixmap thumbnailPixmap = thumbnail ? thumbnail->pixmap() : QPixmap{};
  result &=
      expect(thumbnail && thumbnail->property("imageAvailable").toBool() &&
                 !thumbnailPixmap.isNull() && thumbnailPixmap.width() <= 280 &&
                 thumbnailPixmap.height() <= 180,
             "a local image is decoded directly to a bounded thumbnail");
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent click(QEvent::MouseButtonPress, local, local,
                      thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &click);
    spin();
  }
  QWidget *viewer = nullptr;
  for (QWidget *candidate : QApplication::topLevelWidgets())
    if (candidate->objectName() == QStringLiteral("messageImageViewer"))
      viewer = candidate;
  result &= expect(viewer && viewer->isVisible(),
                   "clicking a thumbnail opens the non-modal image viewer");
  const auto *viewerImage = viewer ? viewer->findChild<QLabel *>(QStringLiteral(
                                         "messageImageViewerImage"))
                                   : nullptr;
  result &= expect(viewerImage && !viewerImage->pixmap().isNull(),
                   "the shown viewer contains a fitted image pixmap");
  if (viewer)
    viewer->close();
  spin();

  auto &payload = std::get<UserMessageData>(message.payload);
  payload.imagePaths = {directory.filePath(QStringLiteral("missing.png"))};
  result &= expect(card->apply(message),
                   "changing the image list invalidates card presentation");
  thumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  result &=
      expect(thumbnail && !thumbnail->property("imageAvailable").toBool() &&
                 thumbnail->text().contains(QStringLiteral("unavailable")),
             "an unreadable image has a stable restrained placeholder");

  payload.imagePaths = {path};
  card->apply(message);
  thumbnail =
      card->findChild<QLabel *>(QStringLiteral("messageImageThumbnail"));
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent click(QEvent::MouseButtonPress, local, local,
                      thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &click);
    spin();
  }
  viewer = nullptr;
  for (QWidget *candidate : QApplication::topLevelWidgets())
    if (candidate->objectName() == QStringLiteral("messageImageViewer"))
      viewer = candidate;
  delete card;
  spin();
  result &= expect(viewer && viewer->isVisible(),
                   "an open viewer is independent of its originating card");
  if (viewer)
    viewer->close();
  spin();
  return result;
}

bool testGeneratedImagePresentationAndGenericBound() {
  QTemporaryDir directory;
  const QString path = directory.filePath(QStringLiteral("generated.png"));
  QImage source(800, 450, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(QStringLiteral("#e9f7f0")));
  bool result = expect(directory.isValid() && source.save(path),
                       "generated-image fixture is readable");

  VisibleCardData generated{
      AuthoritativeItemKey{"generated", "turn", "image"},
      CardKind::ImageGeneration,
      "generated",
      "turn",
      "image",
      ImageGenerationData{path, QStringLiteral("completed"),
                          QStringLiteral("A generated UI proposal")}};
  ConversationCard generatedCard(generated);
  generatedCard.show();
  spin();
  auto *thumbnail = generatedCard.findChild<QLabel *>(
      QStringLiteral("messageImageThumbnail"));
  result &= expect(thumbnail && thumbnail->property("imageAvailable").toBool(),
                   "generated-image card reuses the bounded thumbnail");
  if (thumbnail) {
    const QPointF local(thumbnail->rect().center());
    QMouseEvent click(QEvent::MouseButtonPress, local, local,
                      thumbnail->mapToGlobal(local.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &click);
    spin();
  }
  QWidget *viewer = nullptr;
  for (QWidget *candidate : QApplication::topLevelWidgets())
    if (candidate->objectName() == QStringLiteral("messageImageViewer"))
      viewer = candidate;
  result &= expect(viewer && viewer->isVisible(),
                   "generated-image thumbnail opens the shared image viewer");
  if (viewer)
    viewer->close();
  spin();

  VisibleCardData viewed{
      AuthoritativeItemKey{"generated", "turn", "view"},
      CardKind::ImageGeneration,
      "generated",
      "turn",
      "view",
      ImageGenerationData{path, {}, {}}};
  ConversationCard viewedCard(viewed);
  viewedCard.show();
  spin();
  const auto viewedLabels = viewedCard.findChildren<QLabel *>();
  result &= expect(
      std::ranges::any_of(viewedLabels, [](QLabel *label) {
        return label->property("kind").toString() == QStringLiteral("title") &&
               label->text() == QStringLiteral("Image");
      }) &&
          std::ranges::any_of(viewedLabels, [](QLabel *label) {
            return label->objectName() ==
                       QStringLiteral("messageImageThumbnail") &&
                   label->property("imageAvailable").toBool();
          }),
      "plain image-view cards use a neutral title and the shared thumbnail");

  VisibleCardData generic{
      AuthoritativeItemKey{"generated", "turn", "unknown"},
      CardKind::GenericActivity,
      "generated",
      "turn",
      "unknown",
      GenericActivityData{QStringLiteral("Unknown activity"),
                          {{"large", std::string(100000, 'x')}}}};
  ConversationCard genericCard(generic);
  genericCard.show();
  spin();
  auto *details = genericCard.findChild<QLabel *>(
      QStringLiteral("genericActivityMetadata"));
  result &= expect(details && details->text().size() < 4200 &&
                       details->text().endsWith(
                           QStringLiteral("[Activity details truncated]")),
                   "unknown activity text is bounded before Qt lays it out");
  return result;
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;
  bool result = testFollowPauseAndStableAnchor();
  result &= testThreadLocalScrollAndComposerExtent();
  result &= testPromptAdmissionFollowOwnership();
  result &= testMutableCardsAndCommandOutput();
  result &= testInitialCommandGeometrySettlement();
  result &= testBottomAnchoredCommandOutputGrowth();
  result &= testCommandOutputStateAcrossNavigation();
  result &= testPendingPromptAnimation();
  result &= testMessageImagePresentation();
  result &= testGeneratedImagePresentationAndGenericBound();
  if (result)
    std::cout << "Conversation card tests passed\n";
  return result ? 0 : 1;
}
