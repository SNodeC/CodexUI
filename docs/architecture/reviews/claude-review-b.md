# External architecture review — Claude B

# CodexUI — Architectural and Code Review

**Repository:** `github.com/SNodeC/CodexUI`
**Revision reviewed:** `629660e8882bddc7473ee5f1aab90c755ee6edd4` (master HEAD, 553 commits)
**Method:** full clone, read-only inspection of source, build system, CI and tests. Nothing was built, executed, or visually verified. No runtime measurement below is mine.
**Perspective:** general software-engineering review of the system as it stands — not a commit archaeology.

---

## 1. Executive assessment

CodexUI is a genuinely ambitious piece of work with an unusually good *core* and an unusually poor *periphery*. The concurrency design, the protocol catalogue and the toolkit-neutral DTO boundary are better than most codebases of this size. Almost everything built on top of them has grown by accretion, and the growth is now the dominant engineering risk.

The three things that would most improve this codebase, in order:

1. **There is no test framework and no library target.** 20 ad-hoc test binaries, half with hand-rolled assertion helpers, 8 of which short-circuit and hide failures. Every test target recompiles production sources from scratch — and with *different preprocessor definitions* than production.
2. **God objects.** `ShellWidget::Impl` (~2 500 lines, ~46 methods, ~55 members) and `ConversationView` (4 041 lines) each own protocol routing, scheduling, policy, geometry, widget lifetime and test instrumentation simultaneously.
3. **The design-token layer is defined and then bypassed.** 61 named colour constants in `UiStyle.h`; 171 raw hex literals in the stylesheet that the same component generates, plus 35 more scattered across 7 files.

**Volume:** production C++ 37 822 LOC (`middle` 14 156, `codex` root 11 862, `nodegraph` 8 201, `ui` 3 603); tests 29 128; browser frontend 4 925 + 2 931 test. No `.ui`, `.qss` or `.qrc` files exist — all layout and styling is C++ string literals and imperative construction.

---

## 2. What is genuinely well engineered

Worth stating plainly, because the rest of this document is critical.

- **`nodegraph::NodeGraph`** (`NodeGraph.h`) — single-writer / many-reader over `std::shared_mutex`, with `tryRead()` returning `std::optional<ReadAccess>` so the GUI thread *never blocks* on the worker. Revision stamps are separated into `changedRevision`, `fieldChangedRevision`, `statusChangedRevision` and `structureChangedRevision`, so a UI scan can distinguish "text streamed" from "topology moved". That distinction is the difference between a UI that reflows constantly and one that doesn't. `WriteAccess::finish()` publishes one revision for a whole transaction and unlocks before returning. This is a well-thought-out design.
- **Retirement protocol** — removed nodes stay reachable in `retiredNodes_` until Qt acknowledges detachment through a typed mailbox, with `retiredOrderGeneration` so a bounded resume scan stays valid. This solves a real and subtle problem (UI holding `NodeRef`s across a removal) properly rather than with a lock.
- **`ProtocolCatalog.cpp`** — a compile-time table of 252 protocol methods with `static_assert` on per-direction and per-disposition counts and key uniqueness. A protocol method cannot be silently dropped or double-registered. This is exactly right.
- **`MiddleTypes.h`** — a clean, toolkit-neutral DTO layer with value semantics and defaulted `operator==`, sitting between graph and widgets. The comments explaining *why* (e.g. why `LocalPromptKey` is thread-id-independent, why `TurnSection::rootCardKey` exists) are high quality.
- **`ConversationItemModel`** uses an order-statistic treap so a history-prefix drop doesn't reindex the retained suffix. `ConversationHeightIndex` is the same idea for variable row heights. Both are the correct data structure for the job.
- **Nested-modal-loop safety.** `ShellWidget::Impl` holds a `std::shared_ptr<bool> alive` token captured by every deferred lambda, *in addition to* passing `owner` as the `QTimer::singleShot` context object. That looks redundant until you notice there are six `QDialog::exec()` / `QMessageBox::question()` nested event loops driven from `Impl` methods, during which the worker's `QSocketNotifier` keeps delivering graph changes. The token covers the window the context object doesn't. Someone thought carefully about this.
- **Comment quality throughout is well above average** — most non-obvious decisions carry a rationale.

---

## 3. Layer map and verdict

