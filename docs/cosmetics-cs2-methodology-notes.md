# Cosmetics CS2 methodology notes — knife / glove / agent model swap (from working source)

Status: methodology extracted from **two complete, working CS2 client-DLL inventory changers** that the
user supplied (`Andromeda-CS2-Base-master` and `nerv`). Both swap the **knife model**, **gloves**, and
weapon skins purely from `client.dll`; Andromeda additionally swaps the **agent (player) model**. This
doc records *exactly how they do it* — the functions, the byte signatures, the schema fields, the call
order, and the frame timing — so it can be ported into `AfxHookSource2/Filmmaker/Cosmetics/`.

> **Headline finding that overturns prior research.** `docs/cosmetics-model-override-research.md` §4
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

Both agree on every mechanism below. Where they differ it is noted.

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

What the repo **already has** (per `cosmetics-recompose-research.md`): the composite trio
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
   apply loop currently skips (`cosmetics-recompose-research.md` table: "Gloves … Does NOT work yet").
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
swaps are still available through the model-swap path. Knife-type swaps are now intentionally
default-off: `mvm_debug_20260629_065323.log` reproduced a crash after `knifeSwap=1` when the demo
player switched weapons. Use `mirv_filmmaker cosmetics modelswap knife 1` only for focused testing
until the subclass/model lifetime issue is understood. To VIEW body swaps, use a third-person camera
(`mirv_filmmaker follow preset behind` + `follow place`); a first-person view shows only the viewmodel.

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
   `cosmetics-model-override-research.md` §5 — these sources don't answer it because their target models
   are always precached for the local player. If it fails, precaching the model first (resource-system
   call) is the next step.
3. **Viewmodel vs. world model entities.** The knife swap must hit both the weapon's world entity and the
   first-person viewmodel knife entity (both sources do this via the HUD-arms scene-node walk,
   `GetKnifeModel`/`get_hud_weapon`). For a spectated player the first-person viewmodel only exists when
   first-person-spectating; in third-person only the world model entity is present. Apply to whatever
   weapon/pawn entities the spectated player actually has.

---

## 8. Reconciliation with the existing two research docs

- `docs/cosmetics-model-override-research.md`: §1 "writing `m_iItemDefinitionIndex` alone does not change
  the model" — **still true**; the model only changes because of the separate `SetModel` call. §4
  "`SetModel` is a server.dll function — the binding constraint" / "no client-only technique exists" —
  **overturned**: `C_BaseModelEntity::SetModel` is a client.dll function, AOB above, called every frame
  by both working tools for knife and agent swaps. The nSkinz Source-1 `IVModelInfoClient`/model-index
  analysis in §2 is moot — CS2 uses a string-path `SetModel`, not an integer model index.
- `docs/cosmetics-recompose-research.md`: the composite trio findings stand and are reused. The recompose
  doc's "Gloves: not a scanned weapon entity, lives on the pawn `m_EconGloves`" diagnosis is **correct**
  and §3 here is the apply path for it. The "Knife — change knife TYPE: does NOT work" and "Agents: does
  NOT work" rows are addressed by §2 (`SetModel`+`UpdateSubclass`+mesh mask) and §4 (`SetModel`)
  respectively.

## Sources (local, user-supplied — read this session)
- `C:\Users\ayden\Downloads\Andromeda-CS2-Base-master` — full CS2 client inventory changer (knife, glove,
  agent, music, skins). Primary source for agents and the central AOB table.
- `C:\Users\ayden\Downloads\nerv` — CS2 client cheat with `skin_changer` (incl. knife) + `glove_changer`.
  Corroborates every AOB; cleaner hand-built attribute-list path; reference `CUtlStringToken` murmur2.
