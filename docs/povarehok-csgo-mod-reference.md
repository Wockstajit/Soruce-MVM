# Povarehok (CS:GO FX mod) Reference

Everything known about the CS:GO particle/material mod at `reference/csgo effect mod/`
— both **what the mod itself is** (history, features, how CS:GO loads it) and **why it's
hard to port to CS2** (the exact texture-failure patterns already hit and fixed, plus a
debugging checklist). Written to hand to an LLM (or a human) either exploring the mod
fresh or debugging one specific broken texture in the converted result. See also
[docs/mw2019-fx-mapping-reference.md](mw2019-fx-mapping-reference.md) (the other source
pack) and `docs/filmmaker_effects_modifiers.md` (how the converted result is actually
wired into the DLL, `FxMode::On`/`Less`).

## 1. What it is

A from-scratch particle overhaul for CS:GO by an author going by **Povarehok**, built over
roughly 4 months (per the author's own release post) and explicitly made for
**moviemaking/cinematic editing**, not competitive play. It replaces almost every particle
system a player sees: HE grenade explosions, molotov/incendiary fire, C4 detonation, every
weapon's muzzleflash + tracer + shell casing + barrel smoke, player-hit impacts (blood,
headshots), environment impacts (wood/metal/concrete/wallbang), plus a few novelty extras
(an optional "money sprays out on headshot" swap, a taser shock-rope that keeps glowing
after the shot, warp/heat-distortion on muzzleflashes and explosions, a small lingering
fog radius after C4 detonates — "yaderka mode").

## 2. How CS:GO actually loads it (and why that's exactly what makes the CS2 port hard)

This is **not** a Workshop addon and **not** a scripted mod like the GMod ARC9/MW2019 pack
— there's no Lua, no VScript, no per-weapon config. It's a plain **Source 1 content-
replacement pack**: a folder tree of `.pcf` (particle definitions), `.vmt` (materials),
`.vtf` (textures), and `.mdl` (models) distributed as a `.rar` and installed through
**MIGI** — a third-party CS:GO tool that mounts extra "addon" folders as additional search
paths without touching the base game files. The author's own install instructions (quoted
from the mod's release notes) are literally:

> 1. If u want update impacts/explosive particles, just go to `Counter-Strike Global
>    Offensive\migi\csgo\addons\PARTICLESADDONFOLDER\particles` and replace
>    `explosives_fx.pcf` and `impact_fx.pcf`
> 2. If u want update weapon particles, go to the same `particles` folder, rename the
>    file to `PARTICLESADDONFOLDER.pcf`, and drop it in

That mechanism only works because of how Source 1's particle system resolves things:
**every particle system is looked up purely by its own name** (e.g. `impact_concrete`,
`weapon_muzzle_flash_assaultrifle`, `blood_impact_medium`, `explosion_c4_500`), never by
which file it physically lives in — whichever mounted `.pcf` defines that name *first* in
the active search paths wins. So the mod doesn't need to hook or patch anything: it just
ships `.pcf` files under the *same* relative path CS:GO already expects
(`particles/impact_fx.pcf`, etc.) that **redefine those exact same system names** with new
particle-editor content. In native CS:GO the game itself has no idea a mod is active — it's
silently shadowing the stock files.

**This is the single biggest reason the CS2 port is hard, not just an installation detail.**
CS2 has no such "first loose file with this name wins" precache resolution, and CS2 doesn't
understand Source 1 `.pcf`/`.vmt`/`.vtf` at all. There's no shortcut — every asset has to be
converted to Source 2 `.vpcf`/`.vmat`/`.vtex` (via `source1import`) and the DLL has to
explicitly swap to the converted file's own distinct path at runtime (see `ParticleFxRules.cpp`'s
`FXVAR`/`FXRULE` macros). Nothing about "just copy the folder in" works, and every one of
the texture problems in §10 traces back to some assumption in that conversion pipeline that
held for stock CS:GO content but not for this specific mod's assets.

