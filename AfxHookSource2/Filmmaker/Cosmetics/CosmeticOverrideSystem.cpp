#include "CosmeticOverrideSystem.h"

#include "CosmeticCatalog.h"
#include "CosmeticModelSwap.h"
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

typedef void (__fastcall* DirectUpdateCompositeMaterial_t)(void* compositeOwner, bool force);
typedef void (__fastcall* DirectUpdateCompositeMaterialSet_t)(void* weapon, bool force);
typedef void (__fastcall* DirectUpdateSkin_t)(void* weapon, bool force);
typedef void (__fastcall* DirectSetAttributeValueByName_t)(void* itemView, const char* attrName, float value);

struct DirectCompositeFns {
	DirectUpdateCompositeMaterial_t updateCompositeMaterial;
	DirectUpdateCompositeMaterialSet_t updateCompositeMaterialSet;
	DirectUpdateSkin_t updateSkin;
	DirectSetAttributeValueByName_t setAttributeValueByName;
	bool resolved;
};

struct DirectCompositeResult {
	bool resolved;
	bool called;
	bool faulted;
};

void* ResolveRelCall(size_t callAddr) {
	if (!callAddr)
		return nullptr;
	int32_t rel = *(int32_t*)(callAddr + 1);
	return (void*)(callAddr + 5 + rel);
}

size_t FindClientTextPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

const DirectCompositeFns& ResolveDirectCompositeFns() {
	static bool s_attempted = false;
	static DirectCompositeFns s_fns = {};
	if (s_attempted)
		return s_fns;
	s_attempted = true;

	// Static-research signatures from Andromeda-CS2-Base. The two SEARCH_TYPE_CALL patterns resolve
	// to their E8 target; the others are function prologues and are used directly.
	// NOTE: FindPatternString consumes TWO hex chars per byte, so a wildcard byte MUST be "??"
	// (a single "?" is read as half a byte and silently shifts every literal after it -> no match,
	// which is what previously left resolved=0 even though these AOBs are present in client.dll).
	size_t updateCompositeCall = FindClientTextPattern("E8 ?? ?? ?? ?? 48 8D 8B ?? ?? ?? ?? 48 89 BC 24");
	size_t setAttrCall = FindClientTextPattern("E8 ?? ?? ?? ?? 66 41 0F 6E D4");
	size_t updateCompositeSet = FindClientTextPattern("40 55 53 41 57 48 8D AC 24 00 FE ?? ??");
	size_t updateSkin = FindClientTextPattern("48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0A 33 D2 48 8B CF E8 ?? ?? ?? ?? 48 8D 8F 60 19 00 00");

	s_fns.updateCompositeMaterial = (DirectUpdateCompositeMaterial_t)ResolveRelCall(updateCompositeCall);
	s_fns.updateCompositeMaterialSet = (DirectUpdateCompositeMaterialSet_t)updateCompositeSet;
	s_fns.updateSkin = (DirectUpdateSkin_t)updateSkin;
	s_fns.setAttributeValueByName = (DirectSetAttributeValueByName_t)ResolveRelCall(setAttrCall);
	s_fns.resolved = s_fns.updateCompositeMaterial && s_fns.updateCompositeMaterialSet && s_fns.updateSkin;
	return s_fns;
}

