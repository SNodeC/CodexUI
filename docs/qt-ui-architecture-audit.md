# CodexUI Qt UI architecture and usability audit

**Status:** complete read-only architectural audit; no remediation is implemented or approved here.
**Audit date:** 2026-09-12 (Europe/Vienna)
**Repository:** `/home/voc/projects/drafts/CodexUI/codexui`
**Revision reviewed:** `629660e8882bddc7473ee5f1aab90c755ee6edd4` (`master`) plus the pre-existing uncommitted worktree recorded below.
**Completion rule:** implementation belongs in a separate thread and may begin only after this audit is approved.

## Executive result

| Gate | Result |
|---|---|
| Source/config input inventory at completion | **185/185 reviewed and individually listed**: 184 tracked baseline files plus `AGENTS.md`, which appeared untracked after the recorded baseline and was preserved/audited |
| `rg --files` verification | Baseline **181/181**; completion **182/182 non-artifact paths**; the three additional tracked paths are hidden `.github/workflows/ci.yml`, `.gitignore`, and `web/.gitignore` |
| Native production `.cpp`/`.h` | **83/83 reviewed** |
| Native Qt tests/benchmark `.cpp` | **20/20 reviewed** |
| CMake files | **4/4 reviewed** (three native/Qt relevant; one browser packaging/parity relevant) |
| `.ui` / `.qss` files | **0 / 0**; QSS is embedded principally in `UiStyle.cpp`, with local widget style sheets elsewhere |
| Architecture/behavior Markdown | **17/17 reviewed** |
| Other tracked resources, screenshots, CI, and browser-parity files | **all reviewed or explicitly classified in the ledger** |
| Findings | **48 total: 1 Critical, 16 High, 31 Medium, 0 Low** |
| Audit edits | **0 production LOC, 0 test LOC; this document is the one permitted artifact** |

The known Markdown shift is confirmed as a systemic architecture defect, not a pixel-offset defect. `ConversationPassiveDelegate` and `ConversationCard`/`MarkdownTextView` independently implement the same card shell, Markdown layout, geometry, affordances, hit testing, and presentation state. Materialization replaces one rendered and semantic surface with another. Sharing constants or a `QTextDocument` cannot make a direct `QAbstractTextDocumentLayout` paint path and a `QTextBrowser` widget path one renderer.

The intended architecture is otherwise clear: the app-server owns provider facts and persistence; one `NodeGraph` owns current native protocol state; the worker is its sole writer; Qt derives projections and owns only local interaction/presentation mechanics. The principal violation is that virtualization owns **appearance** as well as **presence**. The target invariant is: **one visual concept has one renderer and one semantic tree for its visible lifetime; virtualization decides only whether that component is resident.**

## Audit controls and worktree protection

Before diagnostics, the existing dirty worktree was recorded and treated as user-owned, unverified work:

```text
 M src/codex/middle/ConversationCards.cpp
 M src/codex/middle/ConversationView.cpp
 M src/codex/middle/ConversationView.h
 M tests/codex/ConversationCardsTest.cpp
 M tests/codex/ConversationVirtualizationTest.cpp
 M web/src/app/App.tsx
```

Baseline diff: native production **+132/-59 (net +73)**, native tests **+278/-12 (net +266)**, browser production **+1/-1 (net 0)**. The native diff adds a dynamic `keyboardFocusVisible` property/state, coordinate offsets, duplicated passive status colors, and renderer-parity pixel tests. It is classified as an **architectural workaround/unverified experiment**, not an accepted correction. Nothing was discarded, staged, committed, installed, restarted, or pushed.

After that baseline was captured, an untracked `AGENTS.md` appeared in the shared working directory. It was not created by this audit, is not an audit artifact, and its provenance cannot be established from Git. It is preserved as externally owned work, was read before continuing, and is included in the completion ledger. Its one-renderer/deletion-first instructions agree with, rather than alter, the independently established diagnosis.

Four independent audit tracks were delegated, followed by an independent adversarial review:

1. rendering, layout, styling, DPI, and virtualization;
2. interaction, focus, clipboard, keyboard, and accessibility;
3. state flow, protocol acknowledgement, optimistic behavior, and ownership;
4. performance, lifecycle, tests, LOC, and source history;
5. adversarial challenge of the root-cause proof, deletion design, estimates, and regression gates.

The primary audit independently re-ran inventory, source, LOC, history, and diagnostic checks; inspected every file class in the ledger; spot-checked every accepted source reference; reconciled cross-track disagreements; and rejected one proposed prompt-acknowledgement finding. The local-prompt/authoritative-card key alias in `NodeGraphUiAdapter.cpp:2173-2203` deliberately matches `WorkerLogic.cpp:1376-1406`; without a counterexample it is not a defect.

### Evidence classification

* **Source-proven** means the mechanism, duplicate owner, missing invalidation, stale contract, or test limitation follows directly from the current working tree and relevant history.
* **Runtime-observed** means a diagnostic reproduced the behavior in the named environment; it is not generalized beyond that environment.
* **Runtime hypothesis/test gap** means source makes a failure possible or a contract is absent, but no product failure was established. These are never described as confirmed defects.
* Severity reflects architectural/user risk, not certainty. Every counted item states its evidence class. The unresolved-unknowns section is not included in the 48 findings.

## Architectural model

### Complete native data flow

```text
app-server / codex-bridge transport
  -> ClientRuntime on the SNode.C worker thread
  -> protocol decode and method disposition
  -> ProtocolUpdater (authoritative protocol facts) and WorkerLogic
  -> one NodeGraph transaction (worker is sole writer)
  -> GraphChanged / UiEffect / WorkerStopped through bounded SPSC queue + eventfd
  -> WorkerMailboxReceiver / FrontendSession on Qt main thread
  -> ShellWidget coalescing, selection routing, and retry scheduling
  -> NodeGraphUiAdapter projection (graph lock released before QWidget calls)
  -> UiViewState / MiddleTypes snapshots and ConversationItemModel
  -> ThreadPane, ConversationView, InspectorPane, ComposerPane, dialogs, or chrome
  -> Qt layout/style/paint and accessibility tree
  -> user signal / callback
  -> NodeAction or RuntimeAction through the reverse bounded queue
  -> WorkerLogic admission/correlation -> ClientRuntime app-server request
  -> response/notification -> NodeGraph transaction
  -> correlated acknowledgement/failure projection -> final visual state
```

Local Git changes intentionally form a separate path: `QFileSystemWatcher`/two-second refresh -> `GitDiffProvider` global-thread-pool libgit2 job -> generation-checked `GitDiffSnapshot` -> embedded `DiffViewer` or review window. The browser is a separate process/toolkit path (`BrowserFrontendSession` -> protocol normalizer -> presentation model -> React). It cannot share a renderer with Qt, but equivalent semantic policy should use shared fixtures instead of copied rules.

### Authoritative owners and current violations

| Concept | Intended single owner | Current derived/render owner | Audit result |
|---|---|---|---|
| Provider thread/turn/item/request facts and persistence | app-server | `ProtocolUpdater` -> `NodeGraph` | Sound boundary except raw/canonical status disagreement downstream. |
| Current native state, ordering, relations, revisions | `NodeGraph`; worker sole writer | immutable reads through `NodeGraphUiAdapter` | Sound lock/thread invariant; some projections duplicate ordering/classification. |
| Transport and correlation | `ClientRuntime`/`WorkerLogic` | `FrontendSession` typed delivery | Generally sound; provider-history failure does not clear the UI wait flag. |
| Visible thread selection | Qt user state in Shell/ThreadPane | adapter consumes selected `NodeRef` | Restoration/retry mechanisms fragment focus/selection ownership. |
| Local prompt admission/queue | `WorkerLogic` graph nodes | Shell/composer/conversation render | Mostly authoritative; new-thread creation also has Shell/ThreadPane overlays. |
| Conversation ordering | graph relations/provider order | adapter full, exact-row, tail and Shell delta algorithms | Violated: equivalent placement logic exists in several paths. |
| Card presentation and geometry | one card renderer/layout | passive delegate, live widget, Inspector variants | **Violated.** |
| Virtualization | `ConversationView` residency policy | also selects renderer and semantic surface | **Violated. Presence policy changes appearance.** |
| Card interaction/focus/accessibility | one card component | passive hit regions/synthetic forwarding and rich child widgets | **Violated.** |
| Scroll/follow/anchor | `ConversationView` | nested text widgets own additional scroll/follow state | Explicit but duplicated wheel arbitration and incomplete focus persistence. |
| Inspector presentation | `InspectorPane` | adapter copies all rows; pane creates all widgets | Documented bound removed; third Markdown/card shell. |
| Thread rows | `ThreadPane` | widget per loaded row plus delegate overlay | Documented bound removed. |
| Composer draft/attachments | Composer/Shell local draft | editor/settings/dialogs | Clear local fact owner; fragile geometry cache and synchronous rebuild. |
| Turn-setting provider facts | graph/catalog | `TurnSettingsWidget` also maps/serializes policy | Business rules leak into widget. |
| Pending requests | graph interaction/one policy | Shell, adapter, Inspector, dialog | Duplicated classification/control. |
| Local diff snapshot | should be one snapshot owner per repository/scope | embedded and review providers | Pipeline and documents multiply. |
| Animation/reduced motion | should be one application policy | each pane/timer decides | No Qt reduced-motion owner. |
| Styling/font/DPI epoch | system/application style | startup QSS plus fixed local values/caches | No authoritative invalidation epoch. |

### Creation-to-destruction lifecycles

**Application.** `main.cpp` creates `QApplication`, configuration/session, `MainWindow`, and `ShellWidget`; `FrontendSession::start()` creates worker/runtime machinery; eventfds bridge worker messages to Qt. QObject children die with parents. `ShellWidget::Impl` clears handlers on destruction; session shutdown/wait owns the worker. A later `setRuntimeStoppedHandler` in application setup overwrites the Shell handler, making notification ownership implicit (M-05).

**Graph nodes.** Worker transactions create/update/remove `shared_ptr<Node>` identities. Removal unlinks and retires nodes. Qt receives direct references or bounded rescan work; attachments are supposed to be detached on Qt-main before retirement acknowledgement. Current production has no caller that sets the declared opaque Qt attachment; the attachment path and a large disabled test block are residue (H-08).

**Conversation rows.** Adapter projection creates stable keyed rows; `ConversationItemModel` owns model storage/indexes and `ConversationHeightIndex` owns estimated/measured vertical positions. `ConversationView` selects a range. Passive-eligible rows stay model-only and are painted by its delegate; an interaction/focus/visibility event creates `ConversationCard`, transfers cached document/state, positions it, and suppresses delegate content. Release serializes collapse/selection/command scroll state, deletes the widget later, and resumes passive paint. That renderer/semantic replacement is the critical defect. Row trim/removal/thread forgetting does not consistently retire all keyed caches.

**Prompts.** Composer submit captures text/settings/attachments -> Shell resolves the explicitly selected thread -> action admission creates a local-prompt graph node -> one unacknowledged prompt per thread dispatches -> a correlated result marks it accepted/failed or reparents it to explicit recovery. Provider notification creates the authoritative item while retaining the local presentation key. No automatic resend is intended.

**Requests.** Server request -> interaction node -> Inspector/request button/dialog projection -> user accept/reject/structured response -> reverse action -> worker correlation -> node retirement/final refresh. Modal content is built from payload; stale/disconnect/duplicate and keyboard semantics are not fully qualified.

**Diff.** Each embedded/review surface constructs its own provider, polling timer, documents, highlighters, watcher set, and global-thread-pool requests. Generation checks prevent stale results from applying, but obsolete jobs still run; destruction relies on receiver/context suppression rather than work cancellation.

## Root-cause proof and deletion-based target

### Why the visible defect exists

`ConversationPassiveDelegate` (`ConversationView.cpp:362-775`) creates a `PassivePresentation`, calculates card/header/body rectangles, paints shell/status/copy/disclosure/content, lays out a `QTextDocument`, and performs pointer/link hit testing. The live path constructs `ConversationCard` (`ConversationCards.cpp:1611-2502`) whose `MarkdownTextView` is a `QTextBrowser` (`:975-1158`) in a different layout/style/paint stack. Materialization/dematerialization (`ConversationView.cpp:2860-3009,3754-3993`) replaces the complete surface. Different document viewports, frames, font/style contexts, layout rounding, child geometry, and paint origins can shift every glyph and control.

### Why the architecture allowed it

The virtualization optimization introduced in `bd2297e` had no invariant limiting virtualization to object residency. It was permitted to recreate presentation in a delegate. Later commits shared values (`0d21950`) and documents (`aab2723`) and added synthetic interaction (`d58020d`) rather than deleting the second renderer. Tests then asserted pixel parity between both implementations. The architecture rewards synchronizing duplicates instead of making divergence impossible.

### Preventing and target invariants

1. One conversation-card shell, Markdown renderer, geometry owner, interaction implementation, focus state, and accessibility subtree exists for every resident row.
2. Virtualization decides only which stable keyed cards exist (viewport plus measured overscan); it never selects a different appearance.
3. Nonresident rows retain canonical data and an explicitly invalidated height estimate, not a separately painted card.
4. Cross-row Turn background/connector paint remains view-owned; it is not a duplicate card renderer.
5. Caches are bounded to retained identities and keyed by content/presentation revision plus width/font/style/DPI epoch.
6. Browser/native implementations remain toolkit-specific but consume the same semantic fixtures for status, requests, ordering, and acknowledgement.

### Exact future consolidation/deletion scope (not implemented)

