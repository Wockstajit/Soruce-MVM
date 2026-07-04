#include "CameraTimelineHud.h"

#include "PanoramaFindPanel.h"

#include "CameraTimelineJs.h"
#include "../Movie/CameraPath.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../MirvTime.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


double r2(double v) { double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0; }

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			// A PAUSED demo is still an active demo: IsPlayingDemo() flips to false the
			// moment the demo is paused (e.g. SPACE), so gating the HUD on it alone tore
			// the timeline down on every pause. Treat paused as active so the UI stays up.
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

} // namespace

CameraTimelineHud& CameraTimelineHudRef() {
	static CameraTimelineHud s_instance;
	return s_instance;
}

void* CameraTimelineHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "CamTimelineRoot");
}

bool CameraTimelineHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kCameraTimelineJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void CameraTimelineHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#CamTimelineRoot'); if(e) e.DeleteAsync(0); $.CamTimeline=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
}

// Declared in Filmmaker.cpp; queried so the timeline yields the curve zone to the experimental
// graph editor (forces compact scrub view + hides its own curve-editor toggle) while it is on.
bool GraphEditorExperiment_Enabled();
const char* CameraEditor_HudViewName(); // declared in Filmmaker.cpp; game-HUD visibility while hosted
bool CameraEditor_ScaledHud(float& x0, float& y0, float& x1, float& y1); // scaled-preview rect + active
bool ConfigHud_Enabled(); // declared in ConfigHud.cpp; the Game UI picker also applies while Config is open
bool ConfigHud_ScaledHud(float& x0, float& y0, float& x1, float& y1); // Config panel's scaled-preview rect

std::string CameraTimelineHud::BuildStateJson() {
	CameraPath& cp = CameraPathRef();
	const std::vector<CamMarker>& mk = cp.Markers();
	const int n = (int)mk.size();
	const int tickMin = n > 0 ? mk.front().tick : 0;
	const int tickMax = n > 0 ? mk.back().tick : 0;

	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);
	const int sel = cp.Selected();
	const bool selValid = (sel >= 0 && sel < n);

	std::ostringstream o;
	o << "{";
	o << "\"open\":" << (m_visible ? "true" : "false");
	o << ",\"hosted\":" << (m_editorHosted ? "true" : "false");
	o << ",\"configOpen\":" << (ConfigHud_Enabled() ? "true" : "false"); // Config panel wants the hudView applied too
	o << ",\"hudView\":\"" << CameraEditor_HudViewName() << "\""; // game-HUD visibility while hosted
	{
		// Scaled-preview rect + active flag, so the JS scales the native game HUD into the same
		// rect the world blit uses (HUD then lines up with the shrunk world preview). The
		// Config panel publishes its own rect through the identical contract; the two panels
		// are mutually exclusive so at most one of these is active.
		float sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
		bool hudScale = CameraEditor_ScaledHud(sx0, sy0, sx1, sy1);
		if (!hudScale)
			hudScale = ConfigHud_ScaledHud(sx0, sy0, sx1, sy1);
		o << ",\"hudScale\":" << (hudScale ? "true" : "false");
		o << ",\"previewRect\":[" << sx0 << "," << sy0 << "," << sx1 << "," << sy1 << "]";
	}
	o << ",\"cursor\":" << (Cursor() ? "true" : "false");
	o << ",\"cursorForced\":" << (CursorForced() ? "true" : "false");
	o << ",\"view\":\"timeline\"";
	o << ",\"graphExp\":" << (GraphEditorExperiment_Enabled() ? "true" : "false");
	o << ",\"count\":" << n;
	o << ",\"selected\":" << sel;
	o << ",\"segment\":" << cp.PlaySegment();
	o << ",\"interp\":\"" << cp.InterpName() << "\"";
	o << ",\"timing\":\"" << cp.TimingName() << "\"";
	o << ",\"speedMode\":\"" << cp.SpeedModeName() << "\"";
	o << ",\"constSpeed\":" << r2(cp.ConstSpeed());
	o << ",\"playing\":" << ((cp.IsPlaying() || cp.PlaybackPending()) ? "true" : "false");
	o << ",\"previewHudHidden\":" << (cp.PreviewHudHidden() ? "true" : "false");
	o << ",\"scrubbing\":" << (cp.IsScrubbing() ? "true" : "false");
	o << ",\"tick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	o << ",\"tickMin\":" << tickMin;
	o << ",\"tickMax\":" << tickMax;
	o << ",\"scrubTick\":" << (long long)(cp.ScrubTick() + 0.5);
	o << ",\"selEase\":" << (selValid ? (int)mk[sel].ease : 0);
	o << ",\"selSpeedMul\":" << (selValid ? r2(mk[sel].speedMul) : 1.0);
	o << ",\"selIsLast\":" << ((selValid && sel == n - 1) ? "true" : "false");
	o << ",\"markers\":[";
	for (int i = 0; i < n; ++i) {
		if (i) o << ",";
		const CamMarker& m = mk[i];
		o << "{\"tick\":" << m.tick
			<< ",\"x\":" << r2(m.x) << ",\"y\":" << r2(m.y) << ",\"z\":" << r2(m.z)
			<< ",\"pitch\":" << r2(m.pitch) << ",\"yaw\":" << r2(m.yaw) << ",\"roll\":" << r2(m.roll)
			<< ",\"fov\":" << r2(m.fov) << ",\"ease\":" << (int)m.ease << ",\"speedMul\":" << r2(m.speedMul) << "}";
	}
	o << "]}";
	return o.str();
}

void CameraTimelineHud::RunFrame() {
	m_bridge.Init();

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) { Teardown(); return; }
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	if (!BuildIfNeeded())
		return;

	// Live REPL (camtl eval) in the panel context.
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

	// Timeline geometry is derived from Panorama's live root layout, not just
	// from the state JSON. Render every frame so opening during an unsettled
	// layout cannot leave the bar stuck at its fallback width.
	m_bridge.RunScript("$.CamTimeline && $.CamTimeline.render();");
}

} // namespace Filmmaker
