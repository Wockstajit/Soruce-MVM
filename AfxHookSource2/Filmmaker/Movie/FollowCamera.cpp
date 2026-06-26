#include "FollowCamera.h"

#include "CameraBridge.h"
#include "CameraPath.h"
#include "../Filmmaker.h"
#include "../Panorama/CameraTimelineHud.h"
#include "../Panorama/GraphEditorExperimentHud.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../Demo/DemoInfoHelper.h"
#include "../../../shared/AfxConsole.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

constexpr double kMediumDistance = 1800.0;
constexpr double kFarDistance = 3500.0;
constexpr double kVeryFarDistance = 7000.0;
constexpr double kTeleportDistance = 1800.0;

CEntityInstance* EntityAt(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

std::string EntityClass(CEntityInstance* entity) {
	if (!entity) return "";
	const char* client = entity->GetClientClassName();
	if (client && *client) return client;
	const char* cls = entity->GetClassName();
	return cls ? cls : "";
}

bool IsFiniteOrigin(const FollowVec3& p) {
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
		&& std::fabs(p.x) < FLT_MAX && std::fabs(p.y) < FLT_MAX && std::fabs(p.z) < FLT_MAX;
}

bool ReadOrigin(CEntityInstance* entity, FollowVec3& out) {
	if (!entity) return false;
	float x = FLT_MAX, y = FLT_MAX, z = FLT_MAX;
	entity->GetOrigin(x, y, z);
	out = FollowVec3{ x, y, z };
	return IsFiniteOrigin(out);
}

bool IsGrenadeClass(const std::string& className) {
	const std::string s = Lower(className);
	return s.find("projectile") != std::string::npos
		&& (s.find("grenade") != std::string::npos || s.find("flashbang") != std::string::npos
			|| s.find("molotov") != std::string::npos || s.find("incendiary") != std::string::npos
			|| s.find("decoy") != std::string::npos);
}

bool IsBombClass(const std::string& className) {
	const std::string s = Lower(className);
	return s == "c_c4" || s.find("plantedc4") != std::string::npos
		|| s.find("weapon_c4") != std::string::npos;
}

bool IsWeaponClass(const std::string& className) {
	const std::string s = Lower(className);
	if (IsBombClass(s) || IsGrenadeClass(s)) return false;
	return s.find("weapon") != std::string::npos
		|| s.find("ak47") != std::string::npos || s.find("deagle") != std::string::npos;
}

bool IsDroppedWeapon(CEntityInstance* entity, const std::string& className) {
	return entity && IsWeaponClass(className) && !entity->GetOwnerEntityHandle().IsValid();
}

bool IsWeaponOrBomb(const std::string& className) {
	return IsWeaponClass(className) || IsBombClass(className);
}

std::wstring Utf8ToWide(const std::string& s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	if (n <= 0) return std::wstring();
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
	return out;
}

std::string GrenadeTypeName(const std::string& className) {
	const std::string s = Lower(className);
	if (s.find("flash") != std::string::npos) return "Flashbang";
	if (s.find("smoke") != std::string::npos) return "Smoke";
	if (s.find("hegrenade") != std::string::npos || s.find("frag") != std::string::npos) return "HE Grenade";
	if (s.find("molotov") != std::string::npos) return "Molotov";
	if (s.find("incendiary") != std::string::npos) return "Incendiary";
	if (s.find("decoy") != std::string::npos) return "Decoy";
	return "Grenade";
}

// Friendly weapon name from an engine class (C_AK47, C_WeaponM4A1, ...) so the UI
// reads "AK-47" / "M4A4" / "M4A1-S" instead of the raw class. Order matters: test the
// more specific variants (silencer) before the base token they contain.
std::string PrettyWeaponName(const std::string& className) {
	const std::string s = Lower(className);
	auto has = [&](const char* t) { return s.find(t) != std::string::npos; };
	if (has("m4a1_silencer") || has("m4a1s")) return "M4A1-S";
	if (has("m4a1") || has("m4a4"))           return "M4A4";
	if (has("usp"))                            return "USP-S";
	if (has("hkp2000") || has("p2000"))        return "P2000";
	if (has("ak47"))                           return "AK-47";
	if (has("awp"))                            return "AWP";
	if (has("ssg08"))                          return "SSG 08";
	if (has("scar20"))                         return "SCAR-20";
	if (has("g3sg1"))                          return "G3SG1";
	if (has("sg556") || has("sg553"))          return "SG 553";
	if (has("aug"))                            return "AUG";
	if (has("galil"))                          return "Galil AR";
	if (has("famas"))                          return "FAMAS";
	if (has("deagle") || has("deserteagle"))   return "Desert Eagle";
	if (has("revolver"))                       return "R8 Revolver";
	if (has("elite"))                          return "Dual Berettas";
	if (has("fiveseven"))                      return "Five-SeveN";
	if (has("glock"))                          return "Glock-18";
	if (has("p250"))                           return "P250";
	if (has("cz75"))                           return "CZ75-Auto";
	if (has("tec9"))                           return "Tec-9";
	if (has("p90"))                            return "P90";
	if (has("bizon"))                          return "PP-Bizon";
	if (has("mac10"))                          return "MAC-10";
	if (has("mp5sd"))                          return "MP5-SD";
	if (has("mp7"))                            return "MP7";
	if (has("mp9"))                            return "MP9";
	if (has("ump"))                            return "UMP-45";
	if (has("nova"))                           return "Nova";
	if (has("xm1014"))                         return "XM1014";
	if (has("sawedoff"))                       return "Sawed-Off";
	if (has("mag7"))                           return "MAG-7";
	if (has("m249"))                           return "M249";
	if (has("negev"))                          return "Negev";
	if (has("taser") || has("zeus"))           return "Zeus x27";
	if (has("knife") || has("bayonet"))        return "Knife";
	if (has("c4"))                             return "C4";
	// Fallback: drop the engine prefix and show whatever class remains.
	std::string base = className;
	if (s.rfind("c_weapon", 0) == 0)      base = className.substr(8);
	else if (s.rfind("c_", 0) == 0)       base = className.substr(2);
	return base.empty() ? "Weapon" : base;
}

// Human-readable name for any followed entity: the player's name for a pawn, a friendly
// weapon/grenade name otherwise. Used for the "Target:" label so it reads "jOAO" or
// "AK-47" instead of "CSPlayerPawn" / "C_AK47".
std::string EntityFriendlyName(CEntityInstance* entity) {
	if (!entity) return "";
	if (entity->IsPlayerPawn()) {
		const auto ch = entity->GetPlayerControllerHandle();
		CEntityInstance* controller = ch.IsValid() ? EntityAt(ch.GetEntryIndex()) : nullptr;
		const char* name = controller ? controller->GetPlayerName() : nullptr;
		return (name && *name) ? name : "Player";
	}
	const std::string cls = EntityClass(entity);
	if (cls.empty()) return "Entity";
	if (IsBombClass(cls)) return "C4";
	if (IsGrenadeClass(cls)) return GrenadeTypeName(cls);
	if (IsWeaponClass(cls)) return PrettyWeaponName(cls);
	return cls;
}

void ResolveOwner(CEntityInstance* entity, int& ownerIndex, std::string& ownerName, int& ownerTeam) {
	ownerIndex = -1;
	ownerName.clear();
	ownerTeam = 0;
	if (!entity) return;
	const auto ownerHandle = entity->GetOwnerEntityHandle();
	if (!ownerHandle.IsValid()) return;
	ownerIndex = ownerHandle.GetEntryIndex();
	CEntityInstance* owner = EntityAt(ownerIndex);
	if (!owner) return;
	ownerTeam = owner->GetTeam();
	CEntityInstance* controller = owner;
	if (owner->IsPlayerPawn()) {
		const auto controllerHandle = owner->GetPlayerControllerHandle();
		controller = controllerHandle.IsValid() ? EntityAt(controllerHandle.GetEntryIndex()) : nullptr;
	}
	if (controller && controller->IsPlayerController()) {
		const char* name = controller->GetPlayerName();
		if (name && *name) ownerName = name;
	}
	if (ownerName.empty())
		ownerName = owner->IsPlayerPawn() ? "Player pawn" : "Entity";
}

uint64_t EntityHandleValue(CEntityInstance* entity) {
	if (!entity) return 0;
	const auto h = entity->GetHandle();
	return h.IsValid() ? (uint64_t)(uint32_t)h.ToInt() : 0;
}

FollowAttachPoint MakeAttachPoint(const char* id, const char* label, const char* kind,
	bool valid, int index, const char* source) {
	FollowAttachPoint out;
	out.id = id ? id : "";
	out.label = label ? label : out.id;
	out.kind = kind ? kind : "";
	out.valid = valid;
	out.index = index;
	out.source = source ? source : "";
	return out;
}

void AddAttachmentIfValid(std::vector<FollowAttachPoint>& out, CEntityInstance* entity,
	const char* id, const char* label, const char* kind) {
	if (!entity || !id || !*id) return;
	const uint8_t idx = entity->LookupAttachment(id);
	if (idx != 0)
		out.push_back(MakeAttachPoint(id, label, kind, true, idx, "model-attachment"));
}

std::vector<FollowAttachPoint> AvailableAttachPoints(CEntityInstance* entity, FollowTargetType type, bool weapon, bool bomb) {
	std::vector<FollowAttachPoint> out;
	if (!entity) return out;
	out.push_back(MakeAttachPoint("entity", "Entity", "entity", true, 0, "entity-origin"));

	if (type == FollowTargetType::Player) {
		out.push_back(MakeAttachPoint("eyes", "Eyes", "virtual", true, -1, "player-render-eye"));
		out.push_back(MakeAttachPoint("head", "Head", "virtual", true, -1, "player-virtual"));
		out.push_back(MakeAttachPoint("chest", "Chest", "virtual", true, -1, "player-virtual"));
		out.push_back(MakeAttachPoint("pelvis", "Pelvis", "virtual", true, -1, "player-virtual"));
		out.push_back(MakeAttachPoint("feet", "Feet", "virtual", true, -1, "player-virtual"));
		AddAttachmentIfValid(out, entity, "weapon_hand_R", "Right hand", "attachment");
		AddAttachmentIfValid(out, entity, "weapon_hand_L", "Left hand", "attachment");
		return out;
	}

	if (type == FollowTargetType::Grenade || bomb)
		return out;

	if (weapon) {
		AddAttachmentIfValid(out, entity, "muzzle", "Muzzle", "attachment");
		AddAttachmentIfValid(out, entity, "muzzle_flash", "Muzzle flash", "attachment");
		AddAttachmentIfValid(out, entity, "shell_eject", "Shell eject", "attachment");
		AddAttachmentIfValid(out, entity, "eject", "Eject", "attachment");
		AddAttachmentIfValid(out, entity, "magazine", "Magazine", "attachment");
		AddAttachmentIfValid(out, entity, "mag", "Magazine", "attachment");
		AddAttachmentIfValid(out, entity, "clip", "Magazine", "attachment");
		AddAttachmentIfValid(out, entity, "bolt", "Bolt", "attachment");
		AddAttachmentIfValid(out, entity, "slide", "Slide", "attachment");
	}
	return out;
}

std::vector<std::string> AttachPointIds(const std::vector<FollowAttachPoint>& points) {
	std::vector<std::string> out;
	out.reserve(points.size());
	for (const auto& point : points)
		if (point.valid)
			out.push_back(point.id);
	return out;
}

struct AttachTransformResult {
	bool valid = false;
	bool oriented = false;
	int entityIndex = -1;
	uint64_t entityHandle = 0;
	std::string entityType;
	std::string className;
	std::string modelName;
	std::string attachId;
	std::string attachKind;
	int attachIndex = -1;
	std::string source;
	FollowVec3 pos;
	FollowAngles ang;
};

bool ResolvePlayerVirtualPoint(CEntityInstance* pawn, const std::string& id,
	AttachTransformResult& out) {
	if (!pawn || !pawn->IsPlayerPawn()) return false;
	FollowVec3 origin;
	if (!ReadOrigin(pawn, origin)) return false;
	float eye[3] = {};
	float a[3] = {};
	pawn->GetRenderEyeOrigin(eye);
	pawn->GetRenderEyeAngles(a);
	const FollowVec3 eyes{ eye[0], eye[1], eye[2] };
	const FollowAngles eyeAngles{ a[0], a[1], a[2] };
	out.valid = true;
	out.oriented = true;
	out.attachKind = "virtual";
	out.attachIndex = -1;
	out.source = id == "eyes" ? "player-render-eye" : "player-virtual";
	out.ang = eyeAngles;
	if (id == "eyes") {
		out.pos = eyes;
	} else if (id == "head") {
		out.pos = FollowVec3{ eyes.x, eyes.y, eyes.z - 3.0 };
	} else if (id == "chest") {
		out.pos = FollowVec3{
			origin.x + (eyes.x - origin.x) * 0.55,
			origin.y + (eyes.y - origin.y) * 0.55,
			origin.z + (eyes.z - origin.z) * 0.55
		};
		out.ang.pitch = 0.0;
	} else if (id == "pelvis") {
		out.pos = FollowVec3{
			origin.x + (eyes.x - origin.x) * 0.25,
			origin.y + (eyes.y - origin.y) * 0.25,
			origin.z + (eyes.z - origin.z) * 0.25
		};
		out.ang.pitch = 0.0;
	} else if (id == "feet") {
		out.pos = origin;
		out.ang.pitch = 0.0;
	} else {
		return false;
	}
	return true;
}

AttachTransformResult ResolveAttachTransform(CEntityInstance* entity, int entityIndex,
	FollowTargetType type, const std::string& requestedAttachId) {
	AttachTransformResult out;
	out.entityIndex = entityIndex;
	out.entityHandle = EntityHandleValue(entity);
	out.entityType = FollowTargetTypeName(type);
	out.className = EntityClass(entity);
	out.attachId = requestedAttachId.empty() ? "entity" : requestedAttachId;
	if (!entity) {
		out.source = "missing-entity";
		return out;
	}

	if (type == FollowTargetType::Player && ResolvePlayerVirtualPoint(entity, out.attachId, out))
		return out;

	if (out.attachId != "entity") {
		const uint8_t idx = entity->LookupAttachment(out.attachId.c_str());
		SOURCESDK::Vector origin;
		SOURCESDK::Quaternion q;
		if (idx && entity->GetAttachment(idx, origin, q)) {
			out.valid = true;
			out.oriented = true;
			out.attachKind = "attachment";
			out.attachIndex = idx;
			out.source = "model-attachment";
			out.pos = FollowVec3{ origin.x, origin.y, origin.z };
			out.ang = FollowQuatToAngles(q.x, q.y, q.z, q.w);
			return out;
		}
	}

	out.attachKind = "entity";
	out.attachIndex = 0;
	out.source = out.attachId == "entity" ? "entity-origin" : "fallback-entity-origin";
	out.valid = ReadOrigin(entity, out.pos);
	out.oriented = false;
	return out;
}

std::string JsonEscape(const std::string& value) {
	std::ostringstream out;
	for (unsigned char c : value) {
		switch (c) {
		case '\\': out << "\\\\"; break;
		case '"': out << "\\\""; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (c < 0x20) out << '?';
			else out << (char)c;
		}
	}
	return out.str();
}

void AppendAttachPointJson(std::ostringstream& o, const FollowAttachPoint& point) {
	o << "{\"id\":\"" << JsonEscape(point.id) << "\""
		<< ",\"label\":\"" << JsonEscape(point.label) << "\""
		<< ",\"kind\":\"" << JsonEscape(point.kind) << "\""
		<< ",\"valid\":" << (point.valid ? "true" : "false")
		<< ",\"index\":" << point.index
		<< ",\"source\":\"" << JsonEscape(point.source) << "\"}";
}

const char* TeamName(int team) {
	return team == 2 ? "T" : (team == 3 ? "CT" : "-");
}

class PlayerTargetProvider final : public IFollowTargetProvider {
public:
	explicit PlayerTargetProvider(int index) : m_index(index) {}
	bool IsValid() override {
		CEntityInstance* pawn = EntityAt(m_index);
		return pawn && pawn->IsPlayerPawn() && pawn->GetHealth() > 0;
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 out;
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn) return out;
		float eye[3] = {};
		pawn->GetRenderEyeOrigin(eye);
		out = FollowVec3{ eye[0], eye[1], eye[2] };
		return out;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn) { pos = {}; return false; }
		float eye[3] = {};
		pawn->GetRenderEyeOrigin(eye);
		pos = FollowVec3{ eye[0], eye[1], eye[2] };
		float a[3] = {};
		pawn->GetRenderEyeAngles(a);
		ang = FollowAngles{ a[0], a[1], a[2] }; // pitch, yaw, roll
		return true;
	}
	std::string GetDisplayName() override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn) return "Missing player";
		auto controllerHandle = pawn->GetPlayerControllerHandle();
		CEntityInstance* controller = controllerHandle.IsValid() ? EntityAt(controllerHandle.GetEntryIndex()) : nullptr;
		const char* name = controller ? controller->GetPlayerName() : nullptr;
		return name && *name ? name : "Player";
	}
	FollowTargetStatus GetStatus() override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn || !pawn->IsPlayerPawn()) return FollowTargetStatus::Missing;
		return pawn->GetHealth() > 0 ? FollowTargetStatus::Alive : FollowTargetStatus::Dead;
	}
	int EntityIndex() const override { return m_index; }
