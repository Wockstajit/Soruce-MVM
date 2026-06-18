#pragma once

// Panorama JS for the BO2-inspired camera-marker UI. Built ONCE into the HUD
// (CSGOHud) context by MarkerHud.cpp. It renders one of two things depending on
// the camera-path mode pushed in the "state" JSON:
//   * a centred SETTINGS CARD (hit-testable) while editing with the menu open;
//   * a centred PROMPT BANNER (click-through) during reposition / preview.
//   C++ -> JS : sets host attribute "state" (JSON) then runs $.MarkerHud.render().
//   JS  -> C++: buttons issue console commands (GameInterfaceAPI.ConsoleCommand
//               'mirv_filmmaker marker ...'), the same back-end the hotkeys use.
//
// Visual language matches the project's Movie Director HUD (dark translucent
// panel, CS gold accent, blue for Freeze). The root is full-screen + click-through;
// only the settings card is hit-testable so it never blocks demo/game input.

namespace Filmmaker {

inline const char* kMarkerHudJs = R"MKJS(
(function () {
  try {
    var existing = $('#MarkerHudRoot'); if (existing) existing.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      cardW: '360px',
      bg: 'rgba(0,0,0,0.92)',
      accent: '#f0b323ff',
      freeze: '#4aa3ffff',
      label: '#9aa4b0ff',
      value: '#eef2f6ff',
      btnBg: '#ffffff14',
      sep: '#80808030',
      font: 'Stratum2, "Arial Unicode MS"'
    };

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[markerhud] cmd failed: ' + e + '\n'); }
    }

    // Full-screen, click-through container (only the settings card is hittable).
    var root = $.CreatePanel('Panel', ctx, 'MarkerHudRoot', {});
    root.hittest = false;
    root.style.width = '100%';
    root.style.height = '100%';
    root.style.zIndex = '60';

    // Full-screen hit-test CATCHER, shown only while the settings card is open. It sits
    // BEHIND the card (created first) and absorbs any click that misses the card, so CS2's
    // spectator can't switch players out from under the editor. The card's buttons are
    // later siblings, so they still receive their own clicks.
    var catcher = $.CreatePanel('Panel', root, '', {});
    catcher.hittest = true;
    catcher.style.width = '100%';
    catcher.style.height = '100%';
    catcher.visible = false;
    catcher.SetPanelEvent('onactivate', function () { /* swallow */ });

    // ============================ SETTINGS CARD ============================
    var card = $.CreatePanel('Panel', root, '', {});
    card.hittest = true;
    var cs = card.style;
    cs.width = S.cardW;
    cs.horizontalAlign = 'center';
    cs.verticalAlign = 'center';
    cs.backgroundColor = S.bg;
    cs.borderRadius = '6px';
    cs.flowChildren = 'down';
    cs.color = S.label;
    cs.fontFamily = S.font;
    cs.fontSize = '15px';

    var header = $.CreatePanel('Panel', card, '', {});
    header.style.flowChildren = 'right';
    header.style.width = '100%';
    header.style.paddingTop = '12px'; header.style.paddingBottom = '12px';
    header.style.paddingLeft = '14px'; header.style.paddingRight = '14px';

    function mkArrow(parent, glyph, command) {
      var a = $.CreatePanel('Label', parent, '', {});
      a.text = glyph; a.hittest = true;
      a.style.color = S.accent; a.style.fontSize = '22px'; a.style.fontWeight = 'bold';
      a.style.width = '34px'; a.style.textAlign = 'center'; a.style.verticalAlign = 'center';
      a.SetPanelEvent('onactivate', function () { cmd(command); });
      return a;
    }
    mkArrow(header, '◀', 'mirv_filmmaker marker prev');
    var title = $.CreatePanel('Label', header, '', {});
    title.text = 'CAMERA MARKER';
    title.style.color = S.value; title.style.fontSize = '17px'; title.style.fontWeight = 'bold';
    title.style.letterSpacing = '2px'; title.style.width = 'fill-parent-flow(1.0)';
    title.style.textAlign = 'center'; title.style.verticalAlign = 'center';
    mkArrow(header, '▶', 'mirv_filmmaker marker next');

    function mkSep(parent) {
      var p = $.CreatePanel('Panel', parent, '', {});
      p.style.width = '100%'; p.style.height = '1px'; p.style.backgroundColor = S.sep;
    }
    mkSep(card);

    function mkSection(parent) {
      var p = $.CreatePanel('Panel', parent, '', {});
      p.style.flowChildren = 'down'; p.style.width = '100%';
      p.style.paddingTop = '8px'; p.style.paddingBottom = '8px';
      p.style.paddingLeft = '14px'; p.style.paddingRight = '14px';
      return p;
    }

    function mkSelector(parent, labelText, command) {
      var row = $.CreatePanel('Panel', parent, '', {});
      row.style.flowChildren = 'right'; row.style.width = '100%';
      row.style.marginBottom = '6px'; row.style.minHeight = '26px';
      var k = $.CreatePanel('Label', row, '', {});
      k.text = labelText; k.style.color = S.label;
      k.style.width = 'fill-parent-flow(1.0)'; k.style.verticalAlign = 'center';
      var btn = $.CreatePanel('Panel', row, '', {});
      btn.hittest = true; btn.style.backgroundColor = S.btnBg; btn.style.borderRadius = '4px';
      btn.style.paddingTop = '4px'; btn.style.paddingBottom = '4px';
      btn.style.paddingLeft = '12px'; btn.style.paddingRight = '12px';
      btn.style.minWidth = '120px'; btn.style.verticalAlign = 'center';
      var v = $.CreatePanel('Label', btn, '', {});
      v.style.color = S.value; v.style.fontWeight = 'bold'; v.style.textAlign = 'center'; v.style.width = '100%';
      btn.SetPanelEvent('onactivate', function () { cmd(command); });
      return v;
    }
)MKJS"
R"MKJS(
    // Speed Mode and Path Mode are DELIBERATELY separate axes: Speed Mode decides
    // how long each segment takes; Path Mode (Linear/Bezier) decides the curve shape.
    var SPEED_STEPS = [0.2, 0.5, 0.8, 1.0];

