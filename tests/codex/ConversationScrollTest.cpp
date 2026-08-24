// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include <utils/Config.h>

#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/ShellWidget.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QEventLoop>
#include <QLayoutItem>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
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

    insertCard(shell, 14, 84);
    spinEvents(50);
    bool result = expect(scrollBar->value() == pausedValue,
                         "paused bottom append preserves scrollbar value");
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
