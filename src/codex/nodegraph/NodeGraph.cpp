// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/NodeGraph.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <stdexcept>

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

} // namespace

std::size_t NodeIdHash::operator()(const NodeId &id) const noexcept {
  const std::size_t kind = static_cast<std::size_t>(id.kind);
  const std::size_t value = std::hash<std::string>{}(id.canonical);
  return value ^ (kind + 0x9e3779b9U + (value << 6U) + (value >> 2U));
}

Node::Node(NodeId id, NodeState state)
    : id_(std::move(id)),
      state_(std::make_shared<const NodeState>(std::move(state))) {}

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

NodeGraph::ReadAccess::ReadAccess(
    const NodeGraph &graph, std::shared_lock<std::shared_mutex> lock) noexcept
    : graph_(&graph), lock_(std::move(lock)) {}

std::uint64_t NodeGraph::ReadAccess::revision() const noexcept {
  return graph_->revision_;
}

NodeRef NodeGraph::ReadAccess::find(const NodeId &id) const {
  const auto found = graph_->nodes_.find(id);
  return found == graph_->nodes_.end() ? NodeRef{} : found->second;
}

const std::vector<NodeRef> &
NodeGraph::ReadAccess::orderedNodes() const noexcept {
  return graph_->orderedNodes_;
}

const std::vector<NodeRef> &
NodeGraph::ReadAccess::retiredNodes() const noexcept {
  return graph_->retiredNodes_;
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
  const auto active = graph_->nodes_.find(node->id_);
  if ((active != graph_->nodes_.end() && active->second == node) ||
      contains(graph_->retiredNodes_, node))
    return;
  throw std::invalid_argument("node does not belong to this graph");
}

NodeGraph::WriteAccess::WriteAccess(
    NodeGraph &graph, std::unique_lock<std::shared_mutex> lock) noexcept
    : graph_(&graph), lock_(std::move(lock)) {}

NodeGraph::WriteAccess::WriteAccess(WriteAccess &&other) noexcept
    : graph_(std::exchange(other.graph_, nullptr)),
      lock_(std::move(other.lock_)), affected_(std::move(other.affected_)),
      removed_(std::move(other.removed_)), dirty_(other.dirty_),
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

NodeRef NodeGraph::WriteAccess::upsert(NodeId id, NodeState initial) {
  if (NodeRef existing = find(id))
    return existing;
  NodeRef node(new Node(std::move(id), std::move(initial)));
  graph_->nodes_.emplace(node->id_, node);
  graph_->orderedNodes_.emplace_back(node);
  markAffected(node);
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
  node->state_ = std::make_shared<const NodeState>(std::move(state));
  markAffected(node);
}

void NodeGraph::WriteAccess::setField(const NodeRef &node, std::string key,
                                      Value value) {
  requireLive(node);
  NodeState next = *node->state_;
  const auto found = next.fields.find(key);
  if (found != next.fields.end() && found->second == value)
    return;
  next.fields.insert_or_assign(std::move(key), std::move(value));
  replaceState(node, std::move(next));
}

void NodeGraph::WriteAccess::eraseField(const NodeRef &node,
                                        std::string_view key) {
  requireLive(node);
  NodeState next = *node->state_;
  if (next.fields.erase(std::string(key)) == 0)
    return;
  replaceState(node, std::move(next));
}

void NodeGraph::WriteAccess::appendStringField(const NodeRef &node,
                                               std::string key,
                                               std::string_view suffix) {
  requireLive(node);
  NodeState next = *node->state_;
  Value &field = next.fields[std::move(key)];
  std::string combined;
  if (const std::string *current = field.asString())
    combined = *current;
  combined.append(suffix);
  field = Value(std::move(combined));
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
  clearParent(child);
  child->parent_ = parent.get();
  if (!contains(parent->children_, child.get()))
    parent->children_.emplace_back(child.get());
  markAffected(parent);
  markAffected(child);
}

void NodeGraph::WriteAccess::clearParent(const NodeRef &child) {
  requireLive(child);
  if (!child->parent_)
    return;
  NodeRef parent = pin(child->parent_);
  eraseValue(parent->children_, child.get());
  child->parent_ = nullptr;
  markAffected(parent);
  markAffected(child);
}

