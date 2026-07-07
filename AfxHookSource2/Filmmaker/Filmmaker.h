#pragma once

// Public entry points for the filmmaker demo-browser feature.
// This is the single integration surface the rest of AfxHookSource2 touches.

#include <string>
#include <cstddef>

namespace Filmmaker {

class DemoLibrary;
class FilmmakerMenu;

// Lazily initialized; safe to call repeatedly.
void EnsureInitialized();

// Called once per rendered frame from the Present hook (RENDER thread).
// Pumps thread-safe backend work only (start scans, apply picked folders).
// Must NOT touch Panorama (V8 lives on the main thread).
void RunFrame();

// Called once per frame from FrameStageNotify (MAIN/UI thread). Drives the
// Panorama menu (RunScript / panel IO), which must run on the main thread.
void RunMainThreadFrame();

// Releases the worker thread on shutdown.
void Shutdown();

// Actions (callable from the console thread or the UI command queue).
void RequestRescan();
void RequestAddFolder();
void Watch(std::size_t index);

DemoLibrary& Library();
FilmmakerMenu& Menu();

// Absolute path of the demo most recently launched via OUR Watch(), or empty if a
// demo was started another way (e.g. console playdemo or the native Your Matches tab).
// Returns a copy (thread-safe). Prefer PlayingDemoPath() for the camera-path sidecar.
std::wstring CurrentDemoPath();

// Canonical absolute path of the demo the ENGINE is actually playing, irrespective of
// how it started (our Downloaded tab, native Your Matches, or console playdemo). The
// camera-marker system keys its per-demo sidecar off this so the same .dem always maps
// to the same path. Falls back to CurrentDemoPath() (canonicalized) when the engine path
// can't be recovered, so behavior is never worse than Watch()-only. Cheap to call per frame.
std::wstring PlayingDemoPath();

// --- Movie director (camera modes + in-game help HUD) ---
// Input taps, called from the WndProc / input thread. Return true if the event
// was consumed (the caller should then swallow it). Safe no-ops if unused.
bool MovieInput_OnKey(int vkey, bool down);
bool MovieInput_OnMouseButton(int button, bool down); // 0 = left, 1 = right
bool MovieInput_OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x = -1, int y = -1);

// HUD panel show/hide (driven by the mirv_filmmaker hud console command).
void MovieHud_Set(bool visible);
void MovieHud_Toggle();
bool MovieHud_Visible();

// Queues Panorama JS to run in the HUD-panel context (mirv_filmmaker hud_eval).
void MovieHud_Eval(const std::string& js);

// Native demo-bar inline speed buttons (mirv_filmmaker speedbar). When off, the
// native timescale dropdown is restored.
void DemoSpeedBar_Set(bool enabled);
void DemoSpeedBar_Toggle();
bool DemoSpeedBar_Enabled();

// True while the BO2-style camera-marker SETTINGS menu is open (Editing mode).
// main.cpp ORs this into GetSuspendMirvInput() so the OS cursor is shown and
// free-cam mouse-look is suspended while the menu is up (exactly like the
// console-open case), making the menu's Panorama buttons clickable. Free cam
// itself stays enabled, so the camera holds its pose and look resumes when the
// menu closes. Cheap; safe to call every frame from the input/render path.
bool MarkerMenu_WantsCursor();

// True while filmmaker cursor mode owns the mouse. The camera timeline / curve
// editor forces this on while open; otherwise the regular native-bar/G cursor
// toggle controls it in third-person/freecam.
bool CameraTimeline_WantsCursor();

// True while the camera timeline panel is open (any cursor mode). Read by the
// input layer to decide whether G should toggle the panel's UI-mouse mode.
bool CameraTimeline_Visible();

// True while the offline mvm_test FX menu is open in UI-cursor mode. main.cpp's
// CCSGOInput::MouseInputEnabled detour returns false while this holds so a LIVE match
// releases the mouse to the cursor (see the .cpp comment for why suspend alone can't).
bool LiveFxMenu_WantsGameMouseReleased();

// True while the camera PATH is actively driving the final view (dolly playing + free cam
// engaged). main.cpp's view-setup hook uses this to block CS2's demo-view-override
// (TrueView) from re-owning the view each frame the demo advances. CampathDebug() gates the
// verbose [campath]/[setupview] instrumentation (toggle via "camtl debug 0|1").
bool CameraPathOwnsView();
bool CampathDebug();

