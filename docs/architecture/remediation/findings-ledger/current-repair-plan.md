<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../../media/nav/banner-findings-ledger-compact.svg"><img src="../../../media/nav/banner-findings-ledger.svg" alt="CodexUI — Findings ledger" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Findings ledger"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/up-findings-ledger-dark.svg"><img src="../../../media/nav/up-findings-ledger-light.svg" alt="Up to Findings ledger" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/organization-dark.svg"><img align="right" src="../../../media/nav/organization-light.svg" alt="SNodeC organization" width="160" height="28"></picture></a><a href="../../../../README.md#project-overview" title="CodexUI repository"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/repository-dark.svg"><img align="right" src="../../../media/nav/repository-light.svg" alt="CodexUI repository" width="160" height="28"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Qt architecture remediation findings ledger

## Current verified repair plan — 2026-09-20

This checkpoint implements the replacement plan accepted after the review of
`6a71db068b9a593b24c9ae638fbec9016e76376b`. The older entries below remain
historical evidence, not additional authorization to expand this plan.
The original implementation baseline was clean `6a71db0`. Step 2 starts from
clean `eb6d56d` on `master`, two commits ahead of `origin/master`.

| Step | Accepted correction | Status |
|---|---|---|
| 1 / NEW-1 | UTF-8-safe byte bounding at the generic-activity adapter boundary | Completed; broader performance qualification remains open below |
| 2 / D-6 | Copy removes generated placeholders only, preserving authored U+200B | Completed and verified; Copy cost and separate DPR-suite limitation recorded below |
| 3 / M-4, D-4a | Model-backed accessible conversation identities and plan statuses | Completed and offscreen-verified within approved growth; native screen-reader qualification remains separate |
| 4 / NEW-2, D-4b, D-3, WEB-1, T-1–T–3 | Shared native/browser detail, Copy, label and Markdown contracts | Completed and locally verified, including the separately authorized AISuite receive-bound correction; broader qualification remains separate |
| 5 / L-1 | Allocation-free scalar height updates and justified exception specifications | Completed; allocation-failure proof, consumer/DPR/performance gates and ASan/UBSan pass |
| Cleanup | NEW-6–NEW-8, L-4–L-6, L-8: bounded test/documentation/code cleanup | Pending |
| Qualification | CI-1–CI-4, T-5/NEW-5, SHELL-1: coverage and measured performance | Pending; native-platform checks remain environment-constrained |

The user explicitly restored this priority order: item 3 follows the completed
D-6 correction, not further performance work. Commit `6056570` contains NEW-1; `eb6d56d`
contains the previously authorized targeted thread-row deadline correction.
Other SHELL-1 measurements remain under qualification, not an additional task.

Candidate findings remain candidates: no speculative renderer, virtualization,
treap, lifetime, scheduler or graph rewrite is authorized. In particular H-2
has no reproduced section-range mismatch (800 comparisons passed); the broad
D-9/T-4 atomicity allegation is contradicted by prevalidation and passing
tests; M-5 revisits missing visible rows; NEW-5 did not increase actual shell
minimum width in the styled probe. D-5 stays withdrawn. Keep existing ownership
guards, useful diagnostics, incremental update paths and visual behavior.

### Separately authorized C-1 — projection-task lifetime (2026-09-21)

- **Authorization and baseline:** after reviewing Claude's C-1 against clean
  `7a89045`, the user explicitly requested the reviewed fix, including its
  quantified **+2 production CLOC**. This is a separately requested correction;
  the numbered backlog above and the unresolved item-4 gate are unchanged.
- **Verified invariant violation:** copied `NodeGraphUiAdapter` instances in
  both shell pool jobs borrow the session's graph. `alive` only invalidates
  queued UI continuations; it does not extend graph lifetime. `main()` joins
  the client worker, destroys the window, then destroys the session/graph.
  Global-pool projection jobs have no join before that graph destruction.
  Conversation snapshots/deltas and Inspector projection are both affected.
- **Review corrections:** in Qt 6.10.2, `QCoreApplication` itself joins/deletes
  the global pool, not only static destruction after `main`; it still runs
  after graph destruction here. `QThreadPool` destruction already joins its
  jobs, but an explicit shell-teardown join makes the boundary clear. Waiting
  does not deliver queued UI callbacks; keep the existing token invalidation
  first. A single-thread limit is not part of the fix: existing per-category
  in-flight guards already coalesce requests, and serializing Inspector behind
  conversation could introduce unnecessary latency.
- **Replacement and ownership:** add one `QThreadPool projectionPool` member;
  immediately after `alive=false` in `~Impl`, wait for that pool; replace both
  global-pool dispatch sites with the member. This removes unowned execution
  for graph readers without moving projection back to the GUI thread or
  introducing shared graph ownership. Merely checking the token in the worker
  would leave a check/use race. Waiting on the global pool would instead tie
  shell shutdown to unrelated Git/image jobs. No new flag, timer, callback,
  cache, renderer, protocol state or concurrency limit is introduced.
- **Documentation:** updated `codex-architecture.md` and
  `two-thread-shared-node-graph.md` to describe the actual single writer,
  UI/pool readers, session-before-shell lifetime requirement, queued result
  delivery and shutdown join. Git/image work remains separate.
- **Deterministic regression proof:** a standalone executable reuses the
  existing shell integration fixtures and links the actual production
  libraries. Test-only linker wrappers around Qt pool dispatch/join pause
  the two real projection runnables before their adapter work. Cases cover
  both runnables running, one running with the second queued, and completed
  work whose UI continuations have not yet been delivered. A controller
  releases them when teardown reaches its join or incorrectly returns;
  there is no timing guess about when a large projection happens to run.
  The one-thread limit in the queued case is confined to this probe.
- **Before/after result:** the pre-change executable returns from destruction
  with **0/2 jobs finished** in both outstanding-work cases and fails four
  lifetime assertions (exit 1). It keeps the graph alive for cleanup rather
  than deliberately causing a use-after-free. The same probe object linked
  against the corrected shell returns with **2/2 finished** in all three
  cases (exit 0). It then destroys the graph and explicitly delivers queued
  results; both Debug and ASan/UBSan versions pass without a stale UI access.
  The already-queued-result case also passes before the fix, confirming that
  the existing callback token was not the defective ownership boundary.
- **Probe reproducibility:** source/build driver/binaries and logs are under
  `/tmp/codexui-c1-lifetime.v30ZwE/`. Build with
  `node /tmp/codexui-c1-lifetime.v30ZwE/build.mjs <build-directory> <binary>`;
  the driver reuses the target's actual compile/link commands and production
  libraries. `link-only <existing-probe-object>` allows the same probe object
  in a before/after comparison. Run as `xvfb-run -a env
  QT_QPA_PLATFORM=offscreen timeout 15s <binary>` (30 s for sanitizers).
  `before.log`, `after.log` and `sanitized.log` record all three cases.
  The wrappers remain outside production and the permanent test suite.
- **Shutdown latency:** controlled Debug cases took **2.443 / 1.960 / 1.563
  ms** after the fix; ASan/UBSan **6.026 / 4.916 / 2.405 ms**. These small
  fixture measurements are not a bound for large histories: teardown waits
  for remaining finite task work. Tasks must not wait for a blocking UI
  callback; returning after a timeout would not solve graph ownership.
- **Build and regression commands:** `cmake --build
  /tmp/codexui-current-debug.fMMvCu --target codex-ui
  codexui-shell-integration-test codexui-application-layout-test -j14` passes;
  shell integration also builds with `-j14` in
  `/tmp/codexui-current-asan-ubsan.2oIJr4`. Normal integration command:
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu -R '^codexui-shell-integration$'
  --repeat until-fail:3 --output-on-failure -j14`. All three baseline runs
  pass (**8.39 / 8.33 / 8.40 s**); all three fixed runs pass (**7.96 / 7.95 /
  8.47 s**). Existing heartbeat, staged presentation, thread switching,
  paging and frame-work checks remain unchanged. Logs:
  `/tmp/codexui-c1-{baseline,after}.log`. Suite times do not prove every
  interaction became faster.
- **Sanitizer integration:** `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  ctest --test-dir /tmp/codexui-current-asan-ubsan.2oIJr4
  --output-on-failure -j14 -R '^codexui-shell-integration$'` passes **1/1**,
  **11.87 s**, with no sanitizer diagnostics or new suppressions. Log:
  `/tmp/codexui-c1-sanitizers.log`. All Debug/sanitizer Xvfb wrappers exit 0;
  these are offscreen checks, not native-compositor qualification.
- **Accounting:** `ShellWidget.cpp` **2,768 → 2,770 production CLOC (+2)**,
  **2,913 → 2,915 physical lines (+2)**. Permanent test-code delta **0**;
  existing tests and thresholds are unchanged, with focused instrumentation
  confined to the standalone probe. Documentation is accounted separately.
  Final diff review and `git diff --check` pass. C-1 is complete for the
  reviewed graph-reader lifetime invariant. No commit, push, install,
  process restart or upstream edit was performed in this stage.

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

### Step 2 approved implementation gate

- The user approved approximately **+60–100 net production CLOC** after
  deletion, including the demonstrated append-boundary dependency. Subsequent
  approval allows collision-checked import tags only as source-origin metadata,
  not a character-removal heuristic or a change to Markdown semantics.
- Remove blanket U+200B deletion and the separate HTML-to-ODF reparse. Export
  all formats from one selected fragment cleaned using document provenance.
  Ordinary and prepared Markdown must preserve authored characters unchanged.
- Record generated characters and generated portions of rich-text attributes
  during clipboard preparation. Use the existing Qt parser, not another parser.
  No retained secondary renderer, timer, cache, or reconciliation mechanism.
- Correct a source/document tail pair only where the existing last-block
  assumption is not justified. Preserve the independently reparsable tail path.
- Baseline at `eb6d56d`: presentation .cpp/.h **883 production CLOC**;
  cards test **4,623 CLOC**, virtualization test **6,828 CLOC**. Existing cards
  and virtualization suites passed (11.02 / 7.76 seconds), via
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu -R
  '^(codexui-conversation-cards|codexui-conversation-virtualization)$'
  -j14 --output-on-failure`. Xvfb wrapper now succeeds as well.
