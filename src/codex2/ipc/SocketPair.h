// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX2_IPC_SOCKETPAIR_H
#define CODEXUI_CODEX2_IPC_SOCKETPAIR_H

namespace codexui::codex2::ipc {

class SocketPair final {
public:
  SocketPair() noexcept;
  SocketPair(const SocketPair &) = delete;
  SocketPair(SocketPair &&other) noexcept;
  ~SocketPair();

  SocketPair &operator=(const SocketPair &) = delete;
  SocketPair &operator=(SocketPair &&other) noexcept;

  [[nodiscard]] bool isValid() const noexcept;
  [[nodiscard]] bool hasFirstEndpoint() const noexcept;
  [[nodiscard]] bool hasSecondEndpoint() const noexcept;
  [[nodiscard]] int error() const noexcept;

  [[nodiscard]] int firstEndpoint() const noexcept;
  [[nodiscard]] int secondEndpoint() const noexcept;
  [[nodiscard]] int releaseFirstEndpoint() noexcept;
  [[nodiscard]] int releaseSecondEndpoint() noexcept;

  void closeFirstEndpoint() noexcept;
  void closeSecondEndpoint() noexcept;

private:
  int first = -1;
  int second = -1;
  int creationError = 0;
};

} // namespace codexui::codex2::ipc

#endif // CODEXUI_CODEX2_IPC_SOCKETPAIR_H
