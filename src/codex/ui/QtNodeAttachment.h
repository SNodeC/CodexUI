// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_UI_QTNODEATTACHMENT_H
#define CODEXUI_CODEX_UI_QTNODEATTACHMENT_H

#include <QPointer>

#include <cstdint>

class QWidget;

namespace codexui::codex::ui {

// Qt-main owns this opaque record and stores its address in Node's single UI
// attachment slot.  The shared node graph deliberately knows nothing about
// QWidget or about this type.
enum class NodeMaterialization : std::uint8_t {
  Placeholder,
  Overscan,
  ViewportVisible,
};

struct QtNodeAttachment final {
  QPointer<QWidget> widget;
  std::uint64_t renderedRevision = 0;
  NodeMaterialization materialization = NodeMaterialization::Placeholder;
  bool viewportVisible = false;
};

} // namespace codexui::codex::ui

#endif // CODEXUI_CODEX_UI_QTNODEATTACHMENT_H
