// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/NodeGraph.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace codexui::nodegraph {
namespace {

template <typename Range, typename ValueType>
bool contains(const Range &range, const ValueType &value) {
  return std::find(range.begin(), range.end(), value) != range.end();
}

template <typename Range, typename ValueType>
void eraseValue(Range &range, const ValueType &value) {
  range.erase(std::remove(range.begin(), range.end(), value), range.end());
}

NodeRef pin(Node *node) { return node ? node->shared_from_this() : NodeRef{}; }

template <typename T>
void ensureAppendCapacity(std::vector<T> &values, std::size_t additional) {
  if (additional <= values.capacity() - values.size())
    return;
  const std::size_t required = values.size() + additional;
  const std::size_t grown =
      values.capacity() <= values.max_size() / 2
          ? std::max<std::size_t>(1, values.capacity() * 2)
          : values.max_size();
  values.reserve(std::max(required, grown));
}

} // namespace

std::size_t NodeIdHash::operator()(const NodeId &id) const noexcept {
  const std::size_t kind = static_cast<std::size_t>(id.kind);
  const std::size_t value = std::hash<std::string>{}(id.canonical);
  return value ^ (kind + 0x9e3779b9U + (value << 6U) + (value >> 2U));
}

Node::Node(NodeId id, NodeState state, std::uint64_t insertionOrder)
    : id_(std::move(id)),
      state_(std::make_shared<const NodeState>(std::move(state))),
      insertionOrder_(insertionOrder) {}

const NodeId &Node::id() const noexcept { return id_; }

void *Node::uiAttachment() const noexcept {
  return uiAttachment_.load(std::memory_order_acquire);
}

void Node::setUiAttachment(void *attachment) noexcept {
  uiAttachment_.store(attachment, std::memory_order_release);
}

bool GraphChange::empty() const noexcept {
  return affected.empty() && removed.empty();
}

NodeGraph::WriteAccess NodeGraph::write() {
  return WriteAccess(*this, std::unique_lock<std::shared_mutex>(mutex_));
}

std::optional<NodeGraph::ReadAccess> NodeGraph::tryRead() const {
  std::shared_lock<std::shared_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return std::nullopt;
  return ReadAccess(*this, std::move(lock));
}

std::uint64_t NodeGraph::publishedRevision() const noexcept {
  return publishedRevision_.load(std::memory_order_acquire);
}

std::uint64_t
NodeGraph::publishedStructureRevision(NodeKind kind) const noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  if (index >= publishedStructureRevisions_.size())
    return 0;
  return publishedStructureRevisions_[index].load(std::memory_order_acquire);
}

NodeGraph::ReadAccess::ReadAccess(
    const NodeGraph &graph, std::shared_lock<std::shared_mutex> lock) noexcept
    : graph_(&graph), lock_(std::move(lock)) {}

std::uint64_t NodeGraph::ReadAccess::revision() const noexcept {
  return graph_->revision_;
}

std::uint64_t
NodeGraph::ReadAccess::structureRevision(NodeKind kind) const noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  return index < graph_->structureRevisions_.size()
             ? graph_->structureRevisions_[index]
             : 0;
}

NodeRef NodeGraph::ReadAccess::find(const NodeId &id) const {
  const auto found = graph_->nodes_.find(id);
  return found == graph_->nodes_.end() ? NodeRef{} : found->second;
}

const std::vector<NodeRef> &
NodeGraph::ReadAccess::orderedNodes() const noexcept {
  return graph_->orderedNodes_;
}

std::uint64_t NodeGraph::ReadAccess::insertionOrder(const NodeRef &node) const {
  if (!node)
    return 0;
  requireMember(node);
  return node->insertionOrder_;
}

const std::vector<NodeRef> &
NodeGraph::ReadAccess::retiredNodes() const noexcept {
  return graph_->retiredNodes_;
}

std::size_t NodeGraph::ReadAccess::retiredCount() const noexcept {
  return graph_->retiredNodes_.size();
}

NodeRef NodeGraph::ReadAccess::retiredAt(std::size_t index) const {
  return index < graph_->retiredNodes_.size() ? graph_->retiredNodes_[index]
                                              : NodeRef{};
}

