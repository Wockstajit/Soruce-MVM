#include "CameraEditorHud.h"

#include "PanoramaFindPanel.h"

#include "CameraEditorJs.h"
#include "../Cosmetics/CosmeticUiQueries.h"
#include "CameraTimelineHud.h"
#include "ConfigHud.h" // ConfigHud_Enabled: don't clear the shared viewport-scaler request while Config owns it
#include "GraphEditorExperimentHud.h"
#include "MovieHud.h"
#include "../Movie/CameraPath.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/FollowCamera.h"
#include "../Cosmetics/CosmeticOverrideSystem.h"
#include "../Cosmetics/CosmeticDebugLog.h" // Filmmaker::MvmDebugLog_Active (gates JS preview diagnostics)

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../ClientEntitySystem.h" // AfxGetLocalObserverState (spectator gating for Customize)
#include "../../MirvTime.h"
#include "../../SchemaSystem.h"
#include "../../ViewportScaler.h" // AfxViewportScaler scaled-preview bridge
#include "../../WrpConsole.h" // advancedfx::Message (debug-overlay-gated [vpscale] diagnostics)

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


double r2(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0; // NaN/inf -> keep JSON valid
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0;
}

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			// A PAUSED demo is still an active demo: IsPlayingDemo() flips to false the
			// moment the demo is paused (e.g. SPACE). Gating the editor on it alone made
			// the whole workspace force-exit (OnExit) on every pause, which also dropped
			// the input gating and let scroll/click/ESC leak to the game. Keep paused = up.
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

} // namespace

CameraEditorHud& CameraEditorHudRef() {
	static CameraEditorHud s_instance;
	return s_instance;
}

void* CameraEditorHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "CamEditorRoot");
}

bool CameraEditorHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kCameraEditorJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symPreviewRect = m_bridge.MakeSymbol("previewrect");
	m_symDebugPanels = m_bridge.MakeSymbol("debugpanels");
	m_symCustomizeOpen = m_bridge.MakeSymbol("customizeopen");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void CameraEditorHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#CamEditorRoot'); if(e) e.DeleteAsync(0); var d=$('#CamEditorDebugRoot'); if(d) d.DeleteAsync(0);"
			" var c=$('#CamEditorConfirmRoot'); if(c) c.DeleteAsync(0); var s=$('#CamEditorSettingsRoot'); if(s) s.DeleteAsync(0); $.CamEditor=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
	m_customizeOpen = false; // don't leave input permanently swallowed if the editor tears down mid-modal
}

// Declared in Filmmaker.cpp.
bool GraphEditorExperiment_Enabled();
void GraphEditorExperiment_Set(bool enabled);
const char* CameraEditor_HudViewName();

// One-shot enter: default the bottom to the REGULAR (native CS2) timeline -- the familiar game
// demo bar, docked to fit left of the inspector (CameraTimelineJs), minus its CAM EDITOR/MOUSE
// buttons. The bottom tab bar switches to the camera timeline or graph. Host the custom timeline,
// hide the floating movie-director cards, select a key for the inspector. Free cam is NOT forced
// on -- the user toggles it explicitly (the editor must not hijack the camera on open).
void CameraEditorHud::OnEnter() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	m_prevMovieHud = MovieHudRef().Visible();
	MovieHudRef().SetVisible(false);

	tl.SetEditorHosted(true);
	tl.SetVisible(false); // default bottom mode is the native (Regular) timeline; camera timeline hidden
	tl.SetCursor(true); // start in UI-cursor so the inspector is immediately clickable

	m_bottomMode = BottomMode::Native;
	GraphEditorExperiment_Set(false);

	// Scale the live game into the preview rect by default -- that "shrunk viewport" IS the
	// point of the editor (vs. the full-screen crop). `mirv_filmmaker editor scale off`
	// reverts to the crop. Auto-disables itself while recording (engine-side check).
	m_scaleEnabled = true;

	// Do NOT hijack the camera on open. Opening the editor only shows the UI -- it must not
	// enable free cam, pause, seek, or jump to a camera. Pre-select a key for the inspector
	// read-out ONLY (no teleport), and only when nothing is already selected, so the current
	// view and camera mode are left exactly as they were. Jumping to / previewing / editing a
	// camera now requires an explicit action (nav arrows, preview, edit -> SelectForEditor).
	if (cp.Count() > 0 && cp.Selected() < 0) cp.SelectIndex(0, /*teleport*/ false);
}

