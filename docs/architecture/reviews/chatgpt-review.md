# External architecture review — ChatGPT

# 1. Executive summary

I completed a read-only, GitHub-grounded audit of the Qt/native UI architecture in `SNodeC/CodexUI` at:

**`629660e8882bddc7473ee5f1aab90c755ee6edd4`**

`master` had **not advanced** from the expected commit when I began.

The central conclusion is clear:

> The current conversation virtualization architecture has crossed an unhealthy boundary: a performance optimization introduced a second implementation of the same visual component, and subsequent commits have progressively added machinery to keep the two implementations synchronized.

The known Markdown movement is therefore not fundamentally a Markdown bug. It is one manifestation of a broader architectural duplication between:

* `ConversationPassiveDelegate`
* passive `QTextDocument` rendering
* passive manual sizing
* passive control painting/hit-testing

and:

* `ConversationCard`
* `MarkdownTextView`
* QWidget/layout sizing
* actual copy/disclosure controls.

The current architecture attempts to preserve visual equivalence through shared metrics, document transfer, explicit height reconciliation, interaction-state extraction/restoration, staged materialization, synthetic input replay and geometry-settlement logic. Those mechanisms reduce symptoms, but they cannot establish the stronger invariant that a visible card has one authoritative visual identity.

I found **no evidence of a comparably systemic QObject ownership/threading failure**. In fact, several lower-level architectural boundaries are good:

* `NodeGraph` remains the authoritative application state.
* `NodeGraphUiAdapter` provides a deliberate read/project/release boundary before QWidget work.
* `ConversationItemModel` is a proper Qt projection rather than the state authority.
* `ConversationHeightIndex` is an appropriate virtualization primitive.
* native Qt and browser React renderers are legitimately separate frontends.

The highest-value remediation is therefore **not a larger abstraction layer**. It is deletion:

> Keep the virtualized row/model/height infrastructure, but make one visual implementation authoritative for visible conversation cards.

For this UI, the cleanest direction is a **bounded pool of real `ConversationCard` widgets for visible + overscan rows**, with offscreen rows represented only by model/state plus cached scalar heights. The delegate may still paint selection/background decoration, but should no longer implement the card itself.

That change should permit deletion of a substantial body of passive presentation, Markdown-cache, hit-testing, transfer, parity and synthetic-interaction code.

---

# 2. Commit audited

Repository:

`SNodeC/CodexUI`

Branch:

`master`

Audited commit:

**`629660e8882bddc7473ee5f1aab90c755ee6edd4`**

Git tree:

**`e14c16d2e01b84c7841dbbbb2cdbe59967270414`**

The recursive tree response reported `truncated:false`, so the repository inventory used for this audit was complete at that SHA.

The developer's uncommitted local changes listed in the prompt are **not represented in this audit** and may already change some findings.

---

# 3. Access limitations

This review used only committed GitHub evidence:

* repository tree;
* source files;
* headers;
* CMake files;
* committed documentation;
* Git history and commit diffs;
* GitHub Actions configuration and results.

I did **not** have access to:

* the developer's working tree;
* uncommitted changes;
* local build products;
* installed CodexUI executables;
* running processes;
* developer screenshots;
* developer logs.

I attempted to establish an independent ephemeral checkout solely from the pinned GitHub revision, but the execution environment could not resolve `github.com`. I therefore did **not** clone, compile, execute or visually inspect CodexUI locally.

Accordingly, statements about runtime effects below are source-derived risks or consequences, not claims of runtime reproduction.

There was also no subagent facility available in this session. I could not literally send the identical prompt to two isolated reviewer agents. Instead I performed two deliberately separate evidence passes:

1. rendering/state/model/architecture/history;
2. lifetime/events/focus/accessibility/tests/CI/usability.

I reconciled them only afterward.

---

# 4. Audit method

The review followed five evidence layers.

First, I pinned `master` and used the complete Git tree plus CMake manifests to construct the inventory.

Second, I traced the architecture from protocol reception through:

`ClientRuntime / FrontendSession`
→ `ProtocolUpdater / WorkerLogic`
→ `NodeGraph`
→ `NodeGraphUiAdapter`
→ pane-specific projection/models
→ widgets/delegates
→ layouts/rendering/input
→ outgoing commands
→ optimistic state
→ graph reconciliation.

Third, I examined the conversation virtualization implementation particularly deeply, including the passive and materialized code paths, sizing, document ownership, event forwarding, focus preservation, scroll state and staging.

Fourth, I examined the required historical commits to determine why the current mechanisms exist rather than judging their shape in isolation.

Finally, I inspected native tests, the conversation benchmark and GitHub Actions to determine what passing CI actually proves.

---

# 5. Complete Qt file coverage ledger

The complete GitHub tree contains **83 production files in the native C++/Qt and closely coupled nodegraph layers**, plus **20 native/nodegraph test files**, relevant design documentation, build/workflow files and the browser-side contract files necessary for frontend parity analysis.

To keep the ledger readable, paired `.h/.cpp` files are one row; both files were counted independently in the inventory.

