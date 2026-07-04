#include "MovieHud.h"

#include "PanoramaFindPanel.h"

#include "MovieHudJs.h"
#include "../Filmmaker.h"            // CameraPath_PreviewHudHidden()
#include "../Movie/MovieMode.h"
#include "../Movie/CameraBridge.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets + g_CurrentGameCamera
#include "../../MirvTime.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>
#include <cmath>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


// 1-decimal rounding so tiny float jitter doesn't spam panel updates.
double r1(double v) { return std::floor(v * 10.0 + 0.5) / 10.0; }

} // namespace

MovieHud& MovieHudRef() {
	static MovieHud s_instance;
	return s_instance;
}

void* MovieHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx)
		return nullptr;
	return FindChildById(ctx, "MovieHudRoot");
}

bool MovieHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kMovieHudJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastJson.clear();
	return m_built;
}

void MovieHud::Teardown() {
	// Remove the panel from whatever context we last built it in, so it can't
	// linger over the main menu. Safe to call when nothing was built.
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#MovieHudRoot'); if(e) e.DeleteAsync(0); $.MovieHud=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastJson.clear();
}

std::string MovieHud::BuildStateJson() {
	MovieMode& mode = MovieModeRef();

	bool playing = false, paused = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile()) {
			playing = pDemo->IsPlayingDemo();
			if (playing) paused = pDemo->IsDemoPaused();
		}
	}

	int tick = 0; bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
	double dtime = 0.0; bool haveTime = g_MirvTime.GetCurrentDemoTime(dtime);

	// Masked for a clean shot while a camera-path preview has the HUD toggled off.
	bool visible = m_visible && !CameraPath_PreviewHudHidden();

	std::ostringstream o;
	o << "{";
	o << "\"visible\":" << (visible ? "true" : "false");
	o << ",\"mode\":\"" << mode.ModeName() << "\"";
	o << ",\"playing\":" << (playing ? "true" : "false");
	o << ",\"paused\":" << (paused ? "true" : "false");
	o << ",\"freecam\":" << (CameraBridge_GetFreeCamEnabled() ? "true" : "false");
	o << ",\"speed\":" << r1(CameraBridge_GetFreeCamSpeed());
	o << ",\"xray\":" << (mode.GetXray() ? "true" : "false");
	o << ",\"cursor\":" << (mode.GetCursor() ? "true" : "false");
	o << ",\"player\":\"\"";
	o << ",\"ox\":" << r1(g_CurrentGameCamera.origin[0]);
	o << ",\"oy\":" << r1(g_CurrentGameCamera.origin[1]);
	o << ",\"oz\":" << r1(g_CurrentGameCamera.origin[2]);
	o << ",\"pitch\":" << r1(g_CurrentGameCamera.angles[0]);
	o << ",\"yaw\":" << r1(g_CurrentGameCamera.angles[1]);
	o << ",\"roll\":" << r1(g_CurrentGameCamera.angles[2]);
	o << ",\"fov\":" << r1(CameraBridge_GetGameCameraFov());
	if (haveTick) o << ",\"tick\":" << tick;
	if (haveTime) o << ",\"time\":" << r1(dtime);
	o << "}";
	return o.str();
}

void MovieHud::RunFrame() {
	// Keep the director's mode in sync if free cam was toggled via console.
	MovieModeRef().SyncFromFreeCam(CameraBridge_GetFreeCamEnabled());

	m_bridge.Init(); // resolve engine + RunScript (idempotent)

	// The movie HUD exists ONLY while a demo is actively playing. In the main
	// menu (or any non-demo session) we must not build a panel at all, otherwise
	// it can overlay the menu and steal clicks.
	bool playingDemo = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			playingDemo = pDemo->IsPlayingDemo();
	}

	unsigned char* hud = playingDemo ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) {           // main menu / not playing a demo -> tear down + bail
		Teardown();
		return;
	}
	if (hud != m_hudPanel) { // HUD (re)created -> rebuild against the new context
		m_hudPanel = hud;
		m_built = false;
	}
	m_bridge.SetContextPanel(hud); // pin to the in-game HUD (not the main menu)

	if (!BuildIfNeeded())
		return;

	// Drain queued hud_eval scripts (live REPL) in the HUD context.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	m_root = FindRoot();
	if (!m_root) { // vanished -> rebuild next frame
		m_built = false;
		return;
	}

	std::string json = BuildStateJson();
	if (json != m_lastJson) {
		m_bridge.SetAttributeString(m_root, m_symState, json.c_str());
		m_bridge.RunScript("$.MovieHud && $.MovieHud.render();");
		m_lastJson = json;
	}
}

} // namespace Filmmaker
