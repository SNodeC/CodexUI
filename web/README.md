<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p>
  <a href="../README.md#project-overview" title="Codex(W)UI repository"><img src="../docs/media/page-banner.svg" alt="Codex(W)UI documentation" width="100%"></a>
</p>
<!-- snodec:end page-header -->

# CodexWUI

<!-- snodec:begin back -->
<p>
  <a href="../README.md#project-overview" title="CodexUI"><img src="../docs/media/menu/back-codexui.svg" alt="CodexUI" width="96" height="24"></a>
</p>
<!-- snodec:end back -->

CodexWUI is the static browser frontend of Codex(W)UI. It connects directly to an AISuite `codex-bridge` WebSocket endpoint with the `codex` subprotocol. It contains no web server, bridge router, controller authority, or persistent Codex state. Its build and browser qualification require Node.js 22 or newer and Chrome or Chromium. Set `CHROME_BIN` to its executable if it is outside the usual system paths.

## Source layout

Follow the [browser build guide](../docs/build.md#browser-frontend) for the AISuite checkout layout, SDK build, application build and release qualification. Run its commands from the repository root.

`npm run profile` from this directory repeats the large-thread presentation measurement. There is no standalone Node development or production server.

For the shortest launch path, use [First run: browser edition](../README.md#browser-edition).

## Deployment

`npm run build:app` creates the relocatable static artifact in `app-dist/`. The standalone packaging project verifies and installs it with:

```sh
cmake -S . -B package-build -DCMAKE_INSTALL_PREFIX=/desired/prefix
cmake --build package-build --target codexui-web-artifact
cmake --install package-build --component CodexWebUI
```

The combined CodexUI build includes this project by default and installs the artifact in `share/codexui/web`; native-only builds must explicitly configure `-DCODEXUI_INSTALL_WEB=OFF`. The existing `codex-bridge` HTTP/WebSocket listener serves `/`, `/index.html`, and the generated `/assets/` files from that directory while `/codex` remains the WebSocket upgrade route. A page delivered over HTTPS automatically derives the corresponding same-origin `wss://.../codex` URL.

The configured bridge URL is retained in browser local storage. Controller/observer roles are not authentication; keep the listener on loopback or behind an external authenticated TLS boundary. Observers may receive broadcasts and read permitted workspace files. Provider-side workspace paths and generated-image paths are displayed as remote metadata; the application does not imply access to the browser machine's filesystem. Failed WebSocket openings release their transport through the SDK detach callback; an explicit retry waits for that release before constructing its replacement.

## Verification

- `npm test` builds TypeScript and runs all presentation, projection, lifecycle, viewport, supporting-surface, and server-render qualification tests.
- `npm run build:app` verifies the production Vite bundle.
- `npm run qualify:browser` serves that bundle only for the duration of a headless-Chromium responsive, focus, drawer, and target-size qualification.
- `npm run verify:artifact` proves that the output is non-empty and relocatable below an arbitrary static base path.
- The web qualification job checks the SDK from AISuite's default branch independently, runs the web suite, records the performance profile, installs the artifact through CMake, and uploads the verified staged tree as `codexui-web`.

The authoritative scope and exceptions are in [`../docs/web-1.0-contract.md`](../docs/web-1.0-contract.md#page-overview).
