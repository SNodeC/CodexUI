// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_PROMPTCOORDINATOR_H
#define CODEXUI_CODEX_MIDDLE_PROMPTCOORDINATOR_H

#include "codex/AttachmentDraft.h"
#include "codex/PresentationModel.h"
#include "codex/middle/MiddleTypes.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace codexui::codex::middle {

[[nodiscard]] std::string
promptWithFileLinks(std::string prompt,
                    std::span<const AttachmentDraft> attachments);

struct PromptSubmission {
  std::uint64_t id = 0;
  std::uint64_t admissionOrdinal = 0;
  std::string threadId;
  std::string clientUserMessageId;
  std::string prompt;
  std::vector<AttachmentDraft> attachments;
  nlohmann::json turnOptions = nlohmann::json::object();
  PromptState state = PromptState::Queued;
  std::int64_t acceptedAtMilliseconds = 0;
  std::string error;
  std::optional<AuthoritativeItemKey> admissionAnchor;
  bool admissionAtStart = false;
  bool startsTurn = false;
  std::optional<std::string> expectedTurnId;
  std::optional<AuthoritativeItemKey> materializedItem;

  [[nodiscard]] bool
  acceptedTransitionActive(std::int64_t nowMilliseconds) const noexcept;
  [[nodiscard]] bool
  localCardVisible(std::int64_t nowMilliseconds) const noexcept;
};

struct PromptDispatch {
  std::uint64_t id = 0;
  std::string threadId;
  std::string clientUserMessageId;
  std::string prompt;
  std::vector<AttachmentDraft> attachments;
  nlohmann::json turnOptions = nlohmann::json::object();
  std::optional<std::string> expectedTurnId;
};

struct PromptVisualAlias {
  LocalPromptKey key;
  std::optional<AuthoritativeItemKey> admissionAnchor;
  std::uint64_t admissionOrdinal = 0;
};

struct AuthoritativeItem {
  AuthoritativeItemKey key;
  const ItemPresentation *presentation = nullptr;
  std::optional<PromptVisualAlias> promptAlias;
};

struct AuthoritativeItemIndex {
  std::string threadId;
  std::vector<AuthoritativeItem> ordered;
  std::map<AuthoritativeItemKey, std::size_t> positions;
  std::unordered_map<std::string, std::size_t> userMessagesByClientId;
  std::set<std::tuple<std::string, std::string, std::size_t>>
      userMessagesByText;
  std::unordered_map<std::string, std::size_t> turnRootUserMessagePositions;

  [[nodiscard]] std::optional<std::size_t>
  position(const AuthoritativeItemKey &key) const noexcept;
};

[[nodiscard]] AuthoritativeItemIndex
indexAuthoritativeItems(const std::string &threadId,
                        const ThreadPresentation *thread);

// Owns only local submission state. It does not schedule timers and cannot
// infer acknowledgement from presentation events: acknowledge() is intended
// to be called exclusively by the matching turn.start/turn.steer completion.
class PromptCoordinator final {
public:
  [[nodiscard]] std::uint64_t
  admit(std::string threadId, std::string prompt,
        std::vector<AttachmentDraft> attachments, nlohmann::json turnOptions,
        const ThreadPresentation *authoritativeThread,
        std::optional<std::string> activeTurnId, std::int64_t nowMilliseconds);

  // Starts at most one queued submission for a thread. The active turn is
  // sampled at dispatch time because earlier queued submissions may have
  // created a turn since admission.
  [[nodiscard]] std::optional<PromptDispatch>
  beginNext(const std::string &threadId,
            std::optional<std::string> activeTurnId = std::nullopt);

  [[nodiscard]] bool acknowledge(const std::string &threadId,
                                 std::uint64_t submissionId,
                                 std::optional<std::string> authoritativeTurnId,
                                 std::int64_t nowMilliseconds);
  [[nodiscard]] bool fail(const std::string &threadId,
                          std::uint64_t submissionId, std::string error);
  [[nodiscard]] bool requeue(const std::string &threadId,
                             std::uint64_t submissionId);
  std::size_t failQueued(const std::string &threadId, const std::string &error);

  // Used when the app-server assigns an id to an explicit New Thread draft.
  // LocalPromptKey is unaffected by this move.
  [[nodiscard]] bool reassignThread(const std::string &fromThreadId,
                                    const std::string &toThreadId);

  // Correlates prompts with authoritative userMessage items. Exact client ids
  // may bind before acknowledgement so the awaiting card is never duplicated;
  // the content fallback is used only after the real operation callback. Fully
  // resolved submissions are removed after their accepted transition while a
  // compact visual alias retains the admitted card identity and boundary.
  void reconcile(const std::string &threadId,
                 const ThreadPresentation &authoritativeThread,
                 std::int64_t nowMilliseconds);
  void reconcile(const std::string &threadId,
                 AuthoritativeItemIndex &authoritativeItems,
                 std::int64_t nowMilliseconds);

  [[nodiscard]] std::span<const PromptSubmission>
  submissions(const std::string &threadId) const noexcept;
  [[nodiscard]] const PromptSubmission *
  submission(const std::string &threadId,
             std::uint64_t submissionId) const noexcept;
  [[nodiscard]] bool hasInFlight(const std::string &threadId) const noexcept;
  [[nodiscard]] std::vector<std::string> queuedThreadIds() const;

  void clearThread(const std::string &threadId);

private:
  [[nodiscard]] PromptSubmission *find(const std::string &threadId,
                                       std::uint64_t submissionId) noexcept;
  void applyVisualAliases(const std::string &threadId,
                          AuthoritativeItemIndex &authoritativeItems) const;

  std::map<std::string, std::vector<PromptSubmission>> byThread;
  std::map<std::string, std::map<AuthoritativeItemKey, PromptVisualAlias>>
      visualAliasesByThread;
  std::uint64_t nextSubmissionId = 1;
  std::uint64_t nextAdmissionOrdinal = 1;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_PROMPTCOORDINATOR_H
