#pragma once

// INTERNAL shared surface of the ParticleFx subsystem. Not part of the public API --
// include ParticleFx.h for that. This header exists so the subsystem can be split into
// focused translation units while the runtime behavior (one mutex over the rule/telemetry
// state, lock-free atomics for the hook's hot path) stays exactly what the original
// single-file implementation had:
//
//   ParticleFx.cpp            core: engine binding, apply-reseek, public class methods,
//                             the `fx` console command dispatch
//   ParticleFxRules.cpp       category classification + the FXRULE swap tables +
//                             target selection + swap-target pre-queueing
//   ParticleFxHook.cpp        SEH-guarded engine reads, vtable resolution, the
//                             create-collection detour, handle resolution + JIT redirect
//   ParticleFxSpray.cpp       spray-gated barrel smoke (SprayHeat/SprayPair)
//   ParticleFxMoney.cpp       money-on-headshot candidates + game-event plumbing
//   ParticleFxDiagnostics.cpp FxDebugHud state, telemetry ring/name stats, agent logs,
//                             watched-player fire window
//   ParticleFxSettings.cpp    JSON persistence (filmmaker_fx.json)
//
// Everything here lives in Filmmaker::fx (internal); the public entry points stay in
// Filmmaker as declared by ParticleFx.h.

