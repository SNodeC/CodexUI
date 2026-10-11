<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../../media/nav/banner-findings-ledger-compact.svg"><img src="../../../media/nav/banner-findings-ledger.svg" alt="CodexUI — Findings ledger" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Findings ledger"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/up-findings-ledger-dark.svg"><img src="../../../media/nav/up-findings-ledger-light.svg" alt="Up to Findings ledger" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/organization-dark.svg"><img align="right" src="../../../media/nav/organization-light.svg" alt="SNodeC organization" width="160" height="28"></picture></a><a href="../../../../README.md#project-overview" title="CodexUI repository"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/repository-dark.svg"><img align="right" src="../../../media/nav/repository-light.svg" alt="CodexUI repository" width="160" height="28"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# S5 resumed-worktree and final S3 checkpoints

### 2026-09-18 resumed-worktree checkpoint

This checkpoint updates the plan above; it does not create a competing plan.
After the completed M-10/H-13/M-19/M-27/M-12 and Clang-portability work, the
current native production tree is **37,374 CLOC** and native tests are **45,244
CLOC**. Relative to HEAD the active worktree is native production **+1,087**,
browser production **-6**, tests **-67**, and root CMake **-9** physical lines.
Pre-existing changes remain
user-owned and are not reset or hidden.

- [x] **Live whole-UI lag remediation:** exact deferred GraphChanged delivery,
  stable semantic card identity, bounded row admission, latest-wins worker
  projection, off-GUI Markdown preparation, and no-op DiffViewer updates are
  implemented. The offscreen/Xvfb live matrix recorded conversation, Threads,
  Inspector, and prompt interaction at p99 16 ms with no repeatable frame above
  24 ms; the one 29 ms first-activation Inspector sample did not reproduce in
  the immediate 300-sample rerun (max 18 ms). Temporary probes are deleted.
- [x] **Build ownership and warning gate:** `UiStyle`, conversation UI,
  `NodeGraphUiAdapter`, diff UI, middle UI, runtime, and shell are reusable
  targets. Every production `.cpp` is listed once; the production tree builds
  with `-Wall -Wextra -Werror` (or `/W4 /WX`), and TLS/RFCOMM/WebSocket
  definitions use one helper for every relevant target. Compile-only consumer
  builds pass; no test executable was run in this checkpoint.
- [x] **Shared-label and DTO remainder:** Composer uses the shared selectable
  label factory. Browser file-change `cwd`, local-prompt admission/recovery
  shape, and bounded generic-activity detail now match native meaning; the
  browser no longer retains a second unbounded raw card payload. TypeScript
  compilation passes.
- [x] **M-11 source reduction:** attachment MIME classification no longer reads
  file contents on the GUI thread. Selection performs only bounded metadata
  work for at most 16 entries. A total-byte rejection policy is not a Qt
  performance repair and would change accepted product behavior, so it is not
  introduced as remediation.
- [x] **MR-9D:** one shared disclosure interface now preserves Button role,
  Press/SetFocus actions, and exact expandable/expanded/collapsed state. Copy
  and the existing notice surface use one semantic announcement helper
  (`QAccessibleAnnouncementEvent` on Qt 6.8+, standard Alert on Qt 6.6/6.7).
  The offscreen/Xvfb live probe observed exactly one state event for a real
  transition, none for the repeated no-op, and one announcement. Adding
  `Q_OBJECT` to the shared disclosure class was required so Qt's accessible
  factory sees its concrete type instead of caching the built-in QToolButton
  interface; the first live probe disproved the incomplete factory-only
  implementation. Press and SetFocus are implemented by that same interface,
  not by a shadow object.
- [x] **MR-9E live closure:** the sole thumbnail cache now keys physical DPR,
  decodes to a physical target, uses device-independent geometry, and reloads
  on `DevicePixelRatioChange`. An environment-gated probe in the real
  `codex-ui` executable was run only through offscreen/Xvfb and then deleted.
  At DPR 1/1.25/1.5/2 a real 3024x4032 3.8 MiB JPEG produced correct 129x172
  logical geometry and 129x172, 161x215, 193x258, and 258x344 physical rasters.
  The measured 77--79 ms synchronous defect is replaced in place: warmed
  construction and cache hits are 0 ms, decode completes in 42--46 ms without
  blocking event delivery (maximum 2 ms GUI heartbeat gap), and exactly one
  intrinsic-height notification changes the card extent from 131 to 263 px.
  A destroyed in-flight thumbnail also completes safely under ASan/UBSan.
  The maximum allowed 16-image message was then live-stressed at all four
  DPRs: warmed construction was 1 ms, all rasters completed in 55--56 ms,
  exactly one height notification was emitted, and the worst GUI heartbeat gap
  was 4 ms.
  Worker threads create only QImage; QPixmap/cache/widget/accessibility/geometry
  remain GUI-thread-owned. No timer, second renderer, or second cache was added;
  the old synchronous decode and three dynamic probe properties were deleted.
  Temporary live instrumentation is removed.
- [x] **M-01:** notices and graph-driven selection are durable NodeGraph state;
  only bounded, non-authoritative protocol diagnostics remain in the transient
  worker queue. `UiEffect`, its enum/kinds, queue reservation, Shell dedupe and
  fallback routing are deleted. Prompt admission now writes its prompt,
  selection, and rejection notice in one graph transaction, avoiding the
  revision-N-after-N+1 publication order found during final review. Production
  and every consumer target compile warning-clean; no test executable was run.
- [x] **M-18:** FileChanges uses Qt's one native hypertext interaction and
  accessibility implementation. Custom cursor-range mouse/key hit testing is
  deleted, and open failures route through the existing notice surface.
- [x] **H-12/M-25:** one `DiffViewer` now owns the sole provider, refresh
  coalescer, repository poll, and snapshot used by both the embedded surface and
  review window. The review no longer owns a provider, poll, workspace copy, or
  eagerly active documents; only its selected layout retains text. Same-file
  cursor/selection and both scroll axes survive refresh, and semantic no-ops do
  not replace text. The owner switches the one provider request between compact
  and expanded context and returns to compact when review closes.
- [x] **M-26:** hidden diff polling is stopped and duplicate review wakeups are
  deleted. A temporary real-window event filter measured the complete idle
  stream for 2.2 seconds: exactly 22 timer events, all from the documented
  100 ms eventfd wake-recovery poll; every visual/thread/conversation/composer/
  inspector/diff timer emitted zero. The retained poll performs only bounded
  atomic/queue checks and preserves delivery after an admitted wake failure;
  deleting it would weaken reliability. Temporary instrumentation is removed.
- [x] **M-04:** rename revalidates the retained Thread node after the nested
  modal loop before sending and preserves authored text only for an unadmitted
  live rename. Optimistic-create selection retries were already deleted; the
  current source has no second general focus retry owner.
- [x] **H-13/M-10:** the composer is a normal layout child and Qt layout is its
  sole geometry authority. The reserve/anchor overlay, trailing-space scroll
  compensation, canonical-height callback, settlement callback, and
  composer-growth pause state are deleted. Attachment removal mutates the exact
  stable row, safely defers only destruction of the emitting QObject, transfers
  focus to an adjacent Remove/Attach control, and announces removal without a
  full rebuild. A real `codex-ui` offscreen/Xvfb live gate passed at DPR
  1/1.25/1.5/2: expanded composer reduced the conversation viewport 542→394 px
  with no overlap; removal/deletion/focus passed; combined scroll/edit p99 was
  2--4 ms and maximum 4--5 ms. Temporary instrumentation is deleted.
- [x] **H-14/M-13:** all native motion surfaces consume the existing
  `UiStyle::animationsEnabled` fact, and reusable raw visual color literals are
  confined to `UiStyle` tokens. No theme epoch, policy object, or alternate
  ThreadPane appearance was introduced.
- [x] **M-19:** request shape/disclosure is bounded at the existing policy
  boundary, controls are keyboard-accessible, first-invalid focus is explicit,
  and Shell re-reads the current request after the modal before submission.
  Deleting the ephemeral-toggle `name->clear()` preserves authored names while
  the draft still omits the name whenever Temporary is enabled. A four-DPR
  offscreen/Xvfb live dialog pass proved name preservation, invalid-editor
  focus ownership, and Unicode RTL/emoji submission. The offscreen plugin has
  no active top-level after the nested warning, but the parent dialog's focus
  target is correct; no activation retry was added.
