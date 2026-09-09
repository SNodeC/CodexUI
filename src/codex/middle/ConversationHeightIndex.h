// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_CONVERSATIONHEIGHTINDEX_H
#define CODEXUI_CODEX_MIDDLE_CONVERSATIONHEIGHTINDEX_H

#include <QtGlobal>

#include <cstddef>
#include <span>
#include <vector>

namespace codexui::codex::middle {

// Variable-height prefix index for the conversation's flat visual rows.
// Ordinary position lookup and one-row height changes are logarithmic. Tail
// appends extend the Fenwick tree without traversing retained rows; uncommon
// non-tail structure changes rebuild from the already validated model order.
class ConversationHeightIndex final {
public:
  void clear() noexcept;
  void reset(std::size_t count, int estimatedHeight);
  void assign(std::span<const int> heights);
  void insert(std::size_t row, std::span<const int> heights);
  void remove(std::size_t row, std::size_t count);
  void move(std::size_t sourceRow, std::size_t count,
            std::size_t destinationRow);

  [[nodiscard]] std::size_t size() const noexcept {
    return heights_.size() - offset_;
  }
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
  [[nodiscard]] qint64 prefix(std::size_t count) const noexcept;
  [[nodiscard]] qint64 physicalPrefix(std::size_t count) const noexcept;
  void append(int height);
  void normalize();
  void rebuild();

  std::vector<int> heights_;
  std::vector<qint64> tree_{0};
  std::size_t offset_ = 0;
  mutable std::size_t lastLookupSteps_ = 0;
  std::size_t lastUpdateSteps_ = 0;
  std::size_t rebuildCount_ = 0;
};

} // namespace codexui::codex::middle

#endif // CODEXUI_CODEX_MIDDLE_CONVERSATIONHEIGHTINDEX_H
