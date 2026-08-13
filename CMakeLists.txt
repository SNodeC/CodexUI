# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

project(CodexUI LANGUAGES CXX)

find_package(AISuite CONFIG REQUIRED)

add_executable(codex-ui src/main.cpp)
target_compile_features(codex-ui PRIVATE cxx_std_20)
target_link_libraries(codex-ui PRIVATE AISuite::OpenAICodexFrontendClient)
