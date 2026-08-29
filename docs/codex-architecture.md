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
- the `PresentationModel`, which is the sole retained authoritative store for
  normalized presentation state;
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

Without endpoint configuration, CodexUI selects AISuite's shared per-user
runtime path (`XDG_RUNTIME_DIR` when private, otherwise
`/tmp/codex-bridge-<uid>/codex-bridge.sock`), so it discovers a default
`codex-bridge` instance without a configuration file.

Bridge provider lifecycle is normalized as `connection.provider` with an
independent provider generation. Disconnect or generation change completes all
outstanding UI operations exactly once, clears provider-scoped presentation
state, and rehydrates the selected thread after the new provider reports
`ready`. Late results from a retired generation are ignored.

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

The Qt endpoint retains queued output as independently owned chunks, releases
each consumed chunk immediately, and limits read and write work per notifier
activation. Both endpoints treat framing or dispatch failure as terminal.

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
| `runtime.shutdown` | yes | Acknowledge and drain, then stop the SNode.C runtime |
| `connection.connect` | no | Connect the selected configured frontend transport |
| `connection.disconnect` | no | Explicitly disconnect the selected frontend transport |
| `connection.reconnect` | no | Explicit bridge transport reconnect |
| `connection.configure` | yes | Apply a transient endpoint selection and connect it |
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
- `thread.read`: returned `thread`, with `replace` when no newer presentation
  event arrived after the read began, otherwise `merge` so a late snapshot
  cannot erase newer live Plan, Agent, command, or turn-diff domain detail;
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
- `dynamic-tool-call`, `authentication-refresh`, and `attestation`.

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

Qt owns `PresentationModel`, the sole retained authoritative store for
normalized presentation state. Widgets and projections read from it; they do
not retain competing copies of thread, turn, item, plan, agent, request, or
global-domain state. The app-server remains the semantic and persistence
authority, so the model is not a persistence layer or substitute for
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

Selecting a thread hydrates it once per bridge connection, including when the
thread-list projection already reports materialized or active turns. The
`thread.read` result is merge-authoritative: it fills reconstruction data but
does not erase retained live-only Plan, Agent, or turn-diff domain details that
the provider omits. This explicit hydration state prevents a partial discovery
projection from being mistaken for an operation-ready thread. Reload is the
explicit forced fresh-read operation.

### 7.1 Upcoming-Turn Settings

The real shell has a codex-native upcoming-turn settings surface backed by the
normalized `PresentationModel`. Its primary controls are:

- model and model-constrained reasoning effort;
- sandbox access and the sandbox-native network choice;
- workspace;
- approval policy;
- personality/style.

The compact More menu contains the named permission profile, approval
reviewer, service tier, reasoning summary, and collaboration mode. Model,
effort, service-tier, and permission-profile choices are populated from fresh
app-server catalogs. A named permission profile and a sandbox policy are
mutually exclusive, matching the native app-server contract. An explicit Access
or Network choice therefore returns the permission-profile control to Thread
default and submits the selected sandbox policy. Other individual controls keep
the active permission profile and submit their supported app-server overrides.

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

### 7.2 Thread Creation and Per-Thread Actions

New thread creation starts with a canonical custom dialog. It captures the
workspace, optional thread name, optional base and developer instructions, and
the native ephemeral flag. The dialog creates only a transient draft. CodexUI
does not create an empty provider thread until the user submits the first
prompt, so canceling or switching away cannot leave a phantom app-server
thread. Model, reasoning, access, permission, style, service-tier, reviewer,
and collaboration choices remain in the shared upcoming-turn controls rather
than being duplicated in the dialog.

Accepting the dialog immediately creates one optimistic thread-list row without
inserting a synthetic thread into `PresentationModel`. The row uses a stable
visual identity and an orange pending sweep. A successful `thread/start`
rekeys that same row to the authoritative thread ID, but it remains pending
until the matching first `turn/start` callback succeeds. That callback switches
the existing row to canonical presentation; failures stop animation and retain
the row with an explicit failure state. Native and web follow the same
lifecycle.

Workspace selection uses the shared custom file browser in directory-only
mode. It validates that the selected directory exists and returns an absolute
local path. The accepted workspace is encoded as the native `thread/start`
`cwd`; CodexUI does not persist it as an application preference.