DirectCompositeResult FireDirectCompositeRefresh(
	unsigned char* weapon,
	unsigned char* itemView,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t compositeOwnerOffset,
	int32_t paintKit,
	float wear,
	int32_t seed) {
	DirectCompositeResult out = {};
	const DirectCompositeFns& fns = ResolveDirectCompositeFns();
	out.resolved = fns.resolved;
	if (!weapon || !itemView || !fns.resolved)
		return out;

	unsigned char* compositeOwner = weapon + compositeOwnerOffset;
	__try {
		if (offRestoreMaterial)
			*(bool*)(itemView + offRestoreMaterial) = true;
		if (fns.setAttributeValueByName) {
			fns.setAttributeValueByName(itemView, "set item texture prefab", (float)paintKit);
			fns.setAttributeValueByName(itemView, "set item texture wear", wear);
			fns.setAttributeValueByName(itemView, "set item texture seed", (float)seed);
		}
		fns.updateCompositeMaterial(compositeOwner, true);
		fns.updateCompositeMaterialSet(weapon, false);
		fns.updateSkin(weapon, true);
		// Force the renderable to adopt the freshly composited skin NOW (Andromeda fires this right
		// after UpdateSkin); without it the new composite is not shown until the next networked update.
		PostDataUpdate(weapon);
		out.called = true;
	} __except (1) {
		out.faulted = true;
	}
	return out;
}

SOURCESDK::CS2::Cvar_s* FindPaintkitOverrideCvar() {
	if (!SOURCESDK::CS2::g_pCVar)
		return nullptr;
	SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar("cl_paintkit_override", false);
	if (!handle.IsValid())
		return nullptr;
	return SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
}

bool ReadCvarInt(SOURCESDK::CS2::Cvar_s* cvar, int* out) {
	if (!cvar || !out)
		return false;
	__try {
		switch (cvar->m_eVarType) {
		case SOURCESDK::CS2::EConVarType_Bool:
			*out = cvar->m_Value.m_bValue ? 1 : 0;
			return true;
		case SOURCESDK::CS2::EConVarType_Int16:
			*out = (int)cvar->m_Value.m_i16Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt16:
			*out = (int)cvar->m_Value.m_u16Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Int32:
			*out = cvar->m_Value.m_i32Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt32:
			*out = (int)cvar->m_Value.m_u32Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Int64:
			*out = (int)cvar->m_Value.m_i64Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt64:
			*out = (int)cvar->m_Value.m_u64Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Float32:
			*out = (int)cvar->m_Value.m_flValue;
			return true;
		case SOURCESDK::CS2::EConVarType_Float64:
			*out = (int)cvar->m_Value.m_dbValue;
			return true;
		case SOURCESDK::CS2::EConVarType_String:
			*out = atoi(cvar->m_Value.m_szValue.Get());
			return true;
		default:
			return false;
		}
	} __except (1) {
		return false;
	}
}

