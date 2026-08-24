# CodexUI Interaction and Presentation Decisions

This document defines the current CodexUI interaction contract. AISuite and the
Codex app-server own protocol and domain semantics; CodexUI owns only local
presentation, input, selection, and scroll state.

## Thread identity and prompt routing

- The selected thread is identified by its stable app-server thread ID.
- Sending always targets the visibly selected thread. CodexUI validates the
  visible selection before dispatch and never creates a thread as an implicit
  fallback for missing or inconsistent selection state.
- A new thread is created only from an explicit New Thread intent. Its dialog
  captures the workspace, optional name, instructions, and ephemeral state.
- Background thread activity, list refreshes, reconnects, and creation by
  another frontend never change the user's selected thread.
- Selecting a thread that already has materialized turns reuses its retained
  per-thread presentation, including Plan, Agents, and Changes. Automatic
  `thread.read` is reserved for an unmaterialized thread; Reload remains the
  explicit fresh-read action.

## Prompt submission and acknowledgment

Submitting a prompt creates a client-local pending prompt card at the bottom of
the destination thread immediately. The card is gray and its rounded border has
a moving highlight painted by Qt until the app-server acknowledges the
operation.

Pending prompt state is keyed by thread ID and a client-local submission ID. It
therefore remains visible when the user switches threads and returns. On
successful acknowledgment, animation stops and the card becomes a normal user
message as soon as the authoritative app-server item is materialized. Matching
uses stable item identity and prompt content so an older identical prompt is not
mistaken for the submitted item. A failed submission remains visible with an
explicit error state.

The composer is cleared immediately after local admission and remains enabled.
Users may enter additional prompts while earlier prompts await acknowledgment.
CodexUI queues submissions per thread and dispatches them in order: only one
unacknowledged prompt operation is in flight for a thread. After each result,
the next queued prompt is sent using the app-server state produced by the
preceding acknowledgment. Different threads remain independent.

For an explicit new-thread draft, prompts entered while `thread.create` is in
flight remain attached to that draft. When creation succeeds, all pending
prompts move to the returned stable thread ID and are dispatched in order.

## Conversation scrolling

The message view follows incoming content only while it is already at the
bottom. If the user scrolls upward, automatic following pauses so the current
text can be read. Returning to the bottom re-enables following.

This policy applies to new messages, streaming updates, pending prompt cards,
and card reconstruction. It is based on the scroll bar's actual bottom state,
not on turn activity.

Reaching the current bottom re-enables following regardless of how it happens.
This includes direct user scrolling and the range clamp that can occur when a
composer contraction removes trailing scroll space.

The complete center region is wheel- and touchpad-scroll sensitive. Wheel
events over non-scrollable center chrome and the horizontal splitter handles
are forwarded to the message view. A nested scrollable control, such as shell
output, consumes its own events.

## Composer geometry

The upcoming-turn controls are anchored to the bottom of the center pane. The
prompt editor starts at one line, grows upward for multiline input, and stops at
its configured maximum height, after which it scrolls internally.

The message-view layout reserves only the composer's canonical height. When
prompt text, attachments, settings, or attention controls increase that height,
the composer grows upward as an overlay: the viewport keeps its normal geometry
and may be partly covered. An equal-height trailing spacer is added to the
scrollable conversation content so the final card can still be moved into the
visible region with the normal gap above the composer.

Growing this spacer preserves the current scrollbar value and does not move the
messages automatically. Reaching its new maximum re-enables bottom-follow for
subsequent content. When the composer returns to canonical height, the spacer
is removed; Qt may clamp a former bottom position to the reduced range, after
which the normal viewport state and bottom-follow policy apply again.

## Shell-output cards

Shell-output boxes are created only when command output contains visible,
non-whitespace text; commands without presentable output have no empty output
surface. A shown box has no non-content minimum height, grows from zero to a
maximum of 220 pixels, and exposes a styled vertical scrollbar only when content
exceeds that limit. Output follows its bottom while already at the bottom. A
manual upward scroll pauses following until the user returns to the bottom.
Each output card retains its own follow/pause position across conversation-card
reconstruction.

## Inspector and Info presentation

The State and Protocol viewers use the common CodexUI scrollbar styling and
show vertical scrollbars only when needed. The Protocol log occupies the
expanding area of its tab; protocol statistics are displayed below the log.
Protocol and State data are diagnostic presentation only and do not create
domain authority.

## Desktop identity

The application identity is `codex-ui`. The executable, desktop entry,
`StartupWMClass`, application icon name, and installed SVG icon use that same
identity so Linux desktop environments associate the running window with the
correct launcher and taskbar icon.

## Progress indication

Long-running operations need scoped progress presentation rather than a global
busy state. Candidate scopes include prompt acknowledgment, thread creation,
and loading a long thread. Pending prompt acknowledgment already has its own
animated card border. Any additional progress indicator must preserve input and
navigation that can safely remain interactive, identify the operation it
represents, and avoid suggesting that unrelated threads are blocked. No general
spinner contract is defined yet.
