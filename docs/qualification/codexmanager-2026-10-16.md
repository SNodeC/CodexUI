# CodexManager implementation and qualification

Date: 2026-10-16. Checkout: `/home/voc/projects/drafts/CodexUI/codexui`. Remote: `git@github.com:SNodeC/CodexUI.git`. Branch: `feature/codexmanager`, based on `master` commit `5f6517d`. The implementation is an uncommitted working-tree change; no PR, push, merge or system installation was performed.

## Architecture and scope

The governing invariant is one saved profile definition, one explicit owner per component, and independent app-server homes/endpoints/lifetimes across profiles. The existing applications had no profile lifecycle coordinator. CodexManager adds that owner as a separate Qt executable and reuses the existing bridge, app-server, diagnostic client and UI foundations. It does not add a conversation model or another transport implementation. Every CodexUI remains a separate process with its existing session/runtime threads.

`Profile` and `LaunchPlan` are the authorities for configuration and generated commands. The same commands drive startup and the manual instructions. `ProfileRuntime` owns asynchronous service operations and bounded readiness/log buffers. Typed component states drive display; UI widgets do not maintain a second process-state model. Systemd user units provide durable ownership locally and over explicitly configured SSH. Stdio app-server ownership remains with the bridge; Unix providers have an explicit manager/service owner. Network providers follow the agreed external-or-SSH ownership policy.

The native integration supplies window identity, accent, separate settings and an activation socket. Manager exit destroys helper processes, leaving owned backend/UI services intact. Closing editors or the manager disconnects helper callbacks before captured widgets are destroyed. No existing conversation presentation code was changed.

The three runtime timers bound management commands (20 seconds), readiness (35 seconds), and status polling (5 seconds). The editor bounds executable-capability discovery at 10 seconds. Zero-delay callbacks defer initial widget refresh and queued process dispatch to the event loop. All service/SSH work is asynchronous per profile. There is no prior manager implementation to use as a performance baseline; this qualification makes no inference-throughput or desktop-compositor latency claim.

## Build and checks

Build directory: `/tmp/codex/build/CodexUI/manager`. Debug build, Ninja, GCC 16.2.0, Qt 6.11.2, installed AISuite/SNode.C dependencies and `codex-cli 0.154.0`. Browser packaging was disabled with `CODEXUI_INSTALL_WEB=OFF`; browser sources were not changed or tested. Compiler temporary files and build cache stayed below this build directory.

The following targets built with the repository's warning-as-error options:

```bash
cmake --build /tmp/codex/build/CodexUI/manager \
  --target codex-manager codexmanager-test codex-ui --parallel 14
```

The final targeted CTest entry passed in 13.31 seconds: **19 QtTest results passed, zero failed, zero skipped**, including initialization and cleanup. It covered persistence and conflicts, shell quoting, all 11 deployment plans, multiple homes, duplicate-start prevention, observer control release, ordered Unix startup, SSH supervision loss and adoption, refusal to stop foreign services, failed/cancelled startup, editor capability discovery, closing during pending helpers, activation/identity, manager screens and real local services.

Installed-command checks exercised 11 frontend choices × four provider choices × bridge/UI/readiness-client configurations. Configuration-only output is accepted only when it has the expected schema and no parser error diagnostic: the framework can print configuration alongside an argument error. These checks establish option compatibility; dummy certificate files do not qualify a TLS handshake.

```bash
xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  QT_SCALE_FACTOR=1.0 QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough \
  CODEXMANAGER_LIVE_QUALIFICATION=1 \
  CODEXMANAGER_EVIDENCE_DIR=/tmp/codex/build/CodexUI/manager/evidence/1.0 \
  TMPDIR=/tmp/codex/build/CodexUI/manager/tmp \
  XDG_CACHE_HOME=/tmp/codex/build/CodexUI/manager/cache \
  ctest --test-dir /tmp/codex/build/CodexUI/manager \
    -R '^codexmanager$' --output-on-failure --parallel 14
```

The live case started two genuine isolated stacks: a bridge-owned stdio app-server and a separately managed Unix app-server, each with its own actual CodexUI. Both reached protocol readiness and exposed separate activation sockets. Reopening used activation; stopping one left the other ready. It requested no inference, changed no authentication, and stopped its own services. The test redirects Qt settings below its temporary directory and removes its endpoint artifacts after verified shutdown.

The actual UI, editor, disclosure and activation cases also passed under Xvfb/offscreen at DPR 1.25, 1.5 and 2.0. Geometry cases used widths 740, 1240 and 1600. Screenshots were inspected for clipped controls and inaccessible details. The offscreen plugin reports that `raise()` is unsupported; IPC activation and widget visibility were tested, while real desktop focus behavior was not qualified.

Task-relevant C++/headers passed `clang-format --dry-run --Werror`; new/adapted CMake modules passed `cmake-format --check`; the tracked diff passed `git diff --check`. Shared formatting/analysis baseline files were copied into the repository and integrated through explicit targets, with discovery adapted as documented in `AGENTS.md`. Formatting was not run across unrelated existing sources.

The normal install rules were exercised with `DESTDIR=/tmp/codex/build/CodexUI/manager/stage`, producing both executables and their desktop entries/icons. This was a staged packaging check, not installation into the running system.

## Evidence, limits and accounting

[Overview](../media/codex-manager-overview.png), [expanded details](../media/codex-manager-details.png), and [profile editor](../media/codex-manager-editor.png) are unmodified Qt widget captures using disposable demonstration profiles and simulated status helpers. Their visual evidence must not be confused with the separate real-service lifecycle test. Additional screenshots and logs remain under `/tmp/codex/build/CodexUI/manager`; remove that directory when the development build and evidence are no longer needed. No additional checkout or worktree was created. Final cleanup verification found zero loaded qualification units, no remaining `codexmanager-*` runtime sockets/locks, and no test settings tree in the user home. Failed-run fixture directories and temporary editing scripts were removed.

Live SSH hosts, network/TLS handshakes, Bluetooth hardware, authenticated model work and real desktop window focus were not tested. SSH process ownership/reconnection was tested through service/SSH fixtures. TLS trust and SNI behavior remain provided by the installed SNode.C implementation; the manager adds no hostname-verification mechanism. Registered listener collisions and effective local home aliases are checked; arbitrary SSH aliases cannot be proven to name the same host.

Production C++/headers: **+2763 / −40 lines**, net **+2723**. Test C++: **+714 / −0 lines**. These are physical lines, including comments and blank lines. Growth implements the requested new executable and lifecycle capability, which had no existing owner; shared UI, transport and conversation implementations were reused. Build, documentation, assets and copied tooling configuration are accounted separately from these C++ totals.

Implementation/test source fingerprint: `9aebecbabe2b507eb2b3f732e82b85d537616e0b91466ce75ae5029ffafad2e9`. It is SHA-256 over sorted path names plus NUL, file bytes plus NUL, for manager `.cpp`/`.h` files, native `ProfileWindow.cpp`/`.h`, `src/codex/main.cpp`, root/manager/test CMake files and `tests/manager/ManagerTest.cpp`.
