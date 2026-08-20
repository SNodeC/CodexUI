// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/SidebarWidget.h"
#include "ui/ThreadSetupDialog.h"
#include "ui/UpcomingTurnDock.h"
#include "ui/UiStyle.h"

#include <ai/openai/codex/frontend/client/State.h>
#include <ai/openai/codex/frontend/client/StateTypes.h>
#include <ai/openai/codex/typed/Conversation.h>
#include <ai/openai/codex/typed/Types.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QStyle>
#include <QStyleOptionComboBox>

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace
{

namespace sdk = ai::openai::codex::frontend::client;
namespace typed = ai::openai::codex::typed;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

void settleEvents(int passes = 4, int delayMs = 20)
{
    for (int pass = 0; pass < passes; ++pass)
    {
        QEventLoop loop;
        QTimer::singleShot(delayMs, &loop, &QEventLoop::quit);
        loop.exec();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

sdk::ExecutionConfiguration configuration(std::string model,
                                          typed::ReasoningEffort effort,
                                          std::string cwd)
{
    sdk::ExecutionConfiguration result;
    result.approvalPolicy = typed::ApprovalPolicy::onRequest();
    result.approvalsReviewer = typed::ApprovalsReviewer::user();
    result.collaborationMode.mode = typed::ModeKind::defaultMode();
    result.collaborationMode.settings.model = typed::ModelId{model};
    result.cwd = typed::AbsolutePath{std::move(cwd)};
    result.effort = std::move(effort);
    result.model = typed::ModelId{std::move(model)};
    result.modelProvider = "openai";
    result.personality = typed::Personality::friendly();
    result.sandboxPolicy = typed::WorkspaceWriteSandboxPolicy{};
    result.serviceTier = std::string{"default"};
    result.summary = typed::ReasoningSummary::automatic();
    return result;
}

bool testUpcomingTurnCanonicalRebase()
{
    codexui::UpcomingTurnDock dock;
    dock.resize(960, dock.baseHeight());
    dock.show();
    dock.setActionState(true, false, true, false);

    const sdk::ExecutionConfiguration first =
        configuration("gpt-5.6", typed::ReasoningEffort::high(), "/workspace/first");
    dock.setCanonicalConfiguration(first, QStringLiteral("thread-a"));
    settleEvents();

    auto* model = dock.findChild<QComboBox*>(QStringLiteral("upcomingModel"));
    auto* effort = dock.findChild<QComboBox*>(QStringLiteral("upcomingReasoning"));
    auto* cwd = dock.findChild<QLineEdit*>(QStringLiteral("upcomingWorkspace"));
    auto* settings = dock.findChild<QFrame*>(QStringLiteral("upcomingTurnSettings"));
    auto* composer = dock.findChild<QFrame*>(QStringLiteral("upcomingComposer"));
    bool passed = expect(model && effort && cwd,
                         "the upcoming-turn canonical controls must be discoverable");
    if (!model || !effort || !cwd || !settings || !composer)
        return false;
    const int stableBaseHeight = dock.height();
    const int stableComposerTop = composer->geometry().top();

    passed &= expect(model->currentText() == QStringLiteral("gpt-5.6")
                         && effort->currentData().toString() == QStringLiteral("high")
                         && cwd->text() == QStringLiteral("/workspace/first"),
                     "untouched upcoming-turn controls must show canonical thread settings");
    passed &= expect(dock.draft().threadIdentity == QStringLiteral("thread-a")
                         && dock.draft().empty() && !dock.hasSettingsChanges(),
                     "canonical values must not become local turn overrides");

    const int xhighIndex = effort->findData(QStringLiteral("xhigh"));
    effort->setCurrentIndex(xhighIndex);
    QCoreApplication::processEvents();
    const codexui::UpcomingTurnDraft changed = dock.draft();
    passed &= expect(xhighIndex >= 0 && dock.hasSettingsChanges()
                         && changed.effort.hasValue()
                         && changed.effort->value == "xhigh" && changed.model.isOmitted()
                         && changed.cwd.isOmitted(),
                     "only an explicitly changed control must enter the typed turn draft");
    auto* changedSurface = effort->parentWidget();
    auto* settingsHint = dock.findChild<QLabel*>(QStringLiteral("upcomingSettingsHint"));
    passed &= expect(changedSurface && changedSurface->property("changed").toBool()
                         && settingsHint && settingsHint->isVisible()
                         && !settingsHint->text().isEmpty()
                         && dock.height() == stableBaseHeight
                         && composer->geometry().top() == stableComposerTop
                         && settings->geometry().bottom() < composer->geometry().top(),
                     "changed upcoming-turn values must have a visible persistent-setting indication");

    const sdk::ExecutionConfiguration refreshed =
        configuration("gpt-5.7", typed::ReasoningEffort::low(), "/workspace/refreshed");
    dock.setCanonicalConfiguration(refreshed, QStringLiteral("thread-a"));
    settleEvents();
    passed &= expect(model->currentText() == QStringLiteral("gpt-5.7")
                         && cwd->text() == QStringLiteral("/workspace/refreshed")
                         && effort->currentData().toString() == QStringLiteral("xhigh"),
                     "same-thread refreshes must rebase untouched controls without overwriting a user change");
    passed &= expect(dock.draft().effort.hasValue()
                         && dock.draft().effort->value == "xhigh",
                     "a same-thread state update must preserve the pending typed override");

    dock.setCanonicalConfiguration(refreshed, QStringLiteral("thread-b"));
    settleEvents();
    passed &= expect(effort->currentData().toString() == QStringLiteral("low")
                         && dock.draft().threadIdentity == QStringLiteral("thread-b")
                         && dock.draft().empty() && !dock.hasSettingsChanges(),
                     "switching threads must discard the prior thread's draft and rebase every control");

    const int defaultIndex = effort->findData(QStringLiteral("default"));
    effort->setCurrentIndex(defaultIndex);
    const codexui::UpcomingTurnDraft normalizedBeforeCompletion = dock.draft();
    sdk::ExecutionConfiguration normalized = refreshed;
    normalized.effort = typed::ReasoningEffort::medium();
    dock.setCanonicalConfiguration(normalized, QStringLiteral("thread-b"));
    passed &= expect(effort->currentData().toString() == QStringLiteral("default")
                         && dock.hasSettingsChanges(),
                     "a canonical update must not overwrite a submitted control before operation completion");
    dock.acknowledgeSubmittedSettings(normalizedBeforeCompletion);
    passed &= expect(effort->currentData().toString() == QStringLiteral("medium")
                         && dock.draft().empty() && !dock.hasSettingsChanges(),
                     "a newer authoritative revision must resolve an explicit-null setting even when normalized");

    effort->setCurrentIndex(defaultIndex);
    const codexui::UpcomingTurnDraft normalizedAfterCompletion = dock.draft();
    dock.acknowledgeSubmittedSettings(normalizedAfterCompletion);
    normalized.effort = typed::ReasoningEffort::high();
    dock.setCanonicalConfiguration(normalized, QStringLiteral("thread-b"));
    passed &= expect(effort->currentData().toString() == QStringLiteral("high")
                         && dock.draft().empty() && !dock.hasSettingsChanges(),
                     "the first newer authoritative revision must resolve a submitted reset without stale intent");

    effort->setCurrentIndex(xhighIndex);
    const codexui::UpcomingTurnDraft submittedXhigh = dock.draft();
    effort->setCurrentIndex(effort->findData(QStringLiteral("low")));
    dock.acknowledgeSubmittedSettings(submittedXhigh);
    passed &= expect(effort->currentData().toString() == QStringLiteral("low")
                         && dock.hasSettingsChanges() && dock.draft().effort.hasValue()
                         && dock.draft().effort->value == "low",
                     "submission acknowledgement must preserve an edit made after the submitted draft");

    const codexui::UpcomingTurnDraft staleThreadB = dock.draft();
    const sdk::ExecutionConfiguration thirdThread =
        configuration("gpt-5.8", typed::ReasoningEffort::medium(), "/workspace/third");
    dock.setCanonicalConfiguration(thirdThread, QStringLiteral("thread-c"));
    effort->setCurrentIndex(xhighIndex);
    const codexui::UpcomingTurnDraft threadCBeforeStaleAcknowledgement = dock.draft();
    dock.acknowledgeSubmittedSettings(staleThreadB);
    passed &= expect(dock.draft().threadIdentity == QStringLiteral("thread-c")
                         && effort->currentData().toString() == QStringLiteral("xhigh")
                         && dock.hasSettingsChanges() && dock.draft().effort.hasValue()
                         && dock.draft().effort->value == "xhigh"
                         && dock.draft().presentationKeys
                                == threadCBeforeStaleAcknowledgement.presentationKeys,
                     "a late acknowledgement from another thread must not mutate the current thread's draft");
    return passed;
}

bool testAnchoredGrowingComposer()
{
    codexui::AnchoredTurnSurface surface;
    auto* conversation = new QWidget;
    conversation->setObjectName(QStringLiteral("phase1TestConversation"));
    auto* dock = new codexui::UpcomingTurnDock;
    surface.setConversationWidget(conversation);
    surface.setUpcomingTurnDock(dock);
    surface.resize(960, 760);
    surface.show();
    dock->setActionState(true, false, true, false);
    settleEvents();

    auto* editor = dock->findChild<QPlainTextEdit*>(QStringLiteral("upcomingPromptEditor"));
    bool passed = expect(editor != nullptr,
                         "the anchored upcoming-turn surface must expose its prompt editor");
    if (!editor)
        return false;

    const QRect conversationBaseline = conversation->geometry();
    const int editorBaselineHeight = editor->height();
    const int dockBaselineHeight = dock->height();
    passed &= expect(editor->document()->blockCount() == 1 && editorBaselineHeight <= 48,
                     "the empty prompt editor must begin as one visible line");
    passed &= expect(conversationBaseline.height() == surface.height() - dock->baseHeight(),
                     "the conversation viewport must reserve only the fixed dock base height");
    passed &= expect(dock->geometry().bottom() + 1 == surface.height(),
                     "the upcoming-turn dock must remain anchored to the surface bottom edge");

    QString longPrompt;
    for (int line = 0; line < 80; ++line)
        longPrompt += QStringLiteral("A deliberately long prompt line used to exercise upward growth.\n");
    editor->setPlainText(longPrompt);
    settleEvents(6);

    const QRect conversationWhileExpanded = conversation->geometry();
    passed &= expect(editor->height() > editorBaselineHeight && editor->height() <= 236
                         && dock->height() > dockBaselineHeight,
                     "multiline prompt input must grow upward until its bounded maximum height");
    passed &= expect(conversationWhileExpanded == conversationBaseline,
                     "prompt expansion must not resize or move the conversation viewport");
    passed &= expect(dock->geometry().bottom() + 1 == surface.height()
                         && dock->geometry().top() < surface.height() - dockBaselineHeight,
                     "an expanded dock must keep its base fixed and move only its top edge upward");
    const bool internallyScrollable = editor->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded
        && (editor->verticalScrollBar()->maximum() > 0
            || editor->document()->size().height() > editor->viewport()->height());
    if (!internallyScrollable) {
        std::cerr << "scroll diagnostics: policy=" << editor->verticalScrollBarPolicy()
                  << " maximum=" << editor->verticalScrollBar()->maximum()
                  << " documentHeight=" << editor->document()->size().height()
                  << " viewportHeight=" << editor->viewport()->height()
                  << " blocks=" << editor->document()->blockCount()
                  << " editorHeight=" << editor->height() << '\n';
    }
    passed &= expect(internallyScrollable,
                     "after maximum growth the prompt editor must scroll internally");

    editor->setPlainText(QStringLiteral("short prompt"));
    settleEvents(6);
    passed &= expect(editor->height() <= editorBaselineHeight + 8
                         && dock->height() <= dockBaselineHeight + 8,
                     "shrinking prompt text must return the editor and dock to their compact baseline");
    passed &= expect(conversation->geometry() == conversationBaseline
                         && dock->geometry().bottom() + 1 == surface.height(),
                     "prompt shrinkage must not jump the conversation or unanchor the dock");

    surface.resize(1000, 760);
    editor->setPlainText(QString(320, QLatin1Char('w')));
    settleEvents(6);
    const int wideWrappedHeight = editor->height();
    surface.resize(520, 760);
    settleEvents(6);
    const int narrowWrappedHeight = editor->height();
    passed &= expect(wideWrappedHeight > editorBaselineHeight
                         && narrowWrappedHeight > wideWrappedHeight
                         && conversation->height() == surface.height() - dock->baseHeight()
                         && dock->geometry().bottom() + 1 == surface.height(),
                     "a wrapped prompt must reflow upward after a width change without resizing the conversation height");
    surface.resize(1000, 760);
    settleEvents(6);
    passed &= expect(editor->height() < narrowWrappedHeight
                         && conversation->height() == surface.height() - dock->baseHeight()
                         && dock->geometry().bottom() + 1 == surface.height(),
                     "widening a wrapped prompt must shrink it without moving the dock base");
    return passed;
}

bool testNarrowUpcomingTurnLayout()
{
    codexui::UpcomingTurnDock dock;
    dock.setCanonicalConfiguration(
        configuration("gpt-test", typed::ReasoningEffort::high(), "/workspace"),
        QStringLiteral("thread-narrow"));
    dock.setActionState(true, false, true, false);
    dock.resize(520, dock.baseHeight());
    dock.show();
    settleEvents();

    auto* settings = dock.findChild<QFrame*>(QStringLiteral("upcomingTurnSettings"));
    auto* composer = dock.findChild<QFrame*>(QStringLiteral("upcomingComposer"));
    auto* model = dock.findChild<QComboBox*>(QStringLiteral("upcomingModel"));
    auto* effort = dock.findChild<QComboBox*>(QStringLiteral("upcomingReasoning"));
    auto* style = dock.findChild<QComboBox*>(QStringLiteral("upcomingStyle"));
    auto* access = dock.findChild<QComboBox*>(QStringLiteral("upcomingAccess"));
    auto* workspace = dock.findChild<QLineEdit*>(QStringLiteral("upcomingWorkspace"));
    auto* more = dock.findChild<QPushButton*>(QStringLiteral("upcomingMore"));
    auto* status = dock.findChild<QLabel*>(QStringLiteral("upcomingTurnStatus"));
    auto* send = dock.findChild<QPushButton*>(QStringLiteral("upcomingSendButton"));
    bool passed = expect(settings && composer && model && effort && style && access
                             && workspace && more && status && send,
                         "the narrow upcoming-turn layout controls must be discoverable");
    if (!settings || !composer || !model || !effort || !style || !access
        || !workspace || !more || !status || !send)
        return false;

    const auto inDock = [&dock](QWidget* widget) {
        return QRect(widget->mapTo(&dock, QPoint(0, 0)), widget->size());
    };
    const QRect modelRect = inDock(model);
    const QRect effortRect = inDock(effort);
    const QRect styleRect = inDock(style);
    const QRect accessRect = inDock(access);
    const QRect workspaceRect = inDock(workspace);
    const QRect moreRect = inDock(more);
    const QRect statusRect = inDock(status);
    const QRect sendRect = inDock(send);
    passed &= expect(settings->geometry().bottom() < composer->geometry().top()
                         && modelRect.bottom() < accessRect.top()
                         && effortRect.right() < styleRect.left()
                         && workspaceRect.right() < moreRect.left()
                         && statusRect.right() < sendRect.left(),
                     "the two-row settings and composer actions must not overlap at narrow width");
    passed &= expect(modelRect.width() > effortRect.width()
                         && effortRect.width() >= 70 && styleRect.width() >= 70
                         && accessRect.width() >= 70 && workspaceRect.width() >= 70,
                     "narrow settings must retain readable choice widths with extra space for the model");
    return passed;
}

bool testTypedModelCatalog()
{
    codexui::UpcomingTurnDock dock;
    dock.setActionState(true, false, true, false);
    dock.setCanonicalConfiguration(
        configuration("retired-model", typed::ReasoningEffort::high(), "/workspace"),
        QStringLiteral("thread-models"));

    typed::Model alpha;
    alpha.id = typed::ModelId{"preset-alpha"};
    alpha.model = typed::ModelId{"model-alpha"};
    alpha.displayName = "Alpha model";
    alpha.isDefault = true;
    alpha.defaultReasoningEffort = typed::ReasoningEffort{"ultra"};
    alpha.supportedReasoningEfforts = {
        typed::ReasoningEffortOption{"Maximum analysis", typed::ReasoningEffort{"ultra"}}};
    alpha.supportsPersonality = false;
    typed::Model beta;
    beta.id = typed::ModelId{"preset-beta"};
    beta.model = typed::ModelId{"model-beta"};
    beta.displayName = "Beta model";
    beta.defaultReasoningEffort = typed::ReasoningEffort::low();
    beta.supportedReasoningEfforts = {
        typed::ReasoningEffortOption{"Fast analysis", typed::ReasoningEffort::low()}};
    beta.supportsPersonality = true;
    dock.setModelCatalog({alpha, beta});

    auto* model = dock.findChild<QComboBox*>(QStringLiteral("upcomingModel"));
    auto* effort = dock.findChild<QComboBox*>(QStringLiteral("upcomingReasoning"));
    auto* personality = dock.findChild<QComboBox*>(QStringLiteral("upcomingStyle"));
    bool passed = expect(model && effort && personality,
                         "the typed model catalogue must populate model-dependent controls");
    if (!model || !effort || !personality)
        return false;
    passed &= expect(model->findData(QStringLiteral("model-alpha")) >= 0
                         && model->findData(QStringLiteral("model-beta")) >= 0
                         && model->findData(QStringLiteral("preset-alpha")) < 0
                         && model->currentData().toString() == QStringLiteral("retired-model"),
                     "model choices must submit the model slug and retain a canonical model absent from the catalogue");

    codexui::UpcomingTurnDock defaultDock;
    defaultDock.setCanonicalConfiguration(std::nullopt, QStringLiteral("new-thread"), true);
    defaultDock.setModelCatalog({alpha, beta});
    auto* defaultModel = defaultDock.findChild<QComboBox*>(QStringLiteral("upcomingModel"));
    auto* defaultEffort = defaultDock.findChild<QComboBox*>(QStringLiteral("upcomingReasoning"));
    passed &= expect(defaultModel && defaultEffort
                         && defaultModel->currentData().toString() == QStringLiteral("model-alpha")
                         && defaultModel->currentText() == QStringLiteral("Alpha model")
                         && defaultModel->toolTip() == QStringLiteral("Codex default model")
                         && defaultEffort->currentData().toString() == QStringLiteral("default")
                         && defaultEffort->currentText().contains(QStringLiteral("Ultra"))
                         && defaultDock.draft().empty(),
                     "a new thread must visibly preselect the advertised model and its Codex reasoning default without submitting overrides");

    dock.setCanonicalConfiguration(
        configuration("model-alpha", typed::ReasoningEffort::high(), "/workspace"),
        QStringLiteral("thread-models"));
    passed &= expect(model->currentText() == QStringLiteral("Alpha model")
                         && model->currentData().toString() == QStringLiteral("model-alpha")
                         && effort->findData(QStringLiteral("ultra")) >= 0
                         && !personality->isEnabled(),
                     "the selected typed model must expose its advertised effort and personality capability");

    model->setCurrentIndex(model->findData(QStringLiteral("model-beta")));
    beta.displayName = "Beta model updated";
    dock.setModelCatalog({alpha, beta});
    const codexui::UpcomingTurnDraft draft = dock.draft();
    passed &= expect(model->currentText() == QStringLiteral("Beta model updated")
                         && draft.model.hasValue()
                         && draft.model->value == "model-beta"
                         && effort->currentData().toString() == QStringLiteral("low")
                         && draft.effort.hasValue() && draft.effort->value == "low"
                         && personality->isEnabled(),
                     "a model change must preserve its slug and choose an advertised compatible effort");
    return passed;
}

bool testUpcomingTurnActionStates()
{
    codexui::UpcomingTurnDock dock;
    dock.setCanonicalConfiguration(
        configuration("gpt-test", typed::ReasoningEffort::high(), "/workspace"),
        QStringLiteral("thread-actions"));
    auto* editor = dock.findChild<QPlainTextEdit*>(QStringLiteral("upcomingPromptEditor"));
    auto* send = dock.findChild<QPushButton*>(QStringLiteral("upcomingSendButton"));
    auto* stop = dock.findChild<QPushButton*>(QStringLiteral("upcomingStopButton"));
    auto* sandbox = dock.findChild<QComboBox*>(QStringLiteral("upcomingAccess"));
    auto* approval = dock.findChild<QComboBox*>(QStringLiteral("upcomingApproval"));
    bool passed = expect(editor && send && stop && sandbox && approval,
                         "the upcoming-turn action controls must be discoverable");
    if (!editor || !send || !stop || !sandbox || !approval)
        return false;

    dock.setActionState(false, true, false, true);
    passed &= expect(!stop->isHidden() && stop->isEnabled() && !editor->isEnabled(),
                     "a running turn must show an enabled Stop action and lock the composer");
    dock.setActionState(false, false, false, true);
    passed &= expect(!stop->isHidden() && !stop->isEnabled() && !editor->isEnabled(),
                     "an interrupt in flight must keep Stop visible but disabled");
    dock.setActionState(false, false, false, false);
    passed &= expect(stop->isHidden() && !send->isHidden()
                         && !send->isEnabled() && !editor->isEnabled(),
                     "a non-writable thread must restore the disabled Send action without a stale Stop");
    dock.setActionState(true, false, true, false);
    passed &= expect(!send->isHidden() && editor->isEnabled()
                         && sandbox->isEnabled() && approval->isEnabled(),
                     "an idle writable thread must re-enable its composer and known policy controls");
    return passed;
}

bool testUnsupportedCanonicalSettingsFailSoft()
{
    codexui::UpcomingTurnDock dock;
    dock.setActionState(true, false, true, false);
    sdk::ExecutionConfiguration unsupported =
        configuration("gpt-test", typed::ReasoningEffort::high(), "/workspace");
    typed::UnknownSandboxPolicy unknownSandbox;
    unknownSandbox.type = "future-sandbox";
    unsupported.sandboxPolicy = std::move(unknownSandbox);
    unsupported.approvalPolicy = typed::GranularAskForApproval{};
    dock.setCanonicalConfiguration(unsupported, QStringLiteral("thread-unsupported"));

    auto* sandbox = dock.findChild<QComboBox*>(QStringLiteral("upcomingAccess"));
    auto* approval = dock.findChild<QComboBox*>(QStringLiteral("upcomingApproval"));
    bool passed = expect(sandbox && approval,
                         "the closed execution-setting controls must be discoverable");
    if (!sandbox || !approval)
        return false;
    passed &= expect(sandbox->currentData().toString() == QStringLiteral("future-sandbox")
                         && approval->currentData().toString() == QStringLiteral("granular")
                         && !sandbox->isEnabled() && !approval->isEnabled()
                         && dock.draft().empty(),
                     "unsupported typed policies must remain visible, read-only, and absent from the write draft");

    dock.setCanonicalConfiguration(
        configuration("gpt-test", typed::ReasoningEffort::high(), "/workspace"),
        QStringLiteral("thread-supported"));
    passed &= expect(sandbox->isEnabled() && approval->isEnabled()
                         && sandbox->findData(QStringLiteral("future-sandbox")) < 0
                         && approval->findData(QStringLiteral("granular")) < 0,
                     "unsupported fallback choices must not leak into another thread's editable controls");
    return passed;
}

bool testUnavailableCanonicalSettingsRemainEditable()
{
    codexui::UpcomingTurnDock dock;
    dock.setCanonicalConfiguration(std::nullopt, QStringLiteral("thread-partial"));
    typed::Model advertisedDefault;
    advertisedDefault.id = typed::ModelId{"preset-default"};
    advertisedDefault.model = typed::ModelId{"model-default"};
    advertisedDefault.displayName = "Default model";
    advertisedDefault.defaultReasoningEffort = typed::ReasoningEffort::high();
    advertisedDefault.isDefault = true;
    dock.setModelCatalog({advertisedDefault});
    dock.setActionState(true, false, true, false);

    auto* model = dock.findChild<QComboBox*>(QStringLiteral("upcomingModel"));
    auto* effort = dock.findChild<QComboBox*>(QStringLiteral("upcomingReasoning"));
    auto* sandbox = dock.findChild<QComboBox*>(QStringLiteral("upcomingAccess"));
    auto* approval = dock.findChild<QComboBox*>(QStringLiteral("upcomingApproval"));
    auto* cwd = dock.findChild<QLineEdit*>(QStringLiteral("upcomingWorkspace"));
    auto* personality = dock.findChild<QComboBox*>(QStringLiteral("upcomingStyle"));
    auto* reviewer = dock.findChild<QComboBox*>(QStringLiteral("upcomingApprovalReviewer"));
    auto* summary = dock.findChild<QComboBox*>(QStringLiteral("upcomingReasoningSummary"));
    auto* collaboration = dock.findChild<QComboBox*>(QStringLiteral("upcomingCollaborationMode"));
    auto* serviceTier = dock.findChild<QComboBox*>(QStringLiteral("upcomingServiceTier"));
    bool passed = expect(model && effort && sandbox && approval && cwd && personality
                             && reviewer && summary && collaboration && serviceTier,
                         "partial-thread settings controls must be discoverable");
    if (!model || !effort || !sandbox || !approval || !cwd || !personality
        || !reviewer || !summary || !collaboration || !serviceTier)
        return false;
    passed &= expect(model->isEnabled() && effort->isEnabled()
                         && sandbox->isEnabled() && approval->isEnabled() && cwd->isEnabled()
                         && model->currentData().toString() == QStringLiteral("unavailable")
                         && model->currentText() == QStringLiteral("Unavailable")
                         && model->toolTip().isEmpty()
                         && effort->currentData().toString() == QStringLiteral("unavailable")
                         && sandbox->currentData().toString() == QStringLiteral("unavailable")
                         && approval->currentData().toString() == QStringLiteral("unavailable")
                         && effort->currentText() == QStringLiteral("Unavailable")
                         && dock.draft().empty(),
                     "missing canonical thread configuration must remain wholly unavailable without inventing a default-model override");
    const int unavailableModelIndex = model->findData(QStringLiteral("unavailable"));
    passed &= expect(unavailableModelIndex >= 0
                         && !(model->model()->flags(model->model()->index(unavailableModelIndex, 0))
                              & Qt::ItemIsEnabled),
                     "the unavailable model sentinel must be display-only rather than a writable model choice");
    QStyleOptionComboBox choiceOption;
    choiceOption.initFrom(model);
    const QRect nativeChoiceIndicator = model->style()->subControlRect(
        QStyle::CC_ComboBox, &choiceOption, QStyle::SC_ComboBoxArrow, model);
    passed &= expect(model->hasFrame()
                         && model->property("codexChevron").toBool()
                         && nativeChoiceIndicator.isValid() && !nativeChoiceIndicator.isEmpty(),
                     "each upcoming-turn combo must reserve a visible choice-indicator region");

    model->setCurrentIndex(model->findData(QStringLiteral("model-default")));
    QCoreApplication::processEvents();
    passed &= expect(effort->currentData().toString() == QStringLiteral("high")
                         && collaboration->isEnabled(),
                     "choosing a model from unavailable state must establish a valid reasoning value before collaboration is enabled");
    collaboration->setCurrentIndex(collaboration->findData(QStringLiteral("plan")));

    sandbox->setCurrentIndex(sandbox->findData(QStringLiteral("danger-full-access")));
    approval->setCurrentIndex(approval->findData(QStringLiteral("never")));
    cwd->setText(QStringLiteral("/workspace/explicit"));
    QMetaObject::invokeMethod(cwd, "textEdited", Q_ARG(QString, cwd->text()));
    const codexui::UpcomingTurnDraft draft = dock.draft();
    passed &= expect(draft.sandboxPolicy.hasValue()
                         && std::holds_alternative<typed::DangerFullAccessSandboxPolicy>(
                             *draft.sandboxPolicy)
                         && draft.approvalPolicy.hasValue()
                         && std::holds_alternative<typed::ApprovalPolicy>(*draft.approvalPolicy)
                         && draft.cwd.hasValue() && *draft.cwd == "/workspace/explicit"
                         && draft.model.hasValue() && draft.model->value == "model-default"
                         && draft.effort.hasValue() && draft.effort->value == "high"
                         && draft.collaborationMode.hasValue()
                         && draft.collaborationMode->settings.reasoningEffort.hasValue()
                         && draft.collaborationMode->settings.reasoningEffort->value == "high",
                     "explicit edits on a partial thread must produce only valid typed turn overrides");

    dock.setCanonicalConfiguration(
        configuration("model-default", typed::ReasoningEffort::high(), "/workspace/stored"),
        QStringLiteral("thread-complete"));
    passed &= expect(model->findData(QStringLiteral("unavailable")) < 0
                         && effort->findData(QStringLiteral("unavailable")) < 0
                         && personality->findData(QStringLiteral("unavailable")) < 0
                         && sandbox->findData(QStringLiteral("unavailable")) < 0
                         && approval->findData(QStringLiteral("unavailable")) < 0
                         && reviewer->findData(QStringLiteral("unavailable")) < 0
                         && serviceTier->findData(QStringLiteral("unavailable")) < 0
                         && summary->findData(QStringLiteral("unavailable")) < 0
                         && collaboration->findData(QStringLiteral("unavailable")) < 0,
                     "display-only unavailable sentinels must not leak into another thread's writable choices");
    return passed;
}

bool setInstructions(codexui::ThreadSetupDialog& dialog,
                     const QString& base,
                     const QString& developer)
{
    auto* baseEdit = dialog.findChild<QPlainTextEdit*>(QStringLiteral("baseInstructionsEdit"));
    auto* developerEdit =
        dialog.findChild<QPlainTextEdit*>(QStringLiteral("developerInstructionsEdit"));
    if (!baseEdit || !developerEdit)
        return false;
    baseEdit->setPlainText(base);
    developerEdit->setPlainText(developer);
    return true;
}

bool testThreadSetupResults()
{
    codexui::ThreadSetupDialog newThread(codexui::ThreadSetupDialog::Mode::NewThread);
    newThread.show();
    settleEvents();
    auto* newThreadName = newThread.findChild<QLineEdit*>(QStringLiteral("threadNameEdit"));
    bool passed = expect(newThreadName && newThreadName->hasFocus(),
                         "New Thread must initially focus its primary name field");
    newThread.setSuggestedThreadName(QStringLiteral("Chosen name"));
    newThread.setTemporary(true);
    passed &= expect(setInstructions(newThread,
                                     QStringLiteral("Base α"),
                                     QStringLiteral("Developer β")),
                     "the New Thread dialog must expose both foundational instruction fields");
    // result() returns a value, so retain one variant for safe inspection.
    const codexui::ThreadSetupResult createdResult = newThread.result();
    const auto* created = std::get_if<codexui::NewThreadSetup>(&createdResult);
    passed &= expect(created && created->name == QStringLiteral("Chosen name")
                         && created->temporary
                         && created->instructions.baseInstructions == QStringLiteral("Base α")
                         && created->instructions.developerInstructions
                                == QStringLiteral("Developer β"),
                     "New Thread must return name, lifetime and exact foundational instructions");

    codexui::ThreadSetupDialog fork(codexui::ThreadSetupDialog::Mode::ForkThread);
    fork.setSuggestedThreadName(QStringLiteral("Forked name"));
    fork.setTemporary(false);
    passed &= expect(setInstructions(fork, QString{}, QStringLiteral("Fork constraint")),
                     "the Fork dialog must expose both foundational instruction fields");
    const codexui::ThreadSetupResult forkResult = fork.result();
    const auto* forked = std::get_if<codexui::ForkThreadSetup>(&forkResult);
    passed &= expect(forked && forked->name == QStringLiteral("Forked name")
                         && !forked->temporary && forked->instructions.baseInstructions.isEmpty()
                         && forked->instructions.developerInstructions
                                == QStringLiteral("Fork constraint"),
                     "Fork must preserve blank-as-inherit semantics and its explicit instruction override");

    codexui::ThreadSetupDialog resume(
        codexui::ThreadSetupDialog::Mode::ResumeWithOptions);
    passed &= expect(setInstructions(resume,
                                     QStringLiteral("Resume base"),
                                     QStringLiteral("Resume developer")),
                     "Resume with options must expose both foundational instruction fields");
    const codexui::ThreadSetupResult resumeResult = resume.result();
    const auto* resumed = std::get_if<codexui::ResumeWithOptionsSetup>(&resumeResult);
    passed &= expect(resumed
                         && resumed->instructions.baseInstructions
                                == QStringLiteral("Resume base")
                         && resumed->instructions.developerInstructions
                                == QStringLiteral("Resume developer")
                         && !resume.findChild<QLineEdit*>(QStringLiteral("threadNameEdit"))
                         && !resume.findChild<QCheckBox*>(
                             QStringLiteral("temporaryThreadCheckBox")),
                     "Resume with options must contain only its supported foundational overrides");
    return passed;
}

bool testThreadActionGating()
{
    const sdk::State emptyState;
    sdk::ThreadState thread;
    thread.id = typed::ThreadId{"thread-actions"};
    thread.status = "idle";
    thread.fullyLoaded = true;
    thread.archived = false;

    const codexui::ThreadActionAvailability idle =
        codexui::detail::threadActionAvailability(emptyState, thread);
    bool passed = expect(idle.open && idle.rename && idle.fork && idle.resumeWithOptions
                             && idle.archive && idle.remove && !idle.interrupt && !idle.unarchive,
                         "an idle fully loaded thread must expose only its safe lifecycle actions");

    thread.archived = true;
    const codexui::ThreadActionAvailability archived =
        codexui::detail::threadActionAvailability(emptyState, thread);
    passed &= expect(archived.open && archived.rename && archived.fork && archived.unarchive
                         && archived.remove && archived.resumeWithOptions && !archived.archive
                         && !archived.interrupt,
                     "an archived thread must remain forkable/resumable and expose Unarchive");

    thread.archived = false;
    thread.status = "detached";
    thread.fullyLoaded = true;
    const codexui::ThreadActionAvailability detached =
        codexui::detail::threadActionAvailability(emptyState, thread);
    passed &= expect(detached.open && detached.rename && detached.fork
                         && detached.resumeWithOptions && detached.archive && !detached.unarchive
                         && detached.remove && !detached.interrupt,
                     "a raw provider status without an active typed turn must not masquerade as running");

    thread.fullyLoaded = false;
    const codexui::ThreadActionAvailability partial =
        codexui::detail::threadActionAvailability(emptyState, thread);
    passed &= expect(partial.open && partial.rename && !partial.fork && !partial.resumeWithOptions
                         && !partial.archive && !partial.unarchive && !partial.remove
                         && !partial.interrupt,
                     "a partial potentially-running thread must fail closed for destructive actions");
    return passed;
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    application.setStyleSheet(codexui::UiStyle::applicationStyleSheet());

    bool passed = true;
    passed &= testUpcomingTurnCanonicalRebase();
    passed &= testAnchoredGrowingComposer();
    passed &= testNarrowUpcomingTurnLayout();
    passed &= testTypedModelCatalog();
    passed &= testUpcomingTurnActionStates();
    passed &= testUnsupportedCanonicalSettingsFailSoft();
    passed &= testUnavailableCanonicalSettingsRemainEditable();
    passed &= testThreadSetupResults();
    passed &= testThreadActionGating();
    return passed ? 0 : 1;
}
