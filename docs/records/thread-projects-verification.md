<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p>
  <a href="../../README.md#project-overview" title="Codex(W)UI repository"><img src="../media/page-banner.svg" alt="Codex(W)UI repository" width="100%"></a>
</p>
<!-- snodec:end page-header -->

# Thread Projects Verification

Historical implementation and verification evidence. This is not a current qualification claim or a user setup procedure.

Implementation baseline: `b9ff6e5` on `master`, initially clean.
This feature is native-only; browser protocol/presentation compatibility is
retained. No AISuite, SNode.C, app-server, installation or running-service changes
were made.

## Plan status

1. Graph/protocol integration: implemented and targeted checks passing.
2. Bounded query paging and compatibility: implemented and targeted checks passing.
3. Grouped presentation and view preferences: implemented; four-DPR widget checks passing.
4. Management and new-thread workflows: implemented; typed dispatch, actual
   creation/edit dialogs, input validation and empty-group thread actions checked.
5. Qualification: targeted checks passing; full repository qualification remains
   open for the separately recorded pre-existing/fluctuating failures below.
   Real upstream project persistence and native compositor/screen-reader behavior
   have not been claimed as verified by offscreen/mock-backed checks.

## Verification record — 2026-09-26

Build directory: `/path/to/workspace/drafts/CodexUI/build/Desktop_GCC-Debug`.
Every Qt execution used `xvfb-run -a env QT_QPA_PLATFORM=offscreen`; no xcb or
desktop-approval workflow was used. Builds used `--parallel 14`.

Initial baseline: 18 existing thread-panel/runtime/shell checks passed. The final
targeted run passed **24/24**, including four newly registered 10,000-thread
grouped benchmarks, the existing twelve thread benchmarks, four thread UI DPR
variants, runtime dispatch, shell integration, protocol updater and worker logic.

```sh
cmake --build /path/to/workspace/drafts/CodexUI/build/Desktop_GCC-Debug --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest \
  --test-dir /path/to/workspace/drafts/CodexUI/build/Desktop_GCC-Debug \
  -R 'codexui-(nodegraph-thread-pane|thread-groups-performance|thread-pane-performance|client-runtime-dispatch|shell-integration|protocol-updater|worker-logic)' \
  --output-on-failure --parallel 14
```

The real ThreadPane and project dialog were rendered and visually inspected at
DPR 1, 1.25, 1.5 and 2; widths included 250, 330 and 420 logical pixels, with
enlarged-font input. The checks cover identity-preserving regrouping, cross-project
children, standalone sections, title/archive filtering, observer controls,
unsupported-method fallback and semantic-no-op pixel equality. Screenshots are
temporary artifacts under `/tmp/codexui-groups-*.png` and
`/tmp/codexui-group-dialog-*.png`.

The Unix-bridge integration harness verifies typed CRUD wire paths, metadata
preservation, empty section acknowledgements, two-read concurrency, cursor retry,
old/recreated query rejection, archived discovery, observer rejection, capability
fallback, and section failure after successful thread creation. Its provider is
simulated; these are not claims of mutations against the user's real projects.

Browser: `npm run build` and `node --test --test-concurrency=14 tests/*.test.mjs`
in `web/`: **336/336 passed**, no skips.

### Performance comparison

An independent Debug build of the unchanged starting commit was made from
`git archive HEAD` in `/tmp/codexui-grouping-baseline.Al3Rxx` with the same compiler
and installed dependencies (`CODEXUI_INSTALL_WEB=OFF`). This did not alter the
worktree or install anything.

At 10,000 threads, baseline flat projection/population/no-op: approximately
106/38/21 ms. New grouped projection/population/no-op across four DPRs:
164–165/39–41/17–18 ms. Full grouping projection is additional work, not a claimed
speedup; it stays within the existing quantitative admission budget. Flat
benchmarks remain around 1.44 s overall versus 1.40–1.48 s baseline observations.
These elapsed values are environment-sensitive, not a universal no-lag claim.

Both modes retain constant widgets: 32 before/after population with the new
controls, zero index widgets, one fixed document. No-op paints: zero. Offscreen
animation paints/timers: zero. Visible animation remains bounded to its row.

### Wider-suite limitations

The full 76-test run (before adding four grouped benchmark registrations) passed
72 and failed four. Existing CI policy `CODEXUI_TIMING_POLICY=report` was used for
that run; correctness assertions were not bypassed or weakened.

- Conversation cards: tooltip style assertion also fails on unchanged `b9ff6e5`.
- 10,000-row conversation DPR 2: structural-transaction correctness gate also
  fails on unchanged `b9ff6e5`. The full run additionally observed a no-op failure.
- Conversation DPR 1 and Inspector geometry DPR 1.25 failed in the broad run,
  then passed isolated repeats without source changes in those subsystems.
- The separate 8 ms token-projection timing limit is exceeded on both builds
  (roughly 16 ms). It is reported, not treated as proof of passing the strict limit.

