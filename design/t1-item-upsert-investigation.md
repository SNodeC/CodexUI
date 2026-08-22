# T1 item-upsert reconciliation investigation

Status: implementation gate triggered; no `src/` change is justified by the
current T1 brief.

Reviewed targets:

- CodexUI `3679e38` (merged PR #38)
- AISuite `61ed370`
- CodexUI history `7e19d8c`, failed attempt `c680e37`, and revert `f7b931c`

## Standing safeguards

- No `git merge`, `git pull`, `git rebase`, `git cherry-pick`. Linear history only.
- One concern per PR. If the diff touches a second concern, stop and split.
- Per-commit deletion census: count of removed lines under `src/`, and a reason for every removal longer than five lines.
- Call-site audit before implementation: how many call sites exist, how many will change, how many are deliberately left, and why.
- Tests are runtime proof. A test asserting on source text, policy, or the presence of a symbol does not count.
- Do not refresh a golden hash or protocol fingerprint without naming, in the commit message, the semantic change that moved it.
- If a task says "investigate and report before implementing", the first commit contains no `src/` changes.

## Finding in one sentence

`c680e37` failed only when a valid exact content append and an item upsert were
coalesced: the exact append succeeded and returned before topology
reconciliation, while the existing reconciliation is too entangled and not
exhaustively keyed enough to satisfy T1's performance and widget-identity
invariants by adding only a third scope set.

## 1. What `requiresFullRefresh` concretely changes

`requiresFullRefresh` is not an input to `ConversationWidget::render`. It exists
only in `WorkbenchWidget::scheduleStateRefresh` at
`src/ui/WorkbenchWidget.cpp:357-364`.

When it is true, Workbench:

1. sets `selectedPresentationFullRefreshPending`;
2. clears every accumulated exact-content update and its byte count;
3. reaches the 16 ms flush with `exactContentOnly == false`; and
4. calls `ConversationWidget::render` without exact-content hints.

When it is false:

- an affected conversation with no exact identity is promoted back to a full
  refresh at `src/ui/WorkbenchWidget.cpp:389-396`;
- exact identities are retained and, only when no Inspector, Sidebar, or
  pending turn/resume work also needs the frame, Workbench calls
  `updateExactMessageContent` directly at
  `src/ui/WorkbenchWidget.cpp:453-457`; otherwise it forwards the hints to
  `render`; and
- if Workbench reaches `render` with valid exact appends, `render` tries the
  same exact updater and returns on success at
  `src/ui/ConversationWidget.cpp:3004-3010`.

The segment topology/key reconciliation at
`src/ui/ConversationWidget.cpp:3221-3588` therefore does not run after a
successful exact append. It runs for the no-hint/full route and as a fallback
after an exact update cannot be applied. There is no separate full-refresh
branch inside `render`.

The reconciliation does not blindly recreate every widget. It preserves
compatible turns and segments, skips equal presentation keys, updates supported
widgets in place, and creates or replaces only entries it considers
incompatible (`src/ui/ConversationWidget.cpp:3290-3583`). The comment in
`c680e37` implying that `fullyAffectedThreadIds` itself invalidated all retained
widgets was therefore factually wrong.

There is a second limitation: the compatibility test is not an exhaustive
keyed diff. It accepts a removable old prefix followed by an aligned surviving
sequence. An insertion or regrouping in the middle clears that turn's item
layout at `src/ui/ConversationWidget.cpp:3411-3427`, deleting unchanged later
widgets. Reusing the existing block cannot prove the broad invariant that every
unchanged presentation key retains the same `QWidget*`.

## 2. Why the reverted attempt failed

A lone `ItemUpsertedChange` under `c680e37` still worked. Because it carried no
exact content identity, Workbench promoted it to the general reconciliation.

The failing sequence was:

1. an `ItemContentAppendedChange` and an `ItemUpsertedChange` reached the same
   scope/mailbox/16 ms presentation window;
2. `c680e37` kept the thread out of `fullyAffectedThreadIds`;
3. Workbench retained the append and classified the refresh as exact-content
   only;
4. `updateExactMessageContent` applied the append and returned `true`; and
5. `render` returned before deriving the new segment list, so the new item never
   received a widget.

The existing mixed-scope test could not reproduce this. It combines an item
upsert with `ItemContentReplacedChange`, whose append payload is absent.
`updateExactMessageContent` rejects that input at
`src/ui/ConversationWidget.cpp:3623-3624`, so the test always falls through to
the safe reconciliation path. The valid append fixture is tested separately
and never combined with an upsert.

## 3. Operations bundled with general reconciliation

Before and alongside the segment reconciliation, the current `render` path
performs:

- canonical upcoming-turn configuration synchronization;
- the viewport freeze check;
- the incomplete-history containment proof;
- generation, follow-tail, anchor, and thread-switch pin bookkeeping;
- thread title and detail rewriting;
- a forward scan over all ordered turns to find the last retained turn;
- latest-turn summary and failure recomputation;
- `latestTimelineWindow` recomputation;
- segment regeneration for the selected window;
- turn and segment compatibility checks and presentation-key computation; and
- conditional timeline layout and scroll settling.

Inspector rendering is separate, but `ItemUpsertedChange` marks the relevant
Inspector thread, so Workbench refreshes it. Workbench's selected-presentation
refresh also rebuilds breadcrumb/context text and scans the latest turn's items
for its activity count at `src/ui/WorkbenchWidget.cpp:629-666`.

## 4. Required work for a pure item upsert

A pure item upsert requires:

- the existing freeze ordering;
- selection of the current bounded window, because a tail addition can evict
  the oldest visible item or turn;
- segment regeneration for that window, because an activity bucket can retain
  its segment ID while gaining a row;
- presentation-key comparison, because an item upsert may replace an existing
  same-identity item as well as add one;
- exhaustive keyed topology reconciliation that preserves every unchanged
  widget;
- updated rendered item-range and item-identity bookkeeping;
- geometry/follow-tail settling when the visible structure changes; and
- retention and application of every coalesced exact-content append;
- the separately gated Inspector refresh, because item changes can affect its
  plan, activity, and file-change projections; and
- Workbench's latest-turn agent-activity status update, because a new
  collaboration/subagent item can change that count.

It does not require thread title/detail rewriting, the all-turn current-turn
scan, turn summary/failure reconstruction, workspace breadcrumb reconstruction,
thread-switch pinning, or unrelated attachment/settings/controller work.

The bounded window calculation itself is necessary. What T1 must avoid is a
destructive or monolithic full presentation rebuild, not the bounded calculation
needed to know which entries currently belong in a capped window.

## 5. Mandatory gate result

A structural boolean could suppress the exact-content early return and enter
the existing late reconciliation. That would make new widgets appear, but it
would not satisfy the stated invariant:

- it would execute the monolithic metadata, all-turn, summary, and general
  presentation work identified above;
- an exact message append would go through canonical message reconstruction
  rather than retaining the O(delta) direct append path; and
- a middle insertion or regrouping could still destroy later widgets whose
  presentation keys did not change.

There is no callable boundary that performs only a fully keyed timeline
reconciliation. Reconciliation and the unnecessary full-render work are
inseparable in the current structure. The brief explicitly says to stop in
this case because the task becomes a `render()` split. This investigation
therefore makes no `src/` change.

The proposed thread-ID-only structural set also cannot honor a literal ban on
window re-derivation. By the time it reaches `ConversationWidget`, it has lost
the changed turn/item identity and whether the upsert added or replaced an
item. A richer structural delta or a separately maintained window delta would
be required to patch the capped window without recalculating it.

## 6. Correction to the proposed scope taxonomy

The expected shape in the brief cannot be applied literally at AISuite
`61ed370`:

- the public `client::Change` variant has no `TurnRemovedChange` or
  `ItemRemovedChange`; it exposes only `ThreadRemovedChange` for explicit
  removals;
- `TurnUpsertedOccurrence` carries an internal `replaceItems` bit;
- legacy `turn.updated` sets `replaceItems = true` at AISuite
  `Occurrence.cpp:2318-2332`;
- the reducer then replaces the turn's ordered items and deletes omitted
  descendants at `Occurrence.cpp:2705-2724`; and
- the public client collapses this to `TurnUpsertedChange` without exposing the
  replacement bit at `Client.cpp:449-454`.

CodexUI leaves the SDK's supported legacy-v1 fallback enabled. Consequently,
`TurnUpsertedChange` must remain fully affected under the current public
contract. Moving it into an add-only structural set could reintroduce stale
widgets after a legitimate descendant deletion.

`ItemUpsertedChange` is narrower: its reducer upserts one composite item
identity and does not interpret other descendants' omission as deletion. A
future structural scope may therefore classify resolved item upserts, but not
all turn upserts. Thread-read publications map to `ThreadUpsertedChange` and
must remain fully affected.

## 7. Call-site audit for the follow-up implementation

- `ConversationWidget::render` has one production caller and 68 direct test
  calls. A new structural entry point or mode would change the one production
  caller and new targeted tests; existing tests should deliberately remain on
  the default general-render contract.
- `StateUpdateScope` has one canonical per-update producer,
  `stateUpdateScope`, plus broad manually constructed worker scopes for
  lifecycle/discovery boundaries. Only the two resolved parent branches of
  `ItemUpsertedChange` should become structural. Broad scopes deliberately
  remain broad.
- `mergeScope` is the single GUI-mailbox merge site. A future structural set
  must use the same bounded unique-union, overflow-to-`allThreadsAffected`, and
  clear-on-all discipline as the existing thread sets.
- `WorkbenchWidget::scheduleStateRefresh` is the single production consumer of
  the distinction. Full/deletion-capable input must dominate structural input;
  structural input must not clear exact-content updates.

## 8. Runtime proof required in the follow-up PR

1. Render an existing live turn, retain every original segment address, apply
   50 item upserts, and assert that every new item has a widget while every
   unchanged presentation key retains the exact original `QWidget*`.
2. Apply deletion-capable authority to the same thread and assert the removed
   widget is both absent from lookup and destroyed (`QPointer` becomes null).
3. Coalesce a valid exact append with an item upsert in both arrival orders.
   Assert the appended text, all new widgets, original widget identities, and
   unchanged `sourceMaterializationCount`/incremental append instrumentation.
4. Add an insertion/regrouping case, not only tail appends, so the keyed reuse
   invariant is actually proved.
5. Exercise mailbox merging so structural scope and exact append metadata
   survive in both orders while a later full scope still dominates.

## Baseline runtime result

Before any edit, both relevant runtime suites passed:

```text
CodexUIFrontendSessionTest ....... Passed
CodexUIConversationLayoutTest .... Passed
100% tests passed, 0 tests failed out of 2
```

Command:

```sh
cmake --build build \
  --target CodexUIFrontendSessionTest CodexUIConversationLayoutTest
ctest --test-dir build \
  --output-on-failure \
  -R 'CodexUI(FrontendSession|ConversationLayout)Test'
```

That green baseline confirms the current suite does not expose the mixed
append/upsert defect.