private:
	int m_index;
};

class EntityTargetProvider : public IFollowTargetProvider {
public:
	EntityTargetProvider(int index, FollowTargetType type) : m_index(index), m_type(type) {}
	bool IsValid() override {
		CEntityInstance* entity = EntityAt(m_index);
		if (!entity) return false;
		const std::string cls = EntityClass(entity);
		switch (m_type) {
		case FollowTargetType::Grenade: return IsGrenadeClass(cls);
		case FollowTargetType::Weapon: return IsWeaponOrBomb(cls);
		default: return true;
		}
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 out;
		ReadOrigin(EntityAt(m_index), out);
		return out;
	}
	std::string GetDisplayName() override {
		const std::string cls = EntityClass(EntityAt(m_index));
		if (cls.empty()) return "Entity";
		if (m_type == FollowTargetType::Grenade) return GrenadeTypeName(cls);
		return PrettyWeaponName(cls);
	}
	FollowTargetStatus GetStatus() override {
		return IsValid() ? FollowTargetStatus::Active : FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return m_index; }
protected:
	int m_index;
	FollowTargetType m_type;
};

class AttachmentTargetProvider final : public EntityTargetProvider {
public:
	AttachmentTargetProvider(int index, FollowTargetType type, const std::string& attachment)
		: EntityTargetProvider(index, type), m_attachment(attachment) {}
	// Attach rides whatever entity is selected (a player pawn OR a weapon), so validity
	// is simply "the entity still exists" -- NOT a weapon-class check (that broke Player
	// Bone attach) and NOT a strict attachment lookup (world weapon models often lack
	// muzzle/shell points, which made Live Preview bail with "target disappeared"). The
	// exact point is resolved in GetWorldTransform, with an origin fallback.
	bool IsValid() override { return EntityAt(m_index) != nullptr; }
	FollowVec3 GetWorldPosition() override {
		FollowVec3 pos; FollowAngles ang;
		GetWorldTransform(pos, ang);
		return pos;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		AttachTransformResult resolved = ResolveAttachTransform(EntityAt(m_index), m_index, m_type, m_attachment);
		pos = resolved.pos;
		ang = resolved.ang;
		return resolved.valid && resolved.oriented;
	}
	// Just the entity name (player or weapon); the attach point is shown separately in the
	// "Attach pt" row, so appending "/ head" here only made the Target label noisy.
	std::string GetDisplayName() override {
		return EntityFriendlyName(EntityAt(m_index));
	}
private:
	std::string m_attachment;
};

// Follows a player's CURRENTLY HELD weapon: re-resolves the active weapon each frame
// so the camera tracks through weapon switches. With an attachment name it reads that
// point (muzzle/etc.) on the live weapon; otherwise the weapon's origin.
class HeldWeaponProvider final : public IFollowTargetProvider {
public:
	HeldWeaponProvider(int playerIndex, std::string attachment)
		: m_playerIndex(playerIndex), m_attachment(std::move(attachment)) {}
	int ActiveWeaponIndex() const {
		CEntityInstance* pawn = EntityAt(m_playerIndex);
		if (!pawn || !pawn->IsPlayerPawn()) return -1;
		const auto weapon = pawn->GetActiveWeaponHandle();
		return weapon.IsValid() ? weapon.GetEntryIndex() : -1;
	}
	bool IsValid() override {
		const int wi = ActiveWeaponIndex();
		if (wi < 0) return false;
		CEntityInstance* weapon = EntityAt(wi);
		if (!weapon) return false;
		// Attachment is optional: GetWorldTransform rides the weapon origin when the named
		// point (muzzle/etc.) isn't on the model, so don't invalidate on a missing point.
		return true;
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 pos; FollowAngles ang;
		GetWorldTransform(pos, ang);
		return pos;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		pos = {};
		const int wi = ActiveWeaponIndex();
		AttachTransformResult resolved = ResolveAttachTransform(EntityAt(wi), wi, FollowTargetType::Weapon, m_attachment);
		pos = resolved.pos;
		ang = resolved.ang;
		return resolved.valid && resolved.oriented;
	}
	std::string GetDisplayName() override {
		CEntityInstance* weapon = EntityAt(ActiveWeaponIndex());
		const std::string cls = EntityClass(weapon);
		std::string base = cls.empty() ? "Held weapon" : PrettyWeaponName(cls);
		return m_attachment.empty() ? base : (base + " / " + m_attachment);
	}
	FollowTargetStatus GetStatus() override {
		CEntityInstance* pawn = EntityAt(m_playerIndex);
		if (!pawn || !pawn->IsPlayerPawn()) return FollowTargetStatus::Missing;
		if (pawn->GetHealth() <= 0) return FollowTargetStatus::Dead;
		return IsValid() ? FollowTargetStatus::Active : FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return ActiveWeaponIndex(); }
private:
	int m_playerIndex;
	std::string m_attachment;
};

} // namespace

const char* FollowTargetTypeName(FollowTargetType type) {
	switch (type) {
	case FollowTargetType::Grenade: return "Grenade";
	case FollowTargetType::Weapon: return "Weapon / C4";
	default: return "Player";
	}
}

FollowCamera& FollowCameraRef() {
	static FollowCamera instance;
	return instance;
}

double FollowCamera::WallDt() {
	LARGE_INTEGER frequency, now;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&now);
	if (!m_lastQpc) m_lastQpc = now.QuadPart;
	double dt = (double)(now.QuadPart - m_lastQpc) / (double)frequency.QuadPart;
	m_lastQpc = now.QuadPart;
	if (dt < 0.0) dt = 0.0;
	if (dt > 0.1) dt = 0.1;
	return dt;
}

void FollowCamera::ResetMotion() {
	m_motionInitialized = false;
	m_haveLastRawTarget = false;
	m_retargeting = false;
	m_retargetElapsed = 0.0;
	m_deathRetargetElapsed = 0.0;
	m_lastQpc = 0;
	m_attachDebug = AttachDebugSnapshot();
	m_attachRebaseValid = false;
	m_attachBaseOriented = false;
	m_attachOffsetIsLocal = false;
	m_havePrevAttachFresh = false;
	m_havePrevDebug = false;
}