```
main.cpp
  └─ FrontendSession ........... worker thread + shared graph + 2 eventfd mailboxes   GOOD
       └─ ClientRuntime ........ SNode.C transport, protocol I/O (2 828 LOC)          MIXED
       └─ WorkerLogic .......... sole graph writer (1 548 LOC)                        GOOD
       └─ ProtocolUpdater ...... protocol → graph mutation (3 513 LOC)                MIXED
  └─ MainWindow → ShellWidget ... application shell (3 548 LOC)                       POOR
       └─ NodeGraphUiAdapter ... graph → DTO projection (2 250 LOC)                   MIXED
       └─ MiddleRegionWidget ... panel layout / splitters (603)                       OK
            ├─ ThreadPane ...... thread list + hierarchy (1 144)                      OK
            ├─ ConversationView. virtualized item view (4 041)                        POOR
            │    ├─ ConversationItemModel (1 308)                                     GOOD
            │    ├─ ConversationHeightIndex (266)                                      GOOD
            │    ├─ ConversationPresentation (362)                                     GOOD
            │    └─ ConversationCards (2 673)                                          POOR
            ├─ InspectorPane ... (1 274)                                              MIXED
            └─ ComposerPane .... (525)                                                OK
  └─ UiStyle .................... design tokens + global stylesheet (550)             POOR
web/ ........................... parallel TypeScript reimplementation (4 925)         RISK
```

The shape of the problem is visible in the map: the boundaries are correct, the *sizes* are wrong. `ConversationView` is larger than the entire protocol updater; `ShellWidget` is larger than the whole nodegraph library's core.

---

## 4. Architecture findings

### A-1 — `ShellWidget::Impl` is a god object — **High**

`src/codex/ShellWidget.cpp:1045`. One private struct, ~2 500 lines, ~46 member functions, ~55 data members. It simultaneously owns:

| Concern | Evidence |
|---|---|
| Protocol change routing | `handleGraphChanged` (149 lines), `routeNode`, `ThreadPaneRoute`, `ConversationRoute` |
| Optimistic creation lifecycle | `optimisticCreationThreadId`, `creationDraftCorrelation`, `nextCreationDraftSerial`, `creationInFlight`, `reconcileOptimisticCreation` |
| Frame scheduling / coalescing | `renderScheduled`, `graphBindingScheduled`, `paneCommitScheduled`, `draftSelectionScheduled`, `graphFallbackScheduled`, `pendingThreadPane`, `pendingConversation`, `pendingInspector`, `pendingChrome`, `pendingConversationHistoryPage`, `pendingConversationAuthorityReplacement` |
| Pane commit | `commitPendingPanes()` — **387 lines** |
| Chrome rendering | `render()` — **246 lines**, plus `ShellChromeValues` diffing |
| Dialog orchestration | `beginNewThreadDialog`, `beginForkThreadDialog`, `renameThreadDialog`, `confirmDeleteThread`, `chooseAttachments` |
| Pending-request policy | `pendingRequest`, `acceptPending`, `rejectPending`, `reviewPending`, `respondToFirstPending` |
| Catalog revision tracking | `modelCatalogRevision`, `permissionProfileCatalogRevision`, 4 associated booleans |
| Test instrumentation | `shellRenderCommits`, `threadPaneRoutes`, `conversationRoutes`, `inspectorRoutes` |

Eleven independent boolean "pending" flags are a hand-rolled dirty-region state machine with no invariant, no single transition point, and 2^11 nominal states. Nothing enforces which combinations are legal.

**Direction:** extract four collaborators that already exist implicitly — `GraphChangeRouter` (owns the route structs and `handleGraphChanged`), `PaneCommitScheduler` (owns every `*Scheduled`/`pending*` flag behind one `markDirty(Pane)` / `commit()` API), `ThreadLifecycleController` (optimistic creation, forks, rename, delete), and `ShellChrome` (the `render()` + `ShellChromeValues` diff). The shell then becomes wiring. Each extracted piece is independently testable, which none of them currently are.

### A-2 — `ConversationView` is a second god object — **High**

`src/codex/middle/ConversationView.h` declares 60+ private methods and 30+ members in one class. It owns: Qt item-view protocol, height index, materialization policy, a passive paint renderer, hit-testing, mouse-event synthesis and forwarding, scroll anchoring, follow/pause mode, per-thread scroll state, history windowing, staged snapshot commits, interactive-resize coalescing, turn-surface painting, card interaction state capture/restore, and a card factory. `appendTailCard()` alone is 248 lines.

Four of those are separable with no behaviour change and are the natural first cuts: the **passive renderer** (§5), the **scroll/anchor policy**, the **history window** (`historyWindows_`, `historyLimitForThread`, `requestNextHistoryPage` — pure bookkeeping, no Qt), and **interaction-state capture/restore**.

### A-3 — The design-token layer exists and is bypassed — **High**

`UiStyle.h` defines 61 `inline constexpr auto` colour tokens with a documented OKLCH derivation (`base .550/.090; hover .490/.080; …`). Then:

