// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/NodeGraph.h"
#include "codex/nodegraph/SpscQueue.h"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using codexui::nodegraph::GraphChange;
using codexui::nodegraph::NodeGraph;
using codexui::nodegraph::NodeId;
using codexui::nodegraph::NodeKind;
using codexui::nodegraph::NodeRef;
using codexui::nodegraph::NodeState;
using codexui::nodegraph::SpscQueue;
using codexui::nodegraph::Value;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

const std::uint64_t *
unsignedField(const std::shared_ptr<const NodeState> &state,
              std::string_view key) {
  if (!state)
    return nullptr;
  const auto found = state->fields.find(key);
  if (found == state->fields.end())
    return nullptr;
  return found->second.asUInt64();
}

void readNeverWaitsForWriter() {
  NodeGraph graph;
  std::barrier phase(2);
  std::atomic<bool> readerWasRejected{false};

  std::thread writer([&] {
    auto write = graph.write();
    static_cast<void>(
        write.upsert(NodeId{NodeKind::Runtime, "runtime-under-write"}));
    phase.arrive_and_wait();
    phase.arrive_and_wait();
    static_cast<void>(write.finish());
  });

  phase.arrive_and_wait();
  {
    auto read = graph.tryRead();
    readerWasRejected.store(!read.has_value(), std::memory_order_release);
  }
  phase.arrive_and_wait();
  writer.join();

  expect(readerWasRejected.load(std::memory_order_acquire),
         "tryRead rejects immediately while WriteAccess owns the graph");
}

void correlatedStateIsPublishedAtomically() {
  constexpr std::uint64_t Iterations = 5'000;

  NodeGraph graph;
  NodeRef node;
  std::uint64_t initialRevision = 0;
  {
    auto write = graph.write();
    node = write.upsert(NodeId{NodeKind::Thread, "atomic-thread"},
                        NodeState{.fields = {{"left", std::uint64_t{0}},
                                             {"right", std::uint64_t{0}}}});
    initialRevision = write.finish().revision;
  }

  std::barrier phase(2);
  std::atomic<bool> valid{true};

  std::thread writer([&] {
    for (std::uint64_t sequence = 1; sequence <= Iterations; ++sequence) {
      auto write = graph.write();
      write.setField(node, "left", sequence);

      // Let the Qt-like reader attempt a non-blocking read while the update is
      // deliberately incomplete and the write lock is still held.
      phase.arrive_and_wait();
      phase.arrive_and_wait();

      write.setField(node, "right", sequence);
      const GraphChange change = write.finish();
      if (change.revision != initialRevision + sequence ||
          change.affected.size() != 1 || change.affected.front() != node)
        valid.store(false, std::memory_order_relaxed);

      // Keep the next write transaction from starting until the reader has
      // inspected this complete revision.
      phase.arrive_and_wait();
      phase.arrive_and_wait();
    }
  });

  std::thread reader([&] {
    for (std::uint64_t sequence = 1; sequence <= Iterations; ++sequence) {
      phase.arrive_and_wait();
      {
        auto unavailable = graph.tryRead();
        if (unavailable)
          valid.store(false, std::memory_order_relaxed);
      }
      phase.arrive_and_wait();

      phase.arrive_and_wait();
      {
        auto read = graph.tryRead();
        if (!read) {
          valid.store(false, std::memory_order_relaxed);
        } else {
          const auto state = read->state(node);
          const std::uint64_t *left = unsignedField(state, "left");
          const std::uint64_t *right = unsignedField(state, "right");
          if (!left || !right || *left != sequence || *right != sequence ||
              read->revision() != initialRevision + sequence ||
              read->changedRevision(node) != read->revision())
            valid.store(false, std::memory_order_relaxed);
        }
      }
      phase.arrive_and_wait();
    }
  });

  writer.join();
  reader.join();

  expect(valid.load(std::memory_order_relaxed),
         "readers see either no lock or one complete correlated revision");
  expect(graph.publishedRevision() == initialRevision + Iterations,
         "each correlated transaction publishes exactly one revision");
}

