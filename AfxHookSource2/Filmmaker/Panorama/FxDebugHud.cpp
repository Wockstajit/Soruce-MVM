#include "FxDebugHud.h"

#include "FxDebugHudJs.h"
#include "../Movie/ParticleFx.h"

#include "../../DeathMsg.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

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

// Per-shot blink: each swap event increments the class counter, and the square is lit on
// ODD counts -- so every bullet visibly toggles it 1-0-1-0 (user request 2026-07-04; the
// old 4s hold read as "stuck on" during sustained fire and lingered across weapon
// switches). Parity is timescale-proof: it flips per EVENT, not per wall-clock window.
// The staleness cutoff darkens a square left odd once firing stops / the weapon changes.
bool LitMs(unsigned long long fireMs, unsigned long long nowMs, unsigned long long count) {
	return fireMs != 0 && (nowMs - fireMs) < 1500 && (count & 1) != 0;
}

} // namespace

FxDebugHud& FxDebugHudRef() {
	static FxDebugHud s_instance;
	return s_instance;
}

void* FxDebugHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx)
		return nullptr;
	return FindChildById(ctx, "FxDebugHudRoot");
}

bool FxDebugHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kFxDebugHudJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastJson.clear();
	return m_built;
}

void FxDebugHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#FxDebugHudRoot'); if(e) e.DeleteAsync(0); $.FxDebugHud=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastJson.clear();
}

std::string FxDebugHud::BuildStateJson() {
	FxDebugHudState st;
	ParticleFx_GetDebugHudState(st);

	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (st.enabled ? "true" : "false");
	o << ",\"litMuzzle\":" << (LitMs(st.muzzleMs, st.nowMs, st.muzzleN) ? "true" : "false");
	o << ",\"litTracer\":" << (LitMs(st.tracerMs, st.nowMs, st.tracerN) ? "true" : "false");
	o << ",\"litOnSmoke\":" << (LitMs(st.onSmokeMs, st.nowMs, st.onSmokeN) ? "true" : "false");
	o << ",\"litOnWisp\":" << (LitMs(st.onWispMs, st.nowMs, st.onWispN) ? "true" : "false");
	o << ",\"litModSmoke\":" << (LitMs(st.modSmokeMs, st.nowMs, st.modSmokeN) ? "true" : "false");
	o << ",\"litModWisp\":" << (LitMs(st.modWispMs, st.nowMs, st.modWispN) ? "true" : "false");
	o << "}";
	return o.str();
}

void FxDebugHud::RunFrame() {
	if (!ParticleFx_DebugHudEnabled()) {
		Teardown();
		return;
	}

	m_bridge.Init();

	bool playingDemo = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			playingDemo = pDemo->IsPlayingDemo();
	}

	unsigned char* hud = playingDemo ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) {
		Teardown();
		return;
	}
	if (hud != m_hudPanel) {
		m_hudPanel = hud;
		m_built = false;
	}
	m_bridge.SetContextPanel(hud);

	if (!BuildIfNeeded())
		return;

	m_root = FindRoot();
	if (!m_root) { m_built = false; return; }

	std::string json = BuildStateJson();
	if (json != m_lastJson) {
		m_bridge.SetAttributeString(m_root, m_symState, json.c_str());
		m_lastJson = json;
	}
	// Re-render every frame: LitMs fade depends on wall clock and positionBelowDirector()
	// needs layout passes even when the JSON string is unchanged during the 4s hold.
	m_bridge.RunScript("$.FxDebugHud && $.FxDebugHud.render();");
}

} // namespace Filmmaker
