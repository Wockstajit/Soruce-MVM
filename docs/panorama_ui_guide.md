# Panorama UI architecture guide (CS2 filmmaker)

This is the general guide to how the filmmaker's Panorama UI works: first how it is
**actually implemented** in this repo, then the original design research (based on the
public structure found in Osiris and older AdvancedFX/HLAE Panorama work) that the
implementation follows. Offline/demo filmmaking only, not for live gameplay.

## As implemented in this repo (`AfxHookSource2/Filmmaker/Panorama/`)

The concepts below all shipped; the concrete shape is:

- **`PanoramaBridge.cpp`** — the small engine wrapper (`CUIEngine::RunScript`, attribute
  get/set, context-panel pinning). Engine access is resolved by byte-pattern scanning in
  `DeathMsg.cpp` (`getPanoramaAddrs*`), the single fragile file a CS2 update can break.
- **One panel per file pair:** each UI surface is a C++ bridge (`*Hud.cpp` / `FilmmakerMenu.cpp`)
  plus its embedded Panorama JS in an adjacent `*Js.h` header — `FilmmakerGuiJs.h` (the
  main-menu Demos page), `MovieHudJs.h`, `MarkerHudJs.h`, `CameraTimelineJs.h`,
  `CameraEditorJs.h` (+ per-section fragment headers `CameraEditor*Js.h`), `ConfigHudJs.h`,
  `GraphEditorJs.h`, `DemoBarButtonsJs.h`. The bridge builds the panel once, pushes a state
  JSON attribute per frame, and reads a command attribute back (JS emits
  `mirv_filmmaker ...` console commands or attribute writes).
- **`PanoramaFindPanel.{h,cpp}`** — the shared `Filmmaker::FindChildById` panel lookup
  (bounded recursive child search by id over the live engine panel tree). It used to be
  copy-pasted into every bridge file; new bridges must include this header instead of
  redefining it.
- **Threading rule (critical):** all Panorama work runs on the game's MAIN thread via
  `Filmmaker::RunMainThreadFrame`. Calling `RunScript` from the render thread crashes V8
  (`v8::Context::Exit() - Cannot exit non-entered context`).
- **Hard-won constraints:** no mouse-move event in HUD JS (build dragging from `Slider`
  panels); never set `style = ''` (clear colors with `rgba(0,0,0,0)`); MSVC's ~16 KB
  string-literal cap forces `)FMJS" R"FMJS(`-style splits in the `*Js.h` headers; netcon
  `ui_eval` truncates at 256 bytes, so drive the UI through predefined `$.Filmmaker.*`
  helpers.
- Native-UI reference material: the decompiled CS2 Panorama files live in `reference/`
  (check there before live-probing native panels over netcon).

Everything below is the original approach guide.

## Goal

Build UI that looks and behaves like native CS2 UI by using CS2's Panorama system instead of drawing a separate ImGui/DirectX overlay.

The target is:

```text
CS2 Panorama XML/CSS/JS style UI
Native-looking panels
Native controls such as buttons, sliders, dropdowns, text entries, preview panels
UI attached to existing CS2 menu or HUD roots
Commands sent back to your C++ filmmaking code
```

The target is not:

```text
Dear ImGui overlay
OBS/browser overlay
External window
Generic DirectX overlay
Online cheat menu
VAC-safe live-server tool
```

## High-level architecture

The Panorama path works like this:

```text
AfxHookSource2 / filmmaker DLL
  -> find or receive access to CS2's Panorama UI engine
  -> find an existing root panel, usually main menu, settings tab, or HUD root
  -> run custom Panorama JavaScript inside that panel context
  -> JavaScript creates real Panorama panels with $.CreatePanel
  -> panels reuse Valve's CSS classes and built-in panel types
  -> UI writes commands into a panel attribute
  -> C++ reads the command attribute each frame
  -> C++ applies the filmmaker feature
```

The important concept is that Panorama UI is not drawn manually. You ask CS2's own UI engine to create panels.

## Core pieces you need

### 1. Panorama UI engine wrapper

You need a small C++ wrapper around CS2's Panorama UI engine. The wrapper should expose only the few operations needed for UI creation and cleanup.

Conceptually:

