# CodexUI Interaction and Presentation Decisions

This document defines the current CodexUI interaction contract. AISuite and the
Codex app-server own protocol and domain semantics; CodexUI owns only local
presentation, input, selection, and scroll state.

All Qt popup and context menus share one application-level visual contract,
including menus created automatically by text widgets. They use a compact
white rounded surface, neutral border, 30-pixel actions, neutral hover,
blue-tinted checked actions, muted disabled actions, and inset separators.

## Conversation source and structure

`UiSession` owns the sole retained `PresentationModel` for normalized UI state
and projects toolkit-neutral snapshots. The concrete message view consumes its
selected-thread snapshot plus client-local prompt admissions; cards and
inspectors do not access the model or maintain a second domain store. Qt keeps
only renderer mechanics such as scroll anchors, folding, expansion, focus,
geometry, and the editable composer form.

The conversation has one semantic grouping level: an app-server turn contains
its items in server order. When a turn has a prompt, its first You card is the
visible turn container and owns all later cards from that turn. Steering You
cards are nested with the activity they steer rather than starting a second
visual turn. For every turn represented in the retained activity window, the
projection identifies that opening prompt from the complete authoritative turn
and pins it outside the activity budget. History paging therefore never
promotes a later steering You card to turn ownership; loading earlier activity
retains the same root identity without duplication. Authoritative cards are
keyed by stable thread, turn, and item IDs; local prompt cards are keyed by
their submission IDs. The same keyed reconcile path handles initial display
and updates, mutating a card in place when its visible data changes. An
identical visible projection does not rebuild widgets or change geometry.
The complete prompt content, including attachments, is separated from the
first nested turn card by the canonical 8 px structural gap. This spacing is
layout geometry and never becomes part of authored Markdown.

Local prompt admission resumes bottom following when the only pause was caused
by composer overlay growth, so the complete pending prompt becomes visible.
It never overrides a pause created by user scrolling.

Mouse-wheel and touchpad gestures use Qt's native platform/device scroll
handling. CodexUI only records whether the resulting position follows the
bottom or is owned by the user.

## Thread identity and prompt routing

- The selected thread is identified by its stable app-server thread ID.
- The Conversation heading reports `Last activity` from one monotonic
  presentation timestamp. After hydration it starts at the greater of the
  app-server's `updatedAt` and optional `recencyAt`. During the live session,
  meaningful thread-scoped protocol traffic in either direction advances it
  immediately. Selection-driven `thread/read` and `thread/resume` hydration,
  global connection traffic, and catalog traffic do not count as activity.
  Live traffic does not alter thread ordering. Only local prompt admission
  advances the presentation model's effective `updatedAt` and `recencyAt`;
  these local values are not persisted by CodexUI.
- The visible sidebar order contains confirmed root threads only. Minimal
  thread placeholders created by scoped protocol traffic remain retained but
  invisible until an explicit list, read, resume, create, or fork admits them
  as roots. A valued `parentThreadId` immediately assigns structural child
  ownership, so child threads never flash in the root list while later agent
  correlation is pending.
- The sidebar sorts all visible rows by a user-selected criterion. `Recent` is
  the default and uses the app-server's provider-defined `recencyAt` value,
  newest first. `Created` uses `createdAt` newest first, and `Last changed`
  uses `updatedAt` newest first. `Alphanumeric` sorts displayed titles
  case-insensitively with natural number ordering, so 2 precedes 10 and titles
  beginning with numbers precede other titles. Timestamp values that are not
  available sort after timestamped threads. The directions are fixed; the UI
  does not provide a separate ascending/descending control.
- Admitting a prompt immediately advances its root thread group's effective
  `updatedAt` and `recencyAt`, so the ordinary timestamp comparator moves it to
  the first position under `Recent` and `Last changed`. Rapid prompts receive
  strictly increasing timestamp ticks. Stale provider timestamps cannot move
  a locally newer thread backwards; newer provider timestamps reconcile
  naturally. `Created` and `Alphanumeric` remain unaffected.