void FollowCamera::PlaceCamera() {
	const bool replaced = m_state.hasCamera;
	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	m_state.cameraPosition = FollowVec3{ pos[0], pos[1], pos[2] };
	m_state.cameraAngles = FollowAngles{ ang[0], ang[1], ang[2] };
	m_state.fov = fov;
	m_state.hasCamera = true;
	m_repositioning = false;
	ResetMotion();
	// Include the tick so each Place produces a DISTINCT status line -- the on-screen text
	// changes every time, making a registered click visibly distinguishable from a no-op.
	int placeTick = 0; g_MirvTime.GetCurrentDemoTick(placeTick);
	m_lastMessage = (replaced ? "Follow Camera replaced @ tick " : "Follow Camera placed @ tick ")
		+ std::to_string(placeTick) + ".";
	advancedfx::Message("[followcam] single camera %s pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) fov=%.1f.\n",
		replaced ? "replaced" : "placed",
		pos[0], pos[1], pos[2], ang[0], ang[1], ang[2], fov);
}

void FollowCamera::BeginReposition() {
	if (m_state.mode == FollowMode::Attach) {
		const bool haveTarget = m_state.targetEntityIndex >= 0
			|| (m_state.targetType == FollowTargetType::Weapon && m_state.weaponPlayerIndex >= 0)
			|| (m_state.targetType == FollowTargetType::Grenade && m_state.targetHandle);
		if (!haveTarget) {
			advancedfx::Warning("[followcam] attach reposition refused: select a target first.\n");
			m_lastMessage = "Select a target before repositioning attach.";
			return;
		}
	}
	m_resumePreviewAfterReposition = m_state.enabled && m_state.mode == FollowMode::Attach;
	StopPreview("reposition started");
	m_repositioning = true;
	CameraBridge_SetFreeCamEnabled(true);
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(false);
	if (m_state.mode == FollowMode::Attach) {
		m_lastMessage = "Move freecam, then left-click to save attach offset/rotation/FOV.";
		advancedfx::Message("[followcam] repositioning attach mount: move + left-click to save relative pose (X/Esc cancel).\n");
	} else {
		advancedfx::Message("[followcam] repositioning single camera: move + left-click to place (X/Esc cancel).\n");
	}
}

void FollowCamera::PlaceReposition() {
	if (!m_repositioning) return;
	if (m_state.mode == FollowMode::Attach) {
		const bool resumePreview = m_resumePreviewAfterReposition;
		if (!CaptureAttachPoseFromCurrentView())
			return;
		m_repositioning = false;
		m_resumePreviewAfterReposition = false;
		if (CameraEditor_Active())
			CameraEditor_SetCursorMode(true);
		if (resumePreview)
			Preview();
		advancedfx::Message("[followcam] attach reposition complete; editor mouse restored.\n");
		return;
	}
	PlaceCamera();
	m_resumePreviewAfterReposition = false;
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[followcam] reposition complete; editor mouse restored.\n");
}

void FollowCamera::CancelReposition() {
	if (!m_repositioning) return;
	m_repositioning = false;
	m_resumePreviewAfterReposition = false;
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[followcam] reposition cancelled; editor mouse restored.\n");
}

bool FollowCamera::CaptureAttachPoseFromCurrentView() {
	if (m_state.mode != FollowMode::Attach) {
		advancedfx::Warning("[followcam] attach capture refused: switch to Attach mode first.\n");
		m_lastMessage = "Switch to Attach mode before saving a relative mount.";
		return false;
	}

	auto provider = MakeProvider();
	if (!provider || !provider->IsValid()) {
		advancedfx::Warning("[followcam] attach capture refused: selected target is not valid.\n");
		m_lastMessage = "Selected attach target is not valid.";
		return false;
	}

	FollowVec3 target;
	FollowAngles baseAngles;
	const bool oriented = provider->GetWorldTransform(target, baseAngles);
	if (!IsFiniteOrigin(target)) {
		advancedfx::Warning("[followcam] attach capture refused: target transform is invalid.\n");
		m_lastMessage = "Selected attach target transform is invalid.";
		return false;
	}

	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	const FollowVec3 camPos{ pos[0], pos[1], pos[2] };
	const FollowAngles camAngles{ ang[0], ang[1], ang[2] };
	if (!oriented)
		baseAngles = camAngles;

	const FollowVec3 worldOffset{
		camPos.x - target.x,
		camPos.y - target.y,
		camPos.z - target.z
	};
	m_state.offset = FollowInverseRotateVector(worldOffset, baseAngles);
	m_state.rotationOffset = FollowAngles{
		FollowWrapDegrees(camAngles.pitch - baseAngles.pitch),
		FollowWrapDegrees(camAngles.yaw - baseAngles.yaw),
		FollowWrapDegrees(camAngles.roll - baseAngles.roll)
	};
	m_state.fov = std::clamp(fov, 1.0, 170.0);
	m_state.cameraPosition = camPos;
	m_state.cameraAngles = baseAngles;
	m_baseAngles = baseAngles;
	ResetMotion();
	m_lastMessage = "Attach mount saved relative to '" + m_state.attachmentName + "'.";
	advancedfx::Message(
		"[followcam] attach mount saved: attach=%s offset=(%.2f %.2f %.2f) rotation=(%.2f %.2f %.2f) fov=%.1f.\n",
		m_state.attachmentName.c_str(),
		m_state.offset.x, m_state.offset.y, m_state.offset.z,
		m_state.rotationOffset.pitch, m_state.rotationOffset.yaw, m_state.rotationOffset.roll,
		m_state.fov);
	return true;
}

void FollowCamera::ClearCamera() {
	CancelReposition();
	StopPreview();
	m_state.hasCamera = false;
	m_state.targetEntityIndex = -1;
	m_state.targetHandle = 0;
	m_selectedGrenadeHandle = 0;
	m_grenadeTrackPending = false;
	m_haveLastKnownTarget = false;
	m_lastMessage = "Follow Camera cleared.";
	CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
	advancedfx::Message("[followcam] camera cleared.\n");
}

void FollowCamera::Preview() {
	if (m_state.mode == FollowMode::LockOn && !m_state.hasCamera) {
		advancedfx::Warning("[followcam] preview refused: place the follow camera first (lock-on mode).\n");
		m_lastMessage = "Place the camera first, or switch to Attach mode.";
		return;
	}
	const bool haveTarget = m_state.targetEntityIndex >= 0
		|| (m_state.targetType == FollowTargetType::Weapon && m_state.weaponPlayerIndex >= 0);
	if (!haveTarget) {
		advancedfx::Warning("[followcam] preview refused: select a target first.\n");
		m_lastMessage = "Select a target first.";
		return;
	}
	CameraPathRef().StopPreview();
	CameraPathRef().StopScrub();
	GraphEditorExperimentHudRef().SetDrive(false);
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetPathDrawEnabled(false);
	m_state.enabled = true;
	ResetMotion();
	advancedfx::Message("[followcam] preview started: type=%s entity=%d.\n",
		FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex);
}

void FollowCamera::StopPreview(const char* reason) {
	if (m_state.enabled && reason && *reason)
		advancedfx::Message("[followcam] preview stopped: %s.\n", reason);
	m_state.enabled = false;
	m_attachRebaseValid = false;     // don't let a stale attach pose drive the view after stop
	m_havePrevAttachFresh = false;
	m_grenadeTrackPending = false;
	m_grenadeSeekObserved = false;
	m_grenadeResumeIssued = false;
	if (reason && *reason)
		m_lastMessage = std::string("Tracking stopped: ") + reason + ".";
	if (CameraEditor_Active() && CameraTimelineHudRef().Cursor())
		GraphEditorExperimentHudRef().SetDrive(true);
	ResetMotion();
}

bool FollowCamera::ViewSetupAttachOverride(float curTime,
	double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov) {
	(void)curTime;
	if (!m_attachRebaseValid || !m_state.enabled || m_state.mode != FollowMode::Attach)
		return false;
	// Sample the target HERE (view setup = mid-render), where entities are interpolated, instead of
	// the tick-stepped value RunFrame read at FrameStageNotify.
	auto provider = MakeProvider();
	if (!provider || !provider->IsValid()) return false;
	FollowVec3 freshTarget; FollowAngles freshAng;
	const bool freshOriented = provider->GetWorldTransform(freshTarget, freshAng);
	if (!IsFiniteOrigin(freshTarget))
		return false;
	// Diagnostic: freshDelta is the frame-to-frame movement of the render-time sample. If the
	// render-time sample is interpolated this is a small, smoothly-varying value EVERY frame; if it
	// were still stepped it would read 0.000 then jump (the FrameStageNotify signature).
	const double freshDelta = m_havePrevAttachFresh ? FollowDistance(m_prevAttachFresh, freshTarget) : 0.0;
	const double steppedVsFresh = FollowDistance(m_attachSteppedTarget, freshTarget);
	m_prevAttachFresh = freshTarget; m_havePrevAttachFresh = true;
	if (m_debug && ((m_debugViewFrame++ % 15) == 0)) {
		advancedfx::Message("[followcam][rtsample] fresh=(%.2f %.2f %.2f) freshDelta=%.3f steppedVsFresh=%.3f apply=%d.\n",
			freshTarget.x, freshTarget.y, freshTarget.z, freshDelta, steppedVsFresh,
			m_renderTimeSample ? 1 : 0);
	}
	if (!m_renderTimeSample) return false;
	// Rebase the smoothed camera onto the interpolated target. For oriented attach points the
	// offset is stored in target-local axes, so it rotates with the same render-time bone pose the
	// visible model uses. A world-space offset from the stepped pose makes hand/weapon mounts look
	// stable in screenshots but swim/jitter in motion as the bone orientation interpolates.
	const FollowVec3 freshOffset = freshOriented && m_attachOffsetIsLocal
		? FollowRotateVector(m_attachOffsetFromTarget, freshAng)
		: m_attachOffsetFromTarget;
	x = freshTarget.x + freshOffset.x;
	y = freshTarget.y + freshOffset.y;
	z = freshTarget.z + freshOffset.z;
	const FollowAngles freshOutput = freshOriented && m_attachBaseOriented
		? FollowAddAngles(freshAng, m_attachAngleOffsetFromBase)
		: m_outputAngles;
	pitch = freshOutput.pitch;
	yaw = freshOutput.yaw;
	roll = freshOutput.roll;
	fov = m_state.fov;
	return true;
}

void FollowCamera::SetTargetType(FollowTargetType type) {
	if (m_state.targetType == type) return;
	m_state.targetType = type;
	m_state.targetEntityIndex = -1;
	m_state.weaponPlayerIndex = -1;
	m_state.targetHandle = 0;
	m_grenadeTrackPending = false;
	m_selectedEvent = -1;
	// Start from safe defaults. Candidate selection upgrades these to validated
	// model/player points (for example muzzle) only when the selected target exposes them.
	if (type == FollowTargetType::Weapon) m_state.attachmentName = "entity";
	else if (type == FollowTargetType::Grenade) m_state.attachmentName = "entity";
	else if (type == FollowTargetType::Player) m_state.attachmentName = "head";
	ResetMotion();
}

void FollowCamera::SetMode(FollowMode mode) {
	if (m_state.mode == mode) return;
	if (m_state.mode != FollowMode::Attach) {
		m_lockOnLookSmoothing = m_state.lookSmoothing;
		m_lockOnPositionSmoothing = m_state.positionSmoothing;
		m_lockOnPrediction = m_state.prediction;
		m_lockOnDeadzone = m_state.deadzone;
		m_lockOnMaxTurnSpeed = m_state.maxTurnSpeed;
	}
	m_state.mode = mode;
	if (mode == FollowMode::Attach) {
		m_state.lookSmoothing = 0.0;
		m_state.positionSmoothing = 0.0;
		m_state.prediction = 0.0;
		m_state.deadzone = 0.0;
		m_state.maxTurnSpeed = 0.0;
	} else {
		m_state.lookSmoothing = m_lockOnLookSmoothing;
		m_state.positionSmoothing = m_lockOnPositionSmoothing;
		m_state.prediction = m_lockOnPrediction;
		m_state.deadzone = m_lockOnDeadzone;
		m_state.maxTurnSpeed = m_lockOnMaxTurnSpeed;
	}
	if (mode == FollowMode::Attach
		&& std::fabs(m_state.offset.x) < 0.001
		&& std::fabs(m_state.offset.y) < 0.001
		&& std::fabs(m_state.offset.z) < 0.001) {
		m_state.offset = FollowVec3{ 72.0, 0.0, 8.0 };
	}
	ResetMotion();
}

