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
app-server remains authoritative for provider facts. This data path has no
mirror, snapshot history, journal, presentation model, view-state model,
projector, serialized internal protocol, or third execution context. The
pre-existing `GitDiffProvider` may use Qt's global thread pool for local
libgit2 work; it neither consumes app-server traffic nor reads or writes the
node graph and is outside this deliberately narrow data path.

The framework-neutral implementation lives only in `src/codex/nodegraph/`, is
named `codexui-nodegraph`, and uses namespace `codexui::nodegraph`. Its
standalone headless tests live in `tests/codex/nodegraph/`. It has no dependency
on Qt, SNode.C, sockets, a running app-server, wall-clock timing, or any legacy
CodexUI presentation class.

The independent gate is available without configuring the application:

```sh
cmake -S . -B build-nodegraph -DCODEXUI_NODEGRAPH_ONLY=ON
cmake --build build-nodegraph
ctest --test-dir build-nodegraph --output-on-failure
```

## Nodes and current state

Every node has:

- a unique graph `NodeId` and concrete `NodeKind`;
- one immutable current `NodeState` storage object;
- ordered parent/child and directly derived cross-entity relations;
- a changed revision and removed marker;
- one opaque, non-owning UI attachment slot.

Globally unique protocol entities, such as threads, use their canonical wire
IDs directly. Protocol turn IDs are scoped by thread and item IDs are scoped
by turn in the graph index; each scoped node retains its raw canonical wire ID
as `protocolId`. Process and watch identities also include the current
connection generation. Outbound calls recover the raw IDs at the protocol
boundary, while internal relations use stable `NodeRef`s. State and ordering
live on the nodes; no render commit or second domain model is introduced.
Unknown methods and tagged-union alternatives are retained in unknown
nodes/current fields without changing known state.

Nodes are held by `std::shared_ptr<Node>`. The graph, queued notifications,
materialized widgets, and active reads therefore pin lifetime. Relations may
be non-owning while protected by graph synchronization. Removal unlinks a node
and erases its indexes under the write lock, marks it removed, and includes a
stable `NodeRef` in the direct Qt notification. If that notification coalesces,
the graph's retired-node set remains the lifetime source and Qt collects it in
bounded 64-node rescan slices. Qt clears the attachment and destroys the
QWidget on Qt-main, then acknowledges detachment through the typed action
queue; final node destruction waits for the graph retirement pin and every
other `NodeRef` to be released.

Only Qt-main sets, clears, or dereferences the opaque attachment. The native Qt
adapter may place a `QPointer<QWidget>`, last rendered revision, and viewport
materialization state behind it. The worker never inspects it, and no permanent
NodeId-to-widget registry is allowed.

## Synchronization and atomic updates

One graph reader/writer lock protects state, indexes, relations, order,
revisions, and lifetime transitions. The SNode.C worker is the sole writer.
For each decoded message it prepares values before locking, takes one write
access, and applies every related node/index/relation change. A transaction
that changes graph state increments the graph revision exactly once, unlocks,
and only then queues a notification. An idempotent update or an explicitly
state-neutral message is a no-op: it does not advance either graph or node
revisions and does not queue `GraphChanged`.

Qt uses try-read acquisition only. Failure schedules another Qt event-loop
pass; Qt never waits for the writer. A successful read either briefly pins a
node's immutable current storage or extracts only values needed for the visible
render. The access is released before any QWidget call. No callback executes
while a graph or queue lock is held.

This gives Qt either the state before a decoded message or the complete state
after it, never an intermediate graph.

## Protocol contract

The implementation's checked, closed inventory is `ProtocolCatalog.cpp`. Its
method names were verified against the app-server registry at Codex
`305eed102d6ab5fc1228fec0737ba240eb29826b`, including compatibility and
internal methods omitted by public schema generation. The accompanying
`docs/app-server-protocol/master-data-model.md` is historical protocol research,
not the design of this runtime. The complete surface is:

| Direction | Methods |
| --- | ---: |
| client requests | 157 |
| server requests | 11 |
| server notifications | 83 |
| client notifications | 1 (`initialized`) |

