#pragma once

// MovieMode: the in-demo "camera director". It owns the camera MODE state
// machine (Default first-person / ThirdPerson / FreeCam) and the X-ray / cursor
// toggles, and translates captured input (mouse wheel, LMB/RMB, X, G) into
// actions.
//
// Threading: input taps (On*) run on the WndProc/input thread and only ENQUEUE
// work (mode changes + console commands) under a mutex. FlushActions() runs on
// the main/UI thread (from Filmmaker::RunMainThreadFrame) and is the only place
// that touches the engine (ExecuteClientCmd) or the camera system, mirroring how
// the rest of the Filmmaker module keeps engine work on the main thread.
//
// It reuses CS2's own spectator commands (spec_mode / spec_next / spec_prev /
// spec_show_xray) rather than hand-rolling entity/observer manipulation, and
// drives the free camera through CameraBridge_SetFreeCamEnabled.

#include <mutex>
#include <vector>
#include <string>

namespace Filmmaker {

class MovieMode {
public:
	enum class Mode { Default = 0, ThirdPerson = 1, FreeCam = 2 };

	// --- input taps (WndProc / input thread). Return true if consumed. ---
	bool OnKey(int vkey, bool down);
	bool OnMouseButton(int button, bool down); // 0 = left, 1 = right
	bool OnMouseWheel(int delta, bool shiftDown);

	// --- main thread ---
	void FlushActions();

	// --- state for the HUD (thread-safe reads) ---
	Mode GetMode();
	bool GetXray();
	bool GetCursor();
	const char* ModeName();

	// Programmatic mode set (e.g. when the user toggles free cam via console).
	void SyncFromFreeCam(bool freeCamEnabled);

private:
	void ApplyMode(Mode m);                 // main thread only
	void EnqueueCmd(const std::string& c);  // any thread (locks)

	std::mutex m_mutex;
	Mode m_mode = Mode::Default;
	bool m_xray = false;
	// Retained for the HUD "cursor" state field only. The director no longer
	// intercepts clicks at all (CS2's own spectator + native demo UI handle
	// them), so this stays true and is informational.
	bool m_cursor = true;
	bool m_controlDown = false; // input-thread modifier state for Ctrl+Z curve undo
	bool m_spaceDown = false;   // input-thread Space state; swallows OS auto-repeat so transport toggles once per press
	bool m_modeDirty = false;
	Mode m_pendingMode = Mode::Default;
	std::vector<std::string> m_cmdQueue;
};

// Singleton accessor (constructed on first use).
MovieMode& MovieModeRef();

} // namespace Filmmaker