Delete the passive eligibility policy, `PassiveBlock`, `PassivePresentation`, `ConversationPassiveDelegate`, its 128-document LRU, delegate-specific card/header/status/Markdown sizing and painting, passive control/link hit testing, synthetic mouse forwarding, document-transfer bridge, duplicate pointer state, delegate paint branch, and renderer-parity tests/documentation. Reuse existing `ConversationCard`/`MarkdownTextView` for viewport plus overscan. Retain `ConversationItemModel`, `ConversationHeightIndex`, stable key/state restoration, the materialization range policy, and view-owned cross-row Turn painting. Do **not** add pooling initially, an always-materialized exception, a third facade, or a reconciliation pass.

Current production code LOC for the six conversation renderer/view files is **7,126**. A conservative first phase should remove 850-1,200 implementation LOC and add at most 100-150, for a **net production decrease of 750-1,050 LOC** (six-file target **6,076-6,376**; repository native-production target **33,204-33,504** from 34,254). Tests are a separate delta: delete parity/synchronization tests and add smaller invariant, lifecycle, behavior, accessibility, and bounded-object tests. If production LOC does not decrease, or any passive appearance/hit-test path remains active, stop.

### Regression surface and proof required

The phase touches all ten card kinds, Turn decoration, Markdown links/selection/copy, fold state, command nested scrolling/follow-tail, images, file-change links, plans, reasoning, local-prompt animation/recovery, streamed updates, height estimates, anchors, follow-tail, history paging/trimming, focus traversal, accessibility, font/style/DPI changes, thread switching, and object/document bounds. Required proof is: source absence checks; full non-short-circuit native suites; DPR-correct 1x/1.25x/1.5x/2x tests; activated X11/XWayland and Wayland; font family/size and runtime style; reduced motion; keyboard/screen-reader/clipboard/link/drag selection; representative 320/1,280/10,000-row workloads including huge Markdown/commands/images/files/streaming; p50/p95/max layout/paint/construction, object/document census and RSS; user-visible checksums; and user visual confirmation. Passing current pixel-parity tests is not proof.

## End-to-end concept coverage

| Concept | Ingress / canonical state | Projection / model | Renderer and geometry | Interaction -> acknowledgement/final state |
|---|---|---|---|---|
| Application shell | transport/config/runtime and graph root | Shell chrome reads | `MainWindow` -> `ShellWidget` top bar + `MiddleRegionWidget` | connect/disconnect/controller actions -> runtime result/effect -> status/notice; local single-flight state is inconsistent. |
| Thread panel | graph threads/changes | adapter `threads`/`threadRow` -> snapshot | `QListWidget` plus one row QWidget each | select/load-more/context/sort -> Shell action -> graph response; selection is user-owned. |
| Thread hierarchy | parent/child/agent relations | adapter depth/parent IDs | flat list, indentation, disclosure | Left/Right/disclosure alter local expanded set; no tree accessibility semantics. |
| Thread sorting | provider/root order and timestamp/name | ThreadPane comparator/criterion | full list reorder/rebuild | local sort choice -> refresh; only roots sort locally and selection restoration is fragile. |
| Thread timestamps | graph `updatedAt`/activity | humanized row metadata | row QLabel | no action; age refreshes only with pane refresh and can become stale. |
| Thread context menus | stable ID under pointer | lookup without selecting | transient `QMenu` | reload/rename/fork/archive/delete -> node/runtime action -> graph/result/notice. |
| New-thread dialog | local creation draft/catalogs | `NewThreadDraft` | modal fields/workspace chooser | accept -> Shell + ThreadPane optimism + worker state -> create on first prompt -> confirm/fail. |
| Fork dialogs | selected graph thread | quick default/options draft | confirmation/options dialog | fork -> correlated result -> new/selected thread or error notice. |
| Ephemeral threads | creation property -> provider thread fact | graph/thread snapshot | ordinary row/conversation | provider owns lifecycle; dialog clears a user name without explanation. |
| Conversation panel | selected thread relations/history | adapter full/exact/tail -> item model/height index | item view, Turn paint, duplicate passive/live cards | scroll/materialize/history/select -> local state or provider action -> projection/reflow. |
| Passive/materialized lifecycle | same `VisibleCardData` | separate passive projection plus live card | delegate document paint swaps to QWidget/browser | hover/focus/click can swap; release swaps back; critical visual/semantic change. |
| Markdown | agent/reasoning/request string | passive cache or card document | direct document-layout draw, `MarkdownTextView`, Inspector third view | select/copy/link -> clipboard/open URL; behavior differs by path. |
| Command execution/output | command item/status/output deltas | `CommandExecutionData` | card + nested `CommandOutputView` | fold/select/copy/inner scroll/follow -> local state; streaming status/output -> apply/settle. |
| File changes | provider item or local libgit2 snapshot | card data / `GitDiffSnapshot` | card links and separate diff/review editors | activate -> open diff/repository; failures may be silent. |
| Images | URL/path/revised prompt/status | `ImageGenerationData` | synchronous thumbnail/cache | copy/open/fold; loading/decode error and DPR invalidation incomplete. |
| Plans | plan item/current graph plan | card data; different Inspector projection | card rows and Inspector frames | streamed status/fold; duplicated outcome normalization. |
| Reasoning | reasoning summary/content deltas | `ReasoningData` | Markdown passive/live | fold/select/copy/link; streaming reparses/reflows. |
| Agent activity | child thread/status/activity | card data; Inspector agent snapshot | conversation card and separately rebuilt Inspector frame | fold/copy/inspect; Inspector streaming rebuild loses identity. |
| Generic activity | retained known/unknown item fields | `GenericActivityData` | generic passive/live card | fold/copy where present; fallback presentation is lossy. |
| Inspector | selected-thread projections | `InspectorSnapshot` | tabbed scroll areas, all QFrames, Markdown, diff child | tab/hide/refresh/request actions; documented row bound absent. |
| Requests | server interaction nodes | adapter + Shell classifier + policy | top button, Inspector row, modal form | accept/reject/respond -> worker correlation -> retire/failure; policy duplicated. |
| Composer | local text/attachments/selected thread/settings | composer/editor | bottom overlay + conversation trailing spacer | Send/Steer -> immediate admission/pending/clear -> accepted/failed/recovery; Stop -> interrupt. |
| Turn settings | catalogs/provider values/local touched draft | widget-built option model | toolbutton/menu/combos | change -> draft -> next-action serialization; business policy resides in widget. |
| Notices | typed effect, runtime errors, fallback fields | Shell/MiddleRegion string | transient banner/label | next state/timer hides; no accessible live announcement; dual state path. |
| Loading/empty/error | connection/hydration/history/read-retry/effects | Shell flags + conversation state | overlay, empty label, notice, failed local prompt | result/retry changes state; history failure can remain permanently awaiting. |
| Splitters/responsive panels | QSettings/local visibility/window geometry | MiddleRegion/Shell local state | splitters, fixed minima, overlay sync | drag/show/hide -> save/restore/reflow; 1100x700 minimum limits narrow use. |

### Conversation-card inventory

| Kind / visible variant | Canonical source | Body renderer | Interactions | Principal concern |
|---|---|---|---|---|
| User message (“You”) | authoritative user item | Markdown passive/live | selection, Copy; steering label | whole shell rerenders; empty affordance divergence. |
| Local pending/steering/recovery prompt | local prompt graph node | Markdown passive/live | Copy/recovery; sweep | animation and acknowledgement cross owners; no reduced motion. |
| Agent/Codex update/final answer | agent item + final flag | Markdown passive/live | Copy, selection, links | known glyph shift; duplicated header/status. |
| Command execution | command item/output/status | live nested text plus passive shell/summary | fold, copy/select, inner scroll/follow | wheel state and dematerialized cache. |
| Agent activity | child-agent facts | composed text/status | fold/copy | separately implemented in Inspector. |
| Reasoning | reasoning item | Markdown passive/live | fold/select/copy/link | streaming/document-transfer path. |
| File changes | file-change item | bounded list/links | fold/open/copy | keyboard/failure semantics; diff work duplicates. |
| Image generation | image item | thumbnail/labels | fold/open/copy | synchronous decode and non-DPR cache. |
| Plan | plan item/steps/status | structured rows | fold/copy | card/Inspector projections differ. |
| Generic activity | remaining item kinds | text/metadata | fold/copy when present | lossy fallback and duplicate presentation. |

## Severity-ranked findings

### Critical (1)

**C-01 — Two complete conversation-card renderers are swapped during materialization (source-proven; runtime visual defect reported).** Evidence and root-cause proof are above. The swap affects shell, Markdown, glyphs, geometry, hit testing, focus, accessibility, and actions across every card type. The only acceptable first direction is deletion of the passive renderer while preserving bounded viewport/overscan residency.

### High (16)

**H-01 — Materialization changes interaction and accessibility trees (source-proven).** Passive rows expose only model text/enabled/selectable roles (`ConversationItemModel.cpp:164-215`); copy/fold/link are pointer hit regions. Live cards create named focusable buttons and text controls (`ConversationCards.cpp:129-295`). Passive controls can paint/hit-test even when rich cards hide them (`ConversationView.cpp:495-525,611-625`; `ConversationCards.cpp:1906-1925`).

**H-02 — Conversation presentation caches lack complete authoritative invalidation/bounds (source-proven).** Height, collapse, interaction, and command-output maps (`ConversationView.h:336-347`) survive full reconciliation and history trimming (`ConversationView.cpp:1069-1093,2067-2100`); targeted removal omits command output (`:1482-1513`); thread forgetting clears only history/scroll (`:1921-1924`). Height lacks content/style/font/DPR epochs. Long-session growth is a runtime hypothesis; missing retirement paths are proven.

**H-03 — Status has multiple normalizers and a demonstrated alias mismatch (source-proven).** `ProtocolUpdater::statusFromValue` accepts `complete`/`succeeded`, `error`/`blocked`, and `cancelled`/`stopped`; downstream prefers raw strings while `UiStatus::classifyStatus` and browser status code omit part of that vocabulary. Raw `cancelled` can be canonical `Interrupted` but present as unknown. One canonical fact must cross the protocol boundary.

**H-04 — Failed provider history can permanently suppress retry (source-proven state-machine gap).** `historyPageAwaitingProvider` is set on admitted action and cleared on rebind or authority/structure progress. A runtime request failure only notices/removes correlation; it does not clear this flag. “Load more” can remain disabled until unrelated state changes.

**H-05 — Conversation ordering/placement is implemented in parallel full, exact-row, tail, and Shell-delta paths (source-proven duplication).** Adapter `conversation`, `rowChange`, `tailCard`, and `promptMaterialization` plus `ShellWidget::commitPendingPanes` repeat hierarchy, visibility, placement, and prompt/card transitions. Examples are tested, but there is no one projection algorithm reused by full and incremental application.

**H-06 — Optimistic new-thread presentation has three owners (source-proven duplication).** Shell holds creation/selection-retry state, ThreadPane holds an optimistic row overlay, and WorkerLogic owns admitted draft/prompt graph lifecycle. Confirmation/failure/reconnect reconcile these rather than derive from one fact owner.

**H-07 — Pending-request classification/control is duplicated (source-proven).** Method-to-kind, actionability, recovery, payload shaping, button state, Inspector rows, and dialog responses span Shell helpers, adapter, `PendingRequestPolicy`, Inspector, and dialog. One interaction projection/policy should be authoritative.

**H-08 — Opaque Qt node-attachment lifecycle is production residue and 3,248 test lines are disabled (source-proven).** `QtNodeAttachment.h`, attachment messages, retirement scans, and CMake inclusion remain, but no production caller sets the attachment. `ConversationCardsTest.cpp:4128-7375` puts 25 tests behind undefined `CODEXUI_DIRECT_GRAPH_WIDGET_TESTS`, including discarded APIs.

**H-09 — Documented Inspector bounded virtualization was removed (source/history-proven).** `docs/ui-behavior.md:619-627` promises 48 rows plus two overscan. Adapter `inspector` (`NodeGraphUiAdapter.cpp:1127-1605`) scans/copies all rows and `InspectorPane.cpp:852-1070` creates every widget. `ded5055` removed prior bounds; tests use one/two rows and assert no bound.

**H-10 — Inspector contains a third Markdown/card-shell and destroys descendant identity on updates (source-proven).** Its Markdown/copy/disclosure/status controls (`InspectorPane.cpp:147-258`) duplicate conversation components. Agent patching deletes/rebuilds child trees (`:286-297,409-523,946-1013`), losing selection/focus/control identity; expanded-agent identity is not pruned.

**H-11 — Documented thread-row virtualization was removed (source/history-proven).** `docs/native-ui-ux-qualification-inventory.md:68-72` promises visible rows plus overscan. `ThreadPane.cpp:807-1072` creates an item and QWidget subtree for every loaded row. `6c1ed14` deleted visibility/overscan/work-budget code. Tests cover paging, not widget counts.

**H-12 — Embedded/review diffs duplicate provider/polling/presentation and lack in-flight backpressure (source-proven).** Each owns a provider and two-second poll (`DiffViewer.cpp:328-425,551-675`). Each refresh starts a global-pool task; generation checks discard results but do not cancel work (`GitDiffProvider.cpp:386-419`). Review eagerly builds unified, left, and right documents/highlighters (`DiffViewer.cpp:507-525`).

**H-13 — Composer attachment removal synchronously deletes the signal emitter/siblings (source-proven lifetime hazard; no crash demonstrated).** The clicked handler clears/rebuilds the attachment layout immediately. This risks reentrancy and certainly destroys focus/control identity. A stable model-driven row mechanism should replace rebuild-on-signal without a compensating timer.

