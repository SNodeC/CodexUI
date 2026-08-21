// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_ANCHOREDTURNSURFACE_H
#define CODEXUI_UI_ANCHOREDTURNSURFACE_H

#include <QWidget>

class QResizeEvent;

namespace codexui {

class UpcomingTurnDock;

// Keeps the conversation viewport at a fixed geometry and anchors the dock to
// the bottom edge. When the composer grows, only the dock's top edge moves and
// the additional area overlays the conversation.
class AnchoredTurnSurface final : public QWidget
{
    Q_OBJECT

public:
    explicit AnchoredTurnSurface(QWidget* parent = nullptr);
    void setConversationWidget(QWidget* widget);
    void setUpcomingTurnDock(UpcomingTurnDock* widget);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void relayout();

    QWidget* conversation = nullptr;
    UpcomingTurnDock* dock = nullptr;
};

} // namespace codexui

#endif // CODEXUI_UI_ANCHOREDTURNSURFACE_H
