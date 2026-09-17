# Qt virtualized conversation view

## Purpose and status

This document records the current native conversation-rendering contract. It
replaces the historical description of a passive delegate plus promoted card
widgets. That two-renderer design is superseded and is not a migration path.

The S1 renderer remediation is implemented and qualified under the local
deterministic offscreen Debug, Release, and ASan/UBSan matrices. Native display
plugins and a real assistive-technology bridge remain explicit cross-platform
qualification work; they are not represented by the offscreen harness.

## Governing invariant

A logical conversation row has one authoritative renderer:
`middle::ConversationCard`.

- Every row intersecting the viewport or bounded overscan is represented by a
  real `ConversationCard`.
- The same object owns appearance, geometry, Copy and disclosure controls,
  Markdown selection and links, command-output interaction, focus, and child
  accessibility.
- Interaction never promotes a painted substitute to a different visual
  object and never requires synthetic mouse delivery.
- Rows outside residency retain model data, stable-keyed interaction state
  where required, and at most one cached scalar height. They retain no card,
  placeholder, Markdown document, or parallel painted presentation.
- The view may paint cross-row Turn decoration. It may not paint a second card
  body, header, control, status, or hit target.

Virtualization therefore controls residency and work, not presentation.

## Ownership

`NodeGraph` remains authoritative protocol/application state. A
`NodeGraphUiAdapter` read projects complete value data after releasing graph
locks. `ConversationItemModel` then owns Qt row identity and ordering, but does
not become protocol authority.

`ConversationView` owns:

- the item model and variable-height index;
- per-thread follow/pause and anchor state;
- bounded `ConversationCard` residency;
- stable-keyed local fold, selection, and command-output state;
- cached scalar card heights;
- history-window and atomic structural-staging policy; and
- cross-row Turn decoration.

`ConversationCard` owns the complete presentation and interaction semantics of
one resident row. A stable `NodeRef` remains an opaque action target; Qt code
does not dereference it.

## Model and geometry contract

`ConversationItemModel::Row` stores the last projected `VisibleCardData`, its
stable key and action target, and structural Turn facts needed by the view.
Identical effective values emit no model signal. Same-thread changes use
precise insert, remove, move, or row-data operations; a different authoritative
thread is the reset boundary.

`ConversationHeightIndex` is the sole row-position authority. It stores one
nonnegative extent per model row in an order-statistic tree. Prefix positions,
position-to-row lookup, single-row height changes, and traversal to the next or
previous nonzero row use logarithmic tree paths. Zero-height rows remain in the
model when presentation options or a collapsed Turn hide them, but viewport
work skips long zero-height runs without scanning every hidden row.

For a resident card, the card's settled height at the canonical row width is
exact. For a nonresident card, the index may use a stable-keyed exact scalar
height from the same content/width/environment or a conservative estimate.
Content or width changes retire an invalid scalar before it can be reused.

A scroll anchor is a stable row key plus exact pixel offset. Height changes,
history insertion, and residency changes restore that anchor unless the view
was already following the tail.

## Residency and lifecycle

`ConversationView::materializationRows()` selects the viewport plus bounded
overscan. Materialization creates the sole card implementation under a hidden
staging host, settles it at its canonical width, reparents it to the viewport,
and exposes it at the exact indexed geometry. There is no widget pool and no
renderer switch.

Rows intersecting the viewport are admitted synchronously so scrolling never
reveals an unrendered row. Missing hidden overscan is admitted nearest-first,
at most one card per callback, through the existing card-admission scheduler.
Structural staging has priority. Each callback derives its candidate from the
current model, height index, and viewport rather than retaining a row queue, so
scrolling, resizing, thread replacement, and row removal cannot stale pending
work. A settled viewport therefore owns the complete one-viewport overscan,
while input-critical work need construct only newly visible rows.

