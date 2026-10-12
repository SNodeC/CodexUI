<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../../media/nav/banner-findings-ledger-compact.svg"><img src="../../../media/nav/banner-findings-ledger.svg" alt="CodexUI — Findings ledger" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Findings ledger"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/up-findings-ledger-dark.svg"><img src="../../../media/nav/up-findings-ledger-light.svg" alt="Up to Findings ledger" title="Up to Findings ledger" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/organization-dark.svg"><img align="right" src="../../../media/nav/organization-light.svg" alt="SNodeC organization" title="SNodeC organization"></picture></a><a href="../../../../README.md#project-overview" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/repository-dark.svg"><img align="right" src="../../../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Consolidated findings and external-review crosswalk

## Recorded baseline verification

- `cmake --build ../build/Desktop_GCC-Debug -j2`: no work; success.
- `ctest --test-dir ../build/Desktop_GCC-Debug --output-on-failure -j2`:
  19/19 passed in 14.38 s.
- `ctest --test-dir ../build/Desktop_GCC-Debug -N`: exactly 19 registered
  tests; the conversation benchmark executable is not registered.
- Existing browser dependencies, `npm test`: 91/91 passed.
- `npm run profile`: 10,000 authoritative/visible and 2,000 deltas;
  hydrate 45.8 ms, project 34.6 ms, stream 9.57 ms.
- Existing ASan build: 18/19 passed in 20.55 s. Conversation virtualization
  failed its large-file wall-time/object assertion; there was no sanitizer
  diagnostic.
- Existing TSan nodegraph build: 5/5 passed in 6.99 s.
- Selected native suites at scale factors 1.25, 1.5, and 2.0: 8/9 passed at
  each factor. The same conversation-virtualization gray-ring pixel assertion
  failed at every fractional/high scale.

The current conversation benchmark was also sampled under the offscreen
platform. Columns are initial construction milliseconds, 240-step scroll
microseconds, tail-append microseconds, and peak RSS KiB:

| DPR | Rows | Initial | Scroll | Append | RSS |
|---:|---:|---:|---:|---:|---:|
| 1.0 | 320 | 10 | 396,272 | 451 | 46,832 |
| 1.0 | 1,280 | 31 | 463,244 | 494 | 47,084 |
| 1.0 | 10,000 | 262 | 626,060 | 558 | 67,020 |
| 1.25 | 320 | 15 | 450,030 | 468 | 47,376 |
| 1.25 | 1,280 | 31 | 506,970 | 503 | 48,368 |
| 1.25 | 10,000 | 258 | 686,136 | 523 | 68,492 |
| 1.5 | 320 | 15 | 490,127 | 469 | 49,616 |
| 1.5 | 1,280 | 34 | 543,471 | 507 | 49,856 |
| 1.5 | 10,000 | 254 | 699,690 | 523 | 70,424 |
| 2.0 | 320 | 12 | 613,199 | 472 | 53,460 |
| 2.0 | 1,280 | 33 | 657,810 | 517 | 54,280 |
| 2.0 | 10,000 | 260 | 817,174 | 546 | 74,524 |

Every sample reported zero materialized cards and eight descendant widgets.
That proves the workload measures the passive implementation rather than the
interactive component. Peak RSS also depends on launcher ancestry unless each
sample is isolated; the table uses isolated `timeout` child processes.

## Governing invariants

- **I-1 — Single presentation:** one logical component has one renderer,
  geometry authority, interaction implementation, and accessibility model.
- **I-2 — Single state authority:** protocol state belongs to `NodeGraph`;
  toolkit projections and optimistic state have one explicit owner and one
  deterministic reconciliation path.
- **I-3 — Residency-only virtualization:** virtualization controls bounded
  residency, reuse, scheduling, and cached scalar heights, never appearance.
- **I-4 — Stable interaction identity:** interaction reaches the visual object
  the user sees; stable semantic identity owns persistent local state.
- **I-5 — Semantic no-op:** a no-op cannot move pixels, reflow, change focus,
  restart animation, or replace accessibility objects.
- **I-6 — Replacement deletes:** superseded renderers, workarounds, caches,
  bridges, and migration paths disappear in the same stage.
- **I-7 — Qt lifetime/thread correctness:** graph locks never span QWidget
  work; QObject ownership, queued delivery, retirement, and focus are explicit.
- **I-8 — Shared native foundations:** `UiStyle`, native Markdown, Copy, and
  disclosure primitives are singular where semantics are shared.
- **I-9 — Inclusive UI:** visible selection, keyboard operation, accessible
  state/events, reduced motion, responsive geometry, and DPR behavior are
  architectural requirements.
- **I-10 — Enforceable qualification:** every case executes; benchmarks have
  numerical gates; builds, feature definitions, platforms, sanitizers, and
  frontend semantic fixtures reflect production truth.

In the tables below, verification is abbreviated as **C**orrectness,
**P**erformance, **A**ccessibility, and **R**egression. LOC estimates are
production/test respectively, are non-additive where findings share a stage,
and are design forecasts rather than deletion quotas.

## Consolidated critical and high findings

