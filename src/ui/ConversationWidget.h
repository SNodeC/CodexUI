// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_CONVERSATIONWIDGET_H
#define CODEXUI_UI_CONVERSATIONWIDGET_H

#include <QWidget>

#include <QString>

class QFrame;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QVBoxLayout;

namespace ai::openai::codex::frontend::client {
class State;
}

namespace codexui {

class ConversationWidget : public QWidget
{
public:
    explicit ConversationWidget(QWidget* parent = nullptr);
    void render(const ai::openai::codex::frontend::client::State& state, const QString& threadId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QFrame* composer = nullptr;
    QPlainTextEdit* editor = nullptr;
    QLabel* contextPath = nullptr;
    QLabel* threadTitle = nullptr;
    QLabel* threadDetail = nullptr;
    QFrame* turnSummary = nullptr;
    QVBoxLayout* turnSummaryLayout = nullptr;
    QLabel* turnFailure = nullptr;
    QScrollArea* scrollArea = nullptr;
    QVBoxLayout* timeline = nullptr;
    QString renderedThreadId;
};

} // namespace codexui

#endif // CODEXUI_UI_CONVERSATIONWIDGET_H
