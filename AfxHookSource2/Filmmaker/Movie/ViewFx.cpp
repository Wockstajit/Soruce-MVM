#include "ViewFx.h"

#include "FollowCameraMath.h"
#include "ViewFxVm.h"

#include "../../../shared/AfxConsole.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {

namespace {

// V_CalcRoll, ported from the Quake-lineage GoldSrc SDK (deps/release/halflife/cl_dll/view.cpp):
// roll ramps linearly with the strafe-perpendicular speed component up to rollSpeed, then
// clamps at rollAngle. side is velocity projected onto the view's own right vector.
double CalcRoll(double side, double rollAngle, double rollSpeed) {
	const double sign = side < 0.0 ? -1.0 : 1.0;
	side = side < 0.0 ? -side : side;
	if (side < rollSpeed) side = side * rollAngle / rollSpeed;
	else side = rollAngle;
	return side * sign;
}

// V_CalcBob, ported from the same GoldSrc SDK lineage (deps/release/halflife/cl_dll/view.cpp):
// a free-running walk-cycle clock (bobtime) maps through an asymmetric triangle->sine ramp --
// quick rise over the first bobUp fraction of the cycle, slower settle over the rest -- so the
// vertical camera bob reads like a footstep landing, not a plain sine wave. Amplitude is
// proportional to planar speed (sqrt(vel.x^2+vel.y^2), matching the original's XY-only read so
// jumping doesn't spike it), blended 30/70 between a flat and cycling term (the SDK's own
// tuning) so the camera never goes perfectly flat between steps, then clamped.
constexpr double kPi = 3.14159265358979323846;

double BobCyclePhase(double bobTime, double bobCycle, double bobUp) {
	double cycle = bobTime - std::floor(bobTime / bobCycle) * bobCycle;
	cycle /= bobCycle;
	if (cycle < bobUp) return kPi * cycle / bobUp;
	return kPi + kPi * (cycle - bobUp) / (1.0 - bobUp);
}

double CalcBob(double phase, double planarSpeed, double ampPerUnitSpeed, double clampLo, double clampHi) {
	double bob = planarSpeed * ampPerUnitSpeed;
	bob = bob * 0.3 + bob * 0.7 * std::sin(phase);
	if (bob < clampLo) bob = clampLo; else if (bob > clampHi) bob = clampHi;
	return bob;
}

// Reference ("100%") tuning the intensity knobs scale off of; the knob is a plain amplitude
// multiplier (fraction = percent / 100) on top of these, extrapolating past 1.0 for the top
// of the slider's range. The sensitivity/cadence terms (rollSpeed, bobCycle/bobUp, bobFreq,
// smoothing half-times) stay fixed -- only the "how much" terms move with the slider, so each
// effect's character doesn't change shape as it's dialed up or down, just its strength.
constexpr double kRollBaseAngle = 4.5;   // degrees, at 100%
constexpr double kRollSpeed = 180.0;     // strafe-speed units/sec to reach max roll angle
constexpr double kBobBaseAmpPerSpeed = 0.01; // GoldSrc v_bob default, at 100%
constexpr double kBobCycle = 0.8;        // seconds per step cycle, GoldSrc v_bobcycle default
constexpr double kBobUpFraction = 0.5;   // GoldSrc v_bobup default (rise/settle split)
constexpr double kBobBaseClampLo = -7.0; // world units, at 100% (GoldSrc's own clamp)
constexpr double kBobBaseClampHi = 4.0;  // world units, at 100%
constexpr double kSwayBaseIdleAmp = 0.40; // viewmodel units, at 100%
constexpr double kSwayBaseBobAmp = 0.80;  // viewmodel units, at 100%
constexpr double kSwayBobFreq = 7.5;      // footstep cadence, radians/sec at full speed

// Deadzone free aim, modeled on ARC9's sh_freeaim.lua (github.com/necoarctic/ARC-9):
//   * each aim delta is only PARTIALLY absorbed into the free-aim offset (ARC9 uses
//     diff * 0.25) -- this damping is what keeps the effect from feeling hair-trigger;
//   * the offset is clamped per-axis AND by 2D magnitude to a radius;
//   * the offset is HELD, never chased back to center mid-swipe (ARC9 has no auto
//     recenter at all); we add a slow at-rest recenter so a demo camera doesn't stay
//     miscentered for minutes, gated on the aim being quiet for a while first.
// The camera shows (true aim - freeAim); the viewmodel is nudged toward the true aim.
constexpr double kDzAbsorb = 0.25;         // fraction of each aim delta absorbed (ARC9's 0.25)
constexpr double kDzBaseRadiusDeg = 4.0;   // free-aim radius at 100%
constexpr double kDzSnapDeg = 45.0;        // per-axis per-tick delta beyond this = POV snap, reset
constexpr double kDzQuietDeg = 0.04;       // per-tick delta below this counts as "aim at rest"
constexpr double kDzQuietDelay = 0.35;     // seconds of rest before recentering starts
constexpr double kDzRecenterHalfTime = 0.8; // at-rest recenter half-time, seconds
constexpr double kVmShiftPerDegYaw = 0.12;  // viewmodel X units per lag degree (subtle hint)
constexpr double kVmShiftPerDegPitch = 0.10; // viewmodel Z units per lag degree

// Sway staleness: if the speed estimate stops updating (paused demo / another camera system
// owns the pose), fade the bob out instead of riding the frozen speed forever.
constexpr float kSwayStaleAfter = 0.30f; // wall seconds without a speed update = stale
constexpr float kSwayStaleFade = 0.20f;  // fade-out window once stale
constexpr double kSwaySpeedDeadband = 15.0; // u/s; below this the weapon holds perfectly still

struct RollPreset { double angle, speed; };
RollPreset RollPresetForIntensity(double percent) {
	if (percent <= 0.0) return { 0.0, 1.0 };
	return { kRollBaseAngle * (percent / 100.0), kRollSpeed };
}

struct SwayPreset { double idleAmp, bobAmp, bobFreq; };
SwayPreset SwayPresetForIntensity(double percent) {
	if (percent <= 0.0) return { 0.0, 0.0, 1.0 };
	const double frac = percent / 100.0;
	return { kSwayBaseIdleAmp * frac, kSwayBaseBobAmp * frac, kSwayBobFreq };
}

constexpr double kSpeedRef = 250.0;      // ~sprint speed reference the walk-bob ramps up to
constexpr float kTrackResetGap = 0.25f;  // seconds; a bigger jump EITHER WAY = fresh sample
constexpr double kSmoothHalfTime = 0.10; // velocity smoothing half-time, seconds
// The view-setup clock (g_MirvTime.curtime_get()) advances every RENDER frame, but the
// spectated POV's view origin only moves on demo-TICK boundaries. So most samples show
// dt > 0 with dx == 0 -- integrating those as "velocity 0" hammered the smoothed estimate
// toward zero between ticks (live-measured: speed sawtoothing 10 -> 1e-21 while the player
// sprinted), which killed the roll and kept the walk-bob at zero. Instead, a no-movement
// sample only ACCUMULATES its dt; the next sample with real displacement integrates over the
// whole accumulated window. Only when no movement arrives for kStillTimeout (several ticks)
// is the player treated as genuinely stationary and the velocity decayed.
constexpr double kMoveEpsilon = 0.01;    // units; below this a sample counts as "no movement"
constexpr double kStillTimeout = 0.06;   // seconds (~4 ticks at 64tps) of no movement = truly still
// One-sample view-origin travel farther than this means the POV jumped (spectator switch /
// teleport / a different view rendering through the same hook), NOT real movement.
constexpr double kTeleportDistance = 600.0;

double ClampIntensity(double percent) {
	if (!(percent > 0.0)) return 0.0; // also catches NaN
	return percent > kViewFxMaxIntensity ? kViewFxMaxIntensity : percent;
}

} // namespace

