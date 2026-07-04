#pragma once

// MODIFIERS section of the CONFIG panel: camera-feel intensity sliders (strafe roll /
// view bob / weapon sway / deadzone aim -> ViewFx) + the Body Cam / Action Cam / fisheye
// preset buttons. JS fragment concatenated (via #include, as an adjacent raw string
// literal) into the kConfigHudJs script body assembled in ConfigHudJs.h. Not a standalone
// script -- shares scope/closures with the root IIFE (S, row, lbl, btn, makeSlider,
// section, cmd, clamp01, st). See ConfigHudJs.h for the entry point.

R"CFJS(
    // ===================== MODIFIERS ====================================
    // Camera "feel" tuning (ViewFx.h / BodyCam.h -- neither is Camera Editor / Follow-camera
    // UI; these are display/feel modifiers, so they live here). Roll/Sway are each a plain
    // 0-150% intensity slider (0 = off) instead of a fixed set of steps, so they can be
    // dialed up or down like the Follow inspector's offset/rotation sliders -- built locally
    // with the same makeSlider recipe so ConfigHudJs stays standalone (no
    // CameraEditorWidgetsJs import).
    var modSec = section(inspector, 'MODIFIERS');
    var MAX_FX_PCT = 150;
    function intensityRow(parent, label, cmdPrefix, hint) {
      var head = row(parent); head.style.marginTop = '2px';
      var nl = lbl(head, label, S.value, 13); nl.style.fontWeight = 'bold';
      nl.style.width = 'fill-parent-flow(1.0)'; nl.style.verticalAlign = 'center';
      var vl = lbl(head, 'Off', S.accent, 12); vl.style.width = '46px'; vl.style.textAlign = 'right';
      vl.style.verticalAlign = 'center'; vl.style.fontWeight = 'bold';
      var sliderRow = row(parent); sliderRow.style.marginTop = '4px';
      var ms = makeSlider(sliderRow, 16, S.accent);
      if (hint) {
        var h = lbl(parent, hint, S.dim, 10);
        h.style.whiteSpace = 'normal'; h.style.marginTop = '5px'; h.style.marginBottom = '10px';
      }
      // Feedback-loop guards. Panorama can deliver SliderValueChanged for PROGRAMMATIC value
      // writes too (and after the writing scope has already returned), so a plain re-entrancy
      // flag around `sl.value = ...` is not enough: render() synced the value each frame ->
      // deferred event -> console command -> print -> state re-render -> sync -> ... = one
      // "mirv_filmmaker: ..." console line EVERY FRAME (the console-spam lag). Two value-based
      // guards break the cycle no matter how the events are scheduled:
      //   * the handler only issues a command when the PCT actually changed vs the last one
      //     sent/synced (lastPct), and
      //   * sync() only writes sl.value when it differs beyond one slider quantum.
      var lastPct = -1;
      $.RegisterEventHandler('SliderValueChanged', ms.sl, function (panel, v) {
        var frac = clamp01(v);
        var pct = Math.round(frac * MAX_FX_PCT);
        if (pct === lastPct) return;
        lastPct = pct;
        ms.setFill(frac);
        vl.text = pct > 0 ? (pct + '%') : 'Off';
        cmd(cmdPrefix + ' ' + pct + ' quiet');
      });
      return {
        sync: function (pct) {
          pct = Math.round(pct || 0);
          var frac = clamp01(pct / MAX_FX_PCT);
          if (pct !== lastPct) {
            lastPct = pct;
            if (Math.abs((ms.sl.value || 0) - frac) > 0.5 / MAX_FX_PCT) ms.sl.value = frac;
          }
          ms.setFill(frac);
          vl.text = pct > 0 ? (pct + '%') : 'Off';
        }
      };
    }
    var rollCtl = intensityRow(modSec, 'Strafe Roll', 'mirv_filmmaker viewfx roll',
      'Quake/Doom-style camera tilt on strafe. Only applies in plain spectate -- off during free cam, camera path, or Body Cam.');
    var bobCtl = intensityRow(modSec, 'View Bob', 'mirv_filmmaker viewfx bob',
      'GoldSrc-style vertical camera bob on the walk cycle. Only applies in plain spectate -- off during free cam, camera path, or Body Cam.');
    var swayCtl = intensityRow(modSec, 'Weapon Sway', 'mirv_filmmaker viewfx sway',
      'Movement-scaled weapon sway + walk bob on the viewmodel. Still players hold perfectly steady.');
    var deadzoneCtl = intensityRow(modSec, 'Deadzone Aim', 'mirv_filmmaker viewfx deadzone',
      'Decoupled viewmodel: the weapon leads inside an aim deadzone while the camera catches up with smoothing.');
    var bodyCamBtn = btn(modSec, 'Body Cam: Off', function () { cmd('mirv_filmmaker bodycam toggle'); }, S.value);
    bodyCamBtn.style.width = '100%'; bodyCamBtn.style.marginRight = '0px'; bodyCamBtn.style.marginTop = '0px';
    bodyCamBtn.__lbl.style.horizontalAlign = 'center';
    var bodyCamHint = lbl(modSec, 'Chest-mounted camera on the spectated player (Attach + Follow system). Needs a player POV to engage.', S.dim, 10);
    bodyCamHint.style.whiteSpace = 'normal'; bodyCamHint.style.marginTop = '4px';
    var actionCamBtn = btn(modSec, 'Action Cam: Off', function () { cmd('mirv_filmmaker actioncam toggle'); }, S.value);
    actionCamBtn.style.width = '100%'; actionCamBtn.style.marginRight = '0px'; actionCamBtn.style.marginTop = '8px';
    actionCamBtn.__lbl.style.horizontalAlign = 'center';
    var acFisheyeBtn = btn(modSec, 'Fisheye Lens: On', function () { cmd('mirv_filmmaker actioncam fisheye toggle'); }, S.label);
    acFisheyeBtn.style.width = '100%'; acFisheyeBtn.style.marginRight = '0px'; acFisheyeBtn.style.marginTop = '4px';
    acFisheyeBtn.__lbl.style.horizontalAlign = 'center';
    var actionCamHint = lbl(modSec, 'Head-mounted GoPro-style camera on the spectated player (wide FOV, smoothed handheld angles). The fisheye lens applies while it is on.', S.dim, 10);
    actionCamHint.style.whiteSpace = 'normal'; actionCamHint.style.marginTop = '4px';
)CFJS"