| Path / pair                         | Primary responsibility               | UI/state concepts                                          | Rendering / interaction                        | Ownership role                     | Audit      |
| ----------------------------------- | ------------------------------------ | ---------------------------------------------------------- | ---------------------------------------------- | ---------------------------------- | ---------- |
| `src/codex/main.cpp`                | application bootstrap                | app/session/window                                         | QApplication                                   | root lifetime                      | reviewed   |
| `MainWindow.*`                      | top-level window                     | shell                                                      | QWidget                                        | owns shell                         | deep       |
| `ShellWidget.*`                     | application UI coordinator           | threads, active thread, protocol requests, pending dialogs | QWidget/signals                                | principal UI coordinator           | deep       |
| `ClientRuntime.*`                   | app-server runtime/protocol dispatch | connection/request lifecycle                               | none                                           | runtime authority below UI         | structural |
| `FrontendSession.*`                 | Qt/main-thread ↔ worker bridge       | mailbox, correlations, graph updates                       | notifier/timers/signals                        | owns worker/thread bridge          | deep       |
| `WorkerMailboxReceiver.*`           | mailbox dispatch                     | worker messages                                            | event-driven                                   | Qt receiver                        | structural |
| `Configuration.*`                   | configuration                        | runtime/settings                                           | none                                           | value/config                       | structural |
| `ConnectionDialog.*`                | connection UI                        | endpoint/session settings                                  | dialog                                         | transient child                    | structural |
| `NewThreadDialog.*`                 | thread creation                      | creation options                                           | dialog                                         | transient child                    | structural |
| `FileSelectionDialog.*`             | attachments/files                    | selection                                                  | dialog                                         | transient child                    | structural |
| `PendingRequestDialog.*`            | approvals/user input                 | request response                                           | dialog                                         | transient child                    | structural |
| `PendingRequestPolicy.*`            | request policy                       | approval/input state                                       | none                                           | policy                             | structural |
| `TurnSettingsWidget.*`              | model/reasoning controls             | turn configuration                                         | QWidget                                        | shell/composer child               | structural |
| `DiffViewer.*`                      | diff presentation                    | file changes                                               | custom QWidget/text                            | child                              | structural |
| `GitDiffProvider.*`                 | diff acquisition                     | repository state                                           | none                                           | service                            | structural |
| `AttachmentDraft.h`                 | attachment state                     | draft                                                      | none                                           | value type                         | reviewed   |
| `ForkNaming.h`                      | fork naming                          | thread workflow                                            | none                                           | policy/value                       | reviewed   |
| `CurrentProtocolAdapters.h`         | protocol adaptation                  | app-server messages                                        | none                                           | translation                        | reviewed   |
| `NodeGraphJson.*`                   | graph serialization                  | graph                                                      | none                                           | utility                            | structural |
| `UiStatus.h`                        | native status text/policy            | status/phase/lifecycle                                     | formatting                                     | presentation policy                | deep       |
| `middle/MiddleTypes.*`              | conversation DTOs/types              | cards, sections, local prompt lifecycle                    | none                                           | presentation data contract         | deep       |
| `middle/ConversationPresentation.*` | shared presentation metrics          | card header geometry                                       | constants/metrics                              | cross-renderer parity              | deep       |
| `middle/ConversationCards.*`        | interactive/live cards               | all conversation card kinds                                | QWidget/QTextBrowser/QTextEdit/custom controls | materialized visual implementation | very deep  |
| `middle/ConversationView.*`         | virtualization                       | scrolling, rows, staging, passive/live transition          | QAbstractItemView/delegate/widgets             | geometry/materialization owner     | very deep  |
| `middle/ConversationItemModel.*`    | Qt row model                         | card projection                                            | QAbstractListModel                             | projection/index owner             | very deep  |
| `middle/ConversationHeightIndex.*`  | virtual height index                 | row positions/heights                                      | none                                           | geometry index                     | deep       |
| `middle/ThreadPane.*`               | thread list/hierarchy                | hierarchy, sort, timestamp, optimistic threads             | QListView/delegate                             | pane state                         | deep       |
| `middle/InspectorPane.*`            | inspector                            | agent/protocol/detail expansion                            | widgets/QTextDocument→HTML                     | pane-local presentation            | deep       |
| `middle/ComposerPane.*`             | prompt/composer                      | send/stop/attach/steering                                  | widgets/editor                                 | input state                        | deep       |
| `middle/MiddleRegionWidget.*`       | workspace/panel geometry             | splitters/narrow layout                                    | QWidget/splitters                              | geometry owner                     | deep       |
| `ui/NodeGraphUiAdapter.*`           | graph→UI DTO seam                    | threads/conversation/inspector snapshots                   | none                                           | projection boundary                | deep       |
| `ui/UiViewState.h`                  | renderer-local UI DTOs               | expansion/optimism/state                                   | none                                           | UI-state schema                    | deep       |
| `ui/QtNodeAttachment.h`             | graph/Qt attachment lifetime         | NodeRef binding                                            | none                                           | lifetime helper                    | deep       |
| `ui/UiStyle.*`                      | native styling                       | palette/metrics/dynamic state                              | QPalette/QSS-like code                         | style authority                    | deep       |
| `ui/ExpandingPromptEditor.*`        | composer editor                      | expanding text input                                       | QTextEdit                                      | widget                             | deep       |
| `ui/BrandMark.*`                    | brand presentation                   | logo                                                       | custom paint                                   | simple widget                      | structural |
| `nodegraph/NodeGraph.*`             | canonical graph                      | threads/turns/items                                        | none                                           | authoritative application state    | deep       |
| `nodegraph/ProtocolUpdater.*`       | protocol→graph reconciliation        | streaming/lifecycle                                        | none                                           | graph writer                       | deep       |
| `nodegraph/WorkerLogic.*`           | worker-side request/protocol logic   | protocol lifecycle                                         | none                                           | worker state                       | deep       |
| `nodegraph/ThreadChannels.*`        | per-thread channels                  | request routing                                            | none                                           | concurrency primitive              | structural |
| `nodegraph/EventFd.*`               | wakeup                               | queue notification                                         | fd                                             | low-level lifetime                 | structural |
| `nodegraph/Messages.h`              | worker/UI messages                   | request/update DTOs                                        | none                                           | transport values                   | reviewed   |
| `nodegraph/PromptText.*`            | prompt normalization                 | prompt                                                     | none                                           | value/policy                       | reviewed   |
| `nodegraph/ProtocolCatalog.*`       | protocol metadata                    | method/type catalog                                        | none                                           | static authority                   | structural |
| `nodegraph/Value.*`                 | protocol values                      | JSON-like data                                             | none                                           | value                              | structural |
| `nodegraph/SpscQueue.h`             | interthread queue                    | mailbox                                                    | none                                           | concurrency primitive              | structural |

Native tests reviewed/covered:

* `ClientRuntimeDispatchTest.cpp`
* `ConversationCardsTest.cpp`
* `ConversationItemModelTest.cpp`
* `ConversationViewBenchmark.cpp`
* `ConversationVirtualizationTest.cpp`
* `EstablishedUiUxTest.cpp`
* `GitChangesLiveTest.cpp`
* `NodeGraphConversationUiTest.cpp`
* `NodeGraphInspectorUiTest.cpp`
* `NodeGraphThreadPaneUiTest.cpp`
* `NodeGraphUiAdapterTest.cpp`
* `PendingRequestPolicyTest.cpp`
* `ShellIntegrationTest.cpp`
* `CurrentProtocolAdaptersTest.cpp`
* `GraphConcurrencyTest.cpp`
* `NodeGraphJsonTest.cpp`
* `NodeGraphTest.cpp`
* `ProtocolUpdaterTest.cpp`
* `ThreadChannelsTest.cpp`
* `WorkerLogicTest.cpp`

Architecture/contract documentation covered:

* `design/ux-decisions/thread-turn-model.md`
* `docs/app-server-protocol/master-data-model.md`
* `docs/codex-architecture.md`
* `docs/native-ui-ux-qualification-inventory.md`
* `docs/qt-virtualized-conversation-view.md`
* `docs/two-thread-shared-node-graph.md`
* `docs/ui-behavior.md`
* `docs/ui-ux-internal-api.md`
* `docs/web-1.0-contract.md`
* `docs/web-qualification.md`
* `docs/web-release.md`

Build/CI boundary covered:

* root `CMakeLists.txt`
* `src/codex/nodegraph/CMakeLists.txt`
* `tests/nodegraph/CMakeLists.txt`
* `web/CMakeLists.txt`
* `.github/workflows/ci.yml`

There are **no Qt Designer `.ui` files** and no standalone native `.qss` file at the audited tree. Native styling is programmatic.

Browser files considered where they define shared concepts or duplication:

* `web/src/app/App.tsx`
* `BrowserFrontendSession.ts`
* `BrowserStorage.ts`
* `ComposerKeyboard.ts`
* `ConversationViewportState.ts`
* `Humanize.ts`
* `PendingRequestResponses.ts`
* `TurnSettingsOptions.ts`
* `conversation/ConversationProjection.ts`
* `MiddleTypes.ts`
* `PromptCoordinator.ts`
* `presentation/PresentationModel.ts`
* `PresentationProtocol.ts`
* `PresentationStatus.ts`
* `ProtocolNormalizer.ts`
* `styles.css`

---

# 6. UI subsystem map

The native architecture is approximately:

```text
app-server
    │
ClientRuntime
    │
FrontendSession / worker mailbox
    │
WorkerLogic + ProtocolUpdater
    │
NodeGraph  ← canonical durable/live state
    │
NodeGraphUiAdapter
    ├──────────────┬───────────────────┐
 ThreadPane   ConversationItemModel   InspectorPane
                    │
             ConversationView
              │             │
      passive delegate   ConversationCard
                          │
             MarkdownTextView / CommandOutputView
```

Application composition:

```text
MainWindow
  └─ ShellWidget
       └─ MiddleRegionWidget
            ├─ ThreadPane
            ├─ ConversationView
            │    + ComposerPane region
            └─ InspectorPane
```

This general decomposition is sound until the final conversation rendering boundary.

---

# 7. State and ownership map

## Authoritative application state

`NodeGraph` is the principal authority for:

* threads;
* turns;
* conversation items;
* protocol lifecycle;
* agent activity;
* request-derived state.

That is a good architectural decision.

## Projection state

`NodeGraphUiAdapter` converts graph state into toolkit-facing DTOs and deliberately avoids holding the graph lock while widgets are updated.

That separation should be preserved.

## Qt model state

`ConversationItemModel` owns:

* row order;
* stable row references;
* role projection;
* target indexing.

It does not try to become the authoritative business model.

Again, good.

## View-local state

`ConversationView` owns substantially more:

* per-thread scroll position;
* anchor preservation;
* materialized card set;
* staged cards;
* cached heights;
* section ranges;
* collapse state;
* command-output scroll/follow-tail state;
* text selection;
* focus restoration;
* child-control state;
* passive-document transfer;
* current-index interaction;
* history paging state;
* animation scheduling.

Some of that is legitimately view state. The problem is that much of it exists only because visual object identity changes between passive and materialized states.

---

# 8. End-to-end behavior flows

## Protocol update

```text
app-server event
→ ClientRuntime
→ FrontendSession mailbox
→ worker/ProtocolUpdater
→ NodeGraph mutation
→ UI notification
→ ShellWidget routing
→ NodeGraphUiAdapter snapshot
→ ConversationItemModel / ThreadPane / InspectorPane
→ layout / paint
```

## Prompt/send

```text
ComposerPane input
→ ShellWidget/FrontendSession request
→ local optimistic LocalPrompt
→ ConversationItemModel
→ ConversationView
→ visible optimistic card
→ server acceptance/update
→ NodeGraph reconciliation
→ LocalPrompt promoted to authoritative UserMessage
```

The in-place `LocalPrompt → UserMessage` promotion is one of the better parts of the design because it avoids unnecessarily replacing a visible card.

## Steering

The same input path is reused with turn/thread context and reconciled against graph updates.

## Thread creation / ephemeral / forks

UI request state is initiated in shell/dialog/thread-pane code, represented optimistically where necessary, then replaced/reconciled when authoritative thread state enters `NodeGraph`.

The distinction between renderer-local optimistic state and graph-authoritative state is intentional and generally defensible.

## Commands

```text
protocol command item
→ graph node
→ conversation projection
→ Command card
→ CommandOutputView
→ streaming append
→ follow-tail unless user has manually detached
```

## History paging

`ConversationView` tracks scrolling/anchors and requests earlier history when the viewport approaches the appropriate boundary, then corrects geometry while preserving the anchor.

## Conversation interaction

