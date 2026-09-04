// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_WORKERMAILBOXRECEIVER_H
#define CODEXUI_CODEX_WORKERMAILBOXRECEIVER_H

#include "codex/nodegraph/Messages.h"
#include "codex/nodegraph/ThreadChannels.h"

#include <core/eventreceiver/ReadEventReceiver.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace codexui::codex {

// Observes the Qt-to-worker eventfd on the existing SNode.C event loop. The
// receiver borrows ThreadChannels, never owns or closes its eventfd, and
// destroys itself after SNode.C has finished unregistering the descriptor.
class WorkerMailboxReceiver final
    : private core::eventreceiver::ReadEventReceiver {
public:
  using MessageHandler = std::function<void(nodegraph::QtToWorkerMessage)>;
  using FailureHandler = std::function<void(std::string)>;

  // Must be called on the SNode.C worker thread. The returned pointer remains
  // valid only until close() starts deferred destruction.
  [[nodiscard]] static WorkerMailboxReceiver *
  create(nodegraph::ThreadChannels &channels, MessageHandler onMessage,
         FailureHandler onFailure);

  WorkerMailboxReceiver(const WorkerMailboxReceiver &) = delete;
  WorkerMailboxReceiver &operator=(const WorkerMailboxReceiver &) = delete;

  // Must be called on the SNode.C worker thread. ThreadChannels must outlive
  // the receiver and be closed only after the worker event loop has stopped.
  void close();

private:
  static constexpr std::size_t MaximumMessagesPerEvent = 64;

  WorkerMailboxReceiver(nodegraph::ThreadChannels &channels,
                        MessageHandler onMessage, FailureHandler onFailure);
  ~WorkerMailboxReceiver() override;

  void readEvent() override;
  void unobservedEvent() override;
  void destruct() override;
  void shutdownEvent(const core::ShutdownContext &context) override;

  void consumeWakeAndMessages();
  void scheduleNextDrain();
  void invalidateScheduledDrain() noexcept;
  void fail(std::string reason) noexcept;

  nodegraph::ThreadChannels &channels_;
  MessageHandler onMessage_;
  FailureHandler onFailure_;
  std::shared_ptr<WorkerMailboxReceiver *> deferredReceiver_;
  bool drainScheduled_ = false;
  bool closing_ = false;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_WORKERMAILBOXRECEIVER_H
