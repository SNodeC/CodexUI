// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX2_CLIENTRUNTIME_H
#define CODEXUI_CODEX2_CLIENTRUNTIME_H

namespace codexui::codex2 {

class Configuration;

int runClientRuntime(int socketPairDescriptor, Configuration &configuration,
                     bool connectBridge);

} // namespace codexui::codex2

#endif // CODEXUI_CODEX2_CLIENTRUNTIME_H
