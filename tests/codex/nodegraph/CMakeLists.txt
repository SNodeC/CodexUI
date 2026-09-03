# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

add_executable(codexui-nodegraph-test NodeGraphTest.cpp)
target_link_libraries(codexui-nodegraph-test PRIVATE codexui-nodegraph)
add_test(NAME codexui-nodegraph COMMAND codexui-nodegraph-test)
set_tests_properties(codexui-nodegraph PROPERTIES TIMEOUT 10)

add_executable(codexui-protocol-updater-test ProtocolUpdaterTest.cpp)
target_link_libraries(codexui-protocol-updater-test PRIVATE codexui-nodegraph)
add_test(NAME codexui-protocol-updater COMMAND codexui-protocol-updater-test)
set_tests_properties(codexui-protocol-updater PROPERTIES TIMEOUT 10)
