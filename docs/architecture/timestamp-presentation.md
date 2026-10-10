<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p>
  <a href="../../README.md#project-overview" title="Codex(W)UI repository"><img src="../media/page-banner.svg" alt="Codex(W)UI repository" width="100%"></a>
</p>
<!-- snodec:end page-header -->

# App-server timestamp presentation

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

## Verification scope

Timestamp tests exercise units, missing values, lifecycle reconciliation, routing, keyboard access and layout at multiple DPRs. Their offscreen results do not establish native compositor or real screen-reader qualification; an enlarged-font window may exceed its requested minimum size. The separate implementation record preserves exact tests, commands and historical outcomes.

## Implementation record

Historical verification and change accounting are kept in the [implementation record](../records/timestamp-verification.md#page-overview). Follow the current user guide above for behavior; historical test commands are not an onboarding requirement.
