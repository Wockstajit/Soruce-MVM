#include "CamPlayback.h"

#include "CameraBridge.h"
#include "../../MirvTime.h"
#include "../../../shared/AfxConsole.h"

#include <Windows.h>
#include <cmath>

namespace Filmmaker {

namespace {
	// Playback smoothing (fixes slow-timescale jitter + makes Smooth/Linear glide).
	constexpr double kClockRateTau = 0.25;    // s: smooth the demo-time RATE estimate
	constexpr double kClockCorrectTau = 0.30; // s: gently pull the predicted clock to truth
	constexpr double kPoseSmoothTau = 0.06;   // s: low-pass on the output camera pose

	constexpr int kTimingFreeze = 1;          // CameraPath::Timing::Freeze

	// Wrap a degree delta to [-180,180] so angle smoothing always takes the short way.
	double WrapDeg(double d) {
		while (d > 180.0) d -= 360.0;
		while (d < -180.0) d += 360.0;
		return d;
	}
}

double CamPlayback::WallDt() {
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	if (m_wallLastQpc == 0) m_wallLastQpc = now.QuadPart;
	double dt = (double)(now.QuadPart - m_wallLastQpc) / (double)freq.QuadPart;
	m_wallLastQpc = now.QuadPart;
	if (dt < 0.0) dt = 0.0; else if (dt > 0.1) dt = 0.1;
	return dt;
}

void CamPlayback::Reset() {
	m_playT = 0.0;
	m_playSeg = 0;
	m_lastLogSeg = -1;
	m_logAccum = 0.0;
	m_wallLastQpc = 0;
	m_liveClock = -1.0;
	m_liveClockRate = -1.0;
	m_prevDemoNow = -1.0;
	m_prevLiveClock = -1.0;
	m_rateDemoAccum = 0.0;
	m_rateWallAccum = 0.0;
	m_poseSmoothInit = false;
	m_engaged = false;
	m_engageApplied = false;
}

void CamPlayback::StartPlay(double duration, bool rangeGated, double startTiming) {
	Reset();
	m_duration = duration;
	m_playT = startTiming < 0.0 ? 0.0 : (startTiming > duration ? duration : startTiming);
	m_playing = true;
	m_scrubbing = false;
	m_rangeGated = rangeGated;
}

void CamPlayback::Stop() {
	m_playing = false;
	m_scrubbing = false;
}

void CamPlayback::BeginScrub(double tick) {
	m_playing = false;
	m_scrubbing = true;
	m_scrubTick = tick;
	m_poseSmoothInit = false; // scrub pushes raw poses; drop any stale smoothing state
}

bool CamPlayback::TickPlay(const CamMarkers& mk, const CamPathEval& eval, const Context& ctx) {
	if (!m_playing)
		return false;
	const int n = mk.Count();
	if (n < 2 || m_duration <= 0.0)
		return true; // nothing sensible to play -> signal "end" so the facade stops

	const double wallDt = WallDt();
	const bool freeze = (ctx.timing == kTimingFreeze);

	double demoNow = 0.0; g_MirvTime.GetCurrentDemoTime(demoNow);
	bool atEnd = false;
	CamPathEval::Pose pose;

	if (!freeze) {
		// PREDICTIVE demo clock -- the root fix for slow-timescale jitter. The engine's
		// demo time arrives COARSE and occasionally reversed at low timescale. Instead
		// of following it directly: (1) estimate the demo-time RATE (low-passed),
		// (2) ADVANCE the clock at that rate each frame, (3) gently CORRECT toward the
		// true demo time, and (4) SNAP on a genuine seek.
		if (m_liveClock < 0.0 || std::fabs(demoNow - m_liveClock) > 0.5) {
			// init / seek
			m_liveClock = demoNow; m_liveClockRate = -1.0; m_prevDemoNow = demoNow;
			m_prevLiveClock = demoNow; m_rateDemoAccum = 0.0; m_rateWallAccum = 0.0;
		} else {
			double dDemo = demoNow - m_prevDemoNow; // >= 0 when advancing
			m_prevDemoNow = demoNow;

			// Unbiased rate: accumulate the demo advance AND the wall time across
			// frames, then rate = demoAdvanced / wallElapsed over the window. Counting
			// the FLAT frames (where the coarse demo clock didn't tick) in the
			// denominator is what keeps the estimate honest -- the old code sampled
			// dDemo/wallDt only on the tick frames, so a whole tick landing in one
			// frame read as near real-time and the predictor raced ahead, then the
			// corrector hauled it back: the visible back-and-forth wiggle.
			if (dDemo > 0.0) m_rateDemoAccum += dDemo; // skip tiny backward blips
			m_rateWallAccum += wallDt;
			if (m_rateWallAccum >= kClockRateTau) {
				double inst = m_rateDemoAccum / m_rateWallAccum;
				m_liveClockRate = (m_liveClockRate < 0.0) ? inst
					: m_liveClockRate + (inst - m_liveClockRate) * 0.5;
				m_rateDemoAccum = 0.0; m_rateWallAccum = 0.0;
			}

			double rate = (m_liveClockRate > 0.0) ? m_liveClockRate : 0.0;
			m_liveClock += rate * wallDt; // predict at the (now unbiased) demo rate
			m_liveClock += (demoNow - m_liveClock) * (1.0 - std::exp(-wallDt / kClockCorrectTau));

			// Monotonic guard: a momentary over-predict + the correction pull must
			// never dip the clock below its last value (would read as the camera
			// stuttering backwards). Holding keeps motion one-directional; a real
			// seek is caught by the snap branch above.
			if (m_liveClock < m_prevLiveClock) m_liveClock = m_prevLiveClock;
			m_prevLiveClock = m_liveClock;
		}
		const double clk = m_liveClock;
		const double tFirst = mk.Front().time, tLast = mk.Back().time;

		// Range-gated (timeline play): hand the camera back to the normal demo view
		// until the REAL playhead reaches the first marker, so the dolly never moves /
		// holds before its first keyframe. Use demoNow (not the predicted clock) for
		// the gate so the camera can't anticipate the start. The free-cam state is
		// forced on the first frame (m_engageApplied) and then only on transitions.
		if (m_rangeGated) {
			bool wantEngage = (demoNow >= tFirst - 1e-4);
			if (!m_engageApplied || wantEngage != m_engaged) {
				CameraBridge_SetFreeCamEnabled(wantEngage);
				m_engaged = wantEngage;
				m_engageApplied = true;
				m_poseSmoothInit = false;
			}
			if (!wantEngage) {
				m_playSeg = 0;
				return false; // waiting for the path to start (not the end)
			}
		}

		pose = eval.EvalAtDemoTime(mk, clk);
		atEnd = (clk >= tLast - 1e-4);
	} else {
		// Freeze: advance along the speed-mode TIMING axis on wall-clock.
		m_playT += wallDt;
		if (m_playT >= m_duration) { m_playT = m_duration; atEnd = true; }
		pose = eval.EvalAtTiming(mk, m_playT);
	}
	m_playSeg = pose.seg;

	double poseX = 0, poseY = 0, poseZ = 0, posePitch = 0, poseYaw = 0, poseFov = 0;
	if (pose.valid) {
		// Low-pass the OUTPUT pose: rounds the velocity "corner" at each marker so the
		// move glides through keyframes and mops up any residual clock roughness.
		// Angles smooth along the SHORTEST arc so the +-180 wrap never spins.
		if (!m_poseSmoothInit) {
			m_sX = pose.x; m_sY = pose.y; m_sZ = pose.z;
			m_sPitch = pose.pitch; m_sYaw = pose.yaw; m_sRoll = pose.roll; m_sFov = pose.fov;
			m_poseSmoothInit = true;
		} else {
			double a = 1.0 - std::exp(-wallDt / kPoseSmoothTau);
			m_sX += (pose.x - m_sX) * a; m_sY += (pose.y - m_sY) * a; m_sZ += (pose.z - m_sZ) * a;
			m_sFov += (pose.fov - m_sFov) * a;
			m_sPitch += WrapDeg(pose.pitch - m_sPitch) * a;
			m_sYaw   += WrapDeg(pose.yaw   - m_sYaw)   * a;
			m_sRoll  += WrapDeg(pose.roll  - m_sRoll)  * a;
		}
		CameraBridge_SetCameraPose(m_sX, m_sY, m_sZ, m_sPitch, m_sYaw, m_sRoll, m_sFov);
		poseX = m_sX; poseY = m_sY; poseZ = m_sZ; posePitch = m_sPitch; poseYaw = m_sYaw; poseFov = m_sFov;
	}

	// Debug: log on every segment change + a throttled progress line.
	const int seg = pose.seg;
	const double camClk = freeze ? m_playT : m_liveClock;
	const double behind = freeze ? 0.0 : (demoNow - camClk);
	const int tgtSeg = (seg + 1 < n) ? seg + 1 : n - 1;
	const float segSpeed = (ctx.speedMode == 1 /*Constant*/) ? ctx.constSpeed
		: (ctx.speedMode == 2 /*PerSegment*/ ? mk.At(seg).speedMul : 1.0f);
	bool segChanged = (seg != m_lastLogSeg);
	m_logAccum += 1.0;
	if (segChanged || m_logAccum >= 30.0 || atEnd) {
		advancedfx::Message(
			"[campath] play clock=%s seg=%d/%d prog=%.0f%% mk #%d->#%d interp=%s speed=%s x%.2f "
			"camClk=%.2f demoTime=%.2f behind=%+.2fs target(tick=%d time=%.2f) "
			"pos=(%.1f %.1f %.1f) pitch=%.1f yaw=%.1f fov=%.1f%s.\n",
			(freeze ? "Freeze(wall)" : "Live(replay)"),
			seg + 1, (n > 1 ? n - 1 : 1), pose.p * 100.0,
			seg, tgtSeg, ctx.interpName, ctx.speedModeName, segSpeed,
			camClk, demoNow, behind, mk.At(tgtSeg).tick, mk.At(tgtSeg).time,
			poseX, poseY, poseZ, posePitch, poseYaw, poseFov,
			pose.valid ? "" : " (no pose)");
		m_lastLogSeg = seg;
		m_logAccum = 0.0;
	}

	return atEnd;
}

void CamPlayback::TickScrub(const CamMarkers& mk, const CamPathEval& eval) {
	if (!m_scrubbing)
		return;
	// Deterministic: evaluate the pure path at the requested tick and push it raw.
	// No predictive clock, no pose low-pass -> identical tick yields identical pose.
	CamPathEval::Pose pose = eval.EvalAtTick(mk, m_scrubTick);
	if (!pose.valid)
		return;
	m_playSeg = pose.seg;
	CameraBridge_SetCameraPose(pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll, pose.fov);
}

} // namespace Filmmaker
