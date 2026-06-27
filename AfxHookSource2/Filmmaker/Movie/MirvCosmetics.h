#pragma once

// Offline demo-only cosmetic overrides for the spectated player (movie-making tool).
//
// Phase 3 implements WEAPON SKINS: once per main-thread frame, each overridden player's ACTIVE
// weapon is rewritten to composite from fallback paint-kit/wear/seed/stattrak fields (the same
// mechanism Osiris/nSkinz use for skin changers). Setting C_EconItemView::m_iItemIDHigh to -1
// makes the client ignore the networked item and use the C_EconEntity::m_nFallback* fields instead.
//
// This only ever runs against local demo-playback entities driven by the filmmaker tool; it is
// never used for live/online play. Overrides are keyed by the player PAWN entity index (the value
// the cam editor already has as obsTarget). All writes are gated on g_cosmeticsOffsetsOk so a
// renamed schema field disables skins rather than corrupting memory.

namespace Filmmaker {

// paintKit 0 means "no skin" -- callers should Cosmetics_ClearPlayer instead to restore the
// original networked item.
void Cosmetics_SetSkin(int playerPawnEntityIndex, int paintKit, float wear, int seed, int statTrak);
void Cosmetics_SetWeapon(int playerPawnEntityIndex, int weaponDefIndex, int paintKit, float wear, int seed, int statTrak);
void Cosmetics_SetKnife(int playerPawnEntityIndex, int knifeDefIndex, int paintKit, float wear, int seed);
void Cosmetics_SetGloves(int playerPawnEntityIndex, int gloveDefIndex, int paintKit, float wear, int seed);
void Cosmetics_SetAgent(int playerPawnEntityIndex, int agentDefIndex);
void Cosmetics_ClearPlayer(int playerPawnEntityIndex);
void Cosmetics_ClearAll();

// Applies all active overrides. Cheap no-op when there are no overrides or the econ offsets did
// not resolve. Call once per main-thread frame.
void Cosmetics_RunFrame();

// False if the econ schema offsets failed to resolve -- overrides will be ignored. Used to warn
// the user when they try to apply a skin.
bool Cosmetics_Available();

// Whether to force a material re-composite (weapon vtable UpdateComposite/UpdateCompositeSec)
// after writing the fallback fields. ON by default -- without it the skin is written but never
// renders. Exposed as a kill-switch in case the vtable indices are wrong on some build.
void Cosmetics_SetRecompose(bool enabled);
bool Cosmetics_GetRecompose();

// True if a recompose vtable call raised an SEH exception (bad index) -- recompose auto-disables.
bool Cosmetics_GetFaulted();
// Runtime override of the UpdateComposite / UpdateCompositeSec vtable indices (-1 = skip a call),
// so the right index can be dialed in live without rebuilding. Setting it re-arms recompose.
void Cosmetics_GetVtIdx(int* comp, int* sec);
void Cosmetics_SetVtIdx(int comp, int sec);

// Diagnostic: resolves the spectated player's active weapon and reports its current econ state
// (so we can confirm targeting + that writes land). Returns false if no weapon resolved.
bool Cosmetics_DebugWeapon(int playerPawnEntityIndex, int* outWeaponIndex, int* outItemIdHigh,
	int* outPaintKit, int* outDefIndex, float* outWear);

}
