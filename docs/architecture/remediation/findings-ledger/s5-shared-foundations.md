<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../../media/nav/banner-findings-ledger-compact.svg"><img src="../../../media/nav/banner-findings-ledger.svg" alt="CodexUI — Findings ledger" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Findings ledger"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/up-findings-ledger-dark.svg"><img src="../../../media/nav/up-findings-ledger-light.svg" alt="Up to Findings ledger" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/organization-dark.svg"><img align="right" src="../../../media/nav/organization-light.svg" alt="SNodeC organization" width="160" height="28"></picture></a><a href="../../../../README.md#project-overview" title="CodexUI repository"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/repository-dark.svg"><img align="right" src="../../../media/nav/repository-light.svg" alt="CodexUI repository" width="160" height="28"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# S5 shared foundations and live conversation gates

## S5 stage gate: shared native foundations and bounded secondary views

**Status: complete. Entry accounting was 34,530 native production CLOC and
31,614 native test CLOC; final per-slice accounting is recorded below.**

The first S5 invariant is one native implementation of Markdown, Copy, and
disclosure behavior, with one stable object tree for each retained Inspector
row. Current-worktree tracing confirms that `InspectorPane` still converts a
temporary `QTextDocument` to rich-label HTML, owns separate Agent Copy and
disclosure widgets, and deletes/recreates every Plan/Agent descendant on a
changed row. The full Inspector snapshot remains the state authority; the
accepted correction moves the existing `MarkdownTextView` into a shared
native source owner, promotes the richer existing card controls to shared
native primitives, patches typed retained row children in place, deletes the
Inspector implementations and `clearLayout`, removes duplicated rendered-row
snapshot maps, and prunes expansion keys that no longer correspond to an
authoritative row. Reusing an entire `ConversationCard` is rejected because an
Inspector agent row aggregates several protocol items and has no valid
`CardKey`; inventing one would add false identity and a third renderer.

This H-10/X-09 slice deletes before it reshapes: 84 physical lines of duplicate
Agent controls, 17 lines of passive Markdown conversion, 12 lines of recursive
subtree deletion, two rendered DTO maps, and their rebuild branches. Moving
the already-existing Markdown widget to its own shared compilation unit and
moving the already-existing control implementation into the same
`ConversationPresentation` owner introduce no new presentation authority,
cache, timer kind, functional state, or renderer.
Expected coherent effect is native production **−60..−160 CLOC** and native
tests **0..+120 CLOC**. Required checks are exact descendant/document identity
through semantic no-ops and streaming updates, Markdown selection/link/copy,
Agent Copy MIME and feedback, disclosure/focus/accessible names and state,
thread-switch/removal pruning, row/document construction counts, all
conversation/Inspector/application-layout tests, and the quantitative
conversation performance matrix.

The same S5 foundation slice also removes the byte-equivalent private
`DiffViewer::ChevronComboBox` and `TurnSettingsWidget::CompactComboBox`
implementations before introducing one existing-`UiStyle`-owned combo class.
Both callers retain their own choices, settings semantics, names, dimensions,
and layout; only arrow painting is consolidated. This replaces two geometry and
paint authorities with one and is expected to reduce native production by
roughly 8--18 CLOC without new state. Relevant DiffViewer, Shell/settings,
application-layout, keyboard-focus, and DPR rendering checks must continue to
pass.

The label inventory resolves X-08 without flattening intentional behavior:
the Shell, MiddleRegion, Inspector, ThreadPane, and ConversationCard factories
are byte-equivalent and all opt into mouse selection, while Composer's factory
intentionally does not. S5 deletes only the five equivalent implementations
and reuses one existing-`UiStyle` factory; Composer retains its distinct
interaction policy. This is expected to remove roughly 25--45 native
production CLOC with no new state or abstraction layer. Regression checks must
retain plain-text safety, wrapping, size policy, selection, parent ownership,
focus/navigation, and all affected Shell/middle/card/thread/Inspector behavior.

### S5 H-10/shared-control checkpoint

**Complete for H-10; S5 remains in progress.** The invariant restored here is
that native Card and Inspector Markdown, Copy, disclosure, and status
presentation have one implementation each, while a retained Inspector row has
one stable semantic object tree. The stage deleted the passive Inspector
`QTextDocument`/HTML path, both private Agent controls, both private card
controls, `clearLayout`, the three global rendered-DTO maps, dead functional
and test QObject properties, stale expansion IDs, manual status repolishing,
and both private combo renderers. Existing implementations were moved into
`ConversationPresentation` (Markdown/Card-and-Agent controls/status) and
`UiStyle` (combo arrow rendering); the earlier stage-gate wording that said all
controls moved to `UiStyle` was imprecise and is superseded by this accounting.

The final native accounting is **34,462 production CLOC**, down 68 from the S5
entry of 34,530, and **31,775 test CLOC**, up 161 from 31,614. Excluding the
two CMake files, source-only accounting is 34,446 production and 31,753 tests.
`InspectorPane.{h,cpp}` is now 1,223 CLOC. No new cache, retry, timer kind,
callback path, renderer, or state authority was added. The retained Copy
animation and hold timer are the pre-existing single shared feedback mechanism;
the retained Agent DTO is row-local no-op/Copy presentation state, not protocol
authority.

Verification executed without short-circuiting:

- `cmake --build /tmp/codexui-h07-build.IkJECO -j2`
- `ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -R
  '^(codexui-conversation-cards|codexui-inspector-graph)$' -j2` — 2/2 pass.
- The five-suite Card/Inspector/layout/Diff/Shell focused run — 5/5 pass in
  10.31 seconds.
- `ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -j2` —
  31/31 pass in 101.44 seconds; all 12 quantitative row/DPR performance gates
  pass in 83.01 seconds versus the 82.71-second S5-entry run (+0.36%).
- `git diff --check` — clean; residue searches find one `MarkdownTextView`, one
  Copy control, one disclosure control, and one chevron combo implementation.

The tests prove exact Agent frame/control/document identity, focus, selection,
latest-value rich Copy and zero Markdown mutation through status/no-op updates;
Plan row stability and deliberate thread-owned document retirement; request-row
identity/reordering/exact `NodeRef` replacement; empty-status omission and
running-to-completed token colors without dynamic `tone` state or repolishing;
and the existing multi-DPR control geometry/performance matrix. Focused
reduced-motion feedback and explicit disclosure accessibility state-change
events were absent before this slice and remain classified under M-16/M-21 in
S7 rather than being falsely claimed here. Active-tab demand-window work remains
H-09/M-28 below.

The statement above that this checkpoint pruned stale expansion IDs was later
**disproven during H-09 acceptance**: the old test did not remove and reinsert
the same semantic Agent key, and the widget had no authoritative validity set
for an offscreen key. H-09/M-28 now supplies a bounded retained-key request and
an exact graph-validated result, collapses a resident row when FIFO retention
evicts it, and tests same-key removal/reinsertion. The shared-control portion of
H-10 remains complete; this correction is accounted to H-09/M-28 rather than
hidden by the earlier false-positive test.

### S5 X-08 shared-label checkpoint

**Complete.** Five byte-equivalent selectable-label factories now consume the
one `UiStyle::makeLabel`; their former local implementations are deleted.
Composer's non-selectable factory remains intentionally separate, so the
consolidation does not silently change input or accessibility behavior. The
result is **34,429 native production CLOC** (−33 for this slice, cumulative
−101 from S5 entry) and unchanged **31,775 native test CLOC**. The first build
correctly exposed one formerly-hidden include dependency in
`MiddleRegionWidget`; adding the direct `UiStyle` include completed the
boundary without a shim or new state.

The final build and five affected Card/Thread/Inspector/layout/Shell suites pass
5/5 in 9.43 seconds. The 12-profile quantitative conversation matrix passes in
82.80 seconds versus 82.71 at S5 entry (+0.11%). `git diff --check` is clean,
and the residue inventory now finds exactly the shared selectable factory plus
the intentionally distinct Composer factory.

### S5 M-20 settings-authority stage gate

**Complete.** The restored invariant is that provider settings belong to
`NodeGraph`, the adapter owns their toolkit-neutral projection, and one
non-`QObject` draft policy owns authored overrides, reconciliation,
compatibility, and serialization. Before this repair, `ShellWidget` extracted
raw settings/catalog JSON and tracked four catalog flags/revisions,
`TurnSettingsWidget` used its controls as both draft storage and serialization
input, and `App.tsx::SettingsPanel` independently interpreted
catalog/default/compatibility rules beside the TypeScript value policy.

