// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <QFontInfo>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionToolButton>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace codexui::UiStyle {

bool animationsEnabled(const QWidget &widget) {
  return widget.style()->styleHint(QStyle::SH_Widget_Animation_Duration,
                                   nullptr, &widget) > 0;
}

QLabel *makeLabel(QString value, const char *kind, QWidget *parent) {
  auto *label = new QLabel(std::move(value), parent);
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

void drawChevron(QPainter &painter, const QRect &indicator, bool enabled,
                 bool highlighted, ChevronDirection direction) {
  if (!indicator.isValid() || indicator.isEmpty())
    return;
  const QPointF center = indicator.center();
  QPainterPath chevron;
  if (direction == ChevronDirection::Right) {
    chevron.moveTo(center.x() - 1.5, center.y() - 3.5);
    chevron.lineTo(center.x() + 2.0, center.y());
    chevron.lineTo(center.x() - 1.5, center.y() + 3.5);
  } else if (direction == ChevronDirection::Left) {
    chevron.moveTo(center.x() + 1.5, center.y() - 3.5);
    chevron.lineTo(center.x() - 2.0, center.y());
    chevron.lineTo(center.x() + 1.5, center.y() + 3.5);
  } else {
    chevron.moveTo(center.x() - 3.5, center.y() - 1.5);
    chevron.lineTo(center.x(), center.y() + 2.0);
    chevron.lineTo(center.x() + 3.5, center.y() - 1.5);
  }

  QColor color(QString::fromLatin1(secondary));
  if (!enabled)
    color = QColor(QString::fromLatin1(placeholder));
  else if (highlighted)
    color = QColor(QString::fromLatin1(primary));

  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(chevron);
}

void drawChevron(QWidget *widget, const QRect &indicator, bool enabled,
                 bool highlighted, ChevronDirection direction) {
  QPainter painter(widget);
  drawChevron(painter, indicator, enabled, highlighted, direction);
}

void ChevronToolButton::paintEvent(QPaintEvent *event) {
  QToolButton::paintEvent(event);
  QStyleOptionToolButton option;
  initStyleOption(&option);
  const QRect contents =
      style()->subElementRect(QStyle::SE_ToolButtonLayoutItem, &option, this);
  const int indicatorWidth =
      style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, this);
  const QRect indicator(contents.right() - std::max(12, indicatorWidth),
                        contents.top(), std::max(12, indicatorWidth),
                        contents.height());
  drawChevron(this, indicator, option.state & QStyle::State_Enabled,
              option.state &
                  (QStyle::State_MouseOver | QStyle::State_HasFocus));
}

