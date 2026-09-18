// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_NODEGRAPH_NODEGRAPH_H
#define CODEXUI_CODEX_NODEGRAPH_NODEGRAPH_H

#include "codex/nodegraph/Value.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

constexpr NodeStatus nodeStatusFromText(std::string_view status) noexcept {
  if (status == "pending" || status == "queued")
    return NodeStatus::Pending;
  if (status == "running" || status == "inProgress" || status == "active" ||
      status == "started")
    return NodeStatus::Running;
  if (status == "completed" || status == "complete" || status == "succeeded" ||
      status == "idle")
    return NodeStatus::Completed;
  if (status == "failed" || status == "error" || status == "systemError" ||
      status == "blocked")
    return NodeStatus::Failed;
  if (status == "interrupted" || status == "cancelled" ||
      status == "canceled" || status == "stopped")
    return NodeStatus::Interrupted;
  if (status == "notLoaded")
    return NodeStatus::NotLoaded;
  if (status == "connected")
    return NodeStatus::Connected;
  if (status == "disconnected")
    return NodeStatus::Disconnected;
  return NodeStatus::Unknown;
}

struct NodeStatusView final {
  NodeStatus semantic = NodeStatus::Unknown;
  std::string_view unknownText;

  [[nodiscard]] constexpr bool empty() const noexcept {
    return semantic == NodeStatus::Unknown && unknownText.empty();
  }
};

[[nodiscard]] std::string_view statusTextFromValue(const Value *value);
[[nodiscard]] NodeStatus nodeStatusFromValue(const Value *value);
[[nodiscard]] NodeStatusView nodeStatusView(const Value *value);

// Protocol facts are retained directly here. The immutable storage is replaced
// as one unit by the sole writer and can be pinned briefly by Qt after a
// successful try-read.
struct NodeState final {
  NodeStatus status = NodeStatus::Unknown;
  Value::Object fields;

  bool operator==(const NodeState &) const = default;
};

[[nodiscard]] inline const Value *valueMember(const NodeState &state,
                                              std::string_view key) noexcept {
  return valueMember(state.fields, key);
}

[[nodiscard]] NodeStatusView nodeStatusView(const NodeState &state);
[[nodiscard]] NodeStatusView agentActivityStatus(const NodeState &state);
[[nodiscard]] bool agentActivityCanCreate(const NodeState &state);

struct InspectorPlanView final {
  const Value *steps = nullptr;
  const Value *explanation = nullptr;
  std::size_t rowCount = 0;

  [[nodiscard]] explicit operator bool() const noexcept { return steps; }
};

[[nodiscard]] InspectorPlanView
inspectorPlanView(const NodeState &state) noexcept;
[[nodiscard]] const Value::Object *
reportedAgentState(const NodeState &state, std::string_view childId) noexcept;
[[nodiscard]] NodeStatusView inspectorAgentActivityStatus(
    const NodeState &state, const NodeState *turn, NodeStatusView child,
    bool childIdVisible) noexcept;

enum class RelationKind : std::uint8_t {
  RootThread,
  StructuralChildThread,
  AgentChildThread,
  ThreadOwner,
  ForkChildThread,
  ProjectMembership,
  SectionMembership,
  TurnRootItem,
  PendingOperation,
  InteractionTarget,
  PendingInteraction,
  PendingPrompt,
  PromptMaterialization,
  ProcessOwner,
  ReviewTarget,
  ActiveTurn,
  UiSelectionTarget,
};

class Node final : public std::enable_shared_from_this<Node> {
public:
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;

  [[nodiscard]] const NodeId &id() const noexcept;
  [[nodiscard]] std::uint64_t incarnation() const noexcept {
    return insertionOrder_;
  }

private:
  friend class NodeGraph;

  explicit Node(NodeId id, NodeState state, std::uint64_t insertionOrder);

  NodeId id_;
  std::shared_ptr<const NodeState> state_;
  Node *parent_ = nullptr;
  // Child slot while parented; retirement slot while graph-retained. The
  // vector identity is authoritative. Atomic access keeps foreign membership
  // probes race-free without adding a second retirement index.
  std::atomic<std::size_t> parentIndex_{0};
  std::vector<Node *> children_;
  std::unordered_map<RelationKind, std::vector<Node *>> relations_;
  // Derived from relations_ so removal can find exact incoming edges without
  // searching unrelated nodes. Only WriteAccess mutates both sides.
  std::vector<std::pair<Node *, RelationKind>> incomingRelations_;
  std::unordered_map<std::string, std::uint64_t> fieldChangedRevisions_;
  std::uint64_t statusChangedRevision_ = 0;
  // Changes only when this node's parent, ordered children, or an outgoing
  // relation changes. UI scans use the stamp to validate bounded structural
  // reads without treating ordinary streaming fields as topology changes.
  std::uint64_t structureChangedRevision_ = 0;
  std::uint64_t changedRevision_ = 0;
  // Immutable node incarnation and position in the graph's append-only
  // insertion order. It distinguishes a removed node from a later node that
  // reuses the same protocol identity.
  std::uint64_t insertionOrder_ = 0;
};

