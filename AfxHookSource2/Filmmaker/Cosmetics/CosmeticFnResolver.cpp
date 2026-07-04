// Model-swap function/data resolution shared by every swap path (knife, glove, weapon
// mesh, agent): client.dll byte-pattern scans into the Fns table (ResolveModelSwapFns),
// the SEH-guarded wrappers around the resolved native calls, econ-schema lookups (item
// definition -> model path, paint kit -> legacy flag), and the animgraph-reset
// experiment. Every wrapper degrades to "returns false" on a fault or a moved pattern --
// a CS2 update turns a swap into a no-op, never a crash.

#include "CosmeticModelSwapInternal.h"

#include "../../ClientEntitySystem.h"   // entity-list globals (windows types)
#include "../../SchemaSystem.h"          // g_clientDllOffsets
#include "../../SceneSystem.h"           // CResourceSystem / g_pCResourceSystem (blocking resource precache)
#include "../../../shared/binutils.h"
#include "CosmeticDebugLog.h"           // MvmDebugLog_Active / MvmDebugLog_Linef (diagnostics)

#include <cstring>
#include <cstdio>

namespace Filmmaker {
namespace modelswap {

// ---- client.dll pattern resolution (same approach as CosmeticOverrideSystem.cpp) -------------------
// FindPatternString consumes TWO hex chars per wildcard byte, so a wildcard MUST be "??" not "?".

namespace {

size_t FindClientPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

void* ResolveRelCall(size_t callAddr) {
	if (!callAddr)
		return nullptr;
	int32_t rel = *(int32_t*)(callAddr + 1);
	return (void*)(callAddr + 5 + rel);
}

// CGameSceneNode::PostDataUpdate(this, 0, 0) -- vtable index 22 (Andromeda SDK::VMT_Index, current
// build). Forces the renderable to re-derive its model/mesh/body-group, so a swap shows while paused.
typedef void (__fastcall* PostDataUpdate_t)(void* sceneNode, int a, int b);
constexpr int kPostDataUpdateVtableIndex = 22;

// CEconItemDefinition::m_pszModelName -- a raw econ-data offset (NOT a schema field), confirmed
// identical in Andromeda (0x148) and nerv (get_model_name @ 0x148). Build-specific; the GetStaticData
// path validates the returned string and falls back to the knife table if it looks wrong.
constexpr ptrdiff_t kEconItemDefModelNameOffset = 0x148;
// Econ item schema layout (Andromeda g_CEconItemSchema_GetPaintKits / nerv OFFSET_PAINT_KITS).
constexpr ptrdiff_t kEconItemSystem_SchemaOffset = 0x8;
constexpr ptrdiff_t kEconItemSchema_PaintKitsOffset = 0x2F0;
constexpr ptrdiff_t kPaintKit_IsUseLegacyModelOffset = 0xAE;
// CEconItemSchema item-definition CUtlMap offsets to try (build varies; first hit is cached).
const ptrdiff_t kItemDefMapOffsets[] = { 0x128, 0x120, 0x130, 0x248, 0x250, 0x258 };

} // namespace

Fns g_fns;
ModelSwapResolveStatus g_status;
bool g_precacheOn = true;
uint64_t g_precacheCalls = 0;
ptrdiff_t g_itemDefMapOffset = 0;

// --- experimental animgraph reset (approach #1; see CosmeticModelSwap.h) ---------------------------
// DISPROVEN for this build (default OFF). The reference offsets are the overlay-research VIEWMODEL/AG1
// layout; on the demo world weapon the instance read at 0xD08 returns null (logged inst=0x0 wrote=0), and
// current CS2 weapons run AG2 (m_pGraphInstanceAG2) where the null-vars trick does not map. Kept toggleable
// for experiments ("cosmetics animreset 1 [offsets <instHex> <varsHex>]"). The real fix is precache (#2).
namespace {

bool g_animResetOn = false;
uint32_t g_animInstOff = 0xD08;  // entity -> CAnimationGraphInstance*
uint32_t g_animVarsOff = 0x2E0;  // CAnimationGraphInstance -> pAnimGraphNetworkedVariables

// POD result of the SEH-guarded reset read/write (no C++ objects in the __try body).
struct AnimResetResult { uintptr_t inst = 0; bool wrote = false; bool faulted = false; };

void DoResetAnimGraph(unsigned char* entity, uint32_t instOff, uint32_t varsOff, AnimResetResult* out) {
	*out = AnimResetResult{};
	__try {
		void* inst = *(void**)(entity + instOff);
		out->inst = (uintptr_t)inst;
		if ((uintptr_t)inst > 0x10000) {
			*(void**)((unsigned char*)inst + varsOff) = nullptr;
			out->wrote = true;
		}
	} __except (1) {
		out->faulted = true;
	}
}

} // namespace

// Reset the entity's animgraph networked-variables pointer after a model swap, then log a breadcrumb
// (kept OUT of the __try so MvmDebugLog's std::string is legal). `tag` distinguishes world vs viewmodel.
void ResetAnimGraph(unsigned char* entity, int entityIndex, const char* tag) {
	if (!g_animResetOn || !entity)
		return;
	AnimResetResult r;
	DoResetAnimGraph(entity, g_animInstOff, g_animVarsOff, &r);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("knife.swap",
			"step=animreset.%s idx=%d instOff=0x%x inst=0x%llx wrote=%d faulted=%d varsOff=0x%x",
			tag, entityIndex, g_animInstOff, (unsigned long long)r.inst, r.wrote ? 1 : 0, r.faulted ? 1 : 0, g_animVarsOff);
}

// ---- CUtlStringToken hash (CS2 == murmur2, seed 0x31415926, lowercased) -- ported from nerv ----------

namespace {

uint32_t Murmur2(const void* key, int len, uint32_t seed) {
	const uint32_t m = 0x5bd1e995;
	const int r = 24;
	uint32_t h = seed ^ (uint32_t)len;
	const unsigned char* data = (const unsigned char*)key;
	while (len >= 4) {
		uint32_t k = *(const uint32_t*)data;
		k *= m; k ^= k >> r; k *= m;
		h *= m; h ^= k;
		data += 4; len -= 4;
	}
	switch (len) {
	case 3: h ^= data[2] << 16; // fallthrough
	case 2: h ^= data[1] << 8;  // fallthrough
	case 1: h ^= data[0]; h *= m;
	}
	h ^= h >> 13; h *= m; h ^= h >> 15;
	return h;
}

uint32_t StringTokenHash(const char* str) {
	if (!str) return 0;
	char buf[32];
	int n = 0;
	for (; str[n] && n < (int)sizeof(buf) - 1; ++n) {
		char c = str[n];
		buf[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	buf[n] = '\0';
	return Murmur2(buf, n, 0x31415926u);
}

} // namespace

// ---- model-path validation + econ-schema model lookup -----------------------------------------------

bool IsSupportedModelPath(const char* model) {
	if (!model || !*model || std::strstr(model, ".."))
		return false;
	size_t len = std::strlen(model);
	if (len < 6 || 0 != _stricmp(model + len - 5, ".vmdl"))
		return false;
	return 0 == _strnicmp(model, "weapons/models/", 15)
		|| 0 == _strnicmp(model, "agents/models/", 14)
		|| 0 == _strnicmp(model, "characters/models/", 18);
}

bool IsSupportedPlayerModelPath(const char* model) {
	return IsSupportedModelPath(model)
		&& (0 == _strnicmp(model, "agents/models/", 14)
			|| 0 == _strnicmp(model, "characters/models/", 18));
}

namespace {

bool CopyModelPath(const char* model, char* out, size_t outSize) {
	if (!model || !out || outSize == 0 || !IsSupportedModelPath(model))
		return false;
	size_t i = 0;
	for (; model[i] && i < outSize - 1; ++i) out[i] = model[i];
	out[i] = '\0';
	return i > 0;
}

// Walk CEconItemSchema CUtlMap<int, CEconItemDefinition*> (same node layout as paint kits) for defIndex.
bool TryGetModelFromDefIndex(int defIndex, char* out, size_t outSize, ptrdiff_t* outMapOff) {
	if (!g_fns.getEconItemSystem || defIndex <= 0)
		return false;
	__try {
		void* sys = g_fns.getEconItemSystem(nullptr);
		if (!sys || (uintptr_t)sys < 0x10000)
			return false;
		void* schema = *(void**)((unsigned char*)sys + kEconItemSystem_SchemaOffset);
		if (!schema || (uintptr_t)schema < 0x10000)
			return false;
		const ptrdiff_t* tryOffs = g_itemDefMapOffset ? &g_itemDefMapOffset : nullptr;
		for (int pass = 0; pass < (tryOffs ? 1 : (int)(sizeof(kItemDefMapOffsets) / sizeof(kItemDefMapOffsets[0]))); ++pass) {
			ptrdiff_t mapOff = tryOffs ? *tryOffs : kItemDefMapOffsets[pass];
			unsigned char* map = (unsigned char*)schema + mapOff;
			int count = *(int*)map;
			unsigned char* elems = *(unsigned char**)(map + 8);
			if (count <= 0 || count > 100000 || (uintptr_t)elems < 0x10000)
				continue;
			for (int i = 0; i < count; ++i) {
				unsigned char* node = elems + (ptrdiff_t)i * 32;
				int key = *(int*)(node + 16);
				if (key != defIndex)
					continue;
				void* def = *(void**)(node + 24);
				if (!def || (uintptr_t)def < 0x10000)
					continue;
				const char* model = *(const char**)((unsigned char*)def + kEconItemDefModelNameOffset);
				if (!CopyModelPath(model, out, outSize))
					continue;
				if (!g_itemDefMapOffset)
					g_itemDefMapOffset = mapOff;
				if (outMapOff)
					*outMapOff = mapOff;
				return true;
			}
		}
		if (g_fns.getItemDefByIndex) {
			void* def = g_fns.getItemDefByIndex(schema, defIndex);
			if (def && (uintptr_t)def > 0x10000) {
				const char* model = *(const char**)((unsigned char*)def + kEconItemDefModelNameOffset);
				if (CopyModelPath(model, out, outSize))
					return true;
			}
		}
	} __except (1) {
	}
	return false;
}

} // namespace

bool TryGetModelFromStaticData(unsigned char* itemView, char* out, size_t outSize, const char** failureReason) {
	auto fail = [&](const char* why) {
		if (failureReason) *failureReason = why;
		return false;
	};
	if (!itemView)
		return fail("noItemView");
	int defIndex = 0;
	__try {
		if (g_clientDllOffsets.C_EconItemView.m_iItemDefinitionIndex)
			defIndex = (int)*(uint16_t*)(itemView + g_clientDllOffsets.C_EconItemView.m_iItemDefinitionIndex);
	} __except (1) {
		defIndex = 0;
	}
	if (g_fns.getStaticData) {
		__try {
			void* def = g_fns.getStaticData(itemView);
			if (def) {
				const char* model = *(const char**)((unsigned char*)def + kEconItemDefModelNameOffset);
				if (model && (uintptr_t)model >= 0x10000 && IsSupportedModelPath(model)) {
					size_t i = 0;
					for (; model[i] && i < outSize - 1; ++i) out[i] = model[i];
					out[i] = '\0';
					if (i > 0) {
						if (failureReason) *failureReason = nullptr;
						return true;
					}
				}
			}
		} __except (1) {
		}
	}
	if (defIndex > 0 && TryGetModelFromDefIndex(defIndex, out, outSize, nullptr)) {
		if (failureReason) *failureReason = nullptr;
		return true;
	}
	if (!g_fns.getStaticData)
		return fail("noFn");
	return fail("noDef");
}

// ---- SEH-guarded native-call wrappers ----------------------------------------------------------------

// SEH-guarded blocking precache of a model resource (approach #2). Runs on the swap thread (main) so the
// .vmdl + its per-model animation data are resident before SetModel and before the worker-thread anim pass
// poses the new model -- which is the null-table crash this fixes. No-op if the resource system is missing.
bool SafePrecacheModel(const char* model) {
	if (!g_precacheOn || !model || !*model || !g_pCResourceSystem)
		return false;
	__try {
		g_pCResourceSystem->PreCache(model);
		++g_precacheCalls;
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded SetModel on an entity (weapon world model, viewmodel, or pawn).
bool SafeSetModel(unsigned char* entity, const char* model) {
	if (!g_fns.setModel || !entity || !model || !*model)
		return false;
	__try {
		g_fns.setModel(entity, model);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded SetMeshGroupMask on an entity's scene node.
bool SafeSetMeshMask(unsigned char* entity, uint64_t mask) {
	const ptrdiff_t offNode = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (!g_fns.setMeshGroupMask || !entity || offNode == 0 || mask == 0)
		return false;
	__try {
		void* node = *(void**)(entity + offNode);
		if (!node)
			return false;
		g_fns.setMeshGroupMask(node, mask);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded CGameSceneNode::PostDataUpdate (vtable index 22) on an entity's scene node. This is the
// renderable-refresh Andromeda fires after SetModel/SetMeshGroupMask/UpdateSkin; without it a model or
// mesh swap written while a demo is PAUSED sticks in memory but is not re-evaluated by the renderer.
bool SafePostDataUpdate(unsigned char* entity) {
	const ptrdiff_t offNode = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (!entity || offNode == 0)
		return false;
	__try {
		void* node = *(void**)(entity + offNode);
		if (!node || (uintptr_t)node < 0x10000)
			return false;
		void** vt = *(void***)node;
		if (!vt)
			return false;
		void* fn = vt[kPostDataUpdateVtableIndex];
		if (!fn)
			return false;
		((PostDataUpdate_t)fn)(node, 0, 0);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded subclass re-derive: write m_nSubclassID = token(defStr), then UpdateSubclass(weapon).
bool SafeUpdateSubclass(unsigned char* weapon, int targetDef) {
	const ptrdiff_t offSub = g_clientDllOffsets.C_BaseEntity.m_nSubclassID;
	if (!g_fns.updateSubclass || !weapon)
		return false;
	__try {
		if (offSub) {
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%d", targetDef);
			*(uint32_t*)(weapon + offSub) = StringTokenHash(buf);
		}
		g_fns.updateSubclass(weapon);
		return true;
	} __except (1) {
		return false;
	}
}

// ---- econ-schema paint-kit legacy lookup ---------------------------------------------------------------

namespace {

// CUtlMap<int, CPaintKit*> node layout (x64): left@0 right@4 parent@8 type@12 key@16 value@24 (32 bytes).
// Map header: size@0 allocCount@4 memory@8 root@16 numElements@20.
void* FindPaintKitInSchema(void* schema, int paintKitId) {
	if (!schema || paintKitId <= 0)
		return nullptr;
	unsigned char* map = (unsigned char*)schema + kEconItemSchema_PaintKitsOffset;
	unsigned char* memory = *(unsigned char**)(map + 8);
	if (!memory || (uintptr_t)memory < 0x10000)
		return nullptr;
	int root = *(int*)(map + 16);
	if (root >= 0) {
		int idx = root;
		for (int guard = 0; guard < 96 && idx >= 0; ++guard) {
			unsigned char* node = memory + (ptrdiff_t)idx * 32;
			int key = *(int*)(node + 16);
			if (paintKitId < key)
				idx = *(int*)(node + 0);
			else if (paintKitId > key)
				idx = *(int*)(node + 4);
			else {
				void* pk = *(void**)(node + 24);
				return (pk && (uintptr_t)pk > 0x10000) ? pk : nullptr;
			}
		}
	}
	// Fallback: scan the node pool (covers builds where the tree root is stale but nodes are populated).
	int pool = *(int*)(map + 4);
	if (pool <= 0 || pool > 100000)
		pool = *(int*)map;
	if (pool <= 0 || pool > 100000)
		return nullptr;
	for (int i = 0; i < pool; ++i) {
		unsigned char* node = memory + (ptrdiff_t)i * 32;
		if (*(int*)(node + 16) != paintKitId)
			continue;
		void* pk = *(void**)(node + 24);
		if (pk && (uintptr_t)pk > 0x10000)
			return pk;
	}
	return nullptr;
}

// SEH-guarded econ-schema paint-kit legacy lookup. Returns 1 legacy / 0 modern / -1 unknown.
int SafePaintKitLegacy(int paintKitId) {
	if (!g_fns.getEconItemSystem || paintKitId <= 0)
		return -1;
	__try {
		void* sys = g_fns.getEconItemSystem(nullptr); // static accessor; ignores `this`
		if (!sys || (uintptr_t)sys < 0x10000)
			return -1;
		void* schema = *(void**)((unsigned char*)sys + kEconItemSystem_SchemaOffset);
		if (!schema || (uintptr_t)schema < 0x10000)
			return -1;
		void* pk = FindPaintKitInSchema(schema, paintKitId);
		if (!pk)
			return -1;
		return *(uint8_t*)((unsigned char*)pk + kPaintKit_IsUseLegacyModelOffset) ? 1 : 0;
	} __except (1) {
		return -1;
	}
}

} // namespace

} // namespace modelswap

// ---- public API (declared in CosmeticModelSwap.h) ------------------------------------------------------

using namespace modelswap;

const ModelSwapResolveStatus& ResolveModelSwapFns() {
	if (g_status.attempted)
		return g_status;
	g_status.attempted = true;

	g_fns.setModel = (SetModel_t)FindClientPattern(
		"40 53 48 83 EC ?? 48 8B D9 4C 8B C2 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 40");
	g_fns.setMeshGroupMask = (SetMeshGroupMask_t)FindClientPattern(
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8D 99 ?? ?? ?? ?? 48 8B 71");
	g_fns.updateSubclass = (UpdateSubclass_t)FindClientPattern(
		"4C 8B DC 53 48 81 EC ?? ?? ?? ?? 48 8B 41");
	g_fns.setBodyGroup = (SetBodyGroup_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? EB 0C 48 8B CF"));
	g_fns.setBodyGroupNumeric = (SetBodyGroupNumeric_t)FindClientPattern(
		"85 D2 0F 88 ?? ?? ?? ?? 55 53");
	g_fns.regenerateSkins = (RegenerateSkins_t)FindClientPattern(
		"48 83 EC ?? E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 10");
	g_fns.updateBodyGroupChoice = (UpdateBodyGroupChoice_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? 4C 8B AC 24 ?? ?? ?? ?? 48 8B BC 24"));
	g_fns.getStaticData = (GetStaticData_t)FindClientPattern(
		"40 56 48 83 EC ?? 48 89 5C 24 ?? 48 8B F1 48 8B 1D ?? ?? ?? ?? 48 85 DB 75");
	g_fns.getEconItemSystem = (GetEconItemSystem_t)FindClientPattern(
		"48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85 81");
	g_fns.setAttributeValueByName = (SetAttributeValueByName_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? 66 41 0F 6E D4"));
	g_fns.constructPaintKit = (ConstructPaintKit_t)FindClientPattern(
		"48 89 5C 24 ?? 56 48 83 EC ?? 48 8B 01 FF 50");
	g_fns.getItemDefByIndex = (SchemaGetItemDefByIndex_t)FindClientPattern(
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B D9 89 54 24");

	g_status.setModel = g_fns.setModel != nullptr;
	g_status.setMeshGroupMask = g_fns.setMeshGroupMask != nullptr;
	g_status.updateSubclass = g_fns.updateSubclass != nullptr;
	g_status.setBodyGroup = g_fns.setBodyGroup != nullptr;
	g_status.updateBodyGroupChoice = g_fns.updateBodyGroupChoice != nullptr;
	g_status.getStaticData = g_fns.getStaticData != nullptr;
	g_status.econItemSystem = g_fns.getEconItemSystem != nullptr;
	return g_status;
}

void SetAnimGraphReset(bool enabled) { modelswap::g_animResetOn = enabled; }
bool AnimGraphReset() { return modelswap::g_animResetOn; }
void SetAnimGraphResetOffsets(uint32_t instOff, uint32_t varsOff) {
	modelswap::g_animInstOff = instOff;
	modelswap::g_animVarsOff = varsOff;
}
void GetAnimGraphResetOffsets(uint32_t* instOff, uint32_t* varsOff) {
	if (instOff) *instOff = modelswap::g_animInstOff;
	if (varsOff) *varsOff = modelswap::g_animVarsOff;
}

bool PrecacheModelResource(const char* modelPath) { ResolveModelSwapFns(); return SafePrecacheModel(modelPath); }
void SetPrecacheModels(bool enabled) { g_precacheOn = enabled; }
bool PrecacheModels() { return g_precacheOn; }

int PaintKitLegacyModel(int paintKitId) {
	ResolveModelSwapFns();
	return SafePaintKitLegacy(paintKitId);
}

} // namespace Filmmaker
