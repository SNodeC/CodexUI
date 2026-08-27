// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/PromptCoordinator.h"

#include <QUrl>
#include <algorithm>
#include <set>
#include <utility>

namespace codexui::codex::middle {
namespace {

QString text(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto value = object.find(key);
  return value != object.end() && value->is_string() ? value->get<std::string>()
                                                     : std::string{};
}

QString userMessageText(const nlohmann::json &item) {
  QStringList parts;
  const auto content = item.find("content");
  if (content != item.end() && content->is_array()) {
    for (const nlohmann::json &entry : *content) {
      const std::string value = stringValue(entry, "text");
      if (!value.empty())
        parts.push_back(text(value));
    }
  }
  if (parts.empty()) {
    const std::string value = stringValue(item, "text");
    if (!value.empty())
      parts.push_back(text(value));
  }
  return parts.join(QStringLiteral("\n"));
}

QString markdownLinkLabel(QString label) {
  label.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  label.replace(QLatin1Char('['), QStringLiteral("\\["));
  label.replace(QLatin1Char(']'), QStringLiteral("\\]"));
  label.replace(QLatin1Char('\r'), QLatin1Char(' '));
  label.replace(QLatin1Char('\n'), QLatin1Char(' '));
  return label;
}

} // namespace

std::optional<std::size_t> AuthoritativeItemIndex::position(
    const AuthoritativeItemKey &key) const noexcept {
  const auto found = positions.find(key);
  return found == positions.end() ? std::nullopt
                                  : std::optional<std::size_t>{found->second};
}

AuthoritativeItemIndex
indexAuthoritativeItems(const std::string &threadId,
                        const ThreadPresentation *thread) {
  AuthoritativeItemIndex result;
  result.threadId = threadId;
  if (!thread)
    return result;
  for (const std::string &turnId : thread->turnOrder) {
    const auto turn = thread->turns.find(turnId);
    if (turn == thread->turns.end())
      continue;
    for (const std::string &itemId : turn->second.itemOrder) {
      const auto item = turn->second.items.find(itemId);
      if (item == turn->second.items.end())
        continue;
      const std::size_t position = result.ordered.size();
      result.ordered.push_back(
          {AuthoritativeItemKey{threadId, turnId, itemId}, &item->second});
      result.positions.emplace(result.ordered.back().key, position);
      if (stringValue(item->second.raw, "type") == "userMessage") {
        const std::string clientId = stringValue(item->second.raw, "clientId");
        if (!clientId.empty())
          result.userMessagesByClientId.try_emplace(clientId, position);
        const QString content = userMessageText(item->second.raw).trimmed();
        result.userMessagesByText.emplace(std::string{}, content, position);
        result.userMessagesByText.emplace(turnId, content, position);
      }
    }
  }
  return result;
}

QString promptWithFileLinks(QString prompt,
                            std::span<const AttachmentDraft> attachments) {
  QStringList links;
  for (const AttachmentDraft &attachment : attachments) {
    if (attachment.mimeType.startsWith(QStringLiteral("image/")) ||
        attachment.mimeType.startsWith(QStringLiteral("audio/")))
      continue;
    QString target =
        QUrl::fromLocalFile(attachment.path).toString(QUrl::FullyEncoded);
    target.replace(QLatin1Char('['), QStringLiteral("%5B"));
    target.replace(QLatin1Char(']'), QStringLiteral("%5D"));
    target.replace(QLatin1Char('('), QStringLiteral("%28"));
    target.replace(QLatin1Char(')'), QStringLiteral("%29"));
    links.push_back(QStringLiteral("- [%1](%2)")
                        .arg(markdownLinkLabel(attachment.name), target));
  }
  if (links.empty())
    return prompt;
  return prompt + QStringLiteral("\n\nAttached files:\n") +
         links.join(QLatin1Char('\n'));
}

bool PromptSubmission::acceptedTransitionActive(
    qint64 nowMilliseconds) const noexcept {
  return state == PromptState::Accepted && acceptedAtMilliseconds > 0 &&
         nowMilliseconds >= acceptedAtMilliseconds &&
         nowMilliseconds - acceptedAtMilliseconds <
             AcknowledgementTransitionMilliseconds;
}

bool PromptSubmission::localCardVisible(qint64 nowMilliseconds) const noexcept {
  return state == PromptState::Queued || state == PromptState::InFlight ||
         state == PromptState::Failed || !materializedItem ||
         acceptedTransitionActive(nowMilliseconds);
}

std::uint64_t PromptCoordinator::admit(
    std::string threadId, QString prompt,
    std::vector<AttachmentDraft> attachments, nlohmann::json turnOptions,
    const ThreadPresentation *authoritativeThread,
    std::optional<std::string> activeTurnId, qint64 nowMilliseconds) {
  PromptSubmission submission;
  submission.id = nextSubmissionId++;
  submission.admissionOrdinal = nextAdmissionOrdinal++;
  submission.threadId = std::move(threadId);
  submission.clientUserMessageId = "codexui-" +
                                   std::to_string(nowMilliseconds) + '-' +
                                   std::to_string(submission.id);
  submission.prompt = std::move(prompt);
  submission.attachments = std::move(attachments);
  submission.turnOptions = std::move(turnOptions);
  submission.expectedTurnId = std::move(activeTurnId);

  if (authoritativeThread) {
    const auto items =
        indexAuthoritativeItems(submission.threadId, authoritativeThread);
    if (!items.ordered.empty())
      submission.admissionAnchor = items.ordered.back().key;
  }

  const std::uint64_t id = submission.id;
  byThread[submission.threadId].push_back(std::move(submission));
  return id;
}

std::optional<PromptDispatch>
PromptCoordinator::beginNext(const std::string &threadId,
                             std::optional<std::string> activeTurnId) {
  auto found = byThread.find(threadId);
  if (found == byThread.end())
    return std::nullopt;
  if (std::any_of(found->second.begin(), found->second.end(),
                  [](const PromptSubmission &submission) {
                    return submission.state == PromptState::InFlight;
                  }))
    return std::nullopt;
  auto next = std::find_if(found->second.begin(), found->second.end(),
                           [](const PromptSubmission &submission) {
                             return submission.state == PromptState::Queued;
                           });
  if (next == found->second.end())
    return std::nullopt;
  next->state = PromptState::InFlight;
  // Start versus steer is an operation-time fact. A turn which was active
  // when the prompt entered the local queue may have completed meanwhile.
  next->expectedTurnId = std::move(activeTurnId);
  return PromptDispatch{next->id,
                        next->threadId,
                        next->clientUserMessageId,
                        next->prompt,
                        next->attachments,
                        next->turnOptions,
                        next->expectedTurnId};
}

bool PromptCoordinator::acknowledge(
    const std::string &threadId, std::uint64_t submissionId,
    std::optional<std::string> authoritativeTurnId, qint64 nowMilliseconds) {
  PromptSubmission *pending = find(threadId, submissionId);
  if (!pending || pending->state != PromptState::InFlight)
    return false;
  pending->state = PromptState::Accepted;
  pending->acceptedAtMilliseconds = nowMilliseconds;
  pending->error.clear();
  if (authoritativeTurnId)
    pending->expectedTurnId = std::move(authoritativeTurnId);
  return true;
}

bool PromptCoordinator::fail(const std::string &threadId,
                             std::uint64_t submissionId, QString error) {
  PromptSubmission *pending = find(threadId, submissionId);
  if (!pending || (pending->state != PromptState::InFlight &&
                   pending->state != PromptState::Queued))
    return false;
  pending->state = PromptState::Failed;
  pending->error = std::move(error);
  return true;
}

bool PromptCoordinator::requeue(const std::string &threadId,
                                std::uint64_t submissionId) {
  PromptSubmission *pending = find(threadId, submissionId);
  if (!pending || pending->state != PromptState::InFlight)
    return false;
  pending->state = PromptState::Queued;
  return true;
}

std::size_t PromptCoordinator::failQueued(const std::string &threadId,
                                          const QString &error) {
  auto found = byThread.find(threadId);
  if (found == byThread.end())
    return 0;
  std::size_t count = 0;
  for (PromptSubmission &submission : found->second) {
    if (submission.state != PromptState::Queued)
      continue;
    submission.state = PromptState::Failed;
    submission.error = error;
    ++count;
  }
  return count;
}

bool PromptCoordinator::reassignThread(const std::string &fromThreadId,
                                       const std::string &toThreadId) {
  if (fromThreadId == toThreadId)
    return true;
  auto source = byThread.find(fromThreadId);
  if (source == byThread.end())
    return true;
  auto destination = byThread.find(toThreadId);
  const bool sourceInFlight =
      std::any_of(source->second.begin(), source->second.end(),
                  [](const PromptSubmission &submission) {
                    return submission.state == PromptState::InFlight;
                  });
  const bool destinationInFlight =
      destination != byThread.end() &&
      std::any_of(destination->second.begin(), destination->second.end(),
                  [](const PromptSubmission &submission) {
                    return submission.state == PromptState::InFlight;
                  });
  if (sourceInFlight && destinationInFlight)
    return false;

  std::vector<PromptSubmission> moved = std::move(source->second);
  byThread.erase(source);
  for (PromptSubmission &submission : moved) {
    submission.threadId = toThreadId;
    if (submission.admissionAnchor)
      submission.admissionAnchor->threadId = toThreadId;
    if (submission.materializedItem)
      submission.materializedItem->threadId = toThreadId;
  }
  auto &target = byThread[toThreadId];
  target.insert(target.end(), std::make_move_iterator(moved.begin()),
                std::make_move_iterator(moved.end()));
  std::ranges::sort(target, {}, &PromptSubmission::admissionOrdinal);
  return true;
}

void PromptCoordinator::reconcile(const std::string &threadId,
                                  const ThreadPresentation &authoritativeThread) {
  auto authoritativeItems =
      indexAuthoritativeItems(threadId, &authoritativeThread);
  reconcile(threadId, authoritativeItems);
}

void PromptCoordinator::reconcile(const std::string &threadId,
                                  AuthoritativeItemIndex &authoritativeItems) {
  auto found = byThread.find(threadId);
  if (found == byThread.end())
    return;
  std::vector<bool> claimed(authoritativeItems.ordered.size());
  for (const PromptSubmission &submission : found->second)
    if (submission.materializedItem) {
      const auto position =
          authoritativeItems.position(*submission.materializedItem);
      if (position)
        claimed[*position] = true;
    }

  for (PromptSubmission &submission : found->second) {
    if (submission.materializedItem)
      continue;
    if (!submission.admissionAnchor &&
        submission.state == PromptState::Queued &&
        !authoritativeItems.ordered.empty())
      submission.admissionAnchor = authoritativeItems.ordered.back().key;

    const auto exact = authoritativeItems.userMessagesByClientId.find(
        submission.clientUserMessageId);
    if (exact == authoritativeItems.userMessagesByClientId.end() ||
        claimed[exact->second])
      continue;
    const AuthoritativeItemKey &key =
        authoritativeItems.ordered[exact->second].key;
    submission.materializedItem = key;
    submission.expectedTurnId = key.turnId;
    claimed[exact->second] = true;
  }

  for (PromptSubmission &submission : found->second) {
    if (submission.materializedItem)
      continue;

    // Semantic acknowledgement remains callback-only. Without protocol
    // client-id support, do not guess from text before that callback arrives.
    if (submission.state != PromptState::Accepted)
      continue;

    std::size_t firstCandidate = 0;
    if (submission.admissionAnchor) {
      const auto anchor =
          authoritativeItems.position(*submission.admissionAnchor);
      if (anchor)
        firstCandidate = *anchor + 1;
    }

    const std::string turnId =
        submission.expectedTurnId.value_or(std::string{});
    const QString prompt = submission.prompt.trimmed();
    auto candidate = authoritativeItems.userMessagesByText.lower_bound(
        {turnId, prompt, firstCandidate});
    while (candidate != authoritativeItems.userMessagesByText.end() &&
           std::get<0>(*candidate) == turnId &&
           std::get<1>(*candidate) == prompt &&
           claimed[std::get<2>(*candidate)])
      candidate = authoritativeItems.userMessagesByText.erase(candidate);
    if (candidate == authoritativeItems.userMessagesByText.end() ||
        std::get<0>(*candidate) != turnId || std::get<1>(*candidate) != prompt)
      continue;
    const std::size_t index = std::get<2>(*candidate);
    const AuthoritativeItemKey &key = authoritativeItems.ordered[index].key;
    submission.materializedItem = key;
    if (!submission.expectedTurnId)
      submission.expectedTurnId = key.turnId;
    claimed[index] = true;
    authoritativeItems.userMessagesByText.erase(candidate);
  }
}

void PromptCoordinator::compactResolved(const std::string &threadId,
                                        qint64 nowMilliseconds) {
  auto found = byThread.find(threadId);
  if (found == byThread.end())
    return;
  for (PromptSubmission &submission : found->second) {
    if (submission.state != PromptState::Accepted ||
        !submission.materializedItem ||
        submission.acceptedTransitionActive(nowMilliseconds) ||
        submission.clientUserMessageId.empty())
      continue;
    std::string{}.swap(submission.clientUserMessageId);
    QString{}.swap(submission.prompt);
    std::vector<AttachmentDraft>{}.swap(submission.attachments);
    submission.turnOptions = nlohmann::json::object();
    submission.error.clear();
    submission.error.squeeze();
    submission.expectedTurnId.reset();
    submission.acceptedAtMilliseconds = 0;
  }
}

std::span<const PromptSubmission>
PromptCoordinator::submissions(const std::string &threadId) const noexcept {
  const auto found = byThread.find(threadId);
  if (found == byThread.end())
    return {};
  return found->second;
}

const PromptSubmission *
PromptCoordinator::submission(const std::string &threadId,
                              std::uint64_t submissionId) const noexcept {
  const auto found = byThread.find(threadId);
  if (found == byThread.end())
    return nullptr;
  const auto candidate =
      std::ranges::find(found->second, submissionId, &PromptSubmission::id);
  return candidate == found->second.end() ? nullptr : &*candidate;
}

bool PromptCoordinator::hasInFlight(
    const std::string &threadId) const noexcept {
  const auto pending = submissions(threadId);
  return std::ranges::any_of(pending, [](const PromptSubmission &submission) {
    return submission.state == PromptState::InFlight;
  });
}

std::vector<std::string> PromptCoordinator::queuedThreadIds() const {
  std::vector<std::string> result;
  for (const auto &[threadId, submissions] : byThread) {
    if (std::ranges::any_of(submissions,
                            [](const PromptSubmission &submission) {
                              return submission.state == PromptState::Queued;
                            }))
      result.push_back(threadId);
  }
  return result;
}

void PromptCoordinator::clearThread(const std::string &threadId) {
  byThread.erase(threadId);
}

PromptSubmission *PromptCoordinator::find(const std::string &threadId,
                                          std::uint64_t submissionId) noexcept {
  auto found = byThread.find(threadId);
  if (found == byThread.end())
    return nullptr;
  auto candidate =
      std::ranges::find(found->second, submissionId, &PromptSubmission::id);
  return candidate == found->second.end() ? nullptr : &*candidate;
}

} // namespace codexui::codex::middle
