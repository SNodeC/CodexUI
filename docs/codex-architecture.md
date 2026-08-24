# CodexUI Architecture

## 1. Purpose

CodexUI is a remote frontend for `codex-bridge`. It uses the AISuite
`ai::openai::codex` frontend proxy SDK and presents Codex app-server behavior
without introducing another backend, protocol authority, or retained semantic
store.

The architecture has three explicit boundaries:

```text
Qt presentation
    <-> normalized UI command/event protocol
SNode.C client runtime + codex frontend proxy SDK
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
    -> ai::openai::codex::frontend::CodexBridge
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
- `ai::openai::codex::frontend::CodexBridge`;
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

Every action is dispatched through its generated AISuite codex operation type.
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

The codex SDK preserves complete native app-server JSON and unknown fields in
its generated C++ values on the SNode.C side. The regular socketpair boundary
carries bounded normalized presentation data, including only the native fields
needed to render and answer a pending request. The request object is retained
transiently until that request is resolved and is never rendered as a raw dump.
Arbitrary raw JSON crosses the boundary only through the explicit bounded
`diagnostic.raw.send` development action. Raw data is not normal UI state,
deletion authority, or an escape from typed normalization.

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

### 7.1 Upcoming-Turn Settings

The real shell has a codex-native upcoming-turn settings surface. It does not
compile or adapt the legacy frontend `State` types. Its primary controls are:

- model and model-constrained reasoning effort;
- sandbox access and the sandbox-native network choice;
- workspace;
- approval policy;
- personality/style.

The compact More menu contains the named permission profile, approval
reviewer, service tier, reasoning summary, and collaboration mode. Model,
effort, service-tier, and permission-profile choices are populated from fresh
app-server catalogs. A named permission profile and a sandbox policy are
mutually exclusive, matching the native app-server contract.

The settings object is a transient draft bound to the stable selected thread
identity. User changes are serialized into native `thread/start` and
`turn/start` fields; untouched fields remain omitted so UI defaults cannot
replace provider state. Collaboration mode is the deliberate exception:
app-server may retain Plan mode without returning it from a later
`thread/read`, so every `turn/start` explicitly sends the Code or Plan mode
currently displayed by CodexUI. The new-thread workspace always has an
explicit local fallback. Settings are disabled while steering because
`turn/steer` does not accept upcoming-turn configuration. No setting is
persisted by CodexUI or treated as canonical before the app-server publishes
it.

## 8. Plans and Agents

### 8.1 Plans

`turn/plan/updated` is the canonical structured plan update. A normalized plan
replacement carries the thread ID, turn ID, optional explanation, and ordered
steps with `pending`, `inProgress`, or `completed` status.

Plan presentation is retained across tab and thread switching. It changes only
for the identified turn and is cleared only by an explicit authoritative empty
or replacement event for that turn. A completed textual plan item may be shown
as conversation activity. When no structured plan survives a fresh
`thread/read`, the Plan inspector renders the newest retained textual plan item
as a read-only compatibility view; it never overrides a newer authoritative
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

Only spawn operations create agent rows. Provisional spawn starts without a
child identity are not independently presented, and `wait`, `sendInput`, and
other collaboration operations update an already identified child rather than
being counted as additional agents. Once supplied, the child thread ID is the
stable presentation identity across spawn completion, child activity, wait,
and result events.

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

The Requests view presents each pending request independently. Command and
file-change approvals use native decision enums, user-input answers preserve
question IDs and support options/free text/secret input, MCP form responses
return structured JSON, and permission approvals preserve the requested
permission object and selected turn/session scope. Dynamic tools unavailable
in CodexUI return a typed failed-tool response. Authentication, attestation,
and unknown capabilities receive an explicit JSON-RPC error rather than
remaining pending indefinitely. Canceling the dialog itself does not resolve
the request.

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
codex has no snapshot authority, replay store, frontend `State`, or backend
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
through exactly one configured frontend transport supported by codex and
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

AISuite codex generates concrete C++ datatypes for the complete exported Codex
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

The codex workbench is retained as a deliberately plain development harness
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

## 17. Implemented Components and APIs

The canonical CodexUI implementation contains both the complete visual shell
and the permanent functional presentation harness. The previous transport,
frontend `State`, snapshot, replay, and SDK sources are preserved on the
dedicated `legacy-codex` Git branch and are not part of this source tree or CI
build. `ExpandingPromptEditor` and the visual style helpers live under
`src/codex/ui` because they contain no protocol authority.

The implementation is divided into the following concrete components:

| Component | Responsibility |
| --- | --- |
| `Configuration` | CodexUI `utils::SubCommand`; adds only CodexUI-specific frame-size and WebSocket-path options |
| `SocketPair` | Movable RAII owner for the unnamed nonblocking `AF_UNIX` socketpair |
| `QtSocketPairEndpoint` | Qt-thread descriptor adapter using `QSocketNotifier`, bounded reads, and bounded writes |
| `SNodeSocketPairEndpoint` | SNode.C-thread descriptor adapter using `ReadEventReceiver` and `WriteEventReceiver`, bounded reads, and bounded writes |
| `FrontendSession` | Qt-side asynchronous command facade, correlation registry, lifecycle owner, and socketpair JSONL endpoint |
| `ClientRuntime` | SNode.C-thread application graph, selected transport, frontend proxy SDK dispatch, reconnect, and shutdown |
| `ProtocolNormalizer` | Native app-server/bridge input to `codexui.presentation` result/event conversion |
| `PresentationProtocol` | Frame construction, validation, authority, sequence, generation, and scope utilities |
| `PresentationModel` | Qt-owned stable-ID reducer for threads, turns, items, plans, agents, requests, global domains, and telemetry |
| `WorkbenchWidget` | Permanent development harness and user-intent adapter |
| `ShellWidget` | Normal externally designed CodexUI shell over the same model and command API |
| `TurnSettingsWidget` | Codex-native transient settings draft and native thread/turn option encoder |
| `PendingRequestDialog` | Typed, generation-preserving UI for app-server server-request families |
| `MainWindow` | Top-level Qt window ownership only |

### 17.1 FrontendSession API

`FrontendSession` is the normal Qt-side entry point. It provides asynchronous
methods for thread discovery/read/create/resume/fork/rename/archive/delete,
model and environment discovery, turn start/steer/interrupt, controller
claim/release, reconnect, raw diagnostic send, and typed server-request
resolution. Every correlated method returns a presentation correlation ID and
optionally invokes a Qt-thread response callback. It never blocks the GUI
thread or exposes a transport socket.

The generic operation method:

```cpp
request(std::string operation,
        nlohmann::json parameters,
        ResponseHandler handler = {})
