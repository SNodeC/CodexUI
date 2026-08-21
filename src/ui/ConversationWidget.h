// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_CONVERSATIONWIDGET_H
#define CODEXUI_UI_CONVERSATIONWIDGET_H

#include "app/AttachmentManager.h"

#include <ai/openai/codex/frontend/client/StateTypes.h>

#include <QWidget>

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <vector>

class QFrame;
class QLabel;
class QResizeEvent;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace ai::openai::codex::frontend::client {
class State;
}
namespace ai::openai::codex::typed {
struct Model;
}

namespace codexui {

class AnchoredTurnSurface;
class UpcomingTurnDock;
struct UpcomingTurnDraft;

struct ConversationContentAppend
{
    std::uint64_t baseContentBytes = 0;
    std::uint64_t discardPrefixBytes = 0;
    std::uint64_t deltaUtf8Bytes = 0;
    QString delta;
};

struct ConversationContentUpdate
{
    QString turnId;
    QString itemId;
    ai::openai::codex::frontend::client::ItemContentChannel channel =
        ai::openai::codex::frontend::client::ItemContentChannel::AgentText;
    std::optional<ConversationContentAppend> append;
};

using ConversationContentUpdates = std::vector<ConversationContentUpdate>;

class ConversationWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ConversationWidget(QWidget* parent = nullptr);
    void render(const ai::openai::codex::frontend::client::State& state,
                const QString& threadId,
                bool newThreadDraft = false,
                const ConversationContentUpdates* exactContentChanges = nullptr);
    void setModelCatalog(const std::vector<ai::openai::codex::typed::Model>& catalog);
    [[nodiscard]] bool updateExactMessageContent(
        const ai::openai::codex::frontend::client::State& state,
        const QString& threadId,
        const ConversationContentUpdates& exactContentChanges);
    void clearPrompt();
    void clearPromptIfUnchanged(const QString& submittedPrompt);
    [[nodiscard]] const QList<AttachmentInfo>& attachments() const noexcept;
    [[nodiscard]] QString attachmentWorkspace() const;
    void clearAttachmentsIfUnchanged(const QList<AttachmentInfo>& submittedAttachments);
    void focusComposer();
    [[nodiscard]] UpcomingTurnDraft upcomingTurnDraft() const;
    void clearUpcomingTurnSettings();
    void acknowledgeSubmittedSettings(const UpcomingTurnDraft& submitted);
    void setActionState(bool primaryAllowed,
                        bool stopAllowed,
                        bool editorAllowed,
                        bool settingsAllowed,
                        bool stopVisible,
                        bool steerMode,
                        const QString& actionThreadIdentity,
                        const QString& activeTurnIdentity);
    void setWriteStatus(const QString& text, bool error = false);

signals:
    void sendRequested(const QString& prompt, bool steerRequested);
    void stopRequested();
    void upcomingTurnSettingsChanged();
    void turnDetailsRequested(const QString& turnId);
    void latestPresentationRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void scheduleTimelineLayout(int previousScroll,
                                bool followLatest,
                                bool threadChanged,
                                bool timelineShrank);
    void captureTimelineAnchor();
    void settleTimelineLayout();
    void settleThreadSwitchLayout(std::uint64_t generation, int remainingPasses);
    void synchronizeTimelineHeight(bool allowShrink = true);
    void activityLayoutChanged();
    [[nodiscard]] bool shouldFreezePresentation(const QString& threadId,
                                                bool newThreadDraft) const;
    void markPresentationDeferred();
    void requestDeferredPresentationAtTail();
    AnchoredTurnSurface* anchoredSurface = nullptr;
    UpcomingTurnDock* upcomingTurnDock = nullptr;
    QLabel* contextPath = nullptr;
    QLabel* threadTitle = nullptr;
    QLabel* threadDetail = nullptr;
    QLabel* turnFailure = nullptr;
    QScrollArea* scrollArea = nullptr;
    QFrame* timelineWindowNotice = nullptr;
    QLabel* timelineWindowDetail = nullptr;
    QWidget* timelineHost = nullptr;
    QVBoxLayout* timeline = nullptr;
    QTimer* layoutSettleTimer = nullptr;
    QString renderedThreadId;
    // Identity only; conversation content remains owned by immutable AISuite State.
    QByteArray renderedSummaryKey;
    QStringList renderedTurnIds;
    QHash<QString, QWidget*> renderedTurnWidgets;
    QHash<QString, QLabel*> renderedTurnLabels;
    QHash<QString, QLabel*> renderedTurnStatusLabels;
    QHash<QString, QVBoxLayout*> renderedTurnItemLayouts;
    QHash<QString, QStringList> renderedSegmentIds;
    QHash<QString, QByteArray> renderedSegmentKeys;
    QHash<QString, QWidget*> renderedSegmentWidgets;
    QPointer<QWidget> pendingViewportAnchor;
    std::uint64_t renderGeneration = 0;
    std::uint64_t pinLatestGeneration = 0;
    bool pinLatestDuringLayout = false;
    bool followingLatest = false;
    bool pendingFollowLatest = false;
    bool pendingThreadChanged = false;
    bool pendingTimelineShrink = false;
    bool resizeLayoutPending = false;
    bool deferredPresentationPending = false;
    bool deferredPresentationRequestScheduled = false;
    int pendingPreviousScroll = 0;
    int pendingViewportAnchorY = 0;
    bool renderedNewThreadDraft = false;
};

} // namespace codexui

#endif // CODEXUI_UI_CONVERSATIONWIDGET_H
