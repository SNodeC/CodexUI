<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p>
  <a href="../README.md#project-overview" title="Codex(W)UI repository"><img src="media/page-banner.svg" alt="Codex(W)UI repository" width="100%"></a>
</p>
<!-- snodec:end page-header -->

# Native thread projects and sections

Projects and sections are native grouping features. Memberships remain server-owned; grouping changes presentation rather than creating duplicate threads.

## Authoritative visual decision

- One enclosing card per project.
- Projects precede standalone sections and ungrouped threads, including drafts. Titles are painted as whitespace-normalized single lines; tooltips and accessible names retain the full authoritative title.
- Within it, sections are headings and threads are plain rows, not nested cards.
- A section without a project has its own enclosing card.
- Newly created empty sections are discoverable as standalone cards; adding or
  removing their last unprojected member preserves the card's identity.
- A thread without either membership has its own individual card.
- There is no enclosing synthetic "No project" card.
- Projects and sections remain independent server memberships. Repeating a
  section heading in different projects does not duplicate any thread.
- Same-membership agent/fork children retain their hierarchy. Cross-membership
  children appear in their own group and offer Go to parent.

The existing QTreeWidget, delegate, disclosure controls, selection and accessible
tree remain the sole implementation. The delegate owns enclosing-card painting;
the superseded per-row stylesheet frames are removed. No index widgets,
container widgets, second renderer, scrolling offsets or layout timers were added.

## Available controls

The grouping menu switches Projects / Sections / Ungrouped, exposes creation,
catalog refresh and additional catalog pages. Context menus provide project and
section details/edit/delete, assignment/removal, project ordering and thread
ordering within a shared section. Drag/drop is intentionally not part of this
menus-first stage.

Title search is submitted with Enter; clearing it removes the filter. Active,
Archived and All are supported. All uses two independently paged server queries:
omitting `archived` does **not** mean all on the app-server. For Section order in
All, active rows precede archived rows, each in server order; the API does not
expose a cross-filter position suitable for merging their order. The sort action
documents this distinction.

Project roots and descriptions are editable. Descriptions use the explicit
`codexui.description` metadata key and are not presented as model instructions.
Other clients' metadata is read freshly before replacement and preserved.
This is not a compare-and-swap transaction: the protocol offers no revision
precondition against a concurrent writer between that read and update.

New-thread group context carries the project into thread/start and assigns the
section after creation. A failed section assignment retains the created thread,
reports the failure, and can be retried through Assign section without resending
the first prompt. Explicit prompt recovery retains its original group context.

Observers can browse and inspect; mutations are rejected at the worker boundary,
including a controller-role change during the metadata read/update sequence.
Unsupported catalog methods fall back to the existing thread hierarchy and are
not retried repeatedly within that connection.

## State and lifetime

- NodeGraph remains the membership/entity authority; Qt retains projections and
  view preferences only.
- One worker-owned query pager replaces the former single global thread cursor.
  Queries are keyed by method and normalized filters, not by their cursor.
- Group/catalog pages are limited to 100 entries and two concurrent reads.
  Expansion never hydrates conversation history.
- Filter changes invalidate old query identities. Late replies cannot populate
  a removed/recreated query or undo newer membership/archive notifications.
- Loading, errors, retry and end-of-pagination come from graph Catalog records.
- The existing initial rollout-repair timer is retained once for the general
  thread list, not multiplied per group. No polling timer was added.
- Catalog refresh occurs on reconnect, local mutation, project notifications,
  panel visibility and explicit refresh. There is no section-change notification
  in these bindings, so another client's section edits become visible on these
  refresh boundaries rather than through invented notifications.
- Expansion persistence applies to project/section headers, never leaf threads.
  Scroll-driven page admission inspects visible rows, not the complete tree.

## Qualification boundary

These features are native-only. Offscreen/mock-backed checks do not establish real upstream project persistence, native-compositor behavior or screen-reader qualification. See the [qualification scope](native-ui-ux-qualification-inventory.md#page-overview) and historical execution evidence below.

## Implementation record

Historical verification and change accounting are kept in the [implementation record](records/thread-projects-verification.md#page-overview). Follow the current user guide above for behavior; historical test commands are not an onboarding requirement.