void NodeGraph::WriteAccess::replaceChildren(
    const NodeRef &parent, std::span<const NodeRef> children) {
  requireLive(parent);
  std::vector<NodeRef> next;
  next.reserve(children.size());
  for (const NodeRef &child : children) {
    requireLive(child);
    if (parent == child)
      throw std::invalid_argument("a node cannot parent itself");
    for (Node *ancestor = parent.get(); ancestor;
         ancestor = ancestor->parent_) {
      if (ancestor == child.get())
        throw std::invalid_argument("a parent relation cannot form a cycle");
    }
    if (!contains(next, child))
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
  for (Node *oldChildPointer : previous) {
    if (std::ranges::none_of(next, [oldChildPointer](const NodeRef &child) {
          return child.get() == oldChildPointer;
        })) {
      NodeRef oldChild = pin(oldChildPointer);
      oldChild->parent_ = nullptr;
      markAffected(oldChild);
    }
  }

  for (const NodeRef &child : next) {
    if (child->parent_ && child->parent_ != parent.get()) {
      NodeRef previousParent = pin(child->parent_);
      eraseValue(previousParent->children_, child.get());
      markAffected(previousParent);
    }
    child->parent_ = parent.get();
    markAffected(child);
  }
  parent->children_.clear();
  parent->children_.reserve(next.size());
  for (const NodeRef &child : next)
    parent->children_.emplace_back(child.get());
  markAffected(parent);
}

void NodeGraph::WriteAccess::relate(const NodeRef &source, RelationKind kind,
                                    const NodeRef &target) {
  requireLive(source);
  requireLive(target);
  auto &targets = source->relations_[kind];
  if (contains(targets, target.get()))
    return;
  targets.emplace_back(target.get());
  markAffected(source);
  markAffected(target);
}

void NodeGraph::WriteAccess::unrelate(const NodeRef &source, RelationKind kind,
                                      const NodeRef &target) {
  requireLive(source);
  requireLive(target);
  const auto found = source->relations_.find(kind);
  if (found == source->relations_.end() ||
      !contains(found->second, target.get()))
    return;
  eraseValue(found->second, target.get());
  if (found->second.empty())
    source->relations_.erase(found);
  markAffected(source);
  markAffected(target);
}

void NodeGraph::WriteAccess::replaceRelated(const NodeRef &source,
                                            RelationKind kind,
                                            std::span<const NodeRef> targets) {
  requireLive(source);
  std::vector<NodeRef> next;
  next.reserve(targets.size());
  for (const NodeRef &target : targets) {
    requireLive(target);
    if (!contains(next, target))
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

  if (next.empty()) {
    source->relations_.erase(kind);
  } else {
    std::vector<Node *> ordered;
    ordered.reserve(next.size());
    for (const NodeRef &target : next)
      ordered.emplace_back(target.get());
    source->relations_.insert_or_assign(kind, std::move(ordered));
  }
  markAffected(source);
  for (Node *target : previous)
    markAffected(pin(target));
  for (const NodeRef &target : next)
    markAffected(target);
}

void NodeGraph::WriteAccess::unlinkNode(const NodeRef &node) {
  if (node->parent_) {
    NodeRef parent = pin(node->parent_);
    eraseValue(parent->children_, node.get());
    node->parent_ = nullptr;
    markAffected(parent);
  }
  for (Node *childPointer : node->children_) {
    NodeRef child = pin(childPointer);
    child->parent_ = nullptr;
    markAffected(child);
  }
  node->children_.clear();

  for (const NodeRef &candidate : graph_->orderedNodes_) {
    if (candidate == node)
      continue;
    bool changed = false;
    for (auto relation = candidate->relations_.begin();
         relation != candidate->relations_.end();) {
      const std::size_t before = relation->second.size();
      eraseValue(relation->second, node.get());
      changed = changed || relation->second.size() != before;
      if (relation->second.empty())
        relation = candidate->relations_.erase(relation);
      else
        ++relation;
    }
    if (changed)
      markAffected(candidate);
  }
  node->relations_.clear();
}

void NodeGraph::WriteAccess::remove(const NodeRef &node) {
  requireLive(node);
  unlinkNode(node);
  graph_->nodes_.erase(node->id_);
  eraseValue(graph_->orderedNodes_, node);
  node->removed_ = true;
  if (!contains(graph_->retiredNodes_, node))
    graph_->retiredNodes_.emplace_back(node);
  if (!contains(removed_, node))
    removed_.emplace_back(node);
  dirty_ = true;
}

void NodeGraph::WriteAccess::releaseRetired(std::span<const NodeRef> nodes) {
  for (const NodeRef &node : nodes) {
    const auto found = std::find(graph_->retiredNodes_.begin(),
                                 graph_->retiredNodes_.end(), node);
    if (found == graph_->retiredNodes_.end())
      continue;
    graph_->retiredNodes_.erase(found);
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

void NodeGraph::WriteAccess::markAffected(const NodeRef &node) {
  if (node && !contains(affected_, node))
    affected_.emplace_back(node);
  dirty_ = true;
}

GraphChange NodeGraph::WriteAccess::publish() {
  finished_ = true;
  if (!dirty_)
    return GraphChange{graph_->revision_, {}, {}};
  ++graph_->revision_;
  for (const NodeRef &node : affected_)
    node->changedRevision_ = graph_->revision_;
  for (const NodeRef &node : removed_)
    node->changedRevision_ = graph_->revision_;
  graph_->publishedRevision_.store(graph_->revision_,
                                   std::memory_order_release);
  return GraphChange{graph_->revision_, std::move(affected_),
                     std::move(removed_)};
}

} // namespace codexui::nodegraph
