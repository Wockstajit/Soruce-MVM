#pragma once

// Panorama JS for the camera TIMELINE + CURVE EDITOR. Built ONCE into the in-game
// HUD (CSGOHud) context by CameraTimelineHud.cpp. One panel, two in-place views.
//
// While the panel is OPEN it REPLACES the native CS2 demo bar: the native
// CSGOHudDemoController contents (slider + transport + the G-mouse / next-player /
// next-camera hotkey labels) are hidden and ours occupies that bottom space; the
// native bar is restored when we close.
//
// UI-mouse mode (state.cursor): while the camera timeline / curve editor is open it
// is forced on so the panel is always clickable. In regular native-demo-bar mode it
// can be toggled from third-person/freecam by G or the injected MOUSE button.
//
//   * TIMELINE: a native-styled scrubber (HorizontalSlider) + keyframe diamonds +
//     transport + speed buttons.
//   * CURVE EDITOR: IWXMVM's multi-lane value-vs-time graph (X/Y/Z/Pitch/Yaw/Tilt/
//     FOV), drawn with the rotated-panel line trick + diamond keyframes, a draggable
//     playhead, and a draggable VALUE slider that edits the selected key/channel.
//
//   C++ -> JS : attributes "state" (light, every frame) + "curve" (heavy, on change;
//               carries a "rev" the JS gates lane relayout on), then
//               $.CamTimeline.render().
//   JS  -> C++: buttons / sliders issue "mirv_filmmaker camtl ..." console commands.

