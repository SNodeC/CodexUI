# CodexUI codex2 Architecture

## 1. Purpose

CodexUI is a remote frontend for `codex-bridge`. It uses the AISuite
`ai::openai::codex2` frontend proxy SDK and presents Codex app-server behavior
without introducing another backend, protocol authority, or retained semantic
store.

The architecture has three explicit boundaries:

```text
Qt presentation
    <-> normalized UI command/event protocol
SNode.C client runtime + codex2 frontend proxy SDK
    <-> slim codex-bridge envelope over a selected SNode.C transport
codex-bridge
    <-> native Codex app-server JSON-RPC
Codex app-server
```

The Codex app-server remains authoritative for Codex account, configuration,
model, thread, turn, item, plan, tool, approval, and persistence semantics.
`codex-bridge` adds multi-client routing and telemetry. CodexUI adds only
client-local interaction and presentation state.

## 2. Runtime Object Graph

CodexUI has two main operating-system threads. A Codex conversation thread is
a protocol object and is unrelated to these execution threads.

```text
                         CodexUI process

        Qt GUI thread                         SNode.C client thread
   +------------------------+             +---------------------------+
   | Qt application loop    |             | SNode.C event loop        |
   | widgets                |             | selected client transport |
   | presentation model     |             | ClientConnection          |
   | normalized UI events   |             | frontend proxy SDK        |
   | user interaction       |             | protocol normalizer       |
   +-----------+------------+             +-------------+-------------+
               |                                            |
               | bounded full-duplex Unix socketpair        |
               +--------------------------------------------+
                                                            |
                                                            v
                                                     codex-bridge
                                                            |
                                                            v
                                                     Codex app-server
```

The SNode.C side has the same principal application shape as
`codex-bridge-client`. The socketpair gateway replaces that application's
interactive stdin parser and terminal presenter:

```text
Qt command gateway
    -> ClientSession-equivalent dispatcher
    -> ai::openai::codex2::frontend::CodexBridge
    -> frontend::client::ClientConnection
    -> exactly one enabled SNode.C client transport
```

All objects have explicit application ownership. Socket contexts,
subprotocols, and factories borrow the SDK/mediator they need. No singleton is
required.

## 3. Thread Ownership

### 3.1 Qt GUI Thread

The Qt thread exclusively owns:

- `QApplication`, the Qt event loop, and all GUI objects;
- selected thread, selected tab, scroll, expansion, draft, and focus state;
- the presentation model derived from normalized UI events;
- rendering and user-action translation;
- correlation of normalized UI operation results with UI intents.

Only the Qt thread may mutate Qt objects or presentation state. It performs no
bridge transport, app-server framing, JSON-RPC correlation, or typed app-server
decoding.

### 3.2 SNode.C Client Thread

The SNode.C thread exclusively owns:

- the SNode.C event loop;
- the selected frontend transport and its connection lifecycle;
- `ai::openai::codex2::frontend::CodexBridge`;
- `frontend::client::ClientConnection` and transport adapters;
- frontend SDK method execution and callbacks;
- bridge-envelope and native app-server message classification;
- app-server JSON-RPC request/response/server-request correlation;
- typed protocol decoding and normalization into bounded UI events;
- bridge connection, role, and diagnostic telemetry.

The SNode.C thread never accesses widgets or Qt presentation objects.

Conversation discovery remains latency-sensitive. On connection CodexUI asks
only for the thread list; selecting a thread can therefore issue its
`thread/read` without waiting behind unrelated catalog traffic. The complete
account, configuration, model, permission, skill, hook, plugin, app, and MCP
catalog set is queried lazily when its presentation surface is opened. These
are fresh app-server requests, not a CodexUI or bridge cache.

The shared provider handshake is owned by `codex-bridge`, not by any frontend.
Its `initialize` request advertises `experimentalApi: true`, making the complete
generated experimental feature types and typed list/enablement operations
available through the frontend proxy SDK. CodexUI does not perform a second
provider initialization.

