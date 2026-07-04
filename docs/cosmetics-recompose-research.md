# Cosmetics recompose research — making the written skin actually render

Status: research plus follow-up experiments. The repo now resolves and clears
`C_EconEntity::m_bAttributesInitialized`, and the apply loop can overwrite existing
`m_NetworkedDynamicAttributes` paint/wear/seed values. The remaining gap is a reliable visual
material rebuild trigger. Targets the gap documented at
`AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp:412-421` (`SetRecompose` /
`m_vtComposite` / `m_vtCompositeSec`): the fallback-field write **sticks** (confirmed live:
`m_iItemIDHigh=-1`, `m_nFallbackPaintKit/m_flFallbackWear/m_nFallbackSeed` updated, `patched=2
reverted=0` every frame) but the weapon keeps rendering its original skin. This doc is about the
missing visual-rebuild step only — it does not touch the separate, already-researched
knife/agent **model**-swap problem (`docs/cosmetics-model-override-research.md`).

Update after this pass: the code now exposes an explicit experimental bridge for the only visual
path verified to work in CS2: `mirv_filmmaker cosmetics paintkitbridge ...`. It writes the global
`cl_paintkit_override` cvar and restores the previous value when disabled. This is **not** a true
per-player/per-entity skin hook; it is global and only affects the next weapon deploy/create, because
that is when CS2 reads the cvar while building the composite material. The reusable live verifier is
`automation/verify/verify-cosmetics-paintkit-bridge.ps1`, with image-diff support in
`automation/tools/image_diff.py`.

## BREAKTHROUGH (2026-06-29): the per-player demo skin DOES render — via the Andromeda direct composite call

The "airtight" impossibility conclusion below (the `### Follow-up` and `## Mechanism` sections) is
**superseded**. A per-player weapon skin override now renders on the spectated demo weapon, confirmed
live with cropped, paused-tick, noise-floor-controlled screenshot diffs and direct visual inspection
(four distinct paint kits produced four visibly distinct finishes on the same USP-S). What changed:

1. **Root cause of every prior failure was a pattern-syntax bug, not a structural impossibility.**
   The Andromeda composite functions (`UpdateCompositeMaterial` / `UpdateCompositeMaterialSet` /
   `UpdateSkin`) were wired into `cosmetics composite once`, but their AOB patterns used a single `?`
   per wildcard byte. `Afx::BinUtils::FindPatternString` consumes **two hex chars per byte**, so a
   wildcard byte must be written `??`; a lone `?` is read as half a byte and silently shifts every
   subsequent literal. Result: the functions never resolved (`resolved=0`) and the entire direct
   composite path was a **no-op** — so the "write-then-rebuild never works" finding was actually
   "the rebuild call was never executing." Fixed at
   `CosmeticOverrideSystem.cpp` `ResolveDirectCompositeFns()` (`?` -> `??` on all four AOBs); the four
   patterns are present and resolve uniquely in the current `client.dll` (the `updateSkin` prologue
   has two near-identical hits 0x50 apart — first is taken; flagged as a minor ambiguity to watch).

2. **The working recipe (verified live, demo paused, USP-S def 61):**
   - override on the **actually held weapon's defIndex** (a held USP-S is def **61**, not slot `0` —
     the old verifier hardcoded `weapon 0`, which never bound and is why its data path showed
     `def6=796` unchanged);
   - **fallback identity ON** (`m_iItemIDHigh = -1`) so the composite build consults our fallback
     fields (`fallback paint/wear/seed`), plus the networked `def6` write;
   - then **`mirv_filmmaker cosmetics composite once 0x608`** — this is the **essential lever**.
     Isolated cleanly: changing the profile paint and waiting on the per-frame apply alone did **not**
     update the render (weapon kept the prior composite); firing `composite once` immediately
     re-composited to the new paint. (The old per-frame *vtable* `recompose` path faults/disarms —
     `recompose=0 faulted=1` — and was never the working path.)

3. **Proof, not a threshold artifact.** paint 504 -> dark/worn, 915 -> dark olive, 568 -> green,
   638 -> pink/purple camo, all on the same paused tick with only the paint number changing. The
   override also **persists through short playback** (the per-frame apply keeps the data written and
   the cached composite survives until the weapon entity is recreated). **Caveat:** a weapon redeploy
   / round change / **demo seek recreates the entity from the authoritative item (796)**, so
   `composite once` must be re-fired after seeking — natural for frame-by-frame filmmaking.

4. **The old whole-frame verifier gave a false-positive PASS.** It diffed the full 1600x1200 frame
   AND advanced the demo ~900ms (`demo_resume`/`demo_pause`) between shots, so ~100k pixels changed
   from motion regardless of the skin. `automation/verify/verify-cosmetics-composite-direct.ps1` is
   rewritten to stay paused, use the resolved defIndex + fallback, and compare a **viewmodel crop**
   against a same-tick TAA noise floor (`image_diff.py --crop`). It now passes on the real signal
   (weapon-crop mean ~1.5 vs noise ~0.28).

Net: the Andromeda GitHub base **did** help — its composite-function targets are exactly right. The
`cl_paintkit_override` build-time-hook direction (Path C below) is no longer the only option; the
direct post-write composite call works per-player, offline, during demo playback.

### UI auto-update wiring + per-slot status (2026-06-29)

The Cam Editor → Customize flow now renders skins without any manual command. Root gap: the UI
(`Panorama/CameraEditorJs.h` `applyCosmeticCommand`) sends `cosmetics player ... weapon <def>
paint <pk>` + `enabled 1` but nothing fired the composite, and `RunFrame` called
`ApplyMatchedWeapons(..., fireDirectComposite=false)`. Fix: added **`m_autoComposite`** (default ON,
`cosmetics autocomposite 0|1`); `RunFrame` now passes `fireDirectComposite=m_autoComposite`, and the
direct composite is change-gated (`needComposite || attr.changed`) so it fires once on a skin-data
change and re-fires after a seek/redeploy recreates the entity. **No fallback identity needed** —
verified the composite renders with `m_iItemIDHigh` left valid (the composite reads our `def6`
networked attribute + the `setAttributeValueByName` call inside the refresh).

Verified live (cropped, paused, noise-floor-controlled), replaying *exactly* the UI's commands:

| Slot | Status | Notes |
|---|---|---|
| **Primary** (e.g. SSG08 def 40) | **WORKS** | autocomposite OFF→mean 0.24 (no render), ON→mean 6.36 (full weapon repaint). |
| **Secondary** (e.g. USP-S def 61) | **WORKS** | UI command path mean 2.97 vs noise 0.17. |
| **Knife — paint, player already has a custom knife** | **Expected to work** (same composite path) — NOT verified live; the test demo's spectated players carry default knives. |
| **Knife — default knife / change knife TYPE** | **Does NOT work** | Default knife has `networkedAttrs count=0` (no attrs to overwrite) and is not a paintable composite target (engine logs `vCompMat ... did not affect any target materials on knife_default_ct`); writing `m_iItemDefinitionIndex=507` does NOT swap the renderable model (worldModel stays `knife_default_ct`). This is the deferred model-swap path (`docs/cosmetics-model-override-research.md`). |
| **Gloves** | **Does NOT work yet** | Gloves are not a scanned weapon entity — they live on the pawn (`C_CSPlayerPawn::m_EconGloves`, an embedded `C_EconItemView`). The apply loop never touches them and the glove composite-owner pointer is unknown. Needs its own pass. |
| **Agents** | **Does NOT work** | Full player-model swap, not a weapon composite. Separate mechanism (`docs/cosmetics-model-override-research.md`). |

