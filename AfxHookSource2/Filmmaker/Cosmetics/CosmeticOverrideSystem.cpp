#include "CosmeticOverrideSystem.h"

#include "CosmeticCatalog.h"
#include "CosmeticModelSwap.h"
#include "CosmeticDirectComposite.h"
#include "CosmeticDebugLog.h"

#include "../Demo/PlayingDemoPath.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle, AfxGetLocalObserverState
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../../../shared/binutils.h"
#include "../../MirvTime.h"               // g_MirvTime.GetCurrentDemoTick for the tick nudge
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (demo seek)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Engine pointer (same one CameraPath/MovieMode use) for the demo re-seek that forces a renderable
// rebuild after a cosmetic change (the user-requested "nudge a few ticks" lever).
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Min frames between clobber-driven re-composites during demo playback (demo packets reset attrs often).
constexpr uint64_t kClobberCompositeCooldownFrames = 64;

// Resolves an entity by index the same way MirvCosmetics.cpp / CameraEditorHud.cpp do: bounds +
// null-list checks before touching the (possibly stale) global pointers.
CEntityInstance* EntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

// Same heuristic as CameraEditorHud.cpp's LooksLikeWeaponEntity: matches both server-style
// "weapon_*" class names and the client "CWeaponX" / "C_WeaponX" schema class names, so it is
// resilient to either naming scheme being present at runtime.
bool LooksLikeWeaponEntity(CEntityInstance* entity) {
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClass = entity ? entity->GetClientClassName() : nullptr;
	return (className && std::strstr(className, "weapon_"))
		|| (className && std::strstr(className, "Weapon"))
		|| (clientClass && std::strstr(clientClass, "Weapon"));
}

unsigned char* PawnForSteamId(uint64_t steamId) {
	if (steamId == 0)
		return nullptr;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* controller = EntFromIndex(i);
		if (!controller || !controller->IsPlayerController() || controller->GetSteamId() != steamId)
			continue;
		SOURCESDK::CS2::CBaseHandle handle = controller->GetPlayerPawnHandle();
		CEntityInstance* pawn = handle.IsValid() ? EntFromIndex(handle.GetEntryIndex()) : nullptr;
		return pawn && pawn->IsPlayerPawn() ? (unsigned char*)pawn : nullptr;
	}
	return nullptr;
}

// SEH-guarded scalar reads off an entity (used by the pawn glove/agent pass; kept as free functions
// so the caller can use C++ objects without tripping MSVC C2712 "__try + object unwinding").
float SafeReadEntityFloat(unsigned char* base, ptrdiff_t off, float fallback) {
	if (!base || off == 0) return fallback;
	__try { return *(float*)(base + off); } __except (1) { return fallback; }
}

int SafeReadGloveDefIndex(unsigned char* pawn) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!pawn || o.C_CSPlayerPawn.m_EconGloves == 0 || o.C_EconItemView.m_iItemDefinitionIndex == 0)
		return 0;
	__try {
		return (int)*(uint16_t*)(pawn + o.C_CSPlayerPawn.m_EconGloves + o.C_EconItemView.m_iItemDefinitionIndex);
	} __except (1) {
		return 0;
	}
}

// SEH-guarded read of an entity's LIVE rendered model path (CModelState::m_ModelName) via the same
// body-component/skeleton chain CosmeticDebug.cpp uses. Lets the diagnostic log show whether a knife/
// weapon entity is actually rendering the swapped model or still the original (shadow_daggers vs the
// target nomad/bayonet). POD-only body so __try is permitted. out[0]='\0' on any failure.
void ReadEntityModelPath(CEntityInstance* ent, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!ent || !out || outSize == 0)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000) return;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
		if ((uintptr_t)name <= 0x10000) return;
		size_t i = 0;
		for (; name[i] && i + 1 < outSize; ++i) out[i] = name[i];
		out[i] = '\0';
	} __except (1) {
		if (outSize) out[0] = '\0';
	}
}

// SEH-guarded read of an entity's LIVE rendered mesh-group mask -- see ReadEntityMeshGroupMask in
// CosmeticModelSwap.cpp (shared with viewmodel mirror diagnostics).

// POD result of the SEH-guarded write -- no constructors, safe to return out of __try/__except.
struct ApplyResult {
	bool patched = false;
	bool reverted = false;
	bool needComposite = false; // a (re)composite is warranted this frame (drives the optional vcall)
};

