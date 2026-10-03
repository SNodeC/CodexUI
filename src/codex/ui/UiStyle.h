// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_UISTYLE_H
#define CODEXUI_UI_UISTYLE_H

#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QString>
#include <QToolButton>

class QPaintEvent;
class QPainter;
class QRect;
class QScrollArea;
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
inline constexpr auto strongText = "#344054";
inline constexpr auto secondary = "#667085";
inline constexpr auto secondaryStrong = "#475467";
inline constexpr auto placeholder = "#98a2b3";
inline constexpr auto threadInactive = "#cacccf";
inline constexpr auto onAccent = panel;
inline constexpr auto neutralSurface = "#eef1f5";
inline constexpr auto neutralSurfaceHover = "#e3e8ef";
inline constexpr auto neutralBorder = "#c8d0dc";
inline constexpr auto neutralBorderHover = "#aeb8c6";
inline constexpr auto neutralBorderPressed = "#9eabbc";
inline constexpr auto codeSurface = "#111827";
inline constexpr auto codeText = "#e5e7eb";
inline constexpr auto diffHunkSurface = "#edf3ff";
inline constexpr auto activeTurnBorder = "#6f98e8";
inline constexpr auto brandAccent = "#63d5a5";
inline constexpr QRgb pendingSteeringSweepEdge = qRgba(22, 123, 128, 0);
inline constexpr QRgb pendingSteeringSweepCenter = qRgba(92, 180, 184, 105);
inline constexpr QRgb pendingPromptSweepEdge = qRgba(47, 111, 235, 0);
inline constexpr QRgb pendingPromptSweepCenter = qRgba(117, 160, 239, 105);
inline constexpr int commandOutputHorizontalPadding = 7;
inline constexpr int commandOutputVerticalPadding = 4;
inline constexpr int projectCardEdgePadding = 8;
// Semantic ramps are precomputed from shared OKLCH role targets. Equivalent
// roles have the same perceptual lightness/chroma and retain the family hue.
// base .550/.090; hover .490/.080; pressed .430/.070;
// surface .968/.014; surface-hover .948/.023; border .850/.065;
// strong-border .770/.105; text .460/.075.
// Selected aliases surface-hover; selected-hover and border-hover preserve
// the existing blue role's OKLCH lightness/chroma across all family hues.
inline constexpr auto blue = "#5471a6";
inline constexpr auto blueHover = "#47608e";
inline constexpr auto bluePressed = "#3a5076";
inline constexpr auto blueSelected = "#e5eefe";
inline constexpr auto blueSelectedHover = "#d8e7ff";
inline constexpr auto blueSurface = "#eff5fe";
inline constexpr auto blueBorder = "#b7cff9";
inline constexpr auto blueBorderHover = "#9ebcf3";
inline constexpr auto blueBorderStrong = "#8fb4f8";
inline constexpr auto blueText = "#415882";
inline constexpr auto blueSurfaceHover = blueSelected;
inline constexpr auto hover = "#f1f5fb";
inline constexpr auto green = "#388262";
inline constexpr auto greenHover = "#2f6f53";
inline constexpr auto greenPressed = "#265c44";
inline constexpr auto greenSurface = "#edf8f2";
inline constexpr auto greenBorder = "#a8dcc1";
inline constexpr auto greenText = "#2a654c";
inline constexpr auto greenSurfaceHover = "#e1f3e9";
inline constexpr auto greenSelected = greenSurfaceHover;
inline constexpr auto greenSelectedHover = "#d1eedf";
inline constexpr auto greenBorderHover = "#88cdab";
inline constexpr auto greenBorderStrong = "#71c9a1";
inline constexpr auto amber = "#896d2c";
inline constexpr auto amberHover = "#755d25";
inline constexpr auto amberPressed = "#614d1d";
inline constexpr auto amberSurface = "#f9f4ea";
inline constexpr auto amberSurfaceHover = "#f5eddd";
inline constexpr auto amberBorder = "#e1cb9d";
inline constexpr auto amberBorderStrong = "#d2af62";
inline constexpr auto amberText = "#6b5521";
inline constexpr auto amberSelected = amberSurfaceHover;
inline constexpr auto amberSelectedHover = "#f1e5cb";
inline constexpr auto amberBorderHover = "#d4b87b";
inline constexpr auto orange = "#986438";
inline constexpr auto orangeHover = "#82552f";
inline constexpr auto orangePressed = "#6c4626";
inline constexpr auto orangeSurface = "#fcf2eb";
inline constexpr auto orangeSurfaceHover = "#faebdf";
inline constexpr auto orangeBorder = "#efc4a4";
inline constexpr auto orangeBorderStrong = "#e6a46f";
inline constexpr auto orangeText = "#774d2b";
inline constexpr auto orangeSelected = orangeSurfaceHover;
inline constexpr auto orangeSelectedHover = "#f9e1ce";
inline constexpr auto orangeBorderHover = "#e5af84";
inline constexpr auto red = "#9f5b5d";
inline constexpr auto redHover = "#884d4f";
inline constexpr auto redPressed = "#713f41";
inline constexpr auto redSurface = "#fef1f1";
inline constexpr auto redBorder = "#f6bdbe";
inline constexpr auto redText = "#7d4648";
inline constexpr auto redSurfaceHover = "#fde8e8";
inline constexpr auto redSelected = redSurfaceHover;
inline constexpr auto redSelectedHover = "#fedddd";
inline constexpr auto redBorderHover = "#eda6a6";
inline constexpr auto redBorderStrong = "#f0999a";
inline constexpr auto purple = "#7268a2";
inline constexpr auto purpleHover = "#61588a";
inline constexpr auto purplePressed = "#504873";
inline constexpr auto purpleSurface = "#f4f3fd";
inline constexpr auto purpleBorder = "#cec7f6";
inline constexpr auto purpleText = "#59507f";
inline constexpr auto purpleSurfaceHover = "#edebfd";
inline constexpr auto purpleSelected = purpleSurfaceHover;
inline constexpr auto purpleSelectedHover = "#e5e2fd";
inline constexpr auto purpleBorderHover = "#bbb2ef";
inline constexpr auto purpleBorderStrong = "#b4a8f2";
inline constexpr auto teal = "#138282";
inline constexpr auto tealHover = "#0f6e6e";
inline constexpr auto tealPressed = "#0b5c5c";
inline constexpr auto tealSurface = "#eaf8f7";
inline constexpr auto tealBorder = "#9bdcdb";
inline constexpr auto tealBorderStrong = "#53c9c9";
inline constexpr auto tealText = "#0d6565";
inline constexpr auto tealSurfaceHover = "#ddf3f2";
inline constexpr auto tealSelected = tealSurfaceHover;
inline constexpr auto tealSelectedHover = "#cbeeed";
inline constexpr auto tealBorderHover = "#75cdcc";

