# CodexUI engineering instructions

The global engineering instructions in `~/.codex/AGENTS.md` apply in full.

## System boundaries

- `NodeGraph` is the authoritative application-state boundary.
- `NodeGraphUiAdapter` projects graph state into toolkit-facing value types without
  retaining graph locks during QWidget work.
- Qt models and widgets are projections and interaction surfaces; they must not
  become competing authorities for protocol state.
- Renderer-local optimistic state must have one explicit owner and one deterministic
  reconciliation path to authoritative app-server state.
- Preserve the established single-writer graph and UI-thread ownership boundaries.

## Unified presentation architecture

- Every user-visible concept must have one authoritative presentation policy, one
  geometry authority, one interaction implementation, and one accessibility model.
- Performance optimization may change residency, reuse, caching, or scheduling. It
  must not create another implementation of the same visible component.
- A logical conversation card must not change visual implementation when it becomes
  hovered, selected, focused, copied, expanded, or otherwise interactive.
- Visible and overscan conversation rows must use the same authoritative card
  implementation.
- Offscreen virtualization may retain model data and cached scalar geometry, but it
  must not maintain a parallel complete renderer.
- Do not duplicate card layout, Markdown rendering, header geometry, status
  presentation, Copy controls, disclosure controls, hit-testing, or accessibility
  semantics.
- Interaction must be delivered to the object whose geometry the user sees. Do not
  synthesize input across a renderer-replacement boundary.
- Materialization or recycling alone must not move pixels, change height, restart an
  animation, lose selection, change focus, or alter accessibility.
- Store persistent interaction state by stable semantic identity, never by incidental
  child-widget position or object topology.

## Shared UI foundations

- `UiStyle` tokens are the authority for reusable colors and visual metrics.
- Do not introduce raw reusable UI color literals outside the designated token
  definitions.
- Use one shared native Markdown presentation implementation wherever behavior is
  intended to be identical.
- Use shared Copy and disclosure primitives; retain separate controllers only when
  their semantics genuinely differ.
- Shared controls must provide consistent geometry, focus, keyboard behavior,
  accessibility, feedback timing, and reduced-motion behavior.
- Use typed members or value types for functional state. Do not use dynamic QObject
  properties as hidden functional state.
- Normalize protocol presentation values at the adapter boundary rather than during
  repeated paint operations.

## Conversation behavior invariants

- Preserve in-place optimistic-prompt reconciliation with authoritative user messages.
- Preserve explicit command-output follow-tail and user-detached states.
- Streaming, acknowledgement, history insertion, thread switching, resizing, Copy,
  disclosure, text selection, focus, and animations must not introduce unrelated
  geometry changes.
- A width change should cause only the necessary reflow and must not create competing
  measurements of the same card.
- Keep card widget, document, cache, parsing, measurement, and per-frame work counts
  bounded and measurable.

## Native and browser contract

- Separate native and browser renderers are valid; separate meanings for shared data
  and lifecycle concepts are not.
- Shared DTO meaning, normalization, status classification, identifiers, bounding
  rules, and lifecycle behavior must come from one contract or from differential
  fixtures executed by both implementations.
- A test that executes only one frontend must not be described as a parity test.

## Qt correctness

- Treat QObject ownership, connection lifetime, event delivery, focus, keyboard
  navigation, accessibility, device-pixel ratio, and deferred layout as architectural
  concerns.
- Add timers or deferred callbacks only where Qt requires a later event-loop phase;
  document the required ordering and test it.
- Do not suppress layout events to force stability without first correcting geometry
  ownership.
- Qualify geometry-sensitive behavior at DPR 1.0, 1.25, 1.5, and 2.0.
- Every visible control must have stable hit geometry and an accessible representation.

## Tests, performance, and build consistency

- Every test case must execute even when an earlier case fails, and failures must be
  individually identifiable.
- Geometry coverage must include both axes, representative content and Markdown,
  multiple widths, and multiple DPRs.
- Performance-sensitive architecture requires quantitative limits for widget count,
  document count, parse count, measurement count, and per-frame work.
- A benchmark that merely completes is not a regression gate.
- Compile production sources once through reusable targets where practical, and keep
  relevant feature definitions consistent between production and tests.
- Native/browser parity must be checked by running both implementations over a shared
  corpus, not by maintaining hand-copied expected literals.

## Required change gate

Before editing production code:

1. Trace the complete affected path.
2. State the violated invariant.
3. Inventory every implementation and owner of the concept.
4. Identify the code, state, and special cases the correction will delete.
5. Define correctness, performance, accessibility, and regression checks.
6. Record production and test LOC separately.

Before accepting a change:

1. Remove the architectural cause rather than compensating for it.
2. Delete superseded code in the same migration stage.
3. Account for every added timer, cache, flag, state variable, dynamic property, and
   duplicated path.
4. Execute all relevant tests.
5. Compare performance with the recorded baseline.
6. Keep production LOC neutral or lower for fixes and materially lower for
   architectural simplifications.
7. Report exact verification and every remaining limitation.

## Current architecture work

- Do not perform opportunistic production fixes in the conversation-presentation
  subsystem while its architecture remediation is being designed.
- Treat conversation rendering, geometry, materialization, interaction forwarding,
  Markdown document transfer, and card-state restoration as one subsystem.
- Do not add a third presentation abstraction while retaining complete passive and
  materialized card implementations.
- The accepted remediation must converge on one authoritative visible-card renderer
  and must materially reduce production Qt code.
