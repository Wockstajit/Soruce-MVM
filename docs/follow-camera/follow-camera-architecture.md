# Follow / Attach Camera — Architecture

## Components

| Layer | Where | Responsibility |
|---|---|---|
| State + logic | `FollowCamera` singleton | Owns `FollowCameraState`, runs the per-frame solve, picks targets, drives Preview-Tick seeks, builds the state JSON. |
| Math | `FollowCameraMath.{h,cpp}` | Pure functions: look-at, smoothing, quat→angles, local-offset rotation. No engine deps (also compiled standalone by the test target). |
| Target providers | `FollowCamera.cpp` | Adapt a player / entity / attachment / held-weapon into a position (+ optional orientation) each frame. |
| Commands | `FollowCommand.cpp` `Follow_RunCommand` | The `mirv_filmmaker follow ...` console surface (the automation API; routed from the `FilmmakerCommand.cpp` dispatcher). |
| UI | `CameraEditorJs.h` (Panorama JS) | The editor's Follow inspector. Reads the pushed state JSON, issues console commands back. |
| State bridge | `CameraEditorHud.cpp` | Embeds the follow state JSON into the editor's `state` attribute each frame and calls `render()`. |
| Pose application | `CameraBridge.h` | `CameraBridge_SetCameraPose` pushes an absolute pose; `CameraBridge_GetCurrentCamera` reads the free cam. |
| Demo pre-scan | `FilmmakerDemoInfoGo/main.go` (Go) → `DemoInfoHelper.{h,cpp}` (C++) | Writes / reads `<demo>.dem.fmjson` (v6), including the weapon/C4 event list. |

## Internal model

```cpp
enum class FollowTargetType { Player, Grenade, Weapon };  // Weapon = held/dropped weapon OR C4
enum class FollowMode       { LockOn, Attach };           // turret vs ride-along
enum class WeaponSource     { Auto, Held, Dropped };      // for Weapon type
```
Defined in [FollowCamera.h:17](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:17) /
[:26](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:26) /
[:33](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:33).

`type` and `mode` are **orthogonal** — the five UI "Target Type" buttons are presets over
`(type, mode)`, and a separate Lock-on/Attach toggle flips `mode` for any type. The state
struct ([FollowCameraState](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:77)) carries
`mode`, `rotationOffset`, `weaponSource`, `weaponPlayerIndex`, plus the pre-existing
`offset`, `fov`, smoothing, and on-death fields, and reuses `attachmentName` as the
bone / weapon attach point.

## Provider interface

[`IFollowTargetProvider`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:106) gained a
transform method so attach mode gets orientation:

```cpp
virtual bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang); // true = orientation available
```

| Provider | File | Notes |
|---|---|---|
| `PlayerTargetProvider` | [FollowCamera.cpp:177](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:177) | Player pawn; eye origin/angles give an orientation. |
| `EntityTargetProvider` | [FollowCamera.cpp:222](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:222) | Dropped weapon / grenade / C4 by handle; render angles when reachable. |
| `AttachmentTargetProvider` | [FollowCamera.cpp:253](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:253) | `LookupAttachment(name)` + `GetAttachment(idx, origin, quat)`; quat→angles via `FollowQuatToAngles`. Attachment-points only — no bone-matrix work. |
| `HeldWeaponProvider` | [FollowCamera.cpp:290](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:290) | Resolves `pawn->GetActiveWeaponHandle()` each frame, so it tracks the *currently held* weapon across switches. |

[`MakeProvider`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:804) chooses the
provider from `(type, mode, weaponSource)`. [`Candidates`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:553)
builds the selectable list (players / grenades / held + dropped weapons + C4).

## RunFrame: two paths

[`RunFrame`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:846) (called from
`Filmmaker::RunMainThreadFrame` only while the camera editor is active):

- **Lock-on** — the original turret solve (`FollowLookAt` + `FollowSmoothAngles`); the
  placed camera stays put and `rotationOffset` is added as an aim-trim after the look-at
  ([FollowCamera.cpp:1074](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1074)).
- **Attach** — `provider->GetWorldTransform()` gives `basePos,baseAng`;
  `finalPos = basePos + FollowRotateVector(offset, baseAng)` (local-space offset);
  `finalAng = baseAng + rotationOffset`; `FollowSmoothPosition/Angles` still apply (near-zero
  = rigid) ([FollowCamera.cpp:987](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:987)).

