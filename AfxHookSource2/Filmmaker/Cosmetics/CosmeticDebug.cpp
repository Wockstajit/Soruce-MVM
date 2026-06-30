// Diagnostics for the SteamID-keyed cosmetic override system: "mirv_filmmaker cosmetics status"
// and the spectated-player econ dump used while wiring up new overrides. Read-only: never writes
// to any entity. Entity field reads are SEH-guarded (POD-only bodies inside __try/__except) the
// same way CosmeticOverrideSystem.cpp's ApplyCosmeticWrite / MirvCosmetics::DebugWeapon are, since
// a stale/misclassified entity pointer here would otherwise access-violate the game process.

#include "CosmeticOverrideSystem.h"
#include "CosmeticCatalog.h"
#include "CosmeticModelSwap.h"
#include "CosmeticDebugLog.h" // MvmDebugLog_Active / MvmDebugLog_Linef (uiclick before/after lines)
#include "CosmeticGloveLabels.h"

#include "../Platform/TextEncoding.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk
#include "../../MirvTime.h"            // g_MirvTime.GetCurrentDemoTick (seek detection for the live skin log)

#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include "../../../shared/AfxConsole.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Same bounds-checked resolve used by CosmeticOverrideSystem.cpp / MirvCosmetics.cpp.
CEntityInstance* EntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

// POD result of the SEH-guarded weapon-econ read -- no constructors, safe to populate inside
// __try/__except (which may not contain C++ objects that need unwinding).
struct WeaponDebugInfo {
	bool ok = false;
	int weaponIndex = -1;
	char className[64] = {};
	uint64_t ownerXuid = 0;
	int itemDefIndex = 0;
	int32_t itemIdHigh = 0;
	int32_t fallbackPaintKit = 0;
	float fallbackWear = 0.0f;
};

// Resolves the spectated pawn's active weapon and SEH-guarded-reads its econ fields. Mirrors
// MirvCosmetics::Cosmetics_DebugWeapon, generalized to also report the original-owner XUID (which
// is what the apply loop keys profiles off of) and the weapon's class name.
bool ReadWeaponDebugInfo(int pawnIndex, WeaponDebugInfo* out) {
	if (!out || !g_cosmeticsOffsetsOk)
		return false;

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return false;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid())
		return false;

	CEntityInstance* weapon = EntFromIndex(wh.GetEntryIndex());
	if (!weapon)
		return false;

	// GetClassName() returns a C++ const char*; copy it into the POD struct BEFORE entering the
	// __try so the guarded block only ever touches raw pointers/primitives.
	const char* cls = weapon->GetClassName();
	out->weaponIndex = wh.GetEntryIndex();
	out->className[0] = '\0';
	if (cls) {
		size_t i = 0;
		for (; cls[i] != '\0' && i + 1 < sizeof(out->className); ++i)
			out->className[i] = cls[i];
		out->className[i] = '\0';
	}

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;

	__try {
		unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
		uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
		out->ownerXuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		out->itemDefIndex = (int)*(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
		out->itemIdHigh = *(int32_t*)(itemView + o.C_EconItemView.m_iItemIDHigh);
		out->fallbackPaintKit = *(int32_t*)(w + o.C_EconEntity.m_nFallbackPaintKit);
		out->fallbackWear = *(float*)(w + o.C_EconEntity.m_flFallbackWear);
		out->ok = true;
	} __except (1) {
		// A bad offset / misclassified entity access-violates HERE; swallow it instead of taking
		// down the game (1 == EXCEPTION_EXECUTE_HANDLER; the literal avoids <windows.h> macro
		// clashes, matching MirvCosmetics::SafeVCall / CosmeticOverrideSystem::ApplyCosmeticWrite).
		out->ok = false;
	}
	return out->ok;
}

// POD dump of one CAttributeList's contents (defIndex -> value pairs). Filled inside a
// __try/__except, so no C++ objects -- a fixed array, no std::vector.
struct AttrListDump {
	bool ok = false;
	bool resolved = false; // the list offset itself was non-zero (field exists on this build)
	int count = 0;
	struct { int def; float val; } items[24] = {};
};

// Walks the embedded attribute vector at (itemView + listOff + C_AttributeList.m_Attributes) and
// copies up to 24 {defIndex,value} pairs out. Accepts both observed vector layouts (count+ptr and
// ptr+count), the same tolerance as CameraEditorHud.cpp::TryMakeAttributeVector. SEH-guarded: a bad
// pointer/offset faults HERE and is swallowed. listOff==0 means "field not present on this build".
void ReadAttrList(unsigned char* itemView, ptrdiff_t listOff, AttrListDump* out) {
	*out = AttrListDump{};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (listOff == 0 || o.C_AttributeList.m_Attributes == 0
		|| o.CEconItemAttribute.m_iAttributeDefinitionIndex == 0 || o.CEconItemAttribute.m_flValue == 0)
		return;
	out->resolved = true;

	int stride = (int)o.CEconItemAttribute.m_size;
	int minStride = (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float);
	if (stride < minStride) stride = minStride;

	unsigned char* vectorField = itemView + listOff + o.C_AttributeList.m_Attributes;
	__try {
		int count = *(int*)vectorField;
		unsigned char* data = *(unsigned char**)(vectorField + 8);
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			data = *(unsigned char**)vectorField;
			count = *(int*)(vectorField + 16);
		}
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			out->ok = true; // a valid but EMPTY list (count 0) -- distinguish from a fault
			out->count = 0;
			return;
		}
		int n = count > 24 ? 24 : count;
		for (int i = 0; i < n; ++i) {
			unsigned char* attr = data + (ptrdiff_t)i * stride;
			out->items[i].def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
			out->items[i].val = *(float*)(attr + o.CEconItemAttribute.m_flValue);
		}
		out->count = n;
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

void PrintAttrList(const char* cmd, const char* label, ptrdiff_t listOff, const AttrListDump& d) {
	if (!d.resolved) {
		advancedfx::Message("    %s: (offset unresolved -- field not on this build)\n", label);
		return;
	}
	if (!d.ok) {
		advancedfx::Message("    %s @+0x%zx: (read faulted)\n", label, (size_t)listOff);
		return;
	}
	if (d.count == 0) {
		advancedfx::Message("    %s @+0x%zx: (empty)\n", label, (size_t)listOff);
		return;
	}
	advancedfx::Message("    %s @+0x%zx: %d attr(s)\n", label, (size_t)listOff, d.count);
	for (int i = 0; i < d.count; ++i)
		advancedfx::Message("      def=%d value=%.4f\n", d.items[i].def, d.items[i].val);
}

// Resolves the spectated pawn's active weapon and dumps BOTH attribute lists (local + networked)
// so we can see where a networked/demo player's paint kit actually lives. Read-only.
void DumpSpectatedWeaponAttributes(int pawnIndex, const char* cmd) {
	if (!g_cosmeticsOffsetsOk)
		return;
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return;
	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid())
		return;
	CEntityInstance* weapon = EntFromIndex(wh.GetEntryIndex());
	if (!weapon)
		return;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	AttrListDump local, networked;
	ReadAttrList(itemView, o.C_EconItemView.m_AttributeList, &local);
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &networked);

	advancedfx::Message("  attribute lists (paintKit=def6, seed=def7, wear=def8, stattrak=def81):\n");
	PrintAttrList(cmd, "m_AttributeList", o.C_EconItemView.m_AttributeList, local);
	PrintAttrList(cmd, "m_NetworkedDynamicAttributes", o.C_EconItemView.m_NetworkedDynamicAttributes, networked);
}

// POD result of the read-only agent/player-model name read.
struct ModelNameResult {
	bool ok = false;
	char name[256] = {};
};

// Walks the read-only model-state chain on a player pawn and copies its model path
// (CModelState::m_ModelName) into a POD buffer. SEH-guarded: a bad pointer/offset faults here and is
// swallowed. Chain: pawn + m_CBodyComponent(ptr deref) + m_skeletonInstance + m_modelState + m_ModelName.
void ReadPawnModelName(int pawnIndex, ModelNameResult* out) {
	*out = ModelNameResult{};
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)pawn;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp > 0x10000) {
			unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
			const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
			if ((uintptr_t)name > 0x10000) {
				size_t i = 0;
				for (; name[i] && i + 1 < sizeof(out->name); ++i)
					out->name[i] = name[i];
				out->name[i] = '\0';
				out->ok = (i > 0);
			}
		}
	} __except (1) {
		out->ok = false;
	}
}

