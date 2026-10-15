<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-web-ui-compact.svg"><img src="../media/nav/banner-web-ui.svg" alt="CodexUI — Web UI" width="100%"></picture></p>
<p><a href="README.md#banner" title="Up to Web UI"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-web-ui-dark.svg"><img src="../media/nav/up-web-ui-light.svg" alt="Up to Web UI" title="Up to Web UI" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/account-dark.svg"><img align="right" src="../media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# CodexWebUI qualification

This record qualifies the browser frontend against
[the web 1.0 contract](contract-1.0.md#banner). Cross-frontend agreement is
evidence-backed only where the same fixture corpus is executed by native C++
and browser TypeScript. The other suites are browser-local regression evidence;
they neither execute Qt nor claim pixel identity with it.

## Browser catch-up qualification

The browser catch-up suite executes production React controls against an isolated simulated WebSocket provider. It covers project metadata preservation, section creation, empty/grouped navigation, search and archive filters, grouped first-prompt creation, token/context display, timing navigation, command/output scroll and folding, agent content and retained disclosure, pending-request attention, pane controls, and explicit failed-prompt restoration. These are browser-local interaction checks; they do not establish real provider persistence or cross-platform screen-reader behavior. Set `CODEXUI_BROWSER_SCREENSHOTS` to capture the desktop verification frame for visual inspection.

The shared `frontend-presentation.json` corpus now includes timing-path/unit and token-snapshot cases executed by both native `NodeGraphUiAdapterTest` and browser `browser-catchup.test.mjs`. Browser tests also verify that `TimingPaths.ts` exactly matches the generated native `ProtocolTimingPaths.inc` table. Regenerate it with `node web/tools/generate-timing-paths.mjs` after changing the schema-declared native timing paths. Frontend-local request tests separately cover stale search results, query pagination and retry, generation retirement, unavailable catalogs, group deletion/recreation, observer gating, fresh project metadata, section-assignment failure, and restoration identity.

### Local verification — 2026-10-15

The implementation was verified in the working tree based on `ee461fe`, preserving the pre-existing workflow, build-guide, package, and browser-build changes. Node.js v24.21.0 and Chrome 155.0.8059.39 were used. The installed AISuite frontend SDK was resolved through `/usr/local`; the full CMake-prepared isolated release passed 402 browser tests, presentation profiling, production bundling, Chrome qualification, and arbitrary-base-path artifact relocation. All 83 native CTest cases passed under Xvfb with `QT_QPA_PLATFORM=offscreen`, 14-way execution where applicable, and `CODEXUI_TIMING_POLICY=report`. The native adapter aggregation benchmark reported one accepted elapsed-time warning; this run does not establish that the native timing overrun was fixed.

| Measurement | Before catch-up | Final isolated release | Existing limit |
| --- | ---: | ---: | ---: |
| Model: 10,000-item hydration | 24.31 ms | 24.95 ms | 47 ms |
| Model: complete projection | 15.44 ms | 15.29 ms | 45 ms |
| Model: 2,000 streaming deltas | 2.48 ms | 2.36 ms | 4 ms |
| Model: 10,000 presentation lifetimes | 5.52 ms | 6.14 ms | 20 ms |
| Browser: 10,000-item hydration wall time | 86.00 ms | 86.10 ms | 141 ms |
| Browser: 2,000-delta settlement | 25.60 ms | 25.20 ms | 47 ms |

The browser retained 80 resident cards for the 10,000-item workload, stable card/text/settings identities, zero settings structural mutations during streaming, and a detached-anchor displacement of 0.391 px against its 1 px bound. These are individual local measurements, not statistically established performance improvements. Desktop, project-dialog, mobile, and mobile-Threads screenshots were inspected; the populated mobile shell retains its footer, and group menus reuse the existing viewport-bounded keyboard/focus controller. The production bundle passes qualification and emits Vite's advisory for a JavaScript chunk above 500 kB.

Production accounting for this change is +782/-180 lines (net +602), including the generated timing table, its generator, and only this task's additions to the pre-existing untracked browser CMake setup. Tests, fixtures, and browser qualification support are +803/-61 lines (net +742). Documentation and unrelated pre-existing changes are excluded. Growth supplies previously absent workflows and typed projections; the previous singleton thread pager and separate group-popup implementation were removed. The existing single rollout-repair timer is reused. Query identities and causal revisions retire with their provider lifetime; command scroll and agent disclosure use the existing thread-presentation store; pane geometry has one React owner, and keyboard resizing uses the displayed pane widths after a window-size change. Browser `ResizeObserver` settlement distinguishes its own scroll events from user detachment without a new polling timer.

The new native/browser differential evidence covers the shared timing and token corpus. Other new workflows are browser-local simulated-provider evidence. Real app-server project persistence, cross-client upstream interoperability, real screen readers, and native compositor behavior were not qualified by this change; the native-only filesystem, Git, desktop integration, and transport exclusions remain in force.

## Native visual alignment — 2026-10-15

The working tree based on `a25498d` aligns supported browser presentation with the native reference: generated palette and brand, compact Noto Sans typography, pane widths/splitters, native thread/group spacing, restrained card geometry, shared Copy/disclosure glyphs, disabled colors, header facts, visible two-column turn settings, and a compact expanding composer. Native `UiStyle` remains the reusable color authority; a generator and drift test replace the independent browser palette. Standard menus now use platform popovers, deleting the settings visibility state and avoiding closed-details deferred layout. The existing composer observer also handles width changes; no additional observer or timer was introduced.

The isolated CMake release against the installed AISuite frontend SDK passed all 403 browser tests, profiling, production bundling, real Chrome interaction qualification, and relocatable artifact verification. Node.js v24.21.0 and Chrome 155.0.8059.39 were used. The browser checks actual repeated typing, Backspace, cursor movement, selection, Shift+Enter, suppressed repeated submission Enter, draft-preserving resize, menu keyboard focus/Escape/outside dismissal, nested drawer Escape, and bounded scrolling with 200 thread rows. Existing control eligibility, streaming identity, focus, anchors, and performance gates remain unchanged.

| Measurement | Before catch-up baseline | This isolated release | Existing limit |
| --- | ---: | ---: | ---: |
| Model: 10,000-item hydration | 24.31 ms | 24.44 ms | 47 ms |
| Model: complete projection | 15.44 ms | 16.31 ms | 45 ms |
| Model: 2,000 streaming deltas | 2.48 ms | 2.47 ms | 4 ms |
| Model: 10,000 presentation lifetimes | 5.52 ms | 6.23 ms | 20 ms |
| Browser: 10,000-item hydration wall time | 86.00 ms | 77.80 ms | 141 ms |
| Browser: 2,000-delta settlement | 25.60 ms | 28.20 ms | 47 ms |

These are individual local measurements, not statistical improvement claims. The browser retained 80 cards, two layout/style passes for hydration and streaming, zero settings/conversation structural mutations during streaming, and 0 px detached-anchor displacement. The bundle still emits Vite's advisory for a JavaScript chunk above 500 kB while passing the artifact gate.

Nine relevant native layout, thread-pane, and timestamp CTests passed under Xvfb with `QT_QPA_PLATFORM=offscreen`, 14-way execution, and `CODEXUI_TIMING_POLICY=report`; native production sources were unchanged. Native widget and browser screenshots were inspected at DPR 1, 1.25, 1.5, and 2, plus narrow browser layouts. Browser measurements confirm a 50 px compact form/32 px editor at all four scales and the native yellow selection colors. Local visual evidence is in `/tmp/codexui-native-ui-comparison-20261015`; this temporary directory is not a durable CI artifact. The Qt reference is an isolated offline widget fixture; browser workflows use a simulated provider. Different content and platform text rasterization prevent a claim of pixel identity. Noto Sans uses the installed local font with system fallback elsewhere. This change does not qualify real upstream persistence, compositor behavior, or real screen readers; the contract's native-only filesystem, attachment, Git, desktop, and transport exclusions remain.

Production accounting is +326/-326 lines (net 0), including the palette/brand generator and generated assets. Tests and browser qualification support are +109/-38 lines (net +71). Documentation and the unrelated untracked proposal are excluded. The change replaces existing presentation and interaction policy rather than retaining a second implementation.

## Verification gates

### Accepted timing policy — 2026-09-22

The owner accepted the outstanding elapsed-time overruns as non-blocking.
CI sets `CODEXUI_TIMING_POLICY=report`: native elapsed-time assertions (including
graph scaling comparisons), the browser model profile and production-browser
qualification still measure the same workloads against the same limits, but
report timing overruns as warnings. Without this setting they remain strict.
Native CI uses verbose CTest output so accepted overruns and measurements remain
visible even when a test passes. This is an explicit acceptance of timing risk,
not evidence that an overrun was fixed or that interaction is universally lag-free.

Correctness, completion timeouts, residency/memory bounds, work counts,
layout/style counts, identity, focus, anchors and artifact checks still fail CI.
Functional predicates remain mandatory even when combined with timing
comparisons. No blanket `continue-on-error` is applied to a suite containing
correctness checks.

- Generated protocol names, operation maps, and source-schema digest match the
  checked-in C++ protocol.
- The shared frontend-presentation corpus is executed by both C++ and
  TypeScript and checks status normalization, lifecycle fallbacks, active-turn
  identity, Plan replacement/outcomes, and Agent status precedence.
- Browser-local SDK tests cover routing, pending callbacks, bridge roles,
  provider generations, disconnect failure, WebSocket framing, and
  backpressure.
- Browser-local normalizer and presentation-model tests cover authority,
  generation, sequence, hydration, terminal state, repository hints, and child
  ownership.
- Conversation tests cover grouping, prompt admission,
  materialization, reasoning-first order, history, terminal output, image
  metadata, and agent correlation within the browser implementation.
- Browser-session tests use a scripted bridge endpoint and assert the protocol
  presentation-action methods, resume-before-turn ordering, provider
  recovery, observer direct paging, one-page cursor continuation/retry,
  chronological multi-page item merge, active-child hydration, the global
  eight-item-page ceiling, and malformed-input containment.
- Settings and pending-request tests assert the protocol request payloads.
- Server rendering verifies the shell landmarks and accessible connection and
  disclosure controls without requiring a second UI state implementation.
- Viewport tests cover stable card/pixel anchors, native folding geometry, and
  nested-scroll boundary ownership; responsive tests cover the 1160/760 mode
  boundaries, semantic thread-tree selection, focus styling, contrast tokens,
  and coarse-pointer targets.
- The packaging gate rejects a missing production artifact, performs both
  standalone and combined CMake installs, and verifies the staged entry point
  and assets.

## Measured presentation cost

Measurement command:

```sh
npm run profile --prefix web
```

Representative input: one authoritative thread containing 100 turns and
10,000 mixed conversation items, followed by 2,000 deltas applied to one live
item. Five fresh Node 24.19.0 processes were run on 2026-09-14 on an Intel Core
i7-14700HX (x86-64). Limits are the rounded-up 1.25× observed maxima.

| Operation | Median | Five-run range | Hard limit |
| --- | ---: | ---: | ---: |
| Authoritative hydration, 10,000 items | 37.09 ms | 32.90–37.37 ms | 47 ms |
| Full projection, 10,000 visible cards | 35.46 ms | 35.07–35.61 ms | 45 ms |
| Apply 2,000 streaming deltas | 2.99 ms | 2.87–3.06 ms | 4 ms |

The production view starts with the native 80-item history window, schedules
at most one React publication per animation frame, retains keyed cards, and
increases the effective window while a reader is paused. The 10,000-card full
projection is therefore a qualification stress case, not the initial DOM
surface.

No speculative cache or alternate reducer was introduced: the measured pure
model and projection remained comfortably below one display interval for the
normal 80-item window, so the native authority/index structure was retained.

The production-App gate uses the built Vite artifact, a scripted WebSocket at
the real bridge boundary, and a fresh headless Chrome 151 process per run:

```sh
npm run build:app --prefix web
npm run qualify:browser --prefix web
```

Five runs on the same date and machine produced these local baselines. The gate
also requires exactly 80 resident cards, stable card/text/settings-control
identity, zero structural mutations outside the streamed text node, follow-tail
within 1 px, and a detached semantic anchor within 1 px.

| Production-App phase | Median | Five-run range | Hard limit |
| --- | ---: | ---: | ---: |
| 10,000-item hydration wall time | 93.20 ms | 90.80–112.80 ms | 141 ms |
| 10,000-item hydration task time | 102.14 ms | 98.93–122.66 ms | 154 ms |
| Idle task time | 0.70 ms | 0.51–0.77 ms | 1 ms |
| Equal-settings acknowledgement task time | 19.77 ms | 18.10–19.95 ms | 25 ms |
| 2,000-delta synchronous ingest | 14.80 ms | 13.90–15.80 ms | 20 ms |
| 2,000-delta settlement | 36.30 ms | 35.30–37.30 ms | 47 ms |
| 2,000-delta task time | 36.66 ms | 35.69–37.77 ms | 48 ms |

Hydration was exactly two layout/style passes in every sample; streaming was
exactly one; idle and the semantic settings no-op were exactly zero. These are
local regression gates, not portable hardware performance claims.

CI runs functional checks on Node 22 and runs qualification on Node 24.20.0
and Chrome for Testing 152.0.7977.82. Those exact versions are held stable for
comparisons; they do not pin SNode.C or AISuite, which both follow `master`.
The profile prints its Node version and browser qualification prints the actual
Chromium version. Timing limits are unchanged; runner timing still needs to be
distinguished from semantic failures.

On 2026-09-22 the hosted runner moved from Node 24.20.0 / Chrome 152.0.7977.82
to Node 24.21.0 / Chrome 153.0.8010.52 between two successive commits.
The latter run timed out starting Chrome; its diagnostic browser run also
reported five hydration style passes rather than two. This does not establish
a browser regression, but makes an uncontrolled runtime unsuitable for isolating
application changes. The fixed versions above passed local production-browser
qualification and precede that runner change. Runtime updates require explicit
requalification, not a change to performance limits. The original 2026-09-14
numbers above remain historical measurements, not measurements of these versions.

### Paginated-protocol requalification — 2026-09-21

The browser fixture now follows metadata-only `thread/resume`,
`thread/turns/list` and `thread/items/list`; it rejects legacy `thread/read`.
Its 80 turns × 125 items retain the original 10,000-item / 80-visible-card
initial-render workload, without measuring an additional history expansion.
Idle is observed for 33.3 ms from the host, without running test animation
callbacks inside the browser. All existing limits above remain unchanged.
An injected idle workload fails at 7.97 ms, as intended.

With the local AISuite inbound-frame bound correction, ten serial fresh
production-browser processes passed every gate. Ingest was 6.3–7.1 ms;
hydration wall/task maxima were 114.2 / 118.63 ms; streaming settlement/task
maxima were 37.3 / 33.03 ms. Idle remained at most 0.14 ms with no layout/style
work. Three immediately preceding runs with the original SDK ingested in
8.6–10.1 ms. Separate instrumentation confirmed 2,000 input frames delivered
with 2,000 -> 0 temporary frame-size encoding buffers. Byte limits and
malformed-message handling did not change.

The Node gate passed 50/50 fresh-process runs, with maxima of 29.826 / 20.306 /
3.525 / 6.738 ms for hydration / projection / streaming / presentation churn.
SDK tests passed 44/44 and browser tests passed 330/330 with concurrency 14.
Timing runs were serial and separate from builds/tests; all execution checks
used `xvfb-run -a env QT_QPA_PLATFORM=offscreen`. The production artifact was
rebuilt and its relocatability verified. Intermediate failures, source/LOC
accounting, exact commands and qualification limitations are retained in the
[remediation ledger](../architecture/remediation/findings-ledger/README.md#banner).
This is local-worktree evidence, not a claim of universally lag-free operation;
the AISuite source change must accompany CodexUI in subsequent releases.

## Browser representation and resilience

- WebSocket and WSS are the only exposed transports.
- The bridge URL is explicit and retained in browser local storage.
- Provider-local paths are presented as metadata; no local browser filesystem
  capability is implied.
- Provider generation changes clear provider-owned presentation state and
  trigger discovery/hydration through the existing bridge authority.
- Notices are bounded to one visible message and follow the native 10-second
  error and 6-second informational auto-dismiss policy.
- Reduced-motion preference disables shimmer, spinner, and smooth scrolling.
- The center pane owns per-thread follow/paused scroll state with stable
  card/pixel anchors; a paused anchor is not evicted when streamed items
  arrive. The composer grows upward over an opaque reserved surface while
  command surfaces retain their own follow/pause and wheel-boundary ownership.
- The release gate starts a real headless Chromium at 760, 521, and 360 px and
  confirms zero document-width overflow with the full connection-control
  shape. It also verifies the visible focus ring, synchronous drawer focus,
  inert background, bidirectional Tab wrapping, Escape focus return,
  breakpoint-removal fallback, and 44 px coarse-pointer targets.