Deletion preceded reshaping. This stage deleted Shell's settings JSON copier,
catalog-array copier, catalog revision/presence bookkeeping, and field-revision
fallback; deleted the widget's control-derived serializer, canonical/catalog
JSON authorities, touched array, duplicated normalization, and compatibility
branches; and deleted React's inline catalog filtering, static reasoning list,
free-text service-tier policy, and always-enabled compatibility controls. The
existing `NodeGraphUiAdapter`, `TurnSettingsWidget` files, and
`TurnSettingsOptions.ts` now form one value transaction and one draft policy
per frontend. Controls are projections and edit sources only. This is not a
class-only extraction: the former policy/state owners and their branches were
removed in the same substage.

Current history resolves the collaboration contradiction. Commit `eb916a6`
deliberately established that `turn/start` always includes
`collaborationMode` when a selected or default catalog model resolves;
`thread/start` has no collaboration field, and `turn/steer` carries no
turn-start settings. The later browser touched-only guard is semantic drift,
not a newer product decision. Commit `c8a4528` deliberately preserves authored
browser drafts per stable thread identity, so native draft switching must gain
that same deterministic identity behavior rather than deleting it. An
authoritative settings update clears only corresponding acknowledged fields;
top-level settings members replace rather than recursively merge nested
objects, matching `ProtocolUpdater::mergeEffectiveThreadSettings`.

The local audit is the strongest evidence for M-20. Claude A explicitly did not
audit `TurnSettingsWidget`; Claude B establishes that a TypeScript-only parity
test cannot prove native/browser meaning; ChatGPT inventories both settings
implementations without tracing them deeply. None contradicts the locally
verified responsibility violation. The completed read-only audit's claim that
`App.tsx` is prohibited is **disproven by the active user scope and instruction
search**: no such exclusion exists. Its existing user-owned Copy-icon hunk will
be preserved byte-for-byte, and only the settings import/call/panel region may
be changed.

One schema-versioned settings corpus now executes in both existing frontend
runners. It covers malformed/hidden catalogs, empty IDs, unknown editable
selections, model defaults/effort constraints/service tiers, personality
support, permission/access/network mutual exclusion, canonical aliases and
top-level replacement, acknowledgement, per-thread switching, workspace,
every thread/turn option, default-model collaboration, and start-versus-steer
wire payloads. Qt boundary checks cover labels/buddies, accessible names,
enabled state, tooltips, changed indication, editable values, and signal
blocking. The real-Chromium gate checks all twelve DOM label associations,
disabled reasons, unknown values, all authored values, and the exact resulting
`turn/start` payload. It also proves that a semantic settings no-op preserves
the grid, controls, options, focus, scroll, and zero layout/style/structural
mutation.

The last separately measurable M-20 checkpoint was native production **−15
CLOC**, browser production **−16 CLOC**, combined production **−31 CLOC**;
native tests were **+679 CLOC**, browser tests **+168 CLOC**, and the shared
fixture was **+315 CLOC**. Later H-08/M-32/M-33 edits were co-mingled without a
saved tree, so a more precise final M-20-only total would be invented. The
final audit found one remaining duplicate workspace precedence path and fixed
it by making the new-thread dialog workspace a fallback only when the policy
did not author `cwd`: one production line changed for **net 0 production
CLOC**. That correction added **+3 browser-test CLOC** and **+91 CLOC** to the
existing Chromium qualification tool (60-line DOM transaction, 26 assertions,
5-line catalog-fixture expansion). No timer, cache, dynamic functional
property, serializer-reading-widget path, or parallel settings authority was
introduced.

Exact-current verification passes: native Debug **31/31** after rerunning the
two environment-sensitive cases with their required host/socket and isolated
configuration conditions; native Release **31/31** in 82.06 seconds with no
build warnings; all **12/12** quantitative DPR/performance profiles; browser
**282/282**; and the complete browser release gate. The latter measured 10k
hydration at 88.60 ms wall/95.98 ms task, a semantic no-op at 11.02 ms with
zero layout/style passes, a 2k stream at 14.60/36.80/37.23 ms, and a detached
anchor delta of 0.390625 px. A fresh exact-current GCC 16.2 ASan+UBSan build
completed without warnings and passed **31/31** tests in 223.84 seconds with
`detect_leaks=1`, fail-fast sanitizers, and an explicit 64 MiB ASan quarantine;
no ASan, LeakSanitizer, or UBSan diagnostic occurred. With ASan's unspecified
runtime-default quarantine, only the 10,000-row DPR 1.5 and 2.0 sampled-RSS
limits failed (29/31), while every live-resource, residency, geometry, no-op,
and timing gate passed. Repeating with only `quarantine_size_mb=64` reduced
sampled peaks from approximately 938--983 MiB to 472--487 MiB and passed the
same matrix, classifying that excess as sanitizer allocator retention rather
than retained UI state. Prebuilt dependencies were not instrumented, and
sanitizer timings use the benchmark's explicit 4x allowance rather than the
Release performance baseline.

### S5 native thread-hydration completion gate

**M-32 complete, including the live controller/observer paginated gate.** The
user-visible normal thread-loading failure is graph state, not a
`ConversationView` rendering
decision: Shell shows the failed snapshot only after the selected exact Thread
has `hydrationState=failed`. A genuine current `thread/read` error may author
that state. The current false-failure path is `WorkerLogic`: it discards the
`AppliedMessage::primary` returned by `ProtocolUpdater::applyInto`, then scans
every Item in the merged Thread and independently guesses whether the result
was accepted. A rejected completion can therefore author false ready/failed
state, and the scan lets one incomplete live or legacy Item become a second
authority over the exact request outcome.

Current-worktree and captured-runtime evidence separate the hypotheses. The
committed negative Worker test constructs a typeless `item/plan/delta` and a
schema-invalid read Turn that omits `items`, thereby codifying whole-thread
failure rather than the valid app-server contract. The current authoritative
replacement already removes an omitted provider delta when a valid result
contains `items: []`. Eleven captured real `thread/read` results preserve the
frontend request IDs, contain exact Thread/Turn/item structure, and contain
zero typeless items, including the 108-Turn/2,526-item response. Payload shape
and wire-ID loss are therefore disproven for that capture; a genuine transport
error remains possible for the user's uncaptured instance.

The restored invariant is that the exact current Operation accepted by the
reducer is the sole hydration-completion authority. This stage first deletes
the full merged-graph Item scan, its `checkedItems` state, and the
schema-invalid failure expectation. `completeThreadHydration` consumes the
existing `AppliedMessage::primary`: a rejected/stale completion is inert; an
accepted success additionally requires the existing exact current Thread and
matching payload Thread ID; only an accepted current error may author failure.
Partial Item state remains locally projectable as generic activity and cannot
replace request lifecycle truth. No flag, timer, cache, callback, retry, or
additional state owner is introduced.

Required checks cover accepted success/error, prior-error clearing, operation
retirement, reused IDs for late success and late error, missing/wrong IDs and
methods, replaced Thread incarnations, mismatched response Thread IDs, a valid
empty-items authoritative replacement, Shell Loading-versus-failed
presentation, and the existing runtime bridge read. Performance verification
must show O(1) completion admission instead of an O(history-items) scan and
rerun the native quantitative matrix. Accessibility behavior is unchanged;
the existing loading/error label remains the one representation. Expected
effect is native production **−20..−40 CLOC** and native tests **0..+80 CLOC**.

The completed slice deletes **33 native production CLOC** and adds **195 native
test CLOC**. The larger-than-estimated test change is deliberate exact-operation
and scaling coverage; it does not add runtime machinery. Current totals are
**34,679 native production CLOC** and **34,949 native test CLOC**. No timer,
cache, flag, retry, compatibility branch, or alternate hydration authority was
added.

The completed qualification evidence is:

- an isolated exact app-server 0.144.6 persisted fixture returned
  `historyMode=legacy` from `thread/list` and accepted
  `thread/read(includeTurns:true)` with one complete Turn;
- `cmake --build /tmp/codexui-h07-build.IkJECO -j2` reports no pending build
  work;
- `ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -E
  '^codexui-conversation-performance-' -j2` passes all **19/19** registered
  correctness tests;
- `ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -R
  '^codexui-conversation-performance-' -j1` passes **12/12** quantitative gates
  at 320, 1,280, and 10,000 rows and DPR 1.0, 1.25, 1.5, and 2.0;
- the direct Worker gate measures 64 completions behind 2,000 versus 40,000
  retained Items at **2,503,657 ns / 2,893,759 ns** (1.16×, below the 4× plus
  10 ms limit);
- a normal `QT_QPA_PLATFORM=xcb` application window under Xvfb plus xfwm4 with
  a synthetic bridge peer selected a visible legacy row, emitted the exact
  `thread/read {includeTurns:true}`, accepted the correlated result, completed
  its controller-only settings resume, and rendered the selected Thread; the
  corrected full xcb QWidget run passes **8/8** executables with a real window
  manager and active focus;
