#pragma once

// Panorama JS for the CONFIG panel: a lightweight clone of the Camera Editor workspace
// (CameraEditorJs.h) built ONCE into the in-game HUD (CSGOHud) context by ConfigHud.cpp.
//
// Deliberately a SEPARATE, self-contained script: it keeps the editor's WHOLE layout --
// the letterboxed live preview left of a right inspector column, the bottom backdrop, the
// tab bar docked above CS2's native demo bar (which CameraTimelineJs docks to fit while
// Config is open), and the ⚙ gear with the native demo-playback settings fly-out -- but
// strips every camera tool: no Camera/Graph tabs (only "Regular Timeline"), no path/follow
// sections, no Customize. Just general UI / game-display configuration.
//
// Accent color is the theme's existing BLUE (the Camera Editor's "freeze" token) instead of
// the editor's gold, so the two workspaces read as siblings without being confusable.
//
// The game-HUD visibility ("Game UI") picker drives the SAME shared HudView state as the
// Camera Editor ("mirv_filmmaker editor hud ..."), applied centrally by CameraTimelineJs
// (which honours the picker while EITHER the editor is hosted OR Config is open).
//
//   C++ -> JS : attribute "state" ({enabled, cursor, hudView}), then $.ConfigHud.render().
//               render() (re)publishes the "previewrect" attribute -- normalised preview-rect
//               fractions -- which ConfigHud.cpp forwards to the D3D viewport scaler (same
//               single-source-of-truth contract as the Camera Editor).
//   JS  -> C++: buttons issue "mirv_filmmaker ..." console commands / cvar writes.

namespace Filmmaker {

inline const char* kConfigHudJs = R"CFJS(
(function () {
  try {
    var existing = $('#ConfigHudRoot'); if (existing) existing.DeleteAsync(0);
    var existingSettings = $('#ConfigSettingsRoot'); if (existingSettings) existingSettings.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    // Same theme tokens as CameraEditorJs.h, with the panel accent flipped to the theme's
    // blue (S.freeze there) so Config is visually distinct from the gold Camera Editor.
    var S = {
      accent: '#4aa3ffff', danger: '#c92a2acc',
      bg: '#0c0e12ff', bgSoft: '#14181eff',
      sectionBg: '#1a1f26ff', cardBorder: '#ffffff10',
      cardShadow: '#00000066 0px 3px 12px 0px',
      label: '#9aa4b0ff', value: '#eef2f6ff', dim: '#6b7480ff',
      btnBg: '#ffffff14', btnOn: '#4aa3ff33',
      clear: 'rgba(0,0,0,0)', // NEVER style.X = '' (aborts the script)
      font: 'Stratum2, "Arial Unicode MS"'
    };
    // Keep these in sync with CameraEditorJs.h so the two workspaces line up 1:1.
    var INSPECTOR_W = 430, BOTTOM_LIFT = 28;
    var TAB_BAR_H = 42, TAB_GAP = 4;
    var NATIVE_BAR_H = 96; // fallback height for CS2's native demo bar (measured at runtime)

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[confighud] cmd failed: ' + e + '\n'); }
    }
    function rdInt(name) {
      try { var v = parseInt(GameInterfaceAPI.GetSettingString(name)); return isFinite(v) ? v : 0; }
      catch (e) { return 0; }
    }
    function finitePositive(v) { return typeof v === 'number' && isFinite(v) && v > 1; }

    // ---- minimal builders (same recipes as CameraEditorWidgetsJs.h, kept local so this
    //      script stays standalone) ----
    function mk(type, parent, props) { return $.CreatePanel(type, parent, '', props || {}); }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color; if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font; return l;
    }
    function btn(parent, text, onClick, color) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.backgroundColor = S.btnBg; b.style.borderRadius = '3px';
      b.style.border = '1px solid ' + S.cardBorder;
      b.style.paddingTop = '6px'; b.style.paddingBottom = '6px';
      b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
      b.style.marginRight = '5px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, 14); l.style.fontWeight = 'bold';
      b.SetPanelEvent('onactivate', onClick);
      b.__lbl = l; return b;
    }
    function row(parent) {
      var r = mk('Panel', parent); r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '4px'; r.style.verticalAlign = 'middle'; return r;
    }
    function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
    // Layered slider: rounded track + a colored fill bar drawn UNDER a real native Slider
    // (same recipe as CameraEditorWidgetsJs.h's makeSlider, kept local so this script stays
    // standalone). The native Slider is the only drag mechanism available -- this HUD context
    // has no mouse-move event for a hand-rolled draggable control.
    function makeSlider(parent, height, fillColor) {
      var wrap = mk('Panel', parent);
      wrap.style.width = 'fill-parent-flow(1.0)'; wrap.style.height = (height || 16) + 'px';
      wrap.style.verticalAlign = 'center';
      var track = mk('Panel', wrap); track.hittest = false;
      track.style.width = '100%'; track.style.height = '4px'; track.style.borderRadius = '2px';
      track.style.verticalAlign = 'center'; track.style.backgroundColor = S.btnBg;
      var fill = mk('Panel', wrap); fill.hittest = false;
      fill.style.width = '0%'; fill.style.height = '4px'; fill.style.borderRadius = '2px';
      fill.style.verticalAlign = 'center'; fill.style.horizontalAlign = 'left';
      fill.style.backgroundColor = fillColor || S.accent;
      var sl = $.CreatePanel('Slider', wrap, '', { direction: 'horizontal' }); sl.AddClass('HorizontalSlider');
      sl.style.width = '100%'; sl.style.height = '100%'; sl.style.verticalAlign = 'center';
      return { wrap: wrap, sl: sl, setFill: function (frac) {
        fill.style.width = (clamp01(frac) * 100).toFixed(1) + '%';
      } };
    }
    function section(parent, title) {
      var s = mk('Panel', parent); s.style.flowChildren = 'down'; s.style.width = '100%';
      s.style.marginTop = '12px'; s.style.paddingTop = '9px'; s.style.paddingBottom = '11px';
      s.style.paddingLeft = '11px'; s.style.paddingRight = '11px';
      s.style.backgroundColor = S.sectionBg; s.style.borderRadius = '6px';
      s.style.border = '1px solid ' + S.cardBorder;
      s.style.boxShadow = S.cardShadow;
      var hr = mk('Panel', s); hr.style.flowChildren = 'right'; hr.style.marginBottom = '7px';
      var chip = mk('Panel', hr); chip.hittest = false;
      chip.style.width = '3px'; chip.style.height = '10px'; chip.style.borderRadius = '1px';
      chip.style.backgroundColor = S.accent; chip.style.verticalAlign = 'center'; chip.style.marginRight = '7px';
      var t = lbl(hr, title, S.label, 11); t.style.fontWeight = 'bold'; t.style.letterSpacing = '2px';
      t.style.verticalAlign = 'center';
      return s;
    }

