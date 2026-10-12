<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../../media/nav/banner-findings-ledger-compact.svg"><img src="../../../media/nav/banner-findings-ledger.svg" alt="CodexUI — Findings ledger" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Findings ledger"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/up-findings-ledger-dark.svg"><img src="../../../media/nav/up-findings-ledger-light.svg" alt="Up to Findings ledger" title="Up to Findings ledger" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/organization-dark.svg"><img align="right" src="../../../media/nav/organization-light.svg" alt="SNodeC organization" title="SNodeC organization"></picture></a><a href="../../../../README.md#project-overview" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/repository-dark.svg"><img align="right" src="../../../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Historical remediation and S1–S4 checkpoints

## Historical remediation record

Date: 2026-09-12
Repository: `/path/to/workspace/drafts/CodexUI/codexui`
Branch/HEAD: `master` at `629660e8882bddc7473ee5f1aab90c755ee6edd4`
Upstream: `origin/master`, ahead/behind `0/0`

This is the authoritative ledger for the Qt architecture remediation. It
reconciles the current-worktree audit with both Claude reviews and the ChatGPT
review before production edits. The local audit is strongest because it
inspected and executed the dirty worktree; the external reviews inspected the
committed HEAD only.

## Evidence and ownership baseline

The pre-existing user-owned worktree contains no staged changes, conflicts,
renames, or mode changes. It contains these tracked modifications:

| File | Existing delta | Provenance/purpose |
|---|---:|---|
| `ConversationCards.cpp` | +20/-4 | Mouse-versus-keyboard Copy focus presentation and half-pixel check alignment. |
| `ConversationView.cpp` | +109/-55 | Passive/live geometry and color parity compensation; retains both renderers. |
| `ConversationView.h` | +3/-0 | State supporting the passive/live compensation. |
| `ConversationCardsTest.cpp` | +33/-2 | Copy bounds and mouse/keyboard focus checks. |
| `ConversationVirtualizationTest.cpp` | +245/-10 | Passive/live pixel-parity matrix and related geometry checks. |
| `web/src/app/App.tsx` | +1/-1 | Half-pixel check-path alignment. |

The native production delta is +132/-59 (net +73); native tests are +278/-12
(net +266). These changes are preserved as user work. Their semantic intent is
retained where valid, while passive-renderer compensation and parity tests are
superseded by the one-renderer repair. The six untracked instruction/audit
artifacts are also user-owned and remain untouched. Exact authorship of
untracked files cannot be established from Git.

Current `cloc` baselines (including native CMake code) are:

| Scope | Code LOC |
|---|---:|
| Native production, `src/codex` | 34,254 |
| Native tests, `tests/codex` | 27,707 |
| Browser production, `web/src` | 4,938 |
| Browser tests, `web/tests` | 2,422 |
| Six conversation renderer files | 7,126 |

No production or test file was edited while this ledger was prepared.

## S1 renderer-replacement checkpoint (historical)

The ledger was created before editing. This checkpoint records the first
coherent renderer replacement before the subsequently approved correctness,
runtime-environment, and accessibility work. The final S1 accounting appears
below and supersedes these values without erasing their provenance.

| Scope | Baseline code LOC | Current code LOC | Delta |
|---|---:|---:|---:|
| Native production, `src/codex` | 34,254 | 33,487 | −767 |
| Native tests, `tests/codex` | 27,707 | 28,497 | +790 |
| Six conversation renderer files | 7,126 | 6,340 | −786 |

Against `HEAD`, native production is physically +1,059/−1,797 (net −738)
and native tests are +2,490/−1,394 (net +1,096). Subtracting the recorded
pre-existing user patch, the S1 work itself is approximately net −811
physical production lines and +830 physical test lines. The test increase is
primarily the quantitative benchmark and invariant coverage; it is not hidden
as production reduction.

Implemented and focused-verified at this checkpoint:

- the passive renderer, passive Markdown/document LRU, document adoption and
  transfer, passive Copy/disclosure painting and hit testing, synthetic mouse
  forwarding, and renderer-parity-only machinery are deleted;
- viewport and overscan rows use the sole real `ConversationCard`; offscreen
  rows remain values plus stable semantic state and cached scalar height;
- materialization remains bounded and lazy, with no widget pool or third
  renderer;
- zero-height hidden runs use the existing height tree's logarithmic
  next/previous nonzero-row traversal instead of linear row scans;