- The Plan inspector preserves app-server step states while the owning turn is
  active. If a stale step still reports `inProgress` after its owning turn or
  thread becomes terminal, the display reconciles that step to `completed`,
  Failed, or Interrupted. Pending steps remain Pending, and retained protocol
  data is not rewritten.
- Each visible thread is presented as a compact card. Its status indicator is
  part of that card, and hover and selection strengthen the same card surface
  instead of introducing a separate row treatment. The Sort and Transport
  controls use the same centered chevron and compact text-to-indicator spacing
  as the prompt settings.
- A left click selects a thread and changes the displayed conversation. A
  right click opens actions for the pointed-to card without changing the
  selected thread or displayed conversation. That card retains its hover
  treatment until the non-blocking menu closes; dismissing the menu does not
  replay the closing click into another control.
- Sending always targets the visibly selected thread. CodexUI validates the
  visible selection before dispatch and never creates a thread as an implicit
  fallback for missing or inconsistent selection state.
- A new thread is created only from an explicit New Thread intent. Its dialog
  captures the workspace, optional name, instructions, and ephemeral state.
  In the browser, the workspace is an app-server-local path entered as text;
  browser file pickers cannot truthfully select an arbitrary server directory.
- Background thread activity, list refreshes, reconnects, and creation by
  another frontend never change the user's selected thread. Completion of a
  locally started creation likewise selects the returned thread only while
  its optimistic draft remains visibly selected; later navigation is
  preserved while the draft's queued prompts continue independently.
- Selecting a thread hydrates it once per bridge connection even when the
  discovery result already contains an active turn. The full read is merged
  into the retained per-thread presentation, so live Plan and Agents state
  cannot be erased by an incomplete reconstruction. Reload remains the
  explicit forced fresh-read action.

## Prompt submission and acknowledgment

With the prompt editor focused, Enter (including keypad Enter) submits through
the same admission path as the Send button. Shift+Enter always inserts a new
line, including when Control or Meta is also held; Control+Enter and Meta+Enter
remain submission aliases, while Alt+Enter does not submit. Auto-repeated Enter
events and Enter used to confirm an active input-method composition never
submit a prompt. Auto-repeat is consumed instead of inserting an accidental
newline. Send and Steer are enabled only when admission is available and the
draft contains non-whitespace text. Focus uses the canonical blue composer
border without changing its geometry. Whitespace is used only for admission
validation: the exact authored text, including intentional leading and trailing
space and blank lines, is passed to the submission path unchanged.

Submitting a prompt creates a client-local pending prompt card at the bottom of
the destination thread immediately. The card begins with the calm blue
user-card treatment and an emphasized border. If the correlated app-server
result has not arrived after one second, a brighter blue highlight begins
sweeping left and right.
Ordinary attached files appear as local Markdown links at the bottom of that
card from its first frame. The same composed Markdown is sent to app-server and
retained by the authoritative user message, so acknowledgment does not reflow
the attachment presentation. Filename URL delimiters such as `#` and `?` are
encoded as path content rather than being misread as a fragment or query.

Each pending prompt has a process-wide client-local submission ID and remains
associated with its destination thread. It therefore remains visible when the
user switches threads and returns. Successful acknowledgment or definitive
failure stops delayed feedback immediately. The one-second timer controls only
whether pending feedback is visible; it never acknowledges or promotes the
prompt. If the authoritative app-server item arrives before or after the
result, it inherits the pending card's stable visual anchor and replaces it as
soon as both correlation and acknowledgment are complete. Only the correlated
`turn.start` or `turn.steer` completion callback acknowledges a prompt;
conversation events never infer acknowledgment. Each operation carries a
unique `clientUserMessageId`, which binds the authoritative user item without
confusing identical prompt text. A failed submission remains visible with an
explicit error state.

