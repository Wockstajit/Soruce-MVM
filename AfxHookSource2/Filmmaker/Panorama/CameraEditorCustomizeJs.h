#pragma once

// CUSTOMIZE (LOADOUT) MODAL: cosmetic selection, wear controls, 3D preview wiring.
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
    // ===================== CUSTOMIZE (LOADOUT) MODAL ====================
    // A CS2-loadout-style modal for re-skinning the spectated player in the demo view. Only
    // reachable while spectating a player (obsMode 2/3); never in freecam. The dropdowns are
    // driven by a cosmetics dataset (Phase 2 replaces this stub with data generated from the
    // CS2 game files). Selecting an item updates the modal preview now; the per-frame econ/model
    // override that changes the actual demo render is wired in via pickCosmetic() once the native
    // "mirv_filmmaker cosmetics ..." back-end lands (Phases 3-4).

    // Rarity tiers -> colors, matching CS2's csgostyles.css (color-rarity-0..7).
    var RARITY = {
      0: { n: 'Default',    c: '#6a6156ff' },
      1: { n: 'Consumer',   c: '#b0c3d9ff' },
      2: { n: 'Industrial', c: '#5e98d9ff' },
      3: { n: 'Mil-Spec',   c: '#4b69ffff' },
      4: { n: 'Restricted', c: '#8847ffff' },
      5: { n: 'Classified', c: '#d32ce6ff' },
      6: { n: 'Covert',     c: '#eb4b4bff' },
      7: { n: 'Exceedingly Rare', c: '#e4ae39ff' }
    };
    function rarColor(r) { return (RARITY[r] || RARITY[0]).c; }
    function makeFauxItemId(defIndex, paintKitId) {
      try { return (BigInt('0xF000000000000000') | (BigInt(paintKitId || 0) << BigInt(16)) | BigInt(defIndex || 0)); }
      catch (e) { return null; }
    }
    function safeCall(panel, method) {
      if (!panel || !panel.IsValid || !panel.IsValid() || typeof panel[method] !== 'function') return null;
      try {
        var args = [];
        for (var i = 2; i < arguments.length; i++) args.push(arguments[i]);
        return panel[method].apply(panel, args);
      } catch (e) { return null; }
    }
    function fauxFromMeta(meta) {
      if (!meta || !meta.def) return null;
      return makeFauxItemId(meta.def, meta.paint || meta.paintKit || 0);
    }
    // Canonical econ item id (string) for a catalog meta, built by CS2's own econ system so an
    // <ItemImage> panel can render the REAL inventory thumbnail in the in-game HUD context (this is
    // exactly what avatar.js / itemtile.js do). Returns '' when the item has no real def index
    // (e.g. "default" rows) so the caller falls back to a rarity swatch.
    function econItemId(meta) {
      if (!meta) return '';
      var def = parseInt(meta.def || 0, 10) || 0;
      var paint = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
      if (def <= 0) return '';
      try {
        if (typeof InventoryAPI !== 'undefined' && InventoryAPI.GetFauxItemIDFromDefAndPaintIndex) {
          var id = InventoryAPI.GetFauxItemIDFromDefAndPaintIndex(def, paint);
          if (id && id !== '0') return id;
        }
      } catch (e) {}
      return '';
    }
    // Item id for MapPlayerPreviewPanel equips. Prefer CS2's own faux-id builder, but keep the
    // local high-bit faux-id fallback so staged catalog picks still drive the preview if the
    // InventoryAPI helper is unavailable in the HUD context.
    function previewEquipItemId(meta) {
      var id = econItemId(meta);
      if (id) return '' + id;
      var faux = fauxFromMeta(meta);
      return faux ? ('' + faux) : '';
    }

    // Stub dataset: { key:value, ... } -> [label, value, {color, img}] options. Phase 2 swaps
    // this for a real JSON catalog generated from items_game.txt/csgo_english.txt. The shape is
    // already slot-based so the UI and commands match the final player-targeted API.
    var COSMETICS = {
      agent:  [['Default agent', 'default', { color: '#e6eaef', team: 'ANY' }],
               ['CT | SAS', 'ctm_sas', { color: rarColor(3), team: 'CT', def: 0, model: 'agents/models/ctm_sas/ctm_sas.vmdl' }],
               ['CT | FBI', 'ctm_fbi', { color: rarColor(4), team: 'CT', def: 0, model: 'agents/models/ctm_fbi/ctm_fbi.vmdl' }],
               ['T | Anarchist', 'tm_anarchist', { color: rarColor(3), team: 'T', def: 0, model: 'agents/models/tm_anarchist/tm_anarchist.vmdl' }],
               ['T | Balkan', 'tm_balkan', { color: rarColor(4), team: 'T', def: 0, model: 'agents/models/tm_balkan/tm_balkan_varianta.vmdl' }]],
      primary:[['Default rifle skin', '0', { color: rarColor(0), def: 7, paint: 0 }],
               ['AK-47 | Redline', '282', { color: rarColor(4), def: 7, paint: 282, wearMin: 0.10, wearMax: 0.70 }],
               ['AK-47 | Asiimov', '801', { color: rarColor(5), def: 7, paint: 801, wearMin: 0.05, wearMax: 0.70 }],
               ['AWP | Asiimov', '279', { color: rarColor(6), def: 9, paint: 279, wearMin: 0.18, wearMax: 1.00 }],
               ['AWP | Dragon Lore', '344', { color: rarColor(6), def: 9, paint: 344, wearMin: 0.00, wearMax: 0.70 }],
               ['M4A4 | Howl', '309', { color: rarColor(7), def: 16, paint: 309, wearMin: 0.00, wearMax: 0.40 }]],
      secondary:[['Default pistol skin', '0', { color: rarColor(0), def: 1, paint: 0 }],
               ['Desert Eagle | Blaze', '37', { color: rarColor(5), def: 1, paint: 37, wearMin: 0.00, wearMax: 0.08 }],
               ['USP-S | Kill Confirmed', '504', { color: rarColor(6), def: 61, paint: 504, wearMin: 0.00, wearMax: 1.00 }],
               ['Glock-18 | Fade', '38', { color: rarColor(5), def: 4, paint: 38, wearMin: 0.00, wearMax: 0.08 }]],
      knife:  [['Default CT knife', '42:0', { color: rarColor(0), def: 42, paint: 0, team: 'CT' }],
               ['Default T knife', '59:0', { color: rarColor(0), def: 59, paint: 0, team: 'T' }],
               ['Karambit | Doppler', '507:417', { color: rarColor(7), def: 507, paint: 417, wearMin: 0.00, wearMax: 0.08 }],
               ['Butterfly Knife | Fade', '515:38', { color: rarColor(7), def: 515, paint: 38, wearMin: 0.00, wearMax: 0.08 }],
               ['M9 Bayonet | Marble Fade', '508:413', { color: rarColor(7), def: 508, paint: 413, wearMin: 0.00, wearMax: 0.08 }]],
      gloves: [['Default CT Gloves', '5029:0', { color: rarColor(0), def: 5029, paint: 0, team: 'CT' }],
               ['Default T Gloves', '5028:0', { color: rarColor(0), def: 5028, paint: 0, team: 'T' }],
               ['Sport Gloves | Pandora Box', '5030:10037', { color: rarColor(7), def: 5030, paint: 10037, wearMin: 0.06, wearMax: 0.80 }],
               ['Specialist Gloves | Crimson Kimono', '5034:10033', { color: rarColor(7), def: 5034, paint: 10033, wearMin: 0.06, wearMax: 0.80 }],
               ['Moto Gloves | Spearmint', '5033:10026', { color: rarColor(7), def: 5033, paint: 10026, wearMin: 0.06, wearMax: 0.80 }]]
    };
)EDJS"
#include "CameraEditorCosmeticsCatalog.inc"
R"EDJS(
    // Current selections + the spectated player this loadout belongs to.
    var custSel = { agent: 'default', primary: '0', secondary: '0', knife: '0', gloves: '0' };
    var WEAR_PRESETS = {
      fn: { label: 'Factory New', lo: 0.00, hi: 0.07, value: 0.01 },
      mw: { label: 'Minimal Wear', lo: 0.07, hi: 0.15, value: 0.10 },
      ft: { label: 'Field-Tested', lo: 0.15, hi: 0.38, value: 0.22 },
      ww: { label: 'Well-Worn', lo: 0.38, hi: 0.45, value: 0.40 },
      bs: { label: 'Battle-Scarred', lo: 0.45, hi: 1.00, value: 0.55 },
      custom: { label: 'Custom Float', lo: 0.00, hi: 1.00, value: 0.01 }
    };
    var WEAR_OPTIONS = [['Factory New', 'fn'], ['Minimal Wear', 'mw'], ['Field-Tested', 'ft'], ['Well-Worn', 'ww'], ['Battle-Scarred', 'bs'], ['Custom Float', 'custom']];
    var custWear = {
      primary: { preset: 'fn', custom: 0.01 },
      secondary: { preset: 'fn', custom: 0.01 },
      knife: { preset: 'fn', custom: 0.01 },
      gloves: { preset: 'ft', custom: 0.22 }
    };
    var custItemWear = {};
    var custWearUpdating = false;
    var custSearchDisplayGuard = false; // true while WE set a search TextEntry's .text (skip its change handler)
    var custColScroll = { off: 0 };     // right control-column manual scroll offset (px), driven by custWheel
    var CUST_DROP_VIEW_MAX = 300;       // max px height of a dropdown's scrollable row viewport
    var CUST_SCROLL_STEP = 64;          // px scrolled per wheel notch
    var custTargetIndex = -1, custTargetName = 'Player', custTargetTeam = '';
    var custTargetKey = 'pawn:-1', custActiveWeaponDef = 0, custActiveWeaponSlot = '', custDisplayedLoadoutSig = '';
    var custActiveWeaponPickup = false, custActiveWeaponOwnerSteam = '', custActiveWeaponOwnerName = '';
    var custLoadout = { primary: null, secondary: null, knife: null, gloves: null };
    // Which weapon slot the 3D preview currently poses the character HOLDING -- clicking into a
    // Primary/Secondary/Melee block's weapon/finish/wear controls switches this (see setActiveCategory
    // / the `category` itemDrop option below), mirroring the native loadout's selected-group behavior
    // (loadout_grid.js UpdateCharModel: whichever slot you're browsing is what the model holds).
    // Gloves are not a "held" item -- they render on the hands regardless of this value.
    var custActiveCategory = 'primary';
    function setActiveCategory(slot) {
      if (slot === custActiveCategory) return;
      custActiveCategory = slot;
      updatePreview();
    }
    // Which slot the preview should default to holding when a player is freshly loaded into the modal:
    // prefer whatever they're actually carrying, so the model isn't shown holding an empty slot.
    function initialActiveCategory() {
      if (custLoadout.primary && custLoadout.primary.defIndex > 0) return 'primary';
      if (custLoadout.secondary && custLoadout.secondary.defIndex > 0) return 'secondary';
      return 'knife';
    }
    // Which weapon def the primary/secondary blocks are CURRENTLY editing. 0 = follow the player's
    // held weapon; > 0 = an explicit weapon the user picked from the weapon selector (so a skin can be
    // assigned to ANY weapon, not just the one the player is holding). Each weapon def stores its own
    // skin in the SteamID-keyed backend, so a player can carry a distinct skin on every weapon at once.
    var custWeaponDef = { primary: 0, secondary: 0 };
    var WEAPON_SLOT_BY_DEF = {
      1:'secondary', 2:'secondary', 3:'secondary', 4:'secondary', 30:'secondary', 32:'secondary', 36:'secondary', 61:'secondary', 63:'secondary', 64:'secondary',
      7:'primary', 8:'primary', 9:'primary', 10:'primary', 11:'primary', 13:'primary', 14:'primary', 16:'primary', 17:'primary', 19:'primary', 23:'primary', 24:'primary', 25:'primary', 26:'primary', 27:'primary', 28:'primary', 29:'primary', 33:'primary', 34:'primary', 35:'primary', 38:'primary', 39:'primary', 60:'primary',
      42:'knife', 59:'knife', 500:'knife', 503:'knife', 505:'knife', 506:'knife', 507:'knife', 508:'knife', 509:'knife', 512:'knife', 514:'knife', 515:'knife', 516:'knife', 517:'knife', 518:'knife', 519:'knife', 520:'knife', 521:'knife', 522:'knife', 523:'knife', 525:'knife', 526:'knife'
    };
)EDJS"
R"EDJS(
    // Rich item dropdown: thumbnail/rarity swatch + rarity-colored name, in both the collapsed
    // field bar and the popup rows. Reuses the shared open/close/positioning machinery
    // (customDrops / openDrop / showDropPopup), so only one popup is open at a time.
    var ROW_LABEL_W = '96px';
    function rowLabelCell(parent, text) {
      var l = lbl(parent, text, S.dim, 12); l.hittest = false; l.style.width = ROW_LABEL_W;
      l.style.verticalAlign = 'center'; l.style.fontWeight = 'bold'; l.style.letterSpacing = '1px';
      return l;
    }
    // Rich item/finish/wear row: an optional left label cell ("Weapon"/"Finish"/"Wear"/"Gloves", per
    // the reference mock's labeled-card layout) plus either a swatch+name ('item' kind -- Agent,
    // Weapon, Finish, Gloves rows) or a value+meter bar ('wear' kind). Both open the SAME floating
    // popup (customDrops / openDrop / showDropPopup), so only one popup is open at a time. opts:
    //   rowLabel: string label cell text (omitted = no label cell, e.g. the top Agent row)
    //   kind: 'wear' for the value+meter row; anything else = swatch+name
    //   showWearTag: 'item' rows only -- split a baked-in " · Factory New" suffix (see
    //     withWearLabel()) into a separate dim trailing label instead of one run-on string
    //   searchable: false suppresses the popup search box (the 6-entry wear-preset list doesn't need it)
    //   height: field height override (the Agent row reads slightly taller in the reference)
    function itemDrop(parent, id, onPick, opts) {
      opts = opts || {};
      var kind = opts.kind || 'item';
      var field = mk('Panel', parent); field.id = id; field.hittest = true;
      field.style.width = '100%'; field.style.height = (opts.height || '48px'); field.style.flowChildren = 'right';
      field.style.verticalAlign = 'center'; field.style.borderRadius = '4px'; field.style.marginBottom = '8px';
      field.style.backgroundColor = '#1b2230'; field.style.border = '1px solid #ffffff2e';
      field.style.paddingLeft = '10px'; field.style.paddingRight = '10px';
      if (opts.rowLabel) rowLabelCell(field, opts.rowLabel);
      var swatch = null, img = null, disp = null, wearTag = null, meterTrack = null, meterFill = null, valueTxt = null;
      if (kind === 'wear') {
        valueTxt = lbl(field, '', S.value, 14); valueTxt.hittest = false; valueTxt.style.fontWeight = 'bold';
        valueTxt.style.width = 'fill-parent-flow(1.0)'; valueTxt.style.verticalAlign = 'center';
        meterTrack = mk('Panel', field); meterTrack.hittest = false; meterTrack.style.width = '120px'; meterTrack.style.height = '6px';
        meterTrack.style.verticalAlign = 'center'; meterTrack.style.marginRight = '12px'; meterTrack.style.borderRadius = '3px';
        meterTrack.style.backgroundColor = '#0c0f14'; meterTrack.style.border = '1px solid #ffffff1a'; meterTrack.style.overflow = 'clip';
        meterFill = mk('Panel', meterTrack); meterFill.hittest = false; meterFill.style.height = '100%';
        meterFill.style.width = '0%'; meterFill.style.backgroundColor = S.accent;
      } else {
        swatch = mk('Panel', field); swatch.hittest = false; swatch.style.width = '54px'; swatch.style.height = '38px';
        swatch.style.verticalAlign = 'center'; swatch.style.marginRight = '10px'; swatch.style.borderRadius = '3px';
        swatch.style.backgroundColor = '#0c0f14'; swatch.style.border = '1px solid #ffffff14';
        img = mk('Image', swatch); img.hittest = false; img.style.width = '100%'; img.style.height = '100%';
        try { img.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e) {}
        disp = lbl(field, '', '#e6eaef', 14); disp.hittest = false; disp.style.verticalAlign = 'center';
        disp.style.width = 'fill-parent-flow(1.0)'; disp.style.fontWeight = 'bold';
        disp.style.whiteSpace = 'nowrap'; disp.style.textOverflow = 'ellipsis';
        if (opts.showWearTag) {
          wearTag = lbl(field, '', S.dim, 12); wearTag.hittest = false; wearTag.style.verticalAlign = 'center';
          wearTag.style.marginLeft = '8px'; wearTag.style.whiteSpace = 'nowrap';
        }
      }
      var caret = lbl(field, '▾', '#e6eaef', 12); caret.hittest = false;
      caret.style.verticalAlign = 'center'; caret.style.marginLeft = '8px';
      var pop = mk('Panel', root); pop.visible = false; pop.hittest = true;
      pop.style.position = '0px 0px 0px'; pop.style.zIndex = '420'; pop.style.flowChildren = 'down';
      pop.style.backgroundColor = '#0e1620fa'; pop.style.border = '1px solid ' + S.accent;
      // overflow: clip (NOT native scroll). The wheel is consumed in C++ and routed here, so the row
      // list scrolls MANUALLY -- rec.rows is translated inside rec.rowsView (a clipped viewport), sized
      // every frame by sizeOpenDrop(). Native `overflow: scroll` wouldn't help: its scrollbar can't be
      // driven from a C++-forwarded notch, and the raw wheel is blocked before Panorama sees it.
      pop.style.overflow = 'clip';
      pop.style.borderRadius = '4px';
      pop.style.boxShadow = '#000000d0 0px 4px 14px 2px';
      var rec = {
        field: field, disp: disp, caret: caret, pop: pop, onPick: onPick, opts: [], sig: '', img: img, swatch: swatch,
        query: '', kind: kind, searchable: opts.searchable !== false, search: null, rows: null, rowsView: null, scrollOff: 0
      };
      field.SetPanelEvent('onactivate', function () { if (opts.category) setActiveCategory(opts.category); rec.scrollOff = 0; toggleDrop(rec); });
      customDrops.push(rec);
)EDJS"
R"EDJS(
      function panoramaImageSrc(img) {
        if (!img) return '';
        var rel = String(img).replace(/\\/g, '/');
        if (rel.indexOf('s2r://') === 0 || rel.indexOf('file://') === 0) return rel;
        if (rel.indexOf('panorama/images/') === 0) rel = rel.substring(16);
        if (rel.slice(-11) === '_png.vtex_c') rel = rel.substring(0, rel.length - 2);
        if (rel.slice(-9) === '_png.vtex') return 's2r://panorama/images/' + rel;
        if (rel.slice(-4).toLowerCase() === '.png') rel = rel.substring(0, rel.length - 4);
        return 's2r://panorama/images/' + rel + '_png.vtex';
      }

      function applySwatch(target, meta) {
        // 1) Direct Panorama image resource path. Catalog metadata stores image refs relative to
        //    panorama/images/ and verified against <img>_png.vtex_c in the CS2 VPK. Expand them to
        //    the native CS2 resource URL here so agents, weapons, knives, and gloves all use the
        //    same loading path.
        if (meta && meta.img) {
          if (target.__iimg) target.__iimg.visible = false;
          target.__im = target.__im || mk('Image', target);
          target.__im.style.width = '100%'; target.__im.style.height = '100%';
          try { target.__im.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e0) {}
          try { target.__im.SetImage(panoramaImageSrc(meta.img)); } catch (e1) {}
          target.__im.visible = true;
          target.style.backgroundColor = '#0c0f14';
          target.style.border = '1px solid ' + ((meta && meta.color) ? meta.color : '#ffffff14');
          return;
        }
        var itemId = econItemId(meta);
        if (itemId) {
          // Real CS2 econ thumbnail via a native <ItemImage> panel (renders in the HUD context).
          if (!target.__iimg) {
            try { target.__iimg = $.CreatePanel('ItemImage', target, '', {}); }
            catch (e) { target.__iimg = null; }
            if (target.__iimg) {
              target.__iimg.style.width = '100%'; target.__iimg.style.height = '100%';
              try { target.__iimg.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e2) {}
            }
          }
          if (target.__iimg) {
            try { target.__iimg.itemid = itemId; } catch (e3) {}
            target.__iimg.visible = true;
            if (target.__im) target.__im.visible = false;
            // Keep rarity readable as a thin edge, not a big block.
            target.style.backgroundColor = '#0c0f14';
            target.style.border = '1px solid ' + ((meta && meta.color) ? meta.color : '#ffffff14');
            return;
          }
        }
        if (target.__iimg) target.__iimg.visible = false;
        target.style.backgroundColor = (meta && meta.color) ? meta.color : '#0c0f14';
        target.style.border = '1px solid #ffffff14';
      }
      // One popup ROW. Kept separate so the keystroke path (rebuildRows) can refill the list
      // without touching the persistent search TextEntry.
      function buildRow(o) {
        if (rec.query && (o[0] || '').toLowerCase().indexOf(rec.query) < 0) return;
        var meta = o[2] || {};
        var prow = mk('Panel', rec.rows); prow.hittest = true; prow.style.width = '100%'; prow.style.flowChildren = 'right';
        prow.style.paddingTop = '5px'; prow.style.paddingBottom = '5px'; prow.style.paddingLeft = '6px'; prow.style.paddingRight = '10px';
        prow.style.verticalAlign = 'middle'; prow.style.borderLeft = '3px solid ' + (meta.color || '#00000000');
        if (meta.selected) prow.style.backgroundColor = S.btnOn;
        if (rec.kind !== 'wear') {
          var sw = mk('Panel', prow); sw.hittest = false; sw.style.width = '46px'; sw.style.height = '30px'; sw.style.marginRight = '9px';
          sw.style.verticalAlign = 'center'; sw.style.borderRadius = '3px'; sw.style.border = '1px solid #ffffff14';
          applySwatch(sw, meta);
        }
        var t = lbl(prow, o[0], meta.color || '#e6eaef', 14); t.hittest = false; t.style.verticalAlign = 'center';
        t.style.width = 'fill-parent-flow(1.0)'; t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.fontWeight = 'bold';
        prow.SetPanelEvent('onactivate', function () { closeAllDrops(); rec.onPick(o[1]); });
      }
      // Refill ONLY the rows container from the current query. Called on every keystroke, so it must
      // NOT rebuild the popup (that would destroy the focused search box). The search TextEntry is
      // left untouched, so focus + caret survive and typing flows continuously.
      function rebuildRows() {
        if (!rec.rows || !rec.rows.IsValid || !rec.rows.IsValid()) return;
        rec.rows.RemoveAndDeleteChildren();
        for (var i = 0; i < rec.opts.length; i++) buildRow(rec.opts[i]);
      }
      // FULL popup rebuild: lays out the persistent search box + an empty rows container ONCE, then
      // fills the rows. Runs only when the option SET changes (see update()'s sig, which no longer
      // includes rec.query) -- never per keystroke. The old code called this from ontextentrychange,
      // which RemoveAndDeleteChildren'd the search TextEntry mid-edit: focus was lost and every
      // character after the first was dropped, so the dropdown search was effectively unusable.
      rec.rebuildRows = rebuildRows; // exposed so the C++-routed typing path (custChars) can refilter
      function rebuild() {
        rec.pop.RemoveAndDeleteChildren();
        rec.search = null; rec.rows = null; rec.rowsView = null; rec.scrollOff = 0;
        if (rec.searchable) {
          // Display-only box: it shows the query, but the CHARACTERS come from custChars() (C++ WM_CHAR
          // route), not from Panorama keyboard focus. ontextentrychange is kept as a fallback for hosts
          // that DO deliver keys, guarded so our own programmatic .text writes don't double-process.
          var search = $.CreatePanel('TextEntry', rec.pop, id + 'Search', { placeholder: 'Search' });
          search.style.width = '100%'; search.style.height = '34px'; search.style.marginBottom = '4px';
          search.style.backgroundColor = '#101823'; search.style.border = '1px solid #ffffff24';
          search.style.color = S.value; search.style.fontSize = '14px'; search.style.paddingLeft = '8px';
          try { custSearchDisplayGuard = true; search.text = rec.query || ''; custSearchDisplayGuard = false; } catch (e) {}
          search.SetPanelEvent('ontextentrychange', function () {
            if (custSearchDisplayGuard) return;
            try { rec.query = (search.text || '').toLowerCase(); } catch (e) { rec.query = ''; }
            rebuildRows(); // refill rows only -- keep this TextEntry alive/focused
          });
          rec.search = search;
        }
        // Clipped viewport (height sized to content up to a cap by sizeOpenDrop) holding the row list,
        // which is transform-translated to scroll.
        rec.rowsView = mk('Panel', rec.pop); rec.rowsView.hittest = true;
        rec.rowsView.style.width = '100%'; rec.rowsView.style.overflow = 'clip'; rec.rowsView.style.flowChildren = 'down';
        rec.rows = mk('Panel', rec.rowsView); rec.rows.hittest = true;
        rec.rows.style.width = '100%'; rec.rows.style.flowChildren = 'down';
        rebuildRows();
      }
      return {
        dd: field,
        setSearch: function (q) { rec.query = (q || '').toLowerCase(); rebuild(); },
        open: function () { if (openDrop !== rec) closeAllDrops(); showDropPopup(rec); },
        update: function (optsArr, selValue) {
          rec.opts = optsArr || [];
          var sel = null;
          for (var d = 0; d < rec.opts.length; d++) {
            var m = rec.opts[d][2] || (rec.opts[d][2] = {});
            m.selected = (rec.opts[d][1] === selValue);
            if (m.selected) sel = rec.opts[d];
          }
          if (!sel && rec.opts.length) sel = rec.opts[0];
          var meta = sel ? (sel[2] || {}) : {};
          if (kind !== 'wear') {
            // Split a baked-in " · Factory New" suffix (withWearLabel()) off the option label so the
            // Finish row shows the skin name and the wear tag as two visually separate zones (per the
            // reference mock) instead of one run-on string.
            var fullLabel = sel ? sel[0] : '—', mainLabel = fullLabel, tagLabel = '';
            if (opts.showWearTag && fullLabel) {
              var sepIdx = fullLabel.lastIndexOf(' · ');
              if (sepIdx > 0) { mainLabel = fullLabel.substring(0, sepIdx); tagLabel = fullLabel.substring(sepIdx + 3); }
            }
            rec.disp.text = mainLabel; rec.disp.style.color = meta.color || '#e6eaef';
            if (wearTag) wearTag.text = tagLabel;
            rec.field.style.border = '1px solid ' + (meta.color || '#ffffff2e');
            applySwatch(rec.swatch, meta);
          }
          // The sig tracks the OPTION SET + selection only -- NOT rec.query. A query change filters
          // rows via rebuildRows() (from the search box) and must never force a full rebuild(), or it
          // would tear down the focused search TextEntry mid-typing.
          var sig = ''; for (var k = 0; k < rec.opts.length; k++) sig += rec.opts[k][0] + '|'; sig += '#' + selValue;
          if (sig !== rec.sig) { rec.sig = sig; rebuild(); }
        },
        // 'wear' kind only: sets the value text + meter fill directly (update() above only feeds the
        // popup's preset list for this kind, since there's no swatch/name to show in the field).
        updateWear: function (valueLabel, fraction, color) {
          if (kind !== 'wear') return;
          valueTxt.text = valueLabel;
          // Full-width smooth gradient: the skin's rarity color from the left up to the wear value,
          // blending into black for the remainder (white washed out against light rarity colors).
          // Factory New = a sliver of color then black; Battle-Scarred = mostly color with a little
          // black. The blend band (+-10%) keeps the transition soft instead of a hard fill edge.
          var f = clamp01(fraction);
          var c = color || S.accent;
          var lo = Math.max(0, f - 0.10), hi = Math.min(1, f + 0.10);
          meterFill.style.width = '100%';
          meterFill.style.backgroundColor = 'gradient( linear, 0% 0%, 100% 0%, from( ' + c + ' ), color-stop( '
            + lo.toFixed(3) + ', ' + c + ' ), color-stop( ' + hi.toFixed(3) + ', #0c0f14 ), to( #0c0f14 ) )';
          field.style.border = '1px solid ' + (c || '#ffffff2e');
        }
      };
    }

    // ---- Modal chrome --------------------------------------------------
)EDJS"
R"EDJS(
    var custOverlay = mk('Panel', root); custOverlay.visible = false; custOverlay.hittest = true;
    custOverlay.style.width = '100%'; custOverlay.style.height = '100%'; custOverlay.style.zIndex = '230';
    custOverlay.style.backgroundColor = 'rgba(0,0,0,0.78)';
    custOverlay.SetPanelEvent('onactivate', function () { closeCustomize(); }); // click-outside closes
    var custWin = mk('Panel', custOverlay); custWin.hittest = true;
    custWin.style.width = '80%'; custWin.style.height = '82%';
    custWin.style.horizontalAlign = 'center'; custWin.style.verticalAlign = 'center';
    custWin.style.backgroundColor = 'rgba(12,15,20,0.99)'; custWin.style.border = '1px solid ' + S.accent;
    custWin.style.borderRadius = '8px'; custWin.style.boxShadow = '#000000ee 0px 0px 40px 10px';
    custWin.style.flowChildren = 'down';
    custWin.SetPanelEvent('onactivate', function () { /* swallow clicks inside the window */ });

    var custHead = mk('Panel', custWin); custHead.style.flowChildren = 'right'; custHead.style.width = '100%';
    custHead.style.paddingTop = '18px'; custHead.style.paddingBottom = '18px'; custHead.style.paddingLeft = '26px'; custHead.style.paddingRight = '18px';
    custHead.style.backgroundColor = '#00000040'; custHead.style.borderBottom = '1px solid #ffffff14';
    // Title is two labels (not one string) so the player name can read in the orange accent color
    // while "CUSTOMIZE PLAYER" stays neutral -- matches the reference mock's two-tone header.
    var custTitleWrap = mk('Panel', custHead); custTitleWrap.style.flowChildren = 'right';
    custTitleWrap.style.width = 'fill-parent-flow(1.0)'; custTitleWrap.style.verticalAlign = 'center';
    var custTitlePrefix = lbl(custTitleWrap, 'CUSTOMIZE PLAYER', S.value, 22); custTitlePrefix.style.fontWeight = 'bold';
    custTitlePrefix.style.letterSpacing = '2px'; custTitlePrefix.style.verticalAlign = 'center';
    lbl(custTitleWrap, '  ·  ', S.dim, 22).style.verticalAlign = 'center';
    var custTitleName = lbl(custTitleWrap, 'PLAYER', S.accent, 22); custTitleName.style.fontWeight = 'bold';
    custTitleName.style.letterSpacing = '2px'; custTitleName.style.verticalAlign = 'center';
    custTitleName.style.whiteSpace = 'nowrap'; custTitleName.style.textOverflow = 'ellipsis';
    var custClose = btn(custHead, '✕', function () { closeCustomize(); }, S.value);
    custClose.style.marginRight = '0px'; custClose.style.width = '46px'; custClose.style.height = '46px';
    custClose.style.backgroundColor = '#ffffff10'; custClose.style.border = '1px solid #ffffff2a'; custClose.style.borderRadius = '5px';
    custClose.__lbl.style.fontSize = '19px'; custClose.__lbl.style.width = '100%'; custClose.__lbl.style.textAlign = 'center';

    var custPickupBanner = mk('Panel', custWin); custPickupBanner.visible = false; custPickupBanner.hittest = false;
    custPickupBanner.style.width = '100%'; custPickupBanner.style.flowChildren = 'right'; custPickupBanner.style.verticalAlign = 'center';
    custPickupBanner.style.paddingLeft = '26px'; custPickupBanner.style.paddingRight = '26px';
    custPickupBanner.style.paddingTop = '12px'; custPickupBanner.style.paddingBottom = '12px';
    custPickupBanner.style.backgroundColor = '#3a2a10cc'; custPickupBanner.style.borderBottom = '1px solid #ffb34755';
    var custPickupIcon = mk('Panel', custPickupBanner); custPickupIcon.hittest = false;
    custPickupIcon.style.width = '22px'; custPickupIcon.style.height = '22px'; custPickupIcon.style.verticalAlign = 'center';
    custPickupIcon.style.marginRight = '12px'; custPickupIcon.style.borderRadius = '11px'; custPickupIcon.style.backgroundColor = S.accent;
    var custPickupIconLbl = lbl(custPickupIcon, '!', '#241a06ff', 15); custPickupIconLbl.hittest = false;
    custPickupIconLbl.style.fontWeight = 'bold'; custPickupIconLbl.style.width = '100%'; custPickupIconLbl.style.textAlign = 'center';
    custPickupIconLbl.style.verticalAlign = 'center';
    var custPickupLbl = lbl(custPickupBanner, '', '#ffdba6', 13); custPickupLbl.style.width = 'fill-parent-flow(1.0)';
    custPickupLbl.style.fontWeight = 'bold'; custPickupLbl.style.whiteSpace = 'normal'; custPickupLbl.style.verticalAlign = 'center';

    var custBody = mk('Panel', custWin); custBody.style.flowChildren = 'right'; custBody.style.width = '100%';
    custBody.style.height = 'fill-parent-flow(1.0)';
    custBody.style.paddingTop = '18px'; custBody.style.paddingBottom = '20px'; custBody.style.paddingLeft = '22px'; custBody.style.paddingRight = '22px';

    // LEFT: preview. This is the SAME native panel the loadout / item-inspect / end-of-round win
    // panel use (MapPlayerPreviewPanel) -- and the win panel proves it renders inside the in-game
    // HUD context. It needs the FULL creation attribute set (a `map` scene + camera + composition
    // layer + character mode); the earlier empty black box was a panel created without a `map`
    // scene, so it had nothing to render. Config mirrors reference/ vanity-loadout.xml /
    // inspect.js. Created lazily on first open (the scene load is heavy); 2D card if it can't load.
    var prevWrap = mk('Panel', custBody); prevWrap.style.width = '46%'; prevWrap.style.height = '100%'; prevWrap.style.marginRight = '22px';
    prevWrap.style.backgroundColor = '#080a0e'; prevWrap.style.borderRadius = '6px'; prevWrap.style.border = '1px solid #ffffff14';
    prevWrap.style.flowChildren = 'down';
    prevWrap.style.overflow = 'clip';
    // Stage holds the 3D/2D preview and fills all space above the single wheel-zoom helper row (a
    // fixed-height sibling); fill-parent-flow(1.0) here is the same remaining-space pattern custBody
    // uses against custHead/custPickupBanner above. The near-black backdrop + inner border stand in
    // for a vignette -- Panorama has no CSS gradient support to verify here (see sigscan.py note).
    var prevStage = mk('Panel', prevWrap); prevStage.style.width = '100%'; prevStage.style.height = 'fill-parent-flow(1.0)';
    prevStage.style.flowChildren = 'down'; prevStage.style.overflow = 'clip';
    // Single hint: the only preview interaction is wheel-to-zoom while hovering the character (no
    // rotate/pan). The wheel is captured in C++ and routed to custWheel, which zooms when the cursor
    // is over this stage (see overPreview/zoomPreview) instead of scrolling the control column.
    var prevHelperRow = mk('Panel', prevWrap); prevHelperRow.hittest = false;
    prevHelperRow.style.width = '100%'; prevHelperRow.style.height = '38px'; prevHelperRow.style.flowChildren = 'right';
    prevHelperRow.style.horizontalAlign = 'center'; prevHelperRow.style.verticalAlign = 'center';
    prevHelperRow.style.backgroundColor = '#00000055'; prevHelperRow.style.borderTop = '1px solid #ffffff14';
    (function () {
      var it = mk('Panel', prevHelperRow); it.style.flowChildren = 'right'; it.style.verticalAlign = 'center';
      var ic = lbl(it, '▣', S.dim, 13); ic.style.marginRight = '6px'; ic.style.verticalAlign = 'center';
      var tx = lbl(it, 'Scroll to zoom', S.dim, 12); tx.style.verticalAlign = 'center'; tx.style.letterSpacing = '1px';
    })();
    var previewClip = null, preview3d = null, previewItem3d = null, preview3dTried = false, previewSerial = 0, previewModelKey = '';
    // Wheel-zoom over the character (CSS scale on the composited panel -- no re-composite, so it keeps
    // working while the demo is paused, unlike resizing the panel which would need a fresh scene tick).
    var previewZoom = 1.0, PREVIEW_ZOOM_MIN = 0.6, PREVIEW_ZOOM_MAX = 1.6, PREVIEW_ZOOM_STEP = 0.16;
    // Tuned for the cam_loadoutmenu_ct full-body camera (see PREVIEW_SCENES): 1.3 fills the preview
    // box with the whole figure at zoom 1; zoom range 0.6..MAX then spans "small full body with
    // breathing room" to "weapon close-up". (The old 0.74/-12/+22 fit was for the cropped vanity cam.)
    // FIT_X/Y live-set 2026-07-01 by the user via click-and-drag + previewSave('start') at zoom 0.96.
    var PREVIEW_FIT_SCALE = 1.3, PREVIEW_FIT_X = -25.2, PREVIEW_FIT_Y = 30.6;
    var previewPlayerSeq = 0; // bumped each time a NEW player-preview panel is created (paused-composite nudge trigger)
    var previewNudgedSeq = 0;
    var previewNudgeArmed = false;
    // Zoom-in TARGET framing (user-requested): zoomed all the way out = the full-body figure
    // (PREVIEW_FIT_* above); zooming IN glides the view UP to chest height and a touch LEFT so the
    // held weapon stays fully in frame at close-up. Implemented by interpolating the transform
    // origin (and an extra x shift) with the zoom factor: origin at the chest means magnification
    // grows around the chest, so the visible region rises as you zoom. Live-tunable via
    // previewFit('{"ox":N,"oy":N,"sx":N,"sy":N}') or click-and-drag + previewSave('zoom').
    // OX/OY tuned 2026-07-01 against screenshots (previewFit A/B at max zoom). SX/SY/ZOOM_MAX
    // live-set the same day by the user via click-and-drag + previewSave('zoom') at zoom 1.6 --
    // previewSave('zoom') sets ZOOM_MAX = whatever zoom you were at (so scrolling can never exceed
    // where you stopped) and bakes the drag straight into SX/SY, since that makes t=1 exactly here.
    var PREVIEW_ZOOMIN_OX = 60;   // transform-origin X% at max zoom (right of centre -> content drifts LEFT)
    var PREVIEW_ZOOMIN_OY = 28;   // transform-origin Y% at max zoom (~chest height of the standing figure)
    var PREVIEW_ZOOMIN_SX = -16.8; // extra x translate (px) blended in at max zoom (nudges the figure left)
    var PREVIEW_ZOOMIN_SY = 62.1;  // extra y translate (px) blended in at max zoom (drag-savable, see previewSave)
    // Click-and-drag pan: an ACCUMULATED, uncommitted offset (design px) added on top of the baked
    // fit, live-updated while the user drags inside the preview box. Persists across separate drag
    // gestures (release then drag again keeps nudging) until previewSave() bakes it into the START
    // or ZOOM-IN constants above, or previewPanReset() discards it. See previewUpdateDrag().
    var previewPanLive = { x: 0, y: 0 };
    function applyPreviewZoom() {
      // 0 at zoom<=1 (full-body framing untouched), 1 at max zoom (full chest-height migration).
      var t = (previewZoom > 1.0) ? (previewZoom - 1.0) / (PREVIEW_ZOOM_MAX - 1.0) : 0;
      if (t > 1) t = 1;
      var ox = 50 + (PREVIEW_ZOOMIN_OX - 50) * t;
      var oy = 50 + (PREVIEW_ZOOMIN_OY - 50) * t;
      var tx = PREVIEW_FIT_X + PREVIEW_ZOOMIN_SX * t + previewPanLive.x;
      var ty = PREVIEW_FIT_Y + PREVIEW_ZOOMIN_SY * t + previewPanLive.y;
      var z = (previewZoom * PREVIEW_FIT_SCALE).toFixed(3);
      var org = ox.toFixed(1) + '% ' + oy.toFixed(1) + '%';
      var tr = 'translate3d(' + Math.round(tx) + 'px, ' + Math.round(ty) + 'px, 0px) scale3d(' + z + ', ' + z + ', 1)';
      if (preview3d && preview3d.IsValid && preview3d.IsValid()) {
        preview3d.style.transformOrigin = org; preview3d.style.transform = tr;
      }
      if (typeof preview2d !== 'undefined' && preview2d) { preview2d.style.transformOrigin = org; preview2d.style.transform = tr; }
    }
    // One frame of click-and-drag panning. Panorama has no mouse-move event in this in-game HUD
    // context, so this reads the C++ cursor probe published each frame in st.mx/my/lmb (same pipe
    // CameraEditorHud::BuildStateJson feeds from CameraBridge_GetUiCursor -- built for the
    // experimental graph editor's drag tool, reused here as-is). A press must LAND on the preview
    // stage to start a drag (so dragging elsewhere in the modal, e.g. a dropdown, is untouched);
    // once started it tracks the whole modal area so a fast drag that slips outside the box edge
    // doesn't drop the gesture. Delta is converted device-px -> design-px by the same /actualuiscale
    // division positionPreview3d() uses, so drag speed matches the cursor 1:1 regardless of zoom.
    var previewDragActive = false;
    var previewDragAnchor = { x: 0, y: 0 };
    var previewDragBase = { x: 0, y: 0 };
    var previewDragLastLmb = false;
    function previewUpdateDrag() {
      if (!custOverlay.visible || !st) return;
      var lmb = !!st.lmb, mx = st.mx, my = st.my;
      if (lmb && !previewDragLastLmb && overPreview(mx, my)) {
        previewDragActive = true;
        previewDragAnchor.x = mx; previewDragAnchor.y = my;
        previewDragBase.x = previewPanLive.x; previewDragBase.y = previewPanLive.y;
        previewLog('dragStart', 'mx=' + mx + ' my=' + my);
      } else if (!lmb) {
        if (previewDragActive) previewLog('dragEnd', 'pan=' + previewPanLive.x.toFixed(1) + ',' + previewPanLive.y.toFixed(1));
        previewDragActive = false;
      }
      if (previewDragActive && lmb) {
        var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
        var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
        var nx = previewDragBase.x + (mx - previewDragAnchor.x) / sx;
        var ny = previewDragBase.y + (my - previewDragAnchor.y) / sy;
        if (nx !== previewPanLive.x || ny !== previewPanLive.y) {
          previewPanLive.x = nx; previewPanLive.y = ny;
          applyPreviewZoom();
        }
      }
      previewDragLastLmb = lmb;
    }
    // Bakes the current dragged pan into the framing you asked to save, then clears the live offset
    // so the NEXT drag starts fresh from zero again. 'start' = the zoomed-out/default framing
    // (PREVIEW_FIT_X/Y); 'zoom' = the zoomed-in framing (PREVIEW_ZOOMIN_SX/SY, blended in at max
    // zoom by applyPreviewZoom's t). Save whichever one matches the zoom level you were just at --
    // e.g. drag at zoom 1, then previewSave('start'); zoom to max, drag again, previewSave('zoom').
    function previewSavePosition(which) {
      which = ('' + (which || '')).toLowerCase();
      if (which === 'start') {
        PREVIEW_FIT_X += previewPanLive.x; PREVIEW_FIT_Y += previewPanLive.y;
        previewPanLive.x = 0; previewPanLive.y = 0;
        applyPreviewZoom();
        return 'saved start x=' + PREVIEW_FIT_X.toFixed(1) + ' y=' + PREVIEW_FIT_Y.toFixed(1);
      }
      if (which === 'zoom' || which === 'zoomin') {
        // "Save zoom" means "wherever I stopped scrolling in IS the max zoom position" -- so the
        // CURRENT zoom level becomes the new ceiling (PREVIEW_ZOOM_MAX), which makes the blend factor
        // t = (zoom-1)/(ZOOM_MAX-1) equal exactly 1 right here, and the raw dragged pan can be baked
        // straight into SX/SY with no projection needed (both are only ever evaluated at t=1 from
        // now on, since scrolling can never exceed where you are right now).
        if (previewZoom <= 1.001) {
          return 'previewSave("zoom") needs the preview zoomed in past 1.0 (currently ' + previewZoom.toFixed(2) + ')';
        }
        PREVIEW_ZOOM_MAX = previewZoom;
        PREVIEW_ZOOMIN_SX += previewPanLive.x; PREVIEW_ZOOMIN_SY += previewPanLive.y;
        previewPanLive.x = 0; previewPanLive.y = 0;
        applyPreviewZoom();
        return 'saved zoom max=' + PREVIEW_ZOOM_MAX.toFixed(2) + ' sx=' + PREVIEW_ZOOMIN_SX.toFixed(1) + ' sy=' + PREVIEW_ZOOMIN_SY.toFixed(1);
      }
      return 'usage: previewSave("start"|"zoom")';
    }
    function previewPanReset() {
      previewPanLive.x = 0; previewPanLive.y = 0;
      applyPreviewZoom();
      return 'pan reset';
    }
)EDJS"
R"EDJS(
    function zoomPreview(dir) {
      var z = previewZoom + dir * PREVIEW_ZOOM_STEP;
      if (z < PREVIEW_ZOOM_MIN) z = PREVIEW_ZOOM_MIN;
      if (z > PREVIEW_ZOOM_MAX) z = PREVIEW_ZOOM_MAX;
      if (z === previewZoom) return;
      previewZoom = z; applyPreviewZoom();
    }
    // Live fit tuner (netcon): previewFit('{"scale":0.8,"x":-12,"y":22,"zoom":1}') adjusts the CSS
    // fit transform without a panel rebuild, so the default framing can be dialed in against
    // screenshots and then baked into the PREVIEW_FIT_* defaults above.
    function previewFit(json) {
      var o = null; try { o = json ? JSON.parse(json) : null; } catch (e) { return 'bad json'; }
      if (o) {
        if (typeof o.scale === 'number') PREVIEW_FIT_SCALE = o.scale;
        if (typeof o.x === 'number') PREVIEW_FIT_X = o.x;
        if (typeof o.y === 'number') PREVIEW_FIT_Y = o.y;
        if (typeof o.zoom === 'number') previewZoom = o.zoom;
        if (typeof o.ox === 'number') PREVIEW_ZOOMIN_OX = o.ox;
        if (typeof o.oy === 'number') PREVIEW_ZOOMIN_OY = o.oy;
        if (typeof o.sx === 'number') PREVIEW_ZOOMIN_SX = o.sx;
        if (typeof o.sy === 'number') PREVIEW_ZOOMIN_SY = o.sy;
      }
      applyPreviewZoom();
      return 'fit scale=' + PREVIEW_FIT_SCALE + ' x=' + PREVIEW_FIT_X + ' y=' + PREVIEW_FIT_Y + ' zoom=' + previewZoom +
        ' zoomin ox=' + PREVIEW_ZOOMIN_OX + ' oy=' + PREVIEW_ZOOMIN_OY + ' sx=' + PREVIEW_ZOOMIN_SX + ' sy=' + PREVIEW_ZOOMIN_SY +
        ' pan=' + previewPanLive.x.toFixed(1) + ',' + previewPanLive.y.toFixed(1);
    }
    function overPreview(x, y) {
      if (typeof x !== 'number' || typeof y !== 'number') return false;
      if (!prevStage || !prevStage.IsValid || !prevStage.IsValid()) return false;
      var a = prevStage.GetPositionWithinWindow ? prevStage.GetPositionWithinWindow() : null;
      if (!a) return false;
      var w = prevStage.actuallayoutwidth || 0, h = prevStage.actuallayoutheight || 0;
      return x >= a.x && x <= a.x + w && y >= a.y && y <= a.y + h;
    }
    // ---- Preview diagnostics (mvm_debug) -----------------------------------------------------
    // Gated on st.mvmDebug, which CameraEditorHud::BuildStateJson publishes from
    // Filmmaker::MvmDebugLog_Active() -- so `mvm_debug start` (the existing native session logger,
    // see CosmeticDebugLog.cpp) transparently starts capturing preview state too: every $.Msg() call
    // is mirrored into the tier0 logging listener the debug log taps, no separate toggle needed. When
    // no log is running this is a single boolean check per call site, so it's cheap to leave in place.
    // Prints go to the console (and therefore netcon), so keep them OFF (this guard) outside a
    // debug session -- they would otherwise spam every wheel notch / frame.
    function mvmDebugOn() { return !!(st && st.mvmDebug); }
    // Raw rects for the preview stage vs the clip box that is supposed to contain it, plus the zoom
    // scale -- the exact numbers needed to tell "clipped correctly but framed oddly" apart from
    // "escaping its own clip box" when reading a log next to a screenshot.
    function previewGeom() {
      var stageA = (prevStage && prevStage.GetPositionWithinWindow) ? prevStage.GetPositionWithinWindow() : null;
      var stageW = prevStage ? (prevStage.actuallayoutwidth || 0) : 0, stageH = prevStage ? (prevStage.actuallayoutheight || 0) : 0;
      var clipA = (previewClip && previewClip.GetPositionWithinWindow) ? previewClip.GetPositionWithinWindow() : null;
      var clipW = previewClip ? (previewClip.actuallayoutwidth || 0) : 0, clipH = previewClip ? (previewClip.actuallayoutheight || 0) : 0;
      var p3dW = preview3d ? (preview3d.actuallayoutwidth || 0) : 0, p3dH = preview3d ? (preview3d.actuallayoutheight || 0) : 0;
      return {
        zoom: previewZoom, fitScale: PREVIEW_FIT_SCALE, effScale: previewZoom * PREVIEW_FIT_SCALE,
        fitX: PREVIEW_FIT_X, fitY: PREVIEW_FIT_Y,
        zoomin: { ox: PREVIEW_ZOOMIN_OX, oy: PREVIEW_ZOOMIN_OY, sx: PREVIEW_ZOOMIN_SX, sy: PREVIEW_ZOOMIN_SY },
        pan: { x: previewPanLive.x, y: previewPanLive.y, dragging: previewDragActive },
        stage: { x: stageA ? stageA.x : -1, y: stageA ? stageA.y : -1, w: stageW, h: stageH },
        clip: { x: clipA ? clipA.x : -1, y: clipA ? clipA.y : -1, w: clipW, h: clipH },
        p3d: { w: p3dW, h: p3dH }
      };
    }
    var previewLogLastSig = ''; // last logged snapshot signature -- collapses identical per-frame calls
    function previewLog(tag, extra) {
      if (!mvmDebugOn()) return;
      try {
        var g = previewGeom();
        var sig = tag + '|' + custTargetKey + '|' + custActiveCategory + '|' + previewModelKey + '|' +
          g.zoom.toFixed(2) + '|' + (extra || '');
        if (tag === 'frame' && sig === previewLogLastSig) return; // heartbeat-only tag may repeat every frame
        previewLogLastSig = sig;
        var p3dState = preview3d ? ((preview3d.IsValid && preview3d.IsValid()) ? 'valid' : 'invalid')
          : (preview3dTried ? 'failed' : 'none');
        var line = '[mvmpreview] ' + tag +
          ' open=' + (custOverlay.visible ? 1 : 0) +
          ' target=' + custTargetKey + '(' + custTargetName + ')' +
          ' cat=' + custActiveCategory +
          ' p3d=' + p3dState +
          ' modelKey=' + previewModelKey +
          ' zoom=' + g.zoom.toFixed(2) + ' effScale=' + g.effScale.toFixed(3) +
          ' stage=' + g.stage.x + ',' + g.stage.y + ' ' + g.stage.w + 'x' + g.stage.h +
          ' clip=' + g.clip.x + ',' + g.clip.y + ' ' + g.clip.w + 'x' + g.clip.h +
          ' sel.agent=' + (custSel.agent || '') + ' sel.primary=' + (custSel.primary || '') +
          ' sel.secondary=' + (custSel.secondary || '') + ' sel.knife=' + (custSel.knife || '') +
          ' sel.gloves=' + (custSel.gloves || '');
        if (extra) line += ' ' + extra;
        $.Msg(line);
      } catch (e) {}
    }
)EDJS"
R"EDJS(
    // Native 3D MapPlayerPreviewPanel (vanity loadout scene). The make-or-break detail is TIMING:
    // the panel must be CREATED after the modal overlay is visible AND has a laid-out size, or it
    // instantiates but renders black. So creation is deferred via $.Schedule from openCustomize
    // (recreatePreview), not done inline while the overlay is still 0-sized. The 2D card below is a
    // fallback shown only if the native panel fails to instantiate.
    var USE_3D_PREVIEW = true;

    // 2D loadout card (fallback): player header + one rarity-colored row per slot, so the spectated
    // player's actual weapons/knife/gloves (and any picked skins) stay readable if 3D is unavailable.
    var preview2d = mk('Panel', prevStage); preview2d.style.width = '100%'; preview2d.style.height = '100%';
    preview2d.style.flowChildren = 'down'; preview2d.style.paddingTop = '30px';
    preview2d.style.paddingLeft = '30px'; preview2d.style.paddingRight = '30px'; preview2d.visible = true;
    var prev2dPlayer = lbl(preview2d, '', S.value, 23); prev2dPlayer.style.fontWeight = 'bold'; prev2dPlayer.style.letterSpacing = '1px';
    var prev2dTeam = lbl(preview2d, '', S.dim, 13); prev2dTeam.style.marginTop = '2px'; prev2dTeam.style.marginBottom = '22px';
    function prev2dRow(label) {
      var row = mk('Panel', preview2d); row.style.width = '100%'; row.style.flowChildren = 'down';
      row.style.marginBottom = '16px'; row.style.paddingLeft = '13px'; row.style.paddingTop = '3px'; row.style.paddingBottom = '3px';
      row.style.borderLeft = '3px solid ' + S.accent;
      var l = lbl(row, label, S.dim, 11); l.style.fontWeight = 'bold'; l.style.letterSpacing = '2px';
      var v = lbl(row, '', S.value, 17); v.style.fontWeight = 'bold'; v.style.marginTop = '3px';
      v.style.whiteSpace = 'nowrap'; v.style.textOverflow = 'ellipsis'; v.style.width = '100%';
      return { row: row, l: l, v: v };
    }
    var prev2dCard = {
      agent: prev2dRow('AGENT'), primary: prev2dRow('PRIMARY'), secondary: prev2dRow('SECONDARY'),
      knife: prev2dRow('MELEE / KNIFE'), gloves: prev2dRow('GLOVES')
    };
    function renderLoadoutCard() {
      prev2dPlayer.text = (custTargetName || 'Player').toUpperCase();
      var tm = normalizeTeamName(custTargetTeam);
      prev2dTeam.text = tm === 'CT' ? 'Counter-Terrorist' : (tm === 'T' ? 'Terrorist' : 'Spectated player');
      var slots = ['agent', 'primary', 'secondary', 'knife', 'gloves'];
      for (var i = 0; i < slots.length; i++) {
        var slot = slots[i], entry = prev2dCard[slot];
        var has = (slot === 'primary' || slot === 'secondary')
          ? !!(custLoadout[slot] && custLoadout[slot].defIndex > 0) : true;
        entry.row.visible = has;
        if (!has) continue;
        var opt = selOpt(slot, custSel[slot]);
        var meta = opt ? (opt[2] || {}) : {};
        var name = opt ? opt[0] : '—';
        if (slot !== 'agent') name = withWearLabel(slot, name);
        entry.v.text = name;
        var col = meta.color || S.value;
        entry.v.style.color = col;
        entry.row.style.borderLeft = '3px solid ' + col;
      }
    }

    // Default agent model for the spectated player's team (until we read their real model in C++).
    function teamDefaultModel() {
      return (normalizeTeamName(custTargetTeam) === 'CT') ? 'agents/models/ctm_sas/ctm_sas.vmdl'
                                                          : 'agents/models/tm_phoenix/tm_phoenix.vmdl';
    }
    function loadoutTeam() {
      return (normalizeTeamName(custTargetTeam) === 'CT') ? 'ct' : 't';
    }
    function loadoutItem(slot) {
      try { return LoadoutAPI.GetItemID(loadoutTeam(), slot) || ''; }
      catch (e) { return ''; }
    }
)EDJS"
R"EDJS(
    function previewState() {
      var playerState = preview3d ? ('player=' + (preview3d.IsValid ? preview3d.IsValid() : '?'))
                                  : (preview3dTried ? 'player-fallback' : 'player-uninit');
      var itemState = previewItem3d ? (' item=' + (previewItem3d.IsValid ? previewItem3d.IsValid() : '?')) : ' item-uninit';
      return playerState + itemState;
    }
    // Scene presets for the MapPlayerPreviewPanel. CRUCIAL: the vanity/loadout scenes
    // (ui/buy_menu + cam_vanityloadout / cam_loadoutmenu_*) only render in the MAIN-MENU Panorama
    // root; in the in-game HUD (CSGOHud, where this editor lives) they instantiate but composite
    // nothing -> a black box. That conclusion turned out to be an attribute problem, not a scene
    // restriction: a `composition-layer-texture-name` attribute (paired with `require-composition-layer`)
    // is what actually lets Panorama's compositor bind and display the panel's render target, and we
    // were never setting it. A CS2 cheat-menu thread (github/UC "Preview Models in Menu") confirms the
    // vanity/buy_menu scene DOES composite outside the main-menu root once that attribute -- plus
    // `player`, `sync_spawn_addons`, and `csm_split_plane0_distance_override` -- are set:
    //   $.CreatePanel('MapPlayerPreviewPanel', ctx, 'preview_texture_name', {
    //     map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', 'require-composition-layer': true,
    //     'composition-layer-texture-name': 'preview_texture_name', playermodel: '...',
    //     animgraphcharactermode: 'buy-menu', player: true, mouse_rotate: true,
    //     sync_spawn_addons: true, 'transparent-background': true, 'pin-fov': 'vertical',
    //     csm_split_plane0_distance_override: '250.0' });
    // So the vanity scene (proper lit loadout backdrop, not the flat grey mvp-banner fallback) is the
    // default again; match_mvp/loadoutmenu_ct stay for live A/B over `previewTry`.
    // Scene 0 (default) uses cam_loadoutmenu_ct, NOT cam_vanityloadout: the vanity camera frames only
    // the upper body and hard-crops at the thighs INSIDE the rendered texture (live-verified: CSS
    // zoom-out to 0.6 showed the same thigh cut with black below -- the legs simply are not in the
    // texture, and pin-fov horizontal made no difference). The loadout camera renders the full body
    // head-to-boots, so zooming out actually reveals the whole character; PREVIEW_FIT_SCALE below is
    // tuned for it (1.3 fills the box with the full figure at zoom 1).
    var PREVIEW_SCENES = [
      { map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', mode: 'buy-menu', pname: 'vanity_character', bg: 'true' },
      { map: 'ui/match_mvp', camera: 'camera', mode: 'mvp-banner', pname: 'mvp_char', bg: 'false' },
      { map: 'ui/buy_menu', camera: 'cam_vanityloadout', mode: 'buy-menu', pname: 'vanity_character', bg: 'true' }
    ];
    var previewSceneIdx = 0;
    function currentScene() { return PREVIEW_SCENES[previewSceneIdx] || PREVIEW_SCENES[0]; }
    function destroyPreview3d() {
      try { if (previewClip && previewClip.DeleteAsync) previewClip.DeleteAsync(0); } catch (e0) {}
      try { if (preview3d && preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e1) {}
      previewClip = null; preview3d = null; preview3dTried = false; previewModelKey = '';
      previewDragCatcher = null; // child of previewClip -- deleted along with it above
    }
    var previewDragCatcher = null; // transparent hit-test blocker over preview3d, see below
    function ensurePreviewClip() {
      if (previewClip && previewClip.IsValid && previewClip.IsValid()) return previewClip;
      previewClip = mk('Panel', root);
      previewClip.hittest = false;
      previewClip.visible = false;
      previewClip.style.overflow = 'clip';
      previewClip.style.zIndex = '232';
      previewClip.style.backgroundColor = 'rgba(0,0,0,0)';
      // Sits ABOVE preview3d (zIndex 1 > preview3d's 0) and absorbs every click itself (hittest=true,
      // fully transparent) so the native panel's own mouse_rotate/panzoom_enabled (kept true -- see
      // createPreview3d comment -- because disabling them was a compositing risk we didn't want to
      // retest) never receives a drag to fight our click-and-drag pan with. Our own pan tracking
      // (previewUpdateDrag) reads the OS-cursor pipe directly and does not depend on this panel
      // getting the hit -- it only needs to exist so nothing ELSE does.
      previewDragCatcher = mk('Panel', previewClip);
      previewDragCatcher.hittest = true;
      previewDragCatcher.style.width = '100%'; previewDragCatcher.style.height = '100%';
      previewDragCatcher.style.position = '0px 0px 0px';
      previewDragCatcher.style.zIndex = '1';
      previewDragCatcher.style.backgroundColor = 'rgba(0,0,0,0)';
      return previewClip;
    }
    // Create the native MapPlayerPreviewPanel with the current scene's attrs (optionally overridden,
    // for live tuning over netcon). Returns the panel or null.
    function createPreview3d(overrides) {
      var sc = currentScene();
      var texName = 'CustPreviewTex' + (++previewSerial);
      var attrs = {
        'require-composition-layer': 'true', 'composition-layer-texture-name': texName,
        'pin-fov': 'vertical', 'transparent-background': sc.bg,
        'class': 'mvp_map', map: sc.map, camera: sc.camera, animgraphcharactermode: sc.mode,
        playername: sc.pname, player: 'true', mouse_rotate: 'true', sync_spawn_addons: 'true',
        csm_split_plane0_distance_override: '250.0',
        // CRITICAL for the paused demo: without this the panel HIDES ITSELF while its composite
        // materials are still building, and on a paused demo (no game frames advancing) that build
        // may never signal complete -> the character never appears ("no visible person"). The
        // native vanity-loadout.xml AND loadout_grid.xml both set this false for exactly this
        // reason (show the model immediately, don't wait/cull). This is the panel property the
        // UnknownCheats thread warns about ("prevent panorama from culling them").
        hide_while_waiting_for_composite_materials: 'false',
        // These native interaction attrs are also part of the compositor path for this panel. We keep
        // them enabled for rendering, then place a transparent blocker above the preview so user drags
        // never reach the native rotate/pan handlers. Wheel is swallowed in C++ and routed to custWheel.
        hittest: 'true', panzoom_enabled: 'true'
      };
      attrs.playermodel = (overrides && overrides.playermodel) ? overrides.playermodel : teamDefaultModel();
      // Merge ANY other override keys verbatim (camera, map, pin-fov, animgraphcharactermode, ...) so
      // previewRebuild('{"pin-fov":"horizontal"}') can live-A/B panel attrs over netcon without a
      // rebuild -- the api comment always advertised map/camera overrides but only playermodel was
      // actually honored.
      if (overrides) { for (var ok in overrides) { if (ok !== 'playermodel') attrs[ok] = '' + overrides[ok]; } }
      var p = null;
      try {
        // Create inside a root-level clipped viewport, not inside the styled/flowing preview frame.
        // The clip panel prevents CSS zoom from drawing over the controls, while staying root-level
        // avoids the blank composition target seen when the native preview was nested in prevStage.
        var clip = ensurePreviewClip();
        p = clip ? $.CreatePanel('MapPlayerPreviewPanel', clip, texName, attrs) : null;
        if (!(p && p.IsValid && p.IsValid())) p = null;
      } catch (e) { p = null; }
      if (p) {
        previewPlayerSeq++;
        p.style.width = '100%';
        p.style.height = '100%';
        p.style.position = '0px 0px 0px';
        p.style.zIndex = '0';
        p.style.overflow = 'clip';
        positionPreview3d();
      }
      return p;
    }
    function positionPreview3d() {
      if (!previewClip || !previewClip.IsValid || !previewClip.IsValid()) return;
      if (!custOverlay.visible || !prevStage || !prevStage.IsValid || !prevStage.IsValid()) {
        previewClip.visible = false;
        return;
      }
      var a = prevStage.GetPositionWithinWindow ? prevStage.GetPositionWithinWindow() : null;
      var rawW = prevStage.actuallayoutwidth || 0, rawH = prevStage.actuallayoutheight || 0;
      if (!a || rawW <= 0 || rawH <= 0) { previewClip.visible = false; return; }
      // GetPositionWithinWindow()/actuallayoutwidth report ACTUAL (device) pixels, but previewClip is
      // a ROOT-level panel whose style 'px' values are interpreted in DESIGN-space units (same space
      // every other style declaration in this file uses) and then scaled up to device pixels by
      // actualuiscale (measured live at 1.1111 = 1600x1200 device / 1440x1080 design). Assigning the
      // raw device-px numbers straight into style.x/y/width/height skipped that conversion, so the
      // clip box rendered ~11% too big and off-position relative to prevStage's real on-screen box --
      // negligible-looking at zoom 1, but the error compounds with zoom and pushes the character
      // outside the intended frame ("not locked to the box"). custMeasureH() below already divides by
      // actualuiscale_y for the same reason (device px -> design px); mirror that here for x/y/w/h.
      var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var w = Math.floor(rawW / sx);
      var h = Math.floor(rawH / sy);
      if (w <= 0 || h <= 0) { previewClip.visible = false; return; }
      var x = Math.floor(a.x / sx), y = Math.floor(a.y / sy);
      previewClip.visible = true;
      previewClip.style.position = x + 'px ' + y + 'px 0px';
      previewClip.style.x = x + 'px';
      previewClip.style.y = y + 'px';
      previewClip.style.width = w + 'px';
      previewClip.style.height = h + 'px';
      if (preview3d && preview3d.IsValid && preview3d.IsValid()) {
        preview3d.visible = true;
        preview3d.style.width = '100%';
        preview3d.style.height = '100%';
      }
    }
)EDJS"
R"EDJS(
    function nudgePreviewCompositeIfPaused() {
      if (!preview3d || previewNudgedSeq === previewPlayerSeq) return;
      if (!previewNudgeArmed) return;
      previewNudgedSeq = previewPlayerSeq;
      previewNudgeArmed = false;
      refreshPreviewComposition();
      // Live-confirmed (screenshots + mvm_debug): the Panorama-side SetReadyForDisplay toggle above is
      // NOT enough by itself -- the native MapPlayerPreviewPanel composite stays fully black while the
      // demo sits paused no matter how long you wait, close/reopen the modal, or re-pick items; only an
      // actual demo_resume made it render (and it then STAYS rendered after re-pausing). Reuse the same
      // briefly-resume-then-repause lever the cosmetics backend already relies on for third-person body
      // swaps (CosmeticOverrideSystem::RequestApplyNudge/MaybeFireTickNudge) via a dedicated one-shot
      // command, so the preview panel gets the live frames it needs without a real cosmetic mutation.
      if (st && st.paused) {
        previewLog('nativeNudge');
        cmd('mirv_filmmaker cosmetics previewnudge');
      }
    }
    function refreshPreviewComposition() {
      if (!preview3d || !preview3d.IsValid || !preview3d.IsValid()) return;
      // Keep this independent of demo playback: the preview is a Panorama scene, so refresh the
      // composition layer through the panel lifecycle instead of seeking the demo timeline.
      safeCall(preview3d, 'SetReadyForDisplay', false);
      try {
        $.Schedule(0.0, function () {
          if (preview3d && preview3d.IsValid && preview3d.IsValid()) safeCall(preview3d, 'SetReadyForDisplay', true);
        });
      } catch (e) {
        safeCall(preview3d, 'SetReadyForDisplay', true);
      }
    }
    function schedulePreviewCompositeNudge() {
      try { $.Schedule(0.85, nudgePreviewCompositeIfPaused); } catch (e) { nudgePreviewCompositeIfPaused(); }
    }
    function ensurePreview3d(model) {
      if (!USE_3D_PREVIEW) { preview2d.visible = true; return null; }
      model = model || teamDefaultModel();
      if (preview3d || preview3dTried) return preview3d;
      preview3dTried = true;
      preview3d = createPreview3d({ playermodel: model });
      preview2d.visible = !preview3d;
      return preview3d;
    }
    function ensureViewmodelPreview() {
      if (previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid()) return previewItem3d;
      previewItem3d = null;
      try {
        previewItem3d = $.CreatePanel('MapItemPreviewPanel', prevStage, 'CustViewmodel3D' + (++previewSerial), {
          map: 'ui/xpshop_item', camera: 'camera_weapon_0', 'require-composition-layer': 'true',
          player: 'false', initial_entity: 'item', mouse_rotate: 'true', sync_spawn_addons: 'true',
          'transparent-background': 'true', 'pin-fov': 'vertical', hittest: 'true',
          panzoom_enabled: 'true', hide_while_waiting_for_composite_materials: 'false'
        });
        if (!(previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid())) previewItem3d = null;
      } catch (e) { previewItem3d = null; }
      if (previewItem3d) {
        previewItem3d.style.width = '100%';
        previewItem3d.style.height = '34%';
        previewItem3d.style.marginTop = '8px';
        safeCall(previewItem3d, 'SetHideStaticGeometry', true);
        safeCall(previewItem3d, 'SetReadyForDisplay', true);
      }
      return previewItem3d;
    }
    function setItemPreviewItem(panel, itemId) {
      if (!panel || itemId == null) return;
      safeCall(panel, 'SetActiveItem', 0);
      if (safeCall(panel, 'SetItemItemId', itemId, '') == null) safeCall(panel, 'SetItemItemId', itemId.toString(), '');
      safeCall(panel, 'StartWeaponLookat');
      safeCall(panel, 'SetReadyForDisplay', true);
    }
    function equipPlayerPreviewMeta(panel, meta) {
      if (!panel || !meta) return;
      var itemId = previewEquipItemId(meta);
      if (!itemId) return;
      if (safeCall(panel, 'EquipPlayerWithItem', itemId) == null) safeCall(panel, 'EquipPlayerWithItem', itemId.toString());
    }
    function previewMetaId(meta) {
      var id = previewEquipItemId(meta);
      if (id) return '' + id;
      if (!meta) return '0:0';
      return [
        parseInt(meta.def || 0, 10) || 0,
        parseInt(meta.paint || meta.paintKit || 0, 10) || 0
      ].join(':');
    }
    // Push the chosen agent (or the spectated player's team default) onto whichever preview exists.
    // The item the preview should show HELD right now: whichever slot is "active" (see
    // setActiveCategory -- switches when the user clicks into Primary/Secondary/Melee). If that
    // slot's current selection is the generic "no paint" row (meta.def<=0, shared across every
    // weapon of that def), fall back to the weapon actually targeted for the slot (effectiveSlotDef)
    // so the model still holds a plain, unskinned copy of the right gun instead of nothing.
    function activeHeldItemMeta() {
      var cat = (custActiveCategory === 'secondary' || custActiveCategory === 'knife') ? custActiveCategory : 'primary';
      var opt = selOpt(cat, custSel[cat]);
      var meta = opt ? (opt[2] || {}) : {};
      if (parseInt(meta.def || 0, 10) > 0) return meta;
      var slotDef = (cat === 'primary' || cat === 'secondary') ? effectiveSlotDef(cat) : 0;
      return (slotDef > 0) ? { def: slotDef, paint: 0, color: meta.color } : meta;
    }
    function previewSceneKey(model, agentMeta, heldMeta, glovesMeta) {
      return [
        model || '',
        previewMetaId(agentMeta),
        previewMetaId(heldMeta),
        previewMetaId(glovesMeta)
      ].join('|');
    }
    // Mirrors the native vanity-loadout recipe (reference/ scripts/common/characteranims.js
    // PlayAnimsOnPanel): SetPlayerCharacterItemID + SetPlayerModel together give the composited
    // agent its real material (skin tone, team paint job); SetPlayerModel alone -- what this used to
    // do -- leaves the model on the flat, uncomposited fallback material (the washed-out grey/white
    // look the modal used to show). Then EquipPlayerWithItem for the held weapon/knife (ONE at a
    // time, matching whichever slot is active) and the gloves (always, independent of the held item).
    function applyPreview() {
      var agent = selOpt('agent', custSel.agent), gloves = selOpt('gloves', custSel.gloves);
      var held = activeHeldItemMeta();
      var model = (agent && agent[2] && agent[2].model) ? agent[2].model : teamDefaultModel();
      var agentMeta = agent ? agent[2] : null;
      var glovesMeta = gloves ? gloves[2] : null;
      var sceneKey = previewSceneKey(model, agentMeta, held, glovesMeta);
      if (preview3d && preview3d.IsValid && !preview3d.IsValid()) {
        destroyPreview3d();
      }
      ensurePreview3d(model);
      if (preview3d) {
        var changed = previewModelKey !== sceneKey;
        previewModelKey = sceneKey;
        var sc = currentScene();
        if (sc.startCamera) safeCall(preview3d, 'TransitionToCamera', sc.startCamera, 0);
        safeCall(preview3d, 'SetActiveCharacter', 0); // char 0 = the single previewed agent
        var agentItemId = previewEquipItemId(agentMeta);
        if (agentItemId) safeCall(preview3d, 'SetPlayerCharacterItemID', agentItemId);
        safeCall(preview3d, 'SetPlayerModel', model);
        equipPlayerPreviewMeta(preview3d, held);
        equipPlayerPreviewMeta(preview3d, glovesMeta);
        safeCall(preview3d, 'SetReadyForDisplay', true);
        applyPreviewZoom();
        if (changed) {
          pokePreviewSoon(); refreshPreviewComposition();
          previewLog('sceneChanged', 'model=' + model + ' agentItemId=' + (agentItemId || 0) +
            ' heldId=' + previewMetaId(held) + ' glovesId=' + previewMetaId(glovesMeta));
          // Paused demo: the preview scene only ticks on live frames, so an item/category change
          // equips the new weapon but the character NEVER plays the pose transition -- the gun
          // floats in mid-air in the old stance (user-reported: pick Tec-9 while a rifle pose is
          // held). Request the same brief resume/re-pause the modal-open path uses so the animgraph
          // gets real ticks to settle into the new hold pose. Backend-debounced (8 frames), so a
          // rapid pick burst coalesces into one nudge.
          if (st && st.paused) { previewLog('poseNudge'); cmd('mirv_filmmaker cosmetics previewnudge'); }
        }
      } else {
        renderLoadoutCard();
        previewLog('fallback2d');
      }
    }

    // The native MapPlayerPreviewPanel only composites its scene when its parent is VISIBLE and has
    // a real laid-out size. Creating it while the modal overlay was still hidden (the old open
    // order) left it permanently blank -- it only "worked" when external automation re-poked it
    // after the modal was already up. So we make the overlay visible first, then re-assert the
    // preview for a few frames from render() (layout settles a frame or two after visible flips).
    var previewPokeFrames = 0;
    var previewHealTries = 0; // bounded retries for maintainPreview's self-heal when the panel is missing
    function pokePreviewSoon() { previewPokeFrames = 16; previewHealTries = 8; }
)EDJS"
R"EDJS(
    function maintainPreview() {
      if (!custOverlay.visible) return;
      // Keep the composition-layer panel marked ready EVERY frame while the modal is open. Without
      // this continuous (cheap, idempotent) re-assert the scene composites once and then lapses to
      // black after the initial settle burst -- the "appears then disappears" bug. The heavier
      // re-equip/re-model (applyPreview) only runs during the post-open settle frames.
      if (preview3d && preview3d.IsValid && preview3d.IsValid()) {
        positionPreview3d();
        safeCall(preview3d, 'SetReadyForDisplay', true);
      }
      // Same idempotent re-assert for the optional held-item/viewmodel preview when one exists, so a
      // paused demo (no game frames driving the composite) can't let it lapse to black either.
      if (previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid())
        safeCall(previewItem3d, 'SetReadyForDisplay', true);
      // Self-heal: if the panel failed to create while the modal is up, retry a FRESH create -- on a
      // PAUSED demo the one-shot post-open schedule may land before layout settles, leaving no panel.
      // Reset preview3dTried so ensurePreview3d (inside applyPreview) actually re-attempts; bounded by
      // previewHealTries so a genuinely unavailable scene falls back to the 2D card instead of
      // rebuilding forever. Uses applyPreview (not recreatePreview, which would re-arm this counter).
      if (USE_3D_PREVIEW && !preview3d && previewHealTries > 0) {
        previewHealTries--;
        previewLog('selfHeal', 'triesLeft=' + previewHealTries);
        preview3dTried = false; previewModelKey = '';
        applyPreview();
      }
      if (previewPokeFrames > 0) { previewPokeFrames--; applyPreview(); }
    }
    // Destroy + rebuild the 3D preview from scratch. Must run when the modal is already visible and
    // laid out (deferred via $.Schedule from openCustomize) -- creating it on a 0-sized parent makes
    // it render black even though it reports valid.
    function recreatePreview() {
      if (!custOverlay.visible) return;
      if (!USE_3D_PREVIEW) { preview2d.visible = true; renderLoadoutCard(); return; }
      previewLog('recreate');
      destroyPreview3d();
      applyPreview();      // creates the panel + equips + SetReadyForDisplay
      pokePreviewSoon();   // re-assert SetReadyForDisplay for the frames after the new panel lays out
      if (previewNudgeArmed) schedulePreviewCompositeNudge();
    }

)EDJS"
R"EDJS(
    function normalizeTeamName(t) {
      t = (t || '').toString().toUpperCase();
      if (t === '3' || t === 'CT' || t.indexOf('COUNTER') >= 0) return 'CT';
      if (t === '2' || t === 'T' || t.indexOf('TERROR') >= 0) return 'T';
      return '';
    }
    function normalizeLoadoutWeapon(w) {
      if (!w || !(parseInt(w.defIndex || 0, 10) > 0)) return null;
      return {
        entityIndex: parseInt(w.entityIndex || -1, 10),
        defIndex: parseInt(w.defIndex || 0, 10) || 0,
        paintKit: parseInt(w.paintKit || 0, 10) || 0,
        wear: num(w.wear),
        // Per-slot pickup ownership (C++ derives it from the weapon ENTITY's original-owner xuid,
        // so it is independent of which weapon the player currently has out).
        pickup: !!w.pickup,
        ownerSteamId: w.ownerSteamId ? ('' + w.ownerSteamId) : '',
        ownerName: w.ownerName ? ('' + w.ownerName) : ''
      };
    }
    // Slot-level pickup state: TRUE when the weapon sitting in this loadout slot originally belongs
    // to another player. Unlike custActiveWeaponPickup this does NOT flicker off when the player
    // switches to a grenade/knife/bomb -- the slot keeps its entity-derived ownership until the
    // weapon is actually dropped, the player changes, or a seek rebuilds the loadout.
    function slotPickup(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return false;
      var w = custLoadout[slot];
      return !!(w && w.pickup && w.ownerSteamId);
    }
    function slotOwnerSteam(slot) { var w = custLoadout[slot]; return (w && w.ownerSteamId) || ''; }
    function slotOwnerName(slot) {
      var w = custLoadout[slot];
      if (!w || !w.ownerSteamId) return '';
      return w.ownerName || ('Player ' + w.ownerSteamId);
    }
    function collectPanelText(panel, out, depth) {
      if (!panel || depth > 24) return;
      try {
        if (typeof panel.text === 'string' && panel.text) out.push(panel.text);
      } catch (e) {}
      var count = 0;
      try { count = panel.GetChildCount ? panel.GetChildCount() : 0; } catch (e2) { count = 0; }
      for (var i = 0; i < count; i++) {
        var child = null;
        try { child = panel.GetChild(i); } catch (e3) { child = null; }
        collectPanelText(child, out, depth + 1);
      }
    }
    function hudWeaponText() {
      var roots = ['HudWeaponSelection', 'hud-WPN-main', 'HudAmmo'];
      var text = [];
      for (var i = 0; i < roots.length; i++) {
        var p = null;
        try { p = $.GetContextPanel().FindChildTraverse(roots[i]); } catch (e) { p = null; }
        collectPanelText(p, text, 0);
      }
      return text.join(' | ');
    }
    function inferActivePaintFromHud(slot) {
      if (!slot) return null;
      var activeText = hudWeaponText();
      if (!activeText) return null;
      var arr = COSMETICS[slot] || [];
      var activeDef = parseInt(custActiveWeaponDef || 0, 10) || 0;
      for (var i = 0; i < arr.length; i++) {
        var label = arr[i][0] || '', meta = arr[i][2] || {};
        var def = parseInt(meta.def || 0, 10) || 0;
        var paint = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
        if (!label || paint <= 0 || def !== activeDef) continue;
        if (activeText.indexOf(label) >= 0) {
          return { value: arr[i][1], defIndex: def, paintKit: paint };
        }
      }
      return null;
    }
    function applyCustomizeTargetLoadout(ct) {
      custLoadout = { primary: null, secondary: null, knife: null, gloves: null, agentModel: '' };
      if (ct && ct.weapons) {
        custLoadout.primary = normalizeLoadoutWeapon(ct.weapons.primary);
        custLoadout.secondary = normalizeLoadoutWeapon(ct.weapons.secondary);
        custLoadout.knife = normalizeLoadoutWeapon(ct.weapons.knife);
        custLoadout.gloves = normalizeLoadoutWeapon(ct.weapons.gloves);
      }
      if (ct && typeof ct.agentModel === 'string') custLoadout.agentModel = ct.agentModel;
      var activeSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
      if (activeSlot && !custLoadout[activeSlot])
        custLoadout[activeSlot] = { entityIndex: parseInt(ct && ct.activeWeaponIndex || -1, 10), defIndex: custActiveWeaponDef, paintKit: 0, wear: 0.01 };
      if (activeSlot && custLoadout[activeSlot] && !(parseInt(custLoadout[activeSlot].paintKit || 0, 10) > 0)) {
        var inferred = inferActivePaintFromHud(activeSlot);
        if (inferred) custLoadout[activeSlot].paintKit = inferred.paintKit;
      }
    }
    function customizeLoadoutSignature() {
      function part(slot) {
        var w = custLoadout[slot] || {};
        return [parseInt(w.defIndex || 0, 10) || 0, parseInt(w.paintKit || 0, 10) || 0, Number(w.wear || 0).toFixed(4)].join(':');
      }
      return [custTargetKey || '', normalizeTeamName(custTargetTeam), part('primary'), part('secondary'), part('knife'), part('gloves'), custLoadout.agentModel || ''].join('|');
    }
    // Distinct weapons available for a primary/secondary slot (the weapon-selector list). Derives one
    // entry per weapon def from the full skin catalog, preferring a real weapon name ("AK-47") over the
    // generic "Default ... skin" label, and carrying that weapon's default thumbnail for the swatch.
    function weaponsForSlot(slot) {
      var arr = COSMETICS[slot] || [];
      var byDef = {}, order = [];
      for (var i = 0; i < arr.length; i++) {
        var m = arr[i][2] || {};
        var def = parseInt(m.def || 0, 10) || 0;
        if (def <= 0) continue;
        var label = '' + arr[i][0];
        var hasSkinName = label.indexOf('|') >= 0;
        var nm = (hasSkinName ? label.split('|')[0] : label).replace(/^\s+|\s+$/g, '');
        if (!byDef[def]) { byDef[def] = { name: nm, color: m.color, img: m.img }; order.push(def); }
        else if (hasSkinName) { byDef[def].name = nm; } // prefer a real "AK-47"-style name
        if (parseInt(m.paint || 0, 10) === 0 && m.img && !byDef[def].img) byDef[def].img = m.img;
      }
      order.sort(function (a, b) { return (byDef[a].name < byDef[b].name) ? -1 : 1; });
      var out = [];
      for (var j = 0; j < order.length; j++) {
        var d = order[j];
        out.push([byDef[d].name || ('Weapon ' + d), '' + d,
          { color: byDef[d].color || rarColor(0), def: d, paint: 0, img: byDef[d].img }]);
      }
      return out;
    }
    // The weapon def the primary/secondary skin dropdown currently targets: an explicitly-picked weapon
    // if any, else the player's held weapon, else the first weapon in the catalog (so the block is
    // usable even when the player is not holding a weapon in that slot).
    function effectiveSlotDef(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return 0;
      if (custWeaponDef[slot] > 0) return custWeaponDef[slot];
      var held = (custLoadout[slot] && parseInt(custLoadout[slot].defIndex || 0, 10)) || 0;
      if (held > 0) return held;
      var ws = weaponsForSlot(slot);
      return ws.length ? (parseInt(ws[0][1], 10) || 0) : 0;
    }
    function weaponLabelForDef(def) {
      def = parseInt(def || 0, 10) || 0;
      if (def <= 0) return 'weapon';
      var slot = WEAPON_SLOT_BY_DEF[def] || '';
      if (slot === 'primary' || slot === 'secondary') {
        var ws = weaponsForSlot(slot);
        for (var i = 0; i < ws.length; i++) if (parseInt(ws[i][1], 10) === def) return ws[i][0];
      }
      return 'Weapon ' + def;
    }
    // Route finish edits to the ORIGINAL owner when the slot's held weapon is a pickup and the
    // dropdown is editing that exact weapon. Keyed off the SLOT's entity-derived ownership
    // (slotPickup), with the legacy active-weapon fields kept only as a fallback, so switching to a
    // grenade/knife mid-edit no longer flips the routing (or the warning) on and off.
    function shouldRoutePickupToOwner(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return false;
      var held = (custLoadout[slot] && parseInt(custLoadout[slot].defIndex || 0, 10)) || 0;
      if (slotPickup(slot)) return held > 0 && effectiveSlotDef(slot) === held;
      if (!custActiveWeaponPickup || !custActiveWeaponOwnerSteam || custActiveWeaponDef <= 0) return false;
      if (custActiveWeaponSlot !== slot) return false;
      return effectiveSlotDef(slot) === custActiveWeaponDef;
    }
    function pickupOwnerSteam(slot) {
      if (slotPickup(slot)) return slotOwnerSteam(slot);
      return custActiveWeaponOwnerSteam;
    }
    function pickupOwnerName(slot) {
      if (slotPickup(slot)) return slotOwnerName(slot);
      return custActiveWeaponOwnerName || (custActiveWeaponOwnerSteam ? ('Player ' + custActiveWeaponOwnerSteam) : '');
    }
    function resolveCosmeticTarget(slot) {
      var target = (custTargetKey && custTargetKey.indexOf('steam:') === 0)
        ? custTargetKey.substring(6) : 'current';
      if (shouldRoutePickupToOwner(slot)) return pickupOwnerSteam(slot) || target;
      return target;
    }
    // The banner + per-slot "!" badges are driven by SLOT ownership, so they stay up while the
    // player waves a grenade/knife around and only clear when the pickup is actually gone.
    function pickupSlots() {
      var out = [];
      if (slotPickup('primary')) out.push('primary');
      if (slotPickup('secondary')) out.push('secondary');
      return out;
    }
    // slot -> { row, lbl } warning badges; created alongside the PRIMARY/SECONDARY blocks below
    // (pickupWarnRow), updated here every time the loadout state refreshes.
    var custPickupBadges = {};
    function updatePickupBadges() {
      var slots = ['primary', 'secondary'];
      for (var i = 0; i < slots.length; i++) {
        var s = slots[i], b = custPickupBadges[s];
        if (!b || !b.row || !b.row.IsValid || !b.row.IsValid()) continue;
        var on = slotPickup(s);
        b.row.visible = on;
        if (on) b.lbl.text = 'Picked up from ' + slotOwnerName(s) + ' - finish edits apply to their loadout.';
      }
    }
    function updatePickupBanner() {
      var slots = pickupSlots();
      if (!slots.length) {
        custPickupBanner.visible = false;
        updatePickupBadges();
        logPickupState('banner');
        return;
      }
      var parts = [];
      for (var i = 0; i < slots.length; i++) {
        var s = slots[i];
        var wn = weaponLabelForDef((custLoadout[s] && custLoadout[s].defIndex) || 0);
        parts.push(slotOwnerName(s) + '\'s ' + wn);
      }
      custPickupLbl.text = 'Carrying ' + parts.join(' and ') + ' (picked up). Finish changes apply to the original owner\'s loadout, not ' + (custTargetName || 'this player') + '.';
      custPickupBanner.visible = true;
      updatePickupBadges();
      logPickupState('banner');
    }
    // Temporary debug: one console line whenever the pickup/warning state changes, so netcon +
    // mvm_debug can verify behavior across weapon switches, player switches, and seeks.
    var lastPickupLogSig = '';
    function logPickupState(tag) {
      try {
        function slotDbg(s) {
          var w = custLoadout[s];
          if (!w) return s + '=none';
          return s + '=def' + w.defIndex + (w.pickup ? (' PICKUP owner=' + w.ownerSteamId + '(' + (w.ownerName || '?') + ')') : ' owned');
        }
        var sig = '[mvmpickup] target=' + custTargetKey + '(' + custTargetName + ')' +
          ' activeDef=' + custActiveWeaponDef + ' activeSlot=' + (custActiveWeaponSlot || '-') +
          ' activePickup=' + (custActiveWeaponPickup ? 1 : 0) +
          ' ' + slotDbg('primary') + ' ' + slotDbg('secondary') +
          ' warn=[' + pickupSlots().join(',') + '] banner=' + (custPickupBanner.visible ? 1 : 0);
        if (sig === lastPickupLogSig) return;
        lastPickupLogSig = sig;
        $.Msg(sig + ' (' + tag + ')');
      } catch (e) {}
    }
    function filteredOptions(slot) {
      var arr = COSMETICS[slot] || [];
      if (slot === 'primary' || slot === 'secondary') {
        var slotDef = effectiveSlotDef(slot);
        if (slotDef <= 0) return [];
        var weaponOut = [];
        for (var wi = 0; wi < arr.length; wi++) {
          var wm = arr[wi][2] || {};
          var wPaint = parseInt(wm.paint || wm.paintKit || 0, 10) || 0;
          var wDef = parseInt(wm.def || 0, 10) || 0;
          if ((wPaint === 0 && (wDef === 0 || wDef === slotDef)) || (wPaint !== 0 && wDef === slotDef))
            weaponOut.push(arr[wi]);
        }
        return weaponOut;
      }
      if (slot === 'knife') return knifeOptionsWithCurrentDefault(arr);
      if (slot !== 'agent') return arr;
      var team = normalizeTeamName(custTargetTeam);
      var out = [];
      for (var i = 0; i < arr.length; i++) {
        var meta = arr[i][2] || {};
        if (!team || meta.team === 'ANY' || meta.team === team) out.push(arr[i]);
      }
      return out.length ? out : arr;
    }
    function knifeNameForDef(defIndex, arr) {
      if (defIndex === 42) return 'Default CT knife';
      if (defIndex === 59) return 'Default T knife';
      for (var i = 0; i < arr.length; i++) {
        var meta = arr[i][2] || {};
        if ((parseInt(meta.def || 0, 10) || 0) !== defIndex) continue;
        var label = arr[i][0] || '';
        var split = label.indexOf(' | ');
        return split > 0 ? label.substring(0, split) : label;
      }
      return 'Knife #' + defIndex;
    }
    function knifeOptionsWithCurrentDefault(arr) {
      var w = custLoadout.knife || null;
      var def = w ? (parseInt(w.defIndex || 0, 10) || 0) : 0;
      if (def <= 0) return arr;
      var value = def + ':0';
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return arr;
      var out = [[knifeNameForDef(def, arr), value, { color: rarColor(0), def: def, paint: 0 }]];
      for (var j = 0; j < arr.length; j++) out.push(arr[j]);
      return out;
    }
    function clampWearForMeta(meta, value) {
      var lo = (meta && typeof meta.wearMin === 'number') ? meta.wearMin : 0.0;
      var hi = (meta && typeof meta.wearMax === 'number') ? meta.wearMax : 1.0;
      value = clamp01(value);
      if (value < lo) value = lo;
      if (value > hi) value = hi;
      return value;
    }
    function wearValue(slot) {
      var w = custWear[slot] || (custWear[slot] = { preset: 'fn', custom: 0.01 });
      var preset = WEAR_PRESETS[w.preset] || WEAR_PRESETS.fn;
      var raw = (w.preset === 'custom') ? num(w.custom) : preset.value;
      var opt = selOpt(slot, custSel[slot]);
      return clampWearForMeta(opt ? opt[2] : null, raw);
    }
    function wearPresetLabel(slot) {
      var w = custWear[slot] || {};
      return (WEAR_PRESETS[w.preset] || WEAR_PRESETS.fn).label;
    }
    function withWearLabel(slot, label) {
      if (!label || !custWear[slot]) return label || '';
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      if (!meta.paint) return label;
      var suffix = wearPresetLabel(slot);
      if ((custWear[slot] || {}).preset === 'custom') suffix = 'Float ' + wearValue(slot).toFixed(2);
      return label + ' · ' + suffix;
    }
    function defaultWearForSlot(slot) {
      return slot === 'gloves' ? { preset: 'ft', custom: 0.22 } : { preset: 'fn', custom: 0.01 };
    }
    function presetForWearValue(value) {
      value = clamp01(num(value));
      if (value < 0.07) return 'fn';
      if (value < 0.15) return 'mw';
      if (value < 0.38) return 'ft';
      if (value < 0.45) return 'ww';
      return 'bs';
    }
    function copyWearState(w, slot) {
      var d = defaultWearForSlot(slot);
      w = w || d;
      var custom = (typeof w.custom === 'number' && isFinite(w.custom)) ? w.custom : d.custom;
      return { preset: w.preset || d.preset, custom: custom };
    }
    function itemWearKey(slot, value) {
      return slot + ':' + (value === undefined || value === null ? (custSel[slot] || '0') : ('' + value));
    }
    function saveActiveItemWear(slot) {
      if (!wearControls || !wearControls[slot]) return;
      custItemWear[itemWearKey(slot)] = copyWearState(custWear[slot], slot);
    }
    function loadActiveItemWear(slot) {
      if (!wearControls || !wearControls[slot]) return;
      var key = itemWearKey(slot);
      custWear[slot] = copyWearState(custItemWear[key], slot);
    }
)EDJS"
R"EDJS(
    function decoratedOptions(slot) {
      var src = filteredOptions(slot), out = [];
      for (var i = 0; i < src.length; i++) {
        var meta = src[i][2] || {};
        var m = {}; for (var k in meta) m[k] = meta[k];
        out.push([withWearLabel(slot, src[i][0]), src[i][1], m]);
      }
      return out;
    }
)EDJS"
R"EDJS(
    function optionExists(slot, value) {
      var arr = filteredOptions(slot);
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return true;
      return false;
    }
    function loadoutDefaultSelection(slot) {
      var w = custLoadout[slot] || null;
      if (!w || !(parseInt(w.defIndex || 0, 10) > 0)) return '0';
      var def = parseInt(w.defIndex || 0, 10) || 0;
      var paint = parseInt(w.paintKit || 0, 10) || 0;
      if ((slot === 'primary' || slot === 'secondary') && paint > 0 && optionExists(slot, '' + paint)) return '' + paint;
      if ((slot === 'primary' || slot === 'secondary') && optionExists(slot, def + ':0')) return def + ':0';
      if (slot === 'knife' && paint > 0 && optionExists(slot, def + ':' + paint)) return def + ':' + paint;
      if (slot === 'knife' && optionExists(slot, def + ':0')) return def + ':0';
      return '0';
    }
    // Gloves: if the spectated player wears custom gloves, default to that glove type; otherwise
    // fall back to the team's default gloves (CT 5029 / T 5028), per the "no gloves -> default by
    // side" rule. The exact glove finish isn't networked through the fallback fields, so a custom
    // pair resolves to the first finish of that glove model (its type is what matters here).
    function glovesDefaultSelection() {
      var arr = COSMETICS.gloves || [];
      var w = custLoadout.gloves || null;
      var def = w ? (parseInt(w.defIndex || 0, 10) || 0) : 0;
      var paint = w ? (parseInt(w.paintKit || 0, 10) || 0) : 0;
      if (def > 0 && paint > 0) {
        for (var i = 0; i < arr.length; i++) {
          var m = arr[i][2] || {};
          if ((parseInt(m.def || 0, 10) || 0) === def && (parseInt(m.paint || 0, 10) || 0) === paint) return arr[i][1];
        }
      }
      return (normalizeTeamName(custTargetTeam) === 'CT') ? '5029:0' : '5028:0';
    }
    function agentDefaultSelection() {
      // Match the player's live model path (read off the pawn, e.g.
      // 'agents/models/ctm_st6/ctm_st6_variantg.vmdl') to a catalog agent by its meta.model.
      // NOTE: the catalog key is SINGULAR 'agent' (matching the slot name), not 'agents'.
      var model = custLoadout.agentModel || '';
      if (model) {
        var arr = COSMETICS.agent || [];
        for (var i = 0; i < arr.length; i++) {
          var m = arr[i][2] || {};
          if (m.model && m.model === model) return arr[i][1];
        }
      }
      return 'default';
    }
    function resetSelectionsFromLoadout() {
      custWeaponDef = { primary: 0, secondary: 0 }; // follow held weapons for a freshly-opened player
      custActiveCategory = initialActiveCategory();
      custSel = {
        agent: agentDefaultSelection(),
        primary: loadoutDefaultSelection('primary'),
        secondary: loadoutDefaultSelection('secondary'),
        knife: loadoutDefaultSelection('knife'),
        gloves: glovesDefaultSelection()
      };
      custWear = {
        primary: defaultWearForSlot('primary'),
        secondary: defaultWearForSlot('secondary'),
        knife: defaultWearForSlot('knife'),
        gloves: defaultWearForSlot('gloves')
      };
      for (var slot in custLoadout) {
        var w = custLoadout[slot];
        if (!w || !(parseInt(w.paintKit || 0, 10) > 0)) continue;
        var wear = clamp01(num(w.wear));
        custWear[slot] = { preset: presetForWearValue(wear), custom: wear };
      }
      custItemWear = {};
      saveActiveItemWear('primary');
      saveActiveItemWear('secondary');
      saveActiveItemWear('knife');
      saveActiveItemWear('gloves');
    }

    // RIGHT: final loadout-style controls. No arms/viewmodel override and no CT/T glove split.
)EDJS"
R"EDJS(
    // Right column is a clipped VIEWPORT; its content lives in ctrlColInner, which is transform-scrolled
    // by custWheel (the wheel is consumed in C++ and routed in, so native `overflow: scroll` can't be
    // used -- Panorama never sees the raw wheel). sizeColScroll() clamps the offset every frame.
    var ctrlCol = mk('Panel', custBody); ctrlCol.style.width = 'fill-parent-flow(1.0)'; ctrlCol.style.height = '100%';
    ctrlCol.style.overflow = 'clip'; ctrlCol.hittest = true;
    var ctrlColInner = mk('Panel', ctrlCol); ctrlColInner.style.width = '100%'; ctrlColInner.style.flowChildren = 'down';
    // Wear picker: a labeled 'wear' kind itemDrop (value text + meter bar, click opens the preset
    // popup) plus the custom-float TextEntry (shown only for the 'custom' preset). No mouse-move drag
    // here -- the meter is a read-only indicator; changing wear stays dropdown/text-entry driven, per
    // the in-game-HUD "no mouse-move event" constraint (see CameraEditorWidgetsJs.h customDrop notes).
    function wearBlock(parent, slot, category) {
      var wrap = mk('Panel', parent); wrap.style.width = '100%'; wrap.style.flowChildren = 'down';
      var preset = itemDrop(wrap, 'CustWear' + slot, function (v) { setWear(slot, v, null); },
        { rowLabel: 'Wear', kind: 'wear', searchable: false, category: category });
      // The float box is DISPLAY-ONLY as far as Panorama focus is concerned: typed characters come
      // from the C++ WM_CHAR route (custChar -> activeWearFloat), never from TextEntry keyboard
      // focus. hittest=false keeps clicks from ever focusing the TextEntry -- focusing it made the
      // engine restyle/collapse the field ("the box shrinks when you type"). A transparent cover
      // panel takes the click instead: it selects this slot as the typing target and arms
      // fresh-typing (next digit REPLACES the stamped value instead of appending to it).
      var box = mk('Panel', wrap); box.style.width = '100%'; box.style.height = '32px'; box.style.marginBottom = '8px';
      var custom = $.CreatePanel('TextEntry', box, 'CustWearFloat' + slot, { placeholder: '0.00 - 1.00' });
      custom.hittest = false;
      custom.style.width = '100%'; custom.style.height = '100%';
      custom.style.backgroundColor = '#101823'; custom.style.border = '1px solid #ffffff24';
      custom.style.color = S.value; custom.style.fontSize = '14px'; custom.style.paddingLeft = '8px';
      custom.__box = box;
      var cover = mk('Panel', box); cover.hittest = true; cover.style.width = '100%'; cover.style.height = '100%';
      cover.SetPanelEvent('onactivate', function () {
        if (category) setActiveCategory(category);
        custom.__fresh = true;
      });
      return { preset: preset, custom: custom };
    }
    // Agent: a single prominent row, no label cell and no wear (agents aren't skinned) -- matches the
    // reference mock's top agent card. Everything else groups into a titled card via the shared
    // section() helper (CameraEditorWidgetsJs.h), reusing the same chrome the rest of the editor uses.
    var agentDrop = itemDrop(ctrlColInner, 'CustAgent', function (v) { pickCosmetic('agent', v); }, { height: '58px' });
    agentDrop.dd.style.marginBottom = '18px';
    // Per-slot "picked up" warning: a small circled "!" + owner note right under the slot's Weapon
    // row. Driven by slotPickup() (entity ownership), NOT the active weapon, so it stays visible
    // while the player has a grenade/knife/bomb out and only clears when the pickup is gone.
    function pickupWarnRow(parent) {
      var row = mk('Panel', parent); row.visible = false; row.hittest = false;
      row.style.width = '100%'; row.style.flowChildren = 'right'; row.style.marginTop = '5px';
      var ic = mk('Panel', row); ic.hittest = false; ic.style.width = '16px'; ic.style.height = '16px';
      ic.style.borderRadius = '8px'; ic.style.backgroundColor = S.accent; ic.style.verticalAlign = 'center'; ic.style.marginRight = '8px';
      var il = lbl(ic, '!', '#241a06ff', 11); il.hittest = false; il.style.fontWeight = 'bold';
      il.style.width = '100%'; il.style.textAlign = 'center'; il.style.verticalAlign = 'center';
      var tx = lbl(row, '', '#ffdba6', 11); tx.hittest = false; tx.style.width = 'fill-parent-flow(1.0)';
      tx.style.whiteSpace = 'normal'; tx.style.verticalAlign = 'center';
      return { row: row, lbl: tx };
    }
    var primaryBlock = section(ctrlColInner, 'PRIMARY');
    var primaryWeaponDrop = itemDrop(primaryBlock, 'CustPrimaryWeapon', function (v) { pickWeapon('primary', v); }, { rowLabel: 'Weapon', category: 'primary' });
    custPickupBadges.primary = pickupWarnRow(primaryBlock);
    var primaryDrop = itemDrop(primaryBlock, 'CustPrimary', function (v) { pickCosmetic('primary', v); }, { rowLabel: 'Finish', showWearTag: true, category: 'primary' });
    var primaryWear = wearBlock(primaryBlock, 'primary', 'primary');
    var secondaryBlock = section(ctrlColInner, 'SECONDARY');
    var secondaryWeaponDrop = itemDrop(secondaryBlock, 'CustSecondaryWeapon', function (v) { pickWeapon('secondary', v); }, { rowLabel: 'Weapon', category: 'secondary' });
    custPickupBadges.secondary = pickupWarnRow(secondaryBlock);
    var secondaryDrop = itemDrop(secondaryBlock, 'CustSecondary', function (v) { pickCosmetic('secondary', v); }, { rowLabel: 'Finish', showWearTag: true, category: 'secondary' });
    var secondaryWear = wearBlock(secondaryBlock, 'secondary', 'secondary');
    var knifeBlock = section(ctrlColInner, 'MELEE / KNIFE');
    var knifeDrop = itemDrop(knifeBlock, 'CustKnife', function (v) { pickCosmetic('knife', v); }, { rowLabel: 'Weapon', showWearTag: true, category: 'knife' });
    var knifeWear = wearBlock(knifeBlock, 'knife', 'knife');
    var glovesBlock = section(ctrlColInner, 'GLOVES');
    var glovesDrop = itemDrop(glovesBlock, 'CustGloves', function (v) { pickCosmetic('gloves', v); }, { rowLabel: 'Gloves' });
    var glovesWear = wearBlock(glovesBlock, 'gloves');
    var wearControls = { primary: primaryWear, secondary: secondaryWear, knife: knifeWear, gloves: glovesWear };
    var wearDrops = { primary: primaryWear.preset, secondary: secondaryWear.preset, knife: knifeWear.preset, gloves: glovesWear.preset };
    // Selected option's [label, meta] for a slot (for the preview).
    function selOpt(slot, value) {
      var arr = filteredOptions(slot);
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return arr[i];
      return arr.length ? arr[0] : null;
    }
    function coerceSelection(slot) {
      if ((slot === 'primary' || slot === 'secondary') && !(custLoadout[slot] && custLoadout[slot].defIndex > 0)) {
        custSel[slot] = '0';
        return;
      }
      var arr = filteredOptions(slot);
      if (!arr.length) return;
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === custSel[slot]) return;
      saveActiveItemWear(slot);
      custSel[slot] = arr[0][1];
      loadActiveItemWear(slot);
    }
    // Preview refresh -> drive the 3D character (or the 2D fallback). applyPreview() is defined in
    // the preview block above.
    function updatePreview() { applyPreview(); }
    function updateWearControl(slot) {
      var wc = wearControls[slot]; if (!wc) return;
      var w = custWear[slot] || (custWear[slot] = { preset: 'fn', custom: 0.01 });
      wc.preset.update(WEAR_OPTIONS, w.preset);
      var showFloat = w.preset === 'custom';
      if (wc.custom.__box) wc.custom.__box.visible = showFloat; else wc.custom.visible = showFloat;
      var val = wearValue(slot);
      // Only rewrite the custom-float box when the number it shows actually differs from the target
      // AND the user is not mid-typing (__fresh === false means keystrokes arrived since the last
      // programmatic stamp -- re-stamping the clamped value between keystrokes made typing "0.4"
      // impossible). The COMMANDED wear is always the clamped wearValue(); the box display catches
      // up on the next preset/slot action.
      var curNum = parseFloat(wc.custom.text);
      if (wc.custom.__fresh !== false && (!isFinite(curNum) || Math.abs(curNum - val) > 0.001)) {
        custWearUpdating = true;
        wc.custom.text = val.toFixed(2);
        custWearUpdating = false;
        wc.custom.__fresh = true;
      }
      var opt = selOpt(slot, custSel[slot]), meta = opt ? (opt[2] || {}) : {};
      var lo = (typeof meta.wearMin === 'number') ? meta.wearMin : 0.0;
      var hi = (typeof meta.wearMax === 'number') ? meta.wearMax : 1.0;
      var frac = (hi > lo) ? ((val - lo) / (hi - lo)) : val;
      wc.preset.updateWear(wearPresetLabel(slot) + ' (' + val.toFixed(2) + ')', frac, meta.paint ? meta.color : S.accent);
    }
    function setWear(slot, preset, customValue) {
      if (!custWear[slot]) custWear[slot] = { preset: 'fn', custom: 0.01 };
      if (preset) custWear[slot].preset = preset;
      if (customValue !== null && customValue !== undefined) custWear[slot].custom = clamp01(customValue);
      // A preset picked from the dropdown (no explicit custom value) re-arms the float box for a
      // fresh programmatic stamp; a typed custom value must NOT (the user is mid-edit).
      if ((customValue === null || customValue === undefined) && wearControls && wearControls[slot])
        wearControls[slot].custom.__fresh = true;
      saveActiveItemWear(slot);
      markTouched(slot);
      populateCustomize();
      updatePreview();
    }
    // Sends the actual "mirv_filmmaker cosmetics player ..." command for one slot's CURRENT selection.
    // Called from commitTouchedSlots() (Apply / Reset-to-Default / Clear-All) -- NOT from every
    // pick/wear change anymore; the modal now stages edits and only writes to the demo on Apply (or
    // the immediate Reset/Clear actions), matching the reference mock's Apply/Cancel action bar.
    function applyCosmeticCommand(slot) {
      // The agent slot has no wear entry (agents are not skinned), so it must NOT be gated on
      // custWear[slot] -- doing so silently dropped every agent pick (no command was ever sent).
      if (custTargetIndex < 0) return;
      if (slot !== 'agent' && !custWear[slot]) return;
      // Target the player by SteamID -- the backend store is SteamID-keyed. Picked-up weapons keep the
      // original owner's econ id, so route finish commands to that owner when editing the held pickup.
      var target = resolveCosmeticTarget(slot);
      var routedPickup = shouldRoutePickupToOwner(slot);
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      var def = parseInt(meta.def || 0, 10) || 0;
      var pk = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
      var wear = wearValue(slot).toFixed(4);
      // Echo the VERBATIM skin label the user clicked (exactly the dropdown text, e.g. "AK-47 |
      // Redline" / "Skeleton Knife") to the game console + MVM debug log, so the human-readable name
      // appears right next to the native before/after weapon snapshot. Strip characters that would
      // break the console-command tokenizer; the native "uilog" handler joins the tokens back.
      var label = (opt && opt[0]) ? ('' + opt[0]) : '(unknown)';
      var safeLabel = label.replace(/["';\r\n]+/g, ' ').replace(/\s+/g, ' ');
      var routeNote = routedPickup ? (' [owner ' + pickupOwnerSteam(slot) + ']') : '';
      cmd('mirv_filmmaker cosmetics uilog [' + slot + '] ' + safeLabel + routeNote + ' (def ' + def + ' paint ' + pk + ' wear ' + wear + ')');
      var sent = false;
      var command = '';
      if ((slot === 'primary' || slot === 'secondary') && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' weapon ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'knife' && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' knife ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'gloves' && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' gloves ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'agent') {
        command = 'mirv_filmmaker cosmetics player ' + target + ' agent ' + (meta.model || 'default');
        sent = true;
      }
      // Enable before sending the profile mutation so normal UI use does not emit the temporary
      // "cosmetics are disabled" hint from the native command handler.
      if (sent) {
        cmd('mirv_filmmaker cosmetics enabled 1');
        cmd(command);
      }
    }
    // Apply a selection. Updates the UI + local preview immediately (stages the pending change); the
    // actual demo-facing command only fires when the user commits via Apply / Reset to Default /
    // Clear All (commitTouchedSlots()) -- see markTouched().
    function pickCosmetic(slot, value) {
      if (slot === 'primary' || slot === 'secondary' || slot === 'knife') custActiveCategory = slot;
      saveActiveItemWear(slot);
      custSel[slot] = value;
      loadActiveItemWear(slot);
      markTouched(slot);
      populateCustomize();
      updatePreview();
      previewLog('pick', 'slot=' + slot + ' value=' + value);
    }
    // Re-target which weapon a primary/secondary block edits (the weapon selector). Resets the finish
    // dropdown to that weapon's vanilla default and repopulates; does NOT stage a pending change on its
    // own -- only picking a FINISH (or Wear) does (so merely browsing weapons changes nothing in the demo).
    function pickWeapon(slot, value) {
      if (slot !== 'primary' && slot !== 'secondary') return;
      var def = parseInt(value, 10) || 0;
      if (def <= 0) return;
      custActiveCategory = slot;
      saveActiveItemWear(slot);
      custWeaponDef[slot] = def;
      custSel[slot] = optionExists(slot, def + ':0') ? (def + ':0') : '0';
      loadActiveItemWear(slot);
      populateCustomize();
      updatePreview();
      previewLog('pickWeapon', 'slot=' + slot + ' def=' + def);
    }
    function populateCustomize() {
      coerceSelection('agent');
      coerceSelection('primary');
      coerceSelection('secondary');
      coerceSelection('knife');
      coerceSelection('gloves');
      // Primary/secondary blocks are ALWAYS shown now: the weapon selector lets the user assign a skin
      // to any weapon, including ones the player is not currently holding.
      primaryBlock.visible = true;
      secondaryBlock.visible = true;
      knifeBlock.visible = true;
      agentDrop.update(decoratedOptions('agent'), custSel.agent);
      primaryWeaponDrop.update(weaponsForSlot('primary'), '' + effectiveSlotDef('primary'));
      secondaryWeaponDrop.update(weaponsForSlot('secondary'), '' + effectiveSlotDef('secondary'));
      primaryDrop.update(decoratedOptions('primary'), custSel.primary);
      secondaryDrop.update(decoratedOptions('secondary'), custSel.secondary);
      knifeDrop.update(decoratedOptions('knife'), custSel.knife);
      glovesDrop.update(decoratedOptions('gloves'), custSel.gloves);
      updateWearControl('primary');
      updateWearControl('secondary');
      updateWearControl('knife');
      updateWearControl('gloves');
      updatePickupBanner();
      updateActionBar();
    }
)EDJS"
R"EDJS(
    // ---- Manual scroll + C++-routed typing --------------------------------------------------
    // The modal is an EXCLUSIVE input surface: MovieMode/main.cpp swallow the raw wheel + typed keys
    // from the game (so nothing leaks to the demo behind the modal) and forward them here via
    // $.CamEditor.custWheel / custChars. Because Panorama never sees the raw wheel, scrolling is done
    // by translating the content inside a clipped viewport; because CS2 doesn't give the in-game HUD
    // TextEntry real keyboard focus, the search string is built from the forwarded characters.
    function custMeasureH(panel) {
      if (!panel || !panel.IsValid || !panel.IsValid()) return 0;
      var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      return (panel.actuallayoutheight || 0) / (sy || 1);
    }
    // Apply a style value only when it changed, so per-frame maintenance doesn't thrash layout.
    function setStyleIf(panel, key, prop, value) {
      if (panel['__' + key] === value) return;
      panel['__' + key] = value;
      panel.style[prop] = value;
    }
    function sizeColScroll() {
      if (!custOverlay.visible || !ctrlColInner || !ctrlColInner.IsValid || !ctrlColInner.IsValid()) return;
      var viewH = custMeasureH(ctrlCol), contentH = custMeasureH(ctrlColInner);
      var maxOff = Math.max(0, contentH - viewH);
      if (custColScroll.off > maxOff) custColScroll.off = maxOff;
      if (custColScroll.off < 0) custColScroll.off = 0;
      setStyleIf(ctrlColInner, 'sy', 'transform', 'translateY(' + (-Math.round(custColScroll.off)) + 'px)');
    }
    function sizeOpenDrop() {
      var rec = openDrop;
      if (!rec || !rec.pop || !rec.pop.visible || !rec.rowsView || !rec.rows) return;
      var contentH = custMeasureH(rec.rows);
      var viewH = (contentH > 0) ? Math.min(CUST_DROP_VIEW_MAX, contentH) : CUST_DROP_VIEW_MAX;
      setStyleIf(rec.rowsView, 'vh', 'height', Math.round(viewH) + 'px');
      var maxOff = Math.max(0, contentH - viewH);
      if (rec.scrollOff > maxOff) rec.scrollOff = maxOff;
      if (rec.scrollOff < 0) rec.scrollOff = 0;
      setStyleIf(rec.rows, 'sy', 'transform', 'translateY(' + (-Math.round(rec.scrollOff)) + 'px)');
    }
    // Every render() frame while the editor is up: keep the manual-scroll viewports sized to their
    // (possibly changed) content and re-clamp offsets. Cheap no-op when the modal is closed.
    function custScrollMaintain() {
      if (!custOverlay.visible) return;
      sizeColScroll();
      sizeOpenDrop();
    }
    // Wheel notch forwarded from C++ (MovieMode swallowed the raw wheel). +1 = up, -1 = down. Over
    // the character preview it zooms; otherwise it scrolls the open dropdown or right control column.
    function custWheel(notches, x, y) {
      if (!custOverlay.visible) return 'closed';
      var n = parseInt(notches, 10) || 0;
      if (!n) return 'ok';
      if (overPreview(parseFloat(x), parseFloat(y))) {
        zoomPreview(n > 0 ? +1 : -1);
        previewLog('zoom', 'notches=' + n + ' cursor=' + x + ',' + y);
        return 'zoom';
      }
      var d = -n * CUST_SCROLL_STEP; // n>0 (wheel up) -> toward top -> smaller offset; scales with notch count
      var rec = (openDrop && openDrop.pop && openDrop.pop.visible && openDrop.rowsView) ? openDrop : null;
      if (rec) { rec.scrollOff += d; sizeOpenDrop(); }
      else { custColScroll.off += d; sizeColScroll(); }
      return 'ok';
    }
    // The custom-float wear box of the slot currently being edited, when it's on screen -- the target
    // for typed numbers while no dropdown is open (agents/skins are picked from dropdowns; only wear
    // is free-typed). Returns null if there's nothing to type into.
    function activeWearFloat() {
      var wc = wearControls && wearControls[custActiveCategory];
      if (!wc || !wc.custom || !wc.custom.IsValid || !wc.custom.IsValid()) return null;
      var shown = wc.custom.__box ? wc.custom.__box.visible : wc.custom.visible;
      if (shown) return { slot: custActiveCategory, panel: wc.custom };
      return null;
    }
    // One forwarded character (WM_CHAR code). A searchable OPEN dropdown takes text; otherwise a
    // visible custom-float wear box takes digits. Escape backs out (close dropdown, else close modal)
    // so the user is never trapped with keys captured.
    function custChar(code) {
      if (code === 27) { if (openDrop) closeAllDrops(); else closeCustomize(); return; }
      var rec = (openDrop && openDrop.searchable) ? openDrop : null;
      if (rec) {
        if (code === 8) { rec.query = (rec.query || '').slice(0, -1); }
        else if (code === 13) { return; }
        else if (code >= 32 && code !== 127) { rec.query = (rec.query || '') + String.fromCharCode(code).toLowerCase(); }
        else { return; }
        if (rec.search && rec.search.IsValid && rec.search.IsValid()) {
          try { custSearchDisplayGuard = true; rec.search.text = rec.query; custSearchDisplayGuard = false; } catch (e) {}
        }
        if (rec.rebuildRows) rec.rebuildRows();
        rec.scrollOff = 0; sizeOpenDrop();
        return;
      }
      var f = activeWearFloat();
      if (!f) return;
      var t = '';
      try { t = f.panel.text || ''; } catch (e) { t = ''; }
      if (code === 8) t = t.slice(0, -1);
      else if ((code >= 48 && code <= 57) || code === 46) { // 0-9 and '.'
        // Fresh-typing: the first character after a programmatic stamp (preset switch, clamp,
        // slot change, clicking the box) REPLACES the shown value -- appending to "0.40" gave
        // nonsense like "0.40.5".
        if (f.panel.__fresh !== false) t = '';
        t = t + String.fromCharCode(code);
      }
      else return;
      f.panel.__fresh = false;
      custWearUpdating = true; try { f.panel.text = t; } catch (e2) {} custWearUpdating = false;
      var v = parseFloat(t);
      if (isFinite(v)) setWear(f.slot, 'custom', v);
    }
    function custChars(csv) {
      if (!custOverlay.visible || csv == null) return 'closed';
      var parts = ('' + csv).split(',');
      for (var i = 0; i < parts.length; i++) { var code = parseInt(parts[i], 10); if (code >= 0) custChar(code); }
      return 'ok';
    }
)EDJS"
R"EDJS(
    // Tells the C++ side (CameraEditorHud::UpdateCustomizeModalState, "customizeopen" attribute)
    // whether the modal is open, so MovieMode/GetSuspendMirvInput can treat it as an exclusive
    // input surface (swallow Space/clicks/wheel, suspend free-cam mouse-look) -- see Filmmaker.h
    // CameraEditor_CustomizeModalOpen(). Published on every open/close AND every render() frame
    // as a safety net, since custOverlay.visible also flips from other paths (editor disabled,
    // target no longer a player, etc.) that don't go through openCustomize/closeCustomize.
    function publishCustomizeOpenState() {
      try { root.SetAttributeString('customizeopen', custOverlay.visible ? '1' : '0'); } catch (e) {}
    }
    function openCustomize() {
      closeAllDrops(); closeSettings();
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      resetSelectionsFromLoadout();
      custTitleName.text = (custTargetName || 'PLAYER').toUpperCase();
      populateCustomize();
      custDisplayedLoadoutSig = customizeLoadoutSignature();
      custOverlay.visible = true; // visible BEFORE building the 3D preview so its scene composites
      previewNudgeArmed = true;
      previewLog('open');
      // CREATE the 3D panel only AFTER the overlay is visible AND laid out (next layout passes). Do
      // not call updatePreview() eagerly here: that creates a throwaway panel, then the delayed
      // recreate replaces it and can leave the final render target black. One delayed create gets
      // one Panorama-only composition refresh.
      try { $.Schedule(0.18, recreatePreview); } catch (e) { recreatePreview(); }
      publishCustomizeOpenState();
    }
    function closeCustomize() {
      // closeCustomize() is also called EVERY frame as a safety net while the modal "shouldn't"
      // be open (render()'s `if (!cur)` branch, and its `if (!onPlayer)` branch whenever the mirv
      // free cam is on -- which stays on after a Follow "Live" preview stops, or after any camera
      // path place/preview). When the modal is already closed this must be a TRUE no-op: the
      // unconditional closeAllDrops() below was tearing down every OTHER customDrop popup (Follow
      // inspector Type/Target/Attach/Source/Event) on the very frame it opened, which read as
      // "the dropdowns stop working after leaving Live mode".
      if (!custOverlay.visible) return;
      closeAllDrops();
      destroyPreview3d();
      custOverlay.visible = false;
      custResetConfirm.visible = false;
      // Closing (X / click-outside / Cancel) always discards whatever hasn't been committed via
      // Apply/Reset/Clear -- the modal never writes to the demo except through those three paths.
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      publishCustomizeOpenState();
      previewLog('close'); // always a REAL close now (the early-return above filters no-ops)
    }
    var customizeDrops = { agent: agentDrop, primary: primaryDrop, secondary: secondaryDrop, knife: knifeDrop, gloves: glovesDrop };
)EDJS"
R"EDJS(
    // ===================== BOTTOM ACTION BAR =====================
    // Reset/Clear are immediate (they mirror picking every slot's vanilla default, resp. wiping the
    // whole stored profile, and commit right away like a native "reset" action). Finish/Wear/Agent/
    // Gloves picks above are STAGED (pickCosmetic/pickWeapon/setWear only update custSel/custWear +
    // the local preview) and only reach the demo via Apply -- Cancel/X/click-outside discard them
    // (closeCustomize() above clears custTouched/custDirty without ever calling applyCosmeticCommand).
    var custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
    var custDirty = false;
    function markTouched(slot) { custTouched[slot] = true; custDirty = true; updateActionBar(); }
    function commitTouchedSlots() {
      var any = false;
      for (var slot in custTouched) if (custTouched[slot]) { applyCosmeticCommand(slot); any = true; }
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      updateActionBar();
      return any;
    }
    // "Reset to Default" reverts the target to their ACTUAL recorded loadout (whatever skin/agent/
    // knife/gloves the demo captured), not to a blank "no skin" state -- it wipes every stored
    // override for this player (same backend action the old "Clear All" used) and reseeds the modal
    // from the live loadout, same as opening it fresh. There is no separate "Clear All": Cancel
    // already discards unstaged picks, so a second destructive button was redundant with this one.
    function resetToDefault() {
      closeAllDrops();
      var target = (custTargetKey && custTargetKey.indexOf('steam:') === 0) ? custTargetKey.substring(6) : 'current';
      cmd('mirv_filmmaker cosmetics enabled 1');
      cmd('mirv_filmmaker cosmetics clearPlayer ' + target);
      custWeaponDef = { primary: 0, secondary: 0 };
      custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
      custDirty = false;
      resetSelectionsFromLoadout();
      populateCustomize();
      updatePreview();
      updateActionBar();
    }
    // Who Apply would actually write to -- mirrors the pickup-banner logic (updatePickupBanner) so the
    // button label and the warning banner never disagree about the target.
    function applyLabelTarget() {
      var ps = pickupSlots();
      if (ps.length) return slotOwnerName(ps[0]);
      if (custActiveWeaponPickup && custActiveWeaponOwnerSteam) return custActiveWeaponOwnerName || ('Player ' + custActiveWeaponOwnerSteam);
      return custTargetName || 'Player';
    }
    function updateActionBar() {
      custApplyBtn.__lbl.text = 'APPLY TO ' + applyLabelTarget().toUpperCase();
      custApplyBtn.style.backgroundColor = custDirty ? S.accent : S.btnBg;
      custApplyBtn.__lbl.style.color = custDirty ? '#10141aff' : S.dim;
      custApplyBtn.hittest = custDirty;
    }
    function actionBtn(parent, text, color, textColor) {
      var b = mk('Panel', parent); b.hittest = true; b.style.verticalAlign = 'center';
      b.style.backgroundColor = color; b.style.borderRadius = '4px'; b.style.marginRight = '12px';
      b.style.paddingTop = '13px'; b.style.paddingBottom = '13px'; b.style.paddingLeft = '20px'; b.style.paddingRight = '20px';
      var l = lbl(b, text, textColor || S.value, 14); l.style.fontWeight = 'bold'; l.style.letterSpacing = '1px';
      b.__lbl = l;
      return b;
    }
    var custActionBar = mk('Panel', custWin); custActionBar.style.width = '100%'; custActionBar.style.height = '76px';
    custActionBar.style.flowChildren = 'right'; custActionBar.style.verticalAlign = 'center';
    custActionBar.style.paddingLeft = '26px'; custActionBar.style.paddingRight = '26px';
    custActionBar.style.backgroundColor = '#00000040'; custActionBar.style.borderTop = '1px solid #ffffff14';
    var custResetBtn = actionBtn(custActionBar, 'RESET TO DEFAULT', S.btnBg, '#ff8a8aff');
    var custActionSpacer = mk('Panel', custActionBar); custActionSpacer.style.width = 'fill-parent-flow(1.0)';
    var custApplyBtn = actionBtn(custActionBar, 'APPLY', S.accent, '#10141aff');
    var custCancelBtn = actionBtn(custActionBar, 'CANCEL', S.btnBg, S.value);
    custCancelBtn.style.marginRight = '0px';
    custCancelBtn.SetPanelEvent('onactivate', function () { closeCustomize(); });
    custApplyBtn.SetPanelEvent('onactivate', function () { if (custDirty) { commitTouchedSlots(); closeCustomize(); } });

    // Confirm dialog for "Reset to Default" -- it immediately wipes the target's WHOLE stored
    // cosmetic profile (not just the currently staged edits; Cancel already covers those), so it
    // gets one guard click. Mirrors the CLEAR ALL CAMERA PATHS confirm pattern in CameraEditorJs.h,
    // kept as its own instance here since that one is wired specifically to the path inspector.
    var custResetConfirm = mk('Panel', root); custResetConfirm.visible = false; custResetConfirm.hittest = true;
    custResetConfirm.style.width = '100%'; custResetConfirm.style.height = '100%'; custResetConfirm.style.zIndex = '260';
    custResetConfirm.style.backgroundColor = 'rgba(0,0,0,0.72)';
    custResetConfirm.SetPanelEvent('onactivate', function () { custResetConfirm.visible = false; });
    var custResetBox = mk('Panel', custResetConfirm); custResetBox.hittest = true;
    custResetBox.style.width = '480px'; custResetBox.style.horizontalAlign = 'center'; custResetBox.style.verticalAlign = 'center';
    custResetBox.style.backgroundColor = 'rgba(16,20,26,0.99)'; custResetBox.style.border = '1px solid ' + S.accent;
    custResetBox.style.borderRadius = '6px'; custResetBox.style.boxShadow = '#000000ee 0px 0px 28px 6px';
    custResetBox.style.flowChildren = 'down'; custResetBox.style.paddingTop = '24px'; custResetBox.style.paddingBottom = '22px';
    custResetBox.style.paddingLeft = '26px'; custResetBox.style.paddingRight = '26px';
    custResetBox.SetPanelEvent('onactivate', function () { /* swallow clicks inside the box */ });
    var custResetTitle = lbl(custResetBox, 'RESET TO DEFAULT?', S.value, 19); custResetTitle.style.fontWeight = 'bold';
    custResetTitle.style.letterSpacing = '1px'; custResetTitle.style.marginBottom = '10px';
    var custResetBody = lbl(custResetBox, '', S.label, 13); custResetBody.style.whiteSpace = 'normal'; custResetBody.style.marginBottom = '18px';
    var custResetActions = mk('Panel', custResetBox); custResetActions.style.flowChildren = 'right'; custResetActions.style.horizontalAlign = 'right';
    var custResetNo = btn(custResetActions, 'No', function () { custResetConfirm.visible = false; }, S.value);
    custResetNo.style.width = '110px'; custResetNo.__lbl.style.textAlign = 'center'; custResetNo.__lbl.style.width = '100%';
    var custResetYes = btn(custResetActions, 'Yes, reset', function () { custResetConfirm.visible = false; resetToDefault(); }, S.danger);
    custResetYes.style.width = '160px'; custResetYes.style.marginRight = '0px';
    custResetYes.__lbl.style.textAlign = 'center'; custResetYes.__lbl.style.width = '100%';
    custResetBtn.SetPanelEvent('onactivate', function () {
      custResetBody.text = 'This removes every skin, agent, knife, and glove override for ' + (custTargetName || 'this player') + ' and reverts to their original loadout.';
      custResetConfirm.visible = true;
    });
)EDJS"
R"EDJS(
    // Persistent "Customize" button pinned to the editor's BOTTOM-RIGHT corner. It is a child of
    // root (NOT the scrolling inspector), so it stays put across Path/Follow/Attach/Lock-on tabs
    // and is always visible while the editor is open (visibility set in render()).
    var customizeBtn = mk('Panel', root); customizeBtn.hittest = true; customizeBtn.visible = false;
    customizeBtn.style.horizontalAlign = 'right'; customizeBtn.style.verticalAlign = 'bottom';
    customizeBtn.style.marginRight = '16px'; customizeBtn.style.marginBottom = '16px';
    customizeBtn.style.zIndex = '120'; customizeBtn.style.backgroundColor = S.accent;
    customizeBtn.style.borderRadius = '5px';
    customizeBtn.style.paddingTop = '11px'; customizeBtn.style.paddingBottom = '11px';
    customizeBtn.style.paddingLeft = '22px'; customizeBtn.style.paddingRight = '22px';
    customizeBtn.style.boxShadow = '#000000aa 0px 3px 12px 2px';
    var customizeBtnLbl = lbl(customizeBtn, '✦  CUSTOMIZE PLAYER', '#10141aff', 15);
    customizeBtnLbl.style.fontWeight = 'bold'; customizeBtnLbl.style.letterSpacing = '1px';
    customizeBtn.SetPanelEvent('onactivate', function () { openCustomize(); });
)EDJS"
