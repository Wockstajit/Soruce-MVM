# Modern Warfare (GMod ARC9) — extracted FX source tree

This is the **committed, self-contained Source 1 input** for the "Modern" (MW2019) FX
pack. It removes the build-time dependency on a local Garry's Mod install: the FX
converter reads these files directly instead of reaching into
`G:\SteamLibrary\steamapps\workshop\content\4000`.

## What's here

```
particles/filmmaker/modern/*.pcf   the 3 ARC9/MW2019 particle libraries
materials/**                        the VMT/VTF closure those PCFs reference
modern-missing-materials.txt        sprites that exist in NO local source (pruned at convert)
```

The 3 PCFs and ~100 material files (≈44 MB) are everything the converter needs. The raw
GMod GMAs they came from are ~2.9 GB and are **not** committed.

## Provenance / how to regenerate

Extracted from these Steam Workshop items (GMod, appid 4000) by
[`extract-modern-particles-gmod.py`](../../tools/extract-modern-particles-gmod.py):

| Workshop ID | Addon | Provides |
|---|---|---|
| `2910505837` | ARC9 Weapon Base | `arc9_fas_muzzleflashes.pcf`, `arc9_fas_explosions.pcf` + most materials |
| `3258297368` | [ARC9] Modern Warfare 2019 | `mw2019_tracer.pcf` |
| `2459720887` | Modern Wokefare Base | a few shared particle sprites |

To refresh this tree from an updated GMod install:

The simplest way to refresh from an updated GMod install is to let the converter do it:

```powershell
powershell -File fx/tools/convert-povarehok-source1.ps1 -RefreshModernFromGmod -Compile
```

That re-extracts straight back into this folder before compiling. To only re-extract
(no compile), call the extractor directly:

```powershell
python fx/tools/extract-modern-particles-gmod.py `
  --workshop-root "G:\SteamLibrary\steamapps\workshop\content\4000" `
  --gmod-root     "G:\SteamLibrary\steamapps\common\GarrysMod" `
  --output        "fx/sources/modern-warfare-gmod"
```

The FX converter (`fx/tools/convert-povarehok-source1.ps1`) consumes this folder
automatically on every build — it only reaches into GMod when this committed tree is
missing, or when run with `-RefreshModernFromGmod`.

See [`docs/mw2019-fx-mapping-reference.md`](../../../docs/mw2019-fx-mapping-reference.md)
for how these effects map onto CS2 weapons.
