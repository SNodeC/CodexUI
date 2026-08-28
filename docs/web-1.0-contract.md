# CodexWebUI 1.0 Contract

## Purpose

CodexWebUI is the browser presentation of the existing CodexUI product. It is
not another backend and does not replace or reimplement `codex-bridge`.
Version 1.0 requires a production-built browser application that can perform
the complete web parity scope below against the canonical bridge. A web
feature is complete only after it independently proves behavioral equality
with the corresponding current C++ feature.

The native Qt application remains supported. Native and browser frontends may
be connected to the same bridge at the same time and follow the same
controller/observer rules.

## Fixed architecture

```text
CodexWebUI (React presentation)
    -> TypeScript presentation projection
    -> framework-neutral TypeScript frontend SDK
    -> browser WebSocket using the `codex` subprotocol
    -> existing codex-bridge
    -> Codex app-server
```

There is no web-specific application server, proxy controller, repository,
cache, event bus, or retained Codex-domain store.

`codex-bridge` remains the only multi-frontend router. Its C++ implementation
continues to own controller policy, observer restrictions, upstream request-ID
translation, response ownership, server-request routing, provider lifecycle,
and notification fanout. None of that server-side routing is copied into
TypeScript.

The TypeScript SDK implements only frontend responsibilities already present
in `ai::openai::codex::frontend::CodexBridge` and `ClientConnection`:

- bridge-envelope classification and connection telemetry;
- frontend request/response correlation;
- notification and server-request dispatch;
- provider-generation and disconnect retirement;
- controller claim, release, and transfer commands;
- one attached browser WebSocket transport and its lifecycle.

## Repository boundary

AISuite owns a publishable, framework-neutral package:

```text
packages/codex-frontend/
    src/protocol/generated.ts
    src/protocol/envelope.ts
    src/CodexBridgeClient.ts
    src/WebSocketTransport.ts
    src/errors.ts
    src/index.ts
```

Its working package name is `@snodec/codex-frontend`. The existing pinned
Codex schema and generator are the only source for generated C++ and
TypeScript protocol declarations.

CodexUI owns the browser application:

```text
web/
    src/presentation/
    src/conversation/
    src/components/
    src/app/
```

The web presentation normalizer and reducer live here because they express
CodexUI behavior, not bridge semantics. During development and CI CodexUI pins
the AISuite package to an exact revision, as the native build already does for
the C++ package.

## Frontend technology

CodexWebUI is a client-rendered TypeScript application using React and Vite.
It does not require server rendering: the product is an interactive remote
client, has no public content routes, and acquires its state after connecting
to a bridge.

The initial application has one shell and no URL-routing dependency. Thread,
Inspector, disclosure, selection, draft, and scroll state are application
state, not pages. A router is added only if a later user-facing navigation
requirement proves one necessary.

The application uses:

- React components for compositional presentation and keyed card identity;
- ordinary CSS with CodexUI-owned tokens for visual styling;
- Vitest for SDK, reducer, projection, and component tests;
- Playwright for browser-level workflows and supported-browser qualification;
- npm lockfiles and reproducible production builds.

No general-purpose state framework is introduced. A typed presentation model
is the sole retained normalized store. React subscribes to snapshots from that
model; components retain only interaction state they own.

## Presentation boundary

The semantic vocabulary and authority rules of `codexui.presentation` version
1 remain the shared native/web contract. In the browser they form an internal
TypeScript boundary rather than an additional network hop:

```text
bridge/app-server input
    -> TypeScript normalizer
    -> codexui.presentation frame
    -> TypeScript presentation model
    -> typed visible projections
    -> React
```

The TypeScript implementation must match the existing rules for stable IDs,
merge/replace/remove authority, generation retirement, unknown events,
incomplete reconstruction, child-thread ownership, and ordered items. Shared
JSON fixtures verify equivalent C++ and TypeScript normalization/reduction.

Equality is judged by observable behavior and state transitions, not source
structure or pixel identity. For the same ordered inputs, native and web must
produce equivalent commands, authority decisions, retained identities,
ordering, lifecycle outcomes, visible projections, and user-action
eligibility. Platform-native geometry may differ only where the behavioral
contract explicitly permits it.

The presentation model is an in-memory view of app-server publications. It is
not persistence or semantic authority and is rebuilt by discovery and
hydration after a new connection generation.

## Version 1.0 parity

### Required

The following behavior is required before the combined product is called
version 1.0.

