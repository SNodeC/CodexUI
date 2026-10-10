# Native qualification execution record — 2026-09-27

<!-- snodec:begin back -->
<p>
  <a href="../README.md#project-overview" title="CodexUI"><img src="media/menu/back-codexui.svg" alt="CodexUI" width="96" height="24"></a>
</p>
<!-- snodec:end back -->

Specification: [canonical interactive inventory and visual approval matrix](native-ui-ux-qualification-inventory.md).
This records execution, not a replacement plan or reduced acceptance scope.

**Overall verdict: incomplete, not approved.** The recorded repairs are verified
within the stated checks. They do not complete every canonical combination.

## Build and isolation

- Baseline HEAD: `b9ff6e5d0d4115922174c1a5fdd4310537c7975b`, master, with
  preserved pre-existing project/section and qualification changes.
- GCC Debug, installed Qt 6.10.2; build directory
  `/home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug`.
- Builds and suitable CTest execution used `--parallel 14`; serial benchmark
  properties remained respected. Timing policy was the accepted `report` mode.
- Every Qt launch used `xvfb-run -a env QT_QPA_PLATFORM=offscreen`.
- Diagnostic v21 links the production MainWindow/session/widget libraries and
  drives actual widgets. Controlled graph input is not a real-server response.
- No installation, commit, push or change to the user's running system.

## Verified repairs

### Attachment row geometry — §§14, 22.6

Previously, every row was fixed at 28 logical pixels and the scroll viewport
used that constant independently of its contents. Enlarged filename text was
clipped. The replacement uses existing Qt layout hints, including height for
width, on layout requests and content resize. The row retains a 28px minimum;
the viewport shows the natural first four rows. Remove buttons size to their
contents with the existing minimum. Fixed-height arithmetic and its imperative
refresh calls were deleted. No new timer, cache or stored state was introduced.

- Regression checks cover six long names, font changes 10→16→10pt, widths
  900→440→900, four-row cap, natural scroll range, shrink to one row and empty.
- The former test-only content minimum-height assignment was removed: the
  production layout now establishes its own scroll range.
- Live checks cover six file attachments, font changes 9→16→9pt, requested shell
  widths 1536/1100 and removal/focus at DPR 1/1.25/1.5/2. Enlarged-font shell
  minimums can exceed the requested width; captures show the actual layout.
- All eight normal/enlarged-font narrow composer captures were opened and
  inspected. Filenames and remove controls fit, with four complete visible rows.

### Pixel-only conversation scrolling — §§10–11

The original mixed pixel/angle policy below is superseded by v110. Supporting
pixel-only input did not justify changing Qt's normal mixed-event distance.

The view previously delegated movement to Qt's angle-wheel path, then read
pixelDelta only for follow-state bookkeeping. The existing wheel handler now
applies supplied pixel displacement directly. Angle-only input still uses Qt.
The existing central gesture owner and follow-state policy are unchanged.

- Live sequences at all four DPRs moved exactly +60 pixels for pixel-only -60,
  +60 for pixel -60 combined with angle -120, and -17 for pixel +17.
- Regression checks additionally cover an omitted Begin event and the existing
  nested output/editor/attachment gesture ownership scenarios.
- No second router, event-forwarding mechanism, animation, cache or timer added.

Incremental changes for these two repairs: production **+34/-21 = +13 lines**
(ComposerPane source/header and ConversationView); tests **+62/-5 = +57 lines**.
This is below the approved +28 production-line allowance. Existing dirty-tree
changes are not included in those counts. `git diff --check` passed.

## Verification results

```
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen CODEXUI_TIMING_POLICY=report \
  ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --output-on-failure
```

- Full rebuilt suite: **80/80 passed, 162.12s**. Two accepted timing warnings:
  NodeGraphUiAdapterTest.cpp:196 and ConversationViewBenchmark.cpp:2123.
- Application-layout executable separately passed at all four DPRs with
  `QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough`.
- Wheel-routing sample: baseline direct/routed p95 24/24µs, routed max 28µs;
  full rerun 18/19µs, max 20µs. Samples varied (an earlier focused rerun measured
  27/29µs, max 30µs). This does not establish universal input-to-paint latency.
- An 80-request interactive scenario passed at all four DPRs: bounded residency,
  keyboard traversal, review/cancel, observer restrictions and resolution.
  This is controlled-input functional coverage, not approval of all request
  families and visual states.
- Final source cleanup only hoisted the unchanged content-width expression into
  a local variable; the focused layout/wheel checks passed again afterward,
  2/2 in 2.85s.

Evidence: `/tmp/codexui-final-g9DeRT/ctest-v21-full.log`, `layout-*.log`, and
`/tmp/codexui-interactive-uCYB7o/captures/v21-*-composer-font*-width1100.png`.
Detailed earlier execution evidence and remaining combinations are recorded in
`/tmp/codexui-interactive-uCYB7o/qualification-results.md`.

## Continuation: v22 interactive evidence

The v22 diagnostic executable links the current production libraries, including
the final content-width cleanup. These checks used DPR 1, normal motion and a
1536×960 shell through the same Xvfb/offscreen workflow. No production or test
source was changed during this continuation.

### Passed within this configuration — §§17, 22.7

Controlled graph input selected three disposable repositories; actual libgit2
diff generation, Qt controls and review widgets were used.

- All repositories listed both visible roots; Hidden added the third root.
- Repository selection filtered the file list correctly. Staged was empty for
  these untracked fixtures; Since HEAD restored the expected files.
- An 18 MiB text fixture triggered the existing 16 MiB display limit. The file
  remained listed, with `Display truncated` and an explicit omission message.
- Open review, dismissal and Copy worked. Selecting the large file and changing
  filters did not widen the Inspector.
- Full-shell multi-repository, truncation and hidden-repository captures, plus
  the review-dialog capture, were opened and inspected. The narrow preview
  intentionally scrolls horizontally; the review shows the full omission text.

Evidence: `/tmp/codexui-final-g9DeRT/diff-checks.log` and
`/tmp/codexui-interactive-uCYB7o/captures/v22-diff-*.png`.
Other DPR/font combinations, stale-result cancellation and error paths remain
open; this does not approve all of §17.

### Reproduced failure, subsequently fixed in v24 — §§8–10, 21

A generated rollout in the isolated app-server home contains 5,000 completed
turns, each with one user and one assistant item. The real app-server parsed it
and served it through `thread/turns/list` and `thread/items/list`; this is real
protocol/pagination coverage with artificial history, not model-produced data.

- Initial selection displayed 160 rows. Repeated **Load earlier activities**
  interactions progressed to 9,920 rows, with seven resident card widgets at
  sampled top-of-page positions.
- The paging control then disappeared, but the first displayed turn was still
  `qualification-turn-40`: the oldest 40 turns / 80 rows were not shown.
- A separate read-only `thread/turns/list` request with ascending order returned
  turns 0 and 1 and their user/assistant items. The source rollout contains all
  5,000 turns. Thus server availability is established, but the exact failing
  delivery/graph/projection boundary has not yet been established.
- The early driver mistakenly expected scrolling alone to page; it was corrected
  to click the existing button. Later attempts also wait for its enabled state.
  These driver corrections do not resolve or excuse the final-page discrepancy.
- Aggregate diagnostic heartbeat p95 was 16 ms, maximum 367 ms. This includes
  diagnostic activity and is not an event-to-presentation measurement or a
  lag-free verdict.

Evidence: `/tmp/codexui-final-g9DeRT/retained-pagination*.log`, generated rollout
`/tmp/codexui-interactive-uCYB7o/codex/sessions/2026/09/26/rollout-2026-09-26T20-00-21-c0944fc0-2c49-4b91-b968-f064b6d17715.jsonl`,
and `captures/v22-retained-at-end-of-paging.png` under the same evidence root.
Do not mark 10,000-row real-history completeness or its accessibility count
passed: the intended final assertions were not reached.

### Temporary execution block after interruption

The resumed session has restricted filesystem/network access. Connecting to
the existing diagnostic Unix socket fails with `PermissionError: [Errno 1]
Operation not permitted`. Live interaction and repair verification cannot
continue in that session. No substitute desktop workflow or speculative fix
was introduced. The source trace reached the runtime page handler, graph
ingestion and ShellWidget projection/commit handoff; exact attribution still
requires the isolated live process state.

Access was subsequently restored. The v22 processes had exited, so v23/v24 used
a newly launched independent bridge/app-server, the same disposable history and
fresh diagnostic sockets. The investigation below supersedes that open attribution.

## Verified projection retry repair — v24

### Cause and correction

The invariant is that a pending conversation projection must retain a wakeup
until it can read and present authoritative graph state. A failed non-blocking
`tryRead()` does not guarantee another graph-change notification.

The v23 reproduction stalled with **1,920 graph items versus 1,760 model rows**.
The debugger showed `conversationProjectionRequested=true`, no in-flight
projection and no scheduled start. Breakpoints in both
`startConversationSnapshot()` and its completion callback caught empty
`conversationInfo()` results. Both paths set the pending flag and returned
without scheduling another attempt. The defect was therefore not confined to
the final page: a subsequent page could incidentally wake the lost projection,
but the final page had no subsequent request to do so.

The fix in `ShellWidget.cpp` uses `requestConversationProjection(false)` at the
start boundary and folds the completion-side metadata read into the existing
projection-failure retry branch. The latter also retains whether a full snapshot
is required. The duplicated wait-only branch is deleted. Existing quiet-period,
retry, generation and owned-pool mechanisms remain authoritative; no new timer,
cache, flag, worker, renderer or blocking read was added.

Incremental accounting against the pre-repair dirty tree:

- Production: **+6/-8 = -2 lines**, including the explanatory comment.
- Tests: **+48/-0 lines** in the existing Shell integration suite.
- The new case holds the graph writer across deferred projection startup,
  releases it without mutation/notification and requires the missing row to
  become visible. It fails on a separately linked pre-fix ShellWidget, with the
  exact assertion `a contended projection retries after unlock without another
  event`; the fixed suite passes. No production test hook was introduced.

### Live and regression evidence

- **All four DPRs passed real-server pagination to 5,000 turns / 10,000 rows**:
  controller at 1.0; observers at 1.25, 1.5 and 2.0. Each checked the oldest user
  message, newest assistant message, logical accessibility count 10,000 and
  bounded residency (seven cards at sampled page tops; fewer than 40 at the end).
- First/last-page captures at all four DPRs were opened and inspected. Nested
  turn spacing, endpoint text, timing placement and observer-disabled controls
  remained coherent. These settled captures do not approve every transition frame.
- Twenty rapid thread selections retained all 10,000 rows and the exact detached
  scrollbar value. Rapid search replacements retained only the final matching
  result set. This does not exhaust all deliberately delayed response orderings.
- Full rebuilt CTest run: **80/80 passed, 155.51s**, `--parallel 14`, Xvfb/offscreen,
  accepted `CODEXUI_TIMING_POLICY=report`. Prior run: 162.12s. Live clients and a
  separate negative-control run overlapped this run; the duration difference is
  **not** a controlled performance improvement measurement.
- Final comment-only edit was rebuilt with `--parallel 14`; the focused Shell
  integration run passed again, **1/1 in 11.42s**. The command's additional wheel
  name regex matched no extra test; it is not counted as a separate wheel run.
- `git diff --check` passed. No commit, push, installation or upstream change.

Commands for the production verification:

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen CODEXUI_TIMING_POLICY=report \
  ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --output-on-failure
```

Evidence under `/tmp/codexui-final-g9DeRT/`: `projection-wake.log`,
`retained-v23.log`, `retained-v24.log`, `retained-v24-observer.log`,
`retained-v24-dpr1.5.log`, `retained-v24-dpr2.log`, `pre-fix-regression.log`,
`pagination-ctest.log`, `pagination-focused.log`, `history-switch.log`.
Captures under `/tmp/codexui-interactive-uCYB7o/captures/`:
`v24-controller-{first,last}-page.png`, `v24-observer-{first,last}-page.png`,
`v24-dpr{1.5,2}-{first,last}-page.png`, `v24-rapid-thread-switch.png`.

### Additional live checks — §§14, 20

- Controller rename and restoration propagated to the observer. The four-card
  content and count stayed unchanged; the observer's unsent draft survived and
  Send remained disabled. Evidence: `multifrontend-rename.log` and opened
  `v24-rename-observer.png`. This is not every concurrent lifecycle mutation.
- A copied disposable image was attached, then removed before Send. The real
  server accepted the text prompt and completed a turn; both frontends displayed
  `Image unavailable` with its filename. The opened capture
  `v24-attachment-removed-result.png` establishes the explicit missing-image
  presentation, not delivery of image contents or request rejection. Unreadable
  files and recoverable submission-error combinations remain separate cases.
- Driver mistakes were corrected without changing production behavior: the
  initial rename driver targeted assistant text `OK` instead of the dialog's
  `QPushButton`; the initial search assertion expected a nonexistent Beta thread.
  Corrected interactions/assertions use the actual dialog control and an existing
  Alpha thread. Neither initial failure is classified as an application defect.

## Continuation: v25/v27 — timestamps, accessibility actions, input-to-paint

Same HEAD and preserved production worktree as v24. No production or repository
test code changed in this continuation. Temporary diagnostic executables link
the current production libraries; only the external qualification driver gained
schema-extractor/diagnostic input, accessible actions and paint instrumentation.
All launches used Xvfb/offscreen with isolated XDG directories. These runs used
controlled inputs, not a real app-server: no production backend, credentials or
personal history was accessed. Qt's normal motion was enabled.

### Passed within the recorded conditions — §§19, 21, 22.9

- At DPR 1/1.25/1.5/2, executed 20 timestamp contexts × three value states
  (supplied, explicit missing, boundary): **240 context/state checks**. The
  20 contexts cover the 18 inventory families, with separate remote expiry and
  image-reset cases. All passed through the production schema extractor,
  diagnostic channel, Inspector formatting and displayed-text checks. Additional
  empty payloads exercised absent optional paths; they do not fabricate times.
- Verified seconds versus milliseconds, negative-duration rejection, file-zero
  sentinel, out-of-range dates, valid epoch zero and the two Vienna local
  02:30 times at the DST transition with distinct UTC offsets. Protocol Select
  All / Copy reproduced the displayed text at all four DPRs.
- All 60 individual DPR-1 case captures were opened in seven legible contact
  sheets and inspected: wrapped fields/units remain readable and inside the
  panel. Fractional-DPR full-shell/DST and DPR-2 command/focus captures were also
  inspected. This does **not** qualify every field's enlarged-font/narrow-window
  or retained-graph Timing presentation at all DPRs. The controlled diagnostic
  seam deliberately leaves local-record time unavailable; it does not prove
  ClientRuntime's real-server diagnostic provenance anew.
- At all four DPRs, **24 presented logical conversation rows** were revealed
  and focused using their actual accessibility actions; logical interface IDs
  remained stable and each revealed row had one resident renderer child. The
  25th row was Reasoning hidden by the presentation toggle and correctly lacked
  a focus action. Timing, Copy and disclosure accessible Press actions worked.
  These are Qt-interface checks, not complete application action traversal or
  real screen-reader interoperability.

Evidence: `/tmp/codexui-qualification-next-uQIYlU/{timestamps,a11y}-*.log`,
`timing-review-{1,2,3,4,5,6,7}.png`, and
`/tmp/codexui-interactive-uCYB7o/captures/v25-*-timestamp-*.png`,
`v25-*-a11y-*.png`.

### Input-to-paint measurement — §20 (not universal latency approval)

The v27 probe starts before queuing a wheel input, requires the target's
scrollbar to move, observes its viewport paint, then measures through the end
of the enclosing Qt UpdateRequest, including descendant paints. It does not
force a repaint. Earlier v26 measurements ended at viewport Paint and are not
used as whole-update measurements.

Each scenario sampled 32 inputs on threads, conversation, command output,
300-line prompt, Plan, Agents, Requests, Protocol, State and Timing. Fixtures
included 320 listed threads, 80 agents, 40 requests and 120 plan rows. Traffic
was controlled graph notifications, either status-only or status plus streamed
text; no new cards were created by the traffic. This does not measure real
transport/worker contention, physical input or compositor presentation.

| DPR | Traffic | Completed surface samples | Highest surface p95 | Worst sample |
| --- | --- | --- | --- | --- |
| 1 | Idle | 320 across 10 surfaces | 6.586 ms | 8.945 ms |
| 1 | Status only | 288 across 9 surfaces | 6.272 ms | 6.770 ms |
| 1 | Streaming | 288 across 9 surfaces | 6.308 ms | 9.154 ms |
| 2 | Idle | 320 across 10 surfaces | 7.797 ms | 8.085 ms |
| 2 | Status only | 288 across 9 surfaces | 7.726 ms | 7.736 ms |
| 2 | Streaming | 288 across 9 surfaces | 7.904 ms | 9.038 ms |

**The State-under-traffic scenarios failed and are excluded from these completed
sample counts, not counted as passed.** They led to the reproduction below.
These are single-run observations, not a production-change speed comparison.
Fractional-DPR latency, Changes/review/dialog input-to-paint and additional
traffic/keyboard configurations remain open.

Evidence: `v27-input-paint-1-corrected.log`, `v27-input-paint-2.log` under the same
new evidence directory. Initial driver attempts clicked unused tab-bar space
instead of Info; the corrected driver invokes the actual Info tab action.
Wheel direction was also corrected to avoid intentionally scrolling outward
at a boundary. The State failure remained after both driver corrections.

### Confirmed qualification failure — State and Timing reading position

At DPR 1 and 2, one controlled notification while reading selected text at the
bottom caused both the scrollbar and selection to reset:

| View / update | DPR 1 scrollbar | DPR 2 scrollbar | Selection |
| --- | --- | --- | --- |
| State / thread status | 682 → 0 | 674 → 0 | cleared |
| Timing / hook completion | 211 → 0 | 211 → 0 | cleared |

The QTextDocument identity remains the same. The exact cause is destructive
whole-content replacement by `InspectorPane::refreshState()` and the Timing
branch of `InspectorPane::refresh()` using `QPlainTextEdit::setPlainText()`.
The same-context refresh does not preserve reading position/selection. A second,
identical notification is a control: text, selection and scrollbar remain
unchanged, confirming that existing semantic no-op guards work.

The before/after full-shell captures were opened and inspected. Logs:
`info-reading-{1,2}.log`, `info-reading-noop-1.log`; captures:
`v27-1-{stateInfoView,timingInfoView}-{before,after}.png`.
This is a reading-state defect, not evidence of expensive rasterisation.

Proposed correction, not implemented: one shared same-context read-only-text
update policy preserving the view's reading state; retain deliberate resets
when thread/object context changes. Do not suppress incoming data or add a
timer, cache or independent follow controller. Deletion alone cannot preserve
both live information and reading state. Expected net production growth is
approximately 20–40 lines, subject to the required approval before editing.

Continuation cleanup: isolated diagnostic clients were closed through their
control sockets; both final v27 processes exited with status 0. No real backend
or authentication copy was needed in this continuation. Production/test LOC
delta: **0/0**; only this repository execution record changed. No commit, push,
installation or production-process restart. `git diff --check` passed.

## Approved Info refresh repair — §§19–20, 22.7, 22.9

The user approved the preceding 20–40-line proposal. The same starting worktree
was preserved. The correction is confined to `InspectorPane.cpp/.h`:

- State and Timing now update changed spans within individual Qt text blocks.
  Unchanged blocks, selections and wrapping remain owned by the existing editor.
  Related edits use one `QTextCursor` edit block; there is no scroll-position
  correction, extra follow controller, deferred callback or update suppression.
- Exact no-ops return before editing. Empty/new contexts use Qt's initial text
  assignment, retaining the established initial caret and scroll position.
  A different Timing target clears the previous context; the existing thread
  retirement clears both editors. Reopening the same target does not clear it.
- The redundant `stateSnapshot` byte cache and both destructive same-context
  assignments are removed. Protocol's separate follow/detach policy is unchanged.
- An intermediate whole-document replacement plus scrollbar restoration was
  rejected: wrapped State content moved from scrollbar 89 to 129. A single
  document-wide changed span also destroyed selection between two independent
  updates. Neither intermediate implementation remains. DiffViewer and UiStyle
  have no changes from this repair.

Incremental accounting against the saved pre-repair dirty files: production
**+47/-10 = +37 lines**, tests **+157/-0**. No persistent member was added;
one was removed. The one local update helper replaces both Info update policies.

### Correctness and live reproduction

The existing Inspector suite now exercises both reading surfaces at all four
DPRs: updated facts are actually displayed; document/focus and reverse selection
remain stable; independent changes surrounding a selected field preserve it;
no-op refreshes emit no document mutation; same-target reopening preserves state;
new thread/target resets start at the top; shrinking content clamps correctly;
UTF-16 and multiline/CRLF titles retain complete content; horizontal reading
position survives updates. The focused Inspector run passed **9/9 in 5.05s**
before the final additional changed-fact assertion.

Isolated diagnostic driver v32, run labels v33, links the final production
libraries. All clients used Xvfb/offscreen; controlled messages enter the actual
WorkerLogic/ProtocolUpdater/graph/presentation path. This is **not** an independent
real bridge/app-server network run, and does not measure worker-lock contention.

- At **DPR 1, 1.25, 1.5 and 2**, real mouse partial selections and Ctrl+A survived
  a changed status/hook notification in both editors. Clipboard text matched the
  retained selection, and repeated identical notifications changed nothing.
- State retained scroll **62** with the partial selection and **674** with all
  21,835 characters selected; Timing retained **48** and **218**, respectively,
  with 7,299 characters in the full selection. Document identity remained stable.
- Opened and inspected all **32 before/after captures** and **32 Inspector layout
  captures** (two surfaces × two fonts × two requested widths × four DPRs).
  Representative full-shell capture was also inspected. Selection, text and
  borders remained stable. Dates/raw values wrapped within the assigned panel.
  Transparent child captures were composited over the actual Inspector token
  background for the contact sheets, not interpreted as black UI backgrounds.
- Fonts were 9/16pt; requested shell widths 1100/1536. At 16pt, Qt's existing
  minimum layout grew beyond the requested 1060 height (Inspector height 1358).
  These captures qualify the **actual resulting geometry**, not fitting that
  enlarged layout into a 1060-pixel screen. Native screen/window-manager behavior
  remains unqualified.

### Four-DPR interaction measurements

Each table row contains 320 inputs: 32 each for threads, conversation, command
output, long prompt, Plan, Agents, Requests, Protocol, State and Timing. All
**3,840 inputs** caused the intended scroll movement and a measured viewport
paint, without missing samples. Incoming modes include notifications that create
no cards. Builds and automated suites did not overlap these measurements.

| DPR | Traffic | Worst sample (ms) | Highest per-surface p95 (ms) |
| --- | --- | ---: | ---: |
| 1 | idle | 6.681 | 6.575 |
| 1 | status-only | 6.964 | 6.365 |
| 1 | streaming | 6.356 | 6.324 |
| 1.25 | idle | 7.282 | 7.174 |
| 1.25 | status-only | 7.088 | 6.960 |
| 1.25 | streaming | 7.862 | 7.844 |
| 1.5 | idle | 7.509 | 7.459 |
| 1.5 | status-only | 7.522 | 7.465 |
| 1.5 | streaming | 7.503 | 7.410 |
| 2 | idle | 8.263 | 8.093 |
| 2 | status-only | 8.327 | 8.124 |
| 2 | streaming | 8.077 | 8.002 |

The metric remains queued input through completion of the enclosing UpdateRequest
containing the target paint, including child painting—not compositor latency.
Compared with the preceding v27 baseline, State-under-traffic now passes rather
than resetting. Other worst samples remain in the same range: DPR-2 idle/status
maxima were 8.085/7.736ms, now 8.263/8.327ms; streaming was 9.038ms, now 8.077ms.
These individual runs do **not** establish a statistically significant speedup,
zero regression on every machine, or universally lag-free interaction. Diff,
dialog and additional keyboard/burst cases remain open.

### Commands, evidence and runner corrections

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen CODEXUI_TIMING_POLICY=report \
  ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --output-on-failure
python3 /tmp/codexui-info-refresh-bOhqTi/run-live.py
```

The live runner launches every Qt process using Xvfb/offscreen, serializes DPR
runs, sets isolated XDG/control paths and quits its own clients afterward. No
production service, upstream repository, credentials or personal data were used.

Evidence: `/tmp/codexui-info-refresh-bOhqTi/` contains the pre-edit backups,
build/suite logs, `v33-*-input-paint.log`, `v33-*-reading.log`, the runner and eight
opened contact sheets. Original captures are under
`/tmp/codexui-interactive-uCYB7o/captures/v33-*`.

One earlier CTest run passed 79 cases but could not launch the Inspector executable
while it was being relinked (`permission denied`, BAD_COMMAND). This was a runner
ordering error, not a test assertion or Xvfb failure; the entire Inspector set
then passed. An initial mouse-selection probe hit a blank Timing line and failed
its setup assertion; it was corrected to select actual text before qualification.
Neither failed attempt is counted as successful evidence. The final frozen-build
run, `ctest-clean.log`, passed **80/80 in 165.63s**, with no overlapping rebuild or
live measurement. The earlier baseline was 155.51s; these whole-suite wall times
include serial performance cases and are not a controlled speed comparison.
`git diff --check` passed. All isolated diagnostic clients exited; no commit,
push, installation or production-process restart was performed.

## Continuation: Changes/diff qualification — §§17, 20, 22.7

Same `b9ff6e5` HEAD, preserved dirty worktree and final production libraries as
the Info repair above. Temporary v34/v35 diagnostic executables added only
external observation of `GitDiffProvider` loading/snapshot signals and timing
around Qt paint delivery. No production or repository test code changed.
Independent Xvfb/offscreen clients used fresh XDG configuration, normal motion,
and DPR 1/1.25/1.5/2. Inputs were controlled graph notifications, not a real
bridge/app-server. Git repositories, commits and changes were disposable
fixtures under `/tmp/codexui-diff-qualification-Bw1uro`; actual libgit2 and the
production diff provider, preview and review widgets processed them.

### Passed cases, not approval of all of §17

- At all four DPRs: All repositories and a single repository, Unstaged/Staged/
  Since HEAD, added/untracked, modified, deleted, staged renamed and binary files.
  Selecting files and changing scopes did not widen the Inspector.
- Selected-text Copy and whole-diff Copy matched the displayed content. The
  unified review matched the preview; side-by-side horizontal and vertical
  scrolling stayed synchronized. Compact/Expanded controls and review dismissal
  worked. A no-op polling refresh preserved the preview selection and scroll.
- Missing repositories displayed `Changes unavailable` with the reason; Copy
  and Open review were disabled. Selecting the valid workspace restored its
  changes without a stuck loading indication.
- Three outstanding-request cancellations per DPR: select the 400-file slow
  repository, observe `loadingChanged(true)` with no snapshot yet, then select
  the missing repository. No stale slow-repository snapshot was delivered;
  the current error snapshot arrived and loading ended. This exercises the
  existing generation-based cancellation, not a diagnostic replacement for it.

Full functional scripts: `v36-1-diff.log`, `v35-1.25-diff.log`, and
`v40-{1.5,2}-diff.log`. Cancellation evidence: `v39-1-probe.log`,
`v35-1.25-probe.log`, and `v40-{1.5,2}-probe.log`. The early v35/v36 glyph probes
failed their setup assertions after the cancellation checks; those failures do
not invalidate the separately completed cancellation cases and are not counted
as successful glyph measurements.

### Failed visual case: selected-file heading loses its width

At font 16 pt, requested shell width 1100 and actual Inspector width 300,
`repo-a / changed.txt` received only **4 logical pixels × 116 pixels**. It shows
clipped character fragments and reserves a tall, mostly empty header area.
The 404-pixel Inspector at shell width 1536 also clips the last word rather than
presenting a deliberate ellipsis. This is visible at all four DPRs; normal-font
file/status/error cases remain legible. Forty file/error/layout captures were
opened as eight contact sheets, with individual review captures also inspected.

Source cause: `DiffViewer` puts the word-wrapped `UiStyle::makeLabel` title and
both actions into one `QHBoxLayout`. The label correctly uses horizontal
`QSizePolicy::Ignored` to prevent panel growth, but the buttons consume almost
all remaining width at large fonts. That policy does not provide elision or
responsive rearrangement. Do not revert it to a minimum width that reintroduces
automatic panel widening. Proposed correction, not applied: reshape this
existing header layout so title and actions can occupy separate rows when
required, using Qt layout policy rather than offsets, resize timers or a new
geometry cache. Qualify both wide and narrow appearance before approval.

### Failed performance case: dense Unicode diff painting

Executed **768 wheel inputs**, 32 per surface × two surfaces (preview/unified
review) × three traffic modes × four DPRs. Traffic modes were idle, selected-
thread status updates creating no cards, and those updates plus text deltas to
another thread. This does not cover visible same-thread streaming or all burst
conditions. Every sampled scroll moved and all 32 completion samples per group
were received, but the dense Unicode fixture did **not** meet smooth-interaction
qualification. Maximum completed-update times across the three modes:

| DPR | Preview maximum ms | Unified review maximum ms |
| --- | ---: | ---: |
| 1 | 104.701 | 127.102 |
| 1.25 | 114.269 | 135.421 |
| 1.5 | 391.737 | 497.202 |
| 2 | 114.447 | 142.953 |

Instrumentation around the target viewport's actual Qt Paint event attributes
almost all this time to text painting, not queued message routing. These are
Debug/offscreen process observations, not compositor/hardware latency or a
controlled claim that DPR 1.5 itself causes the larger values.

