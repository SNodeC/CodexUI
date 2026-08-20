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
struct ThreadState;
}

namespace codexui {

enum class ThreadAction {
    Open,
    Rename,
    Fork,
    Interrupt,
    ResumeWithOptions,
    Archive,
    Unarchive,
    Delete,
    CopyId,
};

struct ThreadActionAvailability {
    bool open = true;
    bool rename = true;
    bool fork = true;
    bool interrupt = false;
    bool resumeWithOptions = true;
    bool archive = false;
    bool unarchive = false;
    bool remove = true;

    bool operator==(const ThreadActionAvailability&) const = default;
};

namespace detail {
[[nodiscard]] ThreadActionAvailability
threadActionAvailability(const ai::openai::codex::frontend::client::State& state,
                         const ai::openai::codex::frontend::client::ThreadState& thread);
}

class SidebarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SidebarWidget(QWidget* parent = nullptr);
    void setThreads(const ai::openai::codex::frontend::client::State& state, const QString& selectedThreadId);
    void setConnectionStatus(const QString& title, const QString& detail, const QString& color);
    void setNewThreadEnabled(bool enabled);
    void setThreadInteractionEnabled(bool enabled);

signals:
    void hideRequested();
    void newThreadRequested();
    void threadSelected(const QString& threadId);
    void threadActionRequested(const QString& threadId, codexui::ThreadAction action);

private:
    struct ThreadPresentation {
        QString id;
        QString title;
        QString details;
        QString color;
        ThreadActionAvailability actions;
        bool running = false;
        bool attention = false;
        bool archived = false;

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
    bool threadInteractionEnabled = false;
};

} // namespace codexui

#endif // CODEXUI_UI_SIDEBARWIDGET_H
