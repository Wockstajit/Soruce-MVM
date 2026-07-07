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

class CEntityInstance;

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
// entityIndex is for diagnostics only (the world weapon's entity-list index) -- when mvm_debug is
// running this path emits always-flushed "knife.swap"/"knife.vm" breadcrumbs around every native call
// so the last line written before a crash pinpoints which step (SetModel / mesh / subclass / viewmodel
// mirror / PostDataUpdate) faulted on a freshly-recreated entity during a quick weapon switch.
bool ApplyKnifeModelSwap(unsigned char* weapon, unsigned char* itemView,
	unsigned char* pawnForViewmodel, int targetDef, uint64_t meshMask, int entityIndex = -1);

// Resolve the WORLD-model .vmdl path a knife-type swap to targetDef would apply -- the SAME resolution
// ApplyKnifeModelSwap uses internally (GetStaticData on the def-already-set itemView, falling back to the
// built-in knife def->model table). Lets the apply loop compare against an entity's LIVE model path and
// re-fire the swap whenever the engine reverted the model (e.g. a demo seek that reused the entity slot,
// so the index-based throttle never tripped). Returns true and fills out[] on success. SEH-guarded.
bool ResolveKnifeModelPath(int targetDef, unsigned char* itemView, char* out, size_t outSize);

// EXPERIMENTAL animgraph reset (candidate fix for the knife-type-swap animation crash, "approach #1").
// After a knife SetModel, null the entity's animation-graph-instance networked-variables pointer -- the
// `real-time-internal-overlay-research-main` technique (viewModel->pAnimationGraphInstance->
// pAnimGraphNetworkedVariables = nullptr) -- so the graph rebuilds for the new model instead of posing it
// with stale per-model data. The reference value (instance @ entity+0xD08, vars @ +0x2E0) is for the LOCAL
// VIEWMODEL; the demo WORLD weapon layout differs, so the offsets are TUNABLE from the console
// ("cosmetics animreset ..."). The write is SEH-guarded + pointer-sanity-checked and logged as a
// "knife.swap step=animreset" breadcrumb (so a bad offset shows inst=garbage instead of corrupting state).
void SetAnimGraphReset(bool enabled);
bool AnimGraphReset();
void SetAnimGraphResetOffsets(uint32_t instOff, uint32_t varsOff);
void GetAnimGraphResetOffsets(uint32_t* instOff, uint32_t* varsOff);

// PRECACHE the target model before SetModel ("approach #2", the proven root-cause fix). The knife-type
// crash is a null per-model animation table: a raw SetModel points the replay weapon at a .vmdl whose
// resource/anim data was never loaded in this demo (the player never carried that knife), so the engine's
// async anim pass dereferences a null table (client.dll+0x3561c0 returns null -> +0x3399cc reads [null+8];
// see docs/cosmetics-cs2-methodology-notes.md §11). PrecacheModelResource does a BLOCKING resource load
// (resourcesystem.dll CResourceSystem::PreCache, the same call the skybox path uses) on the swap thread so
// the model + its anim data are present before SetModel/the worker-thread pose. SEH-guarded; no-op if the
// resource system is unavailable. Default ON; toggle via "cosmetics precache 0|1". Returns true if the
// blocking load fired. A "knife.swap step=precache" breadcrumb records whether it ran for each swap.
bool PrecacheModelResource(const char* modelPath);
void SetPrecacheModels(bool enabled);
bool PrecacheModels();

// Set a weapon/world entity's mesh-group mask directly (legacy-vs-CS2 correction for a weapon skin
// override that did NOT change the weapon type). Reads m_pGameSceneNode internally. No-op if mask==0.
// Resets the entity's animgraph after the mesh-group write (same fix ApplyKnifeModelSwap uses for the
// worker-thread null-deref crash -- a mesh-group toggle leaves the same kind of stale per-mesh anim
// state behind as a full model swap does). entityIndex is diagnostics-only (breadcrumb tag); -1 if unknown.
void ApplyWeaponMeshMask(unsigned char* weaponEntity, uint64_t meshMask, unsigned char* pawnForViewmodel, int entityIndex = -1);

// Refresh every weapon renderable parented below the pawn's HUD-arms entity. This synchronizes the
// first-person viewmodel after the world weapon's material/model was rebuilt.
void RefreshWeaponViewmodel(unsigned char* pawn);

// SEH-guarded read of an entity's live mesh-group mask (CModelState::m_MeshGroupMask). Returns 0 on failure.
uint64_t ReadEntityMeshGroupMask(CEntityInstance* ent);

// Result of mirroring world-weapon cosmetics onto the matching first-person viewmodel child.
struct ViewmodelMirrorResult {
	bool armsFound = false;
	bool childMatched = false;
	bool meshWritten = false;
	bool compositeCalled = false;
	bool compositeFaulted = false;
	bool namedSetter = false;
	int vmEntityIndex = -1;
	uint64_t meshBefore = 0;
	uint64_t meshAfter = 0;
	int paintBefore = -1;
	int paintAfter = -1;
};

// Mirror mesh + skin composite from a world weapon onto its first-person viewmodel (HUD-arms child with
// the same weapon class). This is the fix for "third person correct, first person legacy/default" when
// HudModelArms exists but the viewmodel never received FireDirectCompositeRefresh.
ViewmodelMirrorResult MirrorWeaponCosmeticsToViewmodel(
	unsigned char* pawn,
	unsigned char* worldWeapon,
	int worldEntityIndex,
	unsigned char* worldItemView,
	uint64_t meshMask,
	int32_t paintKit,
	float wear,
	int32_t seed,
	int statTrak,
	ptrdiff_t compositeOwnerOffset,
	ptrdiff_t offRestoreMaterial);

// Read-only snapshot of the active weapon's first-person viewmodel paint/mesh (for skin.live logging).
bool ReadActiveViewmodelWeaponState(unsigned char* pawn, const char* wantWeaponClass,
	int* outVmIndex, int* outPaint, uint64_t* outMeshMask);

// FxAlign muzzle probe: resolve the first-person VIEWMODEL weapon entity index (the weapon-like child
// of the pawn's HUD-arms) WITHOUT the class/paint match ReadActiveViewmodelWeaponState requires --
// that returns -1 on demo POV pawns whose arms child does not class-match the world weapon. Any
// weapon-like arms child IS the first-person gun, and its muzzle/muzzle_flash attachment is the
// viewmodel muzzle a m_bViewModelEffect particle anchors to. Prefers a class match, else the first
// weapon-like child. Fills optional dbg with the arms source + every weapon-like child (idx:class).
// Returns the viewmodel weapon entity index, or -1. The first-person viewmodel weapon is a
// CLIENT-ONLY entity (index >= 0x4000), so it does NOT round-trip through the entity-list index
// lookup -- outEntity receives the resolved CEntityInstance* directly (use that, not EntityAt(idx),
// to read its muzzle attachment). SEH-guarded.
int ResolveViewmodelWeaponEntityIndex(unsigned char* pawn, const char* wantWeaponClass,
	char* dbg = nullptr, size_t dbgSize = 0, void** outEntity = nullptr);

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
	uint32_t accountId, bool composePaintKit = true);

} // namespace Filmmaker
