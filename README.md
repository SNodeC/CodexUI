# CodexUI

CodexUI is a native Qt 6 Widgets graphical Codex client built on AISuite's
public Codex frontend C++ SDK. The current UI shell implements the Figma v2
workbench ([node 4:2](https://www.figma.com/design/IScmS9lHPduDueN2sVEsiO/Codex-UI-Prototype-v0.1?node-id=4-2))
using deterministic presentation data.

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
Codex backend/frontend SDK project. The Q1 shell does not connect to
`codex-backend`; runtime binding through AISuite is the next phase.

## Build

Qt 6 Widgets and an installed AISuite package that exports its public Codex
frontend client are required.

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel 28
```

## License

CodexUI is available under the LGPL-3.0-or-later OR MIT dual license.
