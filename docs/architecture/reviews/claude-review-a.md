# External architecture review — Claude A

I ran the audit directly against a fresh clone rather than delegating it. I hit my tool budget for this turn partway through the file-by-file sweep, so below is a **partial audit** — everything stated is source-verified at the exact commit, and I've marked precisely where coverage stops.

## Commit audited

`629660e8882bddc7473ee5f1aab90c755ee6edd4` — "Fix conversation card presentation and interaction", Sat Sep 12 02:25:18 2026 +0200. This **is** the current `master` HEAD (553 commits). It did not advance.

**Access:** I cloned and read the tree, history, CI workflow and CMake. I did **not** build, run, execute any test, or view any rendered pixel. No runtime claim below is mine. The uncommitted local changes to `ConversationCards.cpp`, `ConversationView.cpp/.h`, `ConversationCardsTest.cpp`, `ConversationVirtualizationTest.cpp`, `App.tsx` are outside my evidence entirely.

## The core defect is a triple, not a double, derivation

The known defect is real and worse than "two renderers". For one visible card, the committed code derives geometry **three independent times**, and they provably disagree in source:

| Quantity | Passive delegate | Materialized `ConversationCard` | Passive *hit test* |
|---|---|---|---|
| Corner radius | `drawRoundedRect(bounds, 10.0, 10.0)` — `ConversationView.cpp:398` | QSS `border-radius: 8px` for `[messageRole=user]` / `[messagePhase=final]` / `[update]` — `UiStyle.cpp:252,256,258` | — |
| Copy ink X | `rect.right() − 43` (`CopyInkLeftFromRight`) | button at `right−13−14−4−16`, ink `+4.5` ⇒ `right−42.5` | `rect.right() − 52`, width 24 |
| Copy ink Y | `rect.top() + 14.0` | header top `+5.5` ⇒ `rect.top() + 16.5` | `top`, height 24 |
| Copy control width | implicit 8/9px glyph | `CopyControlWidth = 16` | hardcoded **24** |
| Disclosure width | `DisclosureControlWidth = 14` | `DisclosureControlWidth = 14` ✓ | hardcoded **24** |
| Frame extent | `CardFrameExtent = 2` (constant) | `2 * card->frameWidth()` (queried) — `ConversationView.cpp:2738` | — |
| Body inset | `left + 12`, width `W − 28` | viewport at `left + 13`, doc width `viewport()->width() − 2` — `ConversationCards.cpp:1096-1102` | same as paint |

The `− 28` and the `− 2` are tuned to agree on **width**; nothing makes them agree on **origin**. The body text is laid out at the same width but painted 1px apart, so every materialization shifts the whole Markdown surface horizontally without any semantic change. The corner radius changes 10→8. The Copy glyph moves ~1.5px left and ~2.5px down. The disclosure hit zone shrinks from 24px to 14px — a click that toggles a passive card lands on dead margin once it is materialized.

**Architectural cause:** `passivePresentation()` (`ConversationView.cpp:236-330`) is a second, complete presentation projection of `VisibleCardData`, parallel to `ConversationCard::Impl::createComposition`. It re-derives titles, statuses, backgrounds, borders, title colours and block lists from the same payload. It also hardcodes colour literals — `#f4f3fd`, `#cec7f6`, `#59507f`, `#f9f4ea`, `#e1cb9d`, `#6b5521`, `#667085`, `#1d2633` — that are already named tokens in `UiStyle.h` (`purpleSurface`, `purpleBorder`, `purpleText`, `yellowSurface`, …). The delegate bypasses the token layer the QSS uses, so a token change silently desynchronises the two renderers. `ConversationView::paintEvent` adds a *third* painter for the turn surface with `#6f98e8`, which is not any UiStyle token at all.

**The invariant that is duplicated:** "a row's pixels are a pure function of its `VisibleCardData` + width + collapse state." Two implementations of a pure function cannot be kept equal by construction — only by test, and the tests do not check it (below).

## Materialization on mouse-down is a visible-jump generator

`ConversationView::mousePressEvent` (line 3770) computes a passive hit, then calls `materializeRow(index.row(), true)`, which replaces the delegate-measured height (`delegate->cardSize`) with a widget-measured one (`measureCard`), then explicitly compensates:

```
if (before != heights_.totalHeight()) { updateScrollRange();
    if (follow) setScrollValue(max); else restoreAnchor(anchor); }
```

