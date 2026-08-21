// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/PresentationRefreshAccumulator.h"

#include <algorithm>
#include <limits>

namespace codexui::detail {
namespace {

[[nodiscard]] bool canRetainBytes(std::uint64_t retained,
                                  std::uint64_t additional) noexcept
{
    return retained <= maximumCoalescedContentDeltaBytes
           && additional <= maximumCoalescedContentDeltaBytes - retained;
}

} // namespace

BoundedMergeResult mergeConversationContentUpdate(
    ConversationContentUpdates& updates,
    std::uint64_t& retainedUtf8Bytes,
    const StateUpdateScope::ItemContentIdentity& identity)
{
    auto existing = std::find_if(
        updates.begin(), updates.end(),
        [&identity](const ConversationContentUpdate& update)
        {
            return update.turnId == identity.turnId
                   && update.itemId == identity.itemId
                   && update.channel == identity.channel;
        });
    if (existing == updates.end())
    {
        if (static_cast<qsizetype>(updates.size())
            >= maximumCoalescedPresentationIdentities)
            return BoundedMergeResult::CapacityExceeded;

        ConversationContentUpdate next{
            identity.turnId,
            identity.itemId,
            identity.channel,
            std::nullopt};
        if (identity.append)
        {
            const std::uint64_t deltaBytes = static_cast<std::uint64_t>(
                identity.append->deltaUtf8.size());
            if (!canRetainBytes(retainedUtf8Bytes, deltaBytes))
                return BoundedMergeResult::CapacityExceeded;
            next.append = ConversationContentAppend{
                identity.append->baseContentBytes,
                identity.append->discardPrefixBytes,
                deltaBytes,
                QString::fromUtf8(identity.append->deltaUtf8)};
            retainedUtf8Bytes += deltaBytes;
        }
        updates.push_back(std::move(next));
        return BoundedMergeResult::Retained;
    }

    if (existing->append && identity.append
        && existing->append->discardPrefixBytes == 0
        && identity.append->discardPrefixBytes == 0
        && existing->append->baseContentBytes
               <= std::numeric_limits<std::uint64_t>::max()
                      - existing->append->deltaUtf8Bytes
        && existing->append->baseContentBytes
               + existing->append->deltaUtf8Bytes
               == identity.append->baseContentBytes)
    {
        const std::uint64_t deltaBytes = static_cast<std::uint64_t>(
            identity.append->deltaUtf8.size());
        if (!canRetainBytes(retainedUtf8Bytes, deltaBytes))
            return BoundedMergeResult::CapacityExceeded;
        existing->append->delta.append(QString::fromUtf8(identity.append->deltaUtf8));
        existing->append->deltaUtf8Bytes += deltaBytes;
        retainedUtf8Bytes += deltaBytes;
        return BoundedMergeResult::Retained;
    }

    // A replacement, rolling window, or non-contiguous append is represented
    // by an authoritative item refresh. It no longer retains the previous
    // optional text delta in this frame accumulator.
    if (existing->append)
    {
        retainedUtf8Bytes = existing->append->deltaUtf8Bytes
                                    <= retainedUtf8Bytes
                                ? retainedUtf8Bytes
                                      - existing->append->deltaUtf8Bytes
                                : 0;
    }
    existing->append.reset();
    return BoundedMergeResult::Retained;
}

BoundedMergeResult appendUniqueSidebarThread(
    QStringList& orderedThreadIds,
    QSet<QString>& retainedThreadIds,
    const QString& threadId)
{
    if (retainedThreadIds.contains(threadId))
        return BoundedMergeResult::Retained;
    if (orderedThreadIds.size() >= maximumCoalescedPresentationIdentities)
        return BoundedMergeResult::CapacityExceeded;
    retainedThreadIds.insert(threadId);
    orderedThreadIds.append(threadId);
    return BoundedMergeResult::Retained;
}

} // namespace codexui::detail