namespace Filmmaker {

inline const char* kCameraTimelineJs = R"TLJS(
(function () {
  try {
    var existing = $('#CamTimelineRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', freeze: '#4aa3ffff',
      bg: 'rgba(12,14,18,0.96)', panelBorder: '#ffffff1f',
      track: '#ffffff1a', grid: '#ffffff0e', gridMid: '#ffffff1c',
      line: '#f0b323dd', lineSel: '#ffffffff', playhead: '#ff5a5aee',
      label: '#9aa4b0ff', value: '#eef2f6ff', btnBg: '#ffffff14', btnOn: '#f0b32333',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var W = 1250, LANE_H = 42, LANE_GAP = 8, LABELW = 132;
    var EDITOR_INSPECTOR_W = 372; // keep in sync with CameraEditorJs INSPECTOR_W (dock centering)
    var LANES_H = (LANE_H + LANE_GAP) * 7;
    var CH = ['x','y','z','pitch','yaw','roll','fov'];
    var CHLBL = ['X','Y','Z','PITCH','YAW','TILT','FOV'];
    var CHHELP = ['','','','up / down','left / right','',''];
    var EASE = ['none','in','out','inout'];
    var EASE_LBL = ['None','Ease In','Ease Out','Ease In/Out'];

    function cmd(c) {
      // Any action disarms a pending "Clear — sure?" confirm (render restores the label).
      clearConfirm = false;
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
      b.SetPanelEvent('onactivate', onClick);
      b.__lbl = l; return b;
    }
    // A thin rotated panel from (x0,y0) to (x1,y1): the Panorama "line" trick.
    function lineSeg(parent, x0, y0, x1, y1, color, thick) {
      var dx = x1 - x0, dy = y1 - y0; var len = Math.sqrt(dx * dx + dy * dy); if (len < 0.5) len = 0.5;
      var ang = Math.atan2(dy, dx) * 180 / Math.PI;
      var p = mk('Panel', parent); p.hittest = false;
      p.style.position = x0.toFixed(1) + 'px ' + (y0 - thick / 2).toFixed(1) + 'px 0px';
      p.style.width = len.toFixed(1) + 'px'; p.style.height = thick + 'px';
      p.style.transformOrigin = '0% 50%';
      p.style.transform = 'rotateZ(' + ang.toFixed(2) + 'deg)';
      p.style.backgroundColor = color; return p;
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
    // Hide/show the native demo bar (CSGOHudDemoController "Contents": SliderRow +
    // ControlRow). We cache the panel once found; cheap and idempotent each frame.
    var nativeContents = null;
    function setNativeHidden(hide) {
      if (!nativeContents) { var sr = findNative('SliderRow'); if (sr && sr.GetParent) nativeContents = sr.GetParent(); }
      if (nativeContents) nativeContents.visible = !hide;
    }

    // Patch the LIVE native demo bar (CSGOHudDemoController): remove the
    // next-camera / next-player / mouse-cursor hotkey hints, and inject our
    // "MOUSE" + "CAM EDITOR" buttons into it (idempotent; re-adds if the bar is recreated).
    // Runs every frame while in a demo so the clutter never reappears.
    function patchNativeBar() {
      var ids = ['HotKey_Next_Camera', 'HotKey_Player_Next', 'HotKey_Toggle_Mouse_Cursor'];
      for (var i = 0; i < ids.length; i++) { var p = findNative(ids[i]); if (p) p.visible = false; }
      var host = findNative('HotKeyLabels') || findNative('ControlRow');
      if (!host) return;
      function childIndex(p) {
        if (!p || !host.GetChildCount) return -1;
        var n = host.GetChildCount();
        for (var ci = 0; ci < n; ci++) if (host.GetChild(ci) === p) return ci;
        return -1;
      }
      var oldMouse = host.FindChildTraverse && host.FindChildTraverse('CamCursorBtn');
      var oldEditor = host.FindChildTraverse && host.FindChildTraverse('CamEditorBtn');
      if ((!oldMouse && oldEditor) || (oldMouse && oldEditor && childIndex(oldMouse) > childIndex(oldEditor))) {
        try { if (oldMouse) oldMouse.DeleteAsync(0); } catch (orderErr1) {}
        try { oldEditor.DeleteAsync(0); } catch (orderErr2) {}
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
      var mb = nativeBtn('CamCursorBtn', 'MOUSE (G)', function () { cmd('mirv_filmmaker camtl cursor toggle'); });
      var curOn = !!(st && st.cursor);
      mb.style.backgroundColor = curOn ? '#c92a2acc' : '#ffffff14';
      mb.__lbl.style.color = curOn ? '#ffffffff' : S.label;
      var cb = nativeBtn('CamEditorBtn', 'CAM EDITOR', function () { cmd('mirv_filmmaker editor toggle'); });
      cb.style.backgroundColor = '#f0b32333';
      cb.__lbl.style.color = S.accent;
    }

    // Legacy cleanup hook: if an older build hid native HUD siblings, restore them. The
    // camera timeline HUD toggle is disabled; this must not hide anything anymore.
    var OURS = { 'CamTimelineRoot': 1, 'MarkerHudRoot': 1, 'MovieHudRoot': 1, 'CamEditorRoot': 1 };
    var hiddenNatives = null; // [{p, v}] captured while hidden; null while the HUD is shown
    function setGameHudHidden(hide) {
      if (hide) {
        if (!hiddenNatives) hiddenNatives = [];
        var nk = ctx.GetChildCount ? ctx.GetChildCount() : 0;
        for (var i = 0; i < nk; i++) {
          var c = ctx.GetChild(i);
          if (!c || OURS[c.id]) continue;
          if (c.visible) { hiddenNatives.push({ p: c, v: true }); try { c.visible = false; } catch (e) {} }
        }
      } else if (hiddenNatives) {
        for (var j = 0; j < hiddenNatives.length; j++) { try { hiddenNatives[j].p.visible = hiddenNatives[j].v; } catch (e2) {} }
        hiddenNatives = null;
      }
    }

    // ---- root + hit catcher + panel ------------------------------------
    var root = $.CreatePanel('Panel', ctx, 'CamTimelineRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '55';
    var catcher = mk('Panel', root); catcher.hittest = true; catcher.visible = false;
    catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.SetPanelEvent('onactivate', function () { /* swallow stray clicks */ });

    var panel = mk('Panel', root); panel.hittest = true;
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
    var viewBtn = btn(header, 'Curve Editor', function () {
      cmd('mirv_filmmaker camtl view ' + (curView === 'timeline' ? 'curve' : 'timeline'));
    }, S.accent);
    var closeBtn = btn(header, '✕', function () { cmd('mirv_filmmaker camtl close'); }, S.value);
)TLJS"
R"TLJS(
    // ===================== TIMELINE VIEW ================================
    var tl = mk('Panel', panel); tl.style.flowChildren = 'down'; tl.style.width = (LABELW + W) + 'px';
    var trow = mk('Panel', tl); trow.style.flowChildren = 'right'; trow.style.width = '100%'; trow.style.marginBottom = '8px';
    var playBtn = btn(trow, '▶', function () {
      cmd('mirv_filmmaker camtl play');
    }, S.accent);
    playBtn.visible = false; // camera-path playback disabled pending rewrite
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

    // Timeline scrubber spans the FULL content width (no label gutter -- that column is
    // only for the curve editor's per-channel lane labels).
    var TLW = LABELW + W;
    var srow = mk('Panel', tl); srow.style.width = TLW + 'px'; srow.style.height = '40px';
    var diamWrap = mk('Panel', srow); diamWrap.hittest = false;
    diamWrap.style.width = TLW + 'px'; diamWrap.style.height = '22px'; diamWrap.style.position = '0px 0px 0px';
    // (No separate track panel: the native Slider draws its own groove, so a second
    // trackBg here produced a doubled line that ran past the keyframes.)
    // Native CS2 sliders need BOTH the class (styling) and the direction ATTRIBUTE
    // (drag axis): e.g. <Slider class="HorizontalSlider" direction="horizontal"/>.
    // Without direction the Slider defaults to vertical, so the thumb drags up/down
    // instead of left/right -- pass it in the CreatePanel construction props.
    var scrub = $.CreatePanel('Slider', srow, 'CamScrub', { direction: 'horizontal' }); scrub.AddClass('HorizontalSlider');
    scrub.style.width = TLW + 'px'; scrub.style.height = '24px'; scrub.style.position = '0px 10px 0px';

    // ===================== CURVE VIEW ===================================
    var cv = mk('Panel', panel); cv.style.flowChildren = 'down'; cv.style.width = (LABELW + W) + 'px'; cv.visible = false;
    var ctb = mk('Panel', cv); ctb.style.flowChildren = 'right'; ctb.style.width = '100%'; ctb.style.marginBottom = '6px';
    btn(ctb, 'Zoom +', function () { cmd('mirv_filmmaker camtl zoom in'); }, S.label);
    btn(ctb, 'Zoom −', function () { cmd('mirv_filmmaker camtl zoom out'); }, S.label);
    btn(ctb, 'Reset', function () { cmd('mirv_filmmaker camtl zoom reset'); }, S.label);
    btn(ctb, '◀ Pan', function () { cmd('mirv_filmmaker camtl pan -1'); }, S.label);
    btn(ctb, 'Pan ▶', function () { cmd('mirv_filmmaker camtl pan 1'); }, S.label);
    var cInfo = lbl(ctb, '', S.label, 12); cInfo.style.verticalAlign = 'center'; cInfo.style.marginLeft = '12px';

    var phrow = mk('Panel', cv); phrow.style.width = (LABELW + W) + 'px'; phrow.style.height = '16px';
    var phSlider = $.CreatePanel('Slider', phrow, 'CamPlayhead', { direction: 'horizontal' }); phSlider.AddClass('HorizontalSlider');
    phSlider.style.width = W + 'px'; phSlider.style.height = '14px'; phSlider.style.position = LABELW + 'px 0px 0px';

    // graphArea: NO flow so lanesInner + the playhead line OVERLAY (not stack).
    var graphArea = mk('Panel', cv); graphArea.style.width = (LABELW + W) + 'px'; graphArea.style.height = LANES_H + 'px';
    var lanesInner = mk('Panel', graphArea); lanesInner.style.flowChildren = 'down';
    lanesInner.style.width = '100%'; lanesInner.style.position = '0px 0px 0px';
    var laneGraphs = [], laneLabels = [], laneNames = [], laneRange = [];
    for (var c = 0; c < CH.length; c++) {
      var row = mk('Panel', lanesInner); row.style.flowChildren = 'right'; row.style.width = '100%'; row.style.height = (LANE_H + LANE_GAP) + 'px';
      var lc = mk('Panel', row); lc.style.width = LABELW + 'px'; lc.style.height = LANE_H + 'px';
      lc.style.flowChildren = 'down'; lc.style.verticalAlign = 'center'; lc.hittest = true;
      var nm = lbl(lc, CHLBL[c], S.accent, 12); nm.style.fontWeight = 'bold';
      if (CHHELP[c]) {
        var hp = lbl(lc, CHHELP[c], S.label, 9);
        hp.style.marginTop = '-1px';
      }
      laneRange.push(lbl(lc, '', S.label, 10));
      (function (ci, lp) { lp.SetPanelEvent('onactivate', function () { selectChannel(ci); }); })(c, lc);
      var g = mk('Panel', row); g.style.width = W + 'px'; g.style.height = LANE_H + 'px';
      g.style.backgroundColor = S.grid; g.style.borderRadius = '2px'; g.style.border = '1px solid transparent'; g.hittest = true;
      // Lane clicks select the property only. Keyframe diamonds own camera selection;
      // keeping these responsibilities separate prevents the parent lane click from
      // immediately overriding a clicked camera with the old playhead-nearest camera.
      (function (ci, gp) { gp.SetPanelEvent('onactivate', function () { selectChannel(ci); }); })(c, g);
      laneLabels.push(lc); laneNames.push(nm);
      laneGraphs.push(g);
    }
    var phLine = mk('Panel', graphArea); phLine.hittest = false;
    phLine.style.position = LABELW + 'px 0px 0px'; phLine.style.width = '2px';
    phLine.style.height = LANES_H + 'px'; phLine.style.backgroundColor = S.playhead;

    // value editor row: a draggable slider that edits the SELECTED key + channel.
    var cedit = mk('Panel', cv); cedit.style.flowChildren = 'right'; cedit.style.width = '100%'; cedit.style.marginTop = '8px'; cedit.visible = false;
    var ceLabel = lbl(cedit, '', S.value, 13); ceLabel.style.verticalAlign = 'center'; ceLabel.style.width = '210px';
    var cevSlider = $.CreatePanel('Slider', cedit, 'CamVal', { direction: 'horizontal' }); cevSlider.AddClass('HorizontalSlider');
    cevSlider.style.width = '860px'; cevSlider.style.height = '16px'; cevSlider.style.verticalAlign = 'center';
    var cevVal = lbl(cedit, '', S.accent, 13); cevVal.style.verticalAlign = 'center'; cevVal.style.marginLeft = '12px'; cevVal.style.fontWeight = 'bold';
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
    var st = null, curve = null, curView = 'timeline', selChannel = -1;
    var lastRev = -1, lastView = '', lastTlSig = '';
    var lastCurveSelected = -2, lastCurveChannel = -1;
    // Dynamically-created Panorama sliders work in their default 0..1 range (setting
    // min/max/out-of-range value is unreliable and leaves the thumb stuck mid-track), so
    // we keep value normalized and map 0..1 <-> tick / channel-value ourselves.
    var scrubT0 = 0, scrubT1 = 1, phT0 = 0, phT1 = 1, valLo = 0, valHi = 1;
    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function sliderTick(v, a, b) { return Math.round(a + v * (b - a)); }

    function frac(tick, t0, t1) { var d = t1 - t0; return d > 0 ? (tick - t0) / d : 0; }
    function activeTick() {
      return st && st.scrubbing ? st.scrubTick : (st ? st.tick : 0);
    }
    function selectChannel(ci) {
      selChannel = ci;
      if (curve) rebuildCurveLanes();
      if (api && api.render) api.render();
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
    // Select the marker nearest the visible playhead (used when a lane background is clicked).
    function selectNearestInLane() {
      if (!st || !st.markers || st.markers.length === 0) return;
      var best = -1, bd = 1e15, tick = activeTick();
      for (var i = 0; i < st.markers.length; i++) { var d = Math.abs(st.markers[i].tick - tick); if (d < bd) { bd = d; best = i; } }
      if (best >= 0) cmd('mirv_filmmaker marker select ' + best);
    }

    // CS2 sliders fire SliderValueChanged (while dragging) + SliderReleased (on let-go),
    // delivered via $.RegisterEventHandler with the value as the 2nd arg -- NOT the
    // 'onvaluechanged' panel event. While dragging we PREVIEW the camera (smooth, no
    // world seek); on release we seek the demo world to the final tick.
    $.RegisterEventHandler('SliderValueChanged', scrub, function (panel, v) {
      var t = sliderTick(v, scrubT0, scrubT1);
      tReadout.text = 'tick ' + t + '   (release to seek)';
      cmd('mirv_filmmaker camtl scrubpreview ' + t);
    });
    $.RegisterEventHandler('SliderReleased', scrub, function (panel, v) {
      cmd('mirv_filmmaker camtl scrub ' + sliderTick(v, scrubT0, scrubT1));
    });
    $.RegisterEventHandler('SliderValueChanged', phSlider, function (panel, v) {
      cmd('mirv_filmmaker camtl scrubpreview ' + sliderTick(v, phT0, phT1));
    });
    $.RegisterEventHandler('SliderReleased', phSlider, function (panel, v) {
      cmd('mirv_filmmaker camtl scrub ' + sliderTick(v, phT0, phT1));
    });
    // Value editor: update the selected camera/property continuously while dragging.
    // The backend records one undo snapshot for the whole gesture.
    var valueDragActive = false, valueSyncing = false;
    $.RegisterEventHandler('SliderValueChanged', cevSlider, function (panel, v) {
      if (valueSyncing) return;
      var value = valLo + v * (valHi - valLo);
      cevVal.text = value.toFixed(2);
      if (st && st.selected >= 0 && selChannel >= 0) {
        if (!valueDragActive) {
          valueDragActive = true;
          cmd('mirv_filmmaker camtl editbegin');
        }
        cmd('mirv_filmmaker camtl setvalpreview ' + st.selected + ' ' + selChannel + ' ' + value.toFixed(3));
      }
    });
    $.RegisterEventHandler('SliderReleased', cevSlider, function (panel, v) {
      if (valueDragActive) {
        valueDragActive = false;
        cmd('mirv_filmmaker camtl editend');
      }
    });

    function rebuildTimelineDiamonds() {
      diamWrap.RemoveAndDeleteChildren();
      if (!st || !st.markers) return;
      var t0 = st.tickMin, t1 = st.tickMax; if (t1 <= t0) t1 = t0 + 1;
      for (var i = 0; i < st.markers.length; i++) (function (i) {
        var x = frac(st.markers[i].tick, t0, t1) * TLW;
        var sel = (i === st.selected);
        diamond(diamWrap, x, 11, sel ? 19 : 15, sel ? S.lineSel : S.accent, function () {
          cmd('mirv_filmmaker camtl select ' + i);
        });
      })(i);
    }
    function rebuildCurveLanes() {
      if (!curve || !curve.lanes) return;
      var n = curve.n, t0 = curve.t0, t1 = curve.t1; if (t1 <= t0) t1 = t0 + 1;
      for (var c = 0; c < curve.lanes.length && c < laneGraphs.length; c++) {
        var lane = curve.lanes[c], g = laneGraphs[c];
        g.RemoveAndDeleteChildren();
        g.style.backgroundColor = S.grid;
        g.style.border = '1px solid #ffffff0d';
        laneLabels[c].style.backgroundColor = 'transparent';
        laneNames[c].style.color = S.accent;
        laneRange[c].text = lane.min.toFixed(1) + ' .. ' + lane.max.toFixed(1);
        for (var gi = 1; gi < 10; gi++)
          lineSeg(g, W * gi / 10, 0, W * gi / 10, LANE_H, gi === 5 ? S.gridMid : S.grid, 1);
        lineSeg(g, 0, LANE_H / 2, W, LANE_H / 2, S.gridMid, 1);
        var px = -1, py = -1;
        for (var i = 0; i < n; i++) {
          var v = lane.pts[i];
          if (v === null) { px = -1; continue; }
          var x = (i / (n - 1)) * W, y = (1 - v) * LANE_H;
          if (px >= 0) lineSeg(g, px, py, x, y, S.line, 2);
          px = x; py = y;
        }
        if (st && st.markers) {
          var span = lane.max - lane.min; if (Math.abs(span) < 1e-6) span = 1;
          for (var m = 0; m < st.markers.length; m++) (function (c, m) {
            var mk2 = st.markers[m], mt = mk2.tick;
            if (mt < t0 - 1 || mt > t1 + 1) return;
            var dx = frac(mt, t0, t1) * W, norm = (mk2[CH[c]] - lane.min) / span;
            if (norm < 0) norm = 0; if (norm > 1) norm = 1;
            var sel = (m === st.selected && c === selChannel);
            diamond(g, dx, (1 - norm) * LANE_H, sel ? 14 : 9, sel ? S.lineSel : S.accent,
              function () { selectChannel(c); cmd('mirv_filmmaker camtl select ' + m); });
          })(c, m);
        }
      }
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
    var api = {};
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; setNativeHidden(false); setGameHudHidden(false); return; }
      try { st = JSON.parse(raw); } catch (e) { return; }

      patchNativeBar(); // keep the native demo bar de-cluttered + our button present
      // Camera Editor Mode hosts this panel and wants a clean workspace: hide the whole
      // gameplay HUD (radar/health/ammo/scoreboard/native demo bar). Restored on exit.
      var hosted = !!st.hosted;
      setGameHudHidden(hosted);
      closeBtn.visible = !hosted; // editor mode exits via its own "✕ Exit" button
      // When hosted, the right inspector takes the screen's right edge, so dock the
      // timeline left-aligned and centered UNDER the preview area (screen width minus the
      // inspector) instead of the whole screen. Standalone: centered on the full screen.
      if (hosted) {
        var rsx = root.actualuiscale_x || 1;
        var rw = (root.actuallayoutwidth || 0) / rsx;
        var panelW = LABELW + W + 28;
        var ml = (rw > 10) ? Math.floor(((rw - EDITOR_INSPECTOR_W) - panelW) / 2) : 0;
        if (ml < 0) ml = 0;
        panel.style.horizontalAlign = 'left';
        panel.style.marginLeft = ml + 'px';
      } else {
        panel.style.horizontalAlign = 'center';
        panel.style.marginLeft = '0px';
      }

      curView = st.view || 'timeline';
      var previewHidden = !!st.previewHudHidden;
      root.visible = !!st.open && !previewHidden;
      // Hide the native demo bar when our panel is open (it replaces it) or when
      // camera-path preview needs a clean frame.
      setNativeHidden(!!st.open || previewHidden);
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
      tl.visible = (curView === 'timeline');
      cv.visible = (curView === 'curve');
      viewBtn.__lbl.text = (curView === 'timeline') ? 'Curve Editor' : 'Timeline';
      hTitle.text = (curView === 'timeline') ? 'CAMERA TIMELINE' : 'CAMERA CURVE EDITOR';
      hInfo.text = 'tick ' + activeTick() + '   ·   ' + st.count + ' keys   ·   sel #'
        + (st.selected >= 0 ? (st.selected + 1) : '-') + '   ·   seg ' + (st.segment + 1)
        + '   ·   ' + st.interp + (st.scrubbing ? '   ·   SCRUBBING' : '');
      if (!clearConfirm) { clearBtn.__lbl.text = 'Clear'; clearBtn.__lbl.style.color = S.value; }

      if (curView === 'timeline') {
        var t0 = st.tickMin, t1 = st.tickMax; if (t1 <= t0) t1 = t0 + 1;
        scrubT0 = t0; scrubT1 = t1;
        var shownTick = activeTick();
        if (!scrub.mousedown) scrub.value = clamp01((shownTick - t0) / (t1 - t0)); // normalized; don't fight a drag
        if (st.count < 2) tReadout.text = 'Place 2+ camera markers (K or + Add), then drag to scrub';
        else if (!scrub.mousedown) tReadout.text = 'tick ' + shownTick + '   time ' + (st.time != null ? st.time.toFixed(2) : '?') + 's';
        playBtn.__lbl.text = st.playing ? '⏸' : '▶';
        var sig = st.tickMin + ':' + st.tickMax + ':' + st.selected + ':' + (st.markers ? st.markers.map(function (m) { return m.tick; }).join(',') : '');
        if (sig !== lastTlSig) { lastTlSig = sig; rebuildTimelineDiamonds(); }
      }

      if (curView === 'curve') {
        var rawc = root.GetAttributeString('curve', '');
        if (rawc) { try { curve = JSON.parse(rawc); } catch (e2) { curve = null; } }
        if (curve) {
          var ct0 = curve.t0, ct1 = curve.t1; if (ct1 <= ct0) ct1 = ct0 + 1;
          cInfo.text = 'view ' + ct0 + ' .. ' + ct1 + ' ticks   ·   Ctrl+Z undo';
          if (curve.rev !== lastRev || lastView !== 'curve' || st.selected !== lastCurveSelected || selChannel !== lastCurveChannel) {
            lastRev = curve.rev;
            lastCurveSelected = st.selected;
            lastCurveChannel = selChannel;
            rebuildCurveLanes();
          }
          phT0 = ct0; phT1 = ct1;
          var curveTick = activeTick();
          if (!phSlider.mousedown) phSlider.value = clamp01((curveTick - ct0) / (ct1 - ct0));
          phLine.style.position = (LABELW + frac(curveTick, ct0, ct1) * W).toFixed(1) + 'px 0px 0px';

          // value editor for the selected key + channel
          if (selChannel >= 0 && st.selected >= 0 && curve.lanes && curve.lanes[selChannel] && st.markers && st.markers[st.selected]) {
            var lane = curve.lanes[selChannel];
            var lo = lane.min, hi = lane.max; if (hi <= lo) hi = lo + 1;
            var kv = st.markers[st.selected][CH[selChannel]];
            ceLabel.text = 'Edit Key #' + (st.selected + 1) + ' · ' + CHLBL[selChannel];
            if (!valueDragActive) {
              valLo = lo; valHi = hi;
              if (!cevSlider.mousedown) {
                valueSyncing = true;
                cevSlider.value = clamp01((kv - lo) / (hi - lo));
                valueSyncing = false;
              }
            }
            cevVal.text = kv.toFixed(2);
            cedit.visible = true;
          } else { cedit.visible = false; }
        }
      }

      updateKeys(freeze);
      lastView = curView;
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
