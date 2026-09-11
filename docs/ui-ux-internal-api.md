# Native UI/UX internal API contract

This document is the canonical contract for the boundary between CodexUI's
application logic and the established native Qt UI/UX. It describes behavior,
call ordering, identity, ownership, and failure semantics in addition to C++
method signatures. `docs/ui-behavior.md` remains the visible-product contract,
and `docs/native-ui-ux-qualification-inventory.md` remains the test inventory.

The reference API is the native UI at
`dbcbb1b4d30dc24d96ed06647198a1abc6fa4c3c`. The shared-node-graph cutover may
add narrow adapter hooks, but it must not make widgets parse protocol data,
read the graph, own application state, or change the meaning of an existing
entry point.

## Boundary and call discipline

The UI accepts complete, toolkit-neutral values and emits user intentions.
The graph adapter owns no nodes and retains no projected state. A projection
call follows this sequence:

1. Qt tries one short graph read.
2. The adapter validates the supplied stable `NodeRef` and extracts only the
   values required by the established UI DTO.
3. The graph read guard is released.
4. Qt calls the existing widget API with the complete DTO.
5. The widget compares stable identity and visible values, mutates its own
   QWidget tree, and retains genuinely local state such as focus, scroll,
   folds, draft text, settings edits, or menu state.

No QWidget method, signal callback, modal dialog, notification, or typed
mailbox send may occur while a graph read is held. A failed nonblocking read
schedules a nonzero-delay Qt retry and changes no visible state. A graph
revision alone is not a UI instruction.

User actions travel in the opposite direction. The widget callback identifies
the exact visible object. Shell code resolves or receives its stable
`NodeRef`, releases any graph read, moves only new authored payload into one
typed action, and attempts admission once. Rejection keeps authored input in
the widget. Admission may be followed by a wake-failure notice, but never by
an automatic retry of a non-idempotent action.

## Compatibility matrix

The status column describes the current shared-graph branch. "Compatible"
means both shape and temporal semantics match. "Narrow extension" means the
old entry point still has its old meaning and an additional method carries a
concrete graph-cutover requirement.

| Surface | Established behavior | Current status |
| --- | --- | --- |
| `ThreadPane::Actions` | Emits New, Refresh, Hide, Select, Reload, Rename, Quick fork, Fork with options, Archive toggle, and Remove exactly once using the row's canonical string ID. | Compatible. Shell resolves the visible ID to the exact current `NodeRef` before admission. |
| `ThreadPane::refresh` | Consumes one complete hierarchy snapshot; retains expansion, selection, sort choice, optimistic rows, hover/context state; identical effective rows do no work. | Compatible after restoring provider/controller gating, effective activity time, unreachable-root retention, and ordered child relations. |
| optimistic thread methods | Begin one draft row, promote it without replacing its visual identity, mark failure, and remove only on confirmation/abandonment. | Compatible. Promotion is correlated to the admitted creation prompt rather than guessed from later payload fields. |
| `ConversationView::reconcile` | Consumes one complete `ConversationSnapshot`; keys mutate compatible cards in place; one Turn section owns one opening You card and all nested cards; identical snapshots are a no-op. | Compatible after restoring encoded section keys, canonical root pinning, stable prompt aliasing, and the identical-snapshot early return. |
| initial conversation selection | Never exposes part of an authoritative replacement. The first content frame has complete cards, final parentage, final width/height, and final anchor. When switching populated threads, the outgoing surface stays stable until the incoming final snapshot is ready. | Compatible. Provider fragments are blocked, the outgoing conversation/heading/Inspector remain staged, and readiness replaces them once with the complete bounded window. |
| conversation history window | Starts at 80 authoritative items. Pinned opening prompts do not consume the budget. Load More adds 80. While paused, new authoritative tail items expand the effective window; following resets it to the requested window. | Compatible after replacing the unbounded adapter request with per-thread requested/effective counters and excluding local prompts from the authoritative count. |
| card DTO and rendering | Typed payloads preserve the old card kinds, text, metadata, status, images, truncation disclosure, plan, diff counts, and unknown fallback. Presentation options are applied by `ConversationView`, not by protocol logic. | Compatible. Generic detail is a safe bounded rendering string because the graph deliberately does not retain raw payloads for UI convenience. |
| prompt materialization | An admitted local card keeps its `LocalPromptKey` while the authoritative user item arrives; the same widget changes type in place and preserves owner, anchor, focus, and local fold state. | Compatible through the exact adapter projection and one targeted model `dataChanged`; acknowledgement carries the related prompt `NodeRef` and performs no complete conversation reconciliation. |
| prompt recovery | A definite/uncertain failed prompt remains visible and restores text/attachments only by explicit user action, without overwriting an existing draft. | Compatible through a narrow additive recovery callback carrying the exact prompt `NodeRef`. |
| `setEmptyMessage` | Changes only the empty-state text and preserves the current anchor/follow behavior. It does not authorize clearing an existing conversation. | Compatible. Hydration staging decides whether an empty snapshot may be reconciled. |
| presentation options | Reasoning/Codex-update visibility and initial command/image/file-change folding remain local UI preferences; changing them reuses current card widgets and state. | Compatible. The adapter does not reinterpret these preferences. |
| scroll/follow API | `modeForThread`, `isAtBottom`, wheel forwarding, trailing composer space, and local-prompt preparation remain owned by `ConversationView`; each thread retains mode and anchor. | Compatible; old widget implementation is retained. Full mixed-history movie qualification is pending. |
| `ComposerPane::Actions` | Submit returns admission success; only success clears the draft. Stop, Attach, Accept, Review, and Deny are exact one-shot intentions. | Compatible. Submit/Steer selects the operation from canonical active-turn state and targets the visibly selected thread. |
| composer state setters | Attention, active turn, submit eligibility, settings eligibility, attachments, and overlay height are effective visible state; repeated values must not rebuild the composer. | Compatible. `setAttentionEnabled(bool)` keeps its original meaning; `setAttentionActionEnabled` is an additive split needed for recoverable review versus provider-actionable buttons. |
| `TurnSettingsWidget::setContext` | Reconciles identity, canonical settings, model catalog, permission profiles, revision, and update while preserving locally touched fields. | Compatible. Additive `setCanonicalContext`, `setModelCatalog`, and `setPermissionProfileCatalog` allow unrelated graph revisions not to rebuild catalog controls. |
| `MiddleRegionWidget` | Sole owner of three-pane geometry, heading, notice overlay, pane visibility, splitter state, option buttons, and cross-pane wheel routing. | Compatible. Existing signatures and geometry behavior remain; heading setters now return early for identical effective values. |
| `InspectorPane::refresh` | Accepts one complete Inspector DTO but refreshes only the visible tab; tab/scroll/expansion state is local and unrelated tab data creates no QWidget work. | Compatible. The adapter may calculate current values, but hidden tabs retain no secondary authority and perform no QWidget work. |
| Inspector Agents | One row per logical spawned child; canonical child-thread ID wins, spawn item ID is fallback; later progress/result/status updates the same row; terminal status wins; first-spawn order is stable. | Compatible after correcting activity-item projection and restricting creators to real spawn tools or started/empty sub-agent activity. |
| Inspector State | Shows useful bounded current state, counts, selected context, current domains, and pending interactions without secrets or raw protocol authority. | Compatible. Values are derived on demand from the current graph. |
| Inspector Protocol | Appends bounded chronological diagnostic metadata with time, sequence, generations, direction, authority, scope, correlation, outcome, and safe error. It is explicitly non-authoritative and preserves paused/tail scroll. | Compatible through additive `appendProtocolDiagnostic`; the legacy `appendProtocolFrame` entry point remains and converts safe metadata only. |
| shell chrome/status | Heading, connection, controller, requests, workspace, settings, status, and composer eligibility change only when their effective visible values change. | Compatible after provider readiness and hydration gating; memoized effective values prevent item streaming from rewriting chrome. |
| removal/lifetime | Removed nodes detach their QWidget references synchronously before retirement acknowledgement; queued stable references keep nodes alive but cannot resurrect them. | Compatible. |
| graph contention | No blocking, no visible half-update, no zero-delay retry loop, and no loss of the newest reduced state. | Compatible in focused tests; full sustained-contention qualification remains. |