void FollowCamera::SetWeaponSource(WeaponSource source) {
	if (m_state.weaponSource == source) return;
	m_state.weaponSource = source;
	ResetMotion();
}

void FollowCamera::UpdateGrenadeCache() const {
	for (auto& pair : m_grenadeCache)
		pair.second.active = false;

	int currentTick = 0;
	g_MirvTime.GetCurrentDemoTick(currentTick);
	float interval = g_MirvTime.interval_per_tick_get();
	if (!(interval > 0.00001f)) interval = 1.0f / 64.0f;
	const float currentGameTime = g_MirvTime.curtime_get();

	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity) continue;
		const std::string cls = EntityClass(entity);
		if (!IsGrenadeClass(cls)) continue;
		const auto entityHandle = entity->GetHandle();
		if (!entityHandle.IsValid()) continue;
		const uint64_t handle = (uint64_t)(uint32_t)entityHandle.ToInt();

		int throwTick = currentTick;
		const float creationTime = entity->GetGrenadeTrajectoryCreationTime();
		if (std::isfinite(creationTime) && creationTime > 0.0f
			&& std::isfinite(currentGameTime) && currentGameTime > 0.0f) {
			// Both values are server/game time. Convert their relative age onto the
			// current playback/demo tick, avoiding the server-start tick offset.
			const double estimated = currentTick + (creationTime - currentGameTime) / interval;
			if (std::isfinite(estimated) && std::fabs(estimated - currentTick) < 10000.0)
				throwTick = (int)std::llround(estimated);
		}

		auto& record = m_grenadeCache[handle];
		record.handle = handle;
		record.entityIndex = i;
		record.className = cls;
		record.grenadeType = GrenadeTypeName(cls);
		record.throwTick = throwTick;
		record.active = true;
		ResolveOwner(entity, record.throwerIndex, record.throwerName, record.team);
	}

	for (auto it = m_grenadeCache.begin(); it != m_grenadeCache.end();) {
		if (it->second.throwTick >= 0 && std::abs(it->second.throwTick - currentTick) > 5000)
			it = m_grenadeCache.erase(it);
		else
			++it;
	}
}

std::vector<FollowTargetCandidate> FollowCamera::Candidates() const {
	std::vector<FollowTargetCandidate> out;
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return out;

	// Distance is measured from the LIVE view (where the operator is flying / looking), not the
	// placed lock-on camera -- so "Select Nearest" picks the target nearest the current shot and
	// the list readout reflects how close the operator is. Falls back to the placed camera only
	// if the live camera read returns garbage.
	FollowVec3 reference = m_state.cameraPosition;
	{
		double vpos[3] = {}, vang[3] = {}, vfov = 90.0;
		CameraBridge_GetCurrentCamera(vpos, vang, vfov);
		if (std::isfinite(vpos[0]) && std::isfinite(vpos[1]) && std::isfinite(vpos[2]))
			reference = FollowVec3{ vpos[0], vpos[1], vpos[2] };
	}

	if (m_state.targetType == FollowTargetType::Grenade) {
		UpdateGrenadeCache();
		int currentTick = 0;
		g_MirvTime.GetCurrentDemoTick(currentTick);
		for (const auto& pair : m_grenadeCache) {
			const GrenadeRecord& record = pair.second;
			// Browse window: grenades thrown within ~1000 ticks either side of the playhead
			// (recently detected, or about to exist once scrubbed into range).
			if (record.throwTick < 0 || std::abs(record.throwTick - currentTick) > 1000)
				continue;
			FollowTargetCandidate candidate;
			candidate.entityIndex = record.active ? record.entityIndex : -1;
			candidate.handle = record.handle;
			candidate.name = record.grenadeType;
			candidate.className = record.className;
			candidate.grenadeType = record.grenadeType;
			candidate.ownerIndex = record.throwerIndex;
			candidate.ownerName = record.throwerName;
			candidate.team = record.team;
			candidate.throwTick = record.throwTick;
			candidate.tickDelta = record.throwTick - currentTick;
			candidate.status = record.active ? "active projectile" : "recorded nearby";
			candidate.alive = record.active;
			if (record.active) {
				CEntityInstance* grenade = EntityAt(record.entityIndex);
				candidate.attachPoints = AvailableAttachPoints(grenade, FollowTargetType::Grenade, false, false);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
				FollowVec3 position;
				if (ReadOrigin(grenade, position))
					candidate.distance = FollowDistance(reference, position);
			} else {
				candidate.attachPoints.push_back(MakeAttachPoint("entity", "Entity", "entity", true, 0, "recorded-grenade"));
				candidate.attachments = AttachPointIds(candidate.attachPoints);
			}
			out.push_back(std::move(candidate));
		}
		std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
			const int ad = std::abs(a.tickDelta), bd = std::abs(b.tickDelta);
			return ad == bd ? a.throwTick < b.throwTick : ad < bd;
		});
		return out;
	}

	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity) continue;

		FollowTargetCandidate candidate;
		CEntityInstance* target = entity;
		if (m_state.targetType == FollowTargetType::Player) {
			if (!entity->IsPlayerController()) continue;
			const auto pawnHandle = entity->GetPlayerPawnHandle();
			if (!pawnHandle.IsValid()) continue;
			target = EntityAt(pawnHandle.GetEntryIndex());
			if (!target || !target->IsPlayerPawn()) continue;
			candidate.entityIndex = pawnHandle.GetEntryIndex();
			const char* playerName = entity->GetPlayerName();
			candidate.name = playerName && *playerName ? playerName : "Player";
			candidate.team = target->GetTeam();
			candidate.alive = target->GetHealth() > 0;
			candidate.status = candidate.alive ? "alive" : "dead";
			if (m_state.mode == FollowMode::Attach) {
				candidate.attachPoints = AvailableAttachPoints(target, FollowTargetType::Player, false, false);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
			}
		} else { // Weapon: dropped + held weapons and the C4
			const std::string cls = EntityClass(entity);
			if (!IsWeaponOrBomb(cls)) continue;
			candidate.entityIndex = i;
			candidate.className = cls;
			candidate.isWeapon = IsWeaponClass(cls);
			candidate.isBomb = IsBombClass(cls);
			ResolveOwner(entity, candidate.ownerIndex, candidate.ownerName, candidate.team);
			candidate.held = candidate.ownerIndex >= 0;
			candidate.dropped = !candidate.held;
			candidate.name = candidate.isBomb ? "C4" : (cls.empty() ? "Weapon" : PrettyWeaponName(cls));
			candidate.alive = true;
			if (candidate.held)
				candidate.status = (candidate.isBomb ? "C4 carried by " : "held by ")
					+ (candidate.ownerName.empty() ? "player" : candidate.ownerName);
			else
				candidate.status = candidate.isBomb ? "C4 on the ground" : "dropped on floor";
			if (m_state.mode == FollowMode::Attach) {
				candidate.attachPoints = AvailableAttachPoints(entity, FollowTargetType::Weapon, candidate.isWeapon, candidate.isBomb);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
			}
		}

		FollowVec3 position;
		if (m_state.targetType == FollowTargetType::Player) {
			float eye[3] = {};
			target->GetRenderEyeOrigin(eye);
			position = FollowVec3{ eye[0], eye[1], eye[2] };
		} else if (!ReadOrigin(target, position)) {
			continue;
		}
		candidate.distance = FollowDistance(reference, position);
		const auto handle = target->GetHandle();
		candidate.handle = handle.IsValid() ? (uint64_t)(uint32_t)handle.ToInt() : 0;
		out.push_back(candidate);
	}

	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
		// Weapons HELD by a player rank above dropped ones (you almost always want the
		// in-use weapon); within each group, nearest first. Players (held==false for both)
		// fall straight through to the distance compare.
		if (a.held != b.held) return a.held;
		return a.distance < b.distance;
	});
	return out;
}

bool FollowCamera::SelectCandidate(const FollowTargetCandidate& candidate) {
	if (candidate.entityIndex < 0 && !(m_state.targetType == FollowTargetType::Grenade && candidate.handle))
		return false;
	if (m_haveLastKnownTarget) {
		m_retargetFrom = m_lastKnownTarget;
		m_retargeting = true;
		m_retargetElapsed = 0.0;
	}
	m_state.targetEntityIndex = candidate.entityIndex;
	m_state.targetHandle = candidate.handle;
	if (m_state.targetType == FollowTargetType::Grenade)
		m_selectedGrenadeHandle = candidate.handle;
	// Held-weapon tracking re-resolves the player's active weapon each frame; remember
	// the owner so the camera survives weapon switches. Dropped items have no owner.
	if (m_state.targetType == FollowTargetType::Weapon)
		m_state.weaponPlayerIndex = candidate.held ? candidate.ownerIndex : -1;
	if (m_state.mode == FollowMode::Attach && !candidate.attachPoints.empty()) {
		bool currentValid = false;
		for (const auto& point : candidate.attachPoints) {
			if (point.valid && point.id == m_state.attachmentName) {
				currentValid = true;
				break;
			}
		}
		if (!currentValid) {
			const char* preferred = m_state.targetType == FollowTargetType::Player ? "head"
				: (m_state.targetType == FollowTargetType::Weapon ? "muzzle" : "entity");
			const FollowAttachPoint* fallback = nullptr;
			for (const auto& point : candidate.attachPoints) {
				if (point.valid && point.id == preferred) { fallback = &point; break; }
			}
			if (!fallback) {
				for (const auto& point : candidate.attachPoints) {
					if (point.valid) { fallback = &point; break; }
				}
			}
			if (fallback)
				m_state.attachmentName = fallback->id;
		}
	}
	m_haveLastRawTarget = false;
	m_lastMessage = "Selected " + candidate.name + ".";
	advancedfx::Message("[followcam] target selected: type=%s entity=%d name=%s distance=%.1f.\n",
		FollowTargetTypeName(m_state.targetType), candidate.entityIndex, candidate.name.c_str(), candidate.distance);
	return true;
}

bool FollowCamera::SelectNearest() {
	auto candidates = Candidates();
	if (candidates.empty()) {
		advancedfx::Warning("[followcam] no %s target found.\n", FollowTargetTypeName(m_state.targetType));
		return false;
	}
	if (m_state.targetType == FollowTargetType::Player) {
		for (const auto& candidate : candidates)
			if (candidate.alive)
				return SelectCandidate(candidate);
	}
	return SelectCandidate(candidates.front());
}

bool FollowCamera::SelectEntity(int entityIndex) {
	auto candidates = Candidates();
	for (const auto& candidate : candidates)
		if (candidate.entityIndex == entityIndex)
			return SelectCandidate(candidate);
	advancedfx::Warning("[followcam] entity %d is not a valid %s target.\n",
		entityIndex, FollowTargetTypeName(m_state.targetType));
	return false;
}

bool FollowCamera::SelectHandle(uint64_t handle) {
	if (!handle) return false;
	if (m_state.targetType != FollowTargetType::Grenade)
		m_state.targetType = FollowTargetType::Grenade;
	UpdateGrenadeCache();
	auto it = m_grenadeCache.find(handle);
	if (it != m_grenadeCache.end()) {
		FollowTargetCandidate candidate;
		candidate.handle = it->second.handle;
		candidate.entityIndex = it->second.active ? it->second.entityIndex : -1;
		candidate.name = it->second.grenadeType;
		candidate.grenadeType = it->second.grenadeType;
		candidate.className = it->second.className;
		candidate.ownerName = it->second.throwerName;
		candidate.ownerIndex = it->second.throwerIndex;
		candidate.team = it->second.team;
		candidate.throwTick = it->second.throwTick;
		candidate.status = it->second.active ? "active projectile" : "recorded nearby";
		return SelectCandidate(candidate);
	}
	advancedfx::Warning("[followcam] grenade handle %llu is no longer in the session index.\n",
		(unsigned long long)handle);
	m_lastMessage = "Selected grenade is no longer in the session index.";
	return false;
}

