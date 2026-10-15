<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-records-compact.svg"><img src="../media/nav/banner-records.svg" alt="CodexUI — Records" width="100%"></picture></p>
<p><a href="README.md#banner" title="Up to Records"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-records-dark.svg"><img src="../media/nav/up-records-light.svg" alt="Up to Records" title="Up to Records" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/account-dark.svg"><img align="right" src="../media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Codex 0.162.1 local migration results

Date: 2026-10-11. The migration is implemented locally in CodexUI and AISuite. This report records the local implementation and verification before publication. Experimental API access remains enabled. No system-wide Codex installation, personal-service restart, production Codex-home access, credential copy or production database migration was performed. Runtime qualification used the complete localized release package, disposable homes/configuration/workspaces and loopback mock model providers.

The [migration inventory](codex-0.162.1-migration-inventory.md#banner) records the decisions, complete schema delta, experimental usage, persistence/package requirements and answers to the preceding questions. This report records the resulting implementation and its verification. It does not claim production deployment or complete acceptance of the canonical [22-section native interactive and visual suite](../qualification/native-test-suite.md#banner).

AISuite dependency: commit [`a913b80228e5a8320517cd989043ad3d6e6ad73b`](https://github.com/SNodeC/AISuite/commit/a913b80228e5a8320517cd989043ad3d6e6ad73b). Build and stage that SDK before rebuilding either CodexUI frontend.

## Source and architectural invariant

CodexUI verification baseline: `52be73cd39b7de9aefb9c9a8e367337f16b899ca`. AISuite verification baseline: `4f936b61a9200df1c08b276e745e67c549736dd8`. Codex target: `rust-v0.162.1`, revision `092d3acd6bec3e3a14bdc7e7a2810ab628ab759d`. Qt: 6.11.2. Node: v24.21.0.

One pinned upstream schema and registry generate both language SDKs. The bridge remains the initialization, routing and observer-policy authority. Native NodeGraph and browser PresentationModel retain application state; their adapters preserve protocol meaning at the presentation boundary. Codex retains persistence ownership. No compatibility alias, migration database, alternate renderer, timer, queue, cache or second settings authority was introduced.

## Implemented diff

| Owner | Result |
| --- | --- |
| AISuite generator and checked-in SDKs | Pin 0.162.1 and regenerate C++/TypeScript together. TypeScript composition now preserves sibling shared properties alongside alternatives. Positive and negative type checks cover URL/file-ID images, required discriminators and shared MCP identity. |
| AISuite routing and provider tests | Audit all 170 client requests while preserving the exact 50 observer reads. All twelve new requests remain controller-only. Update the real coding fixture's exact version gate; retain its sandboxed read/patch/test behavior. Unsupported image file-ID content is rejected explicitly by the existing Anthropic adapter. |
| CodexUI protocol catalog | Register twelve new requests and three explicitly state-neutral notifications. Remove unused `thread/rollback`; preserve `thread/revert`. Catalog coverage is 271 entries, including the existing legacy/internal entries. |
| Native and browser approval policies | Disclose and validate `kind=command` or `writeStdin`. Missing kind retains the upstream older-server default; malformed/unknown kinds remain blocked. Shared fixtures preserve stdin input, terminal context and callback identity in review, and reject reusable shell-policy amendments for stdin approval. |
| Native and browser conversation projections | Replace lossy `finalAnswer` booleans with protocol phase. Label partial answers distinctly and retain them when commentary updates are hidden. Turn completion still comes from authoritative turn state. Shared fixtures cover commentary, partial, final, empty and future phases, plus Copy behavior. |
| Native and browser history | Ingest item-entry wrapper lifecycle milliseconds into existing item state. Sparse and delayed history cannot erase observed lifecycle facts. Native uses existing field revisions; browser preserves established lifecycle timestamps during retained-history merges. No independent timestamp store was added. |
| Timestamp presentation | Add schema-declared item-wrapper and persistent-attachment paths. Attachment creation uses Unix seconds; arbitrary attachment payloads are not recursively interpreted as timestamps. Remove the rollback path and update provenance. |
| Resume settings | Verify additive wrapper collaborationMode through existing settings reconciliation, including browser post-dispatch changes. No new production settings mechanism was necessary. |
| Documentation | Update current provenance/counts and preserve dated historical acceptance results. |

Optional gateway, prediction, persistent-attachment, plugin and root-turn features remain server-owned and available through the generated SDK/raw transport. This migration adds no new UI workflows for them. Existing unsupported external-token refresh/attestation and the Anthropic text/coding profile's media/reasoning limitations remain as documented in the inventory.

The verified AISuite code was recovered unchanged from stash `75a62ab49c9bc70e56d83e576e12629f00b6b4fa` after a later branch reset. Documentation updates were reapplied at their new paths, preserving the intervening documentation navigation changes; the current build guide and profile index were also updated to the target version. The recovered SDK passed all 44 tests and semantic type checks again before publication.

## Local build products

| Product | Path |
| --- | --- |
| AISuite source | `/home/voc/projects/drafts/AISuite-extraction/AISuite-final` |
| AISuite Release build | `/home/voc/projects/drafts/CodexUI/build/AISuite-0.162.1` |
| Project-local AISuite installation | `/home/voc/projects/drafts/CodexUI/build/aisuite-0.162.1-stage` |
| Existing CodexUI Debug build | `/home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug` |
| Additional CodexUI Release build | `/home/voc/projects/drafts/CodexUI/build/Codex-0.162.1-Release` |
| Rebuilt browser distribution | `web/app-dist` |
| Localized complete app-server package | `/tmp/codex-upgrade-audit/package/bin/codex` |

The existing Debug build was reconfigured with `AISuite_DIR=/home/voc/projects/drafts/CodexUI/build/aisuite-0.162.1-stage/lib/cmake/AISuite` and rebuilt/relinked. Its link command uses the staged static AISuite library and its include paths use the staged generated SDK. The system AISuite installation and system Codex executable were not replaced. Builds used `--parallel 14`.

The local release package is a testing artifact, not a production executable selection. Keep its complete directory layout when choosing a subsequent deployment; the inventory describes its companion resources and the database snapshot requirement.

## Verification

| Check | Result and scope |
| --- | --- |
| Pinned regeneration `tools/regenerate-codex-protocol.sh --check` | Passed. Both checked-in language surfaces match the exact experimental schema and registry; manifest uses repository-relative provenance. |
| AISuite frontend SDK `npm test` | 44/44 passed, including C++/TypeScript graph/binding parity and the new positive/negative TypeScript semantic checks. |
| AISuite CTest | 32/33 passed inside the managed sandbox. The mock Anthropic coding fixture failed when its nested sandboxed read did not return the disposable source file. The unchanged sandboxed fixture then passed outside the outer managed sandbox: real read, patch, unit test and final answer through 0.162.1 and the local mock provider. Thus every AISuite case passed in its recorded execution environment; the first failure remains recorded. |
| Native builds | Debug and Release passed against the staged SDK and current Qt. |
| Final focused native CTest | 11/11 passed: protocol bindings/updater, pending policy, cards/model, timestamps at DPR 1/1.25/1.5/2, Git changes and shell integration. The shared-fixture adapter test also passed in its final isolated Release run. |
| Native real-server probe | Passed at DPR 1/1.25/1.5/2: actual New thread and Send clicks, streaming, Review → Accept, command output and authoritative completion through the rebuilt bridge and real 0.162.1 server. |
| Native partial-answer/diff probe | Passed at all four DPRs: partial answer visibly retained with updates hidden; select a temporary Git diff and move the cursor over its rendered text without a crash. Widget screenshots were captured and reviewed. Xvfb wrapper and child both returned zero. |
| Localized real-server operation fixture | Passed model/config/skills/thread discovery, create/turn streaming, command approval, local PNG input, read/resume, opaque item pagination and revert with the rebuilt bridge. |
| Browser `npm test` | 370/370 passed, including shared approval/phase fixtures, lifecycle wrappers, neutral notifications, resume collaboration mode and partial-answer visibility with updates hidden. |
| Browser build and artifact verification | Passed; relocatable distribution rebuilt from the regenerated SDK. |
| Real Chrome qualification | Passed with Chrome/155.0.8059.39: responsive widths 760/521/360, zero overflow, drawer/focus/target checks and existing application performance gates. Uses the qualification fixture, not a live production account. |
| Browser presentation profile | Final profile passed after removing an unnecessary new-item copy; quantitative results below. An earlier 4.13 ms streaming overrun is retained in the evidence. |
| Diff checks | `git diff --check` passed in both repositories. Removed rollback handling and the phase boolean have no remaining production consumer. Historical documents retain their historical versions. |

Evidence logs and disposable probe sources are under `/tmp/codex-upgrade-audit`. Principal logs are `regeneration-check.log`, `aisuite-sdk-implemented.log`, `aisuite-ctest.log`, `aisuite-coding-turn-host.log`, `codexui-final-adapter-ctest.log`, `codexui-final-migration-ctest.log`, `codexui-web-tests-final.log`, `codexui-browser-qualification-final.log`, and `codexui-browser-artifact-final.log`. Real native logs/screenshots use `implemented-native-*` and `implemented-phase-diff-*` paths. These temporary evidence files may be removed by system cleanup; this report and the regression fixtures persist in the repositories.

## Broad native gate failures and baseline comparison

All 84 native CTests were executed in both Debug and Release, under Xvfb with Qt offscreen, 14-way scheduling and the tests' required serialization. Each full run recorded 67 passes and 17 failures: twelve thread-pane no-op paint gates, four 10k grouped-thread timeouts, and one adapter timing gate. No assertion, timing policy or timeout was weakened.

The unchanged parent commit was extracted into a temporary source tree and rebuilt against current Qt with the previous AISuite SDK. Its representative Debug adapter, 320-row thread-pane and 10k grouped-thread cases reproduced all three failure classes: 15.31 ms versus the adapter's 8 ms gate, one no-op paint with otherwise stable pixels/model/focus/anchor, and the existing 45-second grouped timeout. The migration Debug run measured 15.75 ms for the same adapter aggregate; full conversation projection was 91.70/345.00 ms for 10k/40k cards on the parent and 93.43/344.38 ms after migration. These are recorded observations, not proof of identical performance on every machine.

The final adapter Release run passed when measured in isolation. The sixteen thread-pane benchmark failures remain unresolved baseline issues. Consequently, the broad native acceptance gate is not fully green. Evidence: `codexui-native-ctest.log`, `codexui-release-ctest.log`, `baseline-currentqt-ctest.log` and `codexui-final-adapter-ctest.log`. Full manual 22-section visual acceptance and production authentication/database acceptance are not claimed by this local migration.

Browser baseline profiling used the unchanged parent presentation sources with the same local dependency tree and Node version; it did not connect to a provider. The final profile retained 10,000 authoritative items/cards, applied 2,000 deltas and retired all 10,000 presentation lifetimes.

| Browser profile | Parent (ms) | Final (ms) | Existing limit (ms) |
| --- | ---: | ---: | ---: |
| Hydration | 23.99 | 23.83 | 47 |
| Projection | 16.83 | 15.78 | 45 |
| Streaming | 2.42 | 2.32 | 4 |
| Presentation churn | 5.42 | 5.42 | 20 |

Evidence: `baseline-browser-profile.log` and `codexui-browser-profile-reduced.log`. These single recorded runs demonstrate the gates for these fixtures; they are not a throughput or universal latency claim.

## Line and architecture accounting

Physical line counts versus the source parents, excluding new inventory/report prose:

| Repository and category | Added | Deleted | Net |
| --- | ---: | ---: | ---: |
| CodexUI handwritten production | 87 | 37 | +50 |
| CodexUI tests and shared fixtures | 464 | 135 | +329 |
| AISuite handwritten production/generation tools | 13 | 14 | −1 |
| AISuite generated C++/TypeScript production | 2,523 | 544 | +1,979 |
| AISuite tests, including the new type-test file | 24 | 3 | +21 |

Handwritten production growth across both repositories is +49 lines, within the inventory's authorized 20–60-line estimate. The growth consists of required catalog entries, metadata ingestion, conservative approval validation and phase preservation; the obsolete rollback path and lossy boolean are deleted. Generated growth represents the target release's wire surface, not a second handwritten implementation. Lifecycle history uses the existing merge owners. The unnecessary browser hydration copy found during profiling was removed before final verification.
