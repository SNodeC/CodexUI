<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-guides-compact.svg"><img src="../media/nav/banner-guides.svg" alt="CodexUI — Guides" width="100%"></picture></p>
<p><a href="README.md#banner" title="Up to Guides"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-guides-dark.svg"><img src="../media/nav/up-guides-light.svg" alt="Up to Guides" title="Up to Guides" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/account-dark.svg"><img align="right" src="../media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Build and install Codex(W)UI from source

## Native desktop

### Prerequisites

Native source builds are supported on Linux. The native build requires KF6 Sonnet 6.13+ and a C++20 toolchain, CMake 3.20+, Ninja, Qt 6.6+ Widgets (the installed Qt version must satisfy that minimum), pkg-config, libgit2 development files, and compatible **SNode.C** and **AISuite** CMake installations. Their source checkouts and build directories are independent of CodexUI; CMake discovers their installed packages through `CMAKE_PREFIX_PATH`. On Debian/Ubuntu, development packages include `libgit2-dev`, `qt6-base-dev` and `libkf6sonnet-dev` (plus your toolchain/build tools). Check the installed Qt version with `pkg-config --modversion Qt6Widgets`: the package name alone does not guarantee Qt 6.6+. Use a distribution/toolchain providing that minimum rather than bypassing CMake's check.

Prompt spelling requires a Sonnet runtime backend and dictionaries, separately from the development headers. On Debian, install `sonnet6-plugins` plus language packages such as `hunspell-en-us` and `hunspell-de-at`. The checker uses Sonnet's resolved default language; its context menu shows the effective language or unavailability and offers the installed dictionaries. Custom Sonnet installations can be supplied through `CMAKE_PREFIX_PATH` alongside the other dependency prefixes.

Spelling starts enabled. Use the prompt's standard context menu to choose a correction, ignore a word for the app session, toggle checking or switch dictionaries. Language and enable choices are local to that editor session; ignoring a word is shared for the running app session. These controls do not change global Sonnet settings or personal dictionaries. Spell checking only annotates the draft; it never corrects or changes submitted text automatically. Completed words are checked as you type; existing findings are retained and drawn with vivid red underlines by Qt. Browser spelling uses the browser's own dictionaries and menu.

Build [SNode.C](https://github.com/SNodeC/snode.c#banner) and [AISuite](https://github.com/SNodeC/AISuite#banner) from their default branches. AISuite must export `AISuite::OpenAICodex`. Running the application also requires a configured Codex app-server through `codex-bridge`.

The native build also produces `codex-manager`. Building the manager tests requires a Python 3 interpreter. At runtime, managed profiles need a systemd user manager and `flock`, plus SSH for remote supervision; full data deletion requires Linux and Python 3.11+ on each target host. See the [profile-manager guide](profile-manager.md) for required bridge/client executables and deployment choices.

### Configure and build

Clone the native checkout (Git selects the default branch), then replace the two installation prefixes below:

```sh
git clone https://github.com/SNodeC/CodexUI.git
cd CodexUI
```

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCODEXUI_INSTALL_WEB=OFF \
  -DCMAKE_PREFIX_PATH="/path/to/aisuite;/path/to/snodec"
cmake --build build --parallel 14
```

This is an explicit **native-only** build. For a combined install, build the web artifact first as described below, then configure with `CODEXUI_INSTALL_WEB=ON` and `CODEXUI_WEB_DIST_DIR=/path/to/build/codex-web/web/app-dist`.

For installed SDK prerequisites and browser build instructions, see [Browser frontend](#browser-frontend).

### Install the build

From the same checkout and build directory used for the native build, optionally install into `/usr/local` to add both `codex-ui` and `codex-manager`, their desktop entries and application icons. For installation without administrator privileges, configure a writable `CMAKE_INSTALL_PREFIX` and omit `sudo`:

```sh
sudo cmake --install build
```

### Verify the native installation

Run `codex-ui --help` from the installed prefix to check executable and shared-library lookup, then follow [First run](../../README.md#first-run) for an interactive connection. In a source checkout, use `./build/codex-ui --help`. Check `codex-manager --help` too (or `./build/codex-manager --help` before installation), then follow the [manager guide](profile-manager.md) to configure and authenticate a profile. A help response is not proof of full desktop qualification.

## Quality and development

Install Xvfb and `xauth`, then run native checks in the documented isolated environment:

```sh
xvfb-run -a env QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build --output-on-failure --parallel 14
```

The repository includes shared native/browser presentation fixtures, native interaction and geometry checks, and workload benchmarks. Selected native suites exercise DPR **1.0, 1.25, 1.5 and 2.0**. CI's accepted elapsed-time policy reports timing overruns; correctness and work-count checks remain enforced.

The [interactive qualification inventory](../qualification/native-test-suite.md#banner) defines the wider visual and UX acceptance scope. Its [execution record](../qualification/native-results-2026-09-27.md#banner) remains incomplete: a green CI run is not a claim of complete desktop, accessibility or hardware touchpad qualification.

## Browser frontend

The browser release build needs **Node.js 22+**, **Chrome or Chromium** for browser qualification, and the installed AISuite frontend SDK package. Set `CHROME_BIN` if the browser is not installed at one of the usual system paths. Install the SDK with AISuite's normal build or its [standalone SDK installation](https://github.com/SNodeC/AISuite/blob/master/packages/codex-frontend/README.md#consume-the-sdk-from-source). The two source repositories may be checked out anywhere; CodexUI does not read the AISuite checkout.

From the CodexUI repository root, select your SDK installation prefix and any writable build directory:

```sh
cmake -S web/cmake -B /path/to/build/codex-web -DCMAKE_PREFIX_PATH=/usr/local
cmake --build /path/to/build/codex-web --target codexui-web-release
```

CMake resolves the installed SDK archive, prepares an isolated npm workspace, and runs the existing complete release qualification. Generated package metadata, node_modules and browser artifacts stay in the build directory. Use `CMAKE_PREFIX_PATH` for a custom SDK installation prefix. No sibling checkout or build-directory arrangement is required.

The output is `/path/to/build/codex-web/web/app-dist/`. Install it using the existing static-artifact packager:

```sh
cmake -S web -B /path/to/build/codex-web-package \
  -DCODEXUI_WEB_DIST_DIR=/path/to/build/codex-web/web/app-dist \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build /path/to/build/codex-web-package
sudo cmake --install /path/to/build/codex-web-package --component CodexWebUI
```

For a combined native/browser install, pass the same `CODEXUI_WEB_DIST_DIR` when configuring the native project. The bridge can serve the installed artifact and `/codex` WebSocket from one listener. See [browser packaging and deployment](../../web/README.md#banner) for endpoint configuration.

For native execution, return to [First run](../../README.md#first-run); qualification commands and coverage boundaries are in [Quality and development](#quality-and-development).

### Dependency version scope

The documented development build uses dependency default-branch source and records the resolved revisions. `find_package(snodec 2.0)` is a CMake version floor, not a claim that every released 2.0 binary exposes all APIs required by the current AISuite/CodexUI source. Use the source-build route for this guide; compatibility with a particular signed release requires qualification of that exact dependency/application pair.
