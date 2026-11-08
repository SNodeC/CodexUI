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
#include "codex/middle/ConversationView.h"
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
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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
                check(PendingRequestPolicy::asyncQuestionAnswer(request, submission) == entry["text"].get<std::string>(),
                      entry["id"].get<std::string>() + " preserves question context and chosen answers");
                check(!PendingRequestPolicy::asyncQuestionAnswer(request, {"decline", entry["answer"]}),
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

    void form(const nlohmann::json& questions) {
        PendingRequestDescriptor request;
        request.kind = PendingRequestKind::UserInput;
        request.method = "agentMessage";
        request.raw = PendingRequestPolicy::asyncQuestionRequest(questions);
        request.availability = PendingRequestAvailability::Actionable;
        bool held = false;
        int attempts = 0;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog != nullptr, "question form is modal");
            if (dialog) {
                const auto choices = dialog->findChildren<QRadioButton*>();
                check(choices.size() == 3 && choices[0]->isChecked(),
                      "first option is preselected with an exclusive free-text alternative");
                if (choices.size() == 3) {
                    choices[1]->click();
                }
                dialog->grab().save(QString::fromStdString("/tmp/codex/build/CodexUI/async-question-dialog/question-form-" +
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
            check(PendingRequestPolicy::asyncQuestionAnswer(request.raw, submission) ==
                      std::optional<std::string>("Approve the follow-up PR?\nHold"),
                  "dialog submits the selected answer, not the default");
            return attempts == 2;
        });
        check(result.has_value() && held && attempts == 2, "failed admission retains the form and selected answer for deliberate retry");
    }

    void shell(Configuration& configuration, const nlohmann::json& questions, int scenario) {
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
                shell.grab().save(QString::fromStdString("/tmp/codex/build/CodexUI/async-question-dialog/question-card-" +
                                                         qEnvironmentVariable("QT_SCALE_FACTOR").toStdString() + ".png"));
            }
        }
        static_cast<void>(takeActions(channels));
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (scenario == 1) {
                    buttons->button(QDialogButtonBox::Cancel)->click();
                } else {
                    const auto choices = dialog->findChildren<QRadioButton*>();
                    if (choices.size() >= 2) {
                        choices[1]->click();
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
            check(prompts[0].target == thread && prompts[0].promptText == "Approve the follow-up PR?\nHold",
                  "answer targets the original thread and preserves question context");
            const auto transition = worker.admitPrompt(prompts[0]);
            check(transition.command.has_value(), "answer uses ordinary prompt admission while the agent is active");
        }
        if (scenario == 0 && card) {
            auto plain = item;
            plain["questions"] = nlohmann::json::array();
            plain["phase"] = "final_answer";
            notify(worker, "item/completed", {{"threadId", "questions"}, {"turnId", "turn"}, {"item", plain}});
            spin();
            check(phase && phase->text() == QStringLiteral("final answer") && answer->isHidden(),
                  "clearing structured questions restores ordinary final-answer presentation");
            check(card->grab().toImage().pixelColor(card->width() * card->devicePixelRatioF() / 2, 0) ==
                      QColor(codexui::UiStyle::finalAnswerBorder),
                  "question updates restore the ordinary violet border");
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
    history(fixture);
    visibility(fixture);
    form(fixture["cases"][0]["questions"]);
    for (int scenario = 0; scenario < 4; ++scenario) {
        shell(*configuration, fixture["cases"][0]["questions"], scenario);
    }
    std::cout << failures << " async question failure(s)\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
