// ParticleFx core: engine binding + demo state, the apply-now reseek, the ParticleFx
// class' public methods, the main-thread pump, and the `fx` console command dispatch.
// The subsystem is split into focused translation units -- see the map in
// ParticleFxInternal.h (rules / hook / spray / money / settings / diagnostics).

#include "ParticleFxInternal.h"

#include "FxAlign.h"                       // Modern muzzle-FX alignment probe (fx align)
#include "../Filmmaker.h"                  // PlayingDemoPath (level-change detection)
#include "../Cosmetics/CosmeticDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)
#include "../Platform/JsonBuilder.h"
#include "../../../shared/AfxConsole.h"    // advancedfx::Message/Warning + ICommandArgs

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

// Engine pointer (same one CameraPath/MovieMode use) for the toggle-time re-apply seek.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Filmmaker {
namespace fx {

// ============================== core state =========================================

// One mutex guards all mutable rule/telemetry state. Creation frequency is low (dozens per
// second at the wildest), so a plain mutex in the hook is fine; the engine call used for
// swap resolution is made OUTSIDE the lock.
std::mutex g_mx;
bool g_enabled = true;
FxMode g_modes[kFxCategoryCount] = {}; // default FxMode::On
std::vector<CustomRule> g_customRules;
bool g_logging = false;
bool g_moneyHeadshot = false; // guarded by g_mx
// Published by PumpMainThread each frame: the hook runs on whatever thread creates
// particles, and polling the engine's demo interface there is not established as
// safe, so it reads this atomic instead.
std::atomic<int> g_demoTickNow{ -1 };

// ============================== apply-now reseek ===================================
// The hook only affects particle systems at CREATION time; systems already alive (and
// frozen on screen while the demo is paused) are untouched by a toggle, which reads as
// "the setting does nothing" / "it stays loaded after I change it". The engine-native
// fix: a tiny backward demo seek destroys every live particle (full update) and replays
// the recent event stream, re-creating the moment's effects UNDER THE NEW RULES -- More,
// On, Less, and Off/default swap/pass-through choices visibly replace each other. Any
// settings change requests this (debounced 400ms so a burst of UI clicks costs one seek);
// the pump performs it only while the demo is PAUSED --
// during playback new effects appear within seconds anyway and a seek would hitch.
// (`fx apply` forces one immediately regardless of pause.) Note surface DECALS (blood
// splatter on walls) are not particles and are unaffected.
std::atomic<unsigned long long> g_applyAtMs{ 0 };

bool DemoIsPlaying() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsPlayingDemo();
	return false;
}
bool DemoIsPaused() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsDemoPaused();
	return false;
}
int DemoTick() {
	if (!g_pEngineToClient) return -1;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->GetDemoTick();
	return -1;
}

void ApplyReseekNow() {
	const int tick = DemoTick();
	if (tick <= 1 || !g_pEngineToClient)
		return;
	// One tick back: guarantees a real backward seek (goal == current can no-op), and
	// ~15ms of position change is imperceptible.
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_gototick %d", tick - 1);
	g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.apply", "reseek to %d (paused=%d) to re-create live effects", tick - 1,
			DemoIsPaused() ? 1 : 0);
}

void RequestApplyReseek() {
	if (!DemoIsPlaying())
		return;
	g_applyAtMs.store(GetTickCount64() + 400);
}

} // namespace fx

// ============================== public API =========================================

using namespace fx;

ParticleFx& ParticleFxRef() {
	static ParticleFx s_instance;
	return s_instance;
}

const char* FxModeName(FxMode mode) { return kModeNames[(int)mode < kModeCount ? (int)mode : 0]; }
const char* FxCategoryKey(FxCategory cat) {
	return (cat >= 0 && cat < kFxCategoryCount) ? kCategoryKeys[cat] : "?";
}
bool FxModeSupported(FxCategory cat, FxMode mode) {
	return cat >= 0 && cat < kFxCategoryCount && ModeSupported(cat, mode);
}

