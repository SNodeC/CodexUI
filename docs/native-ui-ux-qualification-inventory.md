# CodexUI canonical interactive Xvfb test suite

Last updated: 2026-09-26.

This is the canonical interactive qualification specification for native CodexUI,
including projects and sections. It replaces the earlier cutover inventory at
this same path; do not maintain a competing native interactive checklist.
It supplements the behavior and architecture documents, not a second UI model.

This document specifies checks to perform. It is not an executable runner or a
claim that any check has passed. Existing unit, integration, differential and
benchmark suites remain complementary.

## Execution and evidence policy

- Sections 1–22 below are the complete user-approved interactive and visual inventory.
  Exercise every applicable bullet; do not silently narrow it to selected panels.
- Record each run's commit, worktree changes, build type, Qt/style, DPR, viewport,
  backend, role and traffic conditions.
- Identify cases by section and scenario. Record Passed, Failed, Blocked or
  Not applicable, with the observed result and supporting evidence. Explain
  every Blocked or Not applicable result.
- Store execution reports separately and link back to this specification.
  A previous pass is not proof for a changed build.
- A simulated backend and a real backend are different evidence classes.
  Record which was used; never substitute one claim for the other.
- Run builds and suitable test execution with at least 14-way parallelism.
  Respect required serialization for measurements and shared resources.
- Report Xvfb startup failure separately from child-process results. Do not
  silently replace a failed Xvfb launch with a direct offscreen run.
- Do not install, restart personal services, or mutate personal backend data
  merely to execute this inventory.

Authorized Qt invocation pattern, with a built executable and isolated runtime
configuration supplied by the runner:

```sh
xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  QT_SCALE_FACTOR=1.25 \
  QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough \
  /path/to/built/codex-ui
```

Repeat at the DPRs below. Do not introduce an xcb, Wayland or native-desktop
approval workflow. With the offscreen plugin, capture Qt widget images and drive
Qt input events; a recording of the Xvfb root window alone does not prove that
the offscreen application was rendered or exercised.

## 1. Common execution conditions

Apply these conditions across the sections below:

- Run exclusively through **Xvfb with `QT_QPA_PLATFORM=offscreen`**.
- Exercise actual widgets through mouse, keyboard, wheel, clipboard and drag/drop events.
- Run geometry-sensitive checks at **DPR 1.0, 1.25, 1.5 and 2.0**.
- Cover normal, narrow and enlarged windows; narrow side panels; normal and enlarged fonts.
- Cover empty, small, large and very large datasets—including 10,000-row conversations and thread lists.
- Repeat interactions with **no incoming messages**, ordinary streaming, burst traffic, and **messages that do not create cards**.
- Exercise controller and observer roles, normal and reduced motion, successful and failed requests.
- Use isolated configuration, temporary workspaces and disposable projects/threads. Never delete or modify personal data for qualification.
- Record screenshots, interaction latency, resulting state and failures. A successful click alone does not establish correct behavior.

## 2. Application shell and overall layout

- Start with no connection, no selected thread, a selected thread and an empty draft.
- Resize the whole window horizontally and vertically; minimize and restore where supported.
- Drag both panel splitters; verify usable minimum widths and no overlapping controls.
- Hide and restore the threads panel and Inspector; preserve their relevant state.
- Verify long workspace paths, status text and timestamps cannot force unwanted window growth.
- Check footer attribution, links, status indicators and connection controls at narrow widths.
- Open menus and dialogs near window edges; ensure all actions remain reachable.
- Check tooltips for correct content, styling, placement and disappearance.
- Close the application during loading, streaming, background projection, attachment preparation and diff loading; verify clean shutdown.

## 3. Connection and controller ownership

- Configure every transport exposed by the build; verify the correct fields and validation.
- Connect, disconnect and reconnect; cancel configuration without changing the connection.
- Exercise unavailable endpoints, connection loss, provider failure and recovery.
- Verify connection, provider and controller status remain distinguishable.
- Claim and release control; observe changes from a second frontend.
- Lose control while composing, with a menu open, and while a mutation is awaiting its response.
- Verify observers can browse permitted data but cannot submit mutations.
- Preserve unsent input on failed submission or loss of control.
- Reconnect without duplicate cards, duplicate requests or stale group memberships.
- Verify unsupported grouping APIs fall back cleanly without repeated failing requests.

## 4. Threads panel: browsing and presentation

- Switch between **Projects, Sections and Ungrouped** presentation.
- Verify projects appear before standalone sections and ungrouped threads, including drafts.
- Check empty groups, populated groups and the absence of a synthetic “No project” card.
- Expand and collapse groups and thread hierarchies using mouse and keyboard.
- Preserve selection and expansion across refreshes and regrouping.
- Display long, multiline, Unicode and duplicate thread titles without vertical clipping.
- Verify complete titles remain available through tooltips and accessibility.
- Select threads at the top, middle and bottom; load more pages by the supported controls and scrolling.
- Verify the newest active thread becomes visible correctly when recency sorting moves it to the top.
- Scroll immediately to the top after reordering—no compensating down/up gesture.
- Preserve parent/child relationships within a group; exercise **Go to parent** across different groups.
- Verify statuses, activity animations, request indicators and timestamps update on the correct row.

