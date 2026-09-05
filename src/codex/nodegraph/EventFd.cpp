// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/EventFd.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace codexui::nodegraph {
namespace {

int currentError() noexcept { return errno != 0 ? errno : EIO; }

} // namespace

bool EventFd::NotifyResult::accepted() const noexcept {
  return status == NotifyStatus::Notified ||
         status == NotifyStatus::AlreadySignaled;
}

bool EventFd::DrainResult::accepted() const noexcept {
  return status == DrainStatus::Drained || status == DrainStatus::Empty;
}

EventFd::EventFd() noexcept {
  do {
    descriptor_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  } while (descriptor_ < 0 && errno == EINTR);

  if (descriptor_ < 0)
    creationError_ = currentError();
}

EventFd::~EventFd() { close(); }

EventFd::EventFd(EventFd &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      creationError_(std::exchange(other.creationError_, 0)) {}

EventFd &EventFd::operator=(EventFd &&other) noexcept {
  if (this != &other) {
    close();
    descriptor_ = std::exchange(other.descriptor_, -1);
    creationError_ = std::exchange(other.creationError_, 0);
  }
  return *this;
}

int EventFd::descriptor() const noexcept { return descriptor_; }

bool EventFd::valid() const noexcept { return descriptor_ >= 0; }

int EventFd::creationError() const noexcept { return creationError_; }

EventFd::NotifyResult EventFd::notify() const noexcept {
  if (!valid())
    return {NotifyStatus::Closed, EBADF};

  constexpr std::uint64_t Wake = 1;
  for (;;) {
    const ssize_t written = ::write(descriptor_, &Wake, sizeof(Wake));
    if (written == static_cast<ssize_t>(sizeof(Wake)))
      return {NotifyStatus::Notified, 0};
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return {NotifyStatus::AlreadySignaled, 0};
    return {NotifyStatus::Error, written >= 0 ? EIO : currentError()};
  }
}

EventFd::DrainResult EventFd::drain() const noexcept {
  if (!valid())
    return {DrainStatus::Closed, 0, EBADF};

  std::uint64_t count = 0;
  for (;;) {
    const ssize_t received = ::read(descriptor_, &count, sizeof(count));
    if (received == static_cast<ssize_t>(sizeof(count)))
      return {DrainStatus::Drained, count, 0};
    if (received < 0 && errno == EINTR)
      continue;
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return {DrainStatus::Empty, 0, 0};
    return {DrainStatus::Error, 0, received >= 0 ? EIO : currentError()};
  }
}

void EventFd::close() noexcept {
  const int descriptor = std::exchange(descriptor_, -1);
  if (descriptor >= 0)
    static_cast<void>(::close(descriptor));
}

} // namespace codexui::nodegraph
