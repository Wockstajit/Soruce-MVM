# Cam-Editor Customization System — Handoff

> **Purpose of this file:** hand off the in-progress "Customize" (skin-changer / loadout) feature
> for the CS2 filmmaker tool (a fork of HLAE / AfxHookSource2) to a fresh chat. It contains: (1)
> the original approved plan, (2) exactly what has been built so far (compiling), and (3) the
> remaining phases with implementation guidance.
>
> **Status:** Phases 0, 1, 2, and 3 are implemented and **compile + stage cleanly**
> (`STAGE_EXIT=0`, staged to `build/staging-release`). Phase 4 (knife/glove/agent MODEL swaps in
> the demo render) is pending. Phase 3 (weapon skins) needs **in-game verification** (see caveat).
>
> **2026-06-26 update — real catalog + current-loadout display.** The stub `COSMETICS` is gone:
> `automation/tools/generate_cosmetics_catalog.py` now parses the live `items_game.txt` with the `vdf`
> library (the old hand-rolled KeyValues parser silently dropped ~80% of paint kits — it produced
> only 168 skins) and emits **every** released finish: 1407 weapon skins, 611 knife finishes,
> 72 glove finishes, 80 agents → `CameraEditorCosmeticsCatalog.inc` (#included into the editor JS)
> + `Data/cosmetics.json`. Names come from each paint kit's `description_tag` (real finish names
> like "Redline"/"Hyper Beast", never dev names like `cu_ak47_cobra`); rarity from
> `paint_kits_rarity`; per-skin wear ranges from `wear_remap_min/max`. The modal now also shows
> the spectated player's **current** weapons/knife/gloves: C++ `BuildCustomizeTargetJson` reads the
> pawn's owned weapons (def + fallback paint) and `m_EconGloves` (new non-fatal offset), and the JS
> preselects the matching catalog entry per slot, filtering the primary/secondary dropdowns to that
> player's actual weapon. Gloves default to the team default (CT 5029 / T 5028) when none.
>
> Plan file (original): `C:\Users\ayden\.claude\plans\review-this-repo-as-polymorphic-treasure.md`
> Related memory: `camera-customize-feature.md`, `camera-editor-mode-feature.md`,
> `valveresourceformat-cli-tool.md`, `user-runs-builds.md`.

---

## 0. Critical context for the next agent

- **The cam-editor UI is embedded Panorama-JS inside C++ headers**, not separate XML/CSS. The
  whole editor is one big JS string literal in
  `AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h`, built at runtime into the in-game HUD
  (CSGOHud) context by `CameraEditorHud.cpp`.
- **MSVC caps a single string literal at 16 KB** → error `C2026`. The JS is split into adjacent
  `R"EDJS( ... )EDJS"` chunks that the compiler concatenates. **When you add a big JS block, you
  MUST add `)EDJS"` / `R"EDJS(` chunk breaks** between statements or the build fails. (This bit us
  once already.)
- **UI ↔ C++ bridge:** JS calls `GameInterfaceAPI.ConsoleCommand("mirv_filmmaker ...")`. C++ pushes
  state to JS as a JSON string attribute (`BuildStateJson()` in `CameraEditorHud.cpp`) which the JS
  reads in `$.CamEditor.render()`.
- **JS can reach native HUD panels** via `ctx.FindChildTraverse('PanelId')` (used to hide the
  native End/gear buttons; `ctx = $.GetContextPanel()` = CSGOHud).
- **Panorama in-game HUD JS has no mouse-move event** — any dragging must use native `Slider`
  panels. (Existing project constraint.)
- **Build:** `build.bat` compiles, stages, **then launches CS2 + a live dashboard** (and uses
  `pause` on errors), so it hangs a non-interactive run. A compile-only batch was used during this
  work (mirrors build.bat minus the live.bat tail). The user normally runs `build.bat` themselves.
  Per the `user-runs-builds` memory, build after edits.
