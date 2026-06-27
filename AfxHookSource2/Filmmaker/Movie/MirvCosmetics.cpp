#include "MirvCosmetics.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include <cstdint>
#include <unordered_map>

namespace Filmmaker {

namespace {

struct SkinOverride {
	int weaponDefIndex = 0; // 0 = legacy active-weapon override
	int paintKit = 0;
	float wear = 0.0f;
	int seed = 0;
	int statTrak = -1; // -1 = no StatTrak
	bool dirty = true; // request a (re)composite on the next frame this weapon is seen
};

struct PendingPreviewOverride {
	int defIndex = 0;
	int paintKit = 0;
	float wear = 0.0f;
	int seed = 0;
};

// pawn entity index -> weapon definition index -> override. Definition 0 is kept for the legacy
// "skin" command and applies to the current active weapon only.
std::unordered_map<int, std::unordered_map<int, SkinOverride>> g_weaponSkins;
std::unordered_map<int, PendingPreviewOverride> g_knives;
std::unordered_map<int, PendingPreviewOverride> g_gloves;
std::unordered_map<int, int> g_agents;

// Optional vtable re-composite path kept for research. The normal refresh path invalidates the
// C_EconItemView initialized flags after writing m_iItemIDHigh=-1 + m_nFallback*, which is safer
// than calling a stale CS2 vtable index during demo playback. OFF by default; use
// "cosmetics recompose 1" only when testing a known-good index.
bool g_recompose = false;
bool g_recomposeFaulted = false; // set if a vtable call raised an SEH exception (auto-disables)

// CBasePlayerWeapon virtual methods that rebuild the composited skin material. UpdateComposite =
// vtable[7], UpdateCompositeSec = vtable[100] per 7sim/CS2-Internal (entity.h) + the UnknownCheats
// writeup. The index-100 call crashed live on a spectated demo weapon, so by default we call ONLY
// UpdateComposite (sec index = -1 = skip). Both indices are runtime-settable (cosmetics vtidx) so
// they can be dialed in without rebuilding, and every call is SEH-guarded below. Signature is
// void* __thiscall(thisptr, bool bRegenerate) -- on x64 __thiscall == __fastcall.
int g_vtComposite = 7;
int g_vtCompositeSec = -1;
typedef void* (__fastcall* UpdateComposite_t)(void* thisptr, bool bShould);

// SEH-isolated vtable call: a wrong index access-violates HERE and is caught, instead of taking
// down the game. Kept free of C++ objects so __try/__except is permitted (no object unwinding).
// Returns false if the call raised a structured exception. (1 == EXCEPTION_EXECUTE_HANDLER; using
// the literal avoids pulling in <windows.h> and its macro clashes.)
static bool SafeVCall(void* obj, int idx, bool arg) {
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

CEntityInstance* EntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

// Resolves the spectated pawn's active weapon entity. Returns nullptr if anything along the path
// is missing.
CEntityInstance* ActiveWeaponOf(int pawnIndex) {
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return nullptr;
	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid())
		return nullptr;
	return EntFromIndex(wh.GetEntryIndex());
}

} // namespace

void Cosmetics_SetSkin(int idx, int paintKit, float wear, int seed, int statTrak) {
	Cosmetics_SetWeapon(idx, 0, paintKit, wear, seed, statTrak);
}

void Cosmetics_SetWeapon(int idx, int weaponDefIndex, int paintKit, float wear, int seed, int statTrak) {
	if (idx < 0) return;
	if (paintKit <= 0) {
		auto it = g_weaponSkins.find(idx);
		if (it != g_weaponSkins.end()) {
			it->second.erase(weaponDefIndex);
			if (it->second.empty()) g_weaponSkins.erase(it);
		}
		return;
	}
	SkinOverride o;
	o.weaponDefIndex = weaponDefIndex;
	o.paintKit = paintKit;
	o.wear = wear;
	o.seed = seed;
	o.statTrak = statTrak;
	o.dirty = true;
	g_weaponSkins[idx][weaponDefIndex] = o;
}

void Cosmetics_SetKnife(int idx, int knifeDefIndex, int paintKit, float wear, int seed) {
	if (idx < 0) return;
	if (knifeDefIndex <= 0 && paintKit <= 0) { g_knives.erase(idx); return; }
	g_knives[idx] = PendingPreviewOverride{ knifeDefIndex, paintKit, wear, seed };
}

void Cosmetics_SetGloves(int idx, int gloveDefIndex, int paintKit, float wear, int seed) {
	if (idx < 0) return;
	if (gloveDefIndex <= 0 && paintKit <= 0) { g_gloves.erase(idx); return; }
	g_gloves[idx] = PendingPreviewOverride{ gloveDefIndex, paintKit, wear, seed };
}

void Cosmetics_SetAgent(int idx, int agentDefIndex) {
	if (idx < 0) return;
	if (agentDefIndex <= 0) { g_agents.erase(idx); return; }
	g_agents[idx] = agentDefIndex;
}

void Cosmetics_ClearPlayer(int idx) { g_weaponSkins.erase(idx); g_knives.erase(idx); g_gloves.erase(idx); g_agents.erase(idx); }

void Cosmetics_ClearAll() { g_weaponSkins.clear(); g_knives.clear(); g_gloves.clear(); g_agents.clear(); }

bool Cosmetics_Available() { return g_cosmeticsOffsetsOk; }

void Cosmetics_SetRecompose(bool enabled) { g_recompose = enabled; if (enabled) g_recomposeFaulted = false; }
bool Cosmetics_GetRecompose() { return g_recompose; }
bool Cosmetics_GetFaulted() { return g_recomposeFaulted; }
void Cosmetics_GetVtIdx(int* comp, int* sec) { if (comp) *comp = g_vtComposite; if (sec) *sec = g_vtCompositeSec; }
void Cosmetics_SetVtIdx(int comp, int sec) {
	g_vtComposite = comp; g_vtCompositeSec = sec;
	g_recompose = true; g_recomposeFaulted = false;        // re-arm so the new indices get tried
	for (auto& byPlayer : g_weaponSkins) for (auto& kv : byPlayer.second) kv.second.dirty = true; // force a re-composite next frame
}

bool Cosmetics_DebugWeapon(int pawnIndex, int* outWeaponIndex, int* outItemIdHigh,
	int* outPaintKit, int* outDefIndex, float* outWear) {
	if (!g_cosmeticsOffsetsOk) return false;
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn()) return false;
	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid()) return false;
	CEntityInstance* weapon = EntFromIndex(wh.GetEntryIndex());
	if (!weapon) return false;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
	if (outWeaponIndex) *outWeaponIndex = wh.GetEntryIndex();
	if (outItemIdHigh)  *outItemIdHigh = *(int32_t*)(itemView + o.C_EconItemView.m_iItemIDHigh);
	if (outPaintKit)    *outPaintKit = *(int32_t*)(w + o.C_EconEntity.m_nFallbackPaintKit);
	if (outDefIndex)    *outDefIndex = *(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
	if (outWear)        *outWear = *(float*)(w + o.C_EconEntity.m_flFallbackWear);
	return true;
}

