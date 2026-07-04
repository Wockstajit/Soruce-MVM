#pragma once

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Camera "feel" modifiers layered on top of whatever pose the engine (or another Filmmaker
// camera system) already computed this frame:
//   * strafe ROLL   - Quake/Doom-style view tilt on lateral strafe.
//   * view BOB      - GoldSrc-style vertical camera bob on the walk cycle (the CAMERA origin,
//                     not the weapon -- see SWAY below for the viewmodel-only equivalent).
//   * weapon SWAY   - movement-scaled walk bob/drift added to the viewmodel offset.
//   * aim DEADZONE  - decoupled viewmodel: the recorded aim moves the WEAPON first inside a
//                     deadzone cone while the CAMERA catches up with smoothing (the camera
//                     lags the true aim by a clamped angle; the viewmodel is shifted toward
//                     the true aim so the weapon reads as leading).
// All default OFF and share one lightly-smoothed planar-speed/angle tracker, so turning on
// any one doesn't require the others.
//
// Each is a single continuous 0-150% INTENSITY knob (0 = off, 100 = the reference tuning),
// not a discrete preset -- lets the Config panel expose a plain increase/decrease slider.
//
// Self-contained: no engine sig-scanning of its own. Roll + deadzone camera lag are applied
// by the existing view-setup trampoline (main.cpp) only when nothing else already owns the
// camera pose that frame (default POV / third-person spectate); sway + the deadzone
// viewmodel shift are applied by the existing mirv_viewmodel detour (ViewModel.cpp). This
// file only owns the state + math all of it rides on.
constexpr double kViewFxMaxIntensity = 150.0;

// Pose deltas for THIS frame, to ADD to the engine's unmodified view. All zero when every
// effect is off (or the tracker has no data yet), so callers can cheaply skip the write.
struct ViewFxPoseDeltas {
	double roll = 0.0;    // strafe roll, degrees
	double pitch = 0.0;   // deadzone camera lag, degrees
	double yaw = 0.0;     // deadzone camera lag, degrees
	double zOffset = 0.0; // view bob, world units -- ADD directly to the camera origin Z
	bool Any() const { return roll != 0.0 || pitch != 0.0 || yaw != 0.0 || zOffset != 0.0; }
};

class ViewFx {
public:
	// value is clamped to [0, kViewFxMaxIntensity]; 0 turns the effect off.
	void SetRollIntensity(double percent);
	void SetBobIntensity(double percent);
	void SetSwayIntensity(double percent);
	void SetDeadzoneIntensity(double percent);
	double RollIntensity() const { return m_rollIntensity; }
	double BobIntensity() const { return m_bobIntensity; }
	double SwayIntensity() const { return m_swayIntensity; }
	double DeadzoneIntensity() const { return m_deadzoneIntensity; }
	bool RollEnabled() const { return m_rollIntensity > 0.0; }
	bool BobEnabled() const { return m_bobIntensity > 0.0; }
	bool SwayEnabled() const { return m_swayIntensity > 0.0; }
	bool DeadzoneEnabled() const { return m_deadzoneIntensity > 0.0; }

	// Called once per view-setup frame, ONLY when nothing else already owns the camera pose
	// this frame (default engine POV/third-person spectate) -- (x, y) is the planar view
	// origin, (pitchDeg, yawDeg) the view angles, all still the engine's own unmodified pose
	// at the call site. Feeds the smoothed planar-velocity estimate (shared with sway) and the
	// smoothed aim (deadzone), and returns the pose deltas to ADD this frame. All-zero when
	// the effects are off or right after a reset (seek, spectator switch, big time gap), so a
	// pose jump can never read as a velocity/aim spike.
	ViewFxPoseDeltas TrackAndPoseDeltas(float curTime, double x, double y, double pitchDeg, double yawDeg);

	// Weapon-sway offset (viewmodel-local units, same X/Y/Z axes mirv_viewmodel already uses)
	// for this frame, ADDED on top of whatever base offset the caller already computed --
	// never overwrites. Runs its own free-running wall-clock phase (nowSeconds, real time so
	// it behaves the same whether the demo is playing or paused) plus the last tracked planar
	// speed to scale the walk-bob; ALL of it is movement-gated (a stationary player's weapon
	// sits perfectly still).
	void SwayOffset(float nowSeconds, float& outX, float& outY, float& outZ);

	// Deadzone viewmodel shift for this frame (same axes/contract as SwayOffset): translates
	// the viewmodel toward the TRUE aim while the camera lags it, selling "weapon moves first".
	// All-zero while the deadzone is off.
	void DeadzoneViewmodelShift(float& outX, float& outY, float& outZ);

	// View-bob viewmodel FOLLOW for this frame. The camera half of the bob (zOffset above)
	// raises the world-render origin, but CS2 anchors the viewmodel to the pawn's own
	// unmodified eye position -- without this the gun visually slides opposite the camera by
	// the full bob amount (magnified at lens distance) while the world's shift reads as
	// nothing, i.e. "only the gun bobs". GoldSrc's V_CalcNormalRefdef moves BOTH: vieworg[2]
	// += bob AND view->origin[2] += bob (+ bob*0.4 along forward). This returns that
	// viewmodel-side pair -- outUp = bob (track the camera), outForward = 0.4*bob (the SDK's
	// forward push) -- in the same viewmodel-local axes SwayOffset uses (y = forward, z = up).
	// All-zero while bob is off.
	void BobViewmodelFollow(float& outForward, float& outUp) const;

	// Smoothed planar speed (units/sec) most recently tracked via TrackAndPoseDeltas.
	double PlanarSpeed() const { return m_speed; }

	// Diagnostics: main.cpp calls this on view-setup frames where another camera system already
	// owned the pose (so tracking was skipped). Counted so "viewfx state" can show WHY the speed
	// estimate is not updating (gated vs teleport-looping vs simply paused).
	void NoteGatedByOverride() { ++m_dbgGated; }

	std::string DebugStateJson() const;

private:
	void ResetTracker(float curTime, double x, double y, double pitchDeg, double yawDeg);

	double m_rollIntensity = 0.0;     // percent, 0-150
	double m_bobIntensity = 0.0;      // percent, 0-150
	double m_swayIntensity = 0.0;     // percent, 0-150
	double m_deadzoneIntensity = 0.0; // percent, 0-150

	// Planar-velocity tracker (roll + bob input, shared speed for sway).
	bool m_haveTrackSample = false;
	float m_lastTrackTime = 0.0f;
	double m_lastX = 0.0, m_lastY = 0.0;
	double m_velX = 0.0, m_velY = 0.0; // smoothed world-space planar velocity
	double m_speed = 0.0;              // |velX, velY|, smoothed
	double m_lastRollDelta = 0.0;      // held across same-tick render frames (see .cpp)
	double m_bobTime = 0.0;            // free-running walk-cycle clock (curTime-driven, see .cpp)
	double m_lastBobDelta = 0.0;       // held across same-tick render frames (see .cpp)
	int m_teleportStreak = 0;          // consecutive rejected jumps (see .cpp escape hatch)
	double m_pendingDt = 0.0;          // banked time from no-movement samples (see .cpp)

	// Deadzone free-aim tracker (ARC9 sh_freeaim.lua model): each accepted tick ABSORBS a
	// fraction of the aim delta into a free-aim offset, clamped to a radius, and HOLDS it --
	// no chase-recenter mid-swipe. See .cpp for the dynamics.
	double m_lastAimPitch = 0.0, m_lastAimYaw = 0.0; // accepted stream's previous aim
	double m_freeAimPitch = 0.0, m_freeAimYaw = 0.0; // where the TRUE aim sits relative to camera
	double m_aimQuietTime = 0.0;                     // seconds of near-zero aim deltas (recenter gate)
	double m_camDeltaPitch = 0.0, m_camDeltaYaw = 0.0; // applied camera lag = -freeAim, degrees

	// Sway phase clock (independent free-running clock; not tied to the tracker above), plus a
	// staleness gate: when the tracker stops producing speed updates (paused demo, override
	// gate), the wall-clock bob must fade out instead of riding a frozen speed forever.
	bool m_haveSwaySample = false;
	float m_lastSwayTime = 0.0f;
	double m_swayPhase = 0.0;
	unsigned long long m_speedSerial = 0;      // bumped whenever m_speed is (re)computed
	unsigned long long m_swaySeenSerial = 0;   // last serial SwayOffset observed
	float m_swaySerialWall = 0.0f;             // wall time when the serial last changed

	// Diagnostics for "viewfx state" (single writer = render thread; racy console reads are fine).
	unsigned long long m_dbgCalls = 0;     // TrackAndPoseDeltas invocations
	unsigned long long m_dbgGapResets = 0; // fresh-sample resets (first call / seek / big gap)
	unsigned long long m_dbgHolds = 0;     // same-tick holds (curTime unchanged)
	unsigned long long m_dbgSteps = 0;     // real integration steps
	unsigned long long m_dbgTeleports = 0; // rejected teleport-sized jumps
	unsigned long long m_dbgGated = 0;     // view-setup frames another system owned the pose
};

ViewFx& ViewFxRef();

// Console command entry: handles "mirv_filmmaker viewfx ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "viewfx").
void ViewFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