| ID and review provenance | Affected files, symbols, state, behavior; current-worktree evidence and classification | Invariant, dependencies, and accepted remediation | Required verification | Expected LOC and implementation status |
|---|---|---|---|---|
| **C-01** Local; Claude A “core defect”/mouse-down/complexity; Claude B R-1/R-2/R-5; ChatGPT F-QT-001/002/003/004/006/008/013/015/016 | `ConversationView.cpp`: passive presentation/delegate/document LRU/hit testing and materialization; `ConversationCards.*`: live widgets/document bridge. **Verified and architecturally replaced in the current worktree.** Dirty parity offsets did not remove the second renderer; S1 does. | I-1/I-3/I-4/I-5/I-6. Keep `ConversationCard` for visible+overscan; keep offscreen model+scalar height and view-owned Turn decoration; delete passive renderer/cache/bridge/synthesis in one stage. No pool or third presentation abstraction. | C: all card kinds, hover/focus/copy/fold/link/stream/promotion/resize/history/thread switch. P: bounded widgets/docs/work at 320/1,280/10k. A: stable visible tree/actions. R: anchors, follow-tail, DPR matrix. | The renderer-replacement checkpoint was P −767/T +790 `cloc`; approved single-authority correctness work follows in the final S1 accounting. **S1 complete.** |
| **H-01** Local; Claude B R-4/Q-3/Q-5; ChatGPT F-QT-006/011/012 | Passive rows exposed only model text while live rows exposed controls; selection was calculated but not visibly painted. **Original defect and later M-44 mouse-selection/keyboard-focus conflation are repaired.** | I-1/I-4/I-9; depends C-01. The sole live card receives normal child delivery; semantic selection and genuine keyboard focus remain distinct; S1-D05 supplies one physical accessibility tree. | First gesture, overscan work, roles/names/actions/selected/focused state, and Breeze/Fusion four-DPR focus pixels are qualified. | **Implemented in S1/MR-7; real AT-SPI remains S8.** |
| **H-02** Local; ChatGPT F-QT-015; Claude B R-5 adjacent | Old passive, collapse, ordinal interaction, and command-scroll maps could outlive model identity; height lacked runtime environment invalidation. **Defects verified and fixed.** | I-2/I-3/I-4/I-7; C-01 before cache audit. Retire remaining keyed scalars/state on exact removal/trim/forget; normalize content transitions once; invalidate height on authoritative content/width/font/style/DPR changes. | C: remove/trim/switch/reinsert/shorten/regrow identities. P: map/card/doc counts bounded over churn. A: focused object retained; semantic state only. R: collapse and command follow state. | Old maps/ordinal topology are deleted; S1-D03/D04 cover the remaining transition/environment paths. **Implemented.** |
| **H-03** Local; Claude A status drift; Claude B A-4/A-8; ChatGPT status inventory | Native and browser alias, lifecycle, plan, and visible-tone classification originally disagreed. **Verified at baseline; repaired in the current worktree.** `ProtocolUpdater::statusFromValue` is the native ingestion authority, `UiStatus` is typed presentation policy, and the browser implements the same meanings against the shared corpus. | I-2/I-8/I-10. Project canonical typed status once, retain unknown raw labels without assigning semantics, and delete repeated adapter/widget classification. Separate renderers consume one data contract rather than sharing renderer code. | C: full alias/status/lifecycle/plan corpus executes in C++ and TS. P: no repeated paint-time normalization. A: native visible tones consume typed policy. R: card/thread/agent/request status. | Included in combined P −142/T-data +932. **S2 complete; shared corpus passes both runners.** |
| **H-04** Local | Shell `historyPageAwaitingProvider` was set on admission but runtime failure only erased correlation and emitted a notice, suppressing retry. **Verified and fixed.** | I-2/I-5. The graph `Operation` targeting the thread is the sole in-flight fact; Shell/runtime duplicate pending guards are deleted and the view retains only a demand watermark. | C: admit/success/failure/disconnect/stale/duplicate/retry. P: no polling. A: enabled/busy/failure announcement. R: pagination/anchors. | **Implemented in S3; focused and full suites pass.** |
| **H-05** Local; Claude B A-2/A-8 | Adapter full/exact-row/tail/prompt projections plus Shell placement logic repeated ordering, hierarchy, visibility and promotion. **Verified and fixed.** | I-2/I-5/I-6. One adapter projection/placement algorithm supplies full snapshots and exact deltas; Shell only coalesces/routes; one view transaction applies a delta. | C: full vs incremental differential corpus; reorder/append/remove/promotion/history. P: bounded rows scanned/copied per delta. A: stable object identity. R: per-thread viewport state. | **Implemented in S3.** Exact root morph and bounded-delivery tests pass; no renderer/cache/timer was added. |
| **H-06** Local; Claude B A-1/A-8 | Shell creation/selection retry state, ThreadPane optimistic overlay, and WorkerLogic admitted graph lifecycle owned the same new-thread presentation. **Verified and fixed.** | I-2/I-4/I-6. WorkerLogic/graph own facts after admission; pre-admission draft stays Qt-owned; `creationCorrelation` preserves one row identity through canonical promotion. | C: admit/confirm/fail/reconnect/duplicate/rapid create/selection. P: no duplicate lifecycle/retry work; the whole-viewport animation loop was M-09, not H-06. A: stable row/focus/state. R: draft restoration and steering. | **Implemented in S3.** General cross-surface focus policy remains M-04. M-09's viewport-paint defect is complete under H-11; its reduced-motion remainder belongs to H-14. |
| **H-07** Local; Claude B A-1/A-8 | Method-to-kind, actionability, payload shaping, recovery, Shell controls, adapter rows and dialog response logic overlapped. **Verified and consolidated.** | I-2/I-6/I-8. Existing typed policy/descriptor is the native authority; Inspector/Shell/dialog consume it and runtime revalidates the same policy at the wire boundary. | C: every method/action/payload, stale/disconnect/repeat/bounds. P: bounded review work and no stored derived cache. A: names, enabled/busy, error focus. R: approval and user-input dialogs. | **Implemented for the verified S3 contract.** Approved growth is recorded above. Standard-MCP schema/content and file-patch revision safety remain separate unapproved additions. |
| **H-08a** Local; Claude B A-7 (remedy superseded) | `Node::uiAttachment_`, `QtNodeAttachment.h`, attachment-probe/retry branches, CMake references, 3,148 current-worktree disabled direct-widget lines, and their remaining definition-only fixture/budget helpers had no production use. **Verified obsolete residue and deleted.** | I-6/I-7. The opaque graph-to-toolkit dependency and dormant implementation archive are gone; no side table replaced them. | C: graph/session retirement without attachment; all eight affected suites pass. P: no extra scan/state. A: N/A. R: real queued change/rescan order. | **Implemented in S4 checkpoint: H-08/X-05 native P −58; native T −3,305.** |
| **H-08b** Local split; Claude B positive retirement finding | Retired-node membership and `UiDetached` acknowledgement are directly proven to keep a node readable when a synthesized rescan overtakes an older queued `GraphChanged`. **Verified required, not residue.** The parallel full-vector read API was not required and is deleted. | I-7. Preserve the bounded scan, generation cursor, backlog barrier, per-kind dedupe/backpressure, and revision-neutral release. This is graph queue-order authority, not widget lifetime state. | C: observed rescan-before-stale-callback order and post-release membership/lifetime distinction pass; shutdown/saturation remain broader qualification. P: 64-node slices and 640-node completion pass. A: N/A. R: TSan and full nodegraph suite remain S8. | Core protocol preserved with a smaller read surface and no redundant in-drain timer wake. **Eight affected suites pass; no compatibility renderer/attachment remains.** |
| **H-09** Local | Adapter Inspector projection scanned/copied all rows and active/visited tabs retained every row widget despite the documented 48+overscan contract. **Verified at entry and repaired.** | I-2/I-3/I-6. One indexed graph projection feeds one shared variable-height residency/geometry/anchor/focus/accessibility owner; lossy caps, local caches, passive delegates, and retry cursors are absent. | C: lossless scroll/expand/update/request/diff identity; P: row/widget/document/scan/per-frame bounds at 10k; A: exact visible/overscan semantics/focus/tree; R: all tabs and four DPRs. | Combined H-09/M-28 P **+1,560** under the approved +1,600 ceiling; T **+4,685**. **Complete in S5; Release 39/39, Inspector 9/9, targeted ASan+UBSan 10/10.** |
| **H-10** Local; Claude A/B Copy/disclosure/Markdown duplication; ChatGPT F-QT-009/010 | Inspector owned a third native Markdown path, duplicate Copy/disclosure/status shells, rebuilt Agent descendants, and did not authoritatively prune expanded IDs. **Verified and repaired.** | I-1/I-4/I-6/I-8/I-9. Card and Inspector consume the sole Markdown, Copy, disclosure, and status implementations; retained semantic children patch in place. H-09/M-28 later corrected H-10's false-positive expansion-pruning claim with bounded graph validation. | C: Plan/Agent/request content, fold/copy/link/selection/status/thread switch; P: zero document mutation for status/no-op and bounded retained objects; A: stable focus/name/tree, with explicit state events still M-21; R: tabs/streaming/DPR. | H-10 slice P −68/T +161 including combo consolidation; expansion retention completed under H-09/M-28. **Complete; accessibility events remain M-21.** |
| **H-11** Local | `ThreadPane::refresh` created five row QWidgets plus a layout for every presented root or expanded descendant and split appearance between those widgets and a delegate. **Verified and repaired.** | I-1/I-2/I-3/I-4/I-9. One stable `QTreeWidget` item hierarchy and one full-row delegate replace all index widgets, flattened state, manual hierarchy input, and rebuild restoration. Model-backed accessibility wrappers project that same tree, never a shadow hierarchy. | C: hierarchy/sort/paging/context/selection/promotion/reincarnation/anchors. P: N-independent QWidgets, zero index widgets, row-local paint/timer work at 10k. A: exact parent/level/expanded/action/focus/identity. R: four DPRs and keyboard navigation. | Final P **+214** under the separately approved +1,600 ceiling; T **+954**; config **+40**. **S5 complete: Release/Debug/ASan H-11 18/18, full Release 54/54.** |
| **H-12** Local | Review and embedded diffs each owned a provider, 2 s poll, and presentation; review eagerly owned three documents. **Verified and fixed.** | I-2/I-4/I-6/I-10; pair with M-25/M-26. One `DiffViewer` provider/poll/snapshot feeds embedded and review surfaces; only the selected review layout retains text and stable viewport state. | C: refresh/switch/error/stale/close. P: one in-flight job, one poll, lazy active document. A: stable cursor/selection/tree. R: unified/split/embedded modes. | **Implemented.** M-26 retains only the repository-wide combined idle-wakeup measurement. |
| **H-13** Local | Composer remove rebuilt every row synchronously while handling the emitting button. Lifetime hazard and focus loss were **verified and fixed**; a crash was never reproduced. | I-4/I-7. The exact stable path row is removed in place; only QObject destruction is deferred through Qt, and focus moves deterministically to adjacent Remove/Attach. | C/P/A/R qualified in the four-DPR composer live gate; exact mutation is O(1) in widgets and does not rebuild siblings. | **Implemented with M-10; live-qualified and temporary probe removed.** |
| **H-14** Local; Claude A Copy primitive concern | Pending sweep, spinners, follow-tail, Thread sweep, and Copy previously had inconsistent motion policy. **Verified and fixed.** | I-2/I-8/I-9. Every native motion surface consumes the existing Qt style-hint fact through `UiStyle::animationsEnabled`; static semantic feedback remains when disabled. No new policy owner exists. | C/P/A/R covered by the existing motion paths and source audit. | **Implemented in MR-9A.** |
| **H-15** Local; Claude A tests; Claude B B-3/B-4/T-2/T-4/T-5/T-7; ChatGPT F-QT-012/014/017 | Seven default mains short-circuited, focused cases were not in the default path, pixel helpers were DPR-invalid, and CI was one Ubuntu/GCC/Debug lane. **Source structure repaired:** every multi-case main now accumulates results, focused cases are present in the default suite, DPR helpers are corrected, and twelve quantitative profiles are registered. | I-9/I-10. Preserve non-short-circuiting named cases and quantitative gates; add native-platform/AT-SPI and CI execution breadth without claiming unavailable evidence. | Compile-only GCC Release/Debug/ASan and Clang Release are clean; prior executed DPR profiles are recorded. Native compositor/AT-SPI and new CI runtime lanes remain prohibited/unavailable. | **Implementation complete; only S8 execution-platform qualification remains.** |
| **H-16** Local current-worktree-only | Dirty native patch grew production/test LOC, added passive colors/offsets/dynamic focus state and parity tests, and retained both renderers. **Verified and superseded without discarding user intent.** External reviews did not inspect it. | I-1/I-5/I-6. Preserve valid mouse/keyboard focus and check-centering intent; supersede passive offsets/colors/tests through C-01 deletion. Never reset or hide the user patch. | C/A: Copy focus and glyph geometry on sole widget. P: included C-01. R: web half-pixel change preserved. | Included C-01. **Implemented in S1; the user-owned web alignment remains untouched.** |

