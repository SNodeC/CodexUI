// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/WorkerMailboxReceiver.h"

#include <Log.h>
#include <core/EventReceiver.h>

#include <utility>

namespace codexui::codex {
namespace {

constexpr double WakeRecoveryIntervalSeconds = 0.1;

snode::log::Scope makeLogScope() {
  return {.origin = snode::log::Origin::Application,
          .boundary = snode::log::Boundary::Application,
          .component = "codexui.nodegraph",
          .identity = {.instance = "worker-mailbox",
                       .role = snode::log::Role::Client}};
}

} // namespace

WorkerMailboxReceiver *
WorkerMailboxReceiver::create(nodegraph::ThreadChannels &channels,
                              MessageHandler onMessage,
                              FailureHandler onFailure) {
  if (!onMessage || channels.qtToWorkerEventFd() < 0)
    return nullptr;

  auto *receiver = new WorkerMailboxReceiver(channels, std::move(onMessage),
                                             std::move(onFailure));
  if (!receiver->ReadEventReceiver::enable(channels.qtToWorkerEventFd())) {
    delete receiver;
    return nullptr;
  }
  return receiver;
}

WorkerMailboxReceiver::WorkerMailboxReceiver(
    nodegraph::ThreadChannels &channels, MessageHandler onMessage,
    FailureHandler onFailure)
    : core::eventreceiver::ReadEventReceiver(
          "CodexUI worker mailbox", makeLogScope(),
          utils::Timeval(WakeRecoveryIntervalSeconds)),
      channels_(channels), onMessage_(std::move(onMessage)),
      onFailure_(std::move(onFailure)),
      deferredReceiver_(std::make_shared<WorkerMailboxReceiver *>(this)) {}

WorkerMailboxReceiver::~WorkerMailboxReceiver() { invalidateScheduledDrain(); }

void WorkerMailboxReceiver::close() {
  if (closing_)
    return;
  closing_ = true;
  invalidateScheduledDrain();
  if (ReadEventReceiver::isEnabled())
    ReadEventReceiver::disable();
}

void WorkerMailboxReceiver::readEvent() { consumeWakeAndMessages(true); }

void WorkerMailboxReceiver::readTimeout() {
  // The eventfd is the normal notification path. This bounded timeout is only
  // a recovery path for a payload admitted immediately before a failed wake,
  // including ShutdownRequest, so the worker cannot sleep indefinitely.
  if (channels_.qtToWorkerSizeApprox() != 0)
    consumeWakeAndMessages(false);

  // SNode.C descriptor inactivity timeouts remain expired until explicitly
  // rearmed. Leaving this receiver expired makes the worker event loop poll
  // with a zero timeout forever after the first idle 100 ms. Rearm the narrow
  // wake-failure safety net after every timeout so an idle worker sleeps.
  if (!closing_)
    setTimeout(utils::Timeval(WakeRecoveryIntervalSeconds));
}

void WorkerMailboxReceiver::unobservedEvent() {
  invalidateScheduledDrain();
  delete this;
}

void WorkerMailboxReceiver::destruct() { close(); }

void WorkerMailboxReceiver::shutdownEvent(
    const core::ShutdownContext &context) {
  static_cast<void>(context);
  close();
}

void WorkerMailboxReceiver::consumeWakeAndMessages(bool drainWake) {
  if (closing_)
    return;

  if (drainWake) {
    const nodegraph::EventFd::DrainResult wake =
        channels_.drainQtToWorkerWake();
    if (!wake.accepted()) {
      fail("Qt-to-worker eventfd failed while draining");
      return;
    }
  }

  std::size_t consumed = 0;
  while (!closing_ && consumed < MaximumMessagesPerEvent) {
    nodegraph::QtToWorkerMessage message;
    if (!channels_.tryReceiveForWorker(message))
      return;

    ++consumed;
    try {
      // tryReceiveForWorker has already moved the message out of the queue and
      // released its slot before application logic is entered.
      onMessage_(std::move(message));
    } catch (...) {
      fail("Qt-to-worker action handler threw an exception");
      return;
    }
  }

  if (!closing_ && channels_.qtToWorkerSizeApprox() != 0)
    scheduleNextDrain();
}

void WorkerMailboxReceiver::scheduleNextDrain() {
  if (closing_ || drainScheduled_)
    return;
  drainScheduled_ = true;

  const std::weak_ptr<WorkerMailboxReceiver *> weak = deferredReceiver_;
  try {
    core::EventReceiver::atNextTick([weak] {
      const std::shared_ptr<WorkerMailboxReceiver *> receiver = weak.lock();
      if (!receiver || *receiver == nullptr)
        return;
      (*receiver)->drainScheduled_ = false;
      (*receiver)->consumeWakeAndMessages(false);
    });
  } catch (...) {
    drainScheduled_ = false;
    fail("SNode.C rejected deferred Qt-to-worker mailbox draining");
  }
}

void WorkerMailboxReceiver::fail(std::string reason) noexcept {
  if (closing_)
    return;
  close();
  if (!onFailure_)
    return;
  try {
    onFailure_(std::move(reason));
  } catch (...) {
    // The receiver is already closed. Failure reporting must never revive the
    // only mailbox consumer or prevent the worker shutdown path.
  }
}

void WorkerMailboxReceiver::invalidateScheduledDrain() noexcept {
  if (deferredReceiver_)
    *deferredReceiver_ = nullptr;
  deferredReceiver_.reset();
  drainScheduled_ = false;
}

} // namespace codexui::codex