std::uint64_t NodeGraph::ReadAccess::retiredOrderGeneration() const noexcept {
  return graph_->retiredOrderGeneration_;
}

bool NodeGraph::ReadAccess::contains(const NodeRef &node) const noexcept {
  if (!node)
    return false;
  const auto active = graph_->nodes_.find(node->id_);
  return (active != graph_->nodes_.end() && active->second == node) ||
         graph_->retiredIndex_.contains(node.get());
}

std::shared_ptr<const NodeState>
NodeGraph::ReadAccess::state(const NodeRef &node) const {
  if (!node)
    return {};
  requireMember(node);
  return node->state_;
}

std::uint64_t
NodeGraph::ReadAccess::changedRevision(const NodeRef &node) const {
  if (!node)
    return 0;
  requireMember(node);
  return node->changedRevision_;
}

std::uint64_t
NodeGraph::ReadAccess::fieldChangedRevision(const NodeRef &node,
                                            std::string_view field) const {
  if (!node)
    return 0;
  requireMember(node);
  const auto found = node->fieldChangedRevisions_.find(std::string(field));
  return found == node->fieldChangedRevisions_.end() ? 0 : found->second;
}

std::uint64_t
NodeGraph::ReadAccess::statusChangedRevision(const NodeRef &node) const {
  if (!node)
    return 0;
  requireMember(node);
  return node->statusChangedRevision_;
}

std::uint64_t
NodeGraph::ReadAccess::structureChangedRevision(const NodeRef &node) const {
  if (!node)
    return 0;
  requireMember(node);
  return node->structureChangedRevision_;
}

bool NodeGraph::ReadAccess::removed(const NodeRef &node) const {
  if (!node)
    return true;
  requireMember(node);
  return node->removed_;
}

NodeRef NodeGraph::ReadAccess::parent(const NodeRef &node) const {
  if (!node)
    return {};
  requireMember(node);
  return pin(node->parent_);
}

std::size_t NodeGraph::ReadAccess::childCount(const NodeRef &node) const {
  if (!node)
    return 0;
  requireMember(node);
  return node->children_.size();
}

NodeRef NodeGraph::ReadAccess::childAt(const NodeRef &node,
                                       std::size_t index) const {
  if (!node)
    return {};
  requireMember(node);
  return index < node->children_.size() ? pin(node->children_[index])
                                        : NodeRef{};
}

std::vector<NodeRef>
NodeGraph::ReadAccess::children(const NodeRef &node) const {
  std::vector<NodeRef> result;
  if (!node)
    return result;
  requireMember(node);
  result.reserve(node->children_.size());
  for (Node *child : node->children_)
    result.emplace_back(pin(child));
  return result;
}

std::size_t NodeGraph::ReadAccess::relatedCount(const NodeRef &node,
                                                RelationKind kind) const {
  if (!node)
    return 0;
  requireMember(node);
  const auto found = node->relations_.find(kind);
  return found == node->relations_.end() ? 0 : found->second.size();
}

NodeRef NodeGraph::ReadAccess::relatedAt(const NodeRef &node, RelationKind kind,
                                         std::size_t index) const {
  if (!node)
    return {};
  requireMember(node);
  const auto found = node->relations_.find(kind);
  if (found == node->relations_.end() || index >= found->second.size())
    return {};
  return pin(found->second[index]);
}

std::vector<NodeRef> NodeGraph::ReadAccess::related(const NodeRef &node,
                                                    RelationKind kind) const {
  std::vector<NodeRef> result;
  if (!node)
    return result;
  requireMember(node);
  const auto found = node->relations_.find(kind);
  if (found == node->relations_.end())
    return result;
  result.reserve(found->second.size());
  for (Node *target : found->second)
    result.emplace_back(pin(target));
  return result;
}

void NodeGraph::ReadAccess::requireMember(const NodeRef &node) const {
  if (contains(node))
    return;
  throw std::invalid_argument("node does not belong to this graph");
}

NodeGraph::WriteAccess::WriteAccess(
    NodeGraph &graph, std::unique_lock<std::shared_mutex> lock) noexcept
    : graph_(&graph), lock_(std::move(lock)) {}

