// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_FORKNAMING_H
#define CODEXUI_CODEX_FORKNAMING_H

#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace codexui::codex {
namespace detail {

struct ForkNameParts final {
  std::string base;
  std::vector<std::uint64_t> lineage;
};

inline ForkNameParts parseForkName(std::string_view title) {
  constexpr std::string_view Marker = " (fork ";
  if (!title.ends_with(')'))
    return {std::string(title), {}};
  const std::size_t marker = title.rfind(Marker);
  if (marker == std::string_view::npos || marker == 0)
    return {std::string(title), {}};

  std::vector<std::uint64_t> lineage;
  std::string_view remaining = title.substr(
      marker + Marker.size(), title.size() - marker - Marker.size() - 1);
  while (!remaining.empty()) {
    const std::size_t separator = remaining.find('.');
    const std::string_view component = remaining.substr(0, separator);
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(
        component.data(), component.data() + component.size(), number);
    if (component.empty() || component.front() == '0' || error != std::errc{} ||
        end != component.data() + component.size() || number == 0)
      return {std::string(title), {}};
    lineage.push_back(number);
    if (separator == std::string_view::npos)
      break;
    remaining.remove_prefix(separator + 1);
  }
  return {std::string(title.substr(0, marker)), std::move(lineage)};
}

} // namespace detail

inline std::string
suggestForkName(std::string_view sourceTitle,
                std::span<const std::string> existingThreadTitles) {
  detail::ForkNameParts source = detail::parseForkName(sourceTitle);
  if (source.base.empty())
    source.base = "Thread";

  std::unordered_set<std::uint64_t> directChildren;
  for (const std::string &title : existingThreadTitles) {
    const detail::ForkNameParts candidate = detail::parseForkName(title);
    if (candidate.base != source.base ||
        candidate.lineage.size() != source.lineage.size() + 1)
      continue;
    bool hasParentLineage = true;
    for (std::size_t index = 0; index < source.lineage.size(); ++index) {
      if (candidate.lineage[index] != source.lineage[index]) {
        hasParentLineage = false;
        break;
      }
    }
    if (hasParentLineage)
      directChildren.insert(candidate.lineage.back());
  }

  std::uint64_t next = 1;
  while (directChildren.contains(next))
    ++next;
  source.lineage.push_back(next);

  std::string result = source.base + " (fork ";
  for (std::size_t index = 0; index < source.lineage.size(); ++index) {
    if (index != 0)
      result.push_back('.');
    result += std::to_string(source.lineage[index]);
  }
  result.push_back(')');
  return result;
}

} // namespace codexui::codex

#endif // CODEXUI_CODEX_FORKNAMING_H