ViewFx& ViewFxRef() {
	static ViewFx s_instance;
	return s_instance;
}

void ViewFx::SetRollIntensity(double percent) { m_rollIntensity = ClampIntensity(percent); }
void ViewFx::SetBobIntensity(double percent) { m_bobIntensity = ClampIntensity(percent); }
void ViewFx::SetSwayIntensity(double percent) { m_swayIntensity = ClampIntensity(percent); }
void ViewFx::SetDeadzoneIntensity(double percent) { m_deadzoneIntensity = ClampIntensity(percent); }

void ViewFx::ResetTracker(float curTime, double x, double y, double pitchDeg, double yawDeg) {
	m_haveTrackSample = true;
	m_lastTrackTime = curTime;
	m_lastX = x; m_lastY = y;
	m_velX = m_velY = 0.0; m_speed = 0.0;
	++m_speedSerial; // a reset IS a speed update (to 0); lets sway fade cleanly, not stale-freeze
	m_lastRollDelta = 0.0;
	m_bobTime = 0.0; m_lastBobDelta = 0.0; // fresh walk-cycle phase, never carry a stale bob across a seek
	m_teleportStreak = 0;
	m_pendingDt = 0.0;
	m_lastAimPitch = pitchDeg; m_lastAimYaw = yawDeg; // deadzone: aim starts centered (no lag)
	m_freeAimPitch = m_freeAimYaw = 0.0;
	m_aimQuietTime = 0.0;
	m_camDeltaPitch = m_camDeltaYaw = 0.0;
}

