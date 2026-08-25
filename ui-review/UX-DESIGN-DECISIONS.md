# CodexUI UI/UX Decisions

This document records the implemented CodexUI visual and interaction contract.

## Visual system

- CodexUI uses a light theme with neutral application surfaces and four opaque
  semantic color families. Blue remains the unchanged primary-action reference;
  green, orange, and red use matching interaction steps and comparable
  white-text contrast.
- Hover, focus, selection, disabled, warning, error, pending, and active states
  remain visually distinct.
- User messages are blue-tinted cards. Codex narrative is visually lighter.
  Commands, tool activity, files, and collaboration activity use raised cards.
- Scrollbars use one compact application style across conversation, nested
  output, State, Protocol, and Inspector surfaces.

| Family | Primary | Hover | Pressed | Soft surface | Border | Surface text |
|---|---|---|---|---|---|---|
| Blue | `#2f6feb` | `#285fca` | existing blue behavior | `#e5eeff` | `#bfd3f9` | `#285fca` |
| Green | `#18865e` | `#14734f` | `#105f41` | `#e9f7f0` | `#a9d8c1` | `#176b45` |
| Orange | `#a85d0c` | `#8e4d09` | `#743e07` | `#fff6df` | `#e5c77d` | `#8a5208` |
| Red | `#c43d4d` | `#aa3342` | `#8f2b38` | `#fff0f2` | `#efb8c0` | `#982f3d` |

Filled semantic buttons use white text and the primary, hover, and pressed
steps without opacity changes. Their primary contrast against white ranges
from 4.55:1 to 5.09:1. Blue denotes primary action or active work, green
denotes success or connection, orange denotes warning or attention, and red
denotes failure, stop, removal, or another destructive action. Activity dots
use the same primary colors at 10 pixels so their state remains legible without
creating a separate indicator palette. The existing gray palette is unchanged;
only inactive thread dots use the lighter, less saturated `#cacccf` so active
blue threads retain clear visual priority.

## Application layout

The window consists of a 64-pixel identity/status bar, a hideable thread
sidebar, the center conversation/composer region, a hideable Inspector, and a
40-pixel status bar. Horizontal splitters resize the three main regions.

The center region is wheel- and touchpad-scroll sensitive across its full
width, including non-scrollable chrome and the splitter handles. Nested
scrollable controls consume wheel events while they can move in that direction;
at an edge, the conversation receives the event.

## Conversation structure

`PresentationModel` is the retained normalized presentation source. The
conversation projects it into one transparent section per app-server turn,
with cards in server order. Stable turn/item and local-submission keys drive a
single reconcile path for both first display and updates. Retained cards mutate
in place, and identical visible projections do not trigger layout work.

## Conversation following

The message view smoothly follows appended or streamed content only while
already at the bottom. Geometry bursts retarget one short monotonic animation.
Manual upward scrolling interrupts it and pauses following. Returning to the
bottom restores it. While paused, a visible-card/pixel-offset anchor preserves
the reading position across appends, card reflow, and reconstruction. This
follow mode and anchor are retained independently for each thread.

## Composer

The upcoming-turn settings and composer remain anchored to the bottom. The
prompt editor starts at one line, grows upward to its maximum, and then scrolls
internally. The message view reserves the canonical composer height. Additional
growth overlays, but does not resize, the viewport. A trailing content spacer
grows by the overlap so the user can scroll the final card above the composer.
Spacer growth does not move the existing reading position. Shrinking the
composer removes the spacer and restores the canonical geometry.

## Pending prompt presentation

Local admission creates a muted-blue prompt card immediately. A brighter blue
highlight sweeps left and right until app-server acknowledgment.
The card belongs to its destination thread and persists through navigation.
Only the correlated `turn.start` or `turn.steer` completion callback
acknowledges it. Each request carries a unique `clientUserMessageId`; after a
successful callback the card keeps a 500-millisecond accepted transition before
normal message presentation. Failure produces an explicit error state.

The input remains enabled after admission. Multiple prompts can be composed
while earlier cards are pending. They are dispatched sequentially per thread.

## Command execution output

Output boxes grow from zero to 220 pixels. Longer output receives a styled
vertical scrollbar. Each box independently follows output at its bottom and
pauses when the user scrolls upward.

## Inspector

The Inspector contains Plan, Agents, Changes, Requests, and Info. Info contains
State and Protocol viewers. Both use application scrollbars. In Protocol, the
log expands above a statistics summary placed at the bottom. Plan, Agents,
Changes, and Requests retain their last visible per-thread presentation across
thread and tab navigation.

## Desktop integration

The application ID, executable, desktop entry, startup window class, icon name,
and installed SVG use `codex-ui`, giving Linux launchers and taskbars one stable
desktop identity.

## Long-operation feedback

Progress feedback is scoped to the operation it represents. Prompt
acknowledgment uses the pending card's animated highlight sweep. Thread creation
and long thread loading may receive dedicated scoped indicators, but no global
spinner or application-wide input lock is defined.
