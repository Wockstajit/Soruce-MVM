# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**SOURCE:MVM** — a fork of [HLAE / advancedfx](https://github.com/advancedfx/advancedfx) focused on building a native-feeling, in-game cinematics / demo-replay toolkit for **Counter-Strike 2 (Source 2)**. All active development targets CS2; the inherited GoldSource (`AfxHookGoldSrc`) and Source 1 (`AfxHookSource`) code is left intact but is **not** being worked on here.

Almost all new work lives in **`AfxHookSource2/`** (the CS2 hook DLL) and, within it, **`AfxHookSource2/Filmmaker/`** (the demo browser + camera tooling). When in doubt, that is where the feature you are touching lives.

## Build & run

The project is Windows-only and builds with CMake + Visual Studio 2022. A release is **two architectures** (Win32 for the GUI/injector, x64 for the CS2 hook) puzzled together by `cmake/MultiBuild.cmake`.

### Primary workflow — `build.bat`

`build.bat` (repo root) is the day-to-day build. The user generally wants Claude to run it after making changes. It:
1. Kills any running `cs2.exe` / `hlae.exe` first — the staged `AfxHookSource2.dll` is loaded into `cs2.exe` while the game runs, so the install/copy step fails with "being used by another process" otherwise.
2. Puts Cargo, gettext, and Go on `PATH`, and clears `NoDefaultCurrentDirectoryInExePath` (a hardened-shell var that breaks the `ShaderBuilder.exe` step — see [memory: build-shader-9009-fix]).
3. Runs `cmake -DAFX_MULTIBUILD_STAGING=ON -P cmake/MultiBuild.cmake` (builds both arches → `build/staging-release/bin/HLAE.exe`).
4. Builds the Go demo-info helper (`go build` in `FilmmakerDemoInfoGo/` → `build/staging-release/bin/x64/FilmmakerDemoInfo/FilmmakerDemoInfo.exe`).
5. Offers to recompile the converted FX asset packs (`fx/tools/convert-povarehok-source1.ps1 -Compile` → `build/fx/povarehok-source1import/`, then stages the compiled `source_mvm_fx` game dir into `build/staging-release/fx/` so a shipped build carries it) via a 3-second Y/N prompt that **defaults to No** (the conversion takes minutes and the mod sources rarely change). One run converts both the "Povarehok" pack (On/Less modes, staged as `particles/filmmaker/povarehok/{regular,less/impacts,less/smoke}/`) and the MW2019 "modern" pack (Modern modes, staged as `particles/filmmaker/modern/`; its Source 1 inputs are committed in-repo under `fx-sources/modern-warfare-gmod/`, so **no GMod install is needed** — pass `-RefreshModernFromGmod` to re-extract them from a local GMod via `extract-modern-particles-gmod.py`). Forced automatically when no compiled pack exists on disk (fresh checkout / cancelled prior conversion). Non-fatal — a machine missing the converter checkouts (`misc/source1import`, `misc/Source2Converter`) still gets a working DLL/game build, just with whatever pack (if any) was already on disk.
6. Launches CS2 + the live dashboard via `automation/launch/live.bat`, which auto-mounts that pack over `USRLOCALCSGO` if present (see `automation/launch/launch-cs2-netcon.ps1`).

`launch.bat` just runs the already-staged `build/staging-release/bin/HLAE.exe` without rebuilding.

### Targeted dev builds (faster, single arch)

When iterating on the CS2 hook only, prefer the x64 preset over a full MultiBuild:

```batch
cmake --preset x64-release
cmake --build --preset x64-release --target AfxHookSource2
cmake --install build/x64-release --config Release --prefix build/staging-release
```

Presets: `win32-debug`, `x64-debug`, `win32-release`, `x64-release`. Use `win32-*` for the C# GUI / injector / Win32 hooks, `x64-*` for the CS2 hook. Build dirs are reused, so a one-file change only rebuilds affected targets. Full build options live in [BUILDING.md](BUILDING.md).

### Build prerequisites

VS 2022 (Desktop C++ **and** .NET desktop dev workloads + .NET Framework 4.6.2 targeting pack), Node.js 24 LTS, Python 3, GNU gettext, Rust/Cargo with the `i686-pc-windows-msvc` target added, and Go (for the demo helper). Rust is built `--locked` by default; pass `-DAFX_RUST_CARGO_LOCKED=FALSE` when intentionally updating `Cargo.lock`.

### Tests

This is a game-hook DLL, so most "tests" are **live in-game verification harnesses**: `automation/verify/*.ps1` drive a running CS2 over netcon (e.g. `automation/verify/verify-followcam.ps1`, `verify-watch-ui.ps1`). To exercise a feature, launch CS2 via `automation/launch/launch-cs2-netcon.ps1` (windowed 1600x1200) and run the matching verifier.

The one pure-logic unit test is the CTest target `FollowCameraMathTests` (`automation/tests/follow-camera-math-tests.cpp`, registered in `AfxHookSource2/CMakeLists.txt` via `add_test`). Build and run it standalone:

```batch
cmake --build --preset x64-release --target FollowCameraMathTests
ctest --test-dir build/x64-release -C Release -R FollowCameraMathTests --output-on-failure
```

