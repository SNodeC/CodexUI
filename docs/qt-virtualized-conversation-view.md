# Qt virtualized conversation view

## Scope

This document records the focused migration of the native conversation surface
from one retained QWidget tree per loaded card to a Qt item model and
viewport-proportional item view. It does not redesign `NodeGraph`, SNode.C,
CodexBridge, the typed mailboxes, Inspector, ThreadPane, shell chrome, settings,
or the visual language.

The authoritative product and ownership contracts remain
`two-thread-shared-node-graph.md`, `ui-ux-internal-api.md`, `ui-behavior.md`, and
`native-ui-ux-qualification-inventory.md`. Where those documents describe the
old retained-card implementation, this migration preserves the stated visible
result while replacing its loaded-history-sized QWidget and layout work.

## Verified starting point

- branch point: `f22f652e687631568929a622fc1058df09280839`
- local implementation branch: `codex/qt-virtualized-conversation-view`
- initial worktree: clean
- persistent incremental build directory:
  `/home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug`
- build: Debug, Ninja, Qt 6 Widgets
- native baseline: 17/17 suites pass. Sixteen pass in the managed sandbox; the
  listener-dependent `codexui-client-runtime-dispatch` suite passes when run
  outside it so its private Unix listener can be created.

No remote operation is part of this work.

## User-visible defect and diagnosed cause

The existing surface bounds the default history window, targets ordinary card
updates well, and stages new cards invisibly. It nevertheless creates and
retains one `ConversationCard` subtree and one `TurnSectionWidget` for every
loaded row. Its structural fallback and width reflow traverse complete maps of
those widgets and their nested layouts. Loading more history therefore grows
QObject count, memory, construction time, layout work, focus/accessibility
surface, and worst-case event-loop latency with the loaded thread rather than
with the viewport.

From the user's point of view this is the remaining source of delayed thread
selection, paging pauses, and intermittent scrolling/streaming contention in a
long heterogeneous conversation. Cached tail append paths reduce common-case
work, but do not change the history-sized ownership structure.

## Reproducible baseline

`codexui-conversation-view-benchmark` is a non-gating instrumentation target.
It drives the production view with heterogeneous card data, waits for its
atomic staging transaction, sweeps 240 scroll positions, and reports live
card, section, QWidget, geometry, time, and peak-resident-memory counters.

Xcb measurements were taken under Xvfb at a 900 x 700 view size using the same
Debug binary and build directory:

| Loaded rows | Initial reveal | Conversation cards | Turn sections | Descendant QWidgets | Peak resident memory | 240-position sweep |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 320 | 733 ms | 320 | 320 | 4,209 | 125,324 KiB | 329.7 ms |
| 1,280 | 4,040 ms | 1,280 | 1,280 | 16,809 | 281,616 KiB | 325.2 ms |

The approximately 13.1 descendant widgets per row and one section per row are
the decisive baseline. A 10,000-row run is intentionally deferred until the
virtualized implementation exists; allocating the extrapolated old hierarchy
would add no architectural information and risks unnecessary memory pressure.

The offscreen platform independently records 999/3,969/15,849 descendant
widgets for 80/320/1,280 rows and initial reveals of 148/633/3,529 ms. Platform
differences change constants, not the linear ownership result.

## Required implementation shape

The code follows the existing problem boundaries directly:

1. `NodeGraph` remains the sole current domain state and the worker remains its
   sole writer.
2. `NodeGraphUiAdapter` continues to make short nonblocking reads and returns
   complete toolkit-neutral card values only after releasing the graph guard.
3. A thin Qt list model holds stable row indexing and last-rendered values. It
   is not application authority, never interprets graph revisions as changes,
   and never retains protocol payloads.
4. Structural snapshots are flattened into canonical card order with explicit
   Turn/root/nested metadata. Stable `NodeRef` targets are carried unchanged.
5. The model emits the narrowest valid Qt signal for the actual ordered
   difference. An exact targeted card update resolves directly to one row.
6. A variable-height index provides bounded prefix, row lookup, and height
   update operations. Viewport anchoring is expressed as stable row identity
   plus an exact pixel offset.
7. The item view owns only visible presentation plus small bounded overscan.
   Passive presentation uses a delegate where established interaction permits;
   real card widgets/editors exist only for visible rich interaction.
8. Fold, focus, selection, command inner-scroll, delayed prompt feedback, and
   other genuinely local interaction state are keyed by stable row identity and
   survive materialization changes.
9. Selection and Load 80 prepare the new model/geometry and initial visible
   materialization behind the existing stable surface, then reveal one complete
   frame. Ordinary streaming is coalesced within one GUI frame and affects only
   the addressed row.

No snapshot authority, event journal, projector, callback registry, generic
observer, message bus, third logic thread, or alternate transport is introduced.

## Item-model and height-index contract

`middle::ConversationItemModel` is the thin Qt indexing surface. A row contains
one last-rendered `VisibleCardData`, its unchanged `NodeRef` action target, and
only the structural facts the view cannot infer safely: stable key, Turn
section, root/nested position, first/last position, presentation visibility,
and active-Turn emphasis. It does not accept graph revisions, protocol values,
or mutations. A graph read and DTO projection always precede a model call.

The model deliberately does not expose `VisibleCardData` through `QVariant`.
Delegates and the view borrow it through the typed `card(row)` accessor on
Qt-main, avoiding another copy of large streamed text. Standard roles expose
only small identity, structure, visibility, title, and accessibility values.
`indexForTarget(NodeRef)` compares the pinned pointer identity as well as the
lookup result, so an action cannot silently retarget a replacement node with a
similar protocol ID.

Model changes have these exact meanings:

- a different `threadId` is a complete authority replacement and emits one
  model reset;
- a retained same-thread key is moved with `beginMoveRows/endMoveRows` only
  when its canonical position actually changes;
- absent/present same-thread keys use contiguous remove/insert ranges;
- a changed retained card or structural role emits `dataChanged` for that row
  and the affected roles only;
- an identical snapshot, card, or visibility tuple emits no signal and does
  not increment a presentation-work counter.

`middle::ConversationHeightIndex` is the view's variable-row geometry index.
It stores integer row extents in a Fenwick prefix tree. `top`, `bottom`, total
extent, position-to-row lookup, and a changed row height are logarithmic. A
tail append extends the tree from prefix sums without traversing existing
heights. Non-tail insertion/removal/movement is uncommon structural work and
rebuilds the prefix tree from the already validated model order. Geometry
values are nonnegative and accumulated as `qint64`; scrollbar conversion is a
separate view concern.

The deterministic foundation test exercises 10,000 rows and asserts no Qt
widget construction is involved. At that size, position lookup and one-row
height update each take at most 15 Fenwick steps, and appending rows leaves the
rebuild counter unchanged.

## Qualification counters

The final implementation reports at least these inspectable values on the
conversation view so deterministic tests and full-application recordings can
correlate visible behavior with work:

- model insert/remove/move/data-change/reset counts;
- height-index lookup and update counts;
- materialized row/editor count and peak count;
- row construction, release, layout, and paint counts;
- targeted visible and offscreen update counts;
- structural stage starts, commits, and maximum pass duration;
- complete-view geometry/repaint fallbacks, which must remain zero during
  ordinary scrolling and streaming.

Before/after values, interaction ownership, delegate/editor decisions,
sanitizer results, and movie artifacts will be appended as the migration is
qualified.
