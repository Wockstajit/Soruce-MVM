# Follow / Attach Camera — QA Automation Handoff

The Follow camera is **100% console-command driven**, so an automation bot can exercise and
assert every mode over the netconsole **without the physical mouse**. Drive with
`mirv_filmmaker follow …` / `editor …`; assert with `follow state64` (+ `follow campose`).

## Prerequisites

1. **Build** — `build.bat` from the repo root → `BUILD OK`. Confirms:
   - `build\staging-release\bin\HLAE.exe` + `build\staging-release\bin\x64\AfxHookSource2.dll`
   - `build\staging-release\bin\x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe` (the Go demo-info
     helper — the `go build` step must succeed, not just warn).
2. **A demo** with players, grenades, weapon drops and a C4 (any competitive demo).
3. PowerShell 5+ (the harness scripts are cross-version).

## Launch CS2 with a netconsole (no GUI, no input takeover)

```powershell
powershell -ExecutionPolicy Bypass -File automation\launch\launch-cs2-netcon.ps1 -Port 29010 `
  -Cs2Dir "F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive"
```

Launches CS2 windowed 1600x1200 with the hook injected and `-netconport 29010`, then waits
for the window + port to be responsive. Adjust `-Cs2Dir` for the local Steam path. Then
load and play/pause a demo (`playdemo <name>` over the netcon, or via the UI).

Send commands over the netcon with the shared helper or the verifier's `Send`/`Read-State`
pattern ([automation/netcon/cs2-netcon.ps1](../../automation/netcon/cs2-netcon.ps1),
[automation/netcon/fm-eval.ps1](../../automation/netcon/fm-eval.ps1)). `state64` output is base64 (chunked
`index/total payload`); decode and `ConvertFrom-Json` — see `Read-EncodedState` in
[verify-followcam.ps1](../../automation/verify/verify-followcam.ps1).

## One-shot verifiers (already in the repo)

```powershell
pwsh automation\verify\verify-followcam.ps1 -Port 29010          # lifecycle, placement, mouse, preview, screenshots
pwsh automation\verify\verify-grenade-tracking.ps1 -Port 29010   # discover grenade, seek before throw, prove reacquire
```

Artifacts (logs, screenshots, `run.json`) land in `automation\runs\<name>\<timestamp>\`.

## Exercising the six modes (command → assert)

Open the workspace first: `mirv_filmmaker editor on`. Read state with
`mirv_filmmaker follow state64` → JSON `f`. Add `mirv_filmmaker follow campose` after a
preview frame to assert the camera actually moved to the target.

### 1. Player lock-on
```
mirv_filmmaker follow mode lockon
mirv_filmmaker follow type player
mirv_filmmaker follow place
mirv_filmmaker follow nearest
mirv_filmmaker follow preview
```
Assert: `f.mode==0`, `f.type==0`, `f.hasCamera==true`, `f.targetIndex>=0`,
`f.targetValid==true`, `f.enabled==true`. `campose`: `pos` ≈ placed point, `target` ≈ the
player.

### 2. Player bone attach
```
mirv_filmmaker follow type player
mirv_filmmaker follow mode attach
mirv_filmmaker follow nearest
mirv_filmmaker follow bone head
mirv_filmmaker follow offset 0 0 8
mirv_filmmaker follow fov 70
mirv_filmmaker follow preview
```
Assert: `f.mode==1`, `f.attachment=="head"`, `f.offset==[0,0,8]`, `f.fov==70`,
`f.enabled==true`. `campose`: `mode=attach`, `pos` tracks the head, survives the player's
death while the body persists (`f.holdLast`/`switchWeapon` as configured).

### 3. Grenade
```
mirv_filmmaker follow type grenade
mirv_filmmaker follow nearest          # or selecthandle <h> from f.candidates
mirv_filmmaker follow previewtick      # routes to the throw-tick seek + reacquire
```
Assert: `f.type==1`, after `previewtick` `f.grenadePending==true` then the demo seeks to
~throw−40 and the projectile is reacquired (`f.targetValid==true`). Toggle `follow mode
attach` to ride it.

### 4. Weapon / C4 (event-driven)
```
mirv_filmmaker follow type weapon
mirv_filmmaker follow state64          # wait for f.eventStatus==2 (ready)
mirv_filmmaker follow eventselect <index from f.events[]>
mirv_filmmaker follow previewtick      # jumps to drop tick − 40, sets weaponSource=dropped
mirv_filmmaker follow nearest
mirv_filmmaker follow preview
```
Assert: `f.type==2`, `f.events[]` non-empty once `f.eventStatus==2`,
`f.selectedEvent==<index>`, after `previewtick` `f.weaponSource==2` (dropped). For held
tracking: `follow weaponsource held` → `f.weaponSource==1`, camera rides the player's
active weapon through switches (`f.weaponPlayerIndex>=0`).

### 5. Weapon attach point
```
mirv_filmmaker follow type weapon
mirv_filmmaker follow mode attach
mirv_filmmaker follow weaponsource held
mirv_filmmaker follow nearest
mirv_filmmaker follow bone muzzle
mirv_filmmaker follow preview
```
Assert: `f.mode==1`, `f.type==2`, `f.weaponSource==1`, `f.attachment=="muzzle"` (only if the
model exposes it — check the selected candidate's `attachments[]`). `campose` tracks the gun
as the player moves/shoots.

### 6. Lock-on ↔ attach toggle (any type)
```
mirv_filmmaker follow mode attach
mirv_filmmaker follow mode lockon
```
Assert: `f.mode` flips; placement (`hasCamera`) is required only in lock-on, while attach
owns the view without a placed camera.

## State fields to assert (no screenshots needed)

From `follow state64`: `enabled`, `hasCamera`, `mode`, `type`, `typeName`, `weaponSource`,
`weaponPlayerIndex`, `targetIndex`, `targetHandle`, `targetValid`, `targetName`,
`targetDistance`, `offset[3]`, `rotation[3]`, `fov`, `look`, `position`, `prediction`,
`deadzone`, `maxTurn`, `autoDead`, `switchWeapon`, `switchBomb`, `holdLast`, `attachment`,
`grenadePending`, `grenadeThrowTick`, `eventStatus`, `selectedEvent`, `events[]`,
`candidates[]`.

From `follow campose`:
`[followcam][campose] enabled=… mode=lockon|attach type=… pos=(x y z) ang=(p y r) fov=… target=(x y z).`
— assert `pos`/`ang` track `target` (lock-on: `pos` static, `ang` follows; attach: `pos`
rides the target ± `offset`).

## Screenshot spot-checks (optional)

For visual confirmation, capture the primary monitor via the harness (engine/OS capture, no
mouse):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File automation\capture\capture-main-monitor.ps1 -Out shot.png
```

`verify-followcam.ps1` already does this for the Path-camera and Follow-camera UIs, using
`mirv_filmmaker editor eval "$.CamEditor.setInspectorMode('follow')"` and
`setAttachmentMenuOpen(true)` to frame the panel first.

