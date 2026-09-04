// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ThreadChannels.h"

#include <utility>
#include <variant>

namespace codexui::nodegraph {

bool messageAdmitted(ChannelSendStatus status) noexcept {
  return status != ChannelSendStatus::QueueFull;
}

bool wakeFailed(ChannelSendStatus status) noexcept {
  return status == ChannelSendStatus::AcceptedWakeFailed ||
         status == ChannelSendStatus::CoalescedRescanWakeFailed;
}

bool ThreadChannels::valid() const noexcept {
  return workerToQtWake_.valid() && qtToWorkerWake_.valid();
}

int ThreadChannels::workerToQtEventFd() const noexcept {
  return workerToQtWake_.descriptor();
}

int ThreadChannels::qtToWorkerEventFd() const noexcept {
  return qtToWorkerWake_.descriptor();
}

int ThreadChannels::workerToQtCreationError() const noexcept {
  return workerToQtWake_.creationError();
}

int ThreadChannels::qtToWorkerCreationError() const noexcept {
  return qtToWorkerWake_.creationError();
}

ChannelSendStatus ThreadChannels::sendGraphChanged(GraphChange change) {
  if (change.empty())
    return ChannelSendStatus::Accepted;
  const std::uint64_t revision = change.revision;
  if (change.affected.size() > MaximumDirectGraphReferences ||
      change.removed.size() >
          MaximumDirectGraphReferences - change.affected.size()) {
    requireRescan(revision);
    return wakeWorkerToQt(true);
  }
  if (workerToQt_.sizeApprox() >=
      WorkerToQtCapacity - WorkerToQtReservedSlots) {
    requireRescan(revision);
    return wakeWorkerToQt(true);
  }
  WorkerToQtMessage message(std::in_place_type<GraphChanged>,
                            GraphChanged{revision, std::move(change.affected),
                                         std::move(change.removed), false});
  if (workerToQt_.tryPush(std::move(message)))
    return wakeWorkerToQt(false);

  requireRescan(revision);
  return wakeWorkerToQt(true);
}

ChannelSendStatus ThreadChannels::sendUiEffect(UiEffect &effect) {
  const std::size_t limit = effect.kind == UiEffectKind::SelectThread
                                ? WorkerToQtCapacity - 1
                                : WorkerToQtCapacity - WorkerToQtReservedSlots;
  if (workerToQt_.sizeApprox() >= limit)
    return ChannelSendStatus::QueueFull;
  if (!workerToQt_.tryEmplace(std::in_place_type<UiEffect>, std::move(effect)))
    return ChannelSendStatus::QueueFull;
  return wakeWorkerToQt(false);
}

ChannelSendStatus ThreadChannels::sendWorkerStopped(WorkerStopped &stopped) {
  if (!workerToQt_.tryEmplace(std::in_place_type<WorkerStopped>,
                              std::move(stopped)))
    return ChannelSendStatus::QueueFull;
  return wakeWorkerToQt(false);
}

EventFd::DrainResult ThreadChannels::drainWorkerToQtWake() const noexcept {
  return workerToQtWake_.drain();
}

bool ThreadChannels::tryReceiveForQt(WorkerToQtMessage &message) {
  if (queuedTurnAfterRescan_) {
    queuedTurnAfterRescan_ = false;
    if (workerToQt_.tryPop(message))
      return true;
  }
  const std::uint64_t revision =
      rescanRevision_.exchange(0, std::memory_order_acq_rel);
  if (revision != 0) {
    queuedTurnAfterRescan_ = true;
    message = GraphChanged{revision, {}, {}, true};
    return true;
  }
  return workerToQt_.tryPop(message);
}

ChannelSendStatus ThreadChannels::sendNodeAction(NodeAction &action) {
  if (qtToWorker_.sizeApprox() >= QtToWorkerCapacity - 1)
    return ChannelSendStatus::QueueFull;
  if (!qtToWorker_.tryEmplace(std::in_place_type<NodeAction>,
                              std::move(action)))
    return ChannelSendStatus::QueueFull;
  return wakeQtToWorker();
}

ChannelSendStatus ThreadChannels::sendRuntimeAction(RuntimeAction &action) {
  if (qtToWorker_.sizeApprox() >= QtToWorkerCapacity - 1)
    return ChannelSendStatus::QueueFull;
  if (!qtToWorker_.tryEmplace(std::in_place_type<RuntimeAction>,
                              std::move(action)))
    return ChannelSendStatus::QueueFull;
  return wakeQtToWorker();
}

ChannelSendStatus ThreadChannels::sendShutdown(ShutdownRequest &request) {
  if (!qtToWorker_.tryEmplace(std::in_place_type<ShutdownRequest>,
                              std::move(request)))
    return ChannelSendStatus::QueueFull;
  return wakeQtToWorker();
}

EventFd::DrainResult ThreadChannels::drainQtToWorkerWake() const noexcept {
  return qtToWorkerWake_.drain();
}

bool ThreadChannels::tryReceiveForWorker(QtToWorkerMessage &message) {
  return qtToWorker_.tryPop(message);
}

std::size_t ThreadChannels::workerToQtSizeApprox() const noexcept {
  return workerToQt_.sizeApprox();
}

std::size_t ThreadChannels::qtToWorkerSizeApprox() const noexcept {
  return qtToWorker_.sizeApprox();
}

bool ThreadChannels::rescanPending() const noexcept {
  return rescanRevision_.load(std::memory_order_acquire) != 0;
}

void ThreadChannels::close() noexcept {
  workerToQtWake_.close();
  qtToWorkerWake_.close();
}

ChannelSendStatus
ThreadChannels::wakeWorkerToQt(bool coalesced) const noexcept {
  const EventFd::NotifyResult wake = workerToQtWake_.notify();
  if (wake.accepted())
    return coalesced ? ChannelSendStatus::CoalescedRescan
                     : ChannelSendStatus::Accepted;
  return coalesced ? ChannelSendStatus::CoalescedRescanWakeFailed
                   : ChannelSendStatus::AcceptedWakeFailed;
}

ChannelSendStatus ThreadChannels::wakeQtToWorker() const noexcept {
  return qtToWorkerWake_.notify().accepted()
             ? ChannelSendStatus::Accepted
             : ChannelSendStatus::AcceptedWakeFailed;
}

void ThreadChannels::requireRescan(std::uint64_t revision) noexcept {
  std::uint64_t observed = rescanRevision_.load(std::memory_order_relaxed);
  while (observed < revision &&
         !rescanRevision_.compare_exchange_weak(observed, revision,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
  }
}

} // namespace codexui::nodegraph