No unrelated conversation/Inspector fixes or threshold changes were made. Full
suite green/native-platform qualification is therefore not marked complete.

## Change accounting

`cloc src`: 38,554 → 39,968 code lines (**+1,414**).
`cloc tests`: 49,069 → 49,936 code lines (**+867**).
Physical source delta including comments/blank lines: production **+1,467**,
tests **+882**. CMake: **+10**, documentation separate. Within the approved
production-growth envelope. Existing pager and row-frame paths were replaced,
not retained as parallel implementations.

No commit, push, installation or restart has been performed.

## Follow-up: project-first ordering and single-line titles

Both reported defects were reproduced before correction using the real ThreadPane
under Xvfb/offscreen. The enum-based comparator ranked Thread before Project;
draft insertion independently reserved row zero. The shared kind order now puts
Project, Section, Thread, Page in that order, and draft placement honors it.
The redundant Page-last special case is deleted. No additional sorting pass,
timer, cache or renderer is introduced.

The fixed-height delegate treated embedded title line breaks as additional lines.
It now elides a single-line whitespace-normalized painting string and draws it
with TextSingleLine. Authoritative titles, tooltips and accessible names retain
their full text. LF, CRLF, Unicode line separators and long Unicode titles are
checked against single-line pixels and unchanged row geometry.

The build and the same 24-check command above pass after the correction.
Four additional screenshot runs used widths 250/330/420 and font sizes 12/16;
all passed and were visually inspected at DPR 1/1.25/1.5/2. For example:

```sh
xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.25 \
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough \
  CODEXUI_GROUP_WIDTH=250 CODEXUI_GROUP_FONT=16 \
  CODEXUI_GROUP_SCREENSHOT=/tmp/codexui-groups-final-correction-1.25.png \
  /path/to/workspace/drafts/CodexUI/build/Desktop_GCC-Debug/codexui-nodegraph-thread-pane-ui-test
```

Grouped 10,000-thread benchmark wall times: 1.02–1.04 s versus the preceding
1.03 s; flat 10,000-thread times: 1.43–1.46 s versus 1.43–1.46 s. All registered
gates passed; these are measured checks, not universal no-lag proof.
Follow-up growth: production +9 code/physical lines, tests +54 code/physical
lines, within the already approved feature envelope. It corrects placement at
the existing reconciliation boundary rather than adding a later repair callback.
The separately documented wider-suite/native-platform limitations remain open.

## Creation-workflow qualification

The real creation menus/dialogs were exercised at all four DPRs. Checks cover
blank-name rejection, relative-root rejection, multiple absolute project roots,
Unicode descriptions, idempotency keys, exactly one submitted mutation, no
invented optimistic group, and New thread here with the correct project/section
and workspace. Eight dialog screenshots were inspected under Xvfb/offscreen.

This reproduced an empty-section omission: Projects view previously constructed
sections only from thread memberships. It now also projects catalog sections
with no incoming SectionMembership relation. The existing section builder is
reused; there is no second presentation path, cache, timer or speculative server
state. Empty-to-populated-to-empty transitions preserve the same standalone row.
The assertion failed before this correction and passed afterward at every DPR.

This continuation adds **9 production code/physical lines** and **125 test code
lines / 127 physical lines**. The total stays inside the approved feature-growth
allowance. The extra branch projects already-authoritative catalog data; the
new local lambda consolidates the existing section construction.

Build: the same `cmake --build ... --parallel 14` command above. Verification:

```sh
xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest \
  --test-dir /path/to/workspace/drafts/CodexUI/build/Desktop_GCC-Debug \
  -R 'codexui-(nodegraph-thread-pane|thread-groups-performance|thread-pane-performance|client-runtime-dispatch|shell-integration|protocol-updater|worker-logic|nodegraph-ui-adapter)' \
  --output-on-failure --parallel 14
```

Result: **24/25 passed**. The added adapter suite hit its previously reproduced
10,000-thread token-projection timing limit (16,059 us versus 8,000 us).
Rerunning only that suite with the already accepted
`CODEXUI_TIMING_POLICY=report` passed all its correctness checks. No timing limit
or correctness assertion was changed. Grouped 10,000-thread benchmarks took
1.02–1.06 s versus the preceding 1.02–1.04 s; flat benchmarks took 1.45–1.48 s
versus 1.43–1.46 s. Structural residency/work gates passed.

Screenshots used the same executable and Xvfb/offscreen environment, with
`QT_SCALE_FACTOR` set to each of 1/1.25/1.5/2,
`QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough`, and
`CODEXUI_GROUP_CREATE_SCREENSHOT=/tmp/codexui-create-<DPR>`.
All four additional widget runs passed. These checks use authoritative graph
fixtures and the separately tested simulated bridge, not the user's real project
store. Upstream persistence and native-compositor qualification remain unclaimed.