// Generalized model-name read: same CModelState::m_ModelName chain as ReadPawnModelName, but for ANY
// entity (a weapon entity is a C_BaseAnimGraph and carries the same body-component/skeleton chain),
// so we can dump the weapon's own world-model path. SEH-guarded POD body.
void ReadEntityModelName(CEntityInstance* ent, ModelNameResult* out) {
	*out = ModelNameResult{};
	if (!ent)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp > 0x10000) {
			unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
			const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
			if ((uintptr_t)name > 0x10000) {
				size_t i = 0;
				for (; name[i] && i + 1 < sizeof(out->name); ++i)
					out->name[i] = name[i];
				out->name[i] = '\0';
				out->ok = (i > 0);
			}
		}
	} __except (1) {
		out->ok = false;
	}
}

// POD dump of one weapon entity's full visual/econ cache state. Every render-relevant field the
// visual-rebuild research cares about, read in a single SEH-guarded pass. `have*` flags distinguish
// "offset unresolved on this build" (false) from "read 0" (true, value 0).
struct WeaponVisualDiag {
	bool ok = false;
	uint64_t ownerXuid = 0;
	int defIndex = 0;
	int32_t itemIdHigh = 0;
	uint32_t itemIdLow = 0;
	uint32_t accountId = 0;
	int32_t fbPaint = 0;
	float fbWear = 0.0f;
	int32_t fbSeed = 0;
	int32_t fbStat = 0;
	bool haveVisualsData = false;  unsigned char visualsDataSet = 0;
	bool haveClearUgc = false;     unsigned char clearUgc = 0;
	bool haveReloadEvent = false;  int32_t reloadEvent = 0;
	bool haveAttrInit = false;     unsigned char attrInit = 0;
};