A separate same-viewport content experiment at DPR 1 exposed the added lines of
otherwise equivalent 600-line changes (30 repeated text segments per line).
Twelve samples per content variant measured target Paint-event durations of
4.057–6.564 ms for ASCII, 26.118–29.339 ms for Greek, and 255.966–272.016 ms for
Greek plus emoji. Valid repeats at DPR 1.5 and 2 also showed the substantial
emoji cost. The exact Qt/font operation responsible remains **unresolved**;
do not claim a proven fix, introduce a parallel diff renderer, remove Unicode,
or hide the issue by relaxing limits. Further paint-path profiling is required
before choosing a production correction.

The first glyph probes never reached the document bottom using the attempted
Ctrl+End setup, so they produced no paint samples. The corrected probe used the
actual vertical scrollbar's End action and asserted its value was beyond row
500 before measuring upward scrolling; failed setups are retained in the logs.

Commands/scripts: `python3 .../build-driver.py codex-ui-interactive-v35`,
`python3 .../run.py` with isolated run labels, and `summarize.py` under the
temporary directory above. Launches remained `xvfb-run -a env
QT_QPA_PLATFORM=offscreen`, PassThrough rounding, with fresh per-run XDG roots.
No rebuild or test suite overlapped the measurements. All diagnostic clients
exited through their own control sockets. Production/test LOC delta this
continuation: **0/0**; only this execution record changed in the repository.
No commit, push, installation, credential access or personal-service restart.

## Continuation — approved Changes header-layout correction

Replaced the preview header's single horizontal layout with one grid: the
existing title spans the first row and the existing Copy/Open review controls
occupy the second row. This is deliberately two rows at both widths, not an
adaptive layout. The label retains its shared horizontal Ignored policy, so
the correction does not force Inspector widening. No new widgets, callbacks,
timers, caches or functional state were added. A QFormLayout prototype failed
because the ignored-width label received zero width; that prototype was removed.

Production delta: `DiffViewer.cpp` **+6 / −4, net +2 lines**, within the approved
eight-line allowance. Test delta: `GitChangesLiveTest.cpp` **+69 / −0**. The
regression checks title allocation, pane-width stability, action containment/
non-overlap, exact Copy content and disabled actions for an empty snapshot.
Its height-for-width assertion does **not** prove every character is visible;
the separate visual failure below demonstrates that limitation.

Verification commands:

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14 --output-on-failure -R 'git-changes-live|inspector|application-layout'
for dpr in 1.25 1.5 2; do
  xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR="$dpr" QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug/codexui-git-changes-live-test
done
```

Build passed; focused CTest **11/11 passed in 7.92 s**; the three additional DPR
runs also passed. The Inspector performance cases completed in 1.04–1.11 s;
these are suite durations, not frame timings or a before/after speedup claim.
The final build recheck reported no work to do. `git diff --check` passed.

Live production-widget driver `codex-ui-interactive-v42` ran exclusively under
Xvfb/offscreen with isolated configuration and disposable Git repositories.
`header.py` under `/tmp/codexui-diff-qualification-Bw1uro` exercised fonts 9/16,
requested shell widths 1100/1536, short/long Unicode filenames and all four
DPRs: **32 geometry/action scenarios**, 64 Inspector/full-shell captures.
All 32 Inspector captures were visually inspected through eight contact sheets;
not all full-shell captures were inspected. Copy matched the diff; keyboard
Space opened review, its content matched, and Escape dismissed it. The review
button's accessible name, actions and enabled state were checked. This is not
real screen-reader qualification. No production backend was used.

The reproduced short-filename collapse is corrected: at font 16 in the narrow
Inspector the title changed from **4×116** to **234×29 logical pixels**, and at
the wider setting it receives **346×29**. Both actions remain below it without
overlap. Requested shell width 1100 at font 16 is constrained by the existing
whole-shell minimum (approximately 1323); this is not proof of an actual
1100-pixel shell. Existing cramped repository/scope captions at that font were
not changed by this header repair.

**Residual visual failure:** a long unbroken filename is still clipped at the
right edge, without an ellipsis. QLabel's word wrapping does not split that
token, and its height-for-width result describes the same clipped layout.
Successful geometry/action assertions therefore do not close full filename
legibility. No inserted whitespace, forced width, resize callback or alternative
renderer was added to conceal it. A further text-presentation correction needs
separate scope/approval; the entire Changes visual qualification remains open.

Evidence: `v42-1-probe.log`, `v43-{1.25,1.5,2}-probe.log`, and
`v44-{1,1.25,1.5,2}-probe.log` plus `header-{short,long}-*.png` contact sheets in
the temporary directory above. All header clients exited. No commit, push,
installation, credential access or personal-service restart occurred. The
dense-Unicode painting failure was not modified or reclassified by this repair.

## Continuation — long-filename root cause and correction options

Investigation only; production/test delta this continuation **0/0**. HEAD is
still `b9ff6e5`; the preceding uncommitted header repair and all other worktree
changes were preserved.

Trace: `GitDiffProvider` supplies the unchanged file path; `fileTitle()` creates
the optional repository prefix; `DiffViewer::showSelectedFile()` assigns it to
the shared `UiStyle::makeLabel` title. That factory explicitly selects plain
text, mouse selection and word wrapping. Qt 6.10.2's
`QLabelPrivate::ensureTextLayouted()` sets its document to `QTextOption::WordWrap`,
not `WrapAtWordBoundaryOrAnywhere`. Its width measurement uses that same
document. Thus geometry assertions can pass while an unbroken token remains
wider than the widget. This is not filename truncation in Git or stale data.
The public QLabel API exposes no alternative word-wrap mode.

Source reference:
[Qt 6.10.2 QLabel implementation](https://github.com/qt/qtbase/blob/v6.10.2/src/widgets/widgets/qlabel.cpp#L1417).

An isolated diagnostic at `/tmp/codexui-filename-wrap-fxUf4w/probe.cpp` compared
the actual selectable plain-text QLabel with a Qt wrapping text view and a
read-only QLineEdit. It ran through Xvfb/offscreen at DPR 1/1.25/1.5/2, fonts
9/16 and content widths 234/346: **16 combinations**, all final runs exit 0.
At font 16 and width 234 the QLabel's longest laid-out line is **1289.47 px**;
the wrapping view's longest line is **216.109 px**. Both alternative widgets
preserve exact selected Copy text; the read-only field accepts Home/End
navigation and rejects typed mutation. Narrow enlarged-font captures at DPR 1
and 2 were opened and inspected; this is a toolkit experiment, not full-shell
qualification or a production implementation. The wrapping prototype uses
explicit diagnostic sizing; production cannot adopt that as a resize workaround.

The initial probe build failed on GCC 16 warnings in Qt headers. Building with
the same system-header treatment used by the application resolved that; adding
keyboard interaction required the Qt Widgets test feature definition. Runs of
an older probe after a failed build are not evidence for the new keyboard
checks. The final rebuilt probe and four successful final invocations include
those checks.

Two legitimate presentation policies remain, requiring a scoped choice:

- **Recommended narrow option:** replace only the filename label with a standard
  read-only, horizontally navigable path field. Keep the path unchanged and
  selection/Copy handled by Qt, retain one-line bounded geometry, and preserve
  the existing buttons/grid and Inspector width. Estimated additional production
  growth **12–20 lines**, including declaration, initialization, styling and
  resetting the cursor on a changed path; estimated regression coverage 40–70
  test lines. This visibly changes a heading into a read-only field and needs
  approval beyond the preceding layout-only correction.
- **Wrapping heading:** use a read-only text view with native
  `WrapAtWordBoundaryOrAnywhere` and layout-driven height-for-width. This retains
  multiline presentation but needs more geometry integration and can consume
  substantial vertical space for long paths. It is not a one-line QLabel fix.

Neither option adds separators to filenames, mutates Qt private objects, starts
resize timers or forces panel widening. No production option has been applied;
full filename qualification and the separate dense-Unicode paint issue remain
open. No backend, personal process, installation or credentials were touched.

## Continuation — approved read-only filename field

Implemented the approved narrow option. `DiffViewer` now uses a standard
read-only QLineEdit instead of the filename QLabel. The existing grid/buttons
are unchanged; the field uses UiStyle's title typography and ordinary line-edit
focus/selection styling. Qt owns horizontal navigation, selection and Copy.
The exact filename remains the field value, newly selected paths start at
position zero, and empty/invalid selection clears the value and shows a
placeholder. No character injection, elided source copy, custom renderer,
new callback, timer, cache or functional flag was introduced.

This continuation: **net +11 production lines**, below the approved maximum
20; **net +40 test lines**. This is incremental to the preceding +2-production/
+69-test header repair, excluding all unrelated pre-existing edits. The only
UiStyle change in this continuation extends the existing title selector to the
new field; the other uncommitted UiStyle changes were preserved.

Verification on the final implementation:

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14 --output-on-failure -R 'git-changes-live|inspector|application-layout|ui-style'
for dpr in 1.25 1.5 2; do
  xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR="$dpr" QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug/codexui-git-changes-live-test
done
```

Build passed; **12/12 focused tests passed in 7.66 s**; the three additional DPR
runs each returned 0. Coverage includes full/partial filename Copy, Home/End,
rejected typing/deletion/paste, unchanged-snapshot selection/focus, accessible
read-only state/full value, width containment and empty-state clearing. The
Inspector performance gates passed, with suite durations 1.00–1.06 s versus
1.04–1.11 s in the preceding record. These durations are not a controlled
header latency comparison or a claim of an application-wide speedup.

Full-application diagnostic runs used `codex-ui-interactive-v45`, production
libraries, the existing fixture graph and real disposable Git repositories,
never a personal backend. Runs v45 (long filename) and v46 (short filename)
each covered four DPRs × fonts 9/16 × requested shell widths 1100/1536:
**32 live scenarios, all passed**. Every run checked filename Home/End, exact
Copy, rejection of edits, selection surviving an actual polling interval,
unchanged diff Copy, keyboard Open review and matching review text. The field
remained 34 logical pixels high in the captured configurations; no filename
length-dependent vertical growth or Inspector widening occurred. As before,
the enlarged-font shell minimum constrains requested width 1100; this does not
qualify an actual 1100-pixel enlarged-font shell.

The 32 Inspector captures were opened as eight contact sheets. All 48 long-path
Home/End/selected captures were opened as four additional contact sheets, plus
an individual enlarged-font narrow full-shell capture. The complete `.cpp`
suffix is visibly reachable through End at every tested DPR/font/width; focus
and selected text remain readable. Other full-shell captures were retained but
not all inspected. Existing cramped repository/scope captions at enlarged fonts
are unchanged and are not approved by this filename-only result.

Live commands were `QUAL_RUN=v45 QUAL_BINARY=codex-ui-interactive-v45
QUAL_SETUP_ONLY=1 QUAL_HEADER_ONLY=1 QUAL_LONG=1 python3
/tmp/codexui-diff-qualification-Bw1uro/run.py 1 1.25 1.5 2`, then the same with
`QUAL_RUN=v46` and without `QUAL_LONG`. The runner launches Xvfb/offscreen with
isolated configuration. Evidence resides in `v45-*-probe.log`,
`v46-*-probe.log`, `header-{short,long}-*.png` and `filename-navigation-*.png`
under that temporary directory, with source captures under
`/tmp/codexui-interactive-uCYB7o/captures`. Both run groups exited; no v45 client
remained in the final process check. `git diff --check` passed.

**The selected-filename clipping case is closed under the approved scrollable
field contract.** This does not close the full canonical matrix or the separate
dense-Unicode diff-painting failure. No commit, push, installation, credentials
access or personal-process restart occurred.

## Continuation — dense-Unicode paint isolation, v47–v49

Status: **performance failure reproduced; no production correction applied**.
HEAD remains `b9ff6e5d0d4115922174c1a5fdd4310537c7975b`, with the previously
recorded dirty worktree. All GUI probes used Xvfb/offscreen; no personal backend,
system installation or upstream repository was changed.

The v49 full-application run used the same production libraries as v45, a
fixture graph, and real disposable Git repositories. Its three outstanding-diff
cancellation checks passed. Twelve upward-wheel samples for each 600-line
fixture measured these target-widget paint ranges at DPR 1:

| Content | Target paint | Completed update |
| --- | --- | --- |
| ASCII | 1.354–4.646 ms | 3.260–10.249 ms |
| Greek | 6.223–14.373 ms | 8.244–17.843 ms |
| Greek plus emoji | 80.538–81.585 ms | 82.282–83.425 ms |

These are isolated glyph-content comparisons, not incoming-traffic or
application-wide latency approval. Command:
`QUAL_RUN=v49 QUAL_BINARY=codex-ui-interactive-v49 QUAL_SETUP_ONLY=1 python3
/tmp/codexui-diff-qualification-Bw1uro/run.py 1`.
Seed, diff, probe and client exit all returned 0. Evidence is in `v49-1-*.log`.

### Driver correction and invalid shutdown evidence

Inspection found allocator-abort messages on teardown in both v47 and v48
(including the run without gprofng). The temporary driver's `diffEvents` and
`diffClock` locals were declared after its window. They therefore died before
window destruction, while provider callbacks still referenced them.
`GitDiffProvider::~GitDiffProvider()` calls `cancel()`, which emits
`loadingChanged(false)`. Moving those diagnostic locals before the window
corrected their ownership order. The runner now also checks the client's exit
status, not just the interaction scripts. v49 repeated the scenario and exited
cleanly. v47/v48 are **not clean-shutdown passes**; this evidence does not
establish a production teardown defect. No production teardown guard was added.

### Toolkit isolation and proposed correction boundary

A standalone read-only, unwrapped `QPlainTextEdit` reproduces the slowdown
without CodexUI, incoming messages or a syntax highlighter. Callgrind attributes
93.12% of measured instructions to the `FT_Load_Glyph` call subtree and 85.79%
to `png_read_image`. These are instruction proportions, not wall-time shares.
The stack passes through `QTextLayout::draw`, shaping and
`QFontEngineFT::recalcAdvances`.

