// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/InteractiveRequestDialog.h"

#include <ai/openai/codex/frontend/Codec.h>
#include <ai/openai/codex/frontend/GeneratedProtocol.h>
#include <ai/openai/codex/frontend/Messages.h>
#include <ai/openai/codex/frontend/client/Client.h>
#include <ai/openai/codex/frontend/client/State.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace frontend = ai::openai::codex::frontend;
namespace client = frontend::client;
namespace generated = frontend::generated;

namespace codexui {

struct InteractiveRequestDialogTestAccess {
    static void submit(InteractiveRequestDialog& dialog)
    {
        dialog.submitCurrent();
    }

    static QRadioButton* approvalChoice(InteractiveRequestDialog& dialog, std::size_t index)
    {
        return index < dialog.approvalChoices.size() ? dialog.approvalChoices[index] : nullptr;
    }

    static void setApprovalIndex(InteractiveRequestDialog& dialog, int index)
    {
        dialog.drafts[dialog.currentRequestId].approvalIndex = index;
    }

    static QLabel* addNestedLayout(InteractiveRequestDialog& dialog)
    {
        auto* nested = new QVBoxLayout;
        auto* label = new QLabel(QStringLiteral("nested fixture"));
        nested->addWidget(label);
        dialog.body->layout()->addItem(nested);
        return label;
    }

    static QLineEdit* freeTextEditor(InteractiveRequestDialog& dialog)
    {
        return dialog.questionEditors.empty() ? nullptr : dialog.questionEditors.front().freeText;
    }

    static QString draftFreeText(const InteractiveRequestDialog& dialog)
    {
        const auto draft = dialog.drafts.find(dialog.currentRequestId);
        if (draft == dialog.drafts.end())
            return {};
        const auto question = draft->second.questions.find("question-1");
        return question == draft->second.questions.end() ? QString{} : question->second.freeText;
    }
};

} // namespace codexui

