// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ConversationWidget.h"

#include <ai/openai/codex/frontend/Messages.h>
#include <ai/openai/codex/frontend/client/Client.h>
#include <ai/openai/codex/frontend/client/State.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace frontend = ai::openai::codex::frontend;
namespace client = frontend::client;

struct MessageFixture
{
    std::string id;
    frontend::ThreadItemKind kind = frontend::ThreadItemKind::AgentMessage;
    std::string text;
    std::string status = "completed";
    bool contentTruncated = false;
    bool textTruncated = false;
};

struct TurnFixture
{
    std::string id;
    std::vector<MessageFixture> messages;
};

struct ThreadFixture
{
    std::string id;
    std::vector<TurnFixture> turns;
};

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

void settleEvents(int passes = 3, int delayMs = 25)
{
    for (int pass = 0; pass < passes; ++pass) {
        QEventLoop loop;
        QTimer::singleShot(delayMs, &loop, &QEventLoop::quit);
        loop.exec();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

void settleTimeline()
{
    settleEvents(4, 75);
}

frontend::Json messageJson(const std::string& threadId,
                           const std::string& turnId,
                           const MessageFixture& fixture)
{
    frontend::Json data = frontend::Json::object();
    if (fixture.kind == frontend::ThreadItemKind::UserMessage) {
        data = frontend::Json{{"clientId", nullptr},
                              {"contentTruncated", fixture.contentTruncated},
                              {"text", fixture.text},
                              {"textTruncated", fixture.textTruncated},
                              {"originalContentBytes",
                               fixture.text.size() + (fixture.contentTruncated ? 1U : 0U)},
                              {"retainedContentBytes", fixture.text.size()},
                              {"originalContentItems", fixture.contentTruncated ? 2 : 1},
                              {"retainedContentItems", 1}};
    }
    return frontend::Json{{"id", fixture.id},
                          {"type", frontend::toString(fixture.kind)},
                          {"threadId", threadId},
                          {"turnId", turnId},
                          {"status", fixture.status},
                          {"summary", fixture.text},
                          {"agentText", fixture.kind == frontend::ThreadItemKind::AgentMessage ? fixture.text : ""},
                          {"reasoningText", ""},
                          {"reasoningSummary", ""},
                          {"commandOutput", ""},
                          {"droppedContentBytes", 0},
                          {"contentTruncated", fixture.contentTruncated},
                          {"data", std::move(data)},
                          {"extensions", frontend::Json::object()}};
}

client::State makeState(const std::vector<ThreadFixture>& fixtures)
{
    client::ClientOptions options;
    options.requestedCapabilities.clear();
    options.credentialProvider = [] {
        return client::AuthenticationContext{frontend::NoCredential{}, std::string{"conversation-layout-test"}};
    };
    client::Client sdk(std::move(options));
    auto connection = sdk.openConnection({
        [](client::OutboundMessage) {
            return client::SendResult{client::SendStatus::Accepted, std::nullopt};
        },
        [](std::string) {},
    });
    connection.transportConnected();
    if (!connection
             .receive(frontend::ServerMessage{frontend::Welcome{
                 "fixture-session",
                 frontend::SessionRole::Observer,
                 frontend::SequenceNumber{0},
                 frontend::SyncMode::Snapshot}})
             .accepted)
        return {};

    frontend::Json threads = frontend::Json::array();
    for (const ThreadFixture& threadFixture : fixtures) {
        frontend::Json turns = frontend::Json::array();
        for (const TurnFixture& turnFixture : threadFixture.turns) {
            frontend::Json items = frontend::Json::array();
            for (const MessageFixture& message : turnFixture.messages)
                items.push_back(messageJson(threadFixture.id, turnFixture.id, message));
            turns.push_back(frontend::Json{{"id", turnFixture.id},
                                           {"threadId", threadFixture.id},
                                           {"status", "completed"},
                                           {"active", false},
                                           {"terminal", true},
                                           {"items", std::move(items)},
                                           {"extensions", frontend::Json::object()}});
        }
        threads.push_back(frontend::Json{{"id", threadFixture.id},
                                         {"title", threadFixture.id},
                                         {"status", "idle"},
                                         {"fullyLoaded", true},
                                         {"turns", std::move(turns)},
                                         {"extensions", frontend::Json::object()}});
    }

    frontend::Json state{{"backendRevision", 1},
                         {"lifecycle", "ready"},
                         {"diagnostics", {{"received", 0}, {"recent", frontend::Json::array()}}},
                         {"sessions", frontend::Json::array()},
                         {"threadList", {{"hasLoadedPage", true}, {"complete", true}, {"pagesLoaded", 1}}},
                         {"threads", std::move(threads)},
                         {"pendingRequests", frontend::Json::array()},
                         {"codexExtensions", frontend::Json::array()},
                         {"omittedCodexExtensions", 0},
                         {"journal", {{"oldestReplayableAfter", 0}, {"currentSequence", 0}}},
                         {"sequenceExhausted", false}};
    if (!connection
             .receive(frontend::ServerMessage{
                 frontend::Snapshot{frontend::SequenceNumber{0}, std::move(state)}})
             .accepted)
        return {};
    if (!connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber{0}}}).accepted)
        return {};
    return sdk.state();
}