// SEH-isolated vtable call for the OPTIONAL forced re-composite (see CosmeticOverrideSystem.h /
// SetRecompose). POD-only body (no C++ objects) so __try/__except is permitted; a wrong index
// access-violates HERE and is reported via the return value instead of crashing. Signature matches
// CBasePlayerWeapon::UpdateComposite(bool) -- on x64 __thiscall == __fastcall. (1 ==
// EXCEPTION_EXECUTE_HANDLER; the literal avoids pulling in <windows.h> and its macro clashes.)
typedef void* (__fastcall* UpdateComposite_t)(void* thisptr, int arg);
bool SafeVCall(void* obj, int idx, int arg) {
	__try {
		void** vt = *(void***)obj;
		void* fn = vt[idx];
		if (!fn) return false;
		((UpdateComposite_t)fn)(obj, arg);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded read of a weapon entity's econ identity (owner XUID + embedded C_EconItemView pointer
// + live item-definition index). LooksLikeWeaponEntity is only a class-name heuristic, so a
// "weapon_" entity that is not actually a C_EconEntity would access-violate if read directly --
// guarding here means such an entity is skipped, never crashing the game. POD-only body: `out` is
// constructed by the caller; inside __try we only assign primitives.
struct WeaponEconRead {
	uint64_t xuid = 0;
	unsigned char* itemView = nullptr;
	int liveDef = 0;
};
bool TryReadWeaponEconInfo(unsigned char* w, ptrdiff_t offXuidLow, ptrdiff_t offXuidHigh,
	ptrdiff_t offAttrMgr, ptrdiff_t offItem, ptrdiff_t offDef, WeaponEconRead* out) {
	__try {
		uint32_t xLow = *(uint32_t*)(w + offXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + offXuidHigh);
		out->xuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		out->itemView = w + offAttrMgr + offItem;
		out->liveDef = (int)*(uint16_t*)(out->itemView + offDef);
		return true;
	} __except (1) {
		return false;
	}
}

// POD result of the networked-attribute overwrite. `changed` is true when at least one value was
// actually different from our target (drives the re-composite), `written` counts matched attributes.
// `hasPaint` is true when a paint-kit attribute (def 6) was actually present to overwrite -- when it is
// FALSE the item has no networked paint attribute (default knives/gloves, freshly-picked-up or viewmodel
// entities), so the override must be applied via the engine named-setter fallback instead.
struct AttrWriteResult {
	bool ok = false;
	int written = 0;
	bool changed = false;
	bool hasPaint = false;
};

// THE real skin write (Phase 2 breakthrough). A networked demo weapon stores its actual skin in
// m_NetworkedDynamicAttributes (def 6 = paint kit, 7 = seed, 8 = wear, 81 = StatTrak) -- the
// m_nFallback* fields are only consulted when the item has NO networked attributes, so on a demo
// item they are ignored. This OVERWRITES the matching attribute VALUES in place (it cannot add new
// ones -- a missing def, e.g. StatTrak on a non-ST gun, is skipped). Walks the same vector layout as
// CosmeticDebug.cpp::ReadAttrList. POD-only body so __try/__except is permitted. vectorField =
// itemView + networkedOff + attrSubOff (the CAttributeList::m_Attributes vector).
void WriteNetworkedSkinAttributes(unsigned char* itemView, ptrdiff_t networkedOff, ptrdiff_t attrSubOff,
	int stride, ptrdiff_t defOff, ptrdiff_t valOff, int paintKit, int seed, float wear, int statTrak,
	AttrWriteResult* out) {
	*out = AttrWriteResult{};
	if (networkedOff == 0 || attrSubOff == 0 || defOff == 0 || valOff == 0 || stride <= 0)
		return;
	unsigned char* vectorField = itemView + networkedOff + attrSubOff;
	__try {
		// Accept both observed vector layouts (count+ptr at +0/+8, or ptr+count at +0/+16), the same
		// tolerance as ReadAttrList.
		int count = *(int*)vectorField;
		unsigned char* data = *(unsigned char**)(vectorField + 8);
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			data = *(unsigned char**)vectorField;
			count = *(int*)(vectorField + 16);
		}
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			out->ok = true; // valid but empty -- nothing to overwrite
			return;
		}
		for (int i = 0; i < count; ++i) {
			unsigned char* attr = data + (ptrdiff_t)i * stride;
			int def = (int)*(uint16_t*)(attr + defOff);
			float nv;
			if (def == 6) { nv = (float)paintKit; out->hasPaint = true; }
			else if (def == 7) nv = (float)seed;
			else if (def == 8) nv = wear;
			else if (def == 81 && statTrak >= 0) nv = (float)statTrak;
			else continue;
			float* pv = (float*)(attr + valOff);
			if (*pv != nv) { *pv = nv; out->changed = true; }
			++out->written;
		}
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

// The proven write recipe from MirvCosmetics::Cosmetics_RunFrame, generalized to an arbitrary
// econ entity + arbitrary fallback offsets. Every pointer/offset is resolved by the caller BEFORE
// entering this function so the __try body only ever touches POD pointers/primitives -- required
// because __try/__except may not contain C++ objects that need unwinding.
//
// w        = entity base pointer (unsigned char*)
// itemView = w + m_AttributeManager + m_Item (the embedded C_EconItemView)
// offItemIdHigh/offPaint/offWear/offSeed/offStat = C_EconEntity fallback field offsets (from w)
// offDef                                          = C_EconItemView::m_iItemDefinitionIndex (from itemView)
// offItemIdLow/offAccountId                       = optional (0 = skip), from itemView
// offInit/offInitTags                             = optional (0 = skip), from itemView
// offRestoreMaterial/offImageReq/offImageTried/offCachedFile = optional item-view cache hints
// offAttrParity                                  = optional CAttributeManager cache hint
// offVisualsData/offClearUgc/offReloadEvent      = optional C_CSWeaponBase visuals-cache hints
// knifeDefOverride > 0 requests a def/model swap (best-effort; see header comment on agent/gloves
// for what is and is not safe to do here -- this is just numeric def swap, not a model load).
// markStaleEnabled = do the visuals-stale field writes when a value changed (per-frame auto-rebuild).
// forceStale       = do them unconditionally (the manual "rebuild once" path), even when unchanged.
ApplyResult ApplyCosmeticWrite(
	unsigned char* w,
	unsigned char* itemView,
	unsigned char* attrManager,
	ptrdiff_t offItemIdHigh,
	ptrdiff_t offPaint,
	ptrdiff_t offWear,
	ptrdiff_t offSeed,
	ptrdiff_t offStat,
	ptrdiff_t offDef,
	ptrdiff_t offItemIdLow,
	ptrdiff_t offAccountId,
	ptrdiff_t offInit,
	ptrdiff_t offInitTags,
	ptrdiff_t offAttrInit,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t offImageReq,
	ptrdiff_t offImageTried,
	ptrdiff_t offCachedFile,
	ptrdiff_t offAttrParity,
	ptrdiff_t offVisualsData,
	ptrdiff_t offClearUgc,
	ptrdiff_t offReloadEvent,
	int32_t paintKit,
	float wear,
	int32_t seed,
	int32_t statTrak,
	uint32_t accountId,
	int knifeDefOverride,
	bool writeFallbackId,
	bool markStaleEnabled,
	bool forceStale,
	CosmeticRebuildFlags flags) {
	__try {
		int32_t* pItemIdHigh = (int32_t*)(itemView + offItemIdHigh);
		int32_t* pPaint = (int32_t*)(w + offPaint);
		float* pWear = (float*)(w + offWear);
		int32_t* pSeed = (int32_t*)(w + offSeed);
		int32_t* pStat = (int32_t*)(w + offStat);
		uint16_t* pDef = (uint16_t*)(itemView + offDef);

		bool restoreItemId = !writeFallbackId && (*pItemIdHigh == -1);
		bool reverted = writeFallbackId && (*pItemIdHigh != -1);
		bool need = reverted
			|| restoreItemId
			|| (*pPaint != paintKit)
			|| (*pWear != wear)
			|| (*pSeed != seed)
			|| (*pStat != statTrak)
			|| (knifeDefOverride > 0 && *pDef != (uint16_t)knifeDefOverride);

		// itemIDHigh=-1 forces the legacy fallback-field path. Opt-in only: it INVALIDATES the item
		// id (breaks the UI inventory read -- "no AK") and is unnecessary now that we overwrite the
		// networked attributes directly. Default OFF (see m_fallbackId / "cosmetics fallback").
		if (writeFallbackId) {
			*pItemIdHigh = -1;
			if (offAccountId) *(uint32_t*)(itemView + offAccountId) = accountId; // best-effort
			if (offItemIdLow) *(uint32_t*)(itemView + offItemIdLow) = 0;         // best-effort
		} else if (restoreItemId) {
			// Older builds / live diagnostics could leave demo weapons in fallback mode, which makes
			// CS2's loadout HUD lose the real weapon icon. In normal mode keep the demo item id valid.
			*pItemIdHigh = 0;
		}
		*pPaint = paintKit;
		*pWear = wear;
		*pSeed = seed;
		*pStat = statTrak;
		if (knifeDefOverride > 0) *pDef = (uint16_t)knifeDefOverride; // best-effort knife DEF swap

		// The visuals-stale field writes. Gated on a value changing AND per-frame auto-rebuild being on
		// (markStaleEnabled), OR forceStale (the "rebuild once" path) which re-asserts unconditionally.
		// EACH individual write is ALSO gated on its rebuild flag (all default OFF) so the harmful ones
		// (clearUgc/initialized blank the HUD weapon icon) are not written unless explicitly enabled for
		// research -- see CosmeticRebuildFlags / "cosmetics rebuildflags". Default = none -> HUD-safe.
		bool doStaleWrites = (need && markStaleEnabled) || forceStale;
		if (doStaleWrites) {
			if (flags.initialized) {
				if (offInit) *(bool*)(itemView + offInit) = false;
				if (offInitTags) *(bool*)(itemView + offInitTags) = false;
			}
			if (flags.imageCache) {
				if (offRestoreMaterial) *(bool*)(itemView + offRestoreMaterial) = true;
				if (offImageReq) *(bool*)(itemView + offImageReq) = false;
				if (offImageTried) *(bool*)(itemView + offImageTried) = false;
				if (offCachedFile) *(char*)(itemView + offCachedFile) = 0;
			}
			if (flags.attrParity && attrManager && offAttrParity) {
				int32_t* parity = (int32_t*)(attrManager + offAttrParity);
				*parity = *parity + 1;
			}
			if (flags.visualsDataSet && offVisualsData) *(bool*)(w + offVisualsData) = false;
			if (flags.clearUgc && offClearUgc) *(bool*)(w + offClearUgc) = true;
			if (flags.reloadEvent && offReloadEvent) {
				// Set m_nCustomEconReloadEventId NEGATIVE (not ++). Binary analysis of the live client.dll
				// (docs/archive/cosmetics-recompose-research.md, 2026-06-28) shows C_CSWeaponBase vtable slot 11
				// (the econ OnDataChanged path) only requests a "clientside_reload_custom_econ" composite
				// rebuild when this field is < 0 ("cmp dword [weapon+0x18bc],0 / jge skip"); a positive value
				// (the old ++ bump) is the skip case, which is why it never rebuilt. -1 == "reload me".
				int32_t* reloadEvent = (int32_t*)(w + offReloadEvent);
				*reloadEvent = -1;
			}
			// Clear C_EconEntity::m_bAttributesInitialized on the WEAPON (offset from w, NOT itemView)
			// to try to force an econ attribute / composite re-init. See docs/archive/cosmetics-recompose-research.md.
			if (flags.attrInit && offAttrInit) *(bool*)(w + offAttrInit) = false;
		}

		ApplyResult r;
		r.patched = true;
		r.reverted = (reverted && need);
		r.needComposite = need;
		return r;
	} __except (1) {
		// A misclassified entity / bad offset access-violates HERE and is swallowed instead of
		// crashing the game (1 == EXCEPTION_EXECUTE_HANDLER; the literal avoids <windows.h> macro
		// clashes the same way MirvCosmetics::SafeVCall does).
		return ApplyResult{};
	}
}

} // namespace

CosmeticOverrideSystem& CosmeticsRef() {
	static CosmeticOverrideSystem s;
	return s;
}

void Cosmetics_RunFrame() {
	// Per-player LIVE skin-state snapshot into the mvm_debug log. Runs BEFORE the apply (and is not
	// gated on cosmetics being enabled) so the log shows what the game is actually rendering for the
	// spectated player -- to compare against any override -- on every player switch / seek / weapon /
	// loadout change. No-op unless mvm_debug is active. (CosmeticDebug.cpp)
	Cosmetics_TickSkinStateLog();
	CosmeticsRef().RunFrame();
}

void CosmeticOverrideSystem::Init() {
	if (m_initialized)
		return;
	// Cosmetics are demo-session state, not application preferences. Discard profiles written by
	// older builds and keep a valid, empty compatibility file so a crash/relaunch cannot resurrect
	// overrides onto a different demo or recreated player entity.
	m_store.ClearAll();
	m_store.SetEnabled(true);
	m_armed = true;
	m_store.Save();
	m_initialized = true;
}

void CosmeticOverrideSystem::Shutdown() {
	ResetSessionOverrides("shutdown");
}

void CosmeticOverrideSystem::ResetSessionOverrides(const char* reason) {
	RestorePaintkitBridgeCvar();
	m_store.ClearAll();
	m_store.SetEnabled(true);
	m_armed = true;
	m_agentState.clear();
	m_gloveState.clear();
	m_lastGloveSpectatedSid = 0;
	m_knifeSwapState.clear();
	m_pendingNudge = false;
	m_nudgePhase = 0;
	m_nudgeWasPaused = false;
	m_framesSinceSeek = 0;     // re-arm the knife-swap stability window on demo load/close
	m_lastDemoTickObserved = -1;
	m_seekSettleFrames = 0;
	m_vmMirrorSettleFrames = 0;
	m_lastCompositeTick = -1;
	m_compositeBurstRemaining = 0;
	m_lastCompositeFrameByIdx.clear();
	m_store.Save(); // best-effort normalization of the legacy compatibility file
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.session", "cleared runtime overrides: reason=%s enabled=1 profiles=0 armed=1",
			reason ? reason : "unknown");
}

bool CosmeticOverrideSystem::OffsetsAvailable() const {
	return g_cosmeticsOffsetsOk;
}

bool CosmeticOverrideSystem::InDemoContext() const {
	return !ResolvePlayingDemoPath().empty();
}

void CosmeticOverrideSystem::SetEnabled(bool e) {
	(void)e;
	// Filmmaker demo workflow: cosmetics stay enabled for the whole session.
	m_store.SetEnabled(true);
	m_armed = true;
}

