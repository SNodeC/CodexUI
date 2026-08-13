// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_WORKBENCHWIDGET_H
#define CODEXUI_UI_WORKBENCHWIDGET_H

#include <QWidget>
#include <QString>

class QFrame;
class QLabel;
class QPushButton;
class QSplitter;

namespace codexui {

class ConversationWidget;
class FrontendSession;
class InspectorWidget;
class SidebarWidget;

class WorkbenchWidget : public QWidget
{
public:
    explicit WorkbenchWidget(FrontendSession& frontendSession, QWidget* parent = nullptr);

private:
    void refreshLifecycle();
    void refreshState();
    void selectThread(const QString& threadId);
    void setSidebarVisible(bool visible);
    void setInspectorVisible(bool visible);

    FrontendSession& frontendSession;
    QSplitter* splitter = nullptr;
    SidebarWidget* sidebar = nullptr;
    ConversationWidget* conversation = nullptr;
    InspectorWidget* inspector = nullptr;
    QPushButton* restoreSidebar = nullptr;
    QPushButton* restoreInspector = nullptr;
    QFrame* codexStatusDot = nullptr;
    QLabel* synchronizationStatus = nullptr;
    QString selectedThreadId;
};

} // namespace codexui

#endif // CODEXUI_UI_WORKBENCHWIDGET_H