## Consolidated medium findings

| ID and provenance | Affected evidence and classification | Invariant, dependencies, accepted remediation | Required C/P/A/R verification | Expected LOC and status |
|---|---|---|---|---|
| **M-01** Local | Typed `UiEffect` delivery coexisted with graph fallback/dedup in Shell. **Verified and repaired.** Notice and selection are now authoritative graph state; only metadata-only protocol diagnostics and terminal stop remain transient. | I-2/I-6/I-7. Delete the competing stream, its enum, reserved slot, fallback and dedupe. Related prompt/selection/notice facts commit in one graph revision. | C compile-time consumer migration plus saturation/FIFO source audit; P no retry/dedupe churn; A existing Notice projection; R runtime stop/errors. | Production mechanism deleted; test sources now assert graph authority. **Implemented; all targets compile warning-clean, live notice/a11y surface qualified, no test binary executed by current instruction.** |
| **M-02** Local | Active turn was derived from both `RelationKind::ActiveTurn` and status fallbacks in adapter/Shell. **Verified at baseline; repaired.** | I-2/I-6; H-03 first. Native graph relation and browser event-ordered `activeTurnId` are their respective typed lifecycle authorities; duplicate status derivation and thread-status rewriting are deleted. The documented provisional native turn remains presentation of admitted local graph state, not provider identity. | C start/complete/interrupted/stale and opposing event orders; P one relation/fact lookup; A busy/current state; R turn cards/actions. | Included S2. **Complete; shared lifecycle corpus passes.** |
| **M-03** Local | Inspector fell turn→thread status for plan steps while conversation used item status. **Verified at baseline; repaired.** | I-2/I-8; H-03 first. One canonical effective-plan-status projection is consumed by native conversation, native Inspector, and browser presentation. | C status/plan replacement corpus; P no paint normalization; A same status text/tone; R empty/streaming/final/malformed plans. | Included S2. **Complete; native and browser cases pass.** |
| **M-04** Local | Focus/selection restoration and modal lifetime were originally split across retries and retained nodes. Optimistic-create retries were deleted in S3; the later verified rename nested-loop lifetime risk is now fixed. | I-2/I-4/I-5/I-7. Each transition uses its existing owner/destination. Rename revalidates graph membership after the modal and preserves authored text only for an unadmitted live request, with no retry flag. | C switch/hide/modal removal/queue-full/rebuild/recycle; P no retry timers or stale nodes; A focus order; R keyboard/mouse. | **Implemented.** Cross-surface visual-focus breadth remains M-30, not M-04 ownership work. |
| **M-05** Local | Shell installed a runtime-stopped handler, `main.cpp` overwrote the same slot, and Shell destruction cleared it. **Verified and fixed.** | I-2/I-7. Application lifetime owns the one handler and routes the effect once; competing Shell ownership is deleted. | C stop before/after Shell destruction; P N/A; A notice once; R shutdown/reconnect. | **Implemented in S3.** |
| **M-06** Local | Mutating actions inconsistently represented in-flight/single-flight state. History and request response gaps were **verified and fixed**. The S4 Shell inventory found no remaining Shell-owned provider in-flight flag: surviving fields are bounded presentation delivery, pre-admission authored input, or semantic identity. | I-2/I-5; derive pending from each authoritative operation, preserving intentional multi-flight prompts; do not add generic Shell busy flags. Repeated-action feedback absent from the current contract is not deletable duplicate state. | C admit/reject/timeout/disconnect/stale/repeat; P no poll; A enabled/busy/result; R stop/connect/thread/request actions. | **S3 implementation complete; S4 responsibility inventory complete. Any broader action-busy contract remains M-07/S5 and approval-gated if it grows production.** |
| **M-07** Local | Manual flat rebuild/current-index restoration was **verified and deleted**. Relative-time aging is **disproven** (timestamps are absolute); a pane-local pending latch is **superseded** by Runtime cursor/single-flight authority; root sorting plus provider child order is the documented contract. | I-2/I-4/I-5; H-11 deletes manual restoration and preserves the one Runtime paging authority and intentional ordering split. | C hierarchy/root-sort/child-order/rapid page requests/stale cursor; P bounded item updates; A stable tree order/focus; R context selection. | Included H-11. **S5 complete.** |
| **M-08** Local; Claude B R-3/Q-7 adjacent; ChatGPT F-QT-010 | Thread hierarchy was visually indented list data rather than a semantic tree; Left/Right/hit testing were custom. Stock Qt 6.6/6.10 tree accessibility is also flat and its Toggle action does not expand. **Verified and repaired.** | I-1/I-4/I-9; H-11 supplies one native tree controller plus model-backed semantic accessibility over the same items, not delegate/widget or model/shadow-tree parity. | C expand/select/context; P bounded; A role/level/parent/expanded/collapsed/action/identity; R mouse/keyboard and supported Qt. | Included H-11. **S5 complete; real AT/platform breadth remains S8.** |
| **M-09** Local | A 32 ms timer ran for any optimistic row, including offscreen, and repainted the entire thread viewport. **Verified and repaired.** | I-3/I-5/I-9; H-11 retains the existing timer only for affected visible row rects; H-14 supplies the one reduced-motion authority. | C visible/offscreen transition; P paint/wakeup bounds; A static non-motion state; R scrolling/paging. | **Implemented in H-11/H-14.** |
| **M-10** Local | Composer reserve, overlay, trailing inset, one-shot canonical height, and composer-growth pause duplicated geometry/scroll authority. **Verified and fixed.** | I-2/I-5/I-6/I-9. Composer is one normal layout child; reserve/anchor/trailing/settlement machinery and secondary pause cause are deleted. | Four-DPR live gate proves grow geometry, no overlap, focus, scroll/edit latency, and follow/user-pause behavior. | **Implemented with H-13; production reduced.** |
| **M-11** Local | File selection performed content MIME probing on the GUI thread. **Verified and repaired.** The remaining `QFileInfo`/extension classification loop is bounded to 16 entries and does not read file contents. | I-3/I-7/I-10. Delete content probing; keep metadata-only selection. A total-byte rejection changes accepted product behavior and is not an architectural UI-performance fix. | Source audit confirms `MatchExtension` only and no file-content read/decode on the GUI path. | **Implemented by reduction; no async subsystem or new policy.** |
| **M-12** Local | `ImageThumbnail` synchronously decoded and cached by path/size/time without DPR/physical target. **Verified and repaired.** | I-3/I-5/I-9. One DPR-correct cache remains; only QImage decode runs in the global worker pool, with one queued GUI completion, one stale key, and QObject lifetime gating. | Four-DPR offscreen/Xvfb live gate: correct logical/physical geometry, single-image max 2 ms GUI gap, maximum-16-image max 4 ms gap/one height reconciliation; destroyed in-flight widget passes ASan/UBSan. | **Implemented. Checkpoint delta P +52 CLOC / T −98 CLOC; no second renderer/cache/timer and probe properties deleted.** |
| **M-13** Local; Claude A raw colors; Claude B A-3 | Reusable literals/local sheets and conflicting tooltip policy bypassed tokens. **Verified and fixed.** | I-5/I-8/I-9. Reusable native colors live only in `UiStyle`; existing Qt font/style/DPR events invalidate geometry, with no theme epoch/cache. ThreadPane pixels were preserved. | Source scan finds no raw reusable hex outside token definitions; runtime-environment qualification is covered by the existing event path. | **Implemented in MR-9B.** |
| **M-14** Local | `ContentSizedTextView`, `CommandOutputView`, and MiddleRegion repeated wheel gesture ownership while command follow-tail was separately authoritative. Composer/attachment gestures could also change owner after crossing surfaces. **Verified; the first S1 centralization was reopened and finally fixed in MR-6.** | I-2/I-4/I-6. Explicit command follow/detach state remains; `MiddleRegionWidget` is the one nested/composer scroll-gesture owner, duplicate editor wheel handlers/flags are deleted, and retargeting preserves the original input metadata. | C pixel/angle/touchpad/nested/composer/attachment edges and End; P registered direct/routed gate and no duplicate handling; A keyboard scrolling; R manual detach/follow. | **Complete in MR-6.** P -22/T +260; Release 26/26 plus performance 29/29 pass. Native input/frame breadth remains MR-8. |
| **M-15** Local; Claude A/B renderer geometry; ChatGPT F-QT-004 | `measureCard` repeatedly invalidated/activated layouts, removed `LayoutRequest`, and event filters consumed/re-measured requests. The mechanism was **verified and deleted**; missed descendants were never reproduced. | I-1/I-5/I-6/I-7. The sole card's settled `heightForWidth` result and `ConversationHeightIndex` are the remaining renderer/row geometry authorities; semantic no-op and late geometry tests constrain Qt ordering. | C width/content/no-op/late child; P measurement/frame counts; A no focus/tree replacement; R anchors/DPR. | Included C-01/S1-D02/D04. **Implemented in S1.** |
| **M-16** Local | Copy completion was visual-only and zero-width cleanup could replace rich MIME with plain text. **Verified and repaired.** Qt's clipboard setter exposes no failure result, so a native “copy failed” branch is not implementable evidence and is not fabricated. | I-4/I-8/I-9. One shared Copy primitive publishes plain/Markdown data and announces success; native selection cleanup preserves text, HTML, Markdown, and rebuilt ODF MIME. | Reduced-motion Copy retains static feedback; the shared announcement path provides the semantic event. | **Implemented in MR-9A/MR-9D; duplicate primitives/timers are absent.** |
| **M-17** Local; ChatGPT F-QT-007 | Dematerialization stored selection by child ordinal and incomplete focus/direction. **Original defect verified and fixed.** | I-3/I-4. Focused cards remain residency-pinned; recyclable state is keyed by stable card plus semantic role, bounded source fingerprint, and directional cursor, and S1-D03 normalizes owner transitions before restoration. | C selection direction/fold/recycle/no-op/truncate/disappear/regrow; P bounded keyed state; A focus identity; R all selectable card kinds. | **Implemented in S1.** |
| **M-18** Local | FileChangesView used custom cursor-range mouse/key hit testing and discarded open failures. **Verified and repaired.** | I-4/I-9. One `QTextBrowser` now owns native mouse/keyboard/hypertext accessibility; open failure uses the existing notice authority. The custom event/state path is deleted. | C source path and production compilation; P bounded native document; A native link contract; R mouse/keyboard live breadth remains S8. | Net production reduction in the containing stage. **Implemented; all targets compile warning-clean.** |
| **M-19** Local | Mouse-only request links, unbounded recursive forms, weak invalid-focus, stale modal data, and ephemeral-name clearing were verified; blanket “all labels unbuddied” was disproven. | I-2/I-4/I-9/I-10. Existing policy bounds forms/disclosure, native controls provide keyboard access, first-invalid focus is explicit, and Shell re-reads current request after the modal. The remaining destructive ephemeral-name clear is deleted. | Four-DPR live dialogs prove name preservation, invalid focus ownership, and Unicode submission; the offscreen plugin's absent active top-level is not patched. | **Implemented; production −2 physical lines in final slice.** |
| **M-20** Local | `TurnSettingsWidget` owned catalog interpretation, compatibility filtering, touched/default omission, and outbound JSON beside QWidget controls. **Verified and repaired.** | I-2/I-6/I-10. Native `TurnSettingsPolicy` and browser `TurnSettingsOptions` are the sole frontend-local value policies; widgets/React controls only project and author edits. Shell/raw copier, widget serializer, inline React policy, and duplicate workspace precedence were deleted or consolidated. | C shared schema-v5 corpus in C++/TS plus exact native/browser wire flows; P linear catalog and zero-work semantic no-op; A labels/buddies/names/disabled reasons; R thread/start, turn/start and steer. | Last separable P −31; final precedence correction P 0; T +679 native/+171 browser/+315 fixture plus +91 browser qualification tool. **Complete in S5; Debug 31/31, Release 31/31, browser 282/282 and full browser release gate pass.** |
| **M-21** Local | Custom disclosure state and Copy/notice completion lacked exact native accessibility events. **Verified and repaired after scope reduction.** Built-in QWidget text/value/busy changes retain Qt's native event authority; a second coordinator would duplicate it. | I-1/I-4/I-9. One shared Button-role disclosure interface emits exact expanded/collapsed transitions; one version-compatible announcement helper serves Copy, notice, request, and attachment completion. | Live offscreen/Xvfb probe observed one event for a transition, none for a no-op, and one announcement; real AT-SPI remains S8. | **Implemented in MR-9D; no event coordinator or shadow accessible object.** |
| **M-22** Local; Claude A benchmark note; Claude B T-6; ChatGPT F-QT-014 | Baseline benchmark was unregistered, ungated, read a stale metric, and exercised zero cards. **Verified at baseline and corrected.** ChatGPT’s historical “registered among 20” claim remains disproven. | I-3/I-10. The benchmark drives the sole renderer and is registered for 12 row/DPR profiles with exit-gated median/p95/max, pixels, identity, exact residency, staging, documents/widgets, construction, streaming, resize, sparse traversal, height steps, event work, and RSS. Test-side observation avoids a new production metrics authority. Individual Qt event-dispatch duration and document-mutation locality are not mislabeled as whole-frame or parser CPU. | C checksum/exit gating; P p50/p95/max, cards/docs/constructions/measurement proxies/frame/RSS; A N/A; R row counts/DPR. | P 0; benchmark physical +1,281/−73 at final S1. **Implemented; Debug, Release, and ASan/UBSan matrices pass.** |
| **M-23** Local | Conversation, Inspector/thread bounds, platform/a11y qualification docs, and old dark screenshots contradicted current code or execution. **Verified and repaired for active documentation.** | I-6/I-10. Current architecture docs describe the sole renderer and explicit offscreen/native limits; audit/review inputs remain immutable historical evidence rather than current design claims. | Source-term crosscheck finds deleted renderer mechanisms only in the historical audit/reviews and this ledger's provenance. | **Implemented; native-platform limitations remain explicit S8 evidence gaps, not stale claims.** |
| **M-24** Local; Claude A false parity; Claude B A-8/T-3; ChatGPT POS-005 qualifier | Native/TS status, DTO fields, request/settings/projection/optimism meanings were copied; `*-parity` tests ran TS only. **Status/plan/lifecycle and false-parity portions were repaired in S2; request semantics and native projection/optimism ownership were repaired in S3; settings meaning was repaired in S5.** Separate native/browser renderers remain valid. | I-2/I-10. Shared data fixtures execute status, plan, lifecycle, request, and settings meaning in both implementations; frontend-local suites have honest names. | C shared corpora and exact boundary fixtures in both runners; P bounded values/linear catalogs; A shared labels/states where intended; R both frontends. | **S2/S3/S5 semantic slices complete; only M-29/platform breadth remains for S8.** |
| **M-25** Local | Diff refresh replaced documents/lists; review held three copies and lost horizontal/cursor/selection state. **Verified and fixed.** | I-4/I-5/I-6/I-9; H-12. Only the active layout retains text; same-file refresh preserves semantic cursor/selection and both axes; semantic no-ops do not replace text. | C selected file/layout/update; P docs/highlighters/builds; A stable text position; R split/unified/embedded. | **Implemented with H-12.** |
| **M-26** Local | Worker/Shell/thread/conversation/diff recurring timers lacked a combined idle-work budget. **Verified and resolved.** Duplicate review provider/poll is deleted and hidden diff polling stops. | I-3/I-9/I-10. A live 2.2 s inventory observes only the documented eventfd recovery poll: 22 bounded checks; all presentation timers are inactive. A timer manager is rejected. | C state progresses; P exactly 10 recovery wakeups/s and zero idle presentation wakeups; A reduced motion; R reconnect/loading/follow. | **Implemented and measured.** |
| **M-27** Local | 1100x700 window minimum, pane minima, fixed controls, and splitter constraints conflicted with narrow/magnified use. **Verified and fixed by reduction.** | I-5/I-9. Delete the window/pane floor and let existing layouts expand text controls and yield labels; no responsive UI fork. | Live 800×600/1100×700/1536×960 at 1.0×/1.5× fonts; panes/Hide reachable and normal splitter remains 282/834/404. | **Implemented; ThreadPane styling unchanged.** |
| **M-28** Local | Inspector adapter scanned all descendants and Shell retry/full projections compounded copies without quantitative scan bounds. **Verified at entry and repaired.** | I-2/I-3/I-10; H-05/H-09/H-10. Graph-owned Plan/Agent selectors and Runtime-owned Request order project only the demanded bounded page; full fallback vectors/scans are deleted. | C complete visible facts; P values, projections, page bursts, scaling, and frame work quantitatively bounded; A no omitted visible semantics; R hydration/retry. | Included in combined H-09/M-28 P +1,560/T +4,685. **Complete in S5.** |
| **M-29** Local | Cross-surface Unicode/grapheme/RTL/IME coverage was absent beyond focused conversation cases; no general product defect was reproduced. | I-9/I-10. Existing bounded authoritative boundaries remain; no per-widget truncation copies are added. | Fusion/Breeze × four DPR live probe renders Arabic/Hebrew/combining/emoji/CJK and accepts real IME commit. Durable automated breadth remains S8 under the no-test instruction. | **Current product behavior live-qualified; persistent coverage blocked by current workflow constraint.** |
| **M-30** Local | Global button focus changed border width 1→2 px and therefore could change contents geometry; other focus risks were narrowed by prior MR-7/MR-9 work. | I-4/I-5/I-9. Use a color-only transition on the existing border; retain semantic mouse selection and genuine keyboard-focus decoration. | Fusion/Breeze × four DPR: 40 visible tab-focusable controls keep exact geometry, 27 visibly change pixels. Native plugins remain S8. | **Offscreen product behavior implemented and live-qualified; no ThreadPane visual change.** |
| **M-31** Local; Claude B B-1/B-2 | CMake repeated production sources and transport feature definitions differed between app/tests. **Verified and repaired.** Every production `.cpp` is now listed once through reusable targets; one helper propagates TLS/RFCOMM/WebSocket definitions to relevant consumers. | I-6/I-10. Preserve the reusable-target boundary and consistent feature context. | C compile all targets/features; P duplicate compilation removed; A N/A; R Release/warnings clean, sanitizer/CI breadth remains S8. | Product source LOC 0; root CMake is net −9 physical lines at checkpoint. **Implemented; compile-only all-target gate passes.** |
| **M-32** User-reported current native failure; local source/runtime trace | Shell's failed snapshot is intentional for a genuine current read error, but `WorkerLogic::completeThreadHydration` discarded the reducer's exact accepted Operation and independently made merged Item completeness a second result authority. **Verified and repaired.** | I-2/I-3/I-5/I-6. `AppliedMessage::primary` is the sole completion authority; rejected completions are inert; the graph-wide Item veto and schema-invalid failure expectation are deleted. Authoritative `historyMode` chooses `thread/read` versus `thread/turns/list`; no version/error retry fallback exists. | C exact/stale/reused/wrong-method/wrong-thread success and error plus live legacy/paginated observer selection; P O(1) completion at 2,526+ items and full quantitative matrix; A unchanged loading/error representation; R installed bridge 1.0-rc1 + app-server 0.154 + exact-current CodexUI xcb. | **P −33; T +195. Complete in S5.** Unit, benchmark, synthetic-xcb, and live controller/observer paginated gates pass; the installed/running bridge allowlist is verified. |
| **M-33** M-32 local quantitative verification | A single newest-leaf Operation retirement copied/scanned the complete live graph. **Reproduced pre-fix at 84,299,100 ns / 2,349,936,111 ns (2,000/40,000 Items, 27.9×); the reverse-index implementation measures 2,503,657 ns / 2,893,759 ns (1.16×), with post-reduction direct Debug tail removal at 981,528 ns / 1,123,057 ns and Release at 108,839 ns / 150,598 ns.** | I-3/I-6/I-7/I-10. The private derived incoming edge index is inside the sole graph relation authority and global inbound discovery is deleted; direct topology and only the movable insertion-order suffix are mutated after fallible preparation. Independent correctness/reduction review and graph tests find no concrete defect or equally local smaller mechanism. | C every relation mutation/removal shape and exact revisions; P small-retirement ratio gate plus batch/full matrix; A N/A; R queued retirement lifetime, saturation, runtime/hydration. | **Complete in S5 under the explicit +55 production ceiling.** Final reduction: P −1/T −17 from the candidate; narrow M-33 P approximately +55/T approximately +169. Co-mingled whole-file totals remain separately disclosed rather than invented as exact stage history. |
| **M-34** User-reported live native failure; direct screen/process/persistence inspection | An acknowledged non-root steering `UserMessage` is absent from the live model while persistence retains it. The current opening Turn root remains present. **Failure and both faulty boundaries verified and repaired:** snapshot and incremental history had retained only a raw suffix/root. | I-2/I-4/I-5/I-7. One shared landmark rule retains newest N authoritative rows plus every loaded `You` and the view-owned root; only unloaded provider pages remain pageable. Root-only metadata/trim is replaced without a cache/index/renderer/timer. | C multi-turn and same-turn steering, optimistic promotion, 80/160 paging, switch/anchors; P 10k/40k projection, bounded trim/residency and 12 DPR profiles; A exact `You` identity/content; R active/historical threads. | **Complete in S5. P 36,207 (−2); T 40,500 (+615). Debug 16/16, Release 4/4 + 12/12 performance, ASan/UBSan 4/4 + 12/12 performance pass.** |
| **M-35** User-reported live performance regression; local history/A-B profile | The installed current Debug build restored the old full-widget scroll cost: current normal 8.909/11.319/11.666 ms and seek 60.609/64.061/64.518 ms at 10k; historical passive rendering was 8.7x--11.6x faster than full widgets. **Verified uncommitted S1 regression and repaired:** hidden overscan construction no longer runs synchronously in the wheel handler; the height index was ruled out. | I-1/I-3/I-4/I-9. The sole `ConversationCard` and lazy residency remain. One existing scheduler admits hidden overscan nearest-first, one card per callback, while visible coverage stays synchronous; no second renderer or latency workaround exists. | C real progressive wheel/anchor/focus/callback destruction; P handler latency plus widget/document/parse/measure/materialize/paint and exact admission bounds at 320/1,280/10,000 and four DPRs; A hidden residents excluded and no focus/tree churn; R committed versus dirty history. | **Complete in S5. P 36,207 (neutral); T 40,919 (+419 from M-34, +380 after failing-first). Debug, Release, and ASan/UBSan 13/13 registered matrices plus exact focused rebuilt gates pass.** |
| **M-36** Two user screenshots plus live geometry/history inspection | `014544` freezes the 220 px output construction maximum although six lines need about 93 px; `014741` freezes a wrapped command too short. **Verified and repaired:** uncommitted card-local settlement exported height before nested layouts reached final widths, then child resize hints competed with the cached scalar. | I-1/I-4/I-5/I-8. One synchronous card-local transaction assigns stable viewport widths, settles inner and outer layouts, then exports one scalar. Construction maxima and resize-driven ancestor propagation are deleted; no posted-event removal, offset, timer, or kind workaround exists. | C short/multiline command, short/medium/long/live output, initial/staged/fold/follow/evict/resize/switch; P one bounded settlement/no scroll remeasure; A stable bounds; R four DPRs/no-op pixels. | **Complete in S5. P 36,203 (−4); T 40,988 (+69). Debug, Release, and ASan/UBSan registered 14/14 matrices plus four-DPR focused settlement pass.** |
| **M-37** M-34 adversarial source audit | A delayed `thread/turns/list` snapshot could delete a fully authoritative post-request completed item because replacement ignored the request boundary. No such overlap occurred near M-34, so this was a **verified source risk, not the reproduced live cause**. Existing placeholder-removal expectations disproved blanket preservation. | I-2/I-6/I-7. The existing request revision now protects only typed direct lifecycle Items with a valid post-request start/completion field; both paginated and full reads use it, while rollback/revert remain authoritative. No retry, tombstone, cache, recursive preservation, or second membership owner exists. | Completed-`userMessage`, started/completed status-independent cases, malformed/status/delta rejection, included merge, whole-Turn root/count/order, rollback/revert, and unrelated-graph scaling pass. | P **0**; T **+304** (forecast disproven). Exact-current Debug, Release, ASan/UBSan, and TSan pass. **Complete.** |

