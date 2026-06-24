#pragma once

// CameraTimelineHud: a native-CS2-styled camera TIMELINE / scrub bar.
//
// Mirrors the proven MovieHud / MarkerHud bridge pattern: a PanoramaBridge pinned
// to the in-game HUD panel, the panel built ONCE from embedded JS, and a small
// state JSON pushed each frame. JS buttons / the scrubber emit
// "mirv_filmmaker camtl ..." console commands (the same back-end as the hotkeys).
//
// The "state" attribute is pushed with playhead tick/time, marker list, and
// transport settings. The old embedded curve view was retired in favor of
// GraphEditorExperimentHud / GraphEditorJs.

#include "PanoramaBridge.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker {

class CameraTimelineHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

	void SetVisible(bool v) { m_visible = v; }
	void Toggle() { m_visible = !m_visible; }
	bool Visible() const { return m_visible; }

	// UI-mouse mode: MirvInput is suspended so the OS cursor shows. While the camera
	// timeline / curve editor is open standalone this is forced on; otherwise it is the
	// regular native-demo-bar mouse toggle.
	//
	// EDITOR-HOSTED exception: inside Camera Editor Mode the cursor is NOT forced -- the
	// user must still be able to press G to drop into GAME mouse and fly the free cam to
	// frame a shot. So when hosted the cursor follows the toggle (m_cursor) only.
	void SetCursor(bool v) { m_cursor = v; }
	void ToggleCursor() { m_cursor = !m_cursor; }
	bool Cursor() const { return m_editorHosted ? m_cursor : (m_visible || m_cursor); }
	bool CursorForced() const { return m_editorHosted ? false : m_visible; }

	// Camera Editor Mode hosting: CameraEditorHud forces this panel open + docked and
	// owns the gameplay-HUD hide (driven by the "hosted" state flag this pushes to JS).
	void SetEditorHosted(bool v) { m_editorHosted = v; }
	bool EditorHosted() const { return m_editorHosted; }

	void SetView(int) { }
	void ToggleView() { }
	int View() const { return 0; }

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #CamTimelineRoot
	short m_symState = -1;
	bool m_built = false;
	bool m_visible = false; // hidden until 'camtl open'
	bool m_cursor = false;  // global freecam cursor mode (G toggles; never opens the editor)
	bool m_editorHosted = false; // true while hosted inside Camera Editor Mode
	std::string m_lastState;
	std::vector<std::string> m_evalQueue;
};

CameraTimelineHud& CameraTimelineHudRef();

} // namespace Filmmaker
