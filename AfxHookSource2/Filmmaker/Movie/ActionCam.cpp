#include "ActionCam.h"

#include "FollowCamera.h"
#include "CameraBridge.h"          // release free-cam camera control on stop (see RestoreView)
#include "../Filmmaker.h"          // CameraEditor_Active (editor owns free-cam lifecycle itself)

#include "../../ClientEntitySystem.h" // AfxGetSpectatedPawnIndex
#include "../../FisheyePass.h"        // AfxFisheye::SetRequest (render-layer lens pass)
#include "../../../shared/AfxConsole.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstdlib>
#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {
	// GMod-faithful mount (the addon's convar defaults: actioncam_cam_position x=5 z=8,
	// angles 0, fov 110, off a head attachment): the camera floats 8 units ABOVE and 5 in
	// front of the head, looking where the head looks. Combined with the wide FOV and the
	// player model being drawn (spec_mode 3 below = GMod's drawviewer=1), the player's own
	// body/weapon fills the lower frame -- THAT framing is the entire "action cam" look. A
	// lens at eye height looking forward is just first person with a wide FOV (the first
	// version's mistake, user-reported). FollowCamera offset axes: x = fwd, y = right, z = up.
	// Defaults are mutable statics (not constexpr) so `actioncam offset/pitch` can retune the
	// mount live in-game without a rebuild.
	double g_fwdOffset = 5.0;
	double g_rightOffset = 0.0;
	double g_upOffset = 8.0;
	double g_pitchOffset = 0.0; // degrees, + = tilt down toward the gun
	constexpr double kFov = 110.0;              // GMod Action Cam default FOV
	// "Handheld gimbal" feel. GMod's default anglesmooth 0.95 maps to LerpAngle(5/sec) --
	// an exponential half-time of ~0.14s; FollowCamera's half-time smoothing at 0.12 is the
	// same curve, so this matches the addon's out-of-the-box floatiness.
	constexpr double kLookSmoothing = 0.12;
	constexpr double kPositionSmoothing = 0.03;

	// Same restore contract as BodyCam.cpp's RestoreViewAfterBodyCam (see the history there):
	// every FollowCamera preview latches free-cam camera control ON and only the Camera Editor
	// normally turns it off. Action Cam runs from the Config panel with the editor closed, so
	// it must release the latch itself or the OS cursor stays captured for mouse-look forever.
	void RestoreViewAfterActionCam() {
		if (CameraEditor_Active()) return;
		CameraBridge_SetFreeCamEnabled(false);
		if (g_pEngineToClient) {
			g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 2", true);
			g_pEngineToClient->ExecuteClientCmd(0, "mirv_filmmaker camtl cursor off", true);
		}
	}

	// True between a successful ActionCam_Set(true) and the moment it stops (by either path).
	// Primary gate for the fisheye request, so a manually-built head-attach Follow preview from
	// the editor never accidentally engages the lens.
	bool s_actionCamEngaged = false;

	// Sticky lens preference (defaults ON -- the lens IS the action-cam look; the toggle exists
	// to turn it off). Applies only while the Action Cam is engaged.
	bool s_fisheyeEnabled = true;

	// What we last told the render layer, so the pump only calls SetRequest on changes.
	bool s_fisheyeRequested = false;

	void PublishFisheye(bool want) {
		if (want == s_fisheyeRequested) return;
		s_fisheyeRequested = want;
		AfxFisheye::SetRequest(want, 1.0f);
	}
}

bool ActionCam_Active() {
	const FollowCameraState& st = FollowCameraRef().State();
	return st.enabled && st.mode == FollowMode::Attach && st.targetType == FollowTargetType::Player
		&& st.attachmentName == "head";
}

bool ActionCam_FisheyeEnabled() { return s_fisheyeEnabled; }

void ActionCam_SetFisheye(bool enable) {
	s_fisheyeEnabled = enable;
	// Take effect immediately when engaged; RunFrame would also catch it next frame, but the
	// Config button should feel instant.
	PublishFisheye(s_actionCamEngaged && ActionCam_Active() && s_fisheyeEnabled);
}

