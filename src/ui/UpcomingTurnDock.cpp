// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/UpcomingTurnDock.h"

#include <QComboBox>
#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStyleOptionComboBox>
#include <QTextDocument>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <variant>

namespace codexui
{
namespace
{
namespace sdk = ai::openai::codex::frontend::client;
namespace typed = ai::openai::codex::typed;

constexpr int oneLineEditorHeight = 30;
constexpr int maximumEditorHeight = 200;

class CompactComboBox final : public QComboBox
{
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QComboBox::paintEvent(event);

        QStyleOptionComboBox option;
        initStyleOption(&option);
        const QRect indicator = style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
        if (!indicator.isValid() || indicator.isEmpty())
            return;

        const QPointF center = indicator.center();
        QPainterPath chevron;
        chevron.moveTo(center.x() - 3.5, center.y() - 1.5);
        chevron.lineTo(center.x(), center.y() + 2.0);
        chevron.lineTo(center.x() + 3.5, center.y() - 1.5);

        QColor color(QStringLiteral("#667085"));
        if (!(option.state & QStyle::State_Enabled))
            color = QColor(QStringLiteral("#98a2b3"));
        else if (option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus))
            color = QColor(QStringLiteral("#1d2633"));

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(chevron);
    }
};

QString fromUtf8(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string toUtf8(const QString& value)
{
    const QByteArray encoded = value.toUtf8();
    return std::string(encoded.constData(), static_cast<std::size_t>(encoded.size()));
}

QLabel* plainLabel(const QString& text, const char* objectName = nullptr)
{
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    if (objectName)
        result->setObjectName(QString::fromLatin1(objectName));
    return result;
}

QWidget* labelledControl(const QString& label, QWidget* control)
{
    auto* result = new QFrame;
    result->setObjectName(QStringLiteral("turnSettingChip"));
    result->setProperty("changed", false);
    result->setStyleSheet(QStringLiteral(
        "QFrame#turnSettingChip{background:transparent;border:0;}"
        "QFrame#turnSettingChip[changed=\"true\"]{background:#e5eeff;border:0;border-radius:7px;}"));
    auto* layout = new QVBoxLayout(result);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto* caption = plainLabel(label);
    caption->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;font-weight:600;"));
    caption->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    caption->setFixedHeight(13);
    caption->setBuddy(control);
    control->setAccessibleName(label);
    layout->addWidget(caption, 0, Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(control, 0);
    result->setFixedHeight(52);
    return result;
}

void addChoice(QComboBox* combo, const QString& text, const QString& key)
{
    if (combo->findData(key) < 0)
        combo->addItem(text, key);
}

void selectKey(QComboBox* combo,
               const QString& key,
               const QString& fallbackText = {},
               bool fallbackSelectable = true)
{
    int index = combo->findData(key);
    if (index < 0)
    {
        combo->addItem(fallbackText.isEmpty() ? key : fallbackText, key);
        index = combo->count() - 1;
        if (!fallbackSelectable) {
            if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
                if (QStandardItem* item = model->item(index))
                    item->setFlags(item->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
            }
        }
    }
    combo->setCurrentIndex(index);
}

QString effortKey(const typed::OptionalNullable<typed::ReasoningEffort>& value)
{
    return value.hasValue() ? fromUtf8(value->value) : QStringLiteral("default");
}

QString personalityKey(const typed::OptionalNullable<typed::Personality>& value)
{
    return value.hasValue() ? fromUtf8(value->value) : QStringLiteral("default");
}

QString summaryKey(const typed::OptionalNullable<typed::ReasoningSummary>& value)
{
    return value.hasValue() ? fromUtf8(value->value) : QStringLiteral("default");
}

QString sandboxKey(const typed::SandboxPolicy& value)
{
    return std::visit(
        [](const auto& item) -> QString {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, typed::DangerFullAccessSandboxPolicy>)
                return QStringLiteral("danger-full-access");
            if constexpr (std::is_same_v<T, typed::ReadOnlySandboxPolicy>)
                return QStringLiteral("read-only");
            if constexpr (std::is_same_v<T, typed::WorkspaceWriteSandboxPolicy>)
                return QStringLiteral("workspace-write");
            if constexpr (std::is_same_v<T, typed::ExternalSandboxPolicy>)
                return QStringLiteral("external");
            if constexpr (std::is_same_v<T, typed::UnknownSandboxPolicy>)
                return item.type ? fromUtf8(*item.type) : QStringLiteral("unknown");
        },
        value);
}

QString approvalKey(const typed::AskForApproval& value)
{
    return std::visit(
        [](const auto& item) -> QString {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, typed::ApprovalPolicy>)
                return fromUtf8(item.value);
            if constexpr (std::is_same_v<T, typed::GranularAskForApproval>)
                return QStringLiteral("granular");
            if constexpr (std::is_same_v<T, typed::UnknownAskForApproval>)
                return item.discriminator ? fromUtf8(*item.discriminator) : QStringLiteral("unknown");
        },
        value);
}

QString collaborationKey(const typed::CollaborationMode& value)
{
    return fromUtf8(value.mode.value);
}

std::optional<typed::SandboxPolicy> sandboxForKey(const QString& key)
{
    if (key == QStringLiteral("danger-full-access"))
        return typed::DangerFullAccessSandboxPolicy{};
    if (key == QStringLiteral("read-only"))
        return typed::ReadOnlySandboxPolicy{};
    if (key == QStringLiteral("external"))
        return typed::ExternalSandboxPolicy{};
    if (key == QStringLiteral("workspace-write"))
        return typed::WorkspaceWriteSandboxPolicy{};
    return std::nullopt;
}

std::optional<typed::AskForApproval> approvalForKey(const QString& key)
{
    const typed::ApprovalPolicy value{toUtf8(key)};
    if (!value.isKnown())
        return std::nullopt;
    return typed::AskForApproval{value};
}

bool sandboxIsEditable(const typed::SandboxPolicy& value)
{
    return !std::holds_alternative<typed::UnknownSandboxPolicy>(value);
}

bool approvalIsEditable(const typed::AskForApproval& value)
{
    const auto* policy = std::get_if<typed::ApprovalPolicy>(&value);
    return policy && policy->isKnown();
}

QString defaultSettingLabel()
{
    return QStringLiteral("Codex default");
}

QString unavailableSettingLabel()
{
    return QStringLiteral("Unavailable");
}

void resetSandboxChoices(QComboBox* combo)
{
    combo->clear();
    addChoice(combo, defaultSettingLabel(), QStringLiteral("default"));
    addChoice(combo, QStringLiteral("Workspace"), QStringLiteral("workspace-write"));
    addChoice(combo, QStringLiteral("Read only"), QStringLiteral("read-only"));
    addChoice(combo, QStringLiteral("Full access"), QStringLiteral("danger-full-access"));
    addChoice(combo, QStringLiteral("External"), QStringLiteral("external"));
}

void resetApprovalChoices(QComboBox* combo)
{
    combo->clear();
    addChoice(combo, defaultSettingLabel(), QStringLiteral("default"));
    addChoice(combo, QStringLiteral("On request"), QStringLiteral("on-request"));
    addChoice(combo, QStringLiteral("Untrusted"), QStringLiteral("untrusted"));
    addChoice(combo, QStringLiteral("Never"), QStringLiteral("never"));
}

void resetPersonalityChoices(QComboBox* combo)
{
    combo->clear();
    addChoice(combo, defaultSettingLabel(), QStringLiteral("default"));
    addChoice(combo, QStringLiteral("None"), QStringLiteral("none"));
    addChoice(combo, QStringLiteral("Friendly"), QStringLiteral("friendly"));
    addChoice(combo, QStringLiteral("Pragmatic"), QStringLiteral("pragmatic"));
}

