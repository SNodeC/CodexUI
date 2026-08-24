# CodexUI as-built UI inventory

This document records the current CodexUI presentation as implemented and observed. It is a baseline for later design work, not a redesign proposal. The accompanying state-by-state capture log is in [STATE-MATRIX.md](STATE-MATRIX.md).

## Exact baseline

| Component | Baseline |
|---|---|
| CodexUI | `8a6440243c1d97cca07ee52305ed0262a5d8ab52` (`master`, 2026-08-19, `Add CodexUI UX redesign roadmap`) |
| AISuite source and installed package | `8be3408830d78ca6ace58792f3cccb9139631f18` (`master`, 2026-08-19, `Merge pull request #40 from SNodeC/agent/support-full-codex-user-input`) |
| AISuite package/API version | `0.1.1`; Codex shared-library SOVERSION `2` |
| Codex app-server | `codex-cli 0.144.6` |
| SNode.C reported by `codex-backend --version` | `1.0-rc1` |

The baseline used Ninja, GCC 16.2.0, CMake 4.3.4, `RelWithDebInfo`, and Qt
6.10.2 (`Widgets` and `Network`). Both builds completed successfully, and
`ldd` confirmed that the captured CodexUI executable loaded the intended
workspace-local AISuite libraries.

## Capture environment

| Property | Value |
|---|---|
| Operating environment | Linux `7.1.8+deb14-amd64`, x86-64 |
| Desktop/session | KDE Plasma 6.7.4, Wayland session |
| Display variables | `DISPLAY=:0`, `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=/run/user/1000` |
| Screen | Built-in `eDP-1`, 1920 x 1200 at 60 Hz |
| Scale/device pixel ratio | Output scale `1`; no Qt scale override was set |
| Color/display features | sRGB, HDR disabled/incapable, brightness 100% |
| Screenshot path | The application was launched through Qt's XCB platform plugin under XWayland so the exact application client window could be captured with native X11 window capture (`xwd`) and converted losslessly to PNG. |

The default comparison size is the application's own 1536 x 960 client size. Narrow and large variants are 1100 x 700 and 1800 x 1080 respectively.

The real current `codex-backend` and real Codex app-server were used. To avoid mutating or competing with the user's concurrently used Codex home, the capture run used an isolated temporary Codex home initialized from the user's valid configuration/authentication and persisted state, with normal backend/app-server behavior thereafter. The frontend connected over a dedicated local Unix socket and used the normal AISuite immutable state projection. No fake protocol state or production-code instrumentation was introduced. A real thread, prompt, streamed response, final response, restart/reload, and backend disconnect/reconnect were exercised.

## Source-level structure

### Main application and workbench

`Application` constructs `MainWindow`, which owns a single `WorkbenchWidget`. `MainWindow` sets the window title to **CodexUI — Codex Workbench**, a minimum size of 1100 x 700, and a default size of 1536 x 960. It applies the global stylesheet and a 12-pixel application font.

`WorkbenchWidget` is the principal composition and presentation coordinator:

```text
WorkbenchWidget
├── top bar (56 px)
├── horizontal QSplitter
│   ├── SidebarWidget
│   ├── ConversationWidget
│   └── InspectorWidget
└── status bar (40 px)
```

The outer layout has zero margins and spacing. The splitter has an 8-pixel handle, non-collapsible children, stretch factors `0 / 1 / 0`, and initial sizes `282 / 834 / 404`. Restoring both side panels applies the same nominal widths while leaving at least 500 pixels for the center. Sidebar and Inspector each have a Hide action; when hidden, matching controls in the top bar restore them.

The top bar contains the workbench title, a Commands menu, current model/provider status, pending-request count, and the right-panel restore control. Commands currently includes reconnect behavior rather than a broad command palette. The bottom status bar presents connection/provider identity, controller state, synchronization/state status, agent activity, and request count.

`WorkbenchWidget` receives immutable AISuite `client::State` revisions from `FrontendSession`. State notifications are coalesced before presentation refresh (approximately one 16 ms presentation interval), and the update scope is used to avoid unconditional conversation, Sidebar, and Inspector rebuilds. The controller is acquired and released through the typed SDK. No separate conversation model exists in the UI.

### Sidebar and thread list

`SidebarWidget` owns the Work header, Hide button, New Thread button, thread-list scroll area, and backend status footer. Its width is constrained to 220–440 pixels. The main margins are approximately 10 pixels horizontally and 14–17 pixels vertically.

