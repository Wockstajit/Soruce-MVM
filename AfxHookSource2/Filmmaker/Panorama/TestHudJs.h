#pragma once

// Panorama JS for the mvm_test FX panel: effects-only right-side overlay for offline
// live matches. Built into CSGOHud by TestHud.cpp.

namespace Filmmaker {

inline const char* kTestHudJs = R"CFJS(
(function () {
  try {
    var existing = $('#TestHudRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#4aa3ffff', danger: '#c92a2acc',
      bg: '#0c0e12ff', bgSoft: '#14181eff',
      sectionBg: '#1a1f26ff', cardBorder: '#ffffff10',
      cardShadow: '#00000066 0px 3px 12px 0px',
      label: '#9aa4b0ff', value: '#eef2f6ff', dim: '#6b7480ff',
      btnBg: '#ffffff14', btnOn: '#4aa3ff33',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var INSPECTOR_W = 430;

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[testhud] cmd failed: ' + e + '\n'); }
    }
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

    var root = $.CreatePanel('Panel', ctx, 'TestHudRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%';
    root.style.zIndex = '52';

    var inspector = mk('Panel', root); inspector.hittest = false;
    inspector.style.horizontalAlign = 'right'; inspector.style.verticalAlign = 'top';
    inspector.style.width = INSPECTOR_W + 'px'; inspector.style.height = '100%';
    inspector.style.backgroundColor = S.bg; inspector.style.borderLeft = '1px solid #ffffff14';
    inspector.style.flowChildren = 'down';
    inspector.style.paddingTop = '14px'; inspector.style.paddingBottom = '14px';
    inspector.style.paddingLeft = '14px'; inspector.style.paddingRight = '14px';
    inspector.style.fontFamily = S.font;
    inspector.style.overflow = 'squish scroll';

    var head = row(inspector); head.style.marginTop = '0px';
    var hTitle = lbl(head, 'FX TEST', S.value, 16); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.width = 'fill-parent-flow(1.0)';
    hTitle.style.verticalAlign = 'center';
    var exitBtn = btn(head, '✕ Exit', function () { cmd('mvm_test menu close'); }, S.value);
    exitBtn.style.marginRight = '0px';

    var mouseRow = row(inspector);
    var mouseBtn = btn(mouseRow, 'MOUSE: UI  (G)', function () { cmd('mirv_filmmaker camtl cursor toggle'); }, S.accent);
    mouseBtn.style.width = 'fill-parent-flow(1.0)'; mouseBtn.style.marginRight = '0px';
    mouseBtn.__lbl.style.horizontalAlign = 'center';

    var hintLbl = lbl(inspector, 'Offline live match FX testing. Shoot to see changes (no demo reseek). Insert toggles this panel.', S.dim, 10);
    hintLbl.style.whiteSpace = 'normal'; hintLbl.style.marginTop = '6px'; hintLbl.style.marginBottom = '4px';

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
    var fxNote = lbl(fxSec, "FX starts Off. Choosing any non-Off mode turns Effects Control on. Shoot weapons / throw grenades to preview swaps. Smoke grenades are never affected.", S.dim, 10);
    fxNote.style.whiteSpace = 'normal'; fxNote.style.marginTop = '7px';
    var fxStatus = lbl(fxSec, 'Hook not armed yet - effects play unmodified until it arms (auto-retries).', '#e8b339ff', 10);
    fxStatus.style.whiteSpace = 'normal'; fxStatus.style.marginTop = '3px'; fxStatus.visible = false;

    var st = null;
    var api = {};
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      root.visible = !!st.enabled;
      if (!st.enabled) {
        inspector.visible = false;
        return;
      }
      inspector.visible = true;

      var cur = !!st.cursor;
      inspector.hittest = cur;
      mouseBtn.__lbl.text = cur ? 'MOUSE: UI  (G)' : 'MOUSE: GAME  (G)';
      mouseBtn.style.backgroundColor = cur ? S.btnOn : S.btnBg;
      mouseBtn.__lbl.style.color = cur ? S.accent : S.label;

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
    };
    $.TestHud = api;
    $.Msg('[testhud] panel built.\n');
  } catch (e) {
    $.Msg('[testhud] build error: ' + e + '\n');
  }
})();
)CFJS";

} // namespace Filmmaker
