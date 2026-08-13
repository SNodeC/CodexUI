# CodexUI

CodexUI is a native graphical Codex client built on AISuite's public Codex
frontend C++ SDK. It connects to an independently running `codex-backend`,
which owns persistent and shared Codex state and communicates with the Codex
app-server.

```text
CodexUI
    |
    | AISuite public frontend C++ SDK
    v
codex-backend
    |
    v
codex app-server
```

This repository contains only the UI/client. AISuite remains the reusable
Codex backend/frontend SDK project. CodexUI does not communicate with the
Codex app-server directly.

## Build

An installed AISuite package that exports its public Codex frontend client is
required.

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

## License

CodexUI is available under the LGPL-3.0-or-later OR MIT dual license.
