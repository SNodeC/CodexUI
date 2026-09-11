# Native UI/UX qualification inventory

This is the acceptance inventory for the native two-thread shared-node-graph
cutover. It supplements `ui-behavior.md`; it does not define a second UI model.
Every row must be checked with current `NodeGraph` state, existing widgets, and
the real application where timing or event propagation matters.

## State matrix applied to every relevant surface

Each scenario below is exercised in every applicable user state:

- no thread selected, empty selected thread, hydrating thread, ready idle
  thread, active turn, interrupted/failed/completed turn, and disconnected or
  non-controller runtime;
- following the conversation bottom, paused one card above the bottom, paused
  near the middle, paused at the oldest loaded item, scrollbar thumb held, and
  keyboard focus inside a card, nested output, composer, menu, or dialog;
- short content, one very tall card, many turns, the initial 80-item window,
  one or more explicit Load 80 pages, and history containing unknown items;
- long mixed histories containing normal and steering You cards, interim and
  final agent messages, reasoning, file changes, agent activity, generated
  images, image attachments, unknown fallbacks, approvals, review requests,
  user-input and MCP decisions, and command cards with both short and very long
  output;
- selected/root thread, selected child thread, activity in an unselected
  thread, and a selected thread whose parent or child changes;
- one notification, several coalesced notifications, a sustained streaming
  burst, graph-read contention, removal, reconnect, and replayed state;
- normal width, narrow split panes, resize in progress, hidden/reopened pane,
  and the saved pane/sort/tab/fold state after navigation.

For a paused viewport, every mutation records the first visible stable card,
its pixel offset, scrollbar value and maximum, horizontal positions of visible
content, focused widget, selection, expanded states, and live widget IDs. A
change below the viewport must preserve the visible card and pixel exactly. A
height change above it may change the scrollbar value only by the compensating
height delta. A change in the visible card may alter only that card's required
geometry. No case may expose a placeholder, reserved empty extent, parentless
child, partial order, or intermediate lifecycle style.

## Shell, window, and cross-pane routing

| Area | Situations and expected result | Qualification |
| --- | --- | --- |
| Main window | Initial show, maximize/restore, resize, splitter drag, pane hide/show, minimum useful size | No clipped controls, oscillating layout requests, stale overlay geometry, or whole-window repaint from a descendant update |
| Top chrome | Connection label/dot/menu, controller button, request indicator, restore-pane buttons | Setters/style polish/paint occur only when the effective displayed value changes; item streaming does no work here |
| Bottom status | Ready/connecting/disconnected/error and attribution | Stable geometry; exact state/tone; unrelated thread changes do not repaint it |
| Conversation chrome | Selected-thread title, workspace, last activity, lifecycle state and visibility/filter buttons | Only changed effective fields update; item text streaming does not rewrite thread chrome; final status is immediate |
| Transient notice | Success/error text, replacement, timeout, resize, navigation | Non-layout-shifting overlay, latest notice wins, no input obstruction after dismissal |
| GraphChanged routing | Item field, item structure, thread field, thread topology, interaction, catalog, connection, removal | Only dependent visible panes receive work; hidden panes become dirty only; removals detach synchronously |
| Frame coalescing | Many reduced notifications inside one GUI frame | One bounded commit per affected pane; final completed content is exact; no raw-frame merging before graph reduction |
| Identical state | Repeated same node state/effective shell state | Zero row/card construction, geometry, layout, style polish, or repaint |
| Idle application | Fresh launch with no selection and ready/disconnected states | Near-zero idle CPU; no zero-delay timer, scan, repaint, or layout loop |

Full-app evidence: Xvfb movie plus pane-level paint/layout/style/scan counters,
with fixed screen geometry and pixel-difference regions.

## Thread pane