The visual shell's thread sidebar has no global More menu. A right-click
context menu is created for the stable thread ID under the pointer and exposes
Reload, Rename, Fork, Archive/Unarchive, and Delete. Read-only Reload remains
available to an observer; mutations require the connected controller role.
Opening or invoking the menu does not select the row or disturb the thread
currently being reviewed.

### 7.3 Message Attachments

The composer opens the same custom file browser in multi-file mode. It supports
up to sixteen unique files and reports detected MIME type and size. Local
admission moves the prompt and attachments into a per-thread pending card and
immediately clears the composer so another prompt can be entered. Images become
native `localImage` input and audio becomes `localAudio`. Other files are
appended to the admitted prompt as Markdown links to their local paths, so the
temporary and authoritative cards carry the same durable representation.

These are app-server local-path references, not bytes uploaded through
`codex-bridge`. The provider must be able to access the selected path. This is
correct for a local CodexUI/app-server workspace and remains explicit for a
remote bridge topology; adding remote file transfer would require a separate
bounded protocol and security design.

Image paths are retained in pending and authoritative user-message
presentation. The conversation shows bounded thumbnails below the Markdown
prompt in one source-ordered horizontal ribbon; overflow scrolls horizontally
without wrapping or widening the card. Selecting a thumbnail opens a non-modal,
fit-to-window viewer. CodexUI never fetches remote image URLs implicitly, and
missing local images remain visible as unavailable placeholders.

Authoritative `imageGeneration` items use their app-server `savedPath` and the
same thumbnail/viewer. Their Base64 `result` is transport data and is never
rendered as text. Authoritative `imageView` items use their local `path` and the
same presentation with the neutral title `Image`. Unknown item types retain a
generic diagnostic card, but its visible JSON is bounded before Qt performs
text layout.

Conversation-card folding is presentation state, not protocol state. Each
stable visual card key retains its user-selected collapsed state in the
`ConversationView` for the UI session. New message cards default expanded and
new activity cards default collapsed. The card owns one header and one content
container, so streamed payload updates remain live while folded without
changing visible height. `ConversationView` owns the fold geometry transaction,
including title anchoring within the natural scroll range, alongside its
existing single-owner scrolling calculations. Expansion scrolls only as needed
to reveal the complete card when it fits in the unobscured viewport above any
grown composer overlay. At the lower limit, normal range clamping may move the
selected title rather than creating artificial blank space.

### 7.4 Changes and Diff Presentation

The Changes inspector is authoritative over the local Git worktrees associated
with the selected thread. It does not use app-server `turn.diff.changed` or
`fileChange` messages as review content. `ThreadPresentation` retains bounded,
deduplicated command working directories and changed-path hints from the
thread's authoritative items. The provider resolves each directory upward with
libgit2, deduplicates repository roots, validates ambiguous paths against the
worktree, index, and HEAD, and persists the resolved roots per thread. A path
that is currently changed ranks above the same clean tracked path; equal-rank
matches remain available together. It never performs a recursive downward
workspace search.

Resolved roots are synchronously persisted in QSettings and loaded from either
the native string-list representation or the scalar representation used by the
INI backend for a single root. Consequently, restart hydration does not depend
on historical command items being present in `thread.read`.

The repository selector defaults to All repositories when several candidates
match. Candidate paths containing a dot-prefixed directory are excluded by
default; the persistent Hidden option explicitly includes them. The provider
exposes Unstaged, Staged, and Since HEAD scopes. Untracked
content—including files created outside CodexUI—renames, copies, deletions,
type changes, conflicts, and binary metadata come from libgit2. A folder
outside Git remains a valid Codex workspace, but its Changes tab reports that
review requires a repository.

The Inspector contains a compact unified preview with stable file selection,
addition/deletion counts, Copy, Open review, and file-double-click review. The
modeless Change Review window remains usable beside the conversation and offers
Unified or Side by side layout plus Compact or Expanded context. Preferences
persist across threads. Repository collection runs outside the UI thread,
superseded results are discarded, and rendered diff content is bounded to 16
MiB with an explicit truncation state. Every returned file carries its resolved
absolute pathname. CodexUI watches existing changed files and their parent
directories, then debounces filesystem events into a fresh libgit2 snapshot.
Parent-directory watches keep deletion, recreation, rename, and atomic file
replacement consistent. A visible-only two-second refresh remains the safety
net for newly created files in previously unwatched nested directories and for
index-only changes. Files disappear from selection as soon as libgit2 reports
that they are clean again.

