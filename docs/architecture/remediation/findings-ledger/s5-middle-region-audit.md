<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../../media/nav/banner-findings-ledger-compact.svg"><img src="../../../media/nav/banner-findings-ledger.svg" alt="CodexUI — Findings ledger" width="100%"></picture></p>
<p><a href="README.md#banner" title="Up to Findings ledger"><picture><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/up-findings-ledger-dark.svg"><img src="../../../media/nav/up-findings-ledger-light.svg" alt="Up to Findings ledger" title="Up to Findings ledger" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../../media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../../media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/account-dark.svg"><img align="right" src="../../../media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/organization-dark.svg"><img align="right" src="../../../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../../../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../../media/nav/repository-dark.svg"><img align="right" src="../../../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# S5 middle-region audit and MR-9 evidence

### 2026-09-16 Middle Region adversarial re-audit and superseding plan

This section supersedes the ordering sentence immediately above. It reconciles
three independent read-only audits with an exact-current Release rebuild, the
current source, committed `629660e`, and the user's live observations. It does
not reopen the deleted passive renderer and does not authorize another renderer,
pool, timer, cache, reconciliation layer, or frontend-only ordering policy.

The current worktree has one visible/overscan renderer (`ConversationCard`) and
retains lazy QWidget/document residency. The renderer consolidation is sound.
At re-audit entry the remaining loss and latency defects were outside that
renderer: graph snapshot admission/replacement, controller-only graph order, a
second UI-owned logical history window, excessive hidden staging, incomplete
wheel routing, and selection/focus conflation. MR-1 through MR-6 have since
closed every item in that list except selection/focus conflation and the native
qualification assigned to MR-8.

Evidence classification and accepted remediation:

- **Loaded ordinary-card loss — verified current architecture defect.** The
  adapter selects the newest `AuthoritativeHistoryPageSize` ordinary Items plus
  landmarks; `ConversationView::HistoryWindow` independently expands or resets
  that local window; `ConversationItemModel::trimHistoryTo` physically removes
  already-loaded ordinary rows. A restart resets the process-local window to 80.
  This is distinct from widget virtualization and violates the required
  offscreen-model-plus-scalar-height invariant. Delete adapter suffix/landmark
  membership selection, view demand/effective history state, model trimming,
  retained-hidden counts, and their policy tests. Provider cursors/`hasMore`
  become the sole loaded-history boundary; all loaded graph Items remain model
  rows and only card/document residency stays bounded. Expected P −150..−350;
  replacement tests +100..+300 after deleting defect-preserving cases.
- **M-34 — complete for the exact reproduced missing-You defect, superseded in
  architecture by deletion of the wider UI history window.** Current code keeps
  every loaded `UserMessage`, so a current You/root disappearance is not caused
  by the local 80-row trim. Do not add another You special case.
- **M-37 — complete in its narrow evidence-backed scope.** Exact request
  revision plus typed Item lifecycle evidence protects post-request completed
  Items. It remains a source-risk repair rather than the reproduced M-34 cause.
- **M-38 — verified open defect.** Hydration/reload and Load More use
  method-specific admission authorities, allowing `thread/read` and
  `thread/turns/list` to overlap for one Thread. Use one graph-derived pending
  snapshot predicate covering read/turns-list (and items-list if enabled) at
  every admission and pending-UI boundary. Process-local sets may retain only
  callback/queue bookkeeping, never admission authority. Expected P −10..−35;
  T +100..+180.
- **M-39 — verified latent reducer defect.** `thread/items/list` computes a
  correlated request boundary but drops it at `ingestItem`. Pass the existing
  boundary; add no state. The installed runtime does not dispatch this endpoint,
  so it is not classified as the live loss cause. Expected P approximately 0;
  T +60..+120.
- **M-41 — verified open source risk, exact live chronology unresolved.** A
  post-request empty/current Turn can be removed by a delayed older snapshot
  because M-37 recognizes only a direct lifecycle-proven Item. First test the
  empty `turn/started` shape, then use existing canonical identity, status and
  revision evidence if sufficient. If the graph cannot distinguish provider
  lifecycle provenance without new state, stop for approval. Expected P 0..+20
  only if absorbed into the existing classifier; T +120..+220.
- **M-43 — verified controller/observer order defect.** Controller prompt
  correlation rewrites Turn children and snapshot replacement permanently
  reinjects the private submitted slot; observers retain provider order. Delete
  both order mutations and make provider child order the sole semantic order.
  Retain the local presentation key only as the explicit transient identity for
  in-place QObject/focus/selection promotion; it must not influence order or
  membership. Gate identical controller/observer content order over independent
  graphs. Observer display before a provider notification is impossible under
  the present bridge contract; if notification-to-paint remains unacceptable
  after staging reduction, a shared bridge admission event is new cross-repo
  architecture and requires explicit approval. Expected CodexUI P −50..−120;
  T +140..+260.
- **M-42 — verified source cause, executable regression still required.** A
  hidden staged command consumes its construction-time zero-delay tail
  settlement before final reparent/show. The replacement width authority omitted
  the old resize handler's immediate/deferred settlement, while a new direct card
  still has its queued callback. Make existing `CommandOutputView::settleWidth`
  settle the existing follow/detached owner and delete redundant constructor
  settlement. Add no timer/flag/cache/API. Expected P approximately 0; T
  +80..+110.
- **S1-D10/M-14 — reopened, verified incomplete, and completed in MR-6.** The central router recorded
  a native owner but consumes crossed events instead of delivering them to that
  owner; zero-delta/End outer events are also dropped. The passing test uses an
  orphan output and does not assert crossed-owner movement. Keep one router,
  delete child boundary arbitration, preserve exact wheel metadata and semantic
  owner through crossing/reversal/boundary/End, and test real materialized cards.
  Command output retains only follow/detach semantics and commits reattachment at
  the gesture boundary. Final effect P -22/T +260; the test forecast grew only
  because final adversarial audits added side-pane, semantic-no-op, fractional
  coordinate, and registered routed-performance gates.
