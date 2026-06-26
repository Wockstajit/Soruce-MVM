#pragma once

// Panorama JS for CAMERA EDITOR MODE: a docked editor workspace built ONCE into the
// in-game HUD (CSGOHud) context by CameraEditorHud.cpp.
//
// The chrome frames the live game as a "preview" in the top-left and fills the rest of
// the screen with editor panels:
//   * a RIGHT inspector column (selected-camera/property controls), and
//   * a BOTTOM backdrop behind the existing CameraTimeline panel (which renders in its
//     own root at a higher z-index, so it sits on top of this backdrop).
// The preview frame emerges automatically from the inspector's left accent border and
// the backdrop's top accent border (the backdrop border is clipped by the inspector
// where they meet, giving an L-shaped frame around the top-left preview rect).
//
// The gameplay HUD hide lives in CameraTimelineJs (driven by its "hosted" state flag),
// so a single script owns native-HUD visibility; this script only draws chrome.
//
// All controls issue the EXISTING "mirv_filmmaker camtl ... / marker ..." console
// commands -- the same back-end as the hotkeys and the timeline. Per the Panorama input
// constraint (in-game HUD JS has no mouse-move event) every draggable control is a
// native Slider panel.
//
//   C++ -> JS : attribute "state" (camera readouts + selected-key settings), then
//               $.CamEditor.render().
//   JS  -> C++: buttons / sliders issue console commands.

