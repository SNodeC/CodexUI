# Qt architecture remediation findings ledger

## Current verified repair plan — 2026-09-20

This checkpoint implements the replacement plan accepted after the review of
`6a71db068b9a593b24c9ae638fbec9016e76376b`. The older entries below remain
historical evidence, not additional authorization to expand this plan.
The implementation baseline is a clean `master`, equal to `origin/master`.

| Step | Accepted correction | Status |
|---|---|---|
| 1 / NEW-1 | UTF-8-safe byte bounding at the generic-activity adapter boundary | Completed; broader performance qualification remains open below |
| 2 / D-6 | Copy removes generated placeholders only, preserving authored U+200B | Awaiting scope approval for the reproduced append-boundary dependency below; origin-aware design and growth estimate remain provisional; no production edits yet |
| 3 / M-4, D-4a | Model-backed accessible conversation identities and plan statuses | Pending |
| 4 / NEW-2, D-4b, D-3, WEB-1, T-1–T-3 | Shared native/browser detail, Copy, label and Markdown contracts | Pending |
| 5 / L-1 | Allocation-free scalar height updates and justified exception specifications | Pending |
| Cleanup | NEW-6–NEW-8, L-4–L-6, L-8: bounded test/documentation/code cleanup | Pending |
| Qualification | CI-1–CI-4, T-5/NEW-5, SHELL-1: coverage and measured performance | Pending; native-platform checks remain environment-constrained |

Candidate findings remain candidates: no speculative renderer, virtualization,
treap, lifetime, scheduler or graph rewrite is authorized. In particular H-2
has no reproduced section-range mismatch (800 comparisons passed); the broad
D-9/T-4 atomicity allegation is contradicted by prevalidation and passing
tests; M-5 revisits missing visible rows; NEW-5 did not increase actual shell
minimum width in the styled probe. D-5 stays withdrawn. Keep existing ownership
guards, useful diagnostics, incremental update paths and visual behavior.

### Step 1 change gate — NEW-1

- **Invariant:** bounded projected UTF-8 must remain valid for every native
  consumer; enforce it once at the graph-to-presentation boundary.
- **Path:** protocol strings in `NodeState::fields` → `graphDisplayDetail` /
  `GraphDetailBuilder` → `GenericActivityData::displayDetail`, through both
  snapshot and delta projections → visual detail, Copy and accessible text.
- **Replacement:** remove unchecked prefix slicing in the existing builder;
  retain the 4,000-byte body bound and existing truncation notice, cutting at a
  complete UTF-8 boundary. Rename the limit to bytes. No new helper, retained
  state, renderer, timer, cache or downstream correction.
- **Other policies:** protocol stream-tail retention keeps a suffix rather
  than this prefix; it is not a duplicate detail serializer. Native/browser
  serialization alignment and downstream bound consolidation stay in step 4.
- **Accounting baseline:** adapter production 2,531 CLOC / 2,681 physical
  lines; adapter test 3,084 CLOC / 3,223 physical lines. Expected production
  reduction; one public-boundary test exercises snapshots and deltas.
- **Before-change verification:** existing adapter suite passed three times
  (3.77 / 3.55 / 3.60 seconds). New regression cases fail on the unchanged
  production implementation at all six partial 2/3/4-byte boundaries, in both
  snapshot and delta projections (12 failures); later existing cases execute.
- **Commands:** build with `cmake --build /tmp/codexui-current-debug.fMMvCu
  --target codexui-nodegraph-ui-adapter-test -j14`; execute through
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu -R '^codexui-nodegraph-ui-adapter$' -j14
  --output-on-failure`, with `--repeat until-fail:3` for the baseline.
- **Environment:** the offscreen child executes, but the Xvfb wrapper returns
  1 in this sandbox. This is not native-display qualification. The adapter
  correctness test itself does not construct widgets or require a display.

### Step 1 result

- Replaced the existing builder's prefix slice and renamed its byte limit.
  For valid UTF-8, the boundary check inspects at most three preceding bytes;
  it does not decode or scan the whole payload. Existing state and the
  truncation notice are unchanged. No new production helper or mechanism.
- The new regression case checks 20 inputs through both snapshot and delta
  projection (40 exact-output checks): empty/short text, ASCII and 2/3/4-byte
  characters before, across and after the byte boundary. All pass after the
  change; all 12 partial-codepoint outputs failed before it.
- Adapter repeat: 3.59 / 3.49 / 3.49 seconds after, versus
  3.77 / 3.55 / 3.60 seconds before. This is a whole-suite timing comparison,
  not a claim that the one-byte-boundary correction sped up the UI.
- Built `codex-ui`, adapter, cards and virtualization targets with `-j14`.
  CTest selection `^(codexui-nodegraph-ui-adapter|codexui-conversation-cards|
  codexui-conversation-virtualization)$` executes all three: adapter passes
  (3.95 s), cards passes (11.70 s), virtualization fails its unrelated
  `collapsedLargeCardsSkipBodyProjection` timing gate (140,891 us > 100,000 us;
  one rebuild and 5,000 blocks remain correct). A separate repeat fails at
  148,510 us. The virtualization executable was not relinked by this change
  (mtime 12:19:02); it does not contain the adapter fix. No test threshold or
  unrelated production code was changed. Keep this observed timing failure
  in the existing performance-qualification work, not as proof of regression
  from NEW-1 or as an additional authorized refactor.
- Final touched-file accounting: production 2,530 CLOC / 2,680 physical lines
  (**-1 / -1**); tests 3,142 CLOC / 3,282 physical lines (**+58 / +59**).
  Documentation is accounted separately. `git diff --check` passes.
- Step 1 is complete for its adapter invariant. Native-display qualification,
  the unrelated performance failure, and the different browser serialization
  contract are not claimed fixed by this stage.

### Step 1 review follow-up

- Added exact-capacity cases for ASCII and 2/3/4-byte characters: the complete
  formatted line fits without a truncation notice, or the value fills the
  capacity and the following formatter newline triggers truncation. Failures
  now identify the character width, boundary case and snapshot/delta path.
- The regression case now exercises 28 inputs / 56 exact-output checks. The
  same adapter build and CTest commands above pass (3.89 s, child result
  `100% tests passed`). The Xvfb wrapper still returns 1; no native-display
  coverage is claimed. No production code changed in this follow-up.
- Current total delta against the clean baseline: production **-1 CLOC / -1
  physical line**; tests **+70 CLOC / +71 physical lines** (the review follow-up
  adds 12 test lines). `git diff --check` passes.

### Step 2 investigation and addition gate — D-6

- **Invariant:** presentation-only content must never leak into Copy, while
  authored Unicode, Markdown semantics, selection and blank-line geometry are
  preserved. The canonical source and the existing Markdown document remain
  the authorities; no additional renderer or interaction implementation.
- **Cause:** `userMessageMarkdown` inserts U+200B for empty prose rows and
  `MarkdownTextView::createMimeDataFromSelection` deletes every U+200B from
  exported formats, without distinguishing authorship. Plain and prepared
  Markdown views also inherit that deletion although they inserted no marker.
  Whole-card Copy already reads canonical source and is not the faulty path.
- **Rejected reductions, measured with Qt 6.10.2 offscreen:** backslash hard
  breaks preserve internal blank rows but leave a visible final backslash in
  trailing-empty-row cases. U+2028 produces one text block and changes the
  sample's height from 71 to 59; U+2029 keeps three blocks but changes code-span
  newline and escaping behavior. Empty-link markup gives three empty/ordinary
  blocks with no accessible/hittable anchors in prose, but becomes literal
  `[]()` inside code spans. These are counterexamples, not accepted fixes.
- **Other rejected direction:** parsing each prose paragraph independently to
  insert empty Qt blocks would multiply parser invocations with paragraph
  count. It is not accepted as an unmeasured replacement of the single parse.
- **Clipboard control:** a plain `QTextBrowser` preserves an authored U+200B
  in both plain-text and Markdown selection MIME. Thus the reproduced
  selection loss is not excused as an upstream Qt clipboard limitation.
- **Proposed next approach (unchanged in intent):** provenance-aware document
  preparation and selection export, replacing character-value-only cleanup
  and per-format reparsing. Keep one document, unchanged visible formatting,
  the existing incremental-update boundary and bounded work. Exact origin
  mapping must be demonstrated before accepting an implementation; none is
  claimed implemented by this investigation.
- **Addition gate:** the reductions tested do not satisfy all invariants.
  Estimate approximately **+40–80 net production CLOC** for origin-aware
  handling after deleting superseded cleanup. Request explicit approval
  before implementing that addition; the estimate is not completed accounting
  or authorization to exceed it. No production/test files changed for step 2.
- **Probe artifacts:** `/tmp/codexui-blank-lines-probe.cpp` and
  `/tmp/codexui-empty-block-probe.cpp`, built with
  `c++ -std=c++20 <source> -o <binary> $(pkg-config --cflags --libs Qt6Widgets)`
  and invoked through `xvfb-run -a env QT_QPA_PLATFORM=offscreen <binary>`.
  Xvfb startup remains unavailable in the sandbox; offscreen children run.

### Step 2 continuation — rejected replacement and newly proved dependency

- A `/tmp`-only probe now measures the previously unmeasured paragraph-import
  alternative. Importing each nonblank prose run and inserting real empty Qt
  blocks preserves ordinary internal blank rows, but changes leading-empty
  geometry (71 → 59 px), inline-code-adjacent geometry (68 → 65 px), and
  cross-line emphasis/code/backslash behavior. It is **not accepted**.
- Ten runs of the 1,000-repeat sample take 152,300 us with the existing
  import versus 430,881 us with paragraph imports; repeat 155,037 versus
  400,863 us. Parser calls increase from 10 to 20,000. Both use disabled
  document layout while importing. These are local construction measurements,
  not whole-UI frame-time claims; no production replacement was applied.
- The same probe, linked against the unchanged production conversation
  library, reproduces an **incremental-update boundary defect** through actual
  `MarkdownTextView::setContent`: start with `First\n\nThird`, then append
  ` appended`. The updated document contains five blocks
  (`First`, blank marker, `First`, blank marker, `Third appended`); a fresh
  view of the identical canonical source contains three. This is a controlled
  widget reproduction, not a claim of observing the user's live session or
  tracing a specific incoming protocol message.
- **Exact cause:** `markdownTailState` pairs Markdown source offset 0 with
  `document.lastBlock().position()` (8 in this sample). One normalized
  Markdown paragraph spans three Qt blocks. `appendMarkdownDocument` removes
  only the last Qt block, then inserts the entire source paragraph. The two
  coordinates therefore do not describe the same replacement boundary.
- **Proposed correction:** make the existing tail pair describe the same
  source/document range, preserving prefix identity and using the existing
  full-replacement path when an independent tail cannot be established.
  Verify incremental-versus-fresh content, formatting, geometry, selection
  and parse/layout work before accepting it; no new timer, reconciliation
  state or unconditional full-reparse workaround is proposed.
- This dependency was not a separately accepted implementation task. Per the
  user's instruction to ask before increasing the work, **request scope
  approval before fixing it**. D-6 remains incomplete; no safe origin mapping
  or final production-growth estimate is claimed. This entry records evidence,
  not authorization to expand the plan.
- Artifact: `/tmp/codexui-prose-blocks-probe.cpp`, linked to the current
  `libcodexui-conversation-ui.a`, height-index, nodegraph and UiStyle libraries;
  run as `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  /tmp/codexui-prose-blocks-probe`. Offscreen child completes; Xvfb wrapper
  returns 1 under the previously recorded sandbox restriction. The standalone
  probe compile emits a GCC 16/Qt-header SFINAE warning; the repository test
  build is clean. No tracked production/test edits belong to this probe.

## Historical remediation record

Date: 2026-09-12  
Repository: `/home/voc/projects/drafts/CodexUI/codexui`  
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
  `/home/voc/Screenshots/Screenshot_20260916_143549.png` (controller) and
  `/home/voc/Screenshots/Screenshot_20260916_143648.png` (observer) show the
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

### 2026-09-16 Middle Region adversarial re-audit and superseding plan

This section supersedes the ordering sentence immediately above. It reconciles
three independent read-only audits with an exact-current Release rebuild, the
current source, committed `629660e`, and the user's live observations. It does
not reopen the deleted passive renderer and does not authorize another renderer,
pool, timer, cache, reconciliation layer, or frontend-only ordering policy.

The current worktree has one visible/overscan renderer (`ConversationCard`) and
retains lazy QWidget/document residency. The renderer consolidation is sound.
At re-audit entry the remaining loss and latency defects were outside that
renderer: graph snapshot admission/replacement, controller-only graph order, a
second UI-owned logical history window, excessive hidden staging, incomplete
wheel routing, and selection/focus conflation. MR-1 through MR-6 have since
closed every item in that list except selection/focus conflation and the native
qualification assigned to MR-8.

Evidence classification and accepted remediation:

- **Loaded ordinary-card loss — verified current architecture defect.** The
  adapter selects the newest `AuthoritativeHistoryPageSize` ordinary Items plus
  landmarks; `ConversationView::HistoryWindow` independently expands or resets
  that local window; `ConversationItemModel::trimHistoryTo` physically removes
  already-loaded ordinary rows. A restart resets the process-local window to 80.
  This is distinct from widget virtualization and violates the required
  offscreen-model-plus-scalar-height invariant. Delete adapter suffix/landmark
  membership selection, view demand/effective history state, model trimming,
  retained-hidden counts, and their policy tests. Provider cursors/`hasMore`
  become the sole loaded-history boundary; all loaded graph Items remain model
  rows and only card/document residency stays bounded. Expected P −150..−350;
  replacement tests +100..+300 after deleting defect-preserving cases.
- **M-34 — complete for the exact reproduced missing-You defect, superseded in
  architecture by deletion of the wider UI history window.** Current code keeps
  every loaded `UserMessage`, so a current You/root disappearance is not caused
  by the local 80-row trim. Do not add another You special case.
- **M-37 — complete in its narrow evidence-backed scope.** Exact request
  revision plus typed Item lifecycle evidence protects post-request completed
  Items. It remains a source-risk repair rather than the reproduced M-34 cause.
- **M-38 — verified open defect.** Hydration/reload and Load More use
  method-specific admission authorities, allowing `thread/read` and
  `thread/turns/list` to overlap for one Thread. Use one graph-derived pending
  snapshot predicate covering read/turns-list (and items-list if enabled) at
  every admission and pending-UI boundary. Process-local sets may retain only
  callback/queue bookkeeping, never admission authority. Expected P −10..−35;
  T +100..+180.
- **M-39 — verified latent reducer defect.** `thread/items/list` computes a
  correlated request boundary but drops it at `ingestItem`. Pass the existing
  boundary; add no state. The installed runtime does not dispatch this endpoint,
  so it is not classified as the live loss cause. Expected P approximately 0;
  T +60..+120.
- **M-41 — verified open source risk, exact live chronology unresolved.** A
  post-request empty/current Turn can be removed by a delayed older snapshot
  because M-37 recognizes only a direct lifecycle-proven Item. First test the
  empty `turn/started` shape, then use existing canonical identity, status and
  revision evidence if sufficient. If the graph cannot distinguish provider
  lifecycle provenance without new state, stop for approval. Expected P 0..+20
  only if absorbed into the existing classifier; T +120..+220.
- **M-43 — verified controller/observer order defect.** Controller prompt
  correlation rewrites Turn children and snapshot replacement permanently
  reinjects the private submitted slot; observers retain provider order. Delete
  both order mutations and make provider child order the sole semantic order.
  Retain the local presentation key only as the explicit transient identity for
  in-place QObject/focus/selection promotion; it must not influence order or
  membership. Gate identical controller/observer content order over independent
  graphs. Observer display before a provider notification is impossible under
  the present bridge contract; if notification-to-paint remains unacceptable
  after staging reduction, a shared bridge admission event is new cross-repo
  architecture and requires explicit approval. Expected CodexUI P −50..−120;
  T +140..+260.
- **M-42 — verified source cause, executable regression still required.** A
  hidden staged command consumes its construction-time zero-delay tail
  settlement before final reparent/show. The replacement width authority omitted
  the old resize handler's immediate/deferred settlement, while a new direct card
  still has its queued callback. Make existing `CommandOutputView::settleWidth`
  settle the existing follow/detached owner and delete redundant constructor
  settlement. Add no timer/flag/cache/API. Expected P approximately 0; T
  +80..+110.
- **S1-D10/M-14 — reopened, verified incomplete, and completed in MR-6.** The central router recorded
  a native owner but consumes crossed events instead of delivering them to that
  owner; zero-delta/End outer events are also dropped. The passing test uses an
  orphan output and does not assert crossed-owner movement. Keep one router,
  delete child boundary arbitration, preserve exact wheel metadata and semantic
  owner through crossing/reversal/boundary/End, and test real materialized cards.
  Command output retains only follow/detach semantics and commits reattachment at
  the gesture boundary. Final effect P -22/T +260; the test forecast grew only
  because final adversarial audits added side-pane, semantic-no-op, fractional
  coordinate, and registered routed-performance gates.
- **M-44 — verified and completed in MR-7.** Card selection was painted as a forced
  keyboard focus primitive, and focus on any child performs `ClearAndSelect`.
  Delete card-local selected/focus state and child-focus-to-row-selection
  coupling. `QItemSelectionModel` remains semantic/accessibility authority;
  genuine keyboard current focus uses view-owned decoration. Final effect is P
  +3/T +145 under the user's explicit +5 production-CLOC approval; the test
  delta includes eight permanently registered style/DPR/accessibility gates.
- **M-35 qualification — implementation retained, completion narrowed.** The
  one-card-per-pass hidden-overscan admission works and must remain. All 12
  registered quantitative offscreen profiles pass. Exact-current 10k Release
  DPR1 measures normal p95 10.815 ms, seek p95 31.472 ms, and initial stage
  169 ms. The same binary on managed XCB measures 19.414 ms, 64.086 ms, and
  316 ms respectively and fails native resize/RSS gates. Therefore registration,
  execution and quantitative gating are complete, but representative native
  smoothness is open. Add a native input-to-painted-frame gate before changing
  scrolling. Do not restore the passive renderer.
- **Hidden structural staging — verified load-latency cause.** A minimum-height
  three-viewport budget constructs 43 cards before publication in the measured
  profile. Stage only the first complete visible frame around the saved anchor
  or following tail, publish atomically, then reuse the existing one-card
  overscan scheduler. Delete the three-viewport budget and synchronous-overscan
  commit special case. Expected P −10..−30; T +50..+110.
- **M-36 — current repair retained.** The two reported malformed-command strings
  pass exact width/height/no-op checks at DPR 1/1.25/1.5/2. The screenshots
  predate the current source. Native Breeze/Wayland confirmation remains a
  qualification item, not a second geometry implementation.
- **Static accessibility is substantially repaired; dynamic events remain open.**
  The materialized real cards form the accessible tree and hidden staging is
  excluded. M-21 still covers mutation/selection/busy/stream announcements;
  Disclosure must derive expanded state from the shared control, Copy needs an
  announcement, and semantic no-ops must emit none. M-16 rich clipboard MIME,
  H-14 reduced motion, focus modality, DPR-correct thumbnails, and M-13 token
  literals also remain open in their existing later stages.

Exact-current pre-edit baseline for this re-audit:

- focused Release correctness suites pass **9/9**:
  adapter, graph-conversation, runtime dispatch, cards, item model,
  virtualization, application layout, Shell integration, and protocol updater;
- registered Release conversation performance profiles pass **12/12** at
  rows 320/1,280/10,000 and DPR 1/1.25/1.5/2 under `offscreen`;
- the same native XCB binary exposes the performance failure above, so an
  offscreen pass is not accepted as proof of live smoothness;
- exact pre-MR-1 physical accounting is **36,486 native production CLOC** and
  **42,021 native test CLOC**. The 26/42-CLOC difference from the final H-11
  checkpoint is the already-present, not-yet-accepted S1-D10 implementation and
  test work; it is not attributed to H-11 or hidden in this cluster.

The one authoritative implementation order is now:

1. [x] **MR-1:** M-38/M-39/M-41 snapshot admission and temporal boundary.
2. [x] **MR-2:** M-43 provider-order authority; delete controller-only order
   mutation while preserving transient in-place visual identity.
3. [x] **MR-3:** delete the UI-owned loaded-history window; retain every loaded
   model row and bounded widget/document residency.
4. [x] **MR-4:** reduce hidden structural staging to first-visible-frame work.
5. [x] **MR-5:** M-42 final-geometry command-output follow/detach settlement.
6. [x] **MR-6:** S1-D10 central wheel ownership with real-card regressions.
7. [x] **MR-7:** M-44 semantic selection versus genuine keyboard focus.
8. [x] **MR-8:** native scroll/resize/DPR gates, followed only by measured
   deletion or neutral consolidation of duplicate movement/reflow work.
9. [x] **MR-9:** M-16/M-21/H-14/M-13/M-12 focus, accessibility, motion,
   clipboard, image-DPR, and token findings are implemented. MR-9F retains
   only the explicitly unavailable S8 native-platform/AT-SPI execution breadth.

Completed prerequisites remain explicitly complete: S1 single renderer and
passive-path deletion; S2 semantic fixtures; S3 projection/policy slices; S4
state/scheduler reductions; S5 H-09/M-28; M-32 through M-37 in their classified
scope; and H-11. This re-audit does not erase their evidence or mark the entire
Qt remediation complete.

**MR-1 implementation status: complete.** One graph-derived predicate now
gates `thread/read`, `thread/turns/list`, and `thread/items/list` snapshots for
the same Thread; process-local hydration sets retain callback/queue bookkeeping
only. `thread/items/list` passes its already-correlated request revision into
the existing ingest boundary. The same replacement classifier now recognizes a
canonical Turn whose non-unknown lifecycle status changed after that boundary;
it preserves a post-request empty `turn/started` while still retiring an
otherwise identical pre-request empty Turn. No retry, timer, cache, tombstone,
flag, provenance field, or second authority was added.

Failing-first tests reproduced both competing wire-request directions, the
dropped item-page boundary, and empty-Turn deletion. Exact-current Release then
passes the adapter, graph-conversation, current-protocol, runtime-dispatch,
Shell-integration, NodeGraph, protocol-updater, and WorkerLogic suites **8/8**.
MR-1 accounting is P **+16 CLOC** and T **+149 CLOC**, ending at **36,502 native
production CLOC** and **42,170 native test CLOC**. The production growth is
inside the existing approved ceiling and the complete Middle Region cluster
remains forecast materially negative; MR-2/MR-3 delete the order/window
mechanisms that dominate this cluster.

**MR-2 implementation status: complete.** Provider `children(Turn)` order is
now the sole row-order authority. `TurnRootItem` remains decoration authority:
the first authoritative User in final provider order owns the Turn surface,
rows before a non-first root are standalone, and only following rows are
nested/folded. A prior root is retained only when it is the still-present
explicit local optimistic prompt and no authoritative User exists. The full,
item-page, exact-adapter, model, and hidden-staging paths use this same rule.

This stage deletes submitted-slot reinjection, correlation-time provider row
reordering, root-first `std::rotate`, Worker-side authoritative item transfer,
the adapter's duplicate key override, root-first model validation, and the
implicit prompt-acknowledgement heuristic. Optimistic identity is not an order
authority: one explicit graph-derived `PromptMaterialization` transaction fact
lets a LocalPrompt become the provider User in place, preserving its QWidget,
document, selection, focus, persistent model index, geometry, and accessible
object. Replayed/no-op frames do not acknowledge again. No timer, retained
cache, compatibility renderer, persistent flag, or second state authority was
added; the only new DTO value is this per-transaction relation fact, and the
NodeGraph query uses its existing incoming-relation index.

Regression coverage now includes controller and observer graphs receiving the
same `item/started` notification through the real route -> exact delta -> view
path (without full replacement), provider-order snapshot convergence,
non-first-root exact insertion, root-relative folding, coalesced unseen local
promotion, exact and full replay, destructive callback ordering, and local-root
snapshot races. A non-first same-slot root replacement also preserves its
section-front role throughout both Qt insert/remove signal boundaries instead
of transiently claiming `FirstInTurn`. The full Release tree builds with
`cmake --build /tmp/codexui-release-gate.I6nD6Z -j14`. One non-short-circuit
`ctest ... -j14` invocation passes the 12 relevant correctness suites in
**9.76 s**; the 12 registered conversation profiles at 320/1,280/10,000 rows
and DPR 1/1.25/1.5/2.0 pass in **52.81 s**. `git diff --check` is clean.

MR-2 accounting is P **-58 CLOC** and T **+698 CLOC**, ending at **36,444
native production CLOC** and **42,868 native test CLOC**. An existing exact
root-role handoff between two retained authoritative User rows still rejects
atomically and invokes the established complete-authority projection; neighbor
validation prevents wrong order or partial mutation. This is not a correctness
fallback or a reproduced card-loss defect, but its possible full-projection
latency remains an explicit MR-4 performance qualification. Relation-only root
reinsertion/counting is likewise not accepted as membership policy and is
deleted with the duplicate native history window in MR-3.

**MR-3 implementation status: complete.** Every loaded graph Item now has one
canonical `ConversationItemModel` row. The model is no longer a second
pagination authority: provider cursors and `historyHasMore` decide whether an
older page exists, while virtualization alone bounds QWidget/document
residency, measurement, parsing, and frame work. `TurnRootItem` remains
decoration authority and cannot inject a row absent from provider child order.

This stage deletes `historyLoadedItemCount` maintenance, the adapter's suffix
budget/landmark and relation-only reinsertion paths, all authoritative/hidden
count DTO fields, model history classification/trimming, view-local history
demand and reconciliation, Shell local reveal paging, and their policy-only
tests. Missing rows or canonical neighbors now reject an exact transaction and
request the existing complete-authority projection; they are never accepted as
"off-window" no-ops. Provider request-pending state remains one typed boolean
per thread. No timer, cache, callback, compatibility path, renderer, or new
state authority was added.

Projection was reduced further while qualifying the complete model: the
adapter no longer buffers a second vector of every Turn child, no longer builds
up to 4 KiB of generic diagnostic text before replacing it for known card
types, and reads only normalized `historyHasMore` for pagination chrome. The
runtime continues to read the paired normalized `historyNextCursor` when it
dispatches `LoadHistory`.

Regression coverage now proves complete 100/160/10,001-row membership,
canonical order, strict exact-delta admission, provider-page prepend, exact
paused anchors, live-tail following, stable model/card identity, accessibility
content, stale-incarnation viewport ownership, and no model reset during
provider pagination. The Shell provider test retains all 100 initially loaded
rows, emits exactly one `LoadHistory`, adds a real older row as row 101, removes
the continuation control on the final page, and stays at no more than 48 live
cards.

Verification used the required parallelism:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14` passes for the complete
  Release tree;