- the installed bridge is version **1.0-rc1**, SHA-256
  `9b6ee2cfeceac4f23780a7fd8cbfd311d0adba2b438f4577afc79df89a154c2a`.
  The main running bridge executable and the isolated live-verification bridge
  have that exact hash, and the installed binary contains the required
  `thread/turns/list` observer route;
- two exact-current native clients connected to the already-running isolated
  installed bridge, with the first retaining controller authority and the
  second visibly reporting `Claim control`/observer state. Selecting the known
  paginated Thread `01a09630-5237-78e3-938e-5817990c3092` caused the observer
  to send correlated request `frontend-6-request-7` using
  `thread/turns/list`, `itemsView=full`, `limit=80`, and descending order. The
  bridge returned a correlated `result` as role `observer`, and the UI rendered
  the “Big architectural refactoring” history instead of the failure snapshot.
  No bridge, app-server, or pre-existing UI was restarted or replaced.

The earlier “external allowlist update” blocker is therefore **superseded by
installed and live evidence**. Paginated selection and observer routing are in
scope and pass. A live ephemeral/not-yet-materialized Thread remains a distinct
contract case: the create-first-prompt flow avoids an illegal history read, and
future evidence must continue choosing the legal history authority from
authoritative Thread metadata rather than version probes, error-string
fallback, or retries.

### S5 graph-retirement locality gate

**Complete under the explicitly approved M-33 ceiling of +55 production
lines.** The new hydration completion gate no longer scans Thread history, yet
the pre-fix 64 exact
request/result cycles retained behind 40,000 unrelated Items took
2,349,936,111 ns versus 84,299,100 ns behind 2,000 Items. The remaining growth
comes from `NodeGraph::WriteAccess::removeMany`: retiring even the newest leaf
Operation copies the complete canonical map and retired containers, rebuilds
the complete insertion-order vector, and walks every live node twice to
discover incoming relations. The existing batch-removal test only doubles the
number being removed along with total graph size, so it did not constrain the
cost of a small retirement against a large unrelated retained graph.

The restored invariant is that mutation work is proportional to the affected
topology and the insertion-order suffix that can actually move, never to an
unrelated graph prefix. The current worktree maintains one derived
incoming-relation index inside the existing relation authority and uses it to
delete the two whole-graph topology walks. Removal mutates the already-owned
canonical and ordered containers after all
fallible validation/allocation, preserving insertion order, stable retired
`NodeRef`s, graph-change identities, child-list events, structure revisions,
PendingOperation closure, and revision-neutral acknowledgement. The derived
index replaces discovery scans in the same change; it is not protocol state or
a second semantic relation authority.

Correctness checks cover relate/unrelate/replace, self-relations, inbound and
outbound edges, parent/child removal, mixed and duplicate batches,
PendingOperation closure, rejected foreign refs, stable retirement lifetime,
and exact GraphChange/revision behavior. The M-32 end-to-end gate will compare
the same completion workload behind 2,000 and 40,000 unrelated Items; existing
batch scaling, full nodegraph/runtime/Shell tests, sanitizers, and the native
quantitative matrix remain required. Accessibility is unaffected. The original
native production forecast of **−20..−80 CLOC** was disproven by the strong
exception guarantee: removing the whole-graph copies requires staging exact
map iterators, inverse targets, and the first movable ordered suffix before the
no-fail mutation phase. After that evidence was presented, the user explicitly
approved a hard **+55 production-line** ceiling for this stage.

Post-fix execution now records **2,503,657 ns / 2,893,759 ns** for the exact
2,000/40,000-Item 64-completion workload (1.16× instead of 27.9×), plus
**986,463 ns / 1,112,941 ns** for newest-tail retirement behind 2,000/40,000
unchanged neighbors. All NodeGraph relation, removal, retirement-lifetime,
batch-scaling, misuse, and concurrency cases pass. No concrete correctness or
exception-safety defect was found in the current reverse-index maintenance. A
final independent reduction audit verified that `relations_` remains the sole
semantic authority; the private `incomingRelations_` vector is derived, has no
public reader, and is maintained by every existing relation mutation. It also
confirmed that deleting grouping, target deduplication, capacity preparation,
or suffix discovery would respectively restore the 40,000-target scan,
duplicate shared-target work, weaken exception safety, or copy canonical
containers again.

That audit attributed 56 production lines to the narrow reverse-index core. A
single-use relation alias was then deleted for **−1 production CLOC**, leaving
the approved **+55 production CLOC**. Redundant retirement-slot coverage inside
the inverse-edge test was deleted for **−17 test CLOC**; the M-33-specific test
attribution is now approximately **+169 CLOC** (a separate 38-CLOC
`PendingOperation` cascade belongs to M-32). Exact historical stage accounting
cannot be reconstructed because H-08, M-32, and M-33 were co-mingled without a
saved tree; the auditable whole-file NodeGraph diff is now **+336/−178 (net
+158 physical lines)** and the previously recorded co-mingled test subtotal is
**+1,102/−213 (net +889)**. These totals are not mislabeled as M-33-only.

After the reductions, Debug nodegraph, protocol-updater, worker-logic,
graph-concurrency, and Shell integration tests passed **5/5** in 7.20 seconds.
The direct Debug locality measurement was 981,528 / 1,123,057 ns behind
2,000/40,000 unchanged neighbors; the independent Release run measured 108,839
/ 150,598 ns (1.38x). The exact-current ASan+UBSan 31/31 matrix also covers this
implementation. No compatibility index, fallback scan, timer, cache, or second
relation API remains.

### S5 H-09/M-28 residency approval gate

**Complete under the user's explicit +1,600 native production CLOC ceiling
(36,252 total against the 34,692 entry baseline).** The original
+650 ceiling was retained as a hard stop when the first correct candidate
measured 36,552 native production CLOC, +1,860 from the entry baseline. Two
independent read-only reduction audits found that deleting the full 1,210-CLOC
excess would require either restoring a history scan, weakening
focus/paging/residency guarantees, or extracting a callback-heavy common
viewport that would move rather than remove policy. They converged on 615--875
CLOC of defensible further deletion. On 2026-09-15 the user first approved 600
additional CLOC, revising the ceiling from +650 to +1,250, then explicitly
accepted **+1,600** after current-worktree audits proved +1,250 too small for
the correct indexed projection and residency contract. Correctness,
accessibility, DPR, Release, sanitizer, and quantitative performance gates are
unchanged. The +1,860 candidate remains rejected. Deletion-only consolidation
reduced the accepted result to **36,252 CLOC (+1,560)**, 300 lines below that
candidate and 40 lines below the revised ceiling. Further safe
reduction remains required as an architectural objective; the new margin is
not an allowance for optional production mechanisms.
H-10 removed duplicated renderers and rebuild ownership. The
remaining defect has two ordered causes: active Plan/Agents/Requests tabs still
materialize every row, and the adapter still obtains those rows through full
Plan/Agent folds and Request copies. Today, visiting all tabs retains
approximately `3P + 10A + D + 7R` row widgets (`D <= A` result documents),
full P/A/R vectors, and all-row maps. Agent projection scans selected history
and can rescan the same child history; Request and State projection also retain
avoidable full scans/copies. The old 2,796--3,863-CLOC cursor/retry/window
implementations are explicitly rejected.

The current Shell `inspectorScanPasses` and `inspectorRowConstructions`
assertions are **disproven as measurements**: their production dynamic
properties no longer exist, so they read zero regardless of work. They must be
deleted and replaced by tests at the eventual indexed projection/residency
boundaries, not restored as QObject observability.

Two independent read-only reviews recommended landing graph indexes and
residency atomically. The primary architecture decision instead separates them
at their real dependency boundary without declaring the second fixed: H-09
first replaces only QWidget/document residency and geometry; M-28 immediately
follows by replacing full graph folds/copies with bounded projection. This is
not a compatibility migration or a parallel renderer. The user's governing
invariant explicitly permits offscreen rows as model data plus cached scalar
heights, so the existing value vectors remain legitimate model input during
H-09 while every all-row QWidget path is deleted. A demand window cannot be
specified or verified until that one viewport authority exists. **H-09 may be
completed independently; M-28 must remain open until the full projection path
is deleted.**

