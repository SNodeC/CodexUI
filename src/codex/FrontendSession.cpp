// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/FrontendSession.h"

#include "codex/ClientRuntime.h"

#include <QElapsedTimer>
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
  workerWakeRecoveryTimer = std::make_unique<QTimer>();
  workerWakeRecoveryTimer->setInterval(100);
  QObject::connect(workerWakeRecoveryTimer.get(), &QTimer::timeout,
                   workerWakeRecoveryTimer.get(), [this] {
                     if (!stopping &&
                         (channels.workerToQtSizeApprox() != 0 ||
                          channels.rescanPending() ||
                          workerFinished.load(std::memory_order_acquire)))
                       drainWorkerMessages();
                   });
  workerWakeRecoveryTimer->start();
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
  if (workerWakeRecoveryTimer)
    workerWakeRecoveryTimer->stop();

  if (started && !workerFinished.load(std::memory_order_acquire)) {
    nodegraph::ShutdownRequest request;
    while (!workerFinished.load(std::memory_order_acquire)) {
      const nodegraph::ChannelSendStatus status =
          channels.sendShutdown(request);
      if (nodegraph::deliveryGuaranteed(status))
        break;
      // The bounded queue preserves FIFO ordering. The worker is its sole
      // consumer, so yielding until it admits shutdown cannot duplicate or
      // silently discard any already-admitted user action.
      std::this_thread::yield();
    }
  }

  wait();
  workerNotifier.reset();
  workerWakeRecoveryTimer.reset();
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
  const bool assignedCorrelation = action.correlation.empty();
  if (assignedCorrelation)
    action.correlation =
        "ui-action-" + std::to_string(nextUiActionCorrelation++);
  const nodegraph::ChannelSendStatus status = channels.sendNodeAction(action);
  if (status == nodegraph::ChannelSendStatus::QueueFull) {
    if (assignedCorrelation)
      action.correlation.clear();
    if (action.kind == nodegraph::NodeActionKind::PromptMaterialized) {
      queueNodeAcknowledgement(std::move(action));
      scheduleWorkerMessageDrain();
      return nodegraph::ChannelSendStatus::Accepted;
    }
  }
  return status;
}

nodegraph::ChannelSendStatus
FrontendSession::sendRuntimeAction(nodegraph::RuntimeAction &action) {
  if (stopping || (started && workerFinished.load(std::memory_order_acquire)))
    return nodegraph::ChannelSendStatus::QueueFull;
  const bool assignedCorrelation = action.correlation.empty();
  if (assignedCorrelation)
    action.correlation =
        "ui-action-" + std::to_string(nextUiActionCorrelation++);
  const nodegraph::ChannelSendStatus status =
      channels.sendRuntimeAction(action);
  if (status == nodegraph::ChannelSendStatus::QueueFull && assignedCorrelation)
    action.correlation.clear();
  return status;
}

