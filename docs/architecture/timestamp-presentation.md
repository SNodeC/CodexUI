<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p>
  <a href="../../README.md#project-overview" title="Codex(W)UI repository"><img src="../media/page-banner.svg" alt="Codex(W)UI documentation" width="100%"></a>
</p>
<!-- snodec:end page-header -->

# App-server timestamp presentation

<!-- snodec:begin back -->
<p>
  <a href="../../README.md#project-overview" title="CodexUI"><img src="../media/menu/back-codexui.svg" alt="CodexUI" width="96" height="24"></a>
</p>
<!-- snodec:end back -->

## Authority and inventory

The protocol remains the time authority. `ProtocolTimingPaths.inc` records the
declared fields from Codex `rust-v0.154.0`, source revision
`6b9826e3aa83b1a5947db50f4332cb9c65f1b340`. It composes shared type paths rather
than searching arbitrary tool output or user text for date-looking keys.
Update these paths alongside future protocol schema changes.

| Context | Fields | Units |
| --- | --- | --- |
| Thread | createdAt, updatedAt, recencyAt, sectionEnteredAt | Unix seconds |
| Turn | startedAt, completedAt; durationMs | Unix seconds; duration milliseconds |
| Item lifecycle | startedAtMs, completedAtMs; durationMs | Unix milliseconds; duration milliseconds |
| Timeline | started_at, completed_at; duration_ms | Unix seconds; duration milliseconds |
| Approval and automatic/strict review | startedAtMs, completedAtMs, where declared | Unix milliseconds |
| Hook | startedAt, completedAt; durationMs | Unix seconds; duration milliseconds |
| Project | createdAt, updatedAt, recencyAt | Unix seconds |
| Thread goal | createdAt, updatedAt | Unix seconds |
| Workspace message | createdAt, archivedAt | Unix seconds |
| File metadata | createdAtMs, modifiedAtMs | Unix milliseconds; zero means unavailable |
| Plugin | installedAt | Unix seconds |
| External import history | completedAtMs | Unix milliseconds |
| Rate-limit windows and image failure | resetsAt | Unix seconds |
| Reset credit | grantedAt, expiresAt | Unix seconds |
| Model upgrade | retirementAt | Unix seconds |
| Remote control | lastSeenAt, expiresAt, where declared | Unix seconds |
| Server envelope | emittedAtMs | Unix milliseconds |
| currentTime/read response | currentTimeAt | Unix seconds, **client clock** |

Absent data is not reconstructed from arrival order, GUI receipt time, final-answer
position or another object's clock. Explicit null remains unavailable. Sparse
retained history cannot erase lifecycle timestamps already observed for that
same item/turn. Zero is a valid epoch except for the file-metadata sentinel.
Durations are never formatted as dates; out-of-range dates are identified rather
than overflowing or guessing units.

## Where to see them

- Conversation headers show compact local start/end times and reported duration.
  Only the turn-root card adds its owning turn's timing. Each ordinary card uses
  its own item lifecycle. Narrow headers retain a keyboard-accessible clock
  button instead of imposing a wider minimum width.
- Hover for exact details, or activate the timing button to open **Info → Timing**.
  The same view is reachable through the thread context menu's **Timing details**.
- Timing details show the selected object, owning turn/thread and related project
  and goal, plus bounded current peripheral facts already retained in NodeGraph.
  Dates include numeric local UTC offset, timezone abbreviation, UTC and raw
  protocol value/unit. Changing threads clears the previous timing selection.
- Thread tooltips/accessibility descriptions include the server's four thread
  times separately from the existing locally influenced activity/sort fields.
- **Info → Protocol** shows the timestamp inventory for observed requests,
  responses and notifications, including event-only data not retained in graph
  objects. `emittedAtMs` is server emission; `recorded locally` is the worker's
  receipt/recording clock, not the old GUI-rendering time. `currentTimeAt` is
  explicitly identified as the client clock.

No new fetches are made solely to populate timing details. Unrequested APIs,
omitted historical fields and entries aged out of the bounded protocol log are
not available. This is a display of supplied data, not a new timestamp database.

## Architecture and work bounds

There is one schema-path extractor and one native time formatter. Existing graph
state, projected values, single card renderer, inspector routing and protocol
diagnostic queue remain the owners of their respective responsibilities.
Extraction limits are 128 facts and 2048 visited schema-path nodes per payload;
peripheral graph enumeration is limited to 64 indexed nodes and 128 displayed
facts. Protocol overflow is visibly identified. Graph object projection does not
walk embedded conversation children: each child has its own graph node.