Bottom line: **weapon (primary/secondary) skins auto-apply and render through the UI**; knife paint
should work for players who already own a custom knife; **gloves, agents, and knife TYPE swaps remain
the model-override problem** and are not delivered by the composite path.

### UPDATE (2026-06-29): the model-override rows are now delivered too

The "gloves / agents / knife TYPE swaps remain the model-override problem" caveat above is **resolved**.
The model-swap path (`CosmeticModelSwap.cpp`, `C_BaseModelEntity::SetModel` + `SetMeshGroupMask` +
`UpdateSubclass` + glove body group, all resolved on the live build) now also fires
`CGameSceneNode::PostDataUpdate` (vtable index 22, from Andromeda) after each write, and
`CosmeticOverrideSystem::MaybeFireTickNudge` briefly resumes the demo ~10 ticks after a change and
re-pauses ("let the game play ~10 ticks", automated). Together these make agent / glove / knife-type /
legacy-mesh swaps render — the missing piece was a renderable refresh + live frames, not the writes.
Verified live: weapon skin VISIBLE in place; agent `m_ModelName` flips `ctm_st6`->`ctm_fbi`
(authoritative); all model-swap functions resolve. Details in
`docs/cosmetics-cs2-methodology-notes.md` §6b. View body swaps in third person
(`mirv_filmmaker follow preset behind` + `follow place`); a first-person view shows only the viewmodel.

### LOG-DRIVEN CORRECTION (2026-06-29): knife type swaps are opt-in

`mvm_debug_20260629_065323.log` shows the crash path after a UI customizer pick:
`knife steamId=... def=500`, then `cosmetics.apply ... knifeSwap=1`, then a short tick nudge, then
CS2 Breakpad reset messages when the player switched weapons. The "cosmetics are disabled" console
line in that same sequence was only a command-order warning: the UI had sent the profile mutation
before `cosmetics enabled 1`, and the next log entry enabled cosmetics successfully.

Conclusion: the paint/composite path remains usable, but the knife TYPE model/subclass swap is not
production-safe in demo playback. It now defaults OFF behind `mirv_filmmaker cosmetics modelswap knife
1` for focused testing; normal customizer use can still write the profile without firing
`ApplyKnifeModelSwap`.

### LOG-DRIVEN CORRECTIONS (2026-06-29): reset scope, current knife resources, and viewmodels

`mvm_debug_20260629_062346.log` exposed three separate faults that were previously being conflated:

1. Cosmetic profiles were auto-saved to `%APPDATA%\HLAE\cosmetic_profiles.json`. The session began
   with four old player profiles already loaded, and a tick reset from about 944 to 1 did not change
   the demo path. Once any new edit armed the system, all old profiles applied to recreated entities.
   Profiles are now runtime-only, the compatibility JSON is normalized to
   `{"enabled":false,"players":{}}`, and state is cleared on demo path changes or a large regression
   to the first second. A close, reload, or demo restart therefore cannot reapply an earlier edit.
2. The Karambit fallback requested the removed resource
   `weapons/models/knife/knife_karam/weapon_knife_karam.vmdl`; the log contains the corresponding
   `resource ... is not in the system` error. Live `items_game.txt` and VPK contents use
   `knife_karambit/weapon_knife_karambit.vmdl`. Bowie, Navaja, and Talon had the same legacy-name
   problem and their fallback paths were corrected too.
3. The apply loop passed no owner pawn into knife/weapon model refresh, and the first-person mesh
   branch was an empty placeholder. It now resolves the original owner's pawn, refreshes weapon
   children under `m_hHudModelArms`, and refreshes them again after the direct composite call.
   Gloves no longer force the unrelated `first_or_third_person` body-group choice (which removed
   third-person hands); the engine chooses the pawn body group and the selected glove definition's
   `agents/models/shared/arms/...` model is applied to the HUD-arms entity.

These corrections are build-verified but require live demo verification for final visual behavior.

## Tooling added for this investigation (current build)

Two console commands were added to drive the investigation in-game (both in
`AfxHookSource2/Filmmaker/Cosmetics/`):