## 4. Inter-Thread Socketpair

One unnamed full-duplex Unix socketpair is the only cross-thread transport:

```text
Qt endpoint: commands ->, events <-
        AF_UNIX SOCK_STREAM socketpair
SNode.C endpoint: commands <-, events ->
```

The implementation uses:

- `AF_UNIX`, `SOCK_STREAM`, `SOCK_NONBLOCK`, and `SOCK_CLOEXEC`;
- one endpoint registered with Qt through `QSocketNotifier`;
- one endpoint registered with the SNode.C descriptor event system;
- bounded JSONL frames in both directions;
- bounded socket and application write queues;
- exclusive endpoint ownership and deterministic close behavior.

The socket buffers are both the bounded queues and the readiness mechanism. No
parallel in-memory queue, condition variable, eventfd, or other wakeup
descriptor is added.

The local `SocketPair` follows the ownership shape of SNode.C's
`core::pipe::Pipe`: movable, noncopyable, error-reporting, and responsible for
closing descriptors it still owns. Its endpoint adapters contain no CodexUI
presentation policy so the primitive can move into SNode.C later.

Named pipes/FIFOs are not used. They add names, filesystem cleanup, directional
composition, and discovery semantics that two threads in one process do not
need. Socketpair overhead is immaterial for the expected control/event volume.

## 5. CodexUI Presentation Protocol v1

### 5.1 Boundary and Reuse

The Codex app-server protocol terminates in the SNode.C thread. Every normal
socketpair message uses the presentation protocol identified by:

```json
{"protocol":"codexui.presentation","version":1}
```

No bridge envelope, JSON-RPC envelope, native app-server method name, Qt object
name, widget pointer, or widget identifier is part of the normal contract. Qt
does not parse app-server methods or correlate app-server JSON-RPC IDs.

The protocol is transport-neutral JSON. A stream transport carries one bounded
JSON object per JSONL line. A browser WebSocket carries the same object in one
text message. Browser and Qt consumers therefore share the same reducer and
event semantics without sharing Qt classes or the internal socketpair.

### 5.2 Frame Grammar

Exactly three frame kinds cross the socketpair:

| `kind` | Direction | Purpose |
| --- | --- | --- |
| `command` | UI to SNode.C | Asynchronous user or lifecycle intent |
| `result` | SNode.C to UI | One terminal result for a correlated command |
| `event` | SNode.C to UI | Unsolicited presentation-state or diagnostic update |

All frames contain `protocol`, `version`, and `kind`. A command contains
`action` and `data`; commands expecting a result also contain
`correlationId`. A result contains `action`, `correlationId`, `ok`, and either
`data` or `error`. An event contains `type` and `data`.

Every SNode.C-to-UI frame contains:

- `sequence`: process-local, monotonically increasing output sequence;
- `generation`: bridge connection generation;
- `authority`: `none`, `merge`, `replace`, or `remove`;
- optional `scope`: stable `threadId`, `turnId`, `itemId`, `requestId`, or
  `processId` identities represented by the frame.

`correlationId` identifies one asynchronous command/result exchange. It never
identifies a widget or a presentation entity. Widgets are reached indirectly
through the reducer using stable IDs in `scope` and domain data.

Sequence zero is reserved for a Qt-local diagnostic that did not cross the
socketpair. Such a diagnostic has no state authority.

### 5.3 Authority

Authority has one meaning across all domains:

- `none`: telemetry, notice, or diagnostics; no retained-domain authority;
- `merge`: update only represented fields and preserve omitted fields;
- `replace`: replace exactly the represented scope and collection completeness;
- `remove`: remove exactly the stable scope identified by the frame.

An omitted field is unchanged. It is never an implicit deletion. Empty data is
authoritative only when accompanied by `replace` or `remove` for an explicit
scope. Unknown event types and diagnostics never mutate retained conversation
state.

