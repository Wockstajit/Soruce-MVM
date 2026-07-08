# Effects modifiers (Config panel EFFECTS section)

The EFFECTS section also exposes **Improved Ragdolls**. It defaults Off and can be
toggled with `mirv_filmmaker ragdoll on|off|toggle`. The selection is carried by living
player models so CS2 can initialize physics normally at death. Existing bodies are never
swapped when the option changes; the new setting applies to future deaths. Off selects
Valve's original player models and stock physics. Agent appearance remains unchanged.

Runtime particle-effect control for demo playback: per-category **On / Less / Modern /
Off** (each category only offers the modes it has real assets for) over the demo's visual
effects, toggled live from the Config panel (`mirv_filmmaker config`) or the console
(`mirv_filmmaker fx ...`), persisted to `%APPDATA%\HLAE\filmmaker_fx.json`.

## Offline live-match testing (`mvm_test`)

For shooting bots on a local practice map (not demo playback), use the **`mvm_test`**
harness instead of the demo-only CONFIG panel:

```text
mvm_test start [map]     load map + bots (default de_dust2) and arm test mode
mvm_test menu on|off|toggle   FX effects panel (Insert does the same)
mvm_test status          print active flag, map, menu, fx hook state
mvm_test stop            disarm test mode and close the menu
```

One-click launch (hook + FX pack + `mvm_test start`):

```batch
automation\launch\test.bat
```

Or:

```powershell
powershell.exe -ExecutionPolicy Bypass -File automation\launch\launch-cs2-test.ps1 -Map de_dust2
```

The **Insert** key on a **live map** (not demo playback) auto-arms test mode and opens the
effects-only FX TEST panel — no `mvm_test start` required if you are already on a map.
Press Insert again to close. `mvm_test start` also opens the menu automatically after load.
**G** toggles **UI mouse ↔ game mouse** while the panel is open (same as the MOUSE button
in the panel header). UI mode lets you click effect toggles; game mode returns control for
aiming and shooting.

Same per-category controls as CONFIG → EFFECTS. No demo reseek in live play — shoot to see swaps.

Fresh runtime defaults are fully opt-in: the master switch and all eight categories start
**Off**, including Map Ambience. With no saved configuration, startup and demo entry do not
arm the particle hook or load anything under `particles/filmmaker/`. Explicitly saved
configurations still load exactly as stored.

Selecting a non-Off category automatically turns the master on and makes only that
category's selected pack eligible for swaps. Loading is **front-loaded into a warm-up
burst** at demo open and after settings switches (added 2026-07-06; the previous
demand-only trickle spread one cold blocking load per 0.25-2s across minutes of playback
and read as a stutter storm):

- **Demo open (level change):** the per-level handle cache is wiped (required -- handles
  dangle across map transitions), the FULL active target set is queued, and once the new
  level settles (demo tick advancing on two consecutive pumps, 3s wall-clock fallback)
  the main-thread pump burst-resolves it under a per-frame time budget (100ms while the
  demo is paused or within 10s of open, 20ms otherwise). ~100-145 targets complete in
  roughly 1-2.5s concentrated at the demo's first moments.
- **Settings switch mid-demo** (category mode, master on, moneyshot, custom rules): the
  new selection's uncached targets are queued and burst on the next pump (full budget if
  paused). Finishing a warm-up while paused triggers the debounced apply-reseek so the
  frozen frame recomposes under the resolved swaps.
- **Disk prefetch:** the first warm-up also kicks a once-per-process background thread
  (`ParticleFxPrefetch.cpp`) that plain-reads every file of the mounted pack
  (`USRLOCALCSGO`) to warm the OS file cache -- no engine APIs, so it is thread-safe --
  making each main-thread blocking resolve cheap.
- **Lazy fallback unchanged:** a target the burst missed still fails open once, gets
  queued on first real use, and is resolved by the old adaptive trickle (slow rate with
  heavier backoff during playback, faster while paused, never at the main menu).

`fx state` reports `warmupPhase` (0 idle/trickle, 1 waiting for level settle, 2
bursting) and the mvm_debug log gains `fx.resolve` "warmup armed / burst start / done"
lines plus `fx.prefetch` totals. Switching a category replaces obsolete jobs; turning
the master Off cancels all pending work while retaining the configured modes and
already-resolved handles until the next level change.

Two converted asset packs feed the modes:

- **On / Less** — the CS:GO-era **Povarehok** mod (reference copies in
  `reference/csgo effect mod/`). Less is only offered where the mod's less folders
  actually differ from regular (verified per-file 2026-07-03: only `impact_fx`
  and `explosions_fx` do) -- so Less appears on Bullet Impacts, HE Grenade, and Bomb.
- **Modern** — the GMod **ARC9 / MW2019** packs (class muzzle flashes, per-shot barrel
  smoke, the mw2019 tracer family, the MW frag explosion), extracted read-only from the
  local GMod install and converted through the same pipeline. Offered on Bullet Tracers,
  Muzzle Flash & Shells, and HE Grenade. (`'more'` is accepted as a legacy console alias
  of `on`.)

The runtime does not invent fake sprites or swap grenades to unrelated stock CS2
particles; every mode points at Source 2 resources converted from the real Source 1
`.pcf` assets with `kristiker/source1import`.

## Asset conversion

Everything the converter needs is repo-local, so a plain run converts with no
machine-specific setup:

- `misc/source1import/` — clone of <https://github.com/kristiker/source1import> (gitignored;
  re-clone if missing).
- `misc/Source2Converter/` — clone of <https://github.com/REDxEYE/Source2Converter> with its
  `SourceIO` submodule initialized (gitignored; re-clone if missing).
- `reference/csgo effect mod/legacy_csgo_deps/` — the two stock CS:GO models the mod's
  particles reference (`models/gibs/wood_gib01b`, `models/props_debris/concrete_chunk07a`,
  each as `.mdl/.vvd/.dx90.vtx/.phy`) plus their VMT/VTF materials, extracted once from a
  legacy CS:GO `pak01_dir.vpk`. Bundled so conversion has no dependency on a CS:GO install.