bool FollowCamera::TrackSelectedGrenade() {
	if (!m_state.hasCamera) {
		advancedfx::Warning("[followcam] grenade tracking refused: place the Follow Camera first.\n");
		m_lastMessage = "Place the Follow Camera before tracking a grenade.";
		return false;
	}
	if (!m_selectedGrenadeHandle) {
		advancedfx::Warning("[followcam] grenade tracking refused: select a grenade first.\n");
		m_lastMessage = "Select a grenade from the nearby list first.";
		return false;
	}
	UpdateGrenadeCache();
	auto it = m_grenadeCache.find(m_selectedGrenadeHandle);
	if (it == m_grenadeCache.end() || it->second.throwTick < 0) {
		advancedfx::Warning("[followcam] grenade tracking refused: throw tick is unavailable.\n");
		m_lastMessage = "The selected grenade has no usable throw tick.";
		return false;
	}

	m_state.targetType = FollowTargetType::Grenade;
	m_state.targetHandle = m_selectedGrenadeHandle;
	m_state.targetEntityIndex = -1;
	m_grenadeThrowTick = it->second.throwTick;
	m_grenadeSeekTick = (std::max)(0, m_grenadeThrowTick - 40);
	m_grenadeTrackPending = true;
	m_grenadeSeekObserved = false;
	m_grenadeResumeIssued = false;
	m_state.enabled = true;
	ResetMotion();
	CameraPathRef().StopPreview();
	CameraPathRef().StopScrub();
	GraphEditorExperimentHudRef().SetDrive(false);
	CameraBridge_SetPathDrawEnabled(false);
	CameraBridge_SetFreeCamEnabled(true);

	if (g_pEngineToClient) {
		std::ostringstream command;
		command << "demo_gototick " << m_grenadeSeekTick;
		g_pEngineToClient->ExecuteClientCmd(0, command.str().c_str(), true);
	}
	m_lastMessage = "Seeking to tick " + std::to_string(m_grenadeSeekTick)
		+ ", 40 ticks before the throw.";
	advancedfx::Message("[followcam] grenade track armed: %s by %s, throwTick=%d seekTick=%d handle=%llu.\n",
		it->second.grenadeType.c_str(), it->second.throwerName.c_str(),
		m_grenadeThrowTick, m_grenadeSeekTick, (unsigned long long)m_selectedGrenadeHandle);
	return true;
}

bool FollowCamera::ResolvePendingGrenade(int currentTick) {
	auto it = m_grenadeCache.find(m_selectedGrenadeHandle);
	if (it == m_grenadeCache.end() || !it->second.active) {
		const std::string expectedType = it != m_grenadeCache.end() ? it->second.grenadeType : "";
		auto best = m_grenadeCache.end();
		for (auto candidate = m_grenadeCache.begin(); candidate != m_grenadeCache.end(); ++candidate) {
			if (!candidate->second.active) continue;
			if (!expectedType.empty() && candidate->second.grenadeType != expectedType) continue;
			if (std::abs(candidate->second.throwTick - m_grenadeThrowTick) > 4) continue;
			best = candidate;
			break;
		}
		if (best == m_grenadeCache.end())
			return false;
		it = best;
		m_selectedGrenadeHandle = it->first;
	}
	m_state.targetEntityIndex = it->second.entityIndex;
	m_state.targetHandle = it->second.handle;
	m_grenadeTrackPending = false;
	ResetMotion();
	m_lastMessage = "Tracking " + it->second.grenadeType + " from throw tick "
		+ std::to_string(it->second.throwTick) + ".";
	advancedfx::Message("[followcam] grenade acquired at tick %d: entity=%d handle=%llu.\n",
		currentTick, m_state.targetEntityIndex, (unsigned long long)m_state.targetHandle);
	return true;
}

std::unique_ptr<IFollowTargetProvider> FollowCamera::MakeProvider() const {
	const bool attach = m_state.mode == FollowMode::Attach;
	switch (m_state.targetType) {
	case FollowTargetType::Player:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Player, m_state.attachmentName);
		return std::make_unique<PlayerTargetProvider>(m_state.targetEntityIndex);
	case FollowTargetType::Grenade:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Grenade, m_state.attachmentName);
		return std::make_unique<EntityTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Grenade);
	case FollowTargetType::Weapon: {
		const bool held = (m_state.weaponSource == WeaponSource::Held)
			|| (m_state.weaponSource == WeaponSource::Auto && m_state.weaponPlayerIndex >= 0);
		if (held && m_state.weaponPlayerIndex >= 0)
			return std::make_unique<HeldWeaponProvider>(m_state.weaponPlayerIndex,
				attach ? m_state.attachmentName : std::string());
		if (attach && !m_state.attachmentName.empty())
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Weapon, m_state.attachmentName);
		return std::make_unique<EntityTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Weapon);
	}
	default:
		return std::make_unique<PlayerTargetProvider>(m_state.targetEntityIndex);
	}
}

bool FollowCamera::TryDeathRetarget() {
	auto trySwitch = [&](int entityIndex) -> bool {
		if (entityIndex < 0) return false;
		const FollowTargetType oldType = m_state.targetType;
		const WeaponSource oldSrc = m_state.weaponSource;
		m_state.targetType = FollowTargetType::Weapon;
		m_state.weaponSource = WeaponSource::Dropped;
		m_state.weaponPlayerIndex = -1;
		if (SelectEntity(entityIndex)) return true;
		m_state.targetType = oldType;
		m_state.weaponSource = oldSrc;
		return false;
	};
	if (m_state.switchToDroppedBombOnDeath && trySwitch(m_lastPlayerBombIndex)) return true;
	if (m_state.switchToDroppedWeaponOnDeath && trySwitch(m_lastPlayerWeaponIndex)) return true;
	return false;
}