That `if` is the architecture admitting the two measurements disagree. The compensation runs on **press**, before any drag — the user sees content move under the cursor at the moment of clicking. `restoreAnchor` hides the scroll shift but cannot hide the row-height change itself.

## Complexity accumulated around the invalid boundary

Measured from git history:

| Commit | Date | `ConversationView.cpp` | `ConversationCards.cpp` |
|---|---|---|---|
| `6c4a0e1` Virtualize conversation row materialization | 09-09 | 1388 | 1936 |
| `bd2297e` Paint passive rows with a delegate | 09-09 | **2051** | 1956 |
| `aab2723` Reuse Markdown documents across presentation | 09-10 | 3685 | 2141 |
| `629660e` HEAD | 09-12 | **4041** | **2673** |

3324 → 6714 lines in three days. `bd2297e` alone added 685 lines to `ConversationView.cpp` with 19 deletions across the whole commit — the delegate was added and nothing was removed. Every subsequent commit in that window is a compensating fix: "Preserve virtualized conversation interactions", "Prevent recursive conversation mouse forwarding", "Keep virtual conversation geometry exact", "Repaint rows displaced by card expansion", "Fix conversation card presentation and interaction". `aab2723` then built a 128-entry `QTextDocument` transfer cache with `takeMarkdownDocument`/`adoptMarkdownDocument` to hand documents *between* the two renderers — an abstraction added on top of the duplication rather than in place of it.

**LOC (HEAD):** production `src/**` C++ 37,822 (middle 14,156; codex root 11,862; nodegraph 8,201; ui 3,603). Tests C++ 29,128. Web TS/TSX 4,925 + 2,931 test. No `.ui`, `.qss` or `.qrc` files exist — all styling is C++ string literals.

## Test findings

- **`tests/codex/ConversationVirtualizationTest.cpp:2858`** — `main()` chains 28 suites with `&&`. It **short-circuits**: the first failure suppresses every later suite. The `CODEXUI_REMAINING_UI_TESTS` branch uses `&=` and does not. Severity: High — CI reports one failure where there may be twenty.
- **`passiveAndInteractivePresentationShareExactGeometry`** is the only guard on the core defect and compares **`visualRect(index).height()` only**. It cannot detect the horizontal shift, the radius change, the Copy-icon move, or any reflow that happens to preserve total height. Its fixture is 100 repeated `'W'` characters — no links, lists, code fences, tables, CJK, RTL or emoji — at one width (620) and one DPR.
- **`delegateAndWidgetChevronsMatchExactly`** compares `view.viewport()->grab().toImage()` (device pixels, screen DPR) against `paintedWidgetBounds()`, which renders into a `QImage` with **no** `devicePixelRatio` set (logical pixels). At DPR ≠ 1 these are different coordinate spaces. The test is DPR-1-only and silently so.
- **`codexui-conversation-view-benchmark`** is built (`CMakeLists.txt:445`) but **never passed to `add_test`**. There is no performance gate in CI at all.
- CI is a single `ubuntu-24.04` / gcc 15.3 / Qt6 Debug job with `QT_QPA_PLATFORM=offscreen`. No Release build, no Wayland, no XWayland, no HiDPI, no fractional scaling, no sanitizers, no accessibility assertions.
- The `web/tests/*-parity.test.mjs` files assert parity against **hand-copied literals**, never against executed C++. A C++-side divergence cannot fail them.

## Verified duplication inventory (partial)

