# Effects modifiers (Config panel EFFECTS section)

Runtime particle-effect control for demo playback: per-category **On / Less / Modern /
Off** (each category only offers the modes it has real assets for) over the demo's visual
effects, toggled live from the Config panel (`mirv_filmmaker config`) or the console
(`mirv_filmmaker fx ...`), persisted to `%APPDATA%\HLAE\filmmaker_fx.json`.

Fresh runtime defaults are fully opt-in: the master switch and all eight categories start
**Off**, including Map Ambience. With no saved configuration, startup and demo entry do not
arm the particle hook or load anything under `particles/filmmaker/`. Explicitly saved
configurations still load exactly as stored.

Selecting a non-Off category automatically turns the master on and queues only that
category's selected pack. Money on Headshot and newly added custom block/swap rules do the
same. Pending names are deduplicated and resolved at one resource per frame. Switching a
category replaces obsolete jobs; turning the master Off cancels all pending work while
retaining the configured modes and already-resolved handles until the next level change.

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
  land on a cached infinite plane at stair/ledge height, and physical impact chunks/bits
  have their residual bounce zeroed so they settle after contact.
- Impact smoke/dust renderers are kept alpha-blended and capped to neutral overbright:
  additive/overbright converted smoke stacked repeated bullet hits into red/orange clouds
  instead of normal alpha fade. Re-run just these idempotent repairs on an existing tree
  with `--runtime-impact-fixes-only`.
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

| Key | UI label | Modes | Matches (path prefix on the vpcf resource name) |
|---|---|---|---|
| `impacts` | Bullet Impacts | On/Less/Off | `particles/impact_fx/`, `particles/water_impact/`, `particles/breakable_fx/` |
| `tracers` | Bullet Tracers | On/Modern/Off | weapon-fx paths whose name contains `tracer` |
| `weaponfx` | Muzzle Flash & Shells | On/Modern/Off | `particles/weapons/cs_weapon_fx/`, `particles/unified_weapon_fx/` |
| `blood` | Blood | On/Off | any path containing `blood` (blood_impact/, impact_fx/, screen splatter) |
| `explosions` | HE Grenade | On/Less/Modern/Off | explosion folders, minus `c4`/`bomb` names |
| `bombfx` | Bomb (C4) | On/Less/Off | explosion folders, `c4`/`bomb` in the name |
| `molotov` | Molotov Fire | On/Off | `particles/burning_fx/`, `particles/inferno_fx*` |
| `mapfx` | Map Ambience | On/Off | `particles/maps/`, `particles/ambient_fx/`, `particles/environment/`, `particles/rain_fx/`, `particles/critters/` |

`particles/ui/` (HUD/MVP effects) is always excluded. CS2 smoke grenades are deliberately
not touched because the gameplay smoke is a volumetric system, not a normal particle swap.

Modes (unsupported ones snap to On on the C++ side; the panel only offers the real ones):

- **On**: converted Povarehok `regular` assets.
- **Less**: the mod's reduced variants -- bullet impacts from `less/impacts`, HE/bomb
  explosions from `less/smoke`. Only offered where those folders genuinely differ.
