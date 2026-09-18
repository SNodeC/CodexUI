# CodexWebUI qualification

This record qualifies the browser frontend against
[the web 1.0 contract](web-1.0-contract.md). Cross-frontend agreement is
evidence-backed only where the same fixture corpus is executed by native C++
and browser TypeScript. The other suites are browser-local regression evidence;
they neither execute Qt nor claim pixel identity with it.

## Verification gates

- Generated protocol names, operation maps, and source-schema digest match the
  checked-in C++ protocol.
- The shared frontend-presentation corpus is executed by both C++ and
  TypeScript and checks status normalization, lifecycle fallbacks, active-turn
  identity, Plan replacement/outcomes, and Agent status precedence.
- Browser-local SDK tests cover routing, pending callbacks, bridge roles,
  provider generations, disconnect failure, WebSocket framing, and
  backpressure.
- Browser-local normalizer and presentation-model tests cover authority,
  generation, sequence, hydration, terminal state, repository hints, and child
  ownership.
- Conversation tests cover grouping, prompt admission,
  materialization, reasoning-first order, history, terminal output, image
  metadata, and agent correlation within the browser implementation.
- Browser-session tests use a scripted bridge endpoint and assert the protocol
  presentation-action methods, resume-before-turn ordering, provider
  recovery, observer direct paging, one-page cursor continuation/retry,
  chronological multi-page item merge, active-child hydration, the global
  eight-item-page ceiling, and malformed-input containment.
- Settings and pending-request tests assert the protocol request payloads.
- Server rendering verifies the shell landmarks and accessible connection and
  disclosure controls without requiring a second UI state implementation.
- Viewport tests cover stable card/pixel anchors, native folding geometry, and
  nested-scroll boundary ownership; responsive tests cover the 1160/760 mode
  boundaries, semantic thread-tree selection, focus styling, contrast tokens,
  and coarse-pointer targets.
- The packaging gate rejects a missing production artifact, performs both
  standalone and combined CMake installs, and verifies the staged entry point
  and assets.

## Measured presentation cost

Measurement command:

```sh
npm run profile --prefix web
```

Representative input: one authoritative thread containing 100 turns and
10,000 mixed conversation items, followed by 2,000 deltas applied to one live
item. Five fresh Node 24.19.0 processes were run on 2026-09-14 on an Intel Core
i7-14700HX (x86-64). Limits are the rounded-up 1.25× observed maxima.

| Operation | Median | Five-run range | Hard limit |
| --- | ---: | ---: | ---: |
| Authoritative hydration, 10,000 items | 37.09 ms | 32.90–37.37 ms | 47 ms |
| Full projection, 10,000 visible cards | 35.46 ms | 35.07–35.61 ms | 45 ms |
| Apply 2,000 streaming deltas | 2.99 ms | 2.87–3.06 ms | 4 ms |

The production view starts with the native 80-item history window, schedules
at most one React publication per animation frame, retains keyed cards, and
increases the effective window while a reader is paused. The 10,000-card full
projection is therefore a qualification stress case, not the initial DOM
surface.

No speculative cache or alternate reducer was introduced: the measured pure
model and projection remained comfortably below one display interval for the
normal 80-item window, so the native authority/index structure was retained.

The production-App gate uses the built Vite artifact, a scripted WebSocket at
the real bridge boundary, and a fresh headless Chrome 151 process per run:

```sh
npm run build:app --prefix web
npm run qualify:browser --prefix web
```

Five runs on the same date and machine produced these local baselines. The gate
also requires exactly 80 resident cards, stable card/text/settings-control
identity, zero structural mutations outside the streamed text node, follow-tail
within 1 px, and a detached semantic anchor within 1 px.

| Production-App phase | Median | Five-run range | Hard limit |
| --- | ---: | ---: | ---: |
| 10,000-item hydration wall time | 93.20 ms | 90.80–112.80 ms | 141 ms |
| 10,000-item hydration task time | 102.14 ms | 98.93–122.66 ms | 154 ms |
| Idle task time | 0.70 ms | 0.51–0.77 ms | 1 ms |
| Equal-settings acknowledgement task time | 19.77 ms | 18.10–19.95 ms | 25 ms |
| 2,000-delta synchronous ingest | 14.80 ms | 13.90–15.80 ms | 20 ms |
| 2,000-delta settlement | 36.30 ms | 35.30–37.30 ms | 47 ms |
| 2,000-delta task time | 36.66 ms | 35.69–37.77 ms | 48 ms |

Hydration was exactly two layout/style passes in every sample; streaming was
exactly one; idle and the semantic settings no-op were exactly zero. These are
local regression gates, not portable hardware performance claims.

## Browser representation and resilience

- WebSocket and WSS are the only exposed transports.
- The bridge URL is explicit and retained in browser local storage.
- Provider-local paths are presented as metadata; no local browser filesystem
  capability is implied.
- Provider generation changes clear provider-owned presentation state and
  trigger discovery/hydration through the existing bridge authority.
- Notices are bounded to one visible message and follow the native 10-second
  error and 6-second informational auto-dismiss policy.
- Reduced-motion preference disables shimmer, spinner, and smooth scrolling.
- The center pane owns per-thread follow/paused scroll state with stable
  card/pixel anchors; a paused anchor is not evicted when streamed items
  arrive. The composer grows upward over an opaque reserved surface while
  command surfaces retain their own follow/pause and wheel-boundary ownership.
- The release gate starts a real headless Chromium at 760, 521, and 360 px and
  confirms zero document-width overflow with the full connection-control
  shape. It also verifies the visible focus ring, synchronous drawer focus,
  inert background, bidirectional Tab wrapping, Escape focus return,
  breakpoint-removal fallback, and 44 px coarse-pointer targets.