- **Reference files:** extracted native CS2 Panorama is at `panorama ref/` (read-only). Key ones:
  `layout/hud/huddemocontroller.xml` + `scripts/hud/huddemocontroller.js` (native demo settings),
  `styles/csgostyles.css` (rarity colors, lines ~51-79), `layout/vanity-loadout.xml` /
  `layout/inspect.xml` (`MapPlayerPreviewPanel` 3D preview), `layout/itemtile.xml`,
  `layout/slider_toggle.xml`.

### Decisions already made with the user
- **Build the full system, phased** (UI + cleanup first → skins → model swaps).
- **Preview:** attempt native 3D `MapPlayerPreviewPanel`, fall back to 2D.
- **Item data:** generate from CS2 game files (Phase 2).

---

## 1. ORIGINAL PLAN (as approved)

### Context
The cam editor could place cameras / drive dollies / follow players, but had **no cosmetic
customization**, its bottom "regular timeline" still showed CS2's **End / End-Playback button**
(which dumps you out of the demo), and there was no consolidated menu for the four native
demo-playback toggles. This change adds a **loadout-style "Customize" modal** (only while
spectating a player in 1st/3rd person, never freecam) to override the spectated player's **agent,
arms/viewmodel, gloves, knife, and weapon skin**, with thumbnails + rarity coloring and a 3D
preview when possible. Selections must update both the modal preview and the actual demo render.
It also removes the native End button and moves the four native demo settings into a **gear menu**
shown only in cam-editor mode. The cosmetic-application layer is the only part needing new native
work (AfxHookSource2 had no econ/model hooks, only viewmodel offset/FOV/hand + glow).

### Key facts established during research
- **Native demo settings → cvars** (from `panorama ref/scripts/hud/huddemocontroller.js`). The
  gear menu just issues these — **no C++ needed**:
  - X-Ray → `spec_show_xray 0|1`
  - True View → `cl_demo_predict 0|1` (use `2` when "allow wrong version" is also on)
  - Include DOA actions → `cl_trueview_show_doa_predictions 0|1` (only when `cl_demo_predict > 0`)
  - Allow demo mismatch versions → `cl_demo_predict 1 ↔ 2`
- **The "End" button** = native `#EndPlayback` (`panorama ref/layout/hud/huddemocontroller.xml:82`).
  Our own timeline has no End button. Hide via JS `ctx.FindChildTraverse('EndPlayback')` (or the
  existing `mirv_panorama panelStyle panelId=EndPlayback` command in `DeathMsg.cpp:1950`).
- **Spectator state** readable in C++: `CEntityInstance::GetObserverMode()` / `GetObserverTarget()`
  (`ClientEntitySystem.cpp`). OBS modes: `IN_EYE=2` (1st), `CHASE=3` (3rd), `ROAMING=4` (freecam).
- **Rarity colors** in `styles/csgostyles.css` (`color-rarity-0..7`); `MapPlayerPreviewPanel` 3D
  preview with camera presets `cam_loadoutmenu_ct` / `cam_vanityloadout`; toggle switch
  `slider_toggle.xml`; popup/backdrop pattern `popups/popups_shared.css`.
- **Osiris econ mechanism** (to adapt for the spectated player, offline only): write per-frame on
  the weapon's `C_EconItemView` — `m_iItemIDHigh = -1` (force fallback), `m_iItemDefinitionIndex`,
  `m_iAccountID`, `m_nFallbackPaintKit`, `m_flFallbackWear`, `m_nFallbackSeed`,
  `m_nFallbackStatTrak`, `m_szCustomName`; for knife/glove/agent also swap the entity model index.
  All offsets resolved at runtime through the existing schema system.

### Phased plan
- **Phase 0 — Timeline/settings cleanup** (pure Panorama-JS): remove the End button; add a gear
  settings menu (4 toggles → the cvars above) shown only while the editor is active.
- **Phase 1 — Customize button + modal shell** (JS + small C++ state push): push `obsMode`/
  `obsTarget` into `BuildStateJson()`; show a "Customize" button only when spectating a player in
  1st/3rd person; large centered modal with darkened backdrop, X + click-outside close; five rich
  dropdowns (Agent, Arms/Viewmodel, Gloves, Knife, Weapon skin) with thumbnails + rarity coloring;
  keep-original arms/FOV toggle; preview (native 3D attempt → 2D fallback).
