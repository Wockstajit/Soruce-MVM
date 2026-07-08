#include "ConfigHud.h"

#include "PanoramaFindPanel.h"

#include "ConfigHudJs.h"
#include "CameraTimelineHud.h"
#include "MovieHud.h" // hidden while Config is open (F8 director card must not sit in the preview)
#include "../Movie/ViewFx.h" // MODIFIERS: strafe roll + weapon sway levels
#include "../Movie/BodyCam.h" // MODIFIERS: chest-cam preset state
#include "../Movie/ActionCam.h" // MODIFIERS: head-cam preset + fisheye lens state
#include "../Movie/ParticleFx.h" // EFFECTS: per-category particle toggles
#include "../Movie/RagdollFx.h" // EFFECTS: improved player-ragdoll toggle
#include "../Filmmaker.h" // CameraEditor_Active/Set (mutual exclusion), CameraEditor_HudViewName

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../ViewportScaler.h" // AfxViewportScaler scaled-preview bridge
#include "../../../shared/AfxConsole.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			// A PAUSED demo is still an active demo (see CameraEditorHud.cpp for the history:
			// IsPlayingDemo() alone flips false on pause and would force-exit the panel).
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

} // namespace

ConfigHud& ConfigHudRef() {
	static ConfigHud s_instance;
	return s_instance;
}

bool ConfigHud_Enabled() { return ConfigHudRef().Enabled(); }

bool ConfigHud_ScaledHud(float& x0, float& y0, float& x1, float& y1) {
	return ConfigHudRef().ScaledHud(x0, y0, x1, y1);
}

void* ConfigHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "ConfigHudRoot");
}

bool ConfigHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kConfigHudJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symPreviewRect = m_bridge.MakeSymbol("previewrect");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void ConfigHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#ConfigHudRoot'); if(e) e.DeleteAsync(0);"
			" var s=$('#ConfigSettingsRoot'); if(s) s.DeleteAsync(0); $.ConfigHud=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
}

void ConfigHud::OnEnter() {
	// Start in UI-cursor mode so the panel is immediately clickable (same as the Camera Editor).
	CameraTimelineHudRef().SetCursor(true);
	// Hide the floating movie-director card (the F8 CONTROLS panel) -- it would otherwise sit
	// inside the scaled preview. Restored to its previous visibility on exit (editor parity).
	m_prevMovieHud = MovieHudRef().Visible();
	MovieHudRef().SetVisible(false);
}

void ConfigHud::OnExit() {
	// HudView is shared, persistent state -- CameraTimelineJs automatically restores the full
	// game HUD when neither the editor nor this panel is open, so nothing to reset here.
	MovieHudRef().SetVisible(m_prevMovieHud);
	// Drop any pending scaled-preview blit so the next full-screen frame renders normally.
	m_previewValid = false;
	AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
	Teardown();
}

// Reads the "previewrect" fractions the Config JS published this frame and forwards them to
// the render-layer scaler (same contract as CameraEditorHud::UpdateScaleRequest; the blit
// only actually runs engine-side when not recording).
void ConfigHud::UpdateScaleRequest() {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool valid = false;
	if (m_root) {
		std::string pr = m_bridge.GetAttributeString(m_root, m_symPreviewRect, "");
		std::istringstream is(pr);
		const bool parsed = (bool)(is >> x0 >> y0 >> x1 >> y1);
		valid = parsed && (x1 - x0 > 0.01f) && (y1 - y0 > 0.01f)
			&& x0 >= 0 && y0 >= 0 && x1 <= 1.0001f && y1 <= 1.0001f;
	}
	m_previewValid = valid;
	if (valid) { m_previewX0 = x0; m_previewY0 = y0; m_previewX1 = x1; m_previewY1 = y1; }
	AfxViewportScaler::SetRequest(valid, x0, y0, x1, y1);
}

std::string ConfigHud::BuildStateJson() {
	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_enabled ? "true" : "false");
	o << ",\"cursor\":" << (CameraTimelineHudRef().Cursor() ? "true" : "false");
	o << ",\"hudView\":\"" << CameraEditor_HudViewName() << "\"";
	// MODIFIERS section: camera "feel" toggles owned by ViewFx.h / BodyCam.h, not this file --
	// this just reads their state each frame the same way the rest of the panel does.
	o << ",\"rollPct\":" << ViewFxRef().RollIntensity();
	o << ",\"bobPct\":" << ViewFxRef().BobIntensity();
	o << ",\"swayPct\":" << ViewFxRef().SwayIntensity();
	o << ",\"deadzonePct\":" << ViewFxRef().DeadzoneIntensity();
	o << ",\"bodyCam\":" << (BodyCam_Active() ? "true" : "false");
	o << ",\"actionCam\":" << (ActionCam_Active() ? "true" : "false");
	o << ",\"acFisheye\":" << (ActionCam_FisheyeEnabled() ? "true" : "false");
	// EFFECTS section: particle-effect toggles owned by ParticleFx.h -- read-only mirror here,
	// same contract as the MODIFIERS block above (JS writes back via console commands).
	{
		ParticleFx& fx = ParticleFxRef();
		o << ",\"fxOn\":" << (fx.Enabled() ? "true" : "false");
		o << ",\"fxReady\":" << (fx.Installed() ? "true" : "false");
		o << ",\"fxMoneyshot\":" << (fx.MoneyHeadshot() ? "true" : "false");
		o << ",\"ragdollOn\":" << (RagdollFx_Enabled() ? "true" : "false");
		for (int c = 0; c < kFxCategoryCount; ++c)
			o << ",\"fx_" << FxCategoryKey((FxCategory)c) << "\":\""
			  << FxModeName(fx.Mode((FxCategory)c)) << "\"";
	}
	o << "}";
	return o.str();
}

void ConfigHud::RunFrame() {
	m_bridge.Init();

	// Mutual exclusion: the Camera Editor is the bigger workspace -- if it comes up while the
	// Config panel is open, the Config panel yields. (Opening Config closes the editor in the
	// command handler, so this only covers the editor-opened-later direction.)
	if (m_enabled && CameraEditor_Active())
		m_enabled = false;

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;

	// Demo not playing (or HUD gone): force-exit cleanly so we never leave a stale panel.
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

	if (!BuildIfNeeded())
		return;

	m_root = FindRoot();
	if (!m_root) {
		m_built = false;
		return;
	}

	std::string state = BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	// Always render while enabled so the preview layout re-asserts each frame; render()
	// (re)publishes the "previewrect" attribute the D3D blit reads below.
	m_bridge.RunScript("$.ConfigHud && $.ConfigHud.render();");

	// TRUE scaled preview: forward the rect render() just published to the viewport scaler.
	UpdateScaleRequest();
}

void ConfigHud_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[confighud][state] %s\n", ConfigHudRef().DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "open") || 0 == _stricmp(arg, "1")) ConfigHudRef().SetEnabled(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "close") || 0 == _stricmp(arg, "0")) ConfigHudRef().SetEnabled(false);
	else ConfigHudRef().Toggle();
	// Opening Config closes the Camera Editor (they draw the same right-side column area).
	if (ConfigHudRef().Enabled() && CameraEditor_Active())
		CameraEditor_Set(false);
	advancedfx::Message("mirv_filmmaker: config panel %s (must be in a demo).\n",
		ConfigHudRef().Enabled() ? "ON" : "off");
}

} // namespace Filmmaker
