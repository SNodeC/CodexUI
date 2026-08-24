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

The previous stateful implementation is preserved on the dedicated
`legacy-codex` Git branch. It is not part of the canonical source tree or build.

## Applications

- `codex-ui`: the normal visual application.
- `codex-ui-harness`: the permanent protocol and reducer development harness.

Both applications use the same transport, socketpair, normalization,
presentation protocol, and model implementation. Only their top-level Qt
consumer differs.

## Build

Qt 6 Widgets, Threads, SNode.C `master`/HEAD, and an installed canonical
AISuite package exporting `AISuite::OpenAICodex` are required.

```sh
cmake -S . -B build-codex -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="/path/to/aisuite;/path/to/snodec"
cmake --build build-codex --parallel 8
ctest --test-dir build-codex --output-on-failure --parallel 8
```

## Architecture

The complete thread model, presentation protocol, authority rules, normalized
event vocabulary, public APIs, shell behavior, implementation report, and test
boundaries are documented in
[`docs/codex-architecture.md`](docs/codex-architecture.md).

## License

CodexUI is available under the LGPL-3.0-or-later OR MIT dual license.