## 5. Threads panel: search, filtering and sorting

- Submit title searches with Enter; edit, replace and clear them.
- Cover no results, Unicode, whitespace and matching titles on later pages.
- Switch between **Active, Archived and All**.
- Exercise **Alphanumeric, Created, Recent and Section order**.
- Verify manual project ordering independently of thread sorting.
- Change search/filter/sort while earlier page requests are still outstanding.
- Ensure late responses cannot restore an obsolete result set.
- Verify pagination does not duplicate, omit or mix results between queries.
- Check loading, retry, failure and end-of-results presentation.
- Verify the documented Active-before-Archived behavior for Section order in All.

## 6. Project handling

- Create projects with valid names, one root and multiple roots.
- Validate empty names, invalid roots and relative paths; cancel without creating anything.
- Open project details; inspect names, roots, descriptions and available metadata.
- Edit names, roots and multiline/Unicode descriptions.
- Preserve unrelated metadata when editing the CodexUI description.
- Verify descriptions are not presented or submitted as model instructions.
- Assign, reassign and remove a thread’s project membership.
- Reorder projects before another project and to the end.
- Create a thread from an empty or populated project; verify its project and workspace context.
- Delete a project with confirmation; retain its threads.
- Exercise failed creation, editing, assignment, ordering and deletion without inventing successful state.
- Verify changes after refresh, reconnect and application restart against an isolated real backend.

## 7. Section handling

- Create, inspect, rename and delete sections.
- Verify a newly created empty section is discoverable.
- Display standalone sections as enclosing cards and sections within projects as headings.
- Exercise one section shared by threads in different projects without duplicating threads.
- Assign, reassign and remove section membership independently of project membership.
- Move a thread before another member and to the end of the shared section.
- Check empty → populated → empty transitions without losing section identity.
- Create a thread from a section, including a section displayed within a project.
- If thread creation succeeds but section assignment fails, retain the thread and permit assignment retry without resending its prompt.
- Delete a section without deleting its threads or removing their project memberships.
- Verify another frontend’s section edits appear on the supported refresh boundaries.
- Ensure the UI does not imply unsupported project/section nesting.

## 8. Thread creation and lifecycle

- Create ordinary and temporary threads; cancel creation.
- Enter workspace, optional name, base instructions and developer instructions.
- Exercise the workspace picker and invalid input.
- Verify draft selection, single-draft restrictions and first-prompt submission.
- Rename, reload, archive, unarchive and delete disposable threads.
- Exercise Quick fork and Fork with options; check parent relationship and resulting selection.
- Open a retained very long thread and progressively load its history.
- Switch rapidly between threads while history requests remain outstanding.
- Delete or archive the selected thread from another frontend; verify coherent selection and recovery.
- Preserve applicable per-thread viewport state without leaking state between threads.
- Check failures, retry and explicit unsent-prompt recovery for every lifecycle action.

## 9. Message panel: rendering and ordering

- Display every current card kind: user, local prompt, assistant, reasoning, command, agent activity, file changes, generated image, plan and generic activity.
- Check empty, short, long and unusually wide content.
- Verify nested cards retain the intended gap inside turn surfaces; no overlapping borders.
- Verify turn roots and steering cards appear at the correct authoritative positions.
- Exercise delayed acknowledgement, delayed authoritative entry and notification/response ordering variations.
- Preserve the prompt’s widget during authoritative promotion.
- Keep the waiting animation until authoritative conversation entry; do not stop merely on request acceptance.
- Verify turn emphasis and composer Send/Steer/Stop state agree with authoritative turn completion.
- Preserve protocol ordering, including commands that genuinely complete after a final answer.
- Insert older history without losing current cards, changing unrelated order or jumping the viewport.
- Collapse and expand cards and turns; preserve state through scrolling and supported refreshes.
- Select arbitrary cards; verify no unintended blue bottom border or unrelated visual change.

## 10. Message panel: scrolling and virtualization

- Wheel-scroll, pixel-scroll and drag the scrollbar through short and very long conversations.
- Exercise keyboard scrolling, top/bottom navigation and large scrollbar jumps.
- Test both previously visited and newly exposed content.
- Scroll during text streaming, new-card arrival, lifecycle updates and history insertion.
- Test automatic following, manual detachment and returning to the bottom.
- Preserve a reading anchor while content changes above, within and below the viewport.
- Verify boundary clamping when content becomes shorter or the viewport becomes taller.
- Resize during scrolling and streaming; check width-dependent reflow.
- Keep text selection, focus and disclosure state when cards leave and re-enter residency.
- Ensure semantic no-ops cause no movement, reflow, focus change or animation restart.
- Verify widget/document residency and per-frame construction/measurement remain bounded.