ThreadFixture sequentialTurns(std::string threadId, int turnCount)
{
    ThreadFixture result{std::move(threadId), {}};
    for (int index = 0; index < turnCount; ++index) {
        const std::string suffix = std::to_string(index);
        result.turns.push_back({"turn-" + result.id + "-" + suffix,
                                {{"item-" + result.id + "-" + suffix,
                                  frontend::ThreadItemKind::AgentMessage,
                                  "message " + result.id + " " + suffix}}});
    }
    return result;
}

ThreadFixture singleTurn(std::string threadId, int messageCount)
{
    ThreadFixture result{std::move(threadId), {}};
    result.turns.push_back({"turn-" + result.id, {}});
    for (int index = 0; index < messageCount; ++index) {
        const std::string suffix = std::to_string(index);
        result.turns.front().messages.push_back(
            {"item-" + result.id + "-" + suffix,
             index % 2 == 0 ? frontend::ThreadItemKind::UserMessage : frontend::ThreadItemKind::AgentMessage,
             "message " + result.id + " " + suffix});
    }
    return result;
}

ThreadFixture activityTurn(std::string threadId, int itemCount)
{
    ThreadFixture result{std::move(threadId), {}};
    result.turns.push_back({"turn-" + result.id, {}});
    for (int index = 0; index < itemCount; ++index) {
        const std::string suffix = std::to_string(index);
        result.turns.front().messages.push_back(
            {"item-" + result.id + "-" + suffix,
             frontend::ThreadItemKind::Reasoning,
             "activity " + result.id + " " + suffix});
    }
    return result;
}

QWidget* timeline(codexui::ConversationWidget& conversation)
{
    return conversation.findChild<QWidget*>(QStringLiteral("conversationTimeline"));
}

QFrame* windowNotice(codexui::ConversationWidget& conversation)
{
    return conversation.findChild<QFrame*>(QStringLiteral("conversationWindowNotice"));
}

QWidget* segment(codexui::ConversationWidget& conversation, const QString& id)
{
    for (QWidget* candidate : conversation.findChildren<QWidget*>(QStringLiteral("conversationSegment"))) {
        if (candidate->property("segmentId").toString() == id)
            return candidate;
    }
    return nullptr;
}

QLabel* messageLabel(QWidget* messageSegment, const QString& objectName)
{
    return messageSegment ? messageSegment->findChild<QLabel*>(objectName) : nullptr;
}

bool segmentHasLabel(QWidget* messageSegment, const QString& text)
{
    if (!messageSegment)
        return false;
    for (QLabel* label : messageSegment->findChildren<QLabel*>()) {
        if (label->text() == text)
            return true;
    }
    return false;
}

bool hasLabel(codexui::ConversationWidget& conversation, const QString& text)
{
    for (QLabel* label : conversation.findChildren<QLabel*>()) {
        if (label->text() == text)
            return true;
    }
    return false;
}