The problematic path is:

```text
passive painted row
→ delegate hit-test
→ current index/materialize request
→ construct ConversationCard
→ possibly transfer QTextDocument
→ run widget layout and measurement
→ restore state
→ synthesize/replay mouse event
→ interact with actual widget
```

That path changes both the renderer and interactive object tree in response to an input gesture.

---

# 9. Rendering/materialization architecture

## F-QT-001 — Two authoritative renderers for one card

**Severity: High**

**Symbols:** `ConversationPassiveDelegate`, `ConversationCard`, `PassivePresentation`, `ConversationPresentation`.

**Evidence:** passive rows independently render card backgrounds, title, status, Markdown/body content and controls. Materialized rows use a separate QWidget hierarchy.

**User consequence:** a card can visibly change when becoming interactive despite no semantic content change.

**Architectural cause:** performance virtualization was implemented by replacing an expensive component with a separately implemented visual equivalent.

**Violated invariant:**

> One logical component should have one authoritative presentation implementation.

**Related tests:** extensive passive/materialized parity tests in `ConversationVirtualizationTest.cpp`.

**Missing test:** a test can compare geometry, but no test can make two independent renderers structurally identical for all fonts/DPR/platforms.

**Correction:** eliminate passive card rendering as a second implementation.

**Expected deletion:** roughly 700–1,200 net production lines as part of the complete virtualization simplification.

**Migration risk:** Medium–High because scrolling/performance must be preserved.

---

## F-QT-002 — Materialization changes visual identity

**Severity: High**

`ConversationView` doesn't merely activate behavior on an existing component. It substitutes a different implementation.

The code therefore needs:

* Markdown document handoff;
* geometry reconciliation;
* selection state capture;
* control-state restoration;
* focus preservation;
* explicit remeasurement;
* current-index-driven promotion.

That is exactly why Markdown can move.

The clean invariant should instead be:

> Materialization may change resource residency, but not the visual implementation of an already-visible component.

---

# 10. Layout and geometry findings

## F-QT-003 — Geometry exists in multiple implementations

**Severity: High**

Geometry knowledge appears in:

* passive delegate card sizing;
* `CardHeaderMetrics`;
* `ConversationCard` layout;
* QTextDocument widths;
* widget size hints;
* `ConversationView::measureCard`;
* height-index updates.

`ConversationPresentation` was later introduced to share metrics between passive and materialized paths. That reduces literal duplication while preserving the duplicated implementations.

The historical phrase that the passive renderer is the **“optical reference”** is especially revealing: one implementation is being used as the specification for another implementation instead of there being one implementation.

### Consequences

* font changes can break parity;
* platform style changes can break parity;
* DPR differences can break parity;
* scrollbar appearance can alter widget widths;
* QTextBrowser viewport geometry need not exactly equal manually established QTextDocument width;
* layout requests may arrive after the view believes geometry is final.

---

## F-QT-004 — Geometry settlement contains compensating machinery

**Severity: High**

`ConversationView::measureCard` repeatedly manipulates geometry/layout state and ultimately removes pending `QEvent::LayoutRequest` events from the card.

This is a warning sign.

It does not prove a Qt bug, but it demonstrates that the architecture is fighting asynchronous layout behavior sufficiently hard that pending layout requests are being suppressed to establish a stable measurement.

**Correction direction:** there should be a single height-for-width contract and one geometry owner. A visible pooled widget should be sized once against a known width; its resulting height updates the height index.

---

## F-QT-005 — Pixel stability is not an enforceable invariant today

**Severity: High**

Operations capable of changing geometry/pixels with semantically unchanged content include:

* passive→materialized transition;
* materialized→passive transition;
* focus;
* hover;
* control feedback;
* collapse;
* streamed text;
* Markdown width recalculation;
* repolishing;
* scrollbar state;
* card replacement/reconciliation;
* history insertion;
* resizing;
* splitter movement.

Some are legitimate—hover and feedback should alter appearance—but renderer switching should not.

---

# 11. Qt lifetime and event findings

## Positive finding: worker/UI ownership

`FrontendSession` has an explicit notifier/timer/thread structure and context-bound delayed callbacks. I found no source evidence justifying a claim of a systemic dangling-connection or QObject-parent defect.

`NodeGraphUiAdapter` also deliberately releases graph-reading scope before invoking QWidget work.

These boundaries should not be disturbed merely to fix the conversation renderer.

---

## F-QT-006 — Synthetic event forwarding crosses an unstable object boundary

**Severity: High**

A passive row cannot interact directly with actual controls because those controls do not exist.

The delegate therefore:

1. hit-tests a painted approximation;
2. requests materialization;
3. locates the newly created widget;
4. synthesizes/forwards the original action into the new widget tree;
5. restores/focuses the relevant object.

**User consequences:**

* fragile first-click behavior;
* focus behavior differing from subsequent clicks;
* hover/tooltip discrepancies;
* link behavior depending on promotion timing;
* potential selection differences;
* harder keyboard parity.

**Violated invariant:**

> The object receiving pointer/keyboard interaction should be the object whose geometry the user saw.

The fix is deletion of the forwarding path, not making event replay cleverer.

---

## F-QT-007 — Interaction restoration uses implementation topology

**Severity: Medium**

`CardInteractionState` records control/selection state across card destruction/materialization, including identification by child/control position.

That couples persistent interaction state to the widget hierarchy.

**Clean direction:** persistent semantic UI state should be keyed by stable logical identifiers:

* card key;
* field/control role;
* selection offsets;
* follow-tail state.

It should not depend on which child happens to be ordinal N.

---

## F-QT-008 — Markdown document ownership requires explicit destructor workaround

**Severity: Low**

`MarkdownTextView` explicitly detaches a shared document before the QTextEdit/QTextBrowser base destructor because the base class still refers to it.

The workaround shows good Qt lifetime awareness and is not, by itself, wrong.

It is nevertheless another cost of document transfer/reuse created by the dual rendering architecture.

