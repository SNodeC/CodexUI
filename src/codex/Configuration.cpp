// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/Configuration.h"

namespace codexui::codex {

Configuration::Configuration(utils::SubCommand *parent)
    : utils::SubCommand(parent, this, "Applications") {
  maximumFrameBytesOption =
      setConfigurable(addOption("--bridge-maximum-frame-bytes",
                                "Maximum encoded bridge envelope size", "BYTES",
                                DefaultMaximumFrameBytes, CLI::PositiveNumber),
                      true);
  webSocketEndpointOption = setConfigurable(
      addOption("--bridge-websocket-endpoint",
                "HTTP path used for a Codex bridge WebSocket connection",
                "PATH", std::string{"/codex"}, CLI::Validator{}),
      true);
}

Configuration::~Configuration() = default;

std::size_t Configuration::maximumFrameBytes() const {
  return maximumFrameBytesOption->as<std::size_t>();
}

std::string Configuration::webSocketEndpoint() const {
  return webSocketEndpointOption->as<std::string>();
}

} // namespace codexui::codex
