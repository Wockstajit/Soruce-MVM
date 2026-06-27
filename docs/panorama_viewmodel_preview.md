# CS2 Panorama Weapon / Viewmodel 3D Preview

This file explains how CS2 renders a **live 3D weapon + arms (viewmodel) model inside a Panorama
panel** — the kind of preview shown in the Settings ▸ Visuals ▸ Viewmodel tab (knife + hands on the
left, controls on the right). It is the companion to `panorama_ui_guide.md`, which covers creating
the surrounding UI. The findings here are distilled from CS2's native Panorama panel types and the
way Osiris drives them (`Source/CS2/Panorama/CUI_3dPanel.h`, `CUI_Item3dPanel.h`, and the older
`Source/UI/Panorama/Tabs/VisualsTab/ViewmodelModPreviewPanel.h`).

## The short version

You do **not** render the weapon yourself. CS2 already has native Panorama panel types that own a
small offscreen 3D scene ("portrait world") and render a model into the panel's texture. You:

1. Create a native 3D item panel from JS (`MapItemPreviewPanel`).
2. Point it at a weapon/item by **item definition id** (which weapon) plus an **attributes** string
   (skin / wear / stattrak / etc).
3. Let its built-in camera (`camera_weapon_0`) frame the weapon; optionally trigger a "look at"
   animation.

The arms/hands come from the viewmodel scene the preview world loads — they are part of the
`camera_weapon` portrait setup, not something you add separately.

## The native panel types

```text
CPanel2D                      // base Panorama panel
  └── CUI_3dPanel             // a panel that owns a 3D portrait scene + camera
        └── CUI_Item3dPanel   // a 3D panel specialized for econ items (weapons/knives/gloves)
```

### CUI_3dPanel (`Source/CS2/Panorama/CUI_3dPanel.h`)

The generic "render a 3D scene into this panel" type. Relevant members:

```cpp
struct CUI_3dPanel : CPanel2D {
    using PortraitWorld = CCS_PortraitWorld*;  // the offscreen mini-scene that holds the model + lights
    using Fov           = float;               // camera field of view
    using FovWeight     = float;               // blend weight when interpolating FOV
};
```

`CCS_PortraitWorld` is the self-contained scene: it owns the model entity, lighting, and the camera
definitions (e.g. `camera_weapon_0`). The panel renders that world to a composition layer and draws
it as the panel's content. This is the same machinery the main menu uses for agent/loadout previews.

### CUI_Item3dPanel (`Source/CS2/Panorama/CUI_Item3dPanel.h`)

Specializes `CUI_3dPanel` for economy items. Relevant members:

```cpp
struct CUI_Item3dPanel : CUI_3dPanel {
    // Load a specific item (weapon/knife/glove) into the scene.
    //   itemId     : the item id to display (econ item / definition reference)
    //   attributes : extra attributes string (paint kit, wear, seed, stattrak, name tag, ...)
    using SetItemItemId    = void (*)(CUI_Item3dPanel* thisptr, ItemId itemId, const char* attributes);

    // Animate the camera to "look at" the loaded weapon (the subtle inspect framing).
    using StartWeaponLookAt = bool (*)(CUI_Item3dPanel* thisptr);
};
```

- **`SetItemItemId`** is how you choose *which* weapon and *how it looks*. The first arg selects the
  weapon/knife/glove; the `attributes` string carries the cosmetic state (paint kit id, float/wear,
  pattern seed, StatTrak, name tag). This is what makes the preview show a specific skinned knife
  rather than a default model.
- **`StartWeaponLookAt`** kicks off the camera framing/inspect animation onto the weapon.

## Driving it from Panorama JS (recommended)

For a filmmaker tool you almost never need to call the C++ methods directly. The native panel is
creatable and configurable straight from JS with attributes, which is far more update-resilient:

```js
var preview = $.CreatePanel('MapItemPreviewPanel', parent, 'WeaponPreview', {
  map: 'ui/xpshop_item',          // the preview "world" / scene to load
  camera: 'camera_weapon_0',      // the weapon framing camera defined in that world
  'require-composition-layer': true,
  player: false,                  // false = item/weapon preview (not a full player)
  initial_entity: 'item',
  mouse_rotate: false,            // set true to let the user spin the model
  sync_spawn_addons: true,
  'transparent-background': true, // so it composites over your panel background
  'pin-fov': 'vertical',
  style: 'width: 700px; height: 400px;'
});

preview.SetHideStaticGeometry(true); // hide the map/world geometry, keep just the model + arms
```

Notes:

- `MapItemPreviewPanel` is the **XML/JS tag name**; `CUI_Item3dPanel` is the **C++ type** behind it.
- `map: 'ui/xpshop_item'` + `camera: 'camera_weapon_0'` is the weapon-with-arms framing (this is the
  pairing the store / viewmodel previews use). The arms/hands are part of that portrait world.
- `SetHideStaticGeometry(true)` removes the surrounding scene geometry so you get a clean
  weapon + hands on a transparent background, exactly like the screenshot.
- To preview a **full player model** instead of a weapon, use the sibling type
  `MapPlayerPreviewPanel` (backed by the player portrait world) — same idea, different scene/camera.

## Choosing the weapon / skin shown

Two ways, depending on how much control you need:

1. **Attribute-driven (simplest):** set the item attributes on the panel in JS so CS2 resolves the
   model itself. Good for "show the currently equipped knife / loadout weapon."
2. **Explicit (`SetItemItemId`):** resolve the panel's C++ `CUI_Item3dPanel*` (via your Panorama
   bridge / handle) and call `SetItemItemId(itemId, attributes)` to force a specific weapon + paint
   kit, then `StartWeaponLookAt()` to frame it. Use this when you want to drive the preview from
   filmmaker config (e.g. "preview the AK with this skin and wear for offline filming").

## Why this is useful for a filmmaker tool

- **Viewmodel FOV preview** — show the live arms/weapon while the user drags an FOV slider.
- **Weapon / knife / glove preview** — pick the exact item + skin to film with, offline.
- **Loadout / agent preview** — `MapPlayerPreviewPanel` for player models and agents.
- All of it is native CS2 rendering inside a real Panorama panel — no separate overlay, no manual
  DirectX, and it composites correctly with the rest of your UI.

## Risks / notes

- The C++ method signatures (`SetItemItemId`, `StartWeaponLookAt`) and panel layout are the fragile,
  reverse-engineered part. Keep them isolated in the Panorama bridge layer (see
  `panorama_ui_guide.md` → "How to resolve these at runtime"); the JS attribute names are far more
  stable across CS2 updates.
- The portrait-world camera names (`camera_weapon_0`) and preview map (`ui/xpshop_item`) are Valve
  assets and could be renamed by an update — keep them in the JS layer so a fix never touches C++.
- This is for offline / demo / menu use, consistent with the rest of the HLAE filmmaker design.

## Source references

```text
CS2 native 3D panel types (current Osiris master):
  Source/CS2/Panorama/CUI_3dPanel.h
  Source/CS2/Panorama/CUI_Item3dPanel.h

Older Osiris viewmodel preview that drove MapItemPreviewPanel from JS:
  Source/UI/Panorama/Tabs/VisualsTab/ViewmodelModPreviewPanel.h   (pre-refactor)

Companion UI-creation guide:
  docs/panorama_ui_guide.md
```
