// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include <sys/resource.h>

namespace codexui::codex::middle {
namespace {

VisibleCardData cardData(std::size_t index) {
  const std::string suffix = std::to_string(index);
  VisibleCardData card;
  card.key = AuthoritativeItemKey{"benchmark-thread", "turn-" + suffix,
                                  "item-" + suffix};
  card.threadId = "benchmark-thread";
  card.turnId = "turn-" + suffix;
  card.itemId = "item-" + suffix;
  switch (index % 8) {
  case 0:
    card.kind = CardKind::UserMessage;
    card.payload = UserMessageData{"Prompt " + suffix, {}};
    break;
  case 1:
    card.kind = CardKind::AgentMessage;
    card.payload = AgentMessageData{
        "A compact final answer for benchmark row " + suffix + '.', true};
    break;
  case 2:
    card.kind = CardKind::CommandExecution;
    card.payload = CommandExecutionData{
        "printf benchmark", "line one\nline two", "completed", "/tmp", 0, 4};
    break;
  case 3:
    card.kind = CardKind::Reasoning;
    card.payload = ReasoningData{"Reasoning summary " + suffix};
    break;
  case 4:
    card.kind = CardKind::AgentActivity;
    card.payload = AgentActivityData{
        "worker", "completed", "completed", {}, "Agent result " + suffix};
    break;
  case 5:
    card.kind = CardKind::FileChanges;
    card.payload =
        FileChangesData{"completed",
                        {{"src/example-" + suffix + ".cpp", "update", 2, 1}},
                        "/tmp"};
    break;
  case 6:
    card.kind = CardKind::Plan;
    card.payload = PlanData{"Plan " + suffix,
                            {{"Inspect", "completed"}, {"Change", "pending"}},
                            {}};
    break;
  default:
    card.kind = CardKind::GenericActivity;
    card.payload = GenericActivityData{
        "toolCall", {}, "completed", "detail: benchmark " + suffix};
    break;
  }
  return card;
}

ConversationSnapshot snapshot(std::size_t count) {
  ConversationSnapshot result;
  result.threadId = "benchmark-thread";
  result.sections.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    VisibleCardData card = cardData(index);
    TurnSection section;
    section.key = "turn-section-" + std::to_string(index);
    section.turnId = card.turnId;
    section.cards.push_back(std::move(card));
    result.sections.push_back(std::move(section));
  }
  return result;
}

void settle(ConversationView &view) {
  QElapsedTimer deadline;
  deadline.start();
  while (view.structuralStagingActive() && deadline.elapsed() < 120000)
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  QApplication::processEvents(QEventLoop::AllEvents, 50);
}

} // namespace
} // namespace codexui::codex::middle

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  using namespace codexui::codex::middle;

  const std::size_t count =
      argc > 1 ? std::max<std::size_t>(1, std::strtoull(argv[1], nullptr, 10))
               : 80;
  ConversationView view;
  view.resize(900, 700);
  view.show();
  QApplication::processEvents();

  ConversationSnapshot data = snapshot(count);
  QElapsedTimer initial;
  initial.start();
  view.reconcileStaged(std::move(data));
  settle(view);
  const qint64 initialMilliseconds = initial.elapsed();

  QElapsedTimer scroll;
  scroll.start();
  constexpr int ScrollSamples = 240;
  const int maximum = view.verticalScrollBar()->maximum();
  for (int sample = 0; sample < ScrollSamples; ++sample) {
    view.verticalScrollBar()->setValue(maximum * sample /
                                       std::max(1, ScrollSamples - 1));
    QApplication::processEvents(QEventLoop::AllEvents, 2);
  }
  const qint64 scrollMicroseconds = scroll.nsecsElapsed() / 1000;

  const auto cards = view.findChildren<ConversationCard *>();
  const auto widgets = view.findChildren<QWidget *>();
  std::size_t sections = 0;
  for (QWidget *widget : widgets)
    if (widget->property("turnSectionKey").isValid())
      ++sections;
  rusage usage{};
  static_cast<void>(getrusage(RUSAGE_SELF, &usage));

  QJsonObject result{
      {"rows", static_cast<qint64>(count)},
      {"initialMilliseconds", initialMilliseconds},
      {"scrollSweepMicroseconds", scrollMicroseconds},
      {"scrollSamples", ScrollSamples},
      {"conversationCards", cards.size()},
      {"turnSections", static_cast<qint64>(sections)},
      {"descendantWidgets", widgets.size()},
      {"peakResidentKiB", static_cast<qint64>(usage.ru_maxrss)},
      {"scrollMaximum", maximum},
      {"geometryPasses",
       static_cast<qint64>(
           view.property("conversationGeometryPasses").toULongLong())}};
  std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
            << '\n';
  return view.structuralStagingActive() ? 2 : 0;
}