void FrontendSession::drainWorkerMessages() {
  if (stopping)
    return;

  const nodegraph::EventFd::DrainResult wake = channels.drainWorkerToQtWake();
  if (!wake.accepted()) {
    if (workerNotifier)
      workerNotifier->setEnabled(false);
    if (graphUiEffectHandler) {
      try {
        graphUiEffectHandler(nodegraph::UiEffect{
            nodegraph::UiEffectKind::ShowNotice,
            std::nullopt,
            "Worker-to-Qt wake-up failed; CodexUI is shutting down",
            {}});
      } catch (...) {
      }
    }
    // The application quit path calls shutdown(), which uses the independently
    // owned Qt-to-worker eventfd before joining the worker.
    notifyRuntimeStopped();
    return;
  }
  if (rescanRetirementPending)
    collectRescanRetirements();

  constexpr std::size_t MaximumMessagesPerPass = 128;
  constexpr qint64 MaximumDrainMilliseconds = 2;
  QElapsedTimer drainBudget;
  drainBudget.start();
  std::size_t processed = 0;
  nodegraph::WorkerToQtMessage message;
  while (processed < MaximumMessagesPerPass &&
         (processed == 0 || drainBudget.elapsed() < MaximumDrainMilliseconds) &&
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
            queueRetirementAcknowledgements(payload.removed);
            if (payload.rescanRequired) {
              requireRescanRetirementCollection();
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

  // A synthesized rescan is deliberately delivered ahead of older queued
  // notifications. Keep retired nodes graph-readable until that entire older
  // backlog has passed Qt; queued NodeRefs alone pin lifetime but do not keep
  // ReadAccess membership after releaseRetired().
  const bool workerBacklogDrained =
      channels.workerToQtSizeApprox() == 0 && !channels.rescanPending();
  if (workerBacklogDrained && !rescanRetirementPending)
    flushNodeAcknowledgements();
  if (workerFinished.load(std::memory_order_acquire))
    notifyRuntimeStopped();
  if (!workerBacklogDrained || rescanRetirementPending ||
      !pendingNodeAcknowledgements.empty())
    scheduleWorkerMessageDrain();
}

void FrontendSession::scheduleWorkerMessageDrain() {
  if (workerDrainScheduled || stopping)
    return;
  workerDrainScheduled = true;
  // Yield through at least one native event-dispatch turn between backlog
  // slices so wheel, key, paint, and socket events cannot be starved by a
  // self-replenishing zero-delay drain loop.
  QTimer::singleShot(1, Qt::PreciseTimer, workerNotifier.get(), [this] {
    workerDrainScheduled = false;
    drainWorkerMessages();
  });
}

void FrontendSession::requireRescanRetirementCollection() {
  if (!rescanRetirementPending) {
    retirementScanOffset = 0;
    retirementScanGenerationKnown = false;
  }
  rescanRetirementPending = true;
}

void FrontendSession::collectRescanRetirements() {
  constexpr std::size_t MaximumRetirementsPerPass = 64;
  auto read = graph.tryRead();
  if (!read) {
    scheduleWorkerMessageDrain();
    return;
  }

  const std::uint64_t orderGeneration = read->retiredOrderGeneration();
  if (!retirementScanGenerationKnown ||
      orderGeneration != retirementScanOrderGeneration) {
    retirementScanOffset = 0;
    retirementScanOrderGeneration = orderGeneration;
    retirementScanGenerationKnown = true;
  }

  const std::size_t count = read->retiredCount();
  retirementScanOffset = std::min(retirementScanOffset, count);
  const std::size_t end =
      std::min(count, retirementScanOffset + MaximumRetirementsPerPass);
  std::vector<nodegraph::NodeRef> retired;
  retired.reserve(end - retirementScanOffset);
  for (std::size_t index = retirementScanOffset; index < end; ++index)
    retired.emplace_back(read->retiredAt(index));
  const std::uint64_t revision = read->revision();
  std::uint64_t providerAuthorityRevision = 0;
  if (const nodegraph::NodeRef connection =
          read->find({nodegraph::NodeKind::Connection, "connection"})) {
    const auto state = read->state(connection);
    providerAuthorityRevision =
        nodegraph::unsignedIntegerFromValue(
            nodegraph::valueMember(*state, "providerAuthorityRevision"))
            .value_or(0);
  }
  std::vector<nodegraph::NodeRef> providerRetired;
  std::vector<nodegraph::NodeRef> ordinaryRetired;
  providerRetired.reserve(retired.size());
  ordinaryRetired.reserve(retired.size());
  for (const nodegraph::NodeRef &node : retired) {
    std::vector<nodegraph::NodeRef> &destination =
        providerAuthorityRevision != 0 &&
                read->changedRevision(node) <= providerAuthorityRevision
            ? providerRetired
            : ordinaryRetired;
    destination.emplace_back(node);
  }
  const bool complete = end == count;
  read.reset();

  if (graphChangedHandler) {
    const auto notify = [this,
                         revision](const std::vector<nodegraph::NodeRef> &nodes,
                                   std::uint64_t authorityRevision) {
      if (nodes.empty())
        return;
      try {
        graphChangedHandler(nodegraph::GraphChanged{
            revision, {}, nodes, false, {}, authorityRevision});
      } catch (...) {
      }
    };
    notify(providerRetired, providerAuthorityRevision);
    notify(ordinaryRetired, 0);
  }
  queueRetirementAcknowledgements(retired);

  if (!complete) {
    retirementScanOffset = end;
    return;
  }

  retirementScanOffset = 0;
  rescanRetirementPending = false;
  retirementScanGenerationKnown = false;
}

void FrontendSession::queueRetirementAcknowledgements(
    std::span<const nodegraph::NodeRef> nodes) {
  for (const nodegraph::NodeRef &node : nodes) {
    if (!node)
      continue;
    queueNodeAcknowledgement({node, nodegraph::NodeActionKind::UiDetached});
  }
}

void FrontendSession::queueNodeAcknowledgement(nodegraph::NodeAction action) {
  if (!action.target)
    return;
  const std::uint8_t kind =
      action.kind == nodegraph::NodeActionKind::UiDetached ? 2 : 1;
  const auto [position, inserted] =
      pendingNodeAcknowledgementKinds.try_emplace(action.target.get(), 0);
  const std::uint8_t previous = position->second;
  if ((previous & kind) != 0)
    return;
  position->second |= kind;
  try {
    pendingNodeAcknowledgements.push_back(std::move(action));
  } catch (...) {
    if (inserted)
      pendingNodeAcknowledgementKinds.erase(position);
    else
      position->second = previous;
    throw;
  }
}

void FrontendSession::flushNodeAcknowledgements() {
  constexpr std::size_t MaximumAcknowledgementsPerPass = 64;
  std::size_t processed = 0;
  while (!pendingNodeAcknowledgements.empty() &&
         processed < MaximumAcknowledgementsPerPass) {
    nodegraph::NodeAction &action = pendingNodeAcknowledgements.back();
    nodegraph::Node *const target = action.target.get();
    const std::uint8_t kind =
        action.kind == nodegraph::NodeActionKind::UiDetached ? 2 : 1;
    const nodegraph::ChannelSendStatus status = channels.sendNodeAction(action);
    if (!nodegraph::deliveryGuaranteed(status))
      return;
    pendingNodeAcknowledgements.pop_back();
    if (auto pending = pendingNodeAcknowledgementKinds.find(target);
        pending != pendingNodeAcknowledgementKinds.end()) {
      pending->second &= static_cast<std::uint8_t>(~kind);
      if (pending->second == 0)
        pendingNodeAcknowledgementKinds.erase(pending);
    }
    ++processed;
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
