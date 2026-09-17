// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/NodeGraph.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using codexui::nodegraph::boolFromValue;
using codexui::nodegraph::ChildListChange;
using codexui::nodegraph::exactStringFromValue;
using codexui::nodegraph::GraphChange;
using codexui::nodegraph::InspectorAgentSelector;
using codexui::nodegraph::InspectorThreadIndex;
using codexui::nodegraph::NodeGraph;
using codexui::nodegraph::NodeId;
using codexui::nodegraph::NodeKind;
using codexui::nodegraph::NodeRef;
using codexui::nodegraph::NodeState;
using codexui::nodegraph::NodeStatus;
using codexui::nodegraph::RelationKind;
using codexui::nodegraph::scalarTextFromValue;
using codexui::nodegraph::signedIntegerFromValue;
using codexui::nodegraph::unsignedIntegerFromValue;
using codexui::nodegraph::Value;
using codexui::nodegraph::valueMember;

static_assert(noexcept(std::declval<NodeGraph::WriteAccess &>().finish()));

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

NodeRef candidateSource(const InspectorAgentSelector &selector,
                        std::size_t field) {
  if (field >= selector.candidates.size() ||
      selector.candidates[field].empty())
    return {};
  const std::size_t contributor = *selector.candidates[field].rbegin() / 2;
  return contributor < selector.contributors.size()
             ? selector.contributors[contributor]
             : NodeRef{};
}

bool sameChildListChanges(const std::vector<ChildListChange> &actual,
                          std::initializer_list<ChildListChange> expected) {
  return actual.size() == expected.size() &&
         std::equal(actual.begin(), actual.end(), expected.begin());
}

bool childListOwnersBelongToChange(const GraphChange &change) {
  return std::ranges::all_of(change.childListsChanged, [&](const auto &entry) {
    return entry.owner && (std::ranges::find(change.affected, entry.owner) !=
                               change.affected.end() ||
                           std::ranges::find(change.removed, entry.owner) !=
                               change.removed.end());
  });
}

bool indexedChildrenMatch(const NodeGraph::ReadAccess &read,
                          const NodeRef &parent,
                          std::initializer_list<NodeRef> expected) {
  if (read.childCount(parent) != expected.size())
    return false;
  std::size_t index = 0;
  for (const NodeRef &child : expected) {
    const std::optional<std::size_t> childIndex = read.childIndex(child);
    if (read.childAt(parent, index) != child || read.parent(child) != parent ||
        !childIndex || *childIndex != index)
      return false;
    ++index;
  }
  return true;
}

bool testValue() {
  const Value nullValue;
  const Value explicitNull = nullptr;
  const Value boolean = true;
  const Value falseBoolean = false;
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

  const Value positiveSigned = 7;
  const Value smallestSigned = std::numeric_limits<std::int64_t>::min();
  const Value largestSignedAsUnsigned =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const Value oversizedUnsigned = std::uint64_t{1} << 63;
  const Value largestUnsigned = std::numeric_limits<std::uint64_t>::max();
  const Value *enabled = valueMember(*object.asObject(), "enabled");
  passed &= expect(enabled == &object.asObject()->find("enabled")->second &&
                       !valueMember(*object.asObject(), "missing"),
                   "decoded object lookup returns the owned member without "
                   "inserting a missing key");
  const NodeState fields{NodeStatus::Unknown, {{"name", "thread"}}};
  passed &= expect(
      valueMember(fields, "name") == &fields.fields.find("name")->second &&
          !valueMember(fields, "missing") && fields.fields.size() == 1,
      "decoded state lookup delegates to the same non-mutating "
      "member authority");
  passed &=
      expect(exactStringFromValue(&string) == "node" &&
                 exactStringFromValue(&signedInteger).empty() &&
                 exactStringFromValue(nullptr).empty(),
             "exact decoded strings reject missing and non-string values");
  passed &=
      expect(scalarTextFromValue(&signedInteger) == "-7" &&
                 scalarTextFromValue(&unsignedInteger) == "9" &&
                 scalarTextFromValue(&smallestSigned) ==
                     std::to_string(std::numeric_limits<std::int64_t>::min()) &&
                 scalarTextFromValue(&largestUnsigned) ==
                     std::to_string(std::numeric_limits<std::uint64_t>::max()),
             "scalar text accepts every integer boundary without loss");
  passed &=
      expect(scalarTextFromValue(&boolean).empty() &&
                 scalarTextFromValue(&real).empty() &&
                 scalarTextFromValue(&array).empty() &&
                 scalarTextFromValue(&object).empty(),
             "scalar text rejects bool, floating, array, and object values");
  passed &= expect(boolFromValue(&boolean) && !boolFromValue(&falseBoolean) &&
                       !boolFromValue(nullptr) && boolFromValue(&string, true),
                   "decoded bool preserves false and applies fallback only to "
                   "missing or mismatched values");
  passed &= expect(signedIntegerFromValue(&signedInteger) == -7 &&
                       signedIntegerFromValue(&unsignedInteger) == 9 &&
                       signedIntegerFromValue(&largestSignedAsUnsigned) ==
                           std::numeric_limits<std::int64_t>::max(),
                   "decoded signed integers accept both in-range alternatives");
  passed &= expect(!signedIntegerFromValue(&oversizedUnsigned) &&
                       !signedIntegerFromValue(&real) &&
                       !signedIntegerFromValue(&array),
                   "decoded signed integers reject overflow and non-integers");
  passed &= expect(unsignedIntegerFromValue(&positiveSigned) == 7 &&
                       unsignedIntegerFromValue(&largestUnsigned) ==
                           std::numeric_limits<std::uint64_t>::max(),
                   "decoded unsigned integers accept non-negative signed and "
                   "unsigned alternatives");
  passed &= expect(!unsignedIntegerFromValue(&signedInteger) &&
                       !unsignedIntegerFromValue(&string) &&
                       !unsignedIntegerFromValue(&object) &&
                       !unsignedIntegerFromValue(nullptr),
                   "decoded unsigned integers reject negatives, non-integers, "
                   "and missing values");

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
        read->revision() == 1 && runtime->incarnation() == 1 &&
            thread->incarnation() == 2 && turn->incarnation() == 3 &&
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
    write.relate(parent, RelationKind::ReviewTarget, firstTarget);
    write.relate(parent, RelationKind::ReviewTarget, secondTarget);
    write.relate(parent, RelationKind::ReviewTarget, firstTarget);
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
        sameOrder(read->related(parent, RelationKind::ReviewTarget),
                  {firstTarget, secondTarget}) &&
            read->relatedCount(parent, RelationKind::ReviewTarget) == 2 &&
            read->relatedAt(parent, RelationKind::ReviewTarget, 0) ==
                firstTarget &&
            read->relatedAt(parent, RelationKind::ReviewTarget, 1) ==
                secondTarget &&
            !read->relatedAt(parent, RelationKind::ReviewTarget, 2) &&
            read->relatedCount(parent, RelationKind::ProcessOwner) == 0 &&
            !read->relatedAt(parent, RelationKind::ProcessOwner, 0) &&
            read->relatedCount({}, RelationKind::ReviewTarget) == 0 &&
            !read->relatedAt({}, RelationKind::ReviewTarget, 0) &&
            read->hasIncomingRelation(firstTarget,
                                      RelationKind::ReviewTarget) &&
            read->hasIncomingRelation(secondTarget,
                                      RelationKind::ReviewTarget) &&
            !read->hasIncomingRelation(secondTarget,
                                       RelationKind::ProcessOwner) &&
            !read->hasIncomingRelation({}, RelationKind::ReviewTarget),
        "bounded cross-node relation access preserves order and handles "
        "missing, out-of-range, null, and incoming queries");
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
    write.relate(parent, RelationKind::ReviewTarget, firstTarget);
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
    passed &=
        expect(stateOnly.revision == 2 && stateOnly.childListsChanged.empty(),
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
    auto original = graph.write();
    original.setParent(source, child);
    auto write = std::move(original);
    write.relate(source, RelationKind::StructuralChildThread, target);
    const GraphChange structured = write.finish();
    passed &= expect(structured.revision == 3 &&
                         sameChildListChanges(structured.childListsChanged,
                                              {{source, NodeKind::Thread}}),
                     "parent and relation changes publish together with the "
                     "exact ordered-child owner");
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
    passed &=
        expect(stateOnly.revision == 4 && stateOnly.childListsChanged.empty(),
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
    passed &= expect(cleared.revision == 5 &&
                         sameChildListChanges(cleared.childListsChanged,
                                              {{source, NodeKind::Thread}}),
                     "clearing structure publishes one revision with its "
                     "ordered-child owner");
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
    passed &= expect(removed.revision == 7 && removed.childListsChanged.empty(),
                     "removing a relation target publishes once without "
                     "claiming an ordered-child change");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "removal structure stamps are readable"))
      return false;
    passed &= expect(
        read->structureChangedRevision(source) == 7 &&
            read->related(source, RelationKind::StructuralChildThread).empty(),
        "removal advances owners whose outgoing relations were unlinked "
        "without claiming their ordered children changed");
  }

  NodeRef turn;
  NodeRef item;
  {
    auto write = graph.write();
    turn = write.upsert(id(NodeKind::Turn, "structure-turn"));
    item = write.upsert(id(NodeKind::Item, "structure-item"));
    write.setParent(turn, item);
    const GraphChange itemStructure = write.finish();
    passed &= expect(itemStructure.revision == 8 &&
                         sameChildListChanges(itemStructure.childListsChanged,
                                              {{turn, NodeKind::Item}}),
                     "an unrelated item hierarchy publishes once with its "
                     "ordered-child owner");
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
  NodeRef foreignParent;
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
    foreignParent = write.upsert(id(NodeKind::Thread, "foreign-parent"));
    firstTurn = write.upsert(id(NodeKind::Turn, "turn-1"));
    secondTurn = write.upsert(id(NodeKind::Turn, "turn-2"));
    thirdTurn = write.upsert(id(NodeKind::Turn, "turn-3"));
    write.relate(runtime, RelationKind::RootThread, firstRoot);
    write.relate(runtime, RelationKind::RootThread, secondRoot);
    write.setParent(thread, firstTurn);
    write.setParent(thread, secondTurn);
    write.setParent(foreignParent, thirdTurn);
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
    passed &=
        expect(sameChildListChanges(
                   changed.childListsChanged,
                   {{thread, NodeKind::Turn}, {foreignParent, NodeKind::Turn}}),
               "ordered replacement reports both the destination and adopted "
               "child's previous owner on the transaction event");
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
  {
    auto write = graph.write();
    write.setParent(foreignParent, secondTurn);
    const GraphChange reparented = write.finish();
    passed &= expect(
        reparented.revision == 3 &&
            sameChildListChanges(
                reparented.childListsChanged,
                {{foreignParent, NodeKind::Turn}, {thread, NodeKind::Turn}}),
        "ordinary reparenting reports both child-list owners on its event");
  }
  return passed;
}