| Area | Situations and expected result |
| --- | --- |
| Discovery | Empty list, first page, additional pages, refresh, reconnect and provider reset show only admitted roots and never flash placeholders as roots |
| Rows | Name/preview/ID fallback, status dot, hover, selected state, badges/tooltips and exact canonical state vocabulary match the legacy appearance |
| Selection | Left click changes selection once; background activity never steals it; selecting a child keeps its root visible; removal clears selection safely |
| Hierarchy | Root/child ownership, expansion/collapse, deep indentation, late parent, fork reassignment and child removal retain complete reachable topology |
| Sorting | Exactly Alphanumeric, Created, and Recent; natural-number titles; newest-first timestamps; missing timestamps last; fast DB-only first page, automatic background file reconciliation, cursor-guarded near-end paging; Recent admission promotion, acknowledgement confirmation, rejection rollback, and criterion-specific re-sorting |
| Local draft row | New Thread insertion, chosen-name preservation, prompt-bound animation, promotion to provider ID, failure, abandon-empty, second-create guard and later-navigation preservation use one stable row |
| Context menu | Right-click targets the pointed row without selection, holds hover while open, dismisses without click-through, and exposes correct reload/rename/Quick fork/Fork with options/archive/delete state; advanced fork fields and hierarchical chosen names reach the exact action |
| Incremental update | Name/status/tooltips patch only the affected row and repaint only its rectangle; no whole-list update suppression |
| Atomic topology | Insert/remove/reparent/reorder computes a complete target before commit and never exposes partial ordering |
| Large list | Visible rows plus two-row overscan only; bounded scans complete under unrelated revisions; scroll position and expansion remain stable |
| Pane visibility | Hidden pane performs zero QWidget work and resolves the latest graph once when shown |

Existing coverage includes the ThreadPane tests in `ApplicationLayoutTest`;
required additions are paint/style/row-construction region counters, selected
child/root persistence in the full shell, and an idle-CPU/full-app check.

## Conversation initialization, history, and ownership

| Area | Situations and expected result |
| --- | --- |
| No selection | Empty instruction and disabled settings/composer are stable and idle |
| Initial hydration | Cold ready thread, `notLoaded` resume, delayed `thread/read`, active thread and failed hydration preserve authored input and show no partial history |
| Atomic selection | All cards in the selected loaded window are constructed and laid out invisibly; one final frame appears with correct bottom/remembered anchor and no prior reserved blank space |
| Mixed long selection | A retained history containing every supported card family, expanded command output, pending decisions, failures and completed work is exposed in one final frame with canonical order, ownership, expansion and heights |
| Thread switch | Outgoing thread remains visually stable until the incoming final layout is ready; each thread restores its own anchor, follow mode, folds and nested output scroll |
| Load 80 | Existing viewport remains unchanged while all newly requested cards materialize; one final anchored frame appears; repeated pages do not make later live updates history-sized |
| Turn ownership | Every represented child card has its canonical opening You card as QWidget ancestor from its first visible frame; steering You remains nested and never becomes the turn owner |
| Root handoff | Local prompt to authoritative user item retains the same widget, parent, key, fold, height and anchor; item/result arrival in either order has no parentless frame |
| Authoritative replacement | Removed/rolled-back nodes disappear atomically; retained optimistic tails stay attached; stale IDs have no widget |
| Final geometry | Initial wrapping uses final viewport width; after reveal, card/turn/content heights and scroll maximum remain unchanged without new data |

Automated checks capture every paint-time representation count and parent
chain, not only the state after `spinUntil`. The full-app movie must show no
intermediate empty extent or later layout correction at 30 fps.

## Conversation updates in every scroll mode

