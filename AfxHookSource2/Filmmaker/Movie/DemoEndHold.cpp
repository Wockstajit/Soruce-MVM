#include "DemoEndHold.h"

#include "../Filmmaker.h"               // PlayingDemoPath()
#include "../Demo/DemoHeaderReader.h"   // playback_ticks from the .dem header
#include "../../MirvTime.h"

#include "../../../shared/AfxConsole.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// How many ticks BEFORE the header's playback_ticks we freeze. The check runs once per rendered
// frame, so at high demo timescale the tick can advance a chunk between checks (64 tps * 16x /
// 60 fps ~= 17 ticks/frame); one second of headroom means we still catch it before the engine
// processes the DEM_Stop record and disconnects to the main menu.
constexpr int kEndMarginTicks = 64;
// Demos shorter than this (a couple of seconds) aren't worth guarding -- and a tiny/garbage
// playback_ticks would put the hold point at/near tick 0 and freeze playback instantly.
constexpr int kMinTotalTicks = 4 * kEndMarginTicks;

bool s_enabled = true;
std::wstring s_path;        // demo the cached tick count belongs to
int s_totalTicks = -1;      // header playback_ticks; -1 = unusable for this demo -> feature inert
bool s_holding = false;     // paused at the end-hold; next resume = restart from the beginning
bool s_restarting = false;  // seek back to the start was issued; suppress re-pausing until it lands
unsigned long long s_restartDeadlineMs = 0;

void Reset() {
	s_path.clear();
	s_totalTicks = -1;
	s_holding = false;
	s_restarting = false;
}

void DemoCmd(const char* c) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
}

} // namespace

void DemoEndHold_SetEnabled(bool enabled) {
	s_enabled = enabled;
	if (!enabled) Reset();
}

bool DemoEndHold_Enabled() { return s_enabled; }

void DemoEndHold_RunFrame() {
	if (!s_enabled || !g_pEngineToClient) { Reset(); return; }

	auto* demo = g_pEngineToClient->GetDemoFile();
	const bool playing = demo && demo->IsPlayingDemo();
	const bool paused = demo && demo->IsDemoPaused(); // a paused demo reports IsPlayingDemo() false
	if (!playing && !paused) {
		// IsPlayingDemo() also blips false transiently DURING a demo_gototick seek -- exactly
		// the state our restart puts the engine in. Dropping state here would re-read the
		// still-past-the-end tick next frame and instantly re-pause, killing the restart.
		if (s_restarting && GetTickCount64() <= s_restartDeadlineMs)
			return;
		Reset();
		return;
	}

	// Re-resolve the demo's tick count whenever the playing demo changes. Header read is a
	// small bounded file read, done once per demo.
	std::wstring path = PlayingDemoPath();
	if (!path.empty() && path != s_path) {
		s_path = path;
		s_holding = false;
		s_restarting = false;
		DemoHeaderInfo hi = ReadDemoHeader(path);
		s_totalTicks = (hi.ok && hi.playbackTicks >= kMinTotalTicks) ? hi.playbackTicks : -1;
	}
	if (s_totalTicks <= 0)
		return;

	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick))
		return;

	int startTick = demo ? demo->GetDemoStartTick() : 0;
	if (startTick < 0) startTick = 0;
	const int holdTick = startTick + s_totalTicks - kEndMarginTicks;

	if (s_restarting) {
		// The seek back to the start replays packets over several frames, during which the
		// current tick still reads at/past the hold point. Don't re-arm the pause until the
		// tick is clearly below it (or the seek evidently never landed).
		if (tick < holdTick || GetTickCount64() > s_restartDeadlineMs)
			s_restarting = false;
		else
			return;
	}

	if (tick < holdTick) { s_holding = false; return; }

	// At/past the hold point.
	if (paused) {
		// Any pause at the end counts as the hold (ours or the user's own), so a single
		// play press from here restarts from the beginning.
		s_holding = true;
		return;
	}
	if (s_holding) {
		// We were holding at the end and playback is running again = the user pressed play.
		// Restart from the beginning instead of letting the engine run into the stop record.
		s_holding = false;
		s_restarting = true;
		s_restartDeadlineMs = GetTickCount64() + 15000;
		char cmd[64];
		sprintf_s(cmd, "demo_gototick %d", startTick);
		DemoCmd(cmd);
		advancedfx::Message("mirv_filmmaker: end of demo -- restarting playback from the beginning.\n");
	} else {
		// First arrival at the end: freeze before the engine can process the demo's stop
		// record (which would disconnect back to the main menu).
		DemoCmd("demo_pause");
		s_holding = true;
		advancedfx::Message("mirv_filmmaker: end of demo -- holding on the last tick (play again restarts from the beginning).\n");
	}
}

} // namespace Filmmaker
