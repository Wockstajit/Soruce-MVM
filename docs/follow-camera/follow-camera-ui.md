# Follow / Attach Camera — UI

The Follow inspector lives in the camera editor workspace (`mirv_filmmaker editor on`),
right-hand inspector column, **FOLLOW CAMERA** tab. It is built in Panorama JS in
[CameraEditorJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h) (follow section
lines 259–487, `render()` follow block 711–790).

Per the Panorama input constraint (in-game HUD JS has **no** mouse-move event), every
draggable control is a native `Slider` panel — see `vslider()`
([CameraEditorJs.h:465](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h:465)).

## Layout (top → bottom)

| Block | Controls | Visibility | Source |
|---|---|---|---|
| **Header** | `FOLLOW / ATTACH CAMERA` + "one camera" hint | hint hidden in attach mode | :260 |
| **Placement** | `Place`/`Replace` · `Reposition` · `Clear` | lock-on only (`!attachMode`) | :263 |
| **Live Preview** | `▶ Preview Follow Cam` / `Stop` (big, primary) | always | :275 |
| **Status / warn** | target line + warning line | always | :280 |
| **Mode** | `Lock-on` · `Attach` segmented toggle | always | :286 |
| **Target Type** | `Player` · `Grenade` · `Weapon/C4` · `Player Bone` · `Weapon Attach` | always | :296 |
| **Target** | `Select Nearest` + `Target ▾` dropdown (scrolling candidate list) | always | :312 |
| **Weapon source** | `Auto` · `Held` · `Dropped` | Weapon type only (`type===2`) | :386 |
| **Attach point** | `Attach pt` selector + scrolling menu | Attach mode only | :396 |
| **Events** | `Preview Tick` + scrolling drop/pickup list | Grenade/Weapon (`type 1\|2`); list = Weapon only | :425 |
| **Framing** | `Off X/Y/Z` · `Pitch/Yaw/Roll` · `FOV` sliders | always | :460 |
| **Advanced Tracking** ▸ | `Look` · `Pos` · `Predict` · `Deadzone` · `Turn °/s` + on-death option buttons (collapsible) | always | :496 |

The five **Target Type** buttons are presets over `(type, mode)`
([`FOLLOW_TYPES`](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h:298)); each issues
the underlying `follow type …` / `follow mode …` commands and is highlighted when its
`__match(type, mode)` predicate holds. The **Mode** toggle flips `mode` for any type.

## Slider ranges

| Slider | Command | Range | Decimals |
|---|---|---|---|
| Off X / Y / Z | `offsetx` / `offsety` / `offsetz` | −256 … 256 | 1 |
| Pitch / Yaw / Roll | `rotpitch` / `rotyaw` / `rotroll` | −180 … 180 | 0 |
| FOV | `fov` | 1 … 170 | 0 |
| Look | `look` | 0 … 1.0 | 2 |
| Pos | `position` | 0 … 1.0 | 2 |
| Predict | `prediction` | 0 … 0.5 | 2 |
| Deadzone | `deadzone` | 0 … 10 | 1 |
| Turn °/s | `maxturn` | 0 … 1440 | 0 |

## State JSON → control mapping

C++ pushes the follow state as `st.follow` (see
[BuildStateJson](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1149); embedded by
[CameraEditorHud.cpp:196](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorHud.cpp:196)).
`render()` reads `f = st.follow`:

| Field | Drives |
|---|---|
| `mode` (0 lockon / 1 attach) | Mode toggle highlight; `attachModeOn`; placement & attach-point visibility |
| `type` (0 player / 1 grenade / 2 weapon) | Type highlight; source/events/attach visibility |
| `weaponSource` (0 auto / 1 held / 2 dropped) | Source buttons |
| `hasCamera` | `Place`↔`Replace` label |
| `enabled` | `▶ Preview`↔`Stop` label + highlight |
| `targetName`, `targetIndex`, `targetValid` | Status line + dropdown labels |
| `repositioning`, `message`, `distanceLevel` | Warning line |
| `candidates[]` | Target dropdown rows; attach-point options (`attachments[]`) |
| `attachment` | Attach-point selector label |
| `events[]`, `eventStatus` (0 idle/1 loading/2 ready/3 failed), `selectedEvent` | Event list rows + state messages |
| `grenadePending` | `Preview Tick` button label/highlight |
| `offset[3]`, `rotation[3]`, `fov` | Framing sliders |
| `look`, `position`, `prediction`, `deadzone`, `maxTurn` | Advanced sliders |
| `autoDead`, `switchWeapon`, `switchBomb`, `holdLast` | On-death option buttons |

## UI poke for automation

The editor exposes JS hooks callable via `mirv_filmmaker editor eval …`, used by the
screenshot step in `verify-followcam.ps1`:

- `$.CamEditor.setInspectorMode('follow')` — switch to the Follow tab.
- `$.CamEditor.setAttachmentMenuOpen(true)` — open the attach-point menu for a screenshot.

But the UI is **not** required to drive the feature — all controls map 1:1 to the
`mirv_filmmaker follow …` commands in [follow-camera-commands.md](follow-camera-commands.md).

