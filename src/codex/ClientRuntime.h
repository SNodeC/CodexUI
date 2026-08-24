// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_CLIENTRUNTIME_H
#define CODEXUI_CODEX_CLIENTRUNTIME_H

namespace codexui::codex {

class Configuration;

int runClientRuntime(int socketPairDescriptor, Configuration &configuration,
                     bool connectBridge);

} // namespace codexui::codex

#endif // CODEXUI_CODEX_CLIENTRUNTIME_H