**H-14 — Qt has no single reduced-motion policy (source-proven accessibility gap).** Pending sweep, spinners/loading, follow-tail, thread animation, and other timers/animations run unconditionally. Browser CSS supports `prefers-reduced-motion`; Qt has no policy owner.

**H-15 — Test architecture can report qualification while hiding platform/DPI failures (source-proven, runtime-observed).** GUI CTests force offscreen; several mains short-circuit with `&&`; 25 tests are compile-disabled; spinner and parity helpers mix physical grabs/logical coordinates; timing checks straddle timer boundaries. Passing 19/19 at 1x is not platform or architecture proof.

**H-16 — The pre-existing dirty “fix” preserves the defective mechanism and grows LOC (source-proven).** Native production is net +73 and tests net +266. It adds duplicated colors, offsets, a dynamic focus property/state, and two-renderer parity tests. It violates deletion-first and single-owner requirements and must not become the remediation base.

### Medium (31)

**M-01 — Notices have two state paths (source-proven).** Typed `UiEffect` delivery coexists with graph-field fallback scanning in Shell, allowing duplicate/reordered presentation. One stream should own effect delivery/loss semantics.

**M-02 — Active-turn identity is derived from both status and relations (source-proven duplication).** Adapter/Shell helpers independently classify status and inspect active-turn relations. The graph should publish one canonical relation/status invariant.

**M-03 — Plan outcome projection has fallback variants (source-proven).** Conversation and Inspector combine item, turn, and thread status differently; equivalent plans can present differently.

**M-04 — Selection/focus restoration has multiple owners and retry callbacks (source-proven).** Shell, ThreadPane, ConversationView, dialog opening, and pane visibility each retain/rediscover focus/selection; `findChild` and zero-delay retries make final ownership order-dependent.

**M-05 — Runtime stopped-handler ownership is overwritten (source-proven).** Shell installs a handler, later startup replaces it, and Shell cleanup clears the shared callback. Other QObject-context callback lifetimes inspected were generally sound.

**M-06 — Mutating actions lack consistent in-flight feedback/single-flight gates (source-proven gap; repeat outcome partly runtime-unknown).** Stop, connection/controller, request response, reload/archive/delete/fork often depend on queue admission plus eventual notice. Rapid repeat, timeout, stale/duplicate, and disconnect behavior is not comprehensively modeled/tested.

**M-07 — Thread hierarchy/sort/time/pagination rules are incomplete (source-proven).** Roots sort locally while children retain provider order; relative timestamps refresh only with graph changes; Load More has no explicit local pending gate; rebuild/current-index restoration is spread across callbacks.

**M-08 — Thread hierarchy is visually indented, not semantically a tree (source-proven accessibility gap).** `QListWidget` rows/disclosures do not expose tree levels, relationships, or standard expand/collapse actions; Left/Right behavior is custom.

**M-09 — One optimistic thread repaints the whole list viewport every 32 ms (source-proven).** The timer remains active if any loaded row is optimistic/awaiting, including offscreen rows; paint/work is not bounded to an affected visible row.

**M-10 — Composer overlay geometry is manually duplicated and construction-invalidated (source-proven).** Hidden reserve, overlaid composer, trailing spacer, and a `canonicalHeight_` captured via zero-delay callback coordinate one height. Runtime font/style/DPR invalidation is absent; editor maximum height is likewise derived once.

**M-11 — Attachment selection does synchronous filesystem/MIME work and bounds count, not bytes (source-proven).** `QFileInfo`/content probing can block on slow mounts. Fifteen attachments can still be arbitrarily large; partial failure feedback is weak.

**M-12 — Image decode/cache is synchronous and not DPR/screen keyed (source-proven).** Large/offscreen decode can block construction/update; cross-screen scale may reuse inappropriate raster geometry; loading/decode failure feedback is incomplete.

**M-13 — Styling has no authoritative runtime font/style/DPI epoch (source-proven).** Embedded fixed-light QSS and pixel sizes coexist with local style sheets; tooltip rules conflict; one focus border changes 1 to 2 pixels. Caches are not globally invalidated on font/style/palette/screen change.

**M-14 — Nested wheel arbitration is duplicated (source-proven).** `ContentSizedTextView` and `CommandOutputView` repeat gesture-retention/delta state (`ConversationCards.cpp:1207-1261,1368-1399,1555+`); MiddleRegion separately routes ancestors. One mechanism should own propagation.

**M-15 — Conversation reflow uses compensatory mechanisms (source-proven mechanism; failure hypothetical).** Exact resize can retain stale-width estimates; `measureCard` activates nested layouts/removes posted `LayoutRequest`; filters consume layout requests and correct again. This obscures height ownership and may miss later-created descendants.

**M-16 — Copy feedback is visual-only and clipboard transformation is lossy (source-proven).** Clipboard failure/ownership is not surfaced; icon/tooltip feedback lacks announcement; Markdown zero-width cleanup can replace rich MIME with plain text.

**M-17 — Dematerialization preserves partial selection state, not one focus identity (source-proven gap).** Offsets/collapse/command scroll are cached and focused cards may be pinned, but child focus/tab position and selection direction are not a durable single state.

**M-18 — File-change links have incomplete keyboard/failure behavior (source-proven).** Keyboard focus/activation is anchored to a text representation, not each semantic link; failure to resolve/open can be silent.

**M-19 — Dialog semantics/failure states are incomplete (source-proven gaps).** Dynamic request labels are inconsistently buddied, MCP links are mouse-biased, invalid-input focus/announcement is weak, forms are unbounded, and modal data can stale. Ephemeral creation silently clears a typed name.

**M-20 — Turn settings mix policy and serialization into a QWidget (source-proven responsibility violation).** Catalog interpretation, compatibility filtering, touched/default omission, and outbound JSON sit beside controls, preventing independent model-contract testing.

**M-21 — Dynamic UI state lacks a coherent accessibility event stream (source-proven absence).** Pending/accepted/failed, notices, streamed text, expanded state, busy/loading, request arrival, and copy completion update visually without systematic `QAccessible` state/name/value/live events.

**M-22 — The conversation benchmark cannot observe the target defect/design (source/runtime-proven).** Generated rows are passive-eligible, producing zero cards/eight widgets even at 10,000. It has aggregate sweep/peak RSS only, no percentiles, correctness checksum, document count, thresholds, or CTest registration.

**M-23 — Documentation/screenshots encode obsolete or contradictory architecture (source/history-proven).** `ui-behavior` says cards remain materialized offscreen while passive docs disagree; Inspector/thread bounds remain after removal; qualification claims Xvfb/XCB/accessibility while CTest forces offscreen. UI-review PNGs show an older dark “CODEX WORKBENCH” design than current light styling.

**M-24 — Browser/native semantic policy is copied (source-proven duplication).** Separate renderers are necessary, but status vocabulary, request shaping, turn settings, conversation projection, and optimistic state are implemented twice. Parity tests may preserve a shared defect such as missing aliases; shared fixtures should replace duplicative rules where possible.

**M-25 — Diff updates rebuild text/accessibility state (source-proven).** Snapshot application replaces complete document content and scans blocks; review holds three copies. Selection/cursor/scroll/screen-reader position lacks an explicit restoration contract.

**M-26 — Recurring timers lack an idle-work budget (source-proven gap).** Frontend/worker recovery polling, thread animation, conversation animation/settlement, and two diff polls are independently owned. Lifetimes look QObject-safe, but wake/idle-power cost is unmeasured.

**M-27 — Hard minimum geometry limits responsive use (source-proven).** A 1100x700 minimum, fixed control sizes, and splitter sync constrain small displays, magnification, and large fonts despite a checked-in narrow-screen contract.

**M-28 — Inspector/Shell projections scan/copy more state than visible presentation needs (source-proven).** Short try-reads prevent lock blocking, but retries/full collections repeat O(N) work. No startup/hydration/thread/Inspector scan bounds exist.

**M-29 — Long/partial/Unicode data is incompletely bounded (test gap).** Markdown/command output have focused bounds; request forms, agent descendants, diff file count, attachment bytes, paths, multiline labels, Unicode graphemes/RTL/IME lack a complete cross-surface matrix.

**M-30 — Focus visibility/geometry is inconsistent (source-proven plus platform-observed).** Application QSS weakens some platform defaults; hover/focus are conflated; a 1-to-2-pixel border can alter geometry. Pane hide/show and modal close do not consistently specify destination. Direct platform tests exposed activation-sensitive failures.

**M-31 — Native CMake recompiles production translation units independently into many test executables (source-proven build duplication).** Root CMake contains 103 `src/codex/*.cpp` target entries for 30 unique production translation units; 25 occur more than once (`MiddleTypes.cpp` ten times, `UiStyle.cpp` nine, conversation model/view sources six to seven). This increases build cost and permits tests to use feature definitions/link context different from `codex-ui`. A reusable non-`main` production target should replace source-list duplication only if the refactor deletes more build structure than it adds and preserves target-specific seams.

## Qt-correctness audit matrix

| Area | Result |
|---|---|
| QObject parent/child ownership | Most widgets/timers/providers have parents or QObject contexts. No confirmed dangling callback was found. Composer synchronous emitter deletion (H-13), Inspector subtree replacement (H-10), and overwritten handler ownership (M-05) remain hazards. |
| Signal/slot lifetime | Context-bound lambdas are generally safe; global/session handler replacement is implicit. Diff results are receiver/context guarded but computation is not canceled. |
| Thread affinity | QWidget work remains Qt-main; worker is sole graph writer; libgit2 uses the global pool and queued results. No cross-thread QWidget access was found. |
| Reentrancy/queued delivery | Graph locks are released before widget calls. Immediate layout clearing from a clicked attachment and modal-request mutation deserve correction. Frame coalescing/zero-delay retries add order dependence. |
| QPointer/dangling callbacks | QPointer appears around optional widgets/attachments, but the graph attachment abstraction is unused residue. Diagnostics found no use-after-free. |
| Timers/animations | Parent/context ownership is generally correct; there are many independent timers, exact-boundary tests, and no reduced-motion/idle budget. |
| Event-filter lifetime | QObject lifetime handles removal, but card descendant filters can miss later children; consuming `LayoutRequest` and broad wheel routing blur ownership. |
| Focus/current index | Custom restoration crosses rebuild/materialization. Child focus persistence, pane transitions, and platform activation are incomplete. |
| Clipboard/tooltip lifetime | Clipboard ownership/failure is not handled; X11 selection warning was observed. Tooltip styling/feedback is split across application/local code. |
| Model reset/persistent indexes | Conversation model uses stable IDs and incremental signals. ThreadPane rebuilds a list widget; restoration must remain ID-based. No invalid persistent-index dereference was proven. |
| Layout requests / `updateGeometry` | Conversation repeatedly invalidates/activates layouts and removes posted layout events; style/geometry owner is not singular. |
| Polish/unpolish/stylesheets | Dynamic properties/local sheets rely on repolish behavior without a central style epoch. Fixed application rules can suppress native focus/a11y affordances. |
| Paint events/regions | Turn cross-row decoration is legitimate view paint. Passive card paint is duplicate. Thread animation repaints a whole viewport. |
| High DPI/device rounding | Logical Qt geometry is mostly used, but thumbnails/pixmaps and tests omit DPR keys/conversions. Fractional failures remain product-versus-harness unknowns. |
| Font metrics/height-for-width | Wrapped content has several measurements/caches. Large-font startup passed selected tests; runtime font/style invalidation is absent. |
| Size hints/minimum hints | Fixed sizes and captured canonical heights are common. Hard window minimum impairs magnification/responsiveness. |
| Nested scroll/scrollbar ownership | Conversation, command, content views, Inspector, and diff form nested scroll areas with duplicated wheel/follow state. One Wayland follow-tail scenario failed. |
| Event propagation/mouse grabs | Passive synthetic press/release has no demonstrated cancel/grab-equivalent state. Direct platform tests were inconclusive. |
| Keyboard navigation/drag selection | Core shortcuts exist; pseudo-tree, file/request links, dematerialized selection, and activation-sensitive tests leave gaps. |
| Context menus | Thread menu correctly keeps selection unchanged and uses stable pointer-row identity; full keyboard-menu coverage is absent. |
| Accessibility names/roles/state | Rich buttons often have names; passive actions, tree levels, live regions, streamed state, and dynamic request forms are incomplete. No screen-reader run exists. |

## Interaction and adverse-state matrix

| Interaction | Immediate feedback / success | Rejection, timeout, disconnect, stale/duplicate, rapid repeat | Keyboard/mouse/a11y conclusion |
|---|---|---|---|
| Copy / Ctrl+C | icon/tooltip morph; clipboard text | no ownership/failure feedback; timer boundary flaky | rich keyboard support exists; passive action pointer-only; cleanup can discard rich MIME. |
| Selection | rich text selection; offsets cached | renderer swap/rebuild can change focus/direction; Unicode/RTL incomplete | drag/Ctrl+C failed in an inactive XCB fixture; passive semantics absent. |
| Folding | disclosure toggles local state | rapid transition/cache retirement not comprehensive | rich button accessible; passive hit region is not. |
| Scrolling/follow-tail | anchor/follow animation and paused state | streamed/reflow/switch/history are complex; one Wayland nested-tail failure | wheel ownership duplicated; nested keyboard navigation incomplete. |
| Steering | immediate local card and sequential dispatch | terminal/uncertain recovery modeled; rapid queue core-tested | action derives selected thread/active turn; no accessible admission announcement. |
| Send | admitted card, clear composer, pending animation, correlation | disconnect/hydration gates and no auto-resend; timeout UI not complete | Enter policy implemented; state changes not announced. |
| Stop | stable active-turn interrupt sent | repeated/in-flight/timeout state not explicit | button works; busy/accepted semantics incomplete. |
| Thread creation | dialog + optimistic row/draft + confirmation | three owners reconcile failure/reconnect | keyboard dialog conventional; ephemeral-name behavior unexplained. |
| Quick fork/options | menu/dialog feedback | busy notice; stale/repeated requests not comprehensively gated | standard actions; focus return unspecified. |
| Thread navigation/sorting | current row/local sort update | rebuild/load-more/time aging can perturb presentation | custom Left/Right; no semantic tree roles/levels. |
| Panel resize/show/hide | splitter/buttons update | zero-delay reflow, fixed minima, font/DPI unqualified | hiding a focused pane has no stated focus target. |
| Context menus | stable ID under pointer; selection unchanged | authority rejection becomes notice | mouse/menu keyboard path exists; dedicated keyboard coverage incomplete. |
| Approvals/user input | request button/Inspector/modal form | can stale/disconnect; repeat/busy handling fragmented | labels/buddies/link activation/error focus/live announcements incomplete. |