### 5.4 UI-to-SNode.C Commands

The v1 command catalog used by the application is:

| Action | Result | Meaning |
| --- | --- | --- |
| `runtime.shutdown` | no | Orderly SNode.C runtime shutdown |
| `connection.reconnect` | no | Explicit bridge transport reconnect |
| `controller.claim` | no | Request controller ownership |
| `controller.release` | no | Release controller ownership |
| `threads.list` | yes | Discover threads without deletion authority |
| `thread.read` | yes | Read one thread with full turns where available |
| `thread.create` | yes | Start a thread |
| `thread.resume` | yes | Resume a thread through app-server semantics |
| `thread.fork` | yes | Fork a thread through app-server semantics |
| `thread.rename` | yes | Set a thread name |
| `thread.archive` | yes | Archive a thread |
| `thread.unarchive` | yes | Unarchive a thread |
| `thread.delete` | yes | Delete a thread |
| `models.list` | yes | Read the available model catalog |
| `turn.start` | yes | Start a turn in an idle thread |
| `turn.steer` | yes | Steer the identified active turn |
| `turn.interrupt` | yes | Interrupt the identified active turn |
| `pending-request.resolve` | no | Send typed result/error for a server request |
| `diagnostic.raw.send` | no | Explicit development-only native JSON path |

The implemented typed action catalog additionally covers:

- thread goals, metadata, sections, compaction, rollback, shell commands,
  guardian decisions, item injection, loaded-thread discovery, and unsubscribe;
- reviews and experimental-feature listing and enablement;
- account read, login, login cancellation, logout, rate limits, token usage,
  reset-credit consumption, credit nudges, and workspace messages;
- configuration read, requirements read, single-value write, and batch write;
- model-provider capabilities and permission-profile discovery;
- skills, hooks, marketplaces, plugins, plugin sharing, and apps;
- MCP status, refresh, OAuth login, resource reads, and tool calls;
- filesystem reads, writes, metadata, directory operations, copy/remove, and
  watch management;
- one-off command execution, stdin writes, resize, and termination;
- external-agent configuration discovery/import/history, fuzzy file search,
  feedback upload, and Windows sandbox setup/readiness.

Every action is dispatched through its generated AISuite codex2 operation type.
Qt sends semantic presentation action names and typed `data`; native app-server
method names do not cross the regular socketpair contract. `initialize` and
`initialized` are deliberately absent because the bridge owns the one shared
provider handshake.

Commands are asynchronous. No Qt call blocks waiting for SNode.C. Unsupported
correlated actions receive one `result` with `ok:false` and a structured error.

### 5.5 SNode.C-to-UI Results

Results preserve their originating `action` and `correlationId`. The currently
reduced result payloads are:

- `threads.list`: `threads`, `nextCursor`, and `backwardsCursor`, with `merge`;
- `thread.read`: complete returned `thread`, with `replace` scoped to that
  thread;
- `thread.create`, `thread.resume`, and `thread.fork`: returned `thread`, with
  `merge`;
- `thread.rename`, `thread.archive`, `thread.unarchive`, and `thread.delete`:
  terminal operation status; their app-server notifications carry state
  authority;
- `models.list`: `models` and `nextCursor`, with `replace`;
- `turn.start`: returned `turn`, with `merge` scoped to its thread;
- all other successful actions: typed result data with `none` until a reducer
  explicitly declares a presentation scope.

A failed result contains a structured `error` and has no state authority.

### 5.6 Event Vocabulary

The core retained-state events are:

- `thread.upsert`, `thread.name.changed`, `thread.status.changed`,
  `thread.lifecycle`, and `thread.removed`;
- `turn.upsert`, `turn.diff.changed`, `turn.moderation.changed`, and
  `plan.replaced`;