1. **`CardCopyButton` vs `AgentCopyButton`** — same 16×24 glyph, drawn 1px apart on both axes (`4.5,5.5`/`7.5,8.5` vs `3.5,4.5`/`6.5,7.5`; check path `4,12→7.5,15.5→15,7.5` vs `3,12→6.5,15.5→14,7.5`). Card version animates a 160ms morph and honours `SH_Widget_Animation_Duration` (reduced motion); Agent version hard-switches via bare `QTimer::singleShot` with **no** reduced-motion check.
2. **`CardDisclosureButton` vs `AgentDisclosureButton`** — `AgentDisclosureButton::paintEvent` re-inlines the body of `presentation::cardDisclosureIndicator()` verbatim instead of calling the existing shared symbol.
3. **Three more chevron geometries** — `ThreadDisclosureIndicator` (`rect().adjusted(3,3,-3,-3)`), `UiStyle::ChevronToolButton`, `TurnSettingsWidget::ChevronMenuButton` (near-identical copy of `ChevronToolButton` differing only in `QPushButton`/`SE_PushButtonContents`), `InspectorPane::InfoChoiceButton` (fixed 18px).
4. **`DiffViewer::ChevronComboBox` vs `TurnSettingsWidget::CompactComboBox`** — byte-identical except the class name.
5. **`makeLabel`** — six copies (`ShellWidget`, `ConversationCards`, `MiddleRegionWidget`, `ThreadPane`, `ComposerPane`, `InspectorPane`). Five identical; **`ComposerPane`'s omits `setTextInteractionFlags(Qt::TextSelectableByMouse)`**, so composer labels are silently unselectable while every other pane's are. A copy-paste divergence that is already a live behaviour bug.
6. **`text(std::string_view)`** ×8, **`text(const std::string&)`** ×3, **`utf8()`** ×3, **`clearLayout()`** ×2, **`textList()`** ×2.
7. **`UiStatus.h::displayStatus` vs `PresentationStatus.ts::displayStatus`** — C++ uses byte-wise `std::isupper`/`std::tolower` (ASCII, locale-dependent); TS uses Unicode `/u` regex and `toLocaleLowerCase`. For any non-ASCII status these produce different strings. Two frontends justify two implementations; they do not justify two *specifications*.

## Hidden state in dynamic properties

`ConversationView`, `ConversationItemModel`, `ConversationLoadingOverlay`, `CardCopyButton` and `ConversationCard` all carry test-observable state in `setProperty`: `conversationHoveredDisclosureKey` (functional — the delegate reads it via `parent()->property(...)` at paint time), `conversationDelegateDocumentTransfers`, `conversationDelegateDocumentReturns`, `conversationDelegateIncrementalAppends`, `conversationDelegateWidthRelayouts`, `conversationDelegateDocumentRebuilds`, `conversationHeightIndexUpdateSteps`, `conversationConstructionMicros`, `spinnerVisible`, `spinnerAnimationTick`, `copyIconState`. `conversationHoveredDisclosureKey` is not instrumentation — it is real hover state stored untyped on a `QObject`, read by a `const` paint path.

## Accessibility

`ConversationView` sets **no** accessible name, role or state for itself or any row. Rows exist only as model indices; a passive row has no `QAccessibleInterface` at all. `paintCard` receives `option.state |= State_Selected` (line 4002) and never reads it — selection is invisible in both renderers, while a focus rect is drawn separately. Screen-reader users get nothing from a passive conversation.

## What I have NOT covered

I stopped mid-sweep. Not audited: `ShellWidget.cpp` (3548), `ClientRuntime.cpp` (2828), `NodeGraphUiAdapter.cpp` (2250), `TurnSettingsWidget.cpp` (909), `DiffViewer.cpp` (925), `InspectorPane.cpp` beyond the button classes, `ThreadPane.cpp` beyond the delegate, `ComposerPane`, `MiddleRegionWidget`, the dialogs, `nodegraph/*`, and the whole `web/` tree. The end-to-end flow tracing, the full coverage ledger, the state/ownership map, the Qt lifetime audit, the LOC-delta estimates per remediation, and the ordered roadmap are not done.

**The audit is not complete.** What's above is sound and source-backed, but it's roughly the conversation subsystem plus a cross-cutting duplication scan — not the full Qt portion the brief asks for.

If you want, I can continue in a follow-up turn and produce the remaining sections as a proper markdown deliverable. The natural split is: (a) finish the file ledger + shell/adapter/protocol tracing, (b) the target architecture and deletion estimate, which needs (a) first.

One thing worth saying plainly about the remediation requirement: a clean target here is a single `CardPresentation` value type — one function `VisibleCardData → {header, blocks, palette, metrics}` — consumed by *both* a painter and a widget builder, with every metric coming from one struct and every colour from `UiStyle`. That deletes `passivePresentation()`, the delegate's parallel document cache, `takeMarkdownDocument`/`adoptMarkdownDocument`, the triple hit-test geometry, and the chevron/copy parity tests that exist only to keep two implementations aligned. My rough read is a net production reduction in the low four figures of lines, concentrated in `ConversationView.cpp`. I'd want to finish the sweep before committing to a number.