- `UiStyle.cpp::applicationStyleSheet()` — the *same component* — writes **171 raw hex literals** into a `R"QSS(...)"` string rather than interpolating the constants it just defined.
- 35 further hex literals appear across `ConversationView.cpp`, `ConversationCards.cpp`, `InspectorPane.cpp`, `MiddleRegionWidget.cpp`, `ShellWidget.cpp`, `DiffViewer.cpp`, `BrandMark.cpp`. Most duplicate an existing token: `#667085` (×8) is `secondary`, `#1d2633` (×8) is `primary`, `#98a2b3` (×6) is `placeholder`, `#f4f3fd`/`#cec7f6`/`#59507f` are the `purple*` ramp.
- At least one literal, `#6f98e8` (`ConversationView.cpp:3975`, active turn-surface border), matches **no** token at all.

Changing a token in the header today changes nothing on screen. The abstraction is pure cost.

**Direction:** generate the QSS from the tokens (`QStringLiteral(...).arg(...)` or a small `qss(tokenmap)` helper), and make the tokens the only permitted source of colour. A grep for `"#` outside `UiStyle.h` should return zero.

### A-4 — Status is protocol-typed all the way to the paint call — **Medium**

`UiStatus.h` already provides `StatusKind` and `classifyStatus()`. But `MiddleTypes.h` carries `std::string status` in `CommandExecutionData`, `AgentActivityData`, `FileChangesData`, `ImageGenerationData`, `GenericActivityData`, `PlanStepData` — raw protocol strings. Every consumer re-classifies: `presentation::statusLabel()` is called five separate times inside `passivePresentation()` alone, and again in the widget path, and again in `InspectorPane::statusLabel`. The classification of `"inProgress"` → `"running"` runs on every repaint of every visible card.

**Direction:** the adapter (`NodeGraphUiAdapter`) is the correct place to normalise. DTOs should carry `StatusKind` plus, where needed, the pre-rendered label. The raw string stops at the adapter boundary.

### A-5 — Redundant identity inside `VisibleCardData` — **Medium**

`MiddleTypes.h:182`:

```cpp
struct VisibleCardData {
  CardKey key;          // AuthoritativeItemKey{threadId, turnId, itemId}
  CardKind kind;
  std::string threadId; // ← same data
  std::string turnId;   // ← same data
  std::string itemId;   // ← same data
  ...
};
```

Two owners of the same three strings, with nothing keeping them in sync, plus three redundant string copies per card in a structure that is copied per frame. `CardKey` is a `variant` and the flat fields are the "works for all three alternatives" escape hatch — replace them with accessor functions on the variant.

### A-6 — Exceptions used as routine liveness control flow — **Medium**

`NodeGraph::ReadAccess::requireMember` throws `std::invalid_argument` for a non-member node (`NodeGraph.cpp:280`). `ShellWidget.cpp:363` and `:557` catch it *per graph change* to detect retirement:

```cpp
} catch (const std::invalid_argument &) {
  // A queued NodeRef may have been retired by a later graph transaction.
  route = {true, true, {}, true};   // conservatively refresh everything
}
```

The graph already exposes a non-throwing `contains()` probe for exactly this, and line 550 uses it — so the two call sites in the same file disagree about the idiom. Worse, the catch at :363 escalates to a *full refresh* of every pane, so a routine retirement race triggers the most expensive path available.

Meanwhile `NodeGraphUiAdapter.cpp` (2 250 lines, the other major graph reader) has **zero** `catch` blocks while making 49 `state()` calls. Most are on nodes obtained by traversal within the same read and are therefore safe, but the discipline is implicit rather than enforced.

**Direction:** pick one. Either `contains()`-guard everywhere and make the throw a genuine programming-error assert, or wrap the read in a non-throwing accessor returning `std::optional`.

### A-7 — The graph knows about the UI — **Low (deliberate, but worth naming)**

`Node::uiAttachment_` is an `std::atomic<void*>` slot on the domain object, documented as "only the Qt main thread may set, clear, or dereference it". It is an untyped back-pointer from the domain model into the presentation layer, and `ui/QtNodeAttachment.h` exists solely to give it meaning. It works and is carefully documented, but it inverts the dependency and makes `NodeGraph` non-reusable outside this UI. A side table keyed on `Node*` in the adapter would cost one hash lookup and restore the layering.

### A-8 — The entire presentation stack exists twice, and has already drifted — **High**

`docs/web-1.0-contract.md` states the browser frontend must "independently prove behavioral equality with the corresponding current C++ feature". That is a deliberate architectural choice and I am not arguing against having a web frontend. The problem is that the duplication is *unmanaged*: comparing `src/codex/middle/MiddleTypes.h` with `web/src/conversation/MiddleTypes.ts` at this commit shows the contract is already violated in the type definitions themselves:

| Type | C++ | TypeScript |
|---|---|---|
| `GenericActivityData` | `displayDetail: string` (bounded to 4 096 chars) | `raw: JsonObject` (unbounded) |
| `FileChangesData` | `status, changes, cwd` | `status, changes` — **`cwd` missing** |
| `LocalPromptData` | + `admittedAtMs`, `requiresExplicitRecovery` | neither |
| `TurnSection` | + `rootPinned` | missing |
| `VisibleCardData` | + `activeWork`, `target` | neither |

