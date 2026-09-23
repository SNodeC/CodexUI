# CodexUI architecture

CodexUI presents the Codex app-server through the existing `codex-bridge`.
The app-server remains authoritative for provider data and persistence; CodexUI
keeps only current in-memory state and genuinely local interaction state.

## Native application

The native application uses one shared `NodeGraph`, with one protocol writer
and UI/projection readers:

```text
Qt main thread
  existing widgets, viewport, drafts and local interaction state
  non-blocking reads from one NodeGraph
        <-> bounded typed SPSC queues and two Linux eventfds
existing SNode.C worker thread
  transport, CodexBridge, protocol decode/encode and sole graph writes
shell-owned projection pool
  read-only conversation snapshots/deltas and Inspector projection
  owning value results queued back to the Qt thread
```

`FrontendSession` owns the graph and outlives the shell. Shell destruction
invalidates queued UI continuations, then joins its projection pool before
the session can destroy the graph. Existing in-flight guards coalesce work
separately for conversation and Inspector; neither worker accesses widgets.
Global-pool shutdown at application destruction would be too late for these
borrowed graph readers. Local Git/image jobs remain separate global-pool work.

The composer also owns a single-worker attachment-preparation pool for pasted
images and dropped files. It touches no graph state, returns results to the Qt
thread, and is joined before composer destruction. Pending results are invalidated
when the draft is cleared/replaced; image files are durable application data.

There is no internal JSONL, socketpair payload path, presentation model, mirror
graph, snapshot history, or callback framework. The complete implemented
native contract, ownership rules, protocol inventory, backpressure behavior,
widget binding, and qualification are in
[two-thread-shared-node-graph.md](two-thread-shared-node-graph.md).

Concrete interaction and rendering behavior is specified in
[ui-behavior.md](ui-behavior.md).

## Browser application

The browser frontend connects to `codex-bridge` over WebSocket and has a
separate TypeScript implementation appropriate to that process boundary. Its
architecture, parity boundary, and limitations are documented in
[web-1.0-contract.md](web-1.0-contract.md). The native node graph is not a new
service or public protocol and is not shared with the browser.

## Local Git Changes

The existing `GitDiffProvider` performs scoped local libgit2 work separately
from the app-server/UI data path. It does not access the shared graph or add an
authority for protocol state.
