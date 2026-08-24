// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ipc/SNodeSocketPairEndpoint.h"

#include <core/system/socket.h>
#include <log/SemanticLogger.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace codexui::codex::ipc {
namespace {

constexpr std::size_t MaximumChunkBytes = 16U * 1024U;
constexpr std::size_t MaximumWriteBytesPerEvent = 256U * 1024U;

logger::LogScope makeLogScope() {
  return {logger::LogOrigin::Application,
          logger::LogBoundary::Connection,
          "codexui.ipc",
          "socketpair",
          logger::LogRole::Client,
          {}};
}

} // namespace

SNodeSocketPairEndpoint *
SNodeSocketPairEndpoint::create(int descriptor, std::size_t maximumQueuedBytes,
                                std::size_t maximumReadBytesPerEvent) {
  if (descriptor < 0 || maximumQueuedBytes == 0 ||
      maximumReadBytesPerEvent == 0)
    return nullptr;

  auto *endpoint = new SNodeSocketPairEndpoint(descriptor, maximumQueuedBytes,
                                               maximumReadBytesPerEvent);
  const bool readEnabled = endpoint->ReadEventReceiver::enable(descriptor);
  const bool writeEnabled =
      readEnabled && endpoint->WriteEventReceiver::enable(descriptor);
  if (!readEnabled || !writeEnabled) {
    if (readEnabled)
      endpoint->ReadEventReceiver::disable();
    if (writeEnabled)
      endpoint->WriteEventReceiver::disable();
    endpoint->closeDescriptor();
    delete endpoint;
    return nullptr;
  }

  endpoint->WriteEventReceiver::suspend();
  endpoint->initializing = false;
  return endpoint;
}

SNodeSocketPairEndpoint::SNodeSocketPairEndpoint(
    int descriptor, std::size_t maximumQueuedBytes,
    std::size_t maximumReadBytesPerEvent)
    : core::eventreceiver::ReadEventReceiver("SocketPairEndpoint",
                                             makeLogScope(), TIMEOUT::DISABLE),
      core::eventreceiver::WriteEventReceiver("SocketPairEndpoint",
                                              makeLogScope(), TIMEOUT::DISABLE),
      descriptor(descriptor), maximumQueuedBytes(maximumQueuedBytes),
      maximumReadBytesPerEvent(maximumReadBytesPerEvent) {}

SNodeSocketPairEndpoint::~SNodeSocketPairEndpoint() { closeDescriptor(); }

bool SNodeSocketPairEndpoint::send(const char *data, std::size_t size) {
  const std::size_t outstanding = queuedBytes();
  if (closing || !WriteEventReceiver::isEnabled() ||
      size > maximumQueuedBytes || outstanding > maximumQueuedBytes - size)
    return false;
  if (size == 0)
    return true;

  if (writeOffset != 0 && (writeOffset == writeBuffer.size() ||
                           writeOffset >= writeBuffer.size() / 2)) {
    writeBuffer.erase(writeBuffer.begin(),
                      writeBuffer.begin() +
                          static_cast<std::ptrdiff_t>(writeOffset));
    writeOffset = 0;
  }
  writeBuffer.insert(writeBuffer.end(), data, data + size);
  if (WriteEventReceiver::isSuspended())
    WriteEventReceiver::resume();
  return true;
}

bool SNodeSocketPairEndpoint::send(const std::string &data) {
  return send(data.data(), data.size());
}

std::size_t SNodeSocketPairEndpoint::queuedBytes() const noexcept {
  return writeBuffer.size() - writeOffset;
}

void SNodeSocketPairEndpoint::setOnData(DataHandler handler) {
  onData = std::move(handler);
}

void SNodeSocketPairEndpoint::setOnError(ErrorHandler handler) {
  onError = std::move(handler);
}

void SNodeSocketPairEndpoint::setOnClosed(ClosedHandler handler) {
  onClosed = std::move(handler);
}

void SNodeSocketPairEndpoint::close() {
  if (closing)
    return;
  closing = true;
  writeBuffer.clear();
  writeOffset = 0;
  if (ReadEventReceiver::isEnabled())
    ReadEventReceiver::disable();
  if (WriteEventReceiver::isEnabled())
    WriteEventReceiver::disable();
}

void SNodeSocketPairEndpoint::readEvent() {
  std::array<char, MaximumChunkBytes> chunk{};
  std::size_t totalRead = 0;
  while (!closing && totalRead < maximumReadBytesPerEvent) {
    const std::size_t requested =
        std::min(chunk.size(), maximumReadBytesPerEvent - totalRead);
    const ssize_t result =
        core::system::recv(descriptor, chunk.data(), requested, 0);
    if (result > 0) {
      const std::size_t size = static_cast<std::size_t>(result);
      totalRead += size;
      if (onData)
        onData(chunk.data(), size);
      continue;
    }
    if (result == 0) {
      close();
      return;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    reportError(errno != 0 ? errno : EIO);
    return;
  }
}

void SNodeSocketPairEndpoint::writeEvent() {
  std::size_t totalWritten = 0;
  while (!closing && queuedBytes() != 0 &&
         totalWritten < MaximumWriteBytesPerEvent) {
    const std::size_t requested =
        std::min({queuedBytes(), MaximumChunkBytes,
                  MaximumWriteBytesPerEvent - totalWritten});
    const ssize_t result = core::system::send(
        descriptor, writeBuffer.data() + writeOffset, requested, MSG_NOSIGNAL);
    if (result > 0) {
      const std::size_t size = static_cast<std::size_t>(result);
      writeOffset += size;
      totalWritten += size;
      continue;
    }
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    reportError(result == 0 ? EPIPE : (errno != 0 ? errno : EIO));
    return;
  }

  if (queuedBytes() == 0) {
    writeBuffer.clear();
    writeOffset = 0;
    if (!closing)
      WriteEventReceiver::suspend();
  }
}

void SNodeSocketPairEndpoint::unobservedEvent() {
  if (initializing)
    return;
  closeDescriptor();
  if (onClosed)
    onClosed();
  delete this;
}

void SNodeSocketPairEndpoint::destruct() { close(); }

void SNodeSocketPairEndpoint::shutdownEvent(
    const core::ShutdownContext &context) {
  static_cast<void>(context);
  close();
}

void SNodeSocketPairEndpoint::closeDescriptor() noexcept {
  if (descriptor >= 0) {
    ::shutdown(descriptor, SHUT_RDWR);
    ::close(descriptor);
    descriptor = -1;
  }
}

void SNodeSocketPairEndpoint::reportError(int errorNumber) {
  if (onError)
    onError(errorNumber);
  close();
}

} // namespace codexui::codex::ipc
