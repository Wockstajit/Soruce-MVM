# Cosmetics CS2 methodology notes — knife / glove / agent model swap (from working source)

Status: original methodology extracted from two local CS2 client-DLL inventory changers, now extended with
the third local overlay/inventory reference reviewed in section 9. The older two-source notes below remain
useful for function names and call order, but section 9 is the current demo-playback comparison.

> **Current implementation file map (2026-07-04 refactor).** The Cosmetics code was split into
> one-responsibility translation units; references below to `CosmeticOverrideSystem.cpp` /
> `CosmeticModelSwap.cpp` may now live in a focused TU (see
> [cosmetics-overview.md](cosmetics-overview.md) for the full map):
> - AOB/vtable resolution + the SEH wrappers (`SafePostDataUpdate`, `SafePrecacheModel`, `GetStaticData`
>   lookups) → `CosmeticFnResolver.cpp` (shared `Fns` table in `CosmeticModelSwapInternal.h`)
> - knife-type swap (`ApplyKnifeModelSwap`, the `knife.swap`/`knife.vm` breadcrumbs) → `CosmeticKnifeSwap.cpp`
> - gloves (`SafeApplyGloves`) → `CosmeticGloveSwap.cpp`
> - seek detection + tick nudge (`MaybeFireTickNudge`, `OnDemoSeekDetected`) → `CosmeticDemoSync.cpp`
> - `cl_paintkit_override` bridge → `CosmeticPaintKitBridge.cpp`
> - direct composite (`ResolveDirectCompositeFns`, `FireDirectCompositeRefresh`) → `CosmeticDirectComposite.cpp`
> - `CosmeticModelSwap.cpp` keeps the HUD-arms/viewmodel machinery + weapon mesh-mask + agent swap;
>   `CosmeticOverrideSystem.cpp` keeps the profile match + apply loop.

> **Headline finding that overturns prior research.** `docs/archive/cosmetics-model-override-research.md` §4
> concluded "**`SetModel` is a server.dll function — the binding constraint**" and that "there is no
> confirmed public technique for swapping a rendered model purely from client.dll in CS2." **That is
> wrong.** Both supplied cheats resolve **`C_BaseModelEntity::SetModel(const char*)` as a normal
> `client.dll` function by AOB** and call it every frame to change knife and agent models. The signature
> is identical in both code bases (see the table below). The model swap is a client-side call, not a
> server call. This is the missing lever for the knife-TYPE-swap and agent rows that the recompose doc
> marked "Does NOT work."

These two tools operate on the **local player's own loadout** in live/offline play (they read the real
`CCSPlayerInventory` and copy item identity into the live weapon entities). The *rendering mechanism* they
use is fully client-side and server-independent — that is the part that ports. The demo-playback
adaptation (apply to a **spectated** pawn/weapon instead of the local pawn) and its caveats are in the
last section.

---

## 1. Two sources, what each covers

| | Andromeda-CS2-Base-master | nerv |
|---|---|---|
| Knife model+skin swap | ✅ `CInventoryChanger::OnFrameStageNotify` | ✅ `c_skin_changer::process_knife` |
| Weapon skin (paint) | ✅ via `SetAttributeValueByName` + fallback fields | ✅ via hand-built `m_AttributeList` |
| Gloves | ✅ `CInventoryChanger::SetGlove` | ✅ `c_glove_changer::run` |
| **Agent / player model** | ✅ `CInventoryChanger::SetAgent` | ❌ (has `is_agent()` helper, no feature) |
| Function resolution style | central `CFunctionList` (AOB table) | inline `g_opcodes->scan(...)` per call site |
| Frame hook | `FrameStageNotify`, `FrameStage == 6` | `FrameStageNotify`, `stage == 7` |

Source files (read this session):
- Andromeda: `AndromedaClient/Features/CInventoryChanger/CInventoryChanger.cpp`,
  `CS2/SDK/Types/CEntityData.{hpp,cpp}`, `CS2/SDK/CFunctionList.{hpp,cpp}`,
  `CS2/SDK/FunctionListSDK.hpp`, `CS2/SDK/Update/Offsets.hpp`,
  `CS2/SDK/Econ/CEconItemDefinition.{hpp,cpp}`.
- nerv: `features/skin_changer/skin_changer.cpp`, `features/glove_changer/glove_changer.cpp`,
  `features/shared/econ_item_attribute_manager.cpp`, `features/shared/item_schema.cpp`,
  `valve/classes/c_cs_player_pawn.hpp`.
- real-time-internal-overlay-research-main: `src/game/skins/skin_changer.cpp`,
  `src/game/game_hooks.cpp`, `src/game/menu/menu.cpp`,
  `src/sdk/source2-sdk/econ/ceconitem.{hpp,cpp}`,
  `src/sdk/source2-sdk/entity/c_econitemview.hpp`,
  `src/sdk/source2-sdk/entity/c_csweaponbase.{hpp,cpp}`.

The references agree on the client-side rendering mechanisms below. Where one repo adds an extra
mechanism, it is called out explicitly.

---

## 2. The knife model + skin swap (client-only)

This is the recipe the repo's docs called the "deferred model-swap path." It is a per-weapon-entity
sequence run each frame for the held knife.

### Call order (Andromeda `OnFrameStageNotify`, lines 184-220; nerv `process_knife`, lines 99-161)

1. **Point the econ item view at the desired knife identity.**
   - `item->m_iItemDefinitionIndex() = selectedKnifeDef;` (e.g. 500 = Bayonet)
   - `item->m_iEntityQuality() = QUALITY_UNUSUAL;` (nerv; 3)
   - Andromeda copies the whole identity from the loadout item view: `m_iItemID`, `m_iItemIDHigh`,
     `m_iItemIDLow`, `m_iAccountID`, and sets `m_bDisallowSOC=false`,
     `m_bRestoreCustomMaterialAfterPrecache=true`.

2. **Swap the actual model resource — the new lever.** Call the client `SetModel` on BOTH the
   world/weapon entity **and** the first-person viewmodel knife entity:
   ```cpp
   weapon->SetModel( knifeDef->m_pszModelName() );          // e.g. "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl"
   if (auto* hud = GetKnifeViewModel(weapon, pawn))
       hud->SetModel( knifeDef->m_pszModelName() );
   ```
   `m_pszModelName` is `CEconItemDefinition + 0x148` (Andromeda confirms with a live dump:
   def 500 → `weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl`). The viewmodel-knife entity
   is found by walking the HUD-arms scene-node children and matching the one whose owner is the weapon
   (Andromeda `GetKnifeModel`/`GetViewModel`; nerv `get_hud_weapon`).

3. **Fix the mesh group for legacy vs. new models.**
   ```cpp
   weapon->m_pGameSceneNode()->SetMeshGroupMask(meshMask);
   hud->m_pGameSceneNode()->SetMeshGroupMask(meshMask);
   ```
   Mask comes from the paint kit's "uses old/legacy model" flag. **The two code bases disagree on the
   exact value and it is knife-specific:**
   - Andromeda (weapons & knife): `meshMask = 1 + isLegacyModel` → legacy=2, modern=1.
   - nerv weapon skins: `uses_old_model ? 2 : 1` (same as Andromeda).
   - nerv **knife**: `uses_old_model ? 1 : 2` (**inverted** for knives — modern=2).
   Treat this as "try 1, if the mesh is wrong try 2"; the legacy flag is read from the paint kit
   (Andromeda `g_CCPaintKit_IsUseLegacyModel = 0xAE`; nerv `pk->uses_old_model()`).

4. **Re-derive the weapon subclass from the new def index.** This makes the weapon adopt the new knife's
   VData/animation set:
   ```cpp
   uint32_t hash = CUtlStringToken(std::to_string(knifeDef).c_str()).GetHashCode(); // murmur2, seed 0x31415926, lowercased
   weapon->m_nSubclassID().SetHashCode(hash);
   weapon->UpdateSubclass();
   ```
   nerv does the same with a hand-rolled `string_token_hash` (murmur2, seed `0x31415926`, input
   lowercased) → writes `m_nSubclassID` → calls `UpdateSubclass()`. **This is the CS2 `CUtlStringToken`
   hash and the repo will need it** (see `c_cs_player_pawn.hpp:11-53` for the reference murmur2).

5. **Rebuild the composite material (the skin) — already partly solved in this repo.**
   ```cpp
   weapon->UpdateCompositeMaterial( (CCompositeMaterialOwner*)((PBYTE)weapon + 0x608) );
   weapon->UpdateCompositeMaterialSet();
   weapon->UpdateSkin();          // nerv: vtable[110](force=true)
   weapon->m_pGameSceneNode()->PostDataUpdate();   // vtable call, args (0,0)
   ```
   `0x608` (`g_CompositeMaterialOffset`) is the `CCompositeMaterialOwner` embedded in the weapon. nerv
   additionally calls `update_weapon_data()` (vtable[195]) and a global `regenerate_skins()`.

6. **Clear the stale HUD weapon icon** so the selection panel redraws with the new knife
   (`CCSGO_HudWeaponSelection::ClearHudWeaponIcon`; both set `item->pCEconItemDescription()/m_name_description_ptr() = 0`).

### Paint attributes (two interchangeable ways to attach paint/seed/wear)

- **Andromeda — engine setter + fallback fields:**
  ```cpp
  weapon->m_nFallbackPaintKit() = loadout->GetCustomPaintKitIndex();
  weapon->m_nFallbackSeed()     = seed;
  weapon->m_flFallbackWear()    = wear;
  C_EconItemView_SetAttributeValueByName(item, "set item texture prefab", paintKit);
  C_EconItemView_SetAttributeValueByName(item, "set item texture seed",  (float)seed);
  C_EconItemView_SetAttributeValueByName(item, "set item texture wear",  wear);
  ```