## 11. Command-output interaction

- Cover no output, whitespace-only output, one line, several lines and large output.
- Check wrapping, long unbroken lines, tabs, Unicode, CR/LF and trailing newlines.
- Verify natural height for short output and the height cap for long output.
- Check for empty black bars, clipped last lines and excess bottom gaps.
- Verify follow-tail for **new and retained** command cards.
- Detach by scrolling upward; incoming output must not steal the reading position.
- Return to the bottom and verify the intended follow-tail behavior.
- Collapse, expand, resize, switch threads and reload while output exists or streams.
- Start a scroll outside the output and move the pointer inside: the conversation must remain the scroll owner.
- Start inside at the directional boundary: the conversation receives the gesture.
- Start inside with available scroll range: the output retains that gesture even after reaching its boundary.
- Verify command Copy preserves content and the agreed trailing-empty-line policy.

## 12. Markdown, selection, Copy and links

- Render all heading levels, paragraphs, lists, quotes, code, tables, links and images.
- Verify user-message line breaks and paragraph spacing, including multiple empty lines.
- Cover literal punctuation, Unicode, emoji, bidirectional text and authored U+200B.
- Stream incomplete Markdown structures and complete them without damaging prior content.
- Select by mouse and keyboard; select across blocks and within code.
- Keep selection visible and intact during unrelated updates and scrolling.
- Exercise whole-card Copy separately from selected-text Copy.
- Inspect plain-text and available rich clipboard formats; preserve authored characters and formatting.
- Remove presentation-generated placeholders without deleting authored ones.
- Activate links through the intended handler; intercept external launches in the isolated workflow.
- Verify Copy feedback, disclosure hit areas and reduced-motion behavior.

## 13. Prompt interaction region

- Type, edit, select, cut, copy, paste, undo and redo.
- Verify Enter submission and Shift+Enter line insertion.
- Cover empty lines, CRLF paste, trailing newlines and long wrapped/unwrapped input.
- Grow and shrink the editor correctly; scroll internally after its height cap.
- Exercise cursor navigation and selection in a long prompt while messages arrive.
- Verify key autorepeat cannot submit duplicate prompts.
- Exercise input-method composition without premature submission.
- Send ordinary prompts, attachment-only input where allowed, and steering prompts.
- Verify successful admission clears the intended draft only; rejected submission preserves it.
- Stop an active turn; exercise delayed stop acknowledgement and failure.
- Switch threads while composing and verify the defined draft-retention behavior.
- Open approval/review controls without losing unsent input.

## 14. Attachments: picker, paste and drag/drop

- Attach one and multiple images through the picker.
- Attach supported non-image files and mixed file selections.
- Paste image MIME data representing screenshot clipboard content.
- Paste file URLs and ordinary text; distinguish attachments from text correctly.
- Drag/drop images and other local files onto the prompt region.
- Handle duplicates, attachment-count limits, directories, unreadable files and removed files.
- Check Unicode filenames, spaces, long paths and large images.
- Reject unsupported remote-file input clearly without silently losing existing attachments.
- Verify thumbnails/labels, removal controls and narrow-layout behavior.
- Submit attachments with a new turn and steering; inspect the actual outgoing payload.
- Switch threads or lose control during preparation; prevent cross-thread delivery.
- Preserve pasted-image files for queued submission/history; do not delete them on acknowledgement or widget destruction.

## 15. Upcoming-turn settings

- Exercise Model, Reasoning, Access, Network, Workspace, Approval and Style.
- Exercise Permission profile, Approval reviewer, Service tier, Reasoning summary and Collaboration mode.
- Verify model-dependent choices and disabled controls, particularly Style.
- Keep thread defaults distinguishable from explicit overrides.
- Change models and check dependent settings remain valid.
- Open and operate the Additional menu at narrow widths and enlarged fonts.
- Verify settings affect the intended next request—not an already-running turn.
- Check observer restrictions and role changes while settings are open.
- Switch threads and verify settings show the correct authority without stale values.

## 16. Inspector: Plan and Agents

- Switch tabs repeatedly during incoming traffic.
- Display structured plan steps with pending/running/completed status.
- Update and replace plans without duplicate steps or stale status.
- Distinguish structured plan presentation from Markdown-only content.
- Display many agents, long names, status changes and long Markdown results.
- Expand/collapse and Copy agent results.
- Tab and Backtab through controls, including rows taller than the viewport.
- Keep the exact focused renderer, document and selection while scrolling.
- Exercise interior anchoring and bottom clamping after font, style and size changes.
- Verify bounded row residency and no hidden-tab reconciliation storm.
- Switch threads without retaining another thread’s Plan or Agents data.

