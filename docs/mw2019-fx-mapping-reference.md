# MW2019 (GMod ARC9) FX Mapping Reference

Source-of-truth notes on how the GMod addon **ARC9 Modern Warfare 2019** maps muzzle
flashes, barrel smoke, tracers, and explosions onto its weapons, extracted by reading
the addon's own Lua. Written to hand to an LLM (or a human) porting these effects onto
CS2 native weapons in `AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp` (the variant tables). See also
[memory: modern-pack-mw2019-mapping] and `docs/filmmaker_effects_modifiers.md`
(FxMode::Modern) for how this feeds the actual DLL implementation.

**Sources read** (GMod workshop content, already extracted next to their `.gma` via
gmpublisher):

| Layer                    | Workshop ID  | Folder                                                                                                                                          |
| ------------------------ | ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| ARC9 Weapon Base         | `2910505837` | `G:\SteamLibrary\steamapps\workshop\content\4000\2910505837\ARC9 Weapon Base`                                                                   |
| ARC9 Modern Warfare 2019 | `3258297368` | `G:\SteamLibrary\steamapps\workshop\content\4000\3258297368\ARC9 Modern Warfare 2019`                                                           |
| ARC9 MW2019 Shared Pack  | `3258299652` | `G:\SteamLibrary\steamapps\workshop\content\4000\3258299652\ARC9 Modern Warfare 2019 Shared Pack` (models/sounds/materials only — no particles) |

## 1. Two-layer addon architecture

MW2019 doesn't reinvent the firing/FX pipeline — it's a content pack laid on top of the
generic **ARC9 Weapon Base**, which owns the class-based fallback particle set and the
actual code that fires effects.

| Layer                               | What it owns                                                                                                                                                                                              |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **ARC9 Weapon Base** (`2910505837`) | Generic firing pipeline + class-based fallback particles (`particles/arc9_fas_muzzleflashes.pcf`, `arc9_fas_explosions.pcf`). Defines every field a weapon can set.                                       |
| **ARC9 MW2019** (`3258297368`)      | Adds `mw2019_effects.pcf`, `mw2019_tracer.pcf`, `mw2019_rockettrail.pcf`, `mw2019_explosions_pak.pcf`, and one Lua file per weapon that overrides the base pack's fallback values with COD-specific ones. |

MW2019 weapons set a handful of string fields (`MuzzleParticle`, `AfterShotParticle`,
`TracerEffect`) on top of the base pack's generic effect system — they don't define new
effect code.

## 2. The firing pipeline

```
SWEP:PrimaryAttack()
  └─ SWEP:DoEffects()                     [ARC9 Weapon Base: weapons/arc9_base/sh_effects.lua]
       reads self:GetProcessedValue("MuzzleParticle")
       └─ util.Effect("arc9_muzzleeffect", data)
            └─ CreateParticleSystem(muzzleDevice, MuzzleParticle, PATTACH_POINT_FOLLOW, att)
                 [effects/arc9_muzzleeffect.lua — attaches to the barrel/suppressor bone]

  └─ SWEP:DoEject()  → util.Effect(ShellEffect or "ARC9_shelleffect")
       [effects/arc9_shelleffect.lua — physical shell prop + "port_smoke"/"shellsmoke" puff]

  └─ util.Effect("arc9_aftershoteffect")  [effects/arc9_aftershoteffect.lua]
       reads self:GetProcessedValue("AfterShotParticle")   ← this is the barrel smoke
       └─ CreateParticleSystem(muzzleDevice, AfterShotParticle, PATTACH_POINT_FOLLOW, att)

  └─ util.Effect(self.TracerEffect)        e.g. "cod2019_tracer_fast"
       [effects/cod2019_tracer_fast/init.lua]
       └─ CreateParticleSystem(weapon, "mw2019_tracer_fast", PATTACH_ABSORIGIN, att)
            :SetControlPoint(0, muzzlePos)   -- CP0 = start
            :SetControlPoint(1, hitPos)      -- CP1 = end
```

**Key fact for the port:** in GMod the tracer is a genuine two-control-point beam
particle (CP0 = start, CP1 = end).

