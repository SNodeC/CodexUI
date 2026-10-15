<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-guides-compact.svg"><img src="../media/nav/banner-guides.svg" alt="CodexUI — Guides" width="100%"></picture></p>
<p><a href="README.md#banner" title="Up to Guides"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-guides-dark.svg"><img src="../media/nav/up-guides-light.svg" alt="Up to Guides" title="Up to Guides" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/account-dark.svg"><img align="right" src="../media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Thread projects and sections

Projects and sections are shared native and browser grouping features. Memberships remain server-owned; grouping changes presentation rather than creating duplicate threads.

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

In native CodexUI, the existing QTreeWidget, delegate, disclosure controls, selection and accessible tree remain the sole implementation. The delegate owns enclosing-card painting;
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

A local new-thread draft appears immediately inside its chosen project or section. Draft creation, refresh and prompt recovery use the same grouping projection as graph-owned threads; admission promotes the same row and preserves its selection.

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

The browser follows this grouping contract through its existing presentation model, session, and React thread-row renderer. Native offscreen checks and browser mock-backed checks do not establish real upstream project persistence, native-compositor behavior or screen-reader qualification. See the [qualification scope](../qualification/native-test-suite.md#banner) and historical execution evidence below.

## Implementation record

Historical verification and change accounting are kept in the [implementation record](../records/thread-projects-verification.md#banner). Follow the current user guide above for behavior; historical test commands are not an onboarding requirement.