- collapsed header phase changes measure their actual before/after geometry;
- twelve quantitative performance CTests are registered for 320/1,280/10,000
  rows at DPR 1.0/1.25/1.5/2.0; and
- the latest focused card, item-model, and virtualization run passes 3/3.

The test-LOC forecast for C-01 was disproven by the required performance and
invariant qualification: deleting parity-only coverage did not offset the
1,095-line benchmark expansion and focused regression matrix. Production
reduction remained within the requested approximate −750..−1,050
renderer-replacement design target at this checkpoint. The later approved
single-authority correctness work is accounted separately in the final S1
record below.

### S1 discoveries after consolidation

| ID | Evidence and classification | Architectural disposition and verification | Status/LOC effect |
|---|---|---|---|
| **S1-D01 — sparse hidden-row traversal** | Materialization, release, Turn painting, visual selection, anchor/history navigation, and cursor navigation walked every zero-height row between visible endpoints. **Locally verified source/performance defect.** | I-3/I-10. Extend the existing height index, not a second index, with logarithmic next/previous nonzero traversal; assert lookup-step and 20-vs-20,000-hidden-row work bounds. | **Implemented; focused and quantitative case pass.** P +21 gross/−0 with linear loops reshaped; T + focused assertions. |
| **S1-D02 — collapsed phase geometry** | A collapsed agent message changing update→final changed margins while the collapsed fast path returned paint-only, leaving indexed geometry stale. **Locally reproduced.** | I-1/I-5. Compare the sole card's natural collapsed height before/after rather than add offsets, timers, or reconciliation state. | **Implemented; card identity and independently measured geometry test pass.** Net-neutral reshape. |
| **S1-D03 — retained-state transition drift** | Resident card updates clamped/removed stale selections and command state through the widget, while an offscreen retained state could survive owner removal, truncation, or output disappearance and later resurrect. Outer command pause could remain after output disappeared. **Defect reproduced and fixed.** | I-2/I-4/I-5. `ConversationCard::State` records semantic role, a bounded UTF-16 length/SHA-256 source fingerprint, directional cursors, and command owner state. One `normalizeState` rule is applied to resident, staged, and offscreen retained state; disappearance, truncation, incompatible ownership, and output removal retire state and release the outer pause. Normalization is folded into the existing retained-key pass rather than scanning every model row. | **Implemented.** Focused tests cover resident/offscreen output removal, staged inactive truncation/regrowth, append-only selection preservation, folded release/rematerialization/expansion, independent detached outputs, owner replacement, thread retirement, and no resurrection. Included in the final combined post-checkpoint accounting below. |
| **S1-D04 — runtime geometry environment** | Scalar and renderer-local height caches were keyed by content/width but not effective font, style, or DPR. `FontChange`, `StyleChange`, and Qt 6.6 `DevicePixelRatioChange` could therefore reuse stale geometry. **Defect reproduced and fixed.** | I-1/I-3/I-5/I-9. Existing card/height/reflow authorities now invalidate on the view's Qt environment events. One queued post-style phase is used because descendants receive repolish after the view; it remeasures the same resident/staged objects, clears retained scalar heights, and preserves anchors/follow mode. No epoch cache, application filter, screen connection, or reconstruction was added. | **Implemented.** Focused tests cover resident and staged cards, inactive-thread scalar retirement, font/style/DPR events, identity/document/focus/selection preservation, anchor preservation, and following-tail restoration. Included in the approved combined checkpoint delta. |
| **S1-D05 — default item-view accessibility tree** | Qt's default `QAbstractItemView` exposed virtual Table/Cell objects that did not contain the real card controls; adding leaf interfaces alone would leave competing accessibility authorities. **Verified against local Qt 6.10 behavior and fixed.** | I-1/I-4/I-9. One process-lifetime factory now projects the existing physical hierarchy as Pane→List→resident ordered ListItem/control objects and delegates selection to the existing selection model. Nonresident model cells and the hidden staging tree are absent. | **Implemented.** The accessibility fixture checks roles, names, parent/child symmetry, focus, selection, offscreen state, history/empty/loading controls, no virtual table/cells, no staging objects, and no-op object/tree identity. Real AT-SPI remains an explicit S8 platform qualification limitation. Included in the approved combined checkpoint delta. |
| **S1-D06 — active documentation described deleted design** | `qt-virtualized-conversation-view.md` described delegate cards, passive documents, promotion, synthetic forwarding, and stale benchmark/ASan claims. `ui-ux-internal-api.md` named removed command-state APIs and overstated focus retention. **Verified.** | I-6/I-10. Replace the old architecture description with the sole-renderer contract and distinguish deterministic offscreen evidence from unrun native-platform/AT-SPI qualification. | **Implemented; final source/test/documentation term cross-check is clean.** |
| **S1-D07 — duplicate structural damage authority** | `finishExactStructureChange` replayed row/section/anchor geometry in a 110-CLOC damage planner, but viewport paint-region probes showed Qt already coalesced each structural model batch into one viewport-bounded paint; its “offscreen paint avoided” counters did not observe actual paint behavior. **Locally measured duplicate mechanism.** | I-1/I-5/I-6. Delete the planner and its two test-channel counters. Let the precise model/index/anchor transaction finish, then use the item view's existing viewport invalidation authority. | **Implemented; −110 production CLOC.** A 10,000-row offscreen exact-operation batch and a visible exact removal assert model/index/section/anchor results, one-or-two coalesced viewport-bounded paints, and the required changed pixels. |
| **S1-D08 — fixed-pass residency settlement** | A fixed number of layout/materialization passes was neither a proof of convergence nor an honest work metric. **Verified source/performance gap.** | I-3/I-5/I-10. Keep one residency loop that stops on a stable materialized set; count actual scans and gate normal updates at `<= 4`, seeks at `<= 8`, and both at no more than constructions plus one. | **Implemented.** All 12 row/DPR profiles satisfy the quantitative scan bounds; no second scheduler or residency authority was added. |
| **S1-D09 — retained-state size and multi-command pause** | Retaining complete selection sources made local state content-sized, and a single detached-command transition could resume the outer view while another command remained detached. **Verified source defects.** | I-2/I-3/I-4. Replace retained source copies with fixed-size fingerprints and derive whether any command remains detached from the one resident/retained state authority before releasing the compositional pause cause. | **Implemented.** Churn tests gate bounded fingerprints and two independently detached commands through follow and removal transitions. |
| **S1-D10 — split wheel continuation authority** | Card text/output handlers duplicated edge decisions; composer and attachment gestures were not recorded, and outer forwarding dropped source/device/timestamp metadata. **Verified and locally reproduced by adversarial event sequences.** The first centralization was subsequently disproven by a real-card audit: it recorded an owner but swallowed crossed-owner and zero/End delivery, while its orphan-widget test could pass without owner movement. | I-2/I-4/I-6/I-9. Delete card-local edge arbitration and the separate `ConversationView` forwarding/guard path. `MiddleRegionWidget` owns one phased gesture across real nested cards, composer surfaces, outer chrome, side panes, and pointer crossing; one scoped redispatch boundary preserves native Qt pixel/angle handling and exact metadata. | **Complete in MR-6.** P -22; T +260. Release 26/26 correctness and 29/29 registered quantitative tests pass; routed p95/max is 5/8 us in the final focused run. Native input-to-painted-frame breadth remains MR-8. |
| **S1-D11 — animation and Markdown-work observability** | Pending animation advanced by refreshing the whole card presentation every 32 ms, and streaming tests did not prove that an immutable long Markdown prefix remained untouched. **Verified source/performance gaps.** | I-3/I-5/I-10. Make the existing animation timer repaint-only. Observe `QTextDocument` changes and prefix-block revision from the test side rather than add a production parse counter or cache. | **Implemented.** The timer no longer invokes content/geometry projection; every profile reports 24/24 mutable-tail updates, zero streaming card reconstruction, and bounded document/layout/paint work. |
| **S1-D12 — focus pin survived focus departure** | Focus-pinned cards outside overscan were hidden but remained resident after focus moved, so later offscreen changes could still reach a QWidget/document. A dormant test asserted that obsolete behavior. **Verified source defect.** | I-3/I-4/I-9/I-10. Derive the departing pin from the existing visible resident set and reuse the one residency-window release decision. The former event receiver leaves the resident map and viewport/accessibility tree synchronously, then alone uses Qt deferred destruction; every ordinary release stays immediate so physical widget/document counts remain bounded. No pin flag or second scheduler exists. | **Implemented.** The obsolete dormant test is deleted; the executed virtualization fixture proves the focused object remains pinned during interaction, survives the active key dispatch outside the active tree, then is destroyed after Tab transfers focus. Focused stress and full matrix results are recorded below. |