- `conversation.item.upsert`, `conversation.item.append`,
  `conversation.command.interaction`, `conversation.file-change.output-appended`,
  `conversation.file-change.patch-replaced`, `conversation.mcp.progress`, and
  `conversation.reasoning.part-added`;
- `agents.activity.upsert`;
- `pending-request.upsert` and `pending-request.removed`.

Connection and operational events are:

- `connection.lifecycle`, `connection.bridge`, `connection.controller`, and
  `connection.remote-control.changed`;
- `terminal.command.output-appended`, `terminal.process.output-appended`, and
  `terminal.process.completed`;
- `activity.hook.started` and `activity.hook.completed`;
- `approval.review.started`, `approval.review.completed`, and
  `approval.strict-review.required`.

Catalog, account, settings, and workspace events are:

- `account.changed`, `account.rate-limits.changed`, and
  `account.login.completed`;
- `catalog.skills.invalidated` and `catalog.apps.changed`;
- `integration.mcp.login-completed`, `integration.mcp.status-changed`, and
  `integration.mcp.event`;
- `workspace.project.changed`, `workspace.files.changed`,
  `workspace.search.changed`, and `workspace.search.completed`;
- `settings.external-agent-import.progress` and
  `settings.external-agent-import.completed`;
- thread goal, queue, project, environment, settings, token-usage, compacted,
  and reverted events under the `thread.*` namespace;
- model reroute, verification, and safety-buffering events under `model.*`.

Realtime and platform events are normalized under `realtime.*` and `system.*`.
Warnings and errors use `notice.added`. Unknown or malformed input uses
`system.diagnostic`. Every generated app-server notification is either mapped
to one of these semantic event types or produces a diagnostic-only event; it is
never forwarded as generic presentation state.

### 5.7 Pending Requests

All app-server server-request families normalize to
`pending-request.upsert`. Its data contains the native stable request ID, a
presentation category, and typed request data. Categories are:

- `command-approval`, `file-change-approval`, `user-input`,
  `mcp-elicitation`, and `permissions-approval`;
- `dynamic-tool-call`, `authentication-refresh`, and `attestation`;
- `legacy-patch-approval` and `legacy-command-approval`.

Resolution uses `pending-request.resolve` in the other direction and
`pending-request.removed` when authoritative resolution is observed. Secret
request content is not copied into diagnostics.

### 5.8 Raw JSON and Compatibility

The codex2 SDK preserves complete native app-server JSON and unknown fields.
Raw JSON remains available only through the explicit bounded
`diagnostic.raw.send` development action and SNode.C-side diagnostics. It is
not normal UI state, deletion authority, or an escape from typed normalization.

Consumers reject an unsupported protocol name or major version. They ignore
unknown semantic event types without deleting state. New optional fields,
actions, and event types are backward-compatible within version 1 when old
consumers can safely ignore them. Any change to frame meaning, authority, or
identity requires a new major version.

## 6. Presentation Authority and Reduction

Qt owns a transient presentation model so widgets can be rendered efficiently.
That model is not a semantic cache, persistence layer, or substitute for
app-server history.

Presentation reduction follows these rules:

1. Stable `threadId`, `turnId`, `itemId`, agent-thread ID, and request ID define
   identity; row position never defines identity.
2. Incremental events merge only fields they represent.
3. Deltas append to the identified field of the identified item.
4. A richer completed item is not degraded by a later partial item view.
5. Authoritative replacement is honored only when the normalized event marks
   the represented scope and completeness explicitly.
6. Explicit removals remove exactly their identified scope.
7. Unknown, malformed, stale-generation, or diagnostic-only events do not
   mutate retained presentation content.
8. Thread/turn completion does not itself remove completed activity.

This prevents an incomplete publication from acquiring accidental deletion
authority while preserving the app-server's explicit authority.

## 7. Thread Selection and Interaction

The selected Codex thread is user-owned UI state.

- A thread created or updated by another frontend does not change selection.
- Incoming activity in a parallel thread does not change selection.
- Controller changes, reconnects, list refreshes, and read completions do not
  change selection merely because another thread is newer.