    // ---- root: full-screen, non-hittest. Children draw bottom -> top (editor clone). ----
    var root = $.CreatePanel('Panel', ctx, 'ConfigHudRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%';
    root.style.zIndex = '52'; // just under CamEditorRoot (53) -- mutually exclusive anyway

    // Preview catcher: swallows clicks over the preview (UI-cursor mode only) so clicking the
    // game view can't leak through to spectator target switching. render() shrinks its height
    // to stop above the bottom band so the native demo bar stays clickable (editor native mode).
    var catcher = mk('Panel', root); catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.style.position = '0px 0px 0px'; catcher.hittest = false;
    catcher.SetPanelEvent('onactivate', function () { /* swallow preview clicks */ });

    // View status tag pinned to the top-left of the live preview (same pulse as the editor).
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
    var previewPulseBright = true;
    function pulsePreviewDot() {
      if (!dot || !dot.IsValid || !dot.IsValid()) return;
      previewPulseBright = !previewPulseBright;
      dot.style.opacity = previewPulseBright ? '1.0' : '0.22';
      $.Schedule(previewPulseBright ? 0.75 : 0.30, pulsePreviewDot);
    }
    $.Schedule(0.75, pulsePreviewDot);

    // Bottom backdrop: workspace footer strip behind the tab bar, anchored ABOVE the native
    // demo bar so the bar itself (a lower-z HUD root) stays visible + clickable.
    var backdrop = mk('Panel', root); backdrop.hittest = false;
    backdrop.style.verticalAlign = 'top'; backdrop.style.horizontalAlign = 'left';
    backdrop.style.position = '0px 0px 0px';
    backdrop.style.width = '100%'; backdrop.style.height = TAB_BAR_H + 'px';
    backdrop.style.backgroundColor = S.bg; backdrop.style.borderTop = '1px solid #ffffff14';

    // Bottom tab bar: same divider as the editor's, but Config has ONLY the Regular Timeline
    // (the native CS2 demo bar is always the bottom editor here) + the ⚙ gear on the right.
    var tabBar = mk('Panel', root); tabBar.hittest = false;
    tabBar.style.verticalAlign = 'top'; tabBar.style.horizontalAlign = 'left';
    tabBar.style.position = '0px 0px 0px';
    tabBar.style.height = TAB_BAR_H + 'px';
    tabBar.style.backgroundColor = S.bgSoft;
    tabBar.style.borderTop = '1px solid ' + S.accent; tabBar.style.borderBottom = '1px solid ' + S.accent;
    tabBar.style.flowChildren = 'right';
    tabBar.style.paddingTop = '4px'; tabBar.style.paddingBottom = '4px';
    tabBar.style.paddingLeft = '12px'; tabBar.style.paddingRight = '12px';
    var regularViewBtn = btn(tabBar, 'Regular Timeline', function () { /* only mode -- always active */ }, S.accent);
    regularViewBtn.style.verticalAlign = 'center'; regularViewBtn.style.width = '150px';
    regularViewBtn.style.backgroundColor = S.btnOn;
    regularViewBtn.style.border = '1px solid ' + S.accent;
    regularViewBtn.__lbl.style.horizontalAlign = 'center';
    var tabSpacer = mk('Panel', tabBar); tabSpacer.hittest = false;
    tabSpacer.style.width = 'fill-parent-flow(1.0)'; tabSpacer.style.height = '1px';
    var gearBtn = btn(tabBar, '⚙', function () { toggleSettings(); }, S.value);
    gearBtn.style.verticalAlign = 'center'; gearBtn.style.marginRight = '0px';
    gearBtn.style.width = '38px'; gearBtn.__lbl.style.horizontalAlign = 'center';
    gearBtn.__lbl.style.fontSize = '18px';
)CFJS"
R"CFJS(
    // ===================== CONFIG SETTINGS MENU =========================
    // Same fly-out as the Camera Editor's gear: mirrors CS2's native demo-playback settings
    // (X-Ray / True View / DOA / mismatch), each driving the SAME cvar the native panel does,
    // read back live so the pills reflect engine state. TOP-LEVEL root (sibling of
    // ConfigHudRoot) so its z beats the timeline root; deleted in ConfigHud::Teardown.
    var settingsOverlay = $.CreatePanel('Panel', ctx, 'ConfigSettingsRoot', {});
    settingsOverlay.visible = false; settingsOverlay.hittest = true;
    settingsOverlay.style.width = '100%'; settingsOverlay.style.height = '100%'; settingsOverlay.style.zIndex = '221';
    settingsOverlay.SetPanelEvent('onactivate', function () { closeSettings(); });
    var settingsCard = mk('Panel', settingsOverlay); settingsCard.hittest = true;
    settingsCard.style.width = '360px'; settingsCard.style.horizontalAlign = 'right'; settingsCard.style.verticalAlign = 'bottom';
    settingsCard.style.marginRight = (INSPECTOR_W + 16) + 'px'; settingsCard.style.marginBottom = (TAB_BAR_H + NATIVE_BAR_H + 16) + 'px';
    settingsCard.style.backgroundColor = 'rgba(14,18,24,0.99)';
    settingsCard.style.border = '1px solid ' + S.accent; settingsCard.style.borderRadius = '6px';
    settingsCard.style.boxShadow = '#000000ee 0px 0px 26px 5px';
    settingsCard.style.flowChildren = 'down';
    settingsCard.style.paddingTop = '14px'; settingsCard.style.paddingBottom = '16px';
    settingsCard.style.paddingLeft = '16px'; settingsCard.style.paddingRight = '16px';
    settingsCard.SetPanelEvent('onactivate', function () { /* swallow clicks inside */ });
    var settingsTitle = lbl(settingsCard, 'CONFIG SETTINGS', S.dim, 12);
    settingsTitle.style.fontWeight = 'bold'; settingsTitle.style.letterSpacing = '2px'; settingsTitle.style.marginBottom = '4px';
    function settingToggle(label, explain, onClick) {
      var r = mk('Panel', settingsCard); r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '12px'; r.style.verticalAlign = 'middle';
      var col = mk('Panel', r); col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      col.style.verticalAlign = 'center'; col.style.paddingRight = '12px';
      var t = lbl(col, label, S.value, 14); t.style.fontWeight = 'bold';
      if (explain) { var e = lbl(col, explain, S.dim, 10); e.style.marginTop = '2px'; e.style.whiteSpace = 'normal'; }
      var pill = mk('Panel', r); pill.hittest = true; pill.style.width = '46px'; pill.style.height = '24px';
      pill.style.borderRadius = '12px'; pill.style.backgroundColor = '#00000088'; pill.style.verticalAlign = 'center';
      pill.style.border = '1px solid #ffffff14';
      var knob = mk('Panel', pill); knob.hittest = false; knob.style.width = '18px'; knob.style.height = '18px';
      knob.style.borderRadius = '9px'; knob.style.backgroundColor = '#cfd6deff'; knob.style.verticalAlign = 'center';
      knob.style.marginLeft = '3px'; knob.style.marginRight = '3px';
      pill.SetPanelEvent('onactivate', function () { onClick(); $.Schedule(0, refreshSettings); refreshSettings(); });
      return { row: r, label: t, pill: pill, knob: knob };
    }
    function setToggle(rec, on, enabled) {
      rec.pill.style.backgroundColor = on ? '#2bb24cff' : '#00000088';
      rec.pill.style.border = '1px solid ' + (on ? '#1c8a3aff' : '#ffffff14');
      rec.knob.style.horizontalAlign = on ? 'right' : 'left';
      rec.row.style.opacity = enabled ? '1.0' : '0.4';
      rec.pill.hittest = enabled;
    }
    var xrayTog = settingToggle('X-Ray', 'See players through walls (spec_show_xray).', function () {
      cmd('spec_show_xray ' + (rdInt('spec_show_xray') ? 0 : 1));
    });
    var trueviewTog = settingToggle('True View', 'Render each POV as the player actually saw it.', function () {
      var p = rdInt('cl_demo_predict');
      if (p > 0) cmd('cl_demo_predict 0');
      else cmd('cl_demo_predict ' + (rdInt('cl_demo_predict') >= 2 ? 2 : 1));
    });
    var doaTog = settingToggle('Include DOA actions', 'Predict dead-or-alive actions (needs True View).', function () {
      if (rdInt('cl_demo_predict') <= 0) return;
      cmd('cl_trueview_show_doa_predictions ' + (rdInt('cl_trueview_show_doa_predictions') ? 0 : 1));
    });
    var mismatchTog = settingToggle('Allow demo mismatch versions', 'Apply True View even on a mismatched demo build.', function () {
      var p = rdInt('cl_demo_predict');
      if (p === 1) cmd('cl_demo_predict 2');
      else if (p === 2) cmd('cl_demo_predict 1');
    });
    function refreshSettings() {
      var predict = rdInt('cl_demo_predict');
      var tvOn = predict > 0;
      setToggle(xrayTog, rdInt('spec_show_xray') !== 0, true);
      setToggle(trueviewTog, tvOn, true);
      setToggle(doaTog, rdInt('cl_trueview_show_doa_predictions') !== 0, tvOn);
      setToggle(mismatchTog, predict >= 2, tvOn);
    }
    function openSettings() { refreshSettings(); settingsOverlay.visible = true; gearBtn.style.backgroundColor = S.btnOn; }
    function closeSettings() { settingsOverlay.visible = false; gearBtn.style.backgroundColor = S.btnBg; }
    function toggleSettings() { if (settingsOverlay.visible) closeSettings(); else openSettings(); }