void CosmeticOverrideSystem::Save() {
	m_store.Save();
}

void CosmeticOverrideSystem::SetRecompose(bool e) {
	m_recompose = e;
	if (e) m_recomposeFaulted = false;
}

void CosmeticOverrideSystem::GetVtIdx(int* comp, int* sec) const {
	if (comp) *comp = m_vtComposite;
	if (sec) *sec = m_vtCompositeSec;
}

void CosmeticOverrideSystem::SetVtIdx(int comp, int sec) {
	m_vtComposite = comp;
	m_vtCompositeSec = sec;
	m_recompose = true;        // re-arm so the new indices get tried next frame
	m_recomposeFaulted = false;
}

void CosmeticOverrideSystem::SetWeapon(uint64_t steamId, int defIndex, int paintKit, float wear, int seed, int statTrak) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);

	// Knives have their own single slot + model/type swap; route a knife def there. Every other weapon
	// def gets its OWN entry in the per-weapon map, so each weapon type the player carries keeps an
	// independent skin (the "any weapon, per player" model). A defIndex of 0 is rejected -- a per-weapon
	// override requires an explicit weapon (the old "0 = any in slot" wildcard is gone).
	if (CosmeticCatalog::IsKnifeDef(defIndex)) {
		SetKnife(steamId, defIndex, paintKit, wear, seed);
		return;
	}
	if (defIndex <= 0)
		return;

	CosmeticItem& item = profile.weapons[defIndex];
	item.set = true;
	item.defIndex = defIndex;
	item.paintKit = paintKit;
	item.wear = wear;
	item.seed = seed;
	item.statTrak = statTrak;
	// statTrak is left as-supplied for weapons (unlike SetKnife/SetGloves which force -1).

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::ClearWeapon(uint64_t steamId, int defIndex) {
	CosmeticProfile* profile = m_store.Find(steamId);
	if (!profile)
		return;
	if (CosmeticCatalog::IsKnifeDef(defIndex)) {
		profile->knife = CosmeticItem{};
	} else {
		profile->weapons.erase(defIndex);
	}
	if (profile->Empty())
		m_store.ClearPlayer(steamId);
}

void CosmeticOverrideSystem::SetKnife(uint64_t steamId, int defIndex, int paintKit, float wear, int seed) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	profile.knife.set = true;
	profile.knife.defIndex = defIndex;
	profile.knife.paintKit = paintKit;
	profile.knife.wear = wear;
	profile.knife.seed = seed;
	profile.knife.statTrak = -1;

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::SetGloves(uint64_t steamId, int defIndex, int paintKit, float wear, int seed) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	profile.gloves.set = true;
	profile.gloves.defIndex = defIndex;
	profile.gloves.paintKit = paintKit;
	profile.gloves.wear = wear;
	profile.gloves.seed = seed;
	profile.gloves.statTrak = -1;

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;

	// Arm an immediate multi-frame burst even when observer-services returns target -1 (POV demos).
	GloveApplyState& st = m_gloveState[steamId];
	st.frames = 4;
	st.pawnStable = 4;
	m_gloveBypassSid = steamId;
	m_gloveBypassFrames = 90;
}

void CosmeticOverrideSystem::SetAgent(uint64_t steamId, const char* model, int defIndex) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	if (!model || !*model || 0 == _stricmp(model, "default")) {
		profile.agent.set = false;
		profile.agent.model.clear();
	} else {
		profile.agent.set = true;
		profile.agent.model = model;
		profile.agent.defIndex = defIndex;
	}

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::ClearPlayer(uint64_t steamId) {
	m_store.ClearPlayer(steamId);
}

void CosmeticOverrideSystem::ClearAll() {
	m_store.ClearAll();
}

int CosmeticOverrideSystem::CurrentSpectatedPawnIndex() const {
	// Robust spectated-pawn resolution: engine observer-services first, then a render-eye match
	// against the game camera. The observer-services path alone reads target -1 in POV/GOTV demos
	// (so 'current' could never resolve there); the eye-match fallback fixes that. See
	// AfxGetSpectatedPawnIndex in ClientEntitySystem.cpp.
	return AfxGetSpectatedPawnIndex();
}

uint8_t CosmeticOverrideSystem::CurrentObserverMode() const {
	return AfxGetLocalObserverState(nullptr);
}

uint64_t CosmeticOverrideSystem::CurrentSpectatedSteamId() const {
	int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		if (ent->GetPlayerPawnHandle().GetEntryIndex() == pawnIndex)
			return ent->GetSteamId();
	}
	return 0;
}

std::string CosmeticOverrideSystem::NameForSteamId(uint64_t steamId) const {
	if (steamId == 0)
		return std::string();

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		if (ent->GetSteamId() != steamId)
			continue;
		const char* sanitized = ent->GetSanitizedPlayerName();
		if (sanitized && *sanitized)
			return std::string(sanitized);
		const char* raw = ent->GetPlayerName();
		if (raw && *raw)
			return std::string(raw);
		return std::string();
	}
	return std::string();
}

void CosmeticOverrideSystem::RunFrame() {
	// Tick clock + nudge run BEFORE the apply gate so a debounced re-seek still fires after a clear
	// (which empties the store) and so the clock advances every frame regardless of apply state.
	++m_frameCounter;
	if (m_vmMirrorSettleFrames > 0) --m_vmMirrorSettleFrames;
	// Decay unconditionally (regardless of the enabled/armed/profiles/demo gate below): this used to
	// live only in the SEEK-SAFETY GATE further down, which the empty-profile early-return skips
	// entirely -- so with zero cosmetic profiles set (e.g. a fresh session, or the Customize preview's
	// one-shot previewnudge with no profile yet), a seek anywhere in the session left this stuck at its
	// post-seek value FOREVER, and MaybeFireTickNudge() (called below, BEFORE that gate) permanently
	// deferred every pending nudge as a result (confirmed live: mvm_debug showed seekSettle stuck at
	// 152 for 2000+ consecutive frames, never decrementing, previewnudge silently doing nothing).
	if (m_seekSettleFrames > 0) --m_seekSettleFrames;
	// Cosmetic state is scoped to one uninterrupted demo run: clear ONLY when the demo PATH changes (a
	// different demo loaded, or the demo closed). Overrides are keyed by owner XUID and the apply loop
	// re-discovers entities every frame, so they intentionally PERSIST across scrubs in BOTH directions
	// -- including a scrub all the way back to tick 0. The old "demo tick reset" path wiped the store on
	// any back-to-start scrub, which silently desynced the Customize UI (it re-read the now-default
	// loadout) from the still-applied/composited overrides in the game; that wipe was removed. A genuine
	// reload loads a fresh path and is still caught by the path change below; use "cosmetics clear" to
	// drop overrides on demand.
	{
		std::wstring curDemo = ResolvePlayingDemoPath();
		if (curDemo != m_lastDemoPath) {
			m_lastDemoPath = curDemo;
			ResetSessionOverrides(curDemo.empty() ? "demo closed" : "demo path changed");
		}
	}
	DetectDemoSeekEarly();
	MaybeFireTickNudge();
	// Belt-and-suspenders: enabled cosmetics must always be armed in demo context so the apply loop
	// is not blocked until the first UI mutation (log evidence: armed=0 blocked ~10s of spectating).
	if (m_store.Enabled() && InDemoContext() && !m_armed) {
		m_armed = true;
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.armed", "auto-armed in demo context frame=%llu", (unsigned long long)m_frameCounter);
	}
	if (MvmDebugLog_Active() && (m_frameCounter % 512) == 0) {
		MvmDebugLog_Linef("cosmetics.heartbeat",
			"armed=%d enabled=%d profiles=%zu inDemo=%d totalNudges=%llu",
			m_armed ? 1 : 0, m_store.Enabled() ? 1 : 0,
			m_store.All().size(), InDemoContext() ? 1 : 0, (unsigned long long)m_totalNudges);
	}
	// Cheap no-op gate: disabled, NOT armed this demo, offsets unresolved, no profiles, or no demo.
	if (!m_store.Enabled() || !m_armed || !g_cosmeticsOffsetsOk || m_store.Empty() || !InDemoContext()) {
		UpdatePaintkitBridge();
		return;
	}

	// SEEK-SAFETY GATE: skip apply while entities rebuild after a demo seek (see DetectDemoSeekEarly).
	// Decrement now happens unconditionally at the top of RunFrame (see comment there); this only
	// decides whether to skip the apply loop this frame.
	if (m_seekSettleFrames > 0) {
		UpdatePaintkitBridge();
		return;
	}

	int curTick = 0;
	bool haveTick = g_MirvTime.GetCurrentDemoTick(curTick);

	// Knife-swap stability window. The seek settle above is enough for the SEH-guarded POD
	// attribute writes + the composite refresh, but NOT for the knife model-type swap: its SetModel +
	// UpdateSubclass re-derive the weapon's VData/animation set, and if that lands on an entity the
	// engine is still reconstructing after a seek the animation state is left inconsistent and faults
	// a few SECONDS later (outside the SEH guard) -- the "scrub, knife redeploys, then it crashes"
	// report. So the knife swap waits for a longer run of uninterrupted frames since the last seek;
	// rapid stacked scrubs keep resetting m_framesSinceSeek, so it can never fire mid-reconstruction.
	// Frame-based (not tick-based) so it also advances while paused, where the demo tick is frozen.
	if (m_framesSinceSeek < 0x40000000) ++m_framesSinceSeek;
	// Short post-seek settle so the knife model swap fires onto a constructed entity (its scene node is
	// ready) rather than mid-rebuild where the engine would immediately revert our model. This was 64
	// frames purely to avoid the unloaded-model anim CRASH on a half-rebuilt entity; CosmeticAnimFix now
	// neutralizes that crash, so the window is tightened to a few frames -- which closes the visible
	// window where the knife reverted to its ORIGINAL model (skin already re-applied) right after a seek.
	const int kKnifeSwapStableFrames = 8;
	const bool knifeSwapStable = m_framesSinceSeek >= kKnifeSwapStableFrames;

	// Composite during playback: demo packets reset networked attrs every tick, so firing a full
	// direct-composite on every attr.changed (or every 16 demo ticks) visibly flickers between the
	// demo skin and the override. Instead: a short burst after apply, then clobber-driven re-composite
	// at most once per kClobberCompositeCooldownFrames per entity. cl_paintkit_override (bridge) is
	// ON by default so the engine builds with our paint when it does rebuild naturally.
	const bool burstComposite = m_compositeBurstRemaining > 0;
	if (burstComposite)
		--m_compositeBurstRemaining;

	++m_lastStats.frame;
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
	m_lastStats.namedSetterApplied = 0;
	m_lastStats.paintkitBridgeValue = 0;
	m_lastStats.directCompositeCalls = 0;
	m_lastStats.directCompositeResolved = 0;
	m_lastStats.directCompositeFaulted = 0;
	m_lastStats.modelSwapResolved = 0;
	m_lastStats.knifeModelsApplied = 0;
	m_lastStats.weaponMeshFixed = 0;
	m_lastStats.pawnsScanned = 0;
	m_lastStats.glovesApplied = 0;
	m_lastStats.agentsApplied = 0;

	// Per-frame: mark visuals stale only on change (gated by m_rebuildAuto inside ApplyCosmeticWrite),
	// fire the recompose vcall only if the user armed it via "cosmetics recompose 1", and fire the
	// resolved direct composite refresh whenever a matched weapon's skin data changed this frame (the
	// auto-update lever -- makes a UI skin pick render immediately and re-applies after seeks/redeploys).
	ApplyMatchedWeapons(/*forceStale=*/false, /*fireRebuildCall=*/m_recompose, /*fireDirectComposite=*/m_autoComposite,
		/*allowKnifeSwap=*/knifeSwapStable, /*periodicComposite=*/burstComposite);
	// Gloves + agent (pawn-level model swap), keyed by owner SteamID like the weapon loop.
	m_gloveApplyBudget = 1;
	m_constructPaintBudget = 1;
	ApplyPawnCosmetics();
	// Cumulative (never-reset) apply counters -- so a one-shot/gated apply stays observable in status.
	m_totalKnife += m_lastStats.knifeModelsApplied;
	m_totalWeaponMesh += m_lastStats.weaponMeshFixed;
	m_totalGloves += m_lastStats.glovesApplied;
	m_totalAgent += m_lastStats.agentsApplied;
	if (MvmDebugLog_Active()) {
		const CosmeticFrameStats& s = m_lastStats;
		bool event = s.entitiesMatched > 0 && (s.attrValuesChanged > 0 || s.knifeModelsApplied > 0 ||
			s.glovesApplied > 0 || s.agentsApplied > 0 || s.weaponMeshFixed > 0);
		if (event) {
			MvmDebugLog_Linef("cosmetics.apply", "matched=%d patched=%d attrChanged=%d "
				"knifeSwap=%d weaponMesh=%d gloves=%d agents=%d skinAttrsWritten=%d",
				s.entitiesMatched, s.entitiesPatched,
				s.attrValuesChanged, s.knifeModelsApplied, s.weaponMeshFixed, s.glovesApplied,
				s.agentsApplied, s.attrValuesWritten);
		}
	}
	UpdatePaintkitBridge();
}

