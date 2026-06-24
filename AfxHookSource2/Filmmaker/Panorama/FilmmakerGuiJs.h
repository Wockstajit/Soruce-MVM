#pragma once

// Panorama JS that EDITS CS2's native "Watch matches and tournaments" tab in place:
//   * swaps the main-menu Watch nav button icon to the filmmaker (film) icon
//   * hides the "Majors"/Tournaments tab from the Watch navbar
//   * adds "Add demo folder" + "Refresh" icon buttons to the navbar (top-right)
//   * takes over the Downloaded tab body with OUR parsed demo data, rendered
//     ONE-TO-ONE with CS2's native Watch -> Downloaded page (left MatchList of
//     map tiles, right scoreboard with CT/T scoreboxes + per-round timeline).
//
// HOW THE NATIVE LOOK IS ACHIEVED (the important part):
//   The Watch page only loads mainmenu_watch.vcss. Panels we $.CreatePanel with
//   matchtile/matchinfo/scoreboard classes therefore get NO styling at page
//   level (that was the cause of the overlapping/unstyled Downloaded layout).
//   The native engine solves this by loading each piece from its own layout
//   FILE -- every layout carries its <styles><include> which cascade to that
//   panel's subtree. We do EXACTLY what native matchlist.js/watchtile.js do:
//     - each left tile is built by panel.BLoadLayout('.../matchtiles/player.xml')
//       (brings matchtile.vcss + the MatchTile/MatchMap/MatchInfo structure)
//     - the right detail is built by panel.BLoadLayout('.../matchinfo.xml')
//       (brings matchinfo_scoreboard.vcss + matchinfo.vcss + the full scoreboard
//        DOM with native ids, and registers snippet_mi-round-summary-bar)
//   We then populate those native ids the same way the native scripts do
//   (SetDialogVariable + FindChildTraverse).
//
// TAB ISOLATION (the other cause that was broken):
//   We do NOT replace the navbar buttons' native onactivate handlers. The
//   Downloaded RadioButton keeps its native handler
//   (mainmenu_watch.NavigateToTab('JsDownloaded',...)), so native bookkeeping
//   (_m_activeTab) stays correct and clicking "Your Matches" properly re-hides
//   JsDownloaded -- and our body, which lives INSIDE JsDownloaded, hides with it.
//   We only ADD an 'Activated' listener on the Downloaded button (via
//   $.RegisterEventHandler, which runs alongside the native handler) that builds
//   our content and hides the native downloaded list. Crucially, render()/refresh
//   NEVER force JsDownloaded visible -- visibility is owned entirely by native
//   tab state. "Your Matches" is left completely untouched (native empty state).
//
// A hidden host panel (#FilmmakerMenuTab) carries the C++<->JS bridge attributes
// (unchanged contract with FilmmakerMenu.cpp):
//   C++ -> JS : sets host "demos" attribute (JSON), runs $.Filmmaker.render().
//   JS  -> C++: appends to host "cmd": "refresh", "addfolder", "watch <index>",
//               "rounds <index>" (lazy per-round parse request).