- Required verification: authored literal/entity characters, partial/full
  selections, plain/HTML/Markdown/ODF agreement, links and image attributes,
  prepared and incremental content, unchanged geometry/selection/document
  identity, all four DPRs, and construction/Copy cost against the baseline.
  Prototype evidence alone is not completion.

### Step 2 implementation — export owns provenance work

- The first implementation annotated the live document during `setContent`.
  Measurement rejected that placement: 21,000-unit prompt construction rose
  from 27.18 to 35.87 ms for plain text, and 27.29 to 53.73 ms for mixed
  authored U+200B. **That implementation has been removed**, not retained as
  a compatibility path. Provenance is needed for export, not painting.
- The accepted implementation does no origin parsing/annotation during
  construction, materialization, scrolling, resizing, or streaming. Canonical
  source remains the authority. When exporting a user-message selection,
  the existing projection with/without generated blanks establishes whether
  cleanup is needed. Other selections use Qt's unchanged native exporter.
- If source has neither an authored U+200B nor an entity introducer, every
  projected U+200B is provably generated. Otherwise, import tags are checked
  against source, parsed text and format properties, including decoded
  entities. A collision chooses another checked token. The same Qt Markdown
  importer runs once in a temporary, layout-disabled document to recover
  origins; tags are normalized before position mapping to the selection.
- Only the copied fragment gets two `QTextFormat::UserProperty` annotations:
  generated characters and clean authored rich-text attribute values. Neither
  is a QObject dynamic property or retained widget/model state. Remove the
  identified characters and apply attribute values once, then produce plain
  text, HTML, Markdown and ODF from that one fragment. No HTML-to-ODF reparse,
  second renderer, new timer, callback, cache or live-document mutation.
- The tail correction maps a whole mutable source paragraph to document
  position zero, not its last Qt block. For non-initial tails containing line
  breaks or numeric entities, Qt's fragment importer establishes whether the
  tail really occupies one block. Multi-block tails use the existing full
  replacement path: entity-created block margins are context-dependent (the
  probe reproduced equal text but a 6 px height discrepancy after splicing).
  The ordinary single-line tail remains unchanged; ambiguous tails incur one
  additional tail-only parse to establish eligibility, not a geometry guess.
- New checks exercise literal/entity U+200B, forced literal/entity import-tag
  collisions, full/partial selections, bold text, inline code, links/images,
  blank lines, prepared Markdown, unchanged live revision/selection, and
  repeated incremental-vs-fresh updates. ODF expectations are independently
  cleaned rich fragments; the former assertion that reparsed Copy's own HTML
  is removed because it prescribed the superseded implementation.
- Temporary provenance documents scale with prompt size **on Copy** for the
  ambiguous-source case. This is not a constant-time Copy claim. Long-copy
  timings and limitations must remain visible with the final results.

### Step 2 verification and accounting

- Built `codex-ui`, `codexui-conversation-cards-test`,
  `codexui-conversation-virtualization-test`, `codexui-inspector-graph-test`,
  and `codexui-conversation-view-benchmark` with
  `cmake --build /tmp/codexui-current-debug.fMMvCu --target <targets> -j14`.
  Final repository build is clean under its `-Wall -Wextra -Werror` flags.
- Final command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu -R
  '^(codexui-conversation-cards|codexui-conversation-virtualization|codexui-inspector-graph|codexui-conversation-performance-.*)$'
  -j14 --output-on-failure`. **15/15 passed**, wrapper exit 0, 131.33 s.
  Cards: 11.12 s; virtualization: 7.53 s; Inspector: 0.59 s. All 12 registered
  performance gates passed (320/1,280/10,000 rows, each at four DPRs), including
  immutable Markdown prefix/selection retention and bounded residency/work.
  Log: `/tmp/codexui-d6-accepted-verification.log`.
- Focused public-widget/clipboard checks pass at DPR **1, 1.25, 1.5, 2**:
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=<dpr>
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough CODEXUI_MARKDOWN_SELECTION_TESTS=1
  /tmp/codexui-current-debug.fMMvCu/codexui-conversation-cards-test`.
  This filter runs the existing line-break case and the new authored-character
  case; both also remain in the unfiltered suite. Each wrapper exits 0.
  Logs: `/tmp/codexui-d6-export-dpr-<dpr>.log`.
- **Existing CI-2 qualification limitation, not a new remediation item:** the
  full cards suite passes at DPR 1 but fails its unchanged thumbnail assertion
  at 1.25/1.5/2. That assertion compares physical `QPixmap` dimensions with
  logical 280×180 limits although production sets the pixmap DPR. The same
  failure reproduces at DPR 2 in the untouched September 19 executable
  `/tmp/codexui-protocol-debug/codexui-conversation-cards-test` (exit 1).
  Logs: `/tmp/codexui-d6-cards-dpr-<dpr>.log` and
  `/tmp/codexui-d6-predating-cards-dpr-2.log`. Do not report full-suite
  four-DPR qualification as passed; no image code or assertion was altered.
- Copy measurements use actual public `MarkdownTextView::copy()` and request
  all four MIME formats, including ODF. Median of five runs, source lengths
  210 / 2,100 / 21,000 UTF-16 units: **before 878 / 7,399 / 73,857 us;
  final 712 / 6,436 / 72,608 us**. These are local measurements, not a claim
  of universally faster or lag-free interaction.
- Partial selections expose the cost explicitly: selecting a normal word in
  the 21,000-unit mixed-source sample takes **1,224 us** (before 79 us).
  A selection actually containing the ambiguous authored U+200B takes
  **21,262 us** (the defective baseline took 116 us and deleted that character).
  The no-candidate short-circuit avoids full origin import for ordinary words.
  There is no retained cache hiding this source-sized work on ambiguous Copy.
- Construction medians (seven runs) for the 21,000-unit plain and literal
  samples and 27,000-unit entity sample: **before 27,183 / 27,287 / 34,466 us;
  after relocating origin work to Copy 27,297 / 27,521 / 34,443 us**. These
  samples have whole-source mutable paragraphs, so the subsequent tail-only
  eligibility guard does not add a parse. No Copy-origin work remains in
  construction, scrolling, or ordinary streaming.
- Standalone diagnostic sources/binaries and CSV/JSON outputs use the
  `/tmp/codexui-d6-{selection,construction,partial-copy}-*` names. They are
  built with `g++ -std=c++20` against the production library and Qt6Widgets;
  the construction/partial baseline compiles the original presentation sources
  taken from `git show HEAD:src/codex/middle/ConversationPresentation.{h,cpp}`
  in `/tmp/codexui-d6-baseline`. All executions use Xvfb/offscreen. Standalone
  probe compilation reports the already-observed GCC16/Qt-header SFINAE
  warning; the repository build does not. No native compositor, screen reader,
  older Qt version, or Release qualification is claimed by this step.
- Final accounting against `eb6d56d`: **production +100 CLOC / +106 physical
  lines** across presentation .cpp/.h (883 → 983 CLOC); **tests +281 CLOC /
  +287 physical lines** (4,623 → 4,904 CLOC). Documentation is separate.
  Added mechanisms are bounded temporary export provenance and conservative
  parser-backed tail eligibility; no renderer, persistent state, timer, cache,
  callback, QObject property, or upstream change was added. The superseded
  blanket cleanup and ODF HTML reparse are deleted. `git diff --check` passes.
  Changes remain uncommitted; no processes were installed/restarted or pushed.

### Step 3 investigation and proposed addition gate — M-4 / D-4a

- **Checkpoint:** at the user's explicit request, committed D-6 and its
  verification record as `a14ad23` (`Preserve authored Unicode in Markdown
  selection Copy`). The worktree was then clean. No push, installation,
  upstream change or process restart. This investigation changes documentation
  only; the diagnostic source lives outside the repository.
- **Invariant:** loaded logical conversation rows and selection must remain
  accessible independently of renderer residency. A surviving semantic key
  keeps its accessible identity when scrolled away and back. Existing controls
  remain the only implementation of their actions and text interaction.
- **Exact path:** graph projection -> `ConversationItemModel` rows/stable keys
  -> `ConversationView` residency -> `ConversationAccessible::physicalChildren`.
  The last step incorrectly makes visible `ConversationCard` children the
  item authority. `itemForIndex`, selection lookup and row-event dispatch also
  require resident widgets. `releaseCard` deletes a widget; Qt deletes that
  widget's cached accessible interface although its model row survives.
- **Local reproduction, Qt 6.10.2, Xvfb/offscreen:** 200 model rows, 9 resident
  cards, only **5 accessible rows**; scrolling to the opposite end changes the
  accessible count to **6**. Selecting an offscreen model row reports one model
  selection but **zero accessible selections** without moving the viewport.
  The original row key survives, but its accessible ID is deleted and differs
  when returning to that row. Enumeration itself constructs no card widgets.
  One thousand complete enumerate/index-back traversals of this small exposed
  window took **251,557 us**. This is a diagnostic baseline, not a scrolling
  benchmark or a claim about native screen-reader behavior.
- **D-4a reproduction:** changing a structured plan step from Pending to
  Completed produces the identical model accessible string
  `Plan\n\nsame step`. `accessibleCardText` appends `step.text` but not
  `step.status`. Qualification: current graph conversation projection produces
  legacy-text `PlanData`; structured Inspector steps take a different path and
  already expose their status description. This demonstrates the structured
  conversation/model omission, not a new claim that the Inspector tab loses
  its statuses.