// Reads everything in WeaponVisualDiag off a weapon entity. Caller resolves itemView; this only
// touches POD pointers inside __try. Optional offsets (the C_CSWeaponBase visuals flags) are gated by
// != 0 so a missing field reports have*=false instead of reading a bogus address.
void ReadWeaponVisualDiag(unsigned char* w, unsigned char* itemView, WeaponVisualDiag* out) {
	*out = WeaponVisualDiag{};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	__try {
		uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
		out->ownerXuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		out->defIndex = (int)*(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
		out->itemIdHigh = *(int32_t*)(itemView + o.C_EconItemView.m_iItemIDHigh);
		if (o.C_EconItemView.m_iItemIDLow) out->itemIdLow = *(uint32_t*)(itemView + o.C_EconItemView.m_iItemIDLow);
		if (o.C_EconItemView.m_iAccountID) out->accountId = *(uint32_t*)(itemView + o.C_EconItemView.m_iAccountID);
		out->fbPaint = *(int32_t*)(w + o.C_EconEntity.m_nFallbackPaintKit);
		out->fbWear = *(float*)(w + o.C_EconEntity.m_flFallbackWear);
		out->fbSeed = *(int32_t*)(w + o.C_EconEntity.m_nFallbackSeed);
		out->fbStat = *(int32_t*)(w + o.C_EconEntity.m_nFallbackStatTrak);
		if (o.C_CSWeaponBase.m_bVisualsDataSet) {
			out->visualsDataSet = *(unsigned char*)(w + o.C_CSWeaponBase.m_bVisualsDataSet);
			out->haveVisualsData = true;
		}
		if (o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC) {
			out->clearUgc = *(unsigned char*)(w + o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC);
			out->haveClearUgc = true;
		}
		if (o.C_CSWeaponBase.m_nCustomEconReloadEventId) {
			out->reloadEvent = *(int32_t*)(w + o.C_CSWeaponBase.m_nCustomEconReloadEventId);
			out->haveReloadEvent = true;
		}
		if (o.C_EconEntity.m_bAttributesInitialized) {
			out->attrInit = *(unsigned char*)(w + o.C_EconEntity.m_bAttributesInitialized);
			out->haveAttrInit = true;
		}
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

// Extracts a specific attribute def's value out of an already-read AttrListDump. Returns false if the
// def is not present (e.g. StatTrak on a non-ST gun). The list reader already SEH-guarded the read.
bool AttrValueForDef(const AttrListDump& d, int def, float* outVal) {
	if (!d.ok) return false;
	for (int i = 0; i < d.count; ++i) {
		if (d.items[i].def == def) { if (outVal) *outVal = d.items[i].val; return true; }
	}
	return false;
}

const char* SlotLabel(CosmeticSlot slot) {
	switch (slot) {
	case CosmeticSlot::Primary: return "primary";
	case CosmeticSlot::Secondary: return "secondary";
	case CosmeticSlot::Knife: return "knife";
	case CosmeticSlot::Gloves: return "gloves";
	case CosmeticSlot::Agent: return "agent";
	default: return "none";
	}
}

void PrintItemLine(const char* label, const CosmeticItem& item) {
	if (!item.set)
		return;
	advancedfx::Message("    %s def=%d paint=%d wear=%.4f seed=%d st=%d\n",
		label, item.defIndex, item.paintKit, item.wear, item.seed, item.statTrak);
}

// ---- LIVE per-player skin-state readers (for the mvm_debug "skin.live" log) -------------------------

// POD live skin of a single weapon ENTITY. paint/seed/wear are read from where a demo weapon's REAL
// skin lives -- the networked dynamic attributes (def 6/7/8/81) -- falling back to the local attr list,
// then the C_EconEntity fallback fields. attrSrc names which source supplied the paint so the log shows
// whether the value is the demo's real skin, an override we wrote, or nothing.
struct WeaponSkinLive {
	bool ok = false;
	uint64_t ownerXuid = 0;
	int def = 0;
	int paint = -1;
	int seed = -1;
	float wear = -1.0f;
	int stat = -1;
	char attrSrc[16] = "none";
};

// No __try in this body (it only calls the SEH-guarded POD helpers), so std::string-free + safe to
// build with C++ temporaries. ent must be a weapon-like econ entity.
void ReadWeaponSkinLive(CEntityInstance* ent, WeaponSkinLive* out) {
	*out = WeaponSkinLive{};
	if (!ent || !g_cosmeticsOffsetsOk)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)ent;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	WeaponVisualDiag d;
	ReadWeaponVisualDiag(w, itemView, &d);
	if (!d.ok)
		return;
	out->ownerXuid = d.ownerXuid;
	out->def = d.defIndex;

	AttrListDump net, loc;
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
	ReadAttrList(itemView, o.C_EconItemView.m_AttributeList, &loc);
	float p = 0, s = 0, wv = 0, st = 0;
	if (AttrValueForDef(net, 6, &p)) {
		out->paint = (int)p;
		std::snprintf(out->attrSrc, sizeof(out->attrSrc), "networked");
		if (AttrValueForDef(net, 7, &s)) out->seed = (int)s;
		if (AttrValueForDef(net, 8, &wv)) out->wear = wv;
		if (AttrValueForDef(net, 81, &st)) out->stat = (int)st;
	} else if (AttrValueForDef(loc, 6, &p)) {
		out->paint = (int)p;
		std::snprintf(out->attrSrc, sizeof(out->attrSrc), "local");
		if (AttrValueForDef(loc, 7, &s)) out->seed = (int)s;
		if (AttrValueForDef(loc, 8, &wv)) out->wear = wv;
		if (AttrValueForDef(loc, 81, &st)) out->stat = (int)st;
	} else {
		// No paint attribute present -> the C_EconEntity fallback fields (only meaningful in fallback-id
		// mode). 0 fallback paint on a networked demo item just means "vanilla / not skinned".
		out->paint = d.fbPaint;
		out->seed = d.fbSeed;
		out->wear = d.fbWear;
		out->stat = d.fbStat;
		std::snprintf(out->attrSrc, sizeof(out->attrSrc), d.fbPaint > 0 ? "fallback" : "none");
	}
	out->ok = true;
}

// POD live identity of the pawn's embedded m_EconGloves item view (def + cache flags). Paint/seed/wear
// are read separately via ReadAttrList on the same glove item view in the logger.
struct GloveLiveInfo {
	bool ok = false;
	int def = 0;
	bool haveQuality = false; int32_t quality = 0;
	bool haveItemId = false;  int32_t itemIdHigh = 0;
	bool haveInit = false;    unsigned char initialized = 0;
};

void ReadGloveLiveInfo(unsigned char* pawn, GloveLiveInfo* out) {
	*out = GloveLiveInfo{};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!pawn || o.C_CSPlayerPawn.m_EconGloves == 0 || o.C_EconItemView.m_iItemDefinitionIndex == 0)
		return;
	unsigned char* glove = pawn + o.C_CSPlayerPawn.m_EconGloves;
	__try {
		out->def = (int)*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		if (o.C_EconItemView.m_iEntityQuality) { out->quality = *(int32_t*)(glove + o.C_EconItemView.m_iEntityQuality); out->haveQuality = true; }
		if (o.C_EconItemView.m_iItemIDHigh)    { out->itemIdHigh = *(int32_t*)(glove + o.C_EconItemView.m_iItemIDHigh); out->haveItemId = true; }
		if (o.C_EconItemView.m_bInitialized)   { out->initialized = *(unsigned char*)(glove + o.C_EconItemView.m_bInitialized); out->haveInit = true; }
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

struct GloveSnapshot {
	bool ok = false;
	int def = 0;
	int paint = 0;
	float wear = 0.0f;
	int seed = 0;
	uint8_t team = 0;
};

bool ReadGloveSnapshotForSteamId(uint64_t steamId, GloveSnapshot* out) {
	*out = GloveSnapshot{};
	if (steamId == 0)
		return false;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!g_cosmeticsOffsetsOk || o.C_CSPlayerPawn.m_EconGloves == 0)
		return false;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ctrl = EntFromIndex(i);
		if (!ctrl || !ctrl->IsPlayerController() || ctrl->GetSteamId() != steamId)
			continue;
		SOURCESDK::CS2::CBaseHandle ph = ctrl->GetPlayerPawnHandle();
		CEntityInstance* pawnEnt = ph.IsValid() ? EntFromIndex(ph.GetEntryIndex()) : nullptr;
		if (!pawnEnt || !pawnEnt->IsPlayerPawn())
			return false;
		unsigned char* pawn = (unsigned char*)pawnEnt;
		if (o.C_BaseEntity.m_iTeamNum) {
			__try { out->team = *(uint8_t*)(pawn + o.C_BaseEntity.m_iTeamNum); }
			__except (1) { out->team = 0; }
		}
		GloveLiveInfo g;
		ReadGloveLiveInfo(pawn, &g);
		if (!g.ok)
			return false;
		out->def = g.def;
		unsigned char* glove = pawn + o.C_CSPlayerPawn.m_EconGloves;
		AttrListDump net, loc;
		ReadAttrList(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
		ReadAttrList(glove, o.C_EconItemView.m_AttributeList, &loc);
		float p = 0.0f, s = 0.0f, wv = 0.0f;
		if (AttrValueForDef(net, 6, &p)) {
			out->paint = (int)p;
			if (AttrValueForDef(net, 7, &s)) out->seed = (int)s;
			if (AttrValueForDef(net, 8, &wv)) out->wear = wv;
		} else if (AttrValueForDef(loc, 6, &p)) {
			out->paint = (int)p;
			if (AttrValueForDef(loc, 7, &s)) out->seed = (int)s;
			if (AttrValueForDef(loc, 8, &wv)) out->wear = wv;
		}
		out->ok = true;
		return true;
	}
	return false;
}

std::string GloveLabelForSteamId(uint64_t steamId, const CosmeticProfile* prof, bool preferProfileOverride) {
	GloveSnapshot live;
	ReadGloveSnapshotForSteamId(steamId, &live);
	if (preferProfileOverride && prof && prof->gloves.set && prof->gloves.defIndex > 0)
		return CosmeticGloveLabels::FormatGloveSkinLabel(prof->gloves.defIndex, prof->gloves.paintKit);
	if (live.ok)
		return CosmeticGloveLabels::FormatGloveSkinLabel(live.def, live.paint);
	if (live.team != 0)
		return CosmeticGloveLabels::TeamDefaultGloveLabel(live.team);
	return "(unknown gloves)";
}

std::string s_pendingGloveUiLabel;

void StorePendingGloveLabelInternal(const char* uilogText) {
	if (!uilogText || !*uilogText)
		return;
	if (std::strncmp(uilogText, "[gloves]", 8) != 0)
		return;
	const char* p = uilogText + 8;
	while (*p == ' ')
		++p;
	const char* defParen = std::strstr(p, " (def ");
	if (defParen)
		s_pendingGloveUiLabel.assign(p, defParen - p);
	else
		s_pendingGloveUiLabel = p;
}

std::string TakePendingGloveLabelInternal() {
	std::string out = s_pendingGloveUiLabel;
	s_pendingGloveUiLabel.clear();
	return out;
}

} // namespace

void Cosmetics_PrintStatus(const char* cmd) {
	CosmeticOverrideSystem& sys = CosmeticsRef();
	const CosmeticFrameStats& stats = sys.LastFrameStats();

	advancedfx::Message("%s cosmetics: enabled=%d armed=%d offsetsResolved=%d demoContext=%d debug=%d\n",
		cmd, sys.Enabled() ? 1 : 0, sys.Armed() ? 1 : 0, sys.OffsetsAvailable() ? 1 : 0, sys.InDemoContext() ? 1 : 0, sys.Debug() ? 1 : 0);

	const std::wstring wpath = sys.Store().FilePath();
	const std::string upath = WideToUtf8(wpath);
	advancedfx::Message("  profiles=%zu file='%s'\n", sys.Store().All().size(), upath.c_str());

	advancedfx::Message("  lastFrame: scanned=%d matched=%d patched=%d reverted=%d frame=%llu\n",
		stats.entitiesScanned, stats.entitiesMatched, stats.entitiesPatched, stats.entitiesReverted,
		(unsigned long long)stats.frame);
	advancedfx::Message("  attrs: listsRead=%d valuesWritten=%d valuesChanged=%d empty=%d namedSetter=%d\n",
		stats.attrListsRead, stats.attrValuesWritten, stats.attrValuesChanged, stats.attrListsEmpty,
		stats.namedSetterApplied);

	int vtComp = 0, vtSec = 0;
	sys.GetVtIdx(&vtComp, &vtSec);
	advancedfx::Message("  recompose=%d faulted=%d vtComp=%d vtSec=%d vtArg=%d fallbackId=%d\n",
		sys.Recompose() ? 1 : 0, sys.RecomposeFaulted() ? 1 : 0, vtComp, vtSec, sys.VtArg(), sys.FallbackId() ? 1 : 0);
	advancedfx::Message("  paintkitbridge=%d cvarFound=%d value=%d forced=%d (global deploy-time cl_paintkit_override)\n",
		sys.PaintkitBridge() ? 1 : 0, sys.PaintkitBridgeCvarFound() ? 1 : 0,
		sys.PaintkitBridgeLastValue(), sys.PaintkitBridgeForcedValue());
	advancedfx::Message("  directComposite: resolved=%d calls=%d faulted=%d ownerOffset=0x%llx\n",
		stats.directCompositeResolved, stats.directCompositeCalls, stats.directCompositeFaulted,
		(unsigned long long)sys.CompositeOwnerOffset());
	const char* meshModeStr = sys.MeshLegacyMode() == -2 ? "auto" : (sys.MeshLegacyMode() == -1 ? "modern" : "legacy");
	advancedfx::Message("  modelswap=%d knifeType=%d resolved=%d  mesh=%s(modern=%llu legacy=%llu)\n",
		sys.ModelSwap() ? 1 : 0, sys.KnifeModelSwap() ? 1 : 0,
		sys.ModelSwapResolved() ? 1 : 0, meshModeStr,
		(unsigned long long)sys.MaskModern(), (unsigned long long)sys.MaskLegacy());
	advancedfx::Message("  lastFrame modelSwap: knife=%d weaponMesh=%d pawns=%d gloves=%d agents=%d\n",
		stats.knifeModelsApplied, stats.weaponMeshFixed, stats.pawnsScanned, stats.glovesApplied, stats.agentsApplied);
	advancedfx::Message("  ticknudge=%d ticks=%d totalNudges=%llu (auto play-out so body swaps re-render)\n",
		sys.TickNudge() ? 1 : 0, sys.TickNudgeTicks(), (unsigned long long)sys.TotalNudges());
	advancedfx::Message("  totalApplied: knife=%llu weaponMesh=%llu gloves=%llu agents=%llu (cumulative, proof of execution)\n",
		(unsigned long long)sys.TotalKnifeApplied(), (unsigned long long)sys.TotalWeaponMeshApplied(),
		(unsigned long long)sys.TotalGlovesApplied(), (unsigned long long)sys.TotalAgentsApplied());
	const ModelSwapResolveStatus& ms = ResolveModelSwapFns();
	advancedfx::Message("  modelSwapFns: setModel=%d meshMask=%d subclass=%d bodyGroup=%d bodyGroupChoice=%d staticData=%d econSchema=%d\n",
		ms.setModel ? 1 : 0, ms.setMeshGroupMask ? 1 : 0, ms.updateSubclass ? 1 : 0, ms.setBodyGroup ? 1 : 0,
		ms.updateBodyGroupChoice ? 1 : 0, ms.getStaticData ? 1 : 0, ms.econItemSystem ? 1 : 0);

	if (sys.Store().All().empty())
		advancedfx::Message("  (no profiles)\n");

	for (const auto& [steamId, profile] : sys.Store().All()) {
		advancedfx::Message("  %llu '%s':\n", (unsigned long long)steamId, profile.name.c_str());
		// Per-weapon overrides, one line each (label by def index so each gun is distinguishable).
		for (const auto& [def, item] : profile.weapons) {
			if (!item.set)
				continue;
			char label[32];
			std::snprintf(label, sizeof(label), "weapon[%d]", def);
			PrintItemLine(label, item);
		}
		PrintItemLine("knife", profile.knife);
		if (profile.gloves.set) {
			advancedfx::Message("    gloves def=%d paint=%d wear=%.4f seed=%d%s\n",
				profile.gloves.defIndex, profile.gloves.paintKit, profile.gloves.wear, profile.gloves.seed,
				sys.ModelSwap() ? "" : " (modelswap OFF -> not applied)");
		}
		if (profile.agent.set) {
			advancedfx::Message("    agent model='%s'%s\n", profile.agent.model.c_str(),
				sys.ModelSwap() ? "" : " (modelswap OFF -> not applied)");
		}
		if (profile.Empty())
			advancedfx::Message("    (empty)\n");
	}

	// Also dump the live spectated player's weapon econ state when watching a demo, so "status" is
	// a one-stop view (global flags + stored profiles + what the apply loop sees right now).
	if (sys.InDemoContext())
		Cosmetics_PrintSpectatedDebug(cmd);
}

void Cosmetics_PrintSpectatedDebug(const char* cmd) {
	CosmeticOverrideSystem& sys = CosmeticsRef();

	const uint8_t obsMode = sys.CurrentObserverMode();
	const int pawnIndex = sys.CurrentSpectatedPawnIndex();
	const uint64_t steamId = sys.CurrentSpectatedSteamId();
	const CosmeticProfile* profile = (steamId != 0) ? sys.Store().Find(steamId) : nullptr;

	advancedfx::Message("%s cosmetics: spectate: obsMode=%u pawn=%d steam=%llu profiled=%d\n",
		cmd, (unsigned int)obsMode, pawnIndex, (unsigned long long)steamId, profile ? 1 : 0);

	if (pawnIndex < 0) {
		advancedfx::Warning("%s cosmetics: no spectated pawn.\n", cmd);
		return;
	}
	if (!g_cosmeticsOffsetsOk) {
		advancedfx::Warning("%s cosmetics: econ offsets did not resolve; cannot read weapon state.\n", cmd);
		return;
	}

	WeaponDebugInfo info;
	if (!ReadWeaponDebugInfo(pawnIndex, &info)) {
		advancedfx::Warning("%s cosmetics: pawn=%d has no active weapon resolved (or read faulted).\n", cmd, pawnIndex);
		return;
	}

	advancedfx::Message("  weapon: index=%d class='%s' ownerXuid=%llu defIndex=%d itemIdHigh=%d paintKit=%d wear=%.4f\n",
		info.weaponIndex, info.className, (unsigned long long)info.ownerXuid, info.itemDefIndex,
		info.itemIdHigh, info.fallbackPaintKit, info.fallbackWear);

	// Dump both attribute lists so we can see where the real (networked) paint kit lives -- the
	// fallback fields above read 0 for a real demo item (itemIdHigh != -1).
	DumpSpectatedWeaponAttributes(pawnIndex, cmd);

	// Read-only agent/player-model path (for the modal's AGENT display).
	ModelNameResult mdl;
	ReadPawnModelName(pawnIndex, &mdl);
	advancedfx::Message("  agent model: %s\n", mdl.ok ? mdl.name : "(unresolved)");

	if (!profile) {
		advancedfx::Message("  (no profile for steamId=%llu -- nothing would be overridden)\n", (unsigned long long)steamId);
		return;
	}

	// Echo what the override WOULD apply for the currently-held item, for before/after visibility.
	CosmeticSlot liveSlot = CosmeticCatalog::IsKnifeDef(info.itemDefIndex)
		? CosmeticSlot::Knife
		: CosmeticCatalog::SlotForDefIndex(info.itemDefIndex);

	const CosmeticItem* wouldApply = nullptr;
	if (liveSlot == CosmeticSlot::Knife && profile->knife.set) {
		wouldApply = &profile->knife;
	} else {
		const CosmeticItem* cand = profile->FindWeapon(info.itemDefIndex);
		if (cand && cand->set)
			wouldApply = cand;
	}

	if (wouldApply) {
		advancedfx::Message("  would apply: slot=%s def=%d paint=%d wear=%.4f seed=%d st=%d\n",
			SlotLabel(liveSlot), wouldApply->defIndex, wouldApply->paintKit, wouldApply->wear,
			wouldApply->seed, wouldApply->statTrak);
	} else {
		advancedfx::Message("  would apply: (no matching slot override for the currently-held item, slot=%s)\n",
			SlotLabel(liveSlot));
	}
}

bool Cosmetics_SpectateSteamId(const char* cmd, uint64_t targetSteam) {
	CosmeticOverrideSystem& sys = CosmeticsRef();
	if (!sys.InDemoContext()) {
		advancedfx::Warning("%s cosmetics spectate: not in a demo.\n", cmd);
		return false;
	}
	if (targetSteam == 0) {
		advancedfx::Warning("%s cosmetics spectate: invalid steamId.\n", cmd);
		return false;
	}

	int specIndex = 0;
	int targetSpecIndex = -1;
	unsigned int targetHealth = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		specIndex++;
		if (ent->GetSteamId() != targetSteam)
			continue;

		SOURCESDK::CS2::CBaseHandle pawnHandle = ent->GetPlayerPawnHandle();
		if (!pawnHandle.IsValid()) {
			advancedfx::Message("%s cosmetics spectate: steam=%llu dead (no pawn) -- skipped.\n",
				cmd, (unsigned long long)targetSteam);
			return false;
		}
		CEntityInstance* pawn = EntFromIndex(pawnHandle.GetEntryIndex());
		if (!pawn || !pawn->IsPlayerPawn()) {
			advancedfx::Message("%s cosmetics spectate: steam=%llu dead (pawn gone) -- skipped.\n",
				cmd, (unsigned long long)targetSteam);
			return false;
		}
		targetHealth = pawn->GetHealth();
		if (targetHealth <= 0) {
			advancedfx::Message("%s cosmetics spectate: steam=%llu dead (health=%u) -- skipped.\n",
				cmd, (unsigned long long)targetSteam, targetHealth);
			return false;
		}
		targetSpecIndex = specIndex;
		break;
	}
	if (targetSpecIndex < 0) {
		advancedfx::Warning("%s cosmetics spectate: steam=%llu not in match.\n",
			cmd, (unsigned long long)targetSteam);
		return false;
	}

	// Prefer direct observer-target write when the local viewer pawn exists; otherwise
	// fall back to a single spec_player call (only for alive targets -- never scan 1..10).
	CEntityInstance* viewerPawn = AfxGetLocalViewerPawn();
	bool usedObserverWrite = false;
	if (viewerPawn) {
		for (int i = 0; i <= highest; ++i) {
			CEntityInstance* ent = EntFromIndex(i);
			if (!ent || !ent->IsPlayerController() || ent->GetSteamId() != targetSteam)
				continue;
			SOURCESDK::CS2::CBaseHandle targetPawnHandle = ent->GetPlayerPawnHandle();
			unsigned char* viewer = (unsigned char*)viewerPawn;
			void* pObs = nullptr;
			__try {
				pObs = *(void**)(viewer + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				pObs = nullptr;
			}
			if (pObs) {
				unsigned char* obs = (unsigned char*)pObs;
				__try {
					*(uint8_t*)(obs + g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode) = 2;
					*(unsigned int*)(obs + g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget) =
						targetPawnHandle.ToInt();
					usedObserverWrite = true;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					usedObserverWrite = false;
				}
			}
			break;
		}
	}

	if (!g_pEngineToClient) {
		advancedfx::Warning("%s cosmetics spectate: engine client unavailable.\n", cmd);
		return false;
	}
	if (usedObserverWrite) {
		g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 2", true);
	} else {
		char specCmd[32];
		std::snprintf(specCmd, sizeof(specCmd), "spec_player %d", targetSpecIndex);
		g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 2", true);
		g_pEngineToClient->ExecuteClientCmd(0, specCmd, true);
	}

	advancedfx::Message("%s cosmetics spectate: steam=%llu specIndex=%d health=%u ok.\n",
		cmd, (unsigned long long)targetSteam, targetSpecIndex, targetHealth);
	return true;
}

void Cosmetics_PrintVisualDiag(const char* cmd) {
	CosmeticOverrideSystem& sys = CosmeticsRef();

	if (!g_cosmeticsOffsetsOk) {
		advancedfx::Warning("%s cosmetics visualdiag: econ offsets did not resolve; cannot read weapon state.\n", cmd);
		return;
	}
	if (!sys.InDemoContext()) {
		advancedfx::Warning("%s cosmetics visualdiag: not in a demo (no weapon to inspect).\n", cmd);
		return;
	}

	const int pawnIndex = sys.CurrentSpectatedPawnIndex();
	const uint64_t steamId = sys.CurrentSpectatedSteamId();
	if (pawnIndex < 0) {
		advancedfx::Warning("%s cosmetics visualdiag: no spectated pawn (spectate a player first).\n", cmd);
		return;
	}

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn()) {
		advancedfx::Warning("%s cosmetics visualdiag: pawn=%d not resolvable.\n", cmd, pawnIndex);
		return;
	}
	const unsigned int pawnHealth = pawn->GetHealth();
	advancedfx::Message("  spectatePawn=%d spectateHealth=%u spectateSteam=%llu\n",
		pawnIndex, pawnHealth, (unsigned long long)steamId);
	if (pawnHealth <= 0) {
		advancedfx::Warning("%s cosmetics visualdiag: pawn=%d is dead (health=%u) -- skip.\n",
			cmd, pawnIndex, pawnHealth);
		return;
	}
	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon) {
		advancedfx::Warning("%s cosmetics visualdiag: pawn=%d has no active weapon resolved.\n", cmd, pawnIndex);
		return;
	}

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	// Class names (read-only const char*; GetClassName is a C++ call, kept out of any SEH helper).
	const char* cls = weapon->GetClassName();
	const char* clientCls = weapon->GetClientClassName();

	// All entity field reads happen inside POD-only SEH helpers (ReadWeaponVisualDiag / ReadAttrList /
	// ReadEntityModelName) -- this function itself has NO __try, so it is free to use std::string
	// temporaries below without tripping MSVC C2712 (no __try + object-unwinding in one function).
	WeaponVisualDiag d;
	ReadWeaponVisualDiag(w, itemView, &d);

	AttrListDump networked, local;
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &networked);
	ReadAttrList(itemView, o.C_EconItemView.m_AttributeList, &local);

	ModelNameResult model;
	ReadEntityModelName(weapon, &model);

	advancedfx::Message("%s cosmetics visualdiag (READ-ONLY):\n", cmd);
	advancedfx::Message("  weapon: index=%d class='%s' clientClass='%s'\n",
		wh.GetEntryIndex(), cls ? cls : "(null)", clientCls ? clientCls : "(null)");
	advancedfx::Message("  activeWeaponHandle: raw=0x%08x entry=%d serial=%d valid=%d\n",
		(unsigned int)wh.ToInt(), wh.GetEntryIndex(), wh.GetSerialNumber(), wh.IsValid() ? 1 : 0);

	if (!d.ok) {
		advancedfx::Warning("  (visual-diag field read faulted -- offsets may be stale)\n");
		return;
	}

	advancedfx::Message("  spectatePawn=%d ownerXuid=%llu spectateSteam=%llu defIndex=%d\n",
		pawnIndex, (unsigned long long)d.ownerXuid, (unsigned long long)steamId, d.defIndex);
	advancedfx::Message("  itemId: high=%d low=%u accountId=%u\n", d.itemIdHigh, d.itemIdLow, d.accountId);
	advancedfx::Message("  fallback: paint=%d wear=%.4f seed=%d stattrak=%d\n",
		d.fbPaint, d.fbWear, d.fbSeed, d.fbStat);

	// Networked dynamic attributes -- where a demo weapon's real skin lives (def6/7/8/81).
	float nPaint = 0, nSeed = 0, nWear = 0, nStat = 0;
	bool hP = AttrValueForDef(networked, 6, &nPaint);
	bool hS = AttrValueForDef(networked, 7, &nSeed);
	bool hW = AttrValueForDef(networked, 8, &nWear);
	bool hT = AttrValueForDef(networked, 81, &nStat);
	advancedfx::Message("  networkedAttrs: count=%d  paint(def6)=%s  seed(def7)=%s  wear(def8)=%s  stattrak(def81)=%s\n",
		networked.ok ? networked.count : -1,
		hP ? std::to_string((int)nPaint).c_str() : "(absent)",
		hS ? std::to_string((int)nSeed).c_str() : "(absent)",
		hW ? std::to_string(nWear).c_str() : "(absent)",
		hT ? std::to_string((int)nStat).c_str() : "(absent)");
	advancedfx::Message("  localAttrs (m_AttributeList): count=%d (usually empty for demo items)\n",
		local.ok ? local.count : -1);

	// The visuals-cache flags + their resolved schema offsets, so the values can be plugged straight
	// into an IDA/Ghidra xref pass (search for byte access at weapon_base + these offsets).
	advancedfx::Message("  visuals flags (the rebuild gate):\n");
	if (d.haveVisualsData)
		advancedfx::Message("    m_bVisualsDataSet            = %u   @+0x%zx\n", (unsigned)d.visualsDataSet, (size_t)o.C_CSWeaponBase.m_bVisualsDataSet);
	else
		advancedfx::Message("    m_bVisualsDataSet            = (offset unresolved)\n");
	if (d.haveClearUgc)
		advancedfx::Message("    m_bClearWeaponIdentifyingUGC = %u   @+0x%zx\n", (unsigned)d.clearUgc, (size_t)o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC);
	else
		advancedfx::Message("    m_bClearWeaponIdentifyingUGC = (offset unresolved)\n");
	if (d.haveReloadEvent)
		advancedfx::Message("    m_nCustomEconReloadEventId   = %d   @+0x%zx\n", d.reloadEvent, (size_t)o.C_CSWeaponBase.m_nCustomEconReloadEventId);
	else
		advancedfx::Message("    m_nCustomEconReloadEventId   = (offset unresolved)\n");
	if (d.haveAttrInit)
		advancedfx::Message("    m_bAttributesInitialized     = %u   @+0x%zx (on C_EconEntity)\n", (unsigned)d.attrInit, (size_t)o.C_EconEntity.m_bAttributesInitialized);

	// Echo which stale-mark writes are currently ENABLED, so the live flag values above can be tied
	// back to what we are (or are not) writing. All-OFF is the HUD-safe default.
	const CosmeticRebuildFlags& rf = sys.GetRebuildFlags();
	advancedfx::Message("  rebuildflags (enabled writes): visualsdata=%d clearugc=%d reloadevent=%d "
		"initialized=%d attrinit=%d imagecache=%d attrparity=%d\n",
		rf.visualsDataSet ? 1 : 0, rf.clearUgc ? 1 : 0, rf.reloadEvent ? 1 : 0,
		rf.initialized ? 1 : 0, rf.attrInit ? 1 : 0, rf.imageCache ? 1 : 0, rf.attrParity ? 1 : 0);

	advancedfx::Message("  worldModel: %s\n", model.ok ? model.name : "(unresolved)");
	advancedfx::Message("  note: viewmodel is a separate C_BaseViewModel entity sharing this econ item; "
		"a skin change alters the composited material, not the model path above.\n");
}

