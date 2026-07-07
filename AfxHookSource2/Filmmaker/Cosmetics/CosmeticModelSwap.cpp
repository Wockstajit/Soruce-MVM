// Model-swap core: HUD-arms / first-person-viewmodel resolution and mirroring, plus the
// weapon mesh-mask (legacy vs CS2 model) and agent (player model) swaps. The subsystem
// is split into focused translation units -- see the map in CosmeticModelSwapInternal.h
// (fn resolver / knife swap / glove swap / this core).

#include "CosmeticModelSwapInternal.h"

#include "../../ClientEntitySystem.h"   // entity-list globals, g_GetEntityFromIndex
#include "../../SchemaSystem.h"          // g_clientDllOffsets
#include "../../../deps/release/prop/cs2/sdk_src/public/entityhandle.h"
#include "CosmeticDebugLog.h"           // MvmDebugLog_Active / MvmDebugLog_Linef (diagnostics)
#include "CosmeticDirectComposite.h"    // FireDirectCompositeRefresh / FireNamedSkinAttributes
#include "CosmeticAnimFix.h"            // EnsureAnimCrashFixInstalled (mesh-mask writes hit the same anim crash)

#include <cstring>
#include <cstdio>

namespace Filmmaker {
namespace modelswap {

namespace {

// Resolve a CEntityInstance* by entity index (same guard pattern as CosmeticOverrideSystem.cpp).
void* EntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return g_GetEntityFromIndex(*g_pEntityList, index);
}

bool LooksLikeWeapon(void* ent) {
	if (!ent) return false;
	CEntityInstance* e = (CEntityInstance*)ent;
	const char* cn = e->GetClassName();
	const char* cc = e->GetClientClassName();
	return (cn && std::strstr(cn, "weapon_")) || (cn && std::strstr(cn, "Weapon")) || (cc && std::strstr(cc, "Weapon"));
}

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

bool SameModelPath(const char* a, const char* b) {
	return a && *a && b && *b && 0 == _stricmp(a, b);
}

void ResolveWeaponMatchModelPath(unsigned char* worldWeapon, const char* explicitModel, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!out || outSize == 0)
		return;
	if (explicitModel && *explicitModel) {
		size_t i = 0;
		for (; explicitModel[i] && i + 1 < outSize; ++i) out[i] = explicitModel[i];
		out[i] = '\0';
		return;
	}
	if (!worldWeapon)
		return;
	ReadEntityModelPath((CEntityInstance*)worldWeapon, out, outSize);
}

bool ViewmodelChildMatchesWeapon(const char* wantClass, const char* wantModel, const char* childClass, const char* childModel) {
	if (wantClass && childClass && 0 == std::strcmp(childClass, wantClass))
		return true;
	return SameModelPath(wantModel, childModel);
}

} // namespace

bool HudArmsOwnedByPawnIndex(unsigned char* arms, int pawnIdx) {
	if (!arms || pawnIdx <= 0)
		return false;
	__try {
		CEntityInstance* ent = (CEntityInstance*)arms;
		const char* cls = ent->GetClassName();
		if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
			return false;
		SOURCESDK::CS2::CBaseHandle owner = ent->GetOwnerEntityHandle();
		return owner.IsValid() && owner.GetEntryIndex() == pawnIdx;
	} __except (1) {
		return false;
	}
}

unsigned char* HudArmsForPawn(unsigned char* pawn) {
	const ptrdiff_t offArms = g_clientDllOffsets.C_CSPlayerPawn.m_hHudModelArms;
	if (!pawn || offArms == 0)
		return nullptr;
	CEntityInstance* pawnInst = (CEntityInstance*)pawn;
	SOURCESDK::CS2::CBaseHandle pawnHandle = pawnInst->GetHandle();
	const int pawnIdx = pawnHandle.IsValid() ? pawnHandle.GetEntryIndex() : -1;
	if (pawnIdx <= 0)
		return nullptr;
	__try {
		uint32_t raw = *(uint32_t*)(pawn + offArms);
		SOURCESDK::CS2::CBaseHandle h(raw);
		if (h.IsValid()) {
			const int armsIdx = h.GetEntryIndex();
			if (armsIdx > 0) {
				unsigned char* ent = (unsigned char*)EntFromIndex(armsIdx);
				if (ent && HudArmsOwnedByPawnIndex(ent, pawnIdx))
					return ent;
			}
		}
	} __except (1) {
	}
	// Demo/spectate: m_hHudModelArms can be stale while the arms entity still exists in the list.
	const int highest = GetHighestEntityIndex();
	for (int i = 1; i <= highest; ++i) {
		CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = ent->GetClassName();
		if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
			continue;
		if (HudArmsOwnedByPawnIndex((unsigned char*)ent, pawnIdx))
			return (unsigned char*)ent;
	}
	return nullptr;
}

