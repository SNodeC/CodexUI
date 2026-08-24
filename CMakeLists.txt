# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

project(CodexUI LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_AUTOMOC ON)

find_package(AISuite CONFIG REQUIRED)
find_package(
    snodec 2.0 CONFIG REQUIRED
    COMPONENTS net-un-stream-legacy net-in-stream-legacy net-in6-stream-legacy
)
find_package(
    snodec 2.0 CONFIG QUIET
    COMPONENTS net-in-stream-tls net-in6-stream-tls
)
find_package(
    snodec 2.0 CONFIG QUIET
    COMPONENTS net-rc-stream-legacy net-rc-stream-tls
)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)

set(
    CODEXUI_CODEX2_COMMON_SOURCES
    src/codex2/ClientRuntime.cpp
    src/codex2/ClientRuntime.h
    src/codex2/Configuration.cpp
    src/codex2/Configuration.h
    src/codex2/FrontendSession.cpp
    src/codex2/FrontendSession.h
    src/codex2/MainWindow.cpp
    src/codex2/MainWindow.h
    src/codex2/PresentationModel.cpp
    src/codex2/PresentationModel.h
    src/codex2/PresentationProtocol.cpp
    src/codex2/PresentationProtocol.h
    src/codex2/ProtocolNormalizer.cpp
    src/codex2/ProtocolNormalizer.h
    src/codex2/ipc/QtSocketPairEndpoint.cpp
    src/codex2/ipc/QtSocketPairEndpoint.h
    src/codex2/ipc/SNodeSocketPairEndpoint.cpp
    src/codex2/ipc/SNodeSocketPairEndpoint.h
    src/codex2/ipc/SocketPair.cpp
    src/codex2/ipc/SocketPair.h
    src/codex2/main.cpp
    src/ui/ExpandingPromptEditor.cpp
    src/ui/ExpandingPromptEditor.h
    src/ui/UiStyle.cpp
    src/ui/UiStyle.h
)

qt_add_executable(
    codex-ui
    ${CODEXUI_CODEX2_COMMON_SOURCES}
    src/codex2/ShellWidget.cpp
    src/codex2/ShellWidget.h
    src/codex2/PendingRequestDialog.cpp
    src/codex2/PendingRequestDialog.h
    src/codex2/TurnSettingsWidget.cpp
    src/codex2/TurnSettingsWidget.h
)
qt_add_executable(
    codex-ui-harness
    ${CODEXUI_CODEX2_COMMON_SOURCES}
    src/codex2/WorkbenchWidget.cpp
    src/codex2/WorkbenchWidget.h
)

function(configure_codexui_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(
        ${target}
        PRIVATE
            AISuite::OpenAICodex2
            Qt6::Widgets
            Threads::Threads
            snodec::net-un-stream-legacy
            snodec::net-in-stream-legacy
            snodec::net-in6-stream-legacy
    )
endfunction()

configure_codexui_target(codex-ui)
configure_codexui_target(codex-ui-harness)
target_compile_definitions(codex-ui-harness PRIVATE CODEXUI_DEVELOPMENT_HARNESS=1)

if(TARGET snodec::net-in-stream-tls AND TARGET snodec::net-in6-stream-tls)
    foreach(target codex-ui codex-ui-harness)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX2_FRONTEND_TLS=1)
        target_link_libraries(
            ${target} PRIVATE snodec::net-in-stream-tls snodec::net-in6-stream-tls
        )
    endforeach()
endif()

if(TARGET snodec::net-rc-stream-legacy AND TARGET snodec::net-rc-stream-tls)
    foreach(target codex-ui codex-ui-harness)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX2_FRONTEND_RFCOMM=1)
        target_link_libraries(
            ${target} PRIVATE snodec::net-rc-stream-legacy snodec::net-rc-stream-tls
        )
    endforeach()
endif()

if(TARGET snodec::http-client AND TARGET snodec::websocket-client)
    foreach(target codex-ui codex-ui-harness)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX2_FRONTEND_WEBSOCKET=1)
        target_link_libraries(${target} PRIVATE snodec::http-client snodec::websocket-client)
    endforeach()
endif()

install(
    TARGETS codex-ui codex-ui-harness
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