    // A "− value +" stepper row that snaps the value to SPEED_STEPS and issues a
    // console command. getCur() reads the live value from the pushed state.
    function mkStepper(parent, labelText, getCur, cmdName) {
      var row = $.CreatePanel('Panel', parent, '', {});
      row.style.flowChildren = 'right'; row.style.width = '100%';
      row.style.marginBottom = '6px'; row.style.minHeight = '26px';
      var k = $.CreatePanel('Label', row, '', {});
      k.text = labelText; k.style.color = S.label;
      k.style.width = 'fill-parent-flow(1.0)'; k.style.verticalAlign = 'center';
      function step(dir) {
        var cur = getCur();
        var idx = 0, best = 1e9;
        for (var i = 0; i < SPEED_STEPS.length; i++) {
          var d = Math.abs(SPEED_STEPS[i] - cur);
          if (d < best) { best = d; idx = i; }
        }
        idx += (dir > 0 ? 1 : -1);
        if (idx < 0) idx = 0; if (idx >= SPEED_STEPS.length) idx = SPEED_STEPS.length - 1;
        cmd(cmdName + ' ' + SPEED_STEPS[idx].toFixed(2));
      }
      function mkB(glyph, dir) {
        var b = $.CreatePanel('Label', row, '', {});
        b.text = glyph; b.hittest = true;
        b.style.color = S.accent; b.style.fontWeight = 'bold'; b.style.fontSize = '18px';
        b.style.width = '26px'; b.style.textAlign = 'center'; b.style.verticalAlign = 'center';
        b.style.backgroundColor = S.btnBg; b.style.borderRadius = '4px';
        b.style.paddingTop = '2px'; b.style.paddingBottom = '2px';
        b.SetPanelEvent('onactivate', function () { step(dir); });
      }
      mkB('−', -1);
      var vlab = $.CreatePanel('Label', row, '', {});
      vlab.style.color = S.value; vlab.style.fontWeight = 'bold';
      vlab.style.width = '64px'; vlab.style.textAlign = 'center'; vlab.style.verticalAlign = 'center';
      mkB('+', +1);
      return { row: row, val: vlab };
    }

    var sec = mkSection(card);
    var vSpeedMode = mkSelector(sec, 'Speed Mode', 'mirv_filmmaker marker speedmode cycle');
    var vInterp = mkSelector(sec, 'Path Mode (curve)', 'mirv_filmmaker marker interp cycle');
    var vTiming = mkSelector(sec, 'Freeze / Live (all cams)', 'mirv_filmmaker marker timing toggle');
    var vAutoSnap = mkSelector(sec, 'Auto Snap To Selected Camera', 'mirv_filmmaker marker autosnap toggle');

    // Global Constant Speed (shown only when Speed Mode == Constant).
    var constStepper = mkStepper(sec, 'Constant Speed (whole path)',
      function () { return (st && st.constSpeed != null) ? st.constSpeed : 1.0; },
      'mirv_filmmaker marker constspeed');

    // Per-segment multiplier for this marker's OUTGOING segment (shown only when
    // Speed Mode == Per-Segment and this is NOT the last marker).
    var mulStepper = mkStepper(sec, 'Segment Speed (to next)',
      function () { return (st && st.speedMul != null) ? st.speedMul : 1.0; },
      'mirv_filmmaker marker speedmul');