- Python deps: `pip install Pillow numpy vdf dataclassy==0.10.4 parsimonious==0.10.0
srctools==2.3.4 vtf2img vpk`.
- **Modern pack sources:** the MW2019 Source 1 inputs are **committed in-repo** under
  `fx/sources/modern-warfare-gmod/` (the ~44 MB extracted tree — 3 PCFs plus their VMT/VTF
  closure), so a build needs no GMod install. To refresh that tree from an updated GMod
  install, run the converter with `-RefreshModernFromGmod` (requires the workshop items
  `2910505837` (ARC9 Weapon Base), `3258297368` ([ARC9] Modern Warfare 2019), and
  `2459720887` (Modern Wokefare Base) subscribed): `fx/tools/extract-modern-particles-gmod.py`
  re-pulls the three PCFs (`arc9_fas_muzzleflashes`, `arc9_fas_explosions`, `mw2019_tracer`)
  plus their material closure out of the GMAs and GMod base VPKs (nothing GMod-side is
  modified). A handful of sprites the FAS PCFs reference exist in no local source (GMod
  itself falls back for them); their renderers are pruned by the post-process
  (`modern-missing-materials.txt` lists them).

Convert the referenced Source 1 assets before expecting the modes to look like the old mod:

```powershell
powershell -File fx/tools/convert-povarehok-source1.ps1 -Compile
```

(`-Source1ImportDir` / `-Source2ConverterDir` / `-LegacyCsgoDir` or the matching env vars
override the repo-local defaults.)

The wrapper stages the mod PCFs under a stable namespace before running
`utils/materials_import.py -b cs2` and `utils/particles_import.py -b cs2`, exports the mod
VTF textures (including sprite-sheet reconstruction: embedded `.sht` data becomes per-frame
slices + a `.mks` + a `.vtex` input, without which every atlas draws as a frame GRID),
post-processes the converted VPCFs (`fx/tools/postprocess_povarehok.py`, which also drives
`postprocess_common.py` and `postprocess_modern.py`), converts the
bundled MDLs with Source2Converter, compiles everything with CS2's `resourcecompiler.exe`,
and finally validates the full runtime closure (`fx/tools/validate-povarehok-assets.py`).

The post-process pass exists because CS2 renders these conversions differently than
Source 1 did; it never invents placeholder assets, it only repairs/tones the real ones:

- FP muzzle-flash closure: overbright/alpha/radius tone-down (Source 2 lighting blows
  out the Source 1 values).
- Molotov roots: incompatible cinematic children removed, fire toned.
- Dead references (children/fallbacks the mod itself never shipped) stripped so CS2
  stops logging `Failed loading resource` at demo load.
- Sprite renderers without an `m_hTexture` draw SOLID WHITE QUADS (CS2 ignores the
  legacy `m_hMaterial`): the original texture is injected from the VMT's `$basetexture`
  when it exists, otherwise the renderer (Source 1 heat-distortion/warp quads, screen
  overlays) is removed.
- `$DUALSEQUENCE` combine keys are stripped: the rebuilt sheets are single-mode, and the
  dual-sample modes produce background-dependent dark fringes ("black ring" smoke).
- Impact world-trace constraints are forced to per-particle tracing so debris does not
  land on a cached infinite plane at stair/ledge height. Physical impact chunks/bits have
  residual bounce zeroed and spin capped at 0.5 seconds, so they tumble in flight but stop
  rotating shortly after reaching the floor.
- Impact smoke/dust renderers are kept alpha-blended and capped to neutral overbright:
  additive/overbright converted smoke stacked repeated bullet hits into red/orange clouds
  instead of normal alpha fade. Non-fire bullet impacts and weapon wisps that still
  referenced the volumetric `vistasmokev1` sheet are retargeted to Insurgency-exported
  thin smoke (`materials/particle/insurgency/*`, with in-pack `insandstorm_*` /
  `smoke1` / `sq_fulldustfront1_2` fallbacks) by `patch_non_fire_vistasmoke_replacements`
  in `fx/tools/postprocess_povarehok.py`. Molotov and explosion systems keep vistasmoke.
  Re-run just these idempotent repairs on an existing tree with `--runtime-impact-fixes-only`
  (blending only) or a full post-process pass after re-converting.
- VMT parameters are matched against **comment-stripped** text (2026-07-03): several mod
  VMTs carry `// "$additive" "1"` -- additive tried and disabled by the author -- and a
  comment-blind match turned translucent smoke additive. Additive ignores the alpha mask,
  so every soft cloud rendered its full square sheet frame (the molotov/C4 gray squares);
  `repair_wrongly_additive_renderers` also undoes this on an already-converted tree.