namespace {

bool HudArmsContainsWeaponClass(unsigned char* arms, const char* wantClass) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!arms || !wantClass || !*wantClass)
		return false;
	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return false;
	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode)
			return false;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner && LooksLikeWeapon(owner)) {
				const char* oc = ((CEntityInstance*)owner)->GetClassName();
				if (oc && 0 == std::strcmp(oc, wantClass))
					return true;
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
	}
	return false;
}

unsigned char* HudArmsFromPawnSceneChildren(unsigned char* pawn) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!pawn || o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return nullptr;
	__try {
		void* pawnNode = *(void**)(pawn + o.C_BaseEntity.m_pGameSceneNode);
		if (!pawnNode)
			return nullptr;
		void* child = *(void**)((unsigned char*)pawnNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner) {
				const char* cls = ((CEntityInstance*)owner)->GetClassName();
				const char* ccls = ((CEntityInstance*)owner)->GetClientClassName();
				if ((cls && std::strstr(cls, "hudmodel_arms") != nullptr)
					|| (cls && std::strstr(cls, "HudModelArms") != nullptr)
					|| (ccls && std::strstr(ccls, "HudModelArms") != nullptr)) {
					return (unsigned char*)owner;
				}
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
	}
	return nullptr;
}

struct HudArmsResolve {
	unsigned char* arms = nullptr;
	const char* source = "none";
	int armsIdx = -1;
	int hudArmsEntities = 0;
};

HudArmsResolve ResolveHudArmsForViewmodel(unsigned char* ownerPawn, const char* wantWeaponClass) {
	HudArmsResolve r;
	auto finish = [&](unsigned char* arms, const char* src) {
		if (!arms)
			return;
		r.arms = arms;
		r.source = src;
		__try {
			SOURCESDK::CS2::CBaseHandle ah = ((CEntityInstance*)arms)->GetHandle();
			if (ah.IsValid())
				r.armsIdx = ah.GetEntryIndex();
		} __except (1) {
			r.armsIdx = -1;
		}
	};

	if (ownerPawn) {
		unsigned char* arms = HudArmsForPawn(ownerPawn);
		if (arms)
			finish(arms, "ownerPawn");
	}
	if (!r.arms && ownerPawn) {
		unsigned char* arms = HudArmsFromPawnSceneChildren(ownerPawn);
		if (arms)
			finish(arms, "ownerSceneChild");
	}
	if (!r.arms) {
		CEntityInstance* localPawn = AfxGetLocalViewerPawn();
		if (localPawn) {
			unsigned char* arms = HudArmsForPawn((unsigned char*)localPawn);
			if (arms)
				finish(arms, "localViewer");
			else {
				arms = HudArmsFromPawnSceneChildren((unsigned char*)localPawn);
				if (arms)
					finish(arms, "localSceneChild");
			}
		}
	}
	if (!r.arms && wantWeaponClass && *wantWeaponClass) {
		const int highest = GetHighestEntityIndex();
		for (int i = 1; i <= highest; ++i) {
			CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
			if (!ent)
				continue;
			const char* cls = ent->GetClassName();
			if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
				continue;
			++r.hudArmsEntities;
			if (HudArmsContainsWeaponClass((unsigned char*)ent, wantWeaponClass))
				finish((unsigned char*)ent, "entityScanClass");
		}
	}
	if (!r.arms) {
		const int highest = GetHighestEntityIndex();
		for (int i = 1; i <= highest; ++i) {
			CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
			if (!ent)
				continue;
			const char* cls = ent->GetClassName();
			if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
				continue;
			finish((unsigned char*)ent, "entityScanAny");
			break;
		}
	}
	return r;
}

// #region agent log
bool IsLikelyViewmodelClassText(const char* text) {
	return text && (
		std::strstr(text, "ViewModel") != nullptr ||
		std::strstr(text, "viewmodel") != nullptr ||
		std::strstr(text, "HudModel") != nullptr ||
		std::strstr(text, "Arms") != nullptr
	);
}

void AppendSceneChildProbe(char* out, size_t outSize, int& used, const char* tag, unsigned char* entity) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!out || outSize == 0 || !entity || used >= (int)outSize)
		return;
	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return;
	__try {
		void* node = *(void**)(entity + o.C_BaseEntity.m_pGameSceneNode);
		if (!node)
			return;
		void* child = *(void**)((unsigned char*)node + o.CGameSceneNode.m_pChild);
		int count = 0;
		for (; child && count < 8 && used + 180 < (int)outSize; ++count) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			const char* cls = owner ? ((CEntityInstance*)owner)->GetClassName() : "?";
			const char* ccls = owner ? ((CEntityInstance*)owner)->GetClientClassName() : "?";
			int idx = -1;
			if (owner) {
				SOURCESDK::CS2::CBaseHandle h = ((CEntityInstance*)owner)->GetHandle();
				if (h.IsValid())
					idx = h.GetEntryIndex();
			}
			used += std::snprintf(out + used, outSize - (size_t)used,
				" | %sChild[%d]=%d cls='%s' ccls='%s'",
				tag ? tag : "child", count, idx, cls ? cls : "?", ccls ? ccls : "?");
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
	}
}