- **Existing authorities to retain:** model rows, stable-key lookup and exact
  structural notifications; view-owned folding/visibility, scalar geometry,
  selection model and navigation; `UiStatus`/`displayStatus`; the real card
  controls and native Qt text interfaces. No graph lock belongs in this layer.
- **Rejected deletion-only alternative:** Qt's built-in item accessibility
  uses model-backed cells, but `QAccessibleTableCell` has zero children and the
  reusable implementation is a private Qt header. Using it unchanged does not
  preserve rich card controls; inheriting private Qt internals adds a version
  dependency. Qt's cache associates a widget interface with that widget's
  lifetime, so returning one reused interface for replacement widgets is not
  a valid identity solution either.
- **Proposed replacement:** enumerate model rows (plus the existing native
  history/empty/loading chrome), and lazily register logical row interfaces by
  semantic key. Keep only interface identity/lifetime bookkeeping, not copies
  of row payloads, geometry, folding or selection state. Offscreen queries read
  the model and scalar geometry without materializing widgets. Folding and
  filtering remain explicit hidden/unavailable states, distinct from being
  outside the viewport. Selection alone does not scroll; an explicit focus/
  reveal action uses the existing view navigation and residency path.
- A resident card supplies its existing native control subtree beneath the
  logical row, with its outer accessible wrapper acting only as a container,
  not a second ListItem/selection authority. Do not duplicate Copy, disclosure,
  text selection or link implementations. Preserve parent/child symmetry,
  native control IDs and exact event targets. Actual row removal/thread
  replacement retires old identities; no stale handle may resolve to another
  thread's row. Scroll recycling must not retire logical identities.
- **Delete in the same stage:** resident-card scanning/sorting for list
  enumeration, resident-only index/selection lookup and event guards, and
  assertions requiring absent offscreen rows/selections. Retain checks for
  real controls, no-op events, disabled interactions, loading/history chrome,
  text selection and bounded widget residency. Do not weaken those checks.
- **Addition gate:** deletion cannot supply a stable identity after its widget
  dies. A narrowly scoped logical-interface registry and lifecycle/event
  integration are required. Estimated **+150–300 net production CLOC** after
  deleting the resident-only path; test replacement/extension estimated
  **+150–300 net test CLOC**. These are estimates requiring approval, not
  completed accounting. No new renderer, timer, graph-state cache, global
  focus hook or second geometry/interaction implementation is proposed.
- **Baseline accounting:** view .cpp/.h **3,451 production CLOC**, item model
  .cpp/.h **1,743 production CLOC**; virtualization test **6,828 CLOC**, model
  test **1,174 CLOC** (`cloc --hide-rate --by-file <files>`).
- **Baseline verification:** build model/virtualization targets using
  `cmake --build /tmp/codexui-current-debug.fMMvCu --target
  codexui-conversation-item-model-test codexui-conversation-virtualization-test
  -j14`; then `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu -R
  '^(codexui-conversation-item-model|codexui-conversation-virtualization)$'
  -j14 --output-on-failure`: **2/2 pass**, 0.34 / 7.67 seconds, wrapper exit 0.
  The existing resident-tree case explicitly encodes the faulty contract; its
  passing result is not evidence against the probe.
- **Diagnostic:** `/tmp/codexui-item3-a11y.7zKOZH/probe.cpp`, built with
  `g++ -std=c++20 -g -fPIC -Isrc <probe.cpp> <conversation-ui.a>
  <conversation-height-index.a> <nodegraph.a> <ui-style.a>
  $(pkg-config --cflags --libs Qt6Widgets) -o <probe>` against the current
  Debug libraries, executed with `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  /tmp/codexui-item3-a11y.7zKOZH/probe` (exit 0). Standalone compilation has
  the previously recorded GCC16/Qt-header SFINAE warning; repository build is
  clean. An initial probe namespace typo failed compilation; that attempt
  supplied no runtime evidence and was corrected before the successful run.
- **Required implementation verification:** logical counts independent of
  residency; stable IDs through scroll, insertion, moves and optimistic
  promotion; safe removal/reset/thread switching; folding/visibility states;
  exact selection/focus events and semantic no-ops; one real control subtree;
  bounded text/status summaries; history/empty/staging behavior; destruction
  with outstanding accessible references. Measure queried-interface memory,
  traversal cost, construction/parse/measurement counts and incoming-update
  work, and rerun the registered performance gates and relevant DPR checks.
  Xvfb/offscreen interfaces do not qualify a native screen reader; retain that
  explicit limitation under the existing qualification item.