// Manual, on-demand rebuild: re-asserts the visuals-stale field writes (forceStale) on every matched
// weapon entity even when nothing changed, and fires the recompose vcall once if armed. Decoupled
// from the per-frame change detection so the user can retrigger a refresh attempt for a screenshot.
// Does NOT require the apply loop to be enabled -- it is an explicit action -- but still hard-gated on
// offsets resolving, a demo playing, and at least one stored profile. Returns matched entity count.
int CosmeticOverrideSystem::RebuildOnce() {
	if (!g_cosmeticsOffsetsOk || !InDemoContext() || m_store.Empty())
		return 0;
	MvmCrashWatch_Arm(-1, "weapon-rebuild");
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
	m_lastStats.namedSetterApplied = 0;
	m_lastStats.directCompositeCalls = 0;
	m_lastStats.directCompositeResolved = 0;
	m_lastStats.directCompositeFaulted = 0;
	return ApplyMatchedWeapons(/*forceStale=*/true, /*fireRebuildCall=*/m_recompose,
		/*fireDirectComposite=*/m_autoComposite,
		/*allowKnifeSwap=*/true, /*periodicComposite=*/false);
}

int CosmeticOverrideSystem::CompositeOnce() {
	if (!g_cosmeticsOffsetsOk || !InDemoContext() || m_store.Empty())
		return 0;
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
	m_lastStats.namedSetterApplied = 0;
	m_lastStats.directCompositeCalls = 0;
	m_lastStats.directCompositeResolved = 0;
	m_lastStats.directCompositeFaulted = 0;
	return ApplyMatchedWeapons(/*forceStale=*/true, /*fireRebuildCall=*/false, /*fireDirectComposite=*/true,
		/*allowKnifeSwap=*/true, /*periodicComposite=*/false);
}

bool CosmeticOverrideSystem::SetRebuildFlag(const char* name, bool on) {
	if (!name) return false;
	if (0 == _stricmp(name, "visualsdata") || 0 == _stricmp(name, "visualsdataset")) m_rebuildFlags.visualsDataSet = on;
	else if (0 == _stricmp(name, "clearugc")) m_rebuildFlags.clearUgc = on;
	else if (0 == _stricmp(name, "reloadevent")) m_rebuildFlags.reloadEvent = on;
	else if (0 == _stricmp(name, "initialized") || 0 == _stricmp(name, "init")) m_rebuildFlags.initialized = on;
	else if (0 == _stricmp(name, "attrinit")) m_rebuildFlags.attrInit = on;
	else if (0 == _stricmp(name, "imagecache")) m_rebuildFlags.imageCache = on;
	else if (0 == _stricmp(name, "attrparity") || 0 == _stricmp(name, "parity")) m_rebuildFlags.attrParity = on;
	else return false;
	return true;
}

void CosmeticOverrideSystem::SetAllRebuildFlags(bool on) {
	m_rebuildFlags.visualsDataSet = on;
	m_rebuildFlags.clearUgc = on;
	m_rebuildFlags.reloadEvent = on;
	m_rebuildFlags.initialized = on;
	m_rebuildFlags.attrInit = on;
	m_rebuildFlags.imageCache = on;
	m_rebuildFlags.attrParity = on;
}

uint64_t CosmeticOverrideSystem::CurrentActiveWeaponOwnerSteamId() const {
	if (!g_cosmeticsOffsetsOk || !InDemoContext())
		return 0;

	const int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return 0;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon || !LooksLikeWeaponEntity(weapon))
		return 0;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	WeaponEconRead econ;
	if (!TryReadWeaponEconInfo((unsigned char*)weapon,
			o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
			o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
			o.C_EconItemView.m_iItemDefinitionIndex, &econ))
		return 0;

	return econ.xuid;
}

int CosmeticOverrideSystem::ResolveSpectatedPaintkitOverride() const {
	if (m_paintkitBridgeForcedValue > 0)
		return m_paintkitBridgeForcedValue;

	if (!m_store.Enabled() || !g_cosmeticsOffsetsOk || m_store.Empty() || !InDemoContext())
		return 0;

	const int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return 0;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon || !LooksLikeWeaponEntity(weapon))
		return 0;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	WeaponEconRead econ;
	if (!TryReadWeaponEconInfo((unsigned char*)weapon,
			o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
			o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
			o.C_EconItemView.m_iItemDefinitionIndex, &econ))
		return 0;

	if (econ.xuid == 0)
		return 0;

	const CosmeticProfile* prof = m_store.Find(econ.xuid);
	if (!prof)
		return 0;

	const CosmeticItem* item = nullptr;
	if (CosmeticCatalog::IsKnifeDef(econ.liveDef)) {
		if (prof->knife.set)
			item = &prof->knife;
	} else {
		const CosmeticItem* cand = prof->FindWeapon(econ.liveDef);
		if (cand && cand->set)
			item = cand;
	}

	if (!item || item->paintKit <= 0)
		return 0;
	return item->paintKit;
}

