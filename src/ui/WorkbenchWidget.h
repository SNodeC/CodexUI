// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_WORKBENCHWIDGET_H
#define CODEXUI_UI_WORKBENCHWIDGET_H

#include <QWidget>

class QPushButton;
class QSplitter;

namespace codexui {

class InspectorWidget;
class SidebarWidget;

class WorkbenchWidget : public QWidget
{
public:
    explicit WorkbenchWidget(QWidget* parent = nullptr);

private:
    void setSidebarVisible(bool visible);
    void setInspectorVisible(bool visible);

    QSplitter* splitter = nullptr;
    SidebarWidget* sidebar = nullptr;
    InspectorWidget* inspector = nullptr;
    QPushButton* restoreSidebar = nullptr;
    QPushButton* restoreInspector = nullptr;
};

} // namespace codexui

#endif // CODEXUI_UI_WORKBENCHWIDGET_H