NodeGraph::WriteAccess::WriteAccess(WriteAccess &&other) noexcept
    : graph_(std::exchange(other.graph_, nullptr)),
      lock_(std::move(other.lock_)), affected_(std::move(other.affected_)),
      affectedIndex_(std::move(other.affectedIndex_)),
      revisionTouches_(std::move(other.revisionTouches_)),
      revisionTouchIndex_(std::move(other.revisionTouchIndex_)),
      pendingStateRevisions_(std::move(other.pendingStateRevisions_)),
      pendingStructureRevisions_(std::move(other.pendingStructureRevisions_)),
      removed_(std::move(other.removed_)),
      removedIndex_(std::move(other.removedIndex_)), dirty_(other.dirty_),
      finished_(other.finished_) {
  other.dirty_ = false;
  other.finished_ = true;
}

NodeGraph::WriteAccess::~WriteAccess() {
  if (graph_ && lock_.owns_lock() && !finished_ && dirty_)
    std::terminate();
}

std::uint64_t NodeGraph::WriteAccess::revision() const noexcept {
  return graph_ ? graph_->revision_ : 0;
}

NodeRef NodeGraph::WriteAccess::find(const NodeId &id) const {
  const auto found = graph_->nodes_.find(id);
  return found == graph_->nodes_.end() ? NodeRef{} : found->second;
}

const std::vector<NodeRef> &
NodeGraph::WriteAccess::orderedNodes() const noexcept {
  return graph_->orderedNodes_;
}

std::uint64_t
NodeGraph::WriteAccess::changedRevision(const NodeRef &node) const {
  requireLive(node);
  return node->changedRevision_;
}

std::uint64_t
NodeGraph::WriteAccess::fieldChangedRevision(const NodeRef &node,
                                             std::string_view field) const {
  requireLive(node);
  const auto found = node->fieldChangedRevisions_.find(std::string(field));
  return found == node->fieldChangedRevisions_.end() ? 0 : found->second;
}

std::uint64_t
NodeGraph::WriteAccess::statusChangedRevision(const NodeRef &node) const {
  requireLive(node);
  return node->statusChangedRevision_;
}

std::uint64_t
NodeGraph::WriteAccess::structureChangedRevision(const NodeRef &node) const {
  requireLive(node);
  return node->structureChangedRevision_;
}

bool NodeGraph::WriteAccess::hasPendingChanges() const noexcept {
  return dirty_;
}

NodeRef NodeGraph::WriteAccess::upsert(NodeId id, NodeState initial) {
  if (NodeRef existing = find(id))
    return existing;
  NodeRef node(
      new Node(std::move(id), std::move(initial), graph_->nextInsertionOrder_));
  PendingStateRevision pending;
  pending.status = node->state_->status != NodeStatus::Unknown;
  for (const auto &[field, value] : node->state_->fields) {
    static_cast<void>(value);
    pending.fields.insert(field);
    node->fieldChangedRevisions_.emplace(field, 0);
  }

  // Allocate every auxiliary slot first. If canonical insertion then fails,
  // roll these transaction-local entries back before propagating the error.
  graph_->nodes_.reserve(graph_->nodes_.size() + 1);
  ensureAppendCapacity(graph_->orderedNodes_, 1);
  pendingStateRevisions_.reserve(pendingStateRevisions_.size() + 1);
  affectedIndex_.reserve(affectedIndex_.size() + 1);
  ensureAppendCapacity(affected_, 1);
  pendingStateRevisions_.emplace(node.get(), std::move(pending));
  try {
    affectedIndex_.insert(node.get());
    try {
      affected_.emplace_back(node);
    } catch (...) {
      affectedIndex_.erase(node.get());
      throw;
    }
    try {
      graph_->nodes_.emplace(node->id_, node);
    } catch (...) {
      affected_.pop_back();
      affectedIndex_.erase(node.get());
      pendingStateRevisions_.erase(node.get());
      throw;
    }
  } catch (...) {
    pendingStateRevisions_.erase(node.get());
    throw;
  }
  graph_->orderedNodes_.emplace_back(node);
  ++graph_->nextInsertionOrder_;
  dirty_ = true;
  return node;
}

