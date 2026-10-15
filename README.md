<!-- snodec:begin header -->
<a name="project-overview"></a>

<p align="center">
  <picture>
    <source media="(max-width: 600px)" srcset="docs/media/hero-mobile.svg">
  <img src="docs/media/readme-hero.svg" alt="Codex(W)UI — A workspace for the whole coding conversation." width="100%">
  </picture>
</p>

<!-- snodec:begin organization -->
<p><a href="https://github.com/SNodeC" title="SNode.C organization"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/media/nav/organization-dark.svg"><img align="right" src="docs/media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization" width="160" height="28"></picture></a><br clear="all"></p>
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

Built on [AISuite](https://github.com/SNodeC/AISuite#project-overview) and [SNode.C](https://github.com/SNodeC/snode.c#project-overview), both frontends connect through AISuite's `codex-bridge`. Codex app-server remains responsible for agent execution and persistent history; both applications give you the controls and visibility around it.

<!-- snodec:begin menu -->
<p>
  <a href="#quick-start" title="Start"><img src="docs/media/menu/start.svg" alt="Start" width="160" height="28"></a>
  <a href="#install" title="Install"><img src="docs/media/menu/install.svg" alt="Install" width="160" height="28"></a>
  <a href="docs/guides/build-from-source.md#page-overview" title="Build"><img src="docs/media/menu/build.svg" alt="Build" width="160" height="28"></a>
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

Next: [read the grouping and lifecycle guide](docs/guides/thread-projects.md#page-overview).

### Choose your frontend

| | CodexUI | CodexWUI |
| --- | --- | --- |
| Interface | Qt 6 Widgets | React / TypeScript |
| Connection | SNode.C transports | WebSocket |
| Install | Desktop executable | Static web assets |

**CodexUI** integrates with the Linux desktop for local workspaces, project/section organization and desktop file interaction. Installation provides `codex-ui`, a desktop entry and an icon. Available transports depend on the build.

**CodexWUI** (build/install component `CodexWebUI`) connects through AISuite's frontend SDK without installing the Qt application. `codex-bridge` serves the built web assets; no Node runtime is required.

The frontends share protocol and lifecycle meaning, **not an identical feature inventory**. Native project/section management is not yet a browser feature. See the [native-only features and browser limitations](docs/web/contract-1.0.md#native-only-in-10) before choosing a deployment.

## Install

Build [native Qt desktop](docs/guides/build-from-source.md#native-desktop) or [browser assets](docs/guides/build-from-source.md#browser-frontend) served by codex-bridge; follow the build guide for prerequisites and commands.

## First run

<a id="3-connect-and-work"></a>

### Native desktop

**You need:** the native build, a configured Codex executable available to the bridge user, and AISuite's `codex-bridge`. The bridge's generated protocol targets Codex 0.162.1; use the [AISuite entry page](https://github.com/SNodeC/AISuite#project-overview) to check compatibility before changing Codex versions.

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

For the detailed state, threading and protocol boundaries, see the [architecture guide](docs/architecture/overview.md#page-overview).

## Documentation

Manual CI builds and qualifies the desktop and browser editions, including shared verification and frontend SDK checks. See the organization [workflow graph](https://github.com/SNodeC/.github/blob/main/docs/workflow-graph.md#page-overview) for triggers and artifact boundaries.

[Documentation index](docs/README.md#page-overview): guides, architecture, qualification and engineering records for CodexUI.

| Start here | Go deeper |
| --- | --- |
| [Using CodexUI](docs/guides/interaction-guide.md#page-overview) | [Native state and threading](docs/architecture/shared-node-graph.md#page-overview) |
| [Projects and sections](docs/guides/thread-projects.md#page-overview) | [Architecture overview](docs/architecture/overview.md#page-overview) |
| [Timestamps and durations](docs/architecture/timestamp-presentation.md#page-overview) | [Browser contract](docs/web/contract-1.0.md#page-overview) |
| [Browser build and deployment](web/README.md#page-overview) | [Web qualification](docs/web/qualification.md#page-overview) · [Release process](docs/web/release-process.md#page-overview) |

For bug reports, include the revision, Qt version, connection type and the smallest reproducible sequence. Remove credentials and private conversation content from logs and screenshots.

## Platforms

The documented native platform is Linux with Qt 6.6+ Widgets. Browser frontend scope, coverage and limitations are defined by the [browser contract](docs/web/contract-1.0.md#page-overview); they are not a promise of native/browser feature parity. Review the [known limitations](docs/guides/interaction-guide.md#known-limitations) before relying on approval handling or native performance.

## Releases

No GitHub release or shared binary-package distribution is published yet; the documented installation routes build from source. Future release notes belong on the [releases page](https://github.com/SNodeC/CodexUI/releases). The [web release process](docs/web/release-process.md#page-overview) describes development qualification, not a downloadable product release.

## Contributing

[Engineering instructions for coding agents](AGENTS.md).

[Report an issue](https://github.com/SNodeC/CodexUI/issues) using the sanitized information in [Documentation](#documentation); read the existing [agent/engineering instructions](AGENTS.md) and [build/qualification guide](docs/guides/build-from-source.md#quality-and-development).

<!-- snodec:begin ecosystem -->
## Ecosystem

Depends on [AISuite](https://github.com/SNodeC/AISuite#project-overview) and [SNode.C](https://github.com/SNodeC/snode.c#project-overview)
<!-- snodec:end ecosystem -->

<!-- snodec:begin footer -->
Choose either [MIT](LICENSE-MIT) or [LGPL-3.0-or-later](LICENSE-LGPL-3.0-or-later).

Built with [Qt](https://www.qt.io/). Codex(W)UI is an independent project; neither frontend is an official OpenAI application.

Maintainer: [Volker Christian](https://github.com/VolkerChristian). [Organization](https://github.com/SNodeC) · [Contributing](#contributing) · [Security](https://github.com/SNodeC/.github/blob/main/SECURITY.md)
<!-- snodec:end footer -->