int CosmeticOverrideSystem::ApplyMatchedWeapons(bool forceStale, bool fireRebuildCall, bool fireDirectComposite,
	bool allowKnifeSwap, bool periodicComposite) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent)
			continue;
		if (!LooksLikeWeaponEntity(ent))
			continue; // gloves/agent are not econ weapon entities; handled separately below

		unsigned char* w = (unsigned char*)ent;

		// SEH-guarded read of owner XUID + embedded C_EconItemView pointer + live item-definition
		// index. A "weapon_" entity that is not actually a C_EconEntity is skipped here instead of
		// crashing. Dropped weapons keep OriginalOwnerXuid, so a dropped gun still carries its
		// owner's override.
		WeaponEconRead econ;
		if (!TryReadWeaponEconInfo(w,
				o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
				o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
				o.C_EconItemView.m_iItemDefinitionIndex, &econ))
			continue;

		uint64_t xuid = econ.xuid;
		if (xuid == 0)
			continue;

		CosmeticProfile* prof = m_store.Find(xuid);
		if (!prof)
			continue;

		++m_lastStats.entitiesScanned;

		unsigned char* itemView = econ.itemView;
		int liveDef = econ.liveDef;

		CosmeticItem* item = nullptr;
		int knifeDefOverride = 0;

		if (CosmeticCatalog::IsKnifeDef(liveDef)) {
			if (prof->knife.set) {
				item = &prof->knife;
				// Desired knife TYPE swap target, taken from the PROFILE -- NOT gated on
				// (item->defIndex != liveDef). ApplyCosmeticWrite writes m_iItemDefinitionIndex = target,
				// so the frame after the first write liveDef == target and the old gate computed 0; since
				// a profile change/seek also re-arms the 64-frame stability window, the "def differs" frame
				// and the "stable" frame never coincided and the model swap NEVER fired (confirmed in
				// mvm_debug: liveDef flipped 525->503 but liveModel stayed knife_skeleton). The per-entity
				// m_knifeSwapState throttle below still fires SetModel only ONCE per activation, so driving
				// this purely from the profile is safe.
				if (m_knifeModelSwap && item->defIndex > 0)
					knifeDefOverride = item->defIndex;
			}
		} else {
			// Per-weapon override: look the held weapon's own def up in the player's weapons map. This
			// is exactly "this player's M4 -> this M4 skin", independent per weapon type, so every gun
			// the player carries keeps its own skin.
			CosmeticItem* cand = prof->FindWeapon(liveDef);
			if (cand && cand->set)
				item = cand;
		}

		if (!item)
			continue;

		++m_lastStats.entitiesMatched;

		// THE real skin write (Phase 2): overwrite the networked dynamic attributes (def 6/7/8/81),
		// where a demo weapon's actual skin lives. The fallback-field ApplyCosmeticWrite below is now
		// a harmless backup (those fields are ignored while networked attributes are present).
		int attrStride = (int)o.CEconItemAttribute.m_size;
		int attrMinStride = (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float);
		if (attrStride < attrMinStride) attrStride = attrMinStride;
		AttrWriteResult attr;
		WriteNetworkedSkinAttributes(itemView,
			o.C_EconItemView.m_NetworkedDynamicAttributes, o.C_AttributeList.m_Attributes, attrStride,
			o.CEconItemAttribute.m_iAttributeDefinitionIndex, o.CEconItemAttribute.m_flValue,
			item->paintKit, item->seed, item->wear, item->statTrak, &attr);
		if (attr.ok) {
			++m_lastStats.attrListsRead;
			m_lastStats.attrValuesWritten += attr.written;
			if (attr.changed)
				++m_lastStats.attrValuesChanged;
			if (attr.written == 0)
				++m_lastStats.attrListsEmpty;
		}

		// DEFAULT-ITEM PAINT (the "pistol went to default no-skin" fix): a default/vanilla demo weapon
		// has NO networked paint attribute (attr.hasPaint == false), so the overwrite path can't paint it
		// and the named-setter alone did not render (confirmed: a default Deagle stayed default). For
		// these items, force the legacy fallback path (m_iItemIDHigh = -1) so the client composites from
		// the m_nFallbackPaintKit/Wear/Seed fields we write -- the proven nSkinz mechanism. Scoped to
		// no-paint-attr items with an actual paint pick, so painted demo weapons (the AK) keep their HUD
		// identity via the overwrite path. (Trade-off: the HUD weapon icon for a re-skinned DEFAULT gun
		// may blank, which is acceptable for movie work and only affects guns that were vanilla in the demo.)
		bool useFallbackId = m_fallbackId || (!attr.hasPaint && item->paintKit > 0);

		ApplyResult result = ApplyCosmeticWrite(
			w,
			itemView,
			w + o.C_EconEntity.m_AttributeManager,
			o.C_EconItemView.m_iItemIDHigh,
			o.C_EconEntity.m_nFallbackPaintKit,
			o.C_EconEntity.m_flFallbackWear,
			o.C_EconEntity.m_nFallbackSeed,
			o.C_EconEntity.m_nFallbackStatTrak,
			o.C_EconItemView.m_iItemDefinitionIndex,
			o.C_EconItemView.m_iItemIDLow,
			o.C_EconItemView.m_iAccountID,
			o.C_EconItemView.m_bInitialized,
			o.C_EconItemView.m_bInitializedTags,
			o.C_EconEntity.m_bAttributesInitialized,
			o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache,
			o.C_EconItemView.m_bInventoryImageRgbaRequested,
			o.C_EconItemView.m_bInventoryImageTriedCache,
			o.C_EconItemView.m_szCurrentLoadCachedFileName,
			o.CAttributeManager.m_iReapplyProvisionParity,
			o.C_CSWeaponBase.m_bVisualsDataSet,
			o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC,
			o.C_CSWeaponBase.m_nCustomEconReloadEventId,
			(int32_t)item->paintKit,
			item->wear,
			(int32_t)item->seed,
			(int32_t)item->statTrak,
			(uint32_t)xuid,
			knifeDefOverride,
			useFallbackId,
			m_rebuildAuto,
			forceStale,
			m_rebuildFlags);

		if (result.patched)
			++m_lastStats.entitiesPatched;
		if (result.reverted)
			++m_lastStats.entitiesReverted;

		// Whether attrs/mesh need touching this frame (may be every tick during playback).
		bool refreshWarranted = result.needComposite || attr.changed || forceStale || periodicComposite;
		// Direct composite + FP mirror: debounced -- NOT on every attr.changed (demo packet clobber).
		bool compositeWarranted = result.needComposite || forceStale || periodicComposite;
		if (!compositeWarranted && attr.changed) {
			uint64_t last = 0;
			auto it = m_lastCompositeFrameByIdx.find(i);
			if (it != m_lastCompositeFrameByIdx.end())
				last = it->second;
			if (m_frameCounter >= last + kClobberCompositeCooldownFrames)
				compositeWarranted = true;
		}

		// MISSING-ATTRIBUTE FALLBACK (the attrWritten=0 fix): when the item has no networked paint
		// attribute to overwrite (attr.hasPaint == false -- default/vanilla weapons, picked-up or
		// viewmodel entities), apply the skin through the engine named-setter so it still paints. The
		// composite below renders it. Idempotent, so it is safe even when the composite path also calls
		// the setter; gated to the refresh cadence so it is not run redundantly every frame.
		bool namedSetter = false;
		if (item->paintKit > 0 && !attr.hasPaint && refreshWarranted) {
			namedSetter = FireNamedSkinAttributes(itemView, item->paintKit, item->wear, item->seed, item->statTrak);
			if (namedSetter)
				++m_lastStats.namedSetterApplied;
		}

		// Optional forced re-composite (OFF by default; see SetRecompose). Writing the fallback
		// fields + invalidating the init flags is not always enough to make a demo entity visually
		// rebuild its skin material -- this calls the weapon's UpdateComposite vtable method when a
		// (re)composite is warranted. SEH-guarded: a wrong index disables recompose, never crashes.
		// fireRebuildCall lets RunFrame keep this opt-in (only when m_recompose is armed) while the
		// manual "rebuild once" path forces a call attempt on every matched entity (forceStale).
		if (fireRebuildCall && (result.needComposite || attr.changed || forceStale)) {
			bool ok = true;
			if (m_vtComposite >= 0) ok = SafeVCall(ent, m_vtComposite, m_vtArg);
			if (ok && m_vtCompositeSec >= 0) ok = SafeVCall(ent, m_vtCompositeSec, m_vtArg);
			if (!ok) { m_recompose = false; m_recomposeFaulted = true; }
		}

		// Experimental Andromeda-style direct refresh (OFF except "cosmetics composite once"). This is
		// distinct from the vtable-probe path above: it calls the named client.dll functions resolved
		// by byte signatures, using the researched weapon+ownerOffset composite owner pointer. The
		// call is SEH-guarded and reports resolved/called/faulted counters.
		// MODEL SWAP (knife TYPE swap / legacy-vs-CS2 weapon mesh) -- run BEFORE the composite below so
		// the skin re-composites onto the NEW model/mesh. Change-gated like the composite; all calls are
		// SEH-guarded inside CosmeticModelSwap. The def index was already written by ApplyCosmeticWrite
		// (knifeDefOverride), so GetStaticData resolves the target knife model. Resolve the original
		// owner's pawn so the first-person HUD weapon receives the same model/mesh refresh.
		// ownerPawn drives ONLY the first-person viewmodel refresh. Gate it to the case where THIS
		// weapon is the pawn's ACTIVE (deployed) weapon: a holstered weapon has no first-person
		// viewmodel, and walking the HUD-arms children while a DIFFERENT weapon is mid-deploy (e.g.
		// switching knife->AK during live playback, where the knife swap re-fires on entity recreation)
		// reaches the other weapon's half-built viewmodel and CRASHES on its next animation. The WORLD
		// (third-person) model/mesh swap below still runs for every matched weapon -- only the
		// first-person viewmodel mirror is gated to the active weapon.
		// Resolve the HOLDING pawn + whether THIS entity is its ACTIVE (deployed) weapon, EVERY matched
		// frame (not gated on a change). The knife swap now fires when the knife BECOMES active even if
		// the profile was changed earlier (while holstered, or during a seek) -- a standing condition, not
		// a one-frame event -- and the diagnostic log reports active-ness. ownerPawn != null <=> active.
		//
		// Resolve via the weapon ENTITY's own live m_hOwnerEntity (who is actually carrying it right
		// now), NOT via PawnForSteamId(xuid) -- xuid here is the econ/skin OWNER, a DIFFERENT player than
		// the holder for a picked-up weapon. Looking up the econ owner's pawn and checking ITS active
		// weapon handle against `i` can never succeed for a pickup (the econ owner isn't the one holding
		// this entity -- the picker-upper is), which silently disabled the first-person viewmodel mirror
		// (RefreshViewmodelWeapons, gated on ownerPawn below) for EVERY picked-up weapon regardless of
		// what skin was applied. Confirmed live: reskinning a teammate's picked-up AWP via the Customize
		// UI wrote the new paint attribute every click (uiclick log showed netPaint changing) but the
		// first-person view never updated, because ownerPawn was always null for that entity.
		unsigned char* ownerPawn = nullptr;
		{
			SOURCESDK::CS2::CBaseHandle oh = ent->GetOwnerEntityHandle();
			CEntityInstance* holder = oh.IsValid() ? EntFromIndex(oh.GetEntryIndex()) : nullptr;
			unsigned char* pawn = (holder && holder->IsPlayerPawn()) ? (unsigned char*)holder : nullptr;
			if (pawn) {
				SOURCESDK::CS2::CBaseHandle aw = ((CEntityInstance*)pawn)->GetActiveWeaponHandle();
				if (aw.IsValid() && aw.GetEntryIndex() == i)
					ownerPawn = pawn;
			}
		}
		bool knifeFired = false; // set true if the knife model/type swap actually ran this frame (diag)
		// Mesh-group (legacy CS:GO vs modern CS2 model) diagnostics for the per-weapon log line below.
		// A weapon .vmdl carries BOTH meshes; SetMeshGroupMask selects which one renders, and each paint
		// kit is authored for one. dbgMeshLegacy: 1=legacy / 0=modern / -1=unknown (schema lookup failed)
		// / -2=not evaluated this frame. When it is -1 the resolved mask is 0 and the mesh is LEFT
		// UNTOUCHED -- so a skin authored for the OTHER mesh than the one the weapon currently shows will
		// not render (the user's "the model doesn't switch" symptom).
		int dbgMeshLegacy = -2;
		unsigned long long dbgMeshMask = 0;
		int dbgMeshApplied = 0;
		if (m_modelSwap) {
			bool isKnife = CosmeticCatalog::IsKnifeDef(liveDef);
			// The non-knife weapon mesh-group fix is cheap (SetMeshGroupMask + PostDataUpdate) and is
			// re-asserted every matched frame when the live mesh mask differs (see the else-branch below).
			if (isKnife) {
				// Fire when the knife is the owner's ACTIVE weapon + the post-seek stability window elapsed +
				// its def is still the wrong type. NO attr-change-this-frame requirement (after a seek the
				// change and "stable" never coincide -- that is why an edited knife never swapped). A per-owner
				// throttle keyed on the knife entity index + target def fires it ONCE per activation: the engine
				// reverts the def every tick, but we re-fire only on entity recreation (new index) or a new
				// target, never every frame (the per-frame re-swap was the crash).
				// NOTE: no longer gated on ownerPawn (active). The WORLD (third-person) knife model swap must
				// run for a HOLSTERED knife too -- otherwise, after a seek recreated the entity, the skin
				// paint re-applied (ungated) while the model stayed the ORIGINAL knife, showing a mismatched
				// model+skin (e.g. Butterfly mesh + Bayonet's skin) until the knife was next deployed. The
				// first-person viewmodel mirror inside ApplyKnifeModelSwap auto-skips when pawnForViewmodel
				// is null (holstered), and we re-fire on the holstered->active edge to run it once. Safe now
				// that CosmeticAnimFix neutralizes the unloaded-model anim crash the active-only gate avoided.
				if (m_knifeModelSwap && knifeDefOverride > 0) {
					KnifeSwapState& ks = m_knifeSwapState[xuid];
					bool recreated = (ks.entityIndex != i);
					bool newTarget = (ks.swappedDef != knifeDefOverride);
					bool active = (ownerPawn != nullptr);
					bool becameActive = active && !ks.lastActive; // deploy edge -> run the viewmodel mirror once
					// Self-correcting trigger: re-fire whenever the knife's LIVE world model isn't our target.
					// A demo seek can reconstruct the knife in the SAME entity slot (index unchanged) while
					// resetting its model to the original -- the index/active-edge throttles never trip, so the
					// knife kept its ORIGINAL model with the swapped skin until it was re-deployed. Comparing the
					// actual rendered model path catches that case (and any other silent revert). Settles to
					// false in one frame once SetModel takes, so it does not thrash.
					bool modelMismatch = false;
					{
						char targetModel[260];
						if (ResolveKnifeModelPath(knifeDefOverride, itemView, targetModel, sizeof(targetModel)) && targetModel[0]) {
							char liveKnifeModel[160];
							ReadEntityModelPath(ent, liveKnifeModel, sizeof(liveKnifeModel));
							modelMismatch = liveKnifeModel[0] && 0 != _stricmp(liveKnifeModel, targetModel);
						}
					}
					if (recreated || newTarget || becameActive || modelMismatch) {
						// Always-flushed re-fire decision. recreated=1 (a NEW entity index for the same owner)
						// is the seek/quick-switch case: the engine destroyed+recreated the knife entity, so
						// the swap re-fires onto the rebuilt entity. framesSinceSeek shows how settled the
						// world is; allowSwap=0 means SUPPRESSED until the (short) post-seek window elapses.
						// active=0 = holstered (world model swap only; viewmodel mirror skipped).
						if (MvmDebugLog_Active())
							MvmDebugLog_LinefAlways("knife.fire",
								"xuid=%llu idx=%d prevIdx=%d liveDef=%d targetDef=%d prevDef=%d recreated=%d newTarget=%d active=%d modelMismatch=%d allowSwap=%d framesSinceSeek=%d -> %s",
								(unsigned long long)xuid, i, ks.entityIndex, liveDef, knifeDefOverride, ks.swappedDef,
								recreated ? 1 : 0, newTarget ? 1 : 0, active ? 1 : 0, modelMismatch ? 1 : 0, allowKnifeSwap ? 1 : 0, m_framesSinceSeek,
								allowKnifeSwap ? "FIRE" : "SUPPRESS-settle");
						if (allowKnifeSwap) {
							uint64_t mask = ResolveMeshMask(item->paintKit, /*knife=*/true,
								m_meshLegacyMode, m_maskModern, m_maskLegacy);
							if (mask == 0) mask = m_maskModern; // a knife model always needs a mesh group
							// ownerPawn null when holstered -> world swap runs, viewmodel mirror is skipped.
							knifeFired = ApplyKnifeModelSwap(w, itemView, ownerPawn, knifeDefOverride, mask, i);
							if (knifeFired) {
								++m_lastStats.knifeModelsApplied;
								ks.entityIndex = i;
								ks.swappedDef = knifeDefOverride;
							}
						}
					}
					ks.lastActive = active; // track each matched frame so a holstered->active edge fires once
				}
			} else if (item->paintKit > 0) {
				// Mesh-group (legacy CS:GO vs modern CS2 model) selection, only actually WRITTEN when the
				// LIVE mesh mask differs from our target, so we correct an engine revert (the user's "the
				// model doesn't switch when I pick an older/newer skin" bug) without thrashing the
				// renderable when it is already right.
				uint64_t mask = ResolveMeshMask(item->paintKit, /*knife=*/false,
					m_meshLegacyMode, m_maskModern, m_maskLegacy);
				dbgMeshMask = mask;
				// Surface the raw legacy decision in the log (only while debugging -- it walks the econ
				// schema). -1 here means the lookup could not classify this paint kit, so the mesh is left
				// untouched and the skin may not render on whatever mesh the weapon currently shows.
				if (MvmDebugLog_Active())
					dbgMeshLegacy = PaintKitLegacyModel(item->paintKit);
				if (mask != 0) {
					uint64_t liveMask = ReadEntityMeshGroupMask(ent);
					// Re-apply when live mask differs, paint just changed, or an explicit rebuild was requested.
					// attr.changed matters even when liveMask == mask already: a same-mesh-group re-pick (e.g.
					// legacy skin A -> legacy skin B) still needs ApplyWeaponMeshMask/RefreshViewmodelWeapons's
					// SetMeshGroupMask+PostDataUpdate re-assert on the HUD-arms child so it re-picks up the
					// world weapon's freshly-composited material -- confirmed live: writing the new paint
					// attribute alone (mesh unchanged) rendered the DEFAULT/blank weapon in first person, not
					// the new skin. This previously also re-fired the now-removed MirrorWeaponCosmeticsToViewmodel
					// call (which caused a fatal OOM crash by treating the HUD-arms child as a C_EconEntity it
					// isn't -- see the removal comment below); with that call gone, the remaining side effects
					// here (SetModel/SetMeshGroupMask/ResetAnimGraph/PostDataUpdate) are all safe, legitimate
					// C_BaseModelEntity-level operations on that entity.
					bool needMesh = (liveMask != mask) || forceStale || result.needComposite || attr.changed;
					if (needMesh) {
						// No cooldown/throttle here -- fire every matched frame the mismatch (or a fresh
						// paint pick) persists. A throttle was tried (space out retries per entity+target)
						// to reduce how often RefreshViewmodelWeapons's scene-graph walk ran, back when
						// MirrorWeaponCosmeticsToViewmodel was ALSO firing on the same trigger and its
						// broken econ-attribute writes (into a HUD-arms child that isn't a C_EconEntity --
						// see the removal comment below) caused real crashes. Confirmed live the throttle
						// itself broke correctness: the engine reverts this weapon's mesh group EVERY
						// SINGLE FRAME (preMask=2 -> we write postMask=1 -> next frame preMask=2 again,
						// forever), so any gap between corrections leaves the render sitting in the WRONG
						// state for however many frames the gap lasts -- confirmed by capturing worldMask=2
						// via visualdiag moments after a modern-paint apply that should have shown mask=1.
						// With MirrorWeaponCosmeticsToViewmodel gone, RefreshViewmodelWeapons's own walk is
						// safe to call unconditionally every frame (SEH-guarded, only touches SetModel/
						// SetMeshGroupMask/ResetAnimGraph/PostDataUpdate on the C_BaseModelEntity ancestry
						// this entity legitimately has) -- live-tested crash-free across repeated rapid
						// same-mesh-group and cross-mesh-group repaints.
						ApplyWeaponMeshMask(w, mask, ownerPawn, i);
						++m_lastStats.weaponMeshFixed;
						dbgMeshApplied = 1;
					}
					if (MvmDebugLog_Active() && (needMesh || dbgMeshLegacy < 0)) {
						uint64_t postMask = ReadEntityMeshGroupMask(ent);
						MvmDebugLog_LinefAlways("cosmetics.weapon.mesh",
							"idx=%d def=%d wantPaint=%d wantLegacy=%d wantMask=%llu preMask=%llu postMask=%llu applied=%d paintChanged=%d",
							i, liveDef, item->paintKit, dbgMeshLegacy,
							(unsigned long long)mask, (unsigned long long)liveMask,
							(unsigned long long)postMask, dbgMeshApplied, attr.changed ? 1 : 0);
					}
				} else if (MvmDebugLog_Active()) {
					MvmDebugLog_LinefAlways("cosmetics.weapon.mesh",
						"SKIP idx=%d def=%d wantPaint=%d legacyLookup=-1 (mesh untouched -- skin may not render)",
						i, liveDef, item->paintKit);
				}
			}
		}

		if (fireDirectComposite && compositeWarranted) {
			DirectCompositeResult direct = FireDirectCompositeRefresh(
				w,
				itemView,
				o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache,
				m_compositeOwnerOffset,
				(int32_t)item->paintKit,
				item->wear,
				(int32_t)item->seed);
			if (direct.resolved)
				m_lastStats.directCompositeResolved = 1;
			if (direct.called) {
				++m_lastStats.directCompositeCalls;
				m_lastCompositeFrameByIdx[i] = m_frameCounter;
				if (MvmDebugLog_Active()) {
					const char* why = result.needComposite ? "apply" : (forceStale ? "force" :
						(periodicComposite ? "burst" : "clobber"));
					MvmDebugLog_Linef("cosmetics.composite", "idx=%d def=%d paint=%d reason=%s",
						i, liveDef, item->paintKit, why);
				}
			}
			if (direct.faulted)
				m_lastStats.directCompositeFaulted = 1;
		}

		// REMOVED: MirrorWeaponCosmeticsToViewmodel call. Root-caused against the CS2-OFFSETS schema
		// dump (client_dll.json): the first-person HUD-arms weapon is a C_CS2HudModelWeapon, whose parent
		// chain is C_CS2HudModelWeapon -> C_CS2HudModelBase -> C_LateUpdatedAnimating -> CBaseAnimGraph ->
		// C_BaseModelEntity -> C_BaseEntity. C_EconEntity ALSO derives from CBaseAnimGraph but is a
		// SIBLING branch, not an ancestor -- C_CS2HudModelWeapon has NO C_EconEntity fields at all
		// (no m_AttributeManager, no fallback paint/wear/seed, nothing). This function computed
		// "vmWeapon + o.C_EconEntity.m_AttributeManager" and "vmWeapon + m_compositeOwnerOffset" as if it
		// were a real econ weapon, which are both meaningless reads into unrelated C_CS2HudModelWeapon
		// memory. Confirmed live: paintBefore/paintAfter always read -1 (garbage "item view" pointer) and
		// the direct-composite call always faulted (fault=1); re-firing it more often (to fix a separate
		// same-mesh-group repaint bug) triggered a ~154GB allocation attempt and a fatal OOM crash.
		//
		// The actual fix for "first person doesn't show the new skin" is RefreshViewmodelWeapons (called
		// from ApplyWeaponMeshMask above via ownerPawn) -- it only touches SetModel/SetMeshGroupMask/
		// ResetAnimGraph/PostDataUpdate, all on the C_BaseModelEntity/CBaseAnimGraph ancestry that
		// C_CS2HudModelWeapon legitimately shares with real weapons. No econ-attribute write is needed on
		// the HUD-arms child at all -- the composited material is a shared resource keyed by model path +
		// mesh group, already generated correctly by the WORLD weapon's own (correctly-offset) composite
		// call above; the viewmodel just needs to be told to re-pick it up, which the mesh/model mirror
		// already does.

		// Per-matched-weapon diagnostic (deduped per unique payload). The decisive field is
		// attrWritten: a demo weapon's REAL skin lives in m_NetworkedDynamicAttributes (def 6 paint /
		// 7 seed / 8 wear) -- attrWritten=0 means that attribute is NOT present on this item, so the
		// paint cannot be written there (the skin will not render via the networked-attr path no matter
		// what). attrOk=0 means the attribute vector itself could not be read. needComposite reflects
		// whether a value differed this frame (drives the re-composite / model-swap fire).
		if (MvmDebugLog_Active()) {
			// idx + class distinguish the multiple entities one weapon can have (world model vs first-person
			// viewmodel vs a picked-up copy). active=1 means it is the owner's deployed weapon. liveModel is
			// the ACTUAL rendered model path -- a knife swap that did not take shows the original model here.
			// attrWritten=0 = this entity has NO networked paint attribute to write (can't be skinned via the
			// networked-attr path) -- the typical attrWritten=0 entity is the viewmodel / a picked-up weapon.
			const char* cls = ent->GetClassName();
			char liveModel[160];
			ReadEntityModelPath(ent, liveModel, sizeof(liveModel));
			// LIVE rendered mesh-group mask (post-apply). Compare across frames: if it != our meshMask on
			// a later (meshApplied=0) frame, the engine reverted it; the value on a CORRECTLY-rendering
			// weapon is the real legacy/modern mask to target.
			unsigned long long liveMeshMask = (unsigned long long)ReadEntityMeshGroupMask(ent);
			// writePath shows HOW the skin was written: "overwrite" = an existing networked def-6 paint
			// attr was overwritten; "namedSetter" = no paint attr existed so the engine named-setter
			// fallback applied it (the attrWritten=0 fix); "none" = neither (e.g. paintKit 0 / vanilla).
			const char* writePath = attr.hasPaint ? "overwrite"
				: (useFallbackId ? (namedSetter ? "fallbackId+named" : "fallbackId")
					: (namedSetter ? "namedSetter" : "none"));
			MvmDebugLog_Linef("cosmetics.weapon",
				"idx=%d cls='%s' xuid=%llu liveDef=%d active=%d isKnife=%d knifeSwapDef=%d knifeFired=%d "
				"allowKnife=%d paint=%d wear=%.3f seed=%d stat=%d attrOk=%d attrWritten=%d hasPaint=%d "
				"writePath=%s attrChanged=%d patched=%d needComposite=%d "
				"meshMode=%d meshLegacy=%d meshMask=%llu meshApplied=%d liveMeshMask=%llu liveModel='%s'",
				i, cls ? cls : "?", (unsigned long long)xuid, liveDef, ownerPawn ? 1 : 0,
				CosmeticCatalog::IsKnifeDef(liveDef) ? 1 : 0, knifeDefOverride, knifeFired ? 1 : 0,
				allowKnifeSwap ? 1 : 0, item->paintKit, item->wear, item->seed, item->statTrak,
				attr.ok ? 1 : 0, attr.written, attr.hasPaint ? 1 : 0, writePath, attr.changed ? 1 : 0,
				result.patched ? 1 : 0, result.needComposite ? 1 : 0,
				m_meshLegacyMode, dbgMeshLegacy, dbgMeshMask, dbgMeshApplied, liveMeshMask, liveModel);
		}
	}

	// GLOVES + AGENT are pawn-level (not econ weapon entities) and are applied separately in
	// ApplyPawnCosmetics(), called from RunFrame after this weapon pass. See CosmeticModelSwap.cpp.
	return m_lastStats.entitiesMatched;
}