The accepted H-09 replacement is one shared variable-height row-residency
mechanism used by Plan, Agents, and Requests. It reuses
`ConversationHeightIndex`, retains `PlanStepFrame`, `AgentFrame`, the Request
row, shared Markdown, Copy, disclosure, and status controls as the sole
renderers, and keeps only visible/overscan widgets plus at most one focused
row. It deletes `planFrames`, `agentFrames`, `requestFrames`, the three desired
set/reorder loops, and three `QVBoxLayout` row-geometry authorities in the same
stage. Hidden pages release their entire QWidget/document tree. No lossy cap,
timer/retry cursor, passive renderer, delegate, pool, full-widget cache, or
new height implementation is accepted.

The H-09 slice was forecast at **+300..+480 gross / -220..-360 deleted, net
-60..+260 native production CLOC**, with **+250..+450 test CLOC**. That forecast
was disproven by the complete remote-page, variable-height, focus-pin,
keyboard, request-lifetime, and geometry contract. The combined H-09/M-28
ceiling is now **net +1,600 native production CLOC** after the explicit
reapprovals recorded above. M-28's
subsequent allowed scope is a bounded Plan/Agent/Request window contract,
graph-owned source/identity indexes, direct exact Request lookup and summary,
and deletion of full snapshot vectors, Shell request copies, and historical
adapter folds. The reviews agree that an endpoint-only Agent relation is
lossy: fan-out, contributor folding, fallback identity, correlation,
reincarnation, and result-source semantics require typed graph-owned selectors
or equivalent canonical index, never copied formatted UI rows.

H-11 has a separate accessibility gate. A nested `QTreeWidget` plus one
full-row delegate can delete the current flat hierarchy, per-row widgets,
manual disclosure/hit testing, custom Left/Right navigation, and full-viewport
animation repaint for an expected −150..−250 CLOC before accessibility.
However Qt 6.6 exposes Tree/TreeItem role and expanded state but flattens item
parents and supplies no expand/collapse accessible action. A correct custom
interface is estimated at +200..+320 CLOC, moving the coherent ThreadPane
result to roughly −50..+170. Plain QTree semantics cannot be falsely labelled
complete; any net growth or new accessibility interface also requires explicit
approval.

H-09 restores the invariant that virtualization
owns residency, scheduling, and cached scalar heights but never appearance:
one exact row QWidget is both what the user sees and what receives input and
accessibility. A semantic no-op retains exact visible descendants, geometry,
focus, selection, expansion, scroll, and accessibility identity. Scroll,
resize, tab/thread switch, insertion, removal, reorder, and DPR qualification
must keep ordinary residency at or below 50 rows plus at most one focus pin.
Production and test CLOC are recorded from the exact pre-stage worktree before
editing, then remeasured after each coherent slice.

The exact H-09 pre-edit CLOC baseline is native production **34,692**, browser
production **5,538**, native tests **34,933**, browser tests **3,263**, and
shared fixture data **921**. `InspectorPane.{cpp,h}` accounts for **1,230** of
the native production total. The registered pre-edit Inspector executable
passes its existing small fixtures in **0.26 s** offscreen; this is a
correctness baseline only and is explicitly not a quantitative residency gate.
An exact-current offscreen construction probe records Plan/Agent respectively:
**143,403/87,933 us and 320/320 row widgets at 320 rows**,
**947,027/645,931 us and 1,280/1,280 at 1,280**, and
**49,108,185/36,110,836 us and 10,000/10,000 at 10,000**. The corresponding
QObject-tree `QTextDocument` counts were 1,929, 7,689, and 60,009. This is the
quantitative pre-edit baseline; it proves superlinear all-row construction and
unbounded retained documents rather than merely a source-derived risk.

At its entry gate, M-28 remained explicitly open. The reconciled graph design
favored exact source
objects and selectors rather than copied presentation values: ordered Plan
sources must reveal the predecessor after removal; Agent indexing must preserve
fan-out, multiple contributors, exact/fallback collisions, correlation,
terminal status precedence, and latest child result; Requests already have
`Runtime -> PendingInteraction` order and must not gain a redundant discovery
cache. State's whole-graph summary and Changes' history scan are separately
tracked and cannot be called fixed by P/A/R residency or windowing.

The accepted M-28 replacement is now concrete. `WriteAccess::finish()` is the
only safe maintenance seam because `WorkerLogic` and direct graph writers can
mutate topology after `ProtocolUpdater` returns. A Thread-owned derived index
will retain only ordered semantic selectors: the selected Plan source plus its
row selectors, and ordered logical-Agent identities plus their exact ordered
source/child contributors. It will not retain formatted labels, Markdown,
`UiStatus`, request descriptors, or QWidget data. Ordinary value streaming will
therefore reproject only demanded rows; identity/topology changes will rebuild
only the affected Thread index from final transaction state. Dependencies on
child Thread status, correlation, lifetime, and newest nonempty agent-message
results remain graph-owned and exact. An endpoint-only `LogicalAgent` relation
is rejected as lossy, and a ProtocolUpdater-only cache is rejected as stale for
direct and post-updater writers.

The Qt boundary becomes one bounded page request containing projection, first
row, count, and an optional semantic anchor key. Its result contains total row
count, structure generation, anchor index, and no more than the residency
window's values. `RowViewport` remains the sole geometry/residency authority;
the existing Shell pane-commit scheduler remains the sole retry/coalescing
mechanism. Full Plan/Agent/Request vectors, all-vector comparison maps, and
Shell request copies are deleted. Request order comes directly from the
existing `Runtime -> PendingInteraction` relation: window, exact-target, and
count/attention-summary reads replace the full request collection, while graph
removal notifications retire local submission overlays. No Request cache,
cursor, timer, passive renderer, or compatibility snapshot is accepted.

The graph-index slice is implemented and accepted after adversarial review. Plan now
retains one authoritative selected source plus positional row selectors; Agent
retains logical identities, exact contributors, and dependency-local winner
sets rather than formatted UI rows. Bounded adapter projection consumes those
selectors directly. The remaining Request slice has additionally verified a
duplicated authority: `ProtocolUpdater` records each pending Interaction under
both Runtime and its then-current Thread, so reparenting an Interaction target
leaves the Thread edge stale while Requests/Shell invalidation misses the
ancestry change. The accepted correction deletes the Thread edge and its
cleanup traversal. Runtime membership/order remains authoritative; exact
InteractionTarget ancestry with payload Thread fallback supplies context; the
adapter derives Thread attention counts in one pass and owns one Requests
dependency predicate consumed by both Inspector and Shell. A released-retired
or foreign exact target is rejected at that adapter boundary before relation
queries. No cache, timer, forwarding path, or replacement relation is added.
Expected Request-slice effect was native production -5..+16 CLOC and native
tests +150..+230 CLOC; correctness includes target/ancestor moves and removal,
payload fallback, exact target retirement, selected/unselected Shell routing,
and pending-count movement, while the performance gate compares 2,000/10,000
pending requests without persistent derived state. **Implementation status:
implemented and focused-green. Runtime is the sole pending-membership/order
authority; exact ancestry supplies Request context and exact-only Thread badge
counts, while payload identity is fallback context only. A live ancestor move
retains the exact Inspector and compact-attention objects, keyboard focus,
accessibility IDs, and unrelated Thread selection while moving context/badge
through one Thread, Inspector, and chrome commit.**

The M-28 correctness gate covers Plan empty/malformed/legacy precedence and
predecessor removal; Agent fan-out, contributor folding, non-creating updates,
exact/source collision, correlation, reincarnation/rebind, terminal precedence,
child-result replacement/reorder/removal, paging, draft promotion, and provider
reset; and Request window order, insert/remove anchors, exact action lookup,
attention choice, local submitting state, and stale retirement. Performance
must show no more than 50 projected row values, 50 ordinary resident widgets,
51 with one focus pin, no full descriptor/vector copy, no selected-history scan
for an ordinary resident-row value update, and bounded scroll/frame work at
10,000 rows. Accessibility, focus, selection, DPR 1.0/1.25/1.5/2.0, Debug,
Release, and sanitizer checks remain mandatory. The original M-28 LOC forecast
was disproven by the exact contributor, predecessor, mutation-order, and paging
contracts. The combined H-09/M-28 result must remain at or below the revised
**+1,600 native production CLOC** ceiling. **Implementation status: Plan/Agent
selector indexing, bounded paging, and Request authority/invalidation are
implemented and locally green. Candidate sets are the sole winner authority;
source/Turn dependency paths and transaction bookkeeping are merged; bounded
page mechanics are shared; cached Plan Turn, message-position mirrors, staged
index publication, cached detail/receiver/result/status winners, and redundant
residency transitions are deleted. A shared Plan decoder and shared Agent
status/result semantics replaced repeated folds. Exact semantic message order
is indexed independently from insertion order, fixing locally reproduced Item
and Turn reorder defects. Direct section revisions now replace transaction-
local changed/order mirrors while preserving untouched Plan or Agent storage
and revisions. Final native production is **36,252 CLOC (+1,560)** and native
tests are **39,618 CLOC (+4,685)** against the 34,692/34,933 stage-entry
baselines. `InspectorPane.{cpp,h}` is 1,718 CLOC. **Combined H-09/M-28
implementation and acceptance are complete.**