## Final S1 accounting

The accounting command is `git ls-files <scope> | cloc --list-file=-`; native
production includes `src/codex/nodegraph/CMakeLists.txt`. The six-file scope is
`ConversationCards`, `ConversationPresentation`, and `ConversationView`
headers and implementations.

| Scope | Original baseline | Renderer checkpoint | Final S1 | Final vs baseline | Final vs checkpoint |
|---|---:|---:|---:|---:|---:|
| Native production | 34,254 | 33,487 | 33,747 | −507 | +260 |
| Native tests | 27,707 | 28,497 | 29,878 | +2,171 | +1,381 |
| Six renderer files | 7,126 | 6,340 | 6,564 | −562 | +224 |

The coherent renderer replacement itself met the requested reduction target at
−767 production CLOC. The subsequently approved +205..+345 allowance covered
correctness, runtime-environment, accessibility, input-continuity, and
quantitative-gate work that could not be absorbed by deleting the renderer
fork; final post-checkpoint growth is +260. The stage remains a net production
reduction of 507 CLOC. Against `HEAD`, the final native production physical
diff is +2,198/−2,649 (net −451); tests are +5,412/−2,856 (net +2,556).
The benchmark alone is +1,281/−73 physical lines and accounts for most of the
test growth.

## Final S1 verification

