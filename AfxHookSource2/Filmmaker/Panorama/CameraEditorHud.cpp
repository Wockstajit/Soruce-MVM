#include "CameraEditorHud.h"

#include "CameraEditorJs.h"
#include "CameraTimelineHud.h"
#include "GraphEditorExperimentHud.h"
#include "MovieHud.h"
#include "../Movie/CameraPath.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/FollowCamera.h"
#include "../Movie/MirvCosmetics.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../ClientEntitySystem.h" // AfxGetLocalObserverState (spectator gating for Customize)
#include "../../MirvTime.h"
#include "../../SchemaSystem.h"
#include "../../ViewportScaler.h" // AfxViewportScaler scaled-preview bridge
#include "../../WrpConsole.h" // advancedfx::Message (debug-overlay-gated [vpscale] diagnostics)

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Bounded recursive id search (same approach as CameraTimelineHud / MovieHud).
void* FindChildById(void* panel, const char* id, int depth = 0) {
	if (!panel || depth > 64)
		return nullptr;
	unsigned char* childrenField = (unsigned char*)panel + CS2::PanoramaUIPanel::children;
	const int count = *(int*)childrenField;
	void** arr = *(void***)(childrenField + 8);
	if (!arr || count <= 0 || count > 100000)
		return nullptr;
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		char* cid = *(char**)((unsigned char*)child + CS2::PanoramaUIPanel::panelId);
		if (cid && 0 == std::strcmp(cid, id))
			return child;
	}
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		if (void* found = FindChildById(child, id, depth + 1))
			return found;
	}
	return nullptr;
}

double r2(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0; // NaN/inf -> keep JSON valid
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0;
}

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			// A PAUSED demo is still an active demo: IsPlayingDemo() flips to false the
			// moment the demo is paused (e.g. SPACE). Gating the editor on it alone made
			// the whole workspace force-exit (OnExit) on every pause, which also dropped
			// the input gating and let scroll/click/ESC leak to the game. Keep paused = up.
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

std::string JsonEscape(const char* value) {
	std::string out;
	if (!value) return out;
	for (const unsigned char* p = (const unsigned char*)value; *p; ++p) {
		switch (*p) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (*p >= 0x20) out += (char)*p;
			break;
		}
	}
	return out;
}

