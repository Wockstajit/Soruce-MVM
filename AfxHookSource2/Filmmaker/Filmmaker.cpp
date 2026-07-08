#include "Filmmaker.h"

#include "Demo/DemoLibrary.h"
#include "Demo/PlayingDemoPath.h"
#include "Panorama/FilmmakerMenu.h"
#include "Panorama/MovieHud.h"
#include "Panorama/MarkerHud.h"
#include "Panorama/FxDebugHud.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/CameraEditorHud.h"
#include "Panorama/ConfigHud.h"
#include "Panorama/TestHud.h"
#include "Panorama/GraphEditorExperimentHud.h"
#include "Panorama/DemoBarButtons.h"
#include "Movie/CameraBridge.h"
#include "Movie/MovieMode.h"
#include "Movie/CameraPath.h"
#include "Movie/FollowCamera.h"
#include "Movie/ActionCam.h"
#include "Movie/BodyCam.h"
#include "Movie/ViewFxVm.h"
#include "Movie/ParticleFx.h"
#include "Movie/RagdollFx.h"
#include "Movie/DemoEndHold.h"
#include "Movie/ThirdPersonCamera.h"
#include "Cosmetics/CosmeticOverrideSystem.h"
#include "Platform/FolderPicker.h"
#include "Platform/TextEncoding.h"

#include "../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../shared/AfxConsole.h"

#include <Windows.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

// Provided by main.cpp.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

DemoLibrary g_library;
FilmmakerMenu g_menu;

std::once_flag g_initOnce;
std::atomic<bool> g_rescanRequested{ false };
std::atomic<bool> g_pickerOpen{ false };
std::atomic<bool> g_shuttingDown{ false }; // set by Shutdown(); picker thread checks before committing

std::mutex g_pendingMutex;
std::vector<std::wstring> g_pendingFolders;

std::mutex g_demoPathMutex;
std::wstring g_currentDemoPath; // last demo launched via Watch (for the marker sidecar)

void DoInit() {
	g_library.Init();
	CosmeticsRef().Init(); // load persisted offline-demo cosmetic profiles (%APPDATA%\HLAE\)
	ParticleFxRef().LoadSettings(); // effect toggles (%APPDATA%\HLAE\filmmaker_fx.json); hook arms lazily
	g_rescanRequested.store(true); // initial scan happens on the next RunFrame
}

// Builds a "playdemo" command for a demo path. Prefers a path relative to the
// game's csgo/ content root (which the engine resolves reliably); otherwise
// falls back to the absolute path. Strips the .dem extension, forward slashes.
std::string BuildPlaydemoCommand(const std::wstring& path) {
	std::wstring install = g_library.InstallRoot();
	if (!install.empty() && install.back() != L'\\' && install.back() != L'/')
		install += L'\\';
	std::wstring csgo = install + L"csgo\\";

	auto startsWithCI = [](const std::wstring& s, const std::wstring& prefix) {
		if (s.size() < prefix.size())
			return false;
		for (size_t i = 0; i < prefix.size(); ++i)
			if (towlower(s[i]) != towlower(prefix[i]))
				return false;
		return true;
	};

	std::wstring rel;
	if (startsWithCI(path, csgo))
		rel = path.substr(csgo.size());
	else
		rel = path;

	std::string u8 = WideToUtf8(rel);
	std::replace(u8.begin(), u8.end(), '\\', '/');
	if (u8.size() >= 4) {
		std::string tail = u8.substr(u8.size() - 4);
		std::transform(tail.begin(), tail.end(), tail.begin(), [](char c) { return (char)tolower(c); });
		if (tail == ".dem")
			u8.resize(u8.size() - 4);
	}

	return std::string("playdemo \"") + u8 + "\"";
}

} // namespace

void EnsureInitialized() {
	std::call_once(g_initOnce, DoInit);
}

DemoLibrary& Library() {
	EnsureInitialized();
	return g_library;
}

FilmmakerMenu& Menu() {
	EnsureInitialized();
	return g_menu;
}

void RequestRescan() {
	EnsureInitialized();
	g_rescanRequested.store(true);
}

