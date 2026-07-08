# fx/ — in-game particle FX asset pipeline

This folder owns the **two custom particle FX packs** the filmmaker's Config → Effects
section can swap CS2's stock particles for. It's a build/release concern, kept separate
from `automation/` (which is test-only).

| Pack | Modes | Source inputs | Runtime namespace |
|---|---|---|---|
| **Povarehok** | On / Less | `reference/csgo effect mod/` (git-ignored, ~GB CS:GO mod) | `particles/filmmaker/povarehok/{regular,less/impacts,less/smoke}/` |
| **Modern** (MW2019) | Modern | [`sources/modern-warfare-gmod/`](sources/modern-warfare-gmod/) (committed, ~44 MB) | `particles/filmmaker/modern/...` |
| **Improved Ragdolls** | Runtime toggle | [`sources/improved-ragdolls/`](sources/improved-ragdolls/) canonical player `.phy` + current Valve models via VRF | `models/filmmaker/improved_ragdolls/...` |

Both packs are produced by **one converter run** and mounted at runtime by
`AfxHookSource2/Filmmaker/Movie/ParticleFx*.cpp` (swap tables: `ParticleFxRules.cpp`).

## Layout

```
fx/
  tools/                              the conversion pipeline
    convert-povarehok-source1.ps1     driver: stages both packs, imports, compiles, validates
    extract-modern-particles-gmod.py  (re)extracts the Modern source tree from a GMod install
    extract-insurgency-smoke.py       exports Insurgency Sandstorm smoke VTFs + catalog
    stage-insurgency-smoke.py         stages Insurgency picks into materials/particle/insurgency/
    export-source1-vtf.py             VTF -> vtex + sprite-sheet reconstruction
    postprocess_common.py             shared CS2 VPCF fixes (see docs/povarehok-csgo-mod-reference.md §10)
    postprocess_povarehok.py          Povarehok-only fixes + CLI entry point (drives postprocess_common/_modern too)
    postprocess_modern.py             Modern (MW2019)-only fixes
    convert-improved-ragdolls.py      Source 1 PHY metadata + VRF CS2 models -> ModelDoc ragdolls
    validate-povarehok-assets.py      runtime resource-closure check
  sources/
    modern-warfare-gmod/              committed Modern (MW2019) Source 1 inputs
    insurgency-sandstorm/             Insurgency smoke exports (see README; optional at convert)
```

## Build

```powershell
powershell -File fx/tools/convert-povarehok-source1.ps1 -Compile
```

- No GMod install needed — the Modern inputs are committed under `sources/`.
- VRF (`Source2Viewer-CLI.exe`) is used offline to decompile current CS2 CT/T agent
  models. Their meshes, materials, animations, hitboxes, and attachments are preserved;
  only ModelDoc physics is augmented.
- Add `-RefreshModernFromGmod` to re-pull the Modern inputs from a local GMod first.
- Add `-IncludeInsurgencySmoke` to export/stage Insurgency Sandstorm muzzle/impact/shell
  sprites into `materials/particle/insurgency/` (non-fatal if the game is not installed).
- Output lands under `build/fx/…` and is **not** committed.
- Before compiling, the content tree is **pruned to the runtime closure** derived from
  `ParticleFxRules.cpp`'s swap tables (the validator's `--emit-closure`). The mod ships far
  more than the DLL ever references — unpruned the pack was ~969 MB / 8,700 files, of
  which only ~15% was reachable; pruned it's roughly ~150 MB. Adding a new `FXRULE`
  automatically keeps its assets — there is no manual exclude list.

### Improved Ragdolls

`convert-improved-ragdolls.py` verifies the deduplicated canonical player sidecar by
SHA-256. It extracts 21 body definitions, 20 joint
constraints, damping, inertia, and the authored total mass of 800. The Source 1 masses
are proportionally normalized to Valve CS2's 272-unit player mass; copying 800 literally
made the port 2.94x heavier and produced the reported unnaturally fast collapse. VRF decompiles every
current `agents/models/ctm_*` and `agents/models/tm_*` player model to ModelDoc 28; the
tool then adds per-body mass markup, conical joints, and the six intermediate physics
bodies absent from stock CS2. Missing shapes are derived from each model's hitboxes so
the current Valve skeleton remains authoritative.

The generated models use an alternate `models/filmmaker/improved_ragdolls/agents/models/...`
namespace. The main-thread toggle selects the alternate model on living pawns and never
swaps an existing dead body; this lets CS2 initialize ragdoll physics normally and makes
mid-demo toggles apply safely to future deaths. Off selects genuine Valve physics.

The generated sources and compiled models live only under `build/fx/`. Hostage files,
Source 1 sound scripts/WAVs, and duplicate `.phy` sidecars are never staged. After a CS2
update, rebuild the FX pack to regenerate wrappers from Valve's current models.

`build.bat` runs this opt-in (3-second prompt, defaults to No; forced on a fresh checkout),
then stages the compiled `source_mvm_fx` game dir into `build/staging-release/fx/` so the
shipped build is self-contained. `automation/launch/launch-cs2-netcon.ps1` mounts it via
`USRLOCALCSGO`.

## Docs

- [`docs/filmmaker_effects_modifiers.md`](../docs/filmmaker_effects_modifiers.md) — how the modes work in-game.
- [`docs/povarehok-csgo-mod-reference.md`](../docs/povarehok-csgo-mod-reference.md) — Povarehok conversion + texture-failure patterns.
- [`docs/mw2019-fx-mapping-reference.md`](../docs/mw2019-fx-mapping-reference.md) — how MW2019 effects map onto CS2 weapons.
