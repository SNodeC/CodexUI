# Context menu improvements qualification

Verified on 2026-11-10 in the canonical checkout `/home/voc/projects/drafts/CodexUI/codexui`, branch `feat/context-menu-creation-and-fork`, based on `f327a4b` with the task changes uncommitted. GitHub reports `master` as the default branch and no open pull requests. No additional checkout was created.

The governing invariant is that the original thread and selected turn survive menu dismissal, modal dialogs, and asynchronous replies by identity. The app-server remains the authority for fork history, new group IDs, and membership. Turn forks use the existing `thread/fork` request with inclusive `lastTurnId`; creation assigns only the ID returned by the successful create response. No original history is trimmed to produce a fork. Failed assignment retains the created group and reports the failure without automatically repeating creation.

Production changes add 344 lines and remove 242, for **102 net lines**, within the approved 250-line maximum. Tests, shared fixtures, and browser qualification tooling add 479 lines and remove 28, for **451 net test lines**. Production growth supplies the menu actions, retained targets, existing-flow completion handling, keyboard navigation reuse, and visible separator styling. Existing group form validation and result-identity extraction were reshaped rather than duplicated.

| Check | Final result | Scope |
| --- | --- | --- |
| Native Release build | Passed | GNU C++ 16.2.0, Qt Widgets 6.11.2; shell integration, client runtime dispatch, and thread pane UI targets |
| Native CTest | 6/6 passed | Shell integration, runtime protocol dispatch, and thread pane UI at DPR 1, 1.25, 1.5, and 2; `ctest -j 14` |
| Additional scaled shell integration | 3/3 passed | DPR 1.25, 1.5, and 2; same final native build |
| Browser TypeScript compilation | Passed | Node 24.21.0 and repository TypeScript configuration |
| Browser session tests | 62/62 passed | Existing session consumers and mutation behavior |
| Shared fork membership fixtures | 13/13 passed | Includes quick and options forks at the middle turn, inherited membership, and source preservation |
| Turn history tests | 19/19 passed | Includes rejected and stale bounded forks, source identity and history preservation, and existing removal behavior |
| Shared creation fixtures | 8/8 passed in browser and exercised by native runtime test | Project/section success, creation failure, assignment failure, and target retirement |
| Focused real-browser UI | Passed | Chrome 156.0.8078.79; 12 checks for ordering, first visible creation entry, visible divider, keyboard navigation, both fork modes, options title, cancellation, source preservation, exact assignment target, and failure feedback |
| Diff whitespace and relevant C++ formatting | Passed | Repository formatting baseline; only task-relevant C++ ranges formatted |

Native checks ran under Xvfb with `QT_QPA_PLATFORM=offscreen`. Final native runs used task-local configuration/cache directories. Scaled runs use separate directories per DPR and `QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough`. The menu scenarios include ordinary and enlarged fonts and normal/narrow shell sizes. Initial parallel scaled runs using shared configuration failed status/project-visibility checks; the same build passed after configuration/cache isolation and use of the native build working directory. No conclusion about the individual cause of those initial failures is claimed.

The native bridge and browser WebSocket are simulated backends. The browser UI runs the real built frontend in headless Chrome. These results verify frontend dispatch, target retention, interaction, presentation, and processing of simulated responses; they do not claim a live app-server, CI, installation, deployment, or full release qualification. No performance improvement is claimed.

Builds and evidence are retained in `/tmp/codex/build/CodexUI/context-menus` for review. This directory contains the native build, browser compilation and bundle, test logs, isolated test configuration, and menu screenshots. Remove it after reviewing or archiving the evidence; it contains no unique source changes. Temporary editing scripts outside that directory were removed.

Key evidence files are `native-tests.log`, `shell-dprs.log`, `shell-dpr-*.log`, `browser-ui.log`, `browser-session.log`, `fork-membership.log`, `turn-removal.log`, `context-menu-creation.log`, and `evidence/`. Native and browser turn-menu screenshots were visually inspected: both show the two fork actions above a visible divider and the removal action.

The changes remain in the canonical checkout on the task branch. No new commit, push, pull request, installation, or publication was performed.