The final reduction audit removed an unconditional `raise()` from every
resident layout pass. Rows have one non-overlapping geometry authority, so the
restack had no presentation purpose; deleting it reduced an isolated visible
Agent update from 259 to 3 paint events without changing geometry, focus, or
overlap. No renderer or document is constructed for that update; it performs
one Markdown source transition, two document mutations, one measurement, and
two layout requests. The only deferred geometry callback is the existing
font/style/DPR invalidation, now documented as waiting for Qt descendants to
observe one environment. The retained Agent expansion set is graph-validated,
deduplicated, FIFO-bounded to 50 keys, and collapses an evicted resident through
the existing viewport geometry authority.

The registered benchmark now builds a real 10,000-Plan/Agent/Request graph and
executes adapter paging rather than preprojected vectors. Across actual DPR
1.0, 1.25, 1.5, and 2.0, initial Plan projection is about 131--134 ms, Agent
activation 69--71 ms, and Request activation 50--53 ms, compared with the
pre-stage 49.1/36.1 second 10,000-row all-widget Plan/Agent baseline. Resident
peaks are Plan 33, Agent 48, Request 24, and Markdown documents 48; no response
contains more than 50 ordinary rows or 51 values with a focus pin. Individual
real projections remain below 0.9 ms and 32 rotating 10,000-row windows remain
below 15 ms. The independent 2,000/10,000 Request paging ratio gate passes.
Construction, retirement, parse, measurement, layout, paint, page-demand, and
QWidget/document peaks all have quantitative structural limits.

Correctness covers insertion and removal above a retained Request anchor,
focused-row action lifetime, hidden-tab freshness, exact Agent expansion
retirement, no-op accessible IDs, exact accessible row/control cardinality,
keyboard traversal, resizing, thread replacement, and all four DPRs. Release
passes **39/39**; the focused Debug adapter/Inspector/geometry/performance
matrix passes **10/10** under ASan+UBSan with leak detection and a bounded
quarantine, and the registered Inspector matrix passes **9/9** in Debug. The
current restricted continuation executes every Debug case: all conversation
and Inspector quantitative tests pass; the Git live case passes with an
isolated writable XDG configuration; only the runtime-dispatch executable is
environment-blocked because this sandbox denies its private AF_UNIX listener,
while that exact executable passed in the unrestricted full Release run.
`git diff --check` is clean. Real AT-SPI and native XCB/Wayland breadth remain
S8 qualification, not a hidden H-09/M-28 implementation path.

### S5 M-34/M-35/M-36 live conversation gate and M-37 paging boundary

**Status: M-34, M-35, M-36, M-37, H-11, the later reopened S1-D10, and
M-44/MR-7 are complete. MR-8 is next.** Three
independent current-product observations
were made in the selected `Big architectural refactoring` thread on
2026-09-16. They are new worktree findings, not retroactive reinterpretations
of an external review.

**M-34 — missing `You` cards.** Older non-root user-authored cards are absent,
and a steering message submitted during a still-running turn will also age out
while later Codex/tool cards remain. Read-only GDB inspection of the already
running `/usr/local/bin/codex-ui` (PID 2285053; no restart or mutation) first
found the selected thread `01a09630-5237-78e3-938e-5817990c3092` in Following
mode with 82 retained model rows, 80 history-activity rows, 6,077 hidden
authoritative items, two model sections, and `hasMore=true`. A later exhaustive
row dump found 100 rows/98 activities/6,124 hidden items and proved that the
current opening prompt (`Now you have full acces...`) was present as the active
Turn root; it was merely above the bottom viewport. The definitely missing live
row is the acknowledged non-root steering prompt `Older turn-you cards gets
lost`, which remains in the persisted rollout but not in the model. Therefore
the report is **not** evidence that the opening current root disappeared in
this capture. It is direct evidence that non-root `UserMessage` rows disappear,
including current-turn steering inputs once enough later activity accumulates.

Current source establishes the first faulty boundary. `NodeGraphUiAdapter`
selects a newest raw-authoritative-item suffix and pins only the Turn root when
that Turn already has selected activity. `ConversationItemModel::trimHistoryTo`
then protects only that root while front-trimming every non-root `UserMessage`
as ordinary history activity. The window is therefore defined in protocol-item
units rather than prompt-owned semantic conversation segments. This is a
projection/window defect, not graph ingestion or lazy widget residency.
Expected offscreen widget non-residency is not classified as loss: a valid
model row retains a positive scalar height and materializes when scrolled into
the viewport.

The live capture proves that a loaded acknowledged `You` card is absent, but it
does not prove that activity from that older prompt's segment remains in the
current raw suffix. A cutoff-owner-only rule would fix a reproducible orphan
without restoring the exact reported row and is therefore **superseded**.
Loaded user-authored messages are semantic transcript landmarks, not disposable
tool/activity detail: they remain model rows even when their complete detail
segment has aged outside N; only provider pages not yet loaded remain behind
explicit history paging. QWidget/document residency remains independently
viewport-bounded.

The violated invariants are I-2/I-4/I-5/I-7: every authored semantic item has
one stable identity from graph through adapter/model/view, bounded history may
control ordinary history detail but may not discard an admitted `You` concept,
and snapshot and incremental paths must implement one rule. The accepted
reduction/change keeps the newest N authoritative rows plus every loaded `You`
landmark and the view-owned Turn root; rows retained only as landmarks do not
consume N. Replace `TurnSection::rootPinned` rather than add parallel per-card
state, and delete root-only trim branches/accounting. No copied history cache,
prompt index, renderer, card injection, timer, or reconciliation flag is
authorized. The no-new-state reverse scan must first pass a quantitative
10,000/40,000-item snapshot gate; if it does not, work stops before proposing
new graph state.

The exact pre-production-edit worktree is **36,209 native production CLOC**
(the paused H-11 header is already 43 below its 36,252 entry) and **39,885 native
test CLOC**. Two public-boundary regressions take tests to **40,034 CLOC
(+149)**. Both fail on the old rule: `codexui-nodegraph-ui-adapter-test` reports
that eight budget rows plus all loaded `You` landmarks were not retained, and
`codexui-conversation-item-model-test` reports that trim discarded a loaded
landmark. Verification must additionally cover optimistic promotion, 80/160
paging, switch-away/back, anchors, positive scalar geometry, exact resident-card
identity/content, accessibility, and the four-DPR residency matrix. Remaining
test forecast is **+40..+110 CLOC**; production must be **neutral or lower**,
otherwise work stops for approval.

**M-34 implementation status: complete.** `conversationHistoryLandmark` is the
single semantic rule for a Turn root or `UserMessage`. The adapter now projects
the newest N authoritative activities plus every loaded `You` landmark and the
view-owned root; local prompts remain resident and retained landmarks do not
consume N. `TurnSection::retainedHistoryOwnerCount` replaces root-only pinning,
and represented-set arithmetic remains the sole hidden-count calculation. The
item model applies the same rule after structural transactions, rejects a
missing semantic landmark to the existing authoritative-snapshot path, and
keeps ordinary off-window deltas as admitted no-ops. A presentation-only
promotion of an omitted row to `UserMessage` is covered by a failing-first
regression and can no longer disappear.

The final performance gate exposed a second M-34-local inefficiency: a
same-position root replacement preserved canonical membership but rescanned all
10,000 history rows. The structural operation now carries one ephemeral
`historyMembershipPreserved` fact, derived and consumed inside the same
transaction; it preserves the existing row membership and deletes that no-op
scan. This is not persistent UI state, a cache, flag authority, timer, queue, or
renderer. The reproduced DPR-2 structural-commit median/p95/max changed from
5.443/5.601/5.685 ms (failing the 5 ms median gate) to
4.656/4.727/4.794 ms, with the gate unchanged.

Final M-34 accounting is **36,207 native production CLOC**, down **2** from the
36,209 entry, and **40,500 native test CLOC**, up **615** from 39,885. The test
forecast was disproven by the required 10k/40k scaling, dense-landmark,
mixed-operation differential, presentation-promotion, accessibility, thread
round-trip, bounded-work, and non-short-circuit coverage; the production
neutral-or-lower gate is met. No persistent cache/index/timer/queue, renderer,
or compatibility path was added.

Exact-current verification:

- Debug focused correctness plus all row/DPR profiles:
  `ctest --test-dir ../build/Desktop_GCC-Debug --output-on-failure -j1 -R
  '^(codexui-nodegraph-ui-adapter|codexui-nodegraph-conversation-ui|codexui-conversation-item-model|codexui-conversation-virtualization|codexui-conversation-performance-)'`
  passed **16/16** in 91.94 s.
- Release focused correctness passed **4/4** in 5.12 s; the separately rebuilt
  exact-current performance matrix passed **12/12** in 59.47 s.
- `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:quarantine_size_mb=64` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` focused correctness passed
  **4/4** in 15.70 s; the exact-current sanitizer performance matrix passed
  **12/12** in 186.46 s without a diagnostic.
- Targeted `clang-format --dry-run --Werror` checks and `git diff --check` pass.
  Independent adversarial review plus one million randomized valid structural
  transitions found no snapshot/incremental counterexample.

The count-less disconnected-ordinary-row nuance is **disproven on the
production path**: `ProtocolUpdater` maintains or repairs
`historyLoadedItemCount` for structural history changes. Synthetic callers
without that authority must request a full snapshot. Native XCB/Wayland and
real AT-SPI breadth remain S8 qualification, and the installed application was
not replaced or restarted; completion here is the exact current worktree and
registered-gate result.

**M-35 — laggy conversation scrolling regression.** The same live thread now
scrolls visibly poorly according to the user, despite the live model retaining
only 82 rows. This is a separate performance failure: M-34 can omit content
without causing frame latency, while a materialization/layout/paint regression
can lag with a correct model. History and same-era A/B execution localize the
regression to the uncommitted S1 sole-widget conversion between committed HEAD
`629660e` (2026-09-12) and WIP evidence no later than stash `9b8a422`
(2026-09-14 18:55); the installed live binary has the exact current Debug Build
ID. Commit `6c4a0e1`'s full-widget viewport measured 2,367,482/1,921,881 us for
240 positions at 320/1,280 rows, while `bd2297e`'s passive delegate measured
203,512/219,760 us on the same historical fixture: an 11.6x/8.7x improvement.
S1 correctly deleted that second renderer but restored the earlier synchronous
full-widget cost profile without replacing its scroll mechanics.

Fresh current Debug 10,000-row/DPR-1 measurement records normal-scroll
median/p95/max **8.909/11.319/11.666 ms**, up to **38 layouts and 100 paints per
event**, and seek **60.609/64.061/64.518 ms**, with 31 cards/481 widgets. The
registered benchmark still passes because its normal gates are 15/30/50 ms and
seek gates 70/85/120 ms. The height index is ruled out as primary (30
logarithmic update steps). The proven hot path synchronously constructs,
Markdown-parses, and settles the three-viewport residency window, reapplies
geometry/visibility to every resident, and unconditionally invalidates the full
viewport inside `scrollContentsBy`.

The violated invariants are I-1/I-3/I-4/I-9: lazy residency must remain, wheel
input must reach the visual object directly, and work per scroll frame must be
bounded by the viewport/overscan population rather than retained history or
document count. Remediation must delete or consolidate redundant per-wheel
layout, measurement, materialization, animation, or paint work before adding
mechanisms. Verification requires recorded pre-fix and post-fix wheel/frame
profiles, materialization and retirement counts, parse/measurement/document
counts, dirty-region bounds, anchor/pixel stability, and the existing
320/1,280/10,000-row DPR 1.0/1.25/1.5/2.0 matrix without weakening gates.
Production is forecast **neutral to negative** and tests **+40..+100 CLOC**;
any necessary production growth requires explicit approval.

**M-35 accepted implementation gate.** Disposable instrumentation of the exact
current worktree attributes a 233 px scroll sample's 9.134 ms mean total to
6.670 ms in the synchronous handler, including 5.378 ms (58.9% of total) in
card construction/configuration/settlement, 0.618 ms in resident layout, and
0.265 ms in release. It constructs 3.26 cards per event. Large seeks construct
29.5 cards and spend 49.594 of 60.068 ms in materialization. By contrast,
60 px samples with no construction have handler mean/median/p95/max
0.466/0.562/0.650/0.730 ms; one construction raises the mean handler to
2.538 ms. Experiments deleting the viewport invalidation or substituting Qt's
native child scroll produced no material gain, so neither is an accepted fix.

The coherent replacement reuses and renames the existing one-card-per-1-ms
structural staging scheduler as the sole card-admission scheduler. Every row
intersecting the viewport still materializes synchronously through the one
`ConversationCard`; the existing one-viewport overscan remains the release and
residency bound. Missing hidden overscan is admitted nearest-first, at most one
card per existing callback, with structural snapshot staging taking priority.
Each callback derives work from the current height/model window, so there is no
row queue to become stale across scrolling, resizing, structure, or thread
switches. Focus pins, retained semantic interaction state, accessibility,
follow-tail, and renderer identity remain unchanged.

Delete in the same stage: synchronous convergence of the complete three-
viewport window, the “converge the whole window before release” policy, the
structural-only scheduler naming/guard, and tests equating one input event with
complete hidden-overscan residency. Add no timer, flag, cache, queue, callback
type, renderer, or compatibility path. Immediate tests require exact visible
coverage and zero synchronous hidden-overscan construction in a warmed 60 px
wheel sequence; eventual tests require complete bounded overscan without
anchor/pixel/focus/accessibility movement. The quantitative target is warmed
wheel-handler p95 <=2 ms and max <=5 ms, one card at most per admission
callback, and no regression in the 12 row/DPR profiles. Production remains a
hard **neutral-or-negative** gate; test forecast remains **+40..+100 CLOC**.

**M-35 implementation status: complete.** The existing structural one-card
scheduler is now the sole card-admission scheduler. Rows intersecting the
viewport still converge synchronously. Hidden overscan is admitted nearest
first, exactly one `materializeRow` call per generic callback; structural
staging retains priority. The callback does not repeat full visible
convergence. It restores the current anchor, releases from the same residency
window, and reschedules from current model/geometry state, so it stores no row
identity that can go stale. Scheduling the next callback before the existing
publication hook also preserves the view-lifetime guard when that hook deletes
the view.

The superseded mechanism was deleted in the same stage: complete synchronous
three-viewport convergence on wheel input, the structural-only scheduler
naming, and `pendingStructuralCardIndex_`. Reverse-filled structural work now
uses the vector's existing order instead of a second progress cursor. No timer,
cache, row queue, callback type, renderer, compatibility path, or persistent
functional flag was added. The only new QObject property,
`conversationCardAdmissionPasses`, is test-only diagnostic observation and is
never read by production behavior. The test-side event probe observes each
synchronous property change after the sole materialization call and gates at
most one construction per callback.

The quantitative benchmark sends 120 real progressive `QWheelEvent`s through
the production forwarding path. Every profile records at least 80 distinct
scrollbar values (120 observed), exact immediate and post-admission viewport
coverage, zero synchronous construction, at most one immediate full scan, and
construction count equal to admission-pass count. The historical normal full-
scan ceiling remains <=4; hidden admission has an independent <=4 ceiling and
the exact one-card bound. Normal constructions/releases tighten from 24 to 8.
Seek retains its <=8 combined-work ceiling while constructions/releases tighten
from 64 to 32. Across the Debug profiles, progressive wheel-handler p95 is
approximately 0.69--0.82 ms and maximum approximately 0.73--0.88 ms, compared
with the reproduced 10,000-row pre-fix normal-scroll p95/max of
11.319/11.666 ms. These numbers time synchronous input handling; asynchronous
admission remains separately and structurally gated rather than being hidden
inside the latency number.

Exact-current verification is:

- Debug registered virtualization plus 12 performance profiles: **13/13** in
  **114.13 s**; exact rebuilt focused correctness tests: **4/4** in **19.58 s**.
- Release registered virtualization plus 12 performance profiles: **13/13** in
  **58.63 s**; exact rebuilt focused correctness tests: **4/4** in **14.83 s**.
- ASan+UBSan with leak detection and halt-on-error: registered virtualization
  plus 12 performance profiles **13/13** in **267.08 s**; exact rebuilt focused
  correctness tests **4/4** in **31.03 s**.
- Ten repeated virtualization runs pass after the callback-lifetime correction.
  `clang-format --dry-run --Werror` on all four affected C++ files and `git diff
  --check` both pass.

The exact commands use `cmake --build . --target
codexui-nodegraph-conversation-ui-test codexui-conversation-cards-test
codexui-conversation-item-model-test codexui-conversation-virtualization-test`,
then `QT_QPA_PLATFORM=offscreen ctest --output-on-failure -R
'^(codexui-nodegraph-conversation-ui|codexui-conversation-cards|codexui-conversation-item-model|codexui-conversation-virtualization)$'`;
the sanitizer run additionally sets
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:quarantine_size_mb=64` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The 12-profile runs execute
the registered `codexui-conversation-performance-*` cases in each build tree.