void resetReviewerChoices(QComboBox* combo)
{
    combo->clear();
    addChoice(combo, QStringLiteral("User"), QStringLiteral("user"));
    addChoice(combo, defaultSettingLabel(), QStringLiteral("default"));
    addChoice(combo, QStringLiteral("Auto review"), QStringLiteral("auto_review"));
    addChoice(combo, QStringLiteral("Guardian"), QStringLiteral("guardian_subagent"));
}

void resetSummaryChoices(QComboBox* combo)
{
    combo->clear();
    addChoice(combo, defaultSettingLabel(), QStringLiteral("default"));
    addChoice(combo, QStringLiteral("Auto"), QStringLiteral("auto"));
    addChoice(combo, QStringLiteral("Concise"), QStringLiteral("concise"));
    addChoice(combo, QStringLiteral("Detailed"), QStringLiteral("detailed"));
    addChoice(combo, QStringLiteral("None"), QStringLiteral("none"));
}

void resetCollaborationChoices(QComboBox* combo)
{
    combo->clear();
    addChoice(combo, defaultSettingLabel(), QStringLiteral("default"));
    addChoice(combo, QStringLiteral("Plan"), QStringLiteral("plan"));
}

typed::CollaborationMode collaborationForKey(
    const QString& key,
    const QString& selectedModel,
    const QString& selectedEffort,
    const std::optional<sdk::ExecutionConfiguration>& canonical)
{
    typed::CollaborationMode result;
    if (canonical)
        result = canonical->collaborationMode;
    result.mode.value = toUtf8(key);
    const QString effectiveModel = !selectedModel.isEmpty()
        ? selectedModel
        : canonical ? fromUtf8(canonical->model.value) : QString{};
    result.settings.model = typed::ModelId{toUtf8(effectiveModel)};
    if (selectedEffort == QStringLiteral("default"))
        result.settings.reasoningEffort = typed::OptionalNullable<typed::ReasoningEffort>::explicitNull();
    else if (!selectedEffort.isEmpty() && selectedEffort != QStringLiteral("unavailable"))
        result.settings.reasoningEffort = typed::ReasoningEffort{toUtf8(selectedEffort)};
    else
        result.settings.reasoningEffort =
            typed::OptionalNullable<typed::ReasoningEffort>{};
    return result;
}

QString friendlyValue(QString value)
{
    value.replace(QLatin1Char('-'), QLatin1Char(' '));
    value.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!value.isEmpty())
        value[0] = value.at(0).toUpper();
    return value;
}

QComboBox* compactCombo(const char* name)
{
    auto* result = new CompactComboBox;
    result->setObjectName(QString::fromLatin1(name));
    result->setProperty("codexChevron", true);
    result->setFixedHeight(34);
    result->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    result->setMinimumContentsLength(3);
    return result;
}

} // namespace

bool UpcomingTurnDraft::empty() const noexcept
{
    return model.isOmitted() && effort.isOmitted() && personality.isOmitted() && sandboxPolicy.isOmitted()
        && approvalPolicy.isOmitted() && approvalsReviewer.isOmitted() && cwd.isOmitted() && serviceTier.isOmitted()
        && summary.isOmitted() && collaborationMode.isOmitted();
}