    // Per-mode explanation / "no outgoing segment" note.
    var expl = $.CreatePanel('Label', sec, '', {});
    expl.style.color = S.label; expl.style.fontSize = '12px'; expl.style.marginTop = '2px';

    mkSep(card);

    function mkAction(parent, text, color, onClick) {
      var b = $.CreatePanel('Panel', parent, '', {});
      b.hittest = true; b.style.width = '100%';
      b.style.paddingTop = '9px'; b.style.paddingBottom = '9px';
      b.style.paddingLeft = '16px'; b.style.paddingRight = '16px';
      var l = $.CreatePanel('Label', b, '', {});
      l.text = text; l.style.color = color; l.style.fontWeight = 'bold'; l.style.fontSize = '15px';
      b.SetPanelEvent('onactivate', onClick);
      return l;
    }

    var asec = $.CreatePanel('Panel', card, '', {});
    asec.style.flowChildren = 'down'; asec.style.width = '100%';
    asec.style.paddingTop = '6px'; asec.style.paddingBottom = '8px';
    mkAction(asec, 'Reposition Camera', S.accent, function () { cmd('mirv_filmmaker marker reposition'); });
    mkAction(asec, 'Remove Marker', S.value, function () {
      if (st && st.selected >= 0) cmd('mirv_filmmaker marker delete ' + st.selected);
    });
    var delAllConfirm = false;
    var delAllLbl = mkAction(asec, 'Remove All Markers', S.value, function () {
      if (!delAllConfirm) { delAllConfirm = true; delAllLbl.text = 'Remove All - click again to confirm'; delAllLbl.style.color = S.accent; return; }
      delAllConfirm = false; delAllLbl.text = 'Remove All Markers'; delAllLbl.style.color = S.value;
      cmd('mirv_filmmaker marker deleteall confirm');
    });

    mkSep(card);

    // Control hints for normal editing.
    var hints = $.CreatePanel('Label', card, '', {});
    hints.style.color = S.label; hints.style.fontSize = '12px';
    hints.style.paddingTop = '8px'; hints.style.paddingLeft = '14px';
    hints.style.paddingRight = '14px'; hints.style.paddingBottom = '4px';
    hints.text = 'K Place   ·   J Arm → Space Play   ·   L Delete (aim)   ·   F Settings (aim)';

    var foot = $.CreatePanel('Panel', card, '', {});
    foot.hittest = true; foot.style.flowChildren = 'right'; foot.style.width = '100%';
    foot.style.paddingTop = '6px'; foot.style.paddingBottom = '10px';
    foot.style.paddingLeft = '14px'; foot.style.paddingRight = '14px';
    var doneL = $.CreatePanel('Label', foot, '', {});
    doneL.text = 'F / ESC  Done'; doneL.style.color = S.accent; doneL.style.fontWeight = 'bold'; doneL.style.letterSpacing = '1px';
    foot.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker marker close'); });

    // ============================ PROMPT BANNER ============================
    // Click-through (hittest=false) so the user can free-look + left-click the
    // world during reposition, and so it never blocks the preview.
    var banner = $.CreatePanel('Panel', root, '', {});
    banner.hittest = false;
    var bs = banner.style;
    bs.flowChildren = 'down'; bs.horizontalAlign = 'center'; bs.verticalAlign = 'top';
    // Keep the preview control clear of the upper HUD and spectator labels.
    bs.marginTop = '160px'; bs.minWidth = '294px';
    bs.backgroundColor = 'rgba(0,0,0,0.78)'; bs.borderRadius = '5px';
    bs.paddingTop = '8px'; bs.paddingBottom = '8px';
    bs.paddingLeft = '17px'; bs.paddingRight = '17px';
    bs.fontFamily = S.font;
    var bTitle = $.CreatePanel('Label', banner, '', {});
    bTitle.style.color = S.accent; bTitle.style.fontSize = '13px'; bTitle.style.fontWeight = 'bold';
    bTitle.style.letterSpacing = '1px'; bTitle.style.horizontalAlign = 'center';
    var bSub = $.CreatePanel('Label', banner, '', {});
    bSub.style.color = S.value; bSub.style.fontSize = '11px'; bSub.style.horizontalAlign = 'center';
    bSub.style.marginTop = '4px';
    var bKeys = $.CreatePanel('Label', banner, '', {});
    bKeys.style.color = S.label; bKeys.style.fontSize = '10px'; bKeys.style.horizontalAlign = 'center';
    bKeys.style.marginTop = '5px';

