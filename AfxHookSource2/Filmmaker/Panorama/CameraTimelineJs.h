#pragma once

// Panorama JS for the camera TIMELINE scrub bar. Built ONCE into the in-game
// HUD (CSGOHud) context by CameraTimelineHud.cpp.
//
// While the panel is OPEN it REPLACES the native CS2 demo bar: the native
// CSGOHudDemoController contents (slider + transport + the G-mouse / next-player /
// next-camera hotkey labels) are hidden and ours occupies that bottom space; the
// native bar is restored when we close.
//
// UI-mouse mode (state.cursor): while the camera timeline is open it
// is forced on so the panel is always clickable. In regular native-demo-bar mode it
// can be toggled from third-person/freecam by G or the injected MOUSE button.
//
//   * TIMELINE: a native-styled scrubber (HorizontalSlider) + keyframe diamonds +
//     transport + speed buttons.
//
//   C++ -> JS : attribute "state" (light, every frame), then $.CamTimeline.render().
//   JS  -> C++: buttons / sliders issue "mirv_filmmaker camtl ..." console commands.

namespace Filmmaker {

inline const char* kCameraTimelineJs = R"TLJS(
(function () {
  try {
    var existing = $('#CamTimelineRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', freeze: '#4aa3ffff',
      bg: 'rgba(12,14,18,0.96)', bgSolid: 'rgba(12,14,18,1)', panelBorder: '#ffffff1f',
      track: '#ffffff1a', grid: '#ffffff0e', gridMid: '#ffffff1c',
      line: '#f0b323dd', lineSel: '#ffffffff', playhead: '#ff5a5aee',
      label: '#9aa4b0ff', value: '#eef2f6ff', btnBg: '#ffffff14', btnOn: '#f0b32333',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var W = 1250, W_DEFAULT = 1250, LABELW = 132;
    var EDITOR_INSPECTOR_W = 430; // keep in sync with CameraEditorJs INSPECTOR_W
    var EDITOR_BOTTOM_H = 176;    // keep in sync with CameraEditorJs BOTTOM_H
    var EDITOR_BOTTOM_LIFT = 28;  // lifted above the CS2 build/version text (editor's bottom tab bar sits ABOVE this bar)
    var HUD_INSET_X = 14, HUD_INSET_TOP = 12; // keep scaled CS2 HUD clear of the preview frame
    var EASE = ['none','in','out','inout'];
    var EASE_LBL = ['None','Ease In','Ease Out','Ease In/Out'];

    function disarmClear() {
      clearConfirm = false;
      if (clearBtn && clearBtn.__lbl) {
        clearBtn.__lbl.text = 'Clear';
        clearBtn.__lbl.style.color = S.value;
      }
    }
    function cmd(c) {
      // Any action disarms a pending "Clear — sure?" confirm.
      disarmClear();
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[camtl] cmd failed: ' + e + '\n'); }
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
      b.style.paddingTop = '4px'; b.style.paddingBottom = '4px';
      b.style.paddingLeft = '9px'; b.style.paddingRight = '9px';
      b.style.marginRight = '5px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, 13); l.style.fontWeight = 'bold';
      b.SetPanelEvent('onactivate', function () { if (b !== clearBtn) disarmClear(); onClick(); });
      b.__lbl = l; return b;
    }
    function diamond(parent, cx, cy, size, color, onClick) {
      var p = mk('Panel', parent);
      p.style.position = (cx - size / 2).toFixed(1) + 'px ' + (cy - size / 2).toFixed(1) + 'px 0px';
      p.style.width = size + 'px'; p.style.height = size + 'px';
      p.style.backgroundColor = color; p.style.transformOrigin = '50% 50%';
      p.style.transform = 'rotateZ(45deg)';
      p.hittest = !!onClick; if (onClick) p.SetPanelEvent('onactivate', onClick);
      return p;
    }

    // Find a native HUD panel by id (walk to the topmost ancestor, then traverse).
    function findNative(id) {
      var top = ctx; while (top.GetParent && top.GetParent()) top = top.GetParent();
      var p = top.FindChildTraverse && top.FindChildTraverse(id);
      if (!p && ctx.FindChildTraverse) p = ctx.FindChildTraverse(id);
      return p;
    }
    // Hide/show the native demo bar (CSGOHudDemoController "Contents": SliderRow + ControlRow), and
    // -- for the editor's Regular Timeline mode -- DOCK it to fit left of the inspector. We touch
    // ONLY width + horizontalAlign (on Contents and its root); the undock path resets BOTH to their
    // defaults (100% / center), so the bar restores pixel-perfect when the editor closes. (The old
    // dock also changed height/verticalAlign/margin and never reset them -> the broken-on-exit bar.)
    var nativeContents = null, nativeRoot = null, nativeBgOn = false;
    function ensureNative() {
      if (!nativeContents) {
        var sr = findNative('SliderRow');
        if (sr && sr.GetParent) { nativeContents = sr.GetParent(); nativeRoot = nativeContents.GetParent && nativeContents.GetParent(); }
      }
      return nativeContents;
    }
    function setNativeHidden(hide) {
      if (ensureNative()) nativeContents.visible = !hide;
    }
    function nativeDock(barW) {
      if (!ensureNative()) return;
      var w = Math.max(360, Math.floor(barW || 0));
      nativeContents.style.width = w + 'px'; nativeContents.style.horizontalAlign = 'left';
      // Give the docked bar the same opaque editor background as the camera/graph panels.
      // The editor's backdrop is dropped in Regular Timeline mode (it would cover this lower-z
      // native bar), so this is the ONLY fill behind the bar -- it must be FULLY opaque (bgSolid),
      // not the 0.96 S.bg the camera/graph use (those look solid only because they're layered over
      // the opaque backdrop; a single 0.96 layer here would let the game blur show through).
      nativeContents.style.backgroundColor = S.bgSolid;
      if (nativeRoot && nativeRoot.style) { nativeRoot.style.width = w + 'px'; nativeRoot.style.horizontalAlign = 'left'; nativeRoot.style.backgroundColor = S.bgSolid; }
      nativeBgOn = true;
      rescaleMarkers('RoundMarkers', true, true);
      rescaleMarkers('HighlightMarkers', true, false);
      rescaleMarkers('HighlightIcons', false, false);
      markersDocked = true; // docked geometry is now written on the native marker children
    }
    function nativeUndock() {
      if (!ensureNative()) return;
      // Restore width/align FIRST so the bar always un-docks, even if a later assignment fails.
      nativeContents.style.width = '100%'; nativeContents.style.horizontalAlign = 'center';
      if (nativeRoot && nativeRoot.style) { nativeRoot.style.width = '100%'; nativeRoot.style.horizontalAlign = 'center'; }
      // Only clear the fill we added (NOTE: Panorama rejects '' as a style value -> use a fully
      // transparent color), and skip it entirely when we never docked so CS2's own frosted
      // demo-bar background is left untouched during normal playback.
      if (nativeBgOn) {
        nativeContents.style.backgroundColor = 'rgba(0,0,0,0)';
        if (nativeRoot && nativeRoot.style) nativeRoot.style.backgroundColor = 'rgba(0,0,0,0)';
        nativeBgOn = false;
      }
      // Marker restore/capture state machine (see rescaleMarkers below). The docked geometry we
      // wrote onto the marker children does NOT go away when the container returns to 100% --
      // the native script never re-lays them out -- so we must actively write the basis back.
      // While our docked styles are still applied, keep REAPPLYING the basis scaled to the live
      // container width (which is animating back to the full native width); once the container
      // reads its basis width again the children are pixel-identical to their native geometry.
      // Only AFTER that (plus a couple frames of style->actual-layout lag) resume capturing the
      // basis from live reads. Capturing while our docked styles were still on the children was
      // the compounding-shrink bug: every open/close cycle baked the shrunken left/width in as
      // the new "basis", so the kill/round icons kept compressing and never lined up again.
      if (markersDocked) {
        rescaleMarkers('RoundMarkers', true, true);
        rescaleMarkers('HighlightMarkers', true, false);
        rescaleMarkers('HighlightIcons', false, false);
        if (markersAtBasisWidth()) { markersDocked = false; markerCaptureHold = 2; }
      } else if (markerCaptureHold > 0) {
        markerCaptureHold--;
      } else {
        // Fully restored: refresh the basis every frame while undocked -- cheap, and self-heals
        // if the native script recreates the markers (round data changed, spectated player
        // switched) while undocked.
        captureMarkerBasis('RoundMarkers');
        captureMarkerBasis('HighlightMarkers');
        captureMarkerBasis('HighlightIcons');
      }
    }
    // ---- Round/kill marker rescale -----------------------------------------------------------
    // CS2's own huddemocontroller.js positions the round-shade bars (#RoundMarkers), the
    // highlight-reel intervals (#HighlightMarkers) and the kill/death icons (#HighlightIcons) as
    // ABSOLUTE PIXEL children, computed ONCE against the bar's width at that moment: RoundMarkers
    // is guarded by an internal one-shot flag that never fires again, and the other two are only
    // recomputed when the spectated player changes. Docking the bar narrower for the editor's
    // Regular Timeline mode resizes Contents/Root (and RoundMarkers/HighlightMarkers/HighlightIcons
    // correctly cascade to 100% of that via native CSS -- verified live: container 1578px->1099px),
    // but the existing marker children keep their STALE full-width geometry (verified live: a
    // marker's actuallayoutwidth read 71px in BOTH states) -- they don't shrink with their parent,
    // so the round-shade boundaries / kill icons no longer line up with the (correctly rescaled)
    // slider thumb: the playhead ends up visually inside the wrong round's shading.
    // Fix: capture each marker's left offset + width while UNDOCKED (the bar's true native width
    // == the basis the native script computed against), then reapply those figures scaled to the
    // CURRENT container width every time we dock. Must read the BASIS ONCE and always scale FROM
    // it (not from the live value) -- rescaling from an already-rescaled live read would compound
    // the scale factor every frame.
    var markerBasis = {}; // id -> { w: container width at capture (device px), items: [{left,width}] }
    var markersDocked = false;  // docked geometry is currently written on the native marker children
    var markerCaptureHold = 0;  // undocked frames to wait after a restore before trusting live reads
    // True once every marker container with a captured basis is laid out at its basis width
    // again (i.e. the undock has visually settled and a scale-1 rescale restored the children).
    function markersAtBasisWidth() {
      for (var id in markerBasis) {
        var b = markerBasis[id]; if (!b || !b.w) continue;
        var p = findNative(id); if (!p) continue;
        // Child count changed = the native script rebuilt this set with fresh geometry while we
        // were docked; none of our styles are on the new children, so it can't block settling.
        if (p.GetChildCount && p.GetChildCount() !== b.items.length) continue;
        if (Math.abs((p.actuallayoutwidth || 0) - b.w) > 1.5) return false;
      }
      return true;
    }
    function captureMarkerBasis(id) {
      var p = findNative(id);
      if (!p || !p.GetChildCount) return;
      var n = p.GetChildCount();
      if (n === 0) { delete markerBasis[id]; return; }
      var items = [];
      for (var i = 0; i < n; i++) {
        var c = p.GetChild(i);
        items.push({ left: c.actualxoffset || 0, width: c.actuallayoutwidth || 0 });
      }
      markerBasis[id] = { w: p.actuallayoutwidth || 0, items: items };
    }
    // scaleWidth=false for HighlightIcons: those are fixed-size (22px) CSS icons -- rescale their
    // left offset only, don't stretch/squish the icon itself. usePosition matches whichever
    // property the NATIVE script used to place that element, so we don't change its layout
    // behavior: RoundMarkers children use style.position (no flow-children on the parent, so the
    // CSS margin-top:30px combines with our y=0); HighlightMarkers/HighlightIcons children use
    // style.marginLeft (switching those to style.position would drop HighlightIcons' CSS
    // vertical-align:center, since position sets an explicit y and takes the child out of flow).
    function rescaleMarkers(id, scaleWidth, usePosition) {
      var basis = markerBasis[id];
      var p = findNative(id);
      if (!basis || !basis.w || !p || !p.GetChildCount) return;
      var n = p.GetChildCount();
      if (n !== basis.items.length) return; // native script is mid-rebuild (count changed); wait for the next undocked capture
      var curW = p.actuallayoutwidth || 0;
      var scale = curW / basis.w;
      if (!isFinite(scale) || scale <= 0) return;
      var uiscale = p.actualuiscale_x || ctx.actualuiscale_x || 1;
      for (var i = 0; i < n; i++) {
        var c = p.GetChild(i), it = basis.items[i];
        var leftPx = ((it.left * scale) / uiscale).toFixed(1);
        if (usePosition) c.style.position = leftPx + 'px 0px 0px';
        else c.style.marginLeft = leftPx + 'px';
        if (scaleWidth) c.style.width = ((it.width * scale) / uiscale).toFixed(1) + 'px';
      }
    }
)TLJS"
R"TLJS(
    // Patch the LIVE native demo bar (CSGOHudDemoController): remove the
    // next-camera / next-player / mouse-cursor hotkey hints, and inject our
    // "MOUSE" + "CAM EDITOR" buttons into it (idempotent; re-adds if the bar is recreated).
    // Runs every frame while in a demo so the clutter never reappears.
    //
    // The injected buttons are HIDDEN only when the editor replaces the native bar with the
    // camera-timeline or graph overlay (hosted + open / graphExp). In the editor's default NATIVE
    // bottom mode the native bar IS the bottom panel, so we keep the buttons present on it.
    function patchNativeBar() {
      var ids = ['HotKey_Next_Camera', 'HotKey_Player_Next', 'HotKey_Toggle_Mouse_Cursor'];
      for (var i = 0; i < ids.length; i++) { var p = findNative(ids[i]); if (p) p.visible = false; }
      // "End Playback" and the native gear (opens CS2's own X-Ray/True View panel, which our Camera
      // Editor's own ⚙ menu replaces) are suppressed PERMANENTLY from the moment a demo starts --
      // not just while the Camera Editor is open. This runs every frame regardless of editor state
      // (patchNativeBar is called unconditionally from CameraTimelineHud, which is always active
      // during demo playback), so unlike the editor-only CameraEditorJs.h version this doesn't
      // reappear when the editor is closed or before it's ever opened.
      var endBtn = findNative('EndPlayback');
      if (endBtn) endBtn.visible = false;
      var gearBtn = findNative('SettingsButton');
      if (gearBtn) gearBtn.visible = false;
      var host = findNative('HotKeyLabels') || findNative('ControlRow');
      if (!host) return;
      function childIndex(p) {
        if (!p || !host.GetChildCount) return -1;
        var n = host.GetChildCount();
        for (var ci = 0; ci < n; ci++) if (host.GetChild(ci) === p) return ci;
        return -1;
      }
      var hosted = !!(st && st.hosted);
      var cfgOpenBar = !!(st && st.configOpen);
      var oldMouse = host.FindChildTraverse && host.FindChildTraverse('CamCursorBtn');
      var oldEditor = host.FindChildTraverse && host.FindChildTraverse('CamEditorBtn');
      var oldConfig = host.FindChildTraverse && host.FindChildTraverse('ConfigBtn');
      var oldSwap = host.FindChildTraverse && host.FindChildTraverse('ModeSwapBtn');
      // Ordering guard (MOUSE -> CAM EDITOR -> CONFIG): if any of the three is missing while a
      // later one exists, or any pair is out of order, delete them all and rebuild next frame.
      var badOrder = (!oldMouse && (oldEditor || oldConfig)) || (!oldEditor && oldConfig)
        || (oldMouse && oldEditor && childIndex(oldMouse) > childIndex(oldEditor))
        || (oldEditor && oldConfig && childIndex(oldEditor) > childIndex(oldConfig));
      if (badOrder) {
        try { if (oldMouse) oldMouse.DeleteAsync(0); } catch (orderErr1) {}
        try { if (oldEditor) oldEditor.DeleteAsync(0); } catch (orderErr2) {}
        try { if (oldConfig) oldConfig.DeleteAsync(0); } catch (orderErr3) {}
        return;
      }
      function nativeBtn(id, text, onactivate) {
        var b = host.FindChildTraverse && host.FindChildTraverse(id);
        if (!b) {
          b = $.CreatePanel('Panel', host, id, {});
          b.hittest = true;
          b.style.height = '34px'; b.style.verticalAlign = 'center'; b.style.marginRight = '8px';
          b.style.paddingLeft = '12px'; b.style.paddingRight = '12px';
          b.style.borderRadius = '3px';
          var l = $.CreatePanel('Label', b, '', {});
          l.style.fontWeight = 'bold'; l.style.fontSize = '15px';
          l.style.verticalAlign = 'center'; l.style.horizontalAlign = 'center';
          b.__lbl = l;
          b.SetPanelEvent('onactivate', onactivate);
        } else if (!b.__lbl) {
          b.__lbl = (b.GetChildCount && b.GetChildCount() > 0) ? b.GetChild(0) : $.CreatePanel('Label', b, '', {});
        }
        b.__lbl.text = text;
        return b;
      }
      // Workspace open (Camera Editor hosted OR Config): the standalone MOUSE / CAM EDITOR /
      // CONFIG buttons collapse into ONE swap button that flips to the OTHER workspace --
      // Config shows "CAM EDITOR" (gold), the editor's Regular Timeline shows "CONFIG" (blue).
      // The workspaces provide their own mouse toggle (G / inspector button), so the injected
      // MOUSE button must NOT appear in the bar while either is open.
      if (hosted || cfgOpenBar) {
        try { if (oldMouse) oldMouse.visible = false; } catch (hm) {}
        try { if (oldEditor) oldEditor.visible = false; } catch (he) {}
        try { if (oldConfig) oldConfig.visible = false; } catch (hc) {}
        var sb = nativeBtn('ModeSwapBtn', cfgOpenBar ? 'CAM EDITOR' : 'CONFIG', function () {
          // Read the CURRENT state at click time (one shared handler for both directions).
          if (st && st.configOpen) cmd('mirv_filmmaker editor on');
          else cmd('mirv_filmmaker config on');
        });
        sb.style.backgroundColor = cfgOpenBar ? '#f0b32333' : '#4aa3ff33';
        sb.__lbl.style.color = cfgOpenBar ? S.accent : S.freeze;
        sb.visible = true;
        return;
      }
      try { if (oldSwap) oldSwap.visible = false; } catch (hsw) {}
      var mb = nativeBtn('CamCursorBtn', 'MOUSE (G)', function () { cmd('mirv_filmmaker camtl cursor toggle'); });
      var curOn = !!(st && st.cursor);
      mb.style.backgroundColor = curOn ? '#c92a2acc' : '#ffffff14';
      mb.__lbl.style.color = curOn ? '#ffffffff' : S.label;
      mb.visible = true; // re-show if we hid it for a previous overlay bottom mode
      var cb = nativeBtn('CamEditorBtn', 'CAM EDITOR', function () { cmd('mirv_filmmaker editor toggle'); });
      cb.style.backgroundColor = '#f0b32333';
      cb.__lbl.style.color = S.accent;
      cb.visible = true;
      // CONFIG: the lightweight UI/display settings panel (ConfigHud), BLUE so the two
      // workspaces read as distinct siblings.
      var gb = nativeBtn('ConfigBtn', 'CONFIG', function () { cmd('mirv_filmmaker config toggle'); });
      gb.style.backgroundColor = '#4aa3ff33';
      gb.__lbl.style.color = S.freeze;
      gb.visible = true;
    }

    // Legacy cleanup hook: if an older build hid native HUD siblings, restore them. The
    // camera timeline HUD toggle is disabled; this must not hide anything anymore.
    // CamEditorConfirmRoot/CamEditorSettingsRoot: promoted to TOP-LEVEL siblings of CamEditorRoot
    // (see CameraEditorJs.h z-layer map) so their z-index beats the Graph/Timeline roots -- which
    // also makes them direct ctx children like CamEditorRoot, so they must be whitelisted here too
    // or this "Hide All" sweep force-hides them the instant they open (no visible symptom besides
    // the popup silently vanishing -- it never goes through closeSettings(), so nothing logs it).
    var OURS = { 'CamTimelineRoot': 1, 'MarkerHudRoot': 1, 'MovieHudRoot': 1, 'CamEditorRoot': 1, 'GraphExpRoot': 1, 'CamEditorConfirmRoot': 1, 'CamEditorSettingsRoot': 1, 'ConfigHudRoot': 1, 'ConfigSettingsRoot': 1 };
    var VIEWPORT_HUD_ROOTS = {
      'HudBottomGradient': 1,
      'HudInWorld': 1,
      'HudTopCenter': 1,
      'HudTopLeft': 1,
      'HudLowerLeft': 1,
      'HudBottomCenter': 1,
      'HudBottomRight': 1,
      'HudTopRight': 1,
      'HudCompass': 1,
      'HudChat': 1,
      'HudRadar': 1,
      'HudMoney': 1,
      'HudHealthArmor': 1,
      'jsHudHealthArmorAmmoMore': 1,
      'HudWeaponSelection': 1,
      'HudDeathNotice': 1,
      'ScoreAndTimeAndBomb': 1,
      'HudTeamCounter': 1,
      'HudWinPanel': 1,
      'HudSpectatorVignetting': 1,
      'HudRadio': 1,
      'HudInstructor': 1,
      'HudVote': 1,
      'HudRosettaSelector': 1,
      'MapOverview': 1,
      'HudPerfStatsBasics': 1,
      'Scoreboard': 1,
      'HudDeathPanel': 1
    };
    // Spectator observer chrome stripped for the "in-game" look: the player name/clan/weapon
    // side bar, the centred player avatar, and the spectator vignette. Radar + HP/ammo stay.
    var SPEC_PANELS = ['jsHudSpecplayer__Bg', 'HudSpecplayer__Avatar', 'HudSpectatorVignetting'];
    var hiddenNatives = null; // [{p, v, id}] captured while ANY panel is hidden; null while fully shown
    var hiddenNativeIds = {};
    var curHudView = 'full';
    function restoreGameHud() {
      if (hiddenNatives) {
        for (var j = 0; j < hiddenNatives.length; j++) {
          var h = hiddenNatives[j];
          try { if (h.p && !(h.p.IsValid && !h.p.IsValid())) h.p.visible = h.v; } catch (e2) {}
          // The native HUD can rebuild while the editor is open. Restore by id too so panels hidden
          // by an older JS closure do not stay hidden after leaving the camera editor.
          try {
            if (h.id) {
              var cur = ctx.FindChildTraverse && ctx.FindChildTraverse(h.id);
              if (cur) cur.visible = h.v;
            }
          } catch (e3) {}
        }
      }
      hiddenNatives = null;
      hiddenNativeIds = {};
    }
    function forceFullHudPanels() {
      // Defensive restore for the spectator HUD pieces that "In-Game" mode hides. If the script was
      // rebuilt while those panels were already hidden, hiddenNatives may not contain them.
      for (var i = 0; i < SPEC_PANELS.length; i++) {
        try {
          var p = ctx.FindChildTraverse && ctx.FindChildTraverse(SPEC_PANELS[i]);
          if (p) p.visible = true;
        } catch (e) {}
      }
    }
    function hidePanel(c, forceRestoreVisible) {
      if (!c) return;
      var id = '';
      try { id = c.id || ''; } catch (eid) {}
      if (!hiddenNatives) hiddenNatives = [];
      var key = id || ('__panel_' + hiddenNatives.length);
      if ((forceRestoreVisible || c.visible) && !hiddenNativeIds[key]) {
        hiddenNativeIds[key] = 1;
        hiddenNatives.push({ p: c, v: forceRestoreVisible ? true : !!c.visible, id: id });
      }
      try { c.visible = false; } catch (e) {}
    }
    // view: 'full' (show everything) | 'hidden' (hide the whole gameplay HUD) | 'game' (keep
    // radar + HP/ammo, drop the spectator observer panel). Called every frame so a panel the
    // game re-shows (round start, etc.) gets re-hidden; a mode change restores the baseline first.
    function applyHudView(view) {
      if (view !== curHudView) { restoreGameHud(); curHudView = view; }
      if (view === 'full') { forceFullHudPanels(); return; }
      if (!hiddenNatives) hiddenNatives = [];
      if (view === 'hidden') {
        var nk = ctx.GetChildCount ? ctx.GetChildCount() : 0;
        for (var i = 0; i < nk; i++) {
          var c = ctx.GetChild(i);
          if (!c || OURS[c.id]) continue;
          if ((c.FindChildTraverse && (c.FindChildTraverse('SliderRow') || c.FindChildTraverse('ControlRow')))) continue;
          if (c.visible) hidePanel(c, false);
        }
      } else { // 'game'
        for (var s = 0; s < SPEC_PANELS.length; s++)
          hidePanel(ctx.FindChildTraverse && ctx.FindChildTraverse(SPEC_PANELS[s]), true);
      }
    }
)TLJS"
R"TLJS(
    var viewportHudPanels = [];
    function isOurPanel(p) {
      return !!(p && OURS[p.id]);
    }
    function isViewportHudRoot(p) {
      return !!(p && VIEWPORT_HUD_ROOTS[p.id]);
    }
    function isNativeDemoBarPanel(p) {
      return !!(p && p.FindChildTraverse && (p.FindChildTraverse('SliderRow') || p.FindChildTraverse('ControlRow')));
    }
    function restoreHudViewport() {
      for (var i = 0; i < viewportHudPanels.length; i++) {
        var p = viewportHudPanels[i];
        try {
          if (!p || (p.IsValid && !p.IsValid())) continue;
          // Snap the transform back with NO animation, then re-arm the panel's own transition on
          // the next frame -- once the transform has already settled -- so toggling back to Hide All
          // doesn't trigger an animated "slide out", and native HUD anims still work after we let go.
          p.style.transition = 'none';
          p.style.transformOrigin = p.__camViewportOrigin || '50% 50%';
          p.style.transform = p.__camViewportTransform || 'none';
          p.__camViewportAdjusted = false;
          (function (panel, trans) {
            $.Schedule(0, function () {
              try { if (!panel || (panel.IsValid && !panel.IsValid())) return; panel.style.transition = trans; } catch (e3) {}
            });
          })(p, p.__camViewportTransition || '');
        } catch (e) {}
      }
      viewportHudPanels = [];
      forceClearViewportTransforms();
    }
    // Defensive: the tracked viewportHudPanels list lives in THIS script closure, but the native
    // HUD (and so this whole timeline script) can rebuild while the editor is open. After such a
    // rebuild the live HUD roots still carry the editor's scale3d/translate3d + __camViewportAdjusted
    // from the PREVIOUS closure, yet the new closure's tracked list is empty -- so the loop above
    // restores nothing and the gameplay HUD stays shrunk/offset (not lined up with the now full-screen
    // world) after leaving the camera editor. Sweep the live HUD roots by their own flag and clear any
    // orphaned transform. Mirrors the visibility-side forceFullHudPanels() guard.
    function forceClearViewportTransforms() {
      var n = ctx.GetChildCount ? ctx.GetChildCount() : 0;
      for (var i = 0; i < n; i++) {
        var c = ctx.GetChild(i);
        if (!c || !isViewportHudRoot(c) || !c.__camViewportAdjusted) continue;
        try {
          c.style.transition = 'none';
          c.style.transformOrigin = c.__camViewportOrigin || '50% 50%';
          c.style.transform = c.__camViewportTransform || 'none';
          c.__camViewportAdjusted = false;
        } catch (e) {}
      }
    }
    function rememberHudViewportPanel(p) {
      if (!p || p.__camViewportAdjusted) return;
      try {
        p.__camViewportTransform = p.style.transform || 'none';
        p.__camViewportOrigin = p.style.transformOrigin || '50% 50%';
        p.__camViewportTransition = p.style.transition || '';
        p.__camViewportAdjusted = true;
        viewportHudPanels.push(p);
      } catch (e) {}
    }
    function applyGameHudViewport(active, rect) {
      if (!active || !rect || rect.length < 4) { restoreHudViewport(); return; }
      var rsx = ctx.actualuiscale_x || root.actualuiscale_x || 1;
      var rsy = ctx.actualuiscale_y || root.actualuiscale_y || 1;
      var rawW = ctx.actuallayoutwidth || root.actuallayoutwidth || 0;
      var rawH = ctx.actuallayoutheight || root.actuallayoutheight || 0;
      var rw = rawW / rsx, rh = rawH / rsy;
      if (!(rw > 1 && rh > 1)) { restoreHudViewport(); return; }
      var x0 = Number(rect[0]), y0 = Number(rect[1]), x1 = Number(rect[2]), y1 = Number(rect[3]);
      if (!(isFinite(x0) && isFinite(y0) && isFinite(x1) && isFinite(y1) && x1 > x0 && y1 > y0)) {
        restoreHudViewport(); return;
      }
      // Map the FULL screen 1:1 into the EXACT preview rect the world blit uses -- NO insets, so
      // the HUD lines up pixel-for-pixel with the scaled game view (the rect already matches the
      // game aspect, so sx == sy). x0*rw / y0*rh is the rect's top-left in virtual px.
      var sx = x1 - x0, sy = y1 - y0;
      var tx = x0 * rw, ty = y0 * rh;
      var n = ctx.GetChildCount ? ctx.GetChildCount() : 0;
      for (var i = 0; i < n; i++) {
        var c = ctx.GetChild(i);
        if (!c || isOurPanel(c) || isNativeDemoBarPanel(c) || !isViewportHudRoot(c)) continue;
        rememberHudViewportPanel(c);
        try {
          // transform-origin stays '0% 0%' (each panel's OWN top-left) -- the only form Panorama
          // reliably accepts; a 3-value/px origin throws and silently drops the whole transform.
          // To still scale uniformly about the SCREEN origin (so RIGHT/BOTTOM-anchored panels like
          // death notices / ammo land inside the rect, not under the inspector), we compensate in
          // the translate: a panel laid out at screen (px,py) needs translate (tx-(1-sx)*px, ...).
          // Full-screen panels have px=py=0 -> this reduces to the plain (tx,ty) mapping, and if an
          // offset is unavailable it falls back to that same safe behaviour.
          var px = (c.actualxoffset || 0) / rsx, py = (c.actualyoffset || 0) / rsy;
          var ptx = tx - (1 - sx) * px, pty = ty - (1 - sy) * py;
          // Panorama applies the listed translate before the scale for these HUD panels, so the
          // translate itself gets scaled. Feed it pre-divided values to produce screen-pixel tx/ty.
          var cssTx = ptx / sx, cssTy = pty / sy;
          c.style.transition = 'none'; // snap into the preview rect; no animated slide/scale
          c.style.transformOrigin = '0% 0%';
          c.style.transform = 'translate3d(' + cssTx.toFixed(1) + 'px, ' + cssTy.toFixed(1) + 'px, 0px) scale3d('
            + sx.toFixed(5) + ', ' + sy.toFixed(5) + ', 1)';
        } catch (e2) {}
      }
    }

    // ---- root + hit catcher + panel ------------------------------------
    var root = $.CreatePanel('Panel', ctx, 'CamTimelineRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '55'; // z-layer map: PanoramaBridge.h
    var catcher = mk('Panel', root); catcher.hittest = true; catcher.visible = false;
    catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.SetPanelEvent('onactivate', function () { /* swallow stray clicks */ });

    var panel = $.CreatePanel('Panel', root, 'CamTimelineBar', {}); panel.hittest = true;
    var ps = panel.style;
    ps.horizontalAlign = 'center'; ps.verticalAlign = 'bottom'; ps.marginBottom = '0px';
    ps.width = (LABELW + W + 28) + 'px';
    ps.backgroundColor = S.bg; ps.borderRadius = '6px'; ps.border = '1px solid ' + S.panelBorder;
    ps.boxShadow = '#000000cc 0px 0px 12px 2px'; // native demo-bar style depth
    ps.flowChildren = 'down'; ps.paddingTop = '8px'; ps.paddingBottom = '10px';
    ps.paddingLeft = '14px'; ps.paddingRight = '14px'; ps.fontFamily = S.font;

    // ===================== HEADER =======================================
    var header = mk('Panel', panel); header.style.flowChildren = 'right'; header.style.width = '100%';
    header.style.marginBottom = '8px';
    var hTitle = lbl(header, 'CAMERA TIMELINE', S.value, 15); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.verticalAlign = 'center';
    var hInfo = lbl(header, '', S.label, 12); hInfo.style.marginLeft = '16px';
    hInfo.style.width = 'fill-parent-flow(1.0)'; hInfo.style.verticalAlign = 'center';
    var mouseLbl = lbl(header, '', S.accent, 12); mouseLbl.style.verticalAlign = 'center'; mouseLbl.style.marginRight = '10px';
    var clearConfirm = false;
    var clearBtn = btn(header, 'Clear', function () {
      if (!clearConfirm) { clearConfirm = true; clearBtn.__lbl.text = 'Clear — sure?'; clearBtn.__lbl.style.color = S.accent; return; }
      clearConfirm = false; clearBtn.__lbl.text = 'Clear'; clearBtn.__lbl.style.color = S.value;
      cmd('mirv_filmmaker camtl clear');
    }, S.value);
    // ("Curve Editor" view toggle removed: the graph editor is the curve editor now.)
    var closeBtn = btn(header, '✕', function () { cmd('mirv_filmmaker camtl close'); }, S.value);
    // In the Camera Editor, switch the bottom panel back to the (default) graph editor.
    var graphBtn = btn(header, '≡ Graph', function () { cmd('mirv_filmmaker editor curveeditor graph'); }, S.accent);
)TLJS"
R"TLJS(
    // ===================== SHARED TRANSPORT =============================
    var trow = mk('Panel', panel); trow.style.flowChildren = 'right'; trow.style.width = '100%'; trow.style.marginBottom = '8px';
    var playBtn = btn(trow, '▶', function () {
      var isPlaying = transportShownPlaying();
      transportOverride = !isPlaying;
      transportOverrideUntil = nowMs() + 2500;
      setTransportButton(transportOverride);
      cmd(isPlaying ? 'mirv_filmmaker camtl pause' : 'mirv_filmmaker camtl play');
    }, S.accent);
    playBtn.style.width = '33px';
    playBtn.style.paddingLeft = '0px';
    playBtn.style.paddingRight = '0px';
    playBtn.__lbl.style.width = '100%';
    playBtn.__lbl.style.textAlign = 'center';
    btn(trow, '⏮', function () { gotoKey(-1); }, S.value);
    btn(trow, '⏭', function () { gotoKey(1); }, S.value);
    btn(trow, '◀ 1', function () { if (st) cmd('mirv_filmmaker camtl scrub ' + (activeTick() - 1)); }, S.value);
    btn(trow, '1 ▶', function () { if (st) cmd('mirv_filmmaker camtl scrub ' + (activeTick() + 1)); }, S.value);
    var tReadout = lbl(trow, '', S.value, 13); tReadout.style.verticalAlign = 'center';
    tReadout.style.marginLeft = '10px'; tReadout.style.width = 'fill-parent-flow(1.0)';
    var SPD = [0.1, 0.25, 0.5, 1, 2, 4];
    var speedBtns = [], activeSpeed = 1;
    function updateSpeedButtons() {
      for (var bi = 0; bi < speedBtns.length; bi++) {
        var on = Math.abs(speedBtns[bi].speed - activeSpeed) < 0.0001;
        speedBtns[bi].panel.style.backgroundColor = on ? S.btnOn : S.btnBg;
        speedBtns[bi].panel.__lbl.style.color = on ? S.accent : S.label;
      }
    }
    for (var si = 0; si < SPD.length; si++) (function (v) {
      var sb = btn(trow, (v < 1 ? v : v + '') + 'x', function () {
        activeSpeed = v; updateSpeedButtons(); cmd('demo_timescale ' + v);
      }, S.label);
      speedBtns.push({ panel: sb, speed: v });
    })(SPD[si]);
    updateSpeedButtons();

    // ===================== TIMELINE VIEW ================================
    var tl = mk('Panel', panel); tl.style.flowChildren = 'down'; tl.style.width = (LABELW + W) + 'px';

    // Timeline scrubber spans the full content width.
    var TLW = LABELW + W;
    var srow = mk('Panel', tl); srow.style.width = TLW + 'px'; srow.style.height = '40px';
    var diamWrap = mk('Panel', srow); diamWrap.hittest = false;
    diamWrap.style.width = TLW + 'px'; diamWrap.style.height = '18px'; diamWrap.style.position = '0px 0px 0px';
    // (No separate track panel: the native Slider draws its own groove, so a second
    // trackBg here produced a doubled line that ran past the keyframes.)
    // Native CS2 sliders need BOTH the class (styling) and the direction ATTRIBUTE
    // (drag axis): e.g. <Slider class="HorizontalSlider" direction="horizontal"/>.
    // Without direction the Slider defaults to vertical, so the thumb drags up/down
    // instead of left/right -- pass it in the CreatePanel construction props.
    var scrub = $.CreatePanel('Slider', srow, 'CamScrub', { direction: 'horizontal' }); scrub.AddClass('HorizontalSlider');
    scrub.style.width = TLW + 'px'; scrub.style.height = '24px'; scrub.style.position = '0px 16px 0px';
)TLJS"
R"TLJS(
    // ===================== SHARED KEYFRAME FOOTER =======================
    var keyFooter = mk('Panel', panel); keyFooter.style.flowChildren = 'right'; keyFooter.style.width = '100%';
    keyFooter.style.marginTop = '10px'; keyFooter.style.verticalAlign = 'middle';
    var K = {};
    K.kLabel = lbl(keyFooter, 'Keyframe', S.label, 13); K.kLabel.style.verticalAlign = 'center'; K.kLabel.style.marginRight = '10px';
    K.add = btn(keyFooter, '+ Add', function () { cmd('mirv_filmmaker camtl addkey'); }, S.accent);
    K.del = btn(keyFooter, '− Delete', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl delkey ' + st.selected); }, S.value);
    K.retime = btn(keyFooter, '⟲ Tick→Here', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl movekey ' + st.selected + ' ' + st.tick); }, S.value);
    K.ease = btn(keyFooter, 'Ease: None', function () {
      if (!st || st.selected < 0) return;
      var nx = ((st.selEase || 0) + 1) % 4; cmd('mirv_filmmaker camtl ease ' + st.selected + ' ' + EASE[nx]);
    }, S.value);
    K.spdMinus = btn(keyFooter, '−', function () { stepSpeed(-1); }, S.accent);
    K.spdLbl = lbl(keyFooter, 'Seg x1.00', S.value, 13); K.spdLbl.style.verticalAlign = 'center'; K.spdLbl.style.width = '92px'; K.spdLbl.style.textAlign = 'center';
    K.spdPlus = btn(keyFooter, '+', function () { stepSpeed(1); }, S.accent);
    K.interp = btn(keyFooter, 'Curve: Linear', function () { cmd('mirv_filmmaker camtl interp'); }, S.value); K.interp.style.marginLeft = '12px';
    K.sm = btn(keyFooter, 'Manual', function () { cmd('mirv_filmmaker marker speedmode cycle'); }, S.value);
    K.tm = btn(keyFooter, 'Live', function () { cmd('mirv_filmmaker marker timing toggle'); }, S.value);

    // =====================================================================
    var st = null;
    var graphExpActive = false; // experimental graph editor owns the curve zone -> scrub drives IT
    var lastTlSig = '', lastContentW = -1;
    // Dynamically-created Panorama sliders work in their default 0..1 range (setting
    // min/max/out-of-range value is unreliable and leaves the thumb stuck mid-track), so
    // we keep value normalized and map 0..1 <-> tick / channel-value ourselves.
    var scrubT0 = 0, scrubT1 = 1;
    var scrubSyncing = false;
    var transportOverride = null, transportOverrideUntil = 0;
    function nowMs() { return (new Date()).getTime(); }
    function transportShownPlaying() {
      return transportOverride !== null ? transportOverride : !!(st && st.playing);
    }
    function syncTransportButton() {
      if (st && transportOverride !== null && (st.playing === transportOverride || nowMs() > transportOverrideUntil)) {
        transportOverride = null;
      }
      setTransportButton(transportShownPlaying());
    }
    function setTransportButton(playing) {
      playBtn.__lbl.text = playing ? '▮▮' : '▶';
      playBtn.__lbl.style.fontSize = playing ? '15px' : '13px';
    }
    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function sliderTick(v, a, b) { return Math.round(a + v * (b - a)); }

    function frac(tick, t0, t1) { var d = t1 - t0; return d > 0 ? (tick - t0) / d : 0; }
    function activeTick() {
      return st && st.scrubbing ? st.scrubTick : (st ? st.tick : 0);
    }
    function gotoKey(dir) {
      if (!st || !st.markers || st.markers.length === 0) return;
      var cur = st.tick, bestI = -1, bestT = null;
      for (var i = 0; i < st.markers.length; i++) {
        var t = st.markers[i].tick;
        if (dir < 0 && t < cur) { if (bestT === null || t > bestT) { bestT = t; bestI = i; } }
        if (dir > 0 && t > cur) { if (bestT === null || t < bestT) { bestT = t; bestI = i; } }
      }
      if (bestI >= 0) cmd('mirv_filmmaker camtl select ' + bestI);
    }
    function stepSpeed(d) {
      if (!st || st.selected < 0) return;
      var steps = [0.2, 0.5, 0.8, 1.0], cur = st.selSpeedMul, idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; i++) { var dd = Math.abs(steps[i] - cur); if (dd < best) { best = dd; idx = i; } }
      idx += (d > 0 ? 1 : -1); if (idx < 0) idx = 0; if (idx >= steps.length) idx = steps.length - 1;
      cmd('mirv_filmmaker camtl speed ' + st.selected + ' ' + steps[idx].toFixed(2));
    }
    // CS2 sliders fire SliderValueChanged (while dragging) + SliderReleased (on let-go),
    // delivered via $.RegisterEventHandler with the value as the 2nd arg -- NOT the
    // 'onvaluechanged' panel event. While dragging we PREVIEW the camera (smooth, no
    // world seek); on release we seek the demo world to the final tick.
    $.RegisterEventHandler('SliderValueChanged', scrub, function (panel, v) {
      if (scrubSyncing || (st && st.playing)) return;
      var t = sliderTick(v, scrubT0, scrubT1);
      tReadout.text = 'tick ' + t + '   (release to seek)';
      // While the experiment owns the curve zone, the scrubber previews ITS curves (not the stable
      // path's) so the two never fight for the camera -- same commands the experiment's own ruler uses.
      if (graphExpActive) cmd('mirv_filmmaker grapheditor playhead ' + t);
      else cmd('mirv_filmmaker camtl scrubpreview ' + t);
    });
    $.RegisterEventHandler('SliderReleased', scrub, function (panel, v) {
      if (st && st.playing) return;
      var t = sliderTick(v, scrubT0, scrubT1);
      if (graphExpActive) { cmd('demo_gototick ' + t); cmd('mirv_filmmaker grapheditor playhead release'); }
      else cmd('mirv_filmmaker camtl scrub ' + t);
    });
    function rebuildTimelineDiamonds() {
      diamWrap.RemoveAndDeleteChildren();
      if (!st || !st.markers) return;
      var t0 = st.tickMin, t1 = st.tickMax; if (t1 <= t0) t1 = t0 + 1;
      for (var i = 0; i < st.markers.length; i++) (function (i) {
        var x = frac(st.markers[i].tick, t0, t1) * TLW;
        var sel = (i === st.selected);
        diamond(diamWrap, x, 8, 14, sel ? S.lineSel : S.accent, function () {
          cmd('mirv_filmmaker camtl select ' + i);
        });
      })(i);
    }
    function updateKeys(freeze) {
      var has = (st && st.selected >= 0);
      K.kLabel.text = has ? ('Key #' + (st.selected + 1)) : 'Keyframe (none)';
      K.del.visible = K.retime.visible = K.ease.visible = has;
      K.ease.__lbl.text = 'Ease: ' + EASE_LBL[(st && st.selEase) || 0];
      K.spdLbl.text = 'Seg x' + (st && st.selSpeedMul != null ? st.selSpeedMul.toFixed(2) : '1.00');
      var perSeg = (st && st.speedMode === 'Per-Segment');
      K.spdMinus.visible = K.spdPlus.visible = K.spdLbl.visible = (perSeg && has && !st.selIsLast);
      K.interp.__lbl.text = 'Curve: ' + (st ? st.interp : 'Linear');
      K.sm.__lbl.text = st ? st.speedMode : 'Manual';
      K.tm.__lbl.text = freeze ? 'Freeze' : 'Live'; K.tm.__lbl.style.color = freeze ? S.freeze : S.accent;
    }
)TLJS"
R"TLJS(
    // Responsive width: in hosted (editor) mode the bottom bar fills the space left of the
    // inspector, which varies with resolution / uiscale -- so the timeline width can't be the
    // fixed build-time W. Recompute the inner content width each render and restyle every
    // width-dependent panel, then invalidate the diamond cache so it relayouts at the new W.
    // Wrapped in try/catch: render() has no outer guard and a throw here would abort
    // the whole render (which also drops the injected native-bar MOUSE / CAM EDITOR buttons).
    function applyLayout(contentW) {
      try {
        contentW = Math.floor(contentW);
        if (contentW < 360) contentW = 360;
        if (contentW === lastContentW) return;
        lastContentW = contentW;
        W = contentW - LABELW; TLW = contentW;
        tl.style.width = contentW + 'px';
        srow.style.width = contentW + 'px';
        diamWrap.style.width = contentW + 'px';
        scrub.style.width = contentW + 'px';
        lastTlSig = ''; // force diamond relayout at new W
      } catch (e) { $.Msg('[camtl] applyLayout error: ' + e + '\n'); }
    }

    var api = {};
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; setNativeHidden(false); applyHudView('full'); restoreHudViewport(); return; }
      try { st = JSON.parse(raw); } catch (e) { return; }