void BuildViewmodelProbe(unsigned char* ownerPawn, const char* wantWeaponClass, int worldEntityIndex,
	char* out, size_t outSize) {
	if (!out || outSize == 0)
		return;
	int used = 0;
	out[0] = '\0';

	int ownerPawnIdx = -1;
	if (ownerPawn) {
		__try {
			SOURCESDK::CS2::CBaseHandle ph = ((CEntityInstance*)ownerPawn)->GetHandle();
			if (ph.IsValid())
				ownerPawnIdx = ph.GetEntryIndex();
		} __except (1) {
			ownerPawnIdx = -1;
		}
	}

	int obsTargetIdx = -1;
	const int obsMode = (int)AfxGetLocalObserverState(&obsTargetIdx);
	CEntityInstance* localViewerPawn = AfxGetLocalViewerPawn();
	int localPawnIdx = -1;
	const char* localPawnClass = "?";
	int localActiveWeaponIdx = -1;
	if (localViewerPawn) {
		__try {
			SOURCESDK::CS2::CBaseHandle lph = localViewerPawn->GetHandle();
			if (lph.IsValid())
				localPawnIdx = lph.GetEntryIndex();
			localPawnClass = localViewerPawn->GetClassName();
			SOURCESDK::CS2::CBaseHandle aw = localViewerPawn->GetActiveWeaponHandle();
			if (aw.IsValid())
				localActiveWeaponIdx = aw.GetEntryIndex();
		} __except (1) {
			localPawnIdx = -1;
			localPawnClass = "?";
			localActiveWeaponIdx = -1;
		}
	}

	used += std::snprintf(out + used, outSize - (size_t)used,
		"VIEWPROBE worldIdx=%d wantClass='%s' ownerPawn=%d "
		"obsMode=%d obsTarget=%d localPawn=%d localClass='%s' localActive=%d",
		worldEntityIndex, wantWeaponClass ? wantWeaponClass : "?", ownerPawnIdx,
		obsMode, obsTargetIdx,
		localPawnIdx, localPawnClass ? localPawnClass : "?", localActiveWeaponIdx);

	AppendSceneChildProbe(out, outSize, used, "owner", ownerPawn);
	AppendSceneChildProbe(out, outSize, used, "local", (unsigned char*)localViewerPawn);

	int candidates = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 1; i <= highest && candidates < 8 && used + 160 < (int)outSize; ++i) {
		CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = nullptr;
		const char* ccls = nullptr;
		int ownerIdx = -1;
		__try {
			cls = ent->GetClassName();
			ccls = ent->GetClientClassName();
			SOURCESDK::CS2::CBaseHandle oh = ent->GetOwnerEntityHandle();
			if (oh.IsValid())
				ownerIdx = oh.GetEntryIndex();
		} __except (1) {
			cls = nullptr;
			ccls = nullptr;
			ownerIdx = -1;
		}

		const bool interesting =
			IsLikelyViewmodelClassText(cls) ||
			IsLikelyViewmodelClassText(ccls) ||
			(wantWeaponClass && cls && 0 == std::strcmp(cls, wantWeaponClass)) ||
			(ownerPawnIdx > 0 && ownerIdx == ownerPawnIdx) ||
			(localPawnIdx > 0 && ownerIdx == localPawnIdx);
		if (!interesting)
			continue;

		used += std::snprintf(out + used, outSize - (size_t)used,
			" | cand[%d]=%d cls='%s' ccls='%s' owner=%d",
			candidates, i, cls ? cls : "?", ccls ? ccls : "?", ownerIdx);
		++candidates;
	}
}
// #endregion

const char* DesiredBodyChoiceName(uint64_t meshMask) {
	if (meshMask == 1)
		return "body_hd";
	if (meshMask == 2)
		return "body_legacy";
	return "?";
}

} // namespace