void RequestAddFolder() {
	EnsureInitialized();
	bool expected = false;
	if (!g_pickerOpen.compare_exchange_strong(expected, true))
		return; // a picker is already open

	// The folder dialog is modal and can't be cleanly joined on shutdown, so this stays
	// detached; instead it checks g_shuttingDown before touching the shared queue so a quit
	// while the picker is open can't push into (soon-to-be) torn-down state.
	std::thread([] {
		std::wstring picked;
		const bool ok = PickFolderBlocking(picked);
		if (ok && !picked.empty() && !g_shuttingDown.load()) {
			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingFolders.push_back(picked);
		}
		g_pickerOpen.store(false);
	}).detach();
}

void Watch(std::size_t index) {
	EnsureInitialized();
	std::wstring path;
	if (!g_library.PathByIndex(index, path)) {
		advancedfx::Warning("mirv_filmmaker: no demo at index %zu\n", index);
		return;
	}
	if (!g_pEngineToClient) {
		advancedfx::Warning("mirv_filmmaker: engine not ready, cannot play demo\n");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_demoPathMutex);
		g_currentDemoPath = path; // the camera-marker sidecar keys off this
	}
	std::string cmd = BuildPlaydemoCommand(path);
	advancedfx::Message("mirv_filmmaker: %s\n", cmd.c_str());
	g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
}

std::wstring CurrentDemoPath() {
	std::lock_guard<std::mutex> lock(g_demoPathMutex);
	return g_currentDemoPath;
}

std::wstring PlayingDemoPath() {
	// Authoritative source: the demo the engine is actually playing (works for our tab,
	// native Your Matches, and console playdemo alike).
	std::wstring eng = ResolvePlayingDemoPath();
	if (!eng.empty())
		return eng;

	// If no demo is currently playing, do NOT fall back to the last Watch() path. Returning a
	// stale path here keeps CameraPath bound to the previous demo while CS2 is between demos,
	// which lets the old markers bleed into the next session.
	bool playing = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			playing = pDemo->IsPlayingDemo();
	}
	if (!playing)
		return L"";

	// Fallback: a demo is playing but the engine path couldn't be recovered. Use the path our
	// own Watch() recorded (canonicalized so it matches the engine form when it does resolve),
	// so a demo opened from our Downloaded tab is never worse off than before. Cached on the
	// input string to avoid a CreateFile() every frame in this degraded path.
	std::wstring our = CurrentDemoPath();
	if (our.empty())
		return L"";
	static std::mutex s_m;
	std::lock_guard<std::mutex> lk(s_m);
	static std::wstring s_lastIn, s_lastOut;
	if (our != s_lastIn) { s_lastIn = our; s_lastOut = CanonicalDemoPath(our); }
	return s_lastOut;
}

void RunFrame() {
	EnsureInitialized();

	// ViewFxVm write-site 3 (render-thread pump; viewmodel-rotation experiment).
	ViewFxVm_RenderPump();

	// Apply any folders chosen by the picker thread.
	std::vector<std::wstring> pending;
	{
		std::lock_guard<std::mutex> lock(g_pendingMutex);
		pending.swap(g_pendingFolders);
	}
	bool changed = false;
	for (const auto& folder : pending) {
		if (g_library.AddFolder(folder)) {
			changed = true;
			advancedfx::Message("mirv_filmmaker: added demo folder %s\n", WideToUtf8(folder).c_str());
		}
	}
	if (changed)
		g_rescanRequested.store(true);

	// Start a (re)scan on the render thread so scan lifetime is single-threaded.
	bool wantScan = true;
	if (g_rescanRequested.compare_exchange_strong(wantScan, false)) {
		if (!g_library.IsScanning())
			g_library.StartScan();
		else
			g_rescanRequested.store(true); // retry next frame
	}
}

