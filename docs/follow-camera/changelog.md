# Follow / Attach Camera — Changelog

Running log of what the rebuild added/changed, with file:line references for review.

## Follow / Attach Camera rebuild

### Internal model & math
- New enums `FollowMode { LockOn, Attach }` and `WeaponSource { Auto, Held, Dropped }`,
  orthogonal to `FollowTargetType { Player, Grenade, Weapon }`
  ([FollowCamera.h:17](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:17), :26, :33).
- `FollowCameraState` gained `mode`, `rotationOffset`, `weaponSource`, `weaponPlayerIndex`;
  reuses `attachmentName` as the attach point
  ([FollowCamera.h:77](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:77)).
- `IFollowTargetProvider::GetWorldTransform(pos, ang)` added so attach mode gets orientation
  ([FollowCamera.h:114](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.h:114)).
- New math helpers `FollowQuatToAngles` and `FollowRotateVector`
  ([FollowCameraMath.cpp:39](../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.cpp:39), :54).

### Providers & solve
- `AttachmentTargetProvider` uses `GetAttachment(idx, origin, quat)` → angles
  ([FollowCamera.cpp:253](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:253)).
- New `HeldWeaponProvider` re-resolves `GetActiveWeaponHandle()` each frame, tracking the
  currently held weapon across switches
  ([FollowCamera.cpp:290](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:290)).
- `MakeProvider` selects by `(type, mode, weaponSource)`
  ([FollowCamera.cpp:804](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:804));
  `Candidates` merges held + dropped weapons + C4
  ([FollowCamera.cpp:553](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:553)).
- `RunFrame` split into lock-on (turret + aim-trim) and attach (local-offset ride) paths
  ([FollowCamera.cpp:846](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:846), attach :987, lock-on trim :1074).

### Commands ([FollowCommand.cpp Follow_RunCommand](../../AfxHookSource2/Filmmaker/FollowCommand.cpp))
- Added `follow mode`, `weaponsource`, `rotation` / `rotpitch|rotyaw|rotroll`,
  `offsetx|offsety|offsetz`, `fov`, `bone`, `eventselect`, `previewtick`, and the
  `campose` instrumentation dump. All prior commands + `state` / `state64` retained.

### UI ([CameraEditorJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h), follow section 259–487)
- Rebuilt into grouped blocks: Mode toggle, 5 Target-Type presets, Placement, Target
  dropdown, Weapon source, Attach point, Events + Preview Tick, Framing sliders
  (offset/rotation/FOV), collapsible Advanced Tracking.
- `render()` reads `mode`, `rotation`, `fov`, `weaponSource`, `events` and shows/hides
  blocks per type+mode ([CameraEditorJs.h:711](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h:711)).
- `BuildStateJson` extended with `mode`, `weaponSource`, `rotation`, `fov`, `eventStatus`,
  `selectedEvent`, and an `events[]` array
  ([FollowCamera.cpp:1149](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1149)).

### Demo event pre-scan (`.fmjson` events, v6)
- Go writer emits a top-level `events[]` (weapon/C4 drop + pickup ticks);
  `schemaVersion = 6` ([main.go:35](../../FilmmakerDemoInfoGo/main.go:35), `addEvent` :182,
  handlers :231–307).
- C++ `DemoEvent` struct + `events` parse; `kSchemaVersion = 6`
  ([DemoEntry.h:38](../../AfxHookSource2/Filmmaker/Demo/DemoEntry.h:38),
  [DemoInfoHelper.cpp:20](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp:20), parse :229).
- Lazy background load keyed on `GetDemoFilePath()`
  ([FollowCamera.cpp:1290](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1290)).

### Instrumentation & automation
- `follow campose` prints the computed pose + resolved target world pos
  ([FollowCamera.cpp:1278](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1278)).
- `FollowCameraMathTests` CTest target
  ([CMakeLists.txt:362](../../AfxHookSource2/CMakeLists.txt:362),
  driver [automation/tests/follow-camera-math-tests.cpp](../../automation/tests/follow-camera-math-tests.cpp)).
- Netcon verifiers under `automation/verify/` (`verify-followcam.ps1`,
  `verify-grenade-tracking.ps1`, …).

## Build-completion fixes (this pass)

This issue blocked a clean `build.bat`; it is fixed and the build is green.

1. **MSVC C2026 "string too big"** — the embedded Panorama JS raw-string literal in
   `CameraEditorJs.h` exceeded MSVC's ~16 KB single-literal cap after the follow-section
   rebuild. Split the oversized chunk into two adjacent `R"EDJS(…)EDJS"` literals at a line
   boundary (CameraEditorJs.h ~807); all chunks now < 16 KB.

**Verification:** `build.bat` → `BUILD OK` (HLAE x64 DLL + `FilmmakerDemoInfo` Go helper
built); `FollowCameraMathTests` → "all checks passed".