std::shared_ptr<const NodeState>
NodeGraph::WriteAccess::state(const NodeRef &node) const {
  requireLive(node);
  return node->state_;
}

NodeRef NodeGraph::WriteAccess::parent(const NodeRef &node) const {
  requireLive(node);
  return pin(node->parent_);
}

std::vector<NodeRef>
NodeGraph::WriteAccess::children(const NodeRef &node) const {
  requireLive(node);
  std::vector<NodeRef> result;
  result.reserve(node->children_.size());
  for (Node *child : node->children_)
    result.emplace_back(pin(child));
  return result;
}

std::vector<NodeRef> NodeGraph::WriteAccess::related(const NodeRef &node,
                                                     RelationKind kind) const {
  requireLive(node);
  std::vector<NodeRef> result;
  const auto found = node->relations_.find(kind);
  if (found == node->relations_.end())
    return result;
  result.reserve(found->second.size());
  for (Node *target : found->second)
    result.emplace_back(pin(target));
  return result;
}

void NodeGraph::WriteAccess::replaceState(const NodeRef &node,
                                          NodeState state) {
  requireLive(node);
  if (*node->state_ == state)
    return;
  const NodeState &before = *node->state_;
  std::shared_ptr<const NodeState> storage =
      std::make_shared<const NodeState>(std::move(state));
  const NodeState &after = *storage;

  const auto pendingPosition = pendingStateRevisions_.find(node.get());
  const bool hadPending = pendingPosition != pendingStateRevisions_.end();
  PendingStateRevision pending =
      hadPending ? pendingPosition->second : PendingStateRevision{};
  pending.status = pending.status || before.status != after.status;
  for (const auto &[field, value] : before.fields) {
    const auto found = after.fields.find(field);
    if (found == after.fields.end() || found->second != value)
      pending.fields.insert(field);
  }
  for (const auto &[field, value] : after.fields) {
    const auto found = before.fields.find(field);
    if (found == before.fields.end() || found->second != value)
      pending.fields.insert(field);
  }
  auto fieldRevisions = node->fieldChangedRevisions_;
  for (const std::string &field : pending.fields)
    fieldRevisions.try_emplace(field, 0);

  if (hadPending) {
    std::swap(pendingPosition->second, pending);
  } else {
    pendingStateRevisions_.emplace(node.get(), std::move(pending));
  }
  const std::array affected{node};
  try {
    prepareChanges(affected, {});
  } catch (...) {
    if (hadPending)
      std::swap(pendingPosition->second, pending);
    else
      pendingStateRevisions_.erase(node.get());
    throw;
  }
  node->fieldChangedRevisions_.swap(fieldRevisions);
  node->state_ = std::move(storage);
}

void NodeGraph::WriteAccess::setField(const NodeRef &node, std::string key,
                                      Value value) {
  requireLive(node);
  const auto found = node->state_->fields.find(key);
  if (found != node->state_->fields.end() && found->second == value)
    return;
  NodeState next = *node->state_;
  next.fields.insert_or_assign(std::move(key), std::move(value));
  replaceState(node, std::move(next));
}

void NodeGraph::WriteAccess::eraseField(const NodeRef &node,
                                        std::string_view key) {
  requireLive(node);
  if (!node->state_->fields.contains(key))
    return;
  NodeState next = *node->state_;
  next.fields.erase(std::string(key));
  replaceState(node, std::move(next));
}

void NodeGraph::WriteAccess::setStatus(const NodeRef &node, NodeStatus status) {
  requireLive(node);
  if (node->state_->status == status)
    return;
  NodeState next = *node->state_;
  next.status = status;
  replaceState(node, std::move(next));
}

void NodeGraph::WriteAccess::touchRevision(const NodeRef &node) {
  requireLive(node);
  if (affectedIndex_.contains(node.get()))
    return;
  const auto [position, inserted] = revisionTouchIndex_.insert(node.get());
  if (!inserted)
    return;
  try {
    revisionTouches_.emplace_back(node);
  } catch (...) {
    revisionTouchIndex_.erase(position);
    throw;
  }
}