- **M-44 — verified and completed in MR-7.** Card selection was painted as a forced
  keyboard focus primitive, and focus on any child performs `ClearAndSelect`.
  Delete card-local selected/focus state and child-focus-to-row-selection
  coupling. `QItemSelectionModel` remains semantic/accessibility authority;
  genuine keyboard current focus uses view-owned decoration. Final effect is P
  +3/T +145 under the user's explicit +5 production-CLOC approval; the test
  delta includes eight permanently registered style/DPR/accessibility gates.
- **M-35 qualification — implementation retained, completion narrowed.** The
  one-card-per-pass hidden-overscan admission works and must remain. All 12
  registered quantitative offscreen profiles pass. Exact-current 10k Release
  DPR1 measures normal p95 10.815 ms, seek p95 31.472 ms, and initial stage
  169 ms. The same binary on managed XCB measures 19.414 ms, 64.086 ms, and
  316 ms respectively and fails native resize/RSS gates. Therefore registration,
  execution and quantitative gating are complete, but representative native
  smoothness is open. Add a native input-to-painted-frame gate before changing
  scrolling. Do not restore the passive renderer.
- **Hidden structural staging — verified load-latency cause.** A minimum-height
  three-viewport budget constructs 43 cards before publication in the measured
  profile. Stage only the first complete visible frame around the saved anchor
  or following tail, publish atomically, then reuse the existing one-card
  overscan scheduler. Delete the three-viewport budget and synchronous-overscan
  commit special case. Expected P −10..−30; T +50..+110.
- **M-36 — current repair retained.** The two reported malformed-command strings
  pass exact width/height/no-op checks at DPR 1/1.25/1.5/2. The screenshots
  predate the current source. Native Breeze/Wayland confirmation remains a
  qualification item, not a second geometry implementation.
- **Static accessibility is substantially repaired; dynamic events remain open.**
  The materialized real cards form the accessible tree and hidden staging is
  excluded. M-21 still covers mutation/selection/busy/stream announcements;
  Disclosure must derive expanded state from the shared control, Copy needs an
  announcement, and semantic no-ops must emit none. M-16 rich clipboard MIME,
  H-14 reduced motion, focus modality, DPR-correct thumbnails, and M-13 token
  literals also remain open in their existing later stages.

Exact-current pre-edit baseline for this re-audit:

- focused Release correctness suites pass **9/9**:
  adapter, graph-conversation, runtime dispatch, cards, item model,
  virtualization, application layout, Shell integration, and protocol updater;
- registered Release conversation performance profiles pass **12/12** at
  rows 320/1,280/10,000 and DPR 1/1.25/1.5/2 under `offscreen`;
- the same native XCB binary exposes the performance failure above, so an
  offscreen pass is not accepted as proof of live smoothness;
- exact pre-MR-1 physical accounting is **36,486 native production CLOC** and
  **42,021 native test CLOC**. The 26/42-CLOC difference from the final H-11
  checkpoint is the already-present, not-yet-accepted S1-D10 implementation and
  test work; it is not attributed to H-11 or hidden in this cluster.

The one authoritative implementation order is now:

1. [x] **MR-1:** M-38/M-39/M-41 snapshot admission and temporal boundary.
2. [x] **MR-2:** M-43 provider-order authority; delete controller-only order
   mutation while preserving transient in-place visual identity.
3. [x] **MR-3:** delete the UI-owned loaded-history window; retain every loaded
   model row and bounded widget/document residency.
4. [x] **MR-4:** reduce hidden structural staging to first-visible-frame work.
5. [x] **MR-5:** M-42 final-geometry command-output follow/detach settlement.
6. [x] **MR-6:** S1-D10 central wheel ownership with real-card regressions.
7. [x] **MR-7:** M-44 semantic selection versus genuine keyboard focus.
8. [x] **MR-8:** native scroll/resize/DPR gates, followed only by measured
   deletion or neutral consolidation of duplicate movement/reflow work.
9. [x] **MR-9:** M-16/M-21/H-14/M-13/M-12 focus, accessibility, motion,
   clipboard, image-DPR, and token findings are implemented. MR-9F retains
   only the explicitly unavailable S8 native-platform/AT-SPI execution breadth.

Completed prerequisites remain explicitly complete: S1 single renderer and
passive-path deletion; S2 semantic fixtures; S3 projection/policy slices; S4
state/scheduler reductions; S5 H-09/M-28; M-32 through M-37 in their classified
scope; and H-11. This re-audit does not erase their evidence or mark the entire
Qt remediation complete.

**MR-1 implementation status: complete.** One graph-derived predicate now
gates `thread/read`, `thread/turns/list`, and `thread/items/list` snapshots for
the same Thread; process-local hydration sets retain callback/queue bookkeeping
only. `thread/items/list` passes its already-correlated request revision into
the existing ingest boundary. The same replacement classifier now recognizes a
canonical Turn whose non-unknown lifecycle status changed after that boundary;
it preserves a post-request empty `turn/started` while still retiring an
otherwise identical pre-request empty Turn. No retry, timer, cache, tombstone,
flag, provenance field, or second authority was added.

Failing-first tests reproduced both competing wire-request directions, the
dropped item-page boundary, and empty-Turn deletion. Exact-current Release then
passes the adapter, graph-conversation, current-protocol, runtime-dispatch,
Shell-integration, NodeGraph, protocol-updater, and WorkerLogic suites **8/8**.
MR-1 accounting is P **+16 CLOC** and T **+149 CLOC**, ending at **36,502 native
production CLOC** and **42,170 native test CLOC**. The production growth is
inside the existing approved ceiling and the complete Middle Region cluster
remains forecast materially negative; MR-2/MR-3 delete the order/window
mechanisms that dominate this cluster.

**MR-2 implementation status: complete.** Provider `children(Turn)` order is
now the sole row-order authority. `TurnRootItem` remains decoration authority:
the first authoritative User in final provider order owns the Turn surface,
rows before a non-first root are standalone, and only following rows are
nested/folded. A prior root is retained only when it is the still-present
explicit local optimistic prompt and no authoritative User exists. The full,
item-page, exact-adapter, model, and hidden-staging paths use this same rule.

