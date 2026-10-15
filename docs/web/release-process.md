<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-web-ui-compact.svg"><img src="../media/nav/banner-web-ui.svg" alt="CodexUI — Web UI" width="100%"></picture></p>
<p><a href="README.md#banner" title="Up to Web UI"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-web-ui-dark.svg"><img src="../media/nav/up-web-ui-light.svg" alt="Up to Web UI" title="Up to Web UI" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/account-dark.svg"><img align="right" src="../media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# CodexWebUI release-candidate process

This document specifies candidate targets and qualification gates, not an announcement of published releases. The 1.0.0 values below are source package/manifest targets; check the repository Releases page for publication.

## Candidate manifest

| Component | Target version or revision | Release output |
| --- | --- | --- |
| CodexUI native application | 1.0.0 | installed `codex-ui`, desktop entry, and icon |
| CodexWebUI | 1.0.0 | relocatable `<browser-build>/web/app-dist/` static artifact |
| AISuite frontend SDK | 1.0.0 from AISuite default-branch source | publishable `@snodec/codex-frontend` package |

CI checks out AISuite default-branch source for both native and web jobs, and SNode.C default-branch source for the native build. Checkout logs record the resolved commits; dependency commits are not pinned. The independent SDK and application builds are documented in [the browser build guide](../guides/build-from-source.md#browser-frontend).

## Qualification and release gate

A release candidate is eligible only when one CI revision proves all of the following:

- the SDK from AISuite default-branch source installs from its lockfile and passes protocol generation, routing/lifecycle, and WebSocket transport tests;
- the browser application installs from its lockfile and passes the complete registered web test suite, including the shared frontend-presentation corpus;
- the production bundle passes the real-Chromium responsive, focus, drawer, overflow, and coarse-pointer qualification workflow;
- the production Vite artifact builds, passes its relocatability check, and is installed through the standalone CMake packaging project;
- CI asserts the staged `share/codexui/web/index.html` and generated assets before uploading that installed tree, without source or development files;
- the complete registered native CTest suite passes against AISuite default-branch source;
- `git diff --check` passes and the recorded performance profile shows no material regression from `web-qualification.md`.

The CI artifact is named `codexui-web`. Combined CMake builds require the web artifact by default; a native-only build must explicitly set `CODEXUI_INSTALL_WEB=OFF`. Installation copies the static tree into the CodexUI data directory. The existing `codex-bridge` listener serves it alongside `/codex`; set `codex --bridge-web-root PATH` to the installed artifact when it differs from AISuite's configured default static root. No Node process, web-specific backend, or protocol extension is part of the release.

## Operator checks

Before promoting the artifact, connect once over `wss://` in a supported desktop browser and confirm discovery, one prompt/response, reconnect hydration, a pending request, thread actions, settings, and each Inspector tab. This final smoke check validates deployment origin/TLS policy rather than creating a second behavioral test authority.

Native-only capabilities and honest browser representations remain exactly as listed in the 1.0 contract. A failed or unavailable gate is reported as such; it is not converted into a release pass by documentation.
