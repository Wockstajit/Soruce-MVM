#include "CameraEditorHud.h"

#include "CameraEditorJs.h"
#include "CameraTimelineHud.h"
#include "MovieHud.h"
#include "../Movie/CameraPath.h"
#include "../Movie/CameraBridge.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../MirvTime.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Bounded recursive id search (same approach as CameraTimelineHud / MovieHud).
void* FindChildById(void* panel, const char* id, int depth = 0) {
	if (!panel || depth > 64)
		return nullptr;
	unsigned char* childrenField = (unsigned char*)panel + CS2::PanoramaUIPanel::children;
	const int count = *(int*)childrenField;
	void** arr = *(void***)(childrenField + 8);
	if (!arr || count <= 0 || count > 100000)
		return nullptr;
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		char* cid = *(char**)((unsigned char*)child + CS2::PanoramaUIPanel::panelId);
		if (cid && 0 == std::strcmp(cid, id))
			return child;
	}
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		if (void* found = FindChildById(child, id, depth + 1))
			return found;
	}
	return nullptr;
}

double r2(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0; // NaN/inf -> keep JSON valid
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0;
}

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			return pDemo->IsPlayingDemo();
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
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void CameraEditorHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#CamEditorRoot'); if(e) e.DeleteAsync(0); $.CamEditor=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
}

// One-shot enter: host the timeline (decouples its forced cursor so G can still toggle
// to fly), hide the floating movie-director cards, enable free cam, and select a key so
// the inspector has something to edit. Idempotent on re-entry is handled by the caller.
void CameraEditorHud::OnEnter() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	m_prevMovieHud = MovieHudRef().Visible();
	MovieHudRef().SetVisible(false);

	tl.SetEditorHosted(true);
	tl.SetVisible(true);
	tl.SetCursor(true); // start in UI-cursor so the inspector is immediately clickable

	CameraBridge_SetFreeCamEnabled(true);
	if (cp.Count() > 0) cp.SelectForEditor(cp.Selected() >= 0 ? cp.Selected() : 0);
}

// One-shot exit: restore everything the enter step changed and tear down the chrome.
void CameraEditorHud::OnExit() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	tl.SetEditorHosted(false);
	tl.SetVisible(false);
	cp.StopScrub();

	MovieHudRef().SetVisible(m_prevMovieHud);

	Teardown();
}

std::string CameraEditorHud::BuildStateJson() {
	CameraPath& cp = CameraPathRef();
	const std::vector<CamMarker>& mk = cp.Markers();
	const int n = (int)mk.size();
	const int sel = cp.Selected();
	const bool selValid = (sel >= 0 && sel < n);

	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);

	double camOrigin[3] = { 0,0,0 }, camAngles[3] = { 0,0,0 }, camFov = 0.0;
	CameraBridge_GetCurrentCamera(camOrigin, camAngles, camFov);

	CameraTimelineHud& tl = CameraTimelineHudRef();

	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_enabled ? "true" : "false");
	o << ",\"cursor\":" << (tl.Cursor() ? "true" : "false");
	o << ",\"tick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	o << ",\"count\":" << n;
	o << ",\"selected\":" << sel;
	o << ",\"interp\":\"" << cp.InterpName() << "\"";
	o << ",\"timing\":\"" << cp.TimingName() << "\"";
	o << ",\"speedMode\":\"" << cp.SpeedModeName() << "\"";
	o << ",\"constSpeed\":" << r2(cp.ConstSpeed());
	o << ",\"freeCam\":" << (CameraBridge_GetFreeCamEnabled() ? "true" : "false");
	o << ",\"freeCamSpeed\":" << r2(CameraBridge_GetFreeCamSpeed());
	o << ",\"cam\":{\"x\":" << r2(camOrigin[0]) << ",\"y\":" << r2(camOrigin[1]) << ",\"z\":" << r2(camOrigin[2])
		<< ",\"pitch\":" << r2(camAngles[0]) << ",\"yaw\":" << r2(camAngles[1]) << ",\"roll\":" << r2(camAngles[2])
		<< ",\"fov\":" << r2(camFov) << "}";
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
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	// Enter / exit edge transitions.
	if (m_enabled && !m_wasEnabled) { OnEnter(); m_wasEnabled = true; }
	else if (!m_enabled && m_wasEnabled) { OnExit(); m_wasEnabled = false; }

	if (!m_enabled)
		return;

	// While enabled, re-assert hosting every frame (cheap) so a stray timeline close or
	// HUD recreation can't leave the workspace half-torn-down.
	CameraTimelineHudRef().SetEditorHosted(true);
	CameraTimelineHudRef().SetVisible(true);

	if (!BuildIfNeeded())
		return;

	// Live REPL (editor eval) in the panel context.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	m_root = FindRoot();
	if (!m_root) { m_built = false; return; }

	std::string state = BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	// Always render while enabled so the chrome re-asserts its layout each frame.
	m_bridge.RunScript("$.CamEditor && $.CamEditor.render();");
}

} // namespace Filmmaker
