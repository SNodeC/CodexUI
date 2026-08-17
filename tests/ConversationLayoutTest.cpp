// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ConversationWidget.h"

#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QLayout>
#include <QTimer>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

bool expectAtLeast(int actual, int required, const char* message)
{
    if (actual >= required)
        return true;
    std::cerr << message << " (actual " << actual << ", required " << required << ")\n";
    return false;
}

void settleEvents()
{
    for (int pass = 0; pass < 3; ++pass) {
        QEventLoop loop;
        QTimer::singleShot(25, &loop, &QEventLoop::quit);
        loop.exec();
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

int requiredHeight(QWidget& host)
{
    QLayout* layout = host.layout();
    if (!layout)
        return 0;
    const int width = host.contentsRect().width();
    return width > 0 && layout->hasHeightForWidth() ? layout->heightForWidth(width) : layout->sizeHint().height();
}

QLabel* emptyStateDetail(codexui::ConversationWidget& conversation)
{
    for (QLabel* label : conversation.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Choose a synchronized thread from the sidebar."))
            return label;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    codexui::ConversationWidget conversation;

    QWidget* timeline = conversation.findChild<QWidget*>(QStringLiteral("conversationTimeline"));
    QLabel* detail = emptyStateDetail(conversation);
    bool passed = true;
    passed &= expect(timeline != nullptr, "the conversation timeline must be discoverable");
    passed &= expect(detail != nullptr, "the wrapped empty-state detail must be discoverable");
    if (!timeline || !detail)
        return 1;

    QString longDetail;
    for (int index = 0; index < 32; ++index) {
        if (!longDetail.isEmpty())
            longDetail += QLatin1Char(' ');
        longDetail += QStringLiteral("wrapped timeline content must retain its natural height");
    }
    detail->setText(longDetail);
    detail->updateGeometry();

    conversation.resize(900, 700);
    conversation.show();
    settleEvents();

    passed &= expect(detail->sizePolicy().hasHeightForWidth(),
                     "a wrapping label must advertise height-for-width to its parent layouts");
    passed &= expect(timeline->layout()->hasHeightForWidth(),
                     "height-for-width must propagate through the timeline layout");
    passed &= expectAtLeast(timeline->height(), requiredHeight(*timeline),
                            "the wide timeline must not be shorter than its wrapped content");
    passed &= expectAtLeast(detail->height(), detail->heightForWidth(detail->width()),
                            "the wide wrapped label must receive its required height");
    const int wideHeight = timeline->height();

    conversation.resize(520, 700);
    settleEvents();
    passed &= expectAtLeast(timeline->height(), requiredHeight(*timeline),
                            "the narrow timeline must not be shorter than its wrapped content");
    passed &= expectAtLeast(detail->height(), detail->heightForWidth(detail->width()),
                            "the narrow wrapped label must receive its required height");
    const int narrowHeight = timeline->height();
    passed &= expect(narrowHeight > wideHeight,
                     "the timeline must grow when wrapped content receives less width");

    conversation.resize(900, 700);
    settleEvents();
    passed &= expectAtLeast(timeline->height(), requiredHeight(*timeline),
                            "the widened timeline must still contain its wrapped content");
    passed &= expect(timeline->height() < narrowHeight,
                     "the timeline must shrink again after wrapped content receives more width");

    return passed ? 0 : 1;
}
