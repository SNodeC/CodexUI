# CodexUI UI/UX Decisions

This document records the implemented CodexUI visual and interaction contract.

## Visual system

- CodexUI uses a light theme with neutral application surfaces and restrained
  blue, green, amber, and red state colors.
- Hover, focus, selection, disabled, warning, error, pending, and active states
  remain visually distinct.
- User messages are blue-tinted cards. Codex narrative is visually lighter.
  Commands, tool activity, files, and collaboration activity use raised cards.
- Scrollbars use one compact application style across conversation, nested
  output, State, Protocol, and Inspector surfaces.

## Application layout

The window consists of a 64-pixel identity/status bar, a hideable thread
sidebar, the center conversation/composer region, a hideable Inspector, and a
40-pixel status bar. Horizontal splitters resize the three main regions.

The center region is wheel- and touchpad-scroll sensitive across its full
width, including non-scrollable chrome and the splitter handles. Nested
scrollable controls consume their own wheel events.

## Conversation following

The message view follows appended or streamed content only while already at
the bottom. Manual upward scrolling pauses following. Returning to the bottom
restores it. Card reconstruction retains this state.

## Composer

The upcoming-turn settings and composer remain anchored to the bottom. The
prompt editor starts at one line, grows upward to its maximum, and then scrolls
internally. The message view reserves the canonical composer height. Additional
growth overlays, but does not resize, the viewport. A trailing content spacer
grows by the overlap so the user can scroll the final card above the composer.
Spacer growth does not move the existing reading position. Shrinking the
composer removes the spacer and restores the canonical geometry.

## Pending prompt presentation

Local admission creates a gray prompt card immediately. A Qt-painted moving
highlight travels around its rounded border until app-server acknowledgment.
The card belongs to its destination thread and persists through navigation.
Acknowledgment replaces it with normal message presentation; failure produces
an explicit error state.

The input remains enabled after admission. Multiple prompts can be composed
while earlier cards are pending. They are dispatched sequentially per thread.

## Nested shell output

Output boxes grow from zero to 220 pixels. Longer output receives a styled
vertical scrollbar. Each box independently follows output at its bottom and
pauses when the user scrolls upward.

## Inspector

The Inspector contains Plan, Agents, Changes, Requests, and Info. Info contains
State and Protocol viewers. Both use application scrollbars. In Protocol, the
log expands above a statistics summary placed at the bottom.

## Desktop integration

The application ID, executable, desktop entry, startup window class, icon name,
and installed SVG use `codex-ui`, giving Linux launchers and taskbars one stable
desktop identity.

## Long-operation feedback

Progress feedback is scoped to the operation it represents. Prompt
acknowledgment uses the pending card's animated highlight sweep. Thread creation and long
thread loading may receive dedicated scoped indicators, but no global spinner
or application-wide input lock is defined.
