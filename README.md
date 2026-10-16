<a id="banner"></a>
<!-- snodec:begin header -->
<a name="project-overview"></a>

<p align="center">
  <picture>
    <source media="(max-width: 600px)" srcset="docs/media/hero-mobile.svg">
  <img src="docs/media/readme-hero.svg" alt="Codex(W)UI — A workspace for the whole coding conversation." width="100%">
  </picture>
</p>

<!-- snodec:begin organization -->
<p><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="docs/media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="docs/media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="docs/media/nav/account-dark.svg"><img align="right" src="docs/media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="docs/media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="docs/media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="docs/media/nav/organization-dark.svg"><img align="right" src="docs/media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="docs/media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="docs/media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="docs/media/nav/repository-dark.svg"><img align="right" src="docs/media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end organization -->

# Codex(W)UI

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/media/snodec-chip-dark.svg">
  <img src="docs/media/snodec-chip-light.svg" alt="Applications · App" width="240" height="32">
</picture>
<!-- snodec:end header -->

<!-- snodec:begin status -->
[![CI](https://img.shields.io/github/actions/workflow/status/SNodeC/CodexUI/build-and-qualify.yml?branch=master&label=CI&style=flat)](https://github.com/SNodeC/CodexUI/actions/workflows/build-and-qualify.yml) [![License: MIT or LGPL 3.0 or later](https://img.shields.io/badge/License-MIT%20OR%20LGPL--3.0--or--later-334155?style=flat)](LICENSE)
<!-- snodec:end status -->

**Follow the work, not just the final answer.** Codex(W)UI offers two frontends: **CodexUI**, a native Qt desktop application, and **CodexWUI**, a browser application connecting over WebSocket. They bring conversations, commands, plans, approvals and file changes into a visual coding workspace.

Built on [AISuite](https://github.com/SNodeC/AISuite#banner) and [SNode.C](https://github.com/SNodeC/snode.c#banner), both frontends connect through AISuite's `codex-bridge`. Codex app-server remains responsible for agent execution and persistent history; both applications give you the controls and visibility around it.

[CodexManager](docs/guides/profile-manager.md) is the companion Qt profile manager, built as `codex-manager` alongside `codex-ui`. Configure independent homes, local/SSH/external ownership and connections, then start a separate CodexUI window for each profile. Sign in through the manager using browser login, a device code or an API key; remove a registration or explicitly delete an exclusively owned profile and its data.

<!-- snodec:begin menu -->
<p>
  <a href="#quick-start" title="Start"><img src="docs/media/menu/start.svg" alt="Start" width="160" height="28"></a>
  <a href="#install" title="Install"><img src="docs/media/menu/install.svg" alt="Install" width="160" height="28"></a>
  <a href="docs/guides/build-from-source.md#banner" title="Build"><img src="docs/media/menu/build.svg" alt="Build" width="160" height="28"></a>
  <a href="#first-run" title="Use"><img src="docs/media/menu/use.svg" alt="Use" width="160" height="28"></a>
  <a href="#configuration" title="Configure"><img src="docs/media/menu/configure.svg" alt="Configure" width="160" height="28"></a>
  <a href="#architecture" title="Architecture"><img src="docs/media/menu/architecture.svg" alt="Architecture" width="160" height="28"></a>
  <a href="#contributing" title="Contribute"><img src="docs/media/menu/contribute.svg" alt="Contribute" width="160" height="28"></a>
</p>

<!-- snodec:end menu -->

## Quick start

1. [Install](#install): prepare the native or browser frontend.
2. [Use](#first-run): connect your chosen frontend and send a prompt.
3. [Configure](#configuration): review runtime settings and access controls before deployment.

## What you see

### The workspace

[![Native CodexUI showing a conversation, completed command output, turn settings and the Inspector.](docs/media/native-workspace.png)](docs/media/native-workspace.png)

*Actual native application, captured during an isolated qualification session with a disposable demonstration thread. Click the image to inspect it at full resolution. [Image provenance](docs/media/README.md).*

- **Organize the work:** browse threads, search titles, archive work and return to retained history. In the native app, group threads by projects and shared sections.
- **Follow execution:** read streamed responses, reasoning summaries and command output in one conversation. Send prompts, steer an active turn, stop work and respond to approval requests.
- **Inspect results:** keep plans, agent activity, requests, changes and timing details beside the conversation. Review repository diffs, copy output and inspect the protocol when diagnosing a problem.

### A conversation you can work with

- **Rich, distinct activity cards.** Markdown, commands, file changes, plans, images and agent activity keep their own controls and presentation.
- **History without loading every widget.** Paginated history and virtualized native rendering keep offscreen content as data rather than another renderer.
- **Input beyond plain text.** The native composer accepts image paste and local file drag/drop, alongside the attachment picker and multiline editing. Pasted images are stored as durable local PNG files with no automatic expiry; review retained files when handling sensitive material.
- **Context when you need it.** Supplied timestamps, durations, token information and detailed Inspector views make execution easier to understand.

### Projects and sections, without duplicating threads

In the native threads panel, **projects contain the visual grouping; sections organize the threads within it**. A section can appear in multiple projects because project and section memberships are independent. Standalone sections and ungrouped threads remain visible too.

To create a project or section, click the current grouping label—**Projects**, **Sections** or **Ungrouped**—above the thread list. Use a thread's context menu to assign its project and section. Creation and assignment require controller access and server support.

Next: [read the grouping and lifecycle guide](docs/guides/thread-projects.md#banner).

### Choose your frontend

| | CodexUI | CodexWUI |
| --- | --- | --- |
| Interface | Qt 6 Widgets | React / TypeScript |
| Connection | SNode.C transports | WebSocket |
| Install | Desktop executable | Static web assets |

**CodexUI** integrates with the Linux desktop for local workspaces, project/section organization and desktop file interaction. Installation provides `codex-ui` and the companion `codex-manager`, with desktop entries and icons. Available transports depend on the build.

**CodexWUI** (build/install component `CodexWebUI`) connects through AISuite's frontend SDK without installing the Qt application. `codex-bridge` serves the built web assets; no Node runtime is required.

The frontends share protocol and lifecycle meaning, **not an identical feature inventory**. Native project/section management is not yet a browser feature. See the [native-only features and browser limitations](docs/web/contract-1.0.md#native-only-in-10) before choosing a deployment.

## Install

Build [native Qt desktop](docs/guides/build-from-source.md#native-desktop) or [browser assets](docs/guides/build-from-source.md#browser-frontend) served by codex-bridge; follow the build guide for prerequisites and commands.

## First run

<a id="3-connect-and-work"></a>

### Managed native profiles

Run the installed `codex-manager` (or `./build/codex-manager` with the build layout below), create a profile, and choose **Account / Sign in…** to authenticate its app-server. Use **Start & open** to launch its separate CodexUI window. Each profile has its own `CODEX_HOME`; multiple profiles can run concurrently on the same machine. Managed local/SSH services require a working systemd user manager, while externally managed components have explicit startup instructions.

The profile action menu distinguishes **Remove from manager…**, which preserves data, from **Delete profile and its data…**, which previews exact directories, requires typed confirmation and protects default/shared homes. Full deletion requires Linux and Python 3.11+ on the target host. See [profiles, authentication and deletion](docs/guides/profile-manager.md) for prerequisites, ownership and remote deployment details.

### Native desktop

**You need:** the native build, a configured Codex executable available to the bridge user, and AISuite's `codex-bridge`. The bridge's generated protocol targets Codex 0.162.1; use the [AISuite entry page](https://github.com/SNodeC/AISuite#banner) to check compatibility before changing Codex versions.

Start `codex-bridge --config-file /dev/null` as your own user in one terminal. In a second terminal, run from this checkout:

```sh
./build/codex-ui
```

The default native transport is Unix JSONL, using the same private socket as the bridge: a valid private `XDG_RUNTIME_DIR` plus `/codex-bridge.sock`, otherwise `/tmp/codex-bridge-<uid>/codex-bridge.sock`. In **Connection**, select the built transport, verify its **Socket path** (or host/port for a network transport), and choose **Apply and connect**. Dialog changes apply to this UI session; command-line/SNode.C configuration supplies startup defaults.

Wait for the provider to become ready. If you are an observer and no controller owns the session, choose **Claim control**. Controller ownership authorizes mutations on this bridge connection; it is not a login or a replacement for Codex approval/sandbox settings. Choose or create a thread and send a prompt in a disposable workspace.

**Expected result:** the thread list loads, the UI identifies your controller role, and a submitted prompt receives streamed activity and a response. A connection without provider readiness is not a successful agent run. Model requests may incur provider charges; avoid private workspaces while trying the example.

### Browser edition

After [building the browser assets](docs/guides/build-from-source.md#browser-frontend), replace the artifact path below with your browser build output. Run the installed bridge with WebSocket support enabled and port **8080** free; the current directory does not matter:

```sh
codex-bridge --config-file /dev/null codex --bridge-web-root /path/to/build/codex-web/web/app-dist \
  codex-bridge-websocket-ipv4 --disabled=false \
  local --host 127.0.0.1 --port 8080
```

Open **http://127.0.0.1:8080/**. The page and `/codex` WebSocket share that loopback listener. Connect, claim control if it is vacant, and use a disposable thread for a first prompt. Expect the thread list and streamed response, as with the native route. For installed assets, endpoint overrides and remote deployment, use the [browser deployment guide](web/README.md#deployment); no Node server is required.

If the first run fails, check [connection and controller ownership](docs/guides/interaction-guide.md#connection-and-controller-ownership) before retrying the prompt.

## Configuration

For native endpoints, use **Connection** as described in [First run](#first-run); startup defaults come from command-line/SNode.C settings, while the dialog applies to the current session. Browser endpoints and retained URL settings are documented in [browser deployment](web/README.md#deployment).

Controller/observer roles are routing permissions, **not authentication or tenant isolation**. Observers can receive session broadcasts and read approved workspace data, including files. Keep the Unix socket private and browser/network listeners on loopback unless an external authenticated TLS boundary protects them. App-server configuration remains authoritative for model selection, approvals and sandbox permissions.

See [Connection and controller ownership](docs/guides/interaction-guide.md#connection-and-controller-ownership) for endpoint and startup configuration; see [Using CodexUI](docs/guides/interaction-guide.md#using-codexui) for interaction details and [Architecture](#architecture) for component ownership.

<a id="how-it-fits-together"></a>

## Architecture

**CodexUI / CodexWUI** ↔ **AISuite bridge** ↔ **Codex app-server**

The bridge allows multiple frontends to follow the same app-server session, with one controller at a time and policy-limited observers. Controller status is distinct from the agent's approval and sandbox settings.

On the native side, a SNode.C worker owns protocol processing and writes one shared `NodeGraph`; Qt projects that state into widgets. The browser uses AISuite's TypeScript SDK and its own presentation state. Neither frontend replaces app-server's persistent history or tool execution.

For the detailed state, threading and protocol boundaries, see the [architecture guide](docs/architecture/overview.md#banner).

## Documentation

Manual CI builds and qualifies the desktop and browser editions, including shared verification and frontend SDK checks. See the organization [workflow graph](https://github.com/SNodeC/.github/blob/main/docs/workflow-graph.md#banner) for triggers and artifact boundaries.

[Documentation index](docs/README.md#banner): guides, architecture, qualification and engineering records for CodexUI.

| Start here | Go deeper |
| --- | --- |
| [CodexManager profiles, sign-in and deletion](docs/guides/profile-manager.md) | [Manager requirements and ownership](docs/architecture/profile-manager-requirements.md) |
| [Using CodexUI](docs/guides/interaction-guide.md#banner) | [Native state and threading](docs/architecture/shared-node-graph.md#banner) |
| [Projects and sections](docs/guides/thread-projects.md#banner) | [Architecture overview](docs/architecture/overview.md#banner) |
| [Timestamps and durations](docs/architecture/timestamp-presentation.md#banner) | [Browser contract](docs/web/contract-1.0.md#banner) |
| [Browser build and deployment](web/README.md#banner) | [Web qualification](docs/web/qualification.md#banner) · [Release process](docs/web/release-process.md#banner) |

For bug reports, include the revision, Qt version, connection type and the smallest reproducible sequence. Remove credentials and private conversation content from logs and screenshots.

## Platforms

The documented native platform is Linux with Qt 6.6+ Widgets. Browser frontend scope, coverage and limitations are defined by the [browser contract](docs/web/contract-1.0.md#banner); they are not a promise of native/browser feature parity. Review the [known limitations](docs/guides/interaction-guide.md#known-limitations) before relying on approval handling or native performance.

## Releases

No GitHub release or shared binary-package distribution is published yet; the documented installation routes build from source. Future release notes belong on the [releases page](https://github.com/SNodeC/CodexUI/releases). The [web release process](docs/web/release-process.md#banner) describes development qualification, not a downloadable product release.

## Contributing

[Engineering instructions for coding agents](AGENTS.md).

[Report an issue](https://github.com/SNodeC/CodexUI/issues) using the sanitized information in [Documentation](#documentation); read the existing [agent/engineering instructions](AGENTS.md) and [build/qualification guide](docs/guides/build-from-source.md#quality-and-development).

<!-- snodec:begin ecosystem -->
## Ecosystem

Depends on [AISuite](https://github.com/SNodeC/AISuite#banner) and [SNode.C](https://github.com/SNodeC/snode.c#banner)
<!-- snodec:end ecosystem -->

<!-- snodec:begin footer -->
Choose either [MIT](LICENSE-MIT) or [LGPL-3.0-or-later](LICENSE-LGPL-3.0-or-later).

Built with [Qt](https://www.qt.io/). Codex(W)UI is an independent project; neither frontend is an official OpenAI application.

Maintainer: [Volker Christian](https://github.com/VolkerChristian#banner). [Organization](https://github.com/SNodeC#banner) · [Contributing](#contributing) · [Security](https://github.com/SNodeC/.github/blob/main/SECURITY.md)
<!-- snodec:end footer -->
