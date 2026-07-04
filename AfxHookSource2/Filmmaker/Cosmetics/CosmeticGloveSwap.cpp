// Glove swap: SEH-guarded writes into the pawn's embedded m_EconGloves C_EconItemView
// (def index / quality / account id / paint attributes) plus the body-group and
// gloves-changed rebuild that makes the engine recompose the glove render. Data writes
// are confirmed to persist; rendering on a spectated remote demo pawn is the open half
// (see [memory: glove-render-worldmodelgloves]).

#include "CosmeticModelSwapInternal.h"

#include "../../ClientEntitySystem.h"   // CEntityInstance, CBaseHandle
#include "../../SchemaSystem.h"          // g_clientDllOffsets
#include "CosmeticDebugLog.h"           // MvmDebugLog_* + MvmAgentLog + MvmCrashWatch_Arm

#include <cstdio>

namespace Filmmaker {
namespace modelswap {

namespace {

// Overwrite paint/seed/wear attrs in-place on one C_EconItemView attribute list (networked or local).
void WriteGlovePaintAttrs(unsigned char* glove, ptrdiff_t listOff, int paintKit, float wear, int seed,
	int* paintW, int* seedW, int* wearW, int* attrVec, int* attrCount) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!glove || !listOff || !o.C_AttributeList.m_Attributes
		|| !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_flValue
		|| !o.CEconItemAttribute.m_size)
		return;
	unsigned char* vec = glove + listOff + o.C_AttributeList.m_Attributes;
	int count = *(int*)vec;
	unsigned char* data = *(unsigned char**)(vec + 8);
	if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
		data = *(unsigned char**)vec;
		count = *(int*)(vec + 16);
	}
	if (count <= 0 || count > 128 || (uintptr_t)data <= 0x10000)
		return;
	if (listOff == o.C_EconItemView.m_NetworkedDynamicAttributes) {
		if (attrVec) *attrVec = 1;
		if (attrCount) *attrCount = count;
	}
	const int stride = (int)o.CEconItemAttribute.m_size;
	for (int i = 0; i < count; ++i) {
		unsigned char* attr = data + (ptrdiff_t)i * stride;
		const int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
		float* pv = (float*)(attr + o.CEconItemAttribute.m_flValue);
		if (def == 6) { *pv = (float)paintKit; if (paintW) *paintW = 1; }
		else if (def == 7) { *pv = (float)seed; if (seedW) *seedW = 1; }
		else if (def == 8) { *pv = wear; if (wearW) *wearW = 1; }
	}
}

void ReadGlovePaintAttrs(unsigned char* glove, ptrdiff_t listOff, int* paint, int* seed, float* wear) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!glove || !paint || !listOff || !o.C_AttributeList.m_Attributes
		|| !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_flValue
		|| !o.CEconItemAttribute.m_size)
		return;
	unsigned char* vec = glove + listOff + o.C_AttributeList.m_Attributes;
	int count = *(int*)vec;
	unsigned char* data = *(unsigned char**)(vec + 8);
	if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
		data = *(unsigned char**)vec;
		count = *(int*)(vec + 16);
	}
	if (count <= 0 || count > 128 || (uintptr_t)data <= 0x10000)
		return;
	const int stride = (int)o.CEconItemAttribute.m_size;
	for (int i = 0; i < count; ++i) {
		unsigned char* attr = data + (ptrdiff_t)i * stride;
		const int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
		const float v = *(float*)(attr + o.CEconItemAttribute.m_flValue);
		if (def == 6) *paint = (int)v;
		else if (def == 7 && seed) *seed = (int)v;
		else if (def == 8 && wear) *wear = v;
	}
}

