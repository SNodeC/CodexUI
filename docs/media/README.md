<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p>
  <a href="../../README.md#project-overview" title="Codex(W)UI repository"><img src="page-banner.svg" alt="Codex(W)UI documentation" width="100%"></a>
</p>
<!-- snodec:end page-header -->

# Landing-page assets

- `readme-hero.svg`: original, editable vector artwork for this README. The small three-pane motif is a schematic illustration, not a screenshot. The application mark follows `resources/icons/codex-ui.svg`.
- `native-workspace.png`: unmodified Qt widget capture from the isolated v88 native qualification session, `v88-controller-retained-completed.png`. The disposable thread shows a completed command and response; it is not a performance claim or a capture of private user conversation history. Captured at 1536 × 960, DPR 1, through Xvfb with the Qt offscreen platform. The associated qualification is recorded in `../native-ui-ux-qualification-results.md`.

The image predates the final v110 scrolling correction and the CI-only QString compatibility fix; neither adds visible UI. Do not substitute the old dark UI review captures for the current native interface. For a new screenshot, use an isolated application and disposable data, inspect it for private information, and retain an honest capture description here.

SVGs use embedded geometry, system-font fallbacks and fixed backgrounds. They have no remote resources, scripts or font downloads and work on light and dark GitHub pages. Assets are distributed under the repository's dual license.
