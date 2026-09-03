# Two-thread shared node graph

## Scope and baseline

This document is the implementation contract for CodexUI's deliberately narrow
replacement of its native app-server-to-widget data path. It is not a reusable
event-sourcing system, presentation framework, protocol runtime, or callback
architecture.

Work began on `codex/two-thread-shared-node-graph` with a clean worktree. `HEAD`,
that branch, `master`, and `origin/master` all resolved to
`dbcbb1b4d30dc24d96ed06647198a1abc6fa4c3c`. No repository-specific agent
instruction file was present; `README.md`, `docs/codex-architecture.md`,
`docs/ui-behavior.md`, the checked-in tests, and CI are the compatibility
oracles.

The unchanged baseline built successfully. All nine native CTest tests passed
with an isolated writable `XDG_CONFIG_HOME`, and all 83 WebUI tests passed.
Without that isolated settings directory, `codexui-git-changes-live` failed its
persisted-resolution recreation case because the test shared ambient QSettings;
the isolated run records the product baseline without weakening a test.

## One graph and two threads

The native application has exactly two relevant execution threads:

```text
Qt main thread                         existing SNode.C worker thread
------------------------------         ----------------------------------
all QWidget ownership                  SNode.C event loop and transport
viewport and renderer mechanics        CodexBridge decode/encode
short non-blocking graph reads    <-->  sole graph writer
typed user actions                     protocol updates and correlations
QSocketNotifier                        native descriptor receiver
        |                                         |
        +-- bounded SPSC + eventfd each way ------+
```

There is one current in-memory `NodeGraph`, shared by those threads. The
app-server remains authoritative for provider facts. There is no mirror,
snapshot history, journal, presentation model, view-state model, projector,
serialized internal protocol, or third execution context.

The framework-neutral implementation lives only in `src/codex/nodegraph/`, is
named `codexui-nodegraph`, and uses namespace `codexui::nodegraph`. Its
headless tests live only in `tests/codex/nodegraph/`. It has no dependency on
Qt, SNode.C, sockets, a running app-server, wall-clock timing, or any legacy
CodexUI presentation class.

## Nodes and current state

Every node has:

- a canonical protocol-boundary `NodeId` and concrete `NodeKind`;
- one immutable current `NodeState` storage object;
- ordered parent/child and directly derived cross-entity relations;
- a changed revision and removed marker;
- one opaque, non-owning UI attachment slot.

Graph indexes use canonical IDs. Relations use stable in-process node
references where natural. State and ordering live on the nodes; no render
commit or second domain model is introduced. Unknown methods and tagged-union
alternatives are retained in unknown nodes/current fields without changing
known state.

Nodes are held by `std::shared_ptr<Node>`. The graph, queued notifications,
materialized widgets, and active reads therefore pin lifetime. Relations may
be non-owning while protected by graph synchronization. Removal unlinks a node
and erases its indexes under the write lock, marks it removed, and includes a
stable `NodeRef` in the Qt notification. Qt clears the attachment and destroys
the QWidget on Qt-main; final node destruction waits for the last `NodeRef`.

Only Qt-main sets, clears, or dereferences the opaque attachment. The native Qt
adapter may place a `QPointer<QWidget>`, last rendered revision, and viewport
materialization state behind it. The worker never inspects it, and no permanent
NodeId-to-widget registry is allowed.

## Synchronization and atomic updates

One graph reader/writer lock protects state, indexes, relations, order,
revisions, and lifetime transitions. The SNode.C worker is the sole writer.
For each decoded message it prepares values before locking, takes one write
access, applies every related node/index/relation change, increments the graph
revision once, unlocks, and only then queues a notification.

Qt uses try-read acquisition only. Failure schedules another Qt event-loop
pass; Qt never waits for the writer. A successful read either briefly pins a
node's immutable current storage or extracts only values needed for the visible
render. The access is released before any QWidget call. No callback executes
while a graph or queue lock is held.

This gives Qt either the state before a decoded message or the complete state
after it, never an intermediate graph.

## Protocol contract

The authoritative checked-in inventory is
`docs/app-server-protocol/master-data-model.md`, derived from the app-server
method registry at Codex `305eed102d6ab5fc1228fec0737ba240eb29826b` and
including compatibility/internal methods omitted by public schema generation.
The complete surface is:

| Direction | Methods |
| --- | ---: |
| client requests | 157 |
| server requests | 11 |
| server notifications | 83 |
| client notifications | 1 (`initialized`) |

The nodegraph contains a checked, closed inventory of those exact names. Each
entry is explicitly classified as graph update, worker operation/result,
reverse interaction, typed UI effect, or intentionally state-neutral. Tests
assert the exact four counts, uniqueness, lookup, and handling of every entry.
The current verified source wins if generated bindings change.