void pinsAndNodeRefsOutliveReplacementAndRemoval() {
  NodeGraph graph;
  NodeRef node;
  {
    auto write = graph.write();
    node =
        write.upsert(NodeId{NodeKind::Item, "pinned-item"},
                     NodeState{.fields = {{"generation", std::uint64_t{1}}}});
    static_cast<void>(write.finish());
  }

  std::shared_ptr<const NodeState> pinnedState;
  {
    auto read = graph.tryRead();
    if (read)
      pinnedState = read->state(node);
  }
  std::weak_ptr<const NodeState> weakState = pinnedState;
  std::weak_ptr<codexui::nodegraph::Node> weakNode = node;

  std::barrier phase(2);
  std::atomic<bool> writerValid{true};
  std::thread writer([&, workerNode = node] {
    phase.arrive_and_wait();
    {
      auto write = graph.write();
      write.replaceState(
          workerNode, NodeState{.fields = {{"generation", std::uint64_t{2}}}});
      const GraphChange replaced = write.finish();
      if (replaced.affected.size() != 1 ||
          replaced.affected.front() != workerNode)
        writerValid.store(false, std::memory_order_relaxed);
    }
    phase.arrive_and_wait();
    phase.arrive_and_wait();

    GraphChange removed;
    {
      auto write = graph.write();
      write.remove(workerNode);
      removed = write.finish();
    }
    if (removed.removed.size() != 1 || removed.removed.front() != workerNode)
      writerValid.store(false, std::memory_order_relaxed);
    {
      auto write = graph.write();
      write.releaseRetired(removed.removed);
      static_cast<void>(write.finish());
    }
    phase.arrive_and_wait();
  });

  phase.arrive_and_wait();
  phase.arrive_and_wait();
  const std::uint64_t *initialGeneration =
      unsignedField(pinnedState, "generation");
  expect(initialGeneration && *initialGeneration == 1,
         "an immutable state pin survives concurrent state replacement");
  expect(node && node->id().canonical == "pinned-item" && !weakNode.expired() &&
             !weakState.expired(),
         "a stable NodeRef and state pin remain alive after replacement");
  phase.arrive_and_wait();
  phase.arrive_and_wait();
  writer.join();

  expect(writerValid.load(std::memory_order_relaxed),
         "the worker replaced, removed, and retired the same NodeRef");
  {
    auto read = graph.tryRead();
    expect(read && !read->find(NodeId{NodeKind::Item, "pinned-item"}) &&
               read->retiredNodes().empty(),
           "removal unlinks the node and releases graph ownership");
  }
  initialGeneration = unsignedField(pinnedState, "generation");
  expect(initialGeneration && *initialGeneration == 1 && node &&
             node->id().canonical == "pinned-item",
         "external pins remain valid after removal and retirement release");

  pinnedState.reset();
  expect(weakState.expired(),
         "replaced immutable storage is destroyed after its pin is released");
  node.reset();
  expect(weakNode.expired(),
         "a removed node is destroyed after the final NodeRef is released");
}

struct MoveOnlyPayload final {
  std::uint64_t sequence = 0;
  bool owned = false;

  MoveOnlyPayload() = default;
  explicit MoveOnlyPayload(std::uint64_t value)
      : sequence(value), owned(true) {}

  MoveOnlyPayload(const MoveOnlyPayload &) = delete;
  MoveOnlyPayload &operator=(const MoveOnlyPayload &) = delete;

  MoveOnlyPayload(MoveOnlyPayload &&other) noexcept
      : sequence(other.sequence), owned(std::exchange(other.owned, false)) {}

  MoveOnlyPayload &operator=(MoveOnlyPayload &&other) noexcept {
    if (this == &other)
      return *this;
    sequence = other.sequence;
    owned = std::exchange(other.owned, false);
    return *this;
  }
};

void queuePreservesRejectedAndOrderedMoveOnlyPayloads() {
  {
    SpscQueue<MoveOnlyPayload, 3> queue;
    for (std::uint64_t sequence = 0; sequence < queue.capacity(); ++sequence) {
      MoveOnlyPayload payload(sequence);
      expect(queue.tryPush(std::move(payload)),
             "an available queue slot admits a move-only payload");
      expect(!payload.owned, "an admitted payload transfers ownership once");
    }
    expect(queue.full() && queue.sizeApprox() == queue.capacity(),
           "all declared queue slots are usable");

    MoveOnlyPayload rejected(99);
    expect(!queue.tryPush(std::move(rejected)),
           "a full queue explicitly rejects a payload");
    expect(rejected.owned && rejected.sequence == 99,
           "full rejection leaves the producer-owned payload intact");

    for (std::uint64_t sequence = 0; sequence < queue.capacity(); ++sequence) {
      MoveOnlyPayload received;
      expect(queue.tryPop(received) && received.owned &&
                 received.sequence == sequence,
             "the bounded queue retains FIFO order");
    }
    expect(queue.empty(), "the bounded queue is empty after every pop");
  }

  constexpr std::uint64_t Transfers = 1'250'000;
  SpscQueue<MoveOnlyPayload, 257> queue;
  std::barrier launch(2);
  std::atomic<bool> valid{true};

  std::thread producer([&] {
    launch.arrive_and_wait();
    for (std::uint64_t sequence = 0; sequence < Transfers; ++sequence) {
      MoveOnlyPayload payload(sequence);
      while (!queue.tryPush(std::move(payload))) {
        if (!payload.owned || payload.sequence != sequence)
          valid.store(false, std::memory_order_relaxed);
      }
      if (payload.owned)
        valid.store(false, std::memory_order_relaxed);
    }
  });

  std::thread consumer([&] {
    launch.arrive_and_wait();
    for (std::uint64_t expected = 0; expected < Transfers; ++expected) {
      MoveOnlyPayload received;
      while (!queue.tryPop(received)) {
      }
      if (!received.owned || received.sequence != expected)
        valid.store(false, std::memory_order_relaxed);
    }
  });

  producer.join();
  consumer.join();

  expect(valid.load(std::memory_order_relaxed),
         "at least one million move-only payloads cross exactly once in order");
  expect(queue.empty() && queue.sizeApprox() == 0,
         "the stressed queue drains completely");
}

} // namespace

int main() {
  readNeverWaitsForWriter();
  correlatedStateIsPublishedAtomically();
  pinsAndNodeRefsOutliveReplacementAndRemoval();
  queuePreservesRejectedAndOrderedMoveOnlyPayloads();

  if (failures != 0)
    std::cerr << failures << " graph concurrency assertion(s) failed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
