# fx/ — in-game particle FX asset pipeline

This folder owns the **two custom particle FX packs** the filmmaker's Config → Effects
section can swap CS2's stock particles for. It's a build/release concern, kept separate
from `automation/` (which is test-only).

| Pack | Modes | Source inputs | Runtime namespace |
|---|---|---|---|
| **Povarehok** | On / Less | `reference/csgo effect mod/` (git-ignored, ~GB CS:GO mod) | `particles/filmmaker/povarehok/{regular,less/impacts,less/smoke}/` |
| **Modern** (MW2019) | Modern | [`sources/modern-warfare-gmod/`](sources/modern-warfare-gmod/) (committed, ~44 MB) | `particles/filmmaker/modern/...` |

Both packs are produced by **one converter run** and mounted at runtime by
`AfxHookSource2/Filmmaker/Movie/ParticleFx.cpp`.

## Layout

```
fx/
  tools/                              the conversion pipeline
    convert-povarehok-source1.ps1     driver: stages both packs, imports, compiles, validates
    extract-modern-particles-gmod.py  (re)extracts the Modern source tree from a GMod install
    export-source1-vtf.py             VTF -> vtex + sprite-sheet reconstruction
    postprocess_common.py             shared CS2 VPCF fixes (see docs/povarehok-csgo-mod-reference.md §10)
    postprocess_povarehok.py          Povarehok-only fixes + CLI entry point (drives postprocess_common/_modern too)
    postprocess_modern.py             Modern (MW2019)-only fixes
    validate-povarehok-assets.py      runtime resource-closure check
  sources/
    modern-warfare-gmod/              committed Modern (MW2019) Source 1 inputs
```

## Build

```powershell
powershell -File fx/tools/convert-povarehok-source1.ps1 -Compile
```

- No GMod install needed — the Modern inputs are committed under `sources/`.
- Add `-RefreshModernFromGmod` to re-pull the Modern inputs from a local GMod first.
- Output lands under `build/fx/…` and is **not** committed.
- Before compiling, the content tree is **pruned to the runtime closure** derived from
  `ParticleFx.cpp`'s swap tables (the validator's `--emit-closure`). The mod ships far
  more than the DLL ever references — unpruned the pack was ~969 MB / 8,700 files, of
  which only ~15% was reachable; pruned it's roughly ~150 MB. Adding a new `FXRULE`
  automatically keeps its assets — there is no manual exclude list.

`build.bat` runs this opt-in (3-second prompt, defaults to No; forced on a fresh checkout),
then stages the compiled `source_mvm_fx` game dir into `build/staging-release/fx/` so the
shipped build is self-contained. `automation/launch/launch-cs2-netcon.ps1` mounts it via
`USRLOCALCSGO`.

## Docs

- [`docs/filmmaker_effects_modifiers.md`](../docs/filmmaker_effects_modifiers.md) — how the modes work in-game.
- [`docs/povarehok-csgo-mod-reference.md`](../docs/povarehok-csgo-mod-reference.md) — Povarehok conversion + texture-failure patterns.
- [`docs/mw2019-fx-mapping-reference.md`](../docs/mw2019-fx-mapping-reference.md) — how MW2019 effects map onto CS2 weapons.
