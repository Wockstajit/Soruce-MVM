# SOURCE:MVM Project Context

This document contains project background for agents. Keep `CLAUDE.md` and `AGENTS.md` focused on instructions. Put durable project knowledge here.

## What this repo is

SOURCE:MVM is a fork of HLAE / advancedfx focused on building a native-feeling in-game cinematics and demo-replay toolkit for Counter-Strike 2, Source 2.

Active development targets CS2. The inherited GoldSource and Source 1 code is left intact but is not the focus.

Primary active areas:

- `AfxHookSource2/`, the CS2 x64 hook DLL.
- `AfxHookSource2/Filmmaker/`, the demo browser, camera tooling, UI, effects, cosmetics, and movie-making systems.

## Build and run

The project is Windows-only and builds with CMake plus Visual Studio 2022. A release build combines two architectures:

- Win32 for the GUI and injector.
- x64 for the CS2 hook.

The full release build is coordinated by `cmake/MultiBuild.cmake`.

### Normal full build

Use `build.bat` from the repo root for the day-to-day full build. It handles the staged release build, helper binaries, FX staging, and launch flow.

Important behavior:

1. Offers to recompile converted FX asset packs. The prompt defaults to No because conversion can take minutes.
2. Kills running `cs2.exe` and `hlae.exe` before rebuilding so locked staged DLLs and mounted FX assets are released.
3. Rebuilds FX packs first when requested, but continues even if conversion fails.
4. Sets needed tool paths for Cargo, gettext, and Go.
5. Runs the multi-build CMake script.
6. Builds the Go demo-info helper.
7. Stages the compiled `source_mvm_fx` game directory.
8. Launches CS2 and the live dashboard through the automation launch scripts.

`launch.bat` runs the already-staged `build/staging-release/bin/HLAE.exe` without rebuilding.

### Faster CS2 hook-only build

When iterating on the CS2 hook only, prefer:

```batch
cmake --preset x64-release
cmake --build --preset x64-release --parallel --target AfxHookSource2
cmake --install build/x64-release --config Release --prefix build/staging-release
```

Use Win32 presets for the C# GUI, injector, and old Win32 hooks. Use x64 presets for the CS2 hook.

### Build prerequisites

Required tools include:

- Visual Studio 2022 with Desktop C++ workload.
- Visual Studio .NET desktop development workload.
- .NET Framework 4.6.2 targeting pack.
- Node.js 24 LTS.
- Python 3.
- GNU gettext.
- Rust/Cargo with `i686-pc-windows-msvc` target.
- Go for the demo helper.

Rust builds locked by default. Use `-DAFX_RUST_CARGO_LOCKED=FALSE` only when intentionally updating `Cargo.lock`.

## Tests and verification

Most tests are live in-game verification harnesses because this is a game-hook DLL.

Useful locations:

- `automation/verify/*.ps1`, netcon-driven live verification scripts.
- `automation/launch/launch-cs2-netcon.ps1`, launches CS2 in a verification-friendly setup.
- `automation/tests/follow-camera-math-tests.cpp`, pure logic Follow Camera math tests.

Run Follow Camera math tests with:

```batch
cmake --build --preset x64-release --target FollowCameraMathTests
ctest --test-dir build/x64-release -C Release -R FollowCameraMathTests --output-on-failure
```

Generated logs and screenshots belong under ignored automation output paths, not in committed source folders.

## Main repo layout

- `hlae/`, C# .NET launcher GUI, shipped as Win32 `HLAE.exe`.
- `AfxHookSource2/`, C++ CS2 hook DLL. This is the main active code.
- `AfxHookSource/`, inherited Source 1 hook code.
- `AfxHookGoldSrc/`, inherited GoldSource hook code.
- `AfxCppCli/`, old C++/CLI code.
- `ShaderBuilder`, `shaders/`, `ShaderDisassembler`, shader build tooling.
- `shared/`, shared C++ code across hooks.
- `deps/`, submodules and CMake-pulled dependencies.
- `FilmmakerDemoInfoGo/`, Go demo parser helper that outputs scoreboard and event JSON.

## Critical threading model

`AfxHookSource2/main.cpp` installs detours. Two callbacks matter:

- `CS2_Client_FrameStageNotify`, main/UI thread. Calls `Filmmaker::RunMainThreadFrame()`.
- DirectX Present hook, render thread. Calls `Filmmaker::RunFrame()`.

Panorama executes V8 JavaScript and must run on the game main thread.

Do not call `RunScript` from the render thread. It can crash with `v8::Context::Exit() - Cannot exit non-entered context`.

Keep the split:

- Main/UI thread for Panorama and UI work.
- Render-thread pump only for thread-safe backend work.

## Filmmaker feature area

`AfxHookSource2/Filmmaker/Filmmaker.h` is the integration surface for the rest of the DLL. Read it before touching Filmmaker systems.

File organization rule:

- One responsibility per file.
- If a file gains a second unrelated job, split it.
- Large subsystems should use an `*Internal.h` shared-state header and focused `.cpp` files behind one public header.
- Embedded Panorama JS belongs in per-panel `*Js.h` fragments.
- Console features get their own `*Command.cpp` next to the dispatcher.

Important areas:

- `Filmmaker.{h,cpp}`, facade and per-frame integration.
- `FilmmakerCommand.cpp`, pure dispatcher for `mirv_filmmaker` commands.
- `Demo/`, demo discovery, header reading, sidecar parsing, library scanning.
- `Panorama/`, engine access, panel lookup, embedded JS, and HUD bridges.
- `Movie/`, camera paths, markers, playback, follow camera, effects, body/action cam, and ParticleFx.
- `Cosmetics/`, SteamID-keyed loadout, skins, knives, gloves, agents, and model swaps.
- `Data/`, generated skin catalog data.
- `Platform/`, JSON, protobuf wire helpers, text encoding, folder picker.
- `Config/`, persisted demo folder settings.

## Feature docs

Read the matching doc before changing a feature:

| Feature | Doc |
|---|---|
| Demo browser / Watch tab | `docs/filmmaker_demos_feature.md` |
| Movie director HUD + input | `docs/filmmaker_movie_controls.md` |
| Camera Editor workspace | `docs/camera-editor-feature.md` |
| Follow / Attach camera | `docs/follow-camera/` |
| Config panel | `docs/filmmaker_config_panel.md` |
| Particle effects and FX asset pipeline | `docs/filmmaker_effects_modifiers.md`, `fx/README.md`, `docs/mw2019-fx-mapping-reference.md`, `docs/povarehok-csgo-mod-reference.md` |
| Cosmetics | `docs/cosmetics-overview.md` |
| Panorama UI architecture | `docs/panorama_ui_guide.md`, `docs/panorama_viewmodel_preview.md`, `docs/panorama-player-preview-summary.md` |
| Superseded research | `docs/archive/` |

## Engine integration and signature scanning

There is no stable CS2 SDK for these internals. Engine functions and globals are resolved at runtime by byte-pattern scanning loaded modules.

Important notes:

- Panorama and main-menu panel accessors are resolved from signatures.
- Pattern resolution largely lives near `DeathMsg.cpp` helpers.
- `misc/sigscan.py` can validate whether a pattern uniquely matches a DLL.
- When a CS2 update breaks UI, suspect a moved pattern or renamed Panorama panel ID first.
- Pattern misses should warn and degrade gracefully instead of crashing.

## Demo scoreboard pipeline

`DemoLibrary::ScanWorker` runs in two phases:

1. Fast phase: reads `.dem` header and `.dem.info` sidecar for map, duration, account IDs, and K/A/D. Names are not available here.
2. Slow phase: shells out to `FilmmakerDemoInfo.exe` for names, sides, MVPs, per-round data, and weapon/C4 events.

Slow results are cached beside the demo as `<demo>.fmjson`.

Schema contract:

- `schemaVersion` in `FilmmakerDemoInfoGo/main.go` must match `kSchemaVersion` in `Demo/DemoInfoHelper.cpp`.
- Bump both when changing JSON shape.
- Cache invalidation depends on demo mtime, helper exe mtime, and schema version.

## Repo conventions

Do not dump files in the repo root.

- `docs/`, project documentation. Update the existing doc for a feature instead of creating a parallel doc.
- `docs/archive/`, superseded research with an archived banner explaining what replaced it.
- `automation/`, testing and tooling only. Launchers, verifiers, capture helpers, netcon tools, and automation tests belong here.
- `automation/runs/` and `automation/output/`, generated screenshots and logs. These are ignored and should not be committed.
- `fx/`, in-game particle FX packs and build/release asset pipeline.
- `fx/tools/`, converters and post-processors.
- `fx/sources/`, committed FX source inputs.
- `build/fx/`, generated compiled FX output. Ignored.
- `tools/`, repo build and release helper scripts.
- `misc/`, dev tools and odds-and-ends.
- `.gitignore`, update when a new generated or local artifact class appears.

## Gotchas

- Close CS2 before building because the staged DLL is locked while loaded.
- MSVC has a roughly 16380-byte string literal cap. Split embedded Panorama JS raw literals when needed.
- `Windows.h` defines `min` and `max` macros. Use manual comparisons in files that include it.
- Panorama UI has no mouse-move event. Build dragging UI from Slider panels instead of custom drag handling.
- Never set Panorama `style = ''`. Use transparent color values such as `rgba(0,0,0,0)`.
- Netcon `ui_eval` truncates at 256 bytes. Use short calls into predefined JS helpers.
- Demo scrubbing is slow because seeking replays full packets. Avoid redundant `SeekDemoTick` calls.

## CS2 and VAC boundary

This DLL is for offline demo and movie work only. Never connect it to VAC-protected or online servers.
