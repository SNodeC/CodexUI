// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_PROTOCOLCATALOG_H
#define CODEXUI_CODEX_NODEGRAPH_PROTOCOLCATALOG_H

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace codexui::nodegraph {

enum class ProtocolDirection {
  ClientRequest,
  ServerRequest,
  ServerNotification,
  ClientNotification,
};

enum class MessageDisposition {
  GraphUpdate,
  NoticeGraphUpdate,
  WorkerOperationResult,
  ReverseInteraction,
  IntentionallyStateNeutral,
};

struct MethodDescriptor {
  std::string_view method;
  ProtocolDirection direction;
  MessageDisposition disposition;
};

[[nodiscard]] std::span<const MethodDescriptor> protocolMethods() noexcept;

[[nodiscard]] std::optional<std::reference_wrapper<const MethodDescriptor>>
findProtocolMethod(ProtocolDirection direction,
                   std::string_view method) noexcept;

[[nodiscard]] std::size_t
protocolMethodCount(ProtocolDirection direction) noexcept;

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_PROTOCOLCATALOG_H
