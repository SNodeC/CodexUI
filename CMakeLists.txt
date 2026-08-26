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
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
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
    src/codex/GitDiffProvider.cpp
    src/codex/GitDiffProvider.h
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
    CODEXUI_SHELL_SOURCES
    src/codex/ShellWidget.cpp
    src/codex/ShellWidget.h
    src/codex/middle/ComposerPane.cpp
    src/codex/middle/ComposerPane.h
    src/codex/middle/ConversationCards.cpp
    src/codex/middle/ConversationCards.h
    src/codex/middle/ConversationProjection.cpp
    src/codex/middle/ConversationProjection.h
    src/codex/middle/ConversationView.cpp
    src/codex/middle/ConversationView.h
    src/codex/middle/InspectorPane.cpp
    src/codex/middle/InspectorPane.h
    src/codex/middle/MiddleRegionWidget.cpp
    src/codex/middle/MiddleRegionWidget.h
    src/codex/middle/MiddleTypes.cpp
    src/codex/middle/MiddleTypes.h
    src/codex/middle/PromptCoordinator.cpp
    src/codex/middle/PromptCoordinator.h
    src/codex/middle/ThreadPane.cpp
    src/codex/middle/ThreadPane.h
)

qt_add_executable(
    codex-ui
    ${CODEXUI_CODEX_COMMON_SOURCES}
    ${CODEXUI_SHELL_SOURCES}
    src/codex/PendingRequestDialog.cpp
    src/codex/PendingRequestDialog.h
    src/codex/TurnSettingsWidget.cpp
    src/codex/TurnSettingsWidget.h
)
function(configure_codexui_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(
        ${target}
        PRIVATE
            AISuite::OpenAICodex
            PkgConfig::LIBGIT2
            Qt6::Widgets
            Threads::Threads
            snodec::net-un-stream-legacy
            snodec::net-in-stream-legacy
            snodec::net-in6-stream-legacy
    )
endfunction()

configure_codexui_target(codex-ui)

if(TARGET snodec::net-in-stream-tls AND TARGET snodec::net-in6-stream-tls)
    target_compile_definitions(codex-ui PRIVATE CODEXUI_CODEX_FRONTEND_TLS=1)
    target_link_libraries(
        codex-ui PRIVATE snodec::net-in-stream-tls snodec::net-in6-stream-tls
    )
endif()

if(TARGET snodec::net-rc-stream-legacy AND TARGET snodec::net-rc-stream-tls)
    target_compile_definitions(codex-ui PRIVATE CODEXUI_CODEX_FRONTEND_RFCOMM=1)
    target_link_libraries(
        codex-ui PRIVATE snodec::net-rc-stream-legacy snodec::net-rc-stream-tls
    )
endif()

if(TARGET snodec::http-client AND TARGET snodec::websocket-client)
    target_compile_definitions(codex-ui PRIVATE CODEXUI_CODEX_FRONTEND_WEBSOCKET=1)
    target_link_libraries(codex-ui PRIVATE snodec::http-client snodec::websocket-client)
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
        codexui-conversation-projection-test
        tests/codex/ConversationProjectionTest.cpp
        src/codex/middle/ConversationProjection.cpp
        src/codex/middle/ConversationProjection.h
        src/codex/middle/MiddleTypes.cpp
        src/codex/middle/MiddleTypes.h
        src/codex/middle/PromptCoordinator.cpp
        src/codex/middle/PromptCoordinator.h
    )
    target_compile_features(
        codexui-conversation-projection-test PRIVATE cxx_std_20
    )
    target_include_directories(codexui-conversation-projection-test PRIVATE src)
    target_link_libraries(
        codexui-conversation-projection-test PRIVATE Qt6::Widgets
    )
    add_test(
        NAME codexui-conversation-projection
        COMMAND codexui-conversation-projection-test
    )
    set_tests_properties(
        codexui-conversation-projection PROPERTIES TIMEOUT 10
    )

    qt_add_executable(
        codexui-conversation-cards-test
        tests/codex/ConversationCardsTest.cpp
        src/codex/middle/ConversationCards.cpp
        src/codex/middle/ConversationCards.h
        src/codex/middle/ConversationView.cpp
        src/codex/middle/ConversationView.h
        src/codex/middle/MiddleTypes.cpp
        src/codex/middle/MiddleTypes.h
    )
    target_compile_features(
        codexui-conversation-cards-test PRIVATE cxx_std_20
    )
    target_include_directories(codexui-conversation-cards-test PRIVATE src)
    target_link_libraries(
        codexui-conversation-cards-test PRIVATE Qt6::Widgets
    )
    add_test(
        NAME codexui-conversation-cards
        COMMAND codexui-conversation-cards-test
    )
    set_tests_properties(
        codexui-conversation-cards
        PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    qt_add_executable(
        codexui-application-layout-test
        tests/codex/ApplicationLayoutTest.cpp
        src/codex/DiffViewer.cpp
        src/codex/DiffViewer.h
        src/codex/GitDiffProvider.cpp
        src/codex/GitDiffProvider.h
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
        src/codex/middle/ComposerPane.cpp
        src/codex/middle/ComposerPane.h
        src/codex/middle/ConversationCards.cpp
        src/codex/middle/ConversationCards.h
        src/codex/middle/ConversationView.cpp
        src/codex/middle/ConversationView.h
        src/codex/middle/InspectorPane.cpp
        src/codex/middle/InspectorPane.h
        src/codex/middle/MiddleRegionWidget.cpp
        src/codex/middle/MiddleRegionWidget.h
        src/codex/middle/MiddleTypes.cpp
        src/codex/middle/MiddleTypes.h
        src/codex/middle/ThreadPane.cpp
        src/codex/middle/ThreadPane.h
    )
    target_compile_features(
        codexui-application-layout-test PRIVATE cxx_std_20
    )
    target_include_directories(codexui-application-layout-test PRIVATE src)
    target_link_libraries(
        codexui-application-layout-test
        PRIVATE AISuite::OpenAICodex PkgConfig::LIBGIT2 Qt6::Widgets
    )
    add_test(
        NAME codexui-application-layout
        COMMAND codexui-application-layout-test
    )
    set_tests_properties(
        codexui-application-layout
        PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    qt_add_executable(
        codexui-git-changes-live-test
        tests/codex/GitChangesLiveTest.cpp
        src/codex/DiffViewer.cpp
        src/codex/DiffViewer.h
        src/codex/GitDiffProvider.cpp
        src/codex/GitDiffProvider.h
        src/codex/ui/UiStyle.cpp
        src/codex/ui/UiStyle.h
    )
    target_compile_features(codexui-git-changes-live-test PRIVATE cxx_std_20)
    target_include_directories(codexui-git-changes-live-test PRIVATE src)
    target_link_libraries(
        codexui-git-changes-live-test PRIVATE PkgConfig::LIBGIT2 Qt6::Widgets
    )
    add_test(
        NAME codexui-git-changes-live
        COMMAND codexui-git-changes-live-test
    )
    set_tests_properties(
        codexui-git-changes-live
        PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    set(CODEXUI_SHELL_TEST_SOURCES ${CODEXUI_CODEX_COMMON_SOURCES})
    list(
        REMOVE_ITEM CODEXUI_SHELL_TEST_SOURCES
        src/codex/main.cpp
        src/codex/MainWindow.cpp
        src/codex/MainWindow.h
    )
    qt_add_executable(
        codexui-shell-integration-test
        tests/codex/ShellIntegrationTest.cpp
        ${CODEXUI_SHELL_TEST_SOURCES}
        ${CODEXUI_SHELL_SOURCES}
        src/codex/PendingRequestDialog.cpp
        src/codex/PendingRequestDialog.h
        src/codex/TurnSettingsWidget.cpp
        src/codex/TurnSettingsWidget.h
    )
    configure_codexui_target(codexui-shell-integration-test)
    add_test(
        NAME codexui-shell-integration
        COMMAND codexui-shell-integration-test
    )
    set_tests_properties(
        codexui-shell-integration
        PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

endif()

install(
    TARGETS codex-ui
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
