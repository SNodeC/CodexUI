// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

// SNode.C declares emit() before Qt defines its emit macro.
// clang-format off
#include <core/SNodeC.h>
#include <utils/Config.h>
// clang-format on
#include "codex/Configuration.h"
#include "codex/FrontendSession.h"
#include "codex/NodeGraphJson.h"
#include "codex/PendingRequestDialog.h"
#include "codex/ShellWidget.h"
#include "codex/middle/ConversationCards.h"
#include "codex/middle/ConversationItemModel.h"
#include "codex/middle/ConversationPresentation.h"
#include "codex/middle/ConversationView.h"
#include "codex/nodegraph/PromptText.h"
#include "codex/nodegraph/WorkerLogic.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextFragment>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>

namespace codexui::codex {
    class FrontendSessionTestPeer final {
    public:
        static nodegraph::NodeGraph& graph(FrontendSession& session) {
            return session.graph;
        }
        static nodegraph::ThreadChannels& channels(FrontendSession& session) {
            return session.channels;
        }
    };
} // namespace codexui::codex

namespace {
    using namespace codexui::codex;
    using namespace codexui::nodegraph;
    int failures = 0;

    void check(bool condition, const std::string& message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL " << message << '\n';
        }
    }

    void spin() {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 100) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
    }

    void notify(WorkerLogic& worker, std::string method, nlohmann::json params) {
        static_cast<void>(worker.apply({DecodedMessageKind::ServerNotification, std::move(method), std::nullopt, objectFromJson(params)}));
    }

    std::vector<NodeAction> takeActions(ThreadChannels& channels) {
        static_cast<void>(channels.drainQtToWorkerWake());
        QtToWorkerMessage message;
        std::vector<NodeAction> actions;
        while (channels.tryReceiveForWorker(message)) {
            if (const auto* action = std::get_if<NodeAction>(&message)) {
                actions.push_back(*action);
            }
        }
        return actions;
    }

    void corpus(const nlohmann::json& fixture) {
        for (const auto& entry : fixture["cases"]) {
            const auto request = PendingRequestPolicy::asyncQuestionRequest(entry["questions"]);
            check(request.is_null() == !entry.contains("text"), entry["id"]);
            if (entry.contains("text") && !request.is_null()) {
                const PendingRequestSubmission submission{"submit", entry["answer"]};
                check(PendingRequestPolicy::asyncQuestionAnswer(request, submission, "question") == entry["wire"].get<std::string>(),
                      entry["id"].get<std::string>() + " preserves question context and chosen answers");
                check(!PendingRequestPolicy::asyncQuestionAnswer(request, {"decline", entry["answer"]}, "question"),
                      "decline is not an implicit answer");
            }
        }
        check(PendingRequestPolicy::asyncQuestionRequest(nlohmann::json::array({{{"title", std::string(4097, 'x')}}})).is_null(),
              "oversized title is bounded");
        check(PendingRequestPolicy::asyncQuestionRequest(
                  nlohmann::json::array({{{"title", "Question"}, {"options", std::vector<std::string>(65, "choice")}}}))
                  .is_null(),
              "oversized choices are bounded");
    }

    void replyCorpus(const nlohmann::json& fixture) {
        for (const auto& entry : fixture["cases"]) {
            if (entry.contains("wire")) {
                const auto replies = questionReplies(entry["wire"].get<std::string>());
                check(replies.size() == entry["questions"].size(), "each question has its own explicit reply link");
                check(questionReplyMarkdown(replies) == entry["markdown"].get<std::string>(),
                      "readable replies emphasize answers without envelope text");
                for (std::size_t i = 0; i < replies.size(); ++i) {
                    check(replies[i].questionItemId == questionReplyKey("question", i),
                          "reply identity includes source item and question index");
                }
                QTextDocument document;
                document.setMarkdown(
                    middle::presentation::userMessageMarkdown(QString::fromStdString(entry["wire"].get<std::string>()), true));
                const auto selected = document.find(QString::fromStdString(entry["answer"]["0"]["answers"][0].get<std::string>()));
                check(!selected.isNull() && selected.charFormat().fontWeight() >= QFont::Bold, "chosen answer renders in bold");
                check(!document.toPlainText().contains("send_user_message_question_reply"), "wire envelope is hidden in the reply card");
            }
        }
        for (const auto& entry : fixture["replyCases"]) {
            const auto replies = questionReplies(entry["source"].get<std::string>());
            check(replies.size() == entry["replies"].size(), entry["id"].get<std::string>() + " parser shape");
            if (!replies.empty()) {
                check(questionReplyMarkdown(replies) == entry["markdown"].get<std::string>(), entry["id"].get<std::string>() + " display");
            }
        }
        nlohmann::json questions = nlohmann::json::array(), answers = nlohmann::json::object();
        for (int index = 0; index < 32; ++index) {
            questions.push_back({{"title", std::string(4096, '\1')}});
            answers[std::to_string(index)] = {{"answers", nlohmann::json::array({std::string(4096, '\2')})}};
        }
        const auto wire = PendingRequestPolicy::asyncQuestionAnswer(
            PendingRequestPolicy::asyncQuestionRequest(questions), {"submit", answers}, "question");
        check(wire && wire->size() > 1048576 && questionReplies(*wire).size() == 32,
              "maximum supported free-text answers preserve every explicit link");
        return;
    }

    void history(const nlohmann::json& fixture) {
        for (const auto& entry : fixture["cases"]) {
            NodeGraph graph;
            ProtocolUpdater updater(graph);
            const ProtocolRequestId id("history");
            const auto operation =
                updater.apply({DecodedMessageKind::ClientRequest, "thread/read", id, {{"threadId", "history"}, {"includeTurns", true}}});
            const auto item = nlohmann::json{{"id", "question"},
                                             {"type", "agentMessage"},
                                             {"phase", "final_answer"},
                                             {"delivery", "async"},
                                             {"text", "Retained question"},
                                             {"questions", entry["questions"]}};
            const auto response = nlohmann::json{
                {"thread",
                 {{"id", "history"},
                  {"turns",
                   nlohmann::json::array({{{"id", "turn"}, {"status", "completed"}, {"items", nlohmann::json::array({item})}}})}}}};
            static_cast<void>(
                updater.apply({DecodedMessageKind::ClientResult, "thread/read", id, objectFromJson(response), operation.primary}));
            NodeRef thread;
            {
                auto read = graph.tryRead();
                thread = read->find({NodeKind::Thread, "history"});
            }
            ui::NodeGraphUiAdapter adapter(graph);
            const auto conversation = adapter.conversation(thread);
            check(conversation && conversation->sections.size() == 1 && conversation->sections[0].cards.size() == 1,
                  entry["id"].get<std::string>() + " historical message is retained");
            if (conversation && !conversation->sections.empty() && !conversation->sections[0].cards.empty()) {
                const auto* message = std::get_if<middle::AgentMessageData>(&conversation->sections[0].cards[0].payload);
                check(message && message->questionRequest.is_null() == !entry.contains("text"),
                      entry["id"].get<std::string>() + " history and live question projection agree");
            }
        }
    }

    void duplicateReplyHistory(const nlohmann::json& fixture) {
        NodeGraph graph;
        ProtocolUpdater updater(graph);
        const auto wire = fixture["cases"][0]["wire"].get<std::string>();
        auto records = nlohmann::json::parse(wire.substr(34, wire.size() - 69));
        records.push_back(records[0]);
        auto reply = nlohmann::json{
            {"id", "reply"},
            {"type", "userMessage"},
            {"content",
             nlohmann::json::array(
                 {{{"type", "text"},
                   {"text", "<send_user_message_question_reply>\n" + records.dump() + "\n</send_user_message_question_reply>"}}})}};
        const ProtocolRequestId id("duplicate");
        const auto operation =
            updater.apply({DecodedMessageKind::ClientRequest, "thread/read", id, {{"threadId", "duplicates"}, {"includeTurns", true}}});
        static_cast<void>(updater.apply(
            {DecodedMessageKind::ClientResult,
             "thread/read",
             id,
             objectFromJson(
                 {{"thread",
                   {{"id", "duplicates"},
                    {"turns",
                     nlohmann::json::array({{{"id", "turn"}, {"status", "completed"}, {"items", nlohmann::json::array({reply})}}})}}}}),
             operation.primary}));
        reply["content"][0]["text"] = wire;
        static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                         "item/completed",
                                         std::nullopt,
                                         objectFromJson({{"threadId", "duplicates"}, {"turnId", "turn"}, {"item", reply}})}));
        const auto question = nlohmann::json{{"id", "question"},
                                             {"type", "agentMessage"},
                                             {"delivery", "async"},
                                             {"text", "Approve?"},
                                             {"questions", fixture["cases"][0]["questions"]}};
        static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                         "item/completed",
                                         std::nullopt,
                                         objectFromJson({{"threadId", "duplicates"}, {"turnId", "turn"}, {"item", question}})}));
        NodeRef thread;
        {
            auto read = graph.tryRead();
            thread = read->find({NodeKind::Thread, "duplicates"});
        }
        ui::NodeGraphUiAdapter adapter(graph);
        const auto snapshot = adapter.conversation(thread);
        for (const auto& card : snapshot->sections[0].cards) {
            if (card.itemId == "question") {
                check(std::get<middle::AgentMessageData>(card.payload).questionStatus == "✓ Answer sent",
                      "duplicate reply links before the source page arrives remain safe and reconstruct one answer");
            }
        }
        return;
    }

    void persistedReplies(const nlohmann::json& fixture) {
        for (const auto& entry : fixture["cases"]) {
            if (entry.contains("wire")) {
                for (const bool answerFirst : {false, true}) {
                    NodeGraph graph;
                    ProtocolUpdater updater(graph);
                    const auto question = nlohmann::json{{"id", "question"},
                                                         {"type", "agentMessage"},
                                                         {"delivery", "async"},
                                                         {"phase", "commentary"},
                                                         {"text", "Original question"},
                                                         {"questions", entry["questions"]}};
                    auto reply = nlohmann::json{{"id", "reply"},
                                                {"type", "userMessage"},
                                                {"content", nlohmann::json::array({{{"type", "text"}, {"text", entry["wire"]}}})}};
                    const auto items = answerFirst ? nlohmann::json::array({reply, question}) : nlohmann::json::array({question, reply});
                    const ProtocolRequestId id("restore");
                    const auto operation = updater.apply(
                        {DecodedMessageKind::ClientRequest, "thread/read", id, {{"threadId", "restore"}, {"includeTurns", true}}});
                    static_cast<void>(updater.apply(
                        {DecodedMessageKind::ClientResult,
                         "thread/read",
                         id,
                         objectFromJson(
                             {{"thread",
                               {{"id", "restore"},
                                {"turns", nlohmann::json::array({{{"id", "turn"}, {"status", "completed"}, {"items", items}}})}}}}),
                         operation.primary}));
                    NodeRef thread;
                    {
                        auto read = graph.tryRead();
                        thread = read->find({NodeKind::Thread, "restore"});
                    }
                    ui::NodeGraphUiAdapter adapter(graph);
                    const auto restored = adapter.conversation(thread);
                    NodeRef questionNode, replyNode;
                    for (const auto& card : restored->sections[0].cards) {
                        if (card.itemId == "question") {
                            questionNode = card.target;
                            const auto& data = std::get<middle::AgentMessageData>(card.payload);
                            check(data.questionStatus == "✓ Answer sent" && data.text == entry["markdown"].get<std::string>(),
                                  "reload restores both answer status and readable choices");
                            if (entry["id"] == "two-independent-choices") {
                                for (const int width : {300, 440, 900}) {
                                    middle::ConversationCard rendered(card, false, nullptr, width);
                                    rendered.resize(width, rendered.settleHeightForWidth(width));
                                    rendered.show();
                                    QApplication::processEvents();
                                    auto* label = rendered.findChild<QLabel*>(QStringLiteral("agentMessagePhase"));
                                    const QRect bounds(label->mapTo(&rendered, QPoint{}), label->size());
                                    const auto textBounds =
                                        label->fontMetrics().boundingRect(label->contentsRect(), Qt::TextWordWrap, label->text());
                                    check(bounds.left() >= 0 && bounds.right() < width &&
                                              textBounds.width() <= label->contentsRect().width() &&
                                              textBounds.height() <= label->contentsRect().height(),
                                          "answered status remains fully readable at narrow and normal widths");
                                    rendered.grab().save(QString::fromStdString(
                                        "/tmp/codex/build/CodexUI/async-answer-state/answered-width-" + std::to_string(width) + "-" +
                                        qEnvironmentVariable("QT_SCALE_FACTOR").toStdString() + ".png"));
                                }
                            }

                        } else if (card.itemId == "reply") {
                            replyNode = card.target;
                        }
                    }
                    auto write = graph.write();
                    write.remove(replyNode);
                    const auto removed = write.finish();
                    const auto route = adapter.conversationRoute({removed.revision,
                                                                  removed.affected,
                                                                  removed.removed,
                                                                  false,
                                                                  removed.childListsChanged,
                                                                  removed.providerAuthorityRevision},
                                                                 thread);
                    check(route.affected && !route.authorityReplacement &&
                              std::ranges::find(route.items, questionNode) != route.items.end(),
                          "removing a reply targets its question without a conversation rescan");
                    const auto delta = adapter.conversationDelta(thread, std::array{questionNode}, false);
                    check(delta && std::get<middle::AgentMessageData>(delta->presentations[0].payload).questionStatus.empty(),
                          "removing a reply removes its answered status");
                    reply["content"][0]["text"] = entry["text"];
                    static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                                     "item/completed",
                                                     std::nullopt,
                                                     objectFromJson({{"threadId", "restore"}, {"turnId", "turn"}, {"item", reply}})}));
                    const auto legacy = adapter.conversationDelta(thread, std::array{questionNode}, false);
                    check(legacy && std::get<middle::AgentMessageData>(legacy->presentations[0].payload).questionStatus.empty(),
                          "legacy plain-text answers are never guessed from text");
                    reply["content"][0]["text"] = entry["wire"];
                    static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                                     "item/completed",
                                                     std::nullopt,
                                                     objectFromJson({{"threadId", "restore"}, {"turnId", "turn"}, {"item", reply}})}));
                    const auto replaced = adapter.conversationDelta(thread, std::array{questionNode}, false);
                    check(replaced &&
                              std::get<middle::AgentMessageData>(replaced->presentations[0].payload).questionStatus == "✓ Answer sent",
                          "a reply loaded after the question restores status incrementally");
                }
            }
        }
    }

    void visibility(const nlohmann::json& fixture) {
        for (bool retained : {false, true}) {
            NodeGraph graph;
            ProtocolUpdater updater(graph);
            nlohmann::json items = nlohmann::json::array();
            for (const auto& entry : fixture["visibilityCases"]) {
                auto item = entry["item"];
                item.update({{"id", entry["id"]}, {"type", "agentMessage"}, {"text", "Visibility fixture"}});
                items.push_back(std::move(item));
            }
            const ProtocolRequestId id("visibility");
            const auto operation =
                updater.apply({DecodedMessageKind::ClientRequest, "thread/read", id, {{"threadId", "visibility"}, {"includeTurns", true}}});
            const nlohmann::json response{
                {"thread",
                 {{"id", "visibility"},
                  {"turns",
                   nlohmann::json::array(
                       {{{"id", "turn"}, {"status", "completed"}, {"items", retained ? items : nlohmann::json::array()}}})}}}};
            static_cast<void>(
                updater.apply({DecodedMessageKind::ClientResult, "thread/read", id, objectFromJson(response), operation.primary}));
            if (!retained) {
                for (const auto& item : items) {
                    static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                                     "item/completed",
                                                     std::nullopt,
                                                     objectFromJson({{"threadId", "visibility"}, {"turnId", "turn"}, {"item", item}})}));
                }
            }
            NodeRef thread;
            {
                auto read = graph.tryRead();
                thread = read->find({NodeKind::Thread, "visibility"});
            }
            ui::NodeGraphUiAdapter adapter(graph);
            middle::ConversationItemModel model;
            const auto snapshot = adapter.conversation(thread);
            check(snapshot.has_value(), "visibility conversation is projected");
            if (snapshot) {
                static_cast<void>(model.replaceConversation(*snapshot));
                check(model.rowCount() == static_cast<int>(items.size()), "visibility cases retain every model row");
                for (bool updates : {false, true, false}) {
                    static_cast<void>(model.setVisibility({true, updates}));
                    for (std::size_t index = 0; index < items.size(); ++index) {
                        const auto& entry = fixture["visibilityCases"][index];
                        const bool presented =
                            model.data(model.index(static_cast<int>(index), 0), middle::ConversationItemModel::PresentedRole).toBool();
                        check(presented == (updates || entry["visibleWhenUpdatesHidden"].get<bool>()),
                              std::string(retained ? "history " : "live ") + entry["id"].get<std::string>() +
                                  (updates ? " with updates shown" : " with updates hidden"));
                    }
                }
                for (bool populated : {false, true}) {
                    auto changed = items[0];
                    if (!populated) {
                        changed["questions"] = nlohmann::json::array();
                    }
                    static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification,
                                                     "item/completed",
                                                     std::nullopt,
                                                     objectFromJson({{"threadId", "visibility"}, {"turnId", "turn"}, {"item", changed}})}));
                    static_cast<void>(model.replaceConversation(*adapter.conversation(thread)));
                    check(model.data(model.index(0, 0), middle::ConversationItemModel::PresentedRole).toBool() == populated,
                          "clearing and restoring questions updates visibility while commentary is hidden");
                }
            }
        }
    }

    void form(const nlohmann::json& entry) {
        PendingRequestDescriptor request;
        request.kind = PendingRequestKind::UserInput;
        request.method = "agentMessage";
        request.raw = PendingRequestPolicy::asyncQuestionRequest(entry["questions"]);
        request.availability = PendingRequestAvailability::Actionable;
        bool held = false;
        int attempts = 0;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog != nullptr, "question form is modal");
            if (dialog) {
                const auto choices = dialog->findChildren<QRadioButton*>();
                check(choices.size() == static_cast<qsizetype>(entry["questions"].size() * 3) && choices[0]->isChecked(),
                      "first option is preselected with an exclusive free-text alternative");
                for (std::size_t i = 0; i < entry["questions"].size(); ++i) {
                    for (auto* choice : choices) {
                        if (choice->parentWidget()->accessibleDescription().toStdString() ==
                                entry["questions"][i]["title"].get<std::string>() &&
                            choice->text().toStdString() == entry["answer"][std::to_string(i)]["answers"][0].get<std::string>()) {
                            choice->click();
                        }
                    }
                }
                check(choices[1]->isChecked(), "selecting the second question preserves the first question's independent choice");
                dialog->grab().save(QString::fromStdString("/tmp/codex/build/CodexUI/async-answer-state/question-form-" +
                                                           qEnvironmentVariable("QT_SCALE_FACTOR").toStdString() + ".png"));
                QTimer::singleShot(0, [&] {
                    auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    if (warning) {
                        warning->accept();
                    }
                });
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                buttons->button(QDialogButtonBox::Ok)->click();
                held = dialog->isVisible() && choices[1]->isChecked();
                buttons->button(QDialogButtonBox::Ok)->click();
            }
        });
        const auto result = PendingRequestDialog::present(request, nullptr, nullptr, {}, [&](const PendingRequestSubmission& submission) {
            ++attempts;
            check(PendingRequestPolicy::asyncQuestionAnswer(request.raw, submission, "question") ==
                      std::optional<std::string>(entry["wire"].get<std::string>()),
                  "dialog submits the selected answer, not the default");
            return attempts == 2;
        });
        check(result.has_value() && held && attempts == 2, "failed admission retains the form and selected answer for deliberate retry");
    }

    void shell(Configuration& configuration, const nlohmann::json& questions, const std::string& fixtureAnswer, int scenario) {
        FrontendSession session(configuration);
        auto& graph = FrontendSessionTestPeer::graph(session);
        auto& channels = FrontendSessionTestPeer::channels(session);
        WorkerLogic worker(graph, channels);
        ShellWidget shell(session);
        shell.resize(1200, 800);
        shell.show();
        static_cast<void>(worker.transportEvent("connected"));
        static_cast<void>(worker.bridgeState("test", "controller", "test", 1, "ready"));
        notify(worker,
               "thread/started",
               {{"thread", {{"id", "questions"}, {"name", "Questions"}, {"cwd", "/tmp"}, {"status", {{"type", "idle"}}}}}});
        NodeRef thread;
        {
            auto read = graph.tryRead();
            thread = read->find({NodeKind::Thread, "questions"});
        }
        static_cast<void>(worker.threadHydration(thread, "ready"));
        notify(worker, "turn/started", {{"threadId", "questions"}, {"turn", {{"id", "turn"}, {"status", "inProgress"}}}});
        const auto item = nlohmann::json{{"id", "question"},
                                         {"type", "agentMessage"},
                                         {"phase", "commentary"},
                                         {"delivery", "async"},
                                         {"text", "Approve the follow-up PR?"},
                                         {"questions", questions}};
        notify(worker, "item/completed", {{"threadId", "questions"}, {"turnId", "turn"}, {"item", item}});
        spin();
        auto* list = shell.findChild<QTreeWidget*>(QStringLiteral("threadList"));
        QTreeWidgetItemIterator iterator(list);
        while (*iterator && (*iterator)->data(0, Qt::UserRole).toString() != QStringLiteral("questions")) {
            ++iterator;
        }
        if (*iterator) {
            list->setCurrentItem(*iterator);
        }
        spin();
        ui::NodeGraphUiAdapter adapter(graph);
        auto* view = shell.findChild<middle::ConversationView*>();
        auto* updates = shell.findChild<QToolButton*>(QStringLiteral("conversationUpdatesToggle"));
        check(updates != nullptr, "native updates visibility control is available");
        if (updates && updates->isChecked()) {
            updates->click();
            spin();
        }
        check(view && !view->presentationOptions().showCodexUpdates, "native updates control hides commentary updates");
        const auto conversation = adapter.conversation(thread);
        check(conversation && !conversation->sections.empty(), "async message survives authoritative projection");
        auto* answer = shell.findChild<QPushButton*>(QStringLiteral("answerAsyncQuestion"));
        check(answer && answer->isVisible(), "async message exposes an accessible Answer action");
        QWidget* card = answer;
        while (card && !qobject_cast<middle::ConversationCard*>(card)) {
            card = card->parentWidget();
        }
        auto* phase = card ? card->findChild<QLabel*>(QStringLiteral("agentMessagePhase")) : nullptr;
        check(phase && phase->text() == QStringLiteral("Question"), "structured question replaces the protocol final-answer label");
        check(phase && phase->palette().color(QPalette::WindowText) == QColor(codexui::UiStyle::questionMessageText),
              "question label uses the accepted lime text");
        if (card) {
            const QImage image = card->grab().toImage();
            check(image.pixelColor(image.width() / 2, image.height() - 6) == QColor(codexui::UiStyle::questionMessageSurface),
                  "question card paints the accepted lime background");
            check(image.pixelColor(image.width() / 2, 0) == QColor(codexui::UiStyle::questionMessageBorder),
                  "question card paints the accepted green border");
            if (scenario == 0) {
                shell.grab().save(QString::fromStdString("/tmp/codex/build/CodexUI/async-answer-state/question-card-" +
                                                         qEnvironmentVariable("QT_SCALE_FACTOR").toStdString() + ".png"));
            }
        }
        const auto questionTarget = card ? qobject_cast<middle::ConversationCard*>(card)->data().target : NodeRef{};
        const auto revealQuestion = [&] {
            auto* model = view ? qobject_cast<middle::ConversationItemModel*>(view->model()) : nullptr;
            if (model) {
                view->verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
                view->scrollTo(model->indexForTarget(questionTarget), QAbstractItemView::PositionAtCenter);
            }
            spin();
            card = nullptr;
            for (auto* candidate : shell.findChildren<middle::ConversationCard*>()) {
                if (candidate->data().target == questionTarget) {
                    card = candidate;
                }
            }
            phase = card ? card->findChild<QLabel*>(QStringLiteral("agentMessagePhase")) : nullptr;
            answer = card ? card->findChild<QPushButton*>(QStringLiteral("answerAsyncQuestion")) : nullptr;
            check(card && phase, "the original question rematerializes by stable identity after scrolling");
            return;
        };
        static_cast<void>(takeActions(channels));
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (scenario == 1) {
                    buttons->button(QDialogButtonBox::Cancel)->click();
                } else {
                    const auto choices = dialog->findChildren<QRadioButton*>();
                    for (const auto& reply : questionReplies(fixtureAnswer)) {
                        for (auto* choice : choices) {
                            if (choice->parentWidget()->accessibleDescription().toStdString() == reply.question &&
                                choice->text().toStdString() == reply.answer) {
                                choice->click();
                            }
                        }
                    }
                    if (scenario == 2) {
                        auto changed = item;
                        changed["questions"][0]["title"] = "A different question";
                        notify(worker, "item/completed", {{"threadId", "questions"}, {"turnId", "turn"}, {"item", changed}});
                    } else if (scenario == 3) {
                        static_cast<void>(worker.bridgeState("observer", "observer", "other", 1, "ready"));
                        spin();
                    }
                    if (scenario >= 2) {
                        QTimer::singleShot(0, [&] {
                            auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                            if (warning) {
                                warning->accept();
                            }
                        });
                    }
                    buttons->button(QDialogButtonBox::Ok)->click();
                    if (scenario >= 2) {
                        check(dialog->isVisible(), "stale question or lost control keeps authored input visible");
                        dialog->reject();
                    }
                }
            }
        });
        if (answer) {
            answer->click();
        }
        const auto actions = takeActions(channels);
        std::vector<NodeAction> prompts;
        for (const auto& action : actions) {
            check(action.kind != NodeActionKind::ResolveInteraction, "async answers never send a blocking server response");
            if (action.kind == NodeActionKind::SubmitPrompt) {
                prompts.push_back(action);
            }
        }
        check(prompts.size() == (scenario == 0 ? 1U : 0U), "only explicit successful submission admits one prompt");
        if (!prompts.empty()) {
            check(prompts[0].target == thread && prompts[0].promptText == fixtureAnswer,
                  "answer targets the original thread and preserves question context");
            const auto transition = worker.admitPrompt(prompts[0]);
            check(transition.command.has_value(), "answer uses ordinary prompt admission while the agent is active");
            spin();
            revealQuestion();
            check(phase && phase->text() == QStringLiteral("Question · Sending…"),
                  "local admission updates the existing question header immediately");
            if (card) {
                card->grab().save(QString::fromStdString("/tmp/codex/build/CodexUI/async-answer-state/sending-" +
                                                         std::to_string(questions.size()) + "-" +
                                                         qEnvironmentVariable("QT_SCALE_FACTOR").toStdString() + ".png"));
            }
            if (transition.command) {
                const auto rejected = worker.completePrompt(transition.command->localPrompt, false, "Rejected for test");
                spin();
                revealQuestion();
                check(phase && phase->text() == QStringLiteral("Question"), "a rejected reply never claims to be sent");
                const auto retry = worker.admitPrompt(prompts[0]);
                check(retry.command.has_value(), "an explicit retry uses ordinary prompt admission");
                if (retry.command) {
                    const auto accepted = worker.completePrompt(retry.command->localPrompt, true, {}, "turn");
                    spin();
                    revealQuestion();
                    check(phase && phase->text() == QStringLiteral("Question · ✓ Answer sent"),
                          "server acknowledgement marks the original card as answered");
                    if (card) {
                        card->grab().save(QString::fromStdString("/tmp/codex/build/CodexUI/async-answer-state/answered-" +
                                                                 std::to_string(questions.size()) + "-" +
                                                                 qEnvironmentVariable("QT_SCALE_FACTOR").toStdString() + ".png"));
                    }

                    check(answer && answer->text() == QStringLiteral("Answer again…"), "an acknowledged question permits a correction");
                    notify(worker,
                           "item/completed",
                           {{"threadId", "questions"},
                            {"turnId", "turn"},
                            {"item",
                             {{"id", "answer"},
                              {"type", "userMessage"},
                              {"clientId", retry.command->clientUserMessageId},
                              {"content", nlohmann::json::array({{{"type", "text"}, {"text", fixtureAnswer}}})}}}});
                    spin();
                    revealQuestion();
                    check(phase && phase->text() == QStringLiteral("Question · ✓ Answer sent"),
                          "authoritative user-message reconciliation preserves answered status");
                    const auto echoFirst = worker.admitPrompt(prompts[0]);
                    if (echoFirst.command) {
                        spin();
                        revealQuestion();
                        check(phase && phase->text() == QStringLiteral("Question · Sending…"),
                              "an explicit repeat has its own pending delivery");
                        notify(worker,
                               "item/completed",
                               {{"threadId", "questions"},
                                {"turnId", "turn"},
                                {"item",
                                 {{"id", "echo-before-ack"},
                                  {"type", "userMessage"},
                                  {"clientId", echoFirst.command->clientUserMessageId},
                                  {"content", nlohmann::json::array({{{"type", "text"}, {"text", fixtureAnswer}}})}}}});
                        spin();
                        revealQuestion();
                        check(phase && phase->text() == QStringLiteral("Question · ✓ Answer sent"),
                              "authoritative echo confirms delivery before a delayed RPC result");
                        static_cast<void>(worker.completePrompt(echoFirst.command->localPrompt, false, "Late response"));
                        spin();
                        revealQuestion();
                        check(phase && phase->text() == QStringLiteral("Question · ✓ Answer sent"),
                              "confirmed history remains authoritative after a late RPC failure");
                    }
                }
            }
        }
        if (scenario == 0 && card) {
            auto plain = item;
            plain["questions"] = nlohmann::json::array();
            plain["phase"] = "final_answer";
            notify(worker, "item/completed", {{"threadId", "questions"}, {"turnId", "turn"}, {"item", plain}});
            spin();
            revealQuestion();
            check(phase && phase->text() == QStringLiteral("final answer") && (!answer || answer->isHidden()),
                  "clearing structured questions restores ordinary final-answer presentation");
            if (card) {
                check(card->grab().toImage().pixelColor(card->width() * card->devicePixelRatioF() / 2, 0) ==
                          QColor(codexui::UiStyle::finalAnswerBorder),
                      "question updates restore the ordinary violet border");
            }
        }
    }
} // namespace

int main(int argc, char** argv) {
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    auto* configuration = utils::Config::configRoot.newSubCommand<codexui::codex::Configuration>();
    QApplication application(argc, argv);
    application.setStyleSheet(codexui::UiStyle::applicationStyleSheet());
    core::SNodeC::init(argc, argv);
    std::ifstream input(CODEXUI_ASYNC_QUESTIONS_FIXTURE);
    nlohmann::json fixture;
    input >> fixture;
    corpus(fixture);
    replyCorpus(fixture);
    history(fixture);
    persistedReplies(fixture);
    duplicateReplyHistory(fixture);
    visibility(fixture);
    form(fixture["cases"][0]);
    form(fixture["cases"][2]);
    for (int scenario = 0; scenario < 4; ++scenario) {
        shell(*configuration, fixture["cases"][0]["questions"], fixture["cases"][0]["wire"], scenario);
    }
    shell(*configuration, fixture["cases"][2]["questions"], fixture["cases"][2]["wire"], 0);
    std::cout << failures << " async question failure(s)\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