### 7.5 Conversation Projection and Prompt Admission

The selected conversation is a pure projection of `PresentationModel` plus
client-local prompt admissions. Its one structural grouping level is the
app-server turn: each retained turn contributes one transparent section, and
its items remain in exact server order. A turn is identified only by its stable
turn ID; CodexUI does not infer a turn boundary from a user-message card.

Authoritative cards use the stable `(threadId, turnId, itemId)` identity.
Locally admitted cards use a process-wide submission identity that remains
stable when a new-thread draft receives its app-server thread ID. Initial
render and later updates use the same keyed reconcile path. Existing widgets
are updated in place, absent keys are removed, new keys are inserted at their
projected positions, and an identical typed projection is a true visual no-op.

Prompt admission and app-server acknowledgment are separate states. On Send or
Steer, CodexUI immediately appends a client-local pending user card to the
destination thread. The card uses a muted blue user-prompt treatment and a
Qt-painted highlight sweeping left and right until the correlated app-server
result callback arrives. Only the matching `turn.start` or `turn.steer`
completion callback acknowledges the prompt; conversation events cannot infer
acknowledgment. Each request carries a unique `clientUserMessageId`, allowing
the resulting user item to bind exactly even when prompts have identical text.
A fast successful result retains a 500-millisecond accepted transition so the
state change remains visible. Pending cards survive thread switching and
become normal authoritative user messages when the corresponding app-server
item materializes. The pending and authoritative forms share one visual key
and anchor during the accepted transition. Once materialization and that
transition are complete, the local submission is removed and the retained item
uses its authoritative identity. Failure produces a retained error card.

The composer remains enabled while acknowledgments are outstanding. Multiple
prompts may be admitted, but CodexUI dispatches them sequentially per thread so
each operation observes the turn state established by the preceding result.
Queues for different threads are independent. New-thread prompts remain bound
to the explicit creation draft until `thread.create` returns its stable ID.
Dispatch waits for explicit connection-generation thread hydration. A
provider-marked `notLoaded` thread is resumed first, and a transient
thread-not-found submission result permits exactly one resume-and-retry before
becoming a terminal error. Failed hydration rejects local admission without
clearing the composer draft; an explicit reload is required before sending.
Transport eligibility is rechecked at the queued dispatch boundary: a
disconnect leaves the prompt queued until bridge-open re-drives dispatch, and
an in-flight resume gates both hydration reads and turn operations.

The conversation smoothly follows new content only while its vertical scrollbar
is at the bottom. Geometry bursts retarget a short monotonic animation to the
latest maximum. Manual upward scrolling interrupts that animation immediately
and pauses following until the user returns to the bottom. Programmatic Qt
range clamps from card reflow do not change this user-owned state. While paused,
the first visible stable card and its viewport offset anchor the reading position
across appends, card reflow, and reconstruction. Wheel and touchpad events over
non-scrollable center-pane chrome and splitter handles are forwarded to the
conversation. Command text and output retain a gesture that began while they
could move in its direction, including later updates at the reached boundary.
A fresh outward gesture begun at an existing boundary is routed to the
conversation.
Follow/pause mode and the stable anchor are stored per thread and restored when
the user returns to that thread.

The update pipeline compares each card's typed visible projection.
Protocol-only changes cannot mutate widgets or scroll state. All visible item
changes in one reconcile are measured and applied inside one paint-suppressed
layout transaction, followed by one scroll settlement. This is especially
important for Command execution cards, whose streaming output and bounded
nested viewer alter geometry. New authoritative items are inserted at their
server-ordered layout position without rebuilding retained cards. While
following is paused, the effective history window expands with appends so its
stable visual anchor cannot be evicted; the requested bound is restored when
following resumes.

The bottom composer overlay has a canonical in-layout reservation. As multiline
input, attachments, settings, or attention controls grow beyond that height,
the conversation viewport keeps its geometry and the composer overlays its
lower portion. A content-owned logical trailing extent grows by the same extra
height, extending the natural `QScrollArea` range so the final card can be
scrolled to the overlay boundary. Permanent scroll-owned bottom padding is not
used; the moving composer owns the standard divider with the canonical 8 px
vertical spacing on both sides and 10 px horizontal outset beyond its adjacent
content. The scrollbar maximum is never assigned manually.