In [Qt 6.10.2's FreeType engine](https://raw.githubusercontent.com/qt/qtbase/v6.10.2/src/gui/text/freetype/qfontengine_ft.cpp),
initialization disables its glyph cache for bitmap/color fonts (line 965).
`recalcAdvances` requests metrics-only glyph loading (lines 2374–2379), but
`loadGlyph` calls FreeType without the bitmap-metrics-only flag (line 1712).
Thus measuring the bitmap emoji repeatedly decodes its PNG. This matches the
profile; it is not evidence for another CodexUI scheduler change.

FreeType provides [FT_LOAD_BITMAP_METRICS_ONLY](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html)
for obtaining bitmap metrics without loading the image. A standalone probe
using the installed Noto Color Emoji font compared 10,000 loads of U+1F642:
599,634 microseconds normally versus 324 with that flag, with all eight
`FT_Glyph_Metrics` fields equal on every iteration. This is supporting evidence
for an isolated Qt correction, **not validation of a patched Qt or whole UI**.
The proposed source boundary is Qt's existing metrics-only load, not an
application-side glyph cache, font substitution or second renderer. Any patch
must preserve actual bitmap loading for drawing and qualify other font types,
transforms and all four DPRs. No upstream edit is authorized or applied yet.

Enabling Qt's layout cache was tested only in the standalone diagnostic. With
highlighting and three-line scroll steps, later repaint times were 15.960–16.749
ms at DPR 1, 18.489–19.952 at 1.25, 18.801–22.198 at 1.5, and 19.229–19.476
at 2. The first repaint still took 101–107 ms. This does not establish a complete
fix; no cache was added to CodexUI. Disabling kerning, optional shaping or
switching to design metrics also failed to remove the dominant cost. Disabling
emoji parsing was diagnostic only and is not an accepted appearance change.

Probe sources, binaries, logs and Callgrind data are under
`/tmp/codexui-diff-paint-vy6YDo`. Compilation used `g++ -std=c++20 -O2 -g
-Wall -Wextra -Werror` with Qt6Widgets or FreeType pkg-config flags. GUI runs
used `xvfb-run -a env QT_QPA_PLATFORM=offscreen`, with
`QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough` for the four-DPR comparison.
The profile used `valgrind --tool=callgrind --instr-atstart=no`; instrumentation
was restricted to eight repaints. Performance runs were serialized.

This continuation changes **0 production lines and 0 repository test lines**.
Only temporary diagnostic code and this evidence report changed. No commit,
push, install or personal-process restart occurred. The full canonical
qualification remains unfinished; the Qt performance failure is not waived.

## Continuation — attachment rejection and retry, four DPRs

Sections 14 and 22.6: the unchanged production application was driven through
actual prompt drag/drop with isolated fixture state at DPR 1/1.25/1.5/2,
normal font and 1536×960 logical shell. Sixteen scenarios passed: an unreadable
file, directory and nonexistent path were each rejected without removing the
previous attachment or authored draft, emitting a prompt submission, or leaving
Send disabled; a subsequent valid file addition succeeded at each DPR.
Each client exited with status 0. The unreadable file was a new disposable
fixture whose permissions were changed to 000, not a personal file.

The diagnostic label `removed` refers to a nonexistent input path. It does
**not** prove recovery when an already attached file disappears during or before
server submission. That distinct boundary remains open. No real backend was
used in these checks.

Command: `python3 /tmp/codexui-attachment-recovery-R0itks/run.py 1 1.25 1.5 2`.
Sources and logs are in that directory. Sixteen full-shell captures were saved
under `/tmp/codexui-interactive-uCYB7o/captures/attachment-recovery-*`.
Four were opened: DPR 1 unreadable, DPR 1.25 directory, DPR 1.5 missing path,
and DPR 2 successful retry. Notices, retained attachment labels, authored text,
removal controls and Send were visible without overlap in those captures.
The other captures are retained, not claimed individually visually approved.
This is not enlarged-font or narrow-panel qualification. Production/test LOC:
0/0; temporary diagnostic scripts and this report only.

## Continuation — approved isolated Qt bitmap-metrics correction

The user subsequently authorized the proposed Qt correction in an isolated
build, not installation. Qt 6.10.2's `QFontEngineFT::loadGlyph()` now adds
`FT_LOAD_BITMAP_METRICS_ONLY` when `fetchMetricsOnly && !embolden`. Actual
painting still loads bitmap pixels. Synthetic emboldening retains the original
path because it may need bitmap data when calculating adjusted metrics.
This removes redundant PNG decoding at its metrics-only source; no CodexUI
renderer, cache, timer, font substitution or layout workaround was added.

Source, patch, scripts and logs are retained at
`/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz`.
`qt-bitmap-metrics.patch` is an applicable upstream-source patch, not an installed
library. The official Qt 6.10.2 archive SHA-256 is
`aeb78d29291a2b5fd53cb55950f8f5065b4978c25fb1d77f627d695ab9adf21e`.
Builds used `cmake --build build --target Gui Widgets Network Test
QOffscreenIntegrationPlugin --parallel 14`; the patched rebuild targeted `Gui`.
Xcb, EGLFS, LinuxFB, OpenGL and Vulkan were disabled. The offscreen plugin was
built and all GUI checks ran under Xvfb with `QT_QPA_PLATFORM=offscreen`.

The first full-client run failed at QApplication startup: the isolated Qt
defaulted to `reduce_relocations=ON`, unlike the installed distribution Qt and
the already-built application. This was not counted as a pass. Both matched
baseline and patched Qt builds were reconfigured with
`-DFEATURE_reduce_relocations=OFF`. No application code or build was changed to
hide that incompatibility. The compatible full-client runs v52 (unpatched) and
v53 (patched) subsequently all exited with status 0.

`run-matrix.py baseline-compatible`, `run-matrix.py patched-compatible` and
`compare-matrix.py baseline-compatible patched-compatible` compared 192 images:
three fonts, two sizes, normal/bold-italic, selection, narrow layout and rotated/
scaled/sheared painting, at all four DPRs. Images were pixel-identical and
recorded geometry, text, font and scrollbar metrics matched. Mixed content
included Latin, Greek, emoji sequences, Arabic, Hebrew, Chinese and combining
characters. Standalone highlighted three-line scroll painting fell from
99.6–116.5 ms to 11.2–15.6 ms. These are serialized measurements of this corpus,
not a universal frame-time guarantee.

Full CodexUI Changes-pane runs used the unchanged v49 diagnostic client with
isolated library/plugin paths and disposable Git repositories. For each DPR,
12 wheel events per ASCII/Greek/emoji content type were observed, alongside
three outstanding diff cancellations. Dense-emoji target paint ranges:

| DPR | Unpatched Qt (ms) | Patched Qt (ms) |
| --- | --- | --- |
| 1 | 80.043–84.247 | 10.621–19.031 |
| 1.25 | 82.986–84.372 | 11.461–19.490 |
| 1.5 | 82.839–85.774 | 11.598–21.977 |
| 2 | 83.787–88.364 | 11.701–19.095 |

Completed update ranges with patched Qt reached 26.205 ms; therefore this does
not establish a strict 16.7 ms maximum or universal smoothness. ASCII/Greek
ranges and complete measurements are retained in `full-summary.py` output and
v52/v53 probe logs under `/tmp/codexui-diff-qualification-Bw1uro`.
This comparison had no incoming protocol traffic. Compatible narrow DPR-1.5
and transformed DPR-2 diagnostic captures, plus full-client Changes-pane DPR-1
and DPR-2 emoji captures, were opened and inspected; their text, selection and
controls remained readable without new visual artifacts.

LOC: isolated Qt production **+2**, CodexUI production **0**, repository tests
**0**. Temporary diagnostic programs and this report are separate. The installed
Qt, production applications and personal backends remain untouched. Deployment
and remaining canonical qualification are not complete.

Broader regression command (all 80 cases executed):
`xvfb-run -a env QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH=<isolated>/build/lib
QT_PLUGIN_PATH=<isolated>/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins
ctest --parallel 14 --output-on-failure --output-log
<isolated>/ctest-patched-compatible.log`.
The first run was **75 passed / 5 failed**, 149.31 seconds. Four failures were
Breeze selection/focus variants: installed Breeze transitively loads the
installed Qt Wayland client library, which required the Vulkan private symbol
`QBasicPlatformVulkanInstance` omitted from the isolated Qt configuration.
Plugin discovery alone was insufficient. This is an isolated-build compatibility
failure, not a passed Breeze qualification or evidence of a glyph regression.
The local Qt build is being aligned for these plugin dependencies; no Wayland
or xcb GUI test is authorized or used.

The fifth failure was the adapter's 8 ms aggregate-projection limit at 10,000
threads: 16.915 ms in the parallel run, 14.697 ms when run alone with patched Qt,
and **14.691 ms when run alone with installed Qt**. Logs:
`adapter-serial.log` and `adapter-installed.log`. That comparison does not
support attributing this failure to the Qt glyph change. It remains a recorded
timing failure; no production change or weakened assertion was made.

The additional isolated build enabled Vulkan/OpenGL/EGL to supply the first
missing private symbol. An initial incremental automoc build missed the newly
enabled OpenGL header; refreshing that source header's timestamp regenerated
the moc output and the build succeeded without another source patch. Breeze
still could not load. A complete linker audit (`ldd -r` against installed
`breeze6.so`, with isolated `LD_LIBRARY_PATH`) identified remaining dependencies
on `QSpiAccessibleBridge` and `QX11Info` private symbols. All four Breeze checks
remain failed/unqualified; a discovered plugin name is not a usable plugin.
Command-output variants that returned success despite Qt's style fallback are
also **not** evidence that Breeze rendering was exercised.

`libatspi2.0-dev_2.62.0-2_amd64.deb` was downloaded and unpacked only into
`<isolated>/dependencies/atspi`; no package was installed. Its local pkg-config
include path was adjusted for that extraction. Its `xres` development dependency
is also absent, so CMake reset the requested ATSPI feature to OFF. No stub,
suppressed plugin failure or application workaround was introduced. Qt Xcb
remains disabled. Extending this isolated build with the X11 compatibility
symbols required by KDE is explicitly distinct from running an xcb GUI; the
authorized GUI workflow remains Xvfb/offscreen. This boundary remains open.

The original matched patched libraries were preserved as
`<isolated>/patched-metrics-lib` before these plugin-compatibility rebuilds;
`baseline-lib` is the corresponding minimal unpatched configuration. Existing
v52/v53 measurements and the 192-image comparison used matching configurations
at execution time. The later `build/lib` has different graphics features:
do not compare it to `baseline-lib` as an identical-configuration experiment
without rebuilding the baseline. The source patch remains the same two lines.

## Continuation — timing surfaces and narrow Inspector header

Using installed Qt and disposable fixture clients, 48 combinations were driven:
supplied / null / invalid lifecycle timestamps × 9/16 pt × requested shell
widths 1536/1100 × DPR 1/1.25/1.5/2. All four clients exited 0. The application
may enforce its own minimum width; 1100 is the requested width, not an assertion
that the final enlarged-font shell was exactly 1100 pixels wide.

Checked both visible card timing controls, no header-sibling rectangle overlap,
focusability, duration raw units, DST-repeated local times with CEST/CET,
explicit invalid/null descriptions, valid thread epoch, item/turn/thread Timing
details, full selected-text Copy and clearing stale context on thread switch.
The script clicked one of the two controls per combination; it does not claim
complete per-control accessibility action traversal or timing-only update
stability. Fixture cases are not real app-server timestamp qualification.

Command: `QUAL_RUN=timing-surfaces-v2 python3
/tmp/codexui-timing-surfaces-JBfjvO/run.py 1 1.25 1.5 2`.
Logs are in that directory; 96 card/Inspector captures are retained under
`/tmp/codexui-interactive-uCYB7o/captures/timing-surfaces-v2-*`.
Opened captures: supplied DPR 1 / 16 pt / narrow cards and details;
missing DPR 2 / 9 pt / wide details; invalid DPR 1.25 / 16 pt / narrow details.
The narrow enlarged-font Inspector captures **fail visual approval**: Hide is
clipped and the remaining Timing reading area is severely compressed. Numeric
geometry assertions did not detect the clipped button text.

Exact header cause was confirmed with an additional four-DPR disposable-client
experiment. `InspectorPane.cpp` explicitly sets Hide's minimum to 58×24; at
16 pt its intrinsic minimum-width hint is 71 but the allocated width is 58.
Clearing only the explicit minimum-width override in the disposable client
allows 71 and removes the clipping. The DPR-1 corrected capture was opened.
No production source was changed by that diagnostic action.

The narrow source correction proposed is to replace
`hide->setMinimumSize(58, 24)` with `hide->setMinimumHeight(24)`, leaving width
under Qt's existing font/style-aware sizing authority. It introduces no state,
timer or new layout policy and is production-LOC neutral. It is not yet applied
or marked complete. Exact expanded-font Info readability and other narrow
combinations remain part of the broader visual matrix, not silently passed.

## Continuation — approved X11/AT-SPI support in isolated Qt

The user authorized compiling the support required by Breeze, while retaining
Xvfb/offscreen exclusively. No dependency was installed. Development packages
were downloaded and extracted under the isolated Qt directory's
`dependencies/sysroot`; pkg-config prefixes were mechanically relocated there,
and local linker symlinks point to existing runtime libraries. The system package
database, running applications and personal backends were not modified.

Configuration now has `FEATURE_xcb`, `FEATURE_xcb_xlib`,
`FEATURE_xkbcommon_x11`, `FEATURE_accessibility_atspi_bridge`, Vulkan, OpenGL and
EGL enabled. `FEATURE_reduce_relocations=OFF` still matches the application;
`QT_QPA_DEFAULT_PLATFORM=offscreen` is explicit. The old failed Xcb configure
probe was rerun after supplying its dependencies. Enabling the features also
required regenerating the isolated Gui automoc cache. No Qt source change was
needed beyond the approved two-line glyph correction.

Commands and logs are retained under
`/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz`:

- Configure: `PKG_CONFIG_PATH=<isolated>/dependencies/sysroot/usr/lib/x86_64-linux-gnu/pkgconfig
  cmake -S qtbase-everywhere-src-6.10.2 -B build
  -DCMAKE_PREFIX_PATH=<isolated>/dependencies/sysroot/usr
  -DFEATURE_xcb=ON -DFEATURE_xcb_xlib=ON -DFEATURE_xkbcommon_x11=ON
  -DFEATURE_accessibility_atspi_bridge=ON -DQT_QPA_DEFAULT_PLATFORM=offscreen`.
  `configure-x11-atspi-recheck.log` records the fresh Xcb probe; the final cache
  confirms every requested feature is ON, not silently reset to OFF.
- Build: `cmake --build build --target Gui Widgets Network Test
  QOffscreenIntegrationPlugin --parallel 14`; `build-x11-atspi-retry.log`.
- Link audit: `LD_LIBRARY_PATH=<isolated>/build/lib ldd -r
  /usr/lib/x86_64-linux-gnu/qt6/plugins/styles/breeze6.so`;
  `breeze-link-audit-complete.log` contains no unresolved symbols.
- GUI checks: `dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=offscreen
  LD_LIBRARY_PATH=<isolated>/build/lib
  QT_PLUGIN_PATH=<isolated>/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins
  ctest --parallel 14 -R selection-focus-breeze --output-on-failure`.
  All **four Breeze selection/focus DPR variants passed**; the tests explicitly
  reject a style fallback. The private D-Bus session is separate from the desktop.

For a new matched comparison, the correction was temporarily removed only from
the isolated Qt source, rebuilt and copied to `baseline-x11-lib`, then restored
and rebuilt into `build/lib`. Both have identical feature configuration.
Comparison scripts used this new pair, not the earlier minimal baseline.
`comparison-x11.log` reports **192 pixel-identical images** and matching
text/font/geometry/scroll metrics across all four DPRs. Standalone highlighted
scroll painting was 102.7–120.4 ms baseline and 11.3–16.3 ms patched.

An extra Breeze image run exposed a diagnostic runner bug: it replaced the
supplied plugin path and fell back to Fusion. Its verified isolated process
group was stopped; none of those partial captures count as Breeze evidence.
The runner now preserves the supplied plugin path, and its diagnostic executable
asserts both the requested style and the offscreen platform. The replacement
`baseline-breeze-confirmed` / `patched-breeze-confirmed` comparison passed:
**another 192 pixel-identical images**, with matching metrics. The baseline
paint range was 99.7–117.9 ms; patched 11.3–16.4 ms. No fallback warnings occurred.

Full-client comparison v55/v56 used the unchanged v49 CodexUI diagnostic binary,
disposable Git repositories and no incoming protocol traffic. All eight clients
exited 0. Each performed three outstanding-diff cancellation scenarios and
12 measured wheel events for each of ASCII, Greek and emoji content.

| DPR | Baseline emoji paint (ms) | Patched emoji paint (ms) |
| --- | --- | --- |
| 1 | 80.548–82.257 | 9.573–9.797 |
| 1.25 | 83.065–88.184 | 11.706–11.998 |
| 1.5 | 83.177–84.662 | 11.757–19.757 |
| 2 | 84.406–85.700 | 12.161–22.956 |

Completed updates still reached 28.290 ms. The measured improvement is verified;
universal smoothness or a 16.7 ms worst-case bound is not. Full-client DPR-1.25
emoji, default-style DPR-1.5 selected bold/italic text, Breeze DPR-1 selected
mixed text and Breeze DPR-2 transformed mixed text captures were opened and
inspected without new rendering defects. Pixel equality across the corpus is
separate from manually opening every image.

CodexUI production/test LOC change for this compatibility-build stage: **0/0**.
The isolated Qt correction remains **+2 production lines**, verified against
the original archive and by reverse patch dry-run. No commit, push, installation
or personal-process restart occurred. Overall canonical qualification remains open.

The full 80-case regression run then completed in **148.91 seconds: 79 passed,
one failed** (`ctest-x11-atspi-complete.log`), with 14-way execution where
permitted and the registered performance cases serialized. All Breeze cases now
passed, including command-output geometry. The only reported failure was the
already-recorded 10,000-thread adapter token-projection time bound (16.074 ms
against 8 ms during this run), also reproduced previously with installed Qt.
No assertion, timeout or timing policy was changed to turn that failure green.
This run preceded the separate Inspector-header/lifetime qualification below.

## Continuation — Inspector caption and font/style lifetime, v57–v60

The Inspector correction above is now applied: `setMinimumHeight(24)` replaces
`setMinimumSize(58, 24)`. Qt again owns the caption's font-aware minimum width.
The focused full-client check passes at all four DPRs (`header-font-fixed-*`):
16 pt Hide is 71 px wide, not 58. The DPR-1.5 capture was opened; the caption and
Inspector heading are complete. A provisional standalone Inspector test did not
reproduce the full-shell constraint and was removed, not counted as proof.
Existing Inspector, layout, Git and shell checks passed 12/12 before the next
correction. No earlier repository tests were removed.

The expanded live sequence (Hide / restore / font 9→16→9, wide and narrow)
exposed a **pre-existing conversation lifetime defect**, not a Qt metrics-patch
regression. v57 crashed at every DPR; the pre-fix v54 binary also crashed.
It reproduced with installed Qt and the isolated Qt build. GDB established:

1. Application-font handling regenerates the shared stylesheet.
2. Qt's `updateObjects` traverses its snapshot of styled widgets.
3. `QAbstractItemView::event` handles a style event and resizes its viewport.
4. `ConversationView::resizeEvent` immediately reflows; restoring the scrollbar
   synchronously materializes/releases rows.
5. Qt subsequently accesses a deleted child. The observed faulting ImageRibbon
   address exactly matches the earlier destructor trace.

The existing `geometryEnvironmentReflowPending_` was set **after** base event
handling and did not prevent resize reflow. The correction sets it before base
handling and makes `reflowAfterResize` honor the already-pending environment
pass. No new flag, callback, timer, cache, renderer or blanket deferred deletion
was added. Normal presentation/materialization is not blocked by that flag.
An initial broader materialization guard failed three existing tests and was
discarded. A dispatch-only guard passed those tests but did not stop the live
crash and was also discarded. Only the resize-boundary correction remains.

The existing runtime-geometry test now changes scrollbar width as well as font,
forcing a stylesheet-driven viewport resize, and checks that resident widgets
survive the synchronous repolish. With only this production correction
temporarily removed, the strengthened suite reproduced SIGSEGV. Restoring it
passes the complete conversation-virtualization suite, including retained
selection, anchors, staged publication and command-output state. User-owned
changes were preserved during this controlled comparison.

Final live v60 results, using installed Qt, disposable fixtures and private
D-Bus/XDG state:

- **24/24** Hide/restore/font/width combinations across four DPRs pass; every
  client exits 0. Hide remains font-sized and retains widget identity.
- **48/48** supplied/null/invalid timestamp × font × width × DPR checks pass,
  including both card timing-control geometry checks, Inspector text/Copy and
  unrelated-thread context clearing. Every client exits 0.
- The enlarged-font/narrow DPR-1 full shell was opened: Hide is no longer
  clipped. Timing text remains heavily wrapped in the narrow panel; this is not
  a blanket usability approval of all enlarged-font Inspector states.

Evidence directory: `/tmp/codexui-timing-surfaces-JBfjvO`.
Commands: `cmake --build ../build/Desktop_GCC-Debug --parallel 14`;
`dbus-run-session -- env QUAL_RUN=header-resize-fixed
QUAL_BINARY=codex-ui-interactive-v60 QUAL_SCRIPT=header-fixed.py python3
/tmp/codexui-timing-surfaces-JBfjvO/run.py`;
the timestamp repeat uses `QUAL_RUN=timing-surfaces-v60` and `checks.py`.
Every driver launch itself uses `xvfb-run -a env QT_QPA_PLATFORM=offscreen`.
Logs include `header-{old,new-qt,deletion,address,release}-debug-1-ui.log`,
`environment-regression-baseline.log`, `environment-final-fixed.log`,
`header-resize-fixed-*-checks.log`, and `timing-surfaces-v60-*-checks.log`.

Incremental repository changes in this stage: **−2 production physical lines,
+8 test lines**. Inspector sizing is LOC-neutral; conversation ordering/guard
changes remove two lines. No installation, commit, push, personal-backend use or
process restart occurred. The final full suite with isolated Qt and confirmed
Breeze completed **79/80 in 154.40 seconds** (`final-full-suite.log`). All 33
registered performance cases passed, including all 12 conversation size/DPR
variants; the only failure remains adapter token projection, 16.945 ms against
8 ms. Earlier matched-feature full-suite time was 148.91 seconds; this aggregate
difference is not a controlled microbenchmark or proof of changed hot-path cost.
The full suite's strengthened runtime-geometry case passed. No thresholds were
changed. The whole canonical matrix remains open.

## Continuation — broader accessible-action traversal, v61/v62

The diagnostic driver now queues accessible actions on the GUI event loop so
modal actions do not hold its control-socket response. This is instrumentation
only; production action dispatch is unchanged. Using disposable fixture clients
at all four DPRs, Inspector Hide/restore, every one of its five tab selections,
all three Info choices and their Back actions worked through Qt accessibility.
Each run collected 395 visible-control/interface observations across nine UI
states; these are repeated observations, not 395 distinct controls. All clients
exited 0. No real screen reader or desktop bus was used.

The first runner incorrectly expected hidden, disabled tab-scroll buttons to
offer Press; it was corrected to enumerate actual PageTab children. The corrected
four-DPR runs still **fail accessible naming qualification**:

- State, Protocol and Timing choice buttons have empty accessible names. Their
  child labels contain the text, but the buttons have no Label relationship.
- `stateInfoView`, `protocolInfoLog` and `codexDiffText` have empty accessible
  names and no labeling relationships.

The relation-aware v62 repeat confirms the missing labels. It also filters a
false positive: Qt's editable Model combo has an unnamed internal QLineEdit,
but the owning combo has the proper `Label: Model` relationship. Qt's Linux
implementation deliberately reports the current combo text as Name and relies
on relations for its label; no combo rewrite is justified by this probe.

Source authority is narrow: `infoChoice(title, description)` renders child
labels but never assigns the button name; the State/Protocol constructors omit
their text-view names; `diffView(objectName)` only assigns an internal QObject
identifier. That last helper also constructs Unified/Before/After review views,
so the same naming policy must cover all four consumers. Those three dialog
views are source-traced here, not claimed as live action-qualified by this run.

Proposed, **not applied**: use the existing Info title as the button's accessible
name; assign semantic names to the State/Protocol editors; pass a semantic name
through the existing diff-view factory for all its consumers. Expected net
production growth **6–10 lines**, with no new state, timers, callbacks or custom
accessibility implementation. Deleting controls or reusing opaque object IDs
would not supply the missing user-facing meaning. Approval is required before
adding these assignments. Repository production/test delta for this inspection:
**0/0**.

Evidence: `a11y-shell-corrected-v61-{1,1.25,1.5,2}-checks.log`,
`a11y-relations-v62-1-checks.log` and matching UI logs in
`/tmp/codexui-timing-surfaces-JBfjvO`. Commands use the same private-D-Bus,
Xvfb/offscreen runner with `QUAL_SCRIPT=a11y-shell.py`. Failed naming assertions
are retained. The controls' successful actions do not override the naming failure
or constitute complete application accessibility qualification.

## Continuation — approved Inspector and diff accessible names, v63

The user approved the preceding 6–10-line proposal. Implemented at the existing
construction authorities: Info choice buttons reuse their visible title;
State/Protocol editors have semantic names; the existing `diffView` factory
accepts the semantic name for preview, Unified, Before and After consumers.
No new renderer, accessible interface, functional state, timer, cache or
per-refresh work. This turn adds **9 net production lines and 30 test lines**,
separate from all pre-existing worktree changes. HEAD remains `b9ff6e5`.

The 14-way Debug build succeeded for `codex-ui`,
`codexui-inspector-graph-test` and `codexui-git-changes-live-test`.
`git diff --check` passed. All live clients used the isolated patched Qt 6.10.2
build above, private D-Bus, Xvfb and the offscreen plugin, with disposable
configuration and WorkerLogic/NodeGraph fixtures, not a real app-server.

**Passed at DPR 1, 1.25, 1.5 and 2:** Inspector Hide/restore; accessible Press
on all five tabs; State, Protocol and Timing choices and Back; exact semantic
names for all three choice buttons and three text views; Changes preview name;
Open review, Unified and Side-by-side actions and the three review editor
names; Escape closes the review; every client exits 0. Each run records 395
repeated visible-control observations across nine shell states, plus explicit
review editor interface records. No unlabeled control remains within this
traversal's scope. This is not full-application accessibility qualification.
The unnamed Model combo's internal editor is accepted only after checking its
owning combo's nonempty Label relationship; no production combo change.

The first `a11y-names-v63-*` run captured the shell rather than the review
dialog for two screenshot calls. The corrected `a11y-review-v63-*` repeat
explicitly captures the dialog, passes all four DPRs again, and logs review
interfaces. The DPR-1.25 Info and Side-by-side images were inspected: controls
and text remain visible, with no appearance change intended by naming metadata.
No new performance claim is made; the change only assigns names at construction.

Commands (paths refer to this machine's disposable evidence):

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --target codex-ui codexui-inspector-graph-test codexui-git-changes-live-test
dbus-run-session -- env \
  LD_LIBRARY_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/lib \
  QT_PLUGIN_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins \
  QUAL_RUN=a11y-review-v63 QUAL_BINARY=codex-ui-interactive-v63 QUAL_SCRIPT=a11y-shell.py \
  python3 /tmp/codexui-timing-surfaces-JBfjvO/run.py
```

The latter runner launches each client through `xvfb-run -a env
QT_QPA_PLATFORM=offscreen`, with PassThrough scaling. Logs live at
`/tmp/codexui-timing-surfaces-JBfjvO/a11y-review-v63-*`; images at
`/tmp/codexui-interactive-uCYB7o/captures/a11y-review-v63-*`.

**Newly exposed failure, not fixed:** the focused CTest run passed Inspector
graph and all four geometry variants, but GitChangesLiveTest segfaulted during
standalone DiffViewer destruction with its review window left open: **5/6
executables passed**, not a green verification result. The crash also prevents
the later cases within that executable from running. Exact CTest command uses
the same library/plugin environment under `dbus-run-session -- xvfb-run -a env
QT_QPA_PLATFORM=offscreen`, then:

```sh
ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --output-on-failure \
  -R 'codexui-(inspector-(graph|geometry)|git-changes-live)' \
  --output-log /tmp/codexui-timing-surfaces-JBfjvO/a11y-v63-ctest.log
```

GDB reproduces with both installed Qt and isolated Qt. During QObject child
deletion, `refreshTimer` (constructed before the review window) has already
been deleted when the review's `destroyed` connection calls
`DiffViewer::refreshRepository()`, then `QTimer::start()` on that dead object.
The connection uses the enclosing viewer as its receiver/context, rather than
the timer whose lifetime its callback requires. This connection is unchanged
from HEAD. The stack is in `a11y-v63-review-teardown-gdb.log` and
`a11y-v63-review-teardown-installed-qt.log` in the same evidence directory.
This proves the standalone-owner destruction case, not that the full shell's
different parent arrangement crashes identically.

Proposed narrow correction, **not applied**: replace the destroyed-signal
lambda with the existing timer as receiver and its no-argument `start` slot:

```cpp
connect(reviewWindow, &QObject::destroyed, refreshTimer,
        qOverload<>(&QTimer::start));
```

Qt then removes the connection with the actual receiver. Two existing lines
are replaced by two lines, deleting the lambda; no new teardown flag, timer or
callback. Normal review close must still schedule refresh, and closing the
viewer with review open must stop crashing. Keep the failing regression rather
than closing the review early merely to avoid it. The separate review refresh
callback and owner lifetime also need checking before accepting this change.
Approval for this separate lifetime correction is requested; accessible-name
checks passing does not close the canonical matrix or erase this failure.

## Continuation — approved review-close lifetime correction, v64

The user approved the preceding proposal. `DiffViewer::openReview` now connects
the review's `destroyed` signal directly to `refreshTimer` and its no-argument
`start` slot. The old lambda and enclosing-viewer receiver are deleted. Qt owns
disconnection at the actual receiver's lifetime boundary. **Production: two
lines replaced by two, net 0; repository tests: 0 changed this turn.** No new
state, flags, timers, caches, callbacks or destructor guard. All prior dirty
files are preserved; no commit, installation or personal-process restart.

The other review callback is used only by Compact/Expanded clicks. The current
shell constructs its Inspector once and destroys it with the containing window;
there is no separate Inspector replacement in that production path. This change
does not promise that a review parented to an arbitrary external window can
safely remain interactive after a caller independently deletes its DiffViewer.
The reproduced failure was standalone-owner teardown, not that hypothetical
ownership sequence.

Verification at the same HEAD/build/toolkit settings as v63:

- Complete 14-way build passed.
- The unchanged test that leaves a standalone viewer's review open now passes;
  the previously interrupted later GitChangesLiveTest cases execute as well.
  Focused Inspector/geometry/diff CTest result: **6/6**, 3.55 s.
- GitChangesLiveTest also passed with **installed Qt** at DPR
  **1/1.25/1.5/2**, through private D-Bus + Xvfb/offscreen and isolated XDG data.
- Four-DPR live v64 runs repeat the prior accessible actions/names, then
  exercise three review open/close cycles, Compact/Expanded refresh and
  close-triggered refresh (provider loading and snapshot signals observed).
  Each finally leaves its review open for full-shell shutdown; all clients
  exit 0. The original standalone crash is covered by the regression executable,
  not inferred from the shell's different ownership arrangement.
- Full CTest: **79/80**, 150.85 s. Only the already-recorded adapter aggregate
  token projection limit fails: 10,000 threads **16.168 ms** versus **8 ms**.
  All 33 performance-labeled cases pass. No threshold was changed. This is not
  a clean performance comparison: the first part overlapped functional live
  checks. Prior full run was 154.40 s with the same failure at 16.945 ms; these
  aggregate timings do not establish an improvement or universal responsiveness.
- `git diff --check` passed.

Commands use the same full library/plugin paths and private-D-Bus/Xvfb/offscreen
environment documented for v63:

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
# Under the isolated Qt/Xvfb environment:
ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --output-on-failure \
  --output-log /tmp/codexui-timing-surfaces-JBfjvO/review-lifetime-v64-full-ctest.log
# Private-D-Bus runner; it launches every client through Xvfb/offscreen:
QUAL_RUN=review-lifetime-v64 QUAL_BINARY=codex-ui-interactive-v64 \
  QUAL_SCRIPT=review-lifetime.py python3 /tmp/codexui-timing-surfaces-JBfjvO/run.py
```

Evidence: `review-lifetime-v64-{1,1.25,1.5,2}-{checks,ui}.log`,
`review-lifetime-v64-ctest.log`, `review-lifetime-v64-full-ctest.log`, and
`review-lifetime-v64-installed-dpr-{1,1.25,1.5,2}.log` under
`/tmp/codexui-timing-surfaces-JBfjvO`.

### Continued qualification — hidden repositories and review layout

At all four DPRs, actual libgit2-backed disposable repositories and production
widgets passed: include hidden roots (8 → 12 files), select a hidden repository
(4 files), review it, then exclude hidden roots while one is selected. The
selection returns to All repositories with 8 visible-root files and no Inspector
width change. Unified/Side-by-side and Compact/Expanded controls worked. This
uses controlled graph notifications, not a real app-server. Scripts/logs:
`diff-hidden-review.py`, `diff-hidden-v64-*` and `diff-review-visual-v64-*` in the
same evidence directory. No fixture repository was modified by these runs.

Two additional failures remain **unfixed**, outside the approved lifetime edit:

1. Both `files` and `reviewFiles` are accessible Qt lists with named children,
   but have an empty accessible Name and no Label relationship. The earlier
   control traversal omitted QListWidget; the expanded traversal records both
   actual interfaces rather than treating that omission as a pass. The minimal
   correction is two constructor assignments using semantic file-list names.
2. A long filename in the review's `title` QLabel sets a content-derived minimum
   width on the entire dialog. At DPR 1 and 1.25, requesting widths 900/1200
   produces **1436** logical pixels at 9 pt and **2202** at 16 pt; the same
   defect is recorded in the other two DPR logs. The wrapper already enables
   word wrap, but the long unbroken filename segment still sets the minimum.
   `renderSelected()` supplies the complete filename, and the horizontal header
   puts mode buttons alongside it. This is not the corrected Inspector preview
   field: the review still uses the old label representation.

The visual script returned 0 because geometry and names were diagnostic outputs,
not assertions in that script; **that exit code is not a visual qualification
pass**. Sixteen long-review captures cover 9/16 pt × 900/1200 requested width ×
four DPRs. The DPR-1 16-pt/900-width image was opened and visibly confirms the
oversized header/window. The DPR-1.25 hidden-repository review image was also
opened: ordinary short-title controls and side-by-side contents were readable.
Other retained captures are not claimed individually inspected.

Proposed next correction, **not applied**: reuse the existing read-only,
horizontally navigable and selectable filename-field policy for the review
header as well as the preview, with one local construction helper rather than
duplicated setup. Preserve heading typography, complete Copy and keyboard access,
and selection on unchanged snapshots. Remove the review's content-width-driving
label path; do not clamp the window or add resize callbacks. Together with the
two list names, expected growth is **12–18 net production lines** (to be checked
before implementation). Verify both surfaces at all four DPRs, 9/16 pt and
narrow/wide widths, long Unicode/unbroken filenames, keyboard/Copy, no-op refresh,
all mode actions and both review-close/owner-teardown paths. This addition needs
user approval. Full canonical qualification remains open.

## Continuation: v65 — approved review filename and list accessibility correction

The subsequent user approval covers the two failures recorded above. Both are
now corrected. Preview and review share the existing read-only filename-field
construction and unchanged-value update policy. The review's width-driving
QLabel is deleted, not constrained by a resize workaround. Heading typography
uses the existing style token; selection survives unchanged filenames, and an
empty snapshot clears the filename. Both actual QListWidget interfaces now have
semantic names: Changed files and Review files. No timer, state, cache, event
forwarding or additional geometry authority was introduced.

Incremental accounting against the start of this correction: **+11 net
production lines**, **+33 test lines**, within the approved 12–18-line estimate.
Two local helpers replace the previous preview-only setup/update blocks.

Verification:

- Complete `cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug
  --parallel 14` passed.
- Focused CTest regex `codexui-(inspector-(graph|geometry)|git-changes-live|
  ui-style-source-policy|application-layout)` (without the displayed line break)
  passed **8/8**, 3.61 s, with `--parallel 14 --output-on-failure`.
- `codexui-git-changes-live-test` passed separately at DPR **1/1.25/1.5/2**.
  Coverage includes full Copy, selection/focus through a changed diff with an
  unchanged filename, empty-state clearing and owner teardown with review open.
- Four-DPR live v65 runs passed using the isolated Qt build, private D-Bus and
  Xvfb/offscreen. Long Unicode/unbroken filenames, 9/16-pt application fonts and
  requested widths 900/1200 produced the requested widths in **all 16 cases**.
  Previously the review forced 1436/2202 logical pixels at 9/16 pt.
- Partial/full keyboard selection and Copy preserve exact text. Read-only
  Backspace/Paste cannot change it. Selection and focus survive 2.3 seconds of
  actual provider polling. All four review mode controls remain inside the
  dialog and work; reopening then full-shell shutdown with review open exits 0.
- **All 16** `review-header-v65-*-verified-font*-width*.png` captures were
  opened and visually inspected. Controls and diff panes are contained and
  readable; the filename uses normal horizontal navigation rather than
  forcing the window wider. This is not native-compositor or real-AT proof.
- A serialized DPR-1.25 scrolling comparison used the same 350+-block Unified
  review, 1200×780 geometry, three sets of 24 wheel events and completed Qt
  UpdateRequest measurements. v64 median/p95/max: **10.425/11.322/13.987 ms**;
  v65: **10.686/12.634/14.863 ms**. Each has 72 samples. Both observed maxima
  were below 16.7 ms, but the new run's tail was higher; these measurements
  neither establish a speedup nor prove universally lag-free interaction.
- `git diff --check` passed. No new full-suite pass is claimed: the most recent
  full-suite result remains the v64 79/80 result recorded above.

External driver build: `python3 /tmp/codexui-interactive-uCYB7o/build-driver.py
codex-ui-interactive-v65`. Runner variables: `QUAL_RUN=review-header-v65`,
`QUAL_BINARY=codex-ui-interactive-v65`, `QUAL_SCRIPT=review-header.py`; runner
`python3 /tmp/codexui-timing-surfaces-JBfjvO/run.py` launches all four DPRs.
The serialized comparison uses `review-scroll-compare.py`, runs argument `1.25`,
and labels `review-scroll-baseline-v64` / `review-scroll-current-v65` with the
corresponding binaries. All use the isolated Qt library/plugin paths and private
D-Bus/Xvfb/offscreen environment documented above.

Logs are under `/tmp/codexui-timing-surfaces-JBfjvO`:
`review-header-v65-ctest.log`, `review-header-v65-dpr-*.log`,
`review-header-v65-*-{seed,checks,ui}.log` and
`review-scroll-{baseline-v64,current-v65}-1.25-{seed,checks,ui}.log`.
Captures are under `/tmp/codexui-interactive-uCYB7o/captures`.
No commit, push, install or personal process modification occurred.

## Continuation: v66 — attachment loss after preparation and explicit recovery

Sections 13, 14, 21 and 22.6: actual widgets and the production WorkerLogic
admission/failure state machine were exercised with **controlled rejection**,
not a real app-server. The driver only adds external diagnostic commands to
consume the application's submitted actions through `admitPrompt` /
`admitFirstPrompt`, then invoke `completePrompt` with a declared fixture error.
No production or repository test code changed after v65.

At DPR **1/1.25/1.5/2**, both a removed file and a permission-000 file were
first attached successfully, then made unavailable before submission. Eight
scenarios passed: exact text/path reached admission; the rejection exposed Not
sent and Restore to composer; an intervening newer draft was not overwritten;
clearing that draft allowed exact restoration into the intended new-thread
draft; restoring the fixture file, removing/re-adding its attachment and
explicitly sending again preserved the exact text/path in retry admission.
Temporary files and permissions were restored afterward. All clients exited 0.

Recovery layout was additionally exercised at 9 pt / requested width 1100,
16 pt / requested width 1100, and 16 pt / width 1536 (height 960), both failure
families at all four DPRs. Send and Remove stayed inside the actual shell and
the draft/attachment remained intact in all **24 combinations**. At enlarged
font the shell can honor its existing minimum rather than the requested 1100;
these checks do not claim an actual 1100-pixel shell at every font/DPR.

Evidence under `/tmp/codexui-timing-surfaces-JBfjvO`:
`attachment-after-preparation.py`,
`attachment-after-preparation-v66-rerun-*-{seed,checks,ui}.log` (all four DPRs),
`attachment-after-preparation-v66-fonts-{1.5,2}-*.log`, and
`attachment-after-preparation-v66-fonts-final-{1,1.25}-*.log`.
Runner command uses `QUAL_BINARY=codex-ui-interactive-v66`,
`QUAL_SCRIPT=attachment-after-preparation.py` and the corresponding `QUAL_RUN`
with `python3 /tmp/codexui-timing-surfaces-JBfjvO/run.py [DPRs]`, wrapped in the
same private D-Bus, isolated Qt and Xvfb/offscreen environment as v65.

Opened captures in `/tmp/codexui-interactive-uCYB7o/captures`:

- `attachment-after-preparation-v66-rerun-1-removed-{failed,restored}.png`
- `attachment-after-preparation-v66-rerun-1.25-unreadable-{failed,restored}.png`
- `attachment-after-preparation-v66-fonts-1.5-removed-restored-font16-width1100.png`
- `attachment-after-preparation-v66-fonts-1.5-unreadable-restored-font16-width1536.png`
- `attachment-after-preparation-v66-fonts-2-removed-restored-font16-width1100.png`
- `attachment-after-preparation-v66-fonts-2-unreadable-restored-font9-width1100.png`
- `attachment-after-preparation-v66-fonts-final-1-removed-restored-font16-width1100.png`
- `attachment-after-preparation-v66-fonts-final-1-unreadable-restored-font16-width1536.png`
- `attachment-after-preparation-v66-fonts-final-1.25-removed-restored-font9-width1100.png`
- `attachment-after-preparation-v66-fonts-final-1.25-unreadable-restored-font16-width1100.png`

These show legible failure/recovery text and reachable controls. Other captures
are retained, not individually visually approved. An old notice can temporarily
overlay the first card: `MiddleRegionWidget` deliberately puts the notice and
conversation in the same grid cell. This is not classified as a new geometry
defect; the canonical notice criterion concerns disappearance after dismissal.

Two external-driver mistakes were corrected without production changes: empty
clipboard Paste did not clear the protected newer draft (Select All → Delete
does), and the geometry lookup initially assumed a namespaced metaobject name
although MainWindow inherits `QMainWindow` without its own Q_OBJECT. Failed
initial runs remain recorded and are not application failures or passes.

Limit: this proves recovery from a submission rejection after file loss, **not**
that a real app-server must reject every unavailable attachment. The earlier
v24 real-server missing-image result remains separate evidence. Automatic retry,
successful backend delivery and physical file-manager interaction are not
inferred from this fixture. Incremental production/test LOC: **0/0**.

### Continued accessibility qualification — three dialog construction omissions

The actual Qt accessible interfaces were inspected in Attach files, Select
workspace, New thread and Fork with options at **all four DPRs and 9/16 pt**.
Each DPR recorded 62 focusable-control instances. Ten instances per DPR lacked
both an accessible Name and a named Label relationship, representing three
construction omissions rather than ten independent defects:

1. `FileSelectionDialog` creates `codexFileBrowser` without a name or label
   relationship, in both attachment and workspace modes.
2. Its `codexAttachmentList` has a visible Selected files caption, but that
   caption has no buddy relationship to the list; the accessible list is unnamed.
3. `NewThreadDialog::field()` assigns the Workspace caption's buddy to
   `workspaceRow`, the non-interactive container, not its QLineEdit. The actual
   editor has neither Name nor Label relationship in both Create and Fork.
   Name/Base instructions/Developer instructions correctly point to their
   editable controls and have named Label relationships in the same traversal.

This violates the existing visible-control accessibility requirement. These are
reproduced interface failures, not inferred screen-reader behavior. No production
edit has been made in either file. FileSelectionDialog is also used from New
Thread's Browse button and upcoming-turn workspace settings; the single
constructor is the correction boundary for all picker entry points.

Proposed correction, requiring approval for **4–6 net production lines**:

- Give the uncaptioned filesystem tree a semantic accessible name at construction.
- Retain the existing Selected files QLabel in a local variable and make the
  list its buddy. Reuse the visible caption rather than duplicating its meaning.
- Let the existing `field` helper receive an optional actual buddy, following
  the already-used policy in TurnSettingsWidget, and pass `workspace` for the
  Workspace row. Replace the incorrect container relationship; do not add a
  parallel naming layer for that editor.

Deletion alone cannot supply missing semantic metadata. The buddy correction
replaces existing behavior; the small addition supplies the absent relationships.
No new widget, persistent state, callback, timer or layout policy is required.
Expected focused regression additions: roughly **25–45 test lines**, using
existing targets. Verify actual interfaces, both picker modes, Create/Fork,
keyboard focus/navigation, unchanged appearance and selection/cancel semantics
through the same Xvfb/offscreen workflow. This is not a hot-path scheduling fix.

Evidence: external `dialog-accessibility.py` and
`dialog-accessibility-v66-all-{1,1.25,1.5,2}-{seed,checks,ui}.log` in
`/tmp/codexui-timing-surfaces-JBfjvO`, using runner v66 as above with
`QUAL_SCRIPT=dialog-accessibility.py`. All clients exited 0; the diagnostic
script's zero exit **does not** signify accessibility approval. The missing
names are explicitly recorded as Failed.

Opened captures: `dialog-accessibility-v66-1-{attachments-font9,
attachments-font16,workspace-font16,new-thread-font16}.png` and
`dialog-accessibility-v66-all-{1-fork-font16,1.25-attachments-font16,
1.5-workspace-font16,2-new-thread-font16}.png` in the common captures directory.
Visible captions are present and readable despite the missing semantic linkage.
At DPR 2/enlarged font, New Thread uses a short scrollable body within the
available offscreen screen; that capture alone does not qualify navigation to
every initially clipped field. No visual redesign is proposed.

## Continuation: v67–v69 — approved dialog accessible names

The three v66 construction omissions are corrected in FileSelectionDialog and
NewThreadDialog. The filesystem tree has a semantic name; the attachment list
and Workspace editor derive their names from their existing visible captions.
The Workspace buddy now targets the editor instead of its container. There are
no new widgets, timers, callbacks, caches or functional state, and no layout or
scrolling-path change. Incremental production delta: **+10/-5 = +5 lines**;
regression delta: **+45/-0**, excluding the pre-existing +108 test lines.

The first implementation relied on QLabel buddies alone. Its four regression
assertions failed, and v67 live traversal reproduced the failures. Qt 6.10.2's
`QAccessibleTable::text()` reads `accessibleName()` directly, while
`QAccessibleWidget::buddyString()` searches immediate siblings only. Workspace's
caption is outside the editor's immediate parent. The correction therefore uses
construction-time names from the same captions, following TurnSettingsWidget's
existing helper. The regression now checks the actual interface and its caption
source; the earlier caption-mutation expectation was invalid for these static
captions and Qt's item-view interface. No production caption-update path exists.

Commands and results (private D-Bus, isolated Qt library/plugin paths as in v65):

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  LD_LIBRARY_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/lib \
  QT_PLUGIN_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins \
  ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --output-on-failure \
  -R 'codexui-(shell-integration|application-layout|git-changes-live|ui-style-source-policy)$'
```

- Build passed; all four focused suites passed, **11.06 s**. This is not a new
  full-suite result or a performance comparison.
- v69 live naming traversal: **248 focusable-control observations**, four
  dialogs × two fonts (9/16pt) × DPR 1/1.25/1.5/2, all named. Both picker modes,
  Create and Fork were exercised, with picker Add/Remove, Workspace focus and
  exact clipboard content, Escape cancellation and clean exit.
- All eight live scenarios at each DPR passed using reverse-Tab traversal;
  every expected Create/Fork control was reached without a mouse. Ordinary Tab
  intentionally inserts text in these QPlainTextEdits. The diagnostic first
  assumed Ctrl+Tab would advance; it does not in this build. A separate probe
  confirmed Tab insertion and Shift+Tab exit for both editors, both purposes,
  both fonts and all DPRs. This is not a complete focus trap and does not justify
  changing text-entry behavior merely to satisfy the driver.
- The driver also originally used `hasFocus()`, which includes focus proxies.
  v69 records the exact `QApplication::focusWidget()` instead. Failed earlier
  logs remain evidence; they are not silently relabeled as passes.
- Opened and inspected all four dialog captures at DPR 1/font 9 and at
  DPR 1.25/1.5/2/font 16, plus New Thread's focused Developer instructions at
  all four DPRs/font 16. Captions and buttons are legible; scrolling clips
  off-viewport body content at small logical screen sizes. At DPR 2/font 16
  the body is particularly short; this is not approval of every narrow-layout
  reading/typing condition. Other captures are retained, not visually approved
  merely because they were produced.

Evidence: `/tmp/codexui-timing-surfaces-JBfjvO/dialog-accessibility-v68-ctest.log`,
`dialog-accessibility-v69-backtab-{1,1.25,1.5,2}-{seed,checks,ui}.log`, and
`dialog-keyboard-v69-{1,1.25,1.5,2}-checks.log`. External scripts:
`dialog-accessibility-verified.py`, `dialog-keyboard-probe.py`; executable
`/tmp/codexui-interactive-uCYB7o/codex-ui-interactive-v69` links current production
libraries. All Qt launches used Xvfb/offscreen; these are controlled graph runs,
not real app-server or screen-reader qualification. Captures use the matching
log prefixes under `/tmp/codexui-interactive-uCYB7o/captures/`.

## Continuation: v70 — outstanding thread-query responses (§5)

The diagnostic client now ran the actual FrontendSession/ClientRuntime worker
and AISuite transport, connected through its configuration dialog to a private
Unix JSONL server implementing the bridge envelope. Automatic startup connection
was disabled in the external driver, so it could not touch the personal bridge.
This is a **simulated bridge/backend**, not an upstream interoperability claim.
No production code changed for this qualification.

At all four DPRs, using actual search/filter/sort controls in Ungrouped mode:

- A → B → A with both abandoned responses delivered before the recreated A
  response showed only the current A result.
- C → D with D completed first and C delivered late preserved D over twelve
  subsequent observations; no stale visible row appeared.
- Active → Archived while the active request was pending showed the archived
  result and rejected the late active result.
- Recent → Created while a request was pending retained the selected local
  Created ordering. Two rows whose creation and recency orders differ were
  displayed in creation order after the refreshed response.

`ClientRuntime` retires obsolete query cycles before applying their payloads.
Created is a local projection sort, not a change to the page's server sortKey.
Same-key refresh waits for the previous request to complete; the diagnostic
initially assumed a concurrently dispatched request with a different sortKey.
That failed assumption was corrected after tracing ThreadPane::changeBrowser
and the browser page scheduler, not by changing production code. An earlier
probe also assumed one request in Projects mode, overlooking the independent
unprojected-group request; the final cases explicitly use Ungrouped mode.

Build remains the v68 production build. External executable v70 adds only an
opt-in manual-connect launch mode. Commands used `dbus-run-session -- env` with
the isolated Qt paths above and `QUAL_RUN=query-races-v70-r4 python3
/tmp/codexui-timing-surfaces-JBfjvO/query-races.py DPR`. That script launches Qt
through Xvfb/offscreen. Evidence is in `query-races-v70-r4-{1,1.25,1.5,2}.log`
and matching `-ui.log` files; all clients exited 0, with no server-thread errors.
Opened all four `query-races-v70-r4-DPR-sort-race.png` captures: search text,
Archived selection, Created caption and the two correctly ordered rows are
readable, without overlapping controls. This does not qualify project/section
query races, other pagination shapes or every filter combination.

The r6 continuation additionally passed a failed second-page request and
explicit retry at all four DPRs. The first row remained available, Retry loading
was exposed, its accessible Press action retried the exact `second` cursor,
and an overlapping first-row entry in the successful reply did not duplicate
the row. Exactly two final rows remained. The viewport automatically requested
the second page while it had room; an earlier probe incorrectly waited for an
intermediate Load more caption. No production adjustment was needed.
Logs: `query-races-v70-r6-DPR.log`, same command with the r6 QUAL_RUN prefix.
Opened r6 DPR-1 `page-error` and DPR-2 `page-retry` captures; the retained row,
distinct retry action and final two-row state are readable. All four clients
exited 0. Private test bus daemons retained after launcher exit were identified
by their exact qualification-log file descriptors and cleaned up separately;
no personal bus or backend was targeted.

## Continuation: v71 — short dialog caret visibility failure (§§8, 21, 22.6)

Names and reachability are insufficient to approve typing in a clipped form.
After keyboard-focusing each instructions editor, the diagnostic pasted twelve
lines and used Ctrl+End. It checked the cursor rectangle against the editor
viewport's actual ancestor-clipped visibleRegion, not merely its own rectangle.
All eight cases passed at DPR 1 and 1.25. Failures:

- DPR 1.5, font 9: Developer instructions in Create and Fork.
- DPR 2, font 9: Developer instructions in Create and Fork.
- DPR 2, font 16: both instruction editors in both dialogs.

Thus **8 of 32 typing cases failed**. All four clients exited 0. Opened captures
`dialog-caret-v71-1.5-new-thread-developer-font9.png` and
`dialog-caret-v71-2-new-thread-developer-font16.png` confirm the last edited line
and caret are concealed below the outer scroll viewport. For example at DPR 2,
font 9, the caret is at editor y=68 but only 62 pixels of that viewport are
visible. This is not a guessed screenshot measurement.

Root cause: NewThreadDialog places independently scrollable, up-to-110px editors
in a screen-height-constrained QScrollArea. QPlainTextEdit ensures visibility
inside itself; Qt's QScrollArea::focusNextPrevChild ensures the input rectangle
is visible when focus moves. The outer area is not informed when the cursor
moves later inside the already-focused editor. There is no existing cursor
visibility connection in NewThreadDialog. The two scrollbars have distinct
valid responsibilities; removing either breaks long-form or long-text access.

Proposed narrow correction, **not implemented; approval required**: notify the
existing outer QScrollArea on cursor-position changes in the focused editor,
using Qt's existing ensureCursorVisible/ensureWidgetVisible primitives. Settle
the inner cursor first so the outer area reads its final input-method rectangle.
Use the outer scroll area as QObject connection context. Do not introduce a
timer, cached geometry, offset, event forwarding or a second scrolling owner.
Expected addition: **8–12 production lines**, **25–40 regression lines**.
Deletion alone cannot supply this missing notification; expanding the dialog
beyond available screen geometry would violate the existing layout contract.
The Qt primitives' no-op behavior, paste/typing/cursor navigation, selection,
manual scrolling, font/size transitions and all four DPRs require verification
before this proposal can be accepted as a complete correction. Tab insertion
semantics must remain unchanged.

Evidence: `/tmp/codexui-timing-surfaces-JBfjvO/dialog-caret.py`,
`dialog-caret-v71-{1,1.25,1.5,2}-{checks,ui}.log`, and matching captures in the
common captures directory. v71 links unchanged production libraries and adds
cursor visibility diagnostics only. Qt source was inspected locally at 6.10.2;
the [documented Tab policy](https://doc.qt.io/qt-6/qplaintextedit.html#tabChangesFocus-prop)
also explains why Tab insertion is not itself a missing focus setting.

## Continuation: v72 — approved nested-dialog caret visibility

The user approved the v71 proposal. `NewThreadDialog` now connects each of its
two instruction editors' cursor-position changes to the existing outer scroll
area. Only the focused editor first reveals its caret internally, then asks
the outer area to reveal that editor's input-method rectangle. This restores
visibility through both viewports without changing their ownership, Tab policy,
or form geometry. The QObject connection context is the owning outer area.
No timer, cached position, offset, flag, or alternative scroll implementation
was added. This stage adds **9 production lines and 33 test lines**; prior
worktree changes are excluded from those figures.

The original keyboard-focus/paste/Ctrl+End reproduction passed **32/32** cases:
Create/Fork × Base/Developer instructions × 9/16 pt × DPR 1/1.25/1.5/2.
The previous build failed eight. Opened post-fix captures include DPR-1 Create
Base/9 pt, DPR-1.25 Fork Developer/16 pt, DPR-1.5 Create Developer/9 pt and
DPR-2 Create Developer/16 pt; the last edited line is no longer concealed by
the enclosing form. Very short enlarged-font forms still require scrolling;
this is not a redesign that displays every form field simultaneously.

Expanded live interactions passed **704/704 checks** in v72-r2: Unicode paste,
Home/End, line and page movement, forward/backward selection, typing, newline,
Tab insertion, undo/redo, manual outer scrolling, subsequent navigation,
unchanged End geometry, and navigation after size/font changes. An initial
driver assertion incorrectly expected a nonempty selection after extending it
back to its original anchor. Each selection direction was corrected to start
from the opposite endpoint; no application behavior was changed to satisfy it.
The regression additionally checks that an unfocused editor's cursor change
does not move the outer form.

Build: `cmake --build ../build/Desktop_GCC-Debug --parallel 14`.
The shell-integration, application-layout, git-changes-live and
ui-style-source-policy suites passed **4/4**, parallel 14, 11.73 s total;
the preceding comparable run was 11.06 s before the added regression.
Execution used `dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=offscreen`
and the isolated Qt library/plugin paths recorded above. The new regression
has non-short-circuiting assertions. `git diff --check` passed.

Serial cursor-to-editor-paint measurements used 60 alternating Ctrl+Home/End
inputs per DPR in an idle Create/Developer editor:

| DPR | Before median / p95 / max, ms | After median / p95 / max, ms |
| --- | --- | --- |
| 1 | 0.557 / 1.098 / 1.161 | 0.505 / 1.145 / 1.185 |
| 1.25 | 0.544 / 0.838 / 0.938 | 0.571 / 1.336 / 1.513 |
| 1.5 | No editor-paint samples in clipped baseline | 0.668 / 1.569 / 1.806 |
| 2 | No editor-paint samples in clipped baseline | 0.636 / 1.217 / 1.481 |

The fixed build produced all 240 expected samples. The missing baseline samples
are not a zero-cost measurement or a comparable pass; its first diagnostic
attempt failed on an empty sample list. The first baseline's low-DPR runs also
overlapped another qualification client, so the table uses the subsequent
serial repeat only. Tail latency increased in the DPR-1.25 serial sample; no
speedup or exact cost neutrality is claimed. All observed post-fix values were
under 2.32 ms over the two measurement runs. These idle cursor samples do not
qualify all traffic, hardware or desktop responsiveness.

Evidence: `/tmp/codexui-timing-surfaces-JBfjvO/dialog-caret-v72-ctest.log`,
`dialog-caret-v72-DPR-{checks,ui}.log`,
`dialog-caret-interactions-v72-r2-DPR-{checks,ui}.log`,
`dialog-caret-cost-v7{1,2}-serial-DPR-{checks,ui}.log`, and matching captures
under `/tmp/codexui-interactive-uCYB7o/captures`. All these clients exited 0.
The external v72 driver links current production libraries; no installation,
commit, push or personal service restart occurred.

Final rebuild after include ordering and the same four-suite invocation also
passed **4/4 in 11.63 s** (`dialog-caret-v72-final-ctest.log`). All qualification
clients launched in this continuation have exited. One private r2 query-run
D-Bus daemon retained after launcher exit was identified by its exact log file
descriptor and stopped by PID after TERM did not end it; no personal process
was targeted.

## Continuation: v73 — project/section outstanding-query qualification (§5)

The actual ClientRuntime/AISuite transport was exercised against a private Unix
bridge simulator at all four DPRs. No personal endpoint was used. The v73-r6
runs repeated the v70 ungrouped search/filter/sort and page-failure/retry cases,
then changed the query with an expanded Project and, separately, an expanded
Section. The scheduler's two-request concurrency limit was preserved: one
obsolete broad request was released to admit the current queries, while the
obsolete group response was held until the new results were visible.

Both grouped cases passed at every DPR. Obsolete response titles deliberately
matched the current search, preventing local text filtering from masking a
stale-response defect. Twelve subsequent observations found no obsolete row.
Current members were verified below the correct expanded enclosing group,
with greater row indentation. All clients exited 0; simulator threads reported
no errors. No production or regression source changes were needed.

Earlier diagnostic iterations corrected harness assumptions, not application
code: the actual menu action is `Projects → Sections`; group tooltips begin
with a kind prefix; browser requests are limited to two in flight; response
titles must satisfy the current search; and thread response membership is
`section: {id, ...}`, not the request parameter `sectionId`. The v73-r5 Section
capture exposed that final fixture mistake. Only v73-r6 qualifies membership
placement. Opened r6 Section captures at all four DPRs confirm enclosing surfaces,
disclosure, indentation and intentional title elision. Earlier r5 Project
captures at DPR 1/1.25/2 and Section at 1.5 were also inspected, but the latter
does not count as correct section-membership evidence.

Commands: `dbus-run-session -- env` with the isolated Qt paths, followed by
`QUAL_RUN=group-query-v73-r6 QUAL_BINARY=codex-ui-interactive-v72
QUAL_EXTRA_SCRIPT=/tmp/codexui-timing-surfaces-JBfjvO/group-query-races.py
python3 /tmp/codexui-timing-surfaces-JBfjvO/query-races.py DPR`.
The script launches the client through Xvfb/offscreen. Logs are
`group-query-v73-r6-DPR.log` and `-ui.log` in that directory; screenshots have
the matching prefix in the common captures directory. This is simulated-backend
evidence, not real-server persistence or concurrent-frontend qualification.

## Continuation: v74 — font-only instruction-caret failure remains open

The approved cursor-change correction does not cover font-only reflow. In a
focused Developer instructions editor containing 60 lines, changing the
application font from 9 to 16 pt leaves the caret at y=116 in a 92px editor
viewport. At DPR 1.25 and 2 the enclosing form additionally hides that editor
entirely. This was reproduced at all four DPRs in both v71 (before the current
fix) and v72: **pre-existing, not introduced by v72**. Authored text, document
identity and keyboard focus remain unchanged. A subsequent navigation key
restores visibility in v72 at all four DPRs.

The broader DPR-2 resize run passed resize-without-navigation cases, but failed
all eight font-change-without-navigation cases (Create/Fork × Base/Developer ×
9→12 or 16→20 pt). Its 200 assertions reported those eight failures. A separate
9→16 pt probe reproduced the same cause without resizing at all; its process
exit 0 means diagnostic collection succeeded, **not that font visibility
passed**. Opened `dialog-font-v74-2-font16.png` confirms the focused Developer
editor is outside the visible form, while its caption remains visible.

Local Qt 6.10.2 source explains the independent trigger:
`QPlainTextEdit::changeEvent` changes the document's default font but does not
reveal its caret; `QScrollArea::event` and `resizeEvent` update scrollbar ranges
without revealing the descendant input rectangle. A cursor-position connection
cannot run when reflow leaves the cursor position unchanged. No timer or range
offset can correctly replace that missing geometry-change boundary.

Proposed next correction, **not implemented; approval required**: consolidate
the existing focused-instruction visibility routine and invoke it after the
owning form has processed Qt's layout/geometry event as well as on cursor
movement. Retain the inner-to-outer Qt primitives and existing scroll owners;
do not react to ordinary manual scrolling by forcing the cursor back into view.
Use the existing dialog/form ownership rather than application-global hooks or
per-frame polling. Estimated additional scope: **18–24 production lines and
20–30 regression lines**, replacing the inline cursor lambda with the common
routine. Deletion alone cannot observe this independent font-only transition.
Correct event ordering, absence of re-entrant layout work, manual scrolling,
unfocused controls, and four-DPR font/style/resize checks are required before
accepting the implementation; the proposed hook is not yet runtime-verified.

Evidence: `dialog-caret-resize-v72-2-{checks,ui}.log`,
`dialog-font-v74-DPR-{checks,ui}.log`,
`dialog-font-v74-baseline-DPR-{checks,ui}.log` in the same evidence directory.
The extra v72-r3 DPR-2 run also passed 184 checks including the unchanged End
position after restoring the original font/size; that does not override the
separate font-only failures. No additional production change was made.

## Continuation: v75 — approved font/resize caret correction (§§8, 21, 22.6)

The user approved the v74 proposal. The dialog now owns one focused-instruction
visibility routine, replacing the former inline cursor callback. It invokes
Qt's inner `ensureCursorVisible()` followed by the enclosing scroll area's
`ensureWidgetVisible()`, both on focused cursor movement and after Qt processes
the dialog's `LayoutRequest` or `Resize` event. Qt remains the geometry and
scrollbar authority. No timer, cached geometry, scroll offset, event forwarding,
global hook or alternate editor was added. One member points to the existing
QObject-owned scroll area; it does not introduce another owner.

Local Qt 6.10.2 `QApplicationPrivate::notify_helper` calls the widget layout's
`widgetEvent` before delivering the event to the widget. `QLayout::widgetEvent`
performs layout for both Resize and LayoutRequest. This establishes the ordering
without a delayed callback. The initial LayoutRequest-only implementation fixed
font changes but failed three resize-only cases at DPR 1.25: the caret remained
inside the editor but below the enclosing form viewport. The repeated diagnostic
and opened failure capture confirmed that result. Adding Resize to the same
boundary removed that omission; no second geometry mechanism was introduced.

Final verification, production-linked isolated clients, Xvfb/offscreen:

- `dialog-font-interactions-v75-r3`: **800/800 assertions**, 200 at each DPR.
  Create and Fork, both instruction editors, normal/enlarged fonts, Unicode
  paste, navigation, forward/backward selection, typing, newline, literal Tab,
  undo/redo, manual form scrolling, semantic no-op End, resize without cursor
  movement and font changes without cursor movement all passed.
- `dialog-caret-style-v75`: **288/288 assertions**, 72 at each DPR, using all
  available styles (Breeze, Windows, Fusion). Focus/caret visibility, selection,
  authored text and document identity survived style/font transitions; ordinary
  manual scrolling was retained over subsequent idle event-loop turns.
- Opened final enlarged-font captures at all four DPRs, including Create and
  Fork, both editors, selection and the three styles. The focused insertion
  point/selection endpoint is visible, with reachable Continue/Cancel. At small
  logical screens the form is necessarily scrollable: nonfocused fields and
  captions can lie outside its viewport. This is not approval of every dialog
  state merely because these captures pass.
- Four focused suites passed after the final rebuild: shell-integration 11.97s,
  application-layout 2.69s, git-changes-live 3.68s, style-source-policy 0.03s;
  **4/4, 11.97s elapsed**. The repository regression covers font-only changes,
  resize-only changes, focus/text/cursor preservation and manual scrolling.
- All final live clients exited 0. The initial test launch raced a still-running
  linker and was **Not Run** (permission denied); it is not counted as a test
  pass or as an Xvfb/application failure. Both subsequent completed suite runs
  passed. Earlier failed interaction logs remain evidence, not discarded runs.

Serial input-to-paint checks used the unchanged `dialog-caret-cost.py`: 60
alternating Ctrl+Home/End inputs per DPR, measured through the resulting Qt
update/paint, with no concurrent qualification clients or builds. The historical
v72 p95 values were 1.145 / 1.336 / 1.569 / 1.217 ms. To distinguish environmental
variation from the change, the retained v72 executable was measured again
between two final-v75 runs:

| DPR | v72 repeat p95 / max, ms | v75 first p95 / max, ms | v75 repeat p95 / max, ms |
| --- | --- | --- | --- |
| 1 | 1.774 / 1.910 | 1.632 / 1.857 | 1.213 / 1.419 |
| 1.25 | 1.862 / 1.967 | 1.746 / 2.244 | 1.851 / 1.934 |
| 1.5 | 1.586 / 2.237 | 1.626 / 2.049 | 1.800 / 1.960 |
| 2 | 1.677 / 1.698 | 1.388 / 1.705 | 1.539 / 1.682 |

All runs captured all 60 samples. The ranges overlap; a consistent slowdown is
not established, nor is universal no-regression or compositor smoothness proved.
The worst final-build sample was 2.244 ms in this bounded cursor workload, not
a bound for application-wide traffic. Evidence prefixes:
`dialog-caret-cost-v75-serial`, `dialog-caret-cost-v75-baseline-repeat`,
`dialog-caret-cost-v75-repeat` under the same directory/runner.

Stage accounting relative to the pre-v75 worktree: **production +19 net lines;
tests +26 lines**, within the approved 18–24 / 20–30 allowances. Other dirty-tree
changes are preserved. `git diff --check` passed. No commit, push, installation
or personal-process changes were made.

Commands and evidence (all paths are disposable qualification artifacts):

```sh
cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
python3 /tmp/codexui-interactive-uCYB7o/build-driver.py codex-ui-interactive-v75-r3
dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  LD_LIBRARY_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/lib \
  QT_PLUGIN_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins \
  ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  --parallel 14 --output-on-failure \
  -R 'codexui-(shell-integration|application-layout|git-changes-live|ui-style-source-policy)$' \
  --output-log /tmp/codexui-timing-surfaces-JBfjvO/dialog-font-v75-r3-ctest.log
```

Live runner: `/tmp/codexui-timing-surfaces-JBfjvO/run.py 1 1.25 1.5 2`,
launched under private D-Bus with the same library/plugin environment. For the
800 checks use `QUAL_BINARY=codex-ui-interactive-v75-r3`,
`QUAL_SCRIPT=dialog-caret-interactions.py`, `QUAL_CARET_RESIZE_CHECK=1` and
`QUAL_RUN=dialog-font-interactions-v75-r3`. For style checks use
`QUAL_BINARY=codex-ui-interactive-v75-style`, `QUAL_SCRIPT=dialog-caret-style.py`
and `QUAL_RUN=dialog-caret-style-v75`; the latter driver adds only diagnostic
style selection, not production code. Logs are `RUN-DPR-{checks,ui,seed}.log`
in that directory. Captures use the same prefix in
`/tmp/codexui-interactive-uCYB7o/captures`.

## Continuation: v76 — grouped pagination recovery and All/Section order (§5)

The production-linked client and AISuite transport were connected to a separate
Unix-socket simulated bridge for each run. No production backend was used.
At DPR 1 / 1.25 / 1.5 / 2, both Projects and Sections passed:

- Admit a first group page, fail its second page, retain the earlier thread and
  show an actionable Retry loading row inside the correct enclosing group.
- Activate that row through Qt accessibility; send the same group/query/cursor
  again. Return the first thread again plus another thread; display each once,
  remove the retry row and retain the enclosing group/indentation.
- With Sections expanded, select All and Section order, then search for Unicode
  text. Verify separate active/archived requests with `section_position` / `asc`.
  Deliver archived results first; final rows still place active threads first,
  preserve each server-provided ordering and remain stable across subsequent
  event-loop turns. Finish unrelated unassigned queries and verify no leftover
  Loading row. This does not infer global ordering from arrival order.

The underlying v70 ungrouped query races and overlapping-page retry were also
re-executed in these runs. Final prefix `group-pages-v76-r5` passed at all four
DPRs; all four clients exited 0 with no simulator error. The r3 run separately
passed grouped retries, and r4 passed section ordering but left intentionally
held unassigned requests pending; only r5 also qualifies their settled state.

An initial diagnostic expected Retry as a top-level accessible item, then used
an incomplete group caption. Both assumptions were wrong: the accessible tree
correctly nests the retry beneath `Project · Project P` / `Section · Section S`.
Source, widget inventory and an opened capture confirmed the visible retry.
The driver now traverses the actual accessible parent/child relationship. These
failed harness runs are not production defects and are retained in the logs.

Opened error/recovered captures across all four DPRs show one coherent group
surface, retained earlier rows, correctly inset retry text and no extra thread
card inside the group. This is simulated-backend recovery evidence, not real
server persistence, complete catalog pagination or all lifecycle-error coverage.
Production/test repository changes for v76: **0/0**.

Evidence: `/tmp/codexui-timing-surfaces-JBfjvO/group-pages-v76-r5-DPR.log`
and `-ui.log`, screenshots with the same prefix in the captures directory.
Command per DPR, private D-Bus, same isolated Qt library/plugin environment:

```sh
QUAL_RUN=group-pages-v76-r5 QUAL_BINARY=codex-ui-interactive-v75-r3 \
QUAL_EXTRA_SCRIPT=/tmp/codexui-timing-surfaces-JBfjvO/group-query-pages.py \
python3 /tmp/codexui-timing-surfaces-JBfjvO/query-races.py DPR
```

The runner launches every client with Xvfb/offscreen and keeps each socket,
configuration and backend isolated. Concurrent runs here are correctness checks,
not performance samples.

## Continuation: v77 — pending prompt motion and promotion (§§9, 22.8)

Using the real production widgets and WorkerLogic with controlled notification
input, normal and reduced motion were checked separately at all four DPRs.
For both a new turn prompt and a steering prompt:

- Submit using the visible Send/Steer control and admit exactly one prompt.
- Capture eight card frames after the existing pending-feedback delay, then
  eight more after acceptance but before authoritative user-message entry.
- Supply the authoritative user item with its actual outgoing `clientId`
  correlation. Verify that the renderer widget identity is unchanged and capture
  eight further frames.
- Normal motion: pending and accepted-but-not-entered frames show changing sweep
  pixels. Authoritative frames are pixel-identical. Each sampled phase retains
  the same card position and size.
- Reduced motion: all three phases are pixel-stable; the pending caption and
  semantic blue/teal surface remain visible. Acceptance does not falsely remove
  the pending caption; authoritative promotion removes it.

Final runs `prompt-motion-v77-r4` and `prompt-motion-v77-reduced` passed at all
four DPRs, with 18 assertions and 48 captured frames per run (**144 assertions,
384 frames total**). No pixel regions were masked. Opened representative normal
and reduced-motion frames across all four DPRs: readable text, fixed control
geometry, blue turn/teal steering feedback and disappearance of only the pending
phase on promotion. Full-shell before/completed captures are also retained.
All eight clients exited 0. These sequences do not qualify every insertion,
regrouping or scrolling transition, nor normal-motion behavior under every load.

Early probes used incorrect diagnostic object identifiers: the Markdown widget
is a subclass, the pending renderer's object name is `pendingPromptCard`, and
Send/Steer reuse `composerSendButton`. The final driver follows the actual
ConversationCard ancestor and asserts the changing submission caption. Those
setup failures do not establish card loss or renderer replacement.

Production/test repository changes: **0/0**. The diagnostic driver only added
the outgoing client-user-message identifier to its existing admission response.
Run with `QUAL_BINARY=codex-ui-interactive-v77`,
`QUAL_SCRIPT=prompt-motion.py`, and the appropriate `QUAL_RUN` through the same
private-D-Bus/Xvfb/offscreen four-DPR runner. Reduced runs additionally set
`QUAL_REDUCED_MOTION=1`. Evidence uses the matching prefixes in the common
timing-surfaces log and interactive captures directories.

## Continuation: v78 — section assignment partial-success recovery (§§7–8)

Four-DPR live interaction through the actual client/runtime/AISuite transport
and isolated simulated bridge passed the following sequence:

1. Open a section context menu, choose New thread here, enter a disposable
   workspace in the New thread dialog, then submit the first prompt.
2. Return a successfully created thread; reject its `thread/section/move` while
   allowing `turn/start` and authoritative user-message entry. The created
   thread and prompt remain present, with an explicit assignment-failure notice.
3. Choose Assign section on the retained thread and retry. Verify the same
   thread/section pair; return success and observe the thread move inside the
   existing section. The wire log contains **one `thread/start`, one
   `turn/start`, two `thread/section/move` requests**—no prompt resend.
4. Dismiss the notice. The notice disappears and the authoritative prompt remains
   readable. Existing overlay behavior described in v66 is unchanged.

Final `section-recovery-v78-r3` runs passed at DPR 1 / 1.25 / 1.5 / 2; clients
exited 0, no simulator errors. Opened failure/recovered/dismissed captures show
the retained thread, explicit warning, one grouped row after retry and readable
prompt after dismissal. Earlier probes selected the composer Workspace field
instead of the dialog's same-named field; the final driver scopes by dialog
ancestry and asserts the exact outgoing temporary cwd. It also supplies the
independent authoritative thread-status notification rather than inferring
thread status from the turn notification. Earlier captures are not status or
workspace qualification.

Production/test repository delta: **0/0**. This does not qualify every lifecycle
failure, real-backend persistence, or concurrent mutation from another frontend.
Evidence and runner are the same as v76, using `QUAL_RUN=section-recovery-v78-r3`,
`QUAL_BINARY=codex-ui-interactive-v77` and
`QUAL_EXTRA_SCRIPT=/tmp/codexui-timing-surfaces-JBfjvO/section-create-recovery.py`.

## Full rebuilt regression check after v78

All **80/80 CTest cases passed in 154.63s**, using private D-Bus,
`xvfb-run -a env QT_QPA_PLATFORM=offscreen CODEXUI_TIMING_POLICY=report`,
the isolated Qt library/plugin paths above, and `ctest --parallel 14
--output-on-failure`. No suite was short-circuited. Two elapsed-time warnings
were reported under the user's accepted timing policy:
`EstablishedUiUxTest.cpp:537` and `NodeGraphUiAdapterTest.cpp:196`.
These warnings were not silently converted into strict timing passes.

Log: `/tmp/codexui-timing-surfaces-JBfjvO/full-v78-ctest.log`;
detailed case output: the build's `Testing/Temporary/LastTest.log`.
An external diagnostic-driver compilation overlapped part of this suite run;
therefore these elapsed times are **not** a controlled performance comparison.
The separate serial v75 input-to-paint comparison remains the relevant measured
comparison for the caret change. Aggregate suite success is not full canonical
interactive/visual approval.

Cleanup: all v75–v78 diagnostic clients and their socket simulators exited.
Fourteen orphaned private D-Bus helpers were identified by their exact PIDs and
stdout files matching these completed qualification runs. They ignored TERM
and were stopped individually with KILL. No personal session bus, application,
bridge or app-server was signaled; helpers without sufficient ownership evidence
were left untouched.

## v79 — Project/Section dialog font and narrow-layout qualification

Canonical sections 6, 7, 18 and 22; same HEAD/build environment as v75–v78.
Independent simulated bridge through the production transport; private D-Bus
and Xvfb/offscreen. No personal backend or application was used.

`group-form-v79` exercised new/edit/details for Projects and Sections, fonts
9/16 pt, a 420×300 requested dialog size, accessible field labels and complete
control visibility. Editable Project Roots/Description received 60 Unicode
lines; Ctrl+End placed the caret at the end, followed by a font-only increase
to 12/20 pt without moving the caret. Each DPR ran all 132 assertions:

| DPR | Passed | Failed |
| --- | ---: | ---: |
| 1.0 | 126 | 6 |
| 1.25 | 126 | 6 |
| 1.5 | 126 | 6 |
| 2.0 | 126 | 6 |

The same six failures occurred at every DPR: Description at both font sizes,
and Roots at the enlarged font, in both Create and Edit. These are **focused
caret visibility failures**, not missing data or clipped whole controls.
Section form checks passed. All four clients exited 0 without simulator errors;
the diagnostic scripts correctly failed their final assertion after collecting
all cases. Representative Project failure, Section narrow-form and Project
read-only details captures were opened, not merely generated. This is not
approval of every remaining visual combination.

The additional `group-form-v79-evidence` run at DPR 1.25 isolates the cause:
Project/Create Description after 16→20 pt has cursor Y=333 in a 144-pixel-high
visible editor viewport. Its full widget remains visible. Ctrl+End, already at
the document end, restores cursor Y=75 without changing the widget, document,
text or geometry. The corresponding font-transition screenshot shows lines
55–56 while the caret belongs at the end of line 59.

Source trace: `ThreadPane::editGroup` constructs plain `QPlainTextEdit` fields.
Qt 6.10.2 `QPlainTextEdit::changeEvent` changes the document font without
ensuring caret visibility; `resizeEvent` relayouts the document and adjusts
scrollbar ranges but likewise does not reveal the caret. The v75 NewThreadDialog
post-layout correction applies only to that dialog, not these fields.

Proposed correction, **not implemented pending growth approval**: put inner
viewport caret preservation in one shared editable-dialog text widget, handling
font changes and viewport resize after Qt's base processing, only when focused
and editable. Use it for Project Roots/Description, NewThreadDialog instruction
fields and the existing structured-response editor. Remove NewThreadDialog's
duplicate inner-viewport `ensureCursorVisible`; retain its distinct outer form
visibility responsibility. Read-only metadata/diff/log viewers keep their
reading-position behavior. This requires no timer, cached position, synthetic
key event or new state. Estimated net production growth: **25–35 lines**;
regression additions: **40–65 lines**, separate from approved v75 growth.
Deletion alone cannot provide the missing editor event behavior.

Required verification: repeat the failed matrix at all four DPRs; test resize,
selection/undo, font restoration, inactive/read-only reading positions and
manual scrolling; rerun v75 nested-viewport checks; compare interactive cost
against the unchanged build. The proposed event implementation still needs that
verification; the successful Ctrl+End diagnostic is not itself a fix.

Evidence: `/tmp/codexui-timing-surfaces-JBfjvO/group-form-v79-*.log`,
`group-form-v79-evidence-1.25.log`, `group-form-layout.py`, and matching widget
captures under `/tmp/codexui-interactive-uCYB7o/captures/`.
Production/test repository changes for v79 investigation: **0/0**.

## v80 — approved shared editable-dialog caret authority

The user approved the v79 proposal. `UiStyle::DialogTextEdit` now owns inner
caret visibility after font and viewport-size changes, only while focused and
editable. Project Roots/Description, Create/Fork instructions and structured
request input use that same widget. NewThreadDialog no longer scrolls the
editor's inner viewport; it retains its distinct outer-form responsibility.
Read-only metadata/diff/log viewers are unchanged. No timer, cache, stored
position, synthetic key event, global hook or replacement renderer was added.

The first implementation passed all Project checks but failed six paste cases
in Create/Fork at DPR 2.0. Qt's `cursorPositionChanged` is emitted before its
later cursor reveal, while `QScrollArea::ensureWidgetVisible` reads the editor's
input-method cursor rectangle synchronously. The shared editor now settles that
rectangle in its constructor-installed cursor callback, before consumers attach
their outer-form callbacks. This transfers the existing required inner-before-
outer ordering; it does not restore the deleted dialog-level inner scroll call.
The initial run is retained as failed evidence, not counted as approval.

Final `codex-ui-interactive-v80-r2` verification:

- Project/Section Create/Edit/Details: **528/528 assertions**, 132 per DPR.
  All 24 v79 failures now pass. `group-form-v80-r2-*` logs and captures.
- Create/Fork instruction editing: **800/800 assertions**, 200 per DPR.
  Includes paste, navigation, selection, newline/Tab, undo/redo, no-op End,
  manual scrolling, font-only and resize-only changes.
  `dialog-editor-v80-r2-*` logs and captures.
- Breeze/Windows/Fusion: **288/288 assertions**, 72 per DPR, preserving text,
  document identity, selection and manual scrolling.
  `dialog-style-v80-r2-*` logs and captures.
- Opened Project and instruction-editor captures across all four DPRs show
  the focused caret in the correct visible region. Scrolled-out nonfocused
  fields are not claimed to be simultaneously visible.
- Final focused CTest rerun: **3/3 passed in 12.12s** (shell integration,
  application layout, style policy). The earlier 19-case run also passed,
  including the thread-pane DPR/performance cases; its timing overlapped
  diagnostic work and is not a controlled performance comparison.

Stage accounting, excluding the pre-existing worktree: **+35 production lines,
+54 regression lines**, within the approved ceilings. The new regression
checks focus/editability gating, text/document/selection identity, reflow and
manual scrolling. The only added connection per editor replaces the prior
inner-scroll responsibility in the parent callback; font/resize use normal
synchronous Qt event overrides. No additional functional state is retained.

Build: `cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug
--parallel 14`; logs `build-v80.log` and `build-v80-r2.log` under
`/tmp/codexui-timing-surfaces-JBfjvO/`. Diagnostic binaries link the rebuilt
production libraries through `/tmp/codexui-interactive-uCYB7o/build-driver.py`.
All runs use the established isolated Qt library/plugin paths, private D-Bus,
Xvfb/offscreen, independent configuration, and either the explicitly simulated
bridge or controlled WorkerLogic input. No installation, personal backend access,
commit or push occurred. A final formatting-only split of one `if` statement
does not change the executable behavior exercised by `v80-r2`.

### v80 serial input-to-paint comparison

Repeated Ctrl+Home/End input-to-paint measurements in the instruction editor,
60 samples per run, all four DPRs. No build or other qualification run overlapped
these measurements. First pass alternated baseline (`v79`) then corrected
(`v80-r2`) for each DPR; repeat reversed that order. All 16 clients exited 0.

| DPR | Baseline p95, first / repeat (ms) | Corrected p95, first / repeat (ms) |
| --- | --- | --- |
| 1 | 2.055 / 1.540 | 1.386 / 1.170 |
| 1.25 | 1.020 / 1.627 | 1.796 / 1.664 |
| 1.5 | 1.214 / 1.464 | 1.441 / 1.594 |
| 2 | 2.437 / 2.153 | 2.495 / 1.621 |

Worst individual sample: baseline 2.601 ms, corrected 2.773 ms. The fractional
DPR results include modest increases and run-to-run variation; these data do
**not** establish zero overhead or universally lag-free interaction. They show
no large stall in this exercised path. The correction does not execute in
conversation streaming, document parsing or thread projection.

Commands use `dbus-run-session -- env` with the isolated Qt library/plugin paths,
`QUAL_RUN=dialog-cost-v80[-repeat]-<version>`,
`QUAL_BINARY=codex-ui-interactive-<version>`,
`QUAL_SCRIPT=dialog-caret-cost.py`, and
`python3 /tmp/codexui-timing-surfaces-JBfjvO/run.py <DPR>`; that runner launches
`xvfb-run -a env QT_QPA_PLATFORM=offscreen` for each client. Raw samples are in
the corresponding `*-checks.log` files, not rounded away by this table.

### v80 final rebuilt suite and cleanup

The final build (including the formatting-only change) succeeded with
`cmake --build ... --parallel 14`. The complete suite then passed **80/80 in
153.01 seconds**, no skips, using private D-Bus + Xvfb/offscreen and
`CODEXUI_TIMING_POLICY=report ctest --parallel 14 --output-on-failure`.
One accepted elapsed-time warning remains at `NodeGraphUiAdapterTest.cpp:196`;
it is not represented as a strict timing pass. No diagnostic UI/build ran
concurrently with this suite. Logs: `build-v80-final.log`, `full-v80-ctest.log`,
`full-v80-run.log`, and the build's `Testing/Temporary/LastTest.log`.

All diagnostic clients and simulated socket servers exited. Thirty-five
orphaned private D-Bus helpers were identified by exact PID and stdout paths
matching completed v80 qualification logs, then stopped individually after
ignoring TERM. Personal processes and helpers without verified ownership were
not signaled. Evidence files and user data were preserved. `git diff --check`
passes. Full canonical qualification remains open; suite success does not
override the v81 visual failure or unexecuted combinations below.

## v81 — pre-existing outer-form clipping in structured request input

Continuing canonical §§18, 21 and 22.7 found a separate failure. Keyboard Tab
reaches the structured JSON editor in an MCP request. At the normal font its
caret is visible before reflow. Increasing the font moves the **entire editor**
below the outer QScrollArea viewport. In the DPR-1 reproduction the editor's
inner caret is Y=111 within its 192-pixel widget, but its visible region through
ancestors is empty. Text, document identity, selection, undo/redo and exact
authored submission remain correct. This is not the v79 inner-viewport failure.

Final diagnostic `structured-editor-v80.py` uses real keyboard Tab to reach the
field before pasting; it compares the typed UI submission's `input`, not a
not-yet-produced wire response's `result.content`. Earlier diagnostic versions
used direct programmatic focus and the wrong submission envelope; those are
not valid evidence for keyboard reachability or payload failure.

The corrected diagnostic executes 28 checks per DPR: 15 pass and 13 fail at
each of 1 / 1.25 / 1.5 / 2. The 13 failures are visibility checks (font/resize,
plus the enlarged-font initial paste). Running the identical diagnostic against
unchanged `codex-ui-interactive-v79` at DPR 1 gives the **same 13 failures**.
All diagnostic clients exit 0. This establishes the outer-form defect predates
v80, not that the entire request-dialog matrix passes.

Root cause: `PendingRequestDialog::present` constructs a plain QDialog and
QScrollArea. Unlike NewThreadDialog, it has no post-layout outer-form focused-
control reveal. Qt's QScrollArea adjusts ranges on layout/resize; its keyboard
focus traversal reveals the widget only at the focus transition. Subsequent
font reflow and cursor movement need not trigger that transition. The shared
editor correctly manages only its own viewport and must not take over ancestor
scrollbars to compensate.

Proposed next correction, **not implemented**: consolidate the existing
NewThreadDialog outer-form visibility policy at a shared scrolling-dialog
boundary and reuse it for PendingRequestDialog. Keep the inner editor authority
unchanged; use Qt's post-layout/resize and cursor-navigation boundaries, preserve
manual scrolling between those events, and add no timer, cached offset or
global focus hook. Delete the superseded NewThreadDialog-specific outer-event
implementation in that same change. Estimated additional net production growth:
**15–30 lines**; regression additions **40–65 lines**. This is outside v80's
approved growth and needs approval before implementation. Verify both nested
dialogs at all DPRs, long disclosure content, font/resize, keyboard traversal,
selection, manual scrolling, cancellation and exact request submission.

Evidence: `structured-editor-v80-r3-*-checks.log`, matching `*-run.log`,
`structured-editor-v80-baseline-1-checks.log`, and the corresponding dialog
captures under the established temporary evidence directories. The failed
capture was opened and shows request disclosure while the focused editor is
below the visible form. No production/test changes were made for v81.

## v82 — approved shared outer-form focus visibility (§§8, 18, 21, 22.6–22.7)

Approval: consolidate the existing NewThread outer-form policy and reuse it in
PendingRequestDialog; additional net production allowance 15–30 lines and
regression allowance 40–65 lines. HEAD remains `b9ff6e5`; earlier worktree changes
are preserved. Including the traversal correction below, this stage adds
**19 production lines net and 65 regression
lines**, measured separately against the pre-v82 working tree, not against HEAD.

`UiStyle::ScrollFormDialog` now owns the existing form scroll area and reveals
its focused descendant through Qt after layout/resize and explicit editor cursor
navigation. Both NewThread and PendingRequestDialog use this policy. The old
NewThread event/reveal implementation and duplicate scroll-area setup are
deleted. `DialogTextEdit` still owns only its inner caret visibility. The new
shared boundary adds no timer, cached offset, global focus connection, protocol
state or replacement renderer; ordinary manual scrolling remains undisturbed
between geometry/navigation events. The existing QObject ownership retains the
form and the signal receiver bounds the editor callback lifetime.

Build: `cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug
--parallel 14` succeeded. Focused CTest invocation used private D-Bus,
Xvfb/offscreen, isolated Qt library/plugin paths, `CODEXUI_TIMING_POLICY=report`,
`--parallel 14 --output-on-failure -R
'shell-integration|application-layout|ui-style|pending-request'`: **4/4 pass,
12.36 seconds**. The first regression fixture omitted required MCP `serverName`
and `threadId`, so the policy correctly did not construct an editable form.
Those two initial assertions were fixture failures; the corrected fixture adds
the required metadata without changing production behavior or weakening checks.

Interactive clients link the same production libraries, use independent
configuration and controlled WorkerLogic input, and launch exclusively through
private D-Bus + `xvfb-run -a env QT_QPA_PLATFORM=offscreen`. At DPR
1 / 1.25 / 1.5 / 2:

- Structured MCP response: **112/112 checks** (28/DPR), including keyboard entry,
  long JSON, font/resize visibility, selection, document identity, manual scroll,
  undo/redo and exact typed submission. All 13 previously failing v81 checks per
  DPR now pass. Corrected request-editor captures were opened at all four DPRs.
- New Thread/Fork, both instruction editors: **800/800 checks** (200/DPR),
  including ordinary Tab insertion, caret navigation, font-only and resize-only
  changes, manual scrolling, selection and undo/redo. Representative captures
  were opened at each DPR; caret and footer actions remain visible. Outer form
  scrolling can intentionally clip nonfocused content, not the active caret.
- Breeze/Windows/Fusion: **288/288 checks** (72/DPR). These are toolkit-style
  checks, not compositor or native-desktop qualification.

Evidence under `/tmp/codexui-timing-surfaces-JBfjvO`: `build-v82[-r2].log`,
`focused-v82-r2-ctest.log`, `structured-editor-v82-*`, `dialog-editor-v82-*`,
`dialog-style-v82-*`; images under the established interactive capture directory.
All clients in these completed interactive runs exited 0.

### v82 serial input-to-paint comparison

`dialog-caret-cost.py` measures 60 Ctrl+Home/End-to-editor-paint samples per run.
Baseline is `codex-ui-interactive-v80-r2`, corrected is `codex-ui-interactive-v82`.
First ran baseline then corrected, repeated corrected then baseline, each over
all four DPRs. No build or other qualification client overlapped these runs.
All 16 clients exited 0; raw samples remain in the logs.

| DPR | Baseline p95 first / repeat (ms) | Corrected p95 first / repeat (ms) |
| --- | --- | --- |
| 1 | 0.829 / 0.821 | 1.407 / 0.794 |
| 1.25 | 0.945 / 1.014 | 0.965 / 1.011 |
| 1.5 | 1.237 / 1.304 | 1.264 / 1.156 |
| 2 | 1.141 / 2.390 | 0.590 / 1.023 |

Maximum individual sample: baseline 2.568 ms, corrected 2.242 ms. Variation and
the DPR-1 first-pass increase preclude a zero-overhead claim; these measurements
show no large stall in the exercised path, not universally lag-free interaction.
The correction does not execute in conversation streaming or graph projection.
Commands use the previously documented `run.py` with
`QUAL_RUN=dialog-cost-v82[-repeat]-{baseline,new}`, the corresponding binary and
`QUAL_SCRIPT=dialog-caret-cost.py`. Git diff whitespace validation passed.

### v82 complete regression suite

The complete rebuilt suite passed **80/80, no skips, 151.27 seconds**. Command:
`dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=offscreen
CODEXUI_TIMING_POLICY=report` with the isolated Qt library/plugin paths, then
`ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug
--parallel 14 --output-on-failure --output-log
/tmp/codexui-timing-surfaces-JBfjvO/full-v82-ctest.log`.
No build or other qualification client overlapped the run; CTest retained the
registered performance-case serialization. One accepted elapsed-time warning
at `NodeGraphUiAdapterTest.cpp:196` remains, not a strict timing pass. The stdout
record is `full-v82-run.log`. This run preceded the additional focus-traversal
correction below; it must not be represented as its final verification.

### Request keyboard diagnostic correction

The broader nine-family dialog probe initially required a forward Tab cycle
through the editable JSON field. Tab intentionally inserts text; a subsequent
Ctrl+Tab assumption was also incorrect for the deployed Qt editor. Both versions
reported only the MCP cycle assertion failing at fonts 9 and 16; these are not
proof of an application focus trap. An explicit probe on both baseline and v82
confirmed native Backtab exits the editor without changing its text and cycles
through Decision, form, Cancel and Submit. Escape cancellation remains available.
The source trace agrees: editable `QPlainTextEdit` defaults to text-inserting
Tab; ignored Backtab reaches parent focus traversal. No production keyboard
policy was changed. The corrected broader diagnostic checks the native backward
cycle and then revealed a distinct visibility omission, described below.

### v82 continuation — focus enters the form from an external footer

The valid backward-cycle diagnostic completes the keyboard cycle and cancels
without submission, but **11/132 visibility checks fail per DPR**: focus can
move from Submit into an offscreen combo, line editor or JSON editor without
revealing it. The unchanged v80-r2 baseline reproduces the same 11 failures at
DPR 1. The captured command-approval decision has focus but an empty visible
region (`[0,0,0,0]`). This is not the rejected Ctrl+Tab assumption.

Qt's `QScrollArea::focusNextPrevChild` calls `ensureWidgetVisible` after traversal
originating inside that area. The footer is outside it, so backwards entry from
Submit invokes dialog traversal without the scroll-area reveal. The approved
shared boundary now overrides `focusNextPrevChild`: let QDialog change focus,
then reveal the resulting focused form descendant. No change to Tab insertion,
no per-control event filter, no application-global hook, no timer or focus cache.
This completes the same focused-control visibility invariant within the approved
growth allowance; it adds eight production lines to the initial v82 correction.
The existing regression now also sends Backtab from the footer and checks actual
focus plus caret visibility. An initial test build incorrectly used QTest in a
target that does not link QtTest; it was replaced by Qt's existing key-event
delivery mechanism without adding a build dependency.

Evidence: `request-nav-v82-r3-*-checks.log`,
`request-nav-v82-baseline-1-checks.log` and its opened hidden-focus capture.
The final production executable is `codex-ui-interactive-v82-r3`:

- Corrected nine-family backward traversal: **528/528 checks** (132/DPR), then
  repeated with focused-entry captures and the same **528/528** result. Both
  fonts preserve visibility, complete the focus cycle and cancel without a
  response action. Command and MCP focused-entry captures were opened at every
  DPR. This verifies traversal/cancellation, not every family's acceptance and
  error lifecycle.
- Structured response: **112/112**; New Thread/Fork: **800/800**;
  Breeze/Windows/Fusion: **288/288**, repeated on this final production build.
  All completed diagnostic clients exited 0.
- Focused CTest: **4/4, 12.44 seconds**, after preserving the regression's
  initial dialog-show event turn. A previous test iteration removed that turn
  and edited without draining the initial pending show/focus events; one
  font-visibility assertion failed. The test now
  reuses its established manually scrolled position for Backtab rather than
  duplicating scroll setup or dropping the initial show boundary.
- Final serial Ctrl+Home/End paint p95 at DPR 1 / 1.25 / 1.5 / 2:
  **1.571 / 1.586 / 1.828 / 1.676 ms**, maximum sample **2.209 ms**. These are
  higher than several earlier samples but within their low-millisecond scale;
  no zero-overhead or universal smoothness claim is made. No build or other
  qualification client overlapped these final measurements.

Final logs use `request-nav-v82-final-*`, `structured-editor-v82-final-*`,
`dialog-editor-v82-final-*`, `dialog-style-v82-final-*`,
`focused-v82-r4-ctest.log` and `dialog-cost-v82-final-*`. The complete final
suite passed **80/80, no skips, 150.77 seconds**, with the same private
D-Bus/Xvfb/offscreen command and 14-way execution; logs
`full-v82-final-{ctest,run}.log`. No build or diagnostic client overlapped it.
The accepted elapsed-time warning at `NodeGraphUiAdapterTest.cpp:196` remains.
The approved outer-form correction is complete within these verified conditions;
this is not completion of the entire canonical inventory.

## v83 — enlarged-font protocol timestamp inventory (§§19, 22.9)

No production/test source changes. Reused the existing 20-context timestamp
fixture, now at font 16 and requested width 1100, all four DPRs. All **240
context/state checks** passed (supplied, null, boundary), plus epoch-zero,
repeated Vienna DST time with differing offsets and exact displayed-text Copy.
All clients exited 0. This exercises the production extractor, diagnostic
channel and protocol view with controlled values, not a live app-server clock.

The first run used an obsolete right-edge mouse coordinate to choose Info; at
the enlarged font it hit tab navigation instead. It stopped before injecting
timestamp cases. The final runner uses the actual Info tab's accessible Press
action, verifies the resulting Protocol control and then exercises its widgets.
No production change was made for this runner error.

The opened enlarged/narrow thread and file-sentinel captures remain within the
panel, but text wraps heavily at this narrow width. This is a readability
limitation, not proof of ideal appearance. Captures exist for all 240 cases;
only the explicitly opened examples have visual review. Complete per-field
visual approval and retained-graph combinations remain open; generated captures
and text assertions do not alone satisfy those requirements.

Evidence: `timestamp-fields-v83-r2-*-checks.log`, matching run logs and
`timestamp-fields-v83-r2-*-timestamp-*.png`. No personal backend was accessed.

## v84 — remaining dropdowns use the existing shared chevron (§§3–5, 18, 22)

Visual inspection after v82 found Decision dropdowns lacked their arrow. A
complete source inventory found three remaining direct `QComboBox` constructors:
request Decision, connection Transport and thread archive filter. Opened DPR-1
captures show the missing arrow in Breeze, Windows and Fusion, at fonts 9/16;
request captures additionally show it at all four DPRs. The widgets still expose
the correct Qt combo accessibility/action model and their popups operate.

The stylesheet supplies custom combo chrome while these three sites bypass the
existing `UiStyle::ChevronComboBox` painter used by settings and Changes. The
correction reuses that existing class everywhere. Its constructor now owns its
existing styling marker; three repeated caller-side assignments are deleted.
No new arrow painter, widget identity replacement, input handler, timer, cache or
functional state. The marker remains presentation-only. Parent ownership,
signals, typed item data and Qt's keyboard/popup/accessibility behavior remain
the same. ConnectionDialog's direct QComboBox include is replaced by UiStyle.

Stage accounting: **production −1 line net, repository tests 0**. No growth or
new architecture required. Source count across the six touched files decreased
4558→4557; v82's approved accounting remains separate. Pre-existing changes are
preserved. `git diff --check` passed. Build and final qualification results follow
when complete; this section is not yet a pass declaration.

Baseline evidence: `dropdown-v84-baseline-r2-1-checks.log` (12 style/font/surface
states), `connection-dropdown-v84-baseline-1-run.log` (six Transport states),
and corresponding opened captures. The Transport run uses the existing isolated
Unix simulated bridge and actual ClientRuntime/AISuite transport, not the user's
backend. It also reruns the established late-query/pagination scenarios. The
initial graph-only attempt could not open Connection Configuration because that
fixture has no settings; this setup failure is not a product defect. All clients
and the simulated server exited cleanly.

Final v84 evidence:

- Build succeeded with `--parallel 14` (`build-v84.log`); focused suites
  **19/19, 22.16 seconds**. Their performance cases overlapped interactive
  qualification, so this elapsed result is functional evidence, not a timing
  baseline. Final complete suite ran separately: **80/80, no skips, 150.82
  seconds**, with the usual isolated Qt paths, private D-Bus + Xvfb/offscreen,
  `CODEXUI_TIMING_POLICY=report`, and `ctest --parallel 14 --output-on-failure`.
  One accepted `NodeGraphUiAdapterTest.cpp:196` timing warning remains. Logs:
  `focused-v84-ctest.log`, `full-v84-{ctest,run}.log`.
- **72 dropdown/style/font/DPR states** exercised: three controls × three
  styles × two fonts × four DPRs. All 72 control crops were opened and inspected;
  arrows, captions, borders and focus appearance are readable and contained.
  All clients exited 0. The transport simulator reported no errors.
- Baseline/corrected DPR-1 comparisons across all 18 control/style/font cases
  found no changed width, height, minimum/hint width, focus policy or choice
  inventory. Decision/archive comparisons also retained accessible role, name,
  actions and label relations. No source still constructs a plain QComboBox.
- Initial `-popup` captures mistakenly captured only the main window, which
  excludes the separate popup window; they do **not** establish popup appearance.
  Corrected runs assert an actual `QComboBoxPrivateContainer`, capture it,
  dismiss it with Escape and verify dismissal. All 72 states pass again. Opened
  popup samples show readable choices, selection and scrolling for Transport.
  Complete popup-state visual coverage is not inferred from the control crops.

Serial focus-to-paint comparison (`dropdown-cost-v84.py`, 60 samples/run,
baseline v82-r3 then v84, all DPRs, no overlapping build/qualification client):

| DPR | Baseline p95 (ms) | Corrected p95 (ms) |
| --- | --- | --- |
| 1 | 1.647 | 1.324 |
| 1.25 | 1.467 | 1.510 |
| 1.5 | 2.133 | 1.515 |
| 2 | 1.617 | 1.613 |

Maximum sample baseline 2.419 ms, corrected 2.078 ms. A slight DPR-1.25 increase
and run variation remain; these samples show no large added stall in the
exercised focus path, not zero rendering cost or universal performance proof.
The control uses the already-established chevron painter and adds no work to
graph projection or conversation materialization.

Logs/captures use `dropdown-v84-final-*`, `connection-dropdown-v84-final-*`,
`dropdown-v84-popup-*`, `connection-dropdown-v84-popup-*`, and
`dropdown-cost-v84-{baseline,final}-*` under the established evidence paths.

## v85 — project/section catalog pagination and recovery (§§5–7)

Both catalogs passed at DPR 1 / 1.25 / 1.5 / 2 using the actual client/runtime
and AISuite transport against an isolated Unix-socket simulator:

- A failed second page retains first-page groups; accessible Retry resends the
  same cursor, and overlapping returned groups are not duplicated.
- A partial refresh preserves previously loaded groups until the final page;
  completion removes omitted groups and retains the new catalog.
- A repeated cursor produces the explicit retry/error state without an unbounded
  request loop; an explicit refresh recovers.
- Refreshing again while an old response is outstanding discards obsolete data;
  the old response cannot insert a ghost group into the current catalog.

All four clients exited 0 and all simulator error lists were empty. These runs
also repeated the existing query-race and thread-page retry checks. Initial
probes incorrectly inspected custom-row `text()` rather than their semantic
tooltip and assumed queued refreshes could bypass outstanding work; those
harness assumptions were corrected without changing production scheduling.

Opened full-shell captures: DPR 1 project error, DPR 1.25 section completed
refresh, DPR 1.5 project recovery, DPR 2 section repeated-cursor error. Group
surfaces, captions and retry rows are readable and contained. Other generated
captures are not claimed as visually reviewed. No real-server persistence claim.

Evidence: `catalog-pages-v85-r4-*-run.log` and matching captures in the established
evidence directories. Runner `catalog-pages-v85.py` is passed as
`QUAL_EXTRA_SCRIPT` to `query-races.py`, with `QUAL_BINARY=codex-ui-interactive-v84`,
private D-Bus and Xvfb/offscreen. Production/test repository delta: **0/0**.

## v86 — thread lifecycle rejection and recovery (§8)

Actual widgets and client/runtime/AISuite transport, isolated Unix-socket
simulator, all four DPRs; production/test source delta **0/0**:

- Rename rejection retains the authoritative title; explicit retry and the
  authoritative notification update it.
- Archive and unarchive rejection retain the thread in the appropriate filter;
  successful retries move it out of that filter and into the other one.
- Rejected Quick fork creates no new row; retry creates one fork, sends its
  requested name and opens its conversation with an enabled prompt editor.
- Cancelled deletion sends nothing; rejected deletion retains the row; confirmed
  retry removes only the intended source. Its fork remains usable and selected.

All final clients exited 0, simulator errors empty. Wire requests were inspected
for method and exact target identity. The first fork probe expected a cleared
thread list, but loaded unrelated threads legitimately remain; it now compares
the before/after inventory. Initial screenshots captured the fork row before
the asynchronous conversation bind. The final runner additionally waits for the
correct conversation title and asserts editor availability; all four DPRs pass.
Neither observation required a production correction.

Opened full-shell captures show readable rejection notices and retained rows
(DPR 1 rename; DPR 1.5 unarchive), selected usable fork (DPR 1.25) and retained
fork after source deletion (DPR 2). Other captures are not implicitly visually
approved. This is controlled protocol failure/recovery evidence, not real-server
persistence or concurrent streaming proof.

Logs: `lifecycle-failures-v86-final2-*-run.log`, matching captures; external
scenario `lifecycle-failures-v86.py` with the same runner, binary and isolated
environment as v85. Functional DPR clients ran concurrently; elapsed times are
not a performance comparison.

Cleanup: 59 orphan private D-Bus helpers from completed v82–v85 qualification
runs were verified by PID, command and stdout evidence path. They ignored TERM
and were stopped individually with KILL. No personal processes or evidence files
were removed; helpers without matching ownership evidence were left untouched.

## v87 — Copy feedback motion, clipboard and geometry (§§12, 22.4, 22.8)

At all four DPRs, normal and reduced motion separately, exercised keyboard Copy
on both a user card and a Markdown answer card. Ten frames were captured per
button across feedback and return, plus its baseline: **16 button sequences,
176 images**. Actual production widgets, controlled graph input and isolated
Xvfb/offscreen clients; no source/test changes.

All sequences preserved exact clipboard source text, button/card geometry,
widget identities and accessible name/role/actions. Normal motion produced
intermediate glyph frames; reduced motion used just the stable check/copy states.
Every final frame returned to the original copy glyph. All eight clients exited
0. Concurrent clients make these functional/motion checks, not timer-deadline
or input-latency measurements.

Opened unscaled contact sheets of both buttons' complete sequences for
all eight motion/DPR combinations: crisp copy/check glyphs, contained intermediate
morphs and unchanged alignment. Agent-surface and other
transition combinations remain separate obligations.

Evidence: `copy-motion-v87-{final,reduced}-*-{run,checks}.log`, corresponding
captures and `copy-motion-v87-contact.png`. Runner `run.py` uses
`QUAL_SCRIPT=copy-motion-v87.py`, `QUAL_BINARY=codex-ui-interactive-v84`; reduced
motion explicitly sets `QUAL_REDUCED_MOTION=1`. Private D-Bus and isolated Qt
library/plugin paths as above. Production/test repository delta: **0/0**.

## v88 — real concurrent frontends during streaming (§§6–8, 20–21)

Used a new disposable Unix bridge/app-server pair, app-server 0.154.0, with
private configuration, state, workspace and an authorized temporary credential
copy. The controller ran at DPR 1 and observer at 1.25, both through private
D-Bus + Xvfb/offscreen using diagnostic v84 and actual production transport.
No personal service or thread was used. Production/test change: **0/0**.

While a real model-issued command streamed output, actual widget actions renamed
the selected thread, assigned project and section membership, and archived,
unarchived and deleted an unrelated disposable thread. Both clients received
rename/project state. The observer received section membership at the documented
explicit refresh boundary; no section notification was fabricated. After the
mutations both clients still reported the selected thread active and output had
grown. The observer's unsent draft remained intact and submission stayed disabled.
The controller/observer streaming captures were opened and inspected.

The final script's completion assertion failed because it expected exactly
`qualification done`, while the model supplied `qualification done.`. This is
not recorded as a wholly passing run. A separate successful client-restart
qualification then compared both frontends' 18 logical accessible items, order,
final answer and complete retained command text against the backend payload.
Both clients agreed and exited 0. Both completed-thread captures were opened;
the retained output followed its tail and the answer remained within its turn.
Draft persistence across restart was not established by this follow-up.

The backend's `thread/turns/list` payload itself contained output lines 2–45,
not 1–45. The displayed 44 lines matched that payload exactly. This rules out a
UI-only loss in this comparison, but does not establish why the backend omitted
line 1; no app-server or bridge source was changed. Full command stdout
completeness is therefore not claimed.

Setup failures remain evidence, not application failures: project roots required
objects and an idempotency key; empty threads needed a completed initial turn to
be retained; the first command-output lookup used an obsolete widget name; an
initial section check incorrectly waited for a nonexistent notification; and a
fork fixture was unavailable to the row lookup for an unestablished reason.
The final concurrent-mutation run used a visible standalone disposable thread.

Evidence: `/tmp/codexui-live-v88-UWZ5v6/frontends-r5-run.log`,
`retained-r2-run.log`, `authoritative-output.json`, `live-frontends.py`,
`streaming-mutations.py`, and the `v88-*-mutated-while-streaming.png` /
`v88-*-retained-completed.png` captures. These are functional/concurrency checks,
not quantitative latency or all-DPR streaming qualification.

Cleanup: the temporary authentication copy was removed. The verified private
bridge/app-server PIDs 2710453/2710463 ignored TERM and were stopped with KILL.
Original credentials, personal services and the disposable evidence were retained.

## v89 — one disabled-button styling authority (§§3, 13, 18, 22)

The real observer capture in v88 exposed a disabled Stop button painted like
an enabled red action. Baseline v84 reproduction across Breeze/Windows/Fusion
and fonts 9/16 showed identical enabled-idle/disabled background pixels, while
Qt's enabled state and accessibility correctly reported the role restriction.
The semantic role rules followed and overrode the early generic disabled rule.
Primary and Steer had duplicated downstream disabled exceptions; Stop, Decline,
Accept and Review did not.

Moved the shared disabled policy after the semantic roles and included the
existing `[kind]` selector specificity. Deleted the earlier generic and the
duplicated Primary/Steer rules. `UiStyle.cpp`: **production −2 net lines;
repository tests 0**. No new state, timer, callback, token, renderer or authority;
authorization and interaction policy are unchanged. Enabled role colors remain
unchanged; Accept/Review use the existing request tone, not the green success role.

Live corrected checks passed at DPR 1/1.25/1.5/2:

- Stop: three styles × two fonts × controller/observer × idle/hover, 96 crops.
  Enabled/accessibility state and visibly different disabled appearance agreed.
- Command approval's Accept/Review and file approval's Decline/Review, each in
  composer and Inspector: 384 crops. Applicable controls had unchanged sizes
  across role changes; disabled clicks emitted no action. The newly visible
  observer-status explanation can legitimately change the enclosing row height.
- All 480 native-resolution crops were opened in 12 labeled contact pages.
  Captions, boundaries and disabled contrast were inspected. Enlarged-font
  controller/observer full-shell examples were also opened. All clients exited 0.

Initial diagnostics incorrectly assumed all six request controls coexist.
Command approval's default action inventory does not supply Decline, while
file changes require detailed review instead of direct Accept. The final probe
uses both actual request families and resolves the earlier fixture through its
correlated request identity. No application policy was changed for these setup
errors. An earlier runner invocation used unavailable `python`; final runs used
`python3` and successfully started Xvfb.

Build: `cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug
--parallel 14`, successful. Evidence under
`/tmp/codexui-timing-surfaces-JBfjvO`: `build-v89.log`,
`stop-disabled-v89-{baseline,final}-*`, `request-disabled-v89-final2-*`,
`stop-disabled-v89.py`, `request-disabled-v89.py`, `disabled-contact-v89.py`.
Runner `run.py`, binary `codex-ui-interactive-v89`, private D-Bus and the same
isolated Qt library/plugin paths, Xvfb/offscreen and PassThrough scaling.
Final full suite: **80/80, no skips, 154.08 seconds**, using the same private
D-Bus/Xvfb/offscreen environment and `CODEXUI_TIMING_POLICY=report`,
`ctest --parallel 14 --output-on-failure --output-log
/tmp/codexui-timing-surfaces-JBfjvO/full-v89-ctest.log`. No other qualification
client/build overlapped it. The accepted elapsed-time warning at
`NodeGraphUiAdapterTest.cpp:196` remains; it is not a strict timing pass.

Serial archive-control focus-to-paint comparison reused `dropdown-cost-v84.py`,
60 samples per binary/DPR with no competing qualification client/build. Baseline
v84 → v89 p95 at DPR 1 / 1.25 / 1.5 / 2: **1.514→1.080 / 1.352→1.466 /
1.410→1.593 / 1.567→1.571 ms**; maximum across each build **1.993→2.053 ms**.
The samples remain low-millisecond with ordinary variation, not proof of a
speedup or universal smoothness. Logs: `disabled-cost-v89-{v84,v89}-*`.
`git diff --check` passed. The focused disabled-style correction is qualified
within these conditions; the overall canonical verdict remains incomplete.

## v90 — Agent feedback passes; narrow header fails (§§16, 21, 22.7–22.8)

Using production v89 with controlled graph input, exercised Agent Copy at fonts
9/16, requested widths 1536/1100, normal/reduced motion and all four DPRs:
**32 sequences, 352 button images** including baselines. Clipboard output
preserved the prompt, authored U+200B and Markdown; frame/control identities,
geometry and accessible button name/role/actions stayed stable. Normal motion
had intermediate frames; reduced motion had just check/copy states; every final
glyph returned to baseline. Tab reached disclosure and keyboard collapse/expand
worked. All eight clients exited 0. The four complete unscaled contact sheets
were opened and inspected. Concurrent execution is not a timing-deadline claim.

**Visual verdict remains failed:** opening the enlarged/narrow Inspector
captures at all four DPRs revealed the Agent title/status clipped and the actual
agent name invisible. Copy/disclosure passing does not excuse this failure.
A separate v84 baseline probe reproduced it before the disabled-style edit:
at 16 pt/requested width 1100, the actual resident row was 170 logical pixels;
`agentName` received width **0**, `agentTitle` **50** versus a **62** size hint,
and `agentStatus` **50** versus a **100** hint. The opened baseline image confirms
`Agen comp` rather than complete identity/status. At the wide condition the
342-pixel row provided the complete labels and 110 pixels for the name.

Cause: `InspectorPane::AgentFrame` places title, name, status, Copy and disclosure
in one non-wrapping `QHBoxLayout`. Title/status request fixed natural widths;
the unwrapped name is Ignored and absorbs compression first. `RowViewport`
correctly assigns viewport width and measures the existing layout, but the
single horizontal header cannot fit its minimum content. This is not missing
model data, an animation defect, or an authority/lifetime problem. No production
change was made for this newly confirmed failure.

Proposed correction, pending approval: reshape only this header into two Qt-owned
rows, identity/name then status/actions. Keep one header for collapsed/expanded
cards and retain the existing Copy/disclosure instances, status policy, graph
projection, residency and scalar-height measurement. A bounded read-only Qt name
field, using the already established filename-field treatment, can retain the
full selectable name and keyboard horizontal access without an unbounded label
minimum. Do not widen the Inspector, shrink fonts, crop required status, add
offsets, timers, cached widths or an alternate renderer. Delete the single-row
arrangement and its contradictory name sizing policy in the same change.

Deleting the redundant static title alone still cannot make name, status and
actions readable at this width. A static stacked QLabel-only layout would also
leave long unbroken names clipped; mere word-wrap is not a complete answer.
Expected allowance for the bounded-name setup and Qt layout reshaping:
**10–20 net production lines**, with **30–60 net regression lines** plus necessary
existing control-type lookup adjustments. This is an estimate, not implemented
LOC. Required checks: short/long/Unicode names, status transitions, all DPRs,
normal/enlarged/narrow/wide conditions, Copy/disclosure and keyboard focus,
selection/identity, reflow anchors, bounded residency and serial performance
against v89. The small header presentation change and growth need approval before
editing production code.

Evidence: `agent-motion-v90-{normal,reduced}-*`, `agent-header-v90-baseline-*`,
scripts `agent-motion-v90.py`, `agent-header-probe-v90.py`, `agent-contact-v90.py`
in `/tmp/codexui-timing-surfaces-JBfjvO`, and their captures under the shared
capture directory. Repository production/test delta for v90: **0/0**.

## v91 — approved Agent header correction (§§16, 21, 22.7)

Baseline remains `b9ff6e5` plus the preserved worktree. The user approved the
v90 proposal: two header rows and a bounded, selectable name field, with an
estimated +10–20 production lines and +30–60 regression lines.

The old single header row is replaced, not retained as another mode. The first
row contains the Agent title and a read-only QLineEdit using the existing code
typography; the second contains the authoritative status and existing shared
Copy/disclosure controls. Qt owns layout, horizontal text navigation, selection
and accessibility. The full path remains the tooltip. An unchanged name does
not reset cursor/selection; a changed name starts at its beginning. Whole-Agent
Copy still reads the authoritative projected content, not the field selection.

The initial implementation exposed a boundary error in the existing Inspector
anchor policy: when newly measured rows exceeded the 52-pixel scalar estimate,
restoring only a leading-row anchor after an End gesture moved the final row
below the viewport. No estimate was increased to conceal this. The existing
Anchor value now records end-of-list intent when the scrollbar has a nonzero
maximum and is at that maximum; restoration uses the new maximum in that case.
Interior positions retain the existing semantic key/pixel-offset behavior.
There is one new temporary anchor boolean, no timer, cache, secondary geometry
authority or renderer. The same viewport policy serves Plan, Agents and Requests.

Stage accounting against saved pre-v91 files: production **+30/−11, net +19**;
tests **+72/−13, net +59**. This includes existing Agent-name lookup changes,
the Unicode/name-selection regression and a quiescence check before the
hidden-tab anchor test records its position. The latter previously sampled a
partially admitted viewport; it retains the exact original scrollbar-equality
assertion. The new field-selection check explicitly completes initial window
activation before selecting, as the live workflow does. No assertion, budget
or case was removed.

Initial full-suite run: **71/80 passed**, nine Inspector cases failed; retained
as `full-v91-{run,ctest}.log`. After boundary correction, four performance cases
still caught the unsettled hidden-tab fixture described above. Final focused
run: **9/9 passed**, including four DPR geometry and four 10,000-row cases
(`inspector-v91-r3-*`). These focused runs overlapped diagnostic work and are
correctness evidence, not isolated timing comparisons.

Final live header driver `codex-ui-interactive-v91-r2` passed **48 combinations**:
Fusion/Windows/Breeze × fonts 9/16 × requested shell widths 1536/1100 × DPR
1/1.25/1.5/2. Each checks short and long Unicode names, bounded controls,
Home/End, Select All/Copy, read-only Cut, selection/focus/geometry across a
semantic no-op, accessible name, Tab to Copy/disclosure, and expansion.
All four clients exited 0. All 48 header-start captures were opened as twelve
native-resolution contact sheets; complete Agent/status text and both controls
remain visible. Very narrow names scroll inside the field, with complete text
available through keyboard, Copy, tooltip and accessibility. This is not a
claim that every generated end-position/expanded screenshot has visual approval.

Evidence/scripts: `agent-header-v91-r2-*`, `agent-header-v91.py`,
`agent-contact-v91.py` under `/tmp/codexui-timing-surfaces-JBfjvO`; captures under
`/tmp/codexui-interactive-uCYB7o/captures`.

Final rebuilt full suite: **80/80 passed, no skips, 150.87 seconds**
(`full-v91-final-{run,ctest}.log`). The existing accepted elapsed-time warning
at `NodeGraphUiAdapterTest.cpp:196` remains; no timing policy was changed.
Build command: `cmake --build
/home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14`.
Test invocation, with the isolated Qt libraries/plugins recorded above:

```sh
dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  LD_LIBRARY_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/lib \
  QT_PLUGIN_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins \
  CODEXUI_TIMING_POLICY=report \
  ctest --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
    --parallel 14 --output-on-failure \
    --output-log /tmp/codexui-timing-surfaces-JBfjvO/full-v91-final-ctest.log
```

Final normal/reduced-motion replay passed **32 sequences**, preserving exact
renderer/control geometry, accessible actions, Copy data (including authored
U+200B), and keyboard disclosure. All eight clients exited 0. All **352 button
frames** were opened as four native-resolution contact sheets. The four normal
motion full-shell captures were also opened. These revealed the separate
expanded-body problem recorded below; a passed button sequence is not approval
of that body.

Final 200-Agent navigation passed at fonts 9/16 and all four DPRs: End reaches
the actual final row and maximum on the first gesture; focusing/selecting the
last name then navigating Home/End retains the exact field and selection.
All four clients exited 0 (`agent-scroll-v91-final-*`).

Serial latency comparison, with no competing builds or diagnostic clients:
60 Copy-focus-to-paint samples per DPR per executable, v89 versus v91-r2:

| DPR | baseline p95, ms | final p95, ms | baseline max, ms | final max, ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 1.020 | 0.946 | 1.217 | 1.030 |
| 1.25 | 1.420 | 1.023 | 1.893 | 1.187 |
| 1.5 | 1.294 | 0.997 | 1.719 | 1.039 |
| 2 | 0.981 | 1.307 | 1.022 | 1.669 |

The DPR-2 sample worsened by 0.326 ms at p95; all final measured maxima remained
below 1.7 ms. This narrow workload establishes neither universal improvement
nor lag-free behavior. The full suite separately preserves its work-count
bounds. Evidence: `agent-cost-v91-{v89,v91-r2}-*`; script `agent-cost-v91.py`.
All eight measurement clients exited 0.

The live runners use the same isolated Qt environment as the full-suite command,
with `QUAL_BINARY=codex-ui-interactive-v91-r2`, per-run `QUAL_RUN` and
`QUAL_SCRIPT`, then `dbus-run-session -- python3
/tmp/codexui-timing-surfaces-JBfjvO/run.py <DPR>`. That runner starts each child
through `xvfb-run -a env QT_QPA_PLATFORM=offscreen`; no Xvfb startup failure was
reported. These are controlled protocol fixtures, not new real-backend claims.

### Continued qualification: expanded Agent body clips after redisclosure

Opened final full-shell captures at DPR 1.25 and 1.5/font 16/narrow width show
the Markdown result laid out wider than its visible surface after the final
collapse/re-expand. Earlier captures in the same sequence wrap correctly;
the final one clips “Agent result” and body text at the right, leaving excess
vertical space. Replayed the identical sequence using **pre-change v89 at
DPR 1.25** and opened its final full-shell image: the same body failure exists
there. It is not a new header regression, but remains a failed canonical visual
case. No production correction has been applied for it.

The shared Markdown widget and Inspector measurement/disclosure path need to be
traced before proposing an exact repair; captured geometry stability alone did
not detect this document-layout defect. Evidence:
`agent-motion-v91-normal-{1.25,1.5}-shell.png` and
`agent-motion-v91-baseline-1.25-shell.png`, with corresponding run logs.
Additional width probes produced correctly wrapped captures with both document
and viewport width 146. The failure is therefore not established as a persistent
width mismatch. The ordinary widget inspector also calls `sizeHint()`, which
can measure Markdown; its observations cannot prove absence of a layout defect.
The separate non-measuring `documentWidths` probe and extra event-loop turn also
produced a good capture (`agent-motion-v91-raw-width-*`). Preserve the failing
images and investigate ordering without treating these later good captures as
a fix. No exact production root cause is claimed yet for this second finding.

### Continued qualification: 24-point shell-brand crash — not yet fixed

The 200-Agent End-navigation probe succeeds at fonts 9 and 16, then crashes
when application font changes to 24. Reproduced independently in **pre-change
v89**, under the authorized Xvfb/offscreen workflow and GDB. This is not caused
by the new name field or anchor correction.

Exact backtrace: `BrandLockup::changeEvent()` → `layoutChildren()` →
`std::clamp<int>(-5, 0, -6)` at `src/codex/ui/BrandMark.cpp:131`.
The subtitle's font metrics exceed `BrandMarkSize = 36`, but the lockup fixes
its height to 36 and uses `36 - subtitleMetrics.height()` as the clamp upper
bound. The standard-library precondition is violated, aborting the process.
Only making the bound nonnegative would leave the oversized subtitle clipped.

Proposed next correction, not applied: replace this manual placement/fixed-height
combination with a Qt-owned horizontal layout that preserves the 36-pixel logo,
title sizing, 12-pixel spacing and baseline relationship, while allowing the
lockup's natural height to accommodate the subtitle. Delete the manual
sizeHint/resize/placement machinery; do not add a catch, timer or smaller-font
override. Expected production reduction roughly 15–25 lines; regression growth
roughly 20–40 lines. Qualification must compare normal-size pixels and enlarged
font/style transitions at all four DPRs before accepting that change.

Evidence: `agent-scroll-v91-r2-1-*` and
`agent-scroll-v91-baseline-1-ui.log` (GDB stack). The baseline runner's shutdown
request failed after the crash; that is not a successful application exit.
The new-build client exited 134. Both diagnostic processes ended; no personal
backend or authentication was used.

## v92 — font-safe brand geometry and non-mutating Agent inspection

Baseline remains HEAD `b9ff6e5` plus the preserved, accumulated qualification
worktree. Only `src/codex/ui/BrandMark.cpp` and the existing shell font regression
were changed in production/tests during this stage. Private Qt 6.10.2 metrics
build, GCC Debug, isolated configuration and controlled protocol fixtures;
no personal backend, installation, commit or upstream change.

### Brand correction: verified

The invariant is that the brand container accommodates its actual text metrics
without changing normal-size appearance. Removed the fixed 36-pixel container
height; its existing `sizeHint()` now owns the natural height, the maximum of
the 36-pixel logo and subtitle metrics. The existing placement uses that same
height. This removes the invalid clamp precondition at its source instead of
clamping a negative bound while leaving the text cropped. The logo/title remain
36 pixels high and preserve the existing baseline policy.

The earlier proposal to replace placement with `QHBoxLayout` was not used:
Qt 6.10.2 `QWidgetItem::setGeometry()` does not implement baseline alignment.
Its vertical alignment branches handle top/bottom/centering; substituting it
would change the established subtitle baseline. This smaller correction retains
one existing placement authority and introduces no widget, timer, callback,
cache or functional flag. Also removed the two redundant explicit assignments
of QLabel's default non-wrapping policy.

Stage accounting against `/tmp/codexui-timing-surfaces-JBfjvO/v92-baseline`:
production **+7 / -8 = -1 line**; tests **+24 / -0 = +24 lines**. The reduction
is smaller than the prior 15–25-line estimate because the baseline-preserving
placement remains. The added regression exercises fonts 9/16/24/32/back to 9,
text/container containment, and unchanged-font geometry/identity.

Verification:

- `cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14` succeeded.
- Focused `ctest ... --parallel 14 --output-on-failure -R shell-integration`
  passed in 13.72 seconds under private D-Bus + Xvfb/offscreen.
- Full `ctest ... --parallel 14 --output-on-failure --output-log
  /tmp/codexui-timing-surfaces-JBfjvO/full-v92-ctest.log`: **80/80 passed**, no
  skips, 152.25 seconds (v91: 150.87 seconds). Accepted timing warning remains
  at `NodeGraphUiAdapterTest.cpp:196`; timing policy remains `report`, not a new
  strict elapsed-time pass. Existing work-count assertions remain enabled.
- `brand-v92.py`: Fusion/Windows/Breeze, fonts 9/16/24/32/back to 9, requested
  widths 1536/1100, DPR 1/1.25/1.5/2. All **120 state visits** passed containment;
  all four clients exited 0. Enlarged shell minimums may exceed requested width;
  do not treat requested width as measured width.
- `brand-contact-v92.py`: all **48 baseline comparisons** (fonts 9/16, three
  styles, two widths, four DPRs) were pixel-identical to pre-change v91-r2.
  Opened all twelve native-resolution contact sheets covering all four fonts,
  styles and DPRs; no vertically clipped brand text or distorted logo. Full-shell
  captures also exist; only the explicitly opened full-shell images are visual
  evidence, not blanket approval of every other element.

Live commands use the existing environment paths above, `QUAL_RUN=brand-v92`,
`QUAL_BINARY=codex-ui-interactive-v92`, `QUAL_SCRIPT=brand-v92.py`, followed by
`dbus-run-session -- python3 /tmp/codexui-timing-surfaces-JBfjvO/run.py
1 1.25 1.5 2`. The runner starts each application with `xvfb-run -a env
QT_QPA_PLATFORM=offscreen`. No Xvfb startup failure was reported.
Baseline repeats use `QUAL_RUN=brand-base-v92`, `QUAL_FONTS=9,16`, and
`QUAL_BINARY=codex-ui-interactive-v91-r2`. Captures are in
`/tmp/codexui-interactive-uCYB7o/captures/brand-{v92,base-v92}-*`.

### Agent clipping: diagnostic trigger identified, no production patch

The v91 screenshots genuinely show clipping, but the earlier implication that
ordinary redisclosure caused it is not established. The diagnostic `widgets`
operation called `sizeHint()` on every widget. Instrumentation now proves that
querying `agentCardContent` changes the visible Agent result document width
from **146 to 296**, without changing its 146-pixel viewport. This is the exact
width in the failing capture, both before and after `grab()`.

Call path in the inspected Qt 6.10.2 source:
`QWidget::sizeHint()` → `QLayout::totalSizeHint()` →
`heightForWidth(sizeHint().width())` → shared `MarkdownTextView::heightForWidth()`
→ `refreshPreferredHeight()` → `QTextDocument::setTextWidth()`. Enumeration order
determines whether the Markdown widget's own subsequent `sizeHint()` restores
the displayed width; this explains the apparently intermittent captures.

Added a geometry-only diagnostic `rawWidgets` operation, with no layout-hint
queries, and document-width getters around capture. Replayed the same normal
motion/Copy/Tab/collapse/expand sequence at fonts 9/16, widths 1536/1100, all
four DPRs: **16 sequences passed**, all clients exited 0. Opened all four final
full-shell captures: heading and body wrap correctly. Document/viewport widths
both remain 146 in each final capture. Evidence: `agent-raw-v92-*`; the explicit
measurement trace is `agent-hints-v92-1.25-ui.log`, and the failing capture is
`agent-clip-v92-1.25-shell.png`.

This corrects the evidence attribution; it does not establish that hypothetical
height queries are generally harmless in production. The shared Markdown
measurement side effect remains a robustness consideration, but this replay
does not justify adding a resize timer, renderer, or speculative correction.
No Markdown/Inspector production changes were made for this finding. Earlier
diagnostic images affected by hint queries must not be used as sole evidence
of spontaneous application clipping.

### Additional font-change crash: qualification still fails

The 200-Agent navigation run passed fonts 9/16/24 at DPR 1/1.25/2, but DPR 1.5
segfaulted during the transition to font 24. This is not the former clamp abort.
The recovered core shows `QStyleSheetStyle::repolish(QApplication*)` →
`updateObjects()` accessing an invalid QObject private pointer, called from
`ShellWidget::eventFilter()` at line 2992 while applying the font-dependent
stylesheet. A debugger replay passed; it does not invalidate the original crash.
Do not classify this as fixed, baseline-only, or a timing-warning acceptance.

Evidence: `agent-scroll-v92-1.5-{checks,ui}.log`,
`agent-scroll-v92-1.5.core`, `agent-scroll-v92-core-bt.log`; successful debugger
replay `agent-scroll-v92-gdb-1.5-*`. A release-breakpoint run disrupted admission
timing and failed its bottom assertion at font 9; it is diagnostic-only and not
counted as a UI pass (`agent-release-v92-1.5-*`).

A subsequent **geometry-only** replay with conditional debugger tracing reproduced
the crash on its first 9 → 16 → 24 font cycle. Two row releases occurred while
Qt's repolish traversal was on the stack. The second full path is:

`updateObjects()` → `QTabWidget::changeEvent()` → `setUpLayout()` → parent/child
geometry changes → `RowViewport::event()` → `QAbstractScrollArea::event()` →
`RowViewport::resizeEvent()` → `synchronize()` → `reconcile()` →
`releaseOutside()` → `release()` → synchronous row deletion.

Qt's `updateObjects()` holds raw QObject pointers in its traversal snapshot.
Deleting a row while that snapshot remains active leaves invalid entries.
The existing environment guard is not sufficient: an ancestor's style change
can resize the viewport before the viewport receives its own style-change event.
Merely moving the local guard ahead of the base event handler would not cover
that ancestor-induced resize path.

Proposed correction, **not applied**: make retirement uniformly use the existing
hide/detach/`deleteLater()` path. This keeps retired rows out of residency and the
visible/accessibility tree immediately, while QObject destruction occurs after
the active Qt event returns. Remove the synchronous-delete branch and the
`dispatchingRow_` / `invokingAction_` special-case tracking that becomes redundant.
Expected reduction roughly 20 production lines, no new timer/cache/functional
state; regression coverage roughly 30–60 lines. Verify actual deferred destruction,
retired-widget bounds, focus/request callbacks, shutdown and repeated font/style
transitions under incoming updates before accepting it. This is the next
requested scope approval, not an implemented fix or a completed qualification.

Trace: `agent-font-repro-v92-1.5-ui.log`, script `agent-font-repro-v92.py`,
GDB commands `agent-release-v92.gdb`. The diagnostic controller lost its socket
after the application crashed; its failed shutdown request is not a clean exit.
Verified that no v92 diagnostic application or debugger remains running.

## v93 — approved Inspector row-retirement lifetime correction (§§16, 18, 21)

The user approved the v92 proposal. The invariant is that leaving viewport
residency must not destroy objects while Qt is still delivering events or
traversing a stylesheet snapshot. `RowViewport::release()` now uniformly hides,
detaches and schedules the existing deferred-delete mechanism. The old direct
delete branch and `dispatchingRow_` / `invokingAction_` tracking are removed.
No new timer, cache, renderer, callback or functional state is introduced.

Owner teardown is deliberately distinct from viewport retirement: resident rows
remain QObject children and are reclaimed by parent destruction, even after the
event loop has stopped. The viewport destructor still disconnects its global
focus callback, but no longer detaches all residents into a post-loop deletion
queue. Existing Inspector teardown ordering is preserved. Previously retired
objects are handled by Qt's deferred-delete boundary; Qt 6.10.2's
`QCoreApplicationPrivate::execCleanup()` also flushes that boundary on normal exit.

Stage delta against saved pre-v93 files: production **+10/−31, net −21**;
tests **+55/−0** (+54 Inspector, +1 Shell integration). The regression asserts
immediate removal from residency without immediate destruction, reclamation at
DeferredDelete, and destruction of still-resident children during owner teardown
without another event-loop iteration. It failed against the old implementation
(ten immediate-lifetime assertions), then passed at all four DPRs. Six existing
test checkpoints now explicitly deliver DeferredDelete before asserting destroyed
QPointer targets; their destruction assertions are unchanged. A tight
`processEvents()` loop does not substitute for that Qt boundary.

### Live evidence and performance comparison

All clients use the actual production libraries, private D-Bus, isolated config
and Xvfb/offscreen with the private Qt 6.10.2 metrics build. Inputs are controlled
ProtocolUpdater fixtures, **not a real backend interoperability claim**.

- Original 200-Agent crash sequence: **120 font transitions** (ten 9/16/24
  cycles at each DPR), Home/End and retained focus. All four clients passed and
  exited 0. `agent-font-v93-*` logs.
- Combined Agent traffic/style/tab replay: **144 states**, all four DPRs,
  Fusion/Windows/Breeze, fonts 9/16/24, requested widths 1536/1100 and both roles.
  Every state updates existing Agent statuses without adding cards, checks End,
  switches Plan/Agents/Info and verifies retired Agent rows are reclaimed.
  Actual Agent row peak was 12; detached rows were zero at settled checkpoints.
  `inspector-lifetime-v93-*` logs. The first diagnostic's object-name filter
  omitted PlanStepFrame; this run therefore does **not** prove Plan object
  reclamation. The filter was corrected before further checks.
- Command/file-change approval controls: **48 family/style/font combinations**,
  each checked in controller and observer roles, all four DPRs. Accessible
  disabled state, unchanged role-switch geometry and blocked observer actions
  passed. All clients exited 0. `request-retirement-v93-*` logs.
- Opened native-resolution contact sheets show legible request controls with
  distinct disabled states, and narrow 24-point Agent headers with separate
  status/actions. Agent names intentionally use the established read-only
  horizontally navigable field; a shortened visible value is not missing data.
  These captures do not qualify every remaining element/state combination.

Serialized 10,000-row Inspector before/after runs passed at every DPR, with no
builds or live diagnostic clients running concurrently. Both revisions retained
Plan/Agent/Request peaks **35/32/24**, materialized Markdown peak **32**, widget
peak **621**, 16 UI page responses, maximum page burst 4, maximum row
constructions 39 and maximum paints per frame 195. Existing quantitative work
limits passed unchanged. Single-sample Agent scroll timings (milliseconds):

| DPR | Before | After |
| --- | ---: | ---: |
| 1.0 | 44.620 | 51.447 |
| 1.25 | 41.730 | 42.691 |
| 1.5 | 44.811 | 45.494 |
| 2.0 | 55.004 | 49.442 |

These are samples, not a statistically established speedup or proof of universal
lag freedom. The benchmark's pre-event-loop document-retirement counter is lower
after deferred retirement; it is not an assertion of leaked documents. The new
regression and settled live object counts separately verify actual reclamation.

Commands/evidence under `/tmp/codexui-timing-surfaces-JBfjvO`:
`cmake --build /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14`;
focused Xvfb/offscreen CTest `--parallel 14 -R 'codexui-inspector-(geometry|graph)'`
passed **5/5**; baseline/final `codexui-inspector-graph-test --performance 10000 DPR`
logs are `v93-perf-{before,after}-DPR.log`. Timing policy remains the previously
authorized `report`; semantic and work-count assertions remain strict.

The first full run executed all 80 cases, **79 passed**: Shell integration's
hidden-Request QPointer assertion needed the same explicit DeferredDelete
checkpoint. That failed run is retained as `full-v93-ctest.log`, not reported as
passed. The final full rerun passed **80/80, no skips, in 153.94 seconds**
(`full-v93-final-ctest.log`). This compares with v92's 152.25-second complete
run; wall time alone is not a responsiveness measurement. `git diff --check`
is clean. The final test log contains one previously accepted timing warning at
`NodeGraphUiAdapterTest.cpp:196`; it is not a new correctness failure or a
strict timing pass. All 384 request-control captures were opened as twelve native-resolution
contact sheets, covering the family/style/font/role/DPR combinations above.

Continued live accessibility/lifetime qualification with the corrected object
filter (`inspector-actions-v93-*`): **24 style/font/DPR combinations**, three
styles, fonts 9/24 and all four DPRs. Plan's three visual statuses match its
accessible descriptions. All three Plan row objects are present while active
and actually destroyed after leaving the tab. Accessible Press on the focused
Request Accept control emits exactly one action; subsequent authoritative
resolution reclaims the row. Returning to Requests does not resurrect it.
All four clients exited 0. This is Qt accessibility-interface/action evidence,
not real screen-reader interoperability. Opened DPR-1 enlarged Plan and Request
captures show readable statuses and wrapped request details; other captures
from this continuation still require visual review.

The final `agent-selection-v93-*` replay also passed at all four DPRs: 200 Agents,
fonts 9/16/24, repeated End reaches the actual last row; selecting the last
Agent's name and scrolling Home/End retains that exact widget, focus and selected
text. All clients exited 0. No additional production changes were needed.

## v94 — project/section mutation rejection and retry (§§6–7)

The v93 production build was exercised through the actual ClientRuntime/AISuite
Unix transport with an isolated bridge-envelope simulator, not the personal
bridge and not a real app-server. Repository production/test delta: **0/0**.

At all four DPRs, actual group menus and Create/Edit/Delete dialogs verified:

- Rejected Project and Section creation do not invent group rows. Explicit retry
  creates the authoritative returned group. Project create sends the absolute
  root object, a nonempty idempotency key and the Unicode/multiline organizational
  description in metadata, not model instructions.
- Rejected edits preserve the old group name. Successful retries show the new
  name. Project update performs the existing read-before-update and preserves
  the unrelated metadata key supplied by that read.
- A rejected Move project to end preserves the project; retry targets its exact
  ID. The successful fixture returns its unchanged position, so it verifies
  request/rejection handling, **not changed relative ordering**.
- Cancelled deletion emits no mutation. Rejected deletion retains the group;
  successful retry removes it. The unrelated initial thread remains. Member
  thread retention is not established by this empty-group fixture.

All four clients exited 0, simulator error lists empty. The inherited runner's
query-race and page-retry checks also passed on the current production build.
These functional clients ran concurrently for three DPRs; elapsed times are not
performance evidence. Opened full-shell captures at DPR 1 Project-edit rejection,
1.25 Section-create rejection, 1.5 Project-move rejection and 2 Section recovery
show readable notices, correct retained/removed groups and no control overlap.
Other captured states are not implicitly visually approved.

Evidence: `group-mutations-v94-DPR-run.log`, corresponding `-ui.log` and captures;
external scenario `/tmp/codexui-timing-surfaces-JBfjvO/group-mutations-v94.py`.
Command: private `dbus-run-session`, the isolated Qt library/plugin paths,
`QUAL_RUN=group-mutations-v94 QUAL_BINARY=codex-ui-interactive-v93-final
QUAL_EXTRA_SCRIPT=/tmp/codexui-timing-surfaces-JBfjvO/group-mutations-v94.py
python3 /tmp/codexui-timing-surfaces-JBfjvO/query-races.py DPR`; each Qt child
launches through Xvfb/offscreen. No installation, commit, push or personal
service restart was performed.

## v95 — manual project order and populated-group deletion (§§5–7)

Same v93 binary, isolated simulator/transport and Xvfb/offscreen workflow as v94;
repository production/test change **0/0**. All four DPRs passed:

- One shared section appears under two different projects; each of three fixture
  threads appears exactly once after expanding the relevant parents.
- Manual project order moves P from first to last on an authoritative position
  update. Changing thread sort to Alphanumeric leaves that project order intact.
  Move project before Q sends the exact `beforeProjectId` and restores P/Q/R.
- Deleting populated project P preserves its member as a standalone Shared S
  member; the other Shared S member remains under Q. No duplicate/lost thread.
- Deleting Shared S then removes only section membership: all three threads
  remain and Q/R project groups remain. The old P member becomes ungrouped;
  Q's member remains in Q.

The initial diagnostic assumed nested sections use the standalone `Section ·`
caption, then repeatedly tried to expand the intentionally empty R project.
Those two lookup/driver failures did not reach the intended final assertions.
The final scenario follows the actual nested caption and skips the known-empty
fixture when expanding populated groups. No product behavior or assertion of
member presence was weakened. Logs preserve all failed attempts.

Final evidence: `group-members-v95-r3-DPR-run.log`, matching client logs and
captures; scenario `group-members-v95.py`, executed as `QUAL_EXTRA_SCRIPT` through
the v94 runner. All clients exited 0; simulator errors empty. Three DPRs ran
concurrently, so this is not a latency comparison. Opened captures show the
standalone/nested Shared S treatment after project deletion (DPR 1), moved-to-end
project order (1.25), restored before-order (1.5) and members without section
groups (2). This establishes controlled protocol/UI behavior, not real-server
group-deletion persistence across restart.

## v96 — current-build timestamp surfaces (§§19, 22.9)

Replayed the existing supplied/missing/invalid timing-surface scenario against
the final v93 production libraries: **48 mode/font/width/DPR combinations**,
fonts 9/16, requested widths 1536/1100, all four DPRs. Controlled protocol input
and actual Qt controls, with isolated Xvfb/offscreen clients. No source/test
change (**0/0**) and no real-server timestamp claim.

Thread tooltip preserves epoch zero. Supplied item/turn times show the correct
CET/CEST distinction across the supplied boundary, UTC and exact raw units;
missing values remain Not supplied and invalid dates/negative durations remain
explicitly invalid. Timing actions do not overlap neighboring controls, open
the intended details, and selected-text Copy exactly matches the details.
Switching to the unrelated fixture thread clears the old timing identity.
All four clients exited 0. Clients ran concurrently, so this is not performance
evidence. The script still records requested rather than guaranteed shell width.

Opened captures: DPR 1 narrow/enlarged supplied full shell, DPR 1.25
narrow/enlarged missing-value cards, DPR 1.5 invalid-value details and DPR 2
wide/enlarged supplied full shell. Timing actions, fallback captions and
wrapped Inspector text are readable; other captures are not implicitly visually
approved. This does not replace the separate full protocol timestamp inventory
or remaining hover/focus/transition combinations.

Evidence: `timing-surfaces-v96-DPR-{seed,checks,ui}.log` and matching captures;
existing `checks.py` via the isolated `run.py` with
`QUAL_RUN=timing-surfaces-v96 QUAL_BINARY=codex-ui-interactive-v93-final`.

## v97 — current-build accessible actions (§§17, 19, 21)

The existing `a11y-shell.py` scenario passed with the final v93 libraries at
all four DPRs: 396 focusable-control observations per client, **1,584 total**,
no unnamed-control failures within this traversal. Accessible Press hides and
restores Inspector without replacing it, selects each Inspector tab, opens
State/Protocol/Timing and returns, opens Changes review, switches Unified and
Side by side, and closes via Escape. The expected page/review accessible names
and selected-tab states passed. Clients exited 0.

This is Qt interface/action evidence, not a real screen-reader traversal or
an inventory of every application control. Opened DPR-1 side-by-side review
and DPR-1.5 Agents captures: controls, labels and content are contained and
readable. Other generated captures are not implicitly visually approved.
Evidence: `a11y-shell-v97-DPR-{seed,checks,ui}.log` and matching captures, using
the existing isolated `run.py` with `QUAL_SCRIPT=a11y-shell.py` and
`QUAL_BINARY=codex-ui-interactive-v93-final`. Production/test source delta 0/0.

## v98 — Protocol follow-tail qualification failure (§19)

The new external `protocol-scroll-v98.py` drives the real Inspector with
controlled diagnostics through the session mailbox. Visible following,
Home detachment, End resumption, the 2,000-block limit, exact selected-text
Copy, hidden-tab updates, and font 9/16/24 at requested widths 1536/1100 were
checked. Final r4 passed the complete sequence at DPR 1, 1.5 and 2; DPR 1.25
failed on returning from a hidden tab. One DPR-1.25 repetition passed, but
r6 reproduced the failure on its second repeated hidden-update cycle:
scroll value **2954**, maximum **3064**, with no user upward scroll.
This is not a four-DPR qualification pass.

### Diagnostic errors separated from the product defect

The initial script passed `modifiers` instead of the driver's `mods`, so Copy
was not invoked. The driver's unchecked empty `clipboard()->mimeData()` then
crashed at temporary `driver.cpp:379`, confirmed under GDB; no product frame
caused that crash. Corrected modifier handling then exposed another wrong
driver assumption: the read-only QPlainTextEdit uses unmodified Home/End for
document scrolling, not Ctrl+Home/End. The final scenario uses that native
behavior and Ctrl+A/C for selection/Copy. Earlier failed logs are retained,
not counted as product regressions or as successful checks.

### Proven cause of false detachment

GDB `protocol-scroll-v98-trace3-1.25-ui.log` shows this exact chain:

```
tab switch -> hide -> QPlainTextEdit::focusOutEvent
 -> QWidgetTextControl::selectionRect
 -> QPlainTextDocumentLayout::layoutBlock
 -> documentSizeChanged -> QPlainTextEditPrivate::adjustScrollbars
 -> QAbstractSlider::setValue -> InspectorPane valueChanged callback
 -> protocolFollowsTail = false
```

The callback at `InspectorPane.cpp:1451–1459` cannot distinguish user scrolling
from Qt laying out a selected, wrapped document. The scoped mutation flag is
already false during focus loss. `showProtocolTail()` subsequently restores
the falsely detached position. This violates the invariant that layout/focus
changes cannot change user follow intent. The bounded log itself remains
intact. Trace runs are functional diagnostics, not timing measurements; their
breakpoint overhead also delayed presentation beyond an early script check.

### Proposed correction — not implemented

Use the scrollbar's input-action signal and its pending slider position for
follow intent, as the existing CommandOutputView already does. Qt's default
tracking slider, wheel, read-only Home/End/page keys, and selection autoscroll
reach `actionTriggered`; geometry-only `setValue` does not. Remove the obsolete
value-inference/mutation-guard path rather than extend its timing window.
Reuse the existing scroll revision to invalidate a queued restoration when
new user input occurs; do not add a timer or another follow-state authority.
Core replacement sketch:

```cpp
connect(bar, &QScrollBar::actionTriggered, this, [this, bar](int) {
  ++protocolScrollRevision;
  const int value = bar->sliderPosition();
  protocolFollowsTail = value >= bar->maximum() - 1;
  if (!protocolFollowsTail)
    protocolPausedScrollValue = value;
});
```

Expected production delta neutral or a small reduction after deleting the
unused mutation guard; no new stored state, cache, renderer or timer. Required
verification before accepting: repeated selected-log hide/show at all DPRs,
wheel/drag/Home/End/selection-autoscroll detachment and resumption, updates
while hidden, bounded eviction, font/width changes, and input between a queued
restore and its delivery. The current production source is unchanged by v98.
Any additional mechanism or growth needs approval; the full qualification
remains incomplete pending this correction and the other open combinations.

## v99 — current-build normal-motion Agent interaction (§§16, 21, 22.8)

The existing `agent-motion-v90.py` ran against v93 with non-mutating widget
inspection, normal motion, four DPRs, fonts 9/16 and requested widths
1536/1100: **16 combinations passed**, all clients exited 0. Each Copy sequence
produced 6–7 distinct captured animation frames then returned to its original
appearance without geometry changes. Clipboard preserved authored U+200B,
Unicode and Markdown; accessible identity/name/action stayed stable. Tab moved
to disclosure and Space collapsed/re-expanded the same Agent.

Opened DPR-1.25 enlarged/narrow Inspector capture: status, actions and wrapped
result text remain contained; the intentionally horizontally scrolling
read-only name field is not claimed to display every character at once.
This repeats affected interaction coverage after the row-lifetime repair,
not all animation/latency combinations. Evidence: `agent-motion-v99-DPR-*.log`
and captures. Production/test source delta 0/0.

## v100 — Protocol correction attempted, rejected on accessibility regression

The user authorized the v98 correction. Pre-edit snapshots are in
`/tmp/codexui-timing-surfaces-JBfjvO/v100-baseline`; the existing dirty worktree
was preserved. The first replacement connected `actionTriggered`, used
`sliderPosition()`, invalidated the existing deferred-restore revision on input,
and removed `mutatingProtocolLog`. It added no state or timer.

The new `protocolFollowTracksInputNotLayout` regression failed on the original
code in all five ordinary/four-DPR Inspector runs: geometry-only value changes
detached following, and queued restoration overrode newer input. The candidate
passed those checks and the then-current complete suite: **80/80, 151.91 s**.
Commands used the established private D-Bus/Xvfb/offscreen environment with the
isolated Qt paths; build and CTest parallelism 14. Logs:
`v100-before-ctest.log`, `v100-after-ctest.log`, `v100-full-ctest.log`.

### Live replay and corrected diagnostic assertion

`protocol-scroll-v100-r2-DPR` passed at all four DPRs, including twelve repeated
hidden-update cycles per client, bounded retention, selected-text Copy,
Home/End detach/resume and font/width changes. Initial v100 runs stopped on a
one-line bottom-margin discrepancy (value 161, maximum 162). The saved v93
executable reproduced precisely the same discrepancy; the opened initial
capture visibly contains the complete last log line (179). The diagnostic
tail predicate was aligned with the existing production `maximum - 1` policy;
it still rejects v98's 110-line displacement. No production geometry change
or timing relaxation was made for that observation. Failed evidence remains.

### Additional invariant check rejects the candidate

The extended regression invokes the real native scrollbar
`QAccessibleValueInterface::setCurrentValue(minimum)` then appends a diagnostic.
**All five Inspector runs failed:** the candidate pulls accessible scrolling
back to the bottom. Log: `v100-accessibility-ctest.log`. The native Qt 6.10.2
implementation in `src/widgets/accessible/rangecontrols.cpp:356` calls
`QAbstractSlider::setValue`, not `setSliderPosition`/`actionTriggered`. Thus the
proposed signal-only correction loses an existing user input path. Real AT-SPI
integration is not claimed; the failure is at Qt's public accessible interface.

The candidate was not accepted. Only this turn's attempted production edits
were undone with a scoped patch; `cmp` confirms InspectorPane.cpp/.h are
byte-identical to their pre-v100 snapshots, preserving all earlier approved
repairs. The stronger regression remains, **+72 test lines; production net 0**.
Consequently the interim 80/80 result is not a current all-green declaration:
the restored implementation still fails the newly added original-defect checks.
The restored full build succeeded; `v100-restored-ctest.log` confirms the three
original-defect assertions fail in all five Inspector runs while the newly
added accessible-setter assertion passes. `git diff --check` passed.
No commit, push, installation or personal-process change.

### Approval boundary for a complete correction

The input-authority rule remains correct, but requires a native scrollbar
accessibility adapter that translates accessible value requests into the same
input action used by mouse/keyboard scrolling. Keep Qt's scrollbar painting,
layout and event delivery; replace its accessible value entry point for this
control, retaining one accessible representation. Then remove the incorrect
value-inference/mutation-guard path. Do not infer input from focus, visibility,
an arbitrary timeout, or a process-wide accessibility-active flag.

Expected additional production cost: approximately **60–90 lines**, including
the narrow scrollbar type, accessible value interface and registration; final
net depends on deleted Inspector guard code. No added timer, cache or follow
state is needed. Scope is the Protocol scrollbar, not a blanket rewrite of
conversation scrolling. Changing Qt's native accessible setter instead would
require separate toolkit authorization and broader Qt compatibility review.
Implementation is paused for approval of the local addition. Required checks
include native accessible setters and pointer/keyboard/selection scrolling,
no-op values, disabled state, focus/layout transitions, late queued restores,
bounded log eviction and all four DPRs. Performance comparison remains due for
the eventual accepted implementation; no candidate speedup is claimed.

## v101 — approved Protocol input authority and accessible scrolling (§§19–21)

The approved local adapter is implemented. The Protocol scrollbar remains a
native QScrollBar: painting, layout, tracking and event delivery are unchanged.
Its single accessible interface preserves native range/value semantics and
routes enabled accessible value requests through setSliderPosition, hence the
same SliderMove action as ordinary input. The public accessible widget base
matches Qt 6.10's locale interface, with the older base retained for the
project's supported older Qt headers. Only Protocol uses this type.

Follow intent now changes on input actions, not geometry-driven valueChanged.
Input invalidates the existing queued-restoration revision. Removed
mutatingProtocolLog and protocolPausedScrollValue: the latter cached the last
action position rather than Qt's settled position after selection release.
QPlainTextEdit::mouseReleaseEvent can call ensureCursorVisible after the final
autoscroll action. The debugger recorded cached position 331 while the settled
scrollbar was 317; the next append restored 331. Each document mutation now
captures the actual scrollbar position instead. There is no new timer, cache,
renderer, follow flag, event forwarding or production diagnostic hook.

Production delta against v100-baseline: **+76/−16, net +60 lines**. Tests:
**+113/−0** against that baseline, including the retained 72-line v100 regression
(this continuation adds 41). This stays within the approved 60–90-line addition.
The added mechanisms are one narrowly typed native scrollbar, its accessible
value interface and lifetime-independent factory registration. Diff review and
`git diff --check` passed. No commit, push, installation or personal-backend use.

### Verification and environment corrections

Current permissions require a writable build within the repository. Configured
`cmake -S . -B build-qualification -G Ninja -DCMAKE_BUILD_TYPE=Debug
-DBUILD_TESTING=ON -DCMAKE_CXX_COMPILER=/bin/g++`; built with
`cmake --build build-qualification --parallel 14`.

Initial runs exposed two environment problems, not green qualification:
inherited non-writable desktop configuration/runtime paths caused eight Breeze
timeouts and a Git settings-persistence failure; private writable XDG paths
made all nine pass without source changes. Separately Xvfb aborted in
NVIDIA/GLX startup, while offscreen children still executed. Their child results
do not count as successful Xvfb runs. Explicit Xvfb `-extension GLX` startup was
verified with xdpyinfo; Qt remained offscreen throughout. No xcb/native-desktop
application workflow was introduced.

Final full suite: **80/80, 151.70 seconds**, `v101-r3-full.log`, successful Xvfb
and child exit. Exact invocation, from the repository:

```sh
env XDG_RUNTIME_DIR=/tmp/codexui-v101-runtime-5WHwn0 \
 XDG_CONFIG_HOME=/tmp/codexui-v101-runtime-5WHwn0/r3-test-config \
 XDG_DATA_HOME=/tmp/codexui-v101-runtime-5WHwn0/r3-test-data \
 dbus-run-session -- xvfb-run -a \
 -s '-screen 0 1280x1024x24 -extension GLX' \
 -e /tmp/codexui-timing-surfaces-JBfjvO/v101-r3-ctest-xvfb.log \
 env QT_QPA_PLATFORM=offscreen \
 LD_LIBRARY_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/lib \
 QT_PLUGIN_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins \
 CODEXUI_TIMING_POLICY=report \
 ctest --test-dir build-qualification --parallel 14 --output-on-failure
```

`protocol-input-v101.py` against `codex-ui-interactive-v101-r3`, through the
isolated `run.py 1 1.25 1.5 2`, passed all four DPRs with all clients exiting 0.
Evidence prefix: `protocol-input-v101-final`. It checks twelve selected-log
hidden-update cycles per DPR, 2,000-block retention, exact Protocol/State Copy,
font 9/16/24 and widths 1536/1100, wheel/Home/End, accessible detach/resume with
stable identity, native thumb dragging and selection autoscroll. Regression
checks additionally cover no-op accessible values, disabled state and input
superseding queued restoration. Controlled diagnostics traverse the actual
session mailbox; this is not a real-server or physical-input claim.

Earlier drag scripts returned before their held gesture ended, did not move the
global cursor read by Qt autoscroll, and could drag the still-selected Ctrl+A
text instead of starting a new selection. The final driver/script explicitly
handles those conditions. Their earlier failures are retained, not counted as
product failures or passes. Opened final DPR-1/1.25/2 tail captures and DPR-1
24-point/DPR-1.5 16-point narrow captures: final lines, selection, scrollbar,
Back control and statistics are readable; narrow tabs use Qt's scroll arrows.

### Serial input-to-update comparison

`perf-run-v101.py` compared the pre-fix production libraries and the corrected
libraries using the same diagnostic driver and isolated Qt. Each DPR/traffic
condition has 72 samples per version (three runs), with no concurrent owned
build/test workloads. Traffic consists of incoming diagnostics, not new cards.
All eight clients and launchers exited 0; `v101-perf.log` retains raw results.

| DPR | Idle p95 before → after (ms) | Incoming diagnostics p95 before → after (ms) |
| --- | --- | --- |
| 1 | 4.097 → 3.556 | 3.323 → 3.296 |
| 1.25 | 5.777 → 5.067 | 4.419 → 4.315 |
| 1.5 | 5.312 → 5.557 | 5.202 → 5.205 |
| 2 | 6.470 → 6.334 | 6.127 → 6.130 |

Maximum fixed sample: 6.812 ms, baseline 6.566 ms. These measurements are
comparable, with small variation in both directions, not proof of a universal
speedup or zero lag. The Protocol correction is verified within these cases;
real screen-reader interoperability and the rest of the canonical matrix remain
separate obligations.

## v102 — Thread-query boundaries and stale replies, four DPRs

Continued canonical thread-browser qualification using the current v101-r3
production-linked diagnostic application and a private Unix JSONL bridge
simulator. No personal service was used and no production code was changed.
`query-races.py` with `QUAL_EXTRA_SCRIPT=query-boundaries-v102.py` passed at
DPR 1, 1.25, 1.5 and 2: all four clients and Xvfb wrappers exited 0, with no
simulator errors. Logs are under
`/tmp/codexui-timing-surfaces-JBfjvO/query-boundaries-v102-final-*.log`.

The runs covered A→B→A queries, reversed responses, archive/sort changes,
failed later-page retry, overlapping pages, Unicode and internal whitespace,
empty results, matching later-page rows, whitespace-only clearing, and a late
response belonging to the abandoned search. The wire requests preserve internal
whitespace and Unicode, trim query edges, and omit `searchTerm` after clearing.

An initial diagnostic incorrectly expected clearing search to discard previously
loaded threads. Tracing the adapter confirmed the intended graph-backed behavior:
clearing removes the title predicate while retaining known threads matching the
archive filter. The corrected check requires exactly the eight known matching
rows, no duplicates, and no row from the obsolete response. This was a diagnostic
expectation correction, not a product repair.

Opened and inspected the DPR-1 empty-result, DPR-1.25 later-page and DPR-2
cleared-search screenshots: controls and Unicode titles remain readable, rows do
not overlap, and the expected result sets are visible. This qualifies those
captures, not every width/font/state combination or a real-server search.

Invocation: `QUAL_RUN=query-boundaries-v102-final
QUAL_BINARY=codex-ui-interactive-v101-r3
QUAL_EXTRA_SCRIPT=/tmp/codexui-timing-surfaces-JBfjvO/query-boundaries-v102.py
dbus-run-session -- python3 /tmp/codexui-timing-surfaces-JBfjvO/query-races.py DPR`,
with the isolated Qt/XDG environment recorded in v101 and Xvfb GLX disabled.
Production/test-source LOC delta for this qualification: 0/0.

## v103 — keyboard traversal exposes clipped conversation controls (§21)

**Not passed; correction proposed, not implemented.** The v101-r3 diagnostic
application reproduced invisible keyboard focus at DPR 1, 1.25, 1.5 and 2 under
the same isolated Xvfb/offscreen environment as v101/v102. Each client and Xvfb
wrapper exited normally; the qualification scripts correctly returned failure.
Evidence: `/tmp/codexui-timing-surfaces-JBfjvO/keyboard-v103-r4-*-checks.log`.

An accumulative DPR-1 replay then completed all five Inspector tabs and the
State/Protocol/Timing detail pages rather than stopping at the first invisible
control. It recorded 21 invisible-focus observations: the same conversation
header's Timing, Copy and disclosure controls in seven shell states. All 21
remained invisible after an additional 500 ms. Reverse traversal closed in each
state (32–70 focus stops); periodic incoming diagnostic messages did not steal
focus. Those successful assertions do not override the visibility failure.
This is a seeded production-widget scenario, not real-server interoperability.

The first failing Timing control had global y=17 and height=15; the conversation
view began at y=165. Its visible region was `[0,0,0,0]`; the conversation scrollbar
was at value/maximum 2240. Opened the corresponding full-shell capture
`keyboard-v103-evidence-1-invisible-focus.png`: the image card's lower content
remains visible while its focused header is above the viewport.

The source trace identifies one missing responsibility:

- `ConversationView::layoutMaterializedCards()` defines card visibility by any
  intersection with the viewport.
- `ConversationCard::Impl::refreshViewportTabFocus()` applies that card-level
  visibility to all its controls, including a clipped header.
- The existing `QApplication::focusChanged` handler synchronizes the current row
  with auto-scrolling deliberately disabled. It does not reveal the newly
  focused child's geometry.

The invariant is that keyboard navigation must reveal its focused control while
preserving the current renderer, geometry owner and explicit scrolling policy.
Deleting offscreen header controls from Tab navigation would instead make them
unreachable. Removing the auto-scroll suppression is insufficient: row-level
reveal does not know which control is focused, especially in a tall card, and
moving focus within the same row need not change the current index.

Proposed correction: extend the existing view-owned focus-change path to reveal
the focused control's mapped bounds with the existing scroll-value/follow-mode
methods, only when necessary. Preserve partially visible oversized text surfaces
and manual scrolling with unchanged focus; do not scroll an entire tall card to
its opposite edge. No new timer, cached rectangle, follow flag, event forwarding,
or renderer. Estimate **12–20 net production lines**, **40–70 regression lines**;
approval is required before implementation under the repository change gate.
Verify upper/lower clipping, tall cards, forward/reverse navigation, mouse and
accessible focus, unchanged-focus manual scrolling, retirement and streaming,
all four DPRs, and compare interaction cost with the recorded baseline.

The diagnostic initially misidentified the prompt editor's class and assumed
Ctrl+Tab would navigate. These were diagnostic errors, not product defects.
The established policy (already recorded in v69/v82) is Tab text insertion and
native Backtab exit. The final diagnostic sends Qt's actual Backtab key code,
preserves the draft and uses the full reverse cycle; the forward sweep explicitly
ends at the text-entry boundary. Earlier failed logs remain separate evidence.

Invocation: `QUAL_RUN=keyboard-v103-evidence
QUAL_BINARY=codex-ui-interactive-v101-r3 QUAL_SCRIPT=keyboard-v103.py
dbus-run-session -- python3 /tmp/codexui-timing-surfaces-JBfjvO/run.py 1` with
the v101 environment. The initial four-DPR reproduction uses
`QUAL_RUN=keyboard-v103-r4` and arguments `1 1.25 1.5 2`.
Production/test-source changes in v103: **0/0**; diagnostic scripts are in `/tmp`.

## v104 — approved keyboard-focus visibility correction (§§10, 21, 22.4)

Implemented in `ConversationView`'s existing focus-change handler. It maps the
newly keyboard-focused control into the viewport and makes only the smallest
necessary vertical scroll, through the existing scroll/follow authority. A
control taller than and spanning the viewport does not cause an opposite-edge
jump. Manual scrolling with unchanged focus is untouched. No new timer,
callback, cache, flag, renderer or geometry owner was introduced.

The first version incorrectly treated automatic focus restoration as navigation;
the existing command-follow and staged-state cases failed. It was rejected.
Qt's `QApplicationPrivate::setFocusWidget` sets the public window attribute
`WA_KeyboardFocusChange` for Tab/Backtab traversal and shortcuts, and clears it
for automatic/mouse focus. The final version consumes that existing marker,
already used by the view's focus decoration; it does not introduce an inferred
input-intent flag. Automatic focus and the existing logical accessible-row
reveal path retain their prior policy. No accessibility behavior is claimed
beyond the exercised interfaces and existing suite.

Accounting against `/tmp/codexui-timing-surfaces-JBfjvO/v104-baseline`:
**production +16/−0; tests +68/−0**. The added regression covers clipped upper and
lower controls, oversized bodies, unchanged-focus manual scrolling, automatic
focus restoration and renderer/size preservation. The existing eight
Fusion/Breeze DPR cases execute it. No prior tests were weakened.

Verification on HEAD `b9ff6e5` plus the preserved worktree:

- `cmake --build build-qualification --parallel 14`: passed.
- Full `ctest --parallel 14 --output-on-failure` under isolated D-Bus and Xvfb
  (`-extension GLX`), `QT_QPA_PLATFORM=offscreen`, isolated writable XDG paths,
  private Qt 6.10.2 paths and `CODEXUI_TIMING_POLICY=report`: **80/80 passed,
  155.53 s**. Xvfb/child exit 0. Log `v104-full.log`; this functional run overlapped
  live qualification and is not a controlled performance comparison.
- `keyboard-v103.py` against `codex-ui-interactive-v104-r2`, prefix `keyboard-v104`,
  all four DPRs: **passed**, no invisible-focus observations. Full backward
  cycles and forward traversal to the intentional prompt text-entry boundary
  cover five Inspector tabs plus State/Protocol/Timing detail pages. Incoming
  diagnostics do not steal focus. All clients/launchers exit 0.
- `focus-visual-v104.py`: normal 9-point/1536 requested width and enlarged
  16-point/1100 requested width, all four DPRs: passed control-visibility checks.
  The enlarged shell enforces a wider actual minimum; this does not claim an
  actual 1100-pixel window. Opened DPR-1 Timing captures at both fonts, DPR-1.25
  enlarged Copy, DPR-1.5 enlarged disclosure and DPR-2 enlarged Timing captures:
  focused controls are visible and preserve their established glyphs/borders.
- `git diff --check`: passed.

### Serial scrolling comparison

`perf-run-v104.py` compared the pre-fix `codex-ui-interactive-v101-r3` with
`codex-ui-interactive-v104-r2`, without concurrent owned builds/tests/UI clients.
Three 24-event runs per condition/version/DPR: **1,152 samples**, all eight
clients/launchers exit 0. The measured endpoint is completion of the outer Qt
update containing the conversation scrollbar paint, not compositor presentation.
Traffic comprises incoming diagnostics without new cards. Raw evidence:
`conversation-latency-v104-r3-{baseline,fixed}-DPR-checks.log`.

| DPR | Idle p95 before → after (ms) | Diagnostic-traffic p95 before → after (ms) |
| --- | --- | --- |
| 1 | 4.710 → 4.150 | 5.030 → 4.248 |
| 1.25 | 4.574 → 1.795 | 4.912 → 3.360 |
| 1.5 | 2.022 → 2.992 | 2.904 → 3.367 |
| 2 | 5.603 → 5.565 | 4.790 → 5.965 |

Variation is in both directions; this is not evidence of a universal speedup or
of never-worse latency. Largest fixed sample **18.612 ms**, baseline **15.256 ms**;
the accepted report-only timing policy must not be presented as a strict
16.7-ms pass. Earlier benchmark attempts incorrectly assumed Home had reset the
view; its scrollbar stayed at the maximum, so downward wheel events produced
no scroll/paint samples. Those attempts are not measurements. The final runner
explicitly resets via the native accessible scrollbar and asserts value zero.
The Home-navigation observation is being investigated separately; no production
navigation change was bundled into this correction.

## v105 — Home navigation loses authority to follow-tail (§10)

**Reproduced; not fixed.** Continuing the canonical top/bottom keyboard checks
found a baseline defect independent of v104. With a followed conversation and
unmeasured earlier rows, End then Home selects/focuses the first logical item
but leaves it offscreen. Reproduced in pre-v104 production at DPR 1 and in v104
at all four DPRs. The clients/launchers exited 0; scenario assertions failed.
Logs: `navigation-v105-{baseline,fixed}-DPR-checks.log` under the common temporary
evidence directory. No production/test-source changes for this investigation.

In the DPR-1 replay, Home began at scroll value 2240 and settled at 2507 (the
new bottom after measurement), rather than zero. The first logical item's
accessible state was focused/selected **and offscreen**. Opened
`navigation-v105-trace-1-home-from-end.png`: the viewport still displays the tail.
A subsequent Home while that same first item remains current also fails.

The source and debugger agree on two parts of the same missing navigation
authority:

1. Qt keyboard navigation changes the current index, then calls
   `ConversationView::scrollTo()`. That calls `revealRow(..., false)`, unlike the
   explicit accessible-row action's `true`. Following therefore remains active.
   Admitting/measuring the newly exposed earlier rows calls
   `restoreViewport(anchor, follow=true)` and restores the bottom over Home's
   requested position. The captured stack includes `keyPressEvent` →
   `currentChanged` → `scrollTo`; the nested restore includes `setScrollValue(0)`
   → `scrollContentsBy` → `updateMaterialization` → `restoreAnchor(follow=true)`.
2. If the first row is already current, Qt's `keyPressEvent` does not change the
   index and does not call `scrollTo` at all. A selection-change-only correction
   would therefore leave repeated Home broken after manual scrolling away.

Debugger command file: `navigation-v105.gdb`; run prefix
`navigation-v105-trace`, current diagnostic binary, same Xvfb/offscreen environment.
The debugger inferior exited normally. This is not a bridge/protocol ordering
problem and not caused by the new child-focus visibility code.

Proposed narrow correction, **approval required before implementation**: make
the existing view's keyboard-navigation boundary own one explicit reveal through
its existing reveal/follow methods, including when the logical index is unchanged.
Keep Qt's native index/selection calculation, suppress its competing automatic
reveal only during that navigation dispatch, then perform the explicit reveal.
Do not globally reinterpret every `scrollTo` call as user input: Qt also calls it
during showing/restoring the view. No timer, persistent input flag, alternative
renderer or post-layout repair. Expected **12–24 net production lines** and
**50–80 regression lines**, subject to the implementation staying within that
bound; existing geometry and residency authority remain unchanged.

Verification must cover Following and Paused, cold and already measured rows,
Home/End/PageUp/PageDown/arrows, repeated same-index navigation after scrolling,
native selection modifiers, accessible navigation, child-control traversal,
streaming/manual anchors and the four-DPR live/suite checks. The existing
keyboard suite starts by scrolling to the minimum (already pausing following)
and therefore did not exercise the reproduced cold-history transition.

## v106 — approved explicit conversation keyboard navigation (§§10, 21)

Implements the user-approved v105 correction. Qt still computes the destination
and selection for Home/End, PageUp/PageDown and Up/Down. During that dispatch,
the existing `autoScroll` property is temporarily disabled and then restored;
the existing explicit `revealRow(..., true)` performs the only requested reveal,
even when the current index does not change. Home/End use the existing top/bottom
row hints. Automatic show/current-restoration calls retain their previous
following semantics. No timer, persistent flag, cache, callback, alternate
geometry authority or renderer was added. This replaces the automatic reveal
for explicit navigation, not a second corrective scroll after it.

Stage delta against `v106-baseline` copies in the common evidence directory:
**production +19/−0; regression +59/−0**, within the approved +12–24/+50–80.
Only ConversationView.cpp and its existing keyboard regression case changed.
All other pre-existing worktree changes were preserved.

Build: `cmake --build build-qualification --parallel 14`, passed. The first
eight-style/DPR regression run caught incorrect new test assumptions, not an
unfixed Home failure: Qt SingleSelection selects an unselected Ctrl+Home
destination, and revealing the last card need not consume the following turn
spacing. Assertions now match Qt's `selectionCommand` and the actual row
visibility contract; no pre-existing assertion was removed or weakened.
The initial output remains `v106-focused.log`.

Final complete suite: **80/80 passed, no skips, 154.53 seconds**, command:

```sh
env XDG_RUNTIME_DIR=/tmp/codexui-v101-runtime-5WHwn0 \
 XDG_CONFIG_HOME=/tmp/codexui-v106-full-config \
 XDG_DATA_HOME=/tmp/codexui-v106-full-data \
 LD_LIBRARY_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/lib \
 QT_PLUGIN_PATH=/home/voc/projects/drafts/CodexUI/qt-metrics-validation-YOhjyz/build/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins \
 dbus-run-session -- xvfb-run -a \
 -s '-screen 0 1280x1024x24 -extension GLX' \
 -e /tmp/codexui-timing-surfaces-JBfjvO/v106-full-xvfb.log \
 env QT_QPA_PLATFORM=offscreen CODEXUI_TIMING_POLICY=report \
 ctest --test-dir build-qualification -j14 --output-on-failure
```

Captured output: `v106-full.log`. Xvfb and child exit 0. Full-suite execution
overlapped correctness-only diagnostic clients, not the separate serial timing
comparison. The accepted report-only elapsed-time policy was not changed.

Production-linked diagnostic executable `codex-ui-interactive-v106` was built
using `QUAL_BUILD=$PWD/build-qualification python3
/tmp/codexui-interactive-uCYB7o/build-driver.py codex-ui-interactive-v106`.
The unchanged v105 failure reproduction now passes at **all four DPRs**:
Home reaches zero from the followed tail and after scrolling away while the
first item remains current. `navigation-v106.py` additionally passes cold/warm
navigation, repeated End, arrows, paging, Ctrl/Shift selection and diagnostic
traffic that creates no cards. The full forward/backward shell traversal through
all five Inspector tabs and State/Protocol/Timing also passes at every DPR,
preserving the previously qualified prompt Tab policy and visible child focus.
All twelve clients/launchers exited 0. Scripts and logs:
`navigation-v106-*`, `navigation-v106-complete-*`, `keyboard-v106-*`.
Runner: `run.py 1 1.25 1.5 2` under dbus-run-session with the Qt environment
above, `QUAL_BINARY=codex-ui-interactive-v106`, and respectively
`QUAL_SCRIPT=navigation-v105.py`, `navigation-v106.py`, `keyboard-v103.py`.
Each run uses isolated configuration/data and controlled protocol fixtures;
no personal app-server/bridge was used.

Opened full-shell captures `navigation-v106-complete-1-home.png` and
`navigation-v106-complete-1.5-end.png`: the selected first/last row is actually
visible, keyboard emphasis is contained, and neighboring panels retain their
layout. Generated but unopened captures do not acquire visual approval.

### Serial scrolling comparison

`perf-run-v106.py` compared v104-r2 with v106 after all builds, tests and other
owned diagnostic clients had ended. At each DPR: three repetitions of 24 wheel
inputs per idle/diagnostic condition, **1,152 total samples** across both binaries.
Measurement ends at the outer Qt UpdateRequest containing the scrollbar paint;
it is not compositor presentation. All eight clients/launchers exited 0.
The unchanged `conversation-latency-v104.py` explicitly resets and verifies the
scrollbar position rather than relying on the navigation behavior being repaired.

| DPR | idle p95 before → after, ms | diagnostic p95 before → after, ms |
| --- | ---: | ---: |
| 1 | 4.965 → 5.569 | 4.569 → 4.667 |
| 1.25 | 4.924 → 6.192 | 4.342 → 4.569 |
| 1.5 | 6.478 → 4.725 | 4.883 → 4.683 |
| 2 | 6.024 → 6.245 | 5.811 → 6.014 |

Worst observed sample: baseline 16.600 ms, current 15.985 ms. Quantiles vary in
both directions; the largest p95 increase is 1.268 ms. This limited sample does
not prove universally unchanged latency or lag-free behavior. The correction
adds no work to wheel-specific handling; the existing event boundary now checks
whether a key is a navigation key. No new timing limit was relaxed or mechanism
added in response to these measurements. Raw results: `perf-v106-run.log` and
`conversation-latency-v106-{baseline,fixed}-DPR-checks.log`.

## v107 — logical conversation accessibility traversal (§21)

No production/test-source changes. `conversation-a11y-v107.py` traverses the
25 logical rows in the all-content-family fixture at fonts 9/16, requested
widths 1536/1100, and DPR 1/1.25/1.5/2 using the current v106 executable.
The hidden Reasoning row remains enumerated but is not focus-actionable;
each of the other 24 rows is explicitly focused through its accessible action.
**384 focus sequences passed**. Each retained the same logical accessible ID,
name and description through residency changes and incoming diagnostic events,
exposed exactly one resident renderer child, and made the focused row visible.
All four clients/launchers exited 0. This does not establish real screen-reader
interop or every insertion/removal/folding combination.

Evidence: `conversation-a11y-v107-DPR-{seed,checks,ui,xvfb}.log`; same `run.py`
invocation/environment as v106 with `QUAL_RUN=conversation-a11y-v107` and
`QUAL_SCRIPT=conversation-a11y-v107.py`. Requested widths are not a claim that
the enlarged shell's minimum permits that actual width. Captures were generated;
their visual approval is separate from these functional/interface checks.
Opened `conversation-a11y-v107-2-font16-width1100.png`: the last card is visible,
its compact clock fallback and header controls fit, and enlarged text remains
contained. Inspector tabs use their native overflow arrows at this narrow width.

## v108 — large Agent-list navigation with enlarged fonts (§§16, 21)

Continued the existing 200-Agent navigation scenario on v106, now including
**24-point** as well as 9/16-point fonts at all four DPRs. Non-mutating
`rawWidgets` diagnostics were used. In all 12 font/DPR combinations, the first
End gesture reached the actual last Agent and scrollbar maximum; three repeated
End observations per combination remained correct. Selecting the last Agent
name, then navigating Home/End, preserved its exact focused QLineEdit and
selection. All four clients/launchers exited 0. No production/test-source change.

Same runner/environment as v106, with `QUAL_SCRIPT=agent-scroll-v91.py`,
`QUAL_RUN=agent-scroll-v108`, `QUAL_RAW_WIDGETS=1`, `QUAL_REQUIRE_BOTTOM=1`,
`QUAL_FONTS=9,16,24`. Logs: `agent-scroll-v108-DPR-{seed,checks,ui,xvfb}.log`.
Opened `agent-scroll-v108-1.25-font24-bottom.png`: the selected last name,
completed status and Copy/disclosure controls remain visible. The name field
scrolls horizontally as designed; tab overflow arrows remain usable. This is
not visual approval of every uninspected frame or every Agent body state.

## v109 — accessible folding and incoming-card insertion (§§9–10, 21)

`conversation-fold-v109.py` passed with the current v106 executable at all four
DPRs and two font/requested-width pairs (9/1536 and 16/1100): **eight folding
sequences** plus four incoming-turn insertion sequences. Actions are delivered
through the actual Qt accessible interfaces. Collapsing a turn keeps its root
renderer and logical row IDs, makes the nested Codex row inaccessible to focus,
and does not undo folding on incoming diagnostics. Expanding restores the same
logical item and its explicit accessible-focus action. Appending a new prompt
and reply preserves every existing logical ID and the previously focused item;
the new reply can then be focused/read through accessibility.

All four clients/launchers exited 0. Source/test delta **0/0**. Same runner and
isolated environment, `QUAL_RUN=conversation-fold-v109`,
`QUAL_SCRIPT=conversation-fold-v109.py`; matching seed/checks/ui/Xvfb logs.
Opened `conversation-fold-v109-1-16-expanded.png` and
`conversation-fold-v109-1.5-9-collapsed.png`: heading/clock/Copy/disclosure fit
after re-expansion, and folding leaves no reserved empty body. These are widget
and interface checks, not real screen-reader interoperability.

## v110 — restore native scroll-distance authority (§§10–11, 16, 20)

The user reported slow touchpad scrolling in Conversation and Inspector and
explicitly required restoration of native scrolling, not a speed multiplier.
The earlier pixel-only repair wrongly also preferred pixels when angle data
was available. At HEAD the Conversation used Qt's wheel interpretation; the
uncommitted pixel-preference change altered that behavior. Inspector's custom
filter also interpreted distances itself, including a hardcoded three-line
angle conversion with integer division. Importantly, Qt installs the owner
filter on its scrollbars too: the earlier suggestion that Inspector background
scrolling necessarily bypassed the custom filter was incorrect.

Both now leave angle-bearing events to Qt, including events with both pixel
and angle data. Inspector forwards child-content events to its owning native
scroll area but lets its scrollbars consume their own events. The existing
direct-displacement handling remains only for genuinely pixel-only events,
which Qt 6.10.2's QScrollBar does not consume. No new sensitivity, multiplier,
timer, cache, stored input state or gesture router. MiddleRegion gesture
ownership and follow policies are unchanged. Production **+9/−9, net zero**;
tests **+61/−7, net +54**, against `v110-baseline` copies. Preserved unrelated
worktree changes. The replaced regression assertion explicitly required the
wrong mixed-event pixel preference; it now compares against a real Qt
QScrollBar instead, while retaining pixel-only and nested-ownership coverage.

The first candidate was rejected: forwarding from the Inspector filter without
distinguishing Qt's own scrollbar caused recursion. Initial suite: 75/80
passed, five Inspector cases segfaulted; four live clients also failed.
Retained logs: `v110-tests.log`, `native-scroll-v110-fixed-*`. The corrected
boundary in r2 permits native scrollbar delivery and does not recurse.
No recursion flag or suppress/retry workaround was introduced.

Final build: `cmake --build build-qualification --parallel 14`, passed.
Final full suite: **80/80 passed, no skips, 151.92 seconds**. Same isolated
Qt/XDG/dbus-run-session/Xvfb `-extension GLX` environment as v106, with
`QT_QPA_PLATFORM=offscreen CODEXUI_TIMING_POLICY=report
ctest --test-dir build-qualification -j14 --output-on-failure`.
Output: `v110-r2-tests.log`; Xvfb diagnostics `v110-r2-tests-xvfb.log`.
Xvfb/child exited 0. The native-reference regression covers scroll-line
preferences 1/3/7, fractional input/reversal, mixed deltas, Conversation
Ctrl/Shift semantics and Inspector background/Markdown. Full-suite tests
also retain following, manual anchors, receiver lifetime and nested ownership.

Live production-linked binary: `codex-ui-interactive-v110-r2`, built with the
same `build-driver.py`/QUAL_BUILD command as v106. The existing isolated
`run.py 1 1.25 1.5 2` runner used `QUAL_SCRIPT=native-scroll-v110.py` and
`QUAL_RUN=native-scroll-v110-fixed-r2`. **All four DPRs passed**, including
idle and diagnostic traffic, fractional sequences, momentum phases, reversal,
and pixel-only fallback; all four clients exited 0.

The baseline replay (`native-scroll-v110-baseline-r2`, v106 executable, DPR 1)
fails the corrected distance assertion as expected, while the client exits 0.
For the representative event pixelY=−17/angleY=−120, the old views moved 17
logical pixels; the fixed views move 60, equal to the default native three-line
20-pixel-step path. This is an example, not a claimed physical-device ratio.
The original diagnostic mistakenly compared two affected paths and passed;
that output is preserved under `native-scroll-v110-baseline-*`. The corrected
reference uses angle-only input, and the independent C++ oracle uses Qt itself.

Opened `native-scroll-v110-fixed-r2-1.25-native-scroll.png`: both panels retain
their established geometry and readable content after scrolling. This does not
claim physical touchpad/compositor qualification, which remains outside the
authorized workflow. Nothing installed, committed, pushed or restarted.

### Equal-distance scrolling cost

After the full suite and all other diagnostic clients exited, serial comparison
of v106/v110-r2 used the unchanged `conversation-latency-v104.py` via
`perf-run-v110.py`. Angle-only inputs move equal distances in both binaries,
so this compares processing/presentation cost rather than different distances.
**1,152 samples**, 72 per version/DPR/traffic condition; all eight clients exited
0. Measure: input delivery to the outer Qt UpdateRequest containing scrollbar
paint, not compositor presentation. No own concurrent builds/tests/UI clients.

| DPR | idle p95 before → after, ms | diagnostics p95 before → after, ms |
| --- | ---: | ---: |
| 1 | 5.192 → 5.438 | 4.572 → 4.814 |
| 1.25 | 4.789 → 4.947 | 4.977 → 4.172 |
| 1.5 | 5.710 → 6.937 | 4.652 → 4.475 |
| 2 | 6.395 → 6.548 | 5.653 → 5.895 |

Maximum: before 15.729 ms, after 15.500 ms. Quantiles vary in both directions;
the largest p95 increase is 1.227 ms. No universal speedup/no-lag claim follows
from this finite measurement. The existing accepted report-only timing policy
is unchanged. Logs: `perf-v110-run.log` and
`conversation-latency-v110-{baseline,fixed}-DPR-checks.log`.

The separate serial Inspector comparison used `inspector-scroll-cost-v110.py`
on the expanded 400-paragraph Agent body, DPR 1, equal-distance angle-only
input through Markdown. **288 samples** across two versions and idle/diagnostic
conditions; both clients exited 0. Idle p95: 3.350 → 3.448 ms; diagnostic p95:
3.883 → 3.146 ms. Maximum: 3.978 → 3.784 ms. The other DPRs have the functional
distance checks above, not an Inspector timing comparison. Evidence:
`inspector-cost-v110-{baseline,fixed}-1-checks.log`. `git diff --check` passed.

## Still open

The canonical inventory has not been narrowed. The v103 clipped conversation
keyboard-focus defect is corrected and qualified in v104.
The separate v105 keyboard-navigation/follow-tail defect is corrected in v106;
the related logical accessible-focus traversal passes in v107, and folding/
insertion identity checks pass in v109. Large Agent navigation also passes at
24-point font in v108.
The v98 Protocol follow defect
and the accessible-input limitation exposed by v100 are corrected and verified
in v101; this does not close the entire matrix. Unexecuted combinations include
remaining pagination/catalog query combinations beyond v70/v73/v76/v85/v102; remaining
lifecycle failures/partial-success retries beyond v78/v86/v94/v95; further concurrent frontend/traffic combinations beyond v88;
remaining normal-motion transitions beyond v77/v87/v90; completion of v91
qualification beyond its recorded passes; the font-change repolish crash
recorded in v92 is corrected and its reproduced sequence passes in v93.
The former Agent clipping was diagnostic-induced in the traced
replay, and the BrandLockup clamp crash is corrected as recorded above;
additional real-backend unavailable-file
outcomes beyond the v24 observation and controlled recovery verified in v66;
remaining multi-repository/diff configurations; remaining timestamp
surface/font/width/DPR combinations beyond v25/v83/v96; full
application accessibility action traversal; remaining event-to-presentation
latency conditions beyond the ten-surface runs above;
and the remaining element/state/font/width matrix.
The three reproduced dialog accessibility omissions recorded in v66 are resolved
by v68/v69 above; broader application traversal and visual combinations remain.
The separately reproduced short-dialog typing failure in v71 is corrected and
qualified by v72 above; broader dialog combinations remain in the matrix.
The font-only focused-caret failure in v74 and the additional resize-only case
are corrected and qualified in v75 above; this does not close the entire matrix.
The Project editor font-only caret failure in v79 is corrected and qualified
by v80. The separately reproduced outer-form request-editor failure in v81
is corrected and qualified by v82; remaining dialog combinations stay open.
The dense-Unicode correction is verified in the isolated Qt build above but is
not installed; the measured worst-case update is not universally below 16.7 ms.
The original
short-heading width collapse and residual filename clipping are resolved by
the two approved header corrections. Successful filename qualification does
not override remaining visual/performance failures elsewhere in the matrix.

An earlier disposable real app-server accepted 10,000 injected response items but returned
no turns from `thread/turns/list`. That setup is **not** counted as a successful
retained-history pagination test or as a CodexUI defect. No production history
or app-server source was modified. This earlier setup failure was superseded by
v24's successful real-server 5,000-turn/10,000-row retained-history qualification
at all four DPRs; it is not a remaining blocker for that already-recorded case.

Native compositor, physical touchpad, actual Spectacle/file-manager and real
screen-reader interoperability remain outside the authorized offscreen workflow.
Offscreen passes must not be represented as proof of those behaviors.

Earlier v21 cleanup: diagnostic UIs were asked to quit; the independently launched bridge
and app-server did not exit on TERM and were stopped by their verified PIDs.
The temporary authentication copy was removed; original credentials, production
processes and disposable evidence files were preserved.

The v22 clients/backend were launched independently afterward. Their current
shutdown status cannot be verified from the restricted resumed session; do not
infer v22 cleanup from the v21 record above.
The v22 temporary authentication copy was removed after access became blocked;
the original production credentials were not changed.

V24 cleanup: all four isolated Qt clients quit through their diagnostic control
sockets and exited with status 0. The independently launched bridge/app-server
were verified by PID and command line; after not exiting on TERM, only those
two processes were stopped with KILL. The renewed temporary authentication copy
was removed. Original credentials and production processes were untouched.
