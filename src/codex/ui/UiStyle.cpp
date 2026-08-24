// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/UiStyle.h"

namespace codexui::UiStyle {

QString applicationStyleSheet() {
  return QStringLiteral(R"QSS(
        * {
            color: #1d2633;
            font-family: "Inter", "Noto Sans", "DejaVu Sans", sans-serif;
            font-size: 12px;
        }
        QMainWindow, QWidget#workbench { background: #f6f8fb; }
        QLabel { background: transparent; font-weight: 400; }
        QLabel[kind="muted"] { color: #667085; }
        QLabel[kind="section"] {
            color: #667085;
            font-size: 10px;
            font-weight: 600;
        }
        QLabel[kind="attentionSection"] {
            color: #a76812;
            font-size: 9px;
            font-weight: 600;
        }
        QLabel[kind="heading"] { font-size: 18px; font-weight: 600; }
        QLabel[kind="title"] { font-size: 13px; font-weight: 600; }
        QLabel[kind="body"] { font-size: 13px; }
        QLabel[kind="meta"] { color: #667085; font-size: 10px; }
        QLabel[kind="small"] { color: #667085; font-size: 9px; }
        QPushButton, QToolButton {
            background: #ffffff;
            border: 1px solid #d7dee8;
            border-radius: 7px;
            padding: 0 12px;
            font-size: 11px;
            font-weight: 600;
        }
        QPushButton:hover, QToolButton:hover { background: #f1f5fb; border-color: #b9c4d2; }
        QPushButton:pressed, QToolButton:pressed { background: #e5eeff; border-color: #bfd3f9; }
        QPushButton:focus, QToolButton:focus { border: 2px solid #2f6feb; }
        QPushButton:disabled, QToolButton:disabled { color: #98a2b3; background: #f6f8fb; border-color: #d7dee8; }
        QPushButton[kind="primary"] { background: #2f6feb; border-color: #2f6feb; color: white; }
        QPushButton[kind="primary"]:hover { background: #285fca; border-color: #285fca; }
        QPushButton[kind="steer"] { background: #ffffff; border-color: #2f6feb; color: #2f6feb; }
        QPushButton[kind="steer"]:hover { background: #e5eeff; border-color: #285fca; color: #285fca; }
        QPushButton[kind="cancel"] { background: #eef1f5; border-color: #c8d0dc; color: #475467; }
        QPushButton[kind="cancel"]:hover { background: #e3e8ef; border-color: #aeb8c6; }
        QPushButton[kind="subtle"], QToolButton[kind="subtle"] {
            color: #667085;
            background: transparent;
            border-color: transparent;
        }
        QToolButton[kind="composerAction"] {
            background: #ffffff;
            border: 1px solid #d7dee8;
            color: #667085;
            padding: 0;
        }
        QToolButton[kind="composerAction"]:hover { background: #f1f5fb; border-color: #b9c4d2; }
        QPushButton[kind="agentLink"] { background: #e5eeff; border-color: #bfd3f9; color: #2f6feb; text-align: left; }
        QPushButton[kind="stop"] { background: #ffffff; border-color: #b83a3a; color: #b83a3a; }
        QPushButton[kind="stop"]:hover { background: #fff1f1; }
        QPushButton[codexChevron="true"] { padding-right: 26px; }
        QPushButton[codexChevron="true"]::menu-indicator { image: none; width: 0; }
        QPushButton[changed="true"] {
            background: #e5eeff;
            color: #2f6feb;
            border-color: #2f6feb;
        }
        QFrame[kind="panel"] { background: #ffffff; }
        QFrame[kind="raised"] { background: #ffffff; border: 1px solid #d7dee8; border-radius: 10px; }
        QFrame[messageRole="user"] { background: #eaf2ff; border: 1px solid #bfd3f9; border-radius: 8px; }
        QFrame[messageRole="agent"] { background: #ffffff; border: 0; border-radius: 8px; }
        QFrame[kind="summary"] { background: #f8fafc; border: 1px solid #d7dee8; border-radius: 7px; }
        QFrame[kind="greenBadge"] { background: #e9f7f0; border-radius: 6px; }
        QFrame[kind="blueBadge"] { background: #e5eeff; border-radius: 5px; }
        QFrame[kind="amberBadge"] { background: #fff6df; border: 1px solid #e5c77d; border-radius: 7px; }
        QFrame[kind="composer"] { background: #ffffff; border: 1px solid #d7dee8; border-radius: 10px; }
        QFrame[kind="composer"][focused="true"] { border: 2px solid #2f6feb; }
        QPlainTextEdit {
            background: transparent;
            border: 0;
            color: #1d2633;
            font-size: 13px;
            padding: 0;
            selection-background-color: #e5eeff;
            selection-color: #1d2633;
        }
        QPlainTextEdit[empty="true"] { color: #98a2b3; }
        QPlainTextEdit[kind="dialogEditor"] {
            background: #ffffff;
            border: 1px solid #d7dee8;
            border-radius: 7px;
            padding: 8px;
        }
        QPlainTextEdit[kind="dialogEditor"]:focus { border-color: #2f6feb; }
        QLineEdit {
            background: #ffffff;
            border: 1px solid #d7dee8;
            border-radius: 7px;
            min-height: 32px;
            padding: 0 9px;
            selection-background-color: #e5eeff;
            selection-color: #1d2633;
        }
        QLineEdit:focus { border-color: #2f6feb; }
        QLineEdit:disabled { color: #98a2b3; background: #f6f8fb; }
        QComboBox {
            background: #ffffff;
            border: 1px solid #d7dee8;
            border-radius: 7px;
            min-height: 30px;
            padding: 0 24px 0 9px;
        }
        QComboBox:hover { border-color: #b9c4d2; }
        QComboBox:focus { border-color: #2f6feb; }
        QComboBox:disabled { color: #98a2b3; background: #f6f8fb; }
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
            background: #ffffff;
            color: #1d2633;
            border: 1px solid #d7dee8;
            selection-background-color: #e5eeff;
            selection-color: #1d2633;
        }
        QTreeView#codexFileBrowser, QListWidget#codexAttachmentList {
            background: #ffffff;
            alternate-background-color: #f8fafc;
            border: 1px solid #d7dee8;
            border-radius: 7px;
            outline: 0;
        }
        QListWidget#codexDiffFiles {
            background: #ffffff;
            border: 1px solid #d7dee8;
            border-radius: 7px;
            outline: 0;
        }
        QListWidget#codexDiffFiles::item {
            min-height: 27px;
            padding: 3px 7px;
        }
        QListWidget#codexDiffFiles::item:hover { background: #f1f5fb; }
        QListWidget#codexDiffFiles::item:selected {
            background: #e5eeff;
            color: #1d2633;
        }
        QPlainTextEdit#codexDiffText {
            background: #ffffff;
            border: 1px solid #d7dee8;
            border-radius: 7px;
            padding: 7px;
            font-size: 11px;
        }
        QTreeView#codexFileBrowser::item, QListWidget#codexAttachmentList::item {
            min-height: 28px;
            padding: 3px 7px;
        }
        QTreeView#codexFileBrowser::item:hover,
        QListWidget#codexAttachmentList::item:hover { background: #f1f5fb; }
        QTreeView#codexFileBrowser::item:selected,
        QListWidget#codexAttachmentList::item:selected {
            background: #e5eeff;
            color: #1d2633;
        }
        QHeaderView::section {
            background: #f8fafc;
            color: #667085;
            border: 0;
            border-bottom: 1px solid #d7dee8;
            padding: 7px;
            font-size: 10px;
            font-weight: 600;
        }
        QCheckBox, QRadioButton { spacing: 8px; }
        QDialog { background: #ffffff; }
        QScrollArea { background: #f6f8fb; border: 0; }
        QTabWidget QScrollArea { background: #fbfcfe; }
        QDialog QScrollArea { background: #ffffff; }
        QScrollArea > QWidget > QWidget { background: transparent; }
        QAbstractScrollArea::corner { background: transparent; border: 0; }
        QScrollBar:vertical {
            background: transparent;
            border: 0;
            width: 8px;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: #b9c4d2;
            min-height: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover { background: #98a2b3; }
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
            background: #b9c4d2;
            min-width: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:horizontal:hover { background: #98a2b3; }
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
        QSplitter::handle { background: #d7dee8; }
        QSplitter::handle:horizontal { width: 8px; }
        QTabBar { background: transparent; }
        QTabBar::tab {
            background: transparent;
            color: #667085;
            min-width: 62px;
            height: 30px;
            border-radius: 7px;
            font-size: 11px;
        }
        QTabBar::tab:selected { background: #e5eeff; color: #1d2633; font-weight: 600; }
        QTabBar::tab:hover:!selected { background: #f1f5fb; color: #1d2633; }
        QTabBar::tab:focus { border: 1px solid #2f6feb; }
        QMenu { background: #ffffff; color: #1d2633; border: 1px solid #d7dee8; padding: 5px; }
        QMenu::item { padding: 7px 28px 7px 10px; border-radius: 4px; }
        QMenu::item:selected { background: #e5eeff; color: #1d2633; }
        QMenu::item:disabled { color: #98a2b3; }
        QMenu::separator { height: 1px; background: #d7dee8; margin: 5px 8px; }
        QToolTip { background: #ffffff; color: #1d2633; border: 1px solid #b9c4d2; padding: 5px; }
    )QSS");
}

} // namespace codexui::UiStyle
