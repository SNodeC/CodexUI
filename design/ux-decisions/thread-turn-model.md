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

A pending card is muted blue and has a brighter left-to-right highlight sweep. Its identity is the
stable thread ID plus a client-local submission ID. Thread switching does not
remove or relocate it.

Only one prompt operation per thread is unacknowledged at a time. Further
prompts are admitted and displayed immediately but dispatched in order. This
ensures that a later prompt observes the active-turn state published by the
preceding acknowledgment. Queues belonging to different threads are
independent.

Successful acknowledgment stops the animation. The local card is reconciled
with the new authoritative user item using stable item identity and prompt
content. Failure stops the animation and leaves an explicit error card.

## Start, steer, and interrupt

- An idle loaded thread uses `turn.start`.
- An active thread uses `turn.steer` with the stable active turn ID.
- A not-loaded thread is resumed before starting its turn.
- Stop uses `turn.interrupt` for the stable active turn ID.

CodexUI does not fabricate turns or infer active identity from row position.

## Conversation hierarchy

This accepted presentation rule is independent of the composer-overlay change;
its renderer update remains separate work.

The app-server's authoritative hierarchy supplies the single semantic grouping
level in the conversation: one section per turn, with items retained in their
exact server order. A turn boundary is taken from the stable turn ID and never
inferred from the presence of a visible user-message card; reviews, automation,
or another controlling client may initiate work without a prompt typed in this
window, while Steer adds input to an existing turn.

Consecutive operational items may share a collapsible Activity presentation,
but that disclosure is not another protocol-level object and must not reorder
items or introduce arbitrary visible batching. Pending prompts remain
thread-local presentation cards until acknowledgment supplies their
authoritative turn and item position.

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