- **Phase 2 — Item dataset + thumbnails** (offline generation): from `items_game.txt` +
  `csgo_english.txt` via the ValveResourceFormat CLI → JSON dataset + thumbnails shipped with the
  tool; load it into the editor JS replacing the stub data.
- **Phase 3 — Weapon-skin override in the demo** (new C++ econ pass): resolve econ schema offsets
  non-fatally; per-frame write the fallback fields on the spectated player's active weapon; wire
  the skin dropdown to `mirv_filmmaker cosmetics skin ...`.
- **Phase 4 — Model swaps: knife, gloves, agent, arms** (harder): change item def index + force
  models; reuse `mirv_viewmodel` for FOV/offset/hand; the keep-original toggle gates arms override.

---

## 2. WHAT HAS BEEN DONE (Phases 0, 1, 3 — all compiling)

### Phase 0 — Timeline/settings cleanup ✅
File: `AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h`
- **End button removed:** `setNativeEndHidden(hide)` helper (near the `api`/`st` declarations)
  uses `ctx.FindChildTraverse('EndPlayback')` to force the native button hidden while the editor is
  open; restored on close. Called with `true` in the enabled render path, `false` in the disabled
  branch.
- **Native gear removed too:** the same helper also hides CS2's own demo-bar gear `#SettingsButton`
  (scoped via `EndPlayback`'s parent `ControlRow` so it can't hit a same-named panel elsewhere),
  since our ⚙ replaces it.
- **Gear settings menu:** a `⚙` button on the bottom tab bar (next to Regular Timeline / Camera /
  Graph) opens `settingsOverlay` / `settingsCard` with four pill toggles wired to the native cvars
  (`spec_show_xray`, `cl_demo_predict` 1/2, `cl_trueview_show_doa_predictions`). Reads state back
  with `GameInterfaceAPI.GetSettingString` so the pills reflect the engine. DOA + mismatch rows
  disable unless True View is on. The gear lives on the tab bar, so it only exists while the editor
  is open. Functions: `settingToggle`, `setToggle`, `refreshSettings`, `openSettings`,
  `closeSettings`, `toggleSettings`, `rdInt`.

### Phase 1 — Customize button + modal shell ✅
- **C++ state push:** `AfxGetLocalObserverState(int* outTargetIndex)` added to
  `AfxHookSource2/ClientEntitySystem.cpp` (+ declared in `.h`). Resolves the local viewer's pawn
  (split-screen player 0 → pawn) and returns observer mode + spectated entity index.
  `CameraEditorHud::BuildStateJson()` now emits `"obsMode"` and `"obsTarget"` (it `#include`s
  `../../ClientEntitySystem.h`).
