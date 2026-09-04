// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_NODEGRAPH_H
#define CODEXUI_CODEX_NODEGRAPH_NODEGRAPH_H

#include "codex/nodegraph/Value.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codexui::nodegraph {

enum class NodeKind : std::uint8_t {
  Runtime,
  Connection,
  Thread,
  Turn,
  Item,
  Interaction,
  Operation,
  Catalog,
  CatalogEntry,
  Account,
  Configuration,
  PermissionProfile,
  Skill,
  Hook,
  Plugin,
  App,
  McpServer,
  Project,
  ThreadSection,
  Process,
  RealtimeSession,
  FilesystemWatch,
  ExternalAgentImport,
  FuzzyFileSearchSession,
  LoginAttempt,
  Notice,
  UnknownProtocol,
};

struct NodeId final {
  NodeKind kind = NodeKind::UnknownProtocol;
  std::string canonical;

  bool operator==(const NodeId &) const = default;
};

struct NodeIdHash final {
  [[nodiscard]] std::size_t operator()(const NodeId &id) const noexcept;
};

enum class NodeStatus : std::uint8_t {
  Unknown,
  Pending,
  Running,
  Completed,
  Failed,
  Interrupted,
  NotLoaded,
  Connected,
  Disconnected,
};

// Protocol facts are retained directly here. The immutable storage is replaced
// as one unit by the sole writer and can be pinned briefly by Qt after a
// successful try-read.
struct NodeState final {
  NodeStatus status = NodeStatus::Unknown;
  Value::Object fields;

  bool operator==(const NodeState &) const = default;
};

enum class RelationKind : std::uint8_t {
  RootThread,
  StructuralChildThread,
  AgentChildThread,
  ThreadOwner,
  ForkChildThread,
  ProjectMembership,
  SectionMembership,
  TurnRootItem,
  OperationTarget,
  InteractionTarget,
  PendingInteraction,
  PendingPrompt,
  PromptMaterialization,
  ProcessOwner,
  ActiveTurn,
  UiSelectionTarget,
};

class Node final : public std::enable_shared_from_this<Node> {
public:
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;

  [[nodiscard]] const NodeId &id() const noexcept;

  // The slot is intentionally untyped and non-owning. Only the Qt main thread
  // may set, clear, or dereference it; the worker never examines it.
  [[nodiscard]] void *uiAttachment() const noexcept;
  void setUiAttachment(void *attachment) noexcept;

private:
  friend class NodeGraph;

  explicit Node(NodeId id, NodeState state, std::uint64_t insertionOrder);

  NodeId id_;
  std::shared_ptr<const NodeState> state_;
  Node *parent_ = nullptr;
  std::vector<Node *> children_;
  std::unordered_map<RelationKind, std::vector<Node *>> relations_;
  std::unordered_map<std::string, std::uint64_t> fieldChangedRevisions_;
  std::uint64_t statusChangedRevision_ = 0;
  // Changes only when this node's parent, ordered children, or an outgoing
  // relation changes. UI scans use the stamp to validate bounded structural
  // reads without treating ordinary streaming fields as topology changes.
  std::uint64_t structureChangedRevision_ = 0;
  std::uint64_t changedRevision_ = 0;
  // Immutable position in the graph's append-only insertion order. Qt uses
  // this only to resume bounded scans after unrelated removals shift the
  // ordered-node vector.
  std::uint64_t insertionOrder_ = 0;
  bool removed_ = false;
  std::atomic<void *> uiAttachment_{nullptr};
};

using NodeRef = std::shared_ptr<Node>;

struct GraphChange final {
  std::uint64_t revision = 0;
  std::vector<NodeRef> affected;
  std::vector<NodeRef> removed;

  [[nodiscard]] bool empty() const noexcept;
};

class NodeGraph final {
public:
  class ReadAccess;
  class WriteAccess;

  NodeGraph() = default;
  NodeGraph(const NodeGraph &) = delete;
  NodeGraph &operator=(const NodeGraph &) = delete;

  [[nodiscard]] WriteAccess write();
  [[nodiscard]] std::optional<ReadAccess> tryRead() const;
  [[nodiscard]] std::uint64_t publishedRevision() const noexcept;
  [[nodiscard]] std::uint64_t
  publishedStructureRevision(NodeKind kind) const noexcept;

