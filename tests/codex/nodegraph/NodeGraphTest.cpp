// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/NodeGraph.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using codexui::nodegraph::GraphChange;
using codexui::nodegraph::NodeGraph;
using codexui::nodegraph::NodeId;
using codexui::nodegraph::NodeKind;
using codexui::nodegraph::NodeRef;
using codexui::nodegraph::NodeState;
using codexui::nodegraph::NodeStatus;
using codexui::nodegraph::RelationKind;
using codexui::nodegraph::Value;

bool expect(bool condition, std::string_view message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

template <typename Exception, typename Operation>
bool throws(Operation &&operation) {
  try {
    operation();
  } catch (const Exception &) {
    return true;
  } catch (...) {
  }
  return false;
}

NodeId id(NodeKind kind, std::string canonical) {
  return NodeId{kind, std::move(canonical)};
}

std::size_t count(const std::vector<NodeRef> &nodes, const NodeRef &wanted) {
  return static_cast<std::size_t>(
      std::count(nodes.begin(), nodes.end(), wanted));
}

bool sameOrder(const std::vector<NodeRef> &actual,
               std::initializer_list<NodeRef> expected) {
  return actual.size() == expected.size() &&
         std::equal(actual.begin(), actual.end(), expected.begin());
}

bool testValue() {
  const Value nullValue;
  const Value explicitNull = nullptr;
  const Value boolean = true;
  const Value signedInteger = -7;
  const Value unsignedInteger = std::uint32_t{9};
  const Value real = 2.5;
  const Value string = "node";
  const Value array =
      Value::Array{nullptr, false, -3, std::uint64_t{4}, 1.25, "tail"};
  const Value object = Value::Object{
      {"enabled", true},
      {"nested", Value::Object{{"name", "thread"}}},
      {"values", Value::Array{1, 2, 3}},
  };

  bool passed = true;
  passed &= expect(nullValue.isNull() && explicitNull == nullValue,
                   "Value represents null by default and explicitly");
  passed &= expect(boolean.isBool() && boolean.asBool() && *boolean.asBool() &&
                       !boolean.asString(),
                   "Value exposes bool only through its exact accessor");
  passed &=
      expect(signedInteger.isSigned() && signedInteger.asInt64() &&
                 *signedInteger.asInt64() == -7 && !signedInteger.asUInt64(),
             "Value preserves signed integers without coercion");
  passed &=
      expect(unsignedInteger.isUnsigned() && unsignedInteger.asUInt64() &&
                 *unsignedInteger.asUInt64() == 9 && !unsignedInteger.asInt64(),
             "Value preserves unsigned integers without coercion");
  passed &=
      expect(real.isDouble() && real.asDouble() && *real.asDouble() == 2.5,
             "Value preserves floating-point values");
  passed &= expect(string.isString() && string.asString() &&
                       *string.asString() == "node",
                   "Value owns string values");
  passed &= expect(array.isArray() && array.asArray() &&
                       array.asArray()->size() == 6 &&
                       array.asArray()->back().asString(),
                   "Value recursively owns ordered arrays");

  const Value *nested = object.find("nested");
  const Value *name = nested ? nested->find("name") : nullptr;
  passed &= expect(object.isObject() && name && name->asString() &&
                       *name->asString() == "thread" &&
                       object.find("missing") == nullptr &&
                       string.find("name") == nullptr,
                   "Value object lookup is typed, nested, and non-throwing");

  Value mutableObject = Value::Object{{"name", "before"}};
  Value *mutableName = mutableObject.find("name");
  if (mutableName)
    *mutableName = "after";
  passed &= expect(mutableName && mutableName->asString() &&
                       *mutableName->asString() == "after",
                   "Value mutable lookup addresses the owned member");

  const Value equalObject = Value::Object{
      {"enabled", true},
      {"nested", Value::Object{{"name", "thread"}}},
      {"values", Value::Array{1, 2, 3}},
  };
  passed &= expect(object == equalObject &&
                       object != Value(Value::Object{{"enabled", false}}) &&
                       Value(std::int64_t{1}) != Value(std::uint64_t{1}),
                   "Value equality is recursive and alternative-sensitive");
  return passed;
}

bool testInsertionLookupAndOrder() {
  NodeGraph graph;
  const NodeId runtimeId = id(NodeKind::Runtime, "runtime");
  const NodeId threadId = id(NodeKind::Thread, "shared-id");
  const NodeId turnId = id(NodeKind::Turn, "shared-id");

  NodeRef runtime;
  NodeRef thread;
  NodeRef turn;
  GraphChange inserted;
  {
    auto write = graph.write();
    runtime = write.upsert(runtimeId);
    thread = write.upsert(threadId,
                          NodeState{NodeStatus::Pending, {{"title", "One"}}});
    turn = write.upsert(turnId);
    inserted = write.finish();
  }

  bool passed = true;
  passed &= expect(inserted.revision == 1 && graph.publishedRevision() == 1 &&
                       sameOrder(inserted.affected, {runtime, thread, turn}) &&
                       inserted.removed.empty(),
                   "one insertion transaction publishes one ordered change");
  {
    auto read = graph.tryRead();
    passed &= expect(read.has_value(), "the graph is readable after publish");
    if (!read)
      return false;
    passed &= expect(
        read->revision() == 1 && read->insertionOrder(runtime) == 1 &&
            read->insertionOrder(thread) == 2 &&
            read->insertionOrder(turn) == 3 &&
            read->find(runtimeId) == runtime &&
            read->find(threadId) == thread && read->find(turnId) == turn &&
            !read->find(id(NodeKind::Item, "missing")),
        "canonical kind and id lookup returns stable NodeRefs");
    passed &= expect(sameOrder(read->orderedNodes(), {runtime, thread, turn}),
                     "graph iteration retains insertion order");
    const auto state = read->state(thread);
    if (!expect(state != nullptr, "a live node exposes immutable state"))
      return false;
    const auto title = state->fields.find("title");
    passed &=
        expect(state && state->status == NodeStatus::Pending &&
                   title != state->fields.end() && title->second.asString() &&
                   *title->second.asString() == "One",
               "inserted nodes retain their initial state");
  }

  {
    auto write = graph.write();
    passed &= expect(write.upsert(threadId) == thread,
                     "upsert preserves an existing node identity");
    const GraphChange unchanged = write.finish();
    passed &= expect(unchanged.revision == 1 && unchanged.empty() &&
                         graph.publishedRevision() == 1,
                     "existing-node upsert does not publish a revision");
  }
  return passed;
}

bool testAtomicStateRelationsAndNoOp() {
  NodeGraph graph;
  NodeRef parent;
  NodeRef firstChild;
  NodeRef secondChild;
  NodeRef firstTarget;
  NodeRef secondTarget;
  {
    auto write = graph.write();
    parent = write.upsert(id(NodeKind::Thread, "parent"));
    firstChild = write.upsert(id(NodeKind::Turn, "turn-1"));
    secondChild = write.upsert(id(NodeKind::Turn, "turn-2"));
    firstTarget = write.upsert(id(NodeKind::Operation, "operation-1"));
    secondTarget = write.upsert(id(NodeKind::Operation, "operation-2"));
    static_cast<void>(write.finish());
  }

  std::shared_ptr<const NodeState> pinnedInitial;
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "initial state can be pinned"))
      return false;
    pinnedInitial = read->state(parent);
  }

  GraphChange changed;
  {
    auto write = graph.write();
    write.setStatus(parent, NodeStatus::Running);
    write.setField(parent, "title", "Atomic update");
    write.setField(parent, "sequence", std::uint64_t{12});
    write.setField(parent, "stream", "first second");
    write.setParent(parent, firstChild);
    write.setParent(parent, secondChild);
    write.setParent(parent, firstChild);
    write.relate(parent, RelationKind::OperationTarget, firstTarget);
    write.relate(parent, RelationKind::OperationTarget, secondTarget);
    write.relate(parent, RelationKind::OperationTarget, firstTarget);
    changed = write.finish();
  }

  bool passed = true;
  passed &= expect(changed.revision == 2 && graph.publishedRevision() == 2,
                   "many state and relation changes publish one revision");
  passed &= expect(count(changed.affected, parent) == 1 &&
                       count(changed.affected, firstChild) == 1 &&
                       count(changed.affected, secondChild) == 1 &&
                       count(changed.affected, firstTarget) == 1 &&
                       count(changed.affected, secondTarget) == 1,
                   "a transaction reports each affected node once");
  passed &=
      expect(pinnedInitial && pinnedInitial->status == NodeStatus::Unknown &&
                 pinnedInitial->fields.empty(),
             "a pinned immutable state survives later replacement");

  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "the complete transaction is readable"))
      return false;
    const auto current = read->state(parent);
    if (!expect(current != nullptr, "a live node retains current state"))
      return false;
    const auto title = current->fields.find("title");
    const auto sequence = current->fields.find("sequence");
    const auto stream = current->fields.find("stream");
    passed &= expect(
        current != pinnedInitial && current->status == NodeStatus::Running &&
            title != current->fields.end() && title->second.asString() &&
            *title->second.asString() == "Atomic update" &&
            sequence != current->fields.end() && sequence->second.asUInt64() &&
            *sequence->second.asUInt64() == 12 &&
            stream != current->fields.end() && stream->second.asString() &&
            *stream->second.asString() == "first second",
        "all state mutations become visible together");
    passed &= expect(
        sameOrder(read->children(parent), {firstChild, secondChild}) &&
            read->childCount(parent) == 2 &&
            read->childAt(parent, 0) == firstChild &&
            read->childAt(parent, 1) == secondChild &&
            !read->childAt(parent, 2) && read->childCount({}) == 0 &&
            !read->childAt({}, 0) && read->parent(firstChild) == parent &&
            read->parent(secondChild) == parent,
        "parent and bounded child access preserve order and deduplicate");
    passed &= expect(
        sameOrder(read->related(parent, RelationKind::OperationTarget),
                  {firstTarget, secondTarget}) &&
            read->relatedCount(parent, RelationKind::OperationTarget) == 2 &&
            read->relatedAt(parent, RelationKind::OperationTarget, 0) ==
                firstTarget &&
            read->relatedAt(parent, RelationKind::OperationTarget, 1) ==
                secondTarget &&
            !read->relatedAt(parent, RelationKind::OperationTarget, 2) &&
            read->relatedCount(parent, RelationKind::ProcessOwner) == 0 &&
            !read->relatedAt(parent, RelationKind::ProcessOwner, 0) &&
            read->relatedCount({}, RelationKind::OperationTarget) == 0 &&
            !read->relatedAt({}, RelationKind::OperationTarget, 0),
        "bounded cross-node relation access preserves order and handles "
        "missing, out-of-range, and null sources");
    passed &= expect(read->changedRevision(parent) == 2 &&
                         read->changedRevision(firstChild) == 2 &&
                         read->changedRevision(firstTarget) == 2,
                     "affected nodes receive the transaction revision");
  }

  {
    auto write = graph.write();
    write.setStatus(parent, NodeStatus::Running);
    write.setField(parent, "title", "Atomic update");
    write.eraseField(parent, "missing");
    write.setField(parent, "stream", "first second");
    write.setParent(parent, firstChild);
    write.relate(parent, RelationKind::OperationTarget, firstTarget);
    write.unrelate(parent, RelationKind::ProcessOwner, secondTarget);
    const GraphChange unchanged = write.finish();
    passed &= expect(unchanged.revision == 2 && unchanged.empty() &&
                         graph.publishedRevision() == 2,
                     "semantic no-op writes do not increment graph revision");
  }

  return passed;
}