## Class and method reference

This section is exhaustive for the logic/UI boundary. Private layout helpers
and ordinary Qt overrides are implementation details unless their behavior is
called out below.

For every class below, “Qt-main only” is a hard thread-affinity requirement.
No method throws as part of its normal contract. Allocation failure may still
propagate according to ordinary C++/Qt rules, but must not leave a published
partial graph transaction or an exposed partial QWidget structure. Parameters
described as values may be moved by the callee after admission; const-reference
DTO parameters remain owned by the caller for the call. Callbacks are replaced,
not accumulated, by their setter.

### `ui::NodeGraphUiAdapter`

This final, non-QObject class is the only graph-to-established-DTO translator.
It stores only a non-owning pointer to the one `NodeGraph`. It must not cache a
projection, register callbacks, create widgets, or own a `NodeRef` beyond the
duration/value returned by a call.

Thread/lock contract: called on Qt-main. Each query performs one nonblocking
`tryRead`; successful DTO construction occurs under that read and returns only
after the guard is destroyed. It never calls a callback or QWidget. `nullopt`
always means “no coherent value was available now”, never “render empty”.

- `NodeGraphUiAdapter(const NodeGraph&)` binds the one canonical graph. The
  referenced graph must outlive the adapter.
- `threads(selectedThread)` tries one read and returns the complete
  `ThreadListSnapshot`. `nullopt` means contention and must cause a delayed
  retry, not an empty list render. The optional target is accepted only when
  it is the current non-removed graph instance.
- `conversationInfo(thread)` returns the lightweight hydration/history facts
  needed before a potentially larger projection: authoritative item count,
  display readiness, hydration failure, and provider continuation. Local
  prompts never contribute to the authoritative count.
- `conversation(thread, itemLimit)` returns one complete retained
  `ConversationSnapshot` for a validated thread. `itemLimit` is the effective
  per-thread history window, never an instruction to mutate graph state.
- Presentation visibility remains the item model/view's responsibility so
  toggling it can reuse visible editors and stable local interaction state.
- `card(thread, item)` projects one validated item only when the item
  is still parented by a Turn owned by the supplied thread. It is reserved for
  a targeted visible-card update and must never reconstruct identity from
  payload fields. A stale/detached item returns `nullopt`.
- `promptMaterialization(thread, item)` accepts only an authoritative user item
  related to one current local prompt in `awaitingMaterialization`. It returns
  the authoritative presentation under that prompt's `LocalPromptKey`, the
  authoritative Item `NodeRef` for row ownership, and the separate exact
  prompt `NodeRef` for acknowledgement.
- `rowChange(thread, item)` projects one live selected-thread Item, its exact
  section/root/nesting facts, and the immediate canonical card keys on either
  side. It is the non-snapshot input for an exact middle insertion or move and
  distinguishes a busy graph read from an authoritative absence; it retains no
  ordering state after the read guard is released.
- `tailCard(thread, item)` additionally requires that the exact item
  be the last child of the last canonical Turn and that it not participate in
  prompt-materialization aliasing. It returns one `ConversationTailCard` with
  section/root/nested/activity placement and current history chrome for the
  bounded structural append path. Any ambiguity returns `nullopt`.
- `ConversationInfo` is adapter control metadata, not a presentation model or
  widget snapshot.

| Method | Parameters and return | Preconditions, postconditions, failure |
| --- | --- | --- |
| constructor | `graph`: long-lived canonical graph; no return | Pre: graph outlives adapter. Post: no read and no allocation is performed. |
| `threads` | `selectedThread`: optional stable target; returns optional complete DTO | Stale/removed selection is represented as no selected ID, while valid roots still project. Contention returns `nullopt` without side effects. |
| `conversationInfo` | `thread`: required stable Thread; returns optional control facts | Wrong kind, stale generation, removal, or contention returns `nullopt`. Success does not construct card DTOs. |
| `conversation` | `thread`, positive effective `itemLimit`; returns optional complete snapshot | Pre: the caller has observed `conversationInfo.readyForDisplay`; this primitive projects the graph's current content and does not itself infer temporal hydration completeness. Limit is clamped to at least one. Success preserves canonical order and root ownership. Invalid target/contention returns `nullopt`. |
| `card` | exact `thread` and `item`; returns optional card DTO | Success requires the item still be a child of a Turn owned by the exact thread. It never searches by payload IDs. |
| `promptMaterialization` | exact `thread` and authoritative `item`; returns optional card DTO | Success requires one live related local prompt owned by the thread with a valid submission ID and awaiting-materialization state. No relation inference or payload-ID search is permitted. |
| `rowChange` | exact `thread` and live `item`; returns `ConversationRowProjection` | `graphBusy` requests one nonzero-delay retry and is never interpreted as removal. A present change requires current Turn ownership by the exact thread; an absent change with `graphBusy == false` is authoritative absence. Immediate neighbor keys reflect canonical graph order with a materializing local prompt suppressed behind its authoritative row. |
| `tailCard` | exact `thread` and `item`; returns optional tail DTO | Success requires the exact canonical last item of the exact canonical last Turn, usable loaded-count state, and no prompt alias. The DTO is non-authoritative and owns only values needed for one Qt append. |

### `middle::ThreadPane`

`ThreadPane` owns the sidebar's QWidgets, selected-row rendering, expanded
thread IDs, current sort criterion, pending-prompt animation, context-menu
state, and row comparison values.

Thread/ownership contract: Qt-main only; QObject parenting owns every row and
popup. The pane owns no graph references or provider state. Callbacks may enter
shell code synchronously, so all caller graph guards must already be released.

- `ThreadPane(parent)` constructs the established sidebar and restores its
  local expansion behavior. The sort control contains exactly Alphanumeric,
  Created, and Recent, with Recent selected initially.
- `setActions(Actions)` replaces the callback bundle. Missing callbacks make
  the corresponding gesture a no-op; callbacks execute without graph locks.
- `refresh(snapshot)` compares a complete DTO with the last effective rendered
  list. It patches/reorders only as required, preserves local expansion and
  context state, and does nothing for an identical effective list. It never
  initiates hydration or provider operations itself.
- `beginOptimisticThread(id, title, cwd)` inserts one local draft row using the
  supplied stable provisional ID without changing canonical graph authority.
- `promoteOptimisticThread(draftId, authoritativeId)` changes the row's action
  identity in place and preserves its selection/animation/position.
- `confirmOptimisticThread(threadId)` removes only the matching optimistic
  overlay once a canonical row represents it.