UpcomingTurnDock::UpcomingTurnDock(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("upcomingTurnDock"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#upcomingTurnDock{background:#ffffff;border-top:1px solid #d7dee8;}"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 10, 16, 12);
    root->setSpacing(8);

    auto* headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(0, 0, 0, 0);
    headingRow->setSpacing(8);
    auto* heading = plainLabel(QStringLiteral("UPCOMING TURN"), "upcomingTurnHeading");
    heading->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;font-weight:700;letter-spacing:.5px;"));
    headingRow->addWidget(heading);
    headingRow->addStretch();
    root->addLayout(headingRow);

    settingsSurface = new QFrame;
    settingsSurface->setObjectName(QStringLiteral("upcomingTurnSettings"));
    settingsSurface->setStyleSheet(QStringLiteral(
        "#upcomingTurnSettings{background:#ffffff;border:1px solid #d7dee8;border-radius:10px;}"));
    auto* settingsLayout = new QVBoxLayout(settingsSurface);
    settingsLayout->setContentsMargins(10, 8, 10, 7);
    settingsLayout->setSpacing(5);

    auto* settingsGrid = new QGridLayout;
    settingsGrid->setContentsMargins(0, 0, 0, 0);
    settingsGrid->setHorizontalSpacing(8);
    settingsGrid->setVerticalSpacing(6);

    model = compactCombo("upcomingModel");
    model->setEditable(true);
    effort = compactCombo("upcomingReasoning");
    personality = compactCombo("upcomingStyle");
    sandbox = compactCombo("upcomingAccess");
    approval = compactCombo("upcomingApproval");
    fieldSurfaces[static_cast<std::size_t>(Field::Model)] = labelledControl(QStringLiteral("Model"), model);
    fieldSurfaces[static_cast<std::size_t>(Field::Effort)] = labelledControl(QStringLiteral("Reasoning"), effort);
    fieldSurfaces[static_cast<std::size_t>(Field::Personality)] = labelledControl(QStringLiteral("Style"), personality);
    fieldSurfaces[static_cast<std::size_t>(Field::Sandbox)] = labelledControl(QStringLiteral("Access"), sandbox);
    fieldSurfaces[static_cast<std::size_t>(Field::Approval)] = labelledControl(QStringLiteral("Approval"), approval);
    cwd = new QLineEdit;
    cwd->setObjectName(QStringLiteral("upcomingWorkspace"));
    cwd->setPlaceholderText(QStringLiteral("Codex default workspace"));
    cwd->setFixedHeight(34);
    fieldSurfaces[static_cast<std::size_t>(Field::Cwd)] = labelledControl(QStringLiteral("Workspace"), cwd);
    more = new QPushButton(QStringLiteral("More…"));
    more->setObjectName(QStringLiteral("upcomingMore"));
    more->setFixedHeight(34);
    more->setStyleSheet(QStringLiteral(
        "QPushButton{background:#ffffff;color:#344054;border:1px solid #d7dee8;border-radius:7px;padding:2px 12px;font-weight:600;}"
        "QPushButton:hover{background:#f1f5fb;}"
        "QPushButton[changed=\"true\"]{background:#e5eeff;color:#2f6feb;border-color:#2f6feb;}"));
    auto* moreSurface = labelledControl(QStringLiteral("Additional"), more);

    // Two stable rows keep every choice readable at the supported narrow
    // window width. Model receives the extra column in the primary row while
    // all controls retain the same caption-above-control alignment.
    settingsGrid->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Model)], 0, 0, 1, 2);
    settingsGrid->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Effort)], 0, 2);
    settingsGrid->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Personality)], 0, 3);
    settingsGrid->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Sandbox)], 1, 0);
    settingsGrid->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Approval)], 1, 1);
    settingsGrid->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Cwd)], 1, 2);
    settingsGrid->addWidget(moreSurface, 1, 3);
    settingsGrid->setColumnStretch(0, 4);
    settingsGrid->setColumnStretch(1, 4);
    settingsGrid->setColumnStretch(2, 5);
    settingsGrid->setColumnStretch(3, 4);
    settingsLayout->addLayout(settingsGrid);

    settingsHint = plainLabel({}, "upcomingSettingsHint");
    settingsHint->setStyleSheet(QStringLiteral("color:#2f6feb;font-size:10px;font-weight:600;"));
    // Reserve this row permanently so changing a setting never changes or
    // clips the fixed base geometry of the anchored turn dock.
    settingsHint->setFixedHeight(14);
    settingsLayout->addWidget(settingsHint, 0, Qt::AlignLeft);
    root->addWidget(settingsSurface);

    moreMenu = new QMenu(this);
    moreMenu->setObjectName(QStringLiteral("upcomingMoreMenu"));
    moreMenu->setStyleSheet(QStringLiteral(
        "QMenu{background:#ffffff;border:1px solid #d7dee8;border-radius:10px;padding:0;}"));
    auto* moreContents = new QWidget;
    moreContents->setObjectName(QStringLiteral("upcomingMoreContents"));
    moreContents->setMinimumWidth(430);
    auto* moreLayout = new QGridLayout(moreContents);
    moreLayout->setContentsMargins(16, 14, 16, 14);
    moreLayout->setHorizontalSpacing(8);
    moreLayout->setVerticalSpacing(8);
    serviceTier = compactCombo("upcomingServiceTier");
    serviceTier->setEditable(true);
    summary = compactCombo("upcomingReasoningSummary");
    collaboration = compactCombo("upcomingCollaborationMode");
    reviewer = compactCombo("upcomingApprovalReviewer");
    fieldSurfaces[static_cast<std::size_t>(Field::ServiceTier)] = labelledControl(
        QStringLiteral("Service tier"), serviceTier);
    fieldSurfaces[static_cast<std::size_t>(Field::Summary)] = labelledControl(
        QStringLiteral("Reasoning summary"), summary);
    fieldSurfaces[static_cast<std::size_t>(Field::Collaboration)] = labelledControl(
        QStringLiteral("Collaboration mode"), collaboration);
    fieldSurfaces[static_cast<std::size_t>(Field::Reviewer)] = labelledControl(
        QStringLiteral("Approval reviewer"), reviewer);
    moreLayout->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::ServiceTier)], 0, 0);
    moreLayout->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Summary)], 0, 1);
    moreLayout->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Collaboration)], 1, 0);
    moreLayout->addWidget(fieldSurfaces[static_cast<std::size_t>(Field::Reviewer)], 1, 1);
    auto* moreAction = new QWidgetAction(moreMenu);
    moreAction->setDefaultWidget(moreContents);
    moreMenu->addAction(moreAction);
    more->setMenu(moreMenu);

    composerSurface = new QFrame;
    composerSurface->setObjectName(QStringLiteral("upcomingComposer"));
    composerSurface->setStyleSheet(QStringLiteral(
        "#upcomingComposer{background:#ffffff;border:2px solid #b9c4d2;border-radius:10px;}"
        "#upcomingComposer[focused=\"true\"]{border:2px solid #2f6feb;}"));
    composerSurface->setProperty("focused", false);
    auto* composerLayout = new QHBoxLayout(composerSurface);
    composerLayout->setContentsMargins(10, 8, 10, 8);
    composerLayout->setSpacing(8);
    auto* attach = new QPushButton(QStringLiteral("Attach"));
    attach->setObjectName(QStringLiteral("upcomingAttach"));
    attach->setEnabled(false);
    attach->setToolTip(QStringLiteral("Attachments are not available in this phase."));
    attach->setFixedSize(54, 28);
    attach->setStyleSheet(QStringLiteral("color:#667085;background:transparent;border:0;padding:2px 4px;"));
    composerLayout->addWidget(attach, 0, Qt::AlignBottom);

    editor = new QPlainTextEdit;
    editor->setObjectName(QStringLiteral("upcomingPromptEditor"));
    editor->setPlaceholderText(QStringLiteral("Message Codex"));
    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setMinimumHeight(oneLineEditorHeight);
    editor->setMaximumHeight(maximumEditorHeight);
    editor->setFixedHeight(oneLineEditorHeight);
    editor->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:transparent;color:#1d2633;border:0;padding:3px 2px;font-size:13px;}"));
    editor->installEventFilter(this);
    editor->viewport()->installEventFilter(this);
    currentEditorHeight = oneLineEditorHeight;
    composerLayout->addWidget(editor, 1);

    status = plainLabel(QStringLiteral("Ctrl+Enter to send"), "upcomingTurnStatus");
    status->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;"));
    status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    status->setMinimumWidth(112);
    status->setMaximumWidth(220);
    status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    composerLayout->addWidget(status, 0, Qt::AlignBottom);
    send = new QPushButton(QStringLiteral("Send"));
    send->setObjectName(QStringLiteral("upcomingSendButton"));
    send->setMinimumSize(66, 28);
    send->setStyleSheet(QStringLiteral(
        "QPushButton{background:#2f6feb;color:#ffffff;border:0;border-radius:7px;font-weight:700;}"
        "QPushButton:hover{background:#245fd1;}QPushButton:disabled{background:#d7dee8;color:#98a2b3;}"));
    composerLayout->addWidget(send, 0, Qt::AlignBottom);
    stop = new QPushButton(QStringLiteral("Stop"));
    stop->setObjectName(QStringLiteral("upcomingStopButton"));
    stop->setMinimumSize(66, 28);
    stop->setStyleSheet(QStringLiteral(
        "QPushButton{background:#fff4f2;color:#b83a3a;border:1px solid #f0c2bb;border-radius:7px;font-weight:700;}"
        "QPushButton:hover{background:#ffe9e5;}QPushButton:disabled{color:#98a2b3;border-color:#d7dee8;}"));
    composerLayout->addWidget(stop, 0, Qt::AlignBottom);
    stop->hide();
    root->addWidget(composerSurface);

    addChoice(effort, defaultSettingLabel(), QStringLiteral("default"));
    addChoice(effort, QStringLiteral("Minimal"), QStringLiteral("minimal"));
    addChoice(effort, QStringLiteral("Low"), QStringLiteral("low"));
    addChoice(effort, QStringLiteral("Medium"), QStringLiteral("medium"));
    addChoice(effort, QStringLiteral("High"), QStringLiteral("high"));
    addChoice(effort, QStringLiteral("XHigh"), QStringLiteral("xhigh"));
    resetPersonalityChoices(personality);
    resetSandboxChoices(sandbox);
    resetApprovalChoices(approval);
    resetReviewerChoices(reviewer);
    addChoice(serviceTier, defaultSettingLabel(), QStringLiteral("default"));
    resetSummaryChoices(summary);
    resetCollaborationChoices(collaboration);

    connect(model, &QComboBox::currentIndexChanged, this, [this] {
        markComboChange(Field::Model, model);
        refreshModelDependentControls(true);
    });
    connect(model->lineEdit(), &QLineEdit::textEdited, this, [this] {
        markComboChange(Field::Model, model);
        refreshModelDependentControls(true);
    });
    connect(effort, &QComboBox::currentIndexChanged, this, [this] { markComboChange(Field::Effort, effort); });
    connect(personality, &QComboBox::currentIndexChanged, this,
            [this] { markComboChange(Field::Personality, personality); });
    connect(sandbox, &QComboBox::currentIndexChanged, this, [this] { markComboChange(Field::Sandbox, sandbox); });
    connect(approval, &QComboBox::currentIndexChanged, this, [this] { markComboChange(Field::Approval, approval); });
    connect(reviewer, &QComboBox::currentIndexChanged, this, [this] { markComboChange(Field::Reviewer, reviewer); });
    connect(serviceTier, &QComboBox::currentIndexChanged, this,
            [this] { markComboChange(Field::ServiceTier, serviceTier); });
    connect(serviceTier->lineEdit(), &QLineEdit::textEdited, this,
            [this] { markComboChange(Field::ServiceTier, serviceTier); });
    connect(summary, &QComboBox::currentIndexChanged, this, [this] { markComboChange(Field::Summary, summary); });
    connect(collaboration, &QComboBox::currentIndexChanged, this,
            [this] { markComboChange(Field::Collaboration, collaboration); });
    connect(cwd, &QLineEdit::textEdited, this, [this] { markTextChange(Field::Cwd, cwd); });

    connect(editor, &QPlainTextEdit::textChanged, this, [this] {
        if (editor->toPlainText().trimmed().isEmpty())
            draftPromptTarget.reset();
        else
            draftPromptTarget = currentPromptTarget;
        updatePromptHeight();
        updateSendEnabled();
    });
    connect(send, &QPushButton::clicked, this, [this] {
        const QString value = editor->toPlainText();
        if (send->isEnabled() && !value.trimmed().isEmpty())
            emit sendRequested(value, steeringMode);
    });
    connect(stop, &QPushButton::clicked, this, &UpcomingTurnDock::stopRequested);

    refreshControls(true);
    setActionState(false, false, false, false, false, false);
    root->activate();
    compactBaseHeight = std::max(1, root->sizeHint().height());
    setFixedHeight(compactBaseHeight);
    updateGeometry();
}