namespace Filmmaker {

inline const char* kFilmmakerGuiJs = R"FMJS(
(function () {
  try {
    ['FilmmakerMenuTab', 'FilmmakerOpenMenuButton', 'FmDownloadedBody'].forEach(function (id) {
      var p = $('#' + id); if (p) p.DeleteAsync(0);
    });

    function topRootOf(p) { var r = p; while (r && r.GetParent && r.GetParent()) r = r.GetParent(); return r; }
    var ctx = $.GetContextPanel();
    var root3 = topRootOf(ctx);
    var menuContent = root3.FindChildTraverse('JsMainMenuContent');
    var pageParent = menuContent || ctx;

    var host = $.CreatePanel('Panel', pageParent, 'FilmmakerMenuTab', {});
    host.visible = false; host.style.width = '0px'; host.style.height = '0px';

    var FILM = 's2r://panorama/images/icons/ui/film.vsvg';
    var TEAMS = ['CT', 'TERRORIST'];
    var PLAYERSTATS = ['kills', 'assists', 'deaths', 'mvps', 'score'];

    function statVal(p, name) {
      if (name === 'kills') return p.k;
      if (name === 'assists') return p.a;
      if (name === 'deaths') return p.d;
      if (name === 'mvps') return p.mvps || 0;
      return p.score;
    }
    function mapDisplay(map) { if (!map) return 'Demo'; var t = '#SFUI_Map_' + map; var l = $.Localize(t); return (l === t) ? map : l; }
    // Friendly demo name for the detail title: file basename without the .dem extension.
    function demoName(demo) {
      var f = (demo && demo.fileName) || '';
      f = f.replace(/^.*[\\/]/, '').replace(/\.dem$/i, '');
      return f || mapDisplay(demo && demo.map);
    }
    function setMapIcon(img, map) {
      if (!img) return;
      try { $.RegisterEventHandler('ImageFailedLoad', img, function () { try { img.SetImage('s2r://panorama/images/map_icons/map_icon_none_png.vtex'); } catch (e) {} }); } catch (e) {}
      try { img.SetImage('s2r://panorama/images/map_icons/map_icon_' + (map || '') + '.svg'); } catch (e) {}
    }
    function steamId64(accountId) {
      try { if (typeof BigInt !== 'undefined' && accountId) return (BigInt('76561197960265728') + BigInt(accountId)).toString(); } catch (e) {}
      return '';
    }
    var myAccount = 0;
    try {
      if (typeof MyPersonaAPI !== 'undefined' && MyPersonaAPI.GetXuid && typeof BigInt !== 'undefined') {
        var x = MyPersonaAPI.GetXuid();
        if (x) myAccount = Number(BigInt(x) - BigInt('76561197960265728'));
      }
    } catch (e) {}

    function outcomeOf(demo) {
      // 1 win, 0 tie, -1 loss, or null if my player isn't in this demo
      if (!demo.hasScoreboard || !myAccount || !demo.players) return null;
      var mine = null;
      for (var i = 0; i < demo.players.length; ++i) if (demo.players[i].accountId === myAccount) { mine = demo.players[i]; break; }
      if (!mine) return null;
      var myScore = (mine.team === 1) ? demo.teamScore1 : demo.teamScore0;
      var oppScore = (mine.team === 1) ? demo.teamScore0 : demo.teamScore1;
      return myScore > oppScore ? 1 : (myScore < oppScore ? -1 : 0);
    }

    // ---- state ----
    var demos = [];
    var sel = -1, selPlayer = -1;
    var selKey = null;              // stable key of the selected demo (survives refreshes)
    var tilePanels = [], playerRowPanels = [];
    var renderedKeys = [], tileSigs = [];  // for diffing the left list across refreshes
    var detailSig = null, detailRendered = false;
    var body = null, listPanel = null, info = null, emptyMsg = null, demoTitle = null;
    var scoreboardPanel = null, labelsRow = null, ctTable = null, tTable = null;
    var playerStats = null, roundContainer = null, tickLabels = null, watchBtn = null;
    var statHeadersBuilt = false, detailBuilt = false;

    // Stable per-demo identity (folder+file). Used as a key so refreshes that
    // don't change the demo SET never rebuild rows / move selection / reset scroll.
    function tileKey(demo) {
      if (demo && (demo.folder != null || demo.fileName != null)) return (demo.folder || '') + '|' + (demo.fileName || '');
      return 'idx' + (demo ? demo.index : '?');
    }
    // Signature of the data a left tile actually shows; tiles only update in place
    // when this changes.
    function tileSig(demo) {
      return [demo.hasScoreboard ? 1 : 0, demo.teamScore0, demo.teamScore1, demo.dateText || '', demo.map || '', outcomeOf(demo)].join('');
    }
    // Signature of the right-detail data; the detail only re-renders when it changes.
    function detailSignature(demo) {
      var s = [demo.map || '', demo.duration || 0, demo.dateText || '', demo.hasScoreboard ? 1 : 0, demo.teamScore0, demo.teamScore1, demo.hasRounds ? 1 : 0];
      var pl = demo.players || [];
      for (var i = 0; i < pl.length; ++i) { var p = pl[i]; s.push((p.name || '') + ':' + p.k + ',' + p.a + ',' + p.d + ',' + p.score + ',' + (p.mvps || 0) + ',' + p.team + ',' + ((p.perRound && p.perRound.length) || 0)); }
      return s.join('');
    }
    function keysEqual(a, b) { if (a.length !== b.length) return false; for (var i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false; return true; }
    function indexOfKey(k) { for (var i = 0; i < demos.length; ++i) if (tileKey(demos[i]) === k) return i; return -1; }
)FMJS"
// ---- native Watch page hooks (icon, hide Majors, navbar buttons, tab wiring) ----
R"FMJS(
    function watchPage() { return root3.FindChildTraverse('JsWatchContent'); }

    function applyWatchIcon() {
      var btn = root3.FindChildTraverse('MainMenuNavBarWatch');
      if (!btn) return;
      for (var i = 0; i < btn.GetChildCount(); ++i) { try { btn.GetChild(i).SetImage(FILM); break; } catch (e) {} }
      // Rename the section to "Movie Making". The nav button is icon-only, so its
      // HOVER TOOLTIP is effectively its name -- override the native "Watch matches
      // and tournaments" tooltip with our movie-making purpose. SetPanelEvent
      // replaces the native tooltip handler, so the old text no longer shows.
      if (!btn.__fmTip) {
        btn.__fmTip = true;
        btn.SetPanelEvent('onmouseover', function () { try { UiToolkitAPI.ShowTextTooltipOnPanel(btn, 'Movie Making'); } catch (e) {} });
        btn.SetPanelEvent('onmouseout', function () { try { UiToolkitAPI.HideTextTooltip(); } catch (e) {} });
      }
    }

    function removeMajors(page) {
      var t = page.FindChildTraverse('WatchNavBarButtonTournaments');
      if (t) { t.visible = false; t.AddClass('hide'); }
      var je = page.FindChildTraverse('JsEvents'); if (je) je.visible = false;
    }

    function navButtonsHost(page) {
      var nav = page.FindChildTraverse('watch-navbar');
      if (!nav || nav.GetChildCount() < 1) return null;
      return nav.GetChild(nav.GetChildCount() - 1); // right-aligned container
    }

    function addNavButtons(page) {
      var right = navButtonsHost(page);
      if (!right) return;
      // Hide native right-side controls (the lone refresh IconButton); keep ours.
      // Skip anything we created (ids start with 'Fm') so our button row survives
      // repeated applyWatchEdits() passes.
      for (var i = 0; i < right.GetChildCount(); ++i) {
        var c = right.GetChild(i);
        if (('' + c.id).indexOf('Fm') !== 0) c.visible = false;
      }
      // Put both buttons in a left-right flow row so they sit SIDE BY SIDE
      // instead of stacking on top of each other at the right edge of the
      // (non-flowing) native container -- that overlap hid the Refresh button.
      var bar = page.FindChildTraverse('FmNavBtns');
      if (!bar) bar = $.CreatePanel('Panel', right, 'FmNavBtns', { class: 'left-right-flow vertical-center' });
      // Search button (same magnifying-glass icon as the settings UI). Sits left of
      // Refresh; toggles the demo search input + suggestion dropdown.
      if (!page.FindChildTraverse('FmSearchBtn')) {
        var srb = $.CreatePanel('Button', bar, 'FmSearchBtn', { class: 'IconButton' });
        srb.style.marginRight = '4px';
        $.CreatePanel('Image', srb, '', { src: 's2r://panorama/images/icons/ui/search.vsvg' });
        srb.SetPanelEvent('onactivate', function () { toggleSearch(page); });
        srb.SetPanelEvent('onmouseover', function () { UiToolkitAPI.ShowTextTooltipOnPanel(srb, 'Search demos'); });
        srb.SetPanelEvent('onmouseout', function () { UiToolkitAPI.HideTextTooltip(); });
      }
      if (!page.FindChildTraverse('FmRefreshBtn')) {
        var refb = $.CreatePanel('Button', bar, 'FmRefreshBtn', { class: 'IconButton' });
        refb.style.marginRight = '4px';
        $.CreatePanel('Image', refb, '', { src: 's2r://panorama/images/icons/ui/refresh.vsvg' });
        refb.SetPanelEvent('onactivate', function () { api.addCommand('refresh'); });
        refb.SetPanelEvent('onmouseover', function () { UiToolkitAPI.ShowTextTooltipOnPanel(refb, 'Refresh demos'); });
        refb.SetPanelEvent('onmouseout', function () { UiToolkitAPI.HideTextTooltip(); });
      }
      if (!page.FindChildTraverse('FmAddFolderBtn')) {
        var addb = $.CreatePanel('Button', bar, 'FmAddFolderBtn', { class: 'IconButton' });
        $.CreatePanel('Image', addb, '', { src: 's2r://panorama/images/icons/ui/plus.vsvg' });
        addb.SetPanelEvent('onactivate', function () { api.addCommand('addfolder'); });
        addb.SetPanelEvent('onmouseover', function () { UiToolkitAPI.ShowTextTooltipOnPanel(addb, 'Add demo folder'); });
        addb.SetPanelEvent('onmouseout', function () { UiToolkitAPI.HideTextTooltip(); });
      }
    }

    // The Downloaded navbar RadioButton. Order in the center container is
    // [Your Matches, Downloaded, Tournaments].
    function downloadedButton(page) {
      var nav = page.FindChildTraverse('watch-navbar'); if (!nav) return null;
      var center = nav.GetChild(0); if (!center || center.GetChildCount() < 2) return null;
      return center.GetChild(1);
    }

    // ADD (not replace) a listener so the native onactivate -> NavigateToTab still
    // runs and keeps tab bookkeeping correct. Ours just injects our content.
    function hookDownloadedButton(page) {
      var dlBtn = downloadedButton(page);
      if (!dlBtn || dlBtn.__fmHooked) return;
      dlBtn.__fmHooked = true;
      try {
        $.RegisterEventHandler('Activated', dlBtn, function () { $.Schedule(0.0, function () { takeoverDownloaded(page); }); });
      } catch (e) {
        // Fallback: chain through the native navigation ourselves.
        dlBtn.SetPanelEvent('onactivate', function () {
          $.DispatchEvent('NavigateToTab', 'JsDownloaded', 'downloaded menu button');
          $.Schedule(0.0, function () { takeoverDownloaded(page); });
        });
      }
    }

    // Build our content into JsDownloaded and hide the native downloaded list,
    // WITHOUT changing JsDownloaded's own visibility (native tab state owns that).
    function ensureBody() {
      var page = watchPage(); if (!page) return false;
      var dl = page.FindChildTraverse('JsDownloaded'); if (!dl) return false;
      if (!body || !body.IsValid() || !dl.FindChildTraverse('FmDownloadedBody')) buildBody(dl);
      return !!body;
    }

    // Hide native MatchListAndInfo siblings so only our body shows in JsDownloaded.
    function hideNativeSiblings(dl) {
      for (var i = 0; i < dl.GetChildCount(); ++i) {
        var c = dl.GetChild(i);
        if (c.id !== 'FmDownloadedBody') c.visible = false;
      }
    }

    // Called when Downloaded becomes the active tab (native already toggled
    // JsDownloaded visible). Refresh native-sibling hiding + our content.
    function takeoverDownloaded(page) {
      page = page || watchPage(); if (!page) return false;
      var dl = page.FindChildTraverse('JsDownloaded'); if (!dl) return false;
      if (!ensureBody()) return false;
      hideNativeSiblings(dl);
      if (body) { body.visible = true; body.SetReadyForDisplay(true); }
      renderInto();
      return true;
    }

    function applyWatchEdits() {
      applyWatchIcon();
      var page = watchPage();
      if (!page) return false;
      removeMajors(page);
      addNavButtons(page);
      hookDownloadedButton(page);
      ensureBody();   // build/keep content ready; do NOT force-show the tab
      renderInto();
      return true;
    }
)FMJS"
// ---- body skeleton: list container ($.CreatePanel) + matchinfo.xml detail ----
R"FMJS(
    function mk(parent, type, id, cls, attrs) {
      var o = attrs || {}; if (cls) o.class = cls;
      return $.CreatePanel(type, parent, id || '', o);
    }

    function buildBody(dl) {
      hideNativeSiblings(dl);
      body = $.CreatePanel('Panel', dl, 'FmDownloadedBody', {});
      body.style.width = '100%'; body.style.height = '100%';

      // Outer flow matches the native MatchListAndInfo snippet shell.
      var outer = mk(body, 'Panel', '', 'no-flow full-width full-height');
      var row = mk(outer, 'Panel', '', 'left-right-flow full-width full-height');

      // Left match list. These classes live in mainmenu_watch.vcss (page scope).
      var listC = mk(row, 'Panel', 'id-match-list-container', 'MatchList subsection-content__background-color');
      listPanel = mk(listC, 'Panel', 'FmMatchList', 'MatchList MatchList-scroll MatchList--Filled');

      // Right detail host. matchinfo.xml is loaded into FmMatchInfo (a child) so
      // FmInfo keeps its own background classes and can also hold the empty msg.
      info = mk(row, 'Panel', 'FmInfo', 'subsection-content__background-color full-width full-height no-flow');
      var infoOuter = info; // FmInfo stays put; `info` is reassigned to the matchinfo host below.

      var detailHost = mk(info, 'Panel', 'FmMatchInfo', '');
      detailHost.style.width = '100%'; detailHost.style.height = '100%';
      buildDetailLayout(detailHost);

      emptyMsg = mk(info, 'Panel', 'FmEmptyMsg', 'left-right-flow horizontal-center vertical-center');
      var ei = mk(emptyMsg, 'Image', '', 'info-icon', { texturewidth: '32', textureheight: '32' });
      try { ei.SetImage('s2r://panorama/images/icons/ui/info.vsvg'); } catch (e) {}
      mk(emptyMsg, 'Label', '', 'Info-Message', { text: 'No demos found. Use the + button in the top-right to add a demo folder.' });
      emptyMsg.visible = false;

      // Demo NAME title, shown in the empty banner area above the map/duration/date
      // meta (the red-square spot in the reference). Created LAST on the persistent
      // FmInfo host so it overlays on top of the matchinfo content and survives
      // matchinfo.xml (re)loads into FmMatchInfo.
      demoTitle = mk(infoOuter, 'Label', 'FmDemoTitle', '');
      demoTitle.style.horizontalAlign = 'center'; demoTitle.style.verticalAlign = 'top';
      demoTitle.style.marginTop = '18px'; demoTitle.style.width = '80%';
      demoTitle.style.fontSize = '22px'; demoTitle.style.fontWeight = 'bold';
      demoTitle.style.color = '#ffffffff'; demoTitle.style.textAlign = 'center';
      demoTitle.style.textOverflow = 'ellipsis'; demoTitle.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
      demoTitle.style.textShadow = '0px 2px 4px 1.0 #000000cc';
      demoTitle.visible = false;

      if (!listPanel) $.Msg('[filmmaker] body build incomplete (FmMatchList missing).');
    }

    // Load CS2's real matchinfo.xml so its <styles> cascade in and the native
    // scoreboard DOM (with its ids + the round-summary-bar snippet) exists. Then
    // wire up references to the native ids and trim the buttons we do not use.
    function buildDetailLayout(detailHost) {
      detailBuilt = false; statHeadersBuilt = false;
      try { detailHost.BLoadLayout('file://{resources}/layout/matchinfo.xml', false, false); } catch (e) {}
      info = detailHost;
      // Detect success by presence of the native scoreboard, not the (unreliable)
      // BLoadLayout return value.
      var ok = !!detailHost.FindChildTraverse('Scoreboard');
      if (ok) {
        detailHost.RemoveClass('mi-sb--hidden'); // matchinfo.xml hides the board until data is ready
        scoreboardPanel = detailHost.FindChildTraverse('Scoreboard');
        labelsRow = detailHost.FindChildTraverse('players-table__labels-row');
        ctTable = detailHost.FindChildTraverse('players-table-CT');
        tTable = detailHost.FindChildTraverse('players-table-TERRORIST');
        playerStats = detailHost.FindChildTraverse('id-mi-player-stats');
        roundContainer = detailHost.FindChildTraverse('id-mi-round-stats__container');
        tickLabels = detailHost.FindChildTraverse('id-mi-round-stats__tick-labels');
        watchBtn = detailHost.FindChildTraverse('id-mi-watch');
        // Fully collapse buttons that make no sense for a local demo (download is
        // already done; no souvenir / error / downloading spinner).
        ['id-mi-error-delete', 'id-mi-downloading', 'id-mi-download', 'id-mi-souvenir'].forEach(function (id) {
          var b = detailHost.FindChildTraverse(id); if (b) { b.visible = false; b.AddClass('hide'); }
        });
        // Keep the copy/delete buttons' FOOTPRINT (invisible + non-interactive) so the
        // green Watch button keeps the SAME horizontal position as the native Your
        // Matches tab. Collapsing them slid our Watch button hard to the right edge,
        // which is the misalignment between the two tabs.
        ['id-mi-copy', 'id-mi-delete'].forEach(function (id) {
          var b = detailHost.FindChildTraverse(id);
          if (b) { b.style.opacity = '0'; b.hittest = false; b.enabled = false; b.RemoveClass('hide'); b.visible = true; }
        });
        detailBuilt = !!(scoreboardPanel && ctTable && tTable && watchBtn);
      }
      if (!detailBuilt) {
        $.Msg('[filmmaker] matchinfo.xml load failed; detail will be limited.');
        scoreboardPanel = labelsRow = ctTable = tTable = playerStats = roundContainer = tickLabels = watchBtn = null;
      }
      buildStatHeaders();
    }

    function buildStatHeaders() {
      if (statHeadersBuilt || !labelsRow) return;
      PLAYERSTATS.forEach(function (stat) {
        var c = $.CreatePanel('Panel', labelsRow, '', {});
        c.AddClass('sb-row__cell'); c.AddClass('sb-row__cell--' + stat); c.AddClass('matchinfo-scoreboard-header-stat-cell');
        $.CreatePanel('Label', c, '', { text: $.Localize('#Scoreboard_' + stat + '_header') });
      });
      statHeadersBuilt = true;
    }
)FMJS"
// ---- left match-tile list (native matchtiles/player.xml) ----
R"FMJS(
    // Reconcile the left list against `demos`. Returns true if a structural
    // rebuild happened. When the demo SET is unchanged (the common case during a
    // background scan, where only player data fills in), tiles are updated IN
    // PLACE -- no RemoveAndDeleteChildren, so scroll position, selection and
    // focus are preserved and there is no flicker.
    function reconcileList() {
      if (!listPanel) return false;
      var newKeys = demos.map(tileKey);
      if (tilePanels.length === demos.length && keysEqual(newKeys, renderedKeys)) {
        for (var i = 0; i < demos.length; ++i) {
          var sig = tileSig(demos[i]);
          if (sig !== tileSigs[i]) { updateTile(tilePanels[i], demos[i]); tileSigs[i] = sig; }
        }
        return false;
      }
      listPanel.RemoveAndDeleteChildren();
      tilePanels = []; tileSigs = [];
      demos.forEach(function (demo, i) { tilePanels.push(createTile(demo, i)); tileSigs.push(tileSig(demo)); });
      renderedKeys = newKeys;
      return true;
    }

    function createTile(demo, i) {
      // Mirror native matchlist.js: a RadioButton with player.xml loaded into it.
      // BLoadLayout applies the root panel's class to the button and brings
      // matchtile.vcss into scope, so the tile is styled exactly like native.
      var tile = $.CreatePanel('RadioButton', listPanel, 'fm-tile-' + i, { group: 'FmMatchList' });
      try { tile.BLoadLayout('file://{resources}/layout/matchtiles/player.xml', true, false); } catch (e) {}
      // Detect success by presence of the layout's panels, not the return value,
      // so we never stack a fallback on top of a layout that actually loaded.
      tile.__loaded = !!tile.FindChildTraverse('mapname');
      tile.RemoveClass('MatchTile--Collapse');
      // Bind by stable KEY (not list index) so the handler stays correct even
      // when the list is later reconciled in place.
      tile.SetPanelEvent('onactivate', (function (key) { return function () { selectByKey(key); }; })(tileKey(demo)));
      if (tile.__loaded) populateTile(tile, demo); else buildTileFallback(tile, demo, outcomeOf(demo));
      return tile;
    }

    function updateTile(tile, demo) {
      if (tile.__loaded) { populateTile(tile, demo); }
      else { tile.RemoveAndDeleteChildren(); buildTileFallback(tile, demo, outcomeOf(demo)); }
    }

    function applyOutcomeClass(tile, oc) {
      tile.SetHasClass('MatchVictory', oc === 1);
      tile.SetHasClass('MatchLoss', oc === -1);
      tile.SetHasClass('MatchTied', oc === 0);
    }

    function populateTile(tile, demo) {
      var oc = outcomeOf(demo);
      applyOutcomeClass(tile, oc);
      setMapIcon(tile.FindChildTraverse('mapicon'), demo.map);
      var modeIcon = tile.FindChildTraverse('modeicon');
      if (modeIcon) { try { modeIcon.SetImage('s2r://panorama/images/icons/ui/competitive.vsvg'); } catch (e) {} }
      var mapName = tile.FindChildTraverse('mapname'); if (mapName) mapName.text = mapDisplay(demo.map);

      var s0 = tile.FindChildTraverse('score_team0');
      var sv = tile.FindChildTraverse('vs');
      var s1 = tile.FindChildTraverse('score_team1');
      if (demo.hasScoreboard) {
        if (s0) s0.text = String(demo.teamScore0);
        if (sv) sv.text = '-';
        if (s1) s1.text = String(demo.teamScore1);
      } else {
        if (s0) s0.text = mapDisplay(demo.map);
        if (sv) sv.text = '';
        if (s1) s1.text = '';
      }
      if (s0) s0.SetHasClass('tint--CT', !!demo.hasScoreboard);
      if (s1) s1.SetHasClass('tint--TERRORIST', !!demo.hasScoreboard);

      var ts = tile.FindChildTraverse('timestamp'); if (ts) ts.text = demo.dateText || '';
      var outcome = tile.FindChildTraverse('outcome');
      if (outcome) {
        outcome.SetHasClass('MatchInfo--Hide', oc === null);
        if (oc !== null) outcome.text = oc === 1 ? $.Localize('#WatchMenu_Outcome_Won', outcome) : oc === -1 ? $.Localize('#WatchMenu_Outcome_Lost', outcome) : $.Localize('#WatchMenu_Outcome_Tied', outcome);
      }
      var dl = tile.FindChildTraverse('id-download-state'); if (dl) dl.AddClass('downloaded');
    }

    function setCheckedTile(i) { for (var t = 0; t < tilePanels.length; ++t) { try { tilePanels[t].checked = (t === i); } catch (e) {} } }

    function buildTileFallback(tile, demo, oc) {
      var map = $.CreatePanel('Panel', tile, '', { class: 'MatchMap' });
      setMapIcon($.CreatePanel('Image', map, '', { class: 'MatchMap__Icon' }), demo.map);
      $.CreatePanel('Label', map, '', { class: 'MatchInfo MatchInfo__MapName', text: mapDisplay(demo.map) });
      var infoC = $.CreatePanel('Panel', tile, '', { class: 'MatchInfoContainer' });
      var scoreRow = $.CreatePanel('Panel', infoC, '', { class: 'left-right-flow' });
      if (demo.hasScoreboard) {
        $.CreatePanel('Label', scoreRow, '', { class: 'MatchInfo__Score tint--CT', text: String(demo.teamScore0) });
        $.CreatePanel('Label', scoreRow, '', { text: '-' });
        $.CreatePanel('Label', scoreRow, '', { class: 'MatchInfo__Score tint--TERRORIST', text: String(demo.teamScore1) });
      } else {
        $.CreatePanel('Label', scoreRow, '', { class: 'MatchInfo__Score', text: mapDisplay(demo.map) });
      }
      $.CreatePanel('Label', infoC, '', { class: 'MatchInfo', text: demo.dateText || '' });
    }
)FMJS"
// ---- right detail: meta + CT/T scoreboard player rows ----
R"FMJS(
    function makeRow(table, p, teamId) {
      var row = $.CreatePanel('Panel', table, '', {});
      row.AddClass('sb-row'); row.AddClass('left-right-flow');
      var stats = $.CreatePanel('Panel', row, '', { class: 'left-right-flow' });

      var avCell = $.CreatePanel('Panel', stats, '', {});
      avCell.AddClass('sb-row__cell'); avCell.AddClass('sb-row__cell--avatar'); avCell.AddClass('sb-row__cell--avatar--' + TEAMS[teamId]);
      var av = $.CreatePanel('CSGOAvatarImage', avCell, '', {});
      var sid = steamId64(p.accountId);
      if (sid) { try { av.PopulateFromSteamID(sid); } catch (e) { try { av.steamid = sid; } catch (e2) {} } }

      var nameCell = $.CreatePanel('Panel', stats, '', {});
      nameCell.AddClass('sb-row__cell'); nameCell.AddClass('sb-row__cell--name');
      var nm = $.CreatePanel('Label', nameCell, '', { text: p.name || '[unknown]' });
      nm.AddClass('sb-tint--' + TEAMS[teamId]);

      PLAYERSTATS.forEach(function (stat) {
        if (stat === 'mvps') {
          var mp = $.CreatePanel('Panel', stats, '', {});
          mp.AddClass('sb-row__cell'); mp.AddClass('sb-row__cell--mvps'); mp.AddClass('sb-tint--' + TEAMS[teamId]);
          var star = $.CreatePanel('Image', mp, '', {}); star.AddClass('sb-row__cell--mvps__star'); star.SetImage('s2r://panorama/images/icons/ui/star.vsvg');
          var cnt = $.CreatePanel('Label', mp, '', { text: String(p.mvps || 0) }); cnt.AddClass('sb-row__cell--mvps__count'); cnt.AddClass('mi-mvps-shrink-overflow');
          if (!p.mvps) mp.AddClass('hide-mvps');
        } else {
          var c = $.CreatePanel('Panel', stats, '', {});
          c.AddClass('sb-row__cell'); c.AddClass('sb-row__cell--' + stat);
          var l = $.CreatePanel('Label', c, '', { text: String(statVal(p, stat)) }); l.AddClass('sb-tint--' + TEAMS[teamId]);
        }
      });

      row.SetPanelEvent('onactivate', (function (idx) { return function () { selectPlayer(idx); }; })(p.__idx));
      playerRowPanels[p.__idx] = row;
    }

