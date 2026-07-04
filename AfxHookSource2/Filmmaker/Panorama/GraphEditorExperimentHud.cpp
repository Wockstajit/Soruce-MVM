#include "GraphEditorExperimentHud.h"

#include "PanoramaFindPanel.h"

#include "GraphEditorJs.h"
#include "CameraTimelineHud.h"
#include "../Filmmaker.h"
#include "../Movie/CameraPath.h"
#include "../Movie/CameraBridge.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../MirvTime.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <cmath>
#include <utility>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


double r2(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0; // NaN/inf -> keep JSON valid
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0;
}
double r3(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0;
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 1000.0 + 0.5) / 1000.0;
}

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

static const char* kChNames[7] = { "Pos X", "Pos Y", "Pos Z", "Pitch", "Yaw", "Roll", "FOV" };
static const char* kChColors[7] = { "#ff6b6b", "#7be07b", "#6ba8ff", "#ffcf5a", "#c08aff", "#5ad0ff", "#ff9a5a" };
static const double kChMinSpan[7] = { 64.0, 64.0, 64.0, 20.0, 20.0, 20.0, 10.0 };

} // namespace

GraphEditorExperimentHud& GraphEditorExperimentHudRef() {
	static GraphEditorExperimentHud s_instance;
	return s_instance;
}

void* GraphEditorExperimentHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "GraphExpRoot");
}

bool GraphEditorExperimentHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kGraphEditorJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void GraphEditorExperimentHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#GraphExpRoot'); if(e) e.DeleteAsync(0); $.GraphExp=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
}

void GraphEditorExperimentHud::OnEnter() {
	// Seed our isolated model from the stable path (read-only) ONLY if we don't already have one.
	// Enabling fires every time the bottom panel flips back from the timeline; re-seeding there
	// would wipe the user's graph edits. The model is cleared when the demo changes (RunFrame),
	// so a genuinely new session still seeds fresh.
	if (m_model.TotalKeys() == 0)
		m_model.SeedFromMarkers(CameraPathRef().Markers());
	m_viewInit = false;
	m_scrubbing = false;
	// Camera Editor's G key may have switched to game/free-look before this overlay
	// finished entering. Respect that cursor mode instead of re-taking the camera.
	m_drive = CameraEditor_Active() ? CameraTimelineHudRef().Cursor() : true;
	BumpRev();

	// The UI cursor is shown via our GraphEditorExperiment_WantsCursor() hook into MovieMode's
	// suspend/consume chain (so we do NOT poke the timeline's cursor flag, which would otherwise
	// stay stuck on after a standalone exit). Do not force free cam here: opening/clicking an empty
	// graph should leave the viewer in its current mode. DriveCameraThisFrame enables it only once
	// there are graph keys to sample, and the editor command path focuses an existing camera marker.
	if (m_model.TotalKeys() > 0)
		CameraPathRef().StopScrub();
}

void GraphEditorExperimentHud::OnExit() {
	m_scrubbing = false;
	Teardown();
	// We intentionally leave free cam as-is: the regular Camera Editor (which usually hosts the
	// Experiment button) manages it. Live-drive simply stops because m_enabled is now false.
}

double GraphEditorExperimentHud::PlayheadTick() const {
	if (m_scrubbing) return m_scrubTick;
	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	return (double)curTick;
}

bool GraphEditorExperimentHud::OwnsView() const {
	return m_enabled && m_drive && m_model.TotalKeys() > 0 && CameraBridge_GetFreeCamEnabled();
}

void GraphEditorExperimentHud::DriveCameraThisFrame() {
	if (!m_enabled || !m_drive || m_model.TotalKeys() == 0)
		return;
	double origin[3] = { 0,0,0 }, angles[3] = { 0,0,0 }, fov = 90.0;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	const double defaults[7] = { origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov };
	double pose[7];
	m_model.SamplePose(PlayheadTick(), defaults, pose);
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
}