namespace {

struct RequestFixture {
    std::string summary;
    std::string header;
    std::string prompt;
    std::string option;
    std::string description;
    std::string command;
    std::string cwd;
    bool requestTruncated = false;
    bool paramsTruncated = false;
    bool connectionInvalidated = false;
    bool itemTruncated = false;
    bool itemPresent = true;
    frontend::PendingRequestKind kind = frontend::PendingRequestKind::UserInput;
    bool requestScopePresent = true;
    bool duplicateItemId = false;
    bool allowsFreeText = false;
    bool secret = false;
};

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

frontend::CapabilityAdvertisement expandedCapabilities()
{
    std::vector<frontend::FrontendCapability> defined;
    for (const generated::CapabilityMetadata& capability : generated::AllCapabilities) {
        if (capability.defined)
            defined.push_back(static_cast<frontend::FrontendCapability>(capability.id));
    }
    const std::vector<frontend::FrontendCapability> selected{
        frontend::FrontendCapability::CompleteBackendDomains,
        frontend::FrontendCapability::DedicatedPendingRequests,
        frontend::FrontendCapability::DedicatedNotificationEvents,
        frontend::FrontendCapability::CompleteThreadItems,
        frontend::FrontendCapability::ScopeProjectedState,
    };
    return {std::move(defined), selected, selected, frontend::Json::object()};
}

client::State makeState(const RequestFixture& fixture)
{
    client::ClientOptions options;
    options.credentialProvider = [] {
        return client::AuthenticationContext{frontend::NoCredential{}, std::string{"interactive-request-test"}};
    };
    client::Client sdk(std::move(options));
    auto connection = sdk.openConnection({
        [](client::OutboundMessage) {
            return client::SendResult{client::SendStatus::Accepted, std::nullopt};
        },
        [](std::string) {},
    });
    connection.transportConnected();

    const frontend::Welcome welcome{
        "fixture-session",
        frontend::SessionRole::Observer,
        frontend::SequenceNumber{0},
        frontend::SyncMode::Snapshot,
        frontend::Json{{"permittedScopes", frontend::Json::array({"observe", "control"})},
                       {"projection", frontend::Json{{"identity", "interactive-request-test"}}}},
        expandedCapabilities(),
    };
    if (!connection.receive(frontend::ServerMessage{welcome}).accepted)
        return {};

    frontend::ExpandedPendingRequest request;
    request.pendingRequestId = "7";
    request.kind = fixture.kind;
    if (fixture.requestScopePresent) {
        request.threadId = "target-thread";
        request.turnId = "target-turn";
    }
    request.itemId = "item-1";
    request.summary = fixture.summary;
    request.details = frontend::Json{{"paramsTruncated", fixture.paramsTruncated}};
    if (fixture.kind == frontend::PendingRequestKind::UserInput) {
        request.questions = std::vector<frontend::ExpandedPendingRequestQuestion>{
            {"question-1",
             fixture.header,
             fixture.prompt,
             fixture.allowsFreeText,
             fixture.secret,
             {{fixture.option, fixture.description, frontend::Json::object()}},
             frontend::Json::object()},
        };
    }
    request.truncated = fixture.requestTruncated;
    if (fixture.connectionInvalidated)
        request.extensions["connectionInvalidated"] = true;

    frontend::ExpandedThreadItem item;
    item.id = "item-1";
    item.type = frontend::ThreadItemKind::CommandExecution;
    item.threadId = "target-thread";
    item.turnId = "target-turn";
    item.data = frontend::Json{{"command", fixture.command},
                               {"cwd", fixture.cwd},
                               {"status", "completed"},
                               {"processId", "42"},
                               {"exitCode", 0},
                               {"durationMs", 13}};
    item.truncated = fixture.itemTruncated;

    frontend::ExpandedBackendSnapshotState state;
    state.provider = frontend::Json{{"lifecycle", "ready"},
                                    {"generation", 1},
                                    {"desiredRunning", true},
                                    {"initialization",
                                     frontend::Json{{"codexHome", "/tmp/codex"},
                                                    {"platformFamily", "unix"},
                                                    {"platformOs", "linux"},
                                                    {"userAgent", "fixture"}}},
                                    {"lastError",
                                     frontend::Json{{"category", "none"}, {"code", 0}, {"detailsOmitted", false}}},
                                    {"recovery", frontend::Json{{"attempts", 0}, {"delayMs", 0}, {"status", "idle"}}}};
    state.controller = frontend::Json{{"present", false}, {"controllerSessionId", "1"}};
    state.threadList = frontend::Json{{"hasLoadedPage", true},
                                      {"complete", true},
                                      {"pagesLoaded", 1},
                                      {"stamp", frontend::Json{{"freshness", "current"}, {"generation", 1}}}};
    std::vector<frontend::ExpandedThreadItem> items;
    if (fixture.duplicateItemId) {
        frontend::ExpandedThreadItem wrongItem = item;
        wrongItem.threadId = "other-thread";
        wrongItem.turnId = "other-turn";
        wrongItem.data = frontend::Json{{"command", "wrong duplicate command"},
                                        {"cwd", "/wrong"},
                                        {"status", "completed"}};
        items.push_back(std::move(wrongItem));
    }
    if (fixture.itemPresent)
        items.push_back(std::move(item));
    if (!items.empty())
        state.items = std::move(items);
    state.pendingRequests = std::vector<frontend::ExpandedPendingRequest>{std::move(request)};
    state.capacity = frontend::Json{{"accumulatedContentBytes", 0},
                                    {"accumulatedProcessOutputBytes", 0},
                                    {"activeOperations", 0},
                                    {"droppedProcessOutputBytes", 0},
                                    {"evictedActivityRecords", 0},
                                    {"evictedFilesystemWatches", 0},
                                    {"evictedFuzzySearchSessions", 0},
                                    {"evictedNotices", 0},
                                    {"evictedProcesses", 0},
                                    {"observers", 0},
                                    {"pendingRequests", 1},
                                    {"retainedActivityRecords", 0},
                                    {"retainedFilesystemWatches", 0},
                                    {"retainedFuzzySearchSessions", 0},
                                    {"retainedItems", (fixture.itemPresent ? 1 : 0) + (fixture.duplicateItemId ? 1 : 0)},
                                    {"retainedNotices", 0},
                                    {"retainedProcesses", 0},
                                    {"retainedThreads", 0},
                                    {"retainedTurns", 0},
                                    {"sessions", 0}};
    state.truncation = frontend::Json{{"droppedBytes", 0},
                                      {"omittedEntries", 0},
                                      {"omittedFields", frontend::Json::array()},
                                      {"truncated", false}};

    const auto encoded = frontend::Codec::encodeExpandedSnapshot(
        frontend::ExpandedSnapshot{frontend::SequenceNumber{0}, std::move(state)});
    if (!encoded)
        return {};
    if (!connection
             .receive(frontend::ServerMessage{
                 frontend::Snapshot{frontend::SequenceNumber{0}, encoded.value().at("state")}})
             .accepted)
        return {};
    if (!connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber{0}}}).accepted)
        return {};
    return sdk.state();
}

void settleDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

QCheckBox* onlyCheckBox(codexui::InteractiveRequestDialog& dialog)
{
    const auto boxes = dialog.findChildren<QCheckBox*>();
    return boxes.size() == 1 ? boxes.front() : nullptr;
}

bool hasLabel(codexui::InteractiveRequestDialog& dialog, const QString& value)
{
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text() == value)
            return true;
    }
    return false;
}

bool testCanonicalRefreshAndPlainText()
{
    const RequestFixture first{"<b>old summary</b>",
                               "<i>old header</i>",
                               "<img src=x> old prompt",
                               "old & choice",
                               "<b>old description</b>",
                               "<b>old command</b>",
                               "/old/<b>cwd</b>"};
    const RequestFixture second{"<b>new summary</b>",
                                "<i>new header</i>",
                                "<img src=x> new prompt",
                                "new & choice",
                                "<b>new description</b>",
                                "<b>new command</b>",
                                "/new/<b>cwd</b>"};
    const client::State firstState = makeState(first);
    const client::State secondState = makeState(second);
    client::State currentState = firstState;
    int responses = 0;
    codexui::InteractiveRequestDialog dialog(
        [&currentState]() -> const client::State& { return currentState; },
        [&responses](codexui::InteractiveRequestResponse) { ++responses; });

    dialog.synchronize(currentState);
    QCheckBox* oldChoice = onlyCheckBox(dialog);
    bool passed = expect(oldChoice != nullptr, "the initial canonical request must create one choice");
    if (!oldChoice)
        return false;
    oldChoice->setChecked(true);

    for (QLabel* label : dialog.findChildren<QLabel*>())
        passed &= expect(label->textFormat() == Qt::PlainText, "every dynamic request label must force plain text");
    passed &= expect(hasLabel(dialog, QStringLiteral("<b>old summary</b>")),
                     "markup-looking request text must remain literal");
    passed &= expect(oldChoice->text() == QStringLiteral("old && choice"),
                     "option ampersands must be escaped from Qt mnemonic handling");
    passed &= expect(!oldChoice->toolTip().contains(QStringLiteral("<b>old description</b>")),
                     "dynamic tooltips must not retain raw rich-text markup");

    currentState = secondState;
    dialog.synchronize(currentState);
    settleDeferredDeletes();

    QCheckBox* newChoice = onlyCheckBox(dialog);
    passed &= expect(newChoice != nullptr, "the changed same-ID request must rebuild its choices");
    if (newChoice) {
        passed &= expect(newChoice->text() == QStringLiteral("new && choice"),
                         "the changed same-ID request must show the new option");
        passed &= expect(!newChoice->isChecked(), "a changed same-ID request must discard the stale draft");
    }
    passed &= expect(hasLabel(dialog, QStringLiteral("<b>new summary</b>")),
                     "the changed same-ID request must show its new summary");
    passed &= expect(hasLabel(dialog, QStringLiteral("Command: <b>new command</b>")),
                     "the changed linked item must refresh its command presentation");
    passed &= expect(!hasLabel(dialog, QStringLiteral("<b>old summary</b>")),
                     "the previous same-ID request presentation must be removed");
    passed &= expect(responses == 0, "refreshing canonical request data must not submit a response");

    const auto firstSource = codexui::detail::interactiveRequestSource(firstState, client::PendingRequestId{"7"});
    const client::State equalState = makeState(first);
    const auto equalSource = codexui::detail::interactiveRequestSource(equalState, client::PendingRequestId{"7"});
    passed &= expect(firstSource && equalSource && *firstSource == *equalSource,
                     "equivalent canonical request content must not differ only by source stamps");
    return passed;
}

