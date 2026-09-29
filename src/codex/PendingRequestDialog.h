// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PENDINGREQUESTDIALOG_H
#define CODEXUI_CODEX_PENDINGREQUESTDIALOG_H

#include "codex/PendingRequestPolicy.h"

#include <functional>
#include <optional>

class QWidget;
class QPushButton;

namespace codexui::codex {

class PendingRequestDialog final {
public:
  [[nodiscard]] static std::optional<PendingRequestSubmission>
  present(const PendingRequestDescriptor &request, QWidget *parent,
          const PendingRequestSubmission *initialSubmission = nullptr,
          const std::function<void(QPushButton *)> &observeSubmit = {});
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PENDINGREQUESTDIALOG_H
