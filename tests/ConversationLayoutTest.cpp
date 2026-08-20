// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/ConversationWidget.h"
#include "ui/InspectorWidget.h"

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
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QTabBar>
#include <QToolButton>

#include <algorithm>
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
    bool genericItemTruncatedOnly = false;
    std::string command;
};

struct TurnFixture
{
    std::string id;
    std::vector<MessageFixture> messages;
    std::optional<frontend::Json> plan;
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
    constexpr std::size_t initialCommandOutputBytes = 12U * 1024U;
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
    } else if (fixture.kind == frontend::ThreadItemKind::CommandExecution) {
        data = frontend::Json{{"command", fixture.command.empty() ? "bash -lc test-command" : fixture.command},
                              {"cwd", "/workspace/test"},
                              {"status", fixture.status},
                              {"durationMs", 42}};
        if (fixture.status == "completed")
            data["exitCode"] = 0;
    }
    const bool carriesCommandOutput = fixture.kind == frontend::ThreadItemKind::CommandExecution
                                      || fixture.kind == frontend::ThreadItemKind::FileChange;
    const std::string summary = fixture.kind == frontend::ThreadItemKind::UserMessage
                                    ? std::string{}
                                : carriesCommandOutput
                                    ? fixture.text.substr(0, std::min<std::size_t>(fixture.text.size(), 500))
                                    : fixture.text;
    const std::string initialCommandOutput = carriesCommandOutput
                                                 ? fixture.text.substr(
                                                       0,
                                                       std::min(fixture.text.size(), initialCommandOutputBytes))
                                                 : std::string{};
    return frontend::Json{{"id", fixture.id},
                          {"type", frontend::toString(fixture.kind)},
                          {"threadId", threadId},
                          {"turnId", turnId},
                          {"status", fixture.status},
                          {"summary", summary},
                          {"agentText", fixture.kind == frontend::ThreadItemKind::AgentMessage ? fixture.text : ""},
                          {"reasoningText", ""},
                          {"reasoningSummary", ""},
                          {"commandOutput", initialCommandOutput},
                          {"droppedContentBytes", 0},
                          {"contentTruncated",
                           fixture.contentTruncated || fixture.genericItemTruncatedOnly},
                          {"data", std::move(data)},
                          {"extensions", frontend::Json::object()}};
}

