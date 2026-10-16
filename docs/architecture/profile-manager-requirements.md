# Codex Profile Manager requirements

Status: agreed product requirements, with implementation explicitly requested on 2026-10-16. See the [CodexManager guide](../guides/profile-manager.md) for the implementation and operational requirements. These instructions govern future profile-manager work; they do not authorize production-code additions, deployment, or changes to personal profiles by themselves.

## Purpose and application boundary

Provide a second Qt executable, `codex-manager` (CodexManager), in the CodexUI project alongside `codex-ui`. The manager configures profiles, launches components, supervises owned processes, and connects CodexUI to externally managed systems. CodexUI remains the conversation interface; the bridge remains the protocol router and, for stdio, the owner of its app-server child.

A profile identifies a particular app-server environment and its access path. A profile can contain projects, sections, and conversation threads. A profile is not a single conversation or a source checkout. Do not move project, section, thread, or turn authority into the manager.

```text
Profile Manager
├── Profile A: CodexUI ↔ bridge ↔ app-server
└── Profile B: CodexUI ↔ bridge ↔ app-server
```

## Multiple profiles and app-servers on one machine

- Multiple app-server instances on the same machine must be supported simultaneously, with distinct `CODEX_HOME` directories identifying independent profile environments.
- Do not assume one app-server, bridge, or profile per host, operating-system user, executable, or network address. The same installed executables may serve multiple independent profiles.
- Each managed app-server must receive its profile's exact `CODEX_HOME`. Preserve separate configuration, authentication, persisted conversations, runtime state, and applicable caches; do not silently reuse another profile's home.
- Each profile must have distinct endpoints wherever components would otherwise compete for a listener, including bridge frontend Unix sockets, managed Unix app-server sockets, and network address/port combinations. A stdio app-server uses its own bridge-owned pipes rather than a provider listener.
- Distinguish owned processes, readiness, logs, shutdown, and reconnection by profile/component identity. Stopping or restarting one profile must not affect another, including when both use the same executable on the same host.
- Detect attempts to register or launch the same profile environment twice. Distinct labels do not make the same host and effective `CODEX_HOME` independent. Diagnose endpoint collisions before launching; do not kill another profile to reclaim its socket or port.
- Apply the same multiplicity and isolation requirements to remote hosts managed through SSH.
- Distinct homes do not provide operating-system filesystem isolation. Do not present them as a security boundary against access available to the same operating-system user.

## Profile configuration

Persist one authoritative profile configuration covering the following settings.

| Area | Required information |
| --- | --- |
| Identity | Stable unique ID, display name, description, and accent color |
| Machines | Hosts on which CodexUI, bridge, and app-server run |
| Ownership | Explicit local management, SSH management, or external management for applicable components |
| Executables | Executable paths and applicable arguments on the owning host |
| Environment | App-server `CODEX_HOME`, working directories, and environment settings |
| Frontend connection | How CodexUI reaches the bridge |
| Provider connection | How the bridge reaches app-server |
| Security | Applicable TLS settings, trust configuration, and SSH configuration references |
| Lifecycle | Startup preferences, readiness expectations, and shutdown behavior |
| Diagnostics | Log locations and access methods |

Support creating an empty profile and registering an existing environment. Never silently copy credentials, overwrite existing configuration, or modify an existing Codex home. Provision shared instructions and configuration explicitly: a new `CODEX_HOME` does not automatically inherit the primary home's global instructions.

Associate every executable, working directory, Codex home, socket, log, and certificate path with its host. Never interpret a remote path as a local one.

## Connections and transport capabilities

Configure the two links independently:

```text
CodexUI ↔ codex-bridge ↔ codex app-server
```

### CodexUI to bridge

Expose Unix-domain sockets, supported IPv4/IPv6 legacy/plain stream transports, TLS streams, WebSocket/WSS transports, and RFCOMM variants where available. Present protocol, address family, and encryption as clear choices with the applicable address, port, socket path, WebSocket path, certificate, and trust fields.

Offer only combinations supported by the installed components. Reuse existing transport configuration and validation rather than implementing a parallel transport layer. A local bridge may be managed regardless of its frontend transport.

### Bridge to app-server

The currently established provider modes are bridge-owned stdio, Unix-domain WebSocket, IPv4 WebSocket, and IPv6 WebSocket. Do not imply arbitrary raw TCP, TLS, or other provider modes exist without implementation support.

Automatic startup and supervision for a local Unix-socket app-server is a required proposed capability. The existing bridge's Unix mode connects to an already running app-server. Implement an explicit owner for managed Unix startup; do not claim this is already provided or infer ownership from socket transport alone.

## Process ownership and supervision

| Mode | Required manager behavior |
| --- | --- |
| Managed locally | Start, identify, supervise, stop, and restart the owned component |
| Managed remotely through SSH | Perform those operations through an explicitly configured remote process or service owner |
| Externally managed | Supply startup/shutdown instructions, connect, and observe readiness; do not stop or restart the external component |

