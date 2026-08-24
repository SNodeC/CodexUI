// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_IPC_SNODESOCKETPAIRENDPOINT_H
#define CODEXUI_CODEX_IPC_SNODESOCKETPAIRENDPOINT_H

#include <core/eventreceiver/ReadEventReceiver.h>
#include <core/eventreceiver/WriteEventReceiver.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace codexui::codex::ipc {

class SNodeSocketPairEndpoint final
    : public core::eventreceiver::ReadEventReceiver,
      public core::eventreceiver::WriteEventReceiver {
public:
  using DataHandler = std::function<void(const char *, std::size_t)>;
  using ErrorHandler = std::function<void(int)>;
  using ClosedHandler = std::function<void()>;

  static SNodeSocketPairEndpoint *create(int descriptor,
                                         std::size_t maximumQueuedBytes,
                                         std::size_t maximumReadBytesPerEvent);

  SNodeSocketPairEndpoint(const SNodeSocketPairEndpoint &) = delete;
  SNodeSocketPairEndpoint &operator=(const SNodeSocketPairEndpoint &) = delete;

  [[nodiscard]] bool send(const char *data, std::size_t size);
  [[nodiscard]] bool send(const std::string &data);
  [[nodiscard]] std::size_t queuedBytes() const noexcept;

  void setOnData(DataHandler handler);
  void setOnError(ErrorHandler handler);
  void setOnClosed(ClosedHandler handler);
  void close();

private:
  SNodeSocketPairEndpoint(int descriptor, std::size_t maximumQueuedBytes,
                          std::size_t maximumReadBytesPerEvent);
  ~SNodeSocketPairEndpoint() override;

  void readEvent() override;
  void writeEvent() override;
  void unobservedEvent() override;
  void destruct() override;
  void shutdownEvent(const core::ShutdownContext &context) override;
  void closeDescriptor() noexcept;
  void reportError(int errorNumber);

  int descriptor;
  std::size_t maximumQueuedBytes;
  std::size_t maximumReadBytesPerEvent;
  std::vector<char> writeBuffer;
  std::size_t writeOffset = 0;
  DataHandler onData;
  ErrorHandler onError;
  ClosedHandler onClosed;
  bool initializing = true;
  bool closing = false;
};

} // namespace codexui::codex::ipc

#endif // CODEXUI_CODEX_IPC_SNODESOCKETPAIRENDPOINT_H
