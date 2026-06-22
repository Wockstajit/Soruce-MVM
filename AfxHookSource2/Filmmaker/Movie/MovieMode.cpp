#include "MovieMode.h"

#include "CameraBridge.h"
#include "CameraPath.h"
#include "../Filmmaker.h"            // CameraTimeline_Visible()
#include "../Panorama/MovieHud.h"
#include "../../MirvTime.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <sstream>

// Provided by main.cpp (same global the Filmmaker "Watch" action uses).
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

// Virtual-key codes for the director keys (avoid relying on <windows.h> macros
// for letters, which don't exist).
namespace {
	constexpr int kVK_X = 0x58;
	constexpr int kVK_F8 = 0x77;    // toggle the director help/status HUD
	constexpr int kVK_SHIFT = 0x10; // hold = slow/fine free-cam movement
	constexpr int kVK_CONTROL = 0x11;
	constexpr int kVK_SPACE = 0x20; // pause / resume the demo
	constexpr int kVK_LEFT = 0x25;  // skip -15s
	constexpr int kVK_RIGHT = 0x27; // skip +15s
	constexpr int kVK_ESC = 0x1B;   // close menu / cancel reposition / stop preview
	// Camera-marker / dolly path keys (BO2-style):
	constexpr int kVK_K = 0x4B;     // place a camera marker
	constexpr int kVK_L = 0x4C;     // delete the aimed-at marker
	constexpr int kVK_F = 0x46;     // edit the aimed-at marker
	constexpr int kVK_G = 0x47;     // toggle UI-mouse for the camera timeline panel
	constexpr int kVK_Z = 0x5A;     // Ctrl+Z: undo last curve edit

	constexpr int kSkipSeconds = 15; // arrow-key demo skip step

	// CS2 OBS_MODE_* enum: FIXED=1, IN_EYE=2, CHASE=3, ROAMING=4. The user
	// confirmed `spec_mode 4` is roaming (free fly), so first person (in-eye) = 2
	// and third person (chase) = 3. Centralised so it's a one-line tweak.
	const char* const kSpecModeFirstPerson = "spec_mode 2"; // in-eye
	const char* const kSpecModeThirdPerson = "spec_mode 3"; // chase

	// The director input taps must do NOTHING (and must not consume the event)
	// unless a demo is actively playing. Otherwise they swallow main-menu /
	// live-game mouse and key input. Called from the WndProc thread.
	bool IsDemoActive() {
		if (!g_pEngineToClient)
			return false;
		auto pDemo = g_pEngineToClient->GetDemoFile();
		return pDemo && pDemo->IsPlayingDemo();
	}
}

MovieMode& MovieModeRef() {
	static MovieMode s_instance;
	return s_instance;
}

void MovieMode::EnqueueCmd(const std::string& c) {
	std::lock_guard<std::mutex> lk(m_mutex);
	m_cmdQueue.push_back(c);
}

bool MovieMode::OnMouseWheel(int delta, bool shiftDown) {
	if (!IsDemoActive())
		return false;
	if (delta == 0)
		return false;

	// Inside the camera editor the wheel navigates camera keys and always moves to
	// their exact pose/tick. Do not cycle first/third/free camera modes here.
	if (CameraTimeline_Visible()) {
		EnqueueCmd(delta > 0
			? "mirv_filmmaker camtl selectdelta -1"
			: "mirv_filmmaker camtl selectdelta 1");
		return true;
	}

	// In free cam, Shift+scroll adjusts free-cam SPEED (documented control).
	if (CameraBridge_GetFreeCamEnabled() && shiftDown) {
		CameraBridge_AdjustFreeCamSpeed(delta > 0 ? +1 : -1);
		return true;
	}

	// Plain scroll cycles the camera mode, wrapping around at the ends:
	// up   = Default -> Third -> Free -> Default ...
	// down = Default -> Free  -> Third -> Default ...
	std::lock_guard<std::mutex> lk(m_mutex);
	constexpr int kModeCount = (int)Mode::FreeCam + 1;
	int next = ((int)m_mode + (delta > 0 ? +1 : -1) + kModeCount) % kModeCount;
	m_mode = (Mode)next;
	m_pendingMode = m_mode;
	m_modeDirty = true;
	return true;
}