```cpp
class PanoramaUiEngineBridge {
public:
    void runScript(CUIPanel* contextPanel, const char* scriptSource);
    CUIPanel* getPanelFromHandle(PanelHandle handle);
    PanelHandle getPanelHandle(CUIPanel* panel);
    CPanoramaSymbol makeSymbol(const char* name);
    void deletePanelByHandle(PanelHandle handle);
};
```

Do not start by building a huge wrapper. The first version only needs:

```text
runScript
getPanelFromHandle
getPanelHandle
deletePanelByHandle
makeSymbol
```

Osiris does this through `PanoramaUiEngine.h`, where it wraps `runScript`, `getPanelPointer`, `makeSymbol`, and panel deletion.

#### Verified CUIEngine interface (current Osiris master)

The conceptual wrapper above is correct, but the *exact* engine entry points matter when you wire
the bridge. These are taken verbatim from the current Osiris source
(`Source/CS2/Panorama/CUIEngine.h`) and supersede the looser pseudocode above:

```cpp
struct CUIEngine {
    using getPanelPointer      = CUIPanel* (*)(CUIEngine* thisptr, const PanelHandle* handle);
    using runScript            = void (*)(CUIEngine* thisptr, CUIPanel* contextPanel,
                                          const char* scriptSource,
                                          const char* originFile, std::uint64_t line);
    using makeSymbol           = CPanoramaSymbol (*)(CUIEngine* thisptr, int type, const char* text);
    using onDeletePanel        = void (*)(CUIEngine* thisptr, CPanel2D* panel);
    using RegisterEventHandler = void (*)(CUIEngine* thisptr, CPanoramaSymbol eventName,
                                          CUIPanel* panel, CUtlAbstractDelegate* handler);
};
```

Things to note that the earlier pseudocode got wrong or left out:

- **`runScript` takes two extra args** beyond `(contextPanel, scriptSource)`: an `originFile`
  string and a `line` number. These are only used for Panorama's error/console attribution, so you
  can pass a constant like `"filmmaker"` and `0`. You still must pass them or the call signature is
  wrong and the stack will be corrupted.
- **`makeSymbol` takes an `int type`** first argument (not just the name). Pass the symbol type the
  engine expects; the existing wrapper in this fork already resolves `makeSymbol`
  (`DeathMsg.cpp:115-128`, `:1316-1322`) — reuse it rather than re-deriving the type.
- **Cleanup goes through `onDeletePanel(engine, CPanel2D*)`**, not an ad-hoc free. This is the
  correct, engine-sanctioned deletion path; use it when tearing down the filmmaker panels on reload.
- **Panel event callbacks** (slider changed, text submit, dropdown changed) are attached with
  `RegisterEventHandler(engine, eventSymbol, panel, delegate)`. Osiris' handler shapes live in
  `Source/EntryPoints/GuiEntryPoints.h` (e.g. `hueSliderValueChanged`, `hueSliderTextEntrySubmit`,
  `dropDownSelectionChanged`). For the filmmaker tool you can avoid C++ delegates entirely by using
  Panorama's `onactivate`/`onvaluechanged` attributes in JS that write to the command-queue
  attribute (see "Command bridge" below) — simpler and update-resilient.

#### The CUIPanel wrapper

`CUIPanel` (`Source/CS2/Panorama/CUIPanel.h`) is the engine-side panel object. It wraps a
`CPanel2D* clientPanel` and exposes the handful of operations you need:

```text
setParent(CUIPanel* parent)
setVisible(bool)
getAttributeString(symbol, defaultValue) -> const char*
setAttributeString(symbol, value)
```

It also carries an `EPanelFlag` enum (visibility / "is from layout file" bits) and the panel id
(`m_pchID`). This fork already resolves `getAttributeString`/`setAttributeString`
(`DeathMsg.cpp:1289-1314`), which is the entire C++→JS / JS→C++ bridge you need.

#### How to resolve these at runtime

Modern Osiris does **not** hardcode vtable indices for these. It locates each function as a raw
function pointer via **pattern search** and caches it (its `HookContext` pattern-result system).
That is exactly the style this fork already uses in
`DeathMsg.cpp:getPanoramaAddrsFromClient()` — signature-scan `client.dll`, read a relative offset,
store the resolved pointer/offset. Add `runScript` (and, if needed, `getPanelPointer` /
`onDeletePanel`) the same way, keeping all of the fragile reverse-engineered scanning in that one
place so a CS2 update only ever requires fixing signatures in a single file.