void RunMainThreadFrame() {
	EnsureInitialized();
	// Panorama work (RunScript + panel IO) must run on the main/UI thread.
	g_menu.RunFrame(g_library);

	// End-of-demo hold: pause on the last tick instead of letting the engine disconnect to the
	// main menu; a play press while held restarts the demo from the beginning. Runs early so
	// the pause lands the same frame the tick crosses the hold point.
	DemoEndHold_RunFrame();

	// Movie director: apply queued input actions (engine commands / free-cam
	// toggle) on this thread, then refresh the in-game help/status HUD panel.
	MovieModeRef().FlushActions();
	ThirdPerson_RunFrame();

	// Camera detached from the spectated eye (free cam or third-person orbit): hide the
	// first-person viewmodel and suppress its _fp weapon FX. Both are anchored to the
	// CAMERA, so once the view leaves the eye they render as giant floating arms and
	// mid-air muzzle flashes/smoke. Restored the frame the camera re-attaches.
	{
		static bool s_fpViewHidden = false;
		static ULONGLONG s_lastSpecAssertMs = 0;
		const bool camDetached = CameraBridge_GetFreeCamEnabled()
			|| ThirdPersonCameraRef().State().active;
		if (camDetached != s_fpViewHidden) {
			s_fpViewHidden = camDetached;
			s_lastSpecAssertMs = 0;
			ParticleFx_SetFirstPersonFxSuppressed(camDetached);
			if (g_pEngineToClient) {
				g_pEngineToClient->ExecuteClientCmd(0,
					camDetached ? "r_drawviewmodel false" : "r_drawviewmodel true", true);
				// CHASE while detached: in-eye spectating spawns ONLY the _fp viewmodel
				// weapon FX for the spectated player (suppressed above -> no flashes at
				// all on their gun). Chase makes the engine spawn the THIRD-PERSON
				// world-model FX -- which the ParticleFx swap tables (Modern/Povarehok)
				// then apply to, same as for every other player. The rendered view stays
				// ours either way (the view-setup override outranks the chase camera).
				g_pEngineToClient->ExecuteClientCmd(0,
					camDetached ? "spec_mode 3" : "spec_mode 2", true);
			}
		}
		// Re-assert chase while detached: spec_mode does NOT apply while the demo is
		// paused (verified live), and the engine resets the observer mode on round
		// changes / player switches -- a one-shot exec at the transition silently
		// reverts to in-eye and the spectated player's weapon FX vanish again.
		if (camDetached && g_pEngineToClient) {
			const ULONGLONG now = GetTickCount64();
			if (now - s_lastSpecAssertMs >= 2000) {
				s_lastSpecAssertMs = now;
				g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 3", true);
			}
		}
	}
	MovieHudRef().RunFrame();

	// Camera-marker / dolly path: hover picking, playback driver, auto-save, then
	// the BO2-style marker-edit menu. Both run on this (main/UI) thread.
	CameraPathRef().RunFrame();
	// Camera Editor being open is the normal reason FollowCamera should solve/apply its pose
	// each frame; Body Cam and Action Cam are the others -- they drive the SAME FollowCamera
	// state (Attach mode, "chest" / "head" attachment) but are meant to work from the
	// lightweight Config panel too, where the editor is closed. Without this OR, RunFrame's own
	// editor-closed guard would immediately StopPreview() whatever they just started.
	FollowCameraRef().RunFrame(CameraEditorHudRef().Enabled() || BodyCam_Active() || ActionCam_Active());
	// After RunFrame, so a Body/Action Cam preview that FollowCamera just stopped on its own
	// (target death / demo end) is restored this same frame instead of leaving free-cam latched.
	BodyCam_RunFrame();
	ActionCam_RunFrame();
	// ViewFxVm write-site 2 (main-thread pump; viewmodel-rotation experiment).
	ViewFxVm_MainPump();
	MarkerHudRef().RunFrame();

	// Offline demo skin-changer: re-assert any per-player weapon-skin overrides on this frame's
	// entities. Cheap no-op when nothing is overridden. Entity-touching, so it rides the main
	// thread alongside the camera/follow passes above.
	Cosmetics_RunFrame();
	// Optional improved ragdoll model swap runs after cosmetics so live agent overrides remain
	// authoritative. It only touches dead player pawns; Off restores Valve model paths.
	RagdollFx_RunFrame();

	// Camera timeline / curve editor (native-styled scrubber + IWXMVM-style lanes).
	CameraTimelineHudRef().RunFrame();

	// Native demo-bar inline speed buttons (replaces the timescale dropdown).
	DemoBarButtonsRef().RunFrame();

	// Lightweight CONFIG panel (general UI/display settings; no camera tools). Runs before the
	// Camera Editor so the editor's host orchestration still wins the frame if both got toggled
	// in the same tick (they are mutually exclusive -- each closes the other on open).
	ConfigHudRef().RunFrame();

	// mvm_test offline live-match FX panel (effects-only; Insert toggles).
	TestHudRef().RunFrame();

	// Camera Editor Mode workspace shell. Runs LAST so its host orchestration (timeline
	// hosting + gameplay-HUD hide) is applied on top of every other panel this frame.
	CameraEditorHudRef().RunFrame();

	// Experimental After-Effects-style graph editor (opt-in overlay; default OFF). Runs after
	// everything else so, while enabled, its full-screen overlay sits on top; when disabled it
	// is a cheap no-op and the regular editor is untouched.
	GraphEditorExperimentHudRef().RunFrame();

	// Particle-effect modifiers: lazy hook-arm and one-resource-per-frame resolver. Fresh
	// defaults are fully Off, so startup/demo entry performs no custom particle loading.
	ParticleFxRef().PumpMainThread();
	// Debug squares proving the barrel-smoke wisp's parent swap actually fires, distinctly for
	// Modern vs On/Povarehok (opt-in; 'mirv_filmmaker fx debughud on|off'). No-op when off.
	FxDebugHudRef().RunFrame();
}