- `cmake --build ../build/Desktop_GCC-Debug -j2` rebuilt the exact-current
  virtualization test; `ctest --test-dir ../build/Desktop_GCC-Debug
  --output-on-failure -j2` passed 31/31 tests in 96.62 s.
- `cmake --build ../build/Qt-Remediation-Release -j2` reports no pending work;
  the exact-current Release matrix passed 31/31 tests in 73.47 s.
- `cmake --build ../build/Sanitizers-ASan -j2` rebuilt all affected targets;
  `ctest --test-dir ../build/Sanitizers-ASan --output-on-failure -j2` passed
  31/31 tests in 211.64 s with no ASan/UBSan diagnostic.
- Each matrix executed all twelve registered conversation performance profiles:
  320, 1,280, and 10,000 rows at DPR 1.0, 1.25, 1.5, and 2.0. Their widget,
  document, construction, release, residency-scan, measurement, layout,
  paint, and streaming-work limits all passed.
- Focused Debug, Release, and sanitizer repetitions covered the departing-focus
  event receiver, logical residency removal, deferred destruction, keyboard
  transfer, selection/current state, and view isolation. The final adversarial
  review found no reachable lifetime, cross-view, accessibility, or physical
  residency-count blocker.
- `git diff --check` is clean. Production/test source scans find no passive card
  class, passive Markdown cache, document-adoption bridge, synthetic mouse
  forwarding, or renderer-parity implementation. The remaining
  `applyCardPresentation` name denotes the sole renderer's model-to-card update
  entry point, not a second presentation implementation.
- Final `cloc` recounts are 33,747 native production, 29,878 native test, and
  6,564 across the six renderer files, matching the accounting above. The
  pre-existing `web/src/app/App.tsx` change remains isolated at +1/−1 and was
  not modified during S1.

Limitations are explicit: the deterministic offscreen plugin exercised all
requested DPR values, but native XCB/Wayland input behavior and a real AT-SPI
consumer remain S8 platform qualification work. No S1 correctness or
performance hypothesis remains open, and no temporary renderer compatibility
path or workaround remains.

## S2 stage gate: typed semantic presentation contract

**Status: complete.**

**Invariant being restored.** After protocol ingestion, `NodeState::status` is
the native lifecycle authority and `RelationKind::ActiveTurn` is the provider
turn-identity authority. Adapters project status and plan outcome once into
typed presentation values; widgets and browser render functions do not regain
semantic authority by recognizing raw aliases. Separate native and browser
renderers consume the same status corpus, while remaining separate renderers.

**Reduction before change.** This stage first deletes the duplicate adapter and
Shell raw-status switches, renderer-time alias checks, the unreachable
`TurnPlanKey` variant/encoding/adapter branch, duplicate Inspector plan DTOs,
the React-only plan-outcome helper, and false `parity` suite names. Existing
`UiStatus`, `PlanData`, `ActiveTurn`, and frontend test runners absorb the
remaining behavior. No new production layer, CLI, cache, state flag, timer, or
compatibility path is authorized or planned.

Contradictions are resolved as follows:

