# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

add_library(
    codexui-profile-window STATIC ../codex/ui/ProfileWindow.cpp
                                  ../codex/ui/ProfileWindow.h
)
target_compile_features(codexui-profile-window PUBLIC cxx_std_20)
target_include_directories(
    codexui-profile-window PUBLIC ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    codexui-profile-window
    PUBLIC Qt6::Network Qt6::Widgets
    PRIVATE codexui-build-options
)

add_library(
    codexmanager-core STATIC
    ManagerController.cpp
    ManagerController.h
    Profile.cpp
    Profile.h
    LaunchPlan.cpp
    LaunchPlan.h
    ProfileRuntime.cpp
    ProfileRuntime.h
    DeletionPlan.cpp
    DeletionPlan.h
)
qt_add_resources(
    codexmanager-core deletion PREFIX "/manager" FILES DeleteProfileData.py
)

target_compile_features(codexmanager-core PUBLIC cxx_std_20)
target_include_directories(codexmanager-core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(
    codexmanager-core
    PUBLIC Qt6::Network codexui-ui-style
    PRIVATE codexui-build-options
)

add_library(
    codexmanager-ui STATIC ManagerWindow.cpp ManagerWindow.h ProfileEditor.cpp
                           ProfileEditor.h
)
target_compile_features(codexmanager-ui PUBLIC cxx_std_20)
target_link_libraries(
    codexmanager-ui
    PUBLIC codexmanager-core Qt6::Widgets
    PRIVATE codexui-build-options
)

find_package(
    snodec 2.0 CONFIG REQUIRED COMPONENTS http-server-express-legacy-in
                                          http-server-express-tls-in
)
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/web/src/colors.css"
)
file(READ "${PROJECT_SOURCE_DIR}/web/src/colors.css" manager_colors)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    WebColors.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/WebColors.h" @ONLY
)
add_library(codexmanager-web STATIC ManagerWebServer.cpp ManagerWebServer.h)
target_compile_features(codexmanager-web PUBLIC cxx_std_20)
target_include_directories(
    codexmanager-web
    PUBLIC ${PROJECT_SOURCE_DIR}/src
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(
    codexmanager-web
    PUBLIC codexui-nodegraph Threads::Threads
    PRIVATE snodec::http-server-express-legacy-in
            snodec::http-server-express-tls-in codexui-build-options
)
qt_add_executable(codex-manager main.cpp)
target_compile_definitions(
    codex-manager
    PRIVATE
        CODEXMANAGER_WEB_ROOT="${CMAKE_INSTALL_FULL_DATADIR}/codexui/manager-web"
)
install(DIRECTORY web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/codexui/manager-web)
set_target_properties(
    codex-manager PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
target_link_libraries(
    codex-manager PRIVATE codexmanager-ui codexmanager-web
                          codexui-build-options
)
install(TARGETS codex-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    add_subdirectory(
        ${PROJECT_SOURCE_DIR}/tests/manager ${CMAKE_CURRENT_BINARY_DIR}/tests
    )
endif()

install(FILES ${PROJECT_SOURCE_DIR}/resources/icons/codex-manager.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
)
install(FILES ${PROJECT_SOURCE_DIR}/resources/applications/codex-manager.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)