When a card leaves residency, the view captures its stable semantic state and
deletes the widget. A later resident instance is constructed from current
model data, configured for its final nested/Turn context, and then receives
compatible retained state. Removal, trimming, incompatible kind replacement,
or forgotten thread identity retires both scalar geometry and interaction
state.

Collapsed heavy cards defer hidden body construction. Expansion builds the
body once from the latest authoritative row data. The collapsed header remains
the same renderer and object; deferral does not introduce a header-only
delegate.

A focused card may be pinned outside ordinary overscan until focus moves. This
is a residency exception for interaction continuity, not a second state or
geometry authority. Once focus moves, the card leaves the resident map and
viewport/accessibility hierarchy synchronously. Destruction of that one former
focus receiver is deferred because deleting a QWidget from inside its active
event dispatch would invalidate the receiver's stack; ordinary residency
releases remain immediate so physical widget/document counts cannot accumulate.

## Structural operations

Thread replacement and history-page insertion can be staged behind the old
stable surface or loading overlay. Only rows needed for the first complete
frame are constructed, one nonzero-delay pass at a time. Commit applies the
authoritative model operation, exact height/index state, anchor or follow
position, and resident layout before revealing the result.

Targeted card changes and validated tail appends bypass whole-snapshot staging.
Optimistic `LocalPrompt` to authoritative `UserMessage` is the single allowed
in-place semantic kind transition. It keeps stable visual identity and reports
the exact prompt acknowledgement after the view transaction has committed.

## Interaction-state contract

Persistent local state is addressed by thread ID, stable card key, and semantic
control role. It is never addressed by child ordinal or incidental QObject
topology.

- Fold state remains view-owned and survives residency changes.
- A selection survives an append-only update of the same semantic text owner.
  It is cleared or clamped when that owner disappears, changes meaning, or is
  replaced by shorter content.
- Command-output follow/pause state survives append-only output while the
  output surface exists. Removing or replacing that output retires the inner
  state and releases any outer pause owned by that command.
- A semantic no-op preserves pixels, row extents, card/document/control
  identity, focus, animation state, and accessibility objects.

The same normalization rule must run for resident cards, staged cards, direct
targeted updates, and offscreen retained state. Materializing an offscreen card
solely to normalize state is forbidden.

## Performance qualification

The benchmark is registered as twelve serial CTests: 320, 1,280, and 10,000
rows at requested DPR 1.0, 1.25, 1.5, and 2.0. It exits unsuccessfully when a
quantitative requirement fails. Its current gates include:

| Dimension | Gate |
|---|---|
| resident cards | peak between 1 and 64 |
| documents/widgets | documents `<= max(2, cards * 2)`; widgets `<= cards * 24 + 16` |
| initial staging | model publication remains deferred; 1–64 stage passes; zero synchronous fallback constructions |
| residency | every settled viewport/overscan row is resident, with only bounded edge tolerance and one possible focus pin |
| construction | initial creations 1–64; observed resident-card construction p50/p95/max `<= 5/10/25 ms` (all three limits multiplied by four under ASan/UBSan) |
| progressive wheel input | 120 real forwarded wheel events reach at least 80 distinct values; synchronous p95/max `<= 2/5 ms` (multiplied by four under ASan/UBSan), with immediate and post-admission viewport coverage, zero synchronous construction, at most one synchronous scan, no more than three one-card admission passes between samples, and eventual full overscan |
| overlapping scroll | median/p95/max `<= 15/30/50 ms`; at most eight constructions/releases, four full residency scans, and four one-card admission passes per sample |
| large seek | median/p95/max `<= 70/85/120 ms`; at most 32 constructions/releases, eight full residency scans, four one-card admission passes, and eight combined passes per sample |
| streaming | median/p95/max `<= 10/30/50 ms`; no card reconstruction; every one of 24 Markdown updates begins at the mutable tail; bounded document-change/layout-request/paint counts |
| append | paused append `<= 30 ms`; following median/p95/max `<= 15/30/50 ms`; bounded construction/layout/paint work |
| resize | frame median/p95/max `<= 35/50/75 ms`, one exact settlement `<= 100 ms`, and no index rebuild |
| hidden-row traversal | 20,000 hidden rows remain within the absolute and relative sparse-work limits |
| height update | at most `4 * bit_width(row_count + 1)` tree steps |
| Qt event dispatch | individual layout-event p50/p95/max `<= 1/5/30 ms`; individual paint-event p50/p95/max `<= 5/15/50 ms` |
| memory | normal Linux RSS `<= 192 MiB` and growth `<= 64 MiB`; ASan allocator/quarantine allowance `<= 1 GiB` and growth `<= 768 MiB`; DPR-scaled verification buffers are accounted separately |