    function showBanner(titleText, titleColor, subText, keysText) {
      bTitle.text = titleText; bTitle.style.color = titleColor;
      bSub.text = subText; bKeys.text = keysText;
      banner.visible = true; card.visible = false; catcher.visible = false;
    }
)MKJS"
R"MKJS(
    var st = null;
    var hostAttr = root;

    // Hide sibling HUD panels while preserving this preview root. During armed preview,
    // Tab toggles this so the Cam Path Preview control remains available. During playback,
    // siblings stay hidden and this root hides too, producing a completely clean frame.
    var hiddenPanels = null;
    function setOtherUiHidden(h) {
      if (h) {
        if (!hiddenPanels) hiddenPanels = [];
        var count = ctx.GetChildCount ? ctx.GetChildCount() : 0;
        for (var i = 0; i < count; i++) {
          var p = ctx.GetChild(i);
          if (!p || p === root || p.id === 'MarkerHudRoot') continue;
          if (p.visible) {
            hiddenPanels.push(p);
            try { p.visible = false; } catch (e) {}
          }
        }
      } else if (hiddenPanels) {
        for (var j = 0; j < hiddenPanels.length; j++) {
          try { hiddenPanels[j].visible = true; } catch (e2) {}
        }
        hiddenPanels = null;
      }
    }

    var api = {};
    api.render = function () {
      var raw = hostAttr.GetAttributeString('state', '');
      if (!raw) { root.visible = false; setOtherUiHidden(false); return; }
      var s; try { s = JSON.parse(raw); } catch (e) { return; }
      st = s;

      var mode = s.mode || 'editing';
      var freeze = (s.timing === 'Freeze');
      setOtherUiHidden(((mode === 'previewPlaying' && !s.editorPlay) || mode === 'previewArmed')
        && !!s.hudHidden);

      // Transient notice (e.g. "Need at least 2 camera markers to play path.").
      // Shown over everything EXCEPT the open settings card (which has its own
      // inline explanation), so pressing J with too few markers gives clear feedback.
      if (s.notice && !(mode === 'editing' && s.menuOpen)) {
        showBanner('CAMERA PATH', S.accent, s.notice, '');
        root.visible = true; return;
      }

      if (mode === 'reposition') {
        var n = (s.selected != null && s.selected >= 0) ? (s.selected + 1) : 0;
        showBanner('REPOSITIONING CAMERA #' + n, S.accent,
          'Move to the new position and left-click to place.',
          'Left Click: Place      X / Esc: Cancel');
        root.visible = true; return;
      }
      if (mode === 'previewArmed') {
        root.visible = false; return;
      }
      if (mode === 'previewPlaying') {
        root.visible = false; return;
      }

      // editing
      if (!s.menuOpen) { root.visible = false; return; }
      banner.visible = false; card.visible = true; catcher.visible = true; root.visible = true;

      var nn = (s.selected != null && s.selected >= 0) ? (s.selected + 1) : 0;
      title.text = 'CAMERA MARKER #' + nn + '  (' + s.count + ' total)';
      vSpeedMode.text = s.speedMode || '-';
      vInterp.text = s.interp || '-';
      vTiming.text = freeze ? 'FREEZE' : 'LIVE';
      vTiming.style.color = freeze ? S.freeze : S.accent;
      vAutoSnap.text = s.autoSnap ? 'ON' : 'OFF';
      vAutoSnap.style.color = s.autoSnap ? S.accent : S.label;
      constStepper.val.text = ((s.constSpeed != null) ? s.constSpeed : 1).toFixed(1) + 'x';
      mulStepper.val.text = ((s.speedMul != null) ? s.speedMul : 1).toFixed(1) + 'x';

      // Path interpolation and speed are independent; the speed controls shown
      // depend ONLY on the speed mode (not the path mode).
      var sm = s.speedMode || 'Manual';
      constStepper.row.visible = (sm === 'Constant');
      var perSeg = (sm === 'Per-Segment');
      mulStepper.row.visible = (perSeg && !s.isLast);

      if (!freeze) {
        expl.text = 'LIVE: camera follows the replay — markers are hit at their tick/time. Use demo_timescale for speed. Speed modes below shape FREEZE playback.';
      } else if (sm === 'Manual') {
        expl.text = 'FREEZE · Manual: each segment’s duration = the tick/time gap to the next marker.';
      } else if (sm === 'Constant') {
        expl.text = 'FREEZE · Constant: the whole path glides at one shared speed.';
      } else if (s.isLast) {
        expl.text = 'No outgoing segment — the last marker ends the path.';
      } else {
        expl.text = 'FREEZE · Per-Segment: this marker controls the move to the next marker.';
      }

      delAllConfirm = false; delAllLbl.text = 'Remove All Markers'; delAllLbl.style.color = S.value;
    };
    $.MarkerHud = api;
    api.render();
    $.Msg('[markerhud] panel built.\n');
  } catch (err) {
    $.Msg('[markerhud] gui error: ' + err + '\n');
  }
})();
)MKJS";

} // namespace Filmmaker