void UpcomingTurnDock::setCanonicalConfiguration(const std::optional<sdk::ExecutionConfiguration>& configuration,
                                                 const QString& stableThreadIdentity,
                                                 bool useCodexDefaults)
{
    const bool threadChanged = threadIdentity != stableThreadIdentity;
    const bool configurationChanged = canonicalConfiguration != configuration;
    const bool defaultsContextChanged = codexDefaultsContext != useCodexDefaults;
    threadIdentity = stableThreadIdentity;
    canonicalConfiguration = configuration;
    codexDefaultsContext = useCodexDefaults;
    if (threadChanged)
        touchedFields.fill(false);
    if (threadChanged || configurationChanged || defaultsContextChanged)
        refreshControls(threadChanged);
    updateChangedPresentation();
    settingsSurface->setEnabled(controlsContextAllowed);
    more->setEnabled(controlsContextAllowed);
    settingsSurface->setToolTip(
        canonicalConfiguration || codexDefaultsContext
            ? QString{}
            : QStringLiteral("Thread settings are not available in canonical state; only explicit changes will be submitted"));
    const bool cwdAvailable = controlsContextAllowed;
    fieldSurfaces[static_cast<std::size_t>(Field::Cwd)]->setEnabled(cwdAvailable);
    fieldSurfaces[static_cast<std::size_t>(Field::Cwd)]->setToolTip(
        cwdAvailable ? QString{}
                     : QStringLiteral("Workspace cannot be changed in the current thread state"));
    cwd->setPlaceholderText(
        codexDefaultsContext || (canonicalConfiguration && !canonicalConfiguration->cwd)
            ? QStringLiteral("Codex default workspace")
            : canonicalConfiguration
                ? QStringLiteral("Workspace")
                : unavailableSettingLabel());
}

void UpcomingTurnDock::setModelCatalog(const std::vector<typed::Model>& catalog)
{
    std::vector<typed::Model> next;
    next.reserve(catalog.size());
    for (const auto& entry : catalog) {
        if (entry.hidden || entry.model.value.empty())
            continue;
        if (std::ranges::any_of(next, [&entry](const auto& choice) {
                return choice.model.value == entry.model.value;
            }))
            continue;
        next.push_back(entry);
    }
    if (modelCatalog == next)
        return;
    modelCatalog = std::move(next);
    // A catalogue refresh may change display names/capabilities for a pending
    // model selection, so refresh the model row even when that field is dirty.
    refreshModelControl();
    refreshControls(false);
    updateChangedPresentation();
}

UpcomingTurnDraft UpcomingTurnDock::draft() const
{
    UpcomingTurnDraft result;
    result.threadIdentity = threadIdentity;
    for (std::size_t index = 0; index < result.presentationKeys.size(); ++index)
        result.presentationKeys[index] = currentFieldKey(static_cast<Field>(index));

    const auto key = [](const QComboBox* combo) { return combo->currentData().toString(); };
    if (touched(Field::Model))
    {
        const bool selectedCanonicalItem = model->currentIndex() >= 0
            && model->currentText() == model->itemText(model->currentIndex());
        const QString value = selectedCanonicalItem ? model->currentData().toString() : model->currentText().trimmed();
        if (value.isEmpty())
            result.model = typed::OptionalNullable<typed::ModelId>::explicitNull();
        else if (value != QStringLiteral("unavailable"))
            result.model = typed::ModelId{toUtf8(value)};
    }
    if (touched(Field::Effort))
    {
        const QString value = key(effort);
        if (value == QStringLiteral("default"))
            result.effort = typed::OptionalNullable<typed::ReasoningEffort>::explicitNull();
        else if (value != QStringLiteral("unavailable"))
            result.effort = typed::ReasoningEffort{toUtf8(value)};
    }
    if (touched(Field::Personality))
    {
        const QString value = key(personality);
        if (value == QStringLiteral("default"))
            result.personality = typed::OptionalNullable<typed::Personality>::explicitNull();
        else if (typed::Personality{toUtf8(value)}.isKnown())
            result.personality = typed::Personality{toUtf8(value)};
    }
    if (touched(Field::Sandbox)) {
        if (key(sandbox) == QStringLiteral("default"))
            result.sandboxPolicy = typed::OptionalNullable<typed::SandboxPolicy>::explicitNull();
        else if (const auto value = sandboxForKey(key(sandbox)))
            result.sandboxPolicy = *value;
    }
    if (touched(Field::Approval)) {
        if (key(approval) == QStringLiteral("default"))
            result.approvalPolicy = typed::OptionalNullable<typed::AskForApproval>::explicitNull();
        else if (const auto value = approvalForKey(key(approval)))
            result.approvalPolicy = *value;
    }
    if (touched(Field::Reviewer)) {
        const QString value = key(reviewer);
        if (value == QStringLiteral("default"))
            result.approvalsReviewer = typed::OptionalNullable<typed::ApprovalsReviewer>::explicitNull();
        else if (typed::ApprovalsReviewer{toUtf8(value)}.isKnown())
            result.approvalsReviewer = typed::ApprovalsReviewer{toUtf8(value)};
    }
    if (touched(Field::Cwd))
    {
        const QString value = cwd->text().trimmed();
        result.cwd = value.isEmpty() ? typed::OptionalNullable<std::string>::explicitNull() : toUtf8(value);
    }
    if (touched(Field::ServiceTier))
    {
        const int index = serviceTier->currentIndex();
        const bool selectedCanonicalItem = index >= 0
            && serviceTier->currentText() == serviceTier->itemText(index);
        const QString value = selectedCanonicalItem ? serviceTier->currentData().toString()
                                                    : serviceTier->currentText().trimmed();
        if (value.isEmpty() || value == QStringLiteral("default"))
            result.serviceTier = typed::OptionalNullable<std::string>::explicitNull();
        else if (value != QStringLiteral("unavailable"))
            result.serviceTier = toUtf8(value);
    }
    if (touched(Field::Summary))
    {
        const QString value = key(summary);
        if (value == QStringLiteral("default"))
            result.summary = typed::OptionalNullable<typed::ReasoningSummary>::explicitNull();
        else if (typed::ReasoningSummary{toUtf8(value)}.isKnown())
            result.summary = typed::ReasoningSummary{toUtf8(value)};
    }
    if (touched(Field::Collaboration)) {
        const QString collaborationKey = key(collaboration);
        const QString selectedModel = currentFieldKey(Field::Model);
        const QString selectedEffort = currentFieldKey(Field::Effort);
        if (typed::ModeKind{toUtf8(collaborationKey)}.isKnown()
            && !selectedModel.isEmpty() && selectedModel != QStringLiteral("unavailable")
            && !selectedEffort.isEmpty() && selectedEffort != QStringLiteral("unavailable")) {
            result.collaborationMode = collaborationForKey(
                collaborationKey, selectedModel, selectedEffort, canonicalConfiguration);
        }
    }
    return result;
}

