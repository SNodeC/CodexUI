# CodexUI UI/UX Decisions

This document records the implemented CodexUI visual and interaction contract.

## Visual system

- CodexUI uses a light theme with neutral application surfaces and four opaque
  semantic color families. Blue remains the unchanged primary-action reference;
  green, orange, and red use matching interaction steps and comparable
  white-text contrast.
- Hover, focus, selection, disabled, warning, error, pending, and active states
  remain visually distinct.
- User messages use the blue identity family. Final Codex narrative uses the
  more prominent violet identity family; interim Codex updates remain neutral
  and are identified by their phase label. Commands, tool activity, files, and
  collaboration activity also use neutral raised cards.
- Scrollbars use one compact application style across conversation, nested
  output, State, Protocol, and Inspector surfaces.
- The three primary panels use one prominent neutral 24 px header row:
  `THREADS`, `CONVERSATION`, and `INSPECTOR`, followed by a standard-intensity
  divider and an 8 px content gap. Accent-filled labels are reserved for
  interactive or selected states. Panel headers are one typographic level
  below the active thread title so structure never competes with content.

Canonical application typography is derived from the platform/application
base font size `B`; fixed absolute point sizes are not used for UI chrome.

| Level | Size | Canonical roles |
|---|---:|---|
| Compact | `B - 1 pt` | Metadata, tabs, buttons, table headers, code and diff text |
| Standard | `B` | Body text, controls, editors, list content |
| Structural | `B + 1 pt` | Panel headers, section labels, subordinate brand labels |
| Content heading | `B + 3 pt` | Active thread title and primary in-panel headings |

Weight and color may distinguish roles that share a size. In particular,
uppercase panel headers use Structural size with bold weight and a stronger
neutral color; the mixed-case active thread title uses Content heading size
with semibold weight.

Markdown is authored content rather than application chrome. Its semantic
heading levels intentionally retain Qt's native relative rich-text sizes and
are not mapped to the canonical application scale.

| Family | Primary | Hover | Pressed | Soft surface | Border | Surface text |
|---|---|---|---|---|---|---|
| Blue | `#2f6feb` | `#285fca` | existing blue behavior | `#e5eeff` | `#bfd3f9` | `#285fca` |
| Green | `#18865e` | `#14734f` | `#105f41` | `#e9f7f0` | `#a9d8c1` | `#176b45` |
| Orange | `#a85d0c` | `#8e4d09` | `#743e07` | `#fff6df` | `#e5c77d` | `#8a5208` |
| Red | `#c43d4d` | `#aa3342` | `#8f2b38` | `#fff0f2` | `#efb8c0` | `#982f3d` |
| Violet | `#6941c6` | `#5b37ad` | `#4b2e90` | `#f4f0ff` | `#d4c5f2` | `#53389e` |

Neutral separators and borders use three canonical intensity steps:

| Intensity | Color | Role |
|---|---|---|
| Soft | `#eef1f5` | Subordinate internal separation |
| Standard | `#d7dee8` | Ordinary dividers and card/control borders |
| Strong | `#b9c4d2` | Hover, emphasis, and stronger structural separation |

Ordinary one-pixel lines use Standard. Soft is reserved for deliberately
subordinate structure, while Strong must communicate interaction or hierarchy
rather than decorate a normal boundary.

Filled semantic buttons use white text and the primary, hover, and pressed
steps without opacity changes. Their primary contrast against white ranges
from 4.55:1 to 5.09:1. Blue denotes primary action or active work, green
denotes success or connection, orange denotes warning or attention, and red
denotes failure, stop, removal, or another destructive action. Activity dots
use the same primary colors at 10 pixels so their state remains legible without
creating a separate indicator palette. The existing gray palette is unchanged;
only inactive thread dots use the lighter, less saturated `#cacccf` so active
blue threads retain clear visual priority.

Semantic color is reserved for state-bearing UI: running status text uses blue,
successful completion and connection use green, pending requests and warnings
use orange, and failures, denials, stop, removal, and validation errors use red.
Thread dots continue to describe activity rather than outcome, so completed or
otherwise inactive threads retain the canonical light-gray dot. Reasoning prose
and metadata without an authoritative status remain neutral because their
content does not provide a reliable success, warning, or failure classification.

