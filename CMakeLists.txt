# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

project(CodexUI VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)

option(
    CODEXUI_NODEGRAPH_ONLY
    "Configure only the standalone codexui-nodegraph library and tests"
    OFF
)

add_subdirectory(src/codex/nodegraph)
if(BUILD_TESTING)
    add_subdirectory(tests/codex/nodegraph)
endif()

if(CODEXUI_NODEGRAPH_ONLY)
    return()
endif()

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
find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)

function(configure_codexui_transports target)
    if(TARGET snodec::net-in-stream-tls AND TARGET snodec::net-in6-stream-tls)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX_FRONTEND_TLS=1)
        target_link_libraries(
            ${target} PRIVATE snodec::net-in-stream-tls snodec::net-in6-stream-tls
        )
    endif()
    if(TARGET snodec::net-rc-stream-legacy AND TARGET snodec::net-rc-stream-tls)
        target_compile_definitions(${target} PRIVATE CODEXUI_CODEX_FRONTEND_RFCOMM=1)
        target_link_libraries(
            ${target} PRIVATE snodec::net-rc-stream-legacy snodec::net-rc-stream-tls
        )
    endif()
    if(TARGET snodec::http-client AND TARGET snodec::websocket-client)
        target_compile_definitions(
            ${target} PRIVATE CODEXUI_CODEX_FRONTEND_WEBSOCKET=1
        )
        target_link_libraries(
            ${target} PRIVATE snodec::http-client snodec::websocket-client
        )
    endif()
endfunction()

add_library(codexui-build-options INTERFACE)
if(MSVC)
    target_compile_options(codexui-build-options INTERFACE /W4 /WX)
else()
    target_compile_options(codexui-build-options INTERFACE -Wall -Wextra -Werror)
endif()

add_library(
    codexui-client-runtime STATIC
    src/codex/ClientRuntime.cpp
    src/codex/ClientRuntime.h
    src/codex/Configuration.cpp
    src/codex/Configuration.h
    src/codex/InternalProtocolOperations.h
    src/codex/WorkerMailboxReceiver.cpp
    src/codex/WorkerMailboxReceiver.h
)
target_compile_features(codexui-client-runtime PUBLIC cxx_std_20)
target_include_directories(codexui-client-runtime PUBLIC src)
target_link_libraries(
    codexui-client-runtime
    PUBLIC
        AISuite::OpenAICodex
        codexui-nodegraph
        Qt6::Core
        Threads::Threads
        snodec::net-un-stream-legacy
        snodec::net-in-stream-legacy
        snodec::net-in6-stream-legacy
    PRIVATE codexui-build-options
)
configure_codexui_transports(codexui-client-runtime)

add_library(
    codexui-conversation-height-index STATIC
    src/codex/middle/ConversationHeightIndex.cpp
    src/codex/middle/ConversationHeightIndex.h
)
target_compile_features(codexui-conversation-height-index PUBLIC cxx_std_20)
target_include_directories(codexui-conversation-height-index PUBLIC src)
target_link_libraries(codexui-conversation-height-index PUBLIC Qt6::Core)

add_library(
    codexui-nodegraph-json STATIC
    src/codex/NodeGraphJson.cpp
    src/codex/NodeGraphJson.h
)
target_compile_features(codexui-nodegraph-json PUBLIC cxx_std_20)
target_include_directories(codexui-nodegraph-json PUBLIC src)

add_library(
    codexui-pending-request-policy STATIC
    src/codex/PendingRequestPolicy.cpp
    src/codex/PendingRequestPolicy.h
)
target_compile_features(codexui-pending-request-policy PUBLIC cxx_std_20)
target_include_directories(codexui-pending-request-policy PUBLIC src)

add_library(
    codexui-turn-settings-policy STATIC
    src/codex/TurnSettingsPolicy.cpp
    src/codex/TurnSettingsPolicy.h
)
target_compile_features(codexui-turn-settings-policy PUBLIC cxx_std_20)
target_include_directories(codexui-turn-settings-policy PUBLIC src)

add_library(
    codexui-ui-style STATIC
    src/codex/ui/UiStyle.cpp
    src/codex/ui/UiStyle.h
)
target_compile_features(codexui-ui-style PUBLIC cxx_std_20)
target_include_directories(codexui-ui-style PUBLIC src)
target_link_libraries(codexui-ui-style PUBLIC Qt6::Widgets)
target_link_libraries(codexui-ui-style PRIVATE codexui-build-options)

