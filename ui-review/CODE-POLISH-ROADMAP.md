# CodexUI code-polish roadmap

This roadmap collects the identified code-simplification and performance work
for CodexUI. Its objective is a smaller, clearer application with predictable
runtime behavior—not a larger framework around the UI.

## Rating conventions

Effort describes implementation and verification together:

| Rating | Meaning |
|---|---|
| Low | Narrow, local change with a limited regression surface |
| Medium | Several connected files and focused new tests |
| High | Lifecycle- or layout-sensitive work requiring broad regression testing |
| Ongoing | A review constraint applied to every relevant change |

Complexity notation used below:

| Symbol | Meaning |
|---|---|
| `C` | Visible conversation cards |
| `P` | Retained prompt submissions |
| `I` | Authoritative conversation items |
| `R` | Repositories |
| `F` | Changed files or watched paths |
| `D` | Candidate directories |

## Cutover status

The shared-node-graph cutover completed the architecture-dependent items from
the earlier roadmap: one canonical current graph now owns protocol state;
statuses share one native classification; resolved local prompts retire after
materialization; pending-request counts and child ownership are graph fields
and direct relations; the thread hierarchy renders recursively; and thread,
conversation, and Inspector scans are bounded across event-loop passes. The
legacy presentation model, serialized socketpair path, and its proposed cleanup
work no longer exist.

The remaining opportunities below are local widget or filesystem polish. They
must not introduce another model, projection layer, callback framework, or
execution thread.

## High priority

### Add direct client-message correlation lookup

**Effort: Medium–High · Target: `O(P × I)` → approximately `O(P + I)`**

Canonical thread, turn, item, relation, and order lookup is already graph
native. Conversation reconciliation may still compare unresolved local prompts
with authoritative items. A concrete secondary `clientUserMessageId` lookup
could remove that repeated scan while preserving acknowledgement and stable-card
identity.

### Remove repeated Qt layout searches

**Effort: High · Target: `O(C²)` → approximately `O(C)` for stable order**

`ConversationView` still uses linear `QLayout::indexOf()` checks while arranging
materialized sections and cards. Retaining positions for the bounded visible
window would avoid rediscovering order without changing graph authority,
placeholders, anchors, or focus ownership.

### Keep protocol fixtures canonical

**Effort: Ongoing**

Fixture builders must use unique scoped turn/item IDs and valid combinations
of thread and turn status. Protocol updates must reconcile the exact closed
157/11/83/1 method inventory rather than relying only on generated counts.

## Medium priority

### Precompute materialized thread-row positions

**Effort: Medium**

The graph topology and sorting passes are already bounded. If profiling shows
that `QListWidget` row lookup dominates large visible lists, retain positions
only for the materialized window and perform the minimum required moves.

### Consolidate repeated UI helpers

**Effort: Low–Medium**

Move only genuinely shared status formatting, value extraction, colors, and
borders into the existing narrow `UiStatus`/`UiStyle` helpers. Widget-specific
geometry remains with the owning widget.

## Lower priority and profiling

### Profile repository resolution

**Effort: Low · Current bound: `O(R × I × D)`**

Repository selection combines roots, changed paths and candidate directories,
including filesystem and libgit2 work. It already runs asynchronously; measure
representative multi-repository workloads before changing the algorithm.

### Use sets for repository/path membership

**Effort: Low–Medium**

Replace repeated list membership and deduplication searches where profiling
shows value. Path normalization must remain identical before values enter the
sets.

### Use sets for filesystem-watch reconciliation

**Effort: Low · Target: `O(F²)` → approximately `O(F)`**

Compare desired and existing watch paths with `QSet<QString>` differences
instead of nested `QStringList::contains()` calls. Preserve handling of deleted
files and watched parent directories.

### Profile Inspector reconstruction

**Effort: Low for measurement**

Some tabs destroy and recreate child widgets when their snapshots change.
Expected lists are currently small, so incremental reconciliation should be
introduced only if measurement shows visible cost.

### Profile settings-catalog construction

**Effort: Low for measurement**

Combo-box population performs repeated linear option lookup and can be
quadratic in catalog size. Provider catalogs are normally small, making this a
measurement-led optimization.

### Retain bounded repository hints

**Effort: None beyond optional documentation or tests**

Command directories and changed paths use linear uniqueness checks but are
explicitly capped at 64 and 512 entries. These bounds keep the operation from
growing indefinitely and are currently appropriate.

## Guardrails

### Prefer removal over abstraction

**Effort: Ongoing**

Each refactoring should remove duplicated state, repeated searches or repeated
interpretation. A lower line count is valuable only when responsibilities and
invariants also become clearer.

### Avoid speculative architecture

**Effort: Ongoing**

Do not introduce generic controllers, repositories, event buses or framework
layers without a demonstrated CodexUI responsibility. Typed data and direct
algorithms cover the identified problems.

### Add representative performance tests

**Effort: Ongoing**

Exercise long histories, many visible cards, large thread lists and pending
requests, extending the existing bounded-scan, mailbox-saturation, and Qt
heartbeat coverage. Prefer deterministic operation-count or benchmark evidence
over fragile wall-clock assertions where possible.

### Verify every refactoring step

**Effort: Low per change**

Run focused tests for the affected invariant and the complete test suite after
each step. Investigate every failure rather than classifying it as unrelated or
flaky without evidence.
