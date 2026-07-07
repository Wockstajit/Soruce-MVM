#include "TestHud.h"

#include "PanoramaFindPanel.h"
#include "TestHudJs.h"
#include "CameraTimelineHud.h"
#include "../Movie/ParticleFx.h"
#include "../MvmTest.h"

#include "../../DeathMsg.h"
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
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

} // namespace

TestHud& TestHudRef() {
	static TestHud s_instance;
	return s_instance;
}

bool TestHud_Enabled() { return TestHudRef().Enabled(); }

void* TestHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "TestHudRoot");
}

bool TestHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kTestHudJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void TestHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#TestHudRoot'); if(e) e.DeleteAsync(0); $.TestHud=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
}

void TestHud::OnEnter() {
	CameraTimelineHudRef().SetCursor(true);
}

void TestHud::OnExit() {
	CameraTimelineHudRef().SetCursor(false);
	Teardown();
}

std::string TestHud::BuildStateJson() {
	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_enabled ? "true" : "false");
	o << ",\"cursor\":" << (CameraTimelineHudRef().Cursor() ? "true" : "false");
	{
		ParticleFx& fx = ParticleFxRef();
		o << ",\"fxOn\":" << (fx.Enabled() ? "true" : "false");
		o << ",\"fxReady\":" << (fx.Installed() ? "true" : "false");
		o << ",\"fxMoneyshot\":" << (fx.MoneyHeadshot() ? "true" : "false");
		for (int c = 0; c < kFxCategoryCount; ++c)
			o << ",\"fx_" << FxCategoryKey((FxCategory)c) << "\":\""
			  << FxModeName(fx.Mode((FxCategory)c)) << "\"";
	}
	o << "}";
	return o.str();
}

void TestHud::RunFrame() {
	m_bridge.Init();

	if (!MvmTest_Active() || PlayingDemo()) {
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		return;
	}

	unsigned char* hud = MvmTest_LevelLoaded() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) {
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

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
	m_bridge.RunScript("$.TestHud && $.TestHud.render();");
}

void TestHud_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	(void)cmd;
	if (!MvmTest_CanUseMenu()) {
		if (!PlayingDemo() && MvmTest_LevelLoaded())
			MvmTest_EnsureArmed();
		else {
			advancedfx::Warning("mvm_test: menu requires a live map (not demo playback).\n");
			return;
		}
	}
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[testhud][state] %s\n", TestHudRef().DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "open") || 0 == _stricmp(arg, "1"))
		TestHudRef().SetEnabled(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "close") || 0 == _stricmp(arg, "0"))
		TestHudRef().SetEnabled(false);
	else
		TestHudRef().Toggle();
	advancedfx::Message("mvm_test: FX menu %s (Insert toggles).\n",
		TestHudRef().Enabled() ? "ON" : "off");
}

} // namespace Filmmaker
