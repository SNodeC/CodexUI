# CodexManager embedded web interface qualification — 2026-10-31

Scope: the Qt manager's shared profile authority, embedded SNode.C Express interface, profile browser listeners and explicit CodexWUI navigation. Conversation rendering is outside this change. The invariant is one Qt-thread inventory/store/runtime authority, with HTTP connections and delivery owned exclusively by the SNode.C event-loop thread.

Revision: uncommitted working tree on `feature/manager-web-ui`, based on `3cb9122ac4af19f8d00776e726ee7d99bbee0e52`, in the canonical nested checkout. No additional checkout was created. No commit, push, PR, merge or live installation was performed.

Configuration: Release, GCC 16.2.0, Qt 6.11.2, installed AISuite/SNode.C development components, Node 24 and installed Chrome. Build, npm cache, generated browser workspace, staged installation and evidence are under `/tmp/codex/build/CodexUI/manager-web-ui`. Backend processes and account responses in the default manager tests are simulated through disposable helper executables; HTTP/HTTPS listeners and Chrome interactions are real. The additional opt-in native launch qualification uses installed codex-bridge, codex-bridge-client and Codex app-server executables, the rebuilt CodexUI, and disposable user-systemd services.

## Execution environment

The initial default Xvfb invocation crashed in the system NVIDIA EGL/GBM driver. This system's `xvfb-run` script nevertheless executed its offscreen child after exhausting startup attempts and then returned failure. Those invocations are not accepted as Xvfb qualification, even when their child printed passing tests.

Accepted native/browser runs use `xvfb-run -a -s '-screen 0 1280x1024x24 -extension GLX'` with `QT_QPA_PLATFORM=offscreen`. Xvfb startup and wrapper exit status were checked separately; startup then succeeded. Disabling unused virtual-server GLX avoids the driver crash and does not replace the required offscreen Qt raster platform. Concurrent automatic display selection retried occupied display numbers successfully. XKB warnings were nonfatal.

## Results

| Check | Result | Evidence |
| --- | --- | --- |
| Native manager regression CTest entry | Passed: 56 Qt cases; one explicitly opt-in live-services case skipped | `manager-tests-qualified.log`, `Testing/Temporary/LastTest.log` |
| Actual Qt manager buttons start two real backends and open native CodexUI; reactivation, deletion and profile isolation | Passed: opt-in live test plus lifecycle regression, four Qt cases | `live-window-accepted.log` |
| Guarded deletion Python entry | Passed: 12 cases | `Testing/Temporary/LastTest.log` |
| Installed bridge accepts Unix native access plus each additional WS/WSS IPv4/IPv6 listener and a custom path | Passed, configuration parsing only | `installed-browser-qualified.log` |
| Native geometry/details/editor capability checks at DPR 1.25, 1.5 and 2.0 | Passed: seven Qt cases per run; DPR 1 covered by the complete native entry | `geometry-*-qualified.log` |
| Staged manager HTTP authentication, origin protection and asset containment | Passed | `staged-web-qualified-final.log` |
| Real Chrome profile creation, cross-client SSE updates, backend startup, sign-in/cancellation/sign-out and guarded deletion | Passed | `staged-web-qualified-final.log` |
| Actual rebuilt CodexWUI opens the selected custom WebSocket path despite an injected stale saved endpoint | Passed; the controlled endpoint records the upgrade attempt and returns a fixture error rather than providing inference | `staged-web-qualified-final.log` |
| Manager HTTPS listener | Passed with a disposable self-signed certificate and authenticated API request | `staged-web-qualified-final.log` |
| Concurrent HTTP request delivery and clean process shutdown | Passed | `staged-web-qualified-final.log` |
| CodexWUI session and qualification tests | Passed: 73 cases | `browser-session-tests.log` |
| Production browser artifact and staged native/web installation | Passed; existing Vite chunk-size advisory remains | `browser-artifact.log`, `stage-install.log`, `browser-package-install.log` |
| Formatting, JavaScript syntax and whitespace checks | Passed | Task-relevant clang-format/CMake checks, `node --check`, `git diff --check` |

The desktop (1280 px) and narrow (390 px) browser overview screenshots were inspected; profile text and actions remain visible without horizontal overflow. Screenshots are under `stage-tests/manager-web-evidence/`. The shared-authority Qt regression also verifies that an externally renamed profile updates the currently displayed native details.

## Accounting and limits

Production: 1,428 lines added, 169 removed, net **+1,259**. This includes CMake, embedded CSS generation, HTML/CSS/JavaScript assets and CodexWUI navigation changes. Tests: 499 added, 28 removed, net **+471**. This follow-up strengthens the existing opt-in qualification at the actual manager button/controller boundary; production code is unchanged. Documentation is counted separately. Both gross production additions and net growth remain within the approved 1,500-line allowance.

The new production code provides the HTTP interface, browser presentation and thread boundary, which cannot be obtained by deletion alone. Window-owned registry/runtime state and duplicated native save/import mutation logic were removed; both interfaces now use the shared controller and existing runtime/launch/deletion implementations. Browser transport choices also come from the same native definitions.

The opt-in test exercised both stdio and Unix app-server transports through actual visible Qt launch buttons under Xvfb/offscreen. Fresh signed-out profiles preserve the existing two-click flow: Start & open establishes the backend, then Open CodexUI permits unauthenticated browsing. Both native activation sockets accepted connections; reactivation succeeded. Deleting one profile through the shared controller removed its data and registration while the second stayed ready, then the second stopped cleanly. During test development, one attempt omitted the existing second click and timed out; another retained a reference after the controller deleted its runtime and crashed in test cleanup. The harness now uses the controller deletion boundary and cleans only surviving runtimes; the final run exited zero. Disposable services left by that failed harness were explicitly stopped.

The desktop launcher resolves to `/usr/local/bin/codex-manager`, which still contains the older installed version. The verified new binary is `/tmp/codex/build/CodexUI/manager-web-ui/codex-manager`; no live installation has occurred.

Not qualified: real OpenAI authentication/inference, real SSH hosts, remote routing/firewalls or a deployed certificate chain. No personal profiles, credentials or services were changed. The temporary build and staged artifacts are retained for review and can be removed after review or an authorized installation; failed disposable fixture directories were cleaned up.