## Architecture (the big picture)

### Multi-project layout

- **`hlae/`** — the C# .NET launcher GUI, shipped as the Win32 `HLAE.exe`. Injects the hook DLL into the game.
- **`AfxHookSource2/`** — the C++ **CS2 hook DLL** (x64). This is where the action is. It also statically links a Rust library (`lib/`, mostly mirv-script). Built with `add_definitions(-DGAME_CS2)`.
- **`AfxHookSource`** / **`AfxHookGoldSrc`** / **`AfxCppCli`** — Source 1 / GoldSource hooks and old C++/CLI code. Inherited from upstream, not in active development.
- **`ShaderBuilder` / `shaders/` / `ShaderDisassembler`** — HLSL shaders are compiled to `.acs` combo files by `ShaderBuilder.exe` at build time (see the `add_custom_command` in `AfxHookSource2/CMakeLists.txt`); `copy_resources_release.bat` stages the `.acs` outputs.
- **`shared/`** — C++ shared across hooks. **`deps/`** — git-submodule and CMake-pulled dependencies (Detours, prop/CS2 SDK, etc.); run `git clone --recurse-submodules` or `git submodule update --init`.
- **`FilmmakerDemoInfoGo/`** — standalone **Go** binary (`demoinfocs-golang`) that parses a `.dem` to scoreboard JSON. Self-contained, no .NET runtime needed.

### How the hook drives the game (critical threading model)

`AfxHookSource2/main.cpp` installs the detours. Two callbacks matter and run on **different threads**:

- **`CS2_Client_FrameStageNotify` (MAIN / UI thread)** → calls `Filmmaker::RunMainThreadFrame()`. **All Panorama work runs here.** Panorama executes V8 JavaScript, which *must* be on the game's main thread.
- **The DirectX Present hook (RENDER thread)** → calls `Filmmaker::RunFrame()`, which pumps only thread-safe backend work (start scans, apply picked folders).

**Calling `RunScript` (Panorama) from the render thread crashes** with `v8::Context::Exit() - Cannot exit non-entered context`. Keep this split intact: backend/IO on the render-thread pump, anything that touches the UI on the main-thread pump.

`main.cpp`'s WndProc hook routes raw input into `Filmmaker::MovieInput_OnKey/OnMouseButton/OnMouseWheel` (hotkeys for free-cam, markers, editor). `GetSuspendMirvInput()` is OR'd with editor/menu cursor-wants so the OS cursor shows and free-cam mouse-look pauses while a Panorama menu is up.

### The Filmmaker feature (`AfxHookSource2/Filmmaker/`)

Organized **one component per file**, by responsibility. `Filmmaker.h` is the single integration surface the rest of the DLL touches (well-documented — read it first).

