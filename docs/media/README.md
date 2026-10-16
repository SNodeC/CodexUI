# Landing-page assets

- `readme-hero.svg`: original, editable vector artwork for this README. The small three-pane motif is a schematic illustration, not a screenshot. The application mark follows `resources/icons/codex-ui.svg`.
- `native-workspace.png`: unmodified Qt widget capture from the isolated v88 native qualification session, `v88-controller-retained-completed.png`. The disposable thread shows a completed command and response; it is not a performance claim or a capture of private user conversation history. Captured at 1536 × 960, DPR 1, through Xvfb with the Qt offscreen platform. The associated qualification is recorded in `../qualification/native-results-2026-09-27.md`.

The image predates the final v110 scrolling correction and the CI-only QString compatibility fix; neither adds visible UI. Do not substitute the old dark UI review captures for the current native interface. For a new screenshot, use an isolated application and disposable data, inspect it for private information, and retain an honest capture description here.

SVGs use embedded geometry, system-font fallbacks and fixed backgrounds. They have no remote resources, scripts or font downloads and work on light and dark GitHub pages. Assets are distributed under the repository's dual license.

## CodexManager captures

`codex-manager-overview.png`, `codex-manager-details.png` and `codex-manager-editor.png` are unmodified Qt widget captures from the isolated 2026-10-16 manager qualification at DPR 1.0, under Xvfb with the offscreen plugin. They use disposable profiles and simulated service-status helpers, contain no private conversation history, and are distinct from the real-service lifecycle qualification. The overview/details window is 1240 × 830; the editor is 780 × 700. See [the qualification record](../qualification/codexmanager-2026-10-16.md) for provenance and limits.
