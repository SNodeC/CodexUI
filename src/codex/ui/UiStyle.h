// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_UISTYLE_H
#define CODEXUI_UI_UISTYLE_H

#include <QString>
#include <QToolButton>

class QPaintEvent;
class QRect;
class QWidget;

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
inline constexpr auto blueHover = "#285fca";
inline constexpr auto blueSelected = "#e5eeff";
inline constexpr auto blueBorder = "#bfd3f9";
inline constexpr auto hover = "#f1f5fb";
inline constexpr auto green = "#18865e";
inline constexpr auto greenHover = "#14734f";
inline constexpr auto greenPressed = "#105f41";
inline constexpr auto greenSurface = "#e9f7f0";
inline constexpr auto greenBorder = "#a9d8c1";
inline constexpr auto greenText = "#176b45";
inline constexpr auto orange = "#a85d0c";
inline constexpr auto orangeHover = "#8e4d09";
inline constexpr auto orangePressed = "#743e07";
inline constexpr auto orangeSurface = "#fff6df";
inline constexpr auto orangeSurfaceHover = "#ffefc4";
inline constexpr auto orangeBorder = "#e5c77d";
inline constexpr auto orangeBorderStrong = "#d5ad50";
inline constexpr auto orangeText = "#8a5208";
inline constexpr auto red = "#c43d4d";
inline constexpr auto redHover = "#aa3342";
inline constexpr auto redPressed = "#8f2b38";
inline constexpr auto redSurface = "#fff0f2";
inline constexpr auto redBorder = "#efb8c0";
inline constexpr auto redText = "#982f3d";
inline constexpr auto purple = "#6941c6";

QString applicationStyleSheet();
enum class ChevronDirection { Down, Left, Right };
void drawChevron(QWidget *widget, const QRect &indicator, bool enabled,
                 bool highlighted,
                 ChevronDirection direction = ChevronDirection::Down);

class ChevronToolButton final : public QToolButton {
public:
  using QToolButton::QToolButton;

protected:
  void paintEvent(QPaintEvent *event) override;
};

} // namespace codexui::UiStyle

#endif // CODEXUI_UI_UISTYLE_H
