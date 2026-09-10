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
6. A canonical single-item tail delta is projected by `tailCard` under one
   short graph read, appended with one Qt insert signal, and trims the bounded
   history prefix without scanning or reindexing the retained suffix.
7. A variable-height index provides bounded prefix, row lookup, and height
   update operations. Viewport anchoring is expressed as stable row identity
   plus an exact pixel offset.
8. The item view owns only visible presentation plus small bounded overscan.
   Passive presentation uses a delegate where established interaction permits;
   real card widgets/editors exist only for visible rich interaction.
9. Fold, focus, selection, command inner-scroll, delayed prompt feedback, and
   other genuinely local interaction state are keyed by stable row identity and
   survive materialization changes.
10. Selection and Load 80 prepare the new model/geometry and initial visible
   materialization behind the existing stable surface, then reveal one complete
   frame. Ordinary streaming identities are coalesced for one GUI frame, then
   projected from latest NodeGraph state at no more than eight distinct rows
   per 16 ms presentation pass. Remaining identities retain their order for the
   next nonzero-delay pass; an empty queue schedules no further work.

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
- the established direct full-snapshot API computes the same precise
  insert/remove/move/data differences but is not used by Shell graph routing;
- a validated canonical tail uses one `beginInsertRows/endInsertRows`; stable
  lookup tables point to nodes in a conversation-specific order-statistic row
  tree, so dropping a prefix or changing the middle does not reindex surviving
  rows;
- an identical snapshot, card, or visibility tuple emits no signal and does
  not increment a presentation-work counter.

`middle::ConversationHeightIndex` is the view's variable-row geometry index.
It stores integer row extents in a dedicated order-statistic tree whose nodes
carry subtree counts and `qint64` extent sums. `top`, `bottom`, total extent,
position-to-row lookup, a changed height, and tail or middle
insertion/removal/movement are logarithmic tree-path operations. Only `assign`
for a complete row sequence increments the rebuild counter. Geometry values
are nonnegative; scrollbar conversion is a separate view concern.

The deterministic foundation test exercises 10,000 rows and asserts no Qt
widget construction is involved. At that size, lookup and one-row height
updates remain on logarithmic paths; exact middle insertion, movement, and
removal leave the model identity, stable-key section, and height rebuild
counters unchanged.

## Item-view and passive-delegate contract

`ConversationView` is a narrowly specialized `QAbstractItemView`. The model
owns row identity and order; an extent order-statistic tree maps positions to
variable-height rows; the view materializes only rows whose behavior currently
requires a real control. It does not create placeholder widgets. Cached height
records contain only stable key, width, and measured height and therefore
cannot become presentation authority.

Collapsed cards and text-only resting cards are painted by the item delegate.
Its Markdown document cache is bounded to 128 visible/recent blocks. Hover,
keyboard current-row movement, or a direct press promotes exactly that row to
the established `ConversationCard`, so selection, copying, links, tooltips,
focus, and controls continue to use their existing implementations. Local
pending prompts and expanded command, file, and image surfaces remain real
widgets because their animation, nested scrolling, file actions, and image
controls are intrinsically interactive. Scrolling a promoted editor out of the
bounded overscan stores only its fold and inner-command-scroll state before the
widget is released.

A canonical Turn is still flat in the model, but not visually flattened. The
view paints the continuous outer You surface from the root row through the last
presented nested row. Root content and nested cards remain independently
virtualizable fragments; nested cards keep the established 12-pixel inset,
and the outer padding, section gap, and active-Turn border are part of indexed
row geometry. This preserves the visual ownership relationship without making
one potentially unbounded Turn a single QWidget.

The first delegate measurement in the persistent Debug/Xvfb configuration is:

| Loaded rows | Initial reveal | Retained conversation widgets | Descendant QWidgets | Peak resident memory | 240-position sweep |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 320 | 9 ms | 0 | 8 | 85,488 KiB | 250.2 ms |
| 1,280 | 16 ms | 0 | 8 | 86,332 KiB | 270.1 ms |