- `ctest --test-dir /tmp/codexui-release-gate.I6nD6Z -LE performance -j14
  --output-on-failure` passes **26/26** in **9.91 s**;
- `ctest --test-dir /tmp/codexui-release-gate.I6nD6Z -L performance -j14
  --output-on-failure` passes all **28/28** registered quantitative profiles,
  including the 12 conversation profiles at 320/1,280/10,000 rows and DPR
  1/1.25/1.5/2, in **62.52 s**;
- the exact 10,000-row DPR-1 conversation profile passes with 31 peak resident
  cards, 29 peak documents, one normal admission pass, at most four combined
  normal passes, a 161 ms initial frame, and a 104,372 KiB sampled RSS peak;
- `git diff --check` passes, and a source/test search finds no remaining native
  reference to the deleted window/count contracts.

An additional unregistered 40,000-row stress run preserves the important
virtualization bounds (31 cards, 29 documents, at most four normal combined
passes, 289 ms initial frame) but fails the benchmark's fixed 64 MiB RSS-growth
profile at a 248,492 KiB sampled peak. Loaded value rows are intentionally
O(N); this is recorded as an MR-8 capacity/profile risk rather than restoring a
second logical window or weakening a registered gate.

MR-3 accounting is P **-961 CLOC** and T **-457 CLOC**, ending at **35,483
native production CLOC** and **42,411 native test CLOC**. This is genuine
deletion, not class extraction: the superseded state, calculations, branches,
and tests are absent from the tree.

**MR-4 implementation status: complete.** The restored invariant is that hidden
structural staging prepares only the canonical renderers required to publish one
complete visible frame; it is not another residency window. Publication
materializes visible rows only, after which the existing one-card admission
scheduler fills the ordinary overscan window. The same `ConversationCard`
objects remain the sole visible and overscan renderer throughout.

Deletion preceded extension: this stage removes the minimum-row floor, the
three-viewport hidden-stage budget, and the commit-only synchronous overscan
special case. The remaining bounded selector stages a following tail or the
saved paused-anchor frame. It uses the pending snapshot's current visibility,
folding, mode, width, and height policy and replans that same bounded work when
one of those inputs changes. Supersession and membership/mode changes destroy
obsolete hidden cards before restarting the one existing admission schedule;
unchanged and rejected commits explicitly resume deferred overscan admission.
No renderer, timer, cache, scheduler, persistent flag, or geometry authority
was added.

One Qt reentrancy defect was found while gating retained positive-offset
anchors: model insertion caused `currentChanged` to call
`scrollTo(EnsureVisible)` against half-applied geometry, evicting retained cards
before the authoritative commit. `scrollTo` now obeys the existing `applying_`
transaction boundary. Final anchor restoration and visible materialization
remain the only commit authority; retained paging consequently publishes with
zero synchronous card construction. If an inactive thread's saved semantic
anchor was actually removed, the old model cannot identify its future visible
rows. That rare case deliberately uses the canonical visible-only synchronous
fallback, bounded to one frame, then resumes one-card overscan admission. A
second pending-height index or speculative geometry cache was rejected as a
duplicate authority.

Executed regressions cover following and paused publication, positive-offset
Turn-child anchors, removed inactive anchors, current presentation-policy
changes, collapsed membership, resize and style reflow, thread switching,
supersession, unchanged/rejected snapshots, destructive callbacks, focus,
pixels, accessibility children, staging-host exclusion, and exact one-card
deferred admission. The benchmark now measures publication separately from
eventual overscan rather than conflating the two.