A prompt that starts a turn is the outer soft-blue turn card. A prompt admitted
through `turn.steer` appears immediately inside the active turn as a calm teal
`You` card with a right-aligned `steering` specialization. It uses the same
one-second delayed-feedback rule as the outer card. After
acknowledgment, the same widget becomes a soft-teal inset steering card with
the canonical teal border and title treatment.
No optimistic card is exchanged for a second widget, and the turn grows around
it without changing existing nested card identity.

At acknowledgment, the retained outer You card immediately uses the stronger
static blue running border. That border belongs to the card across its local-
prompt-to-authoritative-message morph while pending feedback stops; it
has no animation, glow, shading, or geometry change. A successful `turn.start`
result retains active ownership until the separate authoritative lifecycle
catches up, so the optimistic-to-running handoff has no neutral-border frame.
Completion restores the canonical border in place.

The composer is cleared immediately after local admission and remains enabled.
Users may enter additional prompts while earlier prompts await acknowledgment.
Unsubmitted composer text and attachments form one shared local draft: ordinary
thread navigation retains them, and submission sends them to the thread that is
visibly selected at that moment. Explicit new-thread creation still starts with
a deliberately cleared composer.

Accepting New Thread immediately inserts one selected orange animated row in
the thread list. It is a presentation-only draft, not a synthetic app-server
thread. Sending the first prompt promotes the same row to the ID returned by
`thread/start`; animation continues until that prompt's `turn/start` callback
succeeds, then the same row adopts canonical styling. Creation or first-prompt
failure stops animation and leaves the row visibly failed. No duplicate row or
replacement transition is permitted.
CodexUI queues submissions per thread and dispatches them in order: only one
unacknowledged prompt operation is in flight for a thread. After each result,
the next queued prompt is sent using the app-server state produced by the
preceding acknowledgment. Different threads remain independent.

Submission waits until the destination thread has completed its connection-
generation hydration. A provider-marked `notLoaded` thread is resumed before
the turn operation. If a submission still receives a transient thread-not-found
result, CodexUI performs one bounded resume-and-retry; a repeated failure is
shown on the pending card rather than retried indefinitely. If hydration has
failed, submission is rejected without clearing the composer draft; Reload
must succeed before that prompt can be admitted. A disconnect after admission
leaves the pending card in place; a dispatched prompt is returned to its queue,
and bridge-open re-drives queued work only after fresh thread hydration. Real
app-server operation failures remain terminal. An active resume prevents a
concurrent hydration read or turn operation for the same thread.

For an explicit new-thread draft, prompts entered while `thread.create` is in
flight remain attached to that draft. When creation succeeds, all pending
prompts move to the returned stable thread ID and are dispatched in order.

Authoritative user-message text is rendered as Markdown through the same safe
`MarkdownNoHTML` path as agent messages. The locally admitted prompt remains a
plain-text transitional card until its authoritative item arrives.

Generated-image items show the app-server-saved image as a bounded thumbnail.
Selecting it opens the shared non-modal image viewer; encoded image data is
never displayed as generic activity text.
Multiple message attachments retain source order in one horizontal ribbon.
The ribbon never wraps or grows the card beyond its available width; horizontal
overflow appears only when required, while its vertical size remains bounded by
the tallest thumbnail. A standard 1 px neutral border, 6 px radius, 4 px inner
padding, and canonical dark surface enclose both thumbnails and scrollbar.
Thumbnails are vertically centered so each has equal top and bottom clearance. The
ribbon remains subordinate content of its existing card, never a nested card.
Available thumbnails are named keyboard targets and open on mouse release
inside the thumbnail or with Enter or Space;
unavailable placeholders remain announced but are not focusable. Markdown
links in Conversation and Inspector content are reachable by keyboard.

