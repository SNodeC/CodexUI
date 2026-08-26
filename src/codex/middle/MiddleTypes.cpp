// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/MiddleTypes.h"

#include <string_view>

namespace codexui::codex::middle {
namespace {

void appendComponent(std::string &result, std::string_view value) {
  result += std::to_string(value.size());
  result.push_back(':');
  result.append(value);
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
  return "prompt:" + std::to_string(std::get<LocalPromptKey>(key).submissionId);
}

bool terminalOutputHasVisibleText(QStringView output) {
  for (qsizetype index = 0; index < output.size(); ++index) {
    const ushort code = output[index].unicode();
    if (code == 0x9b) {
      while (++index < output.size()) {
        const ushort candidate = output[index].unicode();
        if (candidate >= 0x40 && candidate <= 0x7e)
          break;
      }
      continue;
    }
    if (code == 0x90 || code == 0x98 || code == 0x9d || code == 0x9e ||
        code == 0x9f) {
      while (++index < output.size()) {
        const ushort candidate = output[index].unicode();
        if (candidate == 0x07 || candidate == 0x9c)
          break;
        if (candidate == 0x1b && index + 1 < output.size() &&
            output[index + 1].unicode() == '\\') {
          ++index;
          break;
        }
      }
      continue;
    }
    if (code == 0x1b) {
      if (++index >= output.size())
        break;
      const ushort introducer = output[index].unicode();
      if (introducer == '[') {
        while (++index < output.size()) {
          const ushort candidate = output[index].unicode();
          if (candidate >= 0x40 && candidate <= 0x7e)
            break;
        }
        continue;
      }
      if (introducer == ']' || introducer == 'P' || introducer == '^' ||
          introducer == '_' || introducer == 'X') {
        while (++index < output.size()) {
          if (output[index].unicode() == 0x07 ||
              output[index].unicode() == 0x9c)
            break;
          if (output[index].unicode() == 0x1b && index + 1 < output.size() &&
              output[index + 1].unicode() == '\\') {
            ++index;
            break;
          }
        }
        continue;
      }
      if (introducer >= 0x20 && introducer <= 0x2f) {
        while (++index < output.size()) {
          const ushort candidate = output[index].unicode();
          if (candidate >= 0x30 && candidate <= 0x7e)
            break;
        }
      }
      continue;
    }
    if (output[index].isPrint() && !output[index].isSpace())
      return true;
  }
  return false;
}

QString trimTrailingEmptyLines(QStringView text) {
  qsizetype end = text.size();
  while (end > 0) {
    while (end > 0 && (text[end - 1] == QLatin1Char('\n') ||
                       text[end - 1] == QLatin1Char('\r')))
      --end;
    if (end == 0)
      break;

    qsizetype lineStart = end;
    while (lineStart > 0 && text[lineStart - 1] != QLatin1Char('\n') &&
           text[lineStart - 1] != QLatin1Char('\r'))
      --lineStart;
    bool emptyLine = true;
    for (qsizetype index = lineStart; index < end; ++index) {
      if (!text[index].isSpace()) {
        emptyLine = false;
        break;
      }
    }
    if (!emptyLine)
      break;
    end = lineStart;
  }
  return text.first(end).toString();
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
