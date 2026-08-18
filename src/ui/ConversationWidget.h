// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_CONVERSATIONWIDGET_H
#define CODEXUI_UI_CONVERSATIONWIDGET_H

#include <QWidget>

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdint>

class QFrame;
class QLabel;
class QPlainTextEdit;
class QPropertyAnimation;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace ai::openai::codex::frontend::client {
class State;
}

namespace codexui {

class ConversationWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ConversationWidget(QWidget* parent = nullptr);
    void render(const ai::openai::codex::frontend::client::State& state,
                const QString& threadId,
                bool newThreadDraft = false,
                const QHash<QString, QStringList>* exactContentChanges = nullptr);
    void clearPrompt();
    void focusComposer();
    void setActionState(bool sendAllowed, bool stopAllowed, bool editorAllowed);
    void setWriteStatus(const QString& text, bool error = false);

signals:
    void sendRequested(const QString& prompt);
    void stopRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
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
    void updateSendEnabled();

    QFrame* composer = nullptr;
    QPlainTextEdit* editor = nullptr;
    QLabel* composerStatus = nullptr;
    QPushButton* send = nullptr;
    QPushButton* stop = nullptr;
    QLabel* contextPath = nullptr;
    QLabel* threadTitle = nullptr;
    QLabel* threadDetail = nullptr;
    QFrame* turnSummary = nullptr;
    QVBoxLayout* turnSummaryLayout = nullptr;
    QLabel* turnFailure = nullptr;
    QScrollArea* scrollArea = nullptr;
    QFrame* timelineWindowNotice = nullptr;
    QLabel* timelineWindowDetail = nullptr;
    QWidget* timelineHost = nullptr;
    QVBoxLayout* timeline = nullptr;
    QPropertyAnimation* scrollAnimation = nullptr;
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
    int pendingPreviousScroll = 0;
    int pendingViewportAnchorY = 0;
    bool renderedNewThreadDraft = false;
    bool sendContextAllowed = false;
};

} // namespace codexui

#endif // CODEXUI_UI_CONVERSATIONWIDGET_H