| Mutation | Following bottom | User scrolled up / holding scrollbar |
| --- | --- | --- |
| New normal prompt | Pending outer You card appears atomically and bottom remains visible | Card is fully constructed below the viewport; exact visible card/pixel and horizontal position do not move |
| New steering prompt | Pending teal You card appears inside the active owner atomically | Same owner and anchor remain visible; no temporary parentless or outer card |
| Prompt acknowledgement | Same widget morphs to authoritative identity and stops delayed feedback | No movement, replacement or temporary neutral styling |
| New agent/process/review card | Complete card and correct owner appear in one frame | No reserved blank extent, viewport movement or neighboring-card repaint |
| Streaming text/output | Affected card updates once per GUI frame; following remains at bottom | Offscreen updates perform zero QWidget work; visible updates preserve the anchored pixel except for required local growth |
| Status-only change | Header/status/border paint only unless text width truly changes | No global geometry or viewport movement; terminal state cannot be overwritten by stale active state |
| Card height change above viewport | Bottom/follow behavior remains natural | Scroll value compensates by the exact height delta so the painted anchor is stationary |
| Card height change in viewport | Only card and owning Turn section recompute | Same, with minimal unavoidable local displacement and no unrelated repaint |
| Auto-approval/request card | Attention state and card arrive together | No jump even when adjacent incoming events arrive before prompt acknowledgement |
| Removal/revert | Removed card/section vanishes and bottom settles once | Anchor restored around the removed extent; no detached live QWidget |
| Unrelated thread change | No conversation work | No conversation work |

The tests cover first-visible stable identity, pixel offset, vertical and
horizontal coordinates, scrollbar range/value, live QWidget identity, focus,
and per-pane paint/layout counters before, during, and after each transition.

## Card types and card-local interaction

Every supported card is instantiated in pending/running/completed/failed/
interrupted/not-loaded states where meaningful: outer and steering You,
interim and final Codex message, command execution, reasoning, file changes,
agent activity, generated image, image view, plan, MCP/tool activity, approval
and user-input activity, generic known activity, and unknown fallback.

For each type verify:

- canonical surface, title, specialization, status text/tone, metadata, active
  emphasized border while awaiting delayed results, and terminal border;
- exact stable widget identity across field streaming and compatible type
  hydration, with replacement only when the existing widget cannot represent
  the authoritative type;
- canonical parent Turn/You card, 8 px nested gap, no duplicated or unmanaged
  nested card, and correct collapse inheritance;
- default expansion, user fold retention across updates/navigation, chevron
  direction, title anchoring, keyboard focus and no geometry change for
  paint-only state;
- Copy availability, exact plain/Markdown content, 0.5-second check feedback,
  reduced-motion behavior, overlay placement, uniformly narrow phase-to-Copy
  spacing and no header movement;
- authored single newlines remain visible in normal and steering You cards
  while copied Markdown, blank lines, and fenced code remain exact;
- retained text truncation notice and copied disclosure at the byte bound;
- attachment order, encoded filenames, bounded image ribbon, horizontal
  scrolling without vertical growth, missing-image accessibility and modeless
  viewer geometry;
- command output upward growth while following, independent inner scroll
  pause/follow state, metadata, cwd, exit/duration, auto-open for live output,
  user-controlled expansion after completion, and state retention;
- no animation for ordinary active work; delayed local prompt feedback starts
  after one second only while visible and stops exactly on acknowledgement or
  failure. Pending normal and steering headers show their lifecycle state.

## Composer, settings, and prompt interaction

| Area | Situations and expected result |
| --- | --- |
| Submit keyboard | Enter/keypad Enter/Ctrl+Enter/Meta+Enter submit; Shift combinations insert newline; Alt+Enter does not; auto-repeat and IME-confirm never submit |
| Send/Steer enablement | Requires non-whitespace draft, ready controller/provider, valid selected hydrated target; active turn chooses steer; clicking an enabled button admits exactly once |
| Exact target | Navigation between typing and clicking uses the visibly selected stable NodeRef; no fallback thread and no dual send |
| Draft ownership | Navigation retains text/attachments; successful admission clears once; rejection/backpressure/wake failure/hydration failure retains it exactly |
| Multiple prompts | Per-thread queue preserves order and only one unacknowledged mutation is in flight; other threads remain independent |
| Composer geometry | Growth overlays upward, canonical reserve stays stable, bottom-follow pause semantics are preserved, and shrinking restores without a jump |
| Attachments | Add/remove, duplicate names, long names, ordinary file Markdown and image paths retain order/content and update height atomically |
| Turn settings | Model/reasoning/access/network/workspace/approval/style/additional values, defaults, enabling and persistence patch only changed controls |
| Attention bar | Request title/detail and Reject/Accept/Review actions, enablement and composer displacement are atomic and scoped |
| Stop | Visible only for an active turn, targets exact turn, remains safe through completion races |
| Recovery | Definite/uncertain failure card restores only by user action and never overwrites a non-empty draft or attachments |