## 17. Inspector: Changes and diff review

- Select repositories, All repositories, and supported hidden-repository controls.
- Switch Unstaged, Staged and Since HEAD scopes.
- Select files with short/long paths, large diffs and wide lines.
- Verify file selection never automatically widens the Inspector.
- Exercise horizontal/vertical scrolling and text selection.
- Copy the displayed diff; open and navigate the review view.
- Cover added, modified, deleted, renamed, binary and truncated changes.
- Refresh while selection is active; retain it when the file still exists.
- Switch threads/workspaces during diff loading; discard stale results.
- Exercise missing repositories, loading errors and cancellation without stuck indicators.

## 18. Inspector: Requests and approval dialogs

- Display each supported request family and its available actions.
- Accept, reject and open detailed review from both Inspector and composer attention controls.
- Exercise single/multiple questions, choices and free-text answers.
- Verify defaults are not silently submitted.
- Prevent duplicate submission while a response is outstanding.
- Resolve a request from another frontend while its dialog remains open.
- Lose controller access during review; block unauthorized submission.
- Preserve entered responses when admission fails, according to the recovery contract.
- Check keyboard navigation, validation, scrolling and enlarged-font dialog layout.
- Keep request counters and attention indicators synchronized.

## 19. Inspector: Info and timestamps

- Open State, Protocol and Timing details; navigate back.
- Select and copy text in each view.
- Verify state details follow the selected thread/object.
- Stream protocol entries while following the tail and while manually detached.
- Enforce bounded protocol-log history without breaking scrolling.
- Verify timestamps and durations against supplied protocol values.
- Cover absent timestamps, partial lifecycle information, completed/running operations and timezone conversion.
- Open timing details from thread and conversation selections.
- Ensure long values neither widen the Inspector nor leave stale data after switching threads.

## 20. Cross-panel responsiveness and multi-frontend consistency

- **Scroll every scrollable surface** while idle and under incoming traffic: threads, conversation, command output, prompt, every Inspector tab, diffs and dialogs.
- Type, select text, open menus, drag splitters and switch tabs while streaming.
- Repeat with messages that update state but create no card.
- Run a controller and observer against the same isolated backend.
- Compare authoritative card ordering, thread membership, lifecycle state and timestamps.
- Verify observers show prompts after authoritative notifications—not before another frontend’s private optimistic submission.
- Exercise concurrent rename, regroup, archive and deletion operations.
- Measure event-to-presentation latency and worst stalls, not just average throughput.
- Keep reduced-motion geometry checks separate from normal-motion interaction/performance checks.

## 21. Keyboard, accessibility and persistence

- Navigate every actionable control without a mouse; verify visible focus and no focus traps.
- Check accessible names, roles, states, actions and plan-step status.
- Enumerate logical conversation items independently of resident widgets.
- Preserve accessible identity through scrolling, insertion, folding and regrouping.
- Avoid duplicate accessible representations of cards and their resident controls.
- Restart isolated clients and verify supported persistence: configuration, group preferences, expansion and retained backend data.
- Verify stale selection, removed entities and unavailable endpoints recover coherently.

## 22. Mandatory deep visual inspection: complete element inventory

This section is the **canonical visual approval matrix**. Its element tables,
including the complete timestamp inventory, must be checked against the states,
DPRs, font sizes and viewport conditions below. Record a verdict and visual
evidence for each applicable element/condition; an aggregate functional-suite
pass does not approve its appearance. Keep run-specific results separate and
linked to these rows rather than creating a competing specification.

### 22.1 Acceptance procedure and shared appearance requirements

A **deep visual inspection of every UI element below is mandatory**. Passing
functional assertions, producing screenshots, or checking widget rectangles alone
does not satisfy this requirement. The reviewer must open and examine full-shell
captures and legible close-ups of each element, including menus, dialogs,
tooltips, overlays and controls reached only after interaction.

- Inspect normal, hover, pressed, keyboard-focused, selected, disabled,
  expanded/collapsed, loading, empty, error and completed states where applicable.
- Repeat the relevant states at all four DPRs, normal/enlarged fonts, wide/narrow
  layouts, and while incoming messages update the UI.
- Capture before, during and after transitions. Inspect frame sequences for
  flicker, transient parentless cards, late reflow, stale pixels, double borders,
  unexpected blank regions and unrelated movement. A final settled screenshot
  alone cannot establish correct transitions.
- Check both axes: alignment, margins, padding, baselines, text wrapping/elision,
  line/paragraph spacing, scrollbars, focus rings and complete icon outlines.
- Check colors and semantic tones against the existing `UiStyle` tokens, not
  hand-copied alternative color constants. Preserve the established appearance;
  this inventory does not authorize a redesign.
- Text must remain legible. Intentional elision must have a way to recover the
  complete value; accidental clipping, missing glyphs and hidden required
  controls fail. Enlarged fonts must not overlap adjacent controls.