// Walk the pawn's HUD-model-arms scene-node children and mirror a model/mesh write onto the
// FIRST-PERSON viewmodel of the weapon we just changed. CRITICAL: the children under the arms are
// EVERY viewmodel weapon parented to this pawn (knife, pistol, rifle, ...), so a model/mesh write may
// ONLY touch a child whose weapon CLASS matches worldWeapon's class. Blindly SetModel-ing all of them
// to (e.g.) the knife model corrupts the other weapons' viewmodels and CRASHES the moment one of them
// is next deployed -- exactly the "switch to the AK after a knife-type swap" crash: the AK viewmodel
// got the knife model, then AK deploy animations ran on a knife model. worldWeapon == null means "no
// model/mesh change, just refresh the renderables" (PostDataUpdate). Class equality (not the old
// m_hOwnerEntity owner-match, which is wrong on some builds) is the safe discriminator: the world
// weapon and its viewmodel share the same entity class (weapon_knife, weapon_ak47, ...). SEH-guarded.
void RefreshViewmodelWeapons(unsigned char* pawn, const char* model, uint64_t meshMask,
	unsigned char* worldWeapon, int entityIndex) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const bool trace = MvmDebugLog_Active();
	const char* wantBodyChoice = DesiredBodyChoiceName(meshMask);
	char wantModel[160];
	wantModel[0] = '\0';
	if (!pawn) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "ABORT noPawn worldIdx=%d", entityIndex);
		return;
	}
	if (o.C_CSPlayerPawn.m_hHudModelArms == 0 || o.C_BaseEntity.m_pGameSceneNode == 0 ||
		o.CGameSceneNode.m_pChild == 0 || o.CGameSceneNode.m_pNextSibling == 0 ||
		o.CGameSceneNode.m_pOwner == 0) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"ABORT missingOffsets worldIdx=%d offArms=0x%llx offScene=0x%llx",
				entityIndex, (unsigned long long)o.C_CSPlayerPawn.m_hHudModelArms,
				(unsigned long long)o.C_BaseEntity.m_pGameSceneNode);
		return;
	}

	const char* wantClass = nullptr;
	if (worldWeapon) {
		__try { wantClass = ((CEntityInstance*)worldWeapon)->GetClassName(); }
		__except (1) { wantClass = nullptr; }
	}
	ResolveWeaponMatchModelPath(worldWeapon, model, wantModel, sizeof(wantModel));

	HudArmsResolve hres = ResolveHudArmsForViewmodel(pawn, wantClass);
	unsigned char* arms = hres.arms;
	if (!arms) {
		if (trace) {
			int pawnIdx = -1;
			__try {
				SOURCESDK::CS2::CBaseHandle ph = ((CEntityInstance*)pawn)->GetHandle();
				if (ph.IsValid()) pawnIdx = ph.GetEntryIndex();
			} __except (1) {
				pawnIdx = -1;
			}
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"ABORT noHudArms pawnIdx=%d worldIdx=%d mask=%llu armsSrc=%s hudArmsEnts=%d wantClass='%s'",
				pawnIdx, entityIndex, (unsigned long long)meshMask, hres.source, hres.hudArmsEntities,
				wantClass ? wantClass : "?");
		}
		return;
	}

	// Class of the weapon we are mirroring onto its own first-person viewmodel. Only same-class
	// children receive the model/mesh write; every other weapon's viewmodel is left untouched.
	if (trace)
		MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
			"walk.begin worldIdx=%d arms=%p armsIdx=%d armsSrc=%s hudArmsEnts=%d wantClass='%s' wantBody='%s' model='%s' mask=%llu",
			entityIndex, (void*)arms, hres.armsIdx, hres.source, hres.hudArmsEntities,
			wantClass ? wantClass : "?", wantBodyChoice, model ? model : "", (unsigned long long)meshMask);
	// #region agent log
	{
		char adata[384];
		std::snprintf(adata, sizeof(adata),
			"\"worldIdx\":%d,\"armsIdx\":%d,\"armsSrc\":\"%s\",\"wantClass\":\"%s\",\"wantModel\":\"%s\",\"wantBody\":\"%s\",\"mask\":%llu",
			entityIndex, hres.armsIdx, hres.source ? hres.source : "?", wantClass ? wantClass : "?",
			wantModel, wantBodyChoice ? wantBodyChoice : "?", (unsigned long long)meshMask);
		MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:RefreshViewmodelWeapons", "vm_walk_begin", adata);
	}
	// #endregion

	// Per-child breadcrumbs are written + flushed BEFORE each renderable is touched, so if the crash is
	// in a half-built viewmodel during a switch, the LAST "knife.vm child" line names the exact child
	// (its owner ptr + class + whether it matched and got the model write) we touched before dying.
	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode) return;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner && LooksLikeWeapon(owner)) {
				bool sameWeapon = false;
				bool classMatch = false;
				bool modelMatch = false;
				const char* oc = nullptr;
				int childIdx = -1;
				int childPaint = -1;
				char childModel[160];
				childModel[0] = '\0';
				oc = ((CEntityInstance*)owner)->GetClassName();
				SOURCESDK::CS2::CBaseHandle ch = ((CEntityInstance*)owner)->GetHandle();
				if (ch.IsValid())
					childIdx = ch.GetEntryIndex();
				ReadEntityModelPath((CEntityInstance*)owner, childModel, sizeof(childModel));
				classMatch = wantClass && oc && 0 == std::strcmp(oc, wantClass);
				modelMatch = SameModelPath(wantModel, childModel);
				sameWeapon = ViewmodelChildMatchesWeapon(wantClass, wantModel, oc, childModel);
				if (trace)
					MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
						"child worldIdx=%d n=%d owner=%p idx=%d cls='%s' model='%s' wantBody='%s' classMatch=%d modelMatch=%d same=%d paint=%d willWriteModel=%d willWriteMesh=%d",
						entityIndex, guard, owner, childIdx, oc ? oc : "?", childModel,
						wantBodyChoice, classMatch ? 1 : 0, modelMatch ? 1 : 0, sameWeapon ? 1 : 0,
						childPaint,
						(sameWeapon && model && *model) ? 1 : 0, (sameWeapon && meshMask) ? 1 : 0);
				// #region agent log
				if (guard < 4) {
					char adata[512];
					std::snprintf(adata, sizeof(adata),
						"\"worldIdx\":%d,\"n\":%d,\"idx\":%d,\"cls\":\"%s\",\"model\":\"%s\",\"wantClass\":\"%s\",\"wantModel\":\"%s\",\"wantBody\":\"%s\",\"classMatch\":%d,\"modelMatch\":%d,\"same\":%d,\"paint\":%d,\"meshWrite\":%d",
						entityIndex, guard, childIdx, oc ? oc : "?", childModel,
						wantClass ? wantClass : "?", wantModel, wantBodyChoice ? wantBodyChoice : "?",
						classMatch ? 1 : 0, modelMatch ? 1 : 0, sameWeapon ? 1 : 0, childPaint, (sameWeapon && meshMask) ? 1 : 0);
					MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:RefreshViewmodelWeapons", "vm_child", adata);
				}
				// #endregion
				if (sameWeapon) {
					if (model && *model)
						SafeSetModel((unsigned char*)owner, model);
					if (meshMask) {
						uint64_t liveChildMask = ReadEntityMeshGroupMask((CEntityInstance*)owner);
						if (liveChildMask != meshMask)
							SafeSetMeshMask((unsigned char*)owner, meshMask);
					}
					ResetAnimGraph((unsigned char*)owner, entityIndex, "vm");
					SafePostDataUpdate((unsigned char*)owner);
				}
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
		if (trace) MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "walk.FAULT worldIdx=%d (SEH caught during child walk)", entityIndex);
		return;
	}
	SafePostDataUpdate(arms);
	if (trace) MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "walk.end worldIdx=%d", entityIndex);
}

} // namespace modelswap