- [x] **M-27:** the 1100×700 window floor and three competing pane minimum
  widths are deleted. Text-bearing bars/buttons use minimum rather than fixed
  heights, and existing ignored-width labels may yield before controls. The
  normal 1536×960 splitter remains exactly 282/834/404 and ThreadPane styling
  is unchanged. Live offscreen/Xvfb qualification passed 800×600, 1100×700,
  and 1536×960 at 1.0× and 1.5× fonts with all panes and Hide controls
  reachable.
- [x] **M-29/M-30 current product behavior:** the geometry-changing global
  1→2 px focus border is replaced by a color-only transition on the existing
  border. A temporary real-widget probe passed Fusion and Breeze at DPR
  1/1.25/1.5/2: Arabic, Hebrew, combining marks, emoji/graphemes and CJK
  rendered; real IME commit preserved the prompt; all 40 visible tab-focusable
  controls retained exact geometry and 27 produced visible focus-pixel change.
  Temporary instrumentation is deleted. Durable repository-wide adversarial
  fixtures and native-platform breadth remain S8 because executing/adding test
  workflows is currently prohibited by user instruction.
- [ ] **Remaining S8-S9 qualification:** durable Unicode/RTL/IME fixtures, CI
  Release/Clang/sanitizer/platform breadth, and native AT-SPI remain. The
  AISuite publish/pin transition is complete. Multi-case native mains already use
  non-short-circuiting accumulation; generated-protocol drift is locally
  repaired. The CI execution matrix is not expanded while the user's current
  no-test-workflow instruction is in force.
- [x] **S8 compile-only matrix:** exact-current GCC Release, GCC Debug, GCC
  ASan/UBSan, and Clang Release builds complete at 14 jobs without running a
  test executable. Clang exposed and closed four production portability gaps:
  the catalog uniqueness proof now sorts then checks adjacent keys instead of
  quadratic constexpr string comparison; libgit2 options use its initialization
  API instead of warning-prone aggregate macros; one `unique_ptr` return moves
  explicitly; and a template callback no longer captures an unused SDK. Six
  test-helper projection switches now exhaustively classify `Protocol` and the
  final Clang rebuild is warning-clean. CI/platform execution breadth remains
  open under the current no-test/offscreen-only constraints.

The authoritative remaining dependency order is:

1. [ ] **S8 qualification remainder:** supported native-platform/AT-SPI
   breadth, durable Unicode/RTL/IME fixtures, and a CI execution matrix. GCC
   Release/Debug/ASan plus Clang Release compile cleanly, active documentation
   is reconciled, and multi-case mains are non-short-circuiting. The current
   user instruction forbids executing test binaries, so only compile and
   autonomous live offscreen/Xvfb gates run now.
2. [x] **AISuite generated protocol release:** pin exact stable+experimental
   generator inputs, regenerate typed bindings, add regenerate-and-diff CI,
   publish an immutable AISuite revision, update CodexUI's pin, replace eight
   adapters with generated operations, and retain only the two intentionally
   internal typed descriptors. Completed against immutable AISuite revision
   `24b1d16ca87e0f0d13fef75a68877cadbb32c03f`.

The AISuite side of item 2 is published and pinned.
`tools/regenerate-codex-protocol.sh` pins Codex tag `rust-v0.154.0`
and commit `6b9826e3aa83b1a5947db50f4332cb9c65f1b340`, verifies that tag-to-commit
mapping, extracts the release's own experimental precomputed schema, and
regenerates C++/TypeScript/manifest together. A second `--check` run reproduced
all three artifacts byte-for-byte; the generated native library and TypeScript
package compile cleanly. AISuite CI runs the same drift gate. The resulting
bindings expose the expected eight operations and still omit the two upstream
internal-only operation descriptors. CodexUI now pins the published revision,
uses generated operations for all eight, and retains only typed local operation
identity for the two internal `rawResponse*` notifications. The obsolete
`CurrentProtocolAdapters` name and eight untyped descriptors are deleted.

#### AISuite generated-protocol evidence reconciliation

Claude's generated-protocol drift finding is **verified in cause but its
delete-the-whole-shim remedy is superseded by current evidence**. AISuite's
checked-in C++ and TypeScript bindings have matching hashes and exactly match
their pinned Codex source, but that source was paired with the stable-only JSON
schema. CodexUI enables and consumes the experimental surface, so the stable
generator necessarily omits those operations. AISuite CI compares the two
generated outputs with each other but neither pins the input files nor
regenerates them, so it cannot detect this class of drift.

An exact Codex 0.154.0 experimental export plus its matching tagged
`common.rs` generates typed operations for `thread/items/list`,
`thread/turns/list`, `currentTime/read`, both auth-recovery notifications, and
the three legacy realtime-item notifications: **eight of the ten** handwritten
descriptors. `rawResponseItem/completed` and `rawResponse/completed` are marked
internal-only upstream and are deliberately omitted even from the public
experimental operation union, although their typed payload definitions remain
exported. The accepted AISuite repair is therefore: pin both generator inputs
to one Codex release, regenerate and diff them in CI, publish the resulting
AISuite revision, replace those eight descriptors with generated operations,
and retain only two typed internal-operation descriptors. A blanket deletion
would lose valid older-server notification compatibility; retaining ten
untyped `Value` descriptors would preserve avoidable drift.

This was a cross-repository release dependency, not a reason for a CodexUI-local
compatibility layer. AISuite revision `24b1d16...` supplies the immutable release
identity and CodexUI's one revision pin now selects it for native and browser
builds. The transition also reconciles the complete generated operation catalog:
five newly generated client methods are classified, while three older-server
methods remain explicit legacy compatibility. No app-server source, fabricated
SHA, parallel adapter, timer, cache, flag, or protocol state was introduced.

Exact transition verification: AISuite regeneration `--check` reproduced the
pinned Codex 0.154 experimental artifacts and its frontend package passed 20/20;
the exact-pinned CodexUI Release tree built completely warning-clean; the native
protocol-binding gate passed 1/1; and the browser suite passed 287/287. Two broad
native integration executables retain their pre-existing current-environment
failures identically in the pre-transition binary and are not misreported as a
transition regression. Stage accounting is native production **-47 CLOC**,
native tests **-84 CLOC**, and root CMake physically neutral after target/file
renaming.

**MR-9A closure (2026-09-17).** The restored invariant is that motion policy and
native Copy publication each have one authority: every affected surface reads
the same live Qt animation-duration fact, and the shared `CopyButton` publishes
Card/Agent values while `MarkdownTextView` owns selection MIME normalization.
This stage deleted the Copy-local style query, duplicate Card/Inspector
clipboard publication, the keyboard-only selection rewrite, and the
`spinnerAnimationActive`/dynamic `pendingFeedbackVisible` test mirrors. It did
not add a timer, cache, preference, callback, renderer, or persistent state
field. Existing spinner, pending, Thread-row, follow-tail, and Copy mechanisms
are stopped, resumed, or completed through their existing Qt event paths.

Failing-first checks reproduced rich-MIME loss, reduced-motion work in pending,
loading, and Thread surfaces, live style-transition gaps, retained ODF marker
content, and nested-steering transition gaps. The accepted replacement keeps
plain text, HTML, Markdown, and ODF formats while removing the presentation-only
U+200B marker; public Qt ODF serialization is performed only for an actual Copy
selection. Under a zero duration, Copy feedback settles immediately,
follow-tail completes synchronously, and the existing spinner/pending/Thread
timers deliver no repeating animation work. Positive-to-zero-to-positive style
changes stop and resume the same physical widgets without replacing accessible
objects. Two independent read-only adversarial reviews passed the final source
and behavioral assertions.

The MR-8 anchor was production **35,487** and tests **43,719** CLOC. MR-9A closes
at production **35,527** and tests **44,107** CLOC: stage **P +40 / T +388**;
cumulative MR-1..9A production is **-959** from the 36,486 baseline. Production
growth is within the user's explicit +1,600-CLOC ceiling and consists of the
shared live-policy query/change-event handling plus format-preserving public Qt
serialization; reduction or simple call-site substitution could not preserve
ODF or react to runtime style changes. No parallel authority was introduced.

