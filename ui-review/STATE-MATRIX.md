# CodexUI UI state matrix

This matrix complements [UI-INVENTORY.md](UI-INVENTORY.md). Screenshot paths are relative to this directory. Every captured state came from the current real CodexUI/AISuite/backend/app-server stack through normal application behavior. No synthetic frontend protocol state was injected.

## Captured canonical states

| State | Screenshot | Reproduction path | Owning Qt classes | Canonical trigger | Determinism and interaction notes |
|---|---|---|---|---|---|
| Disconnected at launch | `screenshots/01-disconnected.png` | Start CodexUI while its dedicated local backend socket is unavailable. | `WorkbenchWidget`, `SidebarWidget`, `ConversationWidget`, `FrontendSession` | `QLocalSocket` connection failure; AISuite connection state is disconnected. | Deterministic. The empty center remains usable only as presentation; Send and controller-dependent work are disabled. Automatic retry status evolves after the static capture. |
| Connected, no thread selected | `screenshots/02-connected-no-thread.png` | Start backend/app-server and CodexUI with no selected thread in the isolated state. Wait for synchronization. | `WorkbenchWidget`, `SidebarWidget`, `ConversationWidget`, `InspectorWidget` | AISuite Ready/Synchronized state with no selected thread. | Deterministic. New Thread is available; conversation and Inspector show explicit empty states. |
| Completed normal conversation | `screenshots/03-thread-normal-conversation.png` | Create a real thread, submit a short prompt, wait for the real app-server turn to complete. | `ConversationWidget`, coordinated by `WorkbenchWidget` | `ThreadUpserted`, `TurnUpserted`, `ItemUpserted`/content changes, then terminal turn state. | Deterministic given a successful app-server response, although response copy varies. The reflected prompt appears only after canonical State; no optimistic duplicate exists. |
| Active streaming turn | `screenshots/04-active-streaming-turn.png` | Submit a real prompt that produces commentary/tool activity and capture before terminal completion. | `ConversationWidget`, `WorkbenchWidget`, `FrontendSession` | Incremental AISuite item/content occurrences and active `TurnState`. | Timing-dependent but repeatedly reproducible. Static capture omits append cadence: changed segments update in place/reconcile, the timeline follows the latest item when already at the bottom, and Stop is enabled while active. |
| High-content/scrollable conversation | `screenshots/05-long-conversation.png` | Use the real active thread after it accumulated a long test-report prompt/response and scroll within the conversation. | `ConversationWidget` | Fully/partially loaded thread with enough ordered segments to exceed viewport height. | Deterministic from retained state. Vertical scrolling is within the central conversation; horizontal scrolling remains disabled. Very large bodies use a bounded plain-text editor. The widget window is capped at 32 turns/256 items. |
| Inspector Info | `screenshots/06-inspector-info.png` | Select a loaded thread, select Info. | `InspectorWidget`, `WorkbenchWidget` | Selected `ThreadState`, latest `TurnState`, synchronization/controller/list facts. | Deterministic. Values are read-only and mostly selectable. The page scrolls independently. It exposes load/completeness facts that are not prominent in the timeline. |
| Inspector Plan, empty | `screenshots/07-inspector-plan.png` | Select Plan when the latest turn contains no retained plan semantic view. | `InspectorWidget` | No applicable typed plan item in selected/latest turn. | Deterministic for this thread. A contentful plan would replace the explanatory empty state; tab selection does not mutate backend state. |
| Inspector Agents, empty | `screenshots/08-inspector-agents.png` | Select Agents when the latest turn contains no retained collaboration/subagent activity. | `InspectorWidget` | No applicable typed collaboration semantic view. | Deterministic for this thread. This is also the Inspector's default tab. |
| Inspector Changes, empty | `screenshots/09-inspector-changes.png` | Select Changes when the latest turn contains no retained file-change semantic view. | `InspectorWidget` | No applicable typed file-change item. | Deterministic for this thread. A contentful state would show canonical change cards and actions supported by existing typed semantics. |
| Backend lost/reconnect | `screenshots/12-error-reconnect.png` | Keep a completed thread selected, stop the real backend, and wait for the socket failure/retry presentation. | `FrontendSession`, `WorkbenchWidget`, status areas in all three panels | Physical local-socket closure and retryable connection-state changes. | Deterministic. Retained immutable conversation state stays visible while controller actions are disabled. Backoff/reconnect copy changes over time; Commands also offers explicit Reconnect. |
| Narrow window | `screenshots/13-narrow-window.png` | Resize the actual app client to its 1100 x 700 minimum. | `MainWindow`, `WorkbenchWidget`, all three panels | Pure Qt resize/layout event; canonical state unchanged. | Deterministic. Sidebar/Inspector minima and the center minimum create a dense three-panel result. Text elides/wraps; each content area retains its own vertical scrolling. |
| Large window | `screenshots/14-large-window.png` | Resize the actual app client to 1800 x 1080. | `MainWindow`, `WorkbenchWidget`, all three panels | Pure Qt resize/layout event. | Deterministic. The center absorbs most additional width because the splitter stretch factors are `0/1/0`; side panels remain bounded. |
| New-thread draft | `screenshots/15-new-thread-draft.png` | Click New Thread and do not submit a prompt. | `SidebarWidget`, `WorkbenchWidget`, `ConversationWidget` | Local draft selection only; no `thread.start` has yet been submitted. | Deterministic. Copy explains that the thread is created by the first prompt and backend defaults are used. Leaving the draft does not create a canonical empty thread. |
| Both side panels hidden | `screenshots/16-panels-hidden.png` | Use Hide in Sidebar and Inspector. | `WorkbenchWidget`, `SidebarWidget`, `InspectorWidget` | Pure presentation visibility state. | Deterministic. Top-bar restore controls appear and the conversation receives the released width. Canonical state is unaffected. |

