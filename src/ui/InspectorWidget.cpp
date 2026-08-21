// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/InspectorWidget.h"

#include <QFrame>
#include <QCryptographicHash>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace codexui {
namespace {
namespace sdk = ai::openai::codex::frontend::client;
namespace typed = ai::openai::codex::typed;

struct AgentPresentation
{
    QStringList itemIds;
    QString agentPath;
    QString agentThreadId;
    QString kind;
    QString status;
    QString summary;
    QString duration;
};

struct CollaborationPresentation
{
    QString title;
    QString status;
    QString detail;
    bool truncated = false;
};

struct ExecutionConfigurationPresentation
{
    bool recorded = false;
    QString turnId;
    QString unavailableDetail;
    QString model;
    QString effort;
    QString personality;
    QString workspace;
    QString sandbox;
    QString approvalPolicy;
    QString approvalsReviewer;
    QString serviceTier;
    QString summary;
    QString collaborationMode;
    QString activePermissionProfile;
    QString provenance;
};

QString fromUtf8(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString fromUtf8(const std::string& value)
{
    return QString::fromStdString(value);
}

QString humanize(QString value)
{
    value.replace(QLatin1Char('_'), QLatin1Char(' '));
    value.replace(QLatin1Char('-'), QLatin1Char(' '));
    for (qsizetype index = 1; index < value.size(); ++index) {
        if (value.at(index).isUpper() && value.at(index - 1).isLower()) {
            value.insert(index, QLatin1Char(' '));
            ++index;
        }
    }
    if (!value.isEmpty())
        value[0] = value.at(0).toUpper();
    return value;
}

QString compactId(const std::string& id)
{
    const QString value = fromUtf8(id);
    return value.size() > 18 ? value.left(8) + QChar(0x2026) + value.right(7) : value;
}

QString compact(QString value, qsizetype maximum = 500)
{
    value = value.trimmed();
    return value.size() > maximum ? value.left(maximum).trimmed() + QChar(0x2026) : value;
}

QLabel* textLabel(const QString& text, const char* kind = nullptr)
{
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    if (kind)
        result->setProperty("kind", kind);
    return result;
}

QFrame* divider()
{
    auto* line = new QFrame;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:#d7dee8;"));
    return line;
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QLayout* child = item->layout()) {
            clearLayout(child);
        } else if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

QWidget* scrollPage(QVBoxLayout*& content)
{
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget;
    page->setStyleSheet(QStringLiteral("background:transparent;"));
    content = new QVBoxLayout(page);
    content->setContentsMargins(0, 14, 0, 24);
    content->setSpacing(0);
    content->setAlignment(Qt::AlignTop);
    scroll->setWidget(page);
    return scroll;
}

void addEmpty(QVBoxLayout* layout, const QString& title, const QString& detail)
{
    layout->addWidget(textLabel(title.toUpper(), "section"));
    layout->addSpacing(8);
    auto* card = new QFrame;
    card->setProperty("kind", "raised");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 14, 14, 14);
    cardLayout->setSpacing(6);
    auto* heading = textLabel(title);
    heading->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;"));
    cardLayout->addWidget(heading);
    auto* copy = textLabel(detail, "muted");
    copy->setWordWrap(true);
    cardLayout->addWidget(copy);
    layout->addWidget(card);
    layout->addStretch();
}

QString statusColor(const QString& status)
{
    const QString normalized = status.toLower();
    if (normalized.contains(QStringLiteral("fail")) || normalized.contains(QStringLiteral("error")))
        return QStringLiteral("#b83a3a");
    if (normalized.contains(QStringLiteral("complete")) || normalized.contains(QStringLiteral("success"))
        || normalized == QStringLiteral("done"))
        return QStringLiteral("#23845a");
    if (normalized.contains(QStringLiteral("progress")) || normalized.contains(QStringLiteral("running"))
        || normalized.contains(QStringLiteral("active")) || normalized.contains(QStringLiteral("stream")))
        return QStringLiteral("#2f6feb");
    if (normalized.contains(QStringLiteral("interrupt")) || normalized.contains(QStringLiteral("cancel")))
        return QStringLiteral("#a76812");
    return QStringLiteral("#667085");
}

QString planStatusGlyph(const QString& status)
{
    const QString normalized = status.toLower();
    if (normalized.contains(QStringLiteral("complete")))
        return QStringLiteral("✓");
    if (normalized.contains(QStringLiteral("progress")))
        return QStringLiteral("●");
    return QStringLiteral("○");
}

QString itemStatus(const sdk::ItemState& item)
{
    return item.status && !item.status->empty() ? humanize(fromUtf8(*item.status)) : QString{};
}

QString durationText(const sdk::ItemState& item)
{
    if (!item.startedAtMs || !item.completedAtMs || *item.completedAtMs < *item.startedAtMs)
        return {};
    const qint64 duration = *item.completedAtMs - *item.startedAtMs;
    if (duration < 1000)
        return QStringLiteral("%1 ms").arg(duration);
    if (duration < 60000)
        return QStringLiteral("%1 s").arg(QString::number(duration / 1000.0, 'f', 1));
    return QStringLiteral("%1 min").arg(QString::number(duration / 60000.0, 'f', 1));
}

const sdk::TurnState* latestTurn(const sdk::State& state, const sdk::ThreadState& thread)
{
    for (auto iterator = thread.orderedTurns.rbegin(); iterator != thread.orderedTurns.rend(); ++iterator) {
        if (const auto* turn = state.turn(thread.id, *iterator))
            return turn;
    }
    return nullptr;
}

QFrame* detailCard()
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("inspectorDetailCard"));
    card->setProperty("kind", "raised");
    card->setStyleSheet(QStringLiteral(
        "QFrame#inspectorDetailCard{background:#ffffff;border:1px solid #d7dee8;border-radius:8px;}"));
    return card;
}

QFrame* executionConfigurationCard()
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("turnExecutionConfigurationCard"));
    card->setStyleSheet(QStringLiteral(
        "QFrame#turnExecutionConfigurationCard{background:#f8fafc;border:1px solid #d7dee8;border-radius:8px;}"));
    return card;
}

QLabel* addFact(QGridLayout* grid, int& row, const QString& name, const QString& value,
                const QString& color = QStringLiteral("#1d2633"))
{
    if (value.isEmpty())
        return nullptr;
    grid->addWidget(textLabel(name, "meta"), row, 0, Qt::AlignTop);
    auto* copy = textLabel(value);
    copy->setWordWrap(true);
    copy->setTextInteractionFlags(Qt::TextSelectableByMouse);
    copy->setStyleSheet(QStringLiteral("color:%1;font-size:11px;font-weight:500;").arg(color));
    grid->addWidget(copy, row, 1);
    ++row;
    return copy;
}

void addExecutionConfigurationFact(QGridLayout* grid, int& row, const QString& name, const QString& value)
{
    if (value.isEmpty())
        return;
    auto* label = textLabel(name);
    label->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;font-weight:600;"));
    grid->addWidget(label, row, 0, Qt::AlignTop);
    auto* copy = textLabel(value);
    copy->setWordWrap(true);
    copy->setTextInteractionFlags(Qt::TextSelectableByMouse);
    copy->setStyleSheet(QStringLiteral("color:#1d2633;font-size:11px;font-weight:500;"));
    grid->addWidget(copy, row, 1);
    ++row;
}

template<typename T>
QString optionalOpenValueText(const typed::OptionalNullable<T>& value)
{
    if (value.isOmitted())
        return {};
    if (value.isNull())
        return QStringLiteral("Default");
    return humanize(fromUtf8(value->value));
}

QString optionalStringText(const typed::OptionalNullable<std::string>& value)
{
    if (value.isOmitted())
        return {};
    if (value.isNull())
        return QStringLiteral("Default");
    return fromUtf8(*value);
}

QString approvalPolicyText(const typed::AskForApproval& policy)
{
    if (const auto* scalar = std::get_if<typed::ApprovalPolicy>(&policy))
        return humanize(fromUtf8(scalar->value));
    if (const auto* granular = std::get_if<typed::GranularAskForApproval>(&policy)) {
        QStringList enabled;
        if (granular->granular.mcpElicitations)
            enabled.append(QStringLiteral("MCP elicitations"));
        if (granular->granular.requestPermissionsOrDefault())
            enabled.append(QStringLiteral("permission requests"));
        if (granular->granular.rules)
            enabled.append(QStringLiteral("rules"));
        if (granular->granular.sandboxApproval)
            enabled.append(QStringLiteral("sandbox escalation"));
        if (granular->granular.skillApprovalOrDefault())
            enabled.append(QStringLiteral("skill approval"));
        return enabled.isEmpty() ? QStringLiteral("Granular · No categories enabled")
                                 : QStringLiteral("Granular · %1").arg(enabled.join(QStringLiteral(", ")));
    }
    const auto* unknown = std::get_if<typed::UnknownAskForApproval>(&policy);
    return unknown && unknown->discriminator
             ? QStringLiteral("Unrecognized · %1").arg(humanize(fromUtf8(*unknown->discriminator)))
             : QStringLiteral("Unrecognized policy");
}

