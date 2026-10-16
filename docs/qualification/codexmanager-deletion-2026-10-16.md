# CodexManager profile deletion qualification — 2026-10-16

## Result and scope

Implemented in `/home/voc/projects/drafts/CodexUI/codexui` on `feature/codexmanager`, above committed manager baseline `7c23552`. The existing uncommitted authentication implementation is preserved. This work is uncommitted; no push, pull request, installation or deployment was performed. GitHub reports no open PR for this branch.

The governing invariant is that permanent deletion may affect only the selected profile's exclusive directories, after owned components have stopped; registration removal comes last. Registration-only removal remains available separately. The implementation reuses the runtime operation queue, service ownership checks, reverse-order stop sequence, SSH command transport and registry persistence. One embedded Python helper supplies the same filesystem policy for local, SSH and manual execution. An optional deletion plan owns the in-flight operation; no independent process supervisor, timer or deletion implementation was introduced. The existing operation timer allows five minutes for a deletion target.

New local homes receive a UUID ownership marker. Legacy generated home names require explicit exclusive-use confirmation. Directory identity is checked again against the preview, a runtime lock is acquired, and visible same-user Codex/app-server processes are checked before deletion. Default/shared/overlapping directories, mounted subtrees and symlinked root paths are refused. Directory-relative removal preserves external symlink targets. Partial failures retain the registration and an on-disk deletion marker for retry. Local startup refuses a partially deleted home.

## Verification

- Built `codex-manager` and `codexmanager-test` with Qt 6.11.2, GCC 16.2, Debug/Ninja, `CODEXUI_INSTALL_WEB=OFF`, parallelism 14, in `/tmp/codex/build/CodexUI/manager`.
- Full manager CTest passed: 37 QtTest results, no failures or skips, 48.95 seconds. The separate deletion helper CTest passed all 12 filesystem tests. This full run preceded the final extra shared-settings overlap check; affected tests were rerun on the final production revision below.
- Final affected runtime/UI/live checks passed: 9 QtTest results, no failures or skips, 3.143 seconds. These cover overlapping settings/home refusal, local deletion, simulated SSH deletion, foreign-unit refusal, cancellation preserving data and registry, confirmed deletion removing data and registration, and real local lifecycle isolation.
- The final Python helper run passed all 12 cases: protected default/ancestor paths, shared and nested homes, root symlinks, unmarked homes, wrong/malformed ownership markers, external hardlinks, preview directory replacement, held runtime lock, active process, partial-failure retry, missing-directory idempotence, settings scope, and preservation of external symlink targets.
- Deletion-dialog interaction and button geometry checks passed under Xvfb with Qt offscreen at DPR 1.0, 1.25, 1.5 and 2.0. Each scale ran both cancel and confirm paths. Screenshots at DPR 1.0 and 2.0 were visually inspected for readable paths, complete controls and absence of clipping. [Captured dialog](../media/codex-manager-deletion.png).
- The live check created two disposable profiles using real installed bridge/app-server executables and the development CodexUI executable. Deleting the stdio profile stopped its services and removed its home/settings while the Unix-provider profile remained ready. Cleanup stopped the second profile and removed fixture sockets. No personal profile was stopped, deleted, authenticated or signed out; no inference was sent.
- `git diff --check`, task-file `clang-format --dry-run --Werror`, and task CMake formatting checks passed.

## Accounting and evidence limits

Relative to the saved pre-deletion working-tree accounting, production/build source changed by +493/−22 lines (net +471); tests/build wiring changed by +288/−2 (net +286). This authorized new feature needs directory ownership validation, destructive confirmation, filesystem cleanup and failure handling that registration-only removal did not provide. It replaces the duplicated registration-erasure body with a shared final step. Existing authentication changes are excluded from these incremental counts.

No throughput or UI-performance improvement is claimed. All deletion helpers run through the existing asynchronous process queue. Tests use small disposable directory trees; very large homes and interrupted real SSH transfers were not benchmarked. SSH execution was qualified with the existing simulated SSH fixture, not a real remote machine. Offscreen testing does not establish desktop compositor or screen-reader behavior.

Full deletion requires Linux and Python 3.11 or newer with safe directory-relative removal on each target host. Protection depends on consistent SSH aliases and the user's confirmation that the home is not shared through another registry or externally started system. Credentials in external OS keyrings, authorization grants, external source checkouts, socket locations and system journals are outside directory cleanup. Manual cleanup instructions are provided when automatic access is unavailable.

The reused build and evidence remain under `/tmp/codex/build/CodexUI/manager`; no additional checkout was created. Disposable profile directories/services were cleaned by test fixtures. The user guide describes ownership opt-in for older arbitrary homes and recovery after partial deletion.