The benchmark also checks that all card kinds appear through real resident
cards, stable keys match model rows, visible cards exactly match `visualRect`,
rendered text and pixel checksums are nonempty, and an identical update changes
no pixels, objects, focus, documents, or measured work.

The layout- and paint-event timings are durations of individual Qt event
dispatches, not whole-frame CPU measurements. Likewise, the Markdown tail
check proves the observed `QTextDocument` mutation begins at the mutable tail
and leaves the sampled prefix block unchanged; it does not claim direct parser
CPU instrumentation. Linux memory gates sample current resident pages from
`/proc/self/statm`; the inherited process high-water value from `getrusage` is
reported separately for diagnosis and is not used as the gate.

The progressive-wheel timing covers only synchronous forwarding and viewport
materialization; asynchronous admission is measured separately outside that
timed interval. Full viewport scans and one-card admission callbacks have
separate counters because an admission callback no longer performs a viewport
scan. The pre-remediation four-scan normal cap remains on full scans;
admission is independently capped at four callbacks and one construction per
callback. Their per-sample sum is reported and cannot exceed constructions
plus one, but is not mislabeled as the historical full-scan metric. Total
construction and release caps tighten from 24 to eight. The wheel gate limits
the input-critical path to one full scan and zero construction. This is a
change in scheduling semantics, not permission for additional synchronous
work.

These limits preserve the smoothness purpose of lazy materialization. Deleting
the passive renderer does not authorize history-sized widget or document
ownership.

## Current verification record

At the accepted S1 worktree checkpoint:

- the passive card renderer, passive Markdown cache, document-transfer bridge,
  passive Copy/disclosure hit testing, synthetic mouse forwarding, and their
  parity-only tests have been deleted;
- the sole renderer retains viewport/overscan lazy residency;
- logarithmic nonzero-row traversal is covered with a 10,000-row height-index
  case and a 20-versus-20,000 hidden-row benchmark comparison;
- resident, staged, and offscreen interaction state use one semantic
  normalization rule with bounded source fingerprints, including independent
  multi-command detach ownership and command-output owner retirement;
- `MiddleRegionWidget` owns phased wheel routing across conversation,
  command/text bodies, composer editor, attachments, and pointer crossing;
  forwarded events preserve device, source, phase, and timestamp;
- runtime font, style, and DPR changes invalidate scalar and renderer-local
  geometry without replacing cards, documents, focus, selection, or anchors;
- accessibility projects one Pane→List→resident ListItem/control tree and no
  virtual table/cell or hidden staging tree;
- full Debug and Release runs each pass 31/31 tests, including all twelve
  quantitative profiles; and
- the ASan/UBSan run passes 31/31 with no sanitizer diagnostic.

The deterministic offscreen matrix exercises requested DPR 1.0, 1.25, 1.5,
and 2.0. Native XCB/Wayland interaction and real AT-SPI behavior remain
explicit S8 qualification limitations rather than inferred successes.

## Prohibited regressions

Do not reintroduce a delegate-painted card, passive Markdown document cache,
document adoption/transfer, synthetic input forwarding, placeholder widget,
parallel geometry calculation, renderer parity test, or interaction-triggered
renderer replacement. Any future optimization must improve residency, reuse,
caching, or scheduling while preserving the one-renderer contract.