CEntityInstance* EntityFromIndex(int index) {
	if (index < 0 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

int ActiveWeaponDefIndex(CEntityInstance* pawn) {
	if (!pawn || !pawn->IsPlayerPawn() || !g_cosmeticsOffsetsOk)
		return 0;
	SOURCESDK::CS2::CBaseHandle weaponHandle = pawn->GetActiveWeaponHandle();
	if (!weaponHandle.IsValid())
		return 0;
	CEntityInstance* weapon = EntityFromIndex(weaponHandle.GetEntryIndex());
	if (!weapon)
		return 0;
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + offsets.C_EconEntity.m_AttributeManager + offsets.C_AttributeContainer.m_Item;
	return (int)*(uint16_t*)(itemView + offsets.C_EconItemView.m_iItemDefinitionIndex);
}

enum class CustomizeWeaponSlot {
	None,
	Primary,
	Secondary,
	Knife
};

struct CustomizeWeaponInfo {
	int entityIndex = -1;
	int defIndex = 0;
	int paintKit = 0;
	float wear = 0.0f;
};

constexpr int kPaintKitAttributeId = 6;
constexpr int kPaintWearAttributeId = 8;

bool LooksLikePointer(const void* value) {
	uintptr_t p = (uintptr_t)value;
	return p > 0x10000;
}

struct AttributeVectorView {
	unsigned char* data = nullptr;
	int count = 0;
	int stride = 0;
};

bool TryMakeAttributeVector(unsigned char* vectorField, AttributeVectorView& out) {
	out = AttributeVectorView{};
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	int stride = (int)offsets.CEconItemAttribute.m_size;
	int minStride = (int)offsets.CEconItemAttribute.m_flValue + (int)sizeof(float);
	if (stride < minStride)
		stride = minStride;
	if (!vectorField || stride <= 0)
		return false;

	// C_UtlVectorEmbeddedNetworkVar<T> in networked fields is observed as count + pointer,
	// while standard SDK CUtlVector<T> stores pointer first and count at +0x10. Accept either
	// so cosmetic reads degrade cleanly across CS2 layout changes.
	int countA = *(int*)vectorField;
	unsigned char* dataA = *(unsigned char**)(vectorField + 8);
	if (countA > 0 && countA <= 128 && LooksLikePointer(dataA)) {
		out.data = dataA;
		out.count = countA;
		out.stride = stride;
		return true;
	}

	unsigned char* dataB = *(unsigned char**)vectorField;
	int countB = *(int*)(vectorField + 16);
	if (countB > 0 && countB <= 128 && LooksLikePointer(dataB)) {
		out.data = dataB;
		out.count = countB;
		out.stride = stride;
		return true;
	}

	return false;
}

bool TryReadItemAttributeFloat(unsigned char* itemView, int attributeId, float& out) {
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	if (!itemView
		|| offsets.C_EconItemView.m_AttributeList == 0
		|| offsets.C_AttributeList.m_Attributes == 0
		|| offsets.CEconItemAttribute.m_iAttributeDefinitionIndex == 0
		|| offsets.CEconItemAttribute.m_flValue == 0)
		return false;

	unsigned char* attributeList = itemView + offsets.C_EconItemView.m_AttributeList;
	unsigned char* vectorField = attributeList + offsets.C_AttributeList.m_Attributes;
	AttributeVectorView vec;
	if (!TryMakeAttributeVector(vectorField, vec))
		return false;

	const ptrdiff_t defOffset = offsets.CEconItemAttribute.m_iAttributeDefinitionIndex;
	const ptrdiff_t valueOffset = offsets.CEconItemAttribute.m_flValue;
	__try {
		for (int i = 0; i < vec.count; ++i) {
			unsigned char* attr = vec.data + (ptrdiff_t)i * vec.stride;
			int def = (int)*(uint16_t*)(attr + defOffset);
			if (def == attributeId) {
				out = *(float*)(attr + valueOffset);
				return true;
			}
		}
	} __except (1) {
		return false;
	}
	return false;
}

int ReadItemAttributeInt(unsigned char* itemView, int attributeId, int fallback) {
	float value = 0.0f;
	if (!TryReadItemAttributeFloat(itemView, attributeId, value))
		return fallback;
	return (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
}

float ReadItemAttributeFloat(unsigned char* itemView, int attributeId, float fallback) {
	float value = 0.0f;
	if (!TryReadItemAttributeFloat(itemView, attributeId, value))
		return fallback;
	return value;
}

CustomizeWeaponSlot SlotForWeaponDef(int defIndex) {
	switch (defIndex) {
	case 1: case 2: case 3: case 4: case 30: case 32: case 36: case 61: case 63: case 64:
		return CustomizeWeaponSlot::Secondary;
	case 7: case 8: case 9: case 10: case 11: case 13: case 14: case 16: case 17: case 19:
	case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 33: case 34: case 35:
	case 38: case 39: case 40: case 60:
		return CustomizeWeaponSlot::Primary;
	case 42: case 59: case 500: case 503: case 505: case 506: case 507: case 508: case 509:
	case 512: case 514: case 515: case 516: case 517: case 518: case 519: case 520: case 521:
	case 522: case 523: case 525: case 526:
		return CustomizeWeaponSlot::Knife;
	default:
		return CustomizeWeaponSlot::None;
	}
}

bool LooksLikeWeaponEntity(CEntityInstance* entity) {
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClass = entity ? entity->GetClientClassName() : nullptr;
	return (className && std::strstr(className, "weapon_"))
		|| (className && std::strstr(className, "Weapon"))
		|| (clientClass && std::strstr(clientClass, "Weapon"));
}

bool ReadWeaponInfo(int entityIndex, CustomizeWeaponInfo& out) {
	CEntityInstance* weapon = EntityFromIndex(entityIndex);
	if (!weapon || !LooksLikeWeaponEntity(weapon) || !g_cosmeticsOffsetsOk)
		return false;

	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + offsets.C_EconEntity.m_AttributeManager + offsets.C_AttributeContainer.m_Item;
	const int defIndex = (int)*(uint16_t*)(itemView + offsets.C_EconItemView.m_iItemDefinitionIndex);
	if (SlotForWeaponDef(defIndex) == CustomizeWeaponSlot::None)
		return false;

	out.entityIndex = entityIndex;
	out.defIndex = defIndex;
	out.paintKit = ReadItemAttributeInt(itemView, kPaintKitAttributeId, *(int32_t*)(w + offsets.C_EconEntity.m_nFallbackPaintKit));
	out.wear = ReadItemAttributeFloat(itemView, kPaintWearAttributeId, *(float*)(w + offsets.C_EconEntity.m_flFallbackWear));
	return true;
}

void AssignLoadoutSlot(CustomizeWeaponInfo slots[3], const CustomizeWeaponInfo& info) {
	switch (SlotForWeaponDef(info.defIndex)) {
	case CustomizeWeaponSlot::Primary:
		slots[0] = info;
		break;
	case CustomizeWeaponSlot::Secondary:
		slots[1] = info;
		break;
	case CustomizeWeaponSlot::Knife:
		slots[2] = info;
		break;
	default:
		break;
	}
}

void BuildLoadoutSlots(CEntityInstance* pawn, int pawnIndex, CustomizeWeaponInfo slots[3]) {
	if (!pawn)
		return;

	SOURCESDK::CS2::CBaseHandle activeWeaponHandle = pawn->GetActiveWeaponHandle();
	if (activeWeaponHandle.IsValid()) {
		CustomizeWeaponInfo active;
		if (ReadWeaponInfo(activeWeaponHandle.GetEntryIndex(), active))
			AssignLoadoutSlot(slots, active);
	}

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* entity = EntityFromIndex(i);
		if (!entity || !LooksLikeWeaponEntity(entity))
			continue;
		SOURCESDK::CS2::CBaseHandle owner = entity->GetOwnerEntityHandle();
		if (!owner.IsValid() || owner.GetEntryIndex() != pawnIndex)
			continue;
		CustomizeWeaponInfo info;
		if (ReadWeaponInfo(i, info))
			AssignLoadoutSlot(slots, info);
	}
}

// Reads the spectated player's equipped glove item view. Unlike weapons, gloves do not have
// C_EconEntity fallback fields on the pawn; their finish is stored as econ attributes on
// C_EconItemView.m_AttributeList (paint kit = 6, wear = 8).
bool ReadGloveInfo(CEntityInstance* pawn, CustomizeWeaponInfo& out) {
	out = CustomizeWeaponInfo{};
	if (!pawn || !g_cosmeticsOffsetsOk)
		return false;
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	if (offsets.C_CSPlayerPawn.m_EconGloves == 0)
		return false;
	unsigned char* p = (unsigned char*)pawn;
	unsigned char* gloveView = p + offsets.C_CSPlayerPawn.m_EconGloves;
	int defIndex = (int)*(uint16_t*)(gloveView + offsets.C_EconItemView.m_iItemDefinitionIndex);
	if (defIndex <= 0 || defIndex == 5028 || defIndex == 5029)
		return false;
	int paintKit = ReadItemAttributeInt(gloveView, kPaintKitAttributeId, -1);
	if (paintKit <= 0)
		return false;
	out.entityIndex = -1;
	out.defIndex = defIndex;
	out.paintKit = paintKit;
	out.wear = ReadItemAttributeFloat(gloveView, kPaintWearAttributeId, 0.22f);
	return true;
}

void WriteWeaponSlotJson(std::ostringstream& o, const char* name, const CustomizeWeaponInfo& info) {
	o << ",\"" << name << "\":";
	if (info.defIndex <= 0) {
		o << "null";
		return;
	}
	o << "{";
	o << "\"entityIndex\":" << info.entityIndex;
	o << ",\"defIndex\":" << info.defIndex;
	o << ",\"paintKit\":" << info.paintKit;
	o << ",\"wear\":" << r2(info.wear);
	o << "}";
}

std::string BuildCustomizeTargetJson(int pawnIndex) {
	CEntityInstance* pawn = EntityFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return "null";

	CEntityInstance* controller = nullptr;
	SOURCESDK::CS2::CBaseHandle controllerHandle = pawn->GetPlayerControllerHandle();
	if (controllerHandle.IsValid())
		controller = EntityFromIndex(controllerHandle.GetEntryIndex());

	const char* name = nullptr;
	uint64_t steamId = 0;
	if (controller && controller->IsPlayerController()) {
		name = controller->GetSanitizedPlayerName();
		if (!name || !*name) name = controller->GetPlayerName();
		steamId = controller->GetSteamId();
	}
	if (!name || !*name)
		name = "Player";

	int activeWeaponIndex = -1;
	SOURCESDK::CS2::CBaseHandle activeWeaponHandle = pawn->GetActiveWeaponHandle();
	if (activeWeaponHandle.IsValid())
		activeWeaponIndex = activeWeaponHandle.GetEntryIndex();

	CustomizeWeaponInfo loadout[3];
	BuildLoadoutSlots(pawn, pawnIndex, loadout);

	std::ostringstream o;
	o << "{";
	o << "\"pawnIndex\":" << pawnIndex;
	o << ",\"controllerIndex\":" << (controllerHandle.IsValid() ? controllerHandle.GetEntryIndex() : -1);
	o << ",\"key\":\"" << (steamId ? ("steam:" + std::to_string(steamId)) : ("pawn:" + std::to_string(pawnIndex))) << "\"";
	o << ",\"steamId\":\"" << (steamId ? std::to_string(steamId) : "") << "\"";
	o << ",\"name\":\"" << JsonEscape(name) << "\"";
	o << ",\"team\":" << pawn->GetTeam();
	o << ",\"activeWeaponIndex\":" << activeWeaponIndex;
	o << ",\"activeWeaponDefIndex\":" << ActiveWeaponDefIndex(pawn);
	o << ",\"weapons\":{";
	o << "\"primary\":";
	if (loadout[0].defIndex > 0) {
		o << "{\"entityIndex\":" << loadout[0].entityIndex << ",\"defIndex\":" << loadout[0].defIndex
			<< ",\"paintKit\":" << loadout[0].paintKit << ",\"wear\":" << r2(loadout[0].wear) << "}";
	} else {
		o << "null";
	}
	WriteWeaponSlotJson(o, "secondary", loadout[1]);
	WriteWeaponSlotJson(o, "knife", loadout[2]);
	CustomizeWeaponInfo gloveInfo;
	const bool hasGloves = ReadGloveInfo(pawn, gloveInfo);
	o << ",\"gloves\":";
	if (hasGloves)
		o << "{\"defIndex\":" << gloveInfo.defIndex << ",\"paintKit\":" << gloveInfo.paintKit << ",\"wear\":" << r2(gloveInfo.wear) << "}";
	else
		o << "null";
	o << "}";
	o << "}";
	return o.str();
}

std::string BuildCustomizePlayersJson() {
	std::ostringstream o;
	o << "{";
	bool first = true;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* entity = EntityFromIndex(i);
		if (!entity || !entity->IsPlayerPawn())
			continue;
		std::string target = BuildCustomizeTargetJson(i);
		if (target == "null")
			continue;
		if (!first) o << ",";
		first = false;
		o << "\"" << i << "\":" << target;
	}
	o << "}";
	return o.str();
}

} // namespace

