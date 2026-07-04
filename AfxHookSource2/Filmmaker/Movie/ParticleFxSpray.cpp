// ParticleFx spray-gated barrel smoke.
// User call 2026-07-03: lingering barrel smoke should only appear during SUSTAINED
// fire, not after a single shot. The postprocess writes per-flash `mvm_spray_*`
// wrapper systems (flash + barrel smoke); the hook counts creations of each vanilla
// muzzle-flash NAME on the demo-tick clock and upgrades the swap target to the
// wrapper from the kSprayHotCount-th shot of a spray onward. Single-shot snipers
// keep per-shot smoke inside their own compositions instead (a spray gate can never
// trigger on a bolt gun).

#include "ParticleFxInternal.h"

#include "../Cosmetics/CosmeticDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)

#include <cstring>

namespace Filmmaker {
namespace fx {

namespace {

#define MODSPRAY(name) { "particles/filmmaker/modern/arc9_fas_muzzleflashes/" name ".vpcf", \
	"particles/filmmaker/modern/arc9_fas_muzzleflashes/mvm_spray_" name ".vpcf" }
// Less-mode weapon targets live under less/smoke, but the mod's weapon files are
// byte-identical across variants, so both point at the regular wrappers.
#define BPSPRAY(variant, name) { \
	"particles/filmmaker/povarehok/" variant "/weapons/cs_weapon_fx/" name ".vpcf", \
	"particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx/mvm_spray_" name ".vpcf" }
const SprayPair kSprayPairs[] = {
	MODSPRAY("muzzleflash_ar"), MODSPRAY("muzzleflash_smg"), MODSPRAY("muzzleflash_shotgun"),
	MODSPRAY("muzzleflash_pistol"), MODSPRAY("muzzleflash_pistol_deagle"),
	MODSPRAY("muzzleflash_lmg"), MODSPRAY("muzzleflash_dmr"),
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
};
#undef BPSPRAY
#undef MODSPRAY

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