User messages use the canonical soft-blue identity surface. Final Codex
messages use the canonical soft-violet identity surface, while interim Codex
updates remain neutral and identify their phase in the header. Process cards
also remain neutral so they support rather than dominate the primary exchange.
Their lifecycle status is a normal-weight lowercase value at the right of the
header, immediately before Copy, and uses canonical semantic state colors.
An `imageView` item is completed once it materializes; generated-image and
fallback activity cards retain any lifecycle state supplied by the app-server.
Thread rows, conversation metadata, Inspector entries, and process cards share
the same vocabulary: `running`, `completed`, `failed`, `interrupted`, `pending`,
and `not loaded`. Command exit
code, cwd, and duration; file totals; and agent execution context remain below
the primary content. Generated-image cards have no duplicate body-status row.

Every conversation card with visible detail uses the same keyboard-focusable
disclosure chevron: down when expanded and left when collapsed. Title-only
cards, including Reasoning without a public summary, omit the chevron until
detail arrives.
You, Codex, temporary You, Command execution, and Image cards initially render
expanded; Reasoning, File changes, Agent activity, Plan, and fallback activity
cards initially render collapsed. A user-selected state survives
streaming updates, authoritative prompt replacement, and thread switching for
the lifetime of the CodexUI process.

The outer You turn card is itself foldable. Collapsing it hides the complete
nested turn; expanding it restores every child with its independently retained
fold state. Inner disclosure gestures retain their existing title-anchor and
growth rules.

Every card with copyable content places a backgroundless copy icon at the right
of its header, immediately before the disclosure chevron with the canonical
compact 4 px action gap. Both icons share one vertical center; tooltip and
accessible text provide the action label. Copy remains available while that
card is collapsed; contentless cards omit it. Markdown cards copy their exact
retained source as both plain clipboard text and `text/markdown`, never
reconstructed rendered text. Structured cards copy a deterministic plain-text
representation of their primary content. After a successful write, only the
copy glyph quickly morphs into the canonical green check, remains a check for
1.5 seconds, and morphs back without moving the header. A rounded,
non-layout-shifting `Copied` overlay appears at the action. Web clipboard
failure keeps the copy glyph and uses the same local overlay with canonical
error styling; reduced-motion mode makes the icon transitions immediate
without suppressing the result.

Message specializations (`steering`, `update`, and `final answer`) use normal
font weight and sit at the right of the header immediately before Copy. The
title remains at the left; no separator glyph is rendered.

Pending-request dialogs validate required answers and structured MCP content
before accepting the modal. Invalid input keeps the dialog and all entered
content open for correction.

The native and web Conversation headers expose persistent, matching icon-only
controls for Reasoning visibility, interim Codex-update visibility, and the
initial folding state of newly appearing Command execution and Image cards. Final Codex
answers are never filtered. Visibility is a presentation choice only: filtered
cards remain in the retained projection, continue accepting updates, and reappear
with their latest content and user-owned folding state. Changing the Command
preference never refolds an existing card.
Browser persistence is an optional convenience: unavailable or denied local
storage falls back to canonical defaults and never prevents the UI from
starting or accepting preference changes.
Web thread refresh, rename, fork, archive, and delete actions are single-flight.
Mutation controls require current controller readiness, remain disabled while
their operation is pending, and report operation failures through the canonical
notice surface.
Transient notices overlay the conversation workspace in native and WebUI. Their
appearance, timeout, and dismissal never resize or reposition message content.

Folding is an explicit geometry transaction. Collapsing keeps the selected
title row fixed while the natural scroll range permits and shifts following
cards upward. Expanding grows downward when the complete card remains visible;
otherwise the viewport scrolls only enough to reveal it, so the title may move
upward. The visible boundary excludes any extra composer height currently
overlaying the conversation. The gesture pauses follow-latest. At the lower
scroll limit, the viewport accepts the natural clamp instead of retaining
artificial blank space.

When enabled, Reasoning items remain stable progress cards even when the
app-server provides no public summary; later content updates the same retained
card in place.
File-change cards list each supplied path and change kind and derive compact
addition and deletion totals from the supplied per-file unified diffs. They do
not duplicate the full review surface owned by the Changes inspector. Optional
Command duration and Agent model, reasoning effort, child identity, path,
sender, and receivers are shown only when app-server supplied them.