QStringList renderedTurnIds(codexui::ConversationWidget& conversation)
{
    QStringList result;
    QWidget* host = timeline(conversation);
    if (!host || !host->layout())
        return result;
    for (int index = 0; index < host->layout()->count(); ++index) {
        QWidget* turn = host->layout()->itemAt(index)->widget();
        if (turn && turn->objectName() == QStringLiteral("conversationTurn"))
            result.append(turn->property("turnId").toString());
    }
    return result;
}

QWidget* renderedTurn(codexui::ConversationWidget& conversation, const QString& id)
{
    for (QWidget* turn : conversation.findChildren<QWidget*>(QStringLiteral("conversationTurn"))) {
        if (turn->property("turnId").toString() == id)
            return turn;
    }
    return nullptr;
}

QString turnHeading(QWidget* turn)
{
    if (!turn)
        return {};
    for (QLabel* label : turn->findChildren<QLabel*>()) {
        if (label->text().startsWith(QStringLiteral("TURN ")))
            return label->text();
    }
    return {};
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

bool testTurnWindow()
{
    const client::State state = makeState({sequentialTurns("many-turns", 40)});
    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(state, QStringLiteral("many-turns"));
    settleTimeline();

    QWidget* host = timeline(conversation);
    QFrame* notice = windowNotice(conversation);
    const qsizetype maximumTurns = host ? host->property("maximumRenderedTurns").toLongLong() : 0;
    const QStringList turns = renderedTurnIds(conversation);
    bool passed = true;
    passed &= expect(state.thread("many-turns") != nullptr, "the long-turn fixture must produce public AISuite State");
    passed &= expect(host && maximumTurns == 32, "the conversation must publish its bounded live-turn budget");
    passed &= expect(turns.size() == maximumTurns, "the live timeline must remain within the turn budget");
    passed &= expect(!turns.isEmpty() && turns.front() == QStringLiteral("turn-many-turns-8")
                         && turns.back() == QStringLiteral("turn-many-turns-39"),
                     "the turn window must preserve the exact canonical tail order and original identities");
    passed &= expect(notice && notice->isVisible(),
                     "a bounded timeline must truthfully disclose omitted earlier entries");
    passed &= expect(hasLabel(conversation, QStringLiteral("message many-turns 39")),
                     "the newest retained turn must remain visible");
    passed &= expect(!hasLabel(conversation, QStringLiteral("message many-turns 0")),
                     "an entry outside the live turn window must not allocate a widget");
    return passed;
}

bool testSameThreadPrefixExpansion()
{
    const ThreadFixture completeFixture = sequentialTurns("prefix", 40);
    ThreadFixture partialFixture = completeFixture;
    partialFixture.turns.erase(partialFixture.turns.begin(), partialFixture.turns.begin() + 8);

    const client::State partialState = makeState({partialFixture});
    const client::State completeState = makeState({completeFixture});
    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(partialState, QStringLiteral("prefix"));
    settleTimeline();

    QPointer<QWidget> firstTailTurn = renderedTurn(conversation, QStringLiteral("turn-prefix-8"));
    QWidget* preservedAddress = firstTailTurn.data();
    const QString initialHeading = turnHeading(firstTailTurn);
    conversation.render(completeState, QStringLiteral("prefix"));
    settleTimeline();

    return expect(firstTailTurn && firstTailTurn.data() == preservedAddress
                      && firstTailTurn.data()
                             == renderedTurn(conversation, QStringLiteral("turn-prefix-8"))
                      && initialHeading.startsWith(QStringLiteral("TURN 1 ·"))
                      && turnHeading(firstTailTurn).startsWith(QStringLiteral("TURN 9 ·")),
                  "same-thread history expansion must preserve tail widgets and refresh canonical turn ordinals");
}

bool testHotTurnWindow()
{
    const client::State state = makeState({singleTurn("hot", 300)});
    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(state, QStringLiteral("hot"));
    settleTimeline();

    QWidget* host = timeline(conversation);
    const qsizetype maximumItems = host ? host->property("maximumRenderedItems").toLongLong() : 0;
    const auto segments = conversation.findChildren<QWidget*>(QStringLiteral("conversationSegment"));
    qsizetype renderedItems = 0;
    for (QWidget* item : segments)
        renderedItems += item->property("timelineItemCount").toLongLong();
    bool passed = true;
    passed &= expect(state.thread("hot") != nullptr, "the hot-turn fixture must produce public AISuite State");
    passed &= expect(maximumItems == 256 && renderedItems == maximumItems,
                     "one oversized turn must remain within the global live-item budget");
    passed &= expect(segment(conversation, QStringLiteral("message:item-hot-0")) == nullptr,
                     "the oversized turn must not materialize its earliest out-of-window message");
    passed &= expect(segment(conversation, QStringLiteral("message:item-hot-299")) != nullptr
                         && hasLabel(conversation, QStringLiteral("message hot 299")),
                     "the oversized turn must retain its exact newest message");
    passed &= expect(windowNotice(conversation) && windowNotice(conversation)->isVisible(),
                     "the oversized turn must show the presentation-window notice");

    const client::State activityState = makeState({activityTurn("activity", 300)});
    codexui::ConversationWidget activityConversation;
    activityConversation.resize(900, 700);
    activityConversation.show();
    activityConversation.render(activityState, QStringLiteral("activity"));
    settleTimeline();
    QWidget* activityHost = timeline(activityConversation);
    qsizetype renderedActivities = 0;
    bool boundedCards = true;
    for (QWidget* card : activityConversation.findChildren<QWidget*>(QStringLiteral("conversationSegment"))) {
        const qsizetype cardItems = card->property("timelineItemCount").toLongLong();
        renderedActivities += cardItems;
        boundedCards = boundedCards && cardItems <= 16;
    }
    passed &= expect(activityState.thread("activity") != nullptr && boundedCards && activityHost
                         && renderedActivities == activityHost->property("maximumRenderedItems").toLongLong()
                         && renderedActivities == activityHost->property("renderedTimelineItems").toLongLong()
                         && activityHost->property("retainedTimelineItems").toLongLong() == 300,
                     "a contiguous activity run must be chunked and remain within the same global item budget");
    passed &= expect(hasLabel(activityConversation, QStringLiteral("activity activity 299")),
                     "the bounded activity window must retain its newest exact detail");
    return passed;
}

bool testPointerPreservingAppend()
{
    ThreadFixture beforeFixture = singleTurn("append", 256);
    const client::State before = makeState({beforeFixture});
    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(before, QStringLiteral("append"));
    settleTimeline();

    QPointer<QWidget> evicted = segment(conversation, QStringLiteral("message:item-append-0"));
    QPointer<QWidget> survivor = segment(conversation, QStringLiteral("message:item-append-2"));
    QPointer<QWidget> readingAnchor = segment(conversation, QStringLiteral("message:item-append-10"));
    QWidget* survivorAddress = survivor.data();
    QScrollArea* scroll = conversation.findChild<QScrollArea*>();
    if (scroll && readingAnchor)
    {
        const int currentY = scroll->viewport()->mapFromGlobal(readingAnchor->mapToGlobal(QPoint{})).y();
        auto* bar = scroll->verticalScrollBar();
        bar->setValue(qBound(0, bar->value() + currentY - 120, bar->maximum()));
        settleEvents();
    }
    const int anchorYBefore = scroll && readingAnchor
                                  ? scroll->viewport()->mapFromGlobal(readingAnchor->mapToGlobal(QPoint{})).y()
                                  : 0;
    const bool readingHistory = scroll
                                && scroll->verticalScrollBar()->maximum()
                                       - scroll->verticalScrollBar()->value() > 72;

    beforeFixture.turns.front().messages.push_back(
        {"item-append-256", frontend::ThreadItemKind::UserMessage, "reflected prompt"});
    beforeFixture.turns.front().messages.push_back(
        {"item-append-257", frontend::ThreadItemKind::AgentMessage, "final answer"});
    const client::State after = makeState({beforeFixture});
    conversation.render(after, QStringLiteral("append"));
    settleTimeline();

    QWidget* host = timeline(conversation);
    const int anchorYAfter = scroll && readingAnchor
                                 ? scroll->viewport()->mapFromGlobal(readingAnchor->mapToGlobal(QPoint{})).y()
                                 : 0;
    bool passed = true;
    passed &= expect(!evicted && survivor && survivor.data() == survivorAddress
                         && survivor.data() == segment(conversation, QStringLiteral("message:item-append-2")),
                     "rolling the bounded head must preserve every overlapping segment widget");
    passed &= expect(segment(conversation, QStringLiteral("message:item-append-256")) != nullptr
                         && segment(conversation, QStringLiteral("message:item-append-257")) != nullptr
                         && hasLabel(conversation, QStringLiteral("reflected prompt"))
                         && hasLabel(conversation, QStringLiteral("final answer")),
                     "the reflected prompt and final answer must append at the timeline tail");
    passed &= expect(host && host->property("renderedTimelineItems").toLongLong()
                                 <= host->property("maximumRenderedItems").toLongLong(),
                     "appending at the rolling boundary must keep the live-item count bounded");
    passed &= expect(readingHistory && readingAnchor && qAbs(anchorYAfter - anchorYBefore) <= 2,
                     "rolling the bounded head must preserve a reader's surviving viewport segment");

    codexui::ConversationWidget followingConversation;
    followingConversation.resize(900, 700);
    followingConversation.show();
    followingConversation.render(before, QStringLiteral("append"));
    settleTimeline();
    followingConversation.render(after, QStringLiteral("append"));
    settleTimeline();
    QScrollArea* followingScroll = followingConversation.findChild<QScrollArea*>();
    passed &= expect(followingScroll
                         && followingScroll->verticalScrollBar()->value()
                                == followingScroll->verticalScrollBar()->maximum(),
                     "a followed append must settle smoothly at the newest timeline content");
    return passed;
}

bool testInPlaceMessageReplacement()
{
    ThreadFixture agentFixture{"in-place-agent",
                               {{"turn-in-place-agent",
                                 {{"item-in-place-agent",
                                   frontend::ThreadItemKind::AgentMessage,
                                   "streamed prefix",
                                   "in_progress"}}}}};
    codexui::ConversationWidget agentConversation;
    agentConversation.resize(900, 700);
    agentConversation.show();
    agentConversation.render(makeState({agentFixture}), QStringLiteral("in-place-agent"));
    settleTimeline();

    QPointer<QWidget> agentSegment =
        segment(agentConversation, QStringLiteral("message:item-in-place-agent"));
    QPointer<QLabel> agentContent =
        messageLabel(agentSegment, QStringLiteral("conversationMessageContent"));
    QPointer<QLabel> agentStatus =
        messageLabel(agentSegment, QStringLiteral("conversationMessageStatus"));
    QPointer<QLabel> agentTruncation =
        messageLabel(agentSegment, QStringLiteral("conversationMessageTruncation"));
    QWidget* const agentSegmentAddress = agentSegment.data();
    QLabel* const agentContentAddress = agentContent.data();
    QLabel* const agentStatusAddress = agentStatus.data();
    QLabel* const agentTruncationAddress = agentTruncation.data();

    agentFixture.turns.front().messages.front().text =
        "streamed prefix and canonical continuation";
    agentFixture.turns.front().messages.front().status = "completed";
    agentFixture.turns.front().messages.front().contentTruncated = true;
    agentConversation.render(makeState({agentFixture}), QStringLiteral("in-place-agent"));
    settleTimeline();

    bool passed = true;
    passed &= expect(agentSegment && agentSegment.data() == agentSegmentAddress
                         && agentSegment.data()
                                == segment(agentConversation,
                                           QStringLiteral("message:item-in-place-agent"))
                         && agentContent && agentContent.data() == agentContentAddress
                         && agentStatus && agentStatus.data() == agentStatusAddress
                         && agentTruncation && agentTruncation.data() == agentTruncationAddress,
                     "canonical agent-message replacement must preserve the segment and message labels");
    passed &= expect(agentContent
                         && agentContent->text()
                                == QStringLiteral("streamed prefix and canonical continuation")
                         && agentStatus && agentStatus->text() == QStringLiteral("Completed")
                         && agentTruncation && agentTruncation->isVisible()
                         && agentTruncation->text().contains(QStringLiteral("truncated"),
                                                             Qt::CaseInsensitive)
                         && segmentHasLabel(agentSegment, QStringLiteral("CODEX")),
                     "the preserved agent-message widget must reflect canonical content, status and truncation");

    agentFixture.turns.front().messages.front().text = "short canonical replacement";
    agentFixture.turns.front().messages.front().status = "failed";
    agentFixture.turns.front().messages.front().contentTruncated = false;
    agentConversation.render(makeState({agentFixture}), QStringLiteral("in-place-agent"));
    settleTimeline();
    passed &= expect(agentSegment && agentSegment.data() == agentSegmentAddress
                         && agentContent && agentContent.data() == agentContentAddress
                         && agentContent->text() == QStringLiteral("short canonical replacement")
                         && agentStatus && agentStatus->text() == QStringLiteral("Failed")
                         && agentTruncation && !agentTruncation->isVisible(),
                     "a shorter canonical replacement must update the same agent-message widget and hide its marker");

    ThreadFixture userFixture{"in-place-user",
                              {{"turn-in-place-user",
                                {{"item-in-place-user",
                                  frontend::ThreadItemKind::UserMessage,
                                  "draft prompt"}}}}};
    codexui::ConversationWidget userConversation;
    userConversation.resize(900, 700);
    userConversation.show();
    userConversation.render(makeState({userFixture}), QStringLiteral("in-place-user"));
    settleTimeline();

    QPointer<QWidget> userSegment =
        segment(userConversation, QStringLiteral("message:item-in-place-user"));
    QPointer<QLabel> userContent =
        messageLabel(userSegment, QStringLiteral("conversationMessageContent"));
    QPointer<QLabel> userTruncation =
        messageLabel(userSegment, QStringLiteral("conversationMessageTruncation"));
    QWidget* const userSegmentAddress = userSegment.data();
    QLabel* const userContentAddress = userContent.data();
    QLabel* const userTruncationAddress = userTruncation.data();

    userFixture.turns.front().messages.front().text = "final prompt\n\nwith multipart text";
    userFixture.turns.front().messages.front().contentTruncated = true;
    userFixture.turns.front().messages.front().textTruncated = true;
    userConversation.render(makeState({userFixture}), QStringLiteral("in-place-user"));
    settleTimeline();
    passed &= expect(userSegment && userSegment.data() == userSegmentAddress
                         && userContent && userContent.data() == userContentAddress
                         && userTruncation && userTruncation.data() == userTruncationAddress,
                     "canonical user-message replacement must preserve the segment and message labels");
    passed &= expect(userContent
                         && userContent->text()
                                == QStringLiteral("final prompt\n\nwith multipart text"),
                     "the preserved user-message label must show exact canonical multipart text");
    passed &= expect(userTruncation && userTruncation->isVisible(),
                     "the preserved user-message truncation marker must reflect canonical semantics");
    passed &= expect(segmentHasLabel(userSegment, QStringLiteral("YOU")),
                     "the preserved user-message segment must retain its intended visual role");
    return passed;
}

bool testExactContentInvalidation()
{
    ThreadFixture fixture{"exact-content",
                          {{"turn-exact-content",
                            {{"item-exact-first",
                              frontend::ThreadItemKind::AgentMessage,
                              "first prefix",
                              "in_progress"},
                             {"item-exact-second",
                              frontend::ThreadItemKind::AgentMessage,
                              "second stable",
                              "completed"}}}}};
    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(makeState({fixture}), QStringLiteral("exact-content"));
    settleTimeline();

    QPointer<QWidget> first = segment(conversation, QStringLiteral("message:item-exact-first"));
    QPointer<QWidget> second = segment(conversation, QStringLiteral("message:item-exact-second"));
    QPointer<QLabel> firstContent =
        messageLabel(first, QStringLiteral("conversationMessageContent"));
    QPointer<QLabel> secondContent =
        messageLabel(second, QStringLiteral("conversationMessageContent"));
    QWidget* const firstAddress = first.data();
    QWidget* const secondAddress = second.data();

    fixture.turns.front().messages.front().text = "first canonical continuation";
    const QHash<QString, QStringList> exactChanges{
        {QStringLiteral("turn-exact-content"),
         QStringList{QStringLiteral("item-exact-first")}}};
    conversation.render(makeState({fixture}),
                        QStringLiteral("exact-content"),
                        false,
                        &exactChanges);
    settleTimeline();

    bool passed = true;
    passed &= expect(first && first.data() == firstAddress && firstContent
                         && firstContent->text() == QStringLiteral("first canonical continuation"),
                     "an exact content update must mutate its canonical message in place");
    passed &= expect(second && second.data() == secondAddress && secondContent
                         && secondContent->text() == QStringLiteral("second stable"),
                     "an exact content update must preserve unaffected segment widgets");

    fixture.turns.front().messages.back().text = "second structural fallback";
    conversation.render(makeState({fixture}), QStringLiteral("exact-content"));
    settleTimeline();
    passed &= expect(second && second.data() == secondAddress && secondContent
                         && secondContent->text() == QStringLiteral("second structural fallback"),
                     "a full reconciliation fallback must still refresh every changed canonical segment");
    return passed;
}

bool testSegmentReplacementShrink()
{
    ThreadFixture fixture = singleTurn("replacement", 1);
    std::string longText;
    for (int index = 0; index < 120; ++index)
        longText += "A retained line that gives the replacement a measurable wrapped height.\n";
    fixture.turns.front().messages.front().text = std::move(longText);
    const client::State tallState = makeState({fixture});

    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(tallState, QStringLiteral("replacement"));
    settleTimeline();
    QWidget* host = timeline(conversation);
    const int tallHeight = host ? host->height() : 0;

    fixture.turns.front().messages.front().text = "short replacement";
    const client::State shortState = makeState({fixture});
    conversation.render(shortState, QStringLiteral("replacement"));
    settleTimeline();

    return expect(host && host->height() < tallHeight,
                  "replacing a segment with shorter canonical content must release the previous fixed height");
}

bool testThreadSwitchWindow()
{
    const client::State state = makeState({singleTurn("switch-a", 300), singleTurn("switch-b", 2)});
    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    bool passed = true;

    const auto verify = [&](const QString& selected, const QString& present, const QString& absent)
    {
        conversation.render(state, selected);
        settleTimeline();
        QWidget* host = timeline(conversation);
        QScrollArea* scroll = conversation.findChild<QScrollArea*>();
        passed &= expect(segment(conversation, present) != nullptr && segment(conversation, absent) == nullptr,
                         "thread switching must retain only the selected canonical window");
        passed &= expect(host && host->property("renderedTimelineItems").toLongLong()
                                     <= host->property("maximumRenderedItems").toLongLong(),
                         "every selected thread must remain within the live-item budget");
        passed &= expect(scroll && scroll->verticalScrollBar()->value() == scroll->verticalScrollBar()->maximum(),
                         "a selected historical thread must settle at its newest retained entry");
        return host ? host->height() : 0;
    };

    const int firstLongHeight = verify(QStringLiteral("switch-a"), QStringLiteral("message:item-switch-a-299"),
                                       QStringLiteral("message:item-switch-b-1"));
    const int shortHeight = verify(QStringLiteral("switch-b"), QStringLiteral("message:item-switch-b-1"),
           QStringLiteral("message:item-switch-a-299"));
    const int secondLongHeight = verify(QStringLiteral("switch-a"), QStringLiteral("message:item-switch-a-299"),
                                        QStringLiteral("message:item-switch-b-1"));
    passed &= expect(shortHeight > 0 && shortHeight < firstLongHeight && secondLongHeight > shortHeight,
                     "thread switching must release a previous long timeline height before laying out a short thread");
    return passed;
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

    conversation.hide();
    passed &= testTurnWindow();
    passed &= testSameThreadPrefixExpansion();
    passed &= testHotTurnWindow();
    passed &= testPointerPreservingAppend();
    passed &= testInPlaceMessageReplacement();
    passed &= testExactContentInvalidation();
    passed &= testSegmentReplacementShrink();
    passed &= testThreadSwitchWindow();

    return passed ? 0 : 1;
}
