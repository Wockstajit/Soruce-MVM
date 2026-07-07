// ParticleFx spray-gated barrel smoke (both packs since 2026-07-06).
// Every class flash's smoke rides an `mvm_spray_*` wrapper (flash + barrel smoke)
// instead of a per-shot PCF child: children spawn on EVERY shot, which stacked
// overlapping smoke instances during sustained fire and double-smoked the sniper
// compositions (the SCAR-20 report). The hook upgrades a flash swap to its wrapper
// from the first shot but at most once per kSprayUpgradeCooldownTicks per name, so a
// spraying weapon keeps ONE continuous wisp going.

#include "ParticleFxInternal.h"

#include "../Cosmetics/CosmeticDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)

#include <cstring>

namespace Filmmaker {
namespace fx {

namespace {

// Each Povarehok variant owns its wrapper so Less can use its reduced smoke plume
// (70% for muzzle less/smoke; bullet impacts use the separate 50% less/impacts pack)
#define BPSPRAY(variant, name) { \
	"particles/filmmaker/povarehok/" variant "/weapons/cs_weapon_fx/" name ".vpcf", \
	"particles/filmmaker/povarehok/" variant "/weapons/cs_weapon_fx/mvm_spray_" name ".vpcf" }
const SprayPair kSprayPairs[] = {
	BPSPRAY("regular", "weapon_muzzle_flash_assaultrifle"),
	BPSPRAY("regular", "weapon_muzzle_flash_assaultrifle_fp"),
	BPSPRAY("regular", "weapon_muzzle_flash_smg"),
	BPSPRAY("regular", "weapon_muzzle_flash_smg_fp"),
	BPSPRAY("regular", "weapon_muzzle_flash_pistol"),
	BPSPRAY("regular", "weapon_muzzle_flash_pistol_fp"),
	BPSPRAY("regular", "weapon_muzzle_flash_shotgun"),
	BPSPRAY("regular", "weapon_muzzle_flash_shotgun_fp"),
	BPSPRAY("regular", "weapon_muzzle_flash_para"),
	BPSPRAY("regular", "weapon_muzzle_flash_para_fp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_assaultrifle"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_assaultrifle_fp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_smg"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_smg_fp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_pistol"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_pistol_fp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_shotgun"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_shotgun_fp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_para"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_para_fp"),
	BPSPRAY("regular", "weapon_muzzle_flash_smg_silenced"),
	BPSPRAY("regular", "weapon_muzzle_flash_smg_silenced_fp"),
	BPSPRAY("regular", "weapon_muzzle_flash_assaultrifle_silenced"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_smg_silenced"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_smg_silenced_fp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_assaultrifle_silenced"),
	// Snipers (AWP world + FP hunting rifle): previously a direct smoke_long child on
	// the flash, which stacked one 4s plume PER SHOT on the autosnipers (they map to
	// the AWP flash but fire fast) -- the "two smokes on the SCAR-20" report.
	BPSPRAY("regular", "weapon_muzzle_flash_awp"),
	BPSPRAY("regular", "weapon_muzzle_flash_huntingrifle_fp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_awp"),
	BPSPRAY("less/smoke", "weapon_muzzle_flash_huntingrifle_fp"),
// Modern MW2019 wrappers (mvm_spray_<flash>): the smoke half is the class barrel
// smoke the GMod AfterShotParticle mapping assigns (barrel_smoke, or _plume for
// lmg/dmr); the sniper rows wrap the whole mvm composition with barrel_smoke_plume
// (postprocess_modern writes the files; validate-povarehok-assets.py's MODSPRAY_RE
// seeds them into the pack closure -- keep the macro name in sync).
#define MODSPRAY(name) { \
	"particles/filmmaker/modern/arc9_fas_muzzleflashes/" name ".vpcf", \
	"particles/filmmaker/modern/arc9_fas_muzzleflashes/mvm_spray_" name ".vpcf" }
	MODSPRAY("muzzleflash_ar"),
	MODSPRAY("muzzleflash_ar_fp"),
	MODSPRAY("muzzleflash_smg"),
	MODSPRAY("muzzleflash_smg_fp"),
	MODSPRAY("muzzleflash_shotgun"),
	MODSPRAY("muzzleflash_shotgun_fp"),
	MODSPRAY("muzzleflash_pistol"),
	MODSPRAY("muzzleflash_pistol_fp"),
	MODSPRAY("muzzleflash_pistol_deagle"),
	MODSPRAY("muzzleflash_pistol_deagle_fp"),
	MODSPRAY("muzzleflash_lmg"),
	MODSPRAY("muzzleflash_lmg_fp"),
	MODSPRAY("muzzleflash_dmr"),
	MODSPRAY("muzzleflash_dmr_fp"),
	MODSPRAY("mvm_muzzleflash_sniper_awp"),
	MODSPRAY("mvm_muzzleflash_sniper_awp_fp"),
	MODSPRAY("mvm_muzzleflash_sniper_auto"),
	MODSPRAY("mvm_muzzleflash_sniper_auto_fp"),
#undef MODSPRAY
};
#undef BPSPRAY

} // namespace

// "fx align gate off": the alignment probe needs wisp samples for EVERY class, so the
// heat gate can be bypassed (every shot upgrades). Not persisted -- see ParticleFx.h.
std::atomic<bool> g_sprayGateBypass{ false };
std::map<std::string, SprayHeat> g_sprayHeat; // guarded by g_mx

const SprayPair* SprayPairs(size_t& count) {
	count = sizeof(kSprayPairs) / sizeof(kSprayPairs[0]);
	return kSprayPairs;
}

const char* SprayUpgradeFor(const char* target) {
	for (const SprayPair& p : kSprayPairs)
		if (0 == std::strcmp(target, p.base))
			return p.spray;
	return nullptr;
}

// g_mx held. Counts this creation of `low` and returns true once `hotCount` consecutive
// shots within kSprayWindowTicks have fired. Same-tick repeats do not accumulate.
bool SprayHotLocked(const char* low, int hotCount) {
	const int tick = g_demoTickNow.load(std::memory_order_relaxed);
	if (tick < 0)
		return false;
	SprayHeat& h = g_sprayHeat[low];
	if (tick < h.lastTick || tick - h.lastTick > kSprayWindowTicks) {
		h.count = 1;
		h.lastTick = tick;
	} else if (tick > h.lastTick) {
		++h.count;
		h.lastTick = tick;
	}
	return h.count >= hotCount;
}

} // namespace fx

// ============================== public accessors ===================================

using namespace fx;

void ParticleFx_SetSprayGateBypass(bool bypass) {
	g_sprayGateBypass.store(bypass, std::memory_order_relaxed);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.align", "spray gate %s", bypass ? "BYPASSED (every shot wisps)" : "restored");
}

bool ParticleFx_SprayGateBypass() {
	return g_sprayGateBypass.load(std::memory_order_relaxed);
}

} // namespace Filmmaker
