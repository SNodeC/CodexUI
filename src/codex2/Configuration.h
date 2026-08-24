// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX2_CONFIGURATION_H
#define CODEXUI_CODEX2_CONFIGURATION_H

#include <utils/SubCommand.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace CLI {
class Option;
}

namespace codexui::codex2 {

inline constexpr std::size_t DefaultMaximumFrameBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t DefaultMaximumWriteQueueBytes =
    128U * 1024U * 1024U;

class Configuration final : public utils::SubCommand {
public:
  constexpr static std::string_view NAME{"codex-ui"};
  constexpr static std::string_view DESCRIPTION{"Codex bridge Qt client"};

  explicit Configuration(utils::SubCommand *parent);
  ~Configuration() override;

  std::size_t maximumFrameBytes() const;
  std::string webSocketEndpoint() const;

private:
  CLI::Option *maximumFrameBytesOption = nullptr;
  CLI::Option *webSocketEndpointOption = nullptr;
};

} // namespace codexui::codex2

#endif // CODEXUI_CODEX2_CONFIGURATION_H
