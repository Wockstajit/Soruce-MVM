#include "Filmmaker.h"

#include "Demo/DemoLibrary.h"
#include "Demo/PlayingDemoPath.h"
#include "Panorama/FilmmakerMenu.h"
#include "Panorama/MovieHud.h"
#include "Panorama/MarkerHud.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/CameraEditorHud.h"
#include "Panorama/GraphEditorExperimentHud.h"
#include "Panorama/DemoBarButtons.h"
#include "Movie/MovieMode.h"
#include "Movie/CameraPath.h"
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

std::mutex g_pendingMutex;
std::vector<std::wstring> g_pendingFolders;

std::mutex g_demoPathMutex;
std::wstring g_currentDemoPath; // last demo launched via Watch (for the marker sidecar)

void DoInit() {
	g_library.Init();
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

	std::thread([] {
		std::wstring picked;
		const bool ok = PickFolderBlocking(picked);
		if (ok && !picked.empty()) {
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

	// Movie director: apply queued input actions (engine commands / free-cam
	// toggle) on this thread, then refresh the in-game help/status HUD panel.
	MovieModeRef().FlushActions();
	MovieHudRef().RunFrame();

	// Camera-marker / dolly path: hover picking, playback driver, auto-save, then
	// the BO2-style marker-edit menu. Both run on this (main/UI) thread.
	CameraPathRef().RunFrame();
	MarkerHudRef().RunFrame();

	// Camera timeline / curve editor (native-styled scrubber + IWXMVM-style lanes).
	CameraTimelineHudRef().RunFrame();

	// Native demo-bar inline speed buttons (replaces the timescale dropdown).
	DemoBarButtonsRef().RunFrame();

	// Camera Editor Mode workspace shell. Runs LAST so its host orchestration (timeline
	// hosting + gameplay-HUD hide) is applied on top of every other panel this frame.
	CameraEditorHudRef().RunFrame();

	// Experimental After-Effects-style graph editor (opt-in overlay; default OFF). Runs after
	// everything else so, while enabled, its full-screen overlay sits on top; when disabled it
	// is a cheap no-op and the regular editor is untouched.
	GraphEditorExperimentHudRef().RunFrame();
}

void Shutdown() {
	g_library.Shutdown();
}

// --- Movie director input taps (forward to the MovieMode singleton) ---
bool MovieInput_OnKey(int vkey, bool down) { return MovieModeRef().OnKey(vkey, down); }
bool MovieInput_OnMouseButton(int button, bool down) { return MovieModeRef().OnMouseButton(button, down); }
bool MovieInput_OnMouseWheel(int delta, bool shiftDown, bool ctrlDown) { return MovieModeRef().OnMouseWheel(delta, shiftDown, ctrlDown); }

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

// --- camera-path view ownership + debug gate (read by main.cpp's view-setup hook) ---
bool CameraPathOwnsView() { return CameraPathRef().OwnsView(); }
bool CampathDebug() { return CameraPathRef().Debug(); }

// --- Camera Editor Mode (dedicated editor workspace) ---
void CameraEditor_Set(bool enabled) { CameraEditorHudRef().SetEnabled(enabled); }
void CameraEditor_Toggle() { CameraEditorHudRef().Toggle(); }
bool CameraEditor_Active() { return CameraEditorHudRef().Enabled(); }

// Scaled-preview viewport (render-layer): scales the whole frame into the preview rect
// instead of showing a crop. Only renders while the editor is open and not recording.
void CameraEditor_SetScale(bool enabled) { CameraEditorHudRef().SetScale(enabled); }
void CameraEditor_ToggleScale() { CameraEditorHudRef().ToggleScale(); }
bool CameraEditor_ScaleActive() { return CameraEditorHudRef().ScaleEnabled(); }
void CameraEditor_SetUseTimeline(bool useTimeline) { CameraEditorHudRef().SetUseTimeline(useTimeline); }
void CameraEditor_ToggleUseTimeline() { CameraEditorHudRef().ToggleUseTimeline(); }

// --- Camera-path preview: HUD masked (Tab) -> MovieHud hides itself this frame ---
bool CameraPath_PreviewHudHidden() { return CameraPathRef().PreviewHudHidden(); }

// --- Experimental graph editor (isolated opt-in overlay) ---
void GraphEditorExperiment_Set(bool enabled) { GraphEditorExperimentHudRef().SetEnabled(enabled); }
void GraphEditorExperiment_Toggle() { GraphEditorExperimentHudRef().Toggle(); }
bool GraphEditorExperiment_Enabled() { return GraphEditorExperimentHudRef().Enabled(); }
bool GraphEditorExperiment_OwnsView() { return GraphEditorExperimentHudRef().OwnsView(); }
bool GraphEditorExperiment_WantsCursor() { return GraphEditorExperimentHudRef().WantsCursor(); }

} // namespace Filmmaker