ViewFxPoseDeltas ViewFx::TrackAndPoseDeltas(float curTime, double x, double y, double pitchDeg, double yawDeg) {
	++m_dbgCalls;
	ViewFxPoseDeltas out;

	// Fresh sample on the first call OR any big clock jump -- IN EITHER DIRECTION. A demo
	// seek BACKWARD moves curTime behind m_lastTrackTime; the old forward-only gap check
	// missed that, so the same-tick hold below then returned "held" forever (until playback
	// crawled past the pre-seek clock) -- roll/sway looked permanently dead after a
	// forward-then-back scrub, and toggling the effect off/on didn't touch tracker state.
	const float gap = curTime - m_lastTrackTime;
	if (!m_haveTrackSample || gap > kTrackResetGap || gap < -kTrackResetGap) {
		++m_dbgGapResets;
		ResetTracker(curTime, x, y, pitchDeg, yawDeg);
		return out; // all-zero: never let a seek read as a velocity/aim spike
	}

	// curTime only advances when the demo actually steps; between ticks (and while paused)
	// render frames re-enter with the same clock. Hold the current outputs -- recomputing
	// would integrate zero-velocity samples and flicker the effects off (fixed earlier).
	if (curTime <= m_lastTrackTime) {
		++m_dbgHolds;
		out.roll = m_lastRollDelta;
		out.pitch = m_camDeltaPitch;
		out.yaw = m_camDeltaYaw;
		out.zOffset = m_lastBobDelta;
		return out;
	}

	const double dx = x - m_lastX, dy = y - m_lastY;
	const double dist2 = dx * dx + dy * dy;

	// Outlier rejection. A jump bigger than any real one-tick movement is either (a) a
	// spectator switch/teleport, or (b) a DIFFERENT view rendered through the same view-setup
	// hook this frame (preview scenes, etc.) at a far-away origin. For (b) it is critical to
	// NOT adopt the outlier position/time: adopting it corrupted the tracker so the real POV's
	// next sample also read as a teleport, ping-ponging the estimate to a permanent 0 (the
	// "strafe roll never engages / sway never speeds up" symptom). Rejecting without adopting
	// keeps the accepted stream intact regardless of the per-frame view order. For (a) the
	// streak escape below hard-resets onto the new position after ~10 ticks.
	if (dist2 > (kTeleportDistance * kTeleportDistance)) {
		++m_dbgTeleports;
		if (++m_teleportStreak > 20) { // genuine POV change: adopt the new stream cleanly
			++m_dbgGapResets;
			ResetTracker(curTime, x, y, pitchDeg, yawDeg);
			return out;
		}
		out.roll = m_lastRollDelta;
		out.pitch = m_camDeltaPitch;
		out.yaw = m_camDeltaYaw;
		out.zOffset = m_lastBobDelta;
		return out;
	}
	m_teleportStreak = 0;

	const double dt = (double)(curTime - m_lastTrackTime);
	m_lastTrackTime = curTime;

	// ---- Deadzone free-aim tracker (ARC9 model; runs on every accepted tick step, even when
	// the player is stationary -- you can stand still and flick). Each tick's aim delta is
	// PARTIALLY absorbed (kDzAbsorb, ARC9's 0.25 damping) into the free-aim offset, clamped
	// per-axis and by 2D magnitude to the radius, and then HELD: a sustained right-swipe parks
	// the weapon at the right edge of the cone and it STAYS there until the aim moves the
	// other way or has been at rest for kDzQuietDelay (then a slow recenter drains it). The
	// previous exp-chase design recentered between every swipe -- weapon bounced back to
	// middle mid-turn -- and transferred 100% of small deltas, which felt hair-trigger at any
	// intensity (a small radius just meant "instantly pinned + sign-flipping").
	{
		const double dp = FollowWrapDegrees(pitchDeg - m_lastAimPitch);
		const double dyw = FollowWrapDegrees(yawDeg - m_lastAimYaw);
		m_lastAimPitch = pitchDeg; m_lastAimYaw = yawDeg;

		if (std::fabs(dp) > kDzSnapDeg || std::fabs(dyw) > kDzSnapDeg) {
			// POV snap (spectator switch / demo cut / whip flick): recenter instantly rather
			// than banking a bogus offset from a jump that wasn't hand aim.
			m_freeAimPitch = m_freeAimYaw = 0.0;
			m_aimQuietTime = 0.0;
		} else if (DeadzoneEnabled()) {
			const double radius = kDzBaseRadiusDeg * (m_deadzoneIntensity / 100.0);
			m_freeAimPitch += dp * kDzAbsorb;
			m_freeAimYaw += dyw * kDzAbsorb;
			if (m_freeAimPitch > radius) m_freeAimPitch = radius; else if (m_freeAimPitch < -radius) m_freeAimPitch = -radius;
			if (m_freeAimYaw > radius) m_freeAimYaw = radius; else if (m_freeAimYaw < -radius) m_freeAimYaw = -radius;
			const double mag = std::sqrt(m_freeAimPitch * m_freeAimPitch + m_freeAimYaw * m_freeAimYaw);
			if (mag > radius && mag > 0.0) { // 2D clamp, same as ARC9's mag2d clamp
				m_freeAimPitch *= radius / mag;
				m_freeAimYaw *= radius / mag;
			}

			// At-rest recenter (our addition; ARC9 holds forever): only after the aim has been
			// quiet for a while, and slow -- never fights an in-progress swipe.
			if (std::fabs(dp) < kDzQuietDeg && std::fabs(dyw) < kDzQuietDeg) {
				m_aimQuietTime += dt;
				if (m_aimQuietTime > kDzQuietDelay) {
					const double aRe = FollowHalfTimeAlpha(kDzRecenterHalfTime, dt);
					m_freeAimPitch -= m_freeAimPitch * aRe;
					m_freeAimYaw -= m_freeAimYaw * aRe;
				}
			} else {
				m_aimQuietTime = 0.0;
			}

			// Camera shows (true aim - freeAim); freeAim is where the true aim sits in the cone.
			m_camDeltaPitch = -m_freeAimPitch;
			m_camDeltaYaw = -m_freeAimYaw;
		} else {
			m_freeAimPitch = m_freeAimYaw = 0.0;
			m_aimQuietTime = 0.0;
			m_camDeltaPitch = m_camDeltaYaw = 0.0;
		}
		// True-rotation experiment (ViewFxVm): the weapon must rotate BY +freeAim relative to
		// the lagged camera to point at the true aim. No-op unless a working write site/field
		// was configured via "viewfx vmtest".
		ViewFxVm_SetFreeAim(m_freeAimPitch, m_freeAimYaw);
	}

	// ---- Planar-velocity tracker (roll + sway speed) ----
	if (dist2 < kMoveEpsilon * kMoveEpsilon) {
		// No displacement: the POV origin is still parked on the previous demo tick's position.
		// Bank the elapsed time and keep the current estimate; only a sustained still period
		// decays the velocity (see kMoveEpsilon/kStillTimeout notes above).
		m_pendingDt += dt;
		if (m_pendingDt >= kStillTimeout) {
			const double alphaStill = FollowHalfTimeAlpha(kSmoothHalfTime, m_pendingDt);
			m_velX -= m_velX * alphaStill;
			m_velY -= m_velY * alphaStill;
			m_speed = std::sqrt(m_velX * m_velX + m_velY * m_velY);
			++m_speedSerial;
			m_pendingDt = 0.0;
			++m_dbgSteps;
		} else {
			++m_dbgHolds;
		}
	} else {
		// Real movement: integrate over the whole window since the last MOVING sample.
		const double window = dt + m_pendingDt;
		m_pendingDt = 0.0;
		m_lastX = x; m_lastY = y;
		++m_dbgSteps;

		const double alpha = FollowHalfTimeAlpha(kSmoothHalfTime, window);
		m_velX += (dx / window - m_velX) * alpha;
		m_velY += (dy / window - m_velY) * alpha;
		m_speed = std::sqrt(m_velX * m_velX + m_velY * m_velY);
		++m_speedSerial;
	}

	if (RollEnabled()) {
		// Project the smoothed world-space velocity onto the view's right vector (side =
		// v . right, V_CalcRoll's convention) via the same forward/right/up basis
		// FollowCamera's attach math already uses. Only yaw matters for a ground-plane strafe.
		const FollowAngles yawOnly{ 0.0, yawDeg, 0.0 };
		const double side = FollowInverseRotateVector(FollowVec3{ m_velX, m_velY, 0.0 }, yawOnly).y;
		const RollPreset preset = RollPresetForIntensity(m_rollIntensity);
		m_lastRollDelta = CalcRoll(side, preset.angle, preset.speed);
	} else {
		m_lastRollDelta = 0.0;
	}

	// Bob's phase clock always advances (matches GoldSrc: the walk-cycle keeps ticking every
	// frame regardless of speed) -- only the AMPLITUDE below is speed-gated, so a standing
	// player's frozen phase never matters (0 speed = 0 bob regardless of where in the cycle).
	m_bobTime += dt;
	if (BobEnabled()) {
		const double frac = m_bobIntensity / 100.0;
		const double phase = BobCyclePhase(m_bobTime, kBobCycle, kBobUpFraction);
		m_lastBobDelta = CalcBob(phase, m_speed, kBobBaseAmpPerSpeed * frac,
			kBobBaseClampLo * frac, kBobBaseClampHi * frac);
	} else {
		m_lastBobDelta = 0.0;
	}

	out.roll = m_lastRollDelta;
	out.pitch = m_camDeltaPitch;
	out.yaw = m_camDeltaYaw;
	out.zOffset = m_lastBobDelta;
	return out;
}