Verification used the required parallelism:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14` passes for the complete
  Release tree;
- `QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-release-gate.I6nD6Z --output-on-failure -LE performance -j14`
  passes **26/26** in **9.37 s**;
- the corresponding `-L performance -j14` run passes all **28/28** registered
  quantitative profiles in **61.17 s**, including conversation rows
  320/1,280/10,000 at DPR 1/1.25/1.5/2;
- the exact 10,000-row DPR-1 offscreen profile publishes in **83 ms** versus
  the MR-3 **161 ms** baseline, stages **16** cards versus **43**, publishes
  **10** resident/visible rows with **zero** synchronous constructions, then
  performs **9** deferred constructions in **9** one-card admission passes;
  settled peaks remain **31 cards, 29 documents, and 479 widgets**;
- the same Release binary on managed XCB (`DISPLAY=:191`, DPR 1) publishes in
  **108 ms** versus the pre-MR-4 **217 ms** baseline with the same 16/10/0/9
  stage/publication/deferred pattern. The executable still reports native
  interactive-resize-tail and RSS-profile failures; those measured platform
  gates remain explicitly open in MR-8 and are not presented as an MR-4 pass;
- `clang-format --dry-run --Werror` on the three changed implementation/test
  files and `git diff --check` both pass. A separate read-only adversarial audit
  found no remaining MR-4 architectural or correctness blocker.

Using the ledger's unchanged comparable command, `git ls-files <scope> | cloc
--list-file=-`, MR-4 accounts for P **+29 CLOC** and T **+379 CLOC**, ending at
**35,512 tracked native production CLOC** and **42,790 native test CLOC**. The
production delta misses the original -10..-30 forecast but is inside the
user-approved growth ceiling: the additional lines are the bounded pending
policy/replan correctness and the two-line transaction guard, not new state or
parallel architecture, and the audit found no safe deletion. The complete
MR-1..MR-4 cluster remains P **-974 CLOC** from its 36,486-CLOC entry. The
filesystem contains **35,874** native production CLOC when the pre-existing
untracked 362-CLOC `TurnSettingsPolicy` pair is included; it is excluded from
both sides of the tracked MR-4 delta and was not modified in this stage.

**MR-5 implementation status: complete.** The restored invariant is that
`CommandOutputView::State` remains the sole owner of inner follow/detach,
scroll value, and selection, and that the owner is settled against the card's
authoritative final width before the output is painted. `ConversationView`
continues to derive only the outer command-owned pause cause; it does not own
or copy the inner scroll position.

History and a local failing regression confirmed M-42. The former
`CommandOutputView::resizeEvent` measured output and performed immediate plus
Qt-next-phase scroll settlement. When card width ownership was consolidated in
`ConversationCard::settleHeightForWidth`, the replacement `settleWidth` kept
measurement but omitted scroll settlement. A newly created live card normally
still had its construction callback pending; a retained card built under the
hidden staging host consumed that callback before reparent/show, leaving
logical `followsLatest == true` while its physical scrollbar remained at zero.

The coherent replacement adds no mechanism: `settleWidth` now invokes the
existing immediate settlement and re-arms the existing QObject-owned,
coalesced zero-delay settlement required after Qt finalizes child layout. The
stage first deleted duplicate constructor measurement/settlement, the unused
constructor output parameter and its empty-output callback, the pre-width
environment settlement, and the identical-output settlement branch. A
semantic `setOutput` no-op is therefore a real no-op. Changed streaming output,
restored detached state, direct scrollbar interaction, and final-width layout
retain their distinct existing settlement paths. No timer, callback type,
flag, cache, state, property, counter, API surface, or parallel scroll owner was
added.

The new regression selects a staged thread containing a visible expanded
100-line command, places that command before a later admission so its original
construction callback expires while hidden, and proves publication adopts the
staged renderer with zero fallback construction. Reconciliation completion is
correctly treated as a model-authority boundary, not as a finished QWidget
layout phase. A test-only event filter samples the first actual command-output
viewport paint and requires `followsLatest`, a nonzero range, and
`value == maximum`; the same card and output remain at the tail afterward.
Existing executed tests retain exact detached values/reverse selections across
thread navigation, offscreen recycling, folding, resize/style reflow, output
removal/recreation, streaming, owner retirement, and multiple detached
commands. The capped-streaming test now reads the live
`conversationLocalGeometryPasses` counter instead of an unwritten legacy
property.

Verification used the required parallelism:

- the pre-fix focused run failed both first-tail assertions, locally
  reproducing the retained-card defect;
- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14` passes for the complete
  Release tree;
- `QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-release-gate.I6nD6Z --output-on-failure -LE performance -j14`
  passes **26/26** in **9.22 s**;
- the corresponding `-L performance -j14` run passes **28/28** in **62.12 s**,
  including all conversation/thread/inspector profiles at DPR 1/1.25/1.5/2;
- the exact 10,000-row DPR-1 conversation profile passes in **84 ms** initial
  publication with **16** staged, **zero** synchronous, and **9/9** deferred
  constructions/admissions. Command streaming has **zero** card construction,
  **zero** layout requests, and median/p95/maximum handler times of
  **673/735/1,052 microseconds**; peaks remain **31 cards, 29 documents, and
  479 widgets**;
- the managed-XCB virtualization run executes the new first-paint case without
  an MR-5 failure. Its full process still reports pre-existing native
  rejected-frame/residency assertions, which remain MR-8 qualification rather
  than being hidden as an MR-5 pass;
- `clang-format --dry-run --Werror` for all five touched production/test files
  and `git diff --check` pass. Two independent read-only final audits found no
  remaining MR-5 lifecycle, ownership, deletion, or correctness blocker.

MR-5 accounts for P **-6 CLOC** and T **+100 CLOC**, ending at **35,506 tracked
native production CLOC** and **42,890 native test CLOC**. The complete
MR-1..MR-5 cluster is now P **-980 CLOC** from its 36,486-CLOC entry. This is
actual mechanism deletion, not code movement: one final-width authority
replaces constructor, no-op, and pre-width settlement work.

**MR-6 implementation status: complete.** The restored invariant is that a
wheel/touchpad gesture has one semantic owner chosen from its origin and first
nonzero direction. That owner receives every later update, reversal, zero
delta, boundary arrival, pointer crossing, and `ScrollEnd`; a standalone
`NoScrollPhase` notch makes one fresh choice. Routing never changes command
follow/detach ownership and never substitutes manual scrollbar arithmetic for
the selected widget's native Qt behavior.

Three independent read-only audits reconciled the current source with
`a8c82e4`, `8ba61f7`, and committed `629660e`. The old behavior was correct but
implemented the same three-flag gesture state machine in both command-text and
command-output widgets. The current worktree correctly moved the choice into
`MiddleRegionWidget`, but its Native branch merely accepts crossed events
without delivering them to `scrollGestureArea_`; its Conversation branch does
not deliver zero updates or End. The existing test hides both failures: it
uses an orphan output directly under the outer viewport, asserts mostly that
the non-owner did not move, and its depth guard consumes the very forwarded
outer event that it purports to verify.

Deletion preceded replacement. MR-6 deleted `ContentSizedTextView`'s
directional wheel override; reduced `CommandOutputView`'s override to its sole
follow/detach responsibility; and deleted `ConversationView::forwardWheelEvent`,
its clone API, public dispatch accessor, and dispatch flag. `ConversationView`
retains only native item-view wheel dispatch plus a nonzero-vertical-intent
follow-mode update; zero lifecycle and horizontal events are semantic no-ops.
The one MiddleRegion router retains one route enum and weak native-owner
pointer, reduced to only the states needed after origin classification. One
scoped synchronous redispatch state replaces the deleted ConversationView
guard and prevents a chosen nested owner's boundary event from propagating
into the conversation. It adds no timer, cache, callback, per-card gesture
state, manual scroll calculation, or parallel router. If a weak native owner
is destroyed during a gesture, the router consumes through End instead of
silently transferring ownership.

Verification replaced rather than preserved the false-positive test. A real
expanded materialized command card demonstrates actual chosen-owner movement
for all three reported rules, command text and output, both directions/edges,
pointer crossing within and outside the center, reversal, zero updates,
missing Begin, standalone notches, pixel and angle deltas, fractional local
coordinates, exact event metadata, bounded redispatch, stable card identity,
and follow/detach commit only at a complete gesture. Composer editor and
attachment scroll ownership remain native. Failing-first runs reproduced the
crossed-owner, outside-End, semantic-no-op, and fractional-coordinate defects.

Final verification used `-j14`: the full Release build passes, nonperformance
tests pass **26/26**, and the registered performance matrix passes **29/29**.
The new serial `middle-region` gate reuses the application-layout target rather
than duplicating sources; on the final focused run direct native p95 was **6
us** and central routed p95/max was **5/8 us**, gated at **2/5 ms** and against
the paired native baseline. The exact 10,000-row DPR-1 profile passes in **86
ms**, with stage/synchronous/deferred admission **16/0/9**, peaks of **31
cards/29 documents/479 widgets**, viewport-wheel median/p95/max **209/313/422
us**, and command-stream **695/913/1,000 us**. ASan+UBSan passes both functional
and isolated routing gates. The warning build adds no warning in an MR-6 edit;
`git diff --check` and nine-file `clang-format --dry-run --Werror` pass. Managed
XCB executes the wheel case and routed metric without an MR-6 failure; its full
executable still fails the unrelated native prompt-activation harness assigned
to MR-8. Real touchpad input-to-painted-frame, Wayland/X11 breadth, and native
pixel-delta movement remain MR-8 qualifications, not hidden MR-6 claims.

The pre-edit anchor was P **35,506** / T **42,890**. MR-6 ends at P **35,484**
/ T **43,150**: P **-22**, T **+260**, and cumulative MR-1..MR-6 production
**-1,002 CLOC** from 36,486. Test growth exceeded the +80..+160 forecast because
the final adversarial audits required executable side-pane, no-op,
fractional-coordinate, and separately registered performance evidence; no
production mechanism was added to satisfy those gates.

**MR-7 implementation status: complete.** The restored invariant is that
semantic row selection, the item-view navigation cursor, child-control focus,
and visible keyboard focus remain distinct. `QItemSelectionModel` is the only
semantic/accessibility selection authority; `currentIndex()` remains the one
navigation cursor; actual QObject focus remains the child-focus authority. The
sole `ConversationCard` renderer emits focus pixels only when its owning view
is genuinely focused in Qt keyboard modality and its exact viewport geometry
matches the current row.

Deletion preceded replacement. MR-7 deleted `ConversationCard::Impl::selected`,
the public selected getter/setter, both current-or-selected mirrors, the
selection-driven relayout override, and forced focus-state painting. Child
focus no longer performs `ClearAndSelect`; it retains the historical navigation
cursor with `NoUpdate`, so Copy/Markdown/disclosure focus cannot change semantic
selection or make the row accessible/visually focused. The existing focus
callback now identifies only the actual departing/focused card and retains its
sole residency-release responsibility. No selection cache, card focus flag,
dynamic property, timer, overlay, renderer, or accessibility mirror was added.
Qt's existing window keyboard-modality attribute is set at the view's input
boundary so arrow navigation after a mouse click is visible without local
modality state.

Failing-first coverage reproduced the Breeze bottom underline and both
`ClearAndSelect` couplings. The final test sends mouse events to the actual
visible card, verifies semantic and accessible selected state, then proves
exact pixel, geometry, object, and accessible-ID stability for selection and a
repeated selection no-op. Keyboard movement alone adds focus pixels; focus loss
removes them; mouse refocus does not restore them. Real child-control focus
keeps semantic selection unchanged and remains the accessible focus child.
Eight registered gates execute this same invariant under Fusion and Breeze at
DPR **1.0, 1.25, 1.5, and 2.0**, with unsupported styles explicitly skipped
rather than silently substituted.

Final Release verification uses `-j14`: the complete non-performance suite
passes **34/34**; the registered MR-7 matrix passes **8/8**; and the final
conversation plus central-wheel performance set passes **13/13** after the
production reduction. The complete quantitative performance matrix also
passes **29/29**. The exact 10,000-row DPR-1 profile remains within the MR-6
baseline envelope: **82 ms** initial publication, stage/synchronous/deferred
admission **16/0/9**, peaks of **31 cards/29 documents/479 widgets**, viewport
wheel median/p95/max **196/286/342 us**, and command stream
**678/705/942 us**. The focused ASan+UBSan style/DPR matrix passes **8/8** with
no diagnostic. The warning build introduces no MR-7 warning;
`clang-format --dry-run --Werror` and `git diff --check` pass. Three read-only
post-edit audits found no architectural or lifetime blocker; the test audit's
card-target/accessibility gap was closed before completion. Native compositor
focus and external AT-SPI delivery remain MR-8/MR-9 qualification, not hidden
MR-7 claims.

The pre-edit anchor was P **35,484** / T **43,150**. MR-7 ends at P **35,487**
/ T **43,295**: P **+3**, T **+145**, plus **+23 CMake configuration CLOC** for
the durable eight-case matrix. The user explicitly approved up to +5 production
CLOC; the final result stays below that ceiling. Cumulative MR-1..MR-7
production is **-999 CLOC** from 36,486.

**MR-8 implementation status: complete.** The restored invariant is that each native
wheel or touchpad update reaches a painted frame within its display budget,
while visible coverage, one-renderer identity, anchors, focus, accessibility,
and the one-viewport overscan bound remain exact. Lazy residency remains
mandatory; MR-8 may reduce when work runs, but may not restore the passive
renderer, add a widget pool, skip visible coverage, or hide latency behind a
new throttle, cache, timer, or recent-input flag.

The reported live instances and the exact worktree must not be conflated.
`/proc/1976130/exe` and `/proc/1976277/exe` identify an older controller and
observer as the same deleted September 15 Debug inode, matching the pre-M-35
stash path that synchronously converged the full overscan window. However,
newer September 16 processes **2545937/2547420** have byte-identical
`scrollContentsBy`, `updateMaterialization`, overscan-selection, and admission
code to the current Debug build. The user's current-head report is therefore
verified independently of the stale pair; the stale binary is only an
amplifier and will not be used as a blanket explanation or restarted without
authorization.

The initial frame conclusion is **superseded by the completed-paint probe**.
Three isolated XCB 10,000-row/DPR-1 runs record the old `normalScroll` median
**12.502--13.017 ms** and p95 **19.555--20.820 ms**, but that metric includes
three event-loop settlement passes and is not one display frame. Exact
input-to-return-from-top-level-`UpdateRequest` measurements instead record
warm wheel p95 **2.474 ms**, warm scrollbar p95 **2.257 ms**, and
one-third-viewport scrollbar p95/max **13.834/16.042 ms**. The earlier blanket
claim that all 60 warm scrollbar samples were residency-neutral is superseded:
the recorded profile has 59 neutral samples and one bounded construction. The
final gate reports that distinction explicitly. Current-head
small/medium movement therefore meets a 16.667 ms QWidget backing-store
budget; compositor presentation is not claimed. Arbitrary large scrollbar
seeks remain reproduced at **58.089 ms p95** in the three-pass settlement
metric and synchronously construct up to 15 newly visible cards. Thus “no
incoming message” does not imply a warm path: scrolling itself changes
residency when it crosses the retained window. The two other XCB failures are
separately classified resize-tail and RSS profiles.

