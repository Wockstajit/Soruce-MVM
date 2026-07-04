# Cosmetics model-override research (knife/agent model swap)

> **ARCHIVED (2026-07-04).** This document's core conclusion — "a true model swap is not
> available from client.dll alone in CS2" — was **overturned**: knife-type, glove, and agent
> model swaps now work client-side via `C_BaseModelEntity::SetModel` (see
> `../cosmetics-cs2-methodology-notes.md` for the working recipes and
> `../cosmetics-overview.md` for the current architecture; the implementation lives in
> `AfxHookSource2/Filmmaker/Cosmetics/CosmeticKnifeSwap.cpp`, `CosmeticGloveSwap.cpp`, and
> `CosmeticModelSwap.cpp`). Kept for the still-valid `CModelState`/schema layout research
> and the nSkinz/Source-1 comparison below.

Status: research only, no code changed. Gates `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp`
milestones 12-13 (gloves apply, agent apply) — see the comment at
`AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp:374-384`, which explicitly defers to this
document before any entity write is added to that block.

## 1. What we already do, and why it does NOT change the model

`Filmmaker::CosmeticOverrideSystem::RunFrame` (`AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp:283-385`)
walks every `C_EconEntity`-derived weapon each main-thread frame, resolves the original owner's XUID
(`m_OriginalOwnerXuidLow/High`, `AfxHookSource2/SchemaSystem.h:94-95`), looks up a `CosmeticProfile` by
SteamID64, and — via `ApplyCosmeticWrite` (`CosmeticOverrideSystem.cpp:57-116`) — writes:

- `C_EconItemView::m_iItemIDHigh = -1` (forces the client to stop trusting the networked econ item and
  use the entity's own fallback fields — this is the same trick nSkinz/Osiris used in CS:GO)
- `C_EconEntity::m_nFallbackPaintKit / m_flFallbackWear / m_nFallbackSeed / m_nFallbackStatTrak`
- optionally `C_EconItemView::m_iItemDefinitionIndex` ("best-effort knife DEF swap", `knifeDefOverride`,
  `CosmeticOverrideSystem.cpp:90,99`)
- clears `m_bInitialized`/`m_bInitializedTags` to force the client to recompute the composited material

**This pipeline only ever rewrites the econ item view that feeds material compositing.** It never touches
the entity's actual `CModelState`/`m_hModel` resource handle. Compositing changes the skin/pattern texture
applied to whatever `.vmdl` is already loaded on the entity — it cannot turn a default-knife model into a
Butterfly Knife model, because those are two different `.vmdl` resources with two different skeletons.

### Does writing `m_iItemDefinitionIndex` alone already get us the right MODEL? — No.

`knifeDefOverride` in `ApplyCosmeticWrite` only patches the **econ item's definition index** (used for HUD
icons, inspect-panel data, and which paint-kit table the composite step looks up). It does not touch any
model-resource field. The defIndex and the model are two independent properties on the entity: the model is
resolved once at entity-creation time (server-side, from `GiveNamedItem`/loadout resolution; see §3) and is
not re-derived from `m_iItemDefinitionIndex` afterward on the client. Confirmed empirically by nSkinz: even
after `definition_index = new_def`, nSkinz has to **separately** call a model-index setter
(`item->SetModelIndex(...)`) — see §2. So: **paint-only is confirmed insufficient; a true model swap is a
separate, additional step**, and per §3 that step is not available from client.dll alone in CS2.

## 2. nSkinz (CS:GO / Source 1) — how it actually swaps the knife model

