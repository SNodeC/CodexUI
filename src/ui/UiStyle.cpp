// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/UiStyle.h"

namespace codexui::UiStyle {

QString applicationStyleSheet()
{
    return QStringLiteral(R"QSS(
        * {
            color: #e8edf2;
            font-family: "Inter", "Noto Sans", "DejaVu Sans", sans-serif;
            font-size: 12px;
            outline: none;
        }
        QMainWindow, QWidget#workbench { background: #0e1013; }
        QLabel { background: transparent; font-weight: 400; }
        QLabel[kind="muted"] { color: #949ead; }
        QLabel[kind="section"] {
            color: #949ead;
            font-size: 10px;
            font-weight: 600;
        }
        QLabel[kind="attentionSection"] {
            color: #f5a83b;
            font-size: 9px;
            font-weight: 600;
        }
        QLabel[kind="heading"] { font-size: 18px; font-weight: 600; }
        QLabel[kind="title"] { font-size: 13px; font-weight: 600; }
        QLabel[kind="body"] { font-size: 13px; }
        QLabel[kind="meta"] { color: #949ead; font-size: 10px; }
        QLabel[kind="small"] { color: #949ead; font-size: 9px; }
        QPushButton, QToolButton {
            background: #181c21;
            border: 0;
            border-radius: 7px;
            padding: 0 12px;
            font-size: 11px;
            font-weight: 600;
        }
        QPushButton:hover, QToolButton:hover { background: #20252c; }
        QPushButton:pressed, QToolButton:pressed { background: #252b33; }
        QPushButton[kind="primary"] { background: #4f94f5; color: white; }
        QPushButton[kind="primary"]:hover { background: #65a2f6; }
        QPushButton[kind="subtle"] { color: #949ead; }
        QPushButton[kind="agentLink"] { background: #1a2940; color: #4f94f5; text-align: left; }
        QPushButton[kind="stop"] { background: #521a1a; color: #ffd1d1; }
        QFrame[kind="panel"] { background: #13161a; }
        QFrame[kind="raised"] { background: #181c21; border-radius: 10px; }
        QFrame[kind="summary"] { background: #13161a; border-radius: 7px; }
        QFrame[kind="greenBadge"] { background: #143321; border-radius: 6px; }
        QFrame[kind="blueBadge"] { background: #14263d; border-radius: 5px; }
        QFrame[kind="amberBadge"] { background: #3d2912; border-radius: 7px; }
        QFrame[kind="composer"] { background: #13161a; border: 2px solid transparent; border-radius: 10px; }
        QFrame[kind="composer"][focused="true"] { border-color: #4f94f5; }
        QPlainTextEdit {
            background: transparent;
            border: 0;
            color: #e8edf2;
            font-size: 13px;
            padding: 0;
            selection-background-color: #1a2940;
        }
        QPlainTextEdit[empty="true"] { color: #949ead; }
        QScrollArea { background: transparent; border: 0; }
        QScrollArea > QWidget > QWidget { background: transparent; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
        QScrollBar::handle:vertical { background: #343b45; min-height: 28px; border-radius: 3px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QSplitter::handle { background: #181c21; }
        QSplitter::handle:horizontal { width: 8px; }
        QTabBar { background: transparent; }
        QTabBar::tab {
            background: transparent;
            color: #949ead;
            min-width: 62px;
            height: 30px;
            border-radius: 7px;
            font-size: 11px;
        }
        QTabBar::tab:selected { background: #181c21; color: #e8edf2; font-weight: 600; }
        QTabBar::tab:hover:!selected { color: #e8edf2; }
        QMenu { background: #181c21; border: 1px solid #2b3038; padding: 5px; }
        QMenu::item { padding: 7px 28px 7px 10px; border-radius: 4px; }
        QMenu::item:selected { background: #1a2940; }
    )QSS");
}

} // namespace codexui::UiStyle