### Cross-interaction state dimensions

| Required dimension | Audit conclusion across applicable interactions |
|---|---|
| Immediate feedback | Prompt admission and many buttons provide immediate visuals; Stop/controller/request/thread mutations are inconsistent; copy feedback has no semantic announcement. |
| Delayed acknowledgement | Prompt correlation is explicit; thread/request/runtime actions mostly rely on later graph/effect state without one reusable pending contract. |
| Success | Prompt/new-thread paths reconcile stable identity; other operations usually refresh graph state, but success indication is surface-specific. |
| Rejection | Queue-admission rejection generally produces a notice; field/form rejection and focus-to-error are inconsistent. |
| Timeout | No uniform UI timeout owner or recovery presentation was found for mutating actions. Provider history demonstrates a missing failure-to-wait-state transition. |
| Disconnect/reconnect | Prompt no-auto-resend/recovery semantics are strong; other pending controls, modal requests, selection, and history wait state lack a complete matrix. |
| Stale/duplicate response | Worker request correlation/generation checks cover core protocol and diff result application; UI controls do not consistently expose or test stale/duplicate resolution. |
| Rapid repeated action | Per-thread prompt queue is intentional; new creation has a guard; Stop, connection/controller, thread mutations, and request resolution are inconsistent. |
| Keyboard-only | Editor, standard buttons, menus, and much rich text work; passive card actions, pseudo-tree semantics, request/file links, and focus restoration remain gaps. |
| Mouse | All principal controls have mouse paths; passive synthetic forwarding, nested scroll ownership, context highlighting, and deleted chip emitters are concerns. |
| Screen reader | Accessible names exist on selected rich controls; passive rows, hierarchy, dynamic state/live feedback, and forms lack complete roles/events; no real run occurred. |
| Focus visibility | Application/local style is inconsistent; activation-sensitive tests failed; pane/dialog/rebuild/materialization focus destination is not singular. |
| Reduced motion | Browser has a media-query path; native Qt has none. |
| Empty/loading/error | Dedicated views/notices/local failed prompts exist, but accessibility and provider-history failure recovery are incomplete. |
| Partial/very long data | Unknown/partial protocol fields are retained in graph; UI bounding is uneven (conversation stronger, Inspector/request/diff/attachments weaker). |
| Unicode/multiline/IME | Prompt editor supports IME/multiline and current tests cover examples; grapheme selection, RTL, extreme wrapping, paths, and all card/dialog types are not qualified. |
| Links/attachments | Markdown/file/MCP links and file chooser exist; keyboard/open failure/MIME latency/byte bounds/clipboard-richness remain findings. |

## Visual-stability audit

| Trigger | Geometry-changing mechanism without semantic change | Assessment |
|---|---|---|
| Passive/live transition | complete renderer/layout/semantic-tree replacement | Critical demonstrated architecture cause. |
| Focus/hover | materialization plus style/property/border | Can shift presentation; dirty offsets/property mask cause. |
| Copy feedback | icon/tooltip/timer/style | intended paint change must not alter layout; no a11y announcement. |
| Animation start/stop | sweep, spinner, follow, thread overlay | no reduced-motion owner; paint sometimes broader than target. |
| Status/optimistic acknowledgement | label/tone/header/body apply | classification/header rules duplicated; geometry can change. |
| Card replacement/folding | widget create/delete, cache, height correction | anchor logic extensive; focus/selection incomplete. |
| Streamed update | Markdown layout, Inspector rebuild, command settle | repeated layout and selection/follow risk. |
| Scrollbar appearance | nested scroll/text areas | changes available width/wrapping; ownership duplicated. |
| Font/style repolish or DPR/screen move | caches lack epoch; fixed metrics/pixmaps | runtime unqualified; startup-font sample only. |
| Window/splitter resize | estimates, correction passes, overlay geometry | examples tested, not full fonts/DPR/platforms. |
| Thread switch/page load/history trim | staged projection and object/cache release | stale cache and atomic-frame risks. |

At 1x offscreen, the full suite passed. Selected offscreen suites at 1.25x/1.5x/2x consistently failed the virtualization spinner/pixel test; focused parity checks fail more because device-pixel grabs are sampled at logical coordinates. A startup large font (`DejaVu Sans`, 16 pt) passed selected suites, but runtime font/style change was not tested. Direct XCB/Xvfb and real Wayland/XWayland runs produced activation, clipboard, splitter, focus, and nested-follow failures. They remain unresolved product-versus-harness evidence; neither screenshots nor shared constants prove equivalence.

## Performance evidence

### Conversation benchmark

The existing workload is architecture-blind: every generated row stays passive, so object count is zero cards/eight descendant widgets. These are measurements, not thresholds.

| Scale | Rows | Initial ms | 240-step sweep microseconds | Tail append microseconds | Cards/widgets | Peak RSS KiB |
|---|---:|---:|---:|---:|---:|---:|
| 1x | 320 | 10 | 408,412 | 509 | 0 / 8 | 46,400 |
| 1x | 1,280 | 32 | 458,001 | 565 | 0 / 8 | 46,556 |
| 1x | 10,000 | 269 | 600,355 | 538 | 0 / 8 | 67,088 |
| 1.25x | 10,000 | 261 | 674,647 | 829 | 0 / 8 | 68,164 |
| 1.5x | 10,000 | 258 | 713,815 | 584 | 0 / 8 | 70,648 |
| 2x | 10,000 | 263 | 834,217 | 594 | 0 / 8 | 74,900 |

At 1,280 rows, XCB/Xvfb measured 34 ms initial, 559,436 microseconds sweep, 726 microseconds append, 79,280 KiB; Wayland measured 45 ms, 491,475 microseconds, 2,488 microseconds, 87,700 KiB. Reported geometry passes were zero because the benchmark reads a dead/wrong metric instead of `conversationLocalGeometryPasses`.

### Cost inventory

| Surface | Startup/hydration | Offscreen/steady cost | Update/paint/layout risk |
|---|---|---|---|
| Shell/runtime | config, connection, initial projections | 100 ms recovery polling | coalesced retries/full chrome; no startup benchmark. |
| Threads | adapter scan; all row widgets | O(loaded rows) widgets; 32 ms paint while optimism exists | pagination bounds data, not QWidget count. |
| Conversation | full projection, height index, staged visible work | model/heights/caches; passive 128-document LRU; keyed maps unbounded | two Markdown/card renderers; correction/layout passes. |
| Inspector | complete projection/widget build | O(all plan/agent/request rows) | subtree rebuild, Markdown docs; 48+2 bound absent. |
| Diff | async libgit2, duplicate providers | two polls, all watched paths | obsolete tasks run; three review documents/highlighters. |
| Composer/images/files | normally small | synchronous MIME/image work on GUI | font/style/DPR invalidation absent; bytes unbounded. |
| Browser | separate model projection | separate virtual viewport | 10k profile below; not native proof. |

Browser 10,000-item profile: hydrate **45.83 ms**, project **34.73 ms**, 2,000 deltas **9.16 ms**.

### Required performance-topic ledger

| Required topic | Concrete result or source-based bound |
|---|---|
| Initial startup | Not benchmarked. Configuration, session start, shell construction, and initial graph projections occur before stable presentation; this remains an explicit unknown rather than an inferred pass. |
| Initial thread listing | Adapter traversal and `ThreadPane::refresh` are O(loaded threads); every loaded row receives a QWidget. No latency/object threshold exists. |
| Background reconciliation | Graph notifications are frame-coalesced and use try-read/retry, but fallback/full pane projections can rescan collections. Correct lock behavior does not bound projection work. |
| Thread pagination | Provider pages bound new data per request, but every retained loaded row remains widget-backed; Load More has no local pending/work budget test. |
| Conversation hydration | Full adapter projection, model/index construction, structural staging, and initial visible layout occur. Loading cover targets atomic reveal, but future one-renderer staging is unmeasured. |
| Visible-row construction | Current live construction is range/interaction driven; passive-eligible visible rows avoid widgets. Target must quantify card/document creation for viewport+overscan. |
| Offscreen-row cost | Model/height/state remain O(N); no offscreen QWidget for passive rows, but keyed maps can outlive rows. Thread/Inspector offscreen widgets are O(N). |
| Markdown parsing | Passive delegate owns an LRU of 128 documents; live cards own browser documents; handoff may reuse a document but materialization still relayouts/repaints. Parse/reparse counts are not exposed. |
| Markdown repaint | Visible passive Markdown is painted by delegate; live Markdown by QTextBrowser. Paint-region/count metrics are absent and renderer replacement invalidates correctness comparisons. |
| Widget/document counts | Existing benchmark reports 0 cards/8 descendants only because it never enters the live path. Thread/Inspector/diff document/widget counts are not instrumented. |
| Layout requests | View/card sizing explicitly invalidates/activates layouts and consumes some `LayoutRequest`s; only a misnamed/dead geometry counter is reported. |
| Paint regions | Thread optimism repaints a full viewport at 32 ms; conversation uses viewport updates plus child-widget paint; no paint-region area budget exists. |
| Scroll complexity | Height index gives logarithmic prefix/row lookup; measured aggregate sweep grows moderately through 10k, but it excludes live-card churn and has no percentile bound. |
| Splitter resizing | Interactive resize defers exact conversation reflow and synchronizes overlay geometry; no frame-time/paint-count benchmark exists. |
| Streamed output | Exact/tail application avoids full rebuild in tested examples; Markdown/card/Inspector layout costs and command settle frequency have no representative stream benchmark. |
| Command follow-tail | Explicit local follow state and settlement timers exist; one real-Wayland test failed. Complexity/budget and correctness remain unresolved. |
| Timers/animations | 32 ms, 100 ms, one-second-class feedback, and two-second polls are independently scheduled; count/wakeup/idle power is unmeasured. |
| Caches | Passive documents are capped at 128; image and row-state caches lack complete identity/style/DPR retirement contracts. Cardinality is not exposed. |
| Repeated graph projections | Full, exact, tail, plan, request, thread and Inspector paths duplicate scans/formatting. Try-read is nonblocking but does not eliminate repeated CPU/allocation cost. |

## Tests and diagnostics

### Executed results

| Diagnostic | Result |
|---|---|
| Fresh Debug Ninja build (`../build/Desktop_GCC-Debug`) | passed |
| Full native CTest, offscreen 1x | **19/19 passed**; independent runs took 14.57–28.77 s depending on load/cache |
| Nine selected UI suites, offscreen 1.25x/1.5x/2x | **8/9 passed at each scale**; virtualization failed DPR-invalid pixel expectation |
| Focused remaining virtualization at fractional scales | multiple containment/header parity failures; harness mixes coordinate spaces |
| Direct XCB/Xvfb selected executables | graph conversation/thread, Inspector, Git, Shell passed; cards/virtualization/layout had activation-sensitive failures |
| Real Wayland selected executables | graph conversation/thread/Inspector passed; cards, nested follow-tail, and layout focus failed/warned |
| Real XWayland/XCB selected executables | clipboard ownership, splitter exactness, and focus failures; unresolved cause |
| Large font (`DejaVu Sans`, 16 pt), offscreen | cards, virtualization, layout, Inspector, thread passed |
| ASan/UBSan full CTest | **18/19 passed**; only virtualization's 100 ms wall assertion failed under instrumentation; no sanitizer diagnostic |
| TSan nodegraph | **5/5 passed** |
| `cppcheck` native production | completed; seven pass-by-value/rule-of-three warnings, no critical diagnostic |
| Browser `npm test`, existing dependencies | **91/91 passed** |
| Browser `npm run profile` | passed; metrics above |
| Conversation benchmark, sizes/scales/platforms | ran; values above; no thresholds/checksum |

No dependency was installed and no app/service was restarted. The sanitizer wall-time failure is test design, not evidence of memory corruption. Platform failures cannot be dismissed because offscreen passes.

Representative read-only/rebuild commands (existing build/dependency trees only):

```sh
cmake --build ../build/Desktop_GCC-Debug -j2
ctest --test-dir ../build/Desktop_GCC-Debug --output-on-failure
QT_SCALE_FACTOR=1.25 ctest --test-dir ../build/Desktop_GCC-Debug -R '<selected-ui-regex>' --output-on-failure
QT_SCALE_FACTOR=1.5  ctest --test-dir ../build/Desktop_GCC-Debug -R '<selected-ui-regex>' --output-on-failure
QT_SCALE_FACTOR=2    ctest --test-dir ../build/Desktop_GCC-Debug -R '<selected-ui-regex>' --output-on-failure
ctest --test-dir ../build/Sanitizers-ASan --output-on-failure
ctest --test-dir ../build/NodeGraph-TSan-Current --output-on-failure
cppcheck --enable=warning,performance,portability src/codex
(cd web && npm test && npm run profile)
```