## Important states not captured

| State | Why no screenshot was manufactured | Responsible UI/source | Canonical trigger and expected behavior |
|---|---|---|---|
| Approval request | A harmless real command in the capture run did not require approval. Forcing a fake pending request would violate the baseline rules. | `InteractiveRequestDialog`, `WorkbenchWidget`, `FrontendSession` | A canonical typed pending command/file or patch/exec approval opens/updates the non-modal dialog. Complete semantics permit positive/negative response; incomplete valid semantics permit negative response only. |
| User-input request | The real app-server reported that `request_user_input` was unavailable in the capture mode. | `InteractiveRequestDialog`, `WorkbenchWidget`, `FrontendSession` | A canonical `request.userInput` pending request presents ordered questions/options/free text, with password echo for secret input and typed submission. |
| Contentful Plan | The real captured turn contained no plan item. | `InspectorWidget` | A typed plan semantic view produces ordered plan steps and statuses. Updates are presentation-key reconciled. |
| Contentful Agents | No subagent/collaboration activity occurred in the captured turn. | `InspectorWidget` | Typed collaboration semantics replace the empty state with agent/activity cards. |
| Contentful Changes | The captured turn produced no retained file-change item. | `InspectorWidget` | Typed file-change semantics produce canonical change presentation. |
| Terminal failed turn | The real capture turn completed normally; deliberately causing an app-server failure was unnecessary. | `ConversationWidget`, `WorkbenchWidget` | Terminal `TurnState` plus failure detail produces failure summary while retaining preceding timeline content. |
| Non-retryable connection failure | The real disconnect used a normal physical backend stop, which is retryable. No incompatible/auth-failing backend was introduced. | `FrontendSession`, `WorkbenchWidget` | Terminal AISuite connection error disables automatic retries, displays the reason, and leaves explicit Reconnect available where meaningful. |
| Thread list truncated/incomplete | The isolated list did not cross the current canonical list bounds. | `SidebarWidget`, `InspectorWidget` | `ThreadListState.complete == false` or truncation metadata is presented truthfully; it must not be mistaken for an exhaustive list. |
| Conversation history window notice | The capture content was long vertically but did not naturally exceed the 32-turn/256-item materialization window. | `ConversationWidget::latestTimelineWindow` | A larger canonical history shows only the latest window and an explicit “latest N of M” notice while preserving full canonical State outside the QWidget tree. |
| Unsupported pending request kinds | No permissions approval, authentication, attestation, dynamic-tool, or MCP elicitation request occurred. | `InteractiveRequestDialog` | These pending kinds remain visible but currently have no complete typed response form in CodexUI. |

