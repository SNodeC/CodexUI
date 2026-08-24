// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_UI_BRANDMARK_H
#define CODEXUI_UI_BRANDMARK_H

#include <QIcon>
#include <QWidget>

namespace codexui {

class BrandMark final : public QWidget {
public:
  explicit BrandMark(QWidget *parent = nullptr);
  [[nodiscard]] static QIcon icon();

protected:
  void paintEvent(QPaintEvent *event) override;
};

} // namespace codexui

#endif
