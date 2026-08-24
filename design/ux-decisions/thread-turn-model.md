# CodexUI UX Decision: Threads, Turns, and Prompt Admission

Status: **Implemented design baseline**

## Domain model

CodexUI presents the current Codex app-server model directly:

```text
Thread
├── stable identity and lifetime
├── foundational instructions
├── current upcoming-turn configuration
└── Turns
     ├── user input and attachments
     ├── activity and output
     └── completion or failure
```

AISuite and the app-server are authoritative for thread, turn, item, and
configuration semantics. CodexUI retains only bounded presentation state and
client-local interaction state.

## Thread selection and routing

The visible thread selection is user-owned. Background events, reconnects,
thread refreshes, and activity in another thread do not change it.

Send and Steer resolve the destination from the visibly selected row and its
stable thread ID. Missing or inconsistent selection is an error; it never
causes implicit thread creation. New thread creation requires an explicit New
Thread intent.

## New thread

New Thread opens a dialog for creation-only properties:

- workspace;
- optional name;
- optional base instructions;
- optional developer instructions;
- ephemeral lifetime.

The accepted dialog creates a local draft. The app-server thread is created
when the first prompt is admitted. Prompts entered while creation is in flight
remain attached to the draft and move to the returned thread ID when creation
succeeds.

## Upcoming-turn configuration

The single editable settings surface is adjacent to the composer. It contains
the supported model, reasoning, workspace, sandbox/access, network, approval,
personality, service-tier, reasoning-summary, permission-profile, reviewer, and
collaboration choices.

Thread-creation properties are not duplicated in this surface. Untouched
settings remain omitted from native operations so CodexUI does not replace
provider state with inferred defaults.

## Prompt admission and acknowledgment

Clicking Send or Steer performs local admission immediately:

1. create a pending user card in the destination thread;
2. clear the submitted prompt and attachments from the composer;
3. leave the composer enabled for more input;
4. dispatch the operation when it reaches the front of that thread's queue.

A pending card is muted blue and has a brighter highlight sweeping left and
right. Its visual identity is a process-wide local submission ID, so assigning
an app-server thread ID to a creation draft and switching threads do not replace
or relocate it.

Only one prompt operation per thread is unacknowledged at a time. Further
prompts are admitted and displayed immediately but dispatched in order. This
ensures that a later prompt observes the active-turn state published by the
preceding acknowledgment. Queues belonging to different threads are
independent.

Only the correlated `turn.start` or `turn.steer` completion callback can
acknowledge a prompt. A successful callback begins a 500-millisecond accepted
transition. Every submission carries a unique `clientUserMessageId`, allowing
the authoritative user item to inherit the local card's stable visual key even
when multiple prompts have identical text. Failure stops the animation and
leaves an explicit error card.

Prompt dispatch waits for once-per-connection-generation thread hydration. A
provider-marked `notLoaded` thread is resumed first. A transient
thread-not-found submission result triggers one resume-and-retry; a repeated
failure becomes the card's terminal error. Failed hydration leaves the composer
draft intact and requires an explicit reload before admission. Dispatch
rechecks connection and recovery ownership at its queued execution boundary, so
a disconnect cannot send and an in-flight resume cannot overlap a hydration
read or another turn operation.

## Start, steer, and interrupt

- An idle loaded thread uses `turn.start`.
- An active thread uses `turn.steer` with the stable active turn ID.
- A not-loaded thread is resumed before starting its turn.
- Stop uses `turn.interrupt` for the stable active turn ID.

CodexUI does not fabricate turns or infer active identity from row position.

## Conversation hierarchy

The app-server's authoritative hierarchy supplies the single semantic grouping
level in the conversation: one section per turn, with items retained in their
exact server order. A turn boundary is taken from the stable turn ID and never
inferred from the presence of a visible user-message card; reviews, automation,
or another controlling client may initiate work without a prompt typed in this
window, while Steer adds input to an existing turn.

Operational items remain individual cards inside their turn; there is no
second Activity batch or arbitrary visible grouping. Pending prompts remain
thread-local presentation cards until acknowledgment supplies their
authoritative turn and item position.

`PresentationModel` is the retained normalized source. A pure projection adds
local prompt admissions and emits stable keyed turn sections and cards. Initial
display and all updates use the same reconcile path; retained card widgets are
mutated in place, and a visually identical projection performs no layout work.

## Thread lifecycle actions

The thread context menu operates on the stable ID under the pointer and offers
Reload, Rename, Fork, Archive/Unarchive, and Delete. Opening the menu does not
change the selected thread. Mutating actions require controller authority.

## Conversation and composer layout

The conversation is independently scrollable above a bottom-anchored settings
and composer surface. The message viewport reserves the surface's canonical
height. Further composer growth overlays the viewport without resizing it. A
matching trailing spacer in the conversation supplies enough additional range
to scroll the final message above the overlay with the normal visual gap.

Composer growth preserves the current reading position even when it was at the
old bottom. Reaching the extended range's new bottom enables automatic
following again. Composer contraction removes the spacer; a Qt range clamp at
the former bottom is accepted. Incoming content follows with a short,
interruptible animation only while the conversation is at its current bottom.
While following is paused, the first visible stable card and its pixel offset
anchor the reading position across appends, card reflow, and reconstruction.