bool testSubmitTimeRevalidation()
{
    const RequestFixture first{"summary", "header", "prompt", "old choice", "description", "command", "/cwd"};
    const RequestFixture second{"changed summary", "header", "prompt", "new choice", "description", "command", "/cwd"};
    client::State currentState = makeState(first);
    const client::State changedState = makeState(second);
    int responses = 0;
    codexui::InteractiveRequestDialog dialog(
        [&currentState]() -> const client::State& { return currentState; },
        [&responses](codexui::InteractiveRequestResponse) { ++responses; });
    dialog.synchronize(currentState);
    QCheckBox* choice = onlyCheckBox(dialog);
    bool passed = expect(choice != nullptr, "the request must expose its answer choice before submission");
    if (!choice)
        return false;
    choice->setChecked(true);
    auto* submit = dialog.findChild<QPushButton*>(QStringLiteral("interactiveRequestSubmit"));
    passed &= expect(submit && submit->isEnabled(), "a complete answered request must be submit-enabled");

    currentState = changedState;
    if (submit)
        submit->click();
    settleDeferredDeletes();
    passed &= expect(responses == 0, "submit must reject canonical same-ID changes made after presentation");
    passed &= expect(hasLabel(dialog, QStringLiteral("changed summary")),
                     "submit-time revalidation must rebuild the changed canonical request");
    return passed;
}

bool testCompositeApprovalLookup()
{
    RequestFixture fixture{"summary", "header", "prompt", "choice", "description", "target command", "/target"};
    fixture.kind = frontend::PendingRequestKind::CommandExecutionApproval;
    fixture.duplicateItemId = true;
    const client::State scopedState = makeState(fixture);
    const auto source = codexui::detail::interactiveRequestSource(scopedState, client::PendingRequestId{"7"});
    bool passed = expect(source && source->linkedItem.has_value(),
                         "a scoped approval must resolve its linked item despite duplicate bare IDs");
    if (source && source->linkedItem) {
        const auto* command = std::get_if<client::CommandExecutionSemanticView>(&source->linkedItem->details);
        passed &= expect(command && command->command == std::optional<std::string>{"target command"}
                             && command->cwd && command->cwd->value == "/target",
                         "approval lookup must use the request thread, turn, and item identity");
    }

    fixture.requestScopePresent = false;
    const auto unscoped = codexui::detail::interactiveRequestSource(makeState(fixture), client::PendingRequestId{"7"});
    passed &= expect(unscoped && !unscoped->linkedItem
                         && codexui::detail::interactiveRequestResponseSafety(*unscoped)
                                == codexui::InteractiveRequestResponseSafety::NegativeOnly,
                     "an approval with incomplete item identity must not resolve a bare-ID item");
    return passed;
}