The New Thread control is 36 pixels high. The thread area is a vertically scrolling `QScrollArea` with horizontal scrolling disabled. Each custom `ThreadRow` is at least 58 pixels high, with roughly 5 pixels between rows. A row presents an activity/status dot, title or shortened ID, and a secondary status/preview line. Long values are elided and exposed through tooltips. Selection is mouse-driven by the custom frame; the current implementation is not a model/view list and does not expose a thread context menu.

The footer distinguishes application/backend availability and synchronization. Thread rows are reconstructed from the canonical thread collection, while presentation equality prevents needless widget replacement when nothing visible changed.

Dynamic states include no threads, selected thread, active thread, attention/status colors, not-loaded versus loaded threads, truncated list metadata, connected/synchronizing/disconnected backend status, and a pending new-thread draft.

### Conversation

`ConversationWidget` owns the selected-thread context, turn summary, failure presentation, scrollable timeline, and composer. Its minimum width is 480 pixels. The central content margins are approximately 24 pixels horizontally and 14 pixels at the top.

The principal scroll area disables horizontal scrolling. Its content contains both the timeline and the composer; the composer is therefore at the bottom of the scroll content rather than a fixed sibling outside the scroll area. The timeline is deliberately windowed to the latest 32 turns and 256 timeline items. Activity groups show at most 16 detailed activity entries. A notice reports when older canonical history is not materialized.

The renderer reconciles stable turn/segment identity and presentation keys so unchanged widgets are retained. New reflected prompts and final/streamed messages append at the end. When the viewport follows the latest item, scrolling uses a short animation and deferred layout settling; when the user has scrolled away, the current viewport is preserved.

Presentation types include:

- turn header/summary cards with status, item count, failure, and token usage;
- user-message cards from `userMessageSemanticView()`;
- Codex commentary and final message presentation;
- reasoning summaries and reasoning activity;
- command, tool, plan, file-change, collaboration, and other activity cards built from typed semantic views;
- truthful unavailable/partial semantics where the SDK does not expose complete typed detail.