```

supports the complete generated AISuite operation catalog without adding one
Qt facade method per rarely used operation. Frequently used UI actions have
narrow named methods such as `listThreads()`, `readThread()`, `startTurn()`,
`steerTurn()`, and `respondToServerRequest()`.

Lifecycle is explicit: `start()` creates the endpoint/runtime graph,
`shutdown()` requests orderly asynchronous termination, and `wait()` joins the
SNode.C thread. `setEventHandler()` receives normalized frames and
`setRuntimeStoppedHandler()` reports terminal worker shutdown.

### 17.2 Normalizer and reducer APIs

`ProtocolNormalizer` accepts transport lifecycle, bridge telemetry, typed
server notifications, server requests, raw inbound observation, operation
success, and operation rejection. Its only output is a validated bounded
presentation frame through its sink. `knownServerMethod()` makes coverage gaps
observable rather than silently treating an unknown method as state.

`PresentationModel::applyEvent()` is the single public reduction entry point.
The model exposes stable thread ordering and lookup, active-turn lookup,
generation-aware pending-request queries, retained global domains, bounded
telemetry, and pending-request presentation records. Internal upsert helpers
preserve complete fields across partial events, correlate child-agent threads,
and apply explicit merge/replace/remove authority.

### 17.3 Transport availability

The executable always builds Unix, IPv4, and IPv6 JSONL clients. TLS, RFCOMM,
WebSocket, and WSS clients are compiled when their SNode.C targets are
available. Exactly one configured client instance may be enabled. Address,
certificate, timeout, queue, reconnect, and instance-enable options come from
the corresponding SNode.C client configuration; CodexUI adds no duplicate
transport configuration.

The build produces two independently launchable applications:

- `codex-ui` is the normal visual UI/UX shell;
- `codex-ui-harness` is the permanent plain protocol/reducer harness.

They compile the same `FrontendSession`, `ClientRuntime`, socketpair,
normalizer, protocol, and presentation-model sources. Only the top-level Qt
consumer differs. Neither executable has a privileged transport or state path.

The current build links the codex AISuite frontend library as
`AISuite::OpenAICodex`, Qt Widgets, Threads, and the selected SNode.C client
modules. The canonical incremental build directory is `build-codex`.
CodexUI CI consumes AISuite from `master`/HEAD and does not pin a particular
AISuite revision. The canonical AISuite replacement must therefore be merged
before the dependent CodexUI change.

### 17.4 Shell settings and pending-request APIs

`TurnSettingsWidget` owns only an upcoming-turn draft. The shell supplies fresh
provider context and catalogs through:

```cpp
setContext(std::string identity,
           const nlohmann::json &canonical,
           const nlohmann::json &models,
           const nlohmann::json &permissionProfiles);
