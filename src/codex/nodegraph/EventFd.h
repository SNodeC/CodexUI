// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_EVENTFD_H
#define CODEXUI_CODEX_NODEGRAPH_EVENTFD_H

#include <cstdint>

namespace codexui::nodegraph {

// Owns one Linux eventfd used only as a cross-thread wake-up counter.
class EventFd final {
public:
  enum class NotifyStatus : std::uint8_t {
    Notified,
    AlreadySignaled,
    Closed,
    Error,
  };

  struct NotifyResult final {
    NotifyStatus status = NotifyStatus::Error;
    int errorNumber = 0;

    [[nodiscard]] bool accepted() const noexcept;
    bool operator==(const NotifyResult &) const = default;
  };

  enum class DrainStatus : std::uint8_t {
    Drained,
    Empty,
    Closed,
    Error,
  };

  struct DrainResult final {
    DrainStatus status = DrainStatus::Error;
    std::uint64_t count = 0;
    int errorNumber = 0;

    [[nodiscard]] bool accepted() const noexcept;
    bool operator==(const DrainResult &) const = default;
  };

  EventFd() noexcept;
  ~EventFd();

  EventFd(const EventFd &) = delete;
  EventFd &operator=(const EventFd &) = delete;
  EventFd(EventFd &&other) noexcept;
  EventFd &operator=(EventFd &&other) noexcept;

  [[nodiscard]] int descriptor() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] int creationError() const noexcept;

  [[nodiscard]] NotifyResult notify() const noexcept;
  [[nodiscard]] DrainResult drain() const noexcept;

  void close() noexcept;

private:
  int descriptor_ = -1;
  int creationError_ = 0;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_EVENTFD_H