Deleting document handoff would delete the need for this subtle lifetime rule.

---

# 12. Interaction findings

## Copy

The application has distinct native implementations for conversation copy and inspector/agent copy.

Both provide custom painting/feedback but are separate components.

### F-QT-009 — Copy interaction is duplicated

**Severity: Medium**

`CardCopyButton` versus inspector `AgentCopyButton`.

The separation is not semantically required.

A reusable `CopyButton` should own:

* icon;
* accessible name;
* keyboard behavior;
* copied feedback;
* reduced-motion policy;
* tooltip.

Context supplies only the text source.

Expected net deletion: approximately 80–150 lines.

---

## Disclosure/folding

There are different disclosure implementations in:

* cards;
* inspector agent rows;
* thread hierarchy.

The thread behavior itself is legitimately different because it operates on a hierarchy with Left/Right keyboard semantics.

### F-QT-010 — Disclosure visual/control primitive is repeated

**Severity: Low**

A common primitive should own geometry/icon/accessibility, while thread and card controllers retain their distinct behavior.

Do not force all three workflows through one controller.

---

## Ctrl+C and Markdown selection

`MarkdownTextView` explicitly handles copying and removes zero-width helper characters before writing to the clipboard.

That is reasonable.

The architectural concern is that a passive Markdown surface has no native text-selection object until promotion.

This makes selection semantics dependent on materialization.

---

## Commands and follow-tail

The explicit user-detached/follow-tail distinction is sound.

The use of deferred zero-time callbacks is understandable with QTextEdit layout but contributes to ordering complexity. It should be retained only where Qt actually requires post-layout observation.

---

# 13. Accessibility findings

## F-QT-011 — Passive rows do not expose the same accessibility tree as live cards

**Severity: High**

Painted elements are not equivalent to actual:

* buttons;
* links;
* text views;
* focusable controls.

A passive card can visually contain disclosure, Copy and Markdown links while those objects do not yet exist as child widgets.

Even if the row itself has accessible metadata, that is not equivalent to the rich card's child accessibility semantics.

**Violated invariant:**

> Accessibility semantics should not change simply because a visible component crossed a materialization threshold.

This is one of the strongest arguments for keeping actual card widgets for visible rows.

No CI lane currently establishes accessibility-tree parity.

---

## F-QT-012 — Accessibility regression testing is weak

**Severity: Medium**

The source includes conscious accessible naming for custom buttons, which is good.

But the CI matrix does not appear to contain automated coverage for:

* accessible role/state tree;
* tab sequence;
* screen-reader names;
* live region/state changes;
* passive/live equivalence.

---

# 14. Performance findings

The virtualization work was motivated by legitimate performance problems. Reverting to “every conversation row owns a permanent heavyweight QWidget” would be the wrong solution.

But the chosen optimization traded runtime work for a second UI implementation.

## F-QT-013 — Performance optimization created architectural duplication

**Severity: High**

Historical commit `bd2297e` introduced delegate painting for resting text/collapsed rows while keeping `ConversationCard` for interaction.

Subsequent commits then added:

* document reuse;
* exact geometry matching;
* deferred collapsed body projection;
* bounded per-frame materialization;
* staging;
* shared geometry metrics;
* synthetic input transfer.

This history is compelling evidence that the split is the source of accumulated complexity.

---

## F-QT-014 — Benchmark is not a quantitative performance gate

**Severity: Medium**

`ConversationViewBenchmark.cpp` exercises representative behavior and verifies completion/idle conditions, but it is not a robust threshold-based performance regression system covering:

* max frame time;
* P95/P99 scroll latency;
* materializations/frame;
* widget count ceiling;
* QTextDocument count ceiling;
* Markdown parse count;
* allocations;
* total projection cost.

A test named “benchmark” passing in CI is therefore not evidence that a future architecture is fast enough.

---

## F-QT-015 — Passive Markdown cache increases invalidation surface

**Severity: Medium**

The passive delegate maintains a bounded QTextDocument cache.

The cache is not apparently unbounded, so I do **not** classify it as a memory leak.

The problem is architectural:

* document width affects layout;
* font/style affects layout;
* content affects layout;
* materialization may transfer/adopt the document;
* caching and renderer switching are coupled.

A single widget renderer with bounded reuse substantially simplifies this.

---

# 15. Duplication inventory

| Candidate                                           | Verdict                                                                                |
| --------------------------------------------------- | -------------------------------------------------------------------------------------- |
| `ConversationPassiveDelegate` vs `ConversationCard` | **Defective duplication**                                                              |
| passive QTextDocument vs `MarkdownTextView`         | **Defective duplication**                                                              |
| passive size calculation vs widget/layout sizing    | **Defective duplication**                                                              |
| passive hit-testing vs QWidget geometry             | **Defective duplication**                                                              |
| passive Copy/disclosure painting vs controls        | **Defective duplication**                                                              |
| `CardCopyButton` vs `AgentCopyButton`               | **Unnecessary duplication**                                                            |
| card vs inspector disclosure primitive              | mostly unnecessary primitive duplication                                               |
| thread disclosure vs card disclosure                | controller separation legitimate; visual primitive shareable                           |
| browser conversation Copy vs browser inspector Copy | same frontend should preferably share primitive                                        |
| conversation Markdown vs inspector Markdown         | **Native duplication; inspector uses a distinct path**                                 |
| `LocalPrompt` vs `UserMessage`                      | **Legitimate lifecycle distinction**, especially because promotion is supported        |
| native vs browser status formatting                 | separate implementation language legitimate; semantic duplication needs contract tests |
| repeated construction helpers                       | moderate maintainability issue                                                         |
| multiple materialization/measurement paths          | **Defective consequence of renderer split**                                            |
| optimistic vs authoritative animation/state         | partly legitimate; must have one clear reconciliation owner                            |

---

# 16. Workaround and complexity inventory

The strongest architectural “scar tissue” includes:

* passive renderer;
* shared metrics added specifically for renderer parity;
* Markdown document transfer;
* document adoption;
* geometry parity tests;
* repeated materialization measurement;
* staged creation;
* overscan/materialization budgets;
* retained focus widgets outside normal eviction;
* interaction-state extraction;
* selection restoration;
* passive hit-testing;
* synthetic event forwarding;
* `LayoutRequest` suppression;
* dynamic-property repolishing;
* delayed geometry callbacks;
* splitter coalescing plus lost-release timer;
* current-index-triggered materialization.

Not every one of these is wrong independently.

The problem is their cumulative role in making two implementations behave like one.

---

# 17. Test findings

The native test suite is substantial and is a strength of the repository.

At the audited commit, GitHub Actions' native lane reported **20 CTest tests passing**, including:

* shell integration;
* graph/protocol/concurrency;
* UI adapter;
* conversation UI;
* inspector/thread pane;
* conversation model/cards;
* virtualization;
* benchmark;
* Git changes.

A green test suite therefore has real value.

But the tests expose an architectural trap:

## F-QT-016 — Tests institutionalize renderer parity

**Severity: High**

`ConversationVirtualizationTest.cpp` contains tests whose purpose is to ensure the two rendering paths remain geometrically/behaviorally equivalent.

Those tests are useful while the architecture exists, but they also make the duplicated architecture more expensive to remove and can create a false sense that parity is fundamentally achievable.

The eventual remediation should delete—not rewrite—many parity tests after there is only one renderer.

Replace them with invariants such as:

* materialization does not replace visible renderer;
* widget count remains bounded;
* row height remains stable without semantic content change;
* no extra Markdown parse on residency change.

---

## F-QT-017 — Native CI platform coverage is narrow

**Severity: Medium**

The native workflow I inspected uses Ubuntu 24.04, GCC 15 and Qt 6.

It does not form an explicit matrix across:

* X11;
* Wayland;
* XWayland;
* macOS;
* Windows;
* DPR values;
* fractional scaling;
* multiple Qt minor versions.

`ASAN_OPTIONS=detect_leaks=1` appears in the workflow environment, but the workflow itself does not demonstrate that binaries are compiled with AddressSanitizer. I therefore do not count that environment variable as sanitizer coverage by itself.

---

# 18. Git-history findings

The requested sequence is particularly informative.

## `bd2297e` — Paint passive conversation rows with a delegate

This is the architectural fork point.

Motivation: reduce widget/presentation cost for resting rows.

Mechanism: keep the existing interactive widget path but add a separate passive implementation.

This was a reasonable optimization hypothesis but introduced the central duplicated invariant.

## `aab2723` — Reuse Markdown documents across presentation

This addresses duplicated Markdown construction by transferring/reusing documents across the two paths.

It optimizes the symptom while deepening coupling between them.

## `6f81d99` — Keep virtual conversation geometry exact

Parity now becomes an explicit requirement.

More shared/exact measurement machinery is necessary because two layout systems exist.

## `abf0cb1` — Defer collapsed card body projection

Additional work reduction, reasonable in itself.

## `b2de44a` — Bound conversation presentation work per frame

Introduces work budgeting so expensive presentation cannot monopolize the event loop.

Again useful, but the amount of presentation work is partly produced by renderer switching.

## `6c153ec` — Smooth live workspace splitter resizing

This is not evidence of the duplicate-card defect. Resize coalescing is a legitimate independent concern, although the timer-based lost-release protection is another event-ordering mechanism worth keeping bounded and documented.

## `2fa5481` — Improve thread workflow and stabilize conversation UI

Broad workflow/reconciliation/follow-tail/copy refinements accumulate around a UI already carrying substantial interaction complexity.

## `629660e` — Fix conversation card presentation and interaction

The current head shares header metrics and tightens passive/materialized presentation behavior.

That is the strongest recent signal that the project is trying to enforce visual identity across two implementations rather than eliminating the invalid boundary.

### Historical verdict

The complexity did not arise randomly.

It follows a recognizable sequence:

```text
performance problem
→ second renderer
→ parity problem
→ shared geometry
→ document transfer
→ interaction-transfer problem
→ synthetic forwarding/state restoration
→ frame-budget/settlement complexity
```

This is exactly the sort of history where subtraction is preferable to another abstraction.

---

# 19. Production and test LOC analysis

I could not run `cloc` because no local checkout was available, so these are explicitly **source-size-derived estimates**, not exact physical LOC counts.

The largest production implementation files by repository blob size include approximately:

| File                        | Git blob size |
| --------------------------- | ------------: |
| `ConversationView.cpp`      |       ~159 KB |
| `ShellWidget.cpp`           |       ~140 KB |
| `ProtocolUpdater.cpp`       |       ~139 KB |
| `ClientRuntime.cpp`         |       ~121 KB |
| `ConversationCards.cpp`     |       ~101 KB |
| `NodeGraphUiAdapter.cpp`    |        ~89 KB |
| `WorkerLogic.cpp`           |        ~64 KB |
| `InspectorPane.cpp`         |        ~47 KB |
| `ConversationItemModel.cpp` |        ~47 KB |
| `ThreadPane.cpp`            |        ~46 KB |

Large tests include:

| Test                                 | Git blob size |
| ------------------------------------ | ------------: |
| `ConversationCardsTest.cpp`          |       ~344 KB |
| `ProtocolUpdaterTest.cpp`            |       ~207 KB |
| `ShellIntegrationTest.cpp`           |       ~158 KB |
| `ConversationVirtualizationTest.cpp` |       ~123 KB |
| `ClientRuntimeDispatchTest.cpp`      |        ~96 KB |

On normal C++ source densities, the **Qt-facing native production portion is approximately 35–40 K physical source lines**, excluding much of the pure protocol/nodegraph engine.

Including the closely coupled native nodegraph/runtime layer puts the audited C++ implementation on the order of **45–52 K physical LOC**.

Native/nodegraph tests are approximately **35–40 K physical LOC**.