Final commands and results:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14 && ctest
  --test-dir /tmp/codexui-release-gate.I6nD6Z -j14 --output-on-failure`:
  **63/63 passed**, including all **29** registered quantitative performance
  gates, in **78.23 s** real time.
- Focused `codexui-conversation-cards`: **1/1 passed** in **11.20 s** after the
  final actual-timer and nested-steering assertions.
- ThreadPane base, four DPR geometry cases, and twelve quantitative performance
  cases: **16/16 passed** in **7.85 s** with the exact production delegate still
  installed.
- `git diff --check`: clean. The production tree contains exactly one
  `SH_Widget_Animation_Duration` query and no `spinnerAnimationActive` mirror;
  the remaining `pendingFeedbackVisible` is the typed renderer-owned visual
  phase, not a dynamic property or second authority.

Accessibility verification establishes stable widgets/names and no replacement
on motion changes; platform AT-SPI announcements remain deliberately deferred
to MR-9C/D. External clipboard persistence cannot be verified through Qt's
public no-result `setMimeData()` contract. Debug, sanitizer, compositor-present,
and final cross-platform qualification remain MR-9F rather than being claimed
by this Release offscreen gate.

Correctness gates cover normal/reduced transitions, pending/steering/loading/
follow/Copy behavior, rich selection MIME through keyboard and standard context
actions, Card and Agent Markdown MIME, font/style/DPR change, and image
load/fail/change. Accessibility gates record exact event type and existing
physical interface/ID for selection, focus, busy, disclosure, Copy, notice, and
request transitions; identical reapplication must emit none, and streaming must
not gain a second event stream. Performance gates require zero reduced-motion
repeating visual timers/repaints, unchanged normal-mode residency/documents and
bounded row-local animation, bounded image decodes/cache entries, and no new
per-frame work. Real external AT-SPI and compositor-present timing remain S8
platform qualification and will not be mislabeled as offscreen proof.

**MR-9B closure (2026-09-17).** The restored invariant is that reusable color
and geometry tokens plus the one application stylesheet own static appearance;
widget-local sheets contain only genuinely dynamic values. Current-font
geometry is derived from the current `QFont`, and a semantic font no-op cannot
replace widgets, move geometry, change focus, or repolish the application.

Deletion came first. This stage removed the losing tooltip rule; the local
top-bar, status-bar, breadcrumb, conversation, attachment, Markdown,
file-change, image-ribbon, command, and pending-card static sheets; duplicate
status-dot color writers and `statusToneColor`; local command-output padding
constants; the cached editor maximum; repeated static Brand/pending rules; and
the ribbon's conflicting platform-scrollbar geometry estimate. The retained
dynamic sheets own only Brand title pixel size, pending/status foreground, and
the pre-existing scrollbar-overlay margin. The ribbon now measures its actual
styled horizontal scrollbar. ThreadPane production source and pixels were not
changed.

The coherent replacement adds exact `UiStyle` tokens, named and
order-independent application-QSS substitution, status tone selectors, an
application-font equality guard on the existing Shell event filter, and
current-font remeasurement on existing Qt change events. It adds no timer,
cache, flag, callback, renderer, theme/epoch owner, persistent state field, or
parallel visual policy. The transient positional 66-value formatting array was
deleted before closure; unresolved named or numeric placeholders now fail the
test. M-13/X-03's raw-literal/static-QSS slice is complete. M-30's broader
focus/hover characterization remains separate and is not silently claimed
here.

The MR-9A anchor was production **35,527** and tests **44,107** CLOC. MR-9B
closes at production **35,573**: stage **P +46**, cumulative MR-1..9B
production **-913** from the 36,486 baseline. This disproves the predicted
MR-9B reduction. The intermediate candidate was P +101; safe deletion reduced
it by 55 CLOC. The irreducible remainder is the exact typed-token registry,
named substitution, and live application-font geometry path; compressing that
mapping or restoring raw/local policy would make the architecture less clear.
The +46 is within the user's explicit +1,600 production-CLOC approval. Tests
close at **44,362 C++ CLOC** plus **101 CMake policy-test CLOC**, or **44,463**
on the MR-9B-inclusive gauge: stage **T +356**. Root CMake additionally
registers the policy gate and carries the requested-DPR environments; it is
reported separately from production and test source.

Final commands and results:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14`: passed after the
  final source-policy, DPR, and projection-gate changes.
- Focused style/Card/Shell plus four ThreadPane visual DPR cases and the 10k
  projection profile: **8/8 passed** in **12.17 s**.
- `ctest --test-dir /tmp/codexui-release-gate.I6nD6Z --output-on-failure
  -j14`: **64/64 passed** in **74.57 s**, versus MR-9A's 63/63 in 78.23 s.
  All **29/29** registered quantitative gates passed in **63.37 s**; this is no
  measured regression, not a claim that scheduler noise is an optimization.
- Fusion and Breeze conversation focus plus ThreadPane and Inspector geometry
  ran without skips at verified DPR **1.0, 1.25, 1.5, and 2.0**. The twelve
  ThreadPane quantitative cases retain 18 fixed widgets, zero index widgets,
  one fixed document, row-local paint work, and now gate adapter projection.
  Three isolated 10k Release samples measured adapter projection at
  **24.874/24.014/23.918 ms** before installing the 500 ms Release regression
  ceiling (4x only for instrumented builds).
- `git diff --check` and the non-short-circuiting source-policy script pass.
  Independent source and gate reviews found no remaining MR-9B blocker.

Correctness verification covers exact prior colors/selector specificity,
single tooltip ownership, command/image geometry, current-font editor and Brand
fit, focus and object identity, and identical-font-change pixel/geometry no-op.
Accessibility names and physical objects remain stable; MR-9B adds no semantic
tree. Performance verification covers bounded Card/Thread/Inspector residency,
documents, measurement/materialization/paint work, wheel latency, and explicit
thread projection time. The source policy is a scoped syntactic regression
guard plus a current-worktree raw-literal scan, not a full C++/QSS parser.
Refreshed warning/ASan/UBSan, real-compositor/AT-SPI, and non-Linux platform
qualification remain explicitly assigned to MR-9F rather than being
misrepresented by the final Linux/offscreen Release run.

**MR-9C pre-edit gate (2026-09-17).** The invariant being restored is that
each semantic accessibility transition has one event authority on the same
physical interface that exposes the resulting state, while a semantic no-op is
silent on every supported Qt version. Native QWidget/QLabel/QAbstractButton and
QTextDocument events remain native; custom projected selection, logical-row
focus, busy state, and Thread-tree semantics are emitted only at their existing
state-owner boundaries after mutation.

Three independent read-only audits and the primary source trace agree on the
following evidence. Qt 6.6, the configured minimum, emits
`NameChanged`/`DescriptionChanged` for every accessible-property setter call,
whereas the installed Qt 6.10.2 equality-guards those setters. Therefore
application-side equality or deletion is required for cross-version no-op
semantics. `QLabel::setText`, button text, QWidget visibility/enabled/focus, and
native text-document mutation/selection already emit the required events and
must not be mirrored. `QAbstractItemView` emits no row selection/current event;
Conversation's custom ListItem state is consequently silent. Its two custom
busy bits are likewise silent. Thread's required nested custom tree conflicts
with QTreeView's native accessibility events: Qt addresses a private flattened
child number (including a header offset), while `ThreadTreeAccessible::child()`
authoritatively addresses top-level semantic items. Reinterpreting that number
or exposing a second flat tree is rejected.

Deletion precedes replacement: remove the explicit Inspector `NameChanged`,
the status-label accessible-name mirrors, the history-button text/name mirror,
and every repeated same-value accessible setter in this affected path. Bypass
QTreeView's incompatible flattened focus/selection event layer at the existing
`ThreadTreeWidget` subclass boundary while retaining its ordinary view update
behavior. Keep the explicit Thread-item `ObjectDestroyed` event before cache
retirement; it is the sole valid lifetime event for the QObject-less semantic
item.