- **`mirv_filmmaker cosmetics visualdiag`** (`CosmeticDebug.cpp::Cosmetics_PrintVisualDiag`,
  read-only): dumps the spectated weapon's full visual-cache state — entity index/class, owner xuid,
  defIndex, `m_iItemIDHigh/Low`/`m_iAccountID`, the fallback paint/wear/seed/stattrak, the
  `m_NetworkedDynamicAttributes` values for def 6/7/8/81, the world-model path, the active-weapon
  handle, and crucially **the three visuals-cache flags AND their resolved schema offsets**
  (`m_bVisualsDataSet`, `m_bClearWeaponIdentifyingUGC`, `m_nCustomEconReloadEventId`). Those printed
  offsets are exactly what an IDA/Ghidra pass needs to xref the consuming function (search for byte
  access at `weapon_base + <offset>`) — closing the one gap this research could not (the "How to
  locate" section below). Run it before AND after a skin apply / rebuild to compare.
- **`mirv_filmmaker cosmetics rebuild once` / `rebuild auto 0|1`**
  (`CosmeticOverrideSystem.cpp::RebuildOnce` / `SetRebuildAuto`): `rebuild once` re-asserts the
  visuals-stale field writes (`m_bVisualsDataSet=false`, `m_bClearWeaponIdentifyingUGC=true`, bump
  `m_nCustomEconReloadEventId`, clear init flags, bump attr parity) on **every** matched weapon
  entity right now, decoupled from per-frame change detection, and fires the recompose vcall once if
  it is armed (`cosmetics recompose 1`). `rebuild auto` gates whether the per-frame loop does that
  stale-marking on change. **Caveat, consistent with the findings below:** these only re-issue the
  field writes already proven to *stick but not render*; they make the field-poke path manually
  triggerable for A/B screenshots and provide a single on-demand slot to fire a resolved rebuild
  function once one is found — they are not, by themselves, expected to fix the render until the
  consuming function (or a lifecycle event) is actually invoked.

- **`mirv_filmmaker cosmetics paintkitbridge [0|1|auto|force <paint>]`**
  (`CosmeticOverrideSystem.cpp`): a deploy-time workaround around CS2's own
  `cl_paintkit_override`. `auto` follows the currently spectated profiled weapon's paint kit;
  `force <paint>` is for automation/manual proof-of-life without editing stored profiles. It is
  intentionally labeled experimental/global because it cannot target one player after the composite
  has already been built.

## tl;dr

The write almost certainly never reaches the code that builds the skin's composited material
because that code is **not** polled every frame from the econ fields — it runs from the weapon
entity's `OnDataChanged()`/visibility-change lifecycle callback, which only fires on the
*networked* update path. A raw out-of-band memory write from this DLL never triggers that
callback, so the fallback fields sit there correct-but-unconsumed. The most promising **minimal**
fix is not a vtable call at all: it's locating and calling the (non-virtual) material-rebuild
function directly via a byte signature, the same way the rest of this DLL resolves engine
functions. No current-build CS2 signature for that function was found in this pass — that is the
single concrete next step, detailed below.

## Binary-analysis update (2026-06-28): the trigger is `OnDataChanged` (a virtual), and the legacy code is present

A static pass over the **actual current-build `client.dll`**
(`F:\SteamLibrary\...\csgo\bin\win64\client.dll`, 6/11/2026, 37 MB) with capstone + pefile
(throwaway tooling under the session scratchpad; not committed) changes the framing above on
several points. Findings, strongest first:

1. **The CS:GO-legacy `cstrike15` weapon code is compiled into CS2's `client.dll`.** The build
   embeds source-path strings `...\game\shared\cstrike15\weapon_csbase.cpp`, `weapon_knife.cpp`,
   `weapon_csbasegun.cpp`, etc. So the leak architecture this doc relied on is **not just
   historical** — the same `OnDataChanged` / custom-material call sites exist as real functions in
   the running binary.
2. **CS2 replaced CS:GO's `CCustomMaterial` with the Source 2 "composite material" (`.vcompmat`)
   system.** The binary carries `CompositeMaterialGameSystem` / `CCompositeMaterialManager`, the
   paint-kit templates `workshop/paintkits/templates/*.vcompmat`, and a `composite_material_*`
   convar family. So the "rebuild" we want is **"(re)build this weapon's composite material,"** a
   different subsystem than the doc assumed — the `m_vecCompositeMaterialAssemblyProcedures` /
   `CompositeMaterial_t` family flagged as a "loose thread" earlier is in fact the relevant system.
3. **The rebuild TRIGGER is the weapon's `OnDataChanged`, which *is* virtual.** The doc conflated
   the trigger with the *non-virtual* `UpdateCustomMaterial` it calls and concluded "a vtable
   approach is structurally wrong." That is half right: `UpdateCustomMaterial` is non-virtual, but
   the thing that *calls* it — `OnDataChanged(DataUpdateType_t)` — is a normal entity virtual.
   Corroborated by the networked field **`m_nCustomEconReloadEventId`** (name literally = "reload
   the custom econ visuals when this changes"), which is a delta-checked-on-`OnDataChanged` token.
   **So `SafeVCall` is the right mechanism after all — just aimed at `OnDataChanged`, not at the
   guessed "UpdateComposite" index 7.**
4. **The repo already hypothesized this.** `CosmeticOverrideSystem.h` comments that `vtArg=0`
   tests `OnDataChanged(DATA_UPDATE_CREATED)` "the candidate skin-rebuild trigger." The only
   missing piece is the **correct vtable index for `OnDataChanged`** (the historical `7` was a
   guess for a different function).
5. **Why the override fails is now precise.** Real demo weapons render their *real* skins, which
   proves the composite **is** built during demo playback, at entity-creation time. Our override
   fails only because we change econ data *after* that build and nothing re-runs `OnDataChanged`.
   The fix is strictly "re-run the create/data-changed path for an already-created weapon."
6. **Independent confirmation from a working tool.** `yuzhouUvU/cs2_weapons_skin` (client-side)
   applies the skin and then forces a visual refresh by **destroying + recreating the weapon
   entity** (`RemoveWeapon` → entity-system `EntityRemove` → `GiveNamedItem`), letting the natural
   creation-time composite consume the new fallback values in `OnEntitySpawned`. It never calls a
   manual rebuild. Same lesson as `cs2-WeaponPaints`' `!kill` requirement — but those are
   server-authoritative calls, **unavailable during demo playback** (no server simulating the
   world), which is exactly why this tool needs a different lever.

**Concrete anchors located in the current build (for the runtime step below):**
`C_CSWeaponBase` main vftable `@ 0x181a2a798`; `C_EconEntity` main vftable `@ 0x181b5f570`;
`C_CSWeaponBase::GetEconWpnData` accessors `@ 0x180796d50` / `@ 0x180796de0` (cached at
`weapon+0x1cc0`); `m_bVisualsDataSet` / `m_nCustomEconReloadEventId` netvar strings
`@ 0x181a26240` / `@ 0x181a26268`. RTTI is walkable both statically (confirmed) and at runtime via
the repo's existing `FindClassVtable`/`getVTableFn`.

**What static analysis could NOT close (same gap as before, now narrowed):** identifying the exact
`OnDataChanged` vtable slot purely statically did not converge — the composite request is 3+ call
levels deep and/or routed through an indirect (vtable) call, so a linear string/call-xref scan of
the slot bodies surfaces nothing. The reliable way to get the index is a **one-time runtime vtable
dump** of a live weapon entity (the repo can already resolve the vtable via `FindClassVtable`),
then confirm by A/B in-game. This is now a single value to find, not an open-ended signature hunt.

**Refined recommendation (supersedes "Recommended minimal change" item 2's "retire the vtable
approach"):**
- **Path A — recommended, reuses existing infra.** Add a diagnostic that dumps `C_CSWeaponBase`'s
  vtable at runtime so `OnDataChanged`'s index can be identified, then drive the existing
  `cosmetics recompose` / `vtidx` / `vtarg` path to call `weapon->OnDataChanged(DATA_UPDATE_CREATED)`
  **after** the econ write **and** a `m_nCustomEconReloadEventId` bump. Almost all of this already
  exists; only the index + the right `DataUpdateType_t` enum value need confirming in-game.
- **Path B — proven mechanism, more code, non-instant.** Hook the client weapon-entity
  creation / `OnDataChanged` path and write the override *before* the natural composite build
  (mirroring `yuzhouUvU`'s `OnEntitySpawned`). No index needed, but it only takes visual effect on
  the next entity (re)creation — i.e. a demo seek / round change / weapon switch — not instantly.

Both still require a build + live verification to confirm; neither is provable from the static
binary alone.

## Live in-game verification (2026-06-28): mechanism found, data path works, composite regen is the wall

Built the tooling and drove it against a live demo (USP-S / SSG08 of a spectated pro). Artifacts
under `automation/output/ondatachanged_bisect/` (git-ignored). What was added:
`mirv_filmmaker cosmetics vtprobe <idx>` (calls `weapon->vtable[idx](this,0)` after marking visuals
stale, SEH-guarded); the `reloadevent` lever changed from `++` to **`= -1`**
(`CosmeticOverrideSystem.cpp`); and `automation/verify/verify-ondatachanged-bisect.ps1`. Findings,
in order of certainty:

1. **The OnDataChanged index hunt is solved: it is slot 11**, and the trigger condition is exactly
   what the static disassembly predicted. `C_CSWeaponBase` vtable slot 11 (`0x1807aa6d0`) chains to
   the C_EconEntity base, then — **only if `m_nCustomEconReloadEventId < 0`** — calls the composite
   manager's **`"clientside_reload_custom_econ"`** and stores a fresh reload id. Live, after
   `vtprobe 11`, the field went `-1 → 272` (a deterministic id), proving the reload block ran.
   **This also fixes a real bug: the repo's old `reloadevent` lever *incremented* the field
   (→ positive → the `jge skip` branch), so it could never trigger.** The correct value is `-1`.
2. **Slot 11 is safe to call; blind probing is not.** `vtprobe 4` (another DataUpdateType_t-taking
   virtual) returned `faulted=0` but the game crashed (`ResetBreakpadAppId`) immediately after —
   SEH catches access violations, not logic corruption. So the offset-xref that pinpointed slot 11
   (find the function that writes `m_nCustomEconReloadEventId @+0x18bc`) is what made this tractable
   without crash-bisecting nine candidates.
3. **The econ DATA path works end-to-end.** With the override correctly on the held weapon's slot
   (note: `weapon 0` = primary; a held USP-S is *secondary*, so use `weapon 61`) the live
   `m_NetworkedDynamicAttributes` def6 became our paint, the fallback fields updated, and — in
   fallback mode (`m_iItemIDHigh=-1`) — **the in-game HUD weapon name changed to the override skin**
   (USP-S → "Kill Confirmed" for paint 504). The data/identity/name layer fully consumes the write.
4. **But the rendered composite MATERIAL does not regenerate.** Despite the HUD name flipping and
   `def6`/fallback all reading our paint, the 3D view-model kept its *original* composited skin
   (USP-S stayed visually "Check Engine"; direct crop A/B of view-model pixels were identical).
   This held for `arg=0` and `arg=1`, paused and resumed, fallback on/off, and after
   `mat_reloadallmaterials` (no effect). Note `m_bVisualsDataSet` reads **0 even for a
   correctly-rendered stock skin**, so it is NOT the rebuild gate this doc earlier guessed.
5. **Runtime composite override IS achievable — proven by the engine's own dev lever.** `sv_cheats 1;
   cl_paintkit_override <N>` makes weapons render paint kit `<N>` (a freshly-deployed SSG08
   rendered the override finish; `mat_reloadallmaterials` alone did nothing). So the rendering path
   can be driven at runtime; it just isn't driven by an out-of-band econ edit + the reload call.
   `cl_paintkit_override` applies at composite-**build** time (weapon deploy/create) and is global
   (all weapons), confirming the composite is built from the econ item at create/deploy and then
   cached on the renderable — our per-frame poll writes *after* that build, and nothing invalidates
   the cached composite material instance.

**Net:** the "rebuild signature" the task asked for is found and wired (slot 11 / reloadId=-1 /
`clientside_reload_custom_econ`), and it correctly drives the econ identity + HUD name — but it does
**not** force a true composite-material regeneration on an already-built renderable. That last step
(the doc's original wall) is what `cl_paintkit_override` does and our path does not.

**Recommended next direction (Path C — most promising, supersedes A/B above):** locate the
`cl_paintkit_override` *read* site inside the composite-build path (the convar object is registered
at `client.dll` `0x18011cc9f`; find where its value is consulted during weapon visuals build) and
**hook it** to return the spectated/owning player's per-entity override paint instead of the global
convar value. That reuses the engine's proven, already-working composite-build path and makes it
per-player. Failing a hook, Path B (write the override *before* the natural create/deploy composite
build, via an entity-creation/`OnDataChanged` hook rather than the current too-late per-frame poll)
remains the fallback, but only takes effect on weapon (re)deploy.

### Follow-up (same session): every write-based approach exhaustively ruled out — the barrier is build-time timing

Pushed the investigation to a definitive conclusion. None of these render a per-player override (all
verified live with screenshot diffs):
- per-frame econ write (def6 + fallback) — HUD/name only, never the composite;
- slot-11 reload (`reloadId=-1` + `OnDataChanged`), `arg=0` and `arg=1`, fallback on/off, paused/resumed;
- visuals-invalidate (`m_bVisualsDataSet=0`) + reload;
- `mat_reloadallmaterials`;
- **seek-recreate with the override pre-written** (clean A/B at one tick: cosmetics-off real skin
  `def6=796` vs cosmetics-on `def6=504` after recreate → **weapon-region pixel diff max=24, i.e. no
  skin change**; HUD still "Check Engine").
- `cl_paintkit_override <N>` on an **already-deployed** weapon (in place) — **no change** (mean
  pixel diff 0.04); it only affects a weapon at its **next deploy**, and then **globally**.

**Root cause, now airtight:** the weapon's skin composite is built **once, synchronously, at entity
create/deploy, from the *authoritative networked econ item*** (the GC item behind `m_iItemIDHigh`,
paint 796 here) — **not** from our locally-edited `m_NetworkedDynamicAttributes` def6 — and is then
cached on the renderable. Our per-frame write lands *after* that build (the demo's full-update at a
seek re-sends 796 and builds before our `RunMainThreadFrame` overwrites to 504), and nothing
out-of-band invalidates the cached composite. `cl_paintkit_override` is the *only* lever that works
because it is consulted **inside the build**, overriding the value the build uses.

**Therefore a write-then-hope design can never render a per-player skin** — the only viable path is
to intercept the paint kit **at the build's read site** (exactly where `cl_paintkit_override` is
consulted) and return the per-entity override. That read site is masked behind CS2's modern
convar system (the convar object at `0x1823cfaf0` has **zero** value-field xrefs in `client.dll` —
reads go through the convar interface/handle, not a direct `[obj+off]` access), so locating it needs
either deeper convar-system RE or an IDA/Ghidra pass on the composite-input builder near
`client.dll` `0x1807307fd` (which collects `&m_nFallbackPaintKit`/seed/wear for the composite). The
fix is then a Detours/vtable hook at that point — the concrete, and only, remaining work.

## Mechanism

### What we know for certain (CS:GO source, MIT/leak — direct ancestor of CS2's econ system)

CS2's `C_EconEntity`/`C_EconItemView` schema is a near-literal continuation of CS:GO's
`CEconEntity`/`CEconItemView` (same field names: `m_nFallbackPaintKit`, `m_flFallbackWear`,
`m_nFallbackSeed`, `m_nFallbackStatTrak`, `m_AttributeManager` — confirmed by comparing
`AfxHookSource2/SchemaSystem.h:93-119` against the CS:GO leak's `econ_item_view.h`/`econ_entity.h`
field names). The CS:GO leak (`perilouswithadollarsign/cstrike15_src`, leaked source, used here
only as an architecture reference — Valve stripped the econ *schema data* before leaking but the
*calling code* around it is intact) shows the actual rebuild trigger:

`game/shared/cstrike15/weapon_csbase.cpp`, `CWeaponCSBase::OnDataChanged`:

```cpp
#ifdef CLIENT_DLL
	bool bFirstPersonSpectatedState = IsFirstPersonSpectated();
	if ( ( bFirstPersonSpectatedState && !m_bOldFirstPersonSpectatedState ) ||
		 ( !bFirstPersonSpectatedState && m_bOldFirstPersonSpectatedState ) )
	{
		bChangedCarryState = true;
	}

	if ( type == DATA_UPDATE_CREATED )
	{
		// this will trigger the custom material to start making itself (if needed) the weapon
		// will render with the original material for a few frames, then switch to the custom
		// material when it's ready
		UpdateCustomMaterial();
		UpdateOutlineGlow();
	}
	else if ( bChangedCarryState )
	{
		CheckCustomMaterial();
		UpdateOutlineGlow();
	}
#endif
```

(Source: `https://raw.githubusercontent.com/perilouswithadollarsign/cstrike15_src/master/game/shared/cstrike15/weapon_csbase.cpp`,
fetched this session.)

Key facts this establishes:
- **`UpdateCustomMaterial()` / `CheckCustomMaterial()` are private, NON-virtual member functions**
  of the weapon class (confirmed from the matching header,
  `https://raw.githubusercontent.com/perilouswithadollarsign/cstrike15_src/master/game/shared/cstrike15/weapon_csbase.h`:
  declared in a private `CLIENT_DLL`-only block alongside `bool m_bVisualsDataSet;`, no `virtual`
  keyword). **They are not reachable via a vtable call** — our repo's `SafeVCall`/vtable-index
  approach (`CosmeticOverrideSystem.cpp:50-61`) is targeting the wrong kind of function for this
  specific rebuild step. A vtable index can still happen to "work" if it accidentally lands on a
  different virtual (e.g. `UpdateVisibility`) that has a side effect, which would explain why the
  historical index 7 either faults or does nothing useful rather than cleanly fixing the skin.
- **The trigger is the entity lifecycle, not a per-frame field poll.** The rebuild fires from
  exactly two conditions: (a) `DATA_UPDATE_CREATED` — i.e. the entity was just (re)created on the
  client (this is what happens on weapon pickup / `!kill`+respawn / round start), or (b)
  `bChangedCarryState` — the weapon's carried/dropped state or first-person-spectated state
  flipped since the last network update. **Neither condition is satisfied by silently overwriting
  `m_nFallbackPaintKit`/`m_iItemIDHigh` on an already-fully-created, already-carried weapon from
  outside the network-update path** — which is exactly what `ApplyCosmeticWrite` does every frame.
  This is the root cause: the write is correct, but nothing downstream ever runs because the only
  two triggers that consume it never fire.
- **`m_bInitialized`/`m_bInitializedTags` are NOT material-rebuild flags.** The CS:GO leak's
  `CEconItemView::Init()` sets `m_bInitialized = true` then calls `MarkDescriptionDirty()` — this
  is the **inventory tooltip/description text cache**, unrelated to the 3D render path. Toggling
  these to `false` (as `ApplyCosmeticWrite` does at `CosmeticOverrideSystem.cpp:120-123`) is
  harmless but does not, and was never going to, cause a re-render — it would at most invalidate a
  cached UI string. This matches what the repo already observed empirically ("that ALONE does NOT
  visually rebuild the skin").

### Real-world corroboration (CS2-specific, current build)

Two independent live-CS2 data points corroborate the "lifecycle trigger, not per-frame" model
above:

1. A documented player-facing fact: pressing **Q (quick-switch)** is known to force the engine to
   redraw the `C_WeaponCSBase` model, used as a manual fix when a weapon skin/viewmodel desyncs
   after a fast `subclass_change`/skin-changer console spam — i.e. swapping weapons (which causes
   `m_iState`/carry-state and `WEAPON_NOT_CARRIED` transitions, exactly `bChangedCarryState` above)
   is an established empirical "make the skin redraw" trick.
2. `Nereziel/cs2-WeaponPaints` (`https://github.com/Nereziel/cs2-WeaponPaints`), the most widely
   deployed CounterStrikeSharp (server-side) skin plugin, documents in its own setup instructions
   that **knife skins require the player to `!kill` (die/respawn)** to correctly apply visually.
   Respawn destroys and recreates the weapon entity — i.e. it forces `DATA_UPDATE_CREATED` — which
   is precisely the first branch in `OnDataChanged` above. This is a CS2-current-build,
   independently-documented confirmation that the create/recreate path is what makes a changed
   skin actually render, even when the underlying server plugin already wrote the "right" econ
   attributes well before that point.

Both are server-plugin-adjacent observations (the skin data itself arrives over the network in
those cases), but the *visual-refresh trigger* they rely on is purely client-side entity lifecycle
behavior — the same client.dll code path this DLL would need to invoke or imitate.

### CS2 schema evidence (current-build-relevant, not CS:GO-only)

A live source2gen-style dump of current `client.dll` (`a2x/cs2-dumper`,
`https://github.com/a2x/cs2-dumper/blob/main/output/client_dll.hpp`, fetched this session) shows
`C_EconEntity` (via `cs2-sdk`'s mirror of the same generator,
`https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp`) carries a field **not
yet resolved in this repo's `SchemaSystem.h`**:

```cpp
class C_EconEntity : public C_BaseFlex
{
	...
	bool m_bAttributesInitialized;          // resolved in AfxHookSource2/SchemaSystem.h/.cpp
	C_AttributeContainer m_AttributeManager;
	uint32_t m_OriginalOwnerXuidLow;
	uint32_t m_OriginalOwnerXuidHigh;
	int32_t m_nFallbackPaintKit;
	int32_t m_nFallbackSeed;
	float m_flFallbackWear;
	int32_t m_nFallbackStatTrak;
	bool m_bClientside;
	bool m_bParticleSystemsCreated;
	...
};
```

`m_bAttributesInitialized` lives on **`C_EconEntity`** (the weapon/wearable entity itself), which
is a structurally different flag from `C_EconItemView::m_bInitialized` (the embedded item-view,
already wired up in `g_clientDllOffsets.C_EconItemView.m_bInitialized`). The repo now resolves it
and clears it during writes. Live testing still showed no visual rebuild from that lever alone, so
it is useful diagnostics/cleanup but not the complete refresh trigger.

Note also that the same current dump shows `C_EconItemView::m_iItemID` (a single `uint64`) where
this repo's `SchemaSystem.h:109-111` still resolves separate `m_iItemIDHigh`/`m_iItemIDLow`
(`uint32` each). Both namings may coexist across builds/dump tools (a `uint64` field and a
high/low pair can be the same bytes viewed two ways, or the schema may genuinely have changed) —
flagged here as a discrepancy to re-check with `misc/sigscan.py`/the schema dumper against the
actual running build, not something this research could resolve from dumps alone.

## How to locate the rebuild function on the current build

No reliable byte signature or current vtable index for `UpdateCustomMaterial`/`CheckCustomMaterial`
(or their CS2-renamed equivalent) was found in this research pass — this is the main gap. What
*is* concretely actionable, in order of effort:

1. **Resolve `C_EconEntity::m_bAttributesInitialized` via the schema system first (near-zero
   cost).** This repo already walks `client.dll`'s declared-class schema at runtime
   (`AfxHookSource2/SchemaSystem.cpp:13-72`, `getOffsetsFromSchemaSystem`) and the econ block
   (`initCosmeticsOffsets`, `SchemaSystem.cpp:143-179`) already runs the same non-fatal
   `getOffset(...)` pattern for every other `C_EconEntity` field. Add one more line:
   `getOffset(&g_clientDllOffsets.C_EconEntity.m_bAttributesInitialized, "client.dll",
   "C_EconEntity", "m_bAttributesInitialized")`. This is a schema **field**, so it resolves the
   same way the existing fields do — no sigscan needed. Then **read it (do not write it yet)** via
   the existing `cosmetics debug`/`Cosmetics_DebugWeapon`-style diagnostic
   (`AfxHookSource2/Filmmaker/Cosmetics/CosmeticDebug.cpp`) to see whether it ever flips to `true`
   on a freshly-spawned (not-yet-overridden) weapon, and whether clearing it back to `false`
   alongside the existing fallback-field write (then leaving it alone — NOT forcing it back to
   `true`) causes anything to change next frame. This is the cheapest possible experiment and
   needs no new sigscan infrastructure.
2. **If (1) does nothing**, the actual rebuild logic is native, non-virtual code in `client.dll`
   (per the CS:GO architecture above, it is plain member-function code, not a schema-driven
   system) and must be found by signature, not vtable index. Concretely:
   - Use `misc/sigscan.py` (already in the repo, byte-pattern-only, no symbol requirement) against
     a fresh `client.dll` dump.
   - The most promising starting point is **not** to search for `UpdateCustomMaterial` by name
     (it's stripped) but to find it **by xref from a known caller**: `C_BaseAnimGraph`/
     `C_BaseModelEntity::OnDataChanged` or the equivalent CS2 weapon-base `OnDataChanged`
     override. Engine vtables and RTTI-free Source 2 binaries do not expose this directly from a
     byte pattern alone, so the practical path is a disassembler (IDA/Ghidra) pass: locate
     `C_CSWeaponBase`'s (or its base's) `OnDataChanged` implementation — findable by xref from the
     generic entity `OnDataChanged` dispatch the engine already calls for every networked entity
     — and read what it calls when `m_iState`/carry-state changes. Once a stable byte sequence
     around the call site is identified (e.g. the instructions immediately preceding/following the
     call, which tend to be more stable across patches than a full function body), encode that as
     an AOB pattern the same way `getAddress(schemaSystemDll, "...")` is already used in
     `SchemaSystem.cpp:236` for the schema-system interface pointer.
   - **This is genuinely the deliverable this research could not produce**: no current-build
     signature, xref chain, or stable string reference for this function was found via web search
     in this pass. It requires either (a) someone with IDA/Ghidra access to `client.dll` doing the
     xref-from-`OnDataChanged` walk described above, or (b) finding a maintained public CS2
     reversing project that has already done this specific walk (searched this session:
     `a2x/cs2-dumper`, `sezzyaep/CS2-OFFSETS`, `NotOfficer/cs2-sdk`, `Aspasia1337/Aspasia`,
       `samyycX/awesome-cs2`'s linked tools — none publish function-level (as opposed to
       data-member-offset) resolution for this specific call).
3. **A cheaper, lower-confidence alternative**: imitate the trigger condition instead of calling
   the function directly. Since the real trigger is `bChangedCarryState`
   (`WEAPON_NOT_CARRIED` transition) or `DATA_UPDATE_CREATED`, and the engine's own networked
   update path drives those through the *normal* per-entity data-update dispatch (not something
   this DLL currently hooks — confirmed: grepped this repo for `OnDataChanged`/`UpdateVisibility`/
   `DATA_UPDATE_CREATED`, no matches, see `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp`
   and siblings), there is no existing hook point in this codebase to "fire it again" cheaply
   either. This option is listed for completeness but is not actually cheaper than (2) — it still
   needs the same xref work to find where the engine itself decides "carry state changed," just
   aimed at a different (state-comparison) call site instead of the material-build call site.

## Flag semantics (confirmed vs. hypothesis)

| Field | Class | Confirmed meaning | Confirmed effect of writing it |
|---|---|---|---|
| `m_iItemIDHigh = -1` | `C_EconItemView` | "Don't trust networked econ item; use my fallback fields" — this is the one part of the existing pipeline known-good (write sticks, matches CS:GO's "fallback when networked item missing" design intent). | **Necessary, not sufficient.** Confirmed by this session's own in-game test: write sticks every frame, skin still doesn't render. |
| `m_nFallbackPaintKit`/`m_flFallbackWear`/`m_nFallbackSeed`/`m_nFallbackStatTrak` | `C_EconEntity` | The actual values the (unknown, unreached) composite step is supposed to read once it runs. | Write sticks (confirmed). No visual effect observed because the consuming step never runs (per Mechanism section above). |
| `m_bInitialized` | `C_EconItemView` | **Confirmed (CS:GO leak): inventory description-text cache flag, set by `CEconItemView::Init()`, consumed by `MarkDescriptionDirty()`/`EnsureDescriptionIsBuilt()`.** Not part of the 3D render path. | Toggling it is harmless but inert for rendering — matches this repo's own prior empirical finding. |
| `m_bInitializedTags` | `C_EconItemView` | Hypothesis: a sibling flag for a second dirty-cache (e.g. "tag"/sticker description cache), by naming analogy with `m_bInitialized`. Not separately confirmed in the CS:GO leak excerpts fetched this session. | Same as above — no evidence it touches rendering. |
| `m_bAttributesInitialized` | `C_EconEntity` | Current-build schema field, now resolved by the repo. Plausibly the entity-level "have my attributes been consumed into visual/render state" flag. | Cleared during writes. Live testing showed it is insufficient by itself; no visible skin rebuild on an already-held default AK. |
| `m_NetworkedDynamicAttributes` | `C_EconItemView` | Confirmed present in current schema (already resolved, `AfxHookSource2/SchemaSystem.h`, `SchemaSystem.cpp`) — per the existing in-repo comment, this is where a *networked/spectated* item's live paint kit/wear/seed actually live, as opposed to `m_AttributeList` (the local/cooked list, empty for demo players). | The apply loop now overwrites existing def 6/7/8/81 values. It cannot currently create missing attributes, so a default AK with an empty networked list still falls back to the unresolved material-rebuild problem. |

**Is there a separate "composite generation token" that must be bumped?** Not confirmed either
way. CS2's offset dumps do show an unrelated `CompositeMaterial_t`/
`CompositeMaterialAssemblyProcedure_t` enum/struct family (seen in `cs2-dumper`'s output via the
`m_vecCompositeMaterialAssemblyProcedures`/`m_vecCompositeMaterials` offsets surfaced during this
search), but this session could not find the owning class or confirm it is even the same
"composite" as econ skin compositing (Source 2's general-purpose material-compositing system is
also used for unrelated things, e.g. procedural decal/wear systems) — flagged as a loose thread,
not a finding.

## Viewmodel vs. world model

**Per-`C_EconItemView`, not per-entity, but two separate entities each hold their own view.**
Confirmed from the CS:GO architecture (the same `C_EconEntity`/`m_AttributeManager.m_Item`
relationship CS2 inherited): a weapon's first-person viewmodel and its third-person world model
are typically driven by **two different client entities** that both reference the same underlying
econ item data (this repo's earlier model-override research, `docs/cosmetics-model-override-research.md`
§3, independently confirms CS2 has `C_BaseViewModel::m_hWeapon`/`m_hWeaponModel` as separate
handles, and that nSkinz on CS:GO explicitly set the model index on **both** `view_model` and
`world_model` as two separate writes — see that doc's §2). The practical implication for the
recompose problem specifically:

- If the real fix is "call the rebuild function on the weapon entity," it needs to run **once per
  visible representation** — i.e. potentially twice (viewmodel weapon entity and world-model
  weapon entity), not once. `CosmeticOverrideSystem::RunFrame`'s current loop
  (`CosmeticOverrideSystem.cpp:336-422`) walks **all** entities and applies to anything matching
  `LooksLikeWeaponEntity` — this would already iterate over both the viewmodel and world-model
  weapon entities if they are both present as separate indices in the entity list (consistent with
  how `m_OriginalOwnerXuidLow/High` ownership resolution already works generically per-entity in
  that loop). **No code change needed here** if the rebuild call is added at the same per-entity
  granularity the write already happens at — the existing loop structure is already correct for
  this requirement, assuming the viewmodel weapon and world-model weapon are in fact separate
  `C_EconEntity`-derived entities in CS2 (consistent with, but not separately re-confirmed in, the
  model-override doc's findings).
- This is **not separately confirmed for CS2's current build** (no live introspection of whether
  CS2 demo-spectator playback actually instantiates two distinct weapon entities for viewmodel vs.
  world model, vs. a single entity with the engine handling both renders from one econ item). The
  existing `Cosmetics_RunFrame` loop already iterating "every entity that looks like a weapon"
  means this should be moot in practice — whatever entity-level fix is found will naturally apply
  to both representations without additional code, *if* they are in fact separate entities; if
  CS2 uses a single entity for both, there is nothing extra to do at all.

## Recommended minimal change

In order of confidence/cost, smallest first:

1. **(Do first, ~10 minutes, near-zero risk)** Resolve `C_EconEntity::m_bAttributesInitialized` in
   `AfxHookSource2/SchemaSystem.h`/`.cpp` (add the field to the `C_EconEntity` struct and one
   `getOffset(...)` line in `initCosmeticsOffsets`, off the `ok` chain like `m_bInitialized`/
   `m_bInitializedTags` already are at `SchemaSystem.cpp:167-168`). Add it to the existing debug
   dump (`Cosmetics_DebugWeapon`/`CosmeticDebug.cpp`) read-only first. Then, as a cheap experiment,
   set it to `false` alongside the existing `offInit`/`offInitTags` writes in `ApplyCosmeticWrite`
   (`CosmeticOverrideSystem.cpp:120-123`) — same non-fatal, optional-offset pattern already used
   there (`if (offInit) ...`) — and observe in-game whether this alone (no vtable call at all)
   causes the skin to rebuild on the next attribute-consuming pass. **This is the single
   highest-expected-value, lowest-risk change this research identified**, precisely because it is
   a schema field write (the exact mechanism already proven to "stick" safely) rather than a
   speculative vtable/function call.
2. **If (1) doesn't work**, retire the vtable-index approach (`SafeVCall`/`m_vtComposite`/
   `m_vtCompositeSec`, `CosmeticOverrideSystem.cpp:50-61,416-421`) — per the CS:GO leak evidence,
   the rebuild function is non-virtual, so no vtable index is structurally correct for it, and the
   "index 7 faults" symptom is consistent with this (any non-zero index is calling an arbitrary,
   probably-unrelated, virtual function rather than the intended target). This is not itself a fix
   — it is a recommendation to stop spending effort tuning `vtidx` and instead invest in (3).
3. **The actual fix** is most likely a non-virtual function call found via sigscan/xref (see "How
   to locate" step 2 above) — i.e. extend `misc/sigscan.py`-style resolution
   (`AfxHookSource2/SchemaSystem.cpp:236`/`getAddress`-style AOB scanning, already used elsewhere
   in this DLL for the schema-system interface and other engine internals per `CLAUDE.md`'s
   "Engine integration via signature scanning" section) to resolve the weapon's
   `OnDataChanged`-driven custom-material rebuild function (CS2's renamed equivalent of
   `CheckCustomMaterial`/`UpdateCustomMaterial`), then call it directly by resolved address (not
   through any vtable) once per matching entity in the existing `Cosmetics_RunFrame` loop, guarded
   the same SEH-safe way `SafeVCall` already is. **No candidate signature was found in this research
   pass** — this step requires either manual IDA/Ghidra xref work against a live `client.dll` (see
   "How to locate," step 2) or a public reversing project covering this specific function, which
   this search did not turn up.

## Confidence / risk note

- **High confidence**: the existing fallback-field write mechanism is correct and necessary
  (matches CS:GO's designed-for-this fallback path 1:1, and is independently confirmed sticking in
  this session's live test).
- **High confidence**: `m_bInitialized`/`m_bInitializedTags` are inventory-description-cache
  flags, not render triggers — based on directly-read CS:GO leak source, not inference.
- **High confidence**: the real trigger is entity-lifecycle-based (`OnDataChanged` /
  `DATA_UPDATE_CREATED` / carry-state change), not a per-frame poll of the fallback fields — based
  on the same leak source plus two independent CS2-current-build corroborations (Q-switch redraw
  folklore, cs2-WeaponPaints' documented `!kill` requirement for knives).
- **High confidence**: the rebuild function itself is non-virtual in the CS:GO architecture, so a
  vtable-index approach is very unlikely to be the right mechanism even if some index happens not
  to fault.
- **Medium confidence / hypothesis**: `C_EconEntity::m_bAttributesInitialized` is the relevant
  current-build gate. This is an informed guess from the field's name and its class (entity-level,
  not item-view-level) — not confirmed by reading any function body, because no public source
  shows CS2's actual (renamed, refactored) attribute-consumption code. Cheap and safe to test
  directly (item 1 above) precisely because testing it costs nothing more than the same kind of
  write the existing pipeline already performs safely.
- **Low confidence / open problem**: no concrete signature, xref chain, or stable string reference
  for the actual rebuild function was found. This is the one piece of the original ask
  ("candidate signature for the composite function") this research could not deliver. Closing this
  gap needs either disassembler access to a live `client.dll` or a not-yet-found public reversing
  resource; this pass searched `a2x/cs2-dumper`, `sezzyaep/CS2-OFFSETS`, `NotOfficer/cs2-sdk`,
  `Aspasia1337/Aspasia`, `samyycX/awesome-cs2` and general web search without success.
- **Server-side techniques excluded as non-viable, confirmed**: CounterStrikeSharp/SwiftlyS2/
  Metamod skin plugins (`Nereziel/cs2-WeaponPaints`, `samyycX/WeaponSkins`,
  `K4ryuu/K4-AlwaysWeaponSkins`, `yuzhouUvU/cs2_weapons_skin`) all operate server-side and rely on
  the engine's own networked attribute-update path to drive the client's normal `OnDataChanged`
  dispatch — a mechanism categorically unavailable to a client-only DLL replaying an offline demo
  (no server is simulating the world during demo playback, per `CLAUDE.md`'s constraints). Their
  *existence/design* is useful evidence for the trigger mechanism (this doc relies on that), but
  none of their *code* is directly portable, since they never need to solve "force a rebuild
  without a network event" — the network event is what they use instead.

## Sources

- CS:GO leak (architecture reference only, MIT/leak terms apply to the original repo, used here
  for read-only research): `https://github.com/perilouswithadollarsign/cstrike15_src`
  - `game/shared/cstrike15/weapon_csbase.cpp` (`OnDataChanged`, `UpdateCustomMaterial`/
    `CheckCustomMaterial` call sites) — fetched raw via
    `https://raw.githubusercontent.com/perilouswithadollarsign/cstrike15_src/master/game/shared/cstrike15/weapon_csbase.cpp`
  - `game/shared/cstrike15/weapon_csbase.h` (private non-virtual declarations of
    `UpdateCustomMaterial`/`CheckCustomMaterial`)
  - `game/shared/econ/econ_item_view.cpp` (`CEconItemView::Init`, `MarkDescriptionDirty`,
    `EnsureDescriptionIsBuilt`, `Update`, `UpdateGeneratedMaterial`)
  - `materialsystem/custom_material.h` (`CCustomMaterial`/`CCustomMaterialManager` interface:
    `CheckRegenerate`, `RegenerateTextures`, `Finalize`, `GetMaterial`)
  - `game/client/cstrike15/cs_custom_weapon_visualsdata_processor.cpp`
    (`CCSWeaponVisualsDataProcessor::GenerateCompositeMaterialKeyValues`)
- CS2 current-build schema dumps (source2gen family, cross-referenced against each other):
  - `https://raw.githubusercontent.com/NotOfficer/cs2-sdk/master/client.hpp` —
    `C_EconEntity` full field list including `m_bAttributesInitialized`
  - `https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.hpp` —
    `C_EconItemView` field list (`m_bInitialized` @ 0x1E8, `m_bInitializedTags` @ 0x468,
    `m_NetworkedDynamicAttributes` @ 0x280, `m_iItemID` as single uint64 — note vs. this repo's
    `m_iItemIDHigh`/`m_iItemIDLow` split, flagged as unresolved discrepancy above)
  - `https://github.com/sezzyaep/CS2-OFFSETS` (cross-reference, not separately quoted)
- CS2-current corroboration (player/operator-facing, not source-level):
  - `https://github.com/Nereziel/cs2-WeaponPaints` — documented `!kill` requirement for knife
    skin application (entity-recreate-triggers-rebuild evidence)
  - Q-switch-forces-redraw folklore (general CS2 console-command community knowledge, surfaced via
    web search this session; no single authoritative URL — treat as corroborating anecdote, not a
    primary source)
- Local repo (current state, read this session):
  - `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp` (the apply loop;
    `SafeVCall`/`ApplyCosmeticWrite`/`RunFrame`)
  - `AfxHookSource2/SchemaSystem.h` / `AfxHookSource2/SchemaSystem.cpp` (`ClientDllOffsets_t`,
    `initCosmeticsOffsets`, `getOffsetsFromSchemaSystem`, the `getAddress`-based AOB scan used for
    the schema-system interface itself — the existing precedent for how a new sigscan would be
    wired in)
  - `misc/sigscan.py` (the repo's existing pattern-validation tool)
  - `docs/cosmetics-model-override-research.md` (the separate, previously-researched model-swap
    problem; cross-referenced for the viewmodel/world-model entity split and the
    `C_EconEntity`/`C_EconItemView` field-name continuity argument)

## Later reference comparison update (2026-06-29)

The practical implementation guidance now lives in `docs/cosmetics-cs2-methodology-notes.md`,
section 9, after reviewing all three local reference repos in
`reference/skin changer help/`. The newer takeaway is:

- The direct composite path remains necessary for weapon skins, but it is only one part of the visible
  update. The references also update item identity/cache flags, mesh group masks, viewmodels, HUD icon
  caches, and sometimes item descriptions.
- For demo playback, overwriting `m_NetworkedDynamicAttributes` works only when the demo entity already
  has def 6/7/8/81 attributes. If `attrWritten=0`, fallback fields alone are usually insufficient; we
  need an attribute-creation or `SetAttributeValueByName` fallback like the references.
- The deeper `real-time-internal-overlay-research-main` read adds one more likely cache gate:
  copied/synthetic item identity plus `C_EconItemView::m_bDisallowSOC = false`. Its source calls this
  flag write "the fix" before copying fake loadout item IDs into the live weapon item view. In demo
  mode we should not port fake inventory ownership, but we should test session-local synthetic item IDs
  and `m_bDisallowSOC=false` alongside the missing-attribute fallback.
- `real-time-internal-overlay-research-main` also proves StatTrak/nametag and knife viewmodel rebuilds
  are separate refresh surfaces: it calls `AddStattrakEntity()`, `AddNametagEntity()`, and hooks
  `C_BaseModelEntity::SetModel` to rewrite viewmodel knife models before the engine draws the default
  model. Composite refresh alone will not solve those visible surfaces.
- Default knives and gloves are special because they often have no paintable networked attribute vector
  and may not have target materials for the requested paint. Treat `attrWritten=0` as a first-class
  diagnostic result, not as "the composite call failed."
- Gloves and agents are not solved by weapon recomposition. Gloves are pawn `m_EconGloves` plus body
  group / HUD arms refresh; agents are pawn `SetModel`.
- The current demo architecture should stay per-SteamID/per-entity. Do not port the local-player
  inventory/loadout model wholesale.