Those 252 methods have these exact dispositions:

| Disposition | Methods |
| --- | ---: |
| worker operation/result | 158 |
| reverse interaction | 11 |
| graph update | 75 |
| typed UI effect | 6 |
| intentionally state-neutral | 2 |

The six UI-effect notifications are `error`, `warning`, `guardianWarning`,
`deprecationNotice`, `configWarning`, and `windows/worldWritableWarning`. The
two deliberate no-ops are `rawResponseItem/completed` and
`rawResponse/completed`; their presence is recognized without creating a
second raw-response authority.

Tests assert direction and disposition counts, uniqueness, lookup, and
handling of every entry. The installed AISuite generated macros contribute
95 client requests, 10 server requests, 76 server notifications, and the one
client notification. The verified newer set contributes 62 more client
requests (one supplied through a local typed compatibility adapter); local
typed adapters also supply one server request and seven server notifications.
Those named unions equal 157/11/83/1 without double-counting the adapted client
request. A changed generated binding set must be reconciled explicitly with
the verified catalog.

CodexBridge performs native app-server JSON decode and encode. Its typed
frontend callbacks provide a decoded message containing method, direction,
request/correlation identity, and owned current payload values to a plain
dispatcher. The dispatcher finds or creates addressed nodes, updates fields
and directly affected relations/statuses, removes nodes when required, and
commits at most one revision. It is explicit application logic, not a generic
reducer, rule engine, dependency graph, callback registry, or second raw-JSON
parser.

One CodexBridge raw-message hook has a deliberately narrow completeness role.
It observes the outbound `initialized` notification and retains inbound
methods absent from the closed catalog as unknown protocol nodes. It returns
immediately for every known inbound method, whose registered typed callback is
the sole update path. Raw app-server JSON never reaches Qt-main.

Responses are correlated on the worker because response envelopes do not carry
their method. Server requests become pending interaction nodes and are removed
only on an accepted response or `serverRequest/resolved`. Known state-neutral
messages still pass through the exhaustive dispatcher and are tested. Unknown
messages are retained separately and cannot mutate an addressed known node.
Targeted operations also retain the expected stable `NodeRef` and connection
and provider generations. Late or mismatched results cannot update a
replacement node. `thread/read` additionally records the target revision at
dispatch: a stale result may add previously unseen historical nodes, but it
cannot overwrite newer thread fields or newer state on already-known turns and
items.

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

`NodeAction` carries a stable target `NodeRef`, a concrete action kind, and only
newly authored owned data. The worker re-reads all existing target state before
acting. `RuntimeAction` covers connection, controller, catalog, refresh, and
explicit new-thread work that has no existing target node. Pure widget gestures
such as fold, copy, focus, and scroll remain on Qt-main and never enter either
mailbox.

The worker-to-Qt queue has 512 slots. Ordinary graph changes and notices stop
at 510; one slot remains available for a critical selection effect and the
final slot for `WorkerStopped`. A graph transaction containing more than 64
direct node references also uses rescan instead of creating an unbounded
notification. Worker-to-Qt saturation occurs only after graph state is
committed. It records the newest revision in an explicit rescan-required
condition and wakes Qt, so current state remains discoverable. Sequenced
selection and notice effects also retain their latest value in graph state if
direct delivery is full; Qt ignores any older queued effect after reconstructing
that value.

The Qt-to-worker queue has 256 slots. Ordinary actions stop at 255 so shutdown
retains the final slot. A user action is admitted exactly once or rejected
visibly without moving its authored payload out of Qt. Non-idempotent actions
are never retried automatically. Large prompt text and attachments move into
an admitted command rather than being copied. A wake failure after admission
is reported as admitted and non-retryable, preventing a duplicate send.

Thread deletion and provider-generation reset do not discard an admitted
prompt or turn it into an automatic resend. The worker reparents its stable
local prompt node to explicit recovery state in the same graph transaction,
retaining the authored text and attachment links while marking a definite or
uncertain outcome. A late result cannot revive that operation. Qt presents the
retained recovery input, and only a new deliberate user action may submit it.