bool testIncompleteUserInputIsDisabled()
{
    const RequestFixture truncated{"summary", "header", "prompt", "choice", "description", "command", "/cwd", true};
    const RequestFixture paramsTruncated{
        "summary", "header", "prompt", "choice", "description", "command", "/cwd", false, true};
    const RequestFixture invalidated{
        "summary", "header", "prompt", "choice", "description", "command", "/cwd", false, false, true};
    const RequestFixture linkedTruncated{
        "summary", "header", "prompt", "choice", "description", "command", "/cwd", false, false, false, true};
    RequestFixture linkedMissing{
        "summary", "header", "prompt", "choice", "description", "command", "/cwd"};
    linkedMissing.itemPresent = false;

    const auto truncatedSource = codexui::detail::interactiveRequestSource(makeState(truncated), client::PendingRequestId{"7"});
    const auto paramsSource = codexui::detail::interactiveRequestSource(makeState(paramsTruncated), client::PendingRequestId{"7"});
    const auto invalidatedSource = codexui::detail::interactiveRequestSource(makeState(invalidated), client::PendingRequestId{"7"});
    const auto linkedSource = codexui::detail::interactiveRequestSource(makeState(linkedTruncated), client::PendingRequestId{"7"});
    const auto missingSource = codexui::detail::interactiveRequestSource(makeState(linkedMissing), client::PendingRequestId{"7"});
    bool passed = true;
    passed &= expect(truncatedSource
                         && codexui::detail::interactiveRequestResponseSafety(*truncatedSource)
                                == codexui::InteractiveRequestResponseSafety::Disabled,
                     "a truncated request must be non-actionable");
    passed &= expect(paramsSource
                         && codexui::detail::interactiveRequestResponseSafety(*paramsSource)
                                == codexui::InteractiveRequestResponseSafety::Disabled,
                     "truncated typed request parameters must be non-actionable");
    passed &= expect(invalidatedSource
                         && codexui::detail::interactiveRequestResponseSafety(*invalidatedSource)
                                == codexui::InteractiveRequestResponseSafety::Disabled,
                     "a connection-invalidated request must be non-actionable");
    passed &= expect(linkedSource
                         && codexui::detail::interactiveRequestResponseSafety(*linkedSource)
                                == codexui::InteractiveRequestResponseSafety::Disabled,
                     "a request with truncated linked semantics must be non-actionable");
    passed &= expect(missingSource
                         && codexui::detail::interactiveRequestResponseSafety(*missingSource)
                                == codexui::InteractiveRequestResponseSafety::Disabled,
                     "a request with unavailable linked semantics must be non-actionable");
    if (truncatedSource) {
        codexui::InteractiveRequestSource omitted = *truncatedSource;
        omitted.request.truncated = false;
        omitted.request.omittedFields = {"/questions"};
        passed &= expect(codexui::detail::interactiveRequestResponseSafety(omitted)
                             == codexui::InteractiveRequestResponseSafety::Disabled,
                         "a request with omitted semantic fields must be non-actionable");
    }

    client::State currentState = makeState(invalidated);
    int responses = 0;
    codexui::InteractiveRequestDialog dialog(
        [&currentState]() -> const client::State& { return currentState; },
        [&responses](codexui::InteractiveRequestResponse) { ++responses; });
    dialog.synchronize(currentState);
    auto* submit = dialog.findChild<QPushButton*>(QStringLiteral("interactiveRequestSubmit"));
    passed &= expect(submit && !submit->isEnabled(), "an incomplete request must disable submission");
    codexui::InteractiveRequestDialogTestAccess::submit(dialog);
    passed &= expect(responses == 0, "an incomplete request must also fail closed when submission is forced");
    return passed;
}