This stage deletes submitted-slot reinjection, correlation-time provider row
reordering, root-first `std::rotate`, Worker-side authoritative item transfer,
the adapter's duplicate key override, root-first model validation, and the
implicit prompt-acknowledgement heuristic. Optimistic identity is not an order
authority: one explicit graph-derived `PromptMaterialization` transaction fact
lets a LocalPrompt become the provider User in place, preserving its QWidget,
document, selection, focus, persistent model index, geometry, and accessible
object. Replayed/no-op frames do not acknowledge again. No timer, retained
cache, compatibility renderer, persistent flag, or second state authority was
added; the only new DTO value is this per-transaction relation fact, and the
NodeGraph query uses its existing incoming-relation index.

Regression coverage now includes controller and observer graphs receiving the
same `item/started` notification through the real route -> exact delta -> view
path (without full replacement), provider-order snapshot convergence,
non-first-root exact insertion, root-relative folding, coalesced unseen local
promotion, exact and full replay, destructive callback ordering, and local-root
snapshot races. A non-first same-slot root replacement also preserves its
section-front role throughout both Qt insert/remove signal boundaries instead
of transiently claiming `FirstInTurn`. The full Release tree builds with
`cmake --build /tmp/codexui-release-gate.I6nD6Z -j14`. One non-short-circuit
`ctest ... -j14` invocation passes the 12 relevant correctness suites in
**9.76 s**; the 12 registered conversation profiles at 320/1,280/10,000 rows
and DPR 1/1.25/1.5/2.0 pass in **52.81 s**. `git diff --check` is clean.

MR-2 accounting is P **-58 CLOC** and T **+698 CLOC**, ending at **36,444
native production CLOC** and **42,868 native test CLOC**. An existing exact
root-role handoff between two retained authoritative User rows still rejects
atomically and invokes the established complete-authority projection; neighbor
validation prevents wrong order or partial mutation. This is not a correctness
fallback or a reproduced card-loss defect, but its possible full-projection
latency remains an explicit MR-4 performance qualification. Relation-only root
reinsertion/counting is likewise not accepted as membership policy and is
deleted with the duplicate native history window in MR-3.

**MR-3 implementation status: complete.** Every loaded graph Item now has one
canonical `ConversationItemModel` row. The model is no longer a second
pagination authority: provider cursors and `historyHasMore` decide whether an
older page exists, while virtualization alone bounds QWidget/document
residency, measurement, parsing, and frame work. `TurnRootItem` remains
decoration authority and cannot inject a row absent from provider child order.

This stage deletes `historyLoadedItemCount` maintenance, the adapter's suffix
budget/landmark and relation-only reinsertion paths, all authoritative/hidden
count DTO fields, model history classification/trimming, view-local history
demand and reconciliation, Shell local reveal paging, and their policy-only
tests. Missing rows or canonical neighbors now reject an exact transaction and
request the existing complete-authority projection; they are never accepted as
"off-window" no-ops. Provider request-pending state remains one typed boolean
per thread. No timer, cache, callback, compatibility path, renderer, or new
state authority was added.

Projection was reduced further while qualifying the complete model: the
adapter no longer buffers a second vector of every Turn child, no longer builds
up to 4 KiB of generic diagnostic text before replacing it for known card
types, and reads only normalized `historyHasMore` for pagination chrome. The
runtime continues to read the paired normalized `historyNextCursor` when it
dispatches `LoadHistory`.

Regression coverage now proves complete 100/160/10,001-row membership,
canonical order, strict exact-delta admission, provider-page prepend, exact
paused anchors, live-tail following, stable model/card identity, accessibility
content, stale-incarnation viewport ownership, and no model reset during
provider pagination. The Shell provider test retains all 100 initially loaded
rows, emits exactly one `LoadHistory`, adds a real older row as row 101, removes
the continuation control on the final page, and stays at no more than 48 live
cards.

Verification used the required parallelism:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14` passes for the complete
  Release tree;
- `ctest --test-dir /tmp/codexui-release-gate.I6nD6Z -LE performance -j14
  --output-on-failure` passes **26/26** in **9.91 s**;
- `ctest --test-dir /tmp/codexui-release-gate.I6nD6Z -L performance -j14
  --output-on-failure` passes all **28/28** registered quantitative profiles,
  including the 12 conversation profiles at 320/1,280/10,000 rows and DPR
  1/1.25/1.5/2, in **62.52 s**;
- the exact 10,000-row DPR-1 conversation profile passes with 31 peak resident
  cards, 29 peak documents, one normal admission pass, at most four combined
  normal passes, a 161 ms initial frame, and a 104,372 KiB sampled RSS peak;
- `git diff --check` passes, and a source/test search finds no remaining native
  reference to the deleted window/count contracts.

An additional unregistered 40,000-row stress run preserves the important
virtualization bounds (31 cards, 29 documents, at most four normal combined
passes, 289 ms initial frame) but fails the benchmark's fixed 64 MiB RSS-growth
profile at a 248,492 KiB sampled peak. Loaded value rows are intentionally
O(N); this is recorded as an MR-8 capacity/profile risk rather than restoring a
second logical window or weakening a registered gate.

MR-3 accounting is P **-961 CLOC** and T **-457 CLOC**, ending at **35,483
native production CLOC** and **42,411 native test CLOC**. This is genuine
deletion, not class extraction: the superseded state, calculations, branches,
and tests are absent from the tree.

**MR-4 implementation status: complete.** The restored invariant is that hidden
structural staging prepares only the canonical renderers required to publish one
complete visible frame; it is not another residency window. Publication
materializes visible rows only, after which the existing one-card admission
scheduler fills the ordinary overscan window. The same `ConversationCard`
objects remain the sole visible and overscan renderer throughout.

Deletion preceded extension: this stage removes the minimum-row floor, the
three-viewport hidden-stage budget, and the commit-only synchronous overscan
special case. The remaining bounded selector stages a following tail or the
saved paused-anchor frame. It uses the pending snapshot's current visibility,
folding, mode, width, and height policy and replans that same bounded work when
one of those inputs changes. Supersession and membership/mode changes destroy
obsolete hidden cards before restarting the one existing admission schedule;
unchanged and rejected commits explicitly resume deferred overscan admission.
No renderer, timer, cache, scheduler, persistent flag, or geometry authority
was added.

One Qt reentrancy defect was found while gating retained positive-offset
anchors: model insertion caused `currentChanged` to call
`scrollTo(EnsureVisible)` against half-applied geometry, evicting retained cards
before the authoritative commit. `scrollTo` now obeys the existing `applying_`
transaction boundary. Final anchor restoration and visible materialization
remain the only commit authority; retained paging consequently publishes with
zero synchronous card construction. If an inactive thread's saved semantic
anchor was actually removed, the old model cannot identify its future visible
rows. That rare case deliberately uses the canonical visible-only synchronous
fallback, bounded to one frame, then resumes one-card overscan admission. A
second pending-height index or speculative geometry cache was rejected as a
duplicate authority.

Executed regressions cover following and paused publication, positive-offset
Turn-child anchors, removed inactive anchors, current presentation-policy
changes, collapsed membership, resize and style reflow, thread switching,
supersession, unchanged/rejected snapshots, destructive callbacks, focus,
pixels, accessibility children, staging-host exclusion, and exact one-card
deferred admission. The benchmark now measures publication separately from
eventual overscan rather than conflating the two.

Verification used the required parallelism:

- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14` passes for the complete
  Release tree;