This benchmark's rows are all in passive resting states. The 6.4% sweep-time
increase for four times the history contrasts with the old fourfold QWidget
population; work at each position is bounded by the viewport and the delegate's
small document cache. Rich heterogeneous qualification adds only the real
editors required by the visible interaction state.

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
  ordinary scrolling and streaming;
- targeted structural tail appends and their model/section rebuild deltas.

The Shell exposes the ordinary-row presentation budget, rows processed in the
last pass, maximum rows observed in one pass, deferred-pass count, and pane
commit count. A 24-target deterministic burst requires at least three passes,
reaches every latest graph value, creates no card QWidget, leaves ThreadPane,
Inspector, and shell chrome counters unchanged, and becomes timer-idle after
the queue drains. Exact structural changes above a paused stable anchor issue
no viewport repaint; visible insert/remove damage begins at the changed row,
and visible moves repaint only their affected interval unless they cross the
anchor, where only the changed side below the anchor is invalidated.

## Final ownership and lifecycle

`ConversationView` owns one `ConversationItemModel`, one
`ConversationHeightIndex`, one bounded passive-delegate document cache, and
only the rich card widgets intersecting the viewport plus one viewport of
overscan. It also owns the Load More/empty controls and a hidden staging host.
It does not own a `TurnSectionWidget`, one placeholder per row, or a mutable
domain mirror. The SNode.C worker remains the only `NodeGraph` writer and the
existing `NodeGraphUiAdapter` still performs the only graph-to-presentation
projection under short nonblocking reads.

The model row's `NodeRef` is an opaque action target and lifetime pin. Qt never
dereferences it. Actions return that exact identity through the existing typed
queue after all graph guards and QWidget work have ended. A different selected
thread is the normal model-reset boundary; same-thread insert, remove, move,
and value changes use the corresponding narrow Qt model operation.

For the common one-item structural delta, `NodeGraphUiAdapter::tailCard`
verifies that the exact `NodeRef` is the last child of the last canonical Turn
and refuses prompt-materialization aliases. `ConversationView::appendTailCard`
then changes only the old tail edge, the inserted row, an optional pinned
leading owner, and scroll chrome. Coalesced multi-item structure, non-tail
insertion, removal, movement, and aliases retain the union of exact NodeRefs;
the adapter supplies canonical neighbor identities and the view emits only the
required row operations. Graph-read contention is a distinct retry result and
can never be mistaken for authoritative row removal. The former same-thread
whole-snapshot fallback is absent from Shell routing. The established direct
`ConversationView::reconcile` API remains available to non-Shell consumers and
implements precise Qt row differences rather than an unconditional reset.
Only a proven rejection while applying an already-projected exact row batch
may request an explicit authority-recovery replacement; this path has a
dedicated counter and remains zero through coalesced structural qualification.

On selection or explicit rescan, the model performs an authority replacement.
Load 80 instead accepts only a same-thread ordered superset and emits precise
history insertions and row-local changes without a reset. In both cases passive
rows need no construction; initially visible rich rows are created and measured
one per nonzero-delay staging pass beneath the hidden host. The old complete
view or stable loading cover remains visible until model order, row extents,
visible editors, and the restored anchor are ready for one commit. Ordinary
deltas bypass structural staging and resolve directly to one stable model
index.

## Delegate and editor boundary

| Content/state | Presentation | Reason |
| --- | --- | --- |
| Resting user text without images | passive delegate; promoted on hover, current-row focus, or press | Fast history scrolling while preserving selection, copy, context menu, tooltips, and keyboard interaction on demand. |
| Resting Markdown/final answer | passive `QTextDocument` delegate with a 128-document bound; promoted on interaction | Preserves Markdown appearance while preventing document count from scaling with history. |
| Resting reasoning, update, plan, agent activity, and generic tool/activity cards | passive delegate while their current state is noninteractive or collapsed | These rows need text, status, disclosure, and Turn hierarchy but no continuously live editor. |
| Any collapsed completed card | passive delegate | Disclosure can promote exactly the pointed row; no hidden subtree is retained. |
| Local optimistic prompt | real visible `ConversationCard` | Delayed sweep animation, recovery, and authoritative morph are live behavior. |
| Expanded/running command output | real visible `ConversationCard` and `CommandOutputView` | Requires nested scrolling, tail-follow state, selection/copy, streaming output, and completion controls. |
| Expanded file changes and images/attachments | real visible `ConversationCard` | Requires file/image activation, hover/cursor behavior, and rich child controls. |
| Approval and user-input controls | existing real request widgets/dialogs outside the passive row delegate | Their validation, focus, authored input, and exact response target are inherently interactive. |

