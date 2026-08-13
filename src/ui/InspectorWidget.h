// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_INSPECTORWIDGET_H
#define CODEXUI_UI_INSPECTORWIDGET_H

#include <QWidget>

namespace codexui {

class InspectorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorWidget(QWidget* parent = nullptr);

signals:
    void hideRequested();
};

} // namespace codexui

#endif // CODEXUI_UI_INSPECTORWIDGET_H
