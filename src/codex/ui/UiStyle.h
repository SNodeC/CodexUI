// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_UISTYLE_H
#define CODEXUI_UI_UISTYLE_H

#include <QString>

namespace codexui::UiStyle {

inline constexpr auto appBackground = "#f6f8fb";
inline constexpr auto panel = "#ffffff";
inline constexpr auto raised = "#f8fafc";
inline constexpr auto sidebar = "#f8fafc";
inline constexpr auto inspector = "#fbfcfe";
inline constexpr auto divider = "#d7dee8";
inline constexpr auto dividerStrong = "#b9c4d2";
inline constexpr auto primary = "#1d2633";
inline constexpr auto secondary = "#667085";
inline constexpr auto placeholder = "#98a2b3";
inline constexpr auto blue = "#2f6feb";
inline constexpr auto blueSelected = "#e5eeff";
inline constexpr auto blueBorder = "#bfd3f9";
inline constexpr auto hover = "#f1f5fb";
inline constexpr auto green = "#23845a";
inline constexpr auto amberSurface = "#fff6df";
inline constexpr auto amberBorder = "#e5c77d";
inline constexpr auto amber = "#a76812";
inline constexpr auto destructive = "#b83a3a";
inline constexpr auto purple = "#6941c6";

QString applicationStyleSheet();

} // namespace codexui::UiStyle

#endif // CODEXUI_UI_UISTYLE_H