void Cosmetics_RunFrame() {
	if (!g_cosmeticsOffsetsOk || g_weaponSkins.empty())
		return;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	for (auto& byPlayer : g_weaponSkins) {
		CEntityInstance* weapon = ActiveWeaponOf(byPlayer.first);
		if (!weapon)
			continue;

		unsigned char* w = (unsigned char*)weapon;
		// The embedded C_EconItemView lives inside the weapon's m_AttributeManager.m_Item. Setting
		// its m_iItemIDHigh to -1 makes the client composite the skin from the fallback fields.
		unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
		int32_t* pItemIdHigh = (int32_t*)(itemView + o.C_EconItemView.m_iItemIDHigh);
		int activeDefIndex = *(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
		auto ovIt = byPlayer.second.find(activeDefIndex);
		if (ovIt == byPlayer.second.end()) ovIt = byPlayer.second.find(0);
		if (ovIt == byPlayer.second.end()) continue;
		SkinOverride& ov = ovIt->second;
		int32_t* pPaintKit = (int32_t*)(w + o.C_EconEntity.m_nFallbackPaintKit);
		float* pWear = (float*)(w + o.C_EconEntity.m_flFallbackWear);
		int32_t* pSeed = (int32_t*)(w + o.C_EconEntity.m_nFallbackSeed);
		int32_t* pStatTrak = (int32_t*)(w + o.C_EconEntity.m_nFallbackStatTrak);

		// During demo playback the engine re-networks the original econ item each tick, clobbering
		// our -1 back to the real value. Detect that so we re-composite only when needed (on apply,
		// or whenever the demo just overwrote us) instead of rebuilding the material every frame.
		bool needComposite =
			ov.dirty ||
			(*pItemIdHigh != -1) ||
			(*pPaintKit != ov.paintKit) ||
			(*pWear != ov.wear) ||
			(*pSeed != ov.seed) ||
			(*pStatTrak != ov.statTrak);

		*pItemIdHigh = -1;
		*pPaintKit = ov.paintKit;
		*pWear = ov.wear;
		*pSeed = ov.seed;
		*pStatTrak = ov.statTrak;

		if (needComposite) {
			if (o.C_EconItemView.m_bInitialized) *(bool*)(itemView + o.C_EconItemView.m_bInitialized) = false;
			if (o.C_EconItemView.m_bInitializedTags) *(bool*)(itemView + o.C_EconItemView.m_bInitializedTags) = false;
		}

		if (g_recompose && needComposite) {
			bool ok = true;
			if (g_vtComposite >= 0) ok = SafeVCall(weapon, g_vtComposite, true);
			if (ok && g_vtCompositeSec >= 0) ok = SafeVCall(weapon, g_vtCompositeSec, true);
			if (!ok) { g_recompose = false; g_recomposeFaulted = true; } // bad index -> disable, don't crash
		}
		ov.dirty = false;
	}
}

} // namespace Filmmaker
