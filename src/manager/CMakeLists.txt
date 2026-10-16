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
    codexmanager-core STATIC Profile.cpp Profile.h LaunchPlan.cpp LaunchPlan.h
                             ProfileRuntime.cpp ProfileRuntime.h
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

qt_add_executable(codex-manager main.cpp)
set_target_properties(
    codex-manager PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
target_link_libraries(
    codex-manager PRIVATE codexmanager-ui codexui-build-options
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