bool testStructuralChangeRevision() {
  NodeGraph graph;
  NodeRef source;
  NodeRef child;
  NodeRef target;
  {
    auto write = graph.write();
    source = write.upsert(id(NodeKind::Thread, "structure-source"));
    child = write.upsert(id(NodeKind::Thread, "structure-child"));
    target = write.upsert(id(NodeKind::Thread, "structure-target"));
    static_cast<void>(write.finish());
  }

  bool passed = true;
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "new nodes expose structural revisions"))
      return false;
    passed &=
        expect(read->structureChangedRevision(source) == 0 &&
                   read->structureChangedRevision(child) == 0 &&
                   read->structureChangedRevision(target) == 0 &&
                   read->structureChangedRevision({}) == 0 &&
                   read->structureRevision(NodeKind::Thread) == 0 &&
                   graph.publishedStructureRevision(NodeKind::Thread) == 0,
               "node insertion alone does not report a relation change");
  }

  {
    auto write = graph.write();
    write.setField(source, "stream", "field-only");
    const GraphChange stateOnly = write.finish();
    passed &= expect(stateOnly.revision == 2,
                     "the state-only control mutation publishes");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "state-only structure stamps are readable"))
      return false;
    passed &= expect(read->structureChangedRevision(source) == 0,
                     "ordinary fields do not advance structural revisions");
  }

  {
    auto write = graph.write();
    write.setParent(source, child);
    write.relate(source, RelationKind::StructuralChildThread, target);
    const GraphChange structured = write.finish();
    passed &= expect(structured.revision == 3,
                     "parent and relation changes publish together");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "changed structure stamps are readable"))
      return false;
    passed &= expect(
        read->structureChangedRevision(source) == 3 &&
            read->structureChangedRevision(child) == 3 &&
            read->structureChangedRevision(target) == 0,
        "parents, children, and outgoing relation owners receive the exact "
        "structural transaction revision");
    passed &= expect(
        read->structureRevision(NodeKind::Thread) == 3 &&
            graph.publishedStructureRevision(NodeKind::Thread) == 3,
        "the locked and published per-kind structure stamps advance together");
  }

  {
    auto write = graph.write();
    write.setStatus(source, NodeStatus::Running);
    const GraphChange stateOnly = write.finish();
    passed &= expect(stateOnly.revision == 4,
                     "a later status-only mutation publishes independently");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "stable structure stamps are readable"))
      return false;
    passed &=
        expect(read->structureChangedRevision(source) == 3 &&
                   read->structureChangedRevision(child) == 3 &&
                   read->structureRevision(NodeKind::Thread) == 3 &&
                   graph.publishedStructureRevision(NodeKind::Thread) == 3,
               "status churn leaves structural revisions stable");
  }

  {
    auto write = graph.write();
    write.clearParent(child);
    write.unrelate(source, RelationKind::StructuralChildThread, target);
    const GraphChange cleared = write.finish();
    passed &= expect(cleared.revision == 5,
                     "clearing structure publishes one revision");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "cleared structure stamps are readable"))
      return false;
    passed &= expect(read->structureChangedRevision(source) == 5 &&
                         read->structureChangedRevision(child) == 5 &&
                         read->structureChangedRevision(target) == 0,
                     "cleared parent and outgoing relations advance only "
                     "their structural owners");
  }

  {
    auto write = graph.write();
    write.relate(source, RelationKind::StructuralChildThread, target);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    write.remove(target);
    const GraphChange removed = write.finish();
    passed &= expect(removed.revision == 7,
                     "removing a relation target publishes once");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "removal structure stamps are readable"))
      return false;
    passed &= expect(
        read->structureChangedRevision(source) == 7 &&
            read->related(source, RelationKind::StructuralChildThread).empty(),
        "removal advances owners whose outgoing relations were unlinked");
  }

  NodeRef turn;
  NodeRef item;
  {
    auto write = graph.write();
    turn = write.upsert(id(NodeKind::Turn, "structure-turn"));
    item = write.upsert(id(NodeKind::Item, "structure-item"));
    write.setParent(turn, item);
    const GraphChange itemStructure = write.finish();
    passed &= expect(itemStructure.revision == 8,
                     "an unrelated item hierarchy publishes once");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "per-kind structure stamps are readable"))
      return false;
    passed &= expect(
        read->structureRevision(NodeKind::Thread) == 7 &&
            graph.publishedStructureRevision(NodeKind::Thread) == 7 &&
            read->structureRevision(NodeKind::Turn) == 8 &&
            read->structureRevision(NodeKind::Item) == 8,
        "item hierarchy changes do not advance the thread topology stamp");
  }
  return passed;
}