void NodeGraph::WriteAccess::setParent(const NodeRef &parent,
                                       const NodeRef &child) {
  requireLive(parent);
  requireLive(child);
  if (parent == child)
    throw std::invalid_argument("a node cannot parent itself");
  for (Node *ancestor = parent.get(); ancestor; ancestor = ancestor->parent_) {
    if (ancestor == child.get())
      throw std::invalid_argument("a parent relation cannot form a cycle");
  }
  if (child->parent_ == parent.get())
    return;
  parent->children_.reserve(parent->children_.size() + 1);
  NodeRef previousParent = pin(child->parent_);
  const std::array changed{parent, child, previousParent};
  prepareChanges(changed, changed);
  if (previousParent)
    eraseValue(previousParent->children_, child.get());
  child->parent_ = parent.get();
  parent->children_.emplace_back(child.get());
}

void NodeGraph::WriteAccess::clearParent(const NodeRef &child) {
  requireLive(child);
  if (!child->parent_)
    return;
  NodeRef parent = pin(child->parent_);
  const std::array changed{parent, child};
  prepareChanges(changed, changed);
  eraseValue(parent->children_, child.get());
  child->parent_ = nullptr;
}

void NodeGraph::WriteAccess::replaceChildren(
    const NodeRef &parent, std::span<const NodeRef> children) {
  requireLive(parent);
  std::vector<NodeRef> next;
  next.reserve(children.size());
  std::unordered_set<const Node *> seen;
  seen.reserve(children.size());
  for (const NodeRef &child : children) {
    requireLive(child);
    if (parent == child)
      throw std::invalid_argument("a node cannot parent itself");
    for (Node *ancestor = parent.get(); ancestor;
         ancestor = ancestor->parent_) {
      if (ancestor == child.get())
        throw std::invalid_argument("a parent relation cannot form a cycle");
    }
    if (seen.insert(child.get()).second)
      next.emplace_back(child);
  }

  bool unchanged = parent->children_.size() == next.size();
  if (unchanged) {
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (parent->children_[index] != next[index].get() ||
          next[index]->parent_ != parent.get()) {
        unchanged = false;
        break;
      }
    }
  }
  if (unchanged)
    return;

  const std::vector<Node *> previous = parent->children_;
  std::vector<NodeRef> affected{parent};
  std::vector<NodeRef> structureChanged{parent};
  affected.reserve(1 + previous.size() + next.size() * 2);
  structureChanged.reserve(1 + previous.size() + next.size() * 2);
  for (Node *oldChildPointer : previous) {
    if (!seen.contains(oldChildPointer)) {
      NodeRef oldChild = pin(oldChildPointer);
      affected.emplace_back(oldChild);
      structureChanged.emplace_back(std::move(oldChild));
    }
  }

  for (const NodeRef &child : next) {
    const bool parentChanged = child->parent_ != parent.get();
    if (child->parent_ && child->parent_ != parent.get()) {
      NodeRef previousParent = pin(child->parent_);
      affected.emplace_back(previousParent);
      structureChanged.emplace_back(std::move(previousParent));
    }
    affected.emplace_back(child);
    if (parentChanged)
      structureChanged.emplace_back(child);
  }

  parent->children_.reserve(next.size());
  prepareChanges(affected, structureChanged);
  for (Node *oldChildPointer : previous)
    if (!seen.contains(oldChildPointer))
      oldChildPointer->parent_ = nullptr;
  for (const NodeRef &child : next) {
    if (child->parent_ && child->parent_ != parent.get())
      eraseValue(child->parent_->children_, child.get());
    child->parent_ = parent.get();
  }
  parent->children_.clear();
  for (const NodeRef &child : next)
    parent->children_.emplace_back(child.get());
}

void NodeGraph::WriteAccess::relate(const NodeRef &source, RelationKind kind,
                                    const NodeRef &target) {
  requireLive(source);
  requireLive(target);
  const auto current = source->relations_.find(kind);
  if (current != source->relations_.end() &&
      contains(current->second, target.get()))
    return;
  auto nextRelations = source->relations_;
  nextRelations[kind].emplace_back(target.get());
  const std::array affected{source, target};
  const std::array structureChanged{source};
  prepareChanges(affected, structureChanged);
  source->relations_.swap(nextRelations);
}