// SEH-guarded glove field write + pawn/HUD-arms rebuild.
bool SafeApplyGloves(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed, uint32_t accountId,
	bool composePaintKit) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const ptrdiff_t offGloves = o.C_CSPlayerPawn.m_EconGloves;
	if (!pawn || offGloves == 0 || o.C_EconItemView.m_iItemDefinitionIndex == 0) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.glove", "ABORT pawn=%d offGloves=0x%llx defIdxOff=0x%llx (offsets missing)",
				pawn ? 1 : 0, (unsigned long long)offGloves,
				(unsigned long long)o.C_EconItemView.m_iItemDefinitionIndex);
		return false;
	}
	unsigned char* glove = pawn + offGloves; // embedded C_EconItemView
	char gloveModel[260] = {};
	const char* modelFail = "unknown";
	// Diagnostics: POD counters filled inside the SEH block, logged after it (see "cosmetics.glove").
	int dAttrVec = 0, dAttrCount = 0, dPaintW = 0, dSeedW = 0, dWearW = 0, dCoreFaulted = 0, dBodyFaulted = 0;
	int dNamedSetter = 0, dConstructPk = 0;
	int dBodyGroupNum = 0, dBodyGroupStr = 0, dGlovesChanged = 0, dTeam = 0, dBootstrapped = 0;
	const bool trace = MvmDebugLog_Active();
	int pawnIdx = -1;
	__try {
		CEntityInstance* pawnInst = (CEntityInstance*)pawn;
		if (!pawnInst->IsPlayerPawn())
			return false;
		SOURCESDK::CS2::CBaseHandle ph = pawnInst->GetHandle();
		if (ph.IsValid())
			pawnIdx = ph.GetEntryIndex();
		if (o.C_BaseEntity.m_iTeamNum)
			dTeam = (int)*(uint8_t*)(pawn + o.C_BaseEntity.m_iTeamNum);
	} __except (1) {
		return false;
	}
	if (pawnIdx <= 0 || (dTeam != 2 && dTeam != 3))
		return false;
	__try {
		uint16_t curDef = *(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		// Demo pawns often have m_EconGloves def=0 until the engine initializes default gloves.
		// Bootstrap team defaults before writing a custom glove (nerv clear-then-reapply pattern).
		if (curDef == 0) {
			const uint16_t bootDef = (dTeam == 2) ? 5028 : 5029;
			*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex) = bootDef;
			dBootstrapped = 1;
		}
		*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex) = (uint16_t)gloveDef;
		if (o.C_EconItemView.m_iEntityQuality)
			*(int32_t*)(glove + o.C_EconItemView.m_iEntityQuality) = 3; // QUALITY_UNUSUAL
		if (o.C_EconItemView.m_iAccountID)
			*(uint32_t*)(glove + o.C_EconItemView.m_iAccountID) = accountId;
		if (o.C_EconItemView.m_iItemIDHigh)
			*(uint32_t*)(glove + o.C_EconItemView.m_iItemIDHigh) = 1;
		if (o.C_EconItemView.m_iItemIDLow)
			*(uint32_t*)(glove + o.C_EconItemView.m_iItemIDLow) = accountId;
		if (o.C_EconItemView.m_iItemID) {
			uint64_t itemId = ((uint64_t)1u << 32) | (uint64_t)accountId;
			*(uint64_t*)(glove + o.C_EconItemView.m_iItemID) = itemId;
		}
		if (o.C_EconItemView.m_bDisallowSOC)
			*(bool*)(glove + o.C_EconItemView.m_bDisallowSOC) = false;
		if (o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache)
			*(bool*)(glove + o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache) = true;
		// Overwrite paint attributes on networked list, then local list if still empty.
		WriteGlovePaintAttrs(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, paintKit, wear, seed,
			&dPaintW, &dSeedW, &dWearW, &dAttrVec, &dAttrCount);
		if (!dPaintW)
			WriteGlovePaintAttrs(glove, o.C_EconItemView.m_AttributeList, paintKit, wear, seed,
				&dPaintW, &dSeedW, &dWearW, nullptr, nullptr);
		// Missing-attribute fallback: a DEFAULT glove has no networked paint attribute to overwrite
		// (dPaintW stays 0 above), so paint can never be written by the vector path -- apply it through
		// the engine named-setter instead (the same path the weapon loop uses), which adds the attribute
		// to the item view if absent. Without this, default gloves could not be painted at all.
		if (paintKit > 0 && !dPaintW && g_fns.setAttributeValueByName) {
			g_fns.setAttributeValueByName(glove, "set item texture prefab", (float)paintKit);
			g_fns.setAttributeValueByName(glove, "set item texture wear", wear);
			g_fns.setAttributeValueByName(glove, "set item texture seed", (float)seed);
			dNamedSetter = 1;
		}
		if (o.C_EconItemView.m_bInitialized)
			*(bool*)(glove + o.C_EconItemView.m_bInitialized) = true;
	} __except (1) {
		dCoreFaulted = 1;
	}
	if (!dCoreFaulted && composePaintKit && paintKit > 0 && g_fns.constructPaintKit) {
		__try {
			g_fns.constructPaintKit(glove);
			dConstructPk = 1;
		} __except (1) {
			if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=constructPk.FAULT idx=%d", pawnIdx);
		}
	}
	// Body-group / glove-changed flags rebuild render composites -- only on the first burst frame.
	if (!dCoreFaulted && composePaintKit) {
		__try {
			if (g_fns.setBodyGroupNumeric) {
				g_fns.setBodyGroupNumeric(pawn, 0, 1);
				dBodyGroupNum = 1;
			}
			// Both teams need the named first_or_third_person group for third-person glove meshes.
			// (setBodyGroupNumeric(1,1) faults; the string form is safe on T and CT.)
			if ((dTeam == 2 || dTeam == 3) && g_fns.setBodyGroup) {
				g_fns.setBodyGroup(pawn, "first_or_third_person", 1);
				dBodyGroupStr = 1;
			}
			if (g_fns.updateBodyGroupChoice)
				g_fns.updateBodyGroupChoice(pawn);
			if (o.C_CSPlayerPawn.m_nEconGlovesChanged) {
				uint8_t* pChg = (uint8_t*)(pawn + o.C_CSPlayerPawn.m_nEconGlovesChanged);
				*pChg = (uint8_t)(*pChg + 1u);
				dGlovesChanged = 1;
			}
			if (o.C_CSPlayerPawn.m_bNeedToReApplyGloves)
				*(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves) = true;
		} __except (1) {
			dBodyFaulted = 1;
		}
	}
	const int dFaulted = dCoreFaulted ? 1 : 0;
	bool haveGloveModel = false;
	ptrdiff_t defMapOff = g_itemDefMapOffset;
	if (dCoreFaulted) {
		if (trace) {
			MvmCrashWatch_Arm(pawnIdx, "glove-core-fault");
			MvmDebugLog_LinefAlways("glove.swap", "ABORT idx=%d coreFault=1 team=%d", pawnIdx, dTeam);
			MvmDebugLog_LinefAlways("cosmetics.glove",
				"def=%d paint=%d ABORT coreFault=1 team=%d pawnIdx=%d", gloveDef, paintKit, dTeam, pawnIdx);
		}
		return false;
	}
	haveGloveModel = TryGetModelFromStaticData(glove, gloveModel, sizeof(gloveModel), &modelFail);
	defMapOff = g_itemDefMapOffset;
	if (trace) {
		MvmCrashWatch_Arm(pawnIdx, haveGloveModel ? gloveModel : "glove");
		MvmDebugLog_LinefAlways("glove.swap", "BEGIN idx=%d pawn=%p def=%d paint=%d team=%d model='%s'",
			pawnIdx, (void*)pawn, gloveDef, paintKit, dTeam, haveGloveModel ? gloveModel : "");
	}
	if (haveGloveModel)
		SafePrecacheModel(gloveModel);
	const ptrdiff_t offArms = o.C_CSPlayerPawn.m_hHudModelArms;
	uint32_t armsHandle = 0;
	int armsIdx = -1;
	if (pawn && offArms) {
		__try {
			armsHandle = *(uint32_t*)(pawn + offArms);
			SOURCESDK::CS2::CBaseHandle h(armsHandle);
			if (h.IsValid())
				armsIdx = h.GetEntryIndex();
		} __except (1) { armsHandle = 0; }
	}
	unsigned char* arms = nullptr;
	bool armsResolved = false;
	if (composePaintKit) {
		// Refresh the pawn renderable so the new glove body group renders without waiting for a live sim
		// frame (essential during a paused demo). Separate SEH scope from the field writes above.
		SafePostDataUpdate(pawn);
		arms = HudArmsForPawn(pawn);
		armsResolved = arms != nullptr;
		if (arms) {
			__try {
				SOURCESDK::CS2::CBaseHandle ah = ((CEntityInstance*)arms)->GetHandle();
				if (ah.IsValid())
					armsIdx = ah.GetEntryIndex();
			} __except (1) { armsIdx = -1; }
		}
		if (arms && haveGloveModel && HudArmsOwnedByPawnIndex(arms, pawnIdx)) {
			if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=armsSetModel.begin idx=%d arms=%p", pawnIdx, (void*)arms);
			SafeSetModel(arms, gloveModel);
			if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=armsSetModel.end idx=%d", pawnIdx);
			SafePostDataUpdate(arms);
		} else if (arms && trace) {
			MvmDebugLog_LinefAlways("glove.swap", "step=armsSetModel.skip idx=%d arms=%p ownerMismatch=1", pawnIdx, (void*)arms);
		}
	}
	// Read live glove state after writes (persist check; networked first, then local attrs).
	int liveDef = 0, livePaint = 0;
	float liveWear = 0.0f;
	int liveSeed = 0;
	__try {
		liveDef = (int)*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		ReadGlovePaintAttrs(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &livePaint, &liveSeed, &liveWear);
		if (livePaint == 0)
			ReadGlovePaintAttrs(glove, o.C_EconItemView.m_AttributeList, &livePaint, &liveSeed, &liveWear);
	} __except (1) {
	}
	// Diagnostic line (always flushed during mvm_debug). Tells which sub-step broke: dAttrVec=0 -> the demo
	// glove has NO networked attribute vector (paint can't be written -> default skin); bodyGroupFn=0
	// -> UpdateBodyGroupChoice unresolved; arms=0 -> first-person glove model not refreshed; haveModel=0
	// -> glove def's model path didn't resolve.
	if (MvmDebugLog_Active()) {
		MvmDebugLog_LinefAlways("cosmetics.glove",
			"def=%d paint=%d seed=%d wear=%.3f faulted=%d attrVec=%d attrCount=%d wrote(p=%d s=%d w=%d) "
			"namedSetter=%d bootstrapped=%d bodyGroupFn=%d bodyGroupNum=%d bodyGroupStr=%d bodyFault=%d team=%d "
			"needReApplyOff=%d initOff=%d haveModel=%d arms=%d model='%s' "
			"liveDef=%d livePaint=%d liveSeed=%d liveWear=%.3f",
			gloveDef, paintKit, seed, wear, dFaulted, dAttrVec, dAttrCount, dPaintW, dSeedW, dWearW,
			dNamedSetter, dBootstrapped, g_fns.updateBodyGroupChoice ? 1 : 0, dBodyGroupNum, dBodyGroupStr, dBodyFaulted, dTeam,
			o.C_CSPlayerPawn.m_bNeedToReApplyGloves ? 1 : 0,
			o.C_EconItemView.m_bInitialized ? 1 : 0,
			haveGloveModel ? 1 : 0, armsResolved ? 1 : 0, haveGloveModel ? gloveModel : "",
			liveDef, livePaint, liveSeed, liveWear);
		char data[720];
		std::snprintf(data, sizeof(data),
			"\"wantDef\":%d,\"wantPaint\":%d,\"liveDef\":%d,\"livePaint\":%d,"
			"\"wrotePaint\":%d,\"namedSetter\":%d,\"constructPk\":%d,\"bodyGroupNum\":%d,"
			"\"bodyGroupStr\":%d,\"bodyFault\":%d,\"team\":%d,\"glovesChanged\":%d,"
			"\"attrVec\":%d,\"haveModel\":%d,\"modelFail\":\"%s\","
			"\"arms\":%d,\"armsHandle\":%u,\"armsIdx\":%d,\"pawnIdx\":%d,\"getStaticData\":%d,\"getItemDefByIndex\":%d,\"defMapOff\":%lld,"
			"\"disallowSocOff\":%d,\"restoreMatOff\":%d,\"itemIdOff\":%d,\"faulted\":%d",
			gloveDef, paintKit, liveDef, livePaint, dPaintW, dNamedSetter, dConstructPk, dBodyGroupNum,
			dBodyGroupStr, dBodyFaulted, dTeam, dGlovesChanged,
			dAttrVec,
			haveGloveModel ? 1 : 0, modelFail ? modelFail : "",
			armsResolved ? 1 : 0, armsHandle, armsIdx, pawnIdx,
			g_fns.getStaticData ? 1 : 0, g_fns.getItemDefByIndex ? 1 : 0, (long long)defMapOff,
			o.C_EconItemView.m_bDisallowSOC ? 1 : 0,
			o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache ? 1 : 0,
			o.C_EconItemView.m_iItemID ? 1 : 0,
			dFaulted);
		MvmAgentLog(liveDef == gloveDef && (paintKit <= 0 || livePaint == paintKit) ? "H5" : "H4",
			"CosmeticModelSwap.cpp:SafeApplyGloves", "apply_done", data);
		if (trace)
			MvmDebugLog_LinefAlways("glove.swap", "END idx=%d coreFault=%d bodyFault=%d arms=%d compose=%d",
				pawnIdx, dCoreFaulted, dBodyFaulted, armsResolved ? 1 : 0, composePaintKit ? 1 : 0);
	}
	return !dCoreFaulted;
}

} // namespace

} // namespace modelswap

