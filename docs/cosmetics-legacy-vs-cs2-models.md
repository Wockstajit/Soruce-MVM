# Legacy vs CS2 weapon model variants — how CS2 picks a model per skin

Status: investigation + implemented support (the `mirv_filmmaker cosmetics mesh ...` knob; the
auto econ-schema legacy read `PaintKitLegacyModel` now lives in `CosmeticFnResolver.cpp`, the
mesh-mask apply in `CosmeticModelSwap.cpp` — see `docs/cosmetics-overview.md` for the file map).
Live-verified that the apply path executes
and resolves on the current build; the *visual* mesh switch is subject to the demo-playback caveats in
§6. Companion to `docs/cosmetics-cs2-methodology-notes.md` (the source-derived recipes) and
`docs/archive/cosmetics-recompose-research.md` (the skin composite path).

## 1. The mechanism — one model, two (or more) mesh groups

A CS2 weapon `.vmdl` does **not** ship as separate "legacy" and "modern" files. Each weapon model
contains multiple **mesh groups**, and which group renders is chosen by a per-entity **mesh-group
mask** on the scene node (`CGameSceneNode::SetMeshGroupMask(uint64 mask)`). For weapons that were
re-modelled between CS:GO and CS2 (new high-poly geometry + a new UV layout), the model keeps the
**legacy** mesh (the old geometry/UVs) as a separate group so that skins authored against the old UV
map still line up. So "legacy AK vs CS2 AK" is the *same* loaded `.vmdl` rendered with a different mesh
group selected — not a different model resource.

This is why a paint kit (a skin) is the thing that decides the variant, not the weapon: the **paint
kit definition carries a boolean `IsUseLegacyModel`**. A skin made for the old UVs sets it; a skin
made for the new CS2 UVs does not. At weapon-build time CS2 reads that flag and sets the weapon's mesh
group mask accordingly, so the chosen finish renders on the matching geometry.

Confirmed in source (both reference cheats, `docs/cosmetics-cs2-methodology-notes.md` §5):
- `CPaintKit::IsUseLegacyModel()` reads a `uint8` at **paint kit + 0xAE** (Andromeda
  `g_CCPaintKit_IsUseLegacyModel`; nerv `c_paint_kit::uses_old_model`).
- The paint kit object comes from the econ item schema:
  `GetEconItemSystem() -> +0x8 (schema) -> +0x2F0 (GetPaintKits CUtlMap<int,CPaintKit*>) -> find by
  paint-kit id`.
- The mesh-group mask is then `SetMeshGroupMask(node, mask)`.

## 2. Which weapons have legacy + CS2 variants

**Per-paint-kit, not a fixed per-weapon list.** The correct, build-proof way to know whether a given
*finish* wants the legacy mesh is to read its paint kit's `IsUseLegacyModel` flag (which is exactly
what this repo now does — see §4). Hard-coding a weapon list is fragile because Valve adds finishes and
occasionally re-models more weapons.