When a rich row leaves overscan, only stable-keyed fold, text-selection,
current/focus identity, and command inner-scroll values survive; its QWidget is
released. Returning to the row reconstructs the established card, applies its
current model value, restores local state after final geometry, and exposes no
blank reservation. Root Turn folding sets nested row extents to zero and
releases their editors without deleting model identity.

## Behavior-parity matrix

| Existing behavior | Final path | Qualification result |
| --- | --- | --- |
| Turn/You ownership and nested steering | Flat stable rows carry explicit section/root/nested roles; the view paints one continuous Turn surface and applies the established nested inset. | Preserved in model, card, NodeGraph UI, shell, and live steering tests. |
| Optimistic prompt admission | Local prompt is an exact stable row with a real visible card and unchanged typed action target. | Calm first second, delayed sweep, recovery, and no lost draft pass. |
| Authoritative prompt acknowledgement | Local key morphs to canonical user data without changing visual identity; exact `NodeRef` is acknowledged once. | Normal and steering correlation, duplicate text, delayed result, and failure paths pass. |
| Running/delayed emphasis and lifecycle states | Active Turn and card status are row-local roles/data; borders and status update without unrelated geometry. | Pending, running, completed, interrupted, failed, and delayed-result assertions pass. |
| Streaming Markdown/plain text | Visible passive row updates its bounded document and row rectangle; a rich visible row updates only its editor. | Visible row touch count is one; offscreen stream creates/layouts/paints no QWidget. |
| Text selection and copying | Hover/current/press promotes one passive row; stable-keyed selection is captured and restored across updates/eviction. | Selection/copy before, during, and after streaming passes; live clipboard text matched exactly. |
| Markdown, links, and code blocks | Delegate uses the same Markdown policy for rest; promotion hands interaction to the established text widget. | Rendering, context menu, safe link activation, copy source, and code-block behavior pass. |
| Images and attachments | Expanded/image-bearing rows use the existing real widget; collapsed rows may be passive. | Image/file activation, fallback, attachment order, folding, and no remote embedded fetch pass. |
| Command output and completion | Visible expanded command retains `CommandOutputView`; its inner pause/follow state is restored after final geometry. | Long running-to-completed transitions, inner/outer scroll independence, selection, and no freeze pass. |
| Reasoning and update cards | Resting/collapsed content is delegate painted; exact visible interaction promotes one row. | Visibility preferences, active reasoning, stream changes, folds, and copy pass. |
| Plan cards | Resting plan rows use bounded passive presentation; active interaction promotes the row. | Ordered steps, explanation, statuses, updates, copy, fold, and accessibility pass. |
| Agent activity | Resting/collapsed activity is passive; detailed visible interaction uses the established card. | Spawn/progress/completion/interruption, deduplication, expansion state, and focus pass. |
| File changes | Expanded visible file changes retain the rich widget and exact workspace-relative targets. | Status, changed paths, link action, expansion preference, and errors pass. |
| Generic tool calls and errors | Resting/collapsed cards are passive; detailed or focused rows use the existing renderer. | Tool metadata, unknown/fallback activity, errors, interrupted/failed states, context menus, and tooltips pass. |
| Approval controls | Existing request surface remains a real widget outside passive conversation painting and carries the exact request target. | Accept/reject/review shaping passes; live rejection displayed exact facts and created no file. |
| User-input requests | Existing embedded request card and modal remain real widgets with authored input retained until exact response. | Validation/cancel/submit tests pass; live Plan-mode Alpha submission completed authoritatively. |
| Expand/collapse state | Fold state is keyed by stable row; root fold sets nested extents to zero and releases invisible editors. | Card/root folding, automatic preferences, anchor preservation, and rematerialization pass. |
| Hover, cursor, tooltip, context menu | Delegate hit-testing promotes only the pointed row, then existing widget semantics take over. | Pointer forwarding, disclosure/copy ordering, link cursor, menus, and tooltip tests pass. |
| Keyboard navigation and visible focus | Qt current index is stable identity; focused rich editor remains materialized and is scrolled into view. | Tab/Backtab, arrows, activation, modal return, visible focus, and no unrelated focus jump pass. |
| Accessibility | Model roles expose row names/structure; promoted controls retain their established accessible names and focus behavior. | Row, control, dialog, image/link, and nested-scroll accessibility assertions pass. |
| Paused scroll and exact anchoring | Anchor is stable row key plus exact vertical pixel offset and horizontal value; height deltas above it are applied through the index. | Height change, insertion, tail arrival, selection, Load 80, and steering preserve both axes. |
| Follow latest | Tail is followed only when already following; user wheel/slider activity changes to paused mode. | Arrival/completion at tail and manual pause/resume scenarios pass without blank-card exposure. |
| Atomic selection and paging | Passive rows require no construction; initially visible rich rows stage behind the old complete surface/loading cover. | Initial long selection and Load 80 expose one completed frame with bounded event-loop work. |
| Unrelated panes and idle CPU | Exact Shell routing updates Conversation only; no zero-delay retry is used. | ThreadPane/Inspector/chrome/settings counters stay unchanged for unrelated streams; live crops stay visually static. |