Trailing-extent growth temporarily suppresses range-driven bottom following
and restores the previous scrollbar value, so existing messages do not move.
Reaching the new maximum re-enables following. Composer contraction removes the
extent; Qt may clamp the value to the reduced range, and being at that maximum
re-enables following for later content.

### 7.6 Command Execution Output and Info Viewers

Command execution output controls exist only for printable, non-whitespace
output after terminal control sequences are ignored. Empty, whitespace-only, and
ANSI/control-only results create no black output surface. A shown control grows
from zero content height to a 220-pixel maximum. Its width-dependent content
height is measured synchronously inside the conversation update transaction.
Streaming output and command completion mutate the retained outer card in
place; a protocol update with an unchanged visible fingerprint touches neither
the widget nor scroll state. Beyond the maximum the output control uses the
shared styled vertical scrollbar. It follows appended output only while already
at its bottom; manual upward scrolling pauses following, and the state is
retained across in-place output updates. The bounded command-text control uses
the same gesture-boundary ownership as the output control.

The Info tab's State and Protocol viewers use the same scrollbar styling and
show vertical scrollbars only when required. The Protocol log owns the tab's
expanding region and its statistics summary is placed below the log. Inspector
Plan, Agents, and Requests content is read from retained per-thread
presentation snapshots. Changes is instead refreshed from the selected
thread's local Git worktree and is independent of protocol-frame retention.

## 8. Plans and Agents

### 8.1 Plans

`turn/plan/updated` is the canonical structured plan update. A normalized plan
replacement carries the thread ID, turn ID, optional explanation, and ordered
steps with `pending`, `inProgress`, or `completed` status.

Plan presentation is retained across tab and thread switching. It changes only
for the identified turn and is cleared only by an explicit authoritative empty
or replacement event for that turn. The Inspector is the production owner of
structured plans, so they are not duplicated in the conversation. The typed
turn-level conversation key, conversion, placement, and renderer are retained
behind a disabled projection switch for narrow reactivation. Textual `plan`
items remain supported conversation content and use the same card renderer.
When no structured plan survives a fresh `thread/read`, the Plan inspector
renders the newest retained textual plan item as a read-only compatibility
view; it never overrides a newer authoritative structured turn plan.

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
but are omitted from the ordinary top-level thread list. When one is already
user-selected, the sidebar retains that visible row across subsequent
navigation for the session. An authoritative thread removal still drops it.

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

Connection controls sit immediately to the left of Claim/Release control
because transport lifecycle and controller ownership are distinct operations.
The menu exposes Configure, Connect, Disconnect, and Reconnect. It never claims
control as a side effect.

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

A refresh result applies its declared authority. In particular, `thread.read`
merges represented content and has no deletion authority because the current
provider projection is incomplete. Explicit scoped remove events remain
authoritative. Temporary disconnect, incomplete discovery, request failure, or
an unknown message does not authorize clearing the existing presentation.

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

The connection dialog reads the effective SNode.C client configurations to
enumerate compiled transports and provide current endpoint defaults. A user may
override the selected Unix path, IP host/port, WebSocket path, or RFCOMM
address/channel for the running CodexUI session. TLS certificate and
verification configuration remains in the corresponding SNode.C config
object. Runtime overrides are intentionally transient and are not written to a
CodexUI data file.

Changing transport uses one asynchronous lifecycle: disconnect the attached
frontend SDK, terminate the selected SNode.C flow, wait for both to detach,
apply the new selection, then connect once. Repeated logical connect requests
cannot create parallel flows or reuse an attached SDK. Local disconnect,
reconnect, and transport-switch reasons remain distinguishable from remote
closure in normalized diagnostics.

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

## 16. Application Presentation

The production `ShellWidget` is the sole Qt consumer of the
`codexui.presentation` contract and `PresentationModel`. Conversation, Plan,
Agents, Changes, Requests, retained State, and bounded Protocol diagnostics are
integrated into that shell. Diagnostic presentation is telemetry only: it
cannot replay frames, hydrate state, supply deletion authority, or conceal a
missing app-server result. The shell has no semantic snapshot or parallel state
authority.