> ⚠️ **Tracer port (2026-07-04):** GMod tracers are CP0→CP1 beams; the converted MW2019
> parents shipped with broken emitters (random −1..1 count on `mw2019_tracer`) and extreme
> MoveBetweenPoints speeds. GMod's own parent stack is `Lifetime from Time to Impact` +
> `Lifespan Decay` (no `FadeAndKillForTracers` — that operator is Povarehok-only in this
> repo). `postprocess_modern.patch_modern_tracer_cs2_discipline` fixes CS2 motion (13k u/s
> MoveBetweenPoints, exactly one particle per shot, zero spawn offset) while restoring that
> GMod lifecycle on the four routed MW2019 parents (`mw2019_tracer{,_fast,_slow,_small}`).
> Follow-on passes (same file): automatic-weapon parent `mw2019_tracer` drops sniper-only
> children (`mgbase_tracer_glow_large`, `weapon_tracers_4incendiary`); glow/trail leaves
> halve alpha/radius/self-illum; parent `RenderTrails` keep `m_flLengthFadeInTime ≈ 0.12`
> plus head/tail alpha taper; AR parent uses `tracer_middle` (not spark); the visible rope
> child (`mgbase_tracer_trail*`) keeps converted GMod `fas_smoke_beam` with V-scroll so
> smoke wisps drift along the CP0→CP1 beam (GMod-authentic; no static stretched tile).

> **Modern barrel smoke (2026-07-05):** CS2 Modern now matches GMod's per-shot
> `AfterShotParticle` — `barrel_smoke(_plume)` is a direct PCF child of each class flash
> (world + `_fp`), not a spray-gated wrapper. Rope wisps (`barrel_smoke_trail*`) stay
> `C_OP_RenderRopes` in world space for third-person visibility. Eject-port puff
> (`port_smoke` / `shellsmoke`) is attached as a child of each `weapon_shell_casing_*`
> swap target (CS2 has no separate eject-port create).

`GetProcessedValue()` is ARC9's attachment-aware property resolver — it lets an
attachment (e.g. a suppressor) override a weapon's base field at runtime. That
resolution plumbing lives in the base pack's stats file
(`weapons/arc9_base/sh_0_stats.lua`); the actual MW2019 override _values_ live in
`arc9/common/attachments_bulk/mw19_ammo_types.lua`.

## 3. Class → effect defaults (the 90% case)