## Final deterministic qualification

The focused model and view tests cover stable `NodeRef` targeting; exact
insert/remove/move/data-change signals; identical-value no-ops; 10,000 model
rows without QWidget construction; logarithmic height lookup/update; bounded
visible widget counts; visible versus offscreen targeted updates; exact anchor
preservation across height changes and inserts; direct 10,000-row tail append
with zero model, section, or height rebuild; pinned-root prefix trimming;
paused/following tail behavior;
atomic selection and paging; prompt/steering acknowledgment; command completion;
selection/copy; links, files, images, folds, focus, accessibility; heterogeneous
cards; inactive panes; and bounded event-loop passes without idle spin.

The final persistent Debug build passes all 19 native suites under Xvfb/xcb.
The independently reused integrated ASan/UBSan build also passes 19/19 with no
sanitizer diagnostic. The supported NodeGraph/typed-queue/worker TSan boundary
passes 5/5 with no race report; Qt itself is not run under TSan because the
system Qt libraries are not instrumented. `npm run release --prefix web`
passes 83/83 WebUI tests, the 10,000-item profile, the Vite production build,
Chromium responsive/focus qualification, and relocatable artifact verification.

## Final performance measurements

Three Xvfb/xcb samples per size were taken from the same persistent Debug build
and benchmark as the baseline. Values below are medians.

| Loaded rows | Initial reveal | Conversation cards | Descendant QWidgets | Peak resident memory | 240-position sweep | Mean sweep position | One bounded tail append |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 320 | 16 ms | 0 | 8 | 86,116 KiB | 260.2 ms | 1.08 ms | 1.44 ms |
| 1,280 | 25 ms | 0 | 8 | 85,160 KiB | 278.1 ms | 1.16 ms | 1.60 ms |
| 10,000 | 97 ms | 0 | 8 | 100,308 KiB | 355.9 ms | 1.48 ms | 1.73 ms |

The old 320/1,280-row initial reveal was 733/4,040 ms with 4,209/16,809
descendant widgets and 125,324/281,616 KiB peak RSS. At 1,280 rows the final
initial reveal is approximately 162 times faster and uses approximately 70%
less peak resident memory. Four times the loaded history now changes the
scroll sweep by approximately 6.9%, while widget count remains exactly eight;
10,000 passive rows still create zero `ConversationCard` widgets. The original
baseline did not record process CPU counters separately, so the directly
comparable CPU-time proxy is the single-threaded initial-reveal and scroll-sweep
wall time above rather than a fabricated percentage.