using NodeRef = std::shared_ptr<Node>;

inline constexpr std::array<std::string_view, 6> InspectorAgentDetailFields{
    "agentPath", "tool", "model", "reasoningEffort", "prompt",
    "senderThreadId"};

struct InspectorAgentSelector final {
  static constexpr std::size_t CandidateSetCount =
      InspectorAgentDetailFields.size() + 4;

  std::string key;
  std::string childId;
  NodeRef exactChild;
  bool childIdVisible = true;
  std::deque<NodeRef> contributors;
  std::array<std::set<std::size_t>, CandidateSetCount> candidates;

  bool operator==(const InspectorAgentSelector &) const = default;
};

struct InspectorAgentDependency final {
  std::size_t row = 0;
  std::size_t contributor = 0;
};

// Graph-owned semantic selectors for bounded Inspector projection. These
// values identify authoritative sources; they deliberately contain no Qt or
// formatted presentation data.
struct InspectorThreadIndex final {
  NodeRef planSource;
  std::size_t planRowCount = 0;
  bool planHasExplanation = false;
  std::deque<InspectorAgentSelector> agents;
  std::map<std::string, std::size_t> agentPositions;
  std::unordered_map<Node *, std::deque<InspectorAgentDependency>>
      agentDependencies;
  std::map<Node *, std::size_t> agentMessageOrder;
  std::map<std::size_t, NodeRef> agentMessages;
  std::uint64_t planOrderRevision = 0;
  std::uint64_t planChangedRevision = 0;
  std::uint64_t agentOrderRevision = 0;
  std::uint64_t agentChangedRevision = 0;

  [[nodiscard]] NodeRef latestAgentMessage() const {
    return agentMessages.empty() ? NodeRef{} : agentMessages.rbegin()->second;
  }
};

// Identifies one authoritative ordered child subsequence affected by a graph
// transaction. Consumers can distinguish relevant child kinds without
// consulting mutable state from a later revision.
struct ChildListChange final {
  NodeRef owner;
  NodeKind childKind = NodeKind::UnknownProtocol;

  bool operator==(const ChildListChange &) const = default;
};

