#pragma once

// GraphEditorExperimentHud: the EXPERIMENTAL After-Effects-style camera graph editor.
//
// This is an OPT-IN, fully isolated overlay: default OFF, toggled by the "Experiment"
// button (or `mirv_filmmaker grapheditor toggle`). It mirrors the proven CameraEditorHud /
// CameraTimelineHud bridge pattern (PanoramaBridge pinned to the in-game HUD panel, a panel
// built ONCE from embedded JS, a small state JSON pushed each frame, JS issues console
// commands). It owns its own GraphExpModel (per-channel keyframes + Bezier handles) seeded
// read-only from the stable camera path, and it NEVER mutates the stable
// CameraPath/CamMarkers — turning it off (or deleting these files) leaves the regular editor
// exactly as it was.
//
// Live-drive: while enabled + driving, each frame it samples a pose from its own curves at
// the playhead tick and pushes it via CameraBridge_SetCameraPose (the same per-frame pose
// push the stable path uses), and reports OwnsView() so CS2's demo-view-override yields.
//
//   C++ -> JS : attribute "state" (channels/keyframes/handles/selection + streamed mouse),
//               then $.GraphExp.render().
//   JS  -> C++: "mirv_filmmaker grapheditor ..." console commands.

#include "PanoramaBridge.h"
#include "../Movie/GraphExpModel.h"

#include <string>
#include <vector>

namespace Filmmaker {

class GraphEditorExperimentHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame (after CameraEditor).
	void RunFrame();

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

	void SetDrive(bool v) { m_drive = v; }
	void ToggleDrive() { m_drive = !m_drive; }
	bool Drive() const { return m_drive; }

	// View ownership while live-driving (read by main.cpp's setup-view hook, OR'd with the
	// stable path's owner) + cursor routing (read by MovieMode's click-consume / suspend).
	bool OwnsView() const;
	bool WantsCursor() const { return m_enabled; }

	// --- command surface (called from FilmmakerCommand's "grapheditor" dispatch) ---
	void CmdChannel(int ch, int op);          // op: 0 hide,1 show,2 solo,3 unsolo
	void CmdSelect(int ch, int id, bool add);
	void CmdSelectAll();
	void CmdSelectSet(const char* csv);       // "ch:id,ch:id,..." (drag-select result)
	void CmdSelectClear();
	void CmdEditBegin();                      // undo snapshot for the upcoming gesture
	void CmdMoveSelectedBy(double dTick, double dValue);
	void CmdMoveKeyAbs(int ch, int id, double tick, double value);
	void CmdSetValue(int ch, int id, double value);
	void CmdAddKey(int ch, double tick, double value);
	void CmdDeleteKey(int ch, int id);
	void CmdDeleteSelected();
	void CmdSetHandle(int ch, int id, int side, double tx, double dv, bool reflect);
	void CmdClearHandles(int ch, int id);
	void CmdEase(int mode, bool selectedOnly); // 0 ease-in, 1 ease-out, 2 ease-in/out (selection or all)
	void CmdSetInterp(bool smooth, bool selectedOnly); // graph-wide (or selection) Smooth / Linear toggle
	void CmdPlayhead(double tick, bool seek);
	void CmdUndo();
	void CmdRedo();
	void CmdReseed();                         // re-seed from the current stable path

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	void OnEnter();   // seed, force cursor, enable free cam, stop stable scrub
	void OnExit();    // stop driving + tear down
	std::string BuildStateJson();
	void DriveCameraThisFrame();
	void BumpRev() { ++m_rev; }
	double PlayheadTick() const; // m_scrubbing ? m_scrubTick : current demo tick

	PanoramaBridge m_bridge;
	GraphExpModel m_model;

	void* m_hudPanel = nullptr;
	void* m_root = nullptr;       // #GraphExpRoot
	short m_symState = -1;
	bool m_built = false;
	bool m_enabled = false;       // graphEditorExperimentEnabled (default OFF)
	bool m_drive = true;          // live-drive the camera from the experimental curves
	bool m_wasEnabled = false;    // enter/exit edge detection

	bool m_scrubbing = false;     // true while the ruler/playhead is being dragged
	double m_scrubTick = 0.0;

	// Visible tick window (zoom/pan); auto-fit to the keyframe range until the user pans.
	double m_viewT0 = 0.0, m_viewT1 = 1.0;
	bool m_viewInit = false;

	unsigned m_rev = 0;           // bumped on every model edit (JS gates curve rebuild on it)
	std::string m_lastState;
	std::vector<std::string> m_evalQueue;
};

GraphEditorExperimentHud& GraphEditorExperimentHudRef();

} // namespace Filmmaker