- Raw protocol status fields remain retained facts, but cannot override a
  known typed `NodeStatus`. An unknown typed status may retain its raw label for
  forward-compatible display without acquiring active/terminal semantics.
- `ActiveTurn` alone identifies an authoritative running turn. The one
  deliberate exception is adapter-owned presentation of a typed `Pending`,
  local provisional turn before acknowledgement. Relating it early is rejected
  because `WorkerLogic::takeNextPrompt` would choose Steer instead of Start.
- The external blanket identity-deletion proposal is disproven. Stable visual
  `CardKey`, semantic thread/turn/item coordinates, prompt-promotion identity,
  and native action target have distinct lifetimes. Only `TurnPlanKey` is
  unreachable because every production `graphCardData` call receives an Item.
- Existing native and browser tests passing independently are not parity.
  Honest behavior/contract suite names are retained, and a data-only status
  corpus is consumed by both existing runners. A production comparison
  executable is rejected.

S2 starts from 38,685 combined native/web production CLOC (33,747 native,
4,938 web) and 32,300 combined test CLOC (29,878 native, 2,422 web). The exact
pre-edit baselines are Debug/Release/ASan 31/31 from final S1 and `npm test`
91/91 in 1.37 s. Expected stage effect is production −80..−180 and test/data
+100..+250; overlapping ledger estimates are not summed. If the coherent
replacement cannot remain net-reductive in production, implementation stops
for a new approval rather than introducing another status or identity owner.

### S2 discoveries and dispositions

| ID | Current-worktree evidence and classification | Architectural disposition and verification | Status/LOC effect |
|---|---|---|---|
| **S2-D01 — lifecycle fallback precedence** | Native and browser lifecycle handlers defaulted started/completed events by overwriting explicit, forward-compatible provider statuses. **Locally reproduced semantic-authority defect.** | I-2/I-5/I-10. Lifecycle defaults now apply only when the incoming status is absent or empty. The typed status and preserved raw value remain converged; the shared corpus exercises turn and item start/completion with empty and future statuses in both frontends. | **Implemented and cross-frontend verified.** Included in the combined S2 reduction. |
| **S2-D02 — active-turn event ordering** | Browser turn activity was inferred from final status values and rewrote thread status. `thread/completed` followed by `turn/started` and the reverse order have identical final raw statuses but different native `ActiveTurn` relations. **Locally reproduced semantic drift.** | I-2/I-5/I-10. Native retains `RelationKind::ActiveTurn`; browser retains one typed `activeTurnId` lifecycle fact. Event handlers update or clear that fact in protocol order. Status-gated scans and turn-to-thread status rewriting are deleted. | **Implemented and shared-corpus verified.** This necessary typed field is absorbed by larger production deletion; no parallel lifecycle owner remains. |
| **S2-D03 — plan replacement semantics** | Omitted explanation retained stale native text, malformed browser plan payloads could retain old steps, and an explicitly empty structured native plan disappeared while browser retained an empty plan. **Locally reproduced cross-frontend divergence.** | I-2/I-5/I-10. A plan update replaces the complete structured plan: omitted explanation clears, missing/malformed steps become an empty array, and explicit empty plans remain present. One effective-plan-status projection is consumed by conversation and Inspector. | **Implemented and shared-corpus verified.** Duplicate Inspector/React plan policy deleted. |
| **S2-D04 — child-agent terminal propagation** | Closing a browser child thread left its owning agent activity running, while kind-based fallback could overwrite an explicit unknown future status. **Locally reproduced drift.** | I-2/I-5/I-10. Child closure deterministically projects `notLoaded` to the owning agent; kind fallback applies only to a genuinely empty status, never an explicit unknown value. | **Implemented and shared-corpus verified.** |
| **S2-D05 — copied DTO and false-parity residue** | `TurnPlanKey`, payload-level status copies, `ItemPresentation.status`, duplicate plan DTOs, dead adapter parameters, a second status-object parser, direct stale browser agent upserts, and six TS-only `*-parity` filenames duplicated or overstated authority. **Verified.** | I-2/I-6/I-10. Delete unreachable/copied fields and branches, centralize status-object decoding, route agent activity through the canonical model, rename frontend-local suites honestly, and execute one data-only semantic corpus in both existing runners. Stable card, protocol, and target identities with distinct lifetimes remain. | **Implemented.** No production comparison executable or shared renderer was added. |

## Final S2 accounting