inline constexpr auto lime = "#657b3d";
inline constexpr auto limeHover = "#566833";
inline constexpr auto limePressed = "#47562a";
inline constexpr auto limeSurface = "#f2f6ec";
inline constexpr auto limeSurfaceHover = "#eaf1e0";
inline constexpr auto limeSelected = limeSurfaceHover;
inline constexpr auto limeSelectedHover = "#e0ebd1";
inline constexpr auto limeBorder = "#c4d6a8";
inline constexpr auto limeBorderHover = "#afc689";
inline constexpr auto limeBorderStrong = "#a5c075";
inline constexpr auto limeText = "#4e5f2f";

// Yellow at 110 degrees OKLCH hue; selection uses a brighter pure-yellow tint.
inline constexpr auto yellow = "#757633";
inline constexpr auto yellowHover = "#63642a";
inline constexpr auto yellowPressed = "#525322";
inline constexpr auto yellowSurface = "#f5f5eb";
inline constexpr auto yellowSurfaceHover = "#eeefde";
inline constexpr auto yellowSelected = "#ffffb3";
inline constexpr auto yellowSelectedHover = "#e7e9cd";
inline constexpr auto yellowBorder = "#d0d2a1";
inline constexpr auto yellowBorderHover = "#bec180";
inline constexpr auto yellowBorderStrong = "#b8ba69";
inline constexpr auto yellowText = "#5b5c26";

// Active navigation context uses the complete lime family.
inline constexpr auto activeGroupSurface = limeSurface;
inline constexpr auto activeGroupBorder = limeBorderStrong;
inline constexpr auto threadSelected = yellowSelected;
inline constexpr auto threadSelectedBorder = yellowBorder;
inline constexpr auto activeThreadBorder = yellowBorderStrong;

QString applicationStyleSheet();
QString humanizeLabel(QString value);
[[nodiscard]] bool animationsEnabled(const QWidget &widget);
QLabel *makeLabel(QString value, const char *kind = "body",
                  QWidget *parent = nullptr);
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

class ScrollFormDialog : public QDialog {
public:
  explicit ScrollFormDialog(QWidget *parent = nullptr);
  QScrollArea *formScrollArea() const { return scroll_; }
  void revealFormFocus(QWidget *control);

protected:
  bool event(QEvent *event) override;
  bool focusNextPrevChild(bool next) override;

private:
  QScrollArea *scroll_;
};

// Dialog editors keep the active insertion point visible across geometry changes.
class DialogTextEdit final : public QPlainTextEdit {
public:
  explicit DialogTextEdit(QWidget *parent = nullptr);

protected:
  void changeEvent(QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
};

class TokenUsageLabel final : public QLabel {
public:
  explicit TokenUsageLabel(QWidget *parent = nullptr);
  ~TokenUsageLabel() override;
  void setUsage(QString summary, QString details);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  int heightForWidth(int width) const override;
  bool hasHeightForWidth() const override { return true; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  QString wrappedText(int width) const;
  QString summary_;
  QLabel *details_;
};

class ChevronComboBox final : public QComboBox {
public:
  explicit ChevronComboBox(QWidget *parent = nullptr) : QComboBox(parent) {
    setProperty("codexChevron", true);
  }

protected:
  void paintEvent(QPaintEvent *event) override;
};

} // namespace codexui::UiStyle

#endif // CODEXUI_UI_UISTYLE_H
