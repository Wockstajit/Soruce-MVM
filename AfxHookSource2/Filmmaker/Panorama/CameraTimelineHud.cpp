#include "CameraTimelineHud.h"

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

// Bounded recursive id search (same approach as MovieHud's).
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

double r2(double v) { double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0; }
double r3(double v) { double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 1000.0 + 0.5) / 1000.0; }

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			return pDemo->IsPlayingDemo();
	}
	return false;
}

} // namespace

CameraTimelineHud& CameraTimelineHudRef() {
	static CameraTimelineHud s_instance;
	return s_instance;
}

void CameraTimelineHud::ZoomIn() {
	double c = (m_viewT0 + m_viewT1) * 0.5;
	double h = (m_viewT1 - m_viewT0) * 0.5 * 0.7;
	if (h < 16.0) h = 16.0; // don't zoom in past ~32 ticks of span
	m_viewT0 = c - h; m_viewT1 = c + h; m_userZoomed = true;
}
void CameraTimelineHud::ZoomOut() {
	double c = (m_viewT0 + m_viewT1) * 0.5;
	double h = (m_viewT1 - m_viewT0) * 0.5 / 0.7;
	m_viewT0 = c - h; m_viewT1 = c + h; m_userZoomed = true;
}
void CameraTimelineHud::ZoomReset() { m_userZoomed = false; m_zoomInit = false; }
void CameraTimelineHud::Pan(int dir) {
	double w = (m_viewT1 - m_viewT0);
	double d = w * 0.2 * (dir < 0 ? -1.0 : 1.0);
	m_viewT0 += d; m_viewT1 += d; m_userZoomed = true;
}

void CameraTimelineHud::EnsureZoomWindow(int tickMin, int tickMax) {
	if (tickMax <= tickMin) {
		m_viewT0 = tickMin; m_viewT1 = tickMin + 1.0; m_zoomInit = true;
		m_lastTickMin = tickMin; m_lastTickMax = tickMax;
		return;
	}
	// Auto-fit to the full path (with a little padding) until the user zooms/pans.
	bool rangeChanged = (tickMin != m_lastTickMin || tickMax != m_lastTickMax);
	if (!m_zoomInit || (!m_userZoomed && rangeChanged)) {
		double pad = (tickMax - tickMin) * 0.04 + 1.0;
		m_viewT0 = tickMin - pad; m_viewT1 = tickMax + pad;
		m_zoomInit = true;
	}
	m_lastTickMin = tickMin; m_lastTickMax = tickMax;
	if (m_viewT1 <= m_viewT0) m_viewT1 = m_viewT0 + 1.0;
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
	m_symCurve = m_bridge.MakeSymbol("curve");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	m_lastCurveBody.clear();
	m_lastCurveJson.clear();
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
	m_lastCurveBody.clear();
	m_lastCurveJson.clear();
}

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
	o << ",\"cursor\":" << (Cursor() ? "true" : "false");
	o << ",\"cursorForced\":" << (CursorForced() ? "true" : "false");
	o << ",\"view\":\"" << (m_view == 1 ? "curve" : "timeline") << "\"";
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

std::string CameraTimelineHud::BuildCurveJson() {
	CameraPath& cp = CameraPathRef();
	const int N = 48;
	double t0 = m_viewT0, t1 = m_viewT1;
	if (t1 <= t0) t1 = t0 + 1.0;

	double vals[7][48];
	bool ok[7][48];
	double lo[7], hi[7];
	bool any[7];
	for (int c = 0; c < 7; ++c) { lo[c] = 1e30; hi[c] = -1e30; any[c] = false; }

	for (int i = 0; i < N; ++i) {
		double tick = t0 + (t1 - t0) * ((double)i / (double)(N - 1));
		double pose[7];
		bool v = cp.EvalPoseAtTick(tick, pose);
		for (int c = 0; c < 7; ++c) {
			ok[c][i] = v;
			if (v) {
				vals[c][i] = pose[c];
				if (pose[c] < lo[c]) lo[c] = pose[c];
				if (pose[c] > hi[c]) hi[c] = pose[c];
				any[c] = true;
			} else {
				vals[c][i] = 0.0;
			}
		}
	}

	static const char* names[7] = { "X", "Y", "Z", "Pitch", "Yaw", "Tilt", "FOV" };
	std::ostringstream o;
	o << "{";
	o << "\"t0\":" << (long long)(t0 + 0.5) << ",\"t1\":" << (long long)(t1 + 0.5) << ",\"n\":" << N;
	o << ",\"lanes\":[";
	for (int c = 0; c < 7; ++c) {
		if (c) o << ",";
		double mn = any[c] ? lo[c] : 0.0;
		double mx = any[c] ? hi[c] : 1.0;
		double span = mx - mn;
		if (span < 1e-6) span = 1.0;
		o << "{\"name\":\"" << names[c] << "\",\"min\":" << r2(mn) << ",\"max\":" << r2(mx) << ",\"pts\":[";
		for (int i = 0; i < N; ++i) {
			if (i) o << ",";
			if (!ok[c][i]) o << "null";
			else o << r3((vals[c][i] - mn) / span);
		}
		o << "]}";
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

	// Keep the curve window fitted to the path (until the user zooms/pans).
	{
		const std::vector<CamMarker>& mk = CameraPathRef().Markers();
		int tickMin = mk.empty() ? 0 : mk.front().tick;
		int tickMax = mk.empty() ? 0 : mk.back().tick;
		EnsureZoomWindow(tickMin, tickMax);
	}

	bool changed = false;

	std::string state = BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
		changed = true;
	}

	// Heavy curve samples only while the curve view is actually shown.
	if (m_visible && m_view == 1) {
		std::string body = BuildCurveJson();
		if (body != m_lastCurveBody) { ++m_curveRev; m_lastCurveBody = body; }
		std::ostringstream cj;
		cj << "{\"rev\":" << m_curveRev << "," << body.substr(1); // splice rev in front
		std::string curve = cj.str();
		if (curve != m_lastCurveJson) {
			m_bridge.SetAttributeString(m_root, m_symCurve, curve.c_str());
			m_lastCurveJson = curve;
			changed = true;
		}
	}

	if (changed)
		m_bridge.RunScript("$.CamTimeline && $.CamTimeline.render();");
}

} // namespace Filmmaker
