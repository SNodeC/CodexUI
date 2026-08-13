// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_SIDEBARWIDGET_H
#define CODEXUI_UI_SIDEBARWIDGET_H

#include <QWidget>

namespace codexui {

class SidebarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SidebarWidget(QWidget* parent = nullptr);

signals:
    void hideRequested();
};

} // namespace codexui

#endif // CODEXUI_UI_SIDEBARWIDGET_H
