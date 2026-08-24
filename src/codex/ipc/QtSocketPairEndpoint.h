// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_IPC_QTSOCKETPAIRENDPOINT_H
#define CODEXUI_CODEX_IPC_QTSOCKETPAIRENDPOINT_H

#include <QObject>

#include <cstddef>
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
                                QObject *parent = nullptr);
  ~QtSocketPairEndpoint() override;

  QtSocketPairEndpoint(const QtSocketPairEndpoint &) = delete;
  QtSocketPairEndpoint &operator=(const QtSocketPairEndpoint &) = delete;

  [[nodiscard]] bool send(const char *data, std::size_t size);
  [[nodiscard]] bool send(const std::string &data);
  [[nodiscard]] std::size_t queuedBytes() const noexcept;
  [[nodiscard]] bool isOpen() const noexcept;

  void setOnData(DataHandler handler);
  void setOnError(ErrorHandler handler);
  void setOnClosed(ClosedHandler handler);
  void close() noexcept;

private:
  void readReady();
  void writeReady();
  void fail(int errorNumber) noexcept;

  int descriptor = -1;
  std::size_t maximumQueuedBytes;
  std::string writeBuffer;
  std::size_t writeOffset = 0;
  QSocketNotifier *readNotifier = nullptr;
  QSocketNotifier *writeNotifier = nullptr;
  DataHandler onData;
  ErrorHandler onError;
  ClosedHandler onClosed;
  bool closing = false;
};

} // namespace codexui::codex::ipc

#endif // CODEXUI_CODEX_IPC_QTSOCKETPAIRENDPOINT_H