bool WriteCvarInt(SOURCESDK::CS2::Cvar_s* cvar, int value) {
	if (!cvar)
		return false;
	__try {
		switch (cvar->m_eVarType) {
		case SOURCESDK::CS2::EConVarType_Bool:
			cvar->m_Value.m_bValue = value != 0;
			break;
		case SOURCESDK::CS2::EConVarType_Int16:
			cvar->m_Value.m_i16Value = (short)value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt16:
			cvar->m_Value.m_u16Value = (uint16_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Int32:
			cvar->m_Value.m_i32Value = value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt32:
			cvar->m_Value.m_u32Value = (uint32_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Int64:
			cvar->m_Value.m_i64Value = (int64_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt64:
			cvar->m_Value.m_u64Value = (uint64_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Float32:
			cvar->m_Value.m_flValue = (float)value;
			break;
		case SOURCESDK::CS2::EConVarType_Float64:
			cvar->m_Value.m_dbValue = (double)value;
			break;
		case SOURCESDK::CS2::EConVarType_String:
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%d", value);
			cvar->m_Value.m_szValue.Set(buf);
			break;
		}
		default:
			return false;
		}
		++cvar->m_iTimesChanged;
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
struct AttrWriteResult {
	bool ok = false;
	int written = 0;
	bool changed = false;
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
			if (def == 6) nv = (float)paintKit;
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
				// (docs/cosmetics-recompose-research.md, 2026-06-28) shows C_CSWeaponBase vtable slot 11
				// (the econ OnDataChanged path) only requests a "clientside_reload_custom_econ" composite
				// rebuild when this field is < 0 ("cmp dword [weapon+0x18bc],0 / jge skip"); a positive value
				// (the old ++ bump) is the skip case, which is why it never rebuilt. -1 == "reload me".
				int32_t* reloadEvent = (int32_t*)(w + offReloadEvent);
				*reloadEvent = -1;
			}
			// Clear C_EconEntity::m_bAttributesInitialized on the WEAPON (offset from w, NOT itemView)
			// to try to force an econ attribute / composite re-init. See cosmetics-recompose-research.md.
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
	CosmeticsRef().RunFrame();
}

void CosmeticOverrideSystem::Init() {
	if (m_initialized)
		return;
	// Cosmetics are demo-session state, not application preferences. Discard profiles written by
	// older builds and keep a valid, empty compatibility file so a crash/relaunch cannot resurrect
	// overrides onto a different demo or recreated player entity.
	m_store.ClearAll();
	m_store.SetEnabled(false);
	m_store.Save();
	m_initialized = true;
}

void CosmeticOverrideSystem::Shutdown() {
	ResetSessionOverrides("shutdown");
}

void CosmeticOverrideSystem::ResetSessionOverrides(const char* reason) {
	RestorePaintkitBridgeCvar();
	m_store.ClearAll();
	m_store.SetEnabled(false);
	m_agentState.clear();
	m_gloveState.clear();
	m_armed = false;
	m_pendingNudge = false;
	m_nudgePhase = 0;
	m_nudgeWasPaused = false;
	m_store.Save(); // best-effort normalization of the legacy compatibility file
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.session", "cleared runtime overrides: reason=%s enabled=0 profiles=0",
			reason ? reason : "unknown");
}

bool CosmeticOverrideSystem::OffsetsAvailable() const {
	return g_cosmeticsOffsetsOk;
}

bool CosmeticOverrideSystem::InDemoContext() const {
	return !ResolvePlayingDemoPath().empty();
}

void CosmeticOverrideSystem::SetEnabled(bool e) {
	m_store.SetEnabled(e);
	if (e) m_armed = true; // enabling counts as an explicit (re)apply this session
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
	CosmeticSlot slot = CosmeticCatalog::SlotForDefIndex(defIndex);

	CosmeticItem* item = nullptr;
	if (slot == CosmeticSlot::Knife) {
		item = &profile.knife;
	} else if (slot == CosmeticSlot::Secondary) {
		item = &profile.secondary;
	} else {
		// Primary or None (defIndex 0 = "keep the held weapon's own def") -> primary slot.
		item = &profile.primary;
	}

	item->set = true;
	item->defIndex = defIndex;
	item->paintKit = paintKit;
	item->wear = wear;
	item->seed = seed;
	item->statTrak = statTrak;
	// statTrak is left as-supplied for weapons (unlike SetKnife/SetGloves which force -1).

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
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
	MaybeFireTickNudge();
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

	// SEEK-SAFETY GATE: a demo SEEK/scrub mass-destroys and rebuilds the entity list while the engine
	// replays packets. Running our apply -- especially the model-swap engine calls (SetModel /
	// UpdateSubclass / GetActiveWeaponHandle / PostDataUpdate) -- on those half-rebuilt entities races
	// the reconstruction and crashes (confirmed: a backward scrub with the knife swap active crashed
	// inside the demo packet processing). Detect a seek as a large demo-tick jump since the previous
	// applied frame (normal playback advances ~1 tick/frame; a seek jumps hundreds-to-thousands) and
	// SKIP the apply for a short settle window so the list finishes rebuilding before we touch it. The
	// override is XUID-keyed and re-asserts every frame, so a brief skip is invisible once settled.
	{
		int curTick = 0;
		if (g_MirvTime.GetCurrentDemoTick(curTick)) {
			if (m_lastApplyTick >= 0) {
				int d = curTick - m_lastApplyTick;
				if (d < 0) d = -d;
				if (d > 64)
					m_seekSettleFrames = 16; // re-armed on every jump, so it covers a multi-step seek
			}
			m_lastApplyTick = curTick;
		}
	}
	if (m_seekSettleFrames > 0) {
		--m_seekSettleFrames;
		UpdatePaintkitBridge();
		return;
	}

	++m_lastStats.frame;
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
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
	ApplyMatchedWeapons(/*forceStale=*/false, /*fireRebuildCall=*/m_recompose, /*fireDirectComposite=*/m_autoComposite);
	// Gloves + agent (pawn-level model swap), keyed by owner SteamID like the weapon loop.
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
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
	m_lastStats.directCompositeCalls = 0;
	m_lastStats.directCompositeResolved = 0;
	m_lastStats.directCompositeFaulted = 0;
	return ApplyMatchedWeapons(/*forceStale=*/true, /*fireRebuildCall=*/m_recompose, /*fireDirectComposite=*/false);
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
	m_lastStats.directCompositeCalls = 0;
	m_lastStats.directCompositeResolved = 0;
	m_lastStats.directCompositeFaulted = 0;
	return ApplyMatchedWeapons(/*forceStale=*/true, /*fireRebuildCall=*/false, /*fireDirectComposite=*/true);
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

void CosmeticOverrideSystem::SetPaintkitBridge(bool e) {
	if (m_paintkitBridge == e) {
		if (m_paintkitBridge)
			UpdatePaintkitBridge();
		return;
	}
	m_paintkitBridge = e;
	if (!m_paintkitBridge)
		RestorePaintkitBridgeCvar();
	else
		UpdatePaintkitBridge();
}

bool CosmeticOverrideSystem::SetPaintkitBridgeCvar(int value) {
	SOURCESDK::CS2::Cvar_s* cvar = FindPaintkitOverrideCvar();
	m_paintkitBridgeCvarFound = cvar != nullptr;
	if (!cvar)
		return false;

	int current = 0;
	if (!ReadCvarInt(cvar, &current))
		return false;

	if (!m_paintkitBridgeHaveOriginal) {
		m_paintkitBridgeOriginalValue = current;
		m_paintkitBridgeHaveOriginal = true;
	}

	if (current != value && !WriteCvarInt(cvar, value))
		return false;

	m_paintkitBridgeLastValue = value;
	return true;
}

void CosmeticOverrideSystem::RestorePaintkitBridgeCvar() {
	if (!m_paintkitBridgeHaveOriginal) {
		m_paintkitBridgeLastValue = 0;
		return;
	}

	SOURCESDK::CS2::Cvar_s* cvar = FindPaintkitOverrideCvar();
	m_paintkitBridgeCvarFound = cvar != nullptr;
	if (cvar)
		WriteCvarInt(cvar, m_paintkitBridgeOriginalValue);
	m_paintkitBridgeHaveOriginal = false;
	m_paintkitBridgeLastValue = 0;
	m_lastStats.paintkitBridgeValue = 0;
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
		CosmeticSlot slot = CosmeticCatalog::SlotForDefIndex(econ.liveDef);
		const CosmeticItem* cand = (slot == CosmeticSlot::Secondary) ? &prof->secondary
			: (slot == CosmeticSlot::Primary) ? &prof->primary
			: nullptr;
		if (cand && cand->set && (cand->defIndex == 0 || cand->defIndex == econ.liveDef))
			item = cand;
	}

	if (!item || item->paintKit <= 0)
		return 0;
	return item->paintKit;
}

void CosmeticOverrideSystem::RequestApplyNudge() {
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
			// Re-assert the override onto whatever entities the play-out left us with.
			m_agentState.clear();
			m_gloveState.clear();
			m_nudgePhase = 0;
			++m_totalNudges;
		}
		return;
	}

	if (!m_pendingNudge)
		return;
	if (!m_tickNudge) { m_pendingNudge = false; return; } // disabled: drop the request
	// Debounce: wait for a short quiet period after the last change so rapid re-picks coalesce.
	const uint64_t kDebounceFrames = 8;
	if (m_frameCounter - m_nudgeRequestFrame < kDebounceFrames)
		return;
	if (!InDemoContext())
		return; // keep pending; fire once a demo is actually playing
	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick))
		return; // tick not readable yet (mid-seek) -- retry next frame
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
		return;
	}

	// Paused: briefly resume so the engine renders real frames (the only thing that makes a body
	// model / glove / mesh swap re-evaluate), then re-pause after ~m_tickNudgeTicks ticks -- exactly
	// "let the game play ~10 ticks" done automatically. Clear the per-pawn apply gates so the
	// override re-fires onto any entities the play-out recreates.
	m_agentState.clear();
	m_gloveState.clear();
	m_nudgeWasPaused = true;
	m_nudgeStartTick = tick;
	m_nudgePlayStartFrame = m_frameCounter;
	m_nudgePhase = 1;
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.nudge", "START resume plannedTicks=%d", m_tickNudgeTicks);
}