The `cwd` omission is functional: the C++ comment says `cwd` exists so relative provider paths can be resolved when the user opens a changed file. The browser cannot do that.

`displayStatus` diverges too. C++ (`UiStatus.h:44`) uses byte-wise `std::isupper`/`std::tolower` — ASCII-only, locale-dependent; TypeScript (`PresentationStatus.ts:22`) uses Unicode `/u` regexes and `toLocaleLowerCase()`. Any non-ASCII status string produces different output on the two frontends.

None of this is caught, because the "parity" tests (`web/tests/*-parity.test.mjs`) execute **only the TypeScript side** and assert against literals hand-copied from the C++ behaviour. See T-3.

**Direction:** the DTO schema and the pure presentation policies (status classification, plan/markdown rendering, key encoding, bounding rules) should be generated from one source, or at minimum checked by a differential test that runs both implementations over a shared corpus of fixtures.

---

## 5. The duplicate-renderer problem

This is the single largest concentration of defect risk, and it is architectural rather than cosmetic.

### R-1 — One visible card has two renderers and three geometries — **Critical**

`ConversationPassiveDelegate` (`ConversationView.cpp:336`) and `ConversationCard` (`ConversationCards.cpp:1595`) independently render the same `VisibleCardData`. `passivePresentation()` (`:236`) is a *complete second presentation projection* — titles, statuses, backgrounds, borders, title colours, block lists — parallel to `ConversationCard::Impl::createComposition`.

Because the two are independent, they disagree in source:

| Quantity | Passive delegate | Materialized card | Passive **hit test** |
|---|---|---|---|
| Corner radius | `drawRoundedRect(bounds, 10.0, 10.0)` | QSS `border-radius: 8px` (`UiStyle.cpp:252,256,258`) | — |
| Copy-icon X | `rect.right() − 43` | `right − 13 − 14 − 4 − 16 + 4.5 ⇒ right − 42.5` | `rect.right() − 52`, w=24 |
| Copy-icon Y | `rect.top() + 14.0` | header top `+ 5.5 ⇒ top + 16.5` | `top`, h=24 |
| Copy control width | implicit glyph | `CopyControlWidth = 16` | hardcoded **24** |
| Disclosure width | `DisclosureControlWidth = 14` | `= 14` ✓ | hardcoded **24** |
| Frame extent | `CardFrameExtent = 2` (constant) | `2 * card->frameWidth()` (queried) | — |
| Body origin | `rect.left() + 12` | viewport at `left + 13` | — |
| Body text width | `W − 28` | `viewport()->width() − 2` (= `W − 28`) ✓ | — |

The width constants were tuned to agree. Nothing makes the *origins* agree. The result is that every materialization shifts the whole Markdown surface horizontally, changes the corner radius 10→8, and moves the Copy glyph — with no semantic change whatsoever.

**The violated invariant:** *a row's pixels are a pure function of `(VisibleCardData, width, collapsed)`.* Two implementations of a pure function cannot be kept equal by construction, only by test — and the tests do not check pixels (T-2).

### R-2 — Materialization on mouse-press is a visible-jump generator — **High**

`ConversationView::mousePressEvent` (`:3770`) computes a passive hit, then calls `materializeRow(index.row(), true)`, replacing the delegate-measured height (`delegate->cardSize`) with a widget-measured one (`measureCard`), then:

```cpp
if (before != heights_.totalHeight()) {
  updateScrollRange();
  if (follow) setScrollValue(max); else restoreAnchor(anchor);
}
```

That conditional is the architecture conceding that the two measurements disagree. The compensation runs on **press**, before any drag — content moves under the cursor at the moment of clicking. `restoreAnchor` hides the scroll displacement; it cannot hide the row-height change.

### R-3 — A third renderer paints the turn surface — **Medium**

`ConversationView::paintEvent` (`:3931`) manually paints the continuous "You" turn surface behind rows, with its own rounded-rect, its own colours (`#eff5fe`, `#b7cff9`, `#6f98e8`) and its own `TurnSurfaceBottomPadding`. It duplicates what a QSS-styled container frame would do, and it is the reason `appendTailCard` has to hand-compute damage strips (`rowDamage.setLeft(0); … setBottom(bottom + TurnSurfaceBottomPadding + CardFrameExtent)`).

### R-4 — Selection state is computed and then discarded — **Medium**

`paintEvent` sets `option.state |= QStyle::State_Selected` (`:4002`) and `paintCard` never reads `option.state`. The materialized card doesn't render selection either. Selection is therefore invisible in both paths, while a *focus* rect is drawn separately from `currentIndex()`. Users cannot see what is selected.

### R-5 — Cross-renderer document transfer — **Medium**

