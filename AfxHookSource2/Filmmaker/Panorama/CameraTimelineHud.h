#pragma once

// CameraTimelineHud: a native-CS2-styled camera TIMELINE that swaps in place to a
// multi-lane CURVE EDITOR (IWXMVM's two-pane editor, rendered in Panorama). One
// panel, two views.
//
// Mirrors the proven MovieHud / MarkerHud bridge pattern: a PanoramaBridge pinned
// to the in-game HUD panel, the panel built ONCE from embedded JS, and a small
// state JSON pushed each frame. JS buttons / the scrubber emit
// "mirv_filmmaker camtl ..." console commands (the same back-end as the hotkeys).
//
// Two attributes are pushed:
//   * "state" - light, every frame: view, playhead tick/time, marker list, etc.
//   * "curve" - heavy, only when it changes: per-lane normalized polyline samples
//               of CameraPath::EvalPoseAtTick across the visible (zoomed) range.
// The JS relayouts the (expensive) polylines/diamonds only when the curve "rev"
// changes, and updates the playhead + readouts every frame.

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

	void SetView(int view) { m_view = (view != 0) ? 1 : 0; } // 0 timeline, 1 curve
	void ToggleView() { m_view ^= 1; }
	int View() const { return m_view; }

	// Curve-editor horizontal zoom/pan (operates on the visible tick window).
	void ZoomIn();
	void ZoomOut();
	void ZoomReset();
	void Pan(int dir); // dir<0 left, dir>0 right

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();
	std::string BuildCurveJson(); // includes a leading rev that the JS gates relayout on
	void EnsureZoomWindow(int tickMin, int tickMax); // default/clamp the visible window

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #CamTimelineRoot
	short m_symState = -1;
	short m_symCurve = -1;
	bool m_built = false;
	bool m_visible = false; // hidden until 'camtl open'
	bool m_cursor = false;  // global freecam cursor mode (G toggles; never opens the editor)
	bool m_editorHosted = false; // true while hosted inside Camera Editor Mode
	int m_view = 0;         // 0 timeline, 1 curve

	// Visible tick window for the curve editor (also the timeline scrub range).
	double m_viewT0 = 0.0, m_viewT1 = 1.0;
	bool m_zoomInit = false;
	bool m_userZoomed = false; // true once the user zoomed/panned (stop auto-fitting)
	int m_lastTickMin = 0, m_lastTickMax = 0;

	int m_curveRev = 0;          // bumped when the curve JSON content changes
	std::string m_lastState;
	std::string m_lastCurveBody; // curve JSON without the rev (to detect real changes)
	std::string m_lastCurveJson; // full pushed curve JSON
	std::vector<std::string> m_evalQueue;
};

CameraTimelineHud& CameraTimelineHudRef();

} // namespace Filmmaker