- **Modern**: the converted MW2019 pack. Class flashes follow the pack's own weapon Lua
  (rifles `muzzleflash_ar`, SMGs `muzzleflash_smg`, LMGs `muzzleflash_lmg`, autosnipers the
  `mvm_muzzleflash_sniper_auto` composition around `muzzleflash_dmr`, the AWP/bolt snipers
  the `mvm_muzzleflash_sniper_awp` composition around `muzzleflash_smg`, pistols
  `muzzleflash_pistol(_deagle)`, shotguns `muzzleflash_shotgun`, silenced
  `muzzleflash_suppressed`), tracers map to the `mw2019_tracer` family, and HE maps to
  `explosion_grenade` -- the actual detonation system of the pack's frag grenade. Systems
  with no modern equivalent pass through vanilla rather than silently mixing packs, with
  one deliberate exception: **brass shell casings** (`weapon_shell_casing_*`) map to the
  Povarehok casing systems in every non-Off mode, because those render the actual
  converted MW2019 shell meshes (`models/shells/*`); CS2 calibers without a mod
  counterpart use the nearest one (45acp/5.7 -> 9mm, MAG-7/Nova -> the shotgun shell,
  AWP -> the .50 cal).

  Behaviors the GMod mod produced with **Lua** are recreated as composed PCFs by the
  post-process passes `apply_povarehok_gameplay_composites` (postprocess_povarehok.py) and
  `apply_modern_gameplay_composites` (postprocess_modern.py) (idempotent; run standalone on
  an already converted tree with `--gameplay-composites-only` + a per-file resourcecompiler
  pass):

  - **Spray-gated barrel smoke** (reworked 2026-07-03, user: "smoke only after multiple
    shots in a short time"): per-flash `mvm_spray_*` wrapper systems (flash +
    `barrel_smoke(_plume)`) are written by the postprocess, and the hook upgrades a
    muzzle-flash swap to the wrapper only during sustained fire -- it counts creations of
    each vanilla flash name on the demo-tick clock (`kSprayPairs` / `SprayHotLocked` in
    ParticleFxSpray.cpp: consecutive shots <= 32 ticks apart, smoke from the 4th on). Povarehok
    gets the same wrappers around its bundled
    `ac_muzzle_shotgun_alt_barrel_smoke` (the "trails missing in On" report); single-shot
    snipers keep per-shot smoke via composition children instead, since a spray gate can
    never trigger on a bolt gun. (CS2's own `weapon_muzzle_smoke*` systems stay mapped
    too, but current CS2 was never observed creating them in demo playback.)
    The converted `barrel_smoke_trail(_b)` renderers (both packs) are switched from ropes
    to sprites and shortened from 5s to 2-3s: as ropes the engine stretched one ribbon
    across the muzzle's whole sweep, the "smoke arc floating in mid-air" artifact.
  - **Sniper shots** (AWP + autosnipers): the `mvm_muzzleflash_sniper_*` compositions add
    the pack's M82 treatment -- `m82_shocksmoke` dust ring around the shooter,
    `barrel_smoke_plume`, and `muzzle_heatwave` heat distortion. **This is the ONLY place
    heat distortion appears** (mod-authentic scope; briefly added to every unsuppressed
    class flash on 2026-07-03, reverted the same night per user request).
  - **Grenade flight smoke trail**: CS2 has no in-flight particle for HE/smoke/flash/decoy;
    demo playback does create one `particles/entity/spectator_utility_trail.vpcf` per
    thrown grenade (control-pointed to the projectile), and Modern swaps it to
    `mvm_grenade_trail`. Its `mvm_grenade_smoke_trail(_b)` children are authored
    behavior-12 systems whose emit rate is driven by **control-point speed**
    (`PF_TYPE_CONTROL_POINT_SPEED`, 30-500 u/s -> 0-16 puffs/s), so a grenade at rest --
    landed, or a smoke stuck against a wall -- emits nothing instead of puffing forever
    (the previous un-capped clone did; user report 2026-07-03). One trail style for all
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

| File | Responsibility |
|---|---|
| `ParticleFx.cpp` | Core: engine binding + demo state, the apply-now reseek, the public class methods, the main-thread pump, and the `fx` command dispatch. |
| `ParticleFxRules.cpp` | Name classification into categories, the FXRULE variant/swap tables (vanilla → pack assets), per-mode target selection, swap-target pre-queueing. |
| `ParticleFxHook.cpp` | Vtable resolution + the create-collection detour + JIT-manifest target loading. The crash lessons from the 2026-07-02 dump sessions live in its comments. |
| `ParticleFxSpray.cpp` | The spray-gated barrel-smoke state machine (`kSprayPairs`, demo-tick heat). |
| `ParticleFxMoney.cpp` | Money-on-headshot candidates + game-event plumbing (`ParticleFx_OnGameEvent`). |
| `ParticleFxSettings.cpp` | JSON persistence to `%APPDATA%\HLAE\filmmaker_fx.json`. |
| `ParticleFxDiagnostics.cpp` | FxDebugHud state, the `fx recent`/`fx names` telemetry ring, agent-log writers. |

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
screenshot pixel audit. Per swapped creation the hook queues the vanilla+target names and
the engine's returned collection; the main-thread pump resolves the muzzle attachment
("muzzle"/"muzzle_flash" on the first-person viewmodel weapon entity, world weapon
fallback) and the spawn point (SEH-guarded scan of the collection header for the nearest
world float3 — control points/emitter origins — method `cp-scan`; falls back to the
authored `C_INIT_PositionOffset` mean rotated into muzzle space, method `config-offset`,
which can only measure the configured offset, not a wrong engine binding). Samples land
as NDJSON in `%APPDATA%\HLAE\fx_align.jsonl`; the harness aggregates per
weapon-class/effect and fails on mean distance > threshold (default 2.5 units), pass-rate
< 80%, or missing required coverage. During a run the spray-heat gate is bypassed
(`fx align gate off`) so single shots still produce wisp samples for every class — the
harness always restores it (`gate on`) in teardown.
