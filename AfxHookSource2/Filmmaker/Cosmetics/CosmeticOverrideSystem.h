#pragma once

// Offline demo-only cosmetic OVERRIDE SYSTEM.
//
// Once per MAIN-thread frame (Filmmaker::RunMainThreadFrame -> Cosmetics_RunFrame), this walks the
// client entity list, resolves the ORIGINAL OWNER of every econ entity by XUID, and -- if that
// SteamID has a profile -- rewrites the entity's fallback paint-kit/wear/seed/StatTrak so the
// client composites the chosen finish (the nSkinz/Osiris mechanism: C_EconItemView::m_iItemIDHigh
// = -1 makes the client ignore the networked item and use C_EconEntity::m_nFallback* instead).
//
// Keying on owner XUID (not pawn/entity index) is what makes overrides survive weapon switches,
// player/observer switches, round restarts, death/respawn, demo seeks (forward AND backward) and
// entity recreation: every frame we re-discover the entities and re-assert the override. Dropped
// weapons keep their OriginalOwnerXuid, so a dropped gun owned by a profiled player still shows the
// override.
//
// GATING: all application is hard-gated on (a) a demo actually playing (InDemoContext) and (b) the
// econ offsets having resolved (OffsetsAvailable / g_cosmeticsOffsetsOk). A missing offset disables
// cosmetics only -- it never aborts the tool. This is for OFFLINE demo/movie work; never live play.

#include <cstdint>
#include <cstddef>
#include "CosmeticProfile.h"

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Per-frame application stats, surfaced by "cosmetics status" / debug overlay.
struct CosmeticFrameStats {
	int entitiesScanned = 0;   // econ entities walked this frame
	int entitiesMatched = 0;   // owned by a profiled SteamID
	int entitiesPatched = 0;   // had an override written this frame
	int entitiesReverted = 0;  // the engine had clobbered our write and we re-applied it
	int attrListsRead = 0;     // networked dynamic attribute vectors successfully read
	int attrValuesWritten = 0; // def 6/7/8/81 values found and overwritten
	int attrValuesChanged = 0; // values that differed from the requested override
	int attrListsEmpty = 0;    // valid vectors with no matching writable skin attrs
	uint64_t frame = 0;        // monotonically increasing apply-frame counter
	int paintkitBridgeValue = 0; // current cl_paintkit_override bridge value, 0 = inactive/default
	int directCompositeCalls = 0; // experimental Andromeda-style direct composite refresh calls
	int directCompositeResolved = 0; // all direct composite function patterns resolved
	int directCompositeFaulted = 0; // a direct composite refresh access-violated (caught)
	// Model-swap (knife type / agent / gloves / legacy mesh) application stats.
	int modelSwapResolved = 0;   // core model-swap functions (SetModel/SetMeshGroupMask/UpdateSubclass) resolved
	int knifeModelsApplied = 0;  // knife model+type swaps fired this frame
	int weaponMeshFixed = 0;     // weapon skin overrides that adjusted the legacy/CS2 mesh-group mask
	int pawnsScanned = 0;        // player pawns walked for glove/agent apply
	int glovesApplied = 0;       // glove model applies fired this frame
	int agentsApplied = 0;       // agent model swaps fired this frame
};

// Which speculative "visuals are stale" field writes the rebuild step is allowed to perform. These
// were added experimentally to try to force a demo weapon to re-composite its skin; live testing
// (2026-06-28) showed NONE of them trigger the 3D rebuild AND at least one (clearUgc / initialized)
// makes the weapon VANISH from the HUD weapon-switch bar by blanking its econ identity. So every flag
// now DEFAULTS OFF -- normal apply writes only the real skin data (networked attrs + fallback fields),
// which is HUD-safe -- and each is independently toggleable ("cosmetics rebuildflags <name> 0|1") so
// the harmful-vs-useful one can be bisected without writing all of them at once. POD (all bool), so it
// is safe to pass by value into the SEH-guarded write. See docs/cosmetics-recompose-research.md.
struct CosmeticRebuildFlags {
	bool visualsDataSet = false; // C_CSWeaponBase::m_bVisualsDataSet = false (most plausible gate)
	bool clearUgc = false;       // C_CSWeaponBase::m_bClearWeaponIdentifyingUGC = true (HUD-vanish suspect)
	bool reloadEvent = false;    // C_CSWeaponBase::m_nCustomEconReloadEventId = -1 (networked reload token)
	bool initialized = false;    // C_EconItemView::m_bInitialized/m_bInitializedTags = false (desc cache)
	bool attrInit = false;       // C_EconEntity::m_bAttributesInitialized = false (entity attr re-init)
	bool imageCache = false;     // m_bRestoreCustomMaterialAfterPrecache + inventory-image cache hints
	bool attrParity = false;     // CAttributeManager::m_iReapplyProvisionParity ++ (attr cache bump)
};