std::string GraphEditorExperimentHud::BuildStateJson() {
	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);

	// Auto-fit the visible window to the keyframe range until the user pans/zooms (JS-side).
	double mn = m_model.MinTick(), mx = m_model.MaxTick();
	if (mx <= mn) mx = mn + 1.0;
	if (!m_viewInit) {
		double pad = (mx - mn) * 0.04 + 1.0;
		m_viewT0 = mn - pad; m_viewT1 = mx + pad; m_viewInit = true;
	}

	int selCh = -1, selId = -1;
	m_model.FirstSelected(selCh, selId);

	int mxp = 0, myp = 0; bool lmb = false, rmb = false, shiftDown = false; unsigned seq = 0;
	CameraBridge_GetUiCursor(mxp, myp, lmb, rmb, shiftDown, seq);

	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_enabled ? "true" : "false");
	o << ",\"drive\":" << (m_drive ? "true" : "false");
	o << ",\"scrubbing\":" << (m_scrubbing ? "true" : "false");
	o << ",\"playhead\":" << r2(PlayheadTick());
	o << ",\"demoTick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	o << ",\"minTick\":" << r2(mn) << ",\"maxTick\":" << r2(mx);
	o << ",\"viewT0\":" << r2(m_viewT0) << ",\"viewT1\":" << r2(m_viewT1);
	o << ",\"rev\":" << m_rev;
	o << ",\"selCount\":" << m_model.SelectionCount();
	o << ",\"canUndo\":" << (m_model.CanUndo() ? "true" : "false") << ",\"canRedo\":" << (m_model.CanRedo() ? "true" : "false");
	o << ",\"selCh\":" << selCh << ",\"selId\":" << selId;
	o << ",\"mx\":" << mxp << ",\"my\":" << myp << ",\"lmb\":" << (lmb ? "true" : "false")
		<< ",\"rmb\":" << (rmb ? "true" : "false") << ",\"shift\":" << (shiftDown ? "true" : "false") << ",\"seq\":" << seq;
	o << ",\"channels\":[";
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c) {
		if (c) o << ",";
		const GraphExpModel::Channel& ch = m_model.GetChannel(c);
		// Per-channel value range (padded), for the Y mapping in the JS.
		double lo = 1e30, hi = -1e30;
		for (const auto& k : ch.keys) { if (k.value < lo) lo = k.value; if (k.value > hi) hi = k.value; } // KEYFRAME values only -- handles do NOT pull on the scale, so you can drag them past the window edge and the view stays put (no zoom-out / refit for handles)
		if (ch.keys.empty()) { lo = 0.0; hi = 1.0; }
		double center = (lo + hi) * 0.5, span = hi - lo; if (span < 1e-6) span = 1.0;
		double vspan = span * 1.5; if (vspan < kChMinSpan[c]) vspan = kChMinSpan[c];
		lo = center - vspan * 0.5; hi = center + vspan * 0.5;

		o << "{\"ch\":" << c << ",\"name\":\"" << kChNames[c] << "\",\"color\":\"" << kChColors[c] << "\"";
		o << ",\"vis\":" << (ch.visible ? "true" : "false") << ",\"solo\":" << (ch.solo ? "true" : "false");
		o << ",\"edit\":" << (m_model.IsEditable(c) ? "true" : "false");
		o << ",\"min\":" << r2(lo) << ",\"max\":" << r2(hi);
		o << ",\"keys\":[";
		for (size_t i = 0; i < ch.keys.size(); ++i) {
			if (i) o << ",";
			const GraphExpModel::Keyframe& k = ch.keys[i];
			o << "{\"id\":" << k.id << ",\"t\":" << r2(k.tick) << ",\"v\":" << r2(k.value)
				<< ",\"sel\":" << (m_model.IsSelected(c, k.id) ? "true" : "false")
				<< ",\"cl\":{\"a\":" << (k.cpLeft.active ? 1 : 0) << ",\"tx\":" << r3(k.cpLeft.tx) << ",\"dv\":" << r2(k.cpLeft.dv) << "}"
				<< ",\"cr\":{\"a\":" << (k.cpRight.active ? 1 : 0) << ",\"tx\":" << r3(k.cpRight.tx) << ",\"dv\":" << r2(k.cpRight.dv) << "}}";
		}
		o << "]}";
	}
	o << "]}";
	return o.str();
}

void GraphEditorExperimentHud::RunFrame() {
	m_bridge.Init();

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) {
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		// Demo gone: drop the model so the NEXT demo/session re-seeds fresh (OnEnter only seeds
		// an empty model, so without this it would keep showing the previous demo's curves).
		if (m_model.TotalKeys() > 0) m_model.Clear();
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

	if (!BuildIfNeeded())
		return;

	// Live REPL (grapheditor eval) in the panel context.
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
	// Layout depends on Panorama's live root measurements, which can be 0 on the
	// first frame after build. Re-render every frame so the dock corrects itself
	// once actuallayoutwidth / actuallayoutheight settle, even while paused.
	m_bridge.RunScript("$.GraphExp && $.GraphExp.render();");

	// Live-drive runs EVERY frame (independent of the render-on-change gate) so the camera
	// keeps following the curves while the demo plays.
	DriveCameraThisFrame();
}

// ---- command surface --------------------------------------------------------