bool MovieMode::OnMouseButton(int button, bool down) {
	// While repositioning a camera marker, left-click places the selected marker at
	// the current camera pose. Consume both down+up so MirvInput doesn't treat it as
	// a free-cam drag and the game doesn't switch spectator targets.
	if (button == 0 && IsDemoActive()
		&& CameraPathRef().GetMode() == CameraPath::Mode::Reposition) {
		if (down) EnqueueCmd("mirv_filmmaker marker repositionplace");
		return true;
	}
	// In FREE CAM, swallow LMB/RMB so CS2's spectator doesn't yank the view to a
	// different player while the director is flying the camera. EXCEPTIONS: when the
	// marker edit menu is open, OR the camera timeline's UI-mouse mode is on (G), we
	// must let the click reach Panorama -- those panels draw a full-screen hit-test
	// catcher that absorbs stray clicks (so they still can't switch players) while
	// keeping their own buttons/sliders clickable. This is what makes the timeline /
	// curve editor usable in free cam.
	if (IsDemoActive() && GetMode() == Mode::FreeCam
		&& !CameraPathRef().MenuOpen() && !CameraTimeline_WantsCursor())
		return true;
	// Otherwise never consume clicks: first/third-person spectator switches players on
	// LMB/RMB and the native demo UI (demoui) needs clicks to reach Panorama.
	return false;
}

