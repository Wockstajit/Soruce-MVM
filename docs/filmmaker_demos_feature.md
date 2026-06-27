# Filmmaker "Demos" feature

A demo browser built into AfxHookSource2 (CS2). It scans for demos, reads each
demo's map/duration/date and (for downloaded matchmaking demos) the full
scoreboard, and can play any of them. A native Panorama UI (film-icon button in
the main-menu top navbar opening a "Watch → Downloaded"-style page) is authored
on top.

All code lives under `AfxHookSource2/Filmmaker/`, one component per file:

```
Filmmaker/
  Filmmaker.{h,cpp}          facade: init, per-frame pump, watch (playdemo)
  FilmmakerCommand.cpp       the mirv_filmmaker console command
  Demo/
    DemoEntry.h              data structs (demo + scoreboard player)
    DemoHeaderReader.{h,cpp} parse .dem header -> map + duration  (validated)
    DemoInfoReader.{h,cpp}   parse .dem.info -> scoreboard         (validated)
    DemoScanner.{h,cpp}      recursive *.dem discovery
    DemoLibrary.{h,cpp}      owns the list, runs the background scan, builds JSON
  Config/
    DemoFolderStore.{h,cpp}  saved folder list (%APPDATA%\HLAE\filmmaker_demo_folders.txt)
  Platform/
    ProtobufWire.{h,cpp}     minimal protobuf wire reader (no protobuf dependency)
    TextEncoding.{h,cpp}     UTF-8 <-> UTF-16
    JsonBuilder.{h,cpp}      tiny JSON writer (demo payload for the UI)
    FolderPicker.{h,cpp}     native IFileOpenDialog folder picker (worker thread)
  Panorama/
    PanoramaBridge.{h,cpp}   engine access + RunScript (guarded)
    FilmmakerGuiJs.h         the embedded Panorama JS (the actual UI)
    FilmmakerMenu.{h,cpp}    builds the UI, pushes data, polls the command queue
```

## What works now (console-driven, validated)

The demo discovery + parsing + playback is complete and verified against real
CS2 match files. In the HLAE console:

```
mirv_filmmaker scan          (re)scan the CS2 install + your folders
mirv_filmmaker list          list demos with index, map, length, scoreboard, file
mirv_filmmaker folders       show scanned folders
mirv_filmmaker addfolder     open a native folder picker; the folder is saved
mirv_filmmaker removefolder <i>
mirv_filmmaker watch <i>     play the demo at index i (playdemo)
```

- Demos are found by recursively scanning the auto-detected install root
  (`GetProcessFolderW()`) plus any saved folders, on a background thread.
- Map + duration come from the `.dem` header; date from the file timestamp.
- The full per-player K/A/D + score scoreboard and team scores come from the
  `<demo>.dem.info` sidecar (matchmaking downloads). Names are `[unknown]`
  exactly as CS2's own Downloaded page shows, because names are not in that file.
- Saved folders persist across restarts; missing/moved folders are skipped.

### Validation

The header + info parsers were validated by compiling them standalone and
running them against real files in `…\csgo\replays\`. Example
(`match730_…140528`): map `de_ancient`, 41:02, team scores 13–11, 10 players,
player 0 = `31 K / 5 A / 15 D` — matching the in-game scoreboard.

## Panorama UI status

`FilmmakerGuiJs.h` authors the UI (film navbar button + the demos page +
scoreboard cards + Watch/Add-folder/Close buttons, Valve CSS classes). The C++
bridge (`PanoramaBridge`) reuses the engine pointer + makeSymbol +
get/setAttributeString already resolved in `DeathMsg.cpp`, plus:

- **`CUIEngine::RunScript`** — now resolved directly from its prologue in
  `panorama.dll` via the pattern
  `48 89 5C 24 ? 4C 89 4C 24 ? 48 89 54 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D`
  (from current Osiris `PanoramaUiEnginePatternsWindows.h`; verified as a unique
  match in this build with `misc/sigscan.py`). The match address is the function
  itself, called as
  `RunScript(uiEngine, contextPanel, source, "filmmaker", line)` with a non-zero
  `line` to skip script caching.
- **Context panel** — the in-game HUD root panel
  (`AfxHookSource2_GetPanoramaHudPanel`) is used as the build context. So the UI
  **auto-builds once a map/demo is loaded** (the HUD panel is null in the bare
  main menu). After the page is created it is parented under the main-menu
  content and the bridge context is re-pinned to that persistent page, so later
  updates no longer depend on the transient HUD panel.

The JS walks to the top-level window and `FindChildTraverse`s for the main-menu
navbar (`MainMenuNavBarSettings`/…) to attach the film button, so it works even
when the script runs from the HUD context.

**Threading (critical):** `RunScript` executes Panorama's V8 JavaScript, which
must run on the game's MAIN/UI thread. Calling it from the DirectX Present
(render) thread crashes with `v8::Context::Exit() - Cannot exit non-entered
context`. So all Panorama work runs from `Filmmaker::RunMainThreadFrame()`,
called out of `CS2_Client_FrameStageNotify` (`FRAME_RENDER_PASS`) in `main.cpp`.
Only the thread-safe backend pump (scan start, folder apply) runs on the render
thread via `Filmmaker::RunFrame()` in the Present hook.

Console controls:

```
mirv_filmmaker ui_status     show engine / RunScript / context / built
mirv_filmmaker ui_rebuild    rebuild the UI next frame
mirv_filmmaker ui_context <hex>   (advanced) pin a specific context CUIPanel*
```

If the navbar anchor id ever changes across a CS2 update, the film button won't
attach — re-validate the `RunScript` pattern with `misc/sigscan.py` and adjust
the id list in `FilmmakerGuiJs.h`. This remains offline/demo use only.

## Build / test

1. `build.bat` (or `cmake --build --preset x64-release --target AfxHookSource2`).
2. Stage: `cmake --install build/x64-release --config Release --prefix build/staging-release`.
3. Launch CS2 through HLAE (`build/staging-release/bin/HLAE.exe` → Launch CS2).
4. In the CS2 console: `mirv_filmmaker scan`, then `mirv_filmmaker list`, then
   `mirv_filmmaker watch 0`.