bool UpcomingTurnDock::hasSettingsChanges() const noexcept
{
    return std::ranges::any_of(touchedFields, [](bool value) { return value; });
}

void UpcomingTurnDock::clearTouchedSettings()
{
    touchedFields.fill(false);
    refreshControls(true);
    updateChangedPresentation();
    emit settingsChanged();
}

void UpcomingTurnDock::acknowledgeSubmittedSettings(const UpcomingTurnDraft& submitted)
{
    if (submitted.threadIdentity != threadIdentity)
        return;
    resolveSubmittedSettings(submitted);
}

void UpcomingTurnDock::resolveSubmittedSettings(const UpcomingTurnDraft& submitted)
{
    const auto fieldWasSubmitted = [&submitted](Field field) {
        switch (field) {
            case Field::Model:
                return !submitted.model.isOmitted();
            case Field::Effort:
                return !submitted.effort.isOmitted();
            case Field::Personality:
                return !submitted.personality.isOmitted();
            case Field::Sandbox:
                return !submitted.sandboxPolicy.isOmitted();
            case Field::Approval:
                return !submitted.approvalPolicy.isOmitted();
            case Field::Reviewer:
                return !submitted.approvalsReviewer.isOmitted();
            case Field::Cwd:
                return !submitted.cwd.isOmitted();
            case Field::ServiceTier:
                return !submitted.serviceTier.isOmitted();
            case Field::Summary:
                return !submitted.summary.isOmitted();
            case Field::Collaboration:
                return !submitted.collaborationMode.isOmitted();
            case Field::Count:
                break;
        }
        return false;
    };

    bool clearedSubmittedField = false;
    for (std::size_t index = 0; index < submitted.presentationKeys.size(); ++index) {
        const Field field = static_cast<Field>(index);
        if (!fieldWasSubmitted(field)
            || currentFieldKey(field) != submitted.presentationKeys[index])
            continue;
        setTouched(field, false);
        clearedSubmittedField = true;
    }
    if (clearedSubmittedField)
        refreshControls(false);
}

QString UpcomingTurnDock::prompt() const
{
    return editor->toPlainText();
}

void UpcomingTurnDock::clearPrompt()
{
    editor->clear();
}

void UpcomingTurnDock::clearPromptIfUnchanged(const QString& submittedPrompt)
{
    if (editor->toPlainText() == submittedPrompt)
        editor->clear();
}

void UpcomingTurnDock::focusPrompt()
{
    editor->setFocus(Qt::OtherFocusReason);
}

void UpcomingTurnDock::setActionState(bool primaryAllowed,
                                      bool stopAllowed,
                                      bool editorAllowed,
                                      bool settingsAllowed,
                                      bool stopVisible,
                                      bool steerMode,
                                      const QString& actionThreadIdentity,
                                      const QString& activeTurnIdentity)
{
    const bool actionChanged = steeringMode != steerMode;
    sendContextAllowed = primaryAllowed;
    controlsContextAllowed = settingsAllowed;
    steeringMode = steerMode;
    currentPromptTarget = {actionThreadIdentity,
                           steerMode ? activeTurnIdentity : QString{},
                           steerMode};
    editor->setEnabled(editorAllowed);
    settingsSurface->setEnabled(settingsAllowed);
    more->setEnabled(settingsAllowed);
    const bool cwdAvailable = settingsAllowed;
    fieldSurfaces[static_cast<std::size_t>(Field::Cwd)]->setEnabled(cwdAvailable);
    stop->setEnabled(stopAllowed);
    send->setText(steeringMode ? QStringLiteral("Steer") : QStringLiteral("Send"));
    send->setVisible(true);
    stop->setVisible(stopVisible);
    if (actionChanged
        && (status->text() == QStringLiteral("Ctrl+Enter to send")
            || status->text() == QStringLiteral("Ctrl+Enter to steer"))) {
        status->setText(steeringMode ? QStringLiteral("Ctrl+Enter to steer")
                                     : QStringLiteral("Ctrl+Enter to send"));
    }
    updateSendEnabled();
}

void UpcomingTurnDock::setStatus(const QString& text, bool error)
{
    status->setProperty("draftTargetMismatch", false);
    if (text.isEmpty())
    {
        status->setText(steeringMode ? QStringLiteral("Ctrl+Enter to steer")
                                     : QStringLiteral("Ctrl+Enter to send"));
        status->setToolTip({});
        status->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;"));
        return;
    }
    QString visible = text;
    constexpr qsizetype maximumVisibleCharacters = 100;
    if (visible.size() > maximumVisibleCharacters) {
        visible.truncate(maximumVisibleCharacters - 1);
        visible.append(QChar(0x2026));
    }
    status->setText(visible);
    status->setToolTip(Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal));
    status->setStyleSheet(QStringLiteral("color:%1;font-size:10px;").arg(
        error ? QStringLiteral("#b83a3a") : QStringLiteral("#667085")));
}

int UpcomingTurnDock::baseHeight() const noexcept
{
    return compactBaseHeight;
}

