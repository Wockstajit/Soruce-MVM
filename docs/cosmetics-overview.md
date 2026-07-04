# Cosmetics system — overview

The offline demo **cosmetics override** family: per-player weapon skins, knife type/skin,
gloves, and agent (player model) overrides applied to a CS2 demo replay, driven from the
Camera Editor's **Customize** modal or the `mirv_filmmaker cosmetics ...` console surface.

This is the executive summary + file map. Deep dives:

| Topic | Doc |
|---|---|
| Working recipes, AOBs, engine mechanisms (the "how") | [cosmetics-cs2-methodology-notes.md](cosmetics-cs2-methodology-notes.md) |
| Customize modal UI + phased feature handoff | [customize-feature-handoff.md](customize-feature-handoff.md) |
| Why only some weapon skins render (legacy vs CS2 mesh) | [cosmetics-legacy-vs-cs2-models.md](cosmetics-legacy-vs-cs2-models.md) |
| Historical research (superseded conclusions, kept for layout notes) | [archive/cosmetics-model-override-research.md](archive/cosmetics-model-override-research.md), [archive/cosmetics-recompose-research.md](archive/cosmetics-recompose-research.md) |

## What it does

- **Weapon skins** — rewrites the econ item view (fallback paint kit / wear / seed /
  StatTrak, `m_iItemIDHigh = -1`) on every weapon owned by a profiled player, then fires
  the engine's own composite-material rebuild so the skin actually renders. Confirmed
  rendering live (the 2026-06-29 breakthrough).
- **Knife** — paint via the same composite path; knife **type** swap via a client-side
  `SetModel` sequence (viewmodel + world model), crash-protected by a targeted anim-builder
  detour.
- **Gloves** — econ data writes into the pawn's embedded `m_EconGloves` persist; rendering
  on a spectated remote demo pawn is the open half (see [memory: glove-render-worldmodelgloves]).
- **Agent** — full player-model swap via `SetModel`.

Profiles are keyed by **SteamID64**, not entity index, so an override follows the player
across pawn recreation, round restarts, death, observer switches, and demo seeks.
**Offline demo playback only** — never used against live servers.

## Architecture (three layers)

1. **Profile store + apply loop** — `CosmeticOverrideSystem` walks the client entity list
   once per main-thread frame, matches econ entities to profiles by original-owner XUID,
   and applies the data writes + composite refresh.
2. **Model swaps** — the `CosmeticModelSwap` subsystem wraps the client.dll functions
   (`SetModel`, `SetMeshGroupMask`, …) resolved by byte-pattern scan; every call is
   SEH-guarded so a moved pattern degrades to a no-op instead of crashing.
3. **UI** — the Camera Editor's Customize modal (Panorama; see
   `Panorama/CameraEditorCustomizeJs.h`) issues the same `mirv_filmmaker cosmetics ...`
   commands the console does.

## File map (`AfxHookSource2/Filmmaker/Cosmetics/`)

One responsibility per file. Public headers are the `*.h` without "Internal" in the name.

### Core apply system
| File | Responsibility |
|---|---|
| `CosmeticOverrideSystem.{h,cpp}` | The per-frame apply loop: entity walk, XUID→profile match, fallback/attribute writes, composite refresh gating. The public API everything else drives. |
| `CosmeticProfile.{h,cpp}` | The SteamID64-keyed profile store (one player's weapon/knife/glove/agent choices). |
| `CosmeticDemoSync.cpp` | Demo-seek detection + the post-apply "tick nudge" (briefly resume/re-pause so a change made while paused re-renders) and the seek settle window. |
| `CosmeticPaintKitBridge.cpp` | The `cl_paintkit_override` cvar read/write bridge (global, deploy-time; the experimental alternative path). |
| `CosmeticDirectComposite.{h,cpp}` | The resolved Andromeda composite-rebuild calls (`composite once` — the essential render lever). |

### Model-swap subsystem (shared internal surface: `CosmeticModelSwapInternal.h`)
| File | Responsibility |
|---|---|
| `CosmeticModelSwap.{h,cpp}` | Public API + core: HUD-arms/viewmodel resolution and mirroring, weapon mesh-mask (legacy vs CS2 model), agent swap. |
| `CosmeticFnResolver.cpp` | client.dll byte-pattern resolution into the shared `Fns` table + SEH-guarded wrappers + econ-schema lookups. |
| `CosmeticKnifeSwap.cpp` | Knife-type swap: def→world-model fallback table + the full precache→SetModel→mesh-mask→viewmodel-mirror sequence. |
| `CosmeticGloveSwap.cpp` | Glove econ writes + body-group / gloves-changed rebuild. |
| `CosmeticAnimFix.{h,cpp}` | The knife-swap crash fix: client.dll anim-builder detour substituting an empty sequence list for a null out-param. |

### Catalog / classification data
| File | Responsibility |
|---|---|
| `CosmeticCatalog.{h,cpp}` | Item-definition classifier (which slot a def index belongs to) for the native backend. |
| `CosmeticGloveSkinTable.cpp` | Generated glove paint→def table (`misc/gen_glove_skin_table.py`; do not hand-edit). |
| `CosmeticGloveLabels.{h,cpp}` | Glove display-name tables. |
| `../Panorama/CameraEditorCosmeticsCatalog.inc` | The generated user-facing skin catalog compiled into the UI (not read at runtime by the backend). |
| `../Data/cosmetics.json` | Source data for the generated catalog. |

### Command / UI / diagnostics
| File | Responsibility |
|---|---|
| `CosmeticCommands.cpp` | The `mirv_filmmaker cosmetics <subcommand>` console grammar. |
| `CosmeticUiQueries.{h,cpp}` | Customize-modal queries: live weapon/glove/agent econ reads for the spectated pawn, surfaced to Panorama as JSON. |
| `CosmeticDebug.cpp` | Read-only diagnostics printing (`cosmetics status`, spectated econ dump, visual diag). |
| `CosmeticDebugLog.{h,cpp}` | The `mvm_debug` flight-recorder log (grew beyond cosmetics; covers the whole filmmaker surface). |

## Demo-playback caveats

- A **demo seek / round change / weapon redeploy recreates entities** from the authoritative
  demo data, so overrides are re-applied per-frame and the composite is re-fired
  change-gated (`autocomposite`, default on).
- A change made while **paused** doesn't re-render on its own — `CosmeticDemoSync`'s tick
  nudge handles it.
- Whether a weapon skin renders at all can depend on the **legacy vs CS2 mesh group**
  matching the paint kit — see [cosmetics-legacy-vs-cs2-models.md](cosmetics-legacy-vs-cs2-models.md).
