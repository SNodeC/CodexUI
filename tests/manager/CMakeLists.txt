# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
find_package(Qt6 6.6 REQUIRED COMPONENTS Test)
add_executable(codexmanager-test ManagerTest.cpp)
target_link_libraries(
    codexmanager-test PRIVATE codexmanager-ui codexui-profile-window Qt6::Test
                              codexui-build-options
)
add_test(NAME codexmanager COMMAND codexmanager-test)
find_program(CODEXMANAGER_TEST_BRIDGE codex-bridge REQUIRED)
get_filename_component(
    CODEXMANAGER_TEST_BIN "${CODEXMANAGER_TEST_BRIDGE}" DIRECTORY
)
set_tests_properties(
    codexmanager
    PROPERTIES
        TIMEOUT 90 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;PATH=${CODEXMANAGER_TEST_BIN}:$ENV{PATH}"
)

target_compile_definitions(
    codexmanager-test
    PRIVATE CODEXMANAGER_UI_EXECUTABLE="$<TARGET_FILE:codex-ui>"
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME codexmanager-deletion
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/DeleteProfileDataTest.py
)
set_tests_properties(
    codexmanager-deletion PROPERTIES TIMEOUT 30 ENVIRONMENT
                                     "PYTHONDONTWRITEBYTECODE=1"
)

find_program(CODEXMANAGER_NODE node)
find_program(
    CODEXMANAGER_CHROME
    NAMES google-chrome google-chrome-stable chromium chromium-browser
    PATHS /opt/google/chrome
)
if(CODEXMANAGER_NODE AND CODEXMANAGER_CHROME)
    add_test(
        NAME codexmanager-web
        COMMAND
            ${CODEXMANAGER_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/ManagerWebTest.mjs
            $<TARGET_FILE:codex-manager> $<TARGET_FILE:codexmanager-test>
            ${PROJECT_SOURCE_DIR}/src/manager/web ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(
        codexmanager-web
        PROPERTIES TIMEOUT 120 ENVIRONMENT
                   "QT_QPA_PLATFORM=offscreen;CHROME_BIN=${CODEXMANAGER_CHROME}"
    )
endif()

add_executable(codexmanager-settings-test ManagerSettingsTest.cpp)
add_dependencies(codexmanager-settings-test codex-manager)
target_link_libraries(
    codexmanager-settings-test PRIVATE codexmanager-ui codexmanager-web
                                       Qt6::Test codexui-build-options
)
target_compile_definitions(
    codexmanager-settings-test
    PRIVATE CODEXMANAGER_EXECUTABLE="$<TARGET_FILE:codex-manager>"
            CODEXMANAGER_TEST_ASSETS="${PROJECT_SOURCE_DIR}/src/manager/web"
)
add_test(NAME codexmanager-settings COMMAND codexmanager-settings-test)
set_tests_properties(
    codexmanager-settings PROPERTIES TIMEOUT 60 ENVIRONMENT
                                     "QT_QPA_PLATFORM=offscreen"
)
