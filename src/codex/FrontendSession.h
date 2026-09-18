// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_FRONTENDSESSION_H
#define CODEXUI_CODEX_FRONTENDSESSION_H

#include "codex/nodegraph/Messages.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/nodegraph/ThreadChannels.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

class QSocketNotifier;
class QTimer;

namespace codexui::codex {

class Configuration;
class FrontendSessionTestPeer;

// Owns the one SNode.C worker and the two typed eventfd-backed mailboxes. This
// object lives on Qt-main; only the worker passed to runClientRuntime writes
// the shared graph.
class FrontendSession final {
public:
  using RuntimeStoppedHandler = std::function<void()>;
  using GraphChangedHandler =
      std::function<bool(const nodegraph::GraphChanged &)>;
  using ProtocolDiagnosticHandler =
      std::function<void(const nodegraph::ProtocolDiagnostic &)>;

  explicit FrontendSession(Configuration &configuration);
  ~FrontendSession();

  FrontendSession(const FrontendSession &) = delete;
  FrontendSession &operator=(const FrontendSession &) = delete;

  void start(bool connectBridge = true);
  void wait();
  void shutdown();

  void setRuntimeStoppedHandler(RuntimeStoppedHandler handler);
  void setGraphChangedHandler(GraphChangedHandler handler);
  void setProtocolDiagnosticHandler(ProtocolDiagnosticHandler handler);

  // Qt receives read-only access and must use NodeGraph::tryRead().
  [[nodiscard]] const nodegraph::NodeGraph &nodeGraph() const noexcept;

  // On QueueFull newly authored input remains in the widget for visible
  // rejection. On live mailbox saturation the idempotent PromptMaterialized
  // acknowledgement is instead retained internally and reported as accepted.
  [[nodiscard]] nodegraph::ChannelSendStatus
  sendNodeAction(nodegraph::NodeAction &action);
  [[nodiscard]] nodegraph::ChannelSendStatus
  sendRuntimeAction(nodegraph::RuntimeAction &action);

  // Presentation owners call this only after every Qt reference to the
  // retired identities has been detached. The worker keeps the retired graph
  // nodes readable until this explicit lifetime acknowledgement arrives.
  void acknowledgeUiDetached(std::span<const nodegraph::NodeRef> nodes);

private:
  friend class FrontendSessionTestPeer;

  void drainWorkerMessages();
  [[nodiscard]] bool graphDeliveryQuiescent() const noexcept;
  void scheduleWorkerMessageDrain();
  void requireRescanRetirementCollection();
  void collectRescanRetirements();
  void
  queueRetirementAcknowledgements(std::span<const nodegraph::NodeRef> nodes);
  void queueNodeAcknowledgement(nodegraph::NodeAction action);
  void flushNodeAcknowledgements();
  void notifyRuntimeStopped() noexcept;

  nodegraph::NodeGraph graph;
  nodegraph::ThreadChannels channels;
  std::unique_ptr<QSocketNotifier> workerNotifier;
  std::unique_ptr<QTimer> workerWakeRecoveryTimer;
  std::thread clientThread;
  RuntimeStoppedHandler runtimeStoppedHandler;
  GraphChangedHandler graphChangedHandler;
  ProtocolDiagnosticHandler protocolDiagnosticHandler;
  std::deque<nodegraph::GraphChanged> deferredGraphChanges;
  std::vector<nodegraph::NodeAction> pendingNodeAcknowledgements;
  std::unordered_map<nodegraph::Node *, std::uint8_t>
      pendingNodeAcknowledgementKinds;
  std::size_t retirementScanOffset = 0;
  std::uint64_t retirementScanOrderGeneration = 0;
  std::atomic_bool workerFinished{false};
  bool started = false;
  bool stopping = false;
  bool runtimeStopReported = false;
  bool workerDrainScheduled = false;
  bool rescanRetirementPending = false;
  bool retirementScanGenerationKnown = false;
  std::uint64_t nextUiActionCorrelation = 1;
  Configuration &configuration;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_FRONTENDSESSION_H
