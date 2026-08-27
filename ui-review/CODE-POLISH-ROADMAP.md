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
| `T` | Threads |
| `C` | Visible conversation cards |
| `P` | Retained prompt submissions |
| `I` | Authoritative conversation items |
| `A` | Agents |
| `R` | Repositories |
| `F` | Changed files or watched paths |
| `D` | Candidate directories |
| `Q` | Pending requests |

## High priority

### Centralize thread and turn status classification

**Effort: Medium**

Define active, completed, failed and idle once, then use that definition for
thread dots, composer controls, conversation cards and the Inspector. This
prevents contradictory UI such as a completed gray thread still presenting
Steer and Stop.

### Build an authoritative-item index

**Effort: Medium**

Index conversation items by stable key, client ID and position once per
projection. Prompt matching and anchor lookup can then use direct access
instead of repeatedly scanning the complete history.

### Make prompt reconciliation linear

**Effort: Medium–High · Target: `O(P × I)` → approximately `O(P + I)`**

Every unresolved prompt currently searches authoritative history. Reuse the
projection index for exact identity, anchor and fallback matching while
preserving acknowledgement semantics and claimed-item ownership.

### Compact fully resolved prompt submissions

**Effort: Medium**

Resolved submissions currently remain in future reconciliation work. Remove
them after their transition completes, or retain only the compact mapping
needed to preserve stable visual identity across reconstruction and navigation.

### Remove repeated Qt layout searches

**Effort: High · Target: `O(C²)` → approximately `O(C)` for stable order**

`ConversationView` calls the linear `QLayout::indexOf()` operation for each
section and card during reconciliation. Retain known positions and avoid asking
the layout to rediscover an order CodexUI already owns.

### Retain explicit section and card order

**Effort: High**

Compare desired order against retained order vectors and move widgets only at
changed positions. Content-only streaming updates should not traverse and
rearrange the entire layout; scroll anchors and command-output state must remain
stable.

### Split shell integration scenarios

**Effort: Medium**

Separate start, steer, completion, hydration, recovery and navigation into
clearly named scenario functions. Smaller scenarios reduce accidental coupling
and make lifecycle failures attributable to one protocol sequence.

### Provide canonical protocol fixtures

**Effort: Low–Medium**

Fixture builders must guarantee unique turn IDs and valid combinations of
thread and turn status. This prevents impossible mock states from hiding real
defects or rejecting correct invariants.

## Medium priority

### Consolidate per-thread runtime bookkeeping

**Effort: Medium–High**

Hydration, settings hydration, read revision, resume, dispatch and recovery are
currently represented by parallel maps and sets. Store them in one small
`ThreadRuntimeState` per thread to reduce synchronization mistakes and repeated
cleanup code without introducing a new subsystem.

### Replace serialized JSON UI snapshots

**Effort: Medium**

Several render paths build and serialize JSON solely to detect visual changes.
Use small typed snapshot structures with equality instead; this removes
allocation, parsing-shaped code and untyped comparison logic.

### Rebuild thread ordering in one pass

**Effort: Low · Target: `O(T²)` → `O(T)`**

`mergeThreadList()` repeatedly erases IDs from a vector. Use one membership set
and construct the resulting order once while preserving provider order and the
required retained tail.

### Precompute thread-panel positions

**Effort: Medium · Target: `O(T²)` → approximately `O(T)`**

Repeated `QListWidget::row()` calls linearly rediscover current positions.
Retain or calculate row indices once per refresh, then perform only the moves
required by the desired order.

### Aggregate request counts once

**Effort: Low · Target: `O(T × Q)` → `O(T + Q)`**

The thread panel repeatedly scans pending requests for individual threads.
Build one per-thread count map and reuse it for serialization and row updates.

### Consolidate presentation helpers

**Effort: Low–Medium**

Status formatting, JSON string extraction and related classification are
repeated across source files. Move only genuinely shared semantics into the
existing presentation or UI support code.

### Consolidate repeated styling

**Effort: Low–Medium**

Move repeated canonical colors, borders and semantic states into `UiStyle`.
Keep widget-specific geometry and genuinely exceptional presentation local to
the owning widget.

## Thread-hierarchy follow-up

These tasks belong with the planned structural parent/child thread work rather
than the current flat thread-panel polish.

### Add a child-thread ownership index

**Effort: Medium**

Map each child thread directly to its owning agent and parent thread. Model
updates, removals and hydration must maintain this relationship consistently.

### Use indexed agent correlation

**Effort: Low–Medium · Target: repeated global agent scans → direct lookup**

Child status and result updates currently scan child history and agents across
threads, potentially approaching `O(A²)` across many updates. Once the ownership
index exists, update the owning presentation directly.

### Present structural thread hierarchy

**Effort: High**

Render child threads beneath their parent and support expansion, arbitrary
depth and navigation in both directions. This requires recursive presentation
state and dedicated hierarchy tests.

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

**Effort: Medium–High**

Exercise long histories, many visible cards, large thread lists and pending
requests. Prefer deterministic operation-count or benchmark evidence over
fragile wall-clock assertions where possible.

### Verify every refactoring step

**Effort: Low per change**

Run focused tests for the affected invariant and the complete test suite after
each step. Investigate every failure rather than classifying it as unrelated or
flaky without evidence.

## Recommended sequence

1. Centralize status semantics and canonical fixtures.
2. Rebuild thread ordering and aggregate pending-request counts.
3. Index authoritative conversation items and simplify prompt reconciliation.
4. Compact resolved submissions.
5. Retain conversation order and remove quadratic Qt layout searches.
6. Consolidate per-thread runtime state and typed UI snapshots.
7. Address agent correlation with the future structural hierarchy.
8. Apply profiling-led Git, filesystem and Inspector improvements.

This sequence starts with narrow correctness and low-risk algorithmic wins,
then approaches the scroll- and lifecycle-sensitive conversation work with
stronger fixtures and measurements already in place.

## Delivery strategy

Do not deliver the complete roadmap in one pull request. Use several scoped
pull requests, each containing multiple independently reviewable commits:

1. Establish a reliable test baseline, provide canonical protocol fixtures,
   split the shell integration scenarios and centralize status classification.
2. Build the authoritative-item index, make prompt reconciliation linear and
   compact fully resolved submissions.
3. Optimize thread-list reconciliation and conversation layout ordering.
4. Consolidate `ThreadRuntimeState` and replace serialized UI fingerprints with
   local typed snapshots.
5. Add the child-thread ownership index, indexed agent correlation and the
   structural thread hierarchy.
6. Apply only measurement-supported repository, filesystem-watch, Inspector,
   settings-catalog, helper and styling cleanup.

Every commit must compile and pass the focused tests for its changed invariant.
Every pull request must pass the complete test suite before merge. Keep commits
small enough to review and bisect without depending on a later cleanup commit.
