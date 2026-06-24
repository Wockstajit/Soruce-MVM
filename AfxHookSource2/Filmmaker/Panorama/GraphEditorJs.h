#pragma once

// Panorama JS for the EXPERIMENTAL After-Effects-style camera GRAPH EDITOR. Built ONCE into
// the in-game HUD (CSGOHud) context by GraphEditorExperimentHud.cpp. Default OFF; appears only
// when the experiment is toggled on, and is fully torn down (DeleteAsync) when toggled off, so
// the regular timeline/editor are never touched.
//
// Layout (AE-like): LEFT channel list (per-channel show/solo + a scrubby/typed value field),
// TOP time ruler/scrubber, RIGHT graph canvas (grid + per-channel curves + keyframe diamonds +
// Bezier handles + drag-select rectangle + playhead).
//
// Dragging is driven by the streamed mouse (state.mx/my/lmb/seq from the WndProc) since Panorama
// has no mouse-move event: a full-screen catcher consumes the clicks, and this script interprets
// the raw cursor against the canvas to implement keyframe drag, handle drag, drag-select, ruler
// scrub, and AE scrubby number fields. All edits issue "mirv_filmmaker grapheditor ..." commands;
// C++ owns the authoritative model and pushes it back as "state".

namespace Filmmaker {

inline const char* kGraphEditorJs = R"GEJS(
(function () {
  try {
    var existing = $('#GraphExpRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      bg: 'rgba(8,10,14,0.55)', panel: 'rgba(16,20,26,0.98)', border: '#ffffff1f',
      grid: '#ffffff10', gridMid: '#ffffff20', label: '#9aa4b0ff', value: '#eef2f6ff',
      dim: '#6b7480ff', accent: '#f0b323ff', sel: '#ffffffff', playhead: '#ff5a5aee',
      btnBg: '#ffffff14', btnOn: '#f0b32333', handle: '#ffffffcc',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var LEFTW = 236, HEADER_H = 64, RULER_H = 26, FOOTER_H = 26;
    var KF_R = 8, HANDLE_R = 7; // hit radii (layout px)
    // Docked layout: this editor lives in the bottom curve-editor zone -- left of the camera
    // editor's inspector and above the compact timeline scrub bar -- it is NOT full screen.
    var INSPECTOR_W = 372; // keep in sync with CameraEditorJs INSPECTOR_W
    var DOCK_H = 556;      // fills the whole bottom (timeline is hidden while we're open);
                           // keep in sync with CameraEditorJs GRAPH_DOCK_H

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
      b.SetPanelEvent('onactivate', onClick); b.__lbl = l; return b;
    }
    function labelForSpeed(sp) { return sp === 1 ? '1x' : (String(sp) + 'x'); }
    function lineSeg(parent, x0, y0, x1, y1, color, thick) {
      var dx = x1 - x0, dy = y1 - y0; var len = Math.sqrt(dx * dx + dy * dy); if (len < 0.5) len = 0.5;
      var ang = Math.atan2(dy, dx) * 180 / Math.PI;
      var p = mk('Panel', parent); p.hittest = false;
      p.style.position = x0.toFixed(1) + 'px ' + (y0 - thick / 2).toFixed(1) + 'px 0px';
      p.style.width = len.toFixed(1) + 'px'; p.style.height = thick + 'px';
      p.style.transformOrigin = '0% 50%'; p.style.transform = 'rotateZ(' + ang.toFixed(2) + 'deg)';
      p.style.backgroundColor = color; return p;
    }
    function dot(parent, cx, cy, size, color, rot) {
      var p = mk('Panel', parent); p.hittest = false;
      p.style.position = (cx - size / 2).toFixed(1) + 'px ' + (cy - size / 2).toFixed(1) + 'px 0px';
      p.style.width = size + 'px'; p.style.height = size + 'px';
      p.style.backgroundColor = color; p.style.borderRadius = (size / 2) + 'px';
      if (rot) { p.style.transformOrigin = '50% 50%'; p.style.transform = 'rotateZ(45deg)'; p.style.borderRadius = '1px'; }
      return p;
    }
    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : 0; }
)GEJS"
R"GEJS(
    // ---- bezier sampling (mirrors GraphExpModel) ------------------------
    function bez(a0, a1, a2, a3, s) { var u = 1 - s; return u * u * u * a0 + 3 * u * u * s * a1 + 3 * u * s * s * a2 + s * s * s * a3; }
    function segActive(a, b) { return (a.cr && a.cr.a) || (b.cl && b.cl.a); }
    function segPts(a, b) {
      var span = b.t - a.t; if (span <= 1e-9) span = 1e-9;
      var txR = clamp01(a.cr && a.cr.a ? a.cr.tx : 1 / 3), txL = clamp01(b.cl && b.cl.a ? b.cl.tx : 1 / 3);
      return {
        p0x: a.t, p0y: a.v, p1x: a.t + txR * span, p1y: a.v + (a.cr && a.cr.a ? a.cr.dv : 0),
        p2x: b.t - txL * span, p2y: b.v + (b.cl && b.cl.a ? b.cl.dv : 0), p3x: b.t, p3y: b.v
      };
    }
    function sampleSeg(a, b, t) {
      var P = segPts(a, b); var lo = 0, hi = 1, s = (t - a.t) / (b.t - a.t);
      for (var i = 0; i < 24; i++) { s = 0.5 * (lo + hi); var x = bez(P.p0x, P.p1x, P.p2x, P.p3x, s); if (x < t) lo = s; else hi = s; }
      return bez(P.p0y, P.p1y, P.p2y, P.p3y, s);
    }
    function sampleChan(chn, t) {
      var k = chn.keys; if (!k.length) return null;
      if (t <= k[0].t) return k[0].v; if (t >= k[k.length - 1].t) return k[k.length - 1].v;
      for (var i = 0; i + 1 < k.length; i++) if (t >= k[i].t && t <= k[i + 1].t) {
        if (segActive(k[i], k[i + 1])) return sampleSeg(k[i], k[i + 1], t);
        var sp = k[i + 1].t - k[i].t; var f = sp > 1e-9 ? (t - k[i].t) / sp : 0; return k[i].v + f * (k[i + 1].v - k[i].v);
      }
      return k[k.length - 1].v;
    }

    // ---- root + catcher -------------------------------------------------
    // Root stays FULL-SCREEN + transparent + non-hittest: a reliable screen-size reference (other
    // HUDs read their full-screen root the same way) and a pass-through for clicks outside our dock.
    // The editor itself lives in `dock`: a BOTTOM-DOCKED child filling the width left of the camera
    // editor's inspector, lifted above the compact timeline bar. Size/offset are recomputed each
    // frame in computeGeom(); all editor children lay out relative to `dock`.
    var root = $.CreatePanel('Panel', ctx, 'GraphExpRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '60';
    root.style.fontFamily = S.font;
    var dock = mk('Panel', root); dock.hittest = false;
    dock.style.horizontalAlign = 'left'; dock.style.verticalAlign = 'bottom';
    dock.style.backgroundColor = S.panel; dock.style.borderTop = '1px solid ' + S.accent;
    var catcher = mk('Panel', dock); catcher.hittest = true; catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.style.position = '0px 0px 0px';
    catcher.SetPanelEvent('onactivate', function () { /* swallow stray clicks; pipe owns logic */ });

    // ---- header ---------------------------------------------------------
    // Title + live info sit on the LEFT; a two-row control cluster anchors to the RIGHT. Top row
    // = switch-back-to-timeline + demo timescale; bottom row = the graph's own interp / history /
    // delete. (Requested: the Timeline + speed buttons sit ABOVE the Smooth/Linear stuff, right.)
    var header = mk('Panel', dock); header.style.width = '100%'; header.style.height = HEADER_H + 'px';
    header.style.flowChildren = 'right'; header.style.backgroundColor = S.panel;
    header.style.borderBottom = '1px solid ' + S.border; header.style.paddingLeft = '14px'; header.style.paddingRight = '14px';
    var hTitle = lbl(header, 'GRAPH EDITOR', S.value, 15); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.verticalAlign = 'center'; hTitle.style.marginRight = '18px';
    var hInfo = lbl(header, '', S.label, 12); hInfo.style.verticalAlign = 'center'; hInfo.style.width = 'fill-parent-flow(1.0)';

    // Right-anchored, two stacked rows.
    var rightCol = mk('Panel', header); rightCol.style.flowChildren = 'down'; rightCol.style.verticalAlign = 'center';
    var rowTop = mk('Panel', rightCol); rowTop.style.flowChildren = 'right'; rowTop.style.horizontalAlign = 'right'; rowTop.style.marginBottom = '5px';
    var rowBot = mk('Panel', rightCol); rowBot.style.flowChildren = 'right'; rowBot.style.horizontalAlign = 'right';

    // TOP row: back to the camera TIMELINE, then the demo timescale buttons. (The graph editor
    // always drives the camera now, so there is no Drive toggle; the camera is seeded
    // automatically, so no Reseed.)
    btn(rowTop, '≡ Timeline', function () { rawCmd('mirv_filmmaker editor curveeditor timeline'); }, S.value).style.verticalAlign = 'center';
    var speedWrap = mk('Panel', rowTop); speedWrap.style.flowChildren = 'right'; speedWrap.style.verticalAlign = 'center';
    speedWrap.style.marginLeft = '4px'; speedWrap.style.marginRight = '0px';
    var SPD = [0.1, 0.25, 0.5, 1, 2, 4], speedBtns = [], activeSpeed = 1;
    function updateSpeedButtons() {
      for (var si = 0; si < speedBtns.length; si++) {
        var on = Math.abs(speedBtns[si].speed - activeSpeed) < 0.0001;
        speedBtns[si].panel.style.backgroundColor = on ? S.btnOn : S.btnBg;
        speedBtns[si].panel.__lbl.style.color = on ? S.accent : S.label;
      }
    }
    for (var spI = 0; spI < SPD.length; spI++) (function (v) {
      var sb = btn(speedWrap, labelForSpeed(v), function () {
        activeSpeed = v; updateSpeedButtons(); rawCmd('demo_timescale ' + v);
      }, S.label);
      sb.style.paddingLeft = '7px'; sb.style.paddingRight = '7px';
      speedBtns.push({ panel: sb, speed: v });
    })(SPD[spI]);
    if (speedBtns.length) speedBtns[speedBtns.length - 1].panel.style.marginRight = '0px';
    updateSpeedButtons();

    // BOTTOM row: Smooth / Linear set the graph's OWN interpolation (its isolated handles),
    // applied to the selection if any keys are selected, otherwise the whole graph.
    btn(rowBot, '∿ Smooth', function () { cmd('smooth' + (st && st.selCount > 0 ? ' sel' : '')); }, S.value).style.verticalAlign = 'center';
    btn(rowBot, '╱ Linear', function () { cmd('linear' + (st && st.selCount > 0 ? ' sel' : '')); }, S.value).style.verticalAlign = 'center';
    var undoBtn = btn(rowBot, '↺ Undo', function () { cmd('undo'); }, S.value); undoBtn.style.verticalAlign = 'center';
    var redoBtn = btn(rowBot, '↻ Redo', function () { cmd('redo'); }, S.value); redoBtn.style.verticalAlign = 'center';
    var delBtn = btn(rowBot, '✕ Delete Key', function () { if (st && st.selCount > 0) cmd('delsel'); }, S.playhead);
    delBtn.style.verticalAlign = 'center'; delBtn.style.marginRight = '0px';
    // No "Exit Experiment" button: the graph editor is the default curve editor now, opened and
    // closed with the Camera Editor workspace (use the inspector's Exit). Standalone toggle is
    // still available via the `mirv_filmmaker grapheditor off` console command.

    // ---- left channel column -------------------------------------------
    // Each row sits at a FIXED Y (so the value cell's rect stays known for the mouse pipe) but its
    // contents FLOW horizontally + center vertically -- so the swatch / name / value / eye always
    // share one line. (Positioning each element by absolute Y inside the tall column drifted the
    // labels out of their rows.)
    var ROWS_Y0 = RULER_H + 24, ROW_H = 31, VALW = 64, EYEW = 24;
    var EYE_X = LEFTW - 8 - EYEW, VAL_X = EYE_X - 6 - VALW;
    var left = mk('Panel', dock); left.style.position = '0px ' + HEADER_H + 'px 0px';
    left.style.width = LEFTW + 'px'; left.style.backgroundColor = S.panel;
    left.style.borderRight = '1px solid ' + S.border;
    var lhP = mk('Panel', left); lhP.hittest = false; lhP.style.position = '8px ' + (RULER_H + 4) + 'px 0px';
    lhP.style.width = (LEFTW - 16) + 'px'; lhP.style.height = '16px';
    var lh = lbl(lhP, 'CHANNELS', S.dim, 11); lh.style.fontWeight = 'bold'; lh.style.letterSpacing = '2px'; lh.style.verticalAlign = 'center';

    var rows = []; // per-channel row controls + value-cell rect (dock-relative layout coords)
    function makeRow(c) {
      var rowY = ROWS_Y0 + c * ROW_H;
      var rowP = mk('Panel', left); rowP.hittest = false;
      rowP.style.position = '0px ' + rowY + 'px 0px'; rowP.style.width = LEFTW + 'px'; rowP.style.height = (ROW_H - 4) + 'px';
      rowP.style.flowChildren = 'right'; rowP.style.paddingLeft = '8px'; rowP.style.paddingRight = '8px';
      // swatch + name = solo TOGGLE target; fills the row left of the value cell (which the mouse
      // pipe owns for scrub/type). Right-anchored after it: the value cell, then the eye toggle.
      var hot = mk('Panel', rowP); hot.hittest = true; hot.style.flowChildren = 'right';
      hot.style.width = 'fill-parent-flow(1.0)'; hot.style.height = '100%'; hot.style.borderRadius = '3px';
      hot.SetPanelEvent('onactivate', function () { var m = chById(c); cmd('chan ' + c + ' ' + (m && m.solo ? 'unsolo' : 'solo')); });
      var sw = mk('Panel', hot); sw.hittest = false; sw.style.width = '10px'; sw.style.height = '10px';
      sw.style.verticalAlign = 'center'; sw.style.marginLeft = '2px'; sw.style.marginRight = '8px'; sw.style.borderRadius = '2px';
      var nm = lbl(hot, '', S.value, 12); nm.hittest = false; nm.style.verticalAlign = 'center';
      var val = lbl(rowP, '-', S.accent, 12); val.hittest = false; val.style.width = VALW + 'px';
      val.style.textAlign = 'right'; val.style.verticalAlign = 'center'; val.style.fontWeight = 'bold'; val.style.marginRight = '6px';
      var eye = mk('Panel', rowP); eye.hittest = true; eye.style.width = EYEW + 'px'; eye.style.height = '20px';
      eye.style.verticalAlign = 'center'; eye.style.borderRadius = '3px';
      var eyeL = lbl(eye, '◉', S.label, 12); eyeL.style.horizontalAlign = 'center'; eyeL.style.verticalAlign = 'center';
      eye.SetPanelEvent('onactivate', function () { var m = chById(c); cmd('chan ' + c + ' ' + (m && m.vis ? 'hide' : 'show')); });
      rows.push({ rowP: rowP, hot: hot, sw: sw, nm: nm, val: val, eye: eye, eyeL: eyeL,
                  valRect: { x0: VAL_X, y0: HEADER_H + rowY, x1: VAL_X + VALW, y1: HEADER_H + rowY + (ROW_H - 4) } });
    }
    for (var c0 = 0; c0 < 7; c0++) makeRow(c0);
    var hintP = mk('Panel', left); hintP.hittest = false; hintP.style.position = '8px ' + (ROWS_Y0 + 7 * ROW_H + 8) + 'px 0px';
    hintP.style.width = (LEFTW - 16) + 'px'; hintP.style.height = '40px';
    var leftHint = lbl(hintP, '', S.dim, 10); leftHint.style.width = '100%';
    leftHint.text = 'click name = solo · ◉ = show/hide\ndrag value = scrub · click value = type';
)GEJS"
R"GEJS(
    // ---- ruler + canvas layers -----------------------------------------
    var ruler = mk('Panel', dock); ruler.hittest = false; ruler.style.backgroundColor = 'rgba(0,0,0,0.25)';
    ruler.style.borderBottom = '1px solid ' + S.border;
    var rulerLayer = mk('Panel', ruler); rulerLayer.hittest = false; rulerLayer.style.width = '100%'; rulerLayer.style.height = '100%';

    var canvas = mk('Panel', dock); canvas.hittest = false; canvas.style.backgroundColor = 'rgba(0,0,0,0.18)';
    var gridLayer = mk('Panel', canvas); gridLayer.hittest = false;
    var curveLayer = mk('Panel', canvas); curveLayer.hittest = false;
    var kfLayer = mk('Panel', canvas); kfLayer.hittest = false;
    var handleLayer = mk('Panel', canvas); handleLayer.hittest = false;
    var phLine = mk('Panel', canvas); phLine.hittest = false; phLine.style.width = '2px'; phLine.style.backgroundColor = S.playhead;
    var rectVis = mk('Panel', canvas); rectVis.hittest = false; rectVis.visible = false;
    rectVis.style.backgroundColor = '#f0b32322'; rectVis.style.border = '1px solid ' + S.accent;
    [gridLayer, curveLayer, kfLayer, handleLayer].forEach(function (L) { L.style.position = '0px 0px 0px'; L.style.width = '100%'; L.style.height = '100%'; });

    // Playhead grab handle: a clear pin sitting in the ruler directly above the red line, so users
    // know where to click + drag to scrub. The red line itself is ALSO grabbable in the canvas.
    var phHandle = mk('Panel', dock); phHandle.hittest = false; phHandle.style.zIndex = '6';
    phHandle.style.width = '15px'; phHandle.style.height = '11px';
    phHandle.style.backgroundColor = S.playhead; phHandle.style.borderRadius = '3px';
    var phPin = mk('Panel', phHandle); phPin.hittest = false; phPin.style.width = '9px'; phPin.style.height = '9px';
    phPin.style.backgroundColor = S.playhead; phPin.style.transform = 'rotateZ(45deg)'; phPin.style.position = '3px 7px 0px';

    // Shared TextEntry for click-to-type number editing (created lazily, reused).
    var typeBox = null, typeCh = -1;

    // ---- state + interaction vars (closure) -----------------------------
    var st = null, model = [], G = null, uiscale = 1;
    var lastSig = '', activeCh = 0;
    var prevLmb = false, prevRmb = false, gesture = null;
    var dockScreenX = 0, dockScreenY = 0; // dock top-left in screen layout px (for mouse mapping)
    var didInitTimeline = false;          // one-shot: force the host timeline to its compact view
    var easeMenu = null, easeItemPanels = [], easeRects = []; // right-click ease menu (dock-relative rects)

    function cmd(s) { try { GameInterfaceAPI.ConsoleCommand('mirv_filmmaker grapheditor ' + s); } catch (e) {} }
    function rawCmd(s) { try { GameInterfaceAPI.ConsoleCommand(s); } catch (e) {} }

    // ---- coordinate maps (canvas-local layout px) -----------------------
    function tickToX(t) { return (t - st.viewT0) / (st.viewT1 - st.viewT0) * G.cw; }
    function xToTick(x) { return st.viewT0 + (x / G.cw) * (st.viewT1 - st.viewT0); }
    function valToY(v, ch) { var d = ch.max - ch.min; if (Math.abs(d) < 1e-9) d = 1; return (1 - (v - ch.min) / d) * G.ch; }
    function yToVal(y, ch) { var d = ch.max - ch.min; if (Math.abs(d) < 1e-9) d = 1; return ch.min + (1 - y / G.ch) * d; }
    function tickDelta(dxLayout) { return dxLayout / G.cw * (st.viewT1 - st.viewT0); }
    function valDelta(dyLayout, ch) { var d = ch.max - ch.min; if (Math.abs(d) < 1e-9) d = 1; return -(dyLayout / G.ch) * d; }

    function computeGeom() {
      // Measure the screen from the ALREADY-laid-out HUD context panel first. Our own root is
      // created fresh on each (re)build and reports actuallayoutwidth==0 until the next layout pass
      // -- and a resolution switch re-lays-out the whole tree -- so for ~half a second root was 0
      // and the old hardcoded 1280/720 fallback squished the editor into the top-left until it
      // settled. ctx already has a valid full-screen size on frame one, killing that transient.
      uiscale = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var rawW = root.actuallayoutwidth || 0, rawH = root.actuallayoutheight || 0;
      if (rawW < 16) { rawW = ctx.actuallayoutwidth || 0; rawH = ctx.actuallayoutheight || 0; }
      if (rawW < 16) return G != null; // no valid size yet: keep last-good geom, retry next frame
      var scrW = rawW / uiscale;
      var scrH = (rawH > 16 ? rawH : rawW * 0.5625) / uiscale;
      // The graph editor REPLACES the camera timeline (the host hides it while we're open), so we
      // sit flush at the bottom and fill the whole bottom panel -- no timeline bar to sit above.
      var dockW = Math.max(360, scrW - INSPECTOR_W);
      dockScreenX = 0; dockScreenY = Math.max(0, scrH - DOCK_H);
      // Size + place the dock within the full-screen root (bottom-left, flush to the screen bottom).
      dock.style.width = dockW + 'px'; dock.style.height = DOCK_H + 'px';
      dock.style.marginBottom = '0px';
      G = { rw: dockW, rh: DOCK_H, headerH: HEADER_H, rulerH: RULER_H,
            cx: LEFTW, cy: HEADER_H + RULER_H,
            cw: Math.max(120, dockW - LEFTW - 16), ch: Math.max(80, DOCK_H - HEADER_H - RULER_H - FOOTER_H) };
      left.style.height = (DOCK_H - HEADER_H) + 'px';
      ruler.style.position = LEFTW + 'px ' + HEADER_H + 'px 0px';
      ruler.style.width = (dockW - LEFTW) + 'px'; ruler.style.height = RULER_H + 'px';
      canvas.style.position = G.cx + 'px ' + G.cy + 'px 0px';
      canvas.style.width = G.cw + 'px'; canvas.style.height = G.ch + 'px';
      phLine.style.height = G.ch + 'px';
      return true;
    }

    function curCursor() {
      // Streamed cursor is screen-space; make it dock-relative so it lines up with the dock-
      // relative panel layout (the dock is no longer pinned to the screen origin).
      var lx = (st.mx || 0) / uiscale - dockScreenX, ly = (st.my || 0) / uiscale - dockScreenY;
      return { lx: lx, ly: ly, clx: lx - G.cx, cly: ly - G.cy };
    }
    function chById(c) { for (var i = 0; i < model.length; i++) if (model[i].ch === c) return model[i]; return null; }
    function keyById(ch, id) { for (var i = 0; i < ch.keys.length; i++) if (ch.keys[i].id === id) return ch.keys[i]; return null; }
    function keyIndex(ch, id) { for (var i = 0; i < ch.keys.length; i++) if (ch.keys[i].id === id) return i; return -1; }
)GEJS"
R"GEJS(
    // ---- hit-testing ----------------------------------------------------
    function hitKeyframe(clx, cly) {
      var best = null, bd = KF_R * KF_R;
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; var x = tickToX(k.t), y = valToY(k.v, ch);
          var dx = x - clx, dy = y - cly, d = dx * dx + dy * dy;
          if (d <= bd) { bd = d; best = { ch: ch.ch, id: k.id }; }
        }
      }
      return best;
    }
    function handleEndpoint(ch, k, side) {
      var idx = keyIndex(ch, k.id);
      var defSpan = (st.viewT1 - st.viewT0) * 0.06;
      if (side > 0) {
        var nx = (idx + 1 < ch.keys.length) ? ch.keys[idx + 1] : null;
        var span = nx ? (nx.t - k.t) : defSpan; var tx = (k.cr && k.cr.a) ? k.cr.tx : 1 / 3;
        var dv = (k.cr && k.cr.a) ? k.cr.dv : 0;
        return { t: k.t + clamp01(tx) * span, v: k.v + dv, span: span };
      } else {
        var pv = (idx - 1 >= 0) ? ch.keys[idx - 1] : null;
        var span2 = pv ? (k.t - pv.t) : defSpan; var tx2 = (k.cl && k.cl.a) ? k.cl.tx : 1 / 3;
        var dv2 = (k.cl && k.cl.a) ? k.cl.dv : 0;
        return { t: k.t - clamp01(tx2) * span2, v: k.v + dv2, span: span2 };
      }
    }
    function hitHandle(clx, cly) {
      var best = null, bd = HANDLE_R * HANDLE_R;
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; if (!k.sel) continue;
          [-1, 1].forEach(function (side) {
            var e = handleEndpoint(ch, k, side); var x = tickToX(e.t), y = valToY(e.v, ch);
            var dx = x - clx, dy = y - cly, d = dx * dx + dy * dy;
            if (d <= bd) { bd = d; best = { ch: ch.ch, id: k.id, side: side, span: e.span }; }
          });
        }
      }
      return best;
    }
    function keysInRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx);
      var y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy);
      var out = [];
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; var x = tickToX(k.t), y = valToY(k.v, ch);
          // space-separated "ch id" so the flat token list survives the console tokenizer
          // (a single "ch:id,ch:id" token gets split on the comma and loses all but the first).
          if (x >= x0 && x <= x1 && y >= y0 && y <= y1) out.push(ch.ch + ' ' + k.id);
        }
      }
      return out;
    }

    // ---- number field typing -------------------------------------------
    // Hit-test the left-column value cells by their known absolute rects (layout px). No reliance
    // on onmouseover (which the in-game HUD context does not reliably deliver to created panels).
    function numFieldHit(lx, ly) {
      for (var c = 0; c < rows.length; c++) {
        var r = rows[c].valRect;
        if (lx >= r.x0 && lx <= r.x1 && ly >= r.y0 && ly <= r.y1) return c;
      }
      return -1;
    }
    function fieldValue(c) { var ch = chById(c); if (!ch) return 0; var v = sampleChan(ch, st.playhead); return v == null ? 0 : v; }
    function fieldSens(c) { var ch = chById(c); if (!ch) return 0.1; return (ch.max - ch.min) / 300; }
    function commitType() {
      if (!typeBox) return;
      var c = typeCh; var txt = typeBox.text;
      typeBox.visible = false; typeCh = -1;
      var v = parseFloat(txt);
      if (isFinite(v)) { cmd('editbegin'); cmd('addkey ' + c + ' ' + st.playhead.toFixed(2) + ' ' + v.toFixed(3)); }
    }
    function openType(c) {
      if (!typeBox) {
        typeBox = $.CreatePanel('TextEntry', dock, 'GraphExpType', {});
        typeBox.style.height = '22px'; typeBox.style.fontSize = '12px'; typeBox.style.zIndex = '5';
        typeBox.SetPanelEvent('oninputsubmit', function () { commitType(); });
      }
      typeCh = c;
      var rr = rows[c].valRect; // exact value-cell rect (absolute layout px)
      typeBox.style.position = rr.x0 + 'px ' + (rr.y0 - 1) + 'px 0px';
      typeBox.style.width = (rr.x1 - rr.x0) + 'px'; typeBox.visible = true;
      typeBox.text = fieldValue(c).toFixed(2);
      typeBox.SetFocus(); try { typeBox.RaiseSelectAll && typeBox.RaiseSelectAll(); } catch (e) {}
    }

    // ---- right-click ease menu (applies to the WHOLE selection) ---------
    function buildEaseMenu() {
      easeMenu = mk('Panel', dock); easeMenu.hittest = false; easeMenu.visible = false; easeMenu.style.zIndex = '20';
      easeMenu.style.width = '152px'; easeMenu.style.height = '104px';
      easeMenu.style.backgroundColor = 'rgba(18,22,28,0.99)'; easeMenu.style.border = '1px solid ' + S.accent; easeMenu.style.borderRadius = '4px';
      var t = lbl(easeMenu, 'EASE SELECTION', S.dim, 10); t.style.position = '12px 6px 0px'; t.style.fontWeight = 'bold'; t.style.letterSpacing = '1px';
      var labels = ['Ease In', 'Ease Out', 'Ease In / Out'];
      easeItemPanels = [];
      for (var i = 0; i < 3; i++) {
        var it = mk('Panel', easeMenu); it.hittest = false; it.style.position = '4px ' + (22 + i * 26) + 'px 0px';
        it.style.width = '144px'; it.style.height = '24px'; it.style.borderRadius = '3px';
        var l = lbl(it, labels[i], S.value, 13); l.style.position = '10px 4px 0px';
        easeItemPanels.push(it);
      }
    }
    function showEaseMenu(lx, ly) {
      if (!easeMenu) buildEaseMenu();
      var W = 152, H = 104;
      if (lx + W > G.rw) lx = G.rw - W - 2; if (lx < 0) lx = 0;
      if (ly + H > G.rh) ly = G.rh - H - 2; if (ly < 0) ly = 0;
      easeMenu.style.position = lx + 'px ' + ly + 'px 0px'; easeMenu.visible = true;
      var modes = ['in', 'out', 'inout']; easeRects = [];
      for (var i = 0; i < 3; i++) easeRects.push({ x0: lx + 4, x1: lx + 148, y0: ly + 22 + i * 26, y1: ly + 22 + i * 26 + 24, mode: modes[i] });
    }
    function hideEaseMenu() { if (easeMenu) { easeMenu.visible = false; easeRects = []; } }
    function easeMenuOpen() { return easeMenu && easeMenu.visible; }
    function easeHover(lx, ly) {
      for (var i = 0; i < easeRects.length; i++) {
        var inIt = (lx >= easeRects[i].x0 && lx <= easeRects[i].x1 && ly >= easeRects[i].y0 && ly <= easeRects[i].y1);
        if (easeItemPanels[i]) easeItemPanels[i].style.backgroundColor = inIt ? S.btnOn : 'rgba(0,0,0,0)';
      }
    }
    function easeHit(lx, ly) {
      for (var i = 0; i < easeRects.length; i++)
        if (lx >= easeRects[i].x0 && lx <= easeRects[i].x1 && ly >= easeRects[i].y0 && ly <= easeRects[i].y1) return easeRects[i].mode;
      return null;
    }

    // ---- the mouse-driven interaction engine ----------------------------
    function processInput() {
      var lmb = !!st.lmb, rmb = !!st.rmb; var cc = curCursor();
      var inCanvas = (cc.clx >= 0 && cc.clx <= G.cw && cc.cly >= 0 && cc.cly <= G.ch);
      var inRuler = (cc.lx >= G.cx && cc.lx <= G.cx + G.cw && cc.ly >= G.headerH && cc.ly <= G.headerH + G.rulerH);
      var nearPlayhead = inCanvas && Math.abs(cc.clx - tickToX(st.playhead)) <= 6;

      // Right-click opens the ease menu on a keyframe; it applies to the whole selection (the
      // clicked key is added to the selection if it wasn't already part of it).
      if (rmb && !prevRmb) {
        var rk = inCanvas ? hitKeyframe(cc.clx, cc.cly) : null;
        if (rk) { if (!isSelectedId(rk.ch, rk.id)) cmd('select ' + rk.ch + ' ' + rk.id); showEaseMenu(cc.lx, cc.ly); }
        else hideEaseMenu();
      }
      if (easeMenuOpen()) easeHover(cc.lx, cc.ly);
      prevRmb = rmb;

      if (lmb && !prevLmb) {
        // While the ease menu is open a click picks an item (or closes it), and is consumed.
        if (easeMenuOpen()) {
          var em = easeHit(cc.lx, cc.ly); if (em) cmd('ease ' + em);
          hideEaseMenu(); prevLmb = lmb; return;
        }
        // A press anywhere commits an open type field first (click-away).
        if (typeBox && typeBox.visible && typeCh >= 0) commitType();
        if (inCanvas) {
          var h = hitHandle(cc.clx, cc.cly);
          if (h) { cmd('editbegin'); gesture = { type: 'handle', ch: h.ch, id: h.id, side: h.side }; }
          else {
            var k = hitKeyframe(cc.clx, cc.cly);
            if (k) {
              if (!isSelectedId(k.ch, k.id)) cmd('select ' + k.ch + ' ' + k.id);
              cmd('editbegin'); gesture = { type: 'kf', sx: cc.lx, sy: cc.ly, ldt: 0, ldv: 0, ach: k.ch };
            } else if (nearPlayhead) {
              // grab the red playhead LINE in the canvas to scrub (same as the ruler handle).
              gesture = { type: 'ruler' }; cmd('playhead ' + xToTick(cc.clx).toFixed(2));
            } else { gesture = { type: 'rect', sx: cc.clx, sy: cc.cly, cx: cc.clx, cy: cc.cly }; }
          }
        } else if (inRuler) { gesture = { type: 'ruler' }; cmd('playhead ' + xToTick(cc.clx).toFixed(2)); }
        else { var nf = numFieldHit(cc.lx, cc.ly); if (nf >= 0) { gesture = { type: 'num', ch: nf, smx: st.mx, sv: fieldValue(nf), moved: false }; cmd('editbegin'); } }
      } else if (lmb && prevLmb && gesture) {
        if (gesture.type === 'handle') {
          var chh = chById(gesture.ch); var kk = chh ? keyById(chh, gesture.id) : null; if (kk) {
            var e = handleEndpoint(chh, kk, gesture.side);
            var th = xToTick(cc.clx), vh = yToVal(cc.cly, chh);
            var span = e.span > 1e-9 ? e.span : 1;
            var tx = clamp01(gesture.side > 0 ? (th - kk.t) / span : (kk.t - th) / span);
            var dv = vh - kk.v;
            // Shift = STRAIGHTEN: lock the handle flat (level with the key, dv=0) so the curve runs
            // straight through the keyframe; you still set the arm LENGTH (left/right), just not slope.
            if (st.shift) dv = 0;
            // Keep the handle OUTSIDE the diamond: enforce a minimum pixel distance from the key so
            // the arm/dot can't get swallowed into the diamond and become ungrabbable.
            var spanPx = span / (st.viewT1 - st.viewT0) * G.cw, dd = (chh.max - chh.min) || 1;
            var hpx = tx * spanPx, vpx = dv / dd * G.ch, dist = Math.sqrt(hpx * hpx + vpx * vpx), MIN = KF_R + 8;
            if (dist < MIN) {
              if (dist < 1e-3) { tx = clamp01(MIN / (spanPx || 1)); dv = 0; }
              else { var sc = MIN / dist; tx = clamp01(tx * sc); dv = dv * sc; }
            }
            // No vertical clamp: drag the handle as high/low as you want -- the auto-fit zooms the
            // view out to follow it (and back in when it returns), so you are never walled in.
            // reflect=0: left/right tangents are INDEPENDENT (dragging one never moves the other).
            cmd('handle ' + gesture.ch + ' ' + gesture.id + ' ' + (gesture.side > 0 ? 'right' : 'left') + ' ' + tx.toFixed(3) + ' ' + dv.toFixed(3) + ' 0');
          }
        } else if (gesture.type === 'kf') {
          var ach = chById(gesture.ach) || model[0];
          var dt = tickDelta(cc.lx - gesture.sx), dv2 = valDelta(cc.ly - gesture.sy, ach);
          // Shift = lock to the horizontal axis (timing only): freeze the value delta where it was
          // when Shift went down; rebaseline on release so it resumes without a jump.
          if (st.shift) {
            if (!gesture.shiftLock) { gesture.shiftLock = true; gesture.lockDv = gesture.ldv; }
            dv2 = gesture.lockDv;
          } else if (gesture.shiftLock) {
            gesture.shiftLock = false;
            var dd3 = (ach.max - ach.min) || 1; gesture.sy = cc.ly + gesture.lockDv * G.ch / dd3; dv2 = gesture.lockDv;
          }
          cmd('movesel ' + (dt - gesture.ldt).toFixed(3) + ' ' + (dv2 - gesture.ldv).toFixed(3));
          gesture.ldt = dt; gesture.ldv = dv2;
        } else if (gesture.type === 'rect') { gesture.cx = cc.clx; gesture.cy = cc.cly; updateRect(gesture); }
        else if (gesture.type === 'ruler') { cmd('playhead ' + xToTick(cc.clx).toFixed(2)); }
        else if (gesture.type === 'num') {
          var dpx = st.mx - gesture.smx; if (Math.abs(dpx) > 5) gesture.moved = true;
          if (gesture.moved) { var nv = gesture.sv + dpx * fieldSens(gesture.ch); cmd('addkey ' + gesture.ch + ' ' + st.playhead.toFixed(2) + ' ' + nv.toFixed(3)); }
        }
      } else if (!lmb && prevLmb && gesture) {
        if (gesture.type === 'rect') {
          var sel = keysInRect(gesture); if (sel.length) cmd('selset ' + sel.join(' ')); else cmd('selclear');
          rectVis.visible = false;
        } else if (gesture.type === 'ruler') {
          rawCmd('demo_gototick ' + Math.round(xToTick(curCursor().clx))); cmd('playhead release');
        } else if (gesture.type === 'num') { if (!gesture.moved) openType(gesture.ch); }
        gesture = null;
      }
      prevLmb = lmb;
    }
    var selCache = {};
    function isSelectedId(c, id) { return !!selCache[c + ':' + id]; }
    function updateRect(g) {
      var x0 = Math.min(g.sx, g.cx), x1 = Math.max(g.sx, g.cx), y0 = Math.min(g.sy, g.cy), y1 = Math.max(g.sy, g.cy);
      rectVis.visible = true; rectVis.style.position = x0.toFixed(0) + 'px ' + y0.toFixed(0) + 'px 0px';
      rectVis.style.width = (x1 - x0).toFixed(0) + 'px'; rectVis.style.height = (y1 - y0).toFixed(0) + 'px';
    }
)GEJS"
R"GEJS(
    // ---- drawing --------------------------------------------------------
    function drawGrid() {
      gridLayer.RemoveAndDeleteChildren();
      var ach = chById(activeCh) || model[0];
      // vertical time lines + labels
      var span = st.viewT1 - st.viewT0; var step = niceStep(span / 8);
      var t0 = Math.ceil(st.viewT0 / step) * step;
      rulerLayer.RemoveAndDeleteChildren();
      for (var t = t0; t <= st.viewT1; t += step) {
        var x = tickToX(t); if (x < 0 || x > G.cw) continue;
        lineSeg(gridLayer, x, 0, x, G.ch, S.grid, 1);
        var tl = lbl(rulerLayer, '' + Math.round(t), S.dim, 10);
        tl.style.position = (x + 2).toFixed(0) + 'px 6px 0px';
      }
      // horizontal value lines + labels (active channel real units)
      if (ach) for (var gi = 0; gi <= 4; gi++) {
        var y = gi / 4 * G.ch; lineSeg(gridLayer, 0, y, G.cw, y, gi === 2 ? S.gridMid : S.grid, 1);
        var v = yToVal(y, ach); var vl = lbl(gridLayer, v.toFixed(1), S.dim, 9);
        vl.style.position = '2px ' + (y + 1).toFixed(0) + 'px 0px';
      }
    }
    function niceStep(raw) { var p = Math.pow(10, Math.floor(Math.log(Math.max(1, raw)) / Math.LN10)); var n = raw / p; var m = n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10; return Math.max(1, m * p); }

    function drawCurves() {
      curveLayer.RemoveAndDeleteChildren();
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis || ch.keys.length === 0) continue;
        var col = ch.edit ? ch.color : (ch.color + '66');
        var k = ch.keys;
        // flat extrapolation before first / after last
        var fy = valToY(k[0].v, ch); lineSeg(curveLayer, 0, fy, tickToX(k[0].t), fy, ch.color + '55', 1);
        var ly = valToY(k[k.length - 1].v, ch); lineSeg(curveLayer, tickToX(k[k.length - 1].t), ly, G.cw, ly, ch.color + '55', 1);
        for (var j = 0; j + 1 < k.length; j++) {
          var a = k[j], b = k[j + 1];
          if (segActive(a, b)) {
            var P = segPts(a, b); var steps = 14; var px = -1, py = -1;
            for (var s = 0; s <= steps; s++) {
              var u = s / steps; var xv = bez(P.p0x, P.p1x, P.p2x, P.p3x, u), yv = bez(P.p0y, P.p1y, P.p2y, P.p3y, u);
              var X = tickToX(xv), Y = valToY(yv, ch);
              if (px >= 0) lineSeg(curveLayer, px, py, X, Y, col, 2);
              px = X; py = Y;
            }
          } else { lineSeg(curveLayer, tickToX(a.t), valToY(a.v, ch), tickToX(b.t), valToY(b.v, ch), col, 2); }
        }
      }
    }
    function drawKeyframes() {
      kfLayer.RemoveAndDeleteChildren();
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; var x = tickToX(k.t), y = valToY(k.v, ch); if (x < -10 || x > G.cw + 10) continue;
          dot(kfLayer, x, y, k.sel ? 12 : 9, k.sel ? S.sel : ch.color, true);
        }
      }
    }
    function drawHandles() {
      handleLayer.RemoveAndDeleteChildren();
      for (var i = 0; i < model.length; i++) {
        var ch = model[i]; if (!ch.vis) continue;
        for (var j = 0; j < ch.keys.length; j++) {
          var k = ch.keys[j]; if (!k.sel) continue;
          var kx = tickToX(k.t), ky = valToY(k.v, ch);
          [-1, 1].forEach(function (side) {
            var e = handleEndpoint(ch, k, side); var hx = tickToX(e.t), hy = valToY(e.v, ch);
            lineSeg(handleLayer, kx, ky, hx, hy, S.handle, 1);
            dot(handleLayer, hx, hy, 7, S.handle, false);
          });
        }
      }
    }
)GEJS"
R"GEJS(
    function updateChannelRows() {
      for (var c = 0; c < rows.length; c++) {
        var ch = chById(c); var r = rows[c]; if (!ch) continue;
        r.sw.style.backgroundColor = ch.color;
        r.nm.text = ch.name; r.nm.style.color = ch.solo ? S.accent : (ch.edit ? S.value : S.dim);
        var v = sampleChan(ch, st.playhead);
        r.val.text = (v == null) ? '-' : v.toFixed(2);
        r.val.style.color = ch.edit ? S.accent : S.dim;
        r.eye.style.backgroundColor = ch.vis ? S.btnOn : S.btnBg;
        r.eyeL.style.color = ch.vis ? S.accent : S.dim;
        r.hot.style.backgroundColor = (c === activeCh) ? '#ffffff0c' : 'rgba(0,0,0,0)';
      }
    }

    var api = {};
    api.render = function () {
      var raw = root.GetAttributeString('state', ''); if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      model = st.channels || [];
      root.visible = !!st.enabled; if (!st.enabled) return;

      // Put the host timeline in its compact scrub view (once) so the bottom bar stays short and
      // this dock can sit above it. The timeline also self-suppresses its curve toggle while we're on.
      if (!didInitTimeline) { didInitTimeline = true; rawCmd('mirv_filmmaker camtl view timeline'); }

      // selection cache (used by hit-test press path before C++ echoes back)
      selCache = {};
      for (var i = 0; i < model.length; i++) for (var j = 0; j < model[i].keys.length; j++)
        if (model[i].keys[j].sel) selCache[model[i].ch + ':' + model[i].keys[j].id] = 1;

      activeCh = (st.selCh != null && st.selCh >= 0) ? st.selCh : 0;
      // Until we have a valid screen measurement (root reports 0 for the first layout passes after
      // a build / resolution switch), skip the layout-dependent draw rather than render squished.
      if (!computeGeom() || !G) { root.visible = false; return; }
      processInput();
      try {
        var dc = ctx.GetDemoControllerState && ctx.GetDemoControllerState();
        if (dc && typeof dc.fTimeScale === 'number') {
          activeSpeed = dc.fTimeScale;
          updateSpeedButtons();
        }
      } catch (speedErr) {}

      delBtn.__lbl.style.color = (st.selCount > 0) ? S.playhead : S.dim;
      undoBtn.__lbl.style.color = st.canUndo ? S.value : S.dim;
      redoBtn.__lbl.style.color = st.canRedo ? S.value : S.dim;
      hInfo.text = 'tick ' + Math.round(st.playhead) + '   ·   ' + st.selCount + ' selected   ·   '
        + (st.scrubbing ? 'SCRUBBING' : 'live') + '   ·   shift: lock/straighten · box-select · ctrl+A = all · right-click = ease';

      updateChannelRows();

      // Rebuild the (expensive) grid/curves/keyframes/handles only when the model, view, size
      // or active channel changed. The model rev bumps on every edit, so an active drag rebuilds
      // each frame; idle frames only move the playhead.
      var sig = st.rev + ':' + st.viewT0 + ':' + st.viewT1 + ':' + G.cw + ':' + G.ch + ':' + activeCh;
      if (sig !== lastSig) { lastSig = sig; drawGrid(); drawCurves(); drawKeyframes(); drawHandles(); }

      var phx = tickToX(st.playhead);
      phLine.style.position = phx.toFixed(1) + 'px 0px 0px';
      // Playhead grab handle: centred on the line, sitting in the ruler just above the canvas.
      if (phx >= -2 && phx <= G.cw + 2) {
        phHandle.visible = true;
        phHandle.style.position = (G.cx + phx - 7).toFixed(1) + 'px ' + (G.cy - 13) + 'px 0px';
      } else phHandle.visible = false;
    };

    $.GraphExp = api;
    api.render();
    $.Msg('[grapheditor] experimental graph editor built.\n');
  } catch (err) {
    $.Msg('[grapheditor] gui error: ' + err + '\n');
  }
})();
)GEJS";

} // namespace Filmmaker