void FollowCamera::RunFrame(bool cameraEditorActive) {
	if (!cameraEditorActive) {
		CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
		if (m_state.enabled) StopPreview("camera editor closed");
		return;
	}
	if (m_state.hasCamera && m_state.mode == FollowMode::LockOn && !m_state.enabled) {
		CameraBridge_SetFollowCameraMarker(true,
			m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
			m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
	} else {
		CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
	}
	// Attach mode rides the target and needs no placed camera; lock-on requires one.
	if (!m_state.enabled)
		return;
	if (m_state.mode == FollowMode::LockOn && !m_state.hasCamera)
		return;

	double dt = WallDt();
	// Is the demo actually advancing this frame? When it's paused/frozen the entity still
	// micro-jitters from interpolation; dividing that by the (tiny) wall-clock dt yields a
	// huge bogus velocity that drives the "face travel" path and makes an attached camera
	// spin/jitter in place. Detect a frozen tick so we can zero velocity below.
	int frameTick = 0; g_MirvTime.GetCurrentDemoTick(frameTick);
	const int previousFrameTick = m_lastFrameTick;
	const bool demoAdvancing = (previousFrameTick >= 0 && frameTick != previousFrameTick);
	if (demoAdvancing) {
		float interval = g_MirvTime.interval_per_tick_get();
		if (!(interval > 0.00001f)) interval = 1.0f / 64.0f;
		const int tickDelta = std::abs(frameTick - previousFrameTick);
		if (tickDelta > 0 && tickDelta < 128)
			dt = std::clamp((double)tickDelta * (double)interval, 0.0001, 0.1);
	}
	m_lastFrameTick = frameTick;
	if (m_state.targetType == FollowTargetType::Grenade && m_grenadeTrackPending) {
		UpdateGrenadeCache();
		int currentTick = 0;
		g_MirvTime.GetCurrentDemoTick(currentTick);

		if (!m_grenadeSeekObserved) {
			if (currentTick >= m_grenadeSeekTick - 3 && currentTick <= m_grenadeThrowTick - 8)
				m_grenadeSeekObserved = true;
			CameraBridge_SetFreeCamEnabled(true);
			CameraBridge_SetCameraPose(
				m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
				m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
			if (!m_grenadeSeekObserved)
				return;
		}

		if (!m_grenadeResumeIssued && g_pEngineToClient) {
			g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
			m_grenadeResumeIssued = true;
			m_lastMessage = "Waiting for the selected grenade to be thrown.";
		}

		if (currentTick >= m_grenadeThrowTick - 2 && ResolvePendingGrenade(currentTick)) {
			// Continue into the normal provider path this frame.
		} else if (currentTick > m_grenadeThrowTick + 96) {
			m_lastMessage = "Could not reacquire the selected grenade after its throw tick.";
			advancedfx::Warning("[followcam] grenade track failed: handle %llu did not appear near tick %d.\n",
				(unsigned long long)m_selectedGrenadeHandle, m_grenadeThrowTick);
			StopPreview("grenade could not be reacquired after seek");
			return;
		} else {
			CameraBridge_SetFreeCamEnabled(true);
			CameraBridge_SetCameraPose(
				m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
				m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
			return;
		}
	}

	auto provider = MakeProvider();
	if (!provider) return;

	if (m_state.mode == FollowMode::Attach) {
		const int activeEntityIndex = provider->EntityIndex();
		CEntityInstance* activeEntity = EntityAt(activeEntityIndex);
		bool isWeapon = false, isBomb = false;
		if (activeEntity) {
			const std::string cls = EntityClass(activeEntity);
			isWeapon = IsWeaponClass(cls);
			isBomb = IsBombClass(cls);
		}
		auto points = AvailableAttachPoints(activeEntity, m_state.targetType, isWeapon, isBomb);
		bool currentValid = false;
		for (const auto& point : points) {
			if (point.valid && point.id == m_state.attachmentName) {
				currentValid = true;
				break;
			}
		}
		if (!points.empty() && !currentValid) {
			const char* preferred = m_state.targetType == FollowTargetType::Player ? "head"
				: (m_state.targetType == FollowTargetType::Weapon ? "muzzle" : "entity");
			const FollowAttachPoint* fallback = nullptr;
			for (const auto& point : points) {
				if (point.valid && point.id == preferred) { fallback = &point; break; }
			}
			if (!fallback) {
				for (const auto& point : points) {
					if (point.valid) { fallback = &point; break; }
				}
			}
			if (fallback) {
				const std::string old = m_state.attachmentName;
				m_state.attachmentName = fallback->id;
				m_lastMessage = "Attach point '" + old + "' is not valid on this target; using '" + fallback->id + "'.";
				advancedfx::Warning("[followcam] attach point '%s' invalid for entity %d; using '%s'.\n",
					old.c_str(), activeEntityIndex, fallback->id.c_str());
				ResetMotion();
				provider = MakeProvider();
				if (!provider) return;
			}
		}
	}

	if (m_state.targetType == FollowTargetType::Player) {
		CEntityInstance* pawn = EntityAt(m_state.targetEntityIndex);
		if (pawn && pawn->IsPlayerPawn() && pawn->GetHealth() > 0) {
			m_deathRetargetElapsed = 0.0;
			auto weapon = pawn->GetActiveWeaponHandle();
			if (weapon.IsValid()) m_lastPlayerWeaponIndex = weapon.GetEntryIndex();
			m_lastPlayerBombIndex = -1;
			const int pawnIndex = pawn->GetHandle().IsValid() ? pawn->GetHandle().GetEntryIndex() : m_state.targetEntityIndex;
			for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
				CEntityInstance* entity = EntityAt(i);
				if (!entity || !IsBombClass(EntityClass(entity))) continue;
				auto owner = entity->GetOwnerEntityHandle();
				if (owner.IsValid() && owner.GetEntryIndex() == pawnIndex) {
					m_lastPlayerBombIndex = i;
					break;
				}
			}
		}
	}

	if (!provider->IsValid()) {
		if (provider->GetStatus() == FollowTargetStatus::Dead) {
			const bool wantsRetarget = m_state.switchToDroppedWeaponOnDeath || m_state.switchToDroppedBombOnDeath;
			if (wantsRetarget) {
				m_deathRetargetElapsed += dt;
				if (TryDeathRetarget()) {
					provider = MakeProvider();
					m_deathRetargetElapsed = 0.0;
				} else if (m_deathRetargetElapsed < 0.75 && m_haveLastKnownTarget) {
					// Give CS2 a short window to detach/spawn the dropped entity after the
					// pawn health reaches zero. Keep pushing the last pose during the wait.
				} else if (m_state.autoDisableOnDeath) {
					StopPreview("target died and no retarget appeared");
					return;
				}
			} else if (m_state.autoDisableOnDeath) {
				StopPreview("target died");
				return;
			}
		} else if (!m_state.holdLastKnownPosition || !m_haveLastKnownTarget) {
			StopPreview("target disappeared");
			return;
		}
	}

	FollowVec3 rawTarget = m_haveLastKnownTarget ? m_lastKnownTarget : FollowVec3{};
	FollowAngles rawAngles{};
	bool rawOriented = false;
	const bool validNow = provider->IsValid();
	if (validNow) {
		rawOriented = provider->GetWorldTransform(rawTarget, rawAngles);
		if (!IsFiniteOrigin(rawTarget)) {
			StopPreview("target position invalid");
			return;
		}
		if (!demoAdvancing) {
			// Frozen demo: hold still. No velocity (so no spurious prediction / face-travel).
			m_targetVelocity = {};
		} else if (m_haveLastRawTarget && dt > 0.0001) {
			const double distance = FollowDistance(m_lastRawTarget, rawTarget);
			if (distance > kTeleportDistance) {
				m_motionInitialized = false;
				m_targetVelocity = {};
			} else {
				m_targetVelocity = FollowVec3{
					(rawTarget.x - m_lastRawTarget.x) / dt,
					(rawTarget.y - m_lastRawTarget.y) / dt,
					(rawTarget.z - m_lastRawTarget.z) / dt
				};
			}
		}
		m_lastRawTarget = rawTarget;
		m_haveLastRawTarget = true;
		m_lastKnownTarget = rawTarget;
		m_haveLastKnownTarget = true;
	}

	// ============================ ATTACH (ride-along) =========================
	// The camera rides the target: its position is the target/attachment point plus a
	// LOCAL offset, its orientation is the attachment orientation (or direction of
	// travel) plus the rotation trim, and FOV is user-set. No placed camera needed.
	if (m_state.mode == FollowMode::Attach) {
		FollowAngles baseAng = rawAngles;
		if (!rawOriented) {
			const double speed = std::sqrt(
				m_targetVelocity.x * m_targetVelocity.x +
				m_targetVelocity.y * m_targetVelocity.y +
				m_targetVelocity.z * m_targetVelocity.z);
			if (speed > 40.0)
				baseAng = FollowLookAt(FollowVec3{}, m_targetVelocity); // face travel
			else
				// Hold the offset-FREE base, NOT m_outputAngles. m_outputAngles already has the
				// rotation trim folded in, so using it here re-added the trim every frame ->
				// the camera spun continuously (faster at low smoothing). m_baseAngles excludes
				// the trim, so a stationary target produces a stable desired angle.
				baseAng = m_motionInitialized ? m_baseAngles : m_state.cameraAngles;
		}
		m_baseAngles = baseAng; // remember the trim-free base for the next stationary hold
		const FollowVec3 worldOffset = FollowRotateVector(m_state.offset, baseAng);
		const FollowVec3 desiredPos{
			rawTarget.x + worldOffset.x + m_targetVelocity.x * m_state.prediction,
			rawTarget.y + worldOffset.y + m_targetVelocity.y * m_state.prediction,
			rawTarget.z + worldOffset.z + m_targetVelocity.z * m_state.prediction
		};
		const FollowAngles desiredAng = FollowAddAngles(baseAng, m_state.rotationOffset);
		if (!m_motionInitialized) {
			m_smoothedCamPos = desiredPos;
			m_outputAngles = desiredAng;
			m_motionInitialized = true;
		} else {
			m_smoothedCamPos = FollowSmoothPosition(
				m_smoothedCamPos, desiredPos, dt, m_state.positionSmoothing);
			m_outputAngles = FollowSmoothAngles(m_outputAngles, desiredAng, dt,
				m_state.lookSmoothing, m_state.deadzone, m_state.maxTurnSpeed);
			m_outputAngles.roll = desiredAng.roll; // smoothing covers pitch/yaw only
		}
		m_lastCamPos = m_smoothedCamPos;
		m_lastCamAng = m_outputAngles;
		m_lastTargetPos = rawTarget;
		// Stash the rebase basis for ViewSetupAttachOverride: the smoothed camera's offset from the
		// (tick-stepped) target. When the mount has orientation, store it in local axes so render-time
		// rebase rotates it with the fresh bone/player orientation instead of carrying a stale
		// world-space offset across the interpolation boundary.
		const FollowVec3 attachWorldOffset{
			m_smoothedCamPos.x - rawTarget.x,
			m_smoothedCamPos.y - rawTarget.y,
			m_smoothedCamPos.z - rawTarget.z };
		m_attachOffsetFromTarget = rawOriented
			? FollowInverseRotateVector(attachWorldOffset, baseAng)
			: attachWorldOffset;
		m_attachOffsetIsLocal = rawOriented;
		m_attachBaseAngles = baseAng;
		m_attachAngleOffsetFromBase = FollowAngles{
			FollowWrapDegrees(m_outputAngles.pitch - baseAng.pitch),
			FollowWrapDegrees(m_outputAngles.yaw - baseAng.yaw),
			FollowWrapDegrees(m_outputAngles.roll - baseAng.roll)
		};
		m_attachBaseOriented = rawOriented;
		m_attachSteppedTarget = rawTarget;
		m_attachRebaseValid = true;
		const double pitchErr = FollowWrapDegrees(desiredAng.pitch - m_outputAngles.pitch);
		const double yawErr = FollowWrapDegrees(desiredAng.yaw - m_outputAngles.yaw);
		AttachTransformResult resolved = ResolveAttachTransform(
			EntityAt(provider->EntityIndex()), provider->EntityIndex(), m_state.targetType, m_state.attachmentName);
		m_attachDebug.active = true;
		m_attachDebug.valid = validNow && resolved.valid;
		m_attachDebug.oriented = rawOriented;
		m_attachDebug.entityIndex = provider->EntityIndex();
		m_attachDebug.entityHandle = resolved.entityHandle;
		m_attachDebug.entityType = FollowTargetTypeName(m_state.targetType);
		m_attachDebug.className = resolved.className;
		m_attachDebug.modelName = resolved.modelName;
		m_attachDebug.attachId = m_state.attachmentName;
		m_attachDebug.attachKind = resolved.attachKind;
		m_attachDebug.attachIndex = resolved.attachIndex;
		m_attachDebug.source = resolved.source;
		m_attachDebug.rawTarget = rawTarget;
		m_attachDebug.rawAngles = rawAngles;
		m_attachDebug.smoothedTarget = rawTarget;
		m_attachDebug.smoothedAngles = m_outputAngles;
		m_attachDebug.cameraPosition = m_smoothedCamPos;
		m_attachDebug.cameraAngles = m_outputAngles;
		m_attachDebug.distance = FollowDistance(m_smoothedCamPos, rawTarget);
		m_attachDebug.cameraDelta = m_motionInitialized && m_havePrevDebug ? FollowDistance(m_prevDebugCamPos, m_smoothedCamPos) : 0.0;
		m_attachDebug.targetDelta = m_havePrevDebug ? FollowDistance(m_prevDebugTargetPos, rawTarget) : 0.0;
		m_attachDebug.angleDelta = m_havePrevDebug
			? std::sqrt(std::pow(FollowWrapDegrees(m_outputAngles.pitch - m_prevDebugCamAng.pitch), 2.0)
				+ std::pow(FollowWrapDegrees(m_outputAngles.yaw - m_prevDebugCamAng.yaw), 2.0))
			: 0.0;
		m_attachDebug.aimError = std::sqrt(pitchErr * pitchErr + yawErr * yawErr);
		m_attachDebug.jitter = (m_attachDebug.cameraDelta > m_attachDebug.targetDelta + 0.01)
			? (m_attachDebug.cameraDelta - m_attachDebug.targetDelta) : 0.0;
		m_attachDebug.smoothing = m_state.positionSmoothing;
		m_attachDebug.previewTick = frameTick;
		m_attachDebug.demoTick = frameTick;
		m_prevDebugCamPos = m_smoothedCamPos;
		m_prevDebugTargetPos = rawTarget;
		m_prevDebugCamAng = m_outputAngles;
		m_havePrevDebug = true;
		CameraBridge_SetFreeCamEnabled(true);
		CameraBridge_SetCameraPose(
			m_smoothedCamPos.x, m_smoothedCamPos.y, m_smoothedCamPos.z,
			m_outputAngles.pitch, m_outputAngles.yaw, m_outputAngles.roll, m_state.fov);
		if (m_debug && ((m_debugFrame++ % 15) == 0)) {
			advancedfx::Message(
				"[followcam][attachdebug] type=%s entity=%d handle=%llu attach=%s kind=%s valid=%d source=%s "
				"target=(%.1f %.1f %.1f) cam=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) "
				"dist=%.1f camDelta=%.2f targetDelta=%.2f angleDelta=%.2f mountError=%.2f jitter=%.2f tick=%d fov=%.1f.\n",
				FollowTargetTypeName(m_state.targetType), m_attachDebug.entityIndex,
				(unsigned long long)m_attachDebug.entityHandle, m_attachDebug.attachId.c_str(),
				m_attachDebug.attachKind.c_str(), m_attachDebug.valid ? 1 : 0, m_attachDebug.source.c_str(),
				rawTarget.x, rawTarget.y, rawTarget.z,
				m_smoothedCamPos.x, m_smoothedCamPos.y, m_smoothedCamPos.z,
				m_outputAngles.pitch, m_outputAngles.yaw, m_outputAngles.roll,
				m_attachDebug.distance, m_attachDebug.cameraDelta, m_attachDebug.targetDelta,
				m_attachDebug.angleDelta, m_attachDebug.aimError, m_attachDebug.jitter,
				frameTick, m_state.fov);
		}
		return;
	}

	// ============================ LOCK-ON (turret) ===========================
	FollowVec3 desiredTarget{
		rawTarget.x + m_state.offset.x + m_targetVelocity.x * m_state.prediction,
		rawTarget.y + m_state.offset.y + m_targetVelocity.y * m_state.prediction,
		rawTarget.z + m_state.offset.z + m_targetVelocity.z * m_state.prediction
	};

	if (m_retargeting && m_state.retargetBlendTime > 0.0001) {
		m_retargetElapsed += dt;
		const double t = (std::min)(1.0, m_retargetElapsed / m_state.retargetBlendTime);
		const double smooth = t * t * (3.0 - 2.0 * t);
		desiredTarget = FollowVec3{
			m_retargetFrom.x + (desiredTarget.x - m_retargetFrom.x) * smooth,
			m_retargetFrom.y + (desiredTarget.y - m_retargetFrom.y) * smooth,
			m_retargetFrom.z + (desiredTarget.z - m_retargetFrom.z) * smooth
		};
		if (t >= 1.0) m_retargeting = false;
	}

	if (!m_motionInitialized) {
		m_smoothedTarget = desiredTarget;
		m_outputAngles = m_state.cameraAngles;
		m_motionInitialized = true;
	} else {
		m_smoothedTarget = FollowSmoothPosition(
			m_smoothedTarget, desiredTarget, dt, m_state.positionSmoothing);
	}

	const FollowAngles desiredAngles = FollowLookAt(m_state.cameraPosition, m_smoothedTarget);
	m_outputAngles = FollowSmoothAngles(
		m_outputAngles, desiredAngles, dt, m_state.lookSmoothing,
		m_state.deadzone, m_state.maxTurnSpeed);

	// Rotation sliders trim the aim (pitch/yaw) and set the roll on top of the solve.
	const FollowAngles aimedAngles{
		FollowWrapDegrees(m_outputAngles.pitch + m_state.rotationOffset.pitch),
		FollowWrapDegrees(m_outputAngles.yaw + m_state.rotationOffset.yaw),
		FollowWrapDegrees(m_state.cameraAngles.roll + m_state.rotationOffset.roll)
	};
	m_lastCamPos = m_state.cameraPosition;
	m_lastCamAng = aimedAngles;
	m_lastTargetPos = rawTarget;

	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(
		m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
		aimedAngles.pitch, aimedAngles.yaw, aimedAngles.roll, m_state.fov);

	if (m_debug && ((m_debugFrame++ % 15) == 0)) {
		advancedfx::Message(
			"[followcam][tick] type=%s entity=%d valid=%d name=%s dt=%.4f "
			"target=(%.1f %.1f %.1f) predicted=(%.1f %.1f %.1f) "
			"angle=(%.2f %.2f) desired=(%.2f %.2f) distance=%.1f freecam=%d.\n",
			FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex, validNow ? 1 : 0,
			provider->GetDisplayName().c_str(), dt,
			rawTarget.x, rawTarget.y, rawTarget.z,
			m_smoothedTarget.x, m_smoothedTarget.y, m_smoothedTarget.z,
			m_outputAngles.pitch, m_outputAngles.yaw, desiredAngles.pitch, desiredAngles.yaw,
			FollowDistance(m_state.cameraPosition, rawTarget),
			CameraBridge_GetFreeCamEnabled() ? 1 : 0);
	}
}

void FollowCamera::SetOffset(double x, double y, double z) {
	m_state.offset = FollowVec3{ x, y, z };
}

void FollowCamera::SetOffsetAxis(int axis, double value) {
	if (axis == 0) m_state.offset.x = value;
	else if (axis == 1) m_state.offset.y = value;
	else if (axis == 2) m_state.offset.z = value;
}

void FollowCamera::SetRotationOffset(double pitch, double yaw, double roll) {
	m_state.rotationOffset = FollowAngles{ pitch, yaw, roll };
}

void FollowCamera::SetRotationAxis(int axis, double value) {
	if (axis == 0) m_state.rotationOffset.pitch = value;
	else if (axis == 1) m_state.rotationOffset.yaw = value;
	else if (axis == 2) m_state.rotationOffset.roll = value;
}

void FollowCamera::SetFov(double value) {
	m_state.fov = std::clamp(value, 1.0, 170.0);
}

void FollowCamera::SetPreset(const std::string& name) {
	const std::string preset = Lower(name);
	if (preset == "above") SetOffset(0, 0, 48);
	else if (preset == "low" || preset == "lowground") SetOffset(0, 0, -48);
	else if (preset == "shoulder") SetOffset(0, 24, 24);
	else if (preset == "side") SetOffset(0, 64, 12);
	else if (preset == "behind") SetOffset(-64, 0, 16);
	else if (preset == "orbitleft") SetOffset(0, -96, 32);
	else if (preset == "orbitright") SetOffset(0, 96, 32);
	else if (preset == "weapon" || preset == "weaponclose") SetOffset(0, 0, 8);
	else if (preset == "bomb" || preset == "bombclose") SetOffset(0, 0, 12);
	else SetOffset(0, 0, 0);
}

void FollowCamera::SetLookSmoothing(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.lookSmoothing = 0.0;
		return;
	}
	m_state.lookSmoothing = std::clamp(value, 0.0, 5.0);
	m_lockOnLookSmoothing = m_state.lookSmoothing;
}
void FollowCamera::SetPositionSmoothing(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.positionSmoothing = 0.0;
		return;
	}
	m_state.positionSmoothing = std::clamp(value, 0.0, 5.0);
	m_lockOnPositionSmoothing = m_state.positionSmoothing;
}
void FollowCamera::SetPrediction(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.prediction = 0.0;
		return;
	}
	m_state.prediction = std::clamp(value, 0.0, 2.0);
	m_lockOnPrediction = m_state.prediction;
}
void FollowCamera::SetDeadzone(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.deadzone = 0.0;
		return;
	}
	m_state.deadzone = std::clamp(value, 0.0, 45.0);
	m_lockOnDeadzone = m_state.deadzone;
}
void FollowCamera::SetMaxTurnSpeed(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.maxTurnSpeed = 0.0;
		return;
	}
	m_state.maxTurnSpeed = std::clamp(value, 0.0, 4000.0);
	m_lockOnMaxTurnSpeed = m_state.maxTurnSpeed;
}
void FollowCamera::SetAttachmentName(const std::string& value) {
	if (!value.empty()) {
		m_state.attachmentName = value.substr(0, 64);
		ResetMotion();
	}
}

