<a name="project-overview"></a>

<p align="center">
  <img src="docs/media/readme-hero.svg" alt="Codex(W)UI — A workspace for the whole coding conversation." width="100%">
</p>

<p align="center">
  <a href="https://github.com/SNodeC/CodexUI/actions/workflows/ci.yml"><img src="https://github.com/SNodeC/CodexUI/actions/workflows/ci.yml/badge.svg?branch=master" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT%20OR%20LGPL--3.0--or--later-526da3" alt="License: MIT or LGPL 3.0 or later"></a>
</p>

<p align="center">
  <a href="#the-workspace">Explore the workspace</a> ·
  <a href="#get-started">Get started</a> ·
  <a href="#browser-edition">Browser edition</a> ·
  <a href="#documentation">Documentation</a>
</p>

# Codex(W)UI

**Follow the work, not just the final answer.** Codex(W)UI offers two frontends: **CodexUI**, a native Qt desktop application, and **CodexWUI**, a browser application connecting over WebSocket. They bring conversations, commands, plans, approvals and file changes into a visual coding workspace.

Built on [AISuite](https://github.com/SNodeC/AISuite#project-overview) and [SNode.C](https://github.com/SNodeC/snode.c#project-overview), both frontends connect through AISuite's `codex-bridge`. Codex app-server remains responsible for agent execution and persistent history; both applications give you the controls and visibility around it.

[![Native CodexUI showing a conversation, completed command output, turn settings and the Inspector.](docs/media/native-workspace.png)](docs/media/native-workspace.png)

*Actual native application, captured during an isolated qualification session with a disposable demonstration thread. Click the image to inspect it at full resolution. [Image provenance](docs/media/README.md).*

## The workspace

| Organize the work | Follow the execution | Inspect the result |
| --- | --- | --- |
| Browse threads, search titles, archive work and return to retained history. | Read streamed responses, reasoning summaries and command output in one conversation. | Keep plans, agent activity, requests, changes and timing details beside the conversation. |
| In the native app, group threads by projects and shared sections. | Send prompts, steer an active turn, stop work and respond to approval requests. | Review repository diffs, copy output and inspect the protocol when diagnosing a problem. |

### A conversation you can work with

- **Rich, distinct activity cards.** Markdown, commands, file changes, plans, images and agent activity keep their own controls and presentation.
- **History without loading every widget.** Paginated history and virtualized native rendering keep offscreen content as data rather than another renderer.
- **Input beyond plain text.** The native composer accepts image paste and local file drag/drop, alongside the attachment picker and multiline editing.
- **Context when you need it.** Supplied timestamps, durations, token information and detailed Inspector views make execution easier to understand.

### Projects and sections, without duplicating threads

In the native threads panel, **projects contain the visual grouping; sections organize the threads within it**. A section can appear in multiple projects because project and section memberships are independent. Standalone sections and ungrouped threads remain visible too.

To create a project or section, click the current grouping label—**Projects**, **Sections** or **Ungrouped**—above the thread list. Use a thread's context menu to assign its project and section. Creation and assignment require controller access and server support.

[Read the grouping and lifecycle guide →](docs/thread-projects-sections.md)

## Choose your frontend

| | Native · CodexUI | Browser · CodexWUI |
| --- | --- | --- |
| Presentation | Qt 6 Widgets; Linux desktop integration | TypeScript / React; static browser application |
| Connection | SNode.C transports exposed by the build | WebSocket through AISuite's frontend SDK |
| Best fit | Local workspace, project/section organization, desktop file interaction | Browser access to the bridge without installing the Qt application |
| Deployment | `codex-ui` executable, desktop entry and icon | Built assets served by `codex-bridge`; no Node runtime required |

The frontends share protocol and lifecycle meaning, **not an identical feature inventory**. Native project/section management is not yet a browser feature. See the [browser scope and limitations](docs/web-1.0-contract.md) before choosing a deployment.

## How it fits together

**CodexUI / CodexWUI ↔ AISuite bridge ↔ Codex app-server**

The bridge allows multiple frontends to follow the same app-server session, with one controller at a time and policy-limited observers. Controller status is distinct from the agent's approval and sandbox settings.

On the native side, a SNode.C worker owns protocol processing and writes one shared `NodeGraph`; Qt projects that state into widgets. The browser uses AISuite's TypeScript SDK and its own presentation state. Neither frontend replaces app-server's persistent history or tool execution.

## Get started

### 1. Prepare the dependencies

The native build requires a C++20 toolchain, CMake 3.20+, Qt 6.6+ Widgets, pkg-config, libgit2 development files, and installed **SNode.C** and **AISuite** packages. On Debian/Ubuntu, the libgit2 development package is `libgit2-dev`.

Build [SNode.C](https://github.com/SNodeC/snode.c#project-overview) and [AISuite](https://github.com/SNodeC/AISuite#build-from-source) from their `master` branches. AISuite must export `AISuite::OpenAICodex`. Running the application also requires a configured Codex app-server through `codex-bridge`.

### 2. Build the native application

From this repository's root, replace the two installation prefixes below:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCODEXUI_INSTALL_WEB=OFF \
  -DCMAKE_PREFIX_PATH="/path/to/aisuite;/path/to/snodec"
cmake --build build --parallel 14
```

This is an explicit **native-only** build. For a combined install, build the web artifact first as described below, then configure with `CODEXUI_INSTALL_WEB=ON`.

### 3. Connect and work

Start your configured `codex-bridge`, then run:

```sh
./build/codex-ui
```

Use **Connection** to select its endpoint. Acquire control if necessary, choose or create a thread, and send a prompt. App-server configuration determines available models, permissions and approval behavior.

Optional installation adds the executable, desktop entry and application icon:

```sh
cmake --install build
```

## Browser edition

The browser build needs **Node.js 22+** and AISuite's frontend SDK source. Its local package dependency currently expects this checkout layout:

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

Run native checks in the documented isolated environment:

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

## License and ecosystem

Choose either [MIT](LICENSE-MIT) or [LGPL-3.0-or-later](LICENSE-LGPL-3.0-or-later).

Built with [Qt](https://www.qt.io/), [AISuite](https://github.com/SNodeC/AISuite#project-overview) and [SNode.C](https://github.com/SNodeC/snode.c#project-overview). Codex(W)UI is an independent project; neither frontend is an official OpenAI application.
