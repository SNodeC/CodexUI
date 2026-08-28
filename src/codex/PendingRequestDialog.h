// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_PENDINGREQUESTDIALOG_H
#define CODEXUI_CODEX_PENDINGREQUESTDIALOG_H

#include "codex/PresentationModel.h"

#include <nlohmann/json.hpp>

#include <optional>

class QWidget;

namespace codexui::codex {

struct PendingRequestResponse {
  nlohmann::json result = nlohmann::json::object();
  nlohmann::json error = nullptr;
};

class PendingRequestDialog final {
public:
  [[nodiscard]] static std::optional<PendingRequestResponse>
  present(const PendingRequestPresentation &request, QWidget *parent);

  [[nodiscard]] static PendingRequestResponse
  negativeResponse(const PendingRequestPresentation &request);

  [[nodiscard]] static PendingRequestResponse
  positiveResponse(const PendingRequestPresentation &request);
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_PENDINGREQUESTDIALOG_H
