# CodexWebUI qualification

This record qualifies the browser frontend against the observable native
behavior fixed in [the web 1.0 contract](web-1.0-contract.md). It does not
claim pixel identity with Qt.

## Equality gates

- Generated protocol names, operation maps, and source-schema digest match the
  checked-in C++ protocol.
- SDK routing, pending callbacks, bridge roles, provider generations,
  disconnect failure, WebSocket framing, and backpressure use equivalent C++
  lifecycle scenarios.
- Normalizer and presentation-model tests replay the native authority,
  generation, sequence, hydration, terminal-state, repository-hint, and child
  ownership scenarios.
- Conversation tests replay native grouping, prompt admission,
  materialization, reasoning-first order, history, terminal output, image
  metadata, and agent correlation scenarios.
- Browser-session tests use a scripted bridge endpoint and assert the same
  C++ presentation-action methods, resume-before-turn ordering, provider
  recovery, and malformed-input containment.
- Settings and pending-request tests assert the exact native request payloads.
- Server rendering verifies the shell landmarks and accessible connection and
  disclosure controls without requiring a second UI state implementation.

## Measured presentation cost

Measurement command:

```sh
npm run profile --prefix web
```

Representative input: one authoritative thread containing 100 turns and
10,000 mixed conversation items, followed by 2,000 deltas applied to one live
item. Five fresh Node processes were run on 2026-08-28 with Node 24.19.0 on an
Intel Core i7-14700HX (x86-64).

| Operation | Median | Observed range |
| --- | ---: | ---: |
| Authoritative hydration, 10,000 items | 44.09 ms | 43.62–44.36 ms |
| Full projection, 10,000 visible cards | 33.11 ms | 31.31–37.66 ms |
| Apply 2,000 streaming deltas | 9.85 ms | 9.38–10.35 ms |

The production view starts with the native 80-item history window, schedules
at most one React publication per animation frame, retains keyed cards, and
increases the effective window while a reader is paused. The 10,000-card full
projection is therefore a qualification stress case, not the initial DOM
surface.

No speculative cache or alternate reducer was introduced: the measured pure
model and projection remained comfortably below one display interval for the
normal 80-item window, so the native authority/index structure was retained.

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
- The center pane owns per-thread follow/paused scroll state; a paused anchor
  is not evicted when streamed items arrive.