// --- Camera Editor Mode (dedicated editor workspace) ---
// Toggleable workspace that frames the live game as a preview and surrounds it with the
// camera inspector + timeline. Driven by "mirv_filmmaker editor on|off|toggle".
void CameraEditor_Set(bool enabled);
void CameraEditor_Toggle();
// True while Camera Editor Mode is active. Read by the input layer so G keeps toggling
// the UI/GAME mouse (to fly the free cam) even though the timeline panel is open.
bool CameraEditor_Active();
// Switch Camera Editor between UI cursor ownership and game/free-cam mouse look.
// Also pauses/resumes graph-editor view driving so mouse look is not overwritten.
void CameraEditor_SetCursorMode(bool uiCursor);

// TRUE scaled preview viewport (render-layer): scales the whole rendered frame into the
// preview rect instead of showing a crop. Driven by "mirv_filmmaker editor scale on|off|
// toggle"; only takes visible effect while the editor is open and not recording.
void CameraEditor_SetScale(bool enabled);
void CameraEditor_ToggleScale();
bool CameraEditor_ScaleActive();

// Bottom editor selection. "native" leaves CS2's own demo timeline visible; "timeline" opens
// the custom camera timeline; "graph" opens the graph editor.
void CameraEditor_SetUseTimeline(bool useTimeline);
void CameraEditor_ToggleUseTimeline();
void CameraEditor_SetNativeTimeline();

// Game-HUD visibility while the editor is open ("hidden" | "game" | "full"). Read by
// CameraTimelineHud's state push so its JS shows/hides the native gameplay HUD accordingly.
const char* CameraEditor_HudViewName();

// Scaled-preview rect (normalised window fractions) the world blit uses; returns true when the
// scaled viewport is live. The timeline HUD scales the native game HUD into this same rect.
bool CameraEditor_ScaledHud(float& x0, float& y0, float& x1, float& y1);

// True while the Camera Editor's "Customize Player" modal is open (custOverlay.visible, published
// JS -> C++ via the "customizeopen" panel attribute, same pattern as "previewrect"). The modal is
// meant to be a full-screen EXCLUSIVE surface: MovieMode's key/mouse/wheel handlers swallow input
// unconditionally while this is true, and main.cpp's GetSuspendMirvInput() ORs it in so free-cam
// mouse-look can't compete with dragging/scrolling inside the modal.
bool CameraEditor_CustomizeModalOpen();

// Route captured input into the Customize modal's Panorama JS. In the in-game HUD context CS2 keeps
// keyboard/wheel priority (a Panorama TextEntry never gets real focus, and the wheel still drives
// the demo spectator), so the modal cannot rely on native input the way a main-menu popup can.
// Instead MovieMode/main.cpp SWALLOW the raw wheel + typed characters from the game while the modal
// is open and forward them here; CameraEditorHud drains these on the main thread and dispatches them
// to $.CamEditor.custWheel/custChars so the modal scrolls + its search boxes type -- with nothing
// leaking to the demo behind it. Thread-safe (called from the WndProc/input thread).
void CameraEditor_CustomizePushWheel(int notches, int x, int y); // +1 = wheel up, -1 = wheel down; client coords
void CameraEditor_CustomizePushChar(unsigned charCode); // WM_CHAR code unit (incl. 8 backspace, 27 esc)

// --- Experimental After-Effects-style graph editor (isolated, opt-in overlay) ---
// Default OFF; toggled by the "Experiment" button or "mirv_filmmaker grapheditor on|off|
// toggle". OwnsView()/WantsCursor() are OR'd into the stable view-ownership + cursor-routing
// hooks so the experiment can live-drive the camera and stay clickable while open. Deleting
// the GraphEditor* files + reverting these hooks removes the feature entirely.
void GraphEditorExperiment_Set(bool enabled);
void GraphEditorExperiment_Toggle();
bool GraphEditorExperiment_Enabled();
bool GraphEditorExperiment_OwnsView();
bool GraphEditorExperiment_WantsCursor();

// --- mvm_test offline live-match FX testing ---
bool MvmTest_Active();
bool MvmTest_CanUseMenu();
bool TestHud_Enabled();

// True when non-preview UI should be hidden: always during full path playback,
// and while the armed preview has its background HUD toggled off with Tab.
bool CameraPath_PreviewHudHidden();

} // namespace Filmmaker
