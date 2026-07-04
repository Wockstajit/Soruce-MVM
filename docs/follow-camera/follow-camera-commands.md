# Follow / Attach Camera — Command Reference

Every Follow-camera control is a console command. This is the **automation surface** — a
human keybind, the Panorama UI, and a netconsole bot all drive the feature through these.
Dispatch lives in [`Follow_RunCommand`](../../AfxHookSource2/Filmmaker/FollowCommand.cpp)
(routed from the `mirv_filmmaker` dispatcher in `FilmmakerCommand.cpp`).

Prefix everything with `mirv_filmmaker`. Run `mirv_filmmaker follow` with no args for the
in-game help text.

## Editor (workspace) commands

The Follow inspector only exists inside the camera editor workspace.

| Command | Effect |
|---|---|
| `mirv_filmmaker editor on` / `off` / `toggle` | Open/close the camera editor workspace (preview + inspector + bottom editor). |
| `mirv_filmmaker editor scale on` / `off` / `toggle` | True scaled-preview viewport (whole frame shrunk, not cropped). |
| `mirv_filmmaker editor curveeditor native` / `graph` / `timeline` / `camera` / `toggle` | Choose the bottom editor. |
| `mirv_filmmaker editor state64` | Base64 JSON dump of editor state (`[cameraeditor][state64]`). |
| `mirv_filmmaker editor eval <panorama js>` | Run JS in the editor context (optional UI poke). |
| `mirv_filmmaker camtl cursor on` / `off` / `toggle` | UI-mouse vs game-mouse (freecam). Forced on while the editor is open. |

## Lifecycle / placement

| Command | Effect |
|---|---|
| `follow place` | Place (or **Replace**) the lock-on camera at the current free-cam pose. |
| `follow reposition` | Enter reposition mode (move in freecam, left-click to set). |
| `follow repositionplace` / `repositioncancel` | Commit / cancel a reposition. |
| `follow clear` | Remove the placed camera. |
| `follow preview` (alias `play`) | **Live Preview**: make the follow camera the active view. |
| `follow stop` | Stop Live Preview / release the view. |
| `follow status` | Human-readable one-line status (`[followcam] …`). |
| `follow state` | Raw state JSON (`[followcam][state]`). |
| `follow state64` | Base64 state JSON (`[followcam][state64]`) — chunked; use for assertions. |

> Lock-on requires a placed camera; **attach mode needs no placement** — it rides the
> target directly (`OwnsView()` returns true in attach mode without a placed camera).

## Type / mode / source (the six presets)

| Command | Effect |
|---|---|
| `follow type player` | Target a player. |
| `follow type grenade` | Target a thrown grenade. |
| `follow type weapon` (aliases `droppedweapon`, `bomb`, `c4`) | Target a held/dropped weapon or the C4. |
| `follow mode lockon` / `attach` | Turret vs ride-along (orthogonal to type). |
| `follow weaponsource auto` / `held` / `dropped` | For Weapon targets: auto-pick, the player's active weapon, or the ground entity. |
| `follow bone <name>` / `follow attachment <name>` | Attach point name (e.g. `head`, `pelvis`, a weapon muzzle point). Only points the model actually exposes resolve. |

UI preset → commands:

| Preset button | Commands issued |
|---|---|
| Player | `follow type player` |
| Grenade | `follow type grenade` |
| Weapon / C4 | `follow type weapon` |
| Player Bone | `follow type player` + `follow mode attach` |
| Weapon Attach | `follow type weapon` + `follow mode attach` |

## Target selection

| Command | Effect |
|---|---|
| `follow nearest` (alias `newest`) | Select the nearest/newest candidate of the current type. |
| `follow select <entityIndex>` | Select a candidate by entity index. |
| `follow selecthandle <handle>` | Select by entity handle (survives index reuse). |
| `follow trackgrenade` | Arm the grenade throw-tick seek + reacquire flow. |

## Events (Weapon / C4 / Grenade) + Preview-Tick

| Command | Effect |
|---|---|
| `follow eventselect <index>` | Select a recorded drop/pickup event (index from the state JSON `events[]`). |
| `follow previewtick` | Jump the demo to ~40 ticks before the selected event (grenade → routes to the throw-tick flow), then arm acquisition. A drop sets `weaponSource=dropped`. |

## Framing (offset / rotation / FOV)

| Command | Range | Effect |
|---|---|---|
| `follow offset <x> <y> <z>` | — | Set the full offset (local-space in attach). |
| `follow offsetx` / `offsety` / `offsetz <v>` | ±256 (UI) | Per-axis offset (slider granularity). |
| `follow rotation <pitch> <yaw> <roll>` | — | Set the full rotation trim. |
| `follow rotpitch` / `rotyaw` / `rotroll <v>` | ±180 (UI) | Per-axis rotation. Attach: orientation trim; lock-on: aim trim. |
| `follow fov <v>` | 1–170 (UI) | Camera FOV. |
| `follow preset <name>` | — | `centered\|above\|low\|shoulder\|side\|behind\|orbitleft\|orbitright\|weapon\|bomb`. |

## Advanced tracking

| Command | Clamp | Effect |
|---|---|---|
| `follow look <v>` | 0–5 | Look/aim smoothing half-time (s). |
| `follow position <v>` | 0–5 | Position smoothing half-time (s). |
| `follow prediction <v>` | 0–2 | Lead the target by velocity × this. |
| `follow deadzone <v>` | 0–45 | Aim deadzone (degrees). |
| `follow maxturn <v>` | 0–4000 | Max turn rate (°/s). |
| `follow autodead <0\|1>` | — | Auto-disable when the target dies. |
| `follow switchweapon <0\|1>` | — | On death, switch to the dropped weapon. |
| `follow switchbomb <0\|1>` | — | On death, switch to the dropped bomb. |
| `follow hold <0\|1>` | — | Hold the last known position when the target is lost. |

## Instrumentation

| Command | Output |
|---|---|
| `follow debug <0\|1>` | Per-frame `[followcam][tick]` telemetry on/off. |
| `follow campose` | `[followcam][campose] enabled=<0\|1> mode=<lockon\|attach> type=<…> pos=(x y z) ang=(p y r) fov=<f> target=(x y z).` — the **computed** camera pose + resolved target world pos, for asserting the camera is actually tracking/attached. |
| `follow status` | `[followcam] enabled=… hasCamera=… type=… entity=… fov=… offset=(…) look=… pos=… …` |