void ActionCam_Set(bool enable) {
	FollowCamera& follow = FollowCameraRef();
	if (!enable) {
		if (ActionCam_Active()) follow.StopPreview("action cam off");
		RestoreViewAfterActionCam(); // release free-cam even if already stopped (never leave the
		                             // OS cursor / mouse-look latched; same rule as Body Cam).
		s_actionCamEngaged = false;
		PublishFisheye(false);
		return;
	}

	const int target = AfxGetSpectatedPawnIndex();
	if (target < 0) {
		advancedfx::Warning("[actioncam] refused: no spectated player (switch to a player's POV first).\n");
		return;
	}

	follow.SetTargetType(FollowTargetType::Player);
	follow.SetMode(FollowMode::Attach);
	follow.SetAttachmentName("head");
	follow.SetOffset(g_fwdOffset, g_rightOffset, g_upOffset);
	follow.SetRotationOffset(g_pitchOffset, 0.0, 0.0);
	follow.SetFov(kFov);
	follow.SetLookSmoothing(kLookSmoothing);
	follow.SetPositionSmoothing(kPositionSmoothing);
	follow.SelectEntity(target);
	follow.Preview();

	if (follow.State().enabled) {
		// GMod's drawviewer=1 equivalent: in-eye spectate (spec_mode 2) HIDES the spectated
		// pawn's model, which reduces this whole preset to "first person with a wide FOV".
		// Chase mode (spec_mode 3, see MovieMode.cpp's verified CS2 numbering) renders the
		// body; our FollowCamera override still owns the actual camera pose. The stop path
		// (RestoreViewAfterActionCam) already returns to spec_mode 2.
		if (g_pEngineToClient)
			g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 3", true);
		s_actionCamEngaged = true;
		PublishFisheye(s_fisheyeEnabled);
	} else {
		advancedfx::Warning("[actioncam] failed to engage -- see the [followcam] warning above.\n");
	}
}

void ActionCam_Toggle() { ActionCam_Set(!ActionCam_Active()); }

void ActionCam_RunFrame() {
	// Action Cam stopped from under us (FollowCamera's own death/demo-end/target-lost handling,
	// or another camera system taking over the Follow state) -> restore the view once and drop
	// the fisheye, so neither the free-cam latch nor the lens ever outlives the mount.
	if (s_actionCamEngaged && !ActionCam_Active()) {
		RestoreViewAfterActionCam();
		s_actionCamEngaged = false;
	}
	PublishFisheye(s_actionCamEngaged && s_fisheyeEnabled);
}

void ActionCam_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[actioncam][state] %s, fisheye %s, mount fwd=%.1f right=%.1f up=%.1f pitch=%.1f\n",
			ActionCam_Active() ? "on" : "off", s_fisheyeEnabled ? "on" : "off",
			g_fwdOffset, g_rightOffset, g_upOffset, g_pitchOffset);
		return;
	}
	if (0 == _stricmp(arg, "fisheye")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "toggle";
		if (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1")) ActionCam_SetFisheye(true);
		else if (0 == _stricmp(v, "off") || 0 == _stricmp(v, "0")) ActionCam_SetFisheye(false);
		else ActionCam_SetFisheye(!s_fisheyeEnabled);
		advancedfx::Message("mirv_filmmaker: action cam fisheye lens %s%s.\n",
			s_fisheyeEnabled ? "ON" : "off",
			ActionCam_Active() ? "" : " (applies while the action cam is on)");
		return;
	}
	// Live mount tuning (no rebuild): applied immediately when engaged (FollowCamera reads
	// the framing state every frame, so Set* while previewing takes effect on the next one).
	if (0 == _stricmp(arg, "offset")) {
		if (argc >= 6) {
			g_fwdOffset = std::atof(args->ArgV(3));
			g_rightOffset = std::atof(args->ArgV(4));
			g_upOffset = std::atof(args->ArgV(5));
			if (s_actionCamEngaged && ActionCam_Active())
				FollowCameraRef().SetOffset(g_fwdOffset, g_rightOffset, g_upOffset);
		}
		advancedfx::Message("mirv_filmmaker: action cam mount offset fwd=%.1f right=%.1f up=%.1f (GMod default 5 0 8).\n",
			g_fwdOffset, g_rightOffset, g_upOffset);
		return;
	}
	if (0 == _stricmp(arg, "pitch")) {
		if (argc >= 4) {
			g_pitchOffset = std::atof(args->ArgV(3));
			if (s_actionCamEngaged && ActionCam_Active())
				FollowCameraRef().SetRotationOffset(g_pitchOffset, 0.0, 0.0);
		}
		advancedfx::Message("mirv_filmmaker: action cam mount pitch %.1f deg (+ = down toward the gun; GMod default 0).\n",
			g_pitchOffset);
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) ActionCam_Set(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) ActionCam_Set(false);
	else ActionCam_Toggle();
	advancedfx::Message("mirv_filmmaker: action cam %s (needs a spectated player).\n",
		ActionCam_Active() ? "ON" : "off");
}

} // namespace Filmmaker
