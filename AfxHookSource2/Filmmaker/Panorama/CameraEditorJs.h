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
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', freeze: '#4aa3ffff', danger: '#c92a2acc',
      bg: 'rgba(12,14,18,0.97)', bgSoft: 'rgba(20,24,30,0.97)',
      panelBorder: '#ffffff1f', sectionBg: '#ffffff0c',
      label: '#9aa4b0ff', value: '#eef2f6ff', dim: '#6b7480ff',
      btnBg: '#ffffff14', btnOn: '#f0b32333',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var INSPECTOR_W = 372, BOTTOM_H = 176;
    var CH_ROLL = 5, CH_FOV = 6;

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
      b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
      b.style.paddingLeft = '10px'; b.style.paddingRight = '10px';
      b.style.marginRight = '5px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, 13); l.style.fontWeight = 'bold';
      b.SetPanelEvent('onactivate', onClick);
      b.__lbl = l; return b;
    }
    function section(parent, title) {
      var s = mk('Panel', parent); s.style.flowChildren = 'down'; s.style.width = '100%';
      s.style.marginTop = '12px'; s.style.paddingTop = '8px'; s.style.paddingBottom = '10px';
      s.style.paddingLeft = '10px'; s.style.paddingRight = '10px';
      s.style.backgroundColor = S.sectionBg; s.style.borderRadius = '4px';
      var t = lbl(s, title, S.dim, 11); t.style.fontWeight = 'bold'; t.style.letterSpacing = '2px';
      t.style.marginBottom = '6px';
      return s;
    }
    function row(parent) {
      var r = mk('Panel', parent); r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '4px'; r.style.verticalAlign = 'middle'; return r;
    }
    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : 0; }
)EDJS"
R"EDJS(
    // ---- root: full-screen, non-hittest. Children draw bottom -> top. ----
    var root = $.CreatePanel('Panel', ctx, 'CamEditorRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '53';

    // Preview catcher: full-screen click swallow (UI-cursor mode only) so clicking the
    // game preview can't leak through to spectator target switching. Inspector/backdrop
    // children are drawn AFTER it, so their own clicks still land.
    var catcher = mk('Panel', root); catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.style.position = '0px 0px 0px'; catcher.hittest = false;
    catcher.SetPanelEvent('onactivate', function () { /* swallow */ });

    // 'PREVIEW' tag pinned to the top-left of the live preview.
    var tag = mk('Panel', root); tag.hittest = false;
    tag.style.position = '18px 14px 0px'; tag.style.flowChildren = 'right';
    tag.style.backgroundColor = 'rgba(12,14,18,0.65)'; tag.style.borderRadius = '3px';
    tag.style.paddingLeft = '8px'; tag.style.paddingRight = '10px';
    tag.style.paddingTop = '3px'; tag.style.paddingBottom = '3px';
    var dot = mk('Panel', tag); dot.style.width = '8px'; dot.style.height = '8px';
    dot.style.backgroundColor = S.danger; dot.style.borderRadius = '4px';
    dot.style.verticalAlign = 'center'; dot.style.marginRight = '7px';
    var tagLbl = lbl(tag, 'PREVIEW', S.value, 11); tagLbl.style.fontWeight = 'bold';
    tagLbl.style.letterSpacing = '2px'; tagLbl.style.verticalAlign = 'center';

    // Bottom backdrop (sits BEHIND the CameraTimeline panel, which is z55) = workspace
    // footer under the preview.
    var backdrop = mk('Panel', root); backdrop.hittest = false;
    backdrop.style.verticalAlign = 'bottom'; backdrop.style.horizontalAlign = 'left';
    backdrop.style.width = '100%'; backdrop.style.height = BOTTOM_H + 'px';
    backdrop.style.backgroundColor = S.bg; backdrop.style.borderTop = '1px solid #ffffff14';

    // Right inspector column.
    var inspector = mk('Panel', root); inspector.hittest = false;
    inspector.style.horizontalAlign = 'right'; inspector.style.verticalAlign = 'top';
    inspector.style.width = INSPECTOR_W + 'px'; inspector.style.height = '100%';
    inspector.style.backgroundColor = S.bg; inspector.style.borderLeft = '1px solid #ffffff14';
    inspector.style.flowChildren = 'down';
    inspector.style.paddingTop = '14px'; inspector.style.paddingBottom = '14px';
    inspector.style.paddingLeft = '14px'; inspector.style.paddingRight = '14px';
    inspector.style.fontFamily = S.font;

    // Aspect-ratio letterbox: the live preview is sized to the GAME's aspect ratio
    // (rootW/rootH) in the top-left, and the leftover preview area is masked with black
    // bars; an accent frame outlines the 16:9 rect. Geometry is computed each render
    // from the laid-out root size. (The game still renders full-screen behind, so the
    // preview is a CROP shaped to the render aspect, not a scaled copy.)
    var barBottom = mk('Panel', root); barBottom.hittest = false; barBottom.visible = false;
    barBottom.style.backgroundColor = '#000000ff'; barBottom.style.horizontalAlign = 'left'; barBottom.style.verticalAlign = 'top';
    var barRight = mk('Panel', root); barRight.hittest = false; barRight.visible = false;
    barRight.style.backgroundColor = '#000000ff'; barRight.style.horizontalAlign = 'left'; barRight.style.verticalAlign = 'top';
    var frameV = mk('Panel', root); frameV.hittest = false; frameV.visible = false;
    frameV.style.width = '2px'; frameV.style.backgroundColor = S.accent; frameV.style.horizontalAlign = 'left'; frameV.style.verticalAlign = 'top';
    var frameH = mk('Panel', root); frameH.hittest = false; frameH.visible = false;
    frameH.style.height = '2px'; frameH.style.backgroundColor = S.accent; frameH.style.horizontalAlign = 'left'; frameH.style.verticalAlign = 'top';

    // ===================== HEADER =======================================
    var head = row(inspector); head.style.marginTop = '0px';
    var hTitle = lbl(head, 'CAMERA EDITOR', S.value, 16); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.width = 'fill-parent-flow(1.0)';
    hTitle.style.verticalAlign = 'center';
    var exitBtn = btn(head, '✕ Exit', function () { cmd('mirv_filmmaker editor close'); }, S.value);
    exitBtn.style.marginRight = '0px';

    var mouseRow = row(inspector);
    var mouseBtn = btn(mouseRow, 'MOUSE: UI  (G)', function () { cmd('mirv_filmmaker camtl cursor toggle'); }, S.accent);
    mouseBtn.style.width = 'fill-parent-flow(1.0)'; mouseBtn.style.marginRight = '0px';
    mouseBtn.__lbl.style.horizontalAlign = 'center';

    // ===================== SELECTED CAMERA ==============================
    var selSec = section(inspector, 'SELECTED CAMERA');
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

    // ===================== TRANSFORM (readout) ==========================
    var xfSec = section(inspector, 'TRANSFORM');
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
    var interpBtn = btn(pathSec, 'Curve: Linear', function () { cmd('mirv_filmmaker marker interp cycle'); }, S.value);
    interpBtn.style.marginTop = '2px';
    var EASE = ['none', 'in', 'out', 'inout'], EASE_LBL = ['None', 'Ease In', 'Ease Out', 'Ease In/Out'];
    var easeBtn = btn(pathSec, 'Ease: None', function () {
      if (!st || st.selected < 0) return;
      var nx = (((st.sel && st.sel.ease) || 0) + 1) % 4;
      cmd('mirv_filmmaker camtl ease ' + st.selected + ' ' + EASE[nx]);
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

    // ===================== SMOOTHING (display-only) =====================
    var smoothSec = section(inspector, 'SMOOTHING');
    lbl(smoothSec, 'Playback low-pass: auto', S.value, 12);
    lbl(smoothSec, 'Output pose is glide-smoothed during playback.', S.dim, 10);

    // ===================== PLAYBACK hints ===============================
    var playSec = section(inspector, 'PLAYBACK');
    lbl(playSec, 'Space  ▶ / ⏸     ←/→  ±15s', S.value, 12);
    lbl(playSec, 'Scrub + keyframes on the timeline below.', S.dim, 10);
)EDJS"
R"EDJS(
    // =====================================================================
    var st = null;
    var api = {};
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }

      root.visible = !!st.enabled;
      if (!st.enabled) return;

      // UI-cursor gating: panels are only clickable in UI-mouse mode; in GAME mode the
      // mouse flies the free cam and Panorama receives no clicks anyway.
      var cur = !!st.cursor;
      catcher.hittest = cur;
      inspector.hittest = cur;
      backdrop.hittest = cur;
      mouseBtn.__lbl.text = cur ? 'MOUSE: UI  (G)' : 'MOUSE: GAME  (G)';
      mouseBtn.style.backgroundColor = cur ? S.btnOn : S.btnBg;
      mouseBtn.__lbl.style.color = cur ? S.accent : S.label;

      // Aspect-ratio letterbox. Virtual px = actuallayout / uiscale (matches style px).
      var rsx = root.actualuiscale_x || 1, rsy = root.actualuiscale_y || 1;
      var rw = (root.actuallayoutwidth || 0) / rsx, rh = (root.actuallayoutheight || 0) / rsy;
      if (rw > 10 && rh > 10) {
        var areaW = rw - INSPECTOR_W, areaH = rh - BOTTOM_H;
        var aspect = rw / rh;            // the game's render aspect (full window)
        var pw = areaW, ph = pw / aspect;
        if (ph > areaH) { ph = areaH; pw = ph * aspect; } // fit the rect inside the area
        pw = Math.floor(pw); ph = Math.floor(ph);
        var bb = areaH - ph, br = areaW - pw;
        barBottom.style.position = '0px ' + ph + 'px 0px';
        barBottom.style.width = areaW + 'px'; barBottom.style.height = (bb > 0 ? bb : 0) + 'px';
        barBottom.visible = bb > 1;
        barRight.style.position = pw + 'px 0px 0px';
        barRight.style.width = (br > 0 ? br : 0) + 'px'; barRight.style.height = ph + 'px';
        barRight.visible = br > 1;
        frameV.style.position = (pw - 2) + 'px 0px 0px'; frameV.style.height = ph + 'px'; frameV.visible = true;
        frameH.style.position = '0px ' + (ph - 2) + 'px 0px'; frameH.style.width = pw + 'px'; frameH.visible = true;
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