History and two independent read-only audits agree on the boundary and work
inventory. The uncommitted S1 sole-widget conversion correctly deleted the
parallel passive renderer, but restored rich QWidget movement/paint cost for
every visible and overscan row. M-35 removed synchronous hidden-overscan
construction from the wheel handler, while the existing one-card admission
callback still constructs, settles, lays out, and paints on the GUI thread
between inputs. A warmed scroll additionally performs an unchanged
range/anchor restoration, a full resident layout, a second full resident
release scan, repeated admission-window scans, and a full viewport update.
The router and height tree are ruled out as primary causes; prior experiments
also found no material gain from deleting viewport invalidation or substituting
Qt child scrolling.

Deletion/consolidation order before any production edit is:

1. Add a test-only native input-to-completed-paint metric that includes queued
   admission work and records constructions, layouts, paints, coverage, and
   pixel movement; retain the existing immediate-handler metric separately.
2. Delete the redundant outer admission precheck and repeated unchanged
   range/anchor restoration. A warm scroll must perform one geometry/visibility
   reconciliation; height-changing visible construction must still converge
   and restore the exact anchor once.
3. Consolidate layout and retirement traversal so hidden-stays-hidden overscan
   cards do not receive meaningless geometry writes, while cards entering or
   leaving the viewport and the one focus pin remain exact.
4. Re-measure before touching admission cadence. Only a remaining measured
   frame overrun can justify reshaping the existing callback; no second
   scheduler or input-state workaround is accepted.
5. Qualify live resize and DPR 1.0/1.25/1.5/2.0 under offscreen plus locally
   available XCB/Wayland, without weakening existing correctness or resource
   gates.

Native heap profiling adds one verified reduction to step 2. The 320-row
Breeze profile performs **12,933,418 allocations**, peaks at **83.18 MiB** of
tracked heap, and reports only **2.29 MiB** leaked; no ConversationCard or
materialization allocation is leaked. Breeze `QPropertyAnimation` accounts for
**21.37 MiB / 72,839 calls**, with **21.25 MiB / 71,648 calls** reached through
`materializeRow`. Ordinary residency currently constructs a card under the
hidden structural-staging host and immediately reparents it to the viewport;
the redundant `setParent` transition alone reaches 10,053 Breeze animation
allocations, about 992,000 widget allocations, and 229,623 stylesheet matches.
Fusion A/B reduces final RSS from **165,908 KiB** to **93,084 KiB** and normal
p95 from **17.402 ms** to **13.531 ms**, proving style transition churn rather
than an app-object leak or retained-history growth. MR-8 will delete the
ordinary staging-host parent and create normal admission cards directly under
their final viewport owner; the staging host remains only for atomic
structural snapshots. Expected production effect was **-1..-3 CLOC**, test
effect **0 CLOC** beyond the already-added native frame/resource gate. It adds
no state, cache, pool, renderer, timer, callback, or authority.

Required regressions cover progressive angle and pixel deltas, rapid and slow
gestures, row-boundary crossings, large seeks, viewport coverage at first
paint, eventual exact overscan, stable card/document/focus/accessibility
identity, Turn surfaces, scrolling and history anchors, command-output
follow/detach, thread switches, resize reflow, and semantic no-ops. The exact
pre-edit accounting anchor is native production **35,487 CLOC** and native
tests **43,295 CLOC**. Production remains neutral-to-negative; the test-growth
forecast was **+50..+140 CLOC** for the then-missing frame/platform gate.

**MR-8 final result (2026-09-17).** Lazy materialization remains the sole
residency policy and `ConversationCard` remains the sole visible/overscan
renderer. Ordinary admissions now construct directly under their final
viewport owner; the structural staging host remains only for atomic snapshot
publication. The stage also deletes the duplicate outer admission preflight,
unchanged range/anchor restoration, redundant post-restore layouts,
hidden-stays-hidden geometry writes, duplicate scrollbar-triggered layout, and
hot-path reconstruction of already-known stable keys. A materialization
restores the viewport only when authoritative height actually changes. No
timer, throttle, cache, pool, flag, compatibility renderer, or second geometry
authority was added, and no visual policy changed.

The benchmark now drains Qt `DeferredDelete` events in each manual event-loop
cycle. This corrects a benchmark-only lifetime distortion: under Breeze the
10,000-row tracked-heap peak fell from **80.15 MiB** to **22.36 MiB** and the
reported retained allocation from **4.38 MiB** to **255 KiB** without a product
cache or allocator workaround. Seven repeated Breeze 10,000-row native runs
pass the unchanged DPR-1 RSS limit; the worst observed growth is **64,184 KiB**
against **65,536 KiB**.

Exact-current Release and Debug correctness matrices pass **10/10** each, and
their registered quantitative performance matrices pass **12/12** each. The
native XCB matrix passes **8/8** at Fusion and Breeze across DPR
**1.0/1.25/1.5/2.0**. All four scroll profiles at every matrix point pass exact
first-frame pixel movement plus bounded layout, move, resize, paint,
construction, admission, and residency work. At DPR 1, Fusion records warm
wheel/direct-scrollbar/one-third-step p95 values of
**2.360/2.484/8.709 ms** and Breeze records **2.210/2.157/10.993 ms**;
large seek remains separately visible at **24.074 ms** and **29.998 ms**.

These native figures measure completion of the top-level QWidget backing-store
update, not compositor presentation. The local session was locked and both
screen-saver APIs reported active; its Wayland compositor accepted the
backing-store update but withheld its frame callback, after which Qt marked the
automation window unexposed. XCB likewise reported the covered window
unexposed. Compositor-present latency therefore remains an
environmental qualification limitation, not a hidden pass or a reproduced
ConversationView defect. The gate and thresholds were not weakened.

Final accounting is production **35,487 CLOC** and tests **43,719 CLOC**:
MR-8 is P **0** / T **+424**, disproving both the small negative production
forecast and the smaller test forecast. Cumulative MR-1..MR-8 production
remains **-999 CLOC** from the 36,486-CLOC anchor. The larger test delta is the
per-profile frame/pixel/work/resource evidence; no parity-only or duplicate
renderer test was introduced. `git diff --check`, affected Release/Debug
builds, focused correctness, and all registered performance cases pass. No
temporary compatibility path or known MR-8 workaround remains.

### MR-9 pre-edit evidence, dependency order, and acceptance gates

**Status: implementation complete; S8 platform qualification remains
(2026-09-18).** The exact entry was native production
**35,487 CLOC** and native tests **43,719 CLOC**, using the same tracked-file
`cloc` scope as MR-8. The complete Release baseline passes **63/63**, including
all **29** registered quantitative performance cases and all eight registered
conversation focus/style/DPR cases. Lazy conversation residency, the sole
`ConversationCard` renderer, the native Thread tree, and MR-7's distinction
between semantic mouse selection and genuine keyboard focus are fixed
constraints, not MR-9 rewrite targets.

Two independent read-only audits and a primary source/runtime trace refine the
remaining review findings as follows. No original finding disappears:

- **H-14 remains verified.** `CopyButton` alone reads
  `SH_Widget_Animation_Duration`; the loading spinner, conversation follow-tail,
  pending/steering card sweep, and visible Thread-row sweep ignore the same Qt
  fact. The existing static ring, pending card surface, and Thread-row surface
  already provide non-motion feedback. Consolidate the style-hint query in
  existing `UiStyle`; delete the Copy-local query plus the
  `spinnerAnimationActive` and `pendingFeedbackVisible` test mirrors. In reduced
  motion, Copy changes immediately and retains only its semantic hold timeout,
  loading reveals one static delayed ring, follow-tail reaches its destination
  synchronously, and pending surfaces remain static with zero repeating visual
  wakeups. No policy object, stored preference, callback, timer manager, or
  replacement animation is accepted. Expected P **-5..+5**, T **+60..+110**;
  any positive production result must be absorbed by same-stage deletion or
  stopped for approval.
- **M-16 remains verified but is partly superseded by H-10.** One shared native
  Copy control already exists. The remaining duplicate is clipboard publication
  in Card and Agent consumers, while `MarkdownTextView::keyPressEvent` removes
  the presentation-only U+200B marker by replacing the rich selection MIME with
  plain text. Replace that keyboard-only post-write path with Qt's supported
  `createMimeDataFromSelection()` hook, preserving every MIME format while
  cleaning textual representations. Let the shared Copy primitive publish the
  already-surface-specific text/Markdown value and own its feedback. Diff's
  labelled plain-text toolbar action and Markdown selection remain genuinely
  distinct interactions. `QClipboard::setMimeData()` has no success result;
  ownership polling/readback would not prove persistence, so the audit's
  “detect native clipboard failure” demand is classified **unsupported by the
  public Qt contract**, not implemented through a timer or false success gate.
  Expected P **-2..+6**, T **+25..+50**; H-10 already realized the former large
  Copy-control deletion forecast.
- **M-21 remains verified in narrowed form.** Its original blanket “no event
  path” claim is superseded: QWidget name/description, show/hide, enabled,
  focus, label/button text, and QTextDocument mutation already emit Qt
  accessibility events. Current source both duplicates some of those events and
  omits events for custom semantic selection/focus, busy, and expanded state.
  First record the native stream, guard no-op setters, delete the explicit
  Inspector `NameChanged` duplicate and duplicate accessible names, and rely on
  native text streaming events. Then emit only proven missing O(1) transitions
  against the existing physical card/tree/control interfaces; never create a
  shadow accessibility model or event coordinator. A checkable QToolButton is
  not an acceptable disclosure shortcut: the local Qt 6.10 probe changes its
  role from Button (43) to CheckBox (44). True Button-role
  expandable/expanded/collapsed state therefore needs one shared interface over
  the existing `DisclosureButton`. `QAccessibleAnnouncementEvent` is Qt 6.8+,
  while CodexUI supports 6.6; the compatible Copy/request announcement decision
  remains an explicit growth/compatibility gate. Expected deletion-first M-21
  P **+10..+35**, T **+100..+180**; a net-positive containing stage or new
  shared interface is approval-gated before production implementation.
- **M-13/X-03 remain verified but are partly superseded.** Current
  `ConversationView` and Inspector already invalidate geometry from Qt
  font/style/DPR events. The current tree has **34** reusable hex sites outside
  `UiStyle` plus six numeric pending-gradient color sites, rather than the
  original 68. Two global tooltip rules conflict and the last one wins; deleting
  the first preserves current pixels. Exact static local sheets can move into
  the existing application stylesheet while their widget-local ownership is
  deleted; exact missing colors become tokens without substituting near colors.
  ThreadPane styling is excluded from visual change. The global 1-to-2 px focus
  border and selected+focused Thread appearance remain characterization items,
  not permission to alter pixels. Application-font regeneration and
  constructor-only editor geometry must reuse existing Qt change events and the
  M-10 geometry owner, not add a style epoch/cache. Updated expectation P
  **-20..-60**, T **+40..+100**.
- **M-12 was verified and is complete.** `ImageThumbnail` previously decoded
  synchronously and keyed its sole `QPixmapCache` entry without DPR/physical
  target. Deleting the cache would re-decode on virtualized eviction. The sole
  renderer/cache is now DPR-correct; real 77--79 ms cold decode moved through
  the existing global Qt worker pool, while QPixmap/cache/widget/geometry work
  remains on the GUI thread. One stale-request key plus QObject lifetime checks
  replace the synchronous path; test-only dynamic mirrors are deleted.
- **M-30 is narrowed, not reopened wholesale.** Conversation mouse selection
  versus keyboard focus is complete in MR-7. Copy already distinguishes mouse
  and keyboard focus. Disclosure hover/focus and the global focus-border rule
  remain source risks until Fusion/Breeze geometry and pixel characterization
  reproduces a defect. The ThreadPane appearance may not change.

The one authoritative MR-9 implementation order is:

1. [x] Reconcile local Qt evidence and both read-only audits; classify native
   automatic events, unsupported clipboard failure detection, Qt 6.6/6.8
   compatibility, and focus/role contradictions before editing.
2. [x] **MR-9A:** add failing behavioral characterization for reduced motion
   and rich selection MIME, then consolidate H-14 and M-16 by deletion. Preserve
   normal-mode pixels/timing and every existing accessibility name.
3. [x] **MR-9B:** consolidate exact M-13 tokens/static QSS and delete the losing
   tooltip rule; regenerate the one application stylesheet from existing font
   change delivery. Do not change ThreadPane visuals or add a theme/epoch owner.
4. [x] **MR-9C:** record native accessibility events, delete duplicate/no-op
   emissions, and implement only the proven missing custom selection/focus/busy
   events with exact-once and semantic-no-op gates.
5. [x] **MR-9D:** add the one shared
   Button-role disclosure accessibility interface and a Qt-6.6-compatible
   semantic Copy/notice/request announcement path. Stop first if the containing
   production stage is net positive or requires a new compatibility mechanism.
6. [x] **MR-9E:** make the existing thumbnail cache DPR-correct, measure the
   synchronous defect, and replace it with one lifetime-safe asynchronous
   decode boundary while retaining one renderer and one cache.
7. [ ] **MR-9F platform remainder:** run native-platform/AT-SPI execution and
   the currently prohibited full non-short-circuiting suites; compile-only
   Release/Debug/Clang/sanitizer and autonomous offscreen live gates are done.
   Preserve all **29** registered quantitative gates and the Fusion/Breeze DPR
   1.0/1.25/1.5/2.0 pixel and
   geometry checks, warning/sanitizer coverage in scope, exact LOC accounting,
   and a final timer/cache/flag/callback/special-case audit before closing MR-9.

### 2026-09-18 resumed-worktree checkpoint

This checkpoint updates the plan above; it does not create a competing plan.
After the completed M-10/H-13/M-19/M-27/M-12 and Clang-portability work, the
current native production tree is **37,374 CLOC** and native tests are **45,244
CLOC**. Relative to HEAD the active worktree is native production **+1,087**,
browser production **-6**, tests **-67**, and root CMake **-9** physical lines.
Pre-existing changes remain
user-owned and are not reset or hidden.

- [x] **Live whole-UI lag remediation:** exact deferred GraphChanged delivery,
  stable semantic card identity, bounded row admission, latest-wins worker
  projection, off-GUI Markdown preparation, and no-op DiffViewer updates are
  implemented. The offscreen/Xvfb live matrix recorded conversation, Threads,
  Inspector, and prompt interaction at p99 16 ms with no repeatable frame above
  24 ms; the one 29 ms first-activation Inspector sample did not reproduce in
  the immediate 300-sample rerun (max 18 ms). Temporary probes are deleted.
- [x] **Build ownership and warning gate:** `UiStyle`, conversation UI,
  `NodeGraphUiAdapter`, diff UI, middle UI, runtime, and shell are reusable
  targets. Every production `.cpp` is listed once; the production tree builds
  with `-Wall -Wextra -Werror` (or `/W4 /WX`), and TLS/RFCOMM/WebSocket
  definitions use one helper for every relevant target. Compile-only consumer
  builds pass; no test executable was run in this checkpoint.
- [x] **Shared-label and DTO remainder:** Composer uses the shared selectable
  label factory. Browser file-change `cwd`, local-prompt admission/recovery
  shape, and bounded generic-activity detail now match native meaning; the
  browser no longer retains a second unbounded raw card payload. TypeScript
  compilation passes.
- [x] **M-11 source reduction:** attachment MIME classification no longer reads
  file contents on the GUI thread. Selection performs only bounded metadata
  work for at most 16 entries. A total-byte rejection policy is not a Qt
  performance repair and would change accepted product behavior, so it is not
  introduced as remediation.
