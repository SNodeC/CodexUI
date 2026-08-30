# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(CodexWebUI VERSION 1.0.0 LANGUAGES NONE)
    set(CMAKE_INSTALL_DATADIR "share" CACHE PATH "Read-only architecture-independent data directory")
endif()

set(CODEXUI_WEB_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/app-dist" CACHE PATH
    "Directory containing the built CodexWebUI static artifact")

if(NOT EXISTS "${CODEXUI_WEB_DIST_DIR}/index.html")
    message(FATAL_ERROR
        "CodexWebUI artifact is missing at ${CODEXUI_WEB_DIST_DIR}. "
        "Run 'npm ci' and 'npm run release' in the CodexWebUI source directory, "
        "or configure a native-only build with -DCODEXUI_INSTALL_WEB=OFF.")
endif()

add_custom_target(codexui-web-artifact ALL DEPENDS "${CODEXUI_WEB_DIST_DIR}/index.html")
install(DIRECTORY "${CODEXUI_WEB_DIST_DIR}/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/codexui/web"
    COMPONENT CodexWebUI)
