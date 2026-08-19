# CodexUI UX Decision: Thread and Turn Model

Status: **Agreed design baseline**

This document records the current UX decisions for thread creation, persistent execution settings, turns, resume/fork behavior, and thread actions. It is intended as source material for the upcoming Figma reconciliation and redesign work.

## 1. Core mental model

CodexUI distinguishes four concepts:

```text
THREAD
|
+-- Creation properties
+-- Foundational instructions
+-- Current execution settings
+-- Turns
    +-- turn-specific input/settings
    +-- historical effective settings
```

The central UX rule is:

> **Every mutable execution setting has exactly one editable representation: at the next-turn composer.**

There are no separate editable "thread reasoning" and "turn reasoning" controls.

The authoritative values are persistent thread settings, but they are presented beside the next-turn composer because that is the point at which the user needs to understand or change what the next Codex turn will use.

## 2. New Thread

`+ New Thread` opens a fresh conversation immediately. There is no mandatory configuration dialog.

Conceptually:

```text
NEW THREAD

+----------------------------------------------------+
| What do you want Codex to work on?                 |
|                                                    |
+----------------------------------------------------+

Model | Reasoning | Workspace | Access | Approval

> Advanced thread options

                                             Start
```

The initial values come from the **Codex/app-server defaults**. CodexUI does not introduce artificial concepts such as "Starting configuration" or "Initial reasoning".

The controls beside the composer show what the **first turn will use**. They may be changed before pressing **Start**.

## 3. Advanced Thread Options

Only genuine thread-creation/context properties belong here:

```text
Advanced thread options

[ ] Temporary thread
    Do not persist this thread to history

Base instructions
Default                                      Edit...

Developer instructions
Default                                      Edit...
```

Protocol fields are not exposed merely because they exist.

## 4. Foundational instructions

CodexUI distinguishes:

```text
Base instructions
    -> Fundamental behavior of the Codex agent

Developer instructions
    -> Project/workflow/architecture/testing rules

User prompt
    -> Concrete task
```

For normal use, Base and Developer Instructions inherit the Codex/app-server defaults.

They may be explicitly changed during:

- New Thread
- Fork...
- Resume with options...

They are not ordinary next-turn execution settings.

For an already active thread they are informational/read-only unless the user explicitly chooses an operation that permits changing them.

## 5. Next-turn configuration

Every composer, including the first one, shows the effective execution configuration that the next turn will use.

Conceptually:

```text
+----------------------------------------------------+
| Ask Codex...                                       |
|                                                    |
+----------------------------------------------------+

Model | Reasoning | Workspace | Access | Approval

                                                Send
```

Persistent mutable execution settings include, where supported and useful in the UI:

- Model
- Reasoning effort
- Workspace / cwd
- Sandbox / workspace access
- Approval policy / reviewer
- Service tier
- Reasoning summary
- Personality
- Collaboration mode

The final Figma design will decide which settings deserve first-level visibility and which belong under a compact secondary control such as `More...`.

## 6. Meaning of changing a next-turn setting

Suppose the current inherited reasoning value is `High` and the user selects `XHigh` before the next turn.

The meaning is:

```text
Current inherited value
        High
          |
          v
user selects XHigh
          |
          v
Next turn uses XHigh
          +
XHigh becomes the inherited value
for subsequent turns
```

The UI should make this persistence rule discoverable, for example through a tooltip:

> Changes apply to this and subsequent turns.

The control is positioned at the next-turn boundary because that is when the value matters to the user, even though the resulting value is stored persistently with the thread.

## 7. True turn-specific data

These values belong only to one turn and do not become inherited thread configuration:

- Prompt / UserInput
- Attachments, images, and other turn input
- Output schema
- Client message identity

Conceptually:

```text
NEXT TURN
|
+-- inherited execution settings
|   +-- Model
|   +-- Reasoning
|   +-- Workspace
|   +-- Access
|   +-- Approval
|   +-- ...
|
+-- turn-local data
    +-- Prompt
    +-- Attachments
    +-- Output requirements
```

## 8. Historical turns

Historical turns should expose the **effective settings they actually used**, where the Codex app-server and AISuite can provide them.

These values are read-only:

```text
TURN DETAILS

Model        GPT-5.x Codex
Reasoning    XHigh
Workspace    ~/AISuite
Access       Workspace Write
Approval     On Request
```

This permits truthful history even after the current thread configuration has changed:

```text
Turn 12    High
Turn 13    High
Turn 14    XHigh
Turn 15    XHigh

Current next-turn setting:
           Medium
```

A reconnected CodexUI should ideally receive these historical effective values from authoritative app-server/AISuite state rather than infer them from the current thread settings.

Historical effective settings are never editable.

## 9. Thread header

The thread header should identify the conversation and workspace clearly:

```text
AISuite Performance
~/Projects/SNodeC/AISuite
```

It may also contain a quiet read-only summary such as:

```text
GPT-5.x Codex | XHigh | Workspace Write
```

Such a summary must not look like a second set of editable controls. The single editable execution controls remain beside the next-turn composer.

## 10. Normal open / resume

Selecting an existing thread should simply work:

```text
click thread
    |
    v
load/resume if necessary
    |
    v
conversation appears
```

There is no normal resume dialog. Whether CodexUI internally performs `thread/resume` is an implementation detail that should normally remain invisible.

Existing foundational instructions and thread context are retained.

## 11. Thread context menu

The context menu is state-dependent.

### Idle thread

```text
Open
Rename...
Fork...
--------------------
Resume with options...
--------------------
Archive
Delete...
```