### 2. Existing root panel lookup

You need a known CS2 panel to attach your UI into. Good targets:

```text
Main menu panel
Settings panel
HUD root panel
Demo playback HUD panel
```

For a first prototype, the main menu/settings route is easier than HUD injection because the settings menu already has useful styles and controls loaded.

The Osiris approach does this:

```text
1. Get main menu panel.
2. Force-load the CS2 settings tab.
3. Find the settings panel named JsSettings.
4. Run custom JS inside that panel.
5. Move the new custom panel under the main menu content container.
6. Add a navbar button that opens the custom tab.
```

The key idea is to reuse CS2's existing panel hierarchy instead of creating a totally separate root.

### 3. Embedded Panorama JavaScript

Your UI should be authored as Panorama JavaScript. You can embed it as a C++ string or load it from a resource file in development.

A simple embedded script would look like this:

```js
(function () {
  var root = $.CreatePanel('Panel', $.GetContextPanel(), 'FilmmakerMenuTab', {
    class: 'mainmenu-content__container',
    useglobalcontext: 'true'
  });

  root.visible = false;
  root.SetReadyForDisplay(false);

  var navbar = $.CreatePanel('Panel', root, 'FilmmakerNavbar', {
    class: 'content-navbar__tabs content-navbar__tabs--noflow'
  });

  var center = $.CreatePanel('Panel', navbar, 'FilmmakerNavbarCenter', {
    class: 'content-navbar__tabs__center-container'
  });

  var cameraButton = $.CreatePanel('RadioButton', center, 'camera_button', {
    group: 'FilmmakerNavBar',
    class: 'content-navbar__tabs__btn',
    onactivate: '$.Filmmaker.navigateToTab("camera");'
  });

  $.CreatePanel('Label', cameraButton, '', { text: 'Camera' });

  var content = $.CreatePanel('Panel', root, 'FilmmakerContent', {
    class: 'full-width full-height'
  });

  var cameraTab = $.CreatePanel('Panel', content, 'camera', {
    class: 'SettingsMenuTab'
  });

  var cameraTabContent = $.CreatePanel('Panel', cameraTab, '', {
    class: 'SettingsMenuTabContent vscroll'
  });

  $.Filmmaker = {
    rootPanel: root,

    addCommand: function (command, value) {
      var oldCommands = root.GetAttributeString('cmd', '');
      root.SetAttributeString('cmd', oldCommands + command + ' ' + value + '\n');
    },

    navigateToTab: function (tabId) {
      cameraTab.visible = false;
      cameraTab.SetReadyForDisplay(false);

      var panel = root.FindChildInLayoutFile(tabId);
      panel.visible = true;
      panel.SetReadyForDisplay(true);
    }
  };
})();
```

That gives you a real Panorama panel, not a rendered overlay.

## Attaching a tab to the main menu

For a native-looking main menu tab, use the same pattern as Osiris:

```js
(function () {
  var mainMenuContent = $('#JsMainMenuContent');
  var root = $('#FilmmakerMenuTab');

  root.SetParent(mainMenuContent);

  var navParent = $.GetContextPanel()
    .FindChildTraverse('MainMenuNavBarSettings')
    .GetParent();

  var openButton = $.CreatePanel('RadioButton', navParent, 'FilmmakerOpenMenuButton', {
    class: 'mainmenu-top-navbar__radio-iconbtn',
    group: 'NavBar',
    onactivate: 'MainMenu.NavigateToTab("FilmmakerMenuTab", "");'
  });

  $.CreatePanel('Image', openButton, '', {
    class: 'mainmenu-top-navbar__radio-btn__icon',
    src: 's2r://panorama/images/icons/ui/bug.vsvg'
  });
})();
```

For a filmmaker tool, replace the icon with a camera-like built-in icon if one exists in CS2's Panorama image assets.

## Reusing native CS2 styles

The native look mainly comes from using Valve's existing panel classes and controls.

Useful classes seen in the Osiris UI:

```text
mainmenu-content__container
mainmenu-top-navbar__radio-iconbtn
mainmenu-top-navbar__radio-btn__icon
content-navbar__tabs
content-navbar__tabs--dark
content-navbar__tabs--noflow
content-navbar__tabs__center-container
content-navbar__tabs__btn
full-width
full-height
vscroll
SettingsMenuTab
SettingsMenuTabContent
SettingsBackground
SettingsSectionTitleContianer
SettingsSectionTitleLabel
SettingsMenuDropdownContainer
HorizontalSlider
PopupButton
White
horizontal-separator
negativeColor
```

Useful built-in Panorama panel types:

```text
Panel
Label
Button
RadioButton
Image
Slider
TextEntry
CSGOSettingsEnumDropDown
MapItemPreviewPanel
MapPlayerPreviewPanel
```

These are what make the UI feel like CS2 instead of an overlay.

## Common UI helpers

### Section helper

```js
function createSection(parent, sectionName) {
  var background = $.CreatePanel('Panel', parent, '', {
    class: 'SettingsBackground'
  });

  var titleContainer = $.CreatePanel('Panel', background, '', {
    class: 'SettingsSectionTitleContianer'
  });

  $.CreatePanel('Label', titleContainer, '', {
    class: 'SettingsSectionTitleLabel',
    text: sectionName
  });

  var content = $.CreatePanel('Panel', background, '', {
    class: 'top-bottom-flow full-width'
  });

  return content;
}
```

### Separator helper

```js
function separator(parent) {
  $.CreatePanel('Panel', parent, '', {
    class: 'horizontal-separator'
  });
}
```

### Dropdown helper

```js
function createDropDown(parent, labelText, commandPrefix, feature, options) {
  var container = $.CreatePanel('Panel', parent, '', {
    class: 'SettingsMenuDropdownContainer'
  });

  $.CreatePanel('Label', container, '', {
    class: 'half-width',
    text: labelText
  });

  var dropdown = $.CreatePanel('CSGOSettingsEnumDropDown', container, feature, {
    class: 'PopupButton White'
  });

  for (var i = 0; i < options.length; ++i) {
    dropdown.AddOption($.CreatePanel('Label', dropdown, String(i), {
      value: i,
      text: options[i]
    }));
  }

  dropdown.SetPanelEvent('oninputsubmit', function () {
    $.Filmmaker.addCommand('set', commandPrefix + '/' + feature + '/' + dropdown.GetSelected().id);
  });
}
```

### Slider helper

```js
function createSlider(parent, name, commandPrefix, id, min, max) {
  var container = $.CreatePanel('Panel', parent, '', {
    class: 'SettingsMenuDropdownContainer'
  });

  $.CreatePanel('Label', container, '', {
    class: 'half-width',
    text: name
  });

  var sliderContainer = $.CreatePanel('Panel', container, id, {
    style: 'vertical-align: center; horizontal-align: right; flow-children: right; margin-right: 8px;'
  });

  var slider = $.CreatePanel('Slider', sliderContainer, '', {
    class: 'HorizontalSlider',
    style: 'width: 200px; vertical-align: center;',
    direction: 'horizontal'
  });

  slider.min = min;
  slider.max = max;
  slider.increment = 1.0;

  var textEntry = $.CreatePanel('TextEntry', sliderContainer, id + '_text', {
    maxchars: '3',
    textmode: 'numeric',
    style: 'width: 75px; margin-left: 10px; padding-left: 10px; text-align: center; font-size: 20px; color: #ccccccff; font-weight: bold; font-family: Stratum2, notosans, Arial; border: 2px solid #cccccc15;'
  });

  slider.SetPanelEvent('onvaluechanged', function () {
    $.Filmmaker.addCommand('set', commandPrefix + '/' + id + '/' + Math.floor(slider.value));
  });

  textEntry.SetPanelEvent('ontextentrysubmit', function () {
    $.Filmmaker.addCommand('set', commandPrefix + '/' + id + '_text/' + textEntry.text);
  });
}
```

## Command bridge from JS to C++

The simplest bridge is a string attribute on your root panel.

JavaScript writes commands:

```js
$.Filmmaker.addCommand('set', 'camera/sway/25');
```

That writes:

```text
set camera/sway/25
```

C++ reads it each frame:

```cpp
void FilmmakerPanoramaGui::run()
{
    auto panel = uiEngine.getPanelFromHandle(guiPanelHandle);
    if (!panel)
        return;

    const auto cmdSymbol = uiEngine.makeSymbol("cmd");
    const char* commands = panel.getAttributeString(cmdSymbol, "");

    dispatchCommands(commands);

    panel.setAttributeString(cmdSymbol, "");
}
```

Command parser shape:

```text
set camera/sway/25
set camera/fov/90
set demo/playback_speed/50
open campath/editor
close filmmaker
```

Recommended command format:

```text
verb section/feature/value
```

Examples:

```text
set camera/sway/25
set viewmodel/fov/68
set hud/xray/1
button campath/add_keyframe
button recording/start
button recording/stop
```

Keep this bridge boring and explicit. Do not let arbitrary Panorama JS execute arbitrary console commands.

## Cleanup

On unload, remove your UI panels and global JS object.

C++ cleanup:

```cpp
void FilmmakerPanoramaGui::onUnload()
{
    uiEngine.deletePanelByHandle(guiButtonHandle);
    uiEngine.deletePanelByHandle(guiPanelHandle);

    if (auto settings = uiEngine.getPanelFromHandle(settingsPanelHandle))
        uiEngine.runScript(settings, "delete $.Filmmaker;");
}
```

Why cleanup matters:

```text
Prevents duplicate buttons after reload
Prevents stale JS state
Prevents dangling panel handles
Makes development reloads tolerable
```

## Development layout

Recommended repo layout:

```text
AfxHookSource2/
  FilmmakerPanorama/
    FilmmakerPanoramaGui.h
    FilmmakerPanoramaGui.cpp
    FilmmakerPanoramaBridge.h
    FilmmakerPanoramaBridge.cpp
    FilmmakerPanoramaCommands.h
    FilmmakerPanoramaCommands.cpp

resources/
  filmmaker_panorama/
    filmmaker_gui.js
    filmmaker_styles.css
    icons/
```

For early development, load JS from disk so you can edit quickly. For releases, embed it as a resource or generated header.

Recommended dev command:

```text
mirv_filmmaker_panorama reload
```

That should:

```text
1. Delete old FilmmakerMenuTab and FilmmakerOpenMenuButton.
2. Re-run filmmaker_gui.js.
3. Rebind panel handles.
4. Reapply config values into controls.
```

## Minimal first milestone

Build the smallest useful proof of concept:

```text
1. Find main menu panel.
2. Run a small JS script through Panorama.
3. Create FilmmakerMenuTab.
4. Add one navbar button.
5. Add one Camera tab.
6. Add one slider named Camera Sway.
7. Slider writes set camera/sway/value to root panel attribute.
8. C++ reads the attribute and prints the parsed command to HLAE console.
9. Unload removes the panel and button cleanly.
```

Do not start with a full UI. First prove that native panel creation, command passing, config sync, and cleanup work.

## Suggested filmmaker UI tabs

After the first proof of concept works:

```text
Camera
  Camera sway
  Smooth follow
  FOV
  Roll
  Speed presets
  Attach to player or weapon attachment

Campath
  Add keyframe
  Remove keyframe
  Play path
  Ease type
  Export path
  Import path

Demo
  Play/pause
  Playback speed
  Jump tick
  Previous/next kill
  Previous/next round

Viewmodel
  Preview FOV
  Viewmodel offset
  Weapon/knife preview for offline filming

HUD
  X-ray toggle
  Clean HUD toggle
  Killfeed options
  Player labels

Recording
  Start take
  Stop take
  Output folder
  Motion blur / depth / streams status
```

## Applying config values back into UI

After the UI exists, C++ should push current config values into the controls.

Example:

```cpp
void FilmmakerPanoramaGui::updateFromConfig()
{
    auto root = uiEngine.getPanelFromHandle(guiPanelHandle);
    if (!root)
        return;

    setSliderValue(root.findChildInLayoutFile("camera_sway"), config.cameraSway);
    setDropdownValue(root.findChildInLayoutFile("camera_sway_enable"), config.cameraSwayEnabled ? 0 : 1);
}
```

The important rule:

```text
JS creates controls.
C++ owns config truth.
C++ pushes config into controls after creation or reload.
JS only reports user changes.
```

## Preview panels

CS2 has useful native 3D preview panel types:

```text
MapItemPreviewPanel
MapPlayerPreviewPanel
```