// Finds a weapon entity owned by `steamId` whose live item-definition index == targetDef, so the
// snapshot can reach a weapon the player OWNS but is not currently holding (holstered / inventory) --
// the override is keyed by owner XUID + def, so the matching world entity exists even when the weapon
// is not deployed. Mirrors CosmeticOverrideSystem.cpp's LooksLikeWeaponEntity + TryReadWeaponEconInfo
// gating. SEH-guarded POD body (no C++ objects needing unwind, so __try is legal here). Returns the
// entity (and its index via outIndex) or nullptr if none matched.
CEntityInstance* FindOwnedWeaponByDef(uint64_t steamId, int targetDef, int* outIndex) {
	if (outIndex) *outIndex = -1;
	if (steamId == 0 || targetDef <= 0 || !g_cosmeticsOffsetsOk)
		return nullptr;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = ent->GetClassName();
		const char* clientCls = ent->GetClientClassName();
		bool weaponish = (cls && std::strstr(cls, "weapon_")) || (cls && std::strstr(cls, "Weapon"))
			|| (clientCls && std::strstr(clientCls, "Weapon"));
		if (!weaponish)
			continue;
		unsigned char* w = (unsigned char*)ent;
		bool match = false;
		__try {
			uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
			uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
			uint64_t xuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
			unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
			int liveDef = (int)*(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
			match = (liveDef == targetDef && (xuid == steamId || SteamIdForWeaponProfileLookup(ent, i) == steamId));
		} __except (1) {
			match = false;
		}
		if (match) {
			if (outIndex) *outIndex = i;
			return ent;
		}
	}
	return nullptr;
}