- **nerv — hand-built `m_AttributeList` vector** (no engine call): allocate 3
  `econ_item_attribute_t` via the game allocator and write the vector pointer/size directly
  (`econ_item_attribute_manager::create`). Attribute def indices: **paint=6, pattern/seed=7, wear=8**.
  Struct layout: `def_index` at `+0x30`, `value` (float) at `+0x34`, `init_value` at `+0x38`. Frees the
  previous block via `GameFree` first. This is the cleaner path when there is no real owned item to copy
  from (relevant for demo playback, where the spectated player's networked attrs already exist —
  overwrite def 6/7/8 in place like the repo already does, rather than allocating).

---

## 3. Gloves (client-only)

Gloves are **not a weapon entity** — they live on the pawn as an embedded `C_EconItemView`
(`C_CSPlayerPawn::m_EconGloves`). The repo's apply loop never touches them; this is the missing path.

### Recipe (Andromeda `SetGlove` 339-377; nerv `c_glove_changer::run`)

```cpp
auto& glove = pawn->m_EconGloves();          // embedded C_EconItemView on the pawn
glove.m_iItemDefinitionIndex() = gloveDef;   // e.g. 5027/5030/... (NOT 5028/5029 = default)
glove.m_iItemID()  = loadout->m_iItemID();   // Andromeda copies full identity
glove.m_iItemIDHigh() = ...; glove.m_iItemIDLow() = ...; glove.m_iAccountID() = ...;
glove.m_bDisallowSOC() = false;
glove.m_bRestoreCustomMaterialAfterPrecache() = true;

// attach paint (same two options as §2; nerv builds the attribute list, also sets quality=UNUSUAL)
glove.m_iEntityQuality() = QUALITY_UNUSUAL;   // nerv

glove.m_bInitialized() = true;                // <-- gloves use m_bInitialized = TRUE (note: opposite of the "clear init" intuition)
pawn->SetBodyGroup();                          // C_CSPlayerPawn::SetBodyGroup(this, "first_or_third_person", 1)
C_BaseEntity_UpdateBodyGroupChoice(pawn);      // Andromeda only; nerv relies on SetBodyGroup alone
pawn->m_bNeedToReApplyGloves() = true;         // the engine's own "regenerate gloves next frame" flag
```

### Frame timing matters (both use a multi-frame counter)

Gloves do **not** apply in a single frame. Both run the write for several consecutive frames after a
change:
- Andromeda: on spawn-time change or `m_bApplyGloves`, set `uUpdateFrames = 3`; each of the next 3 frames
  re-asserts `m_bInitialized=true`, `SetBodyGroup`, `UpdateBodyGroupChoice`, `m_bNeedToReApplyGloves=true`.
- nerv: `m_update_frames = 4` on config/team/spawn/engine-reset change; a separate `m_clear_frames = 2`
  on team change first *removes* the attributes and zeroes the def index, then the update frames re-apply.
  Triggers tracked: def-index reset by engine, `!m_initialized`, `m_need_to_reapply_gloves`, spawn-time
  change, team change, buy-menu open.

The engine resets gloves on spawn/round/team change, so the apply must watch
`m_flLastSpawnTimeIndex` / team / `m_bNeedToReApplyGloves` and re-fire.

---

## 4. Agent / player model (Andromeda only — the cleanest swap of the three)

Agents are a **full player-model swap** and Andromeda does it with a **single `SetModel` call on the
pawn** — no body group, no composite, no subclass:

```cpp
auto* loadout = inventory->GetItemInLoadout(pawn->m_iTeamNum(), LOADOUT_SLOT_CLOTHING_CUSTOMPLAYER /*38*/);
auto* def     = loadout->GetStaticData();
const char* modelName = def->m_pszModelName();    // the agent .vmdl path from the item definition
if (modelName && *modelName)
    pawn->SetModel(modelName);                    // same C_BaseModelEntity::SetModel as the knife
```

Guarded by a hash so it only re-sets when the chosen agent changes (`hash_64_fnv1a(modelName)` cached in
a static). Requires `pawn->m_pGameSceneNode()->GetSkeletonInstance()` to be valid first.

`IsAgent` is detected by item type string `#Type_CustomPlayer` (FNV1a-32 hashed); default agents are
def `5036`/`5037` and are skipped (`CEconItemDefinition::IsAgent`). The agent model path comes from the
same `m_pszModelName() @ 0x148` used for weapons/knives.

> Skeleton/animgraph caveat from the prior research still stands in principle (a swapped pawn model must
> be animgraph-compatible), but Andromeda demonstrates that Valve's agent models swap cleanly via a bare
> `SetModel` in practice — they share the player skeleton family. No sequence-remap hack (the nSkinz
> Source-1 problem) is needed for agents here.

---

## 5. Consolidated signature / offset table (current CS2 build, from both sources)

All in `client.dll` unless noted. Andromeda patterns are the `CFunctionList.hpp` entries; nerv patterns
are the inline `g_opcodes->scan` strings. **Where both list a function, the byte pattern is identical** —
strong cross-confirmation.

| Function | AOB pattern | Resolve note |
|---|---|---|
| `C_BaseModelEntity::SetModel(this, const char*)` | `40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24 40` | direct (both identical) |
| `CGameSceneNode::SetMeshGroupMask(this, uint64)` | `48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71` | direct (both identical) |
| `C_CSWeaponBase::UpdateSubclass(this)` | `4C 8B DC 53 48 81 EC ?? ?? ?? ?? 48 8B 41` | direct (both identical) |
| `C_CSWeaponBase::UpdateSkin(this, bool)` | `48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ? ? ? ? F6 C3 01 74 0A 33 D2 48 8B CF E8 ? ? ? ? 48 8D 8F 60 19 00 00` | Andromeda AOB / nerv vtable[110] |
| `C_CSWeaponBase::UpdateCompositeMaterial(owner, bool)` | `E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24` | CALL-relative (Andromeda) |
| `C_CSWeaponBase::UpdateCompositeMaterialSet(this, bool)` | `40 55 53 41 57 48 8D AC 24 00 FE ? ?` | direct (Andromeda) |
| `C_CSPlayerPawn::SetBodyGroup(this, "first_or_third_person", 1)` | `E8 ? ? ? ? EB 0C 48 8B CF` | CALL-relative, +1 (both identical) |
| `C_BaseEntity::UpdateBodyGroupChoice(this)` | `E8 ? ? ? ? 4C 8B AC 24 ? ? ? ? 48 8B BC 24` | CALL-relative (Andromeda) |
| `C_EconItemView::SetAttributeValueByName(view, name, float)` | `E8 ? ? ? ? 66 41 0F 6E D4` | CALL-relative (Andromeda) |
| `C_EconItemView::GetStaticData(view)` | `40 56 48 83 EC ? 48 89 5C 24 ? 48 8B F1 48 8B 1D ? ? ? ? ...` | direct (Andromeda) |
| `C_EconItemView::GetCustomPaintKitIndex(view)` | `48 89 5C 24 ? 57 48 83 EC ? 8B 15 ? ? ? ? 48 8B F9 ...` | direct (Andromeda) |
| `C_EconItemView::construct_paint_kit(view)` | `48 89 5C 24 ? 56 48 83 EC ? 48 8B 01 FF 50` | direct (nerv) |
| `CCSGO_HudWeaponSelection::ClearHudWeaponIcon` | `E8 ? ? ? ? 8B F8 C6 84 24` | CALL-relative (both) |
| `FindHudElement(name)` | `4C 8B DC 53 48 83 EC ? 48 8B 05` (nerv) / `4C 8B DC 53 48 83 EC 50 48 8B 05` (Andromeda) | direct |
| `CCSPlayerInventory::GetItemInLoadout(inv, team, slot)` | `40 55 48 83 EC ? 49 63 E8` | direct (Andromeda) |
| `CCSInventoryManager::EquipItemInLoadout` | `48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 89 54 24 ? 57 41 54 41 55 41 56 41 57 48 83 EC ? 0F B7 FA` | direct (Andromeda) |
| `regenerate_skins()` (global) | `48 83 EC ? E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 10` | direct (nerv) |

Constant offsets:
| Offset | Value | Meaning |
|---|---|---|
| `CCompositeMaterialOwner` in weapon | `+0x608` | `g_CompositeMaterialOffset` (Andromeda) — owner passed to `UpdateCompositeMaterial` |
| `CEconItemDefinition::m_pszModelName` | `+0x148` | the `.vmdl` path for knife/glove/agent/weapon |
| `CEconItemDefinition::LoadoutSlot` getter offset | `0x338` | `g_CEconItemDefinition_GetLoadoutSlot` |
| `CCPaintKit::IsUseLegacyModel` | `+0xAE` | legacy-model flag → mesh group mask |
| Econ attribute def indices | paint=`6`, seed/pattern=`7`, wear=`8` | for hand-built attribute list |
| Loadout slots | melee=`0`, hands(gloves)=`41`, custom-player(agent)=`38`, musickit=`54` | `LoadOutSlot_t` enum (Andromeda hpp) |
| Default items to skip | gloves `5028/5029`, agents `5036/5037` | "default, don't override" |