bool testOrderedChildIndexTracksEveryTopologyMutation() {
  NodeGraph graph;
  NodeRef firstParent;
  NodeRef secondParent;
  NodeRef first;
  NodeRef second;
  NodeRef third;
  NodeRef fourth;
  GraphChange changed;
  {
    auto write = graph.write();
    firstParent = write.upsert(id(NodeKind::Thread, "index-parent-1"));
    secondParent = write.upsert(id(NodeKind::Thread, "index-parent-2"));
    first = write.upsert(id(NodeKind::Turn, "index-child-1"));
    second = write.upsert(id(NodeKind::Turn, "index-child-2"));
    third = write.upsert(id(NodeKind::Turn, "index-child-3"));
    fourth = write.upsert(id(NodeKind::Turn, "index-child-4"));
    write.setParent(firstParent, first);
    write.setParent(firstParent, second);
    write.setParent(firstParent, third);
    write.setParent(secondParent, fourth);
    changed = write.finish();
  }

  bool passed = expect(childListOwnersBelongToChange(changed),
                       "child-list events name only affected owners");
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "initial child indexes are readable"))
      return false;
    passed &= expect(
        indexedChildrenMatch(*read, firstParent, {first, second, third}) &&
            indexedChildrenMatch(*read, secondParent, {fourth}) &&
            !read->childIndex(firstParent) && !read->childIndex({}),
        "append parenting stores the exact ordered-child index");
  }

  {
    auto write = graph.write();
    write.setParent(secondParent, second);
    changed = write.finish();
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "reparented child indexes are readable"))
      return false;
    passed &=
        expect(childListOwnersBelongToChange(changed) &&
                   indexedChildrenMatch(*read, firstParent, {first, third}) &&
                   indexedChildrenMatch(*read, secondParent, {fourth, second}),
               "reparenting reindexes both the old and new owner");
  }

  {
    auto write = graph.write();
    const std::array replacement{second, third, first, second};
    write.replaceChildren(firstParent, replacement);
    changed = write.finish();
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "replacement child indexes are readable"))
      return false;
    passed &= expect(
        childListOwnersBelongToChange(changed) &&
            indexedChildrenMatch(*read, firstParent, {second, third, first}) &&
            indexedChildrenMatch(*read, secondParent, {fourth}),
        "authoritative replacement deduplicates, reorders, and reindexes "
        "adopted children");
  }

  {
    auto write = graph.write();
    write.clearParent(third);
    changed = write.finish();
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "cleared child indexes are readable"))
      return false;
    passed &=
        expect(childListOwnersBelongToChange(changed) &&
                   indexedChildrenMatch(*read, firstParent, {second, first}) &&
                   !read->parent(third) && !read->childIndex(third),
               "clearing a parent removes the index and shifts later children");
  }

  {
    auto write = graph.write();
    write.remove(second);
    changed = write.finish();
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "post-removal child indexes are readable"))
      return false;
    passed &= expect(
        childListOwnersBelongToChange(changed) &&
            indexedChildrenMatch(*read, firstParent, {first}) &&
            !read->live(second) && !read->childIndex(second),
        "removal reindexes surviving siblings and clears the retired index");
  }
  return passed;
}

bool testRemovalLifetimeAndRetirement() {
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
    write.relate(relationSource, RelationKind::ReviewTarget, removedNode);
    write.relate(removedNode, RelationKind::ProcessOwner, relationTarget);
    static_cast<void>(write.finish());
  }

  bool passed = true;
  std::weak_ptr<codexui::nodegraph::Node> lifetime = removedNode;
  GraphChange removal;
  auto finishedRemovalWrite = graph.write();
  finishedRemovalWrite.remove(removedNode);
  removal = finishedRemovalWrite.finish();
  passed &= expect(removal.revision == 2 && removal.removed.size() == 1 &&
                       removal.removed.front() == removedNode &&
                       sameChildListChanges(removal.childListsChanged,
                                            {{parent, NodeKind::Turn},
                                             {removedNode, NodeKind::Item}}),
                   "removal queues a stable reference and changed-child-list "
                   "identities");
  {
    const std::array retired{removedNode};
    passed &= expect(
        throws<std::logic_error>(
            [&] { finishedRemovalWrite.releaseRetired(retired); }),
        "a finished writer cannot release retirement ownership without its "
        "graph lock");
  }

  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "the graph is readable after removal"))
      return false;
    passed &=
        expect(!read->find(removedNode->id()) &&
                   sameOrder(read->orderedNodes(),
                             {parent, child, relationSource, relationTarget}) &&
                   !read->live(removedNode),
               "removal erases canonical lookup and graph ordering");
    passed &= expect(
        read->children(parent).empty() && !read->parent(child) &&
            read->related(relationSource, RelationKind::ReviewTarget).empty() &&
            read->related(removedNode, RelationKind::ProcessOwner).empty(),
        "removal unlinks hierarchy and cross-node relations");
    passed &=
        expect(read->retiredCount() == 1 && read->retiredAt(0) == removedNode &&
                   !read->retiredAt(1) && read->retiredOrderGeneration() == 0 &&
                   read->contains(removedNode),
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

  NodeRef acknowledged = removal.removed.front();
  removal.removed.clear();
  removal.childListsChanged.clear();
  removedNode.reset();
  passed &= expect(!lifetime.expired(),
                   "the retired graph reference pins the removed node");
  {
    const std::array<NodeRef, 1> retirements{acknowledged};
    auto write = graph.write();
    write.releaseRetired(retirements);
    const GraphChange released = write.finish();
    passed &= expect(released.revision == 2 && released.empty() &&
                         graph.publishedRevision() == 2,
                     "UI retirement acknowledgement is revision-neutral");
  }
  {
    auto read = graph.tryRead();
    if (!expect(read.has_value(), "the graph is readable after retirement"))
      return false;
    passed &= expect(read->retiredCount() == 0 &&
                         read->retiredOrderGeneration() == 1 &&
                         !read->contains(acknowledged),
                     "releaseRetired drops graph lifetime ownership and "
                     "invalidates an incremental retirement cursor");
    passed &= expect(
        throws<std::invalid_argument>(
            [&] { static_cast<void>(read->state(acknowledged)); }),
        "a still-live retired reference cannot read graph-owned state after "
        "acknowledgement");
  }
  acknowledged.reset();
  passed &= expect(lifetime.expired(),
                   "a finished writer retains no event ownership after "
                   "retirement acknowledgement");

  auto movedFrom = graph.write();
  auto active = std::move(movedFrom);
  passed &=
      expect(throws<std::logic_error>([&] { movedFrom.releaseRetired({}); }),
             "a moved-from writer cannot access retirement ownership");
  static_cast<void>(active.finish());
  return passed;
}

bool testRetirementSlotsAndExactIncarnations() {
  NodeGraph graph;
  NodeGraph foreignGraph;
  NodeRef first;
  NodeRef middle;
  NodeRef last;
  NodeRef foreign;
  {
    auto write = graph.write();
    first = write.upsert(id(NodeKind::Item, "retired-first"));
    middle = write.upsert(id(NodeKind::Item, "retired-middle"));
    last = write.upsert(id(NodeKind::Item, "retired-last"));
    static_cast<void>(write.finish());
  }
  {
    auto write = foreignGraph.write();
    foreign = write.upsert(id(NodeKind::Item, "retired-middle"));
    static_cast<void>(write.finish());
  }
  GraphChange removed;
  {
    const std::array nodes{first, middle, last};
    auto write = graph.write();
    write.removeMany(nodes);
    removed = write.finish();
  }

  bool passed = true;
  {
    const std::array<NodeRef, 5> release{NodeRef{}, middle, middle, foreign,
                                         NodeRef{}};
    auto write = graph.write();
    write.releaseRetired(release);
    static_cast<void>(write.finish());
  }
  NodeRef replacement;
  {
    auto write = graph.write();
    replacement = write.upsert(id(NodeKind::Item, "retired-middle"));
    static_cast<void>(write.finish());
  }
  {
    auto read = graph.tryRead();
    passed &= expect(
        read && read->retiredCount() == 2 && read->retiredAt(0) == first &&
            read->retiredAt(1) == last && read->retiredOrderGeneration() == 1 &&
            read->contains(first) && !read->contains(middle) &&
            read->contains(last) && !read->contains(foreign) &&
            read->find(replacement->id()) == replacement &&
            read->live(replacement),
        "retirement swap-release ignores null, duplicate, and foreign refs "
        "while a same-id incarnation remains exact");
  }
  {
    auto write = graph.write();
    write.remove(replacement);
    static_cast<void>(write.finish());
  }
  {
    auto read = graph.tryRead();
    passed &= expect(read && read->retiredCount() == 3 &&
                         read->retiredAt(2) == replacement &&
                         read->retiredOrderGeneration() == 1,
                     "new retirements append without invalidating earlier "
                     "retirement slots");
  }
  removed.removed.clear();
  {
    const std::array release{last, first, replacement};
    auto write = graph.write();
    write.releaseRetired(release);
    static_cast<void>(write.finish());
  }
  {
    auto read = graph.tryRead();
    passed &= expect(read && read->retiredCount() == 0 &&
                         read->retiredOrderGeneration() == 4 &&
                         !read->contains(first) && !read->contains(last) &&
                         !read->contains(replacement),
                     "out-of-order acknowledgements release each exact "
                     "retired incarnation once");
  }
  return passed;
}

