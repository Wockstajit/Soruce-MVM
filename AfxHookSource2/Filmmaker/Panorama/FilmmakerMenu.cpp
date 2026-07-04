#include "FilmmakerMenu.h"

#include "PanoramaFindPanel.h"
#include "FilmmakerGuiJs.h"
#include "../Filmmaker.h"
#include "../Demo/DemoLibrary.h"

#include "../../DeathMsg.h" // CS2::PanoramaUIPanel offsets

#include <cstring>
#include <cstdlib>
#include <sstream>

namespace Filmmaker {

void* FilmmakerMenu::FindRootPanel() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx)
		return nullptr;
	return FindChildById(ctx, "FilmmakerMenuTab");
}

void FilmmakerMenu::BuildUi() {
	m_buildAttempted = true;
	if (!m_bridge.RunScript(kFilmmakerGuiJs))
		return;

	m_rootPanel = FindRootPanel();
	m_symCmd = m_bridge.MakeSymbol("cmd");
	m_symDemos = m_bridge.MakeSymbol("demos");
	m_built = (m_rootPanel != nullptr);
	m_lastVersion = (uint64_t)-1; // force a data push

	// Intentionally do NOT pin the context to our page: $.Filmmaker lives in the
	// (stable) main-menu JS context, and keeping that context lets FindRootPanel
	// re-locate the tab every frame (it may be shown/hidden/recreated by CS2's
	// tab system once it is parented under #JsMainMenuContent).
}

void FilmmakerMenu::PushData(DemoLibrary& library) {
	if (!m_rootPanel)
		m_rootPanel = FindRootPanel();
	if (!m_rootPanel || m_symDemos < 0)
		return;

	const std::string json = library.BuildJson();
	m_bridge.SetAttributeString(m_rootPanel, m_symDemos, json.c_str());
	m_bridge.RunScript("$.Filmmaker && $.Filmmaker.render();");
}

void FilmmakerMenu::PollCommands(DemoLibrary& library) {
	if (!m_rootPanel || m_symCmd < 0)
		return;

	std::string queued = m_bridge.GetAttributeString(m_rootPanel, m_symCmd, "");
	if (queued.empty())
		return;
	m_bridge.SetAttributeString(m_rootPanel, m_symCmd, "");

	std::istringstream iss(queued);
	std::string line;
	while (std::getline(iss, line)) {
		if (line.empty())
			continue;
		if (line == "refresh") {
			RequestRescan();
		} else if (line == "addfolder") {
			RequestAddFolder();
		} else if (line.rfind("watch ", 0) == 0) {
			Watch((size_t)std::atoi(line.c_str() + 6));
		} else if (line.rfind("rounds ", 0) == 0) {
			library.EnsureRounds((size_t)std::atoi(line.c_str() + 7));
		}
	}
}

void FilmmakerMenu::RunFrame(DemoLibrary& library) {
	m_bridge.Init();

	// Drain any queued ui_eval scripts (live REPL). Runs on the main thread here,
	// independent of whether our tab is built, as long as the bridge can run JS.
	if (!m_evalQueue.empty() && m_bridge.CanRunScript() && m_bridge.ContextPanel()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	// Build the UI once, when the bridge is configured (engine + RunScript +
	// context). Only one attempt per context availability, to avoid per-frame
	// re-injection (which would spam). RequestRebuild() resets this.
	if (!m_built) {
		if (!m_buildAttempted && m_bridge.CanRunScript() && m_bridge.ContextPanel())
			BuildUi();
		if (!m_built)
			return; // nothing to drive yet (safe)
	}

	// Re-locate the tab every frame: it can be shown/hidden by the tab system or
	// dropped on a menu reload. A stale cached pointer would be a use-after-free,
	// so always re-find. If it has truly vanished, rebuild it next frame.
	m_rootPanel = FindRootPanel();
	if (!m_rootPanel) {
		m_built = false;
		m_buildAttempted = false;
		return;
	}

	// Push fresh scan results when the library version changes.
	const uint64_t version = library.Version();
	if (version != m_lastVersion) {
		PushData(library);
		m_lastVersion = version;
	}

	PollCommands(library);

	// Show the page on request (console "mirv_filmmaker ui").
	if (m_showRequested) {
		m_showRequested = false;
		m_bridge.RunScript("$.Filmmaker && $.Filmmaker.show();");
	}
}

} // namespace Filmmaker
