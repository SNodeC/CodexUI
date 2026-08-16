// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_SIDEBARWIDGET_H
#define CODEXUI_UI_SIDEBARWIDGET_H

#include <QWidget>

#include <vector>

class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace ai::openai::codex::frontend::client {
class State;
}

namespace codexui {

class SidebarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SidebarWidget(QWidget* parent = nullptr);
    void setThreads(const ai::openai::codex::frontend::client::State& state, const QString& selectedThreadId);
    void setConnectionStatus(const QString& title, const QString& detail, const QString& color);
    void setNewThreadEnabled(bool enabled);

signals:
    void hideRequested();
    void newThreadRequested();
    void threadSelected(const QString& threadId);

private:
    struct ThreadPresentation {
        QString id;
        QString title;
        QString details;
        QString color;

        bool operator==(const ThreadPresentation&) const = default;
    };

    QVBoxLayout* threadItems = nullptr;
    QFrame* serverDot = nullptr;
    QLabel* serverTitle = nullptr;
    QLabel* serverDetail = nullptr;
    QPushButton* newThread = nullptr;
    std::vector<ThreadPresentation> renderedThreads;
    QString renderedSelection;
    bool threadsRendered = false;
};

} // namespace codexui

#endif // CODEXUI_UI_SIDEBARWIDGET_H