bool testIncompleteApprovalAllowsOnlyNegativeResponses()
{
    RequestFixture incomplete{"summary", "header", "prompt", "choice", "description", "command", "/cwd"};
    incomplete.kind = frontend::PendingRequestKind::CommandExecutionApproval;
    incomplete.itemTruncated = true;
    client::State currentState = makeState(incomplete);
    std::optional<codexui::InteractiveRequestResponse> response;
    codexui::InteractiveRequestDialog dialog(
        [&currentState]() -> const client::State& { return currentState; },
        [&response](codexui::InteractiveRequestResponse value) { response = std::move(value); });
    dialog.synchronize(currentState);

    auto* approve = codexui::InteractiveRequestDialogTestAccess::approvalChoice(dialog, 0);
    auto* approveForSession = codexui::InteractiveRequestDialogTestAccess::approvalChoice(dialog, 1);
    auto* decline = codexui::InteractiveRequestDialogTestAccess::approvalChoice(dialog, 2);
    auto* cancel = codexui::InteractiveRequestDialogTestAccess::approvalChoice(dialog, 3);
    auto* submit = dialog.findChild<QPushButton*>(QStringLiteral("interactiveRequestSubmit"));
    bool passed = expect(approve && approveForSession && decline && cancel && submit,
                         "an approval request must expose all four typed decisions");
    if (!approve || !approveForSession || !decline || !cancel || !submit)
        return false;
    passed &= expect(!approve->isEnabled() && !approveForSession->isEnabled()
                         && decline->isEnabled() && cancel->isEnabled(),
                     "incomplete approval semantics must disable positive decisions only");
    decline->setChecked(true);
    passed &= expect(submit->isEnabled(), "a decline must remain submit-enabled for an incomplete approval");
    codexui::InteractiveRequestDialogTestAccess::submit(dialog);
    passed &= expect(response && codexui::detail::interactiveResponseIsNegative(*response),
                     "an incomplete approval must submit its safe negative response");

    response.reset();
    codexui::InteractiveRequestDialog forcedDialog(
        [&currentState]() -> const client::State& { return currentState; },
        [&response](codexui::InteractiveRequestResponse value) { response = std::move(value); });
    forcedDialog.synchronize(currentState);
    codexui::InteractiveRequestDialogTestAccess::setApprovalIndex(forcedDialog, 1);
    codexui::InteractiveRequestDialogTestAccess::submit(forcedDialog);
    passed &= expect(!response, "a forced positive response must still fail closed for incomplete approval semantics");

    incomplete.connectionInvalidated = true;
    currentState = makeState(incomplete);
    codexui::InteractiveRequestDialog invalidatedDialog(
        [&currentState]() -> const client::State& { return currentState; },
        [&response](codexui::InteractiveRequestResponse value) { response = std::move(value); });
    invalidatedDialog.synchronize(currentState);
    decline = codexui::InteractiveRequestDialogTestAccess::approvalChoice(invalidatedDialog, 2);
    passed &= expect(decline && !decline->isEnabled(),
                     "a connection-invalidated approval must disable negative decisions too");
    codexui::InteractiveRequestDialogTestAccess::setApprovalIndex(invalidatedDialog, 3);
    codexui::InteractiveRequestDialogTestAccess::submit(invalidatedDialog);
    passed &= expect(!response, "a connection-invalidated approval must reject a forced negative response");
    return passed;
}

bool testNestedLayoutRebuildDeletesOnce()
{
    const RequestFixture first{"first", "header", "prompt", "choice", "description", "command", "/cwd"};
    const RequestFixture second{"second", "header", "prompt", "choice", "description", "command", "/cwd"};
    client::State currentState = makeState(first);
    codexui::InteractiveRequestDialog dialog(
        [&currentState]() -> const client::State& { return currentState; },
        [](codexui::InteractiveRequestResponse) {});
    dialog.synchronize(currentState);
    QPointer<QLabel> nested = codexui::InteractiveRequestDialogTestAccess::addNestedLayout(dialog);
    currentState = makeState(second);
    dialog.synchronize(currentState);
    settleDeferredDeletes();
    return expect(nested.isNull(), "rebuilding must clear a directly nested layout without a double delete");
}

