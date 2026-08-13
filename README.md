# CodexUI

CodexUI is a native Qt 6 Widgets graphical Codex client built on AISuite's
public Codex frontend C++ SDK. The UI shell implements the Figma v2 workbench
([node 4:2](https://www.figma.com/design/IScmS9lHPduDueN2sVEsiO/Codex-UI-Prototype-v0.1?node-id=4-2)).

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

CodexUI connects automatically to the local `codex-backend` Unix frontend
socket through Qt and delegates authentication, protocol handling, and state
synchronization to AISuite's public frontend SDK. The sidebar displays real
synchronized threads, and the center work area renders the selected thread's
real turns, messages, semantic items, activity, token usage, and failures from
the current immutable frontend State.

CodexUI remains read-only. Composer actions, thread actions, approvals, and
interrupt/stop are not connected. The right inspector still contains
presentation/demo content.

## Build

Qt 6 Widgets, Qt 6 Network, and an installed AISuite package that exports its
public Codex frontend client are required.

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel 28
```

## License

CodexUI is available under the LGPL-3.0-or-later OR MIT dual license.