- User selection changes the selected thread.
- A user-initiated local new-thread action may select its returned thread as
  part of that same explicit intent.
- An explicitly removed selected thread may clear selection.

There is no automatic switch to the newest, active, or newly created thread.

For an idle selected thread, submitting a prompt starts a new turn. For an
active selected turn, a steering action uses the app-server steering operation
rather than fabricating another local turn. Interrupt targets the stable active
turn ID.

Switching threads or inspector tabs while turns, plans, commands, agents, or
requests are changing must not stop, reset, or reorder those lifecycles.

## 8. Plans and Agents

### 8.1 Plans

`turn/plan/updated` is the canonical structured plan update. A normalized plan
replacement carries the thread ID, turn ID, optional explanation, and ordered
steps with `pending`, `inProgress`, or `completed` status.

Plan presentation is retained across tab and thread switching. It changes only
for the identified turn and is cleared only by an explicit authoritative empty
or replacement event for that turn. A completed textual plan item may be shown
as conversation activity, but it does not override a newer authoritative
structured turn plan.

### 8.2 Agents

Agent presentation is derived from typed collaboration data, especially
`collabAgentToolCall` and `subAgentActivity` items. It retains, when supplied:

- tool operation and stable item ID;
- sender thread ID;
- receiver/agent thread IDs;
- prompt;
- requested model and reasoning effort;
- current tool-call status;
- last known per-agent state and path/activity details.

Completion must not collapse this information into only a generic
"Subagent activity completed" row. Completed and failed agent activity remains
inspectable as part of its owning turn. Later partial events may update status
without erasing richer agent identity or prompt data.

App-server may publish a parent `subAgentActivity(kind=started)` and later
complete the child thread without replacing the parent item with a completed
variant. CodexUI correlates those authoritative records by `agentThreadId` and
projects child turn status and retained child result into the original parent
activity. This is transient presentation correlation, not backend state.
Identified subagent implementation threads remain addressable for correlation
but are omitted from the ordinary top-level thread list.

The Agents view follows the currently selected thread; it never selects an
agent thread or parent thread automatically.

## 9. Pending Requests and Attention State

App-server-initiated requests are normalized into explicit pending-request
events using the native stable JSON-RPC request ID and associated thread ID.
Supported request families include approvals, user input, MCP elicitation,
permission approval, dynamic tool calls, and other generated server-request
types.

The UI attention/brown state is derived only from currently unresolved pending
requests associated with that thread. It is not inferred from historical item
status or retained across process restart without fresh provider evidence.

A pending request is retired exactly once when:

- its typed response/error is accepted and the corresponding resolution is
  observed; or
- `serverRequest/resolved` identifies that same request; or
- the owning connection/generation terminates and the request can no longer be
  answered by this frontend.

Resolution matching uses stable request identity plus available thread and
connection generation context. The presentation request record retains that
generation. A mismatch is diagnostic and must not retire an
unrelated request. Resolved request content is removed from actionable UI while
non-secret lifecycle diagnostics may remain observable.

## 10. Controller and Observer Roles

The bridge permits one controller and multiple observers.

- The controller may mutate Codex state, steer turns, and answer server
  requests.
- Observers receive fanout events and may use bridge-approved read operations.
- Mutating observer operations fail visibly rather than appearing accepted.
- Controller claim and release are explicit.
- No frontend silently steals control.
- A disconnected controller is not replaced by automatic promotion.
- Thread selection is independent of controller ownership.

CodexUI displays connection identity and role. Controls requiring authority are
disabled or produce a precise role error while CodexUI is an observer. A local
policy may request initial control explicitly, but role assignment remains a
bridge decision reported through telemetry.

## 11. Recovery, History, and No-Cache Policy

CodexUI does not request or reconstruct an AISuite-owned snapshot because
codex2 has no snapshot authority, replay store, frontend `State`, or backend
semantic cache.

