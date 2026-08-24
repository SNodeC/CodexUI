// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ipc/QtSocketPairEndpoint.h"

#include <QSocketNotifier>

#include <array>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace codexui::codex::ipc {

QtSocketPairEndpoint::QtSocketPairEndpoint(int descriptor,
                                           std::size_t maximumQueuedBytes,
                                           QObject *parent)
    : QObject(parent), descriptor(descriptor),
      maximumQueuedBytes(maximumQueuedBytes) {
  readNotifier = new QSocketNotifier(descriptor, QSocketNotifier::Read, this);
  writeNotifier = new QSocketNotifier(descriptor, QSocketNotifier::Write, this);
  writeNotifier->setEnabled(false);
  connect(readNotifier, &QSocketNotifier::activated, this,
          [this] { readReady(); });
  connect(writeNotifier, &QSocketNotifier::activated, this,
          [this] { writeReady(); });
}

QtSocketPairEndpoint::~QtSocketPairEndpoint() { close(); }

bool QtSocketPairEndpoint::send(const char *data, std::size_t size) {
  if (!isOpen() || size > maximumQueuedBytes ||
      queuedBytes() > maximumQueuedBytes - size)
    return false;

  if (writeOffset != 0 && writeOffset == writeBuffer.size()) {
    writeBuffer.clear();
    writeOffset = 0;
  }
  writeBuffer.append(data, size);
  writeReady();
  return isOpen();
}

bool QtSocketPairEndpoint::send(const std::string &data) {
  return send(data.data(), data.size());
}

std::size_t QtSocketPairEndpoint::queuedBytes() const noexcept {
  return writeBuffer.size() - writeOffset;
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
  writeBuffer.clear();
  writeOffset = 0;
  if (onClosed)
    onClosed();
}

void QtSocketPairEndpoint::readReady() {
  std::array<char, 64U * 1024U> buffer{};
  while (isOpen()) {
    const ssize_t received =
        ::recv(descriptor, buffer.data(), buffer.size(), 0);
    if (received > 0) {
      if (onData)
        onData(buffer.data(), static_cast<std::size_t>(received));
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
  while (isOpen() && queuedBytes() != 0) {
    const ssize_t sent = ::send(descriptor, writeBuffer.data() + writeOffset,
                                queuedBytes(), MSG_NOSIGNAL);
    if (sent > 0) {
      writeOffset += static_cast<std::size_t>(sent);
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

  writeBuffer.clear();
  writeOffset = 0;
  if (writeNotifier)
    writeNotifier->setEnabled(false);
}

void QtSocketPairEndpoint::fail(int errorNumber) noexcept {
  if (onError)
    onError(errorNumber);
  close();
}

} // namespace codexui::codex::ipc