class CosmeticOverrideSystem {
public:
	// Starts with an empty runtime-only profile store and normalizes the legacy JSON file to empty.
	// Cosmetic changes are intentionally scoped to the current demo session.
	void Init();
	void Shutdown();

	// Apply loop. Call once per MAIN/UI-thread frame. Cheap no-op when disabled, no profiles, no
	// demo playing, or offsets unresolved.
	void RunFrame();

	// --- global state ---
	void SetEnabled(bool e);
	bool Enabled() const { return m_store.Enabled(); }
	// START-CLEAN arming: an override only renders after it is applied THIS demo session (a pick /
	// SetWeapon / enabled 1). Profiles are cleared whenever the demo path changes or playback resets
	// to the beginning, so a reset/reload cannot reapply stale cosmetics to recreated entities.
	void Arm() { m_armed = true; }
	bool Armed() const { return m_armed; }
	void SetDebug(bool e) { m_debug = e; }
	bool Debug() const { return m_debug; }
	bool OffsetsAvailable() const;   // g_cosmeticsOffsetsOk
	bool InDemoContext() const;      // true only while a demo is actively playing

	// --- profile editing (keyed by SteamID64) ---
	// defIndex 0 on SetWeapon = "keep the held weapon's own def, just restyle it"; the slot
	// (primary/secondary) is then resolved from the live weapon at apply time. A non-zero defIndex
	// classifies into primary/secondary via the catalog.
	void SetWeapon(uint64_t steamId, int defIndex, int paintKit, float wear, int seed, int statTrak);
	void SetKnife(uint64_t steamId, int defIndex, int paintKit, float wear, int seed);
	void SetGloves(uint64_t steamId, int defIndex, int paintKit, float wear, int seed);
	void SetAgent(uint64_t steamId, const char* model, int defIndex);
	void ClearPlayer(uint64_t steamId);
	void ClearAll();
	void Save();  // compatibility helper; normal UI mutations are intentionally not persisted

	CosmeticProfileStore& Store() { return m_store; }
	const CosmeticProfileStore& Store() const { return m_store; }
	const CosmeticFrameStats& LastFrameStats() const { return m_lastStats; }

	// --- spectate resolution (for "target current" and debug) ---
	// SteamID64 of the player the local viewer is currently spectating, or 0 if none/not in-eye.
	uint64_t CurrentSpectatedSteamId() const;
	// Entity index of the spectated pawn, or -1.
	int CurrentSpectatedPawnIndex() const;
	// Local observer mode (OBS_MODE_*). 0 = none.
	uint8_t CurrentObserverMode() const;
	// Best-effort current display name for a SteamID (empty if no live controller). Used to stamp
	// the profile's `name` field when an override is set.
	std::string NameForSteamId(uint64_t steamId) const;

	// --- forced material re-composite (refresh path; research/tuning) ---
	// Writing the fallback fields + invalidating the C_EconItemView init flags is sometimes NOT
	// enough to make a demo entity VISUALLY re-composite its skin -- a CBasePlayerWeapon
	// UpdateComposite / UpdateCompositeSec vtable call may be required after the write. The correct
	// CS2 vtable index is build-specific and currently unconfirmed (the historical index 7 faulted on
	// recent builds), so this is OFF by default and the indices are live-tunable via
	// "cosmetics recompose 0|1" / "cosmetics vtidx <comp> <sec>". Every call is SEH-guarded, so a
	// wrong index auto-disables recompose instead of crashing the game.
	void SetRecompose(bool e);
	bool Recompose() const { return m_recompose; }
	bool RecomposeFaulted() const { return m_recomposeFaulted; }
	void GetVtIdx(int* comp, int* sec) const;
	void SetVtIdx(int comp, int sec);
	// Argument passed to the SEH-guarded vtable call (default 1). Set to 0 to test
	// OnDataChanged(DATA_UPDATE_CREATED) -- the candidate skin-rebuild trigger. "cosmetics vtarg".
	void SetVtArg(int a) { m_vtArg = a; }
	int VtArg() const { return m_vtArg; }