std::string FollowCamera::BuildStateJson() const {
	if (m_state.targetType == FollowTargetType::Weapon)
		EnsureEventsLoaded(); // kick off / refresh the drop-event index in the background
	std::unique_ptr<IFollowTargetProvider> provider = MakeProvider();
	const bool targetValid = provider && provider->IsValid();
	const std::string targetName = provider ? provider->GetDisplayName() : "";
	std::vector<FollowAttachPoint> selectedAttachPoints;
	if (provider && m_state.mode == FollowMode::Attach) {
		CEntityInstance* activeEntity = EntityAt(provider->EntityIndex());
		bool activeWeapon = false, activeBomb = false;
		if (activeEntity) {
			const std::string cls = EntityClass(activeEntity);
			activeWeapon = IsWeaponClass(cls);
			activeBomb = IsBombClass(cls);
		}
		selectedAttachPoints = AvailableAttachPoints(activeEntity, m_state.targetType, activeWeapon, activeBomb);
	}
	double targetDistance = 0.0;
	if (provider && targetValid)
		targetDistance = FollowDistance(m_state.cameraPosition, provider->GetWorldPosition());

	std::ostringstream o;
	o << std::fixed << std::setprecision(2);
	o << "{";
	o << "\"enabled\":" << (m_state.enabled ? "true" : "false");
	o << ",\"hasCamera\":" << (m_state.hasCamera ? "true" : "false");
	o << ",\"singleCamera\":true";
	o << ",\"repositioning\":" << (m_repositioning ? "true" : "false");
	o << ",\"type\":" << (int)m_state.targetType;
	o << ",\"typeName\":\"" << FollowTargetTypeName(m_state.targetType) << "\"";
	o << ",\"mode\":" << (int)m_state.mode;
	o << ",\"weaponSource\":" << (int)m_state.weaponSource;
	o << ",\"weaponPlayerIndex\":" << m_state.weaponPlayerIndex;
	o << ",\"targetIndex\":" << m_state.targetEntityIndex;
	o << ",\"targetHandle\":" << m_state.targetHandle;
	o << ",\"targetValid\":" << (targetValid ? "true" : "false");
	o << ",\"targetName\":\"" << JsonEscape(targetName) << "\"";
	o << ",\"targetDistance\":" << targetDistance;
	o << ",\"distanceLevel\":" << (targetDistance >= kVeryFarDistance ? 3 : targetDistance >= kFarDistance ? 2 : targetDistance >= kMediumDistance ? 1 : 0);
	o << ",\"offset\":[" << m_state.offset.x << "," << m_state.offset.y << "," << m_state.offset.z << "]";
	o << ",\"rotation\":[" << m_state.rotationOffset.pitch << "," << m_state.rotationOffset.yaw << "," << m_state.rotationOffset.roll << "]";
	o << ",\"fov\":" << m_state.fov;
	o << ",\"look\":" << m_state.lookSmoothing;
	o << ",\"position\":" << m_state.positionSmoothing;
	o << ",\"prediction\":" << m_state.prediction;
	o << ",\"deadzone\":" << m_state.deadzone;
	o << ",\"maxTurn\":" << m_state.maxTurnSpeed;
	o << ",\"autoDead\":" << (m_state.autoDisableOnDeath ? "true" : "false");
	o << ",\"switchWeapon\":" << (m_state.switchToDroppedWeaponOnDeath ? "true" : "false");
	o << ",\"switchBomb\":" << (m_state.switchToDroppedBombOnDeath ? "true" : "false");
	o << ",\"holdLast\":" << (m_state.holdLastKnownPosition ? "true" : "false");
	o << ",\"attachment\":\"" << JsonEscape(m_state.attachmentName) << "\"";
	o << ",\"attachPoints\":[";
	for (size_t ai = 0; ai < selectedAttachPoints.size(); ++ai) {
		if (ai) o << ",";
		AppendAttachPointJson(o, selectedAttachPoints[ai]);
	}
	o << "]";
	o << ",\"message\":\"" << JsonEscape(m_lastMessage) << "\"";
	o << ",\"grenadePending\":" << (m_grenadeTrackPending ? "true" : "false");
	o << ",\"grenadeThrowTick\":" << m_grenadeThrowTick;
	o << ",\"grenadeSeekTick\":" << m_grenadeSeekTick;
	o << ",\"debug\":" << (m_debug ? "true" : "false");
	o << ",\"renderTimeSample\":" << (m_renderTimeSample ? "true" : "false");
	o << ",\"attachDebug\":{";
	o << "\"active\":" << (m_attachDebug.active ? "true" : "false");
	o << ",\"valid\":" << (m_attachDebug.valid ? "true" : "false");
	o << ",\"oriented\":" << (m_attachDebug.oriented ? "true" : "false");
	o << ",\"entityIndex\":" << m_attachDebug.entityIndex;
	o << ",\"entityHandle\":" << m_attachDebug.entityHandle;
	o << ",\"entityType\":\"" << JsonEscape(m_attachDebug.entityType) << "\"";
	o << ",\"className\":\"" << JsonEscape(m_attachDebug.className) << "\"";
	o << ",\"modelName\":\"" << JsonEscape(m_attachDebug.modelName) << "\"";
	o << ",\"attachId\":\"" << JsonEscape(m_attachDebug.attachId) << "\"";
	o << ",\"attachKind\":\"" << JsonEscape(m_attachDebug.attachKind) << "\"";
	o << ",\"attachIndex\":" << m_attachDebug.attachIndex;
	o << ",\"source\":\"" << JsonEscape(m_attachDebug.source) << "\"";
	o << ",\"rawTarget\":[" << m_attachDebug.rawTarget.x << "," << m_attachDebug.rawTarget.y << "," << m_attachDebug.rawTarget.z << "]";
	o << ",\"rawAngles\":[" << m_attachDebug.rawAngles.pitch << "," << m_attachDebug.rawAngles.yaw << "," << m_attachDebug.rawAngles.roll << "]";
	o << ",\"smoothedTarget\":[" << m_attachDebug.smoothedTarget.x << "," << m_attachDebug.smoothedTarget.y << "," << m_attachDebug.smoothedTarget.z << "]";
	o << ",\"smoothedAngles\":[" << m_attachDebug.smoothedAngles.pitch << "," << m_attachDebug.smoothedAngles.yaw << "," << m_attachDebug.smoothedAngles.roll << "]";
	o << ",\"camera\":[" << m_attachDebug.cameraPosition.x << "," << m_attachDebug.cameraPosition.y << "," << m_attachDebug.cameraPosition.z << "]";
	o << ",\"cameraAngles\":[" << m_attachDebug.cameraAngles.pitch << "," << m_attachDebug.cameraAngles.yaw << "," << m_attachDebug.cameraAngles.roll << "]";
	o << ",\"distance\":" << m_attachDebug.distance;
	o << ",\"cameraDelta\":" << m_attachDebug.cameraDelta;
	o << ",\"targetDelta\":" << m_attachDebug.targetDelta;
	o << ",\"angleDelta\":" << m_attachDebug.angleDelta;
	o << ",\"aimError\":" << m_attachDebug.aimError;
	o << ",\"mountAngleError\":" << m_attachDebug.aimError;
	o << ",\"jitter\":" << m_attachDebug.jitter;
	o << ",\"smoothing\":" << m_attachDebug.smoothing;
	o << ",\"previewTick\":" << m_attachDebug.previewTick;
	o << ",\"demoTick\":" << m_attachDebug.demoTick;
	o << "}";
	o << ",\"candidates\":[";
	auto candidates = Candidates();
	const size_t limit = (std::min<size_t>)(candidates.size(), 24);
	for (size_t i = 0; i < limit; ++i) {
		if (i) o << ",";
		const auto& c = candidates[i];
		o << "{\"index\":" << c.entityIndex
			<< ",\"handle\":" << c.handle
			<< ",\"name\":\"" << JsonEscape(c.name) << "\""
			<< ",\"className\":\"" << JsonEscape(c.className) << "\""
			<< ",\"team\":\"" << TeamName(c.team) << "\""
			<< ",\"alive\":" << (c.alive ? "true" : "false")
			<< ",\"isWeapon\":" << (c.isWeapon ? "true" : "false")
			<< ",\"isBomb\":" << (c.isBomb ? "true" : "false")
			<< ",\"dropped\":" << (c.dropped ? "true" : "false")
			<< ",\"held\":" << (c.held ? "true" : "false")
			<< ",\"ownerIndex\":" << c.ownerIndex
			<< ",\"ownerName\":\"" << JsonEscape(c.ownerName) << "\""
			<< ",\"status\":\"" << JsonEscape(c.status) << "\""
			<< ",\"grenadeType\":\"" << JsonEscape(c.grenadeType) << "\""
			<< ",\"throwTick\":" << c.throwTick
			<< ",\"tickDelta\":" << c.tickDelta
			<< ",\"distance\":" << c.distance
			<< ",\"distanceLevel\":" << (c.distance >= kVeryFarDistance ? 3 : c.distance >= kFarDistance ? 2 : c.distance >= kMediumDistance ? 1 : 0)
			<< ",\"attachments\":[";
		for (size_t ai = 0; ai < c.attachments.size(); ++ai) {
			if (ai) o << ",";
			o << "\"" << JsonEscape(c.attachments[ai]) << "\"";
		}
		o << "],\"attachPoints\":[";
		for (size_t pi = 0; pi < c.attachPoints.size(); ++pi) {
			if (pi) o << ",";
			AppendAttachPointJson(o, c.attachPoints[pi]);
		}
		o << "]}";
	}
	o << "]"; // close candidates array

	// Recorded loadout events (weapon/C4 drops + pickups) from the .fmjson v5 pre-scan.
	const int evStatus = m_eventStatus.load();
	unsigned long long eventScanElapsedMs = 0;
	std::string eventDemoPath;
	std::string eventHelperPath;
	std::string eventError;
	size_t eventCount = 0;
	{
		std::lock_guard<std::mutex> lock(m_eventMutex);
		if (m_eventScanStartMs && evStatus == (int)EventStatus::Loading)
			eventScanElapsedMs = GetTickCount64() - m_eventScanStartMs;
		eventDemoPath = m_eventDemoPath.empty() ? m_eventLoadingPath : m_eventDemoPath;
		eventHelperPath = m_eventHelperPath;
		eventError = m_eventError;
		eventCount = m_events.size();
	}
	o << ",\"eventStatus\":" << evStatus;       // 0 idle 1 loading 2 ready 3 failed
	o << ",\"eventScanElapsedMs\":" << eventScanElapsedMs;
	o << ",\"eventDemoPath\":\"" << JsonEscape(eventDemoPath) << "\"";
	o << ",\"eventHelperPath\":\"" << JsonEscape(eventHelperPath) << "\"";
	o << ",\"eventError\":\"" << JsonEscape(eventError) << "\"";
	o << ",\"eventCount\":" << eventCount;
	o << ",\"selectedEvent\":" << m_selectedEvent;
	o << ",\"events\":[";
	if (m_state.targetType == FollowTargetType::Weapon) {
		auto events = EventsSnapshot();
		int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
		// Show the events nearest the playhead (a long demo has too many to ship every
		// frame); keep the original index so eventselect maps back to the full list.
		std::vector<size_t> order(events.size());
		for (size_t i = 0; i < events.size(); ++i) order[i] = i;
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			return std::abs(events[a].tick - curTick) < std::abs(events[b].tick - curTick);
		});
		if (order.size() > 60) order.resize(60);
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			return events[a].tick < events[b].tick;
		});
		for (size_t k = 0; k < order.size(); ++k) {
			if (k) o << ",";
			const size_t i = order[k];
			const auto& e = events[i];
			o << "{\"index\":" << i
				<< ",\"tick\":" << e.tick
				<< ",\"type\":\"" << JsonEscape(e.type) << "\""
				<< ",\"item\":\"" << JsonEscape(e.item) << "\""
				<< ",\"accountId\":" << e.accountId
				<< ",\"ownerName\":\"" << JsonEscape(e.ownerName) << "\"}";
		}
	}
	o << "]}";
	return o.str();
}

