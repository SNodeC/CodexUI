// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/FrontendSession.h"

#include "codex/ClientRuntime.h"

#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace codexui::codex {

FrontendSession::FrontendSession(Configuration &configuration)
    : configuration(configuration) {
  if (!channels.valid()) {
    const int error = channels.workerToQtCreationError() != 0
                          ? channels.workerToQtCreationError()
                          : channels.qtToWorkerCreationError();
    throw std::system_error(error != 0 ? error : EIO, std::generic_category(),
                            "unable to create CodexUI eventfds");
  }

  workerNotifier = std::make_unique<QSocketNotifier>(
      channels.workerToQtEventFd(), QSocketNotifier::Read);
  QObject::connect(workerNotifier.get(), &QSocketNotifier::activated,
                   workerNotifier.get(), [this] { drainWorkerMessages(); });
}

FrontendSession::~FrontendSession() { shutdown(); }

void FrontendSession::start(bool connectBridge) {
  if (started || stopping)
    return;
  started = true;
  clientThread = std::thread([this, connectBridge] {
    static_cast<void>(
        runClientRuntime(configuration, graph, channels, connectBridge));
    workerFinished.store(true, std::memory_order_release);
  });
}

void FrontendSession::wait() {
  if (clientThread.joinable())
    clientThread.join();
}

void FrontendSession::shutdown() {
  if (stopping)
    return;
  stopping = true;

  if (workerNotifier)
    workerNotifier->setEnabled(false);

  if (started && !workerFinished.load(std::memory_order_acquire)) {
    nodegraph::ShutdownRequest request;
    while (!workerFinished.load(std::memory_order_acquire)) {
      const nodegraph::ChannelSendStatus status =
          channels.sendShutdown(request);
      if (nodegraph::messageAdmitted(status))
        break;
      // The bounded queue preserves FIFO ordering. The worker is its sole
      // consumer, so yielding until it admits shutdown cannot duplicate or
      // silently discard any already-admitted user action.
      std::this_thread::yield();
    }
  }

  wait();
  workerNotifier.reset();
  channels.close();
}

void FrontendSession::setRuntimeStoppedHandler(RuntimeStoppedHandler handler) {
  runtimeStoppedHandler = std::move(handler);
}

void FrontendSession::setGraphChangedHandler(GraphChangedHandler handler) {
  graphChangedHandler = std::move(handler);
}

void FrontendSession::setGraphUiEffectHandler(GraphUiEffectHandler handler) {
  graphUiEffectHandler = std::move(handler);
}

const nodegraph::NodeGraph &FrontendSession::nodeGraph() const noexcept {
  return graph;
}

nodegraph::ChannelSendStatus
FrontendSession::sendNodeAction(nodegraph::NodeAction &action) {
  if (stopping || (started && workerFinished.load(std::memory_order_acquire)))
    return nodegraph::ChannelSendStatus::QueueFull;
  return channels.sendNodeAction(action);
}

nodegraph::ChannelSendStatus
FrontendSession::sendRuntimeAction(nodegraph::RuntimeAction &action) {
  if (stopping || (started && workerFinished.load(std::memory_order_acquire)))
    return nodegraph::ChannelSendStatus::QueueFull;
  return channels.sendRuntimeAction(action);
}

void FrontendSession::drainWorkerMessages() {
  if (stopping)
    return;

  static_cast<void>(channels.drainWorkerToQtWake());
  if (rescanRetirementPending)
    collectRescanRetirements();

  constexpr std::size_t MaximumMessagesPerPass = 128;
  std::size_t processed = 0;
  nodegraph::WorkerToQtMessage message;
  while (processed < MaximumMessagesPerPass &&
         channels.tryReceiveForQt(message)) {
    ++processed;
    std::visit(
        [this](auto &payload) {
          using Message = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Message, nodegraph::GraphChanged>) {
            if (graphChangedHandler) {
              try {
                graphChangedHandler(payload);
              } catch (...) {
              }
            }
            collectDetachedNodes(payload.removed);
            if (payload.rescanRequired) {
              rescanRetirementPending = true;
              collectRescanRetirements();
            }
          } else if constexpr (std::is_same_v<Message, nodegraph::UiEffect>) {
            if (graphUiEffectHandler) {
              try {
                graphUiEffectHandler(payload);
              } catch (...) {
              }
            }
          } else {
            notifyRuntimeStopped();
          }
        },
        message);
  }

  flushDetachAcknowledgements();
  if (channels.workerToQtSizeApprox() != 0 || channels.rescanPending() ||
      rescanRetirementPending || !pendingDetachAcknowledgements.empty())
    scheduleWorkerMessageDrain();
}

void FrontendSession::scheduleWorkerMessageDrain() {
  if (workerDrainScheduled || stopping)
    return;
  workerDrainScheduled = true;
  QTimer::singleShot(0, workerNotifier.get(), [this] {
    workerDrainScheduled = false;
    drainWorkerMessages();
  });
}

void FrontendSession::collectRescanRetirements() {
  auto read = graph.tryRead();
  if (!read) {
    scheduleWorkerMessageDrain();
    return;
  }

  std::vector<nodegraph::NodeRef> retired(read->retiredNodes().begin(),
                                          read->retiredNodes().end());
  const std::uint64_t revision = read->revision();
  read.reset();

  if (graphChangedHandler && !retired.empty()) {
    try {
      graphChangedHandler(nodegraph::GraphChanged{revision, {}, retired, true});
    } catch (...) {
    }
  }
  collectDetachedNodes(retired);

  rescanRetirementPending =
      std::ranges::any_of(retired, [](const nodegraph::NodeRef &node) {
        return node && node->uiAttachment() != nullptr;
      });
}

void FrontendSession::collectDetachedNodes(
    std::span<const nodegraph::NodeRef> nodes) {
  for (const nodegraph::NodeRef &node : nodes) {
    // The graph callback above runs on Qt-main and must destroy/clear any
    // QWidget attachment before its removal can be acknowledged to worker.
    if (!node)
      continue;
    if (node->uiAttachment() != nullptr) {
      rescanRetirementPending = true;
      continue;
    }
    if (std::find(pendingDetachAcknowledgements.begin(),
                  pendingDetachAcknowledgements.end(),
                  node) == pendingDetachAcknowledgements.end())
      pendingDetachAcknowledgements.emplace_back(node);
  }
}

void FrontendSession::flushDetachAcknowledgements() {
  while (!pendingDetachAcknowledgements.empty()) {
    if (pendingDetachAcknowledgements.back()->uiAttachment() != nullptr) {
      rescanRetirementPending = true;
      return;
    }
    nodegraph::NodeAction action;
    action.target = pendingDetachAcknowledgements.back();
    action.kind = nodegraph::NodeActionKind::UiDetached;
    const nodegraph::ChannelSendStatus status = channels.sendNodeAction(action);
    if (!nodegraph::messageAdmitted(status))
      return;
    pendingDetachAcknowledgements.pop_back();
  }
}

void FrontendSession::notifyRuntimeStopped() noexcept {
  if (runtimeStopReported)
    return;
  runtimeStopReported = true;
  if (runtimeStoppedHandler) {
    try {
      runtimeStoppedHandler();
    } catch (...) {
    }
  }
}

} // namespace codexui::codex