void Cosmetics_LogWeaponSnapshot(const char* cmd, const char* phase, uint64_t steamId, int targetDef) {
	CosmeticOverrideSystem& sys = CosmeticsRef();

	// Always emit SOMETHING so a skin click that produced no visible change is still explained
	// (the user's complaint was that nothing showed up at all). Each early-out states why.
	if (!g_cosmeticsOffsetsOk) {
		advancedfx::Message("%s cosmetics uiclick %s: (econ offsets unresolved -- cannot read live weapon)\n", cmd, phase);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s offsetsUnresolved", phase);
		return;
	}
	if (!sys.InDemoContext()) {
		advancedfx::Message("%s cosmetics uiclick %s: (not in a demo -- no live weapon to read)\n", cmd, phase);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s notInDemo", phase);
		return;
	}

	// Prefer the OWNED weapon that matches the picked def (reaches a holstered/not-deployed weapon);
	// fall back to the spectated player's active/held weapon when targetDef<=0 or nothing matched.
	const uint64_t owner = (steamId != 0) ? steamId : sys.CurrentSpectatedSteamId();
	int weaponIndex = -1;
	CEntityInstance* weapon = FindOwnedWeaponByDef(owner, targetDef, &weaponIndex);
	const char* source = "ownedDef";
	if (!weapon) {
		const int pawnIndex = sys.CurrentSpectatedPawnIndex();
		CEntityInstance* pawn = EntFromIndex(pawnIndex);
		if (!pawn || !pawn->IsPlayerPawn()) {
			advancedfx::Message("%s cosmetics uiclick %s: (def=%d not owned by %llu and no spectated pawn=%d)\n",
				cmd, phase, targetDef, (unsigned long long)owner, pawnIndex);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s noPawn targetDef=%d owner=%llu pawn=%d",
					phase, targetDef, (unsigned long long)owner, pawnIndex);
			return;
		}
		SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
		weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
		weaponIndex = wh.IsValid() ? wh.GetEntryIndex() : -1;
		source = (targetDef > 0) ? "activeWeapon(def-not-found)" : "activeWeapon";
		if (!weapon) {
			advancedfx::Message("%s cosmetics uiclick %s: (def=%d not found and pawn=%d has no active weapon)\n",
				cmd, phase, targetDef, pawnIndex);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s noWeapon targetDef=%d pawn=%d", phase, targetDef, pawnIndex);
			return;
		}
	}

	const char* cls = weapon->GetClassName();
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	// All field reads happen inside the POD-only SEH helpers (no __try in this function), so std::string
	// temporaries below are fine (no MSVC C2712).
	WeaponVisualDiag d;
	ReadWeaponVisualDiag(w, itemView, &d);

	AttrListDump networked;
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &networked);
	float nPaint = 0, nSeed = 0, nWear = 0;
	bool hP = AttrValueForDef(networked, 6, &nPaint);
	bool hS = AttrValueForDef(networked, 7, &nSeed);
	bool hW = AttrValueForDef(networked, 8, &nWear);

	char netPaint[24], netSeed[24], netWear[24];
	if (hP) std::snprintf(netPaint, sizeof(netPaint), "%d", (int)nPaint); else std::snprintf(netPaint, sizeof(netPaint), "absent");
	if (hS) std::snprintf(netSeed, sizeof(netSeed), "%d", (int)nSeed); else std::snprintf(netSeed, sizeof(netSeed), "absent");
	if (hW) std::snprintf(netWear, sizeof(netWear), "%.3f", nWear); else std::snprintf(netWear, sizeof(netWear), "absent");

	// The decisive line: networked def6 paint is where a demo weapon's REAL skin lives, so comparing
	// it before vs after the click shows whether the override actually changed what the game reads.
	advancedfx::Message("%s cosmetics uiclick %s: src=%s idx=%d cls='%s' def=%d ok=%d netPaint=%s netSeed=%s netWear=%s fbPaint=%d fbWear=%.3f\n",
		cmd, phase, source, weaponIndex, cls ? cls : "?", d.defIndex, d.ok ? 1 : 0,
		netPaint, netSeed, netWear, d.fbPaint, d.fbWear);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.uiclick",
			"%s src=%s idx=%d cls='%s' def=%d ok=%d netPaint=%s netSeed=%s netWear=%s fbPaint=%d fbWear=%.3f",
			phase, source, weaponIndex, cls ? cls : "?", d.defIndex, d.ok ? 1 : 0,
			netPaint, netSeed, netWear, d.fbPaint, d.fbWear);
}