- **Customize button** (in `CameraEditorJs.h`): a `customizeBtn` **pinned to the editor's
  bottom-right corner** (child of `root`, z-index 120, anchored `horizontalAlign:right` /
  `verticalAlign:bottom`), so it survives Path/Follow/Attach/Lock-on tab switches. **Always
  visible while the editor is open** (per user request — the earlier obsMode/freeCam gate hid it
  because the editor reports "live free cam" even on a player POV). `render()` resolves the target
  player best-effort: `obsTarget` → `st.follow.targetIndex` → nearest candidate → `custTargetIndex`,
  with `custTargetName` from `st.follow.candidates`. NOTE: `AfxGetLocalObserverState` may be
  returning `obsMode=0` in some demos (it didn't gate correctly) — if skins don't target the right
  player, debug that helper or rely on the follow-target fallback.
- **Modal** (`custOverlay` → `custWin`): 80% × 82% centered window, darkened backdrop
  (`rgba(0,0,0,0.78)`, z-index 230), corner **X** (`custClose`), **click-outside-to-close**
  (`custOverlay` `onactivate` → `closeCustomize`), inner clicks swallowed. Left = preview
  (`prevWrap`); right = controls (`ctrlCol`).
- **Rich dropdown** `itemDrop(parent, id, onPick)`: thumbnail/rarity swatch + rarity-colored name
  in both the collapsed field and the popup rows. Reuses the shared `customDrops` / `closeAllDrops`
  / `toggleDrop` / `showDropPopup` machinery (popup parented to `root` at z-index 420 so it floats
  above the modal). `RARITY` map mirrors `csgostyles.css` colors.
- **Five dropdowns** (Agent, Arms/Viewmodel, Gloves, Knife, Weapon Skin) + a **keep-original
  arms/viewmodel/FOV** pill toggle (`armsPill` / `setArmsPill`, dims the arms drop when on).
- **Preview:** attempts `$.CreatePanel('MapPlayerPreviewPanel', …, {camera:'cam_vanityloadout'})`;
  if invalid, builds a 2D summary card (`preview2d`: agent name + rarity swatch + skin name + hint).
  `updatePreview()` refreshes on each selection.
- **Stub dataset** `COSMETICS` (agent/arms/gloves/knife/skin), with **real paint-kit IDs** for
  skins (e.g. Redline `282`, Asiimov `801`, Dragon Lore `344`, Howl `309`). `custSel` tracks
  selections; `populateCustomize()` fills the drops; `pickCosmetic(slot, value)` applies.
- Modal closes when the editor disables or the cursor leaves UI mode.

### Phase 3 — Weapon-skin override (functional core) ✅ (compiles; needs in-game verify)
- **New module** `AfxHookSource2/Filmmaker/Movie/MirvCosmetics.cpp` / `.h` (added to
  `AfxHookSource2/CMakeLists.txt` `target_sources`). Holds a `pawn entity index → SkinOverride`
  map. `Cosmetics_RunFrame()` (called from `Filmmaker.cpp` `RunMainThreadFrame`, after the
  follow/path passes) writes per-frame on the spectated player's **active weapon**
  (`pawn->GetActiveWeaponHandle()`):
  - `m_iItemIDHigh = -1` (in `m_AttributeManager.m_Item`) → forces fallback compositing
  - `m_nFallbackPaintKit`, `m_flFallbackWear`, `m_nFallbackSeed`, `m_nFallbackStatTrak`
  - Cheap no-op when no overrides or offsets missing.
  - API: `Cosmetics_SetSkin/ClearPlayer/ClearAll/RunFrame/Available`.
- **Econ schema offsets** added to `AfxHookSource2/SchemaSystem.h` (`C_EconEntity`,
  `C_AttributeContainer`, `C_EconItemView`) and resolved **non-fatally** in
  `SchemaSystem.cpp::initCosmeticsOffsets()` (gated on `g_cosmeticsOffsetsOk`, **kept off the
  mandatory `bOk` chain** so a renamed field disables skins instead of `ErrorBox`-ing startup).
  Called from `HookSchemaSystem()` right after `initSchemaSystemOffsets()`, before the offset map
  is cleared.
- **Command:** `mirv_filmmaker cosmetics skin <pawnIdx> <paintKit> <wear> <seed> [statTrak]` /
  `clear <pawnIdx>` / `clearall` → `DoCosmetics()` in `FilmmakerCommand.cpp` (+ dispatch branch +
  `#include "Movie/MirvCosmetics.h"`).
- **JS wiring:** `pickCosmetic('skin', value)` issues
  `mirv_filmmaker cosmetics skin <custTargetIndex> <paintKit> 0.01 0` (paintKit `0` → `clear`).
- **Cleanup:** `CameraEditorHud::OnExit()` calls `Cosmetics_ClearAll()` (+ includes
  `../Movie/MirvCosmetics.h`).

### Files touched (complete list)
| File | Change |
|---|---|
| `AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h` | gear menu, native End+gear hide, Customize button, modal, `itemDrop`, `pickCosmetic` skin wiring, **+ several `)EDJS"`/`R"EDJS(` chunk breaks** |
| `AfxHookSource2/Filmmaker/Panorama/CameraEditorHud.cpp` | `obsMode`/`obsTarget` in `BuildStateJson`; includes; `OnExit` → `Cosmetics_ClearAll()` |
| `AfxHookSource2/ClientEntitySystem.cpp` / `.h` | `AfxGetLocalObserverState()` |
| `AfxHookSource2/SchemaSystem.h` | econ offset structs + `extern bool g_cosmeticsOffsetsOk` |
| `AfxHookSource2/SchemaSystem.cpp` | `initCosmeticsOffsets()` (non-fatal) + call |
| `AfxHookSource2/Filmmaker/Movie/MirvCosmetics.cpp` / `.h` | **NEW** — skin override map + per-frame apply |
| `AfxHookSource2/CMakeLists.txt` | added `MirvCosmetics.cpp/.h` |
| `AfxHookSource2/Filmmaker/Filmmaker.cpp` | include + `Cosmetics_RunFrame()` in `RunMainThreadFrame` |
| `AfxHookSource2/Filmmaker/FilmmakerCommand.cpp` | include + `DoCosmetics()` + dispatch branch |

### ⚠️ Phase 3 caveat — verify in-game
The fallback-field approach is exactly how Osiris skins work, **but Osiris targets the LOCAL
player**; we target the **spectated** player in a demo. Unknown: whether writing the fallback
fields makes the client **re-composite the weapon material** for a non-local demo entity. **Test:**
spectate a player holding e.g. an AK, pick `AK-47 | Redline` → if the weapon visibly changes, done;
if not, add a **forced material re-composite** (e.g. toggle the weapon's econ "initialized" flag,
or call the client's composite/refresh path) after writing the fields. This is the main open
question for Phase 3.

---

## 3. REMAINING WORK

### Phase 2 — Item dataset ✅ DONE (2026-06-26)
`automation/tools/generate_cosmetics_catalog.py` reads `scripts/items/items_game.txt` +
`resource/csgo_english.txt` straight out of `pak01_dir.vpk` (the bundled `Vpk` reader) and parses
them with the **`vdf`** package (`pip install vdf` — REQUIRED; the prior hand-rolled parser dropped
~80% of the data). It emits the full catalog:
- **Weapons:** every `[paintkit]weapon_x` combo from `client_loot_lists` + `revolving_loot_lists`
  (the authoritative "which skin on which weapon" source) → 1407 skins across 34 weapons. Each
  primary/secondary slot lists a "Default <weapon> skin" entry + that weapon's finishes; the modal
  filters the dropdown to the spectated player's actual weapon def.
- **Knives:** a curated set of universal finishes (Fade, Doppler/Gamma phases, Marble Fade, Tiger
  Tooth, Damascus, …, resolved to live IDs by internal name) + each knife's own token-matched
  finishes (Lore/Black Laminate/Autotronic/etc.) → ~30 per knife, 611 total.
- **Gloves:** every glove paint kit (id ≥ 10000) grouped to its glove type by name prefix, incl.
  Broken Fang (def 4725) → 72 finishes, plus "Default CT/T Gloves" entries (5029/5028).
- **Agents:** the shipped agent `.vmdl` set from the VPK, split CT/T (names acronym-cleaned).
Names = `description_tag` (real finish names); rarity = `paint_kits_rarity`; wear = `wear_remap_*`.
Output: `CameraEditorCosmeticsCatalog.inc` (#included into `CameraEditorJs.h`, chunked under MSVC's
16 KB literal cap) + `Data/cosmetics.json` (reference). Re-run after a CS2 update to refresh.
**Thumbnails:** still rarity-color swatches only — `alternate_icons2` is no longer populated in
items_game, and econ `.vtex_c` icons don't load via `s2r://` in the HUD context. The 3D preview
already renders the real items via `EquipPlayerWithItem` faux IDs, so this is a deliberate gap.

### Current-loadout display ✅ DONE (2026-06-26)
`BuildCustomizeTargetJson` (CameraEditorHud.cpp) emits `weapons.{primary,secondary,knife,gloves}`
for the spectated pawn: weapons via the owned-weapon scan (def index + `m_nFallbackPaintKit`/wear),
gloves via the new `C_CSPlayerPawn::m_EconGloves` offset (non-fatal — `ReadGloveDefIndex`, returns
0 for the default team gloves). JS `applyCustomizeTargetLoadout` + `loadoutDefaultSelection` /
`glovesDefaultSelection` preselect the matching catalog entry so the modal opens showing what the
player actually has (e.g. "M4A1-S | Hyper Beast", their real knife), defaulting gloves to the team
default when none. Weapon paint reads rely on the demo populating the fallback paint field; when it
doesn't, the slot still shows the correct weapon with its "Default … skin" entry.

### Phase 4 — Model swaps: knife, gloves, agent, arms
Build on Phase 3's per-frame pass in `MirvCosmetics.cpp` and the `pickCosmetic` TODO hooks:
- **Knife:** set the weapon's `m_iItemDefinitionIndex` to the knife def + apply its paint kit +
  force the view/world model index; verify viewmodel sequence compatibility.
- **Gloves:** override the player's glove econ item (`m_iItemDefinitionIndex` + `m_nFallbackPaintKit`
  + `m_flFallbackWear`) and force a glove-model refresh. (Needs new schema offsets, e.g. the glove
  wearable / `m_hMyWearables` / `m_EconGloves`.)
- **Agent:** swap the player pawn's body model to the chosen agent model (precache + set model
  index); same-skeleton swaps are safe — validate animations.