add_library(
    codexui-conversation-ui STATIC
    src/codex/middle/ConversationCards.cpp
    src/codex/middle/ConversationCards.h
    src/codex/middle/ConversationPresentation.cpp
    src/codex/middle/ConversationPresentation.h
    src/codex/middle/ConversationItemModel.cpp
    src/codex/middle/ConversationItemModel.h
    src/codex/middle/ConversationView.cpp
    src/codex/middle/ConversationView.h
    src/codex/middle/MiddleTypes.cpp
    src/codex/middle/MiddleTypes.h
)
target_compile_features(codexui-conversation-ui PUBLIC cxx_std_20)
target_include_directories(codexui-conversation-ui PUBLIC src)
target_link_libraries(
    codexui-conversation-ui
    PUBLIC
        codexui-conversation-height-index
        codexui-nodegraph
        codexui-ui-style
        Qt6::Widgets
    PRIVATE codexui-build-options
)

add_library(
    codexui-ui-adapter STATIC
    src/codex/ui/NodeGraphUiAdapter.cpp
    src/codex/ui/NodeGraphUiAdapter.h
    src/codex/ui/UiViewState.h
)
target_compile_features(codexui-ui-adapter PUBLIC cxx_std_20)
target_include_directories(codexui-ui-adapter PUBLIC src)
target_link_libraries(
    codexui-ui-adapter
    PUBLIC
        codexui-conversation-ui
        codexui-nodegraph-json
        codexui-nodegraph
        codexui-pending-request-policy
        codexui-turn-settings-policy
    PRIVATE codexui-build-options
)

add_library(
    codexui-diff-viewer STATIC
    src/codex/DiffViewer.cpp
    src/codex/DiffViewer.h
    src/codex/GitDiffProvider.cpp
    src/codex/GitDiffProvider.h
)
target_compile_features(codexui-diff-viewer PUBLIC cxx_std_20)
target_include_directories(codexui-diff-viewer PUBLIC src)
target_link_libraries(
    codexui-diff-viewer
    PUBLIC PkgConfig::LIBGIT2 codexui-ui-style Qt6::Widgets
    PRIVATE codexui-build-options
)

add_library(
    codexui-middle-ui STATIC
    src/codex/FileSelectionDialog.cpp
    src/codex/FileSelectionDialog.h
    src/codex/TurnSettingsWidget.cpp
    src/codex/TurnSettingsWidget.h
    src/codex/ui/ExpandingPromptEditor.cpp
    src/codex/ui/ExpandingPromptEditor.h
    src/codex/middle/ComposerPane.cpp
    src/codex/middle/ComposerPane.h
    src/codex/middle/InspectorPane.cpp
    src/codex/middle/InspectorPane.h
    src/codex/middle/MiddleRegionWidget.cpp
    src/codex/middle/MiddleRegionWidget.h
    src/codex/middle/ThreadPane.cpp
    src/codex/middle/ThreadPane.h
)
target_compile_features(codexui-middle-ui PUBLIC cxx_std_20)
target_include_directories(codexui-middle-ui PUBLIC src)
target_link_libraries(
    codexui-middle-ui
    PUBLIC
        AISuite::OpenAICodex
        codexui-conversation-ui
        codexui-diff-viewer
        codexui-pending-request-policy
        codexui-turn-settings-policy
        codexui-ui-adapter
        codexui-ui-style
        Qt6::Widgets
    PRIVATE codexui-build-options
)

add_library(
    codexui-shell-ui STATIC
    src/codex/AttachmentDraft.h
    src/codex/ConnectionDialog.cpp src/codex/ConnectionDialog.h
    src/codex/FrontendSession.cpp
    src/codex/FrontendSession.h
    src/codex/ForkNaming.h
    src/codex/NewThreadDialog.cpp
    src/codex/NewThreadDialog.h
    src/codex/PendingRequestDialog.cpp
    src/codex/PendingRequestDialog.h
    src/codex/ShellWidget.cpp
    src/codex/ShellWidget.h
    src/codex/UiStatus.h
    src/codex/ui/BrandMark.cpp
    src/codex/ui/BrandMark.h
)
target_compile_features(codexui-shell-ui PUBLIC cxx_std_20)
target_include_directories(codexui-shell-ui PUBLIC src)
target_link_libraries(
    codexui-shell-ui
    PUBLIC
        AISuite::OpenAICodex
        codexui-client-runtime
        codexui-middle-ui
        codexui-nodegraph-json
        codexui-nodegraph
        codexui-pending-request-policy
        codexui-turn-settings-policy
        codexui-ui-adapter
        codexui-ui-style
        Qt6::Widgets
        Threads::Threads
    PRIVATE codexui-build-options
)