bool testIncomingRelationIndexTracksEveryMutation() {
  NodeGraph graph;
  NodeRef replacedSource;
  NodeRef clearedSource;
  NodeRef removedSource;
  NodeRef oldTarget;
  NodeRef newTarget;
  NodeRef laterTarget;
  NodeRef selfTarget;
  NodeRef multiSource;
  NodeRef multiTarget;
  {
    auto write = graph.write();
    replacedSource = write.upsert(id(NodeKind::Runtime, "replaced-source"));
    clearedSource = write.upsert(id(NodeKind::Thread, "cleared-source"));
    removedSource = write.upsert(id(NodeKind::Item, "removed-source"));
    oldTarget = write.upsert(id(NodeKind::Thread, "old-target"));
    newTarget = write.upsert(id(NodeKind::Thread, "new-target"));
    laterTarget = write.upsert(id(NodeKind::Thread, "later-target"));
    selfTarget = write.upsert(id(NodeKind::Thread, "self-target"));
    multiSource = write.upsert(id(NodeKind::Process, "multi-source"));
    multiTarget = write.upsert(id(NodeKind::Thread, "multi-target"));
    write.relate(replacedSource, RelationKind::ReviewTarget, oldTarget);
    write.relate(clearedSource, RelationKind::ProcessOwner, oldTarget);
    write.relate(removedSource, RelationKind::ReviewTarget, laterTarget);
    write.relate(selfTarget, RelationKind::ReviewTarget, selfTarget);
    write.relate(multiSource, RelationKind::ReviewTarget, multiTarget);
    write.relate(multiSource, RelationKind::ProcessOwner, multiTarget);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    const std::array replacement{newTarget};
    write.replaceRelated(replacedSource, RelationKind::ReviewTarget,
                         replacement);
    write.unrelate(clearedSource, RelationKind::ProcessOwner, oldTarget);
    write.unrelate(multiSource, RelationKind::ReviewTarget, multiTarget);
    static_cast<void>(write.finish());
  }

  bool passed = true;
  {
    auto write = graph.write();
    write.remove(oldTarget);
    const GraphChange removed = write.finish();
    passed &= expect(removed.affected.empty(),
                     "replaced and cleared incoming edges cannot create a "
                     "false topology change when their former target retires");
  }
  {
    auto write = graph.write();
    write.remove(selfTarget);
    const GraphChange removed = write.finish();
    passed &= expect(removed.affected.empty() && removed.removed.size() == 1,
                     "a self-relation retires without a second source owner");
  }
  {
    auto write = graph.write();
    write.remove(multiTarget);
    const GraphChange removed = write.finish();
    passed &= expect(
        removed.affected == std::vector<NodeRef>{multiSource},
        "removing one of two relation targets preserves the exact remaining "
        "inverse edge until its target retires");
  }

  {
    auto write = graph.write();
    write.remove(removedSource);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    write.remove(laterTarget);
    const GraphChange removed = write.finish();
    passed &= expect(removed.affected.empty(),
                     "removing a relation source clears its derived incoming "
                     "edge before the surviving target later retires");
  }
  {
    auto write = graph.write();
    write.remove(newTarget);
    const GraphChange removed = write.finish();
    passed &= expect(removed.affected == std::vector<NodeRef>{replacedSource},
                     "replacement installs exactly one incoming edge on its "
                     "new target");
  }
  {
    auto read = graph.tryRead();
    passed &= expect(
        read &&
            read->related(replacedSource, RelationKind::ReviewTarget).empty() &&
            read->related(clearedSource, RelationKind::ProcessOwner).empty(),
        "target retirement removes only the still-authoritative outgoing edge");
  }

  NodeRef owner;
  NodeRef otherOwner;
  NodeRef operation;
  NodeRef nestedOperation;
  {
    auto write = graph.write();
    owner = write.upsert(id(NodeKind::Thread, "operation-owner"));
    otherOwner = write.upsert(id(NodeKind::Thread, "other-operation-owner"));
    operation = write.upsert(id(NodeKind::Operation, "owned-operation"));
    nestedOperation =
        write.upsert(id(NodeKind::Operation, "nested-owned-operation"));
    write.relate(owner, RelationKind::PendingOperation, operation);
    write.relate(operation, RelationKind::PendingOperation, nestedOperation);
    write.relate(otherOwner, RelationKind::PendingOperation, nestedOperation);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    write.remove(owner);
    const GraphChange removed = write.finish();
    passed &= expect(
        removed.removed ==
                std::vector<NodeRef>{owner, operation, nestedOperation} &&
            removed.affected == std::vector<NodeRef>{otherOwner},
        "owner retirement closes transitively over exact pending Operations "
        "and unlinks their surviving shared owner");
  }
  {
    auto read = graph.tryRead();
    passed &= expect(
        read &&
            read->related(otherOwner, RelationKind::PendingOperation).empty() &&
            read->related(multiSource, RelationKind::ReviewTarget).empty() &&
            read->related(multiSource, RelationKind::ProcessOwner).empty(),
        "multi-kind and shared-operation inverse edges leave no stale "
        "outgoing relation after target retirement");
  }
  return passed;
}

bool testMisuseRejection() {
  NodeGraph graph;
  NodeGraph otherGraph;
  NodeRef root;
  NodeRef child;
  NodeRef grandchild;
  NodeRef operation;
  NodeRef foreign;
  {
    auto write = graph.write();
    root = write.upsert(id(NodeKind::Thread, "same-canonical"));
    child = write.upsert(id(NodeKind::Turn, "child"));
    grandchild = write.upsert(id(NodeKind::Item, "grandchild"));
    operation = write.upsert(id(NodeKind::Operation, "operation"));
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
            !read->live(foreign) &&
            throws<std::invalid_argument>(
                [&] { static_cast<void>(read->parent(foreign)); }) &&
            throws<std::invalid_argument>(
                [&] { static_cast<void>(read->childIndex(foreign)); }) &&
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
        "including through bounded hierarchy and relation access");
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
                   }) &&
                   throws<std::invalid_argument>([&] {
                     write.relate(root, RelationKind::PendingOperation, child);
                   }) &&
                   throws<std::invalid_argument>([&] {
                     const std::array targets{operation, grandchild};
                     write.replaceRelated(root, RelationKind::PendingOperation,
                                          targets);
                   }),
               "foreign NodeRefs and malformed destructive relations are "
               "rejected before mutation");
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
    passed &=
        expect(!read->parent(root) && read->parent(child) == root &&
                   read->parent(grandchild) == child &&
                   sameOrder(read->children(root), {child}) &&
                   sameOrder(read->children(child), {grandchild}) &&
                   read->related(root, RelationKind::PendingOperation).empty(),
               "misuse rejection preserves the acyclic hierarchy");
  }
  return passed;
}

