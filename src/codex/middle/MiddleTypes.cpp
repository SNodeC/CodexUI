// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/MiddleTypes.h"

#include <cstdint>
#include <string_view>

namespace codexui::codex::middle {
namespace {

void appendComponent(std::string &result, std::string_view value) {
  result += std::to_string(value.size());
  result.push_back(':');
  result.append(value);
}

struct Utf8CodePoint {
  char32_t value = 0;
  std::size_t next = 0;
};

Utf8CodePoint decodeUtf8(std::string_view value, std::size_t offset) noexcept {
  const auto byte = [&value](std::size_t index) {
    return static_cast<unsigned char>(value[index]);
  };
  const unsigned char first = byte(offset);
  if (first < 0x80)
    return {first, offset + 1};

  std::size_t length = 0;
  char32_t codePoint = 0;
  char32_t minimum = 0;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    codePoint = first & 0x1f;
    minimum = 0x80;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    codePoint = first & 0x0f;
    minimum = 0x800;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    codePoint = first & 0x07;
    minimum = 0x10000;
  } else {
    return {0xfffd, offset + 1};
  }
  if (offset + length > value.size())
    return {0xfffd, offset + 1};
  for (std::size_t index = 1; index < length; ++index) {
    const unsigned char continuation = byte(offset + index);
    if ((continuation & 0xc0) != 0x80)
      return {0xfffd, offset + 1};
    codePoint = (codePoint << 6) | (continuation & 0x3f);
  }
  if (codePoint < minimum || codePoint > 0x10ffff ||
      (codePoint >= 0xd800 && codePoint <= 0xdfff))
    return {0xfffd, offset + 1};
  return {codePoint, offset + length};
}

bool unicodeSpace(char32_t value) noexcept {
  return (value >= 0x09 && value <= 0x0d) || (value >= 0x1c && value <= 0x20) ||
         value == 0x85 || value == 0xa0 || value == 0x1680 ||
         (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
         value == 0x2029 || value == 0x202f || value == 0x205f ||
         value == 0x3000;
}

bool printableNonSpace(char32_t value) noexcept {
  if (unicodeSpace(value) || value < 0x20 || (value >= 0x7f && value <= 0x9f))
    return false;
  // Unicode format controls are not visibly printable even though they may
  // influence adjacent text.
  if ((value >= 0x200b && value <= 0x200f) ||
      (value >= 0x202a && value <= 0x202e) ||
      (value >= 0x2060 && value <= 0x206f) || value == 0xfeff)
    return false;
  return true;
}

bool whitespaceOnly(std::string_view value) noexcept {
  for (std::size_t offset = 0; offset < value.size();) {
    const Utf8CodePoint decoded = decodeUtf8(value, offset);
    if (!unicodeSpace(decoded.value))
      return false;
    offset = decoded.next;
  }
  return true;
}

} // namespace

std::string stableKey(const CardKey &key) {
  if (const auto *authoritative = std::get_if<AuthoritativeItemKey>(&key)) {
    std::string result = "item:";
    appendComponent(result, authoritative->threadId);
    appendComponent(result, authoritative->turnId);
    appendComponent(result, authoritative->itemId);
    return result;
  }
  if (const auto *plan = std::get_if<TurnPlanKey>(&key)) {
    std::string result = "plan:";
    appendComponent(result, plan->threadId);
    appendComponent(result, plan->turnId);
    return result;
  }
  return "prompt:" + std::to_string(std::get<LocalPromptKey>(key).submissionId);
}

bool terminalOutputHasVisibleText(std::string_view output) {
  for (std::size_t index = 0; index < output.size();) {
    const Utf8CodePoint current = decodeUtf8(output, index);
    const char32_t code = current.value;
    index = current.next;
    if (code == 0x9b) {
      while (index < output.size()) {
        const Utf8CodePoint candidate = decodeUtf8(output, index);
        index = candidate.next;
        if (candidate.value >= 0x40 && candidate.value <= 0x7e)
          break;
      }
      continue;
    }
    if (code == 0x90 || code == 0x98 || code == 0x9d || code == 0x9e ||
        code == 0x9f) {
      while (index < output.size()) {
        const Utf8CodePoint candidate = decodeUtf8(output, index);
        index = candidate.next;
        if (candidate.value == 0x07 || candidate.value == 0x9c)
          break;
        if (candidate.value == 0x1b && index < output.size()) {
          const Utf8CodePoint terminator = decodeUtf8(output, index);
          if (terminator.value != '\\')
            continue;
          index = terminator.next;
          break;
        }
      }
      continue;
    }
    if (code == 0x1b) {
      if (index >= output.size())
        break;
      const Utf8CodePoint introduced = decodeUtf8(output, index);
      const char32_t introducer = introduced.value;
      index = introduced.next;
      if (introducer == '[') {
        while (index < output.size()) {
          const Utf8CodePoint candidate = decodeUtf8(output, index);
          index = candidate.next;
          if (candidate.value >= 0x40 && candidate.value <= 0x7e)
            break;
        }
        continue;
      }
      if (introducer == ']' || introducer == 'P' || introducer == '^' ||
          introducer == '_' || introducer == 'X') {
        while (index < output.size()) {
          const Utf8CodePoint candidate = decodeUtf8(output, index);
          index = candidate.next;
          if (candidate.value == 0x07 || candidate.value == 0x9c)
            break;
          if (candidate.value == 0x1b && index < output.size()) {
            const Utf8CodePoint terminator = decodeUtf8(output, index);
            if (terminator.value != '\\')
              continue;
            index = terminator.next;
            break;
          }
        }
        continue;
      }
      if (introducer >= 0x20 && introducer <= 0x2f) {
        while (index < output.size()) {
          const Utf8CodePoint candidate = decodeUtf8(output, index);
          index = candidate.next;
          if (candidate.value >= 0x30 && candidate.value <= 0x7e)
            break;
        }
      }
      continue;
    }
    if (printableNonSpace(code))
      return true;
  }
  return false;
}

std::string trimUnicodeWhitespace(std::string_view text) {
  std::size_t first = 0;
  std::size_t last = 0;
  bool found = false;
  for (std::size_t offset = 0; offset < text.size();) {
    const std::size_t start = offset;
    const Utf8CodePoint decoded = decodeUtf8(text, offset);
    offset = decoded.next;
    if (unicodeSpace(decoded.value))
      continue;
    if (!found) {
      first = start;
      found = true;
    }
    last = offset;
  }
  return found ? std::string(text.substr(first, last - first)) : std::string{};
}

std::string trimTrailingEmptyLines(std::string_view text) {
  std::size_t end = text.size();
  while (end > 0) {
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r'))
      --end;
    if (end == 0)
      break;

    std::size_t lineStart = end;
    while (lineStart > 0 && text[lineStart - 1] != '\n' &&
           text[lineStart - 1] != '\r')
      --lineStart;
    if (!whitespaceOnly(text.substr(lineStart, end - lineStart)))
      break;
    end = lineStart;
  }
  return std::string(text.substr(0, end));
}

std::vector<CardKey> ConversationSnapshot::cardKeys() const {
  std::vector<CardKey> result;
  for (const TurnSection &section : sections)
    for (const VisibleCardData &card : section.cards)
      result.push_back(card.key);
  return result;
}

const VisibleCardData *
ConversationSnapshot::find(const CardKey &key) const noexcept {
  for (const TurnSection &section : sections)
    for (const VisibleCardData &card : section.cards)
      if (card.key == key)
        return &card;
  return nullptr;
}

} // namespace codexui::codex::middle