	// Opt-in legacy fallback-id mode: when ON, the apply loop also forces C_EconItemView::m_iItemIDHigh
	// = -1 (the m_nFallback* path). Default OFF -- it invalidates the item id (UI shows "no weapon") and
	// is unnecessary now that the networked dynamic attributes are overwritten directly. "cosmetics fallback".
	void SetFallbackId(bool e) { m_fallbackId = e; }
	bool FallbackId() const { return m_fallbackId; }

	// --- visual-cache rebuild (the refresh path; "cosmetics rebuild ...") ---
	// Writing the econ fields/attributes is confirmed to STICK but does not, by itself, make a demo
	// weapon re-composite its skin material (the consuming step is entity-lifecycle driven, not a
	// per-frame poll -- see docs/cosmetics-recompose-research.md). These give a manual, on-demand way
	// to (re)assert the "visuals are stale" field writes (m_bVisualsDataSet=false,
	// m_bClearWeaponIdentifyingUGC=true, bump m_nCustomEconReloadEventId, clear init flags, bump attr
	// parity) on every matched weapon entity, decoupled from the per-frame change detection, plus fire
	// the OPT-IN recompose vcall once (only if armed via "cosmetics recompose 1"). All writes are
	// SEH-guarded. Returns the number of matched weapon entities the rebuild touched.
	int RebuildOnce();
	// Per-frame auto-rebuild: when ON, the apply loop writes the ENABLED stale-mark flags (see
	// RebuildFlags) whenever a value changes; when OFF, the per-frame loop writes only the raw skin
	// values and the stale-mark flags fire only on an explicit "rebuild once". Default ON, but since
	// every rebuild flag now defaults OFF this writes nothing harmful until a flag is enabled.
	// "cosmetics rebuild auto 0|1".
	void SetRebuildAuto(bool e) { m_rebuildAuto = e; }
	bool RebuildAuto() const { return m_rebuildAuto; }

	// Which stale-mark field writes the rebuild step performs (all default OFF -> HUD-safe). Toggle a
	// single flag by name (returns false on an unknown name) or all at once, for bisecting which write
	// breaks the HUD weapon panel vs. which triggers a visual rebuild. "cosmetics rebuildflags ...".
	const CosmeticRebuildFlags& GetRebuildFlags() const { return m_rebuildFlags; }
	bool SetRebuildFlag(const char* name, bool on);
	void SetAllRebuildFlags(bool on);

	// Experimental deploy-time bridge for the one skin path CS2 is known to visually honor:
	// cl_paintkit_override is read while a weapon composite is being BUILT, not after it is cached.
	// When enabled, the main-thread pump sets that global cvar to the currently spectated profiled
	// weapon's paint kit and restores the previous value when there is no matching target or the
	// bridge is disabled. Limitations are intentional and surfaced in the command text: this is global
	// and only affects the next weapon deploy/create, so it is a proof-of-life/workaround while the
	// true per-entity composite read-site hook is still unknown.
	void SetPaintkitBridge(bool e);
	bool PaintkitBridge() const { return m_paintkitBridge; }
	void SetPaintkitBridgeForcedValue(int paintKit) { m_paintkitBridgeForcedValue = paintKit; }
	int PaintkitBridgeForcedValue() const { return m_paintkitBridgeForcedValue; }
	int PaintkitBridgeLastValue() const { return m_paintkitBridgeLastValue; }
	bool PaintkitBridgeCvarFound() const { return m_paintkitBridgeCvarFound; }