// ---- public API (declared in CosmeticModelSwap.h) ------------------------------------------------------

using namespace modelswap;

bool IsValidAgentModelPath(const char* modelPath) {
	return IsSupportedPlayerModelPath(modelPath);
}

uint64_t ResolveMeshMask(int paintKitId, bool knife, int legacyOverride,
	uint64_t maskModern, uint64_t maskLegacy) {
	int legacy;
	if (legacyOverride == -1) legacy = 0;
	else if (legacyOverride == 1) legacy = 1;
	else legacy = PaintKitLegacyModel(paintKitId); // -2 auto

	if (legacy < 0)
		return 0; // unknown -> leave the mesh untouched

	// Knife polarity differs between the two reference cheats (Andromeda: legacy=2/modern=1; nerv:
	// legacy=1/modern=2). We follow Andromeda's polarity for both weapons and knives (legacy ->
	// maskLegacy, modern -> maskModern) and expose maskModern/maskLegacy as tunables for A/B.
	(void)knife;
	return legacy ? maskLegacy : maskModern;
}

void ApplyWeaponMeshMask(unsigned char* weaponEntity, uint64_t meshMask, unsigned char* pawnForViewmodel, int entityIndex) {
	ResolveModelSwapFns();
	if (meshMask == 0)
		return;
	// approach #3 (the fix that actually works for this crash class, see CosmeticAnimFix.h): install the
	// client.dll anim-builder detour before the mesh-group write below can trigger an animgraph rebuild.
	// ApplyKnifeModelSwap already does this; this function did not, even though its own
	// "approach #1" comment below already identified that a legacy<->CS2 mesh-group toggle hits the SAME
	// null-out-param worker-thread crash as an unfixed knife swap. Confirmed live: a verify run that only
	// exercises weapon-slot mesh toggles (never a knife swap, so the detour was never lazily installed by
	// the knife path) crashed with the identical signature (client.dll read fault at a small offset,
	// animationsystem.dll execute fault) that the knife investigation already root-caused and fixed.
	EnsureAnimCrashFixInstalled();
	SafeSetMeshMask(weaponEntity, meshMask);
	// approach #1 (kept, but per the knife investigation this alone does NOT fix the crash -- see
	// [memory: knife-swap-crash-fix]): reset the animgraph after the mesh-group write so the engine
	// rebuilds it for the new mesh rather than posing it with stale per-mesh-group data.
	ResetAnimGraph(weaponEntity, entityIndex, "world");
	if (pawnForViewmodel)
		RefreshViewmodelWeapons(pawnForViewmodel, nullptr, meshMask, weaponEntity);
	SafePostDataUpdate(weaponEntity);
	// NOTE: a trailing re-assert SafeSetMeshMask() call was tried here to fight a mesh-group revert
	// seen on weapons using the fallback (non-networked) paint path -- live-tested, did NOT converge
	// (preMask still never matched wantMask next frame) and reverted, so removed. The revert happens
	// somewhere other than (or in addition to) PostDataUpdate; root cause still open. See
	// [memory: weapon-mesh-mask-revert-investigation] before trying another patch here.
}

