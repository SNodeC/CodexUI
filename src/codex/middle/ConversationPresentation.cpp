// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationPresentation.h"

#include "codex/UiStatus.h"
#include "codex/ui/UiStyle.h"

#include <QStringList>

#include <algorithm>
#include <cstddef>

namespace codexui::codex::middle::presentation {
namespace {

constexpr qsizetype MaximumGenericActivityCharacters = 4096;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QStringList textList(const std::vector<std::string> &values) {
  QStringList result;
  result.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string &value : values)
    result.push_back(text(value));
  return result;
}

QString displayChangeKind(std::string_view kind) {
  if (kind.empty())
    return QStringLiteral("Changed");
  return UiStyle::humanizeLabel(text(kind));
}

} // namespace

QString statusLabel(std::string_view status) {
  return text(codexui::codex::displayStatus(status));
}

QString planMarkdown(const PlanData &plan) {
  if (!plan.legacyText.empty())
    return text(plan.legacyText);
  QStringList rows;
  if (!plan.explanation.empty())
    rows << text(plan.explanation);
  if (!plan.steps.empty() && !rows.empty())
    rows << QString{};
  for (const PlanStepData &step : plan.steps) {
    const QString marker = step.status == "completed"    ? QStringLiteral("✓")
                           : step.status == "inProgress" ? QStringLiteral("◉")
                                                         : QStringLiteral("○");
    rows << QStringLiteral("%1 %2  ").arg(marker, text(step.text));
  }
  return rows.join(QLatin1Char('\n'));
}

QString agentMetadata(const AgentActivityData &activity) {
  QStringList metadata;
  if (!activity.tool.empty())
    metadata << text(activity.tool);
  if (activity.status.empty() && !activity.kind.empty())
    metadata << statusLabel(activity.kind);
  if (!activity.receivers.empty())
    metadata << textList(activity.receivers).join(QStringLiteral(", "));
  if (!activity.model.empty())
    metadata << text(activity.model);
  if (!activity.reasoningEffort.empty())
    metadata << text(activity.reasoningEffort);
  if (!activity.childThreadId.empty())
    metadata << QStringLiteral("thread %1").arg(text(activity.childThreadId));
  if (!activity.agentPath.empty())
    metadata << text(activity.agentPath);
  if (!activity.senderThreadId.empty())
    metadata << QStringLiteral("sender %1").arg(text(activity.senderThreadId));
  return metadata.join(QStringLiteral("  |  "));
}

QString fileChangesText(const FileChangesData &changes) {
  QStringList rows;
  for (const FileChangeData &change : changes.changes) {
    if (change.path.empty())
      continue;
    QString row = QStringLiteral("%1  ·  %2")
                      .arg(text(change.path), displayChangeKind(change.kind));
    if (change.additions && change.deletions)
      row += QStringLiteral("  +%1 −%2")
                 .arg(*change.additions)
                 .arg(*change.deletions);
    rows << row;
  }
  return rows.join(QLatin1Char('\n'));
}

QString genericActivityTitle(const GenericActivityData &activity) {
  return activity.type.empty() ? QStringLiteral("Activity")
                               : UiStyle::humanizeLabel(text(activity.type));
}

QString boundedGenericActivityDetail(const GenericActivityData &activity) {
  QString rendered = text(activity.displayDetail);
  if (rendered.size() <= MaximumGenericActivityCharacters)
    return rendered;
  rendered.truncate(MaximumGenericActivityCharacters);
  return rendered + QStringLiteral("\n\n[Activity details truncated]");
}

} // namespace codexui::codex::middle::presentation