Schema fields used (resolved via the repo's existing schema system, not hardcoded):
`C_EconItemView`: `m_iItemDefinitionIndex`, `m_iItemID`/`High`/`Low`, `m_iAccountID`, `m_iEntityQuality`,
`m_bInitialized`, `m_bDisallowSOC`, `m_bRestoreCustomMaterialAfterPrecache`, `m_AttributeList`,
`m_szCustomName`. `C_EconEntity`: `m_nFallbackPaintKit/Seed`, `m_flFallbackWear`, `m_nFallbackStatTrak`,
`m_AttributeManager`. `C_BaseEntity`: `m_nSubclassID`, `m_pGameSceneNode`. `C_CSPlayerPawn`:
`m_EconGloves`, `m_bNeedToReApplyGloves`, `m_hHudModelArms`, `m_flLastSpawnTimeIndex`.
`CModelState`: `m_hModel`, `m_ModelName`. `CSkeletonInstance`: `m_modelState`, `m_nHitboxSet`.

---

## 6. How this maps onto THIS repo

What the repo **already has** (per `archive/cosmetics-recompose-research.md`): the composite trio
(`UpdateCompositeMaterial` / `UpdateCompositeMaterialSet` / `UpdateSkin`) resolved and confirmed
rendering weapon **skins** on a spectated weapon via `cosmetics composite once` (the 2026-06-29
breakthrough). So §2 step 5 is done. What these two source bases **add**:

1. **`C_BaseModelEntity::SetModel` (AOB above).** This is the single function the model-override research
   said didn't exist client-side. Add it to the cosmetics function resolver. It is the basis for both the
   knife-TYPE swap and the agent swap.
2. **`CGameSceneNode::SetMeshGroupMask` + `C_CSWeaponBase::UpdateSubclass` + the `CUtlStringToken`
   murmur2 hash.** Needed so a swapped knife renders the right mesh and adopts the right VData. Port the
   murmur2 from `c_cs_player_pawn.hpp:11-53`.
3. **`C_CSPlayerPawn::SetBodyGroup` + `C_BaseEntity::UpdateBodyGroupChoice` + the `m_EconGloves` write +
   `m_bNeedToReApplyGloves` + the multi-frame apply.** This is the entire gloves path, which the repo's
   apply loop currently skips (`archive/cosmetics-recompose-research.md` table: "Gloves … Does NOT work yet").
4. **The agent `SetModel` one-liner** (§4) for the agent row.

Concrete suggested wiring (mirrors the repo's existing `CosmeticOverrideSystem` style):
- Extend the cosmetics function resolver (the same place `ResolveDirectCompositeFns()` lives) with the
  six new AOBs: `SetModel`, `SetMeshGroupMask`, `UpdateSubclass`, `SetBodyGroup`,
  `UpdateBodyGroupChoice`, `ClearHudWeaponIcon`. Use `??` for every wildcard byte (the repo already hit
  the `?`-vs-`??` bug once — see `cosmetics-skin-render-breakthrough`).
- Knife TYPE swap: in the per-entity apply, when the profile requests a knife def ≠ the entity's def,
  run §2 steps 1-6 on that weapon entity + its viewmodel knife entity.
- Gloves: add a pawn-level apply (not in the weapon loop) writing `m_EconGloves` per §3 with a 3-4 frame
  re-assert keyed off `m_flLastSpawnTimeIndex` and `m_bNeedToReApplyGloves`.
- Agent: add a pawn-level `SetModel(agentVmdl)` per §4, hash-gated.

---

## 6b. IMPLEMENTED (2026-06-29): PostDataUpdate refresh + auto play-out nudge

The §6 wiring is now in the repo, and the **renderable-refresh gap** that left model/mesh swaps
written-but-invisible is closed. Two levers, both live-verified:

1. **`CGameSceneNode::PostDataUpdate` (vtable index 22, called `node->vtable[22](node, 0, 0)`).**
   Extracted from Andromeda (`SDK::VMT_Index::CGameSceneNode::PostDataUpdate = 22`,
   `CEntityData.hpp`) — Andromeda fires it after every `SetModel` / `SetMeshGroupMask` / `UpdateSkin`,
   and this repo did **not**. Now wired (SEH-guarded) in
   `AfxHookSource2/Filmmaker/Cosmetics/CosmeticModelSwap.cpp` (`SafePostDataUpdate` + public
   `PostDataUpdate`) and called after the knife model swap, the weapon mesh-mask apply, the glove body
   group, the agent `SetModel`, and (in `CosmeticOverrideSystem.cpp`) after the composite trio in
   `FireDirectCompositeRefresh`. This is what makes the **weapon SKIN re-composite show in place while
   PAUSED** (verified VISIBLE, weapon-crop diff ~2.8 vs noise ~0.2).

2. **Auto play-out "tick nudge"** (`CosmeticOverrideSystem::MaybeFireTickNudge`). PostDataUpdate alone
   does NOT make a third-person **body** swap (agent / gloves / knife-type / legacy-mesh) appear while
   the demo is PAUSED — those re-derive only during LIVE rendered frames. So after a profile change, if
   the demo is paused, the system briefly issues `demo_resume`, lets ~`m_tickNudgeTicks` (default 10)
   ticks render, then re-pauses — literally "let the game play ~10 ticks," done automatically (the
   user's idea; confirmed live as the thing that makes body swaps visible). No-op when already playing;
   debounced so a slider drag coalesces into one nudge. Toggle/tune: `mirv_filmmaker cosmetics
   ticknudge [on|off|<ticks>]`. The agent swap is independently proven by the `m_ModelName` readback
   flipping `agents/.../ctm_st6...vmdl` -> `agents/.../ctm_fbi.vmdl` (authoritative: that is the model
   the renderer draws, regardless of camera framing).

Net: weapon skins/wear/seed apply in place (composite + PostDataUpdate), and agent/gloves/legacy-mesh
swaps are still available through the model-swap path. Knife-type swaps are **default-ON**
(`m_knifeModelSwap = true`) but crash-prone, so they are gated three ways before firing: (a) only on
the owner's ACTIVE (deployed) weapon, never a holstered knife; (b) only on a real value change, never
the periodic composite re-assert; and (c) only after a stable-playback window since the last seek
(`kKnifeSwapStableFrames`, ~64 frames) so rapid stacked scrubs can't fire the SetModel/UpdateSubclass
onto a half-rebuilt entity (the delayed "scrub, knife redeploys, then it crashes" fault, reproduced in
`mvm_debug_20260629_084334.log`). Disable live with `mirv_filmmaker cosmetics modelswap knife 0`. To
VIEW body swaps, use a third-person camera (`mirv_filmmaker follow preset behind` + `follow place`); a
first-person view shows only the viewmodel.

## 7. Demo-playback caveats (the real unknowns for this tool)

Both sources drive the **local player's** entities in a live/offline match. This repo applies to a
**spectated** player during **demo playback**. The rendering calls are generic per-entity `client.dll`
code, so they should work on any weapon/pawn entity — but three things are unverified and must be tested
in-game before trusting them:

1. **Re-application after a networked snapshot.** In a demo, the next delta/full-update can overwrite the
   econ identity and (for `SetModel`) potentially the model handle. Like the existing composite path,
   expect to **re-fire on every relevant frame / after every seek** (the repo already does change-gated
   re-fire for skins; extend the same gating to SetModel/SetBodyGroup). Andromeda/nerv re-assert every
   frame for exactly this reason (the engine resets gloves on spawn; demos reset on seek).
2. **Resource precache.** `SetModel` takes a `.vmdl` path string and the engine resolves it through the
   resource system. During live play the model is precached; during demo playback a knife/agent model
   the demo never referenced may not be loaded. **Test whether `SetModel` to an unreferenced `.vmdl`
   renders or silently no-ops / T-poses.** This is the same open question flagged in
   `archive/cosmetics-model-override-research.md` §5 — these sources don't answer it because their target models
   are always precached for the local player. If it fails, precaching the model first (resource-system
   call) is the next step.
3. **Viewmodel vs. world model entities.** The knife swap must hit both the weapon's world entity and the
   first-person viewmodel knife entity (both sources do this via the HUD-arms scene-node walk,
   `GetKnifeModel`/`get_hud_weapon`). For a spectated player the first-person viewmodel only exists when
   first-person-spectating; in third-person only the world model entity is present. Apply to whatever
   weapon/pawn entities the spectated player actually has.

---

## 8. Reconciliation with the existing two research docs

- `docs/archive/cosmetics-model-override-research.md`: §1 "writing `m_iItemDefinitionIndex` alone does not change
  the model" — **still true**; the model only changes because of the separate `SetModel` call. §4
  "`SetModel` is a server.dll function — the binding constraint" / "no client-only technique exists" —
  **overturned**: `C_BaseModelEntity::SetModel` is a client.dll function, AOB above, called every frame
  by both working tools for knife and agent swaps. The nSkinz Source-1 `IVModelInfoClient`/model-index
  analysis in §2 is moot — CS2 uses a string-path `SetModel`, not an integer model index.
- `docs/archive/cosmetics-recompose-research.md`: the composite trio findings stand and are reused. The recompose
  doc's "Gloves: not a scanned weapon entity, lives on the pawn `m_EconGloves`" diagnosis is **correct**
  and §3 here is the apply path for it. The "Knife — change knife TYPE: does NOT work" and "Agents: does
  NOT work" rows are addressed by §2 (`SetModel`+`UpdateSubclass`+mesh mask) and §4 (`SetModel`)
  respectively.

## Sources (local, user-supplied — read this session)
- `C:\Users\ayden\Downloads\Andromeda-CS2-Base-master` — full CS2 client inventory changer (knife, glove,
  agent, music, skins). Primary source for agents and the central AOB table.
- `C:\Users\ayden\Downloads\nerv` — CS2 client cheat with `skin_changer` (incl. knife) + `glove_changer`.
  Corroborates every AOB; cleaner hand-built attribute-list path; reference `CUtlStringToken` murmur2.

## 9. Three-reference comparison for demo playback (2026-06-29)

This pass reviewed the three local folders the user supplied:

- `reference/skin changer help/Andromeda-CS2-Base-master`
- `reference/skin changer help/nerv`
- `reference/skin changer help/real-time-internal-overlay-research-main`

The practical lesson is that the normal live/offline skin-changer assumption is wrong for demo
playback. Those projects start from the local player's inventory and then copy a selected loadout item
onto the local player's live weapon/glove/pawn. SOURCE:MVM starts from a recorded demo, where the
authoritative inventory belongs to remote demo players and entities are recreated by playback, seeks,
POV changes, weapon switches, round transitions, and tick advancement. We need a standing per-demo-player
override that re-discovers current entities and reapplies to the right weapon/pawn/viewmodel every frame
or after every lifecycle event.

### What each reference does differently

`nerv` is a direct local-entity mutator. `features/skin_changer/skin_changer.cpp` walks the local pawn's
`m_weapon_services()->my_weapons()`, reads each weapon's embedded `C_EconItemView`, and applies by held
weapon definition. It writes paint/wear/seed twice: into the weapon fallback fields
(`m_paint_kit`, `m_wear`, `m_seed`) and into a hand-built econ attribute vector (`def 6 = paint`,
`7 = pattern/seed`, `8 = wear`) allocated with the game's allocator. It then sets mesh group masks on
both the weapon entity and its HUD/viewmodel weapon, calls `update_skin(true)`,
`update_weapon_data()`, clears the item description pointer, clears the HUD weapon icon, and regenerates
skins. For knives it also writes the target knife definition, quality, `SetModel()` on the world weapon
and HUD weapon, mesh group mask, `UpdateSubclass()`, `UpdateSkin()`, and weapon-data refresh. For gloves
it writes `pawn->m_econ_gloves()`, constructs/sets the paint kit, removes/recreates attributes, sets
`m_initialized = true`, calls `set_body_group()`, and sets `m_need_to_reapply_gloves = true` for 3-4
frames after config/spawn/team/engine-reset changes.

`Andromeda-CS2-Base-master` is an inventory-backed changer. It creates fake `CEconItem` objects, adds
them to the local inventory, equips them into loadout slots, and then on `FrameStageNotify` copies the
loadout item identity onto live weapons: `m_iItemID`, `m_iItemIDHigh`, `m_iItemIDLow`, `m_iAccountID`,
`m_bDisallowSOC = false`, and `m_bRestoreCustomMaterialAfterPrecache = true`. It writes fallback paint,
seed, wear, and named econ attributes via `C_EconItemView::SetAttributeValueByName("set item texture
prefab/wear/seed")`. It resolves legacy-model state from the paint kit, sets mesh group masks on the
world weapon and first-person HUD/viewmodel weapon, swaps knife models with `SetModel()`, updates
`m_nSubclassID` and `UpdateSubclass()`, calls `UpdateCompositeMaterial`, `UpdateCompositeMaterialSet`,
`UpdateSkin`, and `CGameSceneNode::PostDataUpdate`, then clears HUD weapon icons. Gloves are pawn-level:
copy full identity from loadout slot 41 into `m_EconGloves`, set initialized true, call body-group
refresh, and set `m_bNeedToReApplyGloves` for several frames. Agents are simple pawn `SetModel()` from
loadout slot 38's `m_pszModelName`.

`real-time-internal-overlay-research-main` is very close to Andromeda's live-inventory approach. Its
`src/game/skins/skin_changer.cpp` tracks fake added item IDs, finds the owner's live weapon entities,
copies the equipped loadout item view identity into the weapon item view, clears `m_bDisallowSOC`, adds
StatTrak and nametag entities, and for knives sets the target item definition and calls `SetModel()` on
the weapon and active viewmodel. It has a `SetModel` hook (`OnSetModel`) that rewrites a viewmodel model
argument before the engine applies it, preventing the default knife from flashing when the game rebuilds
the viewmodel. Its `OnEquipItemInLoadout` hook equips defaults for incompatible slots so the live game
gives the expected base weapon. These inventory/loadout hooks are local-player live-mode conveniences;
the visible-render mechanisms are the useful part for demo playback.

### What our implementation already matches

`AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp` now does the right high-level demo thing:
profiles are keyed by SteamID64, not entity index; the frame loop walks weapon-like entities and matches
`C_EconEntity::m_OriginalOwnerXuidLow/High`; dropped weapons still follow the original owner; entities are
re-discovered after seeks and recreation. It overwrites `m_NetworkedDynamicAttributes` def 6/7/8/81 when
present, writes fallback fields as a backup, can direct-call the Andromeda composite trio, and has a
periodic/dense composite reassert window. `CosmeticModelSwap.cpp` also has the reference functions:
`SetModel`, `SetMeshGroupMask`, `UpdateSubclass`, `UpdateBodyGroupChoice`, `PostDataUpdate`, model-path
lookup from `GetStaticData`, HUD-arms viewmodel walking, agent `SetModel`, glove writes, and tick nudge.
That means the architecture direction is correct: per-player demo overrides plus per-entity/pawn reapply.

### What is still missing or weaker than the references

1. **Missing attribute creation path when no dynamic attribute exists.** Our weapon/glove path mostly
   overwrites existing `m_NetworkedDynamicAttributes`. That works for normal painted weapons, but fails
   when `attrWritten=0` in the debug log: default knives, some viewmodel entities, default gloves, or
   entities whose demo item has no paint attribute vector. `nerv` creates an attribute vector from
   scratch; Andromeda uses `SetAttributeValueByName`. We need one of those paths as a fallback for
   paintable entities that have no def 6/7/8 to overwrite.
2. **Incomplete item-view identity/cache writes in normal mode.** The references usually copy or set
   `m_iItemID`, `m_iItemIDHigh/Low`, `m_iAccountID`, `m_bDisallowSOC=false`,
   `m_bRestoreCustomMaterialAfterPrecache=true`, description pointer, and HUD weapon icon state. Our
   default path deliberately avoids `m_iItemIDHigh=-1` because it can break the HUD, and only touches
   account/item-low in fallback mode. That is safer for demos, but it means some engine consumers may
   still trust stale SOC/loadout state and skip the new item-view state.
3. **HUD/inventory UI refresh is not equivalent.** `nerv` and Andromeda explicitly clear the weapon
   selection icon/description and/or regenerate skins after a change. Our render path can update the
   model/material while the HUD or Panorama item text/icon still reflects the original item.
4. **Gloves need a non-networked fallback path.** Current glove apply writes the embedded pawn
   `m_EconGloves` and overwrites existing networked attributes if the vector exists. If a default glove
   has no vector, paint cannot be applied. The references either construct the paint kit / attribute list
   or copy a real loadout item view, then reassert body-group flags for several frames.
5. **Viewmodel coverage is lifecycle-dependent.** Our `RefreshViewmodelWeapons` is safer than the
   early blind child walk because it class-matches the world weapon, but it only receives `ownerPawn`
   for the active deployed weapon. That is right for safety, but it means a profile changed while the
   target weapon is holstered must re-fire when that weapon becomes active. The current code tries to do
   this for knives with per-owner state; verification still needs to prove it for weapon meshes, knife
   type swaps, and first-person viewmodels after POV changes.
6. **No `SetModel` hook equivalent.** The overlay research repo hooks `SetModel` so the viewmodel knife
   never flashes back to the default model during engine rebuilds. Our approach reasserts after the fact.
   A hook may be unnecessary, but if users see one-frame default-knife/default-agent flashes after seeks
   or deploys, the missing pre-call interception is the likely reason.
7. **Knife type swap remains the riskiest path.** The references are live local-player tools. They can
   assume the local pawn/viewmodel is stable during `FrameStageNotify`. In demo playback, seeks and POV
   changes rebuild animation state asynchronously from our perspective. Our stable-window and active
   weapon gating are correct, but bugs here are more likely to be timing/animation lifecycle issues than
   bad paint writes.
8. **Docs and code comments disagree about persistence.** `CosmeticProfile.h` still describes
   `%APPDATA%` persistence, while the newer system clears runtime profiles on demo path changes and
   normalizes the compatibility JSON to empty. For demo mode the correct behavior is runtime-only unless
   an explicit save/export feature is added.

### Live-mode assumptions that do not carry into demo playback

- "Local player" is the target. In demos the target is usually the spectated player, and third-person
  shots may need cosmetics on many players, not just the current POV.
- "Inventory/loadout owns truth." Demo players' live inventory objects are not editable local loadouts.
  We must key desired cosmetics by SteamID64 and apply to current entities/pawns, not rely on
  `CCSPlayerInventory::GetItemInLoadout` for the remote player.
- "Weapons already exist." In demos they may not exist until the relevant tick/deploy. The apply loop
  must treat missing entities as normal and apply later.
- "One write is enough." Demo snapshots and entity recreation can revert econ item views, models, mesh
  groups, and body groups. Reapply must be idempotent and lifecycle-aware.
- "First person and third person are the same object." References update both world weapon and
  viewmodel/HUD weapon. Demo playback may have only one of them depending on camera/observer mode, and
  each must be refreshed separately.
- "Gloves are weapons." They are not; they are pawn-level `m_EconGloves` plus body groups and arms.
- "Agent is an econ skin." It is a pawn model path swap; paint/fallback/composite logic is irrelevant.
- "Persistence is harmless." In a demo tool, stale profiles reapplying after demo close/reload look like
  bugs. Runtime-only start-clean behavior is the right default.

### Likely root causes of current SOURCE:MVM bugs

1. Weapon skins partially apply because we successfully write fallback fields or networked attr values,
   but the visible composite only rebuilds when direct composite/PostDataUpdate fires on the correct
   live entity. If an entity is recreated after that, it reverts until the reassert path runs again.
2. Skins fail on default knives/gloves because there are no networked paint attributes to overwrite.
   `attrWritten=0` in `mvm_debug` is the decisive sign. Writing fallback fields alone may not feed the
   already-built composite, especially on default knife/glove models that have no target paint material.
3. Knife type changes are unstable because a def/model/subclass swap changes animation/model state on an
   entity that demo playback may be rebuilding. The crash is likely timing around viewmodel/world model
   animation state, not the paint kit itself.
4. Third-person and first-person disagree because world entity, active viewmodel weapon, HUD arms, and
   pawn body groups are separate refresh surfaces. A successful world `SetModel` does not guarantee the
   first-person viewmodel or glove arms were refreshed.
5. Gloves show default or no visual change because the pawn-level write lacks the full reference recipe
   when attributes are absent: full identity/cache state, paint-attribute creation or named setter,
   initialized true, body-group update, need-reapply true, HUD arms model refresh, and multi-frame
   reassert.
6. UI/render inconsistency happens because the references clear item descriptions and HUD weapon icon
   caches after changing item identity. Our current render-focused path can leave stale UI caches.
7. Old overrides reappearing were caused by persistence/session scope. The newer runtime-only clearing
   is the right design; the remaining docs/comments should stop implying persistent profiles are normal.

### Recommended implementation plan

1. **Keep the current per-SteamID, per-frame demo architecture.** Do not port the local-player inventory
   model wholesale. Use the references only for the entity mutation and refresh recipes.
2. **Add an attribute-creation/named-setter fallback.** When a matched weapon/glove has no writable
   def 6/7/8 networked attrs, either allocate a `CAttributeList` like `nerv` or resolve and call
   `C_EconItemView::SetAttributeValueByName` like Andromeda. Prefer named setter first if the function
   is already resolved and stable; otherwise port the small allocator-backed vector writer.
3. **Broaden item-view state writes behind safe gates.** Add optional writes for `m_bDisallowSOC=false`,
   `m_bRestoreCustomMaterialAfterPrecache=true`, `m_iAccountID`, and controlled item-ID handling, with
   diagnostics showing which were written. Keep `m_iItemIDHigh=-1` opt-in because it can break HUD
   identity.
4. **Implement HUD/description cache refresh as a separate optional step.** Resolve/guard
   `CCSGO_HudWeaponSelection::ClearHudWeaponIcon`, clear the econ item description pointer when the
   offset is known, and only fire on actual changes to avoid constant UI churn.
5. **Finish glove parity with references.** For pawn `m_EconGloves`, copy/write full item-view state,
   create or set paint attributes if missing, set quality unusual, set initialized true, call
   `UpdateBodyGroupChoice`, set `m_bNeedToReApplyGloves`, refresh pawn and HUD arms with
   `PostDataUpdate`, and reassert for 3-4 frames on spawn/team/engine-reset/profile changes.
6. **Keep knife type swaps conservative.** Only fire on active deployed weapon, after seek-settle, once
   per entity/target def, with a user-visible disable. If one-frame default flashes persist, consider a
   guarded `SetModel` hook like the overlay research repo, but do that only after logging proves
   after-the-fact reassert is too late.
7. **Make viewmodel/world-model diagnostics first-class.** Every apply log should distinguish entity
   index, class, owner XUID, active weapon, attr written count, world model path, viewmodel model path,
   mesh mask, composite called, and PostDataUpdate called. That is the fastest way to identify whether
   a failure is data write, composite refresh, model swap, or wrong visual surface.
8. **Update comments/docs to runtime-only profile scope.** If persistent presets are desired later, make
   them explicit user-managed presets, not auto-loaded session state.

### Files/functions to change when implementation starts

- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp`
  - `WriteNetworkedSkinAttributes`: add a "missing attrs" result distinct from empty success.
  - `ApplyCosmeticWrite`: add gated `m_bDisallowSOC`, restore-material, account/item identity, and
    description-cache/HUD-refresh hooks.
  - `ApplyMatchedWeapons`: choose between networked attr overwrite, named setter, or created attribute
    list; re-run direct composite/PostDataUpdate after each successful write path.
  - `ApplyPawnCosmetics`: drive the stronger glove apply state machine and keep agent reapply keyed by
    pawn pointer plus model hash.
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticModelSwap.cpp`
  - `SafeApplyGloves`: add paint-attribute creation/named setter fallback and full item-view cache
    writes; log absent attribute vectors as an expected branch, not an opaque failure.
  - `RefreshViewmodelWeapons`: keep class-matching safety, but log when no active viewmodel exists so
    first-person failures are diagnosable.
  - resolver section: add or expose `ClearHudWeaponIcon` / description refresh if implemented here.
- `AfxHookSource2/SchemaSystem.h` and `.cpp`
  - add any missing schema offsets for `m_bDisallowSOC`, `m_iItemID` (full 64-bit if needed),
    custom name/description pointer, glove/item cache fields, and any attribute-list layout fields used
    by the allocator fallback.
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticDebug.cpp`
  - expand `diag` / `visualdiag` with attr-missing reason, item identity/cache flags, viewmodel model,
    glove attr presence, HUD arms handle, and PostDataUpdate/composite counters.
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticProfile.h`
  - update comments to match runtime-only session scope, or add a separate explicit preset persistence
    concept later.
- `AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h`
  - ensure UI commands send the actual held weapon def when known, and distinguish "paint current
    weapon" from "change knife type/model" so the risky path is not fired accidentally.

### Verification matrix

Run every case with `mvm_debug start`, `cosmetics status`, `cosmetics visualdiag`, and cropped
screenshot diffs where possible. Verify both paused and playing demo states.

1. **Primary weapon skin:** choose a visible AK/SSG/AWP paint for the spectated player. Expected:
   attrWritten includes def 6/7/8 or fallback setter path logs success, direct composite calls > 0,
   world and first-person viewmodel repaint immediately, survives 10 seconds of playback.
2. **Secondary weapon skin:** repeat on USP/Glock/Deagle. Expected same as primary.
3. **Missing-attr fallback:** pick a weapon or entity whose debug log currently shows `attrWritten=0`.
   Expected after fix: created/named attributes logged, composite called, visible repaint or clear
   "not paintable" reason.
4. **Default knife paint only:** keep type default and apply paint. Expected: if default knife has no
   paint target, log says no target material; if fallback attr creation works, visible paint appears.
5. **Knife type swap:** default knife -> Karambit/Bayonet with paint. Verify active first-person,
   third-person follow camera, weapon switch away/back, seek back/forward, and no crash. Compare world
   model path and viewmodel model path.
6. **Custom knife paint:** on a demo player already carrying a custom knife, change paint only. Expected:
   no subclass/model swap required; composite updates the existing knife model.
7. **Gloves:** default gloves -> specialist/sport gloves with paint. Verify first-person hands and
   third-person arms after tick nudge, after POV switch, after seek, and after team/round transition.
8. **Agent:** swap a CT/T agent model. Verify third-person camera shows new model, `m_ModelName` readback
   changes, first-person view remains unaffected except arms/gloves.
9. **Per-player isolation:** apply different skins to two players, switch POV between them, and verify
   owner XUID keeps each override on the right weapons and dropped guns.
10. **Entity late creation:** set cosmetics before the target weapon is deployed, then wait for deploy.
    Expected: apply fires when entity appears; no manual re-command needed.
11. **Seek/recreate:** apply skin, seek backward before the change, seek forward, and switch POV.
    Expected: profiles do not persist after demo close/path change, but active session overrides reapply
    after settle without touching half-rebuilt entities.
12. **Clear/reset:** run `cosmetics clear`, close/reload demo, and verify no prior cosmetics reappear
    unless commands are sent again.

## 10. Follow-up: deeper nerv + real-time overlay comparison (2026-06-29)

This section exists because `real-time-internal-overlay-research-main` is not just a duplicate of
Andromeda. It proves several extra pieces that matter to our failing demo customizer: item-view SOC
state, synthetic inventory item identity, StatTrak/nametag attachment refresh, and a `SetModel` hook
that catches viewmodel rebuilds before the wrong model is drawn.

### nerv: the useful non-inventory path

`nerv/features/skin_changer/skin_changer.cpp` is local-player-only, but its weapon mutation path is
useful for demos because it does not depend on fake `CCSPlayerInventory` items. On every
`FrameStageNotify(stage == 7)`, it walks `local_pawn->m_weapon_services()->my_weapons()`, resolves each
`C_EconEntity`, and writes both the entity fallback fields and the embedded `C_EconItemView`.

Important mechanics:

- `apply_skin(...)` always removes and recreates item attributes before refresh. It calls
  `econ_item_attribute_manager::remove(item)` and then `create(item, paintKit, wear, seed)`, writes
  `weapon->m_paint_kit()`, `m_wear()`, and `m_seed()`, optionally writes `item->m_custom_name()`,
  chooses a mesh group from `CPaintKit::uses_old_model()`, updates both world weapon and HUD weapon
  mesh masks, calls `weapon->update_skin(true)`, `weapon->update_weapon_data()`, and clears
  `item->m_name_description_ptr()`.
- `process_knife(...)` writes `item->m_definition_index()` and `m_entity_quality() = QUALITY_UNUSUAL`,
  calls `weapon->set_model(model_path)` and the same for the HUD weapon, creates paint attributes when
  the paint kit is non-zero, sets knife mesh group masks on world and HUD weapon, calls
  `weapon->update_subclass(selected_knife)`, `update_skin(true)`, `update_weapon_data()`, clears the
  description pointer, and clears the HUD weapon icon.
- `get_hud_weapon(...)` resolves first-person weapon models by walking `m_hud_model_arms()` scene-node
  children and matching the child owner entity's `m_owner_entity()` handle back to the world weapon.
  This is an important first-person/third-person split: updating only the world weapon is not enough.
- `features/shared/econ_item_attribute_manager.cpp` gives the missing no-attribute fallback. It
  allocates three `econ_item_attribute_t` records with the game allocator, writes def indexes
  `6 = paint`, `7 = pattern/seed`, `8 = wear`, writes both `value` and `init_value`, and stores the
  new pointer/count into the item attribute vector. This is the cleanest reference for our
  `attrWritten=0` cases.
- `features/glove_changer/glove_changer.cpp` treats gloves as pawn state, not weapon state. It writes
  the pawn's embedded `m_econ_gloves()`, sets definition and quality, constructs/sets the paint kit,
  removes/recreates attributes, sets `m_initialized() = true`, calls `local_pawn->set_body_group()`,
  sets `m_need_to_reapply_gloves() = true`, and repeats for several frames after config, spawn, team,
  or engine-reset changes. On team changes it first clears the glove item for two frames.

What ports to SOURCE:MVM: the allocator-backed attribute creation, HUD/viewmodel refresh separation,
description/HUD icon invalidation, and multi-frame glove state machine. What does not port directly:
the assumption that there is exactly one target, `g_ctx->m_local_pawn`, and that the local player's
weapon-service list is the whole world.

### real-time-internal-overlay-research-main: the missing inventory/SOC details

`real-time-internal-overlay-research-main/src/game/skins/skin_changer.cpp` is also live local-player
code, but it shows why simply writing paint fields can still fail. Its frame hook uses the inventory
as the authoritative source, then copies identity and cache state into live weapon entities.

Important mechanics:

- `src/game/game_hooks.cpp` hooks four places: `FrameStageNotify` -> `OnFrameStageNotify`,
  `FireEventClientSide` -> `OnPreFireEvent`, `EquipItemInLoadout` -> `OnEquipItemInLoadout`, and
  `C_BaseModelEntity::SetModel` -> `OnSetModel`. The `SetModel` hook is the important new reference
  point for demo viewmodel flashes.
- `OnFrameStageNotify(frameStage == 6)` walks entity indexes from `MAX_PLAYERS + 1` to the highest
  entity, filters `IsBasePlayerWeapon()`, and keeps only weapons whose `GetOriginalOwnerXuid()` equals
  the local inventory owner SteamID. For demo mode, this is the part we must generalize to every
  profiled SteamID64, not just the local owner.
- For each live weapon, it finds the matching equipped loadout item. For normal weapons it scans slots
  `0..56` and matches definition index; for knives/gloves/other items it uses the definition's loadout
  slot. It only applies if the loadout item ID is one of the fake items it created.
- The critical identity write is:
  `pWeaponItemView->m_bDisallowSOC() = false`, then copy `m_iItemID`, `m_iItemIDHigh`,
  `m_iItemIDLow`, and `m_iAccountID` from the fake loadout item into the live weapon item view.
  The source comment marks `m_bDisallowSOC=false` as "the fix". That strongly suggests some CS2 code
  ignores or de-prioritizes item-view SOC data while this flag is still blocking SOC use.
- Before visual refresh it calls `pWeapon->AddStattrakEntity()` and `pWeapon->AddNametagEntity()` when
  the weapon is not a UI preview weapon. These are separate model attachments, so StatTrak/nametag can
  be missing even if paint itself applies.
- For knives it writes the target `m_iItemDefinitionIndex`, calls `pWeapon->SetModel(knifeModel)`, and
  if the active viewmodel weapon handle matches, calls `pViewModel->SetModel(knifeModel)`. It also sets
  `pViewModel->pAnimationGraphInstance->pAnimGraphNetworkedVariables = nullptr`, which is a risky but
  explicit animation-state reset to keep the new model from inheriting stale networked animation vars.
- For non-knife weapons it gets the paint kit through `GetCustomPaintKitIndex()`, checks
  `CPaintKit::UsesLegacyModel()`, sets the world weapon scene node mesh group mask to
  `1 + usesOldModel`, and mirrors that mask onto the active viewmodel scene node.
- `OnSetModel(C_BaseModelEntity* pEntity, const char*& model)` runs before the original `SetModel`.
  If the entity is a viewmodel and the active weapon belongs to the local owner, it looks up the fake
  knife loadout item and rewrites the incoming `model` argument to the target knife model. This prevents
  a default knife from drawing for a frame when the engine rebuilds the viewmodel. Our current demo
  approach reasserts after the engine writes the model; this hook shows the stronger pre-call option.
- `OnEquipItemInLoadout(...)` is a live-loadout compatibility hook. When a fake item is equipped into
  an incompatible slot, it equips the default item ID `(0xF << 60) | defIndex` and marks the previous
  SOC item updated. This is useful to understand live mode, but it should not be ported to demos.
- `OnPreFireEvent(...)` rewrites the local knife killfeed weapon string on `player_death`. This is
  optional for demo rendering; it only matters if our UI/HUD killfeed should display the swapped knife
  name/icon.
- `src/game/menu/menu.cpp` creates fake `CEconItem` objects. It dumps item definitions and paint kits
  from the item schema, filters valid skin/item pairs through
  `Helper_GetAlternateIconKeyForWeaponPaintWearItem(defIdx, paintKitID, 0)`, assigns new item and
  inventory IDs from `GetHighestIDs() + 1`, writes account ID, def index, unusual quality for
  knives/gloves, calculated rarity, then calls `SetPaintKit`, `SetPaintSeed`, `SetPaintWear`,
  optional `SetStatTrak`/`SetStatTrakType`, optional `SetCustomName`, and `AddEconItem`.
- `src/sdk/source2-sdk/econ/ceconitem.{hpp,cpp}` shows those setters are dynamic econ attributes:
  paint kit def `6`, seed def `7`, wear def `8`, StatTrak count def `80`, StatTrak type def `81`,
  and custom name def `111`. The setter resolves the attribute definition interface from the item
  schema and calls the game's dynamic-attribute setter.

What ports to SOURCE:MVM: `m_bDisallowSOC=false`, stable item identity/cache writes, StatTrak/nametag
attachment refresh, active viewmodel mesh/model mirroring, and possibly a guarded `SetModel` hook for
viewmodel knife rebuilds. What does not port directly: fake local inventory items, equip-loadout hooks,
local-only SteamID filtering, and any assumption that `CCSPlayerInventory` can represent remote demo
players.

### What this changes in our implementation plan

The current SOURCE:MVM implementation is close on architecture and incomplete on write surfaces.
`AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp` already does the demo-correct part:
profiles are keyed by SteamID64, weapon entities are rediscovered every frame, and matches are based on
`m_OriginalOwnerXuidLow/High` so dropped guns and recreated entities can be reapplied. It also already
has direct Andromeda-style composite signatures and calls `SetAttributeValueByName` inside
`FireDirectCompositeRefresh`. `CosmeticModelSwap.cpp` already resolves `SetModel`,
`SetMeshGroupMask`, `UpdateSubclass`, `UpdateBodyGroupChoice`, `GetStaticData`, and `PostDataUpdate`.

The gap is that these pieces are not yet organized into the same complete recipes as the references:

1. **No complete missing-attribute fallback.** `WriteNetworkedSkinAttributes` can overwrite
   `m_NetworkedDynamicAttributes`, but it cannot create def 6/7/8 when the vector is absent. The direct
   composite path calls `SetAttributeValueByName`, but only as part of the composite experiment; it is
   not treated as the authoritative fallback when `attrWritten=0`. Add an explicit branch:
   networked overwrite first, named setter second, allocator-backed attribute creation third.
2. **Item identity/cache state is still partial.** We write some fallback fields and optional item IDs,
   but the real-time repo shows the live weapon item view may need `m_bDisallowSOC=false` plus coherent
   `m_iItemID`, `m_iItemIDHigh/Low`, and `m_iAccountID`. In demo mode this should be synthetic and
   session-local: a stable fake item ID per `(demo session, SteamID64, slot, def)` is safer than copying
   the user's local inventory or persisting anything.
3. **StatTrak and nametag are attachment refreshes, not just attributes.** If we support StatTrak or
   custom names visually, we need resolved `AddStattrakEntity` and `AddNametagEntity` equivalents or
   diagnostics that clearly say the attribute was written but the attachment was not refreshed.
4. **Viewmodel model resets may need interception, not only reassertion.** Our current model path
   reasserts after entity discovery. If users see default knife flashes when seeking, switching POV, or
   deploying, the real-time `OnSetModel` hook explains why: the engine can draw the wrong model before
   our next frame loop repairs it. A guarded demo-only `SetModel` hook should be considered after logs
   prove after-the-fact reassert is too late.
5. **Glove paint needs the nerv-style create path.** `SafeApplyGloves` currently logs `attrVec=0` and
   then cannot paint default/no-attr gloves. The reference fix is to create or set paint attributes on
   the embedded pawn `m_EconGloves`, set quality/initialized state, update body groups, mark
   `m_bNeedToReApplyGloves`, refresh HUD arms, and repeat for several frames.
6. **HUD/description invalidation is still missing.** nerv clears `m_name_description_ptr`, clears the
   exact HUD weapon icon, and regenerates skins. real-time avoids part of this by feeding full fake SOC
   item identity. Our demo path should resolve this separately so render success and UI success can be
   diagnosed independently.

### Demo-specific design rules after the deeper read

- Keep per-player profiles keyed by SteamID64. Do not collapse back to local-player-only logic.
- Treat missing weapon entities as normal. Apply when an entity appears, not when the UI command is sent.
- Treat every seek, POV switch, weapon deploy, player spawn, and demo tick advance as a possible entity
  or viewmodel reset.
- Mutate world weapon, first-person viewmodel/HUD weapon, pawn gloves, pawn agent model, and HUD item
  caches as separate surfaces. A successful write to one surface does not imply the others updated.
- Keep all synthetic item IDs and SOC/cache state runtime-only. Closing or changing demos must clear
  them unless an explicit preset export/import feature is added.
- Port visible-render mechanics from live changers, not live inventory ownership. Fake inventory and
  `EquipItemInLoadout` solve local offline play; demo playback needs per-entity application to remote
  players.

### Concrete files to update when implementation starts

- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp`
  - promote `SetAttributeValueByName` to an explicit fallback when `attrWritten=0`;
  - add an allocator-backed def 6/7/8 attribute-vector fallback if named setters do not stick;
  - add guarded `m_bDisallowSOC=false`, restore-material, item identity, account ID, and description
    cache writes behind diagnostics;
  - add StatTrak/nametag refresh hooks or explicit "not refreshed" logging;
  - keep applying by owner XUID, not local inventory.
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticModelSwap.cpp`
  - extend `SafeApplyGloves` with the same missing-attribute fallback as weapons;
  - keep world weapon and viewmodel updates separate and logged;
  - consider a demo-only `SetModel` hook only if logs show post-frame reassert flashes.
- `AfxHookSource2/SchemaSystem.{h,cpp}`
  - add/verify offsets for `m_bDisallowSOC`, full item ID fields, item description/custom-name fields,
    attribute-list allocation layout, StatTrak/nametag attachment handles, and any hook target
    signatures needed for HUD icon clear or `SetModel`.
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticDebug.cpp`
  - include `socAllowed`, item ID high/low, synthetic item ID, named-setter result,
    created-attribute result, StatTrak/nametag refresh result, and viewmodel/worldmodel model paths.
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticProfile.h`
  - update comments to match runtime-only demo-session behavior. Persistent presets should be an
    explicit user feature, not automatic replay state.

### Extra verification from this follow-up

Add these to the matrix above after implementing the next pass:

1. A weapon that currently logs `attrWritten=0` should log either `namedSetter=1` or
   `createdAttrs=1`, then repaint without requiring the user to change weapons.
2. A swapped knife should not show the default knife for a frame after `demo_goto`, POV switch, or
   weapon deploy. If it does, test a guarded `SetModel` hook against only the profiled owner/viewmodel.
3. A StatTrak value should show both the written attribute and the rendered counter attachment, or the
   diagnostic should clearly say attachment refresh is unresolved.
4. A custom name should invalidate item description/HUD cache without permanently changing the user's
   live inventory or surviving demo close.
5. Gloves on a default/no-attr player should log created or named paint attributes, refreshed body
   groups, refreshed HUD arms, and multi-frame `m_bNeedToReApplyGloves` reassertion.
6. Two demo players with different synthetic item IDs should keep separate weapon/glove/knife state
   across POV switches and dropped weapon pickups.

## 11. Knife-type-swap CRASH investigation (2026-06-29) — instrumentation + confirmed crash site

> Status: **diagnosed, fix not yet implemented.** This section is the "before the fix" record the work
> built up. Update it again once a fix lands (see "Fix outcome" placeholder at the end).

### Symptom

A knife **TYPE** swap (e.g. `cosmetics player <id> knife 500` = Bayonet) on a demo player crashes the
game a short time later — most reliably on a **quick weapon switch (QQ)**, or once the swapped knife is
animated during playback. Paint-only skin changes never crash. The crash is **model-specific**: the user
confirmed **Shadow Daggers (def 516 → `knife_push`) does NOT crash**, while **Bayonet (def 500 →
`knife_bayonet`) DOES** — same tool, same demo flow. So it is not "all knife swaps," it is "certain knife
models."

### Instrumentation added to chase it (all in the `mvm_debug` log)

All use `MvmDebugLog_LinefAlways` (no dedup, flushed every line) so nothing is lost when the process dies:

- **`knife.fire`** (`CosmeticOverrideSystem.cpp`) — the re-fire decision: `recreated` (new entity index
  for the same owner = the engine destroyed+recreated the knife on a switch), `allowSwap`,
  `framesSinceSeek`, FIRE vs SUPPRESS-settle.
- **`knife.swap` / `knife.vm`** (`CosmeticModelSwap.cpp`) — per-native-call breadcrumbs around
  `SetModel`/`SetMeshGroupMask`/`UpdateSubclass`/viewmodel-mirror/`PostDataUpdate`, written+flushed
  **before** each call so an unmatched `…begin` pinpoints a fault inside that call.
- **`crash.veh`** (`CosmeticDebugLog.cpp`) — a vectored exception handler (registered while the log is
  open, armed for 5 s after each swap via `MvmCrashWatch_Arm`) logs the faulting **module+offset**,
  access type, target address, and thread of any access violation. This is what named the crash site.

### What the logs proved

- The `knife.swap` steps always reach `END ok=1`; **our calls do not fault**. The viewmodel mirror is
  inert in demos (`RefreshViewmodelWeapons` bails — no HUD-arms — so the `knife.vm` walk never runs).
- The fatal crash is **`client.dll+0x3399cc`**, an **`ACCESS_VIOLATION read`, `target=0x8`**, on a
  **worker thread** (≠ main). RVA is identical across runs/demos (deterministic site).
- It only happens once the swapped model is **animated** (playback, or the deploy after a QQ), never
  while truly idle/paused. Entity-recreation on a switch is confirmed: `knife.fire recreated=1` with
  `idx≠prevIdx`, and a `cosmetics.weapon` line showing the engine briefly restored the player's ORIGINAL
  knife model before we re-applied the swap.

### Disassembly of the crash site (capstone + pefile on the live `client.dll`)

CS2 `client.dll` is at `F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo\bin\win64\client.dll`.
Crash function RVA `0x3395d0`; the fault at `0x3399cc`:

```asm
client.dll+0x3399bf:  call   0x3561c0          ; find-or-build the per-model anim data for the swapped model,
                                               ;   writes an out-param into [rbp+0x10]
client.dll+0x3399c8:  mov    rdi, [rbp+0x10]   ; ... the out-param came back NULL
client.dll+0x3399cc:  mov    r15, [rdi + 8]    ; <<< FAULT: read [NULL+8] = 0x8  (matches target=0x8)
client.dll+0x3399d0:  movsxd rax, [rdi]        ; rdi is a CUtlVector{ count@+0, data@+8 } for that model
```

The enclosing function is gated on `[global]->vtable[0x30]() == 3` and walks sequence lists at
`r14+0x18/+0x58/+0x78` — i.e. the **animation / bone-setup pass**. So the engine looks up a **per-model
animation table** for the swapped knife, gets NULL, and dereferences it unchecked.

### Root cause (high confidence) + the model-specific wrinkle

A **raw `SetModel`** points the replay knife at a model whose **animation/sequence data isn't built in
this demo** (the player never carried that knife). When the engine's async anim pass later poses the new
model, the per-model table is null → null deref. This is the CS2 form of the classic Source
**`SetModel: not precached`** crash ([source-sdk-2013 #206](https://github.com/ValveSoftware/source-sdk-2013/issues/206),
[Source-1-Games #3413](https://github.com/ValveSoftware/Source-1-Games/issues/3413)). It matches this
doc's own §4 "animgraph-compatible" and §7 "precache an unreferenced `.vmdl`" caveats.

Open wrinkle: **why Shadow Daggers (`knife_push`) does NOT crash but Bayonet (`knife_bayonet`) does.**
Likely either (a) `knife_push`'s animation data happened to be loaded in that demo while `knife_bayonet`'s
was not, or (b) `knife_push` carries a self-contained animgraph the lookup builds successfully. Needs a
controlled A/B (swap to a knife another player in the same demo carries — model already loaded — and see
if it stops crashing).

### Why the reference cheats never hit this

Both Andromeda and `real-time-internal-overlay-research-main` only ever swap the **local** player's knife,
which is **always precached** (it is in the local inventory) and is viewmodel/main-thread animated. The
overlay repo additionally nulls `viewModel->pAnimationGraphInstance->pAnimGraphNetworkedVariables` after
its `SetModel` (an animgraph reset) — but those are **hardcoded pad offsets** (`+0xD08`/`+0x2E0`, not
schema fields) on the local **viewmodel**. Poking that into a remote demo **world** entity that a worker
thread is concurrently reading would trade this crash for a race. So the references are not a usable
template for our remote-demo-world-entity case — their `SetModel` works *because the model is precached*.

### Fix direction (to implement next)

Precache the target knife model (its animation data) **before** `SetModel`, so `client.dll+0x3561c0`'s
lookup succeeds and the worker-thread anim pass never sees null. Candidate entry points: the local SDK's
`IBaseFileSystem::Precache(const char*)`; the CS2 resource-precache mechanism demonstrated by
[KillStr3aK/ResourcePrecacher](https://github.com/KillStr3aK/ResourcePrecacher). Verify with the
`crash.veh` instrumentation: a clean repro with **no `client.dll` `crash.veh` line** = fixed.

### Instrumentation caveat found in the Shadow-Daggers run (must fix in the VEH)

The non-crashing Shadow-Daggers log spammed **hundreds of `crash.veh` lines in `AfxHookSource2.dll+0x3aff43`,
main thread, `target=0x100000032`** — these are **our own SEH-guarded reads faulting and being safely
caught** (`__except`), not the crash. The VEH should **exclude faults inside `AfxHookSource2.dll`** (and/or
report each unique faulting RIP only once) so our own caught faults can never mask the real `client.dll`
crash line. (Separately, that recurring caught fault at `AfxHookSource2.dll+0x3aff43` reading
`0x100000032` is a latent bad-pointer read in our per-frame cosmetics scan worth tracking down — benign
today because it is guarded.)

### Fix attempts

**Approach #1 (animgraph reset) — DISPROVEN for this build (2026-06-29).** Implemented the overlay-research
trick (null the entity's animgraph-instance networked-vars after SetModel) on the demo world weapon +
matching viewmodel child, with live-tunable offsets (`cosmetics animreset`). The log
(`mvm_debug_20260629_124011.log`) showed it **never fired**: `step=animreset.world instOff=0xd08
inst=0x0 wrote=0` — reading `entity+0xD08` on the world weapon returns null (the pointer-sanity guard
correctly skipped the write). `0xD08`/`0x2E0` are the local-VIEWMODEL/AG1 layout; current CS2 weapons run
**AG2** (`CBaseAnimGraphController::m_pGraphInstanceAG2`, a `CNmGraphInstance*`), whose layout is entirely
different, so the null-vars trick does not map even with a corrected instance offset. Crash persisted
unchanged (`crash.veh client.dll+0x3399cc read target=0x8`, worker thread; Butterfly def 515 swapped fine,
Paracord def 517 crashed ~0.9s later — confirming the model-specific precache theory). Code left in place,
**default OFF**, toggleable for experiments.

**Approach #2 (blocking model precache) — IMPLEMENTED (2026-06-29).** Before `SetModel`, blocking-load the
target `.vmdl` + its anim data via `CResourceSystem::PreCache` (resourcesystem.dll vtable[40], the same
blocking-load call `SceneSystem.cpp` already uses for skyboxes — `g_pCResourceSystem`, resolved at
startup). Runs on the swap (main) thread so the per-model anim table is resident before the worker-thread
anim pass poses the new model, directly preventing the `client.dll+0x3561c0`→null→`+0x3399cc [null+8]`
crash. Wired in `CosmeticModelSwap.cpp` (`SafePrecacheModel`, SEH-guarded) before the knife world-weapon
SetModel and before the agent SetModel; default ON; toggle `mirv_filmmaker cosmetics precache 0|1`. New
breadcrumb `knife.swap step=precache.begin/end fired=N resSys=N totalCalls=N`.

### Fix outcome

- [x] Precache approach used: `CResourceSystem::PreCache(.vmdl)` blocking load before SetModel (knife + agent).
- [x] `step=precache fired=1 resSys=1` confirmed firing (mvm_debug_20260629_125845.log).
- [x] **DISPROVEN — `PreCache(.vmdl)` is INSUFFICIENT.** Same crash, identical site
  `client.dll+0x3399cc read target=0x8` (`mvm_debug_20260629_125845.log` seq 1909), on a Stiletto (def 522)
  swap that fired precache. `CResourceSystem::PreCache` (resourcesystem.dll vtable[40]) loads the **model
  resource** but NOT the model's **sequence/animation resource**, which is what `0x3561c0` looks up.

### Deeper disassembly (2026-06-29, mvm_debug_20260629_125845.log) — the find-or-build call site

```asm
client.dll+0x339987:  call 0x31dc20      ; FIND (non-building) the model's anim/sequence table -> [rbp+0x10]
client.dll+0x339990:  test rax, rax
client.dll+0x339996:  je   0x3399b1      ; if FOUND, use it (no crash); else fall through to BUILD
client.dll+0x3399b1:  lea  r9,  [rbp+0x20]
client.dll+0x3399b5:  mov  rdx, rbx      ; rdx = model key
client.dll+0x3399b8:  lea  r8,  [rbp+0x10] ; r8 = &out (receives the built table)
client.dll+0x3399bc:  mov  rcx, r14      ; rcx = anim context (this)
client.dll+0x3399bf:  call 0x3561c0      ; FIND-OR-BUILD per-model anim data
client.dll+0x3399c8:  mov  rdi, [rbp+0x10] ; out == NULL  (build failed: sequence resource not loaded)
client.dll+0x3399cc:  mov  r15, [rdi+8]  ; <<< FAULT, unchecked null
```

`0x3561c0` does a resource lookup via a global (`[rip+0x22021f7]`, `vtable[0x108]`) then checks `[modelKey+8]`;
when the model's sequence resource is absent it returns the out-param NULL.

### The decisive product fact

The demo player at idx 82 ALREADY carries a **Butterfly (def 515)** (`liveModel=.../knife_butterfly...`
before any swap). So:
- Swapping to **515 (Butterfly)** = the model is already loaded -> **never crashes** (this is why the earlier
  "first swap is safe" looked true; the first swap happened to be 515).
- Swapping to any knife **not carried by any player in the demo** (Stiletto 522, Paracord 517, Shadow Daggers
  516 here) = sequence resource unloaded -> `0x3561c0` null -> **crash**.

So the crash is fully deterministic on "is the target knife's animation resource loaded in this demo." A swap
to a model present on any live weapon entity is safe; a swap to an unloaded model crashes. The remaining work
is either (A) force-load the model's SEQUENCE resource (not just the .vmdl), or (B) guard: refuse/skip a swap
to a model that isn't loaded (deterministic crash-avoidance, limits choices to knives present in the demo).

### Deeper RE toward the any-knife fix (2026-06-29) — why a clean precache is NOT readily available

Goal: find a callable client.dll function that force-loads a model's per-model ANIMATION/sequence data so
`0x3561c0`'s lookup succeeds. Findings from disassembling the live client.dll/resourcesystem.dll:

- **The crash's two globals.** The crash function (`0x339590`) and the find-or-build (`0x3561c0`) both query a
  global **G1 @ runtime 0x25583e0** via `vtable[33]` (`[rax+0x108]`) to resolve a model -> resource desc,
  then `0x3561c0` queries **G2 @ runtime 0x21d9760** via `vtable[11]` (`[rax+0x58]`) for the model's anim
  **index**; **index == -1 => bail, out-param NULL => the `[null+8]` crash.** So G2 is the loaded-model anim
  registry; an unregistered model => -1 => crash. G1 has **no write in client.dll's .text** (200+ reads, 0
  writes) -> it is an **engine interface pointer set at AppSystem connect**, i.e. an animation subsystem in
  another module, reached only through its vtable (can't RTTI it statically; no runtime vtable address).
- **`C_BaseModelEntity::SetModel` (RVA 0x8ddad0)** only resolves the path -> model handle
  (`[0x233d048]->vtable[12]`) and binds it (`0x8ddb10`). That is exactly what we already do (the model
  RENDERS), and it does NOT register the model with the animation registry G2 -> still crashes.
- **VScript `Script_PrecacheModel` / `PrecacheResource` (natives ~0x3b5200/0x3b5270/0x38ae40)** exist but are
  script-VM trampolines: they unmarshal script args (`0x38c060`) and need the script VM context
  (`g_pScriptVM @ 0x21db230`). Not safely callable directly from our hook.
- **resourcesystem.dll `ResourceSystem013` precache slots 40 (`0x171c0`, the repo's blocking PreCache) and 41
  (`0x17360`)** both load resource BYTES through the same internal path (`this->vtable[0x198]/0x278/0x188`).
  Neither registers a model with client.dll's animation system. So slot 41 is NOT a fix either; `PreCache`
  loading the `.vmdl` is necessary-but-insufficient.

**Conclusion:** registering a model's animation data happens in client.dll's animation subsystem (G1/G2) during
the normal model-binding/entity-creation path, NOT via any one-shot resource precache we can reach. A true
any-knife fix is a multi-step engine-internals trace (identify G1's animation interface + the method that
populates G2 for a model, and invoke it safely off the demo world entity) — open-ended and crash-prone, not a
single AOB. The deterministic, shippable alternative remains (B): gate the swap on whether the target model's
anim data is already loaded in the demo (skip + warn otherwise).

### Approach #3 — FIXED (2026-06-29): neutralize the null instead of loading the resource

Rather than force the missing anim resource to load (open-ended), make the null **harmless** at the exact
crash instruction. The builder `0x3561c0` (find-or-build per-model anim data) returns the sequence list via
out-params; for an unloaded model it returns NULL, and the caller dereferences it unchecked
(`+0x3399cc mov r15,[rdi+8]`). But the caller's own next logic
(`movsxd rax,[rdi]; lea r12,[r15+rax*8]; cmp r15,r12; je 0x339c17`) already has a clean **"empty list" path**
for models with zero sequences. So:

**Detour `0x3561c0` (AOB `4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 53 41 57 48 8D AC 24`, validated
UNIQUE in the live client.dll). After the original runs, if an out-param is NULL, point it at a static
EMPTY (count=0) sequence list.** The caller then reads count=0, computes end==start, takes the `je` to the
empty path, processes zero sequences, and finalizes normally. No crash; the swapped knife model still renders
(it just contributes no sequences from the missing resource).

Implemented in `AfxHookSource2/Filmmaker/Cosmetics/CosmeticAnimFix.{h,cpp}` (MS Detours, lazy-installed on
the first knife swap from `ApplyKnifeModelSwap`). Toggle `mirv_filmmaker cosmetics animfix 0|1` (default ON);
breadcrumb `knife.animfix substituted EMPTY seq-list ...`. The `0x3561c0` out-params are transient caller
locals (read-only, never freed/stored), so a shared static empty list is safe across the anim worker threads.

**Live-verified (autonomous harness `automation/verify/verify-knife-swap-nocrash.ps1`,
mvm_debug_20260629_133032.log):** ~91 knife swaps to the unloaded Stiletto (def 522) model across one
session — **crash.veh = 0** (was a 100% crash before), **knife.animfix fired 59x** on the anim worker thread.
The exact swap (Stiletto onto a player whose real knife is loaded, deploying with `recreated=1`) that crashed
every prior run now SURVIVES.

> Note: precache (#2, `CResourceSystem::PreCache(.vmdl)`) is kept ON as belt-and-suspenders — it loads the
> model RESOURCE so the mesh renders promptly; #3 handles the anim-table null that PreCache cannot. The two
> are complementary. Open follow-up (optional): the swapped knife renders but may not play idle/deploy
> ANIMATIONS (its sequences aren't loaded) — acceptable for static cinematic shots; full animation would
> still require the deep G1/G2 anim-resource registration described above.

### Fix outcome (FINAL)

- [x] #1 animgraph reset: DISPROVEN (AG2 build, offset null, never fired).
- [x] #2 model precache `CResourceSystem::PreCache(.vmdl)`: necessary-but-insufficient (loads model, not anim).
- [x] **#3 anim-builder detour + empty-list substitution: WORKS. Knife-type swaps to ANY model no longer crash.**
- [x] Verified via autonomous netcon harness: 0 crashes over ~91 swaps; animfix fired 59x.

### Follow-up FIXED (2026-06-29): model/skin desync on seek (unlocked by the crash fix)

After the crash fix, a knife swap that was then SEEKED looked wrong: the model reverted to the player's
ORIGINAL knife while the swapped SKIN persisted (e.g. Butterfly mesh + Bayonet's Boreal-Forest paint). Cause:
the skin paint applies every frame (ungated), but the knife model swap was gated TWO ways that existed ONLY
to avoid the (now-fixed) crash: (a) a 64-frame post-seek "settle" suppression, and (b) an ACTIVE-weapon-only
gate (`ownerPawn != null`) so a HOLSTERED knife never got its model swapped. After a seek the engine
recreates the (often holstered) knife with its original model -> skin re-applies, model does not -> mismatch.

Fix (CosmeticOverrideSystem.cpp, all three together): (1) shrink `kKnifeSwapStableFrames` 64 -> 8; (2) drop
the `ownerPawn` requirement so the WORLD (third-person) model swap runs for holstered knives too -- the
first-person viewmodel mirror inside `ApplyKnifeModelSwap` already auto-skips when `pawnForViewmodel` is null
(holstered); (3) add a holstered->active edge (`KnifeSwapState::lastActive`) to the re-fire trigger so the
viewmodel mirror runs once the knife is deployed. Now the model swap re-applies as reliably and quickly as
the skin. Live-confirmed by the user. Both relaxations are only safe because CosmeticAnimFix neutralizes the
unloaded-model anim crash they originally guarded against.

Third seek case (2026-06-29): seeking DIRECTLY to a tick where the knife is already deployed left the
original model+skin until the knife was re-pulled. Cause: a seek can reconstruct the knife in the SAME
entity slot (index unchanged) while resetting its model -- so `recreated`/`newTarget`/`becameActive` all stay
false and the index-based throttle never re-fires, even though the engine reverted the model. Fix: added a
`modelMismatch` trigger -- read the entity's LIVE world model (`ReadEntityModelPath` / CModelState::m_ModelName)
and compare it to the resolved target (`ResolveKnifeModelPath`, exposed from CosmeticModelSwap); re-fire the
swap whenever they differ. Self-correcting against silent reverts; settles to false in one frame once SetModel
takes, so it does not thrash. Logged as `modelMismatch=N` in the `knife.fire` breadcrumb.
