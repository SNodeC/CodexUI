# CodexUI UX Design — Roadmap and Phase 1: Threads, Turns, and Configuration

This document is the starting point for the CodexUI UI/UX redesign. It records the agreed roadmap and the semantic and behavioral requirements that will later be used as design input for Figma.

The redesign is intentionally developed **semantics first**: decide how CodexUI should behave and what the user should understand, record those decisions here, then let Figma determine the best visual and interaction design. The approved Figma design will subsequently be implemented in the Qt application.

## UI/UX redesign roadmap

The redesign is divided into the following phases, in priority order:

1. **Thread/Turn Configuration & Lifecycle** — thread creation, foundational instructions, mutable execution settings, first and subsequent turns, historical effective settings, fork/resume behavior, and thread lifecycle actions. **This is the first implementation target because it is required to make CodexUI productively usable as a Codex client.**
2. **Thread Navigation & Multi-thread Work** — active/running/attention states, navigation and orientation across multiple threads, concurrent work, archived-thread access, filtering/search/grouping, and related thread-list behavior. *(To be designed.)*
3. **Conversation & Turn Presentation** — user/Codex messages, reasoning, tool activity, commands, file changes, progress, active/completed turns, and historical turn presentation. *(To be designed.)*
4. **Composer & Productivity** — prompt editing, attachments, turn-local inputs, keyboard behavior, send/interrupt workflow, and efficient composition. Persistent execution configuration itself is defined in Phase 1. *(To be designed.)*
5. **Agents & Collaboration** — parent/sub-agent hierarchy, delegation, live agent activity, completed agents, collaboration state, and navigation into agent work. *(To be designed.)*
6. **Approvals & User Input** — approval requests, user-input requests, attention behavior, multiple pending requests, and security-sensitive interaction. *(To be designed.)*
7. **Inspector** — role and information architecture of Info, Plan, Agents, Changes, and any information that should move elsewhere. *(To be designed.)*
8. **Status & Attention System** — working, waiting for user, approval required, completed, failed, disconnected, unread changes, and cross-UI attention semantics. *(To be designed.)*
9. **Keyboard Workflow** — shortcuts, focus movement, thread switching, search, command-oriented operation, and other keyboard-first productivity behavior. *(To be designed.)*
10. **Responsive Layout & Visual System** — panel resizing/collapse, narrow and large windows, saved layout preferences, density, typography, colors, spacing, hierarchy, and final component language. *(To be designed.)*

Only Phase 1 is specified below. Phases 2–10 intentionally remain open until they are discussed and agreed.

---

# Phase 1 — Thread/Turn Configuration & Lifecycle

## Purpose and priority

Thread/Turn Configuration & Lifecycle is the minimum UX feature set required to turn CodexUI from a viewer/controller into a productively usable Codex client.

The user must be able to understand and control:

- how a thread is created and identified;
- the foundational instructions under which it operates;
- how Codex will execute the upcoming turn;
- which execution settings persist into subsequent turns;
- what settings a historical turn actually used;
- how existing threads are opened, resumed, forked, interrupted, archived, renamed, and deleted.

These requirements deliberately avoid prescribing detailed visual layout. Figma should be free to determine hierarchy, compactness, controls, icons, progressive disclosure, dialogs, and other presentation details while preserving the semantics defined here.

## 1. Core model

CodexUI should reflect the Codex model without unnecessarily exposing app-server protocol mechanics:

```text
Thread
├── identity / lifetime
├── foundational instructions
├── current execution configuration
└── Turns
     ├── user input
     ├── effective execution configuration
     └── result / activity
```

A central rule is:

> **A mutable execution setting has only one editable representation.**

CodexUI must not present separate editable "thread reasoning" and "turn reasoning" controls for what is actually one persistent value.

## 2. Creating a thread

**New Thread** opens a dedicated thread-creation dialog.

It contains only properties genuinely associated with creating or establishing the thread:

- optional thread name;
- Base Instructions;
- Developer Instructions;
- temporary / ephemeral status.

The thread name is optional. If the user does not provide one, a useful title should be derived from the first turn. The exact interaction and wording are left to Figma.

Base and Developer Instructions normally inherit the Codex/app-server defaults but can be customized for the new thread.

The exact organization, progressive disclosure, and wording of the dialog are design decisions for Figma.

## 3. Normal execution settings do not belong in the New Thread dialog

The creation dialog should not ask for normal mutable execution settings such as:

- Model;
- Reasoning;
- Workspace / cwd;
- Sandbox / access;
- Approval policy;
- Service tier;
- Personality;
- Collaboration mode.

The new thread initially receives the appropriate Codex/app-server defaults. These settings become relevant when the user is about to start actual work.

## 4. First turn

After thread creation, the new thread is selected and the normal turn composer is presented.

The first-turn workflow conceptually contains:

- prompt / user input;
- the effective execution configuration that the upcoming turn will use.

Important execution settings include Model, Reasoning, Workspace, Access / Sandbox, Approval, and other supported mutable execution settings.

The values initially reflect the app-server/current-thread defaults. The user may leave them unchanged and simply start working.

## 5. Subsequent turns use the same UX

There should not be a separate configuration model for the first turn.

Every upcoming turn follows the same model:

```text
current inherited configuration
             +
       user changes
             ↓
       upcoming turn
```

The composer presents the execution configuration that the next turn will actually use.

## 6. Persistent mutable execution settings

Settings such as the following are conceptually the thread's current execution configuration:

- Model;
- Reasoning effort;
- Workspace / cwd;
- Sandbox / access;
- Approval policy / reviewer;
- Service tier;
- Reasoning summary;
- Personality;
- Collaboration mode.

They should nevertheless be presented in connection with the upcoming turn because this is where their operational meaning is clearest to the user.

Figma should determine the best compact presentation and which settings deserve immediate visibility versus progressive disclosure.

## 7. Changing an execution setting

If the current inherited value is, for example:

```text
Reasoning = High
```

and the user selects:

```text
Reasoning = XHigh
```

before starting the next turn, the semantics are:

> The upcoming turn uses XHigh, and XHigh becomes the inherited value for subsequent turns.

Conceptually:

```text
Thread current value
        High
          │
          ▼
User selects XHigh
          │
          ▼
Upcoming turn uses XHigh
          │
          +
          ▼
Thread current value becomes XHigh
```

The same principle applies to the other persistent mutable execution settings where supported.

The UI should communicate the persistence without requiring the user to understand `turn/start` versus `thread/settings/update`. A tooltip or equivalent explanation such as **"Changes apply to this and subsequent turns"** is appropriate.

## 8. Exactly one editable control

CodexUI should not expose duplicate controls such as:

```text
Thread Reasoning: High ▾
...
Turn Reasoning: XHigh ▾
```

That suggests two independent values when there is actually one persistent setting.

Instead there is one editable Reasoning control associated with the upcoming-turn workflow. The same rule applies to Model, Workspace, Access, Approval, and other persistent execution settings.

## 9. True turn-specific data

Some information genuinely belongs only to one turn, including:

- prompt / UserInput;
- attachments, images, and other input elements;
- output schema / output requirements;
- other genuinely turn-local request data.

These values do not become inherited thread configuration.

The UI should conceptually distinguish them from persistent execution settings.

## 10. Historical turns

A completed or historical turn should, where authoritative app-server/AISuite data is available, retain and be able to display the **effective configuration actually used for that turn**.

For example:

```text
Turn details

Model       GPT-5.x Codex
Reasoning   XHigh
Workspace   ~/AISuite
Access      Workspace Write
Approval    On Request
```

These values are read-only historical information. They are not another place to modify thread configuration.

This allows CodexUI to truthfully represent histories such as:

```text
Turn 12     High
Turn 13     High
Turn 14     XHigh
Turn 15     XHigh

Current upcoming-turn reasoning:
            Medium
```

A reconnected CodexUI should use authoritative persisted/projected values rather than infer historical settings from the thread's current configuration.

## 11. Thread header

The thread header should clearly establish identity and context, particularly:

- thread name;
- workspace/repository where useful;
- relevant status.

It may contain a compact read-only summary of the current execution configuration if that improves orientation.

The header must not create a second set of editable execution controls. The single editable configuration surface remains associated with the upcoming-turn workflow.

The exact header design is left to Figma.

## 12. Normal opening and resume

Selecting an existing thread should simply open it.

If CodexUI internally needs to load or resume the thread, this normally happens automatically:

```text
Select thread
     ↓
load/resume if required
     ↓
show conversation
```

There should be no mandatory resume dialog during ordinary use. Whether `thread/resume` was required is normally an implementation detail.

## 13. Base and Developer Instructions

Base and Developer Instructions form the thread's foundational context.

**Base Instructions** define fundamental Codex behavior for the thread.

