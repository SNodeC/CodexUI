# Codex app-server protocol: complete framework-neutral C++ master data model

## Scope and reproducible baseline

This document models the complete wire surface reachable from the Codex app-server method registry at OpenAI Codex `main` commit [`305eed102d6ab5fc1228fec0737ba240eb29826b`](https://github.com/openai/codex/commit/305eed102d6ab5fc1228fec0737ba240eb29826b), committed 2026-08-31. It includes stable, method-level experimental, field-level experimental, deprecated compatibility, unstable, and internal-only messages. The canonical library design uses only standard C++ at its public boundary; Qt, Asio, CLI, storage, and transport integrations are adapters.

The latest published release at this pin is [`rust-v0.151.0`](https://github.com/openai/codex/releases/tag/rust-v0.151.0). It has the same 157 client requests and 11 server requests; `main` adds only `modelProvider/authRecoveryStarted` and `modelProvider/authRecoveryCompleted`, raising server notifications from 81 to 83. The executable installed while this document was produced is `codex-cli 0.144.6`; its generated schema is older and must not be confused with this baseline.

Authoritative inputs are the pinned [app-server README](https://github.com/openai/codex/blob/305eed102d6ab5fc1228fec0737ba240eb29826b/codex-rs/app-server/README.md), [method registry](https://github.com/openai/codex/blob/305eed102d6ab5fc1228fec0737ba240eb29826b/codex-rs/app-server-protocol/src/protocol/common.rs), [RPC envelopes](https://github.com/openai/codex/blob/305eed102d6ab5fc1228fec0737ba240eb29826b/codex-rs/app-server-protocol/src/rpc.rs), [v1 types](https://github.com/openai/codex/blob/305eed102d6ab5fc1228fec0737ba240eb29826b/codex-rs/app-server-protocol/src/protocol/v1.rs), [v2 types](https://github.com/openai/codex/tree/305eed102d6ab5fc1228fec0737ba240eb29826b/codex-rs/app-server-protocol/src/protocol/v2), and the [generated JSON schemas](https://github.com/openai/codex/tree/305eed102d6ab5fc1228fec0737ba240eb29826b/codex-rs/app-server-protocol/schema/json).

“Complete” is version-relative. App-server has no negotiated protocol version: `clientInfo.version` is the client application's version. Bindings must be regenerated for the target Codex binary:

```bash
codex app-server generate-json-schema --out DIR --experimental
codex app-server generate-ts --out DIR --experimental
```

Omit `--experimental` for the stable surface. The checked-in JSON intentionally omits three accepted v1 client methods (`getConversationSummary`, `gitDiffToRemote`, `getAuthStatus`) and two internal notifications (`rawResponseItem/completed`, `rawResponse/completed`); therefore the Rust method registry, not a generated public bundle alone, defines the complete runtime union.

## Result

The narrowest complete model has exactly three layers:

1. An append-only wire journal preserves ordering, reverse requests, transient-only facts, unknown fields, and exact request intent.
2. A bidirectional exchange ledger correlates every response with the request that supplies its missing method, scope, filters, and identifiers.
3. Normalized materialized views expose current state without duplicating authoritative data.

A current-state tree by itself is insufficient: many successful mutations return `{}`, response envelopes contain no method, request IDs are bidirectional and connection-scoped, pages are query-scoped, some notifications are invalidations only, and several stream events are never replayed.

## Wire contract

- The transport is JSON-RPC-like but deliberately omits `"jsonrpc":"2.0"`.
- A request is `{id: string|int64, method, params?, trace?}`; `trace` is optional W3C trace context.
- A notification is `{method, params?}`.
- Success is `{id, result}`; failure is `{id, error:{code:int64,message,data?}}`.
- Current server notifications are flattened with optional `emittedAtMs`; current servers populate it, old servers may omit it.
- `stdio` uses one JSON object per line. Experimental WebSocket uses one object per text frame. Unix sockets use WebSocket over HTTP upgrade.
- Ingress saturation returns code `-32001`, message `Server overloaded; retry later.`
- Each connection must send exactly one `initialize` request, receive its response, then send the sole client notification `initialized`. Other pre-handshake requests fail.
- `initialize.capabilities` fixes `experimentalApi`, `requestAttestation`, exact notification opt-outs, and MCP extensions for that connection.
- Transport arrival order is reducer order. `emittedAtMs` is diagnostic provenance, never an identity or sequence.

### Surface counts and tier notation

| Direction                        | Count | Meaning                                                                    |
| -------------------------------- | ----: | -------------------------------------------------------------------------- |
| Client request → server response |   157 | 101 not method-gated, 56 method-level experimental                         |
| Server request → client response |    11 | 10 not method-gated, one experimental                                      |
| Server notification → client     |    83 | 61 non-method-gated, 22 annotated experimental; includes two internal-only |
| Client notification → server     |     1 | `initialized`                                                              |

`S` = available without method-level experimental opt-in; `X` = method-level experimental; `XF` = stable method with gated fields or variants; `U` = unstable; `D` = deprecated; `I` = internal-only; `C` = capability/flow conditional.

## Container and type rules

- Use `std::string`-backed opaque IDs, not a framework UUID type. Codex-generated IDs may currently be UUIDv7, but the wire contract is `string` and also carries legacy, provider, client-supplied, and test IDs.
- Use `std::unordered_map` for lookup, `std::vector` for all protocol order, and `std::unordered_set` only for semantic sets.
- Use `std::variant` for tagged unions and `std::optional` for nullable values.
- Use a tri-state `Patch<T>` for omitted/null/value request fields. Omitted and null have different meanings in many filters and updates.
- Use library-owned `JsonValue`/`JsonObject` wrappers for declared open JSON: tool arguments/results, schemas, metadata, MCP content, config values, and unknown extensions. Do not expose the selected JSON parser in the stable API.
- Keep toolkit-specific dynamic values at adapter/view-model boundaries, not in canonical protocol state.
- Preserve remote and environment-native paths as opaque strings. Do not feed them to local filesystem normalization utilities.
- Keep timestamp units in names: persisted thread/turn times are seconds; item lifecycle and notification emission times are milliseconds.
- Keep raw upstream `ResponseItem` history separate from normalized `ThreadItem` UI/history state.

## Master structure

The code below is structural C++20 pseudocode, not a replacement for generated wire DTO declarations. A `wire::*` name is either the corresponding generated schema DTO or a lossless local projection named by purpose (`ThreadHead` = Thread without its embedded-turn relation, for example). Generate the exact DTO members from schema and give every tagged union an `Unknown` alternative for forward compatibility.

```cpp
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

using ByteBuffer = std::vector<std::byte>;
using CanonicalScopeKey = std::string; // canonical UTF-8 JSON bytes

// Complete, deeply immutable value wrappers. Their hidden node may use any JSON library.
namespace detail { struct JsonNode; }
class JsonValue {
    std::shared_ptr<const detail::JsonNode> node_;
};
class JsonObject {
    JsonValue value_; // construction validates that value_ is an object
};

template<class Tag>
struct OpaqueId {
    std::string wire;
    friend bool operator==(const OpaqueId&, const OpaqueId&) = default;
};

// Implementations pass explicit library-owned Hash<T> functors for opaque/composite keys;
// the structural unordered_map declarations below omit those template arguments.

using ConnectionId  = std::uint64_t; // client-owned reconnect epoch
using InputSeq       = std::uint64_t; // every serialized engine input
using FrameSeq       = std::uint64_t; // every inbound or outbound journal record
using ExchangeId     = std::uint64_t;
using OperationId    = std::uint64_t;
using EffectId       = std::uint64_t;
using QueryGeneration = std::uint64_t;
using ThreadId       = OpaqueId<struct ThreadTag>;
using SessionId      = OpaqueId<struct SessionTag>;
using TurnId         = OpaqueId<struct TurnTag>;
using ItemId         = OpaqueId<struct ItemTag>;
using ProjectId      = OpaqueId<struct ProjectTag>;
using SectionId      = OpaqueId<struct SectionTag>;
using EnvironmentId  = OpaqueId<struct EnvironmentTag>;
using PluginId       = OpaqueId<struct PluginTag>;
using AppId          = OpaqueId<struct AppTag>;

struct RequestId {
    std::variant<std::int64_t, std::string> value; // numeric 1 != string "1"
    friend bool operator==(const RequestId&, const RequestId&) = default;
}; // use an explicit RequestIdHash

enum class PatchKind { Omitted, Null, Value };
template<class T> struct Patch {
    PatchKind kind = PatchKind::Omitted;
    std::optional<T> value;
};

enum class Presence { Unknown, Null, Value };
enum class EvidenceKind {
    DirectPayload,
    SuccessfulMutation,
    QueryMembership,
    StreamDelta,
    Invalidation
};

struct SourceRef {
    FrameSeq frame = 0;
    EvidenceKind evidence = EvidenceKind::DirectPayload;
};

template<class T> struct Fact {
    Presence presence = Presence::Unknown;
    std::optional<T> value; // engaged exactly when presence == Value
    bool stale = false;
    std::optional<SourceRef> source;
};

enum class Endpoint { Client, Server };
enum class FrameKind { Request, Notification, SuccessResponse, ErrorResponse };
enum class FrameDisposition { Received, Prepared, Queued, Written, WriteFailed, Indeterminate };

struct RpcError {
    std::int64_t code = 0;
    std::string message;
    std::optional<JsonValue> data;
};

struct DecodeError {
    std::string message;
};

using DecodeOutcome = std::variant<wire::DecodedMessage, DecodeError>;

struct WireFrame {
    FrameSeq seq = 0;
    ConnectionId connection = 0;
    Endpoint sender = Endpoint::Server;
    FrameDisposition disposition = FrameDisposition::Received;
    std::optional<EffectId> writeEffect;
    std::optional<FrameKind> kind;
    std::optional<RequestId> requestId;
    std::optional<std::string> method; // recovered from ledger for responses
    std::optional<std::int64_t> emittedAtMs;
    std::int64_t recordedAtMs = 0;
    ByteBuffer rawJson; // byte-exact, unknown-field-safe
    std::optional<JsonObject> envelope;
    DecodeOutcome decode;
};

struct RequestSlot {
    ConnectionId connection = 0;
    Endpoint requester = Endpoint::Client;
    RequestId id;
    friend bool operator==(const RequestSlot&, const RequestSlot&) = default;
}; // use an explicit RequestSlotHash

template<class WireKey> struct IncarnationKey {
    ConnectionId connection = 0;
    WireKey wireKey;
    ExchangeId createdBy = 0;
    friend bool operator==(const IncarnationKey&, const IncarnationKey&) = default;
}; // use an explicit IncarnationKeyHash<WireKey>

enum class WriteState { Prepared, Queued, Written, Failed, Unknown };
enum class RpcState { Pending, Succeeded, Failed, Resolved, Abandoned };
enum class OperationState { Prepared, Active, Succeeded, Failed, Resolved, Indeterminate };
enum class ObservationState { None, Awaiting, Observed, Indeterminate };

struct Exchange {
    ExchangeId id = 0;
    RequestSlot slot;
    std::string method;
    FrameSeq requestFrame = 0;
    std::optional<FrameSeq> responseFrame;
    std::optional<FrameSeq> resolvedNotificationFrame;
    std::optional<WriteState> requestWrite;  // client-origin request
    std::optional<WriteState> responseWrite; // client reply to a server-origin request
    RpcState rpcState = RpcState::Pending;
    JsonValue params;
    std::optional<JsonValue> result;
    std::optional<RpcError> error;
    std::vector<wire::EntityRef> relatedEntities;
};

struct Operation {
    OperationId id = 0;
    std::vector<ExchangeId> exchanges;
    OperationState state = OperationState::Prepared;
    ObservationState observation = ObservationState::None;
    std::optional<RpcError> error;
};

struct WireJournal {
    std::vector<WireFrame> records; // privileged, append-only lifecycle records
};

struct ProtocolLedger {
    std::vector<ExchangeId> exchangeOrder;
    std::unordered_map<ExchangeId, Exchange> exchanges;
    std::unordered_map<RequestSlot, ExchangeId> pending;
    std::vector<OperationId> operationOrder;
    std::unordered_map<OperationId, Operation> operations;
};

enum class ConnectionPhase { Disconnected, Connecting, Initializing, Ready, Closing };

struct ExchangeProjection {
    ExchangeId id = 0;
    RequestSlot slot;
    std::string method;
    std::optional<WriteState> requestWrite;
    std::optional<WriteState> responseWrite;
    RpcState rpcState = RpcState::Pending;
    bool payloadProtected = false;
    std::vector<wire::EntityRef> relatedEntities;
};

struct OperationProjection {
    OperationId id = 0;
    std::vector<ExchangeId> exchanges;
    OperationState state = OperationState::Prepared;
    ObservationState observation = ObservationState::None;
};

struct InteractionToken {
    RequestSlot slot;
    ExchangeId exchange = 0;
};

struct PendingInteraction {
    InteractionToken token;
    std::string method;
    JsonValue redactedParams; // protected fields become explicit placeholders
    std::vector<wire::EntityRef> relatedEntities;
};

struct ProtocolState {
    ConnectionId connection = 0;
    ConnectionPhase phase = ConnectionPhase::Disconnected;
    Fact<wire::InitializeParams> clientProfile;
    Fact<wire::InitializeResponse> serverProfile;
    Fact<bool> initializedAcknowledged;
    std::unordered_set<std::string> optedOutNotifications;
    std::unordered_map<ExchangeId, ExchangeProjection> exchanges;
    std::unordered_map<OperationId, OperationProjection> operations;
    std::unordered_map<ExchangeId, PendingInteraction> pendingInteractions;
};
```

The ledger key includes connection and requester because IDs can be strings or integers, can be reused after reconnect/completion, and client-origin and server-origin requests may use the same value. `serverRequest/resolved` can clear a server request after a response or after lifecycle cleanup. Raw frames and exact exchange params/results remain in separately authorized stores; public state exposes only redacted projections and source IDs.

### Conversation, catalog, and runtime views

```cpp
enum class Coverage { Unknown, Partial, Complete };

template<class Id> struct OrderedRelation {
    std::vector<Id> ids;
    Coverage coverage = Coverage::Unknown;
    std::vector<ExchangeId> pageEvidence;
};

struct ItemState {
    ItemId id;
    Fact<TurnId> ownerTurn;              // known-null only for old unscoped history
    Fact<wire::ThreadItem> started;
    Fact<wire::ThreadItem> completed;    // authoritative final item
    Fact<std::int64_t> startedAtMs;
    Fact<std::int64_t> completedAtMs;
    std::vector<FrameSeq> deltaFrames;       // journal owns the bytes
};

struct TurnState {
    TurnId id;
    Fact<wire::TurnHead> head;            // Turn without embedded items
    wire::TurnItemsView bestItemsView = wire::TurnItemsView::NotLoaded;
    OrderedRelation<ItemId> itemOrder;
    std::unordered_map<ItemId, ItemState> items;

    Fact<std::string> unifiedDiff;
    Fact<wire::TurnPlan> plan;
    Fact<JsonValue> moderationMetadata;
    Fact<wire::ThreadTokenUsage> tokenUsage;
    Fact<wire::ModelRoute> modelRoute;
    Fact<std::vector<wire::ModelVerification>> modelVerifications;
    Fact<wire::ModelSafetyBuffering> safetyBuffering;
    std::vector<std::string> hookRunIds;
    std::vector<std::string> guardianReviewIds;
    std::vector<FrameSeq> rawResponseFrames;
};

struct TimelineState {
    // Same-position entries retain response order.
    std::map<std::uint64_t, std::vector<wire::TimelineReference>> byPosition;
    std::vector<ExchangeId> pageEvidence;
    Coverage coverage = Coverage::Unknown;
};

struct ThreadState {
    ThreadId id;
    Fact<wire::ThreadHead> descriptor;    // Thread without embedded turns

    // Lifecycle/index facts not contained in wire::Thread.
    Fact<bool> archived;
    Fact<bool> deleted;
    Fact<bool> loaded;
    Fact<bool> subscribedOnThisConnection;
    Fact<bool> closed;

    Fact<wire::ThreadStatus> status;
    Fact<std::string> name;                   // Presence::Null means explicitly unnamed
    Fact<ProjectId> project;              // Presence::Null means unassigned
    Fact<wire::ThreadSection> section;    // Presence::Null means unsectioned
    Fact<wire::ThreadSettings> settings;
    Fact<wire::ThreadGoal> goal;           // Presence::Null means no goal
    Fact<wire::ThreadMemoryMode> memoryMode;
    Fact<wire::ThreadTokenUsage> tokenUsage;

    OrderedRelation<TurnId> turnOrder;
    std::unordered_map<TurnId, TurnState> turns;
    OrderedRelation<std::string> queueOrder;  // QueuedSubmission.id
    std::unordered_map<std::string, wire::QueuedSubmission> queue;
    TimelineState timeline;

    std::unordered_map<EnvironmentId, Fact<bool>> environmentConnections;
    std::unordered_map<std::string, wire::BackgroundTerminal> backgroundTerminals;
    std::unordered_map<std::string, wire::HookRunSummary> hookRuns;
    std::unordered_map<std::string, wire::GuardianReview> guardianReviews;
    std::unordered_map<std::string, wire::RealtimeItem> realtimeItems;
    Fact<wire::RealtimeSession> liveRealtime;
};

template<class T> struct Entity {
    T value;
    SourceRef firstSeen;
    SourceRef lastChanged;
    bool stale = false;
    bool tombstoned = false;
};

struct QueryKey {
    std::string method;
    CanonicalScopeKey canonicalScopeParams; // filters/sort/scope, excluding cursor/limit
    friend bool operator==(const QueryKey&, const QueryKey&) = default;
}; // use an explicit QueryKeyHash

struct QueryIncarnationKey {
    QueryKey query;
    QueryGeneration generation = 0;
    friend bool operator==(const QueryIncarnationKey&, const QueryIncarnationKey&) = default;
}; // use an explicit QueryIncarnationKeyHash

struct QueryView {
    QueryIncarnationKey key;
    std::vector<wire::QueryRowRef> orderedRows; // entity/match/result refs in wire order
    std::vector<ExchangeId> pages;
    Coverage coverage = Coverage::Unknown;
    bool stale = false;
};

template<class Id> struct ScopedKey {
    CanonicalScopeKey scope;
    Id id;
    friend bool operator==(const ScopedKey&, const ScopedKey&) = default;
}; // use an explicit ScopedKeyHash<Id>

using FeatureKey = ScopedKey<std::string>;
using PermissionProfileKey = ScopedKey<std::string>;
using PluginCatalogKey = ScopedKey<std::string>;
using AppCatalogKey = ScopedKey<std::string>;
using SkillCatalogKey = ScopedKey<std::string>;
using HookCatalogKey = ScopedKey<std::string>;
using McpServerKey = ScopedKey<std::string>;

struct CatalogState {
    std::unordered_map<ProjectId, Entity<wire::Project>> projects;
    OrderedRelation<ProjectId> projectOrder;
    std::unordered_map<SectionId, Entity<wire::ThreadSection>> sections;
    OrderedRelation<SectionId> sectionOrder;

    std::unordered_map<std::string, Entity<wire::Model>> modelsByCatalogId;
    std::unordered_map<FeatureKey, Entity<wire::ExperimentalFeature>> features;
    std::unordered_map<PermissionProfileKey, Entity<wire::PermissionProfileSummary>> permissionProfiles;
    std::unordered_map<std::string, Entity<wire::CollaborationModeMask>> collaborationModes;

    std::unordered_map<PluginCatalogKey, Entity<wire::PluginAggregate>> plugins;
    std::unordered_map<std::string, Entity<wire::Marketplace>> marketplaces;
    std::unordered_map<AppCatalogKey, Entity<wire::AppAggregate>> apps;
    std::unordered_map<SkillCatalogKey, Entity<wire::SkillMetadata>> skills;
    std::unordered_map<HookCatalogKey, Entity<wire::HookMetadata>> hooks;
    std::unordered_map<McpServerKey, Entity<wire::McpServerStatus>> mcpServers;
};

struct RuntimeState {
    std::unordered_map<IncarnationKey<std::string>, wire::CommandProcessState> commandProcesses;
    std::unordered_map<IncarnationKey<std::string>, wire::ProcessState> processes;
    std::unordered_map<IncarnationKey<std::string>, wire::FsWatchState> watches;
    std::unordered_map<IncarnationKey<std::string>, wire::FuzzySearchState> fuzzySearches;
    std::unordered_map<IncarnationKey<std::string>, wire::McpEventStreamState> mcpStreams;
    std::unordered_map<std::string, wire::ExternalImportState> imports; // importId
    std::unordered_map<EnvironmentId, Entity<wire::EnvironmentState>> environments;
    Fact<wire::RemoteControlState> remoteControl;
    Fact<wire::WindowsSandboxReadiness> windowsSandbox;
    Fact<wire::ServerDiagnosticsResponse> diagnostics;
};

struct AccountState {
    Fact<wire::Account> account;          // Presence::Null means signed out
    Fact<bool> requiresOpenAiAuth;
    Fact<wire::AuthProjection> projection;
    Fact<wire::RateLimits> rateLimits;
    Fact<wire::AccountTokenUsage> usage;
    Fact<wire::WorkspaceMessages> workspaceMessages;
    std::unordered_map<std::string, wire::LoginAttempt> loginAttempts;
};

struct ConfigState {
    std::unordered_map<CanonicalScopeKey, Entity<wire::ConfigReadResponse>> effectiveByScope;
    Fact<wire::ConfigRequirements> requirements;
};

struct MasterState {
    ProtocolState protocol;
    AccountState account;
    ConfigState config;
    CatalogState catalogs;
    std::unordered_map<ThreadId, ThreadState> threads;
    RuntimeState runtime;
    std::unordered_map<QueryIncarnationKey, QueryView> queries;
    std::vector<wire::DiagnosticNotice> notices;
};

struct OptimisticState; // operation-keyed overlays; never authoritative facts
struct DerivedIndexes;  // reproducible from forward keys
struct ClientExtensionState; // typed client-owned domain facts, never widget state
struct RedactedMasterState;
struct PublicOptimisticState;
struct PublicDerivedIndexes;
struct PublicClientExtensionState;
struct EngineEventLog;
struct TransactionalOutbox;
struct StoreMetadata;

struct CanonicalApplicationState {
    MasterState confirmed;
    std::shared_ptr<const OptimisticState> optimistic;
    std::shared_ptr<const DerivedIndexes> derived;
    std::shared_ptr<const ClientExtensionState> extensions;
};

struct PublicApplicationState {
    std::shared_ptr<const RedactedMasterState> confirmed;
    std::shared_ptr<const PublicOptimisticState> optimistic;
    std::shared_ptr<const PublicDerivedIndexes> derived;
    std::shared_ptr<const PublicClientExtensionState> extensions;
};

struct CanonicalStoreState {
    WireJournal journal;       // privileged exact protocol evidence
    ProtocolLedger ledger;     // privileged exact request/result correlation
    std::shared_ptr<const EngineEventLog> engineEvents;
    std::shared_ptr<const TransactionalOutbox> outbox;
    std::shared_ptr<const StoreMetadata> metadata;
    CanonicalApplicationState canonicalState;
    PublicApplicationState publicState;
};
```

Connection-scoped identifiers (`command/exec.processId`, `processHandle`, `watchId`, fuzzy `sessionId`, MCP `subscriptionId`) can be reused. Their key is therefore the explicit `(connection, wire key, creation exchange)` `IncarnationKey`, never a concatenated string.

Catalogs are query-context-sensitive. Skills/hooks vary by cwd; apps and MCP by optional thread; features and permission profiles by scope; plugins by cwd, marketplace kind, and filters; config by optional cwd. A single unqualified “latest list” is incorrect. Query method and canonical scope prevent cross-method collisions; generation identifies a cursor chain so late pages cannot update a newer incarnation.

## Core normalized DTOs

`Thread` supplies: `id`, `extra`, `sessionId`, `forkedFromId`, `parentThreadId`, `preview`, `ephemeral`, `section`, `sectionEnteredAt`, `projectId`, `historyMode`, `modelProvider`, `createdAt`, `updatedAt`, `recencyAt`, `status`, `path`, `cwd`, `cliVersion`, `source`, `canAcceptDirectInput`, `threadSource`, `agentNickname`, `agentRole`, `gitInfo`, `name`, and optionally populated `turns`.

`Turn` supplies: `id`, `items`, `itemsView` (`notLoaded|summary|full`), `status` (`inProgress|completed|interrupted|failed`), nullable `error`, `startedAt`, `completedAt`, and `durationMs`.

The 19 `ThreadItem` variants are:

| Type                  | Payload beyond `id`                                                                                                                                       |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `userMessage`         | `clientId?`, ordered `content: UserInput[]`                                                                                                               |
| `hookPrompt`          | `fragments[]`                                                                                                                                             |
| `agentMessage`        | `text`, `phase?`, `memoryCitation?`, `delivery?`                                                                                                          |
| `functionCallOutput`  | `name`, `namespace?`, `output`                                                                                                                            |
| `plan`                | `text`                                                                                                                                                    |
| `reasoning`           | `summary[]`, `content[]`                                                                                                                                  |
| `commandExecution`    | `pluginId?`, `scriptPath?`, `command`, `cwd`, `processId?`, `source`, `status`, `commandActions[]`, `aggregatedOutput?`, `exitCode?`, `durationMs?`       |
| `fileChange`          | `changes[]`, `status`                                                                                                                                     |
| `mcpToolCall`         | `server`, `tool`, `status`, `arguments`, `appContext?`, deprecated `mcpAppResourceUri?`, `pluginId?`, `readOnlyHint?`, `result?`, `error?`, `durationMs?` |
| `dynamicToolCall`     | `namespace?`, `tool`, `arguments`, `status`, `contentItems?`, `success?`, `durationMs?`                                                                   |
| `collabAgentToolCall` | `tool`, `status`, `senderThreadId`, `receiverThreadIds[]`, `prompt?`, `model?`, `reasoningEffort?`, `agentsStates`                                        |
| `subAgentActivity`    | `kind`, `agentThreadId`, `agentPath`                                                                                                                      |
| `webSearch`           | `query`, `action?`, `results?`                                                                                                                            |
| `imageView`           | `path`                                                                                                                                                    |
| `sleep`               | `durationMs`                                                                                                                                              |
| `imageGeneration`     | `status`, `revisedPrompt?`, `result`, `transparentBackground?`, `savedPath?`, `failure?`                                                                  |
| `enteredReviewMode`   | `review` label                                                                                                                                            |
| `exitedReviewMode`    | final `review` text                                                                                                                                       |
| `contextCompaction`   | no additional fields                                                                                                                                      |

`UserInput` is a tagged union of `text{text,text_elements[]}`, `image{url,detail?}`, `localImage{path,detail?}`, `audio{url}`, `localAudio{path}`, `skill{name,path}`, and `mention{name,path}`.

Raw upstream `ResponseItem` is a different union used by injected/raw history. Keep it in the journal or a separate raw-history cache; never merge it into `ThreadItem` merely because some variants look similar.

## Derived relations

Store forward keys once. Reverse maps are rebuildable indexes, not independent truth.

| Relation                 | Derivation                                                                                                                                         |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Session membership       | `Thread.sessionId → Session`; group threads with the same value                                                                                    |
| Fork graph               | `Thread.forkedFromId → Thread`                                                                                                                     |
| Spawn/subagent graph     | `Thread.parentThreadId → Thread`                                                                                                                   |
| Project membership       | `Thread.projectId → Project.id`                                                                                                                    |
| Section membership/order | `Thread.section.id → ThreadSection.id`; `sectionEnteredAt` belongs to the edge                                                                     |
| Conversation containment | Response/notification context establishes `Thread → Turn → Item`; current `thread/items/list` returns `{turnId,item}`                              |
| Queue                    | One ordered `QueuedSubmission[]` per thread, keyed by submission ID; `clientUserMessageId` is a separate correlation key                           |
| RPC correlation          | `(connection, requester, typed requestId) → Exchange`; the request contributes method/scope and the response contributes result/error              |
| Query membership         | Canonical params + method → ordered result rows → entity/match references; cursor pages contribute evidence to that one view                       |
| Review                   | `review/start` source thread/target → returned `reviewThreadId` and Turn; inline uses the source thread, detached creates another thread           |
| Reverse interaction      | Approval/input/elicitation/tool-call params → thread/turn/item or call; `approvalId` distinguishes repeated callbacks; response/resolved closes it |
| Collab                   | `senderThreadId`, `receiverThreadIds`, and `agentsStates` keys → threads                                                                           |
| Subagent activity        | `agentThreadId → Thread`                                                                                                                           |
| Memory citation          | cited `threadIds[] → Thread`                                                                                                                       |
| Guardian/hook            | review `targetItemId → Item`; hook prompt `hookRunId → HookRun`; run notification → thread and optional turn                                       |
| Plugin attribution       | command/MCP item, skill, hook, and MCP-server `pluginId → PluginSummary.id`                                                                        |
| App attribution          | MCP `appContext.connectorId → AppInfo.id`; retain `linkId` and `resourceUri`                                                                       |
| MCP resource provenance  | `originCallId → originating completed MCP call`                                                                                                    |
| Command/process streams  | Command-item or creation exchange → connection-scoped process incarnation → ordered output/interaction/exit events                                 |
| Filesystem watches       | Watch creation exchange + `watchId → path`; each `fs/changed.watchId` joins that incarnation                                                       |
| Model/settings           | runtime model string → `Model.model`; service tier is scoped to that model; active profile IDs join the permission catalog for the same cwd        |
| Environment              | thread selections and connect/disconnect notifications → `EnvironmentId`                                                                           |
| Remote control           | pairing/client rows → environment; clients key by `(environmentId,clientId)`                                                                       |
| Import                   | progress/completion → request result `importId`                                                                                                    |
| Realtime/timeline        | Realtime session → realtime items; timeline position interleaves their references with ordinary `(turnId,itemId)` and turn boundaries              |

Do not collapse `sessionId`, `forkedFromId`, `parentThreadId`, or `threadSource`: they express different relations. Project roots and section/project positions are ordered data, not alphabetic sets.

## Availability and authority matrix

For a query, its scope and exchange outcome become known after either correlated response; result rows become authoritative only on success. For a mutation returning `{}`, the request parameters become confirmed intent only on success; broader side effects may still require a notification or refetch. Server requests use the reverse pair.

| Master state area                | Message(s) making it available                                                                                                    | Authority rule                                                                                                                |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| Wire journal                     | Every received frame and every prepared/queued/written/failed outbound frame lifecycle record                                     | Append raw input before decoding; reducer input order is canonical                                                            |
| Exchange/method/scope            | Any request supplies method/params; its same-slot response supplies result/error; `serverRequest/resolved` may close reverse flow | Only `(connection, requester, typed id)` is a safe join key                                                                   |
| Query view/order/coverage        | Query request parameters plus every correlated successful page response                                                           | Key by method, scope and generation; response order is meaningful and end cursor proves coverage                              |
| Connection/client/server profile | `initialize` request/result, then `initialized`                                                                                   | Capabilities are client-owned and fixed per connection; result gives `userAgent`, `codexHome`, `platformFamily`, `platformOs` |
| Full account                     | `account/read` result                                                                                                             | Authoritative account union plus `requiresOpenaiAuth`                                                                         |
| Auth projection/login            | Login/cancel/logout results; `account/updated`; `account/login/completed`                                                         | Notification is partial; refetch for email/full account                                                                       |
| Rate limits                      | `account/rateLimits/read`; `account/rateLimits/updated`                                                                           | Query is full; notification is sparse—merge present fields only                                                               |
| Usage/workspace messages         | `account/usage/read`; `account/workspaceMessages/read`                                                                            | Query-scoped snapshots                                                                                                        |
| Effective config/requirements    | `config/read`; `configRequirements/read`; config write results                                                                    | `configWarning` is diagnostic, not config state                                                                               |
| Threads and graph keys           | `thread/start`, `thread/resume`, `thread/fork`, list/read/search/unarchive/rollback/revert/metadata results; `thread/started`     | Merge descriptor fields; ordinary empty `turns` means not included                                                            |
| Lifecycle/loading/subscription   | Lifecycle requests; thread status/archive/delete/unarchive/close/revert notifications; `thread/loaded/list`                       | Archive/delete/load/subscription are facts beside `Thread`, not fields in it                                                  |
| Effective thread settings        | Start/resume/fork wrapper results; `thread/settings/updated`                                                                      | No general settings-read RPC; reconnect may leave fields unknown                                                              |
| Goal                             | `thread/goal/get`, `thread/goal/set`, `thread/goal/clear`; goal notifications                                                     | Exactly zero or one per thread                                                                                                |
| Queue                            | `thread/queue/*` results; `thread/queue/changed`                                                                                  | Changed notification invalidates only; refetch list                                                                           |
| Projects                         | `project/*` results; `project/changed`; `thread/project/updated`                                                                  | Project change normally requires refetch; assignment notification is exact                                                    |
| Sections                         | `threadSection/*`; embedded `Thread.section`; successful `thread/section/move`                                                    | No general cross-client section catalog notification                                                                          |
| Turns                            | `turn/start`, `review/start`, queue start, history-bearing Thread results, `thread/turns/list`, turn notifications                | `turn/completed` is status plus final-agent summary fallback, not a full item list                                            |
| Items                            | History, `thread/items/list`, `item/started`, `item/completed`, item deltas                                                       | `item/completed` is authoritative; use composite `(thread, turn, item)` identity                                              |
| Diff/plan/token/model signals    | `turn/diff/updated`, `turn/plan/updated`, `thread/tokenUsage/updated`, reroute/verification/safety/moderation notifications       | Diffs/plans replace; deltas append; model signals are transient                                                               |
| Guardian/review/hooks            | `review/start`; auto-review and hook notifications                                                                                | Keep separate from target item lifecycle                                                                                      |
| Pending server interactions      | 11 reverse request families + client response/error + `serverRequest/resolved`                                                    | Key by `RequestSlot`, not item ID; one item can have several `approvalId`s                                                    |
| Models/features/profiles/modes   | `model/list`, provider capabilities, feature/profile/mode lists and set results                                                   | Query-context catalogs; no catalog-change notification                                                                        |
| Skills/hooks                     | `skills/list`, `hooks/list`, `skills/changed`                                                                                     | Group by cwd; skills change invalidates, it does not supply rows                                                              |
| Plugins/marketplaces/apps        | Marketplace/plugin/app query/action results; `app/list/updated`                                                                   | Preserve scope; installed app runtime state is distinct from metadata                                                         |
| MCP                              | Status list, reload/OAuth/resource/tool/event calls; MCP notifications                                                            | Startup notification patches runtime status only, not inventory                                                               |
| Environment/remote control       | Environment and remote-control calls; connection/status notifications                                                             | Connection changes are transient, not replayed                                                                                |
| Standalone commands/processes    | `command/exec*`, `process/*`, output/exit events                                                                                  | Key by connection incarnation; final command response follows its deltas                                                      |
| Filesystem                       | `fs/*`; `fs/changed`                                                                                                              | Reads are query results; watch state is connection-scoped                                                                     |
| Realtime/timeline                | Realtime calls/events; `thread/timeline/list`                                                                                     | Realtime items are not ordinary Turn items; timeline is the mixed ordering projection                                         |
| Fuzzy search                     | Legacy search or session calls/events                                                                                             | Completion schema carries only `sessionId`; retain query from prior state                                                     |
| External import                  | Detect/import/history results; progress/completed notifications                                                                   | Join by `importId`                                                                                                            |
| Diagnostics/notices              | Diagnostics result; JSON-RPC errors; error/warning/deprecation/config/Windows notifications                                       | Keep correlated RPC errors, turn errors, and diagnostics distinct                                                             |

## Reducer invariants

1. Preserve unknown, known-null, and known-value. Absence or notification opt-out never means false/empty.
2. Never clear cached turns because a newer `Thread.turns` is empty. That usually means “not included.”
3. Respect `Turn.itemsView`: `full > summary > notLoaded`; a lower-fidelity payload cannot erase a higher one.
4. `turn/started.items` is empty. `turn/completed.items` is only a final-agent fallback. Update status/timestamps/error and upsert that fallback; do not replace item order.
5. `item/completed` supersedes `item/started` and live deltas. A subagent activity item may complete after its turn completed.
6. Append message/plan/reasoning/output deltas in transport order. Patch and turn-diff updates are replacement snapshots.
7. Pages update only the exact filter/sort/scope view. Cursors are opaque and query-scoped; completeness is proven only by page coverage.
8. History hydration and deprecated rollback are explicitly lossy; absence is not deletion.
9. Archive/delete/close create lifecycle facts or tombstones. `closed` means unloaded, not deleted.
10. `thread/reverted`, `thread/queue/changed`, `project/changed`, and `skills/changed` can be invalidations rather than replacement payloads.
11. Successful empty results prove acceptance, not every resulting entity value.
12. Raw response usage, safety buffering, realtime media, environment connections, process streams, and warnings are transient and may be unrecoverable after disconnect.
13. Scrub or securely retain API keys, refreshed access tokens, attestation tokens, secret input, and elicitation content. A complete in-memory journal is security-sensitive.
14. Decode exact wire spelling. Most fields are camelCase, but config mirrors snake_case, `UserInput.text_elements` and some timeline boundary fields are snake_case, and `AuthMode` uses `apikey` while `Account.type` uses `apiKey`.

## Framework-neutral library architecture

The canonical implementation is a deterministic standard-C++ state engine, not a GUI model. Transport, persistence, scheduling, and presentation are replaceable adapters. One transaction owner—the store—assigns revisions, installs state, appends public commits, and inserts private effects into the outbox atomically.

| Component          | Responsibility                                                                                              | Must not own                                                           |
| ------------------ | ----------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| `codex-wire`       | Generated DTOs, exact envelope codec, method registry, unknown fallbacks                                    | State transitions or I/O                                               |
| `codex-engine`     | Pure command decisions, frame reduction, exchange correlation, derived relations                            | Mutable global state, threads, sockets, clocks, callbacks, persistence |
| `codex-store`      | Journal/event append, state installation, commit log, transactional outbox, snapshots, cursors, checkpoints | Transport execution or presentation                                    |
| `codex-runtime`    | Optional `std::jthread` actor, input/effect ports, waits, handshake driving, recovery and backpressure      | Protocol semantics                                                     |
| `codex-storage`    | Optional journal/checkpoint persistence and protected secret retention                                      | Reducer policy                                                         |
| Framework adapters | Qt, Asio, CLI, daemon, tests, transports and view projections                                               | Authoritative canonical state                                          |

```text
incoming frame/action/timer/transport outcome
                    │
                    ▼
          one serialized store transaction
          ├─ append deterministic input/raw evidence
          ├─ run pure reducer
          ├─ install canonical + redacted public state revision N
          ├─ append redacted public commit N
          └─ insert private effects into transactional outbox
                    │
             transaction committed
              ┌─────┴─────┐
              ▼           ▼
        revision cursors   effect interpreter ──> app-server/clock/diagnostics
```

The reducer performs no I/O and observes no ambient clock. Connection lifecycle, receive times, timer firings, recovery boundaries, and transport outcomes are explicit inputs.

```cpp
using Revision = std::uint64_t;
using StoreEpoch = std::uint64_t;
using TimerId = std::uint64_t;

struct RevisionToken {
    StoreEpoch epoch = 0;
    Revision revision = 0;
};

struct RawFrameInput {
    ConnectionId connection = 0;
    Endpoint sender = Endpoint::Server;
    ByteBuffer rawJson;
    std::int64_t observedAtMs = 0;
};

struct DecodedFrameInput {
    FrameSeq frame = 0; // references the already-appended WireFrame
    ConnectionId connection = 0;
    Endpoint sender = Endpoint::Server;
    DecodeOutcome decode;
    std::int64_t observedAtMs = 0;
};

using Action = std::variant<
    ProtocolCommand,       // query or mutation sent to app-server
    ServerRequestReply,    // consumes an exact pending reverse interaction
    ClientExtensionAction>;// typed client-owned domain facts, never widget state

struct SubmittedAction {
    OperationId operation = 0;
    Action action;
};

using StoreInput = std::variant<
    BeginConnection,
    RawFrameInput,
    SubmittedAction,
    EffectResult,
    TimerFired,
    ReplayCompleted,
    EndConnection>;

using EngineInput = std::variant<
    BeginConnection,
    DecodedFrameInput,
    SubmittedAction,
    EffectResult,
    TimerFired,
    ReplayCompleted,
    EndConnection>;

struct InputRecord {
    InputSeq seq = 0;
    EngineInput input; // decoded once by the store; persisted for exact reducer replay
};

struct CauseRef {
    InputSeq input = 0;
    std::optional<FrameSeq> frame;
    std::optional<OperationId> operation;
    std::optional<EffectId> effect;
    std::optional<TimerId> timer;
};

using CanonicalChange = std::variant<
    FactChanged,
    EntityUpserted,
    EntityTombstoned,
    OrderedRelationChanged,
    QueryViewChanged,
    ExchangeChanged,
    InteractionChanged,
    ConnectionChanged,
    StreamAppended,
    UnknownExtensionObserved>;

struct PublicStateChange {
    ChangeKind kind;
    std::optional<wire::EntityRef> entity;
    FieldMask changedFields;
    ByteBuffer redactedPayload; // decoded by versioned typed adapters
};

using PrivateEffect = std::variant<
    SendFrame,
    ArmTimer,
    CancelTimer,
    DiagnosticEffect>;

struct Reduction {
    std::shared_ptr<const CanonicalApplicationState> stateAfter;
    std::vector<CanonicalChange> changes; // private, deterministic order
    std::vector<ProtocolLedgerMutation> ledgerChanges;
    std::vector<PrivateEffect> effects;
};

struct ReducerContext {
    std::shared_ptr<const ProtocolLedger> ledger;
    std::shared_ptr<const TransactionalOutbox> outbox;
};

class Reducer {
public:
    Reduction reduce(
        std::shared_ptr<const CanonicalApplicationState> before,
        const ReducerContext&,
        const InputRecord&) const;
};
```

All nested state, including JSON wrappers, is deeply immutable or copy-on-write without mutable aliases. Unordered containers are lookup-only: hash iteration must never determine protocol order, emitted change order, effect order, or checkpoint encoding. Sort keys or retain explicit order vectors.

### Atomic store and transactional outbox

The store is the sole transaction owner. For a `RawFrameInput` it assigns `FrameSeq`, appends raw bytes before decoding, parses exactly once, and creates a `DecodedFrameInput` that references the journal record. Decode failures are ordinary reducer inputs. The transaction then reduces with immutable ledger/outbox context, applies returned ledger mutations, installs canonical state plus its redacted public projection, appends one public commit, and inserts every private effect before publishing the revision.

```cpp
struct PublicTransition {
    RevisionToken before;
    RevisionToken after;
    std::vector<CauseRef> causes;
    std::vector<PublicStateChange> changes; // redacted and ordered
};

enum class OutboxState { Queued, Claimed, Completed, Failed, Unknown };

struct OutboxEntry {
    EffectId id = 0;
    ConnectionId connection = 0;
    std::optional<OperationId> operation;
    std::optional<ExchangeId> exchange;
    std::optional<FrameSeq> preparedFrame;
    OutboxState state = OutboxState::Queued;
    std::string claimOwner;
    std::uint64_t claimGeneration = 0;
    std::optional<std::int64_t> claimedAtMs;
    PrivateEffect effect; // authorized interpreter only
};

struct ApplyReceipt {
    InputSeq input = 0;
    RevisionToken revision;
    std::optional<OperationId> operation;
};

struct SnapshotResult;

class StateStore {
public:
    ApplyReceipt apply(StoreInput); // converts raw frames to persisted EngineInput

    SnapshotCursor openSnapshotCursor(CursorPolicy) const;
    SnapshotResult snapshotAt(RevisionToken) const;
    CursorPage read(const ChangeCursor&, std::size_t maximumCommits) const;
    AckResult acknowledge(ChangeCursor&, Revision through);

    EffectClaim claimNextEffect(std::string_view claimant, std::stop_token);
    void reportEffect(
        EffectId,
        std::uint64_t claimGeneration,
        ConnectionId,
        EffectOutcome); // serialized back through apply(EffectResult)
};
```

A bounded notification queue may delay an interpreter but must never lose a committed effect. The outbox entry remains until acknowledged. Claim ownership, generation and time allow recovery from a dead interpreter; a claimed send whose non-execution cannot be proven becomes `Unknown`, never an automatic retry. A crash after a possible write changes the frame/write and operation state to `Unknown`/`Indeterminate`; non-idempotent mutations are reconciled, never blindly resent.

Outbound lifecycle is explicit:

```text
Prepared → Queued → Written | WriteFailed | Indeterminate
                       │
                       └─ correlated response → Succeeded | Failed | Resolved
```

“Prepared” records intent and exact bytes; only the correlated transport outcome proves that the live connection accepted the write. Socket write success still does not prove RPC success.

Checkpoint scheduling belongs to the store/runtime retention policy, not to the protocol reducer.

### Revision-cursor observation

Callbacks are optional conveniences, not the completeness boundary. The normative consumer API is an atomic redacted snapshot plus an ordered revision cursor:

```cpp
struct StateSnapshot {
    RevisionToken revision;
    std::shared_ptr<const PublicApplicationState> state;
};

struct SnapshotResult {
    std::optional<StateSnapshot> snapshot;
    std::optional<ReplayGap> gap;
};

enum class CursorRetention { PinUntilAcknowledged, GapAllowed };

class ChangeCursor {
public:
    ChangeCursor(ChangeCursor&&) noexcept;
    ChangeCursor& operator=(ChangeCursor&&) noexcept;
    ChangeCursor(const ChangeCursor&) = delete;

private:
    StoreEpoch epoch_ = 0;
    Revision next_ = 0;
    CursorRetention retention_ = CursorRetention::GapAllowed;
    CursorLease lease_;
    friend class StateStore;
};

struct SnapshotCursor {
    StateSnapshot snapshot;
    ChangeCursor cursor; // positioned immediately after snapshot.revision
};

struct CursorPage {
    std::vector<PublicTransition> commits;
    Revision throughRevision = 0;
    std::optional<ReplayGap> gap;
};
```

Each consumer owns its cursor, acknowledgement and backpressure policy. `openSnapshotCursor` is one locked transaction, so no commit can fall between the snapshot and cursor. Slow pinning cursors apply configured producer/storage backpressure; gap-allowed cursors receive an explicit `ReplayGap{checkpointRevision}` after compaction. A restored store has a new `StoreEpoch`, invalidating old cursors.

A wake primitive such as `waitForRevision(RevisionToken after, stop_token)` may coalesce notifications because the cursor remains authoritative. A queued consumer obtains the transition's exact retained state through `snapshotAt(transition.after)`; if that revision was compacted, it receives a gap and resets from a fresh atomic snapshot/cursor.

Three feeds remain distinct:

1. The privileged wire journal records every inbound frame and every outbound prepared/write lifecycle record, including malformed and unknown messages.
2. The privileged exchange/operation ledger records request and write lifecycles with exact params/results.
3. The redacted commit log records every canonical state transaction once.

One frame may produce zero canonical changes; one transaction may change several entities and relations. Conflating these feeds loses either protocol completeness, security separation, or transactional consistency.

### Action and authority boundary

The public write side submits intent; it never exposes `MasterState&`, field setters, or arbitrary patch lambdas.

```cpp
struct OperationReceipt {
    OperationId id = 0;
    RevisionToken acceptedAt;
};

enum class SubmitError { Invalid, NotReady, InvalidInteraction, Unauthorized, Saturated };

// A C++20 expected-equivalent owned by the library.
template<class T, class E> class Result;

class RuntimePort {
public:
    Result<OperationReceipt, SubmitError> submit(ProtocolCommand);
    Result<OperationReceipt, SubmitError> respond(InteractionToken, ServerRequestReply);
    Result<OperationReceipt, SubmitError> submitExtension(ClientExtensionAction);

    RevisionToken waitForRevision(RevisionToken after, std::stop_token);
};
```

`MasterState confirmed` changes only from protocol evidence, successful mutations with declared exact postconditions, and invalidations. `OptimisticState` remains a separate operation-keyed overlay. `ClientExtensionState` may hold application-owned domain data, but selection, drafts, expanded nodes, widget geometry, and other presentation state stay outside this library.

A successful empty result proves acceptance only. The logical operation may enter `ObservationState::Awaiting` until a notification or reconciliation query confirms the value. Later authoritative notifications rebase optimistic overlays; success reconciles them; failure removes them without overwriting newer confirmed facts.

A reverse interaction token becomes single-use when the reply action is accepted, but the reverse exchange's response write remains `Prepared` until written, failed, disconnected, or made obsolete by `serverRequest/resolved`. Every token and effect carries its connection generation.

Never automatically retry a mutation unless a generated method trait proves protocol-supported idempotency. The conservative default is `Never`.

### Handshake, reconnect and recovery

`BeginConnection` creates a fresh `ConnectionId` in `Initializing`, emits exactly one `initialize`, waits for its response, then emits `initialized` and enters `Ready`. Normal actions submitted earlier follow an explicit policy: queue until ready or fail as not ready; they are never forwarded pre-handshake.

Every timer, interaction, outbox entry and effect result carries `ConnectionId` and its own ID. Results from an older generation are journaled diagnostically but cannot mutate the new connection. `EndConnection` abandons pending RPC exchanges, invalidates interactions, and marks writes that might have crossed the transport boundary indeterminate.

Persistent recovery, when a storage adapter is configured, uses two distinct privileged logs:

- `WireJournal` preserves exact protocol evidence.
- `EngineEventLog` preserves all deterministic reducer inputs/domain events, including secret-bearing actions/replies, transport outcomes, timers and recovery boundaries.

A checkpoint atomically contains `CanonicalApplicationState`, its redacted public projection, protocol/operation ledger, complete nonterminal outbox entries and payloads, next-ID counters, `lastInputSeq`, `lastFrameSeq`, commit revision, outbox sequence, store epoch, schema fingerprint, codec version and reducer version. Replay applies engine events with effect interpretation disabled and links them to the retained `WireJournal` tail when byte-exact audit is required; decoded engine events cannot reconstruct raw bytes. If the raw tail was discarded, an implementation may build a separately labelled semantic diagnostic projection, never a replacement `WireJournal`. A final `ReplayCompleted` input classifies uncertain writes, rebuilds timer deadlines, invalidates connection-scoped tokens, and creates new reconciliation effects; historical effects are never executed directly.

Replaying with the same schema/codec/reducer version must reproduce identical state and public transitions. A newer projector may intentionally migrate or rebuild different normalized state from the same raw evidence, and that migration is versioned. Without persistent storage the same transactional ordering holds in memory, but process-crash replay/outbox recovery is not promised; the next connection rehydrates authoritative server state and treats prior externally visible outcomes as unknown.

### Runtime and framework adapters

The optional runtime may use `std::jthread`, `std::stop_token`, a mutex/condition variable, and bounded input wake queues. It serializes inputs, drives the handshake/recovery policy, and lets the interpreter claim effects from the transactional outbox. Embedders with an existing event loop may call `StateStore::apply` from their own serialized execution context.

A framework adapter translates scheduling and presentation only:

- a Qt adapter posts a coalescible “revision advanced” wake-up and drains a cursor on the GUI thread;
- an Asio adapter exposes awaitables over the same revision/effect ports;
- a CLI may block in `waitForRevision`;
- tests submit inputs synchronously with a deterministic clock and fake interpreter.

Reactive selectors and toolkit models are disposable projections above cursors. They may suppress value-equivalent updates for efficiency, but they cannot replace the lossless feeds.

If callbacks are offered, implement them as `wait + drain`: invoke consumer code outside locks, attach the exact revision snapshot, queue nested actions as later inputs, and report skipped revision ranges. Never call arbitrary observers from inside reduction or commit.

### Compatibility, security and resource policy

If all consumers rebuild with the library, generated `std::variant` DTOs are the simplest typed API. Within a declared compiler/STL matrix, PIMPL can stabilize implementation layout. Genuine cross-toolchain ABI requires opaque handles/C ABI or encoded records; public STL containers do not provide it.

The type boundary distinguishes `CanonicalApplicationState` from `PublicApplicationState`. Public snapshots and commits contain only redacted types. Exact raw frames, engine events, exchange params/results, canonical changes and private effects belong to separately authorized interfaces and protected memory/storage. Encryption preserves exact replay; destructive redaction deliberately forfeits exact recovery/audit for those bytes while retaining a protected-value marker. Persistent adapters define encryption, redaction and access-control policy.

Ingress must define maximum frame bytes, JSON nesting, decoded collection sizes, aggregate stream/output retention, journal quotas, cursor leases, malformed UTF-8/JSON behavior, and allocation/error policy. Limits produce journaled diagnostics and explicit gaps/failures, never silent truncation of canonical evidence.

Completeness is mechanically enforced. Every directional registry entry declares:

- decoder and encoder coverage;
- capability/handshake gate;
- reducer policy: `Reduce`, `Invalidate`, or `JournalOnly`;
- request/result or reverse-reply projection;
- authority and optimistic policy;
- reconciliation and retry safety;
- public/private effect and security classification;
- unknown-message behavior and transition classification.

The build fails when any of the 157 client requests, 11 server requests, 83 server notifications, or the `initialized` client notification lacks one of these traits.

## Complete client → server request catalogue

In these tables `Params → Response` means the request carries `Params` and its correlated success makes `Response` available. `∅` means params are omitted/undefined.

### Connection, thread, project, history, skills, plugins, apps, and filesystem

| Method                                 | Params → Response                                                                       | Tier |
| -------------------------------------- | --------------------------------------------------------------------------------------- | ---- |
| `initialize`                           | `v1::InitializeParams → v1::InitializeResponse`                                         | S    |
| `server/diagnostics`                   | `ServerDiagnosticsParams → ServerDiagnosticsResponse`                                   | X    |
| `thread/start`                         | `ThreadStartParams → ThreadStartResponse`                                               | S+XF |
| `thread/resume`                        | `ThreadResumeParams → ThreadResumeResponse`                                             | S+XF |
| `thread/fork`                          | `ThreadForkParams → ThreadForkResponse`                                                 | S+XF |
| `thread/archive`                       | `ThreadArchiveParams → ThreadArchiveResponse`                                           | S    |
| `thread/delete`                        | `ThreadDeleteParams → ThreadDeleteResponse`                                             | S    |
| `thread/unsubscribe`                   | `ThreadUnsubscribeParams → ThreadUnsubscribeResponse`                                   | S    |
| `thread/increment_elicitation`         | `ThreadIncrementElicitationParams → ThreadIncrementElicitationResponse`                 | X    |
| `thread/decrement_elicitation`         | `ThreadDecrementElicitationParams → ThreadDecrementElicitationResponse`                 | X    |
| `thread/name/set`                      | `ThreadSetNameParams → ThreadSetNameResponse`                                           | S    |
| `thread/goal/set`                      | `ThreadGoalSetParams → ThreadGoalSetResponse`                                           | S    |
| `thread/goal/get`                      | `ThreadGoalGetParams → ThreadGoalGetResponse`                                           | S    |
| `thread/goal/clear`                    | `ThreadGoalClearParams → ThreadGoalClearResponse`                                       | S    |
| `thread/queue/add`                     | `ThreadQueueAddParams → ThreadQueueAddResponse`                                         | X    |
| `thread/queue/list`                    | `ThreadQueueListParams → ThreadQueueListResponse`                                       | X    |
| `thread/queue/update`                  | `ThreadQueueUpdateParams → ThreadQueueUpdateResponse`                                   | X    |
| `thread/queue/delete`                  | `ThreadQueueDeleteParams → ThreadQueueDeleteResponse`                                   | X    |
| `thread/queue/reorder`                 | `ThreadQueueReorderParams → ThreadQueueReorderResponse`                                 | X    |
| `thread/queue/start`                   | `ThreadQueueStartParams → ThreadQueueStartResponse`                                     | X    |
| `thread/metadata/update`               | `ThreadMetadataUpdateParams → ThreadMetadataUpdateResponse`                             | S+XF |
| `thread/section/move`                  | `ThreadSectionMoveParams → ThreadSectionMoveResponse`                                   | S    |
| `thread/settings/update`               | `ThreadSettingsUpdateParams → ThreadSettingsUpdateResponse`                             | X    |
| `thread/memoryMode/set`                | `ThreadMemoryModeSetParams → ThreadMemoryModeSetResponse`                               | X    |
| `memory/reset`                         | `∅ → MemoryResetResponse`                                                               | X    |
| `thread/unarchive`                     | `ThreadUnarchiveParams → ThreadUnarchiveResponse`                                       | S    |
| `thread/compact/start`                 | `ThreadCompactStartParams → ThreadCompactStartResponse`                                 | S    |
| `thread/shellCommand`                  | `ThreadShellCommandParams → ThreadShellCommandResponse`                                 | S    |
| `thread/approveGuardianDeniedAction`   | `ThreadApproveGuardianDeniedActionParams → ThreadApproveGuardianDeniedActionResponse`   | S    |
| `thread/backgroundTerminals/clean`     | `ThreadBackgroundTerminalsCleanParams → ThreadBackgroundTerminalsCleanResponse`         | X    |
| `thread/backgroundTerminals/list`      | `ThreadBackgroundTerminalsListParams → ThreadBackgroundTerminalsListResponse`           | X    |
| `thread/backgroundTerminals/terminate` | `ThreadBackgroundTerminalsTerminateParams → ThreadBackgroundTerminalsTerminateResponse` | X    |
| `thread/rollback`                      | `ThreadRollbackParams → ThreadRollbackResponse`                                         | D    |
| `thread/revert`                        | `ThreadRevertParams → ThreadRevertResponse`                                             | S    |
| `thread/list`                          | `ThreadListParams → ThreadListResponse`                                                 | S+XF |
| `project/list`                         | `ProjectListParams → ProjectListResponse`                                               | X    |
| `project/read`                         | `ProjectReadParams → ProjectReadResponse`                                               | X    |
| `project/create`                       | `ProjectCreateParams → ProjectCreateResponse`                                           | X    |
| `project/import`                       | `ProjectImportParams → ProjectImportResponse`                                           | X    |
| `project/update`                       | `ProjectUpdateParams → ProjectUpdateResponse`                                           | X    |
| `project/move`                         | `ProjectMoveParams → ProjectMoveResponse`                                               | X    |
| `project/delete`                       | `ProjectDeleteParams → ProjectDeleteResponse`                                           | X    |
| `threadSection/list`                   | `ThreadSectionListParams → ThreadSectionListResponse`                                   | S    |
| `threadSection/create`                 | `ThreadSectionCreateParams → ThreadSectionCreateResponse`                               | S    |
| `threadSection/update`                 | `ThreadSectionUpdateParams → ThreadSectionUpdateResponse`                               | S    |
| `threadSection/delete`                 | `ThreadSectionDeleteParams → ThreadSectionDeleteResponse`                               | S    |
| `thread/search`                        | `ThreadSearchParams → ThreadSearchResponse`                                             | X    |
| `thread/searchOccurrences`             | `ThreadSearchOccurrencesParams → ThreadSearchOccurrencesResponse`                       | X    |
| `thread/loaded/list`                   | `ThreadLoadedListParams → ThreadLoadedListResponse`                                     | S    |
| `thread/read`                          | `ThreadReadParams → ThreadReadResponse`                                                 | S    |
| `thread/turns/list`                    | `ThreadTurnsListParams → ThreadTurnsListResponse`                                       | S    |
| `thread/items/list`                    | `ThreadItemsListParams → ThreadItemsListResponse`                                       | S    |
| `thread/inject_items`                  | `ThreadInjectItemsParams → ThreadInjectItemsResponse`                                   | S    |
| `skills/list`                          | `SkillsListParams → SkillsListResponse`                                                 | S    |
| `skills/extraRoots/set`                | `SkillsExtraRootsSetParams → SkillsExtraRootsSetResponse`                               | S    |
| `hooks/list`                           | `HooksListParams → HooksListResponse`                                                   | S    |
| `marketplace/add`                      | `MarketplaceAddParams → MarketplaceAddResponse`                                         | S    |
| `marketplace/remove`                   | `MarketplaceRemoveParams → MarketplaceRemoveResponse`                                   | S    |
| `marketplace/upgrade`                  | `MarketplaceUpgradeParams → MarketplaceUpgradeResponse`                                 | S    |
| `plugin/list`                          | `PluginListParams → PluginListResponse`                                                 | S    |
| `plugin/search`                        | `PluginSearchParams → PluginSearchResponse`                                             | X    |
| `plugin/installed`                     | `PluginInstalledParams → PluginInstalledResponse`                                       | S    |
| `plugin/read`                          | `PluginReadParams → PluginReadResponse`                                                 | S    |
| `plugin/skill/read`                    | `PluginSkillReadParams → PluginSkillReadResponse`                                       | S    |
| `plugin/share/save`                    | `PluginShareSaveParams → PluginShareSaveResponse`                                       | S    |
| `plugin/share/updateTargets`           | `PluginShareUpdateTargetsParams → PluginShareUpdateTargetsResponse`                     | S    |
| `plugin/share/list`                    | `PluginShareListParams → PluginShareListResponse`                                       | S    |
| `plugin/share/checkout`                | `PluginShareCheckoutParams → PluginShareCheckoutResponse`                               | S    |
| `plugin/share/delete`                  | `PluginShareDeleteParams → PluginShareDeleteResponse`                                   | S    |
| `app/read`                             | `AppsReadParams → AppsReadResponse`                                                     | S    |
| `app/list`                             | `AppsListParams → AppsListResponse`                                                     | S    |
| `app/installed`                        | `AppsInstalledParams → AppsInstalledResponse`                                           | S    |
| `fs/readFile`                          | `FsReadFileParams → FsReadFileResponse`                                                 | S    |
| `fs/writeFile`                         | `FsWriteFileParams → FsWriteFileResponse`                                               | S    |
| `fs/createDirectory`                   | `FsCreateDirectoryParams → FsCreateDirectoryResponse`                                   | S    |
| `fs/getMetadata`                       | `FsGetMetadataParams → FsGetMetadataResponse`                                           | S    |
| `fs/readDirectory`                     | `FsReadDirectoryParams → FsReadDirectoryResponse`                                       | S    |
| `fs/remove`                            | `FsRemoveParams → FsRemoveResponse`                                                     | S    |
| `fs/copy`                              | `FsCopyParams → FsCopyResponse`                                                         | S    |
| `fs/watch`                             | `FsWatchParams → FsWatchResponse`                                                       | S    |
| `fs/unwatch`                           | `FsUnwatchParams → FsUnwatchResponse`                                                   | S    |

### Turn, realtime, catalog, environment, MCP, auth, process, and configuration

| Method                                     | Params → Response                                                                               | Tier |
| ------------------------------------------ | ----------------------------------------------------------------------------------------------- | ---- |
| `skills/config/write`                      | `SkillsConfigWriteParams → SkillsConfigWriteResponse`                                           | S    |
| `plugin/install`                           | `PluginInstallParams → PluginInstallResponse`                                                   | S    |
| `plugin/uninstall`                         | `PluginUninstallParams → PluginUninstallResponse`                                               | S    |
| `turn/start`                               | `TurnStartParams → TurnStartResponse`                                                           | S+XF |
| `turn/settings/update`                     | `TurnSettingsUpdateParams → TurnSettingsUpdateResponse`                                         | X    |
| `turn/steer`                               | `TurnSteerParams → TurnSteerResponse`                                                           | S+XF |
| `turn/interrupt`                           | `TurnInterruptParams → TurnInterruptResponse`                                                   | S    |
| `thread/realtime/start`                    | `ThreadRealtimeStartParams → ThreadRealtimeStartResponse`                                       | X    |
| `thread/realtime/appendAudio`              | `ThreadRealtimeAppendAudioParams → ThreadRealtimeAppendAudioResponse`                           | X    |
| `thread/realtime/appendText`               | `ThreadRealtimeAppendTextParams → ThreadRealtimeAppendTextResponse`                             | X    |
| `thread/realtime/appendSpeech`             | `ThreadRealtimeAppendSpeechParams → ThreadRealtimeAppendSpeechResponse`                         | X    |
| `thread/realtime/stop`                     | `ThreadRealtimeStopParams → ThreadRealtimeStopResponse`                                         | X    |
| `thread/timeline/list`                     | `ThreadTimelineListParams → ThreadTimelineListResponse`                                         | X    |
| `thread/realtime/listVoices`               | `ThreadRealtimeListVoicesParams → ThreadRealtimeListVoicesResponse`                             | X    |
| `review/start`                             | `ReviewStartParams → ReviewStartResponse`                                                       | S    |
| `model/list`                               | `ModelListParams → ModelListResponse`                                                           | S    |
| `modelProvider/capabilities/read`          | `ModelProviderCapabilitiesReadParams → ModelProviderCapabilitiesReadResponse`                   | S    |
| `experimentalFeature/list`                 | `ExperimentalFeatureListParams → ExperimentalFeatureListResponse`                               | S    |
| `permissionProfile/list`                   | `PermissionProfileListParams → PermissionProfileListResponse`                                   | S    |
| `experimentalFeature/enablement/set`       | `ExperimentalFeatureEnablementSetParams → ExperimentalFeatureEnablementSetResponse`             | S    |
| `remoteControl/enable`                     | `NullableRemoteControlEnableParams → RemoteControlEnableResponse`                               | X    |
| `remoteControl/disable`                    | `NullableRemoteControlDisableParams → RemoteControlDisableResponse`                             | X    |
| `remoteControl/status/read`                | `∅ → RemoteControlStatusReadResponse`                                                           | X    |
| `remoteControl/pairing/start`              | `RemoteControlPairingStartParams → RemoteControlPairingStartResponse`                           | X    |
| `remoteControl/pairing/status`             | `RemoteControlPairingStatusParams → RemoteControlPairingStatusResponse`                         | X    |
| `remoteControl/client/list`                | `RemoteControlClientsListParams → RemoteControlClientsListResponse`                             | X    |
| `remoteControl/client/revoke`              | `RemoteControlClientsRevokeParams → RemoteControlClientsRevokeResponse`                         | X    |
| `collaborationMode/list`                   | `CollaborationModeListParams → CollaborationModeListResponse`                                   | X    |
| `mock/experimentalMethod`                  | `MockExperimentalMethodParams → MockExperimentalMethodResponse`                                 | X    |
| `environment/add`                          | `EnvironmentAddParams → EnvironmentAddResponse`                                                 | X    |
| `environment/info`                         | `EnvironmentInfoParams → EnvironmentInfoResponse`                                               | X    |
| `environment/status`                       | `EnvironmentStatusParams → EnvironmentStatusResponse`                                           | X    |
| `mcpServer/oauth/login`                    | `McpServerOauthLoginParams → McpServerOauthLoginResponse`                                       | S    |
| `config/mcpServer/reload`                  | `∅ → McpServerRefreshResponse`                                                                  | S    |
| `mcpServerStatus/list`                     | `ListMcpServerStatusParams → ListMcpServerStatusResponse`                                       | S    |
| `mcpServer/resource/read`                  | `McpResourceReadParams → McpResourceReadResponse`                                               | S    |
| `mcpServer/event/stream/start`             | `McpServerEventStreamStartParams → McpServerEventStreamStartResponse`                           | X    |
| `mcpServer/event/stream/stop`              | `McpServerEventStreamStopParams → McpServerEventStreamStopResponse`                             | X    |
| `mcpServer/tool/call`                      | `McpServerToolCallParams → McpServerToolCallResponse`                                           | S    |
| `windowsSandbox/setupStart`                | `WindowsSandboxSetupStartParams → WindowsSandboxSetupStartResponse`                             | S    |
| `windowsSandbox/readiness`                 | `∅ → WindowsSandboxReadinessResponse`                                                           | S    |
| `account/login/start`                      | `LoginAccountParams → LoginAccountResponse`                                                     | S+XF |
| `account/bedrock/discover`                 | `BedrockDiscoverParams → BedrockDiscoverResponse`                                               | X    |
| `account/bedrock/setup`                    | `BedrockSetupParams → BedrockSetupResponse`                                                     | X    |
| `account/login/cancel`                     | `CancelLoginAccountParams → CancelLoginAccountResponse`                                         | S    |
| `account/logout`                           | `∅ → LogoutAccountResponse`                                                                     | S    |
| `account/rateLimits/read`                  | `∅ → GetAccountRateLimitsResponse`                                                              | S    |
| `account/rateLimitResetCredit/consume`     | `ConsumeAccountRateLimitResetCreditParams → ConsumeAccountRateLimitResetCreditResponse`         | S    |
| `account/usage/read`                       | `NullableGetAccountTokenUsageParams/∅ → GetAccountTokenUsageResponse`                           | S    |
| `account/workspaceMessages/read`           | `∅ → GetWorkspaceMessagesResponse`                                                              | S    |
| `account/sendAddCreditsNudgeEmail`         | `SendAddCreditsNudgeEmailParams → SendAddCreditsNudgeEmailResponse`                             | S    |
| `feedback/upload`                          | `FeedbackUploadParams → FeedbackUploadResponse`                                                 | S    |
| `command/exec`                             | `CommandExecParams → CommandExecResponse`                                                       | S+XF |
| `command/exec/write`                       | `CommandExecWriteParams → CommandExecWriteResponse`                                             | S    |
| `command/exec/terminate`                   | `CommandExecTerminateParams → CommandExecTerminateResponse`                                     | S    |
| `command/exec/resize`                      | `CommandExecResizeParams → CommandExecResizeResponse`                                           | S    |
| `process/spawn`                            | `ProcessSpawnParams → ProcessSpawnResponse`                                                     | X    |
| `process/writeStdin`                       | `ProcessWriteStdinParams → ProcessWriteStdinResponse`                                           | X    |
| `process/kill`                             | `ProcessKillParams → ProcessKillResponse`                                                       | X    |
| `process/resizePty`                        | `ProcessResizePtyParams → ProcessResizePtyResponse`                                             | X    |
| `config/read`                              | `ConfigReadParams → ConfigReadResponse`                                                         | S    |
| `externalAgentConfig/detect`               | `ExternalAgentConfigDetectParams → ExternalAgentConfigDetectResponse`                           | S    |
| `externalAgentConfig/import`               | `ExternalAgentConfigImportParams → ExternalAgentConfigImportResponse`                           | S    |
| `externalAgentConfig/import/recordHistory` | `ExternalAgentConfigImportHistoryRecordParams → ExternalAgentConfigImportHistoryRecordResponse` | S    |
| `externalAgentConfig/import/readHistories` | `∅ → ExternalAgentConfigImportHistoriesReadResponse`                                            | S    |
| `config/value/write`                       | `ConfigValueWriteParams → ConfigWriteResponse`                                                  | S    |
| `config/batchWrite`                        | `ConfigBatchWriteParams → ConfigWriteResponse`                                                  | S    |
| `configRequirements/read`                  | `∅ → ConfigRequirementsReadResponse`                                                            | S    |
| `account/read`                             | `GetAccountParams → GetAccountResponse`                                                         | S    |
| `getConversationSummary`                   | `v1::GetConversationSummaryParams → v1::GetConversationSummaryResponse`                         | D    |
| `gitDiffToRemote`                          | `v1::GitDiffToRemoteParams → v1::GitDiffToRemoteResponse`                                       | D    |
| `getAuthStatus`                            | `v1::GetAuthStatusParams → v1::GetAuthStatusResponse`                                           | D    |
| `fuzzyFileSearch`                          | `FuzzyFileSearchParams → FuzzyFileSearchResponse`                                               | D    |
| `fuzzyFileSearch/sessionStart`             | `FuzzyFileSearchSessionStartParams → FuzzyFileSearchSessionStartResponse`                       | X+D  |
| `fuzzyFileSearch/sessionUpdate`            | `FuzzyFileSearchSessionUpdateParams → FuzzyFileSearchSessionUpdateResponse`                     | X+D  |
| `fuzzyFileSearch/sessionStop`              | `FuzzyFileSearchSessionStopParams → FuzzyFileSearchSessionStopResponse`                         | X+D  |

The deprecated label follows their placement in the registry's compatibility block. Experimental field checks are independent of method tier, hence `S+XF`. Server-side serialization scopes are implementation ordering rules—not additional identity: global resources serialize by named lock; thread, command, process, fuzzy session, watch, and MCP OAuth families serialize by their natural key; unscoped reads may execute concurrently.

## Complete server → client request catalogue

These are true reverse RPCs: the server sends the request and the client sends the correlated success or error. They live in the same exchange ledger, with `requester = Server`.

| Method                                  | Params → client Response                                                          | Availability / condition                                                                                                                                                                              | Tier |
| --------------------------------------- | --------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---- |
| `item/commandExecution/requestApproval` | `CommandExecutionRequestApprovalParams → CommandExecutionRequestApprovalResponse` | Pending command approval; includes thread/turn/item, start time, optional `approvalId`, environment/reason/network context, command/cwd/actions, requested permissions and proposed policy amendments | S+XF |
| `item/fileChange/requestApproval`       | `FileChangeRequestApprovalParams → FileChangeRequestApprovalResponse`             | Pending patch approval, optional reason and unstable grant root                                                                                                                                       | S+U  |
| `item/tool/requestUserInput`            | `ToolRequestUserInputParams → ToolRequestUserInputResponse`                       | Ordered questions, blocking flag, optional deprecated auto-resolution timeout; response is answer map                                                                                                 | U    |
| `mcpServer/elicitation/request`         | `McpServerElicitationRequestParams → McpServerElicitationRequestResponse`         | MCP server/thread/optional turn and form/URL-mode elicitation; response action plus optional content and `_meta`                                                                                      | S    |
| `item/permissions/requestApproval`      | `PermissionsRequestApprovalParams → PermissionsRequestApprovalResponse`           | Requested cwd/permissions and optional environment/reason; response grants permissions, scope, optional strict-auto-review                                                                            | S    |
| `item/tool/call`                        | `DynamicToolCallParams → DynamicToolCallResponse`                                 | Client-hosted dynamic tool call; response supplies content items and success                                                                                                                          | U/C  |
| `account/chatgptAuthTokens/refresh`     | `ChatgptAuthTokensRefreshParams → ChatgptAuthTokensRefreshResponse`               | Only externally managed ChatGPT-token auth; returns access token, account ID, plan type                                                                                                               | S/C  |
| `attestation/generate`                  | `AttestationGenerateParams → AttestationGenerateResponse`                         | Only when `requestAttestation` is negotiated; returns a fresh token                                                                                                                                   | S/C  |
| `currentTime/read`                      | `CurrentTimeReadParams → CurrentTimeReadResponse`                                 | Experimental external client clock for a thread                                                                                                                                                       | X/C  |
| `applyPatchApproval`                    | `v1::ApplyPatchApprovalParams → v1::ApplyPatchApprovalResponse`                   | Legacy patch approval                                                                                                                                                                                 | D    |
| `execCommandApproval`                   | `v1::ExecCommandApprovalParams → v1::ExecCommandApprovalResponse`                 | Legacy command approval                                                                                                                                                                               | D    |

An RPC response has no `method`; decode it with the pending request's method. `serverRequest/resolved` is a lifecycle cleanup signal and may arrive after the response. `approvalId`, where present, distinguishes multiple callbacks for one item.

## Complete server → client notification catalogue

Every row is one accepted notification union member. The payload type is the exhaustive field contract; “materializes” states which normalized view receives it. All also append a `WireFrame`. Fields omitted from a sparse notification remain unknown or retain their previous fact.

### Conversation, item, process, and MCP notifications

| Method                                      | Payload type                                      | Materializes / reducer action                                                         | Tier |
| ------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------------------------------------- | ---- |
| `error`                                     | `ErrorNotification`                               | Thread/turn error event: error plus `willRetry`; distinct from a correlated RPC error | S    |
| `thread/started`                            | `ThreadStartedNotification`                       | Upsert full Thread descriptor and graph keys                                          | S    |
| `thread/status/changed`                     | `ThreadStatusChangedNotification`                 | Replace thread status/active flags                                                    | S    |
| `thread/archived`                           | `ThreadArchivedNotification`                      | Set archived fact                                                                     | S    |
| `thread/deleted`                            | `ThreadDeletedNotification`                       | Set deleted tombstone                                                                 | S    |
| `thread/unarchived`                         | `ThreadUnarchivedNotification`                    | Clear archived fact                                                                   | S    |
| `thread/closed`                             | `ThreadClosedNotification`                        | Mark unloaded/closed, never deleted                                                   | S    |
| `thread/reverted`                           | `ThreadRevertedNotification`                      | Record revert and invalidate affected hydrated history                                | S    |
| `skills/changed`                            | `SkillsChangedNotification`                       | Invalidate cwd-scoped skills queries                                                  | S    |
| `thread/name/updated`                       | `ThreadNameUpdatedNotification`                   | Set nullable name                                                                     | S    |
| `thread/goal/updated`                       | `ThreadGoalUpdatedNotification`                   | Set thread goal and source turn                                                       | S    |
| `thread/goal/cleared`                       | `ThreadGoalClearedNotification`                   | Set goal to known-null                                                                | S    |
| `thread/queue/changed`                      | `ThreadQueueChangedNotification`                  | Invalidate that thread's queue query                                                  | X    |
| `project/changed`                           | `ProjectChangedNotification`                      | Mark project catalog/entity stale using project ID and change type                    | X    |
| `thread/project/updated`                    | `ThreadProjectUpdatedNotification`                | Set exact nullable project assignment                                                 | X    |
| `thread/environment/connected`              | `EnvironmentConnectionNotification`               | Set thread/environment live connection true                                           | X    |
| `thread/environment/disconnected`           | `EnvironmentConnectionNotification`               | Set thread/environment live connection false                                          | X    |
| `thread/settings/updated`                   | `ThreadSettingsUpdatedNotification`               | Replace effective ThreadSettings                                                      | X    |
| `thread/tokenUsage/updated`                 | `ThreadTokenUsageUpdatedNotification`             | Replace thread/turn token-usage snapshot                                              | S    |
| `turn/started`                              | `TurnStartedNotification`                         | Upsert Turn head; embedded items are initially empty                                  | S    |
| `hook/started`                              | `HookStartedNotification`                         | Upsert hook-run state and join to thread/optional turn                                | S    |
| `turn/completed`                            | `TurnCompletedNotification`                       | Finalize Turn status/times/error; upsert final-agent fallback only                    | S    |
| `hook/completed`                            | `HookCompletedNotification`                       | Finalize hook run                                                                     | S    |
| `turn/diff/updated`                         | `TurnDiffUpdatedNotification`                     | Replace latest unified diff                                                           | S    |
| `turn/plan/updated`                         | `TurnPlanUpdatedNotification`                     | Replace explanation and ordered plan steps                                            | S    |
| `item/started`                              | `ItemStartedNotification`                         | Create/upsert Item, owner edge, and start milliseconds                                | S    |
| `item/autoApprovalReview/started`           | `ItemGuardianApprovalReviewStartedNotification`   | Create guardian review joined to target item                                          | U    |
| `item/autoApprovalReview/completed`         | `ItemGuardianApprovalReviewCompletedNotification` | Finalize guardian review/decision source                                              | U    |
| `autoApprovalReview/strictReviewRequired`   | `StrictReviewRequiredNotification`                | Record strict-review escalation for item/turn                                         | X    |
| `item/completed`                            | `ItemCompletedNotification`                       | Replace item with authoritative final variant and completion milliseconds             | S    |
| `rawResponseItem/completed`                 | `RawResponseItemCompletedNotification`            | Append exact upstream ResponseItem history; never coerce to ThreadItem                | I    |
| `rawResponse/completed`                     | `RawResponseCompletedNotification`                | Append exact upstream response/usage record                                           | I    |
| `item/agentMessage/delta`                   | `AgentMessageDeltaNotification`                   | Append agent text fragment in arrival order                                           | S    |
| `item/plan/delta`                           | `PlanDeltaNotification`                           | Append proposed-plan text fragment                                                    | U    |
| `command/exec/outputDelta`                  | `CommandExecOutputDeltaNotification`              | Append base64 stdout/stderr bytes to command-process incarnation                      | S    |
| `process/outputDelta`                       | `ProcessOutputDeltaNotification`                  | Append base64 output bytes to unsandboxed process incarnation                         | X    |
| `process/exited`                            | `ProcessExitedNotification`                       | Finalize process exit state                                                           | X    |
| `item/commandExecution/outputDelta`         | `CommandExecutionOutputDeltaNotification`         | Append command-item aggregated output                                                 | S    |
| `item/commandExecution/terminalInteraction` | `TerminalInteractionNotification`                 | Append terminal input/output interaction event                                        | S    |
| `item/fileChange/outputDelta`               | `FileChangeOutputDeltaNotification`               | Append deprecated apply-patch output                                                  | D    |
| `item/fileChange/patchUpdated`              | `FileChangePatchUpdatedNotification`              | Replace current patch snapshot                                                        | S    |
| `serverRequest/resolved`                    | `ServerRequestResolvedNotification`               | Clear matching pending reverse request                                                | S    |
| `item/mcpToolCall/progress`                 | `McpToolCallProgressNotification`                 | Append progress for MCP item                                                          | S    |
| `mcpServer/oauthLogin/completed`            | `McpServerOauthLoginCompletedNotification`        | Finalize server/thread-scoped OAuth attempt                                           | S    |
| `mcpServer/startupStatus/updated`           | `McpServerStatusUpdatedNotification`              | Patch server runtime startup status/error/failure reason                              | S    |
| `mcpServer/event/stream/notification`       | `McpServerEventStreamNotification`                | Append event under subscription incarnation                                           | X    |

### Account, catalog, model, diagnostic, search, realtime, and platform notifications

| Method                                  | Payload type                                     | Materializes / reducer action                                                             | Tier |
| --------------------------------------- | ------------------------------------------------ | ----------------------------------------------------------------------------------------- | ---- |
| `account/updated`                       | `AccountUpdatedNotification`                     | Patch auth mode and optional plan projection; not the full Account                        | S    |
| `account/rateLimits/updated`            | `AccountRateLimitsUpdatedNotification`           | Sparse merge of rate-limit values; do not clear absent metadata                           | S    |
| `app/list/updated`                      | `AppListUpdatedNotification`                     | Invalidate/apply installed-app list update in its scope                                   | S    |
| `remoteControl/status/changed`          | `RemoteControlStatusChangedNotification`         | Replace live remote-control status                                                        | S/C  |
| `externalAgentConfig/import/progress`   | `ExternalAgentConfigImportProgressNotification`  | Patch import progress by `importId`                                                       | S    |
| `externalAgentConfig/import/completed`  | `ExternalAgentConfigImportCompletedNotification` | Finalize import by `importId`                                                             | S    |
| `fs/changed`                            | `FsChangedNotification`                          | Append filesystem event under `watchId` incarnation                                       | S    |
| `item/reasoning/summaryTextDelta`       | `ReasoningSummaryTextDeltaNotification`          | Append reasoning-summary text fragment                                                    | S    |
| `item/reasoning/summaryPartAdded`       | `ReasoningSummaryPartAddedNotification`          | Append ordered reasoning-summary part boundary                                            | S    |
| `item/reasoning/textDelta`              | `ReasoningTextDeltaNotification`                 | Append raw reasoning text fragment                                                        | S    |
| `thread/compacted`                      | `ContextCompactedNotification`                   | Deprecated compaction marker; prefer `contextCompaction` item                             | D    |
| `model/rerouted`                        | `ModelReroutedNotification`                      | Record turn model/provider/service-tier route change                                      | S    |
| `model/verification`                    | `ModelVerificationNotification`                  | Append model verification outcome                                                         | S    |
| `modelProvider/authRecoveryStarted`     | `AuthRecoveryNotification`                       | Mark provider auth recovery active; exists on pinned `main`, not v0.151.0                 | S    |
| `modelProvider/authRecoveryCompleted`   | `AuthRecoveryNotification`                       | Finalize provider auth recovery; exists on pinned `main`, not v0.151.0                    | S    |
| `turn/moderationMetadata`               | `TurnModerationMetadataNotification`             | Replace open moderation metadata for the turn                                             | X    |
| `model/safetyBuffering/updated`         | `ModelSafetyBufferingUpdatedNotification`        | Replace live safety-buffering state                                                       | S    |
| `warning`                               | `WarningNotification`                            | Append scoped warning notice                                                              | S    |
| `guardianWarning`                       | `GuardianWarningNotification`                    | Append Guardian warning with target context                                               | S    |
| `deprecationNotice`                     | `DeprecationNoticeNotification`                  | Append deprecation diagnostic                                                             | S    |
| `configWarning`                         | `ConfigWarningNotification`                      | Append configuration warning; never mutate config from it                                 | S    |
| `fuzzyFileSearch/sessionUpdated`        | `FuzzyFileSearchSessionUpdatedNotification`      | Replace session query/results snapshot                                                    | D/C  |
| `fuzzyFileSearch/sessionCompleted`      | `FuzzyFileSearchSessionCompletedNotification`    | Mark session complete; payload has only `sessionId`, so retain prior query                | D/C  |
| `thread/realtime/started`               | `ThreadRealtimeStartedNotification`              | Create live realtime session                                                              | X    |
| `thread/realtime/itemAdded`             | `ThreadRealtimeItemAddedNotification`            | Add realtime item and timeline position                                                   | X    |
| `thread/realtime/item/started`          | `ThreadRealtimeItemStartedNotification`          | Mark realtime item started                                                                | X    |
| `thread/realtime/item/transcript/delta` | `ThreadRealtimeItemTranscriptDeltaNotification`  | Append item transcript fragment                                                           | X    |
| `thread/realtime/item/completed`        | `ThreadRealtimeItemCompletedNotification`        | Finalize realtime item                                                                    | X    |
| `thread/realtime/transcript/delta`      | `ThreadRealtimeTranscriptDeltaNotification`      | Append session transcript fragment                                                        | X    |
| `thread/realtime/transcript/done`       | `ThreadRealtimeTranscriptDoneNotification`       | Finalize session transcript                                                               | X    |
| `thread/realtime/outputAudio/delta`     | `ThreadRealtimeOutputAudioDeltaNotification`     | Append base64 output-audio chunk                                                          | X    |
| `thread/realtime/sdp`                   | `ThreadRealtimeSdpNotification`                  | Store current session-description exchange data                                           | X    |
| `thread/realtime/error`                 | `ThreadRealtimeErrorNotification`                | Append realtime error                                                                     | X    |
| `thread/realtime/closed`                | `ThreadRealtimeClosedNotification`               | Close live realtime session                                                               | X    |
| `windows/worldWritableWarning`          | `WindowsWorldWritableWarningNotification`        | Append sandbox-protection warning/path facts                                              | S    |
| `windowsSandbox/setupCompleted`         | `WindowsSandboxSetupCompletedNotification`       | Finalize Windows sandbox setup state                                                      | S    |
| `account/login/completed`               | `AccountLoginCompletedNotification`              | Finalize nullable `loginId` attempt with success/error and optional onboarding entrypoint | S    |

## Complete client → server notification catalogue

| Method        | Params  | Effect                                                                                                             |
| ------------- | ------- | ------------------------------------------------------------------------------------------------------------------ |
| `initialized` | omitted | Acknowledges successful `initialize`; the only client notification and the handshake barrier before normal traffic |

## Exact decoding and merge notes

- Requests and notifications are tagged by `method`; `ThreadItem`, `UserInput`, account/login, permissions, MCP elicitation, realtime items, and numerous nested values are tagged unions. Decode into `std::variant` plus an `Unknown{std::string discriminator, JsonObject raw}` arm.
- Preserve JSON number widths as schema declares (`std::int64_t`, `std::uint64_t`, `double`) and do not route canonical protocol data through a toolkit's dynamic value container, which can blur number and null semantics.
- Treat `params` optionality separately from `{}`. The registry accepts omitted params for the explicit `∅` rows; other calls require their typed object even when every member is optional.
- For update calls, `Patch<T>::Omitted` means leave unchanged, `Null` means clear, and `Value` means set. Important examples include service tier, list filters such as `sectionId`/`projectId`, git metadata, and thread settings.
- Timeline ordinary item entries carry camelCase `turnId`; some turn-boundary payload fields originate as `turn_id`, `started_at`, `completed_at`, and `duration_ms`. Generate codecs from schema instead of applying one global case transform.
- `AuthMode` wire spelling includes `apikey`; the Account union's corresponding discriminator is `apiKey`. They are different enums.
- Notification opt-out is exact-method and connection-local. Missing an opted-out event lowers evidence coverage; it is never evidence that the represented state is false.

## Implementation recipe

1. Generate stable and experimental DTOs for the exact Codex binary; add unknown-union fallbacks without changing its emitted spelling.
2. Accept `StoreInput`, assign `InputSeq`/`FrameSeq`, and append exact raw frame evidence before decoding; parse once into a persisted `DecodedFrameInput`, retaining malformed/unknown outcomes instead of dropping them.
3. Pass that `EngineInput` plus immutable ledger/outbox context through the pure reducer; it returns exchange/operation mutations, canonical changes and private effects. Record `SourceRef` and page evidence.
4. Atomically install immutable canonical and public application states, append the redacted public transition, and insert private effects into the transactional outbox before publishing the revision.
5. Build reverse indexes—session children, fork children, subagents, project/section membership, item lookup, plugin/app attribution—from forward keys inside that transaction or lazily by revision.
6. Interpret effects only after commit; feed every write/timer outcome back as a connection- and effect-qualified input.
7. Expose atomic snapshot-plus-cursor reads; implement callbacks, selectors, and toolkit models only as adapters over that revision feed.
8. On reconnect, create a new `ConnectionId`, run the exact handshake, classify uncertain old operations, and requery authoritative snapshots. Do not erase persisted conversation/catalog facts merely because the new connection has not observed them yet.

The journals can be bounded only after an application chooses what fidelity it is willing to lose. Exact reducer replay requires `EngineEventLog`; exact wire audit requires `WireJournal`. Otherwise checkpoint canonical/public application states, ledger, outbox, ID counters and version metadata, then retain all newer engine events plus unresolved/indeterminate operations. Raw frames can contain credentials, attestation, secret input, elicitation answers, local paths, and tool output, so persistent storage must be encrypted/redacted and access-controlled.

## Mechanical completeness criteria

For this pinned revision a complete implementation must recognize exactly:

- 157 client request methods and their paired success types;
- 11 server request methods and their paired client success types;
- 83 server notification methods, including the two internal-only events and two post-v0.151.0 auth-recovery events;
- one client notification, `initialized`;
- four envelopes: request, success response, error response, and notification, while retaining optional trace/emission metadata.

The registry is the check source because schema export deliberately filters compatibility/internal entries. Re-run the two generation commands at the top and diff both the method sets and named DTO definitions whenever Codex changes.