Submission live tests cover result-before-item, item-before-result, other events
between admission and acknowledgement, delayed acknowledgement, error,
interrupt, disconnect, deletion, provider reset, reconnect, and navigation
away/back while pending.

Normal Send and steering submission are each repeated while following the
bottom and while paused at the top, middle and near-bottom. The local prompt is
materialized invisibly and exposed complete, remains under its canonical Turn/
You owner while unrelated cards and decisions arrive before acknowledgement,
keeps keyboard focus and the painted anchor stable, targets the selected node
exactly once, and transitions from pending to accepted without replacement.

Every decision workflow is exercised from the user's point of view: approval
accept/reject/review, command permission, file-change review, structured
user-input answers (including validation failure), MCP elicitation, request
cancellation, and completion arriving while the dialog or card has focus.
Admission must target the displayed request exactly, a rejected admission must
retain authored input, and resolution must update or remove only the affected
surface without changing the conversation anchor.

## Inspector and Info

| Tab | Situations and expected result |
| --- | --- |
| All tabs | Only the visible tab scans/materializes/paints; hidden tabs keep one dirty bit and render latest state once on activation; tab and scroll positions persist |
| Plan | Explanation and ordered steps, canonical/terminal status reconciliation, stable row identity, bounded visible window, no jump under revisions |
| Agents | One stable row per logical child thread; spawn-item fallback only without child ID; spawn tools only; replay/progress/completion/interruption deduplicate; first-spawn order and terminal status win |
| Changes | Async repository resolution, hidden-path option, repo selector, scopes, file list, totals, watches, manual/untracked/staged changes, preview and unavailable state do not block other tabs |
| Requests | Pending/recoverable requests, exact target/context/generation, bounded rows, authored input retention, enabled Review/Accept/Reject and removal after exact response |
| State | Current graph summary, selected hierarchy totals, bounded scan under churn, redaction, scroll preservation and no raw authority/history |
| Protocol | Bounded chronological append with direction/time/sequence/authority/scope/correlation/error, redaction, tail-follow vs paused scroll, one-row append and no rebuild |
| Agent row interaction | Existing visual design, default collapse, expansion retention, copy/status/disclosure order, lazy construction and row-local geometry |
| Info switch | State/Protocol toggle patches only the selected page and never resets unrelated Inspector tabs |

Instrumentation records scans, materialized rows, row constructions,
patches, layouts, paints and style polishes per tab. A conversation-only stream
must leave every unrelated Inspector counter unchanged. Inspector tests are
also repeated while the user is scrolled within Plan, Agents, Requests, State,
Protocol and Changes.

## Dialogs, menus, viewers, focus, and accessibility

- Connection, New Thread, file selection, pending request, permission approval,
  user questions, MCP elicitation, DiffViewer review, image viewer, thread
  context menu, transport menu and sort menu retain their modality/modeless
  behavior, geometry, focus, keyboard activation and canonical styling.
- Invalid required answers or JSON keep the modal open with all authored input;
  untrusted labels/links are escaped; complete permission facts are disclosed;
  secrets and raw payloads are not exposed.
- Popup dismissal has no click-through. Tab/Backtab traversal, Enter/Space
  buttons, accessible names/tooltips, Markdown links and image targets work.