Deletion alone is insufficient because Qt cannot infer the custom state bits or
map its private flat index into the one nested Thread hierarchy. The coherent
replacement uses stack events at the existing mutation boundaries, derives
old/new values locally, and adds no timer, cache, flag, callback, coordinator,
shadow state, renderer, or accessible object. Thread presentation events must
not eagerly create interfaces while accessibility is inactive; selected and
visible Conversation objects remain bounded by renderer residency. The
read-only forecast is production **+40..+70** after a deletion floor of eight
lines, above the earlier narrow +10..+35 estimate but inside the user's explicit
+1,600 production-CLOC approval. The MR-9B anchors remain production **35,573**
and tests **44,463**; exact stage deltas will be recorded at closure.

Verification will install one test-only synchronous event recorder, resolve
events to stable accessible IDs rather than incidental child positions, and
gate exact changed/no-op selection, focus, busy, Thread name/description,
expansion, reparent, and destruction transitions. It will also prove that
Markdown mutation/selection and native widget name/visibility/enabled/focus
retain one native event stream, interface identity is stable, and no event
materializes an offscreen Conversation renderer. Correctness and accessibility
run in the Conversation, Thread, and Inspector suites; performance re-runs all
29 quantitative gates and checks unchanged bounded residency/per-frame work.
Qt 6.6 compilation and real AT-SPI remain MR-9F qualification rather than being
inferred from the installed Qt 6.10 offscreen backend.

**MR-9C closure (2026-09-17).** The restored invariant is that each application-
owned semantic transition publishes once, after mutation, on the same retained
accessible interface that exposes the final state. Native widgets retain their
native roles/actions/events, while identical state is silent and model-only
Conversation rows never acquire a renderer merely to emit an event.

Deletion remained part of the replacement. This stage removed the Inspector's
manual duplicate `NameChanged`, status-label accessible-name mirrors, the
custom history-button accessibility wrapper and text/name mirror, and duplicate
row-focus publication on ordinary widget focus entry. Repeated accessible-name
and description setters are equality-guarded for the Qt 6.6 floor. The native
history `QPushButton` again owns Button role, press action, name, geometry, and
disabled state; the existing Conversation List interface alone owns aggregate
history busy state.

The coherent replacement emits only the state Qt cannot infer: Conversation
resident-row selection/current focus and List/loading busy; and Thread nested-
tree selection/current focus, presentation/reparenting, expansion, and exact
objectless-item retirement. Thread refresh captures only already-registered
interfaces, completes the one item reconciliation, and then publishes settled
events. Stable presentation identity survives optimistic target promotion.
The Tree selection interface now follows Qt's direct-child-only contract and
therefore reports/accepts roots only; every nested TreeItem retains its native
semantic selected state and existing press/focus action. Disabled or wholly
hidden Conversation/Thread containers reject accessibility mutations. No
timer, cache, flag, callback, renderer, shadow model, persistent state field,
or visual rule was added.

The event recorder resolves every event to a stable interface ID. Tests prove
changed/reverse/no-op selection (deliberate Qt-compatible Add-before-Remove
order), logical focus, busy, name/description, expansion masks, combined
reparent/presentation, retirement, optimistic promotion, disabled actions, and
interface identity. A model-only Conversation selection leaves scroll,
resident count, accessible children, construction count, and events unchanged.
Streaming retains one native Markdown widget, document, accessible interface,
text interface, selection, and ID; native selection/caret events occur once in
source order, identical selection/content is silent, and no duplicate text
event targets the containing ListItem. Loading failure/retry has exact
Name-then-busy ordering, reverse completion, and repeated-failure silence. The
Shell integration test now checks native button-disabled plus List-busy state
instead of reviving a non-native busy Button implementation.

Two apparent blockers were explicitly reclassified rather than dropped.
`QAccessible::setActive(true)` is not a portable test seam: in Qt 6.6 through
6.9 it only notifies activation observers, while Qt 6.10 additionally mutates
platform accessibility. Native Markdown content events are consequently
required locally only when `QAccessible::isActive()` and real Qt 6.6/AT-SPI
delivery remains MR-9F. Native QWidget focus/show/hide and QLabel/QPushButton
event decomposition are local Qt 6.10 characterization, not cross-version exact
contracts. Qt source also shows that QLabel clear/pixmap replacement does not
publish fallback-name change; the affected MR-9C status paths are text-only, so
that source risk is disproven here and image/thumbnail accessible naming remains
with MR-9E qualification. Semantic Copy, repeated-notice, and request
replacement announcements are still explicitly owned by MR-9D; they cannot be
misrepresented by ordinary property-change tests.

The MR-9B anchor was production **35,573** and tests **44,463** CLOC. MR-9C
closes at production **35,850** and tests **45,277** CLOC: stage **P +277 / T
+814**; cumulative MR-1..9C production is **-636** from the 36,486 baseline.
This disproves the +40..+70 production forecast but remains inside the user's
explicit +1,600-CLOC ceiling. The irreducible increase is the typed physical-
tree interfaces/events and post-mutation exactness needed where Qt's flat item-
view events cannot address the authoritative nested hierarchy. Both final
read-only audits found no clarity-preserving production deletion; micro-
compression would obscure state masks and lifetime ordering.

Final commands and results:

- `cmake --build ../build/Qt-Remediation-Release -j14`: passed, including the
  application and every test target.
- `ctest --test-dir ../build/Qt-Remediation-Release --output-on-failure -j14`:
  **64/64 passed** in **74.40 s**. All **29/29** registered quantitative gates
  passed in **63.24 s** of aggregate labelled time. The first complete run
  exposed one stale Shell assertion; after replacing it with the native Button
  disabled/List busy contract, the focused Shell test passed and the complete
  matrix passed.
- The same run passed Conversation focus pixels under Fusion and Breeze and
  Thread/Inspector geometry at requested DPR **1.0, 1.25, 1.5, and 2.0**. The
  final strengthened Thread expansion-mask target passed separately after the
  full run.
- `/tmp/codexui-h11-warnings.LDqrVn` rebuilt the three affected suites with
  `-j14`: passed with no warning in MR-9C production source. Existing fixture
  aggregate-initializer/sign warnings remain visible and are not suppressed.
- `git diff --check`: clean. Static audit found no added persistent timer,
  cache, flag, callback, renderer, or accessibility authority.

Qt 6.6 compilation, active AT-SPI/platform receipt, possible native
`QTreeWidget` table-model event leakage beside the semantic tree, compositor-
present timing, sanitizer refresh, and non-Linux breadth remain MR-9F and are
not claimed by the offscreen Qt 6.10.2 run.

### M-45 oversized resume frame and paginated history boundary