- Rounded surfaces and dividers must be clean at fractional DPR. Focus/hover
  styling must not alter layout or produce an unrelated blue bottom line.
- Every visible control needs distinguishable enabled/disabled and focus states.
  Status must remain understandable through text/semantics, not color alone.
- Compare static captures only under stationary conditions. Use reduced motion
  for static geometry checks and separate normal-motion sequences to inspect
  animations; do not erase or mask arbitrary differences to obtain a pass.
- Record the element, state, actual viewport/font/DPR, screenshot or frame
  reference, observed appearance and verdict. Record defects even if they already
  existed in the starting build. Never bless a defective image as the baseline.

The tables enumerate appearance obligations, not claims of current compliance.
The behavior inventory in sections 1–21 still applies in full.

### 22.2 Shell, shared controls and transient surfaces

| UI elements | Required appearance |
| --- | --- |
| Application icon, brand mark and title lockup | Recognizable, sharp and proportionate; no stretched image, missing glyph or cropped text. |
| Top bar and workspace breadcrumb | Panel surface with clean bottom divider; subdued workspace text, intentional shortening and complete tooltip; no displacement of right-hand controls. |
| Show threads / Show inspector | Visible when the corresponding panel is hidden; aligned with neighboring controls and not duplicated. |
| Claim control / Release control | Caption and enabled state match current ownership; no stale controller appearance after role changes. |
| Requests indicator | Correct count and attention tone; appears/disappears without overlapping connection controls. |
| Connection button and menu | Readable endpoint/state caption, one chevron, aligned Configure/Connect/Disconnect/Reconnect actions and correct disabled states. |
| Footer attribution and links | Bottom-left attribution to Volker Christian, Codex, CodexUI, AISuite and SNode.C; readable separators and Powered by label; no clipping or overlap with status content. |
| Overall token usage and details popup | Stable numeric alignment; wrapping between complete fields, not inside numbers; detailed popup fits the available screen and disappears correctly. |
| Global status caption and dot | Consistent text/tone for Ready, Offline, Reconnecting, provider and attention states; round dot, aligned baseline and no status ambiguity. |
| Panel splitters and dividers | Consistent separation and visible hit area; no gaps, doubled strokes or vanishing panel content while dragging. |
| Buttons, comboboxes and line edits | Shared rounded geometry, aligned labels and chevrons; visible focus/hover/pressed/disabled state without a size jump. |
| Tabs, menus, checkboxes and radio choices | Complete captions and indicators; selected/checked state clear; keyboard focus visible; popups neither clipped nor detached from their control. |
| Vertical and horizontal scrollbars | Correct orientation, proportion and visible thumb; no residual strips, clipped ends or overlap with text. |
| Tooltips and token popup | Light panel background, primary text, stronger divider border, rounded corners and padding from UiStyle; readable long/multiline content and correct dismissal. |
| Notices, warnings and validation errors | Correct warning/danger tone, readable message and reachable dismissal/action; no concealed content after dismissal or movement unrelated to the notice. |
| Empty/loading/failure surfaces | Purposeful explanatory text or indicator; no unexplained black rectangle, stale thread content or phantom actionable control. |

### 22.3 Threads, projects and sections

| UI elements | Required appearance |
| --- | --- |
| THREADS heading, Hide and New thread | Consistent panel heading hierarchy; aligned buttons; fully usable at narrow panel widths. |
| Grouping menu, sort button, title search and archive filter | Current Projects/Sections/Ungrouped mode, sort criterion and Active/Archived/All selection visible; search placeholder and entered text readable. |
| Project enclosing card | One clean enclosing surface per project, before standalone sections and ungrouped threads; semibold heading and consistent rounded border. |
| Section inside a project | Section heading within the project surface, not another nested enclosing card. |
| Standalone/empty section | Own enclosing card with readable heading; discoverable even without thread children. |
| Thread within a project/section | Plain row within the enclosing card, not another individually framed card. |
| Ungrouped thread | Individual card; no enclosing synthetic No project card. |
| Thread title | Single visual line, vertically centered and deliberately elided; embedded CR/LF must not create clipped extra lines. Full title remains available. |
| Status dot, group appearance marker and disclosure | Round, crisp indicator and correctly oriented chevron with established close spacing; invalid optional color must not create an arbitrary marker. |
| Section icon/color fields | Values readable in details/edit forms; inspect supported painted color markers. An icon metadata field does not itself promise a rendered icon. |
| Hierarchy and indentation | Parent/child levels distinguishable without intrusive connecting lines, excessive indentation or loss of available title width. |
| Hover, selection and keyboard focus | Only the intended row is emphasized; enclosing project/section surface remains coherent; no double frame or stale highlight. |
| Draft/pending/activity feedback | Correct semantic surface and bounded animation region; no animation of unrelated rows or lingering pending appearance after authoritative resolution. |
| Page/loading/retry/end rows | Visually distinct from real threads and groups; clear action or explanation; no blank clickable row. |
| Thread/group context menus | Correct target, complete actions, membership destinations and ordering choices; long names do not make controls unreachable. |
| Project form | Name, Roots and Description labels/editors aligned; one root per line; explanatory non-instruction text visible; metadata/timestamp details readable in read-only mode. |
| Section form | Name, Icon and Colour fields aligned; shared-across-projects explanation visible; no implication that sections are owned exclusively by a project. |
| Create/edit/delete dialogs | Clear title, validation, confirmation and action/cancel buttons; read-only details visually non-editable; deletion text distinguishes groups from retained threads. |

