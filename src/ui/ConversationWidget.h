// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_CONVERSATIONWIDGET_H
#define CODEXUI_UI_CONVERSATIONWIDGET_H

#include <QWidget>

class QFrame;
class QLabel;
class QPlainTextEdit;

namespace codexui {

class ConversationWidget : public QWidget
{
public:
    explicit ConversationWidget(QWidget* parent = nullptr);
    void setThreadIdentity(const QString& id, const QString& title, const QString& detail);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QFrame* composer = nullptr;
    QPlainTextEdit* editor = nullptr;
    QLabel* contextPath = nullptr;
    QLabel* threadTitle = nullptr;
    QLabel* threadDetail = nullptr;
};

} // namespace codexui

#endif // CODEXUI_UI_CONVERSATIONWIDGET_H
