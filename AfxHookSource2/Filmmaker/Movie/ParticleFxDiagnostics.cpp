// ParticleFx diagnostics: FxDebugHud state + watched-player fire window, the telemetry
// ring / per-name stats behind `fx recent` / `fx names`, and the agent-log file writers
// used by the debug sessions. Read-mostly; all mutation happens under g_mx or via atomics.

#include "ParticleFxInternal.h"

#include "../../ClientEntitySystem.h" // AfxGetSpectatedPawnIndex (FxDebugHud watched-player gate)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstring>
#include <fstream>

namespace Filmmaker {
namespace fx {

// ============================== telemetry state ====================================

std::vector<FxEvent> g_ring;
size_t g_ringNext = 0;
std::map<std::string, NameStat> g_nameStats;

std::atomic<unsigned long long> g_totalSeen{ 0 };
std::atomic<unsigned long long> g_totalNoName{ 0 };
std::atomic<unsigned long long> g_totalBlocked{ 0 };
std::atomic<unsigned long long> g_totalSwapped{ 0 };

// #region FxDebugHud squares.
// Timestamps, not booleans, so FxDebugHud can fade the square out a beat after the swap
// instead of it strobing on/off every frame. Gated on g_debugHudEnabled so it costs
// nothing when off.
std::atomic<bool> g_debugHudEnabled{ false };
std::atomic<unsigned long long> g_dbgMuzzleMs{ 0 };
std::atomic<unsigned long long> g_dbgTracerMs{ 0 };
std::atomic<unsigned long long> g_dbgOnSmokeMs{ 0 };
std::atomic<unsigned long long> g_dbgModSmokeMs{ 0 };
// Per-event counters (one increment per swap): the HUD blinks squares on counter parity
// so each bullet visibly toggles 1-0-1-0 regardless of demo_timescale.
std::atomic<unsigned long long> g_dbgMuzzleN{ 0 };
std::atomic<unsigned long long> g_dbgTracerN{ 0 };
std::atomic<unsigned long long> g_dbgOnSmokeN{ 0 };
std::atomic<unsigned long long> g_dbgModSmokeN{ 0 };
// Watched-player gate: light squares only when a swap coincides with weapon_fire from the
// spectated POV player (not every other player shooting elsewhere in the demo).
std::atomic<int> g_watchedUserId{ -1 };
std::atomic<int> g_watchedFireDemoTick{ -1 };
std::atomic<unsigned long long> g_watchedFireWallMs{ 0 };
namespace {
constexpr int kWatchedFireWindowTicks = 128; // wide enough for demo_timescale > 1
constexpr int kWatchedFireLeadTicks = 4;     // particle create often precedes weapon_fire on same shot
constexpr unsigned long long kWatchedFireWindowWallMs = 750;
} // namespace
// #endregion

std::mutex g_dbgFlickerMx;
std::unordered_map<std::string, unsigned long long> g_dbgLastCreateMs; // low name -> last create wall-clock ms

// ============================== telemetry helpers (g_mx held) ======================

void RecordEventLocked(const char* name, char action, const char* target) {
	if (g_ring.size() < kRingCap) {
		g_ring.push_back({ name, action, target ? target : "" });
	} else {
		g_ring[g_ringNext] = { name, action, target ? target : "" };
		g_ringNext = (g_ringNext + 1) % kRingCap;
	}
}

void RecordNameLocked(const char* lowName, bool acted) {
	if (g_nameStats.size() >= kNameStatsCap && g_nameStats.find(lowName) == g_nameStats.end())
		return;
	NameStat& s = g_nameStats[lowName];
	++s.seen;
	if (acted)
		++s.acted;
}

// ============================== watched-player window ==============================

// CS2 game-event userid for the player pawn currently being spectated, or -1 if unknown.
// userid == player-controller entity index - 1 (DeathMsg.cpp convention).
int UserIdForSpectatedPawn() {
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return -1;
	int pawn = AfxGetSpectatedPawnIndex();
	if (pawn < 0) {
		// Locally-recorded demo: the "watched" player is the local viewer itself (there
		// is no observer target). Same fallback FxAlign's muzzle resolution uses.
		CEntityInstance* lp = AfxGetLocalViewerPawn();
		if (lp && lp->IsPlayerPawn()) {
			const auto h = lp->GetHandle();
			if (h.IsValid())
				pawn = h.GetEntryIndex();
		}
	}
	if (pawn < 0)
		return -1;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (!ent || !ent->IsPlayerController())
			continue;
		if (ent->GetPlayerPawnHandle().GetEntryIndex() == pawn)
			return i - 1;
	}
	return -1;
}

// First-person viewmodel weapon FX paths (spectated POV player's own shots).
bool IsFirstPersonWeaponFxPath(const char* low) {
	if (!low || !low[0])
		return false;
	if (std::strstr(low, "_fps") != nullptr)
		return true;
	if (std::strstr(low, "_fp.") != nullptr)
		return true;
	// weapon_muzzle_flash_*_fp.vpcf and similar (no trailing 's').
	const char* fp = std::strstr(low, "_fp");
	if (!fp)
		return false;
	const char next = fp[3];
	return next == 0 || next == '.' || next == '_';
}

// FxDebugHud: only the spectated first-person player's weapon FX (not everyone in the demo).
bool IsWithinWatchedFireWindow(int demoTick) {
	const unsigned long long nowMs = GetTickCount64();
	const unsigned long long fireMs = g_watchedFireWallMs.load(std::memory_order_relaxed);
	if (fireMs != 0 && nowMs >= fireMs && (nowMs - fireMs) <= kWatchedFireWindowWallMs)
		return true;
	const int fireTick = g_watchedFireDemoTick.load(std::memory_order_relaxed);
	if (fireTick < 0 || demoTick < 0)
		return false;
	if (demoTick + kWatchedFireLeadTicks < fireTick)
		return false;
	return (demoTick - fireTick) <= kWatchedFireWindowTicks;
}

bool ShouldUpdateFxDebugHud(const char* vanillaLow, int demoTick, char action) {
	if (g_watchedUserId.load(std::memory_order_relaxed) < 0)
		return false;
	const int cat = ClassifyLower(vanillaLow);
	// Bullet impacts share the weapon_fire tick but are not weapon FX -- do not arm squares.
	if (cat != kFxWeaponFx && cat != kFxTracers)
		return false;
	if (IsFirstPersonWeaponFxPath(vanillaLow))
		return true;
	if (action != '=')
		return IsWithinWatchedFireWindow(demoTick);
	return IsWithinWatchedFireWindow(demoTick);
}

// ============================== agent-log writers ==================================

namespace {

// #region agent log
void DbgAgentLogJsonEscape(std::string* out, const char* s) {
	if (!s) return;
	for (; *s; ++s) {
		const char c = *s;
		if (c == '\\' || c == '"') out->push_back('\\');
		out->push_back(c);
	}
}
// #endregion

} // namespace

// #region agent log
void DbgAgentLog(const char* hypothesisId, const char* location, const char* message,
	const char* vanilla, const char* baseTarget, const char* finalTarget, int demoTick) {
	static const wchar_t* kLogPath =
		L"c:\\Users\\ayden\\Documents\\Github Projects\\cs2 filmaker\\debug-43a665.log";
	std::ofstream f(kLogPath, std::ios::app);
	if (!f)
		return;
	std::string v, b, t;
	DbgAgentLogJsonEscape(&v, vanilla);
	DbgAgentLogJsonEscape(&b, baseTarget);
	DbgAgentLogJsonEscape(&t, finalTarget);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	f << "{\"sessionId\":\"43a665\",\"hypothesisId\":\"" << (hypothesisId ? hypothesisId : "")
	  << "\",\"location\":\"" << (location ? location : "")
	  << "\",\"message\":\"" << (message ? message : "")
	  << "\",\"data\":{\"vanilla\":\"" << v
	  << "\",\"baseTarget\":\"" << b
	  << "\",\"finalTarget\":\"" << t
	  << "\",\"demoTick\":" << demoTick
	  << "},\"timestamp\":" << ms << "}\n";
}

bool DbgIsBarrelSmokePath(const char* s) {
	if (!s || !s[0])
		return false;
	return std::strstr(s, "muzzle") || std::strstr(s, "smoke") || std::strstr(s, "barrel")
		|| std::strstr(s, "spray") || std::strstr(s, "plume");
}
// #endregion

// #region agent log (debug session 7803fe: "muzzle flash/spark flicker" investigation.
// H-dup: ParticleFx.h documents the hook also catching an "internal direct-call
// (profiling-duplicate) path" -- if the SAME logical shot enters Hook_CreateBody twice
// in near-zero time, CS2's per-attachment bookkeeping could kill/replace the first
// instance almost immediately, reading as a flicker. H-comp: the spray-wrapper/sniper/
// grenade "composition" targets (_composition_text in postprocess_common.py) are
// bare m_Children-only containers with no emitter of their own; logged separately so a
// reported flicker can be correlated against whether the target was a composition.)
void DbgFlickerLog(const char* hypothesisId, const char* message, const char* raw,
	const char* target, bool isComposition, bool isDuplicate, long long msSinceSameName,
	int demoTick) {
	static const wchar_t* kLogPath =
		L"c:\\Users\\ayden\\Documents\\Github Projects\\cs2 filmaker\\debug-7803fe.log";
	std::ofstream f(kLogPath, std::ios::app);
	if (!f)
		return;
	std::string r, t;
	DbgAgentLogJsonEscape(&r, raw);
	DbgAgentLogJsonEscape(&t, target);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	f << "{\"sessionId\":\"7803fe\",\"hypothesisId\":\"" << (hypothesisId ? hypothesisId : "")
	  << "\",\"location\":\"ParticleFx.cpp:Hook_CreateBody\""
	  << ",\"message\":\"" << (message ? message : "")
	  << "\",\"data\":{\"raw\":\"" << r
	  << "\",\"target\":\"" << t
	  << "\",\"isComposition\":" << (isComposition ? "true" : "false")
	  << ",\"isDuplicate\":" << (isDuplicate ? "true" : "false")
	  << ",\"msSinceSameName\":" << msSinceSameName
	  << ",\"demoTick\":" << demoTick
	  << "},\"timestamp\":" << ms << "}\n";
}

bool DbgIsFlickerRelevant(const char* s) {
	if (!s || !s[0])
		return false;
	return std::strstr(s, "muz") || std::strstr(s, "spark") || std::strstr(s, "smoke")
		|| std::strstr(s, "barrel") || std::strstr(s, "tracer");
}
// #endregion

// #region agent log (FxDebugHud watched-player filter, session 7803fe)
const char* DbgModernAlignClass(const std::string& target) {
	if (target.find("muzzleflash_ar") != std::string::npos)
		return "assaultrifle";
	if (target.find("muzzleflash_smg") != std::string::npos)
		return "smg";
	if (target.find("muzzleflash_shotgun") != std::string::npos)
		return "shotgun";
	if (target.find("muzzleflash_pistol_deagle") != std::string::npos)
		return "deagle";
	if (target.find("muzzleflash_pistol") != std::string::npos)
		return "pistol";
	if (target.find("muzzleflash_lmg") != std::string::npos)
		return "lmg";
	if (target.find("muzzleflash_dmr") != std::string::npos)
		return "autosniper";
	if (target.find("muzzleflash_suppressed") != std::string::npos)
		return "rifle_silenced";
	if (target.find("mvm_muzzleflash_sniper_awp") != std::string::npos)
		return "awp";
	if (target.find("mvm_muzzleflash_sniper_auto") != std::string::npos)
		return "autosniper";
	return nullptr;
}

void DbgWatchLog(const char* hypothesisId, const char* message, int uid, int watched,
	int demoTick, const char* path) {
	static const wchar_t* kLogPath =
		L"c:\\Users\\ayden\\Documents\\Github Projects\\cs2 filmaker\\debug-7803fe.log";
	std::ofstream f(kLogPath, std::ios::app);
	if (!f)
		return;
	std::string p;
	if (path)
		DbgAgentLogJsonEscape(&p, path);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	f << "{\"sessionId\":\"7803fe\",\"hypothesisId\":\"" << (hypothesisId ? hypothesisId : "")
	  << "\",\"location\":\"ParticleFx.cpp:watch-filter\""
	  << ",\"message\":\"" << (message ? message : "")
	  << "\",\"data\":{\"uid\":" << uid << ",\"watchedUserId\":" << watched
	  << ",\"demoTick\":" << demoTick;
	if (path)
		f << ",\"path\":\"" << p << "\"";
	f << "},\"timestamp\":" << ms << ",\"runId\":\"watch-filter\"}\n";
}
// #endregion

} // namespace fx