- `failOptimisticThread(threadId)` retains the row and changes its local
  failure presentation so recovery/navigation remains possible.
- `isOptimisticThread(threadId)` is a side-effect-free membership query used
  only by shell correlation logic.
- `setSortCriterion(criterion)` selects one of Alphanumeric, Created, and
  Recent and reconciles the current snapshot once. Alphanumeric is natural,
  case-insensitive title order; Created and Recent are newest-first with
  missing timestamps last.
- `currentSortCriterion()` returns that local rule without triggering work.
- `visiblySelectedThreadId()` returns the ID of the row the user currently
  sees as selected. Outbound prompt routing must use this value, not a stale
  shell selection.
- `Actions::select/reload/rename/fork/forkWithOptions/toggleArchive/remove`
  carry exactly the pointed row ID. `Actions::newThread/refresh/loadMore/hide`
  carry no inferred target. `loadMore` is requested only at the bounded
  near-list-end threshold; runtime single-flight and cursor guards decide
  whether a provider request is required.

| Method | Parameters / return | Preconditions and observable effect |
| --- | --- | --- |
| constructor | optional QWidget `parent` | Constructs one empty pane; restores only local settings. Performs no callback. |
| `setActions` | replacement `Actions` value | Post: later gestures use only this bundle. Does not replay a gesture. |
| `refresh` | complete `ThreadListSnapshot` by const reference | Snapshot remains valid for the call only. Post: rendered hierarchy/selection equals its effective value plus the selected ordering, local expansion, and optimistic rows. Identical effective input performs no row work. |
| `beginOptimisticThread` | provisional `id`, display `title`, `cwd` | `id` must be nonempty and process-locally unique. Duplicate begin updates no canonical graph state. |
| `promoteOptimisticThread` | exact `draftId`, exact `authoritativeId` | If draft is absent, no-op. Post: callbacks and visible selection use authoritative ID without replacing unrelated rows. |
| `confirmOptimisticThread` | current provisional/promoted `threadId` | Removes only the matching overlay; canonical row remains. |
| `failOptimisticThread` | exact optimistic ID | Marks only that overlay failed and keeps it recoverable/selectable as defined by UI behavior. |
| `isOptimisticThread` | ID; returns bool | Pure local query. |
| `setSortCriterion` | enum value | Reorders root groups atomically from the retained snapshot. Child hierarchy remains intact. |
| `currentSortCriterion` | no parameters; returns enum | Pure local query. |
| `visiblySelectedThreadId` | no parameters; returns canonical/provisional string | Empty when no visible row is selected. This is the outbound routing source of truth. |

Thread catalog startup is two-stage. The runtime requests one bounded
`recency_at` descending page with `useStateDbOnly=true`, publishes it, then
schedules an automatic first-page request with `useStateDbOnly=false`. The
second request lets app-server reconcile persisted session files into its
database without delaying the first usable sidebar. Its `nextCursor` becomes
the paging authority. `LoadMoreThreads` consumes at most one cursor page per
near-end request using the repaired database, rejects repeated cursors, and
merges rows through the same graph-backed list projection so selection,
hierarchy, optimistic names, pending animation, and current sorting survive.
A provider-generation change invalidates the timer, cursor, and in-flight
cycle together. No manual repair command is exposed.

`suggestForkName(sourceTitle, existingTitles)` is toolkit-independent and
returns the first unused direct descendant of the source's parsed fork
lineage. `NewThreadDialog` accepts either Create or Fork purpose; Fork reuses
the complete creation form with a prefilled, editable suggested name. The
client-only `requestedName` is removed before `thread/fork`, applied as a local
overlay to the returned thread, and synchronized by a separate
`thread/name/set` request.

### `middle::ConversationItemModel`

`ConversationItemModel` is the thin `QAbstractListModel` indexing surface for
the selected conversation. It is owned by `ConversationView` and used only on
Qt-main. `NodeGraph` remains the sole canonical state and the SNode.C worker
remains its sole writer. The model neither reads the graph nor retains protocol
payloads, revisions, a journal, or an independently mutable domain state.

Each row contains the last rendered `VisibleCardData`, its unchanged `NodeRef`
action token, stable key, canonical Turn section key, root/nested position,
presentation visibility, and active-Turn emphasis. `VisibleCardData` is
available to the view/delegate through the typed `card(row)` accessor rather
than copied through `QVariant`; standard roles expose only small identity,
structure, visibility, and accessibility values.

- `replaceConversation(snapshot)` is the explicit complete-authority operation
  for a different thread or genuine rescan and emits `modelReset` only when
  effective state differs. `prependHistoryPage(snapshot)` accepts only a
  same-thread ordered superset, inserts its missing ranges, and never resets or
  moves retained rows.
- `insertCard(row, placement)`, `removeTarget(ref)`, and
  `moveTarget(ref, destination, placement)` are the exact structural
  operations. They reject duplicate/stale/ambiguous targets and emit only the
  matching insert, remove, move, and affected structural-role changes.
- `reconcile(snapshot)` preserves the established direct `ConversationView`
  contract with precise ordered row differences. Production Shell graph
  routing never uses it as a delta fallback: selection/rescan calls replacement,
  paging calls ordered-superset insertion, and live changes carry exact refs.
- `updateCard(card)` resolves the stable key once and returns `Missing`,
  `Incompatible`, `Unchanged`, or `Changed`. Only `Changed` emits row-local
  `dataChanged` with the affected roles.
- `appendTail(tail)` accepts only a unique card in the exact last Turn
  position, changes the former tail's `LastInTurnRole`, and emits one row
  insertion. `trimHistoryTo(limit)` retains an owning Turn root where needed
  and removes only the bounded prefix. Rows live in a conversation-specific
  order-statistic tree; stable-key and exact-NodeRef maps point to stable row
  nodes, so insert, remove, move, and surviving identity lookup never rebuild
  or renumber a loaded-history-sized index.
- `setHistoryChrome(hidden, providerHasMore)` changes only Load More facts;
  `setActiveTurn(row, active)` changes only the exact root role.
- `setVisibility(visibility)` changes only the rows whose presented role
  changes. It does not delete their stable identities or mutate graph state.
- `indexForStableKey(key)` resolves view-local identity. `indexForTarget(ref)`
  additionally compares the pinned node pointer identity so a stale action can
  never retarget a replacement node with a similar provider ID.

### `middle::ConversationHeightIndex`

`ConversationHeightIndex` is a non-QObject order-statistic extent tree owned by
the view. It stores only nonnegative row extents plus subtree row counts and
`qint64` sums. `top`, `bottom`, total height, position-to-row lookup, a changed
row height, and tail or non-tail insert/remove/move operations touch only
logarithmic tree paths. `assign` is the explicit complete-sequence operation
and the only operation counted as a rebuild. Scrollbar conversion remains a
separate view concern. The index contains no card values or authority.

### `middle::ConversationView`

`ConversationView` is the canonical variable-height `QAbstractItemView` for
the conversation. It owns the thin item model, height index, bounded delegate
document cache, visible rich cards/editors, Load More and empty controls, and
genuinely local interaction state. It retains per-thread history windows,
follow/pause anchors, fold state, text selections, focus/current-row identity,
nested command-output scroll state, and presentation options by stable row
key. Shell retains only the selected canonical graph target.