The bounded append column measures the complete synchronous view operation,
including exact anchor/follow restoration and visible materialization. Every
sample reported zero model-index rebuilds and zero section-range rebuilds; the
1.44–1.73 ms spread from 320 through 10,000 rows demonstrates that loaded
history is not traversed.

During a 60-fps live 1,600-line command interval with continuous outer scrolling,
mean decoded-frame luminance deltas were 2.211289 in Conversation, 0.000012 in
ThreadPane, 0.000003 in Inspector, 0 in the shell header, and 0.000689 in the
settings/composer region. The tiny non-conversation values are H.264/cursor
noise; no unrelated content movement is visible. Start/end activity transitions
are excluded because those canonical state changes genuinely update controls.

## Full-application evidence

The Debug application was connected on isolated Xvfb display `:99` to one
workspace-local `codex-bridge`/app-server on `127.0.0.1:8093`. That bridge stayed
alive across all scenarios. Obsolete movies were removed first. Replacement
movies and compact contact sheets are under
`../../build/codexui-adapter-qualification/capture/qt-virtualized-final/`:

- `initial-very-long-thread.mp4`: atomic selection of a copied 42,911-event
  read-only local thread fixture; the first changed conversation surface is
  complete.
- `load-80-anchor.mp4`: exact-pixel paused anchor while 80 earlier activities
  are inserted; no temporary blank extent is exposed.
- `heterogeneous-history-scroll.mp4`: repeated sweeps through the long mixed
  history while editor/widget count remains bounded.
- `streaming-outer-scroll.mp4`: a real 1,600-line command while the outer
  viewport repeatedly leaves and returns to the tail; scrolling remains
  uninterrupted through running-to-completed transition.
- `streaming-command-output-scroll.mp4`: nested command-output selection and
  scrolling remain independent of the outer conversation.
- `steering-paused-anchor.mp4`: a second long command, manual pause above the
  tail, steering admission under the same Turn, and preserved viewport while
  the authoritative final answer arrives below it.
- `atomic-thread-selection.mp4`: populated-thread switches expose complete
  final frames only.
- `selection-fold-focus.mp4`: delegate promotion, real text selection/copy,
  fold/unfold, and visible Tab/Backtab focus. Clipboard verification returned
  the exact selected sentence.
- `approval-request.mp4` and `approval-reject.mp4`: exact command approval
  details and rejection; the requested probe file was never created.
- `user-input-request.mp4` and `user-input-answer.mp4`: Plan-mode embedded
  Alpha/Beta request, Review dialog, authored selection, exact submission, and
  authoritative `Alpha selected.` completion.
- `direct-tail-append.mp4`: the final Debug binary was reconnected without
  restarting the bridge; two follow-tail prompt/final turns were admitted and
  completed without blank reservation, ending with the exact authoritative
  response `SECOND BOUNDED TAIL VERIFIED.`

The movies supplement deterministic geometry and interaction assertions; lossy
video alone cannot prove a sub-frame timing bound. No source, remote, GitHub,
WebUI behavior, transport, thread, or graph-ownership change was made for the
recording setup. The temporary 168 MiB copied long-thread fixture was deleted
after paging qualification.

## Remaining limitations

- A root insertion/removal can genuinely change nesting, width, and collapse
  visibility for every row in that one Turn. That Turn alone is re-evaluated;
  unrelated Turns and the loaded conversation are not traversed.
- A first interaction with a passive row constructs that one real editor. The
  row is measured before exposure, so this trades one local interaction cost
  for loaded-history-independent idle and scrolling cost.
- Full-application recordings are finite samples. Deterministic tests and
  instrumentation are the authority for exact identities, anchors, operation
  counts, accessibility, and offscreen zero-widget work.

No remote operation occurred during implementation or qualification.