## Distinct external findings without a one-to-one local ID

| ID; original review | Classification and current evidence | Accepted disposition, verification, LOC/status |
|---|---|---|
| **X-01; Claude B A-1 — ShellWidget concentration** | **Verified diagnosis, materially reduced.** S3 removed duplicated policy/optimistic owners. S4 deleted the competing `scheduleRender` timer/flag; pane commit is now the sole chrome coalescing/retry scheduler. Remaining routing helpers are coupled to later Inspector/thread/effect findings, and remaining pending fields are delivery or authored-input state rather than duplicate provider truth. | I-2/I-6. Continue deleting dependent ownership; Claude’s proposed four-class extraction is **superseded** because moving the same state is not simplification. The S4 scheduler slice is P −15 and complete; further responsibility reduction belongs to H-09/H-10/H-11/M-01/M-04 in S5/S6. |
| **X-02; Claude B A-2 — ConversationView concentration** | **Verified historical diagnosis and materially reduced.** S1 deleted the renderer fork, synthetic interaction forwarding, passive document/cache ownership, and duplicate damage planner. S4 deleted view-owned active-Turn repair/cache/model mutation and active-thread mirroring in `threadStates_`; other residency, geometry, staging/history, and Turn-decoration responsibilities are evidence-backed and cohesive. | I-1/I-2/I-6. Graph/adapter/model own active-Turn projection; live scalars own active viewport state; the map owns inactive threads only. No class-only split; lazy residency and bounded geometry remain. **S4 implemented: P −93/T +141; full 31/31 and 12/12 quantitative matrix pass.** |
| **X-03; Claude A palette note; Claude B A-3 — token bypass** | **Verified and repaired** as M-13: reusable colors are `UiStyle` tokens, conflicting/local reusable sheets are consolidated, and ThreadPane pixels are preserved. | I-8. Turn-decoration ownership remains in the view; token ownership remains in `UiStyle`. **Implemented in MR-9B.** |
| **X-04; Claude B A-5 — `VisibleCardData` identity duplication** | Flat thread/turn/item coordinates syntactically overlap `AuthoritativeItemKey`, while `LocalPromptKey`, semantic coordinates, stable visual identity, and native `NodeRef` action targets have different lifetimes. **Partly verified and resolved:** unreachable `TurnPlanKey` plus dead parameters/copies are deleted; the blanket redundancy claim is disproven by current promotion/action tests. | I-2. Keep only identities with distinct semantic lifetimes and document native-only handles; do not add mechanical variant accessors. Shared prompt-promotion and target-identity cases verify the retained distinctions. **S2 complete, net production reductive.** |
| **X-05; Claude B A-6 — routine exception liveness flow** | **Verified at baseline and implemented.** S3 replaced one route; S4 deleted the remaining Shell catch after `contains(node)` and a second membership branch made unreachable by the same outer predicate and held `ReadAccess`. | I-7/I-10. Nonmembership uses one boundary probe; core graph contract violations still throw. The post-release test now proves a live `NodeRef` cannot read graph-owned state after membership ends. **Eight S4 suites pass; included in the H-08/X-05 P −58.** |
| **X-06; Claude A hidden state; Claude B Q-2 — dynamic QObject properties** | **Verified with scope and repaired.** Functional state is typed; obsolete `turnContainer`, passive interaction, and image probe mirrors are deleted. Surviving properties are QSS selectors or non-authoritative quantitative observation channels, not functional authorities. | I-2/I-5. Preserve typed owners and do not read instrumentation back into behavior. **Implemented across S1/S4/S5/M-12.** |
| **X-07; Claude B R-4 — invisible selected row** | **Original absence and later M-44 conflation are repaired.** Mouse selection remains semantic/accessibility state without a false keyboard-focus underline; genuine keyboard focus has one pushed card flag and visible decoration. | I-5/I-9. Breeze/Fusion and four-DPR geometry/pixel qualification pass. **Implemented in MR-7/M-30.** |
| **X-08; Claude A helper inventory; Claude B Q-6 — six `makeLabel` copies** | **Verified and fully repaired.** Composer's surviving private factory omitted the shared selectable-text behavior; that defect is now removed. | I-8. Every equivalent caller uses `UiStyle::makeLabel`; no generic policy flag or extra factory remains. | Production reduced; all consumers compile warning-clean. **Complete.** |
| **X-09; Claude A/B Q-7 — Copy/disclosure/chevron/combo multiplication** | **Copy and disclosure are repaired.** One `presentation::CopyButton` and one `presentation::DisclosureButton` serve conversation and Inspector surfaces; the latter owns the one Button-role accessible contract. Other chevrons/combos have distinct controllers and only share `UiStyle` drawing/QSS, so further control consolidation is disproven. | I-1/I-8/I-9. Preserve the shared semantic controls and shared visual primitive. **Implemented.** |
| **X-10; Claude A — repeated text/UTF-8/layout helpers** | Repeated low-level spellings are **verified but not a shared-policy defect**; their ownership and lifetime semantics differ, and a generic utility layer would add abstraction without deleting an authority. Shared label/control/Markdown policy was consolidated separately. | I-6/I-8. No production change accepted for merely syntactic duplication. **Disproven as remaining architecture work.** |
| **X-11; Claude B T-1 — no test framework/per-case registry** | Ad-hoc mains and no external framework are **verified**, but absence of a framework alone is not a product defect. Every multi-case main now accumulates rather than short-circuits and reports failures through the existing smallest mechanism. | I-10. Preserve that execution property; adopt Qt Test only if it deletes infrastructure/LOC. **Implemented without adding a framework.** |
| **X-12; Claude B B-2 — test transport feature drift** | **Verified and repaired** as M-31: one reusable-target helper propagates TLS/RFCOMM/WebSocket definitions and links to production and runtime-dispatch consumers. | I-10. Compile-only all-target feature context is clean. **Implemented with net-negative CMake.** |
| **X-13; Claude B B-3 — no warning/sanitizer configuration** | **Warning/sanitizer configuration repaired; CI execution breadth remains open.** Production and all relevant consumers compile under `-Wall -Wextra -Werror` (or `/W4 /WX`); local GCC ASan/UBSan and Clang Release compile cleanly. | I-10. S8 may add CI runtime/platform lanes when test execution is authorized. |
| **X-14; Claude B T-7 — timeouts/settlement** | CTest timeouts are valid safety bounds, so “timeouts are flakiness management” is **partly disproven**. Poll loops and exact timer-boundary assertions are **verified source risks**; actual flakiness is unresolved. | I-5/I-10. Replace timing guesses with event/state completion in affected tests; keep outer process timeouts. Test LOC neutral/negative. S8. |
| **X-15; Claude B Q-1 — callbacks instead of Qt signals** | General claim is **overstated/disproven as a blanket defect**. Typed `std::function` seams are intentional and most QObject callbacks are context-safe. M-05 and H-13 are concrete defects. | I-7. No wholesale signal conversion. Correct only single-owner/lifetime violations under M-05/H-13. P/T 0 beyond those rows. |

