// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include <utils/Config.h>

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/PresentationProtocol.h"
#include "codex/ShellWidget.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLayoutItem>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <iostream>

namespace codexui::codex {

class ShellWidgetScrollTest {
public:
  static bool run(FrontendSession &session) {
    ShellWidget shell(session);
    shell.resize(1280, 820);
    shell.show();
    spinEvents(60);

    populate(shell, 14, 84);
    QScrollBar *scrollBar = shell.conversationScroll->verticalScrollBar();
    scrollBar->setValue(std::min(330, scrollBar->maximum() - 40));
    shell.conversationFollowsLatest = false;
    spinEvents(20);

    const ShellWidget::ConversationScrollAnchor pausedAnchor =
        shell.captureConversationScrollAnchor();
    const int pausedOffset = anchorOffset(shell, pausedAnchor.key);
    const int pausedValue = scrollBar->value();

    const bool appendKeptViewportHeight = observeViewportHeight(
        shell, [&shell] { insertCard(shell, 14, 84); }, 50);
    bool result = expect(scrollBar->value() == pausedValue,
                         "paused bottom append preserves scrollbar value");
    result &= expect(appendKeptViewportHeight,
                     "card insertion never changes viewport height");
    result &= expect(anchorOffset(shell, pausedAnchor.key) == pausedOffset,
                     "paused bottom append preserves the visible card");
    result &= expect(!shell.conversationFollowsLatest,
                     "paused bottom append does not enable following");

    const ShellWidget::ConversationScrollAnchor reflowAnchor =
        shell.captureConversationScrollAnchor();
    const int reflowOffset = anchorOffset(shell, reflowAnchor.key);
    QWidget *first = card(shell, QStringLiteral("card:0"));
    first->setFixedHeight(first->height() + 73);
    shell.conversationScrollRebuilding = true;
    shell.settleConversationScroll(false, reflowAnchor, false);
    spinEvents(60);
    result &= expect(anchorOffset(shell, reflowAnchor.key) == reflowOffset,
                     "paused reflow above preserves the visible card offset");
    result &= expect(!shell.conversationFollowsLatest,
                     "paused reflow above remains paused");

    const ShellWidget::ConversationScrollAnchor rebuildAnchor =
        shell.captureConversationScrollAnchor();
    const int rebuildOffset = anchorOffset(shell, rebuildAnchor.key);
    shell.stopConversationScrollAnimation();
    shell.conversationScrollRebuilding = true;
    populate(shell, 17, 84, 73);
    shell.settleConversationScroll(false, rebuildAnchor, false);
    spinEvents(70);
    result &= expect(anchorOffset(shell, rebuildAnchor.key) == rebuildOffset,
                     "paused reconstruction preserves the visible card offset");
    result &= expect(!shell.conversationFollowsLatest,
                     "paused reconstruction remains paused");

    const ShellWidget::ConversationScrollAnchor lateReflowAnchor =
        shell.captureConversationScrollAnchor();
    const int lateReflowOffset = anchorOffset(shell, lateReflowAnchor.key);
    first = card(shell, QStringLiteral("card:0"));
    first->setFixedHeight(first->height() + 41);
    shell.conversationLayout->invalidate();
    shell.conversationContent->updateGeometry();
    spinEvents(70);
    result &=
        expect(anchorOffset(shell, lateReflowAnchor.key) == lateReflowOffset,
               "paused late range change restores the retained visual anchor");
    result &= expect(!shell.conversationFollowsLatest,
                     "paused late range change remains paused");

    shell.conversationPausedAnchor = shell.captureConversationScrollAnchor();
    shell.conversationPausedAnchorValid = true;
    for (int index = 16; index >= 7; --index) {
      QWidget *removed = card(shell, QStringLiteral("card:%1").arg(index));
      shell.conversationLayout->removeWidget(removed);
      delete removed;
    }
    shell.conversationLayout->invalidate();
    shell.conversationContent->updateGeometry();
    spinEvents(80);
    result &= expect(!shell.conversationFollowsLatest,
                     "a layout range clamp cannot re-enable following");

    populate(shell, 17, 84, 73);

    scrollBar->setValue(scrollBar->maximum());
    shell.conversationFollowsLatest = true;
    const int formerMaximum = scrollBar->maximum();
    const ShellWidget::ConversationScrollAnchor followAnchor =
        shell.captureConversationScrollAnchor();
    shell.conversationScrollRebuilding = true;
    insertCard(shell, 17, 180);
    shell.settleConversationScroll(true, followAnchor, true);
    spinEvents(70);
    const int animatedValue = scrollBar->value();
    result &= expect(animatedValue > formerMaximum &&
                         animatedValue < scrollBar->maximum(),
                     "bottom following advances through an intermediate value");
    spinEvents(300);
    result &= expect(scrollBar->value() == scrollBar->maximum(),
                     "smooth following reaches the latest content");

    const int secondFormerMaximum = scrollBar->maximum();
    insertCard(shell, 18, 220);
    spinEvents(45);
    result &= expect(scrollBar->value() > secondFormerMaximum,
                     "a later append starts another smooth follow");
    scrollBar->triggerAction(QAbstractSlider::SliderSingleStepSub);
    spinEvents(20);
    const int interruptedValue = scrollBar->value();
    result &= expect(!shell.conversationFollowsLatest,
                     "user scroll immediately pauses smooth following");
    spinEvents(300);
    result &= expect(scrollBar->value() == interruptedValue,
                     "interrupted following does not resume or jump");

    ItemPresentation emptyOutput;
    emptyOutput.raw = {{"type", "commandExecution"},
                       {"command", "true"},
                       {"status", "inProgress"},
                       {"cwd", "/workspace"},
                       {"aggregatedOutput", ""}};
    ItemPresentation whitespaceOutput = emptyOutput;
    whitespaceOutput.raw["aggregatedOutput"] = " \n\t";
    whitespaceOutput.raw["nonVisualProtocolMetadata"] = 7;
    result &= expect(shell.conversationItemFingerprint(emptyOutput) ==
                         shell.conversationItemFingerprint(whitespaceOutput),
                     "nonvisual command updates do not invalidate a card");
    whitespaceOutput.raw["aggregatedOutput"] = "visible\n";
    result &= expect(shell.conversationItemFingerprint(emptyOutput) !=
                         shell.conversationItemFingerprint(whitespaceOutput),
                     "visible command output invalidates its card");

    const nlohmann::json commandItems =
        nlohmann::json::array({{{"id", "empty"},
                                {"type", "commandExecution"},
                                {"command", "true"},
                                {"status", "completed"},
                                {"aggregatedOutput", ""}},
                               {{"id", "whitespace"},
                                {"type", "commandExecution"},
                                {"command", "printf whitespace"},
                                {"status", "completed"},
                                {"aggregatedOutput", " \n\t"}},
                               {{"id", "control"},
                                {"type", "commandExecution"},
                                {"command", "printf control"},
                                {"status", "completed"},
                                {"aggregatedOutput", "\x1b[0m\x1b]0;\x07"}},
                               {{"id", "visible"},
                                {"type", "commandExecution"},
                                {"command", "printf visible"},
                                {"status", "completed"},
                                {"aggregatedOutput", "visible\n"}}});
    const nlohmann::json hydratedThread = {
        {"id", "output-visibility"},
        {"status", {{"type", "idle"}}},
        {"turns", nlohmann::json::array({{{"id", "output-turn"},
                                          {"status", "completed"},
                                          {"items", commandItems}}})}};
    shell.model.applyEvent(presentation::result(
        1, 1, "thread.read", "read-output-visibility", true,
        {{"thread", hydratedThread}}, presentation::Authority::Merge,
        {{"threadId", "output-visibility"}}));
    shell.selectedThreadId = "output-visibility";
    shell.refreshConversation();
    spinEvents(80);
    const auto outputSurfaceCount = [&shell](const std::string &itemId) {
      const std::string key = std::string("output-turn\x1f") + itemId;
      const auto card = shell.conversationCards.find(key);
      if (card == shell.conversationCards.end())
        return 0;
      const QList<QPlainTextEdit *> views =
          card->second->findChildren<QPlainTextEdit *>();
      return static_cast<int>(
          std::count_if(views.begin(), views.end(), [](QPlainTextEdit *view) {
            return view->property("kind").toString() == QStringLiteral("code");
          }));
    };
    result &= expect(outputSurfaceCount("empty") == 0 &&
                         outputSurfaceCount("whitespace") == 0 &&
                         outputSurfaceCount("control") == 0,
                     "non-presentable command output creates no black box");
    result &= expect(outputSurfaceCount("visible") == 1,
                     "presentable command output creates one black box");

    const std::string emptyCommandKey =
        std::string("output-turn") + '\x1f' + "empty";
    const std::string insertedCommandKey =
        std::string("output-turn") + '\x1f' + "inserted";
    QWidget *retainedEmptyCard = shell.conversationCards.at(emptyCommandKey);
    const nlohmann::json insertedCommand = {{"id", "inserted"},
                                            {"type", "commandExecution"},
                                            {"command", "printf inserted"},
                                            {"status", "inProgress"},
                                            {"aggregatedOutput", "inserted\n"}};
    shell.model.applyEvent(presentation::event(
        2, 1, "conversation.item.upsert", {{"item", insertedCommand}},
        presentation::Authority::Merge,
        {{"threadId", "output-visibility"},
         {"turnId", "output-turn"},
         {"itemId", "inserted"}}));
    shell.dirtyConversationItems[insertedCommandKey] = {"output-turn",
                                                        "inserted"};
    const bool incrementalInsertKeptViewportHeight = observeViewportHeight(
        shell, [&shell] { shell.refreshConversationItems(); }, 80);
    result &= expect(
        shell.conversationCards.contains(insertedCommandKey) &&
            shell.conversationCards.at(emptyCommandKey) == retainedEmptyCard,
        "new command card inserts without reconstructing retained cards");
    result &= expect(incrementalInsertKeptViewportHeight,
                     "new command insertion keeps viewport geometry fixed");

    nlohmann::json updatedCommand = insertedCommand;
    updatedCommand["aggregatedOutput"] =
        "inserted\nwith a second visible line\n";
    shell.model.applyEvent(presentation::event(
        3, 1, "conversation.item.upsert", {{"item", updatedCommand}},
        presentation::Authority::Merge,
        {{"threadId", "output-visibility"},
         {"turnId", "output-turn"},
         {"itemId", "inserted"}}));
    shell.dirtyConversationItems[insertedCommandKey] = {"output-turn",
                                                        "inserted"};
    QWidget *insertedCard = shell.conversationCards.at(insertedCommandKey);
    const bool commandUpdateKeptViewportHeight = observeViewportHeight(
        shell, [&shell] { shell.refreshConversationItems(); }, 80);
    result &= expect(commandUpdateKeptViewportHeight &&
                         shell.conversationCards.at(insertedCommandKey) ==
                             insertedCard,
                     "command update mutates in place with fixed viewport");

    nlohmann::json completedCommand = updatedCommand;
    completedCommand["status"] = "completed";
    completedCommand["exitCode"] = 0;
    shell.model.applyEvent(presentation::event(
        4, 1, "conversation.item.upsert", {{"item", completedCommand}},
        presentation::Authority::Merge,
        {{"threadId", "output-visibility"},
         {"turnId", "output-turn"},
         {"itemId", "inserted"}}));
    shell.dirtyConversationItems[insertedCommandKey] = {"output-turn",
                                                        "inserted"};
    const bool completionKeptViewportHeight = observeViewportHeight(
        shell, [&shell] { shell.refreshConversationItems(); }, 80);
    result &= expect(completionKeptViewportHeight &&
                         shell.conversationCards.at(insertedCommandKey) ==
                             insertedCard,
                     "command completion mutates the retained card in place");

    nlohmann::json nonvisualCommand = completedCommand;
    nonvisualCommand["nonVisualProtocolMetadata"] = "ignored";
    shell.model.applyEvent(presentation::event(
        5, 1, "conversation.item.upsert", {{"item", nonvisualCommand}},
        presentation::Authority::Merge,
        {{"threadId", "output-visibility"},
         {"turnId", "output-turn"},
         {"itemId", "inserted"}}));
    shell.dirtyConversationItems[insertedCommandKey] = {"output-turn",
                                                        "inserted"};
    const int valueBeforeNonvisualUpdate =
        shell.conversationScroll->verticalScrollBar()->value();
    shell.refreshConversationItems();
    spinEvents(80);
    result &= expect(
        shell.conversationCards.at(insertedCommandKey) == insertedCard &&
            shell.conversationScroll->verticalScrollBar()->value() ==
                valueBeforeNonvisualUpdate,
        "nonvisual command completion data does not touch card or scroll");

    shell.localNewThreadIntent = true;
    shell.selectedThreadId.clear();
    ShellWidget::PendingPrompt acknowledged;
    acknowledged.id = 91;
    acknowledged.prompt = QStringLiteral("Fast acknowledgment");
    acknowledged.status = ShellWidget::PendingPromptStatus::Acknowledged;
    acknowledged.acknowledgedAtMilliseconds =
        QDateTime::currentMSecsSinceEpoch();
    shell.newThreadPendingPrompts.push_back(std::move(acknowledged));
    shell.refreshConversation();
    spinEvents(60);
    QFrame *pendingCard =
        shell.findChild<QFrame *>(QStringLiteral("pendingPromptCard"));
    bool acceptedLabelFound = false;
    if (pendingCard) {
      for (QLabel *label : pendingCard->findChildren<QLabel *>())
        acceptedLabelFound |=
            label->text().contains(QStringLiteral("Accepted by app-server"));
    }
    const QImage firstFrame =
        pendingCard ? pendingCard->grab().toImage() : QImage{};
    spinEvents(120);
    const QImage secondFrame =
        pendingCard ? pendingCard->grab().toImage() : QImage{};
    result &= expect(pendingCard && acceptedLabelFound,
                     "fast acknowledgment retains a visible transition card");
    result &= expect(!firstFrame.isNull() && firstFrame != secondFrame,
                     "acknowledgment transition visibly animates");

    return result;
  }

private:
  static void spinEvents(int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 2) {
      QApplication::processEvents(QEventLoop::AllEvents, 2);
      QThread::msleep(2);
    }
    QApplication::processEvents(QEventLoop::AllEvents);
  }