The accounting scope and command remain the S1 rule: tracked and untracked
files under each source/test root are passed to `cloc --list-file=-`; native
production and native tests include their in-scope CMake code. The shared JSON
fixture is reported separately rather than hidden in a source-code total.

| Scope | S2 baseline | Final S2 | Delta |
|---|---:|---:|---:|
| Native production, `src/codex` | 33,747 | 33,561 | −186 |
| Browser production, `web/src` | 4,938 | 4,982 | +44 |
| **Combined production** | **38,685** | **38,543** | **−142** |
| Native tests, `tests/codex` | 29,878 | 30,484 | +606 |
| Browser tests, `web/tests` | 2,422 | 2,607 | +185 |
| **Combined test code** | **32,300** | **33,091** | **+791** |
| Shared presentation fixture, `tests/fixtures/frontend-presentation.json` | 0 | 141 | +141 |
| **Combined test/data effect** | **32,300** | **33,232** | **+932** |

Production met the forecasted −80..−180 range and the mandatory net-reduction
gate. Browser production grew by 44 CLOC only inside the coherent replacement
of inferred/re-written lifecycle state with one typed, event-ordered active-turn
fact; native deletion more than absorbed it. The test/data forecast was
disproven: a genuine two-runner status/plan/lifecycle corpus plus native graph,
adapter, runtime, and widget-boundary cases required +932 CLOC. That growth is
reported explicitly; it neither weakens tests nor hides production code in a
test target.

## Final S2 verification

- `cmake --build ../build/Desktop_GCC-Debug -j2` rebuilt the affected native
  targets without warnings. `ctest --test-dir ../build/Desktop_GCC-Debug
  --output-on-failure` executed and passed 31/31 tests in 113.17 s.
- The Debug matrix executed all twelve quantitative conversation profiles at
  320, 1,280, and 10,000 rows and DPR 1.0, 1.25, 1.5, and 2.0; all existing
  widget/document/construction/residency/measurement/layout/paint/streaming and
  RSS gates passed (80.40 s aggregate for the performance label).
- `cmake --build ../build/Qt-Remediation-Release --parallel` completed without
  warnings; the complete Release matrix passed 31/31 in 88.38 s.
- `cmake --build ../build/Sanitizers-ASan --parallel` completed without
  warnings; the complete ASan+UBSan matrix passed 31/31 in 233.55 s with no
  sanitizer diagnostic. The focused `NodeGraph-ASan-Current` matrix passed
  5/5 in 3.82 s.
- `cd web && npm test` rebuilt TypeScript and passed 150/150 tests in 1.39 s.
  `npm run profile` measured 10,000 authoritative/visible items and 2,000
  streamed deltas at 45.56 ms hydrate, 35.31 ms project, and 9.73 ms stream.
  Against the 45.8/34.6/9.57 ms baseline these are −0.5%, +2.1%, and +1.7%,
  respectively: no material regression.
- Independent adversarial review found no remaining S2 production blocker and
  confirmed that both existing runners execute the shared corpus. `git diff
  --check` is clean; active source/test scans find no `TurnPlanKey`, copied
  item/payload status field, direct browser agent-activity upsert, or false
  `*-parity` test filename.

The deterministic offscreen Qt platform covers the requested DPR values.
Native XCB/Wayland and real assistive-technology qualification remain the
explicit S8 platform limitations; they are not unverified S2 semantics.

## S3 stage gate: graph-to-UI projection and operation ownership

**Status: complete.** H-05 routes one adapter-owned conversation delta through
one view entry point, M-05 has one application-lifetime owner, H-04 history
single-flight is graph-operation-owned, and H-06 ends Qt lifecycle ownership
at worker admission while retaining one thread-list item across
local-to-canonical identity. H-07 now owns native request classification,
bounded review context, actions, controls, response construction, recovery,
and Inspector projection in one typed policy. The browser retains its separate
runtime implementation but executes the same semantic fixture corpus. M-01,
the remaining general focus work from M-04, and settings policy M-20 are
explicit carry-forwards to S6, S6, and S5 respectively; they are not hidden S3
completion claims.

**Invariant being restored.** Once a thread, turn, prompt, request, or
operation is admitted into `NodeGraph`, the graph is its sole fact and
lifecycle authority. `NodeGraphUiAdapter` owns one graph-to-value projection
and canonical placement policy under a short read; `ShellWidget` may coalesce
and schedule delivery but must not rederive row ordering, prompt promotion,
request meaning, or operation state. Qt views own only interaction and
presentation state keyed by stable semantic identity.