- `QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-release-gate.I6nD6Z --output-on-failure -LE performance -j14`
  passes **26/26** in **9.37 s**;
- the corresponding `-L performance -j14` run passes all **28/28** registered
  quantitative profiles in **61.17 s**, including conversation rows
  320/1,280/10,000 at DPR 1/1.25/1.5/2;
- the exact 10,000-row DPR-1 offscreen profile publishes in **83 ms** versus
  the MR-3 **161 ms** baseline, stages **16** cards versus **43**, publishes
  **10** resident/visible rows with **zero** synchronous constructions, then
  performs **9** deferred constructions in **9** one-card admission passes;
  settled peaks remain **31 cards, 29 documents, and 479 widgets**;
- the same Release binary on managed XCB (`DISPLAY=:191`, DPR 1) publishes in
  **108 ms** versus the pre-MR-4 **217 ms** baseline with the same 16/10/0/9
  stage/publication/deferred pattern. The executable still reports native
  interactive-resize-tail and RSS-profile failures; those measured platform
  gates remain explicitly open in MR-8 and are not presented as an MR-4 pass;
- `clang-format --dry-run --Werror` on the three changed implementation/test
  files and `git diff --check` both pass. A separate read-only adversarial audit
  found no remaining MR-4 architectural or correctness blocker.

