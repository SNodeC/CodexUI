// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "ui/AnchoredTurnSurface.h"

#include "ui/UpcomingTurnDock.h"

#include <QEvent>
#include <QResizeEvent>

#include <algorithm>

namespace codexui {

AnchoredTurnSurface::AnchoredTurnSurface(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("anchoredTurnSurface"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#anchoredTurnSurface{background:#f6f8fb;}"));
}

void AnchoredTurnSurface::setConversationWidget(QWidget* widget)
{
    if (conversation == widget)
        return;
    if (conversation)
        conversation->removeEventFilter(this);
    conversation = widget;
    if (conversation) {
        conversation->setParent(this);
        conversation->installEventFilter(this);
        conversation->show();
        conversation->lower();
    }
    relayout();
}

void AnchoredTurnSurface::setUpcomingTurnDock(UpcomingTurnDock* widget)
{
    if (dock == widget)
        return;
    if (dock)
        dock->removeEventFilter(this);
    dock = widget;
    if (dock) {
        dock->setParent(this);
        dock->installEventFilter(this);
        dock->show();
        dock->raise();
        connect(dock, &UpcomingTurnDock::dockHeightChanged, this, [this] { relayout(); });
    }
    relayout();
}

bool AnchoredTurnSurface::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == dock || watched == conversation)
        && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize
            || event->type() == QEvent::Show))
        relayout();
    return QWidget::eventFilter(watched, event);
}

void AnchoredTurnSurface::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
}

void AnchoredTurnSurface::relayout()
{
    const int base = dock ? dock->baseHeight() : 0;
    if (conversation)
        conversation->setGeometry(0, 0, width(), std::max(0, height() - base));
    if (dock) {
        const int dockHeight = std::min(height(), std::max(base, dock->height()));
        dock->setGeometry(0, height() - dockHeight, width(), dockHeight);
        dock->raise();
    }
}

} // namespace codexui