## 17. Implemented Components and APIs

The canonical CodexUI implementation contains one complete visual shell.
`ExpandingPromptEditor` and the visual style helpers live under
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
| `ShellWidget` | Product shell and protocol/application coordinator; owns stable selection, hydration, recovery, and command dispatch |
| `MiddleRegionWidget` | Three-pane visual composition and center-region wheel routing |
| `ThreadPane` | Stable-ID thread-list projection and thread actions |
| `ConversationProjection` | Pure thread-to-turn-to-card projection over `PresentationModel` and local prompts |
| `ConversationView` | Stable-key reconciliation, card geometry, per-thread follow/pause state, and anchor-preserving scrolling |
| `ConversationCard` implementations | In-place typed card presentation, including pending prompts and bounded Command execution output |
| `PromptCoordinator` | Per-thread prompt admission queues, callback-only acknowledgment, and authoritative-item correlation |
| `ComposerPane` | Bottom-anchored upcoming-turn controls, attachments, prompt editor, and overlay-height reporting |
| `InspectorPane` | Retained Plan, Agents, Requests, State, and Protocol presentation plus selected-workspace Git review |
| `TurnSettingsWidget` | Codex-native transient settings draft and native thread/turn option encoder |
| `NewThreadDialog` | Transient native thread-start draft with workspace selection and instructions |
| `FileSelectionDialog` | Canonical directory or bounded multi-file browser shared by workspace and attachments |
| `ConnectionDialog` | Session-only selector over effective compiled SNode.C client configurations |
| `GitDiffProvider` | Asynchronous in-process libgit2 repository discovery and scoped diff snapshots |
| `DiffViewer` | Compact repository summary/preview and modeless unified or side-by-side review |
| `PendingRequestDialog` | Typed, generation-preserving UI for app-server server-request families |
| `MainWindow` | Top-level Qt window ownership only |
| `BrandMark` and desktop resources | Shared visual mark and the consistent `codex-ui` executable/application/window/icon identity |

### 17.1 FrontendSession API

`FrontendSession` is the normal Qt-side entry point. It provides asynchronous
methods for thread discovery/read/create/resume/fork/rename/archive/delete,
model and environment discovery, turn start/steer/interrupt, controller
claim/release, transport connect/disconnect/reconnect/configure, raw diagnostic
send, and typed server-request
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
`steerTurn()`, `configureConnection()`, and `respondToServerRequest()`.

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

The build produces one application, `codex-ui`. It integrates the production
shell with `FrontendSession`, `ClientRuntime`, the socketpair, normalizer,
presentation protocol, and presentation model; no alternate UI target has a
privileged transport or state path.

The current build links the codex AISuite frontend library as
`AISuite::OpenAICodex`, Qt Widgets, Threads, libgit2 through pkg-config, and the
selected SNode.C client modules. Git review is performed through libgit2; the
application never launches a Git process. CodexUI CI consumes AISuite from
`master`/HEAD and does not pin a particular AISuite revision. The canonical
AISuite change must therefore be merged before the dependent CodexUI change.
The AISuite dependency build is limited to two compiler jobs because its
generated protocol translation units can otherwise exceed the hosted runner's
aggregate memory.

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
Its custom dialogs return transient value objects and never mutate the
presentation model directly. The composer owns attachment drafts; the
connection dialog edits only the SNode.C runtime selection; and `DiffViewer`
is a read-only consumer of normalized model domains and retained provider
items.

### 17.5 Essential Automated Architecture Tests

The permanent automated-test policy protects architectural boundaries rather
than individual fixes, widget details, or lines of implementation. A defect
correction does not automatically justify another test. A test belongs in the
codex suite only when it validates a boundary whose failure would undermine
the application architecture independently of the particular symptom that
revealed it.

Seven focused CTest executables form the essential suite. They use production
classes directly and are built when standard CMake `BUILD_TESTING` is enabled.
CTest enables that option by default; disabling it remains the conventional
packaging choice and does not select a different runtime implementation.

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
effective transport-settings publication, thread discovery, an authoritative
full thread read, a later live turn, command start, command output, command
completion, authoritative turn-diff publication, and turn completion. The
test verifies the contract at architectural granularity: every emitted frame
has the expected protocol version, monotonic sequence, and connection
generation; connection settings reduce coherently; list/read/live updates
converge on stable thread, turn, and item identities; and the completed model
contains one coherent command result and scoped diff with no active turn left
behind. It does not enumerate every generated app-server method, every
presentation field, or every historical correction.

