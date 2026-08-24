// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ui/BrandMark.h"

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QSizePolicy>

#include <algorithm>

namespace codexui {
namespace {

constexpr int BrandMarkSize = 36;

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

class BrandLockup final : public QWidget {
public:
  explicit BrandLockup(QWidget *parent = nullptr) : QWidget(parent) {
    mark = new BrandMark(this);

    title = new QLabel(QStringLiteral("CodexUI"), this);
    title->setObjectName(QStringLiteral("codexBrandTitle"));
    title->setProperty("kind", "applicationTitle");
    title->setWordWrap(false);
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::Bold);
    int titlePixelSize = BrandMarkSize;
    for (int pixelSize = BrandMarkSize; pixelSize > 0; --pixelSize) {
      titleFont.setPixelSize(pixelSize);
      if (QFontMetrics(titleFont).height() <= BrandMarkSize) {
        titlePixelSize = pixelSize;
        break;
      }
    }
    title->setStyleSheet(
        QStringLiteral("font-size:%1px;font-weight:700;").arg(titlePixelSize));

    subtitle = new QLabel(QStringLiteral("Codex agent workspace"), this);
    subtitle->setObjectName(QStringLiteral("codexBrandSubtitle"));
    subtitle->setProperty("kind", "meta");
    subtitle->setWordWrap(false);

    setAccessibleName(QStringLiteral("CodexUI, Codex agent workspace"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(BrandMarkSize);
  }

  QSize sizeHint() const override {
    return {BrandMarkSize + 12 + title->sizeHint().width() + 12 +
                subtitle->sizeHint().width(),
            BrandMarkSize};
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    layoutChildren();
  }

  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange) {
      updateGeometry();
      layoutChildren();
    }
  }

private:
  void layoutChildren() {
    mark->setGeometry(0, 0, BrandMarkSize, BrandMarkSize);
    int x = BrandMarkSize + 12;
    const int titleWidth = title->sizeHint().width();
    title->setGeometry(x, 0, titleWidth, BrandMarkSize);
    x += titleWidth + 12;

    const QFontMetrics titleMetrics(title->font());
    const QFontMetrics subtitleMetrics(subtitle->font());
    const int titleBaseline =
        (BrandMarkSize - titleMetrics.height()) / 2 + titleMetrics.ascent();
    const int subtitleY =
        std::clamp(titleBaseline - subtitleMetrics.ascent(), 0,
                   BrandMarkSize - subtitleMetrics.height());
    subtitle->setGeometry(x, subtitleY, subtitle->sizeHint().width(),
                          subtitleMetrics.height());
  }

  BrandMark *mark = nullptr;
  QLabel *title = nullptr;
  QLabel *subtitle = nullptr;
};

} // namespace

BrandMark::BrandMark(QWidget *parent) : QWidget(parent) {
  setFixedSize(BrandMarkSize, BrandMarkSize);
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

QWidget *BrandMark::createLockup(QWidget *parent) {
  return new BrandLockup(parent);
}

void BrandMark::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event)

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  paintMark(painter, rect());
}

} // namespace codexui
