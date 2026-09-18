// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/NodeGraph.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace codexui::nodegraph {
namespace {

template <typename Range, typename ValueType>
bool contains(const Range &range, const ValueType &value) {
  return std::find(range.begin(), range.end(), value) != range.end();
}

bool scalarTextNonEmpty(const Value *value) noexcept {
  if (const std::string *text = value ? value->asString() : nullptr)
    return !text->empty();
  return value && (value->isSigned() || value->isUnsigned());
}

std::string_view exactText(const Value *value) noexcept {
  const std::string *text = value ? value->asString() : nullptr;
  return text ? std::string_view(*text) : std::string_view{};
}

template <typename Range, typename ValueType>
void eraseValue(Range &range, const ValueType &value) {
  range.erase(std::remove(range.begin(), range.end(), value), range.end());
}

template <typename T, typename Projection, typename Equal = std::equal_to<>>
void sortUnique(std::vector<T> &values, Projection projection,
                Equal equal = {}) {
  std::ranges::sort(values, {}, projection);
  values.erase(std::unique(values.begin(), values.end(), equal), values.end());
}

NodeRef pin(Node *node) { return node ? node->shared_from_this() : NodeRef{}; }

void requireRelationTargetKind(RelationKind kind, const NodeRef &target) {
  if (kind == RelationKind::PendingOperation &&
      target->id().kind != NodeKind::Operation)
    throw std::invalid_argument(
        "PendingOperation relations require an Operation target");
  if (kind == RelationKind::PendingInteraction &&
      target->id().kind != NodeKind::Interaction)
    throw std::invalid_argument(
        "PendingInteraction relations require an Interaction target");
}

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

std::string_view statusTextFromValue(const Value *value) {
  if (const Value::Object *object = value ? value->asObject() : nullptr) {
    const auto type = object->find("type");
    value = type == object->end() ? nullptr : &type->second;
  }
  const std::string *status = value ? value->asString() : nullptr;
  return status ? std::string_view(*status) : std::string_view{};
}

NodeStatus nodeStatusFromValue(const Value *value) {
  return nodeStatusFromText(statusTextFromValue(value));
}

NodeStatusView nodeStatusView(const NodeState &state) {
  const std::string_view raw = statusTextFromValue(valueMember(state, "status"));
  return {state.status, state.status == NodeStatus::Unknown ? raw
                                                           : std::string_view{}};
}

NodeStatusView nodeStatusView(const Value *value) {
  const std::string_view raw = statusTextFromValue(value);
  const NodeStatus status = nodeStatusFromText(raw);
  return {status, status == NodeStatus::Unknown ? raw : std::string_view{}};
}

NodeStatusView agentActivityStatus(const NodeState &state) {
  const NodeStatusView published = nodeStatusView(state);
  if (!published.empty())
    return published;
  const std::string kind = scalarTextFromValue(valueMember(state, "kind"));
  if (kind == "completed")
    return {NodeStatus::Completed, {}};
  if (kind == "interrupted")
    return {NodeStatus::Interrupted, {}};
  if (kind == "failed")
    return {NodeStatus::Failed, {}};
  if (kind == "started" || kind == "progress")
    return {NodeStatus::Running, {}};
  return {};
}

bool agentActivityCanCreate(const NodeState &state) {
  const std::string_view type = exactText(valueMember(state, "type"));
  if (type == "subAgentActivity") {
    const std::string_view kind = exactText(valueMember(state, "kind"));
    return kind.empty() || kind == "started";
  }
  if (type != "collabAgentToolCall")
    return false;
  const std::string_view tool = exactText(valueMember(state, "tool"));
  return tool == "spawn_agent" || tool == "spawnAgent" ||
         tool == "spawn_agents_on_csv" || tool == "spawnAgentsOnCsv";
}

InspectorPlanView inspectorPlanView(const NodeState &state) noexcept {
  const Value *plan = valueMember(state, "plan");
  const Value *steps = plan && plan->asArray() ? plan : nullptr;
  const Value *explanation = valueMember(state, "planExplanation");
  if (!scalarTextNonEmpty(explanation))
    explanation = nullptr;
  if (const auto *object = plan ? plan->asObject() : nullptr) {
    steps = valueMember(*object, "steps");
    if (!explanation) {
      const Value *nested = valueMember(*object, "explanation");
      if (scalarTextNonEmpty(nested))
        explanation = nested;
    }
  }
  if (!steps)
    return {};
  const auto *array = steps->asArray();
  std::size_t rows = array ? array->size() : 0;
  if (explanation && rows != std::numeric_limits<std::size_t>::max())
    ++rows;
  return {steps, explanation, rows};
}

const Value::Object *reportedAgentState(const NodeState &state,
                                        std::string_view childId) noexcept {
  const Value *value = valueMember(state, "agentsStates");
  const auto *states = value ? value->asObject() : nullptr;
  if (!states)
    return nullptr;
  const auto found = states->find(childId);
  return found == states->end() ? nullptr : found->second.asObject();
}

NodeStatusView inspectorAgentActivityStatus(const NodeState &state,
                                            const NodeState *turn,
                                            NodeStatusView child,
                                            bool childIdVisible) noexcept {
  NodeStatusView result = agentActivityStatus(state);
  const bool terminalTurn =
      turn && (turn->status == NodeStatus::Completed ||
               turn->status == NodeStatus::Failed ||
               turn->status == NodeStatus::Interrupted);
  if (childIdVisible && result.semantic == NodeStatus::Running &&
      terminalTurn && child.empty())
    return {NodeStatus::NotLoaded, {}};
  return result;
}

Node::Node(NodeId id, NodeState state, std::uint64_t insertionOrder)
    : id_(std::move(id)),
      state_(std::make_shared<const NodeState>(std::move(state))),
      insertionOrder_(insertionOrder) {}

const NodeId &Node::id() const noexcept { return id_; }

bool GraphChange::empty() const noexcept {
  return affected.empty() && removed.empty() && childListsChanged.empty() &&
         providerAuthorityRevision == 0;
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

const std::vector<NodeRef> &
NodeGraph::ReadAccess::orderedNodes(NodeKind kind) const noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  static const std::vector<NodeRef> empty;
  return index < graph_->orderedNodesByKind_.size()
             ? graph_->orderedNodesByKind_[index]
             : empty;
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

std::uint64_t
NodeGraph::ReadAccess::pendingInteractionOrderRevision() const noexcept {
  return graph_->pendingInteractionOrderRevision_;
}

bool NodeGraph::ReadAccess::live(const NodeRef &node) const noexcept {
  if (!node)
    return false;
  const auto active = graph_->nodes_.find(node->id_);
  return active != graph_->nodes_.end() && active->second == node;
}

bool NodeGraph::ReadAccess::contains(const NodeRef &node) const noexcept {
  if (live(node))
    return true;
  if (!node)
    return false;
  const std::size_t index = node->parentIndex_.load(std::memory_order_relaxed);
  return index < graph_->retiredNodes_.size() &&
         graph_->retiredNodes_[index] == node;
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

bool NodeGraph::ReadAccess::fieldsChangedAt(const NodeRef &node,
                                            std::uint64_t revision) const {
  if (!node)
    return false;
  requireMember(node);
  return std::ranges::any_of(node->fieldChangedRevisions_,
                             [revision](const auto &field) {
                               return field.second == revision;
                             });
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

NodeRef NodeGraph::ReadAccess::parent(const NodeRef &node) const {
  if (!node)
    return {};
  requireMember(node);
  return pin(node->parent_);
}

std::optional<std::size_t>
NodeGraph::ReadAccess::childIndex(const NodeRef &node) const {
  if (!node)
    return std::nullopt;
  requireMember(node);
  const std::size_t index = node->parentIndex_.load(std::memory_order_relaxed);
  if (!node->parent_ || index >= node->parent_->children_.size() ||
      node->parent_->children_[index] != node.get())
    return std::nullopt;
  return index;
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

bool NodeGraph::ReadAccess::isRelated(const NodeRef &source,
                                      RelationKind kind,
                                      const NodeRef &target) const {
  if (!source || !target)
    return false;
  requireMember(source);
  requireMember(target);
  return std::ranges::find(target->incomingRelations_,
                           std::pair{source.get(), kind}) !=
         target->incomingRelations_.end();
}

bool NodeGraph::ReadAccess::hasIncomingRelation(const NodeRef &target,
                                                RelationKind kind) const {
  if (!target)
    return false;
  requireMember(target);
  return std::ranges::any_of(
      target->incomingRelations_,
      [kind](const auto &relation) { return relation.second == kind; });
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

const InspectorThreadIndex *
NodeGraph::ReadAccess::inspectorIndex(const NodeRef &thread) const {
  if (!thread)
    return nullptr;
  requireMember(thread);
  const auto found = graph_->inspectorIndexes_.find(thread.get());
  return found == graph_->inspectorIndexes_.end() ? nullptr : &found->second;
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
      childListsChanged_(std::move(other.childListsChanged_)),
      childListsChangedIndex_(std::move(other.childListsChangedIndex_)),
      inspectorImpacts_(std::move(other.inspectorImpacts_)),
      removed_(std::move(other.removed_)),
      pendingInteractionOrderChanged_(other.pendingInteractionOrderChanged_),
      dirty_(other.dirty_),
      finished_(other.finished_) {
  other.pendingInteractionOrderChanged_ = false;
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
  const std::size_t kindIndex = static_cast<std::size_t>(node->id_.kind);
  if (kindIndex >= graph_->orderedNodesByKind_.size())
    throw std::invalid_argument("node kind is outside the graph index");
  ensureAppendCapacity(graph_->orderedNodesByKind_[kindIndex], 1);
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
  graph_->orderedNodesByKind_[kindIndex].emplace_back(node);
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
  if (node->id_.kind == NodeKind::Interaction &&
      after.status != NodeStatus::Pending && after.status != NodeStatus::Failed &&
      std::ranges::any_of(node->incomingRelations_, [](const auto &edge) {
        return edge.second == RelationKind::PendingInteraction;
      }))
    throw std::invalid_argument(
        "a pending Interaction must be unlinked before it becomes terminal");

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

Node *NodeGraph::WriteAccess::inspectorThread(Node *node) const noexcept {
  while (node && node->id_.kind != NodeKind::Thread)
    node = node->parent_;
  return node;
}

std::uint8_t NodeGraph::WriteAccess::inspectorItemSections(Node *item) const {
  if (!item || item->id_.kind != NodeKind::Item)
    return 0;
  const std::string type =
      scalarTextFromValue(valueMember(*item->state_, "type"));
  std::uint8_t sections = type == "plan" ? 1 : 0;
  if (type == "subAgentActivity" || type == "collabAgentToolCall" ||
      type == "agentMessage")
    sections |= 2;
  return sections;
}

std::uint8_t NodeGraph::WriteAccess::inspectorTurnSections(Node *turn) const {
  if (!turn || turn->id_.kind != NodeKind::Turn)
    return 0;
  std::uint8_t sections = inspectorPlanView(*turn->state_) ? 1 : 0;
  for (Node *item : turn->children_)
    sections |= inspectorItemSections(item);
  return sections;
}

std::uint8_t NodeGraph::WriteAccess::inspectorIndexedSections(
    Node *thread, Node *node) const {
  const auto indexed = graph_->inspectorIndexes_.find(thread);
  if (!node || indexed == graph_->inspectorIndexes_.end())
    return 0;
  const InspectorThreadIndex &index = indexed->second;
  std::uint8_t sections = index.planSource.get() == node ? 1 : 0;
  if (index.planSource && index.planSource->id_.kind == NodeKind::Item &&
      index.planSource->parent_ == node)
    sections |= 1;
  if (index.agentDependencies.contains(node) ||
      index.agentMessageOrder.contains(node))
    sections |= 2;
  return sections;
}

void NodeGraph::WriteAccess::markInspectorRebuild(Node *thread,
                                                   std::uint8_t sections) {
  if (!thread || thread->id_.kind != NodeKind::Thread || sections == 0)
    return;
  InspectorImpact &impact = inspectorImpacts_[thread];
  impact.rebuild |= sections;
  if (sections & 2)
    impact.append.reset();
}

bool NodeGraph::WriteAccess::markInspectorAppend(Node *thread, Node *turn,
                                                 std::size_t first) {
  if (!thread || !turn || thread->id_.kind != NodeKind::Thread)
    return false;
  const auto lastTurn = std::ranges::find_if(
      thread->children_.rbegin(), thread->children_.rend(),
      [](Node *node) { return node->id_.kind == NodeKind::Turn; });
  if (lastTurn == thread->children_.rend() || *lastTurn != turn)
    return false;
  InspectorImpact &impact = inspectorImpacts_[thread];
  if (impact.rebuild & 2)
    return true;
  if (!impact.append) {
    impact.append = InspectorImpact::AgentAppend{turn, first};
  } else if (impact.append->turn == turn) {
    impact.append->first = std::min(impact.append->first, first);
  } else {
    impact.rebuild |= 2;
    impact.append.reset();
  }
  return true;
}

void NodeGraph::WriteAccess::noteInspectorChildChange(Node *parent,
                                                       Node *child,
                                                       bool appended) {
  if (!parent || !child)
    return;
  if (parent->id_.kind == NodeKind::Turn &&
      child->id_.kind == NodeKind::Item) {
    Node *thread = inspectorThread(parent);
    std::uint8_t sections = inspectorItemSections(child) |
                            inspectorIndexedSections(thread, child);
    if (appended && child->changedRevision_ == 0 &&
        markInspectorAppend(thread, parent, parent->children_.size()))
      sections &= static_cast<std::uint8_t>(~2U);
    markInspectorRebuild(thread, sections);
  } else if (parent->id_.kind == NodeKind::Thread &&
             child->id_.kind == NodeKind::Turn) {
    markInspectorRebuild(parent, inspectorTurnSections(child) |
                                     inspectorIndexedSections(parent, child));
  }
}

void NodeGraph::WriteAccess::noteInspectorChildrenReplacement(
    Node *parent, std::span<Node *const> previous,
    std::span<const NodeRef> next) {
  if (!parent)
    return;
  for (const NodeRef &child : next)
    if (child && child->parent_ && child->parent_ != parent)
      noteInspectorChildChange(child->parent_, child.get(), false);

  bool appended = previous.size() <= next.size();
  for (std::size_t index = 0; appended && index < previous.size(); ++index)
    appended = previous[index] == next[index].get();
  for (std::size_t index = previous.size(); appended && index < next.size();
       ++index)
    appended = next[index]->changedRevision_ == 0 && !next[index]->parent_;

  std::uint8_t sections = 0;
  if (parent->id_.kind == NodeKind::Turn) {
    Node *thread = inspectorThread(parent);
    const std::size_t first = appended ? previous.size() : 0;
    if (appended) {
      for (std::size_t index = first; index < next.size(); ++index)
        sections |= inspectorItemSections(next[index].get());
    } else {
      for (Node *child : previous)
        sections |= inspectorItemSections(child) |
                    inspectorIndexedSections(thread, child);
      for (const NodeRef &child : next)
        sections |= inspectorItemSections(child.get());
    }
    if (appended && first < next.size() &&
        markInspectorAppend(thread, parent, first))
      sections &= static_cast<std::uint8_t>(~2U);
    markInspectorRebuild(thread, sections);
  } else if (parent->id_.kind == NodeKind::Thread) {
    for (Node *child : previous)
      sections |= inspectorTurnSections(child) |
                  inspectorIndexedSections(parent, child);
    for (const NodeRef &child : next)
      sections |= inspectorTurnSections(child.get());
    markInspectorRebuild(parent, sections);
  }
}

void NodeGraph::WriteAccess::noteInspectorRelationChange(
    Node *source, RelationKind kind) {
  if (!source || source->id_.kind != NodeKind::Item ||
      kind != RelationKind::AgentChildThread || source->changedRevision_ == 0)
    return;
  markInspectorRebuild(inspectorThread(source), 2);
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
  noteInspectorChildChange(previousParent.get(), child.get(), false);
  noteInspectorChildChange(parent.get(), child.get(), true);
  const std::array changed{parent, child, previousParent};
  const std::array childrenChanged{
      ChildListChange{parent, child->id().kind},
      ChildListChange{previousParent, child->id().kind}};
  prepareChanges(changed, changed, childrenChanged);
  if (previousParent) {
    eraseValue(previousParent->children_, child.get());
    for (std::size_t index = 0; index < previousParent->children_.size();
         ++index)
      previousParent->children_[index]->parentIndex_.store(
          index, std::memory_order_relaxed);
  }
  child->parent_ = parent.get();
  child->parentIndex_.store(parent->children_.size(),
                            std::memory_order_relaxed);
  parent->children_.emplace_back(child.get());
}

void NodeGraph::WriteAccess::clearParent(const NodeRef &child) {
  requireLive(child);
  if (!child->parent_)
    return;
  NodeRef parent = pin(child->parent_);
  noteInspectorChildChange(parent.get(), child.get(), false);
  const std::array changed{parent, child};
  const std::array childrenChanged{ChildListChange{parent, child->id().kind}};
  prepareChanges(changed, changed, childrenChanged);
  eraseValue(parent->children_, child.get());
  for (std::size_t index = 0; index < parent->children_.size(); ++index)
    parent->children_[index]->parentIndex_.store(index,
                                                 std::memory_order_relaxed);
  child->parent_ = nullptr;
  child->parentIndex_.store(0, std::memory_order_relaxed);
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
  noteInspectorChildrenReplacement(parent.get(), previous, next);
  std::vector<NodeRef> affected{parent};
  std::vector<NodeRef> structureChanged{parent};
  std::array<std::vector<Node *>, NodeGraph::NodeKindCount> previousByKind;
  std::array<std::vector<Node *>, NodeGraph::NodeKindCount> nextByKind;
  for (Node *child : previous)
    previousByKind[static_cast<std::size_t>(child->id().kind)].push_back(child);
  for (const NodeRef &child : next)
    nextByKind[static_cast<std::size_t>(child->id().kind)].push_back(
        child.get());
  std::vector<ChildListChange> childrenChanged;
  for (std::size_t kind = 0; kind < NodeGraph::NodeKindCount; ++kind)
    if (previousByKind[kind] != nextByKind[kind])
      childrenChanged.push_back({parent, static_cast<NodeKind>(kind)});
  affected.reserve(1 + previous.size() + next.size() * 2);
  structureChanged.reserve(1 + previous.size() + next.size() * 2);
  childrenChanged.reserve(childrenChanged.size() + next.size());
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
      structureChanged.emplace_back(previousParent);
      childrenChanged.push_back({std::move(previousParent), child->id().kind});
    }
    affected.emplace_back(child);
    if (parentChanged)
      structureChanged.emplace_back(child);
  }

  parent->children_.reserve(next.size());
  prepareChanges(affected, structureChanged, childrenChanged);
  for (Node *oldChildPointer : previous)
    if (!seen.contains(oldChildPointer)) {
      oldChildPointer->parent_ = nullptr;
      oldChildPointer->parentIndex_.store(0, std::memory_order_relaxed);
    }
  for (const NodeRef &child : next) {
    if (child->parent_ && child->parent_ != parent.get()) {
      eraseValue(child->parent_->children_, child.get());
      for (std::size_t index = 0; index < child->parent_->children_.size();
           ++index)
        child->parent_->children_[index]->parentIndex_.store(
            index, std::memory_order_relaxed);
    }
    child->parent_ = parent.get();
  }
  parent->children_.clear();
  for (std::size_t index = 0; index < next.size(); ++index) {
    const NodeRef &child = next[index];
    child->parentIndex_.store(index, std::memory_order_relaxed);
    parent->children_.emplace_back(child.get());
  }
}

void NodeGraph::WriteAccess::relate(const NodeRef &source, RelationKind kind,
                                    const NodeRef &target) {
  requireLive(source);
  requireLive(target);
  requireRelationTargetKind(kind, target);
  if (kind == RelationKind::PendingInteraction &&
      target->state_->status != NodeStatus::Pending &&
      target->state_->status != NodeStatus::Failed)
    throw std::invalid_argument(
        "PendingInteraction targets must be pending or failed");
  const auto current = source->relations_.find(kind);
  if (current != source->relations_.end() &&
      contains(current->second, target.get()))
    return;
  if (kind == RelationKind::PendingInteraction &&
      current != source->relations_.end() && !current->second.empty() &&
      current->second.back()->insertionOrder_ > target->insertionOrder_)
    throw std::invalid_argument(
        "PendingInteraction relations require incarnation order");
  auto nextRelations = source->relations_;
  nextRelations[kind].emplace_back(target.get());
  ensureAppendCapacity(target->incomingRelations_, 1);
  const std::array affected{source, target};
  const std::array structureChanged{source};
  noteInspectorRelationChange(source.get(), kind);
  prepareChanges(affected, structureChanged);
  source->relations_.swap(nextRelations);
  target->incomingRelations_.emplace_back(source.get(), kind);
  pendingInteractionOrderChanged_ |=
      kind == RelationKind::PendingInteraction &&
      source->id_.kind == NodeKind::Runtime;
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
  noteInspectorRelationChange(source.get(), kind);
  prepareChanges(affected, structureChanged);
  source->relations_.swap(nextRelations);
  eraseValue(target->incomingRelations_, std::pair{source.get(), kind});
  pendingInteractionOrderChanged_ |=
      kind == RelationKind::PendingInteraction &&
      source->id_.kind == NodeKind::Runtime;
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
    requireRelationTargetKind(kind, target);
    if (kind == RelationKind::PendingInteraction &&
        target->state_->status != NodeStatus::Pending &&
        target->state_->status != NodeStatus::Failed)
      throw std::invalid_argument(
          "PendingInteraction targets must be pending or failed");
    if (seen.insert(target.get()).second)
      next.emplace_back(target);
  }
  if (kind == RelationKind::PendingInteraction &&
      !std::ranges::is_sorted(next, {},
                              [](const NodeRef &node) {
                                return node->insertionOrder_;
                              }))
    throw std::invalid_argument(
        "PendingInteraction relations require incarnation order");

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
  for (const NodeRef &target : next)
    if (!contains(previous, target.get()))
      ensureAppendCapacity(target->incomingRelations_, 1);
  const std::array structureChanged{source};
  noteInspectorRelationChange(source.get(), kind);
  prepareChanges(affected, structureChanged);
  source->relations_.swap(nextRelations);
  for (Node *target : previous)
    if (!seen.contains(target))
      eraseValue(target->incomingRelations_, std::pair{source.get(), kind});
  for (const NodeRef &target : next)
    if (!contains(previous, target.get()))
      target->incomingRelations_.emplace_back(source.get(), kind);
  pendingInteractionOrderChanged_ |=
      kind == RelationKind::PendingInteraction &&
      source->id_.kind == NodeKind::Runtime;
}

void NodeGraph::WriteAccess::remove(const NodeRef &node) {
  const std::array nodes{node};
  removeMany(nodes);
}

void NodeGraph::WriteAccess::removeMany(std::span<const NodeRef> nodes) {
  if (nodes.empty())
    return;

  std::vector<NodeRef> removalOrder;
  removalOrder.reserve(nodes.size());
  std::unordered_set<Node *> removalSet;
  removalSet.reserve(nodes.size());
  for (const NodeRef &node : nodes) {
    requireLive(node);
    if (removalSet.insert(node.get()).second)
      removalOrder.emplace_back(node);
  }
  for (std::size_t index = 0; index < removalOrder.size(); ++index) {
    const auto pending =
        removalOrder[index]->relations_.find(RelationKind::PendingOperation);
    if (pending == removalOrder[index]->relations_.end())
      continue;
    for (Node *operation : pending->second)
      if (removalSet.insert(operation).second)
        removalOrder.emplace_back(pin(operation));
  }

  for (const NodeRef &node : removalOrder) {
    noteInspectorChildChange(node->parent_, node.get(), false);
    for (const auto &[source, kind] : node->incomingRelations_)
      noteInspectorRelationChange(source, kind);
  }

  using ParentChange = std::pair<Node *, std::size_t>;
  std::vector<ParentChange> changedParents;
  std::vector<std::pair<Node *, RelationKind>> changedRelations;
  std::vector<Node *> changedIncomingTargets;
  std::vector<NodeRef> topologyChanged;
  std::vector<NodeRef> structureChanged;
  std::vector<ChildListChange> changedChildLists;
  bool pendingInteractionOrderChanged = false;
  const auto noteTopology = [&](Node *node) {
    if (node && !removalSet.contains(node))
      topologyChanged.emplace_back(pin(node));
  };
  for (const NodeRef &node : removalOrder) {
    if (node->id_.kind == NodeKind::Runtime) {
      const auto pending =
          node->relations_.find(RelationKind::PendingInteraction);
      pendingInteractionOrderChanged |=
          pending != node->relations_.end() && !pending->second.empty();
    }
    if (node->parent_) {
      noteTopology(node->parent_);
      changedChildLists.push_back({pin(node->parent_), node->id().kind});
      if (!removalSet.contains(node->parent_))
        changedParents.emplace_back(
            node->parent_, node->parentIndex_.load(std::memory_order_relaxed));
    }
    std::array<bool, NodeGraph::NodeKindCount> childKinds{};
    for (Node *child : node->children_) {
      noteTopology(child);
      childKinds[static_cast<std::size_t>(child->id().kind)] = true;
    }
    for (std::size_t kind = 0; kind < childKinds.size(); ++kind)
      if (childKinds[kind])
        changedChildLists.push_back({node, static_cast<NodeKind>(kind)});
    for (const auto &edge : node->incomingRelations_) {
      noteTopology(edge.first);
      pendingInteractionOrderChanged |=
          edge.second == RelationKind::PendingInteraction && edge.first &&
          edge.first->id_.kind == NodeKind::Runtime;
      if (!removalSet.contains(edge.first))
        changedRelations.emplace_back(edge);
    }
    for (const auto &[kind, targets] : node->relations_)
      for (Node *target : targets)
        if (!removalSet.contains(target))
          changedIncomingTargets.emplace_back(target);
    if (node->parent_ || !node->children_.empty() || !node->relations_.empty())
      structureChanged.emplace_back(node);
  }

  sortUnique(
      changedParents,
      [](const ParentChange &change) {
        return std::pair{change.first->insertionOrder_, change.second};
      },
      [](const ParentChange &left, const ParentChange &right) {
        return left.first == right.first;
      });
  std::ranges::sort(changedRelations, {}, [](const auto &change) {
    return std::pair{change.first->insertionOrder_, change.second};
  });
  sortUnique(changedIncomingTargets,
             [](Node *node) { return node->insertionOrder_; });
  sortUnique(topologyChanged,
             [](const NodeRef &node) { return node->insertionOrder_; });
  sortUnique(changedChildLists, [](const ChildListChange &change) {
    return std::pair{change.owner->insertionOrder_, change.childKind};
  });
  structureChanged.insert(structureChanged.end(), topologyChanged.begin(),
                          topologyChanged.end());

  using CanonicalPosition = decltype(graph_->nodes_)::iterator;
  std::vector<CanonicalPosition> canonicalPositions;
  canonicalPositions.reserve(removalOrder.size());
  for (const NodeRef &node : removalOrder)
    canonicalPositions.emplace_back(graph_->nodes_.find(node->id_));
  std::uint64_t firstRemovedOrder = removalOrder.front()->insertionOrder_;
  for (const NodeRef &node : removalOrder)
    firstRemovedOrder = std::min(firstRemovedOrder, node->insertionOrder_);
  const auto firstRemoved = std::ranges::lower_bound(
      graph_->orderedNodes_, firstRemovedOrder, {},
      [](const NodeRef &node) { return node->insertionOrder_; });

  ensureAppendCapacity(graph_->retiredNodes_, removalOrder.size());
  ensureAppendCapacity(removed_, removalOrder.size());

  // All fallible validation and allocation precedes topology mutation. The
  // remaining standard erasures and scalar/shared-pointer assignments do not
  // allocate and retain direct ownership of every erased node.
  prepareChanges(topologyChanged, structureChanged, changedChildLists);
  pendingInteractionOrderChanged_ |= pendingInteractionOrderChanged;
  for (Node *target : changedIncomingTargets)
    std::erase_if(target->incomingRelations_, [&removalSet](const auto &edge) {
      return removalSet.contains(edge.first);
    });
  for (const ParentChange &change : changedParents) {
    auto &children = change.first->children_;
    const auto suffix =
        children.begin() + static_cast<std::ptrdiff_t>(change.second);
    children.erase(std::remove_if(suffix, children.end(),
                                  [&removalSet](Node *child) {
                                    return removalSet.contains(child);
                                  }),
                   children.end());
    for (std::size_t index = change.second; index < children.size(); ++index)
      children[index]->parentIndex_.store(index, std::memory_order_relaxed);
  }
  for (const NodeRef &node : removalOrder)
    for (Node *child : node->children_)
      if (!removalSet.contains(child)) {
        child->parent_ = nullptr;
        child->parentIndex_.store(0, std::memory_order_relaxed);
      }
  for (auto change = changedRelations.cbegin();
       change != changedRelations.cend();) {
    auto next = change + 1;
    while (next != changedRelations.cend() && *next == *change)
      ++next;
    const auto [source, kind] = *change;
    auto relation = source->relations_.find(kind);
    auto suffix = relation->second.end();
    std::size_t remaining = static_cast<std::size_t>(next - change);
    while (remaining != 0)
      if (removalSet.contains(*--suffix))
        --remaining;
    relation->second.erase(std::remove_if(suffix, relation->second.end(),
                                          [&removalSet](Node *target) {
                                            return removalSet.contains(target);
                                          }),
                           relation->second.end());
    if (relation->second.empty())
      source->relations_.erase(relation);
    change = next;
  }
  for (std::size_t index = 0; index < removalOrder.size(); ++index) {
    const NodeRef &node = removalOrder[index];
    node->parent_ = nullptr;
    node->parentIndex_.store(graph_->retiredNodes_.size(),
                             std::memory_order_relaxed);
    node->children_.clear();
    node->relations_.clear();
    node->incomingRelations_.clear();
    graph_->nodes_.erase(canonicalPositions[index]);
    graph_->retiredNodes_.emplace_back(node);
    removed_.emplace_back(node);
  }
  graph_->orderedNodes_.erase(
      std::remove_if(firstRemoved, graph_->orderedNodes_.end(),
                     [&removalSet](const NodeRef &node) {
                       return removalSet.contains(node.get());
                     }),
      graph_->orderedNodes_.end());
  std::array<bool, NodeGraph::NodeKindCount> removedKinds{};
  for (const NodeRef &node : removalOrder)
    removedKinds[static_cast<std::size_t>(node->id_.kind)] = true;
  for (std::size_t kind = 0; kind < removedKinds.size(); ++kind) {
    if (!removedKinds[kind])
      continue;
    auto &nodes = graph_->orderedNodesByKind_[kind];
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&removalSet](const NodeRef &node) {
                                 return removalSet.contains(node.get());
                               }),
                nodes.end());
  }
}

void NodeGraph::WriteAccess::releaseRetired(std::span<const NodeRef> nodes) {
  if (!graph_ || !lock_.owns_lock() || finished_)
    throw std::logic_error("graph write access is not active");
  for (const NodeRef &node : nodes) {
    if (!node)
      continue;
    const std::size_t index =
        node->parentIndex_.load(std::memory_order_relaxed);
    if (index >= graph_->retiredNodes_.size() ||
        graph_->retiredNodes_[index] != node)
      continue;
    const std::size_t last = graph_->retiredNodes_.size() - 1;
    if (index != last) {
      graph_->retiredNodes_[index] = std::move(graph_->retiredNodes_.back());
      graph_->retiredNodes_[index]->parentIndex_.store(
          index, std::memory_order_relaxed);
    }
    graph_->retiredNodes_.pop_back();
    node->parentIndex_.store(0, std::memory_order_relaxed);
    ++graph_->retiredOrderGeneration_;
  }
}

void NodeGraph::WriteAccess::refreshInspectorAgentContribution(
    InspectorAgentSelector &selector, std::size_t contributor) const {
  const std::size_t firstToken = contributor * 2;
  for (auto &candidates : selector.candidates) {
    candidates.erase(firstToken);
    candidates.erase(firstToken + 1);
  }
  const NodeRef &source = selector.contributors[contributor];
  const NodeState &state = *source->state_;
  const std::string type =
      scalarTextFromValue(valueMember(state, "type"));
  const bool carriesFields = type == "subAgentActivity" ||
                             agentActivityCanCreate(state);
  if (carriesFields) {
    for (std::size_t field = 0; field < InspectorAgentDetailFields.size();
         ++field)
      if (scalarTextNonEmpty(
              valueMember(state, InspectorAgentDetailFields[field])))
        selector.candidates[field].insert(firstToken);
    const Value *receivers = valueMember(state, "receiverThreadIds");
    if (const auto *array = receivers ? receivers->asArray() : nullptr;
        array && std::ranges::any_of(*array, [](const Value &value) {
          return !exactText(&value).empty();
        }))
      selector.candidates[6].insert(firstToken);
    if (scalarTextNonEmpty(valueMember(state, "resultText")))
      selector.candidates[7].insert(firstToken);
  }
  const Value::Object *child = reportedAgentState(state, selector.childId);
  const NodeStatusView childStatus =
      child ? nodeStatusView(valueMember(*child, "status")) : NodeStatusView{};
  NodeStatusView activity =
      carriesFields
          ? inspectorAgentActivityStatus(
                state, source->parent_->state_.get(), childStatus,
                selector.childIdVisible)
          : NodeStatusView{};
  const auto addStatus = [&](NodeStatusView value, std::size_t token) {
    if (value.empty())
      return;
    selector.candidates[8].insert(token);
    if (value.semantic != NodeStatus::Running)
      selector.candidates[9].insert(token);
  };
  addStatus(activity, firstToken);
  addStatus(childStatus, firstToken + 1);
  if (child && scalarTextNonEmpty(valueMember(*child, "message")))
    selector.candidates[7].insert(firstToken + 1);
}

void NodeGraph::WriteAccess::refreshInspectorAgentDependencies(
    InspectorThreadIndex &index, Node *dependency) const {
  for (const InspectorAgentDependency &dependency :
       index.agentDependencies.at(dependency))
    refreshInspectorAgentContribution(index.agents[dependency.row],
                                      dependency.contributor);
}

bool NodeGraph::WriteAccess::refreshInspectorAgentMessage(
    InspectorThreadIndex &index, Node *source) const {
  const NodeRef previous = index.latestAgentMessage();
  const std::size_t order = index.agentMessageOrder.at(source);
  const bool present =
      scalarTextFromValue(valueMember(*source->state_, "type")) ==
          "agentMessage" &&
      scalarTextNonEmpty(valueMember(*source->state_, "text"));
  if (present)
    index.agentMessages.insert_or_assign(order, pin(source));
  else
    index.agentMessages.erase(order);
  return previous != index.latestAgentMessage() || previous.get() == source;
}

bool NodeGraph::WriteAccess::appendInspectorAgentItem(
    InspectorThreadIndex &index, Node *item) const {
  Node *turn = item ? item->parent_ : nullptr;
  if (!turn || turn->id_.kind != NodeKind::Turn ||
      item->id_.kind != NodeKind::Item)
    return false;
  const NodeState &state = *item->state_;
  const std::string type = scalarTextFromValue(valueMember(state, "type"));
  if (type == "agentMessage") {
    const std::size_t order = index.agentMessageOrder.size();
    index.agentMessageOrder.emplace(item, order);
    if (scalarTextNonEmpty(valueMember(state, "text"))) {
      index.agentMessages.emplace(order, pin(item));
    }
    return false;
  }
  if (type != "subAgentActivity" && type != "collabAgentToolCall")
    return false;

  const bool canCreate = agentActivityCanCreate(state);
  const NodeRef source = pin(item);
  bool changed = false;
  bool identified = false;
  const std::string sourceKey =
      "source\n" + std::to_string(item->insertionOrder_) + "\n";
  const auto addChild = [&](std::string key, std::string id,
                            Node *exact = nullptr, bool canonical = true) {
    if (id.empty())
      return;
    identified = true;
    auto found = index.agentPositions.find(key);
    if (found == index.agentPositions.end()) {
      if (!canCreate)
        return;
      const std::size_t row = index.agents.size();
      found = index.agentPositions.emplace(key, row).first;
      index.agents.push_back(
          {std::move(key), std::move(id), pin(exact), canonical});
    }
    InspectorAgentSelector &selector = index.agents[found->second];
    if (exact)
      selector.exactChild = pin(exact);
    if (!selector.contributors.empty() &&
        selector.contributors.back() == source)
      return;
    const std::size_t contributor = selector.contributors.size();
    selector.contributors.push_back(source);
    const InspectorAgentDependency dependency{found->second, contributor};
    index.agentDependencies[item].push_back(dependency);
    index.agentDependencies[turn].push_back(dependency);
    refreshInspectorAgentContribution(selector, contributor);
    changed = true;
  };
  const auto related = item->relations_.find(RelationKind::AgentChildThread);
  bool exact = false;
  if (related != item->relations_.end())
    for (Node *child : related->second)
      if (child && child->id_.kind == NodeKind::Thread) {
        exact = true;
        const std::string identity =
            "thread:" + std::to_string(child->insertionOrder_);
        addChild("child\n" + identity, child->id_.canonical, child);
      }
  if (!exact && canCreate) {
    const Value *child = valueMember(state, "agentThreadId");
    if (const std::string *childId = child ? child->asString() : nullptr)
      addChild(sourceKey + *childId, *childId);
    if (const Value *receivers = valueMember(state, "receiverThreadIds"))
      if (const auto *array = receivers->asArray())
        for (const Value &receiver : *array)
          if (const std::string *id = receiver.asString())
            addChild(sourceKey + *id, *id);
    if (const Value *reported = valueMember(state, "agentsStates"))
      if (const auto *object = reported->asObject())
        for (const auto &[id, value] : *object) {
          static_cast<void>(value);
          addChild(sourceKey + id, id);
        }
  }
  if (!identified && canCreate) {
    std::string id = scalarTextFromValue(valueMember(state, "protocolId"));
    if (id.empty())
      id = item->id_.canonical;
    addChild(sourceKey + id, std::move(id), nullptr, false);
  }
  return changed;
}

InspectorThreadIndex
NodeGraph::WriteAccess::buildInspectorIndex(Node *thread,
                                            std::uint8_t sections) const {
  InspectorThreadIndex result;
  if (!thread || thread->id_.kind != NodeKind::Thread)
    return result;
  constexpr std::uint8_t Plan = 1;
  constexpr std::uint8_t Agents = 2;

  if (sections & Plan)
    for (auto turnPosition = thread->children_.rbegin();
         turnPosition != thread->children_.rend() && !result.planSource;
         ++turnPosition) {
    Node *turn = *turnPosition;
    if (!turn || turn->id_.kind != NodeKind::Turn)
      continue;
    if (const InspectorPlanView plan = inspectorPlanView(*turn->state_)) {
      result.planSource = pin(turn);
      result.planHasExplanation = plan.explanation;
      result.planRowCount = plan.rowCount;
      break;
    }
    for (auto itemPosition = turn->children_.rbegin();
         itemPosition != turn->children_.rend(); ++itemPosition) {
      Node *item = *itemPosition;
      if (!item || item->id_.kind != NodeKind::Item ||
          scalarTextFromValue(valueMember(*item->state_, "type")) != "plan")
        continue;
      result.planSource = pin(item);
      if (scalarTextNonEmpty(valueMember(*item->state_, "text")))
        result.planRowCount = 1;
      break;
    }
    }

  if (sections & Agents)
    for (Node *turn : thread->children_) {
      if (!turn || turn->id_.kind != NodeKind::Turn)
        continue;
      for (Node *item : turn->children_)
        static_cast<void>(appendInspectorAgentItem(result, item));
    }
  return result;
}

void NodeGraph::WriteAccess::refreshInspectorIndexes() {
  constexpr std::uint8_t Plan = 1;
  constexpr std::uint8_t Agents = 2;
  const std::uint64_t revision = graph_->revision_ + 1;
  const auto live = [this](Node *node) {
    if (!node)
      return false;
    const auto found = graph_->nodes_.find(node->id_);
    return found != graph_->nodes_.end() && found->second.get() == node;
  };
  for (const auto &[node, pending] : pendingStateRevisions_) {
    static_cast<void>(pending);
    if (live(node) && node->id_.kind == NodeKind::Thread &&
        graph_->inspectorIndexes_.try_emplace(node).second)
      markInspectorRebuild(node, Plan | Agents);
  }
  const auto markChanged = [&](Node *thread, std::uint8_t sections) {
    if (!live(thread))
      return;
    InspectorThreadIndex &index = graph_->inspectorIndexes_.at(thread);
    if (sections & Plan)
      index.planChangedRevision = revision;
    if (sections & Agents)
      index.agentChangedRevision = revision;
  };
  const auto rebuilding = [&](Node *thread, std::uint8_t section) {
    const auto found = inspectorImpacts_.find(thread);
    return found != inspectorImpacts_.end() &&
           (found->second.rebuild & section);
  };
  const auto planTurn = [](const InspectorThreadIndex &index) {
    Node *source = index.planSource.get();
    return source && source->id_.kind == NodeKind::Item ? source->parent_
                                                        : source;
  };
  const auto markAgentParents = [&](Node *childThread) {
    if (!childThread || childThread->id_.kind != NodeKind::Thread)
      return;
    for (const auto &[source, kind] : childThread->incomingRelations_) {
      Node *thread = inspectorThread(source);
      if (kind != RelationKind::AgentChildThread || !live(thread))
        continue;
      InspectorThreadIndex &index = graph_->inspectorIndexes_.at(thread);
      if (index.agentChangedRevision == revision)
        continue;
      const auto dependencies = index.agentDependencies.find(source);
      if (dependencies != index.agentDependencies.end() &&
          std::ranges::any_of(
              dependencies->second,
              [&](const InspectorAgentDependency &dependency) {
                return index.agents[dependency.row].exactChild.get() ==
                       childThread;
              }))
        index.agentChangedRevision = revision;
    }
  };
  for (const auto &[node, pending] : pendingStateRevisions_) {
    if (!live(node))
      continue;
    const bool statusChanged =
        pending.status || pending.fields.contains("status");
    if (node->id_.kind == NodeKind::Thread) {
      const NodeRef &source = graph_->inspectorIndexes_.at(node).planSource;
      if (pending.status && source && source->id_.kind == NodeKind::Turn)
        markChanged(node, Plan);
      if (statusChanged)
        markAgentParents(node);
      continue;
    }

    Node *thread = inspectorThread(node);
    if (!live(thread))
      continue;
    InspectorThreadIndex &index = graph_->inspectorIndexes_.at(thread);
    if (node->id_.kind == NodeKind::Turn) {
      if (pending.fields.contains("plan") ||
          pending.fields.contains("planExplanation")) {
        const InspectorPlanView shape = inspectorPlanView(*node->state_);
        const bool selected = index.planSource.get() == node;
        if (selected)
          markChanged(thread, Plan);
        const bool newer =
            !planTurn(index) ||
            node->parentIndex_.load(std::memory_order_relaxed) >=
                planTurn(index)->parentIndex_.load(std::memory_order_relaxed);
        if (!rebuilding(thread, Plan) && selected && shape) {
          if (index.planRowCount != shape.rowCount ||
              index.planHasExplanation != bool(shape.explanation))
            index.planOrderRevision = revision;
          index.planRowCount = shape.rowCount;
          index.planHasExplanation = shape.explanation;
        } else if ((selected && !shape) || (!selected && shape && newer)) {
          markInspectorRebuild(thread, Plan);
          markChanged(thread, Plan);
        }
      }
      if (pending.status && index.planSource.get() == node)
        markChanged(thread, Plan);
      if (pending.status && index.agentDependencies.contains(node)) {
        if (!rebuilding(thread, Agents))
          refreshInspectorAgentDependencies(index, node);
        markChanged(thread, Agents);
      }
      continue;
    }
    if (node->id_.kind != NodeKind::Item)
      continue;

    const std::string type =
        scalarTextFromValue(valueMember(*node->state_, "type"));
    const bool created = node->changedRevision_ == 0;
    if (pending.fields.contains("type")) {
      if (created) {
        std::uint8_t missing = inspectorItemSections(node);
        const auto impact = inspectorImpacts_.find(thread);
        if (impact != inspectorImpacts_.end()) {
          missing &= static_cast<std::uint8_t>(~impact->second.rebuild);
          if ((missing & Agents) && impact->second.append &&
              impact->second.append->turn == node->parent_ &&
              node->parentIndex_.load(std::memory_order_relaxed) >=
                  impact->second.append->first)
            missing &= static_cast<std::uint8_t>(~Agents);
        }
        markInspectorRebuild(thread, missing);
        markChanged(thread, missing);
        continue;
      } else {
        const std::uint8_t sections =
            inspectorItemSections(node) |
            inspectorIndexedSections(thread, node);
        markInspectorRebuild(thread, sections);
      }
    }
    if (created)
      continue;

    if (type == "plan" && pending.fields.contains("text") &&
        index.planSource.get() == node) {
      markChanged(thread, Plan);
      if (!rebuilding(thread, Plan)) {
        const std::size_t rows =
            scalarTextNonEmpty(valueMember(*node->state_, "text")) ? 1 : 0;
        if (index.planRowCount != rows)
          index.planOrderRevision = revision;
        index.planRowCount = rows;
      }
    }

    if (type == "agentMessage" && pending.fields.contains("text") &&
        !rebuilding(thread, Agents) &&
        refreshInspectorAgentMessage(index, node))
      markAgentParents(thread);

    if (type != "subAgentActivity" && type != "collabAgentToolCall")
      continue;
    const auto exact =
        node->relations_.find(RelationKind::AgentChildThread);
    const bool hasExact =
        exact != node->relations_.end() &&
        std::ranges::any_of(exact->second, [](Node *child) {
          return child && child->id_.kind == NodeKind::Thread;
        });
    const bool identity =
        (type == "subAgentActivity" && pending.fields.contains("kind")) ||
        (type == "collabAgentToolCall" && pending.fields.contains("tool")) ||
        (!hasExact &&
         (pending.fields.contains("agentThreadId") ||
          pending.fields.contains("receiverThreadIds") ||
          pending.fields.contains("agentsStates") ||
          pending.fields.contains("protocolId")));
    const bool carriesFields =
        type == "subAgentActivity" || agentActivityCanCreate(*node->state_);
    if (identity) {
      markInspectorRebuild(thread, Agents);
      markChanged(thread, Agents);
    } else if ((pending.fields.contains("agentsStates") ||
                (carriesFields &&
                 (statusChanged || pending.fields.contains("kind") ||
                  pending.fields.contains("receiverThreadIds") ||
                  pending.fields.contains("resultText") ||
                  std::ranges::any_of(pending.fields,
                                      [](const std::string &field) {
                                        return std::ranges::find(
                                                   InspectorAgentDetailFields,
                                                   field) !=
                                               InspectorAgentDetailFields.end();
                                      })))) &&
               index.agentDependencies.contains(node)) {
      if (!rebuilding(thread, Agents))
        refreshInspectorAgentDependencies(index, node);
      markChanged(thread, Agents);
    }
  }

  for (auto &[thread, impact] : inspectorImpacts_) {
    if (!impact.append || !live(thread) || (impact.rebuild & Agents))
      continue;
    Node *turn = impact.append->turn;
    if (!turn || inspectorThread(turn) != thread ||
        impact.append->first > turn->children_.size()) {
      markInspectorRebuild(thread, Agents);
      continue;
    }
    InspectorThreadIndex &index = graph_->inspectorIndexes_.at(thread);
    const std::size_t previousRows = index.agents.size();
    const NodeRef previousMessage = index.latestAgentMessage();
    bool projectionChanged = false;
    for (std::size_t position = impact.append->first;
         position < turn->children_.size(); ++position)
      projectionChanged |=
          appendInspectorAgentItem(index, turn->children_[position]);
    if (projectionChanged)
      markChanged(thread, Agents);
    if (index.agents.size() != previousRows)
      index.agentOrderRevision = revision;
    if (index.latestAgentMessage() != previousMessage)
      markAgentParents(thread);
  }

  for (auto &[thread, impact] : inspectorImpacts_) {
    if (!live(thread) || impact.rebuild == 0)
      continue;
    const std::uint8_t sections = impact.rebuild;
    InspectorThreadIndex &target = graph_->inspectorIndexes_.at(thread);
    InspectorThreadIndex next = buildInspectorIndex(thread, sections);
    const bool planChanged =
        target.planSource != next.planSource ||
        target.planRowCount != next.planRowCount ||
        target.planHasExplanation != next.planHasExplanation;
    if ((sections & Plan) && planChanged) {
      target.planOrderRevision = revision;
      markChanged(thread, Plan);
    }
    if ((sections & Agents) &&
        target.agentPositions != next.agentPositions)
      target.agentOrderRevision = revision;
    if ((sections & Agents) && target.agents != next.agents)
      markChanged(thread, Agents);
    const NodeRef latestMessage = next.latestAgentMessage();
    const auto latestChange = latestMessage
                                  ? pendingStateRevisions_.find(
                                        latestMessage.get())
                                  : pendingStateRevisions_.end();
    const bool latestMessageChanged =
        (sections & Agents) &&
        (target.latestAgentMessage() != latestMessage ||
         (latestChange != pendingStateRevisions_.end() &&
          (latestChange->second.fields.contains("type") ||
           latestChange->second.fields.contains("text"))));
    if (latestMessageChanged)
      markAgentParents(thread);
    if (sections & Plan) {
      target.planSource = std::move(next.planSource);
      target.planRowCount = next.planRowCount;
      target.planHasExplanation = next.planHasExplanation;
    }
    if (sections & Agents) {
      target.agents = std::move(next.agents);
      target.agentPositions = std::move(next.agentPositions);
      target.agentDependencies = std::move(next.agentDependencies);
      target.agentMessageOrder = std::move(next.agentMessageOrder);
      target.agentMessages = std::move(next.agentMessages);
    }
  }

  for (const NodeRef &node : removed_)
    if (node && node->id_.kind == NodeKind::Thread)
      graph_->inspectorIndexes_.erase(node.get());
}

GraphChange NodeGraph::WriteAccess::finish() noexcept {
  if (finished_)
    return GraphChange{graph_ ? graph_->revision_ : 0, {}, {}};
  if (dirty_)
    refreshInspectorIndexes();
  GraphChange change = publish();
  if (lock_.owns_lock())
    lock_.unlock();
  return change;
}

void NodeGraph::WriteAccess::requireLive(const NodeRef &node) const {
  if (!graph_ || !lock_.owns_lock() || finished_)
    throw std::logic_error("graph write access is not active");
  if (!node)
    throw std::invalid_argument("node is not live");
  const auto found = graph_->nodes_.find(node->id_);
  if (found == graph_->nodes_.end() || found->second != node)
    throw std::invalid_argument("node does not belong to this graph");
}

void NodeGraph::WriteAccess::prepareChanges(
    std::span<const NodeRef> affected,
    std::span<const NodeRef> structureChanged,
    std::span<const ChildListChange> childrenChanged) {
  const bool wasDirty = dirty_;
  const std::size_t affectedSize = affected_.size();
  const std::size_t childListsChangedSize = childListsChanged_.size();
  std::vector<Node *> addedAffected;
  std::vector<Node *> addedStructure;
  std::vector<ChildListChange> addedChildren;
  addedAffected.reserve(affected.size());
  addedStructure.reserve(structureChanged.size());
  addedChildren.reserve(childrenChanged.size());
  ensureAppendCapacity(affected_, affected.size());
  ensureAppendCapacity(childListsChanged_, childrenChanged.size());
  affectedIndex_.reserve(affectedIndex_.size() + affected.size());
  pendingStructureRevisions_.reserve(pendingStructureRevisions_.size() +
                                     structureChanged.size());
  childListsChangedIndex_.reserve(childListsChangedIndex_.size() +
                                  childrenChanged.size());

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
    for (const ChildListChange &change : childrenChanged) {
      if (!change.owner)
        continue;
      const auto [position, inserted] =
          childListsChangedIndex_.insert(change);
      static_cast<void>(position);
      if (!inserted)
        continue;
      try {
        childListsChanged_.emplace_back(change);
      } catch (...) {
        childListsChangedIndex_.erase(change);
        throw;
      }
      addedChildren.emplace_back(change);
    }
  } catch (...) {
    affected_.resize(affectedSize);
    childListsChanged_.resize(childListsChangedSize);
    for (Node *node : addedAffected)
      affectedIndex_.erase(node);
    for (Node *node : addedStructure)
      pendingStructureRevisions_.erase(node);
    for (const ChildListChange &change : addedChildren)
      childListsChangedIndex_.erase(change);
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
  if (pendingInteractionOrderChanged_)
    graph_->pendingInteractionOrderRevision_ = graph_->revision_;
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
  GraphChange change{graph_->revision_, std::move(affected_),
                     std::move(removed_), std::move(childListsChanged_)};
  childListsChangedIndex_.clear();
  return change;
}

} // namespace codexui::nodegraph
