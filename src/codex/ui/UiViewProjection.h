// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UI_UIVIEWPROJECTION_H
#define CODEXUI_CODEX_UI_UIVIEWPROJECTION_H

#include "codex/ui/UiViewState.h"

#include <functional>
#include <string>
#include <string_view>

namespace codexui::codex {

class PresentationModel;

namespace ui {

[[nodiscard]] ThreadListSnapshot
projectThreadListSnapshot(const PresentationModel &model,
                          std::string selectedThreadId);

[[nodiscard]] InspectorSnapshot projectInspectorSnapshot(
    const PresentationModel &model, std::string selectedThreadId,
    const std::function<bool(std::string_view)> &requestEligible = {});

} // namespace ui
} // namespace codexui::codex

#endif // CODEXUI_CODEX_UI_UIVIEWPROJECTION_H
