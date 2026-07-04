# Panorama Player Preview Research

Scope: offline CS2 demo / movie-making UI only.

Reviewed local references from:

- `reference/coding help/panorama-player-preview/612714-panorama-ui-editing.md`
- `reference/coding help/panorama-player-preview/728536-externally-hud-elements.md`
- `reference/coding help/panorama-player-preview/744463-preview-models-menu.md`
- `reference/coding help/panorama-player-preview/750805-panorama-model.md`

## Useful findings

### 1. `MapPlayerPreviewPanel` is the right native Panorama panel

The useful references are `744463-preview-models-menu.md` and `750805-panorama-model.md`.

They confirm the native player-preview route:

- Create a `MapPlayerPreviewPanel`.
- Use `map: "ui/buy_menu"`.
- Use a loadout/vanity camera such as `cam_loadoutmenu_ct` or `cam_vanityloadout`.
- Set a valid AG2 model path such as `agents/models/tm_professional/tm_professional_varf.vmdl`.
- Enable `player`, `mouse_rotate`, `sync_spawn_addons`, `transparent-background`, and `pin-fov`.
- Pair `require-composition-layer` with `composition-layer-texture-name`.

This directly matches the current repo direction in `AfxHookSource2/Filmmaker/Panorama/CameraEditorCustomizePreviewJs.h` (the Customize modal's 3D-preview fragment, split out of `CameraEditorCustomizeJs.h`).

### 2. The repo already has most of the preview implementation

The existing customize modal already creates a native `MapPlayerPreviewPanel` in:

`AfxHookSource2/Filmmaker/Panorama/CameraEditorCustomizePreviewJs.h` (preview wiring; modal in `CameraEditorCustomizeJs.h`)

Important existing pieces:

- `createPreview3d()` sets `require-composition-layer` and `composition-layer-texture-name`.
- It uses `map: ui/buy_menu`, `cam_vanityloadout` / `cam_loadoutmenu_ct`, `player`, `sync_spawn_addons`, and `csm_split_plane0_distance_override`.
- It sets `hide_while_waiting_for_composite_materials: false`, which matches the reference warning about hidden/culling preview panels.
- `applyPreview()` calls `SetPlayerCharacterItemID`, `SetPlayerModel`, `EquipPlayerWithItem`, and `SetReadyForDisplay`.

So the references do help, but they mostly validate work already present in the repo.

### 3. The most likely remaining player-preview issue is lifecycle/timing, not the basic API

If the preview is black or missing, investigate these first:

- The modal must be visible and non-zero-size before creating the preview panel.
- The panel needs a unique composition-layer texture name.
- The preview may need a few frames or a demo tick nudge while paused.
- Rebuild the panel when the selected model/item set changes.
- Delete the parent/container to remove preview panels cleanly.

The current code already has delayed creation, `previewPokeFrames`, and `nudgePreviewCompositeIfPaused()`, so verification should focus on whether those are firing in the exact failing state.

### 4. Texture capture is only needed for an external/ImGui render path

The UC render-system SRV capture references are useful only if we want to copy the Panorama preview texture into another renderer, such as ImGui or a custom DX overlay.

For SOURCE:MVM's native Panorama UI, the cleaner path is to let the `MapPlayerPreviewPanel` render directly inside the filmmaker modal. Avoid adding a render-system SRV hook unless there is a strong reason to display that texture outside Panorama.

### 5. `GetHudElement` is lower priority

`728536-externally-hud-elements.md` is about finding HUD elements externally and running Panorama JS in a panel context. SOURCE:MVM already has `PanoramaBridge`, `AfxHookSource2_GetPanoramaMainMenuPanel()`, `AfxHookSource2_GetPanoramaHudPanel()`, and `RunScript` wiring.

This may help if the existing HUD/main-menu panel signatures break, but it is not the first thing to implement for player preview.

### 6. VPK Panorama editing is not useful for this feature

`612714-panorama-ui-editing.md` is mostly about editing compiled CS2 Panorama resources. SOURCE:MVM injects Panorama JavaScript at runtime through `RunScript`, so recompiling VPK Panorama files is unnecessary for this UI.

## Recommended next implementation step

Do not start with render-system hooking. Start by live-verifying the existing native preview path:

1. Open the filmmaker camera editor customize modal in a demo.
2. Confirm `createPreview3d()` succeeds and `preview3d.IsValid()` returns true.
3. Confirm the panel has non-zero layout size when created.
4. Confirm `SetReadyForDisplay` is called after creation and after model/item changes.
5. If it is still black while paused, verify whether `nudgePreviewCompositeIfPaused()` runs and whether the tick jump is enough.
6. If the panel is visible but the character is wrong/washed out, verify the selected agent has an econ item id and that `SetPlayerCharacterItemID` runs before `SetPlayerModel`.