Physical tracked-plus-untracked accounting is **36,207 production CLOC**,
exactly neutral against the M-34 baseline, and **40,919 test CLOC**: +419 from
M-34 and +380 from the recorded post-failing-first M-35 baseline. The +40..+100
test forecast was therefore disproven. The extra coverage is deliberate and
reported, not production hidden in tests: it includes real progressive wheel
timing, immediate/far-seek coverage, loading and thread-switch preemption,
hidden accessibility contracts, callback-destruction regression coverage,
exact one-card admission observation, deterministic residency, and all 12
row/DPR profiles. The production hard gate is met.

A previously built full Debug suite also ran 39/39, but it is not claimed as an
exact-current broad source rebuild: the deliberately paused H-11 worktree has a
new `ThreadPane.h` and old `ThreadPane.cpp`, so H-11-dependent targets cannot be
rebuilt until that coherent replacement resumes. This does not affect the
exact rebuilt M-35 targets above. Real AT-SPI event-stream qualification and
native-platform breadth remain S8 work; the M-35 tests prove accessibility
tree/state stability without claiming those platform results.

**M-36 — malformed command-execution card geometry.** The user-supplied live
screenshot `Screenshot_20260916_014544.png` shows a command-execution card with
large, unintended blank vertical bands before the header and between the header
and command field. Adjacent command cards use the expected compact layout, so
this is not a global spacing preference. A second live screenshot,
`Screenshot_20260916_014741.png`, shows the complementary malformed state: a
multiline command editor is internally scrollable/clipped while a one-line
output is compressed into a shallow dark strip, despite sufficient overall
card space. These are two manifestations of the same command-card sizing and
content-allocation authority, not two independent product concepts. The
screenshots and read-only live geometry inspection prove the first faulty owner.
The uncommitted card-local `settleHeightForWidth` exports/fixes a height before
the inner content layout and command/output viewports have their final widths;
child `resizeEvent` handlers then revise preferred heights after
`ConversationView` has cached the stale scalar. `createComposition` pre-resizes
both text views to their maxima, amplifying the same race in opposite directions.
In `014544`, the approximately 342 px card contains exactly the 220 px output
maximum although six output lines need about 93 px; the resulting 127 px surplus
becomes the blank bands. `014741` freezes its wrapped command too short and gains
an internal scrollbar. The committed predecessor explicitly settled inner then
outer layout; its old posted-event removal workaround is not restored.
M-36 is related to M-35 because excess layout and height reconciliation may add
scroll work, but neither finding is treated as the other's cause without a
profile.

The violated invariants are I-1/I-4/I-5/I-8: `ConversationCard` must be the one
visual and geometry authority for command content, and materialization or a
semantic no-op must not introduce extra space or change pixels. The repair must
delete or consolidate the competing height/stretch/reconciliation rule; no
offset, spacer exception, delayed resize, forced fixed height, or card-kind
workaround is authorized. Verification requires compact and multiline commands,
empty/short/long/live output, disclosure/folding, follow-tail and detached
output, eviction/rematerialization, history insertion, width changes, thread
round trips, and DPR 1.0/1.25/1.5/2.0. It must assert measured row height equals
the sole card size hint/geometry within the established rounding contract and
that a semantic no-op leaves geometry and pixels unchanged. Production is
forecast **neutral to negative** and tests **+30..+80 CLOC**; positive
production growth requires approval.

**M-36 implementation status: complete.** `ConversationCard` now performs the
only command-card geometry transaction. It assigns each logically presented
command, output, or file-change editor its final content width, measures that
editor at `maximumViewportSize()` (the stable scrollbar-free viewport), settles
the inner layout, activates the outer layout so Qt polish/frame geometry is
current, and then queries and exports one fixed outer height. Collapsed bodies
are not settled. `ConversationView` consumes only that scalar and remains the
residency/index owner, not an appearance authority.

The superseded mechanisms were deleted in the same stage: command/output
construction at their maximum heights, all three text-view resize-event
remeasurement paths, and their late ancestor `updateGeometry()` propagation.
No timer, delayed callback, retry, offset, cache, state variable, functional
flag, card-kind workaround, second renderer, or posted-event removal was added.
The existing command-output follow-tail callback is unchanged and is not part
of geometry authority.

The hidden-width contradiction is resolved by local runtime evidence. Before a
hidden `QAbstractScrollArea` is shown, its live `viewport()->width()` is stale;
under the exact command/output styles, `maximumViewportSize().width()` equals
the eventual no-scroll viewport and accounts for the frame and viewport
margins. Below the cap this is the realized no-scroll fixed point. At the cap,
a scrollbar can only narrow wrapping while the exported scalar remains clamped,
so it cannot create a second height outcome. The screenshot-shaped wrapped,
short, and six-line fixtures pass at DPR 1.0, 1.25, 1.5, and 2.0.

Failing-first coverage reproduced both defects: the initial expanded fixture
failed compact-height settlement, and an identical snapshot moved geometry and
pixels. The first full matrix then caught a related first-settlement ordering
defect in a collapsed agent card: a fresh staging card exported 44 px before
Qt polish established its 1 px frame, while the retained card correctly became
46 px. The test was not weakened. Activating the outer layout before the one
authoritative natural-height query makes the first settlement idempotent and
deletes one natural-height query relative to M-35's two-query settlement.

Exact-current verification is:

- `cmake --build . --target codexui-conversation-cards-test
  codexui-conversation-virtualization-test
  codexui-conversation-view-benchmark -j2` succeeds in Debug, Release, and
  ASan/UBSan build trees.
- `QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=<1|1.25|1.5|2>
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough CODEXUI_SETTLEMENT_TESTS=1
  ./codexui-conversation-cards-test` passes **4/4**.
- The registered card, virtualization, and 12 quantitative performance cases
  pass **14/14** in Debug (**121.00 s**), Release (**66.19 s**), and ASan/UBSan
  with leak/error halting (**269.70 s**).
- All 12 Debug profiles report zero command-card reconstruction, 24 document
  changes, zero command layout requests, at most 16 command paints, at most 31
  resident cards/29 documents/479 widgets, exact visible residency, zero
  immediate wheel construction, and at most one card per admission callback.
  Command-stream median/p95/max at 10,000 rows/DPR 1 changed from the recorded
  M-35 0.790/0.813/0.948 ms to 0.804/0.825/1.231 ms; the 14/12 microsecond
  median/p95 difference is immaterial and the noisy maximum remains far below
  the unchanged 10/30/50 ms gate. Across all profiles progressive-wheel p95 is
  at most 0.775 ms and interactive-resize p95/max at most 24.503/25.351 ms.
- `clang-format --dry-run --Werror` on the four affected C++ files and `git diff
  --check` pass. Independent read-only source/history review accepts the single
  synchronous authority and the viewport-width fixed point.

Physical tracked-plus-untracked accounting is **36,203 native production
CLOC**, down **4** from M-35, and **40,988 native test CLOC**, up **69**. The
production gate and test forecast are met. Real AT-SPI event-stream and native
XCB/Wayland breadth remain S8 qualification; they are not hidden M-36 work.

**M-37 — a delayed paginated snapshot can retire newer authoritative items.**
This is a source-derived risk, not the reproduced M-34 cause: no overlapping
`thread/turns/list` request occurred near the live report. The correlated
response retains its request revision, but `ProtocolUpdater` calls
`ingestTurn(..., replaceItems=true)` without a preservation boundary; generic
child replacement can therefore delete a fully authoritative post-request
`item/completed` `UserMessage` omitted from the older page. Existing tests
correctly require an authoritative snapshot to remove pre-request stale
membership and post-request untyped delta-only placeholders, so blanket
changed-revision preservation is disproven. The violated invariants are
I-2/I-6/I-7. First add a request -> later authoritative completed item -> delayed
page regression, then reshape the existing snapshot-boundary/fidelity rule so
only newer complete authority survives; do not add a retry, tombstone cache, or
second membership owner. Production must remain neutral/lower; tests forecast
**+40..+90 CLOC**. This forecast was disproven by the complete classifier and
mutation-authority matrix described below.

