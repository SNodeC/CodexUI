// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PRESENTATIONCLIENT_H
#define CODEXUI_CODEX_PRESENTATIONCLIENT_H

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <utility>

namespace codexui::codex {

// Toolkit-neutral, protocol-complete command side of the presentation
// boundary.  It deliberately contains no transport or lifecycle ownership:
// FrontendSession remains the Qt/socketpair adapter and supplies these calls.
// A different renderer can supply the same three functions without inheriting
// from a Qt type or mirroring FrontendSession's convenience methods.
class PresentationClient final {
public:
  using Completion = std::function<void(const nlohmann::json &)>;
  using Request = std::function<std::string(
      std::string, nlohmann::json, Completion)>;
  using Command =
      std::function<bool(std::string, nlohmann::json)>;
  using ServerResponse = std::function<bool(
      nlohmann::json, nlohmann::json, nlohmann::json)>;

  PresentationClient() = default;
  PresentationClient(Request request, Command command,
                     ServerResponse serverResponse)
      : request_(std::move(request)), command_(std::move(command)),
        serverResponse_(std::move(serverResponse)) {}

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(request_) && static_cast<bool>(command_) &&
           static_cast<bool>(serverResponse_);
  }

  std::string execute(std::string action, nlohmann::json data,
                      Completion completion = {}) const {
    return request_ ? request_(std::move(action), std::move(data),
                               std::move(completion))
                    : std::string{};
  }

  bool send(std::string action,
            nlohmann::json data = nlohmann::json::object()) const {
    return command_ && command_(std::move(action), std::move(data));
  }

  bool respond(nlohmann::json requestId, nlohmann::json result,
               nlohmann::json error = nullptr) const {
    return serverResponse_ && serverResponse_(
                                  std::move(requestId), std::move(result),
                                  std::move(error));
  }

private:
  Request request_;
  Command command_;
  ServerResponse serverResponse_;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PRESENTATIONCLIENT_H
