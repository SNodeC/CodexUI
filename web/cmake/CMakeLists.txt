# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)
project(CodexWebBuild LANGUAGES NONE)
find_package(AISuiteFrontend 1.0 CONFIG REQUIRED)
find_program(CODEXUI_WEB_NPM NAMES npm npm.cmd REQUIRED)
get_filename_component(web_source "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(web_work "${CMAKE_CURRENT_BINARY_DIR}/web")
file(GLOB web_root_inputs "${web_source}/*.*")
file(GLOB_RECURSE web_inputs CONFIGURE_DEPENDS
    "${web_source}/src/*" "${web_source}/tests/*" "${web_source}/tools/*"
    "${web_source}/../tests/fixtures/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${web_root_inputs} ${web_inputs})
file(REMOVE_RECURSE "${web_work}" "${CMAKE_CURRENT_BINARY_DIR}/tests")
file(COPY "${web_source}/" DESTINATION "${web_work}"
    PATTERN node_modules EXCLUDE PATTERN dist EXCLUDE PATTERN app-dist EXCLUDE
    PATTERN cmake EXCLUDE PATTERN package-build EXCLUDE PATTERN "*.tgz" EXCLUDE)
file(COPY "${web_source}/../tests/fixtures" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/tests")
add_custom_target(codexui-web-dependencies
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AISuiteFrontend_PACKAGE}" "${web_work}/aisuite-frontend.tgz"
    COMMAND "${CODEXUI_WEB_NPM}" install --package-lock-only --ignore-scripts --no-audit --no-fund ./aisuite-frontend.tgz
    COMMAND "${CODEXUI_WEB_NPM}" ci --no-audit --no-fund
    WORKING_DIRECTORY "${web_work}" VERBATIM)
add_custom_target(codexui-web-release ALL
    COMMAND "${CODEXUI_WEB_NPM}" run release
    DEPENDS codexui-web-dependencies WORKING_DIRECTORY "${web_work}" VERBATIM)