bool MovieMode::OnKey(int vkey, bool down) {
	if (vkey == kVK_CONTROL) {
		m_controlDown = down;
		return false;
	}
	if (!IsDemoActive())
		return false; // director keys only act while a demo plays

	// Swallow OS keyboard AUTO-REPEAT for Space: held Space streams WM_KEYDOWN (down=true)
	// with no intervening up, and every camera-path transport branch below toggles on down,
	// so a held key would start then immediately pause/restart. Act once per physical press;
	// repeats are consumed (return true) so the game never sees them.
	if (vkey == kVK_SPACE) {
		if (down && m_spaceDown) return true;
		m_spaceDown = down;
	}

	if (vkey == kVK_Z && down && m_controlDown && CameraTimeline_Visible()) {
		EnqueueCmd("mirv_filmmaker camtl undo");
		return true;
	}

	// G toggles the native-demo-bar UI cursor only in regular viewing mode, and only
	// from third-person/freecam. The camera timeline / curve editor is always a UI
	// surface, so G is swallowed there and cannot turn the cursor off -- EXCEPT in
	// Camera Editor Mode, where G must keep flipping UI<->GAME so the user can fly the
	// free cam to frame a shot (the editor decouples the timeline's forced cursor).
	if (vkey == kVK_G) {
		if (CameraEditor_Active()) {
			if (down) EnqueueCmd("mirv_filmmaker camtl cursor toggle");
			return true;
		}
		if (CameraTimeline_Visible())
			return true;
		const Mode mode = GetMode();
		if (mode != Mode::ThirdPerson && mode != Mode::FreeCam) {
			if (down) EnqueueCmd("mirv_filmmaker camtl cursor off");
			return true;
		}
		if (down) EnqueueCmd("mirv_filmmaker camtl cursor toggle");
		return true;
	}

	using CPMode = CameraPath::Mode;
	const CPMode cpMode = CameraPathRef().GetMode();

	// --- camera-path sub-modes take priority over the normal director keys ---
	if (cpMode == CPMode::Reposition) {
		// Free-look to position; left-click places (OnMouseButton); X/Esc cancels.
		if (vkey == kVK_X || vkey == kVK_ESC) {
			if (down) EnqueueCmd("mirv_filmmaker marker repositioncancel");
			return true;
		}
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true; // don't scrub the demo
	} else if (cpMode == CPMode::PreviewArmed) {
		// Armed camera-path preview: Space starts the dolly, X/Esc cancels.
		if (vkey == kVK_SPACE) { if (down) EnqueueCmd("mirv_filmmaker marker play"); return true; }
		if (vkey == kVK_X || vkey == kVK_ESC) { if (down) EnqueueCmd("mirv_filmmaker marker previewstop"); return true; }
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true;
	} else if (cpMode == CPMode::PreviewPlaying) {
		// Keep pause/stop routed through CameraPath so demo playback and pose pushing stay in sync.
		if (vkey == kVK_SPACE) { if (down) EnqueueCmd("mirv_filmmaker camtl pause"); return true; }
		if (vkey == kVK_X || vkey == kVK_ESC) { if (down) EnqueueCmd("mirv_filmmaker marker previewstop"); return true; }
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true;
	}

	// ESC closes the marker edit menu (and is consumed so CS2's pause menu doesn't
	// pop). Only acts while the menu is open; otherwise ESC passes through normally so
	// CS2's pause menu opens as usual -- the camera editor/timeline stays visible behind
	// it (the HUD no longer tears down while paused), which is the intended behaviour.
	if (vkey == kVK_ESC) {
		if (CameraPathRef().MenuOpen()) {
			if (down) EnqueueCmd("mirv_filmmaker marker close");
			return true;
		}
		return false;
	}

	if (vkey == kVK_F8) {
		if (down)
			MovieHudRef().Toggle(); // show/hide the help/status panel
		return true;
	}

	if (vkey == kVK_SPACE && CameraEditor_Active() && cpMode == CPMode::Editing) {
		// Skip re-issuing playtest while a start is PENDING (mode stays Editing during the
		// seek settle) -- a held / repeated Space would otherwise restart the seek. Swallow
		// the key regardless so it never leaks to the demo_pause fallback below.
		if (down && !CameraPathRef().PlaybackPending())
			EnqueueCmd("mirv_filmmaker camtl playtest");
		return true;
	}

	if (vkey == kVK_SPACE) {
		// Toggle demo pause. Read the LIVE pause state (not a cached bool) so we
		// stay in sync when the native demo bar / console also pauses.
		if (down) {
			bool paused = false;
			if (g_pEngineToClient) {
				if (auto pDemo = g_pEngineToClient->GetDemoFile())
					paused = pDemo->IsDemoPaused();
			}
			EnqueueCmd(paused ? "demo_resume" : "demo_pause");
		}
		return true; // consume up+down so the game never sees Space (no jump)
	}

	if (vkey == kVK_LEFT || vkey == kVK_RIGHT) {
		// Skip the demo backward/forward by kSkipSeconds via an absolute
		// demo_gototick (CS2 has no relative skip command). Tick rate is derived
		// from MirvTime rather than hard-coded.
		if (down) {
			int tick = 0;
			if (g_MirvTime.GetCurrentDemoTick(tick)) {
				float ipt = g_MirvTime.interval_per_tick_get();
				int ticksPerSec = (ipt > 0.0f) ? (int)(1.0f / ipt + 0.5f) : 64;
				int delta = kSkipSeconds * ticksPerSec;
				int target = tick + (vkey == kVK_RIGHT ? +delta : -delta);
				std::ostringstream oss;
				if (CameraTimeline_Visible() && CameraPathRef().HasPathRange()) {
					target = CameraPathRef().ClampToPathTick(target);
					oss << "mirv_filmmaker camtl scrub " << target;
				} else {
					if (target < 0) target = 0;
					oss << "demo_gototick " << target;
				}
				EnqueueCmd(oss.str());
			}
		}
		return true;
	}

	if (vkey == kVK_SHIFT) {
		// Slow/fine movement while held — only meaningful in free cam (our
		// camera). Always release-restore so sensitivity can't get stuck low.
		// Don't consume: Shift may be bound elsewhere and MirvInput ignores it.
		if (down) {
			if (CameraBridge_GetFreeCamEnabled())
				CameraBridge_SetFreeCamSlow(true);
		} else {
			CameraBridge_SetFreeCamSlow(false);
		}
		return false;
	}

	if (vkey == kVK_X) {
		// In free cam, X is already roll (MirvInput) -> don't steal it.
		if (CameraBridge_GetFreeCamEnabled())
			return false;
		if (down) {
			bool nowOn;
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				m_xray = !m_xray;
				nowOn = m_xray;
			}
			EnqueueCmd(nowOn ? "spec_show_xray 1" : "spec_show_xray 0");
		}
		return true;
	}

	// --- camera-marker / dolly path keys (normal editing mode only) ---
	// All marker mutation runs on the main thread via the mirv_filmmaker console
	// dispatch (FlushActions executes the queued command there). We only enqueue
	// here and read the atomic "aimed-at" marker index for L/F targeting.
	if (cpMode == CPMode::Editing) {
		if (vkey == kVK_K) {
			// Place a marker at the current camera pose (any camera mode).
			if (down) EnqueueCmd("mirv_filmmaker marker place");
			return true;
		}

		if (vkey == kVK_L) {
			// Delete the marker the camera is aimed at (consume only when targeting).
			int h = CameraPathRef().HoveredAtomic();
			if (h >= 0) {
				if (down) EnqueueCmd(std::string("mirv_filmmaker marker delete ") + std::to_string(h));
				return true;
			}
			return false;
		}

		if (vkey == kVK_F) {
			// F toggles the marker settings menu: close it if open, else open it for
			// the aimed-at marker. Otherwise defer so MirvInput keeps F = free-cam down.
			if (CameraPathRef().MenuOpen()) {
				if (down) EnqueueCmd("mirv_filmmaker marker close");
				return true;
			}
			int h = CameraPathRef().HoveredAtomic();
			if (h >= 0) {
				if (down) EnqueueCmd(std::string("mirv_filmmaker marker edit ") + std::to_string(h));
				return true;
			}
			return false;
		}
	}

	return false;
}

