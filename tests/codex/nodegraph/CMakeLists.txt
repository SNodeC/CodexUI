# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

find_package(nlohmann_json 3.11 REQUIRED)

add_executable(codexui-mcp-progress-test McpProgressTest.cpp)
target_link_libraries(
    codexui-mcp-progress-test PRIVATE codexui-nodegraph
                                      nlohmann_json::nlohmann_json
)
target_compile_definitions(
    codexui-mcp-progress-test
    PRIVATE
        CODEXUI_MCP_PROGRESS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/mcp-progress.json"
)
add_test(NAME codexui-mcp-progress COMMAND codexui-mcp-progress-test)
set_tests_properties(codexui-mcp-progress PROPERTIES TIMEOUT 10)

add_executable(codexui-nodegraph-test NodeGraphTest.cpp)
target_link_libraries(codexui-nodegraph-test PRIVATE codexui-nodegraph)
add_test(NAME codexui-nodegraph COMMAND codexui-nodegraph-test)
set_tests_properties(codexui-nodegraph PROPERTIES TIMEOUT 10)

add_executable(codexui-protocol-updater-test ProtocolUpdaterTest.cpp)
target_link_libraries(codexui-protocol-updater-test PRIVATE codexui-nodegraph)
add_test(NAME codexui-protocol-updater COMMAND codexui-protocol-updater-test)
set_tests_properties(codexui-protocol-updater PROPERTIES TIMEOUT 10)

add_executable(codexui-thread-channels-test ThreadChannelsTest.cpp)
target_link_libraries(codexui-thread-channels-test PRIVATE codexui-nodegraph)
add_test(NAME codexui-thread-channels COMMAND codexui-thread-channels-test)
set_tests_properties(codexui-thread-channels PROPERTIES TIMEOUT 10)

add_executable(codexui-worker-logic-test WorkerLogicTest.cpp)
target_link_libraries(codexui-worker-logic-test PRIVATE codexui-nodegraph)
add_test(NAME codexui-worker-logic COMMAND codexui-worker-logic-test)
set_tests_properties(codexui-worker-logic PROPERTIES TIMEOUT 10)

add_executable(codexui-graph-concurrency-test GraphConcurrencyTest.cpp)
target_link_libraries(
    codexui-graph-concurrency-test PRIVATE codexui-nodegraph Threads::Threads
)
add_test(NAME codexui-graph-concurrency COMMAND codexui-graph-concurrency-test)
set_tests_properties(codexui-graph-concurrency PROPERTIES TIMEOUT 20)
