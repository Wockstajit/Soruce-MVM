#include "MovieMode.h"

#include "CameraBridge.h"
#include "CameraPath.h"
#include "FollowCamera.h"
#include "ThirdPersonCamera.h"
#include "../Filmmaker.h"            // CameraTimeline_Visible()
#include "../MvmTest.h"              // mvm_test offline FX menu
#include "../Panorama/ConfigHud.h"   // ConfigHud_Enabled: Config panel gets editor-style G/wheel input priority
#include "../Panorama/TestHud.h"     // TestHud_Enabled: mvm_test FX menu G key
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
	constexpr int kVK_INSERT = 0x2D; // mvm_test FX menu toggle (offline live match)
	constexpr int kVK_Z = 0x5A;     // Ctrl+Z: undo last curve edit
	constexpr int kVK_A = 0x41;     // Ctrl+A: select all keyframes (graph editor)
	constexpr int kVK_DELETE = 0x2E;
	constexpr int kVK_BACKSPACE = 0x08;

	constexpr int kSkipSeconds = 15; // arrow-key demo skip step

	// CS2 OBS_MODE_* enum: FIXED=1, IN_EYE=2, CHASE=3, ROAMING=4. The user
	// confirmed `spec_mode 4` is roaming (free fly), so first person (in-eye) = 2
	// and third person (chase) = 3. Centralised so it's a one-line tweak.
	const char* const kSpecModeFirstPerson = "spec_mode 2"; // in-eye

	// The director input taps must do NOTHING (and must not consume the event)
	// unless a demo is actively playing. Otherwise they swallow main-menu /
	// live-game mouse and key input. Called from the WndProc thread.
	bool IsDemoActive() {
		if (!g_pEngineToClient)
			return false;
		auto pDemo = g_pEngineToClient->GetDemoFile();
		return pDemo && (pDemo->IsPlayingDemo() || pDemo->IsDemoPaused());
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

bool MovieMode::OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x, int y) {
	// mvm_test live-match FX panel: UI cursor on -> swallow wheel (same as demo UI-mouse).
	if (MvmTest_CanUseMenu() && TestHud_Enabled()) {
		if (CameraTimeline_WantsCursor())
			return true;
		return false;
	}
	if (!IsDemoActive())
		return false;
	if (delta == 0)
		return false;

	// Customize modal open: the modal is an EXCLUSIVE surface, exactly like the editor's UI-mouse
	// mode -- NOTHING may reach the game. In the in-game HUD, letting the wheel fall through to CS2
	// still drives the demo spectator (switch player / cycle first-third person) even when a Panorama
	// scroll container is under the cursor -- Panorama does not take the wheel away from the game
	// here. So we CONSUME it (return true -> main.cpp `return 0` before CallWindowProcW) and forward
	// the notch into the modal's JS, which scrolls its own right column / open dropdown list itself
	// (manual transform-scroll, see CameraEditorCustomizeJs.h custWheel). Net: the list scrolls and
	// the demo behind the modal never moves.
	if (CameraEditor_CustomizeModalOpen()) {
		CameraEditor_CustomizePushWheel(delta > 0 ? +1 : -1, x, y);
		return true;
	}

	// In the director FREE CAM the wheel drives the camera when a modifier is held:
	//   Ctrl+scroll  = FOV zoom -- up = zoom in (lower FOV), down = zoom out (higher FOV),
	//   Shift+scroll = move SPEED (documented control).
	// Checked first so framing a shot works no matter which editor surface is open.
	if (CameraBridge_GetFreeCamEnabled() && (ctrlDown || shiftDown)) {
		if (ctrlDown) CameraBridge_AdjustFreeCamFov(delta > 0 ? +1 : -1);
		else CameraBridge_AdjustFreeCamSpeed(delta > 0 ? +1 : -1);
		return true;
	}

	if (GetMode() == Mode::ThirdPerson && shiftDown) {
		ThirdPersonCameraState s = ThirdPersonCameraRef().State();
		ThirdPersonCameraRef().SetDistance(s.distance + (delta > 0 ? -10.0 : 10.0));
		return true;
	}

	// Plain-wheel routing depends on whether the UI mouse is active:
	//   * Camera Editor Mode: the G key flips UI<->GAME. In UI mouse (cursor on) the wheel
	//     stays a no-op so editing never "throws you back into the game"; in GAME mouse
	//     (cursor off) it falls through to the POV cycle below, so you can scroll between
	//     First/Third/Free while controlling the game.
	//   * Standalone camera timeline / experimental graph editor are always UI surfaces, so
	//     the plain wheel is swallowed there (it used to step the selected camera / keyframes).
	if (CameraEditor_Active() || ConfigHud_Enabled()) {
		if (CameraTimeline_WantsCursor())
			return true; // UI mouse: keep the wheel a no-op while editing
		// GAME mouse: fall through to the camera-mode cycle below.
	} else if (CameraTimeline_Visible() || GraphEditorExperiment_Enabled()) {
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
	// Follow Camera reposition uses the same free-look + left-click placement model
	// as path-marker repositioning.
	if (button == 0 && IsDemoActive() && FollowCameraRef().Repositioning()) {
		if (down) EnqueueCmd("mirv_filmmaker follow repositionplace");
		return true;
	}
	// While repositioning a camera marker, left-click places the selected marker at
	// the current camera pose. Consume both down+up so MirvInput doesn't treat it as
	// a free-cam drag and the game doesn't switch spectator targets.
	if (button == 0 && IsDemoActive()
		&& CameraPathRef().GetMode() == CameraPath::Mode::Reposition) {
		if (down) EnqueueCmd("mirv_filmmaker marker repositionplace");
		return true;
	}
	// While the EXPERIMENTAL graph editor is open, swallow RIGHT-click: it drives the editor's
	// ease context menu through the cursor pipe (not Panorama events), so it must not reach CS2's
	// spectator switch. Left-click still passes through (the editor's panels/buttons need it; its
	// full-screen catcher absorbs strays).
	if (button == 1 && IsDemoActive() && GraphEditorExperiment_WantsCursor())
		return true;
	// mvm_test live-match FX panel: UI cursor on -> let Panorama receive clicks; GAME
	// cursor off -> pass input through for aiming/shooting (G toggles via camtl cursor).
	if (MvmTest_CanUseMenu() && TestHud_Enabled()) {
		if (CameraTimeline_WantsCursor())
			return false;
		return false;
	}
	// In FREE CAM, swallow LMB/RMB so CS2's spectator doesn't yank the view to a
	// different player while the director is flying the camera. Gate on the ACTUAL free-cam
	// state (MirvInput camera control), not the tracked spectator mode: free cam is enabled
	// from many places (camera editor, camera paths, ...) that don't run through the scroll
	// mode-cycle, so GetMode() can still read First/Third while the cam is really flying --
	// and an unswallowed click there silently switches the spectated player.
	// EXCEPTIONS: when the marker edit menu is open, the camera timeline's UI-mouse mode is
	// on (G), OR the Customize modal is open, we must let the click reach Panorama -- those
	// surfaces draw a full-screen hit-test panel that absorbs stray clicks (so they still can't
	// switch players) while keeping their own buttons/sliders clickable. This is what makes the
	// timeline / curve editor / Customize modal usable. NOTE: consuming here (return true) makes
	// main.cpp's WndProc `return 0` BEFORE CallWindowProcW, so Panorama never sees the click --
	// that is exactly why the modal must be excepted, or every click in it is swallowed.
	// THIRD PERSON swallows clicks for the same reason: the orbit mouse-look is active,
	// and an unswallowed LMB/RMB is CS2's next/prev-player spectator bind -- so trying
	// to "drag" the orbit silently switched the spectated player. Switch players from
	// first person (scroll back) or the demoui player rows instead.
	if (IsDemoActive()
		&& (CameraBridge_GetFreeCamEnabled() || GetMode() == Mode::FreeCam || GetMode() == Mode::ThirdPerson)
		&& !CameraPathRef().MenuOpen() && !CameraTimeline_WantsCursor()
		&& !GraphEditorExperiment_WantsCursor() && !CameraEditor_CustomizeModalOpen())
		return true;
	// Otherwise never consume clicks: the first-person spectator switches players on
	// LMB/RMB and the native demo UI (demoui) needs clicks to reach Panorama.
	return false;
}

bool MovieMode::OnKey(int vkey, bool down) {
	if (vkey == kVK_CONTROL) {
		m_controlDown = down;
		// Modal open: swallow Ctrl too so no game bind (duck, etc.) fires behind it.
		if (IsDemoActive() && CameraEditor_CustomizeModalOpen()) return true;
		return false;
	}
	// Track Shift for chord detection (Ctrl+Shift+Z = redo). Don't return -- Shift still drives
	// the free-cam slow modifier further below.
	if (vkey == kVK_SHIFT)
		m_shiftDown = down;

	// mvm_test offline FX menu (live match, not demo playback). Insert auto-arms and opens.
	if (vkey == kVK_INSERT && down) {
		if (MvmTest_HandleInsertKey())
			return true;
	}
	if (MvmTest_CanUseMenu()) {
		if (vkey == kVK_G && TestHud_Enabled()) {
			if (down) EnqueueCmd("mirv_filmmaker camtl cursor toggle");
			return true;
		}
	}

	if (!IsDemoActive())
		return false; // director keys only act while a demo plays

	// Customize modal open: swallow EVERY key so NOTHING reaches the game -- no demo transport, no
	// weapon/movement binds, no spectator change, no pause menu. The modal is an exclusive input
	// surface like the editor's UI-mouse mode. Typed text is delivered separately: main.cpp forwards
	// WM_CHAR to the modal JS (CameraEditor_CustomizePushChar), because CS2 does not give an in-game
	// HUD Panorama TextEntry real keyboard focus -- so we drive the search boxes ourselves rather
	// than hoping the keystroke reaches Panorama. Consumed with NO game action here.
	if (CameraEditor_CustomizeModalOpen())
		return true;

	// Swallow OS keyboard AUTO-REPEAT for Space: held Space streams WM_KEYDOWN (down=true)
	// with no intervening up, and every camera-path transport branch below toggles on down,
	// so a held key would start then immediately pause/restart. Act once per physical press;
	// repeats are consumed (return true) so the game never sees them.
	if (vkey == kVK_SPACE) {
		if (down && m_spaceDown) return true;
		m_spaceDown = down;
	}

	if (vkey == kVK_Z && down && m_controlDown) {
		// Ctrl+Z = undo, Ctrl+Shift+Z = redo. Route to whichever curve surface is open: the
		// experimental graph editor owns it while enabled, otherwise the camera path/timeline.
		const bool redo = m_shiftDown;
		if (GraphEditorExperiment_Enabled()) {
			EnqueueCmd(redo ? "mirv_filmmaker grapheditor redo" : "mirv_filmmaker grapheditor undo");
			return true;
		}
		if (CameraTimeline_Visible()) {
			EnqueueCmd(redo ? "mirv_filmmaker camtl redo" : "mirv_filmmaker camtl undo");
			return true;
		}
	}

	if (vkey == kVK_A && down && m_controlDown && GraphEditorExperiment_Enabled()) {
		// Ctrl+A selects every keyframe in the graph editor.
		EnqueueCmd("mirv_filmmaker grapheditor selall");
		return true;
	}

	if ((vkey == kVK_DELETE || vkey == kVK_BACKSPACE) && GraphEditorExperiment_Enabled()) {
		if (down) EnqueueCmd("mirv_filmmaker grapheditor delsel");
		return true;
	}

	// G toggles the native-demo-bar UI cursor only in regular viewing mode, and only
	// from third-person/freecam. The camera timeline / curve editor is always a UI
	// surface, so G is swallowed there and cannot turn the cursor off -- EXCEPT in
	// Camera Editor Mode, where G must keep flipping UI<->GAME so the user can fly the
	// free cam to frame a shot (the editor decouples the timeline's forced cursor).
	// The Config panel gets the same priority: while it is open, G always flips UI<->GAME
	// regardless of camera mode (in first person the regular-mode branch below would force
	// the cursor OFF and the user could never get back into the panel).
	if (vkey == kVK_G) {
		if (CameraEditor_Active() || ConfigHud_Enabled() || TestHud_Enabled()) {
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
	if (FollowCameraRef().Repositioning()) {
		if (vkey == kVK_X || vkey == kVK_ESC) {
			if (down) EnqueueCmd("mirv_filmmaker follow repositioncancel");
			return true;
		}
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true;
	}
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

	// Space drives camera-path play ONLY when a path editor overlay is actually open (Camera
	// timeline or Graph). In the editor's Regular Timeline (native bottom) mode neither is open,
	// so Space must fall through to plain demo pause/resume below -- not trigger a path playtest
	// (which would just warn "need 2 camera markers"). Matches the gate used elsewhere here.
	if (vkey == kVK_SPACE && (CameraTimeline_Visible() || GraphEditorExperiment_Enabled()) && cpMode == CPMode::Editing) {
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
		// Release the orbit camera (and its mouse capture) before handing the view to
		// HLAE freecam -- the two both ride MirvInput and must not overlap.
		ThirdPersonCameraRef().Exit();
		CameraBridge_SetFreeCamEnabled(true);
		break;
	case Mode::ThirdPerson:
		CameraBridge_SetFreeCamEnabled(false);
		ThirdPersonCameraRef().Enter();
		if (g_pEngineToClient) {
			// Leaving free cam: drop the UI cursor so it can't linger into a normal
			// spectator view (and stays in sync for the next time you enter free cam).
			g_pEngineToClient->ExecuteClientCmd(0, "mirv_filmmaker camtl cursor off", true);
		}
		break;
	case Mode::Default:
		CameraBridge_SetFreeCamEnabled(false);
		ThirdPersonCameraRef().Exit();
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

void MovieMode::SetMode(Mode mode) {
	std::lock_guard<std::mutex> lk(m_mutex);
	m_mode = mode;
	m_pendingMode = mode;
	m_modeDirty = true;
}

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
	} else if (ThirdPersonCameraRef().State().active) {
		m_mode = Mode::ThirdPerson;
	} else if (m_mode == Mode::FreeCam) {
		// Free cam was turned off elsewhere; fall back to third person.
		m_mode = Mode::ThirdPerson;
	}
}

} // namespace Filmmaker