void ViewFx::SwayOffset(float nowSeconds, float& outX, float& outY, float& outZ) {
	outX = outY = outZ = 0.0f;
	if (!SwayEnabled()) { m_haveSwaySample = false; return; }

	const bool gapTooLarge = m_haveSwaySample && (nowSeconds - m_lastSwayTime) > kTrackResetGap;
	float dt = 0.0f;
	if (!m_haveSwaySample || gapTooLarge || nowSeconds <= m_lastSwayTime) {
		m_haveSwaySample = true;
	} else {
		dt = nowSeconds - m_lastSwayTime;
		if (dt > 0.1f) dt = 0.1f; // clamp a hitch so the bob can't leap on resume
	}
	m_lastSwayTime = nowSeconds;

	const SwayPreset preset = SwayPresetForIntensity(m_swayIntensity);
	// Speed deadband: micro eye-origin jitter (breathing/lean animations) must not read as
	// walking. Below kSwaySpeedDeadband the weapon holds perfectly still.
	double speedFrac = (m_speed - kSwaySpeedDeadband) / (kSpeedRef - kSwaySpeedDeadband);
	if (speedFrac < 0.0) speedFrac = 0.0; else if (speedFrac > 1.0) speedFrac = 1.0;

	// Staleness gate: the speed estimate only updates while the tracker steps (demo playing,
	// plain POV). When it stops -- demo PAUSED, or another camera system owns the pose -- the
	// last speed freezes, and without this the wall-clock bob kept marching on a frozen demo
	// ("weapon sway continues when they're still", live-confirmed: paused demo, speed stuck at
	// 28). Fade the bob out shortly after updates stop; resume instantly when they return.
	if (m_speedSerial != m_swaySeenSerial) {
		m_swaySeenSerial = m_speedSerial;
		m_swaySerialWall = nowSeconds;
	} else {
		const float age = nowSeconds - m_swaySerialWall;
		if (age > kSwayStaleAfter) {
			float fade = 1.0f - (age - kSwayStaleAfter) / kSwayStaleFade;
			if (fade < 0.0f) fade = 0.0f;
			speedFrac *= fade;
		}
	}

	// Footstep cadence speeds up with movement; this is a proper time-integral (phase +=
	// dt * rate), not phase = t * rate, so the rate changing frame-to-frame never snaps.
	m_swayPhase += dt * preset.bobFreq * (0.4 + 0.6 * speedFrac);

	// ALL sway is movement-gated: a stationary player's weapon must sit perfectly still (the
	// old always-on "idle" sine read as a constant left/right wobble even at a dead stop).
	// The slow drift terms fade in with speed alongside the footstep bob.
	const double t = (double)nowSeconds;
	const double driftX = std::sin(t * 0.9) * preset.idleAmp * speedFrac;
	const double driftY = std::sin(t * 0.6 + 1.3) * preset.idleAmp * 0.6 * speedFrac;
	const double bobZ = std::fabs(std::sin(m_swayPhase)) * preset.bobAmp * speedFrac;
	const double bobX = std::sin(m_swayPhase * 0.5) * preset.bobAmp * 0.4 * speedFrac;

	outX = (float)(driftX + bobX);
	outY = (float)driftY;
	outZ = (float)bobZ;
}