Platform checks invoked existing test executables directly under Xvfb/XCB and the available Wayland/XWayland sessions because CTest properties override `QT_QPA_PLATFORM` to `offscreen`.
The nine-suite scale selection was: `codexui-nodegraph-conversation-ui`, `codexui-nodegraph-thread-pane-ui`, `codexui-conversation-cards`, `codexui-conversation-item-model`, `codexui-conversation-virtualization`, `codexui-application-layout`, `codexui-inspector-graph`, `codexui-git-changes-live`, and `codexui-shell-integration`.

`cppcheck` reported pass-by-value suggestions at `ClientRuntime.cpp:2004`, `PendingRequestPolicy.cpp:32`, `ProtocolUpdater.cpp:301,555`, and `NodeGraphUiAdapter.cpp:193`, plus rule-of-three warnings for the QObject-owned `ConversationCard::Impl` around `ConversationCards.cpp:1629`. These are low-value static diagnostics and are not counted as user-facing findings; no memory/thread-safety defect follows from them.

### Contract-to-test map

| Contract | Existing owner | Result/gap |
|---|---|---|
| Graph state/order/lifetime/transactions | NodeGraph, ProtocolUpdater, WorkerLogic, GraphConcurrency | strong headless coverage; attachment tests obsolete/dormant. |
| Queue/backpressure/wake-up | ThreadChannels, ClientRuntimeDispatch, WorkerLogic | bounded queue tested; shutdown/UI notification owner needs explicit contract. |
| Adapter thread/conversation/Inspector | adapter and three NodeGraph UI tests | examples covered; duplication/status alias/scan/object bounds not prevented. |
| Card render/interaction | ConversationCardsTest | broad 1x behavior; half disabled; timing/platform/a11y gaps. |
| Model/height/virtualization/scroll | ItemModel/Virtualization | broad contracts; parity preserves duplicates; short-circuit/DPR/activation blind spots. |
| Shell/prompt/dialog/settings | ShellIntegration/Established/PendingPolicy | happy/recovery broad; provider-history, rapid repeat, timeout, a11y/platform gaps. |
| Diff live behavior | GitChangesLive | representative updates; no file-count/overlap/cancel/watcher/triple-document bounds. |
| Accessibility | incidental names/keyboard checks | no `QAccessible` tree/event assertions or screen reader. |
| Performance correctness | Conversation benchmark | no target renderer, thresholds, percentiles, document census, startup/thread/Inspector/diff tests, or checksum. |
| DPI/platform/font/reduced motion | ad hoc diagnostics | CTest forces offscreen; pixel tests wrong at DPR; runtime style/reduced-motion absent. |
| Browser semantic parity | ten `.mjs` suites | useful observable parity; can encode same semantic defect. |

## LOC and source-history analysis

### Current code LOC (`cloc`)

| Subsystem | Production code LOC |
|---|---:|
| `src/codex/middle` | 12,891 |
| `src/codex/ui` | 3,268 |
| `src/codex/nodegraph` | 7,274 |
| other `src/codex` shell/runtime/dialog/diff | 10,821 |
| **Native production total** | **34,254** |
| Native tests | **27,707** |
| Browser production | 4,938 |
| Browser tests | 2,422 |

Largest production files (code LOC): `ConversationView.cpp` 3,818; `ShellWidget.cpp` 3,355; `ProtocolUpdater.cpp` 3,242; `ClientRuntime.cpp` 2,660; `ConversationCards.cpp` 2,452; `NodeGraphUiAdapter.cpp` 2,109; `WorkerLogic.cpp` 1,422; `ConversationItemModel.cpp` 1,205; `InspectorPane.cpp` 1,194; `ThreadPane.cpp` 1,076. `ConversationCardsTest.cpp` is 7,505 code LOC, including 3,248 disabled lines.

### History explanation

| Commit | Architectural effect | `src/codex` delta |
|---|---|---:|
| `6c4a0e1` | historical bounded single-card-widget viewport/overscan feasibility; removed O(N) materialization | +1,050/-1,639, net -589 |
| `bd2297e` | introduced passive second renderer | +718/-12, net +706 |
| `d58020d` | added synthetic passive interaction/state | +471/-128, net +343 |
| `0d21950` | shared values, retained renderers | +161/-138, net +23 |
| `aab2723` | shared documents, retained renderers | +659/-99, net +560 |
| `629660e` | added more parity behavior/tests | +382/-68, net +314 |
| `6c1ed14` | removed prior thread visibility/overscan while docs retained it | +659/-1,820, net -1,161 |
| `ded5055` | removed prior Inspector 48+2 bound while docs retained it | +1,221/-3,448, net -2,227 |
| `09f8144` | narrowed graph-driven pane updates but retained complete Inspector/thread projections | +758/-126, net +632 |
| `ce369ff` | retained Inspector row frames/staged layouts; descendant patching still rebuilds content | +570/-143, net +427 |
| `42b1f0e` | introduced libgit2 change review alongside embedded changes, establishing duplicated diff surfaces | +881/-210, net +671 |
| `c225032`, `042dd68`, `d84931d` | successively stabilized composer overlay spacing/boundary/layout requests rather than establishing a single runtime-invalidated geometry owner | combined +42/-29, net +13 |

History proves a bounded one-widget-renderer shape existed, not that today's heavier card meets bounds. It shows accretion: value sharing, document transfer, synthetic interaction, parity geometry, and tests each grew code without deleting the old path.

`git log --follow` was inspected for `ConversationView`, `ConversationCards`, `ShellWidget`, `ThreadPane`, `InspectorPane`, `ComposerPane`, `DiffViewer`, and `GitDiffProvider`. Targeted `git blame` confirmed that the two diff construction regions principally descend from `42b1f0e`, retained Inspector rows principally from `ce369ff`, and current thread-row construction principally from `6c1ed14`/`09f8144`. History explains origin and expansion; it is not used to override current-source evidence.

Largest credible deletion opportunities, ordered: (1) passive conversation renderer/synchronization/parity, net -750 to -1,050 production LOC; (2) obsolete Qt attachment lifecycle plus 3,248 dormant test lines, with production/test deltas separate; (3) one Inspector Markdown/card shell plus bounded rows; (4) unified diff snapshot/provider/poll and lazy layout documents; (5) one conversation ordering projection; (6) model-driven stable Thread/Inspector rows. Splitting monoliths without deletion is not remediation.

## Ordered remediation sequence for a separate approved thread

1. Approve this audit and the one-renderer invariant; freeze the representative benchmark/behavior corpus before editing.
2. Delete passive card presentation, paint, hit testing, document transfer, synthetic forwarding, and parity-only tests.
3. Materialize existing `ConversationCard` only for viewport plus measured overscan; keep view-owned Turn decoration and model/height index.
4. Make only viewport-intersecting controls tabbable while overscan preconstructs; stage the initial visible range behind the loading cover.
5. Bind caches to retained identities and explicit content/style/font/DPR revisions; delete obsolete entries rather than reconcile them.
6. Verify full correctness/accessibility/platform/performance and obtain user visual confirmation. Phase gate: production LOC must fall 750–1,050 and no passive renderer symbol/path may remain.
7. Only then address, as separate deletion phases: canonical conversation projection/status/request policy; Inspector shell/bounds; ThreadPane bounds; diff snapshot/provider; obsolete attachment lifecycle; reduced-motion/style epoch.
8. Report production and test `git diff --numstat` separately after every phase. Stop on LOC growth, a third path, a special case, or unexplained behavior.

### Acceptance gates for that future phase

| Gate | Required evidence |
|---|---|
| A0 — approved boundary | Audit and exact deletion plan approved in a separate implementation thread; dirty baseline classified/preserved. |
| A1 — renderer absence | Repository search finds no passive card policy, projection, delegate, shell/body paint, hit test, synthetic forwarding, document-transfer bridge, or two-renderer parity test. |
| A2 — single ownership | One card/layout/a11y implementation explains pixels and interaction; virtualization code refers only to residency/range/staging, not card appearance. |
| A3 — LOC | Per-phase `git diff --numstat`: production net negative; phase-one net -750 to -1,050 expected. Test and docs LOC reported separately. Stop if production grows. |
| A4 — bounded resources | At 320/1,280/10,000 mixed rows, cards, documents, focusable controls, parse/measure work, and construction churn are O(viewport+overscan), with declared numeric limits. |
| A5 — correctness | All existing tests execute without short-circuit plus invariant tests for stable identity, height/anchor/follow, trim/removal/cache lifecycle, all card kinds, streaming, failures, rapid input, and parity fixtures. |
| A6 — visual stability | DPR-correct checks at 1x/1.25x/1.5x/2x and multiple fonts prove residency/focus/hover/copy alone does not change geometry; shared constants/pixel comparison between renderers is forbidden. |
| A7 — platform/interaction | Activated X11/XWayland and Wayland: keyboard/mouse selection, Ctrl+C, clipboard, links, folding, context menus, nested scroll, focus traversal, panel resize, thread switching. |
| A8 — accessibility/motion | Screen-reader tree/events, names/roles/states/actions, visible focus, viewport-only tab stops, live feedback, and reduced-motion behavior verified. |
| A9 — performance | Fresh-process p50/p95/max startup/hydration/scroll/resize/streaming plus RSS/object/document counts meet agreed budgets without a second renderer. |
| A10 — delivery | No stale process; built artifact checksum recorded. If installation is later authorized, installed-binary checksum must match. User visually confirms before any “fixed” claim. |

## Unresolved unknowns

- Whether direct XCB/Wayland focus, clipboard, splitter, and nested follow-tail failures are product defects or inactive/headless fixture limitations.
- Real screen-reader output/navigation on supported desktops.
- Runtime font family/size, palette/style, and cross-screen DPR changes; only startup large-font was exercised.
- Reduced-motion platform behavior, because Qt has no policy to exercise.
- Actual p50/p95/max construction/layout/paint and object/document churn for the target one-renderer viewport/overscan design.
- Startup, initial thread listing, background reconciliation, Inspector/thread scaling, diff watcher/concurrency, and idle-power measurements.
- Long-session measured cache cardinality; missing invalidation paths are proven, growth was not instrumented.
- Live app-server timeout, duplicate/stale response, and reconnect behavior without a controlled provider fixture.
- Unicode grapheme/RTL/IME and extreme request/attachment/image/diff workloads across all surfaces.
- User visual confirmation on real 1x, 1.25x, 1.5x, and 2x displays.

## File-by-file coverage ledger

**Ledger rule.** Every path tracked at audit start plus the later untracked `AGENTS.md` appears exactly once below. “Reviewed” means the primary inspected the file or its complete role/symbol surface and reconciled relevant independent evidence; delegation alone was not accepted. Non-Qt support/legal files are included to close the universe. The audit document itself is output, not part of the 185-file input.

### Repository/build/documents/resources

| Path | Purpose / owned UI concepts | Models/state consumed | Rendering mechanism | Interaction mechanisms | Ownership/lifetime | Related tests | Duplication/architecture concern | Audit status |
|---|---|---|---|---|---|---|---|---|
| `AGENTS.md` | Repository-local engineering constitution | Architecture, renderer, Qt, test, LOC, and change invariants | Markdown | Instruction review; no runtime interaction | Untracked external worktree file | This audit's gates cross-checked against it | Reinforces one renderer/deletion-first; no conflict found | **Reviewed** |
| `.github/workflows/ci.yml` | CI workflow | toolchain/test matrix | none | Build/tool invocation; no product input | CI run | all suites | H-15/M-23 platform limits | **Reviewed** |
| `.gitignore` | Generated-file exclusions | paths | none | None | repository | git status | Support | **Reviewed** |
| `CMakeLists.txt` | Root native Qt/test build graph | sources/options/dependencies | none | Build/tool invocation; no product input | configure/build | 19 CTests | H-15 offscreen; benchmark unregistered; M-31 repeated translation units | **Reviewed** |
| `LICENSE` | License text | legal metadata | none | None | repository | N/A | Outside Qt behavior | **Reviewed** |
| `LICENSE-LGPL-3.0-or-later` | License text | legal metadata | none | None | repository | N/A | Outside Qt behavior | **Reviewed** |
| `LICENSE-MIT` | License text | legal metadata | none | None | repository | N/A | Outside Qt behavior | **Reviewed** |
| `README.md` | Repository/build/app overview | architecture/build entry | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Claims cross-checked | **Reviewed** |
| `design/ux-decisions/thread-turn-model.md` | Thread/turn/prompt decisions | routing/admission/scroll invariants | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Useful invariant source; deviations noted | **Reviewed** |
| `docs/app-server-protocol/master-data-model.md` | Historical protocol research | protocol concepts | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Explicitly historical, not runtime authority | **Reviewed** |
| `docs/codex-architecture.md` | High-level native/browser architecture | single graph/two threads/local state | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Owner invariant confirmed | **Reviewed** |
| `docs/native-ui-ux-qualification-inventory.md` | Native qualification checklist | UI/test claims | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | H-11/H-15/M-23 stale claims | **Reviewed** |
| `docs/qt-virtualized-conversation-view.md` | Conversation virtualization contract | model/view/delegate/performance | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | C-01 institutionalized; benchmark incomplete | **Reviewed** |
| `docs/two-thread-shared-node-graph.md` | Graph/thread/protocol/lifecycle contract | graph/channels/attachments | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | H-08 stale attachment; core sound | **Reviewed** |
| `docs/ui-behavior.md` | Interaction/presentation contract | threads/conversation/composer/Inspector | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | H-09/M-23 contradictions | **Reviewed** |
| `docs/ui-ux-internal-api.md` | Native class/data/time API contract | adapter/panes/view/cards/Shell | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Documents complexity; claims need update | **Reviewed** |
| `docs/web-1.0-contract.md` | Browser/native process/parity boundary | browser architecture | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | M-24 separate renderer valid; semantics copied | **Reviewed** |
| `docs/web-qualification.md` | Browser parity/performance evidence | web tests/profile | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Reproduced; not native proof | **Reviewed** |
| `docs/web-release.md` | Browser release/operator gates | artifact/equality | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Packaging only | **Reviewed** |
| `resources/applications/codex-ui.desktop` | Desktop launcher metadata | name/executable/icon | desktop shell | Desktop launcher activation | installation | manual packaging | No install performed | **Reviewed** |
| `resources/icons/codex-ui.svg` | Application vector icon | brand geometry/colors | SVG | Visual inspection only | resource | manual packaging | Vector scaling reviewed | **Reviewed** |