### 22.4 Conversation chrome and shared card anatomy

| UI elements | Required appearance |
| --- | --- |
| CONVERSATION heading and divider | Same panel-heading hierarchy as Threads/Inspector; correct spacing above the selected-thread context. |
| Five presentation toggles | Reasoning, assistant updates, initial command expansion, initial image expansion and initial file-change expansion have crisp distinct icons, visible checked/focus states and explanatory tooltips. |
| Thread title, lifecycle and token usage | Readable heading; complete tooltip for elision; lifecycle and token fields do not overlap or force unrequested width growth. |
| History/loading controls and loading overlay | Clear action/progress, correct placement and disappearance; no stale overlay or exposed incomplete history frame. |
| Conversation notice and Dismiss | Message fits its surface; action remains visible; no input obstruction after dismissal. |
| Turn enclosing surface | One coherent rounded boundary; active emphasis returns to ordinary styling on authoritative turn completion; no independently overlapping root-card frame. |
| Card header | Title, phase/status, timing, Copy and disclosure fit on the intended header layout; aligned baselines and consistent compact gaps. |
| Card timing action | Secondary-tone compact text, transparent idle surface, visible hover/focus boundary; narrow clock fallback as specified in 22.9. |
| Copy button | Shared copy glyph and success feedback, correctly centered; no header movement when feedback changes. |
| Disclosure | Shared chevron, correct direction and stable hit target; no excessive spacing from adjacent actions/status. |
| Card body and nested spacing | Content stays inside its surface with the established gap from the turn border; no overlap, unexplained bottom gap or width overshoot. |
| Card selection/focus | Visible, intentional and stable; no stray thin blue bottom border on ordinary mouse selection. |
| Collapsed card/turn | Header remains complete, hidden body leaves no reserved blank area, and expansion restores the correct geometry. |

### 22.5 Every conversation content family

| UI elements | Required appearance |
| --- | --- |
| Normal You / turn root | Blue semantic family, You title, authored text and attachments; root integrates with the view-owned turn surface. |
| Steering You | Teal semantic family and steering phase; visibly nested in its correct turn rather than temporarily shown as a new outer turn. |
| Pending normal/steering prompt | Corresponding blue/teal appearance, pending phase and delayed waiting sweep; accepted-but-not-yet-authoritative prompts remain pending. |
| Failed prompt and recovery | Clear Not sent/error text and available recovery action; no continuing success/pending animation or lost authored text. |
| Codex update | Yellow-family update surface/title and update phase; content legible without implying final completion. |
| Codex final answer | Purple-family surface/title and final answer phase; correct authoritative order and turn ownership. |
| Reasoning | Explicit Reasoning title, readable Markdown summary and correct visibility-toggle behavior; no empty residual region when hidden. |
| Command text and metadata | Readable monospace command area; workspace, exit/status and reported duration remain distinguishable from output. |
| Command output | Dark code surface with light text and token-defined padding; natural short height, capped long height, unclipped last line and no extra bottom block. An empty output must not leave a meaningless black bar. |
| Agent activity | Agent activity title, appropriate status, metadata, prompt/result content and complete disclosure/Copy controls. |
| File changes | Readable path links, change types, available addition/deletion counts and summary; long paths wrap or scroll within the assigned width. |
| Plan card | Explanation and ordered steps; pending/running/completed markers distinct and aligned; no missing status or duplicate step. |
| Generic/tool activity and unknown fallback | Readable humanized title, correct status and bounded detail; truncation notice visible; no raw markup accidentally interpreted as UI. |
| Images and image ribbon | Correct aspect ratio, ordering, bounded thumbnail size and horizontal navigation; missing-image state readable rather than blank. |
| Generated image | Generated-image title/status, supplied revised prompt and actual image; failure and pending states distinct. |
| Image viewer | Complete image within the supported viewing geometry, usable scroll/close controls and no stretched aspect ratio. |
| Markdown body | Distinct heading levels, list indentation, quotes, tables, code and links; authored line/paragraph spacing preserved without an added phantom blank line. |
| Text selection and links | Visible selection with readable selected text; link styling recognizable; copy feedback does not erase the selection. |

