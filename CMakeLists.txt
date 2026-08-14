# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

project(CodexUI LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

find_package(AISuite CONFIG REQUIRED)
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