### Running thread

```text
Open
Rename...
Fork...
--------------------
Interrupt
--------------------
Resume with options...      if applicable
--------------------
Archive                     possibly disabled while running
Delete...                   possibly disabled while running
```

### Archived thread

```text
Open
Fork...
--------------------
Unarchive
Delete...
```

Developer-oriented utilities may live under a secondary menu rather than cluttering the primary menu:

```text
More
+-- Copy Thread ID
```

## 12. Open

`Open` is the normal operation.

If necessary, CodexUI automatically resumes the thread using its existing context and settings. No configuration interaction is required.

## 13. Rename

`Rename...` changes the human-readable thread title. It does not affect execution state, instructions, or Codex context.

## 14. Fork

`Fork...` creates a **new thread derived from the selected thread**.

Because it creates a new thread, foundational context becomes editable again:

```text
FORK THREAD

Base instructions
[ editable ]

Developer instructions
[ editable ]

[ ] Temporary thread

                             Cancel   Fork
```

The new fork then uses the normal next-turn composer and its execution-setting controls.

Forking is the preferred way to continue historical context under changed foundational instructions because it creates a clean semantic boundary.

## 15. Resume with options

`Resume with options...` is an explicit advanced operation:

```text
RESUME THREAD WITH OPTIONS

Base instructions
[ editable ]

Developer instructions
[ editable ]

                         Cancel   Resume
```

It permits the same historical thread to continue under modified foundational instructions.

This is considered an **expert operation**. Normal `Open` resumes using the existing context without interruption.

## 16. Interrupt

`Interrupt` is visible when the thread has a running turn.

```text
Thread A       Working...
    right-click
        |
        v
    Interrupt
```

This is important because multiple Codex threads can be running concurrently. The user should not have to switch to a thread merely to stop its active work.

## 17. Archive / Unarchive

`Archive` is the normal non-destructive way to remove completed threads from the active workspace.

```text
Active Threads
    |
    +-- Archive --> Archived Threads
```

Archived threads remain discoverable and expose `Unarchive` in their context menu.

Archive should generally be preferred over Delete.

## 18. Delete

`Delete...` is destructive and visually separated from ordinary actions. It requires explicit confirmation, for example:

> Permanently delete "AISuite Performance"?

If deletion is unsafe while a turn is running, it should be disabled rather than implicitly interrupting and deleting.

## 19. Temporary threads

Temporary/ephemeral behavior is a creation-time property:

```text
[ ] Temporary thread
    Do not persist this thread to history
```

It belongs under Advanced Thread Options because it changes the lifetime and persistence semantics of the thread. It is not a next-turn setting.

## 20. Overall lifecycle

```text
                 + NEW THREAD
                       |
                       v
              app-server defaults
                       |
              optional Advanced
              +-- Temporary
              +-- Base instructions
              +-- Developer instructions
                       |
                       v
                  FIRST TURN
              +-----------------+
              | Prompt          |
              | Model           |
              | Reasoning       |
              | Workspace       |
              | Access          |
              | Approval        |
              +-----------------+
                       |
                     Start
                       |
                       v
                    THREAD
                       |
                       v
                  NEXT TURN
              +-----------------+
              | Prompt          |
              | current Model   |
              | current Reason. |
              | current Access  |
              | ...             |
              +-----------------+
                       |
                optionally change
                       |
                     Send
                       |
                       v
             changed values become
             current inherited values
                       |
                       v
                  NEXT TURN...
```

Thread actions are orthogonal to that turn lifecycle:

```text
Existing Thread
       |
       +-- Open
       |     +-- automatic resume
       |
       +-- Rename
       |
       +-- Fork
       |     +-- new thread
       |         foundational instructions editable
       |
       +-- Resume with options
       |     +-- same thread
       |         foundational instructions editable
       |
       +-- Interrupt
       |     +-- active turn only
       |
       +-- Archive / Unarchive
       |
       +-- Delete
```

## 21. Design principles carried into Figma

The Figma redesign should preserve these principles:

1. **One editable control per mutable execution setting.** Do not duplicate Model, Reasoning, Access, etc. at thread and turn level.
2. **Place mutable execution settings at the next-turn boundary.** This is where users need to understand and modify them.
3. **Use Codex/app-server defaults for new threads.** Avoid an unnecessary creation-configuration step.
4. **Keep creation-only and foundational options progressive.** Temporary mode and foundational instructions belong under Advanced Thread Options.
5. **Historical truth is read-only.** A historical turn may display its effective execution settings but never edits them.
6. **Normal resume is invisible.** Opening an existing thread should not present protocol mechanics to the user.
7. **Fork is the preferred semantic boundary for changed foundational instructions.** Resume with Options remains available for expert use.
8. **Thread actions are state-dependent.** Interrupt, Archive/Unarchive, and destructive operations appear only when meaningful and safe.
9. **Archive before delete.** Non-destructive lifecycle management should be easier than permanent deletion.
10. **Protocol capability does not automatically imply UI exposure.** Every control must justify itself as a useful user-facing concept.

## 22. Open design questions

The following are intentionally left for later UX/Figma exploration:

- Exact visual layout of the next-turn execution controls.
- Which execution settings remain permanently visible versus move under `More...`.
- Exact thread-header presentation and how much current configuration it summarizes.
- Historical turn-detail presentation and which effective settings are useful enough to show.
- Archived-thread discovery/filtering UI.
- Exact confirmation and disabled-state behavior for destructive operations while work is running.
- Keyboard shortcuts for Open, New Thread, Interrupt, Archive, and related actions.