The particularly relevant middle/conversation UI is roughly **14–17 K LOC**.

These are deliberately ranges.

### Complexity concentration

The main concern is not aggregate LOC but concentration:

* `ConversationView.cpp`
* `ConversationCards.cpp`
* `ShellWidget.cpp`

perform many lifecycle transitions and policy decisions each.

The first two especially contain code that exists primarily to maintain passive/live equivalence.

---

# 20. Clean target architecture

The cleanest target is:

```text
NodeGraph
   ↓
NodeGraphUiAdapter
   ↓
ConversationItemModel
   ↓
ConversationView
   ├─ HeightIndex
   └─ bounded ConversationCard pool
          ├─ MarkdownTextView
          ├─ CommandOutputView
          ├─ CopyButton
          └─ DisclosureButton
```

## Core invariants

### 1. One visible component, one renderer

`ConversationCard` becomes the sole renderer for a visible card.

No manually painted duplicate card body.

### 2. Virtualization controls residency, not implementation

Offscreen:

```text
model row + cached height
```

Visible/overscan:

```text
actual pooled ConversationCard
```

The object may be recycled when well outside the viewport, but a row that becomes interactive is not replaced merely because of interaction.

### 3. Delegate is decoration only

A delegate may paint:

* selection;
* viewport background;
* perhaps inexpensive row separators.

It must not paint another implementation of:

* Markdown;
* status;
* Copy;
* disclosure;
* card background/borders.

### 4. Measurement uses the same component

For unknown heights, use:

* a pooled/prototype `ConversationCard`, or
* the same content-layout primitive used by the card.

Do not reimplement card layout mathematics in a delegate.

### 5. Stable semantic UI state

Store relevant state keyed by card identity:

```text
CardKey → {
  collapsed,
  text selection if preservation is required,
  command followTail,
  command scroll offset,
  ...
}
```

No child ordinal.

### 6. One native Markdown component

Inspector should reuse the same Markdown rendering policy/component rather than:

`QTextDocument → HTML → QLabel`

as a third path.

### 7. Shared controls, specialized controllers

Reuse a common:

* `CopyButton`
* disclosure visual primitive.

Thread hierarchy still owns its specialized hierarchical keyboard controller.

---

# 21. Deletion opportunities

Estimated figures overlap, so they should not simply be summed blindly.

| Remediation                                                        |                    Delete |                 Add |        Approx. net |
| ------------------------------------------------------------------ | ------------------------: | ------------------: | -----------------: |
| remove passive card renderer/document cache/manual hit-testing     |               1,100–1,500 |             250–450 | **−700 to −1,200** |
| simplify materialization/state transfer/geometry settlement        |                   500–800 |             100–200 |   **−300 to −700** |
| consolidate Markdown surfaces                                      |                   150–250 |              50–100 |   **−100 to −180** |
| common Copy/disclosure primitives                                  |                   150–250 |              70–120 |    **−80 to −150** |
| remove obsolete parity tests and replace with residency invariants | substantial test deletion | smaller replacement |  test LOC decrease |
| remove optical-reference/shared-parity helpers no longer needed    |                   100–200 |           near zero |   **−100 to −200** |

A realistic integrated target is:

> **roughly 1,300–2,400 fewer production Qt lines**

without reducing functionality.

I would consider a redesign unsuccessful if it adds a “presentation abstraction” while retaining both the passive renderer and `ConversationCard`.

---

# 22. Ordered remediation roadmap

1. **Freeze semantics first.** Preserve current card types, node projections, scrolling behavior, LocalPrompt promotion and command follow-tail.

2. **Add performance observability before changing the renderer.** Track visible widget count, Markdown parse count, row measurement count and per-frame materialization work.

3. **Introduce bounded `ConversationCard` reuse using the existing card implementation.** Do not build a third renderer.

4. **Switch one card class at a time from delegate-rendered to pooled-widget-rendered.** Markdown You/Codex cards should be first because they expose the known defect.

5. **Delete passive Markdown rendering immediately once that migration is green.**

6. **Remove synthetic mouse forwarding and passive control hit-testing.**

7. **Remove document transfer/adoption between delegate and cards.**

8. **Make the card implementation the sole geometry authority and simplify `measureCard`.**

9. **Remove passive/live parity metrics and corresponding parity tests.**

10. **Migrate inspector Markdown to the common Markdown surface.**

11. **Consolidate Copy/disclosure primitives.**

12. **Audit remaining repolish/updateGeometry paths after the duplicate renderer is gone.** Many may become unnecessary naturally.

13. **Add multi-DPR/platform/accessibility qualification.**

14. **Run a final LOC check.** Production code should materially decrease.

---

# 23. Regression risks

The biggest migration risks are not semantic but performance/viewport behavior:

* too many live QWidget instances;
* QTextBrowser construction spikes;
* Markdown reparsing;
* stale pooled-widget content;
* focus moving when a widget is recycled;
* text selection lost during recycling;
* anchor movement while heights become known;
* command follow-tail state mapped to the wrong recycled card;
* active streaming cards being recycled;
* collapsed rows losing cached heights;
* history insertion upsetting the anchor;
* pooled widgets retaining old dynamic properties;
* lifecycle animations restarting on reuse.

All are testable without retaining a second renderer.

---

# 24. Verification requirements

A clean replacement should not be accepted merely because existing tests pass.

At minimum verify:

* only bounded visible+overscan `ConversationCard` instances exist;
* passive card renderer no longer exists;
* no separate passive Markdown implementation remains;
* interaction does not replace a visible card;
* first click and subsequent clicks use the same widget;
* keyboard focus survives scrolling where policy requires;
* Tab traversal is deterministic;
* screen-reader tree contains visible controls consistently;
* geometry is unchanged by focus/materialization alone;
* Copy feedback never alters layout;
* disclosure does not alter unrelated geometry;
* Markdown selection survives legitimate updates;
* links work using mouse and keyboard;
* command follow-tail detaches/rejoins correctly;
* history insertion preserves anchor;
* thread switch restores intended scroll state;
* streamed updates don't cause full-card reconstruction;
* width change causes exactly one necessary reflow sequence;
* DPR 1.0, 1.25, 1.5, 2.0 are tested;
* Wayland and X11/XWayland are qualified;
* reduced-motion policy is exercised;
* widget, document and cache counts are bounded;
* performance thresholds are recorded rather than merely “test completed”.