CameraEditorHud& CameraEditorHudRef() {
	static CameraEditorHud s_instance;
	return s_instance;
}

void* CameraEditorHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "CamEditorRoot");
}

bool CameraEditorHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kCameraEditorJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symPreviewRect = m_bridge.MakeSymbol("previewrect");
	m_symDebugPanels = m_bridge.MakeSymbol("debugpanels");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void CameraEditorHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#CamEditorRoot'); if(e) e.DeleteAsync(0); var d=$('#CamEditorDebugRoot'); if(d) d.DeleteAsync(0); $.CamEditor=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
}

// Declared in Filmmaker.cpp.
bool GraphEditorExperiment_Enabled();
void GraphEditorExperiment_Set(bool enabled);
const char* CameraEditor_HudViewName();

// One-shot enter: default the bottom to the REGULAR (native CS2) timeline -- the familiar game
// demo bar, docked to fit left of the inspector (CameraTimelineJs), minus its CAM EDITOR/MOUSE
// buttons. The bottom tab bar switches to the camera timeline or graph. Host the custom timeline,
// hide the floating movie-director cards, select a key for the inspector. Free cam is NOT forced
// on -- the user toggles it explicitly (the editor must not hijack the camera on open).
void CameraEditorHud::OnEnter() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	m_prevMovieHud = MovieHudRef().Visible();
	MovieHudRef().SetVisible(false);

	tl.SetEditorHosted(true);
	tl.SetVisible(false); // default bottom mode is the native (Regular) timeline; camera timeline hidden
	tl.SetCursor(true); // start in UI-cursor so the inspector is immediately clickable

	m_bottomMode = BottomMode::Native;
	GraphEditorExperiment_Set(false);

	// Scale the live game into the preview rect by default -- that "shrunk viewport" IS the
	// point of the editor (vs. the full-screen crop). `mirv_filmmaker editor scale off`
	// reverts to the crop. Auto-disables itself while recording (engine-side check).
	m_scaleEnabled = true;

	// Do NOT hijack the camera on open. Opening the editor only shows the UI -- it must not
	// enable free cam, pause, seek, or jump to a camera. Pre-select a key for the inspector
	// read-out ONLY (no teleport), and only when nothing is already selected, so the current
	// view and camera mode are left exactly as they were. Jumping to / previewing / editing a
	// camera now requires an explicit action (nav arrows, preview, edit -> SelectForEditor).
	if (cp.Count() > 0 && cp.Selected() < 0) cp.SelectIndex(0, /*teleport*/ false);
}