void NodeGraph::WriteAccess::unrelate(const NodeRef &source, RelationKind kind,
                                      const NodeRef &target) {
  requireLive(source);
  requireLive(target);
  const auto found = source->relations_.find(kind);
  if (found == source->relations_.end() ||
      !contains(found->second, target.get()))
    return;
  auto nextRelations = source->relations_;
  auto next = nextRelations.find(kind);
  eraseValue(next->second, target.get());
  if (next->second.empty())
    nextRelations.erase(next);
  const std::array affected{source, target};
  const std::array structureChanged{source};
  prepareChanges(affected, structureChanged);
  source->relations_.swap(nextRelations);
}

void NodeGraph::WriteAccess::replaceRelated(const NodeRef &source,
                                            RelationKind kind,
                                            std::span<const NodeRef> targets) {
  requireLive(source);
  std::vector<NodeRef> next;
  next.reserve(targets.size());
  std::unordered_set<const Node *> seen;
  seen.reserve(targets.size());
  for (const NodeRef &target : targets) {
    requireLive(target);
    if (seen.insert(target.get()).second)
      next.emplace_back(target);
  }

  const auto found = source->relations_.find(kind);
  const std::vector<Node *> previous =
      found == source->relations_.end() ? std::vector<Node *>{} : found->second;
  bool unchanged = previous.size() == next.size();
  if (unchanged) {
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (previous[index] != next[index].get()) {
        unchanged = false;
        break;
      }
    }
  }
  if (unchanged)
    return;

  auto nextRelations = source->relations_;
  if (next.empty())
    nextRelations.erase(kind);
  else {
    std::vector<Node *> ordered;
    ordered.reserve(next.size());
    for (const NodeRef &target : next)
      ordered.emplace_back(target.get());
    nextRelations.insert_or_assign(kind, std::move(ordered));
  }
  std::vector<NodeRef> affected;
  affected.reserve(1 + previous.size() + next.size());
  affected.emplace_back(source);
  for (Node *target : previous)
    affected.emplace_back(pin(target));
  for (const NodeRef &target : next)
    affected.emplace_back(target);
  const std::array structureChanged{source};
  prepareChanges(affected, structureChanged);
  source->relations_.swap(nextRelations);
}

void NodeGraph::WriteAccess::remove(const NodeRef &node) {
  const std::array nodes{node};
  removeMany(nodes);
}