The manager starts and supervises the local CodexUI instance it launches. In stdio mode, the bridge launches and owns app-server; the manager must not independently launch a competing provider. Preserve that ownership relationship even when the bridge is managed through SSH.

For non-stdio app-servers, ownership is explicit. Support both managed local Unix app-servers and existing external endpoints. An app-server reached through a network connection is externally started unless explicit SSH management is configured for its host and component. SSH access alone never implies permission or ownership.

## Required deployment scenarios

Support every row and mixed ownership within a row. The baseline column describes behavior without SSH management; configured SSH ownership extends management to eligible remote components.

| Scenario | Bridge | CodexUI to bridge | App-server | Bridge to app-server | Baseline supervision | Whole-system management |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Local, managed | Unix socket | Local, bridge-owned | Stdio | CodexUI, bridge, and its child | Yes |
| 2 | Local, managed | Supported local network endpoint | Local, bridge-owned | Stdio | CodexUI, bridge, and its child | Yes |
| 3 | Local, managed | Unix socket | Local, managed | Unix socket | CodexUI, bridge, and app-server | Yes, after managed Unix startup is implemented |
| 4 | Local, managed | Supported local network endpoint | Local, managed | Unix socket | CodexUI, bridge, and app-server | Yes, after managed Unix startup is implemented |
| 5 | Local, managed | Any supported local transport | Local, external | Unix socket | CodexUI and bridge; provider readiness only | No, unless provider ownership is changed explicitly |
| 6 | Local, managed | Any supported local transport | Local, external | Supported network endpoint | CodexUI and bridge; provider readiness only | No, unless explicit management is configured |
| 7 | Local, managed | Any supported local transport | Remote | Supported network endpoint | CodexUI and bridge; provider readiness only | Yes with configured SSH management of app-server; otherwise no |
| 8 | Remote | Supported remote frontend endpoint | On bridge host | Stdio | Local CodexUI; remote readiness only | Yes with SSH-managed bridge and its owned child; otherwise no |
| 9 | Remote | Supported remote frontend endpoint | On bridge host | Unix socket | Local CodexUI; remote readiness only | Yes when both remote component owners are configured; otherwise no |
| 10 | Remote | Supported remote frontend endpoint | On bridge host | Supported network endpoint | Local CodexUI; remote readiness only | Yes with explicit remote management of both components; otherwise no |
| 11 | Remote on B | Supported remote frontend endpoint | Remote on A | Supported network endpoint | CodexUI on C; remote readiness only | Yes with SSH management of components on A and B; otherwise partial |

A local bridge may also be explicitly externally managed. In that case, the manager launches CodexUI and observes backend readiness without owning the bridge. Support combinations such as an SSH-managed app-server with an externally managed bridge. Whole-system supervision is available only when every component has a configured ownership path.

The most distributed setup is:

```text
Machine A                  Machine B                  Machine C
codex app-server ◄──────── codex-bridge ◄──────────── CodexUI
                                                         ↑
                                                    Profile Manager

Optional supervision from C: SSH → A and SSH → B
```

## Startup, readiness, and shutdown

- Follow component dependencies: prepare or verify app-server, prepare or verify bridge, then launch and connect CodexUI. For stdio, starting the bridge starts its app-server child.
- Prevent duplicate launches and identify existing managed processes reliably.
- Distinguish process running, endpoint reachable, and protocol ready. A listening socket is not app-server readiness.
- Display concrete startup stages, actionable failures, and partial operation when only some components are ready.
- Keep profiles operationally independent; management, logging, or a failure in one profile must not block the UI or another profile.
- Stop only processes owned by the selected profile's configured management mechanism. Use graceful shutdown before explicit escalation.
- Reconnect to existing owned processes after supervision interruptions instead of starting duplicates.
- Closing CodexUI does not automatically stop its bridge. Closing the manager window leaves running profiles alone.
- Distinguish application quit, profile stop, and profile removal, with clear choices about owned processes. Never delete a Codex home when removing a profile registration.

## Instructions for manually managed components

Whenever a component must be started externally, provide a detailed Setup & startup view. Generate exact copyable commands from the saved profile, including executable paths, `CODEX_HOME`, endpoints, and transport options. Manual commands and automatic launches must use the same authoritative launch definition.

For each component show:

1. Where to run the commands: local machine or named remote host.
2. Prerequisites: executable/version, authentication, directories, certificates, and endpoint access.
3. Exact command lines with their shell/platform assumptions.
4. Startup order and dependencies.
5. Verification commands, expected results, and readiness indicators.
6. Shutdown instructions for externally started processes.

Quote arguments correctly and reference secrets securely rather than embedding credentials in displayed commands, ordinary profile metadata, or logs.

```text
Machine A — Start app-server       [Copy command]
Machine B — Start codex-bridge     [Copy command]
Machine C — Check readiness        [Check]
Machine C — Launch CodexUI         [Open]
```