Passive-row hover, cursor, and tooltip hit-testing stay in the delegate and do
not materialize a card. A press or keyboard current-row transition may create
the one required editor. When production materialization starts collapsed, the
real card initially contains only its header/control/status surface; hidden
Markdown, command output, plan/file/activity detail, image, attachment, and
other body projection is deferred until expansion and is built from the latest
row value. Accessible model detail is bounded to 8,192 characters without
first traversing or converting an unbounded plan or file-change collection.

Thread/ownership contract: Qt-main only. Passive historical rows have no
QWidget or placeholder. QObject parentage owns only the rich cards currently
inside the viewport plus one viewport of bounded overscan and temporary hidden
staging cards. Snapshot `NodeRef` action tokens are opaque; the view never
dereferences them. Graph/action callbacks run only after graph guards have been
released, and QWidget work never occurs while a graph or channel lock is held.

- `ConversationView(parent)` creates the established scroll surface, its
  private list model and delegate, height index, Load More control, empty label,
  and hidden staging host. It creates no historical card widgets.
- `setLoadMoreAction(callback)` installs the one user gesture for expanding
  history after the view has advanced its own requested/effective window. The
  callback decides whether to project or send the exact provider action.
- `setPromptMaterializedAction(callback)` is a narrow additive integration
  hook. After a local card has successfully morphed to its authoritative user
  card and after all QWidget work, it returns the exact prompt `NodeRef` for
  worker acknowledgement. `false` stops further acknowledgements in that
  pass; the widget never retries automatically.
- `setPromptRecoveryAction(callback)` is a narrow additive hook fired only by
  explicit recovery on the exact failed local-prompt token.
- `setPresentationCommittedAction(callback)` fires once only after a selected
  thread's staged model, geometry, editors, cover, and spinner have committed.
  Shell uses that boundary to reveal matching heading and Inspector values.
- `setEmptyMessage(message)` changes empty text only, preserving anchor and
  follow behavior. It must not clear cards.
- `setPresentationOptions(options)` updates reasoning/update visibility and
  initial folding preferences using the already retained snapshot. Existing
  card-local fold choices remain authoritative.
- `presentationOptions()` returns the current local preferences without work.
- `reconcile(snapshot)` applies a complete structural value through the item
  model's precise signals, updates only bounded materialized editors, rebuilds
  indexed geometry only when structure genuinely requires it, restores the
  stable row/pixel anchor, and exposes one completed viewport state.
- `beginThreadSelection(threadId)` immediately covers the outgoing message
  viewport with the application background. If the identified selection is
  still unresolved after 500 ms, the cover paints one centered 30 px neutral
  gray ring with a 3 px stroke; its 33 ms animation timer exists only while
  the ring is visible. A superseded thread identity cannot reveal or dismiss
  the current cover.
- `reconcileStaged(snapshot)` preserves that final-state contract for explicit
  selection/rescan replacement. `prependHistoryPageStaged(snapshot)` applies
  Load 80 as a same-thread ordered superset without reset or retained-row
  movement. Only rich rows expected in the initial viewport and bounded
  overscan are constructed and measured one at a time beneath the hidden
  staging host; passive rows need no construction. The old complete surface or
  stable loading cover remains visible until one final commit.
- `applyCardPresentation(card)` is the ordinary exact-row path. An identical
  value is a no-op. An offscreen update changes model data and cached/indexed
  facts without constructing, laying out, or painting a QWidget. A visible
  passive row invalidates only its row rectangle; a visible rich row applies
  only to that editor and propagates only its genuine height delta.
- `applyPromptMaterialization(value)` morphs one local-keyed row, transfers its
  model ownership to the authoritative Item `NodeRef`, and then acknowledges
  the separate prompt `NodeRef`. Prompt retirement cannot remove the row.
- `applyRowChange(value)` resolves the projected neighbor keys against the
  current bounded model and emits only the required insert, move, or structural
  row update. Stable-key section boundaries and the extent tree are updated
  only for the affected old/new Turn; later sections are neither shifted nor
  rebuilt. Coalesced sibling changes are applied in canonical neighbor order.
- `removeCardTarget(ref)` removes only the row currently indexed by that exact
  Item `NodeRef`; unrelated and already-transferred prompt removals are no-ops.
- `appendTailCard(tail)` is the ordinary structural fast
  path after `NodeGraphUiAdapter::tailCard` validates canonical placement. It
  emits one insert, performs an optional bounded prefix trim, preserves the
  stable anchor or existing follow state, and never rebuilds model, section, or
  height indexes. `false` keeps the exact NodeRef on the canonical-neighbor row
  path; it does not request a snapshot diff.
- `historyLimitForThread`, `requestNextHistoryPage`, and
  `forgetThreadPresentation` own the requested/effective 80-row window and its
  lifecycle beside that thread's anchor/follow state. Canonical counts are
  inputs; these methods create no domain authority and issue no provider call.
- `presentedThreadId()` identifies the complete model frame currently exposed
  (or covered during replacement), never the merely selected graph target.
- `conversationModel()` exposes the owned model for Qt selection,
  accessibility, deterministic instrumentation, and exact action targeting;
  callers must not treat it as graph authority.
- `materializedCardCount()` reports the current bounded rich-widget count for
  qualification; it does not count delegate-painted rows.
- `setTrailingSpaceHeight(height)` represents only the composer's overlay
  growth below conversation content and preserves current scroll semantics.
- `prepareForLocalPromptAdmission()` resumes following only when pause was
  caused solely by composer growth. Explicit user scrolling stays paused.
- `forwardWheelEvent(event)` lets the middle region route a wheel/touchpad
  gesture to the canonical conversation scroll owner after nested controls
  decline it.
- `mode()`, `modeForThread(id)`, and `isAtBottom()` are side-effect-free
  scroll-state queries used to calculate the history window and action UX.
- `dispatchingNativeWheel()` prevents recursive event-filter forwarding.
- `trailingSpaceHeight()` reports the current overlay compensation.
- `PresentationOptions` has five independent local values:
  `showReasoning`, `showCodexUpdates`, `commandsInitiallyExpanded`,
  `imagesInitiallyExpanded`, and `fileChangesInitiallyExpanded`. Initial-fold
  values are consulted only while materializing a new matching card; retained
  card-local disclosure state always wins.