bool ParticleFx::EnsureInstalled() { return TryInstall(); }
bool ParticleFx::Installed() const { return g_installed.load(); }

bool ParticleFx::WantsHook() const {
	std::lock_guard<std::mutex> lock(g_mx);
	if (g_logging)
		return true;
	return g_enabled;
}

bool ParticleFx::MoneyHeadshot() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_moneyHeadshot;
}

void ParticleFx::SetMoneyHeadshot(bool on) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_moneyHeadshot = on;
		QueueActiveSwapTargetsLocked();
	}
	if (on)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
}

void ParticleFx::PumpMainThread() {
	// Spray-heat clock for the create hook (safe engine access is main-thread/pump
	// only; the hook reads the atomic). -1 while no demo is playing = gate closed.
	g_demoTickNow.store(DemoIsPlaying() ? DemoTick() : -1, std::memory_order_relaxed);
	// The spectated-pawn -> userid lookup intermittently misses for single frames during
	// playback (observed live 2026-07-04: it flickered to -1 BETWEEN weapon_fire events,
	// so the FxDebugHud/FxAlign fire-window never armed). Hold the last good value for a
	// short decay so one bad frame doesn't drop the watched player.
	{
		static int s_lastGoodUid = -1;                 // main-thread only
		static unsigned long long s_lastGoodMs = 0;
		int uid = DemoIsPlaying() ? UserIdForSpectatedPawn() : -1;
		const unsigned long long nowMs = GetTickCount64();
		if (uid >= 0) {
			s_lastGoodUid = uid;
			s_lastGoodMs = nowMs;
		} else if (DemoIsPlaying() && s_lastGoodUid >= 0 && nowMs - s_lastGoodMs < 2000) {
			uid = s_lastGoodUid;
		}
		g_watchedUserId.store(uid, std::memory_order_relaxed);
	}
	// Alignment probe: drain queued swap creations (entity + attachment resolution is
	// main-thread work). No-op while "fx align" is off.
	FxAlign_PumpMainThread();
	if (g_installed.load()) {
		// CRASH LESSON 4 (2026-07-02, mvm_debug_091656 caught it): handles resolved at
		// the MAIN MENU dangle once a demo loads (map transition purges resources); the
		// first swap then hands the engine freed memory and its child-walk dies at
		// particles.dll+0x3E3xx. So the cache is tied to the level: any demo/level
		// change drops every cached handle and re-resolves in the NEW resource context.
		static std::wstring s_lastDemoPath; // main-thread only
		const std::wstring cur = PlayingDemoPath();
		if (cur != s_lastDemoPath) {
			s_lastDemoPath = cur;
			std::lock_guard<std::mutex> lock(g_mx);
			g_handleCache.clear();
			g_resolveQueue.clear();
			QueueActiveSwapTargetsLocked();
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("fx.resolve", "level change -> handle cache cleared, %zu target(s) requeued",
					g_resolveQueue.size());
		}
		// Drain swap-target resolves here, in the engine's own precache context. One per
		// frame: a cold resolve blocking-loads the resource and can hitch.
		std::string name;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			if (!g_resolveQueue.empty()) {
				name = g_resolveQueue.front();
				g_resolveQueue.erase(g_resolveQueue.begin());
			}
		}
		if (!name.empty())
			ResolveHandleOnMainThread(name.c_str());
		// Debounced apply-now: re-create the paused moment's live effects under the new
		// rules (see the g_applyAtMs comment). Skipped while playing -- self-corrects.
		const unsigned long long applyAt = g_applyAtMs.load();
		if (applyAt && GetTickCount64() >= applyAt) {
			g_applyAtMs.store(0);
			if (DemoIsPlaying() && DemoIsPaused())
				ApplyReseekNow();
		}
		return;
	}
	if (g_installFailedHard)
		return;
	if (!WantsHook())
		return;
	const unsigned long long now = GetTickCount64();
	if (now - g_lastInstallTryMs < 3000)
		return;
	g_lastInstallTryMs = now;
	TryInstall();
}