// One-shot exit: restore everything the enter step changed and tear down the chrome.
void CameraEditorHud::OnExit() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	tl.SetEditorHosted(false);
	tl.SetVisible(false);
	GraphEditorExperiment_Set(false);
	FollowCameraRef().StopPreview("camera editor closed");
	cp.StopScrub();

	MovieHudRef().SetVisible(m_prevMovieHud);

	// NOTE: cosmetic overrides are keyed by SteamID and persist across editor open/close on
	// purpose now (they follow the player through the whole demo). Closing the editor no longer
	// clears them; use "mirv_filmmaker cosmetics clear" or the Customize modal to remove them.

	// Drop any pending scaled-preview blit so the next full-screen frame renders normally --
	// unless the Config panel is taking over the viewport scaler (editor -> config switch).
	if (!ConfigHud_Enabled())
		AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);

	Teardown();
}

std::string CameraEditorHud::BuildStateJson() {
	CameraPath& cp = CameraPathRef();
	const std::vector<CamMarker>& mk = cp.Markers();
	const int n = (int)mk.size();
	const int sel = cp.Selected();
	const bool selValid = (sel >= 0 && sel < n);
	const CameraPath::Mode pathMode = cp.GetMode();
	const bool pathPlaying = cp.IsPlaying() || cp.PlaybackPending();
	const bool pathLive = pathPlaying || pathMode == CameraPath::Mode::PreviewPlaying;

	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);

	double camOrigin[3] = { 0,0,0 }, camAngles[3] = { 0,0,0 }, camFov = 0.0;
	CameraBridge_GetCurrentCamera(camOrigin, camAngles, camFov);

	CameraTimelineHud& tl = CameraTimelineHudRef();

	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_enabled ? "true" : "false");
	o << ",\"graphExp\":" << (GraphEditorExperiment_Enabled() ? "true" : "false");
	o << ",\"graphDrive\":" << (GraphEditorExperimentHudRef().Drive() ? "true" : "false");
	o << ",\"bottomMode\":\"" << (m_bottomMode == BottomMode::Graph ? "graph" :
		(m_bottomMode == BottomMode::CameraTimeline ? "camera" : "native")) << "\"";
	o << ",\"hudView\":\"" << CameraEditor_HudViewName() << "\""; // game-UI visibility picker
	o << ",\"debug\":" << (m_debugOverlay ? "true" : "false");
	if (m_debugOverlay) {
		// Render-layer numbers for the viewport debug overlay: the ACTUAL world-blit rect (px) +
		// the backbuffer/render-target size, so JS can compare them against the Panorama-side
		// preview rect and prove the custom viewport matches the game viewport 1:1.
		int bbW = 0, bbH = 0; float vx = 0, vy = 0, vw = 0, vh = 0;
		const bool blitRan = AfxViewportScaler::GetLastBlit(bbW, bbH, vx, vy, vw, vh);
		o << ",\"dbg\":{\"blitRan\":" << (blitRan ? "true" : "false")
			<< ",\"bbW\":" << bbW << ",\"bbH\":" << bbH
			<< ",\"vx\":" << r2(vx) << ",\"vy\":" << r2(vy) << ",\"vw\":" << r2(vw) << ",\"vh\":" << r2(vh)
			<< ",\"scaleReq\":" << (m_scaleEnabled ? "true" : "false")
			<< ",\"previewValid\":" << (m_previewValid ? "true" : "false");
		std::string panelsJson = "[]";
		if (m_root && m_symDebugPanels >= 0) {
			std::string rawPanels = m_bridge.GetAttributeString(m_root, m_symDebugPanels, "[]");
			if (!rawPanels.empty() && rawPanels[0] == '[')
				panelsJson = rawPanels;
		}
		o << ",\"panels\":" << panelsJson << "}";
	}
	o << ",\"cursor\":" << (tl.Cursor() ? "true" : "false");
	o << ",\"tick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	bool paused = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			paused = pDemo->IsDemoPaused();
	}
	o << ",\"paused\":" << (paused ? "true" : "false");
	// So the Customize preview's JS-side diagnostics (previewLog, see CameraEditorCustomizeJs.h) only
	// fire while `mvm_debug start` is active -- reuses the existing session log instead of adding a
	// second debug toggle; running `mvm_debug start` while the modal is open now also captures rich
	// preview state (selection, model paths, item ids, panel geometry) via the tier0 console capture.
	o << ",\"mvmDebug\":" << (Filmmaker::MvmDebugLog_Active() ? "true" : "false");
	// Live OS-cursor pipe (reused from the experimental graph editor's drag mechanism -- Panorama has
	// no mouse-move event in this in-game HUD context, so CameraBridge_GetUiCursor polls GetCursorPos
	// each frame + edge-captures button state from the WndProc; see main.cpp g_UiCursorProbe). Lets the
	// Customize preview implement click-and-drag panning entirely in JS without any new C++ plumbing.
	{
		int cmx = 0, cmy = 0; bool clmb = false, crmb = false, cshift = false; unsigned cseq = 0;
		CameraBridge_GetUiCursor(cmx, cmy, clmb, crmb, cshift, cseq);
		o << ",\"mx\":" << cmx << ",\"my\":" << cmy << ",\"lmb\":" << (clmb ? "true" : "false") << ",\"mseq\":" << cseq;
	}
	o << ",\"count\":" << n;
	o << ",\"selected\":" << sel;
	o << ",\"interp\":\"" << cp.InterpName() << "\"";
	o << ",\"timing\":\"" << cp.TimingName() << "\"";
	o << ",\"speedMode\":\"" << cp.SpeedModeName() << "\"";
	o << ",\"pathMode\":\"" << cp.ModeName() << "\"";
	o << ",\"pathPlaying\":" << (pathPlaying ? "true" : "false");
	o << ",\"pathLive\":" << (pathLive ? "true" : "false");
	o << ",\"timelineView\":\"" << (tl.View() == 1 ? "curve" : "timeline") << "\"";
	o << ",\"constSpeed\":" << r2(cp.ConstSpeed());
	o << ",\"freeCam\":" << (CameraBridge_GetFreeCamEnabled() ? "true" : "false");
	o << ",\"freeCamSpeed\":" << r2(CameraBridge_GetFreeCamSpeed());
	// Spectator state for the "Customize" gating + modal: obsMode 2 (in-eye/first) or 3
	// (chase/third) means we're watching a player; 4 (roaming) is freecam. obsTarget is the
	// spectated entity index (-1 if none) so the modal can identify whose loadout to edit.
	{
		int obsTargetIndex = -1;
		uint8_t obsMode = AfxGetLocalObserverState(&obsTargetIndex);
		// AfxGetLocalObserverState's target reads -1 in POV/GOTV demos (so customizeTarget would be
		// null and the modal would fall back to a fuzzy nearest-player guess). Use the robust eye-match
		// resolver so customizeTarget is deterministically the player actually being viewed. Returns -1
		// in free cam (no single spectated player) -> customizeTarget null, fallback as before.
		int spectated = AfxGetSpectatedPawnIndex();
		if (spectated >= 0) obsTargetIndex = spectated;
		o << ",\"obsMode\":" << (int)obsMode;
		o << ",\"obsTarget\":" << obsTargetIndex;
		o << ",\"customizeTarget\":" << BuildCustomizeTargetJson(obsTargetIndex);
		o << ",\"customizePlayers\":" << BuildCustomizePlayersJson();
	}
	o << ",\"cam\":{\"x\":" << r2(camOrigin[0]) << ",\"y\":" << r2(camOrigin[1]) << ",\"z\":" << r2(camOrigin[2])
		<< ",\"pitch\":" << r2(camAngles[0]) << ",\"yaw\":" << r2(camAngles[1]) << ",\"roll\":" << r2(camAngles[2])
		<< ",\"fov\":" << r2(camFov) << "}";
	o << ",\"follow\":" << FollowCameraRef().BuildStateJson();
	if (selValid) {
		const CamMarker& m = mk[sel];
		o << ",\"sel\":{\"tick\":" << m.tick
			<< ",\"x\":" << r2(m.x) << ",\"y\":" << r2(m.y) << ",\"z\":" << r2(m.z)
			<< ",\"pitch\":" << r2(m.pitch) << ",\"yaw\":" << r2(m.yaw) << ",\"roll\":" << r2(m.roll)
			<< ",\"fov\":" << r2(m.fov) << ",\"ease\":" << (int)m.ease << ",\"speedMul\":" << r2(m.speedMul)
			<< ",\"isLast\":" << ((sel == n - 1) ? "true" : "false") << "}";
	} else {
		o << ",\"sel\":null";
	}
	o << "}";
	return o.str();
}