      patchNativeBar(); // keep the native demo bar de-cluttered + our button present
      // Camera Editor Mode hosts this panel and wants a clean workspace: hide the whole
      // gameplay HUD (radar/health/ammo/scoreboard/native demo bar). Restored on exit.
      var hosted = !!st.hosted;
      var previewHidden = !!st.previewHudHidden;
      // Editor's "Regular Timeline" bottom mode = hosted with neither the camera overlay (st.open)
      // nor the graph (st.graphExp) active. There the native CS2 demo bar IS the bottom editor.
      var nativeMode = hosted && !st.open && !st.graphExp;
      // While hosted, the editor's UI-visibility picker chooses how much gameplay HUD shows;
      // the lightweight Config panel (ConfigHud) shares the SAME picker state and wants it
      // applied while it is open too. When neither is up the game HUD is fully restored.
      var cfgOpen = !!st.configOpen;
      applyHudView((hosted || cfgOpen) ? (st.hudView || 'full') : 'full');
      // Move the native CS2 gameplay HUD into the same preview rect as the rendered game.
      // This includes radar, score, player strip, health/ammo, inventory and death notices,
      // while excluding our editor/timeline chrome and the docked native demo bar.
      applyGameHudViewport((hosted || cfgOpen) && !!st.hudScale && (st.hudView || 'full') !== 'hidden', st.previewRect);
      // Native demo bar visibility: SHOWN in Regular Timeline mode (and in normal play); hidden when
      // a camera/graph overlay is up, our standalone panel is open, or a clean preview frame is
      // needed. Done up here -- BEFORE the hosted-layout early-return below -- so it never flashes
      // wrong during an unsettled first layout frame.
      setNativeHidden((!!st.open) || (hosted && !!st.graphExp) || previewHidden);
      closeBtn.visible = !hosted; // editor mode exits via its own "✕ Exit" button
      graphBtn.visible = false;   // bottom-panel switching is now the editor's bottom tab bar only
      // When hosted, this panel IS the editor's bottom bar under the preview. It fills the
      // entire width left of the inspector instead of floating as a card inside that bar.
      // Standalone timeline mode keeps the compact native-style card.
      if (hosted) {
        // Measure from the ALREADY-laid-out HUD context panel when our fresh root still reports 0
        // (the first layout passes after a build / resolution switch) -- otherwise the hosted bar
        // collapses to the compact fallback width and squishes for ~half a second until root
        // settles. Only restyle once we actually have a sane full-screen width.
        var rsx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
        var rawW = root.actuallayoutwidth || 0; if (rawW < 16) rawW = ctx.actuallayoutwidth || 0;
        var rw = rawW / rsx;
        var barW = (rw > EDITOR_INSPECTOR_W) ? Math.floor(rw - EDITOR_INSPECTOR_W) : 0;
        if (barW <= 0) return; // no valid width yet: hold last-good layout, retry next frame
        panel.style.horizontalAlign = 'left';
        panel.style.marginLeft = '0px';
        panel.style.marginBottom = EDITOR_BOTTOM_LIFT + 'px'; // rests at the bottom; tab bar sits ABOVE it
        panel.style.width = barW + 'px';
        // fit-children (NOT a fixed height): CameraEditorJs reads this panel's actual height
        // (#CamTimelineBar) to position the tab bar above it + shrink the preview.
        panel.style.height = 'fit-children';
        panel.style.borderRadius = '0px';
        panel.style.border = '0px solid transparent';
        panel.style.boxShadow = 'none';
        applyLayout(barW - 28); // inner content fills the bar minus L/R padding
        // Regular Timeline mode: dock the native bar to fit left of the inspector (same barW).
        // Other hosted modes hide it; keep its geometry reset so it's correct when shown again.
        if (nativeMode) nativeDock(barW); else nativeUndock();
      } else {
        panel.style.horizontalAlign = 'center';
        panel.style.marginLeft = '0px';
        panel.style.marginBottom = '0px';
        panel.style.width = (LABELW + W_DEFAULT + 28) + 'px';
        panel.style.height = 'fit-children';
        panel.style.borderRadius = '6px';
        panel.style.border = '1px solid ' + S.panelBorder;
        panel.style.boxShadow = '#000000cc 0px 0px 12px 2px';
        applyLayout(LABELW + W_DEFAULT); // restore the standalone default width
        // Config panel open (not hosted): dock the native demo bar to fit left of the Config
        // inspector (same width as the editor's Regular Timeline mode); otherwise restore the
        // native demo bar to full width / centre (pixel-perfect on exit).
        if (cfgOpen) {
          var crsx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
          var crawW = root.actuallayoutwidth || 0; if (crawW < 16) crawW = ctx.actuallayoutwidth || 0;
          var crw = crawW / crsx;
          var cBarW = (crw > EDITOR_INSPECTOR_W) ? Math.floor(crw - EDITOR_INSPECTOR_W) : 0;
          if (cBarW > 0) nativeDock(cBarW);
        } else {
          nativeUndock();
        }
      }

