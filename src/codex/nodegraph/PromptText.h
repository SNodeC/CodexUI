// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_PROMPTTEXT_H
#define CODEXUI_CODEX_NODEGRAPH_PROMPTTEXT_H

#include "codex/nodegraph/Messages.h"

#include <span>
#include <string>
#include <string_view>

namespace codexui::nodegraph {

    struct QuestionReply {
        std::string questionItemId, question, answer;
    };
    [[nodiscard]] std::string questionReplyKey(std::string_view item, std::size_t index);
    [[nodiscard]] std::vector<QuestionReply> questionReplies(std::string_view source);
    [[nodiscard]] std::string questionReplyMarkdown(std::span<const QuestionReply> replies);
    [[nodiscard]] std::string messageText(const NodeState& state);

    // Produces the one text value used by both the local pending card and the
    // app-server input. Binary image/audio attachments remain separate input
    // items; ordinary files are safe local-file Markdown links.
    [[nodiscard]] std::string composePromptMarkdown(std::string prompt, std::span<const Attachment> attachments);

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_PROMPTTEXT_H
