# Insurgency Sandstorm — smoke source exports

Particle smoke/dust/muzzle/shell textures extracted from a local **Insurgency: Sandstorm**
install (`insurgency2` on Steam). The FX converter can stage these into
`materials/particle/insurgency/` for `patch_non_fire_vistasmoke_replacements` in
`fx/tools/postprocess_povarehok.py`.

## What's here

```
materials/...          VTF exports as TGAs (+ .mks for flipbooks)
picks.json             auto-selected one asset per bucket (impact/muzzle/shell/thin)
```

Browse the full catalog (PNGs + manifest) under
`automation/output/insurgency-smoke-catalog/` after running the extractor.

## Re-export

```powershell
python fx/tools/extract-insurgency-smoke.py `
  --insurgency-root "F:\SteamLibrary\steamapps\common\insurgency2"
```

## Bucket meanings

| Bucket | Used for |
|---|---|
| `impact_dust` | Wall/concrete bullet puff (primary for `impact_concrete_child_smoke`) |
| `thin_smoke` | Dirt/generic/screen impact wisps (`vistasmokev1_emods` replacements) |
| `muzzle` | `weapon_muzzle_flash_smoke_small*` wisps |
| `shell_eject` | `weapon_shell_eject_smoke_*` brass puff |
| `heavy_smoke` | Catalog only — grenades/explosions keep pack vistasmoke |

## Build integration

Pass `-IncludeInsurgencySmoke` to `fx/tools/convert-povarehok-source1.ps1` (or set
`INSURGENCY_DIR` if the game is not at the default Steam path). Export/staging is
non-fatal: if the install is missing, post-process falls back to in-pack
`insandstorm_t_thinsmoke_*`, `smoke1`, and `sq_fulldustfront1_2`.