**Deletion before change.** The first coherent replacement targets the public
`card`, `promptMaterialization`, `rowChange`, and `tailCard` projection fork;
`PromptMaterialization`/`ConversationTailCard` DTOs; Shell's prompt/tail special
cases, topological row DAG, adjacency queries, removal reconciliation, and
full-snapshot recovery counter; and duplicate prompt acknowledgement logic.
The replacement must preserve the eight-row display-pass bound and reuse the
same projection code for full snapshots and exact deltas. No renderer, cache,
poller, timer, or persistent projection state is permitted.

After that boundary is stable, S3 deletes `historyPageAwaitingProvider` in
favor of the existing pending `Operation` targeting the thread, and removes
post-worker-admission new-thread correlation/overlay/selection retry state in
favor of the local Thread/Turn/Prompt already created by `WorkerLogic`.
Pre-admission authored draft state remains explicitly Qt-owned. The competing
Shell runtime-stopped handler is also deleted so application lifetime has one
owner. Request/action policy is consolidated in the existing typed policy.
Settings policy remains in S5 because combining it with the request correction
would obscure two independent authorities; dependent Inspector/widget
residency remains ordered after the projection boundary.

S3 starts from 38,543 combined production CLOC (33,561 native, 4,982 browser),
33,091 combined test CLOC (30,484 native, 2,607 browser), and 141 lines of
shared fixture data. Relevant native files currently contain 11,431 CLOC:
`ShellWidget.cpp` 3,319, `NodeGraphUiAdapter.cpp` 2,007,
`ConversationView.cpp` 3,132, `ThreadPane.cpp` 996, and their/request/settings
support files 1,977. The pre-edit matrix is Debug/Release/ASan+UBSan 31/31,
NodeGraph sanitizer 5/5, and browser 150/150. Debug focused timings from the
complete run are adapter 0.01 s, conversation UI 0.13 s, thread pane 0.12 s,
client runtime 5.21 s, and Shell integration 6.19 s; all twelve conversation
performance gates total 80.40 s. The browser 10k/2k profile is
45.56/35.31/9.73 ms for hydrate/project/stream.

The whole S3 design target was native production −350..−800 CLOC and test/data
+100..+400 CLOC. That forecast was missed. After deletion-first projection,
operation, and optimistic-lifecycle consolidation, locally reproduced request
safety and native/browser semantic drift required a materially larger bounded
policy and differential corpus. The user explicitly approved continuing with
that quantified production growth. This approval does not cover the two
remaining additions recorded below: full standard-MCP form schema/content
validation or file-change patch-context/revision validation.

Final S3 accounting is 34,696 native production CLOC (+1,135), 5,453 browser
production CLOC (+471), and 40,149 combined production CLOC (+1,606). Native
tests are 34,778 CLOC (+4,294), browser tests are 2,611 CLOC (+4), and combined
tests are 37,389 CLOC (+4,298). The shared fixture is 606 lines (+465, reported
separately from test CLOC). The final exact-response/interaction/operation
transition reshape removed 18 combined production CLOC while adding 60 native
test CLOC. S3 is therefore accepted as an explicitly approved correctness
exception, not represented as a LOC reduction. S4 must resume net-reductive
responsibility remediation; this stage's growth is not a reusable allowance.

## S4 graph/UI lifetime boundary checkpoint

The first coherent S4 deletion separates obsolete toolkit attachment residue
from the required graph-retirement ordering protocol. Read-only Shell, view,
and graph-lifetime audits agreed before editing that production has no setter
of `Node::uiAttachment_`; the type and the 3,148-physical-line
`CODEXUI_DIRECT_GRAPH_WIDGET_TESTS` block describe the discarded direct-graph
widget implementation. Conversely, retired graph membership is required
because a synthesized rescan is delivered ahead of older queued graph
notifications.

