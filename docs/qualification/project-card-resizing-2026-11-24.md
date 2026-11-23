# Native project-card resizing qualification

This run applies the affected portions of the [canonical native suite](native-test-suite.md#banner), especially sections 2, 4, 6, 20, 21 and 22.3. It does not claim qualification of every unrelated native workflow.

## Change and ownership

The governing invariant is that resizing a project's lower boundary changes only that project's body height; all following outer-tree content moves by the same delta and keeps its own dimensions, subject to the panel's natural scroll-range clamp. The former unconditional eight-row cap could not express a chosen project height.

The complete path is NodeGraph membership and hierarchy → NodeGraphUiAdapter's thread snapshot → ThreadPane's stable presentation-key reconciliation → ThreadTreeItem → the shared projectBodyHeight calculation → the existing outer-tree delegate and ProjectThreadView geometry. Mouse input, Alt+Up/Down and accessibility actions all call the same resize operation. The NodeGraph, adapter, protocol and browser renderer are unchanged. The project row already owns local project scroll state; it now owns the chosen height too. Its body remains a projection of the same model, selection and delegate.

The correction replaces the unconditional cap with an eight-row default or a chosen pixel limit. Smaller expanded content still shrinks naturally. At least one complete row remains after a font increase. The gap uses the existing row boundary and color tokens; there is no splitter-widget hierarchy, alternate renderer, new timer, cache, dynamic property or persistence format. The three added state members are the row's chosen height, an optional persistent index owning the active gesture, and its pointer offset. An engaged gesture outlives removal of its model index so remaining input cannot fall through to another project. Release, lost capture and hiding end ownership.

Snapshot placement now preserves the existing row expansion state across Qt take/insert operations. The interrupted-drag test exposed that moving a following project after removal could otherwise collapse it and change its height. This correction stays at the existing placement boundary and adds no retained state.

Height survives refresh, collapse/expand and body-view retirement/recreation while the semantic project row remains retained. Restart persistence and retention after that row is removed from the list are outside this change. Internal scroll follows Qt's natural range clamp when the viewport grows.

## Source and build

Canonical checkout: `/home/voc/projects/snodec/CodexUI/`. Qualification branch: `feat/project-card-resizing`, based on `f4a86bc` (includes the workspace-action lifetime crash fix). This record covers local builds and simulated execution before committing. No installation was performed.

Temporary Release build: `/tmp/codex/build/CodexUI/project-card-resizing/`, using GCC 16.2.0, Qt 6.11.2, Ninja, `BUILD_TESTING=ON` and `CODEXUI_INSTALL_WEB=OFF`; compilation uses `-j14`. This build consumes the canonical source directly. The existing manual build therefore sees the same source changes. Compiler/test temporary files are under the build's `tmp/` directory. Evidence is under `/tmp/codex/artifacts/CodexUI/project-card-resizing/`. No additional checkout or source copy was created. Retain the build and evidence for review; remove those two task directories after acceptance.

## Verification

The final `codex-ui`, thread-pane, application-layout and shell-integration targets built successfully. Changed C++ ranges were formatted with the committed canonical configuration; a second formatting pass produced identical bytes. `git diff --check` passed. Final production and test sources were unchanged after the successful runs below.

All execution used Xvfb with `QT_QPA_PLATFORM=offscreen`, isolated preferences and task-local temporary directories. The final Xvfb wrappers exited successfully; their logs contain nonfatal xkbcomp warnings. The initial sandboxed application-layout run could not create its isolated Unix socket and timed out. Running that same suite outside the sandbox resolved the fixture restriction; the final run passed. The captured fixtures use the native UiStyle stylesheet and Qt's default offscreen style; the feedback case explicitly enables normal motion.

| Check | Result | Evidence |
| --- | --- | --- |
| Thread-pane UI suite, DPR 1.0 / 1.25 / 1.5 / 2.0 | Passed 4/4; includes all existing cases and the added resize, interrupted-drag, font-change, focus, selection, accessibility, feedback and pagination cases | `final-matrix.log` |
| Grouped 10,000-thread performance and complete resize gesture, all four DPRs | Passed 4/4; exact upper/following geometry, unchanged widget/document residency and 100 ms gesture gate | `final-matrix.log` |
| Existing application-layout regression suite | Passed | `final-matrix.log` |
| Existing nested-wheel routing/performance regression | Passed | `final-matrix.log` |
| Actual ShellWidget resize plus existing workspace-action lifetime regression, all four DPRs | Passed 4/4 processes; each covers both requested shell widths and both font sizes, including a graph update during the drag | `final-shell-dpr-*.log`, `final-shell-results.json` |

CTest ran the ten final selected tests with `-j14`; its performance cases retained their existing serial measurement policy. The separate four-DPR shell matrix used a 14-worker executor. No failure suppressed another selected case.

The interaction tests verify upward and downward drag reversals, exact upper-card/body resizing, unchanged following-card dimensions, preserved selection/focus/accessible identity, scroll retention within range, restored height after refresh/folding/offscreen recreation, one-row and loaded-content bounds, a font increase at the minimum, disabled and collapsed states, tooltip text, both keyboard directions and both accessible actions, capture loss, hiding and project removal during a drag. The additional visibility case verifies that revealing pending content starts the existing animation owner and requests its page, and hiding it stops the animation.

## Measurements

Baseline measurements were recorded before production edits. The complete resize gesture includes dispatch, synchronous layout, Qt event processing/painting and release over a populated 10,000-thread projection. These are local synthetic measurements; they do not measure upstream/network latency or establish an overall speedup.

| DPR | Baseline population (ms) | Final population (ms) | Final complete resize (ms) |
| --- | ---: | ---: | ---: |
| 1.0 | 87.899 | 87.724 | 4.894 |
| 1.25 | 87.267 | 88.170 | 4.632 |
| 1.5 | 87.061 | 87.714 | 4.999 |
| 2.0 | 87.190 | 89.089 | 5.113 |

Every grouped profile retained 32 fixed / 56 populated widgets, zero index widgets and one QTextDocument, matching the baseline. Semantic no-ops retained model/pixels/focus/anchor and produced zero paints. Offscreen animation remained stopped, while visible animation remained bounded. The smaller interaction fixtures recorded 1.172–2.578 ms for their slowest move-to-paint samples. The complete-gesture gate is 100 ms, with the existing instrumented-build allowance.

## Visual review

The 224 final captures include 160 pane-state images, 16 tooltips and 48 complete-shell before/during/after images. The pane cases request widths 260/440 and fonts 10/16; the shell cases request 1500/1800 × 1000 and fonts 10/16, all at the four DPRs. Contact sheets in `review/` preserve logical aspect ratios, include full-shell views and legible project close-ups, and were opened and examined. Original PNGs remain under `captures/`.

| Canonical element/state | Observed result | Verdict |
| --- | --- | --- |
| Project enclosing card, lower grip, hover and press | One enclosing border and a centered subtle grip; no extra reserved gap, duplicate surface or movement of the heading during the drag | Passed |
| Shrinking / growing / reversal frames | Following card keeps its shape and content dimensions while moving with the upper boundary; fractional-height viewports retain native scrolling | Passed |
| Minimum / maximum / section content | At least one full content row; growth stops at expanded loaded content; natural smaller content leaves no blank reservation | Passed |
| Keyboard focus / selection | Existing project-row focus/selection appearance remains, with a stable resize target and shared accessible actions | Passed |
| Disabled / collapsed | No active resize grip/actions; the collapsed project returns to its complete heading geometry | Passed |
| Tooltip at every DPR/font | Complete text, legible padding and border; the enlarged DPR-2 tooltip wraps to two complete lines | Passed |
| Full shell, including incoming graph update | Before/during/after layout remains stable outside Threads; all pixels from logical x=450 to the right edge matched each scene's initial capture | Passed |

The standalone fixture obeys Qt's existing minimum-size hints: requested width 260 becomes 299 px at font 10 and 429 px at font 16; requested width 440 remains 440 px initially. The deliberate larger-font transition can leave that top-level fixture at 479/610 px for later disabled/collapsed captures. `capture-metadata.json` records each actual captured extent. Widths below those minimum hints are not qualified by these standalone images. No production width policy was changed to force them.

## Accounting and remaining limits

Production: **+140 / −47 lines, net +93**, within the agreed 150-line ceiling even when counting inserted lines alone. Tests: **+541 / −3 lines, net +538**, counted separately. Documentation is excluded from both counts. Growth supplies the previously absent gesture and its keyboard/accessibility path, replaces the rigid cap in the existing geometry owner, and preserves expansion at the existing placement boundary. There is no parallel implementation to retire.

Review found no added timer, cache, dynamic property, alternate renderer, protocol authority, compatibility path or source checkout. The existing animation and paging owners are refreshed after geometry changes. Final source/binary hashes and checkout identity are in `final-manifest.json`.

Real-desktop pointer capture/compositor behavior and real screen-reader integration remain unverified by Qt offscreen. The graph-backed shell fixture is simulated, not a real app-server interoperability or live-stream/burst test; no personal backend data was touched. Browser behavior and restart persistence were not added or qualified. These limits do not change the verified native, renderer-local resize behavior.
