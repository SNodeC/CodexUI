// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/PresentationRefreshAccumulator.h"

#include <QCoreApplication>

#include <iostream>

namespace {

namespace detail = codexui::detail;
namespace client = ai::openai::codex::frontend::client;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

detail::StateUpdateScope::ItemContentIdentity identity(
    int index,
    QByteArray delta = {},
    std::uint64_t base = 0,
    std::uint64_t discard = 0)
{
    detail::StateUpdateScope::ItemContentIdentity result;
    result.threadId = QStringLiteral("thread");
    result.turnId = QStringLiteral("turn");
    result.itemId = QStringLiteral("item-%1").arg(index);
    result.channel = client::ItemContentChannel::CommandOutput;
    if (!delta.isNull())
    {
        result.append = detail::StateUpdateScope::ItemContentAppend{
            base, discard, std::move(delta)};
    }
    return result;
}

bool testIdentityBound()
{
    codexui::ConversationContentUpdates updates;
    std::uint64_t retainedBytes = 0;
    bool passed = true;
    for (qsizetype index = 0;
         index < detail::maximumCoalescedPresentationIdentities;
         ++index)
    {
        passed &= detail::mergeConversationContentUpdate(
                      updates, retainedBytes, identity(static_cast<int>(index)))
                  == detail::BoundedMergeResult::Retained;
    }
    const auto retainedSize = updates.size();
    passed &= expect(
        detail::mergeConversationContentUpdate(
            updates,
            retainedBytes,
            identity(static_cast<int>(detail::maximumCoalescedPresentationIdentities)))
            == detail::BoundedMergeResult::CapacityExceeded
            && updates.size() == retainedSize && retainedBytes == 0,
        "the frame accumulator must retain exactly 1024 identities and reject the 1025th without mutation");
    return passed;
}

bool testAggregateByteBound()
{
    codexui::ConversationContentUpdates updates;
    std::uint64_t retainedBytes = 0;
    QByteArray maximum(
        static_cast<qsizetype>(detail::maximumCoalescedContentDeltaBytes), 'x');
    bool passed = detail::mergeConversationContentUpdate(
                      updates, retainedBytes, identity(0, std::move(maximum)))
                  == detail::BoundedMergeResult::Retained;
    const auto retainedSize = updates.size();
    passed &= expect(
        detail::mergeConversationContentUpdate(
            updates,
            retainedBytes,
            identity(1, QByteArray(1, 'y')))
            == detail::BoundedMergeResult::CapacityExceeded
            && updates.size() == retainedSize
            && retainedBytes == detail::maximumCoalescedContentDeltaBytes,
        "the frame accumulator must reject the first byte beyond its aggregate 1 MiB bound without mutation");
    return passed;
}

bool testReplacementReleasesRetainedBytes()
{
    codexui::ConversationContentUpdates updates;
    std::uint64_t retainedBytes = 0;
    const auto half = detail::maximumCoalescedContentDeltaBytes / 2;
    bool passed = detail::mergeConversationContentUpdate(
                      updates,
                      retainedBytes,
                      identity(0, QByteArray(static_cast<qsizetype>(half), 'a')))
                  == detail::BoundedMergeResult::Retained;
    passed &= detail::mergeConversationContentUpdate(
                  updates,
                  retainedBytes,
                  identity(0, QByteArray(1, 'b'), 9'999))
              == detail::BoundedMergeResult::Retained;
    passed &= expect(retainedBytes == 0 && updates.size() == 1
                         && !updates.front().append,
                     "a non-contiguous update must become an authoritative replacement and release retained delta bytes");
    passed &= expect(
        detail::mergeConversationContentUpdate(
            updates,
            retainedBytes,
            identity(1,
                     QByteArray(
                         static_cast<qsizetype>(detail::maximumCoalescedContentDeltaBytes),
                         'c')))
            == detail::BoundedMergeResult::Retained
            && retainedBytes == detail::maximumCoalescedContentDeltaBytes,
        "released replacement bytes must be available to a later independent exact append");
    return passed;
}

bool testContiguousAppendMerge()
{
    codexui::ConversationContentUpdates updates;
    std::uint64_t retainedBytes = 0;
    bool passed = detail::mergeConversationContentUpdate(
                      updates, retainedBytes, identity(0, QByteArray("abc"), 10))
                  == detail::BoundedMergeResult::Retained;
    passed &= detail::mergeConversationContentUpdate(
                  updates, retainedBytes, identity(0, QByteArray("def"), 13))
              == detail::BoundedMergeResult::Retained;
    passed &= expect(updates.size() == 1 && updates.front().append
                         && updates.front().append->baseContentBytes == 10
                         && updates.front().append->deltaUtf8Bytes == 6
                         && updates.front().append->delta == QStringLiteral("abcdef")
                         && retainedBytes == 6,
                     "contiguous updates for one channel must merge with exact byte accounting");
    return passed;
}

bool testSidebarIdentityBound()
{
    QStringList ordered;
    QSet<QString> retained;
    bool passed = true;
    for (qsizetype index = 0;
         index < detail::maximumCoalescedPresentationIdentities;
         ++index)
    {
        const QString id = QStringLiteral("thread-%1").arg(index);
        passed &= detail::appendUniqueSidebarThread(ordered, retained, id)
                  == detail::BoundedMergeResult::Retained;
        passed &= detail::appendUniqueSidebarThread(ordered, retained, id)
                  == detail::BoundedMergeResult::Retained;
    }
    passed &= expect(
        ordered.size() == detail::maximumCoalescedPresentationIdentities
            && retained.size() == detail::maximumCoalescedPresentationIdentities
            && detail::appendUniqueSidebarThread(
                   ordered, retained, QStringLiteral("thread-overflow"))
                   == detail::BoundedMergeResult::CapacityExceeded
            && ordered.size() == detail::maximumCoalescedPresentationIdentities,
        "sidebar duplicates must not consume capacity and the 1025th unique identity must be rejected");
    return passed;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    bool passed = true;
    passed &= testIdentityBound();
    passed &= testAggregateByteBound();
    passed &= testReplacementReleasesRetainedBytes();
    passed &= testContiguousAppendMerge();
    passed &= testSidebarIdentityBound();
    return passed ? 0 : 1;
}