bool UpcomingTurnDock::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == editor || watched == editor->viewport())
        && event->type() == QEvent::Resize) {
        // Layout may update the editor and its viewport in separate steps.
        // Re-measure after both geometries have settled so wrapped prompts
        // follow window-width changes without waiting for another keystroke.
        QTimer::singleShot(0, this, [this] { updatePromptHeight(); });
    }
    if (watched == editor)
    {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        {
            composerSurface->setProperty("focused", event->type() == QEvent::FocusIn);
            composerSurface->style()->unpolish(composerSurface);
            composerSurface->style()->polish(composerSurface);
        }
        if (event->type() == QEvent::KeyPress)
        {
            const auto* key = static_cast<QKeyEvent*>(event);
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                && key->modifiers().testFlag(Qt::ControlModifier)
                && !key->isAutoRepeat())
            {
                if (send->isEnabled())
                    emit sendRequested(editor->toPlainText(), steeringMode);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void UpcomingTurnDock::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updatePromptHeight();
}

void UpcomingTurnDock::refreshControls(bool resetAll)
{
    const auto shouldRefresh = [this, resetAll](Field field) { return resetAll || !touched(field); };
    const auto remember = [this](Field field, const QString& key) {
        canonicalKeys[static_cast<std::size_t>(field)] = key;
    };

    const QString missingValue = codexDefaultsContext ? QStringLiteral("default")
                                                      : QStringLiteral("unavailable");
    const typed::Model* catalogDefault = codexDefaultsContext
        ? defaultModelDefinition()
        : nullptr;
    const QString modelValue = canonicalConfiguration
        ? fromUtf8(canonicalConfiguration->model.value)
        : catalogDefault
            ? fromUtf8(catalogDefault->model.value)
            : codexDefaultsContext ? QString{} : missingValue;
    const QString effortValue = canonicalConfiguration ? effortKey(canonicalConfiguration->effort)
                                                       : missingValue;
    const QString personalityValue = canonicalConfiguration ? personalityKey(canonicalConfiguration->personality)
                                                            : missingValue;
    const QString sandboxValue = canonicalConfiguration ? sandboxKey(canonicalConfiguration->sandboxPolicy)
                                                        : missingValue;
    const QString approvalValue = canonicalConfiguration ? approvalKey(canonicalConfiguration->approvalPolicy)
                                                         : missingValue;
    const QString reviewerValue = canonicalConfiguration ? fromUtf8(canonicalConfiguration->approvalsReviewer.value)
                                                         : missingValue;
    const QString cwdValue = canonicalConfiguration && canonicalConfiguration->cwd
        ? fromUtf8(canonicalConfiguration->cwd->value)
        : QString{};
    const QString serviceTierValue = canonicalConfiguration && canonicalConfiguration->serviceTier.hasValue()
        ? fromUtf8(*canonicalConfiguration->serviceTier)
        : canonicalConfiguration ? QStringLiteral("default") : missingValue;
    const QString summaryValue = canonicalConfiguration ? summaryKey(canonicalConfiguration->summary)
                                                        : missingValue;
    const QString collaborationValue = canonicalConfiguration ? collaborationKey(canonicalConfiguration->collaborationMode)
                                                              : missingValue;
    const std::array<QString, static_cast<std::size_t>(Field::Count)> nextCanonicalKeys{
        modelValue,
        effortValue,
        personalityValue,
        sandboxValue,
        approvalValue,
        reviewerValue,
        cwdValue,
        serviceTierValue,
        summaryValue,
        collaborationValue,
    };
    for (std::size_t index = 0; index < nextCanonicalKeys.size(); ++index)
    {
        const Field field = static_cast<Field>(index);
        remember(field, nextCanonicalKeys[index]);
        if (touched(field) && currentFieldKey(field) == nextCanonicalKeys[index])
            setTouched(field, false);
    }

    const bool editableSandbox = !canonicalConfiguration
        || sandboxIsEditable(canonicalConfiguration->sandboxPolicy);
    const bool editableApproval = !canonicalConfiguration
        || approvalIsEditable(canonicalConfiguration->approvalPolicy);
    if (!editableSandbox)
        setTouched(Field::Sandbox, false);
    if (!editableApproval)
        setTouched(Field::Approval, false);

    if (shouldRefresh(Field::Model))
        refreshModelControl();
    if (shouldRefresh(Field::Effort))
    {
        const QSignalBlocker blocker(effort);
        selectKey(effort, effortValue,
                  effortValue == QStringLiteral("default") ? defaultSettingLabel()
                  : effortValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                  : friendlyValue(effortValue),
                  effortValue == QStringLiteral("default")
                      || typed::ReasoningEffort{toUtf8(effortValue)}.isKnown());
    }
    if (shouldRefresh(Field::Personality))
    {
        const QSignalBlocker blocker(personality);
        resetPersonalityChoices(personality);
        selectKey(personality, personalityValue,
                  personalityValue == QStringLiteral("default") ? defaultSettingLabel()
                  : personalityValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                       : friendlyValue(personalityValue),
                  personalityValue == QStringLiteral("default")
                      || typed::Personality{toUtf8(personalityValue)}.isKnown());
    }
    if (shouldRefresh(Field::Sandbox))
    {
        const QSignalBlocker blocker(sandbox);
        resetSandboxChoices(sandbox);
        selectKey(sandbox, sandboxValue,
                  sandboxValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                 : friendlyValue(sandboxValue),
                  sandboxValue == QStringLiteral("default") || sandboxForKey(sandboxValue).has_value());
    }
    if (shouldRefresh(Field::Approval))
    {
        const QSignalBlocker blocker(approval);
        resetApprovalChoices(approval);
        selectKey(approval, approvalValue,
                  approvalValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                  : friendlyValue(approvalValue),
                  approvalValue == QStringLiteral("default") || approvalForKey(approvalValue).has_value());
    }
    if (shouldRefresh(Field::Reviewer))
    {
        const QSignalBlocker blocker(reviewer);
        resetReviewerChoices(reviewer);
        selectKey(reviewer, reviewerValue,
                  reviewerValue == QStringLiteral("default") ? defaultSettingLabel()
                  : reviewerValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                    : friendlyValue(reviewerValue),
                  reviewerValue == QStringLiteral("default")
                      || typed::ApprovalsReviewer{toUtf8(reviewerValue)}.isKnown());
    }
    if (shouldRefresh(Field::Cwd))
    {
        const QSignalBlocker blocker(cwd);
        cwd->setText(cwdValue);
    }
    if (shouldRefresh(Field::ServiceTier))
    {
        const QSignalBlocker blocker(serviceTier);
        selectKey(serviceTier,
                  serviceTierValue,
                  serviceTierValue == QStringLiteral("default") ? defaultSettingLabel()
                  : serviceTierValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                      : serviceTierValue,
                  serviceTierValue != QStringLiteral("unavailable"));
    }
    if (shouldRefresh(Field::Summary))
    {
        const QSignalBlocker blocker(summary);
        resetSummaryChoices(summary);
        selectKey(summary,
                  summaryValue,
                  summaryValue == QStringLiteral("default") ? defaultSettingLabel()
                  : summaryValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                  : friendlyValue(summaryValue),
                  summaryValue == QStringLiteral("default")
                      || typed::ReasoningSummary{toUtf8(summaryValue)}.isKnown());
    }
    if (shouldRefresh(Field::Collaboration))
    {
        const QSignalBlocker blocker(collaboration);
        resetCollaborationChoices(collaboration);
        selectKey(collaboration, collaborationValue,
                  collaborationValue == QStringLiteral("default") ? defaultSettingLabel()
                  : collaborationValue == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                        : friendlyValue(collaborationValue),
                  typed::ModeKind{toUtf8(collaborationValue)}.isKnown());
    }
    refreshModelDependentControls(false);
    fieldSurfaces[static_cast<std::size_t>(Field::Sandbox)]->setEnabled(
        editableSandbox);
    fieldSurfaces[static_cast<std::size_t>(Field::Sandbox)]->setToolTip(
        editableSandbox ? QString{}
                        : QStringLiteral("This projected access policy is read-only in CodexUI"));
    fieldSurfaces[static_cast<std::size_t>(Field::Approval)]->setEnabled(
        editableApproval);
    fieldSurfaces[static_cast<std::size_t>(Field::Approval)]->setToolTip(
        editableApproval ? QString{}
                         : QStringLiteral("This projected approval policy is read-only in CodexUI"));
}

void UpcomingTurnDock::refreshModelControl()
{
    const QString selectedKey = currentFieldKey(Field::Model);
    const int selectedIndex = model->currentIndex();
    const bool selectedCatalogEntry = selectedIndex >= 0
        && model->currentText() == model->itemText(selectedIndex);
    const QString canonicalKey = canonicalKeys[static_cast<std::size_t>(Field::Model)];
    const QString targetKey = touched(Field::Model) ? selectedKey : canonicalKey;

    const QSignalBlocker blocker(model);
    model->clear();
    for (const auto& choice : modelCatalog) {
        const QString key = fromUtf8(choice.model.value);
        QString display = choice.displayName.empty() ? key : fromUtf8(choice.displayName);
        model->addItem(display, key);
    }
    int targetIndex = model->findData(targetKey);
    if (targetIndex < 0) {
        const QString display = targetKey == QStringLiteral("unavailable")
            ? unavailableSettingLabel()
            : targetKey.isEmpty() ? defaultSettingLabel() : targetKey;
        model->addItem(display, targetKey);
        targetIndex = model->count() - 1;
        if (targetKey == QStringLiteral("unavailable")) {
            if (auto* itemModel = qobject_cast<QStandardItemModel*>(model->model())) {
                if (QStandardItem* item = itemModel->item(targetIndex))
                    item->setFlags(item->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
            }
        }
    }
    model->setCurrentIndex(targetIndex);
    const typed::Model* advertisedDefault = defaultModelDefinition();
    const bool showingAdvertisedDefault = codexDefaultsContext
        && advertisedDefault
        && model->currentData().toString() == fromUtf8(advertisedDefault->model.value);
    model->setToolTip(showingAdvertisedDefault
                          ? QStringLiteral("Codex default model")
                          : QString{});
    if (touched(Field::Model) && !selectedCatalogEntry && !selectedKey.isEmpty()) {
        model->setEditText(selectedKey);
        model->lineEdit()->setCursorPosition(static_cast<int>(selectedKey.size()));
    } else {
        model->lineEdit()->setCursorPosition(0);
        model->lineEdit()->deselect();
    }
}

const typed::Model* UpcomingTurnDock::defaultModelDefinition() const
{
    const auto match = std::ranges::find_if(modelCatalog, [](const auto& candidate) {
        return candidate.isDefault;
    });
    return match == modelCatalog.end() ? nullptr : &*match;
}

const typed::Model* UpcomingTurnDock::selectedModelDefinition() const
{
    const std::string selected = toUtf8(currentFieldKey(Field::Model));
    const auto match = std::ranges::find_if(modelCatalog, [&selected](const auto& candidate) {
        return candidate.model.value == selected;
    });
    return match == modelCatalog.end() ? nullptr : &*match;
}

void UpcomingTurnDock::refreshModelDependentControls(bool modelChangedByUser)
{
    const typed::Model* definition = selectedModelDefinition();
    QString defaultEffortText = defaultSettingLabel();
    if (definition && !definition->defaultReasoningEffort.value.empty())
        defaultEffortText = friendlyValue(fromUtf8(definition->defaultReasoningEffort.value))
            + QStringLiteral(" · default");

    QString effortTarget = currentFieldKey(Field::Effort);
    const bool constrainedEfforts = definition && !definition->supportedReasoningEfforts.empty();
    const auto supportsEffort = [definition](const QString& key) {
        return definition && std::ranges::any_of(
            definition->supportedReasoningEfforts,
            [&key](const auto& option) {
                return fromUtf8(option.reasoningEffort.value) == key;
            });
    };
    if (modelChangedByUser && effortTarget == QStringLiteral("unavailable")) {
        effortTarget = definition && !definition->defaultReasoningEffort.value.empty()
            ? fromUtf8(definition->defaultReasoningEffort.value)
            : QStringLiteral("default");
    }
    if (modelChangedByUser && constrainedEfforts
        && effortTarget != QStringLiteral("default") && !supportsEffort(effortTarget)) {
        effortTarget = fromUtf8(definition->defaultReasoningEffort.value);
    }
    {
        const QSignalBlocker blocker(effort);
        effort->clear();
        addChoice(effort, defaultEffortText, QStringLiteral("default"));
        if (constrainedEfforts) {
            for (const auto& option : definition->supportedReasoningEfforts) {
                const QString key = fromUtf8(option.reasoningEffort.value);
                addChoice(effort, friendlyValue(key), key);
            }
        } else {
            addChoice(effort, QStringLiteral("Minimal"), QStringLiteral("minimal"));
            addChoice(effort, QStringLiteral("Low"), QStringLiteral("low"));
            addChoice(effort, QStringLiteral("Medium"), QStringLiteral("medium"));
            addChoice(effort, QStringLiteral("High"), QStringLiteral("high"));
            addChoice(effort, QStringLiteral("XHigh"), QStringLiteral("xhigh"));
        }
        selectKey(effort, effortTarget,
                  effortTarget == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                 : friendlyValue(effortTarget),
                  effortTarget == QStringLiteral("default") || supportsEffort(effortTarget)
                      || typed::ReasoningEffort{toUtf8(effortTarget)}.isKnown());
    }
    if (modelChangedByUser)
        setTouched(Field::Effort,
                   effortTarget != canonicalKeys[static_cast<std::size_t>(Field::Effort)]);

    QString tierTarget = currentFieldKey(Field::ServiceTier);
    std::vector<std::pair<QString, QString>> tiers;
    if (definition) {
        tiers.reserve(definition->serviceTiers.size() + definition->additionalSpeedTiers.size());
        for (const auto& tier : definition->serviceTiers) {
            const QString key = fromUtf8(tier.id.value);
            const QString display = tier.name.empty() ? friendlyValue(key) : fromUtf8(tier.name);
            if (!std::ranges::any_of(tiers, [&key](const auto& item) { return item.second == key; }))
                tiers.emplace_back(display, key);
        }
        for (const auto& tier : definition->additionalSpeedTiers) {
            const QString key = fromUtf8(tier.value);
            if (!std::ranges::any_of(tiers, [&key](const auto& item) { return item.second == key; }))
                tiers.emplace_back(friendlyValue(key), key);
        }
    }
    const bool constrainedTiers = !tiers.empty();
    const auto supportsTier = [&tiers](const QString& key) {
        return std::ranges::any_of(tiers, [&key](const auto& item) { return item.second == key; });
    };
    if (modelChangedByUser && constrainedTiers
        && tierTarget != QStringLiteral("default") && !supportsTier(tierTarget)) {
        tierTarget = definition->defaultServiceTier.hasValue()
            ? fromUtf8(definition->defaultServiceTier->value)
            : QStringLiteral("default");
    }
    {
        const QSignalBlocker blocker(serviceTier);
        serviceTier->clear();
        QString defaultTierText = defaultSettingLabel();
        if (definition && definition->defaultServiceTier.hasValue())
            defaultTierText = friendlyValue(fromUtf8(definition->defaultServiceTier->value))
                + QStringLiteral(" · default");
        addChoice(serviceTier, defaultTierText, QStringLiteral("default"));
        for (const auto& [display, key] : tiers)
            addChoice(serviceTier, display, key);
        selectKey(serviceTier,
                  tierTarget,
                  tierTarget == QStringLiteral("default") ? defaultTierText
                  : tierTarget == QStringLiteral("unavailable") ? unavailableSettingLabel()
                                                                 : friendlyValue(tierTarget),
                  tierTarget != QStringLiteral("unavailable"));
        serviceTier->lineEdit()->setReadOnly(constrainedTiers);
    }
    if (modelChangedByUser)
        setTouched(Field::ServiceTier,
                   tierTarget != canonicalKeys[static_cast<std::size_t>(Field::ServiceTier)]);

    const bool personalityUnsupported = definition && !definition->supportsPersonality;
    if (modelChangedByUser && personalityUnsupported) {
        const QSignalBlocker blocker(personality);
        selectKey(personality, QStringLiteral("default"), defaultSettingLabel());
        setTouched(Field::Personality,
                   canonicalKeys[static_cast<std::size_t>(Field::Personality)]
                       != QStringLiteral("default"));
    }
    fieldSurfaces[static_cast<std::size_t>(Field::Personality)]->setEnabled(
        !personalityUnsupported);
    fieldSurfaces[static_cast<std::size_t>(Field::Personality)]->setToolTip(
        personalityUnsupported
            ? QStringLiteral("The selected model does not support personality settings")
            : QString{});

    const QString selectedModel = currentFieldKey(Field::Model);
    const QString selectedEffort = currentFieldKey(Field::Effort);
    const bool collaborationAvailable = !selectedModel.isEmpty()
        && selectedModel != QStringLiteral("unavailable")
        && !selectedEffort.isEmpty() && selectedEffort != QStringLiteral("unavailable")
        && (canonicalConfiguration || codexDefaultsContext || touched(Field::Model));
    if (!collaborationAvailable)
        setTouched(Field::Collaboration, false);
    fieldSurfaces[static_cast<std::size_t>(Field::Collaboration)]->setEnabled(
        collaborationAvailable);
    fieldSurfaces[static_cast<std::size_t>(Field::Collaboration)]->setToolTip(
        collaborationAvailable
            ? QString{}
            : QStringLiteral("Choose a model and reasoning effort before changing collaboration mode"));
}

void UpcomingTurnDock::updatePromptHeight()
{
    if (!editor || editor->viewport()->width() <= 0)
        return;

    editor->document()->setTextWidth(editor->viewport()->width());
    qreal laidOutHeight = 0;
    QAbstractTextDocumentLayout* documentLayout = editor->document()->documentLayout();
    for (QTextBlock block = editor->document()->begin(); block.isValid(); block = block.next())
        laidOutHeight += documentLayout->blockBoundingRect(block).height();
    const int documentHeight = static_cast<int>(std::ceil(laidOutHeight)) + 10;
    const int wanted = std::clamp(documentHeight, oneLineEditorHeight, maximumEditorHeight);
    editor->setVerticalScrollBarPolicy(wanted >= maximumEditorHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    if (wanted == currentEditorHeight)
        return;
    currentEditorHeight = wanted;
    editor->setFixedHeight(wanted);
    const int wantedDockHeight = compactBaseHeight + wanted - oneLineEditorHeight;
    setFixedHeight(wantedDockHeight);
    updateGeometry();
    emit dockHeightChanged(wantedDockHeight);
}

void UpcomingTurnDock::updateSendEnabled()
{
    const bool hasPrompt = !editor->toPlainText().trimmed().isEmpty();
    const bool actionMatchesDraft = !draftPromptTarget || *draftPromptTarget == currentPromptTarget;
    send->setEnabled(sendContextAllowed && editor->isEnabled() && hasPrompt
                     && actionMatchesDraft);
    const QString mismatch = actionMatchesDraft
                                 ? QString{}
                                 : QStringLiteral(
                                       "The turn state changed while this draft was open. Edit the draft to confirm its new action.");
    send->setToolTip(mismatch);
    editor->setToolTip(mismatch);
    if (!actionMatchesDraft) {
        const QString retained = QStringLiteral("Draft retained for a previous active turn; edit it to retarget");
        status->setProperty("draftTargetMismatch", true);
        status->setText(retained);
        status->setToolTip(Qt::convertFromPlainText(retained, Qt::WhiteSpaceNormal));
        status->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;"));
    } else if (status->property("draftTargetMismatch").toBool()) {
        setStatus({});
    }
}

void UpcomingTurnDock::updateChangedPresentation()
{
    const auto applyChanged = [](QWidget* widget, bool changed) {
        if (!widget || widget->property("changed").toBool() == changed)
            return;
        widget->setProperty("changed", changed);
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    };
    for (std::size_t index = 0; index < fieldSurfaces.size(); ++index)
        applyChanged(fieldSurfaces[index], touchedFields[index]);

    const bool advancedChanged = touched(Field::Reviewer) || touched(Field::ServiceTier)
        || touched(Field::Summary) || touched(Field::Collaboration);
    applyChanged(more, advancedChanged);
    settingsHint->setText(hasSettingsChanges()
                              ? QStringLiteral("Changed values apply to this and subsequent turns")
                              : QString{});
}

void UpcomingTurnDock::markComboChange(Field field, QComboBox* combo)
{
    QString current = combo->currentData().toString();
    if (combo->isEditable())
    {
        const int index = combo->currentIndex();
        const bool matchesSelectedItem = index >= 0 && combo->currentText() == combo->itemText(index);
        if (!matchesSelectedItem)
            current = combo->currentText().trimmed();
    }
    setTouched(field, current != canonicalKeys[static_cast<std::size_t>(field)]);
}

void UpcomingTurnDock::markTextChange(Field field, QLineEdit* edit)
{
    setTouched(field, edit->text().trimmed() != canonicalKeys[static_cast<std::size_t>(field)]);
}

QString UpcomingTurnDock::currentFieldKey(Field field) const
{
    const auto comboKey = [](const QComboBox* combo) {
        if (combo->isEditable())
        {
            const int index = combo->currentIndex();
            const bool matchesSelectedItem = index >= 0 && combo->currentText() == combo->itemText(index);
            if (!matchesSelectedItem)
                return combo->currentText().trimmed();
        }
        return combo->currentData().toString();
    };
    switch (field)
    {
        case Field::Model:
            return comboKey(model);
        case Field::Effort:
            return comboKey(effort);
        case Field::Personality:
            return comboKey(personality);
        case Field::Sandbox:
            return comboKey(sandbox);
        case Field::Approval:
            return comboKey(approval);
        case Field::Reviewer:
            return comboKey(reviewer);
        case Field::Cwd:
            return cwd->text().trimmed();
        case Field::ServiceTier:
            return comboKey(serviceTier);
        case Field::Summary:
            return comboKey(summary);
        case Field::Collaboration:
            return comboKey(collaboration);
        case Field::Count:
            break;
    }
    return {};
}

bool UpcomingTurnDock::touched(Field field) const noexcept
{
    return touchedFields[static_cast<std::size_t>(field)];
}

void UpcomingTurnDock::setTouched(Field field, bool value)
{
    const auto index = static_cast<std::size_t>(field);
    if (touchedFields[index] == value)
        return;
    touchedFields[index] = value;
    updateChangedPresentation();
    emit settingsChanged();
}

AnchoredTurnSurface::AnchoredTurnSurface(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("anchoredTurnSurface"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#anchoredTurnSurface{background:#f6f8fb;}"));
}

void AnchoredTurnSurface::setConversationWidget(QWidget* widget)
{
    if (conversation == widget)
        return;
    if (conversation)
        conversation->removeEventFilter(this);
    conversation = widget;
    if (conversation)
    {
        conversation->setParent(this);
        conversation->installEventFilter(this);
        conversation->show();
        conversation->lower();
    }
    relayout();
}

void AnchoredTurnSurface::setUpcomingTurnDock(UpcomingTurnDock* widget)
{
    if (dock == widget)
        return;
    if (dock)
        dock->removeEventFilter(this);
    dock = widget;
    if (dock)
    {
        dock->setParent(this);
        dock->installEventFilter(this);
        dock->show();
        dock->raise();
        connect(dock, &UpcomingTurnDock::dockHeightChanged, this, [this] { relayout(); });
    }
    relayout();
}

bool AnchoredTurnSurface::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == dock || watched == conversation)
        && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize || event->type() == QEvent::Show))
        relayout();
    return QWidget::eventFilter(watched, event);
}

void AnchoredTurnSurface::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
}

void AnchoredTurnSurface::relayout()
{
    const int base = dock ? dock->baseHeight() : 0;
    if (conversation)
        conversation->setGeometry(0, 0, width(), std::max(0, height() - base));
    if (dock)
    {
        const int dockHeight = std::min(height(), std::max(base, dock->height()));
        dock->setGeometry(0, height() - dockHeight, width(), dockHeight);
        dock->raise();
    }
}

} // namespace codexui