QString sandboxPolicyText(const typed::SandboxPolicy& policy)
{
    if (std::holds_alternative<typed::DangerFullAccessSandboxPolicy>(policy))
        return QStringLiteral("Danger full access");
    if (const auto* readOnly = std::get_if<typed::ReadOnlySandboxPolicy>(&policy))
        return readOnly->networkAccessOrDefault() ? QStringLiteral("Read only · Network enabled")
                                                  : QStringLiteral("Read only · Network restricted");
    if (const auto* external = std::get_if<typed::ExternalSandboxPolicy>(&policy))
        return QStringLiteral("External · Network %1")
            .arg(humanize(fromUtf8(external->networkAccessOrDefault().value)).toLower());
    if (const auto* workspace = std::get_if<typed::WorkspaceWriteSandboxPolicy>(&policy))
        return workspace->networkAccessOrDefault() ? QStringLiteral("Workspace write · Network enabled")
                                                   : QStringLiteral("Workspace write · Network restricted");
    const auto* unknown = std::get_if<typed::UnknownSandboxPolicy>(&policy);
    return unknown && unknown->type
             ? QStringLiteral("Unrecognized · %1").arg(humanize(fromUtf8(*unknown->type)))
             : QStringLiteral("Unrecognized sandbox policy");
}

QString activePermissionProfileText(const typed::OptionalNullable<typed::ActivePermissionProfile>& profile)
{
    if (profile.isOmitted())
        return {};
    if (profile.isNull())
        return QStringLiteral("None");
    QString result = fromUtf8(profile->id);
    if (profile->extends.hasValue())
        result += QStringLiteral(" · Extends %1").arg(fromUtf8(*profile->extends));
    return result;
}

QString executionConfigurationProvenanceText(
    std::optional<sdk::EffectiveExecutionConfigurationProvenance> provenance)
{
    if (!provenance)
        return QStringLiteral("Authoritative turn state");
    switch (*provenance) {
        case sdk::EffectiveExecutionConfigurationProvenance::TurnStartAccepted:
            return QStringLiteral("Recorded when turn start was accepted");
        case sdk::EffectiveExecutionConfigurationProvenance::ThreadSettingsUpdated:
            return QStringLiteral("Confirmed by thread settings update");
    }
    return QStringLiteral("Authoritative turn state");
}

ExecutionConfigurationPresentation executionConfigurationPresentation(const sdk::TurnState* turn,
                                                                       QString unavailableDetail)
{
    ExecutionConfigurationPresentation result;
    result.unavailableDetail = std::move(unavailableDetail);
    if (!turn)
        return result;
    result.turnId = fromUtf8(turn->id.value);
    if (!turn->effectiveExecutionConfiguration)
        return result;

    const auto& configuration = *turn->effectiveExecutionConfiguration;
    result.recorded = true;
    result.model = fromUtf8(configuration.model.value);
    result.effort = optionalOpenValueText(configuration.effort);
    result.personality = optionalOpenValueText(configuration.personality);
    result.workspace = configuration.cwd ? fromUtf8(configuration.cwd->value) : QString{};
    result.sandbox = sandboxPolicyText(configuration.sandboxPolicy);
    result.approvalPolicy = approvalPolicyText(configuration.approvalPolicy);
    result.approvalsReviewer = humanize(fromUtf8(configuration.approvalsReviewer.value));
    result.serviceTier = optionalStringText(configuration.serviceTier);
    result.summary = optionalOpenValueText(configuration.summary);
    result.collaborationMode = humanize(fromUtf8(configuration.collaborationMode.mode.value));
    result.activePermissionProfile = activePermissionProfileText(configuration.activePermissionProfile);
    result.provenance = executionConfigurationProvenanceText(
        turn->effectiveExecutionConfigurationProvenance);
    return result;
}

QString freshnessText(sdk::StateFreshness freshness)
{
    switch (freshness) {
        case sdk::StateFreshness::Current:
            return QStringLiteral("Current");
        case sdk::StateFreshness::Stale:
            return QStringLiteral("Stale");
        case sdk::StateFreshness::Synchronizing:
            return QStringLiteral("Synchronizing");
    }
    return QStringLiteral("Unavailable");
}

QString representationText(sdk::RepresentationMode mode)
{
    switch (mode) {
        case sdk::RepresentationMode::LegacyV1:
            return QStringLiteral("Legacy v1");
        case sdk::RepresentationMode::ExpandedV1:
            return QStringLiteral("Expanded v1");
        case sdk::RepresentationMode::Unknown:
            break;
    }
    return QStringLiteral("Unknown");
}

QString providerLifecycleText(sdk::ProviderLifecycle lifecycle)
{
    switch (lifecycle) {
        case sdk::ProviderLifecycle::Stopped:
            return QStringLiteral("Stopped");
        case sdk::ProviderLifecycle::Starting:
            return QStringLiteral("Starting");
        case sdk::ProviderLifecycle::Initializing:
            return QStringLiteral("Initializing");
        case sdk::ProviderLifecycle::Ready:
            return QStringLiteral("Ready");
        case sdk::ProviderLifecycle::Stopping:
            return QStringLiteral("Stopping");
        case sdk::ProviderLifecycle::Failed:
            return QStringLiteral("Failed");
        case sdk::ProviderLifecycle::Recovering:
            return QStringLiteral("Recovering");
    }
    return {};
}

QString tokenCountsText(const sdk::TokenCountsView& counts)
{
    QStringList values;
    if (counts.inputTokens)
        values.append(QStringLiteral("%1 input").arg(*counts.inputTokens));
    if (counts.outputTokens)
        values.append(QStringLiteral("%1 output").arg(*counts.outputTokens));
    if (counts.reasoningOutputTokens)
        values.append(QStringLiteral("%1 reasoning").arg(*counts.reasoningOutputTokens));
    if (counts.cachedInputTokens)
        values.append(QStringLiteral("%1 cached").arg(*counts.cachedInputTokens));
    if (counts.totalTokens)
        values.append(QStringLiteral("%1 total").arg(*counts.totalTokens));
    return values.join(QStringLiteral(" · "));
}

QString tokenUsageText(const sdk::TurnState& turn)
{
    const auto usage = sdk::tokenUsageView(turn);
    if (!usage)
        return {};
    QStringList values;
    if (usage->total) {
        const QString total = tokenCountsText(*usage->total);
        if (!total.isEmpty())
            values.append(total);
    } else if (usage->last) {
        const QString last = tokenCountsText(*usage->last);
        if (!last.isEmpty())
            values.append(QStringLiteral("Latest: %1").arg(last));
    }
    if (usage->modelContextWindowPresent && usage->modelContextWindow)
        values.append(QStringLiteral("%1 context window").arg(*usage->modelContextWindow));
    if (usage->truncated || !usage->omittedFields.empty())
        values.append(QStringLiteral("usage projection truncated"));
    return values.join(QStringLiteral(" · "));
}

QString failureText(const sdk::TurnState& turn)
{
    const auto failure = sdk::failureView(turn);
    if (!failure)
        return {};
    QStringList values;
    if (failure->message)
        values.append(fromUtf8(*failure->message));
    if (failure->additionalDetails)
        values.append(fromUtf8(*failure->additionalDetails));
    if (failure->codexErrorCategory)
        values.append(humanize(fromUtf8(*failure->codexErrorCategory)));
    else if (failure->unknownErrorDiscriminator)
        values.append(humanize(fromUtf8(*failure->unknownErrorDiscriminator)));
    if (failure->httpStatusCode)
        values.append(QStringLiteral("HTTP %1").arg(*failure->httpStatusCode));
    if (failure->redacted)
        values.append(QStringLiteral("Sensitive detail redacted"));
    if (failure->decodingOmitted)
        values.append(QStringLiteral("Additional detail omitted"));
    return values.isEmpty() ? QStringLiteral("Failure details unavailable") : values.join(QStringLiteral(" · "));
}