---

# 25. Unresolved questions

These cannot be settled from GitHub source alone:

1. How visible is the materialization shift on each actual Qt/platform/font combination?
2. What proportion of normal conversations are eligible for passive rendering?
3. What is the real widget-count/performance break-even point?
4. How expensive is `MarkdownTextView` construction versus reset/reuse on the target machines?
5. Is fractional scaling used routinely by the primary deployment?
6. How do current screen readers expose passive cards?
7. Do users encounter first-click failures or merely pixel movement?
8. Does the current local uncommitted work already remove parts of the duplicate path?
9. Are there Qt-version-specific QTextDocument/QTextBrowser geometry differences relevant to the issue?
10. Which performance target originally motivated `bd2297e`, in measurable milliseconds/frame?

Those need runtime evidence and are intentionally not guessed here.

---

# Consolidated findings

| ID       | Severity      | Finding                                                            |
| -------- | ------------- | ------------------------------------------------------------------ |
| F-QT-001 | **High**      | duplicate passive/live card renderers                              |
| F-QT-002 | **High**      | materialization changes visual implementation/identity             |
| F-QT-003 | **High**      | duplicated geometry/layout authority                               |
| F-QT-004 | **High**      | geometry-settlement workaround chain                               |
| F-QT-005 | **High**      | semantic-no-op renderer transitions can move pixels                |
| F-QT-006 | **High**      | synthetic interaction replay across renderer boundary              |
| F-QT-011 | **High**      | passive/live accessibility-tree mismatch                           |
| F-QT-013 | **High**      | performance optimization responsible for architectural duplication |
| F-QT-016 | **High**      | tests maintain duplicate-renderer parity                           |
| F-QT-007 | Medium        | interaction restoration coupled to widget topology                 |
| F-QT-009 | Medium        | duplicate Copy controls                                            |
| F-QT-012 | Medium        | insufficient accessibility qualification                           |
| F-QT-014 | Medium        | benchmark is not a quantitative performance gate                   |
| F-QT-015 | Medium        | passive document cache increases invalidation complexity           |
| F-QT-017 | Medium        | narrow Qt platform/DPR CI coverage                                 |
| F-QT-010 | Low           | disclosure primitive duplication                                   |
| F-QT-008 | Low           | subtle shared-document destructor workaround                       |
| POS-001  | Informational | NodeGraph is a sound authority boundary                            |
| POS-002  | Informational | NodeGraphUiAdapter is a good lock/UI seam                          |
| POS-003  | Informational | ConversationItemModel/HeightIndex are appropriate abstractions     |
| POS-004  | Informational | LocalPrompt→UserMessage promotion is architecturally sound         |
| POS-005  | Informational | native/browser frontend separation is legitimate                   |

**Count:** 0 Critical, **9 High**, **6 Medium**, **2 Low**, 5 Informational.

---

# Ten most serious findings

1. **Conversation cards have two visual implementations.**
2. **Interaction causes renderer replacement instead of acting on the object already visible.**
3. **Markdown layout is duplicated between passive QTextDocument and `MarkdownTextView`.**
4. **Card height/geometry is separately derived and then reconciled.**
5. **Synthetic event replay is required because passive controls are only painted approximations.**
6. **Passive cards cannot naturally expose the same accessibility hierarchy as materialized cards.**
7. **The performance optimization has generated a long chain of compensating mechanisms.**
8. **Tests now encode parity between two implementations instead of enforcing one implementation.**
9. **The conversation view owns excessive transition/state-transfer complexity created by renderer switching.**
10. **The current benchmark/CI does not establish quantitative performance or cross-platform visual stability.**

---

# Final assessment

**Exact commit audited:**
`629660e8882bddc7473ee5f1aab90c755ee6edd4`

**Repository tree:**
`e14c16d2e01b84c7841dbbbb2cdbe59967270414`

**Qt/native files discovered:** all production C++/headers under the relevant native tree were inventoried; **83 native production files** were accounted for, plus **20 native/nodegraph tests**, relevant build files, architecture documents and browser-side shared-contract files.

**Estimated production LOC reviewed:** approximately **45–52 K C++ LOC** including the closely coupled native runtime/nodegraph layers; approximately **35–40 K Qt-facing native implementation LOC**.

**Estimated test LOC:** approximately **35–40 K**.

**GitHub Actions:** pinned commit had a successful CI run; the native CTest lane reported all **20 registered tests passing**. The web lane was also part of the workflow.

**Runtime behavior not verified:** pixel output, actual scrolling performance, focus behavior on a live executable, screen-reader output, DPR/platform-specific rendering, memory usage and the reported visible Markdown shift were not independently executed or visually reproduced.

## Overall architectural verdict

The Qt frontend is **not generally architecturally unsound**. Its state authority, graph/UI seam and model projection are considerably cleaner than the conversation renderer would suggest.

The principal architectural defect is sharply localized:

> **Conversation virtualization virtualizes presentation by implementing the same component twice.**

That boundary should be removed.

A good repair should leave most of:

* `NodeGraph`,
* `NodeGraphUiAdapter`,
* `ConversationItemModel`,
* `ConversationHeightIndex`,
* protocol/runtime architecture,

largely intact, while substantially simplifying `ConversationView` and `ConversationCards`.

The target should be measurable not only by correctness but by subtraction: **one renderer, fewer state-transfer paths, fewer geometry authorities, fewer tests dedicated to parity, and roughly 1.3–2.4 K fewer production Qt lines.**

Within the stated **GitHub-only limitations**, I consider the source/history/test architecture audit complete.