`takeMarkdownDocument` / `adoptMarkdownDocument` (`ConversationView.cpp:488`, `:512`) move `QTextDocument`s between the delegate's 128-entry LRU cache and the widget. This exists purely to stop the duplication from being *slow*; it doesn't stop it from being *wrong*. It also adds a cache with a non-obvious invalidation contract: eviction is by `used` counter, validity is re-derived by comparing `{text, width, markdown, font}`, and a mismatch silently falls back to a full reparse.

**Direction for R-1…R-5:** collapse to one projection. Introduce a `CardPresentation` value type —

```cpp
struct CardPresentation {
  CardHeader header;           // title, status, control rects
  std::vector<CardBlock> body; // text, markdown?, font role
  CardPalette palette;         // resolved from UiStyle tokens only
  CardMetrics metrics;         // one source for every inset/radius/extent
};
CardPresentation presentationFor(const VisibleCardData&, int width, bool collapsed);
```

— consumed by both a painter and a widget builder. Every inset comes from `CardMetrics`; every colour from `UiStyle`. That deletes `passivePresentation()`, the delegate's parallel document cache, `takeMarkdownDocument`/`adoptMarkdownDocument`, the triple hit-test geometry, and the chevron/copy parity tests that exist only to keep two implementations aligned.

---

## 6. Qt-specific findings

### Q-1 — The codebase largely rejects Qt's signal/slot system — **Medium**