Qt observes the worker eventfd with `QSocketNotifier`. SNode.C observes the Qt
eventfd with its native descriptor mechanism. The prior socketpair/JSONL path
is removed after cutover and is not retained as a fallback.

## Widget and UX compatibility contract

Existing native widgets and styling remain the renderer. The node's one opaque
attachment is the association for a materialized widget; there is no permanent
parallel NodeId-to-widget registry. Conversation cards materialize within a
one-viewport overscan, remain retained within a two-viewport margin, and render
current state only while in the viewport or overscan. Farther cards become
measured placeholders. Thread rows use two rows of overscan. Plan, Agents, and
Requests materialize at most 48 Inspector rows plus a two-row overscan, with
fixed-height spacers representing the rest. Entering any window reads and
renders the latest node revision; nodes beyond the overscan perform no widget
update. Conversation work is limited to eight card operations and 64 visibility
checks per event-loop pass, thread graph work to 64 units and 32 row operations,
and Inspector work to 64 graph reads and 12 widget changes.
Stable keyed identity, preserved heights, and scroll anchoring keep the main
loop responsive. Focus, animation, folding, filters, drafts, editor mechanics,
and scroll-following remain genuinely local QWidget state.

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

Native tests in `tests/codex/` and the WebUI parity suite are behavioral
oracles. They may be extended but not weakened.

## Implemented cutover and qualification

`ClientRuntime` now hosts `WorkerLogic` and `ProtocolUpdater` beside
CodexBridge on the existing SNode.C worker. Every known inbound callback
applies a decoded typed payload to the graph. Provider-facing widget actions
enter the one typed Qt-to-worker queue. Each concrete outbound operation is
revalidated against current connection, controller, generation, target, and
active-turn state before its one direct CodexBridge call; compound workflows
such as creation followed by the first prompt remain ordered distinct
operations. The same mailbox carries the two concrete local lifecycle actions:
prompt-materialization acknowledgement and removed-widget detachment; they
update graph lifetime state without a bridge call. The old presentation
authority, JSONL framing, socketpair endpoints, and temporary comparison path
have been deleted; no outbound operation is dual-sent.

Thread hydration readiness is current graph state keyed to provider
generation. Failed or unresolved hydration rejects admission before moving the
user's draft. Local prompts retain exact admitted text plus safe ordinary-file
Markdown, use request-result acknowledgement independent of item correlation,
and preserve stable card/thread-row identity through canonical promotion.
An empty new-thread draft is discarded when the user selects a real thread;
once its first creation action is admitted, a second New Thread command is
visibly rejected until that exact correlation resolves. Conversation history
reveals retained older nodes before requesting another provider page, and a
paused viewport grows only its effective tail window until following resumes.
Streaming response, reasoning, plan, and command-output fields retain a
UTF-8-aligned 192 KiB newest tail after crossing the 256 KiB threshold and
carry exact omitted-byte metadata that both rendering and copy disclose.
Deleted threads and provider resets reparent affected local prompts to explicit
recovery state; reconnection never resends a non-idempotent operation.
Conversation widgets are materialized only for the viewport plus bounded
overscan, and at most eight card operations run per event-loop pass.

Qualification covers the standalone target/tests, exact source-derived
inventory, graph atomicity, non-blocking read contention, removal lifetime,
queue saturation and stale-effect ordering, eventfd coalescing, moved large
payloads, worker ownership, scoped identity collisions, realtime append/final
semantics, and bounded visible-only rendering. It also includes a Qt heartbeat
while 4,096 distinct inbound items plus 4,096 streaming deltas saturate and
drain the notification queue.

The final clean qualification ran the independently configured nodegraph-only
suite in Debug, AddressSanitizer, and ThreadSanitizer builds (5/5 tests in each,
with no sanitizer findings and no Qt libraries linked), the integrated native
suite (14/14), three consecutive passes of the mailbox, graph-concurrency,
runtime-dispatch, and shell-integration tests, and the WebUI compatibility suite
(83/83).
