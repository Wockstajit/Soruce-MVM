// ParticleFx rules: name classification into FxCategory, the FXRULE swap tables mapping
// vanilla CS2 systems to the converted Povarehok / MW2019 "Modern" pack assets, per-mode
// target selection, and swap-target pre-queueing so the resolver stays ahead of creations.

#include "ParticleFxInternal.h"

#include <cstring>

namespace Filmmaker {
namespace fx {

// ============================== classification =====================================

void LowerCopy(const char* in, char* out, size_t cap) {
	size_t i = 0;
	for (; in[i] && i + 1 < cap; ++i) {
		char c = in[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (c == '\\') c = '/';
		out[i] = c;
	}
	out[i] = 0;
}

namespace {

bool StartsWith(const char* s, const char* prefix) {
	return 0 == std::strncmp(s, prefix, std::strlen(prefix));
}

} // namespace

// n must already be lowercase (LowerCopy). Returns a FxCategory or -1 (untracked).
// Prefixes verified against the build-14166 pak01 particle list (Source2Viewer dump):
// HE grenades detonate as particles/entity/env_explosion/explosion_hegrenade_*, blood
// also appears under impact_fx/ and as explosions_fx/ screen splatter, and current maps
// (mirage etc.) spawn ambience from the LEGACY particles/maps/de_dust/... names plus the
// generic ambient_fx/environment/rain_fx folders.
int ClassifyLower(const char* n) {
	if (StartsWith(n, "particles/ui/")) return -1;         // HUD/MVP effects: never touch
	// The per-grenade spectator/X-ray utility trail: CS2 creates one per thrown grenade
	// during demo playback with its control point following the projectile, which makes it
	// the swap anchor for the GMod-style flight smoke trail (Modern mode). Routed to the
	// weaponfx category; its variant row is Modern-only (On/Less pass the stock line through).
	if (0 == std::strcmp(n, "particles/entity/spectator_utility_trail.vpcf")) return kFxWeaponFx;
	if (std::strstr(n, "blood")) return kFxBlood;          // wins over folder rules
	if (StartsWith(n, "particles/impact_fx/") || StartsWith(n, "particles/water_impact/")
		|| StartsWith(n, "particles/breakable_fx/")) return kFxImpacts;
	if (StartsWith(n, "particles/weapons/cs_weapon_fx/") || StartsWith(n, "particles/unified_weapon_fx/"))
		return std::strstr(n, "tracer") ? kFxTracers : kFxWeaponFx;
	if (StartsWith(n, "particles/explosions_fx/") || StartsWith(n, "particles/entity/env_explosion/"))
		return (std::strstr(n, "c4") || std::strstr(n, "bomb")) ? kFxBombFx : kFxExplosions;
	if (StartsWith(n, "particles/burning_fx/") || StartsWith(n, "particles/inferno_fx")) return kFxMolotov;
	if (StartsWith(n, "particles/maps/") || StartsWith(n, "particles/ambient_fx/")
		|| StartsWith(n, "particles/environment/") || StartsWith(n, "particles/rain_fx/")
		|| StartsWith(n, "particles/critters/")) return kFxMapFx;
	return -1;
}

const char* kCategoryKeys[kFxCategoryCount] = {
	"impacts", "tracers", "weaponfx", "blood", "explosions", "bombfx", "molotov", "mapfx"
};
const char* kModeNames[kModeCount] = { "on", "less", "off", "more", "modern" };

// Which modes each category really has (see FxMode's header comment). Less only where the
// mod's less folders differ (impact_fx + explosions_fx); Modern only where the MW2019 pack
// ships assets (tracers, weapon fx, HE explosion).
bool ModeSupported(int cat, FxMode mode) {
	switch (mode) {
	case FxMode::On:
	case FxMode::Off:
		return true;
	case FxMode::More: // legacy alias, normalized to On before storage
		return true;
	case FxMode::Less:
		return cat == kFxImpacts || cat == kFxExplosions || cat == kFxBombFx;
	case FxMode::Modern:
		return cat == kFxTracers || cat == kFxWeaponFx || cat == kFxExplosions;
	default:
		return false;
	}
}

// Storage normalization: More -> On (legacy), unsupported Less/Modern -> On.
FxMode NormalizeMode(int cat, FxMode mode) {
	if (mode == FxMode::More)
		mode = FxMode::On;
	if (!ModeSupported(cat, mode))
		mode = FxMode::On;
	return mode;
}

int CategoryFromKey(const char* key) {
	for (int i = 0; i < kFxCategoryCount; ++i)
		if (0 == _stricmp(key, kCategoryKeys[i]))
			return i;
	return -1;
}

int ModeFromName(const char* name) {
	// "more" was dropped 2026-07-02 (it targeted a byte-identical asset folder, so On and
	// More never looked different); accept it as an alias of On so old persisted configs
	// and muscle-memory console commands keep working.
	if (0 == _stricmp(name, "more"))
		return (int)FxMode::On;
	for (int i = 0; i < kModeCount; ++i)
		if (0 == _stricmp(name, kModeNames[i]))
			return i;
	return -1;
}

// ============================== the FXRULE swap tables =============================

namespace {

// Mode targets are real Source 1 Povarehok variants converted to Source 2 with
// long0900/source1import and mounted under particles/filmmaker/povarehok/.
//
// Mode mapping from the original CS:GO mod folders (2026-07-02 restructure: the old
// "classic" and "classic updated" folders were BYTE-IDENTICAL, so the old On-vs-More split
// showed no visual difference and the user deleted the plain-classic folder; More was
// dropped and now aliases On for old persisted configs):
//   On   -> p_betterparticlesmod_classic updated_c057b (the one enhanced tier), staged as
//           povarehok/regular
//   Less -> a per-FILE combination of the two "less" folders. Diffed 2026-07-02: the ONLY
//           files that differ from regular at all are impact_fx.pcf (differs in BOTH less
//           folders, each its own way) and explosions_fx.pcf (identical "less" version in
//           both). So: bullet impacts -> povarehok/less/impacts' impact_fx (the reduced-
//           impacts flavor), everything else -> povarehok/less/smoke (whose explosions_fx is
//           the shared less version; its remaining files equal regular, so Less = On for
//           blood/tracers/muzzle/molotov by the mod author's own content).
//
// If the converted pack is not mounted, resolution fails open and the original CS2 effect
// plays. There are no stock CS2 placeholder substitutions here.
//
// `modern` is the optional MW2019 (ARC9/GMod, converted via the same source1import
// pipeline) target under particles/filmmaker/modern/<pcf>/<system>.vpcf. Ground-truth
// mapping extracted from the pack's own weapon Lua 2026-07-03 (memory:
// modern-pack-mw2019-mapping): rifles muzzleflash_ar, SMGs+snipers muzzleflash_smg,
// marksman muzzleflash_dmr, pistols muzzleflash_pistol(_deagle), shotguns
// muzzleflash_shotgun, silenced muzzleflash_suppressed, per-shot barrel smoke
// barrel_smoke(_plume), tracers the mw2019_tracer family, HE frag explosion_grenade.
struct VariantRule {
	const char* match;
	const char* on;
	const char* lessImpacts;
	const char* lessSmoke;
	const char* modern; // null = category Modern mode passes this system through
};

#define FXVAR(variant, pack, name) "particles/filmmaker/povarehok/" variant "/" pack "/" name ".vpcf"
#define FXRULE(match, pack, name) { match, \
	FXVAR("regular", pack, name), \
	FXVAR("less/impacts", pack, name), FXVAR("less/smoke", pack, name), nullptr }
// Same as FXRULE plus a converted-MW2019 target ("<pcf folder>/<system>" under modern/).
#define FXRULE_MODERN(match, pack, name, modernRel) { match, \
	FXVAR("regular", pack, name), \
	FXVAR("less/impacts", pack, name), FXVAR("less/smoke", pack, name), \
	"particles/filmmaker/modern/" modernRel ".vpcf" }
// FXRULE whose Modern target is the Povarehok asset itself: used for the brass
// shell casings, whose converted systems render the actual MW2019 shell models
// (models/shells/* -- see the model aliases in convert-povarehok-source1.ps1),
// so reusing them under Modern is not a pack mix-up.
#define FXRULE_MODERN_BP(match, pack, name) { match, \
	FXVAR("regular", pack, name), \
	FXVAR("less/impacts", pack, name), FXVAR("less/smoke", pack, name), \
	FXVAR("regular", pack, name) }

const VariantRule kVariantBlood[] = {
	FXRULE("particles/blood_impact/blood_impact_basic.vpcf",          "blood_impact", "1.cinematic_blood_impact_v2"),
	FXRULE("particles/blood_impact/blood_impact_light.vpcf",          "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_medium.vpcf",         "blood_impact", "1.cinematic_blood_impact_v2"),
	FXRULE("particles/blood_impact/blood_impact_high.vpcf",           "blood_impact", "blood_impact_heavy"),
	FXRULE("particles/blood_impact/blood_impact_heavy.vpcf",          "blood_impact", "blood_impact_heavy"),
	FXRULE("particles/blood_impact/blood_impact_low.vpcf",            "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_med.vpcf",            "blood_impact", "1.cinematic_blood_impact_v2"),
	FXRULE("particles/blood_impact/blood_impact_friendly.vpcf",       "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_localfrontenemy.vpcf", "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_localplayer.vpcf",    "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_headshot.vpcf",       "blood_impact", "blood_impact_headshot"),
	FXRULE("particles/blood_impact/blood_impact_light_headshot.vpcf", "blood_impact", "blood_impact_light_headshot"),
};
const VariantRule kVariantImpacts[] = {
	FXRULE("particles/impact_fx/impact_concrete.vpcf",    "impact_fx", "impact_concrete"),
	FXRULE("particles/impact_fx/impact_plaster.vpcf",     "impact_fx", "impact_plaster"),
	FXRULE("particles/impact_fx/impact_brick.vpcf",       "impact_fx", "impact_brick"),
	FXRULE("particles/impact_fx/impact_tile.vpcf",        "impact_fx", "impact_tile"),
	FXRULE("particles/impact_fx/impact_sheetrock.vpcf",   "impact_fx", "impact_sheetrock"),
	FXRULE("particles/impact_fx/impact_asphalt.vpcf",     "impact_fx", "impact_asphalt"),
	FXRULE("particles/impact_fx/impact_rock.vpcf",        "impact_fx", "impact_rock"),
	FXRULE("particles/impact_fx/impact_dirt.vpcf",        "impact_fx", "impact_dirt"),
	FXRULE("particles/impact_fx/impact_plastic.vpcf",     "impact_fx", "impact_plastic"),
	FXRULE("particles/impact_fx/impact_metal.vpcf",       "impact_fx", "impact_metal"),
	FXRULE("particles/impact_fx/impact_metal_grate.vpcf", "impact_fx", "impact_metal"),
	FXRULE("particles/impact_fx/impact_metal_vent.vpcf",  "impact_fx", "impact_metal"),
	FXRULE("particles/impact_fx/impact_wood.vpcf",        "impact_fx", "impact_wood"),
	FXRULE("particles/impact_fx/impact_wallbang_light.vpcf",        "impact_fx", "impact_wallbang_light"),
	FXRULE("particles/impact_fx/impact_wallbang_light_silent.vpcf", "impact_fx", "impact_wallbang_light"),
	FXRULE("particles/impact_fx/impact_wallbang_heavy.vpcf",        "impact_fx", "impact_wallbang_heavy"),
	FXRULE("particles/impact_fx/impact_ricochet.vpcf",    "impact_fx", "impact_ricochet"),
};
// Modern tracers: the pack's snipers use mw2019_tracer_fast, pistols _small, shotguns
// _slow, everything automatic the plain mw2019_tracer (the pack's own Lua assignments).
const VariantRule kVariantTracers[] = {
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers.vpcf",              "weapons/cs_weapon_fx", "weapon_tracers",          "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_pistol.vpcf",       "weapons/cs_weapon_fx", "weapon_tracers_pistol",   "mw2019_tracer/mw2019_tracer_small"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_smg.vpcf",          "weapons/cs_weapon_fx", "weapon_tracers_smg",      "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_rifle.vpcf",        "weapons/cs_weapon_fx", "weapon_tracers_rifle",    "mw2019_tracer/mw2019_tracer_fast"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_rifle_scar.vpcf",   "weapons/cs_weapon_fx", "weapon_tracers_rifle",    "mw2019_tracer/mw2019_tracer_fast"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_rifle_ssg.vpcf",    "weapons/cs_weapon_fx", "weapon_tracers_rifle",    "mw2019_tracer/mw2019_tracer_fast"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_assrifle.vpcf",     "weapons/cs_weapon_fx", "weapon_tracers_assrifle", "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_assrifle_aug.vpcf", "weapons/cs_weapon_fx", "weapon_tracers_assrifle", "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_mach.vpcf",         "weapons/cs_weapon_fx", "weapon_tracers_mach",     "mw2019_tracer/mw2019_tracer"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_tracers_shot.vpcf",         "weapons/cs_weapon_fx", "weapon_tracers_shot",     "mw2019_tracer/mw2019_tracer_slow"),
};
const VariantRule kVariantWeaponFx[] = {
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_ak47.vpcf",         "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle",    "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_ak47_fps.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle.vpcf",       "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle",    "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle_fps.vpcf",   "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_aug.vpcf",          "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle",    "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_aug_fps.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_shot.vpcf",         "weapons/cs_weapon_fx", "weapon_muzzle_flash_shotgun",         "arc9_fas_muzzleflashes/muzzleflash_shotgun"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_shot_fps.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_shotgun_fp",      "arc9_fas_muzzleflashes/muzzleflash_shotgun"),
	// The MW2019 pack's own big-bore snipers (AX-50/HDR/Rytec) use the SMG flash; the mvm
	// sniper composition wraps it with the pack's .50-cal extras the mod adds via Lua --
	// M82 shock-dust ring around the shooter, heavy barrel plume, muzzle heat distortion
	// (synthesized by postprocess_modern.py's apply_modern_gameplay_composites, user request 2026-07-03).
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_awp",             "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_awp"),
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip_fps.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_huntingrifle_fp", "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_awp"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_subm.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg",             "arc9_fas_muzzleflashes/muzzleflash_smg"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_subm_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg_fp",          "arc9_fas_muzzleflashes/muzzleflash_smg"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_subm.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg_silenced",    "arc9_fas_muzzleflashes/muzzleflash_suppressed"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_subm_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg_silenced_fp", "arc9_fas_muzzleflashes/muzzleflash_suppressed"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_rif.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_silenced", "arc9_fas_muzzleflashes/muzzleflash_suppressed"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzsilenced_rif_fps.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_silenced", "arc9_fas_muzzleflashes/muzzleflash_suppressed"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol",          "arc9_fas_muzzleflashes/muzzleflash_pistol"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol_fp",       "arc9_fas_muzzleflashes/muzzleflash_pistol"),
	// Deagle/R8, ironsight (scoped) AUG/SG556, and LMG muzzles are separate
	// top-level systems (verified via `fx names` on a live demo, 2026-07-02).
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_deagle.vpcf",            "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol",    "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_deagle_fps.vpcf",        "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol_fp", "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_aug_fps_ironsight.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_sg_fps_ironsight.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	// riffle_lrg = SCAR-20/G3SG1 autosnipers: the pack's Rytec/AX-50 class (SMG flash).
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg.vpcf",        "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle",    "arc9_fas_muzzleflashes/muzzleflash_smg"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg_fps.vpcf",    "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp", "arc9_fas_muzzleflashes/muzzleflash_smg"),
	// weapon_muzzleflash_snip_ar(_fps) is the REAL top-level auto-sniper (SCAR-20/G3SG1)
	// system (verified via `fx names` on the all-weapons test demo 2026-07-03 -- it was
	// unmapped and passing through 100% vanilla, the actual cause of a user-reported
	// "autosniper doesn't match the rest" look). No dedicated regular auto-sniper
	// asset exists in the mod, so On/Less reuse its AWP flash (same "premium sniper" choice
	// Modern already makes); Modern gets the mvm sniper composition around the pack's DMR
	// flash (shock-dust ring + plume + heat distortion, like the AWP above).
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip_ar.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_awp", "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_auto"),
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_snip_ar_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_awp", "arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_auto"),
	// uweapon_muzflsh_mach(_fps) is the LMG (M249/Negev) top-level system -- also unmapped
	// (verified live 2026-07-03), so LMGs got 100% vanilla flash under Modern. "para" is the
	// mod's own LMG-class naming; the arc9 pack ships a dedicated LMG flash.
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_mach.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_para",    "arc9_fas_muzzleflashes/muzzleflash_lmg"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzflsh_mach_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_para_fp", "arc9_fas_muzzleflashes/muzzleflash_lmg"),
	// The thrown-molotov flight trail (the "smoke trail that follows the grenade" a user
	// reported missing 2026-07-03): unmapped before, so it played 100% vanilla CS2's own
	// (much thinner) trail. Povarehok ships its own weapon_molotov_thrown +
	// children; no Modern-pack equivalent exists so Modern still passes through vanilla
	// here. Incendiary reuses the same target (the mod has no separate incendiary asset,
	// matching the existing incendiary/molotov merge in kVariantMolotov below).
	FXRULE("particles/weapons/cs_weapon_fx/weapon_molotov_thrown.vpcf", "weapons/cs_weapon_fx", "weapon_molotov_thrown"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_incend_thrown.vpcf",  "weapons/cs_weapon_fx", "weapon_molotov_thrown"),
	// R8 primary fire ("fanning") uses its own pist_revolver systems, distinct
	// from the deagle-shared secondary fire.
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol",    "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver_fps.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol_fp", "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	FXRULE_MODERN("particles/unified_weapon_fx/uweapon_muzzleflash_pist_fire_revolver.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol",    "arc9_fas_muzzleflashes/muzzleflash_pistol_deagle"),
	// Sustained-fire barrel smoke: On/Less swap to the pack's weapon_muzzle_smoke_long
	// (per-shot wisps + lingering plume). Modern uses the MW2019 barrel_smoke assets.
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke.vpcf",             "weapons/cs_weapon_fx", "weapon_muzzle_smoke"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b.vpcf",           "weapons/cs_weapon_fx", "weapon_muzzle_smoke_b"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b_version_2.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_smoke_b_version_#2"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long.vpcf",        "weapons/cs_weapon_fx", "weapon_muzzle_smoke_long"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long_b.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_smoke_long_b"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke.vpcf",             "weapons/cs_weapon_fx", "weapon_muzzle_smoke",             "arc9_fas_muzzleflashes/barrel_smoke"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b.vpcf",           "weapons/cs_weapon_fx", "weapon_muzzle_smoke_b",           "arc9_fas_muzzleflashes/barrel_smoke"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b_version_2.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_smoke_b_version_#2", "arc9_fas_muzzleflashes/barrel_smoke"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long.vpcf",        "weapons/cs_weapon_fx", "weapon_muzzle_smoke_long",        "arc9_fas_muzzleflashes/barrel_smoke_plume"),
	FXRULE_MODERN("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long_b.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_smoke_long_b",      "arc9_fas_muzzleflashes/barrel_smoke_plume"),
	// Brass shell casings (user report 2026-07-03 "supposed to replace the shells"): the mod
	// ships its own weapon_shell_casing_* systems rendering real converted shell meshes, but
	// they were never mapped, so vanilla brass always passed through in every mode. CS2 has
	// more caliber variants than the mod; nearest-caliber mapping (45acp/57 -> 9mm pistol
	// brass, mag7/nova -> the one shotgun shell, AWP -> the .50 cal). The cosmetic-only
	// weapon_shell_casing_super_trail stays untouched.
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_9mm.vpcf",           "weapons/cs_weapon_fx", "weapon_shell_casing_9mm"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_45acp.vpcf",         "weapons/cs_weapon_fx", "weapon_shell_casing_9mm"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_57.vpcf",            "weapons/cs_weapon_fx", "weapon_shell_casing_9mm"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_rifle.vpcf",         "weapons/cs_weapon_fx", "weapon_shell_casing_rifle"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_awp.vpcf",           "weapons/cs_weapon_fx", "weapon_shell_casing_50cal"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_deagle.vpcf",        "weapons/cs_weapon_fx", "weapon_shell_casing_deagle"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun.vpcf",       "weapons/cs_weapon_fx", "weapon_shell_casing_shotgun"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun_mag7.vpcf",  "weapons/cs_weapon_fx", "weapon_shell_casing_shotgun"),
	FXRULE_MODERN_BP("particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun_nova.vpcf",  "weapons/cs_weapon_fx", "weapon_shell_casing_shotgun"),
	FXRULE_MODERN("particles/unified_weapon_fx/weapon_muzzleflash_basic.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle", "arc9_fas_muzzleflashes/muzzleflash_ar"),
	// Near-ground muzzle blast dust: CS2 spawns this system by proximity, the MW2019 mod's
	// .50-cal Lua spawns engine ThumperDust at the feet -- the FAS M82 shock dust is the
	// pack's own PCF equivalent (Modern only; Povarehok has no version of this).
	{ "particles/unified_weapon_fx/uweapon_muzflsh_ground_smoke.vpcf", nullptr, nullptr, nullptr,
	  "particles/filmmaker/modern/arc9_fas_muzzleflashes/m82_shocksmoke.vpcf" },
	// GMod-style grenade flight smoke trail (user request 2026-07-03): CS2 has NO in-flight
	// particle for HE/smoke/flash/decoy at all -- but demo playback creates one
	// spectator_utility_trail per thrown grenade, control-pointed to the projectile, so
	// swapping it re-anchors a real smoke trail onto every grenade. mvm_grenade_trail is
	// synthesized from the pack's own barrel_smoke_trail systems with un-capped emission
	// (postprocess_modern.py's apply_modern_gameplay_composites). Modern only; On/Less keep the stock line.
	{ "particles/entity/spectator_utility_trail.vpcf", nullptr, nullptr, nullptr,
	  "particles/filmmaker/modern/arc9_fas_muzzleflashes/mvm_grenade_trail.vpcf" },
};
// HE grenade (+ generic env explosions). Modern = the MW2019 frag's real detonation
// system (thrownfrag entity -> cod2019_grenade_explosion effect -> explosion_grenade
// from the FAS explosions PCF).
const VariantRule kVariantExplosions[] = {
	FXRULE_MODERN("particles/explosions_fx/explosion_basic.vpcf",              "explosions_fx", "explosion_basic",               "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/explosions_fx/explosion_hegrenade_brief.vpcf",    "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_a.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_b.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_c.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_d.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_e.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_f.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_g.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	FXRULE_MODERN("particles/entity/env_explosion/explosion_hegrenade_h.vpcf", "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
	// The all-weapons test demo (2026-07-03) showed current CS2 HE detonations ALSO create
	// this plain env_explosion/explosion_grenade name (4 creations, none acted) alongside
	// explosion_basic -- an unmapped chunk of every HE blast played vanilla.
	FXRULE_MODERN("particles/entity/env_explosion/explosion_grenade.vpcf",     "explosions_fx", "explosion_hegrenade_interior",  "arc9_fas_explosions/explosion_grenade"),
};
// The planted-bomb blast, split from the HE category so the two can differ (e.g. Povarehok
// bomb + Modern HE). Povarehok ships its own explosion_c4_500 (verified
// in the regular and less/smoke converted trees); the CS2 engine picks the
// exterior/interior/short variant by site, all mapped to the mod's one bomb system.
// Deliberately NO Modern target: the MW2019 pack's C4 is a small breaching charge, not a
// bomb-site blast (user call, 2026-07-03).
const VariantRule kVariantBomb[] = {
	FXRULE("particles/explosions_fx/explosion_c4_500.vpcf",          "explosions_fx", "explosion_c4_500"),
	FXRULE("particles/explosions_fx/explosion_c4_500_fallback.vpcf", "explosions_fx", "explosion_c4_500"),
	FXRULE("particles/explosions_fx/explosion_c4_interior.vpcf",     "explosions_fx", "explosion_c4_500"),
	FXRULE("particles/explosions_fx/explosion_c4_short.vpcf",        "explosions_fx", "explosion_c4_500"),
};
const VariantRule kVariantMolotov[] = {
	FXRULE("particles/inferno_fx/molotov_groundfire.vpcf",    "inferno_fx", "molotov_groundfire_00high"),
	FXRULE("particles/inferno_fx/incendiary_groundfire.vpcf", "inferno_fx", "molotov_groundfire_00high"),
	FXRULE("particles/inferno_fx/molotov_fire01.vpcf",        "inferno_fx", "molotov_fire01"),
	FXRULE("particles/inferno_fx/molotov_explosion.vpcf",     "inferno_fx", "molotov_explosion"),
	FXRULE("particles/inferno_fx/incendiary_explosion.vpcf",  "inferno_fx", "molotov_explosion"),
	// The dying-embers tail system after the main groundfire burns down; unmapped before
	// (verified live 2026-07-03: seen but never acted), so it played vanilla CS2 fire
	// while the main blaze used the converted asset -- a visible mismatch at the end of
	// every molotov's burn. "fallback" is the mod's own closest reduced-fire variant.
	FXRULE("particles/inferno_fx/molotov_groundfire_remnant.vpcf", "inferno_fx", "molotov_groundfire_fallback"),
};

#undef FXRULE_MODERN_BP
#undef FXRULE_MODERN
#undef FXRULE
#undef FXVAR

// ============================== target selection ===================================

const char* SelectVariantTarget(int cat, FxMode mode, const VariantRule& rule) {
	switch (mode) {
	case FxMode::On:
	case FxMode::More: // legacy alias (pre-restructure persisted configs); same target as On
		return rule.on;
	case FxMode::Less:
		return cat == kFxImpacts ? rule.lessImpacts : rule.lessSmoke;
	case FxMode::Modern:
		// null modern target = this system has no MW2019 equivalent; pass through vanilla
		// (do NOT silently mix Povarehok into a Modern category).
		return rule.modern;
	default:
		return nullptr;
	}
}

// Fallback for CS2 tracer systems NOT in kVariantTracers' exact-match table (root-cause
// fix for "tracers don't fire for every weapon", 2026-07-03). The muzzle-flash table
// needed several live-demo-discovered rows beyond what "obvious" pak names suggested
// (weapon_muzzleflash_snip_ar was a hidden top-level name found only via `fx names` on
// a live demo -- see the kVariantWeaponFx comment above); tracers were never given that
// same live audit, and a purely exact-match table is perpetually one CS2
// update/unaudited-weapon-class behind. Rather than guess a literal name with no live
// data to confirm it (the repo rule is "map from live `fx names`, never from pak
// listings"), classify ANY untabled kFxTracers name by weapon-class substring, mirroring
// the groupings the exact table already encodes (e.g. it already folds rifle_scar and
// rifle_ssg into the same "rifle" bucket). A name this table has never seen still gets a
// sensible pack tracer instead of silently staying 100% vanilla in both On and Modern.
struct TracerFallback {
	const char* substr;
	const char* on;     // Povarehok regular pack file (weapons/cs_weapon_fx/<on>.vpcf)
	const char* modern; // mw2019_tracer/<modern>.vpcf
};
const TracerFallback kTracerFallbacks[] = {
	{ "pistol",    "weapon_tracers_pistol",   "mw2019_tracer_small" },
	{ "revolver",  "weapon_tracers_pistol",   "mw2019_tracer_small" },
	{ "deagle",    "weapon_tracers_pistol",   "mw2019_tracer_small" },
	{ "shot",      "weapon_tracers_shot",     "mw2019_tracer_slow" },
	{ "mach",      "weapon_tracers_mach",     "mw2019_tracer" },
	{ "para",      "weapon_tracers_mach",     "mw2019_tracer" },
	{ "smg",       "weapon_tracers_smg",      "mw2019_tracer" },
	{ "aug",       "weapon_tracers_assrifle", "mw2019_tracer" },
	{ "assrifle",  "weapon_tracers_assrifle", "mw2019_tracer" },
	{ "ssg",       "weapon_tracers_rifle",    "mw2019_tracer_fast" },
	{ "snip",      "weapon_tracers_rifle",    "mw2019_tracer_fast" },
	{ "awp",       "weapon_tracers_rifle",    "mw2019_tracer_fast" },
	{ "scar",      "weapon_tracers_rifle",    "mw2019_tracer_fast" },
	{ "lrg",       "weapon_tracers_rifle",    "mw2019_tracer_fast" },
	{ "rifle",     "weapon_tracers_rifle",    "mw2019_tracer_fast" },
};
constexpr const char* kTracerFallbackDefaultOn = "weapon_tracers";
constexpr const char* kTracerFallbackDefaultModern = "mw2019_tracer";

// Tracers only support On/Modern (never Less -- see ModeSupported), so no lessImpacts/
// lessSmoke case here. Returns a pointer valid until the next call on this thread
// (thread_local buffer; the caller copies it into a std::string immediately).
const char* TracerFallbackTarget(FxMode mode, const char* n) {
	if (mode != FxMode::On && mode != FxMode::Modern)
		return nullptr;
	const char* onName = kTracerFallbackDefaultOn;
	const char* modernName = kTracerFallbackDefaultModern;
	for (const TracerFallback& f : kTracerFallbacks) {
		if (std::strstr(n, f.substr)) {
			onName = f.on;
			modernName = f.modern;
			break;
		}
	}
	static thread_local std::string s_buf;
	if (mode == FxMode::On)
		s_buf = std::string("particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx/") + onName + ".vpcf";
	else
		s_buf = std::string("particles/filmmaker/modern/mw2019_tracer/") + modernName + ".vpcf";
	return s_buf.c_str();
}

} // namespace