void CameraEditorHud::RunFrame() {
	m_bridge.Init();

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;

	// Demo not playing (or HUD gone): force-exit editor mode cleanly so we never leave
	// the gameplay HUD hidden or the timeline orphaned.
	if (!hud) {
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		else if (!ConfigHud_Enabled()) AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	// Enter / exit edge transitions.
	if (m_enabled && !m_wasEnabled) { OnEnter(); m_wasEnabled = true; }
	else if (!m_enabled && m_wasEnabled) { OnExit(); m_wasEnabled = false; }

	if (!m_enabled && !m_debugOverlay) {
		// ConfigHud (which ran earlier this frame) may own the scaler request now -- leave it.
		if (!ConfigHud_Enabled())
			AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	if (!m_enabled && m_debugOverlay) {
		const bool configOwnsScaler = ConfigHud_Enabled();
		if (!BuildIfNeeded()) {
			if (!configOwnsScaler) AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
			return;
		}
		m_root = FindRoot();
		if (!m_root) {
			m_built = false;
			if (!configOwnsScaler) AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
			return;
		}
		std::string state = BuildStateJson();
		if (state != m_lastState) {
			m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
			m_lastState = state;
		}
		m_bridge.RunScript("$.CamEditor && $.CamEditor.render();");
		if (!configOwnsScaler) AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	// While enabled, re-assert hosting every frame (cheap) so a stray timeline close or HUD
	// recreation can't leave the workspace half-torn-down. Native mode leaves CS2's own demo
	// timeline visible; custom camera timeline / graph are explicit bottom overlays.
	const bool useGraph = m_bottomMode == BottomMode::Graph;
	const bool useCameraTimeline = m_bottomMode == BottomMode::CameraTimeline;
	GraphEditorExperiment_Set(useGraph);
	CameraTimelineHudRef().SetEditorHosted(true);
	CameraTimelineHudRef().SetVisible(useCameraTimeline);

	if (!BuildIfNeeded()) {
		AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	// Live REPL (editor eval) in the panel context.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	m_root = FindRoot();
	if (!m_root) {
		m_built = false;
		AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	std::string state = BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	// Always render while enabled so the chrome re-asserts its layout each frame. render()
	// (re)publishes the "previewrect" attribute -- the normalised preview-rect fractions --
	// as the single source of truth shared with the D3D blit.
	m_bridge.RunScript("$.CamEditor && $.CamEditor.render();");

	// Feed any wheel/typed-character input captured off the WndProc thread into the modal JS (only
	// does work while the modal is open + something is queued). Runs after render() so the panel it
	// scrolls/filters is laid out for this frame.
	DispatchCustomizeInput();

	// TRUE scaled preview: forward the rect render() just published to the viewport scaler.
	// The blit only actually runs engine-side when not recording (full-screen capture wins).
	UpdateScaleRequest();
	UpdateCustomizeModalState();
	// While the Customize modal is up, keep the flashbang whiteout suppressed so it never washes
	// out the modal UI or the 3D player preview (the flash post-effect covers Panorama too).
	if (m_customizeOpen)
		CustomizeSuppressFlashTick();
}

// Reads the "previewrect" fractions the editor JS published this frame and forwards them to
// the render-layer scaler. When scaling is off (or the rect is missing/degenerate) the request
// is cleared, so the preview falls back to the Panorama crop.
void CameraEditorHud::UpdateScaleRequest() {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::string pr;
	bool parsed = false, valid = false;

	// Read the published rect whenever the editor is open (cheap), independent of the scale
	// toggle, so diagnostics show the JS->C++ round-trip status even with scaling off.
	if (m_root) {
		pr = m_bridge.GetAttributeString(m_root, m_symPreviewRect, "");
		std::istringstream is(pr);
		parsed = (bool)(is >> x0 >> y0 >> x1 >> y1);
		valid = parsed && (x1 - x0 > 0.01f) && (y1 - y0 > 0.01f)
			&& x0 >= 0 && y0 >= 0 && x1 <= 1.0001f && y1 <= 1.0001f;
	}

	// Cache the rect so the timeline HUD can scale the native game HUD into the same region.
	m_previewValid = valid;
	if (valid) { m_previewX0 = x0; m_previewY0 = y0; m_previewX1 = x1; m_previewY1 = y1; }

	const bool active = m_scaleEnabled && valid;

	// Diagnostics: only while the debug overlay is on, and only when the decision inputs
	// CHANGE (no per-frame spam, silent in normal use).
	static int s_prev = -1;
	int now = (m_scaleEnabled ? 1 : 0) | (valid ? 2 : 0);
	if (now != s_prev) {
		s_prev = now;
		if (m_debugOverlay)
			advancedfx::Message("[vpscale] req: scaleOn=%d raw='%s' parsed=%d valid=%d\n",
				(int)m_scaleEnabled, pr.c_str(), (int)parsed, (int)valid);
	}

	AfxViewportScaler::SetRequest(active, x0, y0, x1, y1);
}

void CameraEditorHud::PushCustomizeWheel(int notches, int x, int y) {
	if (notches == 0) return;
	std::lock_guard<std::mutex> lk(m_custInputMutex);
	if (m_custWheelEvents.size() < 128)
		m_custWheelEvents.push_back({ notches, x, y });
}

void CameraEditorHud::PushCustomizeChar(unsigned charCode) {
	std::lock_guard<std::mutex> lk(m_custInputMutex);
	// Guard against a stuck/huge backlog if the modal ever stops draining (it drains every frame).
	if (m_custChars.size() < 512) m_custChars.push_back(charCode);
}

// Main thread. Drain the wheel + typed-character queue captured off the WndProc thread and hand it
// to the modal JS. Batches per frame: one custWheel(net) call and one custChars('c,c,...') call.
void CameraEditorHud::DispatchCustomizeInput() {
	std::vector<CustomizeWheelEvent> wheels;
	std::vector<unsigned> chars;
	{
		std::lock_guard<std::mutex> lk(m_custInputMutex);
		wheels.swap(m_custWheelEvents);
		chars.swap(m_custChars);
	}
	if (!m_root) return;
	for (const CustomizeWheelEvent& ev : wheels) {
		std::ostringstream o;
		o << "$.CamEditor && $.CamEditor.custWheel(" << ev.notches << "," << ev.x << "," << ev.y << ");";
		m_bridge.RunScript(o.str().c_str());
	}
	if (!chars.empty()) {
		std::ostringstream o;
		o << "$.CamEditor && $.CamEditor.custChars('";
		for (size_t i = 0; i < chars.size(); ++i) { if (i) o << ','; o << chars[i]; }
		o << "');";
		m_bridge.RunScript(o.str().c_str());
	}
}

// Reads the "customizeopen" flag the Customize modal's openCustomize()/closeCustomize()/render()
// publish (see CameraEditorCustomizeJs.h) so the input layer (MovieMode, GetSuspendMirvInput) can
// treat the modal as an exclusive surface. m_root is only non-null while the workspace is built,
// so this naturally reads false (no attribute to read) whenever the editor itself is closed.
void CameraEditorHud::UpdateCustomizeModalState() {
	m_customizeOpen = m_root && (m_bridge.GetAttributeString(m_root, m_symCustomizeOpen, "0") == "1");
}

} // namespace Filmmaker