- **Arms/viewmodel/FOV:** reuse the existing `mirv_viewmodel` command (`ViewModel.cpp`) for
  FOV/offset/hand; the "Override arms with <agent>" path swaps the arms model. The modal's
  keep-original toggle gates whether any of this applies (default: keep the spectated player's
  original).
- Add the matching `mirv_filmmaker cosmetics agent|knife|gloves|arms ...` subcommands in
  `DoCosmetics`, and issue them from `pickCosmetic` (the JS hooks are already stubbed).
- Reference: Osiris `InventoryChanger` for the exact field/model handling.

### Cross-cutting follow-ups
- Decide override **lifetime**: currently cleared on editor exit. Consider keying by SteamID
  (stable across rounds) instead of pawn entity index, and clearing on demo change.
- Wire the **native 3D preview** properly once agent/weapon model names exist (Phase 2 data), or
  keep the 2D fallback.
- Per-skin **wear/seed/StatTrak/nametag** controls in the modal (currently hardcoded wear 0.01,
  seed 0).

---

## 4. How to build & verify
- **Build:** run `build.bat` (it kills CS2, compiles, stages to `build\staging-release`, then
  launches CS2 + live dashboard). For a compile-only check, run just the
  `cmake -DAFX_MULTIBUILD_STAGING=ON -P cmake/MultiBuild.cmake` step inside a VS dev environment
  (the user did this via a temp batch that mirrors build.bat minus the live.bat tail).
