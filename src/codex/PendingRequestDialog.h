// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PENDINGREQUESTDIALOG_H
#define CODEXUI_CODEX_PENDINGREQUESTDIALOG_H

#include "codex/PendingRequestPolicy.h"

#include <optional>

class QWidget;

namespace codexui::codex {

class PendingRequestDialog final {
public:
  [[nodiscard]] static std::optional<PendingRequestResponse>
  present(const PendingRequestDescriptor &request, QWidget *parent);
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PENDINGREQUESTDIALOG_H
