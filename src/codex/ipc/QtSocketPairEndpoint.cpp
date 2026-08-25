// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ipc/QtSocketPairEndpoint.h"

#include <QSocketNotifier>
#include <QPointer>

#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace codexui::codex::ipc {

QtSocketPairEndpoint::QtSocketPairEndpoint(int descriptor,
                                           std::size_t maximumQueuedBytes,
                                           std::size_t maximumReadBytesPerActivation,
                                           std::size_t maximumWriteBytesPerActivation,
                                           QObject *parent)
    : QObject(parent), descriptor(descriptor),
      maximumQueuedBytes(maximumQueuedBytes),
      maximumReadBytesPerActivation(maximumReadBytesPerActivation),
      maximumWriteBytesPerActivation(maximumWriteBytesPerActivation) {
  readNotifier = new QSocketNotifier(descriptor, QSocketNotifier::Read, this);
  writeNotifier = new QSocketNotifier(descriptor, QSocketNotifier::Write, this);
  writeNotifier->setEnabled(false);
  connect(readNotifier, &QSocketNotifier::activated, this,
          [this] { readReady(); });
  connect(writeNotifier, &QSocketNotifier::activated, this,
          [this] { writeReady(); });
}

QtSocketPairEndpoint::~QtSocketPairEndpoint() {
  destroying = true;
  onData = {};
  onError = {};
  onClosed = {};
  closeTransport();
}

bool QtSocketPairEndpoint::send(const char *data, std::size_t size) {
  if (!isOpen() || size > maximumQueuedBytes ||
      queuedBytes() > maximumQueuedBytes - size)
    return false;

  if (size != 0) {
    writeChunks.emplace_back(data, size);
    queuedWriteBytes += size;
  }
  QPointer<QtSocketPairEndpoint> guard(this);
  writeReady();
  return guard && guard->isOpen();
}

bool QtSocketPairEndpoint::send(const std::string &data) {
  return send(data.data(), data.size());
}

std::size_t QtSocketPairEndpoint::queuedBytes() const noexcept {
  return queuedWriteBytes;
}

std::size_t QtSocketPairEndpoint::retainedWriteBytes() const noexcept {
  std::size_t retained = 0;
  for (const std::string &chunk : writeChunks)
    retained += chunk.capacity();
  return retained;
}

bool QtSocketPairEndpoint::isOpen() const noexcept {
  return descriptor >= 0 && !closing;
}

void QtSocketPairEndpoint::setOnData(DataHandler handler) {
  onData = std::move(handler);
}

void QtSocketPairEndpoint::setOnError(ErrorHandler handler) {
  onError = std::move(handler);
}

void QtSocketPairEndpoint::setOnClosed(ClosedHandler handler) {
  onClosed = std::move(handler);
}

void QtSocketPairEndpoint::close() noexcept {
  if (closing)
    return;
  ClosedHandler closed = std::move(onClosed);
  onClosed = {};
  closeTransport();
  if (!destroying && closed) {
    try {
      closed();
    } catch (...) {
    }
  }
}

void QtSocketPairEndpoint::closeTransport() noexcept {
  if (closing)
    return;
  closing = true;
  if (readNotifier)
    readNotifier->setEnabled(false);
  if (writeNotifier)
    writeNotifier->setEnabled(false);
  if (descriptor >= 0) {
    ::shutdown(descriptor, SHUT_RDWR);
    ::close(descriptor);
    descriptor = -1;
  }
  writeChunks.clear();
  firstChunkOffset = 0;
  queuedWriteBytes = 0;
}

void QtSocketPairEndpoint::readReady() {
  std::array<char, 64U * 1024U> buffer{};
  std::size_t totalRead = 0;
  while (isOpen() && totalRead < maximumReadBytesPerActivation) {
    const std::size_t requested =
        std::min(buffer.size(), maximumReadBytesPerActivation - totalRead);
    const ssize_t received =
        ::recv(descriptor, buffer.data(), requested, 0);
    if (received > 0) {
      totalRead += static_cast<std::size_t>(received);
      if (onData) {
        QPointer<QtSocketPairEndpoint> guard(this);
        try {
          onData(buffer.data(), static_cast<std::size_t>(received));
        } catch (...) {
          if (guard)
            guard->fail(EPROTO);
          return;
        }
        if (!guard)
          return;
      }
      continue;
    }
    if (received == 0) {
      close();
      return;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    fail(errno != 0 ? errno : EIO);
    return;
  }
}

void QtSocketPairEndpoint::writeReady() {
  std::size_t totalWritten = 0;
  while (isOpen() && queuedBytes() != 0 &&
         totalWritten < maximumWriteBytesPerActivation) {
    const std::string &chunk = writeChunks.front();
    const std::size_t requested = std::min(
        chunk.size() - firstChunkOffset,
        maximumWriteBytesPerActivation - totalWritten);
    const ssize_t sent = ::send(descriptor, chunk.data() + firstChunkOffset,
                                requested, MSG_NOSIGNAL);
    if (sent > 0) {
      const std::size_t size = static_cast<std::size_t>(sent);
      firstChunkOffset += size;
      queuedWriteBytes -= size;
      totalWritten += size;
      if (firstChunkOffset == chunk.size()) {
        writeChunks.pop_front();
        firstChunkOffset = 0;
      }
      continue;
    }
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      writeNotifier->setEnabled(true);
      return;
    }
    fail(sent == 0 ? EPIPE : (errno != 0 ? errno : EIO));
    return;
  }

  if (queuedBytes() == 0) {
    writeChunks.clear();
    firstChunkOffset = 0;
    if (writeNotifier)
      writeNotifier->setEnabled(false);
  } else if (writeNotifier) {
    writeNotifier->setEnabled(true);
  }
}

void QtSocketPairEndpoint::fail(int errorNumber) noexcept {
  if (closing)
    return;
  ErrorHandler error = std::move(onError);
  ClosedHandler closed = std::move(onClosed);
  onError = {};
  onClosed = {};
  closeTransport();
  QPointer<QtSocketPairEndpoint> guard(this);
  if (!destroying && error) {
    try {
      error(errorNumber);
    } catch (...) {
    }
  }
  if (guard && !destroying && closed) {
    try {
      closed();
    } catch (...) {
    }
  }
}

} // namespace codexui::codex::ipc