// ============================== public accessors ===================================

using namespace fx;

bool ParticleFx_WatchedFireWindow(int demoTick) {
	// No g_watchedUserId >= 0 precondition here: that value is re-derived every frame and
	// flickers to -1 between frames (observed live 2026-07-04), while the fire tick below
	// is only ever SET by a weapon_fire whose uid matched the watched player -- it being
	// recent is the whole signal.
	return IsWithinWatchedFireWindow(demoTick);
}

bool ParticleFx_DebugHudEnabled() {
	return g_debugHudEnabled.load(std::memory_order_relaxed);
}

void ParticleFx_GetDebugHudState(FxDebugHudState& out) {
	out.enabled = g_debugHudEnabled.load(std::memory_order_relaxed);
	out.nowMs = GetTickCount64();
	out.muzzleMs = g_dbgMuzzleMs.load(std::memory_order_relaxed);
	out.tracerMs = g_dbgTracerMs.load(std::memory_order_relaxed);
	out.onSmokeMs = g_dbgOnSmokeMs.load(std::memory_order_relaxed);
	out.modSmokeMs = g_dbgModSmokeMs.load(std::memory_order_relaxed);
	out.muzzleN = g_dbgMuzzleN.load(std::memory_order_relaxed);
	out.tracerN = g_dbgTracerN.load(std::memory_order_relaxed);
	out.onSmokeN = g_dbgOnSmokeN.load(std::memory_order_relaxed);
	out.modSmokeN = g_dbgModSmokeN.load(std::memory_order_relaxed);
}

} // namespace Filmmaker