| Method | Parameters / return | Preconditions and observable effect |
| --- | --- | --- |
| constructor | optional QWidget `parent` | Produces an empty following-mode item view. No historical card widgets exist. |
| `setLoadMoreAction` | replacement `void()` callback | Called once per accepted button gesture after the view advances its local history window; the callback may project retained rows or request the provider. |
| `setPromptMaterializedAction` | replacement `bool(NodeRef)` callback | Called after a successful local-to-authoritative visual transition. Exact token is moved to callback. False aborts only the remaining callbacks in this reconcile. |
| `setPromptRecoveryAction` | replacement `void(NodeRef)` callback | Called only from explicit recovery gesture on the current matching card. |
| `setPresentationCommittedAction` | replacement `void(threadId)` callback | Called after the exact selected staged frame is complete and visible. Superseded stages never call it. |
| `setEmptyMessage` | display `QString` value | Changes only empty-label text; model rows remain. Anchor is preserved. |
| `setPresentationOptions` | complete local options | Updates model presentation roles and visible/materialized rows without a graph query. Existing user fold choices win over initial-fold defaults. |
| `presentationOptions` | returns value copy | Pure query. |
| `reconcile` | complete snapshot const reference; returns changed bool | Explicit immediate authority replacement used by direct consumers/tests. Pre: unique section/card stable keys and correct root keys. Post: model order, indexed geometry, bounded editors, delegate surface, and scroll policy match one complete target. False means no effective model change. |
| `beginThreadSelection` | exact selected thread ID | Immediately covers only the message viewport and starts one 500 ms visual-delay timer. Repeating the same pending identity is a no-op; a new identity cancels superseded staging. |
| `reconcileStaged` | owned complete snapshot | Explicit different-thread or rescan replacement; only initially visible rich editors are prepared beneath the hidden host in bounded event-loop passes before one atomic reveal. |
| `prependHistoryPageStaged` | owned same-thread ordered superset | Inserts missing history ranges and patches changed retained rows without a model reset or movement, then reveals one complete staged frame. |
| `applyCardPresentation` | one exact `VisibleCardData`; returns optional local impact | Wrong thread/key/incompatible kind returns `nullopt`; identical data returns `None`; otherwise only the resolved row, its genuine section-edge geometry, and its visible editor/delegate rectangle may change. |
| `appendTailCard` | one validated `ConversationTailCard`; returns bool | Exact canonical tail updates the view-owned history window, inserts directly, and optionally trims the prefix without retained-history traversal. Wrong thread, duplicate/invalid placement, or active staging returns false so the same NodeRef proceeds through exact neighbor placement. |
| `historyLimitForThread`, `requestNextHistoryPage` | thread ID plus current canonical history facts | Update only per-thread presentation-window counters and return the effective projection limit/provider-request decision. |
| `forgetThreadPresentation`, `presentedThreadId` | retired thread ID / pure current-frame query | Releases per-thread window/anchor state or reports the complete frame currently owned by the model. |
| `conversationModel`, `materializedCardCount` | borrowed model pointer / integer count | Inspection only. The model is non-authoritative and the widget count remains viewport proportional. |
| `setTrailingSpaceHeight` | nonnegative effective pixels | Post: content extent/anchor reflects composer overlay without changing viewport ownership. Repeated value is a no-op. |
| `prepareForLocalPromptAdmission` | no parameters | May change pause caused only by composer growth; never overrides explicit user pause. |
| `forwardWheelEvent` | live `QWheelEvent*`; returns consumed bool | Event is not owned. Nested eligible control must have declined it. |
| `mode`, `modeForThread`, `isAtBottom`, `dispatchingNativeWheel`, `trailingSpaceHeight` | pure queries | No layout, paint, callback, or scroll mutation. |

### `middle::ConversationCard`

Cards remain the established specialized renderers. They do not read the
graph. Their `VisibleCardData` is the entire canonical presentation input.
`ConversationPresentation` supplies only pure status, plan, agent-metadata,
file-change, and generic-activity display values shared with the passive
delegate. It owns no renderer selection, geometry, interaction, or state.

- `ConversationCard(data, parent, commandInitiallyCollapsed,
  imageInitiallyCollapsed, fileChangesInitiallyCollapsed)` creates exactly
  one specialized existing card widget. The three initial-fold values are
  consulted only for their matching kinds; QObject parentage owns the widget.
- `createConversationCard(...)` is the equivalent allocation helper used by
  `ConversationView`; it introduces no registry or alternate ownership.
- `data()` returns the last applied DTO for identity/action comparison.
- `canApply(data)` reports whether the existing concrete card can represent a
  new DTO. It permits the intentional local-prompt to user-message handoff.
- `apply(data)` preserves the old boolean contract: `true` means visible
  presentation changed.
- `applyPresentation(data)` additionally classifies the local effect as
  `None`, `PaintOnly`, or `GeometryChanged`; it does not propagate a global
  invalidation.
- `isCollapsed()` and `setCollapsed(value)` read/write user-owned fold state.
- `setAuthoritativeTurnActive(value)` changes only the owner card's canonical
  active emphasis and returns whether paint state changed.
- `setNestedPresentation(value)` applies the established nested visual style
  when a card is not itself the Turn root. The item view paints the continuous
  Turn/You surface and positions nested rows independently, so a visible card
  never owns historical sibling rows.
- A card never owns nested sibling widgets. The virtualized view owns each
  visible row directly and paints the continuous Turn surface independently.
- `setViewportVisible(value)` pauses purely local visual feedback when a card
  cannot paint; it never changes canonical status.
- `commandOutputScrollState()` and `restoreCommandOutputScrollState(state)`
  preserve the user's inner-output pause/follow position across a necessary
  compatible card reconstruction.
- `foldRequested` reports a local fold gesture. `recoveryRequested` reports
  explicit recovery; neither signal performs graph work directly.

`FileChangesData::cwd` carries the owning thread workspace solely for resolving
a relative displayed path after an explicit link gesture. It is presentation
input, not another workspace authority. File links and `ImageThumbnail`
activation call the platform desktop URL handler after all graph access; no
internal image dialog, callback registry, or file-opening cache is retained.

`ContentSizedTextView::setContent` and `CommandOutputView::setOutput` return
whether effective content/geometry changed. `CommandOutputView` alone owns its
inner wheel/follow state; restoring it must not move the outer conversation.

`presentation::userMessageMarkdown` is a presentation-only projection. It
turns soft newlines in authored user text into visible Markdown line breaks
while leaving blank lines, existing hard breaks, indented code, and fenced code
intact. `MarkdownTextView::markdownSource()` and card copy continue to expose
the original canonical source. Normal and steering user messages use this same
path in both rich widgets and passive delegate documents.

| Method | Parameters / return | Preconditions and observable effect |
| --- | --- | --- |
| `data` | returns const DTO reference | Reference is valid until next successful apply or destruction; caller must not retain it across reconciliation. |
| `canApply` | candidate DTO; returns bool | Pure compatibility check; no QWidget mutation. |
| `apply` | complete candidate DTO; returns visible-change bool | Requires `canApply`; post: `data()` equals candidate and specialized controls show its values. |
| `applyPresentation` | complete candidate DTO; returns impact enum | Same postcondition as `apply`; impact is local and must not be promoted blindly to pane/window invalidation. |
| collapse methods | bool setter / bool query | Fold state is user-owned; the view updates only the affected indexed row/section range and restores the exact anchor. |
| `setAuthoritativeTurnActive` | bool; returns paint-change bool | Valid primarily for the root You card. No geometry change for border-only state. |
| viewport visibility | bool | Affects only local timers/painting, not data or identity. |
| command output state methods | optional state / state const reference | Preserve inner scrollbar value/follow mode without modifying outer anchor. |

### `middle::ComposerPane`

`ComposerPane` owns prompt text, attachments, focus, submission keyboard
rules, attention controls, adaptive layout, and the Send/Steer/Stop surfaces.

- `ComposerPane(anchor)` creates the bottom-aligned overlay relative to the
  supplied center anchor.
- `setActions(Actions)` replaces the user-intention callbacks.
- `setExtraOverlayHeightAction(callback)` reports only the height above the
  canonical reserve so ConversationView can compensate without resizing its
  viewport.
- `setAttachments(values)` replaces the user-owned attachment draft list;
  `attachments()` returns it unchanged and in order.
- `setAttentionVisible(value)` shows/hides the current request surface without
  resolving it.
- `setAttentionRequest(title, detail, directAccept, acceptLabel)` changes the
  effective request presentation and synchronizes geometry only when needed.