void FollowCamera::PrintStatus() const {
	advancedfx::Message(
		"[followcam] enabled=%d hasCamera=%d type=%s entity=%d fov=%.1f "
		"offset=(%.1f %.1f %.1f) look=%.3f pos=%.3f prediction=%.3f "
		"deadzone=%.2f maxTurn=%.1f autoDead=%d switchWeapon=%d switchBomb=%d hold=%d debug=%d rtsample=%d.\n",
		m_state.enabled ? 1 : 0, m_state.hasCamera ? 1 : 0,
		FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex, m_state.fov,
		m_state.offset.x, m_state.offset.y, m_state.offset.z,
		m_state.lookSmoothing, m_state.positionSmoothing, m_state.prediction,
		m_state.deadzone, m_state.maxTurnSpeed,
		m_state.autoDisableOnDeath ? 1 : 0, m_state.switchToDroppedWeaponOnDeath ? 1 : 0,
		m_state.switchToDroppedBombOnDeath ? 1 : 0, m_state.holdLastKnownPosition ? 1 : 0,
		m_debug ? 1 : 0, m_renderTimeSample ? 1 : 0);
}

void FollowCamera::PrintCamPose() const {
	advancedfx::Message(
		"[followcam][campose] enabled=%d mode=%s type=%s pos=(%.2f %.2f %.2f) "
		"ang=(%.2f %.2f %.2f) fov=%.1f target=(%.2f %.2f %.2f).\n",
		m_state.enabled ? 1 : 0,
		m_state.mode == FollowMode::Attach ? "attach" : "lockon",
		FollowTargetTypeName(m_state.targetType),
		m_lastCamPos.x, m_lastCamPos.y, m_lastCamPos.z,
		m_lastCamAng.pitch, m_lastCamAng.yaw, m_lastCamAng.roll, m_state.fov,
		m_lastTargetPos.x, m_lastTargetPos.y, m_lastTargetPos.z);
}

void FollowCamera::EnsureEventsLoaded() const {
	const char* p = g_pEngineToClient ? g_pEngineToClient->GetDemoFilePath() : nullptr;
	std::string path = p ? p : "";
	if (path.empty()) return;

	{
		std::lock_guard<std::mutex> lock(m_eventMutex);
		const int status = m_eventStatus.load();
		if (m_eventDemoPath == path && status == (int)EventStatus::Ready) return;
		if (m_eventDemoPath == path && status == (int)EventStatus::Failed) return;
		if (m_eventLoadingPath == path && status == (int)EventStatus::Loading) return;

		m_eventLoadingPath = path;
		m_eventDemoPath.clear();
		m_events.clear();
		m_eventError.clear();
		m_eventHelperPath.clear();
		m_eventScanStartMs = GetTickCount64();
		m_eventStatus.store((int)EventStatus::Loading);
	}

	// Join any previous worker before starting a new one (done OUTSIDE m_eventMutex so the
	// old worker's final commit, which takes that lock, can't deadlock the join). Cancelling
	// aborts its in-flight helper process promptly so the join is bounded.
	if (m_eventThread.joinable()) {
		m_eventCancel.store(true);
		m_eventThread.join();
	}
	m_eventCancel.store(false);

	// Full parse on a joinable worker (multi-second); the demo path is keyed so a demo
	// change relaunches. ReadDemoInfoViaHelper reuses/refreshes the "<demo>.fmjson" cache
	// and honors m_eventCancel so Shutdown() can stop it.
	m_eventThread = std::thread([this, path]() {
		const std::wstring wpath = Utf8ToWide(path);
		DemoHelperResult result = ReadDemoInfoViaHelper(wpath, {}, &m_eventCancel);
		if (m_eventCancel.load()) return; // shutdown / superseded: don't touch shared state
		std::unordered_map<uint32_t, std::string> names;
		for (const auto& pl : result.players) names[pl.accountId] = pl.name;
		std::vector<FollowEventRecord> evs;
		evs.reserve(result.events.size());
		for (const auto& e : result.events) {
			FollowEventRecord f;
			f.tick = e.tick; f.type = e.type; f.item = e.item; f.accountId = e.accountId;
			auto it = names.find(e.accountId);
			f.ownerName = (it != names.end()) ? it->second : std::string();
			evs.push_back(std::move(f));
		}
		std::lock_guard<std::mutex> lk(m_eventMutex);
		if (m_eventLoadingPath == path) {
			m_events = std::move(evs);
			m_eventDemoPath = path;
			m_eventHelperPath = result.helperPath;
			m_eventError = result.ok ? std::string() : (result.error.empty() ? "demo event scan failed" : result.error);
			m_eventScanStartMs = 0;
			m_eventStatus.store(result.ok ? (int)EventStatus::Ready : (int)EventStatus::Failed);
		}
	});
}

void FollowCamera::Shutdown() {
	m_eventCancel.store(true);
	if (m_eventThread.joinable())
		m_eventThread.join();
}

std::vector<FollowEventRecord> FollowCamera::EventsSnapshot() const {
	std::lock_guard<std::mutex> lock(m_eventMutex);
	return m_events;
}

bool FollowCamera::SelectEvent(int eventIndex) {
	auto events = EventsSnapshot();
	if (eventIndex < 0 || eventIndex >= (int)events.size()) {
		m_selectedEvent = -1;
		return false;
	}
	m_selectedEvent = eventIndex;
	const auto& e = events[eventIndex];
	m_lastMessage = "Event: " + e.type + " @ tick " + std::to_string(e.tick)
		+ (e.item.empty() ? "" : (" (" + e.item + ")")) + ".";
	return true;
}

bool FollowCamera::PreviewTick() {
	// Grenades use the throw-tick state machine (seek + reacquire the projectile).
	if (m_state.targetType == FollowTargetType::Grenade)
		return TrackSelectedGrenade();

	auto events = EventsSnapshot();
	if (m_selectedEvent < 0 || m_selectedEvent >= (int)events.size()) {
		advancedfx::Warning("[followcam] preview tick: select a recorded event first.\n");
		m_lastMessage = "Select a drop/pickup event first.";
		return false;
	}
	const auto& e = events[m_selectedEvent];
	const int seekTick = (std::max)(0, e.tick - 40);
	// A drop leaves the weapon owner-less, so default to the dropped entity; the live
	// candidate list around this tick then exposes it for selection / attach.
	if (e.type == "weapon_drop" || e.type == "bomb_dropped")
		m_state.weaponSource = WeaponSource::Dropped;
	if (g_pEngineToClient) {
		std::ostringstream cmd; cmd << "demo_gototick " << seekTick;
		g_pEngineToClient->ExecuteClientCmd(0, cmd.str().c_str(), true);
	}
	m_lastMessage = "Seeking to tick " + std::to_string(seekTick) + " ("
		+ e.type + (e.ownerName.empty() ? "" : (" - " + e.ownerName)) + ").";
	advancedfx::Message("[followcam] preview tick: %s item=%s seek=%d.\n",
		e.type.c_str(), e.item.c_str(), seekTick);
	return true;
}

} // namespace Filmmaker
