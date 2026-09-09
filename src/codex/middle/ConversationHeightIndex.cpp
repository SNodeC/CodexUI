// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationHeightIndex.h"

#include <algorithm>
#include <iterator>

namespace codexui::codex::middle {
namespace {

int validHeight(int height) noexcept { return std::max(0, height); }

} // namespace

void ConversationHeightIndex::clear() noexcept {
  heights_.clear();
  tree_.assign(1, 0);
  lastLookupSteps_ = 0;
  lastUpdateSteps_ = 0;
}

void ConversationHeightIndex::reset(std::size_t count, int estimatedHeight) {
  heights_.assign(count, validHeight(estimatedHeight));
  rebuild();
}

void ConversationHeightIndex::assign(std::span<const int> heights) {
  heights_.clear();
  heights_.reserve(heights.size());
  for (int height : heights)
    heights_.push_back(validHeight(height));
  rebuild();
}

void ConversationHeightIndex::insert(std::size_t row,
                                     std::span<const int> heights) {
  row = std::min(row, heights_.size());
  if (heights.empty())
    return;
  if (row == heights_.size()) {
    heights_.reserve(heights_.size() + heights.size());
    tree_.reserve(tree_.size() + heights.size());
    for (int height : heights)
      append(validHeight(height));
    return;
  }
  std::vector<int> inserted;
  inserted.reserve(heights.size());
  for (int height : heights)
    inserted.push_back(validHeight(height));
  heights_.insert(heights_.begin() + static_cast<std::ptrdiff_t>(row),
                  inserted.begin(), inserted.end());
  rebuild();
}

void ConversationHeightIndex::remove(std::size_t row, std::size_t count) {
  if (row >= heights_.size() || count == 0)
    return;
  count = std::min(count, heights_.size() - row);
  if (row + count == heights_.size()) {
    heights_.resize(row);
    tree_.resize(row + 1);
    lastUpdateSteps_ = 0;
    return;
  }
  heights_.erase(heights_.begin() + static_cast<std::ptrdiff_t>(row),
                 heights_.begin() + static_cast<std::ptrdiff_t>(row + count));
  rebuild();
}

void ConversationHeightIndex::move(std::size_t sourceRow, std::size_t count,
                                   std::size_t destinationRow) {
  if (sourceRow >= heights_.size() || count == 0)
    return;
  count = std::min(count, heights_.size() - sourceRow);
  destinationRow = std::min(destinationRow, heights_.size() - count);
  if (sourceRow == destinationRow)
    return;
  std::vector<int> moved(
      heights_.begin() + static_cast<std::ptrdiff_t>(sourceRow),
      heights_.begin() + static_cast<std::ptrdiff_t>(sourceRow + count));
  heights_.erase(heights_.begin() + static_cast<std::ptrdiff_t>(sourceRow),
                 heights_.begin() +
                     static_cast<std::ptrdiff_t>(sourceRow + count));
  heights_.insert(heights_.begin() +
                      static_cast<std::ptrdiff_t>(destinationRow),
                  std::make_move_iterator(moved.begin()),
                  std::make_move_iterator(moved.end()));
  rebuild();
}

int ConversationHeightIndex::height(std::size_t row) const noexcept {
  return row < heights_.size() ? heights_[row] : 0;
}

bool ConversationHeightIndex::setHeight(std::size_t row,
                                        int nextHeight) noexcept {
  if (row >= heights_.size())
    return false;
  nextHeight = validHeight(nextHeight);
  const qint64 delta = static_cast<qint64>(nextHeight) - heights_[row];
  if (delta == 0) {
    lastUpdateSteps_ = 0;
    return false;
  }
  heights_[row] = nextHeight;
  lastUpdateSteps_ = 0;
  for (std::size_t index = row + 1; index < tree_.size();
       index += index & (~index + 1)) {
    tree_[index] += delta;
    ++lastUpdateSteps_;
  }
  return true;
}

qint64 ConversationHeightIndex::top(std::size_t row) const noexcept {
  return prefix(std::min(row, heights_.size()));
}

qint64 ConversationHeightIndex::bottom(std::size_t row) const noexcept {
  return row < heights_.size() ? prefix(row + 1) : totalHeight();
}

qint64 ConversationHeightIndex::totalHeight() const noexcept {
  return prefix(heights_.size());
}

std::size_t ConversationHeightIndex::rowAt(qint64 contentY) const noexcept {
  lastLookupSteps_ = 0;
  if (heights_.empty())
    return 0;
  const qint64 total = totalHeight();
  if (total <= 0)
    return 0;
  contentY = std::clamp<qint64>(contentY, 0, total - 1);

  std::size_t bit = 1;
  while ((bit << 1) < tree_.size())
    bit <<= 1;
  std::size_t index = 0;
  qint64 sum = 0;
  for (; bit != 0; bit >>= 1) {
    ++lastLookupSteps_;
    const std::size_t next = index + bit;
    if (next < tree_.size() && sum + tree_[next] <= contentY) {
      index = next;
      sum += tree_[next];
    }
  }
  return std::min(index, heights_.size() - 1);
}

qint64 ConversationHeightIndex::prefix(std::size_t count) const noexcept {
  count = std::min(count, heights_.size());
  qint64 result = 0;
  for (std::size_t index = count; index != 0; index -= index & (~index + 1))
    result += tree_[index];
  return result;
}

void ConversationHeightIndex::append(int height) {
  const std::size_t oldCount = heights_.size();
  const std::size_t index = oldCount + 1;
  const std::size_t lowBit = index & (~index + 1);
  const qint64 preceding =
      prefix(oldCount) - prefix(index > lowBit ? index - lowBit : 0);
  heights_.push_back(height);
  tree_.push_back(preceding + height);
  lastUpdateSteps_ = 1;
}

void ConversationHeightIndex::rebuild() {
  tree_.assign(heights_.size() + 1, 0);
  for (std::size_t index = 1; index < tree_.size(); ++index) {
    tree_[index] += heights_[index - 1];
    const std::size_t parent = index + (index & (~index + 1));
    if (parent < tree_.size())
      tree_[parent] += tree_[index];
  }
  ++rebuildCount_;
  lastLookupSteps_ = 0;
  lastUpdateSteps_ = 0;
}

} // namespace codexui::codex::middle
