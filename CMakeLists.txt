# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

project(CodexUI LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

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
    CODEXUI_CODEX_COMMON_SOURCES
    src/codex/ClientRuntime.cpp
    src/codex/ClientRuntime.h
    src/codex/Configuration.cpp
    src/codex/Configuration.h
    src/codex/ConnectionDialog.cpp
    src/codex/ConnectionDialog.h
    src/codex/DiffViewer.cpp
    src/codex/DiffViewer.h
    src/codex/FrontendSession.cpp
    src/codex/FrontendSession.h
    src/codex/FileSelectionDialog.cpp
    src/codex/FileSelectionDialog.h
    src/codex/MainWindow.cpp
    src/codex/MainWindow.h
    src/codex/NewThreadDialog.cpp
    src/codex/NewThreadDialog.h
    src/codex/PresentationModel.cpp
    src/codex/PresentationModel.h
    src/codex/PresentationProtocol.cpp
    src/codex/PresentationProtocol.h
    src/codex/ProtocolNormalizer.cpp
    src/codex/ProtocolNormalizer.h
    src/codex/ipc/QtSocketPairEndpoint.cpp
    src/codex/ipc/QtSocketPairEndpoint.h
    src/codex/ipc/SNodeSocketPairEndpoint.cpp
    src/codex/ipc/SNodeSocketPairEndpoint.h
    src/codex/ipc/SocketPair.cpp
    src/codex/ipc/SocketPair.h
    src/codex/main.cpp
    src/codex/ui/ExpandingPromptEditor.cpp
    src/codex/ui/ExpandingPromptEditor.h
    src/codex/ui/BrandMark.cpp
    src/codex/ui/BrandMark.h
    src/codex/ui/UiStyle.cpp
    src/codex/ui/UiStyle.h
)

set(
    CODEXUI_GREENFIELD_MIDDLE_SOURCES
    src/greenfield/codex/ShellWidget.cpp
    src/greenfield/codex/ShellWidget.h
    src/greenfield/codex/middle/ComposerPane.cpp
    src/greenfield/codex/middle/ComposerPane.h
    src/greenfield/codex/middle/ConversationCards.cpp
    src/greenfield/codex/middle/ConversationCards.h
    src/greenfield/codex/middle/ConversationProjection.cpp
    src/greenfield/codex/middle/ConversationProjection.h
    src/greenfield/codex/middle/ConversationView.cpp
    src/greenfield/codex/middle/ConversationView.h
    src/greenfield/codex/middle/InspectorPane.cpp
    src/greenfield/codex/middle/InspectorPane.h
    src/greenfield/codex/middle/MiddleRegionWidget.cpp
    src/greenfield/codex/middle/MiddleRegionWidget.h
    src/greenfield/codex/middle/MiddleTypes.cpp
    src/greenfield/codex/middle/MiddleTypes.h
    src/greenfield/codex/middle/PromptCoordinator.cpp
    src/greenfield/codex/middle/PromptCoordinator.h
    src/greenfield/codex/middle/ThreadPane.cpp
    src/greenfield/codex/middle/ThreadPane.h
)

qt_add_executable(
    codex-ui
    ${CODEXUI_CODEX_COMMON_SOURCES}
    ${CODEXUI_GREENFIELD_MIDDLE_SOURCES}
    src/codex/PendingRequestDialog.cpp
    src/codex/PendingRequestDialog.h
    src/codex/TurnSettingsWidget.cpp
    src/codex/TurnSettingsWidget.h
)
qt_add_executable(
    codex-ui-harness
    ${CODEXUI_CODEX_COMMON_SOURCES}
    src/codex/WorkbenchWidget.cpp
    src/codex/WorkbenchWidget.h
)

function(configure_codexui_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(
        ${target}
        PRIVATE
            AISuite::OpenAICodex
            Qt6::Widgets
            Threads::Threads
            snodec::net-un-stream-legacy
            snodec::net-in-stream-legacy
            snodec::net-in6-stream-legacy
    )
endfunction()

configure_codexui_target(codex-ui)
target_include_directories(codex-ui BEFORE PRIVATE src/greenfield)
configure_codexui_target(codex-ui-harness)
target_compile_definitions(codex-ui-harness PRIVATE CODEXUI_DEVELOPMENT_HARNESS=1)

if(TARGET snodec::net-in-stream-tls AND TARGET snodec::net-in6-stream-tls)
    foreach(target codex-ui codex-ui-harness)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX_FRONTEND_TLS=1)
        target_link_libraries(
            ${target} PRIVATE snodec::net-in-stream-tls snodec::net-in6-stream-tls
        )
    endforeach()
