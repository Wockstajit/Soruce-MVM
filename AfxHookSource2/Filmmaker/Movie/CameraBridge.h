#pragma once

// Narrow bridge between the movie director (MovieMode) / HUD (MovieHud) and the
// camera-input system, which lives in main.cpp (g_MirvInputEx / MirvInput).
//
// These functions are DEFINED in main.cpp (where g_MirvInputEx is visible) and
// declared here so the Filmmaker modules can drive/read the free camera without
// seeing the MirvInputEx internals. This keeps camera ownership in the camera
// system and lets the UI/director layers stay decoupled (architecture rule:
// Panorama displays state + sends commands; the camera system owns movement).

namespace Filmmaker {

// Free-cam (MirvInput camera control mode) on/off.
bool  CameraBridge_GetFreeCamEnabled();
void  CameraBridge_SetFreeCamEnabled(bool enable);

// Free-cam speed multiplier; dir > 0 increases, dir < 0 decreases (one step).
float CameraBridge_GetFreeCamSpeed();
void  CameraBridge_AdjustFreeCamSpeed(int dir);

// Free-cam FOV (zoom), one step per call. dir > 0 = scroll up = zoom IN (lower FOV);
// dir < 0 = scroll down = zoom OUT (higher FOV). The new FOV persists frame-over-frame.
void  CameraBridge_AdjustFreeCamFov(int dir);

// Temporarily scale free-cam keyboard+mouse sensitivity for fine/slow movement
// (held while Shift is down). Saves/restores the base sensitivity internally and
// is idempotent, so repeated true/true or false/false calls are safe.
void  CameraBridge_SetFreeCamSlow(bool slow);

// Current (post-override) camera FOV, for the HUD readout.
float CameraBridge_GetGameCameraFov();

// Read the current (post-override) camera pose. Used by the camera-marker system
// to snapshot the free cam when placing / repositioning a marker. Angles are
// pitch, yaw, roll (degrees) to match the engine view-angle order.
void CameraBridge_GetCurrentCamera(double outOrigin[3], double outAngles[3], double& outFov);

// Push an ABSOLUTE camera pose for camera-path playback. Applied on the next
// camera override (MirvInput::Override) and consumed once, so it must be called
// every frame while a path is playing. Works whether or not free cam is on.
void CameraBridge_SetCameraPose(double x, double y, double z,
                                double pitch, double yaw, double roll, double fov);

// Enable/disable the in-world camera-path keyframe gizmos (g_CampathDrawer).
void CameraBridge_SetPathDrawEnabled(bool enable);

// UI cursor probe for the EXPERIMENTAL graph editor. Returns the latest client-area cursor
// position + left/right-button state captured in the WndProc (main.cpp), plus the live Shift
// state (read on the main thread). 'seq' increments on every mouse-move so the JS can detect
// motion even when the coordinates repeat. This is what lets the graph editor implement
// After-Effects-style continuous dragging (Panorama itself has no mouse-move event). 'rmb' drives
// the right-click ease menu; 'shift' drives axis-lock dragging. Coordinates are window-client
// pixels (divide by the panel uiscale to map into Panorama layout space).
void CameraBridge_GetUiCursor(int& x, int& y, bool& lmb, bool& rmb, bool& shift, unsigned& seq);

// Style the in-world keyframe gizmos for the camera-marker system: paint them in
// the global Freeze (blue) / Live (gold) colour with a glow, and draw the aimed-at
// marker (highlight index, -1 = none) white. When enabled, also forces the
// keyframe-camera gizmos + index labels on. Pushed every frame while editing.
void CameraBridge_SetMarkerStyle(bool enabled, bool freeze, int highlightIndex);

} // namespace Filmmaker