## Source-discovered transient states

These transitions are important but a single screenshot is not the most faithful representation.

| State/transition | Steps or trigger | Visible/interactive behavior |
|---|---|---|
| Connecting → synchronizing → ready | Launch CodexUI with an available backend. | Status text and action enablement change as the local socket, Hello/synchronization, controller, and provider become ready. State refreshes are deferred/coalesced rather than rendered inside receive callbacks. |
| Thread not loaded → thread read → loaded | Select a row whose `fullyLoaded` flag is false. | The center shows loading/incomplete facts; `FrontendSession::loadThread` submits one typed read while one is pending. Completion/state update supplies the authoritative conversation. |
| Submit → reflected user message → streaming → final | Enter prompt and press Ctrl+Enter/Send. | Composer is cleared after accepted submission; canonical user-message projection appends; commentary/activity and final response follow. The timeline follows only when the viewport was already following the latest content. |
| User scrolls away during streaming | Scroll upward while an active turn continues. | New state is reconciled without forcibly snapping to bottom; returning near the end restores follow-latest behavior. |
| Interrupt active turn | Press Stop while an active turn is selected. | A typed interrupt operation is submitted. Stop enablement and turn presentation follow canonical active/terminal state rather than local completion assumptions. |
| Switch threads during streaming | Select another Sidebar row while a turn is active. | Selection changes immediately; thread-specific state scopes prevent another thread's prompt/output from appearing in the selected timeline. Returning shows the latest canonical state. |
| Same-ID pending request changes | Backend updates a still-pending request without changing its ID. | Dialog fingerprinting refreshes the shown details and revalidates response safety before submission. |
| Request resolved elsewhere | Another frontend/backend action resolves the currently displayed request. | Canonical pending-request removal disables/closes or advances the dialog; stale positive actions are not sent. |
| Partial socket write | Qt accepts only a prefix of an encoded frame. | No visual state by itself. `FrontendSession` retains the bounded unsent suffix and drains it in order on `bytesWritten`; it does not duplicate a command. |

## Phase 1 states found only in requirements

The following required states from `docs/ux-design/threads-turns-configuration.md` do not exist in the current UI and therefore have no screenshot:

| Required future state | Current absence |
|---|---|
| New Thread creation dialog | New Thread opens an inline draft only; no name, Base Instructions, Developer Instructions, or temporary toggle. |
| Upcoming-turn effective configuration | Composer has no editable Model, Reasoning, Workspace, Sandbox/access, Approval, Service tier, Personality, or Collaboration controls. |
| Changed persistent value for this and later turns | No single editable inherited-value presentation or persistence explanation exists. |
| Historical effective turn configuration | Canonical `TurnState` lacks the complete authoritative configuration needed to render it. |
| Advanced resume with options | Resume is currently an internal operation in the send path, without a user-facing options surface. |
| Thread lifecycle context menu | Rename, Fork, Archive/Unarchive, Delete, More/Copy ID, and contextual Interrupt/Resume actions are not exposed. |

## Capture completeness

Fourteen actual window captures cover connection failure, ready/empty state, draft creation, normal conversation, real streaming, retained high-content scrolling, every Inspector tab, backend loss/retry, both size extremes, and panel visibility. The two security-sensitive interactive request surfaces and several data-dependent Inspector variants are documented from source and canonical trigger paths rather than fabricated.

This is sufficient to hand the current/as-built UI, interaction state model, Phase 1 gap, and exact environment to the Figma phase. Figma should treat uncaptured states above as explicit required variants, not infer their appearance from unrelated screenshots.