void GraphEditorExperimentHud::CmdChannel(int ch, int op) {
	switch (op) {
	case 0: m_model.SetVisible(ch, false); break;
	case 1: m_model.SetVisible(ch, true); break;
	case 2: m_model.SetSolo(ch, true); break;
	case 3: m_model.SetSolo(ch, false); break;
	}
	BumpRev();
}
void GraphEditorExperimentHud::CmdSelect(int ch, int id, bool add) { m_model.Select(ch, id, add); BumpRev(); }
void GraphEditorExperimentHud::CmdSelectAll() { m_model.SelectAll(); BumpRev(); }
void GraphEditorExperimentHud::CmdSelectClear() { m_model.ClearSelection(); BumpRev(); }

void GraphEditorExperimentHud::CmdSelectSet(const char* csv) {
	m_model.ClearSelection();
	if (csv && *csv) {
		const char* p = csv;
		while (*p) {
			int ch = std::atoi(p);
			const char* colon = std::strchr(p, ':');
			if (!colon) break;
			int id = std::atoi(colon + 1);
			m_model.SelectAdd(ch, id);
			const char* comma = std::strchr(colon, ',');
			if (!comma) break;
			p = comma + 1;
		}
	}
	BumpRev();
}

void GraphEditorExperimentHud::CmdEditBegin() { m_model.BeginEdit(); }
void GraphEditorExperimentHud::CmdMoveSelectedBy(double dTick, double dValue) { m_model.MoveSelectedBy(dTick, dValue); BumpRev(); }
void GraphEditorExperimentHud::CmdMoveKeyAbs(int ch, int id, double tick, double value) { m_model.MoveKeyAbs(ch, id, tick, value); BumpRev(); }
void GraphEditorExperimentHud::CmdSetValue(int ch, int id, double value) { m_model.SetKeyValue(ch, id, value); BumpRev(); }

void GraphEditorExperimentHud::CmdAddKey(int ch, double tick, double value) {
	// No internal undo snapshot: callers that mutate per-frame (the number-field scrub, typed
	// commit) bracket the gesture with editbegin themselves, so this stays one undo step.
	int id = m_model.AddKey(ch, tick, value);
	m_model.Select(ch, id, false);
	BumpRev();
}
void GraphEditorExperimentHud::CmdDeleteKey(int ch, int id) { m_model.BeginEdit(); m_model.DeleteKey(ch, id); BumpRev(); }

void GraphEditorExperimentHud::CmdDeleteSelected() {
	m_model.BeginEdit();
	// Collect (ch,id) of selected first, then delete (deleting mutates the selection set).
	std::vector<std::pair<int, int>> doomed;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		for (const auto& k : m_model.GetChannel(c).keys)
			if (m_model.IsSelected(c, k.id)) doomed.emplace_back(c, k.id);
	for (auto& d : doomed) m_model.DeleteKey(d.first, d.second);
	m_model.ClearSelection();
	BumpRev();
}

void GraphEditorExperimentHud::CmdClear() {
	m_model.BeginEdit();
	m_model.Clear();
	m_viewInit = false;
	m_scrubbing = false;
	BumpRev();
}

void GraphEditorExperimentHud::CmdSetHandle(int ch, int id, int side, double tx, double dv, bool reflect) {
	m_model.SetHandle(ch, id, side, tx, dv, reflect); BumpRev();
}
void GraphEditorExperimentHud::CmdClearHandles(int ch, int id) { m_model.BeginEdit(); m_model.ClearHandles(ch, id); BumpRev(); }

void GraphEditorExperimentHud::CmdPlayhead(double tick, bool seek) {
	if (seek) { m_scrubbing = false; }      // release: follow the demo tick again (JS does demo_gototick)
	else { m_scrubbing = true; m_scrubTick = tick; } // drag: preview the camera at this tick (no world seek)
}

void GraphEditorExperimentHud::CmdEase(int mode, bool selectedOnly) { m_model.BeginEdit(); m_model.SetEase(mode, selectedOnly); BumpRev(); }
void GraphEditorExperimentHud::CmdSetInterp(bool smooth, bool selectedOnly) { m_model.BeginEdit(); m_model.SetInterpAll(smooth, selectedOnly); BumpRev(); }
void GraphEditorExperimentHud::CmdUndo() { m_model.Undo(); BumpRev(); }
void GraphEditorExperimentHud::CmdRedo() { m_model.Redo(); BumpRev(); }
void GraphEditorExperimentHud::CmdReseed() { m_model.SeedFromMarkers(CameraPathRef().Markers()); m_viewInit = false; BumpRev(); }

} // namespace Filmmaker
