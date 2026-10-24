// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "codex/NodeGraphJson.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/TimingPresentation.h"
#include "codex/ui/UiStyle.h"

#include <QAccessible>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QThread>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>

using namespace codexui;
using namespace codexui::codex;
using namespace codexui::codex::middle;
using Json = nlohmann::json;
namespace {
int failures = 0;
void check(bool passed, const char *what) {
  if (!passed) {
    ++failures;
    std::cerr << "FAILED: " << what << '\n';
  }
}
void settle() {
  QElapsedTimer wait;
  wait.start();
  do {
    QApplication::processEvents();
    QThread::msleep(1);
  } while (wait.elapsed() < 50);
}
const ProtocolTime *find(const ProtocolTimes &times, std::string_view field) {
  const auto it = std::ranges::find(times, field, &ProtocolTime::field);
  return it == times.end() ? nullptr : &*it;
}
void timestampSchemaAndFormatting() {
  struct Case {
    const char *method;
    bool response;
    Json body;
    const char *path;
    TimeUnit unit;
  };
  const Case cases[]{
      {"thread/items/list", true,
       {{"data", Json::array({{{"startedAtMs", 100001}, {"completedAtMs", 120001},
                               {"item", {{"id", "item"}, {"type", "agentMessage"}}}}})}},
       "data/0/completedAtMs", TimeUnit::Milliseconds},
      {"thread/attachment/add", true,
       {{"attachment", {{"createdAt", 100}, {"payload", {{"createdAt", 999}}}}}},
       "attachment/createdAt", TimeUnit::Seconds},
      {"thread/read",
       true,
       {{"thread",
         {{"createdAt", 0},
          {"updatedAt", 1},
          {"recencyAt", 2},
          {"sectionEnteredAt", 3}}}},
       "thread/createdAt",
       TimeUnit::Seconds},
      {"thread/resume",
       true,
       {{"initialTurnsPage",
         {{"data", Json::array({{{"startedAt", 100},
                                 {"completedAt", 120},
                                 {"durationMs", 20000}}})}}}},
       "initialTurnsPage/data/0/startedAt",
       TimeUnit::Seconds},
      {"thread/timeline/list",
       true,
       {{"data", Json::array({{{"type", "turnCompleted"},
                               {"started_at", 100},
                               {"completed_at", 120},
                               {"duration_ms", 20000}}})}},
       "data/0/completed_at",
       TimeUnit::Seconds},
      {"item/started",
       false,
       {{"startedAtMs", 100001}},
       "startedAtMs",
       TimeUnit::Milliseconds},
      {"item/completed",
       false,
       {{"completedAtMs", 100001}},
       "completedAtMs",
       TimeUnit::Milliseconds},
      {"item/commandExecution/requestApproval",
       false,
       {{"startedAtMs", 100001}},
       "startedAtMs",
       TimeUnit::Milliseconds},
      {"item/fileChange/requestApproval",
       false,
       {{"startedAtMs", 100001}},
       "startedAtMs",
       TimeUnit::Milliseconds},
      {"item/permissions/requestApproval",
       false,
       {{"startedAtMs", 100001}},
       "startedAtMs",
       TimeUnit::Milliseconds},
      {"item/autoApprovalReview/started",
       false,
       {{"startedAtMs", 100001}},
       "startedAtMs",
       TimeUnit::Milliseconds},
      {"item/autoApprovalReview/completed",
       false,
       {{"startedAtMs", 100001}, {"completedAtMs", 101001}},
       "completedAtMs",
       TimeUnit::Milliseconds},
      {"autoApprovalReview/strictReviewRequired",
       false,
       {{"startedAtMs", 100001}},
       "startedAtMs",
       TimeUnit::Milliseconds},
      {"hook/completed",
       false,
       {{"run",
         {{"startedAt", 100}, {"completedAt", 120}, {"durationMs", 20000}}}},
       "run/startedAt",
       TimeUnit::Seconds},
      {"project/read",
       true,
       {{"project", {{"createdAt", 0}, {"updatedAt", 1}, {"recencyAt", 2}}}},
       "project/recencyAt",
       TimeUnit::Seconds},
      {"thread/goal/get",
       true,
       {{"goal", {{"createdAt", 0}, {"updatedAt", 1}}}},
       "goal/updatedAt",
       TimeUnit::Seconds},
      {"plugin/read",
       true,
       {{"plugin", {{"summary", {{"installedAt", 100}}}}}},
       "plugin/summary/installedAt",
       TimeUnit::Seconds},
      {"fs/getMetadata",
       true,
       {{"createdAtMs", 0}, {"modifiedAtMs", 100001}},
       "modifiedAtMs",
       TimeUnit::Milliseconds},
      {"model/list",
       true,
       {{"data", Json::array({{{"upgradeInfo", {{"retirementAt", 100}}}}})}},
       "data/0/upgradeInfo/retirementAt",
       TimeUnit::Seconds},
      {"remoteControl/client/list",
       true,
       {{"data", Json::array({{{"lastSeenAt", 100}}})}},
       "data/0/lastSeenAt",
       TimeUnit::Seconds},
      {"remoteControl/pairing/start",
       true,
       {{"expiresAt", 100}},
       "expiresAt",
       TimeUnit::Seconds},
      {"currentTime/read",
       true,
       {{"currentTimeAt", 100}},
       "currentTimeAt",
       TimeUnit::Seconds},
      {"account/rateLimits/updated",
       false,
       {{"rateLimits", {{"primary", {{"resetsAt", 100}}}}}},
       "rateLimits/primary/resetsAt",
       TimeUnit::Seconds},
      {"item/completed",
       false,
       {{"item", {{"failure", {{"resetsAt", 100}}}}}},
       "item/failure/resetsAt",
       TimeUnit::Seconds}};
  for (const auto &c : cases) {
    const auto times = protocolTimes(c.body, c.method, c.response);
    const auto *value = find(times, c.path);
    check(value && value->unit == c.unit && value->value.has_value(), c.method);
  }
  for (const auto &[type, fields] : std::vector<std::pair<std::string, Json>>{
           {"WorkspaceMessage", {{"createdAt", 100}, {"archivedAt", 200}}},
           {"RateLimitResetCredit", {{"grantedAt", 100}, {"expiresAt", 200}}},
           {"ExternalAgentConfigImportHistory", {{"completedAtMs", 100001}}}})
    check(protocolTimes(objectFromJson(fields), type).size() == fields.size(),
          type.c_str());
  const auto attachmentTimes = protocolTimes(
      Json{{"attachment", {{"createdAt", 100}, {"payload", {{"createdAt", 999}}}}}},
      "thread/attachment/add", true);
  check(!find(attachmentTimes, "attachment/payload/createdAt"),
        "attachment payloads cannot invent schema-declared timestamps");
  auto epoch = ui::timingDetails({protocolTime("createdAt", 0)});
  check(epoch.contains("1970-01-01T00:00:00.000Z"),
        "epoch zero is a real instant");
  check(ui::timingDetails(
            protocolTimes(Json{{"createdAtMs", 0}}, "fs/getMetadata", true))
            .contains("protocol sentinel"),
        "filesystem zero is unavailable, not 1970");
  check(ui::timingDetails({protocolTime("startedAtMs", 100001)})
            .contains("1970-01-01T00:01:40.001Z"),
        "milliseconds are not guessed as seconds");
  check(ui::timingDetails({protocolTime("durationMs", 100001)})
                .contains("100.001 s") &&
            !ui::timingDetails({protocolTime("durationMs", 100001)})
                 .contains("1970"),
        "durations never become dates");
  check(
      ui::timingDetails(
          {protocolTime("createdAt", std::numeric_limits<std::int64_t>::max())})
          .contains("Invalid"),
      "overflowing seconds cannot wrap");
  check(ui::timingDetails({protocolTime("createdAt", {})})
            .contains("Not supplied"),
        "unknown is explicit");
  const qint64 autumnBefore =
      QDateTime(QDate(2026, 10, 25), QTime(0, 30), QTimeZone::UTC)
          .toSecsSinceEpoch();
  const QString before =
      ui::timingDetails({protocolTime("startedAt", autumnBefore)});
  const QString after =
      ui::timingDetails({protocolTime("completedAt", autumnBefore + 3600)});
  check(before.contains("+02:00") && after.contains("+01:00"),
        "DST overlap retains distinct offsets and UTC instants");
  check(
      protocolTimes(Json{{"text", "secret"}, {"output", {{"createdAt", 100}}}},
                    "item/started", false)
          .empty(),
      "arbitrary payload timestamps are never discovered");
  check(protocolTimes(Json{{"createdAt", 1}}, "unknown/method", false).empty(),
        "unknown methods do not expose arbitrary metadata");
  Json many = {{"data", Json::array()}};
  for (int i = 0; i < 10000; ++i)
    many["data"].push_back({{"createdAt", i}});
  const auto bounded = protocolTimes(many, "thread/list", true);
  check(bounded.size() <= 129 &&
            bounded.back().field.find("limit reached") != std::string::npos,
        "large protocol pages have explicit bounded timing extraction");
}

void graphProjectionAndLayout() {
  nodegraph::NodeGraph graph;
  nodegraph::ProtocolUpdater updater(graph);
  const auto publish = [&](const char *method, Json body) {
    return updater.apply({nodegraph::DecodedMessageKind::ServerNotification,
                          method,
                          {},
                          objectFromJson(body)});
  };
  publish("thread/started", {{"thread",
                              {{"id", "timed-thread"},
                               {"createdAt", 100},
                               {"updatedAt", 120},
                               {"recencyAt", 110},
                               {"sectionEnteredAt", 105}}}});
  publish("turn/started",
          {{"threadId", "timed-thread"},
           {"turn",
            {{"id", "turn"}, {"status", "inProgress"}, {"startedAt", 130}}}});
  publish("item/started",
          {{"threadId", "timed-thread"},
           {"turnId", "turn"},
           {"startedAtMs", 130123},
           {"item",
            {{"id", "you"},
             {"type", "userMessage"},
             {"content",
              Json::array({{{"type", "text"},
                            {"text", "Please show all timestamps without "
                                     "changing this layout."}}})}}}});
  publish("item/started", {{"threadId", "timed-thread"},
                           {"turnId", "turn"},
                           {"startedAtMs", 131123},
                           {"item",
                            {{"id", "command"},
                             {"type", "commandExecution"},
                             {"command", "printf 'Timestamp example'"},
                             {"aggregatedOutput", "Timestamp example\n"},
                             {"status", "inProgress"}}}});
  publish(
      "item/completed",
      {{"threadId", "timed-thread"},
       {"turnId", "turn"},
       {"completedAtMs", 133123},
       {"item",
        {{"id", "answer"},
         {"type", "agentMessage"},
         {"phase", "final_answer"},
         {"text", "Completed. **Dates and durations** remain separate."}}}});
  nodegraph::NodeRef thread, you, command, turn;
  {
    auto read = graph.tryRead();
    thread = read->find({nodegraph::NodeKind::Thread, "timed-thread"});
    turn = read->childAt(thread, 0);
    you = read->childAt(turn, 0);
    command = read->childAt(turn, 1);
  }
  ui::NodeGraphUiAdapter adapter(graph);
  const auto goalUpdate =
      publish("thread/goal/updated",
              {{"threadId", "timed-thread"},
               {"goal", {{"createdAt", 140}, {"updatedAt", 150}}}});
  nodegraph::GraphChanged goalChange;
  goalChange.revision = goalUpdate.change.revision;
  goalChange.affected = goalUpdate.change.affected;
  check(adapter.inspectorAffected(goalChange, thread,
                                  ui::InspectorProjection::Timing),
        "nested goal timestamps route to visible timing details");
  publish(
      "hook/completed",
      {{"run", {{"id", "hook"}, {"startedAt", 135}, {"completedAt", 140}}}});
  const auto facts = adapter.inspector(thread, ui::InspectorProjection::Timing);
  check(facts && find(facts->timing, "Goal/createdAt") &&
            find(facts->timing, "Hook:hook/completedAt"),
        "retained peripheral timestamps use existing graph facts");
  auto snapshot = adapter.conversation(thread);
  check(snapshot && snapshot->sections.size() == 1 &&
            snapshot->sections[0].cards.size() == 3,
        "authoritative fixture contains one turn and three cards");
  if (!snapshot || snapshot->sections.empty() ||
      snapshot->sections[0].cards.size() != 3)
    return;
  auto root = snapshot->sections[0].cards[0];
  check(find(root.timing, "turn/startedAt") && find(root.timing, "startedAtMs"),
        "turn root retains both scopes, not an inherited item clock");
  check(!find(snapshot->sections[0].cards[1].timing, "turn/startedAt"),
        "ordinary card never borrows turn start");
  const auto done = publish("turn/completed", {{"threadId", "timed-thread"},
                                               {"turn",
                                                {{"id", "turn"},
                                                 {"status", "completed"},
                                                 {"startedAt", 130},
                                                 {"completedAt", 140},
                                                 {"durationMs", 10000}}}});
  nodegraph::GraphChanged change;
  change.revision = done.change.revision;
  change.affected = done.change.affected;
  auto route = adapter.conversationRoute(change, thread);
  check(route.affected &&
            std::ranges::find(route.items, you) != route.items.end(),
        "turn lifecycle routes to its root");
  auto completed = adapter.conversation(thread);
  const auto completedRoot = completed->sections[0].cards[0];
  check(ui::timingSummary(completedRoot.timing) == ui::timingSummary(root.timing) &&
            ui::timingSummary(completedRoot.timing) == "1970-01-01 01:02:10",
        "completion and duration do not alter the start date/time in the header");
  const auto corrected = publish(
      "turn/completed",
      {{"threadId", "timed-thread"},
       {"turn",
        {{"id", "turn"}, {"status", "completed"}, {"completedAt", 141}}}});
  change.revision = corrected.change.revision;
  change.affected = corrected.change.affected;
  route = adapter.conversationRoute(change, thread);
  check(route.affected && !route.structural,
        "timing-only correction is a presentation delta");
  publish("turn/completed", {{"threadId", "timed-thread"},
                             {"turn",
                              {{"id", "turn"},
                               {"startedAt", nullptr},
                               {"completedAt", nullptr},
                               {"durationMs", nullptr}}}});
  check(find(adapter.conversation(thread)->sections[0].cards[0].timing,
             "turn/completedAt")
                ->value == 141,
        "nullable history cannot erase observed lifecycle timestamps");
  {
    auto write = graph.write();
    write.setField(thread, "localPromptActivityAt", 999);
    static_cast<void>(write.finish());
  }
  const auto row = adapter.threadRow(thread);
  check(row->recencyAt == 999 && find(row->timing, "recencyAt")->value == 110,
        "local sorting activity does not overwrite reported server recency");
  const QFont original = QApplication::font();
  for (const int pointSize : {original.pointSize(), original.pointSize() + 5}) {
    QFont font = original;
    font.setPointSize(pointSize);
    for (const int width : {300, 480, 900}) {
      ConversationCard card(root, false);
      card.setStyleSheet(UiStyle::applicationStyleSheet());
      card.setFont(font);
      card.resize(width, card.settleHeightForWidth(width));
      card.show();
      settle();
      auto *time = card.findChild<QToolButton *>("cardTimingButton");
      auto *copy = card.findChild<QToolButton *>("cardCopyButton");
      auto *fold = card.findChild<QToolButton *>("cardDisclosureButton");
      const QRect geometry = card.geometry();
      const QRect copyRect = copy->geometry(), foldRect = fold->geometry();
      check(time && time->width() > 0 &&
                !time->geometry().intersects(copyRect) &&
                copyRect.right() < foldRect.left(),
            "timestamp and existing actions do not overlap");
      bool invoked = false;
      QObject::connect(&card, &ConversationCard::timingRequested, &card,
                       [&](auto target) { invoked = target == you; });
      time->setFocus(Qt::TabFocusReason);
      QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
      QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
      QApplication::sendEvent(time, &press);
      QApplication::sendEvent(time, &release);
      check(invoked, "timing details are keyboard accessible");
      auto update = root;
      update.timing = completedRoot.timing;
      check(card.applyPresentation(update) == PresentationImpact::PaintOnly,
            "timestamp-only update is paint-only");
      settle();
      check(card.geometry() == geometry && copy->geometry() == copyRect &&
                fold->geometry() == foldRect && time->hasFocus(),
            "time update does not move pixels, copy/disclosure geometry or "
            "focus");
      check(card.applyPresentation(update) == PresentationImpact::None,
            "timestamp no-op stays a no-op");
      auto *accessible = QAccessible::queryAccessibleInterface(time);
      check(accessible &&
                accessible->text(QAccessible::Name) == "Timing details" &&
                accessible->text(QAccessible::Description).contains("UTC:"),
            "timing action exposes exact accessible details");
    }
  }
  MiddleRegionWidget region;
  region.setStyleSheet(UiStyle::applicationStyleSheet());
  region.show();
  region.conversation().setTimingAction(
      [&](auto target) { region.inspector().showTiming(target); });
  region.inspector().setRefreshRequestedAction([&] {
    if (region.inspector().currentProjection() ==
        ui::InspectorProjection::Timing) {
      auto details = adapter.inspector(
          thread, ui::InspectorProjection::Timing,
          region.inspector().rowRequest(ui::InspectorProjection::Timing));
      if (details)
        region.inspector().refresh(*details, ui::InspectorProjection::Timing);
    }
  });
  static_cast<void>(region.conversation().reconcileStaged(*completed));
  region.inspector().showTiming(command);
  settle();
  auto *detail = region.findChild<QPlainTextEdit *>("timingInfoView");
  check(
      detail && detail->toPlainText().contains("ItemLifecycle/startedAtMs") &&
          detail->toPlainText().contains("Thread/sectionEnteredAt"),
      "existing Info panel shows selected card, owning turn and thread times");
  const QString screenshotDir =
      qEnvironmentVariable("CODEXUI_TIMESTAMP_SCREENSHOTS");
  if (!screenshotDir.isEmpty())
    QDir().mkpath(screenshotDir);
  for (const int width : {800, 1500}) {
    region.resize(width, 850);
    settle();
    check(detail->width() > 0 && detail->isVisible(),
          "narrow and wide Info text remains usable");
    if (!screenshotDir.isEmpty())
      check(region.grab().save(screenshotDir +
                               QStringLiteral("/timing-%1-dpr-%2.png")
                                   .arg(width)
                                   .arg(region.devicePixelRatioF())),
            "save actual widget screenshot");
  }
}

void sharedHeaderContractAndAlignment() {
    std::ifstream input(CODEXUI_PRESENTATION_CONTRACT_PATH);
    const auto fixture = Json::parse(input, nullptr, false);
    check(fixture.is_object() && fixture.contains("cardHeader"), "shared header contract exists");
    if (fixture.is_object() && fixture.contains("cardHeader")) {
        const auto& contract = fixture.at("cardHeader");
        for (const auto& entry : contract.at("timingCases")) {
            auto times = protocolTimes(objectFromJson(entry.at("body")), entry.at("type").get<std::string>());
            if (entry.contains("turn")) {
                auto owning = protocolTimes(objectFromJson(entry.at("turn")), "Turn");
                for (auto& time : owning) {
                    time.field = "turn/" + time.field;
                    times.push_back(std::move(time));
                }
            }
            check(ui::timingSummary(times).toStdString() == entry.at("expected").get<std::string>(),
                  entry.at("id").get<std::string>().c_str());
        }
        MiddleRegionWidget region;
        for (const auto& name : {"conversationReasoningToggle",
                                 "conversationUpdatesToggle",
                                 "conversationCommandFoldingToggle",
                                 "conversationImageFoldingToggle",
                                 "conversationFileChangesFoldingToggle"}) {
            const auto* control = region.findChild<QToolButton*>(name);
            check(control && control->size() == QSize(contract.at("controlWidth").get<int>(), contract.at("controlHeight").get<int>()) &&
                      control->iconSize() == QSize(contract.at("iconSize").get<int>(), contract.at("iconSize").get<int>()),
                  "shared header geometry also matches the CONVERSATION toolbar controls");
        }
        const QFont original = QApplication::font();
        for (int fontDelta : {0, 5}) {
            QFont font = original;
            font.setPointSize(original.pointSize() + fontDelta);
            QApplication::setFont(font);
            qApp->setStyleSheet(UiStyle::applicationStyleSheet());
            for (int width : {300, 480, 900}) {
                QWidget surface;
                auto* stack = new QVBoxLayout(&surface);
                stack->setContentsMargins(0, 0, 0, 0);
                std::vector<std::unique_ptr<ConversationCard>> cards;
                for (int index = 0; index < 5; ++index) {
                    VisibleCardData data{AuthoritativeItemKey{"header", "t", std::to_string(index)},
                                         CardKind::AgentMessage,
                                         "header",
                                         "t",
                                         std::to_string(index),
                                         AgentMessageData{"Body", index == 1 ? "partial_answer" : "commentary"}};
                    if (index == 2) {
                        data.kind = CardKind::UserMessage;
                        data.payload = UserMessageData{"Steered prompt", {}};
                    } else if (index == 3) {
                        data.kind = CardKind::CommandExecution;
                        data.payload = CommandExecutionData{"true", "output", {}, 0, 1234};
                        data.status = nodegraph::NodeStatus::Completed;
                    } else if (index == 4) {
                        data.kind = CardKind::Reasoning;
                        data.payload = ReasoningData{};
                    }
                    data.turnOpening = index == 0;
                    data.timing = {protocolTime("startedAtMs", 1792861806123),
                                   protocolTime("completedAtMs", 1792861806456),
                                   protocolTime("durationMs", 333)};
                    auto card = std::make_unique<ConversationCard>(data, false);
                    static_cast<void>(card->setNestedPresentation(index == 2));
                    card->resize(width, card->settleHeightForWidth(width));
                    stack->addWidget(card.get());
                    cards.push_back(std::move(card));
                }
                surface.resize(width, surface.sizeHint().height());
                surface.show();
                settle();
                check(surface.width() == width, "the header never widens its requested viewport");
                int timingRight = -1, copyLeft = -1;
                for (const auto& card : cards) {
                    auto* header = card->findChild<QWidget*>("conversationCardHeader");
                    auto* time = card->findChild<QToolButton*>("cardTimingButton");
                    auto* copy = card->findChild<QToolButton*>("cardCopyButton");
                    auto* fold = card->findChild<QToolButton*>("cardDisclosureButton");
                    const int menuWidth = card == cards.front() ? contract.at("controlWidth").get<int>() : 0;
                    const int right = time->mapTo(card.get(), time->rect().topRight()).x() + menuWidth;
                    const int copyX = copy->mapTo(card.get(), QPoint{}).x();
                    if (timingRight < 0) {
                        timingRight = right;
                        copyLeft = copyX;
                    }
                    check(right == timingRight && (copy->isHidden() || copyX == copyLeft),
                          "nested headers share aligned timestamps and omit the opening header's menu slot; Copy stays aligned throughout");
                    check(time->width() <= time->sizeHint().width(), "the timestamp never centers its text inside unused flexible space");
                    check(copy->size() == QSize(contract.at("controlWidth").get<int>(), contract.at("controlHeight").get<int>()) &&
                              fold->size() == copy->size(),
                          "shared controls use the toolbar hit area");
                    check(time->geometry().right() < header->width() && header->height() <= std::max(24, QFontMetrics(font).height()),
                          "the header fits one row without horizontal overflow");
                    if (width == 900) {
                        check(time->width() >= time->fontMetrics().horizontalAdvance(time->text()) + 8,
                              "normal and enlarged headers show the complete date/time when space is available");
                    }
                    for (auto* label : header->findChildren<QLabel*>()) {
                        check(label->toolTip() == label->text(), "bounded header labels retain their complete text on hover");
                        check(label->width() > 0 && label->geometry().right() <= time->geometry().left(),
                              "title and status stay to the left of the timestamp");
                    }
                }
                const auto* metadata = cards[3]->findChild<QLabel*>("commandMetadata");
                check(metadata && metadata->text().contains("1.2 s"), "command duration remains below its output");
                const QString evidence = qEnvironmentVariable("CODEXUI_TIMESTAMP_SCREENSHOTS");
                if (!evidence.isEmpty()) {
                    surface.grab().save(evidence + QStringLiteral("/aligned-headers-%1-font-%2-dpr-%3.png")
                                                       .arg(width)
                                                       .arg(fontDelta)
                                                       .arg(surface.devicePixelRatioF()));
                }
            }
        }
        QApplication::setFont(original);
        qApp->setStyleSheet(UiStyle::applicationStyleSheet());
    }
}

} // namespace

int main(int argc, char **argv) {
  qputenv("TZ", "Europe/Vienna");
  tzset();
  QApplication app(argc, argv);
  timestampSchemaAndFormatting();
  graphProjectionAndLayout();
  sharedHeaderContractAndAlignment();
  std::cout << "Timestamp contract/layout failures: " << failures << '\n';
  return failures == 0 ? 0 : 1;
}