    function byScore(a, b) { return (b.score || 0) - (a.score || 0); }

    function renderDetail(demo) {
      if (!info) return;
      if (demoTitle) { demoTitle.text = demoName(demo); demoTitle.visible = true; }
      info.SetDialogVariable('map_name', mapDisplay(demo.map));
      info.SetDialogVariable('durationLabel', $.Localize('#CSGO_Watch_Info_1'));
      var mins = demo.duration ? Math.max(Math.floor(demo.duration / 60), 1) : 0;
      info.SetDialogVariable('duration', mins ? ($.ConstructString ? $.ConstructString('#CSGO_Watch_Minute:f', { value: mins }) : (mins + ' min')) : '-');
      info.SetDialogVariable('dateOrRoundLabel', $.Localize('#CSGO_Watch_Info_2'));
      info.SetDialogVariable('dateOrRound', demo.dateText || '-');
      info.SetDialogVariable('score_CT', demo.hasScoreboard ? String(demo.teamScore0) : '-');
      info.SetDialogVariable('score_TERRORIST', demo.hasScoreboard ? String(demo.teamScore1) : '-');
      info.SetDialogVariable('sb_team_name--CT', $.Localize('#teamname_CT'));
      info.SetDialogVariable('sb_team_name--TERRORIST', $.Localize('#teamname_TERRORIST'));

      setMapIcon(info.FindChildTraverse('id-mi-map-icon'), demo.map);
      var modeIcon = info.FindChildTraverse('id-mi-mode-icon');
      if (modeIcon) { try { modeIcon.SetImage('s2r://panorama/images/icons/ui/competitive.vsvg'); } catch (e) {} }

      if (scoreboardPanel) scoreboardPanel.visible = true;
      if (ctTable) ctTable.RemoveAndDeleteChildren();
      if (tTable) tTable.RemoveAndDeleteChildren();
      playerRowPanels = [];

      if (!(demo.hasScoreboard && demo.players && demo.players.length)) {
        if (ctTable) {
          var no = $.CreatePanel('Label', ctTable, '', { text: 'No scoreboard data for this demo.' });
          no.style.padding = '20px'; no.style.color = '#8b949c';
        }
        if (playerStats) playerStats.AddClass('mi-player-stats__collapse');
        return;
      }

      var t0 = [], t1 = [];
      demo.players.forEach(function (p, idx) { p.__idx = idx; (p.team === 1 ? t1 : t0).push(p); });
      t0.sort(byScore); t1.sort(byScore);
      t0.forEach(function (p) { makeRow(ctTable, p, 0); });
      t1.forEach(function (p) { makeRow(tTable, p, 1); });
    }
)FMJS"
// ---- per-round performance timeline (native mi-round-summary-bar) ----
R"FMJS(
    function fillRoundStats(p) {
      if (!roundContainer || !playerStats) return;
      roundContainer.RemoveAndDeleteChildren();
      if (tickLabels) tickLabels.RemoveAndDeleteChildren();
      var pr = (p && p.perRound) || [];
      playerStats.SetHasClass('mi-player-stats__collapse', pr.length === 0);

      var title = info.FindChildTraverse('id-mi-player-stats-title');
      if (title) { info.SetDialogVariable('playerNameTitle', p ? (p.name || '[unknown]') : ''); title.text = $.Localize('#MatchInfo_RoundDataTitle', info); }
      if (!pr.length) return;

      for (var i = 0; i < pr.length; ++i) {
        var r = pr[i];
        var side = TEAMS[r.side === 1 ? 1 : 0];
        var bar = $.CreatePanel('Button', roundContainer, '', {});
        try { bar.BLoadLayoutSnippet('snippet_mi-round-summary-bar'); } catch (e) { continue; }
        bar.AddClass('round-selection-button'); bar.AddClass('no-hover');

        var roundBar = bar.FindChildTraverse('id-mi-round-summary-bar__container');
        var iconContainer = bar.FindChildTraverse('id-mi-icons__container');
        if (!roundBar || !iconContainer) continue;
        var winBar = roundBar.GetChild(0).GetChild(0);
        var border = roundBar.GetChild(1);
        var lossBar = roundBar.GetChild(2).GetChild(0);
        var winIcons = bar.FindChildTraverse('id-mi-eliminations-win');

        winBar.AddClass('sb-tint--' + side); border.AddClass('sb-tint--' + side); if (winIcons) winIcons.AddClass('sb-tint--' + side);
        if (r.w) { lossBar.AddClass('mi-round-summary-bar--EMPTY'); winBar.RemoveClass('mi-round-summary-bar--EMPTY'); }
        else { winBar.AddClass('mi-round-summary-bar--EMPTY'); lossBar.RemoveClass('mi-round-summary-bar--EMPTY'); }

        var star = bar.FindChildTraverse('id-mvp-star');
        if (star) { if (r.mvp) { star.RemoveClass('hide'); star.AddClass('sb-tint--' + side); } else { star.AddClass('hide'); } }

        var nk = r.k || 0, nh = r.hs || 0;
        if (winIcons) for (var k = 0; k < 5; ++k) {
          var kIcon = winIcons.FindChildTraverse('id-mi-icon-elimination_' + k);
          var hIcon = winIcons.FindChildTraverse('id-mi-icon-elimination--headshot_' + k);
          if (!kIcon || !hIcon) continue;
          if (k >= nk) { kIcon.AddClass('hide'); hIcon.AddClass('hide'); }
          else if (k >= nh) { kIcon.RemoveClass('hide'); hIcon.AddClass('hide'); }
          else { kIcon.AddClass('hide'); hIcon.RemoveClass('hide'); }
        }

        var death = iconContainer.FindChildTraverse('id-mi-elimination-death');
        if (death) { if (r.d) death.RemoveClass('hide'); else death.AddClass('hide'); }

        if (tickLabels) {
          var tk = $.CreatePanel('Panel', tickLabels, '', { class: 'mi-tick' });
          var show = ((i + 1) % 5 === 0) || (i === 0) || (i === pr.length - 1);
          $.CreatePanel('Label', tk, '', { class: 'mi-tick-label', text: show ? String(i + 1) : '' });
        }
      }
    }

    function selectPlayer(i) {
      selPlayer = i;
      var demo = demos[sel];
      if (!demo || !demo.players) return;
      var p = demo.players[i];
      if (playerRowPanels[selPlayer]) { try { playerRowPanels[selPlayer].checked = false; } catch (e) {} }
      if (playerRowPanels[i]) { try { playerRowPanels[i].checked = true; } catch (e) {} }
      fillRoundStats(p);
    }

    function bestPlayerIndex(players) { var b = 0; for (var k = 1; k < players.length; ++k) if ((players[k].score || 0) > (players[b].score || 0)) b = k; return b; }

    function wireWatch(demo) {
      if (watchBtn) watchBtn.SetPanelEvent('onactivate', (function (idx) { return function () { api.addCommand('watch ' + idx); }; })(demo.index));
      if (demo.hasScoreboard && !demo.hasRounds) api.addCommand('rounds ' + demo.index);
    }

    // Full (re)render of the right detail for a newly chosen demo: resets the
    // player selection to the top fragger. Used only when the SELECTED demo
    // changes (user click or first load) -- not on every background refresh.
    function selectDemo(i) {
      var demo = demos[i]; if (!demo) return;
      sel = i; selKey = tileKey(demo); selPlayer = -1;
      if (emptyMsg) emptyMsg.visible = false;
      if (info) info.visible = true;
      setCheckedTile(i);
      renderDetail(demo); detailSig = detailSignature(demo); detailRendered = true;
      wireWatch(demo);
      var players = demo.players || [];
      if (players.length) selectPlayer(bestPlayerIndex(players)); else fillRoundStats(null);
    }

    // User clicked a tile -> resolve by stable key (robust to in-place updates).
    function selectByKey(key) { var i = indexOfKey(key); if (i >= 0) selectDemo(i); }

    // The selected demo is unchanged but its data may have filled in (phase-2
    // scan). Re-render the detail ONLY if its data actually changed, and keep the
    // current player selection so the round timeline does not jump around.
    function maybeRefreshDetail(demo) {
      var s = detailSignature(demo);
      if (s === detailSig) return;
      detailSig = s;
      var keep = selPlayer;
      renderDetail(demo);
      wireWatch(demo);
      var players = demo.players || [];
      if (players.length) selectPlayer((keep >= 0 && keep < players.length) ? keep : bestPlayerIndex(players));
      else fillRoundStats(null);
    }

    function renderInto() {
      if (!listPanel) return;
      reconcileList();  // stable: only rebuilds rows when the demo SET changes
      if (!demos.length) {
        if (info) info.visible = false;
        if (demoTitle) demoTitle.visible = false;
        if (emptyMsg) emptyMsg.visible = true;
        selKey = null; sel = -1; detailRendered = false;
        return;
      }
      if (emptyMsg) emptyMsg.visible = false;
      if (info) info.visible = true;
      // Resolve selection by stable key so a refresh keeps the same demo selected.
      var idx = selKey != null ? indexOfKey(selKey) : -1;
      if (idx < 0) idx = (sel >= 0 && sel < demos.length) ? sel : 0; // first load / selection removed
      if (idx !== sel || !detailRendered) {
        selectDemo(idx);
      } else {
        setCheckedTile(idx);
        maybeRefreshDetail(demos[idx]);
      }
    }
)FMJS"
// ---- demo search (button -> input + suggestion dropdown) ----
R"FMJS(
    var searchWrap = null, searchInput = null, searchResults = null;

    // Lazily build the search input + dropdown once, parented to the watch page so it
    // floats below the navbar (right side, clear of the friends rail). Inline-styled so
    // it does not depend on the settings stylesheet being in scope on this page.
    function buildSearch(page) {
      if (searchWrap && searchWrap.IsValid && searchWrap.IsValid()) return;
      var hostPanel = page.FindChildTraverse('main-content') || page;
      searchWrap = $.CreatePanel('Panel', hostPanel, 'FmSearchWrap', {});
      searchWrap.style.flowChildren = 'down';
      searchWrap.style.horizontalAlign = 'right'; searchWrap.style.verticalAlign = 'top';
      searchWrap.style.marginTop = '52px'; searchWrap.style.marginRight = '72px';
      searchWrap.style.width = '340px'; searchWrap.style.zIndex = '100';
      searchWrap.visible = false;

      searchInput = $.CreatePanel('TextEntry', searchWrap, 'FmSearchInput', { placeholder: 'Search map, player or demo name' });
      searchInput.style.width = '100%'; searchInput.style.height = '34px';
      searchInput.style.backgroundColor = 'rgba(0,0,0,0.65)';
      searchInput.style.border = '1px solid rgba(255,255,255,0.12)';
      searchInput.style.borderRadius = '3px'; searchInput.style.color = '#eef2f6ff';
      searchInput.style.paddingLeft = '10px'; searchInput.style.paddingRight = '10px';
      searchInput.style.fontSize = '15px';
      searchInput.SetPanelEvent('ontextentrychange', function () { updateSuggestions(); });
      try { $.RegisterEventHandler('TextEntryChanged', searchInput, function () { updateSuggestions(); }); } catch (e) {}

      searchResults = $.CreatePanel('Panel', searchWrap, 'FmSearchResults', {});
      searchResults.style.flowChildren = 'down'; searchResults.style.width = '100%';
      searchResults.style.marginTop = '2px'; searchResults.style.maxHeight = '320px';
      searchResults.style.overflow = 'squish scroll';
      searchResults.style.backgroundColor = 'rgba(14,16,20,0.98)';
      searchResults.style.border = '1px solid rgba(255,255,255,0.10)';
      searchResults.visible = false;
    }

    function toggleSearch(page) {
      buildSearch(page);
      if (!searchWrap) return;
      var show = !searchWrap.visible;
      searchWrap.visible = show;
      if (show) { try { searchInput.SetFocus(); } catch (e) {} updateSuggestions(); }
      else { try { searchInput.text = ''; } catch (e) {} if (searchResults) searchResults.visible = false; }
    }

    // Return the matched text (for the gray sub-line) if this demo matches q, else null.
    function demoMatches(demo, q) {
      if (mapDisplay(demo.map).toLowerCase().indexOf(q) >= 0) return 'map: ' + mapDisplay(demo.map);
      if (demoName(demo).toLowerCase().indexOf(q) >= 0) return 'demo: ' + demoName(demo);
      var pl = demo.players || [];
      for (var i = 0; i < pl.length; ++i) { var n = pl[i].name || ''; if (n && n.toLowerCase().indexOf(q) >= 0) return 'player: ' + n; }
      return null;
    }

    function updateSuggestions() {
      if (!searchResults) return;
      searchResults.RemoveAndDeleteChildren();
      var q = ''; try { q = (searchInput.text || '').toLowerCase().trim(); } catch (e) {}
      if (!q) { searchResults.visible = false; return; }
      var shown = 0;
      for (var i = 0; i < demos.length && shown < 12; ++i) {
        var why = demoMatches(demos[i], q);
        if (why == null) continue;
        (function (demo, why) {
          var rowBtn = $.CreatePanel('Button', searchResults, '', {});
          rowBtn.style.flowChildren = 'down'; rowBtn.style.width = '100%';
          rowBtn.style.paddingTop = '6px'; rowBtn.style.paddingBottom = '6px';
          rowBtn.style.paddingLeft = '10px'; rowBtn.style.paddingRight = '10px';
          var t = $.CreatePanel('Label', rowBtn, '', { text: mapDisplay(demo.map) + '  -  ' + demoName(demo) });
          t.style.color = '#eef2f6ff'; t.style.fontSize = '14px';
          var sub = $.CreatePanel('Label', rowBtn, '', { text: why }); // gray hint of WHY it matched
          sub.style.color = '#8a93a0ff'; sub.style.fontSize = '12px';
          rowBtn.SetPanelEvent('onactivate', (function (key) { return function () { revealDemo(key); }; })(tileKey(demo)));
        })(demos[i], why);
        ++shown;
      }
      searchResults.visible = shown > 0;
    }

    // Jump to a demo from a suggestion: switch to the Downloaded tab, select it, and
    // scroll its tile into view; then close the search.
    function revealDemo(key) {
      var page = watchPage();
      var dlBtn = page && downloadedButton(page);
      if (dlBtn) { try { dlBtn.checked = true; } catch (e) {} try { $.DispatchEvent('Activated', dlBtn); } catch (e) {} }
      $.Schedule(0.05, function () {
        takeoverDownloaded(page);
        selectByKey(key);
        var idx = indexOfKey(key);
        if (idx >= 0 && tilePanels[idx]) { try { tilePanels[idx].ScrollParentToMakePanelFit(3, false); } catch (e) {} }
      });
      if (searchWrap) searchWrap.visible = false;
      try { searchInput.text = ''; } catch (e) {}
      if (searchResults) searchResults.visible = false;
    }
)FMJS"
// ---- bridge API + bootstrap ----
R"FMJS(
    var api = {};
    api.addCommand = function (command) { host.SetAttributeString('cmd', host.GetAttributeString('cmd', '') + command + '\n'); };
    api.render = function () {
      var raw = host.GetAttributeString('demos', '');
      var data = null; if (raw) { try { data = JSON.parse(raw); } catch (e) {} }
      demos = (data && data.demos) || [];
      if (sel >= demos.length) sel = -1;
      applyWatchEdits();   // builds/keeps content ready; never forces tab visible
    };
    api.show = function () {
      if (typeof MainMenu !== 'undefined' && MainMenu.NavigateToTab) MainMenu.NavigateToTab('JsWatch', 'mainmenu_watch');
      var tries = 0; var tick = function () {
        if (applyWatchEdits()) {
          var page = watchPage();
          var dlBtn = page && downloadedButton(page);
          if (dlBtn) {
            // Programmatic "click" so native NavigateToTab runs (correct tab state)
            // and our Activated handler injects our content.
            try { dlBtn.checked = true; } catch (e) {}
            try { $.DispatchEvent('Activated', dlBtn); } catch (e) {}
            $.Schedule(0.05, function () { takeoverDownloaded(page); });
          }
          return;
        }
        if (++tries > 20) return;
        $.Schedule(0.1, tick);
      };
      $.Schedule(0.05, tick);
    };
    $.Filmmaker = api;

    var watchNav = root3.FindChildTraverse('MainMenuNavBarWatch');
    if (watchNav) {
      applyWatchIcon();
      watchNav.SetPanelEvent('onactivate', function () {
        if (typeof MainMenu !== 'undefined' && MainMenu.NavigateToTab) MainMenu.NavigateToTab('JsWatch', 'mainmenu_watch');
        var tries = 0; var tick = function () { if (applyWatchEdits() || ++tries > 20) return; $.Schedule(0.1, tick); };
        $.Schedule(0.05, tick);
      });
    }

    applyWatchEdits();
    api.render();
    $.Msg('[filmmaker] native Watch tab hooked (demos=' + demos.length + ').');
  } catch (err) {
    $.Msg('[filmmaker] gui error: ' + err);
  }
})();
)FMJS";

} // namespace Filmmaker