- `setAttentionEnabled(value)` retains the old contract: all visible request
  actions share the same enablement.
- `setAttentionActionEnabled(providerEnabled, reviewEnabled)` is the narrow
  additive form that can keep explicit recovery Review available while direct
  provider Accept/Reject is disabled.
- `setActiveTurn(value)` chooses the established Steer/Stop versus Send
  presentation; it does not infer the target.
- `setCanSubmit(value)` supplies canonical eligibility. The final Send/Steer
  button also requires non-whitespace editor content.
- `setSettingsEnabled(value)` changes only settings control eligibility.
- `clearDraft()` clears prompt and attachments exactly once after successful
  admission or an explicit new-thread reset.
- `synchronizeGeometry()` performs the old local layout transaction and emits
  overlay-height change only when the effective height changed.
- `canonicalReserve()`, `canonicalReserveHeight()`, `extraOverlayHeight()`,
  `promptEditor()`, and `turnSettings()` expose established child surfaces to
  the middle/shell coordinators without transferring ownership.
- `Actions::submit` returns `true` only for guaranteed admission. `stop`,
  `attach`, `accept`, `review`, and `deny` are one-shot void intentions.

| Method | Parameters / return | Preconditions and observable effect |
| --- | --- | --- |
| constructor | non-null geometry `anchor` | Anchor outlives pane. Constructs reserve/overlay/editor/settings surfaces. |
| `setActions` | replacement callback value | Does not submit or clear current draft. |
| `setExtraOverlayHeightAction` | replacement `void(int)` | Callback receives effective extra pixels only after a change and outside graph access. |
| attachment methods | ordered vector value / const reference query | Setter replaces only attachment UI state. Query reference is valid until next setter/destruction. |
| attention methods | visible flag, display values, action flags | Patch effective controls only. No method resolves a request. `reviewEnabled` affects Review only. |
| `setActiveTurn` | bool | Changes button labels/visibility and style only on value change. |
| `setCanSubmit` | canonical eligibility bool | Effective button additionally depends on trimmed editor content; setter never clears it. |
| `setSettingsEnabled` | bool | Delegates eligibility without resetting touched values. |
| `clearDraft` | no parameters | Clears editor and attachments and resynchronizes geometry. Caller may invoke only after admitted send or explicit draft reset. |
| `synchronizeGeometry` | no parameters | Idempotently computes canonical/extra height and positions overlay. Guards reentrant layout requests. |
| child/height accessors | no parameters; borrowed pointer/value | Pure; ownership remains with pane. |

### `codex::TurnSettingsWidget`

This widget owns locally touched settings fields and their menus/controls.

- `setContext(identity, canonical, models, permissionProfiles, revision,
  update)` retains the complete legacy entry point and reconciles all three
  canonical sources.
- `setCanonicalContext(identity, canonical, revision, update)` is an additive
  narrow update for selected-thread settings only.
- `setModelCatalog(models)` and
  `setPermissionProfileCatalog(permissionProfiles)` are additive independent
  catalog updates; identical catalog revisions do no control rebuilding.
- `setControlsEnabled(value)` applies effective edit eligibility.
- `setWorkspace(path)` is an explicit user/local-draft change.
- `workspace(fallback)` returns the selected cwd or fallback.
- `threadStartOptions()` returns only options valid for thread creation.
- `turnStartOptions()` returns only options valid for a new turn. Neither
  accessor performs provider work or mutates controls.

| Method | Parameters / return | Preconditions and observable effect |
| --- | --- | --- |
| constructor | optional QWidget parent | Builds controls with default/local values, no provider call. |
| `setContext` | identity, canonical JSON, model JSON, permission JSON, revision, sparse update | Complete compatibility entry point. Applies only untouched canonical fields and refreshes changed catalogs. |
| `setCanonicalContext` | identity, canonical JSON, revision, sparse update | Does not alter cached catalog values. Identity change establishes a new touched-field scope. |
| catalog setters | JSON catalog value | Preserve canonical settings and touched controls; identical value is a no-op. |
| `setControlsEnabled` | bool | Changes interactivity only, not values. |
| `setWorkspace` | QString path | Explicitly changes the local workspace field and marks it touched. |
| `workspace` | UTF-8 fallback; returns UTF-8 path | Pure value extraction with fallback for blank current value. |
| option accessors | no parameters; return owned JSON value | Pure serialization of current effective controls into the correct protocol scope. |

### `middle::InspectorPane`

The pane owns tabs, per-tab comparison state, Agents expansion, State/Protocol
scroll, bounded protocol lines, and the pre-existing asynchronous Changes
viewer. Its DTO is current presentation input, never authority.

- `InspectorPane(parent)` constructs the established Plan, Agents, Changes,
  Requests, State, and Protocol surfaces.
- `setHideAction(callback)` installs the one pane-hide gesture.
- `setRequestActions(review, accept, reject)` installs exact interaction-ID
  callbacks. Dialog validation remains inside the existing request workflow.
- `refresh(snapshot)` retains the newest complete DTO and refreshes only the
  visible tab when the pane is visible. Hidden tabs do no QWidget work and
  render the latest value once when activated.
- `appendProtocolFrame(frame)` preserves the legacy diagnostic entry point but
  extracts only safe metadata and delegates to the bounded diagnostic path.
- `appendProtocolDiagnostic(effect)` appends one already-redacted metadata
  record, preserves tail/paused scroll, detects sequence gaps, and never stores
  raw payloads or changes application authority.
- `tabs()` exposes the established tab widget for Request navigation and saved
  user selection.

| Method | Parameters / return | Preconditions and observable effect |
| --- | --- | --- |
| constructor | optional QWidget parent | Builds all established tab shells but does not populate history rows. |
| `setHideAction` | replacement callback | Called once by Hide gesture. |
| `setRequestActions` | three exact-ID callbacks | Missing callback disables/no-ops that gesture; pane never guesses a target. |
| `refresh` | complete Inspector snapshot const reference | Retains newest value. If visible, refreshes only active tab; if hidden, performs no QWidget projection. Repeated per-tab value is a no-op. |
| `appendProtocolFrame` | legacy safe JSON diagnostic | Input valid for call only. Extracts bounded metadata, discards raw payload, then invokes diagnostic append semantics. |
| `appendProtocolDiagnostic` | typed `UiEffect` const reference | Non-diagnostic kind is ignored. Diagnostic values are bounded/redacted; visible Protocol appends only new lines, hidden Protocol updates retained bounded text only. |
| `tabs` | returns borrowed QTabWidget pointer | Pure; pane retains ownership. |

### `middle::MiddleRegionWidget`

This class remains the sole geometry and cross-pane event owner.

- `threads()`, `conversation()`, `composer()`, and `inspector()` return the
  existing owned pane instances; callers must not replace them.
- `splitterWidget()` exposes the established splitter for saved sizing/tests.
- `setThreadHeading(title, metadata, trailingMetadata, state, tone)` patches
  only changed heading fields and style tone.
- `showNotice(message, error)` presents the existing non-layout-shifting timed
  notice overlay.
- `showSidebar(value)` and `showInspector(value)` preserve splitter geometry
  and user visibility choice; `sidebarVisible()` and `inspectorVisible()` are
  side-effect-free queries.
- `setPaneVisibilityAction(callback)` reports user-visible pane state so shell
  restore controls can mirror it.
- `routeScrollEvent(watched, event)` preserves nested-scroll precedence and
  returns `true` only when the conversation consumed the gesture.

