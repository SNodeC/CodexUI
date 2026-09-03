# Implementation Issues

## Verification can overlap an in-progress worker patch

- Observed: 2026-09-03
- Status: Open coordination issue; no library implementation failure
- Symptom: A read-only verification command can start while the worker owning
  the same source file is between patch operations. The verifier may then see
  a temporarily incomplete translation unit and report syntax or lexer errors
  that do not describe a handed-off source revision.
- Handling: The file-owning worker must explicitly announce a stable handoff
  before any compile, format check, or audit command reads that file. Discard
  results from overlapping commands, do not mark a fixed plan item failed,
  and rerun the same verification against the stable handoff. Linked probes
  must also rebuild every object whose source changed since that object was
  produced; an assertion from a mixed-generation binary is likewise a
  verification artifact rather than a result for the current source tree.

## Plan tab can reflect a transient worker failure

- Observed: 2026-09-02
- Status: Open tooling issue; no library implementation failure
- Symptom: A subagent capacity rejection (`Selected model is at capacity`) was
  displayed as a failed step in the fixed implementation plan even though no
  plan item had failed.
- Authoritative plan state at the time of the incident: items 1–2 complete,
  item 3 in progress, and items 4–9 pending.
- Handling: Preserve the immutable plan text and ordering. Republish only its
  statuses after a worker failure and do not infer plan failure from worker
  execution status.

## Outbound queued/write lifecycle ownership requires resolution

- Observed: 2026-09-03
- Status: Resolved from the master document; no new engine input or policy
  decision required
- Requirement: The wire journal retains distinct Prepared, Queued, Written,
  WriteFailed, and Indeterminate outbound lifecycle records. The exchange
  retains its Prepared write state until terminal protocol or transport
  evidence; the master explicitly requires this for reverse replies.
- Resolution: Queued is the store-owned point at which the action transaction
  inserts the emitted effect as an `OutboxEntry` whose initial state is
  `Queued`, not the later point at which an interpreter claims it. The same
  transaction appends a distinct Queued `WireFrame` with its own `FrameSeq`
  and `writeEffect`; that record may produce zero canonical changes. Claiming
  changes only privileged outbox state to `Claimed`, for which the document
  defines no `FrameDisposition` or `WriteState`.
- Handling: Add no `EngineInput` and do not fabricate a claim-time reducer
  transition. Keep the exchange write state Prepared until Written, Failed,
  Unknown, disconnection, response evidence, or resolved cleanup as
  applicable. Terminal `EffectResult` processing remains the reducer input.

## Awaiting-operation observation correlation is not defined

- Observed: 2026-09-03
- Status: Resolved by approved decision D30
- Requirement: Methods whose reconciliation policy is
  `NotificationOrRefetch` may remain in `ObservationState::Awaiting` after a
  successful response until later authoritative evidence confirms the effect.
- Gap: Neither the master document nor the pinned registry defines a complete
  per-method mapping from a prior operation to the exact notification or
  refetch result that observes it. Inferring by loosely matching identifiers
  would merge unrelated operations.
- Handling: Preserve the documented safe state (`Succeeded` + `Awaiting`) and
  do not invent matching heuristics. Recovery emits one ordered diagnostic
  reconciliation effect for each such operation; an exact requery remains an
  explicit post-Ready action because the protocol defines no safe automatic
  mapping.

## Codex update cards lack a distinct palette identity

- Observed: 2026-09-04
- Status: Open UI/UX issue
- Symptom: Neutral Codex update cards are too inconspicuous while scrolling,
  but the proposed Sky family remains too close to the existing Blue identity
  even when its numerical chroma is slightly stronger.
- Direction: Evaluate a distinct Yellow family while keeping it perceptually
  and semantically separate from Orange warnings and requests. Harmonize
  corresponding roles across all color families by OKLCH lightness and chroma,
  not equal HSL saturation. Codex updates must remain subordinate to violet
  final answers and the result must match in native CodexUI and CodexWUI.