### Native production

| Path | Purpose / owned UI concepts | Models/state consumed | Rendering mechanism | Interaction mechanisms | Ownership/lifetime | Related tests | Duplication/architecture concern | Audit status |
|---|---|---|---|---|---|---|---|---|
| `src/codex/AttachmentDraft.h` | Declares Local attachment value object | composer draft path/name/MIME | none | No direct input; copied by composer actions | draft value lifetime | ShellIntegration | M-11/M-29 | **Reviewed** |
| `src/codex/ClientRuntime.cpp` | Implements worker app-server transport/correlation | configuration, graph, channels, operations | none | Protocol requests/results and runtime callbacks | session worker lifetime | ClientRuntimeDispatch; ShellIntegration | M-05/M-06 | **Reviewed** |
| `src/codex/ClientRuntime.h` | Declares worker app-server transport/correlation | configuration, graph, channels, operations | none | Protocol requests/results and runtime callbacks | session worker lifetime | ClientRuntimeDispatch; ShellIntegration | M-05/M-06 | **Reviewed** |
| `src/codex/Configuration.cpp` | Implements CLI/QSettings configuration | arguments and persisted settings | none | CLI parsing and settings reads/writes | application lifetime | EstablishedUiUx | Persistence boundary reviewed | **Reviewed** |
| `src/codex/Configuration.h` | Declares CLI/QSettings configuration | arguments and persisted settings | none | CLI parsing and settings reads/writes | application lifetime | EstablishedUiUx | Persistence boundary reviewed | **Reviewed** |
| `src/codex/ConnectionDialog.cpp` | Implements connection transport editor | settings JSON | QDialog form | Edit/select transport; accept/cancel | modal parent tree | EstablishedUiUx; ShellIntegration | M-19/M-30 | **Reviewed** |
| `src/codex/ConnectionDialog.h` | Declares connection transport editor | settings JSON | QDialog form | Edit/select transport; accept/cancel | modal parent tree | EstablishedUiUx; ShellIntegration | M-19/M-30 | **Reviewed** |
| `src/codex/CurrentProtocolAdapters.h` | Declares current-protocol compatibility types | AISuite generated values | none | No direct input; typed protocol API | compile-time/value | CurrentProtocolAdapters | Compatibility boundary reviewed | **Reviewed** |
| `src/codex/DiffViewer.cpp` | Implements embedded changes/review UI | GitDiffSnapshot, selection, watchers | list/plain-text/highlighters | Select file/scope, refresh, open review, scroll/copy | widget tree; providers/timers | GitChangesLive | H-12/M-25 | **Reviewed** |
| `src/codex/DiffViewer.h` | Declares embedded changes/review UI | GitDiffSnapshot, selection, watchers | list/plain-text/highlighters | Select file/scope, refresh, open review, scroll/copy | widget tree; providers/timers | GitChangesLive | H-12/M-25 | **Reviewed** |
| `src/codex/FileSelectionDialog.cpp` | Implements workspace/attachment chooser | filesystem model/paths | tree/list/dialog | Navigate/select/add/remove/accept/cancel | modal parent tree | EstablishedUiUx; ShellIntegration | M-11/M-19 | **Reviewed** |
| `src/codex/FileSelectionDialog.h` | Declares workspace/attachment chooser | filesystem model/paths | tree/list/dialog | Navigate/select/add/remove/accept/cancel | modal parent tree | EstablishedUiUx; ShellIntegration | M-11/M-19 | **Reviewed** |
| `src/codex/ForkNaming.h` | Declares fork-name derivation | thread name/ID | none | No direct input; pure call | value lifetime | ShellIntegration | No duplicate owner found | **Reviewed** |
| `src/codex/FrontendSession.cpp` | Implements two-thread bridge/retirement coordinator | graph, channels, callbacks | none | Eventfd delivery, handler registration, shutdown | application session/notifier/timers | ClientRuntimeDispatch; ShellIntegration | H-08/M-05/M-26 | **Reviewed** |
| `src/codex/FrontendSession.h` | Declares two-thread bridge/retirement coordinator | graph, channels, callbacks | none | Eventfd delivery, handler registration, shutdown | application session/notifier/timers | ClientRuntimeDispatch; ShellIntegration | H-08/M-05/M-26 | **Reviewed** |
| `src/codex/GitDiffProvider.cpp` | Implements async libgit2 snapshot producer | repository/scope/context/generation | none | Request/cancel; emits loading/snapshot signals | QObject/global-pool jobs | GitChangesLive | H-12/M-25 | **Reviewed** |
| `src/codex/GitDiffProvider.h` | Declares async libgit2 snapshot producer | repository/scope/context/generation | none | Request/cancel; emits loading/snapshot signals | QObject/global-pool jobs | GitChangesLive | H-12/M-25 | **Reviewed** |
| `src/codex/MainWindow.cpp` | Implements top-level native window | FrontendSession | QMainWindow/Shell | Window/pane actions and close lifecycle | parent owns shell | EstablishedUiUx; ShellIntegration | M-27/M-30 | **Reviewed** |
| `src/codex/MainWindow.h` | Declares top-level native window | FrontendSession | QMainWindow/Shell | Window/pane actions and close lifecycle | parent owns shell | EstablishedUiUx; ShellIntegration | M-27/M-30 | **Reviewed** |
| `src/codex/NewThreadDialog.cpp` | Implements new/fork-options draft editor | NewThreadDraft/workspace/instructions | QDialog form | Edit/choose workspace; accept/cancel | modal parent tree | ShellIntegration; EstablishedUiUx | M-19 | **Reviewed** |
| `src/codex/NewThreadDialog.h` | Declares new/fork-options draft editor | NewThreadDraft/workspace/instructions | QDialog form | Edit/choose workspace; accept/cancel | modal parent tree | ShellIntegration; EstablishedUiUx | M-19 | **Reviewed** |
| `src/codex/NodeGraphJson.cpp` | Implements JSON/value conversion | protocol JSON/value | none | No direct input; conversion API | temporary values | NodeGraphJson | Boundary sound | **Reviewed** |
| `src/codex/NodeGraphJson.h` | Declares JSON/value conversion | protocol JSON/value | none | No direct input; conversion API | temporary values | NodeGraphJson | Boundary sound | **Reviewed** |
| `src/codex/PendingRequestDialog.cpp` | Implements approval/user-input/elicitation form | request descriptor/payload | dynamic modal widgets | Dynamic form edit/link/approve/reject/cancel | stack/modal lifetime | PendingRequestPolicy; ShellIntegration | H-07/M-19/M-21 | **Reviewed** |
| `src/codex/PendingRequestDialog.h` | Declares approval/user-input/elicitation form | request descriptor/payload | dynamic modal widgets | Dynamic form edit/link/approve/reject/cancel | stack/modal lifetime | PendingRequestPolicy; ShellIntegration | H-07/M-19/M-21 | **Reviewed** |
| `src/codex/PendingRequestPolicy.cpp` | Implements request classification/response policy | method/payload/response | none | No direct UI; pure classify/response calls | pure values | PendingRequestPolicy; ShellIntegration | H-07 | **Reviewed** |
| `src/codex/PendingRequestPolicy.h` | Declares request classification/response policy | method/payload/response | none | No direct UI; pure classify/response calls | pure values | PendingRequestPolicy; ShellIntegration | H-07 | **Reviewed** |
| `src/codex/ShellWidget.cpp` | Implements shell/routing/optimistic orchestration | session, graph adapter, selection/drafts/flags | top chrome and panes | Buttons/menus/dialog callbacks -> node/runtime actions | Impl owns tree/handlers | ShellIntegration; EstablishedUiUx | H-04-H-07/M-01-M-06 | **Reviewed** |
| `src/codex/ShellWidget.h` | Declares shell/routing/optimistic orchestration | session, graph adapter, selection/drafts/flags | top chrome and panes | Buttons/menus/dialog callbacks -> node/runtime actions | Impl owns tree/handlers | ShellIntegration; EstablishedUiUx | H-04-H-07/M-01-M-06 | **Reviewed** |
| `src/codex/TurnSettingsWidget.cpp` | Implements turn settings controls/serialization | catalog/current/touched settings | menus/combos/widget | Menus/combos/text changes -> touched draft | composer-owned child | EstablishedUiUx; ShellIntegration | M-20/M-30 | **Reviewed** |
| `src/codex/TurnSettingsWidget.h` | Declares turn settings controls/serialization | catalog/current/touched settings | menus/combos/widget | Menus/combos/text changes -> touched draft | composer-owned child | EstablishedUiUx; ShellIntegration | M-20/M-30 | **Reviewed** |
| `src/codex/UiStatus.h` | Declares status classification/humanization | raw status string | none | No direct input; pure classification | header-only values | Cards/Adapter/Shell tests | H-03/M-24 | **Reviewed** |
| `src/codex/WorkerMailboxReceiver.cpp` | Implements Qt eventfd/SPSC drain | worker messages/channels | none | Socket notifier wake and queued drain | FrontendSession receiver | ClientRuntimeDispatch; ThreadChannels | M-26; otherwise sound | **Reviewed** |
| `src/codex/WorkerMailboxReceiver.h` | Declares Qt eventfd/SPSC drain | worker messages/channels | none | Socket notifier wake and queued drain | FrontendSession receiver | ClientRuntimeDispatch; ThreadChannels | M-26; otherwise sound | **Reviewed** |
| `src/codex/main.cpp` | Implements native process bootstrap/shutdown | configuration/session/application | QApplication/MainWindow | Application event loop/start/close | process lifetime | full suite indirectly | M-05 | **Reviewed** |
| `src/codex/middle/ComposerPane.cpp` | Implements composer/attachments/send/steer/stop | draft/settings/active/connection | overlay layouts/editor/buttons | Edit/attach/send/steer/stop/settings buttons | MiddleRegion child/rebuilt chips | ShellIntegration; EstablishedUiUx | H-13/M-10/M-11/M-30 | **Reviewed** |
| `src/codex/middle/ComposerPane.h` | Declares composer/attachments/send/steer/stop | draft/settings/active/connection | overlay layouts/editor/buttons | Edit/attach/send/steer/stop/settings buttons | MiddleRegion child/rebuilt chips | ShellIntegration; EstablishedUiUx | H-13/M-10/M-11/M-30 | **Reviewed** |
| `src/codex/middle/ConversationCards.cpp` | Implements live card shell/all card bodies | VisibleCardData/interaction state | frames/layouts/browser/text/custom paint | Copy/fold/link/select/scroll/recovery/keyboard | View-owned materialized widgets | Cards; Virtualization | C-01/H-01/H-16/M-12/M-14/M-16 | **Reviewed** |
| `src/codex/middle/ConversationCards.h` | Declares live card shell/all card bodies | VisibleCardData/interaction state | frames/layouts/browser/text/custom paint | Copy/fold/link/select/scroll/recovery/keyboard | View-owned materialized widgets | Cards; Virtualization | C-01/H-01/H-16/M-12/M-14/M-16 | **Reviewed** |
| `src/codex/middle/ConversationHeightIndex.cpp` | Implements row heights/prefix-sum index | height integers | none | No direct input; indexed geometry API | View value member | ItemModel; Virtualization | Retain as single geometry index | **Reviewed** |
| `src/codex/middle/ConversationHeightIndex.h` | Declares row heights/prefix-sum index | height integers | none | No direct input; indexed geometry API | View value member | ItemModel; Virtualization | Retain as single geometry index | **Reviewed** |
| `src/codex/middle/ConversationItemModel.cpp` | Implements stable keyed rows/Qt roles | snapshots/row deltas | QAbstractListModel roles | Qt model roles and incremental row signals | View-owned QObject | ItemModel; Virtualization | H-01/H-05 | **Reviewed** |
| `src/codex/middle/ConversationItemModel.h` | Declares stable keyed rows/Qt roles | snapshots/row deltas | QAbstractListModel roles | Qt model roles and incremental row signals | View-owned QObject | ItemModel; Virtualization | H-01/H-05 | **Reviewed** |
| `src/codex/middle/ConversationPresentation.cpp` | Implements shared presentation helpers/constants | card data/status/text | document helpers | No direct input; presentation helper calls | stateless/cache helper | Cards; Virtualization | C-01/H-16 synchronizes duplicates | **Reviewed** |
| `src/codex/middle/ConversationPresentation.h` | Declares shared presentation helpers/constants | card data/status/text | document helpers | No direct input; presentation helper calls | stateless/cache helper | Cards; Virtualization | C-01/H-16 synchronizes duplicates | **Reviewed** |
| `src/codex/middle/ConversationView.cpp` | Implements virtual item view/scroll/layout | model, height index, snapshots, caches | item view/passive delegate/Turn paint | Wheel/scroll/key/mouse/context/focus/materialization | owns cards/timers/caches | Virtualization; NodeGraphConversationUi | C-01/H-01/H-02/H-05/H-16/M-14-M-17 | **Reviewed** |
| `src/codex/middle/ConversationView.h` | Declares virtual item view/scroll/layout | model, height index, snapshots, caches | item view/passive delegate/Turn paint | Wheel/scroll/key/mouse/context/focus/materialization | owns cards/timers/caches | Virtualization; NodeGraphConversationUi | C-01/H-01/H-02/H-05/H-16/M-14-M-17 | **Reviewed** |
| `src/codex/middle/InspectorPane.cpp` | Implements Inspector tabs/plan/agents/requests/state/changes | InspectorSnapshot/protocol tail | frames/scroll areas/Markdown/diff | Tabs/hide/refresh/fold/copy/request/diff interactions | MiddleRegion child | NodeGraphInspectorUi; ShellIntegration | H-09/H-10/M-21/M-28 | **Reviewed** |
| `src/codex/middle/InspectorPane.h` | Declares Inspector tabs/plan/agents/requests/state/changes | InspectorSnapshot/protocol tail | frames/scroll areas/Markdown/diff | Tabs/hide/refresh/fold/copy/request/diff interactions | MiddleRegion child | NodeGraphInspectorUi; ShellIntegration | H-09/H-10/M-21/M-28 | **Reviewed** |
| `src/codex/middle/MiddleRegionWidget.cpp` | Implements pane/composer composition | visibility/options/notices | splitters/frames/overlay routing | Pane toggles/splitter/wheel routing/notices | Shell child/timer | EstablishedUiUx; ShellIntegration | M-01/M-10/M-14/M-27/M-30 | **Reviewed** |
| `src/codex/middle/MiddleRegionWidget.h` | Declares pane/composer composition | visibility/options/notices | splitters/frames/overlay routing | Pane toggles/splitter/wheel routing/notices | Shell child/timer | EstablishedUiUx; ShellIntegration | M-01/M-10/M-14/M-27/M-30 | **Reviewed** |
| `src/codex/middle/MiddleTypes.cpp` | Implements conversation DTO/card identity/payloads | projected graph/local prompts | none | No direct input; value/identity API | snapshot values | Cards; Adapter; Virtualization | H-05; appropriate DTO boundary | **Reviewed** |
| `src/codex/middle/MiddleTypes.h` | Declares conversation DTO/card identity/payloads | projected graph/local prompts | none | No direct input; value/identity API | snapshot values | Cards; Adapter; Virtualization | H-05; appropriate DTO boundary | **Reviewed** |
| `src/codex/middle/ThreadPane.cpp` | Implements thread hierarchy/sort/context UI | ThreadListSnapshot/optimistic/expanded | list + row widgets/delegate overlay | Select/sort/expand/keyboard/context/load-more | MiddleRegion child/rebuilt rows | NodeGraphThreadPaneUi; ShellIntegration | H-06/H-11/M-07-M-09 | **Reviewed** |
| `src/codex/middle/ThreadPane.h` | Declares thread hierarchy/sort/context UI | ThreadListSnapshot/optimistic/expanded | list + row widgets/delegate overlay | Select/sort/expand/keyboard/context/load-more | MiddleRegion child/rebuilt rows | NodeGraphThreadPaneUi; ShellIntegration | H-06/H-11/M-07-M-09 | **Reviewed** |
| `src/codex/nodegraph/CMakeLists.txt` | Nodegraph target/source inventory | CMake metadata | none | No direct input; declared/implemented API | configure | nodegraph tests | Build boundary reviewed | **Reviewed** |
| `src/codex/nodegraph/EventFd.cpp` | Implements Linux wake descriptor RAII | fd counter | none | Wake/read/write/close API | move-only RAII | ThreadChannels | Sound primitive | **Reviewed** |
| `src/codex/nodegraph/EventFd.h` | Declares Linux wake descriptor RAII | fd counter | none | Wake/read/write/close API | move-only RAII | ThreadChannels | Sound primitive | **Reviewed** |
| `src/codex/nodegraph/Messages.h` | Declares cross-thread messages/actions | NodeRefs/operations/effects | none | No direct input; typed queue payloads | queue values | ThreadChannels; WorkerLogic | M-01/H-08 | **Reviewed** |
| `src/codex/nodegraph/NodeGraph.cpp` | Implements canonical graph/state/order/lifetime | NodeState/relations/revisions | none | Read/write transaction API | shared nodes/locked graph | NodeGraph; GraphConcurrency | Authority sound; H-08 residue | **Reviewed** |
| `src/codex/nodegraph/NodeGraph.h` | Declares canonical graph/state/order/lifetime | NodeState/relations/revisions | none | Read/write transaction API | shared nodes/locked graph | NodeGraph; GraphConcurrency | Authority sound; H-08 residue | **Reviewed** |
| `src/codex/nodegraph/PromptText.cpp` | Implements prompt/attachment text extraction | Value fields | none | No direct input; pure extraction | temporary values | WorkerLogic; ProtocolUpdater | Boundary sound | **Reviewed** |
| `src/codex/nodegraph/PromptText.h` | Declares prompt/attachment text extraction | Value fields | none | No direct input; pure extraction | temporary values | WorkerLogic; ProtocolUpdater | Boundary sound | **Reviewed** |
| `src/codex/nodegraph/ProtocolCatalog.cpp` | Implements closed method/disposition inventory | protocol metadata | none | No direct input; lookup API | static table | ProtocolUpdater; Adapters | Authority inventory sound | **Reviewed** |
| `src/codex/nodegraph/ProtocolCatalog.h` | Declares closed method/disposition inventory | protocol metadata | none | No direct input; lookup API | static table | ProtocolUpdater; Adapters | Authority inventory sound | **Reviewed** |
| `src/codex/nodegraph/ProtocolUpdater.cpp` | Implements protocol normalization/graph writes | decoded messages/graph | none | Decoded-message apply API | worker updater | ProtocolUpdater | H-03 raw/canonical split | **Reviewed** |
| `src/codex/nodegraph/ProtocolUpdater.h` | Declares protocol normalization/graph writes | decoded messages/graph | none | Decoded-message apply API | worker updater | ProtocolUpdater | H-03 raw/canonical split | **Reviewed** |
| `src/codex/nodegraph/SpscQueue.h` | Declares bounded SPSC queue | typed messages | none | Push/pop/backpressure API | channels member | ThreadChannels; GraphConcurrency | Sound bounded mechanism | **Reviewed** |
| `src/codex/nodegraph/ThreadChannels.cpp` | Implements two queues/wake descriptors | worker/Qt messages | none | Send/drain/wake APIs | session lifetime | ThreadChannels; WorkerLogic | Sound/backpressure tested | **Reviewed** |
| `src/codex/nodegraph/ThreadChannels.h` | Declares two queues/wake descriptors | worker/Qt messages | none | Send/drain/wake APIs | session lifetime | ThreadChannels; WorkerLogic | Sound/backpressure tested | **Reviewed** |
| `src/codex/nodegraph/Value.cpp` | Implements framework-neutral protocol value | scalar/array/object | none | No direct input; value accessors | value/shared storage | NodeGraphJson; Graph tests | Sound boundary | **Reviewed** |
| `src/codex/nodegraph/Value.h` | Declares framework-neutral protocol value | scalar/array/object | none | No direct input; value accessors | value/shared storage | NodeGraphJson; Graph tests | Sound boundary | **Reviewed** |
| `src/codex/nodegraph/WorkerLogic.cpp` | Implements sole-writer action/prompt/request state | graph/channels/results | none | Consumes node/runtime actions and protocol results | worker lifetime | WorkerLogic; ProtocolUpdater | H-06/M-02; core authority strong | **Reviewed** |
| `src/codex/nodegraph/WorkerLogic.h` | Declares sole-writer action/prompt/request state | graph/channels/results | none | Consumes node/runtime actions and protocol results | worker lifetime | WorkerLogic; ProtocolUpdater | H-06/M-02; core authority strong | **Reviewed** |
| `src/codex/ui/BrandMark.cpp` | Implements application brand widget | style colors | custom painter | Display only | parent-owned widget | EstablishedUiUx | M-13/M-27 | **Reviewed** |
| `src/codex/ui/BrandMark.h` | Declares application brand widget | style colors | custom painter | Display only | parent-owned widget | EstablishedUiUx | M-13/M-27 | **Reviewed** |
| `src/codex/ui/ExpandingPromptEditor.cpp` | Implements prompt key/height/input policy | text document/input method | QPlainTextEdit | Text/IME/key/wheel/focus input | Composer child | EstablishedUiUx; ShellIntegration | M-10/M-29/M-30 | **Reviewed** |
| `src/codex/ui/ExpandingPromptEditor.h` | Declares prompt key/height/input policy | text document/input method | QPlainTextEdit | Text/IME/key/wheel/focus input | Composer child | EstablishedUiUx; ShellIntegration | M-10/M-29/M-30 | **Reviewed** |
| `src/codex/ui/NodeGraphUiAdapter.cpp` | Implements read-only graph/UI projections | graph/relations/status/fields | none | No direct input; nonblocking projection calls | stack adapter/pinned reads | Adapter + three pane tests | H-03/H-05/H-07/H-09/M-02/M-03/M-28 | **Reviewed** |
| `src/codex/ui/NodeGraphUiAdapter.h` | Declares read-only graph/UI projections | graph/relations/status/fields | none | No direct input; nonblocking projection calls | stack adapter/pinned reads | Adapter + three pane tests | H-03/H-05/H-07/H-09/M-02/M-03/M-28 | **Reviewed** |
| `src/codex/ui/QtNodeAttachment.h` | Declares declared node/widget attachment record | NodeRef/revision/materialization | none | No current production interaction | intended Qt-main attachment | disabled card tests | H-08 obsolete/unexercised | **Reviewed** |
| `src/codex/ui/UiStyle.cpp` | Implements stylesheet/metrics/chevrons/humanization | palette/font/style constants | embedded QSS/custom paint | Style application and chevron button input | application/static/widget children | EstablishedUiUx; Cards | M-13/M-27/M-30 | **Reviewed** |
| `src/codex/ui/UiStyle.h` | Declares stylesheet/metrics/chevrons/humanization | palette/font/style constants | embedded QSS/custom paint | Style application and chevron button input | application/static/widget children | EstablishedUiUx; Cards | M-13/M-27/M-30 | **Reviewed** |
| `src/codex/ui/UiViewState.h` | Declares thread/Inspector projection DTOs | graph snapshots | none | No direct input; snapshot values | value lifetime | Adapter; pane tests | H-07/H-09/H-11 | **Reviewed** |