void Shutdown() {
	g_shuttingDown.store(true);
	FollowCameraRef().Shutdown(); // cancel + join the background loadout-event loader
	g_library.Shutdown();
}

// --- Movie director input taps (forward to the MovieMode singleton) ---
bool MovieInput_OnKey(int vkey, bool down) { return MovieModeRef().OnKey(vkey, down); }
bool MovieInput_OnMouseButton(int button, bool down) { return MovieModeRef().OnMouseButton(button, down); }
bool MovieInput_OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x, int y) { return MovieModeRef().OnMouseWheel(delta, shiftDown, ctrlDown, x, y); }

// --- HUD panel show/hide ---
void MovieHud_Set(bool visible) { MovieHudRef().SetVisible(visible); }
void MovieHud_Toggle() { MovieHudRef().Toggle(); }
bool MovieHud_Visible() { return MovieHudRef().Visible(); }
void MovieHud_Eval(const std::string& js) { MovieHudRef().RequestEval(js); }

// --- Native demo-bar speed buttons on/off ---
void DemoSpeedBar_Set(bool enabled) { DemoBarButtonsRef().SetEnabled(enabled); }
void DemoSpeedBar_Toggle() { DemoBarButtonsRef().Toggle(); }
bool DemoSpeedBar_Enabled() { return DemoBarButtonsRef().Enabled(); }

// --- Camera-marker menu: cursor/suspend hook for main.cpp ---
bool MarkerMenu_WantsCursor() { return CameraPathRef().WantsCursor(); }

// --- Camera timeline panel: cursor/suspend hook for main.cpp ---
bool CameraTimeline_WantsCursor() {
	auto& tl = CameraTimelineHudRef();
	// The camera timeline / curve editor is always a UI surface, so it forces cursor
	// ownership while open. When closed, the regular native-bar/G cursor toggle owns it.
	return tl.Cursor();
}
bool CameraTimeline_Visible() { return CameraTimelineHudRef().Visible(); }

// --- Live-match FX test menu: release the GAME mouse so the cursor drives the panel ---
// main.cpp detours CCSGOInput::MouseInputEnabled and returns false while this is true, so
// the engine stops consuming the mouse for player aim and shows the cursor on a LIVE map
// (GetSuspendMirvInput only suspends HLAE's own free-cam look, which does nothing about the
// live player's aim capture). Gated on:
//   - MvmTest_CanUseMenu(): an offline live match with a map loaded and NOT demo playback --
//     the FX menu is a private-match-only harness and must never touch demo playback (the
//     camera timeline reuses CameraTimelineHudRef().Cursor() during demos, so this explicit
//     demo exclusion keeps the mouse hook from ever firing there); and
//   - the FX panel actually open in UI-cursor mode -- pressing G (MOUSE: GAME) drops the
//     timeline cursor flag so aiming/shooting returns.
bool LiveFxMenu_WantsGameMouseReleased() {
	return MvmTest_CanUseMenu() && TestHudRef().Enabled() && CameraTimelineHudRef().Cursor();
}

