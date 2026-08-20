# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

project(CodexUI LANGUAGES CXX)

include(CTest)

set(CMAKE_AUTOMOC ON)

find_package(AISuite 0.2.0 CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Network Widgets)

qt_add_executable(
    codex-ui
    src/app/Application.cpp
    src/app/Application.h
    src/app/FrontendSession.cpp
    src/app/FrontendSession.h
    src/main.cpp
    src/ui/ConversationWidget.cpp
    src/ui/ConversationWidget.h
    src/ui/InspectorWidget.cpp
    src/ui/InspectorWidget.h
    src/ui/InteractiveRequestDialog.cpp
    src/ui/InteractiveRequestDialog.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/SidebarWidget.cpp
    src/ui/SidebarWidget.h
    src/ui/ThreadSetupDialog.cpp
    src/ui/ThreadSetupDialog.h
    src/ui/UpcomingTurnDock.cpp
    src/ui/UpcomingTurnDock.h
    src/ui/UiStyle.cpp
    src/ui/UiStyle.h
    src/ui/WorkbenchWidget.cpp
    src/ui/WorkbenchWidget.h
)
target_compile_features(codex-ui PRIVATE cxx_std_20)
target_include_directories(codex-ui PRIVATE src)
target_link_libraries(
    codex-ui
    PRIVATE
        AISuite::OpenAICodexFrontendClient
        Qt6::Network
        Qt6::Widgets
)

if(BUILD_TESTING)
    add_executable(
        CodexUIPromptLimitTest
        tests/PromptLimitTest.cpp
        src/app/FrontendSession.cpp
        src/app/FrontendSession.h
    )
    target_compile_features(CodexUIPromptLimitTest PRIVATE cxx_std_20)
    target_include_directories(CodexUIPromptLimitTest PRIVATE src)
    target_link_libraries(
        CodexUIPromptLimitTest
        PRIVATE
            AISuite::OpenAICodexFrontendClient
            Qt6::Network
    )
    add_test(NAME CodexUIPromptLimitTest COMMAND CodexUIPromptLimitTest)

    add_executable(
        CodexUIFrontendSessionTest
        tests/FrontendSessionTest.cpp
        src/app/FrontendSession.cpp
        src/app/FrontendSession.h
    )
    target_compile_features(CodexUIFrontendSessionTest PRIVATE cxx_std_20)
    target_include_directories(CodexUIFrontendSessionTest PRIVATE src)
    target_link_libraries(
        CodexUIFrontendSessionTest
        PRIVATE
            AISuite::OpenAICodexFrontendClient
            Qt6::Network
    )
    add_test(NAME CodexUIFrontendSessionTest COMMAND CodexUIFrontendSessionTest)

    add_executable(
        CodexUIConversationLayoutTest
        tests/ConversationLayoutTest.cpp
        src/ui/ConversationWidget.cpp
        src/ui/ConversationWidget.h
        src/ui/InspectorWidget.cpp
        src/ui/InspectorWidget.h
        src/ui/UpcomingTurnDock.cpp
        src/ui/UpcomingTurnDock.h
    )
    target_compile_features(CodexUIConversationLayoutTest PRIVATE cxx_std_20)
    target_include_directories(CodexUIConversationLayoutTest PRIVATE src)
    target_link_libraries(
        CodexUIConversationLayoutTest
        PRIVATE
            AISuite::OpenAICodexFrontendClient
            Qt6::Widgets
    )
    add_test(NAME CodexUIConversationLayoutTest COMMAND CodexUIConversationLayoutTest)
    set_tests_properties(CodexUIConversationLayoutTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(
        CodexUIPhase1ThreadTurnUxTest
        tests/Phase1ThreadTurnUxTest.cpp
        src/ui/SidebarWidget.cpp
        src/ui/SidebarWidget.h
        src/ui/ThreadSetupDialog.cpp
        src/ui/ThreadSetupDialog.h
        src/ui/UpcomingTurnDock.cpp
        src/ui/UpcomingTurnDock.h
        src/ui/UiStyle.cpp
        src/ui/UiStyle.h
    )
    target_compile_features(CodexUIPhase1ThreadTurnUxTest PRIVATE cxx_std_20)
    target_include_directories(CodexUIPhase1ThreadTurnUxTest PRIVATE src)
    target_link_libraries(
        CodexUIPhase1ThreadTurnUxTest
        PRIVATE
            AISuite::OpenAICodexFrontendClient
            Qt6::Widgets
    )
    add_test(NAME CodexUIPhase1ThreadTurnUxTest COMMAND CodexUIPhase1ThreadTurnUxTest)
    set_tests_properties(CodexUIPhase1ThreadTurnUxTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(
        CodexUIInteractiveRequestDialogTest
        tests/InteractiveRequestDialogTest.cpp
        src/ui/InteractiveRequestDialog.cpp
        src/ui/InteractiveRequestDialog.h
    )
    target_compile_features(CodexUIInteractiveRequestDialogTest PRIVATE cxx_std_20)
    target_include_directories(CodexUIInteractiveRequestDialogTest PRIVATE src)
    target_link_libraries(
        CodexUIInteractiveRequestDialogTest
        PRIVATE
            AISuite::OpenAICodexFrontendClient
            Qt6::Widgets
    )
    add_test(NAME CodexUIInteractiveRequestDialogTest COMMAND CodexUIInteractiveRequestDialogTest)
    set_tests_properties(CodexUIInteractiveRequestDialogTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