Normal text uses wrapping labels and is mouse-selectable where appropriate. Very large message bodies (over the widget's large-message threshold) use a read-only `QPlainTextEdit` with a fixed 240-pixel height, allowing local scrolling without creating an extremely tall label.

The conversation distinguishes no selection, loading/incomplete selection, idle thread, active turn, terminal turn, failed turn, disconnected state with retained canonical content, and thread-list or projection truncation.

### Turn presentation

Turn presentation is assembled within `ConversationWidget`; there is no independent turn model/controller. Each rendered turn has a compact header and its ordered item segments. Turn status comes from typed `TurnState::status`, with active/terminal/connection-invalidated flags. Token usage and failure detail are rendered from bounded typed/opaque state fields.

Historical turns remain in AISuite State, but only the bounded latest window is materialized as Qt widgets. The current State does not expose the complete effective execution configuration for an historical turn, so it cannot yet display the Phase 1 historical configuration table authoritatively.

### Composer

The composer is a 100-pixel-high raised panel with a multi-line plain-text editor, an Attach affordance, a **Ctrl+Enter to send** hint, Send, and Stop. Attach is visibly present but disabled. Send is enabled only when the session/controller/thread state permits it and the editor has content. Stop is enabled for an active turn.

Ctrl+Enter submits; ordinary Enter inserts a newline. Focus is shown with a stronger border. Sending does not maintain an optimistic local prompt cache: the reflected user message arrives through canonical AISuite State. New Thread currently enters a draft state; the first submitted prompt calls typed `thread.start` and then starts the turn. For an existing idle thread, the UI starts a turn; otherwise it resumes the thread before starting the turn.

### Inspector

`InspectorWidget` is constrained to 300–520 pixels wide and defaults to the **Agents** tab. It owns four tabs—Plan, Agents, Changes, and Info—each with its own vertical `QScrollArea`. Main horizontal margins are approximately 18–20 pixels, with a 14-pixel top margin.

- **Plan** resolves plan semantic views from the selected/latest turn and renders ordered steps and status.
- **Agents** resolves collaboration/subagent activity and summary state.
- **Changes** resolves file-change semantic views and offers typed, read-only change presentation.
- **Info** displays thread identity, title/status, workspace/model/provider, loading/realtime state, latest-turn facts, synchronization, state/list completeness, controller, and retained diagnostics.

Inspector content is rebuilt only when its presentation key changes. Empty states are explicit. Dynamic copy is plain text and frequently mouse-selectable. Switching tabs does not alter canonical state.

### Approval and user-input requests

`InteractiveRequestDialog` is a non-modal `QDialog`, minimum width 520 pixels and initial size approximately 580 x 430. It has a scrollable body, current/queued-request presentation, submit/negative actions, next request, and close behavior. It stays synchronized with canonical pending requests rather than retaining an independent request model.

Implemented typed presentations cover simple command/file approvals, patch/exec review approvals, and `request.userInput` questions. Same-ID requests are refreshed when their semantic fingerprint changes. Provider-controlled values are forced to plain text. Incomplete approval semantics allow safe negative decisions while positive actions fail closed. Invalidated requests are completely disabled. Secret answers use password echo mode and drafts are cleared aggressively after use.

Permissions approval, authentication, attestation, dynamic-tool calls, and MCP elicitation remain visible pending-request kinds without complete typed response UI. The dialog can therefore communicate their presence but cannot complete those product flows.

The capture app-server reported `request_user_input` unavailable in the capture mode and a harmless command did not trigger approval. Consequently the actual dialogs could not be reached naturally during this run; no fake request state was injected.

### Connection and reconnect states

`FrontendSession` owns the `QLocalSocket`, AISuite client, serialized-frame queue, local peer verification, state callbacks, and reconnect timer. The UI distinguishes initial connecting, synchronizing, ready, disconnected/retrying, and terminal failure. Socket closures judged retryable use bounded exponential backoff; terminal protocol/capacity/authentication failures do not loop automatically. Commands exposes an explicit Reconnect action.

The send path preserves partial socket writes through a bounded ordered output queue. Inbound work is byte/frame-budgeted and resumed through the event loop, keeping presentation work out of the socket callback. Diagnostics are shown separately and do not themselves force a false connection-state transition.

The reconnect capture was produced by stopping the real current backend while retaining the selected conversation. It shows canonical content remaining visible while the status changes to connection refused/unavailable. Restart/reload of the current UI also restored the same real thread from backend/app-server state.

## Current visual implementation vocabulary

These are literal current conventions, not normalized design tokens proposed for future use.

### Typography

- Application stack: `Inter`, then `Noto Sans`, then `DejaVu Sans`.
- Global/application size: 12 px.
- Main heading: approximately 18 px, weight 600.
- Titles/body emphasis: approximately 13 px.
- Sections and metadata: approximately 10 px; a few dense labels use 9 px.
- Most dynamic textual content is literal plain text; message/output content is selectable.

### Colors

| Role | Current value |
|---|---|
| Application background | `#0e1013` |
| Panel background | `#13161a` |
| Raised/card background | `#181c21` |
| Divider/border | `#2b3038` (input borders use approximately `#343b45`) |
| Primary text | `#e8edf2` |
| Secondary text | `#949ead` |
| Selection/action blue | `#4f94f5` |
| Success/ready green | `#40c27d` |
| Warning/attention amber | `#f5a83b` |
| Collaboration/accent purple | `#7a63e0` |
| Stop/destructive background | approximately `#521a1a` |

### Geometry and spacing

- Top bar: 56 px; status bar: 40 px.
- Default three-panel widths: 282 / 834 / 404; splitter handle: 8 px.
- Sidebar: 220–440 px; Inspector: 300–520 px; Conversation minimum: 480 px.
- Standard panel/content insets are generally 10–24 px; local gaps are usually 4–12 px.
- Thread rows: minimum 58 px with approximately 5 px between rows.
- Raised cards/composer: approximately 10 px corner radius; compact summary and badges use 5–7 px radii.
- Normal buttons: approximately 32 px high, 7 px radius, 12 px horizontal padding, 11 px semibold text.
- New Thread: 36 px high. Composer: 100 px high. Large message editor: 240 px high.
- Scrollbars: 8 px wide, 2 px margin, 28 px minimum handle, 3 px handle radius.
- Tabs: minimum 62 px wide and 30 px high, with approximately 7 px radius.

Notable explicit control sizes include Commands 132 x 32, model/provider 210 x 32, request count 106 x 32, Send/Stop 66 x 32, Attach 54 x 24, Sidebar Hide 52 x 24, and Inspector Hide 58 x 24.

### Reusable presentation patterns

- Dark flat application surface with slightly raised cards.
- Uppercase 9–10 px section labels and subdued metadata.
- Colored status dot plus literal status text.
- Rounded status badges for turn/request state.
- Selected thread row uses blue-tinted fill and left emphasis.
- User input is boxed; Codex narrative is less heavily boxed; tool/activity output is grouped in raised activity cards.
- Empty Inspector pages use a section heading, emphasized empty title, and explanatory secondary copy.
- Long identifiers and paths elide in compact surfaces, with full tooltips or selectable detail in Inspector/dialogs.

## Phase 1 — current implementation mapping

The authoritative target semantics are in `docs/ux-design/threads-turns-configuration.md`. This section only maps current implementation to those requirements.

### Already present

- Thread collection, selection, loading, and canonical historical conversation rendering.
- A New Thread draft and first-prompt thread creation through typed AISuite operations.
- Normal selection of incomplete threads triggers a typed thread read; existing threads are resumed when required before a turn starts.
- Current thread title/ID, workspace, model/provider, status, and load state are visible in existing surfaces.
- Typed start, resume, turn start, and interrupt/Stop flows.
- One prompt composer for first and subsequent turns.
- The public AISuite SDK already exposes many lifecycle operations needed later: start, resume, read, fork, rename, archive, unarchive, remove, and interrupt.

### Present but different from Phase 1 semantics

- **New Thread** currently opens an inline empty draft rather than a dedicated creation dialog.
- The thread is not created until the first prompt is submitted; creation-specific name, Base Instructions, Developer Instructions, and temporary/ephemeral status cannot be edited.
- The composer submits prompts but does not expose the next turn's effective mutable execution configuration.
- Thread selection loads state; resume is primarily an internal send prerequisite rather than a user-visible normal/advanced resume workflow.
- Current model/workspace/provider values are informational rather than the single editable next-turn controls specified by Phase 1.

### Completely missing from the current UI

- Dedicated creation dialog with optional name, Base Instructions, Developer Instructions, and temporary status.
- Editable next-turn Model, Reasoning, Workspace/cwd, Sandbox/access, Approval, Service tier, Reasoning summary, Personality, and Collaboration controls.
- Clear persistence semantics (“this and subsequent turns”) for mutable execution settings.
- Read-only historical effective-configuration presentation for completed turns.
- Advanced Resume with options and foundational-instruction handling.
- Thread context menu actions: Open, Rename, Fork, Interrupt active, Resume with options, Archive/Unarchive, Delete, More/Copy ID.
- User-facing rename, fork, archive/unarchive, delete, and copy-ID flows despite several corresponding SDK methods already existing.

### Likely Qt implementation touch points

Without prescribing a design, the existing ownership boundaries imply that later Phase 1 work would involve:

- `SidebarWidget` for creation entry and thread lifecycle/context actions;
- `ConversationWidget` for the next-turn configuration associated with the composer and historical turn detail affordance;
- `WorkbenchWidget` for coordinating selection, lifecycle actions, persistent next-turn configuration, and dialogs;
- `FrontendSession` for exposing the additional typed AISuite operations and passing explicit typed parameters;
- a focused creation/configuration dialog or panel component if the approved Figma composition requires one.

### Authoritative data/actions still missing or incomplete

AISuite's typed operation parameters already carry much of the necessary write-side data. Thread start/resume/fork and turn start include combinations of base/developer instructions, cwd, model/provider, reasoning effort, approval policy, sandbox policy, service tier, personality, and ephemeral state. Model listing is also available.

The canonical public state does not currently retain enough authoritative read-side information to implement all Phase 1 history and inheritance semantics:

- `ThreadState` exposes ID, title, preview, cwd, model/provider, provider status, load/realtime state, timestamps, and ordered turns, but not the complete current persistent execution configuration or foundational instructions.
- `TurnState` exposes identity, typed status, activity/terminal flags, items, failure, and token usage, but not the complete effective configuration actually used by that historical turn.
- A dedicated typed collaboration-mode setting is not evident in the current thread/turn configuration surface.

Those are contract gaps to resolve before a UI can truthfully display complete inherited and historical configuration. They are not filled by local UI memory in the current application.

## Baseline limits

- Approval and user-input dialogs were source-inventoried but could not be reached naturally in the capture run.
- Plan, Agents, and Changes tabs were captured in their valid empty states; a naturally available contentful example was not present in the isolated run.
- The active streaming screenshot is a real streamed turn, but a static PNG cannot show append cadence, scroll animation, focus transitions, or transient state-coalescing behavior. Those are recorded in the state matrix.
- The capture run used XCB/XWayland solely to obtain deterministic application-window PNGs. The normal desktop session is Wayland, so compositor decorations and subpixel font rendering may differ slightly from a native-Wayland run; widget geometry and application styling are unchanged.

Within those limitations, the source inventory, real-state captures, state matrix, Phase 1 mapping, and exact reproducibility metadata form a sufficient as-built baseline for the next Figma phase.