	// Experimental Andromeda-style refresh path: after writing the same skin attributes/fallback fields
	// as the normal demo apply loop, directly calls the resolved client.dll weapon composite functions:
	// UpdateCompositeMaterial(weapon + ownerOffset), UpdateCompositeMaterialSet(weapon), UpdateSkin(weapon).
	// This is opt-in only ("cosmetics composite once") because both the signatures and owner offset are
	// CS2-build-specific research inputs. All calls are SEH-guarded and counters are reported in status.
	int CompositeOnce();
	void SetCompositeOwnerOffset(ptrdiff_t offset) { m_compositeOwnerOffset = offset; }
	ptrdiff_t CompositeOwnerOffset() const { return m_compositeOwnerOffset; }

	// AUTO-COMPOSITE (the lever that makes a UI skin pick actually render): when ON, the per-frame
	// apply loop fires the resolved direct-composite refresh on every matched weapon WHENEVER its skin
	// data changed this frame -- i.e. right after a profile is set/changed, and again after a demo seek
	// / weapon redeploy recreates the entity from the authoritative item (the engine rebuilds the
	// composite from the real skin, we detect the delta and re-composite to the override). The call is
	// change-gated, so it is a no-op once the skin is stable -- cheap to leave on. This is the default
	// because the field-write path alone does NOT re-render (see docs/cosmetics-recompose-research.md,
	// BREAKTHROUGH 2026-06-29). Toggle for A/B via "cosmetics autocomposite 0|1".
	void SetAutoComposite(bool e) { m_autoComposite = e; }
	bool AutoComposite() const { return m_autoComposite; }

	// MODEL SWAP (agent/player model, gloves, legacy-vs-CS2 weapon mesh, and optional knife TYPE
	// swap). Master toggle for the client-side model-swap path (SetModel/SetBodyGroup/UpdateSubclass).
	// Default ON for non-knife model refreshes; knife type swaps have a separate default-off gate below
	// because they mutate weapon subclass/model state and can destabilize demo weapon switches.
	void SetModelSwap(bool e) { m_modelSwap = e; }
	bool ModelSwap() const { return m_modelSwap; }
	void SetKnifeModelSwap(bool e) { m_knifeModelSwap = e; }
	bool KnifeModelSwap() const { return m_knifeModelSwap; }

	// Legacy-vs-CS2 mesh-group selection tuning. m_meshLegacyMode: -2 = auto (read the paint kit's
	// IsUseLegacyModel flag from the econ schema), -1 = force modern, 1 = force legacy. m_maskModern /
	// m_maskLegacy are the mesh-group bit values written for each (defaults 1 / 2; the two reference
	// cheats disagree on knife polarity, so these are tunable for in-game A/B). "cosmetics mesh ...".
	void SetMeshLegacyMode(int mode) { m_meshLegacyMode = mode; }
	int MeshLegacyMode() const { return m_meshLegacyMode; }
	void SetMeshMasks(uint64_t modern, uint64_t legacy) { m_maskModern = modern; m_maskLegacy = legacy; }
	uint64_t MaskModern() const { return m_maskModern; }
	uint64_t MaskLegacy() const { return m_maskLegacy; }

	// True if the core model-swap client.dll functions resolved (for status output).
	bool ModelSwapResolved() const;

	// Cumulative (since process start) successful model-swap apply counts -- unlike the per-frame
	// stats these are NOT reset each frame, so a one-shot/gated apply (knife/agent fire once then go
	// quiet) is still observable. A non-zero count is PROOF the apply path executed successfully
	// (the underlying SetModel/SetBodyGroup returned without faulting). Surfaced by "cosmetics status".
	uint64_t TotalKnifeApplied() const { return m_totalKnife; }
	uint64_t TotalWeaponMeshApplied() const { return m_totalWeaponMesh; }
	uint64_t TotalGlovesApplied() const { return m_totalGloves; }
	uint64_t TotalAgentsApplied() const { return m_totalAgent; }