| Method | Parameters / return | Preconditions and observable effect |
| --- | --- | --- |
| constructor | optional parent | Constructs exactly one ThreadPane, conversation region, ComposerPane, and InspectorPane in the established splitter. |
| pane accessors | no parameters; borrowed references | Pure; lifetime is the middle region's. |
| splitter accessor | no parameters; borrowed pointer | Pure; caller may inspect/persist sizes but not replace ownership. |
| `setThreadHeading` | five display values | Repeated effective tuple is a no-op. Tone changes repolish only the state label. |
| `showNotice` | message value, error flag | Empty/updated notice uses existing overlay and timer; does not change center layout allocation. |
| pane visibility methods | bool setters / bool getters | Preserve splitter sizes and report effective visibility once through callback. |
| `setPaneVisibilityAction` | replacement callback | Does not emit until a visibility transition. |
| `routeScrollEvent` | watched QObject and live QEvent; returns bool | Does not take ownership. Routes only supported wheel gestures and prevents recursion. |

### `ShellWidget`

`ShellWidget` is the coordinator, not a renderer or model. Its constructor
wires the existing callbacks to typed actions, QSocketNotifier delivery, the
adapter, dialogs, and pane routing. Its event filter delegates wheel behavior
to `MiddleRegionWidget`. It must not parse app-server JSON, call CodexBridge,
or retain another domain model. UI-local history-window counters, current
selection, pending authored recovery, and last effective chrome values are
coordination state permitted by this contract.

`ShellWidget(QWidget*)` requires a running `FrontendSession` owned by its
implementation and creates all visible child panes on Qt-main. Destruction
removes application event filters/notifiers before child teardown. Its
`eventFilter(QObject*, QEvent*)` returns the middle region's decision for
eligible wheel events and otherwise preserves Qt's normal dispatch. Graph
notifications are frame-coalesced only after worker reduction. An ordered
queue projects at most eight distinct ordinary conversation rows from latest
NodeGraph state per 16 ms GUI pass; any remainder schedules exactly one later
nonzero-delay pass, while authoritative structural changes and removals retain
their exact handling. Shell never waits for graph access and never clears user
input merely because a wake write failed after queue admission.

### DTO identity and value types

- `ui::ThreadListSnapshot` / `ThreadListRow` are complete sidebar values.
- `ui::InspectorSnapshot` and its Plan, Agents, Changes, Requests, and State
  children are complete current Inspector values.
- `middle::ConversationSnapshot` / `TurnSection` / `VisibleCardData` are the
  complete retained conversation presentation.
- `AuthoritativeItemKey`, `TurnPlanKey`, and `LocalPromptKey` are stable visual
  identities; their `stableKey` encoding is the QWidget reconciliation key.
- `CardPayload` is a closed variant of the established specialized card DTOs.
  Unknown protocol alternatives use `GenericActivityData`, retaining safe
  bounded visible detail but not a raw protocol authority.
- `AttachmentDraft` is user-owned local editor input until action admission.
  NodeGraph `Attachment` values are moved copies owned by an admitted command.

Every DTO equality operator is part of the no-op contract. Adding a field that
does not influence visible presentation must not force a widget refresh; such
fields belong in adapter control metadata instead.

## Data contracts

### Thread list

`ui::ThreadListSnapshot` is a complete value for one refresh:

- `selectedThreadId` is the canonical currently selected thread or empty;
- `providerReady` requires both a connected transport and provider state
  `ready`;
- `canControl` additionally requires controller role;
- `roots` contains every reachable confirmed root in canonical relation order,
  followed by any temporarily unreachable canonical thread so paging or a late
  owner cannot make a selectable thread disappear.

Each `ThreadListRow` carries canonical ID, display title fallback, cwd, status,
created/updated/recency values, effective last activity, pending count,
archive state, pending-prompt acknowledgement state/deadline, and ordered
children. Effective last activity is the maximum of provider activity,
update/recency, and admitted local prompt activity. The adapter folds confirmed
and optimistic turn order into effective `recencyAt`; the widget owns the
Alphanumeric, Created, and Recent comparators, expansion, pending-prompt
animation, selection visuals, context menus, and row QWidget identity.

### Conversation

`middle::ConversationSnapshot` is the complete currently retained activity
window for one thread. The adapter must never provide a prefix or suffix that
it knows belongs to an unfinished authoritative hydration.

- `threadId` selects the per-thread scroll/follow state.
- `sections` are ordered Turns. Their stable key is the length-delimited
  `turn:<thread-length>:<thread><turn-length>:<turn>` form used by the old
  projection, preventing ambiguous concatenation.
- `rootCardKey` identifies the real opening You item even when it lies before
  the retained 80-item suffix. That root is pinned into the snapshot without
  consuming the activity budget.
- `hiddenAuthoritativeItemCount` excludes pinned roots and local optimistic
  prompts.
- `hasMore` is true for retained hidden items or a provider continuation.
- `activeTurnId` is canonical active-turn identity. A locally admitted pending
  new Turn is visually active until provider acknowledgement.

All stable card identities for the selected 80-item window (and each explicitly
requested next 80) become rows in the thin Qt model. Passive rows are measured
and painted by the bounded delegate without QWidget construction. Only rich
rows in the initial viewport plus overscan are created and laid out beneath the
hidden staging host before one final frame is exposed. Scrolling may release an
offscreen rich editor after saving stable-keyed local interaction state; no
placeholder remains. A new selected-thread row is indexed even while the user
is paused above it, but offscreen insertion performs no QWidget work and exact
anchor restoration prevents vertical or horizontal movement.

`VisibleCardData::key` is visual identity. Canonical items use thread/turn/item
identity; a prompt that began locally keeps its process-wide `LocalPromptKey`
through materialization. `target` is a narrow opaque action/lifetime token for
the integration callbacks. Existing widget code may retain or return it, but
must never read the graph through it. `activeWork` is a canonical presentation
fact for delayed-result cards; it drives the established emphasized border
without making the card infer lifecycle from display strings.

`UiStyle` is the canonical semantic palette boundary. Equivalent family roles
share the documented OKLCH lightness and chroma targets and vary only by the
existing family hue. Code that paints semantic state or card identity consumes
the centralized fixed-hex tokens rather than introducing local RGB variants.

### Composer and settings

The composer owns draft text, attachment presentation, focus, keyboard rules,
and geometry. `Actions::submit` is called with a copy/move of current authored
input, and returns `true` only after exact queue admission. The composer clears
the draft only on `true`.

Send is eligible only when the draft is nonblank, the bridge/provider is ready,
the client controls the provider, and the visible destination is a valid new
draft or hydrated thread. A canonical active Turn changes the action label to
Steer and sends `turn/steer`; otherwise it sends `turn/start`. Stop targets the
exact active Turn. No action reconstructs its target from prompt text or a
later selection.

Settings values are canonical provider data plus local touched-field state.
Catalog updates are independent of selected-thread settings. Repeating the
same identity/revision/catalog performs no control reconstruction or style
work.

### Inspector

`ui::InspectorSnapshot` contains current values for Plan, Agents, Changes,
Requests, and State. It is not another model: the adapter constructs it from a
short current graph read and the pane retains only render-comparison values
and local expansion/scroll state.

