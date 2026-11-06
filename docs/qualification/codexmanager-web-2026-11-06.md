# CodexManager web interface integration review — 2026-11-06

Reviewed the previously uncommitted embedded manager web interface against current `master`, `ddfba3f` (including the intervening native file-dialog, workspace-selection and thread-lifecycle changes), in `/home/voc/projects/drafts/CodexUI/codexui`. Continued `feature/manager-web-ui` from this base. The unrelated working-tree edit to `src/codex/middle/InspectorPane.cpp` was preserved and excluded from the feature. This report supplements the historical [initial qualification](codexmanager-web-2026-10-31.md).

The governing invariant remains one Qt-thread profile-store/controller/runtime authority, shared by the native and browser presentations. Reviewed request/reply ownership and shutdown across the SNode.C/Qt boundary, authenticated HTTP routes and mutation origin checks, profile validation and endpoint collisions, native dialogs during external edits, deletion ownership, persistence, and explicit CodexWUI endpoint navigation. No further production changes were required by the review. The web interface reuses current profile launch, authentication and deletion behavior; it does not proxy conversations or introduce another app-server owner.

## Validation

Release build with GCC 16.2.0, Qt 6.11.2 and the installed AISuite/SNode.C components. Rebuilt `codex-manager`, `codexmanager-test` and `codex-ui` with `-j14`. Reconfigured the established isolated `web/cmake` workspace from current sources, installed its declared dependencies through its existing target, and rebuilt TypeScript and the production Vite bundle. The bundle retains the existing large-chunk advisory. No installation was performed during this review.

Evidence is retained in `/tmp/codex/build/CodexUI/manager-web-ui`, with filenames beginning `review-` and ending in `2026-11-06.log`. This directory is a temporary build/evidence location, not an additional checkout, and may be removed after handover when no longer needed.

| Check | Result | Evidence |
| --- | --- | --- |
| Native manager regression | 57 passed; opt-in live case skipped in this entry and executed separately below | `review-qualified-tests-2026-11-06.log`, `Testing/Temporary/LastTest.log` |
| Profile deletion helper | All 12 Python cases passed | Same CTest run |
| Real Chrome manager interaction and HTTP/TLS tests | All four test groups passed: authentication/origin/static boundaries, profile creation and cross-client SSE updates, browser joining, account fixture flows, guarded deletion, HTTPS and concurrent requests/shutdown | Same CTest run; `src/manager/tests/manager-web-last-run.log` |
| Rebuilt CodexWUI selected endpoint | Browser followed the selected profile link and attempted the exact custom WebSocket path despite a stale saved endpoint | Same browser test, with `CODEXMANAGER_WUI_ASSETS` pointing at the rebuilt artifact |
| Real native launch | Passed: manager launch buttons started two disposable real backends (stdio and Unix providers), opened rebuilt CodexUI processes, verified activation sockets and reactivation, deleted one profile while the second stayed ready, then stopped remaining owned services | `review-live-tests-2026-11-06.log` |
| Native geometry and configuration controls | All seven cases passed at each of DPR 1.25, 1.5 and 2.0; DPR 1 covered by the full native regression | `review-geometry-*-2026-11-06.log` |
| Browser session and qualification regressions | All 73 cases passed | `review-browser-cases-2026-11-06.log` |
| Source formatting and syntax | Task-related clang-format/CMake checks, JavaScript syntax and `git diff --check` passed | Review commands |

Native execution used Xvfb with `QT_QPA_PLATFORM=offscreen` and `-screen 0 1280x1024x24 -extension GLX`. The first sandboxed CTest attempt could not create local network/Unix sockets and its Xvfb startup also failed; it is not accepted as qualification. The accepted run outside those socket restrictions exited zero with a working Xvfb server. Geometry and real-launch runs also exited zero. The 390 px and 1280 px Chrome screenshots were inspected and have no horizontal overflow. The narrow capture shows a transient working state; the test subsequently verifies successful joining.

Node 24's process-isolated reporter summarized the two browser test files rather than their individual cases. The explicit accepted invocation used `node --test --test-isolation=none --test-reporter=tap tests/browser-session.test.mjs tests/qualification.test.mjs` and reported all 73 cases, with no failures or skips.

## Accounting and limits

Production: 1,428 added, 169 removed, net **+1,259**. Tests: 499 added, 28 removed, net **+471**. Documentation is separate. The production addition remains within the approved 1,500-line allowance; this integration review adds documentation only. The feature replaces window-owned inventory/runtime state with the shared controller and adds the required SNode.C HTTP interface, thread communication and browser presentation.

Browser/backend/account tests use disposable helpers except for the explicitly identified real native-launch test. Browser joining verifies navigation and the WebSocket upgrade attempt against a controlled endpoint, not successful inference. Real OpenAI authentication/inference, SSH hosts, external routing/firewalls and deployed certificate trust remain unqualified. The native build includes the user's preserved inspector edit, but manager/library sources and browser artifacts match the reviewed feature; that inspector edit is not part of this submission. No personal profile configuration or backend data was changed by qualification. This review does not assert that the currently installed or running application matches the reviewed build.