void Cosmetics_StorePendingUiGloveLabel(const char* uilogText) {
	StorePendingGloveLabelInternal(uilogText);
}

void Cosmetics_LogGlovePick(uint64_t steamId, int newDef, int newPaint, float newWear, int newSeed) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	const std::string playerName = sys.NameForSteamId(steamId);
	const char* nameDisp = playerName.empty() ? "(unnamed)" : playerName.c_str();

	GloveSnapshot liveBefore;
	ReadGloveSnapshotForSteamId(steamId, &liveBefore);
	std::string beforeLabel;
	if (liveBefore.ok)
		beforeLabel = CosmeticGloveLabels::FormatGloveSkinLabel(liveBefore.def, liveBefore.paint);
	else if (liveBefore.team != 0)
		beforeLabel = CosmeticGloveLabels::TeamDefaultGloveLabel(liveBefore.team);
	else
		beforeLabel = "(unknown gloves)";

	std::string afterLabel = TakePendingGloveLabelInternal();
	if (afterLabel.empty())
		afterLabel = CosmeticGloveLabels::FormatGloveSkinLabel(newDef, newPaint);

	MvmDebugLog_LinefAlways("cosmetics.glove",
		"PICK player='%s' steam=%llu before='%s' after='%s' wantDef=%d wantPaint=%d wantWear=%.4f wantSeed=%d liveBefore(def=%d paint=%d wear=%.4f)",
		nameDisp, (unsigned long long)steamId, beforeLabel.c_str(), afterLabel.c_str(),
		newDef, newPaint, newWear, newSeed,
		liveBefore.ok ? liveBefore.def : -1, liveBefore.ok ? liveBefore.paint : -1,
		liveBefore.ok ? liveBefore.wear : -1.0f);

	char data[384];
	std::snprintf(data, sizeof(data),
		"\"player\":\"%s\",\"steamId\":%llu,\"before\":\"%s\",\"after\":\"%s\",\"wantDef\":%d,\"wantPaint\":%d",
		nameDisp, (unsigned long long)steamId, beforeLabel.c_str(), afterLabel.c_str(), newDef, newPaint);
	MvmAgentLog("DBG", "CosmeticDebug.cpp:LogGlovePick", "glove_pick", data);
}

