#pragma once

// TestHud: effects-only FX testing panel for offline live matches (mvm_test).
// Right-side overlay; no scaled preview. Toggle with Insert or `mvm_test menu`.

#include "PanoramaBridge.h"

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class TestHud {
public:
	void RunFrame();

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

	std::string DebugStateJson() { return BuildStateJson(); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();
	void OnEnter();
	void OnExit();

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr;
	void* m_root = nullptr;
	short m_symState = -1;
	bool m_built = false;
	bool m_enabled = false;
	bool m_wasEnabled = false;
	std::string m_lastState;
};

TestHud& TestHudRef();
bool TestHud_Enabled();
void TestHud_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