Source: `namazso/nSkinz`, `src/Hooks/PostDataUpdate.cpp`
(https://github.com/namazso/nSkinz/blob/master/src/Hooks/PostDataUpdate.cpp), MIT licensed, hooked from the
advancedfx fork (https://github.com/advancedfx/nSkinz).

Knife/weapon model swap (`apply_config_on_attributable_item`, lines 49-111):

```cpp
if (config->definition_override_index && config->definition_override_index != definition_index) {
    if (const auto replacement_item = game_data::get_weapon_info(config->definition_override_index)) {
        definition_index = short(config->definition_override_index);
        // Set the weapon model index -- required for paint kits to work on replacement items
        // after the 29/11/2016 update.
        item->SetModelIndex(g_model_info->GetModelIndex(replacement_item->model));
        item->GetClientNetworkable()->PreDataUpdate(0);
        ...
    }
}
```

Viewmodel + world-model sync (`post_data_update_start`, lines 246-269):

```cpp
const auto view_model = get_entity_from_handle<sdk::C_BaseViewModel>(local->GetViewModel());
const auto view_model_weapon = get_entity_from_handle<...>(view_model->GetWeapon());
const auto override_info = game_data::get_weapon_info(view_model_weapon->GetItemDefinitionIndex());
const auto override_model_index = g_model_info->GetModelIndex(override_info->model);
view_model->GetModelIndex() = override_model_index;          // first-person viewmodel
const auto world_model = get_entity_from_handle<...>(view_model_weapon->GetWeaponWorldModel());
world_model->GetModelIndex() = override_model_index + 1;     // third-person world model
```

Key mechanism: `IVModelInfoClient::GetModelIndex(path)` (`VModelInfoClient004` engine interface, declared
in `src/SDK/interfaces.hpp`, used via `extern sdk::IVModelInfoClient* g_model_info;`,
https://github.com/namazso/nSkinz/blob/master/src/SDK/Sequence.cpp#L8-L20) is a **Source-1-engine
function that resolves/precaches a `.mdl` path string to an integer model index** which is then assigned
directly to an `int m_nModelIndex`-style field on the entity (`GetModelIndex()` is a field accessor, not a
method on `item`/`view_model`/`world_model` — those overloads return a reference into the entity).
`PreDataUpdate(0)` forces the client networkable to re-run its data-update path (so dependent state — e.g.
attachment-driven viewmodel pieces — re-resolves against the new model).

**Animation-graph risk, explicitly handled by nSkinz**: a knife swap changes the model's `.mdl` and its
`.mdl`'s embedded sequence table is NOT the same layout as the original knife's. nSkinz ships an entire
parallel hook (`src/Hooks/Sequence.cpp`,
https://github.com/namazso/nSkinz/blob/master/src/Hooks/Sequence.cpp) that intercepts the viewmodel's
animation-sequence RecvProxy and **remaps sequence numbers per target model** (`get_new_animation`,
lines 38-191) — e.g. for `models/weapons/v_knife_butterfly.mdl`, draw/idle/lookat sequence indices are
different from the default knife's, and nSkinz hard-codes per-model remap tables by hashed model path. The
comment at line 34-37 says outright: *"This only fixes if the original knife was a default knife... The
best would be having a function that converts original knife's sequence into some generic enum... I won't
write that."* — i.e. even the reference implementation treats this as a known-incomplete hack, not a
robust solution.

**Source-1/CS:GO-specific, will NOT carry over unchanged to Source 2/CS2:**
- `IVModelInfoClient`/`VModelInfoClient004` is a Source-1 engine interface; CS2 does not expose model
  resolution this way (model loading in Source 2 goes through the resource system —
  `CStrongHandle<InfoForResourceTypeCModel>` — not an integer "model index" looked up by a global table).
- `int m_nModelIndex` as a flat field does not exist on CS2 schema classes (see §3) — Source 2 entities
  hold a `CStrongHandle` resource reference instead.
- `CBaseWeaponWorldModel` (a separate Source-1 GOTV/CS:GO world-model entity class with its own model
  index) has no confirmed Source-2 equivalent in the schema dump checked (§3); in CS2 the weapon entity
  itself appears to be both the "owner" and the rendered world-model object (no distinct world-model
  entity class was found in the generated client.hpp schema, see `C_CSWeaponBase` at client.hpp:2779).
- The sequence-remap hack is tied to legacy `.mdl` sequence tables; Source 2's AnimGraph-driven model state
  (`CModelState`, `CBodyComponentBaseAnimGraph`) is a structurally different animation system (graph nodes
  + decision trees, not a flat numbered sequence array), so this exact remap technique does not translate.

## 3. CS2 (Source 2) schema — current field names

Source: generated schema dump `NotOfficer/cs2-sdk`, `client.hpp`
(https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp — a source2gen-style dump, the
same family of tool as the local `AfxHookSource2/SchemaSystem.cpp` resolver). Local repo has **no**
`CBaseModelEntity`/`CModelState`/`CSkeletonInstance` definitions anywhere under `deps/release/prop/cs2/`
(confirmed via `Glob` over `deps/release/prop/cs2/**/*.h`) — these fields are *not* declared in any header
the DLL compiles against; they only exist at runtime in the CS2 schema system and must be resolved the same
way `AfxHookSource2/SchemaSystem.cpp` already resolves `C_EconEntity`/`C_EconItemView` fields (via
`getOffsetsFromSchemaSystem` walking `client.dll`'s declared classes).

The model-resolution chain in current CS2 (paths/offsets below are from the cs2-sdk dump; offsets are
build-specific and MUST be re-resolved live via the schema system, the same way
`g_clientDllOffsets.C_EconEntity.*` are today — do **not** hardcode the literal hex offsets quoted here):

```
C_BaseEntity                              (client.hpp:589)
  CBodyComponent* m_CBodyComponent;        // 0x38 — NOT a value member, a pointer

CBodyComponentSkeletonInstance            (client.hpp:5624, : CBodyComponent)
  CSkeletonInstance m_skeletonInstance;    // embedded value member
    (network sub-paths, per MNetworkVarNames on the embedded member)
    -> m_modelState           (CModelState, embedded inside CSkeletonInstance)
    -> m_bIsAnimationEnabled
    -> m_materialGroup
    -> m_nHitboxSet

CSkeletonInstance : CGameSceneNode        (client.hpp:4570)
  CModelState m_modelState;                // embedded value member
    -> m_hModel      (offset +0x210 from CSkeletonInstance base in this dump)
    -> m_ModelName   (offset +0x218 from CSkeletonInstance base in this dump)

CModelState                                (client.hpp:1946, standalone layout)
  CStrongHandle<InfoForResourceTypeCModel> m_hModel;  // 0xa0 (MNetworkChangeCallback "skeletonModelChanged")
  CUtlSymbolLarge m_ModelName;                         // 0xa8 (MNetworkDisable)
```

Important nuances:
- `m_CBodyComponent` is declared as a **pointer** (`CBodyComponent*`) on `C_BaseEntity`, but the actual
  runtime object behind it (for a model entity) is a `CBodyComponentSkeletonInstance`/
  `CBodyComponentBaseAnimGraph` — i.e. you must resolve through the pointer, not assume an embedded struct,
  and the concrete subtype varies by entity type (weapons/players use the `BaseAnimGraph` variant; static
  props use `CBodyComponentBaseModelEntity`).
- `m_hModel` carries `MNetworkChangeCallback "skeletonModelChanged"` — this is the schema system's
  annotation that writing this field server-side fires a network change callback that drives the actual
  resource (re)load on the client. It says nothing about what happens if a client manually overwrites the
  resolved client-side value: there is no guarantee the callback machinery runs, and a `CStrongHandle` is a
  refcounted resource reference (`InfoForResourceTypeCModel`), not a plain index — overwriting it with raw
  memory writes risks an invalid/dangling refcounted handle (use-after-free or resource-system corruption)
  rather than a clean swap, because the resource system tracks load/unload through that handle's own
  refcounting, which a raw write bypasses entirely.
- `m_ModelName` is `MNetworkDisable` (not networked at all) — it is a local-only cache, **not** a live
  input the client uses to (re)resolve the model on a plain write; writing it alone almost certainly does
  nothing visually because nothing re-reads it to trigger a reload.
- No flat `int m_nModelIndex` exists anywhere in the dump. CS2 replaced the CS:GO/Source-1 "global model
  index table" with per-entity `CStrongHandle` resource references.
- `C_ViewmodelWeapon::m_worldModel` (`client.hpp:12434-12438`) is a raw `char*` at offset `0xed0` — not
  marked `MNetworkEnable`/networked, and a bare pointer (not a `CUtlSymbolLarge` or `CStrongHandle`); this
  looks like an internal cached display-name pointer rather than something safe to repoint.
- `C_BaseViewModel` (`client.hpp:4023-4056`) has `m_hWeapon` (`CHandle<C_BasePlayerWeapon>`) and
  `m_hWeaponModel` (`CHandle<C_ViewmodelWeapon>`) — the viewmodel entity references a *weapon entity* and
  a *separate viewmodel-weapon entity*, but **neither field is a model resource itself**; the model lives
  on whichever entity the handle resolves to, via that entity's own `CBodyComponent` chain above.
- `C_CSWeaponBase` (`client.hpp:2779`) carries `m_seqIdle`/`m_seqFirePrimary`/`m_seqFireSecondary`/
  `m_thirdPersonFireSequences[]` (`HSequence` typed, `client.hpp:2793-2802`) — confirming CS2 still has a
  sequence concept per weapon, but driven through the modern `HSequence`/AnimGraph system, not the legacy
  flat int array nSkinz patches around in Sequence.cpp. This is the strongest evidence that a knife-model
  swap on Source 2 carries the *same class of* animation risk nSkinz hit on Source 1 (mismatched
  draw/idle/attack sequences between source and target knife "modelling"), but the exact remediation
  mechanism is unconfirmed (no public CS2-specific reference implementation found, see §4/§5).
- `C_CSPlayerPawn`/`C_CSPlayerPawnBase` (`client.hpp:5893-7300+`) expose no agent/player-model field at
  all — no `m_AgentSkin`, no model-path string, nothing. The only econ-shaped field on the pawn itself is
  `C_EconItemView m_EconGloves` (`client.hpp:7286`, matches local `g_clientDllOffsets.C_CSPlayerPawn.m_EconGloves`
  already resolved in `AfxHookSource2/SchemaSystem.h:130-132`). The agent/player body model is resolved
  server-side at pawn spawn from the player's loadout and is not exposed as a "current model" field the
  client can independently flip — consistent with the SetModel-is-server-only finding in §4.

`C_BaseModelEntity` itself (`client.hpp:1070-1110`) confirms the render-relevant fields sit behind
`m_CRenderComponent` (a *different* pointer from `m_CBodyComponent`, handles render-state like
`m_clrRender`/`m_nRenderMode`, not the model resource) plus `m_CHitboxComponent`. The model resource is
exclusively reached through `m_CBodyComponent` → skeleton instance → `m_modelState`, as charted above.

## 4. `SetModel` is a server.dll function — the binding constraint for this DLL

Source: `Salvatore-Als/cs2-signature-list`, `CBaseModelEntity_SetModel.md`
(https://github.com/Salvatore-Als/cs2-signature-list/blob/main/CBaseModelEntity_SetModel.md) — signature
entry: `void CBaseModelEntity::SetModel(CBaseModelEntity* pthis, const char* pszModel)`, **DLL: server**.

Two independent real-world implementations confirm `SetModel`-class model swapping in CS2 is done
**server-side**, never from client.dll:

1. **`yuzhouUvU/cs2_weapons_skin`** (`Skin.cpp`, https://github.com/yuzhouUvU/cs2_weapons_skin/blob/main/Skin.cpp,
   a Metamod:Source server plugin) does NOT call `SetModel` directly for a knife swap either. Its actual
   knife-swap path (`CON_COMMAND_F(skin, ...)`, `Skin.cpp:407-411`) is:
   ```cpp
   pWeaponServices->RemoveWeapon(pPlayerWeapon);
   FnEntityRemove(g_pGameEntitySystem, pPlayerWeapon, nullptr, -1);
   FnGiveNamedItem(pPlayerPawn->m_pItemServices(), weapon_name->second.c_str(), nullptr, nullptr, nullptr, nullptr);
   ```
   — it **destroys the weapon entity and recreates it from scratch** via the item-give system (which
   resolves the correct model/animgraph/sequences as part of normal weapon spawning), rather than mutating
   a model field on a live entity. For the specific default-knife-to-skinned-knife case it instead drives
   the engine's own subclass-change console command:
   ```cpp
   sprintf(buf, "i_subclass_change %d %d", knife_idx->second, index);
   engine->ServerCommand(buf);
   ```
   i.e. even this implementation defers to an internal engine command (the same mechanism CS2's own
   loadout system uses for "preferred knife by side") rather than touching model fields by hand. Both paths
   are **server-authoritative**: `RemoveWeapon`/`FnEntityRemove`/`FnGiveNamedItem` and
   `i_subclass_change`/`ServerCommand` only exist/are valid in a live game-server context (Metamod hook
   into `server.dll` with a real `IVEngineServer2`), not in offline demo *playback* where there is no
   server simulating the world — the client is replaying networked snapshots/deltas that were already
   baked when the demo was recorded.

2. **`samyycX/CS2-PlayerModelChanger`** (https://github.com/samyycX/CS2-PlayerModelChanger) — a
   CounterStrikeSharp (C#, server-side managed plugin framework) addon specifically for player/agent model
   changing. CounterStrikeSharp's managed `SetModel`-equivalent API is itself a thin wrapper that calls
   into the native `server.dll` function from the signature list above. Confirms the *only* known
   reference implementation for agent-model changing in CS2 is server-side.

**Conclusion: there is no confirmed public technique for swapping a rendered model purely from client.dll
in CS2**, which is the only DLL this tool can hook (per `AfxHookSource2/` being the entire scope, and the
explicit "offline demo/movie work only" constraint in `CLAUDE.md` — there is no live server to call into
during demo playback; `SetModel`/`GiveNamedItem`/`i_subclass_change` are server.dll/server-console-only and
not reachable). This is a materially different situation from the existing paint-kit override, which works
specifically *because* `C_EconItemView`/`C_EconEntity` fallback compositing was deliberately designed as a
**client-side rendering fallback** (for cases like missing/stale networked econ data) — there is no
equivalent client-rendering-only fallback for the model resource itself.

## 5. What IS achievable now vs. genuinely uncertain

### Achievable now (low risk, consistent with existing architecture)
- **Nothing new on the rendering side.** The current paint/skin/defIndex-numeric override
  (`Cosmetics_RunFrame`) is already the ceiling of what a client-only, schema-field-write approach can
  safely do. No code change is needed to keep this working as-is.
- **UI/storage**: gloves and agent profile fields can keep being entered/saved/displayed as "stored, not
  yet applied" (already implemented — `CosmeticDebug.cpp:141-149`, `CosmeticProfile.cpp:15,121,179`).
  Nothing to change here; this doc does not unblock applying them.
- **Diagnostics**: extending `Cosmetics_DebugWeapon`-style introspection (`MirvCosmetics.cpp:149-168`) to
  also dump the resolved `CBodyComponent` → `CSkeletonInstance` → `m_modelState.m_ModelName` chain
  (read-only) is low-risk and would let us confirm offset resolution and current model path live, without
  writing anything. This is a reasonable, contained next step purely for instrumentation.

### Genuinely uncertain / needs in-game experimentation before any write
- **Whether a raw client-side overwrite of `CModelState::m_hModel`/`m_ModelName` does anything at all.**
  Because `m_hModel` is a refcounted `CStrongHandle`, the realistic outcomes of a naive overwrite are: (a)
  no visible change because nothing re-triggers a load from the new value, (b) a crash/resource-system
  corruption from bypassing the handle's own ref-counting, or (c) — best case — the engine's render path
  re-derives the visible mesh from `m_hModel` every frame and a correctly-acquired strong handle to the
  target `.vmdl`'s resource would render. Outcome (c) requires calling into the actual Source 2 resource
  system to acquire a new strong handle for the desired `.vmdl` (likely via the resource manager
  interface, not a raw memory write) — no public reference for doing this from client.dll was found.
- **Animation/skeleton compatibility.** Per §3's `HSequence`/AnimGraph evidence, a knife model swap most
  likely still needs a CS2-side equivalent of nSkinz's sequence remap (or, more likely in Source 2, an
  AnimGraph/skeleton-compatible replacement model), and no CS2-specific reference for this was found
  anywhere in this research pass. This is the single biggest unknown and the most likely source of visible
  breakage (T-pose, snapped bones, or silently-ignored swap) if attempted naively.
  **For the agent (player pawn) case this risk is categorically worse**: a player model swap also changes
  hitboxes (`CSkeletonInstance::m_nHitboxSet`, `client.hpp:4602`), jiggle bones, gameplay-relevant
  attachment points (weapon-hand attachment, viewmodel parenting), and ragdoll setup — all of which are
  baked into the specific skeleton/animgraph asset and are not interchangeable across arbitrary `.vmdl`s
  the way a paint kit composite is. Since this tool is movie-making/recording rather than live gameplay,
  hitboxes/gameplay don't matter, but skeleton/animgraph mismatch (visual T-posing or limb snapping) still
  would.
- **Whether the demo-playback client even owns enough of the resource pipeline to load an arbitrary
  `.vmdl` not already referenced by the demo.** Live gameplay precaches models server-side at map load;
  during demo *playback* the set of resources the client has prepared to render may be constrained to what
  the recorded demo actually referenced. This needs an in-game experiment (try resolving/precaching an
  unrelated `.vmdl` while a demo plays) before any implementation attempt — not confirmed either way by
  this research pass.
- **No CS2-specific public reference implementation for a true client-only model swap was found** despite
  searching specifically for "spectator/demo/clientside model change" CS2 techniques (see §4) — this
  appears to be a genuinely unexplored/undocumented area, not just one this research missed an obvious
  source for.

## 6. Minimal next-step implementation plan

### Milestone A: "knife defIndex + model override" (research/instrumentation only — no apply yet)
1. Extend `AfxHookSource2/SchemaSystem.h`/`.cpp`'s non-fatal cosmetics-offset block
   (`initCosmeticsOffsets`, `SchemaSystem.cpp:141-167`) to additionally resolve, read-only and
   best-effort (mirroring the `g_cosmeticsOffsetsOk` non-fatal pattern):
   - `C_BaseEntity::m_CBodyComponent` (already implicitly available — it is the same `m_pGameSceneNode`
     pattern already resolved at `SchemaSystem.h:26`/`SchemaSystem.cpp:105`, just a different pointer field
     on the same class)
   - `CBodyComponentSkeletonInstance::m_skeletonInstance` (embedded — need its offset to add to the
     `m_CBodyComponent` pointer's target)
   - `CSkeletonInstance::m_modelState` (embedded within the skeleton instance)
   - `CModelState::m_hModel` and `CModelState::m_ModelName` offsets
2. Add a **read-only** diagnostic (new `Cosmetics_DebugModel(int pawnOrWeaponEntityIndex, ...)`, modeled
   on the existing `Cosmetics_DebugWeapon` in `MirvCosmetics.cpp:149-168`) that walks this chain and prints
   the live `m_ModelName` (`CUtlSymbolLarge`, safe to read as a string) for a target entity. Wire it into
   `mirv_filmmaker cosmetics debug` alongside the existing weapon debug output
   (`CosmeticDebug.cpp`/`Cosmetics_PrintSpectatedDebug`).
3. With that diagnostic running, **in-game**, confirm: (a) the offsets resolve at all on the current CS2
   build, (b) the printed model path matches the actually-equipped knife visually, (c) what the model path
   string looks like for a default knife vs. a real Butterfly Knife on a teammate/bot if one can be
   arranged in a test demo (so we know the exact target string, e.g. something under
   `models/weapons/v_knife_butterfly*.vmdl` analogous to the CS:GO path nSkinz used, but CS2's actual path
   needs confirming — do not assume the CS:GO path is unchanged).
4. Only after (3) is confirmed working read-only: attempt a **viewmodel-only, single-player-pawn,
   easily-revertible** experiment — try acquiring/assigning a `CStrongHandle` for a known-good alternate
   `.vmdl` (starting with something structurally similar, e.g. swapping between two default knives of the
   same skeleton family, NOT immediately attempting butterfly-knife-from-default) and observe whether it
   renders, T-poses, or crashes. This experiment determines whether Milestone A is viable at all before any
   further design work.
5. Do not touch `m_iItemDefinitionIndex` writes for "model swap" purposes beyond what `ApplyCosmeticWrite`
   already does (cosmetic/icon purposes only) until step 4 produces a working mechanism — the defIndex
   write alone, confirmed in §1, does not change the model.

### Milestone B: "agent model override"
Strictly gated on Milestone A succeeding for *any* model swap (knife or otherwise) first, since agent
swap is the harder case (full skeleton + hitbox + attachment set, per §5):
1. Repeat the read-only diagnostic from Milestone A step 2-3 against a `C_CSPlayerPawn`'s body component
   chain (same `m_CBodyComponent` → skeleton instance → `m_modelState` path; pawns and weapons are both
   `C_BaseModelEntity`-derived so the chain is identical, only the starting entity differs) to confirm the
   live agent model path matches the catalog values already in `AfxHookSource2/Filmmaker/Data/cosmetics.json`
   (e.g. `agents/models/ctm_fbi/ctm_fbi.vmdl`).
2. If Milestone A's swap mechanism works for weapons, attempt the same mechanism on a pawn between two
   agent models that are known to share a compatible skeleton (Valve's CT/T agent models are generally
   built on a small number of shared base skeletons) before attempting arbitrary cross-skeleton swaps.
3. Only then update `CosmeticOverrideSystem::RunFrame`'s gloves/agent block (currently an intentional
   no-op, `CosmeticOverrideSystem.cpp:374-384`) to actually apply `prof->agent`/`prof->gloves`, following
   the same XUID-keyed, SEH-guarded, non-fatal-on-missing-offset pattern already used for weapons in that
   file.

### What this doc does NOT resolve
This research did not find a confirmed, safe, client-only mechanism to perform step 4 of Milestone A. The
honest state is: **paint/skin/defIndex overrides are done and correct; true model swap (knife or agent) is
an open R&D problem for this client-only, offline-demo-playback architecture**, with the resource-handle
acquisition path and animation/skeleton compatibility being the two concrete unknowns to resolve before any
production code is written.

## Sources

- Local: `AfxHookSource2/SchemaSystem.h`, `AfxHookSource2/SchemaSystem.cpp`,
  `AfxHookSource2/ClientEntitySystem.h`, `AfxHookSource2/ViewModel.cpp`,
  `AfxHookSource2/Filmmaker/Movie/MirvCosmetics.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticProfile.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticCatalog.{h,cpp}`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticDebug.cpp`,
  `AfxHookSource2/Filmmaker/Cosmetics/CosmeticCommands.cpp`,
  `AfxHookSource2/Filmmaker/Data/cosmetics.json`
- nSkinz (advancedfx fork, MIT): https://github.com/advancedfx/nskinz ;
  upstream source used for code excerpts: https://github.com/namazso/nSkinz/blob/master/src/Hooks/PostDataUpdate.cpp ,
  https://github.com/namazso/nSkinz/blob/master/src/Hooks/Sequence.cpp ,
  https://github.com/namazso/nSkinz/blob/master/src/SDK/interfaces.hpp
- CS2 generated schema dump: https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp
  (source2gen-style dump; see also https://github.com/NotOfficer/cs2-sdk)
- CS2 offsets project (cross-reference, not separately quoted above):
  https://github.com/sezzyaep/CS2-OFFSETS
- `CBaseModelEntity::SetModel` signature (server.dll):
  https://github.com/Salvatore-Als/cs2-signature-list/blob/main/CBaseModelEntity_SetModel.md
- Server-side knife/skin plugin (Metamod:Source, GPL):
  https://github.com/yuzhouUvU/cs2_weapons_skin/blob/main/Skin.cpp
- Server-side player-model plugin (CounterStrikeSharp, GPL-3.0):
  https://github.com/samyycX/CS2-PlayerModelChanger
- CS2 entity list reference: https://cs2.poggu.me/dumped-data/entity-list/

## Later reference comparison update (2026-06-29)

This older research doc is partly superseded by `docs/cosmetics-cs2-methodology-notes.md`, especially
section 9. The still-valid conclusion is that writing `m_iItemDefinitionIndex` alone does not change the
rendered model. The obsolete conclusion is that CS2 model swaps are not available from `client.dll`:
all three local references use or corroborate `C_BaseModelEntity::SetModel(const char*)`,
`CGameSceneNode::SetMeshGroupMask`, `C_CSWeaponBase::UpdateSubclass`, and
`CGameSceneNode::PostDataUpdate` from `client.dll`.

For demo playback the remaining model-swap risk is lifecycle timing, not the existence of the function:
seek/POV/deploy can recreate entities and viewmodels while the demo packet stream is rebuilding them.
The correct implementation shape is conservative reapply: target by owner SteamID, operate only on the
current active weapon for first-person viewmodel writes, wait for a post-seek stability window before
knife type swaps, and keep a user-visible kill switch for knife model swaps.

The deeper `real-time-internal-overlay-research-main` pass adds one escalation path if conservative
reapply still shows a default knife for a frame. That repo hooks `C_BaseModelEntity::SetModel` in
`src/game/game_hooks.cpp`, and `src/game/skins/skin_changer.cpp::OnSetModel` rewrites the incoming
viewmodel knife model argument before the original engine call runs. SOURCE:MVM should keep the current
after-the-fact reassert path first, but a guarded demo-only `SetModel` hook is the reference-backed fix
to test if demo seeking, POV switches, or deploy animations redraw the default model before our frame
loop can repair it.
