# Build and install Codex(W)UI from source

<!-- snodec:begin menu -->
<p>
  <a href="../README.md#quick-start" title="Start"><img src="media/menu/snodec-start-108.svg" alt="Start" width="108" height="24"></a>
  <a href="../README.md#install" title="Install"><img src="media/menu/snodec-install-108.svg" alt="Install" width="108" height="24"></a>
  <a href="build.md" title="Build"><img src="media/menu/snodec-build-108.svg" alt="Build" width="108" height="24"></a>
  <a href="../README.md#first-run" title="Use"><img src="media/menu/snodec-use-108.svg" alt="Use" width="108" height="24"></a>
  <a href="../README.md#configuration" title="Configure"><img src="media/menu/snodec-configure-108.svg" alt="Configure" width="108" height="24"></a>
  <a href="../README.md#architecture" title="Architecture"><img src="media/menu/snodec-architecture-108.svg" alt="Architecture" width="108" height="24"></a>
  <a href="../README.md#contributing" title="Contribute"><img src="media/menu/snodec-contribute-108.svg" alt="Contribute" width="108" height="24"></a>
</p>

<!-- snodec:end menu -->

## Native desktop

### Prerequisites

The native build requires a C++20 toolchain, CMake 3.20+, Ninja, Qt 6.6+ Widgets, pkg-config, libgit2 development files, and installed **SNode.C** and **AISuite** packages. On Debian/Ubuntu, the libgit2 development package is `libgit2-dev`.

Build [SNode.C](https://github.com/SNodeC/snode.c#project-overview) and [AISuite](https://github.com/SNodeC/AISuite#project-overview) from their `master` branches. AISuite must export `AISuite::OpenAICodex`. Running the application also requires a configured Codex app-server through `codex-bridge`.

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

## Browser frontend

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

The output is `web/app-dist/`. A combined install places it in `share/codexui/web`; the bridge can serve the application and `/codex` WebSocket from one listener. See [browser packaging and deployment](../web/README.md) for standalone installation and endpoint configuration.

For native execution, return to [First run](../README.md#first-run); native qualification commands and coverage boundaries remain in [Quality and development](../README.md#quality-and-development).