Only **4** classes declare `Q_OBJECT` (`GitDiffProvider`, `ExpandingPromptEditor`, `ConversationCards`' two, `ConversationItemModel`) and only 4 `signals:` blocks exist repository-wide. Against that: **39 `std::function` members in public headers** used as the primary notification mechanism (`ThreadPane::Actions`, `ConversationView::setLoadMoreAction/…`, `ThreadListWidget::toggleExpansion`, etc.).

This is a defensible taste (type-safe, no moc, no string-based lookup), but it forfeits what Qt gives for free: **automatic disconnection when the receiver is destroyed**. The codebase then re-implements that by hand, inconsistently — `QPointer` in 5 files, a `shared_ptr<bool> alive` token in `ShellWidget`, `QScopedValueRollback` reentrancy guards in `ConversationView`, and nothing at all in several callback assignments. Pick one idiom.

Note also that `ConversationView` is a `QAbstractItemView` subclass **without** `Q_OBJECT`. Consequences: `metaObject()->className()` reports `QAbstractItemView`, `qobject_cast<ConversationView*>` cannot work, the class cannot emit signals, and QSS class selectors cannot target it (it relies on `objectName` instead). For a class this central that is a surprising omission.

### Q-2 — Hidden state and test hooks in dynamic QObject properties — **Medium**

At least 15 dynamic properties are used as out-of-band channels:

- **Functional state:** `conversationHoveredDisclosureKey` — real hover state, written in `mouseMoveEvent`, read inside the `const` paint path via `parent()->property(...)`. Untyped, unchecked, and invisible to the type system.
- **Test instrumentation:** `conversationDelegateDocumentTransfers`, `…DocumentReturns`, `…IncrementalAppends`, `…WidthRelayouts`, `…DocumentRebuilds`, `conversationHeightIndexUpdateSteps`, `conversationCardConstructions`, `conversationRowsMaterialized`, `conversationRowsReleased`, `conversationConstructionMicros`, `conversationLocalGeometryPasses`, `spinnerVisible`, `spinnerAnimationTick`, `copyIconState`, `graphRefreshPasses`, `targetedStructuralAppends`.

Production code pays for counters that exist only so tests can observe internals. That is a symptom, not the disease: these classes are untestable through their public API, so the tests reach through properties instead. Fixing A-2 removes most of the need.

### Q-3 — Manual event synthesis and forwarding — **Medium**

`ConversationView::mousePressEvent` constructs a `QMouseEvent` and `QApplication::sendEvent()`s it to a child widget at a translated position, with `forwardingMouseEvent_`, `forwardedButtonAction_`, `forwardedButtonViewportRect_`, `forwardedMouseTarget_`, `forwardedMouseViewportOrigin_`, `forwardedMouseLocalOrigin_` as supporting state, plus a reentrancy guard because "a synthetic event ignored by a card child can propagate back through the viewport". It also `installEventFilter(this)` on **every descendant of every materialized card**.

This entire mechanism exists only because a passive row has no widget to receive the press. It disappears with R-1's remediation if rows are always widgets within the overscan window, or stays but becomes trivial if passive rows are never interactive.

### Q-4 — Destructor-time allocation workaround — **Low**

`MarkdownTextView::~MarkdownTextView` (`ConversationCards.cpp:1016`):

```cpp
setDocument(new QTextDocument(this));  // in the destructor
document_.reset();
```

Allocating and parenting a `QTextDocument` to an object that is being destroyed, to work around `QTextEdit`'s base-destructor ordering. It is correctly reasoned and correctly commented, but it is the kind of workaround that should be a comment on a `QTextDocument` owned by the view, not a shared_ptr fought against at teardown.

### Q-5 — No accessibility surface on the conversation — **High**

`ConversationView` sets no `accessibleName`, no role, no state for itself or any row. Passive rows have no `QAccessibleInterface` at all — they are paint output, not objects. `ConversationItemModel` exposes identity/structure roles but the view's `QAccessibleTable` interface will report rows that don't correspond to anything a user can navigate. A screen-reader user gets essentially nothing from a conversation. Combined with R-4 (invisible selection) this is the weakest area of the product.

### Q-6 — Six copies of `makeLabel`, one of which behaves differently — **Medium**

`ShellWidget.cpp:977`, `ConversationCards.cpp:506`, `MiddleRegionWidget.cpp:119`, `ThreadPane.cpp:220`, `InspectorPane.cpp:136`, `ComposerPane.cpp:40`. Five are byte-identical. **`ComposerPane`'s omits `setTextInteractionFlags(Qt::TextSelectableByMouse)`** — so composer labels cannot be selected while every other pane's can. A live behaviour bug produced by copy-paste divergence, which is precisely the failure mode duplication is supposed to warn about.

Same pattern, smaller stakes: `text(std::string_view)` ×8, `text(const std::string&)` ×3, `utf8()` ×3, `clearLayout()` ×2, `textList()` ×2.

### Q-7 — Chevron and copy-glyph implementations multiply — **Medium**

`UiStyle::drawChevron` is correctly shared. The *callers* are not:

- `UiStyle::ChevronToolButton` and `TurnSettingsWidget::ChevronMenuButton` are the same class modulo `QToolButton`/`QPushButton` and `SE_ToolButtonLayoutItem`/`SE_PushButtonContents`.
- `DiffViewer::ChevronComboBox` and `TurnSettingsWidget::CompactComboBox` are byte-identical except the class name.
- `InspectorPane::AgentDisclosureButton::paintEvent` re-inlines the body of `presentation::cardDisclosureIndicator()` verbatim instead of calling the existing shared symbol.
- `ThreadDisclosureIndicator` uses a fourth geometry (`rect().adjusted(3,3,-3,-3)`).
- `CardCopyButton` and `AgentCopyButton` paint the same 16×24 glyph **1 px apart on both axes** (`4.5,5.5`/`7.5,8.5` vs `3.5,4.5`/`6.5,7.5`; check path `4,12→7.5,15.5→15,7.5` vs `3,12→6.5,15.5→14,7.5`). `CardCopyButton` animates a 160 ms morph and honours `SH_Widget_Animation_Duration` (reduced motion); `AgentCopyButton` hard-switches via a bare `QTimer::singleShot` with **no** reduced-motion check.

**Direction:** one `ChevronDecoration` mixin/helper taking a `subElementRect` provider, and one `CopyButton` used by both card and inspector.

---

## 7. Build system findings

### B-1 — No library target; every test recompiles production sources — **High**

`CMakeLists.txt` is 627 lines and contains exactly one library (`codexui-nodegraph`, 19 lines in a subdirectory) and **14** executables, each re-listing production `.cpp` files. `UiStyle.cpp` is listed **9 times**; `ConversationCards.cpp` **6 times**. Consequences:

- compile time multiplied 6–9× on the heaviest translation units;
- adding a production source requires editing N target lists by hand, and the failure mode is a link error at best;
- targets can and do diverge in how they compile the same file (B-2).

**Direction:** three static libraries — `codexui-nodegraph` (exists), `codexui-middle` (DTOs, model, presentation, height index), `codexui-ui` (widgets, adapter, style) — linked by both `codex-ui` and every test. This is mechanical, low-risk, and removes several hundred lines of CMake.

### B-2 — Transport code is compiled out of every test — **High**

`CODEXUI_CODEX_FRONTEND_TLS`, `…_RFCOMM` and `…_WEBSOCKET` are defined **only on the `codex-ui` target** (`CMakeLists.txt:150,157,164`). `ClientRuntime.cpp` guards ~20 preprocessor regions on them (`:15,26,30,34,1324,1340,1354,1382,1465,1469,1485,1490,2566,2584,2608,2610,2635,2752,2756,2760`). The only test that compiles `ClientRuntime.cpp` — `codexui-client-runtime-dispatch-test` (`:305`) — does **not** get those definitions.

**Every TLS, RFCOMM and WebSocket transport path is compiled to nothing in the test build.** They are not merely untested; they are not even syntax-checked by CI in the configuration CI builds them. A compile error inside one of those blocks would only surface in the production target — which CI does build, so that much is caught — but no behavioural coverage exists at all.

### B-3 — No warning configuration — **Medium**

There is no `-Wall`, `-Wextra`, `-Wpedantic`, `-Werror`, no `CMAKE_CXX_FLAGS` customisation, no sanitizer option anywhere in the build. For a 38 kLOC C++20 codebase doing manual pointer lifetime management, raw `void*` slots, and pixel arithmetic, that is a significant unused safety net. `-Wall -Wextra` plus an ASan/UBSan CI job would likely find real issues in the event-forwarding and card-release paths.

### B-4 — Single-platform, single-configuration CI — **Medium**

`.github/workflows/ci.yml` runs one C++ job: `ubuntu-24.04`, `gcc:15.3.0-trixie` container, Qt6, **Debug only**, `QT_QPA_PLATFORM=offscreen`. No Release build, no Clang, no Windows/macOS, no Wayland or XWayland, no HiDPI or fractional-scaling run, no sanitizers. For a desktop Qt application whose hardest problems are pixel geometry and event handling, offscreen-Debug-on-one-platform is the weakest possible signal. The `Check changed lines` step runs `git diff --check` — whitespace only.

Credit where due: the CI pins the paired AISuite revision by SHA and *verifies* it (`test "$(git -C … rev-parse HEAD)" = "$(cat web/AISUITE_REVISION)"`), and asserts the checked-out CodexUI SHA matches the event SHA. That is better dependency hygiene than most projects.

---

## 8. Test findings

### T-1 — There is no test framework — **High**

Twenty test executables, zero use of QtTest, GoogleTest, Catch2 or doctest. Ten define their own `bool expect(bool, const char*)` that prints to `std::cerr`; the rest use bare returns or `assert`. There is no test registry, no per-case name in CTest output, no XML report, no way to run a single case (except one ad-hoc `CODEXUI_REMAINING_UI_TESTS` env var in `ConversationVirtualizationTest.cpp`), no fixtures, no parameterisation.

`tests/codex/ConversationCardsTest.cpp` is **7 855 lines** — the largest file in the repository, larger than any production file — with no structural framework holding it together.

### T-2 — Eight test binaries short-circuit and hide failures — **Critical**

`ConversationVirtualizationTest.cpp:2858` is representative:

```cpp
const bool result = viewportProportionalFoundation() &&
                    exactStructuralRowsPreserveTheViewport() &&
                    ... 28 suites total ...;
```

`&&` short-circuits. The first failure suppresses every subsequent suite in the binary. Affected: `ClientRuntimeDispatchTest`, `ConversationCardsTest`, `ConversationVirtualizationTest`, `GitChangesLiveTest`, `NodeGraphConversationUiTest`, `NodeGraphUiAdapterTest`, `ShellIntegrationTest`, `nodegraph/NodeGraphTest`.

A developer fixing one failure has no idea whether they are one failure from green or twenty. Note the same file's `CODEXUI_REMAINING_UI_TESTS` branch uses `&=` and does *not* short-circuit — the correct behaviour exists in the file and is not used by default.

**This is a one-line-per-file fix with a large payoff** (`result = a(); result &= b(); …`) and should be done before anything else in this document.

### T-3 — "Parity" tests do not compare implementations — **High**

`web/tests/browser-session-parity.test.mjs`, `conversation-projection-parity.test.mjs`, `model-parity.test.mjs`, `normalizer-parity.test.mjs`, `supporting-surfaces-parity.test.mjs`, `viewport-parity.test.mjs` all execute **only TypeScript** and assert against literals transcribed from C++ behaviour. A C++-side change cannot fail them. This is why the DTO drift in A-8 is live in master.

**Direction:** emit a shared JSON fixture corpus, run both implementations over it (a small C++ CLI harness + the existing node test runner), and diff. That converts "parity" from a naming convention into a property.

### T-4 — Pixel tests are DPR-1-only, silently — **Medium**

`ConversationVirtualizationTest.cpp` compares `view.viewport()->grab().toImage()` (device pixels at the screen's DPR) against `paintedWidgetBounds()`, which renders into a `QImage` constructed with **no `devicePixelRatio`** (logical pixels). At DPR ≠ 1 these are different coordinate spaces and the comparison is meaningless. The hardcoded search window `QRect(row.right() − 26, row.top() + 4, 24, 32)` compounds it. Since CI only runs offscreen at DPR 1, this never surfaces.

### T-5 — The one test guarding the core defect checks only row height — **High**

`passiveAndInteractivePresentationShareExactGeometry()` compares `view.visualRect(index).height()` before and after materialization. It cannot detect the horizontal body shift, the 10→8 radius change, the Copy-glyph move, or any reflow that happens to preserve total height. Its fixture is 100 repeated `'W'` characters — no links, lists, code fences, tables, CJK, RTL, or emoji — at one width (620) and one DPR.

A passing test is evidence only for what it observes; this one observes one integer.

### T-6 — The benchmark is built but never run — **Medium**

`codexui-conversation-view-benchmark` is defined at `CMakeLists.txt:445` and never passed to `add_test`. There is no performance gate of any kind in CI, despite virtualization being the subsystem's entire justification and `docs/qt-virtualized-conversation-view.md` citing benchmark numbers as the baseline.

### T-7 — Timeouts as flakiness management — **Low/Medium**

Every GUI test carries a `set_tests_properties(... TIMEOUT n)` ranging 10–35 s, and the test helpers use `settle(int passes = 4)` / `waitUntil(pred, 500)` loops. Wall-clock settling in an offscreen Debug build is inherently machine-speed-dependent. This will get flaky under load; raising timeouts is not the fix.

---

## 9. Prioritized recommendations

Ordered by (value ÷ risk), highest first. The first three are nearly free.

| # | Action | Effort | Risk | Payoff |
|---|---|---|---|---|
| 1 | Replace `&&` with `&=` in the 8 short-circuiting test mains (T-2) | minutes | none | true failure visibility |
| 2 | Add `-Wall -Wextra` (then `-Werror` once clean); add an ASan/UBSan CI job (B-3) | hours | low | real defects surfaced |
| 3 | Register the benchmark with a coarse regression bound (T-6) | hours | none | virtualization stops silently regressing |
| 4 | Extract `codexui-middle` and `codexui-ui` static libraries; add the transport defines to test targets (B-1, B-2) | 1–2 days | low | 6–9× less recompilation, transports compiled in tests |
| 5 | Generate the QSS from `UiStyle.h` tokens; ban raw hex outside `UiStyle.h` (A-3) | 1–2 days | low | the token layer starts working |
| 6 | Adopt one test framework (QtTest is the least friction for the GUI suites) and split `ConversationCardsTest.cpp` (T-1) | 1 week | low | named cases, isolation, per-case CTest entries |
| 7 | Collapse to one `CardPresentation` projection; delete the passive renderer's parallel projection, document cache and hit geometry (R-1…R-5) | 2–3 weeks | **high** | eliminates the defect class; large net LOC reduction in `ConversationView.cpp` |
| 8 | Extract `GraphChangeRouter`, `PaneCommitScheduler`, `ThreadLifecycleController`, `ShellChrome` from `ShellWidget::Impl` (A-1) | 2–3 weeks | medium | testability, comprehensibility |
| 9 | Differential parity harness over a shared fixture corpus (T-3, A-8) | 1 week | low | the web contract becomes enforceable |
| 10 | Accessibility pass on `ConversationView`: names, roles, visible selection (Q-5, R-4) | 1 week | low | the product becomes usable with a screen reader |

Do **7 before 8**. `ConversationView`'s size is largely caused by the duplication; shrinking it first makes the shell extraction easier to reason about.

A caution on 7: it is the highest-risk item here. The passive renderer exists for a real reason (widget count per row was ~13 descendants; see the doc's own baseline table), so the target is not "delete the delegate" — it is "make the delegate and the widget consume the same projection and the same metric struct". Any design that adds a third abstraction while keeping both existing implementations should be rejected.

---

## 10. Coverage and limits

**Read in full or substantially:** `main.cpp`, `MainWindow`, `ShellWidget.h` + structural survey of `ShellWidget.cpp`, `FrontendSession.h`, `ClientRuntime.h` + macro survey, `UiStatus.h`, `nodegraph/NodeGraph.h`, `nodegraph/Value.h`, `nodegraph/ProtocolCatalog.cpp` (asserts), `middle/MiddleTypes.h/.cpp`, `middle/ConversationPresentation.h/.cpp`, `middle/ConversationCards.h` + key regions of `.cpp`, `middle/ConversationView.h` + key regions of `.cpp`, `middle/ConversationItemModel.h`, `middle/InspectorPane.cpp` (controls), `middle/ThreadPane.cpp` (delegate/list), `ui/UiStyle.h/.cpp`, `TurnSettingsWidget.cpp` (controls), `DiffViewer.cpp` (controls), `web/src/conversation/MiddleTypes.ts`, `web/src/presentation/PresentationStatus.ts`, `CMakeLists.txt`, `nodegraph/CMakeLists.txt`, `.github/workflows/ci.yml`, `docs/qt-virtualized-conversation-view.md`, `docs/web-1.0-contract.md`, test harness structure across all 20 test files.

**Not reviewed in depth:** `ProtocolUpdater.cpp` (3 513), `ClientRuntime.cpp` body (2 828), `NodeGraphUiAdapter.cpp` body (2 250), `WorkerLogic.cpp` (1 548), `ShellWidget.cpp` body (3 548), `ComposerPane`, `MiddleRegionWidget`, `GitDiffProvider`, `PendingRequestPolicy`, the four dialogs, `SpscQueue`/`EventFd`/`ThreadChannels` correctness, the React application (`web/src/app/*`), and the `ui-review/` screenshot inventory.

**Not possible from source:** anything about runtime performance, actual rendered pixels, platform behaviour differences, or memory use. Every figure in this document is a static count or a source-level derivation.