  static bool observeViewportHeight(ShellWidget &shell,
                                    const std::function<void()> &operation,
                                    int milliseconds) {
    const int expectedHeight = shell.conversationScroll->viewport()->height();
    bool stable = true;
    operation();
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 2) {
      QApplication::processEvents(QEventLoop::AllEvents, 2);
      stable &=
          shell.conversationScroll->viewport()->height() == expectedHeight;
      QThread::msleep(2);
    }
    QApplication::processEvents(QEventLoop::AllEvents);
    return stable &&
           shell.conversationScroll->viewport()->height() == expectedHeight;
  }

  static bool expect(bool condition, const char *message) {
    std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
    return condition;
  }

  static QWidget *newCard(int index, int height) {
    auto *result = new QWidget;
    result->setFixedHeight(height);
    result->setProperty("conversationAnchorKey",
                        QStringLiteral("card:%1").arg(index));
    return result;
  }

  static void populate(ShellWidget &shell, int count, int height,
                       int firstExtraHeight = 0) {
    while (QLayoutItem *item = shell.conversationLayout->takeAt(0)) {
      delete item->widget();
      delete item;
    }
    shell.conversationCards.clear();
    shell.conversationTrailingSpace = nullptr;
    for (int index = 0; index < count; ++index)
      shell.conversationLayout->addWidget(
          newCard(index, height + (index == 0 ? firstExtraHeight : 0)));
    shell.addConversationTrailingSpace();
    shell.conversationLayout->addStretch();
    shell.conversationLayout->invalidate();
    shell.conversationContent->updateGeometry();
    spinEvents(50);
  }

  static void insertCard(ShellWidget &shell, int index, int height) {
    shell.conversationLayout->insertWidget(
        std::max(0, shell.conversationLayout->count() - 2),
        newCard(index, height));
    shell.conversationLayout->invalidate();
    shell.conversationContent->updateGeometry();
  }

  static QWidget *card(ShellWidget &shell, const QString &key) {
    for (int index = 0; index < shell.conversationLayout->count(); ++index) {
      QWidget *candidate = shell.conversationLayout->itemAt(index)->widget();
      if (candidate &&
          candidate->property("conversationAnchorKey").toString() == key)
        return candidate;
    }
    return nullptr;
  }

  static int anchorOffset(ShellWidget &shell, const QString &key) {
    QWidget *anchored = card(shell, key);
    return anchored
               ? anchored
                     ->mapTo(shell.conversationScroll->viewport(), QPoint(0, 0))
                     .y()
               : -100000;
  }
};

} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  auto *configuration =
      utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
  codexui::codex::FrontendSession session(*configuration);
  return codexui::codex::ShellWidgetScrollTest::run(session) ? 0 : 1;
}