## Complete external-review crosswalk

This register ensures no external finding disappears merely because the local
audit used a different grouping.

### Claude review A

| Original heading/finding | Ledger disposition |
|---|---|
| Core defect is a triple derivation | Verified: C-01/H-01/M-15. “Triple” means two renderers plus separately derived hit/geometry, not three full renderers. |
| Materialization on mouse-down | Source mechanism verified: C-01/H-01. Runtime movement attributed only to local diagnostics/user report. |
| Complexity accumulated around boundary/history | Verified and superseded by deletion: C-01/H-16/M-15/M-22/M-23. |
| Short-circuiting virtualization main | Verified H-15; current complete count is seven default binaries, not the later Claude B count of eight. |
| Height-only guard and DPR-invalid pixel helper | Verified H-15/X-07. |
| Benchmark built but unregistered/unquantified | Verified M-22. |
| Single offscreen Debug CI | Verified H-15/M-23. |
| TS-only “parity” tests | Verified M-24. |
| Copy/disclosure/chevron/combo duplication | Verified X-09/H-10. |
| Six labels and repeated text/layout helpers | Verified X-08/X-10. |
| Status formatting drift | Verified H-03/M-24. |
| Dynamic QObject functional/test state | Verified X-06. |
| Accessibility and invisible selection | Verified H-01/M-21/M-30/X-07. |
| Two-renderer `CardPresentation` remedy | **Superseded:** it preserves two complete Qt implementations. Metrics/value helpers may remain only where the sole `ConversationCard` consumes them. |