bool testSecretFreeTextIsNotDraftedAndClearsAfterCapture()
{
    RequestFixture secretFixture{"summary", "header", "prompt", {}, {}, "command", "/cwd"};
    secretFixture.allowsFreeText = true;
    secretFixture.secret = true;
    client::State currentState = makeState(secretFixture);
    std::optional<codexui::InteractiveRequestResponse> response;
    codexui::InteractiveRequestDialog dialog(
        [&currentState]() -> const client::State& { return currentState; },
        [&response](codexui::InteractiveRequestResponse value) { response = std::move(value); });
    dialog.synchronize(currentState);

    QLineEdit* editor = codexui::InteractiveRequestDialogTestAccess::freeTextEditor(dialog);
    bool passed = expect(editor && editor->echoMode() == QLineEdit::Password,
                         "a secret question must use a password editor");
    if (!editor)
        return false;
    editor->setText(QStringLiteral("first secret"));
    dialog.synchronize(currentState);
    passed &= expect(editor->text() == QStringLiteral("first secret"),
                     "an unchanged state refresh must not erase the live secret editor");
    passed &= expect(codexui::InteractiveRequestDialogTestAccess::draftFreeText(dialog).isEmpty(),
                     "secret free text must never be copied into the persistent request draft");

    QPointer<QLineEdit> previousEditor = editor;
    dialog.present();
    editor = codexui::InteractiveRequestDialogTestAccess::freeTextEditor(dialog);
    passed &= expect(previousEditor && previousEditor->text().isEmpty(),
                     "rebuilding must clear the previous secret editor before disposal");
    passed &= expect(editor && editor->text().isEmpty(),
                     "a rebuilt secret editor must not restore secret free text from a draft");
    if (!editor)
        return false;

    editor->setText(QStringLiteral("submitted secret"));
    codexui::InteractiveRequestDialogTestAccess::submit(dialog);
    passed &= expect(editor->text().isEmpty(),
                     "successful response capture must immediately clear the live secret editor");
    passed &= expect(codexui::InteractiveRequestDialogTestAccess::draftFreeText(dialog).isEmpty(),
                     "successful response capture must leave no secret draft text");
    const auto* answers = response
                              ? std::get_if<std::vector<ai::openai::codex::typed::UserInputAnswer>>(&response->value)
                              : nullptr;
    passed &= expect(answers && answers->size() == 1 && answers->front().answers.size() == 1
                         && answers->front().answers.front() == "submitted secret",
                     "the typed response must still receive the captured secret answer");

    RequestFixture ordinaryFixture = secretFixture;
    ordinaryFixture.secret = false;
    currentState = makeState(ordinaryFixture);
    codexui::InteractiveRequestDialog ordinaryDialog(
        [&currentState]() -> const client::State& { return currentState; },
        [](codexui::InteractiveRequestResponse) {});
    ordinaryDialog.synchronize(currentState);
    editor = codexui::InteractiveRequestDialogTestAccess::freeTextEditor(ordinaryDialog);
    if (!expect(editor != nullptr, "a non-secret free-text question must create an editor"))
        return false;
    editor->setText(QStringLiteral("ordinary answer"));
    ordinaryDialog.present();
    editor = codexui::InteractiveRequestDialogTestAccess::freeTextEditor(ordinaryDialog);
    passed &= expect(editor && editor->text() == QStringLiteral("ordinary answer"),
                     "non-secret free text must retain the existing draft behavior");
    return passed;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    bool passed = true;
    passed &= testCanonicalRefreshAndPlainText();
    passed &= testSubmitTimeRevalidation();
    passed &= testCompositeApprovalLookup();
    passed &= testIncompleteUserInputIsDisabled();
    passed &= testIncompleteApprovalAllowsOnlyNegativeResponses();
    passed &= testNestedLayoutRebuildDeletesOnce();
    passed &= testSecretFreeTextIsNotDraftedAndClearsAfterCapture();
    return passed ? 0 : 1;
}