- Solid-white-RGB textures used ONLY by additive renderers get their alpha mask
  premultiplied into RGB (`particle_anamorphic_lens` = the big white square on molotov +
  C4 explosions: the lens streak lives in alpha, and CS2's ADD blend ignores alpha).
- Sprite renderers backed by `.mks` sheets get `m_bBlendFramesSeq0 = true`: Source 1
  SpriteCard blended sheet frames by default, the conversions lost it, and stepped frames
  read as jittery/pixelated smoke.
- Weapon + bullet-impact smoke sheets get `m_nAnimationType = ANIMATION_TYPE_FIT_LIFETIME`
  (`fix_looping_smoke_animation` in `postprocess_povarehok.py`, 2026-07-06): the mod authored
  these `C_OP_RenderSprites` smoke systems with a fixed `m_flAnimationRate`, and CS2 wraps a
  fixed-rate sheet (`frame = age*rate mod frames`), so a single shot's muzzle smoke and a
  single wall hit's impact smoke visibly **re-played the sprite over and over** (user report
  2026-07-06). FIT_LIFETIME maps the sheet to `[0..1]` of the particle lifetime = plays once,
  no wrap. Scoped by texture to smoke/dust sheets (`thinsmoke`, `insandstorm`, `wd_gfx_steam`,
  `smoke1`, `fas_dust`, `water_splash`, ...) so muzzle-flash flames, sparks, fire, and blood
  keep their intentional fast flicker. Idempotent; runs in the full pass and both in-place
  entries (`--gameplay-composites-only`, `--runtime-impact-fixes-only`).
  **Round 2 (2026-07-06 pm, smoke still double-played):** FIT_LIFETIME alone was NOT
  sufficient -- the animation rate still multiplies under FIT (`frame = normalizedAge *
  rate * sheetTime`, per the engine/VRF frame math), so any smoke renderer with rate > 1
  overshot the sheet and wrapped anyway (e.g. `cursedgovno1`'s wall-impact splash at
  rate 10 replayed 3-6x per hit; Modern's `muzzleflash_suppressed` dust at rate 5). Two
  additional levers, both in the same passes: `fix_looping_smoke_animation` now **caps
  `m_flAnimationRate` at 1.0** on these smoke renderers and also walks the **Modern**
  `arc9_fas_muzzleflashes` tree (originally Povarehok-only), and
  `common.clamp_sheet_sequences` strips the `LOOP` flag from the one-shot smoke/dust/
  steam/splash `.mks` sheets themselves (`CLAMP_SMOKE_SHEET_RE` -- deliberately narrower
  than the renderer-texture regex: sparks/blood/debris/airburst sheets keep their
  intentional looping flicker) so any residual overshoot holds the last frame instead of
  replaying. Changed sheets need their `.vtex` recompiled (the in-place entries print
  the list).
- `patch_modern_tracer_glow_brightness` is a **one-shot tone pass** (it multiplies
  alpha/radius by 0.5) and only runs in the full conversion pipeline;
  `--gameplay-composites-only` passes `tune_tracer_brightness=False` (caught 2026-07-06:
  an in-place re-run halved the already-halved tracer glow a second time).
- Modern muzzle-flash sprite bursts that reference `hl2_muzzleflash` are retargeted to the
  pack's higher-resolution `fas_muzzleflash_test_b` sheet and receive `C_OP_PositionLock`
  so the burst stays attached to the muzzle. Their authored animation rate, alpha behavior,
  and frame-blend setting stay intact.
  The idempotent in-place patch entry (`--gameplay-composites-only`) runs the three repairs
  above plus the gameplay composites; changed files still need a `resourcecompiler` pass.

A pack that compiles particles but not materials/textures/models renders as white/error
quads in game -- the validator's job is to catch exactly that.

```text
particles/filmmaker/povarehok/regular/...
particles/filmmaker/povarehok/less/impacts/...
particles/filmmaker/povarehok/less/smoke/...
particles/filmmaker/modern/arc9_fas_muzzleflashes/...
particles/filmmaker/modern/arc9_fas_explosions/...
particles/filmmaker/modern/mw2019_tracer/...
```

Those namespaces are what `ParticleFxRules.cpp` uses in its variant tables. The launcher mounts
the compiled output automatically via `USRLOCALCSGO`: it prefers the pack staged into the
shipped build (`build/staging-release/fx/source_mvm_fx`) and falls back to the converter's
working output (`build/fx/povarehok-source1import/source2/game/source_mvm_fx`). If the converted pack is not compiled and mounted into CS2's resource search path,
the swap fails open and the original CS2 effect plays. This is intentional: missing assets
must not fall back to random placeholders.

## Categories

| Key          | UI label              | Modes              | Matches (path prefix on the vpcf resource name)                                                                   |
| ------------ | --------------------- | ------------------ | ----------------------------------------------------------------------------------------------------------------- |
| `impacts`    | Bullet Impacts        | On/Less/Off        | `particles/impact_fx/`, `particles/water_impact/`, `particles/breakable_fx/`                                      |
| `tracers`    | Bullet Tracers        | On/Modern/Off      | weapon-fx paths whose name contains `tracer`                                                                      |
| `weaponfx`   | Muzzle Flash & Shells | On/Modern/Off      | `particles/weapons/cs_weapon_fx/`, `particles/unified_weapon_fx/`                                                 |
| `blood`      | Blood                 | On/Off             | any path containing `blood` (blood_impact/, impact_fx/, screen splatter)                                          |
| `explosions` | HE Grenade            | On/Less/Modern/Off | explosion folders, minus `c4`/`bomb` names                                                                        |
| `bombfx`     | Bomb (C4)             | On/Less/Off        | explosion folders, `c4`/`bomb` in the name                                                                        |
| `molotov`    | Molotov Fire          | On/Off             | `particles/burning_fx/`, `particles/inferno_fx*`                                                                  |
| `mapfx`      | Map Ambience          | On/Off             | `particles/maps/`, `particles/ambient_fx/`, `particles/environment/`, `particles/rain_fx/`, `particles/critters/` |

`particles/ui/` (HUD/MVP effects) is always excluded. CS2 smoke grenades are deliberately
not touched because the gameplay smoke is a volumetric system, not a normal particle swap.

Modes (unsupported ones snap to On on the C++ side; the panel only offers the real ones):

- **On**: converted Povarehok `regular` assets.
- **Less**: reduced variants -- bullet impacts from `less/impacts`, HE/bomb explosions and
  muzzle FX from `less/smoke`. Less bullet impacts are derived from On at **50%** (half the
  emit counts on every impact system, plus half the smoke alpha and radius). Less muzzle
  smoke (from `less/smoke`) stays at **70%** alpha and radius. Timing and textures are
  unchanged.
- **Modern**: the converted MW2019 pack. Class flashes follow the pack's own weapon Lua
  (rifles `muzzleflash_ar`, SMGs `muzzleflash_smg`, LMGs `muzzleflash_lmg`, autosnipers the
  `mvm_muzzleflash_sniper_auto` composition around `muzzleflash_dmr`, the AWP/bolt snipers
  the `mvm_muzzleflash_sniper_awp` composition around `muzzleflash_smg`, pistols
  `muzzleflash_pistol(_deagle)`, shotguns `muzzleflash_shotgun`, silenced
  `muzzleflash_suppressed`). Central sprite leaves use `fas_muzzleflash_test_b`, while their
  authored timing, alpha behavior, and frame-blend settings remain intact; the retained
  `C_OP_PositionLock` keeps the center burst attached to the muzzle.
  Tracers map to the
  `mw2019_tracer` family (AR/rifle → `mw2019_tracer`, snipers → `mw2019_tracer_fast`,
  pistols/SMGs → `mw2019_tracer_small`, shotguns → `mw2019_tracer_slow`), and HE maps
  to `explosion_grenade` -- the actual detonation system of the pack's frag grenade.
  Systems with no modern equivalent pass through vanilla rather than silently mixing packs.
  **Brass shell casings are SEPARATE per pack** (user directive 2026-07-07 "modern and pov
  have different casings; nothing shared"): On/Less render **Povarehok's** casings (generic
  CS:GO shell meshes, `models/shells/shell_*.vmdl`), and Modern renders its **own** casings
  under `particles/filmmaker/modern/weapons/cs_weapon_fx/weapon_shell_casing_*` that point at
  the **`models/shells/mw2019/*`** shell meshes and carry the Modern `port_smoke` eject puff.
  (Before this, Modern reused Povarehok's casing SYSTEMS and Modern's `port_smoke` was bolted
  onto them -- two kinds of cross-pack sharing, both removed: `patch_modern_shell_port_smoke`
  now builds the Modern casings and strips the puff from the Povarehok ones, and the shell
  rows in `ParticleFxRules.cpp` are `FXRULE_MODERN` with a `modern/weapons/cs_weapon_fx/...`
  target instead of `FXRULE_MODERN_BP`.) CS2 calibers without a per-pack counterpart use the
  nearest one (45acp/5.7 -> 9mm, MAG-7/Nova -> the shotgun shell, AWP -> the .50 cal). GMod's
  separate `DoEject` Lua effect has no CS2 top-level create to hook, so the eject puff rides
  as a PCF child of each Modern casing system.

  Behaviors the GMod mod produced with **Lua** are recreated as composed PCFs by the
  post-process passes `apply_povarehok_gameplay_composites` (postprocess_povarehok.py) and
  `apply_modern_gameplay_composites` (postprocess_modern.py) (idempotent; run standalone on
  an already converted tree with `--gameplay-composites-only` + a per-file resourcecompiler
  pass):
  - **Barrel smoke timing = GMod AfterShotParticle emulation (final 2026-07-06)**: BOTH
    packs run every class's barrel smoke through spray-gated `mvm_spray_*` wrappers
    (`kSprayPairs` in ParticleFxSpray.cpp: Povarehok flashes incl. the AWP/hunting-rifle
    snipers, Modern class flashes + `_fp` twins, and the Modern `mvm_muzzleflash_sniper_*`
    compositions). **The GMod reference** (ARC9 base, read from the workshop Lua
    2026-07-06): barrel smoke is NOT per-shot -- every shot `StopEmission()`s the active
    smoke, and ONE new smoke spawns ~one RPM-interval AFTER firing stops
    (`sh_think.lua` AfterShot flag; attach = engine `PATTACH_POINT_FOLLOW` on the muzzle).
    **Our emulation** (the hook can only act at creation time; no StopEmission verb is
    RE'd for CS2 -- `CNewParticleEffect::StopEmission` exists in client.dll RTTI if ever
    needed): the hook upgrades only the shot that STARTS a burst (`SprayHeat.count == 1`,
    plus the `kSprayUpgradeCooldownTicks` ~2s re-arm guard bounding rapid-tap overlap),
    and the wrapper's smoke child carries `m_flDelay = AFTERSHOT_SMOKE_DELAY` (0.45s in
    postprocess_common.py) -- so taps and short bursts bloom their single wisp right
    after firing stops, exactly like GMod; long sprays get one wisp (blooming mid-spray
    rather than at the very end -- the known deviation). History: per-shot smoke children
    stacked plumes and double-smoked the snipers (the SCAR-20 report); the sniper comps
    are smoke-less (flash + shock dust + heatwave) with the plume on their wrappers.
    (CS2's own `weapon_muzzle_smoke*` systems stay mapped too, but current CS2 was never
    observed creating them in demo playback.)
    Modern `barrel_smoke_trail(_b)` keep GMod's **`C_OP_RenderRopes`** (not sprites) with
    CS2 muzzle-tip offset and world-space drift tuning. Povarehok Less wrappers reference
    the 30%-reduced `less/smoke` plume while preserving the same flash. **Buoyant rise
    (2026-07-06):** the converted Modern rope wisps came through with `m_Gravity = [0,0,0]`
    and sat glued to the barrel; `_force_modern_smoke_rise` now forces every barrel-smoke
    `C_OP_BasicMovement` gravity to a clean upward `[0,0,22]` (barrel_smoke_trail{,_b} and
    barrel_smoke_plume), matching Povarehok's `weapon_muzzle_smoke_long` rising plume
    (`[0,0,25]`). The muzzle-local spawn offset is untouched, so the wisp still starts at
    the barrel tip and only the post-spawn drift lifts upward.
    **Povarehok barrel smoke = Modern's recipe (2026-07-06 pm, user report "Povarehok
    smoke isn't at the barrel like Modern / wisps drift loosely"):**
    `patch_cs2_muzzle_smoke_alignment` and `patch_cs2_muzzle_rope_trail_alignment` now
    mirror Modern's working barrel alignment instead of the old viewmodel-space forward
    jet: world-space (`m_bViewModelEffect = false`, so third person / free cam see it),
    spawn at the shared barrel-tip offset (`MUZZLE_OFFSET_BARREL_TIP` in
    postprocess_common, same `[0,0,-0.5]..[0.5,0,0]` Modern uses -- keep in sync with
    FxAlign.cpp's `kModernCfgOffset`), zeroed emission jets (the plume rises via +Z
    gravity instead of streaming 40-120 u/s forward), and a fast 0.1-0.15s fade-in (the
    old 0.5-0.6s fade-in left a visible gap after the per-shot puff died, one component
    of the reported "smoke plays twice"). **Tight barrel tracking:** both packs' barrel
    smoke and rope wisps now carry a FULL-lifetime `C_OP_PositionLock`
    (`ensure_full_position_lock`, start/end times 1e6) -- the engine default lock window
    (~1s) let already-spawned smoke stop following and drift behind the gun during
    reloads/weapon movement. The lock adds the muzzle control point's translation on
    top of the particles' own world-space rise/noise, so it does NOT reintroduce the
    local-space "glued noise" bug documented in `_tune_modern_rope_trail_particles`.
    **De-clumped wisps:** `weapon_muzzle_smoke_long` had FIVE overlapping ribbon/trace
    children (an earlier `ac_muzzle*trail*` name over-match also attached
    `ac_muzzle_smg_trail{,5}` and `ac_muzzle_shotgun_trail` -- short per-shot traces,
    not barrel wisps); the over-matched three are stripped
    (`WRONG_SMOKE_LONG_CHILDREN`), and the real wisp pair's spawn spread is tamed
    (was +/-7 u/s sideways velocity noise, which scattered the rope's points into the
    "crumpled clump" the 2026-07-06 screenshot showed).
  - **FINAL smoke-motion recipe (2026-07-06, after three rounds): stock CS2's own
    pattern -- WORLD-pass + brief 0->0.1s PositionLock + engine-driven CP.** The user's
    target behavior: smoke sits at the barrel while emitted but LAGS in the air like
    real smoke when the gun moves/reloads ("draws like a sheet with the motion of the
    gun"). That is exactly what Valve's `weapon_muzzle_smoke_long` does: world-pass
    rendering, newborn particles locked to the muzzle for 0.1s then free, and the
    engine continuously driving the instance's muzzle CP so emission tracks the gun
    (and a thrown weapon). All barrel smoke in BOTH packs (wisps, plume, sustained
    smoke) now uses `ensure_brief_position_lock` + `m_bViewModelEffect = false` on the
    **world** twins. **First-person reload follow (2026-07-06):** muzzle flashes already
    ship world + `_fp` twins (`muzzleflash_*` / `muzzleflash_*_fp`); barrel smoke and
    rope wisps now mirror that split (`barrel_smoke_fp`, `barrel_smoke_trail*_fp`,
    `weapon_muzzle_smoke_long_fp`, wisp `_fp` copies). `_fps` CS2 systems and
    `mvm_spray_*_fp` wrappers route to the viewmodel-pass smoke so wisps track the
    visible barrel during reload; world / non-`_fp` wrappers keep the world-pass assets
    for third person and chase cam (`g_fpFxSuppress`).
    **Modern rifle barrel wisp visibility/duration FIXED; first-person "follow" NOT (live
    demo test 2026-07-06 pm).** Two user reports: (a) "AK/M4 Modern muzzle smoke disappears
    really quickly, doesn't show all the way"; (b) "the smoke doesn't follow the barrel on
    reload/inspect, it floats where the barrel was". Live netcon+screenshot testing on the
    `all weapon test` demo established:
    - **The spray-gate wrapper works in normal forward play** (shotgun/autosniper wrappers
      observed spawning). An earlier "the wrapper never spawns" reading was a TEST ARTIFACT:
      repeatedly `demo_gototick`-ing to the SAME tick and replaying trips the
      `kSprayUpgradeCooldownTicks` (~2s) re-arm guard at an identical `tickNow`, so the
      burst-start upgrade is suppressed. In genuine forward play the first shot of each
      burst upgrades (`kSprayHotCount == 1`).
    - **(a) was real and is FIXED** in `_tune_modern_rope_trail_particles`
      (`barrel_smoke_trail{,_b}`): the converted rifle wisp died at 20% of its 5s lifespan
      (`C_OP_FadeAndKill m_flEndFadeOutTime = 0.2`), faded from birth
      (`m_flStartFadeOutTime = 0.0`), and was very faint (`m_nAlphaMax = 38`). Now holds
      then fades over a ~3s life (`0.4`->`1.0`, lifetime `3.0`) at `m_nAlphaMax = 100`.
      Verified: firing now shows a clearly visible, longer-lived wisp. (The LMG/DMR
      `barrel_smoke_plume` already faded over full life at alpha 220 -- why only rifles/SMG
      showed the bug.)
    - **(b) SOLVED (2026-07-07) -- smoke emits from the moving muzzle AND flies free in the
      air.** Wanted behavior (user, verified by screenshot): new smoke comes off the barrel as
      the gun sways/reloads/inspects, and the emitted rope then drifts freely in the air like
      real smoke -- NOT a rigid line glued to the muzzle. `make_modern_smoke_fp` now does two
      things beyond the viewmodel-pass flip:
        1. Injects the `m_controlPointConfigurations` "game" driver
           (`_add_fp_muzzle_follow_config`): `PATTACH_POINT_FOLLOW` on the weapon's
           `"muzzle_flash"` attachment (`"self"`), copied from stock CS2 first-person weapon FX
           (`uweapon_muzflsh_ak47_fps.vpcf`, decompiled with the VRF CLI). It makes the ENGINE
           drive control point 0 to the moving viewmodel muzzle every frame, so the emission
           SOURCE tracks the gun. Without it a swapped viewmodel effect's CP0 is set once at
           the muzzle-flash create event and never updated -- the old "smoke hangs where it was
           fired" bug. Stock `weapon_muzzle_smoke` follows the exact same way.
        2. Keeps the world twin's BRIEF 0->0.1s `C_OP_PositionLock` + WORLD-space motion
           (inherited from `_tune_modern_rope_trail_particles`). Already-emitted particles fly
           free in world space and drift/lag in the air as the camera swings.
      **SUPERSEDED (2026-07-07 pm, user directive "Povarehok looks better -- whatever
      Povarehok is doing, do it for Modern too; keep the packs separate, nothing shared"):**
      the follow-config + brief-lock recipe above is REPLACED. `make_modern_smoke_fp` now uses
      the **full-lifetime `C_OP_PositionLock` (0->1e6) + NO follow config** -- the exact recipe
      Povarehok's `weapon_muzzle_smoke_long_fp` already used (both packs route through
      `make_modern_smoke_fp`, so this keeps them consistent). The user judged Povarehok's
      full-lock wisp as following the gun through reload/inspect better than Modern's
      brief-lock split, and that the old brief-lock + follow-config recipe read as "multiple
      wisps (old + new)" -- new smoke emitting at the moving muzzle WHILE brief-locked
      particles flew free and drifted behind. Full-lock collapses that into ONE coherent
      following wisp. The full lock ADDS the engine-driven muzzle CP's translation on top of
      the particle's own rise/noise (does NOT freeze it -- see `common.ensure_full_position_lock`),
      so it rides the barrel without going rigid. Applied to Modern's OWN `arc9_fas_muzzleflashes`
      assets (verified self-contained -- no `/povarehok/` refs), so the mechanism matches while
      the assets stay separate. The prior "rigidly stuck" reports were the full lock combined
      with LOCAL space or the follow-config; full lock + WORLD space + engine-driven CP (no
      follow config) is the Povarehok recipe and does NOT read as stuck.
      **DEAD END -- do NOT reintroduce:** flipping the wisp to LOCAL space (`m_bLocalSpace = true`)
      still reads as rigidly stuck. The follow config ALONE (without the free-drift brief lock) is not enough
      either -- it only moves the spawn point. Muzzle-flash `_fp` twins
      (`make_modern_muzzleflash_fp`) are unaffected; flashes are instantaneous. Idempotent
      (full conversion or `--gameplay-composites-only` + resourcecompiler). Note: one wrapper
      upgrade per burst emits ~1.5s (trail) / 2.2s (plume); a reload/inspect that starts AFTER
      emission ends sees only the drifting tail, not fresh muzzle smoke -- bump the trail's
      `m_flEmissionDuration` if continuous muzzle smoke through the whole reload/inspect is
      wanted.
    Tried and REVERTED along the way (all read as rigid/frozen smoke in game):
    (a) full-life lock time rewrites (`1e6/1e6`, `0/1e6`); (b) injecting the stock
    `m_controlPointConfigurations` PATTACH_POINT_FOLLOW block into swap targets (the
    engine's dispatch already drives swapped instances' CPs; the injected config
    overrode/froze it -- `remove_muzzle_follow_config` strips it from patched trees);
    (c) viewmodel-pass rendering with a bare `m_bLockRot` lock (rides the camera/gun
    rigidly -- never lags in the air).
  - **MW2019 tracer wisp animation (2026-07-06):** the `mgbase_tracer_trail{,_faint}`
    rope children animate in GMod by V-scrolling `fas_smoke_beam` along the ribbon.
    The converted files carry `m_flTextureVScrollRate = -50` (field still exists in
    the engine schema, confirmed via reference/cs2-offsets), but with no explicit
    `m_flTextureVWorldSize` the scroll was imperceptible -- the wisps read as static.
    `patch_modern_tracer_trail_scroll` sets `m_flTextureVWorldSize = 128` (one repeat
    per 128 units -> ~0.4 repeats/s of visible flow); idempotent, runs in-place too
    (unlike the once-only brightness pass).
  - **Povarehok plume restored to the stock CS2 recipe (the black-column screenshot):**
    the converted `weapon_muzzle_smoke_long` IS Valve's asset; the decompiled stock file
    renders its 46-83 dark-gray rope **additively** (`PARTICLE_OUTPUT_BLEND_MODE_ADD` --
    the alignment patch used to strip ADD blanket-style, alpha-blending the dark grays
    into an opaque black column) with spread `[-10,-10,10]..[10,10,15]` (an interim
    +/-1.5 "de-clump" over-tightened the rope into a dense column). The patch now
    re-adds ADD + `m_flSelfIllumAmount = 1.0` (targeted at `beam_smoke_01` renderers
    only -- blanket ADD would square-frame translucent sheet smoke) and the stock
    spread. The FAS wisp trails additionally run through Modern's own
    `_tune_modern_rope_trail_particles` so both packs' wisps are byte-equivalent in
    look and physics.
  - **Sniper shots** (AWP + autosnipers): the `mvm_muzzleflash_sniper_*` compositions add
    the pack's M82 treatment -- `m82_shocksmoke` dust ring around the shooter,
    `barrel_smoke_plume`, and `muzzle_heatwave` heat distortion. **This is the ONLY place
    heat distortion appears** (mod-authentic scope; briefly added to every unsuppressed
    class flash on 2026-07-03, reverted the same night per user request).
  - **MW2019 tracer CS2 discipline** (2026-07-04): routed parents get GMod lifecycle
    (`LifespanFromVelocity` + `Decay`, not `FadeAndKillForTracers`) plus CS2 motion fixes
    (`MoveBetweenPoints` @ 13k u/s, single emit, zero muzzle offset) while keeping MW2019
    textures. Automatic-weapon parent `mw2019_tracer` drops sniper-only children; glow/trail
    leaves halve alpha/radius/self-illum; parent `RenderTrails` use `m_flLengthFadeInTime`
    and head/tail alpha taper; rope trail children (`mgbase_tracer_trail*`) keep converted
    GMod `fas_smoke_beam` with V-scroll (scrolling smoke wisps along the beam).
  - **Grenade flight smoke trail**: CS2 has no in-flight particle for HE/smoke/flash/decoy;
    demo playback does create one `particles/entity/spectator_utility_trail.vpcf` per
    thrown grenade (control-pointed to the projectile), and Modern swaps it to
    `mvm_grenade_trail`. Its `mvm_grenade_smoke_trail(_b)` children are authored
    behavior-12 systems whose emit rate is driven by **control-point speed**
    (`PF_TYPE_CONTROL_POINT_SPEED`, 30-500 u/s -> 0-16 puffs/s), so a grenade at rest --
    landed, or a smoke stuck against a wall -- emits nothing instead of puffing forever
    (the previous un-capped clone did; user report 2026-07-03). The two layers retain the
    earlier `fas_dust_a` / `fas_dust_b` textures, alpha, radius, lifetime, and fade values;
    the deployed smoke cloud remains CS2's vanilla volumetric system. One trail style for all
    grenade types -- the engine uses a single system name, so per-nade styling is not
    possible at the particle layer. On/Less keep the stock spectator line.
  - **Heat distortion restored**: converted explosions lost vanilla's refraction children
    in the swap, and the pack's Source 1 refract quads were removed as unrenderable. The
    pass adds the STOCK CS2 distort systems (`explosion_hegrenade_distort`,
    `explosion_c4_distort01d_1k`) as children of the converted HE/C4 systems (both packs),
    and rewrites `muzzle_heatwave(_long)` as full behavior-12 systems cloned from the
    stock HE distort's renderer (refract keys + `warp_ripple3_normal` sampled in
    LUMINANCE mode + additive output). Do NOT go back to injecting bare
    `m_flRefractAmount` keys into the converted behavior-8 files: that compiles but never
    takes the refract shader path, so the warp normal map renders as an opaque
    purple/rainbow swirl (the 2026-07-03 bug report). Color scale is `[0, 0, 0]`, NOT the
    stock file's warm tan tint: with `PARTICLE_OUTPUT_BLEND_MODE_ADD`, additive zero adds
    nothing, so only the refraction offset is visible (real heat-shimmer, invisible except
    where the background has detail to bend). The stock tint reads fine against a large
    HE fireball but at muzzle scale against a flat wall it WAS the entire visible effect
    -- an expanding warm blob with no apparent distortion (2026-07-03 night report).
    `muzzle_heatwave` is scoped to the sniper compositions only (see above) -- it is
    mod-authentic (no MW2019 Lua references it on any other class) and per user request.

