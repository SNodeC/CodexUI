// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_INSPECTORWIDGET_H
#define CODEXUI_UI_INSPECTORWIDGET_H

#include <ai/openai/codex/frontend/client/State.h>

#include <QString>
#include <QWidget>

class QVBoxLayout;

namespace codexui {

class InspectorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorWidget(QWidget* parent = nullptr);

    void render(const ai::openai::codex::frontend::client::State& state,
                const QString& threadId,
                bool backendReady,
                const QString& backendStatus);

signals:
    void hideRequested();
    void selectionChanged();
    void threadOpenRequested(const QString& threadId);

private:
    void renderUnavailable(const QString& title, const QString& detail);
    void refreshLayoutGeometry();

    QVBoxLayout* planContent = nullptr;
    QVBoxLayout* agentsContent = nullptr;
    QVBoxLayout* changesContent = nullptr;
    QVBoxLayout* infoContent = nullptr;
    QString inspectedThreadId;
    QString selectedAgentItemId;
};

} // namespace codexui

#endif // CODEXUI_UI_INSPECTORWIDGET_H
