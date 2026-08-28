# CodexWebUI

CodexWebUI is the static browser application for CodexUI 1.0. It connects
directly to an AISuite `codex-bridge` WebSocket endpoint with the `codex`
subprotocol. It contains no web server, bridge router, controller authority,
or persistent Codex state.

## Reproducible source layout

The application consumes the exact AISuite source revision in
[`AISUITE_REVISION`](AISUITE_REVISION). Check out the repositories as:

```text
workspace/
├── AISuite-extraction/AISuite-final/
└── CodexUI/codexui/
```

Verify the pin and build both sides:

```sh
test "$(git -C ../../../AISuite-extraction/AISuite-final rev-parse HEAD)" = "$(cat AISUITE_REVISION)"
npm ci --prefix ../../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm test --prefix ../../../AISuite-extraction/AISuite-final/packages/codex-frontend
npm ci
npm run release
```

`npm run dev` starts the development server on port 5173. `npm run profile`
repeats the large-thread presentation measurement.

## Deployment

`npm run build:app` creates the relocatable static artifact in `app-dist/`.
Serve that directory from an HTTP(S) host. A page delivered over HTTPS should
connect to a `wss://` bridge endpoint so the browser does not reject mixed
content. The bridge must accept the page's origin according to its deployment
policy; CodexWebUI does not weaken that policy or proxy traffic.

The configured bridge URL is retained in browser local storage. Provider-side
workspace paths and generated-image paths are displayed as remote metadata;
the application does not imply access to the browser machine's filesystem.

## Verification

- `npm test` builds TypeScript and runs all presentation, projection,
  lifecycle, viewport, supporting-surface, and server-render qualification
  tests.
- `npm run build:app` verifies the production Vite bundle.
- `npm run verify:artifact` proves that the output is non-empty and relocatable
  below an arbitrary static base path.
- The repository CI checks the pinned SDK independently, runs the web suite,
  records the performance profile, and uploads `app-dist` as `codexui-web`.

The authoritative scope and exceptions are in
[`../docs/web-1.0-contract.md`](../docs/web-1.0-contract.md).