      var graphExp = !!st.graphExp;
      graphExpActive = graphExp;
      root.visible = !!st.open && !previewHidden;
      // (native demo bar visibility already resolved above, before the hosted early-return)
      if (!st.open || previewHidden) { catcher.visible = false; return; }

      // UI-mouse mode. The editor forces it on while open; the catcher absorbs stray
      // clicks so spectator target switching cannot leak through behind the panel.
      var cur = !!st.cursor;
      var forcedCur = !!st.cursorForced;
      // When hosted by the editor, the editor draws its own click-catcher (at a lower
      // z-index than this panel) so its inspector stays clickable; suppress ours here so
      // this full-screen catcher (z55) can't sit on top of the editor's inspector.
      catcher.visible = cur && !hosted; catcher.hittest = cur && !hosted;
      panel.hittest = true;
      keyFooter.visible = !hosted;
      mouseLbl.text = forcedCur ? 'Mouse: UI  ·  editor cursor forced' : (cur ? 'Mouse: UI  ·  press G to toggle mouse' : 'Mouse: GAME  ·  press G to toggle mouse');
      mouseLbl.style.color = cur ? S.accent : S.label;
      try {
        var dc = ctx.GetDemoControllerState && ctx.GetDemoControllerState();
        if (dc && typeof dc.fTimeScale === 'number') {
          activeSpeed = dc.fTimeScale;
          updateSpeedButtons();
        }
      } catch (speedErr) {}

