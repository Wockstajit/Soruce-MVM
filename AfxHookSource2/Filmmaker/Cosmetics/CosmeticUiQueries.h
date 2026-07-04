#pragma once

#include <string>

namespace Filmmaker {

// Per-player state for the Camera Editor's CUSTOMIZE (LOADOUT) modal: live weapon/glove/agent
// econ reads for the spectated pawn, surfaced to Panorama as JSON. Split out of CameraEditorHud
// (which stays focused on Panorama build/teardown + per-frame state push) since these are pure
// entity/econ reads with no HUD/bridge involvement.

// pawnIndex -> full loadout JSON (weapons/gloves/agent model + active-weapon pickup info) for the
// CUSTOMIZE modal. Returns the literal string "null" if pawnIndex isn't a valid player pawn.
std::string BuildCustomizeTargetJson(int pawnIndex);

// { "<pawnIndex>": <BuildCustomizeTargetJson>, ... } for every currently valid player pawn.
std::string BuildCustomizePlayersJson();

// Zeroes every pawn's flashbang whiteout fields for this frame. Called per main-thread frame by
// CameraEditorHud while the Customize modal is open so the flash overlay never covers the modal.
void CustomizeSuppressFlashTick();

} // namespace Filmmaker
