// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "TimingPresentation.h"
#include <QDateTime>
#include <QStringList>
#include <QTimeZone>
#include <algorithm>

namespace codexui::codex::ui {
namespace {
QDateTime date(const ProtocolTime &time) {
  if (!time.value || (time.unavailableZero && *time.value == 0) ||
      time.unit == TimeUnit::DurationMilliseconds)
    return {};
  // Limit to the ISO civil range before multiplying; never guess epoch units.
  constexpr qint64 LastSecond = 253402300799;
  constexpr qint64 FirstSecond = -62135596800;
  const qint64 scale = time.unit == TimeUnit::Seconds ? 1 : 1000;
  if (*time.value < FirstSecond * scale ||
      *time.value > LastSecond * scale + scale - 1)
    return {};
  return QDateTime::fromMSecsSinceEpoch(
      time.unit == TimeUnit::Seconds ? *time.value * 1000 : *time.value,
      QTimeZone::UTC);
}
QString duration(qint64 ms) {
  return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 3);
}
} // namespace

QString timingDetails(const ProtocolTimes &times) {
  QStringList lines;
  for (const auto &time : times) {
    const QString field = QString::fromStdString(time.field);
    QString value = QStringLiteral("Not supplied");
    if (time.unit == TimeUnit::DurationMilliseconds && time.value)
      value = *time.value >= 0 ? duration(*time.value)
                               : QStringLiteral("Invalid duration");
    else if (const QDateTime utc = date(time); utc.isValid()) {
      const auto local = utc.toLocalTime();
      value = local.toOffsetFromUtc(local.offsetFromUtc())
                  .toString(Qt::ISODateWithMs) +
              QStringLiteral(" (") + local.timeZoneAbbreviation() +
              QStringLiteral(")\nUTC: ") + utc.toString(Qt::ISODateWithMs);
    } else if (time.value)
      value = time.unavailableZero && *time.value == 0
                  ? QStringLiteral("Unavailable (protocol sentinel)")
                  : QStringLiteral("Invalid / outside supported date range");
    if (time.value)
      value += QStringLiteral("\nRaw: %1 %2")
                   .arg(*time.value)
                   .arg(time.unit == TimeUnit::Seconds
                            ? QStringLiteral("Unix seconds")
                        : time.unit == TimeUnit::Milliseconds
                            ? QStringLiteral("Unix milliseconds")
                            : QStringLiteral("milliseconds (duration)"));
    lines << field + QStringLiteral(": ") + value;
  }
  return lines.isEmpty()
             ? QStringLiteral("No timestamps supplied for this object.")
             : lines.join(QStringLiteral("\n\n"));
}

QString timingSummary(const ProtocolTimes &times) {
  QString start, end, elapsed;
  const bool turn = std::ranges::any_of(
      times, [](const auto &t) { return t.field.starts_with("turn/"); });
  for (const auto &time : times) {
    if (time.field.starts_with("turn/") != turn)
      continue;
    if (time.unit == TimeUnit::DurationMilliseconds) {
      if (time.value && *time.value >= 0)
        elapsed = duration(*time.value);
      continue;
    }
    const auto local = date(time).toLocalTime();
    if (!local.isValid())
      continue;
    if (time.field.find("startedAt") != std::string::npos)
      start = local.toString(QStringLiteral("HH:mm:ss"));
    if (time.field.find("completedAt") != std::string::npos)
      end = local.toString(QStringLiteral("HH:mm:ss"));
  }
  QString result = start;
  if (!end.isEmpty())
    result += (result.isEmpty() ? QString{} : QStringLiteral("–")) + end;
  if (!elapsed.isEmpty())
    result += (result.isEmpty() ? QString{} : QStringLiteral(" · ")) + elapsed;
  return result;
}
} // namespace codexui::codex::ui
