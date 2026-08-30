// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONPROJECTION_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONPROJECTION_H

#include "codex/PresentationModel.h"
#include "codex/middle/MiddleTypes.h"
#include "codex/middle/PromptCoordinator.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace codexui::codex::middle {

// Pure canonical projection. Initial rendering is simply reconciliation from
// an empty snapshot; no separate full-rebuild ordering exists.
class ConversationProjection final {
public:
  static constexpr std::size_t DefaultAuthoritativeItemLimit =
      AuthoritativeHistoryPageSize;

  [[nodiscard]] static ConversationSnapshot
  project(const AuthoritativeItemIndex &authoritativeItems,
          const ThreadPresentation *authoritativeThread,
          std::span<const PromptSubmission> localSubmissions,
          std::size_t authoritativeItemLimit, std::int64_t nowMilliseconds);

  [[nodiscard]] static ConversationSnapshot
  project(const ThreadPresentation &authoritativeThread,
          std::span<const PromptSubmission> localSubmissions,
          std::size_t authoritativeItemLimit, std::int64_t nowMilliseconds) {
    const auto items =
        indexAuthoritativeItems(authoritativeThread.id, &authoritativeThread);
    return project(items, &authoritativeThread, localSubmissions,
                   authoritativeItemLimit, nowMilliseconds);
  }
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONPROJECTION_H
