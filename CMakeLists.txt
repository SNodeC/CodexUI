# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

project(CodexUI LANGUAGES CXX)

find_package(AISuite CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(
    codex-ui
    src/app/Application.cpp
    src/app/Application.h
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
)
target_compile_features(codex-ui PRIVATE cxx_std_20)
target_include_directories(codex-ui PRIVATE src)
target_link_libraries(
    codex-ui
    PRIVATE
        AISuite::OpenAICodexFrontendClient
        Qt6::Widgets
)