The optional **"money on headshot"** feature uses the exact same file-swap trick, documented
in a text file the author bundled inside the mod itself (`Read if u wanna money FX.txt`):

> IF YOU WANT ADD MONEY FX FOR HEADSHOT:
> 1. Go to `Counter Strike: Global Offensive/migi/csgo/addons/p_betterparticlesmod/particles`
> 2. Change name for `impact_fx.pcf` to anything else
> 3. Change name for `impact_fxmoney.pcf` to `impact_fx.pcf`
> 4. Update your MIGI build.
> 5. Now u have some "CHOPPA" by static fx in game lol

In other words this isn't a scoped "only headshots change" toggle at the file level — it's
a **full swap of the entire impact-particle file**. `impact_fxmoney.pcf` is a complete
alternate version of `impact_fx.pcf` where the headshot-specific system names render a
dollar-bill/confetti burst instead of blood; you install one file or the other, never both.

## 3. The three variants on disk

```
reference/csgo effect mod/
├── p_betterparticlesmod_classic updated_c057b/p_betterparticlesmod_classic/   -> On/regular
├── p_betterparticlesmod_lessimpacts/p_betterparticlesmod_lessimpacts/         -> Less (impacts half)
├── p_betterparticlesmod_lesssmoke_22ac2/p_betterparticlesmod_lesssmoke/       -> Less (smoke/explosions half)
└── legacy_csgo_deps/                                                          -> 2 bundled stock CS:GO models the particles reference
```

These are the mod author's **own internal folder names** (note the `p_betterparticlesmod_`
prefix, not "Povarehok") — left exactly as-is on disk; only our own converted-output
namespace was renamed to `povarehok` (see [memory: modern-pack-mw2019-mapping] and the
particle-fx-effects-modifiers memory for that rename).

Each of these three is a full, independent copy of the same folder skeleton (`particles/`,
`materials/`, `models/`) — not a small diff patch. A plain-`classic` (no "updated") folder
existed once too and was deleted by the user 2026-07-02 after an md5 diff proved it
byte-identical to `classic updated`; On now targets `classic updated` directly and there's
no third "classic" variant anymore.

**Diffed per-file (2026-07-02):** `less_impacts` and `less_smoke` are each nearly identical
to `classic updated` — the **only** files that actually differ are `impact_fx.pcf`
(different in both, each its own way) and `explosions_fx.pcf` (identical "toned down"
version in both). Everything else (blood, tracers, muzzle flashes, molotov, money, snow
impacts, shells) is byte-for-byte the same content in all three folders. That's why our CS2
"Less" mode is built as a **per-file combination** — bullet impacts come from
`less_impacts`, everything else that differs comes from `less_smoke` — rather than picking
one folder wholesale.

## 4. File-type primer (Source 1 formats)

| Extension | What it is |
|---|---|
| `.pcf` | Binary "Particle Compiler File" — one file bundles many independently-named particle system definitions (operators, emitters, renderers, and **child** system references that spawn automatically with no scripting). CS:GO resolves systems by name across every mounted `.pcf`. |
| `.vmt` | Plain-text "Valve Material Type" — a shader name (`SpriteCard`, `UnlitGeneric`, the older `Sprite`, ...) plus `$key "value"` parameters. This is what a particle renderer or model actually points at. |
| `.vtf` | Binary "Valve Texture Format" — the bitmap data a `.vmt`'s `$basetexture` names. Can be a single image or, for animated sprites, embed a **sheet** resource (multiple frame rects + sequence timing) so one texture plays like a flipbook. |
| `.mdl`(+`.vvd`/`.dx90.vtx`/`.phy`) | Compiled Source 1 model — only 2 stock CS:GO models are needed here (`legacy_csgo_deps/`: a concrete debris chunk + a wood gib), bundled once from a legacy CS:GO `pak01` so the converter doesn't need a full CS:GO install. |

## 5. Feature → file map

