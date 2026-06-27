# Live UI editing & game-window screenshots

How to change the Filmmaker Panorama UI **live in the running game** (no rebuild),
read panel state, and capture clean screenshots of just the game window.

This is the fast inner loop: tweak a value live → screenshot → iterate → once it
looks right, **bake the change into [`AfxHookSource2/Filmmaker/Panorama/FilmmakerGuiJs.h`](../../AfxHookSource2/Filmmaker/Panorama/FilmmakerGuiJs.h)**
and rebuild so it ships.

---

## 1. What this is

The Filmmaker UI is Panorama (CS2's HTML/CSS-like UI). The whole UI is injected
as one big JavaScript string (`kFilmmakerGuiJs`) baked into the DLL. You normally
have to rebuild to change it — slow.

Instead, while CS2 is running you can run **arbitrary Panorama JS** against the
live UI over the netcon console:

```
mirv_filmmaker ui_eval "<panorama js>"
```

This runs the JS in the UI context immediately. You can move panels, change
styles, add/remove classes, read layout sizes, etc. — and see it on screen at
once. Nothing persists across a relaunch; it is a scratchpad.

## 2. Prerequisites

1. **CS2 running with netcon** on port `29010`. Easiest:
   ```
   automation\launch\live.bat                 (relaunches CS2 + opens the dashboard)
   automation\launch\launch-cs2-netcon.ps1    (just CS2 + netcon, no dashboard)
   ```
2. A demo/map loaded enough that the Watch UI exists. `mirv_filmmaker ui` opens
   the demos page.

## 3. Sending commands

Any TCP client to `127.0.0.1:29010` works. The repo helper:

```powershell
pwsh automation\netcon\cs2-netcon.ps1 -Commands "mirv_filmmaker ui", "mirv_filmmaker ui_eval ""$.Msg('hi\n');"""
```

For an interactive loop, the small inline socket pattern used by the `_diag-*`
scripts is handy (connect once, send many). Minimal version:

```powershell
$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 800
  $buf=New-Object byte[] 8192; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
Send 'mirv_filmmaker ui_eval "$.Msg(`'pong\n`')"'
```

`$.Msg('...\n')` prints back over netcon, so it is how you return values to your
script (search the output for a tag like `[VERIFY] ...`).

## 4. The 256-byte limit (read this!)

**The netcon console truncates any single command at 256 bytes.** Measured: a
251-char command runs; 271 chars is cut off mid-string and throws a JS
SyntaxError. After the `mirv_filmmaker ui_eval "` prefix and closing quote you
get only **~230 chars of usable JS**. Longer payloads silently do nothing.

Two ways to live within it:

- **Keep each `ui_eval` short.** One probe / one tweak per command. Split work
  across several commands.
- **Put heavy logic in the injected script and call a tiny entry point.**
  `RunScript` (how the GUI JS is injected) has **no** length limit, so the real
  work lives in `FilmmakerGuiJs.h` behind short `$.Filmmaker.*` calls. These
  already exist (see the "automation hooks" block):

  | Call | What it does |
  |------|--------------|
  | `$.Filmmaker.gotoTab('downloaded'\|'yourmatches'\|'tournaments')` | Real sub-tab switch (toggles `WatchMenu--Hide` + runs the Downloaded takeover). Needed because native `mainmenu_watch.NavigateToTab` is a module function not in our JS scope, and a synthetic `Activated` on the tab button only highlights it. |
  | `$.Filmmaker.report()` | Prints `[VERIFY] dlHide=.. ymHide=.. tiles=.. styledTile=.. scoreboard=.. playerRows=..` |
  | `$.Filmmaker.tagFirst(n)` / `.stab()` | Tag/select for list-stability checks; `[STAB] ...` |
  | `$.Filmmaker.btnReport()` | `[BTN] ref=.. add=.. sameRow=..` for the navbar buttons |
  | `$.Filmmaker.render()` / `.show()` | Re-render / open the demos page |

  If you need a new long probe, add a method here and rebuild once, rather than
  fighting the 256-byte limit forever.

## 5. Reading & changing panels

Our UI context panel is in the HUD/menu tree, not inside the Watch page, so walk
to the root first, then `FindChildTraverse` down:

```js
var r=$.GetContextPanel(); while(r.GetParent()) r=r.GetParent();
var w=r.FindChildTraverse('JsWatchContent');   // the Watch page
```

Useful APIs:

- `panel.FindChildTraverse('id')` — find a descendant by id.
- `panel.BHasClass('x')`, `AddClass`, `RemoveClass`, `SetHasClass('x', cond)`.
- `panel.style.height='40px'` — set a style (see gotchas below).
- `panel.actuallayoutwidth` / `actuallayoutheight` — the **laid-out** size in px
  (great for diagnosing "is this thing 0px / off-screen?").
- `panel.visible`, `panel.checked` (RadioButton), `panel.GetChildCount()`,
  `GetChild(i)`.

Example — read the navbar button row height live:
```
mirv_filmmaker ui_eval "var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();var b=r.FindChildTraverse('FmNavBtns');$.Msg('[h] '+b.actuallayoutheight+'\n');"
```

Example — nudge a style live, then screenshot, then decide:
```
mirv_filmmaker ui_eval "var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();r.FindChildTraverse('FmRefreshBtn').style.marginLeft='5px';"
```

## 6. The workflow (tweak → bake)

1. Open the UI: `mirv_filmmaker ui`.
2. Probe / change live with short `ui_eval`s until it looks right.
3. Screenshot to confirm (section 7).
4. **Bake** the final values into `FilmmakerGuiJs.h` (the same JS, but now in the
   injected source).
5. `build.bat`, then relaunch CS2 (`live.bat`) to load the new DLL.
6. Verify on the fresh DLL.

Live edits never persist — step 4 is what makes the change real.

## 7. Screenshots — capture just the game window

A full-monitor grab makes the game a small slice of a huge image; UI detail
(tile colors, icons, text) is unreadable. Use the window capture instead:

```powershell
# Just the game's render area (no title bar / borders / desktop) -- best for reading UI:
pwsh automation\capture\capture-game-window.ps1 -Out shot.png

# The whole OS window incl. title bar:
pwsh automation\capture\capture-game-window.ps1 -Out shot.png -Mode window
```

- `-Mode client` (default) crops to CS2's client area at full resolution
  (e.g. 1600x1200) — tight and crisp.
- It auto-restores the window if minimized and is DPI-aware.
- Falls back target is the `cs2` process; override with `-Process <name>`.

Use [`capture-main-monitor.ps1`](../capture/capture-main-monitor.ps1) only when you
deliberately need the desktop too (e.g. the netcon console window beside the
game). For "let me see the UI", always prefer `capture-game-window.ps1`.

## 8. Gotchas

- **Single quotes only inside the JS.** The payload is wrapped in double quotes
  for the console, so a `"` inside ends the argument early and truncates your
  script. Use `'...'` everywhere in the JS.
- **Never assign `style.x = ''`.** An empty style string throws and aborts the
  script. Clear a color with `rgba(0,0,0,0)`, not `''`.
- **`height: 100%` can collapse to 0.** If a parent sizes to fit its children, a
  child asking for `100%` of it resolves to 0 (a circular dependency). Use an
  explicit px height. (This was the "navbar buttons missing" bug.)
- **Win/loss tile color depends on the logged-in account.** A tile is only
  colored when your *currently logged-in* Steam account is a player in that demo.
  Demos you aren't in (e.g. recorded on a different account) stay grey — that is
  expected, not a bug.
- **`$` context ≠ the Watch page.** Always walk to root before `FindChildTraverse`.

## 9. Related files

- [`cs2-netcon.ps1`](../netcon/cs2-netcon.ps1) — send commands, print output.
- [`cs2-live.ps1`](../launch/cs2-live.ps1) / [`live.bat`](../launch/live.bat) — browser dashboard + launcher.
- [`capture-game-window.ps1`](../capture/capture-game-window.ps1) — game-window screenshot (this guide, section 7).
- [`verify-watch-ui.ps1`](../verify/verify-watch-ui.ps1) — full example: drives `$.Filmmaker.*`, asserts, screenshots each step.
- [`FilmmakerGuiJs.h`](../../AfxHookSource2/Filmmaker/Panorama/FilmmakerGuiJs.h) — where live edits get baked in.
