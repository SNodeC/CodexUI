// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_IPC_QTSOCKETPAIRENDPOINT_H
#define CODEXUI_CODEX_IPC_QTSOCKETPAIRENDPOINT_H

#include <QObject>

#include <cstddef>
#include <deque>
#include <functional>
#include <string>

class QSocketNotifier;

namespace codexui::codex::ipc {

class QtSocketPairEndpoint final : public QObject {
public:
  using DataHandler = std::function<void(const char *, std::size_t)>;
  using ErrorHandler = std::function<void(int)>;
  using ClosedHandler = std::function<void()>;

  explicit QtSocketPairEndpoint(int descriptor, std::size_t maximumQueuedBytes,
                                std::size_t maximumReadBytesPerActivation =
                                    256U * 1024U,
                                std::size_t maximumWriteBytesPerActivation =
                                    256U * 1024U,
                                QObject *parent = nullptr);
  ~QtSocketPairEndpoint() override;

  QtSocketPairEndpoint(const QtSocketPairEndpoint &) = delete;
  QtSocketPairEndpoint &operator=(const QtSocketPairEndpoint &) = delete;

  [[nodiscard]] bool send(const char *data, std::size_t size);
  [[nodiscard]] bool send(const std::string &data);
  [[nodiscard]] std::size_t queuedBytes() const noexcept;
  [[nodiscard]] std::size_t retainedWriteBytes() const noexcept;
  [[nodiscard]] bool isOpen() const noexcept;

  void setOnData(DataHandler handler);
  void setOnError(ErrorHandler handler);
  void setOnClosed(ClosedHandler handler);
  void close() noexcept;

private:
  void readReady();
  void writeReady();
  void fail(int errorNumber) noexcept;
  void closeTransport() noexcept;

  int descriptor = -1;
  std::size_t maximumQueuedBytes;
  std::size_t maximumReadBytesPerActivation;
  std::size_t maximumWriteBytesPerActivation;
  std::deque<std::string> writeChunks;
  std::size_t firstChunkOffset = 0;
  std::size_t queuedWriteBytes = 0;
  QSocketNotifier *readNotifier = nullptr;
  QSocketNotifier *writeNotifier = nullptr;
  DataHandler onData;
  ErrorHandler onError;
  ClosedHandler onClosed;
  bool closing = false;
  bool destroying = false;
};

} // namespace codexui::codex::ipc

#endif // CODEXUI_CODEX_IPC_QTSOCKETPAIRENDPOINT_H