bool testAuthoritativeOrderingReplacement() {
  NodeGraph graph;
  NodeRef runtime;
  NodeRef firstRoot;
  NodeRef secondRoot;
  NodeRef thirdRoot;
  NodeRef thread;
  NodeRef firstTurn;
  NodeRef secondTurn;
  NodeRef thirdTurn;
  {
    auto write = graph.write();
    runtime = write.upsert(id(NodeKind::Runtime, "runtime"));
    firstRoot = write.upsert(id(NodeKind::Thread, "root-1"));
    secondRoot = write.upsert(id(NodeKind::Thread, "root-2"));
    thirdRoot = write.upsert(id(NodeKind::Thread, "root-3"));
    thread = write.upsert(id(NodeKind::Thread, "ordered-thread"));
    firstTurn = write.upsert(id(NodeKind::Turn, "turn-1"));
    secondTurn = write.upsert(id(NodeKind::Turn, "turn-2"));
    thirdTurn = write.upsert(id(NodeKind::Turn, "turn-3"));
    write.relate(runtime, RelationKind::RootThread, firstRoot);
    write.relate(runtime, RelationKind::RootThread, secondRoot);
    write.setParent(thread, firstTurn);
    write.setParent(thread, secondTurn);
    static_cast<void>(write.finish());
  }

  GraphChange changed;
  {
    auto write = graph.write();
    const std::array roots{thirdRoot, firstRoot, thirdRoot};
    write.replaceRelated(runtime, RelationKind::RootThread, roots);
    const std::array turns{secondTurn, thirdTurn, secondTurn};
    write.replaceChildren(thread, turns);
    changed = write.finish();
  }

  bool passed = expect(changed.revision == 2,
                       "ordered replacements publish in one transaction");
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "replacement ordering is readable"))
      return false;
    passed &= expect(
        sameOrder(read->related(runtime, RelationKind::RootThread),
                  {thirdRoot, firstRoot}) &&
            sameOrder(read->children(thread), {secondTurn, thirdTurn}),
        "provider order replaces stale order and deduplicates identities");
    passed &=
        expect(!read->parent(firstTurn) && read->parent(secondTurn) == thread &&
                   read->parent(thirdTurn) == thread,
               "omitted children are unlinked while retained children "
               "keep stable NodeRefs");
  }

  {
    auto write = graph.write();
    const std::array roots{thirdRoot, firstRoot};
    const std::array turns{secondTurn, thirdTurn};
    write.replaceRelated(runtime, RelationKind::RootThread, roots);
    write.replaceChildren(thread, turns);
    const GraphChange unchanged = write.finish();
    passed &= expect(unchanged.revision == 2 && unchanged.empty(),
                     "identical authoritative order is a semantic no-op");
  }
  return passed;
}

