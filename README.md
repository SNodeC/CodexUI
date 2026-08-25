# CodexUI

CodexUI is a native Qt 6 Widgets frontend for the AISuite `codex-bridge`.
It presents the Codex app-server protocol without introducing another backend,
semantic cache, snapshot store, or persistence authority.

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

- `codex-ui`: the normal visual application.
- `codex-ui-harness`: the permanent protocol and reducer development harness.

Both applications use the same transport, socketpair, normalization,
presentation protocol, and model implementation. Only their top-level Qt
consumer differs.

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

## Architecture

The complete thread model, presentation protocol, authority rules, normalized
event vocabulary, public APIs, shell behavior, implementation report, and test
boundaries are documented in
[`docs/codex-architecture.md`](docs/codex-architecture.md).

Current message routing, pending-prompt acknowledgment, scrolling, composer
geometry, shell-output, Inspector, and desktop-integration decisions are
documented in
[`docs/ui-behavior.md`](docs/ui-behavior.md).

## License

CodexUI is available under the LGPL-3.0-or-later OR MIT dual license.
