// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_CONVERSATIONWIDGET_H
#define CODEXUI_UI_CONVERSATIONWIDGET_H

#include <QWidget>

#include <QString>

class QFrame;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
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
                bool newThreadDraft = false);
    void clearPrompt();
    void focusComposer();
    void setActionState(bool sendAllowed, bool stopAllowed, bool editorAllowed);
    void setWriteStatus(const QString& text, bool error = false);

signals:
    void sendRequested(const QString& prompt);
    void stopRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
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
    QVBoxLayout* timeline = nullptr;
    QString renderedThreadId;
    bool renderedNewThreadDraft = false;
    bool sendContextAllowed = false;
};

} // namespace codexui

#endif // CODEXUI_UI_CONVERSATIONWIDGET_H
