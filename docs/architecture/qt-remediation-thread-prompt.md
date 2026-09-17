# CodexUI Qt architecture remediation thread prompt

Use `/home/voc/projects/drafts/CodexUI/codexui` as the thread workspace and use
maximum available reasoning effort.

Read and obey the complete instruction chain before doing anything else:

1. `/home/voc/.codex/AGENTS.md`
2. repository-root `AGENTS.md`

Then read these four mandatory audit inputs completely:

1. `docs/qt-ui-architecture-audit.md`
2. `docs/architecture/reviews/claude-review-a.md`
3. `docs/architecture/reviews/claude-review-b.md`
4. `docs/architecture/reviews/chatgpt-review.md`

The local Qt audit has the highest evidentiary weight because it examined the
current worktree and performed local diagnostics. The external reviews remain
mandatory evidence and backlog inputs. Do not silently omit a finding because it
does not occur in the local audit or because another review phrases it differently.

## First task: establish the current truth

Do not edit production or test code yet.

1. Inspect the current branch, `HEAD`, worktree, and all uncommitted changes.
2. Preserve every pre-existing change as user-owned work.
3. Recheck all source references against the current worktree.
4. Distinguish committed behavior, uncommitted behavior, runtime observations, and
   unverified hypotheses.
5. Resolve factual contradictions between the four reviews, including:
   - a shared passive/widget `CardPresentation` projection versus one authoritative
     `ConversationCard` renderer;
   - whether the conversation benchmark is registered, executed, and quantitatively
     gated;
   - source-derived behavior versus locally reproduced behavior;
   - behavior at audited commit `629660e8882bddc7473ee5f1aab90c755ee6edd4`
     versus the current worktree;
   - extracting additional classes versus actually simplifying and deleting
     architecture.

## Consolidated findings ledger

Create one authoritative findings ledger before finalizing an implementation plan.
For every distinct finding from every review, record:

- source review and original finding identifier or heading;
- affected files and symbols;
- whether it remains valid in the current worktree;
- direct local source or runtime evidence;
- evidence class and severity;
- violated architectural invariant;
- duplicates, dependencies, and superseding findings;
- accepted remediation or reason for rejection;
- planned implementation stage;
- correctness, performance, accessibility, and regression verification;
- expected production-code deletion or justified growth;
- current status.

Classify every external finding explicitly as one of:

- verified and planned;
- already fixed;
- duplicate of another finding;
- superseded by a stronger architectural correction;
- disproven with evidence;
- unresolved pending measurement.

Deduplication must preserve provenance. No finding may disappear without an explicit
classification and reason.

## Plan

After the ledger is complete, populate the plan tab with one authoritative,
dependency-ordered remediation plan. Do not maintain competing plans. Each stage
must state:

- the architectural invariant restored;
- exact scope and dependencies;
- architecture, code, state, and tests scheduled for deletion;
- any unavoidable additions and why reduction/change cannot solve them;
- acceptance and rollback criteria;
- runtime and performance measurements;
- expected production and test LOC deltas.

Do not start implementation until the diagnosis, target architecture, ledger, and
plan are internally consistent.

## First implementation stage after planning

The first architectural remediation is expected to be the conversation renderer,
unless current evidence disproves that dependency order:

- Use the existing `ConversationCard` as the one authoritative renderer for viewport
  plus overscan rows.
- Keep virtualization responsible for residency, reuse, scheduling, and cached scalar
  heights only—not for a parallel visual implementation.
- Retain view-owned Turn decoration unless evidence shows a cleaner existing owner.
- Delete the complete passive card renderer.
- Delete passive Markdown rendering and its document cache.
- Delete the document-transfer/adoption bridge between passive and materialized
  implementations.
- Delete passive control painting and hit-testing.
- Delete synthetic pointer-event forwarding across materialization.
- Delete interaction-state and geometry-reconciliation machinery that exists only
  because interaction changes the renderer.
- Delete passive/materialized parity-only tests and replace them with tests of the
  single-renderer invariants, bounded residency, stable geometry, interaction,
  accessibility, and performance.
- Do not introduce a third renderer or a new presentation abstraction that leaves
  both complete existing implementations alive.

Preserve and verify:

- scrolling and anchor restoration;
- history insertion and paging;
- thread switching and per-thread viewport state;
- local-prompt promotion and authoritative acknowledgement;
- streaming updates and active-work animation;
- Copy feedback and Markdown selection;
- disclosure and folding;
- keyboard focus and deterministic navigation;
- links and accessibility semantics;
- command-output follow-tail and user detachment;
- resizing and width-dependent reflow;
- reduced-motion behavior;
- bounded widget, document, parse, measurement, and per-frame work counts.

Expected production reduction for this stage: 750–1,050 LOC. Treat this as a target
to validate, not permission to delete required behavior. Report production and test
LOC independently before and after every stage.

## Continuing remediation

After the renderer stage passes all gates, continue through every verified ledger
finding in dependency order, including findings concerning:

- `ConversationView` and `ShellWidget` responsibility concentration;
- design-token bypass and raw visual literals;
- duplicate Copy, disclosure, label, Markdown, status, and formatting policies;
- dynamic QObject properties used as functional state;
- DTO identity and native/browser semantic drift;
- graph retirement/error handling;
- accessibility and selection visibility;
- test short-circuiting and test structure;
- production-library/CMake duplication and inconsistent compile definitions;
- warnings, sanitizers, platform, DPR, and quantitative performance coverage;
- parity tests that execute only one implementation.

Extraction alone is not simplification. Do not move the same complexity into more
files and call it architecture improvement. Each accepted architectural stage must
reduce competing authorities, state transitions, special cases, or production LOC.

## Mandatory execution rules

- Treat every fix as a fix of the whole architecture.
- Follow the strict order: reduce, then change, and only then add with explicit user
  approval.
- Correct causes rather than downstream symptoms.
- Replace and delete superseded mechanisms within the same migration stage.
- Do not weaken tests or preserve defective duplication merely to keep old tests
  passing.
- Do not claim completion while known workarounds, duplicate implementations,
  temporary compatibility code, or required verification remain.
- Run all relevant tests without short-circuiting later cases after an early failure.
- Compare performance-sensitive changes with the recorded baseline.
- Report exact commands, results, LOC deltas, limitations, and remaining risks.
- Do not install, restart, commit, push, discard, or overwrite user work without the
  authority required for that action.
