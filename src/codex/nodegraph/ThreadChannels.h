// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_THREADCHANNELS_H
#define CODEXUI_CODEX_NODEGRAPH_THREADCHANNELS_H

#include "codex/nodegraph/EventFd.h"
#include "codex/nodegraph/Messages.h"
#include "codex/nodegraph/SpscQueue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace codexui::nodegraph {

enum class ChannelSendStatus : std::uint8_t {
  Accepted,
  QueueFull,
  CoalescedRescan,
  AcceptedWakeFailed,
  CoalescedRescanWakeFailed,
};

[[nodiscard]] bool messageAdmitted(ChannelSendStatus status) noexcept;
[[nodiscard]] bool wakeFailed(ChannelSendStatus status) noexcept;

// The application owns exactly one instance. It contains exactly one bounded
// SPSC queue and one eventfd in each direction; eventfds carry wake counts
// only.
class ThreadChannels final {
public:
  static constexpr std::size_t WorkerToQtCapacity = 512;
  static constexpr std::size_t QtToWorkerCapacity = 256;

  ThreadChannels() = default;
  ThreadChannels(const ThreadChannels &) = delete;
  ThreadChannels &operator=(const ThreadChannels &) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] int workerToQtEventFd() const noexcept;
  [[nodiscard]] int qtToWorkerEventFd() const noexcept;
  [[nodiscard]] int workerToQtCreationError() const noexcept;
  [[nodiscard]] int qtToWorkerCreationError() const noexcept;

  // Worker-thread producer operations.
  [[nodiscard]] ChannelSendStatus sendGraphChanged(GraphChange change);
  [[nodiscard]] ChannelSendStatus sendUiEffect(UiEffect &effect);
  [[nodiscard]] ChannelSendStatus sendWorkerStopped(WorkerStopped &stopped);

  // Qt-thread consumer operations.
  [[nodiscard]] EventFd::DrainResult drainWorkerToQtWake() const noexcept;
  [[nodiscard]] bool tryReceiveForQt(WorkerToQtMessage &message);

  // Qt-thread producer operations. The supplied payload remains intact when
  // QueueFull is returned, so the user's text/attachments stay available.
  [[nodiscard]] ChannelSendStatus sendNodeAction(NodeAction &action);
  [[nodiscard]] ChannelSendStatus sendRuntimeAction(RuntimeAction &action);
  [[nodiscard]] ChannelSendStatus sendShutdown(ShutdownRequest &request);

  // Worker-thread consumer operations.
  [[nodiscard]] EventFd::DrainResult drainQtToWorkerWake() const noexcept;
  [[nodiscard]] bool tryReceiveForWorker(QtToWorkerMessage &message);

  [[nodiscard]] std::size_t workerToQtSizeApprox() const noexcept;
  [[nodiscard]] std::size_t qtToWorkerSizeApprox() const noexcept;
  [[nodiscard]] bool rescanPending() const noexcept;

  // Call only after both event-loop observers are disabled and the worker is
  // joined.
  void close() noexcept;

private:
  [[nodiscard]] ChannelSendStatus wakeWorkerToQt(bool coalesced) const noexcept;
  [[nodiscard]] ChannelSendStatus wakeQtToWorker() const noexcept;
  void requireRescan(std::uint64_t revision) noexcept;

  SpscQueue<WorkerToQtMessage, WorkerToQtCapacity> workerToQt_;
  SpscQueue<QtToWorkerMessage, QtToWorkerCapacity> qtToWorker_;
  EventFd workerToQtWake_;
  EventFd qtToWorkerWake_;
  std::atomic<std::uint64_t> rescanRevision_{0};
  bool queuedTurnAfterRescan_ = false;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_THREADCHANNELS_H