void RefreshWeaponViewmodel(unsigned char* pawn) {
	ResolveModelSwapFns();
	RefreshViewmodelWeapons(pawn, nullptr, 0, nullptr);
}

bool ApplyAgentModel(unsigned char* pawn, const char* modelPath) {
	ResolveModelSwapFns();
	bool setModelFn = g_status.setModel;
	bool pathValid = IsSupportedPlayerModelPath(modelPath);
	bool ok = false;
	if (setModelFn && pawn && pathValid) {
		SafePrecacheModel(modelPath); // approach #2: blocking-load the agent model before SetModel (same risk as knives)
		ok = SafeSetModel(pawn, modelPath);
		if (ok) SafePostDataUpdate(pawn); // refresh the pawn renderable so the agent model shows now
	}
	// Diagnostic line (deduped). setModelFn=0 -> SetModel signature unresolved; pathValid=0 -> the model
	// path was rejected (not an agents//characters/ player model); setModelOk=0 -> SetModel faulted.
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.agent", "setModelFn=%d pawn=%d pathValid=%d setModelOk=%d model='%s'",
			setModelFn ? 1 : 0, pawn ? 1 : 0, pathValid ? 1 : 0, ok ? 1 : 0, modelPath ? modelPath : "");
	return ok;
}

bool PostDataUpdate(unsigned char* entity) {
	return SafePostDataUpdate(entity);
}

uint64_t ReadEntityMeshGroupMask(CEntityInstance* ent) {
	if (!ent)
		return 0;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_MeshGroupMask == 0)
		return 0;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000)
			return 0;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		return *(uint64_t*)(modelState + o.ModelChain.m_MeshGroupMask);
	} __except (1) {
		return 0;
	}
}

int ReadNetPaintFromItemView(unsigned char* itemView) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!itemView || o.C_EconItemView.m_NetworkedDynamicAttributes == 0
		|| o.C_AttributeList.m_Attributes == 0 || o.CEconItemAttribute.m_iAttributeDefinitionIndex == 0
		|| o.CEconItemAttribute.m_flValue == 0)
		return -1;
	int stride = (int)o.CEconItemAttribute.m_size;
	if (stride < (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float))
		stride = (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float);
	unsigned char* vectorField = itemView + o.C_EconItemView.m_NetworkedDynamicAttributes + o.C_AttributeList.m_Attributes;
	__try {
		int count = *(int*)vectorField;
		unsigned char* data = *(unsigned char**)(vectorField + 8);
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			data = *(unsigned char**)vectorField;
			count = *(int*)(vectorField + 16);
		}
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000))
			return -1;
		for (int i = 0; i < count; ++i) {
			unsigned char* attr = data + (ptrdiff_t)i * stride;
			int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
			if (def == 6)
				return (int)*(float*)(attr + o.CEconItemAttribute.m_flValue);
		}
	} __except (1) {
	}
	return -1;
}

