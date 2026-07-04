#pragma once

// Thin wrapper around CS2's Panorama UI engine for the filmmaker menu.
//
// Reuses addresses already resolved in DeathMsg.cpp:
//   - the UI engine pointer,
//   - get/setAttributeString + makeSymbol,
//   - CUIEngine::RunScript (function resolved from panorama.dll), and
//   - the in-game HUD panel (used as the RunScript context to create the UI).
//
// All calls are null-guarded: if RunScript did not resolve, every method is a
// safe no-op and the console-driven demo browser keeps working.

#include <cstdint>
#include <string>

namespace Filmmaker {

// ===========================================================================
// CANONICAL PANORAMA Z-LAYER MAP (single source of truth)
//
// Every in-game filmmaker HUD root is created as a child of the SAME context panel
// (AfxHookSource2_GetPanoramaHudPanel()), so all of these z-index values compete in
// ONE stacking context. Keep them DISTINCT and in this documented order. The literal
// values live in each *Js.h root-creation line; if you change one, update it here too.
//
//   50  MovieHud root          (MovieHudJs.h)            - director help/status cards
//   51  FxDebugHud root        (FxDebugHudJs.h)          - FX wisp-swap debug squares (opt-in)
//   53  CameraEditor root      (CameraEditorJs.h)        - editor chrome/backdrop (below timeline)
//   55  CameraTimeline root    (CameraTimelineJs.h)      - timeline bar (sits on the editor backdrop)
//   60  MarkerHud root         (MarkerHudJs.h)           - BO2-style marker-edit menu
//   65  GraphEditor root       (GraphEditorJs.h)         - experimental graph overlay
//   220 CamEditorConfirmRoot   (CameraEditorJs.h)        - "clear all" confirm modal (above ALL HUD roots;
//                                                          as a child of the z53 root it hid behind z55/z65)
//   221 CamEditorSettingsRoot  (CameraEditorJs.h)        - gear settings fly-out (same reason)
//
// In-root local layers (children, scoped to their own root - do NOT collide cross-root):
//   150 CameraEditor debug overlay,
//   100 FilmmakerGui search popup (main-menu context, separate panel).
// ===========================================================================

class PanoramaBridge {
public:
	// Grabs the engine + RunScript pointers and (in auto mode) the HUD context.
	void Init();

	bool HasEngine() const { return m_engine != nullptr && *m_engine != nullptr; }
	bool HasRunScript() const { return m_runScript != nullptr; }
	bool CanRunScript() const { return HasEngine() && HasRunScript(); }

	// Pins the context panel that scripts run inside (disables HUD auto-context).
	void SetContextPanel(void* uiPanel) { m_contextPanel = uiPanel; m_autoContext = false; }
	void* ContextPanel() const { return m_contextPanel; }

	// Runs Panorama JavaScript in the context panel. No-op unless CanRunScript()
	// and a context panel are set.
	bool RunScript(const std::string& source);

	// Attribute IO on a CUIPanel* (reuses the resolved get/setAttributeString).
	std::string GetAttributeString(void* uiPanel, short symbol, const char* defaultValue = "") const;
	void SetAttributeString(void* uiPanel, short symbol, const char* value) const;

	// Creates / interns a Panorama symbol by name (reuses the resolved makeSymbol).
	short MakeSymbol(const char* name) const;

private:
	unsigned char** m_engine = nullptr;     // AfxHookSource2_GetPanoramaUiEngine()
	void* m_runScript = nullptr;            // AfxHookSource2_GetPanoramaRunScript()
	void* m_contextPanel = nullptr;
	bool m_autoContext = true;              // when set, follow the HUD panel
	uint64_t m_line = 0;                    // non-zero -> RunScript skips caching
};

} // namespace Filmmaker
