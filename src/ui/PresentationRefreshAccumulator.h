// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_PRESENTATIONREFRESHACCUMULATOR_H
#define CODEXUI_UI_PRESENTATIONREFRESHACCUMULATOR_H

#include "app/FrontendSession.h"
#include "ui/ConversationWidget.h"

#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace codexui::detail {

enum class BoundedMergeResult { Retained, CapacityExceeded };

struct SelectedPresentationRefreshAccumulator {
    bool refreshPending = false;
    bool fullRefreshPending = false;
    bool structuralReconciliationPending = false;
    ConversationContentUpdates contentChanges;
    std::uint64_t retainedContentUtf8Bytes = 0;

    void clear() noexcept;
};

void mergeSelectedPresentationRefresh(
    SelectedPresentationRefreshAccumulator& accumulator,
    const StateUpdateScope& scope,
    const QString& selectedThreadId,
    bool awaitedSelectionAffected);

[[nodiscard]] BoundedMergeResult mergeConversationContentUpdate(
    ConversationContentUpdates& updates,
    std::uint64_t& retainedUtf8Bytes,
    const StateUpdateScope::ItemContentIdentity& identity);

[[nodiscard]] BoundedMergeResult appendUniqueSidebarThread(
    QStringList& orderedThreadIds,
    QSet<QString>& retainedThreadIds,
    const QString& threadId);

} // namespace codexui::detail

#endif // CODEXUI_UI_PRESENTATIONREFRESHACCUMULATOR_H
