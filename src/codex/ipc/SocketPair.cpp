// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ipc/SocketPair.h"

#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace codexui::codex::ipc {
namespace {

void closeDescriptor(int &descriptor) noexcept {
  if (descriptor >= 0) {
    ::close(descriptor);
    descriptor = -1;
  }
}

} // namespace

SocketPair::SocketPair() noexcept {
  int endpoints[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                   endpoints) == 0) {
    first = endpoints[0];
    second = endpoints[1];
  } else {
    creationError = errno != 0 ? errno : EIO;
  }
}

SocketPair::SocketPair(SocketPair &&other) noexcept
    : first(std::exchange(other.first, -1)),
      second(std::exchange(other.second, -1)),
      creationError(std::exchange(other.creationError, 0)) {}

SocketPair::~SocketPair() {
  closeFirstEndpoint();
  closeSecondEndpoint();
}

SocketPair &SocketPair::operator=(SocketPair &&other) noexcept {
  if (this != &other) {
    closeFirstEndpoint();
    closeSecondEndpoint();
    first = std::exchange(other.first, -1);
    second = std::exchange(other.second, -1);
    creationError = std::exchange(other.creationError, 0);
  }
  return *this;
}

bool SocketPair::isValid() const noexcept {
  return hasFirstEndpoint() && hasSecondEndpoint();
}

bool SocketPair::hasFirstEndpoint() const noexcept { return first >= 0; }

bool SocketPair::hasSecondEndpoint() const noexcept { return second >= 0; }

int SocketPair::error() const noexcept { return creationError; }

int SocketPair::firstEndpoint() const noexcept { return first; }

int SocketPair::secondEndpoint() const noexcept { return second; }

int SocketPair::releaseFirstEndpoint() noexcept {
  return std::exchange(first, -1);
}

int SocketPair::releaseSecondEndpoint() noexcept {
  return std::exchange(second, -1);
}

void SocketPair::closeFirstEndpoint() noexcept { closeDescriptor(first); }

void SocketPair::closeSecondEndpoint() noexcept { closeDescriptor(second); }

} // namespace codexui::codex::ipc
