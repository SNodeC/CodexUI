// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_UISTYLE_H
#define CODEXUI_UI_UISTYLE_H

#include <QString>
#include <QToolButton>

class QPaintEvent;
class QPainter;
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
inline constexpr auto threadInactive = "#cacccf";
// Semantic ramps are precomputed from shared OKLCH role targets. Equivalent
// roles have the same perceptual lightness/chroma and retain the family hue.
// base .550/.090; hover .490/.080; pressed .430/.070;
// surface .968/.014; surface-hover .948/.023; border .850/.065;
// strong-border .770/.105; text .460/.075.
inline constexpr auto blue = "#5471a6";
inline constexpr auto blueHover = "#47608e";
inline constexpr auto bluePressed = "#3a5076";
inline constexpr auto blueSelected = "#e5eefe";
inline constexpr auto blueSurface = "#eff5fe";
inline constexpr auto blueBorder = "#b7cff9";
inline constexpr auto blueBorderStrong = "#8fb4f8";
inline constexpr auto blueText = "#415882";
inline constexpr auto hover = "#f1f5fb";
inline constexpr auto green = "#388262";
inline constexpr auto greenHover = "#2f6f53";
inline constexpr auto greenPressed = "#265c44";
inline constexpr auto greenSurface = "#edf8f2";
inline constexpr auto greenBorder = "#a8dcc1";
inline constexpr auto greenText = "#2a654c";
inline constexpr auto yellow = "#896d2c";
inline constexpr auto yellowHover = "#755d25";
inline constexpr auto yellowPressed = "#614d1d";
inline constexpr auto yellowSurface = "#f9f4ea";
inline constexpr auto yellowSurfaceHover = "#f5eddd";
inline constexpr auto yellowBorder = "#e1cb9d";
inline constexpr auto yellowBorderStrong = "#d2af62";
inline constexpr auto yellowText = "#6b5521";
inline constexpr auto orange = "#986438";
inline constexpr auto orangeHover = "#82552f";
inline constexpr auto orangePressed = "#6c4626";
inline constexpr auto orangeSurface = "#fcf2eb";
inline constexpr auto orangeSurfaceHover = "#faebdf";
inline constexpr auto orangeBorder = "#efc4a4";
inline constexpr auto orangeBorderStrong = "#e6a46f";
inline constexpr auto orangeText = "#774d2b";
inline constexpr auto red = "#9f5b5d";
inline constexpr auto redHover = "#884d4f";
inline constexpr auto redPressed = "#713f41";
inline constexpr auto redSurface = "#fef1f1";
inline constexpr auto redBorder = "#f6bdbe";
inline constexpr auto redText = "#7d4648";
inline constexpr auto purple = "#7268a2";
inline constexpr auto purpleHover = "#61588a";
inline constexpr auto purplePressed = "#504873";
inline constexpr auto purpleSurface = "#f4f3fd";
inline constexpr auto purpleBorder = "#cec7f6";
inline constexpr auto purpleText = "#59507f";
inline constexpr auto teal = "#138282";
inline constexpr auto tealHover = "#0f6e6e";
inline constexpr auto tealPressed = "#0b5c5c";
inline constexpr auto tealSurface = "#eaf8f7";
inline constexpr auto tealBorder = "#9bdcdb";
inline constexpr auto tealBorderStrong = "#53c9c9";
inline constexpr auto tealText = "#0d6565";

QString applicationStyleSheet();
QString humanizeLabel(QString value);
enum class ChevronDirection { Down, Left, Right };
void drawChevron(QPainter &painter, const QRect &indicator, bool enabled,
                 bool highlighted,
                 ChevronDirection direction = ChevronDirection::Down);
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
