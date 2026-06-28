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