unsigned char* ItemViewForWeapon(unsigned char* weapon) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!weapon || o.C_EconEntity.m_AttributeManager == 0 || o.C_AttributeContainer.m_Item == 0)
		return nullptr;
	return weapon + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
}

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
	ptrdiff_t offRestoreMaterial) {
	ViewmodelMirrorResult r = {};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const bool trace = MvmDebugLog_Active();
	const char* wantBodyChoice = DesiredBodyChoiceName(meshMask);
	char wantModel[160];
	wantModel[0] = '\0';
	if (!pawn || !worldWeapon || paintKit <= 0) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"SKIP mirror worldIdx=%d pawn=%p weapon=%p paint=%d",
				worldEntityIndex, (void*)pawn, (void*)worldWeapon, paintKit);
		return r;
	}

	const char* wantClass = nullptr;
	__try { wantClass = ((CEntityInstance*)worldWeapon)->GetClassName(); }
	__except (1) { wantClass = nullptr; }
	ResolveWeaponMatchModelPath(worldWeapon, nullptr, wantModel, sizeof(wantModel));

	HudArmsResolve hres = ResolveHudArmsForViewmodel(pawn, wantClass);
	unsigned char* arms = hres.arms;
	if (!arms) {
		if (trace) {
			int pawnIdx = -1;
			__try {
				SOURCESDK::CS2::CBaseHandle ph = ((CEntityInstance*)pawn)->GetHandle();
				if (ph.IsValid()) pawnIdx = ph.GetEntryIndex();
			} __except (1) {}
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"MIRROR_ABORT noHudArms pawnIdx=%d worldIdx=%d wantClass='%s' paint=%d mask=%llu "
				"armsSrc=%s armsIdx=%d hudArmsEnts=%d",
				pawnIdx, worldEntityIndex, wantClass ? wantClass : "?", paintKit,
				(unsigned long long)meshMask, hres.source, hres.armsIdx, hres.hudArmsEntities);
			char probe[2048];
			BuildViewmodelProbe(pawn, wantClass, worldEntityIndex, probe, sizeof(probe));
			MvmDebugLog_Linef("cosmetics.weapon.vm", "%s", probe);
		}
		return r;
	}
	r.armsFound = true;

	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return r;

	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode)
			return r;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (!owner || !LooksLikeWeapon(owner)) {
				child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
				continue;
			}
			const char* oc = ((CEntityInstance*)owner)->GetClassName();
			char childModel[160];
			childModel[0] = '\0';
			ReadEntityModelPath((CEntityInstance*)owner, childModel, sizeof(childModel));
			const bool classMatch = wantClass && oc && 0 == std::strcmp(oc, wantClass);
			const bool modelMatch = SameModelPath(wantModel, childModel);
			SOURCESDK::CS2::CBaseHandle vhProbe = ((CEntityInstance*)owner)->GetHandle();
			int childIdx = vhProbe.IsValid() ? vhProbe.GetEntryIndex() : -1;
			if (trace)
				MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
					"mirror.child worldIdx=%d n=%d idx=%d cls='%s' model='%s' wantClass='%s' wantModel='%s' wantBody='%s' classMatch=%d modelMatch=%d",
					worldEntityIndex, guard, childIdx, oc ? oc : "?",
					childModel, wantClass ? wantClass : "?", wantModel, wantBodyChoice, classMatch ? 1 : 0, modelMatch ? 1 : 0);
			// #region agent log
			if (guard < 4) {
				char adata[384];
				std::snprintf(adata, sizeof(adata),
					"\"worldIdx\":%d,\"n\":%d,\"idx\":%d,\"cls\":\"%s\",\"model\":\"%s\",\"wantClass\":\"%s\",\"wantModel\":\"%s\",\"wantBody\":\"%s\",\"classMatch\":%d,\"modelMatch\":%d",
					worldEntityIndex, guard, childIdx, oc ? oc : "?",
					childModel, wantClass ? wantClass : "?", wantModel, wantBodyChoice ? wantBodyChoice : "?",
					classMatch ? 1 : 0, modelMatch ? 1 : 0);
				MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:MirrorWeaponCosmeticsToViewmodel", "vm_mirror_child", adata);
			}
			// #endregion
			if (!ViewmodelChildMatchesWeapon(wantClass, wantModel, oc, childModel)) {
				child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
				continue;
			}

			r.childMatched = true;
			unsigned char* vmWeapon = (unsigned char*)owner;
			unsigned char* vmItemView = ItemViewForWeapon(vmWeapon);
			SOURCESDK::CS2::CBaseHandle vh = ((CEntityInstance*)owner)->GetHandle();
			if (vh.IsValid())
				r.vmEntityIndex = vh.GetEntryIndex();

			r.meshBefore = ReadEntityMeshGroupMask((CEntityInstance*)owner);
			if (vmItemView)
				r.paintBefore = ReadNetPaintFromItemView(vmItemView);

			if (meshMask != 0) {
				SafeSetMeshMask(vmWeapon, meshMask);
				r.meshWritten = true;
			}

			if (vmItemView) {
				if (ReadNetPaintFromItemView(vmItemView) < 0)
					r.namedSetter = FireNamedSkinAttributes(vmItemView, paintKit, wear, seed, statTrak);
				DirectCompositeResult dc = FireDirectCompositeRefresh(
					vmWeapon, vmItemView, offRestoreMaterial, compositeOwnerOffset, paintKit, wear, seed);
				r.compositeCalled = dc.called;
				r.compositeFaulted = dc.faulted;
			}

			r.meshAfter = ReadEntityMeshGroupMask((CEntityInstance*)owner);
			if (vmItemView)
				r.paintAfter = ReadNetPaintFromItemView(vmItemView);

			ResetAnimGraph(vmWeapon, r.vmEntityIndex, "vm");
			SafePostDataUpdate(vmWeapon);

			if (trace) {
				MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
					"MIRROR worldIdx=%d vmIdx=%d armsSrc=%s cls='%s' paint=%d->%d mesh=%llu->%llu "
					"meshW=%d named=%d composite=%d fault=%d",
					worldEntityIndex, r.vmEntityIndex, hres.source, oc ? oc : "?",
					r.paintBefore, r.paintAfter,
					(unsigned long long)r.meshBefore, (unsigned long long)r.meshAfter,
					r.meshWritten ? 1 : 0, r.namedSetter ? 1 : 0,
					r.compositeCalled ? 1 : 0, r.compositeFaulted ? 1 : 0);
				char adata[384];
				std::snprintf(adata, sizeof(adata),
					"\"worldIdx\":%d,\"vmIdx\":%d,\"paintBefore\":%d,\"paintAfter\":%d,"
					"\"meshBefore\":%llu,\"meshAfter\":%llu,\"composite\":%d",
					worldEntityIndex, r.vmEntityIndex, r.paintBefore, r.paintAfter,
					(unsigned long long)r.meshBefore, (unsigned long long)r.meshAfter,
					r.compositeCalled ? 1 : 0);
				MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:MirrorWeaponCosmeticsToViewmodel", "vm_mirror", adata);
			}
			break; // only the matching active viewmodel class
		}
		if (!r.childMatched && trace) {
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"MIRROR_NO_CHILD worldIdx=%d wantClass='%s' arms=%p paint=%d mask=%llu",
				worldEntityIndex, wantClass ? wantClass : "?", (void*)arms, paintKit,
				(unsigned long long)meshMask);
		}
	} __except (1) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "MIRROR_FAULT worldIdx=%d", worldEntityIndex);
	}
	(void)worldItemView;
	return r;
}

