#pragma once

// Client-side MODEL-SWAP primitives for the offline demo cosmetics system: knife-type swap, agent
// (player) model swap, glove apply, and the legacy-vs-CS2 weapon mesh-group selection. These wrap
// the client.dll functions both Andromeda-CS2-Base and nerv use to change a rendered model purely
// client-side (overturning the old "SetModel is server-only" conclusion -- see
// docs/cosmetics-cs2-methodology-notes.md). Every entry point is SEH-guarded: a wrong signature /
// offset is caught and reported, never crashes the game.
//
// All functions read g_clientDllOffsets internally and resolve their client.dll targets lazily by
// byte-pattern on first use (ResolveModelSwapFns). They take raw entity pointers (unsigned char*)
// like the rest of CosmeticOverrideSystem.cpp so the apply loop stays uniform.

#include <cstdint>
#include <cstddef>

namespace Filmmaker {

// Resolution status of each model-swap client.dll function (for diagnostics / "cosmetics status").
struct ModelSwapResolveStatus {
	bool setModel = false;
	bool setMeshGroupMask = false;
	bool updateSubclass = false;
	bool setBodyGroup = false;
	bool updateBodyGroupChoice = false;
	bool getStaticData = false;
	bool econItemSystem = false;
	bool attempted = false;
	// Core knife/agent swap needs setModel; gloves need the body-group-choice refresh. getStaticData/econItemSystem are
	// best-effort enhancers (model-path lookup / legacy detection) with hardcoded fallbacks.
	bool CoreOk() const { return setModel && setMeshGroupMask && updateSubclass; }
	bool GlovesOk() const { return setModel && updateBodyGroupChoice; }
};

// Lazily resolves all model-swap functions from client.dll (once). Safe to call repeatedly; returns
// the cached status. Resolution failures are non-fatal (the corresponding apply is skipped).
const ModelSwapResolveStatus& ResolveModelSwapFns();

// True only for normalized player-model resource paths accepted by ApplyAgentModel.
bool IsValidAgentModelPath(const char* modelPath);

// Legacy-vs-CS2 model classification for a paint kit, via the econ item schema (the same lookup
// Andromeda/nerv use: GetEconItemSystem -> schema -> GetPaintKits -> find by id -> IsUseLegacyModel).
// Returns: 1 = paint kit uses the LEGACY model, 0 = modern CS2 model, -1 = unknown (schema
// unavailable or paint kit not found -> caller should keep the existing mesh). SEH-guarded.
int PaintKitLegacyModel(int paintKitId);

// Resolve the mesh-group mask to render for a paint kit. `knife` selects the knife convention (the two
// reference cheats DISAGREE on the knife mask polarity -- see docs). legacyOverride: -2 = auto from
// PaintKitLegacyModel, -1 = force modern, 1 = force legacy. maskModern/maskLegacy are the tunable
// bit values (defaults 1 and 2). Returns 0 when it should be left untouched (auto + unknown legacy).
uint64_t ResolveMeshMask(int paintKitId, bool knife, int legacyOverride,
	uint64_t maskModern, uint64_t maskLegacy);

// Knife TYPE + model swap on a WORLD weapon entity. The caller must already have set
// itemView->m_iItemDefinitionIndex to targetDef (so GetStaticData resolves the target knife). Resolves
// the target model (.vmdl) via GetStaticData, falling back to a built-in knife def->model table; calls
// SetModel(weapon), SetMeshGroupMask(weapon scene node, meshMask) when meshMask>0, writes
// m_nSubclassID = token(targetDef) + UpdateSubclass(weapon). pawnForViewmodel (may be null) is used to
// also SetModel the first-person viewmodel knife entity when present. Returns true if SetModel fired.
bool ApplyKnifeModelSwap(unsigned char* weapon, unsigned char* itemView,
	unsigned char* pawnForViewmodel, int targetDef, uint64_t meshMask);

// Set a weapon/world entity's mesh-group mask directly (legacy-vs-CS2 correction for a weapon skin
// override that did NOT change the weapon type). Reads m_pGameSceneNode internally. No-op if mask==0.
void ApplyWeaponMeshMask(unsigned char* weaponEntity, uint64_t meshMask, unsigned char* pawnForViewmodel);

// Refresh every weapon renderable parented below the pawn's HUD-arms entity. This synchronizes the
// first-person viewmodel after the world weapon's material/model was rebuilt.
void RefreshWeaponViewmodel(unsigned char* pawn);

// Agent (player) model swap on a pawn entity: SetModel(pawn, modelPath). Returns true if SetModel
// fired. Unsafe/non-player resource paths are rejected before SetModel.
bool ApplyAgentModel(unsigned char* pawn, const char* modelPath);

// Force the entity's renderable to re-evaluate its model / mesh-group / body-group RIGHT NOW
// (CGameSceneNode::PostDataUpdate, vtable index 22 -- the call Andromeda fires after every
// SetModel/SetMeshGroupMask/UpdateSkin so the change shows without waiting for the next networked
// update). This is the missing lever that makes a model/mesh swap visible while a demo is PAUSED: the
// field writes stick, but the engine only re-derives the rendered mesh from the scene node when the
// renderable is refreshed -- which is exactly what PostDataUpdate does. SEH-guarded (a wrong/stale
// scene node faults here and is swallowed); reads m_pGameSceneNode off the entity internally. Returns
// true if the call fired.
bool PostDataUpdate(unsigned char* entity);

// Glove apply on a pawn (Andromeda/nerv recipe): write the embedded m_EconGloves C_EconItemView
// identity (def index, quality, account id, m_bInitialized=true), overwrite its networked paint
// attributes, then UpdateBodyGroupChoice() + m_bNeedToReApplyGloves=true. It also swaps and refreshes
// the pawn's HUD-arms model from the glove econ definition. The caller drives the multi-frame
// re-assert; this performs one frame's write. Returns true if applied.
bool ApplyGloveModel(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed,
	uint32_t accountId);

} // namespace Filmmaker
