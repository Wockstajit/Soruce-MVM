// Diagnostics for the SteamID-keyed cosmetic override system: "mirv_filmmaker cosmetics status"
// and the spectated-player econ dump used while wiring up new overrides. Read-only: never writes
// to any entity. Entity field reads are SEH-guarded (POD-only bodies inside __try/__except) the
// same way CosmeticOverrideSystem.cpp's ApplyCosmeticWrite / MirvCosmetics::DebugWeapon are, since
// a stale/misclassified entity pointer here would otherwise access-violate the game process.

#include "CosmeticOverrideSystem.h"
#include "CosmeticCatalog.h"

#include "../Platform/TextEncoding.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk

#include "../../../shared/AfxConsole.h"

#include <cstdint>

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

} // namespace

void Cosmetics_PrintStatus(const char* cmd) {
	CosmeticOverrideSystem& sys = CosmeticsRef();
	const CosmeticFrameStats& stats = sys.LastFrameStats();

	advancedfx::Message("%s cosmetics: enabled=%d offsetsResolved=%d demoContext=%d debug=%d\n",
		cmd, sys.Enabled() ? 1 : 0, sys.OffsetsAvailable() ? 1 : 0, sys.InDemoContext() ? 1 : 0, sys.Debug() ? 1 : 0);

	const std::wstring wpath = sys.Store().FilePath();
	const std::string upath = WideToUtf8(wpath);
	advancedfx::Message("  profiles=%zu file='%s'\n", sys.Store().All().size(), upath.c_str());

	advancedfx::Message("  lastFrame: scanned=%d matched=%d patched=%d reverted=%d frame=%llu\n",
		stats.entitiesScanned, stats.entitiesMatched, stats.entitiesPatched, stats.entitiesReverted,
		(unsigned long long)stats.frame);
	advancedfx::Message("  attrs: listsRead=%d valuesWritten=%d valuesChanged=%d empty=%d\n",
		stats.attrListsRead, stats.attrValuesWritten, stats.attrValuesChanged, stats.attrListsEmpty);

	int vtComp = 0, vtSec = 0;
	sys.GetVtIdx(&vtComp, &vtSec);
	advancedfx::Message("  recompose=%d faulted=%d vtComp=%d vtSec=%d vtArg=%d fallbackId=%d\n",
		sys.Recompose() ? 1 : 0, sys.RecomposeFaulted() ? 1 : 0, vtComp, vtSec, sys.VtArg(), sys.FallbackId() ? 1 : 0);

	if (sys.Store().All().empty())
		advancedfx::Message("  (no profiles)\n");

	for (const auto& [steamId, profile] : sys.Store().All()) {
		advancedfx::Message("  %llu '%s':\n", (unsigned long long)steamId, profile.name.c_str());
		PrintItemLine("primary", profile.primary);
		PrintItemLine("secondary", profile.secondary);
		PrintItemLine("knife", profile.knife);
		if (profile.gloves.set) {
			advancedfx::Message("    gloves def=%d paint=%d wear=%.4f seed=%d (stored, not yet applied)\n",
				profile.gloves.defIndex, profile.gloves.paintKit, profile.gloves.wear, profile.gloves.seed);
		}
		if (profile.agent.set) {
			advancedfx::Message("    agent model='%s' (stored, not yet applied)\n", profile.agent.model.c_str());
		}
		if (!profile.primary.set && !profile.secondary.set && !profile.knife.set &&
			!profile.gloves.set && !profile.agent.set) {
			advancedfx::Message("    (empty)\n");
		}
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
	} else if (liveSlot == CosmeticSlot::Secondary && profile->secondary.set &&
		(profile->secondary.defIndex == 0 || profile->secondary.defIndex == info.itemDefIndex)) {
		wouldApply = &profile->secondary;
	} else if (liveSlot == CosmeticSlot::Primary && profile->primary.set &&
		(profile->primary.defIndex == 0 || profile->primary.defIndex == info.itemDefIndex)) {
		wouldApply = &profile->primary;
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

} // namespace Filmmaker