- [x] **MR-9D:** one shared disclosure interface now preserves Button role,
  Press/SetFocus actions, and exact expandable/expanded/collapsed state. Copy
  and the existing notice surface use one semantic announcement helper
  (`QAccessibleAnnouncementEvent` on Qt 6.8+, standard Alert on Qt 6.6/6.7).
  The offscreen/Xvfb live probe observed exactly one state event for a real
  transition, none for the repeated no-op, and one announcement. Adding
  `Q_OBJECT` to the shared disclosure class was required so Qt's accessible
  factory sees its concrete type instead of caching the built-in QToolButton
  interface; the first live probe disproved the incomplete factory-only
  implementation. Press and SetFocus are implemented by that same interface,
  not by a shadow object.
- [x] **MR-9E live closure:** the sole thumbnail cache now keys physical DPR,
  decodes to a physical target, uses device-independent geometry, and reloads
  on `DevicePixelRatioChange`. An environment-gated probe in the real
  `codex-ui` executable was run only through offscreen/Xvfb and then deleted.
  At DPR 1/1.25/1.5/2 a real 3024x4032 3.8 MiB JPEG produced correct 129x172
  logical geometry and 129x172, 161x215, 193x258, and 258x344 physical rasters.
  The measured 77--79 ms synchronous defect is replaced in place: warmed
  construction and cache hits are 0 ms, decode completes in 42--46 ms without
  blocking event delivery (maximum 2 ms GUI heartbeat gap), and exactly one
  intrinsic-height notification changes the card extent from 131 to 263 px.
  A destroyed in-flight thumbnail also completes safely under ASan/UBSan.
  The maximum allowed 16-image message was then live-stressed at all four
  DPRs: warmed construction was 1 ms, all rasters completed in 55--56 ms,
  exactly one height notification was emitted, and the worst GUI heartbeat gap
  was 4 ms.
  Worker threads create only QImage; QPixmap/cache/widget/accessibility/geometry
  remain GUI-thread-owned. No timer, second renderer, or second cache was added;
  the old synchronous decode and three dynamic probe properties were deleted.
  Temporary live instrumentation is removed.
- [x] **M-01:** notices and graph-driven selection are durable NodeGraph state;
  only bounded, non-authoritative protocol diagnostics remain in the transient
  worker queue. `UiEffect`, its enum/kinds, queue reservation, Shell dedupe and
  fallback routing are deleted. Prompt admission now writes its prompt,
  selection, and rejection notice in one graph transaction, avoiding the
  revision-N-after-N+1 publication order found during final review. Production
  and every consumer target compile warning-clean; no test executable was run.
- [x] **M-18:** FileChanges uses Qt's one native hypertext interaction and
  accessibility implementation. Custom cursor-range mouse/key hit testing is
  deleted, and open failures route through the existing notice surface.
- [x] **H-12/M-25:** one `DiffViewer` now owns the sole provider, refresh
  coalescer, repository poll, and snapshot used by both the embedded surface and
  review window. The review no longer owns a provider, poll, workspace copy, or
  eagerly active documents; only its selected layout retains text. Same-file
  cursor/selection and both scroll axes survive refresh, and semantic no-ops do
  not replace text. The owner switches the one provider request between compact
  and expanded context and returns to compact when review closes.
- [x] **M-26:** hidden diff polling is stopped and duplicate review wakeups are
  deleted. A temporary real-window event filter measured the complete idle
  stream for 2.2 seconds: exactly 22 timer events, all from the documented
  100 ms eventfd wake-recovery poll; every visual/thread/conversation/composer/
  inspector/diff timer emitted zero. The retained poll performs only bounded
  atomic/queue checks and preserves delivery after an admitted wake failure;
  deleting it would weaken reliability. Temporary instrumentation is removed.
- [x] **M-04:** rename revalidates the retained Thread node after the nested
  modal loop before sending and preserves authored text only for an unadmitted
  live rename. Optimistic-create selection retries were already deleted; the
  current source has no second general focus retry owner.
- [x] **H-13/M-10:** the composer is a normal layout child and Qt layout is its
  sole geometry authority. The reserve/anchor overlay, trailing-space scroll
  compensation, canonical-height callback, settlement callback, and
  composer-growth pause state are deleted. Attachment removal mutates the exact
  stable row, safely defers only destruction of the emitting QObject, transfers
  focus to an adjacent Remove/Attach control, and announces removal without a
  full rebuild. A real `codex-ui` offscreen/Xvfb live gate passed at DPR
  1/1.25/1.5/2: expanded composer reduced the conversation viewport 542→394 px
  with no overlap; removal/deletion/focus passed; combined scroll/edit p99 was
  2--4 ms and maximum 4--5 ms. Temporary instrumentation is deleted.
- [x] **H-14/M-13:** all native motion surfaces consume the existing
  `UiStyle::animationsEnabled` fact, and reusable raw visual color literals are
  confined to `UiStyle` tokens. No theme epoch, policy object, or alternate
  ThreadPane appearance was introduced.
- [x] **M-19:** request shape/disclosure is bounded at the existing policy
  boundary, controls are keyboard-accessible, first-invalid focus is explicit,
  and Shell re-reads the current request after the modal before submission.
  Deleting the ephemeral-toggle `name->clear()` preserves authored names while
  the draft still omits the name whenever Temporary is enabled. A four-DPR
  offscreen/Xvfb live dialog pass proved name preservation, invalid-editor
  focus ownership, and Unicode RTL/emoji submission. The offscreen plugin has
  no active top-level after the nested warning, but the parent dialog's focus
  target is correct; no activation retry was added.
- [x] **M-27:** the 1100×700 window floor and three competing pane minimum
  widths are deleted. Text-bearing bars/buttons use minimum rather than fixed
  heights, and existing ignored-width labels may yield before controls. The
  normal 1536×960 splitter remains exactly 282/834/404 and ThreadPane styling
  is unchanged. Live offscreen/Xvfb qualification passed 800×600, 1100×700,
  and 1536×960 at 1.0× and 1.5× fonts with all panes and Hide controls
  reachable.
- [x] **M-29/M-30 current product behavior:** the geometry-changing global
  1→2 px focus border is replaced by a color-only transition on the existing
  border. A temporary real-widget probe passed Fusion and Breeze at DPR
  1/1.25/1.5/2: Arabic, Hebrew, combining marks, emoji/graphemes and CJK
  rendered; real IME commit preserved the prompt; all 40 visible tab-focusable
  controls retained exact geometry and 27 produced visible focus-pixel change.
  Temporary instrumentation is deleted. Durable repository-wide adversarial
  fixtures and native-platform breadth remain S8 because executing/adding test
  workflows is currently prohibited by user instruction.
- [ ] **Remaining S8-S9 qualification:** durable Unicode/RTL/IME fixtures, CI
  Release/Clang/sanitizer/platform breadth, and native AT-SPI remain. The
  AISuite publish/pin transition is complete. Multi-case native mains already use
  non-short-circuiting accumulation; generated-protocol drift is locally
  repaired. The CI execution matrix is not expanded while the user's current
  no-test-workflow instruction is in force.
- [x] **S8 compile-only matrix:** exact-current GCC Release, GCC Debug, GCC
  ASan/UBSan, and Clang Release builds complete at 14 jobs without running a
  test executable. Clang exposed and closed four production portability gaps:
  the catalog uniqueness proof now sorts then checks adjacent keys instead of
  quadratic constexpr string comparison; libgit2 options use its initialization
  API instead of warning-prone aggregate macros; one `unique_ptr` return moves
  explicitly; and a template callback no longer captures an unused SDK. Six
  test-helper projection switches now exhaustively classify `Protocol` and the
  final Clang rebuild is warning-clean. CI/platform execution breadth remains
  open under the current no-test/offscreen-only constraints.

The authoritative remaining dependency order is:

1. [ ] **S8 qualification remainder:** supported native-platform/AT-SPI
   breadth, durable Unicode/RTL/IME fixtures, and a CI execution matrix. GCC
   Release/Debug/ASan plus Clang Release compile cleanly, active documentation
   is reconciled, and multi-case mains are non-short-circuiting. The current
   user instruction forbids executing test binaries, so only compile and
   autonomous live offscreen/Xvfb gates run now.
2. [x] **AISuite generated protocol release:** pin exact stable+experimental
   generator inputs, regenerate typed bindings, add regenerate-and-diff CI,
   publish an immutable AISuite revision, update CodexUI's pin, replace eight
   adapters with generated operations, and retain only the two intentionally
   internal typed descriptors. Completed against immutable AISuite revision
   `24b1d16ca87e0f0d13fef75a68877cadbb32c03f`.

The AISuite side of item 2 is published and pinned.
`tools/regenerate-codex-protocol.sh` pins Codex tag `rust-v0.154.0`
and commit `6b9826e3aa83b1a5947db50f4332cb9c65f1b340`, verifies that tag-to-commit
mapping, extracts the release's own experimental precomputed schema, and
regenerates C++/TypeScript/manifest together. A second `--check` run reproduced
all three artifacts byte-for-byte; the generated native library and TypeScript
package compile cleanly. AISuite CI runs the same drift gate. The resulting
bindings expose the expected eight operations and still omit the two upstream
internal-only operation descriptors. CodexUI now pins the published revision,
uses generated operations for all eight, and retains only typed local operation
identity for the two internal `rawResponse*` notifications. The obsolete
`CurrentProtocolAdapters` name and eight untyped descriptors are deleted.

#### AISuite generated-protocol evidence reconciliation

Claude's generated-protocol drift finding is **verified in cause but its
delete-the-whole-shim remedy is superseded by current evidence**. AISuite's
checked-in C++ and TypeScript bindings have matching hashes and exactly match
their pinned Codex source, but that source was paired with the stable-only JSON
schema. CodexUI enables and consumes the experimental surface, so the stable
generator necessarily omits those operations. AISuite CI compares the two
generated outputs with each other but neither pins the input files nor
regenerates them, so it cannot detect this class of drift.

An exact Codex 0.154.0 experimental export plus its matching tagged
`common.rs` generates typed operations for `thread/items/list`,
`thread/turns/list`, `currentTime/read`, both auth-recovery notifications, and
the three legacy realtime-item notifications: **eight of the ten** handwritten
descriptors. `rawResponseItem/completed` and `rawResponse/completed` are marked
internal-only upstream and are deliberately omitted even from the public
experimental operation union, although their typed payload definitions remain
exported. The accepted AISuite repair is therefore: pin both generator inputs
to one Codex release, regenerate and diff them in CI, publish the resulting
AISuite revision, replace those eight descriptors with generated operations,
and retain only two typed internal-operation descriptors. A blanket deletion
would lose valid older-server notification compatibility; retaining ten
untyped `Value` descriptors would preserve avoidable drift.

This was a cross-repository release dependency, not a reason for a CodexUI-local
compatibility layer. AISuite revision `24b1d16...` supplies the immutable release
identity and CodexUI's one revision pin now selects it for native and browser
builds. The transition also reconciles the complete generated operation catalog:
five newly generated client methods are classified, while three older-server
methods remain explicit legacy compatibility. No app-server source, fabricated
SHA, parallel adapter, timer, cache, flag, or protocol state was introduced.

Exact transition verification: AISuite regeneration `--check` reproduced the
pinned Codex 0.154 experimental artifacts and its frontend package passed 20/20;
the exact-pinned CodexUI Release tree built completely warning-clean; the native
protocol-binding gate passed 1/1; and the browser suite passed 287/287. Two broad
native integration executables retain their pre-existing current-environment
failures identically in the pre-transition binary and are not misreported as a
transition regression. Stage accounting is native production **-47 CLOC**,
native tests **-84 CLOC**, and root CMake physically neutral after target/file
renaming.

**MR-9A closure (2026-09-17).** The restored invariant is that motion policy and
native Copy publication each have one authority: every affected surface reads
the same live Qt animation-duration fact, and the shared `CopyButton` publishes
Card/Agent values while `MarkdownTextView` owns selection MIME normalization.
This stage deleted the Copy-local style query, duplicate Card/Inspector
clipboard publication, the keyboard-only selection rewrite, and the
`spinnerAnimationActive`/dynamic `pendingFeedbackVisible` test mirrors. It did
not add a timer, cache, preference, callback, renderer, or persistent state
field. Existing spinner, pending, Thread-row, follow-tail, and Copy mechanisms
are stopped, resumed, or completed through their existing Qt event paths.

Failing-first checks reproduced rich-MIME loss, reduced-motion work in pending,
loading, and Thread surfaces, live style-transition gaps, retained ODF marker
content, and nested-steering transition gaps. The accepted replacement keeps
plain text, HTML, Markdown, and ODF formats while removing the presentation-only
U+200B marker; public Qt ODF serialization is performed only for an actual Copy
selection. Under a zero duration, Copy feedback settles immediately,
follow-tail completes synchronously, and the existing spinner/pending/Thread
timers deliver no repeating animation work. Positive-to-zero-to-positive style
changes stop and resume the same physical widgets without replacing accessible
objects. Two independent read-only adversarial reviews passed the final source
and behavioral assertions.

The MR-8 anchor was production **35,487** and tests **43,719** CLOC. MR-9A closes
at production **35,527** and tests **44,107** CLOC: stage **P +40 / T +388**;
cumulative MR-1..9A production is **-959** from the 36,486 baseline. Production
growth is within the user's explicit +1,600-CLOC ceiling and consists of the
shared live-policy query/change-event handling plus format-preserving public Qt
serialization; reduction or simple call-site substitution could not preserve
ODF or react to runtime style changes. No parallel authority was introduced.