    // Right inspector column, same chrome as the Camera Editor's.
    var inspector = mk('Panel', root); inspector.hittest = false;
    inspector.style.horizontalAlign = 'right'; inspector.style.verticalAlign = 'top';
    inspector.style.width = INSPECTOR_W + 'px'; inspector.style.height = '100%';
    inspector.style.backgroundColor = S.bg; inspector.style.borderLeft = '1px solid #ffffff14';
    inspector.style.flowChildren = 'down';
    inspector.style.paddingTop = '14px'; inspector.style.paddingBottom = '14px';
    inspector.style.paddingLeft = '14px'; inspector.style.paddingRight = '14px';
    inspector.style.fontFamily = S.font;
    inspector.style.overflow = 'squish scroll';

    // Aspect-ratio letterbox: black mask bars + accent frame around the shrunk preview
    // (identical recipe to the editor; geometry computed each render).
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

    function hidePreviewLayout() {
      frameL.visible = frameV.visible = frameT.visible = frameH.visible = false;
      barLeft.visible = barRight.visible = barTop.visible = barBottom.visible = false;
      tag.visible = false;
      root.SetAttributeString('previewrect', '');
    }
)CFJS"
R"CFJS(
    // ===================== HEADER =======================================
    var head = row(inspector); head.style.marginTop = '0px';
    var hTitle = lbl(head, 'CONFIG', S.value, 16); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.width = 'fill-parent-flow(1.0)';
    hTitle.style.verticalAlign = 'center';
    var exitBtn = btn(head, '✕ Exit', function () { cmd('mirv_filmmaker config close'); }, S.value);
    exitBtn.style.marginRight = '0px';

    var mouseRow = row(inspector);
    var mouseBtn = btn(mouseRow, 'MOUSE: UI  (G)', function () { cmd('mirv_filmmaker camtl cursor toggle'); }, S.accent);
    mouseBtn.style.width = 'fill-parent-flow(1.0)'; mouseBtn.style.marginRight = '0px';
    mouseBtn.__lbl.style.horizontalAlign = 'center';

    // ===================== GAME UI ======================================
    var uiSec = section(inspector, 'GAME UI');
    var HUD_CYCLE = [['hidden', 'Hide All'], ['game', 'In-Game'], ['full', 'Show All']];
    var hudCycle = btn(uiSec, 'Game UI: Show All', function () {
      var cur = (st && st.hudView) || 'full', idx = 0;
      for (var i = 0; i < HUD_CYCLE.length; i++) if (HUD_CYCLE[i][0] === cur) idx = i;
      cmd('mirv_filmmaker editor hud ' + HUD_CYCLE[(idx + 1) % HUD_CYCLE.length][0]);
    }, S.value);
    hudCycle.style.width = '100%'; hudCycle.style.marginRight = '0px';
    hudCycle.__lbl.style.horizontalAlign = 'center';
)CFJS"
#include "ConfigHudModifiersJs.h"
R"CFJS(
    // ===================== EFFECTS ======================================
    // Particle-effect toggles (ParticleFx.h): each category is a segmented mode control
    // driving 'mirv_filmmaker fx set <cat> <mode>'; state mirrors back through the fx_*
    // fields each render (console changes show up here too). On = Povarehok
    // (regular), Less = the mod's reduced variants (offered ONLY where its less
    // folders really differ: impacts, HE, bomb -- verified 2026-07-03), Modern = the
    // converted MW2019 ARC9 pack (offered where it ships assets: tracers, weapon fx, HE),
    // Off = default CS2 pass-through. HE grenade and the bomb blast are separate rows so
    // they can mix (e.g. Povarehok bomb + Modern HE). ('More' was dropped
    // 2026-07-02 -- byte-identical to On.) Smoke GRENADES are never affected (CS2
    // volumetric smoke is not a particle swap).
    var fxSec = section(inspector, 'EFFECTS');
    var fxMaster;
    (function () {
      var r = row(fxSec); r.style.marginTop = '2px';
      var t = lbl(r, 'Effects Control', S.value, 13); t.style.fontWeight = 'bold';
      t.style.width = 'fill-parent-flow(1.0)'; t.style.verticalAlign = 'center';
      var pill = mk('Panel', r); pill.hittest = true; pill.style.width = '46px'; pill.style.height = '24px';
      pill.style.borderRadius = '12px'; pill.style.backgroundColor = '#00000088'; pill.style.verticalAlign = 'center';
      pill.style.border = '1px solid #ffffff14';
      var knob = mk('Panel', pill); knob.hittest = false; knob.style.width = '18px'; knob.style.height = '18px';
      knob.style.borderRadius = '9px'; knob.style.backgroundColor = '#cfd6deff'; knob.style.verticalAlign = 'center';
      knob.style.marginLeft = '3px'; knob.style.marginRight = '3px';
      pill.SetPanelEvent('onactivate', function () {
        cmd('mirv_filmmaker fx ' + (st && st.fxOn ? 'off' : 'on') + ' quiet');
      });
      fxMaster = { pill: pill, knob: knob };
    })();
    function fxSeg(label, catKey, modes) {
      var head = row(fxSec); head.style.marginTop = '6px';
      var nl = lbl(head, label, S.value, 13);
      nl.style.width = 'fill-parent-flow(1.0)'; nl.style.verticalAlign = 'center';
      var group = mk('Panel', head); group.style.flowChildren = 'right'; group.style.verticalAlign = 'center';
      var caps = { on: 'On', more: 'More', less: 'Less', modern: 'Modern', off: 'Off' };
      var btns = {};
      for (var i = 0; i < modes.length; i++) (function (mode, isLast) {
        var b = btn(group, caps[mode], function () {
          cmd('mirv_filmmaker fx set ' + catKey + ' ' + mode + ' quiet');
        }, S.label);
        b.style.paddingTop = '3px'; b.style.paddingBottom = '3px';
        b.style.paddingLeft = '9px'; b.style.paddingRight = '9px';
        b.style.marginRight = isLast ? '0px' : '4px';
        b.__lbl.style.fontSize = '12px';
        btns[mode] = b;
      })(modes[i], i + 1 === modes.length);
      return { sync: function (mode) {
        for (var m in btns) {
          var on = (m === mode);
          btns[m].style.backgroundColor = on ? S.btnOn : S.btnBg;
          btns[m].style.border = '1px solid ' + (on ? S.accent : S.cardBorder);
          btns[m].__lbl.style.color = on ? S.accent : S.label;
        }
      } };
    }
    // Per-row mode subsets mirror ParticleFx's ModeSupported() -- offering a mode with no
    // distinct assets just confuses (it would snap back to On on the C++ side anyway).
    var fxCtls = [
      ['fx_impacts',    fxSeg('Bullet Impacts', 'impacts', ['on', 'less', 'off'])],
      ['fx_tracers',    fxSeg('Bullet Tracers', 'tracers', ['on', 'modern', 'off'])],
      ['fx_weaponfx',   fxSeg('Muzzle Flash & Shells', 'weaponfx', ['on', 'modern', 'off'])],
      ['fx_blood',      fxSeg('Blood', 'blood', ['on', 'off'])],
      ['fx_explosions', fxSeg('HE Grenade', 'explosions', ['on', 'less', 'modern', 'off'])],
      ['fx_bombfx',     fxSeg('Bomb (C4)', 'bombfx', ['on', 'less', 'off'])],
      ['fx_molotov',    fxSeg('Molotov Fire', 'molotov', ['on', 'off'])],
      ['fx_mapfx',      fxSeg('Map Ambience', 'mapfx', ['on', 'off'])]
    ];
    // Money-on-headshot: swaps the headshot-only particles (independent of the Blood mode).
    var moneyBtn;
    (function () {
      var r = row(fxSec); r.style.marginTop = '6px';
      var t = lbl(r, 'Money on Headshot', S.value, 13);
      t.style.width = 'fill-parent-flow(1.0)'; t.style.verticalAlign = 'center';
      moneyBtn = btn(r, 'Off', function () {
        cmd('mirv_filmmaker fx moneyshot ' + (st && st.fxMoneyshot ? 'off' : 'on'));
      }, S.label);
      moneyBtn.style.paddingTop = '3px'; moneyBtn.style.paddingBottom = '3px';
      moneyBtn.style.paddingLeft = '9px'; moneyBtn.style.paddingRight = '9px';
      moneyBtn.__lbl.style.fontSize = '12px';
    })();
    var fxNote = lbl(fxSec, "FX starts Off. Choosing any non-Off mode turns Effects Control on and lazily loads only the selected categories. On uses the converted Povarehok pack (Regular). Less uses the mod's reduced variants (only offered where they actually differ). Modern uses the converted MW2019 pack: class muzzle flashes, per-shot barrel smoke, MW tracers, and the MW frag explosion for HE. HE Grenade and Bomb are separate so you can mix packs. Off uses default CS2. Missing converted assets fail open to the original CS2 effect. Money on Headshot replaces the headshot-only blood/helmet particles, so it fires exactly on real headshot hits. Smoke grenades are never affected.", S.dim, 10);
    fxNote.style.whiteSpace = 'normal'; fxNote.style.marginTop = '7px';
    var fxStatus = lbl(fxSec, 'Hook not armed yet - effects play unmodified until it arms (auto-retries).', '#e8b339ff', 10);
    fxStatus.style.whiteSpace = 'normal'; fxStatus.style.marginTop = '3px'; fxStatus.visible = false;

    // ===================== PREVIEW / BOTTOM-BAND LAYOUT =================
    // Same geometry math as the editor's "Regular Timeline" bottom mode: the tab bar docks
    // directly above CS2's native demo bar (measured live), the backdrop fills the gap strip
    // behind the tab bar, and the preview shrinks to clear both + the inspector.
    function layoutWorkspace() {
      var rsx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var rsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rawW = root.actuallayoutwidth || 0; if (rawW < 16) rawW = ctx.actuallayoutwidth || 0;
      var rawH = root.actuallayoutheight || 0; if (rawH < 16) rawH = ctx.actuallayoutheight || 0;
      var rw = rawW / rsx, rh = rawH / rsy;
      if (!finitePositive(rw) || !finitePositive(rh)) { hidePreviewLayout(); return; }
      tabBar.style.width = Math.floor(Math.max(0, rw - INSPECTOR_W)) + 'px';

      // Native demo bar height (docked left of the inspector by CameraTimelineJs).
      var natH = NATIVE_BAR_H;
      try {
        var sr = ctx.FindChildTraverse && ctx.FindChildTraverse('SliderRow');
        var nc = sr && sr.GetParent ? sr.GetParent() : null;
        var nh = (nc && nc.visible) ? nc.actuallayoutheight / rsy : 0;
        if (nh > 10 && nh < rh * 0.6) natH = nh;
      } catch (eNat) {}

      var editorTop = natH;
      var bottomH = Math.max(TAB_BAR_H + BOTTOM_LIFT, Math.min(editorTop + TAB_GAP + TAB_BAR_H, Math.max(0, rh - 160)));
      var dockTopY = Math.max(0, rh - bottomH);
      var desiredTabY = rh - editorTop - TAB_GAP - TAB_BAR_H;
      var tabY = Math.max(dockTopY, desiredTabY);
      tabBar.style.position = '0px 0px 0px';
      tabBar.style.marginTop = Math.floor(tabY) + 'px';
      tabBar.style.marginBottom = '0px';
      // Gear settings card glued right above the tab bar's right end (clamped on-screen).
      var scMB = Math.max(0, rh - tabY) + 8;
      var scH = (settingsCard.actuallayoutheight || 0) / rsy;
      if (scH > 8 && rh > scH + 16) scMB = Math.min(scMB, rh - scH - 8);
      settingsCard.style.marginBottom = Math.floor(Math.max(8, scMB)) + 'px';
      settingsCard.style.marginRight = (INSPECTOR_W + 16) + 'px';
      // Backdrop anchored ABOVE the native bar so it fills the TAB_GAP strip behind the tab
      // bar without covering the bar itself (which is a lower-z HUD root).
      backdrop.style.position = '0px 0px 0px';
      backdrop.style.marginTop = Math.floor(dockTopY) + 'px';
      backdrop.style.marginBottom = '0px';
      backdrop.style.height = Math.floor(Math.max(0, bottomH - editorTop)) + 'px';
      backdrop.visible = true;
      catcher.style.height = Math.floor(Math.max(0, rh - bottomH)) + 'px';

      var areaW = rw - INSPECTOR_W, areaH = rh - bottomH;
      if (!finitePositive(areaW) || !finitePositive(areaH)) { hidePreviewLayout(); return; }
      var aspect = rw / rh;
      var pw = areaW, ph = pw / aspect;
      if (ph > areaH) { ph = areaH; pw = ph * aspect; }
      pw = Math.floor(pw); ph = Math.floor(ph);
      if (!finitePositive(pw) || !finitePositive(ph)) { hidePreviewLayout(); return; }
      var ox = Math.floor((areaW - pw) / 2); if (ox < 0) ox = 0;
      var oy = Math.floor((areaH - ph) / 2); if (oy < 0) oy = 0;
      var rgt = ox + pw, bot = oy + ph;

      barLeft.style.position = '0px 0px 0px';
      barLeft.style.width = ox + 'px'; barLeft.style.height = Math.floor(areaH) + 'px';
      barLeft.visible = ox > 1;
      barRight.style.position = rgt + 'px 0px 0px';
      barRight.style.width = Math.floor(areaW - rgt) + 'px'; barRight.style.height = Math.floor(areaH) + 'px';
      barRight.visible = (areaW - rgt) > 1;
      barTop.style.position = ox + 'px 0px 0px';
      barTop.style.width = pw + 'px'; barTop.style.height = oy + 'px';
      barTop.visible = oy > 1;
      barBottom.style.position = ox + 'px ' + bot + 'px 0px';
      barBottom.style.width = pw + 'px'; barBottom.style.height = Math.floor(areaH - bot) + 'px';
      barBottom.visible = (areaH - bot) > 1;

      frameL.style.position = ox + 'px ' + oy + 'px 0px'; frameL.style.height = ph + 'px'; frameL.visible = true;
      frameV.style.position = (rgt - 2) + 'px ' + oy + 'px 0px'; frameV.style.height = ph + 'px'; frameV.visible = true;
      frameT.style.position = ox + 'px ' + oy + 'px 0px'; frameT.style.width = pw + 'px'; frameT.visible = true;
      frameH.style.position = ox + 'px ' + (bot - 2) + 'px 0px'; frameH.style.width = pw + 'px'; frameH.visible = true;

      tag.visible = true;
      tag.style.position = (ox + 18) + 'px ' + (oy + 14) + 'px 0px';

      root.SetAttributeString('previewrect',
        (ox / rw).toFixed(5) + ' ' + (oy / rh).toFixed(5) + ' ' + (rgt / rw).toFixed(5) + ' ' + (bot / rh).toFixed(5));
    }

    // ===================== RENDER =======================================
    var st = null;
    var api = {};
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; hidePreviewLayout(); return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      root.visible = !!st.enabled;
      if (!st.enabled) {
        closeSettings();
        catcher.hittest = false;
        backdrop.visible = false; tabBar.visible = false; inspector.visible = false;
        hidePreviewLayout();
        return;
      }
      inspector.visible = true;
      tabBar.visible = true;

      var cur = !!st.cursor;
      if (!cur) closeSettings(); // can't click the popup in GAME-mouse mode; don't leave it hanging
      catcher.hittest = cur;
      inspector.hittest = cur;
      backdrop.hittest = cur;
      tabBar.hittest = cur;
      mouseBtn.__lbl.text = cur ? 'MOUSE: UI  (G)' : 'MOUSE: GAME  (G)';
      mouseBtn.style.backgroundColor = cur ? S.btnOn : S.btnBg;
      mouseBtn.__lbl.style.color = cur ? S.accent : S.label;

      var hv = st.hudView || 'full', hudName = 'Show All';
      for (var hb = 0; hb < HUD_CYCLE.length; hb++) if (HUD_CYCLE[hb][0] === hv) hudName = HUD_CYCLE[hb][1];
      hudCycle.__lbl.text = 'Game UI: ' + hudName;

      // MODIFIERS: reflect ViewFx / BodyCam state each frame (both can also change from the
      // console, so this isn't purely a click-echo).
      rollCtl.sync(st.rollPct);
      bobCtl.sync(st.bobPct);
      swayCtl.sync(st.swayPct);
      deadzoneCtl.sync(st.deadzonePct);
      var bcOn = !!st.bodyCam;
      bodyCamBtn.__lbl.text = 'Body Cam: ' + (bcOn ? 'On' : 'Off');
      bodyCamBtn.style.backgroundColor = bcOn ? S.btnOn : S.btnBg;
      bodyCamBtn.__lbl.style.color = bcOn ? S.accent : S.value;
      var acOn = !!st.actionCam;
      actionCamBtn.__lbl.text = 'Action Cam: ' + (acOn ? 'On' : 'Off');
      actionCamBtn.style.backgroundColor = acOn ? S.btnOn : S.btnBg;
      actionCamBtn.__lbl.style.color = acOn ? S.accent : S.value;
      var feOn = !!st.acFisheye;
      acFisheyeBtn.__lbl.text = 'Fisheye Lens: ' + (feOn ? 'On' : 'Off');
      acFisheyeBtn.style.backgroundColor = feOn ? S.btnOn : S.btnBg;
      acFisheyeBtn.__lbl.style.color = feOn ? S.accent : S.value;

      // EFFECTS: mirror ParticleFx state (master pill + per-category segments). On is an
      // active classic-mod swap mode, so the hook-ready hint depends on the master switch.
      var fxOn = !!st.fxOn;
      fxMaster.pill.style.backgroundColor = fxOn ? '#2bb24cff' : '#00000088';
      fxMaster.pill.style.border = '1px solid ' + (fxOn ? '#1c8a3aff' : '#ffffff14');
      fxMaster.knob.style.horizontalAlign = fxOn ? 'right' : 'left';
      for (var fi = 0; fi < fxCtls.length; fi++) {
        var fxMode = st[fxCtls[fi][0]] || 'off';
        fxCtls[fi][1].sync(fxMode);
      }
      var msOn = !!st.fxMoneyshot;
      moneyBtn.__lbl.text = msOn ? 'On' : 'Off';
      moneyBtn.style.backgroundColor = msOn ? S.btnOn : S.btnBg;
      moneyBtn.style.border = '1px solid ' + (msOn ? S.accent : S.cardBorder);
      moneyBtn.__lbl.style.color = msOn ? S.accent : S.label;
      fxStatus.visible = fxOn && !st.fxReady;

      layoutWorkspace();
    };
    $.ConfigHud = api;
    $.Msg('[confighud] panel built.\n');
  } catch (e) {
    $.Msg('[confighud] build error: ' + e + '\n');
  }
})();
)CFJS";

} // namespace Filmmaker
