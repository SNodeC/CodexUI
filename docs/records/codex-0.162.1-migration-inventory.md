<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-records-compact.svg"><img src="../media/nav/banner-records.svg" alt="CodexUI — Records" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Records"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-records-dark.svg"><img src="../media/nav/up-records-light.svg" alt="Up to Records" title="Up to Records" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#project-overview" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Codex 0.162.1 migration inventory

Inventory date: 2026-10-11. This records the analysis phase before the migration diff. At that point, neither repository's production code or tests had been edited. The subsequent authorized local implementation and its verification are recorded in [the implementation report](codex-0.162.1-migration-results.md). Experimental API access stays enabled. No system-wide Codex was installed, and production CODEX_HOME/database/authentication were not used for runtime testing.

## Versions and authority

CodexUI baseline is commit `52be73cd39b7de9aefb9c9a8e367337f16b899ca`. AISuite source baseline is commit `4f936b61a9200df1c08b276e745e67c549736dd8`. Its checked-in and installed bindings target `rust-v0.154.0`, commit `6b9826e3aa83b1a5947db50f4332cb9c65f1b340`. The target is **0.162.1**, not the earlier mistyped 0.126.1: tag `rust-v0.162.1`, resolved commit `092d3acd6bec3e3a14bdc7e7a2810ab628ab759d`. GitHub's release was published on 2026-10-09.

The governing invariant is one pinned upstream schema and registry generating both AISuite language surfaces; the bridge owns initialization, routing, control and permissions; CodexUI's NodeGraph owns native application state; frontend adapters project equivalent semantics without replacing server authority. Incoming data must survive transport, typed decoding, normalization, history hydration and presentation without granting additional authorization or fabricating lifecycle state.

The affected path is release package → bridge child launcher → initialize → generated C++/TypeScript SDK → routing/observer policy → native ClientRuntime/browser BrowserFrontendSession → graph/PresentationModel → normalized DTOs → approval and conversation surfaces. Persistence is owned and migrated by upstream Codex; CodexUI must not implement a parallel database migration.