Textual `plan` items remain conversation content. Structured
`turn/plan/updated` state is shown only in the Inspector Plan tab, avoiding a
duplicate representation in the conversation. Its typed conversation key,
conversion, placement, and renderer remain implemented behind a disabled
projection switch so this policy can be reactivated narrowly if required.

## Conversation scrolling

The message view smoothly follows incoming content only while it is already at
the bottom. Consecutive geometry changes retarget one short, monotonic animation
to the newest bottom. If the user scrolls upward, the animation stops
immediately and automatic following pauses so the current text can be read.
Returning to the bottom re-enables following.

Follow/pause mode and the visible-card/pixel-offset anchor are retained per
thread and restored when the user switches back.
Scrollable Command text and output own a wheel or touchpad gesture that begins
while they can move in its direction. Reaching a boundary during that gesture
does not chain into the outer message view. A fresh outward gesture begun at an
already-reached boundary scrolls the conversation instead.

This policy applies to new messages, streaming updates, pending prompt cards,
and card reconstruction. It is based on the scroll bar's actual bottom state,
not on turn activity.

While following is paused, CodexUI anchors the first visible card and its pixel
offset. Appends below the viewport keep the scrollbar value unchanged; card
reflow or reconstruction restores that visual anchor after Qt completes layout.
Incoming data therefore cannot move the user's reading position merely because
content above or below it changed size. Protocol updates that do not change a
card's visible projection do not rebuild that card. Multiple visible card
changes from one refresh are applied as one paint-suppressed layout transaction
with one anchor restoration, including streaming Command execution updates.
Incoming deltas are coalesced to at most one reconcile per display interval;
growing text and Command execution output are appended in place instead of
being recopied and rebuilt for every delta.
New authoritative cards are inserted at their server-ordered position without
reconstructing retained cards. While following is paused, the effective history
window expands with incoming cards so its visible anchor is not evicted; the
requested bound is restored after following resumes.

User scrolling to the current bottom re-enables following. A generic Qt range
clamp caused by card reflow does not count as user intent and cannot silently
re-enable following. Composer contraction is the explicit exception: after its
trailing space is removed, CodexUI recomputes whether the resulting clamped
position is the new bottom.

The complete unobscured center region is wheel- and touchpad-scroll sensitive.
Wheel events over non-scrollable conversation chrome and the horizontal
splitter handles are forwarded to the message view. A gesture begun in the
prompt editor or turn-settings surface is always consumed by that composer
region and never scrolls the conversation behind it. Command text and output
retain a gesture that started while they could scroll; only a fresh gesture
begun at their current boundary is handed to the conversation.

## Composer geometry

The upcoming-turn controls are anchored to the bottom of the center pane. The
prompt editor starts at one line, grows upward for multiline input, and stops at
its configured maximum height, after which it scrolls internally. While all
content fits, its hidden scrollbar is clamped to the top so a fully visible
multiline draft cannot be displaced by a trailing blank-line offset. The
compact-to-multiline transition is decided by an invisible `QTextLayout` using
the editor's exact compact content width, including its document margins. The
live document is never resized for measurement, and its height is updated only
after the grid switch, so the first wrapping character moves directly into the
expanded grid without an intermediate row.

The message-view layout reserves only the composer's canonical height. When
prompt text, attachments, settings, or attention controls increase that height,
the composer grows upward as an overlay: the viewport keeps its normal geometry
and may be partly covered. An equal logical trailing extent is added to the
scrollable conversation content so the final card can still be moved to the
overlay boundary. The visible conversation scrollbar track is inset by the same
extra height, so its lower endpoint remains at the uncovered message boundary
rather than disappearing beneath the composer. Its value, range, and anchoring
semantics remain those of the full conversation. The conversation owns no
permanent bottom padding; the moving composer uses the canonical Changes-tab
treatment of 8 px space, a standard divider extending 10 px beyond the adjacent
content on each side, and another 8 px space. This boundary remains identical
whether the conversation is at its bottom or paused higher in history.