### Native tests

| Path | Purpose / owned UI concepts | Models/state consumed | Rendering mechanism | Interaction mechanisms | Ownership/lifetime | Related tests | Duplication/architecture concern | Audit status |
|---|---|---|---|---|---|---|---|---|
| `tests/codex/ClientRuntimeDispatchTest.cpp` | Test/benchmark: runtime dispatch/handler/correlation | ClientRuntime/FrontendSession | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ClientRuntimeDispatchTest.cpp | M-05/M-06 gaps | **Reviewed** |
| `tests/codex/ConversationCardsTest.cpp` | Test/benchmark: card render/interaction/focus/copy | ConversationCards/UiStyle | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ConversationCardsTest.cpp | H-15/H-16; 3,248 disabled lines | **Reviewed** |
| `tests/codex/ConversationItemModelTest.cpp` | Test/benchmark: incremental model/height identity | ItemModel/HeightIndex | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ConversationItemModelTest.cpp | H-05 examples | **Reviewed** |
| `tests/codex/ConversationViewBenchmark.cpp` | Test/benchmark: conversation cost samples | View/model/cards | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ConversationViewBenchmark.cpp | M-22 zero live cards/no thresholds | **Reviewed** |
| `tests/codex/ConversationVirtualizationTest.cpp` | Test/benchmark: virtualization/scroll/layout/parity | View/cards/model | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ConversationVirtualizationTest.cpp | H-15/H-16 short-circuit/DPR | **Reviewed** |
| `tests/codex/EstablishedUiUxTest.cpp` | Test/benchmark: shell/layout/dialog/composer behavior | multiple Qt widgets | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | EstablishedUiUxTest.cpp | H-15 platform/short-circuit gaps | **Reviewed** |
| `tests/codex/GitChangesLiveTest.cpp` | Test/benchmark: diff provider/watch/view | DiffViewer/GitDiffProvider | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | GitChangesLiveTest.cpp | H-12 no stress/bounds | **Reviewed** |
| `tests/codex/NodeGraphConversationUiTest.cpp` | Test/benchmark: adapter/conversation integration | adapter/view/cards | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | NodeGraphConversationUiTest.cpp | H-05; short-circuit | **Reviewed** |
| `tests/codex/NodeGraphInspectorUiTest.cpp` | Test/benchmark: Inspector projection/render | adapter/Inspector | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | NodeGraphInspectorUiTest.cpp | H-09 no row bound | **Reviewed** |
| `tests/codex/NodeGraphThreadPaneUiTest.cpp` | Test/benchmark: thread pane/paging/navigation | adapter/ThreadPane | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | NodeGraphThreadPaneUiTest.cpp | H-11 no widget bound | **Reviewed** |
| `tests/codex/NodeGraphUiAdapterTest.cpp` | Test/benchmark: headless projection contracts | NodeGraphUiAdapter | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | NodeGraphUiAdapterTest.cpp | H-03/H-05/H-07 gaps | **Reviewed** |
| `tests/codex/PendingRequestPolicyTest.cpp` | Test/benchmark: request descriptor/response | PendingRequestPolicy | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | PendingRequestPolicyTest.cpp | H-07 cross-owner gaps | **Reviewed** |
| `tests/codex/ShellIntegrationTest.cpp` | Test/benchmark: end-to-end shell flows | Shell/runtime/panes | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ShellIntegrationTest.cpp | H-04/M-06/a11y/platform gaps | **Reviewed** |
| `tests/codex/nodegraph/CMakeLists.txt` | Headless nodegraph test targets | CTest metadata | none | Synthetic API, keyboard, mouse, timing, and assertion flow | configure | nodegraph tests | Timeouts reviewed | **Reviewed** |
| `tests/codex/nodegraph/CurrentProtocolAdaptersTest.cpp` | Test/benchmark: compatibility adapters | CurrentProtocolAdapters | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | CurrentProtocolAdaptersTest.cpp | Protocol fixture gate | **Reviewed** |
| `tests/codex/nodegraph/GraphConcurrencyTest.cpp` | Test/benchmark: graph concurrency/transactions | NodeGraph | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | GraphConcurrencyTest.cpp | Strong headless gate | **Reviewed** |
| `tests/codex/nodegraph/NodeGraphJsonTest.cpp` | Test/benchmark: JSON/Value conversion | NodeGraphJson/Value | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | NodeGraphJsonTest.cpp | Boundary gate | **Reviewed** |
| `tests/codex/nodegraph/NodeGraphTest.cpp` | Test/benchmark: graph identity/order/removal/lifetime | NodeGraph | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | NodeGraphTest.cpp | Strong authority gate | **Reviewed** |
| `tests/codex/nodegraph/ProtocolUpdaterTest.cpp` | Test/benchmark: protocol disposition/update | ProtocolUpdater/Catalog | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ProtocolUpdaterTest.cpp | H-03 presentation gap | **Reviewed** |
| `tests/codex/nodegraph/ThreadChannelsTest.cpp` | Test/benchmark: queue/wake/backpressure | ThreadChannels/EventFd | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | ThreadChannelsTest.cpp | Strong mechanism gate | **Reviewed** |
| `tests/codex/nodegraph/WorkerLogicTest.cpp` | Test/benchmark: prompt/action/request transitions | WorkerLogic | Assertions/measurements | Synthetic API, keyboard, mouse, timing, and assertion flow | fixture/process | WorkerLogicTest.cpp | H-04/H-06/M-06 UI gaps | **Reviewed** |