The opaque `Node` back-pointer/API, `QtNodeAttachment`, CMake entry,
attachment-probe/retry branches, both unreachable Shell membership/exception
branches, disabled test archive, and its definition-only graph-fixture and
budget helpers are deleted. The unbounded `ReadAccess::retiredNodes()` vector
view is also deleted; the bounded count/index cursor is now the only retirement
read API. This H-08 slice added no production state, cache, callback, or
fallback. The already-existing durable live-saturation acknowledgement fallback
remains. The 64-node retirement scan, generation cursor, backlog barrier, acknowledgement
deduplication and backpressure, and revision-neutral worker release remain.
Active tests now observe stable retired `NodeRef`s rather than installing fake
widget pointers. One test records that a synthesized rescan is observed before
the older queued `GraphChanged`, proves that callback can still read the retired
node, and only then admits and applies the acknowledgement. It also distinguishes
the still-live external `NodeRef` from graph membership after release.

The generic acknowledgement helper no longer arms a redundant 1 ms drain while
retirement collection is already inside `drainWorkerMessages`; the one caller
that can enqueue outside a drain, saturated `PromptMaterialized`, explicitly
schedules it. This removes an empty event-loop pass without changing batching,
deduplication, or backpressure.

The H-08/X-05 slice accounts for native production **−58 CLOC** and native tests
**−3,305 CLOC**. A separate Shell scheduling consolidation removed the competing
`scheduleRender` timer/flag and made the existing pane-commit scheduler the sole
chrome retry owner (**−15 production CLOC**). Cumulative S4 accounting is
34,623 native production CLOC versus 34,696 at entry (**−73**) and 31,473 native
test CLOC versus 34,778 (**−3,305**) at this H-08/Shell checkpoint. Browser code
and the shared fixture are unchanged. All affected targets built, and
`ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -j2 -R
'^(codexui-conversation-cards|codexui-shell-integration|codexui-nodegraph|codexui-thread-channels|codexui-worker-logic|codexui-protocol-updater|codexui-graph-concurrency|codexui-client-runtime-dispatch)$'`
passed 8/8 in 14.73 s; the later exact-current full run also covers all eight.

### Final S4 responsibility reduction and qualification

**Status: complete.**

The remaining S4 invariant was one authority for active-Turn meaning and one
owner for live viewport state. `ConversationItemModel` now receives active-Turn
state solely from graph/adapter projection. The view paints the Turn decoration
from that root row and no longer caches `activeSectionKey_`, stores active state
in `SectionRange`, or mutates a prior model row through `setActiveTurn`. A real
graph `ActiveTurn` relation transition projects both old and new roots, changes
exactly one active model role, and preserves both `ConversationCard` objects and
their geometry while repainting the active/inactive Turn border in place. The
performance benchmark's former one-row synthetic activation was corrected to
remain an append benchmark rather than exercise malformed authority.

Likewise, `mode_`, the live scrollbar/anchor, and the two pause-cause scalars are
the only active-thread viewport authority. `threadStates_` contains inactive
threads only. Fourteen repeated active-state stores plus their helper are
deleted; one successful switch captures the outgoing state and consumes/erases
the incoming state, while rejected and superseded stages mutate neither. Tests
cover manual anchors, composer-owned pause, detached command output, staged
inactive normalization, and rejected selected-thread staging. The unused
`turnContainer` QObject property and its definition-only test probe are deleted.

No production class was extracted, and no production flag, timer, cache,
callback, compatibility path, or special case was added. A proposed deletion of
the empty `ConversationHistoryButton` tag was locally disproven: plain
`QPushButton` selects Qt's built-in accessibility interface and loses the
required busy state. A follow-up attempt to retain the C++ tag while deleting
its `Q_OBJECT` metadata was independently disproven by the same focused test:
the accessibility factory requires the derived Qt meta-object. The minimal tag
and meta-object are therefore retained solely as factory dispatch for the one
accessibility representation, not as a renderer or state authority.

The view-authority slice is native production **−93 CLOC** and native tests
**+141 CLOC**. Final S4 accounting is 34,530 native production CLOC versus
34,696 at entry (**−166**) and 31,614 native test CLOC versus 34,778
(**−3,164**). `cmake --build /tmp/codexui-h07-build.IkJECO -j2` succeeded.
The exact-current full Debug command
`ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -j2`
passed 31/31 in 101.18 s. Its twelve quantitative profiles passed at 320,
1,280, and 10,000 rows and actual DPR 1.0, 1.25, 1.5, and 2.0 in 82.71 s,
versus 82.60 s at S3 completion. Widget/document residency, no-op stability,
construction/release/scan bounds, streaming locality, seeks, structural deltas,
resize, and RSS gates all remained within their existing thresholds. Release,
sanitizer, native XCB/Wayland, real assistive-technology, and warning
qualification remain assigned to S8.