CodexBridge performs the only native app-server JSON decode and encode. Its
typed frontend callbacks provide a decoded message containing method,
direction, request/correlation identity, and owned current payload values to a
plain dispatcher. The dispatcher finds or creates addressed nodes, updates
fields and directly affected relations/statuses, removes nodes when required,
and commits one revision. It is explicit application logic, not a generic
reducer, rule engine, dependency graph, callback registry, or raw-JSON parser.

Responses are correlated on the worker because response envelopes do not carry
their method. Server requests become pending interaction nodes and are removed
only on an accepted response or `serverRequest/resolved`. Known state-neutral
messages still pass through the exhaustive dispatcher and are tested. Unknown
messages are retained separately and cannot mutate an addressed known node.

## Typed mailboxes and wake-up

Communication consists of exactly two bounded SPSC queues and two Linux
eventfds created with `EFD_NONBLOCK | EFD_CLOEXEC`:

- worker to Qt: `GraphChanged`, `UiEffect`, or `WorkerStopped`;
- Qt to worker: `NodeAction`, `RuntimeAction`, or `ShutdownRequest`.

`GraphChanged` carries the committed graph revision, stable references to
affected and removed nodes, and an explicit rescan-required flag. It never
copies `NodeState`. Eventfds carry only wake counts. A sender pushes one typed
message and writes `uint64_t{1}`; the receiving event loop reads the accumulated
counter and drains its queue.

Worker-to-Qt saturation occurs only after graph state is committed. It records
the newest revision in an explicit rescan-required condition and wakes Qt, so
current state remains discoverable. Qt-to-worker actions are admitted exactly
once or rejected visibly while the user's draft/payload remains owned by Qt.
Non-idempotent actions are never retried automatically. Large prompt text and
attachments move into admitted commands.

Qt observes the worker eventfd with `QSocketNotifier`. SNode.C observes the Qt
eventfd with its native descriptor mechanism. The prior socketpair/JSONL path
is removed after cutover and is not retained as a fallback.

## Widget and UX compatibility contract

Existing native widgets and styling remain the renderer. For each affected
node, Qt renders only when its widget is materialized and viewport-visible.
Entering the viewport reads and renders the latest revision once; leaving it
does no projection work and may release the widget while keeping a measured
placeholder. A small overscan, bounded work per event-loop pass, stable keyed
identity, preserved heights, and scroll anchoring keep the main loop
responsive. Focus, animation, folding, filters, drafts, editor mechanics, and
scroll-following remain genuinely local QWidget state.

The complete visible behavior in `docs/ui-behavior.md` remains required,
including:

- root/child thread hierarchy, ordering, selection, hydration, history paging,
  create, rename, fork, archive, unarchive, delete, and reload;
- stable turn/item/card identity, unknown-item fallback, stream deltas, plans,
  agents, generated images, attachments, Markdown, process/file/MCP details,
  copy, folding, filters, focus, scroll anchoring, and follow-latest behavior;
- exact prompt keyboard rules, per-thread admission queues, drafts,
  attachments, optimistic cards, acknowledgement/error feedback, steering,
  recovery, and no duplicate or dual-send transition;
- command/file approvals, permissions, user-input requests, MCP elicitation and
  tool calls, validation, attention state, and exact reverse responses;
- models, settings, permission profiles, account/rate limits/usage, skills,
  hooks, plugins, apps, MCP catalogs, connection/provider/controller state,
  and notices;
- local Git Changes repository resolution, live refresh, diff presentation,
  Inspector behavior, dialogs, menus, desktop identity, accessibility, and
  progress presentation.

Native tests in `tests/codex/` and the 83 WebUI parity tests are behavioral
oracles. They may be extended but not weakened.

## Cutover and qualification

Integration first hosts graph updates and typed commands beside CodexBridge on
the existing worker. A temporary comparison may feed the old inbound path with
new outbound effects disabled. Visible state, ordering, operations, and pending
interactions must agree before rendering switches. Outbound actions then switch
once; they are never sent down both paths. Temporary comparison adapters,
legacy presentation authority, JSONL framing, and socketpair endpoints are
deleted.

Qualification must demonstrate the standalone target/tests, exact inventory
coverage, graph atomicity, non-blocking read contention, removal lifetime,
queue saturation semantics, eventfd coalescing, moved large payloads, worker
ownership, and bounded visible-only rendering. The full native and browser
suites must pass, including a Qt heartbeat under large inbound traffic. Final
source and documentation must describe only this narrow implemented path.
