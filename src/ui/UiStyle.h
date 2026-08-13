// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_UISTYLE_H
#define CODEXUI_UI_UISTYLE_H

#include <QString>

namespace codexui::UiStyle {

inline constexpr auto appBackground = "#0e1013";
inline constexpr auto panel = "#13161a";
inline constexpr auto raised = "#181c21";
inline constexpr auto divider = "#2b3038";
inline constexpr auto primary = "#e8edf2";
inline constexpr auto secondary = "#949ead";
inline constexpr auto blue = "#4f94f5";
inline constexpr auto green = "#40c27d";
inline constexpr auto amber = "#f5a83b";
inline constexpr auto purple = "#7a63e0";

QString applicationStyleSheet();

} // namespace codexui::UiStyle

#endif // CODEXUI_UI_UISTYLE_H