- **`Filmmaker.{h,cpp}`** — facade: init, the two per-frame pumps, watch/playdemo, plus the public API for every camera/editor mode.
- **`FilmmakerCommand.cpp`** — the `mirv_filmmaker <subcommand>` console command. This is the **central command dispatcher**; nearly every feature is driven through it (`scan`, `watch`, `editor`, `camtl`, `marker`, `follow`, `grapheditor`, `cosmetics`, `ui_eval`, …). Adding a feature usually means adding a subcommand here.
- **`Demo/`** — discovery + parsing. `DemoScanner` finds `*.dem` recursively; `DemoHeaderReader` reads map/duration; `DemoInfoReader` reads the `.dem.info` matchmaking sidecar; `DemoLibrary` owns the list and runs the background scan.
- **`Panorama/`** — the UI. `PanoramaBridge` wraps engine access + `CUIEngine::RunScript`. The actual UI is embedded JS in `*Js.h` headers (`FilmmakerGuiJs.h`, `CameraEditorJs.h`, `CameraTimelineJs.h`, …). `*Hud.cpp` files are the C++ bridges that push state into and read events out of that JS.
- **`Movie/`** — camera systems: `CameraPath`/`CamMarkers` (BO2-style dolly), `FollowCamera`, `CamPlayback`, `MirvCosmetics` (loadout/skin override), `MovieMode`.
- **`Platform/`** — `JsonBuilder`/`JsonParser` (hand-rolled; the DLL does not link a JSON lib), `ProtobufWire` (minimal protobuf reader — the DLL does **not** link protobuf), `TextEncoding`, `FolderPicker` (native `IFileOpenDialog`).
- **`Config/`** — `DemoFolderStore` persists scanned folders under `%APPDATA%\HLAE\`.

### Engine integration via signature scanning

There is no CS2 SDK for these internals. Engine functions/globals (`CUIEngine::RunScript`, the main-menu panel global, Panorama accessors) are resolved at runtime by **byte-pattern scanning** the loaded modules (patterns largely cross-referenced from the Osiris project). Resolution lives in `DeathMsg.cpp` (`getPanoramaAddrs*`). `misc/sigscan.py` is a kept dev tool to validate a pattern is a unique match in a DLL. **When a CS2 update breaks the UI, a moved pattern or a renamed Panorama panel id is the first suspect** — re-validate with `sigscan.py` and check the id lists in the `*Js.h` files. Pattern misses are designed to be non-fatal (warn + fall back), so a broken pattern degrades a feature rather than crashing the game.

### Demo scoreboard pipeline (two-phase)

`DemoLibrary::ScanWorker` runs in two phases (see [memory: filmmaker-demo-parsing]):
1. **Phase 1 (instant):** `.dem` header (map + duration) + `.dem.info` sidecar (account ids + K/A/D, **no names**).
2. **Phase 2 (slow):** shells out to `FilmmakerDemoInfo.exe` per demo for real names, end-of-match sides, MVPs, `perRound`, and weapon/C4 `events[]` (the latter feed the Follow Camera). Results are cached beside the demo as `<demo>.fmjson`.

**Contract:** `schemaVersion` in `FilmmakerDemoInfoGo/main.go` **must equal** `kSchemaVersion` in `Demo/DemoInfoHelper.cpp` — bump **both** when changing the JSON shape, or stale caches get reused. The cache is invalidated against the demo mtime, the helper-exe mtime, and the schema version. (The old C# helper `FilmmakerDemoInfo/` and "names-fast" mode were removed; the Go helper always does a full parse.)

## Repository conventions (where files go)

Keep the repo tidy — **do not dump files in the root.** Before working in a folder, read its `README.md` if present (`automation/README.md`, `docs/follow-camera/README.md`, etc.) and follow it.

- **`docs/`** — project documentation. Feature write-ups live here (`docs/filmmaker_demos_feature.md`, `docs/panorama_ui_guide.md`, `docs/panorama_viewmodel_preview.md`, `docs/follow-camera/`). **Update the existing doc** for a feature rather than creating a parallel one; if a doc is contradicted by current code, fix it as part of the work.
- **`automation/`** — **testing/tooling only**: launchers, verifiers, capture helpers, and automation-only test drivers, each in its subfolder (`launch/`, `verify/`, `capture/`, `netcon/`, `tools/`, `tests/`, `config/`, `lib/`, `docs/`). **Generated screenshots/logs go under `automation/runs/<name>/<timestamp>/` or `automation/output/<feature>/` and are git-ignored** — never commit them. See `automation/docs/AUTOMATION_HYGIENE.md`. Note: the FX asset-conversion pipeline is **not** here — it's a build/release concern and lives in `fx/`.
- **`fx/`** — the in-game particle FX packs (a build/release concern, deliberately separate from test-only `automation/`). `fx/tools/` holds the converter (`convert-povarehok-source1.ps1`) and its helpers; `fx/sources/` holds committed source inputs (`modern-warfare-gmod/` — the MW2019 "Modern" pack's ~44 MB extracted Source 1 tree, so builds need no GMod install). The **compiled** output (~150 MB — the converter prunes the content tree to the runtime closure derived from `ParticleFx.cpp`'s swap tables before compiling, so ~85% of the mod's unreferenced assets never build) is a generated artifact under `build/fx/` (git-ignored) and is staged into `build/staging-release/fx/source_mvm_fx` so a shipped build carries it. The Povarehok pack's raw mod sources still live in the git-ignored `reference/csgo effect mod/`.
- **`tools/`** — repo build/release helper Python scripts (`make_credits.py`, `make_readme.py`) — release tooling, not the place for ad-hoc scripts.
- **`misc/`** — dev tools and odds-and-ends (`sigscan.py`, mirv-script).
- **`.gitignore`** — keep it updated when a new class of generated/local artifact appears. `build/`, `lib/**/target/`, `automation/runs|output/`, `reference/`, `.agents/`, and `.claude/` are already ignored.

## Gotchas worth knowing before you hit them

- **Close CS2 before building** — the staged DLL is locked while loaded. `build.bat` does this for you; a manual `cmake --install` will fail with "Access is denied" if CS2 is open.
- **MSVC ~16380-byte string-literal cap** — the embedded Panorama JS in `*Js.h` exceeds it and is split into adjacent raw literals (`)FMJS"` … `R"FMJS(`), which C++ concatenates. Add another split if a block grows past ~16 KB (error C2026 "string too big").
- **`<Windows.h>` defines `min`/`max` macros** — use manual comparisons, not `std::min`/`std::max`, in files that include it.
- **Panorama UI has no mouse-move event** — any dragging UI must be built from `Slider` panels, not custom drag handling (see [memory: panorama-ui-input-constraint]).
- **Panorama: never set `style = ''`** — an empty-string style throws and aborts the script; clear a color with `rgba(0,0,0,0)` (see [memory: panorama-style-empty-string]).
- **Netcon `ui_eval` truncates at 256 bytes** — drive the UI with short calls into pre-defined JS (`$.Filmmaker.gotoTab` etc.), not long inline JS strings (see [memory: netcon-256-and-ui-helpers]).
- **Demo scrubbing is inherently slow** — seeking replays full packets; this is an engine cost the tool cannot remove (see [memory: demo-seek-cost-model]). Avoid redundant `SeekDemoTick` calls.

## CS2 / VAC note

This DLL is a game hook. It is for **offline demo/movie work only**. Never wire it up to join VAC-protected or online servers.
