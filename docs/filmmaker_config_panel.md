# Config panel (general settings, EFFECTS, MODIFIERS)

The lightweight **CONFIG** Panorama panel — the Camera Editor's "little sibling". It reuses
the editor's inspector look but carries no camera tools. Toggle with
`mirv_filmmaker config [on|off|toggle]`.

Implementation: `AfxHookSource2/Filmmaker/Panorama/ConfigHud.{h,cpp}` (bridge) +
`ConfigHudJs.h` (embedded Panorama JS). Unlike the Camera Editor it does not host the
timeline and does not hide the native HUD.

## Sections

### General (UI / display)
- The shared **"Game UI" visibility picker** — the same HudView state the Camera Editor
  uses (show/hide stays centralised in CameraTimelineHud's JS, honoured while either the
  editor is hosted or this panel is open).
- **Show UI Defaults** reset.
- The native demo-playback display toggles (X-Ray / True View / DOA / mismatch).

### EFFECTS — particle-effect modes
Per-category particle swaps (impacts, tracers, muzzle FX, blood, explosions, molotov, map
ambience) with **On / Less / Off** modes plus the Modern (MW2019) modes, backed by the
runtime particle-create hook in `Movie/ParticleFx*.cpp` and the converted asset packs in
`fx/`. Console equivalent: `mirv_filmmaker fx ...`.

Full documentation (modes, asset pipeline, hook internals, diagnostics):
[filmmaker_effects_modifiers.md](filmmaker_effects_modifiers.md).

### MODIFIERS — camera-feel + preset cams
All default OFF; sliders are an intensity percent (0–150).

| Modifier | What it does | Back-end |
|---|---|---|
| **Strafe roll** | Quake/Doom-style view tilt on lateral strafe. | `Movie/ViewFx.cpp` |
| **View bob** | GoldSrc-style vertical CAMERA bob on the walk cycle (a `V_CalcBob` port). | `Movie/ViewFx.cpp` |
| **Weapon sway** | Movement-scaled walk bob/drift added to the viewmodel offset. | `Movie/ViewFx.cpp` + `Movie/ViewFxVm.cpp` |
| **Aim deadzone** | Decoupled viewmodel: the recorded aim moves the weapon first inside a deadzone cone while the camera catches up with smoothing. | `Movie/ViewFx.cpp` + `Movie/ViewFxVm.cpp` |
| **Body Cam** | One-click chest cam on the spectated player — a preset over the Follow/Attach system (chest attachment + forward offset + wider FOV). | `Movie/BodyCam.cpp` |
| **Action Cam** | One-click head-mounted "GoPro" preset (head attachment, helmet-cam offset, wide FOV, smoothed handheld angles) + optional **fisheye** lens post-process. | `Movie/ActionCam.cpp` + `FisheyePass.cpp` / `afx_fisheye_ps` shader |

Console equivalents:

```
mirv_filmmaker viewfx roll|bob|sway|deadzone [<0-150>|off]
mirv_filmmaker bodycam [on|off|toggle]
mirv_filmmaker actioncam [on|off|toggle]
mirv_filmmaker actioncam fisheye [on|off|toggle]
```

Body Cam / Action Cam are **not separate camera modes** — they drive the same shared
FollowCamera state the Camera Editor's Follow inspector edits, so the two stay in sync
(hand-tuning a preset from the inspector works normally).

## Gotchas

- Fixed 2026-07-02 ([memory: viewfx-bodycam-config-modifiers]): roll/sway flicker,
  switch-player jerk, BodyCam-at-origin on dead pawns. If free-cam latch leaves the mouse
  stuck, `mirv_input end` unsticks it.
- The panel is Panorama: main-thread only, Slider-based dragging, never set `style = ''`
  (CLAUDE.md gotchas apply).
