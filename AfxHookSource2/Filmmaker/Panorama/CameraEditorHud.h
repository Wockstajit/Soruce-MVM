#pragma once

// CameraEditorHud: a dedicated "Camera Editor Mode" workspace shell rendered with
// CS2's native Panorama in the in-game HUD (CSGOHud) context.
//
// This is a PRESENTATION SHELL over the existing camera-editing back-end; it adds no
// new editing logic. While enabled it:
//   * frames the live game as a "preview" in the top-left (a right-side property
//     inspector + a bottom timeline + a dimmed letterbox make the open area read as a
//     viewport -- note the preview is a CROP of the full-screen frame, not a scaled
//     copy, since Panorama only composites overlays on top of the full game render);
//   * hosts the existing camera TIMELINE / curve editor (CameraTimelineHud) docked at
//     the bottom and the BO2 marker settings, all driven by the same
//     "mirv_filmmaker camtl ... / marker ..." console commands;
//   * surfaces selected-camera settings (FOV, roll, interpolation, segment speed,
//     easing, freeze/live, speed mode) as Slider / stepper / cycle controls that issue
//     those same commands;
//   * hides the gameplay HUD (radar/health/ammo/scoreboard/native demo bar) for a
//     clean workspace -- the actual hide is centralised in CameraTimelineHud's JS via
//     the "hosted" state flag, so a single script owns native-HUD visibility.
//
// Mirrors the proven MovieHud / CameraTimelineHud bridge pattern: a PanoramaBridge
// pinned to the in-game HUD panel, a panel built ONCE from embedded JS, and a small
// state JSON pushed each frame.
//
//   C++ -> JS : attribute "state" (camera readouts + selected-key settings), then
//               $.CamEditor.render().
//   JS  -> C++: buttons / sliders issue "mirv_filmmaker ..." console commands.

#include "PanoramaBridge.h"

#include <string>
#include <vector>

namespace Filmmaker {

class CameraEditorHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. Runs LAST so
	// its host orchestration (timeline hosting + HUD hide) wins over the other panels.
	void RunFrame();

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();
	void OnEnter(); // one-shot: host the timeline, hide MovieHud, enable freecam, select a key
	void OnExit();  // one-shot: un-host the timeline, restore MovieHud, stop scrub

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #CamEditorRoot
	short m_symState = -1;
	bool m_built = false;
	bool m_enabled = false;     // Camera Editor Mode on/off
	bool m_wasEnabled = false;  // for enter/exit edge detection
	bool m_prevMovieHud = false; // MovieHud visibility to restore on exit
	std::string m_lastState;
	std::vector<std::string> m_evalQueue;
};

CameraEditorHud& CameraEditorHudRef();

} // namespace Filmmaker