// One-shot exit: restore everything the enter step changed and tear down the chrome.
void CameraEditorHud::OnExit() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	tl.SetEditorHosted(false);
	tl.SetVisible(false);
	GraphEditorExperiment_Set(false);
	FollowCameraRef().StopPreview("camera editor closed");
	cp.StopScrub();

	MovieHudRef().SetVisible(m_prevMovieHud);

	// Drop any cosmetic skin overrides so they don't persist (and possibly mis-apply by stale
	// entity index) after the editor is closed.
	Cosmetics_ClearAll();

	// Drop any pending scaled-preview blit so the next full-screen frame renders normally.
	AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);

	Teardown();
}

std::string CameraEditorHud::BuildStateJson() {
	CameraPath& cp = CameraPathRef();
	const std::vector<CamMarker>& mk = cp.Markers();
	const int n = (int)mk.size();
	const int sel = cp.Selected();
	const bool selValid = (sel >= 0 && sel < n);
	const CameraPath::Mode pathMode = cp.GetMode();
	const bool pathPlaying = cp.IsPlaying() || cp.PlaybackPending();
	const bool pathLive = pathPlaying || pathMode == CameraPath::Mode::PreviewPlaying;

	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);

	double camOrigin[3] = { 0,0,0 }, camAngles[3] = { 0,0,0 }, camFov = 0.0;
	CameraBridge_GetCurrentCamera(camOrigin, camAngles, camFov);

	CameraTimelineHud& tl = CameraTimelineHudRef();

	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_enabled ? "true" : "false");
	o << ",\"graphExp\":" << (GraphEditorExperiment_Enabled() ? "true" : "false");
	o << ",\"graphDrive\":" << (GraphEditorExperimentHudRef().Drive() ? "true" : "false");
	o << ",\"bottomMode\":\"" << (m_bottomMode == BottomMode::Graph ? "graph" :
		(m_bottomMode == BottomMode::CameraTimeline ? "camera" : "native")) << "\"";
	o << ",\"hudView\":\"" << CameraEditor_HudViewName() << "\""; // game-UI visibility picker
	o << ",\"debug\":" << (m_debugOverlay ? "true" : "false");
	if (m_debugOverlay) {
		// Render-layer numbers for the viewport debug overlay: the ACTUAL world-blit rect (px) +
		// the backbuffer/render-target size, so JS can compare them against the Panorama-side
		// preview rect and prove the custom viewport matches the game viewport 1:1.
		int bbW = 0, bbH = 0; float vx = 0, vy = 0, vw = 0, vh = 0;
		const bool blitRan = AfxViewportScaler::GetLastBlit(bbW, bbH, vx, vy, vw, vh);
		o << ",\"dbg\":{\"blitRan\":" << (blitRan ? "true" : "false")
			<< ",\"bbW\":" << bbW << ",\"bbH\":" << bbH
			<< ",\"vx\":" << r2(vx) << ",\"vy\":" << r2(vy) << ",\"vw\":" << r2(vw) << ",\"vh\":" << r2(vh)
			<< ",\"scaleReq\":" << (m_scaleEnabled ? "true" : "false")
			<< ",\"previewValid\":" << (m_previewValid ? "true" : "false");
		std::string panelsJson = "[]";
		if (m_root && m_symDebugPanels >= 0) {
			std::string rawPanels = m_bridge.GetAttributeString(m_root, m_symDebugPanels, "[]");
			if (!rawPanels.empty() && rawPanels[0] == '[')
				panelsJson = rawPanels;
		}
		o << ",\"panels\":" << panelsJson << "}";
	}
	o << ",\"cursor\":" << (tl.Cursor() ? "true" : "false");
	o << ",\"tick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	o << ",\"count\":" << n;
	o << ",\"selected\":" << sel;
	o << ",\"interp\":\"" << cp.InterpName() << "\"";
	o << ",\"timing\":\"" << cp.TimingName() << "\"";
	o << ",\"speedMode\":\"" << cp.SpeedModeName() << "\"";
	o << ",\"pathMode\":\"" << cp.ModeName() << "\"";
	o << ",\"pathPlaying\":" << (pathPlaying ? "true" : "false");
	o << ",\"pathLive\":" << (pathLive ? "true" : "false");
	o << ",\"timelineView\":\"" << (tl.View() == 1 ? "curve" : "timeline") << "\"";
	o << ",\"constSpeed\":" << r2(cp.ConstSpeed());
	o << ",\"freeCam\":" << (CameraBridge_GetFreeCamEnabled() ? "true" : "false");
	o << ",\"freeCamSpeed\":" << r2(CameraBridge_GetFreeCamSpeed());
	// Spectator state for the "Customize" gating + modal: obsMode 2 (in-eye/first) or 3
	// (chase/third) means we're watching a player; 4 (roaming) is freecam. obsTarget is the
	// spectated entity index (-1 if none) so the modal can identify whose loadout to edit.
	{
		int obsTargetIndex = -1;
		uint8_t obsMode = AfxGetLocalObserverState(&obsTargetIndex);
		o << ",\"obsMode\":" << (int)obsMode;
		o << ",\"obsTarget\":" << obsTargetIndex;
		o << ",\"customizeTarget\":" << BuildCustomizeTargetJson(obsTargetIndex);
		o << ",\"customizePlayers\":" << BuildCustomizePlayersJson();
	}
	o << ",\"cam\":{\"x\":" << r2(camOrigin[0]) << ",\"y\":" << r2(camOrigin[1]) << ",\"z\":" << r2(camOrigin[2])
		<< ",\"pitch\":" << r2(camAngles[0]) << ",\"yaw\":" << r2(camAngles[1]) << ",\"roll\":" << r2(camAngles[2])
		<< ",\"fov\":" << r2(camFov) << "}";
	o << ",\"follow\":" << FollowCameraRef().BuildStateJson();
	if (selValid) {
		const CamMarker& m = mk[sel];
		o << ",\"sel\":{\"tick\":" << m.tick
			<< ",\"x\":" << r2(m.x) << ",\"y\":" << r2(m.y) << ",\"z\":" << r2(m.z)
			<< ",\"pitch\":" << r2(m.pitch) << ",\"yaw\":" << r2(m.yaw) << ",\"roll\":" << r2(m.roll)
			<< ",\"fov\":" << r2(m.fov) << ",\"ease\":" << (int)m.ease << ",\"speedMul\":" << r2(m.speedMul)
			<< ",\"isLast\":" << ((sel == n - 1) ? "true" : "false") << "}";
	} else {
		o << ",\"sel\":null";
	}
	o << "}";
	return o.str();
}