Growing this extent preserves the current scrollbar value and does not move the
messages automatically. Reaching its new maximum re-enables bottom-follow for
subsequent content. When the composer returns to canonical height, the extent
is removed; Qt may clamp a former bottom position to the reduced range, after
which the normal viewport state and bottom-follow policy apply again.

## Command execution cards

The card's visible label is **Command execution**.

Command execution output boxes are created only when output contains printable,
non-whitespace text after terminal control sequences are ignored; empty,
whitespace-only, and ANSI/control-only output has no output surface. A shown box
has no non-content minimum height, grows from zero to a maximum of 220 pixels,
and exposes a styled vertical scrollbar only when content exceeds that limit.
The command surface uses the same content-height behavior with its existing
90-pixel maximum. Trailing empty lines are omitted from both displayed texts.
Executed command text opens at its beginning and never follows its bottom;
tail-following belongs only to the streaming output surface.
Their wrapped content height is measured at the final viewport width during the
outer layout transaction. While the conversation follows its bottom, streaming
output growth holds the card bottom and metadata in place and expands upward.
Streaming output, completion status, and metadata update the retained outer
Command execution card in place; they do not replace it. Output follows its
bottom while already at the bottom. A manual upward scroll pauses following
until the user returns to the bottom. Each output card retains its own
follow/pause position across in-place output updates.
When retained stream text exceeds its canonical byte budget, the card shows an
explicit omitted-byte notice followed by the newest retained tail. The same
notice is included when copying the card, so bounded history is never presented
as the complete command output, response, reasoning, or plan text.

## Inspector and Info presentation

The State and Protocol viewers use the common CodexUI scrollbar styling and
show vertical scrollbars only when needed. The Protocol log occupies the
expanding area of its tab; protocol statistics are displayed below the log.
Protocol and State data are diagnostic presentation only and do not create
domain authority. Plan, Agents, and Requests use retained per-thread
presentation snapshots. Agent cards start collapsed and expose status, copy,
and fold actions in that order; folding changes presentation only and never
discards agent content. Changes instead resolves local Git repositories upward
from the selected thread's retained command working directories and refreshes
them asynchronously through libgit2. When several repositories match, All
repositories is the default and a selector can narrow the view. Resolution
considers only repositories reached through visible directory paths by
default. The persistent Hidden option also includes paths containing
dot-prefixed directories. When identical hinted paths occur in several
repositories, repositories where the path is currently changed are preferred
over clean tracked matches. Changes offers Unstaged, Staged, and Since HEAD
scopes; a thread without a resolvable
repository shows an explanatory unavailable state without preventing normal
work. Manual and Codex-created changes are treated identically.

The Inspector shows a compact unified preview. Open review and double-clicking
a changed file open a modeless review window with Unified or Side by side
layout and Compact or Expanded context. These view preferences persist across
threads. The changed-file list ends with a compact footer containing the file
count and semantic green/red totals; a standard gray divider separates that
selection area from the preview. Diff scrollbars show proportional overview
marks using canonical green for additions, red for deletions, and blue for hunk
boundaries. Existing changed files and their parent directories are watched;
reverted or restored files disappear after libgit2 confirms they are clean,
while a short visible-only refresh discovers new untracked files and catches
index-only changes.

## Desktop identity

The application identity is `codex-ui`. The executable, desktop entry,
`StartupWMClass`, application icon name, and installed SVG icon use that same
identity so Linux desktop environments associate the running window with the
correct launcher and taskbar icon.

## Progress indication

Long-running operations need scoped progress presentation rather than a global
busy state. Candidate scopes include prompt acknowledgment, thread creation,
and loading a long thread. Overdue prompt acknowledgment already has its own
delayed highlight sweep. Any additional progress indicator must preserve input
and navigation that can safely remain interactive, identify the operation it
represents, and avoid suggesting that unrelated threads are blocked. No general
spinner contract is defined yet.
