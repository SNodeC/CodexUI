// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_ATTACHMENTINPUT_H
#define CODEXUI_CODEX_ATTACHMENTINPUT_H

#include "codex/AttachmentDraft.h"
#include <QStringList>
#include <vector>

class QMimeData;

namespace codexui::codex {

inline constexpr std::size_t MaximumAttachments = 16;

bool isAttachmentInput(const QMimeData &source);
// Atomic admission: on error the existing draft is unchanged.
QString appendAttachmentFiles(std::vector<AttachmentDraft> &draft,
                              const QStringList &paths);
QString appendAttachmentInput(std::vector<AttachmentDraft> &draft,
                              const QMimeData &source);

} // namespace codexui::codex
#endif
