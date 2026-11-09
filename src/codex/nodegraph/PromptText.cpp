// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/PromptText.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <vector>

namespace codexui::nodegraph {
namespace {

    std::string markdownLabel(std::string_view label, std::string_view escapedCharacters = "\\[]") {
        std::string escaped;
        escaped.reserve(label.size());
        for (const char character : label) {
            if (escapedCharacters.find(character) != std::string_view::npos) {
                escaped.push_back('\\');
            }
            escaped.push_back(character == '\r' || character == '\n' ? ' ' : character);
        }
        return escaped;
    }

bool fileUrlByteAllowed(unsigned char byte) noexcept {
  const bool alphanumeric = (byte >= 'a' && byte <= 'z') ||
                            (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9');
  return alphanumeric || byte == '-' || byte == '.' || byte == '_' ||
         byte == '~' || byte == '/' || byte == ':' || byte == '@' ||
         byte == '!' || byte == '$' || byte == '&' || byte == '\'' ||
         byte == '*' || byte == '+' || byte == ',' || byte == ';' ||
         byte == '=';
}

std::string localFileUrl(std::string_view path) {
  static constexpr char Hex[] = "0123456789ABCDEF";
  std::string result = path.starts_with('/') ? "file://" : "file:";
  for (const unsigned char byte : path) {
    if (fileUrlByteAllowed(byte)) {
      result.push_back(static_cast<char>(byte));
      continue;
    }
    result.push_back('%');
    result.push_back(Hex[byte >> 4]);
    result.push_back(Hex[byte & 0x0f]);
  }
  return result;
}

} // namespace

std::string messageText(const nodegraph::NodeState& state) {
    std::string result;
    if (const auto* content = valueMember(state, "content"); content && content->asArray()) {
        for (const nodegraph::Value& entry : *content->asArray()) {
            const auto* object = entry.asObject();
            const std::string text = object ? scalarTextFromValue(valueMember(*object, "text")) : std::string{};
            if (!text.empty()) {
                result += (result.empty() ? "" : "\n") + text;
            }
        }
    }
    return result.empty() ? scalarTextFromValue(valueMember(state, "text")) : result;
}

std::string questionReplyKey(std::string_view item, std::size_t index) {
    return nlohmann::json::array({"request_user_input_async", item, index}).dump();
}

std::vector<QuestionReply> questionReplies(std::string_view source) {
    std::vector<QuestionReply> result;
    const auto first = source.find_first_not_of(" \t\r\n"), last = source.find_last_not_of(" \t\r\n");
    source = first == std::string_view::npos ? std::string_view{} : source.substr(first, last - first + 1);
    constexpr std::string_view Open = "<send_user_message_question_reply>", Close = "</send_user_message_question_reply>";
    if (source.starts_with(Open) && source.ends_with(Close)) {
        auto data = nlohmann::json::parse(source.substr(Open.size(), source.size() - Open.size() - Close.size()), nullptr, false);
        if (data.is_object()) {
            data = nlohmann::json::array({std::move(data)});
        }
        bool valid = data.is_array() && !data.empty() && data.size() <= 64;
        for (std::size_t index = 0; valid && index < data.size(); ++index) {
            const auto& entry = data[index];
            valid = entry.is_object() && entry.contains("questionItemId") && entry["questionItemId"].is_string() &&
                    entry.contains("question") && entry["question"].is_string() && entry.contains("answer") && entry["answer"].is_string();
            if (valid) {
                result.push_back({entry["questionItemId"], entry["question"], entry["answer"]});
            }
        }
        if (!valid) {
            result.clear();
        }
    }
    return result;
}

std::string questionReplyMarkdown(std::span<const QuestionReply> replies) {
    std::string result;
    for (const auto& reply : replies) {
        std::string answer;
        std::istringstream lines(reply.answer);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            answer += (line.empty() ? "" : "**" + markdownLabel(line, "\\`*_[]<>&") + "**") + "\n";
        }
        if (!answer.empty() && !reply.answer.ends_with('\n')) {
            answer.pop_back();
        }
        result += (result.empty() ? "" : "\n\n") + reply.question + (answer.empty() ? "" : "\n\n" + answer);
    }
    return result;
}

std::string composePromptMarkdown(std::string prompt,
                                  std::span<const Attachment> attachments) {
  std::vector<std::string> links;
  links.reserve(attachments.size());
  for (const Attachment &attachment : attachments) {
    if (attachment.mimeType.starts_with("image/") ||
        attachment.mimeType.starts_with("audio/"))
      continue;
    const std::string &label = attachment.displayName.empty()
                                   ? attachment.path
                                   : attachment.displayName;
    links.emplace_back("- [" + markdownLabel(label) + "](" +
                       localFileUrl(attachment.path) + ')');
  }
  if (links.empty())
    return prompt;
  prompt += "\n\nAttached files:\n";
  for (std::size_t index = 0; index < links.size(); ++index) {
    if (index != 0)
      prompt.push_back('\n');
    prompt += links[index];
  }
  return prompt;
}

} // namespace codexui::nodegraph