The normalizer sink is connected directly to the reducer because the
socketpair itself is independently covered by the first test. This keeps a
failure attributable to either inter-thread transport or semantic reduction
instead of repeating both mechanisms in every case.

#### Conversation Projection

`codexui-conversation-projection-test` verifies the pure typed projection and
prompt coordinator: one section per app-server turn, stable card identity and
server ordering, per-thread prompt queues, dispatch-time Start/Steer choice,
callback-only acknowledgment, exact `clientUserMessageId` correlation,
duplicate-prompt ordering, history bounds, resolved-submission removal, and
Command execution output visibility.

#### Middle-Region Behavior

`codexui-conversation-cards-test` exercises the actual conversation widgets
programmatically. It verifies smooth follow, user-owned pause, stable
card-and-pixel anchoring across every card type and width-dependent reflow,
per-thread restoration, composer trailing space, pending-prompt animation,
and independent Command execution output sizing and scroll ownership.

`codexui-application-layout-test` verifies the three-pane constraints, composer
overlay geometry, complete center-region wheel routing, thread-list selection
projection, nested-scroll handoff, and retained Inspector/Info behavior. These
are state and geometry assertions over Qt widgets, not golden-screenshot or
pixel-perfect visual baselines. The pending-animation check compares two
transient card rasters only to prove that motion exists.

#### Shell Integration

`codexui-shell-integration-test` drives the production `ShellWidget` and
`FrontendSession` across their real socketpair presentation boundary. It
verifies exact visible-thread routing, independent prompt queues, real result
acknowledgment, background completion, retained Plan/Agents state, monotonic
hydration across reconnect, terminal callbacks, failed-hydration draft
retention, bounded child-thread reads, and one-shot thread-not-found recovery.

#### Git Changes Integration

`codexui-git-changes-live-test` uses production `DiffViewer`,
`GitDiffProvider`, QFileSystemWatcher, and libgit2 against a temporary real Git
repository. It performs filesystem writes rather than UI interaction. The test
verifies polling discovery of a manually created nested untracked file and
native watcher refresh after removal, content reversion, deletion restoration,
and atomic replacement. `codexui-application-layout-test` complements it with
in-process repository-resolution coverage for all scopes, duplicate candidates,
ambiguous and absolute paths, All and individual repository selection, hidden
repository exclusion/inclusion, stale hints/selections, and preference for an
actually changed path over an identical clean tracked path.

#### Explicit Exclusions

The permanent automated suite does not include:

- a fake or scripted codex-bridge;
- a fake app-server or synthetic network server;
- external GUI-driving automation, golden screenshots, or pixel-perfect
  styling baselines;
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

The seven focused tests can be built and run directly:

```sh
cmake --build "${BUILD_DIR}" --parallel 8 \
  --target codexui-socketpair-contract-test \
           codexui-presentation-pipeline-test \
           codexui-conversation-projection-test \
           codexui-conversation-cards-test \
           codexui-application-layout-test \
           codexui-git-changes-live-test \
           codexui-shell-integration-test
ctest --test-dir "${BUILD_DIR}" --output-on-failure \
  -R '^codexui-(socketpair-contract|presentation-pipeline|conversation-projection|conversation-cards|application-layout|git-changes-live|shell-integration)$'
```

Each test has a 10-to-30-second CTest ceiling. Normal successful execution is
substantially shorter and requires no network listener, credentials, isolated
Codex home, or user interaction.

## 18. Live Application Validation

The application was exercised against one persistent real topology:

```text
Codex app-server over IPv4 WebSocket
    <-> codex-bridge over IPv4 WebSocket
    <-> CodexUI over IPv4 WebSocket
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
marker command, a three-step completed plan, one subagent thread, later Command
execution activity, and a final answer. At the final checkpoint the normalized
model held one top-level selected thread, three turns, seventeen items, zero pending
requests, and the retained marker and later activity simultaneously.

A new CodexUI process was then validated against the same persistent bridge.
Selecting the completed parent thread retained all top-level rows and
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
   or an equivalent current-plan field. During the current session, the merge
   authority of `thread.read` preserves the live structured plan. On a fresh
   process, a completed textual plan item is used as the Plan inspector's
   read-only fallback when available.
3. Under the configured app-server history representation, a later
   `thread/read` can reconstruct generic
   item IDs and omit a live command-execution item even though the live event
   stream contained the richer item.

CodexUI preserves already observed live presentation when an incomplete read
omits it, but it does not synthesize content that the process has never
observed. A fresh process therefore remains limited to the provider's
reconstruction. This merge policy is bounded in-memory presentation retention,
not a semantic cache or persistence authority.

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

The CodexUI shell is implemented in codex-owned Qt widgets. Those widgets
consume only `PresentationModel` and call only `FrontendSession`.

The implemented shell contains the 64-pixel top bar, hideable work sidebar,
thread list, conversation timeline and composer, hideable inspector, Plan,
Agents, Changes, Requests, and Info surfaces, explicit controller control,
connection lifecycle/configuration, canonical new-thread/workspace/attachment
dialogs, per-thread context actions, complete upcoming-turn settings, a
first-class diff viewer, pending-prompt cards, request status, and the 40-pixel
status bar. Agent
messages, plans, reasoning summaries, agent results, and authoritative user
messages are rendered with Qt Markdown parsing while embedded HTML is disabled.
The transitional local prompt, commands, and command output remain literal.
State and Protocol diagnostics remain nested under Info rather than dominating
normal use.

Pending-request presentation exposes category, stable request ID, connection
generation, owning thread, and a bounded set of safe typed details. The native
request object remains transiently available to the typed response dialog but
is never dumped to the shell, Info/State view, notice banner, or protocol log.
Secret answers are held only by password editors until the dialog is
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
16. Prompt routing always uses the stable visibly selected thread; thread
    creation requires an explicit new-thread intent.
17. Pending prompts are presentation state until acknowledged and are
    dispatched sequentially per thread without disabling the composer.
18. Conversation and nested-output following is enabled exactly while the
    corresponding scrollbar is at its bottom. Conversation following is smooth
    and user-interruptible; its paused state preserves a stable visual anchor.
19. Composer growth overlays the unchanged message viewport and adds equal
    trailing content space without automatically moving existing messages.
20. Thread selection hydrates once per bridge connection; prompt dispatch waits
    for readiness and permits at most one resume-and-retry after a transient
    thread-not-found result.
21. Nonvisual item updates do not reconstruct cards; one coalesced refresh uses
    one hidden layout transaction and one scroll settlement.
22. Conversation hierarchy has exactly one semantic grouping level: stable
    app-server turns containing stable server-ordered items.
23. `PresentationModel` is the only retained normalized presentation store;
    conversation and inspector views are keyed projections, not parallel state
    authorities.

## 22. Resolved Presentation Decisions

The remaining presentation-level choices are implemented as follows:

- every incoming frame is reduced immediately; the selected conversation then
  takes one typed projection snapshot and one stable-key reconcile, with
  identical visible projections producing no widget or geometry work;
- the Info/Protocol view retains at most 2,000 text blocks and the presentation
  model retains at most 256 authority-free telemetry records; the protocol
  statistics summary is below the expanding log;
- pending prompt acknowledgment uses a per-thread animated card rather than an
  application-wide busy state or composer lock;
- reaching the conversation bottom re-enables automatic following, including
  after scrolling through composer-added trailing space or a contraction clamp;
- paused conversation updates preserve the first visible stable card and its
  pixel offset through appends, reflow, and reconstruction;
- typed operation errors and provider notices use a dismissible latest-notice
  banner, while unknown/malformed protocol input remains visible in bounded
  diagnostics and never mutates retained presentation state.

No architectural decision remains open in the canonical CodexUI implementation.
Interactive visual validation covered settings, Markdown, plans, pending
requests, and live agent lifecycle. Provider-omitted history remains visible as
an explicit reconstruction boundary rather than being hidden by client state.
The current CodexUI acceptance boundary requires focused build/tests and live
visual acceptance of thread routing, prompt acknowledgment, scrolling,
composer geometry, attachments, connection, and diff surfaces. No semantic
cache is part of this boundary.
