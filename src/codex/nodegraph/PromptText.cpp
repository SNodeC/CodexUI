// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/PromptText.h"

#include <string_view>
#include <vector>

namespace codexui::nodegraph {
namespace {

std::string markdownLabel(std::string_view label) {
  std::string escaped;
  escaped.reserve(label.size());
  for (const char character : label) {
    if (character == '\\' || character == '[' || character == ']')
      escaped.push_back('\\');
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
