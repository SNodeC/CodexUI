# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

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
