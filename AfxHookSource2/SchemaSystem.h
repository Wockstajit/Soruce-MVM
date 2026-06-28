#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <windows.h>

void HookSchemaSystem(HMODULE schemaSystemDll);

// these resolved in runtime
struct ClientDllOffsets_t {
	struct C_CSGameRulesProxy {
		ptrdiff_t m_pGameRules = 0; // C_CSGameRules*
	} C_CSGameRulesProxy;

	struct C_CSGameRules {
		ptrdiff_t m_gamePhase = 0; // int32
		ptrdiff_t m_nOvertimePlaying = 0; // int32
	} C_CSGameRules;

	struct CEntityInstance {
		ptrdiff_t m_pEntity = 0; // CEntityIdentity*
	} CEntityInstance;

	struct C_BaseEntity {
		ptrdiff_t m_pGameSceneNode = 0; // CGameSceneNode*
		ptrdiff_t m_iHealth = 0; // int32
		ptrdiff_t m_iTeamNum = 0; // uint8
		ptrdiff_t m_hOwnerEntity = 0; // CHandle<C_BaseEntity>
	} C_BaseEntity;

	struct C_BaseModelEntity {
		ptrdiff_t m_Glow = 0; // CGlowProperty
	} C_BaseModelEntity;

	struct CGameSceneNode {
	    ptrdiff_t m_pOwner = 0; // CEntityInstance*
        ptrdiff_t m_pParent = 0; // CGameSceneNode*
        ptrdiff_t m_vecAbsOrigin = 0; // VectorWS
	} CGameSceneNode;

	struct C_BaseCSGrenadeProjectile {
		ptrdiff_t m_bCanCreateGrenadeTrail = 0; // bool
		ptrdiff_t m_nSnapshotTrajectoryEffectIndex = 0; // ParticleIndex_t
		ptrdiff_t m_flTrajectoryTrailEffectCreationTime = 0; // float32

	} C_BaseCSGrenadeProjectile;

	struct C_SmokeGrenadeProjectile {
		ptrdiff_t m_vSmokeColor = 0; // Vector
	} C_SmokeGrenadeProjectile;

	struct CBasePlayerController {
		ptrdiff_t m_iszPlayerName = 0; // char[128]
		ptrdiff_t m_steamID = 0; // uint64
		ptrdiff_t m_hPawn = 0; // CHandle< C_CSPlayerPawnBase >
	} CBasePlayerController;

	struct CCSPlayerController {
		ptrdiff_t m_sSanitizedPlayerName = 0; // CUtlString
	} CCSPlayerController;

	struct C_BasePlayerPawn {
		ptrdiff_t m_hController = 0; // CHandle< CBasePlayerController >
		ptrdiff_t m_pWeaponServices = 0; // CPlayer_WeaponServices*
		ptrdiff_t m_pObserverServices = 0; // CPlayer_ObserverServices*
		ptrdiff_t m_pCameraServices = 0; // CPlayer_CameraServices*
	} C_BasePlayerPawn;

	struct CPlayer_CameraServices {
		ptrdiff_t m_hViewEntity = 0; // CHandle< CBaseEntity >
	} CPlayer_CameraServices;

	struct CPlayer_WeaponServices {
		ptrdiff_t m_hActiveWeapon = 0; // CHandle< CBasePlayerWeapon >
	} CPlayer_WeaponServices;

	struct CPlayer_ObserverServices {
		ptrdiff_t m_iObserverMode = 0; // uint8                                   
		ptrdiff_t m_hObserverTarget  = 0; // CHandle< CBaseEntity >
	} CPlayer_ObserverServices;

	struct C_EnvSky {
		ptrdiff_t m_hSkyMaterial = 0; // CStrongHandle<InfoForResourceTypeIMaterial2>
		ptrdiff_t m_vTintColor = 0; // Color
		ptrdiff_t m_flBrightnessScale = 0; // float32
	} C_EnvSky;

	// Econ/cosmetics fields for the offline demo skin-changer (Phase 3). Resolved separately
	// and NON-FATALLY (see g_cosmeticsOffsetsOk) so a renamed field disables skins instead of
	// aborting startup. Weapons (C_CSWeaponBase) derive from C_EconEntity; setting m_iItemIDHigh
	// to -1 makes the client composite the material from the m_nFallback* fields below.
	struct C_EconEntity {
		ptrdiff_t m_OriginalOwnerXuidLow = 0;  // uint32
		ptrdiff_t m_OriginalOwnerXuidHigh = 0; // uint32
		ptrdiff_t m_nFallbackPaintKit = 0;     // int32 (paint kit / skin id)
		ptrdiff_t m_nFallbackSeed = 0;         // int32 (pattern seed)
		ptrdiff_t m_flFallbackWear = 0;        // float32 (0..1 wear)
		ptrdiff_t m_nFallbackStatTrak = 0;     // int32 (-1 = none)
		ptrdiff_t m_AttributeManager = 0;      // C_AttributeContainer (value member)
		// Experimental composite-refresh lever (Phase 2 research): distinct from
		// C_EconItemView::m_bInitialized (an inventory description-text cache flag that does NOT touch
		// the render path). Clearing this on the weapon is hypothesized to force a re-init of its econ
		// attributes / skin composite. Optional / non-fatal -- 0 = field unavailable, lever skipped.
		ptrdiff_t m_bAttributesInitialized = 0; // bool
	} C_EconEntity;

	struct C_AttributeContainer {
		ptrdiff_t m_Item = 0; // C_EconItemView (value member)
	} C_AttributeContainer;

