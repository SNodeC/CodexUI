// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationHeightIndex.h"

#include <algorithm>

namespace codexui::codex::middle {
namespace {

int validHeight(int height) noexcept { return std::max(0, height); }

} // namespace

ConversationHeightIndex::~ConversationHeightIndex() = default;

void ConversationHeightIndex::assign(std::span<const int> heights) {
  lastUpdateSteps_ = 0;
  root_ = makeRows(heights, lastUpdateSteps_);
  ++rebuildCount_;
  lastLookupSteps_ = 0;
  lastUpdateSteps_ = 0;
}

void ConversationHeightIndex::insert(std::size_t row,
                                     std::span<const int> heights) {
  if (heights.empty())
    return;
  row = std::min(row, size());
  lastUpdateSteps_ = 0;
  auto [left, right] = split(std::move(root_), row, lastUpdateSteps_);
  std::unique_ptr<Node> inserted = makeRows(heights, lastUpdateSteps_);
  root_ = merge(merge(std::move(left), std::move(inserted), lastUpdateSteps_),
                std::move(right), lastUpdateSteps_);
}

void ConversationHeightIndex::remove(std::size_t row, std::size_t count) {
  if (row >= size() || count == 0)
    return;
  count = std::min(count, size() - row);
  lastUpdateSteps_ = 0;
  auto [left, suffix] = split(std::move(root_), row, lastUpdateSteps_);
  auto [removed, right] =
      split(std::move(suffix), count, lastUpdateSteps_);
  root_ = merge(std::move(left), std::move(right), lastUpdateSteps_);
}

void ConversationHeightIndex::move(std::size_t sourceRow, std::size_t count,
                                   std::size_t destinationRow) {
  if (sourceRow >= size() || count == 0)
    return;
  count = std::min(count, size() - sourceRow);
  destinationRow = std::min(destinationRow, size() - count);
  if (sourceRow == destinationRow)
    return;

  lastUpdateSteps_ = 0;
  auto [left, suffix] =
      split(std::move(root_), sourceRow, lastUpdateSteps_);
  auto [moved, right] =
      split(std::move(suffix), count, lastUpdateSteps_);
  root_ = merge(std::move(left), std::move(right), lastUpdateSteps_);
  auto [before, after] =
      split(std::move(root_), destinationRow, lastUpdateSteps_);
  root_ = merge(merge(std::move(before), std::move(moved), lastUpdateSteps_),
                std::move(after), lastUpdateSteps_);
}

std::size_t ConversationHeightIndex::size() const noexcept {
  return nodeCount(root_);
}

int ConversationHeightIndex::height(std::size_t row) const noexcept {
  const Node *current = root_.get();
  while (current) {
    const std::size_t leftCount = nodeCount(current->left);
    if (row < leftCount) {
      current = current->left.get();
      continue;
    }
    if (row == leftCount)
      return current->height;
    row -= leftCount + 1;
    current = current->right.get();
  }
  return 0;
}

bool ConversationHeightIndex::setHeight(std::size_t row,
                                        int nextHeight) noexcept {
  if (row >= size())
    return false;
  nextHeight = validHeight(nextHeight);
  const qint64 delta = static_cast<qint64>(nextHeight) - height(row);
  lastUpdateSteps_ = 0;
  if (delta == 0)
    return false;
  for (Node *current = root_.get(); current;) {
    ++lastUpdateSteps_;
    current->total += delta;
    const std::size_t leftCount = nodeCount(current->left);
    if (row == leftCount) {
      current->height = nextHeight;
      return true;
    }
    if (row < leftCount) {
      current = current->left.get();
    } else {
      row -= leftCount + 1;
      current = current->right.get();
    }
  }
  return false;
}

qint64 ConversationHeightIndex::top(std::size_t row) const noexcept {
  return prefix(std::min(row, size()));
}

qint64 ConversationHeightIndex::bottom(std::size_t row) const noexcept {
  return row < size() ? prefix(row + 1) : totalHeight();
}

qint64 ConversationHeightIndex::totalHeight() const noexcept {
  return nodeTotal(root_);
}

std::size_t ConversationHeightIndex::rowAt(qint64 contentY) const noexcept {
  lastLookupSteps_ = 0;
  if (empty())
    return 0;
  const qint64 total = totalHeight();
  if (total <= 0)
    return 0;
  contentY = std::clamp<qint64>(contentY, 0, total - 1);

  std::size_t precedingRows = 0;
  const Node *current = root_.get();
  while (current) {
    ++lastLookupSteps_;
    const qint64 leftHeight = nodeTotal(current->left);
    if (contentY < leftHeight) {
      current = current->left.get();
      continue;
    }
    contentY -= leftHeight;
    const std::size_t leftCount = nodeCount(current->left);
    if (contentY < current->height)
      return precedingRows + leftCount;
    contentY -= current->height;
    precedingRows += leftCount + 1;
    current = current->right.get();
  }
  return size() - 1;
}

std::size_t
ConversationHeightIndex::nextRowWithExtent(std::size_t row) const noexcept {
  if (row >= size())
    return size();
  const qint64 position = top(row);
  return position < totalHeight() ? rowAt(position) : size();
}

std::size_t ConversationHeightIndex::previousRowWithExtent(
    std::size_t row) const noexcept {
  if (empty())
    return size();
  row = std::min(row, size() - 1);
  const qint64 position = bottom(row);
  return position > 0 ? rowAt(position - 1) : size();
}

qint64 ConversationHeightIndex::prefix(std::size_t count) const noexcept {
  count = std::min(count, size());
  qint64 result = 0;
  const Node *current = root_.get();
  while (current && count != 0) {
    const std::size_t leftCount = nodeCount(current->left);
    if (count <= leftCount) {
      current = current->left.get();
      continue;
    }
    result += nodeTotal(current->left) + current->height;
    count -= leftCount + 1;
    current = current->right.get();
  }
  return result;
}

std::uint64_t ConversationHeightIndex::nextPriority() noexcept {
  priorityState_ ^= priorityState_ >> 12;
  priorityState_ ^= priorityState_ << 25;
  priorityState_ ^= priorityState_ >> 27;
  return priorityState_ * 0x2545f4914f6cdd1dULL;
}

std::size_t ConversationHeightIndex::nodeCount(
    const std::unique_ptr<Node> &node) noexcept {
  return node ? node->count : 0;
}

qint64 ConversationHeightIndex::nodeTotal(
    const std::unique_ptr<Node> &node) noexcept {
  return node ? node->total : 0;
}

void ConversationHeightIndex::updateNode(Node *node) noexcept {
  if (!node)
    return;
  node->count = nodeCount(node->left) + 1 + nodeCount(node->right);
  node->total = nodeTotal(node->left) + node->height + nodeTotal(node->right);
}

std::pair<std::unique_ptr<ConversationHeightIndex::Node>,
          std::unique_ptr<ConversationHeightIndex::Node>>
ConversationHeightIndex::split(std::unique_ptr<Node> root,
                               std::size_t leftCount,
                               std::size_t &steps) {
  if (!root)
    return {};
  ++steps;
  if (nodeCount(root->left) >= leftCount) {
    auto [left, middle] =
        split(std::move(root->left), leftCount, steps);
    root->left = std::move(middle);
    updateNode(root.get());
    return {std::move(left), std::move(root)};
  }
  const std::size_t remaining = leftCount - nodeCount(root->left) - 1;
  auto [middle, right] = split(std::move(root->right), remaining, steps);
  root->right = std::move(middle);
  updateNode(root.get());
  return {std::move(root), std::move(right)};
}

std::unique_ptr<ConversationHeightIndex::Node>
ConversationHeightIndex::merge(std::unique_ptr<Node> left,
                               std::unique_ptr<Node> right,
                               std::size_t &steps) {
  if (!left)
    return right;
  if (!right)
    return left;
  ++steps;
  if (left->priority >= right->priority) {
    left->right = merge(std::move(left->right), std::move(right), steps);
    updateNode(left.get());
    return left;
  }
  right->left = merge(std::move(left), std::move(right->left), steps);
  updateNode(right.get());
  return right;
}

std::unique_ptr<ConversationHeightIndex::Node>
ConversationHeightIndex::makeRows(std::span<const int> heights,
                                  std::size_t &steps) {
  std::unique_ptr<Node> result;
  for (int height : heights) {
    auto node =
        std::make_unique<Node>(validHeight(height), nextPriority());
    result = merge(std::move(result), std::move(node), steps);
  }
  return result;
}

} // namespace codexui::codex::middle