Set as `SWEP.MuzzleParticle` / `SWEP.AfterShotParticle` / `SWEP.TracerEffect` in each
weapon file (falls back to `arc9_cod2019_base.lua`'s `TracerEffect = "cod2019_tracer"`
if a weapon doesn't override it).

| Weapon class    | Example guns                                                                    | MuzzleParticle              | AfterShotParticle (barrel smoke)                              | TracerEffect                             | Extra                                                            |
| --------------- | ------------------------------------------------------------------------------- | --------------------------- | ------------------------------------------------------------- | ---------------------------------------- | ---------------------------------------------------------------- |
| Assault Rifle   | AK-47, M4, FAL, SCAR, Kilo141, RAM-7, Oden, CR-56, AN-94, FAMAS, Grau 5.56, M13 | `muzzleflash_ar`            | `barrel_smoke`                                                | _(default)_ `cod2019_tracer`             | AS-VAL is the odd AR: `muzzleflash_suppressed`                   |
| SMG             | MP5, MP7, P90, Vector, Uzi, Bizon, CX-9, ISO, Striker 45, AUG                   | `muzzleflash_smg`           | `barrel_smoke`                                                | _(default)_                              | —                                                                |
| LMG             | PKM, MG34, Holger-26, Bruen Mk9, M91, RAAL, SA87, FiNN                          | `muzzleflash_lmg`           | `barrel_smoke`                                                | _(default)_                              | Minigun: `barrel_smoke_plume`                                    |
| Marksman/DMR    | Kar98k, M14, Mk2, SKS, SP-R 208                                                 | `muzzleflash_dmr`           | `barrel_smoke`                                                | `cod2019_tracer_fast`                    | Crossbow: `muzzleflash_suppressed`, no smoke/tracer              |
| **Sniper**      | AX-50, HDR, SVD, Rytec AMR                                                      | **`muzzleflash_smg`** ⚠️    | `barrel_smoke`                                                | `cod2019_tracer_fast`                    | `MakeEnvironmentDust(150)` fires on shot — ground dust puff (§6) |
| Shotgun         | Model 680, JAK-12, R9-0, Origin-12, VLK Rogue, .725                             | `muzzleflash_shotgun`       | `barrel_smoke`                                                | `cod2019_tracer_slow`                    | —                                                                |
| Pistol          | M19, M1911, Renetti, Sykov, X16                                                 | `muzzleflash_pistol`        | `barrel_smoke`                                                | `cod2019_tracer_small`                   | Akimbo variants revert to plain `cod2019_tracer`                 |
| Pistol (magnum) | .357, .50 GS                                                                    | `muzzleflash_pistol_deagle` | `barrel_smoke`                                                | `cod2019_tracer_small`                   | —                                                                |
| Launcher        | RPG-7, PILA, JOKR, Strela-P, M32                                                | `muzzleflash_m79`           | `barrel_smoke_plume` (`AfterShotParticleDelay = -1`, instant) | _(default)_, M32 = `cod2019_tracer_slow` | `MakeEnvironmentDust(200)` on fire                               |

⚠️ **Sniper quirk, verified not a bug**: every bolt-action/AMR sniper (AX-50, HDR, SVD,
Rytec) uses `muzzleflash_smg`, _not_ `muzzleflash_dmr`. This is consistent across all
four sniper weapon files — likely a copy-paste artifact in the original addon, but
treat it as intentional/authoritative when porting, not something to "correct."

## 4. Suppressor override

Weapons that are inherently suppressed hardcode
`SWEP.MuzzleParticle = "muzzleflash_suppressed"` directly in their weapon file
(AS-VAL, the bolt crossbow). There's also an ammo/attachment-level field
`ATT.MuzzleParticleSilenced = "AC_muzzle_shotgun_suppressed"` defined on shotgun slug
ammo types in `mw19_ammo_types.lua`, but the runtime code that _consumes_ that field at
fire time lives outside the two packs read here (in the base ARC9 attachment
framework), so there's no verified read-path for dynamic suppressor-attachment
swapping — only the hardcoded per-weapon case above is confirmed from source.

## 5. Tracer effect name → actual particle system

| `SWEP.TracerEffect` (Lua effect) | Real PCF particle it spawns                                                                                                              |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `cod2019_tracer` (default)       | `mw2019_tracer_3`                                                                                                                        |
| `cod2019_tracer_fast`            | `mw2019_tracer_fast`                                                                                                                     |
| `cod2019_tracer_slow`            | `mw2019_tracer_slow_new`                                                                                                                 |
| `cod2019_tracer_small`           | `mw2019_tracer_3` (⚠️ dead code — a commented-out line shows it _used to_ point at `mw2019_tracer_small`, but currently reuses tracer_3) |
| `cod2019_tracer_2`               | `mw2019_tracer_2`                                                                                                                        |
| `cod2019_tracer_inc`             | `mw2019_tracer_inc`                                                                                                                      |
| `cod2019_tracer_rainbow`         | `mw2019_tracer_rainbow`                                                                                                                  |

All spawned via `CreateParticleSystem(weapon, name, PATTACH_ABSORIGIN, attachment)`
then `SetControlPoint(0, start)` / `SetControlPoint(1, end)`.

## 6. Two non-obvious systems

- **HE/frag explosion**: `entities/arc9_cod2019_thrownfrag` →
  `effects/cod2019_grenade_explosion.lua` → precaches `particles/fas_explosions.pcf`
  and plays **`explosion_grenade`**. This is the _base pack's_ explosion system, not
  `mw2019_explosions_pak.pcf` — MW2019 grenades reuse the FAS pack's detonation
  particle rather than bringing their own.
- **Sniper/launcher floor dust**: `MakeEnvironmentDust()` in `arc9_cod2019_base.lua` is
  **not a PCF at all** — it's the native GMod/Source engine effect
  `util.Effect("ThumperDust")` (falls back to `"waterripple"` if the shooter is in
  water), fired at the shooter's feet only when `IsOnGround()`. CS2 has no equivalent
  native effect, so this needs its own authored particle on the CS2 side (mapped to
  `uweapon_muzflsh_ground_smoke` per [memory: modern-pack-mw2019-mapping]).

## 6b. The lingering "smoke follows the gun through the air" trail

This is the effect where, after firing for a while and then stopping, a ribbon of smoke
hangs in world space and visibly trails behind wherever the barrel has been if you swing
your view/weapon around — distinct from the single per-shot muzzle puff described in §3.

- **Asset**: `arc9_fas_muzzleflashes.pcf` (the same base-pack PCF as `MuzzleParticle`/
  `AfterShotParticle`) bundles two extra systems, **`barrel_smoke_trail`** and
  **`barrel_smoke_trail_b`**, that are NOT in the `PrecacheParticleSystem(...)` list in
  `arc9/shared/sh_effects.lua` and have **zero** references anywhere in either Lua tree
  (confirmed by grep across both workshop addons). They are real, present assets in the
  PCF — just never called by name from Lua.
- **Why they fire anyway**: Source 1 `.pcf` particle systems can nest **child** systems
  directly in the particle-editor definition, spawned automatically whenever their
  parent spawns, with no scripting involved. `barrel_smoke_trail(_b)` are almost
  certainly authored as children of the `AfterShotParticle` systems (`barrel_smoke` /
  `barrel_smoke_heavy`) — i.e. every time `arc9_aftershoteffect.lua` fires the per-shot
  puff (§2), it silently drags this trail system along with it. That also explains why a
  single well-timed shot barely shows it, but continuous fire — which keeps re-triggering
  the parent every shot — makes it obvious.
- **Why it looks like it "follows the gun in the air"**: the system's renderer class is
  `C_OP_RenderRopes` (a literal rope/ribbon renderer, not sprites) with roughly a **5
  second** particle lifetime. A rope renderer draws a continuous ribbon connecting the
  emitter's recent path samples — since the emitter is attached `PATTACH_POINT_FOLLOW` to
  the moving barrel, the ribbon quite literally traces the gun's recent movement through
  the air for the ~5s the segment stays alive, which is exactly the visual being
  described (smoke hanging in space, "following" the barrel when you look around).
- **Known CS2-port gotcha**: converting `C_OP_RenderRopes` as-is in Source 2 stretches a
  single ribbon across the muzzle's _entire_ sweep between puffs rather than reading as
  drifting smoke — it needs to become a sprite emitter (with frame blending) and a
  shorter lifetime (~2–3s) to look right, not a literal 1:1 rope port.

## 6c. First-person vs world muzzle-flash split (`_fp` twins)

Bug (user report 2026-07-04): in first person the Modern muzzle flash floats off to the
side of the gun, while in third person it sits correctly on the weapon; the barrel smoke
lines up in both. Root cause: the pack ships **one** flash asset per weapon, and
`kVariantWeaponFx` routed BOTH the first-person (`*_fps`) and the world CS2 muzzle systems
to it. A world-space flash (`m_bViewModelEffect = false`) anchors on the weapon muzzle in
third person / free cam, but the first-person viewmodel is drawn in a separate pass with
viewmodel FOV — a world-space particle placed at the viewmodel muzzle does not line up.
(Barrel smoke aligned because `patch_cs2_modern_barrel_smoke_alignment` already gives it its
own `m_bViewModelEffect = true` viewmodel attach.)

CS2 and Povarehok solve this by shipping a **separate `_fp` flash** with
`m_bViewModelEffect = true` and routing the `_fps` systems to it (see `weapon_muzzle_flash_*_fp`
in `kVariantWeaponFx` / `kSprayPairs`). Modern now does the same:

- `postprocess_modern.make_modern_muzzleflash_fp` writes a `<flash>_fp.vpcf` viewmodel-effect
  twin of every world flash in `MODERN_MUZZLEFLASH_FILES` (identical apart from the flag),
  plus `_fp` variants of the two sniper compositions (`MVM_COMPOSITIONS_FP`, whose flash
  child is the `_fp` leaf; the shock-dust / plume / heat children stay world-space).
- The `*_fps` rows in `kVariantWeaponFx` point at the `_fp` target; the world rows keep the
  world target.
- Detached-camera handling is unchanged: `g_fpFxSuppress` + `IsFirstPersonWeaponFxPath`
  still key on the **source** `_fps`/`_fp` system name (not the swap target), so a
  third-person / free-cam view still suppresses the viewmodel flash and shows the world one
  via the spec_mode-3 chase path. Spray upgrades key on the resolved **target**, and `_fp`
  targets are not `kSprayPairs` bases, so a first-person flash is never re-upgraded to a
  world-space spray wrapper.

## 6d. Flash sheet + thrown-grenade trail port

Modern central muzzle-flash sprite leaves that reference `hl2_muzzleflash` are retargeted
to the pack's higher-resolution `fas_muzzleflash_test_b` sheet and receive
`C_OP_PositionLock` so the center burst remains attached to the muzzle. Authored animation
rates, alpha behavior, and frame-blend settings are left intact. The `_fp` twins described
above inherit the texture and lock, differing from their world flashes only in the required
viewmodel flag.

Thrown-grenade smoke trails are a synthesized CS2-side composition, not a converted
`mw2019_effects.pcf` system. The runtime swaps CS2's demo-only
`particles/entity/spectator_utility_trail.vpcf` to `mvm_grenade_trail` in Modern mode only.
Its two children retain the earlier `materials/effects/fas_dust_a.vtex` and
`fas_dust_b.vtex` recipes, including their original alpha, radius, lifetime, fade, and
growth values. A `PF_TYPE_CONTROL_POINT_SPEED` emit-rate gate makes the trail stop emitting
when the grenade lands. The actual deployed smoke grenade cloud remains CS2's native
volumetric system and is not part of this particle swap.

## 6e. Runtime resolve/precache pacing

The Modern pack has enough top-level swap targets that cold resolution can hitch if demo
entry forces the whole table through synchronous particle-resource loads. The active target
set is therefore separate from the resolve queue: mode rebuilds make Modern targets
eligible, but they do not enqueue the entire pack. The create hook queues a target only
after first real use, that creation fails open, and `ParticleFx.cpp` warms the demand-only
queue one target at a time. Active playback uses a slow wall-clock-spaced trickle with
heavier backoff after cold loads, paused demos warm faster, and the main menu does not
resolve queued FX targets. This changes only **when** target handles are resolved; the
variant tables, cache lifetime, and fail-open behavior are unchanged. Level/demo changes
still clear cached handles because CS2 purges particle resources across map transitions.

## 7. Particle source files (what to extract/convert)

| File                                                                                                                                                  | Contents                                                                                                                                           |
| ----------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `arc9_fas_muzzleflashes.pcf`                                                                                                                          | All `muzzleflash_*` class systems, `barrel_smoke*` (incl. the un-precached rope trail `barrel_smoke_trail`/`_b`, §6b), `shellsmoke`, `port_smoke*` |
| `arc9_fas_explosions.pcf` (loaded as `fas_explosions.pcf` in the grenade effect — the two references don't match; verify the actual filename on disk) | `explosion_grenade` and friends                                                                                                                    |
| `mw2019_tracer.pcf`                                                                                                                                   | `mw2019_tracer`, `_2`, `_3`, `_fast`, `_slow_new`, `_small`, `_inc`, `_rainbow`                                                                    |
| `mw2019_effects.pcf`                                                                                                                                  | Misc COD-specific effects (grenade trails, molotov, etc.)                                                                                          |
| `mw2019_rockettrail.pcf`                                                                                                                              | Launcher rocket trails                                                                                                                             |
| `mw2019_explosions_pak.pcf`                                                                                                                           | Non-grenade explosions (rockets, C4, etc. — grenades don't use this one)                                                                           |