bool ParticleFx::Enabled() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_enabled;
}

void ParticleFx::SetEnabled(bool on) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_enabled = on;
	}
	if (on)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
}

FxMode ParticleFx::Mode(FxCategory cat) const {
	if (cat < 0 || cat >= kFxCategoryCount)
		return FxMode::On;
	std::lock_guard<std::mutex> lock(g_mx);
	return g_modes[cat];
}

void ParticleFx::SetMode(FxCategory cat, FxMode mode) {
	if (cat < 0 || cat >= kFxCategoryCount)
		return;
	mode = NormalizeMode(cat, mode); // More -> On; Less/Modern only where real variants exist
	bool enabled = false;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_modes[cat] = mode;
		QueueActiveSwapTargetsLocked();
		enabled = g_enabled;
	}
	if (enabled)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
}

void ParticleFx::SetLogging(bool on) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_logging = on;
	}
	if (on)
		EnsureInstalled();
}

bool ParticleFx::Logging() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_logging;
}

std::string ParticleFx::DebugStateJson() const {
	JsonBuilder b;
	b.BeginObject();
	b.BoolField("installed", g_installed.load());
	b.BoolField("installFailedHard", g_installFailedHard);
	b.BoolField("sprayGateBypass", g_sprayGateBypass.load(std::memory_order_relaxed));
	b.BoolField("align", FxAlign_Enabled());
	b.BoolField("jitRedirect", g_jitRedirectInstalled);
	b.IntField("seen", (int64_t)g_totalSeen.load());
	b.IntField("noName", (int64_t)g_totalNoName.load());
	b.IntField("blocked", (int64_t)g_totalBlocked.load());
	b.IntField("swapped", (int64_t)g_totalSwapped.load());
	{
		std::lock_guard<std::mutex> lock(g_mx);
		b.BoolField("enabled", g_enabled);
		b.BoolField("logging", g_logging);
		b.BoolField("moneyHeadshot", g_moneyHeadshot);
		b.Key("modes");
		b.BeginObject();
		for (int i = 0; i < kFxCategoryCount; ++i)
			b.StringField(kCategoryKeys[i], kModeNames[(int)g_modes[i]]);
		b.EndObject();
		b.IntField("customRules", (int64_t)g_customRules.size());
		b.IntField("namesTracked", (int64_t)g_nameStats.size());
		b.IntField("handleCache", (int64_t)g_handleCache.size());
		b.IntField("resolveQueue", (int64_t)g_resolveQueue.size());
	}
	b.EndObject();
	return b.Str();
}

// ============================== console command ====================================

