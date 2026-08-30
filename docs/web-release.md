# CodexWebUI 1.0 release

## Release manifest

| Component | Version or revision | Release output |
| --- | --- | --- |
| CodexUI native application | 1.0.0 | installed `codex-ui`, desktop entry, and icon |
| CodexWebUI | 1.0.0 | relocatable `web/app-dist/` static artifact |
| AISuite frontend SDK | 1.0.0 at `5aeedb2c21d7da0d611219365294cc3fb052cddf` | publishable `@snodec/codex-frontend` package |

The SDK revision is machine-readable in `web/AISUITE_REVISION` and is checked
by CI before either SDK or application tests run. The source layout and build
commands are documented in `web/README.md`.

## Equality and release gate

A release candidate is eligible only when one CI revision proves all of the
following:

- the pinned SDK installs from its lockfile and passes protocol generation,
  routing/lifecycle, and WebSocket transport equality tests;
- the browser application installs from its lockfile and passes the complete
  75-test suite across ten web test files;
- the production bundle passes the real-Chromium responsive, focus, drawer,
  overflow, and coarse-pointer qualification workflow;
- the production Vite artifact builds, passes its relocatability check, and is
  installed through the standalone CMake packaging project;
- CI asserts the staged `share/codexui/web/index.html` and generated assets
  before uploading that installed tree, without source or development files;
- the complete seven-test native CTest suite passes against the same AISuite
  revision;
- `git diff --check` passes and the recorded performance profile shows no
  material regression from `web-qualification.md`.

The CI artifact is named `codexui-web`. Combined CMake builds require the web
artifact by default; a native-only build must explicitly set
`CODEXUI_INSTALL_WEB=OFF`. Installation copies the static tree into the
CodexUI data directory, and the existing `codex-bridge` listener serves it
alongside `/codex`. No Node process, web-specific backend, or protocol
extension is part of the release.

## Operator checks

Before promoting the artifact, connect once over `wss://` in a supported
desktop browser and confirm discovery, one prompt/response, reconnect
hydration, a pending request, thread actions, settings, and each Inspector
tab. This final smoke check validates deployment origin/TLS policy rather than
creating a second behavioral test authority.

Native-only capabilities and honest browser representations remain exactly as
listed in the 1.0 contract. A failed or unavailable gate is reported as such;
it is not converted into a release pass by documentation.