namespace Filmmaker {

inline const char* kCameraEditorJs = R"EDJS(
(function () {
  try {
    var existing = $('#CamEditorRoot'); if (existing) existing.DeleteAsync(0);
    var existingDbg = $('#CamEditorDebugRoot'); if (existingDbg) existingDbg.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', freeze: '#4aa3ffff', danger: '#c92a2acc',
      bg: '#0c0e12ff', bgSoft: '#14181eff',
      panelBorder: '#ffffff1f', sectionBg: '#1a1f26ff',
      label: '#9aa4b0ff', value: '#eef2f6ff', dim: '#6b7480ff',
      btnBg: '#ffffff14', btnOn: '#f0b32333',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var INSPECTOR_W = 430, BOTTOM_H = 176, BOTTOM_LIFT = 28;
    var TAB_BAR_H = 34, TAB_GAP = 4; // bottom-editor tab bar (DIVIDER above the active editor) height + gap
    var NATIVE_BAR_H = 96;  // fallback height for CS2's native demo bar (measured at runtime when shown)
    var GRAPH_DOCK_H = 556; // experimental graph-editor dock height; when open it REPLACES the timeline
                            // and fills the whole bottom (keep in sync with GraphEditorJs DOCK_H).
    var CH_ROLL = 5, CH_FOV = 6;
    var dbgGeom = null; // [debug overlay] last preview geometry stashed by render() (px + fractions)

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[editor] cmd failed: ' + e + '\n'); }
    }

    // ---- small builders -------------------------------------------------
    function mk(type, parent, props) { return $.CreatePanel(type, parent, '', props || {}); }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color; if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font; return l;
    }
    function btn(parent, text, onClick, color) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.backgroundColor = S.btnBg; b.style.borderRadius = '3px';
      b.style.paddingTop = '6px'; b.style.paddingBottom = '6px';
      b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
      b.style.marginRight = '5px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, 14); l.style.fontWeight = 'bold';
      b.SetPanelEvent('onactivate', onClick);
      b.__lbl = l; return b;
    }
    function section(parent, title) {
      var s = mk('Panel', parent); s.style.flowChildren = 'down'; s.style.width = '100%';
      s.style.marginTop = '12px'; s.style.paddingTop = '8px'; s.style.paddingBottom = '10px';
      s.style.paddingLeft = '10px'; s.style.paddingRight = '10px';
      s.style.backgroundColor = S.sectionBg; s.style.borderRadius = '4px';
      var t = lbl(s, title, S.dim, 12); t.style.fontWeight = 'bold'; t.style.letterSpacing = '2px';
      t.style.marginBottom = '6px';
      return s;
    }
    function row(parent) {
      var r = mk('Panel', parent); r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '4px'; r.style.verticalAlign = 'middle'; return r;
    }
    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : 0; }
    function finitePositive(v) { return typeof v === 'number' && isFinite(v) && v > 1; }
    // ---- Custom dropdown ------------------------------------------------
    // The embedded HLAE Panorama host does NOT instantiate the native DropDown popup: the control
    // toggles its "DropDownMenuVisible" class but never creates a DropDownMenu panel (verified live
    // over netcon -- zero menus exist in the whole tree while "open"), so nothing ever shows. So a
    // tappable FIELD bar drives our OWN floating popup, parented to the editor root at high z-index
    // (above the opaque inspector, not clipped by its scroll-overflow). Only one popup open at once.
    var customDrops = [], openDrop = null;
    function closeAllDrops() {
      if (!openDrop) return;
      openDrop.pop.visible = false; openDrop.caret.text = '▾'; openDrop = null;
    }
    // Bar styling: distinct fill + border so it reads as clickable. 'empty' = dim/disabled.
    function fieldStyle(field, empty) {
      field.style.height = '30px'; field.style.borderRadius = '4px';
      field.style.backgroundColor = empty ? '#11151b' : '#1b2230';
      field.style.border = '1px solid ' + (empty ? '#ffffff12' : '#ffffff2e');
    }
    // Place the popup directly under its field and show it. dbgAbs() walks actual offsets up to ctx
    // in physical px; divide by uiscale for style px. The popup is a child of root (full-screen at
    // 0,0), so ctx-relative coords == root-relative.
    function showDropPopup(rec) {
      var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1, sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var a = dbgAbs(rec.field);
      var x = a.x / sx, y = a.y / sy;
      var w = (rec.field.actuallayoutwidth || 0) / sx, h = (rec.field.actuallayoutheight || 0) / sy;
      rec.pop.style.position = Math.round(x) + 'px ' + Math.round(y + h + 2) + 'px 0px';
      rec.pop.style.width = Math.round(w) + 'px';
      rec.pop.visible = true; rec.caret.text = '▴'; openDrop = rec;
    }
    function toggleDrop(rec) {
      var wasOpen = (openDrop === rec);
      closeAllDrops();
      if (!wasOpen && rec.opts.length) showDropPopup(rec);
    }
    function rebuildPopup(rec) {
      rec.pop.RemoveAndDeleteChildren();
      for (var i = 0; i < rec.opts.length; i++) (function (o) {
        var meta = o[2] || {};
        var prow = mk('Panel', rec.pop); prow.hittest = true; prow.style.width = '100%';
        prow.style.paddingTop = '6px'; prow.style.paddingBottom = '6px';
        prow.style.paddingLeft = '12px'; prow.style.paddingRight = '12px';
        if (meta.selected) prow.style.backgroundColor = S.btnOn;
        var t = lbl(prow, o[0], meta.color || (meta.selected ? S.accent : '#e6eaef'), 14);
        t.hittest = false; t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.width = '100%';
        prow.SetPanelEvent('onactivate', function () { closeAllDrops(); rec.onPick(o[1]); });
      })(rec.opts[i]);
    }
    // Build a custom dropdown. onPick(value) fires on selection. Same .update() contract as before:
    //   update(opts, selValue [, sigOverride] [, displayOverride])
    // opts items are [label, value] or [label, value, {selected, color}]. The collapsed bar text
    // updates every frame (cheap); the popup ROWS only rebuild when sig changes, so live values in
    // labels (e.g. distance) don't tear down an open popup each tick.
    function customDrop(parent, id, onPick) {
      var field = mk('Panel', parent); field.id = id; field.hittest = true;
      field.style.width = 'fill-parent-flow(1.0)'; field.style.flowChildren = 'right';
      field.style.verticalAlign = 'center'; fieldStyle(field, true);
      var disp = lbl(field, '', '#e6eaef', 14); disp.hittest = false;
      disp.style.width = 'fill-parent-flow(1.0)'; disp.style.textAlign = 'center';
      disp.style.verticalAlign = 'center'; disp.style.whiteSpace = 'nowrap'; disp.style.textOverflow = 'ellipsis';
      disp.style.paddingLeft = '20px'; // balance the right-side caret so text reads centered
      var caret = lbl(field, '▾', '#e6eaef', 11); caret.hittest = false;
      caret.style.verticalAlign = 'center'; caret.style.marginRight = '10px'; caret.style.marginLeft = '4px';
      var pop = mk('Panel', root); pop.visible = false; pop.hittest = true;
      pop.style.position = '0px 0px 0px'; pop.style.zIndex = '400'; pop.style.flowChildren = 'down';
      pop.style.backgroundColor = '#0e1620fa'; pop.style.border = '1px solid ' + S.accent;
      pop.style.borderRadius = '4px'; pop.style.paddingTop = '3px'; pop.style.paddingBottom = '3px';
      pop.style.overflow = 'squish scroll'; pop.style.maxHeight = '340px';
      pop.style.boxShadow = '#000000d0 0px 4px 14px 2px';
      var rec = { field: field, disp: disp, caret: caret, pop: pop, onPick: onPick, opts: [], sig: '' };
      field.SetPanelEvent('onactivate', function () { toggleDrop(rec); });
      customDrops.push(rec);
      return {
        dd: field,
        update: function (opts, selValue, sigOverride, displayOverride) {
          rec.opts = opts || [];
          fieldStyle(field, rec.opts.length === 0);
          var disptext = displayOverride;
          if (disptext == null) {
            disptext = rec.opts.length ? rec.opts[0][0] : '—';
            for (var d = 0; d < rec.opts.length; d++) if (rec.opts[d][1] === selValue) disptext = rec.opts[d][0];
          }
          rec.disp.text = disptext;
          rec.caret.style.color = rec.opts.length ? '#e6eaef' : '#7a828c';
          var sig = sigOverride;
          if (sig == null) { sig = ''; for (var k = 0; k < rec.opts.length; k++) sig += rec.opts[k][0] + '|'; sig += '#' + selValue; }
          if (sig !== rec.sig) { rec.sig = sig; rebuildPopup(rec); }
        }
      };
    }
)EDJS"
R"EDJS(
    // ---- root: full-screen, non-hittest. Children draw bottom -> top. ----
    var root = $.CreatePanel('Panel', ctx, 'CamEditorRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '53'; // z-layer map: PanoramaBridge.h

    var confirmOverlay = mk('Panel', root); confirmOverlay.visible = false; confirmOverlay.hittest = true;
    confirmOverlay.style.width = '100%'; confirmOverlay.style.height = '100%'; confirmOverlay.style.zIndex = '200';
    confirmOverlay.style.backgroundColor = 'rgba(0,0,0,0.72)';
    confirmOverlay.SetPanelEvent('onactivate', function () { hideClearConfirm(); });
    var confirmBox = mk('Panel', confirmOverlay); confirmBox.hittest = true;
    confirmBox.style.width = '520px'; confirmBox.style.height = '230px';
    confirmBox.style.horizontalAlign = 'center'; confirmBox.style.verticalAlign = 'center';
    confirmBox.style.backgroundColor = 'rgba(16,20,26,0.99)';
    confirmBox.style.border = '1px solid ' + S.accent; confirmBox.style.borderRadius = '6px';
    confirmBox.style.boxShadow = '#000000ee 0px 0px 28px 6px';
    confirmBox.style.flowChildren = 'down';
    confirmBox.style.paddingTop = '26px'; confirmBox.style.paddingBottom = '24px';
    confirmBox.style.paddingLeft = '28px'; confirmBox.style.paddingRight = '28px';
    confirmBox.SetPanelEvent('onactivate', function () { /* keep clicks inside from dismissing */ });
    var confirmTitle = lbl(confirmBox, 'CLEAR ALL CAMERA PATHS?', S.value, 22);
    confirmTitle.style.fontWeight = 'bold'; confirmTitle.style.letterSpacing = '2px';
    confirmTitle.style.horizontalAlign = 'center'; confirmTitle.style.marginBottom = '14px';
    var confirmBody = lbl(confirmBox, 'This deletes every path camera/keyframe for this demo.', S.label, 14);
    confirmBody.style.horizontalAlign = 'center'; confirmBody.style.marginBottom = '22px';
    var confirmActions = mk('Panel', confirmBox); confirmActions.style.flowChildren = 'right';
    confirmActions.style.horizontalAlign = 'center';
    function hideClearConfirm() { confirmOverlay.visible = false; }
    function showClearConfirm() { confirmOverlay.visible = true; }
    var confirmNo = btn(confirmActions, 'No', function () { hideClearConfirm(); }, S.value);
    confirmNo.style.width = '132px'; confirmNo.__lbl.style.textAlign = 'center'; confirmNo.__lbl.style.width = '100%';
    var confirmYes = btn(confirmActions, 'Yes, clear all', function () {
      hideClearConfirm();
      cmd('mirv_filmmaker camtl clear');
    }, S.danger);
    confirmYes.style.width = '172px'; confirmYes.style.marginRight = '0px';
    confirmYes.__lbl.style.textAlign = 'center'; confirmYes.__lbl.style.width = '100%';

    // Preview catcher: full-screen click swallow (UI-cursor mode only) so clicking the
    // game preview can't leak through to spectator target switching. Inspector/backdrop
    // children are drawn AFTER it, so their own clicks still land.
    var catcher = mk('Panel', root); catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.style.position = '0px 0px 0px'; catcher.hittest = false;
    catcher.SetPanelEvent('onactivate', function () { closeAllDrops(); /* + swallow preview clicks */ });

    // View status tag pinned to the top-left of the live preview.
    var tag = mk('Panel', root); tag.hittest = false;
    tag.style.position = '18px 14px 0px'; tag.style.flowChildren = 'right';
    tag.style.height = '22px';
    tag.style.backgroundColor = 'rgba(12,14,18,0.65)'; tag.style.borderRadius = '3px';
    tag.style.paddingLeft = '8px'; tag.style.paddingRight = '10px';
    var dot = mk('Panel', tag); dot.style.width = '7px'; dot.style.height = '7px';
    dot.style.backgroundColor = S.danger; dot.style.borderRadius = '4px';
    dot.style.verticalAlign = 'center'; dot.style.marginRight = '7px';
    var tagLbl = lbl(tag, 'PREVIEW', S.value, 11); tagLbl.style.fontWeight = 'bold';
    tagLbl.style.letterSpacing = '2px'; tagLbl.style.verticalAlign = 'center';

    // Recording-style status pulse. Only opacity changes, so the tag never shifts or
    // reflows while flashing.
    var previewPulseBright = true;
    function pulsePreviewDot() {
      if (!dot || !dot.IsValid || !dot.IsValid()) return;
      previewPulseBright = !previewPulseBright;
      dot.style.opacity = previewPulseBright ? '1.0' : '0.22';
      $.Schedule(previewPulseBright ? 0.75 : 0.30, pulsePreviewDot);
    }
    $.Schedule(0.75, pulsePreviewDot);

    // Bottom backdrop (sits BEHIND the CameraTimeline panel, which is z55) = workspace
    // footer under the preview.
    var backdrop = mk('Panel', root); backdrop.hittest = false;
    backdrop.style.verticalAlign = 'top'; backdrop.style.horizontalAlign = 'left';
    backdrop.style.position = '0px 0px 0px';
    backdrop.style.width = '100%'; backdrop.style.height = BOTTOM_H + 'px';
    backdrop.style.backgroundColor = S.bg; backdrop.style.borderTop = '1px solid #ffffff14';

    // Bottom-editor tab bar: a DIVIDER sitting directly ABOVE the active bottom editor with the
    // three bottom-panel switches (where CS2's old CAM EDITOR / MOUSE buttons sat). It stays put
    // across modes; render() positions it just above whichever editor is docked below it, and sizes
    // its width. The native demo bar's own buttons are suppressed while the editor is open, so these
    // tabs replace them. Pure switches (NOT toggles): always select that mode -> never an empty bottom.
    var curBottomMode = 'native';
    var tabBar = mk('Panel', root); tabBar.hittest = false;
    tabBar.style.verticalAlign = 'top'; tabBar.style.horizontalAlign = 'left';
    tabBar.style.position = '0px 0px 0px';
    tabBar.style.height = TAB_BAR_H + 'px';
    tabBar.style.backgroundColor = S.bgSoft;
    tabBar.style.borderTop = '1px solid ' + S.accent; tabBar.style.borderBottom = '1px solid ' + S.accent;
    tabBar.style.flowChildren = 'right';
    tabBar.style.paddingTop = '4px'; tabBar.style.paddingBottom = '4px';
    tabBar.style.paddingLeft = '12px'; tabBar.style.paddingRight = '12px';
    // Order (left->right): Regular Timeline (native CS2 bar), Camera, Graph.
    var regularViewBtn = btn(tabBar, 'Regular Timeline', function () {
      cmd('mirv_filmmaker editor curveeditor native');
    }, S.value);
    regularViewBtn.style.verticalAlign = 'center'; regularViewBtn.style.width = '150px';
    regularViewBtn.__lbl.style.horizontalAlign = 'center';
    var timelineViewBtn = btn(tabBar, 'Camera', function () {
      cmd('mirv_filmmaker editor curveeditor timeline');
    }, S.value);
    timelineViewBtn.style.verticalAlign = 'center'; timelineViewBtn.style.width = '150px';
    timelineViewBtn.__lbl.style.horizontalAlign = 'center';
    var graphViewBtn = btn(tabBar, 'Graph', function () {
      cmd('mirv_filmmaker editor curveeditor graph');
    }, S.value);
    graphViewBtn.style.verticalAlign = 'center'; graphViewBtn.style.width = '150px';
    graphViewBtn.style.marginRight = '0px'; graphViewBtn.__lbl.style.horizontalAlign = 'center';

    // Right inspector column.
    var inspector = mk('Panel', root); inspector.hittest = false;
    inspector.style.horizontalAlign = 'right'; inspector.style.verticalAlign = 'top';
    inspector.style.width = INSPECTOR_W + 'px'; inspector.style.height = '100%';
    inspector.style.backgroundColor = S.bg; inspector.style.borderLeft = '1px solid #ffffff14';
    inspector.style.flowChildren = 'down';
    inspector.style.paddingTop = '14px'; inspector.style.paddingBottom = '14px';
    inspector.style.paddingLeft = '14px'; inspector.style.paddingRight = '14px';
    inspector.style.fontFamily = S.font;
    // Scroll the WHOLE inspector vertically. Without this, once the Follow panel grows
    // (target dropdown / event list / advanced tracking open) the lower controls fall
    // off the bottom of the screen with no way to reach them. 'squish scroll' = no
    // horizontal scroll, vertical scrollbar + wheel when the content overflows.
    inspector.style.overflow = 'squish scroll';

    // Aspect-ratio letterbox: the live preview is sized to the GAME's aspect ratio
    // (rootW/rootH) and CENTRED in the available area (screen minus the right inspector and
    // the bottom bar); the leftover is masked with black bars on all four sides and an accent
    // frame outlines the rect. Geometry is computed each render from the laid-out root size.
    // (The game still renders full-screen behind, so the preview is a CROP shaped to the
    // render aspect when the scaler is off, or a true scaled blit when it is on.)
    var barBottom = mk('Panel', root); barBottom.hittest = false; barBottom.visible = false;
    barBottom.style.backgroundColor = '#000000ff'; barBottom.style.horizontalAlign = 'left'; barBottom.style.verticalAlign = 'top';
    var barRight = mk('Panel', root); barRight.hittest = false; barRight.visible = false;
    barRight.style.backgroundColor = '#000000ff'; barRight.style.horizontalAlign = 'left'; barRight.style.verticalAlign = 'top';
    var barLeft = mk('Panel', root); barLeft.hittest = false; barLeft.visible = false;
    barLeft.style.backgroundColor = '#000000ff'; barLeft.style.horizontalAlign = 'left'; barLeft.style.verticalAlign = 'top';
    var barTop = mk('Panel', root); barTop.hittest = false; barTop.visible = false;
    barTop.style.backgroundColor = '#000000ff'; barTop.style.horizontalAlign = 'left'; barTop.style.verticalAlign = 'top';
    var frameV = mk('Panel', root); frameV.hittest = false; frameV.visible = false;
    frameV.style.width = '2px'; frameV.style.backgroundColor = S.accent; frameV.style.horizontalAlign = 'left'; frameV.style.verticalAlign = 'top';
    var frameH = mk('Panel', root); frameH.hittest = false; frameH.visible = false;
    frameH.style.height = '2px'; frameH.style.backgroundColor = S.accent; frameH.style.horizontalAlign = 'left'; frameH.style.verticalAlign = 'top';
    var frameL = mk('Panel', root); frameL.hittest = false; frameL.visible = false;
    frameL.style.width = '2px'; frameL.style.backgroundColor = S.accent; frameL.style.horizontalAlign = 'left'; frameL.style.verticalAlign = 'top';
    var frameT = mk('Panel', root); frameT.hittest = false; frameT.visible = false;
    frameT.style.height = '2px'; frameT.style.backgroundColor = S.accent; frameT.style.horizontalAlign = 'left'; frameT.style.verticalAlign = 'top';

    root.SetAttributeString('debugpanels', '[]');
    function hidePreviewLayout() {
      frameL.visible = frameV.visible = frameT.visible = frameH.visible = false;
      barLeft.visible = barRight.visible = barTop.visible = barBottom.visible = false;
      tag.visible = false;
      root.SetAttributeString('previewrect', '');
      root.SetAttributeString('debugpanels', '[]');
      dbgGeom = null;
    }

    // ===================== HEADER =======================================
    var head = row(inspector); head.style.marginTop = '0px';
    var hTitle = lbl(head, 'CAMERA EDITOR', S.value, 16); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.width = 'fill-parent-flow(1.0)';
    hTitle.style.verticalAlign = 'center';
    // Exit fully closes the editor and restores the native CS2 demo bar. The bottom-panel switch
    // (Camera Timeline / Graph) lives in the persistent bottom tab bar built above, not here.
    var exitBtn = btn(head, '✕ Exit', function () { cmd('mirv_filmmaker editor close'); }, S.value);
    exitBtn.style.marginRight = '0px';

    var mouseRow = row(inspector);
    var mouseBtn = btn(mouseRow, 'MOUSE: UI  (G)', function () { cmd('mirv_filmmaker camtl cursor toggle'); }, S.accent);
    mouseBtn.style.width = 'fill-parent-flow(1.0)'; mouseBtn.style.marginRight = '0px';
    mouseBtn.__lbl.style.horizontalAlign = 'center';

    var inspectorMode = 'path';
    var modeRow = row(inspector);
    var pathTab = btn(modeRow, 'PATH CAMERA', function () { inspectorMode = 'path'; }, S.accent);
    var followTab = btn(modeRow, 'FOLLOW CAMERA', function () { inspectorMode = 'follow'; }, S.value);
    pathTab.style.width = 'fill-parent-flow(1.0)';
    followTab.style.width = 'fill-parent-flow(1.0)'; followTab.style.marginRight = '0px';

    // Game-UI visibility (persists across the Path/Follow tabs): how much of CS2's own HUD
    // shows behind the editor. Hide All = clean; In-Game = radar + HP/ammo with NO spectator
    // observer panel; Show All = full spectator HUD.
    var hudLabel = lbl(inspector, 'GAME UI', S.dim, 10);
    hudLabel.style.letterSpacing = '1px'; hudLabel.style.marginTop = '7px';
    // One button that cycles Hide All -> In-Game -> Show All (text updates each click).
    var HUD_CYCLE = [['hidden', 'Hide All'], ['game', 'In-Game'], ['full', 'Show All']];
    var hudCycle = btn(inspector, 'Game UI: Hide All', function () {
      var cur = (st && st.hudView) || 'hidden', idx = 0;
      for (var i = 0; i < HUD_CYCLE.length; i++) if (HUD_CYCLE[i][0] === cur) idx = i;
      cmd('mirv_filmmaker editor hud ' + HUD_CYCLE[(idx + 1) % HUD_CYCLE.length][0]);
    }, S.value);
    hudCycle.style.width = '100%'; hudCycle.style.marginRight = '0px';
    hudCycle.__lbl.style.horizontalAlign = 'center';

)EDJS"
R"EDJS(
    // ===================== FOLLOW / LOCK-ON CAMERA =====================
    var followSec = section(inspector, 'FOLLOW / ATTACH CAMERA');
    var singleHint = lbl(followSec, 'ONE CAMERA · PLACING AGAIN REPLACES IT', S.dim, 9);
    singleHint.style.letterSpacing = '1px'; singleHint.style.marginBottom = '4px';
    // Fixed height + always-visible so clearing the text in Attach mode reserves the same
    // vertical space and the panel below doesn't jump up.
    singleHint.style.height = '12px';
    // Camera controls on ONE even row: Place | Reposition | Clear | Live. (Place/Clear are
    // lock-on only; Reposition also captures an attach-relative mount in Attach mode.)
    var followActions = row(followSec);
    var followPlace = btn(followActions, 'Place', function () {
      cmd('mirv_filmmaker follow place');
      // Immediate confirmation the CLICK registered (independent of engine state): flash white.
      followPlace.style.backgroundColor = '#ffffffcc';
      try { $.Schedule(0.18, function () { followPlace.style.backgroundColor = S.btnBg; }); } catch (e) {}
    }, S.accent);
    var followReposition = btn(followActions, 'Reposition', function () { cmd('mirv_filmmaker follow reposition'); }, S.value);
    var followClear = btn(followActions, 'Clear', function () { cmd('mirv_filmmaker follow clear'); }, S.danger);
    var followPreview = btn(followActions, 'Live', function () {
      cmd((st && st.follow && st.follow.enabled) ? 'mirv_filmmaker follow stop' : 'mirv_filmmaker follow preview');
    }, S.accent);
    [followPlace, followReposition, followClear, followPreview].forEach(function (b) {
      b.style.width = 'fill-parent-flow(1.0)'; b.style.paddingLeft = '4px'; b.style.paddingRight = '4px';
      b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.fontSize = '12px'; b.__lbl.style.whiteSpace = 'nowrap';
    });
    followPreview.style.marginRight = '0px';

    var followStatus = lbl(followSec, 'Place a camera and select a target.', S.dim, 11);
    followStatus.style.marginTop = '8px';
    var followWarn = lbl(followSec, '', '#f4c95dff', 10); followWarn.style.marginTop = '3px';
    var attachDebugText = lbl(followSec, '', '#9effa0ff', 9);
    attachDebugText.style.fontFamily = 'Consolas, "Courier New", monospace';
    attachDebugText.style.marginTop = '4px';

    // Camera model: ONE toggle that flips Lock-on (placed turret) <-> Attach (ride-along),
    // instead of two separate buttons.
    var modeToggle = btn(followSec, 'Camera Mode: Lock-on', function () {
      var attach = st && st.follow && st.follow.mode === 1;
      cmd('mirv_filmmaker follow mode ' + (attach ? 'lockon' : 'attach'));
    }, S.value);
    modeToggle.style.width = '100%'; modeToggle.style.marginRight = '0px'; modeToggle.style.marginTop = '9px';
    modeToggle.__lbl.style.horizontalAlign = 'center';
    var modeHint = lbl(followSec, '', S.dim, 9); modeHint.style.marginTop = '3px';

    // Target type: ONE dropdown whose options depend on the camera mode -- Lock-on lists
    // Player / Grenade / Weapon-C4; Attach lists Grenade / Player Bone / Weapon Attach.
    var TYPE_LOCKON = [['Player', 'player'], ['Grenade', 'grenade'], ['Weapon/C4', 'weapon']];
    var TYPE_ATTACH = [['Grenade', 'grenade'], ['Player Bone', 'player'], ['Weapon Attach', 'weapon']];
    var typeRow = row(followSec); typeRow.style.marginTop = '7px';
    var typeLbl = lbl(typeRow, 'Type', S.value, 12); typeLbl.style.width = '60px'; typeLbl.style.verticalAlign = 'center'; typeLbl.style.fontWeight = 'bold';
    var typeDrop = customDrop(typeRow, 'FmTypeDrop', function (value) { cmd('mirv_filmmaker follow type ' + value); });

    var pickRow = row(followSec); pickRow.style.marginTop = '7px';
    var nearestBtn = btn(pickRow, 'Select Nearest', function () { cmd('mirv_filmmaker follow nearest'); }, S.value);
    nearestBtn.style.width = 'fit-children';   // small button; closest target is shown to its right
    var nearestNameLbl = lbl(pickRow, '', S.value, 11);
    nearestNameLbl.style.verticalAlign = 'center'; nearestNameLbl.style.marginLeft = '10px';
    nearestNameLbl.style.width = 'fill-parent-flow(1.0)';
    nearestNameLbl.style.whiteSpace = 'nowrap'; nearestNameLbl.style.textOverflow = 'ellipsis';

    // Target selector: our customDrop. Its popup floats in the editor root above the inspector
    // (high z, not clipped by the inspector's scroll) so it overlays instead of expanding the
    // panel or pushing the controls below it.
    var targetRow = row(followSec); targetRow.style.marginTop = '7px';
    var targetLbl = lbl(targetRow, 'Target', S.value, 12);
    targetLbl.style.width = '60px'; targetLbl.style.verticalAlign = 'center'; targetLbl.style.fontWeight = 'bold';
    // Custom dropdown: each option's value is the candidate object itself, so onPick gets it back.
    var targetDrop = customDrop(targetRow, 'FmTargetDrop', function (c) { if (c) chooseCandidate(c); });
    var pendingCandidate = null;
    var confirmRow = row(followSec); confirmRow.visible = false;
    var confirmText = lbl(confirmRow, 'Target is far away. Use anyway?', '#f4c95dff', 10);
    confirmText.style.width = 'fill-parent-flow(1.0)'; confirmText.style.verticalAlign = 'center';
    btn(confirmRow, 'Use', function () {
      if (pendingCandidate) cmd('mirv_filmmaker follow select ' + pendingCandidate.index);
      pendingCandidate = null; confirmRow.visible = false;
    }, S.accent);
    btn(confirmRow, 'Cancel', function () { pendingCandidate = null; confirmRow.visible = false; }, S.value);

    function chooseCandidate(c) {
      if (st && st.follow && st.follow.type === 1) {
        cmd('mirv_filmmaker follow selecthandle ' + c.handle);
        return;
      }
      if (c.distanceLevel >= 2) {
        pendingCandidate = c;
        confirmText.text = (c.distanceLevel >= 3 ? 'Very far target' : 'Far target') +
          ' (' + Math.round(c.distance) + 'u). Use anyway?';
        confirmRow.visible = true;
      } else cmd('mirv_filmmaker follow select ' + c.index);
    }
    function candidateText(f, c) {
      if (f.type === 1) {
        var delta = c.tickDelta >= 0 ? ('+' + c.tickDelta) : String(c.tickDelta);
        return (c.grenadeType || c.name) + ' · ' + (c.ownerName || 'unknown thrower') +
          (c.team !== '-' ? ' · ' + c.team : '') + ' · tick ' + c.throwTick +
          ' (' + delta + ') · ' + (c.status || 'nearby');
      }
      if (c.isWeapon) return c.name + ' · ' + (c.status || (c.dropped ? 'dropped' : 'held'));
      // Player: "name · TEAM · 123u" -- just who, side, and how close. A dead flag is added only
      // when the player is dead (the old line printed "alive · alive" by appending both the alive
      // flag AND the status string).
      return c.name + (c.team && c.team !== '-' ? ' · ' + c.team : '') +
        (f.type === 0 && !c.alive ? ' · dead' : '') +
        ' · ' + Math.round(c.distance) + 'u';
    }
    function renderCandidates(f) {
      var MAXCAND = 40;
      var list = (f && f.candidates) || [];
      if (!list.length) {
        targetDrop.update([], null, 'empty:' + (f ? f.type : -1),
          (f && f.type === 1) ? 'No throws nearby' : 'No targets — Select Nearest');
        return;
      }
      // Distance is intentionally LEFT OUT of the rebuild signature: it changes every frame as
      // entities move, and rebuilding rows would close an open popup. Only the SET of targets (and
      // which one is selected) triggers a row rebuild; the collapsed bar text still updates live.
      var opts = [], sigParts = [], displayText = null, n = Math.min(MAXCAND, list.length);
      for (var j = 0; j < n; j++) {
        var c2 = list[j];
        var sel = !!(f.targetValid && ((c2.index === f.targetIndex && f.targetIndex >= 0) || (c2.handle && c2.handle === f.targetHandle)));
        var meta = { selected: sel };
        if (c2.distanceLevel >= 2) meta.color = '#f4c95dff';
        var text = candidateText(f, c2);
        opts.push([text, c2, meta]);
        if (sel) displayText = text;
        sigParts.push(c2.index + ':' + c2.handle + ':' + c2.name + ':' + c2.status + ':' + c2.throwTick + (sel ? ':S' : ''));
      }
      targetDrop.update(opts, null, sigParts.join('|') + ':' + f.type,
        displayText || ('Select target (' + n + ')'));
    }

)EDJS"
R"EDJS(
    // ---- Weapon source: Auto / Held / Dropped (Weapon type only) -- one dropdown -------
    var SRC_OPTS = [['Auto', 'auto'], ['Held', 'held'], ['Dropped', 'dropped']];
    var srcRow = row(followSec); srcRow.style.marginTop = '7px';
    var srcLabel = lbl(srcRow, 'Source', S.value, 12); srcLabel.style.width = '60px'; srcLabel.style.verticalAlign = 'center'; srcLabel.style.fontWeight = 'bold';
    var srcDrop = customDrop(srcRow, 'FmSrcDrop', function (value) { cmd('mirv_filmmaker follow weaponsource ' + value); });

    // ---- Attach point (Attach mode only) -- the SAME native dropdown as the others -------
    var attachRow = row(followSec); attachRow.style.marginTop = '7px';
    var attachLabel = lbl(attachRow, 'Attach pt', S.value, 12); attachLabel.style.width = '66px'; attachLabel.style.verticalAlign = 'center'; attachLabel.style.fontWeight = 'bold';
    var attachDrop = customDrop(attachRow, 'FmAttachDrop', function (value) { if (value) cmd('mirv_filmmaker follow bone ' + value); });
    function renderAttachmentMenu(f) {
      var options = [];
      if (f && f.attachPoints && f.attachPoints.length) options = f.attachPoints;
      var list = (f && f.candidates) || [], selected = null;
      for (var i = 0; i < list.length; i++) {
        if ((list[i].index === f.targetIndex) || (f.targetHandle && list[i].handle === f.targetHandle)) { selected = list[i]; break; }
      }
      if (!options.length && selected && selected.attachPoints && selected.attachPoints.length) options = selected.attachPoints;
      if (!options.length && selected && selected.attachments && selected.attachments.length) {
        options = [];
        for (var si = 0; si < selected.attachments.length; si++)
          options.push({ id: selected.attachments[si], label: selected.attachments[si], kind: 'attachment', valid: true });
      }
      var hasCurrent = false;
      for (var oi = 0; oi < options.length; oi++) if (options[oi].id === f.attachment) hasCurrent = true;
      if (f && f.attachment && !hasCurrent) options.unshift({ id: f.attachment, label: f.attachment + ' (invalid)', kind: 'invalid', valid: false });
      var opts = [];
      for (var j = 0; j < options.length; j++) {
        var p = options[j], name = p.id || p.label || 'entity', label = p.label || name;
        if (p.kind && p.kind !== 'attachment') label += ' · ' + p.kind;
        if (!p.valid) label += ' · invalid';
        opts.push([label, name]);
      }
      if (!opts.length) opts = [['No valid attach points', '']];
      attachDrop.update(opts, (f && f.attachment) || '');
    }

    // ---- Events: throw / drop / pickup ticks + Preview Tick -------------
    var eventsLabel = lbl(followSec, 'EVENTS', S.dim, 10);
    eventsLabel.style.letterSpacing = '1px'; eventsLabel.style.marginTop = '9px';
    var previewTickBtn = btn(followSec, 'Preview Tick (jump before event)', function () { cmd('mirv_filmmaker follow previewtick'); }, S.accent);
    previewTickBtn.style.marginTop = '3px'; previewTickBtn.style.marginRight = '0px';
    var eventRow = row(followSec); eventRow.style.marginTop = '5px';
    var eventLabel = lbl(eventRow, 'Event', S.value, 12);
    eventLabel.style.width = '60px'; eventLabel.style.verticalAlign = 'center'; eventLabel.style.fontWeight = 'bold';
    var eventDrop = customDrop(eventRow, 'FmEventDrop', function (value) {
      if (value != null && value !== '') cmd('mirv_filmmaker follow eventselect ' + value);
    });
    var eventSig = '';
    function eventText(e) {
      var label = e.type.replace('weapon_drop', 'death drop').replace('bomb_dropped', 'C4 drop')
        .replace('bomb_pickup', 'C4 pickup').replace('bomb_planted', 'C4 plant').replace('item_pickup', 'pickup');
      return 'tick ' + e.tick + '  ' + label + (e.item ? ('  ' + e.item) : '') + (e.ownerName ? ('  ' + e.ownerName) : '');
    }
    function renderEvents(f) {
      var list = (f && f.events) || [];
      var sig = (f ? (f.eventStatus + ':' + f.selectedEvent + ':') : '') + list.length;
      for (var i = 0; i < list.length; i++) sig += '|' + list[i].index + ':' + list[i].tick;
      if (!list.length) {
        var scanText = 'No drop/pickup events near here.';
        if (f && f.eventStatus === 1) {
          scanText = 'Scanning demo for drop/pickup events...' +
            (f.eventScanElapsedMs ? (' ' + Math.round(f.eventScanElapsedMs / 1000) + 's') : '');
        } else if (f && f.eventStatus === 3) {
          scanText = f.eventError ? ('Event scan failed: ' + f.eventError) : 'Event scan unavailable for this demo.';
        }
        eventDrop.update([], null, 'empty:' + (f ? f.eventStatus : -1), scanText);
        return;
      }
      var opts = [], displayText = null;
      for (var j = 0; j < list.length; j++) {
        var e = list[j], text = eventText(e), on = (f.selectedEvent === e.index);
        opts.push([text, e.index, { selected: on, color: on ? S.accent : S.value }]);
        if (on) displayText = text;
      }
      eventDrop.update(opts, f.selectedEvent, sig, displayText || ('Select event (' + list.length + ')'));
      eventSig = sig;
    }

    // ---- Framing: offset / rotation / FOV -- 2-column grid of native Sliders ----------
    var framingLabel = lbl(followSec, 'FRAMING (OFFSET / ROTATION / FOV)', S.dim, 10);
    framingLabel.style.letterSpacing = '1px'; framingLabel.style.marginTop = '9px';
    var xformPanel = mk('Panel', followSec); xformPanel.style.flowChildren = 'down'; xformPanel.style.width = '100%'; xformPanel.style.marginTop = '3px';
    var xformSliders = [];
    // Shared slider commit handler. (Panorama's Slider/keybind APIs don't expose a held-Shift
    // state during a drag, so fine-drag-on-Shift isn't wired here; the native Slider already
    // supports arrow-key nudging by one step once it has focus.)
    function attachSliderDrag(rec, cmdPrefix) {
      $.RegisterEventHandler('SliderValueChanged', rec.sl, function (panel, v) {
        if (rec.busy) return;
        rec.lastV = v;
        var value = rec.lo + v * (rec.hi - rec.lo);
        rec.vl.text = value.toFixed(rec.decimals);
        cmd(cmdPrefix + ' ' + value.toFixed(3));
      });
    }
    // One slider CELL (label | slider | value), sized to half the row so two fit per line.
    function sliderCell(parent, sink, name, lo, hi, decimals, getVal, command) {
      var cell = mk('Panel', parent); cell.style.flowChildren = 'right';
      cell.style.width = 'fill-parent-flow(1.0)'; cell.style.marginRight = '8px';
      var nl = lbl(cell, name, S.value, 12); nl.style.width = '54px'; nl.style.verticalAlign = 'center';
      nl.style.fontWeight = 'bold'; nl.style.whiteSpace = 'nowrap';
      var sl = $.CreatePanel('Slider', cell, '', { direction: 'horizontal' }); sl.AddClass('HorizontalSlider');
      sl.style.width = 'fill-parent-flow(1.0)'; sl.style.height = '12px'; sl.style.verticalAlign = 'center';
      var vl = lbl(cell, '-', S.accent, 12); vl.style.width = '40px'; vl.style.textAlign = 'right'; vl.style.verticalAlign = 'center'; vl.style.marginLeft = '5px'; vl.style.fontWeight = 'bold';
      var rec = { sl: sl, vl: vl, lo: lo, hi: hi, decimals: decimals, getVal: getVal, command: command, busy: false, lastV: 0, cell: cell };
      attachSliderDrag(rec, 'mirv_filmmaker follow ' + command);
      sink.push(rec); return rec;
    }
    // Lay items [name,lo,hi,dec,getVal,cmd] out two-per-row; a lone final item is left in
    // the first column at half width (reads as "centered-ish", balanced).
    function sliderGrid(parent, sink, items) {
      for (var i = 0; i < items.length; i += 2) {
        var r = row(parent); r.style.marginTop = '3px';
        var a = items[i]; sliderCell(r, sink, a[0], a[1], a[2], a[3], a[4], a[5]);
        if (items[i + 1]) { var b = items[i + 1]; sliderCell(r, sink, b[0], b[1], b[2], b[3], b[4], b[5]); }
        else { var pad = mk('Panel', r); pad.style.width = 'fill-parent-flow(1.0)'; } // keep lone slider at half width
      }
    }
    sliderGrid(xformPanel, xformSliders, [
      ['Off X', -256, 256, 1, function (f) { return f.offset ? f.offset[0] : 0; }, 'offsetx'],
      ['Off Y', -256, 256, 1, function (f) { return f.offset ? f.offset[1] : 0; }, 'offsety'],
      ['Off Z', -256, 256, 1, function (f) { return f.offset ? f.offset[2] : 0; }, 'offsetz'],
      ['Pitch', -180, 180, 0, function (f) { return f.rotation ? f.rotation[0] : 0; }, 'rotpitch'],
      ['Yaw',   -180, 180, 0, function (f) { return f.rotation ? f.rotation[1] : 0; }, 'rotyaw'],
      ['Roll',  -180, 180, 0, function (f) { return f.rotation ? f.rotation[2] : 0; }, 'rotroll'],
      ['FOV',      1, 170, 0, function (f) { return f.fov || 90; }, 'fov']
    ]);

)EDJS"
R"EDJS(
    var presetRow = row(followSec); presetRow.style.marginTop = '6px';
    var PRESETS = [['Center','centered'],['Above','above'],['Low','low'],['Shoulder','shoulder'],['Side','side']];
    PRESETS.forEach(function (it, idx) {
      var b = btn(presetRow, it[0], function () { cmd('mirv_filmmaker follow preset ' + it[1]); }, S.value);
      b.style.width = 'fill-parent-flow(1.0)'; b.style.paddingLeft = '3px'; b.style.paddingRight = '3px';
      b.style.paddingTop = '8px'; b.style.paddingBottom = '8px';
      b.__lbl.style.fontSize = '11px'; b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.whiteSpace = 'nowrap';
      if (idx === PRESETS.length - 1) b.style.marginRight = '0px';
    });

    var advancedOpen = false;
    var advancedToggle = btn(followSec, 'Advanced Tracking  ▸', function () {
      advancedOpen = !advancedOpen;
      advancedPanel.visible = advancedOpen;
      advancedToggle.__lbl.text = advancedOpen ? 'Advanced Tracking  ▾' : 'Advanced Tracking  ▸';
    }, S.value);
    advancedToggle.style.marginTop = '8px'; advancedToggle.style.marginRight = '0px';
    var advancedPanel = mk('Panel', followSec); advancedPanel.style.flowChildren = 'down';
    advancedPanel.style.width = '100%'; advancedPanel.style.marginTop = '4px'; advancedPanel.visible = false;

    // Advanced smoothing sliders -- same 2-column grid as Framing.
    var followSliders = [];
    sliderGrid(advancedPanel, followSliders, [
      ['Look',     0, 1.0,  2, function (f) { return f.look; },       'look'],
      ['Pos',      0, 1.0,  2, function (f) { return f.position; },   'position'],
      ['Predict',  0, 0.5,  2, function (f) { return f.prediction; }, 'prediction'],
      ['Deadzone', 0, 10,   1, function (f) { return f.deadzone; },   'deadzone'],
      ['Turn/s',   0, 1440, 0, function (f) { return f.maxTurn; },    'maxturn']
    ]);

    // Toggle options in a 2x2 grid (were full-width stacked rows).
    var optionBtns = {};
    function followToggle(parent, key, label, command) {
      var b = btn(parent, label, function () {
        var f = st && st.follow; cmd('mirv_filmmaker follow ' + command + ' ' + ((f && f[key]) ? 0 : 1));
      }, S.value);
      b.style.width = 'fill-parent-flow(1.0)'; b.style.marginRight = '8px';
      b.style.paddingTop = '9px'; b.style.paddingBottom = '9px';
      b.__lbl.style.fontSize = '11px'; b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.whiteSpace = 'nowrap';
      b.__base = label; optionBtns[key] = b; return b;
    }
    var TOGGLES = [
      ['autoDead', 'Auto-disable on death', 'autodead'], ['switchWeapon', 'Retarget dropped weapon', 'switchweapon'],
      ['switchBomb', 'Retarget dropped C4', 'switchbomb'], ['holdLast', 'Hold last known position', 'hold']
    ];
    for (var ti = 0; ti < TOGGLES.length; ti += 2) {
      var tr = row(advancedPanel); tr.style.marginTop = '6px';
      followToggle(tr, TOGGLES[ti][0], TOGGLES[ti][1], TOGGLES[ti][2]);
      var lastTog = followToggle(tr, TOGGLES[ti + 1][0], TOGGLES[ti + 1][1], TOGGLES[ti + 1][2]);
      lastTog.style.marginRight = '0px';
    }

    // (Attach-point selector + framing sliders moved up into the FOLLOW / ATTACH section.)

    // ===================== SELECTED CAMERA ==============================
    var pathPanels = [];
    var selSec = section(inspector, 'SELECTED CAMERA');
    pathPanels.push(selSec);
    var navRow = row(selSec);
    btn(navRow, '◀', function () { cmd('mirv_filmmaker camtl selectdelta -1'); }, S.value);
    var selLbl = lbl(navRow, 'none', S.value, 14); selLbl.style.fontWeight = 'bold';
    selLbl.style.width = 'fill-parent-flow(1.0)'; selLbl.style.textAlign = 'center';
    selLbl.style.verticalAlign = 'center';
    btn(navRow, '▶', function () { cmd('mirv_filmmaker camtl selectdelta 1'); }, S.value);
    var actRow = row(selSec);
    btn(actRow, '+ Add', function () { cmd('mirv_filmmaker camtl addkey'); }, S.accent);
    var delBtn = btn(actRow, '− Delete', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl delkey ' + st.selected); }, S.value);
    var retimeBtn = btn(actRow, '⟲ Tick→Here', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl movekey ' + st.selected + ' ' + st.tick); }, S.value);
    var clearAllBtn = btn(actRow, 'Clear All', function () { showClearConfirm(); }, S.danger);
    clearAllBtn.style.marginRight = '0px';

    // ===================== TRANSFORM (readout) ==========================
    var xfSec = section(inspector, 'TRANSFORM');
    pathPanels.push(xfSec);
    var posLbl = lbl(xfSec, '', S.value, 12); posLbl.style.marginTop = '2px';
    var angLbl = lbl(xfSec, '', S.value, 12); angLbl.style.marginTop = '3px';
    var xfHint = lbl(xfSec, '', S.dim, 10); xfHint.style.marginTop = '4px';
)EDJS"
R"EDJS(
    // ===================== LENS (FOV / ROLL sliders) ====================
    var sliders = [];
    function valSlider(parent, name, channel, lo, hi) {
      var r = row(parent);
      var nl = lbl(r, name, S.label, 12); nl.style.width = '46px'; nl.style.verticalAlign = 'center';
      var sl = $.CreatePanel('Slider', r, '', { direction: 'horizontal' }); sl.AddClass('HorizontalSlider');
      sl.style.width = 'fill-parent-flow(1.0)'; sl.style.height = '16px'; sl.style.verticalAlign = 'center';
      var vl = lbl(r, '-', S.accent, 12); vl.style.width = '54px'; vl.style.textAlign = 'right';
      vl.style.verticalAlign = 'center'; vl.style.marginLeft = '8px'; vl.style.fontWeight = 'bold';
      var rec = { sl: sl, vl: vl, lo: lo, hi: hi, ch: channel, drag: false };
      $.RegisterEventHandler('SliderValueChanged', sl, function (p, v) {
        var value = lo + v * (hi - lo); vl.text = value.toFixed(1);
        if (st && st.selected >= 0) {
          if (!rec.drag) { rec.drag = true; cmd('mirv_filmmaker camtl editbegin'); }
          cmd('mirv_filmmaker camtl setvalpreview ' + st.selected + ' ' + channel + ' ' + value.toFixed(3));
        }
      });
      $.RegisterEventHandler('SliderReleased', sl, function (p, v) {
        if (rec.drag) { rec.drag = false; cmd('mirv_filmmaker camtl editend'); }
      });
      sliders.push(rec); return rec;
    }
    var lensSec = section(inspector, 'LENS');
    pathPanels.push(lensSec);
    valSlider(lensSec, 'FOV', CH_FOV, 1, 170);
    valSlider(lensSec, 'Roll', CH_ROLL, -180, 180);
    var lensHint = lbl(lensSec, '', S.dim, 10); lensHint.style.marginTop = '4px';

    function syncSliders() {
      for (var i = 0; i < sliders.length; i++) {
        var s = sliders[i];
        if (!st || !st.sel) { s.vl.text = '-'; s.vl.style.color = S.dim; continue; }
        s.vl.style.color = S.accent;
        var cur = num((s.ch === CH_FOV) ? st.sel.fov : st.sel.roll);
        if (!s.sl.mousedown && !s.drag) s.sl.value = clamp01((cur - s.lo) / (s.hi - s.lo));
        if (!s.drag) s.vl.text = cur.toFixed(1);
      }
    }

    // ===================== PATH (interp / ease / speed / timing) ========
    var pathSec = section(inspector, 'PATH');
    pathPanels.push(pathSec);
    var interpBtn = btn(pathSec, 'Curve: Linear', function () {
      cmd('mirv_filmmaker marker interp cycle');
      // Mirror onto the experimental graph editor (the curve editor): the path interp is binary,
      // so flip the graph to match the value it is ABOUT to become (opposite of the current one).
      cmd('mirv_filmmaker grapheditor ' + ((st && st.interp === 'Linear') ? 'smooth' : 'linear'));
    }, S.value);
    interpBtn.style.marginTop = '2px';
    var EASE = ['none', 'in', 'out', 'inout'], EASE_LBL = ['None', 'Ease In', 'Ease Out', 'Ease In/Out'];
    var easeBtn = btn(pathSec, 'Ease: None', function () {
      if (!st || st.selected < 0) return;
      var nx = (((st.sel && st.sel.ease) || 0) + 1) % 4;
      cmd('mirv_filmmaker camtl ease ' + st.selected + ' ' + EASE[nx]);
      // Mirror onto the experimental graph editor (whole graph): None clears handles back to
      // straight lines; In/Out/In-Out flatten the matching tangents on every keyframe.
      cmd('mirv_filmmaker grapheditor ' + (nx === 0 ? 'linear' : ('ease ' + EASE[nx] + ' all')));
    }, S.value);
    easeBtn.style.marginTop = '5px';
    var smBtn = btn(pathSec, 'Speed: Manual', function () { cmd('mirv_filmmaker marker speedmode cycle'); }, S.value);
    smBtn.style.marginTop = '5px';

    // Segment speed stepper (Per-Segment, non-last key).
    var segRow = row(pathSec); segRow.style.marginTop = '5px';
    var segMinus = btn(segRow, '−', function () { stepSeg(-1); }, S.accent);
    var segLbl = lbl(segRow, 'Seg x1.00', S.value, 13); segLbl.style.width = 'fill-parent-flow(1.0)';
    segLbl.style.textAlign = 'center'; segLbl.style.verticalAlign = 'center';
    var segPlus = btn(segRow, '+', function () { stepSeg(1); }, S.accent); segPlus.style.marginRight = '0px';
    function stepSeg(d) {
      if (!st || !st.sel || st.selected < 0) return;
      var steps = [0.2, 0.5, 0.8, 1.0], cur = st.sel.speedMul, idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; i++) { var dd = Math.abs(steps[i] - cur); if (dd < best) { best = dd; idx = i; } }
      idx += (d > 0 ? 1 : -1); if (idx < 0) idx = 0; if (idx >= steps.length) idx = steps.length - 1;
      cmd('mirv_filmmaker camtl speed ' + st.selected + ' ' + steps[idx].toFixed(2));
    }

    // Constant speed stepper (Constant mode).
    var conRow = row(pathSec); conRow.style.marginTop = '5px';
    var conMinus = btn(conRow, '−', function () { stepConst(-1); }, S.accent);
    var conLbl = lbl(conRow, 'Const x1.00', S.value, 13); conLbl.style.width = 'fill-parent-flow(1.0)';
    conLbl.style.textAlign = 'center'; conLbl.style.verticalAlign = 'center';
    var conPlus = btn(conRow, '+', function () { stepConst(1); }, S.accent); conPlus.style.marginRight = '0px';
    function stepConst(d) {
      if (!st) return;
      var steps = [0.2, 0.5, 0.8, 1.0], cur = st.constSpeed, idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; i++) { var dd = Math.abs(steps[i] - cur); if (dd < best) { best = dd; idx = i; } }
      idx += (d > 0 ? 1 : -1); if (idx < 0) idx = 0; if (idx >= steps.length) idx = steps.length - 1;
      cmd('mirv_filmmaker marker constspeed ' + steps[idx].toFixed(2));
    }

    var timeBtn = btn(pathSec, 'Timing: Live', function () { cmd('mirv_filmmaker marker timing toggle'); }, S.value);
    timeBtn.style.marginTop = '5px';

    // ===================== PLAYBACK hints ===============================
    var playSec = section(inspector, 'SHORTCUTS');
    pathPanels.push(playSec);
    lbl(playSec, 'Space  ▶ / ⏸     ←/→  ±15s', S.value, 12);
    lbl(playSec, 'Scrub + keyframes on the timeline below.', S.dim, 10);
)EDJS"
R"EDJS(
    // ===================== VIEWPORT DEBUG OVERLAY =======================
    // Hidden unless state.debug (mirv_filmmaker editor debug). Read-only readout that compares the
    // Panorama preview rect against the render-layer world-blit rect and publishes per-panel HUD
    // rectangles for automation.
    var dbgPanel = $.CreatePanel('Panel', ctx, 'CamEditorDebugRoot', {});
    dbgPanel.hittest = false; dbgPanel.visible = false;
    dbgPanel.style.zIndex = '150'; dbgPanel.style.position = '8px 40px 0px';
    dbgPanel.style.width = '600px'; dbgPanel.style.flowChildren = 'down'; dbgPanel.style.height = 'fit-children';
    dbgPanel.style.backgroundColor = 'rgba(6,10,8,0.88)'; dbgPanel.style.border = '1px solid #5effa066';
    dbgPanel.style.borderRadius = '4px';
    dbgPanel.style.paddingTop = '8px'; dbgPanel.style.paddingBottom = '8px';
    dbgPanel.style.paddingLeft = '10px'; dbgPanel.style.paddingRight = '10px';
    var dbgText = lbl(dbgPanel, '', '#9effa0ff', 12);
    dbgText.style.fontFamily = 'Consolas, "Courier New", monospace'; dbgText.style.width = '100%';

    function dbgR(v) { v = Number(v || 0); return Math.round(v * 100) / 100; }
    function dbgRectObj(x, y, w, h) { return { x: dbgR(x), y: dbgR(y), w: dbgR(w), h: dbgR(h) }; }
    function dbgArea(r) { return dbgR(Math.max(0, r.w) * Math.max(0, r.h)); }
    function dbgIntersect(a, b) {
      if (!a || !b) return 0;
      var x0 = Math.max(a.x, b.x), y0 = Math.max(a.y, b.y);
      var x1 = Math.min(a.x + a.w, b.x + b.w), y1 = Math.min(a.y + a.h, b.y + b.h);
      return Math.max(0, x1 - x0) * Math.max(0, y1 - y0);
    }
    function dbgCropPct(r, bounds) {
      var area = Math.max(0, r.w) * Math.max(0, r.h);
      if (area <= 0) return 0;
      var inside = dbgIntersect(r, bounds);
      return dbgR(Math.max(0, Math.min(100, 100 - (inside / area) * 100)));
    }
    function dbgOverlapPct(r, blockers) {
      var area = Math.max(0, r.w) * Math.max(0, r.h);
      if (area <= 0) return 0;
      var overlap = 0;
      for (var i = 0; i < blockers.length; i++) overlap += dbgIntersect(r, blockers[i]);
      return dbgR(Math.max(0, Math.min(100, (overlap / area) * 100)));
    }

    // Absolute position of panel p relative to ctx (screen origin), in ACTUAL px.
    function dbgAbs(p) {
      var x = 0, y = 0, g = 0;
      try {
        while (p && g++ < 64) {
          x += (p.actualxoffset || 0); y += (p.actualyoffset || 0);
          if (p === ctx) break;
          p = p.GetParent ? p.GetParent() : null;
        }
      } catch (e) {}
      return { x: x, y: y };
    }
    function dbgFindByClass(rootp, cls, maxDepth) {
      var stack = [{ p: rootp, d: 0 }];
      while (stack.length) {
        var it = stack.pop(), p = it.p;
        if (!p) continue;
        try { if (p !== rootp && p.BHasClass && p.BHasClass(cls)) return p; } catch (e) {}
        if (it.d < maxDepth) {
          var n = p.GetChildCount ? p.GetChildCount() : 0;
          for (var i = 0; i < n; i++) { try { stack.push({ p: p.GetChild(i), d: it.d + 1 }); } catch (e2) {} }
        }
      }
      return null;
    }
    function dbgFindPanel(desc) {
      var p = null;
      try {
        if (desc.panel) return desc.panel();
        if (desc.id) p = ctx.FindChildTraverse && ctx.FindChildTraverse(desc.id);
        if (!p && desc.contentOf) p = ctx.FindChildTraverse && ctx.FindChildTraverse(desc.contentOf);
        if (!p && desc.ids) for (var i = 0; i < desc.ids.length && !p; i++) p = ctx.FindChildTraverse && ctx.FindChildTraverse(desc.ids[i]);
        if (!p && desc.cls) p = dbgFindByClass(ctx, desc.cls, 16);
      } catch (e) {}
      return p;
    }
    function dbgVisibleContentRect(rootp, sx, sy) {
      if (!rootp) return null;
      var rootAbs = dbgAbs(rootp);
      var rootRect = dbgRectObj(rootAbs.x / sx, rootAbs.y / sy,
        (rootp.actuallayoutwidth || 0) / sx, (rootp.actuallayoutheight || 0) / sy);
      var minX = 999999, minY = 999999, maxX = -999999, maxY = -999999, hits = 0;
      var stack = [{ p: rootp, d: 0 }];
      while (stack.length) {
        var it = stack.pop(), p = it.p;
        if (!p) continue;
        try {
          if (p !== rootp && p.visible !== false) {
            var a = dbgAbs(p), w = (p.actuallayoutwidth || 0) / sx, h = (p.actuallayoutheight || 0) / sy;
            var r = dbgRectObj(a.x / sx, a.y / sy, w, h);
            var almostRoot = dbgRectHasArea(rootRect) &&
              Math.abs(r.x - rootRect.x) <= 1 && Math.abs(r.y - rootRect.y) <= 1 &&
              Math.abs(r.w - rootRect.w) <= 1 && Math.abs(r.h - rootRect.h) <= 1;
            if (!almostRoot && dbgRectHasArea(r)) {
              minX = Math.min(minX, r.x); minY = Math.min(minY, r.y);
              maxX = Math.max(maxX, r.x + r.w); maxY = Math.max(maxY, r.y + r.h);
              hits++;
            }
          }
          if (it.d < 10) {
            var n = p.GetChildCount ? p.GetChildCount() : 0;
            for (var i = 0; i < n; i++) stack.push({ p: p.GetChild(i), d: it.d + 1 });
          }
        } catch (e) {}
      }
      if (!hits) return rootRect;
      // Children inside CS2 HUD roots are often intentionally laid out beyond the clipped parent.
      // Clamp the union back to the root so this records live visible pixels, not hidden overflow.
      minX = Math.max(minX, rootRect.x); minY = Math.max(minY, rootRect.y);
      maxX = Math.min(maxX, rootRect.x + rootRect.w); maxY = Math.min(maxY, rootRect.y + rootRect.h);
      return dbgRectObj(minX, minY, Math.max(0, maxX - minX), Math.max(0, maxY - minY));
    }
    function dbgFindAdjustedAncestor(p) {
      var cur = p, guard = 0;
      try {
        while (cur && guard++ < 32) {
          if (cur.__camViewportAdjusted) return cur;
          if (cur === ctx) break;
          cur = cur.GetParent ? cur.GetParent() : null;
        }
      } catch (e) {}
      return null;
    }
    function dbgRectFinite(r) {
      return !!(r && isFinite(r.x) && isFinite(r.y) && isFinite(r.w) && isFinite(r.h) &&
        Math.abs(r.x) < 100000 && Math.abs(r.y) < 100000 &&
        Math.abs(r.w) < 100000 && Math.abs(r.h) < 100000);
    }
    function dbgRectHasArea(r) {
      return dbgRectFinite(r) && r.w > 0.5 && r.h > 0.5;
    }
    function dbgPanelRecord(desc, sx, sy, g, blockers) {
      var p = dbgFindPanel(desc);
      var parent = null, id = '', source = dbgRectObj(0, 0, 0, 0), final = dbgRectObj(0, 0, 0, 0);
      var missing = true, inactive = false, adjusted = false, scaleX = 1, scaleY = 1, tx = 0, ty = 0;
      if (p && !(p.IsValid && !p.IsValid())) {
        missing = false;
        id = p.id || desc.id || '';
        parent = p.GetParent ? p.GetParent() : null;
        var a = dbgAbs(p), w = (p.actuallayoutwidth || 0), h = (p.actuallayoutheight || 0);
        source = desc.contentOf ? dbgVisibleContentRect(p, sx, sy) : dbgRectObj(a.x / sx, a.y / sy, w / sx, h / sy);
        if (!dbgRectHasArea(source)) {
          inactive = true;
          source = dbgRectObj(0, 0, 0, 0);
          final = source;
        } else {
          var anc = dbgFindAdjustedAncestor(p);
          adjusted = !!anc;
          if (g && anc) {
            scaleX = g.x1 - g.x0; scaleY = g.y1 - g.y0;
            var ox = g.x0 * ((root.actuallayoutwidth || ctx.actuallayoutwidth || 0) / sx);
            var oy = g.y0 * ((root.actuallayoutheight || ctx.actuallayoutheight || 0) / sy);
            final = dbgRectObj(ox + source.x * scaleX, oy + source.y * scaleY, source.w * scaleX, source.h * scaleY);
            tx = ox; ty = oy;
          } else {
            final = source;
          }
        }
      }
      var rw = ((root.actuallayoutwidth || ctx.actuallayoutwidth || 0) / sx);
      var rh = ((root.actuallayoutheight || ctx.actuallayoutheight || 0) / sy);
      var vp = g ? dbgRectObj(g.ox, g.oy, g.pw, g.ph) : dbgRectObj(0, 0, rw, rh);
      return {
        name: desc.name, id: id || desc.id || '', parent: (parent && parent.id) || '',
        missing: missing, inactive: inactive, adjusted: adjusted,
        source: source, viewport: vp, final: final,
        sourceArea: dbgArea(source), finalArea: dbgArea(final),
        cropPct: dbgCropPct(final, vp),
        editorOverlapPct: dbgOverlapPct(final, blockers),
        scale: { x: dbgR(scaleX), y: dbgR(scaleY) },
        offset: { x: dbgR(tx), y: dbgR(ty) }
      };
    }
    function dbgFmtRect(r) {
      if (!dbgRectFinite(r)) return 'invalid';
      return 'x=' + r.x.toFixed(0) + ' y=' + r.y.toFixed(0) + ' w=' + r.w.toFixed(0) + ' h=' + r.h.toFixed(0);
    }
    function dbgFmtPanel(p, sx, sy) {
      if (!p || (p.IsValid && !p.IsValid())) return 'n/a';
      var a = dbgAbs(p), w = (p.actuallayoutwidth || 0), h = (p.actuallayoutheight || 0);
      return 'x=' + (a.x / sx).toFixed(0) + ' y=' + (a.y / sy).toFixed(0) +
             ' w=' + (w / sx).toFixed(0) + ' h=' + (h / sy).toFixed(0);
    }
    var dbgDescriptors = [
      { name: 'chat', id: 'HudChat' },
      { name: 'money', id: 'HudMoney' },
      { name: 'health', id: 'HealthBar' },
      { name: 'armor', cls: 'hud-HA-armor' },
      { name: 'ammo', id: 'hud-WPN-main' },
      { name: 'weapon-panel', id: 'HudWeaponSelection' },
      { name: 'death-notices', id: 'HudDeathNotice' },
      { name: 'minimap', id: 'HudRadar' },
      { name: 'minimap-content', contentOf: 'HudRadar' },
      { name: 'score-timer', id: 'ScoreAndTimeAndBomb' },
      { name: 'player-cards', id: 'HudTeamCounter' },
      { name: 'player-cards-content', contentOf: 'HudTeamCounter' },
      { name: 'weapon-panel-content', contentOf: 'HudWeaponSelection' },
      { name: 'round-win-panel', id: 'HudWinPanel' },
      { name: 'round-win-container', cls: 'WinPanelBasicContainer' },
      { name: 'round-win-result', cls: 'WinPanel__Result' },
      { name: 'round-win-mvp-section', cls: 'MVP_section' },
      { name: 'round-win-mvp', id: 'MVP' },
      { name: 'hud-alerts', id: 'HudAlerts' },
      { name: 'progress-bar', id: 'HudProgressBar' },
      { name: 'vote', id: 'HudVote' },
      { name: 'instructor', id: 'HudInstructor' },
      { name: 'reticle', id: 'HudReticle' },
      { name: 'damage-indicator', id: 'HudDamageIndicator' },
      { name: 'radio', id: 'HudRadio' },
      { name: 'trueview-active', ids: ['TrueViewCheckBox', 'TrueViewToggleButton', 'Settings', 'Contents'] },
      { name: 'camera-editor-panel', panel: function () { return inspector; } },
      { name: 'graph-editor-panel', id: 'GraphExpRoot' },
      { name: 'camera-timeline-panel', id: 'CamTimelineBar' }
    ];
    function updateDebug(st) {
      if (!st || !st.debug) { dbgPanel.visible = false; return; }
      dbgPanel.visible = true;
      var rsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, rsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rawW = root.actuallayoutwidth || ctx.actuallayoutwidth || 0, rawH = root.actuallayoutheight || ctx.actuallayoutheight || 0;
      var rw = rawW / rsx, rh = rawH / rsy;
      var L = [];
      L.push('=== CAMERA EDITOR VIEWPORT DEBUG ===');
      L.push('window phys   ' + rawW.toFixed(0) + ' x ' + rawH.toFixed(0) + '   (= render target)');
      L.push('uiscale       ' + rsx.toFixed(4) + ' x ' + rsy.toFixed(4));
      L.push('window virt   ' + rw.toFixed(1) + ' x ' + rh.toFixed(1));
      L.push('aspect        ' + (rh > 0 ? (rw / rh).toFixed(4) : '0'));
      var g = dbgGeom;
      if (g) {
        L.push('-- preview rect (panorama) --');
        L.push('frac          x0=' + g.x0.toFixed(4) + ' y0=' + g.y0.toFixed(4) + ' x1=' + g.x1.toFixed(4) + ' y1=' + g.y1.toFixed(4));
        L.push('px virt       x=' + g.ox.toFixed(0) + ' y=' + g.oy.toFixed(0) + ' w=' + g.pw.toFixed(0) + ' h=' + g.ph.toFixed(0));
        L.push('scale sx,sy   ' + (g.x1 - g.x0).toFixed(4) + ' , ' + (g.y1 - g.y0).toFixed(4) + '  (insets x=0 top=0)');
        L.push('offset tx,ty  ' + (g.x0 * rw).toFixed(1) + ' , ' + (g.y0 * rh).toFixed(1));
        L.push('expected px   x=' + (g.x0 * rawW).toFixed(0) + ' y=' + (g.y0 * rawH).toFixed(0) +
               ' w=' + ((g.x1 - g.x0) * rawW).toFixed(0) + ' h=' + ((g.y1 - g.y0) * rawH).toFixed(0));
      } else { L.push('-- preview rect: not laid out yet --'); }
      var d = st.dbg || {};
      L.push('-- world blit (render layer) --');
      L.push('blitRan=' + (d.blitRan ? 'YES' : 'no') + '  scaleReq=' + (d.scaleReq ? 'on' : 'off') + '  rectValid=' + (d.previewValid ? 'yes' : 'no'));
      L.push('render target ' + (d.bbW || 0) + ' x ' + (d.bbH || 0));
      L.push('actual px     x=' + Number(d.vx || 0).toFixed(0) + ' y=' + Number(d.vy || 0).toFixed(0) +
             ' w=' + Number(d.vw || 0).toFixed(0) + ' h=' + Number(d.vh || 0).toFixed(0));
      if (g && d.blitRan) {
        var ex = g.x0 * rawW, ey = g.y0 * rawH, ew = (g.x1 - g.x0) * rawW, eh = (g.y1 - g.y0) * rawH, tol = 2.0;
        var match = Math.abs(ex - d.vx) <= tol && Math.abs(ey - d.vy) <= tol && Math.abs(ew - d.vw) <= tol && Math.abs(eh - d.vh) <= tol;
        L.push('blit rect match ' + (match ? 'YES (world only)' : 'NO  <-- expected vs actual differ'));
        L.push('HUD alignment is per-panel below; active crop/overlap means NOT locked');
      }
      var blockers = [];
      var inspAbs = dbgAbs(inspector);
      if (inspector.visible) blockers.push(dbgRectObj(inspAbs.x / rsx, inspAbs.y / rsy, (inspector.actuallayoutwidth || 0) / rsx, (inspector.actuallayoutheight || 0) / rsy));
      if (curBottomMode === 'graph' || curBottomMode === 'camera') blockers.push(dbgRectObj(0, rh - (curBottomMode === 'graph' ? GRAPH_DOCK_H + BOTTOM_LIFT : 180), Math.max(0, rw - INSPECTOR_W), curBottomMode === 'graph' ? GRAPH_DOCK_H + BOTTOM_LIFT : 180));
      L.push('-- panels (screen px) --');
      L.push('hud root      ' + dbgFmtPanel(ctx, rsx, rsy));
      var recs = [];
      for (var di = 0; di < dbgDescriptors.length; di++) {
        var rec = dbgPanelRecord(dbgDescriptors[di], rsx, rsy, g, blockers);
        recs.push(rec);
        L.push(rec.name + ' <' + (rec.id || '?') + '> src ' + dbgFmtRect(rec.source) +
          ' final ' + dbgFmtRect(rec.final) + ' area=' + rec.finalArea.toFixed(0) +
          ' crop=' + rec.cropPct.toFixed(1) + '% overlap=' +
          rec.editorOverlapPct.toFixed(1) + '% parent=<' + (rec.parent || '?') + '>' +
          (rec.missing ? ' MISSING' : (rec.inactive ? ' INACTIVE' : '')));
      }
      try { root.SetAttributeString('debugpanels', JSON.stringify(recs)); } catch (eJson) { root.SetAttributeString('debugpanels', '[]'); }
      dbgText.text = L.join('\n');
    }
)EDJS"
R"EDJS(
    // =====================================================================
    var st = null;
    var api = {};
    api.setInspectorMode = function (mode) {
      inspectorMode = mode === 'follow' ? 'follow' : 'path';
    };
    api.setAttachmentMenuOpen = function (open) {
      // Attach pt is a native DropDown now; toggling it open programmatically isn't needed.
    };
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }

      root.visible = !!(st.enabled || st.debug);
      if (!st.enabled) {
        closeAllDrops();
        confirmOverlay.visible = false;
        catcher.hittest = false;
        tag.visible = false;
        backdrop.visible = false;
        tabBar.visible = false;
        inspector.visible = false;
        barLeft.visible = barRight.visible = barTop.visible = barBottom.visible = false;
        frameL.visible = frameV.visible = frameT.visible = frameH.visible = false;
        var crsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, crsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
        var crawW = root.actuallayoutwidth || 0, crawH = root.actuallayoutheight || 0;
        if (crawW < 16) { crawW = ctx.actuallayoutwidth || 0; crawH = ctx.actuallayoutheight || 0; }
        var crw = crawW / crsx, crh = crawH / crsy;
        dbgGeom = (crw > 1 && crh > 1) ? { x0: 0, y0: 0, x1: 1, y1: 1, ox: 0, oy: 0, pw: crw, ph: crh } : null;
        root.SetAttributeString('previewrect', '0 0 1 1');
        updateDebug(st);
        return;
      }
      inspector.visible = true;
      tabBar.visible = true;

      // UI-cursor gating: panels are only clickable in UI-mouse mode; in GAME mode the
      // mouse flies the free cam and Panorama receives no clicks anyway.
      var cur = !!st.cursor;
      if (!cur) closeAllDrops(); // can't click a popup in GAME-mouse mode; don't leave it hanging
      catcher.hittest = cur;
      inspector.hittest = cur;
      backdrop.hittest = cur;
      tabBar.hittest = cur;
      mouseBtn.__lbl.text = cur ? 'MOUSE: UI  (G)' : 'MOUSE: GAME  (G)';
      mouseBtn.style.backgroundColor = cur ? S.btnOn : S.btnBg;
      mouseBtn.__lbl.style.color = cur ? S.accent : S.label;

      // Game-UI visibility picker highlight (default 'hidden' = clean workspace).
      var hv = st.hudView || 'hidden', hudName = 'Hide All';
      for (var hb = 0; hb < HUD_CYCLE.length; hb++) if (HUD_CYCLE[hb][0] === hv) hudName = HUD_CYCLE[hb][1];
      hudCycle.__lbl.text = 'Game UI: ' + hudName;

      var followMode = inspectorMode === 'follow';
      followSec.visible = followMode;
      for (var pm = 0; pm < pathPanels.length; pm++) pathPanels[pm].visible = !followMode;
      followTab.style.backgroundColor = followMode ? S.btnOn : S.btnBg;
      followTab.__lbl.style.color = followMode ? S.accent : S.value;
      pathTab.style.backgroundColor = followMode ? S.btnBg : S.btnOn;
      pathTab.__lbl.style.color = followMode ? S.value : S.accent;
      var bm = st.bottomMode || (st.graphExp ? 'graph' : 'native');
      curBottomMode = bm;
      var regularActive = (bm !== 'camera' && bm !== 'graph'); // 'native' (Regular Timeline)
      timelineViewBtn.style.backgroundColor = bm === 'camera' ? S.btnOn : S.btnBg;
      timelineViewBtn.__lbl.style.color = bm === 'camera' ? S.accent : S.value;
      graphViewBtn.style.backgroundColor = bm === 'graph' ? S.btnOn : S.btnBg;
      graphViewBtn.__lbl.style.color = bm === 'graph' ? S.accent : S.value;
      regularViewBtn.style.backgroundColor = regularActive ? S.btnOn : S.btnBg;
      regularViewBtn.__lbl.style.color = regularActive ? S.accent : S.value;

      var f = st.follow || {};
      var liveBadge = !!(f.enabled || st.pathLive || (st.graphExp && st.graphDrive));
      tagLbl.text = liveBadge ? 'LIVE' : 'PREVIEW';
      var attachModeOn = (f.mode === 1);
      // Camera-model toggle (one button; tap flips Lock-on <-> Attach).
      modeToggle.__lbl.text = (attachModeOn ? 'Camera Mode: Attach' : 'Camera Mode: Lock-on') + '   ⇄';
      modeToggle.style.backgroundColor = S.btnOn; modeToggle.__lbl.style.color = S.accent;
      modeHint.text = attachModeOn ? 'Rides the target (offset / rotation / FOV). Tap to switch.'
                                   : 'A placed camera rotates to track the target. Tap to switch.';
      if (attachModeOn) {
        advancedOpen = false;
        advancedToggle.visible = false;
        advancedPanel.visible = false;
        advancedToggle.__lbl.text = 'Advanced Tracking  ▸';
      } else {
        advancedToggle.visible = true;
        advancedPanel.visible = advancedOpen;
        advancedToggle.__lbl.text = advancedOpen ? 'Advanced Tracking  ▾' : 'Advanced Tracking  ▸';
      }
      // Target-type dropdown: options follow the mode; selection reflects the current type.
      var typeVal = (f.type === 1) ? 'grenade' : (f.type === 2 ? 'weapon' : 'player');
      typeDrop.update(attachModeOn ? TYPE_ATTACH : TYPE_LOCKON, typeVal);
      // Place/Clear are lock-on only. Reposition is shared: lock-on moves the placed camera,
      // attach captures the current freecam pose as offset / rotation / FOV relative to the bone.
      if (singleHint) singleHint.text = attachModeOn ? 'RELATIVE MOUNT · REPOSITION SAVES OFFSET/ROT/FOV' : 'ONE CAMERA · PLACING AGAIN REPLACES IT';
      followPlace.visible = !attachModeOn;
      followReposition.visible = true;
      followClear.visible = !attachModeOn;
      followPlace.__lbl.text = f.hasCamera ? 'Replace' : 'Place';
      // Live = the preview toggle (short label so it never wraps).
      followPreview.__lbl.text = f.enabled ? 'Stop' : 'Live';
      followPreview.style.backgroundColor = f.enabled ? S.btnOn : S.btnBg;
      var haveTarget = (f.targetIndex >= 0) || (f.type === 2 && f.weaponPlayerIndex >= 0);
      followStatus.text = haveTarget
        ? ((f.targetName || ('Entity #' + f.targetIndex)) + '  -  ' + (f.typeName || 'Player') + (attachModeOn ? ' (attach)' : ' (lock-on)'))
        : (attachModeOn ? 'Select a target to ride.' : 'Place a camera, then select a target.');
      followStatus.style.color = f.targetValid ? S.value : S.dim;
      followWarn.text = f.repositioning ? (attachModeOn
        ? 'Reposition active: move in freecam, then left-click to save attach pose.'
        : 'Reposition active: move in freecam, then left-click to place.')
        : (f.message ? f.message
        : (!attachModeOn && f.distanceLevel >= 3 ? 'Very far / likely outside the useful shot area.'
        : (!attachModeOn && f.distanceLevel >= 2 ? 'Selected target is far from this camera.' : '')));
      // Target selector. Small "Select Nearest" button; the closest candidate (list is
      // distance-sorted, so [0]) is named to its right.
      nearestBtn.__lbl.text = (f.type === 1) ? 'Nearest Throw' : 'Select Nearest';
      var nearestCand = (f.candidates && f.candidates.length) ? f.candidates[0] : null;
      nearestNameLbl.text = nearestCand
        ? ('closest: ' + nearestCand.name + (nearestCand.team && nearestCand.team !== '-' ? '  ·  ' + nearestCand.team : ''))
        : 'closest: —';
      nearestNameLbl.style.color = nearestCand ? S.value : S.dim;
      // The native DropDown shows the current target itself (no separate caret button).
      renderCandidates(f);
      // Weapon source (Weapon type only).
      srcRow.visible = (f.type === 2);
      if (f.type === 2) {
        var srcVal = (f.weaponSource === 1) ? 'held' : (f.weaponSource === 2 ? 'dropped' : 'auto');
        srcDrop.update(SRC_OPTS, srcVal);
      }
      // Attach point (attach mode only). The native dropdown shows the chosen point itself.
      attachRow.visible = attachModeOn;
      if (attachModeOn) renderAttachmentMenu(f);
      var ad = f.attachDebug || {};
      attachDebugText.visible = !!(f.debug && attachModeOn && ad.active);
      if (attachDebugText.visible) {
        attachDebugText.text = 'entity ' + (ad.entityIndex == null ? '-' : ad.entityIndex) +
          '  attach ' + (ad.attachId || '-') + '  ' + (ad.source || '-') + '\n' +
          'target ' + ((ad.rawTarget || []).join ? ad.rawTarget.join(', ') : '-') + '\n' +
          'camera ' + ((ad.camera || []).join ? ad.camera.join(', ') : '-') + '\n' +
          'dist ' + Number(ad.distance || 0).toFixed(1) +
          '  aim ' + Number(ad.aimError || 0).toFixed(1) +
          '  jitter ' + Number(ad.jitter || 0).toFixed(2) +
          '  dC ' + Number(ad.cameraDelta || 0).toFixed(2) +
          '  dT ' + Number(ad.targetDelta || 0).toFixed(2);
      } else {
        attachDebugText.text = '';
      }
      // Events + Preview Tick (grenade throws / weapon-C4 drops).
      var eventsRelevant = (f.type === 1 || f.type === 2);
      eventsLabel.visible = eventsRelevant;
      previewTickBtn.visible = eventsRelevant;
      eventRow.visible = (f.type === 2);
      previewTickBtn.__lbl.text = (f.type === 1)
        ? (f.grenadePending ? 'Waiting for Throw...' : 'Preview Tick (track grenade throw)')
        : 'Preview Tick (jump before drop)';
      previewTickBtn.style.backgroundColor = f.grenadePending ? S.btnOn : S.btnBg;
      if (f.type === 2) renderEvents(f);
      else eventDrop.update([], null, 'hidden', 'No events');
      // Framing sliders (offset / rotation / FOV).
      for (var xs = 0; xs < xformSliders.length; xs++) {
        var xr = xformSliders[xs], xv = num(xr.getVal(f));
        xr.busy = true; xr.sl.value = clamp01((xv - xr.lo) / (xr.hi - xr.lo)); xr.busy = false;
        xr.vl.text = xv.toFixed(xr.decimals);
      }
      // Advanced smoothing sliders (now getVal-based, same as Framing).
      for (var fs = 0; fs < followSliders.length; fs++) {
        var rec = followSliders[fs], value = num(rec.getVal(f));
        rec.busy = true; rec.sl.value = clamp01((value - rec.lo) / (rec.hi - rec.lo)); rec.busy = false;
        rec.vl.text = value.toFixed(rec.decimals);
      }
      for (var fk in optionBtns) if (optionBtns.hasOwnProperty(fk)) {
        optionBtns[fk].style.backgroundColor = f[fk] ? S.btnOn : S.btnBg;
        optionBtns[fk].__lbl.style.color = f[fk] ? S.accent : S.value;
        optionBtns[fk].__lbl.text = (f[fk] ? '[x] ' : '[ ] ') + optionBtns[fk].__base;
      }
      // Aspect-ratio letterbox. Virtual px = actuallayout / uiscale (matches style px).
      // Measure from the ALREADY-laid-out HUD context panel when our fresh root still reports 0
      // (the first layout passes after a build / resolution switch) -- otherwise the preview +
      // inspector squish into the top-left for ~half a second until root settles.
      var rsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, rsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rawW = root.actuallayoutwidth || 0, rawH = root.actuallayoutheight || 0;
      if (rawW < 16) { rawW = ctx.actuallayoutwidth || 0; rawH = ctx.actuallayoutheight || 0; }
      var rw = rawW / rsx, rh = rawH / rsy;
      // The tab bar + backdrop span the content width left of the inspector. The tab bar is a
      // DIVIDER docked directly ABOVE the active bottom editor; the editor rests flush at the
      // bottom and the preview shrinks to clear both. Anchor from the top; Panorama does not
      // reliably apply marginBottom on this full-screen root after the viewport is scaled.
      tabBar.style.width = Math.floor(Math.max(0, rw - INSPECTOR_W)) + 'px';
      if (!finitePositive(rw) || !finitePositive(rh)) {
        hidePreviewLayout();
      } else {
      // editorTop = y-from-bottom of the TOP edge of whatever editor is docked below the tab bar.
      // Camera/Graph overlays rest at marginBottom=BOTTOM_LIFT; the native bar rests at the very
      // bottom (Regular Timeline mode). Measure live heights where they can grow.
      var nativeBottom = (bm !== 'camera' && bm !== 'graph'); // 'native' = Regular Timeline
)EDJS"
R"EDJS(
      var editorTop;
      if (bm === 'graph') {
        editorTop = BOTTOM_LIFT + GRAPH_DOCK_H;
      } else if (bm === 'camera') {
        // The hosted CameraTimeline bar (#CamTimelineBar) is fit-children, so it grows when the
        // curve editor opens. Read its ACTUAL height; fall back to BOTTOM_H until it lays out.
        var camH = BOTTOM_H;
        try {
          var tlBar = ctx.FindChildTraverse && ctx.FindChildTraverse('CamTimelineBar');
          var bh = (tlBar && tlBar.visible) ? tlBar.actuallayoutheight / rsy : 0;
          if (bh > 10 && bh < rh * 0.6) camH = bh;
        } catch (eBar) {}
        editorTop = BOTTOM_LIFT + camH;
      } else {
        // Regular Timeline = CS2's native demo bar (docked left of the inspector by CameraTimelineJs),
        // resting at the very bottom. Measure its height so the tab bar sits right above it.
        var natH = NATIVE_BAR_H;
        try {
          var sr = ctx.FindChildTraverse && ctx.FindChildTraverse('SliderRow');
          var nc = sr && sr.GetParent ? sr.GetParent() : null;
          var nh = (nc && nc.visible) ? nc.actuallayoutheight / rsy : 0;
          if (nh > 10 && nh < rh * 0.6) natH = nh;
        } catch (eNat) {}
        editorTop = natH;
      }
      var bottomH = Math.max(TAB_BAR_H + BOTTOM_LIFT, Math.min(editorTop + TAB_GAP + TAB_BAR_H, Math.max(0, rh - 160)));
      var dockTopY = Math.max(0, rh - bottomH);
      var desiredTabY = rh - editorTop - TAB_GAP - TAB_BAR_H;
      var tabY = Math.max(dockTopY, desiredTabY);
      tabBar.style.position = '0px 0px 0px';
      tabBar.style.marginTop = Math.floor(tabY) + 'px';
      tabBar.style.marginBottom = '0px';
      // In Regular Timeline mode the native CS2 bar (a LOWER-z HUD root) is the editor: our opaque
      // backdrop (z53) would HIDE the bar if it covered it, and our full-screen catcher would eat the
      // bar's clicks -- so the catcher shrinks to the preview area (the scaler blacks the rest).
      // For the backdrop we DON'T drop it entirely (that left the TAB_GAP strip just above the bar
      // see-through, bleeding the game render as a thin line); instead we anchor it ABOVE the bar
      // (editorTop..bottomH) so it fills that gap behind the tab bar without covering the native bar.
      // Camera/graph overlays render ABOVE z53, so there the backdrop covers the whole band (0..bottomH).
      backdrop.style.position = '0px 0px 0px';
      backdrop.style.marginTop = Math.floor(dockTopY) + 'px';
      backdrop.style.marginBottom = '0px';
      if (nativeBottom) {
        backdrop.style.height = Math.floor(Math.max(0, bottomH - editorTop)) + 'px';
      } else {
        backdrop.style.height = Math.floor(bottomH) + 'px';
      }
      backdrop.visible = true;
      catcher.style.height = nativeBottom ? Math.floor(Math.max(0, rh - bottomH)) + 'px' : '100%';
      var areaW = rw - INSPECTOR_W, areaH = rh - bottomH;
      if (finitePositive(areaW) && finitePositive(areaH)) {
        var aspect = rw / rh;            // the game's render aspect (full window)
        var pw = areaW, ph = pw / aspect;
        if (ph > areaH) { ph = areaH; pw = ph * aspect; } // fit the rect inside the area
        pw = Math.floor(pw); ph = Math.floor(ph);
        if (!finitePositive(pw) || !finitePositive(ph)) {
          hidePreviewLayout();
        } else {
        // Centre the shrunk preview in the available area so the black letterbox is even on
        // all sides instead of pinned top-left with one fat gap (the curve editor grows the
        // bottom bar, shrinking the preview -- this keeps it middled).
        var ox = Math.floor((areaW - pw) / 2); if (ox < 0) ox = 0;
        var oy = Math.floor((areaH - ph) / 2); if (oy < 0) oy = 0;
        var rgt = ox + pw, bot = oy + ph; // preview right / bottom edges (area space)

        // Four black mask bars: full-height left & right columns + top & bottom caps over the
        // preview column. Together they cover the whole area except the preview rect.
        barLeft.style.position = '0px 0px 0px';
        barLeft.style.width = ox + 'px'; barLeft.style.height = areaH + 'px';
        barLeft.visible = ox > 1;
        barRight.style.position = rgt + 'px 0px 0px';
        barRight.style.width = (areaW - rgt) + 'px'; barRight.style.height = areaH + 'px';
        barRight.visible = (areaW - rgt) > 1;
        barTop.style.position = ox + 'px 0px 0px';
        barTop.style.width = pw + 'px'; barTop.style.height = oy + 'px';
        barTop.visible = oy > 1;
        barBottom.style.position = ox + 'px ' + bot + 'px 0px';
        barBottom.style.width = pw + 'px'; barBottom.style.height = (areaH - bot) + 'px';
        barBottom.visible = (areaH - bot) > 1;

        // Accent frame: full rectangle around the centred preview.
        frameL.style.position = ox + 'px ' + oy + 'px 0px'; frameL.style.height = ph + 'px'; frameL.visible = true;
        frameV.style.position = (rgt - 2) + 'px ' + oy + 'px 0px'; frameV.style.height = ph + 'px'; frameV.visible = true;
        frameT.style.position = ox + 'px ' + oy + 'px 0px'; frameT.style.width = pw + 'px'; frameT.visible = true;
        frameH.style.position = ox + 'px ' + (bot - 2) + 'px 0px'; frameH.style.width = pw + 'px'; frameH.visible = true;

        // Keep the status tag pinned to the centred preview's top-left corner.
        tag.visible = true;
        tag.style.position = (ox + 18) + 'px ' + (oy + 14) + 'px 0px';

        // Publish the preview rect as NORMALISED root fractions (x0 y0 x1 y1 corners) so the
        // render-layer scaler (CViewportScaler) blits the whole frame into exactly this centred
        // rect when "editor scale" is on (it clears the rest black). Single source of truth
        // shared between Panorama (virtual px) and D3D (physical px); fractions stay resolution/
        // uiscale-independent so the two passes line up. The bars double as the visual frame
        // when scaling is OFF (crop mode); when ON they sit black-on-black.
        root.SetAttributeString('previewrect',
          (ox / rw).toFixed(5) + ' ' + (oy / rh).toFixed(5) + ' ' + (rgt / rw).toFixed(5) + ' ' + (bot / rh).toFixed(5));
        // [debug overlay] stash the laid-out preview geometry (fractions + virtual px) for updateDebug.
        dbgGeom = { x0: ox / rw, y0: oy / rh, x1: rgt / rw, y1: bot / rh, ox: ox, oy: oy, pw: pw, ph: ph };
        }
      } else {
        hidePreviewLayout();
      }
      }

      // NOTE: keep this a real BOOLEAN. Assigning the st.sel OBJECT to panel.visible
      // below throws in Panorama and aborts the whole render (which previously blanked
      // Transform, froze the Lens sliders, and desynced the Path section).
      var has = (st.selected >= 0 && !!st.sel);
      selLbl.text = has ? ('Key #' + (st.selected + 1) + ' / ' + st.count) : (st.count > 0 ? '— / ' + st.count : 'no cameras');
      delBtn.visible = retimeBtn.visible = has;

      // Transform readout: the selected key if any, else the live free cam.
      var src = has ? st.sel : (st.cam || null);
      if (src) {
        posLbl.text = 'POS   ' + num(src.x).toFixed(1) + '   ' + num(src.y).toFixed(1) + '   ' + num(src.z).toFixed(1);
        angLbl.text = 'ANG   p ' + num(src.pitch).toFixed(1) + '   y ' + num(src.yaw).toFixed(1) + '   r ' + num(src.roll).toFixed(1);
      } else { posLbl.text = 'POS   -'; angLbl.text = 'ANG   -'; }
      xfHint.text = has ? ('tick ' + st.sel.tick + '   ·   editing selected camera')
                        : 'live free cam   ·   press K to place a camera';

      syncSliders();
      lensHint.text = has ? 'Drag to edit FOV / roll of the selected camera.'
                          : 'Select a camera to edit its lens.';

      interpBtn.__lbl.text = 'Curve: ' + (st.interp || 'Linear');
      easeBtn.__lbl.text = 'Ease: ' + EASE_LBL[(has && st.sel.ease) || 0];
      easeBtn.visible = has;
      smBtn.__lbl.text = 'Speed: ' + (st.speedMode || 'Manual');

      var perSeg = (st.speedMode === 'Per-Segment');
      segRow.visible = perSeg && has && !st.sel.isLast; // visible:false collapses the row
      if (has) segLbl.text = 'Seg x' + st.sel.speedMul.toFixed(2);

      conRow.visible = (st.speedMode === 'Constant');
      conLbl.text = 'Const x' + (st.constSpeed != null ? st.constSpeed.toFixed(2) : '1.00');

      var freeze = (st.timing === 'Freeze');
      timeBtn.__lbl.text = 'Timing: ' + (freeze ? 'Freeze' : 'Live');
      timeBtn.__lbl.style.color = freeze ? S.freeze : S.value;

      updateDebug(st); // viewport/HUD debug overlay (no-op unless st.debug)
    };

    $.CamEditor = api;
    api.render();
    $.Msg('[editor] camera editor workspace built.\n');
  } catch (err) {
    $.Msg('[editor] gui error: ' + err + '\n');
  }
})();
)EDJS";

} // namespace Filmmaker
