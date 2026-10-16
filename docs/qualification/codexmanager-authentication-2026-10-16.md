# CodexManager authentication qualification — 2026-10-16

## Revision and scope

Canonical checkout: `/home/voc/projects/drafts/CodexUI/codexui`. Branch: `feature/codexmanager`. Initial manager checkpoint: signed commit `7c23552` (`feat(manager): manage independent local and remote Codex profiles`). This authentication follow-up is uncommitted. No push, PR, merge or system installation was performed. GitHub reported no existing PR for this branch.

The SHA-256 of `git diff -- src/manager tests/manager` for the verified implementation is `509dd31d57a7ce676f53197ec23826fa4cb84fa4bb68bda0e55803132127365a`. Documentation and screenshots are excluded from that implementation fingerprint.

## Authority and lifetime

The profile's app-server remains the sole authority for account state, credential storage and OAuth refresh. `ProfileRuntime` uses its existing bridge-client process for correlated account requests and notifications. It owns only the active login interaction, account projection and bounded request deadlines. No second connection, OAuth implementation, app-server instance, credential file or persisted authentication setting was added. The existing five-second supervision timer checks deadlines; no additional repeating timer was added.

The connectivity-only Ready label was replaced by Backend connected plus an independent account status. Initial signed-out startup leaves the user at authentication setup; explicitly opening CodexUI for browsing remains possible. Authentication mutations claim free bridge control and release it on completion. An existing controller must explicitly release control or close; the manager never forcibly takes it. Known unauthorized inference notifications change the account presentation to Authentication rejected.

Supported interactive methods are device code, app-server browser callback and API key. Account refresh, login cancellation, sign-out confirmation and host-specific CLI alternatives are present. Browser callbacks belong to the app-server host; device code supports remote login without a callback tunnel. A masked API-key entry is cleared after submission. Login URL/code/key content is excluded from manager logs and profile persistence. RPC failures are displayed without logging their raw payloads. No personal credentials were used during qualification.

Request IDs are never reused during a runtime's lifetime. Login completions are matched by login ID, including notification-before-response ordering. Account-update notifications invalidate older pending reads. Connection loss clears account certainty and ephemeral login details; unconfirmed operations are reported as unknown. Read timeouts do not cancel an independently pending login. A login waits up to ten minutes, then requests cancellation when an ID is known. The manager does not automatically replay login mutations after disconnect.

## Executed checks

Debug/Ninja build in `/tmp/codex/build/CodexUI/manager`, with `CODEXUI_INSTALL_WEB=OFF`, Qt 6.11.2 and GCC 16.2.0. Built `codex-manager`, `codexmanager-test` and `codex-ui` with `--parallel 14`; warning-as-error compilation passed. Compiler temporary files and caches stayed below that build directory. Task-relevant clang-format checks and `git diff --check` passed.

The final `codexmanager` CTest invocation used Xvfb, `QT_QPA_PLATFORM=offscreen`, scale 1.0, `CODEXMANAGER_LIVE_QUALIFICATION=1`, and `--parallel 14`. It passed in 47.34 seconds: **32 QtTest results passed, zero failed, zero skipped**. Coverage includes all prior manager checks plus browser/device/API-key success, sign-out, profile isolation, cancellation before and after the login response, rejection, expired login, early completion, controller contention, disconnect, real account-read timeout without cancelling a login, account rejection and actual dialog input/geometry. The 31-second timeout case deliberately exercises the public timeout behavior with the Qt event loop running.

Two disposable real local profiles exercised the installed bridge/client/app-server over stdio and Unix providers. Both reported signed-out account state through the real protocol. Neither automatically opened CodexUI before authentication. Explicit browsing opened both independent windows, and stopping one preserved the other. The test stopped its services and removed its own socket/lock artifacts.

At DPR 1.25, 1.5 and 2.0, authentication-dialog and manager-layout checks each passed **7 results, zero failed/skipped**. Dialogs were exercised at narrow and normal widths, and overview/details at narrow, normal and wide widths. Widget bounds and one-line input size hints were checked. DPR 1.0 and 2.0 dialog screenshots were visually inspected. Captures use simulated profiles and login data. Offscreen activation does not establish real compositor focus behavior.

Evidence: `auth-build.log`, `auth-final-tests.log`, `Testing/Temporary/LastTest.log`, `auth-dpr-{1.25,1.5,2}.log`, and `evidence/auth-*` under the build directory. Browser/device/API-key completion was exercised with protocol fixtures; no real OpenAI login, account sign-out, inference or quota-consuming request was performed. Live remote SSH, remote browser callbacks, TLS and Bluetooth remain environment-specific qualification, not claims of this test run. A successful account read indicates configured credentials, not verified quota or inference access. No performance-sensitive transport or conversation rendering changes were made, and no throughput improvement is claimed.

## Accounting and cleanup

Authentication production changes: **+496 / -17 lines, net +479**. Test changes: **+291 / -2 lines, net +289**. Documentation and the captured image are accounted separately. This approved feature adds the previously absent login interaction and account protocol projection; reduction alone cannot provide them. The implementation reuses the existing transport, runtime, timer and controller policy, and replaces the ambiguous connectivity-only readiness presentation.

No additional checkout or worktree was created. Keep `/tmp/codex/build/CodexUI/manager` for review and launching the rebuilt manager; it can be removed after review or a subsequent authorized installation. Temporary editing scripts were removed. Existing user profiles and running applications were not changed or restarted.