#include "ParticleFx.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Filmmaker {
namespace fx {

// ============================== shared types =======================================

struct CustomRule {
	std::string match;  // lowercase substring of the full resource path
	std::string target; // exact target resource path to swap to; empty = block (empty system)
};

struct FxEvent {
	std::string name;   // as read off the handle
	char action;        // '=' pass, 'X' blocked, '>' swapped
	std::string target; // for '>'
};

struct NameStat {
	unsigned long long seen = 0;
	unsigned long long acted = 0;
};

struct HandleCacheEntry {
	void* handle = nullptr;
	unsigned long long retryAtMs = 0; // for failed resolves: don't re-resolve until then
};

struct SprayPair {
	const char* base;  // the swap target a variant rule produced
	const char* spray; // the wrapper to use instead while spraying
};

struct SprayHeat {
	int lastTick = -0x40000000;
	int count = 0;
};

constexpr size_t kRingCap = 128;
constexpr size_t kNameStatsCap = 8192;

// The engine's stock do-nothing system: what "blocked" categories are swapped to, so the
// engine always gets a valid collection back (returning null risks crashing callers).
constexpr const char* kEmptySystem = "particles/dev/empty.vpcf";
constexpr const char* kMoneyBurst = "particles/filmmaker/povarehok/regular/impact_fxmoney/impact_helmet_headshot.vpcf";

// Demo-tick based so playback speed does not distort the window (an 0.1x slow-mo
// spray is still a spray). See ParticleFxSpray.cpp.
constexpr int kSprayWindowTicks = 32; // ~0.5s at 64 tick between consecutive shots
constexpr int kSprayHotCount = 4;
constexpr int kPovarehokSprayHotCount = 1;

// ============================== core state (ParticleFx.cpp) ========================

// One mutex guards all mutable rule/telemetry state. Creation frequency is low (dozens per
// second at the wildest), so a plain mutex in the hook is fine; the engine call used for
// swap resolution is made OUTSIDE the lock.
extern std::mutex g_mx;
extern bool g_enabled;
extern FxMode g_modes[kFxCategoryCount];
extern std::vector<CustomRule> g_customRules;
extern bool g_logging;
extern bool g_moneyHeadshot; // guarded by g_mx
extern std::atomic<unsigned long long> g_applyAtMs;
// Published by PumpMainThread each frame: the hook runs on whatever thread creates
// particles, and polling the engine's demo interface there is not established as
// safe, so it reads this atomic instead.
extern std::atomic<int> g_demoTickNow;

bool DemoIsPlaying();
bool DemoIsPaused();
int DemoTick();
void ApplyReseekNow();
void RequestApplyReseek();

// ============================== rules (ParticleFxRules.cpp) ========================

extern const char* kCategoryKeys[kFxCategoryCount];
extern const char* kModeNames[5];
constexpr int kModeCount = 5;

void LowerCopy(const char* in, char* out, size_t cap);
int ClassifyLower(const char* n);                    // lowercase name -> FxCategory or -1
bool ModeSupported(int cat, FxMode mode);
FxMode NormalizeMode(int cat, FxMode mode);
int CategoryFromKey(const char* key);
int ModeFromName(const char* name);
const char* VariantTargetLower(int cat, FxMode mode, const char* n);
// g_mx held. True only when the master switch and at least one category/auxiliary FX
// feature are active. Logging is deliberately excluded: it needs the hook, not assets.
bool HasActiveFxLocked();
// Reconcile the authoritative active-target set and pending resolver queue with current
// settings. Master-off always clears active/pending targets. Resolved handles are retained
// on master-off, but an explicit mode/rule change can invalidate obsolete handles.
void RebuildActiveSwapTargetsLocked(bool invalidateObsoleteHandles);
void RebuildActiveSwapTargets(bool invalidateObsoleteHandles);

// ============================== hook (ParticleFxHook.cpp) ==========================

extern std::atomic<bool> g_installed;
extern bool g_installFailedHard;     // shape mismatch: stop retrying, feature inactive
extern unsigned long long g_lastInstallTryMs;
extern bool g_jitRedirectInstalled;
extern std::unordered_map<std::string, HandleCacheEntry> g_handleCache; // guarded by g_mx
// Names current settings permit the hook to resolve. Checking this set prevents an
// in-flight creation from requeueing an obsolete target after FX is disabled or changed.
extern std::vector<std::string> g_activeTargets; // guarded by g_mx
// Swap-target names waiting for MAIN-THREAD resolution (guarded by g_mx).
extern std::vector<std::string> g_resolveQueue;

bool TryInstall();
// MAIN THREAD ONLY (see the crash lesson at its definition).
void ResolveHandleOnMainThread(const char* name);

// ============================== spray (ParticleFxSpray.cpp) ========================

extern std::atomic<bool> g_sprayGateBypass;
extern std::map<std::string, SprayHeat> g_sprayHeat; // guarded by g_mx

const SprayPair* SprayPairs(size_t& count);
const char* SprayUpgradeFor(const char* target);
// g_mx held. Counts this creation of `low` and returns true once `hotCount` consecutive
// shots within kSprayWindowTicks have fired. Same-tick repeats do not accumulate.
bool SprayHotLocked(const char* low, int hotCount);

// ============================== money (ParticleFxMoney.cpp) ========================

bool IsMoneyHeadshotCandidateLower(const char* n);

// ==================== diagnostics (ParticleFxDiagnostics.cpp) ======================

// Telemetry (guarded by g_mx).
extern std::vector<FxEvent> g_ring; // capped ring, g_ringNext = oldest slot
extern size_t g_ringNext;
extern std::map<std::string, NameStat> g_nameStats;
extern std::atomic<unsigned long long> g_totalSeen;
extern std::atomic<unsigned long long> g_totalNoName;
extern std::atomic<unsigned long long> g_totalBlocked;
extern std::atomic<unsigned long long> g_totalSwapped;

// FxDebugHud state (see the block comment in ParticleFxDiagnostics.cpp).
extern std::atomic<bool> g_debugHudEnabled;
extern std::atomic<unsigned long long> g_dbgMuzzleMs, g_dbgTracerMs, g_dbgOnSmokeMs,
	g_dbgOnWispMs, g_dbgModSmokeMs, g_dbgModWispMs;
extern std::atomic<unsigned long long> g_dbgMuzzleN, g_dbgTracerN, g_dbgOnSmokeN,
	g_dbgOnWispN, g_dbgModSmokeN, g_dbgModWispN;
// Watched-player gate: light squares only when a swap coincides with weapon_fire from the
// spectated POV player (not every other player shooting elsewhere in the demo).
extern std::atomic<int> g_watchedUserId;
extern std::atomic<int> g_watchedFireDemoTick;
extern std::atomic<unsigned long long> g_watchedFireWallMs;

// Flicker-investigation dedup map (agent log, session 7803fe).
extern std::mutex g_dbgFlickerMx;
extern std::unordered_map<std::string, unsigned long long> g_dbgLastCreateMs;

void RecordEventLocked(const char* name, char action, const char* target); // g_mx held
void RecordNameLocked(const char* lowName, bool acted);                    // g_mx held
int UserIdForSpectatedPawn();
bool IsFirstPersonWeaponFxPath(const char* low);
bool IsWithinWatchedFireWindow(int demoTick);
bool ShouldUpdateFxDebugHud(const char* vanillaLow, int demoTick, char action);

// #region agent log helpers (debug sessions 43a665 / 7803fe)
void DbgAgentLog(const char* hypothesisId, const char* location, const char* message,
	const char* vanilla, const char* baseTarget, const char* finalTarget, int sprayCount,
	int demoTick, bool sprayHot, bool sprayUpgraded);
bool DbgIsBarrelSmokePath(const char* s);
void DbgFlickerLog(const char* hypothesisId, const char* message, const char* raw,
	const char* target, bool isComposition, bool isDuplicate, long long msSinceSameName,
	int demoTick);
bool DbgIsFlickerRelevant(const char* s);
const char* DbgModernAlignClass(const std::string& target);
void DbgWatchLog(const char* hypothesisId, const char* message, int uid, int watched,
	int demoTick, const char* path = nullptr);
// #endregion

} // namespace fx
} // namespace Filmmaker
