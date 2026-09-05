// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_CLIENTRUNTIME_H
#define CODEXUI_CODEX_CLIENTRUNTIME_H

namespace codexui::codex {

class Configuration;

} // namespace codexui::codex

namespace codexui::nodegraph {
class NodeGraph;
class ThreadChannels;
} // namespace codexui::nodegraph

namespace codexui::codex {

int runClientRuntime(Configuration &configuration, nodegraph::NodeGraph &graph,
                     nodegraph::ThreadChannels &channels, bool connectBridge);

} // namespace codexui::codex

#endif // CODEXUI_CODEX_CLIENTRUNTIME_H