The feature adds one bounded-residency tool button per card and one Info detail
page. It adds no polling timer, animation, worker, cache, ordering policy,
background service or parallel renderer. Timestamp-only card updates are
paint-only; semantic no-ops do not relayout or replace focus. Existing viewport
Tab-focus policy includes the new button.

## Verification

`TimestampPresentationTest` covers schema paths, units, missing/zero/overflow
values, DST ambiguity, bounded pages, graph lifecycle/history reconciliation,
turn versus item scope, nested goal/hook facts, routing, local versus server
recency, keyboard access, no-op/paint-only behavior, geometry, three card widths
and two font sizes. CTest registers DPR 1 / 1.25 / 1.5 / 2 variants.

`ClientRuntimeDispatchTest` covers server-emission versus local-recording
provenance and reverse currentTime/read response metadata. The shell integration
case exercises the actual session/worker/shell inspector path, thread switching,
800/1500 requested widths and normal/enlarged fonts. Set
`CODEXUI_TIMESTAMP_SCREENSHOTS` to a directory to save rendered UI captures.

All runtime checks use `xvfb-run -a env QT_QPA_PLATFORM=offscreen`; this is not
native-compositor or real screen-reader qualification. At enlarged fonts the
shell may enforce its existing minimum width above the requested 800 pixels;
captures show the actual resulting geometry. Layout checks do not claim that
every pre-existing narrow-shell control is fully expanded.

### Local verification record (2026-09-25)

Baseline: `6f58d7f`. Debug build with installed Qt and AISuite, GCC 16.2.

```sh
cmake -S . -B /tmp/codexui-timestamps-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/codexui-timestamps-debug --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest \
  --test-dir /tmp/codexui-timestamps-debug --output-on-failure --parallel 14
```

Initial baseline: 70/72 passed. Full implementation run: 73/76 passed.
Both have the token-projection 8 ms limit failure and exact tooltip stylesheet
assertion failure. The full implementation run also failed the 10,000-row DPR-2
benchmark. A pristine archived baseline independently reproduces its structural
pixel failure: a pending steering animation starts during a supposedly static
comparison. A subsequent isolated implementation run passes every benchmark
gate at DPR 1 and 2. The benchmark and its limits were not weakened. The full
run's no-op/timing failures are not presented as a clean pass; isolated results
below distinguish them from reproducible feature failures.

All four timestamp DPR cases and eight selection/focus cases pass. Full-shell
integration passes at each DPR with the application stylesheet and captured
normal/enlarged-font layouts. The runtime provenance checks pass. The existing
optimistic-promotion pixel test now settles the returned `GeometryChanged`
impact through the same public measurement boundary used by ConversationView,
before taking its pixel baseline; no production settlement workaround was added.

```sh
for scale in 1 1.25 1.5 2; do
  xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR="$scale" \
    QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough \
    CODEXUI_TIMESTAMP_SCREENSHOTS=/tmp/codexui-timestamp-screenshots \
    /tmp/codexui-timestamps-debug/codexui-shell-integration-test
done
```

Isolated 10,000-row benchmark comparison (microseconds, p95; one sequential run
per build/DPR, not a statistical proof of universal responsiveness):

| Operation | DPR 1 baseline → current | DPR 2 baseline → current |
| --- | --- | --- |
| Warm wheel to paint | 1027 → 1117 | 1495 → 1754 |
| Warm scrollbar to paint | 1052 → 1088 | 1601 → 1692 |
| Command streaming | 907 → 1081 | 1896 → 1840 |
| Following append | 5730 → 6044 | 5943 → 6752 |

Document peak remains 29; height-index update steps remain 13; maximum normal
constructions per frame remain 5. Widget peak is 479 → 510, the additional
timing button per resident card. Semantic no-op stability passes both builds at
both DPRs. There is measurable display cost; this is not a claim of zero overhead.

```sh
# Repeat for baseline/current executables and scale 1/2, sequentially.
xvfb-run -a env QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 \
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough \
  /tmp/codexui-timestamps-debug/codexui-conversation-view-benchmark 10000 2
```

Logs: `/tmp/codexui-timestamps-final-all.log`,
`/tmp/codexui-timestamps-final-focus.log`,
`/tmp/codexui-timestamp-shell-styled-dpr-*.log`, and
`/tmp/codexui-timing-isolated-{baseline,current}-{1,2}.log`.

Change accounting: production **+711 net lines**, tests **+587 net lines**,
within the approved +600–900 / +400–600 estimates; build configuration +23 net
lines. Growth supplies the new schema inventory, shared formatter and UI
projection/actions rather than a parallel state or rendering implementation.

Final rebuilt targeted run: **15/15 passed** (timestamp DPR matrix,
selection/focus, conversation virtualization, runtime dispatch and shell
integration), `/tmp/codexui-timestamps-final-targeted.log`.