Final commands and results:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14 && ctest
  --test-dir /tmp/codexui-release-gate.I6nD6Z -j14 --output-on-failure`:
  **63/63 passed**, including all **29** registered quantitative performance
  gates, in **78.23 s** real time.
- Focused `codexui-conversation-cards`: **1/1 passed** in **11.20 s** after the
  final actual-timer and nested-steering assertions.
- ThreadPane base, four DPR geometry cases, and twelve quantitative performance
  cases: **16/16 passed** in **7.85 s** with the exact production delegate still
  installed.
- `git diff --check`: clean. The production tree contains exactly one
  `SH_Widget_Animation_Duration` query and no `spinnerAnimationActive` mirror;
  the remaining `pendingFeedbackVisible` is the typed renderer-owned visual
  phase, not a dynamic property or second authority.

Accessibility verification establishes stable widgets/names and no replacement
on motion changes; platform AT-SPI announcements remain deliberately deferred
to MR-9C/D. External clipboard persistence cannot be verified through Qt's
public no-result `setMimeData()` contract. Debug, sanitizer, compositor-present,
and final cross-platform qualification remain MR-9F rather than being claimed
by this Release offscreen gate.

Correctness gates cover normal/reduced transitions, pending/steering/loading/
follow/Copy behavior, rich selection MIME through keyboard and standard context
actions, Card and Agent Markdown MIME, font/style/DPR change, and image
load/fail/change. Accessibility gates record exact event type and existing
physical interface/ID for selection, focus, busy, disclosure, Copy, notice, and
request transitions; identical reapplication must emit none, and streaming must
not gain a second event stream. Performance gates require zero reduced-motion
repeating visual timers/repaints, unchanged normal-mode residency/documents and
bounded row-local animation, bounded image decodes/cache entries, and no new
per-frame work. Real external AT-SPI and compositor-present timing remain S8
platform qualification and will not be mislabeled as offscreen proof.

**MR-9B closure (2026-09-17).** The restored invariant is that reusable color
and geometry tokens plus the one application stylesheet own static appearance;
widget-local sheets contain only genuinely dynamic values. Current-font
geometry is derived from the current `QFont`, and a semantic font no-op cannot
replace widgets, move geometry, change focus, or repolish the application.

Deletion came first. This stage removed the losing tooltip rule; the local
top-bar, status-bar, breadcrumb, conversation, attachment, Markdown,
file-change, image-ribbon, command, and pending-card static sheets; duplicate
status-dot color writers and `statusToneColor`; local command-output padding
constants; the cached editor maximum; repeated static Brand/pending rules; and
the ribbon's conflicting platform-scrollbar geometry estimate. The retained
dynamic sheets own only Brand title pixel size, pending/status foreground, and
the pre-existing scrollbar-overlay margin. The ribbon now measures its actual
styled horizontal scrollbar. ThreadPane production source and pixels were not
changed.

The coherent replacement adds exact `UiStyle` tokens, named and
order-independent application-QSS substitution, status tone selectors, an
application-font equality guard on the existing Shell event filter, and
current-font remeasurement on existing Qt change events. It adds no timer,
cache, flag, callback, renderer, theme/epoch owner, persistent state field, or
parallel visual policy. The transient positional 66-value formatting array was
deleted before closure; unresolved named or numeric placeholders now fail the
test. M-13/X-03's raw-literal/static-QSS slice is complete. M-30's broader
focus/hover characterization remains separate and is not silently claimed
here.

The MR-9A anchor was production **35,527** and tests **44,107** CLOC. MR-9B
closes at production **35,573**: stage **P +46**, cumulative MR-1..9B
production **-913** from the 36,486 baseline. This disproves the predicted
MR-9B reduction. The intermediate candidate was P +101; safe deletion reduced
it by 55 CLOC. The irreducible remainder is the exact typed-token registry,
named substitution, and live application-font geometry path; compressing that
mapping or restoring raw/local policy would make the architecture less clear.
The +46 is within the user's explicit +1,600 production-CLOC approval. Tests
close at **44,362 C++ CLOC** plus **101 CMake policy-test CLOC**, or **44,463**
on the MR-9B-inclusive gauge: stage **T +356**. Root CMake additionally
registers the policy gate and carries the requested-DPR environments; it is
reported separately from production and test source.

Final commands and results:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14`: passed after the
  final source-policy, DPR, and projection-gate changes.
- Focused style/Card/Shell plus four ThreadPane visual DPR cases and the 10k
  projection profile: **8/8 passed** in **12.17 s**.
- `ctest --test-dir /tmp/codexui-release-gate.I6nD6Z --output-on-failure
  -j14`: **64/64 passed** in **74.57 s**, versus MR-9A's 63/63 in 78.23 s.
  All **29/29** registered quantitative gates passed in **63.37 s**; this is no
  measured regression, not a claim that scheduler noise is an optimization.
- Fusion and Breeze conversation focus plus ThreadPane and Inspector geometry
  ran without skips at verified DPR **1.0, 1.25, 1.5, and 2.0**. The twelve
  ThreadPane quantitative cases retain 18 fixed widgets, zero index widgets,
  one fixed document, row-local paint work, and now gate adapter projection.
  Three isolated 10k Release samples measured adapter projection at
  **24.874/24.014/23.918 ms** before installing the 500 ms Release regression
  ceiling (4x only for instrumented builds).
- `git diff --check` and the non-short-circuiting source-policy script pass.
  Independent source and gate reviews found no remaining MR-9B blocker.

Correctness verification covers exact prior colors/selector specificity,
single tooltip ownership, command/image geometry, current-font editor and Brand
fit, focus and object identity, and identical-font-change pixel/geometry no-op.
Accessibility names and physical objects remain stable; MR-9B adds no semantic
tree. Performance verification covers bounded Card/Thread/Inspector residency,
documents, measurement/materialization/paint work, wheel latency, and explicit
thread projection time. The source policy is a scoped syntactic regression
guard plus a current-worktree raw-literal scan, not a full C++/QSS parser.
Refreshed warning/ASan/UBSan, real-compositor/AT-SPI, and non-Linux platform
qualification remain explicitly assigned to MR-9F rather than being
misrepresented by the final Linux/offscreen Release run.

**MR-9C pre-edit gate (2026-09-17).** The invariant being restored is that
each semantic accessibility transition has one event authority on the same
physical interface that exposes the resulting state, while a semantic no-op is
silent on every supported Qt version. Native QWidget/QLabel/QAbstractButton and
QTextDocument events remain native; custom projected selection, logical-row
focus, busy state, and Thread-tree semantics are emitted only at their existing
state-owner boundaries after mutation.

Three independent read-only audits and the primary source trace agree on the
following evidence. Qt 6.6, the configured minimum, emits
`NameChanged`/`DescriptionChanged` for every accessible-property setter call,
whereas the installed Qt 6.10.2 equality-guards those setters. Therefore
application-side equality or deletion is required for cross-version no-op
semantics. `QLabel::setText`, button text, QWidget visibility/enabled/focus, and
native text-document mutation/selection already emit the required events and
must not be mirrored. `QAbstractItemView` emits no row selection/current event;
Conversation's custom ListItem state is consequently silent. Its two custom
busy bits are likewise silent. Thread's required nested custom tree conflicts
with QTreeView's native accessibility events: Qt addresses a private flattened
child number (including a header offset), while `ThreadTreeAccessible::child()`
authoritatively addresses top-level semantic items. Reinterpreting that number
or exposing a second flat tree is rejected.

Deletion precedes replacement: remove the explicit Inspector `NameChanged`,
the status-label accessible-name mirrors, the history-button text/name mirror,
and every repeated same-value accessible setter in this affected path. Bypass
QTreeView's incompatible flattened focus/selection event layer at the existing
`ThreadTreeWidget` subclass boundary while retaining its ordinary view update
behavior. Keep the explicit Thread-item `ObjectDestroyed` event before cache
retirement; it is the sole valid lifetime event for the QObject-less semantic
item.

Deletion alone is insufficient because Qt cannot infer the custom state bits or
map its private flat index into the one nested Thread hierarchy. The coherent
replacement uses stack events at the existing mutation boundaries, derives
old/new values locally, and adds no timer, cache, flag, callback, coordinator,
shadow state, renderer, or accessible object. Thread presentation events must
not eagerly create interfaces while accessibility is inactive; selected and
visible Conversation objects remain bounded by renderer residency. The
read-only forecast is production **+40..+70** after a deletion floor of eight
lines, above the earlier narrow +10..+35 estimate but inside the user's explicit
+1,600 production-CLOC approval. The MR-9B anchors remain production **35,573**
and tests **44,463**; exact stage deltas will be recorded at closure.

Verification will install one test-only synchronous event recorder, resolve
events to stable accessible IDs rather than incidental child positions, and
gate exact changed/no-op selection, focus, busy, Thread name/description,
expansion, reparent, and destruction transitions. It will also prove that
Markdown mutation/selection and native widget name/visibility/enabled/focus
retain one native event stream, interface identity is stable, and no event
materializes an offscreen Conversation renderer. Correctness and accessibility
run in the Conversation, Thread, and Inspector suites; performance re-runs all
29 quantitative gates and checks unchanged bounded residency/per-frame work.
Qt 6.6 compilation and real AT-SPI remain MR-9F qualification rather than being
inferred from the installed Qt 6.10 offscreen backend.

**MR-9C closure (2026-09-17).** The restored invariant is that each application-
owned semantic transition publishes once, after mutation, on the same retained
accessible interface that exposes the final state. Native widgets retain their
native roles/actions/events, while identical state is silent and model-only
Conversation rows never acquire a renderer merely to emit an event.

Deletion remained part of the replacement. This stage removed the Inspector's
manual duplicate `NameChanged`, status-label accessible-name mirrors, the
custom history-button accessibility wrapper and text/name mirror, and duplicate
row-focus publication on ordinary widget focus entry. Repeated accessible-name
and description setters are equality-guarded for the Qt 6.6 floor. The native
history `QPushButton` again owns Button role, press action, name, geometry, and
disabled state; the existing Conversation List interface alone owns aggregate
history busy state.

The coherent replacement emits only the state Qt cannot infer: Conversation
resident-row selection/current focus and List/loading busy; and Thread nested-
tree selection/current focus, presentation/reparenting, expansion, and exact
objectless-item retirement. Thread refresh captures only already-registered
interfaces, completes the one item reconciliation, and then publishes settled
events. Stable presentation identity survives optimistic target promotion.
The Tree selection interface now follows Qt's direct-child-only contract and
therefore reports/accepts roots only; every nested TreeItem retains its native
semantic selected state and existing press/focus action. Disabled or wholly
hidden Conversation/Thread containers reject accessibility mutations. No
timer, cache, flag, callback, renderer, shadow model, persistent state field,
or visual rule was added.

The event recorder resolves every event to a stable interface ID. Tests prove
changed/reverse/no-op selection (deliberate Qt-compatible Add-before-Remove
order), logical focus, busy, name/description, expansion masks, combined
reparent/presentation, retirement, optimistic promotion, disabled actions, and
interface identity. A model-only Conversation selection leaves scroll,
resident count, accessible children, construction count, and events unchanged.
Streaming retains one native Markdown widget, document, accessible interface,
text interface, selection, and ID; native selection/caret events occur once in
source order, identical selection/content is silent, and no duplicate text
event targets the containing ListItem. Loading failure/retry has exact
Name-then-busy ordering, reverse completion, and repeated-failure silence. The
Shell integration test now checks native button-disabled plus List-busy state
instead of reviving a non-native busy Button implementation.

Two apparent blockers were explicitly reclassified rather than dropped.
`QAccessible::setActive(true)` is not a portable test seam: in Qt 6.6 through
6.9 it only notifies activation observers, while Qt 6.10 additionally mutates
platform accessibility. Native Markdown content events are consequently
required locally only when `QAccessible::isActive()` and real Qt 6.6/AT-SPI
delivery remains MR-9F. Native QWidget focus/show/hide and QLabel/QPushButton
event decomposition are local Qt 6.10 characterization, not cross-version exact
contracts. Qt source also shows that QLabel clear/pixmap replacement does not
publish fallback-name change; the affected MR-9C status paths are text-only, so
that source risk is disproven here and image/thumbnail accessible naming remains
with MR-9E qualification. Semantic Copy, repeated-notice, and request
replacement announcements are still explicitly owned by MR-9D; they cannot be
misrepresented by ordinary property-change tests.

The MR-9B anchor was production **35,573** and tests **44,463** CLOC. MR-9C
closes at production **35,850** and tests **45,277** CLOC: stage **P +277 / T
+814**; cumulative MR-1..9C production is **-636** from the 36,486 baseline.
This disproves the +40..+70 production forecast but remains inside the user's
explicit +1,600-CLOC ceiling. The irreducible increase is the typed physical-
tree interfaces/events and post-mutation exactness needed where Qt's flat item-
view events cannot address the authoritative nested hierarchy. Both final
read-only audits found no clarity-preserving production deletion; micro-
compression would obscure state masks and lifetime ordering.

Final commands and results:

- `cmake --build ../build/Qt-Remediation-Release -j14`: passed, including the
  application and every test target.
- `ctest --test-dir ../build/Qt-Remediation-Release --output-on-failure -j14`:
  **64/64 passed** in **74.40 s**. All **29/29** registered quantitative gates
  passed in **63.24 s** of aggregate labelled time. The first complete run
  exposed one stale Shell assertion; after replacing it with the native Button
  disabled/List busy contract, the focused Shell test passed and the complete
  matrix passed.
- The same run passed Conversation focus pixels under Fusion and Breeze and
  Thread/Inspector geometry at requested DPR **1.0, 1.25, 1.5, and 2.0**. The
  final strengthened Thread expansion-mask target passed separately after the
  full run.
- `/tmp/codexui-h11-warnings.LDqrVn` rebuilt the three affected suites with
  `-j14`: passed with no warning in MR-9C production source. Existing fixture
  aggregate-initializer/sign warnings remain visible and are not suppressed.
- `git diff --check`: clean. Static audit found no added persistent timer,
  cache, flag, callback, renderer, or accessibility authority.

Qt 6.6 compilation, active AT-SPI/platform receipt, possible native
`QTreeWidget` table-model event leakage beside the semantic tree, compositor-
present timing, sanitizer refresh, and non-Linux breadth remain MR-9F and are
not claimed by the offscreen Qt 6.10.2 run.

### M-45 oversized resume frame and paginated history boundary