- Keyboard focus is visibly identifiable on every reachable editor, button,
  card, disclosure, tab, menu action, link, image target, list row and nested
  scroll view. The focus indicator is never clipped by a card, viewport,
  overlay or splitter; it does not change geometry; focused content is scrolled
  into view; and disabled/non-interactive content is skipped. Focus remains on
  the same logical control through paint-only graph updates and stable-identity
  materialization, returns sensibly after a modal closes, and never jumps due
  to activity in another card, pane or thread.
- Clipboard success/failure and reduced motion are tested. Focus never jumps
  from the composer or a card because an unrelated node changes or a hidden
  pane becomes dirty.
- Unified/side-by-side and compact/expanded diff preferences persist across
  threads; overview marks, scroll positions, selection and modeless lifetime
  remain correct during refresh.

## Failure, contention, lifetime, and performance UX

- Queue full, eventfd wake failure, disconnect, lost controller, stale
  generation, graph contention, malformed/unknown protocol, app-server error,
  removal and shutdown are visible and never consume authored input silently.
- No non-idempotent mutation is retried. Shutdown cannot hang. Retired nodes
  detach QWidget state before acknowledgement and no queued pass touches a
  destroyed pane or stale generation.
- Continuous unrelated revisions cannot starve conversation, thread or
  Inspector scans. Contention uses a nonzero bounded retry and does not create
  idle CPU churn.
- Large thread selection and Load 80 may take a bounded, measurable delay, but
  expose only the final layout. Ordinary live passes remain viewport/frame
  bounded. Long streaming, large topology, Inspector bursts and bulk removal
  keep Qt heartbeat and input responsive.

## Full-application Xvfb movie suite

The final application—not a small test window—is run with workspace-local
Codex home and config, a real `codex-bridge`, and the app-server it launches.
Movies are recorded at 30 fps and paired with logs/counters for these scripts:

1. Fresh start idle for 30 seconds, connect, discover threads, and verify no
   background visual churn or CPU loop.
2. Select a long multi-turn thread from no selection and from another populated
   thread; use a mixed-card fixture with expanded running command output,
   pending user decisions and all retained card families, and verify the first
   changed conversation frame is already final.
3. Pause at top/middle/near-bottom, click Load 80, and verify exact anchor and
   one final reveal.
4. While paused, admit a normal prompt; receive user item, progress, command,
   approval/review, agent activity and completion in varied order; verify only
   affected regions change.
5. Repeat with steering, including events before acknowledgement and
   interruption; verify permanent parent, stable widget and anchor.
6. Stream a long visible response/command at the bottom and while paused;
   compare dirty regions for Conversation, ThreadPane, Inspector, chrome and
   settings.
7. Keep each Inspector tab visible and then hidden during bursts; verify
   row-local updates, stable scrolling and latest-once activation.
8. Expand/collapse cards, agents and thread hierarchy; focus/copy/scroll nested
   outputs; switch threads away/back and verify every local state.
9. Exercise request dialogs and attention actions, connection/controller
   changes, every accept/reject/review/user-input/MCP decision, validation and
   cancellation path, failure/recovery, thread removal and reconnect.
10. Resize and drag splitters during/after activity, then hold idle again to
    prove geometry settles and CPU returns to idle.

Acceptance uses frame-by-frame region differences. Expected animation regions
(pending prompt sweep and optimistic thread row) are whitelisted narrowly;
any changed pixels in unrelated panes, any partial card/row/layout, or any
anchor displacement fails the scenario.

## Test selection and execution rule

For each particular fix, first identify every checked-in native test executable
and every individual scenario that exercises the changed surface, its parent
layout, routing boundary, focus/scroll owner, and protocol admission path. Run
that complete affected set interactively on a dedicated Xvfb display with
`QT_QPA_PLATFORM=xcb`, not only with Qt's minimal `offscreen` plugin. Then run
the corresponding full CodexUI script above against real `codex-bridge` and
its app-server and inspect the movie frame by frame. A focused test gate is
followed by the full native suite and WebUI parity suite at the qualification
boundary.

The inventory and test selection use only files checked into this local branch.
No GitHub or other remote operation is part of testing or documentation.