**Status: complete after controller and observer live transport qualification
(2026-09-18).** A very long
thread caused `JSONL frame exceeds configured maximum before delimiter` in the
bridge/app-server transport. Local tracing verified that CodexUI could request
the complete retained history in one `thread/resume` or `thread/read` response,
while codex-bridge correctly retained a bounded frame guard. Upstream Codex
issue [#40362](https://github.com/openai/codex/issues/40362) independently
reproduces the same shape: a 23,159,303-byte resume record falls to 5,104 bytes
with `excludeTurns:true`. This is a reproduced protocol-boundary defect, not a
JSON parser defect and not permission to make framing unbounded.

The violated invariants are I-2/I-3/I-5/I-6/I-7/I-10: provider history has one
graph authority; transport work and response size must be bounded; controller
and observer hydration must merge through that authority; performance must not
create a second history store or presentation path. The accepted remediation
deletes every automatic complete-history request. A controller first performs
`thread/resume(excludeTurns:true)` to establish metadata/live state and then
requests `thread/turns/list` pages with `limit:80`, descending order, and
`itemsView:"summary"`. An observer cannot resume and therefore starts with the
read-only turn page. Each returned Turn schedules `thread/items/list` pages of
80 through an exact eight-request concurrency ceiling. Existing
`WorkerLogic`/`ProtocolUpdater` and stable graph identities remain the sole
merge and ordering authority; the scheduler retains only pending page work and
no history cache. Metadata-only fork uses the same page path while its first
prompt remains immediately admissible. Browser controller hydration now uses
the same `thread/resume(excludeTurns:true)` then first-turn-page ordering;
browser observers start directly with the read-only turn page. Both frontends
become ready only after that first page and retain bounded cursor continuation,
global eight-request item hydration, active-child hydration, and provider-
generation/stale-result rejection.

AISuite codex-bridge now permits the two pagination methods for observers and
restores each frontend request ID on the response. It does not rewrite frontend
requests, remove the frame guard, or grant observer mutation authority. Exact
source provenance for the installed Codex 0.154.0 binary was recovered from
npm's signed build attestation: tag `rust-v0.154.0`, commit
`6b9826e3aa83b1a5947db50f4332cb9c65f1b340`. Regenerating AISuite against that
exact experimental schema expands generated bindings substantially. The
earlier rejection of that expansion as a CodexUI-only patch is superseded by
the later verified generator-drift finding: AISuite must type the complete
public schema it claims to generate, and CI must reproduce it. The published
AISuite repair now does so. CodexUI pins that immutable revision, uses generated
bindings for eight operations, and retains only the two upstream internal-only
`rawResponse*` operation identities with generated payload types; this is not a
second protocol implementation.

The legacy `thread/read` operation itself remains for bounded metadata reads.
Only `includeTurns:true` and implicit full-history resume/fork behavior are
removed. No version-selected full-history fallback is accepted: a version is
not a negotiated method capability, and selecting the old path for an old
server would recreate the failure on precisely the servers/threads needing
protection. A server without pagination receives the bounded request, returns a
normal unsupported-method/error response, and CodexUI reports that paginated
history support is required. Supporting such a server safely would require a
genuinely bounded protocol supplied by that server, not a client-side retry of
the unbounded operation.

The authoritative interruption plan is:

1. [x] **M-45A:** preserve the JSONL limit; delete automatic full-history
   resume/read/fork; implement one metadata-plus-pages path in native and
   browser clients; allow exact observer page methods; preserve one graph
   merge authority and bounded concurrency.
2. [x] **M-45B:** add native/browser/bridge regressions for request parameters,
   cursor continuation, stable chronological merge, observer routing and ID
   restoration, metadata-first controller ordering, competing-snapshot
   admission, fork prompt independence, and the exact eight-page concurrency
   ceiling.
3. [x] **M-45C:** build the new bridge and execute source-level Release/native,
   browser, frontend-SDK, routing, framer, and provider transport gates without
   installing or restarting it.
4. [x] **M-45D:** the restarted
   controller loaded the 341 MB `Big architectural refactoring` rollout through
   one `thread/turns/list` and one `thread/items/list` request without an
   oversized-frame error, transport restart, or request storm. The only fresh
   observer was then restarted and the same long thread loaded successfully
   through the bounded paginated path without an oversized frame or transport
   failure. Controller and observer share the same graph merge/order authority.
5. [x] **M-46** is disproven as a CodexUI defect and **M-47** is implemented
   and live-qualified below. MR-9D/MR-9E are complete; MR-9F retains only the
   explicitly unavailable S8 platform/AT-SPI execution breadth.
6. [x] **M-45E:** remove the browser's remaining legacy/eager divergence. The
   browser no longer selects with `thread/read`, recursively drains every turn
   page, marks the thread ready before its first page, or applies an eight-page
   ceiling independently per turn. Controller/observer ordering, one-page
   continuation and retry, chronological item-page merge, active-turn recovery,
   active-child hydration, and the global item ceiling are browser-session
   contract cases. CodexUI now pins AISuite `24b1d16...`, whose bridge permits
   both observer page methods and whose generated bindings cover the public
   experimental operations; CI reads that one revision file for native and web
   checkouts. The CodexUI wire path remains runtime-compatible through the
   existing SDK request boundary.

AISuite's isolated implementation delta is production **+2 CLOC** and tests
**+15 CLOC**. The two production lines extend the existing observer read
allowlist and replace no safe behavior; reduction in CodexUI removes the much
larger unbounded request path. Cross-repository CodexUI stage accounting and
the final exact command/result list close with M-45D so that live-gate fixes are
not hidden as a second stage. No bridge or CodexUI binary has been installed or
restarted.

M-45C exact-current results:

- `cmake --build ../build/Qt-Remediation-Release --parallel 14 && ctest
  --test-dir ../build/Qt-Remediation-Release --output-on-failure --parallel
  14`: build passed; **64/64** tests passed in **74.56 s**, including all
  **29/29** registered quantitative gates and the DPR/accessibility matrices.
- `npm test --prefix web`: **285/285** passed, including the exact eight-request
  item-page ceiling and metadata/turn/item separation.
- `cmake --build ../build/Sanitizers-ASan --parallel 14 --target
  codexui-client-runtime-dispatch-test
  codexui-current-protocol-adapters-test codexui-shell-integration-test` plus
  the three-case ASan/UBSan CTest selection: **3/3** passed with leak/error
  halting in **14.56 s**.
- In AISuite, the Release build of `codex-bridge`, `CodexBridgeRoutingTest`,
  `CodexFrontendSdkTest`, and `CodexJsonLineFramerTest` passed. The exact
  transport selection (`CodexJsonLineFramerTest`, `CodexFrontendSdkTest`,
  `CodexBridgeRoutingTest`, `CodexProvider_stdio`) passed **4/4**; the frontend
  package passed **20/20**.
- `clang-format --dry-run --Werror` passes the affected CodexUI C++ files and
  the modified AISuite routing test; `git diff --check` passes both
  repositories. AISuite had unrelated pre-existing whole-file formatting
  drift in `CodexBridge.cpp`, outside the two inserted allowlist lines.
- The built bridge is
  `/path/to/workspace/drafts/AISuite-extraction/build/Desktop_GCC-Release/src/apps/codex-bridge/codex-bridge`,
  SHA-256 `39eb7d2437908b5d80d39373c16023bee4b362fedb3cfce7ababc4be6aae8beb`.
  `/usr/local/bin/codex-bridge` remains the older binary, SHA-256
  `ba533a105f1d2745887bc2a67857c366834abae00ae31f79e6df1bb73e92b1ed`.

The post-restart live gate supersedes the final installed-binary sentence
above without erasing its pre-restart provenance. The production bridge is now
PID 2935081 and the production controller is PID 2936296. The bridge contains
the `thread/turns/list` and `thread/items/list` observer allowlist entries; the
controller remained connected after both page requests and emitted no frame-
limit error. Exact raw request parameters were not retained on the wire, so
`excludeTurns:true` is corroborated by the exact running source path rather
than claimed from a packet capture.

M-45E exact-current verification: `npm test --prefix web` passed all **11/11**
browser suites, with **42/42** browser-session cases. `npm run profile --prefix
web` measured hydration **36.09 ms**, projection **20.92 ms**, streaming **3.20
ms**, and presentation churn **8.87 ms**, within the respective **47/45/4/20
ms** hard limits. A clean temporary extraction of pinned AISuite
`bc448516...` built its SDK, compiled the current browser, and passed all
**42/42** session cases without using the local regenerated bindings. `npm run
build:app --prefix web` and `npm run verify:artifact --prefix web` passed. The
current restricted execution sandbox denied the
qualification tool's local HTTP listener (`listen EPERM 127.0.0.1`), so a fresh
Chromium workflow run is not claimed by M-45E; the production artifact itself
was built and verified.

### M-46 final-answer parent and transcript-order report

**Status: disproven as a CodexUI defect (2026-09-17).** The two cards were both
present and their displayed order matched the provider/app-server event order:
the long-running command genuinely completed after the final answer. No
timestamp inference from the screenshot was valid or required. Provider child
order remains the sole authority established by MR-2, so the proposed
final-answer-specific reorder was reverted and no presentation rule, sort,
timer, cache, or parent override remains. A future mismatch is actionable only
if the graph/model order first diverges from captured provider order; the view
must never manufacture a preferred transcript order.

### M-47 incoming-event GUI backlog and scrolling regression

**Status: implemented and live-verified (2026-09-18).** The repair removed
redundant GUI-thread work at its owning boundaries: exact deferred
`GraphChanged` FIFO delivery, stable semantic card identity, bounded row
admission, latest-wins worker projection, off-GUI Markdown preparation, and
no-op DiffViewer updates. It did not add a renderer, presentation throttle,
skipped visible update, snapshot authority, or reconciliation path.

The autonomous offscreen/Xvfb interaction matrix covered Conversation,
Threads, Inspector, and prompt surfaces both with a long idle thread and while
bridge messages arrived. Every sustained path recorded p99 16 ms and no
repeatable frame above 24 ms; the sole 29 ms first-activation Inspector sample
did not reproduce in an immediate 300-sample rerun (maximum 18 ms). Temporary
timing probes were removed. This closes the reproduced M-47 defect under the
user-mandated offscreen live workflow; native compositor qualification remains
the separate S8 platform item and does not reopen M-47.

### S5 H-11/M-07/M-08/M-09 thread-tree stage gate

**Status: complete after M-37.** H-11 now has one ThreadPane hierarchy, state
projection, renderer, native interaction implementation, and semantic
accessibility representation. Offscreen loaded threads remain lightweight
item-model data; one delegate paints visible rows. A semantic no-op changes no
item, model index, expansion, selection, focus, scroll geometry, pixel, timer,
or accessible identity.

**Authoritative H-11 completion status (2026-09-16).** This is the final H-11
record; checked entries are completed and verified rather than merely started.

- [x] Replace the five-widget flat row reconstruction with one native tree,
  stable presentation-keyed items, and one full-row delegate; delete all index
  widgets, duplicate disclosure interaction, snapshot mirrors, expansion maps,
  dynamic diagnostic properties, and viewport-wide animation painting.
- [x] Preserve hierarchy, reparenting, sorting, selection, scroll anchors,
  pagination triggers, context actions, native keyboard navigation, and exact
  action `NodeRef`s without a second model or paging authority.
- [x] Preserve in-place optimistic draft admission and local-to-canonical
  promotion; retire drafts in one signal-atomic transaction after Qt finishes
  its current-index transition, and detach every item before destruction.
- [x] Project one semantic accessibility tree over the same native items,
  including parent/level, expanded/collapsed state, focus, selection, geometry,
  actions, and exact wrapper retirement.
- [x] Preserve the established Thread panel appearance and correct the locally
  reproduced chevron/status spacing drift to the old 2 px edge gap; connector
  branch painting remains suppressed. The native disclosure target is restored
  to the former 24 x 24 logical-pixel control through Qt's public disclosure
  subelement, with no custom mouse forwarding or pixel change.
- [x] Migrate Shell and established-UX consumers/tests to the native hierarchy;
  focused and ShellIntegration Release plus ASan/UBSan runs pass without a
  lifetime diagnostic.
- [x] Register and execute all twelve quantitative 320/1,280/10,000-row DPR
  1.0/1.25/1.5/2.0 gates; all pass with 18 fixed widgets, zero index widgets,
  one fixed document, zero offscreen animation paints, five bounded visible-row
  paints, and stable no-op pixels/focus/anchor.
- [x] Replace the four remaining fixed-delay H-11 integration waits with their
  existing semantic identity/acknowledgement predicates and rerun the complete
  relevant suite without short-circuiting; the Release focused, application-
  layout, and ShellIntegration gates pass, and the corresponding focused and
  ShellIntegration ASan/UBSan runs report no diagnostic.
- [x] Complete final visual capture/comparison, production/test LOC accounting,
  diff special-case audit, and ledger closure. No compatibility renderer,
  custom input forwarding, unbounded row object, or known H-11 workaround
  remains.

Deletion comes first. This stage deletes the five-QWidget-per-row subtree and
`setItemWidget` path, the second disclosure/status/title paint path, custom
disclosure hit testing and Left/Right navigation, `RenderedThreadList`, the
retained deep `currentSnapshot`, `expandedRows`, the separate optimistic-row
vector, moved-widget reconstruction, manual selection restoration, three
dynamic diagnostic properties, and whole-viewport 32 ms animation invalidation.
It does not restore the rejected lazy index-widget implementation, add a proxy
model, or retain list and tree implementations together.

The accepted replacement is one `QTreeWidget` hierarchy with stable items keyed
by presentation lifetime and one full-row delegate. Each item is the sole Qt
projection of its thread or pre-admission draft and retains the exact action
`NodeRef`; draft admission updates that same item by creation correlation. One
derived key-to-item lookup accelerates exact updates but owns no semantic fact.
Native tree branches own disclosure hit testing and Left/Right behavior.
Structural reconciliation reuses items and changes only affected sibling spans;
content updates are exact-item operations. The existing runtime remains the
sole thread-page cursor/single-flight authority.

Local Qt 6.10.2 diagnostics and supported-Qt-6.6 source inspection disprove the
idea that the stock accessible tree is sufficient. Stock rows are flattened
under the Tree, expose no semantic parent/level, never report `collapsed`, and
their `Toggle` action changes selection rather than expansion. Qt 6.6 also
allocates a fresh row interface per query. The accepted accessibility boundary
therefore lazily wraps the real tree/model indexes by presentation identity,
projects direct semantic parents/children, selection/focus/geometry, mutually
exclusive expanded/collapsed state, and a real expansion action, and retires
the wrapper with the exact item. It stores no copied hierarchy, row value, or
proxy QObject tree. Item presentation uses custom roles so Qt cannot emit its
private flat-row child indexes as accessibility events; correct events target
the one semantic interface.

M-07 is narrowed by current-worktree evidence. Its relative-time claim is
**disproven** because the pane renders absolute local time or date, not an aging
relative label. Its missing pane-local pending gate is **superseded** by
`ClientRuntime`'s authoritative pending/repair/cursor-generation/repeated-cursor
and queued-load-more state; adding another latch would duplicate authority.
Root-only user sorting while children retain provider relation order matches the
documented contract and is **not a defect**. The verified M-07 remainder is the
manual flat rebuild/current-index restoration path, which this stage deletes.

The exact entry accounting is native production **36,252 CLOC**, native tests
**39,618 CLOC**, `ThreadPane.{cpp,h}` **1,158 CLOC**, and its focused test
**616 CLOC**. The existing seven-case focused executable passes Debug in 0.12 s
and exact-current Release in 0.128 s; these are correctness-only baselines.
Source accounting proves 5 row QWidgets plus one layout per loaded row. The
test-side real-graph profile now measures, across actual DPR
1.0/1.25/1.5/2.0: 320 rows at 2.7--2.8 ms adapter / 144.7--145.4 ms population
with 1,612 QWidgets, 320 index widgets, and 321 `QTextDocument`s; 1,280 rows at
14.6--14.9 / 593.5--606.6 ms with 6,412/1,280/1,281; and 10,000 rows at
385.8--391.8 / 6,844.3--6,984.2 ms with 50,012/10,000/10,001. The documents
are QLabel implementation documents, not Markdown state, but are still
unbounded retained objects. Exact semantic no-ops preserve model/pixels/focus
and cost 1.09--1.22, 5.12--5.37, and 49.7--50.7 ms respectively. One offscreen
pending row leaves the timer active and causes five full-viewport paints in
170 ms; the visible case also paints five times with a dirty region larger than
the affected row.

The profiler was added before production edits and is run directly in baseline
mode with `QT_QPA_PLATFORM=offscreen`, `QT_SCALE_FACTOR=<dpr>`, and
`codexui-nodegraph-thread-pane-ui-test <rows> <dpr> baseline`. Production
remains **36,252 CLOC**. The executable's test source is now **883 CLOC** and
native tests **39,885 CLOC**, +267 from stage entry; registering the matrix and
replacing implementation-coupled correctness assertions follows with the
renderer change.

The proven accessibility requirement widens the honest candidate range to
native production **−100..+180 CLOC** after the model-backed accessibility
implementation; this was an uncertainty range, not approval for growth. Native
tests were estimated at **+300..+750 CLOC** after deleting old topology/counter
assertions and adding the requested quantitative matrix. During implementation
the user explicitly approved a separate **+1,600 production-CLOC H-11
ceiling**, superseding the earlier positive-growth stop; the completed H-09
allowance is not reused. Acceptance requires
320/1,280/10,000-row profiles at actual DPR 1.0/1.25/1.5/2.0, zero index
widgets or row documents, N-independent QWidget residency, row-local visible
animation and zero offscreen animation work, stable item/accessibility identities,
semantic tree parents/levels/states/actions, native keyboard navigation,
selection/context/promotion/reincarnation correctness, scroll-anchor and focus
retention, and Debug/Release/sanitizer verification. Reduced-motion policy
remains H-14 unless it can be obtained by deletion in this stage; real AT-SPI
and native XCB/Wayland remain S8 qualification.

**Final H-11 result and accounting (2026-09-16).** The exact original H-11
entry was native production **36,252 CLOC**, native tests **39,618 CLOC**,
`ThreadPane.{cpp,h}` **1,158 CLOC**, the focused ThreadPane test **616 CLOC**,
`ShellIntegrationTest.cpp` **4,767 CLOC**, `EstablishedUiUxTest.cpp` **615
CLOC**, and root `CMakeLists.txt` **696 CLOC**. Before H-11 paused for M-34, its
partial header and profiler work had already moved those totals to production
**36,209** and tests **39,885**. M-34--M-37 then account for production **-6**
and tests **+1,407**, reaching the separately recorded post-M-37 checkpoint of
**36,203 / 41,292**. This reconciles, rather than mixes, original entry,
intervening work, and resumption accounting.

The final worktree is native production **36,460 CLOC** and native tests
**41,979 CLOC**. Isolated H-11 is therefore production **+214 CLOC** and tests
**+954 CLOC** across its pre-pause and resumed work: `ThreadPane.{cpp,h}` is
**1,380 CLOC** (**+222**), Shell removes **8** production CLOC, the focused
test is **1,529 CLOC** (**+913**), ShellIntegration is **4,809 CLOC** (**+42**),
and Established UI/UX is **614 CLOC** (**-1**). The same result viewed only
from the post-M-37 resumption checkpoint is production **+257** and tests
**+687** because the earlier **-43/+267** partial H-11 work was already present.
Root CMake is **736 CLOC** (**+40 configuration CLOC**) after durable functional
and quantitative DPR registration. The physical H-11 diff is production
**+1,183/-933 lines**, tests **+1,528/-528 lines**, and root CMake **+42/-2
lines**. Production finishes 34 CLOC above the +180 estimate but 1,386 below
the explicitly approved +1,600 ceiling; clarity and Qt lifetime safety were
not compressed to fit an obsolete estimate.

The final mechanism audit finds no new cache, retry, polling loop, row widget,
row document, proxy model, dynamic functional property, or parallel renderer.
The existing single animation timer is retained but now runs only for visible
animated items and invalidates exact row rectangles. One typed
`selectionDispatchPending` bit coalesces the final native current-index change
after Qt completes its transition, replacing unsafe synchronous re-entry. One
typed sort-action association replaces incidental QObject child ordering. The
tree-local `QProxyStyle` changes only `SE_TreeViewDisclosureItem`, the public
rectangle consumed by QTreeView's own expand/collapse interaction; it restores
the exact former 24 x 24 target and adds no mouse handler or forwarding path.
Stable item/accessibility identity and one derived presentation-key lookup
replace the deep snapshot, expansion set, optimistic vector, and per-row
widget topology.

The exact pre/post ThreadPane raster under the application and tree style
sheets has **AE 0 / RMSE 0** (every pixel identical); an independent capture
also has the same SHA-256 before and after. Root and nested mouse-boundary tests
prove both axes of the restored native target, and four no-argument functional
CTest registrations execute those checks at DPR 1.0, 1.25, 1.5, and 2.0.
Release, Debug, and ASan/UBSan each pass the complete **18/18** H-11 matrix;
the entire Release suite passes **54/54**. The full `codex-ui` application and
the three H-11-dependent executables build in Release, Debug, ASan/UBSan, and
the warning configuration. The warning build introduces no warning in the
changed ThreadPane/focused sources; existing aggregate-initializer warnings in
other current-worktree production/tests remain classified for the later
repository-wide warning stage.

At 10,000 rows across DPR 1.0/1.25/1.5/2.0, the final Release profile measures
adapter **23.580--27.518 ms**, population **14.523--17.550 ms**, and semantic
no-op **3.922--5.675 ms**, versus baseline **385.8--391.8 ms**,
**6,844.3--6,984.2 ms**, and **49.7--50.7 ms** respectively. Every final
profile has **18** fixed/populated widgets, **0** index widgets, **1** fixed
document, **0** no-op paints, stable no-op pixels/focus/anchor, **0**
offscreen animation paints with no active timer, and **5** bounded visible-row
animation paints. Real AT-SPI and native XCB/Wayland breadth remain explicit S8
qualification; they are not unfinished H-11 implementation.

### S3 read-only evidence reconciliation and dependency order

Three independent read-only traces agreed on the following implementation
order. The primary agent reconciled these results before the first S3
production edit.

1. **H-05 projection boundary first.** Shell currently has four commit paths:
   bounded card updates, prompt materialization, tail append, and general row
   change/removal with a Shell-owned dependency DAG and snapshot fallback.
   The adapter exposes the matching four projection APIs, while the full
   snapshot repeats prompt alias, root, active-turn, and placement policy. The
   accepted replacement is one delta value crossing the graph-lock boundary,
   with ordinary presentation updates and canonically ordered structural
   changes projected by one adapter entry point. `ConversationView` applies
   that value through one public delta transaction; append is an internal
   geometry shape, not a second public presentation contract. Shell retains
   only coalescing and the measured eight-presentation-rows-per-pass schedule.
   A full `conversation()` snapshot on every delta was rejected because it
   would scan/copy retained history and invalidate the 10k-row append/index
   gates. Expected production effect: −150..−350 CLOC; tests neutral to +120.
2. **M-05 is an independent deletion after H-05.** Shell's runtime-stopped
   callback is overwritten by `main.cpp` and Shell destruction can then erase
   the application-owned callback. Keep the application-lifetime registration
   and delete Shell's registration and clear. Expected production effect:
   exactly −7 CLOC; test +18..+25.
3. **H-04/M-06 history operation authority.** There are currently three
   pending authorities: `ConversationView::HistoryWindow`, Shell's
   `historyPageAwaitingProvider`, and `ClientRuntime::pendingHistoryLoads`,
   while a live `Operation(method=thread/turns/list)` already spans admission
   through result/error and targets the exact Thread. The accepted direction
   derives pending from that Operation, deletes both extra guards, and changes
   the view's requested limit into one demand watermark so a failed retry does
   not add another page. Exact single-flight enforcement remains worker-owned.
4. **H-06/M-04/M-09 optimistic creation.** `WorkerLogic` atomically admits a
   local Thread/Turn/Prompt and later migrates that graph-owned prompt to the
   canonical thread; ThreadPane and Shell then duplicate the same admitted
   lifecycle with an overlay, correlation state, full-viewport animation, and
   zero-delay selection retries. Keep only the pre-admission authored draft in
   Qt. After admission, adapter rows derive pending/failed state from the graph
   prompt. Stable row/focus identity must use the existing creation
   correlation across local-to-canonical promotion; no second lifecycle owner
   or polling timer is accepted. Exact identity shape remains a substage gate
   until the current thread projection is traced end to end.
5. **H-07 request policy after projection/operation cleanup.** Native method
   classification exists in both Shell and adapter; Inspector direct-accept
   helpers and dialog decision choices duplicate `PendingRequestPolicy`; Shell
   also duplicates response flattening. Extend the existing policy and one
   adapter request projection while deleting all local copies. Provider method
   and payload stay raw graph facts. Browser remains a separate renderer and
   will execute shared semantic fixtures. The provider-declared command
   decision mismatch (native accepts arbitrary values; browser filters known
   values) is unresolved and must be explicitly decided before that edit.
6. **M-01 deliberately remains unresolved.** Typed `UiEffect` is the normal
   path, but queue-full handling writes graph fallback facts and saturation
   tests prove those facts recover otherwise lost notice/selection delivery.
   Deleting either path now would lose behavior. S3 will not manufacture a
   synchronizer; one durable authority requires a later evidence-backed queue
   decision.

### Final S3 implementation and evidence reconciliation

| Finding | Deleted or consolidated authority | Final classification and verification |
|---|---|---|
| H-05 | Deleted the public card/prompt/tail/row projection fork, Shell-owned placement DAG and prompt acknowledgement copies. Full snapshots and exact changes now use adapter-owned placement; `ConversationView::applyConversationDelta` is the one delta boundary. | **Implemented.** Root replacement is preflighted and applied as one final section transition. Focused model/view tests cover reorder, append, removal, prompt morph, geometry, focus, selection, anchors and bounded eight-row delivery. |
| H-04 / M-06 history | Deleted Shell `historyPageAwaitingProvider` and runtime `pendingHistoryLoads`; retained one view demand watermark and the graph `Operation` relation as the in-flight fact. | **Implemented.** Admit/success/error/retry, pagination, busy state and anchor behavior pass the adapter, Shell and runtime suites. |
| H-06 / optimistic slice of M-04/M-09 | Deleted the post-admission ThreadPane overlay, duplicate Shell correlation lifecycle, full-viewport animation and zero-delay selection retries. Retained only the pre-admission draft and graph `creationCorrelation` identity. | **Implemented.** Local-to-canonical promotion, failure, reconnect, duplicate creation, selection and draft isolation pass. General focus ownership and general reduced-motion policy remain S6/S5 work. |
| M-05 | Deleted Shell installation/clearing of the runtime-stopped callback; application lifetime owns the one handler. | **Implemented.** Destruction and stop delivery are covered without a second callback owner. |
| H-07 | Deleted Shell/Inspector/dialog method maps, response flattening, direct-decision copies, raw command/URL bypasses, retained-response map and repeated command decision parsing within a submission. One native `PendingRequestPolicy` owns kind, review context, actions, controls and wire response. | **Implemented for the verified contract.** Review work is bounded by collection, depth, node and UTF-8 byte limits and fails closed. Result/error is an exact C++ variant and an exact TypeScript union. Native and browser execute the same request/action/response and boundary fixture cases. |
| H-07 request lifecycle | Deleted wire-ID interaction scanning and result-without-operation graph updates. All production completion paths now require the exact current `NodeRef`; success and failure are distinct updater transitions. | **Implemented.** Reused IDs, missing operations, stale identities, synchronous resolution, send failure, retained authored response and retry are exercised. No timer, poller, response cache or functional QObject property was added. |
| M-24 request slice | Replaced copied expected literals with schema-v4 shared data for 29 request cases, 35 submissions, 93 semantic checks per frontend and eight explicit boundary cases. Frontend-local suites retain honest non-parity names. | **Implemented for kind/action/response/bounding meaning.** Native/browser disclosure layout remains renderer-specific; identical disclosure pixels/entry formatting are neither claimed nor required. Broader lifecycle/platform/Unicode qualification remains S8. |
| M-01 | No safe deletion exists while bounded `UiEffect` delivery can saturate and graph fallback facts recover otherwise lost effects. | **Unresolved, carried to S6.** Adding a synchronizer or deleting recovery would violate the authority/durability invariant. |
| M-20 | Turn settings are an independent policy/serialization authority and were not changed under request remediation. | **Verified, carried to S5.** This is an explicit dependency correction, not a disappeared S3 finding. |

Repeated calls to the one request policy are classified as bounded ephemeral
re-evaluation, not parallel state or implementation. Keeping no cached policy
result prevents a stale second authority at response time. Inspector all-row
residency and repeated descriptor copying remain H-09/H-10/M-28 work in S5.
The shared fixture proves common safety meaning through actions and wire
responses; it intentionally does not assert identical native/browser detail
layout. Standard-MCP schema/content validation and exact file-change patch
context plus stale-revision gating remain unresolved correctness hypotheses
behind their existing explicit production-growth approval gates.

Final exact-current verification:

- `cmake --build /tmp/codexui-h07-build.IkJECO -j2`: success.
- Focused request/runtime/adapter/updater/worker/Shell CTest set: 6/6 passed.
- `ctest --test-dir /tmp/codexui-h07-build.IkJECO --output-on-failure -LE performance -j2`: 19/19 passed after exact-identity deletion.
- Final exclusive full Debug CTest run: 31/31 passed; all twelve registered quantitative conversation cases passed at 320, 1,280 and 10,000 rows and DPR 1.0, 1.25, 1.5 and 2.0.
- `npm test -- --runInBand`: 251/251 passed.
- `npm run profile`: 49.05/36.30/9.98 ms for hydrate/project/stream versus 45.56/35.31/9.73 ms at S3 entry. No browser presentation algorithm changed; the single-sample variation remains below 8% and is carried as measurement noise rather than claimed improvement.
- The final exclusive Debug conversation group took 82.60 s versus 80.40 s at S3 entry (+2.7%); every semantic and quantitative bound passed. A prior 574 ms no-op outlier reproduced only under competing benchmark processes while reporting zero layouts/paints and semantic stability. `RUN_SERIAL` covers one CTest scheduler, so final performance qualification uses a shared external lock; thresholds were not weakened and production was not patched for host descheduling.
- `git diff --check`: clean. Current Release, ASan/UBSan, native XCB/Wayland, real assistive-technology and warning qualification remain explicitly assigned to S8 rather than falsely claimed here.

These conclusions preserve lazy materialization: widgets remain bounded to
the viewport and overscan, offscreen rows remain model data plus scalar
heights, and performance is measured before and after each substage. What is
being removed is parallel projection/application policy around that residency,
not residency itself.

### S3 Xvfb/xcb requalification

**Reopened after explicit user-requested platform qualification; implementation
complete and final matrix in progress.** The earlier S3 completion relied on
CTest properties that force `QT_QPA_PLATFORM=offscreen`; it therefore did not
qualify the real `xcb` plugin and was an incorrect completion claim. A first
direct eight-executable bare-Xvfb run passed five executables, failed Copy/focus,
residency/anchor and prompt-focus assertions, and aborted the virtualization
executable in `QWidget::mapToParent`. GDB localized that abort to
`keyboardNavigationAndViewIsolation`, after its no-window-manager focus
precondition had failed. Repeating with `xfwm4` removed the activation/focus
failures and the abort, leaving two virtualization assertions and three Shell
routing assertions.

The current-worktree evidence classifies those remaining failures as test
authority/lifetime defects, not production-renderer defects. The paging test
captured 15 tail residents before scrolling and then compared that obsolete
count with 16 legitimate head residents while staging. The residency test used
painted card geometry as a second residency authority; row 93 was correctly
retained because its scalar row extent, including inter-row space, crossed the
overscan boundary even though its card ended two pixels above it. The Shell
test captured its route baseline before the already-designed frame-coalesced
selection transaction published its final Inspector/chrome commit; after that
boundary, the background delta changes neither route. Finally, raw card/control
pointers survived a failed focus precondition and recycling, allowing a test
failure to suppress later cases.

The accepted correction adds no production mechanism. It moves the staging
baseline to the actual pre-stage frame, derives expected residency from the
existing scalar row extent rather than painted appearance, observes recycled
widgets through `QPointer`, and records Shell routing only after the selected
conversation's existing commit boundary. No tolerance, offset, timer, cache,
flag, renderer, production event suppression, or reconciliation path is added.
Production effect is **0 CLOC**; test effect is **+10 CLOC**. The bare-Xvfb
test now reports all later failures instead of crashing. The corresponding
offscreen virtualization and Shell tests pass, and the corrected
Xvfb/xcb+`xfwm4` tests pass individually.

Required final verification is: all 19 non-performance CTests without
short-circuiting, all twelve quantitative 320/1,280/10,000-row DPR
1.0/1.25/1.5/2.0 gates, and all eight QWidget executables in one
Xvfb/xcb+`xfwm4` session. Accessibility/focus cases are part of that xcb run;
the benchmark must show unchanged bounded widgets/documents/measurements and
per-frame work. Bare Xvfb without a window manager remains an intentionally
recorded invalid focus-activation environment, not a substitute for the full
xcb desktop-like gate. S3 remains **in progress** until those final commands
pass and the diff is audited.