Conversation activity cards remain neutral so supporting process information
does not compete with the user/Codex exchange. Color on those cards is reserved
for authoritative running, completed, warning/interrupted, and failed status.
Conversation cards with detail use one disclosure-header grammar; title-only
cards omit the control. Message, Command execution, and Image cards open
expanded; other activity cards open collapsed by default. User choices remain
session-local. Native and web expose
the same persistent icon-only Conversation-header controls for Reasoning cards,
interim Codex updates, and the initial Command execution and Image folds. Filtering never
removes retained content, final answers remain visible, and changing the Command
default does not override an existing card's user-owned state.

Cards with content use one header action order: title/phase, flexible space,
**Copy**, then disclosure. Copy remains reachable on a collapsed card, while a
contentless card omits it. Authored Markdown copies from the retained source
with `text/markdown` and identical plain text; non-Markdown cards copy their
deterministic primary-content text rather than rendered widget text.
Folding is immediate rather than animated and anchors the selected title row,
so content only contracts upward or grows downward below the interaction point.

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
with cards in server order. The first You card is the visible turn container;
later process, Codex, and steering You cards are nested inside it. The outer
turn remains foldable, and restoring it preserves every child's independent
fold state. A pending steering card uses the animated blue identity and morphs
in place to a softer blue authoritative steering surface. Stable turn/item and
local-submission keys drive a single reconcile path for both first display and
updates. Retained cards mutate in place, and identical visible projections do
not trigger layout work.

The active thread name and its smaller `workspace | state` metadata form one
baseline-aligned lockup, following the application brand/titlebar pattern
without sharing its font size.

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
growth overlays, but does not resize, the viewport. The trailing allowance is
represented as a logical extent equal to the overlap so the user can scroll
the final card to the composer boundary. The scroll content has no permanent
bottom padding. Matching the Changes-tab separator, the moving composer uses
8 px space, a standard divider extending 10 px beyond the adjacent content on
each side, and another 8 px space. This provides the same boundary at the bottom
and while reading higher in history. Extent growth does not move the existing
reading position. Shrinking the composer removes the extent and restores the
canonical geometry.

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

The Inspector contains the peer primary tabs Plan, Agents, Changes, Requests,
and Info. Primary tabs use the shared full-size application typography and are
never nested. Info presents State and Protocol as raised choice rows with
chevrons; selecting one drills into its viewer, with an explicit back action to
the choices. This expresses hierarchy through navigation rather than smaller
text. Both viewers use application scrollbars. In Protocol, the log expands
above a statistics summary placed at the bottom.

Plan steps, agents, and pending requests are peer records and therefore use the
same raised card surface, border, radius, and internal spacing. Summary surfaces
are reserved for subordinate content within a record. Inspector scroll areas
are frameless and transparent so the panel background remains continuous.
Plan, Agents, and Requests retain their last visible per-thread presentation
across thread and tab navigation.

Changes reflects the local Git worktrees resolved from the selected thread's
retained command directories, never a patch reconstructed from conversation
messages. When several repositories match, the compact Inspector surface
defaults to All repositories and offers a repository selector beside scope,
file summary/list, and unified preview. Repository-qualified file labels remove
ambiguity. Copy and Open review belong to the selected-file preview;
double-clicking a file also opens review. The modeless review window provides
Unified or Side by side layout and Compact or Expanded context without blocking
conversation use. Manual filesystem changes use the same libgit2 authority as
Codex changes; filesystem watches and a short safety refresh remove clean files
and discover new untracked files. The compact and review scrollbars provide an
overview ruler: canonical green marks additions, red marks deletions, and blue
marks hunk boundaries. The file list owns a compact muted footer with semantic
addition/deletion totals, followed by a standard gray divider before the selected
file preview. The divider spans the full tab page and aligns with the tab
underline, while adjacent content retains its normal inset. Repository and
scope are not repeated outside their controls.

## Desktop integration

The application ID, executable, desktop entry, startup window class, icon name,
and installed SVG use `codex-ui`, giving Linux launchers and taskbars one stable
desktop identity.

## Long-operation feedback

Progress feedback is scoped to the operation it represents. Prompt
acknowledgment uses the pending card's animated highlight sweep. Thread creation
and long thread loading may receive dedicated scoped indicators, but no global
spinner or application-wide input lock is defined.
