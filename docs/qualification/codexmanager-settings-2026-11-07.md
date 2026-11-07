# CodexManager Qt web settings qualification — 2026-11-07

Canonical checkout: `/home/voc/projects/drafts/CodexUI/codexui`. The requested prerequisite branch `fix/thread-navigation-grouping` was reviewed, committed as `3ab4466`, `717921f` and `4316c82`, and merged locally into `master` as `4586974`. Its native layout, inspector and prompt tests passed, including four-DPI navigation checks, the rebuilt shell integration test, 23 browser unit cases and real-Chrome qualification. No prerequisite changes were discarded. This settings task is based on that merge on `feature/manager-web-settings`.

## Scope and ownership

The manager menu opens a Qt web settings dialog covering HTTP/HTTPS enablement, IPv4 bind addresses and ports, authentication username/password-file path, and HTTPS certificate/key paths. SNode.C remains the sole configuration authority. Qt owns a transient form; owning JSON requests/results cross bounded SPSC queues using the existing eventfds. All configuration reads, API-default changes, validation, per-option promotion, rollback and `utils::Config::writeConfig()` calls execute on the SNode.C event-loop thread. No Qt preference storage, subprocess writer, extra event-loop thread or independent configuration serializer was introduced.

Saving uses the first path in SNode.C's effective configuration-file list, displayed read-only before saving; it does not silently switch to the writer's default path. Non-regular destinations such as `/dev/null` are rejected. The ten editable options are individually promoted at startup so explicit default-equivalent settings survive subsequent saves. Existing configurable values outside the form remain part of SNode.C's generated configuration. Listener/authentication changes require restart. Running status and browser URLs come from listener reports rather than edited values, and the UI explains that command-line arguments retain precedence.

## Validation

Release build, GCC 16.2.0 and Qt 6.11.2 with the newly installed SNode.C promotion/write APIs and existing AISuite components. Built manager, settings test, manager regression test and CodexUI with `-j14`. Tests use Xvfb with `QT_QPA_PLATFORM=offscreen` and `-screen 0 1280x1024x24 -extension GLX`. Socket-using tests ran outside the sandbox's network restrictions. Build, logs and screenshots are retained under `/tmp/codex/build/CodexUI/manager-web-ui` for review; remove them when no longer needed after handover. No additional checkout was created.

| Check | Result | Evidence |
| --- | --- | --- |
| Actual Qt dialog and SNode.C persistence | Passed: checkbox activation, default-equivalent port emission, unrelated log-level preservation, invalid port/credentials/certificate rejection, directory and read-only-file save failures, in-memory rollback, and rollback with an externally removed certificate | `settings-dpi-*-cases.log` |
| Fresh-process load/save-again | Passed: the settings test launches a new process, loads the saved file, verifies active links, saves without changes, and checks that the active port setting remains present | Same logs; child success explicitly asserted by the parent |
| Actual manager restart from the saved file | Passed: authenticated HTTP and HTTPS `/api/state` return 200 on the saved ports, then the manager shuts down normally | Same logs |
| Settings dialog DPI and narrow geometry | Passed at DPR 1, 1.25, 1.5 and 2; 460 × 520 layout has no horizontal form overflow, lower fields remain reachable through scrolling, and Save stays within dialog bounds | `settings-dpi-*-cases.log`, `settings-dialog-*.png` |
| Existing manager regression | Passed: 57 Qt cases; the opt-in real-services case runs separately below | `settings-accepted.log` |
| Guarded deletion and real-Chrome manager interface | Passed: deletion helper plus HTTP authentication/origin/static checks, profile/SSE/account fixtures, selected CodexWUI navigation, TLS, concurrent delivery and shutdown | `settings-accepted.log` |
| Real native CodexUI launch and profile isolation | Passed with two disposable real backends, actual manager launch buttons, native activation and deletion/stop isolation | `settings-live.log` |
| Configuration-only startup | `--show-config` exits successfully and emits promoted options using SNode.C's syntax | `settings-schema.log` |
| Formatting and whitespace | Task-related clang-format and CMake checks and `git diff --check` passed | Review commands |

The settings case named `reloadAndSaveUnchanged` is deliberately skipped in the parent process and executed explicitly in the fresh child process; the parent asserts the child's specific passing case and exit status. The initial existing native regression encountered a stale CodexUI executable after the SNode.C update, reporting a shared-object size mismatch. CodexUI was rebuilt against the installed headers/library; the accepted regression and real launch then passed. Earlier settings-test development corrected checkbox hit targeting, status capitalization and SNode.C's qualified configuration-key expectations. Accepted results are from the final source and test revision.

Screenshots at DPR 1 and 2 were inspected. The narrow captures intentionally scroll the form to the last field; the configuration destination, running status, restart guidance and Save/Close buttons remain visible. HTTPS tests use disposable self-signed certificates with trust checking bypassed only by the test client; deployed certificate trust is not qualified. Real SSH hosts and OpenAI authentication/inference are outside this change. No personal profiles, credentials or services were altered. Mid-write atomic file replacement is not claimed: persistence is delegated to SNode.C's current write API.

## Accounting and delivery

Production: **276 added, 38 removed, net +238**, within the additional 300-line approval. Tests: **377 added, none removed**. Documentation is separate. Growth adds the requested settings presentation and cross-thread configuration flow; the existing password-file reader was consolidated for startup and validation. No installation, push or merge of this settings feature was performed. The prerequisite navigation merge is local; settings changes remain available in the canonical checkout for review.