Connection and process recovery uses fresh app-server queries through the
bridge:

1. establish the frontend transport and observe bridge readiness/role;
2. issue `thread/list` for discovery;
3. issue `thread/read(includeTurns=true)` for the selected materialized thread;
4. continue applying normalized live events.

A refresh result replaces only the scope it explicitly represents. Temporary
disconnect, incomplete discovery, request failure, or an unknown message does
not authorize clearing the existing presentation.

Current app-server behavior may return `itemsView: "notLoaded"`, reject
`includeTurns` for an unmaterialized thread, or reconstruct less live detail
than was previously emitted under its active history mode. CodexUI reports
that provider limitation; it does not invent missing items or add an implicit
long-term history cache. Adding caching later requires a separate explicit
architecture decision covering authority, bounds, persistence, and eviction.

## 12. External Transport and Configuration

The socketpair is internal only. The SNode.C thread connects to `codex-bridge`
through exactly one configured frontend transport supported by codex2 and
SNode.C:

- Unix stream;
- IPv4 or IPv6 stream;
- IPv4 or IPv6 TLS stream;
- IPv4 or IPv6 WebSocket;
- IPv4 or IPv6 WSS;
- RFCOMM or RFCOMM TLS where available.

Transport and encryption do not change normalized UI semantics. WebSocket
changes framing; TLS changes transport protection. Neither creates state,
authority, or authentication semantics.

There is no bearer-token or other codex-bridge authentication layer. Native
Codex account/login operations remain app-server protocol features and are
handled through typed SDK operations when exposed by the UI.

Command-line configuration uses the SNode.C configuration subsystem. Any
CodexUI-specific configuration class is a `utils::SubCommand`. Existing SNode.C
instance options remain authoritative for addresses, Unix paths, IPv4/IPv6,
TLS certificates, WebSocket setup, reconnect behavior, timeouts, and queue
limits; CodexUI must not duplicate those semantics.

Quiet Codex sessions are normal, so transport inactivity read/write timeouts
default to zero (unlimited). Frame bounds, write-queue bounds, connect errors,
and explicit lifecycle controls remain enforced.

## 13. Startup and Shutdown

The process lifecycle is:

```text
QApplication construction and Qt argument handling
    -> core::SNodeC::init(argc, argv)
    -> construct socketpair and both ownership graphs
    -> start the SNode.C client thread
    -> core::SNodeC::start() inside that thread
    -> run the Qt event loop on the main thread
    -> request inner transport shutdown
    -> core::SNodeC::stop()
    -> close socketpair endpoints and join the client thread
```

There is no `core::SNodeC::free()` call. Shutdown is asynchronous and
idempotent. Qt does not destroy objects still used by the client thread, and
the process does not exit while the SNode.C thread is still running.

EOF or terminal failure on either socketpair endpoint initiates orderly
shutdown. External bridge disconnect does not terminate CodexUI; it produces a
normalized disconnected state and follows configured reconnect policy.

## 14. Boundedness and Failure Semantics

Every boundary is bounded:

- bridge transport frame size;
- socketpair frame size;
- socketpair and transport write queues;
- bytes processed per readiness callback;
- retained diagnostics;
- UI presentation work scheduled per event-loop pass.

No operation may block the Qt event loop or wait synchronously across threads.
Backpressure, oversized frames, malformed JSON, queue rejection, transport
closure, and callback failure produce classified diagnostics. They are not
silently converted into generic disconnects or state deletion.

Outstanding normalized UI operations complete once with success or a concrete
failure. A disconnect clears ephemeral request correlation and role telemetry,
not Codex presentation content. Reconnect starts a new connection generation so
late results from an old generation cannot resolve new operations or pending
requests.

## 15. Protocol Compatibility