The Changes projection carries the selected thread workspace, bounded unique
working directories from both command and file-change items, and bounded
changed-path hints. File-change working directories are required when the
thread workspace is a parent of the actual repository; dropping them makes the
existing asynchronous Git viewer incorrectly report no repository or no
changes.

Request rows carry the exact canonical interaction ID, kind, safe display
facts, provider generation, and actionability. Review/Accept/Reject resolve
that exact live interaction. Agents are a UI-only grouping of canonical
protocol items; grouping never removes or merges graph nodes. Changes retains
the established asynchronous local Git provider. Protocol diagnostics use a
separate bounded metadata append because chronological diagnostics cannot be
derived from current graph state; they never become application authority.

## Temporal usage recipes

### Select and hydrate a thread

1. The ThreadPane callback supplies the visible canonical ID.
2. Shell resolves it to one current `NodeRef` and records the selection.
3. ThreadPane selection/chrome may update immediately.
4. If the selected graph thread is not display-ready, issue one Hydrate action.
   Do not project delta-created provider fragments.
5. If a populated conversation is already displayed, keep that surface stable
   while the new thread hydrates. If no conversation is displayed, a stable
   loading empty state is allowed.
6. Once hydration has usable authoritative IDs/types, obtain one complete
   snapshot and call `ConversationView::reconcile` once. Refresh the selected
   Inspector state from the same ready boundary.

### Receive a graph change

1. Detach removals synchronously.
2. Route only identities relevant to ThreadPane, selected conversation,
   visible Inspector behavior, and effective chrome.
3. Union streaming identities in arrival order for one display frame.
4. Project latest current DTOs for no more than eight distinct ordinary
   conversation rows in that pass; retain any remainder for one later 16 ms
   pass and stop scheduling as soon as the queue is empty.
5. Project each other affected surface only when its explicit dependency was
   addressed.
6. Let the receiving view compare stable identities and values. Repeating the
   same DTO must perform zero presentation work.

### Load 80 more activities

1. Increase the selected thread's requested and effective window by 80.
2. If retained graph history satisfies it, project immediately without a
   provider call.
3. Otherwise send one exact `LoadHistory` action only when the provider reports
   more history.
4. Preserve the stable row and exact pixel anchor while the expanded complete
   snapshot is reconciled. If the first visible Turn root was pinned solely to
   own the old bounded suffix, anchor the first retained activity instead,
   because newly revealed siblings are inserted after that owner.
5. Never expose reserved empty space followed by delayed cards.

### Admit and acknowledge a prompt

1. Capture the visibly selected exact thread and active Turn before admission.
2. Attempt one typed action. On rejection return `false` and retain the draft.
3. On admission prepare the item view's local-prompt anchor behavior and clear
   the draft once.
4. Render the pending normal or steering You card under its canonical owner,
   with pending status and delayed feedback animation.
5. Unrelated items may arrive without changing that ownership or anchor.
6. When the authoritative user item arrives, keep the stable visual key and
   local interaction state, update only that row/editor, and send one
   prompt-materialized acknowledgement for the exact prompt node.
7. Stop pending feedback on the correlated successful request acknowledgement
   or definitive failure. Keep the settled optimistic card until authoritative
   item materialization; never dual-send or infer acknowledgement from matching
   text.

## Compatibility evidence

Focused adapter, established-widget, conversation-card, shell-integration,
protocol-updater, and worker-logic tests currently pass. A real CodexUI build
connected to the officially running bridge was recorded on Xvfb with the long
`CodexUI - Minimal architecture` thread. The pre-fix sequence exposed partial
provider fragments and a loading/empty text oscillation. The corrected cold
sequence holds one stable loading surface and changes directly to one complete
parented mixed-card viewport; active command and response updates then mutate
the completed surface in place.

Populated-thread switch staging now preserves the outgoing conversation,
heading, and Inspector until the incoming complete snapshot is ready; the
focused temporal regression passes. A fresh full configure/build and all 17
native tests pass, as do the independently configured 5-test nodegraph suite,
the 83-test WebUI suite, and the six affected suites under ASan/UBSan.

The final full-application movie uses the rebuilt Debug binary, an isolated
Xvfb display, and the officially running bridge. It records cold selection of
the long `CodexUI - Minimal architecture` thread, a switch to a second long
thread, and incoming activity while scrolled above the bottom. The cold load
exposes one complete parented layout, the thread switch exposes no partial
replacement, and the paused viewport keeps identical card positions while the
new content is materialized below it. Automated black-frame analysis of the
conversation region found no blank interval. This evidence verifies the
contract; it does not weaken or redefine it.

A second 30-fps full-application recording validates authored input against
the isolated workspace bridge using the small `GPT-5.6-Sol` model and Low
reasoning. It records a normal prompt from editable draft through admission,
optimistic/authoritative You-card materialization, active Turn state, running
command output, and final response. During that active Turn it records a
steering draft, enabled Steer action, admission, one steering You card under
the same Turn, draft clearing only after admission, and exactly one final
`STEERING ACKNOWLEDGED` response. The active Turn and delayed command retain
their emphasized borders, Stop/Send/Steer eligibility follows canonical state,
and no empty or unparented intermediate conversation frame appears. The proof
artifact is `prompt-and-steering-proof.mp4` in the qualification capture
directory. The official bridge was also tried first, but correctly withheld
controller authority from the second UI; no prompt was sent through that
uncontrolled connection.

No remote or GitHub operation is used to maintain this document.

### Qt item-view qualification (2026-09-10)

The final `QAbstractItemView` implementation was exercised through the complete
Debug application on isolated Xvfb display `:99`, connected to one
workspace-local `codex-bridge` and app-server that remained alive across every
scenario. Obsolete retained-widget recordings were removed before replacement.
Current movies and contact sheets are under
`../../build/codexui-adapter-qualification/capture/qt-virtualized-final/`.

The recordings prove atomic selection of a copied 10,023-event thread; exact
paused anchoring while Load 80 inserts earlier rows, including a Turn owner
pinned outside the old activity suffix; repeated heterogeneous history sweeps;
outer and nested scrolling during a 1,200-line command;
running-to-completed command transition; normal prompt and steering admission;
authoritative steering acknowledgement below a paused viewport; delegate
promotion, selection/copy, fold/unfold and visible focus; command approval
rejection; and Plan-mode user-input Review, selection, submission, and final
acknowledgement. The temporary copied session, state database, configuration,
second UI, and paging bridge were deleted after the paging recording, and the
rejected approval probe created no file.

During an active-only 60-fps interval with continuous outer scrolling, mean
decoded-frame luminance deltas were 2.211289 in Conversation, 0.000012 in
ThreadPane, 0.000003 in Inspector, 0 in the shell header, and 0.000689 in the
settings/composer region. The small non-conversation values are encoding/cursor
noise; no unrelated pane content moves. Exact identity, anchor, widget-count,
offscreen, focus, accessibility, and event-loop limits remain asserted by the
deterministic tests rather than inferred from lossy video.

The persistent Debug and integrated ASan/UBSan builds each pass all 19 native
suites; ASan/UBSan reports no finding. The supported independent NodeGraph,
typed-queue, and worker TSan boundary passes 5/5 without a race report. WebUI is
unchanged and its release gate passes 85/85 tests, performance profiling,
production bundling, Chromium responsive/focus qualification, and relocatable
artifact verification. Full ownership, delegate/editor decisions, benchmark
tables, movie names, and remaining limitations are recorded in
`qt-virtualized-conversation-view.md`.