Source references: [official OpenAI App Server documentation](https://learn.chatgpt.com/docs/app-server), [target release](https://github.com/openai/codex/releases/tag/rust-v0.162.1), [pinned protocol registry](https://github.com/openai/codex/blob/092d3acd6bec3e3a14bdc7e7a2810ab628ab759d/codex-rs/app-server-protocol/src/protocol/common.rs), and [pinned thread contract](https://github.com/openai/codex/blob/092d3acd6bec3e3a14bdc7e7a2810ab628ab759d/codex-rs/app-server-protocol/src/protocol/v2/thread.rs). Version-specific conclusions below use this pinned source rather than assuming today's documentation describes every release detail.

## Questions resolved

| Question | Resolution |
| --- | --- |
| Can the existing system simply switch binaries? | Tested core chat paths work, but a complete migration needs schema generation, generator correction, explicit catalog reconciliation, approval policy correction and history timestamp handling. A binary-only change is not accepted as a finished migration. |
| Keep experimental features? | Yes. Preserve `initialize.capabilities.experimentalApi=true` and experimental schema generation. No disable switch or stable-only fork is proposed. |
| Does the bridge use experimental APIs? | It opts the shared upstream connection into experimental API access and generates the complete experimental protocol for both SDKs. Runtime engine feature flags and protocol capability opt-in are different controls. |
| Does CodexUI use experimental APIs? | Yes: project operations/membership/filtering; collaboration-mode discovery and `turn/start.collaborationMode`; selected named permission profiles through `permissions`. Native Inspector can also request experimental catalogs such as queue/background-terminal reads. |
| What would disabling access lose? | Projects and project assignment/filtering, collaboration/plan-mode parameters and named profile parameters would be rejected. Ordinary turns can also fail because native settings emit collaborationMode when a model is resolved. Section CRUD, basic history paging and ordinary approvals remain stable when experimental parameters are omitted. Disabling is therefore not an equivalent configuration of today's UI. |
| Has anything changed experimentally in 0.162.1? | Yes; the retained opt-in is necessary but does not make removed methods or changed schemas compatible. The method and field inventories below cover the target's experimental export. |
| Replace rollback with revert? | Remove the unused rollback registration/branch/timing path and obsolete rollback-only test cases. Preserve revert. Rollback took a count; revert takes `beforeTurnId`, excludes that turn and all later turns, returns metadata plus cursors, and does not undo workspace files. Do not rename blindly or add a compatibility shim. No production caller of rollback was found beyond catalog/updater/timing handling. |
| Why did the localized harness show “configuration not writable”? | An accidental installed `codex-ui-harness --help` invocation opened a GUI and attempted its normal config handling at `/home/voc/.config/codex-ui-harnessrc`. Sandbox restrictions denied the write. The process was closed; this was not an app-server protocol error or production database migration. Use an explicit disposable config for GUI tests. |
| Where was the earlier crash/build fix? | CodexUI `CMakeLists.txt`, commit `52be73c`: the configured Qt Core version is an interface compile definition on `Qt6::Core`. A CMake reconfiguration discovering a different Qt version changes consumer compile commands, causing rebuild/relink. Merely replacing a system Qt installation does not guarantee an already-configured build immediately discovers it; Qt Creator must rerun CMake with the intended Qt installation. This fix is already committed and is separate from this migration. |
| Authentication changes? | Existing login/read/logout/refresh wire contracts remain available. Dummy-key login, temporary auth-file creation and logout/removal passed. The new gateway OAuth capability is optional: leave it unset to preserve upstream automatic-login behavior. No gateway UI or external-token ownership is added. Existing unsupported CodexUI external ChatGPT-token refresh and attestation responses remain explicit limitations, not newly introduced failures. |
| Will databases migrate? | Yes, upstream adds migrations. Only a synthetic 0.154 home was migrated here; details below. A production switch can migrate real databases on startup. Downgrade by binary replacement is not an accepted rollback plan: restore a coherent pre-upgrade home snapshot if rollback is required. No production migration was performed. |
| Is one executable sufficient? | No for complete functionality. Use the complete versioned release package and preserve its layout; point the existing launcher at `bin/codex`, which receives `app-server` arguments. No launcher API rewrite is needed for the chosen package. |

## Exact protocol surface

The existing generator was run against the target's exact experimental export and source registry into `/tmp/codex-upgrade-audit/generated`, without replacing repository artifacts. Schema SHA-256: `e4e7f0c7d3fd77c48cd8619cad2bf2b315d860ab006ad5a7ebe44f6e32e666c1`; registry SHA-256: `e25ebb90bf79be79def79ebf2dd704057577fa9806710fbea0ee240a909d0dc1`.

| Surface | 0.154.0 | 0.162.1 |
| --- | ---: | ---: |
| Generated C++/TypeScript datatypes with current generator | 2,413 | 2,527 |
| Canonical root types | 90 | 90 |
| Canonical v2 types | 748 | 789 |
| Client requests | 159 | 170 |
| Server requests | 11 | 11 |
| Client notifications | 1 | 1 |
| Server notifications | 81 | 84 |
| CodexUI catalog including 3 legacy and 2 internal entries | 257 | 271 |

Only `thread/rollback` is removed. Twelve requests are added: `account/bedrock/checkGovCloudRequirements`, `account/gatewayOAuth/read`, `account/gatewayOAuth/login`, `account/gatewayOAuth/cancel`, `memory/status`, `rollout/compress`, `thread/attachment/add`, `thread/attachment/list`, `thread/attachment/remove`, `thread/attachmentOwner/list`, `thread/prediction/request`, and `userVerification/cancel`. Three notifications are added: `account/gatewayOAuth/changed`, `thread/attachment/updated`, and `thread/prediction/updated`. No server-request method is added or removed.

Observer policy decision: preserve the current exact 50-operation allowlist. All twelve new operations remain controller-only in this migration, including read candidates. Update the independent 170-operation permission audit to make that deliberate denial explicit; do not expand observer access through naming heuristics. Upstream source identifies attachment lists, gateway readiness, memory readiness and GovCloud requirements as read candidates, but adding observer product capability is separate scope.

New-feature decision: generate all new wire types and register all new methods, but do not add new gateway, persistent-attachment or prediction UI workflows. Classify all three new notifications as `IntentionallyStateNeutral` in the native catalog and authority `none` in browser mappings, since no corresponding UI workflow is supported in this migration. Keep raw payloads in SDK callbacks for consumers and bounded notification metadata in frontend protocol diagnostics; native diagnostics deliberately do not retain full payloads. Test that these notifications cannot overwrite thread execution status or turn results. A notification with a nested `result` is not a turn-completion event. Unknown native notifications currently reach an UnknownProtocol graph node; browser unknown notifications produce diagnostics. Forward compatibility is not equivalent to semantic UI support.

## Required corrections before regenerating accepted bindings

### Hybrid schema handling

AISuite `tools/generate-codex-protocol.mjs` currently chooses `oneOf`/`anyOf` before processing sibling `properties`. The new image schemas declare shared `type`/`detail` properties together with URL-versus-file-ID alternatives. Current regeneration emits `V2ImageUserInput = V2UrlUserInput | V2FileIdUserInput`, dropping the required discriminator; the same happens for image ContentItem and FunctionCallOutputContentItem. The existing MCP elicitation schema also combines shared request identity with alternative payloads. This is a generator responsibility defect, not a reason to hand-edit generated files or introduce frontend-specific shadow types.

Correction decision: reshape the existing TypeScript schema emitter to represent sibling object constraints intersected with the alternative union, with correct parentheses. Preserve independent constraints for allOf and required fields. Keep C++ facades lossless, with alternative child types available through raw JSON; reconcile accessor removals caused by the schema shape rather than manufacturing obsolete fields. Tests must check schema semantics, not only C++/TypeScript name/count parity. A negative type check proved that malformed image values missing `type` are currently accepted: both expected-error directives were unused. The otherwise passing SDK suite does not close this defect.

Images are an additive wire change, not URL removal: UserInput accepts `url` or `fileId`; Responses ContentItem/function outputs accept `image_url` or `file_id`. Local image submission still works. The Anthropic adapter continues to consume supported URL/base64 images; do not pretend provider-specific file IDs can be resolved by Anthropic. Preserve explicit unsupported-input behavior for that new alternative, with a regression test.

### Approval discriminant

Both 0.154 and 0.162 servers send `kind="command"` in command approvals. Native `PendingRequestPolicy.cpp` and browser `PendingRequestResponses.ts` omit `kind` from disclosure fields. Unknown-member detection marks disclosure incomplete and removes approval decisions. Native real-server Review dialogs therefore offered only Cancel on both versions. The bridge/wire approval round-trip itself passed.

Correction decision: disclose and validate the existing `kind` field in both policies. The target enum is **`command` or `writeStdin`**, not command/network. Missing means command for older servers; malformed/unknown values remain non-approvable. Stdin approval must visibly retain its submitted input and terminal context, and must not silently authorize a reusable shell execpolicy amendment. Keep `networkApprovalContext` as its own orthogonal context. Extend shared differential approval fixtures and widget tests, rather than removing incomplete-disclosure protection.

### Lifecycle timestamps

New `ThreadItemEntry.startedAtMs/completedAtMs` are optional Unix milliseconds. Native item-page ingestion consumes only `entry.item`, losing wrapper lifecycle fields; browser `PresentationModel.mergeItemPage` does the same. Existing live notification paths already handle those fields.

Correction decision: merge schema-declared wrapper lifecycle values into the same authoritative item state during history ingestion, using existing stale-response/revision rules so a late history page cannot erase newer live lifecycle facts. Update `ProtocolTimingPaths.inc` from its 0.154 provenance: add the item-wrapper paths, remove rollback, and add persistent attachment timestamps for registered attachment responses/notifications. `ThreadAttachment.createdAt` is Unix **seconds**, confirmed in upstream state model; do not use milliseconds based on name inference or recursively scan arbitrary attachment payloads.

### Message phase

`partial_answer` is a new valid phase; `commentary` and `final_answer` remain. Current native and browser DTOs collapse phases into `finalAnswer:boolean`. The native fixture preserved text but labeled partial answers “update”; both frontends hide them when “show Codex updates” is disabled.

Correction decision: replace that lossy boolean with the existing protocol phase in both DTO projections, without a parallel renderer or a second status authority. Show partial answers as answers with a distinct label and preserve them when commentary updates are hidden. Only final_answer gets final-answer presentation; only authoritative Turn status controls completion. Keep streaming identity, geometry and Copy behavior. Qualify this within the current conversation architecture gate, not as an unrelated renderer refactor.

## File-by-file concrete diff specification

Paths beginning AISuite refer to `/home/voc/projects/drafts/AISuite-extraction/AISuite-final`; CodexUI paths refer to this repository.

| Owner / files | Required change and verification |
| --- | --- |
| AISuite `tools/regenerate-codex-protocol.sh` | Change release/revision to the exact target; keep `CODEX_PROTOCOL_EXPERIMENTAL=1` and verified tag resolution. |
| AISuite `tools/generate-codex-protocol.mjs`; generator semantic tests / frontend type tests | Correct hybrid schemas in the single emitter. Test both URL and file-ID shapes, required discriminators and MCP shared identity. |
| AISuite `src/ai/openai/codex/protocol/generated/{ProtocolTypes.h,manifest.json}` and `packages/codex-frontend/src/protocol/generated.ts` | Regenerate together after generator correction; use normal repository-relative manifest paths, not this temporary probe's paths. Run regeneration drift check and language-surface parity. |
| AISuite `tests/codex/BridgeRoutingTest.cpp` | Audit all 170 requests, 84 notifications and unchanged 11 server requests. Keep 50 approved observer reads and explicitly deny new methods for observers. Production allowlist needs no expansion. |
| AISuite `tests/model/coding_turn.py` | Update exact version assertion from 0.154.0 to 0.162.1. Keep the real sandboxed acceptance path; the temporary danger-full-access experiment is evidence for the adapter only and must not replace that test. |
| AISuite `tests/model/ResponsesAdapterTest.cpp`, existing image/streaming/provider tests | Qualify parser/request compatibility and unsupported file-ID behavior. Preserve final-answer versus commentary output semantics for the Anthropic provider. Production adapter changes are needed only if a boundary test exposes a concrete incompatibility; the real mock-provider coding turn passed. |
| AISuite `docs/codex/responses-profile.md`, `docs/codex/observer-policy.md`, `docs/guides/agent-providers.md`, `docs/architecture/codex-bridge.md` | Update current contract/provenance/counts and test requirements. Do not rewrite historical live acceptance results as if rerun. |
| CodexUI `src/codex/nodegraph/ProtocolCatalog.cpp` and `tests/codex/nodegraph/ProtocolBindingsTest.cpp` | Remove rollback; add twelve requests and three explicitly classified notifications; reconcile 271 catalog entries and 170/11/84/1 generated counts. Retain legitimate legacy/internal entries. |
| CodexUI `src/codex/nodegraph/ProtocolUpdater.cpp`, `tests/codex/nodegraph/ProtocolUpdaterTest.cpp` | Remove rollback-only handling; preserve revert history replacement/cursors. Ingest history wrapper lifecycle fields through existing item authority and revision rules. Test new notification classification and additive resume collaborationMode. |
| CodexUI `src/codex/ProtocolTimingPaths.inc`, `tests/codex/TimestampPresentationTest.cpp` | Correct provenance, wrapper/attachment timestamp paths and units; remove rollback timing path. Keep bounded schema-only traversal. |
| CodexUI `src/codex/PendingRequestPolicy.cpp`, `tests/codex/PendingRequestPolicyTest.cpp`, `tests/codex/ShellIntegrationTest.cpp` | Correct command/writeStdin disclosure and validity; real Review → Accept → command result must pass; unknown/malformed kinds must remain blocked. |
| CodexUI `web/src/app/PendingRequestResponses.ts`, browser approval tests, `tests/fixtures/frontend-presentation.json` | Apply equivalent policy and execute native/browser differential corpus. Avoid separately invented approval meaning. |
| CodexUI `src/codex/ui/NodeGraphUiAdapter.cpp`, `src/codex/middle/{MiddleTypes.h,ConversationCards.cpp,ConversationItemModel.cpp}` and corresponding tests | Replace finalAnswer boolean with phase at the existing projection boundary; use that phase for labels/visibility. No new rendering implementation or lifecycle state. |
| CodexUI `web/src/conversation/{MiddleTypes.ts,ConversationProjection.ts}`, `web/src/app/App.tsx`, browser projection/presentation tests and shared fixture | Same phase meanings, visibility and Copy contract. |
| CodexUI `web/src/presentation/{ProtocolNormalizer.ts,PresentationModel.ts}` and browser session tests | Preserve history lifecycle wrappers, resume settings and explicit new notification handling. Root-turn IDs and optional plugin fields remain server-owned; no new controls in this migration. |
| Native build + AISuite frontend package + CodexUI browser distribution | Build/install or locally stage AISuite first; reconfigure/rebuild all CodexUI Qt consumers against the same regenerated SDK; rebuild TypeScript SDK and refresh local browser package dependency/dist before building browser artifact. No independently maintained web wire schema. |
| Versioned external Codex package | Preserve full package layout and use explicit existing app-server executable override for tests. Production selection/deployment is outside this inventory. |

## Remaining additive schema changes and their owners

There are 43 new canonical types, two removed canonical types and 44 structurally changed existing types. The table below is exhaustive at canonical-type level; union details follow. The machine-readable `/tmp/codex-upgrade-audit/schema-delta.json` records this inventory and the raw schema entries for timestamp additions; both complete upstream schema snapshots remain in the same temporary evidence directory. Documentation-only description changes were excluded. Union arrays were normalized for comparison so ordering does not falsely imply removal.

| Existing type | Added fields | Removed fields | Changed fields / structure |
| --- | --- | --- | --- |
| `ClientRequest` | — | — | ; union/required/other structure |
| `InitializeCapabilities` | explicitGatewayOauth | — | — |
| `McpServerElicitationRequestParams` | — | — | ; union/required/other structure |
| `ServerNotification` | — | — | ; union/required/other structure |
| `v2/AppConfig` | omit_tools_from | — | — |
| `v2/BrowserUseRequirements` | extension | — | — |
| `v2/CodexErrorInfo` | — | — | ; union/required/other structure |
| `v2/ConfigRequirements` | allowedLoginMethods, modelProvider, modelProviders | windowsSandboxPrivateDesktop | allowedWindowsSandboxImplementations |
| `v2/ConfigRequirementsReadResponse` | supportsIndependentSpeedModes | — | — |
| `v2/ContentItem` | — | — | ; union/required/other structure |
| `v2/EnvironmentAddParams` | authBearerToken, skills | — | — |
| `v2/FeedbackUploadResponse` | promptHash | — | — |
| `v2/FunctionCallOutputContentItem` | — | — | ; union/required/other structure |
| `v2/GetAccountResponse` | workspaceRouting | — | — |
| `v2/ListMcpServerStatusParams` | serverName | — | — |
| `v2/McpResourceReadParams` | target | — | — |
| `v2/McpServerOauthLoginCompletedNotification` | loginId | — | — |
| `v2/McpServerOauthLoginResponse` | loginId | — | — |
| `v2/McpServerStatus` | httpOrigin, serverCapabilities | — | — |
| `v2/MessagePhase` | — | — | ; union/required/other structure |
| `v2/MisalignmentErrorDetails` | reviewTarget | — | — |
| `v2/Model` | availableAccessPrograms | — | — |
| `v2/ModelProviderCapabilitiesReadResponse` | — | namespaceTools | ; union/required/other structure |
| `v2/PlanType` | — | — | ; union/required/other structure |
| `v2/PluginDetail` | onboardingSkill | — | — |
| `v2/ResponseItem` | — | — | ; union/required/other structure |
| `v2/SkillMetadata` | — | — | path |
| `v2/SkillSummary` | — | — | path |
| `v2/ThreadForkResponse` | disabledPluginIds | — | — |
| `v2/ThreadGoalClearParams` | origin | — | — |
| `v2/ThreadGoalSetParams` | origin | — | — |
| `v2/ThreadItem` | — | — | ; union/required/other structure |
| `v2/ThreadItemEntry` | completedAtMs, startedAtMs | — | — |
| `v2/ThreadItemsListParams` | — | — | cursor |
| `v2/ThreadRealtimeStartParams` | backendReasoningStatus | — | — |
| `v2/ThreadResumeResponse` | collaborationMode, disabledPluginIds | — | — |
| `v2/ThreadSettings` | disabledPluginIds | — | — |
| `v2/ThreadSettingsUpdateParams` | disabledPluginIds | — | — |
| `v2/ThreadStartParams` | daybreakEnabled | — | — |
| `v2/ThreadStartResponse` | disabledPluginIds | — | — |
| `v2/Turn` | rootTurnId | — | — |
| `v2/TurnStartParams` | disabledPluginIds, parentTurnId, rootTurnId | — | — |
| `v2/UserInput` | — | — | ; union/required/other structure |
| `v2/UserVerificationEnrollResponse` | algorithm, publicKey | — | — |

The apparent top-level image URL removals in a naive union-branch comparison are relocation into URL/file-ID alternatives, not removed functionality. Other union changes: MCP user-verification elicitation gains optional `_meta`; ResponseItem gains `additional_tools`; ThreadItem MCP calls gain `mcpAppUi` and change deprecated `mcpAppResourceUri` default behavior; subAgentActivity gains model and reasoning effort; CodexErrorInfo adds flexUnavailable and tooManyDenials; PlanType adds promax; Windows sandbox implementations become typed enum values. All are generated wire-surface changes. Existing raw payload retention and generic display handle additive optional fields; do not add UI controls to expose every new field.

`SkillMetadata.path` and `SkillSummary.path` move from AbsolutePathBuf to LegacyAppPathString, both represented as strings; preserve paths and avoid imposing a new local canonicalization policy. ThreadItemsList cursor adds an item-anchor alternative, while existing opaque string cursors remain valid and tested. `ModelProviderCapabilitiesReadResponse.namespaceTools` and `ConfigRequirements.windowsSandboxPrivateDesktop` are actually removed; no handwritten production consumer was found in either repository. Update generation and tests rather than emulating their old values. Resume now returns collaborationMode; native settings already have a descriptor and browser settings-field projection already accepts this field through its existing response reconciliation. Verify that path rather than adding a second settings mechanism.

New canonical types:

`v2/AccountRoutingOverride` `v2/BedrockCheckGovCloudRequirementsParams` `v2/BedrockCheckGovCloudRequirementsResponse` `v2/BrowserUseExtensionRequirements` `v2/EnvironmentSkillsParams` `v2/GatewayOAuthCancelResponse` `v2/GatewayOAuthChangedNotification` `v2/GatewayOAuthLoginResponse` `v2/GatewayOAuthReadResponse` `v2/GatewayOAuthStatus` `v2/McpAppDisplayMode` `v2/McpAppUi` `v2/McpResourceReadTarget` `v2/MemoryStatusParams` `v2/MemoryStatusResponse` `v2/ModelAccessPrograms` `v2/RequestHeader` `v2/RolloutCompressResponse` `v2/ThreadAttachment` `v2/ThreadAttachmentAddOutcome` `v2/ThreadAttachmentAddParams` `v2/ThreadAttachmentAddResponse` `v2/ThreadAttachmentListParams` `v2/ThreadAttachmentListResponse` `v2/ThreadAttachmentOperation` `v2/ThreadAttachmentOwner` `v2/ThreadAttachmentOwnerListParams` `v2/ThreadAttachmentOwnerListResponse` `v2/ThreadAttachmentRemoveParams` `v2/ThreadAttachmentRemoveResponse` `v2/ThreadAttachmentUpdatedNotification` `v2/ThreadGoalMutationOrigin` `v2/ThreadItemsListAnchor` `v2/ThreadItemsListCursor` `v2/ThreadPredictionRequestParams` `v2/ThreadPredictionRequestResponse` `v2/ThreadPredictionResult` `v2/ThreadPredictionUpdatedNotification` `v2/ToolExposureSurface` `v2/UserVerificationCancelParams` `v2/UserVerificationCancelResponse` `v2/WindowsSandboxImplementation` `v2/WorkspaceRouting`

## Package and database decisions

The complete official x86_64 Linux package was downloaded and unpacked exclusively under `/tmp/codex-upgrade-audit/package`. Its manifest identifies layoutVersion 1 and entrypoint `bin/codex`. It contains `bin/codex-code-mode-host`, `codex-path/rg`, bundled `codex-resources/bwrap`, bundled zsh, and voice runtime/helper resources. Keep that structure. A bare executable previously produced a missing-code-mode-host warning; downloading that companion locally removed the warning. Do not mix helpers from different releases or alter the system installation to run compatibility tests.

A copy of the synthetic baseline home, containing three completed text/command/image turns, was opened by the candidate, resumed and paged. Stable turn/item IDs, status, content, command, output and exit codes matched; the target added only rootTurnId:null to these returned turns. After creating a project, section and persistent attachment, a second server startup retained their identities and memberships. Revert removed precisely the selected final turn and retained earlier turns.

All six synthetic SQLite databases returned integrity_check=ok. New recorded migrations: state_5.sqlite 55–59 (thread attachments, creator identity, guardian metadata cleanup, archive-sort indexes, attachment reverse lookup); memories_1.sqlite migration 2 (consolidation progress); thread_history_1.sqlite migrations 7–8 (item lifecycle timestamps and turn root). The other three database migration lists were unchanged. Evidence: `/tmp/codex-upgrade-audit/migration/db-inventory.json`, `migration/results-{0,1}.json`, `migration/loaded-history-{0,1}.json`, and `migration-test.log`. This proves this synthetic upgrade path, not every possible production database state.

Dummy API-key login created auth.json only in that temporary home; logout removed it. A local provider with requires_openai_auth=false correctly reports account:null even after dummy login, so that response was not incorrectly treated as proof of a real authenticated account. No credentials were copied, no external model calls were made, and no production authentication was exercised.

## Verification performed and implementation gates

| Evidence | Result and scope |
| --- | --- |
| Seven existing real-app-server transport fixtures against local 0.162.1 | Passed Unix, IPv4/IPv6, TLS IPv4/IPv6, WebSocket IPv4/IPv6 initialization/typed operations. |
| Local Responses fixture through installed bridge and real candidate | Passed create/stream/complete, history, opaque cursors, resume, local PNG and command wire approval. |
| Native Xvfb/offscreen widget probe against real candidate | Passed New thread, Send, streamed/completed graph and Changes tab at the recorded viewport/DPR. Partial-answer text retained. The approval Review dialog failed on both releases as described above. This is targeted qualification, not the full canonical 22-section suite. |
| Filesystem trace of preceding isolated core run | 171 per-process traces, zero accesses containing `/home/voc/.codex`. New checks likewise used explicit temporary homes/workspaces; they were not all independently traced. |
| Candidate regeneration in /tmp | Succeeded: counts/hashes above. C++ generated-header compilation, 170-method assertion and lossless phase-value smoke check passed. |
| Existing focused provider test executables | AnthropicProtocolTest, ResponsesAdapterTest, CodexAgentTest and HttpStreamingTest all exited 0, independently executed in parallel from /tmp. CTest could not write its log into the read-only AISuite build tree; direct execution ensured every test still ran. |
| Candidate TypeScript frontend SDK | Build/type tests plus all 44 tests passed. A separate semantic negative test failed and exposed the hybrid image schema bug. |
| Browser using candidate frontend SDK | TypeScript build and all 337 tests passed in a temporary copy with shared fixtures. No live browser session or complete interactive browser qualification was claimed. |
| Real candidate → AISuite Responses service → mock native Anthropic | Passed read/patch/test tools and final streamed answer with danger-full-access in a disposable project. No real Claude request. |
| Workspace-write coding turn under nested environment | Blocked: 0.162 uses fixed upstream `/tmp/codex-daemon-1000`, which this managed environment exposes with mode 000; it requires owner/mode 0700. Baseline fails with a synthetic bubblewrap mount lock read-only error. These are environment restrictions, not an AISuite-created directory. No shared directory was chmodded or repaired. |
| Synthetic persistence and dummy auth | Passed migration, restart, grouping, attachment, logout and revert. Production data untouched. |

The subsequent migration acceptance must run regenerated SDK/routing tests, current AISuite provider tests, the sandboxed real coding-turn test outside this nested restriction, native graph/timestamp/approval/settings tests, native/browser shared fixtures, browser artifact checks and the canonical native interactive qualification inventory. Native geometry checks retain DPR 1.0/1.25/1.5/2.0 and stable-widget/Copy/visibility requirements. New timestamp merges require stale-history-versus-live-notification regressions. No performance-sensitive renderer mechanism is proposed; existing benchmark limits remain the gate if phase presentation affects layout.

Real ChatGPT OAuth/token refresh, real OpenAI/Claude inference, account-specific gateway/Bedrock policies, every experimental feature and the complete native/browser interactive matrix are **not verified** by source audit or local fixtures. Their contracts and ownership decisions are resolved; deployment qualification remains mandatory. They cannot honestly be declared tested while honoring the instruction to avoid production authentication/data. Use a separately authorized test account/home when executing those gates.

## Reduction and change accounting

Delete rollback-only entries, timing mapping, updater condition and obsolete tests. Reshape the existing generator, approval policies, item hydration and phase DTOs rather than adding compatibility flags, retry paths, caches, timers, alternate renderers or a second protocol authority. Keep observer policy unchanged.

The uncorrected generator dry-run would replace the C++ header with +1,528/−97 physical lines (net +1,431) and TypeScript generated file with +574/−66 (net +508). These are automatic schema-surface growth, not exact proposed handwritten diff counts; fixing hybrid schema emission changes the final generated output. Mandatory catalog entries net +14 lines before formatting. Handwritten corrections should use existing policy/projection boundaries, with an initial estimate of 20–60 net lines if reduction cannot absorb validation, phase and wrapper handling; tests are expected to grow. Exact production/generated/test accounting must be measured from the concrete diff before implementation acceptance.

The user's engineering rule requires explicit approval before implementing unavoidable net production-code growth. This inventory does not grant or infer that approval and makes no production additions. The future diff must first attempt reduction/reshaping, quantify remaining growth and explain why the pinned generated surface and correctness cannot fit existing code. No approval question is necessary to finish this read-only inventory.

Current tracked production change: 0 lines. Current tracked test change: 0 lines. The only repository addition from this task is this inventory document; temporary generation, probes, package and logs reside in `/tmp/codex-upgrade-audit`. All migration design questions identified above have an explicit decision; no claim of production-upgrade readiness is made before the stated verification gates pass.