bool testRemovalLifetimeAndAttachment() {
  NodeGraph graph;
  NodeRef parent;
  NodeRef removedNode;
  NodeRef child;
  NodeRef relationSource;
  NodeRef relationTarget;
  {
    auto write = graph.write();
    parent = write.upsert(id(NodeKind::Thread, "owner"));
    removedNode = write.upsert(id(NodeKind::Turn, "removed-turn"));
    child = write.upsert(id(NodeKind::Item, "child-item"));
    relationSource = write.upsert(id(NodeKind::Operation, "source"));
    relationTarget = write.upsert(id(NodeKind::Process, "target"));
    write.setParent(parent, removedNode);
    write.setParent(removedNode, child);
    write.relate(relationSource, RelationKind::OperationTarget, removedNode);
    write.relate(removedNode, RelationKind::ProcessOwner, relationTarget);
    static_cast<void>(write.finish());
  }

  struct UiAttachment final {
    int renderedRevision = 17;
  } attachment;
  removedNode->setUiAttachment(&attachment);
  bool passed = expect(removedNode->uiAttachment() == &attachment,
                       "a node retains one opaque non-owning UI attachment");

  std::weak_ptr<codexui::nodegraph::Node> lifetime = removedNode;
  GraphChange removal;
  {
    auto write = graph.write();
    write.remove(removedNode);
    removal = write.finish();
  }
  passed &= expect(removal.revision == 2 && removal.removed.size() == 1 &&
                       removal.removed.front() == removedNode &&
                       removedNode->uiAttachment() == &attachment,
                   "removal queues a stable NodeRef for UI detachment");

  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "the graph is readable after removal"))
      return false;
    passed &=
        expect(!read->find(removedNode->id()) &&
                   sameOrder(read->orderedNodes(),
                             {parent, child, relationSource, relationTarget}) &&
                   read->removed(removedNode),
               "removal erases canonical lookup and graph ordering");
    passed &= expect(
        read->children(parent).empty() && !read->parent(child) &&
            read->related(relationSource, RelationKind::OperationTarget)
                .empty() &&
            read->related(removedNode, RelationKind::ProcessOwner).empty(),
        "removal unlinks hierarchy and cross-node relations");
    passed &= expect(
        read->retiredNodes().size() == 1 &&
            read->retiredNodes().front() == removedNode &&
            read->retiredCount() == 1 && read->retiredAt(0) == removedNode &&
            !read->retiredAt(1) && read->retiredOrderGeneration() == 0,
        "removed nodes support bounded retirement reads until "
        "Qt acknowledges");
  }

  {
    auto write = graph.write();
    passed &= expect(throws<std::invalid_argument>(
                         [&] { write.setField(removedNode, "invalid", true); }),
                     "removed nodes reject later graph mutation");
    const GraphChange unchanged = write.finish();
    passed &= expect(unchanged.revision == 2 && unchanged.empty(),
                     "rejected removed-node mutation leaves no revision");
  }

  NodeRef detaching = removal.removed.front();
  removal.removed.clear();
  removedNode.reset();
  passed &= expect(!lifetime.expired(),
                   "the retired graph reference pins the removed node");
  detaching->setUiAttachment(nullptr);
  passed &= expect(detaching->uiAttachment() == nullptr,
                   "Qt can clear the opaque attachment before release");
  {
    std::vector<NodeRef> acknowledged{detaching};
    auto write = graph.write();
    write.releaseRetired(acknowledged);
    const GraphChange released = write.finish();
    passed &= expect(released.revision == 2 && released.empty() &&
                         graph.publishedRevision() == 2,
                     "UI detachment acknowledgement is revision-neutral");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "the graph is readable after retirement"))
      return false;
    passed &=
        expect(read->retiredNodes().empty() && read->retiredCount() == 0 &&
                   read->retiredOrderGeneration() == 1,
               "releaseRetired drops graph lifetime ownership and "
               "invalidates an incremental retirement cursor");
  }
  detaching.reset();
  passed &=
      expect(lifetime.expired() && attachment.renderedRevision == 17,
             "node destruction neither owns nor deletes its UI attachment");
  return passed;
}