### Claude review B

| ID | Ledger disposition |
|---|---|
| A-1 | X-01 verified; proposed class extraction superseded pending real deletion. |
| A-2 | X-02 verified; renderer deletion precedes reassessment. |
| A-3 | X-03/M-13 verified. |
| A-4 | H-03/M-03 verified. |
| A-5 | X-04 partly verified/partly disproven; identity variants differ semantically. |
| A-6 | X-05 verified narrowly. |
| A-7 | H-08a verified residue; side-table remedy superseded by deletion. Core retirement retained as H-08b. |
| A-8 | M-24 verified; separate renderers remain valid. |
| R-1/R-2/R-5 | C-01/H-01/M-15 verified; two-renderer remedy superseded. |
| R-3 | **Disproven as a third card renderer.** Turn surface is legitimate view-owned cross-row decoration; raw color aspect remains X-03. |
| R-4 | X-07 verified. |
| Q-1 | X-15 blanket claim rejected; concrete M-05/H-13 retained. |
| Q-2/Q-3/Q-4/Q-5 | X-06/C-01/H-01 verified; document destructor workaround is deleted with bridge. |
| Q-6/Q-7 | X-08/X-09 verified. |
| B-1/B-2/B-3/B-4 | M-31/X-12/X-13/H-15 verified. |
| T-1 | X-11 verified structure, framework prescription unresolved. |
| T-2 | H-15 verified the underlying defect; exact “eight” count was disproven—seven default mains short-circuited. All are repaired. |
| T-3/T-4/T-5/T-6 | M-24/H-15/X-07/M-22 verified. |
| T-7 | X-14 partly verified/partly disproven. |

