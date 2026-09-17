# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

if(NOT DEFINED CODEXUI_SOURCE_DIR)
    message(FATAL_ERROR "CODEXUI_SOURCE_DIR is required")
endif()

file(
    GLOB_RECURSE codexui_ui_sources
    LIST_DIRECTORIES false
    "${CODEXUI_SOURCE_DIR}/src/codex/*.cpp"
    "${CODEXUI_SOURCE_DIR}/src/codex/*.h"
)
list(
    REMOVE_ITEM codexui_ui_sources
    "${CODEXUI_SOURCE_DIR}/src/codex/ui/UiStyle.h"
)

set(raw_color_pattern
    "#[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]"
)
set(raw_color_failures "")
set(named_color_failures "")
set(numeric_color_failures "")
set(policy_failures "")
foreach(source IN LISTS codexui_ui_sources)
    file(READ "${source}" contents)
    string(REGEX MATCHALL "${raw_color_pattern}" raw_colors "${contents}")
    if(raw_colors)
        list(APPEND raw_color_failures "${source}: ${raw_colors}")
    endif()
    string(REGEX MATCH "color[\t ]*:[\t ]*white" named_color "${contents}")
    if(named_color)
        list(APPEND named_color_failures "${source}: ${named_color}")
    endif()
    string(REGEX MATCH "QColor\\([\t ]*[0-9]" numeric_color "${contents}")
    if(NOT numeric_color)
        string(REGEX MATCH "QColor\\{[\t ]*[0-9]" numeric_color "${contents}")
    endif()
    if(numeric_color)
        list(APPEND numeric_color_failures "${source}: ${numeric_color}")
    endif()
endforeach()

if(raw_color_failures)
    list(JOIN raw_color_failures "\n  " failures)
    list(APPEND policy_failures
        "Reusable UI colors must come from UiStyle tokens:\n  ${failures}"
    )
endif()
if(named_color_failures)
    list(JOIN named_color_failures "\n  " failures)
    list(APPEND policy_failures
        "Named reusable UI colors must come from UiStyle tokens:\n  ${failures}"
    )
endif()
if(numeric_color_failures)
    list(JOIN numeric_color_failures "\n  " failures)
    list(APPEND policy_failures
        "Numeric QColor presentation values must come from UiStyle tokens:\n  ${failures}"
    )
endif()

set(forbidden_local_rules
    "QFrame#topBar"
    "QFrame#customStatusBar"
    "QLabel#workspaceBreadcrumb"
    "QToolTip {"
    "QFrame[kind=\"statusDot\"]"
    "QPlainTextEdit{background:transparent"
    "QFrame#conversation"
    "QFrame#attachmentFileBox"
    "QTextBrowser#markdownTextView"
    "QScrollArea#messageImages"
    "QWidget#messageImageStrip"
    "QPlainTextEdit#fileChangesList"
    "QTextEdit#commandOutputView"
    "QTextEdit#commandTextView"
    "QFrame#pendingPromptCard"
    "statusToneColor"
    "dot->setStyleSheet"
    "connectionStatusDot->setStyleSheet"
    "workspaceBreadcrumb->setStyleSheet"
    "background:transparent\;color:"
)
set(local_rule_failures "")
foreach(source IN LISTS codexui_ui_sources)
    if(source STREQUAL "${CODEXUI_SOURCE_DIR}/src/codex/ui/UiStyle.cpp")
        continue()
    endif()
    file(READ "${source}" contents)
    foreach(rule IN LISTS forbidden_local_rules)
        string(FIND "${contents}" "${rule}" position)
        if(NOT position EQUAL -1)
            list(APPEND local_rule_failures "${source}: ${rule}")
        endif()
    endforeach()
endforeach()
if(local_rule_failures)
    list(JOIN local_rule_failures "\n  " failures)
    list(APPEND policy_failures
        "Static component rules must be owned by the application stylesheet:\n  ${failures}"
    )
endif()
if(policy_failures)
    list(JOIN policy_failures "\n" failures)
    message(FATAL_ERROR "${failures}")
endif()