	// TICK NUDGE (the user-requested demo-refresh lever -- confirmed live as the thing that makes body
	// swaps render). A demo pawn only re-derives its rendered model / mesh-group / body-group during
	// LIVE rendered frames, so an agent / glove / knife-type / legacy-mesh swap applied while the demo
	// is PAUSED is written but never re-evaluated on screen (PostDataUpdate handles the weapon SKIN
	// composite in place, but the body/model swaps need real frames). So after a profile change, if the
	// demo is paused, this briefly RESUMES playback for ~m_tickNudgeTicks ticks and then re-pauses --
	// exactly "let the game play ~10 ticks to see the change," done automatically. Debounced (a slider
	// drag coalesces into one nudge) and a no-op when the demo is already playing. "cosmetics ticknudge ...".
	void RequestApplyNudge();                 // schedule a nudge (called on every profile mutation)
	void SetTickNudge(bool e) { m_tickNudge = e; }
	bool TickNudge() const { return m_tickNudge; }
	void SetTickNudgeTicks(int t) { m_tickNudgeTicks = (t < 1) ? 1 : t; }
	int TickNudgeTicks() const { return m_tickNudgeTicks; }
	bool TickNudgeActive() const { return m_nudgePhase != 0; } // currently playing out a nudge
	uint64_t TotalNudges() const { return m_totalNudges; }

private:
	// Shared matched-weapon apply loop, used by both the per-frame pump (RunFrame) and the on-demand
	// rebuild (RebuildOnce). forceStale forces the visuals-stale field writes even when no value
	// changed; fireRebuildCall fires the SEH-guarded recompose vcall when armed; fireDirectComposite
	// fires the experimental resolved-function composite refresh. Returns matched count.
	int ApplyMatchedWeapons(bool forceStale, bool fireRebuildCall, bool fireDirectComposite);
	// Pawn-level cosmetics (gloves + agent model), keyed by owner SteamID like the weapon loop. Walks
	// player controllers -> profiled pawns and applies the glove/agent model swap. Glove apply is
	// change-gated (re-fires on glove/paint change, respawn, or m_bNeedToReApplyGloves) to avoid
	// rebuilding the body group every frame. Returns pawns matched.
	int ApplyPawnCosmetics();
	void UpdatePaintkitBridge();
	// Fire the debounced demo re-seek if a profile change scheduled one (see RequestApplyNudge).
	void MaybeFireTickNudge();
	void ResetSessionOverrides(const char* reason);
	int ResolveSpectatedPaintkitOverride() const;
	bool SetPaintkitBridgeCvar(int value);
	void RestorePaintkitBridgeCvar();

	CosmeticProfileStore m_store;
	bool m_initialized = false;
	bool m_debug = false;
	CosmeticFrameStats m_lastStats;

	// Forced-recompose state (see SetRecompose). Defaults match the retired MirvCosmetics path:
	// OFF, primary vtable index 7 (historical), secondary skipped (-1).
	bool m_recompose = false;
	bool m_recomposeFaulted = false;
	int m_vtComposite = 7;
	int m_vtCompositeSec = -1;
	int m_vtArg = 1;           // argument for the recompose vtable call; see SetVtArg
	bool m_fallbackId = false; // opt-in itemIDHigh=-1 path; see SetFallbackId
	bool m_rebuildAuto = true; // per-frame stale-mark write of the ENABLED flags; see SetRebuildAuto
	CosmeticRebuildFlags m_rebuildFlags; // which stale-mark writes are allowed (all default OFF)
	bool m_paintkitBridge = false;
	bool m_paintkitBridgeCvarFound = false;
	bool m_paintkitBridgeHaveOriginal = false;
	int m_paintkitBridgeOriginalValue = 0;
	int m_paintkitBridgeForcedValue = -1; // -1 = auto from spectated profiled weapon, >0 = forced global paint
	int m_paintkitBridgeLastValue = 0;
	ptrdiff_t m_compositeOwnerOffset = 0x608; // Andromeda 2026 research value; tune with "composite once 0x..."
	bool m_autoComposite = true; // per-frame fire the direct composite on skin-data change; see SetAutoComposite