void ViewFx::BobViewmodelFollow(float& outForward, float& outUp) const {
	outForward = outUp = 0.0f;
	if (!BobEnabled()) return;
	// m_lastBobDelta is written on the view-setup (render) thread and read here inside the
	// engine's viewmodel calc -- same benign single-writer race SwayOffset already runs on
	// m_speed. Held values between demo ticks match the camera side's same-tick hold.
	outUp = (float)m_lastBobDelta;                 // gun tracks the camera's bob exactly
	outForward = (float)(m_lastBobDelta * 0.4);    // V_CalcNormalRefdef's bob*0.4*forward push
}

void ViewFx::DeadzoneViewmodelShift(float& outX, float& outY, float& outZ) {
	outX = outY = outZ = 0.0f;
	if (!DeadzoneEnabled()) return;
	if (ViewFxVm_RotationActive()) return; // true rotation engaged: the translation hint is off
	// The camera lags the true aim by (m_camDeltaYaw, m_camDeltaPitch); push the viewmodel
	// toward where the aim actually is so the weapon reads as having moved first. Lag is
	// already clamped to the deadzone cone, so these shifts are bounded (~0.5 units at 100%).
	outX = (float)(m_camDeltaYaw * kVmShiftPerDegYaw);
	outZ = (float)(m_camDeltaPitch * kVmShiftPerDegPitch);
}

