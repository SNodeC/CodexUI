# CodexUI

CodexUI 1.0 is a native Qt 6 Widgets and browser frontend for the AISuite
`codex-bridge`. Both applications present the same Codex app-server behavior
without introducing another backend, semantic cache, snapshot store, or
persistence authority.

The native app-server/UI data path has two threads:

```text
Qt GUI thread
    <-> typed bounded SPSC queues + one eventfd per direction
SNode.C client thread
    <-> codex-bridge
    <-> Codex app-server
```

Both threads share one current `NodeGraph`. The SNode.C worker owns
CodexBridge, native app-server decode/encode, protocol-to-graph updates, and
all graph writes. Qt owns every widget and local interaction mechanic, reads
the graph only through non-blocking access, renders visible nodes in bounded
slices, and sends closed typed actions back to the worker. No app-server JSON,
serialized internal state, mirror model, or socketpair crosses this boundary.

## Applications

`codex-ui` is the canonical visual application. Its production shell binds the
existing widgets directly to shared nodes and sends typed node/runtime actions.
There is no parallel legacy UI or alternate application target.

`CodexWebUI` is the browser presentation. It uses the framework-neutral
`@snodec/codex-frontend` SDK from AISuite, connects directly to the bridge over
WebSocket, and implements the same visible controller, prompt, thread, turn,
and reconnect behavior in its own TypeScript state path. It does not share the
native in-process graph or its widget binding. Browser-only limitations are
listed in the [1.0 contract](docs/web-1.0-contract.md).

## Build

Qt 6 Widgets, Threads, libgit2 development files (discoverable as `libgit2`
through pkg-config), SNode.C `master`/HEAD, and an installed canonical AISuite
package exporting `AISuite::OpenAICodex` are required. On Debian and Ubuntu,
the libgit2 package is `libgit2-dev`. A combined install also requires the
`web/app-dist/` artifact produced by the Browser build below. For a deliberately
native-only build, add `-DCODEXUI_INSTALL_WEB=OFF` to the configure command.

```sh
cmake -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="/path/to/aisuite;/path/to/snodec"
cmake --build "${BUILD_DIR}" --parallel 8
ctest --test-dir "${BUILD_DIR}" --output-on-failure --parallel 8
cmake --install "${BUILD_DIR}"
```

Installation includes the `codex-ui` executable, desktop entry, and SVG icon.
The executable name, application ID, `StartupWMClass`, desktop entry, and icon
name intentionally match so Linux launchers and taskbars associate the window
with the installed CodexUI application.

## Browser build

Node.js 22 or newer and the exact AISuite revision recorded in
[`web/AISUITE_REVISION`](web/AISUITE_REVISION) are required. The source
dependency expects the release/CI checkout layout shown below.

```text
workspace/
├── AISuite-extraction/AISuite-final/
└── CodexUI/codexui/
```

```sh
npm ci --prefix ../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm test --prefix ../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm ci --prefix web
npm run release --prefix web
```

The production artifact is `web/app-dist/`. Combined CMake configuration
requires and installs it below `${CMAKE_INSTALL_DATADIR}/codexui/web`; the
standalone `web/CMakeLists.txt` provides the same verified packaging path.
`codex-bridge` serves those files and its `/codex` WebSocket endpoint from the
same listener. Node is not part of the installed runtime. Deployment details
are in [`web/README.md`](web/README.md).

## Architecture

The implemented native thread model, node/state authority rules, typed
mailboxes, protocol coverage, widget binding, and qualification boundaries are
documented in
[`docs/two-thread-shared-node-graph.md`](docs/two-thread-shared-node-graph.md).
[`docs/codex-architecture.md`](docs/codex-architecture.md) is a concise product
overview linking the native and browser-specific contracts.

Current message routing, pending-prompt acknowledgment, scrolling, composer
geometry, shell-output, Inspector, and desktop-integration decisions are
documented in
[`docs/ui-behavior.md`](docs/ui-behavior.md).

The browser architecture, native/web parity boundary, state ownership, and
version 1.0 delivery gates are documented in
[`docs/web-1.0-contract.md`](docs/web-1.0-contract.md).

Measured performance, equality evidence, packaging, and the release gate are
recorded in [`docs/web-qualification.md`](docs/web-qualification.md) and
[`docs/web-release.md`](docs/web-release.md).

## License

CodexUI is available under the LGPL-3.0-or-later OR MIT dual license.