const char* VariantTargetLower(int cat, FxMode mode, const char* n) {
	if (mode == FxMode::Off)
		return nullptr;
	const VariantRule* rules = nullptr;
	size_t count = 0;
	switch (cat) {
	case kFxWeaponFx:   rules = kVariantWeaponFx;   count = sizeof(kVariantWeaponFx) / sizeof(rules[0]); break;
	case kFxTracers:    rules = kVariantTracers;    count = sizeof(kVariantTracers) / sizeof(rules[0]); break;
	case kFxBlood:      rules = kVariantBlood;      count = sizeof(kVariantBlood) / sizeof(rules[0]); break;
	case kFxExplosions: rules = kVariantExplosions; count = sizeof(kVariantExplosions) / sizeof(rules[0]); break;
	case kFxBombFx:     rules = kVariantBomb;       count = sizeof(kVariantBomb) / sizeof(rules[0]); break;
	case kFxImpacts:    rules = kVariantImpacts;    count = sizeof(kVariantImpacts) / sizeof(rules[0]); break;
	case kFxMolotov:    rules = kVariantMolotov;    count = sizeof(kVariantMolotov) / sizeof(rules[0]); break;
	default: return nullptr;
	}
	for (size_t i = 0; i < count; ++i)
		if (0 == std::strcmp(n, rules[i].match))
			return SelectVariantTarget(cat, mode, rules[i]);
	if (cat == kFxTracers)
		return TracerFallbackTarget(mode, n);
	return nullptr;
}