	// Model-swap state (agent / gloves / legacy mesh, plus opt-in knife type). See SetModelSwap /
	// SetKnifeModelSwap / SetMeshMasks.
	bool m_modelSwap = true;     // master enable for the SetModel/SetBodyGroup model-swap path
	// Knife TYPE swap (def -> model). ON by default so a knife pick in the Customize UI actually changes
	// the model instead of painting the default knife (which is what the user sees otherwise: the paint
	// "did not affect any target materials on knife_default_t"). Historically held off as crash-prone on
	// weapon switch; if it destabilizes demo weapon switches, disable live via
	// "mirv_filmmaker cosmetics modelswap knife 0".
	bool m_knifeModelSwap = true;
	int m_meshLegacyMode = -2;   // -2 auto (econ schema), -1 force modern, 1 force legacy
	uint64_t m_maskModern = 1;   // mesh-group mask for the modern CS2 model
	uint64_t m_maskLegacy = 2;   // mesh-group mask for the legacy model
	// Per-pawn glove apply state, keyed by owner SteamID -- avoids rebuilding the body group every
	// frame. countdown re-asserts the write for a few frames after a change (gloves apply over
	// multiple frames, per Andromeda/nerv). sig = glove def/paint/seed/wear packed; spawn gates respawn.
	struct GloveApplyState {
		uint64_t sig = 0;
		float lastSpawn = -1.0f;
		uintptr_t pawn = 0;
		int frames = 0;
	};
	std::unordered_map<uint64_t, GloveApplyState> m_gloveState;
	struct AgentApplyState { uint64_t hash = 0; uintptr_t pawn = 0; };
	std::unordered_map<uint64_t, AgentApplyState> m_agentState;
	// Cumulative successful-apply counters (never reset per frame); see TotalKnifeApplied() etc.
	uint64_t m_totalKnife = 0;
	uint64_t m_totalWeaponMesh = 0;
	uint64_t m_totalGloves = 0;
	uint64_t m_totalAgent = 0;
	// Tick-nudge state (see RequestApplyNudge / MaybeFireTickNudge). m_frameCounter advances every
	// main-thread frame independent of the apply gate so the debounce works even when clearing.
	bool m_tickNudge = true;          // briefly resume playback after a change so body swaps re-render
	int m_tickNudgeTicks = 10;        // how many ticks to play out before re-pausing ("play ~10 ticks")
	bool m_pendingNudge = false;      // a profile changed; a nudge is due once the debounce elapses
	uint64_t m_frameCounter = 0;      // main-thread frames since start (debounce/timing clock)
	uint64_t m_nudgeRequestFrame = 0; // m_frameCounter at the last profile change (debounce anchor)
	uint64_t m_totalNudges = 0;       // cumulative nudges completed (status/proof)
	bool m_armed = false;             // start-clean: overrides apply only after reapplied this demo
	std::wstring m_lastDemoPath;      // detect demo load/close (path change) -> clear the runtime store
	int m_lastApplyTick = -1;         // demo tick at the previous applied frame (seek detection)
	int m_seekSettleFrames = 0;       // frames left to SKIP the apply after a seek so the entity list
	                                  // finishes rebuilding before we touch it again (anti-crash)
	// Active nudge ("play out") state machine: 0 = idle, 1 = resumed and waiting to re-pause.
	int m_nudgePhase = 0;
	int m_nudgeStartTick = 0;         // demo tick when the resume began (to measure ticks played)
	uint64_t m_nudgePlayStartFrame = 0; // m_frameCounter when the resume began (safety timeout)
	bool m_nudgeWasPaused = false;    // whether to re-pause when the play-out finishes
};

// Process-wide singleton (matches the ...Ref() pattern used by MovieMode/CameraPath/etc.).
CosmeticOverrideSystem& CosmeticsRef();

// Thin free function for the main-thread frame pump (keeps Filmmaker.cpp's include surface small).
void Cosmetics_RunFrame();

// Console command entry: handles "mirv_filmmaker cosmetics ...". argc/args/cmd are forwarded from
// FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(2) == "cosmetics").
void Cosmetics_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Diagnostics (implemented in CosmeticDebug.cpp). Print to the console via advancedfx::Message.
void Cosmetics_PrintStatus(const char* cmd);
void Cosmetics_PrintSpectatedDebug(const char* cmd);
// Read-only visual-cache dump for the spectated weapon: the full econ identity, the fallback fields,
// the networked dynamic attribute values (def 6/7/8/81), the C_CSWeaponBase visuals-cache flags
// (m_bVisualsDataSet / m_bClearWeaponIdentifyingUGC / m_nCustomEconReloadEventId) AND their resolved
// schema offsets (for an IDA/Ghidra xref pass), the active weapon handle, and the weapon's world
// model path. Never writes. "cosmetics visualdiag".
void Cosmetics_PrintVisualDiag(const char* cmd);

} // namespace Filmmaker