void Cosmetics_LogWeaponPick(uint64_t steamId, int defIndex, int newPaint, float newWear, int newSeed, int statTrak) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	const std::string playerName = sys.NameForSteamId(steamId);
	const char* nameDisp = playerName.empty() ? "(unnamed)" : playerName.c_str();

	int beforePaint = -1;
	int beforeLegacy = -2;
	int weaponIdx = -1;
	CEntityInstance* weapon = FindOwnedWeaponByDef(steamId, defIndex, &weaponIdx);
	if (weapon) {
		WeaponSkinLive ws;
		ReadWeaponSkinLive(weapon, &ws);
		if (ws.ok) {
			beforePaint = ws.paint;
			if (beforePaint > 0)
				beforeLegacy = PaintKitLegacyModel(beforePaint);
		}
	}

	const int wantLegacy = PaintKitLegacyModel(newPaint);
	const uint64_t wantMask = ResolveMeshMask(newPaint, /*knife=*/false, sys.MeshLegacyMode(),
		1, 2); // log with default masks; tunables live in sys but 1/2 match defaults

	MvmDebugLog_LinefAlways("cosmetics.weapon",
		"PICK player='%s' steam=%llu def=%d idx=%d obs=%u before(paint=%d legacy=%d) want(paint=%d legacy=%d mask=%llu wear=%.4f seed=%d st=%d)",
		nameDisp, (unsigned long long)steamId, defIndex, weaponIdx,
		(unsigned)sys.CurrentObserverMode(),
		beforePaint, beforeLegacy, newPaint, wantLegacy, (unsigned long long)wantMask,
		newWear, newSeed, statTrak);
	char data[512];
	std::snprintf(data, sizeof(data),
		"\"player\":\"%s\",\"steamId\":%llu,\"def\":%d,\"idx\":%d,\"obs\":%u,"
		"\"beforePaint\":%d,\"beforeLegacy\":%d,\"wantPaint\":%d,\"wantLegacy\":%d,\"wantMask\":%llu",
		nameDisp, (unsigned long long)steamId, defIndex, weaponIdx, (unsigned)sys.CurrentObserverMode(),
		beforePaint, beforeLegacy, newPaint, wantLegacy, (unsigned long long)wantMask);
	MvmAgentLog("FP-VM", "CosmeticDebug.cpp:LogWeaponPick", "weapon_pick", data);
}

void Cosmetics_LogSpectateTargetChange(uint64_t fromSteamId, uint64_t toSteamId, const char* reason) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();

	std::string fromName = fromSteamId ? sys.NameForSteamId(fromSteamId) : std::string();
	std::string toName = toSteamId ? sys.NameForSteamId(toSteamId) : std::string();
	if (fromName.empty()) fromName = fromSteamId ? "(unnamed)" : "(none)";
	if (toName.empty()) toName = toSteamId ? "(unnamed)" : "(none)";

	const CosmeticProfile* fromProf = fromSteamId ? sys.Store().Find(fromSteamId) : nullptr;
	const CosmeticProfile* toProf = toSteamId ? sys.Store().Find(toSteamId) : nullptr;
	const std::string fromGloves = fromSteamId
		? GloveLabelForSteamId(fromSteamId, fromProf, true) : std::string("(n/a)");
	const std::string toGloves = toSteamId
		? GloveLabelForSteamId(toSteamId, toProf, true) : std::string("(n/a)");
	const int toProfiled = toProf ? 1 : 0;
	const int gloveRearm = (toProf && toProf->gloves.set && toProf->gloves.defIndex > 0) ? 1 : 0;

	MvmDebugLog_LinefAlways("cosmetics.spectate",
		"SWITCH reason=%s obs=%u pawn=%d from='%s' steam=%llu gloves='%s' -> to='%s' steam=%llu gloves='%s' profiled=%d gloveRearm=%d",
		reason ? reason : "?", (unsigned)sys.CurrentObserverMode(), sys.CurrentSpectatedPawnIndex(),
		fromName.c_str(), (unsigned long long)fromSteamId, fromGloves.c_str(),
		toName.c_str(), (unsigned long long)toSteamId, toGloves.c_str(), toProfiled, gloveRearm);

	char data[384];
	std::snprintf(data, sizeof(data),
		"\"reason\":\"%s\",\"fromName\":\"%s\",\"fromSteam\":%llu,\"toName\":\"%s\",\"toSteam\":%llu,\"gloveRearm\":%d",
		reason ? reason : "?", fromName.c_str(), (unsigned long long)fromSteamId,
		toName.c_str(), (unsigned long long)toSteamId, gloveRearm);
	MvmAgentLog("DBG", "CosmeticDebug.cpp:LogSpectateSwitch", "spectate_switch", data);
}

