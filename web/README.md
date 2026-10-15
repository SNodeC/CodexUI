<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../docs/media/nav/banner-web-ui-compact.svg"><img src="../docs/media/nav/banner-web-ui.svg" alt="CodexUI — Web UI" width="100%"></picture></p>
<p><a href="../README.md#banner" title="Up to CodexUI"><picture><source media="(prefers-color-scheme: dark)" srcset="../docs/media/nav/up-codexui-dark.svg"><img src="../docs/media/nav/up-codexui-light.svg" alt="Up to CodexUI" title="Up to CodexUI" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../docs/media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../docs/media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../docs/media/nav/account-dark.svg"><img align="right" src="../docs/media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../docs/media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../docs/media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../docs/media/nav/organization-dark.svg"><img align="right" src="../docs/media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../docs/media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../docs/media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../docs/media/nav/repository-dark.svg"><img align="right" src="../docs/media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# CodexWUI

CodexWUI is the static browser frontend of Codex(W)UI. It connects directly to an AISuite `codex-bridge` WebSocket endpoint with the `codex` subprotocol. It contains no web server, bridge router, controller authority, or persistent Codex state. Its build and browser qualification require Node.js 22 or newer and Chrome or Chromium. Set `CHROME_BIN` to its executable if it is outside the usual system paths.

## Source layout

Follow the [browser build guide](../docs/guides/build-from-source.md#browser-frontend) for the AISuite checkout layout, SDK build, application build and release qualification. Run its commands from the repository root.

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

## Remote access over WSS

The bridge, not this static frontend, owns the encrypted listener. A build with AISuite frontend TLS/WebSocket components exposes `codex-bridge-wss-ipv4`. For a private VPN or another network boundary that admits only authorized users, provide your own certificate chain/key and bind only that network's interface. The following is a deployment template, not a configured public endpoint:

```sh
VPN_IP=YOUR_PRIVATE_INTERFACE_ADDRESS
codex-bridge codex --bridge-web-root /path/to/installed/share/codexui/web \
  codex-bridge-wss-ipv4 --disabled=false \
    local --host "$VPN_IP" --port 8443 \
    tls --cert /path/to/server-chain.pem --cert-key /path/to/server-key.pem
```

Open `https://YOUR_CERTIFICATE_DNS_NAME:8443/` from an authorized peer. Its default same-origin connection is `wss://YOUR_CERTIFICATE_DNS_NAME:8443/codex`; the bridge and browser negotiate the `codex` subprotocol. The DNS name must resolve to the bound interface and match a certificate trusted by that browser. Restrict private-key permissions and check `codex-bridge codex-bridge-wss-ipv4 --help=expanded` against the installed build before launch. Other non-Unix frontend instances remain disabled unless explicitly enabled.

Verify page/assets delivery, the WSS handshake, controller/observer behavior and a real prompt. Test that an unauthorized network peer cannot reach the listener. TLS supplies encryption/server identity, **not application authentication**: do not expose this recipe to the public Internet merely because WSS works. A public deployment needs an independently authenticated boundary that protects both the static application and WebSocket upgrade and preserves `/codex` and the subprotocol. That boundary's credentials and authorization policy are operator-owned, not built into CodexUI.


## Verification

- `npm test` builds TypeScript and runs all presentation, projection, lifecycle, viewport, supporting-surface, and server-render qualification tests.
- `npm run build:app` verifies the production Vite bundle.
- `npm run qualify:browser` serves that bundle only for the duration of a headless-Chromium responsive, focus, drawer, and target-size qualification.
- `npm run verify:artifact` proves that the output is non-empty and relocatable below an arbitrary static base path.
- The web qualification job checks the SDK from AISuite's default branch independently, runs the web suite, records the performance profile, installs the artifact through CMake, and uploads the verified staged tree as `codexui-web`.

The authoritative scope and exceptions are in [`../docs/web/contract-1.0.md`](../docs/web/contract-1.0.md#banner).
