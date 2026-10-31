// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_ATTACHMENTDRAFT_H
#define CODEXUI_CODEX_ATTACHMENTDRAFT_H

#include <cstdint>
#include <string>

namespace codexui::codex {

    // Admitted paths belong to the app-server. Local input carries temporary
    // upload data until preparation completes; it never enters a prompt.
    struct AttachmentDraft {
        std::string path;
        std::string name;
        std::string mimeType;
        std::int64_t size = 0;

        std::string dataBase64{};
        std::string localPath{};
        std::uint64_t order = 0;

        bool operator==(const AttachmentDraft&) const = default;
    };

} // namespace codexui::codex

#endif // CODEXUI_CODEX_ATTACHMENTDRAFT_H