- **Verify Phase 0/1:** play a demo → enter cam-editor mode → confirm: native End button gone, the
  redundant native gear gone, our ⚙ menu toggles flip the cvars, "Customize" shows only on
  first/third-person spectate (hidden in freecam), modal opens centered with darkened backdrop, X
  and click-outside both close it, dropdowns show rarity colors, preview updates on selection.
- **Verify Phase 3:** spectate a player with a rifle → Customize → Weapon Skin → `AK-47 | Redline`
  → watch the weapon in the demo view. If it doesn't change, implement the forced re-composite.

---

## 5. Quick reference — values & names
- **obsMode:** 1 = fixed, 2 = in-eye (first person), 3 = chase (third person), 4 = roaming
  (freecam), 0 = none. (Customize button is now ALWAYS shown while the editor is open, not gated
  on obsMode; obsMode/obsTarget are still used to pick the skin's target player.)
- **Native demo cvars:** `spec_show_xray` (0/1), `cl_demo_predict` (0/1/2),
  `cl_trueview_show_doa_predictions` (0/1).
- **Rarity (csgostyles.css):** 0 `#6a6156`, 1 `#b0c3d9`, 2 `#5e98d9`, 3 `#4b69ff`, 4 `#8847ff`,
  5 `#d32ce6`, 6 `#eb4b4b`, 7 `#e4ae39`.
- **Cosmetics command:** `mirv_filmmaker cosmetics skin <pawnIdx> <paintKit> <wear> <seed> [stattrak]`
  / `clear <pawnIdx>` / `clearall`.
- **Stub skin paint kits in the JS:** Redline 282, Asiimov 801, Dragon Lore 344, Howl 309.