setControlsEnabled(bool enabled);
```

`workspace()` resolves the visible workspace against the caller's local
fallback. `threadStartOptions()` emits only native `thread/start` fields, while
`turnStartOptions()` emits only native `turn/start` fields. Untouched fields are
omitted except that collaboration mode is always explicit because app-server
does not reliably reconstruct its retained value. Explicitly selecting a
provider default emits `null`; named permissions and sandbox policy remain
mutually exclusive. The three app-server
`thread/start` sandbox strings are encoded directly. The richer
`externalSandbox` object is emitted only as a `turn/start.sandboxPolicy`, where
the native protocol defines it. Reasoning efforts, service tiers, default tier,
and personality availability follow the selected model catalog.

The native collaboration object is not a partial mask: its nested `model` is
mandatory, while `reasoning_effort` and `developer_instructions` use the
app-server schema's snake-case names. When the UI shows `Codex default`, the
encoder resolves the catalog entry marked `isDefault` and sends its concrete
model ID. Until that fresh catalog is available, CodexUI omits the otherwise
explicit collaboration object rather than constructing an invalid one.

`PendingRequestDialog::present()` accepts one generation-preserving
`PendingRequestPresentation` and returns either no value when the user closes
the dialog or a `PendingRequestResponse` containing exactly one native result
or JSON-RPC error. `negativeResponse()` constructs the family-specific explicit
decline used by the Requests surface. The caller resolves through
`FrontendSession::respondToServerRequest()` with the stable connection
generation and request ID; the dialog never mutates presentation state itself.

`ShellWidget` is the sole visual command adapter. It translates selection,
composer, settings, controller, thread-management, and request-review actions
into `FrontendSession` calls. Agent messages, plan text, reasoning summaries,
and agent results pass through `QTextDocument::setMarkdown()` with
`MarkdownNoHTML`; user prompts, commands, and command output remain literal.

### 17.5 Essential Automated Architecture Tests

The permanent automated-test policy protects architectural boundaries rather
than individual fixes, widget details, or lines of implementation. A defect
correction does not automatically justify another test. A test belongs in the
codex suite only when it validates a boundary whose failure would undermine
the application architecture independently of the particular symptom that
revealed it.

Two standalone CTest executables form the initial essential suite. They use
production classes directly and are built from the canonical `build-codex`
directory when standard CMake `BUILD_TESTING` is enabled. CTest enables that
option by default; disabling it remains the conventional packaging choice and
does not select a different runtime implementation.

#### Socketpair Contract

`codexui-socketpair-contract-test` exercises the actual two-thread IPC
mechanism:

```text
QCoreApplication / Qt event loop
    -> QtSocketPairEndpoint
    -> nonblocking AF_UNIX SOCK_STREAM socketpair
    -> SNodeSocketPairEndpoint
    -> SNode.C event loop on its worker thread
