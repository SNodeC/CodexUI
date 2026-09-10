// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONHEIGHTINDEX_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONHEIGHTINDEX_H

#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace codexui::codex::middle {

// Variable-height prefix index for the conversation's flat visual rows.
// Position lookup, one-row height changes, and structural changes all touch
// only an order-statistic path. A complete rebuild is reserved for assigning a
// different authoritative row sequence.
class ConversationHeightIndex final {
public:
  ConversationHeightIndex() = default;
  ~ConversationHeightIndex();

  ConversationHeightIndex(const ConversationHeightIndex &) = delete;
  ConversationHeightIndex &operator=(const ConversationHeightIndex &) = delete;

  void clear() noexcept;
  void reset(std::size_t count, int estimatedHeight);
  void assign(std::span<const int> heights);
  void insert(std::size_t row, std::span<const int> heights);
  void remove(std::size_t row, std::size_t count);
  void move(std::size_t sourceRow, std::size_t count,
            std::size_t destinationRow);

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] int height(std::size_t row) const noexcept;
  [[nodiscard]] bool setHeight(std::size_t row, int height) noexcept;
  [[nodiscard]] qint64 top(std::size_t row) const noexcept;
  [[nodiscard]] qint64 bottom(std::size_t row) const noexcept;
  [[nodiscard]] qint64 totalHeight() const noexcept;
  [[nodiscard]] std::size_t rowAt(qint64 contentY) const noexcept;

  [[nodiscard]] std::size_t lastLookupSteps() const noexcept {
    return lastLookupSteps_;
  }
  [[nodiscard]] std::size_t lastUpdateSteps() const noexcept {
    return lastUpdateSteps_;
  }
  [[nodiscard]] std::size_t rebuildCount() const noexcept {
    return rebuildCount_;
  }

private:
  struct Node {
    Node(int height, std::uint64_t priority)
        : height(height), total(height), priority(priority) {}

    int height = 0;
    std::size_t count = 1;
    qint64 total = 0;
    std::uint64_t priority = 0;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
  };

  [[nodiscard]] qint64 prefix(std::size_t count) const noexcept;
  [[nodiscard]] std::uint64_t nextPriority() noexcept;
  static std::size_t nodeCount(const std::unique_ptr<Node> &node) noexcept;
  static qint64 nodeTotal(const std::unique_ptr<Node> &node) noexcept;
  static void updateNode(Node *node) noexcept;
  static std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>>
  split(std::unique_ptr<Node> root, std::size_t leftCount,
        std::size_t &steps);
  static std::unique_ptr<Node> merge(std::unique_ptr<Node> left,
                                     std::unique_ptr<Node> right,
                                     std::size_t &steps);
  [[nodiscard]] std::unique_ptr<Node>
  makeRows(std::span<const int> heights, std::size_t &steps);

  std::unique_ptr<Node> root_;
  std::uint64_t priorityState_ = 0x243f6a8885a308d3ULL;
  mutable std::size_t lastLookupSteps_ = 0;
  std::size_t lastUpdateSteps_ = 0;
  std::size_t rebuildCount_ = 0;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONHEIGHTINDEX_H