std::string ViewFx::DebugStateJson() const {
	std::ostringstream o;
	o << "{\"rollPct\":" << (float)m_rollIntensity
		<< ",\"bobPct\":" << (float)m_bobIntensity
		<< ",\"swayPct\":" << (float)m_swayIntensity
		<< ",\"deadzonePct\":" << (float)m_deadzoneIntensity
		<< ",\"speed\":" << (float)m_speed
		<< ",\"lastBobZ\":" << (float)m_lastBobDelta
		<< ",\"camLag\":[" << (float)m_camDeltaPitch << "," << (float)m_camDeltaYaw << "]"
		<< ",\"calls\":" << m_dbgCalls
		<< ",\"steps\":" << m_dbgSteps
		<< ",\"holds\":" << m_dbgHolds
		<< ",\"gapResets\":" << m_dbgGapResets
		<< ",\"teleports\":" << m_dbgTeleports
		<< ",\"gated\":" << m_dbgGated << "}";
	return o.str();
}

void ViewFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	ViewFx& fx = ViewFxRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";
	const char* value = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[viewfx][state] %s\n", fx.DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(action, "vmtest")) {
		// True viewmodel-rotation experiment (write-site/field probing); see ViewFxVm.h.
		ViewFxVm_RunCommand(argc, args, cmd);
		return;
	}
	const bool isRoll = 0 == _stricmp(action, "roll");
	const bool isBob = 0 == _stricmp(action, "bob");
	const bool isSway = 0 == _stricmp(action, "sway");
	const bool isDeadzone = 0 == _stricmp(action, "deadzone");
	if (isRoll || isBob || isSway || isDeadzone) {
		double percent = isRoll ? fx.RollIntensity() : isBob ? fx.BobIntensity() : isSway ? fx.SwayIntensity() : fx.DeadzoneIntensity();
		if (*value) {
			if (0 == _stricmp(value, "off")) percent = 0.0;
			else percent = std::atof(value);
		}
		if (isRoll) fx.SetRollIntensity(percent);
		else if (isBob) fx.SetBobIntensity(percent);
		else if (isSway) fx.SetSwayIntensity(percent);
		else fx.SetDeadzoneIntensity(percent);
		// "quiet" (used by the Config sliders, which fire on every drag step): no echo --
		// a console line per drag notch floods the console.
		if (argc >= 5 && 0 == _stricmp(args->ArgV(4), "quiet"))
			return;
		const double applied = isRoll ? fx.RollIntensity() : isBob ? fx.BobIntensity() : isSway ? fx.SwayIntensity() : fx.DeadzoneIntensity();
		const char* name = isRoll ? "view roll (strafe tilt)" : isBob ? "view bob (camera walk-bob)" : isSway ? "weapon sway" : "aim deadzone (decoupled viewmodel)";
		if (applied > 0.0)
			advancedfx::Message("mirv_filmmaker: %s = %d%%.\n", name, (int)std::lround(applied));
		else
			advancedfx::Message("mirv_filmmaker: %s = off.\n", name);
		return;
	}
	advancedfx::Message(
		"%s viewfx roll [<0-150>|off] - Quake/Doom-style camera tilt on strafe, as an intensity percent (default POV/third-person spectate only).\n"
		"%s viewfx bob [<0-150>|off] - GoldSrc-style vertical camera bob on the walk cycle, as an intensity percent (default POV/third-person spectate only).\n"
		"%s viewfx sway [<0-150>|off] - movement-scaled weapon sway + walk bob, as an intensity percent.\n"
		"%s viewfx deadzone [<0-150>|off] - decoupled viewmodel: weapon leads inside an aim deadzone, camera catches up smoothed.\n"
		"%s viewfx state - print the current intensities + tracker diagnostics.\n",
		cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