```

The test constructs the production `SocketPair`, gives one descriptor to the
production Qt endpoint and the other to the production SNode.C endpoint, and
runs both framework event loops. Multiple newline-delimited records travel in
both directions as separately queued writes. The test establishes that byte
ordering is preserved across partial/coalesced stream delivery, both endpoint
queue bounds reject an oversized write without replacing the bound with an
unbounded buffer, and closing the Qt endpoint produces orderly closure on the
SNode.C side without a transport error. It also requires the SNode.C event loop
to terminate cleanly. The test does not introduce another IPC implementation,
polling loop, mock event loop, or synchronous cross-thread method call.

This test deliberately treats the socketpair as an ordered byte stream. JSONL
framing and semantic interpretation remain above this boundary; duplicating
the AISuite `JsonLineFramer` tests here would test another project rather than
CodexUI's thread boundary.

#### Presentation Pipeline

`codexui-presentation-pipeline-test` exercises the production semantic
path without a bridge substitute:

```text
representative native app-server and bridge records
    -> ProtocolNormalizer
    -> codexui.presentation v1 frames
    -> PresentationModel::applyEvent()
    -> coherent Qt-owned presentation state
```

The representative lifecycle includes connection and controller publication,
thread discovery, an authoritative full thread read, a later live turn,
command start, command output, command completion, and turn completion. The
test verifies the contract at architectural granularity: every emitted frame
has the expected protocol version, monotonic sequence, and connection
generation; list/read/live updates converge on stable thread, turn, and item
identities; and the completed model contains one coherent command result with
no active turn left behind. It does not enumerate every generated app-server
method, every presentation field, or every historical correction.

The normalizer sink is connected directly to the reducer because the
socketpair itself is independently covered by the first test. This keeps a
failure attributable to either inter-thread transport or semantic reduction
instead of repeating both mechanisms in every case.

#### Explicit Exclusions

The permanent automated suite does not include:

- a fake or scripted codex-bridge;
- a fake app-server or synthetic network server;
- GUI mouse/keyboard automation or pixel-level styling assertions;
- one test per fixed issue, setting, request family, widget, or source branch;
- the Unix/IPv4/IPv6/TLS/WebSocket/RFCOMM transport matrix already owned by
  AISuite and SNode.C;
- authenticated model execution, approval interaction, or assumptions about
  nondeterministic model output.

A real app-server-to-bridge-to-CodexUI turn remains a manual live acceptance
procedure. It depends on external authentication, service availability,
credits, approval policy, and model behavior, so presenting it as a
deterministic CI test would be misleading. The persistent live topology and
independent bridge observer provide that evidence without introducing a fake
bridge into the CodexUI repository.

The two focused tests can be built and run directly:

```sh
cmake --build build-codex --parallel 8 \
  --target codexui-socketpair-contract-test \
           codexui-presentation-pipeline-test
ctest --test-dir build-codex --output-on-failure \
  -R '^codexui-(socketpair-contract|presentation-pipeline)$'
```

Each test has a ten-second CTest ceiling. Normal successful execution is
substantially shorter and requires no network listener, credentials, isolated
Codex home, or user interaction.

## 18. Live Harness Validation

The harness was exercised against one persistent real topology:

```text
Codex app-server 0.144.6 over IPv4 WebSocket
    <-> codex-bridge over IPv4 WebSocket
    <-> CodexUI harness over IPv4 WebSocket