std::vector<AgentPresentation> agentPresentations(const sdk::State& state,
                                                  const sdk::ThreadState& thread,
                                                  const sdk::TurnState& turn)
{
    std::vector<AgentPresentation> result;
    for (const auto& itemId : turn.orderedItems) {
        const auto* item = state.item(thread.id, turn.id, itemId);
        if (!item)
            continue;
        const auto semantic = sdk::itemSemanticView(*item);
        const auto* activity = semantic ? std::get_if<sdk::SubAgentActivitySemanticView>(&semantic->details) : nullptr;
        if (!activity)
            continue;

        const QString path = activity->agentPath ? fromUtf8(*activity->agentPath) : QString{};
        const QString agentThreadId = activity->agentThreadId ? fromUtf8(activity->agentThreadId->value) : QString{};
        const auto match = std::find_if(result.begin(), result.end(), [&](const AgentPresentation& existing) {
            if (!agentThreadId.isEmpty() && existing.agentThreadId == agentThreadId)
                return true;
            return agentThreadId.isEmpty() && existing.agentThreadId.isEmpty() && !path.isEmpty()
                   && existing.agentPath == path;
        });
        AgentPresentation* presentation = nullptr;
        if (match == result.end()) {
            result.push_back({});
            presentation = &result.back();
            presentation->agentPath = path;
            presentation->agentThreadId = agentThreadId;
        } else {
            presentation = &*match;
        }
        presentation->itemIds.append(fromUtf8(item->id.value));
        if (activity->kind)
            presentation->kind = humanize(fromUtf8(*activity->kind));
        if (item->status && !item->status->empty())
            presentation->status = humanize(fromUtf8(*item->status));
        if (item->summary && !item->summary->empty())
            presentation->summary = fromUtf8(*item->summary);
        const QString duration = durationText(*item);
        if (!duration.isEmpty())
            presentation->duration = duration;
    }
    return result;
}

std::vector<CollaborationPresentation> collaborationPresentations(const sdk::State& state,
                                                                  const sdk::ThreadState& thread,
                                                                  const sdk::TurnState& turn)
{
    std::vector<CollaborationPresentation> result;
    for (const auto& itemId : turn.orderedItems) {
        const auto* item = state.item(thread.id, turn.id, itemId);
        if (!item)
            continue;
        const auto semantic = sdk::itemSemanticView(*item);
        const auto* collab = semantic ? std::get_if<sdk::CollabAgentToolCallSemanticView>(&semantic->details) : nullptr;
        if (!collab)
            continue;
        CollaborationPresentation presentation;
        presentation.title = collab->tool ? humanize(fromUtf8(*collab->tool)) : QStringLiteral("Collaboration activity");
        presentation.status = collab->status ? humanize(fromUtf8(*collab->status)) : itemStatus(*item);
        QStringList detail;
        if (collab->senderThreadId)
            detail.append(QStringLiteral("Sender %1").arg(compactId(collab->senderThreadId->value)));
        if (collab->receiverCount)
            detail.append(QStringLiteral("%1 receiver%2").arg(*collab->receiverCount)
                              .arg(*collab->receiverCount == 1 ? QString{} : QStringLiteral("s")));
        if (collab->agentStateCount)
            detail.append(QStringLiteral("%1 agent state%2").arg(*collab->agentStateCount)
                              .arg(*collab->agentStateCount == 1 ? QString{} : QStringLiteral("s")));
        presentation.detail = detail.join(QStringLiteral(" · "));
        presentation.truncated = semantic->truncated || !semantic->omittedFields.empty() || item->truncated;
        result.push_back(std::move(presentation));
    }
    return result;
}

void addPresentationValue(QCryptographicHash& hash, const QByteArray& value)
{
    hash.addData(QByteArray::number(value.size()));
    hash.addData(QByteArrayLiteral(":"));
    hash.addData(value);
}

void addPresentationValue(QCryptographicHash& hash, const QString& value)
{
    addPresentationValue(hash, value.toUtf8());
}

void addPresentationValue(QCryptographicHash& hash, std::string_view value)
{
    addPresentationValue(hash, QByteArray(value.data(), static_cast<qsizetype>(value.size())));
}

void addPresentationValue(QCryptographicHash& hash, bool value)
{
    addPresentationValue(hash, value ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
}

} // namespace

InspectorWidget::InspectorWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("inspector"));
    setStyleSheet(QStringLiteral("QWidget#inspector{background:#fbfcfe;}"));
    setMinimumWidth(300);
    setMaximumWidth(520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 20, 0);
    root->setSpacing(0);

    auto* header = new QHBoxLayout;
    inspectorHeading = textLabel(QStringLiteral("INSPECTOR"), "section");
    inspectorHeading->setObjectName(QStringLiteral("inspectorHeading"));
    header->addWidget(inspectorHeading);
    header->addStretch();
    historicalBack = new QPushButton(QStringLiteral("Back"));
    historicalBack->setObjectName(QStringLiteral("historicalTurnBack"));
    historicalBack->setProperty("kind", "subtle");
    historicalBack->setFixedSize(58, 24);
    historicalBack->hide();
    header->addWidget(historicalBack);
    auto* hide = new QPushButton(QStringLiteral("Hide"));
    hide->setProperty("kind", "subtle");
    hide->setFixedSize(58, 24);
    header->addWidget(hide);
    root->addLayout(header);
    root->addSpacing(7);

    tabs = new QTabBar;
    tabs->setObjectName(QStringLiteral("inspectorTabs"));
    tabs->setExpanding(false);
    tabs->addTab(QStringLiteral("Plan"));
    tabs->addTab(QStringLiteral("Agents"));
    tabs->addTab(QStringLiteral("Changes"));
    tabs->addTab(QStringLiteral("Info"));
    tabs->setCurrentIndex(1);
    root->addWidget(tabs);
    root->addSpacing(7);
    root->addWidget(divider());

    auto* pages = new QStackedWidget;
    pages->addWidget(scrollPage(planContent));
    pages->addWidget(scrollPage(agentsContent));
    pages->addWidget(scrollPage(changesContent));
    infoScroll = qobject_cast<QScrollArea*>(scrollPage(infoContent));
    pages->addWidget(infoScroll);
    pages->setCurrentIndex(1);
    root->addWidget(pages, 1);

    connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (!historicalTurnMode)
            normalTabIndex = index;
    });
    connect(hide, &QPushButton::clicked, this, &InspectorWidget::hideRequested);
    connect(historicalBack, &QPushButton::clicked,
            this, &InspectorWidget::historicalTurnCloseRequested);
    renderUnavailable(QStringLiteral("Select a thread"),
                      QStringLiteral("Choose a synchronized thread to inspect its current state."));
}

void InspectorWidget::renderUnavailable(const QString& title, const QString& detail)
{
    setHistoricalTurnMode(false);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addPresentationValue(hash, title);
    addPresentationValue(hash, detail);
    const QByteArray presentationKey = hash.result();
    if (presentationKey == unavailablePresentationKey)
        return;
    unavailablePresentationKey = presentationKey;
    inspectedThreadId.clear();
    dependentThreadIds.clear();
    selectedAgentItemId.clear();
    planPresentationKey.clear();
    agentsPresentationKey.clear();
    changesPresentationKey.clear();
    infoPresentationKey.clear();
    infoRevisionValue = nullptr;
    for (QVBoxLayout* layout : {planContent, agentsContent, changesContent, infoContent})
    {
        clearLayout(layout);
        addEmpty(layout, title, detail);
        refreshLayoutGeometry(layout);
    }
}

bool InspectorWidget::dependsOnThread(const QString& threadId) const
{
    return !threadId.isEmpty()
        && (threadId == inspectedThreadId || dependentThreadIds.contains(threadId));
}

void InspectorWidget::setHistoricalTurnMode(bool enabled)
{
    if (historicalTurnMode == enabled)
        return;
    if (enabled)
        normalTabIndex = tabs->currentIndex();
    historicalTurnMode = enabled;
    inspectorHeading->setText(enabled ? QStringLiteral("TURN DETAILS")
                                      : QStringLiteral("INSPECTOR"));
    historicalBack->setVisible(enabled);
    tabs->setVisible(!enabled);
    tabs->setCurrentIndex(enabled ? 3 : normalTabIndex);
}

void InspectorWidget::refreshLayoutGeometry(QVBoxLayout* layout)
{
    layout->invalidate();
    layout->activate();
    if (QWidget* page = layout->parentWidget()) {
        page->adjustSize();
        page->updateGeometry();
        page->update();
    }
}

void InspectorWidget::updateStateRevision(std::uint64_t revision)
{
    if (!infoRevisionValue)
        return;
    const QString revisionText = QString::number(revision);
    if (infoRevisionValue->text() != revisionText)
        infoRevisionValue->setText(revisionText);
}

void InspectorWidget::showInfo()
{
    tabs->setCurrentIndex(3);
    QTimer::singleShot(0, this, [this] {
        if (!infoScroll)
            return;
        if (auto* configuration = infoScroll->findChild<QFrame*>(
                QStringLiteral("turnExecutionConfigurationCard")))
            infoScroll->ensureWidgetVisible(configuration, 0, 12);
    });
}