**Developer Instructions** provide higher-priority project/workflow/architecture/testing constraints.

They are not ordinary next-turn execution controls.

They can be configured when:

- creating a new thread;
- forking a thread;
- explicitly using **Resume with options…**.

During normal operation they should be treated as foundational thread context rather than something casually changed between turns.

## 14. Fork

**Fork…** creates a new thread derived from an existing thread.

Because this establishes a new thread boundary, the user may again modify:

- Base Instructions;
- Developer Instructions;
- appropriate creation/lifetime properties such as temporary status.

The resulting thread then uses the standard first/next-turn workflow for its mutable execution configuration.

Figma should determine the best Fork interaction.

## 15. Resume with options

Normal resume is automatic.

An explicit **Resume with options…** action is available as an advanced operation. It permits changing Base and Developer Instructions when resuming the same historical thread.

This is deliberately different from ordinary Open/Resume and should be presented as an expert operation.

## 16. Thread context menu

The thread context menu should adapt to the current thread state.

Its conceptual actions are:

```text
Open
Rename…
Fork…

Interrupt                  when applicable

Resume with options…       when applicable

Archive / Unarchive
Delete…

More
  Copy Thread ID           optional developer utility
```

Figma should determine grouping, separators, icons, wording refinements, and exact state-dependent presentation.

### Open

The normal operation. If necessary, CodexUI automatically resumes the thread using its existing context and settings.

### Rename

Changes the human-readable thread title independently of execution state. If no explicit name was supplied during creation, the initial useful name may be derived from the first turn.

### Interrupt

Visible when the thread has a running turn. This is especially important because multiple Codex threads may be running concurrently. The user should not need to switch to a thread merely to stop its active work.

### Archive / Unarchive

Archive is the normal non-destructive way to remove completed threads from the active working set. Archived threads remain discoverable and can be restored using Unarchive. Archive should generally be preferred over permanent deletion.

### Delete

Delete is destructive. It should be visually separated from ordinary actions and require appropriate confirmation. CodexUI should not silently interrupt and delete a running thread; state-dependent availability should make destructive behavior explicit.

### Copy Thread ID

An optional developer-oriented utility. It should not clutter the primary menu and may live under a secondary `More` section.

## 17. Temporary threads

Temporary / ephemeral status is a creation-time lifetime property. It changes whether the thread is persisted to normal history.

It belongs to the thread-creation/fork experience rather than the upcoming-turn execution settings.

The exact control and explanatory wording are left to Figma.

## 18. Overall lifecycle

```text
                 NEW THREAD
                     │
                     ▼
          Thread creation dialog
          ──────────────────────
          optional name
          Base Instructions
          Developer Instructions
          temporary/lifetime
                     │
                   Create
                     ▼
                 THREAD
                     │
                     ▼
                FIRST TURN
          ──────────────────────
          prompt / input

          current execution values
          Model
          Reasoning
          Workspace
          Access
          Approval
          ...
                     │
                    Start
                     ▼
                  TURN 1
                     │
              effective settings
              stored/displayable
                     │
                     ▼
                NEXT TURN
          ──────────────────────
          prompt

          inherited current values
          Model
          Reasoning
          Workspace
          Access
          Approval
          ...
                     │
             optionally modify
                     │
                    Send
                     ▼
                  TURN 2
                     │
                     ▼
                    ...
```

Thread management is conceptually independent:

```text
                         THREAD
                           │
          ┌────────────────┼────────────────┐
          │                │                │
         Open             Fork           Interrupt
          │                │             if running
    auto resume      NEW THREAD
                           │
                    foundational
                    instructions
                    editable again

          │
          ├── Resume with options
          │      foundational instructions editable
          │
          ├── Rename
          ├── Archive / Unarchive
          └── Delete
```

## 19. Phase 1 design principle for Figma

These are **semantic and behavioral requirements, not a prescribed visual layout**.

Figma has freedom over:

- layout and hierarchy;
- compactness and density;
- icons and labels;
- progressive disclosure;
- dialog organization;
- whether execution settings appear as chips, selectors, a toolbar, expandable configuration area, or another appropriate interaction pattern.

The invariant mental model is:

> **Thread creation establishes identity, lifetime, and foundational context. The upcoming-turn workflow exposes the one editable set of current execution settings. Changes apply to the upcoming and subsequent turns. Historical turns show their effective settings read-only. Thread management is provided through a state-aware context menu.**