namespace {

void PrintFxHelp(const char* cmd) {
	advancedfx::Message(
		"%s fx set <category> <on|less|modern|off> - control one effect category. Categories:\n"
		"    impacts    - bullet-surface impacts (on|less|off)\n"
		"    tracers    - bullet tracers (on|modern|off)\n"
		"    weaponfx   - muzzle flash, muzzle/shell smoke, brass (on|modern|off)\n"
		"    blood      - blood impacts (on|off)\n"
		"    explosions - HE grenade / generic explosions (on|less|modern|off)\n"
		"    bombfx     - the planted bomb blast (on|less|off)\n"
		"    molotov    - molotov / incendiary fire (on|off)\n"
		"    mapfx      - map ambience particles (on|off)\n"
		"  (on = converted Povarehok [regular], less = the mod's reduced\n"
		"   impact/explosion variants [only impacts/explosions/bombfx really differ],\n"
		"   modern = converted MW2019 ARC9 pack, off = default CS2 pass-through; 'more'\n"
		"   is accepted as a legacy alias of on. Unsupported modes snap to on.\n"
		"   If the asset pack is not mounted, the original CS2 effect plays. Smoke GRENADES are never touched - CS2\n"
		"   volumetric smoke is not a particle swap.)\n"
		"%s fx on|off - master switch over all category modes + rules.\n"
		"%s fx state - status + counters.\n"
		"%s fx log on|off - capture every particle creation (view with 'fx recent').\n"
		"%s fx recent [n] - print the last n captured/acted creations (default 30).\n"
		"%s fx names [filter] - aggregated creation counts seen so far.\n"
		"%s fx block <substr> | unblock <substr> - custom block rule (name substring).\n"
		"%s fx swap <substr> <target.vpcf> | unswap <substr> - custom swap rule.\n"
		"%s fx rules - list custom rules.  %s fx test <name> - dry-run classify a name.\n"
		"%s fx apply - re-create the current moment's live effects under the current\n"
		"    settings (tiny backward seek; toggles do this automatically while paused).\n"
		"%s fx moneyshot on|off - money burst replacing the headshot blood/helmet-spark particles (they only spawn on real headshot hits).\n"
		"%s fx debughud on|off - on-screen squares that blink per shot as each effect-class\n"
		"    swap fires (1-0-1-0 per bullet): cyan = Modern, magenta = On/Povarehok.\n"
		"%s fx align ... - Modern muzzle-FX alignment probe: measures muzzle-to-spawn distance\n"
		"    in Source units per weapon class ('%s fx align' for its own help).\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

// Dry-run of the hook's decision logic for one name (no engine calls).
void FxTestName(const char* name) {
	char low[260];
	LowerCopy(name, low, sizeof(low));
	const int cat = ClassifyLower(low);
	char action = '=';
	std::string target;
	std::string why = "untracked category";
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_enabled) {
			why = "master OFF";
		} else {
			for (const CustomRule& r : g_customRules) {
				if (std::strstr(low, r.match.c_str())) {
					action = r.target.empty() ? 'X' : '>';
					target = r.target;
					why = std::string("custom rule '") + r.match + "'";
					break;
				}
			}
			if (action == '=' && cat >= 0) {
				const FxMode m = g_modes[cat];
				why = std::string("category ") + kCategoryKeys[cat] + " = " + kModeNames[(int)m];
				if (m == FxMode::Off) {
					why += " (default CS2 pass-through)";
				} else {
					if (const char* t = VariantTargetLower(cat, m, low)) {
						action = '>';
						target = t;
						why += " (converted variant table match)";
					} else {
						why += " (no converted variant table entry, passes through)";
					}
				}
			}
		}
	}
	if (action == 'X')
		target = kEmptySystem;
	advancedfx::Message("fx test: %s\n  category: %s\n  decision: %s%s%s\n  reason: %s\n",
		name, cat >= 0 ? kCategoryKeys[cat] : "(other)",
		action == '=' ? "PASS" : (action == 'X' ? "BLOCK -> " : "SWAP -> "),
		action == '=' ? "" : target.c_str(), "", why.c_str());
}

} // namespace

void ParticleFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	ParticleFx& fx = ParticleFxRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[fx][state] %s\n", fx.DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(action, "install")) {
		const bool ok = fx.EnsureInstalled();
		advancedfx::Message("fx: hook %s.\n", ok ? "installed" : "NOT installed (see warnings)");
		return;
	}
	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off")) {
		const bool turningOn = (0 == _stricmp(action, "on"));
		fx.SetEnabled(turningOn);
		if (turningOn)
			g_debugHudEnabled.store(true, std::memory_order_relaxed);
		if (!(argc >= 4 && 0 == _stricmp(args->ArgV(3), "quiet")))
			advancedfx::Message("fx: effects control %s%s.\n", fx.Enabled() ? "ON" : "off",
				turningOn ? " (debug squares ON -- top-right; fx debughud off to hide)" : "");
		return;
	}
	if (0 == _stricmp(action, "set")) {
		if (argc < 5) { advancedfx::Warning("usage: %s fx set <category> <on|less|modern|off>\n", cmd); return; }
		const int cat = CategoryFromKey(args->ArgV(3));
		const int mode = ModeFromName(args->ArgV(4));
		if (cat < 0) { advancedfx::Warning("fx: unknown category '%s' (see '%s fx').\n", args->ArgV(3), cmd); return; }
		if (mode < 0) { advancedfx::Warning("fx: unknown mode '%s' (on|less|modern|off).\n", args->ArgV(4)); return; }
		if (!ModeSupported(cat, (FxMode)mode) && (FxMode)mode != FxMode::More)
			advancedfx::Warning("fx: %s does not support '%s' (no distinct assets); using 'on'.\n",
				kCategoryKeys[cat], args->ArgV(4));
		fx.SetMode((FxCategory)cat, (FxMode)mode);
		if (!(argc >= 6 && 0 == _stricmp(args->ArgV(5), "quiet")))
			advancedfx::Message("fx: %s = %s.%s\n", kCategoryKeys[cat], kModeNames[mode],
				fx.Installed() ? "" : " (hook not armed yet - arms once a demo/map has loaded)");
		return;
	}
	if (0 == _stricmp(action, "log")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) fx.SetLogging(true);
		else if (0 == _stricmp(v, "off")) fx.SetLogging(false);
		else { advancedfx::Message("fx: logging is %s (fx log on|off).\n", fx.Logging() ? "ON" : "off"); return; }
		advancedfx::Message("fx: logging %s.%s\n", fx.Logging() ? "ON" : "off",
			fx.Logging() ? " Play the demo, then 'fx recent' / 'fx names'." : "");
		return;
	}
	if (0 == _stricmp(action, "recent")) {
		const size_t n = (argc >= 4) ? (size_t)atoi(args->ArgV(3)) : 30;
		std::lock_guard<std::mutex> lock(g_mx);
		const size_t count = g_ring.size();
		const size_t want = n && n < count ? n : count;
		advancedfx::Message("fx: last %zu creation(s) (of %zu captured; = pass, X blocked, > swapped):\n", want, count);
		for (size_t i = count - want; i < count; ++i) {
			// ring order: g_ringNext is the OLDEST once full
			const FxEvent& e = g_ring[(g_ringNext + i) % (count < kRingCap ? count : kRingCap)];
			if (e.action == '=')
				advancedfx::Message("  = %s\n", e.name.c_str());
			else
				advancedfx::Message("  %c %s -> %s\n", e.action, e.name.c_str(), e.target.c_str());
		}
		return;
	}
	if (0 == _stricmp(action, "names")) {
		const char* filter = (argc >= 4) ? args->ArgV(3) : nullptr;
		std::vector<std::pair<std::string, NameStat>> rows;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			for (const auto& [name, stat] : g_nameStats)
				if (!filter || std::strstr(name.c_str(), filter))
					rows.push_back({ name, stat });
		}
		std::sort(rows.begin(), rows.end(),
			[](const auto& a, const auto& b) { return a.second.seen > b.second.seen; });
		const size_t cap = 60;
		advancedfx::Message("fx: %zu name(s)%s (top %zu by count; seen/acted):\n",
			rows.size(), filter ? " matching filter" : "", rows.size() < cap ? rows.size() : cap);
		for (size_t i = 0; i < rows.size() && i < cap; ++i)
			advancedfx::Message("  %6llu %5llu  %s\n", rows[i].second.seen, rows[i].second.acted,
				rows[i].first.c_str());
		return;
	}
	if (0 == _stricmp(action, "block") || 0 == _stricmp(action, "swap")) {
		const bool isSwap = 0 == _stricmp(action, "swap");
		if (argc < (isSwap ? 5 : 4)) {
			advancedfx::Warning("usage: %s fx %s <substr>%s\n", cmd, action, isSwap ? " <target.vpcf>" : "");
			return;
		}
		CustomRule r;
		r.match = args->ArgV(3);
		std::transform(r.match.begin(), r.match.end(), r.match.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		if (isSwap)
			r.target = args->ArgV(4);
		{
			std::lock_guard<std::mutex> lock(g_mx);
			g_customRules.push_back(r);
			QueueActiveSwapTargetsLocked();
		}
		fx.EnsureInstalled();
		fx.SaveSettings();
		RequestApplyReseek();
		advancedfx::Message("fx: rule added: '%s' -> %s.\n", r.match.c_str(),
			r.target.empty() ? "BLOCK" : r.target.c_str());
		return;
	}
	if (0 == _stricmp(action, "unblock") || 0 == _stricmp(action, "unswap")) {
		if (argc < 4) { advancedfx::Warning("usage: %s fx %s <substr>\n", cmd, action); return; }
		std::string match = args->ArgV(3);
		std::transform(match.begin(), match.end(), match.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		size_t removed = 0;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			const size_t before = g_customRules.size();
			g_customRules.erase(std::remove_if(g_customRules.begin(), g_customRules.end(),
				[&](const CustomRule& r) { return r.match == match; }), g_customRules.end());
			removed = before - g_customRules.size();
		}
		fx.SaveSettings();
		RequestApplyReseek();
		advancedfx::Message("fx: removed %zu rule(s) matching '%s'.\n", removed, match.c_str());
		return;
	}
	if (0 == _stricmp(action, "moneyshot")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) fx.SetMoneyHeadshot(true);
		else if (0 == _stricmp(v, "off")) fx.SetMoneyHeadshot(false);
		else { advancedfx::Message("fx: moneyshot is %s (fx moneyshot on|off).\n", fx.MoneyHeadshot() ? "ON" : "off"); return; }
		advancedfx::Message("fx: money-on-headshot %s.\n",
			fx.MoneyHeadshot() ? "ON (swaps the headshot-only particles; requires converted money asset)" : "off");
		return;
	}
	if (0 == _stricmp(action, "apply")) {
		// Manual, immediate, regardless of pause state: re-create the current moment's
		// live effects under the current rules (tiny backward seek).
		ApplyReseekNow();
		advancedfx::Message("fx: re-applied settings to live effects (reseek).\n");
		return;
	}
	if (0 == _stricmp(action, "rules")) {
		std::lock_guard<std::mutex> lock(g_mx);
		if (g_customRules.empty()) { advancedfx::Message("fx: no custom rules.\n"); return; }
		for (size_t i = 0; i < g_customRules.size(); ++i)
			advancedfx::Message("  %zu | '%s' -> %s\n", i, g_customRules[i].match.c_str(),
				g_customRules[i].target.empty() ? "BLOCK" : g_customRules[i].target.c_str());
		return;
	}
	if (0 == _stricmp(action, "test")) {
		if (argc < 4) { advancedfx::Warning("usage: %s fx test <particles/....vpcf>\n", cmd); return; }
		FxTestName(args->ArgV(3));
		return;
	}
	if (0 == _stricmp(action, "align")) {
		// Modern muzzle-FX alignment probe (unit-distance measurement; see FxAlign.h).
		FxAlign_RunCommand(argc, args, cmd);
		return;
	}
	if (0 == _stricmp(action, "debughud")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) g_debugHudEnabled.store(true, std::memory_order_relaxed);
		else if (0 == _stricmp(v, "off")) g_debugHudEnabled.store(false, std::memory_order_relaxed);
		else {
			advancedfx::Message("fx: debughud is %s (%s fx debughud on|off).\n",
				g_debugHudEnabled.load(std::memory_order_relaxed) ? "ON" : "off", cmd);
			return;
		}
		advancedfx::Message("fx: debughud %s. Labeled squares blink per shot (each swap "
			"toggles them 1-0-1-0) for muzzle / tracer / On-smoke / On-wisp / Modern-smoke "
			"/ Modern-wisp; they go dark ~1.5s after the last swap.\n",
			g_debugHudEnabled.load(std::memory_order_relaxed) ? "ON" : "off");
		return;
	}
	PrintFxHelp(cmd);
	advancedfx::Message("[fx][state] %s\n", fx.DebugStateJson().c_str());
}

} // namespace Filmmaker