bool testBatchRemovalIsAtomicAndApproximatelyLinear() {
  const auto measure = [](std::size_t count) {
    NodeGraph graph;
    NodeRef survivor;
    std::vector<NodeRef> removed;
    removed.reserve(count);
    {
      auto write = graph.write();
      survivor = write.upsert(id(NodeKind::Runtime, "batch-survivor"));
      for (std::size_t index = 0; index < count; ++index) {
        NodeRef node = write.upsert(
            id(NodeKind::Item, "batch-item-" + std::to_string(index)));
        write.relate(node, RelationKind::ReviewTarget, survivor);
        removed.emplace_back(std::move(node));
      }
      write.relate(survivor, RelationKind::PendingPrompt, removed.front());
      write.relate(survivor, RelationKind::PendingPrompt, removed[count / 2]);
      write.relate(survivor, RelationKind::PendingPrompt, removed.back());
      static_cast<void>(write.finish());
    }

    const auto started = std::chrono::steady_clock::now();
    GraphChange change;
    {
      auto write = graph.write();
      write.removeMany(removed);
      change = write.finish();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    bool valid = change.revision == 2 && change.removed.size() == count;
    auto read = graph.tryRead();
    valid = valid && read && read->orderedNodes().size() == 1 &&
            read->orderedNodes().front() == survivor &&
            read->related(survivor, RelationKind::PendingPrompt).empty() &&
            read->retiredCount() == count && !read->live(removed.front()) &&
            !read->live(removed.back());
    return std::pair{valid, elapsed};
  };

  const auto [smallValid, smallElapsed] = measure(3000);
  const auto [largeValid, largeElapsed] = measure(6000);
  const auto allowance = smallElapsed * 3 + std::chrono::milliseconds(20);
  bool passed = expect(
      smallValid && largeValid && largeElapsed <= allowance,
      "batch removal preserves order, relations, lifetime, and near-linear "
      "scaling");
  std::cout
      << "batch-removal ns (3000 / 6000): "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(smallElapsed)
             .count()
      << " / "
      << std::chrono::duration_cast<std::chrono::nanoseconds>(largeElapsed)
             .count()
      << '\n';

  NodeGraph graph;
  NodeGraph foreignGraph;
  NodeRef local;
  NodeRef foreign;
  {
    auto write = graph.write();
    local = write.upsert(id(NodeKind::Item, "atomic-local"));
    static_cast<void>(write.finish());
  }
  {
    auto write = foreignGraph.write();
    foreign = write.upsert(id(NodeKind::Item, "atomic-foreign"));
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    const std::array invalid{local, foreign};
    passed &= expect(
        throws<std::invalid_argument>([&] { write.removeMany(invalid); }),
        "batch removal validates every NodeRef before mutation");
    const GraphChange unchanged = write.finish();
    passed &= expect(unchanged.revision == 1 && unchanged.empty(),
                     "failed batch validation leaves the graph unchanged");
  }
  {
    auto read = graph.tryRead();
    passed &= expect(read && read->find(local->id()) == local &&
                         read->live(local),
                     "failed batch validation preserves lookup and lifetime");
  }

  const auto measureTailRemoval = [](std::size_t prefixSize) {
    constexpr std::size_t Iterations = 128;
    NodeGraph measuredGraph;
    NodeRef parent;
    NodeRef relationSource;
    std::vector<NodeRef> prefix;
    prefix.reserve(prefixSize);
    {
      auto write = measuredGraph.write();
      parent = write.upsert(id(NodeKind::Thread, "locality-parent"));
      relationSource = write.upsert(id(NodeKind::Runtime, "locality-source"));
      for (std::size_t index = 0; index < prefixSize; ++index) {
        NodeRef item = write.upsert(
            id(NodeKind::Item, "locality-prefix-" + std::to_string(index)));
        write.setParent(parent, item);
        prefix.emplace_back(std::move(item));
      }
      write.replaceRelated(relationSource, RelationKind::PendingPrompt, prefix);
      static_cast<void>(write.finish());
    }

    bool valid = true;
    std::chrono::steady_clock::duration elapsed{};
    for (std::size_t iteration = 0; iteration < Iterations; ++iteration) {
      NodeRef tail;
      {
        auto write = measuredGraph.write();
        tail = write.upsert(id(NodeKind::Item, "locality-tail"));
        write.setParent(parent, tail);
        write.relate(relationSource, RelationKind::PendingPrompt, tail);
        static_cast<void>(write.finish());
      }
      GraphChange removal;
      const auto started = std::chrono::steady_clock::now();
      {
        auto write = measuredGraph.write();
        write.remove(tail);
        removal = write.finish();
      }
      elapsed += std::chrono::steady_clock::now() - started;
      valid =
          valid &&
          removal.affected == std::vector<NodeRef>{parent, relationSource} &&
          sameChildListChanges(removal.childListsChanged,
                               {{parent, NodeKind::Item}});
      {
        auto write = measuredGraph.write();
        write.releaseRetired(removal.removed);
        static_cast<void>(write.finish());
      }
    }
    auto read = measuredGraph.tryRead();
    valid = valid && read && read->childCount(parent) == prefixSize &&
            read->relatedCount(relationSource, RelationKind::PendingPrompt) ==
                prefixSize &&
            read->retiredCount() == 0;
    return std::pair{valid, elapsed};
  };

  const auto [shortPrefixValid, shortPrefixElapsed] = measureTailRemoval(2'000);
  const auto [longPrefixValid, longPrefixElapsed] = measureTailRemoval(40'000);
  const auto localityAllowance =
      shortPrefixElapsed * 4 + std::chrono::milliseconds(5);
  passed &= expect(shortPrefixValid && longPrefixValid &&
                       longPrefixElapsed <= localityAllowance,
                   "single-tail removal is independent of unchanged ordered "
                   "child and relation prefixes");
  std::cout << "tail-removal ns (2000 / 40000 unchanged neighbors): "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   shortPrefixElapsed)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   longPrefixElapsed)
                   .count()
            << '\n';
  return passed;
}

bool testInspectorAgentTailAppendIsIncremental() {
  constexpr std::size_t ContributionCount = 10'000;
  NodeGraph graph;
  NodeRef owner;
  NodeRef turn;
  NodeRef child;
  NodeRef previousContribution;
  {
    auto write = graph.write();
    owner = write.upsert(id(NodeKind::Thread, "indexed-agent-owner"));
    turn = write.upsert(id(NodeKind::Turn, "indexed-agent-turn"));
    child = write.upsert(id(NodeKind::Thread, "indexed-agent-child"));
    write.setParent(owner, turn);
    for (std::size_t index = 0; index < ContributionCount; ++index) {
      NodeState state;
      state.status = NodeStatus::Running;
      state.fields = {{"type", "subAgentActivity"},
                      {"kind", index == 0 ? "started" : "progress"},
                      {"prompt", "contribution " + std::to_string(index)}};
      NodeRef item = write.upsert(
          id(NodeKind::Item, "indexed-agent-" + std::to_string(index)),
          std::move(state));
      write.setParent(turn, item);
      write.relate(item, RelationKind::AgentChildThread, child);
      previousContribution = item;
    }
    static_cast<void>(write.finish());
  }

  std::uintptr_t selectorStorage = 0;
  std::uintptr_t contributorStorage = 0;
  std::uint64_t orderRevision = 0;
  {
    auto read = graph.tryRead();
    const auto *index = read ? read->inspectorIndex(owner) : nullptr;
    if (index && index->agents.size() == 1) {
      selectorStorage =
          reinterpret_cast<std::uintptr_t>(&index->agents.front());
      contributorStorage = reinterpret_cast<std::uintptr_t>(
          &index->agents.front().contributors.front());
      orderRevision = index->agentOrderRevision;
    }
  }

  NodeRef appended;
  std::uint64_t appendRevision = 0;
  const auto appendStarted = std::chrono::steady_clock::now();
  {
    auto write = graph.write();
    NodeState state;
    state.status = NodeStatus::Completed;
    state.fields = {{"type", "subAgentActivity"},
                    {"kind", "completed"},
                    {"prompt", "tail contribution"}};
    appended = write.upsert(id(NodeKind::Item, "indexed-agent-tail"),
                            std::move(state));
    write.setParent(turn, appended);
    write.relate(appended, RelationKind::AgentChildThread, child);
    appendRevision = write.finish().revision;
  }
  const auto appendElapsed =
      std::chrono::steady_clock::now() - appendStarted;

  auto read = graph.tryRead();
  const auto *index = read ? read->inspectorIndex(owner) : nullptr;
  bool passed = expect(
      selectorStorage != 0 && index && index->agents.size() == 1 &&
          reinterpret_cast<std::uintptr_t>(&index->agents.front()) ==
              selectorStorage &&
          reinterpret_cast<std::uintptr_t>(
              &index->agents.front().contributors.front()) ==
              contributorStorage &&
          index->agentOrderRevision == orderRevision &&
          index->agentChangedRevision == appendRevision &&
          !index->agents.front().contributors.empty() &&
          index->agents.front().contributors.back() == appended &&
          appendElapsed < std::chrono::milliseconds(50),
      "Agent tail append updates one selector without rebuilding its 10,000 "
      "contribution history");
  read.reset();

  std::uint64_t fallbackRevision = 0;
  const auto started = std::chrono::steady_clock::now();
  {
    auto write = graph.write();
    write.eraseField(appended, "prompt");
    fallbackRevision = write.finish().revision;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  read = graph.tryRead();
  index = read ? read->inspectorIndex(owner) : nullptr;
  passed &= expect(
      index && index->agents.size() == 1 &&
          candidateSource(index->agents.front(), 4) == previousContribution &&
          index->agentChangedRevision == fallbackRevision &&
          elapsed < std::chrono::milliseconds(50),
      "clearing the latest Agent field exposes its predecessor with bounded indexed work");
  read.reset();

  const auto toolStarted = std::chrono::steady_clock::now();
  {
    auto write = graph.write();
    write.setField(previousContribution, "tool", "display-only-tool");
    static_cast<void>(write.finish());
  }
  const auto toolElapsed = std::chrono::steady_clock::now() - toolStarted;
  read = graph.tryRead();
  index = read ? read->inspectorIndex(owner) : nullptr;
  passed &= expect(
      index && candidateSource(index->agents.front(), 1) ==
                   previousContribution &&
          toolElapsed < std::chrono::milliseconds(50),
      "subAgent display-tool updates do not rebuild 10,000 contributors");
  read.reset();

  NodeRef collaboration;
  {
    auto write = graph.write();
    NodeState state;
    state.status = NodeStatus::Running;
    state.fields = {{"type", "collabAgentToolCall"},
                    {"tool", "spawn_agent"},
                    {"kind", "started"}};
    collaboration = write.upsert(id(NodeKind::Item, "indexed-collaboration"),
                                 std::move(state));
    write.setParent(turn, collaboration);
    write.relate(collaboration, RelationKind::AgentChildThread, child);
    static_cast<void>(write.finish());
  }
  const auto kindStarted = std::chrono::steady_clock::now();
  {
    auto write = graph.write();
    write.setField(collaboration, "kind", "progress");
    static_cast<void>(write.finish());
  }
  const auto kindElapsed = std::chrono::steady_clock::now() - kindStarted;
  passed &= expect(kindElapsed < std::chrono::milliseconds(50),
                   "collaboration lifecycle updates do not rebuild 10,000 "
                   "contributors");
  return passed;
}

bool testInspectorIndexWorkIsBoundedByChangedDependencies() {
  const auto measureTurnStatus = [](std::size_t itemCount) {
    NodeGraph graph;
    NodeRef thread;
    NodeRef turn;
    {
      auto write = graph.write();
      thread = write.upsert(id(NodeKind::Thread, "status-thread"));
      turn = write.upsert(id(NodeKind::Turn, "status-turn"));
      write.setParent(thread, turn);
      for (std::size_t index = 0; index < itemCount; ++index) {
        NodeRef item = write.upsert(
            id(NodeKind::Item, "plain-status-" + std::to_string(index)));
        write.setParent(turn, item);
      }
      static_cast<void>(write.finish());
    }
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
      auto write = graph.write();
      write.setStatus(turn, iteration % 2 == 0 ? NodeStatus::Running
                                               : NodeStatus::Pending);
      static_cast<void>(write.finish());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    auto read = graph.tryRead();
    const auto *index = read ? read->inspectorIndex(thread) : nullptr;
    return std::pair{index && index->agents.empty(), elapsed};
  };
  const auto [shortStatusValid, shortStatus] = measureTurnStatus(2'000);
  const auto [longStatusValid, longStatus] = measureTurnStatus(40'000);
  bool passed = expect(
      shortStatusValid && longStatusValid &&
          longStatus <= shortStatus * 5 + std::chrono::milliseconds(20),
      "Turn status updates do not scan unrelated Item history");

  const auto measureLegacyPlan = [](std::size_t laterTurns) {
    NodeGraph graph;
    NodeRef thread;
    NodeRef plan;
    {
      auto write = graph.write();
      thread = write.upsert(id(NodeKind::Thread, "plan-thread"));
      const NodeRef planTurn =
          write.upsert(id(NodeKind::Turn, "plan-source-turn"));
      NodeState state;
      state.fields = {{"type", "plan"}, {"text", "initial"}};
      plan = write.upsert(id(NodeKind::Item, "legacy-plan"),
                          std::move(state));
      write.setParent(thread, planTurn);
      write.setParent(planTurn, plan);
      for (std::size_t index = 0; index < laterTurns; ++index) {
        const NodeRef turn = write.upsert(
            id(NodeKind::Turn, "later-plan-turn-" + std::to_string(index)));
        write.setParent(thread, turn);
      }
      static_cast<void>(write.finish());
    }
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
      auto write = graph.write();
      write.setField(plan, "text", iteration % 2 == 0 ? "even" : "odd");
      static_cast<void>(write.finish());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    auto read = graph.tryRead();
    const auto *index = read ? read->inspectorIndex(thread) : nullptr;
    return std::pair{index && index->planSource == plan &&
                         index->planRowCount == 1,
                     elapsed};
  };
  const auto [shortPlanValid, shortPlan] = measureLegacyPlan(2'000);
  const auto [longPlanValid, longPlan] = measureLegacyPlan(40'000);
  passed &= expect(
      shortPlanValid && longPlanValid &&
          longPlan <= shortPlan * 5 + std::chrono::milliseconds(20),
      "a selected legacy Plan update does not scan later Turn history");

  const auto measureAgentRemoval = [](std::size_t count) {
    NodeGraph graph;
    NodeRef thread;
    std::vector<NodeRef> items;
    items.reserve(count);
    {
      auto write = graph.write();
      thread = write.upsert(id(NodeKind::Thread, "removal-thread"));
      const NodeRef child =
          write.upsert(id(NodeKind::Thread, "removal-child"));
      for (std::size_t index = 0; index < count; ++index) {
        const NodeRef turn = write.upsert(
            id(NodeKind::Turn, "removal-turn-" + std::to_string(index)));
        NodeState state;
        state.fields = {{"type", "subAgentActivity"}, {"kind", "started"}};
        NodeRef item = write.upsert(
            id(NodeKind::Item, "removal-agent-" + std::to_string(index)),
            std::move(state));
        write.setParent(thread, turn);
        write.setParent(turn, item);
        write.relate(item, RelationKind::AgentChildThread, child);
        items.emplace_back(std::move(item));
      }
      static_cast<void>(write.finish());
    }
    const auto started = std::chrono::steady_clock::now();
    GraphChange change;
    {
      auto write = graph.write();
      write.removeMany(items);
      change = write.finish();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    auto read = graph.tryRead();
    const auto *index = read ? read->inspectorIndex(thread) : nullptr;
    return std::pair{index && index->agents.empty() &&
                         change.removed.size() == count,
                     elapsed};
  };
  const auto [smallRemovalValid, smallRemoval] = measureAgentRemoval(300);
  const auto [largeRemovalValid, largeRemoval] = measureAgentRemoval(3'000);
  passed &= expect(
      smallRemovalValid && largeRemovalValid &&
          largeRemoval <= smallRemoval * 20 + std::chrono::milliseconds(50),
      "batched Agent removals across Turns avoid a quadratic ambiguity pass");
  const auto measureUnrelatedWrites = [](std::size_t count,
                                         bool createThreads) {
    NodeGraph graph;
    NodeRef target;
    NodeRef first;
    NodeRef last;
    {
      auto write = graph.write();
      target = write.upsert(id(NodeKind::Item, "unrelated-target"));
      for (std::size_t index = 0; index < count; ++index) {
        NodeRef node = write.upsert(
            id(createThreads ? NodeKind::Thread : NodeKind::Item,
               "unrelated-" + std::to_string(index)));
        if (index == 0)
          first = node;
        last = std::move(node);
      }
      static_cast<void>(write.finish());
    }
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
      auto write = graph.write();
      write.setField(target, "tick", iteration % 2);
      static_cast<void>(write.finish());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    auto read = graph.tryRead();
    const bool valid =
        read &&
        (!createThreads ||
         (read->inspectorIndex(first) && read->inspectorIndex(last)));
    return std::pair{valid, elapsed};
  };
  const auto [fewPlainValid, fewPlain] =
      measureUnrelatedWrites(2'000, false);
  const auto [manyPlainValid, manyPlain] =
      measureUnrelatedWrites(40'000, false);
  const auto [fewThreadsValid, fewThreads] =
      measureUnrelatedWrites(2'000, true);
  const auto [manyThreadsValid, manyThreads] =
      measureUnrelatedWrites(40'000, true);
  passed &= expect(
      fewPlainValid && manyPlainValid && fewThreadsValid && manyThreadsValid &&
          manyPlain <= fewPlain * 5 + std::chrono::milliseconds(20) &&
          manyThreads <= fewThreads * 5 + std::chrono::milliseconds(20),
      "ordinary finishes do not scan all nodes or all Thread indexes");

  const auto measurePayloadPresence = [](std::size_t bytes) {
    NodeGraph graph;
    NodeRef owner;
    NodeRef turn;
    {
      auto write = graph.write();
      owner = write.upsert(id(NodeKind::Thread, "payload-owner"));
      turn = write.upsert(id(NodeKind::Turn, "payload-turn"));
      const NodeRef child =
          write.upsert(id(NodeKind::Thread, "payload-child"));
      write.setParent(owner, turn);
      NodeState state;
      state.status = NodeStatus::Running;
      state.fields = {{"type", "subAgentActivity"},
                      {"kind", "started"},
                      {"prompt", std::string(bytes, 'p')},
                      {"resultText", std::string(bytes, 'r')}};
      const NodeRef source = write.upsert(id(NodeKind::Item, "payload-source"),
                                          std::move(state));
      write.setParent(turn, source);
      write.relate(source, RelationKind::AgentChildThread, child);
      static_cast<void>(write.finish());
    }
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < 64; ++iteration) {
      auto write = graph.write();
      write.setStatus(turn, iteration % 2 == 0 ? NodeStatus::Running
                                               : NodeStatus::Completed);
      static_cast<void>(write.finish());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    auto read = graph.tryRead();
    const auto *index = read ? read->inspectorIndex(owner) : nullptr;
    return std::pair{index && index->agents.size() == 1, elapsed};
  };
  const auto [smallPayloadValid, smallPayload] = measurePayloadPresence(8);
  const auto [largePayloadValid, largePayload] =
      measurePayloadPresence(2 * 1024 * 1024);
  passed &= expect(
      smallPayloadValid && largePayloadValid &&
          largePayload <= smallPayload * 6 + std::chrono::milliseconds(20),
      "status maintenance tests large Agent payload presence without copies");

  const auto measureExactChildFanout = [](std::size_t unrelatedThreads,
                                           std::size_t exactParents) {
    NodeGraph graph;
    NodeRef child;
    NodeRef message;
    NodeRef firstUnrelated;
    NodeRef lastUnrelated;
    std::vector<NodeRef> owners;
    owners.reserve(exactParents);
    {
      auto write = graph.write();
      child = write.upsert(id(NodeKind::Thread, "fanout-shared-child"));
      const NodeRef childTurn =
          write.upsert(id(NodeKind::Turn, "fanout-shared-child-turn"));
      NodeState messageState;
      messageState.fields =
          {{"type", "agentMessage"}, {"text", "fanout-initial"}};
      message = write.upsert(id(NodeKind::Item, "fanout-shared-message"),
                             std::move(messageState));
      write.setParent(child, childTurn);
      write.setParent(childTurn, message);
      for (std::size_t index = 0; index < unrelatedThreads; ++index) {
        NodeRef thread = write.upsert(
            id(NodeKind::Thread, "fanout-unrelated-" + std::to_string(index)));
        if (index == 0)
          firstUnrelated = thread;
        lastUnrelated = std::move(thread);
      }
      for (std::size_t index = 0; index < exactParents; ++index) {
        NodeRef owner = write.upsert(
            id(NodeKind::Thread, "fanout-owner-" + std::to_string(index)));
        const NodeRef turn = write.upsert(
            id(NodeKind::Turn, "fanout-turn-" + std::to_string(index)));
        NodeState sourceState;
        sourceState.status = NodeStatus::Running;
        sourceState.fields = {{"type", "subAgentActivity"},
                              {"kind", "started"}};
        const NodeRef source = write.upsert(
            id(NodeKind::Item, "fanout-source-" + std::to_string(index)),
            std::move(sourceState));
        write.setParent(owner, turn);
        write.setParent(turn, source);
        write.relate(source, RelationKind::AgentChildThread, child);
        owners.emplace_back(std::move(owner));
      }
      static_cast<void>(write.finish());
    }

    auto read = graph.tryRead();
    const auto *childIndex = read ? read->inspectorIndex(child) : nullptr;
    const std::uint64_t childRevision =
        childIndex ? childIndex->agentChangedRevision : 0;
    const auto *firstIndex =
        !owners.empty() && read ? read->inspectorIndex(owners.front()) : nullptr;
    const std::uint64_t orderRevision =
        firstIndex ? firstIndex->agentOrderRevision : 0;
    const auto *firstUnrelatedIndex =
        firstUnrelated && read ? read->inspectorIndex(firstUnrelated) : nullptr;
    const auto *lastUnrelatedIndex =
        lastUnrelated && read ? read->inspectorIndex(lastUnrelated) : nullptr;
    const std::uint64_t firstUnrelatedRevision =
        firstUnrelatedIndex ? firstUnrelatedIndex->agentChangedRevision : 0;
    const std::uint64_t lastUnrelatedRevision =
        lastUnrelatedIndex ? lastUnrelatedIndex->agentChangedRevision : 0;
    bool valid = childIndex && childIndex->latestAgentMessage() == message &&
                 std::ranges::all_of(owners, [&](const NodeRef &owner) {
                   const auto *index = read->inspectorIndex(owner);
                   return index && index->agents.size() == 1 &&
                          index->agents.front().exactChild == child &&
                          index->agents.front().contributors.size() == 1 &&
                          index->agentOrderRevision == orderRevision;
                 });
    read.reset();

    {
      auto write = graph.write();
      write.setStatus(child, NodeStatus::Pending);
      write.setField(message, "text", "fanout-warmup");
      static_cast<void>(write.finish());
    }
    std::uint64_t finalRevision = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
      auto write = graph.write();
      write.setStatus(child, iteration % 2 == 0 ? NodeStatus::Running
                                                : NodeStatus::Completed);
      write.setField(message, "text",
                     iteration % 2 == 0 ? "fanout-even" : "fanout-odd");
      finalRevision = write.finish().revision;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    read = graph.tryRead();
    childIndex = read ? read->inspectorIndex(child) : nullptr;
    const auto finalChild = read ? read->state(child) : nullptr;
    const auto finalMessage = read ? read->state(message) : nullptr;
    valid = valid && childIndex &&
            childIndex->latestAgentMessage() == message &&
            childIndex->agentChangedRevision == childRevision && finalChild &&
            finalChild->status == NodeStatus::Completed && finalMessage &&
            scalarTextFromValue(valueMember(*finalMessage, "text")) ==
                "fanout-odd" &&
            std::ranges::all_of(owners, [&](const NodeRef &owner) {
              const auto *index = read->inspectorIndex(owner);
              return index && index->agents.size() == 1 &&
                     index->agents.front().exactChild == child &&
                     index->agents.front().contributors.size() == 1 &&
                     index->agentOrderRevision == orderRevision &&
                     index->agentChangedRevision == finalRevision;
            });
    if (unrelatedThreads != 0) {
      firstUnrelatedIndex = read->inspectorIndex(firstUnrelated);
      lastUnrelatedIndex = read->inspectorIndex(lastUnrelated);
      valid = valid && firstUnrelatedIndex && lastUnrelatedIndex &&
              firstUnrelatedIndex->agentChangedRevision ==
                  firstUnrelatedRevision &&
              lastUnrelatedIndex->agentChangedRevision ==
                  lastUnrelatedRevision;
    }
    return std::pair{valid, elapsed};
  };
  const auto [shortFanoutLocalityValid, shortFanoutLocality] =
      measureExactChildFanout(2'000, 8);
  const auto [longFanoutLocalityValid, longFanoutLocality] =
      measureExactChildFanout(40'000, 8);
  const auto [smallFanoutValid, smallFanout] =
      measureExactChildFanout(0, 100);
  const auto [largeFanoutValid, largeFanout] =
      measureExactChildFanout(0, 1'000);
  passed &= expect(
      shortFanoutLocalityValid && longFanoutLocalityValid &&
          longFanoutLocality <=
              shortFanoutLocality * 5 + std::chrono::milliseconds(20),
      "exact child fanout work is independent of unrelated Thread indexes");
  passed &= expect(
      smallFanoutValid && largeFanoutValid &&
          largeFanout <= smallFanout * 15 + std::chrono::milliseconds(20),
      "exact child status and result fanout scale with actual parent count");
  std::cout << "inspector indexed ns (status 2k/40k, plan 2k/40k, removal 300/3k): "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(shortStatus)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(longStatus)
                   .count()
            << ", "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(shortPlan)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(longPlan)
                   .count()
            << ", "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(smallRemoval)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(largeRemoval)
                   .count()
            << "; unrelated plain/thread 2k/40k: "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(fewPlain)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(manyPlain)
                   .count()
            << ", "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(fewThreads)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(manyThreads)
                   .count()
            << "; payload 8/2MiB: "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(smallPayload)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(largePayload)
                   .count()
            << "; exact fanout locality 2k/40k and size 100/1k: "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   shortFanoutLocality)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   longFanoutLocality)
                   .count()
            << ", "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(smallFanout)
                   .count()
            << " / "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(largeFanout)
                   .count()
            << '\n';
  return passed;
}

bool testInspectorStructuralImpactsStayExact() {
  NodeGraph graph;
  NodeRef threadA;
  NodeRef turnA;
  NodeRef childA;
  NodeRef sourceA;
  NodeRef threadB;
  NodeRef turnB;
  NodeRef childB;
  NodeRef sourceB;
  {
    auto write = graph.write();
    threadA = write.upsert(id(NodeKind::Thread, "impact-thread-a"));
    turnA = write.upsert(id(NodeKind::Turn, "impact-turn-a"));
    childA = write.upsert(id(NodeKind::Thread, "impact-child-a"));
    threadB = write.upsert(id(NodeKind::Thread, "impact-thread-b"));
    turnB = write.upsert(id(NodeKind::Turn, "impact-turn-b"));
    childB = write.upsert(id(NodeKind::Thread, "impact-child-b"));
    write.setParent(threadA, turnA);
    write.setParent(threadB, turnB);
    NodeState stateA;
    stateA.fields = {{"type", "subAgentActivity"},
                     {"kind", "started"},
                     {"prompt", "source a"}};
    sourceA = write.upsert(id(NodeKind::Item, "impact-source-a"),
                           std::move(stateA));
    NodeState stateB;
    stateB.fields = {{"type", "subAgentActivity"},
                     {"kind", "started"},
                     {"prompt", "source b"}};
    sourceB = write.upsert(id(NodeKind::Item, "impact-source-b"),
                           std::move(stateB));
    write.setParent(turnA, sourceA);
    write.setParent(turnB, sourceB);
    write.relate(sourceA, RelationKind::AgentChildThread, childA);
    write.relate(sourceB, RelationKind::AgentChildThread, childB);
    static_cast<void>(write.finish());
  }

  NodeRef appended;
  {
    auto write = graph.write();
    write.remove(sourceA);
    NodeState state;
    state.fields = {{"type", "subAgentActivity"},
                    {"kind", "progress"},
                    {"prompt", "thread b tail"}};
    appended = write.upsert(id(NodeKind::Item, "impact-thread-b-tail"),
                            std::move(state));
    write.setParent(turnB, appended);
    write.relate(appended, RelationKind::AgentChildThread, childB);
    write.relate(appended, RelationKind::AgentChildThread, childA);
    static_cast<void>(write.finish());
  }
  auto read = graph.tryRead();
  const auto *indexA = read ? read->inspectorIndex(threadA) : nullptr;
  const auto *indexB = read ? read->inspectorIndex(threadB) : nullptr;
  bool passed = expect(
      indexA && indexA->agents.empty() && indexB &&
          indexB->agents.size() == 1 &&
          indexB->agents.front().contributors.size() == 2 &&
          candidateSource(indexB->agents.front(), 4) == appended,
      "removing an Agent source in one thread does not disable an exact tail append in another");
  read.reset();
  NodeRef plainOne;
  NodeRef plainTwo;
  {
    auto write = graph.write();
    plainOne = write.upsert(id(NodeKind::Item, "impact-plain-one"));
    plainTwo = write.upsert(id(NodeKind::Item, "impact-plain-two"));
    write.setParent(turnB, plainOne);
    write.setParent(turnB, plainTwo);
    static_cast<void>(write.finish());
  }
  std::uint64_t coalescedAgentRevision = 0;
  {
    auto write = graph.write();
    write.setField(appended, "prompt", "coalesced prompt");
    const std::array order{sourceB, appended, plainTwo, plainOne};
    write.replaceChildren(turnB, order);
    coalescedAgentRevision = write.finish().revision;
  }
  read = graph.tryRead();
  indexB = read ? read->inspectorIndex(threadB) : nullptr;
  passed &= expect(
      indexB && indexB->agentChangedRevision == coalescedAgentRevision &&
          candidateSource(indexB->agents.front(), 4) == appended,
      "a same-transaction Agent value update remains visible when topology also rebuilds the section");
  read.reset();

  NodeRef collaboration;
  {
    auto write = graph.write();
    NodeState state;
    state.fields = {{"type", "collabAgentToolCall"},
                    {"tool", "spawn_agent"},
                    {"prompt", "spawned"}};
    collaboration = write.upsert(id(NodeKind::Item, "impact-collaboration"),
                                 std::move(state));
    write.setParent(turnA, collaboration);
    write.relate(collaboration, RelationKind::AgentChildThread, childA);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    write.setField(collaboration, "tool", "send_message");
    static_cast<void>(write.finish());
  }
  read = graph.tryRead();
  indexA = read ? read->inspectorIndex(threadA) : nullptr;
  passed &= expect(indexA && indexA->agents.empty(),
                   "changing a sole exact creator into a non-creator immediately matches a clean rebuild");
  const std::uint64_t emptyAgentRevision =
      indexA ? indexA->agentChangedRevision : 0;
  const std::uint64_t unrelatedAgentRevision =
      indexB ? indexB->agentChangedRevision : 0;
  read.reset();
  {
    auto write = graph.write();
    const NodeRef childTurn =
        write.upsert(id(NodeKind::Turn, "impact-child-turn"));
    NodeState messageState;
    messageState.fields = {{"type", "agentMessage"},
                           {"text", "unobserved result"}};
    const NodeRef message = write.upsert(
        id(NodeKind::Item, "impact-child-message"),
        std::move(messageState));
    write.setParent(childA, childTurn);
    write.setParent(childTurn, message);
    write.setStatus(childA, NodeStatus::Completed);
    write.setStatus(turnA, NodeStatus::Completed);
    static_cast<void>(write.finish());
  }
  read = graph.tryRead();
  indexA = read ? read->inspectorIndex(threadA) : nullptr;
  indexB = read ? read->inspectorIndex(threadB) : nullptr;
  passed &= expect(
      indexA && indexA->agents.empty() &&
          indexA->agentChangedRevision == emptyAgentRevision && indexB &&
          indexB->agents.size() == 1 &&
          indexB->agents.front().exactChild == childB &&
          indexB->agentChangedRevision == unrelatedAgentRevision,
      "child status and result updates do not invalidate an owner whose only "
      "incoming source is not an Agent contributor or a source that "
      "contributes only to a different child");

  NodeGraph lateTypedGraph;
  NodeRef lateOwner;
  NodeRef latePlanTurn;
  NodeRef latePlainTurn;
  NodeRef lateChild;
  NodeRef latePlan;
  NodeRef lateMessage;
  {
    auto write = lateTypedGraph.write();
    lateOwner = write.upsert(id(NodeKind::Thread, "late-typed-owner"));
    latePlanTurn = write.upsert(id(NodeKind::Turn, "late-typed-turn"));
    latePlainTurn =
        write.upsert(id(NodeKind::Turn, "late-typed-plain-turn"));
    lateChild = write.upsert(id(NodeKind::Thread, "late-typed-child"));
    const NodeRef childTurn =
        write.upsert(id(NodeKind::Turn, "late-typed-child-turn"));
    const NodeRef activity =
        write.upsert(id(NodeKind::Item, "late-typed-activity"));
    latePlan = write.upsert(id(NodeKind::Item, "late-typed-plan"));
    lateMessage = write.upsert(id(NodeKind::Item, "late-typed-message"));
    write.setParent(lateOwner, latePlanTurn);
    write.setParent(latePlanTurn, activity);
    write.setParent(latePlanTurn, latePlan);
    write.setParent(lateOwner, latePlainTurn);
    write.setParent(lateChild, childTurn);
    write.setParent(childTurn, lateMessage);
    write.setField(activity, "type", "subAgentActivity");
    write.setField(activity, "kind", "started");
    write.relate(activity, RelationKind::AgentChildThread, lateChild);
    write.setField(latePlan, "type", "plan");
    write.setField(latePlan, "text", "late plan");
    write.setField(lateMessage, "type", "agentMessage");
    write.setField(lateMessage, "text", "late result");
    static_cast<void>(write.finish());
  }
  read = lateTypedGraph.tryRead();
  const auto *lateOwnerIndex =
      read ? read->inspectorIndex(lateOwner) : nullptr;
  const auto *lateChildIndex =
      read ? read->inspectorIndex(lateChild) : nullptr;
  passed &= expect(
      lateOwnerIndex && lateOwnerIndex->agents.size() == 1 &&
          lateOwnerIndex->planSource == latePlan && lateChildIndex &&
          lateChildIndex->latestAgentMessage() == lateMessage,
      "final Item type is indexed when parenting precedes typing in one transaction");
  read.reset();
  std::uint64_t coalescedPlanRevision = 0;
  {
    auto write = lateTypedGraph.write();
    write.setField(latePlan, "text", "coalesced plan");
    const std::array order{latePlainTurn, latePlanTurn};
    write.replaceChildren(lateOwner, order);
    coalescedPlanRevision = write.finish().revision;
  }
  read = lateTypedGraph.tryRead();
  lateOwnerIndex = read ? read->inspectorIndex(lateOwner) : nullptr;
  passed &= expect(
      lateOwnerIndex &&
          lateOwnerIndex->planChangedRevision == coalescedPlanRevision &&
          lateOwnerIndex->planSource == latePlan,
      "a same-transaction Plan value update remains visible when topology also rebuilds the section");
  return passed;
}

bool testInspectorIncrementalAuthorityMatchesFinalGraph() {
  NodeGraph graph;
  NodeRef ownerA;
  NodeRef ownerB;
  NodeRef turnA;
  NodeRef turnB;
  NodeRef child;
  NodeRef agent;
  NodeRef plan;
  {
    auto write = graph.write();
    ownerA = write.upsert(id(NodeKind::Thread, "final-owner-a"));
    ownerB = write.upsert(id(NodeKind::Thread, "final-owner-b"));
    turnA = write.upsert(id(NodeKind::Turn, "final-turn-a"));
    turnB = write.upsert(id(NodeKind::Turn, "final-turn-b"));
    child = write.upsert(id(NodeKind::Thread, "final-child"));
    write.setParent(ownerA, turnA);
    write.setParent(ownerB, turnB);
    NodeState agentState;
    agentState.status = NodeStatus::Running;
    agentState.fields = {{"type", "subAgentActivity"},
                         {"kind", "started"},
                         {"prompt", "original creator"}};
    agent = write.upsert(id(NodeKind::Item, "final-agent"),
                         std::move(agentState));
    NodeState planState;
    planState.fields = {{"type", "plan"}, {"text", "original plan"}};
    plan = write.upsert(id(NodeKind::Item, "final-plan"),
                        std::move(planState));
    write.setParent(turnA, agent);
    write.setParent(turnA, plan);
    write.relate(agent, RelationKind::AgentChildThread, child);
    static_cast<void>(write.finish());
  }

  {
    auto write = graph.write();
    write.setField(agent, "type", "plain");
    write.clearParent(agent);
    write.setField(plan, "type", "plain");
    write.remove(plan);
    static_cast<void>(write.finish());
  }
  auto read = graph.tryRead();
  const InspectorThreadIndex *index = read ? read->inspectorIndex(ownerA)
                                            : nullptr;
  bool passed = expect(
      index && index->agents.empty() && !index->planSource,
      "changing Inspector relevance before detach/removal clears the old owner");
  read.reset();

  NodeRef structured;
  {
    auto write = graph.write();
    NodeState state;
    state.fields = {{"plan", Value::Array{Value("structured step")}}};
    structured = write.upsert(id(NodeKind::Turn, "final-structured-turn"),
                              std::move(state));
    write.setParent(ownerA, structured);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    write.eraseField(structured, "plan");
    write.setParent(ownerB, structured);
    static_cast<void>(write.finish());
  }
  read = graph.tryRead();
  const InspectorThreadIndex *indexA = read ? read->inspectorIndex(ownerA)
                                             : nullptr;
  const InspectorThreadIndex *indexB = read ? read->inspectorIndex(ownerB)
                                             : nullptr;
  passed &= expect(indexA && indexB && !indexA->planSource &&
                       !indexB->planSource,
                   "clearing structured Plan state before a cross-owner move "
                   "matches final graph authority");
  read.reset();

  NodeGraph planMoves;
  NodeRef planOwnerA;
  NodeRef planOwnerB;
  NodeRef olderPlanTurnA;
  NodeRef selectedPlanTurn;
  NodeRef olderPlanA;
  NodeRef selectedPlan;
  NodeRef olderPlanB;
  {
    auto write = planMoves.write();
    planOwnerA = write.upsert(id(NodeKind::Thread, "plan-move-owner-a"));
    planOwnerB = write.upsert(id(NodeKind::Thread, "plan-move-owner-b"));
    olderPlanTurnA =
        write.upsert(id(NodeKind::Turn, "plan-move-older-turn-a"));
    selectedPlanTurn =
        write.upsert(id(NodeKind::Turn, "plan-move-selected-turn"));
    const NodeRef olderTurnB =
        write.upsert(id(NodeKind::Turn, "plan-move-older-turn-b"));
    write.setParent(planOwnerA, olderPlanTurnA);
    write.setParent(planOwnerA, selectedPlanTurn);
    write.setParent(planOwnerB, olderTurnB);
    const auto addPlan = [&](const NodeRef &turn, std::string name,
                             std::string text) {
      NodeState state;
      state.fields = {{"type", "plan"}, {"text", std::move(text)}};
      NodeRef item = write.upsert(id(NodeKind::Item, std::move(name)),
                                  std::move(state));
      write.setParent(turn, item);
      return item;
    };
    olderPlanA =
        addPlan(olderPlanTurnA, "plan-move-older-a", "older A");
    selectedPlan =
        addPlan(selectedPlanTurn, "plan-move-selected", "selected");
    olderPlanB = addPlan(olderTurnB, "plan-move-older-b", "older B");
    static_cast<void>(write.finish());
  }
  read = planMoves.tryRead();
  indexA = read ? read->inspectorIndex(planOwnerA) : nullptr;
  indexB = read ? read->inspectorIndex(planOwnerB) : nullptr;
  passed &= expect(indexA && indexB && indexA->planSource == selectedPlan &&
                       indexB->planSource == olderPlanB,
                   "each owner initially selects its newest legacy Plan");
  read.reset();
  std::uint64_t planMoveRevision = 0;
  {
    auto write = planMoves.write();
    write.setParent(planOwnerB, selectedPlanTurn);
    planMoveRevision = write.finish().revision;
  }
  read = planMoves.tryRead();
  indexA = read ? read->inspectorIndex(planOwnerA) : nullptr;
  indexB = read ? read->inspectorIndex(planOwnerB) : nullptr;
  passed &= expect(
      indexA && indexB && indexA->planSource == olderPlanA &&
          indexB->planSource == selectedPlan &&
          indexA->planChangedRevision == planMoveRevision &&
          indexA->planOrderRevision == planMoveRevision &&
          indexB->planChangedRevision == planMoveRevision &&
          indexB->planOrderRevision == planMoveRevision,
      "moving the selected legacy Plan Turn reveals the old owner's "
      "predecessor and adopts it in the new owner at one exact revision");
  read.reset();
  std::uint64_t planRemovalRevision = 0;
  {
    auto write = planMoves.write();
    write.remove(selectedPlanTurn);
    planRemovalRevision = write.finish().revision;
  }
  read = planMoves.tryRead();
  indexA = read ? read->inspectorIndex(planOwnerA) : nullptr;
  indexB = read ? read->inspectorIndex(planOwnerB) : nullptr;
  passed &= expect(
      indexA && indexB && indexA->planSource == olderPlanA &&
          indexB->planSource == olderPlanB &&
          indexA->planChangedRevision == planMoveRevision &&
          indexA->planOrderRevision == planMoveRevision &&
          indexB->planChangedRevision == planRemovalRevision &&
          indexB->planOrderRevision == planRemovalRevision,
      "removing the adopted Plan Turn reveals only the new owner's "
      "predecessor without invalidating the old owner again");
  const std::uint64_t legacyPlanChangedRevision =
      indexA ? indexA->planChangedRevision : 0;
  const std::uint64_t legacyPlanOrderRevision =
      indexA ? indexA->planOrderRevision : 0;
  read.reset();
  {
    auto write = planMoves.write();
    write.setStatus(planOwnerA, NodeStatus::Failed);
    write.setStatus(olderPlanTurnA, NodeStatus::Completed);
    static_cast<void>(write.finish());
  }
  read = planMoves.tryRead();
  indexA = read ? read->inspectorIndex(planOwnerA) : nullptr;
  passed &= expect(
      indexA && indexA->planSource == olderPlanA &&
          indexA->planChangedRevision == legacyPlanChangedRevision &&
          indexA->planOrderRevision == legacyPlanOrderRevision,
      "Thread and Turn status changes do not invalidate a legacy Plan whose "
      "presentation is status-independent");
  read.reset();

  NodeGraph semantics;
  NodeRef semanticsOwner;
  NodeRef semanticsTurn;
  NodeRef semanticsChild;
  NodeRef creator;
  NodeRef collaboration;
  NodeRef nonSpawn;
  NodeRef ordinary;
  {
    auto write = semantics.write();
    semanticsOwner = write.upsert(id(NodeKind::Thread, "semantic-owner"));
    semanticsTurn = write.upsert(id(NodeKind::Turn, "semantic-turn"));
    semanticsChild = write.upsert(id(NodeKind::Thread, "semantic-child"));
    write.setParent(semanticsOwner, semanticsTurn);
    NodeState creatorState;
    creatorState.status = NodeStatus::Running;
    creatorState.fields = {{"type", "subAgentActivity"},
                           {"kind", "started"},
                           {"prompt", "creator prompt"}};
    creator = write.upsert(id(NodeKind::Item, "semantic-creator"),
                           std::move(creatorState));
    write.setParent(semanticsTurn, creator);
    write.relate(creator, RelationKind::AgentChildThread, semanticsChild);
    NodeState messageState;
    messageState.status = NodeStatus::Completed;
    messageState.fields = {{"type", "collabAgentToolCall"},
                           {"tool", "send_message"},
                           {"prompt", "must not contribute"}};
    nonSpawn = write.upsert(id(NodeKind::Item, "semantic-send"),
                            std::move(messageState));
    write.setParent(semanticsTurn, nonSpawn);
    write.relate(nonSpawn, RelationKind::AgentChildThread, semanticsChild);
    NodeState spawnState;
    spawnState.status = NodeStatus::Completed;
    spawnState.fields = {{"type", "collabAgentToolCall"},
                         {"tool", "spawn_agent"},
                         {"prompt", "temporary spawn prompt"}};
    collaboration = write.upsert(id(NodeKind::Item, "semantic-spawn"),
                                 std::move(spawnState));
    write.setParent(semanticsTurn, collaboration);
    write.relate(collaboration, RelationKind::AgentChildThread,
                 semanticsChild);
    NodeState ordinaryState;
    ordinaryState.fields = {{"type", "commandExecution"}};
    ordinary = write.upsert(id(NodeKind::Item, "semantic-ordinary"),
                            std::move(ordinaryState));
    write.setParent(semanticsTurn, ordinary);
    static_cast<void>(write.finish());
  }
  std::uintptr_t agentStorage = 0;
  std::uint64_t agentChangedRevision = 0;
  std::uint64_t agentOrderRevision = 0;
  {
    auto read = semantics.tryRead();
    const auto *initial = read ? read->inspectorIndex(semanticsOwner) : nullptr;
    if (initial && !initial->agents.empty()) {
      agentStorage =
          reinterpret_cast<std::uintptr_t>(&initial->agents.front());
      agentChangedRevision = initial->agentChangedRevision;
      agentOrderRevision = initial->agentOrderRevision;
    }
  }
  {
    auto read = semantics.tryRead();
    const auto *before = read ? read->inspectorIndex(semanticsOwner) : nullptr;
    const std::uint64_t planRevision =
        before ? before->planChangedRevision : 0;
    const std::uint64_t agentRevision =
        before ? before->agentChangedRevision : 0;
    read.reset();
    {
      auto write = semantics.write();
      write.setField(ordinary, "type", "fileChange");
      static_cast<void>(write.finish());
    }
    read = semantics.tryRead();
    const auto *after = read ? read->inspectorIndex(semanticsOwner) : nullptr;
    passed &= expect(after && after->planChangedRevision == planRevision &&
                         after->agentChangedRevision == agentRevision,
                     "a type change between non-Inspector Item kinds does "
                     "not invalidate either Inspector projection");
  }
  {
    auto read = semantics.tryRead();
    const auto *before = read ? read->inspectorIndex(semanticsOwner) : nullptr;
    const std::uint64_t changedRevision =
        before ? before->agentChangedRevision : 0;
    const std::uint64_t orderRevision =
        before ? before->agentOrderRevision : 0;
    const InspectorAgentSelector selector =
        before && !before->agents.empty() ? before->agents.front()
                                          : InspectorAgentSelector{};
    read.reset();
    {
      auto write = semantics.write();
      write.setField(creator, "unrelated", "ignored");
      static_cast<void>(write.finish());
    }
    read = semantics.tryRead();
    const auto *after = read ? read->inspectorIndex(semanticsOwner) : nullptr;
    passed &= expect(after && after->agentChangedRevision == changedRevision &&
                         after->agentOrderRevision == orderRevision &&
                         !after->agents.empty() &&
                         after->agents.front() == selector,
                     "an unrelated Agent contributor field is an exact "
                     "Inspector semantic no-op");
  }
  {
    auto read = semantics.tryRead();
    const auto *before = read ? read->inspectorIndex(semanticsOwner) : nullptr;
    const std::uint64_t changedRevision =
        before ? before->agentChangedRevision : 0;
    const std::uint64_t orderRevision =
        before ? before->agentOrderRevision : 0;
    const InspectorAgentSelector selector =
        before && !before->agents.empty() ? before->agents.front()
                                          : InspectorAgentSelector{};
    read.reset();
    {
      auto write = semantics.write();
      write.setField(nonSpawn, "prompt", "still not presented");
      static_cast<void>(write.finish());
    }
    read = semantics.tryRead();
    const auto *after = read ? read->inspectorIndex(semanticsOwner) : nullptr;
    passed &= expect(after && after->agentChangedRevision == changedRevision &&
                         after->agentOrderRevision == orderRevision &&
                         !after->agents.empty() &&
                         after->agents.front() == selector,
                     "a non-spawn collaboration contributor invalidates only "
                     "its reported child facts");
  }
  {
    auto write = semantics.write();
    write.setField(semanticsTurn, "plan",
                   Value::Array{Value::Object{{"step", "partial plan"},
                                              {"status", "inProgress"}}});
    static_cast<void>(write.finish());
  }
  read = semantics.tryRead();
  index = read ? read->inspectorIndex(semanticsOwner) : nullptr;
  passed &= expect(
      agentStorage && index && index->planSource == semanticsTurn &&
          index->planRowCount == 1 && !index->agents.empty() &&
          reinterpret_cast<std::uintptr_t>(&index->agents.front()) ==
              agentStorage &&
          index->agentChangedRevision == agentChangedRevision &&
          index->agentOrderRevision == agentOrderRevision,
      "a Plan-only rebuild preserves Agent storage, content, and revisions");
  const std::uint64_t agentRevisionBeforeRawTurnStatus =
      index ? index->agentChangedRevision : 0;
  const std::uint64_t planRevisionBeforeRawStatus =
      index ? index->planChangedRevision : 0;
  const std::uint64_t structuredPlanOrderRevision =
      index ? index->planOrderRevision : 0;
  read.reset();
  {
    auto write = semantics.write();
    write.setField(semanticsOwner, "status", "failed");
    write.setField(semanticsTurn, "status", "completed");
    static_cast<void>(write.finish());
  }
  read = semantics.tryRead();
  index = read ? read->inspectorIndex(semanticsOwner) : nullptr;
  passed &= expect(
      index &&
          index->agentChangedRevision == agentRevisionBeforeRawTurnStatus &&
          index->planChangedRevision == planRevisionBeforeRawStatus &&
          index->planOrderRevision == structuredPlanOrderRevision,
      "raw Thread and Turn status changes do not invalidate projections "
      "whose terminal fold uses typed status");
  read.reset();
  std::uint64_t typedThreadStatusRevision = 0;
  {
    auto write = semantics.write();
    write.setStatus(semanticsOwner, NodeStatus::Failed);
    typedThreadStatusRevision = write.finish().revision;
  }
  read = semantics.tryRead();
  index = read ? read->inspectorIndex(semanticsOwner) : nullptr;
  passed &= expect(index &&
                       index->planChangedRevision ==
                           typedThreadStatusRevision &&
                       index->planOrderRevision == structuredPlanOrderRevision,
                   "typed Thread terminal status invalidates a structured "
                   "Plan with a running step");
  read.reset();
  std::uint64_t typedTurnStatusRevision = 0;
  {
    auto write = semantics.write();
    write.setStatus(semanticsTurn, NodeStatus::Completed);
    typedTurnStatusRevision = write.finish().revision;
  }
  read = semantics.tryRead();
  index = read ? read->inspectorIndex(semanticsOwner) : nullptr;
  passed &= expect(index &&
                       index->planChangedRevision == typedTurnStatusRevision &&
                       index->planOrderRevision == structuredPlanOrderRevision,
                   "typed Turn terminal status invalidates a structured Plan "
                   "with a running step");
  const std::uint64_t planChangedRevision =
      index ? index->planChangedRevision : 0;
  const std::uint64_t planOrderRevision = index ? index->planOrderRevision : 0;
  read.reset();
  {
    auto write = semantics.write();
    write.setField(collaboration, "tool", "send_message");
    static_cast<void>(write.finish());
  }
  read = semantics.tryRead();
  index = read ? read->inspectorIndex(semanticsOwner) : nullptr;
  passed &= expect(
      index && index->agents.size() == 1 &&
          index->planSource == semanticsTurn && index->planRowCount == 1 &&
          index->planChangedRevision == planChangedRevision &&
          index->planOrderRevision == planOrderRevision &&
          candidateSource(index->agents.front(), 4) == creator &&
          candidateSource(index->agents.front(), 8) == creator,
      "an Agent-only rebuild preserves Plan content and revisions while "
      "non-spawn collaboration contributes only reported child facts");
  const InspectorThreadIndex *childIndex =
      read ? read->inspectorIndex(semanticsChild) : nullptr;
  const std::uint64_t childAgentRevision =
      childIndex ? childIndex->agentChangedRevision : 0;
  read.reset();

  NodeRef message;
  std::uint64_t messageRevision = 0;
  {
    auto write = semantics.write();
    const NodeRef childTurn =
        write.upsert(id(NodeKind::Turn, "semantic-child-turn"));
    message = write.upsert(id(NodeKind::Item, "semantic-child-message"));
    write.setParent(semanticsChild, childTurn);
    write.setParent(childTurn, message);
    write.setField(message, "type", "agentMessage");
    write.setField(message, "text", "child result");
    messageRevision = write.finish().revision;
  }
  read = semantics.tryRead();
  index = read ? read->inspectorIndex(semanticsOwner) : nullptr;
  childIndex = read ? read->inspectorIndex(semanticsChild) : nullptr;
  passed &= expect(
      index && childIndex && childIndex->latestAgentMessage() == message &&
          childIndex->agentChangedRevision == childAgentRevision &&
          index->agentChangedRevision == messageRevision,
      "agentMessage append invalidates exact parent rows without changing "
      "the child Thread's empty Agents page");
  read.reset();
  std::uint64_t messageRemovalRevision = 0;
  {
    auto write = semantics.write();
    write.setField(message, "type", "plain");
    messageRemovalRevision = write.finish().revision;
  }
  read = semantics.tryRead();
  index = read ? read->inspectorIndex(semanticsOwner) : nullptr;
  childIndex = read ? read->inspectorIndex(semanticsChild) : nullptr;
  passed &= expect(
      index && childIndex && !childIndex->latestAgentMessage() &&
          childIndex->agentChangedRevision == childAgentRevision &&
          index->agentChangedRevision == messageRemovalRevision,
      "agentMessage type removal invalidates exact parents without changing "
      "the child Thread's empty Agents page");
  read.reset();

  NodeGraph malformed;
  NodeRef malformedOwner;
  NodeRef malformedSource;
  {
    auto write = malformed.write();
    malformedOwner = write.upsert(id(NodeKind::Thread, "malformed-owner"));
    const NodeRef turn = write.upsert(id(NodeKind::Turn, "malformed-turn"));
    const NodeRef invalid = write.upsert(id(NodeKind::Item, "not-a-thread"));
    write.setParent(malformedOwner, turn);
    NodeState state;
    state.fields = {{"type", "subAgentActivity"},
                    {"kind", "started"},
                    {"receiverThreadIds", Value::Array{Value("fallback-a")}}};
    malformedSource = write.upsert(id(NodeKind::Item, "malformed-source"),
                                   std::move(state));
    write.setParent(turn, malformedSource);
    write.relate(malformedSource, RelationKind::AgentChildThread, invalid);
    static_cast<void>(write.finish());
  }
  {
    auto write = malformed.write();
    write.setField(malformedSource, "receiverThreadIds",
                   Value::Array{Value("fallback-b")});
    static_cast<void>(write.finish());
  }
  read = malformed.tryRead();
  index = read ? read->inspectorIndex(malformedOwner) : nullptr;
  passed &= expect(index && index->agents.size() == 1 &&
                       index->agents.front().childId == "fallback-b",
                   "invalid-kind Agent relation cannot suppress fallback "
                   "identity maintenance");

  NodeGraph statusFold;
  NodeRef statusOwner;
  NodeRef statusTurn;
  NodeRef statusChild;
  NodeRef terminal;
  NodeRef unknown;
  NodeRef pending;
  NodeRef running;
  {
    auto write = statusFold.write();
    statusOwner = write.upsert(id(NodeKind::Thread, "status-owner"));
    statusTurn = write.upsert(id(NodeKind::Turn, "status-turn"));
    statusChild = write.upsert(id(NodeKind::Thread, "status-child"));
    write.setParent(statusOwner, statusTurn);
    const auto appendActivity = [&](std::string canonical,
                                    NodeStatus status) {
      NodeState state;
      state.status = status;
      state.fields = {{"type", "subAgentActivity"}, {"kind", "started"}};
      NodeRef source = write.upsert(id(NodeKind::Item, std::move(canonical)),
                                    std::move(state));
      write.setParent(statusTurn, source);
      write.relate(source, RelationKind::AgentChildThread, statusChild);
      return source;
    };
    terminal = appendActivity("status-terminal", NodeStatus::Completed);
    unknown = appendActivity("status-unknown", NodeStatus::Unknown);
    write.setField(unknown, "status", "future-status");
    pending = appendActivity("status-pending", NodeStatus::Pending);
    running = appendActivity("status-running", NodeStatus::Running);
    static_cast<void>(write.finish());
  }
  read = statusFold.tryRead();
  index = read ? read->inspectorIndex(statusOwner) : nullptr;
  passed &= expect(
      index && index->agents.size() == 1 &&
          candidateSource(index->agents.front(), 8) == running &&
          candidateSource(index->agents.front(), 9) == pending,
      "Pending before Running is a lifecycle barrier rather than an old "
      "terminal override");
  read.reset();
  {
    auto write = statusFold.write();
    write.setStatus(pending, NodeStatus::Unknown);
    static_cast<void>(write.finish());
  }
  read = statusFold.tryRead();
  index = read ? read->inspectorIndex(statusOwner) : nullptr;
  passed &= expect(
      index && candidateSource(index->agents.front(), 8) == running &&
          candidateSource(index->agents.front(), 9) == unknown,
      "an authored unknown status before Running also prevents a stale "
      "terminal override");
  read.reset();
  {
    auto write = statusFold.write();
    write.eraseField(unknown, "status");
    static_cast<void>(write.finish());
  }
  read = statusFold.tryRead();
  index = read ? read->inspectorIndex(statusOwner) : nullptr;
  passed &= expect(
      index && candidateSource(index->agents.front(), 8) == running &&
          candidateSource(index->agents.front(), 9) == terminal,
      "a terminal status immediately followed by Running remains terminal");
  read.reset();
  {
    auto write = statusFold.write();
    write.setStatus(terminal, NodeStatus::Unknown);
    write.setStatus(statusTurn, NodeStatus::Completed);
    static_cast<void>(write.finish());
  }
  read = statusFold.tryRead();
  index = read ? read->inspectorIndex(statusOwner) : nullptr;
  passed &= expect(
      index && candidateSource(index->agents.front(), 8) == running &&
          candidateSource(index->agents.front(), 9) == running,
      "a Running Agent in a terminal Turn is deterministically NotLoaded");
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
  passed &= testOrderedChildIndexTracksEveryTopologyMutation();
  passed &= testRemovalLifetimeAndRetirement();
  passed &= testRetirementSlotsAndExactIncarnations();
  passed &= testIncomingRelationIndexTracksEveryMutation();
  passed &= testMisuseRejection();
  passed &= testBatchRemovalIsAtomicAndApproximatelyLinear();
  passed &= testInspectorAgentTailAppendIsIncremental();
  passed &= testInspectorIndexWorkIsBoundedByChangedDependencies();
  passed &= testInspectorStructuralImpactsStayExact();
  passed &= testInspectorIncrementalAuthorityMatchesFinalGraph();
  return passed ? 0 : 1;
}