void InspectorWidget::render(const sdk::State& state,
                             const QString& threadId,
                             bool backendReady,
                             const QString& backendStatus,
                             const QString& selectedTurnId)
{
    if (!backendReady) {
        renderUnavailable(QStringLiteral("Inspector unavailable"),
                          backendStatus.isEmpty() ? QStringLiteral("The synchronized backend is not ready.")
                                                  : backendStatus);
        return;
    }
    if (threadId.isEmpty()) {
        renderUnavailable(QStringLiteral("Select a thread"),
                          QStringLiteral("Choose a synchronized thread to inspect its current state."));
        return;
    }
    const auto* thread = state.thread(threadId.toStdString());
    if (!thread) {
        renderUnavailable(QStringLiteral("Thread unavailable"),
                          QStringLiteral("The selected thread is not retained in the current State."));
        return;
    }
    unavailablePresentationKey.clear();
    if (inspectedThreadId != threadId) {
        inspectedThreadId = threadId;
        dependentThreadIds.clear();
        selectedAgentItemId.clear();
        planPresentationKey.clear();
        agentsPresentationKey.clear();
        changesPresentationKey.clear();
        infoPresentationKey.clear();
        infoRevisionValue = nullptr;
    }

    const auto* turn = latestTurn(state, *thread);
    const bool hasSelectedConfigurationTurn = !selectedTurnId.isEmpty();
    setHistoricalTurnMode(hasSelectedConfigurationTurn);
    const sdk::TurnState* configurationTurn = turn;
    bool requestedConfigurationTurnUnavailable = false;
    if (hasSelectedConfigurationTurn) {
        configurationTurn = state.turn(thread->id, typed::TurnId{selectedTurnId.toStdString()});
        if (!configurationTurn) {
            configurationTurn = nullptr;
            requestedConfigurationTurnUnavailable = true;
        }
    }
    qsizetype configurationTurnNumber = -1;
    if (hasSelectedConfigurationTurn && configurationTurn) {
        const auto iterator = std::find(thread->orderedTurns.begin(),
                                        thread->orderedTurns.end(),
                                        configurationTurn->id);
        if (iterator != thread->orderedTurns.end())
            configurationTurnNumber = static_cast<qsizetype>(
                std::distance(thread->orderedTurns.begin(), iterator)) + 1;
    }
    const QString configurationUnavailableDetail = requestedConfigurationTurnUnavailable
      ? QStringLiteral("The requested turn is not retained for this thread. Current thread settings are not substituted.")
      : configurationTurn
          ? QStringLiteral("AISuite has no authoritative effective execution configuration recorded for this turn. "
                           "Current thread settings are not substituted.")
          : QStringLiteral("No retained turn is available. Current thread settings are not substituted.");
    const ExecutionConfigurationPresentation executionConfiguration =
        executionConfigurationPresentation(configurationTurn, configurationUnavailableDetail);

    // Prefer the authoritative structured turn plan. Plan items remain a
    // compatibility fallback for app-server versions that only emit the item.
    const sdk::TurnPlanState* structuredPlan = turn && turn->plan ? &*turn->plan : nullptr;
    const sdk::ItemState* planItem = nullptr;
    std::optional<sdk::PlanSemanticView> planView;
    if (turn && !structuredPlan) {
        for (auto iterator = turn->orderedItems.rbegin(); iterator != turn->orderedItems.rend(); ++iterator) {
            const auto* item = state.item(thread->id, turn->id, *iterator);
            if (!item)
                continue;
            const auto semantic = sdk::itemSemanticView(*item);
            const auto* plan = semantic ? std::get_if<sdk::PlanSemanticView>(&semantic->details) : nullptr;
            if (!plan)
                continue;
            planItem = item;
            planView = *plan;
            break;
        }
    }
    QCryptographicHash planHash(QCryptographicHash::Sha256);
    addPresentationValue(planHash, turn != nullptr);
    addPresentationValue(planHash, thread->fullyLoaded);
    addPresentationValue(planHash, structuredPlan != nullptr);
    if (structuredPlan) {
        addPresentationValue(planHash,
                             structuredPlan->explanation
                                 ? std::string_view(*structuredPlan->explanation)
                                 : std::string_view{});
        addPresentationValue(planHash, QByteArray::number(structuredPlan->totalSteps));
        addPresentationValue(planHash, structuredPlan->truncated);
        for (const auto& step : structuredPlan->steps) {
            addPresentationValue(planHash, step.step);
            addPresentationValue(planHash, step.status.value);
        }
    } else if (planItem && planView) {
        addPresentationValue(planHash, planItem->id.value);
        addPresentationValue(planHash, itemStatus(*planItem));
        addPresentationValue(planHash, planView->text ? std::string_view(*planView->text) : std::string_view{});
        addPresentationValue(planHash, planView->textTruncated);
        addPresentationValue(planHash, planItem->truncated || !planItem->omittedFields.empty());
    }
    const QByteArray nextPlanKey = planHash.result();
    const bool planChanged = nextPlanKey != planPresentationKey;
    if (planChanged) {
        planPresentationKey = nextPlanKey;
        clearLayout(planContent);
        if (!turn) {
            addEmpty(planContent, QStringLiteral("No plan"), QStringLiteral("This thread has no retained turns."));
        } else if (!structuredPlan && (!planItem || !planView)) {
            addEmpty(planContent, QStringLiteral("No plan"),
                     thread->fullyLoaded ? QStringLiteral("No plan is projected for the latest turn.")
                                         : QStringLiteral("No plan is retained in this partial thread projection."));
        } else {
            planContent->addWidget(textLabel(QStringLiteral("CURRENT PLAN"), "section"));
            planContent->addSpacing(8);
            auto* card = detailCard();
            auto* layout = new QVBoxLayout(card);
            layout->setContentsMargins(14, 14, 14, 14);
            layout->setSpacing(7);
            auto* header = new QHBoxLayout;
            auto* title = textLabel(QStringLiteral("Latest turn plan"));
            title->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;"));
            header->addWidget(title);
            header->addStretch();
            const QString status = structuredPlan ? QStringLiteral("Current")
                                                  : itemStatus(*planItem);
            if (!status.isEmpty()) {
                auto* statusLabel = textLabel(status, "small");
                statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;")
                                               .arg(statusColor(status)));
                header->addWidget(statusLabel);
            }
            layout->addLayout(header);
            if (structuredPlan) {
                if (structuredPlan->explanation && !structuredPlan->explanation->empty()) {
                    auto* explanation = textLabel(fromUtf8(*structuredPlan->explanation));
                    explanation->setWordWrap(true);
                    explanation->setTextInteractionFlags(Qt::TextSelectableByMouse);
                    explanation->setStyleSheet(QStringLiteral("font-size:12px;color:#475467;"));
                    layout->addWidget(explanation);
                }
                for (const auto& step : structuredPlan->steps) {
                    auto* stepRow = new QWidget;
                    stepRow->setObjectName(QStringLiteral("inspectorPlanStep"));
                    auto* stepLayout = new QHBoxLayout(stepRow);
                    stepLayout->setContentsMargins(0, 3, 0, 3);
                    stepLayout->setSpacing(7);
                    const QString stepStatus = humanize(fromUtf8(step.status.value));
                    auto* marker = textLabel(planStatusGlyph(stepStatus));
                    marker->setFixedWidth(14);
                    marker->setStyleSheet(
                        QStringLiteral("color:%1;font-size:11px;font-weight:600;")
                            .arg(statusColor(stepStatus)));
                    stepLayout->addWidget(marker, 0, Qt::AlignTop);
                    auto* stepText = textLabel(fromUtf8(step.step));
                    stepText->setObjectName(QStringLiteral("inspectorPlanStepText"));
                    stepText->setWordWrap(true);
                    stepText->setTextInteractionFlags(Qt::TextSelectableByMouse);
                    stepText->setStyleSheet(QStringLiteral("font-size:12px;"));
                    stepLayout->addWidget(stepText, 1);
                    auto* stepState = textLabel(stepStatus, "small");
                    stepState->setStyleSheet(
                        QStringLiteral("color:%1;font-size:9px;font-weight:600;")
                            .arg(statusColor(stepStatus)));
                    stepLayout->addWidget(stepState, 0, Qt::AlignTop);
                    layout->addWidget(stepRow);
                }
                if (structuredPlan->steps.empty()) {
                    auto* absent = textLabel(QStringLiteral("The current plan contains no steps."), "muted");
                    absent->setWordWrap(true);
                    layout->addWidget(absent);
                }
                if (structuredPlan->truncated) {
                    auto* truncated = textLabel(
                        QStringLiteral("Showing %1 of %2 plan steps")
                            .arg(structuredPlan->steps.size())
                            .arg(structuredPlan->totalSteps),
                        "small");
                    truncated->setObjectName(QStringLiteral("inspectorPlanTruncation"));
                    truncated->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
                    layout->addWidget(truncated);
                }
            } else if (planView->text && !planView->text->empty()) {
                auto* text = textLabel(fromUtf8(*planView->text));
                text->setWordWrap(true);
                text->setTextInteractionFlags(Qt::TextSelectableByMouse);
                text->setStyleSheet(QStringLiteral("font-size:12px;"));
                layout->addWidget(text);
            } else {
                auto* absent = textLabel(QStringLiteral("Plan text is unavailable in the current projection."), "muted");
                absent->setWordWrap(true);
                layout->addWidget(absent);
            }
            if (!structuredPlan
                && (planView->textTruncated || planItem->truncated || !planItem->omittedFields.empty())) {
                auto* truncated = textLabel(QStringLiteral("Plan projection is truncated or partially omitted"), "small");
                truncated->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
                layout->addWidget(truncated);
            }
            planContent->addWidget(card);
            planContent->addStretch();
        }
    }

    // Agents: SubAgentActivitySemanticView is flat in this SDK. Keep the
    // representation flat rather than inferring a parent/child tree.
    std::vector<AgentPresentation> agents;
    std::vector<CollaborationPresentation> collaborations;
    if (turn) {
        agents = agentPresentations(state, *thread, *turn);
        collaborations = collaborationPresentations(state, *thread, *turn);
    }
    dependentThreadIds.clear();
    for (const AgentPresentation& agent : agents) {
        if (!agent.agentThreadId.isEmpty())
            dependentThreadIds.insert(agent.agentThreadId);
    }
    const auto selected = std::find_if(agents.begin(), agents.end(), [this](const AgentPresentation& agent) {
        return agent.itemIds.contains(selectedAgentItemId);
    });
    if (selected == agents.end())
        selectedAgentItemId = agents.empty() ? QString{} : agents.front().itemIds.back();

    QCryptographicHash agentsHash(QCryptographicHash::Sha256);
    addPresentationValue(agentsHash, turn != nullptr);
    addPresentationValue(agentsHash, selectedAgentItemId);
    for (const auto& agent : agents) {
        for (const QString& id : agent.itemIds)
            addPresentationValue(agentsHash, id);
        addPresentationValue(agentsHash, agent.agentPath);
        addPresentationValue(agentsHash, agent.agentThreadId);
        addPresentationValue(agentsHash, agent.kind);
        addPresentationValue(agentsHash, agent.status);
        addPresentationValue(agentsHash, agent.summary);
        addPresentationValue(agentsHash, agent.duration);
        if (const auto* agentThread = agent.agentThreadId.isEmpty()
                                          ? nullptr
                                          : state.thread(agent.agentThreadId.toStdString())) {
            addPresentationValue(agentsHash,
                                 agentThread->status ? std::string_view(*agentThread->status) : std::string_view{});
            addPresentationValue(agentsHash,
                                 agentThread->model ? std::string_view(agentThread->model->value) : std::string_view{});
            addPresentationValue(agentsHash,
                                 agentThread->modelProvider ? std::string_view(*agentThread->modelProvider)
                                                            : std::string_view{});
        }
    }
    for (const auto& collaboration : collaborations) {
        addPresentationValue(agentsHash, collaboration.title);
        addPresentationValue(agentsHash, collaboration.status);
        addPresentationValue(agentsHash, collaboration.detail);
        addPresentationValue(agentsHash, collaboration.truncated);
    }
    if (!agents.empty() && state.hasPendingRequestProjection()) {
        for (const auto& request : state.pendingRequests()) {
            if (request.threadId)
                addPresentationValue(agentsHash, request.threadId->value);
        }
    }
    const QByteArray nextAgentsKey = agentsHash.result();
    const bool agentsChanged = nextAgentsKey != agentsPresentationKey;
    if (agentsChanged) {
        agentsPresentationKey = nextAgentsKey;
        clearLayout(agentsContent);
        if (agents.empty() && collaborations.empty()) {
            addEmpty(agentsContent, QStringLiteral("No agent activity"),
                     turn ? QStringLiteral("No collab or subagent activity is projected for the latest turn.")
                          : QStringLiteral("This thread has no retained turns."));
        } else {
            if (!agents.empty()) {
            agentsContent->addWidget(textLabel(QStringLiteral("AGENT ACTIVITY"), "section"));
            agentsContent->addSpacing(8);
            for (const auto& agent : agents) {
                const QString primaryId = agent.itemIds.back();
                const bool active = agent.itemIds.contains(selectedAgentItemId);
                const QString name = agent.agentPath.isEmpty() ? QStringLiteral("Subagent activity") : agent.agentPath;
                QStringList details;
                if (!agent.kind.isEmpty())
                    details.append(agent.kind);
                if (!agent.status.isEmpty())
                    details.append(agent.status);
                if (!agent.duration.isEmpty())
                    details.append(agent.duration);
                if (!agent.agentThreadId.isEmpty())
                    details.append(QStringLiteral("thread %1").arg(compactId(agent.agentThreadId.toStdString())));
                if (agent.itemIds.size() > 1)
                    details.append(QStringLiteral("%1 activities").arg(agent.itemIds.size()));
                auto* row = new QPushButton(QStringLiteral("%1\n%2").arg(name, details.join(QStringLiteral(" · "))));
                row->setCursor(Qt::PointingHandCursor);
                row->setMinimumHeight(details.isEmpty() ? 38 : 52);
                row->setStyleSheet(QStringLiteral(
                    "QPushButton{background:%1;color:#1d2633;border:1px solid %2;border-radius:8px;text-align:left;padding:6px 10px;"
                    "font-size:11px;font-weight:500;}QPushButton:hover{background:#f1f5fb;}")
                                       .arg(active ? QStringLiteral("#e5eeff") : QStringLiteral("transparent"),
                                            active ? QStringLiteral("#bfd3f9") : QStringLiteral("transparent")));
                row->setToolTip(name);
                connect(row, &QPushButton::clicked, this, [this, primaryId] {
                    selectedAgentItemId = primaryId;
                    emit selectionChanged();
                });
                agentsContent->addWidget(row);
            }
            }

            if (!collaborations.empty()) {
            if (!agents.empty()) {
                agentsContent->addSpacing(16);
                agentsContent->addWidget(divider());
                agentsContent->addSpacing(16);
            }
            agentsContent->addWidget(textLabel(QStringLiteral("COLLABORATION"), "section"));
            agentsContent->addSpacing(8);
            for (const auto& collaboration : collaborations) {
                auto* card = detailCard();
                auto* layout = new QVBoxLayout(card);
                layout->setContentsMargins(12, 10, 12, 10);
                layout->setSpacing(4);
                auto* header = new QHBoxLayout;
                header->addWidget(textLabel(collaboration.title));
                header->addStretch();
                if (!collaboration.status.isEmpty()) {
                    auto* status = textLabel(collaboration.status, "small");
                    status->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;")
                                              .arg(statusColor(collaboration.status)));
                    header->addWidget(status);
                }
                layout->addLayout(header);
                if (!collaboration.detail.isEmpty()) {
                    auto* detail = textLabel(collaboration.detail, "meta");
                    detail->setWordWrap(true);
                    layout->addWidget(detail);
                }
                if (collaboration.truncated) {
                    auto* truncated = textLabel(QStringLiteral("Projected detail is truncated or omitted"), "small");
                    truncated->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
                    layout->addWidget(truncated);
                }
                agentsContent->addWidget(card);
                agentsContent->addSpacing(6);
            }
            }

            const auto selectedAgent = std::find_if(agents.begin(), agents.end(), [this](const AgentPresentation& agent) {
                return agent.itemIds.contains(selectedAgentItemId);
            });
            if (selectedAgent != agents.end()) {
            agentsContent->addSpacing(18);
            agentsContent->addWidget(divider());
            agentsContent->addSpacing(16);
            agentsContent->addWidget(textLabel(QStringLiteral("SELECTED AGENT"), "section"));
            agentsContent->addSpacing(9);
            const QString name = selectedAgent->agentPath.isEmpty() ? QStringLiteral("Subagent activity")
                                                                    : selectedAgent->agentPath;
            auto* heading = textLabel(name);
            heading->setWordWrap(true);
            heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
            agentsContent->addWidget(heading);
            if (!selectedAgent->summary.isEmpty()) {
                auto* summary = textLabel(selectedAgent->summary, "muted");
                summary->setWordWrap(true);
                agentsContent->addWidget(summary);
            }
            agentsContent->addSpacing(20);

            auto* facts = new QGridLayout;
            facts->setContentsMargins(0, 0, 0, 0);
            facts->setHorizontalSpacing(18);
            facts->setVerticalSpacing(9);
            facts->setColumnMinimumWidth(0, 84);
            int row = 0;
            addFact(facts, row, QStringLiteral("Activity"), selectedAgent->kind);
            addFact(facts, row, QStringLiteral("Status"), selectedAgent->status,
                    statusColor(selectedAgent->status));
            addFact(facts, row, QStringLiteral("Duration"), selectedAgent->duration);
            addFact(facts, row, QStringLiteral("Thread"),
                    selectedAgent->agentThreadId.isEmpty()
                        ? QString{}
                        : compactId(selectedAgent->agentThreadId.toStdString()));
            const auto* agentThread = selectedAgent->agentThreadId.isEmpty()
                                          ? nullptr
                                          : state.thread(selectedAgent->agentThreadId.toStdString());
            if (selectedAgent->status.isEmpty() && agentThread && agentThread->status)
                addFact(facts, row, QStringLiteral("Status"), humanize(fromUtf8(*agentThread->status)),
                        statusColor(fromUtf8(*agentThread->status)));
            if (agentThread && agentThread->model)
                addFact(facts, row, QStringLiteral("Model"), fromUtf8(agentThread->model->value));
            if (agentThread && agentThread->modelProvider)
                addFact(facts, row, QStringLiteral("Provider"), fromUtf8(*agentThread->modelProvider));
            std::size_t pending = 0;
            if (!selectedAgent->agentThreadId.isEmpty() && state.hasPendingRequestProjection()) {
                for (const auto& request : state.pendingRequests())
                    pending += request.threadId && request.threadId->value == selectedAgent->agentThreadId.toStdString();
            }
            if (pending > 0)
                addFact(facts, row, QStringLiteral("Attention"),
                        QStringLiteral("%1 pending request%2").arg(pending)
                            .arg(pending == 1 ? QString{} : QStringLiteral("s")),
                        QStringLiteral("#a76812"));
            agentsContent->addLayout(facts);

            if (agentThread) {
                agentsContent->addSpacing(18);
                auto* open = new QPushButton(QStringLiteral("Open thread                                      ↗"));
                open->setProperty("kind", "agentLink");
                open->setFixedHeight(34);
                const QString target = selectedAgent->agentThreadId;
                connect(open, &QPushButton::clicked, this, [this, target] { emit threadOpenRequested(target); });
                agentsContent->addWidget(open);
            }
            }
            agentsContent->addStretch();
        }
    }

    // Changes: only canonical projected metadata is shown. The installed view
    // does not expose path strings, typed change kinds, or line counts.
    std::vector<std::pair<const sdk::ItemState*, sdk::FileChangeSemanticView>> fileChanges;
    if (turn) {
        for (const auto& itemId : turn->orderedItems) {
            const auto* item = state.item(thread->id, turn->id, itemId);
            if (!item)
                continue;
            const auto semantic = sdk::itemSemanticView(*item);
            const auto* changes = semantic ? std::get_if<sdk::FileChangeSemanticView>(&semantic->details) : nullptr;
            if (changes)
                fileChanges.emplace_back(item, *changes);
        }
    }
    QCryptographicHash changesHash(QCryptographicHash::Sha256);
    addPresentationValue(changesHash, turn != nullptr);
    for (const auto& [item, view] : fileChanges) {
        addPresentationValue(changesHash, item->id.value);
        addPresentationValue(changesHash, itemStatus(*item));
        addPresentationValue(changesHash,
                             item->summary ? std::string_view(*item->summary) : std::string_view{});
        addPresentationValue(changesHash,
                             view.status ? std::string_view(*view.status) : std::string_view{});
        addPresentationValue(changesHash,
                             view.changeCount ? QByteArray::number(*view.changeCount) : QByteArray{});
        addPresentationValue(changesHash, view.changesTruncated);
        addPresentationValue(changesHash, item->truncated || !item->omittedFields.empty());
        for (const auto& change : view.changes) {
            addPresentationValue(changesHash, change.pathRedacted);
            addPresentationValue(changesHash,
                                 change.pathBytes ? QByteArray::number(*change.pathBytes) : QByteArray{});
            addPresentationValue(changesHash, change.diffOmitted);
            addPresentationValue(changesHash,
                                 change.diffBytes ? QByteArray::number(*change.diffBytes) : QByteArray{});
        }
    }
    const QByteArray nextChangesKey = changesHash.result();
    const bool changesChanged = nextChangesKey != changesPresentationKey;
    if (changesChanged) {
        changesPresentationKey = nextChangesKey;
        clearLayout(changesContent);
        if (fileChanges.empty()) {
            addEmpty(changesContent, QStringLiteral("No file changes"),
                     turn ? QStringLiteral("No file-change items are projected for the latest turn.")
                          : QStringLiteral("This thread has no retained turns."));
        } else {
            changesContent->addWidget(textLabel(QStringLiteral("REPORTED CHANGES"), "section"));
            changesContent->addSpacing(8);
            std::size_t reportedChanges = 0;
            bool hasCompleteReportedCount = true;
            for (const auto& [item, view] : fileChanges) {
                if (view.changeCount)
                    reportedChanges += *view.changeCount;
                else
                    hasCompleteReportedCount = false;
            }
            auto* summary = textLabel(hasCompleteReportedCount
                                          ? QStringLiteral("%1 reported change entr%2")
                                                .arg(reportedChanges)
                                                .arg(reportedChanges == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                                          : QStringLiteral("%1 file-change item%2")
                                                .arg(fileChanges.size())
                                                .arg(fileChanges.size() == 1 ? QString{} : QStringLiteral("s")));
            summary->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;"));
            changesContent->addWidget(summary);
            changesContent->addSpacing(10);

            for (const auto& [item, view] : fileChanges) {
                auto* card = detailCard();
                auto* layout = new QVBoxLayout(card);
                layout->setContentsMargins(12, 11, 12, 11);
                layout->setSpacing(5);
                auto* header = new QHBoxLayout;
                const QString title = item->summary && !item->summary->empty()
                                          ? compact(fromUtf8(*item->summary), 120)
                                          : view.changeCount ? QStringLiteral("%1 change entr%2")
                                                                   .arg(*view.changeCount)
                                                                   .arg(*view.changeCount == 1 ? QStringLiteral("y")
                                                                                               : QStringLiteral("ies"))
                                                             : QStringLiteral("File-change item");
                auto* heading = textLabel(title);
                heading->setWordWrap(true);
                heading->setStyleSheet(QStringLiteral("font-size:11px;font-weight:600;"));
                header->addWidget(heading, 1);
                const QString status = view.status ? humanize(fromUtf8(*view.status)) : itemStatus(*item);
                if (!status.isEmpty()) {
                    auto* statusLabel = textLabel(status, "small");
                    statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:600;")
                                                   .arg(statusColor(status)));
                    header->addWidget(statusLabel, 0, Qt::AlignTop);
                }
                layout->addLayout(header);

                for (const auto& change : view.changes) {
                    QStringList detail;
                    if (change.pathRedacted)
                        detail.append(QStringLiteral("Affected path redacted"));
                    else if (change.pathBytes)
                        detail.append(QStringLiteral("Affected path unavailable · %1 projected bytes").arg(*change.pathBytes));
                    else
                        detail.append(QStringLiteral("Affected path unavailable"));
                    if (change.diffOmitted)
                        detail.append(QStringLiteral("diff omitted"));
                    else if (change.diffBytes)
                        detail.append(QStringLiteral("%1 diff bytes").arg(*change.diffBytes));
                    auto* entry = textLabel(QStringLiteral("•  %1").arg(detail.join(QStringLiteral(" · "))), "meta");
                    entry->setWordWrap(true);
                    layout->addWidget(entry);
                }
                if (view.changes.empty()) {
                    auto* unavailable = textLabel(QStringLiteral("Per-change detail is not retained."), "meta");
                    unavailable->setWordWrap(true);
                    layout->addWidget(unavailable);
                }
                if (view.changesTruncated || item->truncated || !item->omittedFields.empty()) {
                    auto* truncated = textLabel(QStringLiteral("Change projection is truncated or partially omitted"), "small");
                    truncated->setStyleSheet(QStringLiteral("color:#a76812;font-size:9px;"));
                    layout->addWidget(truncated);
                }
                changesContent->addWidget(card);
                changesContent->addSpacing(7);
            }
            changesContent->addStretch();
        }
    }

    // Info: a compact product view over typed thread, turn, provider, usage,
    // failure, synchronization, and projection state.
    const auto& provider = state.provider();
    const auto& controller = state.controller();
    const auto& truncation = state.truncation();
    const auto& threadList = state.threadList();
    const auto& projection = state.projectionMetadata();
    std::size_t pendingForThread = 0;
    if (state.hasPendingRequestProjection()) {
        for (const auto& request : state.pendingRequests())
            pendingForThread += request.threadId && request.threadId->value == thread->id.value;
    }

    QCryptographicHash infoHash(QCryptographicHash::Sha256);
    addPresentationValue(infoHash, threadId);
    addPresentationValue(infoHash,
                         thread->title ? std::string_view(*thread->title) : std::string_view{});
    addPresentationValue(infoHash, thread->id.value);
    addPresentationValue(infoHash,
                         thread->cwd ? std::string_view(thread->cwd->value) : std::string_view{});
    addPresentationValue(infoHash,
                         thread->status ? std::string_view(*thread->status) : std::string_view{});
    addPresentationValue(infoHash,
                         thread->model ? std::string_view(thread->model->value) : std::string_view{});
    addPresentationValue(infoHash,
                         thread->modelProvider ? std::string_view(*thread->modelProvider) : std::string_view{});
    addPresentationValue(infoHash, thread->ephemeral.has_value());
    addPresentationValue(infoHash, thread->ephemeral.value_or(false));
    addPresentationValue(infoHash, thread->archived.has_value());
    addPresentationValue(infoHash, thread->archived.value_or(false));
    addPresentationValue(infoHash, thread->fullyLoaded);
    addPresentationValue(infoHash, turn != nullptr);
    if (turn) {
        addPresentationValue(infoHash, turn->id.value);
        addPresentationValue(infoHash, turn->status.value);
        addPresentationValue(infoHash, turn->active);
        addPresentationValue(infoHash, turn->terminal);
        addPresentationValue(infoHash, tokenUsageText(*turn));
        addPresentationValue(infoHash, failureText(*turn));
    }
    addPresentationValue(infoHash, hasSelectedConfigurationTurn);
    addPresentationValue(infoHash, QByteArray::number(configurationTurnNumber));
    addPresentationValue(infoHash, executionConfiguration.recorded);
    addPresentationValue(infoHash, executionConfiguration.turnId);
    addPresentationValue(infoHash, executionConfiguration.unavailableDetail);
    addPresentationValue(infoHash, executionConfiguration.model);
    addPresentationValue(infoHash, executionConfiguration.effort);
    addPresentationValue(infoHash, executionConfiguration.personality);
    addPresentationValue(infoHash, executionConfiguration.workspace);
    addPresentationValue(infoHash, executionConfiguration.sandbox);
    addPresentationValue(infoHash, executionConfiguration.approvalPolicy);
    addPresentationValue(infoHash, executionConfiguration.approvalsReviewer);
    addPresentationValue(infoHash, executionConfiguration.serviceTier);
    addPresentationValue(infoHash, executionConfiguration.summary);
    addPresentationValue(infoHash, executionConfiguration.collaborationMode);
    addPresentationValue(infoHash, executionConfiguration.activePermissionProfile);
    addPresentationValue(infoHash, executionConfiguration.provenance);
    addPresentationValue(infoHash, freshnessText(state.freshness()));
    addPresentationValue(infoHash, representationText(state.representationMode()));
    addPresentationValue(infoHash, provider.value.has_value());
    if (provider.value) {
        addPresentationValue(infoHash, providerLifecycleText(provider.value->lifecycle));
        addPresentationValue(infoHash, provider.value->ready);
        addPresentationValue(
            infoHash,
            provider.value->lastError && provider.value->lastError->message
                ? std::string_view(*provider.value->lastError->message)
                : std::string_view{});
    }
    addPresentationValue(infoHash, controller.value.has_value());
    if (controller.value) {
        addPresentationValue(infoHash, controller.value->present);
        addPresentationValue(infoHash, controller.value->ownedByThisClient);
    }
    addPresentationValue(infoHash, state.hasPendingRequestProjection());
    addPresentationValue(infoHash, QByteArray::number(pendingForThread));
    addPresentationValue(infoHash, truncation.truncated);
    addPresentationValue(infoHash, truncation.value.has_value());
    if (truncation.value) {
        addPresentationValue(infoHash, truncation.value->truncated);
        addPresentationValue(infoHash,
                             truncation.value->omittedEntries
                                 ? QByteArray::number(*truncation.value->omittedEntries)
                                 : QByteArray{});
    }
    addPresentationValue(infoHash, threadList.value.has_value());
    if (threadList.value)
        addPresentationValue(infoHash, threadList.value->complete);
    addPresentationValue(infoHash, QByteArray::number(projection.omittedFields.size()));
    addPresentationValue(infoHash, QByteArray::number(projection.redactedFields.size()));
    addPresentationValue(infoHash, thread->realtime.has_value());
    if (thread->realtime) {
        const auto realtime = sdk::realtimeSemanticView(*thread->realtime);
        addPresentationValue(infoHash, realtime.lifecycle);
        addPresentationValue(infoHash, QByteArray::number(realtime.itemCount));
        addPresentationValue(infoHash, realtime.transcriptTruncated);
        addPresentationValue(infoHash,
                             realtime.lastError ? std::string_view(*realtime.lastError)
                                                : std::string_view{});
    }
    const QByteArray nextInfoKey = infoHash.result();
    const bool infoChanged = nextInfoKey != infoPresentationKey;
    const QString revisionText = QString::number(state.revision());
    if (!infoChanged)
        updateStateRevision(state.revision());
    if (infoChanged) {
    infoPresentationKey = nextInfoKey;
    infoRevisionValue = nullptr;
    clearLayout(infoContent);
    if (hasSelectedConfigurationTurn) {
        auto* title = textLabel(configurationTurnNumber > 0
                                    ? QStringLiteral("Effective configuration · Turn %1")
                                          .arg(configurationTurnNumber)
                                    : QStringLiteral("Effective configuration"));
        title->setObjectName(QStringLiteral("historicalTurnConfigurationTitle"));
        title->setStyleSheet(QStringLiteral("color:#1d2633;font-size:16px;font-weight:600;"));
        infoContent->addWidget(title);
        infoContent->addSpacing(5);
        auto* readOnly = textLabel(QStringLiteral("Read-only historical record"));
        readOnly->setStyleSheet(QStringLiteral("color:#667085;font-size:11px;"));
        infoContent->addWidget(readOnly);
        infoContent->addSpacing(12);
        infoContent->addWidget(divider());
    } else {
    infoContent->addWidget(textLabel(QStringLiteral("THREAD"), "section"));
    infoContent->addSpacing(8);
    auto* threadCard = detailCard();
    auto* threadFacts = new QGridLayout(threadCard);
    threadFacts->setContentsMargins(12, 11, 12, 11);
    threadFacts->setHorizontalSpacing(16);
    threadFacts->setVerticalSpacing(8);
    threadFacts->setColumnMinimumWidth(0, 78);
    int threadRow = 0;
    addFact(threadFacts, threadRow, QStringLiteral("Title"),
            thread->title && !thread->title->empty() ? fromUtf8(*thread->title) : QString{});
    addFact(threadFacts, threadRow, QStringLiteral("Thread ID"), fromUtf8(thread->id.value));
    addFact(threadFacts, threadRow, QStringLiteral("CWD"), thread->cwd ? fromUtf8(thread->cwd->value) : QString{});
    addFact(threadFacts, threadRow, QStringLiteral("Status"),
            thread->status ? humanize(fromUtf8(*thread->status)) : QString{});
    addFact(threadFacts, threadRow, QStringLiteral("Model"), thread->model ? fromUtf8(thread->model->value) : QString{});
    addFact(threadFacts, threadRow, QStringLiteral("Provider"),
            thread->modelProvider ? fromUtf8(*thread->modelProvider) : QString{});
    if (thread->ephemeral)
        addFact(threadFacts, threadRow, QStringLiteral("Lifetime"),
                *thread->ephemeral ? QStringLiteral("Temporary") : QStringLiteral("Persistent"));
    if (thread->archived)
        addFact(threadFacts, threadRow, QStringLiteral("Archive"),
                *thread->archived ? QStringLiteral("Archived") : QStringLiteral("Active"));
    addFact(threadFacts, threadRow, QStringLiteral("Projection"),
            thread->fullyLoaded ? QStringLiteral("Fully loaded") : QStringLiteral("Partial"),
            thread->fullyLoaded ? QStringLiteral("#23845a") : QStringLiteral("#a76812"));
    infoContent->addWidget(threadCard);

    if (turn) {
        infoContent->addSpacing(16);
        infoContent->addWidget(textLabel(QStringLiteral("LATEST TURN"), "section"));
        infoContent->addSpacing(8);
        auto* turnCard = detailCard();
        auto* turnFacts = new QGridLayout(turnCard);
        turnFacts->setContentsMargins(12, 11, 12, 11);
        turnFacts->setHorizontalSpacing(16);
        turnFacts->setVerticalSpacing(8);
        turnFacts->setColumnMinimumWidth(0, 78);
        int turnRow = 0;
        addFact(turnFacts, turnRow, QStringLiteral("Turn ID"), fromUtf8(turn->id.value));
        addFact(turnFacts, turnRow, QStringLiteral("Status"), humanize(fromUtf8(turn->status.value)),
                statusColor(fromUtf8(turn->status.value)));
        addFact(turnFacts, turnRow, QStringLiteral("Lifecycle"),
                turn->active ? QStringLiteral("Active")
                             : turn->terminal ? QStringLiteral("Terminal") : QStringLiteral("Retained"));
        addFact(turnFacts, turnRow, QStringLiteral("Token usage"), tokenUsageText(*turn));
        addFact(turnFacts, turnRow, QStringLiteral("Failure"), failureText(*turn), QStringLiteral("#b83a3a"));
        infoContent->addWidget(turnCard);
    }

    }

    infoContent->addSpacing(16);
    if (!hasSelectedConfigurationTurn)
        infoContent->addWidget(textLabel(QStringLiteral("LATEST TURN CONFIGURATION"), "section"));
    infoContent->addSpacing(8);
    auto* configurationCard = executionConfigurationCard();
    auto* configurationLayout = new QVBoxLayout(configurationCard);
    configurationLayout->setContentsMargins(13, 12, 13, 12);
    configurationLayout->setSpacing(7);
    if (!executionConfiguration.recorded) {
        auto* heading = textLabel(requestedConfigurationTurnUnavailable || !configurationTurn
                                    ? QStringLiteral("Unavailable")
                                    : QStringLiteral("Not recorded"));
        heading->setStyleSheet(QStringLiteral("color:#1d2633;font-size:13px;font-weight:600;"));
        configurationLayout->addWidget(heading);
        auto* detail = textLabel(executionConfiguration.unavailableDetail);
        detail->setWordWrap(true);
        detail->setStyleSheet(QStringLiteral("color:#667085;font-size:11px;"));
        configurationLayout->addWidget(detail);
    } else {
        auto* header = new QHBoxLayout;
        auto* heading = textLabel(QStringLiteral("Effective settings"));
        heading->setStyleSheet(QStringLiteral("color:#1d2633;font-size:13px;font-weight:600;"));
        header->addWidget(heading);
        header->addStretch();
        auto* identity = textLabel(compactId(executionConfiguration.turnId.toStdString()));
        identity->setStyleSheet(QStringLiteral("color:#2f6feb;font-size:9px;font-weight:600;"));
        identity->setToolTip(executionConfiguration.turnId);
        header->addWidget(identity, 0, Qt::AlignTop);
        configurationLayout->addLayout(header);

        auto* provenance = textLabel(executionConfiguration.provenance);
        provenance->setWordWrap(true);
        provenance->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;"));
        configurationLayout->addWidget(provenance);
        configurationLayout->addSpacing(2);

        auto* facts = new QGridLayout;
        facts->setContentsMargins(0, 0, 0, 0);
        facts->setHorizontalSpacing(16);
        facts->setVerticalSpacing(7);
        facts->setColumnMinimumWidth(0, 100);
        int row = 0;
        addExecutionConfigurationFact(facts, row, QStringLiteral("Model"), executionConfiguration.model);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Reasoning effort"), executionConfiguration.effort);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Style"), executionConfiguration.personality);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Workspace"), executionConfiguration.workspace);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Sandbox / access"), executionConfiguration.sandbox);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Approval policy"), executionConfiguration.approvalPolicy);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Reviewer"), executionConfiguration.approvalsReviewer);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Service tier"), executionConfiguration.serviceTier);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Reasoning summary"), executionConfiguration.summary);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Collaboration mode"),
                                      executionConfiguration.collaborationMode);
        addExecutionConfigurationFact(facts, row, QStringLiteral("Permission profile"),
                                      executionConfiguration.activePermissionProfile);
        configurationLayout->addLayout(facts);
    }
    infoContent->addWidget(configurationCard);

    if (hasSelectedConfigurationTurn) {
        infoContent->addSpacing(18);
        infoContent->addWidget(divider());
        infoContent->addSpacing(10);
        auto* provenance = textLabel(
            QStringLiteral("Loaded from authoritative AISuite turn state — never inferred from current thread settings."));
        provenance->setWordWrap(true);
        provenance->setStyleSheet(QStringLiteral("color:#667085;font-size:10px;"));
        infoContent->addWidget(provenance);
    }

    if (!hasSelectedConfigurationTurn) {
    infoContent->addSpacing(16);
    infoContent->addWidget(textLabel(QStringLiteral("SYNCHRONIZATION"), "section"));
    infoContent->addSpacing(8);
    auto* stateCard = detailCard();
    auto* stateFacts = new QGridLayout(stateCard);
    stateFacts->setContentsMargins(12, 11, 12, 11);
    stateFacts->setHorizontalSpacing(16);
    stateFacts->setVerticalSpacing(8);
    stateFacts->setColumnMinimumWidth(0, 78);
    int stateRow = 0;
    const QString freshness = freshnessText(state.freshness());
    addFact(stateFacts, stateRow, QStringLiteral("State"), freshness,
            freshness == QStringLiteral("Current") ? QStringLiteral("#23845a")
                                                   : QStringLiteral("#a76812"));
    infoRevisionValue = addFact(stateFacts, stateRow, QStringLiteral("Revision"), revisionText);
    if (infoRevisionValue)
        infoRevisionValue->setObjectName(QStringLiteral("inspectorStateRevision"));
    addFact(stateFacts, stateRow, QStringLiteral("Representation"), representationText(state.representationMode()));
    if (provider.value) {
        addFact(stateFacts, stateRow, QStringLiteral("Provider"), providerLifecycleText(provider.value->lifecycle),
                provider.value->ready ? QStringLiteral("#23845a") : statusColor(providerLifecycleText(provider.value->lifecycle)));
        if (provider.value->lastError && provider.value->lastError->message)
            addFact(stateFacts, stateRow, QStringLiteral("Provider error"),
                    fromUtf8(*provider.value->lastError->message), QStringLiteral("#b83a3a"));
    }
    if (controller.value) {
        addFact(stateFacts, stateRow, QStringLiteral("Controller"),
                controller.value->ownedByThisClient
                    ? QStringLiteral("Owned by this frontend")
                    : controller.value->present ? QStringLiteral("Owned by another frontend")
                                                : QStringLiteral("Unowned"));
    }
    if (state.hasPendingRequestProjection()) {
        addFact(stateFacts, stateRow, QStringLiteral("Attention"),
                QStringLiteral("%1 pending request%2").arg(pendingForThread)
                    .arg(pendingForThread == 1 ? QString{} : QStringLiteral("s")),
                pendingForThread > 0 ? QStringLiteral("#a76812") : QStringLiteral("#667085"));
    }
    if (truncation.truncated || (truncation.value && truncation.value->truncated)) {
        QString detail = QStringLiteral("State projection truncated");
        if (truncation.value && truncation.value->omittedEntries)
            detail += QStringLiteral(" · %1 entries omitted").arg(*truncation.value->omittedEntries);
        addFact(stateFacts, stateRow, QStringLiteral("Truncation"), detail, QStringLiteral("#a76812"));
    }
    if (threadList.value)
        addFact(stateFacts, stateRow, QStringLiteral("Thread list"),
                threadList.value->complete ? QStringLiteral("Complete") : QStringLiteral("Partial"),
                threadList.value->complete ? QStringLiteral("#23845a") : QStringLiteral("#a76812"));
    if (!projection.omittedFields.empty() || !projection.redactedFields.empty()) {
        QStringList detail;
        if (!projection.omittedFields.empty())
            detail.append(QStringLiteral("%1 fields omitted").arg(projection.omittedFields.size()));
        if (!projection.redactedFields.empty())
            detail.append(QStringLiteral("%1 fields redacted").arg(projection.redactedFields.size()));
        addFact(stateFacts, stateRow, QStringLiteral("Scope"), detail.join(QStringLiteral(" · ")),
                QStringLiteral("#a76812"));
    }
    if (thread->realtime) {
        const auto realtime = sdk::realtimeSemanticView(*thread->realtime);
        QStringList detail{humanize(fromUtf8(realtime.lifecycle)), QStringLiteral("%1 items").arg(realtime.itemCount)};
        if (realtime.transcriptTruncated)
            detail.append(QStringLiteral("transcript truncated"));
        if (realtime.lastError)
            detail.append(fromUtf8(*realtime.lastError));
        addFact(stateFacts, stateRow, QStringLiteral("Realtime"), detail.join(QStringLiteral(" · ")));
    }
    infoContent->addWidget(stateCard);
    }
    infoContent->addStretch();
    }

    if (planChanged)
        refreshLayoutGeometry(planContent);
    if (agentsChanged)
        refreshLayoutGeometry(agentsContent);
    if (changesChanged)
        refreshLayoutGeometry(changesContent);
    if (infoChanged)
        refreshLayoutGeometry(infoContent);
}

} // namespace codexui
