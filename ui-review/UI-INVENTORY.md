# CodexUI Current UI Inventory

## Top bar

- One-line CodexUI lockup: 36-pixel brand mark, equally high application title,
  and current-size "Codex agent workspace" subtitle on the title baseline.
- Workspace breadcrumb.
- Inspector visibility, pending-request attention, controller ownership, and
  connection controls.
- Stable desktop identity through the `codex-ui` application ID and icon.

## Thread sidebar

- App-server thread list with selected, active, pending-request, completed, and
  failed status presentation.
- Explicit New Thread action.
- Per-thread Reload, Rename, Fork, Archive/Unarchive, and Delete context actions.
- Selection is keyed by stable thread ID and is not changed by background
  activity.

## Conversation region

- Thread title, workspace, and status context.
- One transparent section per app-server turn, containing server-ordered user,
  Codex, plan, reasoning, Command execution, file-change, and collaboration
  cards projected from `PresentationModel`.
- Stable keyed in-place reconciliation for authoritative cards and local prompt
  cards; visually identical projections perform no widget or geometry update.
- Per-thread pending prompt cards with muted blue content and a brighter blue
  highlight sweeping left and right until the correlated operation callback,
  followed by a 500-millisecond accepted transition.
- Windowed materialization of long conversations with an explicit Load More
  control.
- Short, interruptible smooth bottom-follow only while the user remains at the
  bottom; paused reading uses a stable visible-card/pixel-offset anchor.
- Wheel and touchpad forwarding from surrounding center chrome and splitter
  handles.

## Command execution output

- Visible card label: **Command execution**.
- No output surface for empty, whitespace-only, ANSI-only, or control-only
  output.
- Read-only monospace output with zero content minimum height.
- Automatic growth to 220 pixels.
- Styled vertical scrollbar beyond the maximum.
- Independent follow-bottom/pause state retained across in-place output updates.

## Upcoming-turn surface

- Model and model-constrained reasoning.
- Workspace, sandbox/access, network, approval, and personality controls.
- Permission profile, reviewer, service tier, reasoning summary, and
  collaboration mode in compact secondary controls.
- Bounded attachment list with custom file selection.
- One-line expanding prompt editor, Send/Steer, and Stop.
- Canonical-height layout reservation plus a dynamic trailing conversation
  spacer when the surface grows over the unchanged message viewport.

## New Thread dialog

- Workspace selection.
- Optional name.
- Optional base and developer instructions.
- Ephemeral lifetime choice.
- Creation is completed through the first admitted prompt.

## Inspector

- **Plan:** structured current plan or authoritative textual plan fallback.
- **Agents:** identified collaboration and subagent activity.
- **Changes:** multi-repository selector, per-file unified diff with
  addition/deletion counts, live filesystem refresh, copy, and
  expanded viewing.
- **Requests:** typed approval and input requests with explicit resolution.
- **Info / State:** retained normalized presentation domains.
- **Info / Protocol:** bounded frame log with the statistics summary below it.

State and Protocol use the common styled, as-needed vertical scrollbars.
Plan, Agents, Changes, and Requests retain their visible per-thread state across
thread and tab navigation.

## Status bar

- Connection state and controller role.
- Selected-thread activity and pending-request summary.
- Model and workspace context.

## Local presentation state

CodexUI locally owns visible selection, drafts, pending prompt cards, per-thread
submission queues, scroll-follow state, nested-output scroll state, splitter
sizes, tab selection, and focus. `PresentationModel` is the sole retained store
for normalized presentation domains; these local interaction values do not
replace AISuite or app-server domain authority.
