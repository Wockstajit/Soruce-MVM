# Follow / Attach Camera — Documentation

The **Follow / Attach Camera** is a tracking-and-ride-along camera tool in the CS2
filmmaker (the HLAE fork `AfxHookSource2`). It lets you pick a target in a demo, jump to
the right tick, preview the shot live, fine-tune offset / rotation / FOV, and play the
demo while the camera stays locked to — or rigidly rides — the target.

Two camera models, selectable per target:

- **Lock-on** — a camera is *placed* at a fixed point and only rotates to keep the target
  framed (a turret). `rotation` acts as an aim-trim on top of the look-at solve.
- **Attach** — the camera position **and** orientation are derived from the target every
  frame, with a local-space `offset`, a `rotation` trim, and an `fov` applied. Near-zero
  smoothing = a rigid mount; non-zero smoothing eases the ride.

Six UI presets over `(type, mode)`: **Player**, **Grenade**, **Weapon / C4**,
**Player Bone** (player + attach), **Weapon Attach** (weapon + attach). Plus a free
**Lock-on / Attach** toggle so any target can ride-along.

The whole feature is **100% console-command driven** — the Panorama UI only issues the
same `mirv_filmmaker follow ...` commands a human or an automation bot can send over the
net console. That makes it verifiable without the physical mouse (see the QA handoff).

## Where to start

| You want to… | Read |
|---|---|
| Understand the components + data flow | [follow-camera-architecture.md](follow-camera-architecture.md) |
| Drive the feature from the console (the automation surface) | [follow-camera-commands.md](follow-camera-commands.md) |
| Find a control in the editor UI / map state→control | [follow-camera-ui.md](follow-camera-ui.md) |
| Understand the `.fmjson` v6 weapon/C4 event pre-scan | [demo-event-prescan.md](demo-event-prescan.md) |
| See what changed, with file:line refs | [changelog.md](changelog.md) |
| Run the automated verification (no mouse) | [qa-automation-handoff.md](qa-automation-handoff.md) |

## Build

From the repo root:

```bat
build.bat
```

`build.bat` builds the win32 + x64 HLAE staging release to
`build\staging-release\bin\HLAE.exe` and builds the `FilmmakerDemoInfo` helper
(Go / demoinfocs-golang — the `.dem` → scoreboard/event JSON pre-scanner) next to
`AfxHookSource2.dll`. The
shader-build env-var fix is baked in. A standalone math test target,
`FollowCameraMathTests` (driver: [automation/tests/follow-camera-math-tests.cpp](../../automation/tests/follow-camera-math-tests.cpp)),
is built by CMake and registered with CTest.

> **Schema sync:** the demo pre-scan format is at **v6**. The Go writer
> (`FilmmakerDemoInfoGo/main.go`) and the C++ reader (`DemoInfoHelper`) must agree on the
> version — if you change one, change the other and bump both. See
> [demo-event-prescan.md](demo-event-prescan.md).