AISuite codex2 generates concrete C++ datatypes for the complete exported Codex
app-server protocol, including client requests, client notifications, server
requests, server notifications, responses, errors, nested objects, enums, and
unions. Every generated value preserves its native JSON through `getRaw()` and
preserves unknown fields.

CodexUI uses those generated types and typed callbacks in the SNode.C thread.
Every generated server notification and request is classified into a v1
presentation event or a diagnostic-only event. No known message silently falls
through as generic state. Unknown future messages remain observable through
bounded diagnostics and cannot mutate presentation state. The app-server
source/schema checkout is read-only and is never modified by CodexUI.

The app-server wire is JSON-RPC-shaped but may omit the optional
`"jsonrpc": "2.0"` member. The frontend SDK owns that compatibility; Qt never
depends on the member's presence.

## 16. Permanent Development Harness

The codex2 workbench is retained as a deliberately plain development harness
alongside the final visual UI/UX shell. Both consumers use the same
`codexui.presentation` contract and `PresentationModel`; the harness does not
receive native app-server JSON through a privileged path.

The harness provides:

- explicit thread selection with no automatic selection changes;
- conversation, plan, correlated agent-activity, and generation-aware
  pending-request inspection;
- read-only State inspection for model, account, configuration, permission,
  feature, skills, hooks, plugin, app, MCP, and other retained domains;
- controller/observer role and connection-generation visibility;
- a bounded protocol log containing timestamp, sequence, generation, frame
  kind, action/event type, authority, stable scope IDs, correlation ID, and
  result status;
- explicit sequence-gap and non-monotonic-sequence diagnostics;
- bounded model counters for discovered threads and selected-thread turns and
  items.

The protocol log retains at most 2,000 display records. It is telemetry only:
it cannot replay frames, hydrate the presentation model, supply deletion
authority, or conceal a missing app-server result. `thread/list` discovery
preserves the order supplied by app-server for listed IDs, while IDs omitted
from a non-authoritative discovery page remain retained after that page.

The presentation model separately retains at most 256 authority-free telemetry
records for status and warning presentation. These records cannot mutate or
hydrate conversation state. Authoritative normalized domains not requiring a
specialized reducer remain accessible at their stable global, thread, turn, or
item scope; their `merge`, `replace`, and `remove` semantics are applied before
the visual shell consumes them.

The harness is used first when extending normalization or reduction. Once a
path is proven there, the existing externally designed visual shell consumes
the same model and command surface through narrow Qt adapters. The visual shell
must not reintroduce the legacy frontend State/snapshot architecture.

## 17. Architectural Invariants

1. The app-server is Codex semantic and persistence authority.
2. `codex-bridge` is a thin multi-client router with telemetry, not a cache.
3. The codex2 frontend SDK is a typed proxy, not a frontend state store.
4. SNode.C owns transport, SDK execution, protocol decoding, and normalization.
5. Qt owns widgets, interaction, selection, and transient presentation state.
6. Only normalized commands/events form the regular inter-thread contract.
7. Cross-thread work is asynchronous and bounded.
8. Partial omission is not deletion authority.
9. Stable protocol IDs, never row order, define identity.
10. Controller transfer and thread selection are explicit; neither auto-switches.
11. Plans, completed commands, and completed agent activity remain visible until
    an authoritative scoped update says otherwise.
12. Pending attention exists only while a matching server request is unresolved.
13. Recovery queries app-server; no snapshot, replay store, or semantic cache is
    introduced.
14. Generic Qt and SNode.C socket classes remain free of Codex-specific methods.
15. Native app-server and bridge transport failures remain distinguishable.

## 18. Remaining Narrow Decisions

Only presentation-level choices remain open:

- event batching needed to keep large delta streams responsive;
- the bounded diagnostic retention size;
- the exact user-visible presentation of individual typed errors and unknown
  protocol events.

These choices may refine boundedness and presentation but must not extend the
authority, caching, threading, controller, transport, or protocol boundaries
defined above.