| Area | Required web behavior |
| --- | --- |
| Connection | Configure a WebSocket URL, connect, disconnect, reconnect, and present bridge/provider state |
| Roles | Present controller/observer state and claim or release control |
| Threads | Discover, sort, select, hydrate, reload, create, rename, fork, archive, unarchive, and delete |
| Hierarchy | Preserve parent/child ownership and expandable thread branches |
| Turns | Start, steer, interrupt, resume when required, and recover after provider generations |
| Composer | Retain drafts, admit prompts locally, queue independently per thread, and acknowledge by correlated result |
| Settings | Model, reasoning, access, network, workspace path, approval, personality, profile, reviewer, service tier, summary, and collaboration mode |
| Conversation | User, assistant, reasoning, command, file-change, agent, image-metadata, plan, and fallback activity cards |
| Streaming | Update retained cards without reordering, rebuilding unchanged cards, or moving a paused reading position |
| Attention | Present and resolve every supported pending-request category |
| Inspector | Plan, Agents, Requests, State, and Protocol views |
| Diagnostics | Bounded notices and human-readable event labels without changing protocol values |
| Usability | Responsive desktop layout, keyboard operation, accessible names, contrast, and reduced-motion behavior |

Prompt admission, stable first-response placement, per-thread scroll ownership,
card folding, safe Markdown, bounded command output, controller eligibility,
and reconnect hydration follow the native behavior documents. Visual geometry
may be browser-native; the behavioral invariant may not silently change.

### Browser-specific representation

These capabilities remain required but use an honest browser representation:

- Workspace is an app-server path entered or selected from provider-supplied
  data; it is not inferred from the browser machine.
- File-change conversation cards show the app-server publication.
- Image items show status and saved-path metadata when the saved file is not
  available through a browser URL.
- Connection configuration exposes WebSocket/WSS only.

### Native-only in 1.0

The following Qt capabilities depend on direct access to the native machine
and are explicitly outside browser parity:

- Unix, raw TCP/TLS, IPv6, and RFCOMM frontend transports;
- the native filesystem picker and local-path attachment admission;
- libgit2 worktree discovery, filesystem watching, local Changes snapshots,
  and the modeless native diff window;
- opening provider-side files with a desktop application;
- desktop entry, window manager, taskbar, and native icon integration;
- direct display of provider-local generated-image files.

These exclusions must not be represented by controls that appear functional.
Adding remote file transfer, an asset endpoint, or provider-side Git service
would require a separately designed protocol capability and is not part of the
web implementation.

## State ownership

| State | Owner |
| --- | --- |
| Codex account, configuration, thread, turn, item, plan, request, and persistence | Codex app-server |
| Multi-client routing, provider generation, controller policy, request ownership | `codex-bridge` |
| JSON-RPC callbacks and connection snapshot | TypeScript frontend SDK |
| Normalized threads, turns, items, plans, agents, requests, and telemetry | Web presentation model |
| Selected thread/tab, drafts, folding, scroll anchors, focus, transient menus | React application |

State never appears in two owners in the same layer. Components receive typed
projections and commands; they do not parse app-server methods or retain a
second copy of normalized domain collections.

## Delivery commits and equality gates

1. **Contract and parity**: this document fixes scope, ownership, repository
   boundaries, and frontend structure.
2. **Generated TypeScript protocol**: deterministic TypeScript output from the
   existing pinned schema, with generator drift tests.
3. **TypeScript frontend SDK**: frontend proxy and browser WebSocket transport,
   with the C++ SDK lifecycle/routing invariants ported as tests.
4. **Web presentation model**: normalizer, reducer, command facade, and shared
   native/web fixture corpus.
5. **Core web shell**: connection, thread navigation, conversation, composer,
   streaming, and controller state.
6. **Advanced conversation behavior**: prompt admission, scrolling, folding,
   tools, reasoning, agents, plans, Markdown, and interruption.
7. **Required supporting surfaces**: settings, pending requests, complete
   thread actions, and the required Inspector tabs.
8. **Qualification**: browser workflows, resilience, accessibility,
   responsive behavior, and measured large-thread/streaming performance.
9. **Release**: production packaging, documentation, complete native and web
   suites, and the version 1.0 checklist.

Every implementation commit must be independently reviewable and must add the
focused equality proof for the behavior it introduces. Depending on the layer,
that proof is one or more of:

- the same JSON fixture corpus executed by C++ and TypeScript;
- equivalent C++ and TypeScript request/lifecycle scenario tests;
- visible-projection snapshots derived from the same normalized state;
- browser interaction tests that assert the corresponding native behavior
  contract rather than only DOM structure.

A commit is not complete while its focused equality test, the accumulated web
suite, or the repository's complete native suite fails. Cross-repository
contract changes pin compatible revisions and test both sides before either
dependency is advanced.

## Explicit exclusions

- a JavaScript/TypeScript port of the server-side `codex-bridge` router;
- a Node application server or browser-specific backend;
- protocol-value changes made for presentation convenience;
- duplicated controller, repository, provider, or Codex-domain state;
- speculative caches, controllers, repositories, or event systems;
- URL routing without an actual navigation requirement;
- native-only controls with simulated or incomplete browser behavior.
