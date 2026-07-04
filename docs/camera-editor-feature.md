# Camera Editor Mode

The dedicated in-demo **camera editor workspace**: a Panorama overlay that frames the live
game as a preview viewport with a right-side property inspector, a bottom timeline, and
modal tools (Customize, graph editor). Toggle with `mirv_filmmaker editor toggle`
(commonly bound, e.g. `bind "F9" "mirv_filmmaker editor toggle"`).

Related docs: [panorama_ui_guide.md](panorama_ui_guide.md) (how the Panorama bridge/JS
architecture works), [customize-feature-handoff.md](customize-feature-handoff.md) (the
Customize modal), [follow-camera/](follow-camera/README.md) (the Follow system the editor's
Follow inspector edits), [filmmaker_config_panel.md](filmmaker_config_panel.md) (the
standalone Config panel — the editor's "little sibling").

## What the workspace is

`CameraEditorHud` is a **presentation shell** over the existing camera back-ends — it adds
no new editing logic. While enabled it:

- frames the live game as a **preview** (top-left) with a dimmed letterbox; the preview is
  either a crop of the full-screen frame or, with `editor scale on`, a TRUE scaled viewport
  (the whole frame shrunk via a D3D blit + matching HUD CSS transform — the two scaling
  paths must match 1:1, see [memory: camera-editor-viewport-scaling]);
- shows a right-side **inspector** with per-mode sections (camera path / Follow camera /
  playback);
- docks a **bottom panel** whose mode is selectable (`editor curveeditor ...`): the custom
  camera timeline (default), the experimental graph editor, or the native CS2 demo timeline;
- suspends mirv free-cam mouse-look and shows the OS cursor while its UI wants the mouse.

## The pieces (and where they live)

All paths under `AfxHookSource2/Filmmaker/`.

| Piece | Files | Notes |
|---|---|---|
| Workspace shell | `Panorama/CameraEditorHud.{h,cpp}` + `Panorama/CameraEditorJs.h` (+ per-section JS fragments `CameraEditorPathInspectorJs.h`, `CameraEditorFollowInspectorJs.h`, `CameraEditorCustomizeJs.h`, `CameraEditorDebugJs.h`) | The C++ bridge pushes state JSON per frame; JS emits `mirv_filmmaker ...` console commands back. |
| Camera timeline (scrub bar, keys, easing) | `Panorama/CameraTimelineHud.{h,cpp}` + `CameraTimelineJs.h`; back-end `Movie/CamPlayback.cpp`, command surface `CameraTimelineCommand.cpp` (`camtl`) | Space/▶ gating contract: see [memory: space-play-gating-contract]. |
| Graph editor (experimental) | `Panorama/GraphEditorExperimentHud.{h,cpp}` + `GraphEditorJs.h`; model `Movie/GraphExpModel.cpp`; command `GraphEditorCommand.cpp` | Opt-in, fully isolated; never mutates the stable CameraPath/CamMarkers. |
| Marker / dolly path | `Movie/CameraPath.cpp` (+ `CameraPathPicking.cpp` for the crosshair hover pick), `Movie/CamMarkers.cpp`, `Movie/CamPathEval.cpp`; HUD `Panorama/MarkerHud.{h,cpp}`; command `MarkerCommand.cpp` (`marker`) | BO2-style: K = place, L = delete aimed, F = edit aimed (in free cam). |
| Follow / Lock-On camera | `Movie/FollowCamera.cpp` + `FollowTarget*`/`FollowEventIndex`; command `FollowCommand.cpp` (`follow`) | Full docs in [follow-camera/](follow-camera/README.md). |
| Customize (loadout) modal | JS in `Panorama/CameraEditorCustomizeJs.h` (modal) + `CameraEditorCustomizePreviewJs.h` (3D preview wiring); queries `Cosmetics/CosmeticUiQueries.{h,cpp}`; backend `Cosmetics/` | Per-player skins/knife/gloves/agent — see [cosmetics-overview.md](cosmetics-overview.md). Includes the 3D player preview ([panorama-player-preview-summary.md](panorama-player-preview-summary.md)). |
| Movie/director HUD + input | `Movie/MovieMode.cpp`, `Panorama/MovieHud.{h,cpp}`; input routing in `Filmmaker.cpp` (`MovieInput_*`) | Scroll = cycle cams, LMB/RMB = switch player, Space = pause, F8 = HUD. |

## Console surface

Everything routes through `mirv_filmmaker` (dispatcher: `FilmmakerCommand.cpp`; one
`*Command.cpp` file per feature):

```
mirv_filmmaker editor [on|off|toggle]      the workspace itself
mirv_filmmaker editor scale [...]          true scaled preview viewport
mirv_filmmaker editor curveeditor [...]    native | graph | timeline | camera bottom panel
mirv_filmmaker editor hud [hidden|game|full|cycle]   game UI behind the editor
mirv_filmmaker editor debug [...]          viewport/HUD debug overlay
mirv_filmmaker camtl ...                   camera timeline (scrub, keys, easing)
mirv_filmmaker marker ...                  marker/dolly path
mirv_filmmaker follow ...                  follow / lock-on camera
mirv_filmmaker grapheditor ...             experimental graph editor
mirv_filmmaker cosmetics ...               customize backend
```

Drive the UI from netcon with short calls into predefined JS helpers (`$.Filmmaker.*`) —
`ui_eval` truncates at 256 bytes ([memory: netcon-256-and-ui-helpers]).

## Known contracts / gotchas

- **All Panorama work runs on the game's MAIN thread** (`RunMainThreadFrame`); calling
  `RunScript` from the render thread crashes V8. See CLAUDE.md's threading section.
- **Per-frame close paths must no-op when already closed** — a per-frame `closeCustomize`
  unconditionally closing dropdowns killed unrelated UI ([memory: editor-perframe-close-safety-nets]).
- Round-marker rescale + native-bar EndPlayback/gear integration and the gear-settings
  popup behavior are covered in [memory: camera-editor-mode-feature].
- Dragging UI must be built from `Slider` panels (no mouse-move event in HUD JS).
