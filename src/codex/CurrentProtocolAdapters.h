// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_CURRENTPROTOCOLADAPTERS_H
#define CODEXUI_CODEX_CURRENTPROTOCOLADAPTERS_H

#include <ai/openai/codex/protocol/generated/ProtocolTypes.h>

#include <string_view>

namespace codexui::codex::current_protocol {

// The installed AISuite predates these current app-server alternatives.  The
// bridge only requires an operation's method and an owning Value-compatible
// Params/Response wrapper, so keep the compatibility surface limited to the
// missing operations CodexUI consumes.
using Value = ai::openai::codex::generated::Value;

namespace server_requests {

struct CurrentTimeRead final {
  static constexpr std::string_view method = "currentTime/read";
  using Params = Value;
  using Response = Value;
  static constexpr bool paramsRequired = true;
};

} // namespace server_requests

namespace server_notifications {

struct ModelProviderAuthRecoveryStarted final {
  static constexpr std::string_view method =
      "modelProvider/authRecoveryStarted";
  using Params = Value;
  static constexpr bool paramsRequired = true;
};

struct ModelProviderAuthRecoveryCompleted final {
  static constexpr std::string_view method =
      "modelProvider/authRecoveryCompleted";
  using Params = Value;
  static constexpr bool paramsRequired = true;
};

struct RawResponseItemCompleted final {
  static constexpr std::string_view method = "rawResponseItem/completed";
  using Params = Value;
  static constexpr bool paramsRequired = true;
};

struct RawResponseCompleted final {
  static constexpr std::string_view method = "rawResponse/completed";
  using Params = Value;
  static constexpr bool paramsRequired = true;
};

struct ThreadRealtimeItemStarted final {
  static constexpr std::string_view method = "thread/realtime/item/started";
  using Params = Value;
  static constexpr bool paramsRequired = true;
};

struct ThreadRealtimeItemTranscriptDelta final {
  static constexpr std::string_view method =
      "thread/realtime/item/transcript/delta";
  using Params = Value;
  static constexpr bool paramsRequired = true;
};

struct ThreadRealtimeItemCompleted final {
  static constexpr std::string_view method = "thread/realtime/item/completed";
  using Params = Value;
  static constexpr bool paramsRequired = true;
};

} // namespace server_notifications
} // namespace codexui::codex::current_protocol

#endif // CODEXUI_CODEX_CURRENTPROTOCOLADAPTERS_H