bool testMisuseRejection() {
  NodeGraph graph;
  NodeGraph otherGraph;
  NodeRef root;
  NodeRef child;
  NodeRef grandchild;
  NodeRef foreign;
  {
    auto write = graph.write();
    root = write.upsert(id(NodeKind::Thread, "same-canonical"));
    child = write.upsert(id(NodeKind::Turn, "child"));
    grandchild = write.upsert(id(NodeKind::Item, "grandchild"));
    static_cast<void>(write.finish());
  }
  {
    auto write = otherGraph.write();
    foreign = write.upsert(id(NodeKind::Thread, "same-canonical"));
    static_cast<void>(write.finish());
  }

  bool passed = true;
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "the graph is readable for misuse checks"))
      return false;
    passed &= expect(
        throws<std::invalid_argument>(
            [&] { static_cast<void>(read->state(foreign)); }) &&
            throws<std::invalid_argument>(
                [&] { static_cast<void>(read->changedRevision(foreign)); }) &&
            throws<std::invalid_argument>(
                [&] { static_cast<void>(read->removed(foreign)); }) &&
            throws<std::invalid_argument>(
                [&] { static_cast<void>(read->parent(foreign)); }) &&
            throws<std::invalid_argument>(
                [&] { static_cast<void>(read->children(foreign)); }) &&
            throws<std::invalid_argument>([&] {
              static_cast<void>(
                  read->related(foreign, RelationKind::ForkChildThread));
            }) &&
            throws<std::invalid_argument>([&] {
              static_cast<void>(
                  read->relatedCount(foreign, RelationKind::ForkChildThread));
            }) &&
            throws<std::invalid_argument>([&] {
              static_cast<void>(
                  read->relatedAt(foreign, RelationKind::ForkChildThread, 0));
            }),
        "foreign NodeRefs cannot be read under the wrong graph lock, "
        "including through bounded relation access");
  }
  {
    auto write = graph.write();
    passed &= expect(
        throws<std::invalid_argument>([&] { write.setParent(root, root); }),
        "a node cannot parent itself");
    passed &=
        expect(throws<std::invalid_argument>(
                   [&] { write.setField(foreign, "invalid", true); }) &&
                   throws<std::invalid_argument>([&] {
                     write.relate(root, RelationKind::ForkChildThread, foreign);
                   }),
               "foreign NodeRefs are rejected even when canonical IDs collide");
    const GraphChange unchanged = write.finish();
    passed &= expect(unchanged.revision == 1 && unchanged.empty(),
                     "rejected self and foreign operations do not publish");
  }

  {
    auto write = graph.write();
    write.setParent(root, child);
    write.setParent(child, grandchild);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    passed &= expect(throws<std::invalid_argument>(
                         [&] { write.setParent(grandchild, root); }),
                     "parent assignment rejects an ancestor cycle");
    const GraphChange unchanged = write.finish();
    passed &= expect(unchanged.revision == 2 && unchanged.empty(),
                     "rejected cycle leaves hierarchy and revision unchanged");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "the hierarchy remains readable"))
      return false;
    passed &= expect(!read->parent(root) && read->parent(child) == root &&
                         read->parent(grandchild) == child &&
                         sameOrder(read->children(root), {child}) &&
                         sameOrder(read->children(child), {grandchild}),
                     "misuse rejection preserves the acyclic hierarchy");
  }
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testValue();
  passed &= testInsertionLookupAndOrder();
  passed &= testAtomicStateRelationsAndNoOp();
  passed &= testStructuralChangeRevision();
  passed &= testAuthoritativeOrderingReplacement();
  passed &= testRemovalLifetimeAndAttachment();
  passed &= testMisuseRejection();
  return passed ? 0 : 1;
}