bool CosmeticOverrideSystem::ModelSwapResolved() const {
	return ResolveModelSwapFns().CoreOk();
}

// Pawn-level cosmetics: walk player controllers, resolve each one's pawn + owner SteamID, and apply
// the agent (player) model swap + glove model for any profiled player. Glove apply is change-gated
// (re-fires for a few frames after a glove/paint change, a respawn, or the engine setting
// m_bNeedToReApplyGloves) so the body group is not rebuilt every frame. Agent swap is hash-gated so
// SetModel only fires when the chosen model changes (or the engine recreated the pawn). All entity
// writes are SEH-guarded inside CosmeticModelSwap.
int CosmeticOverrideSystem::ApplyPawnCosmetics() {
	if (!m_modelSwap) {
		if (MvmDebugLog_Active())
			MvmAgentLog("H1", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "modelswap_disabled", "");
		return 0;
	}

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (MvmDebugLog_Active() && o.C_CSPlayerPawn.m_EconGloves == 0) {
		MvmAgentLog("H4", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "econ_gloves_offset_missing",
			"\"m_EconGloves\":0");
	}
	const uint64_t spectatedSteamId = CurrentSpectatedSteamId();
	if (spectatedSteamId != 0)
		m_lastSpectatedSteamId = spectatedSteamId;
	if (m_gloveBypassFrames > 0)
		--m_gloveBypassFrames;
	// Profiled players only get glove writes while spectated; re-arm whenever the spectate target
	// changes so switching back to a player always runs a fresh burst (not only on first UI set).
	if (spectatedSteamId != 0 && spectatedSteamId != m_lastGloveSpectatedSid) {
		if (MvmDebugLog_Active()) {
			char data[128];
			std::snprintf(data, sizeof(data),
				"\"from\":%llu,\"to\":%llu",
				(unsigned long long)m_lastGloveSpectatedSid, (unsigned long long)spectatedSteamId);
			MvmAgentLog("H8", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
				"glove_rearm_spectate_change", data);
		}
		m_lastGloveSpectatedSid = spectatedSteamId;
		CosmeticProfile* sp = m_store.Find(spectatedSteamId);
		if (sp && sp->gloves.set && sp->gloves.defIndex > 0) {
			GloveApplyState& gst = m_gloveState[spectatedSteamId];
			gst.frames = 4;
			gst.pawnStable = 0;
		}
	}
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ctrl = EntFromIndex(i);
		if (!ctrl || !ctrl->IsPlayerController())
			continue;
		uint64_t steamId = ctrl->GetSteamId();
		if (steamId == 0)
			continue;
		CosmeticProfile* prof = m_store.Find(steamId);
		if (!prof || (!prof->agent.set && !prof->gloves.set))
			continue;

		SOURCESDK::CS2::CBaseHandle ph = ctrl->GetPlayerPawnHandle();
		CEntityInstance* pawnEnt = ph.IsValid() ? EntFromIndex(ph.GetEntryIndex()) : nullptr;
		if (!pawnEnt || !pawnEnt->IsPlayerPawn())
			continue;
		unsigned char* pawn = (unsigned char*)pawnEnt;
		++m_lastStats.pawnsScanned;

		// AGENT (player model). Gate on both model hash and pawn pointer so a seek/entity recreation
		// always reapplies even when the requested model path did not change.
		if (prof->agent.set && !prof->agent.model.empty()) {
			uint64_t h = 1469598103934665603ULL; // FNV-1a of the model path
			for (char c : prof->agent.model) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
			auto it = m_agentState.find(steamId);
			if (it == m_agentState.end() || it->second.hash != h || it->second.pawn != (uintptr_t)pawn) {
				if (ApplyAgentModel(pawn, prof->agent.model.c_str())) {
					++m_lastStats.agentsApplied;
					m_agentState[steamId] = { h, (uintptr_t)pawn };
				}
			} else {
				// Re-assert periodically so a demo seek that recreated the pawn re-applies. Cheap: a
				// SetModel with the same path is a no-op once cached, and only one pawn per profile.
				if ((m_lastStats.frame % 32) == 0)
					ApplyAgentModel(pawn, prof->agent.model.c_str());
			}
		}

		// GLOVES. Change-gated multi-frame apply.
		if (prof->gloves.set && prof->gloves.defIndex > 0 && o.C_CSPlayerPawn.m_EconGloves) {
			GloveApplyState& st = m_gloveState[steamId];
			const bool uiBypass = (m_gloveBypassSid == steamId && m_gloveBypassFrames > 0);
			const bool spectatedMatch = (spectatedSteamId != 0 && steamId == spectatedSteamId);
			const bool stickyBurst = (spectatedSteamId == 0 && m_lastSpectatedSteamId == steamId && st.frames > 0);
			if (!spectatedMatch && !uiBypass && !stickyBurst) {
				if (MvmDebugLog_Active() && (m_lastStats.frame % 64) == 0) {
					char data[200];
					std::snprintf(data, sizeof(data),
						"\"steamId\":%llu,\"spectated\":%llu,\"uiBypass\":%d,\"sticky\":%d,\"framesLeft\":%d",
						(unsigned long long)steamId, (unsigned long long)spectatedSteamId,
						uiBypass ? 1 : 0, stickyBurst ? 1 : 0, st.frames);
					MvmAgentLog("H7", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
						"glove_skip_not_spectated", data);
				}
				continue;
			}

			// Track pawn recreation before the team gate -- side switches briefly report team=0 and
			// must still re-arm the burst when the pawn entity is recreated.
			if (st.pawn != (uintptr_t)pawn) {
				const bool armRecreate = spectatedMatch || uiBypass;
				st.pawn = (uintptr_t)pawn;
				st.pawnStable = 0;
				if (armRecreate) {
					st.frames = 4;
					if (MvmDebugLog_Active()) {
						char data[96];
						std::snprintf(data, sizeof(data), "\"steamId\":%llu", (unsigned long long)steamId);
						MvmAgentLog("H8", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
							"glove_rearm_pawn_recreate", data);
					}
				} else {
					st.frames = 0;
				}
			}

			uint8_t team = 0;
			if (o.C_BaseEntity.m_iTeamNum) {
				__try { team = *(uint8_t*)(pawn + o.C_BaseEntity.m_iTeamNum); }
				__except (1) { team = 0; }
			}
			// Spectate/player-switch recreates the pawn with team=0 briefly -- defer writes until valid.
			if (team != 2 && team != 3) {
				if (MvmDebugLog_Active() && (m_lastStats.frame % 32) == 0) {
					char data[128];
					std::snprintf(data, sizeof(data),
						"\"steamId\":%llu,\"team\":%u,\"framesLeft\":%d",
						(unsigned long long)steamId, (unsigned)team, st.frames);
					MvmAgentLog("H9", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
						"glove_skip_team_unready", data);
				}
				continue;
			}
			if (st.pawnStable < 4) {
				++st.pawnStable;
				continue;
			}

			if (st.frames == 0 && (spectatedMatch || uiBypass)) {
				const int liveDef = SafeReadGloveDefIndex(pawn);
				bool engineWantsReapply = false;
				if (o.C_CSPlayerPawn.m_bNeedToReApplyGloves) {
					__try {
						engineWantsReapply = *(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves);
					} __except (1) {
						engineWantsReapply = false;
					}
				}
				if (liveDef != prof->gloves.defIndex || engineWantsReapply) {
					st.frames = 4;
					if (MvmDebugLog_Active()) {
						char data[160];
						std::snprintf(data, sizeof(data),
							"\"steamId\":%llu,\"liveDef\":%d,\"wantDef\":%d,\"engineReapply\":%d",
							(unsigned long long)steamId, liveDef, prof->gloves.defIndex,
							engineWantsReapply ? 1 : 0);
						MvmAgentLog("H8", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
							"glove_rearm_live_mismatch", data);
					}
				}
			}

			uint32_t wearBits = 0;
			std::memcpy(&wearBits, &prof->gloves.wear, sizeof(wearBits));
			uint64_t sig = 1469598103934665603ULL;
			const uint32_t sigParts[] = { (uint32_t)prof->gloves.defIndex,
				(uint32_t)prof->gloves.paintKit, (uint32_t)prof->gloves.seed, wearBits, (uint32_t)team };
			for (uint32_t part : sigParts) { sig ^= part; sig *= 1099511628211ULL; }
			float spawn = SafeReadEntityFloat(pawn, o.C_CSPlayerPawn.m_flLastSpawnTimeIndex, -1.0f);

			if (st.sig != sig || st.lastSpawn != spawn) {
				st.frames = 4; // apply over a few frames (gloves rebuild across frames)
				st.sig = sig;
				st.lastSpawn = spawn;
			}
			if (st.frames > 0) {
				if (m_gloveApplyBudget <= 0)
					continue;
				--m_gloveApplyBudget;
				bool composePaint = (st.frames == 4) && m_constructPaintBudget > 0;
				if (composePaint)
					--m_constructPaintBudget;
				if (MvmDebugLog_Active()) {
					char data[360];
					std::snprintf(data, sizeof(data),
						"\"steamId\":%llu,\"wantDef\":%d,\"wantPaint\":%d,\"framesLeft\":%d,\"pawn\":%llu,"
						"\"team\":%u,\"composePaint\":%d,\"pawnStable\":%d",
						(unsigned long long)steamId, prof->gloves.defIndex, prof->gloves.paintKit, st.frames,
						(unsigned long long)(uintptr_t)pawn, (unsigned)team, composePaint ? 1 : 0, st.pawnStable);
					MvmAgentLog("H6", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "glove_apply_fire", data);
				}
				if (ApplyGloveModel(pawn, prof->gloves.defIndex, prof->gloves.paintKit,
						prof->gloves.wear, prof->gloves.seed, (uint32_t)steamId, composePaint))
					++m_lastStats.glovesApplied;
				--st.frames;
			} else if (MvmDebugLog_Active() && (m_lastStats.frame % 128) == 0) {
				char data[192];
				std::snprintf(data, sizeof(data),
					"\"steamId\":%llu,\"wantDef\":%d,\"framesLeft\":0,\"sig\":%llu",
					(unsigned long long)steamId, prof->gloves.defIndex, (unsigned long long)st.sig);
				MvmAgentLog("H6", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "glove_gate_idle", data);
			}
		}
	}

	m_lastStats.modelSwapResolved = ResolveModelSwapFns().CoreOk() ? 1 : 0;
	return m_lastStats.pawnsScanned;
}

} // namespace Filmmaker