### 22.6 Prompt region, attachments and upcoming-turn settings

| UI elements | Required appearance |
| --- | --- |
| Composer enclosure and focus border | White/panel rounded surface; intentional blue focus border; growth/shrinkage preserves surrounding alignment. |
| Prompt editor, placeholder, caret and selection | Message Codex placeholder subdued; entered text primary; blank lines contribute correct height; caret/selection visible through wrapping and scrolling. |
| Send / Steer / Stop | Correct mutually applicable visibility, caption and semantic tone; Send primary blue, Steer teal, Stop distinct; disabled state obvious and buttons never clipped by editor growth. |
| Attachment button and list | Recognizable attach affordance; image/file labels, thumbnails, full-path tooltips and remove controls aligned within the composer. |
| Attachment preparing/error state | Clear reason for disabled submission or failed attachment; no unexplained empty placeholder or misleading completed thumbnail. |
| Attention strip | Request heading/detail and Reject/Accept/Review controls readable, aligned and visually distinct from ordinary composer content. |
| Model, Reasoning, Access, Network | Labels above/alongside correct controls; current values and dropdown arrows readable; no width-induced overlap. |
| Workspace and browse control | Long path contained in the field, folder icon complete, tooltip available, control aligned with neighboring settings. |
| Approval, Style and Additional | Correct selected/disabled appearance; unsupported Style has an explanatory tooltip, not an apparently broken blank control. |
| Additional settings popup | Permission profile, Approval reviewer, Service tier, Reasoning summary and Collaboration mode all visible/reachable with aligned labels and controls. |
| New-thread/fork dialog | Workspace, Name, Base instructions, Developer instructions, Temporary thread control/explanation and relevant fork options readable; long editors scroll; validation and Continue/Cancel remain reachable. |
| File/workspace picker | Location, Up, Go, file/directory listing, selection summary, Add selected/Remove where applicable, validation and accept/cancel all fit and indicate current selection. |

### 22.7 Inspector and review surfaces

| UI elements | Required appearance |
| --- | --- |
| INSPECTOR heading, Hide and tab strip | Stable panel width; Plan, Agents, Changes, Requests and Info labels legible and selected tab clear. |
| Plan rows | Explanation, step text and canonical pending/running/completed status readable; row spacing and disclosure/scrolling coherent. |
| Agent rows | Name/path tooltip, status, shared Copy/disclosure and expanded Markdown aligned; tall rows and focused controls not clipped. |
| Requests rows | Request title, detail, lifecycle and Accept/Reject/Review fit; stale/completed requests do not remain apparently actionable. |
| Changes controls | Repository selector, hidden-repository control, scope, counts and loading/error summary fit without widening the panel. |
| Changes file list | Selected file clear; path, status and counts readable; long paths do not increase Inspector width. |
| Diff preview/review | Selected-file field read-only, horizontally navigable and fully selectable/copyable without panel widening; hunks, additions/deletions, line content and available overview/layout controls legible; Copy/Open review available; truncation and binary/unavailable states explicit. |
| Info choices and Back controls | State, Protocol and Timing presented as distinct choices; detail page title and return action clear. |
| State view | Readable, bounded, selectable state information; wrapping/scrolling stays inside the panel. |
| Protocol view and statistics | Chronological entries, direction, method/correlation, errors and clock provenance distinguishable; lines and statistics not clipped. |
| Timing view | Object/turn/thread/project context clear; multiline exact timestamps and raw units selectable, contained and not confused with receipt times. |
| Approval/permission/question/MCP dialogs | Complete request facts, choices, free-text/JSON fields where supported, validation, current selection and submission state readable; secret fields appropriately protected. |
| Request cancellation/resolution | Dialog/card no longer appears actionable after resolution; entered text is not silently replaced or obscured during recoverable failures. |

### 22.8 Animation and visual-transition obligations

- Inspect prompt sweeps, draft/activity row feedback, loading indicators, Copy
  feedback and any enabled follow animation in normal motion. Verify only their
  intended regions change and no surrounding text/borders shift.
- Inspect reduced motion separately: static feedback must still convey status;
  disabling animation must not hide an actionable control or pending state.
- Examine card admission, authoritative prompt promotion, history insertion,
  regrouping, filter changes, thread switching and pane restoration frame by
  frame for temporary wrong ownership, incomplete surfaces and late jumps.
- During scrolling, inspect newly exposed strips and retained content for blank
  tiles, ghost text, tearing within widget captures and transient border defects.
- Inspect changes to title, status, timestamps and token counts for unwanted
  wrapping or movement of neighboring controls.

### 22.9 Complete timestamp appearance inventory

The schema inventory and formatter are authoritative:
[Timestamp presentation](architecture/timestamp-presentation.md),
[protocol timing paths](../src/codex/ProtocolTimingPaths.inc) and
[TimingPresentation](../src/codex/ui/TimingPresentation.cpp).
These are supplied facts, not permission to fetch new APIs or invent times.