	struct C_EconItemView {
		ptrdiff_t m_bRestoreCustomMaterialAfterPrecache = 0; // bool, optional refresh hint
		ptrdiff_t m_bInventoryImageRgbaRequested = 0; // bool, optional cache invalidation hint
		ptrdiff_t m_bInventoryImageTriedCache = 0;    // bool, optional cache invalidation hint
		ptrdiff_t m_szCurrentLoadCachedFileName = 0;  // char[], optional cache invalidation hint
		ptrdiff_t m_iItemDefinitionIndex = 0; // uint16
		ptrdiff_t m_iItemIDHigh = 0;          // uint32 (-1 forces fallback fields)
		ptrdiff_t m_iItemIDLow = 0;           // uint32
		ptrdiff_t m_iAccountID = 0;           // uint32
		ptrdiff_t m_AttributeList = 0;        // C_AttributeList (value member)
		ptrdiff_t m_NetworkedDynamicAttributes = 0; // C_AttributeList (value member); where a
		                                            // networked/spectated item's paint kit, wear,
		                                            // seed actually live (m_AttributeList is the
		                                            // local cooked list, empty for demo players).
		ptrdiff_t m_bInitialized = 0;         // bool, optional refresh hint
		ptrdiff_t m_bInitializedTags = 0;     // bool, optional refresh hint
	} C_EconItemView;

	struct CAttributeManager {
		ptrdiff_t m_CachedResults = 0;          // optional attribute-cache invalidation hint
		ptrdiff_t m_iReapplyProvisionParity = 0; // optional attribute-cache invalidation hint
	} CAttributeManager;

	struct C_CSWeaponBase {
		ptrdiff_t m_bVisualsDataSet = 0;             // optional render/econ visuals cache hint
		ptrdiff_t m_bClearWeaponIdentifyingUGC = 0;  // optional render/econ visuals cache hint
		ptrdiff_t m_nCustomEconReloadEventId = 0;    // optional render/econ visuals cache hint
	} C_CSWeaponBase;

	struct C_AttributeList {
		ptrdiff_t m_Attributes = 0; // C_UtlVectorEmbeddedNetworkVar<CEconItemAttribute>
	} C_AttributeList;

	struct CEconItemAttribute {
		ptrdiff_t m_iAttributeDefinitionIndex = 0; // uint16
		ptrdiff_t m_flValue = 0;                   // float32
		uint32_t m_size = 0;                       // schema class size / vector stride
	} CEconItemAttribute;

	// Equipped gloves item view lives directly on the player pawn (value member). Used read-only to
	// tell whether the spectated player wears custom gloves so the Customize modal can show them or
	// fall back to the team default. Optional/non-fatal: a 0 offset just means "treat as default".
	struct C_CSPlayerPawn {
		ptrdiff_t m_EconGloves = 0; // C_EconItemView (value member)
	} C_CSPlayerPawn;

	// READ-ONLY model-state chain for showing which agent/player-model a spectated player wears
	// (this is a READ for the modal display; it is NOT a model swap -- see
	// docs/cosmetics-model-override-research.md for why writing the model is server-side-only).
	// Chain: pawn + m_CBodyComponent(ptr) -> + m_skeletonInstance -> + m_modelState -> + m_ModelName.
	struct ModelChain {
		ptrdiff_t m_CBodyComponent = 0;   // C_BaseEntity::m_CBodyComponent (CBodyComponent*, a POINTER)
		ptrdiff_t m_skeletonInstance = 0; // CBodyComponentSkeletonInstance::m_skeletonInstance (embedded)
		ptrdiff_t m_modelState = 0;       // CSkeletonInstance::m_modelState (embedded CModelState)
		ptrdiff_t m_ModelName = 0;        // CModelState::m_ModelName (CUtlSymbolLarge == const char*)
	} ModelChain;
};

extern struct ClientDllOffsets_t g_clientDllOffsets;

// True only if ALL econ/cosmetics offsets above resolved; skin overrides are gated on this.
extern bool g_cosmeticsOffsetsOk;

// https://github.com/sneakyevil/CS2-SchemaDumper/blob/main/CSchemaSystem.hpp

#define S2_PAD_INSERT(x, y) x ## y
#define S2_PAD_DEFINE(x, y) S2_PAD_INSERT(x, y)
#define S2_PAD(size) char S2_PAD_DEFINE(padding_, __LINE__)[size]

namespace SDK
{
	class CSchemaField
	{
	public:
		const char* m_szName;
		void* m_pType;
		uint32_t m_nOffset;
		uint32_t m_nMetadataSize;
		void* m_nMetadata;
	};

	class CSchemaClass
	{
	public:
		void* vfptr;
		const char* m_szName;
		const char* m_szModuleName;
		const char* m_szName2;
		uint32_t m_nSize;
		uint16_t m_nNumFields;

		S2_PAD(0x2);

		uint16_t m_nStaticSize;
		uint16_t m_nMetadataSize;

		S2_PAD(0x4);

		CSchemaField* m_pFields;
	};

	class CSchemaDeclaredClass
	{
	public:
		void* vfptr;
		const char* m_szName;
		const char* m_szModuleName;
		const char* m_szUnknownStr;
		CSchemaClass* m_Class;
	};

	class CSchemaDeclaredClassEntry
	{
	public:
		uint64_t m_nHash[2];
		CSchemaDeclaredClass* m_pDeclaredClass;
	};

	class CSchemaSystemTypeScope
	{
	public:
		void* vfptr;
		char m_szName[256];

		S2_PAD(0x368);

		uint16_t m_nNumDeclaredClasses;

		S2_PAD(0x6);

		CSchemaDeclaredClassEntry* m_pDeclaredClasses;
	};

	class CSchemaSystem
	{
	public:
		S2_PAD(0x190);

		uint64_t m_nScopeSize;
		CSchemaSystemTypeScope** m_pScopeArray;
	};
}