```

An independent `codex-bridge-client` observer remained connected to the same
bridge while CodexUI held controller ownership. The run used an authenticated
isolated Codex home and an existing persistent bridge process rather than a
simulated provider.

Validated behavior includes:

- initial connection, explicit controller claim/release, and observer fanout;
- fresh thread discovery followed by selected `thread/read(includeTurns=true)`;
- no automatic thread selection when another client or subagent creates a
  thread;
- multiple turns, steering, structured plan updates, command execution,
  command output/completion, and final answers;
- pending-request presentation and resolution without stale brown attention;
- parent/child agent correlation, child history hydration, and retained child
  result presentation in the parent Agents view;
- switching among Conversation, Plan, Agents, Requests, State, and Protocol
  surfaces while turns and agents were active;
- retention of an early completed marker command while later commands, plan
  transitions, subagent activity, and final output arrived;
- stable presentation after turn completion, with no observed disconnect,
  sequence gap, stale pending request, or retained-item disappearance.

The final validation turn lasted about 36 seconds and included a completed
marker command, a three-step completed plan, one subagent thread, later shell
activity, and a final answer. At the final checkpoint the normalized model held
one top-level selected thread, three turns, seventeen items, zero pending
requests, and the retained marker and later activity simultaneously.

A fresh rebuilt visual shell was then validated against the same persistent
bridge. Selecting the completed parent thread retained all top-level rows and
hydrated the Conversation. The Plan inspector reconstructed the retained
textual plan with Markdown formatting; Changes displayed the explicit
read-only empty state; Requests remained at zero; and opening Info lazily
populated the environment State without clearing or blocking Conversation.
Controller and connection status remained stable throughout these tab
transitions. The fresh Agents view correctly remained empty because the
authoritative `thread/read` omitted all prior collaboration items, as documented
below.

A final focused live turn requested exactly one subagent. Raw observer events
contained one completed `spawnAgent` item with child thread ID, one `wait`
operation, the child command/result, and the parent final answer. During the
turn the shell reported `1 agent | 1 active`; after completion it reported
`1 agent | 0 active` and retained one completed agent card with model, effort,
prompt, child thread ID, and result. No provisional spawn or wait row appeared.
The settings controls also displayed explicit chevrons. This run exposed one
additional compatibility defect: Code was displayed after a fresh read while
`turn/start` omitted collaboration mode and app-server silently continued its
retained Plan mode. The encoder now sends the displayed collaboration mode on
every new turn once the mandatory model has been resolved from the fresh
catalog.

The post-fix live acceptance used frontend connection `frontend-27` and a fresh
thread. Its raw `turn/start` contained `mode: "default"`, catalog-resolved model
`gpt-5.6-sol`, and native `reasoning_effort: null` and
`developer_instructions: null` fields. App-server accepted the request,
published matching Default collaboration settings, completed the turn without
tools, and returned the requested `CODE_MODE_OK` response.

Startup latency was traced to eager account/configuration/plugin/app catalogs
queued before the selected thread read. Startup now requests thread discovery
plus the small model and permission-profile catalogs required by the composer.
The larger environment catalog is fetched lazily when Info is first opened,
allowing the selected conversation to hydrate promptly without introducing a
cache.

This live run proves the implemented paths exercised by the scenario; it is
not a claim that every generated operation or every optional transport has
received equivalent live coverage. The canonical build and `git diff --check`
completed successfully. Automated coverage is intentionally limited to the
socketpair contract and presentation pipeline described in Section 17.5; the
real authenticated topology remains the manual live acceptance boundary.

## 19. Provider Limitations Observed Live

### 19.1 Reconstruction Shortcomings

Three app-server reconstruction shortcomings were observed live:

1. Live parent events include `collabAgentToolCall` and child-agent activity,
   but a later `thread/read(includeTurns=true)` returned both parent turns while
   omitting every collaboration and subagent item. A fresh no-cache CodexUI
   therefore cannot reconstruct historical Agents content. During a continuous
   connection CodexUI correlates the authoritative live records by child thread
   ID and keeps implementation threads out of the ordinary top-level list.
2. `turn/plan/updated` notifications produced and updated the Plan view
   correctly during the live session, but a later
   `thread/read(includeTurns=true)` did not return those completed plan updates
   or an equivalent current-plan field. When the read retains a completed
   textual plan item, CodexUI uses that item as the Plan inspector's read-only
   fallback; otherwise it correctly displays `No plan for this thread`.
3. Under legacy history mode, a later `thread/read` can reconstruct generic
   item IDs and omit a live command-execution item even though the live event
   stream contained the richer item.

CodexUI accommodates the representations it receives, but it must not preserve
or synthesize missing provider history across a fresh-read boundary. The
app-server read is authoritative and the codex architecture deliberately has
no semantic cache.

### 19.2 Capability Limitations

The current app-server does not support `historyMode: "paginated"` and returns
`paginated_threads is not supported yet`. A newly started thread is also not
materialized for `thread/read(includeTurns=true)` until it receives its first
user message.

These are provider-boundary discrepancies. CodexUI reports and renders the
authoritative result it receives; it does not hide them with a bridge snapshot,
AISuite cache, or CodexUI persistence layer. A future caching design requires a
separate explicit authority and retention decision.

## 20. Visual Shell Integration Boundary

The externally designed CodexUI shell remains the visual and interaction
reference. Its previous implementation directly consumed the removed AISuite
frontend `State` and SDK types and is preserved on the `legacy-codex` branch;
compiling those widgets unchanged would reintroduce the architecture removed by
the canonical implementation.

The real shell therefore preserves the established layout, hierarchy,
spacing, typography, controls, conversation cards, inspector organization, and
interaction behavior in codex-owned Qt widgets. Those widgets consume only
`PresentationModel` and call only `FrontendSession`. The permanent harness
remains available as the protocol/reducer diagnostic surface and is not itself
the final visual design.

The implemented shell contains the 56-pixel top bar, hideable work sidebar,
thread list, conversation timeline and composer, hideable inspector, Plan,
Agents, Changes, Requests, and Info surfaces, explicit controller control,
connection/request status, complete upcoming-turn settings, and the 40-pixel
status bar. Agent messages, plans, reasoning summaries, and agent results are
rendered with Qt Markdown parsing while embedded HTML is disabled. User text,
commands, and command output remain literal. State and Protocol diagnostics
remain nested under Info rather than dominating normal use.

Pending-request presentation exposes category, stable request ID, connection
generation, owning thread, and a bounded set of safe typed details. The native
request object remains transiently available to the typed response dialog but
is never dumped to the shell, harness State view, notice banner, or protocol
log. Secret answers are held only by password editors until the dialog is
destroyed.

Operation errors, provider notices, protocol diagnostics, and connection
failures produce a dismissible latest-notice banner. Its text is extracted
only from bounded message/detail fields. The complete bounded frame chronology
remains in Info/Protocol. Neither surface has state authority.

Further shell work remains presentation-only. It must not change the socketpair
protocol, app-server normalization, model authority, bridge role semantics,
recovery policy, or AISuite codex implementation unless a proven missing
contract requires a separately reviewed change.

## 21. Architectural Invariants

1. The app-server is Codex semantic and persistence authority.
2. `codex-bridge` is a thin multi-client router with telemetry, not a cache.
3. The codex frontend SDK is a typed proxy, not a frontend state store.
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

## 22. Resolved Presentation Decisions

The remaining presentation-level choices are implemented as follows:

- every incoming frame is reduced immediately, while widget reconstruction is
  coalesced by one 16-millisecond single-shot Qt timer;
- the Info/Protocol view retains at most 2,000 text blocks and the presentation
  model retains at most 256 authority-free telemetry records;
- typed operation errors and provider notices use a dismissible latest-notice
  banner, while unknown/malformed protocol input remains visible in bounded
  diagnostics and never mutates retained presentation state.

No architectural decision remains open in the canonical CodexUI implementation.
Interactive visual validation covered settings, Markdown, plans, pending
requests, and live agent lifecycle. Provider-omitted history remains visible as
an explicit reconstruction boundary rather than being hidden by client state.