Using the ledger's unchanged comparable command, `git ls-files <scope> | cloc
--list-file=-`, MR-4 accounts for P **+29 CLOC** and T **+379 CLOC**, ending at
**35,512 tracked native production CLOC** and **42,790 native test CLOC**. The
production delta misses the original -10..-30 forecast but is inside the
user-approved growth ceiling: the additional lines are the bounded pending
policy/replan correctness and the two-line transaction guard, not new state or
parallel architecture, and the audit found no safe deletion. The complete
MR-1..MR-4 cluster remains P **-974 CLOC** from its 36,486-CLOC entry. The
filesystem contains **35,874** native production CLOC when the pre-existing
untracked 362-CLOC `TurnSettingsPolicy` pair is included; it is excluded from
both sides of the tracked MR-4 delta and was not modified in this stage.

**MR-5 implementation status: complete.** The restored invariant is that
`CommandOutputView::State` remains the sole owner of inner follow/detach,
scroll value, and selection, and that the owner is settled against the card's
authoritative final width before the output is painted. `ConversationView`
continues to derive only the outer command-owned pause cause; it does not own
or copy the inner scroll position.

History and a local failing regression confirmed M-42. The former
`CommandOutputView::resizeEvent` measured output and performed immediate plus
Qt-next-phase scroll settlement. When card width ownership was consolidated in
`ConversationCard::settleHeightForWidth`, the replacement `settleWidth` kept
measurement but omitted scroll settlement. A newly created live card normally
still had its construction callback pending; a retained card built under the
hidden staging host consumed that callback before reparent/show, leaving
logical `followsLatest == true` while its physical scrollbar remained at zero.

The coherent replacement adds no mechanism: `settleWidth` now invokes the
existing immediate settlement and re-arms the existing QObject-owned,
coalesced zero-delay settlement required after Qt finalizes child layout. The
stage first deleted duplicate constructor measurement/settlement, the unused
constructor output parameter and its empty-output callback, the pre-width
environment settlement, and the identical-output settlement branch. A
semantic `setOutput` no-op is therefore a real no-op. Changed streaming output,
restored detached state, direct scrollbar interaction, and final-width layout
retain their distinct existing settlement paths. No timer, callback type,
flag, cache, state, property, counter, API surface, or parallel scroll owner was
added.

The new regression selects a staged thread containing a visible expanded
100-line command, places that command before a later admission so its original
construction callback expires while hidden, and proves publication adopts the
staged renderer with zero fallback construction. Reconciliation completion is
correctly treated as a model-authority boundary, not as a finished QWidget
layout phase. A test-only event filter samples the first actual command-output
viewport paint and requires `followsLatest`, a nonzero range, and
`value == maximum`; the same card and output remain at the tail afterward.
Existing executed tests retain exact detached values/reverse selections across
thread navigation, offscreen recycling, folding, resize/style reflow, output
removal/recreation, streaming, owner retirement, and multiple detached
commands. The capped-streaming test now reads the live
`conversationLocalGeometryPasses` counter instead of an unwritten legacy
property.

Verification used the required parallelism:

- the pre-fix focused run failed both first-tail assertions, locally
  reproducing the retained-card defect;
- `cmake --build /tmp/codexui-release-gate.I6nD6Z -j14` passes for the complete
  Release tree;
- `QT_QPA_PLATFORM=offscreen ctest --test-dir
  /tmp/codexui-release-gate.I6nD6Z --output-on-failure -LE performance -j14`
  passes **26/26** in **9.22 s**;
- the corresponding `-L performance -j14` run passes **28/28** in **62.12 s**,
  including all conversation/thread/inspector profiles at DPR 1/1.25/1.5/2;
- the exact 10,000-row DPR-1 conversation profile passes in **84 ms** initial
  publication with **16** staged, **zero** synchronous, and **9/9** deferred
  constructions/admissions. Command streaming has **zero** card construction,
  **zero** layout requests, and median/p95/maximum handler times of
  **673/735/1,052 microseconds**; peaks remain **31 cards, 29 documents, and
  479 widgets**;
- the managed-XCB virtualization run executes the new first-paint case without
  an MR-5 failure. Its full process still reports pre-existing native
  rejected-frame/residency assertions, which remain MR-8 qualification rather
  than being hidden as an MR-5 pass;
- `clang-format --dry-run --Werror` for all five touched production/test files
  and `git diff --check` pass. Two independent read-only final audits found no
  remaining MR-5 lifecycle, ownership, deletion, or correctness blocker.

MR-5 accounts for P **-6 CLOC** and T **+100 CLOC**, ending at **35,506 tracked
native production CLOC** and **42,890 native test CLOC**. The complete
MR-1..MR-5 cluster is now P **-980 CLOC** from its 36,486-CLOC entry. This is
actual mechanism deletion, not code movement: one final-width authority
replaces constructor, no-op, and pre-width settlement work.

**MR-6 implementation status: complete.** The restored invariant is that a
wheel/touchpad gesture has one semantic owner chosen from its origin and first
nonzero direction. That owner receives every later update, reversal, zero
delta, boundary arrival, pointer crossing, and `ScrollEnd`; a standalone
`NoScrollPhase` notch makes one fresh choice. Routing never changes command
follow/detach ownership and never substitutes manual scrollbar arithmetic for
the selected widget's native Qt behavior.

Three independent read-only audits reconciled the current source with
`a8c82e4`, `8ba61f7`, and committed `629660e`. The old behavior was correct but
implemented the same three-flag gesture state machine in both command-text and
command-output widgets. The current worktree correctly moved the choice into
`MiddleRegionWidget`, but its Native branch merely accepts crossed events
without delivering them to `scrollGestureArea_`; its Conversation branch does
not deliver zero updates or End. The existing test hides both failures: it
uses an orphan output directly under the outer viewport, asserts mostly that
the non-owner did not move, and its depth guard consumes the very forwarded
outer event that it purports to verify.

Deletion preceded replacement. MR-6 deleted `ContentSizedTextView`'s
directional wheel override; reduced `CommandOutputView`'s override to its sole
follow/detach responsibility; and deleted `ConversationView::forwardWheelEvent`,
its clone API, public dispatch accessor, and dispatch flag. `ConversationView`
retains only native item-view wheel dispatch plus a nonzero-vertical-intent
follow-mode update; zero lifecycle and horizontal events are semantic no-ops.
The one MiddleRegion router retains one route enum and weak native-owner
pointer, reduced to only the states needed after origin classification. One
scoped synchronous redispatch state replaces the deleted ConversationView
guard and prevents a chosen nested owner's boundary event from propagating
into the conversation. It adds no timer, cache, callback, per-card gesture
state, manual scroll calculation, or parallel router. If a weak native owner
is destroyed during a gesture, the router consumes through End instead of
silently transferring ownership.

Verification replaced rather than preserved the false-positive test. A real
expanded materialized command card demonstrates actual chosen-owner movement
for all three reported rules, command text and output, both directions/edges,
pointer crossing within and outside the center, reversal, zero updates,
missing Begin, standalone notches, pixel and angle deltas, fractional local
coordinates, exact event metadata, bounded redispatch, stable card identity,
and follow/detach commit only at a complete gesture. Composer editor and
attachment scroll ownership remain native. Failing-first runs reproduced the
crossed-owner, outside-End, semantic-no-op, and fractional-coordinate defects.

Final verification used `-j14`: the full Release build passes, nonperformance
tests pass **26/26**, and the registered performance matrix passes **29/29**.
The new serial `middle-region` gate reuses the application-layout target rather
than duplicating sources; on the final focused run direct native p95 was **6
us** and central routed p95/max was **5/8 us**, gated at **2/5 ms** and against
the paired native baseline. The exact 10,000-row DPR-1 profile passes in **86
ms**, with stage/synchronous/deferred admission **16/0/9**, peaks of **31
cards/29 documents/479 widgets**, viewport-wheel median/p95/max **209/313/422
us**, and command-stream **695/913/1,000 us**. ASan+UBSan passes both functional
and isolated routing gates. The warning build adds no warning in an MR-6 edit;
`git diff --check` and nine-file `clang-format --dry-run --Werror` pass. Managed
XCB executes the wheel case and routed metric without an MR-6 failure; its full
executable still fails the unrelated native prompt-activation harness assigned
to MR-8. Real touchpad input-to-painted-frame, Wayland/X11 breadth, and native
pixel-delta movement remain MR-8 qualifications, not hidden MR-6 claims.

The pre-edit anchor was P **35,506** / T **42,890**. MR-6 ends at P **35,484**
/ T **43,150**: P **-22**, T **+260**, and cumulative MR-1..MR-6 production
**-1,002 CLOC** from 36,486. Test growth exceeded the +80..+160 forecast because
the final adversarial audits required executable side-pane, no-op,
fractional-coordinate, and separately registered performance evidence; no
production mechanism was added to satisfy those gates.

**MR-7 implementation status: complete.** The restored invariant is that
semantic row selection, the item-view navigation cursor, child-control focus,
and visible keyboard focus remain distinct. `QItemSelectionModel` is the only
semantic/accessibility selection authority; `currentIndex()` remains the one
navigation cursor; actual QObject focus remains the child-focus authority. The
sole `ConversationCard` renderer emits focus pixels only when its owning view
is genuinely focused in Qt keyboard modality and its exact viewport geometry
matches the current row.

Deletion preceded replacement. MR-7 deleted `ConversationCard::Impl::selected`,
the public selected getter/setter, both current-or-selected mirrors, the
selection-driven relayout override, and forced focus-state painting. Child
focus no longer performs `ClearAndSelect`; it retains the historical navigation
cursor with `NoUpdate`, so Copy/Markdown/disclosure focus cannot change semantic
selection or make the row accessible/visually focused. The existing focus
callback now identifies only the actual departing/focused card and retains its
sole residency-release responsibility. No selection cache, card focus flag,
dynamic property, timer, overlay, renderer, or accessibility mirror was added.
Qt's existing window keyboard-modality attribute is set at the view's input
boundary so arrow navigation after a mouse click is visible without local
modality state.

Failing-first coverage reproduced the Breeze bottom underline and both
`ClearAndSelect` couplings. The final test sends mouse events to the actual
visible card, verifies semantic and accessible selected state, then proves
exact pixel, geometry, object, and accessible-ID stability for selection and a
repeated selection no-op. Keyboard movement alone adds focus pixels; focus loss
removes them; mouse refocus does not restore them. Real child-control focus
keeps semantic selection unchanged and remains the accessible focus child.
Eight registered gates execute this same invariant under Fusion and Breeze at
DPR **1.0, 1.25, 1.5, and 2.0**, with unsupported styles explicitly skipped
rather than silently substituted.

Final Release verification uses `-j14`: the complete non-performance suite
passes **34/34**; the registered MR-7 matrix passes **8/8**; and the final
conversation plus central-wheel performance set passes **13/13** after the
production reduction. The complete quantitative performance matrix also
passes **29/29**. The exact 10,000-row DPR-1 profile remains within the MR-6
baseline envelope: **82 ms** initial publication, stage/synchronous/deferred
admission **16/0/9**, peaks of **31 cards/29 documents/479 widgets**, viewport
wheel median/p95/max **196/286/342 us**, and command stream
**678/705/942 us**. The focused ASan+UBSan style/DPR matrix passes **8/8** with
no diagnostic. The warning build introduces no MR-7 warning;
`clang-format --dry-run --Werror` and `git diff --check` pass. Three read-only
post-edit audits found no architectural or lifetime blocker; the test audit's
card-target/accessibility gap was closed before completion. Native compositor
focus and external AT-SPI delivery remain MR-8/MR-9 qualification, not hidden
MR-7 claims.

The pre-edit anchor was P **35,484** / T **43,150**. MR-7 ends at P **35,487**
/ T **43,295**: P **+3**, T **+145**, plus **+23 CMake configuration CLOC** for
the durable eight-case matrix. The user explicitly approved up to +5 production
CLOC; the final result stays below that ceiling. Cumulative MR-1..MR-7
production is **-999 CLOC** from 36,486.

**MR-8 implementation status: complete.** The restored invariant is that each native
wheel or touchpad update reaches a painted frame within its display budget,
while visible coverage, one-renderer identity, anchors, focus, accessibility,
and the one-viewport overscan bound remain exact. Lazy residency remains
mandatory; MR-8 may reduce when work runs, but may not restore the passive
renderer, add a widget pool, skip visible coverage, or hide latency behind a
new throttle, cache, timer, or recent-input flag.

The reported live instances and the exact worktree must not be conflated.
`/proc/1976130/exe` and `/proc/1976277/exe` identify an older controller and
observer as the same deleted September 15 Debug inode, matching the pre-M-35
stash path that synchronously converged the full overscan window. However,
newer September 16 processes **2545937/2547420** have byte-identical
`scrollContentsBy`, `updateMaterialization`, overscan-selection, and admission
code to the current Debug build. The user's current-head report is therefore
verified independently of the stale pair; the stale binary is only an
amplifier and will not be used as a blanket explanation or restarted without
authorization.

The initial frame conclusion is **superseded by the completed-paint probe**.
Three isolated XCB 10,000-row/DPR-1 runs record the old `normalScroll` median
**12.502--13.017 ms** and p95 **19.555--20.820 ms**, but that metric includes
three event-loop settlement passes and is not one display frame. Exact
input-to-return-from-top-level-`UpdateRequest` measurements instead record
warm wheel p95 **2.474 ms**, warm scrollbar p95 **2.257 ms**, and
one-third-viewport scrollbar p95/max **13.834/16.042 ms**. The earlier blanket
claim that all 60 warm scrollbar samples were residency-neutral is superseded:
the recorded profile has 59 neutral samples and one bounded construction. The
final gate reports that distinction explicitly. Current-head
small/medium movement therefore meets a 16.667 ms QWidget backing-store
budget; compositor presentation is not claimed. Arbitrary large scrollbar
seeks remain reproduced at **58.089 ms p95** in the three-pass settlement
metric and synchronously construct up to 15 newly visible cards. Thus “no
incoming message” does not imply a warm path: scrolling itself changes
residency when it crosses the retained window. The two other XCB failures are
separately classified resize-tail and RSS profiles.

History and two independent read-only audits agree on the boundary and work
inventory. The uncommitted S1 sole-widget conversion correctly deleted the
parallel passive renderer, but restored rich QWidget movement/paint cost for
every visible and overscan row. M-35 removed synchronous hidden-overscan
construction from the wheel handler, while the existing one-card admission
callback still constructs, settles, lays out, and paints on the GUI thread
between inputs. A warmed scroll additionally performs an unchanged
range/anchor restoration, a full resident layout, a second full resident
release scan, repeated admission-window scans, and a full viewport update.
The router and height tree are ruled out as primary causes; prior experiments
also found no material gain from deleting viewport invalidation or substituting
Qt child scrolling.

Deletion/consolidation order before any production edit is:

1. Add a test-only native input-to-completed-paint metric that includes queued
   admission work and records constructions, layouts, paints, coverage, and
   pixel movement; retain the existing immediate-handler metric separately.
2. Delete the redundant outer admission precheck and repeated unchanged
   range/anchor restoration. A warm scroll must perform one geometry/visibility
   reconciliation; height-changing visible construction must still converge
   and restore the exact anchor once.
3. Consolidate layout and retirement traversal so hidden-stays-hidden overscan
   cards do not receive meaningless geometry writes, while cards entering or
   leaving the viewport and the one focus pin remain exact.
4. Re-measure before touching admission cadence. Only a remaining measured
   frame overrun can justify reshaping the existing callback; no second
   scheduler or input-state workaround is accepted.
5. Qualify live resize and DPR 1.0/1.25/1.5/2.0 under offscreen plus locally
   available XCB/Wayland, without weakening existing correctness or resource
   gates.

Native heap profiling adds one verified reduction to step 2. The 320-row
Breeze profile performs **12,933,418 allocations**, peaks at **83.18 MiB** of
tracked heap, and reports only **2.29 MiB** leaked; no ConversationCard or
materialization allocation is leaked. Breeze `QPropertyAnimation` accounts for
**21.37 MiB / 72,839 calls**, with **21.25 MiB / 71,648 calls** reached through
`materializeRow`. Ordinary residency currently constructs a card under the
hidden structural-staging host and immediately reparents it to the viewport;
the redundant `setParent` transition alone reaches 10,053 Breeze animation
allocations, about 992,000 widget allocations, and 229,623 stylesheet matches.
Fusion A/B reduces final RSS from **165,908 KiB** to **93,084 KiB** and normal
p95 from **17.402 ms** to **13.531 ms**, proving style transition churn rather
than an app-object leak or retained-history growth. MR-8 will delete the
ordinary staging-host parent and create normal admission cards directly under
their final viewport owner; the staging host remains only for atomic
structural snapshots. Expected production effect was **-1..-3 CLOC**, test
effect **0 CLOC** beyond the already-added native frame/resource gate. It adds
no state, cache, pool, renderer, timer, callback, or authority.

Required regressions cover progressive angle and pixel deltas, rapid and slow
gestures, row-boundary crossings, large seeks, viewport coverage at first
paint, eventual exact overscan, stable card/document/focus/accessibility
identity, Turn surfaces, scrolling and history anchors, command-output
follow/detach, thread switches, resize reflow, and semantic no-ops. The exact
pre-edit accounting anchor is native production **35,487 CLOC** and native
tests **43,295 CLOC**. Production remains neutral-to-negative; the test-growth
forecast was **+50..+140 CLOC** for the then-missing frame/platform gate.

**MR-8 final result (2026-09-17).** Lazy materialization remains the sole
residency policy and `ConversationCard` remains the sole visible/overscan
renderer. Ordinary admissions now construct directly under their final
viewport owner; the structural staging host remains only for atomic snapshot
publication. The stage also deletes the duplicate outer admission preflight,
unchanged range/anchor restoration, redundant post-restore layouts,
hidden-stays-hidden geometry writes, duplicate scrollbar-triggered layout, and
hot-path reconstruction of already-known stable keys. A materialization
restores the viewport only when authoritative height actually changes. No
timer, throttle, cache, pool, flag, compatibility renderer, or second geometry
authority was added, and no visual policy changed.

The benchmark now drains Qt `DeferredDelete` events in each manual event-loop
cycle. This corrects a benchmark-only lifetime distortion: under Breeze the
10,000-row tracked-heap peak fell from **80.15 MiB** to **22.36 MiB** and the
reported retained allocation from **4.38 MiB** to **255 KiB** without a product
cache or allocator workaround. Seven repeated Breeze 10,000-row native runs
pass the unchanged DPR-1 RSS limit; the worst observed growth is **64,184 KiB**
against **65,536 KiB**.

Exact-current Release and Debug correctness matrices pass **10/10** each, and
their registered quantitative performance matrices pass **12/12** each. The
native XCB matrix passes **8/8** at Fusion and Breeze across DPR
**1.0/1.25/1.5/2.0**. All four scroll profiles at every matrix point pass exact
first-frame pixel movement plus bounded layout, move, resize, paint,
construction, admission, and residency work. At DPR 1, Fusion records warm
wheel/direct-scrollbar/one-third-step p95 values of
**2.360/2.484/8.709 ms** and Breeze records **2.210/2.157/10.993 ms**;
large seek remains separately visible at **24.074 ms** and **29.998 ms**.

These native figures measure completion of the top-level QWidget backing-store
update, not compositor presentation. The local session was locked and both
screen-saver APIs reported active; its Wayland compositor accepted the
backing-store update but withheld its frame callback, after which Qt marked the
automation window unexposed. XCB likewise reported the covered window
unexposed. Compositor-present latency therefore remains an
environmental qualification limitation, not a hidden pass or a reproduced
ConversationView defect. The gate and thresholds were not weakened.

Final accounting is production **35,487 CLOC** and tests **43,719 CLOC**:
MR-8 is P **0** / T **+424**, disproving both the small negative production
forecast and the smaller test forecast. Cumulative MR-1..MR-8 production
remains **-999 CLOC** from the 36,486-CLOC anchor. The larger test delta is the
per-profile frame/pixel/work/resource evidence; no parity-only or duplicate
renderer test was introduced. `git diff --check`, affected Release/Debug
builds, focused correctness, and all registered performance cases pass. No
temporary compatibility path or known MR-8 workaround remains.

### MR-9 pre-edit evidence, dependency order, and acceptance gates

**Status: implementation complete; S8 platform qualification remains
(2026-09-18).** The exact entry was native production
**35,487 CLOC** and native tests **43,719 CLOC**, using the same tracked-file
`cloc` scope as MR-8. The complete Release baseline passes **63/63**, including
all **29** registered quantitative performance cases and all eight registered
conversation focus/style/DPR cases. Lazy conversation residency, the sole
`ConversationCard` renderer, the native Thread tree, and MR-7's distinction
between semantic mouse selection and genuine keyboard focus are fixed
constraints, not MR-9 rewrite targets.

Two independent read-only audits and a primary source/runtime trace refine the
remaining review findings as follows. No original finding disappears:

- **H-14 remains verified.** `CopyButton` alone reads
  `SH_Widget_Animation_Duration`; the loading spinner, conversation follow-tail,
  pending/steering card sweep, and visible Thread-row sweep ignore the same Qt
  fact. The existing static ring, pending card surface, and Thread-row surface
  already provide non-motion feedback. Consolidate the style-hint query in
  existing `UiStyle`; delete the Copy-local query plus the
  `spinnerAnimationActive` and `pendingFeedbackVisible` test mirrors. In reduced
  motion, Copy changes immediately and retains only its semantic hold timeout,
  loading reveals one static delayed ring, follow-tail reaches its destination
  synchronously, and pending surfaces remain static with zero repeating visual
  wakeups. No policy object, stored preference, callback, timer manager, or
  replacement animation is accepted. Expected P **-5..+5**, T **+60..+110**;
  any positive production result must be absorbed by same-stage deletion or
  stopped for approval.
- **M-16 remains verified but is partly superseded by H-10.** One shared native
  Copy control already exists. The remaining duplicate is clipboard publication
  in Card and Agent consumers, while `MarkdownTextView::keyPressEvent` removes
  the presentation-only U+200B marker by replacing the rich selection MIME with
  plain text. Replace that keyboard-only post-write path with Qt's supported
  `createMimeDataFromSelection()` hook, preserving every MIME format while
  cleaning textual representations. Let the shared Copy primitive publish the
  already-surface-specific text/Markdown value and own its feedback. Diff's
  labelled plain-text toolbar action and Markdown selection remain genuinely
  distinct interactions. `QClipboard::setMimeData()` has no success result;
  ownership polling/readback would not prove persistence, so the audit's
  “detect native clipboard failure” demand is classified **unsupported by the
  public Qt contract**, not implemented through a timer or false success gate.
  Expected P **-2..+6**, T **+25..+50**; H-10 already realized the former large
  Copy-control deletion forecast.
- **M-21 remains verified in narrowed form.** Its original blanket “no event
  path” claim is superseded: QWidget name/description, show/hide, enabled,
  focus, label/button text, and QTextDocument mutation already emit Qt
  accessibility events. Current source both duplicates some of those events and
  omits events for custom semantic selection/focus, busy, and expanded state.
  First record the native stream, guard no-op setters, delete the explicit
  Inspector `NameChanged` duplicate and duplicate accessible names, and rely on
  native text streaming events. Then emit only proven missing O(1) transitions
  against the existing physical card/tree/control interfaces; never create a
  shadow accessibility model or event coordinator. A checkable QToolButton is
  not an acceptable disclosure shortcut: the local Qt 6.10 probe changes its
  role from Button (43) to CheckBox (44). True Button-role
  expandable/expanded/collapsed state therefore needs one shared interface over
  the existing `DisclosureButton`. `QAccessibleAnnouncementEvent` is Qt 6.8+,
  while CodexUI supports 6.6; the compatible Copy/request announcement decision
  remains an explicit growth/compatibility gate. Expected deletion-first M-21
  P **+10..+35**, T **+100..+180**; a net-positive containing stage or new
  shared interface is approval-gated before production implementation.
- **M-13/X-03 remain verified but are partly superseded.** Current
  `ConversationView` and Inspector already invalidate geometry from Qt
  font/style/DPR events. The current tree has **34** reusable hex sites outside
  `UiStyle` plus six numeric pending-gradient color sites, rather than the
  original 68. Two global tooltip rules conflict and the last one wins; deleting
  the first preserves current pixels. Exact static local sheets can move into
  the existing application stylesheet while their widget-local ownership is
  deleted; exact missing colors become tokens without substituting near colors.
  ThreadPane styling is excluded from visual change. The global 1-to-2 px focus
  border and selected+focused Thread appearance remain characterization items,
  not permission to alter pixels. Application-font regeneration and
  constructor-only editor geometry must reuse existing Qt change events and the
  M-10 geometry owner, not add a style epoch/cache. Updated expectation P
  **-20..-60**, T **+40..+100**.
- **M-12 was verified and is complete.** `ImageThumbnail` previously decoded
  synchronously and keyed its sole `QPixmapCache` entry without DPR/physical
  target. Deleting the cache would re-decode on virtualized eviction. The sole
  renderer/cache is now DPR-correct; real 77--79 ms cold decode moved through
  the existing global Qt worker pool, while QPixmap/cache/widget/geometry work
  remains on the GUI thread. One stale-request key plus QObject lifetime checks
  replace the synchronous path; test-only dynamic mirrors are deleted.
- **M-30 is narrowed, not reopened wholesale.** Conversation mouse selection
  versus keyboard focus is complete in MR-7. Copy already distinguishes mouse
  and keyboard focus. Disclosure hover/focus and the global focus-border rule
  remain source risks until Fusion/Breeze geometry and pixel characterization
  reproduces a defect. The ThreadPane appearance may not change.

The one authoritative MR-9 implementation order is:

1. [x] Reconcile local Qt evidence and both read-only audits; classify native
   automatic events, unsupported clipboard failure detection, Qt 6.6/6.8
   compatibility, and focus/role contradictions before editing.
2. [x] **MR-9A:** add failing behavioral characterization for reduced motion
   and rich selection MIME, then consolidate H-14 and M-16 by deletion. Preserve
   normal-mode pixels/timing and every existing accessibility name.
3. [x] **MR-9B:** consolidate exact M-13 tokens/static QSS and delete the losing
   tooltip rule; regenerate the one application stylesheet from existing font
   change delivery. Do not change ThreadPane visuals or add a theme/epoch owner.
4. [x] **MR-9C:** record native accessibility events, delete duplicate/no-op
   emissions, and implement only the proven missing custom selection/focus/busy
   events with exact-once and semantic-no-op gates.
5. [x] **MR-9D:** add the one shared
   Button-role disclosure accessibility interface and a Qt-6.6-compatible
   semantic Copy/notice/request announcement path. Stop first if the containing
   production stage is net positive or requires a new compatibility mechanism.
6. [x] **MR-9E:** make the existing thumbnail cache DPR-correct, measure the
   synchronous defect, and replace it with one lifetime-safe asynchronous
   decode boundary while retaining one renderer and one cache.
7. [ ] **MR-9F platform remainder:** run native-platform/AT-SPI execution and
   the currently prohibited full non-short-circuiting suites; compile-only
   Release/Debug/Clang/sanitizer and autonomous offscreen live gates are done.
   Preserve all **29** registered quantitative gates and the Fusion/Breeze DPR
   1.0/1.25/1.5/2.0 pixel and
   geometry checks, warning/sanitizer coverage in scope, exact LOC accounting,
   and a final timer/cache/flag/callback/special-case audit before closing MR-9.
