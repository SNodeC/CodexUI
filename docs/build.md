# Build and install Codex(W)UI from source

<!-- snodec:begin back -->
<p>
  <a href="../README.md#project-overview" title="CodexUI"><img src="media/menu/back-codexui.svg" alt="CodexUI" width="96" height="24"></a>
</p>
<!-- snodec:end back -->

## Native desktop

### Prerequisites

The native build requires a C++20 toolchain, CMake 3.20+, Ninja, Qt 6.6+ Widgets (the installed Qt version must satisfy that minimum), pkg-config, libgit2 development files, and compatible **SNode.C** and **AISuite** CMake installations. On Debian/Ubuntu, the libgit2 development package is `libgit2-dev`.

Build [SNode.C](https://github.com/SNodeC/snode.c#project-overview) and [AISuite](https://github.com/SNodeC/AISuite#project-overview) from their default branches. AISuite must export `AISuite::OpenAICodex`. Running the application also requires a configured Codex app-server through `codex-bridge`.

### Configure and build

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

For the browser build and its required checkout layout, see [Browser frontend](#browser-frontend).

### Install the build

From the same checkout and build directory used for the native build, optionally install into `/usr/local` to add the executable, desktop entry and application icon. For installation without administrator privileges, configure a writable `CMAKE_INSTALL_PREFIX` and omit `sudo`:

```sh
sudo cmake --install build
```

### Verify the native installation

Run `codex-ui --help` from the installed prefix to check executable and shared-library lookup, then follow [First run](../README.md#first-run) for an interactive connection. In a source checkout, use `./build/codex-ui --help`. A help response is not proof of full desktop qualification.

## Quality and development

Install Xvfb and `xauth`, then run native checks in the documented isolated environment:

```sh
xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build --output-on-failure --parallel 14
```

The repository includes shared native/browser presentation fixtures, native interaction and geometry checks, and workload benchmarks. Selected native suites exercise DPR **1.0, 1.25, 1.5 and 2.0**. CI's accepted elapsed-time policy reports timing overruns; correctness and work-count checks remain enforced.

The [interactive qualification inventory](native-ui-ux-qualification-inventory.md) defines the wider visual and UX acceptance scope. Its [execution record](native-ui-ux-qualification-results.md) remains incomplete: a green CI run is not a claim of complete desktop, accessibility or hardware touchpad qualification.

## Browser frontend

The browser release build needs **Node.js 22+**, **Chrome or Chromium** for browser qualification, and AISuite's frontend SDK source. Set `CHROME_BIN` if the browser is not installed at one of the usual system paths. Its local package dependency currently expects this checkout layout:

```text
workspace/
├── AISuite-extraction/AISuite-final/   # SNodeC/AISuite, default branch
└── CodexUI/codexui/                  # this repository
```

The unusual directory names are required by the current `web/package.json` local SDK dependency; they are not a recommendation for a second checkout system. From an empty directory, create that layout explicitly (Git uses each repository's default branch without hard-coding its name):

```sh
mkdir workspace
cd workspace
git clone https://github.com/SNodeC/AISuite.git AISuite-extraction/AISuite-final
git clone https://github.com/SNodeC/CodexUI.git CodexUI/codexui
cd CodexUI/codexui
```

From `CodexUI/codexui/`:

```sh
npm ci --prefix ../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm test --prefix ../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm ci --prefix web
npm run release --prefix web
```

The output is `web/app-dist/`. A combined install places it in `share/codexui/web`; the bridge can serve the application and `/codex` WebSocket from one listener. See [browser packaging and deployment](../web/README.md) for standalone installation and endpoint configuration.

For native execution, return to [First run](../README.md#first-run); qualification commands and coverage boundaries are in [Quality and development](#quality-and-development).