That said, the weapons that carry a distinct **legacy mesh group** are the ones re-modelled in the
CS:GO "weapon model overhaul" and again for CS2 — predominantly the older rifles/pistols/SMGs that had
a large back-catalogue of skins authored on the old UVs (e.g. AK-47, M4A4, M4A1-S, AWP, USP-S, Glock,
Desert Eagle and similar). Newer weapons (and weapons that were never re-UV'd) ship a single mesh
group, so the legacy flag is irrelevant for them and toggling the mask is a no-op.

Live data point (this build): the **USP-S (def 61)** is one of the weapons with both groups present —
the apply path set the mask without faulting on it (`weaponMesh` apply counter incremented). See §6 for
the important verification caveat about *seeing* the switch.

## 3. How a skin determines the variant (the rule)

```
legacyMesh = paintKit.IsUseLegacyModel        // read from the econ schema, per finish
meshGroupMask = legacyMesh ? <legacy bits> : <modern bits>
SetMeshGroupMask(weaponSceneNode, meshGroupMask)
```

- A finish with the flag SET must render on the legacy mesh, or its pattern/wear will be UV-misaligned
  (smeared, offset, or mapped to the wrong faces).
- A finish with the flag CLEAR renders on the modern CS2 mesh.
- The **default/vanilla finish (paint 0)** uses the modern mesh.

### The mask bit values (and a real source disagreement)

The two reference implementations DISAGREE on the bit polarity, which is the single biggest edge case:

| Case | Andromeda | nerv (weapon skins) | nerv (knife) |
|---|---|---|---|
| modern (CS2) | `1` | `1` | `2` |
| legacy | `2` | `2` | `1` |

So for **weapons** both agree: modern=1, legacy=2. For **knives** nerv inverts it (modern=2,
legacy=1). This repo follows the Andromeda polarity for both (modern=`m_maskModern` default 1, legacy=
`m_maskLegacy` default 2) and exposes the values as tunables so the knife/edge cases can be A/B'd
in-game without a rebuild:

```
mirv_filmmaker cosmetics mesh auto|modern|legacy        # selection mode
mirv_filmmaker cosmetics mesh masks <modernBits> <legacyBits>   # tune the bit values
```

## 4. How the player customizer chooses the model (implementation)

The model-swap subsystem (`PaintKitLegacyModel` in `CosmeticFnResolver.cpp`; the rest in
`CosmeticModelSwap.cpp`):
- `PaintKitLegacyModel(paintKitId)` does the econ-schema lookup above (SEH-guarded) and returns
  `1`=legacy / `0`=modern / `-1`=unknown.
- `ResolveMeshMask(paintKit, knife, mode, maskModern, maskLegacy)` maps that to a mask:
  - `mode = auto` → use the schema flag; **unknown → return 0 = leave the mesh untouched** (never
    guess, so an unrecognised finish keeps whatever mesh the demo already set).
  - `mode = modern|legacy` → force it (for A/B).
- In the apply loop (`CosmeticOverrideSystem::ApplyMatchedWeapons`):
  - **Weapon skin override**: if `paintKit > 0`, compute the mask and `ApplyWeaponMeshMask` — i.e. the
    customizer auto-corrects the legacy/CS2 mesh to match the chosen finish. Unknown legacy → untouched.
  - **Knife type swap**: a knife always needs a mesh group, so an unknown/auto result falls back to
    `maskModern` rather than 0.
- This runs change-gated, right before the skin re-composite, so the composite rebuilds onto the
  selected mesh.

The schema read resolved on the live build (`cosmetics status` → `modelSwapFns: ... econSchema=1`).

## 5. Edge cases

- **Knife mask polarity** (Andromeda vs nerv disagree) — tune with `cosmetics mesh masks` if a swapped
  knife renders the wrong geometry.
- **Unknown paint kit** (not in the schema map, or schema unavailable) — auto mode leaves the mesh
  untouched; only a forced `mesh legacy|modern` will move it.
- **Weapons with a single mesh group** — toggling the mask is a no-op; expected, not a bug.
- **Default finish (paint 0)** — modern mesh; no legacy correction needed.
- **A legacy skin left on the modern mesh (or vice-versa)** — the finish renders but the pattern is
  UV-misaligned. This is the user-visible symptom the auto-selection exists to prevent.
- **Mesh change not visible while the demo is PAUSED** — see §6; the mask write executes but the
  rendered mesh group is not re-evaluated until the engine refreshes the renderable.

## 6. How automation should verify (and a trap to avoid)

**The trap:** do NOT verify the legacy/CS2 switch by comparing two DIFFERENT paint kits (e.g. paint 38
vs paint 44) — the diff you measure is the *paint change*, not the mesh switch, and you get a
false-positive "it works". (This bit this very investigation: an apparent 3.03 mean diff was a paint
change; the controlled same-paint test below showed ~0.16.)

**The correct test:** hold the paint kit, wear, seed, weapon, tick, and camera CONSTANT, and toggle
ONLY the mesh mode:
```
cosmetics player current weapon <def> paint <pk> wear <w> seed 0
cosmetics mesh modern   ; <recapture>    # screenshot A
cosmetics mesh legacy   ; <recapture>    # screenshot B
image_diff A B --crop <weapon-region>
```
A real legacy↔modern switch on a weapon that has both groups changes the silhouette/UVs of the weapon
crop well above the same-tick TAA noise floor. `automation/verify/verify-cosmetics-customizer.ps1`
contains this controlled same-paint toggle.

**Important verification caveat (this build, demo playback):** the controlled same-paint toggle did
**not** produce a visible weapon-crop change while the demo was **paused** (~0.16 mean, noise-floor).
The mask write executes (the `weaponMesh` apply counter increments, no crash, the function resolves),
but the rendered mesh group was not observed to switch on a paused spectated demo weapon. The most
likely reasons (consistent with the skin-composite findings in `docs/archive/cosmetics-recompose-research.md`):
the engine re-derives the renderable mesh from the networked snapshot, and/or the switch needs a
renderable refresh (`PostDataUpdate` on the scene node) or live (un-paused) sim frames to take effect.

**RESOLVED (2026-06-29):** both of those are now done. `CGameSceneNode::PostDataUpdate` (vtable
index 22, from Andromeda) is wired after `SetMeshGroupMask` (and every model swap) in
`CosmeticModelSwap.cpp`, and `MaybeFireTickNudge` (`CosmeticDemoSync.cpp`) auto-plays ~10 ticks after a
change (briefly resume + re-pause) so the mesh group is re-evaluated on live frames without a manual
scrub. See `docs/cosmetics-cs2-methodology-notes.md` §6b. The mesh switch is geometry/UV-subtle, so
verify it on a weapon that has both variants in third person during the play-out, not by a paused
same-tick crop diff.

## Sources
- `docs/cosmetics-cs2-methodology-notes.md` (econ-schema + mesh-mask recipes, source disagreement table)
- Andromeda `CEconItemSchema.hpp` (`GetPaintKits` @ 0x2F0, `CPaintKit::IsUseLegacyModel` @ 0xAE),
  `CInventoryChanger.cpp` (`meshGroupMask = 1 + isLegacy`)
- nerv `i_econ_item_system.hpp` (`c_paint_kit::uses_old_model` @ 0xAE), `skin_changer.cpp`
  (knife mesh polarity inverted vs weapons)
- Local: `AfxHookSource2/Filmmaker/Cosmetics/CosmeticModelSwap.cpp`
  (`ResolveMeshMask`/`ApplyWeaponMeshMask`) + `CosmeticFnResolver.cpp` (`PaintKitLegacyModel`),
  `automation/verify/verify-cosmetics-customizer.ps1`