struct GraphChange final {
  std::uint64_t revision = 0;
  std::vector<NodeRef> affected;
  std::vector<NodeRef> removed;
  std::vector<ChildListChange> childListsChanged;
  std::uint64_t providerAuthorityRevision = 0;

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
    [[nodiscard]] const std::vector<NodeRef> &
    orderedNodes(NodeKind kind) const noexcept;
    [[nodiscard]] std::size_t retiredCount() const noexcept;
    [[nodiscard]] NodeRef retiredAt(std::size_t index) const;
    // Changes only when an existing retirement is released. Appending a
    // newly removed node preserves every earlier index.
    [[nodiscard]] std::uint64_t retiredOrderGeneration() const noexcept;
    [[nodiscard]] std::uint64_t
    pendingInteractionOrderRevision() const noexcept;
    // Retained UI NodeRefs may outlive retirement acknowledgement. This
    // non-throwing probe lets a deferred Qt pass discard such a reference
    // before asking for graph-owned state.
    [[nodiscard]] bool live(const NodeRef &node) const noexcept;
    [[nodiscard]] bool contains(const NodeRef &node) const noexcept;
    [[nodiscard]] std::shared_ptr<const NodeState>
    state(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t changedRevision(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t
    fieldChangedRevision(const NodeRef &node, std::string_view field) const;
    [[nodiscard]] bool fieldsChangedAt(const NodeRef &node,
                                       std::uint64_t revision) const;
    [[nodiscard]] std::uint64_t
    statusChangedRevision(const NodeRef &node) const;
    [[nodiscard]] std::uint64_t
    structureChangedRevision(const NodeRef &node) const;
    [[nodiscard]] NodeRef parent(const NodeRef &node) const;
    [[nodiscard]] std::optional<std::size_t>
    childIndex(const NodeRef &node) const;
    [[nodiscard]] std::size_t childCount(const NodeRef &node) const;
    [[nodiscard]] NodeRef childAt(const NodeRef &node, std::size_t index) const;
    [[nodiscard]] std::vector<NodeRef> children(const NodeRef &node) const;
    [[nodiscard]] std::size_t relatedCount(const NodeRef &node,
                                           RelationKind kind) const;
    [[nodiscard]] NodeRef relatedAt(const NodeRef &node, RelationKind kind,
                                    std::size_t index) const;
    [[nodiscard]] bool isRelated(const NodeRef &source, RelationKind kind,
                                 const NodeRef &target) const;
    [[nodiscard]] bool hasIncomingRelation(const NodeRef &target,
                                           RelationKind kind) const;
    [[nodiscard]] std::vector<NodeRef> related(const NodeRef &node,
                                               RelationKind kind) const;
    [[nodiscard]] const InspectorThreadIndex *
    inspectorIndex(const NodeRef &thread) const;

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
    void removeMany(std::span<const NodeRef> nodes);

    // Removed nodes remain graph-readable until Qt has observed all preceding
    // callbacks, including a synthesized rescan after notification saturation.
    // The sole writer releases them after the typed acknowledgement arrives.
    void releaseRetired(std::span<const NodeRef> nodes);

    // Publishes one revision for the complete mutation and unlocks before
    // returning, so callers can notify Qt without holding graph
    // synchronization. A commit cannot roll back mutations already applied by
    // the sole writer, so allocation or invariant failure is process-fatal.
    [[nodiscard]] GraphChange finish() noexcept;

  private:
    friend class NodeGraph;
    struct PendingStateRevision final {
      bool status = false;
      std::unordered_set<std::string> fields;
    };

    struct ChildListChangeHash final {
      std::size_t operator()(const ChildListChange &change) const noexcept {
        const std::size_t owner = std::hash<Node *>{}(change.owner.get());
        const std::size_t kind = static_cast<std::size_t>(change.childKind);
        return owner ^ (kind + 0x9e3779b9U + (owner << 6U) + (owner >> 2U));
      }
    };

    struct InspectorImpact final {
      struct AgentAppend final {
        Node *turn = nullptr;
        std::size_t first = 0;
      };

      std::uint8_t rebuild = 0;
      std::optional<AgentAppend> append;
    };

    WriteAccess(NodeGraph &graph,
                std::unique_lock<std::shared_mutex> lock) noexcept;

    void requireLive(const NodeRef &node) const;
    [[nodiscard]] Node *inspectorThread(Node *node) const noexcept;
    [[nodiscard]] std::uint8_t inspectorItemSections(Node *item) const;
    [[nodiscard]] std::uint8_t inspectorTurnSections(Node *turn) const;
    [[nodiscard]] std::uint8_t inspectorIndexedSections(Node *thread,
                                                        Node *node) const;
    void markInspectorRebuild(Node *thread, std::uint8_t sections);
    [[nodiscard]] bool markInspectorAppend(Node *thread, Node *turn,
                                           std::size_t first);
    void noteInspectorChildChange(Node *parent, Node *child, bool appended);
    void noteInspectorChildrenReplacement(
        Node *parent, std::span<Node *const> previous,
        std::span<const NodeRef> next);
    void noteInspectorRelationChange(Node *source, RelationKind kind);
    void refreshInspectorIndexes();
    void refreshInspectorAgentContribution(InspectorAgentSelector &selector,
                                           std::size_t contributor) const;
    void refreshInspectorAgentDependencies(InspectorThreadIndex &index,
                                           Node *dependency) const;
    [[nodiscard]] bool refreshInspectorAgentMessage(
        InspectorThreadIndex &index, Node *source) const;
    [[nodiscard]] bool appendInspectorAgentItem(InspectorThreadIndex &index,
                                                Node *item) const;
    [[nodiscard]] InspectorThreadIndex
    buildInspectorIndex(Node *thread, std::uint8_t sections) const;
    void prepareChanges(std::span<const NodeRef> affected,
                        std::span<const NodeRef> structureChanged,
                        std::span<const ChildListChange> childrenChanged = {});
    [[nodiscard]] GraphChange publish();

    NodeGraph *graph_;
    std::unique_lock<std::shared_mutex> lock_;
    std::vector<NodeRef> affected_;
    std::unordered_set<Node *> affectedIndex_;
    std::vector<NodeRef> revisionTouches_;
    std::unordered_set<Node *> revisionTouchIndex_;
    std::unordered_map<Node *, PendingStateRevision> pendingStateRevisions_;
    std::unordered_set<Node *> pendingStructureRevisions_;
    std::vector<ChildListChange> childListsChanged_;
    std::unordered_set<ChildListChange, ChildListChangeHash>
        childListsChangedIndex_;
    std::map<Node *, InspectorImpact> inspectorImpacts_;
    std::vector<NodeRef> removed_;
    bool pendingInteractionOrderChanged_ = false;
    bool dirty_ = false;
    bool finished_ = false;
  };

private:
  static constexpr std::size_t NodeKindCount =
      static_cast<std::size_t>(NodeKind::UnknownProtocol) + 1;

  mutable std::shared_mutex mutex_;
  std::unordered_map<NodeId, NodeRef, NodeIdHash> nodes_;
  std::vector<NodeRef> orderedNodes_;
  std::array<std::vector<NodeRef>, NodeKindCount> orderedNodesByKind_;
  std::vector<NodeRef> retiredNodes_;
  std::uint64_t retiredOrderGeneration_ = 0;
  std::uint64_t pendingInteractionOrderRevision_ = 0;
  std::uint64_t nextInsertionOrder_ = 1;
  std::uint64_t revision_ = 0;
  std::array<std::uint64_t, NodeKindCount> structureRevisions_{};
  std::atomic<std::uint64_t> publishedRevision_{0};
  std::array<std::atomic<std::uint64_t>, NodeKindCount>
      publishedStructureRevisions_{};
  std::map<Node *, InspectorThreadIndex> inspectorIndexes_;
};

} // namespace codexui::nodegraph

#endif // CODEXUI_CODEX_NODEGRAPH_NODEGRAPH_H
