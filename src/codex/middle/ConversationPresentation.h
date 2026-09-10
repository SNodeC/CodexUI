// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H

#include "codex/middle/MiddleTypes.h"

#include <QString>

#include <string_view>

namespace codexui::codex::middle::presentation {

// Pure display-value helpers shared by the passive delegate and the rich card
// editor. They own no state and do not decide which renderer a row uses.
[[nodiscard]] QString statusLabel(std::string_view status);
[[nodiscard]] QString planMarkdown(const PlanData &plan);
[[nodiscard]] QString agentMetadata(const AgentActivityData &activity);
[[nodiscard]] QString fileChangesText(const FileChangesData &changes);
[[nodiscard]] QString genericActivityTitle(const GenericActivityData &activity);
[[nodiscard]] QString
boundedGenericActivityDetail(const GenericActivityData &activity);

} // namespace codexui::codex::middle::presentation

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONPRESENTATION_H