using namespace modelswap;

bool ApplyGloveModel(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed,
	uint32_t accountId, bool composePaintKit) {
	ResolveModelSwapFns();
	if (!pawn || gloveDef <= 0) {
		if (MvmDebugLog_Active())
			MvmAgentLog("H3", "CosmeticModelSwap.cpp:ApplyGloveModel", "bad_args",
				pawn ? "\"pawn\":1" : "\"pawn\":0");
		return false;
	}
	if (!g_status.GlovesOk()) {
		if (MvmDebugLog_Active()) {
			char data[160];
			std::snprintf(data, sizeof(data),
				"\"setModel\":%d,\"updateBodyGroupChoice\":%d",
				g_status.setModel ? 1 : 0, g_status.updateBodyGroupChoice ? 1 : 0);
			MvmAgentLog("H3", "CosmeticModelSwap.cpp:ApplyGloveModel", "gloves_not_ok", data);
			MvmDebugLog_LinefAlways("cosmetics.glove", "ABORT GlovesOk=0 setModel=%d bodyGroupFn=%d",
				g_status.setModel ? 1 : 0, g_status.updateBodyGroupChoice ? 1 : 0);
		}
		return false;
	}
	return SafeApplyGloves(pawn, gloveDef, paintKit, wear, seed, accountId, composePaintKit);
}

} // namespace Filmmaker
