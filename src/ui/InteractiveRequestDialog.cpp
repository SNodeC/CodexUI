// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/InteractiveRequestDialog.h"

#include <ai/openai/codex/frontend/client/State.h>

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <utility>

namespace codexui {
namespace sdk = ai::openai::codex::frontend::client;
namespace frontend = ai::openai::codex::frontend;
namespace typed = ai::openai::codex::typed;
namespace {

QString text(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString kindTitle(frontend::PendingRequestKind kind)
{
    switch (kind) {
        case frontend::PendingRequestKind::CommandExecutionApproval:
            return QStringLiteral("Command execution approval");
        case frontend::PendingRequestKind::FileChangeApproval:
            return QStringLiteral("File change approval");
        case frontend::PendingRequestKind::UserInput:
            return QStringLiteral("Codex needs your input");
        case frontend::PendingRequestKind::ApplyPatchApproval:
            return QStringLiteral("Patch approval");
        case frontend::PendingRequestKind::ExecCommandApproval:
            return QStringLiteral("Command approval");
        case frontend::PendingRequestKind::PermissionsApproval:
            return QStringLiteral("Permission approval");
        case frontend::PendingRequestKind::Authentication:
            return QStringLiteral("Authentication request");
        case frontend::PendingRequestKind::Attestation:
            return QStringLiteral("Attestation request");
        case frontend::PendingRequestKind::DynamicToolCall:
            return QStringLiteral("Dynamic tool request");
        case frontend::PendingRequestKind::McpElicitation:
            return QStringLiteral("MCP elicitation request");
    }
    return QStringLiteral("Interactive request");
}

bool isSimpleApproval(frontend::PendingRequestKind kind)
{
    return kind == frontend::PendingRequestKind::CommandExecutionApproval
           || kind == frontend::PendingRequestKind::FileChangeApproval;
}

bool isReviewApproval(frontend::PendingRequestKind kind)
{
    return kind == frontend::PendingRequestKind::ApplyPatchApproval
           || kind == frontend::PendingRequestKind::ExecCommandApproval;
}

QLabel* wrappedLabel(const QString& value, const char* kind = nullptr)
{
    auto* result = new QLabel(value);
    result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (kind)
        result->setProperty("kind", kind);
    return result;
}

void addDetail(QVBoxLayout* layout, const QString& value)
{
    if (!value.isEmpty())
        layout->addWidget(wrappedLabel(value, "muted"));
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        if (QLayout* child = item->layout()) {
            clearLayout(child);
            delete child;
        }
        delete item;
    }
}

const sdk::PendingRequestState* pendingRequest(const sdk::State& state, const std::string& id)
{
    const auto requests = state.pendingRequests();
    const auto found = std::find_if(requests.begin(), requests.end(), [&id](const auto& request) {
        return request.id.value == id;
    });
    return found == requests.end() ? nullptr : &*found;
}

} // namespace

InteractiveRequestDialog::InteractiveRequestDialog(StateProvider provider,
                                                   ResponseHandler handler,
                                                   QWidget* parent)
    : QDialog(parent)
    , stateProvider(std::move(provider))
    , responseHandler(std::move(handler))
{
    setWindowTitle(QStringLiteral("Needs attention"));
    setWindowModality(Qt::NonModal);
    setModal(false);
    setMinimumWidth(520);
    resize(580, 430);

    root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(14);

    auto* header = new QHBoxLayout;
    auto* title = wrappedLabel(QStringLiteral("NEEDS ATTENTION"), "attentionSection");
    header->addWidget(title);
    header->addStretch();
    queueLabel = wrappedLabel({}, "meta");
    header->addWidget(queueLabel);
    root->addLayout(header);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    body = new QWidget;
    body->setLayout(new QVBoxLayout);
    body->layout()->setContentsMargins(0, 0, 0, 0);
    body->layout()->setSpacing(10);
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    statusLabel = wrappedLabel({}, "meta");
    statusLabel->hide();
    root->addWidget(statusLabel);

    auto* buttons = new QHBoxLayout;
    auto* close = new QPushButton(QStringLiteral("Close"));
    close->setProperty("kind", "subtle");
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(close);
    buttons->addStretch();
    nextButton = new QPushButton(QStringLiteral("Next request"));
    nextButton->setProperty("kind", "subtle");
    connect(nextButton, &QPushButton::clicked, this, [this] { showNext(); });
    buttons->addWidget(nextButton);
    submitButton = new QPushButton(QStringLiteral("Submit response"));
    submitButton->setProperty("kind", "primary");
    connect(submitButton, &QPushButton::clicked, this, [this] { submitCurrent(); });
    buttons->addWidget(submitButton);
    root->addLayout(buttons);
}

void InteractiveRequestDialog::synchronize(const sdk::State& state)
{
    saveCurrentDraft();

    orderedRequestIds.clear();
    std::set<std::string> currentIds;
    for (const auto& request : state.pendingRequests()) {
        orderedRequestIds.push_back(request.id.value);
        currentIds.insert(request.id.value);
    }
    for (auto iterator = drafts.begin(); iterator != drafts.end();) {
        if (!currentIds.contains(iterator->first))
            iterator = drafts.erase(iterator);
        else
            ++iterator;
    }
    std::erase_if(submittedRequestIds, [&currentIds](const auto& id) { return !currentIds.contains(id); });

    const bool newlyNeedsAttention = previousCount == 0 && !orderedRequestIds.empty();
    previousCount = orderedRequestIds.size();
    if (orderedRequestIds.empty()) {
        currentRequestId.clear();
        submittingRequestId.clear();
        hide();
        return;
    }
    if (!currentIds.contains(currentRequestId))
        currentRequestId = orderedRequestIds.front();

    rebuild(state);
    if (newlyNeedsAttention)
        present();
}

void InteractiveRequestDialog::present()
{
    if (orderedRequestIds.empty())
        return;
    saveCurrentDraft();
    rebuild(stateProvider());
    show();
    raise();
    activateWindow();
}

void InteractiveRequestDialog::setSubmitting(const std::string& requestId, const QString& status)
{
    submittingRequestId = requestId;
    if (currentRequestId == requestId)
        setStatus(status);
    updateSubmitEnabled();
}

void InteractiveRequestDialog::responseAccepted(const std::string& requestId)
{
    if (submittingRequestId == requestId)
        submittingRequestId.clear();
    submittedRequestIds.insert(requestId);
    if (currentRequestId == requestId)
        setStatus(QStringLiteral("Response submitted… Waiting for canonical state."));
    updateSubmitEnabled();
}

void InteractiveRequestDialog::responseFailed(const std::string& requestId, const QString& error)
{
    if (submittingRequestId == requestId)
        submittingRequestId.clear();
    submittedRequestIds.erase(requestId);
    if (currentRequestId == requestId)
        setStatus(error.isEmpty() ? QStringLiteral("Response could not be submitted") : error, true);
    updateSubmitEnabled();
}

void InteractiveRequestDialog::saveCurrentDraft()
{
    if (currentRequestId.empty())
        return;
    RequestDraft& draft = drafts[currentRequestId];
    for (std::size_t index = 0; index < approvalChoices.size(); ++index) {
        if (approvalChoices[index]->isChecked())
            draft.approvalIndex = static_cast<int>(index + 1);
    }
    for (const QuestionEditor& editor : questionEditors) {
        QuestionDraft& question = draft.questions[editor.id];
        question.selectedOptions.clear();
        for (const auto& [label, checkbox] : editor.options) {
            if (checkbox->isChecked())
                question.selectedOptions.insert(label);
        }
        if (editor.freeText)
            question.freeText = editor.freeText->text();
    }
}

void InteractiveRequestDialog::rebuild(const sdk::State& state)
{
    const auto* request = pendingRequest(state, currentRequestId);
    if (!request) {
        hide();
        return;
    }

    approvalChoices.clear();
    questionEditors.clear();
    clearLayout(body->layout());
    auto* content = static_cast<QVBoxLayout*>(body->layout());
    const auto view = sdk::pendingRequestPresentation(*request);

    auto* heading = wrappedLabel(kindTitle(request->kind), "heading");
    heading->setToolTip(QStringLiteral("Request ID: %1").arg(text(request->id.value)));
    content->addWidget(heading);
    if (request->summary && !request->summary->empty())
        addDetail(content, text(*request->summary));

    QStringList provenance;
    if (view.threadId)
        provenance.append(QStringLiteral("Thread %1").arg(text(view.threadId->value)));
    if (view.turnId)
        provenance.append(QStringLiteral("Turn %1").arg(text(view.turnId->value)));
    if (view.itemId)
        provenance.append(QStringLiteral("Item %1").arg(text(view.itemId->value)));
    addDetail(content, provenance.join(QStringLiteral(" · ")));

    if (view.itemId) {
        if (const auto* item = state.item(*view.itemId)) {
            const auto semantic = sdk::itemSemanticView(*item);
            if (semantic) {
                if (const auto* command = std::get_if<sdk::CommandExecutionSemanticView>(&semantic->details)) {
                    if (command->command)
                        addDetail(content, QStringLiteral("Command: %1").arg(text(*command->command)));
                    if (command->cwd)
                        addDetail(content, QStringLiteral("Working directory: %1").arg(text(command->cwd->value)));
                } else if (const auto* changes = std::get_if<sdk::FileChangeSemanticView>(&semantic->details)) {
                    if (changes->changeCount)
                        addDetail(content, QStringLiteral("Affected changes: %1").arg(*changes->changeCount));
                }
            }
        }
    }
    if (view.fileChangeCount)
        addDetail(content, QStringLiteral("Files changed: %1").arg(*view.fileChangeCount));
    if (view.commandArgumentCount)
        addDetail(content, QStringLiteral("Command arguments: %1").arg(*view.commandArgumentCount));
    if (view.parsedCommandCount)
        addDetail(content, QStringLiteral("Parsed commands: %1").arg(*view.parsedCommandCount));
    if (view.commandRedacted)
        addDetail(content, QStringLiteral("Command details are redacted by AISuite"));
    if (view.cwdRedacted)
        addDetail(content, QStringLiteral("Working directory is redacted by AISuite"));
    if (view.reasonRedacted)
        addDetail(content, QStringLiteral("Approval reason is redacted by AISuite"));
    if (view.truncated || !view.omittedFields.empty())
        addDetail(content, QStringLiteral("Some projected request details were omitted"));

    RequestDraft& draft = drafts[currentRequestId];
    if (isSimpleApproval(request->kind) || isReviewApproval(request->kind)) {
        auto* prompt = wrappedLabel(QStringLiteral("Choose how Codex should continue:"), "body");
        content->addWidget(prompt);
        const QStringList decisions{
            QStringLiteral("Approve"),
            QStringLiteral("Approve for this session"),
            isSimpleApproval(request->kind) ? QStringLiteral("Decline") : QStringLiteral("Deny"),
            isSimpleApproval(request->kind) ? QStringLiteral("Cancel") : QStringLiteral("Abort"),
        };
        for (int index = 0; index < decisions.size(); ++index) {
            auto* choice = new QRadioButton(decisions[index]);
            choice->setChecked(draft.approvalIndex == index + 1);
            connect(choice, &QRadioButton::toggled, this, [this](bool) { updateSubmitEnabled(); });
            approvalChoices.push_back(choice);
            content->addWidget(choice);
        }
    } else if (request->kind == frontend::PendingRequestKind::UserInput && request->questions
               && !request->questions->empty()) {
        for (const auto& question : *request->questions) {
            auto* section = new QFrame;
            section->setProperty("kind", "raised");
            auto* sectionLayout = new QVBoxLayout(section);
            sectionLayout->setContentsMargins(14, 12, 14, 12);
            sectionLayout->setSpacing(7);
            if (!question.header.empty())
                sectionLayout->addWidget(wrappedLabel(text(question.header), "title"));
            sectionLayout->addWidget(wrappedLabel(text(question.prompt), "body"));

            QuestionEditor editor;
            editor.id = question.id;
            const QuestionDraft& questionDraft = draft.questions[question.id];
            for (const auto& option : question.options) {
                auto* checkbox = new QCheckBox(text(option.label));
                checkbox->setChecked(questionDraft.selectedOptions.contains(option.label));
                if (!option.description.empty())
                    checkbox->setToolTip(text(option.description));
                connect(checkbox, &QCheckBox::toggled, this, [this](bool) { updateSubmitEnabled(); });
                sectionLayout->addWidget(checkbox);
                if (!option.description.empty()) {
                    auto* description = wrappedLabel(text(option.description), "meta");
                    description->setContentsMargins(24, 0, 0, 2);
                    sectionLayout->addWidget(description);
                }
                editor.options.emplace_back(option.label, checkbox);
            }
            if (question.allowsFreeText) {
                editor.freeText = new QLineEdit;
                editor.freeText->setPlaceholderText(question.options.empty() ? QStringLiteral("Type your answer")
                                                                            : QStringLiteral("Other answer"));
                editor.freeText->setText(questionDraft.freeText);
                if (question.isSecret)
                    editor.freeText->setEchoMode(QLineEdit::Password);
                connect(editor.freeText, &QLineEdit::textChanged, this, [this](const QString&) { updateSubmitEnabled(); });
                sectionLayout->addWidget(editor.freeText);
            }
            questionEditors.push_back(std::move(editor));
            content->addWidget(section);
        }
    } else {
        content->addWidget(wrappedLabel(
            QStringLiteral("This Codex request is visible, but this version of CodexUI has no safe typed response UI for it."),
            "body"));
    }
    content->addStretch();

    const auto current = std::find(orderedRequestIds.begin(), orderedRequestIds.end(), currentRequestId);
    const auto index = current == orderedRequestIds.end() ? 0 : std::distance(orderedRequestIds.begin(), current);
    queueLabel->setText(QStringLiteral("Request %1 of %2").arg(index + 1).arg(orderedRequestIds.size()));
    nextButton->setVisible(orderedRequestIds.size() > 1);
    if (submittingRequestId == currentRequestId)
        setStatus(QStringLiteral("Submitting response…"));
    else if (submittedRequestIds.contains(currentRequestId))
        setStatus(QStringLiteral("Response submitted… Waiting for canonical state."));
    else
        setStatus({});
    updateSubmitEnabled();
}

void InteractiveRequestDialog::showNext()
{
    if (orderedRequestIds.size() < 2)
        return;
    saveCurrentDraft();
    const auto current = std::find(orderedRequestIds.begin(), orderedRequestIds.end(), currentRequestId);
    const auto next = current == orderedRequestIds.end() || std::next(current) == orderedRequestIds.end()
                          ? orderedRequestIds.begin()
                          : std::next(current);
    currentRequestId = *next;
    rebuild(stateProvider());
}

void InteractiveRequestDialog::submitCurrent()
{
    saveCurrentDraft();
    const auto& state = stateProvider();
    const auto* request = pendingRequest(state, currentRequestId);
    if (!request) {
        responseFailed(currentRequestId, QStringLiteral("This request is no longer pending"));
        synchronize(state);
        return;
    }
    if (submittingRequestId == currentRequestId || submittedRequestIds.contains(currentRequestId))
        return;

    InteractiveRequestResponse response{request->id, request->kind, typed::ApprovalDecision::cancel()};
    const RequestDraft& draft = drafts[currentRequestId];
    if (isSimpleApproval(request->kind)) {
        switch (draft.approvalIndex) {
            case 1: response.value = typed::ApprovalDecision::accept(); break;
            case 2: response.value = typed::ApprovalDecision::acceptForSession(); break;
            case 3: response.value = typed::ApprovalDecision::decline(); break;
            case 4: response.value = typed::ApprovalDecision::cancel(); break;
            default:
                setStatus(QStringLiteral("Choose an approval decision"), true);
                return;
        }
    } else if (isReviewApproval(request->kind)) {
        typed::ReviewDecision decision;
        switch (draft.approvalIndex) {
            case 1: decision = typed::ApprovedReviewDecision{}; break;
            case 2: decision = typed::ApprovedForSessionReviewDecision{}; break;
            case 3: decision = typed::DeniedReviewDecision{}; break;
            case 4: decision = typed::AbortReviewDecision{}; break;
            default:
                setStatus(QStringLiteral("Choose an approval decision"), true);
                return;
        }
        if (request->kind == frontend::PendingRequestKind::ApplyPatchApproval)
            response.value = typed::ApplyPatchApprovalResponse{std::move(decision)};
        else
            response.value = typed::ExecCommandApprovalResponse{std::move(decision)};
    } else if (request->kind == frontend::PendingRequestKind::UserInput && request->questions
               && !request->questions->empty()) {
        std::vector<typed::UserInputAnswer> answers;
        answers.reserve(request->questions->size());
        for (const auto& question : *request->questions) {
            const QuestionDraft& questionDraft = draft.questions.at(question.id);
            std::vector<std::string> values(questionDraft.selectedOptions.begin(), questionDraft.selectedOptions.end());
            if (!questionDraft.freeText.trimmed().isEmpty())
                values.push_back(questionDraft.freeText.toStdString());
            if (values.empty()) {
                setStatus(QStringLiteral("Answer every question before submitting"), true);
                return;
            }
            answers.push_back({question.id, std::move(values)});
        }
        response.value = std::move(answers);
    } else {
        setStatus(QStringLiteral("This request type is not supported"), true);
        return;
    }

    setSubmitting(currentRequestId, QStringLiteral("Preparing response…"));
    responseHandler(std::move(response));
}

void InteractiveRequestDialog::setStatus(const QString& value, bool error)
{
    statusLabel->setText(value);
    statusLabel->setVisible(!value.isEmpty());
    statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:600;")
                                   .arg(error ? QStringLiteral("#f08a8a") : QStringLiteral("#949ead")));
}

void InteractiveRequestDialog::updateSubmitEnabled()
{
    const bool supported = !approvalChoices.empty() || !questionEditors.empty();
    const bool decisionChosen = approvalChoices.empty()
                                || std::ranges::any_of(approvalChoices, [](const auto* choice) { return choice->isChecked(); });
    const bool busy = !submittingRequestId.empty() || submittedRequestIds.contains(currentRequestId);
    submitButton->setEnabled(supported && decisionChosen && !busy);
}

} // namespace codexui