- **Off**: default CS2 pass-through for that category.

**HE Grenade and Bomb (C4) are separate rows** so the packs can mix -- e.g. Povarehok's
`explosion_c4_500` on the bomb with the MW2019 frag explosion on HE. The
MW2019 pack's own C4 is a small breaching charge, so the bomb deliberately has no Modern
mode.

Blood maps to the mod's cinematic blood impact/headshot systems, so spray, flow, smoke, and
air-trail children come from the asset pack.

**Money on Headshot** (`mirv_filmmaker fx moneyshot on|off`) swaps the headshot-only
particles (`blood_impact_headshot`, `impact_helmet_headshot`, ...) to the converted money
burst -- no event gating; those systems are only ever created on real headshot hits (the
old event-window design missed single lethal headshots because particles spawn before the
`player_hurt`/`player_death` events within the tick).

**Taser/Zeus is untouched by the variant tables**: its `weapon_tracers_taser*` wire
systems are not mapped, so they play vanilla.

**Variant tables must map top-level systems only.** Child systems are created internally by
the engine and bypass the hook. Verify what a demo actually creates with
`mirv_filmmaker fx names <filter>` and then add mappings only for those top-level names.

## How it works

### Code layout (`AfxHookSource2/Filmmaker/Movie/`)

The runtime is split into focused translation units behind one public header
(`ParticleFx.h`; shared internal surface in `ParticleFxInternal.h`):

