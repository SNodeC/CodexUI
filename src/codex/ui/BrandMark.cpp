// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/BrandMark.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>

namespace codexui {
namespace {

void paintMark(QPainter &painter, const QRectF &bounds) {
  const qreal scale = std::min(bounds.width(), bounds.height()) / 36.0;
  painter.save();
  painter.translate(bounds.center().x() - 18.0 * scale,
                    bounds.center().y() - 18.0 * scale);
  painter.scale(scale, scale);

  const QRectF surface(1.0, 1.0, 34.0, 34.0);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(QStringLiteral("#2f6feb")));
  painter.drawRoundedRect(surface, 9.0, 9.0);

  painter.setPen(QPen(QColor(QStringLiteral("#ffffff")), 2.7, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);

  QPainterPath leftBracket;
  leftBracket.moveTo(15.0, 10.5);
  leftBracket.lineTo(9.5, 18.0);
  leftBracket.lineTo(15.0, 25.5);
  painter.drawPath(leftBracket);

  QPainterPath rightBracket;
  rightBracket.moveTo(21.0, 10.5);
  rightBracket.lineTo(26.5, 18.0);
  rightBracket.lineTo(21.0, 25.5);
  painter.drawPath(rightBracket);

  painter.setPen(QPen(QColor(QStringLiteral("#63d5a5")), 3.2, Qt::SolidLine,
                      Qt::RoundCap));
  painter.drawLine(QPointF(15.5, 18.0), QPointF(20.5, 18.0));

  painter.restore();
}

} // namespace

BrandMark::BrandMark(QWidget *parent) : QWidget(parent) {
  setFixedSize(36, 36);
  setAccessibleName(QStringLiteral("CodexUI logo"));
  setToolTip(QStringLiteral("CodexUI"));
}

QIcon BrandMark::icon() {
  QIcon icon;
  for (const int size : {16, 24, 32, 48, 64, 128}) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintMark(painter, QRectF(0.0, 0.0, size, size));
    icon.addPixmap(pixmap);
  }
  return icon;
}

void BrandMark::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event)

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  paintMark(painter, rect());
}

} // namespace codexui
