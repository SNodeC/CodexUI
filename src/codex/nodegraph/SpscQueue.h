// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_SPSCQUEUE_H
#define CODEXUI_CODEX_NODEGRAPH_SPSCQUEUE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace codexui::nodegraph {

template <typename T, std::size_t Capacity> class SpscQueue {
  static_assert(Capacity > 0, "an SPSC queue needs at least one slot");
  static_assert(Capacity <= std::numeric_limits<std::size_t>::max() / 2,
                "SPSC queue capacity is too large");
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "SPSC queue positions must be lock-free");

public:
  SpscQueue() = default;
  ~SpscQueue() = default;

  SpscQueue(const SpscQueue &) = delete;
  SpscQueue &operator=(const SpscQueue &) = delete;
  SpscQueue(SpscQueue &&) = delete;
  SpscQueue &operator=(SpscQueue &&) = delete;

  [[nodiscard]] bool
  tryPush(T &&value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    const std::size_t producer =
        producer_.position.load(std::memory_order_relaxed);
    const std::size_t consumer =
        consumer_.position.load(std::memory_order_acquire);
    if (distance(consumer, producer) == Capacity)
      return false;

    slots_[slotIndex(producer)].emplace(std::move(value));
    producer_.position.store(advance(producer), std::memory_order_release);
    return true;
  }

  template <typename... Args>
    requires std::is_constructible_v<T, Args...>
  [[nodiscard]] bool tryEmplace(Args &&...args) noexcept(
      std::is_nothrow_constructible_v<T, Args...>) {
    const std::size_t producer =
        producer_.position.load(std::memory_order_relaxed);
    const std::size_t consumer =
        consumer_.position.load(std::memory_order_acquire);
    if (distance(consumer, producer) == Capacity)
      return false;

    slots_[slotIndex(producer)].emplace(std::forward<Args>(args)...);
    producer_.position.store(advance(producer), std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool
  tryPop(T &value) noexcept(std::is_nothrow_move_assignable_v<T>) {
    const std::size_t consumer =
        consumer_.position.load(std::memory_order_relaxed);
    const std::size_t producer =
        producer_.position.load(std::memory_order_acquire);
    if (consumer == producer)
      return false;

    std::optional<T> &slot = slots_[slotIndex(consumer)];
    value = std::move(*slot);
    slot.reset();
    consumer_.position.store(advance(consumer), std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool empty() const noexcept {
    const std::size_t consumer =
        consumer_.position.load(std::memory_order_acquire);
    const std::size_t producer =
        producer_.position.load(std::memory_order_acquire);
    return consumer == producer;
  }

  [[nodiscard]] bool full() const noexcept {
    const std::size_t producer =
        producer_.position.load(std::memory_order_acquire);
    const std::size_t consumer =
        consumer_.position.load(std::memory_order_acquire);
    return distance(consumer, producer) == Capacity;
  }

  [[nodiscard]] std::size_t sizeApprox() const noexcept {
    const std::size_t consumer =
        consumer_.position.load(std::memory_order_acquire);
    const std::size_t producer =
        producer_.position.load(std::memory_order_acquire);
    const std::size_t observed = distance(consumer, producer);
    return observed <= Capacity ? observed : Capacity;
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

private:
  static constexpr std::size_t CycleSize = Capacity * 2;
  static constexpr std::size_t CacheLineSize = 64;

  struct alignas(CacheLineSize) Position {
    std::atomic<std::size_t> position{0};
  };

  [[nodiscard]] static constexpr std::size_t
  advance(std::size_t position) noexcept {
    return position + 1 == CycleSize ? 0 : position + 1;
  }

  [[nodiscard]] static constexpr std::size_t
  slotIndex(std::size_t position) noexcept {
    return position < Capacity ? position : position - Capacity;
  }

  [[nodiscard]] static constexpr std::size_t distance(std::size_t from,
                                                      std::size_t to) noexcept {
    return to >= from ? to - from : CycleSize - from + to;
  }

  Position producer_;
  Position consumer_;
  alignas(CacheLineSize) std::array<std::optional<T>, Capacity> slots_{};
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_SPSCQUEUE_H
