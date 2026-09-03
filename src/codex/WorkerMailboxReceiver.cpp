// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/WorkerMailboxReceiver.h"

#include <Log.h>
#include <core/EventReceiver.h>

#include <utility>

namespace codexui::codex {
namespace {

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
                              MessageHandler onMessage) {
  if (!onMessage || channels.qtToWorkerEventFd() < 0)
    return nullptr;

  auto *receiver = new WorkerMailboxReceiver(channels, std::move(onMessage));
  if (!receiver->ReadEventReceiver::enable(channels.qtToWorkerEventFd())) {
    delete receiver;
    return nullptr;
  }
  return receiver;
}

WorkerMailboxReceiver::WorkerMailboxReceiver(
    nodegraph::ThreadChannels &channels, MessageHandler onMessage)
    : core::eventreceiver::ReadEventReceiver("CodexUI worker mailbox",
                                             makeLogScope(), TIMEOUT::DISABLE),
      channels_(channels), onMessage_(std::move(onMessage)),
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

void WorkerMailboxReceiver::readEvent() { consumeWakeAndMessages(); }

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

void WorkerMailboxReceiver::consumeWakeAndMessages() {
  if (closing_)
    return;

  const nodegraph::EventFd::DrainResult wake = channels_.drainQtToWorkerWake();
  if (!wake.accepted()) {
    close();
    return;
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
      close();
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
      (*receiver)->consumeWakeAndMessages();
    });
  } catch (...) {
    drainScheduled_ = false;
    close();
  }
}

void WorkerMailboxReceiver::invalidateScheduledDrain() noexcept {
  if (deferredReceiver_)
    *deferredReceiver_ = nullptr;
  deferredReceiver_.reset();
  drainScheduled_ = false;
}

} // namespace codexui::codex