void MovieMode::ApplyMode(Mode m) {
	switch (m) {
	case Mode::FreeCam:
		CameraBridge_SetFreeCamEnabled(true);
		break;
	case Mode::ThirdPerson:
		CameraBridge_SetFreeCamEnabled(false);
		if (g_pEngineToClient) {
			g_pEngineToClient->ExecuteClientCmd(0, kSpecModeThirdPerson, true);
			// Leaving free cam: drop the UI cursor so it can't linger into a normal
			// spectator view (and stays in sync for the next time you enter free cam).
			g_pEngineToClient->ExecuteClientCmd(0, "mirv_filmmaker camtl cursor off", true);
		}
		break;
	case Mode::Default:
		CameraBridge_SetFreeCamEnabled(false);
		if (g_pEngineToClient) {
			g_pEngineToClient->ExecuteClientCmd(0, kSpecModeFirstPerson, true);
			g_pEngineToClient->ExecuteClientCmd(0, "mirv_filmmaker camtl cursor off", true);
		}
		break;
	}
}

void MovieMode::FlushActions() {
	std::vector<std::string> cmds;
	bool modeDirty = false;
	Mode pending = Mode::Default;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		cmds.swap(m_cmdQueue);
		modeDirty = m_modeDirty;
		pending = m_pendingMode;
		m_modeDirty = false;
	}

	if (modeDirty)
		ApplyMode(pending);

	if (g_pEngineToClient)
		for (const std::string& c : cmds)
			g_pEngineToClient->ExecuteClientCmd(0, c.c_str(), true);
}

MovieMode::Mode MovieMode::GetMode() { std::lock_guard<std::mutex> lk(m_mutex); return m_mode; }
bool MovieMode::GetXray() { std::lock_guard<std::mutex> lk(m_mutex); return m_xray; }
bool MovieMode::GetCursor() { std::lock_guard<std::mutex> lk(m_mutex); return m_cursor; }

const char* MovieMode::ModeName() {
	switch (GetMode()) {
	case Mode::FreeCam: return "Free cam";
	case Mode::ThirdPerson: return "Third person";
	default: return "First person";
	}
}

void MovieMode::SyncFromFreeCam(bool freeCamEnabled) {
	std::lock_guard<std::mutex> lk(m_mutex);

	// Don't fight an in-flight transition: a scroll may have set a new pending
	// mode that FlushActions hasn't applied yet, so the live free-cam state still
	// reflects the OLD mode. Reconciling here would clobber the user's choice and
	// cause the intermittent first/third/free mislabel. Only correct the mode
	// once the queue is drained (m_modeDirty == false), i.e. for genuine external
	// (console) free-cam toggles.
	if (m_modeDirty)
		return;

	if (freeCamEnabled) {
		m_mode = Mode::FreeCam;
	} else if (m_mode == Mode::FreeCam) {
		// Free cam was turned off elsewhere; fall back to third person.
		m_mode = Mode::ThirdPerson;
	}
}

} // namespace Filmmaker