Osiris uses `MapItemPreviewPanel` for the viewmodel preview and `MapPlayerPreviewPanel` for player model previews.

A generic weapon preview panel looks like:

```js
var preview = $.CreatePanel('MapItemPreviewPanel', parent, 'WeaponPreview', {
  map: 'ui/xpshop_item',
  camera: 'camera_weapon_0',
  'require-composition-layer': true,
  player: false,
  initial_entity: 'item',
  mouse_rotate: false,
  sync_spawn_addons: true,
  'transparent-background': true,
  'pin-fov': 'vertical',
  style: 'width: 700px; height: 400px;'
});

preview.SetHideStaticGeometry(true);
```

For filmmaker work, preview panels are useful for:

```text
Viewmodel FOV preview
Weapon/knife preview
Player model preview
Agent/skin preview for offline film setup
```

## Main risks

### Updates break internal access

Panorama JS code is fairly stable. The C++ access to the UI engine and panel objects is the fragile part.

Keep the fragile layer small:

```text
PanoramaUiEngineBridge
PanoramaUiPanel wrapper
Panel handle wrapper
Style property wrapper only if needed
```

Do not scatter raw internal calls across feature code.

### CS2 class names can change

Valve UI class names may change. Keep them in one JS/CSS layer so UI fixes do not touch C++.

### Duplicate panels during reload

Always delete existing panels before creating new ones.

### JS command injection

Do not execute arbitrary strings from Panorama. Parse a small command grammar and reject unknown commands.

### Online use

This belongs in offline/demo/HLAE use. Do not design it for VAC servers.

## Best implementation strategy for HLAE

Recommended plan:

```text
Phase 1: Research branch
  Add FilmmakerPanoramaBridge.
  Add runScript proof of concept.
  Create one tab and one slider.
  Print commands only.

Phase 2: Safe UI framework
  Add reload command.
  Add cleanup.
  Add config sync.
  Add command parser.
  Add error logging.

Phase 3: Filmmaker controls
  Camera tab.
  Demo tab.
  Campath tab.
  HUD tab.

Phase 4: HUD integration
  Try attaching panels under HUD root for demo playback.
  Keep main menu settings tab working as fallback.

Phase 5: Polish
  Use CS2-like spacing, icons, labels, and preview panels.
  Move large JS into resources.
  Add build stamp and debug panel.
```

## What to copy from Osiris conceptually

Copy these ideas:

```text
Use CS2's Panorama engine.
Run custom JS in an existing context panel.
Create panels with $.CreatePanel.
Reuse Valve CSS classes.
Use a root panel attribute as a command queue.
Have C++ poll and clear that command queue each frame.
Clean up panels on unload.
Use MapItemPreviewPanel and MapPlayerPreviewPanel for native previews.
```

Do not copy these parts blindly:

```text
Cheat feature hooks
Live gameplay features
Injection instructions
Pattern signatures
Anti-cheat related assumptions
Large global feature architecture
```

## Source references used for this guide

Public files inspected:

```text
Osiris README
https://github.com/danielkrupinski/Osiris/blob/master/README.md

Osiris Panorama GUI wrapper
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/PanoramaGUI.h

Osiris embedded Panorama JavaScript
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/CreateGUI.js

Osiris Panorama UI engine wrapper
https://github.com/danielkrupinski/Osiris/blob/master/Source/GameClient/Panorama/PanoramaUiEngine.h

Osiris panel wrapper
https://github.com/danielkrupinski/Osiris/blob/master/Source/GameClient/Panorama/PanoramaUiPanel.h

Osiris command bridge
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/PanoramaCommandDispatcher.h
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/SetCommandHandler.h

Osiris viewmodel preview panel
https://github.com/danielkrupinski/Osiris/blob/master/Source/UI/Panorama/Tabs/VisualsTab/ViewmodelModPreviewPanel.h

Osiris init and per-frame run location
https://github.com/danielkrupinski/Osiris/blob/master/Source/EntryPoints/EntryPoints.h
```

## One-sentence summary

To make native-looking CS2 UI, build a tiny C++ Panorama bridge, run custom JS inside an existing CS2 panel, create real Panorama panels with Valve classes and controls, then bridge UI actions back to your filmmaker code through a small command queue.