### UI review material

| Path | Purpose / owned UI concepts | Models/state consumed | Rendering mechanism | Interaction mechanisms | Ownership/lifetime | Related tests | Duplication/architecture concern | Audit status |
|---|---|---|---|---|---|---|---|---|
| `ui-review/CODE-POLISH-ROADMAP.md` | Prior maintenance roadmap | refactor priorities | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Guardrails useful; predates root cause | **Reviewed** |
| `ui-review/STATE-MATRIX.md` | Interaction-state inventory | connection/thread/turn/composer | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Missing stale/a11y/motion matrix | **Reviewed** |
| `ui-review/UI-INVENTORY.md` | Visible UI inventory | bars/panes/cards/composer | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | M-23 older descriptions | **Reviewed** |
| `ui-review/UX-DESIGN-DECISIONS.md` | Visual/layout decisions | tokens/layout/follow | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | M-23 older dark baseline | **Reviewed** |
| `ui-review/screenshots/01-disconnected.png` | Historical screenshot 01 disconnected | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/02-connected-no-thread.png` | Historical screenshot 02 connected no thread | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/03-thread-normal-conversation.png` | Historical screenshot 03 thread normal conversation | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/04-active-streaming-turn.png` | Historical screenshot 04 active streaming turn | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/05-long-conversation.png` | Historical screenshot 05 long conversation | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/06-inspector-info.png` | Historical screenshot 06 inspector info | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/07-inspector-plan.png` | Historical screenshot 07 inspector plan | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/08-inspector-agents.png` | Historical screenshot 08 inspector agents | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/09-inspector-changes.png` | Historical screenshot 09 inspector changes | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/12-error-reconnect.png` | Historical screenshot 12 error reconnect | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/13-narrow-window.png` | Historical screenshot 13 narrow window | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/14-large-window.png` | Historical screenshot 14 large window | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/15-new-thread-draft.png` | Historical screenshot 15 new thread draft | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |
| `ui-review/screenshots/16-panels-hidden.png` | Historical screenshot 16 panels hidden | rendered pixels | PNG | Visual inspection only | artifact | manual review | M-23 stale dark baseline | **Reviewed** |

### Browser parity/packaging

| Path | Purpose / owned UI concepts | Models/state consumed | Rendering mechanism | Interaction mechanisms | Ownership/lifetime | Related tests | Duplication/architecture concern | Audit status |
|---|---|---|---|---|---|---|---|---|
| `web/.gitignore` | Browser generated exclusions | paths | none | None | repository | git status | Support | **Reviewed** |
| `web/AISUITE_REVISION` | Protocol-generator revision | build metadata | none | Build/tool invocation; no product input | repository value | normalizer/model tests | Parity provenance | **Reviewed** |
| `web/CMakeLists.txt` | Browser package/install target | build metadata | none | Build/tool invocation; no product input | configure/install | CI/tools | Qt packaging boundary | **Reviewed** |
| `web/README.md` | Browser build/verification | web packaging/tests | Markdown | Documentation review; no runtime interaction | repository | source/test cross-check | Parity support reviewed | **Reviewed** |
| `web/index.html` | Browser document host | DOM metadata | HTML shell | Browser page host only | page | qualification | Browser support | **Reviewed** |
| `web/package-lock.json` | Locked browser dependencies | npm graph | none | Build/tool invocation; no product input | build | npm tests | No install performed | **Reviewed** |
| `web/package.json` | Browser scripts/dependencies | npm metadata | none | Build/tool invocation; no product input | build | 91 tests/profile | Tooling reviewed | **Reviewed** |
| `web/src/app/App.tsx` | React shell/cards/panes/dialogs | presentation/local state | React/DOM/CSS | DOM click/key/input/scroll/dialog handlers | component tree | web suites | M-24; pre-existing dirty edit | **Reviewed** |
| `web/src/app/BrowserFrontendSession.ts` | WebSocket session/correlation | protocol/session | none | WebSocket send/receive/correlation | browser session | session parity | Separate transport appropriate | **Reviewed** |
| `web/src/app/BrowserStorage.ts` | Browser settings persistence | localStorage | none | Storage read/write calls | origin lifetime | supporting parity | Persistence reviewed | **Reviewed** |
| `web/src/app/ComposerKeyboard.ts` | Composer keyboard policy | event/draft | none | Keyboard-event policy calls | event scope | composer keyboard | M-24 semantics copied | **Reviewed** |
| `web/src/app/ConversationViewportState.ts` | Browser anchor/follow state | viewport metrics | none | Scroll/resize state transitions | component state | viewport parity | Separate mechanism reviewed | **Reviewed** |
| `web/src/app/Humanize.ts` | Browser formatting | strings/timestamps | none | No direct DOM input; pure/module API | pure values | model/qualification | M-24 formatting copied | **Reviewed** |
| `web/src/app/PendingRequestResponses.ts` | Request response shaping | method/payload | none | No direct DOM input; pure/module API | pure values | supporting parity | H-07/M-24 policy copied | **Reviewed** |
| `web/src/app/TurnSettingsOptions.ts` | Settings option policy | catalog/current | none | No direct DOM input; pure/module API | pure values | supporting parity | M-20/M-24 policy copied | **Reviewed** |
| `web/src/conversation/ConversationProjection.ts` | Browser conversation projection | presentation facts | none | No direct DOM input; pure/module API | pure arrays | projection parity | H-05/M-24 separate copy | **Reviewed** |
| `web/src/conversation/MiddleTypes.ts` | Browser card/pane DTOs | projection values | none | No direct DOM input; pure/module API | values | model parity | M-24 semantics copied | **Reviewed** |
| `web/src/conversation/PromptCoordinator.ts` | Browser optimistic prompt queue | session/model/prompts | none | Submit/steer/correlation transitions | browser session | model/session parity | M-24 optimism copied | **Reviewed** |
| `web/src/index.ts` | Browser exports | modules | none | No direct DOM input; pure/module API | module | tests | Support | **Reviewed** |
| `web/src/main.tsx` | Browser bootstrap | session/storage/App | React root | Page bootstrap/event loop | page | qualification | Browser only | **Reviewed** |
| `web/src/presentation/PresentationModel.ts` | Browser presentation state/projection | normalized facts | none | No direct DOM input; pure/module API | browser session | model/projection tests | M-24 parallel owner | **Reviewed** |
| `web/src/presentation/PresentationProtocol.ts` | Browser protocol types | wire shapes | none | No direct DOM input; pure/module API | values | normalizer/model | Process boundary valid | **Reviewed** |
| `web/src/presentation/PresentationStatus.ts` | Browser status classification | raw status | none | No direct DOM input; pure/module API | pure values | model parity | H-03/M-24 aliases | **Reviewed** |
| `web/src/presentation/ProtocolNormalizer.ts` | Browser protocol normalization | wire frames | none | No direct DOM input; pure/module API | pure/session | normalizer parity | Process boundary valid | **Reviewed** |
| `web/src/styles.css` | Browser visual/responsive/motion | CSS classes | DOM/CSS | No direct DOM input; pure/module API | document | responsive/qualification | Reduced-motion contrast; renderer separate | **Reviewed** |
| `web/src/vite-env.d.ts` | Vite declarations | tool types | none | No direct DOM input; pure/module API | compile | build | Support | **Reviewed** |
| `web/tests/browser-session-parity.test.mjs` | Browser behavior/parity test browser-session-parity.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | browser-session-parity.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/card-copy.test.mjs` | Browser behavior/parity test card-copy.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | card-copy.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/composer-keyboard.test.mjs` | Browser behavior/parity test composer-keyboard.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | composer-keyboard.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/conversation-projection-parity.test.mjs` | Browser behavior/parity test conversation-projection-parity.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | conversation-projection-parity.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/model-parity.test.mjs` | Browser behavior/parity test model-parity.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | model-parity.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/normalizer-parity.test.mjs` | Browser behavior/parity test normalizer-parity.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | normalizer-parity.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/qualification.test.mjs` | Browser behavior/parity test qualification.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | qualification.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/responsive-layout.test.mjs` | Browser behavior/parity test responsive-layout.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | responsive-layout.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/supporting-surfaces-parity.test.mjs` | Browser behavior/parity test supporting-surfaces-parity.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | supporting-surfaces-parity.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tests/viewport-parity.test.mjs` | Browser behavior/parity test viewport-parity.test | web model/session/DOM | Node assertions | Synthetic session/model/DOM interaction and assertions | test process | viewport-parity.test.mjs | M-24 useful contract; may preserve shared defect | **Reviewed** |
| `web/tools/profile-presentation.mjs` | Browser qualification tool profile-presentation | artifact/workload | CLI output | CLI invocation only | process | web release/profile | Support; not native proof | **Reviewed** |
| `web/tools/qualify-browser.mjs` | Browser qualification tool qualify-browser | artifact/workload | CLI output | CLI invocation only | process | web release/profile | Support; not native proof | **Reviewed** |
| `web/tools/verify-artifact.mjs` | Browser qualification tool verify-artifact | artifact/workload | CLI output | CLI invocation only | process | web release/profile | Support; not native proof | **Reviewed** |
| `web/tsconfig.json` | TypeScript compiler config | build options | none | Build/tool invocation; no product input | build | npm build/tests | Support | **Reviewed** |
| `web/vite.config.ts` | Bundler configuration | build config | none | Build/tool invocation; no product input | build | artifact tools | Packaging | **Reviewed** |

## Audit completion gate

| Completion condition | Evidence |
|---|---|
| Every current input file reviewed | 185 unique ledger rows equal 184 baseline tracked files plus later `AGENTS.md`; set comparison has no missing, extra, or duplicate input row. |
| `rg --files` closed | 182 current non-artifact ripgrep paths all occur in the ledger; the three hidden tracked paths are also present. |
| Every requested subsystem traced | End-to-end table covers shell, all thread behaviors/dialogs, all card kinds, virtualization, Markdown, command/file/image/plan/reasoning/activity, Inspector/requests, composer/settings/notices/states, and splitters/responsiveness. |
| Qt correctness and behavior states reviewed | Dedicated Qt and adverse-interaction matrices distinguish proven gaps from unresolved runtime hypotheses. |
| Duplication/history/LOC reviewed | 48 reconciled findings, major history chain, current subsystem LOC, deletion opportunities, target invariants, and phase estimate recorded. |
| Tests/performance recorded | Native, sanitizer, scale/platform/font, browser, static-analysis, and benchmark results recorded without treating passes as architectural proof. |
| Independent review reconciled | Four tracks plus adversarial review complete; primary rejected the unsupported prompt-alias claim and incorporated qualified risks/gates. |
| Worktree protected | No production/test edit was made by this audit. The same six tracked paths and exact per-file numstat remain as the recorded baseline. `AGENTS.md` was externally introduced after baseline and preserved. |
| Whitespace/diff gate | Tracked `git diff --check` passes; the audit artifact's no-index whitespace check has no diagnostic. |
| Process/install/commit gate | No install, restart, stop, commit, or push occurred. Three pre-existing CodexUI processes (started before this audit) remain untouched, so no installed-binary/stale-process or user-visual success claim is made. |

**Audit conclusion:** the requested read-only review is complete; the application is **not** claimed fixed or fully qualified. The audit now awaits user approval. Implementation may start only in a separate thread after approval of this diagnosis and deletion plan.