Claude B’s positive findings are retained constraints: `NodeGraph` single-writer
authority, adapter lock/UI separation, `ProtocolCatalog`, the value boundary in
`MiddleTypes`, `ConversationItemModel`/`ConversationHeightIndex`, nested-modal
lifetime tokens, and generally sound QObject context ownership. They are
confirmed informational findings, not deletion targets.

### ChatGPT review

| ID | Ledger disposition |
|---|---|
| F-QT-001 | C-01 duplicate/verified. |
| F-QT-002 | C-01/H-01/M-15/M-17 duplicate/verified. |
| F-QT-003 | C-01/M-15 duplicate/verified. |
| F-QT-004 | M-15 duplicate/verified. |
| F-QT-005 | C-01/H-16/M-13/M-15/M-30 aggregate consequence, verified in parts. |
| F-QT-006 | C-01/H-01/M-17 duplicate/verified. |
| F-QT-007 | M-17 duplicate/verified. |
| F-QT-008 | C-01 document bridge verified; superseded by deletion. |
| F-QT-009 | H-10/X-09/M-16 verified. |
| F-QT-010 | H-10/M-08/X-09 verified; specialized controllers remain valid. |
| F-QT-011 | H-01/M-21 duplicate/verified. |
| F-QT-012 | H-15/M-21/M-30 duplicate/verified. |
| F-QT-013 | C-01/H-16/M-22 history evidence. |
| F-QT-014 | M-22 verified. Its later “20 passing CTests including benchmark” statement is disproven. |
| F-QT-015 | C-01/H-02/M-13/M-22 verified; bounded size alone did not solve invalidation/lifecycle. |
| F-QT-016 | C-01/H-16 duplicate/verified. |
| F-QT-017 | H-15/M-23 duplicate/verified. |
| POS-001 | Confirmed: `NodeGraph` remains authoritative. |
| POS-002 | Confirmed: adapter snapshots avoid graph locks during QWidget work. |
| POS-003 | Confirmed: ItemModel/HeightIndex remain the virtualization foundation. |
| POS-004 | Confirmed: local-prompt identity/promotion is a valid lifecycle distinction. |
| POS-005 | Confirmed: native/browser renderers are legitimately separate; semantic drift is not. |

