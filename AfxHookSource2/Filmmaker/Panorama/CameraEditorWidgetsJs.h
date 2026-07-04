#pragma once

// Shared UI widget builders (mk/lbl/btn/section/row) and the custom dropdown control.
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
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
      b.style.border = '1px solid ' + S.cardBorder;
      b.style.paddingTop = '6px'; b.style.paddingBottom = '6px';
      b.style.paddingLeft = '11px'; b.style.paddingRight = '11px';
      b.style.marginRight = '5px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, 14); l.style.fontWeight = 'bold';
      b.SetPanelEvent('onactivate', onClick);
      b.__lbl = l; return b;
    }
    // Button hierarchy on top of btn() (which stays the SECONDARY default):
    //   btnPrimary  = gold fill + dark text -- at most ONE per row; "the thing to click".
    //   btnDanger   = red-tinted fill + red border/text for destructive actions.
    function btnPrimary(parent, text, onClick) {
      var b = btn(parent, text, onClick, S.accentText);
      b.style.backgroundColor = S.accent; b.style.border = '1px solid ' + S.accent;
      return b;
    }
    function btnDanger(parent, text, onClick) {
      var b = btn(parent, text, onClick, S.dangerText);
      b.style.backgroundColor = S.dangerBg; b.style.border = '1px solid ' + S.dangerBorder;
      return b;
    }
    // Tab treatment: active = raised fill + gold underline + gold text (NOT a full gold pill,
    // so a solid gold fill stays reserved for the one primary action in a row). tabify() at
    // build time, styleTab(b, on) each render.
    function tabify(b) {
      b.style.border = '1px solid ' + S.clear;
      b.style.paddingTop = '8px'; b.style.paddingBottom = '7px';
      b.__lbl.style.horizontalAlign = 'center';
      return b;
    }
    function styleTab(b, on) {
      b.style.backgroundColor = on ? S.bgRaised : S.clear;
      b.style.borderBottom = '2px solid ' + (on ? S.accent : S.clear);
      b.__lbl.style.color = on ? S.accent : S.label;
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
    // Layered slider: rounded track + a colored fill bar drawn UNDER a real native Slider.
    // The native Slider stays the drag mechanism (this HUD context has no mouse-move event,
    // so a hand-rolled draggable control is impossible); we only upgrade the visual layer.
    // Returns { wrap, sl, setFill(frac 0..1) }.
    function makeSlider(parent, height, fillColor) {
      var wrap = mk('Panel', parent);
      wrap.style.width = 'fill-parent-flow(1.0)'; wrap.style.height = (height || 16) + 'px';
      wrap.style.verticalAlign = 'center';
      var track = mk('Panel', wrap); track.hittest = false;
      track.style.width = '100%'; track.style.height = '4px'; track.style.borderRadius = '2px';
      track.style.verticalAlign = 'center'; track.style.backgroundColor = S.trackBg;
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
      // hittest=false explicitly: `pop` is created with hittest=true ONCE and this embedded HUD
      // Panorama context has already shown it does not reliably exclude invisible panels from hit
      // testing (see the "native DropDown never instantiates" note above) -- leaving hittest=true
      // on a merely-invisible popup risks it silently eating clicks at its last on-screen rect for
      // whatever real control happens to sit there next (reported: dropdowns stop opening entirely
      // after closing a previous one).
      openDrop.pop.visible = false; openDrop.pop.hittest = false; openDrop.caret.text = '▾'; openDrop = null;
    }
    // Bar styling: distinct fill + border so it reads as clickable. 'empty' = dim/disabled.
    function fieldStyle(field, empty) {
      field.style.height = '30px'; field.style.borderRadius = '4px';
      field.style.backgroundColor = empty ? '#11151b' : '#1b2230';
      field.style.border = '1px solid ' + (empty ? '#ffffff12' : '#ffffff2e');
    }
    // Place the popup directly under its field and show it. dbgAbs() (a hand-rolled walk of
    // actualxoffset/actualyoffset up to ctx) does NOT account for an ancestor's SCROLL position --
    // it gave the field's pre-scroll offset, so a popup opened after scrolling the Customize modal's
    // right-hand column rendered detached from the field. GetPositionWithinWindow() is CS2's own
    // native API for exactly this (native scripts use it to follow a scrolled/moving element, e.g.
    // reference/ scripts/common/add_major_tokens_anim.js, scripts/operation/operation_main.js) --
    // it resolves screen position correctly regardless of ancestor scrolling. Same physical-px /
    // uiscale convention as dbgAbs. The popup is a child of root (full-screen at 0,0), so
    // window-relative coords == root-relative.
    function showDropPopup(rec) {
      var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1, sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var a = rec.field.GetPositionWithinWindow ? rec.field.GetPositionWithinWindow() : dbgAbs(rec.field);
      var x = a.x / sx, y = a.y / sy;
      var w = (rec.field.actuallayoutwidth || 0) / sx, h = (rec.field.actuallayoutheight || 0) / sy;
      rec.pop.style.position = Math.round(x) + 'px ' + Math.round(y + h + 2) + 'px 0px';
      rec.pop.style.width = Math.round(w) + 'px';
      rec.pop.visible = true; rec.pop.hittest = true; rec.caret.text = '▴'; openDrop = rec;
    }
    function toggleDrop(rec) {
      var wasOpen = (openDrop === rec);
      closeAllDrops();
      if (!wasOpen && rec.opts.length) showDropPopup(rec);
    }
    // Popups are positioned ONCE, at open time, from the field's current on-screen offset -- but a
    // field inside a scrolling container (e.g. the Customize modal's right-hand column) keeps moving
    // after that: dragging the scrollbar with a popup open (or opening one right after a scroll
    // settles) left the popup anchored to the field's PRE-scroll position, visibly detached from the
    // bar it belongs to. Re-run the same placement math every frame the popup is open (called from
    // render(), which already runs every frame) so it tracks the field precisely, including mid-drag.
    function repositionOpenDrop() {
      if (openDrop && openDrop.pop && openDrop.pop.visible) showDropPopup(openDrop);
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