void CosmeticOverrideSystem::UpdatePaintkitBridge() {
	if (!m_paintkitBridge) {
		m_lastStats.paintkitBridgeValue = 0;
		return;
	}

	const int paintKit = ResolveSpectatedPaintkitOverride();
	if (paintKit <= 0) {
		RestorePaintkitBridgeCvar();
		return;
	}

	if (SetPaintkitBridgeCvar(paintKit))
		m_lastStats.paintkitBridgeValue = paintKit;
	else
		m_lastStats.paintkitBridgeValue = 0;
}

int CosmeticOverrideSystem::ApplyMatchedWeapons(bool forceStale, bool fireRebuildCall, bool fireDirectComposite) {
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
				if (m_knifeModelSwap && item->defIndex > 0 && item->defIndex != liveDef)
					knifeDefOverride = item->defIndex;
			}
		} else {
			CosmeticSlot slot = CosmeticCatalog::SlotForDefIndex(liveDef);
			CosmeticItem* cand = (slot == CosmeticSlot::Secondary) ? &prof->secondary
				: (slot == CosmeticSlot::Primary) ? &prof->primary
				: nullptr;
			// A paint kit is weapon-specific: only apply when the override is "any weapon in this
			// slot" (defIndex 0) or it explicitly matches the currently-held weapon def.
			if (cand && cand->set && (cand->defIndex == 0 || cand->defIndex == liveDef))
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
			m_fallbackId,
			m_rebuildAuto,
			forceStale,
			m_rebuildFlags);

		if (result.patched)
			++m_lastStats.entitiesPatched;
		if (result.reverted)
			++m_lastStats.entitiesReverted;

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
		unsigned char* ownerPawn = nullptr;
		if ((m_modelSwap || fireDirectComposite) && (result.needComposite || attr.changed || forceStale)) {
			unsigned char* pawn = PawnForSteamId(xuid);
			if (pawn) {
				SOURCESDK::CS2::CBaseHandle aw = ((CEntityInstance*)pawn)->GetActiveWeaponHandle();
				if (aw.IsValid() && aw.GetEntryIndex() == i)
					ownerPawn = pawn;
			}
		}
		if (m_modelSwap && (result.needComposite || attr.changed || forceStale)) {
			bool isKnife = CosmeticCatalog::IsKnifeDef(liveDef);
			// ownerPawn is non-null ONLY when THIS weapon is the owner's ACTIVE (deployed) weapon (see
			// above). Gate the knife model/subclass swap on it: a HOLSTERED knife is never rendered, and
			// re-running SetModel/UpdateSubclass/SetMeshGroupMask on a holstered knife entity that the
			// engine keeps recreating during live playback (round restart, weapon switch) is the
			// long-standing "crash-prone on weapon switch" race. Swapping only the active knife removes
			// that window while still showing the swap whenever the knife is actually held.
			if (isKnife && m_knifeModelSwap && knifeDefOverride > 0 && ownerPawn) {
				uint64_t mask = ResolveMeshMask(item->paintKit, /*knife=*/true,
					m_meshLegacyMode, m_maskModern, m_maskLegacy);
				if (mask == 0) mask = m_maskModern; // a knife model always needs a mesh group
				if (ApplyKnifeModelSwap(w, itemView, ownerPawn, knifeDefOverride, mask))
					++m_lastStats.knifeModelsApplied;
			} else if (!isKnife && item->paintKit > 0) {
				uint64_t mask = ResolveMeshMask(item->paintKit, /*knife=*/false,
					m_meshLegacyMode, m_maskModern, m_maskLegacy);
				if (mask != 0) {
					ApplyWeaponMeshMask(w, mask, ownerPawn);
					++m_lastStats.weaponMeshFixed;
				}
			}
		}

		if (fireDirectComposite && (result.needComposite || attr.changed || forceStale)) {
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
			if (direct.called)
				++m_lastStats.directCompositeCalls;
			if (direct.faulted)
				m_lastStats.directCompositeFaulted = 1;
			if (direct.called && ownerPawn)
				RefreshWeaponViewmodel(ownerPawn);
		}

		// Per-matched-weapon diagnostic (deduped per unique payload). The decisive field is
		// attrWritten: a demo weapon's REAL skin lives in m_NetworkedDynamicAttributes (def 6 paint /
		// 7 seed / 8 wear) -- attrWritten=0 means that attribute is NOT present on this item, so the
		// paint cannot be written there (the skin will not render via the networked-attr path no matter
		// what). attrOk=0 means the attribute vector itself could not be read. needComposite reflects
		// whether a value differed this frame (drives the re-composite / model-swap fire).
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.weapon",
				"xuid=%llu liveDef=%d isKnife=%d knifeSwapDef=%d paint=%d wear=%.3f seed=%d stat=%d "
				"attrOk=%d attrWritten=%d attrChanged=%d patched=%d needComposite=%d",
				(unsigned long long)xuid, liveDef, CosmeticCatalog::IsKnifeDef(liveDef) ? 1 : 0,
				knifeDefOverride, item->paintKit, item->wear, item->seed, item->statTrak,
				attr.ok ? 1 : 0, attr.written, attr.changed ? 1 : 0,
				result.patched ? 1 : 0, result.needComposite ? 1 : 0);
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
	if (!m_modelSwap)
		return 0;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
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
			uint32_t wearBits = 0;
			std::memcpy(&wearBits, &prof->gloves.wear, sizeof(wearBits));
			uint64_t sig = 1469598103934665603ULL;
			const uint32_t sigParts[] = { (uint32_t)prof->gloves.defIndex,
				(uint32_t)prof->gloves.paintKit, (uint32_t)prof->gloves.seed, wearBits };
			for (uint32_t part : sigParts) { sig ^= part; sig *= 1099511628211ULL; }
			float spawn = SafeReadEntityFloat(pawn, o.C_CSPlayerPawn.m_flLastSpawnTimeIndex, -1.0f);

			GloveApplyState& st = m_gloveState[steamId];
			if (st.sig != sig || st.lastSpawn != spawn || st.pawn != (uintptr_t)pawn) {
				st.frames = 4; // apply over a few frames (gloves rebuild across frames)
				st.sig = sig;
				st.lastSpawn = spawn;
				st.pawn = (uintptr_t)pawn;
			}
			if (st.frames > 0) {
				if (ApplyGloveModel(pawn, prof->gloves.defIndex, prof->gloves.paintKit,
						prof->gloves.wear, prof->gloves.seed, (uint32_t)steamId))
					++m_lastStats.glovesApplied;
				--st.frames;
			}
		}
	}

	m_lastStats.modelSwapResolved = ResolveModelSwapFns().CoreOk() ? 1 : 0;
	return m_lastStats.pawnsScanned;
}

} // namespace Filmmaker