void NodeGraph::WriteAccess::removeMany(std::span<const NodeRef> nodes) {
  if (nodes.empty())
    return;

  // Validate and allocate every replacement container before changing a node.
  // Once topology mutation starts, the remainder of this function consists
  // only of erases, pointer assignments, and noexcept container swaps.
  std::vector<NodeRef> removalOrder;
  removalOrder.reserve(nodes.size());
  std::unordered_set<Node *> removalSet;
  removalSet.reserve(nodes.size());
  for (const NodeRef &node : nodes) {
    requireLive(node);
    if (removalSet.insert(node.get()).second)
      removalOrder.emplace_back(node);
  }
  if (removalOrder.empty())
    return;

  std::vector<NodeRef> survivingOrder;
  survivingOrder.reserve(graph_->orderedNodes_.size() - removalOrder.size());
  std::vector<NodeRef> topologyChanged;
  topologyChanged.reserve(graph_->orderedNodes_.size());
  std::unordered_set<Node *> topologyChangedSet;
  topologyChangedSet.reserve(graph_->orderedNodes_.size());
  std::unordered_set<Node *> removedStructureSet;
  removedStructureSet.reserve(removalOrder.size());

  for (const NodeRef &candidate : graph_->orderedNodes_) {
    const bool removing = removalSet.contains(candidate.get());
    if (!removing)
      survivingOrder.emplace_back(candidate);

    bool structureChanged =
        candidate->parent_ && removalSet.contains(candidate->parent_);
    structureChanged =
        structureChanged ||
        std::ranges::any_of(candidate->children_, [&removalSet](Node *child) {
          return removalSet.contains(child);
        });
    structureChanged =
        structureChanged ||
        std::ranges::any_of(candidate->relations_, [&removalSet](
                                                       const auto &entry) {
          return std::ranges::any_of(entry.second, [&removalSet](Node *target) {
            return removalSet.contains(target);
          });
        });
    if (removing) {
      if (candidate->parent_ || !candidate->children_.empty() ||
          !candidate->relations_.empty())
        removedStructureSet.insert(candidate.get());
    } else if (structureChanged &&
               topologyChangedSet.insert(candidate.get()).second) {
      topologyChanged.emplace_back(candidate);
    }
  }

  auto nextNodes = graph_->nodes_;
  for (const NodeRef &node : removalOrder)
    nextNodes.erase(node->id_);

  auto nextRetiredNodes = graph_->retiredNodes_;
  auto nextRetiredIndex = graph_->retiredIndex_;
  nextRetiredNodes.reserve(nextRetiredNodes.size() + removalOrder.size());
  nextRetiredIndex.reserve(nextRetiredIndex.size() + removalOrder.size());
  for (const NodeRef &node : removalOrder) {
    nextRetiredIndex.emplace(node.get(), nextRetiredNodes.size());
    nextRetiredNodes.emplace_back(node);
  }

  auto nextAffected = affected_;
  auto nextAffectedIndex = affectedIndex_;
  nextAffected.reserve(nextAffected.size() + topologyChanged.size());
  nextAffectedIndex.reserve(nextAffectedIndex.size() + topologyChanged.size());
  for (const NodeRef &node : topologyChanged)
    if (nextAffectedIndex.insert(node.get()).second)
      nextAffected.emplace_back(node);

  auto nextStructureRevisions = pendingStructureRevisions_;
  nextStructureRevisions.reserve(nextStructureRevisions.size() +
                                 topologyChanged.size() +
                                 removedStructureSet.size());
  for (const NodeRef &node : topologyChanged)
    nextStructureRevisions.insert(node.get());
  for (Node *node : removedStructureSet)
    nextStructureRevisions.insert(node);

  auto nextRemoved = removed_;
  auto nextRemovedIndex = removedIndex_;
  nextRemoved.reserve(nextRemoved.size() + removalOrder.size());
  nextRemovedIndex.reserve(nextRemovedIndex.size() + removalOrder.size());
  for (const NodeRef &node : removalOrder)
    if (nextRemovedIndex.insert(node.get()).second)
      nextRemoved.emplace_back(node);

  for (const NodeRef &candidate : graph_->orderedNodes_) {
    if (removalSet.contains(candidate.get())) {
      candidate->parent_ = nullptr;
      candidate->children_.clear();
      candidate->relations_.clear();
      candidate->removed_ = true;
      continue;
    }
    if (candidate->parent_ && removalSet.contains(candidate->parent_))
      candidate->parent_ = nullptr;
    std::erase_if(candidate->children_, [&removalSet](Node *child) {
      return removalSet.contains(child);
    });
    for (auto relation = candidate->relations_.begin();
         relation != candidate->relations_.end();) {
      std::erase_if(relation->second, [&removalSet](Node *target) {
        return removalSet.contains(target);
      });
      if (relation->second.empty())
        relation = candidate->relations_.erase(relation);
      else
        ++relation;
    }
  }

  graph_->nodes_.swap(nextNodes);
  graph_->orderedNodes_.swap(survivingOrder);
  graph_->retiredNodes_.swap(nextRetiredNodes);
  graph_->retiredIndex_.swap(nextRetiredIndex);
  affected_.swap(nextAffected);
  affectedIndex_.swap(nextAffectedIndex);
  pendingStructureRevisions_.swap(nextStructureRevisions);
  removed_.swap(nextRemoved);
  removedIndex_.swap(nextRemovedIndex);
  dirty_ = true;
}

void NodeGraph::WriteAccess::releaseRetired(std::span<const NodeRef> nodes) {
  for (const NodeRef &node : nodes) {
    if (!node)
      continue;
    const auto found = graph_->retiredIndex_.find(node.get());
    if (found == graph_->retiredIndex_.end())
      continue;
    const std::size_t index = found->second;
    const std::size_t last = graph_->retiredNodes_.size() - 1;
    if (index != last) {
      graph_->retiredNodes_[index] = std::move(graph_->retiredNodes_.back());
      graph_->retiredIndex_.at(graph_->retiredNodes_[index].get()) = index;
    }
    graph_->retiredNodes_.pop_back();
    graph_->retiredIndex_.erase(found);
    ++graph_->retiredOrderGeneration_;
  }
}

