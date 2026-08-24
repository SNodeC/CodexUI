// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PROTOCOLNORMALIZER_H
#define CODEXUI_CODEX_PROTOCOLNORMALIZER_H

#include <nlohmann/json.hpp>

#include "codex/PresentationProtocol.h"

#include <functional>
#include <string>
#include <string_view>

namespace codexui::codex {

class ProtocolNormalizer final {
public:
  using Sink = std::function<bool(const nlohmann::json &)>;

  explicit ProtocolNormalizer(Sink sink);

  void transportEvent(std::string_view event, std::string detail = {});
  void bridgeEvent(const nlohmann::json &event);
  void serverNotification(std::string_view method,
                          const nlohmann::json &params);
  void serverRequest(std::string_view method, const nlohmann::json &requestId,
                     const nlohmann::json &params);
  void observeRawInbound(const nlohmann::json &message);

  void operationResult(std::string action, std::string correlationId,
                       nlohmann::json context, const nlohmann::json &response);
  void operationRejected(std::string action, std::string correlationId,
                         int code, std::string message);

private:
  bool emit(nlohmann::json frame) const;
  bool
  emitEvent(std::string type, nlohmann::json data = nlohmann::json::object(),
            presentation::Authority authority = presentation::Authority::None,
            nlohmann::json scope = nlohmann::json::object());
  void diagnostic(std::string source, std::string code, std::string message,
                  nlohmann::json details = nlohmann::json::object());
  bool knownServerMethod(std::string_view method) const;

  Sink sink;
  std::uint64_t connectionGeneration = 0;
  std::uint64_t nextSequence = 1;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PROTOCOLNORMALIZER_H