void CameraEditorHud::RunFrame() {
	m_bridge.Init();

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;

	// Demo not playing (or HUD gone): force-exit editor mode cleanly so we never leave
	// the gameplay HUD hidden or the timeline orphaned.
	if (!hud) {
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		else AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	// Enter / exit edge transitions.
	if (m_enabled && !m_wasEnabled) { OnEnter(); m_wasEnabled = true; }
	else if (!m_enabled && m_wasEnabled) { OnExit(); m_wasEnabled = false; }

	if (!m_enabled && !m_debugOverlay) {
		AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	if (!m_enabled && m_debugOverlay) {
		if (!BuildIfNeeded()) {
			AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
			return;
		}
		m_root = FindRoot();
		if (!m_root) {
			m_built = false;
			AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
			return;
		}
		std::string state = BuildStateJson();
		if (state != m_lastState) {
			m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
			m_lastState = state;
		}
		m_bridge.RunScript("$.CamEditor && $.CamEditor.render();");
		AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	// While enabled, re-assert hosting every frame (cheap) so a stray timeline close or HUD
	// recreation can't leave the workspace half-torn-down. Native mode leaves CS2's own demo
	// timeline visible; custom camera timeline / graph are explicit bottom overlays.
	const bool useGraph = m_bottomMode == BottomMode::Graph;
	const bool useCameraTimeline = m_bottomMode == BottomMode::CameraTimeline;
	GraphEditorExperiment_Set(useGraph);
	CameraTimelineHudRef().SetEditorHosted(true);
	CameraTimelineHudRef().SetVisible(useCameraTimeline);

	if (!BuildIfNeeded()) {
		AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	// Live REPL (editor eval) in the panel context.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	m_root = FindRoot();
	if (!m_root) {
		m_built = false;
		AfxViewportScaler::SetRequest(false, 0, 0, 0, 0);
		return;
	}

	std::string state = BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	// Always render while enabled so the chrome re-asserts its layout each frame. render()
	// (re)publishes the "previewrect" attribute -- the normalised preview-rect fractions --
	// as the single source of truth shared with the D3D blit.
	m_bridge.RunScript("$.CamEditor && $.CamEditor.render();");

	// TRUE scaled preview: forward the rect render() just published to the viewport scaler.
	// The blit only actually runs engine-side when not recording (full-screen capture wins).
	UpdateScaleRequest();
}

// Reads the "previewrect" fractions the editor JS published this frame and forwards them to
// the render-layer scaler. When scaling is off (or the rect is missing/degenerate) the request
// is cleared, so the preview falls back to the Panorama crop.
void CameraEditorHud::UpdateScaleRequest() {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::string pr;
	bool parsed = false, valid = false;

	// Read the published rect whenever the editor is open (cheap), independent of the scale
	// toggle, so diagnostics show the JS->C++ round-trip status even with scaling off.
	if (m_root) {
		pr = m_bridge.GetAttributeString(m_root, m_symPreviewRect, "");
		std::istringstream is(pr);
		parsed = (bool)(is >> x0 >> y0 >> x1 >> y1);
		valid = parsed && (x1 - x0 > 0.01f) && (y1 - y0 > 0.01f)
			&& x0 >= 0 && y0 >= 0 && x1 <= 1.0001f && y1 <= 1.0001f;
	}

	// Cache the rect so the timeline HUD can scale the native game HUD into the same region.
	m_previewValid = valid;
	if (valid) { m_previewX0 = x0; m_previewY0 = y0; m_previewX1 = x1; m_previewY1 = y1; }

	const bool active = m_scaleEnabled && valid;

	// Diagnostics: only while the debug overlay is on, and only when the decision inputs
	// CHANGE (no per-frame spam, silent in normal use).
	static int s_prev = -1;
	int now = (m_scaleEnabled ? 1 : 0) | (valid ? 2 : 0);
	if (now != s_prev) {
		s_prev = now;
		if (m_debugOverlay)
			advancedfx::Message("[vpscale] req: scaleOn=%d raw='%s' parsed=%d valid=%d\n",
				(int)m_scaleEnabled, pr.c_str(), (int)parsed, (int)valid);
	}

	AfxViewportScaler::SetRequest(active, x0, y0, x1, y1);
}

} // namespace Filmmaker