| Surface/element | Required appearance |
| --- | --- |
| Card compact summary | Local `HH:mm:ss` start/end, en dash between supplied start and end, and ` · N.NNN s` for supplied duration; omit absent components instead of inventing zero times. |
| Card with turn timing | Only the owning turn-root adds its turn timing. Ordinary cards describe their own item; do not show another card's or turn's time as theirs. |
| Narrow timing button | Deliberate elision while space permits; clock glyph `◷` when too narrow for a time. Remains focusable/clickable; never forces the header wider or overlaps Copy/disclosure. |
| Missing compact timing | Neutral em dash where no summary is available; tooltip/details explicitly distinguish absence from a valid epoch. |
| Exact timing tooltip / Info → Timing | Field/object scope, local ISO date/time with milliseconds and numeric UTC offset, timezone abbreviation, separate UTC value and raw protocol value/unit. |
| Duration | Seconds with three decimal places in the current formatter; raw milliseconds in details. Never a calendar date or fabricated live countdown. |
| Thread tooltip / Timing details | Created, updated, recency and section-entry facts remain separately labeled; local activity/sort values are not mislabeled as server timestamps. |
| Project details / related project timing | Supplied project times readable in metadata and related timing context; do not invent a compact timestamp badge on every project row. |
| Info → Protocol | Server `emittedAtMs`, locally recorded receipt time and client-clock `currentTimeAt` explicitly distinguished. Event-only facts remain available only while retained. |
| Missing/null/sentinel/invalid values | Not supplied, Unavailable (protocol sentinel), Invalid duration, or Invalid / outside supported date range as applicable; no overflow, guessed units or misleading 1970 fallback. |
| DST/date boundaries | Correct date, offset and timezone distinguish repeated local times; midnight and multi-day operations remain unambiguous in exact details. |
| Timing-only updates | Updated text without header reflow, card replacement, focus loss or animation restart; switching threads clears stale selection/context. |
| Bounded/partial inventory | Truncation/overflow is visible where reported; unavailable or aged-out facts must not appear as complete history. |

Every field family below must have a supplied/missing/boundary fixture and a
visual check on the surface that actually exposes it. Peripheral fields may be
shown only in retained Timing context or observed Protocol entries, not as
dedicated main-panel controls.

| Protocol context | Fields | Required unit/provenance treatment |
| --- | --- | --- |
| Thread | createdAt, updatedAt, recencyAt, sectionEnteredAt | Unix seconds; four distinct meanings. |
| Turn | startedAt, completedAt, durationMs | Unix seconds for dates; milliseconds for duration. |
| Item lifecycle | startedAtMs, completedAtMs, durationMs | Unix milliseconds; duration separate. |
| Timeline | started_at, completed_at, duration_ms | Unix seconds for dates; milliseconds for duration. |
| Approval, automatic/strict review | startedAtMs, completedAtMs where declared | Unix milliseconds, scoped to that review. |
| Hook | startedAt, completedAt, durationMs | Unix seconds for dates; milliseconds for duration. |
| Project | createdAt, updatedAt, recencyAt | Unix seconds, project scope. |
| Thread goal | createdAt, updatedAt | Unix seconds, goal scope. |
| Workspace message | createdAt, archivedAt | Unix seconds; creation and archive distinct. |
| File metadata | createdAtMs, modifiedAtMs | Unix milliseconds; zero is unavailable here. |
| Plugin | installedAt | Unix seconds. |
| External import history | completedAtMs | Unix milliseconds. |
| Rate-limit window / image failure | resetsAt | Unix seconds; reset time, not item completion. |
| Reset credit | grantedAt, expiresAt | Unix seconds; grant and expiry distinct. |
| Model upgrade | retirementAt | Unix seconds; retirement, not installation. |
| Remote control | lastSeenAt, expiresAt where declared | Unix seconds; seen/expiry distinct. |
| Server envelope | emittedAtMs | Unix milliseconds; server emission. |
| currentTime/read response | currentTimeAt | Unix seconds; explicitly client clock. |

Zero remains a valid epoch outside the declared file-metadata sentinel. No
timestamp may be inferred from screenshot position, GUI receipt, final-answer
ordering or another object's timestamp. Raw metadata is not silently treated as
a formatted local time.

## Evidence limits

**Xvfb plus Qt offscreen can establish widget-level interaction and rendering
behavior, but not everything about a real desktop.** Actual Spectacle clipboard
interoperability, compositor behavior, hardware touchpad gestures, native
file-manager drag/drop and real screen-reader integration must remain explicitly
unverified by this workflow.

Likewise, a simulated bridge can prove UI/request handling, but **real project
persistence and upstream interoperability require a separate run against an
isolated real bridge/app-server backend**. Neither category should be marked
passed by inference.
