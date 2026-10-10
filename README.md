<!-- snodec:begin header -->
<a name="project-overview"></a>

<p align="center">
  <img src="docs/media/readme-hero.svg" alt="Codex(W)UI — A workspace for the whole coding conversation." width="100%">
</p>

# Codex(W)UI

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/media/snodec-chip-dark.svg">
  <img src="docs/media/snodec-chip-light.svg" alt="Applications · App" width="240" height="32">
</picture>
<!-- snodec:end header -->

<!-- snodec:begin status -->
[![CI](https://github.com/SNodeC/CodexUI/actions/workflows/ci.yml/badge.svg)](https://github.com/SNodeC/CodexUI/actions/workflows/ci.yml) · [![Licence: MIT or LGPL 3.0 or later](https://img.shields.io/badge/Licence-MIT%20OR%20LGPL--3.0--or--later-334155?style=flat)](LICENSE)
<!-- snodec:end status -->

**Follow the work, not just the final answer.** Codex(W)UI offers two frontends: **CodexUI**, a native Qt desktop application, and **CodexWUI**, a browser application connecting over WebSocket. They bring conversations, commands, plans, approvals and file changes into a visual coding workspace.

Built on [AISuite](https://github.com/SNodeC/AISuite#project-overview) and [SNode.C](https://github.com/SNodeC/snode.c#project-overview), both frontends connect through AISuite's `codex-bridge`. Codex app-server remains responsible for agent execution and persistent history; both applications give you the controls and visibility around it.

<!-- snodec:begin menu -->
<p>
  <a href="#quick-start" title="Start"><img src="docs/media/menu/snodec-start-108.svg" alt="Start" width="108" height="24"></a>
  <a href="#install" title="Install"><img src="docs/media/menu/snodec-install-108.svg" alt="Install" width="108" height="24"></a>
  <a href="#build-from-source" title="Build"><img src="docs/media/menu/snodec-build-108.svg" alt="Build" width="108" height="24"></a>
  <a href="#first-run" title="Use"><img src="docs/media/menu/snodec-use-108.svg" alt="Use" width="108" height="24"></a>
  <a href="#configuration" title="Configure"><img src="docs/media/menu/snodec-configure-108.svg" alt="Configure" width="108" height="24"></a>
  <a href="#architecture" title="Architecture"><img src="docs/media/menu/snodec-architecture-108.svg" alt="Architecture" width="108" height="24"></a>
  <a href="#contributing" title="Contribute"><img src="docs/media/menu/snodec-contribute-108.svg" alt="Contribute" width="108" height="24"></a>
</p>

<!-- snodec:end menu -->

## What you see

### Quick start

1. [Install](#install) — prepare the native or browser frontend.
2. [Use](#first-run) — connect the native frontend and send a prompt; the browser route has its own launch guide.
3. [Configure](#configuration) — review runtime settings and access controls before deployment.

### The workspace

[![Native CodexUI showing a conversation, completed command output, turn settings and the Inspector.](docs/media/native-workspace.png)](docs/media/native-workspace.png)

*Actual native application, captured during an isolated qualification session with a disposable demonstration thread. Click the image to inspect it at full resolution. [Image provenance](docs/media/README.md).*

- **Organize the work:** browse threads, search titles, archive work and return to retained history. In the native app, group threads by projects and shared sections.
- **Follow execution:** read streamed responses, reasoning summaries and command output in one conversation. Send prompts, steer an active turn, stop work and respond to approval requests.
- **Inspect results:** keep plans, agent activity, requests, changes and timing details beside the conversation. Review repository diffs, copy output and inspect the protocol when diagnosing a problem.

#### A conversation you can work with

- **Rich, distinct activity cards.** Markdown, commands, file changes, plans, images and agent activity keep their own controls and presentation.
- **History without loading every widget.** Paginated history and virtualized native rendering keep offscreen content as data rather than another renderer.
- **Input beyond plain text.** The native composer accepts image paste and local file drag/drop, alongside the attachment picker and multiline editing.
- **Context when you need it.** Supplied timestamps, durations, token information and detailed Inspector views make execution easier to understand.

#### Projects and sections, without duplicating threads

In the native threads panel, **projects contain the visual grouping; sections organize the threads within it**. A section can appear in multiple projects because project and section memberships are independent. Standalone sections and ungrouped threads remain visible too.

To create a project or section, click the current grouping label—**Projects**, **Sections** or **Ungrouped**—above the thread list. Use a thread's context menu to assign its project and section. Creation and assignment require controller access and server support.

[![Read the grouping and lifecycle guide →](docs/media/menu/further-grouping-lifecycle.svg)](docs/thread-projects-sections.md)

### Choose your frontend

| | CodexUI | CodexWUI |
| --- | --- | --- |
| Interface | Qt 6 Widgets | React / TypeScript |
| Connection | SNode.C transports | WebSocket |
| Install | Desktop executable | Static web assets |

**CodexUI** integrates with the Linux desktop for local workspaces, project/section organization and desktop file interaction. Installation provides `codex-ui`, a desktop entry and an icon. Available transports depend on the build.

**CodexWUI** connects through AISuite's frontend SDK without installing the Qt application. `codex-bridge` serves the built web assets; no Node runtime is required.

The frontends share protocol and lifecycle meaning, **not an identical feature inventory**. Native project/section management is not yet a browser feature. See the [browser scope and limitations](docs/web-1.0-contract.md) before choosing a deployment.

## Install

Choose your route.

**Native desktop** — Prepare the prerequisites below, [build CodexUI](#build-from-source), then optionally install the build.

**Browser frontend** — Follow [Browser edition](#browser-edition) to build static assets served by `codex-bridge`. No Qt installation or Node runtime is needed to run them.

These are source-installation routes, not a binary-package release promise.

<a id="get-started"></a><a id="1-prepare-the-dependencies"></a><a id="prepare-the-dependencies"></a>

### Prerequisites

The native build requires a C++20 toolchain, CMake 3.20+, Ninja, Qt 6.6+ Widgets, pkg-config, libgit2 development files, and installed **SNode.C** and **AISuite** packages. On Debian/Ubuntu, the libgit2 development package is `libgit2-dev`.

Build [SNode.C](https://github.com/SNodeC/snode.c#project-overview) and [AISuite](https://github.com/SNodeC/AISuite#build-from-source) from their `master` branches. AISuite must export `AISuite::OpenAICodex`. Running the application also requires a configured Codex app-server through `codex-bridge`.

### Install the build

From the same checkout and build directory used for the native build, optionally install into `/usr/local` to add the executable, desktop entry and application icon. For installation without administrator privileges, configure a writable `CMAKE_INSTALL_PREFIX` and omit `sudo`:

```sh
sudo cmake --install build
```

## First run

<a id="3-connect-and-work"></a>

### Connect and work

Start your configured `codex-bridge`, then run:

```sh
./build/codex-ui
```

Use **Connection** to select its endpoint. Acquire control if necessary, choose or create a thread, and send a prompt. App-server configuration determines available models, permissions and approval behavior.

For the browser frontend, continue with [Browser edition](#browser-edition) and its [browser deployment guide](web/README.md); the native command above is not a browser launch command.

## Configuration

Use the **Connection** controls described in [First run](#first-run), and configure the bridge endpoint for the selected frontend. [UI behavior](docs/ui-behavior.md), [browser endpoint configuration](web/README.md) and [controller/observer boundaries](#how-it-fits-together) describe the existing controls. App-server configuration remains authoritative for models, permissions and approvals.

## Build from source

<a id="2-build-the-native-application"></a>

### Build the native application

Prepare the [dependencies](#1-prepare-the-dependencies) listed under Install before configuring this source build.

From this repository's root, replace the two installation prefixes below:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCODEXUI_INSTALL_WEB=OFF \
  -DCMAKE_PREFIX_PATH="/path/to/aisuite;/path/to/snodec"
cmake --build build --parallel 14
```

This is an explicit **native-only** build. For a combined install, build the web artifact first as described below, then configure with `CODEXUI_INSTALL_WEB=ON`.

For the browser build and its required checkout layout, see [Browser edition](#browser-edition).

## Platforms

The native application integrates with the Linux desktop using Qt 6 Widgets. The browser frontend is a static web application served by `codex-bridge`; it does not need a Node runtime after installation. The [frontend comparison](#choose-your-frontend) and [browser contract](docs/web-1.0-contract.md) retain the feature and deployment boundaries.

<a id="how-it-fits-together"></a>

## Architecture

**CodexUI / CodexWUI** ↔ **AISuite bridge** ↔ **Codex app-server**

The bridge allows multiple frontends to follow the same app-server session, with one controller at a time and policy-limited observers. Controller status is distinct from the agent's approval and sandbox settings.

On the native side, a SNode.C worker owns protocol processing and writes one shared `NodeGraph`; Qt projects that state into widgets. The browser uses AISuite's TypeScript SDK and its own presentation state. Neither frontend replaces app-server's persistent history or tool execution.

For the detailed state, threading and protocol boundaries, see the [architecture guide](docs/codex-architecture.md).

## Browser edition

The browser release build needs **Node.js 22+**, **Chrome or Chromium** for browser qualification, and AISuite's frontend SDK source. Set `CHROME_BIN` if the browser is not installed at one of the usual system paths. Its local package dependency currently expects this checkout layout:

```text
workspace/
├── AISuite-extraction/AISuite-final/   # SNodeC/AISuite, master
└── CodexUI/codexui/                  # this repository
```

From `CodexUI/codexui/`:

```sh
npm ci --prefix ../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm test --prefix ../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm ci --prefix web
npm run release --prefix web
```

The output is `web/app-dist/`. A combined install places it in `share/codexui/web`; the bridge can serve the application and `/codex` WebSocket from one listener. See [browser packaging and deployment](web/README.md) for standalone installation and endpoint configuration.

## Quality and development

Install Xvfb and `xauth`, then run native checks in the documented isolated environment:

```sh
xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build --output-on-failure --parallel 14
```

The repository includes shared native/browser presentation fixtures, native interaction and geometry checks, and workload benchmarks. Selected native suites exercise DPR **1.0, 1.25, 1.5 and 2.0**. CI's accepted elapsed-time policy reports timing overruns; correctness and work-count checks remain enforced.

The [interactive qualification inventory](docs/native-ui-ux-qualification-inventory.md) defines the wider visual and UX acceptance scope. Its [execution record](docs/native-ui-ux-qualification-results.md) remains incomplete: a green CI run is not a claim of complete desktop, accessibility or hardware touchpad qualification.

## Documentation

| Start here | Go deeper |
| --- | --- |
| [UI behavior](docs/ui-behavior.md) | [Native state and threading](docs/two-thread-shared-node-graph.md) |
| [Projects and sections](docs/thread-projects-sections.md) | [Architecture overview](docs/codex-architecture.md) |
| [Timestamps and durations](docs/architecture/timestamp-presentation.md) | [Browser contract](docs/web-1.0-contract.md) |
| [Browser build and deployment](web/README.md) | [Web qualification](docs/web-qualification.md) · [Release process](docs/web-release.md) |

For bug reports, include the revision, Qt version, connection type and the smallest reproducible sequence. Remove credentials and private conversation content from logs and screenshots. See [engineering guidelines](AGENTS.md) before contributing architectural changes.

## Contributing

[Report an issue](https://github.com/SNodeC/CodexUI/issues) using the sanitized information in [Documentation](#documentation); read the existing [engineering guidelines](AGENTS.md).

<!-- snodec:begin ecosystem -->
## Ecosystem

[SNode.C organization](https://github.com/SNodeC) · Depends on [AISuite](https://github.com/SNodeC/AISuite#project-overview) and [SNode.C](https://github.com/SNodeC/snode.c#project-overview)
<!-- snodec:end ecosystem -->

<!-- snodec:begin footer -->
## License and ecosystem

Choose either [MIT](LICENSE-MIT) or [LGPL-3.0-or-later](LICENSE-LGPL-3.0-or-later).

Built with [Qt](https://www.qt.io/), [AISuite](https://github.com/SNodeC/AISuite#project-overview) and [SNode.C](https://github.com/SNodeC/snode.c#project-overview). Codex(W)UI is an independent project; neither frontend is an official OpenAI application.

Maintainer: [Volker Christian](https://github.com/VolkerChristian). [Organization](https://github.com/SNodeC) · [Contributing](https://github.com/SNodeC/.github/blob/main/CONTRIBUTING.md) · [Security](https://github.com/SNodeC/.github/blob/main/SECURITY.md)
<!-- snodec:end footer -->