- Qt evidence: [6.10.2 item interfaces and private-header contract](https://github.com/qt/qtbase/blob/v6.10.2/src/widgets/accessible/itemviews_p.h),
  [widget-interface cache ownership](https://github.com/qt/qtbase/blob/v6.10.2/src/gui/accessible/qaccessiblecache.cpp),
  [public accessibility interface](https://doc.qt.io/qt-6/qaccessibleinterface.html).
  At this investigation checkpoint, no item-3 production/test implementation
  had been applied. Only the existing plan item is being advanced.

### Step 3 implementation gate

- The user approved the estimate and required stopping/reporting before
  exceeding it: at most **+300 net production CLOC**; estimated test growth
  is separately tracked against **+300 net test CLOC**.
- Replace resident-only enumeration with logical interfaces; keep only a lazy
  stable-key-to-accessible-ID registry, owned by the viewport interface. Native
  card wrappers become non-semantic containers for the unchanged controls.
  Scope model signal connections to a member QObject whose destruction closes
  them; retire exact removed keys and clear the registry on model replacement.
  No widget/document construction is permitted from accessibility queries.
- A first lifecycle check exposed that explicit accessible navigation must
  declare user scroll intent before height admission, or Following restores
  the bottom. Applying that policy to every programmatic `scrollTo` broke the
  existing command-detachment test and was rejected. The replacement keeps
  one reveal calculation with an explicit call-time user-initiated argument:
  Qt/programmatic callers retain their previous semantics; accessible focus
  uses the existing user-scroll policy. This is not a retained flag, second
  scroll controller, timer or geometry correction.
- The initial optimistic-promotion diagnostic incorrectly used a presentation
  update for a kind change (then an exact delta without a graph target). Those
  are correctly rejected by production contracts. Verify promotion using the
  authoritative reconciliation boundary; do not weaken admission validation.
- Compilation and intermediate failing checks are development evidence, not
  completed verification. Final results and accounting follow only after the
  required checks succeed.

### Step 3 result — logical conversation accessibility

- **Implementation:** replaced resident-widget enumeration/sorting and
  resident-only selection/event lookup with lazily registered logical rows.
  Stable keys identify interfaces; the model remains the content/order
  authority, the view retains scalar geometry and folding, and Qt's selection
  model retains selection. A resident card is only a native control container
  under its logical row. No offscreen Copy/disclosure/Markdown implementation
  was added. Typed plan-step statuses now appear in bounded accessible text.
- **Lifetime and event review:** exact model removal/reset retires interfaces;
  recycling does not. A QObject connection context closes model callbacks with
  the viewport interface. Update notifications visit the changed range and
  only already-queried identities, not all logical rows. Description and
  visibility updates, selection/focus events, and no-op suppression are checked.
  Explicit accessible focus uses the shared reveal path with user intent;
  programmatic `scrollTo` preserves its prior command-output semantics. Local
  persistent model indices and QPointers protect action/update callbacks from
  using deleted identities. Destruction inside a current-index callback is
  covered under ASan/UBSan.
- **Correctness:** 2,000 loaded rows enumerate in order with stable IDs,
  without new card constructions, measurements or QTextDocuments. Real Copy
  and disclosure actions, parent/child symmetry, hidden staging, disabled
  interaction, native text focus/selection, history/loading/empty chrome,
  recycling, insertion, movement, exact removal, filtering, folding, optimistic
  promotion, thread replacement and owner teardown all pass. The full existing
  virtualization suite also retains command follow-tail/manual detachment,
  geometry, anchors, streaming and incremental admission checks. Plan summaries
  distinguish pending/running/completed and remain bounded on large payloads.
- **Build:** `cmake --build /tmp/codexui-current-debug.fMMvCu --target
  codex-ui codexui-conversation-item-model-test
  codexui-conversation-virtualization-test codexui-conversation-cards-test
  codexui-conversation-view-benchmark codexui-inspector-graph-test -j14`
  succeeds with repository warning/error flags, GCC 16 and Qt 6.10.2.
- **Final targeted CTest command:** `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  ctest --test-dir /tmp/codexui-current-debug.fMMvCu -R
  '^(codexui-conversation-item-model|codexui-conversation-virtualization|codexui-conversation-cards|codexui-inspector-graph|codexui-conversation-selection-focus-fusion-.*)$'
  -j14 --output-on-failure`: **8/8 pass**, 11.12 seconds; model 0.33,
  virtualization 7.86, cards 11.12, Inspector 0.60 seconds. Named cases retain
  non-short-circuit execution.
- **Sanitizers:** build the model/virtualization targets with `-j14` in
  `/tmp/codexui-current-asan-ubsan.2oIJr4`; run `xvfb-run -a env
  QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-asan-ubsan.2oIJr4 -R
  '^(codexui-conversation-item-model|codexui-conversation-virtualization)$'
  -j14 --output-on-failure`: **2/2 pass**, 1.07 / 15.33 seconds. Flags:
  `-fsanitize=address,undefined -fno-omit-frame-pointer
  -fno-sanitize-recover=all`; no new suppressions or leak-detection disablement.
- **Full virtualization DPR checks:** in addition to the DPR-1 CTest run,
  execute `xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=<dpr>
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough
  /tmp/codexui-current-debug.fMMvCu/codexui-conversation-virtualization-test`
  for **1.25, 1.5, 2.0**, all pass (including teardown during an action).
  Logs: `/tmp/codexui-item3-dpr-{1.25,1.5,2}.log`. All Xvfb wrappers exit 0;
  offscreen plugin `raise()` notices do not represent native compositor proof.
- **Quantitative gates:** `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest
  --test-dir /tmp/codexui-current-debug.fMMvCu -R
  '^codexui-conversation-performance-' -j14 --output-on-failure`: the first
  complete run passed **12/12**, 119.92 seconds. These registered timing cases
  deliberately retain `RUN_SERIAL` despite `-j14`, avoiding mutual timing
  interference. The final repeat after the callback-lifetime review also passes
  **12/12**, 118.79 seconds, wrapper exit 0.
- **Comparable standalone performance:** execute
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough
  /tmp/codexui-current-debug.fMMvCu/codexui-conversation-view-benchmark 1280 1`
  before/after this stage. Both pass all gates. Before -> after, microseconds:
  warm-input p95 **570 -> 503**; warm no-residency scrollbar p95
  **700 -> 556**; streaming input-to-paint p95 **7,665 -> 7,460**;
  normal-scroll p95 **8,135 -> 7,956**; command-stream p95 **943 -> 760**;
  following-tail append p95 **5,460 -> 5,480**. Peaks remain **31 cards,
  479 widgets, 29 documents**. Stream constructions remain zero, document
  changes remain 48/24, and no-op layout/paint counts remain zero. Sampled peak
  RSS **69,680 -> 69,592 KiB**. Single-run timings contain noise; these are
  bounded-regression observations, not universal lag-free guarantees. Final
  JSON: `/tmp/codexui-item3-benchmark-final.json`.
- **Accessibility-specific cost:** standalone
  `/tmp/codexui-item3-a11y.7zKOZH/logical-probe.cpp`, linked to current Debug
  conversation, height-index, nodegraph and style libraries with Qt6Widgets,
  run using `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  /tmp/codexui-item3-a11y.7zKOZH/logical-probe`. For 2,000 queried rows,
  first full traversal **8,219 us**, repeat **5,649 us**; `mallinfo2` allocated
  heap increase **674,720 bytes**, repeat **400 bytes**. This includes Qt's
  accessible cache/allocator effects, not an exact per-interface size. One
  thousand offscreen presentation updates plus summary-reading notifications
  take **35,369 us**, with exactly 1,000 logical description events and no new
  widgets/documents/measurements. Cache entries are bounded by queried live
  model rows and are all retired on replacement (2,000-ID lifecycle check).
  The old 1,000 traversals of a five-row exposed window are a different workload
  and are not claimed as a direct speedup comparison. Standalone GCC compilation
  reports the pre-existing Qt-header SFINAE warning; the repository build is clean.
- **Accounting against `a14ad23`:** production **+205 CLOC / +203 physical
  lines** (view .cpp 3,164 -> 3,362; .h 287 -> 290; model .cpp
  1,566 -> 1,570). Tests **+229 CLOC / +233 physical lines** (virtualization
  6,828 -> 7,032; model 1,174 -> 1,199). Both remain below the approved
  +300 ceilings; documentation and `/tmp` diagnostics are separate.
  Added persistent bookkeeping is the lazy key/ID map plus connection context
  and each interface's owner/key. No payload cache, renderer, geometry authority,
  timer, global focus hook, retained navigation flag or dynamic property was
  added. `git diff --check` passes. No commit, push, installation, upstream
  modification or running-application restart was performed.
- **Qualification boundary:** this fixes and verifies the Qt interface contract,
  not real AT-SPI/screen-reader operation or native compositor behavior. Those
  remain explicitly open under the already-existing qualification item. Qt's
  [state contract](https://doc.qt.io/qt-6/qaccessible-state.html) marks clipped
  items offscreen/invisible; unlike filtered/folded items, presented offscreen
  rows remain logically selectable and explicitly revealable. No plan task
  was added and item 4 has not been started.

### Step 4 change gate — shared presentation contract

- Baseline is clean `14e1d19`, the user-authorized item-3 commit. Item 4 alone
  is authorized; stop before exceeding **+220 production / +300 test CLOC**.
- Trace: protocol notifications -> native NodeGraph / browser PresentationModel
  -> conversation adapters -> DTOs -> existing display, Copy and accessibility
  consumers. Native and browser rendering remain separate; their data meaning
  is governed by the same differential fixture.
- Reproduced: browser command Copy drops content-line spaces; labels disagree
  on empty/separator-only inputs, padded xhigh and Unicode word boundaries;
  native prompt preparation misses heading levels 2–6; browser discards native
  prose-line preservation whenever structured Markdown appears. Native generic
  details use a 4,000-byte readable tree and include graph bookkeeping, whereas
  browser eagerly serializes every card to JSON and slices 4,096 UTF-16 units.
  Diagnostics produced an unpaired D83D surrogate and an 8,212-byte browser
  detail. Existing baseline checks pass (200 browser cases, two native suites),
  but do not cover these counterexamples.
- Delete eager/full browser JSON formatting and the native render-side generic
  bounds after both adapters satisfy the bounded DTO contract. Reuse command
  trimming; keep label fallback at call sites; consolidate native prompt line
  traversal while aligning browser fence/prose handling. Preserve D-6 Copy
  provenance and incremental Markdown preparation.
- Add only pure bounded formatting/normalization work necessary for the missing
  browser contract. No renderer, retained state, timer, cache, graph authority,
  scheduling mechanism or upstream modification is authorized.
- Baseline production: affected native files 6,439 CLOC, browser files 1,517
  CLOC. Tests: native adapter/cards 8,058 CLOC; three relevant browser files
  820 CLOC. Fixture/build/documentation deltas are also reported separately.
- Required checks: execute both frontends over added fixture groups; exercise
  actual native Copy and adapter snapshot/delta paths; retain large-output,
  Markdown selection/preparation/append checks; cover Unicode byte boundaries,
  ordering/nesting/numbers, whitespace and all heading levels. Use 14-way
  builds/test execution where suitable, Xvfb/offscreen for Qt, sanitizer and
  DPR checks, and compare native/browser performance with recorded baselines.

### Step 4 implementation and verification — 2026-09-20–21

- **Implemented at the existing boundaries:** generic details now share sorted
  readable tree output, four nesting levels, a 4,000-byte UTF-8 body and the
  existing truncation notice. Graph-only identity/retention fields are excluded.
  Floating-point values use shortest round-trip decimal formatting with the
  same exponent range. Native integer alternatives retain exact integer text.
  The browser visits array values only until its output is full and does not
  inspect unrelated generic details on known card types.
- **Deleted:** eager `JSON.stringify` and UTF-16 slicing on every browser card,
  native `boundedGenericActivityDetail`, its two constants and three consumer
  calls, and the duplicated native plain-prompt line traversal. No renderer,
  persistent state, timer, cache, QObject property, worker, bridge or upstream
  change was introduced. New formatting state is local to one projection.
- Command Copy reuses trailing-empty-line trimming and preserves content-line
  spaces. Labels normalize Unicode whitespace before classification, process
  complete code points, and keep empty values empty; generic-card and empty
  file-change label fallbacks belong to their callers. Authored Markdown
  preparation now agrees on all six heading levels, ASCII list markers,
  indentation, fences, CRLF and prose hard breaks. Existing Markdown renderers
  and D-6 selection provenance remain authoritative.
- Shared fixture schema 6 adds **37 cases**: 10 labels, 5 command Copy,
  16 Markdown, 6 generic details. Both frontends execute it. Native checks
  include real Copy clicks, actual documents, every incremental source prefix,
  prepared content, and adapter snapshot/delta agreement. The existing huge
  generic-card check now supplies 100,000 characters through the real adapter,
  preserving its visual bound assertion rather than constructing an invalid
  unbounded DTO. No existing assertion was removed or loosened.
- Before implementation the new browser fixture had **23 failing cases**;
  native failures covered Unicode labels, headings/indentation and generic
  detail/Copy. Logs: `/tmp/codexui-item4-browser-before.log` and
  `/tmp/codexui-item4-native-before.log`. An intermediate numeric case exposed
  `1000000000000000128` versus `1000000000000000100`; requesting the shortest
  scientific significand before applying the common decimal range corrected
  this without rounding integer DTO values.
- Debug build: `cmake --build /tmp/codexui-current-debug.fMMvCu --target
  codex-ui codexui-conversation-cards-test codexui-nodegraph-ui-adapter-test
  codexui-conversation-virtualization-test codexui-conversation-view-benchmark
  codexui-conversation-item-model-test codexui-inspector-graph-test
  codexui-shell-integration-test codexui-application-layout-test -j14`.
  Production compiles with the existing warning/error flags. Builds succeeded.
- Native regression command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest
  --test-dir /tmp/codexui-current-debug.fMMvCu --output-on-failure -j14 -R
  '^codexui-(conversation-cards|nodegraph-ui-adapter|conversation-virtualization|conversation-item-model|inspector-graph|shell-integration|application-layout|ui-style-source-policy)$'`:
  **8/8 pass**, 11.24 s. Log: `/tmp/codexui-item4-consumers-tests.log`.
- Browser: in `web`, `npm run build`, then `node --test --test-concurrency=14
  tests/*.test.mjs`: **325/325 pass**, no skips, including bounded traversal
  and absence of generic-payload inspection for known cards. Log:
  `/tmp/codexui-item4-web-tests.log`.
- DPR command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  QT_SCALE_FACTOR=<1|1.25|1.5|2> QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough
  CODEXUI_PRESENTATION_CONTRACT_TESTS=1
  /tmp/codexui-current-debug.fMMvCu/codexui-conversation-cards-test`:
  **4/4 pass**. This test-only filter runs the new contract plus existing
  line-break and authored-character selection cases; all remain in the full
  suite. Logs: `/tmp/codexui-item4-contract-dpr-*.log`. These are offscreen
  checks, not compositor/screen-reader qualification. The already-recorded
  high-DPR thumbnail assertion limitation was not changed or counted passed.
- ASan/UBSan: build adapter/cards targets with `-j14` in
  `/tmp/codexui-current-asan-ubsan.2oIJr4`. The same Xvfb/CTest command with
  `-R '^codexui-conversation-cards$'` passes **1/1**, 12.83 s, including all new
  adapter-through-Copy cases. No suppressions or sanitizer options were added.
  The broader adapter CTest hit its existing 10 s timeout; direct execution
  completed but failed its existing, unscaled one-second 40k-row timing bound
  (**1.239 s**), with no sanitizer diagnostic. Recompiling the adapter source
  from `git show HEAD:src/codex/ui/NodeGraphUiAdapter.cpp` with identical flags
  and linking it against the same test object/dependencies reproduces this
  timing failure (**1.225 s**). Thus this is not a passing sanitizer suite.
  Baseline artifact: `/tmp/codexui-item4-adapter-baseline.OcIkrj/adapter-test`;
  logs: `/tmp/codexui-item4-sanitizer-adapter-{direct,baseline}.log`.
- Native quantitative command: Xvfb/offscreen CTest in the Debug build with
  `-R '^codexui-conversation-performance-' -j14 --output-on-failure`:
  **12/12 pass**, 120.44 s; the registered timing cases retain `RUN_SERIAL`.
  Log: `/tmp/codexui-item4-performance-matrix.log`. Standalone before/after:
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough
  /tmp/codexui-current-debug.fMMvCu/codexui-conversation-view-benchmark 1280 1`.
  Both pass. Median microseconds before -> after: warm wheel **462 -> 466**,
  warm scrollbar **474 -> 474**, Markdown stream **6343 -> 6227**, streaming
  input-to-paint **5810 -> 5767**. Widget/document peaks remain **479/29**;
  streaming constructions remain zero. These measurements do not prove
  universally lag-free interaction.
- **Unclosed performance gate:** `node tools/profile-presentation.mjs` in
  `web` intermittently exceeds its unchanged **4 ms** 2,000-delta stream
  threshold. The initial baseline passed at 3.58 ms. A five-pair comparison
  using the pre-change projection module and otherwise identical generated
  modules passed **5/5** (2.91–3.69 ms); the current module passed **2/5**, with
  failures at **8.16, 6.87, 8.62 ms**. Earlier current runs failed at 7.31 and
  5.90 ms. Successful current samples project faster, but do not cancel those
  failures. GC/deoptimization-instrumented runs pass; that is diagnostic
  observer sensitivity, not proof of the cause or permission to loosen the
  gate. No warm-up, forced GC, delay, discarded serialization, threshold
  increase or stream-runtime change has been introduced to hide this result.
  Logs: `/tmp/codexui-item4-web-{baseline,current}-repeat-{1..5}.{json,stderr}`;
  comparison snapshot: `/tmp/codexui-item4-web-baseline.rpAGrc`.
- **Accounting against `14e1d19`: production +69 CLOC / +76 physical lines;
  test code +195 CLOC / +198 physical lines; shared fixture +45 lines;
  CMake +2 lines.** Even counting fixture/build growth with tests gives +242,
  within the approved +220 production / +300 test ceilings. Documentation is
  separate. `git diff --check` passes. No commit, push, install or process
  restart was performed.
- Limits: browser JSON integers beyond its safe integer range were already
  lossy before projection; native exact integer values are not deliberately
  rounded here. Sorted object-key enumeration still depends on object width;
  the output bound is not a constant-time guarantee for arbitrary objects.
  Other Qt/Unicode versions and native platform accessibility remain under
  the existing qualification item. **Item 4 is not marked complete while its
  browser performance check remains open; item 5 had not started at that checkpoint.**

### Step 4 performance follow-up — approved change gate (2026-09-21)

- **Scope:** the user approved the reviewed metadata-copy correction and
  stricter existing benchmark verification, without workarounds. Baseline:
  clean `263fb3b`, `master`, synchronized with `origin/master`. This continues
  item 4; it adds no plan task and does not reopen completed presentation fixes.
- **Invariant and trace:** presentation frames enter `PresentationModel`;
  `upsertThread` owns metadata and dispatches history to `upsertTurn` /
  `upsertItem`, which independently own item data. Select metadata before its
  deep copy, rather than deep-copying the complete history and deleting that
  copy immediately. Preserve both replace/merge paths, settings handling,
  input isolation, item order and subsequent streaming. Paginated turn
  loading already enters `upsertTurn` directly and is not rewritten.
- **Evidence:** review instrumentation found 20,000 item copies for 10,000
  retained items, versus 10,000 after the proposed change. Slow benchmark
  stream intervals overlapped major GC; the stage name does not identify
  where garbage originated. The generic-detail serializer is not reached by
  this benchmark's three known card kinds. Removing discarded serialization
  must not be reversed merely to change collection timing.
- **Deletion:** replace `clone(raw); delete threadFields.turns` with shallow
  metadata selection followed by the existing deep clone. No retained state,
  flag, timer, cache, renderer, worker or alternate protocol path is added.
  Per-delta wrapper and projection-allocation experiments did not establish
  reliable gate improvements and are excluded from this implementation.
- **Verification:** preserve the complete four-phase benchmark workload and
  the existing 47/45/4/20 ms limits; compare unrounded values, report all
  exceeded limits and assert real streamed content/item counts. Check input
  isolation through public model boundaries. Run the complete browser suite
  with concurrency 14, repeated timing runs serially, and the existing real
  browser qualification through Xvfb/offscreen. Record all failures; do not
  use forced GC, delays, discarded samples or changed thresholds. Qualify the
  shared native contract separately without changing native production.
- **Accounting gate:** production change is two lines replaced by two lines,
  expected net **0 CLOC**. Test/benchmark changes are accounted separately.
  No production growth or broader optimization is authorized. Item 4 remains
  open until its required verification succeeds.

#### Implemented correction and verification result

- `upsertThread` now selects metadata before cloning it. The existing
  `upsertItem` remains the owner of item copies; neither input frames nor
  nested metadata are aliased. No pagination, projection, renderer or delta
  processing policy changed. The benchmark now reports every phase before
  asserting all failures, compares unrounded times, and verifies actual
  item/card counts and the final appended text. Workload and limits are
  unchanged; there is no warm-up, forced GC or discarded sample.
- Both `npm run build` and `npm run build:app` in `web` pass. Browser command:
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen node --test --test-concurrency=14
  tests/*.test.mjs`: **327/327 pass**, no skips, including new replace/merge
  input-isolation cases. The same cases also passed before the production
  edit: they protect ownership, not an implementation-specific clone count.
- Native build: `cmake --build /tmp/codexui-current-debug.fMMvCu --target
  codexui-conversation-cards-test codexui-nodegraph-ui-adapter-test -j14`.
  Native command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu --output-on-failure -j14 -R
  '^codexui-(conversation-cards|nodegraph-ui-adapter)$'`: **2/2 pass**,
  11.35 seconds. Native production is unchanged.
- Diagnostic command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen node
  /tmp/codexui-step4-review.SnCC1C/clone-proof.mjs
  /path/to/workspace/drafts/CodexUI/codexui/web`. Item copies fall from
  **20,000 to 10,000** for 10,000 retained items; discarded history copies
  fall from **10,000 to zero**. Nested metadata/input isolation and subsequent
  streaming checks pass. This diagnostic is separate from timing runs.
- Matched timing command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen node
  /tmp/codexui-step4-fix.mqeMY4/repeat.mjs
  /path/to/workspace/drafts/CodexUI/codexui/web/tools/profile-presentation.mjs
  30`. Thirty serial processes per version, with the strengthened benchmark
  in both versions and no concurrent build/test workload. Before: **11/30
  pass**; after: **29/30 pass**. Median milliseconds before -> after:
  hydration **33.817 -> 26.560**, projection **18.332 -> 18.791**, streaming
  **6.476 -> 2.794**, presentation-lifetime churn **5.198 -> 6.257**. Thus
  hydration and streaming improve; this is not an all-phase speedup claim.
  The remaining failure is iteration 25: streaming **4.047178 ms > 4 ms**.
  Both repetition commands exit 1; the remaining violation is not waived.
- Existing real-browser command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  node tools/qualify-browser.mjs`. Before the production edit, Chrome starts
  successfully but the mock times out waiting for `thread/read`. The client
  now uses `thread/resume` with `excludeTurns: true`, followed by paginated
  history. This is stale qualification setup, not an Xvfb startup failure or
  proof of a product loading defect. Updating this additional test-only
  mock was requested separately; it has not been changed without approval.
- Logs and timing samples: `/tmp/codexui-step4-fix.mqeMY4/`, especially
  `strict-baseline-profile.jsonl`, `after-profile.jsonl`, `after-tests.log`,
  `native-contract.log`, `native-build.log` and `baseline-browser.log`.
- **Accounting against `263fb3b`: production 0 net CLOC / 0 net physical
  lines; regression tests +22 CLOC / +23 physical lines; benchmark +6 CLOC /
  +6 physical lines.** Documentation is separate. Diff review found no new
  retained state, timer, callback, cache, compatibility path or authority.
  `git diff --check` passes. No commit, push, install or application restart.
- **Item 4 remains open:** the repeated Node performance gate still has one
  failure, and real-browser qualification cannot reach its streaming checks
  until the stale mock is aligned. These results do not prove lag-free UI
  interaction or native compositor/screen-reader behavior.

#### Approved browser-qualification mock alignment (2026-09-21)

- The user explicitly authorized updating the existing mock. No further
  production changes were made; the prior metadata-copy change is preserved.
  Replaced the held full-history `thread/read` response with metadata-only
  `thread/resume`, bounded descending turn summaries, and per-turn item pages.
  The mock rejects legacy reads and checks thread identity, limit 80,
  descending order and summary-mode parameters. It no longer supplies an
  invalid generic `{data: []}` response for metadata resume.
- Preserve the 100-turn / 10,000-item fixture and 2,000 streaming deltas.
  Two turn pages (80 + 20) and 200 item pages (80 + 20 per turn) are served.
  Item replies are held until the real Load earlier activity control requests
  page two; otherwise the control first reveals thousands of already-local
  items rather than requesting older turns. This is mock response ordering,
  not a production delay or compatibility path. One click increases the
  application's exact display limit from 80 to **160**; the assertion follows
  that existing public behavior. Detached-scroll references remain tied to
  the same tail-relative cards. No timing/layout limit was increased.
- Added mock-only pending-response storage, fixture ownership and delivery
  counters. The former held-read path is deleted. Performance failures now
  accumulate until all dependent interaction checks have run and all phase
  measurements have been printed; they still make the executable exit 1.
  Browser evaluation errors include their actual exception description.
- Commands: `node --check web/tools/qualify-browser.mjs`; in `web`,
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen node --test --test-concurrency=14
  tests/*.test.mjs` (**327/327 pass**, no skips), and
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen node tools/qualify-browser.mjs`.
  Xvfb and headless Chromium start successfully; the old API timeout is gone.
- Three serial before/after pairs use the identical updated mock against
  the saved pre-copy-fix `baseline-app-dist` and current `app-dist`. Every
  run reaches the final performance assertion: all preceding behavioral
  checks pass, including responsive drawers, focus/control/card/text identity,
  10,000-item delivery, exact 160-card display, follow-tail, detached anchor,
  semantic no-op and submitted settings. Stream ingest/settled/task timing
  passes in all six runs. Current stream task time: **38.82–40.87 ms < 48 ms**.
- **All six full qualifications still fail** the unchanged hydration gates.
  Current wall/task times: **173.3–174.0 / 159.66–160.62 ms**, limits **141 /
  154 ms**; baseline **158.8–173.9 / 158.19–163.95 ms**. Both versions record
  **5 layout/style passes**, exceeding the existing bound of 2. One run per
  version also records idle **1.06 ms > 1 ms**, with zero layouts/styles.
  These failures reproduce without the metadata-copy correction. The mock
  now exercises multi-page loading plus an explicit history expansion, so
  its hydration result is not directly comparable to the former one-response,
  80-card workload. This is evidence requiring assessment within item 4,
  not authorization to relax its gates or rewrite production speculatively.
- Artifacts: `/tmp/codexui-step4-mock.AJUy53/`. `baseline-{1,2,3}.log` uses
  `qualify-baseline.mjs`, an otherwise identical harness with only its static
  artifact directory changed; `current-{1,2,3}.log` uses the repository tool.
  `browser-tests.log` records the 327 cases. Each paired qualification exits
  1; both versions' earlier failures remain recorded.
- **This follow-up: production 0 CLOC / 0 physical lines; qualification tool
  +38 CLOC / +40 physical lines** (612 -> 650 CLOC). Documentation is separate.
  `git diff --check` passes. No production timers, state or callbacks added;
  no install, commit, push or restart of the user's application. **Item 4 is
  still open**, including the prior standalone Node 4 ms gate violation.

#### Step 4 closure investigation and scalar-append reduction (2026-09-21)

- The user authorized continuing until item 4 can genuinely close. Baseline
  remains `263fb3b` plus the five previously recorded, preserved worktree
  changes. No new plan item, upstream change or commit is introduced.
- Repeated the unchanged Node gate: **29/30 pass**, one stream sample
  **6.978179 ms > 4 ms**. The scalar append path still built
  `{...scope, delta: text}` only to look up the original identity and then
  extract that text. `findItem` reads only thread/turn/item IDs. The file-change
  output path also constructed a one-field wrapper solely to unpack it.
  Both consumers now pass the already-normalized string to `appendText` and
  borrow the original scope for lookup. UTF-8 retention accounting, empty /
  non-string handling, indexed reasoning, ownership and agent propagation
  are unchanged. This deletes two lines and two temporary-object sites;
  no flag, cache, timer, worker or competing identity authority is added.
- A scratch prototype combining this reduction with the already implemented
  metadata correction passed **50/50** fresh Node processes. Unlike the older
  append-only prototype, it does not retain the discarded history clone.
  Implemented it in TypeScript, then reran the actual repository benchmark:
  **50/50 pass**, no discarded runs, forced GC or warm-up. Streaming median
  **2.835 -> 2.702 ms**, maximum **6.978 -> 3.486 ms**; all four unchanged
  limits pass. Hydration/projection/churn maxima: **39.780 / 22.701 / 10.913
  ms**, below **47 / 45 / 20 ms**.
- Commands: in `web`, `npm run build`, `npm run build:app`, and
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen node --test --test-concurrency=14
  tests/*.test.mjs`: **329/329 pass**. Two additional model cases exercise
  frozen incoming scopes/data, empty and non-string deltas, multibyte text,
  retained byte counts and stable item/raw identity for both append consumers.
  Serial timing command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen node
  /tmp/codexui-step4-fix.mqeMY4/repeat.mjs
  /path/to/workspace/drafts/CodexUI/codexui/web/tools/profile-presentation.mjs
  50`. This correction is **−2 production CLOC / −2 physical lines** and
  **+22 test CLOC / +23 physical lines**, relative to the previous checkpoint.
- **Browser test-contract correction proposed, not yet applied:** the mock
  alignment above incorrectly combined initial hydration with explicit
  history expansion, changing the original measured 80-card scenario to 160.
  A scratch fixture of **80 turns × 125 items** preserves 10,000 items,
  resumes metadata-only, hydrates 160 bounded item pages and restores the
  exact original 80-card display and two-layout initial-reveal invariant.
  It does not raise any threshold. Existing session tests separately exercise
  multiple turn pages. User approval of this fixture reshape is pending.
- The first restored-scenario browser trial passes. Five subsequent paired
  trials pass all hydration/interaction assertions; one current run misses
  idle at **1.01 ms**, versus three baseline idle misses of **1.04–1.17 ms**.
  The idle phase itself runs test JavaScript across two animation frames.
  An isolated idle CPU profile samples no application JavaScript; this does
  not prove zero CPU use, but identifies a measurement-overhead concern.
- Scratch verification replaces that active idle driver with a **measured**
  two-frame-duration host-side observation (33.3 ms), not a warm-up or a
  delay before measurement. The unchanged 1 ms / zero layout / zero style
  gates still apply. **5/5 complete browser trials pass**: hydration wall
  **119–129 ms**, task **123.94–133.92 ms**, exactly two layouts/style passes;
  idle **0.14–0.29 ms**, zero script/layout/style; streaming task
  **34.97–38.26 ms**. A positive-control run injecting 2 ms of browser CPU
  work every 8 ms fails the idle gate at **8.25 ms**, as required. The
  observation therefore still catches real work rather than suppressing it.
- Artifacts: `/tmp/codexui-step4-close.dsCeFN/` includes `node-before.jsonl`,
  `node-after.jsonl`, `web-after-scalar.log`, the browser CPU profiles,
  `initial-{baseline,current}-{1..5}.log`, `passive-idle-{1..5}.log` and
  `passive-idle-positive.log`. The initial unlinked scratch Node run could not
  resolve the existing frontend package; it is recorded in `append-trial.jsonl`
  and is not counted as a timing result. The linked prototype is separate.
- **Status at that checkpoint:** scalar append correction implemented and verified;
  browser fixture/idle-measurement corrections remain scratch-only pending
  the requested fixture approval. Item 4 is not yet marked complete. No
  production renderer/scheduler changes are justified by these measurements.

#### Step 4 closure — approved mock correction and AISuite receive bound (2026-09-21)

- **Authorization:** the user approved the 80-turn × 125-item browser fixture
  and host-observed 33.3 ms idle interval. Both are now applied, preserving
  10,000 hydrated items, 80 visible cards and every existing timing/layout
  limit. Hydration uses metadata-only resume, one turn-summary page and 160
  item pages; it no longer waits for obsolete `thread/read`. No history
  expansion is included in the initial-render measurement. An injected idle
  workload still fails the unchanged 1 ms gate (**7.97 ms**), confirming that
  host observation does not conceal browser work.
- **Intermediate failures retained:** the first repository rerun failed
  streaming ingest in 2/5 browser runs (**21.3 / 21.9 ms**, limit 20 ms).
  Profiling identified redundant deep clones in diagnostic history. The
  private normalizer emits fresh frames from independently parsed messages;
  model mutation owns separate copies, and response consumers only read the
  input. History now retains those frames directly while preserving request
  redaction and the 500-frame bound. New coverage verifies both result and
  notification history remain unchanged during subsequent streaming.
  This improved five paired ingest measurements from **14.7–19.8 ms** to
  **8.7–12.4 ms**, but ten production runs still included one **22.2 ms**
  failure. Passing diagnostic traces were not substituted for that failure.
- **AISuite scope and invariant:** after the user explicitly approved the
  proposed source-level change, the clean AISuite checkout was
  `d84dc7d0e98b950441ae1dc9739aa3fd6361142c`, branch
  `feature/agent-provider-anthropic`. Only its browser transport and boundary
  tests changed. Incoming strings require at most three UTF-8 bytes per
  UTF-16 code unit (four per surrogate pair). The receive predicate now
  avoids encoding when that upper bound already fits; otherwise the original
  exact byte check still runs before JSON parsing. The maximum, rejection
  code/reason, malformed-input handling, outbound accounting and backpressure
  are unchanged. No bridge daemon or app-server code changed.
- **Deterministic resource proof:** the same 2,000 delta frames are delivered
  before and after, with **2,000 -> 0** frame-size encoding allocations.
  Six character classes × four limits exercise ASCII, two-/three-byte text,
  surrogate pairs and lone surrogates below, at and above the byte boundary,
  plus the conservative upper bound. All 24 cases pass both before and after.
  The earlier scratch check compared 1,376,326 boundary decisions. This proves
  the removed work and equivalent decisions; it does not retrospectively
  establish the exact cause of the isolated 22.2 ms outlier.
- **Commands and final results:** in AISuite's `packages/codex-frontend`,
  `npm run build`, `./node_modules/.bin/tsc -p tsconfig.type-tests.json`, and
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen node --test --test-concurrency=14
  tests/*.test.mjs`: build/type checks pass, **44/44 SDK tests pass**. In
  CodexUI's `web`, `npm run build`, the same Node test command, and
  `npm run build:app` pass; **330/330 browser tests pass**. Xvfb/offscreen
  `node tools/verify-artifact.mjs` passes. All relevant cases ran without
  short-circuiting; builds/tests did not overlap timing runs.
- **Production-browser gate:** ten serial fresh-process runs of
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen node web/tools/qualify-browser.mjs`
  pass **10/10**. Ingest: **6.3–7.1 ms**, versus **8.6–10.1 ms** in three
  immediately preceding runs with the original SDK. Hydration wall/task
  maxima **114.2 / 118.63 ms** (limits 141 / 154); stream settlement/task
  maxima **37.3 / 33.03 ms** (47 / 48); idle maximum **0.14 ms** (1), with
  no idle layout/style passes. Semantic no-op, card/control/focus identity,
  follow-tail, detached anchor, responsive layout and settings checks pass.
- **Node gate:** 50 serial fresh-process runs of the existing
  `web/tools/profile-presentation.mjs` pass **50/50**. Maximum hydration,
  projection, stream and churn costs: **29.826 / 20.306 / 3.525 / 6.738 ms**,
  below unchanged **47 / 45 / 4 / 20 ms** limits. No forced GC, warm-up,
  discarded samples or production delay was used. Native production is
  unchanged by this follow-up; its latest adapter/cards checks passed 2/2
  and focused DPR contract checks passed at 1 / 1.25 / 1.5 / 2.
- **Accounting:** CodexUI follow-up against `263fb3b`: production **−2 CLOC /
  0 physical lines**, tests/tools **+102 CLOC / +106 physical lines**. Including
  the original item-4 implementation gives **+67 production / +297 test CLOC**;
  fixture/build/documentation remain separately reported. The separately
  authorized AISuite change is **+1 production CLOC / +2 physical lines** and
  **+20 test CLOC / +21 physical lines**. It replaces unconditional temporary
  encoding with a proven bound; no state, helper, cache, timer, renderer or
  alternate transport was added. Both repository diff checks pass.
- **Evidence and completion boundary:** final logs and allocation probe are
  `/tmp/aisuite-frame-bound.K9KZ5y/`; earlier failed runs and profiles remain
  `/tmp/codexui-step4-final.PgVRVS/`. Item 4 is complete against the two local
  worktrees. Both changes still require user-authorized commits/release before
  other checkouts inherit them. Real compositor/screen-reader qualification,
  broader CI coverage and the previously reproduced baseline sanitizer timing
  limitation remain in the existing qualification item, not counted as passes.
  These measurements do not establish universally lag-free interaction. No
  commit, push, install, upstream app-server change or application restart was
  performed; no plan-tab update tool is available in this session.

### Step 5 change gate — allocation-free scalar height updates (L-1)

- **Scope and baseline:** user approved the reviewed item-5 proposal after
  committing item 4 as `71e2eb1`. This stage starts from that clean commit;
  the separate item-4 browser performance gate is not closed or expanded here.
- **Invariant and consumers:** changing a scalar height in the existing index
  needs no allocation or structural mutation. Both conversation measurement,
  folding and reflow and Inspector resident measurement use `setHeight()`;
  their ranges, anchors and row placement consume the same totals/prefixes.
  No presentation, measurement, residency, interaction or accessibility policy
  is changed.
- **Reproduced cause:** `57a4523` introduced a growing `vector<Node *>` while
  retaining `noexcept`. Even an unchanged height fills that vector before
  comparing values. At 10,000 rows, the first-row call allocates four times
  (120 cumulative bytes); middle/last-row calls allocate five times (248
  bytes), whether changed or unchanged. Rejecting allocations immediately
  before either valid call invokes the probe's terminate handler (exit 86).
  Invalid-row calls allocate nothing and retain the previous diagnostic count.
  These are fault-injection results, not an observed live low-memory crash.
- **Replacement:** use existing allocation-free `height(row)` to compute a
  `qint64` difference, return early for zero, then add the difference along
  the existing order-statistic path. Delete the ancestor vector, reverse
  traversal, scalar-update count recomputation and unused vector include.
  Keep `updateNode()` for structural operations. No parent pointer, helper,
  cache, timer, fixed-depth buffer, new authority or recursion is introduced.
  Expected-logarithmic update complexity and the diagnostic meaning remain;
  auxiliary storage becomes constant. Tree structure cannot change between
  the two synchronous, callback-free traversals.
- **Exception specifications:** retain now-allocation-free `setHeight()`'s
  `noexcept`; remove it only from `leadingChromeHeight()` and its caller
  `naturalContentHeight()` (declarations and definitions), because their
  `QLabel::sizeHint()` call does not offer that guarantee. No general Qt
  memory-exhaustion recovery is claimed.
- **Accounting baseline:** touched production 3,893 CLOC / 4,205 physical
  lines; item-model tests 1,199 CLOC / 1,275 physical lines. Estimated
  production reduction, focused test-only growth; no production addition.
- **Baseline checks:** existing Debug targets built with `-j14` (no work);
  Xvfb/offscreen CTest selection for the six consumer suites plus 12
  conversation and four Inspector performance cases passes **22/22**,
  134.58 s. Registered timing cases retain `RUN_SERIAL`; no limits changed.
  Log: `/tmp/codexui-item5-before.log`.
- **Required verification:** exact scalar/prefix/lookup results against a
  reference vector, unchanged/invalid diagnostics, negative clamping, zero
  extents, large sums, and updates after insertion/move/removal. Reject heap
  allocations in a standalone probe without injecting allocator hooks into
  production or the Qt test process. Rerun both consumers, focused DPR
  checks, quantitative gates and sanitizers; compare the same before/after
  microbenchmark. Offscreen checks do not qualify native compositors or AT.

### Step 5 implementation and verification

- Implemented the reviewed two-traversal scalar update, deleting its only
  allocation. Removed the two unsupported view-wrapper exception promises.
  Structural tree operations, node layout, counters, no-op semantics and all
  widget/measurement policies remain unchanged. No compatibility path remains.
- Added one model-suite case with **847** valid scalar updates, empty/invalid
  updates, a reference vector checked after every mutation, integer limits,
  zero extents, sums beyond `INT_MAX`, exact boundary lookup and diagnostic
  checks, both before and after insertion/movement/removal. It joins the
  existing non-short-circuit runner; no existing checks or thresholds changed.
- **Allocation proof:** the standalone probe uses the actual index source,
  with allocation interception confined to the probe executable. The fixed
  first/middle/last-row changed and unchanged calls allocate zero bytes;
  changed, unchanged and invalid calls return normally when all allocations
  are rejected. A further **60,000** changed/unchanged/clamped/limit updates
  after structural edits succeed with allocations forbidden (exit 0,
  `allocations=0`, final total 70,000); the old valid calls terminated (86).
  Probe source/binaries: `/tmp/codexui-item5-review.lAhtKI/allocations.cpp`,
  `allocations` (pre-change) and `allocations-after`.
- Probe build: `g++ -std=c++20 -O2 -Wall -Wextra -Werror -Isrc
  /tmp/codexui-item5-review.lAhtKI/allocations.cpp
  src/codex/middle/ConversationHeightIndex.cpp
  $(pkg-config --cflags --libs Qt6Core) -o
  /tmp/codexui-item5-review.lAhtKI/allocations-after`.
  Execute with `xvfb-run -a env QT_QPA_PLATFORM=offscreen <binary>`; arguments
  `change`, `noop`, `invalid`, `sweep` select allocation-rejection checks.
  No argument measures five 100,000-call batches.
- **Scalar performance:** three alternating before/after pairs with the same
  optimized flags and allocation instrumentation: changed median ns/call
  **68/69/68 → 22/19/18**, unchanged **55/55/53 → 7/6/6**. Each old middle-row
  batch allocated 500,000 times / 24.8 MB cumulatively; all new batches zero.
  Diagnostic counts match (first row 8, middle/last 11, no-op 0, invalid
  preserves the preceding count). Log:
  `/tmp/codexui-item5-scalar-comparison.log`. These are scalar microbenchmarks,
  not end-to-end UI latency or a promise of universally lag-free interaction.
- **Builds:** `cmake --build /tmp/codexui-current-debug.fMMvCu --target
  codex-ui codexui-conversation-item-model-test
  codexui-conversation-virtualization-test codexui-conversation-cards-test
  codexui-inspector-graph-test codexui-conversation-view-benchmark
  codexui-application-layout-test codexui-shell-integration-test -j14` passes
  with the existing warning/error policy. Sanitizer build in
  `/tmp/codexui-current-asan-ubsan.2oIJr4` of model, virtualization and Inspector
  targets with `-j14` also passes.
- **ASan/UBSan:** `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-asan-ubsan.2oIJr4 --output-on-failure -j14 -R
  '^codexui-(conversation-item-model|conversation-virtualization|inspector-graph)$'`:
  **3/3 pass**, 1.24 / 15.79 / 1.10 s, wrapper exit 0. No new suppressions,
  leak-detection changes or sanitizer diagnostics. Log:
  `/tmp/codexui-item5-sanitizers.log`.
- **Matched native baseline/after command:** `xvfb-run -a env
  QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu -V -j14 -R
  '^codexui-(conversation-item-model|conversation-virtualization|conversation-cards|inspector-graph|application-layout|shell-integration|conversation-performance-.*|inspector-performance-.*)$'`:
  **22/22 pass before and after**, 134.58 → 135.21 s, wrapper exit 0.
  Logs: `/tmp/codexui-item5-{before,after}.log`. This covers six consumer
  suites, conversation rows 320/1,280/10,000 at DPR 1/1.25/1.5/2 and Inspector
  10,000 rows at the same DPRs. Timing cases stay serial; the other cases use
  `-j14`. No case was skipped or threshold weakened.
- **UI performance interpretation:** all quantitative gates pass. Conversation
  peaks remain 479 widgets / 29 documents / 31 resident cards in all 12
  comparisons; streaming constructions remain zero; height-index update
  counts match at each size. Timings are not uniformly lower: at 1,280
  rows/DPR 1, median warm wheel is 470 → 463 us and streaming input-to-paint
  5,771 → 5,687 us; at 10,000 rows/DPR 2 those are 760 → 833 us and
  6,565 → 6,994 us. The scalar measurement proves its own improvement, not
  that every end-to-end sample improves. Inspector renderer retention,
  zero update-time row/document construction and its bounded-work gates also
  pass. Full raw measurements remain in the logs above.
- **Focused DPR verification:** `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  ctest --test-dir /tmp/codexui-current-debug.fMMvCu --output-on-failure -j14
  -R '^codexui-(command-output-geometry-.*|conversation-selection-focus-.*|inspector-geometry-.*)$'`:
  **20/20 pass**, 0.75 s, wrapper exit 0, including Fusion and Breeze (no
  style skips), DPR 1/1.25/1.5/2. Log: `/tmp/codexui-item5-dpr.log`.
- **Final accounting:** touched production **3,893 → 3,888 CLOC (-5)**,
  **4,205 → 4,200 physical lines (-5)**; tests **1,199 → 1,288 CLOC (+89)**,
  **1,275 → 1,365 physical lines (+90)**. Documentation is separate. Final
  diff review finds no new flag, timer, cache, callback, property, ownership
  path or duplicated authority; `git diff --check` passes. No commit, push,
  install, process restart or upstream modification performed for item 5.
- Item 5 is complete for its stated scalar and exception-specification
  invariants. The item-4 browser performance gate remains open; native
  compositor/screen-reader qualification remains separate. No plan-tab update
  tool is exposed in this session; this existing ledger records completion.

### Performance continuation gate — targeted thread-row scheduling

- **Invariant:** a completed UI work unit must not start the next pending unit
  after the existing presentation deadline; unfinished work remains in the
  existing queue and eventually reaches its latest graph state. One unit can
  still overrun; this change does not claim a preemptive hard wall-clock cap.
- **Path:** worker graph changes → `FrontendSession` graph handler →
  `threadPaneRoute` → `pendingThreadRows` → `commitPendingPanes` →
  `NodeGraphUiAdapter::threadRow` → `ThreadPane::applyRowPresentation`.
  Structural fallback and retirement acknowledgements remain unchanged.
- **Cause:** the targeted-row stage checks `hasFrameBudget()` only at entry,
  moves the complete queue into a local vector, then drains every row without
  checking elapsed time. The eight-stage scheduler finding already includes
  this work; this is not a new backlog item or a full scheduler rewrite.
- **Reduction:** process the existing pending queue while the existing
  deadline permits; pop completed rows only. Use the already-established deque
  container (as for pending conversation items) to retain FIFO order without
  repeated vector-prefix shifts or starving older rows. Delete the local vector and
  separate fallback flag. Reuse `pendingThreadPane` and the existing
  rescheduling tail. No new timer, cache, flag, callback, renderer, sort policy
  or visual change. Actual production reduction: four lines.
- **Lifetime:** pending targets retain their graph identities until processed
  or cleared by the existing successful full snapshot/provider reset. Existing
  `uiRetainsTarget` already covers that queue. Failed row projection still
  requests the existing full snapshot; no extra retry mechanism.
- **Verification:** extend shell integration at the graph-to-tree boundary.
  Inject a 10 ms cost into each distinct row's accessible-name notification and
  observe a queued callback: the next row must wait for another event-loop
  turn. Verify all eight latest titles, stable tree-item identities, and an
  idle scheduler after completion. Existing retirement, thread-switching,
  streaming, focus/accessibility and graph-contention cases must still run.
- **Accounting baseline:** `ShellWidget.cpp` 2,772 production CLOC; test growth
  reported after verification. Test-only delay and observation remain outside
  production. All builds use `-j14`; CTest is invoked with `-j14` through
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen`.

### Targeted thread-row scheduling result

- Removed the moved batch and fallback flag. The existing queue is now a
  deque, like the conversation-item queue, and is consumed from the front
  only while the existing 8 ms deadline permits. Successfully presented rows
  are removed; unfinished rows remain queued. Structural fallback still
  replaces the whole projection and clears the queue after success.
- Regression proof: the unchanged production implementation processes all
  **eight** deliberately expensive row updates before the queued event-loop
  callback can run (`/tmp/codexui-thread-deadline-before.log`). The corrected
  implementation yields after **one**, completes every row, retains native
  item identity and the latest accessible title, and stops scheduling once
  empty. This is controlled work-budget evidence, not a claim about real
  input-to-paint latency or an 8 ms cap on an individual row.
- The first test draft incorrectly listened for `dataChanged`; this custom
  tree updates its authoritative row fields and accessibility directly. That
  draft was corrected before production changes. The final test observes
  accessible-name events using the existing test-only event probe, extended
  with an optional observer. No production instrumentation was added.
- Build passed without diagnostics:
  `cmake --build /tmp/codexui-current-debug.fMMvCu --target codex-ui
  codexui-shell-integration-test codexui-nodegraph-thread-pane-ui-test
  codexui-inspector-graph-test codexui-conversation-virtualization-test -j14`.
- Shell integration passed three consecutive complete runs: **9.76, 9.18,
  9.23 s**. Command: `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest
  --test-dir /tmp/codexui-current-debug.fMMvCu -R
  '^codexui-shell-integration$' -j14 --repeat until-fail:3
  --output-on-failure`. Existing retirement, graph contention, selection,
  streaming and provider-reset cases still execute; no failures are hidden.
- Shared-helper consumers: **17/17 passed**, including all four thread-panel
  DPRs, Inspector graph and four geometry DPRs, and eight Fusion/Breeze
  conversation selection/focus variants. None skipped. Command: the same
  Xvfb/CTest prefix with `-R '^codexui-(nodegraph-thread-pane-ui.*|inspector-
  (graph|geometry-.*)|conversation-selection-focus-.*)$' -j14
  --output-on-failure` (join the wrapped regular expression without spaces).
- Full virtualization suite after rebuilding with the test-helper change:
  `xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-current-debug.fMMvCu -R
  '^codexui-conversation-virtualization$' -j14 --output-on-failure`.
  All cases ran; its only failure remains the previously recorded
  `collapsedLargeCardsSkipBodyProjection` timing gate, now **152,064 us**,
  one rebuild and 5,000 blocks. No new correctness failure was reported.
- Accounting against `6056570`: production `ShellWidget.cpp` **2,772 →
  2,768 CLOC (-4)** / **2,917 → 2,913 physical lines (-4)**. Test
  `ShellIntegrationTest.cpp` **5,081 → 5,173 CLOC (+92)**; test helper
  **87 → 93 CLOC (+6)**. Total tests **+98 CLOC / +102 physical lines**.
  Documentation is separate. `git diff --check` passes. No new production
  timer, callback, cache, flag, dynamic property or authority; no visual change.

### Performance baseline still open — no completion claim

- Before the scheduler correction, the registered 12-case conversation
  performance matrix ran with `xvfb-run -a env QT_QPA_PLATFORM=offscreen
  ctest --test-dir /tmp/codexui-current-debug.fMMvCu -R
  '^codexui-conversation-performance-' -j14 --output-on-failure`.
  CTest's existing `RUN_SERIAL` setting isolated performance cases despite
  `-j14`. Debug/GCC 16/Qt 6.10.2; raw baseline preserved at
  `/tmp/codexui-performance-baseline-6056570.log`.
- **3/12 passed:** all DPR-1 cases (320, 1,280 and 10,000 rows). **9/12
  failed:** all higher-DPR cases exceed the 10 ms Markdown-stream median
  gate, at **10.817–13.842 ms**. Mutable-tail locality and retained
  card/document checks remain true; the gate message alone must not be
  interpreted as evidence of failed incremental parsing. At 10,000 rows,
  DPR 1.5/2 also exceed the 5 ms structural-commit median gate:
  **5.063/5.493 ms**; structural correctness remains true.
- The independent 5,000-file expansion gate also failed before this change:
  **145,460 us > 100,000 us**, with the expected one body rebuild and 5,000
  document blocks. An isolated Qt reproduction attributes most measurement
  work to full-document layout/shaping (Callgrind artifact:
  `/tmp/codexui-file-layout.callgrind`). Simply using document height,
  batching edits or changing initial width did not meet the gate. A separate
  worker-document handoff experiment still spent roughly 90–96 ms attaching
  on the GUI thread and produced timer-affinity warnings; it was rejected.
  None of these experimental alternatives entered production.
- The scheduler change does not touch these conversation paths or their
  thresholds. These are still performance-qualification failures, not fixed
  by this step. Markdown changes remain deferred as requested. Offscreen
  checks do not establish compositor or assistive-technology qualification.