Both paths push the final pose through `CameraBridge_SetCameraPose` and cache it for the
`campose` instrumentation readout.

## Data flow

```
                 console command (human or netcon bot)
                              │
   mirv_filmmaker follow …    ▼
        ┌──────────────────────────────────┐
        │ FollowCommand.cpp  Follow_RunCommand() │  (routed from FilmmakerCommand.cpp)
        └──────────────────────────────────┘
                              │ setters / actions
                              ▼
        ┌──────────────────────────────────┐
        │ FollowCamera (singleton)          │
        │  RunFrame() ── pose ──► CameraBridge_SetCameraPose
        │  BuildStateJson()                 │
        └──────────────────────────────────┘
                              │ state JSON (every frame)
                              ▼
   CameraEditorHud::BuildStateJson() embeds it as  "follow": { … }
        (CameraEditorHud.cpp:196) ──► SetAttributeString(root,"state",…) (CameraEditorHud.cpp:266)
                              │
                              ▼
        ┌──────────────────────────────────┐
        │ CameraEditorJs render()           │  reads st.follow, updates controls
        │  buttons / sliders ──────────────────► issue mirv_filmmaker follow … (loop back)
        └──────────────────────────────────┘
```

The demo event list flows separately: `FollowCamera::EnsureEventsLoaded`
([FollowCamera.cpp:1290](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1290)) kicks a
detached worker that calls `ReadDemoInfoViaHelper` (which reads/refreshes
`<demo>.dem.fmjson`), keyed on the engine's `GetDemoFilePath()`. The events are merged into
the state JSON only for the Weapon type, nearest-the-playhead first
([FollowCamera.cpp:1232](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1232)).

## File map (every touched area)

| File | Lines | What |
|---|---|---|
| [FollowCamera.h](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h) | 17/26/33 enums · 69 `FollowEventRecord` · 77 `FollowCameraState` · 106 provider iface · 124 class | model, state, interface, instrumentation decls |
| [FollowCamera.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp) | 177/222/253/290 providers · 493 `SetMode` · 499 `SetWeaponSource` · 553 `Candidates` · 804 `MakeProvider` · 846 `RunFrame` · 987 attach path · 1074 lock-on trim · 1106 `SetOffsetAxis` · 1112 `SetRotationOffset` · 1149 `BuildStateJson` · 1278 `PrintCamPose` · 1290 `EnsureEventsLoaded` · 1333 `SelectEvent` · 1346 `PreviewTick` | all logic |
| [FollowCameraMath.h](../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h) / [.cpp](../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.cpp) | `FollowQuatToAngles` (.cpp:39), `FollowRotateVector` (.cpp:54) | new math helpers |
| [FollowCommand.cpp](../../AfxHookSource2/Filmmaker/FollowCommand.cpp) | `Follow_RunCommand` (whole file; routed from `FilmmakerCommand.cpp`) | command surface |
| [CameraEditorJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h) | follow section 259–487 · mode toggle 284 · `FOLLOW_TYPES` 298 · attach-pt 395 · events/Preview-Tick 425 · advanced 496 · `render()` follow block 711–790 | UI |
| [CameraEditorHud.cpp](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorHud.cpp) | `BuildStateJson` 159, embeds follow 196, push 266 | state bridge |
| [DemoEntry.h](../../AfxHookSource2/Filmmaker/Demo/DemoEntry.h) | `DemoEvent` 38 | event record |
| [DemoInfoHelper.cpp](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp) | `kSchemaVersion=6` 20, parse `events` 229 | reader |
| [main.go](../../FilmmakerDemoInfoGo/main.go) | `schemaVersion=6` 35, `addEvent` 182, event handlers 231–307, emit 374 | writer |

> Raw-string note: the embedded Panorama JS in `CameraEditorJs.h` is split into adjacent
> `R"EDJS(…)EDJS"` literals because MSVC caps a single string literal at ~16 KB (error
> C2026). Keep each chunk under that — add another `)EDJS"` / `R"EDJS(` split at a line
> boundary if a chunk grows too large.

