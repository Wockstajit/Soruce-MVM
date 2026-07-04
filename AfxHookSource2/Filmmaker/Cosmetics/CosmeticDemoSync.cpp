// Cosmetic demo synchronization: demo-seek detection and the post-apply "tick nudge"
// state machine. A cosmetic change while the demo is PAUSED does not re-evaluate
// renderables on its own; the engine-native fix is to briefly resume, let ~10 ticks
// render, and re-pause (MaybeFireTickNudge). Seeks invalidate every per-pawn apply gate
// and start a settle window sized to the jump (OnDemoSeekDetected) so the override
// system does not fight the engine while the entity list is being rebuilt.

#include "CosmeticOverrideSystem.h"

#include "CosmeticDebugLog.h"

#include "../../MirvTime.h" // g_MirvTime.GetCurrentDemoTick for the tick nudge

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (demo seek)

// Engine pointer (same one CameraPath/MovieMode use) for the demo re-seek that forces a renderable
// rebuild after a cosmetic change (the user-requested "nudge a few ticks" lever).
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// One-shot composite burst after a skin apply (avoids the old 150-frame hammer during playback).
constexpr int kCompositeBurstShots = 2;

} // namespace

void CosmeticOverrideSystem::RequestCompositeRefresh() {
	m_compositeBurstRemaining = kCompositeBurstShots;
}

void CosmeticOverrideSystem::AbortActiveTickNudge(const char* reason) {
	m_pendingNudge = false;
	if (m_nudgePhase != 1)
		return;
	if (m_nudgeWasPaused && g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
	m_nudgePhase = 0;
	m_nudgeWasPaused = false;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.nudge", "ABORT %s frame=%llu", reason ? reason : "?",
			(unsigned long long)m_frameCounter);
}

void CosmeticOverrideSystem::OnDemoSeekDetected(int tickJump) {
	AbortActiveTickNudge("seek");
	m_agentState.clear();
	m_gloveState.clear();
	m_lastGloveSpectatedSid = 0;
	m_knifeSwapState.clear();
	m_framesSinceSeek = 0;
	m_lastCompositeTick = -1;
	m_compositeBurstRemaining = 0;
	m_lastCompositeFrameByIdx.clear();
	int settle = 32;
	if (tickJump > 128) settle = 48;
	if (tickJump > 512) settle = 64;
	if (tickJump > 2048) settle = 96;
	if (tickJump > 8192) settle = 128;
	// Stack / extend when seeks arrive back-to-back (round skip spam).
	if (m_seekSettleFrames > 0)
		m_seekSettleFrames = (settle > m_seekSettleFrames) ? settle : (m_seekSettleFrames + settle / 2);
	else
		m_seekSettleFrames = settle;
	m_vmMirrorSettleFrames = m_seekSettleFrames * 4;
	if (m_vmMirrorSettleFrames < 96) m_vmMirrorSettleFrames = 96;
	if (MvmDebugLog_Active()) {
		MvmDebugLog_Linef("cosmetics.seek", "jump=%d settleFrames=%d vmSettle=%d", tickJump, settle, m_vmMirrorSettleFrames);
		MvmCrashWatch_Arm(-1, "demo-seek"); // widen crash.veh window across seek/settle for round-skip repros
	}
}

void CosmeticOverrideSystem::DetectDemoSeekEarly() {
	int curTick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(curTick))
		return;
	if (m_lastDemoTickObserved >= 0) {
		int d = curTick - m_lastDemoTickObserved;
		if (d < 0) d = -d;
		if (d > 64)
			OnDemoSeekDetected(d);
	}
	m_lastDemoTickObserved = curTick;
}

void CosmeticOverrideSystem::RequestApplyNudge() {
	RequestCompositeRefresh();
	// Mark a nudge pending and (re)set the debounce anchor to now. A burst of edits (e.g. dragging
	// the wear slider) keeps pushing the anchor forward, so exactly one seek fires after the user
	// stops -- never one seek per intermediate value.
	m_pendingNudge = true;
	m_nudgeRequestFrame = m_frameCounter;
}