qt_add_executable(
    codex-ui
    src/codex/MainWindow.cpp
    src/codex/MainWindow.h
    src/codex/main.cpp
)
function(configure_codexui_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(
        ${target}
        PRIVATE
            AISuite::OpenAICodex
            codexui-client-runtime
            codexui-diff-viewer
            codexui-middle-ui
            codexui-conversation-ui
            codexui-conversation-height-index
            codexui-nodegraph-json
            codexui-nodegraph
            codexui-pending-request-policy
            codexui-shell-ui
            codexui-turn-settings-policy
            codexui-ui-adapter
            codexui-ui-style
            PkgConfig::LIBGIT2
            Qt6::Widgets
            Threads::Threads
            snodec::net-un-stream-legacy
            snodec::net-in-stream-legacy
            snodec::net-in6-stream-legacy
    )
endfunction()

configure_codexui_target(codex-ui)
target_link_libraries(codex-ui PRIVATE codexui-build-options)

option(CODEXUI_INSTALL_WEB "Require and install the CodexWebUI production artifact" ON)
if(CODEXUI_INSTALL_WEB)
    add_subdirectory(web)
else()
    message(STATUS "CodexWebUI packaging disabled by CODEXUI_INSTALL_WEB=OFF")
endif()

configure_codexui_transports(codex-ui)

if(BUILD_TESTING)
    add_executable(
        codexui-nodegraph-ui-adapter-test
        tests/codex/NodeGraphUiAdapterTest.cpp
    )
    target_compile_features(
        codexui-nodegraph-ui-adapter-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-nodegraph-ui-adapter-test PRIVATE src
    )
    target_compile_definitions(
        codexui-nodegraph-ui-adapter-test PRIVATE
        CODEXUI_PRESENTATION_CONTRACT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/frontend-presentation.json"
    )
    target_link_libraries(
        codexui-nodegraph-ui-adapter-test
        PRIVATE
            codexui-ui-adapter
    )
    add_test(
        NAME codexui-nodegraph-ui-adapter
        COMMAND codexui-nodegraph-ui-adapter-test
    )
    set_tests_properties(
        codexui-nodegraph-ui-adapter PROPERTIES TIMEOUT 10
    )

    qt_add_executable(
        codexui-nodegraph-conversation-ui-test
        tests/codex/NodeGraphConversationUiTest.cpp
    )
    target_compile_features(
        codexui-nodegraph-conversation-ui-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-nodegraph-conversation-ui-test PRIVATE src
    )
    target_link_libraries(
        codexui-nodegraph-conversation-ui-test
        PRIVATE
            codexui-ui-adapter
            codexui-conversation-height-index
            Qt6::Widgets
    )
    add_test(
        NAME codexui-nodegraph-conversation-ui
        COMMAND codexui-nodegraph-conversation-ui-test
    )
    set_tests_properties(
        codexui-nodegraph-conversation-ui
        PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    qt_add_executable(
        codexui-nodegraph-thread-pane-ui-test
        tests/codex/NodeGraphThreadPaneUiTest.cpp
    )
    target_compile_features(
        codexui-nodegraph-thread-pane-ui-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-nodegraph-thread-pane-ui-test PRIVATE src
    )
    target_link_libraries(
        codexui-nodegraph-thread-pane-ui-test
        PRIVATE
            codexui-middle-ui
            codexui-ui-adapter
            codexui-nodegraph-json
            codexui-nodegraph
            codexui-pending-request-policy
            codexui-conversation-ui
            Qt6::Widgets
    )
    add_test(
        NAME codexui-nodegraph-thread-pane-ui
        COMMAND codexui-nodegraph-thread-pane-ui-test
    )
    set_tests_properties(
        codexui-nodegraph-thread-pane-ui
        PROPERTIES
            TIMEOUT 15
            LABELS "geometry;qt;thread-pane"
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough;CODEXUI_EXPECTED_DPR=1"
    )
    foreach(thread_dpr IN ITEMS 1.25 1.5 2)
        string(REPLACE "." "_" thread_dpr_name "${thread_dpr}")
        set(thread_geometry_name
            "codexui-nodegraph-thread-pane-ui-dpr-${thread_dpr_name}"
        )
        add_test(NAME ${thread_geometry_name} COMMAND codexui-nodegraph-thread-pane-ui-test)
        set_tests_properties(
            ${thread_geometry_name}
            PROPERTIES
                TIMEOUT 15
                LABELS "geometry;qt;thread-pane"
                ENVIRONMENT
                    "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${thread_dpr};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough;CODEXUI_EXPECTED_DPR=${thread_dpr}"
        )
    endforeach()
    foreach(thread_rows IN ITEMS 320 1280 10000)
        foreach(thread_dpr IN ITEMS 1 1.25 1.5 2)
            string(REPLACE "." "_" thread_dpr_name "${thread_dpr}")
            set(thread_benchmark_name
                "codexui-thread-pane-performance-${thread_rows}-dpr-${thread_dpr_name}"
            )
            add_test(
                NAME ${thread_benchmark_name}
                COMMAND codexui-nodegraph-thread-pane-ui-test
                        ${thread_rows} ${thread_dpr}
            )
            set_tests_properties(
                ${thread_benchmark_name}
                PROPERTIES
                    TIMEOUT 45
                    RUN_SERIAL TRUE
                    LABELS "performance;qt;thread-pane"
                    ENVIRONMENT
                        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${thread_dpr};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
            )
        endforeach()
    endforeach()

    add_executable(
        codexui-nodegraph-json-test
        tests/codex/nodegraph/NodeGraphJsonTest.cpp
    )
    target_compile_features(codexui-nodegraph-json-test PRIVATE cxx_std_20)
    target_include_directories(codexui-nodegraph-json-test PRIVATE src)
    target_link_libraries(
        codexui-nodegraph-json-test
        PRIVATE
            AISuite::OpenAICodex
            codexui-nodegraph-json
            codexui-nodegraph
    )
    add_test(NAME codexui-nodegraph-json COMMAND codexui-nodegraph-json-test)
    set_tests_properties(codexui-nodegraph-json PROPERTIES TIMEOUT 10)

    add_executable(
        codexui-protocol-bindings-test
        tests/codex/nodegraph/ProtocolBindingsTest.cpp
        src/codex/InternalProtocolOperations.h
    )
    target_compile_features(
        codexui-protocol-bindings-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-protocol-bindings-test PRIVATE src
    )
    target_link_libraries(
        codexui-protocol-bindings-test
        PRIVATE AISuite::OpenAICodex codexui-nodegraph
    )
    add_test(
        NAME codexui-protocol-bindings
        COMMAND codexui-protocol-bindings-test
    )
    set_tests_properties(
        codexui-protocol-bindings PROPERTIES TIMEOUT 10
    )

    add_executable(
        codexui-client-runtime-dispatch-test
        tests/codex/ClientRuntimeDispatchTest.cpp
    )
    configure_codexui_target(codexui-client-runtime-dispatch-test)
    configure_codexui_transports(codexui-client-runtime-dispatch-test)
    add_test(
        NAME codexui-client-runtime-dispatch
        COMMAND codexui-client-runtime-dispatch-test
    )
    set_tests_properties(
        codexui-client-runtime-dispatch
        PROPERTIES TIMEOUT 20
    )

    add_executable(
        codexui-pending-request-policy-test
        tests/codex/PendingRequestPolicyTest.cpp
    )
    target_compile_features(
        codexui-pending-request-policy-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-pending-request-policy-test PRIVATE src
    )
    target_link_libraries(
        codexui-pending-request-policy-test
        PRIVATE codexui-pending-request-policy
    )
    add_test(
        NAME codexui-pending-request-policy
        COMMAND codexui-pending-request-policy-test
    )
    set_tests_properties(
        codexui-pending-request-policy PROPERTIES TIMEOUT 10
    )

    qt_add_executable(
        codexui-conversation-cards-test
        tests/codex/ConversationCardsTest.cpp
    )
    target_compile_features(
        codexui-conversation-cards-test PRIVATE cxx_std_20
    )
    target_include_directories(codexui-conversation-cards-test PRIVATE src)
    target_compile_definitions(codexui-conversation-cards-test PRIVATE
        CODEXUI_PRESENTATION_CONTRACT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/frontend-presentation.json")
    target_link_libraries(
        codexui-conversation-cards-test
        PRIVATE codexui-conversation-ui codexui-ui-adapter
    )
    add_test(
        NAME codexui-conversation-cards
        COMMAND codexui-conversation-cards-test
    )
    set_tests_properties(
        codexui-conversation-cards
        PROPERTIES TIMEOUT 35 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    foreach(command_output_style IN ITEMS Fusion Breeze)
        string(TOLOWER "${command_output_style}" command_output_style_name)
        foreach(command_output_dpr IN ITEMS 1 1.25 1.5 2)
            string(
                REPLACE "." "_" command_output_dpr_name "${command_output_dpr}"
            )
            set(command_output_test
                "codexui-command-output-geometry-${command_output_style_name}-dpr-${command_output_dpr_name}"
            )
            add_test(
                NAME ${command_output_test}
                COMMAND codexui-conversation-cards-test
            )
            set_tests_properties(
                ${command_output_test}
                PROPERTIES
                    TIMEOUT 15
                    LABELS "conversation;dpr;geometry;qt"
                    ENVIRONMENT
                        "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=${command_output_style};QT_SCALE_FACTOR=${command_output_dpr};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough;CODEXUI_SETTLEMENT_TESTS=1"
            )
        endforeach()
    endforeach()

    qt_add_executable(
        codexui-conversation-item-model-test
        tests/codex/ConversationItemModelTest.cpp
    )
    target_compile_features(
        codexui-conversation-item-model-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-conversation-item-model-test PRIVATE src
    )
    target_link_libraries(
        codexui-conversation-item-model-test
        PRIVATE codexui-conversation-ui
    )
    add_test(
        NAME codexui-conversation-item-model
        COMMAND codexui-conversation-item-model-test
    )
    set_tests_properties(
        codexui-conversation-item-model
        PROPERTIES TIMEOUT 15
    )

    qt_add_executable(
        codexui-conversation-virtualization-test
        tests/codex/ConversationVirtualizationTest.cpp
    )
    target_compile_features(
        codexui-conversation-virtualization-test PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-conversation-virtualization-test PRIVATE src
    )
    target_link_libraries(
        codexui-conversation-virtualization-test
        PRIVATE codexui-conversation-ui
    )
    add_test(
        NAME codexui-conversation-virtualization
        COMMAND codexui-conversation-virtualization-test
    )
    set_tests_properties(
        codexui-conversation-virtualization
        PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    foreach(selection_style IN ITEMS Fusion Breeze)
        string(TOLOWER "${selection_style}" selection_style_name)
        foreach(selection_dpr IN ITEMS 1 1.25 1.5 2)
            string(REPLACE "." "_" selection_dpr_name "${selection_dpr}")
            set(selection_test
                "codexui-conversation-selection-focus-${selection_style_name}-dpr-${selection_dpr_name}"
            )
            add_test(
                NAME ${selection_test}
                COMMAND codexui-conversation-virtualization-test
                        --selection-focus
            )
            set_tests_properties(
                ${selection_test}
                PROPERTIES
                    TIMEOUT 10
                    SKIP_RETURN_CODE 77
                    LABELS "qt;conversation;dpr;accessibility;geometry"
                    ENVIRONMENT
                        "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=${selection_style};QT_SCALE_FACTOR=${selection_dpr};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
            )
        endforeach()
    endforeach()

    qt_add_executable(
        codexui-conversation-view-benchmark
        tests/codex/ConversationViewBenchmark.cpp
    )
    target_compile_features(
        codexui-conversation-view-benchmark PRIVATE cxx_std_20
    )
    target_include_directories(
        codexui-conversation-view-benchmark PRIVATE src
    )
    target_link_libraries(
        codexui-conversation-view-benchmark
        PRIVATE codexui-conversation-ui
    )
    foreach(benchmark_rows IN ITEMS 320 1280 10000)
        foreach(benchmark_dpr IN ITEMS 1 1.25 1.5 2)
            string(REPLACE "." "_" benchmark_dpr_name "${benchmark_dpr}")
            set(benchmark_name
                "codexui-conversation-performance-${benchmark_rows}-dpr-${benchmark_dpr_name}"
            )
            add_test(
                NAME ${benchmark_name}
                COMMAND codexui-conversation-view-benchmark
                        ${benchmark_rows} ${benchmark_dpr}
            )
            set_tests_properties(
                ${benchmark_name}
                PROPERTIES
                    TIMEOUT 45
                    RUN_SERIAL TRUE
                    LABELS "performance;qt;conversation"
                    ENVIRONMENT
                        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${benchmark_dpr};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
            )
        endforeach()
    endforeach()

    qt_add_executable(
        codexui-application-layout-test
        tests/codex/EstablishedUiUxTest.cpp
    )
    target_compile_features(
        codexui-application-layout-test PRIVATE cxx_std_20
    )
    target_include_directories(codexui-application-layout-test PRIVATE src)
    target_link_libraries(
        codexui-application-layout-test
        PRIVATE
            AISuite::OpenAICodex
            codexui-conversation-ui
            codexui-diff-viewer
            codexui-middle-ui
            codexui-conversation-height-index
            codexui-nodegraph
            codexui-pending-request-policy
            codexui-turn-settings-policy
            PkgConfig::LIBGIT2
            Qt6::Widgets
    )
    add_test(
        NAME codexui-application-layout
        COMMAND codexui-application-layout-test
    )
    set_tests_properties(
        codexui-application-layout
        PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_test(
        NAME codexui-ui-style-source-policy
        COMMAND
            ${CMAKE_COMMAND}
            -DCODEXUI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/codex/UiStyleSourcePolicy.cmake
    )
    set_tests_properties(
        codexui-ui-style-source-policy
        PROPERTIES TIMEOUT 10 LABELS "architecture;qt"
    )
    add_test(
        NAME codexui-middle-region-wheel-performance
        COMMAND codexui-application-layout-test --wheel-routing-performance
    )
    set_tests_properties(
        codexui-middle-region-wheel-performance
        PROPERTIES
            TIMEOUT 15
            RUN_SERIAL TRUE
            LABELS "performance;qt;middle-region"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    qt_add_executable(
        codexui-inspector-graph-test
        tests/codex/NodeGraphInspectorUiTest.cpp
    )
    target_compile_features(codexui-inspector-graph-test PRIVATE cxx_std_20)
    target_include_directories(codexui-inspector-graph-test PRIVATE src)
    target_link_libraries(
        codexui-inspector-graph-test
        PRIVATE
            codexui-ui-adapter
            codexui-diff-viewer
            codexui-middle-ui
            codexui-nodegraph-json
            codexui-conversation-ui
            codexui-conversation-height-index
            codexui-nodegraph
            codexui-pending-request-policy
            PkgConfig::LIBGIT2
            Qt6::Widgets
    )
    add_test(
        NAME codexui-inspector-graph
        COMMAND codexui-inspector-graph-test
    )
    set_tests_properties(
        codexui-inspector-graph
        PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    foreach(inspector_dpr IN ITEMS 1 1.25 1.5 2)
        string(REPLACE "." "_" inspector_dpr_name "${inspector_dpr}")
        set(inspector_benchmark_name
            "codexui-inspector-performance-10000-dpr-${inspector_dpr_name}"
        )
        add_test(
            NAME ${inspector_benchmark_name}
            COMMAND codexui-inspector-graph-test --performance 10000
                    ${inspector_dpr}
        )
        set_tests_properties(
            ${inspector_benchmark_name}
            PROPERTIES
                TIMEOUT 60
                RUN_SERIAL TRUE
                LABELS "performance;qt;inspector"
                ENVIRONMENT
                    "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${inspector_dpr};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
        )
        set(inspector_geometry_name
            "codexui-inspector-geometry-dpr-${inspector_dpr_name}"
        )
        add_test(
            NAME ${inspector_geometry_name}
            COMMAND codexui-inspector-graph-test --geometry ${inspector_dpr}
        )
        set_tests_properties(
            ${inspector_geometry_name}
            PROPERTIES
                TIMEOUT 15
                LABELS "geometry;qt;inspector"
                ENVIRONMENT
                    "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${inspector_dpr};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
        )
    endforeach()

    qt_add_executable(
        codexui-git-changes-live-test
        tests/codex/GitChangesLiveTest.cpp
    )
    target_compile_features(codexui-git-changes-live-test PRIVATE cxx_std_20)
    target_include_directories(codexui-git-changes-live-test PRIVATE src)
    target_link_libraries(
        codexui-git-changes-live-test
        PRIVATE codexui-diff-viewer
    )
    add_test(
        NAME codexui-git-changes-live
        COMMAND codexui-git-changes-live-test
    )
    set_tests_properties(
        codexui-git-changes-live
        PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    qt_add_executable(
        codexui-shell-integration-test
        tests/codex/ShellIntegrationTest.cpp
    )
    configure_codexui_target(codexui-shell-integration-test)
    configure_codexui_transports(codexui-shell-integration-test)
    add_test(
        NAME codexui-shell-integration
        COMMAND codexui-shell-integration-test
    )
    set_tests_properties(
        codexui-shell-integration
        PROPERTIES TIMEOUT 45 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
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