// ============================== swap-target pre-queueing ===========================

// Queue every name current settings could swap to, so targets are (re)resolved ahead of
// the first creation that needs them. Called on install and on every settings change.
void QueueActiveSwapTargetsLocked() {
	auto add = [](const char* n) {
		if (g_handleCache.find(n) != g_handleCache.end())
			return;
		for (const std::string& q : g_resolveQueue)
			if (q == n)
				return;
		g_resolveQueue.push_back(n);
	};
	add(kEmptySystem);
	if (g_moneyHeadshot)
		add(kMoneyBurst);
	for (const CustomRule& r : g_customRules)
		if (!r.target.empty())
			add(r.target.c_str());
	const VariantRule* tables[] = { kVariantWeaponFx, kVariantTracers, kVariantBlood,
		kVariantExplosions, kVariantBomb, kVariantImpacts, kVariantMolotov };
	const size_t counts[] = {
		sizeof(kVariantWeaponFx) / sizeof(VariantRule), sizeof(kVariantTracers) / sizeof(VariantRule),
		sizeof(kVariantBlood) / sizeof(VariantRule), sizeof(kVariantExplosions) / sizeof(VariantRule),
		sizeof(kVariantBomb) / sizeof(VariantRule),
		sizeof(kVariantImpacts) / sizeof(VariantRule), sizeof(kVariantMolotov) / sizeof(VariantRule)
	};
	const FxCategory cats[] = { kFxWeaponFx, kFxTracers, kFxBlood, kFxExplosions, kFxBombFx, kFxImpacts, kFxMolotov };
	for (int t = 0; t < 7; ++t) {
		const FxMode mode = g_modes[cats[t]];
		if (mode == FxMode::Off)
			continue;
		for (size_t i = 0; i < counts[t]; ++i)
			if (const char* target = SelectVariantTarget(cats[t], mode, tables[t][i]))
				add(target);
	}
	// Spray wrappers must be resolvable before the first hot shot needs them.
	if (g_modes[kFxWeaponFx] != FxMode::Off) {
		size_t sprayCount = 0;
		const SprayPair* pairs = SprayPairs(sprayCount);
		for (size_t i = 0; i < sprayCount; ++i)
			add(pairs[i].spray);
	}
	// Pre-resolve every TracerFallbackTarget bucket too (not just the exact-table
	// targets above), so an untabled tracer name's first-ever creation doesn't have to
	// fail open once while the async resolver catches up.
	if (const FxMode tracerMode = g_modes[kFxTracers]; tracerMode == FxMode::On || tracerMode == FxMode::Modern) {
		add(TracerFallbackTarget(tracerMode, ""));
		for (const TracerFallback& f : kTracerFallbacks)
			add(TracerFallbackTarget(tracerMode, f.substr));
	}
}

void QueueActiveSwapTargets() {
	std::lock_guard<std::mutex> lock(g_mx);
	QueueActiveSwapTargetsLocked();
}

} // namespace fx
} // namespace Filmmaker
