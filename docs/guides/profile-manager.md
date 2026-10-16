# CodexManager

CodexManager (`codex-manager`) manages independent Codex profiles and opens a separate native CodexUI process for each one. It is built and installed alongside `codex-ui`. Each profile has its own `CODEX_HOME`, endpoints, service identities, window title, accent and Qt settings. Conversations, projects, sections and inference remain owned by the existing app-server and CodexUI implementations.

![CodexManager with three disposable demonstration profiles.](../media/codex-manager-overview.png)

[Qualification and implementation record](../qualification/codexmanager-2026-10-16.md), including validation limits and screenshot provenance.

## Start

Build `codex-manager` and `codex-ui` using the repository's normal CMake configuration, or install both through the normal install target. The manager requires Qt Widgets and Qt Network. Managed processes require Linux systemd user services and `flock`; remote management additionally requires the OpenSSH client. The existing AISuite `codex-bridge` and `codex-bridge-client`, and the selected Codex executable, must be available on their respective hosts. A source build of the manager prefers its sibling `codex-ui` when creating a profile.

```bash
codex-manager
```

Choose **New profile**, give it a name and choose its home and accent. Keep the defaults for a local bridge with a bridge-owned stdio app-server, or configure the two links independently in **Bridge** and **App-server**. Register an existing local home through **Choose an existing local Codex home**. Saving a registration does not create its home or copy authentication.

**Local tools** checks the configured UI and readiness client asynchronously and disables frontend transports absent from either executable. Before starting an inactive managed component, the manager asks that component to validate the generated configuration on its own host. Unsupported configurations fail before the managed launch. TLS certificate paths belong to the bridge host; the client CA path belongs to the manager/UI host. SNI selects the bridge server name; certificate-chain trust uses the existing transport implementation. SNI alone does not establish hostname verification. Configure valid IPv6 or Bluetooth addresses when choosing those transports. The current app-server provider supports stdio and Unix/IPv4/IPv6 WebSockets; its WebSocket path is `/`.

Open **Details → Setup & startup** for exact commands, including profile-specific authentication on the app-server host. Authenticate there before requesting model work. The manager does not copy credentials or run an interactive login. Starting a locally managed owner creates a missing home with owner-only permissions. The optional shared-instructions choice links an existing local `~/.codex/AGENTS.md` only when the profile has no existing `AGENTS.md`. Remote homes and authentication must be prepared on their host using the displayed commands.

**Start & open** checks the owners, starts missing managed components in dependency order, waits for bridge protocol/provider readiness, and opens CodexUI. **Show CodexUI** activates the profile's existing window. Each UI process retains its own event loop and existing session runtime. Readiness monitoring uses the existing bridge client as an observer and releases control before launching the UI.

## Ownership and remote hosts

| Ownership | Behavior |
| --- | --- |
| Managed locally | Uses a stable per-profile systemd user service on this machine. |
| Managed over SSH | Uses that host's systemd user service through noninteractive, host-key-verified SSH. |
| Started externally | Displays foreground startup/shutdown commands and observes readiness; never stops the external process. |

For stdio, the bridge owns its child app-server; the separate app-server ownership fields do not apply. For Unix providers, choose local, SSH or external ownership. Network app-servers must be started externally or explicitly managed over SSH, as required by the deployment policy. A Unix provider must share its host with the bridge. CodexUI cannot directly reach a remote Unix frontend; use a network frontend or configure a separately prepared tunnel as an external local endpoint.

A local bridge is manageable with every supported frontend transport. Bridge and app-server can run on different hosts and be managed independently over SSH. SSH authentication uses the configured alias, optional identity-file reference, agent and normal SSH configuration. Known host keys must already be verified; the manager never accepts unknown keys automatically. The remote account needs a working systemd user manager; for unattended services after logout, arrange its lifetime with the host administrator, for example through linger. The manager does not change host-wide service policy.

Lost SSH supervision means **status unknown**, even when a prior process was running. On reconnect the manager checks and adopts the same service instead of starting a second copy. Service description and identity are checked before adoption and shutdown. Do not edit or remove a profile while its owned processes are running or their status is unknown; restore supervision and stop them first.

## Lifetime and isolation

Owned units use `codexmanager-<profile-uuid>-{appserver,bridge,ui}.service`. Backend services persist independently of the manager or an SSH terminal. An owning launch holds a nonblocking lock in its effective `CODEX_HOME`, preventing two manager stores from launching that same managed environment. Registration validation rejects shared homes, duplicate Unix sockets and conflicting listener ports within the known profile inventory. Local home aliases are resolved when possible; distinct SSH aliases must consistently identify the same machine because the manager cannot prove that two arbitrary aliases refer to one host.

**Stop profile** stops only verified owned components in reverse dependency order. Systemd first sends its normal termination signal and escalates after the configured 15-second stop timeout. External components remain running. Closing CodexUI leaves its bridge running. Closing the manager window hides it to the tray, or minimizes it when a tray is unavailable. **Quit manager** leaves services and UI windows running. **Remove registration** never deletes a home, authentication or conversation history. To restart, stop the profile, then start it again.

Distinct homes separate Codex state; they are not filesystem security boundaries. Network access controls and TLS remain deployment responsibilities. Environment overrides support non-secret `KEY=value` entries. `CODEX_HOME` and UI identity variables come from the profile; secret-like variable names are rejected. Keep secret values in appropriate host-side credential stores. Known bearer/API token forms are redacted in displayed/exported service logs; arbitrary process output cannot be guaranteed secret-free.

## Storage and diagnostics

Profiles are stored as versioned JSON under Qt's `AppConfigLocation` for **CodexManager**, normally `~/.config/CodexManager/profiles.json`. Writes are atomic and owner-readable/writable; an invalid store is never silently replaced. Import/export copies configuration references, not home contents or credentials. `codex-manager --profiles /absolute/path/profiles.json` selects a different registry. A lock prevents two managers from writing the same registry concurrently.

Details contains **Overview**, **Configuration**, **Setup & startup**, and **Logs** tabs. Technical paths can be expanded. Logs support refresh from local/remote journals, filtering, paused updates, copy and export. The log buffer and helper-command output are bounded. Process running, connection state and protocol readiness are displayed separately; no conversation or inference counts are synthesized.

## Development qualification

The `codexmanager` CTest entry exercises profile storage, deployment commands, quoting, conflicts, independent service lifecycles, SSH loss/adoption, ownership refusal, failed/cancelled startup, native window identity/activation, installed bridge/client/UI command parsing, and actual manager widgets. Service/SSH fixtures are explicitly simulated. Its installed-configuration checks require AISuite's bridge/client executables on `PATH`; TLS fixture files validate option parsing only, not a TLS handshake.

Run through Xvfb with the offscreen Qt plugin. Repeat geometry cases at DPR 1.0, 1.25, 1.5 and 2.0. `CODEXMANAGER_EVIDENCE_DIR` selects a screenshot directory. The separate `liveLocalProfiles` test requires `CODEXMANAGER_LIVE_QUALIFICATION=1`; it creates disposable homes and uniquely named real systemd services, starts the actual bridge/app-server/UI for stdio and Unix providers, checks both windows' activation sockets, stops one while the other remains ready, and cleans up owned services. Run it explicitly against the rebuilt `codex-ui`; it does not authenticate or request inference. Live remote hosts, TLS handshakes and Bluetooth hardware need separate environment-specific qualification.
