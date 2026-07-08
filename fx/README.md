# fx/ — in-game particle FX asset pipeline

This folder owns the **two custom particle FX packs** the filmmaker's Config → Effects
section can swap CS2's stock particles for. It's a build/release concern, kept separate
from `automation/` (which is test-only).

| Pack | Modes | Source inputs | Runtime namespace |
|---|---|---|---|
| **Povarehok** | On / Less | `reference/csgo effect mod/` (git-ignored, ~GB CS:GO mod) | `particles/filmmaker/povarehok/{regular,less/impacts,less/smoke}/` |
| **Modern** (MW2019) | Modern | [`sources/modern-warfare-gmod/`](sources/modern-warfare-gmod/) (committed, ~44 MB) | `particles/filmmaker/modern/...` |
| **Improved Ragdolls** | Runtime toggle | current Valve player models via VRF (Valve `PHYS` block grafted post-compile) | `models/filmmaker/improved_ragdolls/...` |

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
    convert-improved-ragdolls.py      VRF CS2 models -> improved-ragdoll namespace + Valve PHYS graft
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

`convert-improved-ragdolls.py` runs in two phases. **Prepare:** VRF decompiles every
current `agents/models/ctm_*` and `agents/models/tm_*` player model and relocates it
verbatim under `models/filmmaker/improved_ragdolls/agents/models/...` (no physics editing).
**Graft** (`--graft-phys`, run by `convert-povarehok-source1.ps1` *after* resourcecompiler):
raw-extracts each agent's stock `vmdl_c` and grafts its compiled `PHYS` block — Valve's
exact joint reference frames, masses (272), inertia and damping — into the recompiled
improved model, rebuilding the resource block table cleanly.

The graft exists because **CS2's compiler drops Valve's authored ragdoll physics on any
recompile**: a decompiled agent recompiles to `m_joints = []` with zero mass, and no
source-level ModelDoc node can recreate a joint's reference frames (`PhysicsJointConical`
only fills the child frame, leaving the parent frame at identity → limits orient around the
wrong axis → the ragdoll goes floppy and collapses). The earlier attempt to port jahpeg's
Source 1 PHY metadata (per-body mass markup, conical joints, six extra bodies) is why the
old models distorted; that path is gone. `player-profile.phy` is now unused.

The generated models use an alternate `models/filmmaker/improved_ragdolls/agents/models/...`
namespace. The main-thread toggle selects the alternate model on living pawns and never
swaps an existing dead body; because the grafted PHYS is byte-for-byte Valve physics, the
result is indistinguishable from stock (the toggle is currently a correct-but-neutral base
for future reactive ragdoll modes). Off selects genuine Valve physics.

Improved Ragdolls have their **own standalone build**, fully independent of the particle
packs above: [`fx/tools/build-improved-ragdolls.ps1`](tools/build-improved-ragdolls.ps1)
(its own content/game dirs, gameinfo, and CS2 junctions). It shares only the runtime
`source_mvm_fx` mount at stage time and owns solely the `models/filmmaker/improved_ragdolls/`
subtree there. The particle converter must never build ragdolls, and this script must never
build particles. After a CS2 update, rerun it to regenerate wrappers from Valve's current
models.

```
powershell -ExecutionPolicy Bypass -File fx\tools\build-improved-ragdolls.ps1 -Stage
```

`build.bat` runs it as a separate opt-in step (3-second prompt, defaults to No; forced when
none exist), and it stages its models into `build/staging-release/fx/` so the shipped build
is self-contained. `automation/launch/launch-cs2-netcon.ps1` mounts the shared FX dir via
`USRLOCALCSGO`.

## Docs

- [`docs/filmmaker_effects_modifiers.md`](../docs/filmmaker_effects_modifiers.md) — how the modes work in-game.
- [`docs/povarehok-csgo-mod-reference.md`](../docs/povarehok-csgo-mod-reference.md) — Povarehok conversion + texture-failure patterns.
- [`docs/mw2019-fx-mapping-reference.md`](../docs/mw2019-fx-mapping-reference.md) — how MW2019 effects map onto CS2 weapons.
