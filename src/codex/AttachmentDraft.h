// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_ATTACHMENTDRAFT_H
#define CODEXUI_CODEX_ATTACHMENTDRAFT_H

#include <cstdint>
#include <string>

namespace codexui::codex {

// Renderer-neutral description of a locally selected attachment. Paths and
// names are UTF-8; selecting and opening files remains the renderer's job.
struct AttachmentDraft {
  std::string path;
  std::string name;
  std::string mimeType;
  std::int64_t size = 0;

  bool operator==(const AttachmentDraft &) const = default;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_ATTACHMENTDRAFT_H
