#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Action Cam -- one-click head-mounted "GoPro" preset over the Follow/Attach camera system,
// modeled on the GMod "Action Cam" addon (head attachment + forward/up offset + wide FOV +
// smoothed "handheld gimbal" angles) and on BodyCam.h, which established this preset pattern.
// Differences from Body Cam: mounts at the HEAD virtual point (full eye angles incl. pitch,
// vs the chest's flattened pitch), sits slightly up/forward like a helmet cam, and drives an
// optional FISHEYE lens post-process (FisheyePass.cpp via the AfxFisheye bridge) while active.
//
// Per the feature request: no letterboxing, no battery/REC HUD overlay, no sharpen -- just the
// mounted camera plus the toggleable fisheye.
//
// Console: mirv_filmmaker actioncam [on|off|toggle|state]
//          mirv_filmmaker actioncam fisheye [on|off|toggle]
// UI: Config panel -> MODIFIERS -> "Action Cam" + "Fisheye Lens" buttons (ConfigHudJs.h).

// True while the Follow camera is running this preset (Attach mode, player target, "head"
// attachment). Same shape as BodyCam_Active.
bool ActionCam_Active();

// Whether the fisheye lens applies while the Action Cam is active (sticky user preference;
// flipping it mid-run takes effect immediately, flipping it while inactive just arms it).
bool ActionCam_FisheyeEnabled();
void ActionCam_SetFisheye(bool enable);

// Engage on the currently spectated player / stop. Engaging replaces any Body Cam or other
// Follow preview (single FollowCamera instance); stopping restores the normal spectate view.
void ActionCam_Set(bool enable);
void ActionCam_Toggle();

// Main-thread per-frame pump (Filmmaker::RunMainThreadFrame, next to BodyCam_RunFrame):
// notices FollowCamera stopping the preview on its own (target death / demo end) to restore
// the view + free-cam latch, and publishes this frame's fisheye request to the render layer.
void ActionCam_RunFrame();

// Console command entry: handles "mirv_filmmaker actioncam ...".
void ActionCam_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