ChatGPT’s historical headings are evidence rather than independent defects:
`bd2297e` created C-01; `aab2723` added the document bridge; `6f81d99`
accreted M-15 compensation; `abf0cb1` and `b2de44a` are work-control history;
`6c153ec` concerns resize coalescing; `2fa5481` accumulated workflow state; and
`629660e` plus the dirty extension are classified under H-16. Its proposal to
pool widgets, switch one card class at a time, or add production observability
first is **superseded/unresolved**: no pool is justified initially, the passive
renderer must be deleted in the same stage, and new production instrumentation
requires the normal addition gate.

## Contradiction decisions

1. **One renderer wins over a shared projection feeding two renderers.** The
   latter can share values but cannot share Qt layout, pixels, hit testing, or
   accessibility. The existing `ConversationCard` is authoritative for all
   visible and overscan rows.
2. **Lazy materialization remains.** `materializationRows()` retains viewport
   plus one viewport of overscan on each side; offscreen rows remain model data
   plus cached scalar heights. Removing the passive renderer is not removing
   virtualization.
3. **No initial widget pool.** Existing bounded residency is simpler. Pooling
   is an unmeasured later hypothesis and would require evidence and approval if
   it adds state/reuse machinery.
4. **Baseline and current benchmark facts are separated.** Baseline source and
   `ctest -N` proved 19 CTests and an unregistered executable; ChatGPT
   conflated source files with registered tests. The current worktree adds 12
   quantitatively gated row/DPR profiles. All twelve pass in the final Debug,
   Release, and ASan/UBSan S1 matrices.
5. **Source risks are not runtime reproductions.** External reviews ran no
   diagnostics. Only the local audit supplies runtime observations, with
   XCB/Wayland activation and clipboard failures still labelled
   product-versus-harness unresolved.
6. **Committed and dirty behavior differ.** External line evidence describes
   HEAD. The local audit and H-16 own the dirty offsets/colors/focus/parity
   classification.
7. **Class movement is not simplification.** Shell/View extraction proposals
   are accepted only when they delete duplicate ownership/state/transitions and
   reduce production LOC.
8. **Turn decoration is not a card renderer.** The continuous cross-row Turn
   surface stays view-owned. Its duplicate structural damage planner is
   deleted; raw reusable styling remains classified under X-03/M-13 for S7.
9. **Qt attachment residue is not core retirement.** The former is deleted;
   retired-node pinning/acknowledgement remains until queue-order proof permits
   reduction.
10. **Test-framework absence is not license to add infrastructure.** First
    make all existing cases run and report; adopt a framework only if it
    simplifies the suite.

## Unresolved evidence questions and approval gates

- Real user content mix, construction percentiles, widget-reuse break-even,
  screen-reader behavior, first-click behavior on native platforms, and
  Qt-version differences still require post-S1 measurement.
- H-09 is complete under its separately approved and fully accounted +1,600
  ceiling. H-11 is also complete: the user separately approved a +1,600 native
  production-CLOC ceiling, and the coherent one-tree/one-delegate/model-backed-
  accessibility replacement finished at +214 production, +954 test, and +40
  configuration CLOC. M-37 and the complete H-11 verification matrices pass;
  neither stage has a remaining approval gate.
- H-14, M-13, M-18, and M-21 may require a small new policy,
  asynchronous path, semantic control, style epoch, or accessibility event
  mechanism. Reduction/change is attempted first; any unavoidable production
  growth is quantified and approval-requested before implementation.
- The user approved the bounded S1-D03/D04/D05 implementation allowance of
  approximately +205..+345 production LOC after the renderer-deletion
  checkpoint. The final coherent implementation added +260 `cloc` while
  replacing remaining state/environment/accessibility/input authorities; it
  did not add a renderer, cache layer, or accessibility shadow model.
- The user separately approved S3's quantified request-policy correctness
  growth. The exact +1,606 combined production CLOC result is recorded above;
  that approval cannot be reused by later stages.
- Full standard-MCP form schema/content validation remains unapproved and is
  estimated at approximately +170..+260 production CLOC. Current policy
  validates bounded transport shape and fails closed where context is unsafe;
  it does not claim JSON-Schema semantic validation.
- Exact file-change patch context plus stale-revision gating remains
  unapproved and is estimated at approximately +90..+170 production CLOC.
  Current behavior does not claim that stronger pre-apply safety contract.
- H-08b remains intact unless adversarial queued-read tests establish a smaller
  safe retirement protocol.
- Production parse counters are not currently observable. The S1 benchmark
  first uses test-side document/card/block identity and existing counters; a
  new production counter is not implicitly authorized.

## Stage accounting rule

Each stage records its own pre/post production and test `cloc`, exact commands,
correctness/performance/accessibility results, and diff audit. Estimates above
overlap and must never be summed as a promise. A stage is not complete while a
superseded renderer, compatibility path, workaround, unbounded duplicate owner,
or required verification remains.