## SSH management

Configure the host, SSH connection settings, executable paths, environment, working directory, and remote process/service owner for each remotely managed component. Use established SSH authentication and host verification.

Retrieve component status and logs; distinguish SSH connectivity failure from component failure. Losing SSH means supervision is unavailable, not proof the process stopped. On reconnection, discover the existing owned process rather than start a duplicate. Do not terminate unrelated processes sharing an executable name or host.

Choose an explicit durable remote service/process-management mechanism during architecture design. Unattended remote components must not accidentally depend on an interactive SSH terminal staying open. Support bridge and app-server on separate hosts and multiple profile instances on each host.

## UI and UX

The visual direction is Codex Profiles: a polished Qt desktop control center with generous spacing, restrained shadows, rounded profile cards, consistent perceptual colors such as OKLCH-based tokens, and restrained motion. Reuse applicable shared UI foundations and honor accessibility, keyboard navigation, focus visibility, and reduced-motion preferences. Never communicate state by color alone.

### Overview

- Searchable profile cards show name, accent, location/ownership summary, readiness, and recent activity.
- Use state-dependent primary actions: Start & open, Starting…, Open CodexUI, or Show CodexUI when its window already exists. Repeated actions must not launch duplicate processes.
- Expose failures with View logs and specific recovery actions.
- Make simultaneously running profiles easy to distinguish. Carry the profile accent into its CodexUI window through an explicit integration.
- Show startup stages such as Starting bridge, Connecting app-server, Ready, and Opening CodexUI. Use subtle activity indicators rather than continuously pulsing entire cards.

### Profile details and creation

| View | Contents |
| --- | --- |
| Overview | Component topology, ownership, readiness, versions, recent activity, and recovery actions |
| Configuration | Identity, hosts, transports, endpoints, executables, Codex home, ownership, and lifecycle settings |
| Setup & startup | Host-specific commands, prerequisites, verification, and shutdown instructions |
| Logs | Component-specific local/remote output, filters, pause scrolling, copy, and export |

Place technical details such as socket paths and process IDs in expandable areas without hiding necessary configuration. Every collapsed or hidden widget must remain accessible through interaction. The new-profile flow must distinguish a new empty home from registering an existing environment, provide an accent choice and shared-instructions choice, and make authentication an explicit step.

Conversation counts, inference indicators, and related telemetry may be displayed only when supplied by supported authoritative APIs. Keep runtime readiness separate from turn activity; do not build another conversation-state implementation in the manager.

## Architecture, verification, and implementation boundaries

- Keep the manager a separate executable in the existing project and reuse applicable Qt, build, transport-configuration, validation, and UI infrastructure.
- Launch existing components instead of duplicating bridge, app-server, or conversation implementations. Preserve one clear owner for every process and one authority for each profile and launch definition.
- Keep credentials separate from ordinary metadata and redact them from logs and exported diagnostics.
- Follow the global branch, checkout, build-location, formatting, test-scope, and handover instructions and this repository's architecture gates.
- Verify the deployment matrix, simultaneous same-host profiles with different homes, endpoint collision handling, process ownership, startup failure, shutdown, SSH loss/reconnection, and isolation between profiles at relevant public boundaries. Do not claim scenarios passed merely because an endpoint accepts connections.
- Before implementation, decide the managed Unix app-server owner, remote service/process mechanism, profile persistence and credential references, installed capability/version discovery, and CodexUI profile integration.
- Existing stdio-child behavior and external provider transports must be distinguished from new manager functionality. Managed Unix startup, SSH supervision, and profile-specific CodexUI integration require design and explicit production implementation approval; this document is not a claim that those features already exist.

## Profile authentication (implementation authorized 2026-10-16)

Provide account status and interactive authentication directly in the manager, separately from process/transport readiness. Support ChatGPT device-code and browser login, OpenAI API-key entry, cancellation, account refresh and confirmed sign-out through the selected app-server. Device-code login must work with reachable remote/external app-servers without SSH; browser callback reachability belongs to the app-server host. Keep credentials owned by the app-server and exclude keys, login URLs and device codes from manager persistence/logging. Preserve independent homes and account state. Respect existing bridge controller ownership; an open CodexUI may release control explicitly and reclaim it after authentication. Provide host-specific CLI alternatives for unsupported methods and prevent connectivity-only startup from implying authentication readiness.

## Complete profile deletion

Implementation authorized on 2026-10-16. Keep registration-only removal separate from permanent profile-data deletion. Preview exact hosts and paths, require the profile name and exclusive-use confirmation, stop only verified owned components, protect default/shared homes, and remove the registration only after cleanup succeeds. Use the same guarded operation locally, over configured SSH, and in manually runnable instructions. Failure must retain a recoverable registration and disclose possible partial deletion. Deleting local credential files must not be represented as revoking OpenAI authorization.