// --- camera-path view ownership + debug gate (read by main.cpp's view-setup hook) ---
bool CameraPathOwnsView() { return CameraPathRef().OwnsView() || FollowCameraRef().OwnsView(); }
bool CampathDebug() { return CameraPathRef().Debug(); }

// --- Camera Editor Mode (dedicated editor workspace) ---
void CameraEditor_Set(bool enabled) { CameraEditorHudRef().SetEnabled(enabled); }
void CameraEditor_Toggle() { CameraEditorHudRef().Toggle(); }
bool CameraEditor_Active() { return CameraEditorHudRef().Enabled(); }
void CameraEditor_SetCursorMode(bool uiCursor) {
	CameraTimelineHudRef().SetCursor(uiCursor);
	GraphEditorExperimentHudRef().SetDrive(uiCursor);
	if (!uiCursor)
		CameraPathRef().StopScrub();
}

// Scaled-preview viewport (render-layer): scales the whole frame into the preview rect
// instead of showing a crop. Only renders while the editor is open and not recording.
void CameraEditor_SetScale(bool enabled) { CameraEditorHudRef().SetScale(enabled); }
void CameraEditor_ToggleScale() { CameraEditorHudRef().ToggleScale(); }
bool CameraEditor_ScaleActive() { return CameraEditorHudRef().ScaleEnabled(); }
void CameraEditor_SetUseTimeline(bool useTimeline) { CameraEditorHudRef().SetUseTimeline(useTimeline); }
void CameraEditor_ToggleUseTimeline() { CameraEditorHudRef().ToggleUseTimeline(); }
void CameraEditor_SetNativeTimeline() {
	CameraEditorHudRef().SetBottomMode(CameraEditorHud::BottomMode::Native);
}
const char* CameraEditor_HudViewName() {
	switch (CameraEditorHudRef().GetHudView()) {
	case CameraEditorHud::HudView::ShowAll: return "full";
	case CameraEditorHud::HudView::InGame: return "game";
	default: return "hidden";
	}
}
bool CameraEditor_ScaledHud(float& x0, float& y0, float& x1, float& y1) {
	CameraEditorHud& e = CameraEditorHudRef();
	e.PreviewRect(x0, y0, x1, y1);
	return e.ScaledHudActive();
}
bool CameraEditor_CustomizeModalOpen() { return CameraEditorHudRef().CustomizeModalOpen(); }
void CameraEditor_CustomizePushWheel(int notches, int x, int y) { CameraEditorHudRef().PushCustomizeWheel(notches, x, y); }
void CameraEditor_CustomizePushChar(unsigned charCode) { CameraEditorHudRef().PushCustomizeChar(charCode); }

// --- Camera-path preview: HUD masked (Tab) -> MovieHud hides itself this frame ---
bool CameraPath_PreviewHudHidden() { return CameraPathRef().PreviewHudHidden(); }

// --- Experimental graph editor (isolated opt-in overlay) ---
void GraphEditorExperiment_Set(bool enabled) { GraphEditorExperimentHudRef().SetEnabled(enabled); }
void GraphEditorExperiment_Toggle() { GraphEditorExperimentHudRef().Toggle(); }
bool GraphEditorExperiment_Enabled() { return GraphEditorExperimentHudRef().Enabled(); }
bool GraphEditorExperiment_OwnsView() { return GraphEditorExperimentHudRef().OwnsView(); }
bool GraphEditorExperiment_WantsCursor() {
	if (!GraphEditorExperimentHudRef().Enabled())
		return false;
	// Standalone graph-editor mode owns the cursor. Inside Camera Editor Mode,
	// G controls cursor ownership through the hosted timeline flag.
	return !CameraEditorHudRef().Enabled() || CameraTimelineHudRef().Cursor();
}

} // namespace Filmmaker
