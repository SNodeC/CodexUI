// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_INSPECTORWIDGET_H
#define CODEXUI_UI_INSPECTORWIDGET_H

#include <ai/openai/codex/frontend/client/State.h>

#include <QByteArray>
#include <QString>
#include <QWidget>
#include <QSet>

#include <cstdint>

class QVBoxLayout;
class QLabel;
class QScrollArea;
class QTabBar;
class QPushButton;

namespace codexui {

class InspectorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorWidget(QWidget* parent = nullptr);

    void render(const ai::openai::codex::frontend::client::State& state,
                const QString& threadId,
                bool backendReady,
                const QString& backendStatus,
                const QString& selectedTurnId = {});
    void updateStateRevision(std::uint64_t revision);
    [[nodiscard]] bool dependsOnThread(const QString& threadId) const;
    void showInfo();

signals:
    void hideRequested();
    void historicalTurnCloseRequested();
    void selectionChanged();
    void threadOpenRequested(const QString& threadId);

private:
    void renderUnavailable(const QString& title, const QString& detail);
    void refreshLayoutGeometry(QVBoxLayout* layout);
    void setHistoricalTurnMode(bool enabled);

    QVBoxLayout* planContent = nullptr;
    QVBoxLayout* agentsContent = nullptr;
    QVBoxLayout* changesContent = nullptr;
    QVBoxLayout* infoContent = nullptr;
    QString inspectedThreadId;
    QSet<QString> dependentThreadIds;
    QString selectedAgentItemId;
    QByteArray unavailablePresentationKey;
    QByteArray planPresentationKey;
    QByteArray agentsPresentationKey;
    QByteArray changesPresentationKey;
    QByteArray infoPresentationKey;
    QLabel* infoRevisionValue = nullptr;
    QLabel* inspectorHeading = nullptr;
    QPushButton* historicalBack = nullptr;
    QScrollArea* infoScroll = nullptr;
    QTabBar* tabs = nullptr;
    bool historicalTurnMode = false;
    int normalTabIndex = 1;
};

} // namespace codexui

#endif // CODEXUI_UI_INSPECTORWIDGET_H