**Status: complete after controller and observer live transport qualification
(2026-09-18).** A very long
thread caused `JSONL frame exceeds configured maximum before delimiter` in the
bridge/app-server transport. Local tracing verified that CodexUI could request
the complete retained history in one `thread/resume` or `thread/read` response,
while codex-bridge correctly retained a bounded frame guard. Upstream Codex
issue [#40362](https://github.com/openai/codex/issues/40362) independently
reproduces the same shape: a 23,159,303-byte resume record falls to 5,104 bytes
with `excludeTurns:true`. This is a reproduced protocol-boundary defect, not a
JSON parser defect and not permission to make framing unbounded.

The violated invariants are I-2/I-3/I-5/I-6/I-7/I-10: provider history has one
graph authority; transport work and response size must be bounded; controller
and observer hydration must merge through that authority; performance must not
create a second history store or presentation path. The accepted remediation
deletes every automatic complete-history request. A controller first performs
`thread/resume(excludeTurns:true)` to establish metadata/live state and then
requests `thread/turns/list` pages with `limit:80`, descending order, and
`itemsView:"summary"`. An observer cannot resume and therefore starts with the
read-only turn page. Each returned Turn schedules `thread/items/list` pages of
80 through an exact eight-request concurrency ceiling. Existing
`WorkerLogic`/`ProtocolUpdater` and stable graph identities remain the sole
merge and ordering authority; the scheduler retains only pending page work and
no history cache. Metadata-only fork uses the same page path while its first
prompt remains immediately admissible. Browser controller hydration now uses
the same `thread/resume(excludeTurns:true)` then first-turn-page ordering;
browser observers start directly with the read-only turn page. Both frontends
become ready only after that first page and retain bounded cursor continuation,
global eight-request item hydration, active-child hydration, and provider-
generation/stale-result rejection.

AISuite codex-bridge now permits the two pagination methods for observers and
restores each frontend request ID on the response. It does not rewrite frontend
requests, remove the frame guard, or grant observer mutation authority. Exact
source provenance for the installed Codex 0.154.0 binary was recovered from
npm's signed build attestation: tag `rust-v0.154.0`, commit
`6b9826e3aa83b1a5947db50f4332cb9c65f1b340`. Regenerating AISuite against that
exact experimental schema expands generated bindings substantially. The
earlier rejection of that expansion as a CodexUI-only patch is superseded by
the later verified generator-drift finding: AISuite must type the complete
public schema it claims to generate, and CI must reproduce it. The published
AISuite repair now does so. CodexUI pins that immutable revision, uses generated
bindings for eight operations, and retains only the two upstream internal-only
`rawResponse*` operation identities with generated payload types; this is not a
second protocol implementation.

The legacy `thread/read` operation itself remains for bounded metadata reads.
Only `includeTurns:true` and implicit full-history resume/fork behavior are
removed. No version-selected full-history fallback is accepted: a version is
not a negotiated method capability, and selecting the old path for an old
server would recreate the failure on precisely the servers/threads needing
protection. A server without pagination receives the bounded request, returns a
normal unsupported-method/error response, and CodexUI reports that paginated
history support is required. Supporting such a server safely would require a
genuinely bounded protocol supplied by that server, not a client-side retry of
the unbounded operation.

The authoritative interruption plan is:

1. [x] **M-45A:** preserve the JSONL limit; delete automatic full-history
   resume/read/fork; implement one metadata-plus-pages path in native and
   browser clients; allow exact observer page methods; preserve one graph
   merge authority and bounded concurrency.
2. [x] **M-45B:** add native/browser/bridge regressions for request parameters,
   cursor continuation, stable chronological merge, observer routing and ID
   restoration, metadata-first controller ordering, competing-snapshot
   admission, fork prompt independence, and the exact eight-page concurrency
   ceiling.
3. [x] **M-45C:** build the new bridge and execute source-level Release/native,
   browser, frontend-SDK, routing, framer, and provider transport gates without
   installing or restarting it.
4. [x] **M-45D:** the restarted
   controller loaded the 341 MB `Big architectural refactoring` rollout through
   one `thread/turns/list` and one `thread/items/list` request without an
   oversized-frame error, transport restart, or request storm. The only fresh
   observer was then restarted and the same long thread loaded successfully
   through the bounded paginated path without an oversized frame or transport
   failure. Controller and observer share the same graph merge/order authority.
5. [x] **M-46** is disproven as a CodexUI defect and **M-47** is implemented
   and live-qualified below. MR-9D/MR-9E are complete; MR-9F retains only the
   explicitly unavailable S8 platform/AT-SPI execution breadth.
6. [x] **M-45E:** remove the browser's remaining legacy/eager divergence. The
   browser no longer selects with `thread/read`, recursively drains every turn
   page, marks the thread ready before its first page, or applies an eight-page
   ceiling independently per turn. Controller/observer ordering, one-page
   continuation and retry, chronological item-page merge, active-turn recovery,
   active-child hydration, and the global item ceiling are browser-session
   contract cases. CodexUI now pins AISuite `24b1d16...`, whose bridge permits
   both observer page methods and whose generated bindings cover the public
   experimental operations; CI reads that one revision file for native and web
   checkouts. The CodexUI wire path remains runtime-compatible through the
   existing SDK request boundary.

AISuite's isolated implementation delta is production **+2 CLOC** and tests
**+15 CLOC**. The two production lines extend the existing observer read
allowlist and replace no safe behavior; reduction in CodexUI removes the much
larger unbounded request path. Cross-repository CodexUI stage accounting and
the final exact command/result list close with M-45D so that live-gate fixes are
not hidden as a second stage. No bridge or CodexUI binary has been installed or
restarted.

M-45C exact-current results:

- `cmake --build ../build/Qt-Remediation-Release --parallel 14 && ctest
  --test-dir ../build/Qt-Remediation-Release --output-on-failure --parallel
  14`: build passed; **64/64** tests passed in **74.56 s**, including all
  **29/29** registered quantitative gates and the DPR/accessibility matrices.
- `npm test --prefix web`: **285/285** passed, including the exact eight-request
  item-page ceiling and metadata/turn/item separation.
- `cmake --build ../build/Sanitizers-ASan --parallel 14 --target
  codexui-client-runtime-dispatch-test
  codexui-current-protocol-adapters-test codexui-shell-integration-test` plus
  the three-case ASan/UBSan CTest selection: **3/3** passed with leak/error
  halting in **14.56 s**.
- In AISuite, the Release build of `codex-bridge`, `CodexBridgeRoutingTest`,
  `CodexFrontendSdkTest`, and `CodexJsonLineFramerTest` passed. The exact
  transport selection (`CodexJsonLineFramerTest`, `CodexFrontendSdkTest`,
  `CodexBridgeRoutingTest`, `CodexProvider_stdio`) passed **4/4**; the frontend
  package passed **20/20**.
- `clang-format --dry-run --Werror` passes the affected CodexUI C++ files and
  the modified AISuite routing test; `git diff --check` passes both
  repositories. AISuite had unrelated pre-existing whole-file formatting
  drift in `CodexBridge.cpp`, outside the two inserted allowlist lines.
- The built bridge is
  `/home/voc/projects/drafts/AISuite-extraction/build/Desktop_GCC-Release/src/apps/codex-bridge/codex-bridge`,
  SHA-256 `39eb7d2437908b5d80d39373c16023bee4b362fedb3cfce7ababc4be6aae8beb`.
  `/usr/local/bin/codex-bridge` remains the older binary, SHA-256
  `ba533a105f1d2745887bc2a67857c366834abae00ae31f79e6df1bb73e92b1ed`.

The post-restart live gate supersedes the final installed-binary sentence
above without erasing its pre-restart provenance. The production bridge is now
PID 2935081 and the production controller is PID 2936296. The bridge contains
the `thread/turns/list` and `thread/items/list` observer allowlist entries; the
controller remained connected after both page requests and emitted no frame-
limit error. Exact raw request parameters were not retained on the wire, so
`excludeTurns:true` is corroborated by the exact running source path rather
than claimed from a packet capture.

M-45E exact-current verification: `npm test --prefix web` passed all **11/11**
browser suites, with **42/42** browser-session cases. `npm run profile --prefix
web` measured hydration **36.09 ms**, projection **20.92 ms**, streaming **3.20
ms**, and presentation churn **8.87 ms**, within the respective **47/45/4/20
ms** hard limits. A clean temporary extraction of pinned AISuite
`bc448516...` built its SDK, compiled the current browser, and passed all
**42/42** session cases without using the local regenerated bindings. `npm run
build:app --prefix web` and `npm run verify:artifact --prefix web` passed. The
current restricted execution sandbox denied the
qualification tool's local HTTP listener (`listen EPERM 127.0.0.1`), so a fresh
Chromium workflow run is not claimed by M-45E; the production artifact itself
was built and verified.

### M-46 final-answer parent and transcript-order report

**Status: disproven as a CodexUI defect (2026-09-17).** The two cards were both
present and their displayed order matched the provider/app-server event order:
the long-running command genuinely completed after the final answer. No
timestamp inference from the screenshot was valid or required. Provider child
order remains the sole authority established by MR-2, so the proposed
final-answer-specific reorder was reverted and no presentation rule, sort,
timer, cache, or parent override remains. A future mismatch is actionable only
if the graph/model order first diverges from captured provider order; the view
must never manufacture a preferred transcript order.

### M-47 incoming-event GUI backlog and scrolling regression

**Status: implemented and live-verified (2026-09-18).** The repair removed
redundant GUI-thread work at its owning boundaries: exact deferred
`GraphChanged` FIFO delivery, stable semantic card identity, bounded row
admission, latest-wins worker projection, off-GUI Markdown preparation, and
no-op DiffViewer updates. It did not add a renderer, presentation throttle,
skipped visible update, snapshot authority, or reconciliation path.

The autonomous offscreen/Xvfb interaction matrix covered Conversation,
Threads, Inspector, and prompt surfaces both with a long idle thread and while
bridge messages arrived. Every sustained path recorded p99 16 ms and no
repeatable frame above 24 ms; the sole 29 ms first-activation Inspector sample
did not reproduce in an immediate 300-sample rerun (maximum 18 ms). Temporary
timing probes were removed. This closes the reproduced M-47 defect under the
user-mandated offscreen live workflow; native compositor qualification remains
the separate S8 platform item and does not reopen M-47.

### S5 H-11/M-07/M-08/M-09 thread-tree stage gate

**Status: complete after M-37.** H-11 now has one ThreadPane hierarchy, state
projection, renderer, native interaction implementation, and semantic
accessibility representation. Offscreen loaded threads remain lightweight
item-model data; one delegate paints visible rows. A semantic no-op changes no
item, model index, expansion, selection, focus, scroll geometry, pixel, timer,
or accessible identity.

**Authoritative H-11 completion status (2026-09-16).** This is the final H-11
record; checked entries are completed and verified rather than merely started.

- [x] Replace the five-widget flat row reconstruction with one native tree,
  stable presentation-keyed items, and one full-row delegate; delete all index
  widgets, duplicate disclosure interaction, snapshot mirrors, expansion maps,
  dynamic diagnostic properties, and viewport-wide animation painting.
- [x] Preserve hierarchy, reparenting, sorting, selection, scroll anchors,
  pagination triggers, context actions, native keyboard navigation, and exact
  action `NodeRef`s without a second model or paging authority.
- [x] Preserve in-place optimistic draft admission and local-to-canonical
  promotion; retire drafts in one signal-atomic transaction after Qt finishes
  its current-index transition, and detach every item before destruction.
- [x] Project one semantic accessibility tree over the same native items,
  including parent/level, expanded/collapsed state, focus, selection, geometry,
  actions, and exact wrapper retirement.
- [x] Preserve the established Thread panel appearance and correct the locally
  reproduced chevron/status spacing drift to the old 2 px edge gap; connector
  branch painting remains suppressed. The native disclosure target is restored
  to the former 24 x 24 logical-pixel control through Qt's public disclosure
  subelement, with no custom mouse forwarding or pixel change.
- [x] Migrate Shell and established-UX consumers/tests to the native hierarchy;
  focused and ShellIntegration Release plus ASan/UBSan runs pass without a
  lifetime diagnostic.
- [x] Register and execute all twelve quantitative 320/1,280/10,000-row DPR
  1.0/1.25/1.5/2.0 gates; all pass with 18 fixed widgets, zero index widgets,
  one fixed document, zero offscreen animation paints, five bounded visible-row
  paints, and stable no-op pixels/focus/anchor.
- [x] Replace the four remaining fixed-delay H-11 integration waits with their
  existing semantic identity/acknowledgement predicates and rerun the complete
  relevant suite without short-circuiting; the Release focused, application-
  layout, and ShellIntegration gates pass, and the corresponding focused and
  ShellIntegration ASan/UBSan runs report no diagnostic.
- [x] Complete final visual capture/comparison, production/test LOC accounting,
  diff special-case audit, and ledger closure. No compatibility renderer,
  custom input forwarding, unbounded row object, or known H-11 workaround
  remains.

Deletion comes first. This stage deletes the five-QWidget-per-row subtree and
`setItemWidget` path, the second disclosure/status/title paint path, custom
disclosure hit testing and Left/Right navigation, `RenderedThreadList`, the
retained deep `currentSnapshot`, `expandedRows`, the separate optimistic-row
vector, moved-widget reconstruction, manual selection restoration, three
dynamic diagnostic properties, and whole-viewport 32 ms animation invalidation.
It does not restore the rejected lazy index-widget implementation, add a proxy
model, or retain list and tree implementations together.

The accepted replacement is one `QTreeWidget` hierarchy with stable items keyed
by presentation lifetime and one full-row delegate. Each item is the sole Qt
projection of its thread or pre-admission draft and retains the exact action
`NodeRef`; draft admission updates that same item by creation correlation. One
derived key-to-item lookup accelerates exact updates but owns no semantic fact.
Native tree branches own disclosure hit testing and Left/Right behavior.
Structural reconciliation reuses items and changes only affected sibling spans;
content updates are exact-item operations. The existing runtime remains the
sole thread-page cursor/single-flight authority.

Local Qt 6.10.2 diagnostics and supported-Qt-6.6 source inspection disprove the
idea that the stock accessible tree is sufficient. Stock rows are flattened
under the Tree, expose no semantic parent/level, never report `collapsed`, and
their `Toggle` action changes selection rather than expansion. Qt 6.6 also
allocates a fresh row interface per query. The accepted accessibility boundary
therefore lazily wraps the real tree/model indexes by presentation identity,
projects direct semantic parents/children, selection/focus/geometry, mutually
exclusive expanded/collapsed state, and a real expansion action, and retires
the wrapper with the exact item. It stores no copied hierarchy, row value, or
proxy QObject tree. Item presentation uses custom roles so Qt cannot emit its
private flat-row child indexes as accessibility events; correct events target
the one semantic interface.

M-07 is narrowed by current-worktree evidence. Its relative-time claim is
**disproven** because the pane renders absolute local time or date, not an aging
relative label. Its missing pane-local pending gate is **superseded** by
`ClientRuntime`'s authoritative pending/repair/cursor-generation/repeated-cursor
and queued-load-more state; adding another latch would duplicate authority.
Root-only user sorting while children retain provider relation order matches the
documented contract and is **not a defect**. The verified M-07 remainder is the
manual flat rebuild/current-index restoration path, which this stage deletes.

The exact entry accounting is native production **36,252 CLOC**, native tests
**39,618 CLOC**, `ThreadPane.{cpp,h}` **1,158 CLOC**, and its focused test
**616 CLOC**. The existing seven-case focused executable passes Debug in 0.12 s
and exact-current Release in 0.128 s; these are correctness-only baselines.
Source accounting proves 5 row QWidgets plus one layout per loaded row. The
test-side real-graph profile now measures, across actual DPR
1.0/1.25/1.5/2.0: 320 rows at 2.7--2.8 ms adapter / 144.7--145.4 ms population
with 1,612 QWidgets, 320 index widgets, and 321 `QTextDocument`s; 1,280 rows at
14.6--14.9 / 593.5--606.6 ms with 6,412/1,280/1,281; and 10,000 rows at
385.8--391.8 / 6,844.3--6,984.2 ms with 50,012/10,000/10,001. The documents
are QLabel implementation documents, not Markdown state, but are still
unbounded retained objects. Exact semantic no-ops preserve model/pixels/focus
and cost 1.09--1.22, 5.12--5.37, and 49.7--50.7 ms respectively. One offscreen
pending row leaves the timer active and causes five full-viewport paints in
170 ms; the visible case also paints five times with a dirty region larger than
the affected row.

The profiler was added before production edits and is run directly in baseline
mode with `QT_QPA_PLATFORM=offscreen`, `QT_SCALE_FACTOR=<dpr>`, and
`codexui-nodegraph-thread-pane-ui-test <rows> <dpr> baseline`. Production
remains **36,252 CLOC**. The executable's test source is now **883 CLOC** and
native tests **39,885 CLOC**, +267 from stage entry; registering the matrix and
replacing implementation-coupled correctness assertions follows with the
renderer change.

The proven accessibility requirement widens the honest candidate range to
native production **−100..+180 CLOC** after the model-backed accessibility
implementation; this was an uncertainty range, not approval for growth. Native
tests were estimated at **+300..+750 CLOC** after deleting old topology/counter
assertions and adding the requested quantitative matrix. During implementation
the user explicitly approved a separate **+1,600 production-CLOC H-11
ceiling**, superseding the earlier positive-growth stop; the completed H-09
allowance is not reused. Acceptance requires
320/1,280/10,000-row profiles at actual DPR 1.0/1.25/1.5/2.0, zero index
widgets or row documents, N-independent QWidget residency, row-local visible
animation and zero offscreen animation work, stable item/accessibility identities,
semantic tree parents/levels/states/actions, native keyboard navigation,
selection/context/promotion/reincarnation correctness, scroll-anchor and focus
retention, and Debug/Release/sanitizer verification. Reduced-motion policy
remains H-14 unless it can be obtained by deletion in this stage; real AT-SPI
and native XCB/Wayland remain S8 qualification.

**Final H-11 result and accounting (2026-09-16).** The exact original H-11
entry was native production **36,252 CLOC**, native tests **39,618 CLOC**,
`ThreadPane.{cpp,h}` **1,158 CLOC**, the focused ThreadPane test **616 CLOC**,
`ShellIntegrationTest.cpp` **4,767 CLOC**, `EstablishedUiUxTest.cpp` **615
CLOC**, and root `CMakeLists.txt` **696 CLOC**. Before H-11 paused for M-34, its
partial header and profiler work had already moved those totals to production
**36,209** and tests **39,885**. M-34--M-37 then account for production **-6**
and tests **+1,407**, reaching the separately recorded post-M-37 checkpoint of
**36,203 / 41,292**. This reconciles, rather than mixes, original entry,
intervening work, and resumption accounting.

The final worktree is native production **36,460 CLOC** and native tests
**41,979 CLOC**. Isolated H-11 is therefore production **+214 CLOC** and tests
**+954 CLOC** across its pre-pause and resumed work: `ThreadPane.{cpp,h}` is
**1,380 CLOC** (**+222**), Shell removes **8** production CLOC, the focused
test is **1,529 CLOC** (**+913**), ShellIntegration is **4,809 CLOC** (**+42**),
and Established UI/UX is **614 CLOC** (**-1**). The same result viewed only
from the post-M-37 resumption checkpoint is production **+257** and tests
**+687** because the earlier **-43/+267** partial H-11 work was already present.
Root CMake is **736 CLOC** (**+40 configuration CLOC**) after durable functional
and quantitative DPR registration. The physical H-11 diff is production
**+1,183/-933 lines**, tests **+1,528/-528 lines**, and root CMake **+42/-2
lines**. Production finishes 34 CLOC above the +180 estimate but 1,386 below
the explicitly approved +1,600 ceiling; clarity and Qt lifetime safety were
not compressed to fit an obsolete estimate.

The final mechanism audit finds no new cache, retry, polling loop, row widget,
row document, proxy model, dynamic functional property, or parallel renderer.
The existing single animation timer is retained but now runs only for visible
animated items and invalidates exact row rectangles. One typed
`selectionDispatchPending` bit coalesces the final native current-index change
after Qt completes its transition, replacing unsafe synchronous re-entry. One
typed sort-action association replaces incidental QObject child ordering. The
tree-local `QProxyStyle` changes only `SE_TreeViewDisclosureItem`, the public
rectangle consumed by QTreeView's own expand/collapse interaction; it restores
the exact former 24 x 24 target and adds no mouse handler or forwarding path.
Stable item/accessibility identity and one derived presentation-key lookup
replace the deep snapshot, expansion set, optimistic vector, and per-row
widget topology.

The exact pre/post ThreadPane raster under the application and tree style
sheets has **AE 0 / RMSE 0** (every pixel identical); an independent capture
also has the same SHA-256 before and after. Root and nested mouse-boundary tests
prove both axes of the restored native target, and four no-argument functional
CTest registrations execute those checks at DPR 1.0, 1.25, 1.5, and 2.0.
Release, Debug, and ASan/UBSan each pass the complete **18/18** H-11 matrix;
the entire Release suite passes **54/54**. The full `codex-ui` application and
the three H-11-dependent executables build in Release, Debug, ASan/UBSan, and
the warning configuration. The warning build introduces no warning in the
changed ThreadPane/focused sources; existing aggregate-initializer warnings in
other current-worktree production/tests remain classified for the later
repository-wide warning stage.

At 10,000 rows across DPR 1.0/1.25/1.5/2.0, the final Release profile measures
adapter **23.580--27.518 ms**, population **14.523--17.550 ms**, and semantic
no-op **3.922--5.675 ms**, versus baseline **385.8--391.8 ms**,
**6,844.3--6,984.2 ms**, and **49.7--50.7 ms** respectively. Every final
profile has **18** fixed/populated widgets, **0** index widgets, **1** fixed
document, **0** no-op paints, stable no-op pixels/focus/anchor, **0**
offscreen animation paints with no active timer, and **5** bounded visible-row
animation paints. Real AT-SPI and native XCB/Wayland breadth remain explicit S8
qualification; they are not unfinished H-11 implementation.

### S3 read-only evidence reconciliation and dependency order

Three independent read-only traces agreed on the following implementation
order. The primary agent reconciled these results before the first S3
production edit.

1. **H-05 projection boundary first.** Shell currently has four commit paths:
   bounded card updates, prompt materialization, tail append, and general row
   change/removal with a Shell-owned dependency DAG and snapshot fallback.
   The adapter exposes the matching four projection APIs, while the full
   snapshot repeats prompt alias, root, active-turn, and placement policy. The
   accepted replacement is one delta value crossing the graph-lock boundary,
   with ordinary presentation updates and canonically ordered structural
   changes projected by one adapter entry point. `ConversationView` applies
   that value through one public delta transaction; append is an internal
   geometry shape, not a second public presentation contract. Shell retains
   only coalescing and the measured eight-presentation-rows-per-pass schedule.
   A full `conversation()` snapshot on every delta was rejected because it
   would scan/copy retained history and invalidate the 10k-row append/index
   gates. Expected production effect: −150..−350 CLOC; tests neutral to +120.
2. **M-05 is an independent deletion after H-05.** Shell's runtime-stopped
   callback is overwritten by `main.cpp` and Shell destruction can then erase
   the application-owned callback. Keep the application-lifetime registration
   and delete Shell's registration and clear. Expected production effect:
   exactly −7 CLOC; test +18..+25.
3. **H-04/M-06 history operation authority.** There are currently three
   pending authorities: `ConversationView::HistoryWindow`, Shell's
   `historyPageAwaitingProvider`, and `ClientRuntime::pendingHistoryLoads`,
   while a live `Operation(method=thread/turns/list)` already spans admission
   through result/error and targets the exact Thread. The accepted direction
   derives pending from that Operation, deletes both extra guards, and changes
   the view's requested limit into one demand watermark so a failed retry does
   not add another page. Exact single-flight enforcement remains worker-owned.
4. **H-06/M-04/M-09 optimistic creation.** `WorkerLogic` atomically admits a
   local Thread/Turn/Prompt and later migrates that graph-owned prompt to the
   canonical thread; ThreadPane and Shell then duplicate the same admitted
   lifecycle with an overlay, correlation state, full-viewport animation, and
   zero-delay selection retries. Keep only the pre-admission authored draft in
   Qt. After admission, adapter rows derive pending/failed state from the graph
   prompt. Stable row/focus identity must use the existing creation
   correlation across local-to-canonical promotion; no second lifecycle owner
   or polling timer is accepted. Exact identity shape remains a substage gate
   until the current thread projection is traced end to end.
5. **H-07 request policy after projection/operation cleanup.** Native method
   classification exists in both Shell and adapter; Inspector direct-accept
   helpers and dialog decision choices duplicate `PendingRequestPolicy`; Shell
   also duplicates response flattening. Extend the existing policy and one
   adapter request projection while deleting all local copies. Provider method
   and payload stay raw graph facts. Browser remains a separate renderer and
   will execute shared semantic fixtures. The provider-declared command
   decision mismatch (native accepts arbitrary values; browser filters known
   values) is unresolved and must be explicitly decided before that edit.
6. **M-01 deliberately remains unresolved.** Typed `UiEffect` is the normal
   path, but queue-full handling writes graph fallback facts and saturation
   tests prove those facts recover otherwise lost notice/selection delivery.
   Deleting either path now would lose behavior. S3 will not manufacture a
   synchronizer; one durable authority requires a later evidence-backed queue
   decision.

### Final S3 implementation and evidence reconciliation

| Finding | Deleted or consolidated authority | Final classification and verification |
|---|---|---|
| H-05 | Deleted the public card/prompt/tail/row projection fork, Shell-owned placement DAG and prompt acknowledgement copies. Full snapshots and exact changes now use adapter-owned placement; `ConversationView::applyConversationDelta` is the one delta boundary. | **Implemented.** Root replacement is preflighted and applied as one final section transition. Focused model/view tests cover reorder, append, removal, prompt morph, geometry, focus, selection, anchors and bounded eight-row delivery. |
| H-04 / M-06 history | Deleted Shell `historyPageAwaitingProvider` and runtime `pendingHistoryLoads`; retained one view demand watermark and the graph `Operation` relation as the in-flight fact. | **Implemented.** Admit/success/error/retry, pagination, busy state and anchor behavior pass the adapter, Shell and runtime suites. |
| H-06 / optimistic slice of M-04/M-09 | Deleted the post-admission ThreadPane overlay, duplicate Shell correlation lifecycle, full-viewport animation and zero-delay selection retries. Retained only the pre-admission draft and graph `creationCorrelation` identity. | **Implemented.** Local-to-canonical promotion, failure, reconnect, duplicate creation, selection and draft isolation pass. General focus ownership and general reduced-motion policy remain S6/S5 work. |
| M-05 | Deleted Shell installation/clearing of the runtime-stopped callback; application lifetime owns the one handler. | **Implemented.** Destruction and stop delivery are covered without a second callback owner. |
| H-07 | Deleted Shell/Inspector/dialog method maps, response flattening, direct-decision copies, raw command/URL bypasses, retained-response map and repeated command decision parsing within a submission. One native `PendingRequestPolicy` owns kind, review context, actions, controls and wire response. | **Implemented for the verified contract.** Review work is bounded by collection, depth, node and UTF-8 byte limits and fails closed. Result/error is an exact C++ variant and an exact TypeScript union. Native and browser execute the same request/action/response and boundary fixture cases. |
| H-07 request lifecycle | Deleted wire-ID interaction scanning and result-without-operation graph updates. All production completion paths now require the exact current `NodeRef`; success and failure are distinct updater transitions. | **Implemented.** Reused IDs, missing operations, stale identities, synchronous resolution, send failure, retained authored response and retry are exercised. No timer, poller, response cache or functional QObject property was added. |
| M-24 request slice | Replaced copied expected literals with schema-v4 shared data for 29 request cases, 35 submissions, 93 semantic checks per frontend and eight explicit boundary cases. Frontend-local suites retain honest non-parity names. | **Implemented for kind/action/response/bounding meaning.** Native/browser disclosure layout remains renderer-specific; identical disclosure pixels/entry formatting are neither claimed nor required. Broader lifecycle/platform/Unicode qualification remains S8. |
| M-01 | No safe deletion exists while bounded `UiEffect` delivery can saturate and graph fallback facts recover otherwise lost effects. | **Unresolved, carried to S6.** Adding a synchronizer or deleting recovery would violate the authority/durability invariant. |
| M-20 | Turn settings are an independent policy/serialization authority and were not changed under request remediation. | **Verified, carried to S5.** This is an explicit dependency correction, not a disappeared S3 finding. |

Repeated calls to the one request policy are classified as bounded ephemeral
re-evaluation, not parallel state or implementation. Keeping no cached policy
result prevents a stale second authority at response time. Inspector all-row
residency and repeated descriptor copying remain H-09/H-10/M-28 work in S5.
The shared fixture proves common safety meaning through actions and wire
responses; it intentionally does not assert identical native/browser detail
layout. Standard-MCP schema/content validation and exact file-change patch
context plus stale-revision gating remain unresolved correctness hypotheses
behind their existing explicit production-growth approval gates.

Final exact-current verification:

- `cmake --build /tmp/codexui-h07-build.IkJECO -j2`: success.
- Focused request/runtime/adapter/updater/worker/Shell CTest set: 6/6 passed.
- `ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -LE performance -j2`: 19/19 passed after exact-identity deletion.
- Final exclusive full Debug CTest run: 31/31 passed; all twelve registered quantitative conversation cases passed at 320, 1,280 and 10,000 rows and DPR 1.0, 1.25, 1.5 and 2.0.
- `npm test -- --runInBand`: 251/251 passed.
- `npm run profile`: 49.05/36.30/9.98 ms for hydrate/project/stream versus 45.56/35.31/9.73 ms at S3 entry. No browser presentation algorithm changed; the single-sample variation remains below 8% and is carried as measurement noise rather than claimed improvement.
- The final exclusive Debug conversation group took 82.60 s versus 80.40 s at S3 entry (+2.7%); every semantic and quantitative bound passed. A prior 574 ms no-op outlier reproduced only under competing benchmark processes while reporting zero layouts/paints and semantic stability. `RUN_SERIAL` covers one CTest scheduler, so final performance qualification uses a shared external lock; thresholds were not weakened and production was not patched for host descheduling.
- `git diff --check`: clean. Current Release, ASan/UBSan, native XCB/Wayland, real assistive-technology and warning qualification remain explicitly assigned to S8 rather than falsely claimed here.

These conclusions preserve lazy materialization: widgets remain bounded to
the viewport and overscan, offscreen rows remain model data plus scalar
heights, and performance is measured before and after each substage. What is
being removed is parallel projection/application policy around that residency,
not residency itself.

### S3 Xvfb/xcb requalification

**Reopened after explicit user-requested platform qualification; implementation
complete and final matrix in progress.** The earlier S3 completion relied on
CTest properties that force `QT_QPA_PLATFORM=offscreen`; it therefore did not
qualify the real `xcb` plugin and was an incorrect completion claim. A first
direct eight-executable bare-Xvfb run passed five executables, failed Copy/focus,
residency/anchor and prompt-focus assertions, and aborted the virtualization
executable in `QWidget::mapToParent`. GDB localized that abort to
`keyboardNavigationAndViewIsolation`, after its no-window-manager focus
precondition had failed. Repeating with `xfwm4` removed the activation/focus
failures and the abort, leaving two virtualization assertions and three Shell
routing assertions.

The current-worktree evidence classifies those remaining failures as test
authority/lifetime defects, not production-renderer defects. The paging test
captured 15 tail residents before scrolling and then compared that obsolete
count with 16 legitimate head residents while staging. The residency test used
painted card geometry as a second residency authority; row 93 was correctly
retained because its scalar row extent, including inter-row space, crossed the
overscan boundary even though its card ended two pixels above it. The Shell
test captured its route baseline before the already-designed frame-coalesced
selection transaction published its final Inspector/chrome commit; after that
boundary, the background delta changes neither route. Finally, raw card/control
pointers survived a failed focus precondition and recycling, allowing a test
failure to suppress later cases.

The accepted correction adds no production mechanism. It moves the staging
baseline to the actual pre-stage frame, derives expected residency from the
existing scalar row extent rather than painted appearance, observes recycled
widgets through `QPointer`, and records Shell routing only after the selected
conversation's existing commit boundary. No tolerance, offset, timer, cache,
flag, renderer, production event suppression, or reconciliation path is added.
Production effect is **0 CLOC**; test effect is **+10 CLOC**. The bare-Xvfb
test now reports all later failures instead of crashing. The corresponding
offscreen virtualization and Shell tests pass, and the corrected
Xvfb/xcb+`xfwm4` tests pass individually.

Required final verification is: all 19 non-performance CTests without
short-circuiting, all twelve quantitative 320/1,280/10,000-row DPR
1.0/1.25/1.5/2.0 gates, and all eight QWidget executables in one
Xvfb/xcb+`xfwm4` session. Accessibility/focus cases are part of that xcb run;
the benchmark must show unchanged bounded widgets/documents/measurements and
per-frame work. Bare Xvfb without a window manager remains an intentionally
recorded invalid focus-activation environment, not a substitute for the full
xcb desktop-like gate. S3 remains **in progress** until those final commands
pass and the diff is audited.

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