| File                        | Responsibility                                                                                                                                            |
| --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ParticleFx.cpp`            | Core: engine binding + demo state, the apply-now reseek, the public class methods, the main-thread pump, and the `fx` command dispatch.                   |
| `ParticleFxRules.cpp`       | Name classification into categories, the FXRULE variant/swap tables (vanilla → pack assets), per-mode target selection, swap-target pre-queueing.         |
| `ParticleFxHook.cpp`        | Vtable resolution + the create-collection detour + JIT-manifest target loading. The crash lessons from the 2026-07-02 dump sessions live in its comments. |
| `ParticleFxSpray.cpp`       | The spray-gated barrel-smoke state machine (`kSprayPairs`, demo-tick heat).                                                                               |
| `ParticleFxMoney.cpp`       | Money-on-headshot candidates + game-event plumbing (`ParticleFx_OnGameEvent`).                                                                            |
| `ParticleFxSettings.cpp`    | JSON persistence to `%APPDATA%\HLAE\filmmaker_fx.json`.                                                                                                   |
| `ParticleFxDiagnostics.cpp` | FxDebugHud state, the `fx recent`/`fx names` telemetry ring, agent-log writers.                                                                           |

### The hook

One Detours hook on `particles.dll`'s `CParticleSystemMgr` create-collection body catches
particle instantiation:

1. `CreateInterface("ParticleSystemMgr003")` resolves the particle manager.
2. Slot 15 (`FindParticleSystem`) stays callable for resolving swap target names.
3. Slot 17's shared create body is detoured, including the internal direct-call path.
4. The hook reads the resource name off the handle, classifies it, and either passes it
   through or swaps it to a resolved target. Per-category Off is pass-through; explicit
   custom `fx block` rules still swap to `particles/dev/empty.vpcf`.

Swap targets are resolved on the main thread only. Resolving inside the create hook can
re-enter the resource system during particle creation, so the hook is cache-hit-only: an
unresolved target fails open once and gets queued for later resolution.

The pending queue is rebuilt from one authoritative desired-target set after settings or
level changes. A disabled master always publishes an empty target set and queue, including
when diagnostic logging explicitly installs the hook. Level changes invalidate all cached
handles and rebuild only currently enabled targets; level changes while Off leave the
queue empty.

Non-precached targets are loaded through the engine's just-in-time manifest path instead
of the plain single-resource blocking load. That keeps dependency handles fixed up and
avoids the crash path seen when a previously unseen target was loaded mid-create.

Toggles apply to the current paused moment automatically. Settings changes request a
debounced one-tick-backward `demo_gototick`, which destroys live particles and replays the
recent event stream under the new rules. While the demo is playing, the automatic reseek is
skipped to avoid hitching; `mirv_filmmaker fx apply` forces it manually.

Limits: surface decals are not particles and never change; effects older than the last
demo full-packet are not replayed; long-lived ambient systems are usually created once at
map/demo load, so `mapfx` changes may need a demo reload.

## Console reference

```text
mirv_filmmaker fx set <category> <on|less|modern|off> per-category control ('more' = legacy alias of on)
mirv_filmmaker fx on|off                             master switch
mirv_filmmaker fx state                              status + counters
mirv_filmmaker fx log on|off                         capture every creation
mirv_filmmaker fx recent [n]                         print last n captured/acted creations
mirv_filmmaker fx names [filter]                     aggregated per-name creation counts
mirv_filmmaker fx block <substr>                     custom rule: block names containing substr
mirv_filmmaker fx swap <substr> <target.vpcf>        custom rule: swap matching names
mirv_filmmaker fx unblock|unswap <substr>            remove custom rule(s)
mirv_filmmaker fx rules                              list custom rules
mirv_filmmaker fx test <name>                        dry-run the decision for one name
mirv_filmmaker fx apply                              re-create the current moment's live effects now
mirv_filmmaker fx moneyshot on|off                   event-gated money effect on headshot hits
mirv_filmmaker fx align on|off|report|clear          Modern muzzle-FX alignment probe (Source units)
mirv_filmmaker fx align gate on|off                  spray-wisp heat gate (off = every shot wisps, TESTING ONLY)
mirv_filmmaker fx align threshold <units>            per-sample pass distance (default 2.5)
```

Tuning workflow: `fx log on`, play the moment, inspect `fx names`, then adjust the variant
tables in `ParticleFxRules.cpp`. Runtime events also mirror into the `mvm_debug` log
(`fx.create`, `fx.install`, `fx.event`, `state.fx`).

## Automated coverage check

`automation/verify/verify-fx-allweapons.ps1` replays an every-weapon test demo (default:
`all weapon test .dem` in game/csgo) once per mode profile (`-Profiles modern,less` by
default), with `fx log on`. Before playback it also verifies lazy-precache state behavior:
all-Off produces a zero queue, selecting one category/pack auto-enables the master without
changing other modes, repeated/switch selections do not grow stale work, master Off
cancels the queue, and Money/custom rules opt in automatically. From the hook's own
`fx names` counters it then asserts each
expected group — muzzle flashes (incl. silenced), tracers, sustained-fire muzzle smoke,
impacts, HE, bomb, molotov — was both created by the demo (seen) and swapped (acted), and
writes `unmapped-<profile>.txt` listing weapon-path systems nothing acted on (the feed for
new variant-table entries, e.g. thrown-grenade trails). Periodic screenshots land in the
run folder for visual review (white quads, trail-follows-grenade, muzzle alignment).
Requires CS2 up via `automation/launch/launch-cs2-netcon.ps1` with the pack mounted.

## Muzzle alignment check (Source units, not pixels)

`automation/verify/verify-modern-muzzle-alignment.ps1` verifies that the Modern pack's
muzzle flash / first-shot barrel smoke / sustained wisp spawn AT the weapon muzzle, using
the in-game probe `mirv_filmmaker fx align` (`Movie/FxAlign.cpp`) instead of the retired
screenshot pixel audit. The probe covers BOTH packs (2026-07-07): `EffectKindFor`
classifies `/modern/` (muzzleflash_*, barrel_smoke*, mvm_spray_*) AND `/povarehok/`
(weapon_muzzle_flash_*, weapon_muzzle_smoke*). Per swapped creation the hook queues the
vanilla+target names and the engine's returned collection; the main-thread pump resolves
the muzzle attachment and the spawn point.

**First-person viewmodel muzzle resolution — FIXED (2026-07-07).** The reference is now the
FIRST-PERSON viewmodel weapon's `muzzle_flash` attachment (what a `m_bViewModelEffect`
particle actually anchors to), resolved via `ResolveViewmodelWeaponEntityIndex`
(CosmeticModelSwap). The earlier probe measured 0/997 samples against the viewmodel and fell
back to the WORLD muzzle (~5-17u off): the first-person viewmodel weapon is class
`C_CS2HudModelWeapon` (NOT the world weapon class), a CLIENT-ONLY entity at index >= 0x4000,
so `ReadActiveViewmodelWeaponState`'s class match returned -1 and `EntityAt(idx)` could not
round-trip it. The new resolver drops the class match and returns the entity POINTER; the
attachment read is SEH-guarded (`SehReadAttachment`) because the client viewmodel entity can
be mid-reconstruction during live playback (an unguarded read faulted CS2 ~8s into a capture
run). Post-fix: 148/148 samples resolve `source=viewmodel/muzzle_flash`. Use
`mirv_filmmaker fx align vmprobe` to dump, for the spectated player, the world vs viewmodel
muzzle side by side (confirms which reference resolved and the delta).

**Full spatial pass, both packs (2026-07-07).** Ran the entire ~6:18 `all weapon test .dem`
per pack with the probe armed: Modern 637 samples / Povarehok 628 FP samples across all 10
weapon classes, 100% resolving `source=viewmodel/muzzle_flash`. Saved to
`automation/runs/fx-fullpass/{modern,povarehok}.jsonl`.

**The `cp-scan` ABSOLUTE distance is NOT the effect's true offset — do not tune against it.**
cp-scan takes the nearest world float3 in the collection header to the muzzle. The full pass
proved it never finds CP0: Modern reports a near-constant ~18u fingerprint per weapon (same
relative vector regardless of weapon = a fixed structural point ≈ the weapon origin, NOT the
emit point; even instantaneous silenced flashes read 8-13u where CP0 would be ~0), and
Povarehok reports 100-330u with std up to 150u (it grabs the WORLD-DRIFTED smoke rope points,
which lag into the air by design). Per-axis std dev is the only relative signal that stands
out: Modern **shotgun** wisp has both the highest median (30.8u) and the highest variance
(std 13/23/18u vs ~5u for every other weapon).

**GROUND TRUTH = the authored `C_INIT_PositionOffset` (static, in the `.vpcf` files).** The
effect spawns at CP0 (engine-driven to the viewmodel muzzle — proven by the 100% viewmodel
reference resolution + the `_fp` `PATTACH_POINT_FOLLOW` "muzzle_flash" config) PLUS this
authored local offset. Extracted for every FP file, both packs:
- **Modern**: every FP muzzle flash = local `[0,0,-0.5]..[0.5,0,0]` (≈0.25u forward/down of
  the muzzle CP); barrel smoke `_fp` = the same or no offset at all (spawns exactly at CP0).
  All roots carry `m_bViewModelEffect=true`; smoke carries the follow config.
- **Povarehok**: FP flashes mostly have NO PositionOffset (spawn exactly at CP0); FP muzzle
  smoke = `[0,0,-0.5]..[0.5,0,0]` (≈0.25u). Roots carry the VM flag; `weapon_muzzle_smoke_long_fp`
  is the follow-tracked one.
So BOTH packs author every first-person muzzle flash and barrel smoke to spawn **≤0.5u from the
viewmodel muzzle** — i.e. AT the barrel, by construction. Any perceived "a little in front" is
the `muzzle_flash` attachment's own model position (barrel tip, forward of the receiver) or the
smoke sprite's visual RADIUS (a 3-8u puff extends forward of its centroid) or Povarehok's
intended world-space drift — NOT a spawn-offset error.

**Child systems inherit the viewmodel pass from their parent root** (verified 2026-07-07):
Modern's confirmed-correct `muzzleflash_ar_fp` has 10 children that ALL lack
`m_bViewModelEffect`, only the root has it, and it aligns correctly — so a child without the
flag is normal/correct in BOTH packs (Povarehok's flash children lacking the flag is not a bug).

To read the AUTHORED offsets, parse `C_INIT_PositionOffset` `m_OffsetMin/Max` from the text
`.vpcf` under `build/fx/povarehok-source1import/source2/content/source_mvm_fx/particles/filmmaker/`.
The runtime probe remains useful only for the REFERENCE check (does the viewmodel muzzle
resolve) and relative std-dev outliers, not for absolute placement.

Samples land as NDJSON in `%APPDATA%\HLAE\fx_align.jsonl`; the harness aggregates per
weapon-class/effect and fails on mean distance > threshold (default 2.5 units), pass-rate
< 80%, or missing required coverage. During a run the spray-heat gate is bypassed
(`fx align gate off`) so single shots still produce wisp samples for every class — the
harness always restores it (`gate on`) in teardown. NOTE: the `all weapon test .dem` is
~6:18 long; a capture that only runs ~60s covers just the first weapons — play the full
demo per pack for complete coverage.