void CosmeticOverrideSystem::MaybeFireTickNudge() {
	// Phase 1: a nudge is currently playing out (we issued demo_resume) -- re-pause once enough ticks
	// have rendered, or a frame safety cap trips so the demo is never left stuck playing.
	if (m_nudgePhase == 1) {
		int tick = 0;
		bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
		bool enough = haveTick && (tick - m_nudgeStartTick >= m_tickNudgeTicks);
		bool timedOut = (m_frameCounter - m_nudgePlayStartFrame) > 240; // ~2-4s safety
		if (enough || timedOut) {
			if (m_nudgeWasPaused && g_pEngineToClient)
				g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("cosmetics.nudge",
					"DONE re-paused playedTicks=%d enough=%d timedOut=%d",
					tick - m_nudgeStartTick, enough ? 1 : 0, timedOut ? 1 : 0);
			// Re-assert the override onto whatever entities the play-out left us with, and RE-ARM the dense
			// composite window: the ~10-tick play-out is shorter than the tick periodic, so the engine
			// rebuilt the weapon composite during it -- without this the skin reverts to default once paused.
			m_agentState.clear();
			m_gloveState.clear();
			m_lastGloveSpectatedSid = 0;
			m_knifeSwapState.clear();
			m_compositeBurstRemaining = kCompositeBurstShots;
			m_nudgePhase = 0;
			++m_totalNudges;
		}
		return;
	}

	if (!m_pendingNudge)
		return;
	// Throttled (not per-frame): framesSinceReq changes every call so MvmDebugLog_Linef's dedup can't
	// collapse it: this diagnosed the m_seekSettleFrames-stuck-forever bug above via 2000+ lines/3s.
	if (MvmDebugLog_Active() && (m_frameCounter % 16) == 0)
		MvmDebugLog_Linef("cosmetics.nudge.gate",
			"pending=1 seekSettle=%d tickNudge=%d framesSinceReq=%llu inDemo=%d",
			m_seekSettleFrames, m_tickNudge ? 1 : 0,
			(unsigned long long)(m_frameCounter - m_nudgeRequestFrame), InDemoContext() ? 1 : 0);
	if (m_seekSettleFrames > 0)
		return; // defer until entity list settles after a seek
	if (!m_tickNudge) { m_pendingNudge = false; return; } // disabled: drop the request
	// Debounce: wait for a short quiet period after the last change so rapid re-picks coalesce.
	const uint64_t kDebounceFrames = 8;
	if (m_frameCounter - m_nudgeRequestFrame < kDebounceFrames)
		return;
	if (!InDemoContext())
		return; // keep pending; fire once a demo is actually playing
	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.nudge.gate", "tick unreadable, deferring");
		return; // tick not readable yet (mid-seek) -- retry next frame
	}
	m_pendingNudge = false;

	// If the demo is already PLAYING, live frames already re-evaluate the renderable -- the swaps
	// show on their own, so there is nothing to do but re-fire the override onto current entities.
	bool paused = true;
	if (g_pEngineToClient) {
		if (auto* demo = g_pEngineToClient->GetDemoFile())
			paused = demo->IsDemoPaused();
	}
	if (!paused) {
		m_agentState.clear();
		m_gloveState.clear();
		m_lastGloveSpectatedSid = 0;
		return;
	}

	// Paused: briefly resume so the engine renders real frames (the only thing that makes a body
	// model / glove / mesh swap re-evaluate), then re-pause after ~m_tickNudgeTicks ticks -- exactly
	// "let the game play ~10 ticks" done automatically. Clear the per-pawn apply gates so the
	// override re-fires onto any entities the play-out recreates.
	m_agentState.clear();
	m_gloveState.clear();
	m_lastGloveSpectatedSid = 0;
	m_nudgeWasPaused = true;
	m_nudgeStartTick = tick;
	m_nudgePlayStartFrame = m_frameCounter;
	m_nudgePhase = 1;
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.nudge", "START resume plannedTicks=%d", m_tickNudgeTicks);
}

} // namespace Filmmaker
