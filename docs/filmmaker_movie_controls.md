# Filmmaker — Movie Director controls reference

Everything below is **only active while a GOTV/POV demo is actually playing**. In
the main menu and live matches the director taps nothing — your normal mouse/keys
work as usual.

## Help / status HUD

A small panel on the middle-right of the screen showing the live camera state and
this control list.

| Action | Control |
| --- | --- |
| Show / hide the HUD | **F8** |
| Show / hide the HUD (console) | `mirv_filmmaker hud [on\|off\|toggle]` |
| Bind it to your own key | `bind "h" "mirv_filmmaker hud toggle"` |

The HUD is **display-only** — it never blocks clicks, even where it's drawn.

## Camera modes (mouse wheel)

Plain scroll cycles through three camera modes and **wraps around** at the ends:

```
  Scroll Up ▶   First person → Third person → Free cam → (back to First) ...
  ◀ Scroll Down First person → Free cam → Third person → (back to First) ...
```

| Action | Control | Under the hood |
| --- | --- | --- |
| Cycle mode up | **Scroll Up** | first-person / native third-person orbit / free-cam |
| Cycle mode down | **Scroll Down** | same, reversed |

## Playback (in a demo)

| Action | Control | Under the hood |
| --- | --- | --- |
| Pause / resume | **Space** | `demo_pause` / `demo_resume` (reads live pause state) |
| Skip back 15s | **Left arrow** | `demo_gototick` (current tick − 15s) |
| Skip forward 15s | **Right arrow** | `demo_gototick` (current tick + 15s) |

The native demo-bar timescale dropdown is replaced by inline speed buttons
(0.1x–4x). Toggle that off to restore the native dropdown:
`mirv_filmmaker speedbar [on|off|toggle]`.

> First person = `spec_mode 2` (in-eye), Third person = the HLAE-owned orbit camera
> (`ThirdPersonCamera.cpp`, solved in the view-setup hook — CS2's native
> `thirdperson`/chase cvars are NOT used; they fight the demo view pass and jitter),
> Free cam = HLAE free camera. `spec_mode 4` is CS2's native roaming. Numbers are
> centralised in `MovieMode.cpp` — confirm in-game and adjust if needed.

## Free-cam controls (only when in Free cam mode)

These are HLAE's `mirv_input` camera keys; the director just turns the camera on
when you scroll into Free cam. They do nothing in first/third person.

| Action | Key(s) |
| --- | --- |
| Move forward / back | **W / S**  (or Numpad 8 / 2) |
| Strafe left / right | **A / D**  (or Numpad 4 / 6) |
| Move up / down | **R / F**  (or Numpad 9 / 3) |
| Roll left / right | **X / Z**  (or Numpad . / 0) |
| Pitch (look up / down) | **Arrow Up / Down** |
| Yaw (look left / right) | **Arrow Left / Right** |
| FOV in / out | **Numpad 1 / 7**  (or PageDown / PageUp) |
| Reset view + speed | **Home**  (or Numpad 5) |
| Look around | **Mouse** — always-look (point the camera, it stays). Sensitivity is **independent of cam speed**. |
| Slow / fine movement (hold) | **Shift** — applies a 25% slow factor to all free-cam movement + rotation while held |
| Cam speed up / down | **Shift + Scroll** |
| Exit camera control | **Esc** |

> **Shift-scroll changes third-person distance** and free-cam speed. Shift-hold slow
> movement only affects the free camera.

## Spectator (first / third person)

| Action | Control | Notes |
| --- | --- | --- |
| Next player | **Left click** | CS2's own spectator bind — **first person only**; in Third person clicks are consumed so orbiting can't switch players |
| Previous player | **Right click** | CS2's own spectator bind — first person only (see above) |
| Orbit current player | **Mouse move in Third person** | HLAE-owned orbit camera locked to the spectated player's eye; works identically while the demo is paused. |
| Third-person distance | **Shift + Scroll** | Clamped to 30–200 units. |
| Toggle X-ray | **X** | `spec_show_xray` — **first/third person only**. In free cam, X is camera roll instead. |

Third-person commands:

```text
mirv_filmmaker thirdperson on|off|toggle|state
mirv_filmmaker thirdperson preset front|back|left|right
mirv_filmmaker thirdperson angles <yaw> <pitch>
mirv_filmmaker thirdperson distance <30-200>
mirv_filmmaker thirdperson sens <deg per pixel, 0.005-0.5>   (default 0.05)
```

Presets and the orbit yaw are relative to the spectated player's facing: `back = 0`
(camera stays behind them as they turn), `front = 180`, `left = -90`, `right = 90`.
The orbit yaw is eased (~0.12 s) so player flicks swing the camera instead of snapping
it. The camera is solved by HLAE from the pawn's render-time eye pose — no engine
`cam_*` cvars are touched. While the camera is detached (third person OR free cam) the
first-person viewmodel is hidden (`r_drawviewmodel false`) and its `_fp/_fps` particle
systems are blocked to `dev/empty` — they anchor to the CAMERA and would otherwise
render as giant floating arms and mid-air muzzle flashes. The spectator is also held in
chase mode (`spec_mode 3`, re-asserted every 2 s) while detached: in-eye spawns ONLY the
`_fp` variants for the spectated player, so without chase there would be no weapon FX on
their gun at all (this is what makes the Modern/Povarehok swaps show in third person).
Everything restores automatically when the camera returns to first person. World collision (pull-in at walls) is not implemented yet:
the DLL has no engine trace hooked; adding one (Source 2 `GameTraceManager` AOB) is the
known follow-up.

## Native CS2 demo playback UI (pause / speed / highlights / round)

These buttons belong to CS2 itself, not this mod. Open them with the console:

```
demoui
```

The director **does not intercept clicks**, so this panel (and any other in-game
UI) is fully clickable. Bind it for convenience, e.g. `bind "p" "demoui"`.

## Console commands added

| Command | Purpose |
| --- | --- |
| `mirv_filmmaker hud [on\|off\|toggle]` | Show/hide the help+status HUD |
| `mirv_filmmaker thirdperson ...` | Third-person orbit camera controls and presets |
| `mirv_filmmaker speedbar [on\|off\|toggle]` | Inline demo-bar speed buttons (off = native dropdown) |
| `mirv_filmmaker hud_eval <panorama js>` | Run Panorama JS in the HUD context (debug) |
