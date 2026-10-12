<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-records-compact.svg"><img src="../media/nav/banner-records.svg" alt="CodexUI — Records" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Records"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-records-dark.svg"><img src="../media/nav/up-records-light.svg" alt="Up to Records" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNodeC organization" width="160" height="28"></picture></a><a href="../../README.md#project-overview" title="CodexUI repository"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" width="160" height="28"></picture></a><br clear="all"></p>
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

## 2026-10-12: local draft placement

Verification baseline: `9a84371b6d4a8e2504065d89b1b080d5ff0b7df1`; local working-tree fix, Debug, Qt 6.11.2. This is targeted qualification for local draft grouping and promotion, not a complete execution of the [canonical native interactive suite](../qualification/native-test-suite.md). No installed applications, personal services or production Codex data were changed.

Invariant: the intended project/section determines placement from the local draft through worker admission; a confirmed thread then uses server-owned membership. The complete renderer-local draft row is fed through the existing NodeGraphUiAdapter grouping projection. Correlated graph rows supersede that input within the existing traversal. The worker relates admitted local threads to existing group entities; it does not manufacture group entities or override confirmed server membership. The forced top-level placement and duplicated creation/recovery setup were removed. No new timer, cache, persistent flag, renderer or wire API was introduced.

The existing Debug build was rebuilt with 14-way parallelism. Ten focused CTests passed: four ThreadPane DPR variants, shell integration, application layout, worker logic, protocol updater, client-runtime dispatch and style source policy. Four additional ThreadPane runs passed at 220 logical pixels wide, again at DPR 1, 1.25, 1.5 and 2. Widget screenshots were captured and reviewed. Coverage includes project-only drafts, sections inside projects, standalone sections, refreshes, filters, grouping-mode changes, accessible row identity, animation, the actual project context-menu/dialog/composer path, worker admission and canonical attachment. Existing recovery tests execute the consolidated creation path. These fixtures use the real widgets and worker/graph boundaries with a simulated provider; live production-provider acceptance is not claimed.

An initial restricted-sandbox dispatch test could not bind its disposable Unix socket; the final isolated-home run outside that restriction passed. The final Xvfb wrappers and children returned zero. Initial test-fixture issues were corrected without weakening assertions: catalog replies require registered pending requests, a modal dialog can retire its context menu, and the animation fixture explicitly selects normal motion. No production workaround was added for those fixture issues.

The recorded 320-row grouped profile measured adapter/population/no-op work at 7.611/134.545/1.366 ms before the change and 7.906/130.704/1.466 ms after it. Both retained 32 widgets, zero index widgets, one fixed document, stable no-op pixels, no offscreen animation work and five bounded visible animation paints. Both observed one no-op paint; this existing strict benchmark failure was not hidden or weakened. The separate adapter suite still failed its pre-existing 8 ms token-projection gate, measuring 14.850 ms at 10,000 threads in isolation; correctness cases executed. These observations do not establish universal performance or a fully green broad native gate.

Physical change accounting: production +78/-53 lines (net +25), tests +244/-17 (net +227), excluding documentation. The approved production growth carries draft membership through the existing projection and admission boundaries; duplicate setup and root-only placement are deleted. Temporary evidence is under `/tmp/codex-draft-group-fix`, including `ctest-verified.log`, `ctest-narrow.log`, `ctest-adapter-isolated.log`, `baseline-profile.log`, `final-profile.log`, `captures` and `captures-narrow`.