client::State makeState(const std::vector<ThreadFixture>& fixtures)
{
    client::ClientOptions options;
    options.requestedCapabilities = {frontend::FrontendCapability::CompleteThreadItems};
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
    const frontend::CapabilityAdvertisement capabilities{
        {frontend::FrontendCapability::CompleteThreadItems},
        {frontend::FrontendCapability::CompleteThreadItems},
        {frontend::FrontendCapability::CompleteThreadItems},
        frontend::Json::object()};
    if (!connection
             .receive(frontend::ServerMessage{frontend::Welcome{
                 "fixture-session",
                 frontend::SessionRole::Observer,
                 frontend::SequenceNumber{0},
                 frontend::SyncMode::Snapshot,
                 frontend::Json{{"projection",
                                 frontend::Json{{"itemContentUpdateMode", "append-v2"}}}},
                 capabilities}})
             .accepted)
        return {};

    const frontend::Json executionConfiguration{
        {"approvalPolicy", "on-request"},
        {"approvalsReviewer", "user"},
        {"collaborationMode",
         {{"mode", "plan"},
          {"settings", {{"model", "gpt-test"}, {"reasoningEffort", "high"}}}}},
        {"cwd", "/workspace/test"},
        {"effort", "high"},
        {"model", "gpt-test"},
        {"modelProvider", "openai"},
        {"personality", "pragmatic"},
        {"sandboxPolicy", {{"type", "workspaceWrite"}, {"networkAccess", false}}},
        {"serviceTier", "flex"},
        {"summary", "detailed"},
    };
    frontend::Json threads = frontend::Json::array();
    for (const ThreadFixture& threadFixture : fixtures) {
        frontend::Json turns = frontend::Json::array();
        for (const TurnFixture& turnFixture : threadFixture.turns) {
            frontend::Json items = frontend::Json::array();
            for (const MessageFixture& message : turnFixture.messages)
                items.push_back(messageJson(threadFixture.id, turnFixture.id, message));
            frontend::Json turn{{"id", turnFixture.id},
                                {"threadId", threadFixture.id},
                                {"status", "completed"},
                                {"active", false},
                                {"terminal", true},
                                {"effectiveExecutionConfiguration", executionConfiguration},
                                {"effectiveExecutionConfigurationProvenance", "turn_start_accepted"},
                                {"items", std::move(items)},
                                {"extensions", frontend::Json::object()}};
            if (turnFixture.plan)
                turn["plan"] = *turnFixture.plan;
            turns.push_back(std::move(turn));
        }
        threads.push_back(frontend::Json{{"id", threadFixture.id},
                                         {"title", threadFixture.id},
                                         {"status", "idle"},
                                         {"fullyLoaded", true},
                                         {"executionConfiguration", executionConfiguration},
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

    // Exercise the public negotiated append-v2 path instead of putting an
    // over-capacity scalar into a synthetic Snapshot. This mirrors how the
    // real backend restores complete retained command output incrementally.
    constexpr std::size_t initialCommandOutputBytes = 12U * 1024U;
    constexpr std::size_t commandOutputDeltaBytes = 12U * 1024U;
    std::uint64_t sequence = 0;
    for (const ThreadFixture& thread : fixtures) {
        for (const TurnFixture& turn : thread.turns) {
            for (const MessageFixture& item : turn.messages) {
                if (item.kind != frontend::ThreadItemKind::CommandExecution
                    && item.kind != frontend::ThreadItemKind::FileChange)
                    continue;
                std::size_t retained = std::min(item.text.size(), initialCommandOutputBytes);
                while (retained < item.text.size()) {
                    const std::size_t deltaBytes =
                        std::min(commandOutputDeltaBytes, item.text.size() - retained);
                    frontend::FrontendEvent event{
                        frontend::SequenceNumber{++sequence},
                        "item.content.updated",
                        frontend::Json{{"threadId", thread.id},
                                       {"turnId", turn.id},
                                       {"itemId", item.id},
                                       {"channel", "commandOutput"},
                                       {"content", ""},
                                       {"contentDelta", item.text.substr(retained, deltaBytes)},
                                       {"baseContentBytes", retained},
                                       {"contentTruncated", false},
                                       {"droppedContentBytes", 0}},
                        frontend::Json::object()};
                    if (!connection
                             .receive(frontend::ServerMessage{frontend::EventBatch{
                                 event.sequence, event.sequence, {std::move(event)}}})
                             .accepted)
                        return {};
                    retained += deltaBytes;
                }
            }
        }
    }
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
                      && initialHeading == QStringLiteral("TURN 1")
                      && turnHeading(firstTailTurn) == QStringLiteral("TURN 9"),
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
    passed &= expect(windowNotice(conversation)
                         && windowNotice(conversation)->styleSheet().contains(
                             QStringLiteral("QFrame#conversationWindowNotice")),
                     "the presentation-window border must be scoped and never leak to its text");

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
    const auto activityCards = activityConversation.findChildren<QFrame*>(
        QStringLiteral("conversationActivityCard"));
    passed &= expect(!activityCards.isEmpty()
                         && std::ranges::all_of(activityCards, [](const QFrame* card) {
                                return card->styleSheet().contains(
                                           QStringLiteral("QFrame#conversationActivityCard"))
                                    && !card->styleSheet().contains(QStringLiteral("QFrame{"));
                            }),
                     "activity-card borders must be scoped to the card and never leak to child labels");
    return passed;
}

bool testActivityDisclosureAndFullOutput()
{
    std::string output = "initial command output beginning\n"
                         + std::string(70 * 1024, 'i')
                         + "\ninitial command output final sentinel";
    ThreadFixture fixture{"activity-detail",
                          {{"turn-activity-detail",
                            {{"command-activity-detail",
                              frontend::ThreadItemKind::CommandExecution,
                              output,
                              "in_progress"}},
                            frontend::Json{{"explanation", "Use the canonical typed plan."},
                                           {"steps", {"Inspect", "Verify"}},
                                           {"statuses", {"completed", "inProgress"}},
                                           {"totalSteps", 2},
                                           {"truncated", false}}}}};
    const std::string fullCommand = "bash -lc '" + std::string(400, 'x') + " --final-command-sentinel'";
    fixture.turns.front().messages.front().command = fullCommand;

    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(makeState({fixture}), QStringLiteral("activity-detail"));
    settleTimeline();

    auto* card = conversation.findChild<QFrame*>(QStringLiteral("conversationActivityCard"));
    auto* body = card ? card->findChild<QWidget*>(QStringLiteral("conversationActivityBody")) : nullptr;
    auto* row = card ? card->findChild<QWidget*>(QStringLiteral("conversationActivityRow")) : nullptr;
    auto* details = row ? row->findChild<QWidget*>(QStringLiteral("conversationActivityDetails")) : nullptr;
    auto* detailDisclosure = row ? row->findChild<QToolButton*>(QStringLiteral("activityDisclosure")) : nullptr;
    auto* planAvailable = card
                              ? card->findChild<QLabel*>(QStringLiteral("conversationActivityPlanAvailable"))
                              : nullptr;
    QPointer<QPlainTextEdit> outputView;
    QToolButton* groupDisclosure = nullptr;
    if (card && body) {
        for (auto* candidate : card->findChildren<QToolButton*>(QStringLiteral("activityDisclosure"))) {
            if (!body->isAncestorOf(candidate)) {
                groupDisclosure = candidate;
                break;
            }
        }
    }

    bool passed = true;
    passed &= expect(card && body && !body->isHidden() && row && details && details->isHidden(),
                     "an activity group must start expanded while each activity starts collapsed");
    passed &= expect(detailDisclosure && detailDisclosure->isCheckable()
                         && !row->findChild<QPlainTextEdit*>(QStringLiteral("conversationActivityOutput")),
                     "a collapsed activity must not materialize a potentially large output document");
    passed &= expect(detailDisclosure
                         && detailDisclosure->accessibleName().contains(QStringLiteral("bash -lc")),
                     "each activity disclosure must identify its activity in its accessible name");
    passed &= expect(planAvailable && planAvailable->isVisible(),
                     "an activity card must advertise an authoritative typed turn plan on initial render");

    QWidget* const rowAddress = row;
    fixture.turns.front().plan.reset();
    conversation.render(makeState({fixture}), QStringLiteral("activity-detail"));
    settleTimeline();
    passed &= expect(row == rowAddress && planAvailable && !planAvailable->isVisible(),
                     "removing a typed turn plan must update the existing activity-card indicator");
    fixture.turns.front().plan = frontend::Json{
        {"explanation", "The canonical typed plan returned."},
        {"steps", {"Inspect", "Verify"}},
        {"statuses", {"completed", "inProgress"}},
        {"totalSteps", 2},
        {"truncated", false}};
    conversation.render(makeState({fixture}), QStringLiteral("activity-detail"));
    settleTimeline();
    passed &= expect(row == rowAddress && planAvailable && planAvailable->isVisible(),
                     "adding a typed turn plan must update the existing activity-card indicator");

    output = "canonical collapsed-update beginning\n"
             + std::string(72 * 1024, 'c')
             + "\ncanonical collapsed-update final sentinel";
    fixture.turns.front().messages.front().text = output;
    const QHash<QString, QStringList> exactOutputChange{
        {QStringLiteral("turn-activity-detail"),
         QStringList{QStringLiteral("command-activity-detail")}}};
    conversation.render(
        makeState({fixture}), QStringLiteral("activity-detail"), false, &exactOutputChange);
    settleTimeline();
    passed &= expect(row == rowAddress && details && details->isHidden()
                         && !row->findChild<QPlainTextEdit*>(QStringLiteral("conversationActivityOutput")),
                     "a canonical output update must keep a collapsed activity lazy until expansion");
    const int collapsedActivityHeight = timeline(conversation)
                                            ? timeline(conversation)->height()
                                            : 0;
    if (detailDisclosure)
        detailDisclosure->click();
    settleTimeline();
    outputView = row ? row->findChild<QPlainTextEdit*>(QStringLiteral("conversationActivityOutput")) : nullptr;
    auto* detailView = row ? row->findChild<QLabel*>(QStringLiteral("conversationActivityDetail")) : nullptr;
    auto* outputHeading = row
                              ? row->findChild<QLabel*>(QStringLiteral("conversationActivityOutputHeading"))
                              : nullptr;
    passed &= expect(details && !details->isHidden() && outputView && outputView->isVisible(),
                     "an individual activity disclosure must materialize and reveal its complete output");
    passed &= expect(detailDisclosure && detailDisclosure->isChecked() && outputHeading
                         && outputHeading->text() == QStringLiteral("Output"),
                     "expanded activity details must expose accessible checked state and label their output");
    const QString renderedOutput = outputView ? outputView->toPlainText() : QString{};
    passed &= expect(
        output.size() > 64 * 1024
            && renderedOutput == QString::fromStdString(output)
            && renderedOutput.startsWith(QStringLiteral("canonical collapsed-update beginning"))
            && renderedOutput.endsWith(QStringLiteral("canonical collapsed-update final sentinel")),
        "expanding after a collapsed update must reveal the complete >64 KiB canonical output, including its beginning and end");
    passed &= expect(detailView && detailView->text().contains(QString::fromStdString(fullCommand)),
                     "expanded command details must preserve commands longer than the collapsed summary");
    passed &= expect(timeline(conversation)
                         && timeline(conversation)->height() > collapsedActivityHeight,
                     "expanding an activity must grow the fixed timeline host and its scroll range");

    QPlainTextEdit* const outputAddress = outputView.data();
    output += "\nstreamed continuation";
    fixture.turns.front().messages.front().text = output;
    fixture.turns.front().messages.front().status = "completed";
    conversation.render(
        makeState({fixture}), QStringLiteral("activity-detail"), false, &exactOutputChange);
    settleTimeline();
    auto* statusSymbol = row ? row->findChild<QLabel*>(QStringLiteral("conversationActivitySymbol")) : nullptr;
    passed &= expect(row && row == rowAddress && outputView && outputView.data() == outputAddress
                         && outputView->toPlainText() == QString::fromStdString(output)
                         && details && !details->isHidden() && statusSymbol
                         && statusSymbol->text() == QStringLiteral("✓"),
                     "streaming command output and completion status must update the expanded activity in place");

    const int longActivityHeight = timeline(conversation) ? timeline(conversation)->height() : 0;
    fixture.turns.front().messages.front().command = "true";
    conversation.render(makeState({fixture}), QStringLiteral("activity-detail"));
    settleTimeline();
    passed &= expect(timeline(conversation)
                         && timeline(conversation)->height() < longActivityHeight,
                     "shortening expanded activity detail must shrink the fixed timeline host without blank space");

    fixture.turns.front().plan.reset();
    fixture.turns.front().messages.push_back({"command-activity-appended",
                                              frontend::ThreadItemKind::Plan,
                                              "verify the focused implementation",
                                              "completed"});
    conversation.render(makeState({fixture}), QStringLiteral("activity-detail"));
    settleTimeline();
    const auto appendedRows = card
                                  ? card->findChildren<QWidget*>(QStringLiteral("conversationActivityRow"))
                                  : QList<QWidget*>{};
    passed &= expect(card == conversation.findChild<QFrame*>(QStringLiteral("conversationActivityCard"))
                         && appendedRows.size() == 2 && appendedRows.front() == rowAddress
                         && outputView && outputView.data() == outputAddress && !details->isHidden()
                         && planAvailable && planAvailable->isVisible(),
                     "appending a plan activity must preserve expanded output and update the card indicator");

    const std::string fileOutput = "file-change output beginning\n"
                                   + std::string(4'096, 'f')
                                   + "\nfile-change output sentinel";
    fixture.turns.front().messages.push_back({"file-change-output",
                                              frontend::ThreadItemKind::FileChange,
                                              fileOutput,
                                              "in_progress"});
    conversation.render(makeState({fixture}), QStringLiteral("activity-detail"));
    settleTimeline();
    QWidget* fileRow = nullptr;
    if (card) {
        for (auto* candidate : card->findChildren<QWidget*>(QStringLiteral("conversationActivityRow"))) {
            if (candidate->property("itemId").toString() == QStringLiteral("file-change-output")) {
                fileRow = candidate;
                break;
            }
        }
    }
    auto* fileDisclosure = fileRow
                               ? fileRow->findChild<QToolButton*>(QStringLiteral("activityDisclosure"))
                               : nullptr;
    if (fileDisclosure)
        fileDisclosure->click();
    settleEvents();
    auto* fileOutputView = fileRow
                               ? fileRow->findChild<QPlainTextEdit*>(QStringLiteral("conversationActivityOutput"))
                               : nullptr;
    passed &= expect(fileOutputView
                         && fileOutputView->toPlainText() == QString::fromStdString(fileOutput),
                     "any typed activity carrying canonical command output must disclose its complete text");

    const int expandedGroupHeight = timeline(conversation) ? timeline(conversation)->height() : 0;
    if (groupDisclosure)
        groupDisclosure->click();
    settleTimeline();
    passed &= expect(groupDisclosure && groupDisclosure->isCheckable()
                         && !groupDisclosure->isChecked() && body && body->isHidden(),
                     "the activity group disclosure must collapse the complete activity region");
    passed &= expect(timeline(conversation)
                         && timeline(conversation)->height() < expandedGroupHeight,
                     "collapsing an activity group must shrink the fixed timeline host and its scroll range");
    fixture.turns.front().messages.back().status = "completed";
    fixture.turns.front().messages.back().text += "\npost-collapse update";
    conversation.render(makeState({fixture}), QStringLiteral("activity-detail"));
    settleTimeline();
    passed &= expect(card == conversation.findChild<QFrame*>(QStringLiteral("conversationActivityCard"))
                         && body->isHidden() && groupDisclosure && !groupDisclosure->isChecked(),
                     "a canonical activity update must preserve a collapsed group without rebuilding it");
    if (groupDisclosure)
        groupDisclosure->click();
    settleTimeline();
    passed &= expect(body && !body->isHidden() && details && !details->isHidden(),
                     "reopening a group must preserve individual activity expansion state");
    passed &= expect(timeline(conversation)
                         && timeline(conversation)->height() > collapsedActivityHeight,
                     "reopening an activity group must restore the fixed timeline host geometry");
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

bool testCompleteAndLargeUserMessagePresentation()
{
    ThreadFixture completeFixture{
        "complete-user-message",
        {{"turn-complete-user-message",
          {{"item-complete-user-message",
            frontend::ThreadItemKind::UserMessage,
            "complete canonical prompt",
            "completed",
            true,
            false,
            true}}}}};
    codexui::ConversationWidget completeConversation;
    completeConversation.resize(900, 700);
    completeConversation.show();
    completeConversation.render(makeState({completeFixture}),
                                QStringLiteral("complete-user-message"));
    settleTimeline();

    QWidget* completeSegment =
        segment(completeConversation, QStringLiteral("message:item-complete-user-message"));
    QLabel* completeContent =
        messageLabel(completeSegment, QStringLiteral("conversationMessageContent"));
    QLabel* completeMarker =
        messageLabel(completeSegment, QStringLiteral("conversationMessageTruncation"));
    bool passed = expect(completeContent
                             && completeContent->text()
                                    == QStringLiteral("complete canonical prompt"),
                         "a valid typed user-message view must render its complete canonical text");
    passed &= expect(completeMarker && !completeMarker->isVisible(),
                     "non-text omissions and generic item bounds must not mark complete typed user text as truncated");

    std::string largeText(70U * 1024U, 'a');
    largeText.replace(16U, 2U, "\n\n");
    ThreadFixture largeFixture{
        "large-user-message",
        {{"turn-large-user-message",
          {{"item-large-user-message",
            frontend::ThreadItemKind::UserMessage,
            largeText}}}}};
    codexui::ConversationWidget largeConversation;
    largeConversation.resize(900, 700);
    largeConversation.show();
    largeConversation.render(makeState({largeFixture}), QStringLiteral("large-user-message"));
    settleTimeline();

    QPointer<QWidget> largeSegment =
        segment(largeConversation, QStringLiteral("message:item-large-user-message"));
    QPointer<QPlainTextEdit> largeContent = largeSegment
                                                ? largeSegment->findChild<QPlainTextEdit*>(
                                                      QStringLiteral("conversationMessageContent"))
                                                : nullptr;
    QPlainTextEdit* const largeContentAddress = largeContent.data();
    const QString expectedLargeText = QString::fromStdString(largeText);
    passed &= expect(largeContent && largeContent->isReadOnly()
                         && largeContent->toPlainText() == expectedLargeText
                         && largeContent->height() == 240,
                     "large retained user text must remain complete in a bounded read-only editor");

    largeText.replace(0U, 5U, "omega");
    largeFixture.turns.front().messages.front().text = largeText;
    largeConversation.render(makeState({largeFixture}), QStringLiteral("large-user-message"));
    settleTimeline();
    passed &= expect(largeSegment
                         && largeSegment.data()
                                == segment(largeConversation,
                                           QStringLiteral("message:item-large-user-message"))
                         && largeContent && largeContent.data() == largeContentAddress
                         && largeContent->toPlainText() == QString::fromStdString(largeText),
                     "large canonical user-text replacement must update the existing editor in place");
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
                              "completed"},
                             {"item-exact-activity",
                              frontend::ThreadItemKind::CommandExecution,
                              "activity output",
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
    const auto updatedState = makeState({fixture});
    const bool exactApplied = conversation.updateExactMessageContent(
        updatedState, QStringLiteral("exact-content"), exactChanges);
    settleTimeline();

    bool passed = true;
    passed &= expect(exactApplied && first && first.data() == firstAddress && firstContent
                         && firstContent->text() == QStringLiteral("first canonical continuation"),
                     "an exact content update must mutate its canonical message directly in place");
    passed &= expect(second && second.data() == secondAddress && secondContent
                         && secondContent->text() == QStringLiteral("second stable"),
                     "an exact content update must preserve unaffected segment widgets");

    const QHash<QString, QStringList> activityChanges{
        {QStringLiteral("turn-exact-content"),
         QStringList{QStringLiteral("item-exact-activity")}}};
    passed &= expect(!conversation.updateExactMessageContent(
                         updatedState, QStringLiteral("exact-content"), activityChanges),
                     "a non-message content update must retain the full activity-card reconciliation fallback");

    fixture.turns.front().messages.at(1).text = "second structural fallback";
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

bool testInspectorRevisionOnlyUpdate()
{
    const client::State state = makeState({singleTurn("inspector-revision", 1)});
    codexui::InspectorWidget inspector;
    inspector.resize(420, 700);
    inspector.show();
    inspector.render(state, QStringLiteral("inspector-revision"), true,
                     QStringLiteral("State synced"));
    settleEvents();

    auto* revision = inspector.findChild<QLabel*>(QStringLiteral("inspectorStateRevision"));
    const auto detailCards = inspector.findChildren<QFrame*>(QStringLiteral("inspectorDetailCard"));
    const auto expensivePaneWidgets = inspector.findChildren<QWidget*>();
    const std::uint64_t nextRevision = state.revision() + 7;
    inspector.updateStateRevision(nextRevision);
    QCoreApplication::processEvents();

    return expect(!detailCards.isEmpty()
                      && std::ranges::all_of(detailCards, [](const QFrame* card) {
                             return card->styleSheet().contains(
                                        QStringLiteral("QFrame#inspectorDetailCard"));
                         }),
                  "Inspector card borders must be scoped and never leak to child labels")
           && expect(revision && revision->text() == QString::number(nextRevision),
                  "a revision-only update must refresh the Inspector's factual State revision")
           && expect(revision
                         && revision
                                == inspector.findChild<QLabel*>(
                                    QStringLiteral("inspectorStateRevision"))
                         && expensivePaneWidgets == inspector.findChildren<QWidget*>(),
                     "a revision-only update must preserve every existing Inspector pane widget");
}

bool testStructuredPlanPresentation()
{
    ThreadFixture fixture{"structured-plan",
                          {{"turn-structured-plan",
                            {{"message-structured-plan",
                              frontend::ThreadItemKind::AgentMessage,
                              "Working through the plan"}},
                            frontend::Json{{"explanation", "Keep the implementation focused."},
                                           {"steps",
                                            {"Inspect canonical state",
                                             "Render typed plan",
                                             "Validate behavior"}},
                                           {"statuses", {"completed", "inProgress", "pending"}},
                                           {"totalSteps", 3},
                                           {"truncated", false}}}}};
    const client::State state = makeState({fixture});
    codexui::InspectorWidget inspector;
    inspector.resize(420, 700);
    inspector.show();
    inspector.render(state,
                     QStringLiteral("structured-plan"),
                     true,
                     QStringLiteral("State synced"));
    settleEvents();

    auto steps = inspector.findChildren<QLabel*>(QStringLiteral("inspectorPlanStepText"));
    const auto hasStep = [&steps](const QString& text) {
        return std::ranges::any_of(steps, [&text](const QLabel* label) { return label->text() == text; });
    };
    bool passed = expect(state.thread("structured-plan") != nullptr && steps.size() == 3
                             && hasStep(QStringLiteral("Inspect canonical state"))
                             && hasStep(QStringLiteral("Render typed plan"))
                             && hasStep(QStringLiteral("Validate behavior")),
                         "the Plan tab must render the authoritative ordered typed turn plan");

    fixture.turns.front().plan = frontend::Json{
        {"explanation", "The plan changed."},
        {"steps",
         {"Inspect canonical state", "Render typed plan", "Validate behavior", "Publish result"}},
        {"statuses", {"completed", "completed", "inProgress", "pending"}},
        {"totalSteps", 5},
        {"truncated", true}};
    inspector.render(makeState({fixture}),
                     QStringLiteral("structured-plan"),
                     true,
                     QStringLiteral("State synced"));
    settleEvents();
    steps = inspector.findChildren<QLabel*>(QStringLiteral("inspectorPlanStepText"));
    auto* truncation = inspector.findChild<QLabel*>(QStringLiteral("inspectorPlanTruncation"));
    passed &= expect(steps.size() == 4 && truncation && truncation->text().contains(QStringLiteral("4 of 5")),
                     "incremental plan replacement must refresh ordered steps and truthful truncation state");

    fixture.turns.front().plan.reset();
    inspector.render(makeState({fixture}),
                     QStringLiteral("structured-plan"),
                     true,
                     QStringLiteral("State synced"));
    settleEvents();
    passed &= expect(inspector.findChildren<QLabel*>(QStringLiteral("inspectorPlanStepText")).isEmpty(),
                     "removing the canonical plan must clear stale structured Plan rows");
    return passed;
}

bool testHistoricalTurnDetailsMode()
{
    const client::State state = makeState({sequentialTurns("turn-details", 2)});
    codexui::InspectorWidget inspector;
    inspector.resize(420, 700);
    inspector.show();
    inspector.render(state,
                     QStringLiteral("turn-details"),
                     true,
                     QStringLiteral("State synced"),
                     QStringLiteral("turn-turn-details-1"));
    settleEvents();

    auto* heading = inspector.findChild<QLabel*>(QStringLiteral("inspectorHeading"));
    auto* tabs = inspector.findChild<QTabBar*>(QStringLiteral("inspectorTabs"));
    auto* back = inspector.findChild<QPushButton*>(QStringLiteral("historicalTurnBack"));
    auto* title = inspector.findChild<QLabel*>(QStringLiteral("historicalTurnConfigurationTitle"));
    const auto hasText = [&inspector](const QString& text) {
        return std::ranges::any_of(inspector.findChildren<QLabel*>(),
                                   [&text](const QLabel* label) {
                                       return label->text() == text;
                                   });
    };
    bool passed = expect(heading && heading->text() == QStringLiteral("TURN DETAILS")
                             && tabs && !tabs->isVisible() && tabs->currentIndex() == 3
                             && back && back->isVisible(),
                         "selecting a historical turn must enter the dedicated Turn Details mode");
    passed &= expect(title && title->text() == QStringLiteral("Effective configuration · Turn 2")
                         && hasText(QStringLiteral("Read-only historical record"))
                         && hasText(QStringLiteral("gpt-test"))
                         && hasText(QStringLiteral("/workspace/test")),
                     "Turn Details must show the selected turn's authoritative effective configuration");
    passed &= expect(!inspector.findChild<QLabel*>(QStringLiteral("inspectorStateRevision")),
                     "Turn Details must not mix generic synchronization diagnostics into the historical record");

    bool closeRequested = false;
    QObject::connect(&inspector, &codexui::InspectorWidget::historicalTurnCloseRequested,
                     &inspector, [&closeRequested] { closeRequested = true; });
    back->click();
    passed &= expect(closeRequested,
                     "Turn Details must expose an explicit route back to the normal Inspector");

    inspector.render(state,
                     QStringLiteral("turn-details"),
                     true,
                     QStringLiteral("State synced"));
    settleEvents();
    passed &= expect(heading && heading->text() == QStringLiteral("INSPECTOR")
                         && tabs && tabs->isVisible() && back && !back->isVisible()
                         && inspector.findChild<QLabel*>(QStringLiteral("inspectorStateRevision")),
                     "leaving a historical selection must restore the normal Inspector mode");
    return passed;
}

bool testScopedDuplicateTurnIdentity()
{
    ThreadFixture original{"duplicate-turn-original",
                           {{"shared-turn",
                             {{"original-item",
                               frontend::ThreadItemKind::UserMessage,
                               "original scoped message"}}}}};
    ThreadFixture fork{"duplicate-turn-fork",
                       {{"shared-turn",
                         {{"fork-item",
                           frontend::ThreadItemKind::AgentMessage,
                           "fork scoped message"}}}}};
    const client::State state = makeState({original, fork});
    const auto* originalTurn = state.turn(
        ai::openai::codex::typed::ThreadId{"duplicate-turn-original"},
        ai::openai::codex::typed::TurnId{"shared-turn"});
    const auto* forkTurn = state.turn(
        ai::openai::codex::typed::ThreadId{"duplicate-turn-fork"},
        ai::openai::codex::typed::TurnId{"shared-turn"});

    bool passed = expect(originalTurn && forkTurn && originalTurn != forkTurn
                             && state.turn("shared-turn") == nullptr,
                         "the fixture must retain both scoped turns and reject ambiguous bare lookup");

    codexui::ConversationWidget conversation;
    conversation.resize(900, 700);
    conversation.show();
    conversation.render(state, QStringLiteral("duplicate-turn-original"));
    settleTimeline();
    passed &= expect(hasLabel(conversation, QStringLiteral("original scoped message"))
                         && !hasLabel(conversation, QStringLiteral("fork scoped message")),
                     "conversation rendering must resolve a shared turn ID under the selected original thread");

    conversation.render(state, QStringLiteral("duplicate-turn-fork"));
    settleTimeline();
    passed &= expect(hasLabel(conversation, QStringLiteral("fork scoped message"))
                         && !hasLabel(conversation, QStringLiteral("original scoped message")),
                     "conversation rendering must resolve a shared turn ID under the selected fork thread");

    codexui::InspectorWidget inspector;
    inspector.resize(420, 700);
    inspector.show();
    inspector.render(state,
                     QStringLiteral("duplicate-turn-fork"),
                     true,
                     QStringLiteral("State synced"),
                     QStringLiteral("shared-turn"));
    settleEvents();
    const auto hasInspectorText = [&inspector](const QString& text) {
        return std::ranges::any_of(inspector.findChildren<QLabel*>(),
                                   [&text](const QLabel* label) {
                                       return label->text() == text;
                                   });
    };
    auto* title = inspector.findChild<QLabel*>(QStringLiteral("historicalTurnConfigurationTitle"));
    passed &= expect(title && title->text() == QStringLiteral("Effective configuration · Turn 1")
                         && hasInspectorText(QStringLiteral("Effective settings"))
                         && !hasInspectorText(QStringLiteral("Unavailable")),
                     "historical details must resolve a shared turn ID under the inspected thread");
    return passed;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    codexui::ConversationWidget conversation;

    QWidget* timeline = conversation.findChild<QWidget*>(QStringLiteral("conversationTimeline"));
    QLabel* detail = emptyStateDetail(conversation);
    QFrame* emptyCard = conversation.findChild<QFrame*>(QStringLiteral("conversationEmptyState"));
    bool passed = true;
    passed &= expect(timeline != nullptr, "the conversation timeline must be discoverable");
    passed &= expect(detail != nullptr, "the wrapped empty-state detail must be discoverable");
    passed &= expect(emptyCard
                         && emptyCard->styleSheet().contains(
                             QStringLiteral("QFrame#conversationEmptyState")),
                     "the empty-state border must be scoped and never leak to child labels");
    if (!timeline || !detail || !emptyCard)
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
    passed &= testActivityDisclosureAndFullOutput();
    passed &= testPointerPreservingAppend();
    passed &= testInPlaceMessageReplacement();
    passed &= testCompleteAndLargeUserMessagePresentation();
    passed &= testExactContentInvalidation();
    passed &= testSegmentReplacementShrink();
    passed &= testThreadSwitchWindow();
    passed &= testInspectorRevisionOnlyUpdate();
    passed &= testStructuredPlanPresentation();
    passed &= testHistoricalTurnDetailsMode();
    passed &= testScopedDuplicateTurnIdentity();

    return passed ? 0 : 1;
}