  class ReadAccess final {
  public:
    ReadAccess(ReadAccess &&) noexcept = default;
    ReadAccess &operator=(ReadAccess &&) noexcept = default;
    ReadAccess(const ReadAccess &) = delete;
    ReadAccess &operator=(const ReadAccess &) = delete;

    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t structureRevision(NodeKind kind) const noexcept;
    [[nodiscard]] NodeRef find(const NodeId &id) const;
    [[nodiscard]] const std::vector<NodeRef> &orderedNodes() const noexcept;
    [[nodiscard]] std::uint64_t insertionOrder(const NodeRef &node) const;
    [[nodiscard]] const std::vector<NodeRef> &retiredNodes() const noexcept;
    [[nodiscard]] std::size_t retiredCount() const noexcept;
    [[nodiscard]] NodeRef retiredAt(std::size_t index) const;
    // Changes only when an existing retirement is released. Appending a
    // newly removed node preserves every earlier index.
    [[nodiscard]] std::uint64_t retiredOrderGeneration() const noexcept;
    [[nodiscard]] std::shared_ptr<const NodeState>
    state(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t changedRevision(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t
    fieldChangedRevision(const NodeRef &node, std::string_view field) const;
    [[nodiscard]] std::uint64_t
    statusChangedRevision(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t
    structureChangedRevision(const NodeRef &node) const;
    [[nodiscard]] bool removed(const NodeRef &node) const;
    [[nodiscard]] NodeRef parent(const NodeRef &node) const;
    [[nodiscard]] std::size_t childCount(const NodeRef &node) const;
    [[nodiscard]] NodeRef childAt(const NodeRef &node, std::size_t index) const;
    [[nodiscard]] std::vector<NodeRef> children(const NodeRef &node) const;
    [[nodiscard]] std::size_t relatedCount(const NodeRef &node,
                                           RelationKind kind) const;
    [[nodiscard]] NodeRef relatedAt(const NodeRef &node, RelationKind kind,
                                    std::size_t index) const;
    [[nodiscard]] std::vector<NodeRef> related(const NodeRef &node,
                                               RelationKind kind) const;

  private:
    friend class NodeGraph;
    ReadAccess(const NodeGraph &graph,
               std::shared_lock<std::shared_mutex> lock) noexcept;
    void requireMember(const NodeRef &node) const;

    const NodeGraph *graph_;
    std::shared_lock<std::shared_mutex> lock_;
  };

  class WriteAccess final {
  public:
    WriteAccess(WriteAccess &&other) noexcept;
    WriteAccess &operator=(WriteAccess &&) = delete;
    WriteAccess(const WriteAccess &) = delete;
    WriteAccess &operator=(const WriteAccess &) = delete;
    ~WriteAccess();

    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] NodeRef find(const NodeId &id) const;
    [[nodiscard]] const std::vector<NodeRef> &orderedNodes() const noexcept;
    [[nodiscard]] std::uint64_t changedRevision(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t
    fieldChangedRevision(const NodeRef &node, std::string_view field) const;
    [[nodiscard]] std::uint64_t
    statusChangedRevision(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t
    structureChangedRevision(const NodeRef &node) const;
    [[nodiscard]] bool hasPendingChanges() const noexcept;
    [[nodiscard]] NodeRef upsert(NodeId id, NodeState initial = {});
    [[nodiscard]] std::shared_ptr<const NodeState>
    state(const NodeRef &node) const;
    [[nodiscard]] NodeRef parent(const NodeRef &node) const;
    [[nodiscard]] std::vector<NodeRef> children(const NodeRef &node) const;
    [[nodiscard]] std::vector<NodeRef> related(const NodeRef &node,
                                               RelationKind kind) const;

    void replaceState(const NodeRef &node, NodeState state);
    void setField(const NodeRef &node, std::string key, Value value);
    void eraseField(const NodeRef &node, std::string_view key);
    void setStatus(const NodeRef &node, NodeStatus status);
    // Advance an owning aggregate's changed revision after a concrete
    // descendant mutation without adding redundant render work to the
    // transaction's affected-node notification.
    void touchRevision(const NodeRef &node);
    void setParent(const NodeRef &parent, const NodeRef &child);
    void clearParent(const NodeRef &child);
    void replaceChildren(const NodeRef &parent,
                         std::span<const NodeRef> children);
    void relate(const NodeRef &source, RelationKind kind,
                const NodeRef &target);
    void unrelate(const NodeRef &source, RelationKind kind,
                  const NodeRef &target);
    void replaceRelated(const NodeRef &source, RelationKind kind,
                        std::span<const NodeRef> targets);
    void remove(const NodeRef &node);

    // Removed nodes remain reachable only for UI-detachment recovery when a
    // graph notification saturates. The sole writer releases them after Qt has
    // acknowledged detachment through the typed command mailbox.
    void releaseRetired(std::span<const NodeRef> nodes);

    // Publishes one revision for the complete mutation and unlocks before
    // returning, so callers can notify Qt without holding graph
    // synchronization.
    [[nodiscard]] GraphChange finish();

  private:
    friend class NodeGraph;
    struct PendingStateRevision final {
      bool status = false;
      std::unordered_set<std::string> fields;
    };

    WriteAccess(NodeGraph &graph,
                std::unique_lock<std::shared_mutex> lock) noexcept;

    void requireLive(const NodeRef &node) const;
    void noteStateChanges(const NodeRef &node, const NodeState &before,
                          const NodeState &after);
    void noteStructureChange(const NodeRef &node);
    void markAffected(const NodeRef &node);
    void unlinkNode(const NodeRef &node);
    [[nodiscard]] GraphChange publish();

    NodeGraph *graph_;
    std::unique_lock<std::shared_mutex> lock_;
    std::vector<NodeRef> affected_;
    std::unordered_set<Node *> affectedIndex_;
    std::vector<NodeRef> revisionTouches_;
    std::unordered_set<Node *> revisionTouchIndex_;
    std::unordered_map<Node *, PendingStateRevision> pendingStateRevisions_;
    std::unordered_set<Node *> pendingStructureRevisions_;
    std::vector<NodeRef> removed_;
    std::unordered_set<Node *> removedIndex_;
    bool dirty_ = false;
    bool finished_ = false;
  };

private:
  static constexpr std::size_t NodeKindCount =
      static_cast<std::size_t>(NodeKind::UnknownProtocol) + 1;

  mutable std::shared_mutex mutex_;
  std::unordered_map<NodeId, NodeRef, NodeIdHash> nodes_;
  std::vector<NodeRef> orderedNodes_;
  std::vector<NodeRef> retiredNodes_;
  std::unordered_map<Node *, std::size_t> retiredIndex_;
  std::uint64_t retiredOrderGeneration_ = 0;
  std::uint64_t nextInsertionOrder_ = 1;
  std::uint64_t revision_ = 0;
  std::array<std::uint64_t, NodeKindCount> structureRevisions_{};
  std::atomic<std::uint64_t> publishedRevision_{0};
  std::array<std::atomic<std::uint64_t>, NodeKindCount>
      publishedStructureRevisions_{};
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_NODEGRAPH_H