endif()

if(TARGET snodec::net-rc-stream-legacy AND TARGET snodec::net-rc-stream-tls)
    foreach(target codex-ui codex-ui-harness)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX_FRONTEND_RFCOMM=1)
        target_link_libraries(
            ${target} PRIVATE snodec::net-rc-stream-legacy snodec::net-rc-stream-tls
        )
    endforeach()
endif()

if(TARGET snodec::http-client AND TARGET snodec::websocket-client)
    foreach(target codex-ui codex-ui-harness)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX_FRONTEND_WEBSOCKET=1)
        target_link_libraries(${target} PRIVATE snodec::http-client snodec::websocket-client)
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(
        codexui-socketpair-contract-test
        tests/codex/SocketPairContractTest.cpp
        src/codex/ipc/QtSocketPairEndpoint.cpp
        src/codex/ipc/QtSocketPairEndpoint.h
        src/codex/ipc/SNodeSocketPairEndpoint.cpp
        src/codex/ipc/SNodeSocketPairEndpoint.h
        src/codex/ipc/SocketPair.cpp
        src/codex/ipc/SocketPair.h
    )
    target_compile_features(
        codexui-socketpair-contract-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-socketpair-contract-test PRIVATE src
    )
    target_link_libraries(
        codexui-socketpair-contract-test
        PRIVATE Qt6::Core Threads::Threads snodec::net-un-stream-legacy
    )
    add_test(
        NAME codexui-socketpair-contract
        COMMAND codexui-socketpair-contract-test
    )
    set_tests_properties(
        codexui-socketpair-contract PROPERTIES TIMEOUT 10
    )

    add_executable(
        codexui-presentation-pipeline-test
        tests/codex/PresentationPipelineTest.cpp
        src/codex/PresentationModel.cpp
        src/codex/PresentationModel.h
        src/codex/PresentationProtocol.cpp
        src/codex/PresentationProtocol.h
        src/codex/ProtocolNormalizer.cpp
        src/codex/ProtocolNormalizer.h
    )
    target_compile_features(
        codexui-presentation-pipeline-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-presentation-pipeline-test PRIVATE src
    )
    target_link_libraries(
        codexui-presentation-pipeline-test
        PRIVATE AISuite::OpenAICodex
    )
    add_test(
        NAME codexui-presentation-pipeline
        COMMAND codexui-presentation-pipeline-test
    )
    set_tests_properties(
        codexui-presentation-pipeline PROPERTIES TIMEOUT 10
    )

    add_executable(
        codexui-greenfield-projection-test
        tests/codex/GreenfieldProjectionTest.cpp
        src/greenfield/codex/middle/ConversationProjection.cpp
        src/greenfield/codex/middle/ConversationProjection.h
        src/greenfield/codex/middle/MiddleTypes.cpp
        src/greenfield/codex/middle/MiddleTypes.h
        src/greenfield/codex/middle/PromptCoordinator.cpp
        src/greenfield/codex/middle/PromptCoordinator.h
    )
    target_compile_features(
        codexui-greenfield-projection-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-greenfield-projection-test BEFORE PRIVATE src/greenfield src
    )
    target_link_libraries(
        codexui-greenfield-projection-test PRIVATE Qt6::Widgets
    )
    add_test(
        NAME codexui-greenfield-projection
        COMMAND codexui-greenfield-projection-test
    )
    set_tests_properties(
        codexui-greenfield-projection PROPERTIES TIMEOUT 10
    )

    qt_add_executable(
        codexui-greenfield-middle-test
        tests/codex/GreenfieldMiddleTest.cpp
        src/greenfield/codex/middle/ConversationCards.cpp
        src/greenfield/codex/middle/ConversationCards.h
        src/greenfield/codex/middle/ConversationView.cpp
        src/greenfield/codex/middle/ConversationView.h
        src/greenfield/codex/middle/MiddleTypes.cpp
        src/greenfield/codex/middle/MiddleTypes.h
    )
    target_compile_features(
        codexui-greenfield-middle-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-greenfield-middle-test BEFORE PRIVATE src/greenfield src
    )
    target_link_libraries(
        codexui-greenfield-middle-test PRIVATE Qt6::Widgets
    )
    add_test(
        NAME codexui-greenfield-middle
        COMMAND codexui-greenfield-middle-test
    )
    set_tests_properties(
        codexui-greenfield-middle
        PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    qt_add_executable(
        codexui-greenfield-layout-test
        tests/codex/GreenfieldLayoutTest.cpp
        src/codex/DiffViewer.cpp
        src/codex/DiffViewer.h
        src/codex/FileSelectionDialog.cpp
        src/codex/FileSelectionDialog.h
        src/codex/PresentationModel.cpp
        src/codex/PresentationModel.h
        src/codex/PresentationProtocol.cpp
        src/codex/PresentationProtocol.h
        src/codex/TurnSettingsWidget.cpp
        src/codex/TurnSettingsWidget.h
        src/codex/ui/ExpandingPromptEditor.cpp
        src/codex/ui/ExpandingPromptEditor.h
        src/codex/ui/UiStyle.cpp
        src/codex/ui/UiStyle.h
        src/greenfield/codex/middle/ComposerPane.cpp
        src/greenfield/codex/middle/ComposerPane.h
        src/greenfield/codex/middle/ConversationCards.cpp
        src/greenfield/codex/middle/ConversationCards.h
        src/greenfield/codex/middle/ConversationView.cpp
        src/greenfield/codex/middle/ConversationView.h
        src/greenfield/codex/middle/InspectorPane.cpp
        src/greenfield/codex/middle/InspectorPane.h
        src/greenfield/codex/middle/MiddleRegionWidget.cpp
        src/greenfield/codex/middle/MiddleRegionWidget.h
        src/greenfield/codex/middle/MiddleTypes.cpp
        src/greenfield/codex/middle/MiddleTypes.h
        src/greenfield/codex/middle/ThreadPane.cpp
        src/greenfield/codex/middle/ThreadPane.h
    )
    target_compile_features(
        codexui-greenfield-layout-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-greenfield-layout-test BEFORE PRIVATE src/greenfield src
    )
    target_link_libraries(
        codexui-greenfield-layout-test PRIVATE AISuite::OpenAICodex Qt6::Widgets
    )
    add_test(
        NAME codexui-greenfield-layout
        COMMAND codexui-greenfield-layout-test
    )
    set_tests_properties(
        codexui-greenfield-layout
        PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    set(CODEXUI_GREENFIELD_SHELL_TEST_SOURCES ${CODEXUI_CODEX_COMMON_SOURCES})
    list(
        REMOVE_ITEM CODEXUI_GREENFIELD_SHELL_TEST_SOURCES
        src/codex/main.cpp
        src/codex/MainWindow.cpp
        src/codex/MainWindow.h
    )
    qt_add_executable(
        codexui-greenfield-shell-test
        tests/codex/GreenfieldShellIntegrationTest.cpp
        ${CODEXUI_GREENFIELD_SHELL_TEST_SOURCES}
        ${CODEXUI_GREENFIELD_MIDDLE_SOURCES}
        src/codex/PendingRequestDialog.cpp
        src/codex/PendingRequestDialog.h
        src/codex/TurnSettingsWidget.cpp
        src/codex/TurnSettingsWidget.h
    )
    configure_codexui_target(codexui-greenfield-shell-test)
    target_include_directories(
        codexui-greenfield-shell-test BEFORE PRIVATE src/greenfield
    )
    add_test(
        NAME codexui-greenfield-shell
        COMMAND codexui-greenfield-shell-test
    )
    set_tests_properties(
        codexui-greenfield-shell
        PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

endif()

install(
    TARGETS codex-ui codex-ui-harness
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
install(
    FILES resources/icons/codex-ui.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
)
install(
    FILES resources/applications/codex-ui.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)
