# CodexUI static design mockups

This visual appendix is part of the [multiple server design proposal](../multiple-servers-proposal.md). The 66 sheets are fixed design images, not interactive prototypes. They show proposed placement and appearance using representative versions of the current widget anatomy; existing widget wiring remains unchanged until an implementation is approved. All displayed conversations, endpoints, identities, permission grants, request facts and times are sample data.

The [static design atlas](index.html) contains the same sheets as editable, script-free presentation markup. Animation is represented by frozen keyframes rather than moving previews. Sheet 30’s upstream acceptance/inference labels remain conditional on explicit correlated provider signals.

Start with the **[recommended Session Studio redesign](#recommended-session-studio-redesign)**. Sheets 54–66 define the expanded application composition; earlier sheets retain detailed widget, dialog and state coverage. Studio outer navigation supersedes the earlier shell studies where they differ.

## Coverage index

| UI aspect | Sheets |
| --- | --- |
| Recommended Studio Focus/Split/Grid, management, review, login/access, responsive and dark compositions | [54–66](#recommended-session-studio-redesign) |
| Earlier desktop tabs, two/four simultaneous sessions and failure isolation | [01](01-desktop-tabs.png), [02](02-follow-two.png), [03](03-follow-four.png) |
| Server list, empty application, details, add/edit/remove, validation | [04](04-servers.png), [05](05-empty-and-details.png), [06](06-add-server.png), [07](07-edit-remove.png) |
| Unix/TCP/WS connection settings, optional native transports and connection menu | [08](08-connection-settings.png) |
| Controller/observer, provider availability and transport states | [09](09-roles-and-provider.png) |
| Hidden/collapsed widget restoration and filtered content | [10](10-visibility.png) |
| Projects/Sections/Ungrouped, search, sorting, pagination, all context actions | [11](11-thread-browser.png), [12](12-thread-menus.png) |
| Project/section forms, details, ordering, membership, deletion | [13](13-group-dialogs.png), [14](14-group-order-delete.png) |
| New thread, fork, rename/delete, hydration and prompt recovery | [15](15-new-fork.png), [16](16-thread-lifecycle.png) |
| Every conversation card family, turn anatomy, Markdown, Copy and timing | [17](17-all-card-families.png), [18](18-card-anatomy.png) |
| Command output, changed files, image ribbons, native viewer/browser metadata | [19](19-command-images.png) |
| Composer, all turn settings, More popup, attachments, Send/Steer/Stop | [20](20-composer.png) |
| Native workspace/file pickers, paste/drop, preparing/error states | [21](21-pickers.png) |
| Inspector Plan/Agents/Changes and modeless diff review | [22](22-inspector-plan-agents.png), [23](23-changes-review.png) |
| Requests, composer attention, hidden-session counts and resolved states | [24](24-request-surfaces.png) |
| Info State/Protocol/Timing and exact timestamp details | [25](25-inspector-info.png) |
| Command/file/permission approval, questions, MCP, legacy and unsupported capabilities | [26](26-approval-dialogs.png), [27](27-input-mcp.png), [28](28-legacy-capability.png), [37](37-request-unavailable.png) |
| Brand, status/footer, notices, menus/tooltips, tokens and presentation preferences | [29](29-chrome-and-controls.png) |
| New/steering acceptance and inference keyframes, priority and reduced motion | [30](30-motion-storyboard.png) |
| Perceptual palette, border presence/width/color, corners and control states | [31](31-appearance-tokens.png), [32](32-dark-split.png) |
| Narrow layouts, restored pane, empty/loading/failure and live-session isolation | [33](33-responsive.png), [34](34-empty-loading-errors.png) |
| Browser scope, native-only boundaries, long/Unicode values and missing timing | [35](35-browser-scope.png), [36](36-long-content.png) |
| Login, native/browser handoff, IdP credential/MFA/registration/recovery and account menu | [38](38-sign-in.png), [39](39-idp-account-flows.png), [40](40-account-menu.png) |
| Users & access directory, effective user grants, role templates and every-section matrix | [41](41-users-directory.png), [42](42-user-details.png), [43](43-role-templates.png), [44](44-all-section-access.png) |
| Per-server and project/shared-section/thread permission constraints | [45](45-server-access.png), [46](46-project-section-access.png) |
| Denied/read-only views, human/runtime roles, revocation, signout and login failures | [47](47-access-denied.png), [48](48-identity-runtime-roles.png), [49](49-access-lifecycle.png), [50](50-login-failures.png) |
| CodexUI/IdP policy ownership, narrow account presentation and administration safeguards | [51](51-policy-owner.png), [52](52-access-responsive.png), [53](53-delegation-guard.png) |

## Recommended Session Studio redesign

### 54 Session Studio and a redesigned application hierarchy

![Session Studio and a redesigned application hierarchy](54-studio-focus.png)

### 55 Split workspaces with quieter session chrome

![Split workspaces with quieter session chrome](55-studio-split.png)

### 56 Four-session canvas with independent activity and failure

![Four-session canvas with independent activity and failure](56-studio-grid.png)

### 57 A dedicated server library with descriptive profiles

![A dedicated server library with descriptive profiles](57-studio-server-library.png)

### 58 Server details with scoped configuration and access

![Server details with scoped configuration and access](58-studio-server-details.png)

### 59 Project context changes the workspace frame not thread wiring

![Project context changes the workspace frame not thread wiring](59-studio-project-navigation.png)

### 60 A larger owned Inspector for review without changing the destination

![A larger owned Inspector for review without changing the destination](60-studio-review.png)

### 61 Reading mode clears the surrounding layout and preserves every restore path

![Reading mode clears the surrounding layout and preserves every restore path](61-studio-reading.png)

### 62 Users and access becomes a complete administration workspace

![Users and access becomes a complete administration workspace](62-studio-access-center.png)

### 63 A clear identity gateway before any private session content

![A clear identity gateway before any private session content](63-studio-sign-in.png)

### 64 The Studio hierarchy adapts to narrow screens with complete restoration

![The Studio hierarchy adapts to narrow screens with complete restoration](64-studio-responsive.png)

### 65 Session Studio in dark appearance

![Session Studio in dark appearance](65-studio-dark.png)

### 66 The redesigned journeys and a single visible restoration entry point

![The redesigned journeys and a single visible restoration entry point](66-studio-navigation.png)

## Earlier application layout studies and session details

### 01 Desktop workspace and server tabs

![Desktop workspace and server tabs](01-desktop-tabs.png)

### 02 Follow two sessions simultaneously

![Follow two sessions simultaneously](02-follow-two.png)

### 03 Follow four sessions and isolate a failed server

![Follow four sessions and isolate a failed server](03-follow-four.png)

### 04 Server management and all connection states

![Server management and all connection states](04-servers.png)

### 05 Empty application and full server details

![Empty application and full server details](05-empty-and-details.png)

### 06 Add a server with Unix, TCP or WebSocket

![Add a server with Unix, TCP or WebSocket](06-add-server.png)

### 07 Edit metadata, validate input and remove a server

![Edit metadata, validate input and remove a server](07-edit-remove.png)

### 08 Existing connection dialog and session-scoped menu

![Existing connection dialog and session-scoped menu](08-connection-settings.png)

### 09 Transport, provider and ownership stay distinct

![Transport, provider and ownership stay distinct](09-roles-and-provider.png)

### 10 Every hidden or collapsed region can be restored

![Every hidden or collapsed region can be restored](10-visibility.png)


## Threads projects and sections

### 11 Projects, sections, hierarchy and ungrouped threads

![Projects, sections, hierarchy and ungrouped threads](11-thread-browser.png)

### 12 Search, sorting, context actions and result states

![Search, sorting, context actions and result states](12-thread-menus.png)

### 13 Project and section create, edit and details

![Project and section create, edit and details](13-group-dialogs.png)

### 14 Group membership, ordering and deletion

![Group membership, ordering and deletion](14-group-order-delete.png)

### 15 New thread and fork with options

![New thread and fork with options](15-new-fork.png)

### 16 Thread rename, delete, hydration and prompt recovery

![Thread rename, delete, hydration and prompt recovery](16-thread-lifecycle.png)


## Conversation composer and attachments

### 17 All conversation card families

![All conversation card families](17-all-card-families.png)

### 18 Turn anatomy, disclosure, Copy, timing and Markdown

![Turn anatomy, disclosure, Copy, timing and Markdown](18-card-anatomy.png)

### 19 Command output, file cards and image surfaces

![Command output, file cards and image surfaces](19-command-images.png)

### 20 Composer, attachments and every upcoming-turn setting

![Composer, attachments and every upcoming-turn setting](20-composer.png)

### 21 Native workspace and attachment pickers

![Native workspace and attachment pickers](21-pickers.png)


## Inspector and request dialogs

### 22 Inspector Plan and Agents

![Inspector Plan and Agents](22-inspector-plan-agents.png)

### 23 Inspector Changes and modeless diff review

![Inspector Changes and modeless diff review](23-changes-review.png)

### 24 Pending requests, attention and resolution states

![Pending requests, attention and resolution states](24-request-surfaces.png)

### 25 Inspector Info State Protocol and Timing

![Inspector Info State Protocol and Timing](25-inspector-info.png)

### 26 Command file-change and permission approvals

![Command file-change and permission approvals](26-approval-dialogs.png)

### 27 Question inputs and MCP elicitation

![Question inputs and MCP elicitation](27-input-mcp.png)

### 28 Legacy approvals and unsupported capability requests

![Legacy approvals and unsupported capability requests](28-legacy-capability.png)

### 29 Application chrome menus tooltips notices and token details

![Application chrome menus tooltips notices and token details](29-chrome-and-controls.png)


## Motion appearance and responsive states

### 30 Accepted and inference phases for new and steering You cards

![Accepted and inference phases for new and steering You cards](30-motion-storyboard.png)

### 31 Perceptual colors borders corners and control states

![Perceptual colors borders corners and control states](31-appearance-tokens.png)

### 32 Dark appearance and simultaneous following

![Dark appearance and simultaneous following](32-dark-split.png)

### 33 Tablet narrow workspace and restored pane

![Tablet narrow workspace and restored pane](33-responsive.png)

### 34 Workspace empty loading failure and isolated runtime stop

![Workspace empty loading failure and isolated runtime stop](34-empty-loading-errors.png)

### 35 Browser frontend preserves its supported controls

![Browser frontend preserves its supported controls](35-browser-scope.png)

### 36 Long titles paths Unicode and complete timing recovery

![Long titles paths Unicode and complete timing recovery](36-long-content.png)

### 37 Unavailable malformed and unsupported request states

![Unavailable malformed and unsupported request states](37-request-unavailable.png)

## Login account and user access management

Identity and credential/account self-service belong to snode.c-idp. Application grants are shown as CodexUI-owned for the inspected IdP revision, which does not contain the required administrator resource-grant model. These screens are static proposals; no login or user-management operation is performed.

### 38 Sign in through snode.c-idp for native and browser clients

![Sign in through snode.c-idp for native and browser clients](38-sign-in.png)

### 39 IdP-hosted credentials MFA registration and recovery

![IdP-hosted credentials MFA registration and recovery](39-idp-account-flows.png)

### 40 Human account menu My access and account switching

![Human account menu My access and account switching](40-account-menu.png)

### 41 Users and access administration for verified identities

![Users and access administration for verified identities](41-users-directory.png)

### 42 User details and effective resource access

![User details and effective resource access](42-user-details.png)

### 43 One permission model with small role templates

![One permission model with small role templates](43-role-templates.png)

### 44 Access checks cover every application section

![Access checks cover every application section](44-all-section-access.png)

### 45 Per-server access management and permission change review

![Per-server access management and permission change review](45-server-access.png)

### 46 Projects shared sections and thread restrictions

![Projects shared sections and thread restrictions](46-project-section-access.png)

### 47 Restorable visibility is distinct from denied access

![Restorable visibility is distinct from denied access](47-access-denied.png)

### 48 Human permissions controller lease and model policy

![Human permissions controller lease and model policy](48-identity-runtime-roles.png)

### 49 Permission revocation signout and independent sessions

![Permission revocation signout and independent sessions](49-access-lifecycle.png)

### 50 IdP unavailable cancelled expired and failed login

![IdP unavailable cancelled expired and failed login](50-login-failures.png)

### 51 Current CodexUI policy owner and future IdP capability

![Current CodexUI policy owner and future IdP capability](51-policy-owner.png)

### 52 Account and access states at narrow widths

![Account and access states at narrow widths](52-access-responsive.png)

### 53 Delegated administration and access change safeguards

![Delegated administration and access change safeguards](53-delegation-guard.png)

## Reading and verification

The sheets deliberately show presentation, not live data or executable workflows. Text that resembles a button is inert. The source atlas contains no scripts, event handlers, animation rules, editable inputs or operational controls. All 66 images were rendered at 1440 CSS pixels wide and checked for content escaping the sheet boundary. Native widget interaction, live IdP login, trusted authorization enforcement, real upstream phase correlation, SNode.C isolation and production latency remain governed by the proposal’s approval and qualification requirements.