void ChevronComboBox::paintEvent(QPaintEvent *event) {
  QComboBox::paintEvent(event);
  QStyleOptionComboBox option;
  initStyleOption(&option);
  const QRect indicator = style()->subControlRect(
      QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
  drawChevron(this, indicator, option.state & QStyle::State_Enabled,
              option.state &
                  (QStyle::State_MouseOver | QStyle::State_HasFocus));
}

QString applicationStyleSheet() {
  const qreal configuredSize = QFontInfo(QApplication::font()).pointSizeF();
  const qreal baseSize = configuredSize > 0.0 ? configuredSize : 10.0;
  const QString compact =
      QString::number(std::max(1.0, baseSize - 1.0), 'f', 1);
  const QString standard = QString::number(baseSize, 'f', 1);
  const QString section = QString::number(baseSize + 1.0, 'f', 1);
  const QString panelHeader = QString::number(baseSize + 1.0, 'f', 1);
  const QString heading = QString::number(baseSize + 3.0, 'f', 1);

  QString style = QStringLiteral(R"QSS(
        * {
            color: %{primary};
            font-size: %{compact}pt;
        }
        QMainWindow, QWidget#applicationShell { background: %{appBackground}; }
        QFrame#topBar { background: %{panel}; border-bottom: 1px solid %{divider}; }
        QLabel#workspaceBreadcrumb { color: %{secondary}; font-weight: 500; }
        QFrame#customStatusBar { background: %{raised}; border-top: 1px solid %{divider}; }
        QFrame[kind="statusDot"] { background: %{placeholder}; border-radius: 5px; }
        QFrame[kind="statusDot"][tone="active"] { background: %{blue}; }
        QFrame[kind="statusDot"][tone="success"] { background: %{green}; }
        QFrame[kind="statusDot"][tone="warning"] { background: %{orange}; }
        QFrame[kind="statusDot"][tone="danger"] { background: %{red}; }
        QWidget#codexTurnSettings { background: %{panel}; border-top: 1px solid %{divider}; }
        QFrame#inspector { background: %{inspector}; }
        QFrame#inspector QScrollBar:vertical {
            background: %{inspector};
            border: 2px solid %{inspector};
            width: 8px;
            margin: 0;
        }
        QFrame#inspector QScrollBar:horizontal {
            background: %{inspector};
            border: 2px solid %{inspector};
            height: 8px;
            margin: 0;
        }
        QLabel { background: transparent; font-weight: 400; }
        QLabel[kind="muted"] { color: %{secondary}; font-size: %{compact}pt; }
        QLabel[kind="section"] {
            color: %{secondary};
            font-size: %{section}pt;
            font-weight: 600;
        }
        QLabel[kind="panelHeader"] {
            color: %{secondaryStrong};
            font-size: %{panelHeader}pt;
            font-weight: 700;
        }
        QLabel[kind="attentionSection"] {
            color: %{orange};
            font-size: %{compact}pt;
            font-weight: 600;
        }
        QLabel[kind="heading"] { font-size: %{heading}pt; font-weight: 600; }
        QLabel[kind="applicationTitle"] { font-weight: 700; }
        QLabel[kind="brand"] { font-size: %{section}pt; font-weight: 600; }
        QLabel[kind="title"] { font-size: %{standard}pt; font-weight: 600; }
        QLabel[kind="messagePhase"] { font-size: %{standard}pt; font-weight: 400; }
        QLabel[kind="messagePhase"][tone="steering"] { color: %{tealText}; }
        QLabel[kind="body"] { font-size: %{standard}pt; }
        QLabel[kind="code"] { font-family: monospace; font-size: %{standard}pt; font-weight: 400; }
        QLabel[kind="meta"] { color: %{secondary}; font-size: %{compact}pt; }
        QLabel[kind="meta"][tone="strong"] { color: %{primary}; }
        QLabel[kind="small"] { color: %{secondary}; font-size: %{compact}pt; }
        QLabel[kind="settingLabel"] { color: %{secondary}; font-weight: 600; }
        QLabel[tone="active"] { color: %{blueText}; }
        QLabel[tone="success"] { color: %{greenText}; }
        QLabel[tone="warning"] { color: %{orangeText}; }
        QLabel[tone="danger"] { color: %{redText}; }
        QLabel[kind="imageThumbnail"] {
          background: %{raised};
          border: 1px solid %{divider};
          border-radius: 6px;
          padding: 3px;
        }
        QLabel[kind="diffAdditionMeta"] { color: %{greenText}; font-size: %{compact}pt; font-weight: 600; }
        QLabel[kind="diffDeletionMeta"] { color: %{redText}; font-size: %{compact}pt; font-weight: 600; }
        QPushButton, QToolButton {
            background: %{panel};
            border: 1px solid %{divider};
            border-radius: 7px;
            padding: 0 12px;
            font-size: %{compact}pt;
            font-weight: 600;
        }
        QPushButton[comboPeer="true"] { min-height: 30px; max-height: 30px; }
        QPushButton:hover, QToolButton:hover { background: %{hover}; border-color: %{dividerStrong}; }
        QPushButton:pressed, QToolButton:pressed { background: %{blueSelected}; border-color: %{blueBorder}; }
        QPushButton:focus, QToolButton:focus { border: 2px solid %{blue}; }
        QPushButton:disabled, QToolButton:disabled { color: %{placeholder}; background: %{appBackground}; border-color: %{divider}; }
        QPushButton[kind="primary"] { background: %{blue}; border-color: %{blue}; color: %{onAccent}; }
        QPushButton[kind="primary"]:hover { background: %{blueHover}; border-color: %{blueHover}; }
        QPushButton[kind="primary"]:pressed { background: %{bluePressed}; border-color: %{bluePressed}; }
        QPushButton[kind="primary"]:disabled { color: %{placeholder}; background: %{appBackground}; border-color: %{divider}; }
        QPushButton[kind="history"] { background: %{blueSelected}; border-color: %{blueBorder}; color: %{blueText}; }
        QPushButton[kind="history"]:hover { background: %{blueSelectedHover}; border-color: %{blueBorderHover}; }
        QPushButton[kind="request"] { background: %{orangeSurface}; border-color: %{orangeBorder}; color: %{orangeText}; }
        QPushButton[kind="request"]:hover { background: %{orangeSurfaceHover}; border-color: %{orangeBorderStrong}; }
        QPushButton[kind="steer"] { background: %{teal}; border-color: %{teal}; color: %{onAccent}; }
        QPushButton[kind="steer"]:hover { background: %{tealHover}; border-color: %{tealHover}; color: %{onAccent}; }
        QPushButton[kind="steer"]:pressed { background: %{tealPressed}; border-color: %{tealPressed}; color: %{onAccent}; }
        QPushButton[kind="steer"]:disabled { color: %{placeholder}; background: %{appBackground}; border-color: %{divider}; }
        QPushButton[kind="cancel"] { background: %{neutralSurface}; border-color: %{neutralBorder}; color: %{secondaryStrong}; }
        QPushButton[kind="cancel"]:hover { background: %{neutralSurfaceHover}; border-color: %{neutralBorderHover}; }
        QPushButton[kind="subtle"], QToolButton[kind="subtle"] {
            color: %{secondary};
            background: transparent;
            border-color: transparent;
        }
        QPushButton[kind="infoChoice"] {
          background: %{panel};
          border: 1px solid %{divider};
          border-radius: 10px;
          padding: 0;
          text-align: left;
        }
        QPushButton[kind="infoChoice"]:hover { background: %{raised}; border-color: %{dividerStrong}; }
        QPushButton[kind="infoChoice"]:pressed { background: %{hover}; border-color: %{neutralBorderPressed}; }
        QPushButton[kind="segment"] {
          background: %{panel};
          border-color: %{divider};
          border-radius: 7px;
          padding: 0 10px;
        }
        QPushButton[kind="segment"]:checked {
          background: %{blueSelected};
          border-color: %{blueBorder};
          color: %{primary};
        }
        QPushButton[kind="segment"]:hover:!checked { background: %{hover}; }
        QToolButton[kind="presentationToggle"] {
          background: %{panel};
          border: 1px solid %{divider};
          border-radius: 7px;
          padding: 0;
        }
        QToolButton[kind="presentationToggle"]:checked {
          background: %{blueSelected};
          border-color: %{blueBorder};
        }
        QToolButton[kind="presentationToggle"]:hover:!checked {
          background: %{hover};
          border-color: %{dividerStrong};
        }
        QToolButton[kind="composerAction"] {
            background: %{panel};
            border: 1px solid %{divider};
            color: %{secondary};
            padding: 0;
        }
        QToolButton[kind="composerAction"]:hover { background: %{hover}; border-color: %{dividerStrong}; }
        QPushButton[kind="agentLink"] { background: %{blueSelected}; border-color: %{blueBorder}; color: %{blue}; text-align: left; }
        QPushButton[kind="success"] { background: %{green}; border-color: %{green}; color: %{onAccent}; }
        QPushButton[kind="success"]:hover { background: %{greenHover}; border-color: %{greenHover}; }
        QPushButton[kind="success"]:pressed { background: %{greenPressed}; border-color: %{greenPressed}; }
        QPushButton[kind="destructive"], QPushButton[kind="stop"] { background: %{red}; border-color: %{red}; color: %{onAccent}; }
        QPushButton[kind="destructive"]:hover, QPushButton[kind="stop"]:hover { background: %{redHover}; border-color: %{redHover}; }
        QPushButton[kind="destructive"]:pressed, QPushButton[kind="stop"]:pressed { background: %{redPressed}; border-color: %{redPressed}; }
        QPushButton[kind="destructiveCompact"] { background: %{red}; border: 0; color: %{onAccent}; border-radius: 4px; padding: 0; font-weight: 700; }
        QPushButton[kind="destructiveCompact"]:hover { background: %{redHover}; }
        QPushButton[kind="destructiveCompact"]:pressed { background: %{redPressed}; }
        QPushButton[codexChevron="true"] { padding-right: 20px; }
        QPushButton[codexChevron="true"]::menu-indicator { image: none; width: 0; }
        QToolButton[codexChevron="true"] { padding-right: 20px; }
        QToolButton[codexChevron="true"]::menu-indicator { image: none; width: 0; }
        QFrame[kind="panel"] { background: %{panel}; }
        QFrame#conversation { background: %{appBackground}; }
        QFrame#attachmentFileBox { background: %{panel}; border: 1px solid %{divider}; border-radius: 6px; }
        QFrame[kind="raised"] { background: %{panel}; border: 1px solid %{divider}; border-radius: 10px; }
        QFrame[kind="raised"][tone="warning"] { background: %{orangeSurface}; border-color: %{orangeBorder}; }
        QFrame[messageRole="user"] { background: %{blueSurface}; border: 1px solid %{blueBorder}; border-radius: 8px; }
        QFrame[messageRole="user"] QLabel[kind="title"] { color: %{blueText}; }
        QFrame[messageRole="user"][nestedConversationCard="true"] { background: %{tealSurface}; border-color: %{tealBorder}; }
        QFrame[messageRole="user"][nestedConversationCard="true"] QLabel[kind="title"] { color: %{tealText}; }
        QFrame[virtualTurnRoot="true"] { background: transparent; border: none; }
        QFrame[messageRole="agent"][messagePhase="final"] { background: %{purpleSurface}; border: 1px solid %{purpleBorder}; border-radius: 8px; }
        QFrame[messageRole="agent"][messagePhase="final"] QLabel[kind="title"] { color: %{purpleText}; }
        QFrame[messageRole="agent"][messagePhase="update"] { background: %{yellowSurface}; border: 1px solid %{yellowBorder}; border-radius: 8px; }
        QFrame[messageRole="agent"][messagePhase="update"] QLabel[kind="title"] { color: %{yellowText}; }
        QFrame[kind="summary"] { background: %{raised}; border: 1px solid %{divider}; border-radius: 7px; }
        QFrame[kind="standardDivider"] { background: %{divider}; border: none; }
        QFrame[kind="greenBadge"] { background: %{greenSurface}; border: 1px solid %{greenBorder}; border-radius: 6px; }
        QFrame[kind="blueBadge"] { background: %{blueSelected}; border-radius: 5px; }
        QFrame[kind="orangeBadge"] { background: %{orangeSurface}; border: 1px solid %{orangeBorder}; border-radius: 7px; }
        QFrame#conversationNoticeBar[tone="warning"] { background: %{orangeSurface}; border: 1px solid %{orangeBorder}; border-radius: 7px; }
        QFrame#conversationNoticeBar[tone="danger"] { background: %{redSurface}; border: 1px solid %{redBorder}; border-radius: 7px; }
        QWidget#composerOverlay { background: %{appBackground}; }
        QFrame[kind="composer"] { background: %{panel}; border: 1px solid %{divider}; border-radius: 10px; }
        QFrame[kind="composer"][focused="true"] { border-color: %{blue}; }
        QPlainTextEdit, QTextEdit {
            background: transparent;
            border: 0;
            color: %{primary};
            font-size: %{standard}pt;
            padding: 0;
            selection-background-color: %{blueSelected};
            selection-color: %{primary};
        }
        QPlainTextEdit[empty="true"] { color: %{placeholder}; }
        QPlainTextEdit#upcomingPromptEditor {
            background: transparent;
            color: %{primary};
            border: 0;
            padding: 3px 2px;
        }
        QTextBrowser#markdownTextView,
        QPlainTextEdit#fileChangesList {
            background: transparent;
            border: 0;
            padding: 0;
            margin: 0;
        }
        QScrollArea#messageImages {
            background: %{codeSurface};
            border: 1px solid %{divider};
            border-radius: 6px;
        }
        QWidget#messageImageStrip { background: %{codeSurface}; }
        QTextEdit#commandOutputView {
            background: %{codeSurface};
            color: %{codeText};
            border-radius: 6px;
            padding: %{commandOutputVerticalPadding}px %{commandOutputHorizontalPadding}px;
        }
        QTextEdit#commandTextView {
            background: %{raised};
            border: 1px solid %{divider};
            border-radius: 6px;
        }
        QFrame#pendingPromptCard {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 8px;
        }
        QLineEdit#codexWorkspace { min-height: 30px; }
        QPlainTextEdit[kind="code"], QPlainTextEdit[kind="command"],
        QTextEdit[kind="code"], QTextEdit[kind="command"],
        QPlainTextEdit[kind="infoViewer"] {
            font-family: monospace;
            font-size: %{compact}pt;
        }
        QPlainTextEdit[kind="infoViewer"] {
            background: %{raised};
            border: 1px solid %{divider};
            border-radius: 7px;
            padding: 7px;
        }
        QPlainTextEdit[kind="dialogEditor"] {
            background: %{panel};
            border: 1px solid %{divider};
            border-radius: 7px;
            padding: 8px;
        }
        QPlainTextEdit[kind="dialogEditor"]:focus { border-color: %{blue}; }
        QLineEdit {
            background: %{panel};
            border: 1px solid %{divider};
            border-radius: 7px;
            min-height: 32px;
            padding: 0 9px;
            selection-background-color: %{blueSelected};
            selection-color: %{primary};
        }
        QLineEdit:focus { border-color: %{blue}; }
        QLineEdit:disabled { color: %{placeholder}; background: %{appBackground}; }
        QComboBox {
            background: %{panel};
            border: 1px solid %{divider};
            border-radius: 7px;
            min-height: 30px;
            padding: 0 24px 0 9px;
        }
        QComboBox:hover { border-color: %{dividerStrong}; }
        QComboBox:focus { border-color: %{blue}; }
        QComboBox:disabled { color: %{placeholder}; background: %{appBackground}; }
        QComboBox QLineEdit {
            background: transparent;
            border: 0;
            border-radius: 0;
            min-height: 0;
            padding: 0;
        }
        QComboBox::drop-down { border: 0; width: 20px; }
        QComboBox[codexChevron="true"]::down-arrow { image: none; }
        QComboBox QAbstractItemView {
            background: %{panel};
            color: %{primary};
            border: 1px solid %{divider};
            selection-background-color: %{blueSelected};
            selection-color: %{primary};
        }
        QTreeView#codexFileBrowser, QListWidget#codexAttachmentList {
            background: %{panel};
            alternate-background-color: %{raised};
            border: 1px solid %{divider};
            border-radius: 7px;
            outline: 0;
        }
        QListWidget#codexDiffFiles {
            background: %{panel};
            border: 1px solid %{divider};
            border-radius: 7px;
            outline: 0;
        }
        QListWidget#codexDiffFiles::item {
            min-height: 27px;
            padding: 3px 7px;
        }
        QListWidget#codexDiffFiles::item:hover { background: %{hover}; }
        QListWidget#codexDiffFiles::item:selected {
            background: %{blueSelected};
            color: %{primary};
        }
        QPlainTextEdit#codexDiffText {
            background: %{panel};
            border: 1px solid %{divider};
            border-radius: 7px;
            padding: 7px;
            font-size: %{compact}pt;
        }
        QTreeView#codexFileBrowser::item, QListWidget#codexAttachmentList::item {
            min-height: 28px;
            padding: 3px 7px;
        }
        QTreeView#codexFileBrowser::item:hover,
        QListWidget#codexAttachmentList::item:hover { background: %{hover}; }
        QTreeView#codexFileBrowser::item:selected,
        QListWidget#codexAttachmentList::item:selected {
            background: %{blueSelected};
            color: %{primary};
        }
        QHeaderView::section {
            background: %{raised};
            color: %{secondary};
            border: 0;
            border-bottom: 1px solid %{divider};
            padding: 7px;
            font-size: %{compact}pt;
            font-weight: 600;
        }
        QCheckBox, QRadioButton { spacing: 8px; }
        QDialog { background: %{panel}; }
        QScrollArea { background: %{appBackground}; border: 0; }
        QTabWidget QScrollArea { background: %{inspector}; }
        QAbstractScrollArea[kind="inspectorScroll"],
        QAbstractScrollArea[kind="inspectorScroll"] > QWidget > QWidget {
          background: transparent;
          border: 0;
        }
        QDialog QScrollArea { background: %{panel}; }
        QScrollArea > QWidget > QWidget { background: transparent; }
        QAbstractScrollArea::corner { background: transparent; border: 0; }
        QScrollBar:vertical {
            background: transparent;
            border: 0;
            width: 8px;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: %{dividerStrong};
            min-height: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover { background: %{placeholder}; }
        QScrollBar::add-line:vertical {
            background: transparent;
            border: 0;
            height: 0;
            subcontrol-position: bottom;
            subcontrol-origin: margin;
        }
        QScrollBar::sub-line:vertical {
            background: transparent;
            border: 0;
            height: 0;
            subcontrol-position: top;
            subcontrol-origin: margin;
        }
        QScrollBar::up-arrow:vertical, QScrollBar::down-arrow:vertical {
            background: none;
            border: 0;
            width: 0;
            height: 0;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: none;
            border: 0;
        }
        QScrollBar:horizontal {
            background: transparent;
            border: 0;
            height: 8px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal {
            background: %{dividerStrong};
            min-width: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:horizontal:hover { background: %{placeholder}; }
        QScrollBar::add-line:horizontal {
            background: transparent;
            border: 0;
            width: 0;
            subcontrol-position: right;
            subcontrol-origin: margin;
        }
        QScrollBar::sub-line:horizontal {
            background: transparent;
            border: 0;
            width: 0;
            subcontrol-position: left;
            subcontrol-origin: margin;
        }
        QScrollBar::left-arrow:horizontal, QScrollBar::right-arrow:horizontal {
            background: none;
            border: 0;
            width: 0;
            height: 0;
        }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            background: none;
            border: 0;
        }
        QSplitter::handle { background: %{divider}; }
        QSplitter::handle:horizontal { width: 8px; }
        QTabBar { background: transparent; }
        QTabBar::tab {
            background: transparent;
            color: %{secondary};
            min-width: 62px;
            height: 32px;
            border-radius: 7px;
            font-size: %{compact}pt;
        }
        QTabBar::tab:selected { background: %{blueSelected}; color: %{primary}; font-weight: 600; }
        QTabBar::tab:hover:!selected { background: %{hover}; color: %{primary}; }
        QTabBar::tab:focus { border: 1px solid %{blue}; }
        QMenu {
            background: %{panel};
            color: %{primary};
            border: 1px solid %{divider};
            border-radius: 8px;
            padding: 4px;
        }
        QMenu::item {
            min-height: 30px;
            padding: 0 24px 0 10px;
            border-radius: 5px;
            font-weight: 400;
        }
        QMenu::item:selected {
            background: %{hover};
            color: %{primary};
        }
        QMenu::item:checked {
            background: %{blueSelected};
            color: %{blueText};
        }
        QMenu::item:checked:selected { background: %{blueSelectedHover}; }
        QMenu::item:disabled { color: %{placeholder}; }
        QMenu::item:disabled:selected { background: transparent; }
        QMenu::separator {
            height: 1px;
            background: %{divider};
            margin: 4px 8px;
        }
        QToolTip { background: %{panel}; color: %{primary}; border: 1px solid %{dividerStrong}; border-radius: 6px; padding: 5px; }
    )QSS");
  // Stringization keeps QSS placeholders named and order-independent.
  // clang-format off
#define CODEXUI_APPLICATION_COLORS(X) \
  X(tealPressed) X(tealText) X(tealHover) X(teal) X(primary) \
  X(greenPressed) X(greenText) X(greenHover) X(green) X(bluePressed) \
  X(blueText) X(secondaryStrong) X(blueHover) X(blue) X(purpleText) \
  X(secondary) X(yellowText) X(redPressed) X(orangeText) X(redText) \
  X(redHover) X(orange) X(placeholder) X(tealBorder) X(neutralBorderPressed) \
  X(blueBorderHover) X(red) X(greenBorder) X(neutralBorderHover) X(blueBorder) \
  X(dividerStrong) X(neutralBorder) X(purpleBorder) X(divider) X(blueSelectedHover) \
  X(yellowBorder) X(neutralSurfaceHover) X(blueSelected) X(orangeBorderStrong) \
  X(tealSurface) X(greenSurface) X(neutralSurface) X(orangeBorder) X(blueSurface) \
  X(hover) X(purpleSurface) X(redBorder) X(appBackground) X(raised) \
  X(yellowSurface) X(orangeSurfaceHover) X(inspector) X(orangeSurface) \
  X(redSurface) X(panel) X(onAccent) X(codeSurface) X(codeText)
  // clang-format on
#define REPLACE_APPLICATION_COLOR(name)                                        \
  style.replace(QLatin1StringView("%{" #name "}"), QLatin1StringView(name));
  CODEXUI_APPLICATION_COLORS(REPLACE_APPLICATION_COLOR)
#undef REPLACE_APPLICATION_COLOR
#undef CODEXUI_APPLICATION_COLORS
  style.replace(QStringLiteral("%{compact}"), compact);
  style.replace(QStringLiteral("%{standard}"), standard);
  style.replace(QStringLiteral("%{section}"), section);
  style.replace(QStringLiteral("%{heading}"), heading);
  style.replace(QStringLiteral("%{panelHeader}"), panelHeader);
  style.replace(QStringLiteral("%{commandOutputVerticalPadding}"),
                QString::number(commandOutputVerticalPadding));
  style.replace(QStringLiteral("%{commandOutputHorizontalPadding}"),
                QString::number(commandOutputHorizontalPadding));
  return style;
}

QString humanizeLabel(QString value) {
  value = value.trimmed();
  if (value.compare(QStringLiteral("xhigh"), Qt::CaseInsensitive) == 0)
    return QStringLiteral("Extra high");
  QString result;
  result.reserve(value.size() + 4);
  bool space = false;
  for (qsizetype index = 0; index < value.size(); ++index) {
    QChar character = value[index];
    if (character.isSpace() || character == QLatin1Char('-') ||
        character == QLatin1Char('_') || character == QLatin1Char('.') ||
        character == QLatin1Char('/')) {
      space = !result.isEmpty();
      continue;
    }
    const QChar previous = index > 0 ? value[index - 1] : QChar{};
    const QChar next = index + 1 < value.size() ? value[index + 1] : QChar{};
    const bool wordBoundary =
        character.isUpper() && (previous.isLower() || previous.isDigit() ||
                                (previous.isUpper() && next.isLower()));
    if ((space || wordBoundary) && !result.endsWith(QLatin1Char(' ')))
      result.append(QLatin1Char(' '));
    if (wordBoundary || (space && character.isUpper() && next.isLower()))
      character = character.toLower();
    result.append(character);
    space = false;
  }
  if (!result.isEmpty())
    result[0] = result[0].toUpper();
  return result;
}

} // namespace codexui::UiStyle