// Emit ONE full snapshot of the currently-spectated player's LIVE equipped cosmetics into the
// mvm_debug log (category "skin.live"). Covers agent model, gloves+skin, and every owned econ weapon
// (knife/primary/secondary/other) with the def/paint/seed/wear the game is actually rendering AND the
// override the system WOULD apply -- so what renders can be compared against what we set. Read-only;
// every entity read goes through the SEH-guarded POD helpers above (this function itself has no __try,
// so it may freely use std::string). No-op unless mvm_debug is active.
void Cosmetics_LogLiveSkinState(const char* reason) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	if (!g_cosmeticsOffsetsOk || !sys.InDemoContext())
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;

	const int pawnIndex = sys.CurrentSpectatedPawnIndex();
	const uint64_t steamId = sys.CurrentSpectatedSteamId();
	const uint8_t obs = sys.CurrentObserverMode();
	int tick = 0; g_MirvTime.GetCurrentDemoTick(tick);
	const std::string name = sys.NameForSteamId(steamId);
	const CosmeticProfile* prof = (steamId != 0) ? sys.Store().Find(steamId) : nullptr;

	MvmDebugLog_LinefAlways("skin.live",
		"== reason=%s steam=%llu name='%s' pawn=%d obs=%u tick=%d profiled=%d enabled=%d",
		reason ? reason : "?", (unsigned long long)steamId, name.c_str(), pawnIndex,
		(unsigned)obs, tick, prof ? 1 : 0, sys.Enabled() ? 1 : 0);

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (pawnIndex < 0 || !pawn || !pawn->IsPlayerPawn()) {
		MvmDebugLog_LinefAlways("skin.live", "   (no spectated player pawn)");
		return;
	}

	int activeIdx = -1;
	{
		SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
		if (wh.IsValid()) activeIdx = wh.GetEntryIndex();
	}

	// AGENT / player model.
	{
		ModelNameResult agent; ReadPawnModelName(pawnIndex, &agent);
		std::string ov = (prof && prof->agent.set && !prof->agent.model.empty())
			? prof->agent.model : std::string("(none)");
		MvmDebugLog_LinefAlways("skin.live", "   agent model='%s' override='%s'",
			agent.ok ? agent.name : "(unresolved)", ov.c_str());
	}

	// GLOVES (the pawn's embedded m_EconGloves item view -- this IS the third-person glove source; if
	// it reads the override def/paint, the engine should render it without any world-model swap).
	if (o.C_CSPlayerPawn.m_EconGloves == 0) {
		MvmDebugLog_LinefAlways("skin.live", "   gloves (m_EconGloves offset unresolved on this build)");
	} else {
		GloveLiveInfo g; ReadGloveLiveInfo((unsigned char*)pawn, &g);
		unsigned char* glove = (unsigned char*)pawn + o.C_CSPlayerPawn.m_EconGloves;
		AttrListDump net, loc;
		ReadAttrList(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
		ReadAttrList(glove, o.C_EconItemView.m_AttributeList, &loc);
		int gp = -1, gs = -1; float gw = -1.0f; const char* src = "none";
		float p = 0, s = 0, wv = 0;
		if (AttrValueForDef(net, 6, &p)) { gp = (int)p; src = "networked"; if (AttrValueForDef(net, 7, &s)) gs = (int)s; if (AttrValueForDef(net, 8, &wv)) gw = wv; }
		else if (AttrValueForDef(loc, 6, &p)) { gp = (int)p; src = "local"; if (AttrValueForDef(loc, 7, &s)) gs = (int)s; if (AttrValueForDef(loc, 8, &wv)) gw = wv; }
		char ov[112];
		if (prof && prof->gloves.set)
			std::snprintf(ov, sizeof(ov), "def=%d paint=%d wear=%.3f seed=%d", prof->gloves.defIndex, prof->gloves.paintKit, prof->gloves.wear, prof->gloves.seed);
		else
			std::snprintf(ov, sizeof(ov), "(none)");
		const int glovePaintLog = (gp >= 0) ? gp : 0;
		const std::string liveLabel = g.ok
			? CosmeticGloveLabels::FormatGloveSkinLabel(g.def, glovePaintLog) : std::string("(unresolved)");
		MvmDebugLog_LinefAlways("skin.live",
			"   gloves label='%s' liveDef=%d quality=%d itemIdHigh=%d init=%d paint=%d seed=%d wear=%.3f attrSrc=%s netCount=%d override(%s)",
			liveLabel.c_str(),
			g.def, g.haveQuality ? g.quality : -999, g.haveItemId ? g.itemIdHigh : -999,
			g.haveInit ? (int)g.initialized : -1, gp, gs, gw, src, net.ok ? net.count : -1, ov);
	}

	// WEAPONS owned by this player (walk the whole entity list; a player can carry several at once,
	// plus dropped guns still carry the original owner XUID). Classify each into a slot and mark the
	// active/deployed one. "any other skin-related item" the player owns shows here too (slot=none).
	const int highest = GetHighestEntityIndex();
	int logged = 0;
	for (int i = 0; i <= highest && logged < 32; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = ent->GetClassName();
		const char* clientCls = ent->GetClientClassName();
		bool weaponish = (cls && std::strstr(cls, "weapon_")) || (cls && std::strstr(cls, "Weapon"))
			|| (clientCls && std::strstr(clientCls, "Weapon"));
		if (!weaponish)
			continue;
		WeaponSkinLive ws; ReadWeaponSkinLive(ent, &ws);
		if (!ws.ok || ws.ownerXuid != steamId)
			continue;
		++logged;
		CosmeticSlot slot = CosmeticCatalog::IsKnifeDef(ws.def)
			? CosmeticSlot::Knife : CosmeticCatalog::SlotForDefIndex(ws.def);
		const CosmeticItem* item = nullptr;
		if (slot == CosmeticSlot::Knife) { if (prof && prof->knife.set) item = &prof->knife; }
		else if (prof) { const CosmeticItem* c = prof->FindWeapon(ws.def); if (c && c->set) item = c; }
		char ov[112];
		if (item)
			std::snprintf(ov, sizeof(ov), "def=%d paint=%d wear=%.3f seed=%d st=%d", item->defIndex, item->paintKit, item->wear, item->seed, item->statTrak);
		else
			std::snprintf(ov, sizeof(ov), "(none)");
		ModelNameResult wm; ReadEntityModelName(ent, &wm);
		// Legacy classification of the LIVE paint kit (1=legacy CS:GO mesh / 0=modern CS2 mesh /
		// -1=unknown / -2=no paint). A weapon .vmdl holds both meshes; if a skin is authored for the
		// mesh the weapon is NOT currently showing, it renders as default -- compare this between a
		// skin that renders and one that doesn't to confirm the mesh-group mask is the cause.
		int liveLegacy = (slot != CosmeticSlot::Knife && ws.paint > 0) ? PaintKitLegacyModel(ws.paint) : -2;
		MvmDebugLog_LinefAlways("skin.live",
			"   weapon slot=%s idx=%d cls='%s' liveDef=%d active=%d paint=%d seed=%d wear=%.4f stat=%d attrSrc=%s meshLegacy=%d worldModel='%s' override(%s)",
			SlotLabel(slot), i, cls ? cls : "?", ws.def, (i == activeIdx) ? 1 : 0,
			ws.paint, ws.seed, ws.wear, ws.stat, ws.attrSrc, liveLegacy, wm.ok ? wm.name : "(unresolved)", ov);
	}
	if (logged == 0)
		MvmDebugLog_LinefAlways("skin.live", "   (no econ weapons owned by this player in the entity list right now)");

	// First-person viewmodel (HUD-arms child) -- often differs from the world weapon entity above.
	if (pawn && activeIdx >= 0) {
		CEntityInstance* activeEnt = EntFromIndex(activeIdx);
		const char* activeCls = activeEnt ? activeEnt->GetClassName() : nullptr;
		int vmIdx = -1;
		int vmPaint = -1;
		uint64_t vmMesh = 0;
		const bool vmFound = ReadActiveViewmodelWeaponState((unsigned char*)pawn, activeCls,
			&vmIdx, &vmPaint, &vmMesh);
		int vmLegacy = (vmPaint > 0) ? PaintKitLegacyModel(vmPaint) : -2;
		MvmDebugLog_LinefAlways("skin.live",
			"   viewmodel worldIdx=%d vmIdx=%d cls='%s' vmPaint=%d vmMesh=%llu vmLegacy=%d found=%d",
			activeIdx, vmIdx, activeCls ? activeCls : "?", vmPaint,
			(unsigned long long)vmMesh, vmLegacy, vmFound ? 1 : 0);
	} else {
		MvmDebugLog_LinefAlways("skin.live", "   viewmodel (no active weapon / pawn)");
	}
}

// Per-MAIN-thread-frame change detector for the live skin log. Cheap reads each frame (only while
// mvm_debug is active) of the spectated player + active weapon def/paint + glove def/paint + demo
// tick; when any of those change relative to the previous frame -- a player switch, a weapon switch,
// a loadout/skin change, or a demo seek (tick jump) -- it fires ONE full Cosmetics_LogLiveSkinState
// snapshot tagged with the reason. State is function-static (single main-thread caller).
void Cosmetics_TickSkinStateLog() {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	if (!g_cosmeticsOffsetsOk || !sys.InDemoContext())
		return;

	static bool s_have = false;
	static uint64_t s_steam = 0;
	static int s_activeDef = -1;
	static int s_activePaint = INT_MIN;
	static int s_gloveDef = -1;
	static int s_glovePaint = INT_MIN;
	static int s_prevTick = -1;

	const uint64_t steam = sys.CurrentSpectatedSteamId();
	const int pawnIndex = sys.CurrentSpectatedPawnIndex();

	int activeDef = -1, activePaint = INT_MIN, gloveDef = -1, glovePaint = INT_MIN;
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (pawn && pawn->IsPlayerPawn()) {
		const ClientDllOffsets_t& o = g_clientDllOffsets;
		SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
		CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
		if (weapon) {
			WeaponSkinLive ws; ReadWeaponSkinLive(weapon, &ws);
			if (ws.ok) { activeDef = ws.def; activePaint = ws.paint; }
		}
		if (o.C_CSPlayerPawn.m_EconGloves) {
			GloveLiveInfo g; ReadGloveLiveInfo((unsigned char*)pawn, &g);
			if (g.ok) gloveDef = g.def;
			unsigned char* glove = (unsigned char*)pawn + o.C_CSPlayerPawn.m_EconGloves;
			AttrListDump net; ReadAttrList(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
			float p = 0; if (AttrValueForDef(net, 6, &p)) glovePaint = (int)p;
		}
	}

	int tick = 0;
	const bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
	bool seek = false;
	if (haveTick && s_prevTick >= 0) {
		int d = tick - s_prevTick;
		if (d < 0) d = -d;
		seek = (d > 64); // normal playback advances ~1 tick/frame; a seek jumps far
	}
	if (haveTick)
		s_prevTick = tick;

	const char* reason = nullptr;
	if (!s_have) reason = "init";
	else if (steam != s_steam) reason = "player-switch";
	else if (seek) reason = "seek";
	else if (activeDef != s_activeDef) reason = "weapon-change";
	else if (activePaint != s_activePaint || gloveDef != s_gloveDef || glovePaint != s_glovePaint) reason = "loadout-change";

	if (!reason)
		return;

	s_have = true;
	s_steam = steam;
	s_activeDef = activeDef;
	s_activePaint = activePaint;
	s_gloveDef = gloveDef;
	s_glovePaint = glovePaint;

	// Seed state silently until a player is actually being spectated, so the first real snapshot is the
	// baseline ("init") rather than an empty pre-spectate frame.
	if (steam == 0 && pawnIndex < 0)
		return;

	if (reason && 0 == std::strcmp(reason, "player-switch"))
		Cosmetics_LogSpectateTargetChange(s_steam, steam, reason);

	Cosmetics_LogLiveSkinState(reason);
}

} // namespace Filmmaker
