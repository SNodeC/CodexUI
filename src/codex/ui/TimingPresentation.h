// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "codex/ProtocolTiming.h"
#include <QString>

namespace codexui::codex::ui {
QString timingDetails(const ProtocolTimes &times);
QString timingSummary(const ProtocolTimes &times);
} // namespace codexui::codex::ui