      var freeze = (st.timing === 'Freeze');
      tl.visible = true;
      hTitle.text = 'CAMERA TIMELINE';
      hInfo.text = 'tick ' + activeTick() + '   ·   ' + st.count + ' keys   ·   sel #'
        + (st.selected >= 0 ? (st.selected + 1) : '-') + '   ·   seg ' + (st.segment + 1)
        + '   ·   ' + st.interp + (st.scrubbing ? '   ·   SCRUBBING' : '');
      if (!clearConfirm) { clearBtn.__lbl.text = 'Clear'; clearBtn.__lbl.style.color = S.value; }

      var t0 = st.tickMin, t1 = st.tickMax; if (t1 <= t0) t1 = t0 + 1;
      scrubT0 = t0; scrubT1 = t1;
      var shownTick = activeTick();
      if (!scrub.mousedown) {
        scrubSyncing = true;
        scrub.value = clamp01((shownTick - t0) / (t1 - t0)); // normalized; don't fight a drag
        scrubSyncing = false;
      }
      if (st.count < 2) tReadout.text = 'Place 2+ camera markers (K or + Add), then drag to scrub';
      else if (!scrub.mousedown) tReadout.text = 'tick ' + shownTick + '   time ' + (st.time != null ? st.time.toFixed(2) : '?') + 's';
      syncTransportButton();
      var sig = st.tickMin + ':' + st.tickMax + ':' + st.selected + ':' + (st.markers ? st.markers.map(function (m) { return m.tick; }).join(',') : '');
      if (sig !== lastTlSig) { lastTlSig = sig; rebuildTimelineDiamonds(); }

      updateKeys(freeze);
    };

    $.CamTimeline = api;
    api.render();
    patchNativeBar(); // inject immediately at build, even before the first state push
    $.Msg('[camtl] timeline panel built.\n');
  } catch (err) {
    $.Msg('[camtl] gui error: ' + err + '\n');
  }
})();
)TLJS";

} // namespace Filmmaker
