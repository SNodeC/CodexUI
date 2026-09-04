# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

add_library(
    codexui-nodegraph
    Value.cpp
    NodeGraph.cpp
    ProtocolCatalog.cpp
    ProtocolUpdater.cpp
    PromptText.cpp
    EventFd.cpp
    ThreadChannels.cpp
    WorkerLogic.cpp
)

target_compile_features(codexui-nodegraph PUBLIC cxx_std_20)
target_include_directories(
    codexui-nodegraph
    PUBLIC ${PROJECT_SOURCE_DIR}/src
)