**M-37 implementation and qualification status: complete.** Existing correlated
request revision is now the sole temporal boundary for both
`thread/turns/list` and `thread/read`. An omitted post-request Item survives
replacement only when it has a nonempty exact-string `type`, a valid integer
`startedAtMs` or `completedAtMs`, and that lifecycle field changed after the
request boundary. An entirely omitted Turn survives only when one of its
direct Items meets the same rule. Generic changed revision, status-only,
delta-only, malformed timestamp/type, and recursive-subtree preservation are
rejected. Rollback and revert deliberately provide no boundary and therefore
remain authoritative deletion. The explicitly named lost-`You` shape exists
only as a completed-`userMessage` regression fixture; production contains no
`You`-card special case.

No timer, retry, cache, tombstone, persistent flag, renderer, or second
membership authority was added. Native production remains **36,203 CLOC**,
exactly neutral to M-36. Native tests are **41,292 CLOC**, **+304**; the
forecast was disproven by the required malformed/status/delta classifier,
included-Item merge, whole-Turn/root/count/order, rollback/revert, and
unrelated-graph scaling cases.

Exact-current `codexui-protocol-updater-test` passes in Debug, Release,
ASan/UBSan, and TSan. Debug also passes the eight adjacent graph suites, Release
passes the same matrix, and the two sanitizer cases that exceeded their
10-second CTest wrapper timeout pass directly with the required 120-second
bound and no diagnostic. The separate 5,000-file-change expansion gate failure
is M-40 below: history localizes it to committed HEAD `629660e`, and M-37
cannot execute that UI path.

Newly discovered adjacent findings are recorded without displacing the
accepted dependency order:

- **M-38 — competing thread snapshots (deferred until after H-11).** Reload and
  Load More use method-specific admission checks, so `thread/read` and
  `thread/turns/list` can overlap for one Thread. This is a verified
  current-worktree source defect, not an M-37 regression. The accepted
  reduction is one graph-derived pending predicate covering both snapshot
  methods and deletion of duplicate hydration admission setup; no new state,
  timer, or retry is authorized.
- **M-39 — dropped `thread/items/list` boundary (deferred until after H-11).**
  The reducer computes the correlated request boundary but does not pass it to
  `ingestItem`, so a delayed page can overwrite newer fields/status while
  retaining a newer completion timestamp. History traces the omission to the
  earlier paging/boundary work; the installed app-server does not currently
  expose this endpoint, so it is a verified latent reducer defect rather than
  a reproduced live-UI cause. The accepted correction reuses the existing
  boundary argument and adds no mechanism.
- **M-40 — large file-change expansion (deferred until after H-11).** The
  existing `<100 ms` 5,000-file gate now measures approximately 117--211 ms,
  versus 52--60 ms before committed HEAD `629660e`. The committed correctness
  fix introduced a full block/layout scan. Restoring
  `QPlainTextDocumentLayout::documentSize().height()` is disproven because it
  reports visual-line count rather than pixel height. No M-40 production or
  test edit has been made.
- **M-41 — empty `turn/started` provenance (unresolved and deferred).** A valid
  post-request `turn/started` normally has no Items, so M-37 cannot distinguish
  that omitted Turn from generic snapshot/fallback state. Broad Turn
  changed/status preservation would revive rejected placeholder behavior. No
  lifecycle-provenance state is authorized; any unavoidable production growth
  requires a separately quantified approval.
- **M-42 — command-output follow-tail regression (reported; reproduction
  pending, deferred until after H-11).** The current running product no longer
  follows appended command output specifically for retained cards restored
  while a thread is loading; newly created command cards still follow. The
  affected path is therefore narrowed to retained command viewport-state
  restoration through the outer conversation pause/follow policy rather than
  generic streaming append; current-worktree source and history still require
  tracing before assigning a cause. The violated invariant is one
  explicit follow-tail versus user-detached state with no unrelated scroll
  movement. The accepted investigation must distinguish a real user detachment
  from append-time geometry/scroll drift and then correct the existing owner;
  no retry timer, extra follow flag, forced-scroll callback, or reconciliation
  cache is authorized. Verification must cover streaming append, manual
  detachment, reattachment, folding, resize/DPR reflow, thread switching, and
  bounded per-frame work. Expected repair time is approximately 30--75 minutes;
  production/test LOC effects remain unclassified until reproduction.
- **M-43 — controller/observer conversation-card order divergence (reported;
  reproduction pending, deferred until after H-11).** Live screenshots
  `/path/to/workspace/Screenshots/Screenshot_20260916_143549.png` (controller) and
  `/path/to/workspace/Screenshots/Screenshot_20260916_143648.png` (observer) show the
  same `Big architectural refactoring` conversation segment in different card
  orders: the controller places the `You` steering card before `File changes`,
  while the observer places `File changes` before that `You` card. The captures
  are 61 seconds apart, so they verify visible divergence but do not yet
  distinguish stale observer state from deterministic replay/pagination order.
  The violated invariant is one authoritative item identity/lifecycle/order
  contract across controller and observer projections. Investigation must
  correlate exact Item/Turn identities, revisions, relation order, live-delta
  admission, initial load, pagination, and reconnect before assigning cause.
  The accepted remediation must correct the existing graph/projection ordering
  authority; no frontend-only sort, timestamp tie-break patch, delayed refresh,
  or reconciliation cache is authorized. Required regression coverage includes
  controller versus observer live delivery, retained thread load, pagination,
  reconnect, and stable order under equal/missing timestamps. Production/test
  LOC effects and time remain unclassified until reproduction. No M-43 code or
  test work has started. A later live report adds that a newly submitted
  steering `You` card appears in an observer only after a substantial delay,
  whereas the previous implementation projected it immediately. This is linked
  to M-43 until evidence distinguishes order/merge staleness from independent
  transport latency; it does not create a second task or authorize a refresh
  timer. A later controller-side observation strengthens the projection
  diagnosis: a newly completed Codex answer appeared and then vanished from
  the visible conversation while remaining present in the backend conversation
  state supplied to the agent. Until identity/order tracing proves otherwise,
  this is additional M-43 live projection evidence rather than a new task or
  estimate increase.
- **M-44 — mouse selection painted as a keyboard-focus underline (verified and
  completed in MR-7).** Clicking an arbitrary conversation card in the
  current worktree can add a thin pale-blue line at its bottom edge. Source and
  a live-style raster probe identify the exact cause: `ConversationCard`
  converts semantic selection into a forced `PE_FrameFocusRect` with
  `State_KeyboardFocusChange | State_HasFocus`; Breeze renders that primitive
  as the reported one-pixel bottom underline. This is not clipping, QSS, or
  geometry drift, and committed HEAD did not contain this current-worktree
  selection paint. The finding reopens the visual portion of H-01/M-30/X-07
  without reopening the single-renderer architecture. The accepted reduction
  keeps the item selection model as the semantic/accessibility authority and
  deletes selected-as-keyboard-focus painting; visible row focus is shown only
  for genuine keyboard focus/navigation using existing focus state. No overlay,
  timer, dynamic property, renderer, or selection cache is authorized.
  Verification must prove mouse selection changes semantic/accessibility state
  without changing card pixels or geometry, keyboard navigation retains visible
  focus, child-control focus is not row focus, and Breeze/Fusion plus the four
  DPRs agree. MR-7 implements and verifies this reduction below.
- **S1-D10 regression — command-output wheel-gesture ownership (historical
  reopen, completed in MR-6 after H-11).** The previous implementation
  had the reported correct behavior. History identifies `a8c82e4` as the
  original boundary contract, `8ba61f7` as its preservation after output-view
  extraction, and committed `629660e` as the last known-good source. The
  current uncommitted consolidation deleted both editor-local gesture state
  machines but replaced them with a per-event phase/target decision in
  `MiddleRegionWidget::routeScrollEvent`; crossing into command output can now
  steal an outer-started gesture, and a phased inner-start at its directional
  boundary is incorrectly retained by the inner view. The accepted repair does
  not restore the duplicated editor flags: `MiddleRegionWidget` remains the
  sole router and selects one semantic owner from the gesture origin plus first
  nonzero direction, retains it through zero updates, reversal, target crossing,
  reached boundaries, and `ScrollEnd`, while `NoScrollPhase` notches remain
  standalone. Coverage must use real materialized command text/output plus
  composer and attachment surfaces, pixel and angle deltas, both boundaries,
  crossing in both ownership directions, exact event metadata, no recursion,
  and unchanged follow-tail/manual-detachment behavior. MR-6 implemented and
  verified that repair without restoring either child state machine.

This was the authoritative order before the superseding re-audit below:
M-34, M-35, M-36, M-37, H-11, then reopened S1-D10, M-42, M-43, M-44, and the
explicitly deferred M-38/M-39/M-40/M-41 findings. The re-audit reordered and
completed MR-1 through MR-7; MR-8 is now active.
Discovery does not silently promote a finding ahead of an accepted active
stage.