GraphChange NodeGraph::WriteAccess::finish() {
  if (finished_)
    return GraphChange{graph_ ? graph_->revision_ : 0, {}, {}};
  GraphChange change = publish();
  if (lock_.owns_lock())
    lock_.unlock();
  return change;
}

void NodeGraph::WriteAccess::requireLive(const NodeRef &node) const {
  if (!graph_ || !lock_.owns_lock() || finished_)
    throw std::logic_error("graph write access is not active");
  if (!node || node->removed_)
    throw std::invalid_argument("node is not live");
  const auto found = graph_->nodes_.find(node->id_);
  if (found == graph_->nodes_.end() || found->second != node)
    throw std::invalid_argument("node does not belong to this graph");
}

void NodeGraph::WriteAccess::prepareChanges(
    std::span<const NodeRef> affected,
    std::span<const NodeRef> structureChanged) {
  const bool wasDirty = dirty_;
  const std::size_t affectedSize = affected_.size();
  std::vector<Node *> addedAffected;
  std::vector<Node *> addedStructure;
  addedAffected.reserve(affected.size());
  addedStructure.reserve(structureChanged.size());
  ensureAppendCapacity(affected_, affected.size());
  affectedIndex_.reserve(affectedIndex_.size() + affected.size());
  pendingStructureRevisions_.reserve(pendingStructureRevisions_.size() +
                                     structureChanged.size());

  try {
    for (const NodeRef &node : affected) {
      if (!node)
        continue;
      const auto [position, inserted] = affectedIndex_.insert(node.get());
      static_cast<void>(position);
      if (!inserted)
        continue;
      try {
        affected_.emplace_back(node);
      } catch (...) {
        affectedIndex_.erase(node.get());
        throw;
      }
      addedAffected.emplace_back(node.get());
    }
    for (const NodeRef &node : structureChanged) {
      if (node && pendingStructureRevisions_.insert(node.get()).second)
        addedStructure.emplace_back(node.get());
    }
  } catch (...) {
    affected_.resize(affectedSize);
    for (Node *node : addedAffected)
      affectedIndex_.erase(node);
    for (Node *node : addedStructure)
      pendingStructureRevisions_.erase(node);
    dirty_ = wasDirty;
    throw;
  }
  dirty_ = true;
}

GraphChange NodeGraph::WriteAccess::publish() {
  finished_ = true;
  if (!dirty_)
    return GraphChange{graph_->revision_, {}, {}};
  ++graph_->revision_;
  for (auto &[node, pending] : pendingStateRevisions_) {
    if (pending.status)
      node->statusChangedRevision_ = graph_->revision_;
    for (const std::string &field : pending.fields)
      node->fieldChangedRevisions_.insert_or_assign(field, graph_->revision_);
  }
  std::array<bool, NodeGraph::NodeKindCount> changedStructureKinds{};
  for (Node *node : pendingStructureRevisions_) {
    node->structureChangedRevision_ = graph_->revision_;
    changedStructureKinds[static_cast<std::size_t>(node->id_.kind)] = true;
  }
  for (std::size_t index = 0; index < changedStructureKinds.size(); ++index) {
    if (!changedStructureKinds[index])
      continue;
    graph_->structureRevisions_[index] = graph_->revision_;
    graph_->publishedStructureRevisions_[index].store(
        graph_->revision_, std::memory_order_release);
  }
  for (const NodeRef &node : affected_)
    node->changedRevision_ = graph_->revision_;
  for (const NodeRef &node : revisionTouches_)
    node->changedRevision_ = graph_->revision_;
  for (const NodeRef &node : removed_)
    node->changedRevision_ = graph_->revision_;
  graph_->publishedRevision_.store(graph_->revision_,
                                   std::memory_order_release);
  return GraphChange{graph_->revision_, std::move(affected_),
                     std::move(removed_)};
}

} // namespace codexui::nodegraph
