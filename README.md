# CodexUI

CodexUI 1.0 is a native Qt 6 Widgets and browser frontend for the AISuite
`codex-bridge`. Both applications present the same Codex app-server behavior
without introducing another backend, semantic cache, snapshot store, or
persistence authority.

The canonical process has two threads:

```text
Qt GUI thread
    <-> bounded nonblocking Unix socketpair
SNode.C client thread
    <-> codex-bridge
    <-> Codex app-server
```

The Qt thread owns widgets and `PresentationModel`. The SNode.C thread owns the
event loop, selected transport, `AISuite::OpenAICodex` frontend proxy SDK,
native protocol normalization, and connection/controller telemetry. They
exchange only bounded `codexui.presentation` JSONL commands and events.

## Applications

`codex-ui` is the canonical visual application. Its production shell consumes
the normalized presentation protocol and model directly; there is no parallel
legacy UI or alternate application target.

`CodexWebUI` is the browser presentation. It uses the framework-neutral
`@snodec/codex-frontend` SDK from AISuite, connects directly to the bridge over
WebSocket, and follows the same controller, prompt, thread, turn, projection,
and reconnect rules as the native application. Browser-only limitations are
listed in the [1.0 contract](docs/web-1.0-contract.md).

## Build

Qt 6 Widgets, Threads, libgit2 development files (discoverable as `libgit2`
through pkg-config), SNode.C `master`/HEAD, and an installed canonical AISuite
package exporting `AISuite::OpenAICodex` are required. On Debian and Ubuntu,
the libgit2 package is `libgit2-dev`.

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

Node.js 20 or newer and the exact AISuite revision recorded in
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

The production artifact is `web/app-dist/`. CMake installs it below
`${CMAKE_INSTALL_DATADIR}/codexui/web`; `codex-bridge` serves those files and
its `/codex` WebSocket endpoint from the same listener. Node is not part of the
installed runtime. Deployment details are in [`web/README.md`](web/README.md).

## Architecture

The complete thread model, presentation protocol, authority rules, normalized
event vocabulary, public APIs, shell behavior, implementation report, and test
boundaries are documented in
[`docs/codex-architecture.md`](docs/codex-architecture.md).

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