Cross-referencing the author's own changelog text against the actual folder contents:

| Feature (author's words) | Real file(s) |
|---|---|
| HE grenade explosion | `particles/explosions_fx.pcf` (+ `explosions_fx2.pcf`) |
| Molotov / incendiary burn & explosion | `particles/inferno_fx.pcf` (+ `inferno_fx3.pcf`) |
| C4 explosion | also `explosions_fx.pcf` — grenade and C4 detonations are defined in the same file |
| Player hit impacts (headshots, blood) | `particles/blood_impact.pcf` |
| Environment impacts (wood/metal/concrete/wallbang) | `particles/impact_fx.pcf`, `impact_fx_smoke.pcf` |
| Snow-surface impacts | `particles/impact_fxsnow.pcf` |
| Money-on-headshot (optional) | `particles/impact_fxmoney.pcf` (full swap over `impact_fx.pcf`, see §2) |
| All weapon muzzleflashes + tracers + barrel smoke + shell casings | `particles/weapons/cs_weapon_fx.pcf` and `particles/polished_weapons_fx.pcf` (see §7 for why there are two) |
| Shell casing **models** (not just particles) | `models/shells/*.mdl` — one model per caliber (9mm, 5.56, .308/762nato, 12 gauge, .50 cal, .338 mag, 5.7, plus a couple of oddly-named ones like `aalpha12`/`vlk`) |
| Money-burst prop | `models/usMoney/` (a physical dollar-bill model) + `materials/usMoney/` (1/2/10/20/100-dollar bill textures) |
| Per-map ambient dust/fog | `particles/maps20/*.pcf` — **not mentioned in the changelog at all**, see §6 |

The author's "small features" list, mapped to how they're actually built:

- **Barrel smoke after shots** — a smoke puff/trail attached to the weapon's muzzle
  attachment point, defined inside `cs_weapon_fx.pcf`/`polished_weapons_fx.pcf` alongside
  the muzzleflash for the same weapon class.
- **3D model particles for concrete/wood/brick/stone impacts** — some impact systems use a
  `RenderModels`-style operator that spawns real physical debris chunk props (see the
  bundled stock CS:GO gib dependencies under `legacy_csgo_deps/models/`), so heavy impacts
  throw actual tumbling geometry, not just a flat decal + puff.
- **Taser shock-rope that keeps glowing after the shot** — a rope-renderer particle tied to
  the taser wire with a longer fade-out than stock, so the arc visibly lingers.
- **Warp on muzzleflashes/explosions/molotov/C4** — a mild heat-distortion/refraction child
  on the bigger effects, dialed subtly ("a little bit actually" per the author).
- **C4's small lingering fog radius** ("yaderka mode") — an extra low, ground-hugging fog
  puff child added to the C4 detonation system on top of the normal explosion.
- **Headshot sparks** — an extra spark child layered onto the headshot blood system in
  `blood_impact.pcf`.

## 6. Full file inventory (`classic updated` variant; identical layout in all 3)

```
particles/
  blood_impact.pcf              impact_fx.pcf             impact_fxmoney.pcf
  impact_fxsnow.pcf             impact_fx_smoke.pcf       explosions_fx.pcf
  explosions_fx2.pcf            inferno_fx.pcf            inferno_fx3.pcf
  polished_weapons_fx.pcf       weapons/cs_weapon_fx.pcf
```

— these **11 files are the only ones our conversion pipeline (`convert-povarehok-
source1.ps1`'s `$effectPcfs` list) actually touches.** `polished_weapons_fx.pcf` only
exists in `classic updated` (not in either `less_*` folder).

The mod tree also ships a bunch of **extra content the pipeline currently ignores
entirely** — worth knowing if something "worked in CS:GO but is just missing in CS2",
because it genuinely isn't converted at all, not broken:

- `particles/maps20/` — **per-map `.pcf` files** for 14 maps (`de_dust.pcf`, `de_nuke.pcf`,
  `de_inferno.pcf`, `de_train.pcf`, `de_mill.pcf`, `de_overpass.pcf`, `de_bank.pcf`,
  `de_aztec.pcf`, `de_house.pcf`, `de_shacks.pcf`, `cs_office.pcf`, `cs_italy.pcf`,
  `ar_monastery.pcf`, `gg_baggage.pcf`). CS:GO auto-loads a map-named particle file to
  override that specific level's ambient dust/fog/particle props. None of this is wired
  into the CS2 hook — map-specific FX polish from the original mod is simply not ported.
  If "some textures aren't working" turns out to be a *map ambience* issue specifically,
  this is the folder to look at.
- Assorted bonus/kitchen-sink content that looks like it was folded in from other community
  packs and is never referenced by the 11 core files or by any weapon: `baconnparticles.pcf`,
  `betterparticlesmod_classic.pcf`, `block.pcf`, `kkk.pcf`, `13391392139030810398.pcf`,
  `4442342.pcf`, `impact_fx_insurgency.pcf`. Checking file sizes/dates shows the first six of
  these are all within a few hundred bytes of `polished_weapons_fx.pcf` and dated across
  Nov 2022–Jan 2023 — they're superseded work-in-progress **snapshots of the weapon effects
  file** from earlier in development, not anything referenced by the mod's own install
  instructions. Matches the `materials/particles/tfa_ins2` / `materials/particle/ac/*`
  folders (reused Insurgency-mod textures) and a `materials/particles/minecraft/` folder.
  None of this is in the effect-PCF list, so none of it currently reaches CS2 either way.
- `particles/blood_impact.pcf90` / `.pcf900` — **not real `.pcf` files CS:GO would ever
  load** (wrong extension = inert even in native CS:GO); almost certainly stray
  backup/versioned copies the mod author left in the folder by accident. Ignore them.
- `models/shells/mw2019/` — a subfolder duplicating four shell models (`shell_12gauge`,
  `shell_50cal`, `shell_pistol`, `shell_rifle`) that also exist at the top level of
  `models/shells/`. Present identically in all three variants, so it's original mod
  content: Povarehok's own CS:GO mod already borrowed COD MW2019-styled brass models for
  those calibers, independent of the separate GMod ARC9/MW2019 pack this project also uses
  for the "Modern" mode. The duplication under an `mw2019/` path suggests the model's
  *internal* name baked into the `.mdl` doesn't match the path the particle system expects
  it at, so the author placed a second physical copy rather than relying on one resolution.

`materials/` mirrors the particle content: `effects/` (muzzle flash cards, explosion
sprites, tiled fire, lens flares...), `particle/` and `particles/` (impact dust, blood,
smoke, sparks, debris — the bulk of the texture count), `models/shells/` (per-caliber brass
textures), `usMoney/` (the money-burst bills/confetti). Real counts in `classic updated`:
**1,264 `.vmt` files, 954 `.vtf` files** across those folders.

## 7. Why there are two weapon-particle files

`particles/weapons/cs_weapon_fx.pcf` (the correctly-pathed, "real" weapon file CS:GO
expects) and a second, separately-named `particles/polished_weapons_fx.pcf` both ship side
by side and are both meaningfully sized (~305 KB each) — this isn't a case of one being a
stray leftover. The name lines up with a later changelog entry titled **"More Polished /
Clean UPDATE"**, whose notes say gun shots "might look cleaner" and that some "overspammed"
particles were removed — i.e. `polished_weapons_fx.pcf` is the *toned-down revision* of the
weapon effects, shipped as an addition alongside the original rather than replacing it
outright.

## 8. Where the "some textures don't work right" feeling comes from

Reading the material folders shows this mod's textures were assembled from a **large
number of different donor packs/games**, not authored from scratch as one consistent set.
Distinct source-pack naming shows up as top-level subfolders under `materials/`:

- `ac/` — a large recurring donor set (muzzleflash/blood/explosion sprites; also the only
  place inside this CS:GO mod where an MW2019-styled texture atlas turns up,
  `particle/ac/experimental/mw2019_vfx_fluid_pool_4_atlas_2x2`)
- `bb2/`, `bmclassic/`, `bmpep/`, `hl2mmod/` — Source-engine mod texture sets (muzzleflash
  sprites, glow effects)
- `tfa_ins2/` — Insurgency-styled smoke/tracer sprites (from a GMod TFA weapon base, the
  same lineage as the ARC9 packs used elsewhere in this project)
- `baconn/`, `minecraft/`, `insandstorm/`, and a handful of one-off single-texture folders

Because these were authored across different games/engines/years, they don't share one
consistent material convention — some use the modern `SpriteCard` shader, some use plain
`UnlitGeneric` with `$additive`, a few even use the old `Sprite` shader with
`$spriterendermode`/`$spriteorientation` keys. A few VMTs carry a **commented-out**
`$additive` line (the author trying additive blending, then disabling it) — anything
reading VMT text needs to strip comments before checking flags. There are also leftover
duplicate/stray files from development, e.g. `materials/effects/blueblackflash.txt` sitting
next to the real `blueblackflash.vmt` (an earlier draft saved with the wrong extension,
harmless clutter). This heterogeneity is the root cause behind most of the specific failure
patterns in §10 below — the pipeline has to handle several different, mutually-inconsistent
authoring conventions, not one clean asset set.

## 9. VMT keys that actually matter for the port

A typical particle-sprite VMT looks like:

```
"SpriteCard"
{
    "$basetexture"    "effects/mp5_muzzle_card/mp5_muzzle_card"
    "$additive"       "1"          <- ADD blend: ignores alpha entirely, colors just sum
    "$addself"        "3"          <- self-glow multiplier (only meaningful w/ additive)
    "$overbrightfactor" "1.5"      <- brightness multiplier past 1.0
    "$translucent"    "1"          <- alpha-blend instead (respects the alpha channel)
    "$vertexcolor"/"$vertexalpha"  <- lets per-particle tint/alpha drive the sprite
    "$DEPTHBLEND"     "1"          <- soft-particle fade against nearby geometry
}
```

Source 1's `SpriteCard` shader is **always unlit** — none of these particles ever received
scene lighting; any darkening was baked in by the artist at spawn time via color/alpha, not
by the renderer. CS2's equivalent renderer (`C_OP_RenderSprites`) defaults to **scene-lit**
unless `m_flSelfIllumAmount = 1.0` is explicitly set — this single default-flip is the #1
cause of "looks right in daylight, goes dark/boxy in shade" reports.

## 10. Known texture failure patterns (already diagnosed + fixed once — check these first)

Every one of these was hit for real during the CS2 conversion and is patched by
`fx/tools/postprocess_common.py` (shared by both packs). If a *specific* texture still looks wrong,
check whether it actually matches one of these patterns and whether the fix's detection
heuristic is missing it (rather than assuming a brand-new bug class):

| Symptom | Root cause | Fix |
|---|---|---|
| Solid white/error quad | Converted sprite renderer has a `m_hMaterial` ref but **no `m_hTexture`** — CS2's sprite renderer only reads `m_hTexture`, the legacy `m_hMaterial` is silently ignored (Source 1 warp/refract/heat-distortion quads and screen overlays have no CS2 equivalent) | Inject `m_hTexture` from the VMT's `$basetexture` if the converted `.vtex` exists, else delete the renderer entirely (an invisible emitter beats a white square) |
| Big glowing white square (molotov, C4, lens flares) | Some sprites export as **solid-white RGB with the actual shape stored only in alpha** (`particle_anamorphic_lens` etc.); CS2's `PARTICLE_OUTPUT_BLEND_MODE_ADD` **ignores alpha entirely**, so the whole square renders at full white | Premultiply: bake `RGB *= alpha` into the texture for any texture used *only* by additive renderers |
| Translucent smoke suddenly renders as its full square sheet-frame | Several VMTs carry `// "$additive" "1"` — tried and **disabled by the author** (a comment) — but a naive text search for `$additive` matches the commented line too, wrongly flipping the renderer to additive | Always match VMT parameters against **comment-stripped** text, never raw text |
| Smoke/atlas texture draws as a visible grid of frames instead of one cell | The mod's `.vtf` files store animated sprites as an embedded **sheet resource** (`.sht`-equivalent frame table). Generic `source1import` conversion **drops sheet data entirely** and exports a plain flat texture, so every particle samples the whole grid | Custom `export-source1-vtf.py` step: parse the VTF's embedded sheet info (or a lenient manual fallback), slice real frames, emit a `.mks` sequence file so `resourcecompiler` builds a proper `vtex_c` sheet |
| Dark fringe / "black ring" around soft smoke edges | Original VMT uses `$DUALSEQUENCE`/blend-mode keys (`ALPHA_FROM0_RGB_FROM1`-style dual-frame sampling); the rebuilt `.mks` sheets are single-mode, so the dual-sample keys read the wrong frame at the edges | Strip the dual-sequence combine keys from converted VPCFs entirely (reverts to plain single-frame sampling) |
| Flash/glow dims and gets a dark box around it specifically in shaded areas | See §9 — Source 1 `SpriteCard` is unlit; converted renderers omit `m_flSelfIllumAmount`, so CS2's default scene-lighting multiplies the sprite (and its alpha halo) toward black outdoors of direct light | Set `m_flSelfIllumAmount = 1.0` on every converted sprite/trail/rope renderer, and set `PARTICLE_OUTPUT_BLEND_MODE_ADD` wherever the *original* VMT said `$additive 1` |
| Debris/chunks float in mid-air at ledges/stairs | Converted `C_OP_WorldTraceConstraint` collision blocks default to a cached single-trace "infinite plane" mode; near an edge that plane extends past the real geometry | Force `COLLISION_MODE_PER_PARTICLE_TRACE` (real per-particle raycast — fine for an offline movie tool) |
| A lingering smoke *ribbon* seems to hang in mid-air, disconnected from the muzzle | `C_OP_RenderRopes` (rope renderer) draws one continuous ribbon across the emitter's **entire path** between two puffs, rather than reading as drifting smoke, once ported as-is | Convert rope renderers to sprite emitters (with frame blending) and shorten the particle lifetime |

## 11. Practical checklist for "this one texture still looks wrong in CS2"

1. Find the resource in the mod tree: `materials/<...>.vmt` (search by the name shown in
   `mirv_filmmaker fx names` or by grepping for the sprite/model name).
2. Read the **raw** VMT and note: is `$additive` present, and is it commented out? Is there
   a `$basetexture2` / `$DUALSEQUENCE` / sequence-blend key?
3. Check whether the referenced `.vtf` is a multi-frame sprite (look for repeated tiles in
   the raw texture, or just check if the effect is supposed to animate/flip).
4. Look at the corresponding **converted** `.vpcf` under
   `build/fx/povarehok-source1import/source2/content/.../particles/
   filmmaker/povarehok/<variant>/...vpcf` (after a `-Compile` run) and check whether it
   has `m_hTexture` set, whether `m_flSelfIllumAmount`/`m_nOutputBlendMode` got applied,
   and whether it matches one of the seven patterns in §10.
5. If it matches a known pattern but still renders wrong, the postprocess pass's
   **detection heuristic** (a regex/name-hint list, not a general rule) is probably just
   not catching this specific file — extend the relevant function in
   `fx/tools/postprocess_common.py` rather than hand-patching the one `.vpcf`.
6. If it matches **none** of the seven patterns, it's a genuinely new failure mode —
   capture a screenshot + the exact system name via `mirv_filmmaker fx log on` / `fx names`
   before guessing at a fix.
