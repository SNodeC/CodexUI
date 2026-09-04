// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_FRONTENDSESSION_H
#define CODEXUI_CODEX_FRONTENDSESSION_H

#include "codex/nodegraph/Messages.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/nodegraph/ThreadChannels.h"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>

class QSocketNotifier;

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
      std::function<void(const nodegraph::GraphChanged &)>;
  using GraphUiEffectHandler = std::function<void(const nodegraph::UiEffect &)>;

  explicit FrontendSession(Configuration &configuration);
  ~FrontendSession();

  FrontendSession(const FrontendSession &) = delete;
  FrontendSession &operator=(const FrontendSession &) = delete;

  void start(bool connectBridge = true);
  void wait();
  void shutdown();

  void setRuntimeStoppedHandler(RuntimeStoppedHandler handler);
  void setGraphChangedHandler(GraphChangedHandler handler);
  void setGraphUiEffectHandler(GraphUiEffectHandler handler);

  // Qt receives read-only access and must use NodeGraph::tryRead().
  [[nodiscard]] const nodegraph::NodeGraph &nodeGraph() const noexcept;

  // On QueueFull the action is untouched, so newly authored input remains in
  // the widget and can be rejected visibly by its caller.
  [[nodiscard]] nodegraph::ChannelSendStatus
  sendNodeAction(nodegraph::NodeAction &action);
  [[nodiscard]] nodegraph::ChannelSendStatus
  sendRuntimeAction(nodegraph::RuntimeAction &action);

private:
  friend class FrontendSessionTestPeer;

  void drainWorkerMessages();
  void scheduleWorkerMessageDrain();
  void requireRescanRetirementCollection();
  void collectRescanRetirements();
  void collectDetachedNodes(std::span<const nodegraph::NodeRef> nodes);
  void flushDetachAcknowledgements();
  void notifyRuntimeStopped() noexcept;

  nodegraph::NodeGraph graph;
  nodegraph::ThreadChannels channels;
  std::unique_ptr<QSocketNotifier> workerNotifier;
  std::thread clientThread;
  RuntimeStoppedHandler runtimeStoppedHandler;
  GraphChangedHandler graphChangedHandler;
  GraphUiEffectHandler graphUiEffectHandler;
  std::vector<nodegraph::NodeRef> pendingDetachAcknowledgements;
  std::unordered_set<nodegraph::Node *> pendingDetachAcknowledgementIndex;
  std::size_t retirementScanOffset = 0;
  std::uint64_t retirementScanOrderGeneration = 0;
  std::atomic_bool workerFinished{false};
  bool started = false;
  bool stopping = false;
  bool runtimeStopReported = false;
  bool workerDrainScheduled = false;
  bool rescanRetirementPending = false;
  bool retirementScanGenerationKnown = false;
  bool retirementRetryNeeded = false;
  Configuration &configuration;
};

} // namespace codexui::codex

#endif // CODEXUI_CODEX_FRONTENDSESSION_H