bool ReadActiveViewmodelWeaponState(unsigned char* pawn, const char* wantWeaponClass,
	int* outVmIndex, int* outPaint, uint64_t* outMeshMask) {
	if (outVmIndex) *outVmIndex = -1;
	if (outPaint) *outPaint = -1;
	if (outMeshMask) *outMeshMask = 0;
	if (!pawn || !wantWeaponClass || !*wantWeaponClass)
		return false;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	HudArmsResolve hres = ResolveHudArmsForViewmodel(pawn, wantWeaponClass);
	unsigned char* arms = hres.arms;
	if (!arms) {
		return false;
	}
	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return false;

	bool found = false;
	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode)
			return false;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner && LooksLikeWeapon(owner)) {
				const char* oc = ((CEntityInstance*)owner)->GetClassName();
				if (oc && 0 == std::strcmp(oc, wantWeaponClass)) {
					SOURCESDK::CS2::CBaseHandle vh = ((CEntityInstance*)owner)->GetHandle();
					if (outVmIndex && vh.IsValid())
						*outVmIndex = vh.GetEntryIndex();
					if (outMeshMask)
						*outMeshMask = ReadEntityMeshGroupMask((CEntityInstance*)owner);
					unsigned char* iv = ItemViewForWeapon((unsigned char*)owner);
					if (outPaint && iv)
						*outPaint = ReadNetPaintFromItemView(iv);
					found = true;
					break;
				}
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
		return false;
	}
	return found;
}

int ResolveViewmodelWeaponEntityIndex(unsigned char* pawn, const char* wantWeaponClass,
	char* dbg, size_t dbgSize, void** outEntity) {
	if (dbg && dbgSize) dbg[0] = '\0';
	if (outEntity) *outEntity = nullptr;
	if (!pawn)
		return -1;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return -1;
	HudArmsResolve hres = ResolveHudArmsForViewmodel(pawn, wantWeaponClass);
	unsigned char* arms = hres.arms;
	int used = 0;
	if (dbg && dbgSize)
		used = std::snprintf(dbg, dbgSize, "arms=%s(idx=%d)", hres.source, hres.armsIdx);
	if (!arms)
		return -1;

	int classMatchIdx = -1, firstWeaponIdx = -1;
	void* classMatchEnt = nullptr, *firstWeaponEnt = nullptr;
	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode)
			return -1;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner && LooksLikeWeapon(owner)) {
				CEntityInstance* oe = (CEntityInstance*)owner;
				SOURCESDK::CS2::CBaseHandle vh = oe->GetHandle();
				const int idx = vh.IsValid() ? vh.GetEntryIndex() : -1;
				const char* oc = oe->GetClassName();
				if (!firstWeaponEnt) { firstWeaponIdx = idx; firstWeaponEnt = owner; }
				if (!classMatchEnt && wantWeaponClass && oc && 0 == std::strcmp(oc, wantWeaponClass)) {
					classMatchIdx = idx; classMatchEnt = owner;
				}
				if (dbg && dbgSize && used > 0 && used < (int)dbgSize)
					used += std::snprintf(dbg + used, dbgSize - used, " child[%d]=%s", idx, oc ? oc : "?");
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
		return -1;
	}
	void* chosen = classMatchEnt ? classMatchEnt : firstWeaponEnt;
	if (outEntity) *outEntity = chosen;
	return classMatchEnt ? classMatchIdx : firstWeaponIdx;
}

} // namespace Filmmaker
