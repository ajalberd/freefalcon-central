'use strict';

// The campaign map. Units and objectives are drawn on a canvas over the
// campaign's own Kneemap.gif, which is one pixel per grid kilometre -- the
// same 1024x1024 the theater header reports -- so grid coordinates land
// straight on image pixels. Grid x runs west to east and grid y runs south to
// north (ConvertGridToSim in campaign/camplib/find.cpp), which is where the y
// flip below comes from.

const TEAM_COLOURS = [
  '#7a8596', // 0 unaligned
  '#3f8bd8', // 1 US
  '#4fb6e0', // 2 ROK
  '#8f7fe8', // 3 Japan
  '#d8503f', // 4 CIS
  '#e0803f', // 5 PRC
  '#d33a54', // 6 DPRK
  '#4fb06a', // 7 other
];

const KIND_SHAPE = {
  battalion: 'bar',
  brigade: 'bar2',
  squadron: 'wing',
  flight: 'wing',
  package: 'diamond',
  taskforce: 'ship',
};

// Objective categories, in draw order (later = on top) with their glyph.
const OBJ_LAYERS = [
  ['terrain', 'dot'],
  ['infrastructure', 'tick'],
  ['political', 'square'],
  ['industry', 'gear'],
  ['port', 'anchor'],
  ['military', 'flag'],
  ['airdefence', 'tri'],
  ['airbase', 'runway'],
];

// Zoom below which a layer stops drawing, so a fully zoomed-out map is not
// 2600 overlapping glyphs.
const LAYER_MIN_ZOOM = {
  terrain: 1.6, infrastructure: 1.2, political: 0.45, industry: 0.45,
  port: 0.3, military: 0.3, airdefence: 0.3, airbase: 0,
};

// falclib/include/f4vu.h, the values that fit in the two bits a feature gets
// on disk. The engine recomputes an objective's condition from these on load.
const FEATURE_STATUS = ['Normal', 'Repaired', 'Damaged', 'Destroyed'];

// The game's own icon atlases, one per team colour, loaded once per theater.
const ICONS = {theater: null, manifest: null, images: {}, ready: false};

// Ground-imagery tiles, served from the theater's own terrain. Keyed
// "z/x/y"; an entry is an Image that may still be loading.
const TERRAIN = {theater: null, info: null, tiles: new Map(), pending: 0};
const TERRAIN_TILE_CAP = 600;

async function loadTerrain(redraw) {
  if (TERRAIN.theater === S.theater) return;
  TERRAIN.theater = S.theater;
  TERRAIN.info = null;
  TERRAIN.tiles.clear();
  try {
    const d = await api('/api/terrain?theater=' + encodeURIComponent(S.theater));
    if (d.available) { TERRAIN.info = d; if (redraw) redraw(); }
  } catch (e) { /* the kneeboard map still works without it */ }
}

// A tile is fetched at most once; the draw loop just asks for it every frame
// and skips whatever has not arrived yet.
function terrainTile(z, x, y, redraw) {
  const key = z + '/' + x + '/' + y;
  let img = TERRAIN.tiles.get(key);
  if (img) return img.complete && img.naturalWidth ? img : null;
  if (TERRAIN.pending > 12) return null;          // don't flood the server
  img = new Image();
  TERRAIN.pending++;
  img.onload = () => { TERRAIN.pending--; if (redraw) redraw(); };
  img.onerror = () => { TERRAIN.pending--; };
  img.src = TERRAIN.info.url
    .replace('{z}', z).replace('{x}', x).replace('{y}', y);
  TERRAIN.tiles.set(key, img);
  if (TERRAIN.tiles.size > TERRAIN_TILE_CAP) {
    const oldest = TERRAIN.tiles.keys().next().value;
    TERRAIN.tiles.delete(oldest);
  }
  return null;
}

const M = {
  data: null,
  img: null,
  gameIcons: true,       // the campaign map's symbols, or plain markers
  base: 'terrain',       // 'terrain' (ground imagery) or 'kneemap'
  showTacan: true,
  view: {scale: 1, ox: 0, oy: 0},
  hiddenTeams: new Set(),
  hiddenKinds: new Set(),
  hiddenCats: new Set(['terrain']),
  showUnits: true,
  showObjectives: true,
  legendOpen: true,
  // The objectives the trigger script watches. `scriptAll` adds the places
  // named by front-line event conditions, not just the ones that end the war.
  showScript: true,
  scriptAll: false,
  scriptTargets: null,
  selected: null,        // {sort: 'unit'|'objective', item}
  hover: null,
  file: null,
  place: null,           // the unit type armed for placement
};

// Centre the map on a grid coordinate. Used by the Victory tab when you click
// an objective the trigger script watches.
let mapFocusTarget = null;
function mapFocus(x, y) {
  mapFocusTarget = {x: x, y: y};
  if (mapFocus._apply) mapFocus._apply();
}

// The teams of the open file. The Units and Objectives tabs open the same
// detail pages as the map without the map having loaded, so they leave their
// own copy here.
function teamsList() {
  return (M.data && M.data.teams) || M.lastTeams || [];
}

function teamColour(team) {
  return TEAM_COLOURS[team] || TEAM_COLOURS[0];
}

// Which atlas a team draws from. `colour` is the campaign header's
// team_colour byte, and TeamColorIconIDs in src/ui/src/common/teamdata.cpp
// maps it onto white/green/blue/brown/orange/yellow/red/grey in that order.
function teamAtlas(team) {
  if (!ICONS.ready || !M.data) return null;
  const t = M.data.teams[team];
  const idx = t ? (t.colour | 0) : 0;
  const name = (ICONS.manifest.teamColours || [])[idx];
  const img = name && ICONS.images[name];
  return (img && img.complete && img.naturalWidth)
    ? {img: img, frames: ICONS.manifest.colours[name].frames} : null;
}

async function loadIcons(redraw) {
  if (ICONS.theater === S.theater) return;
  ICONS.theater = S.theater;
  ICONS.manifest = null;
  ICONS.images = {};
  ICONS.ready = false;
  let d;
  try {
    d = await api('/api/icons?theater=' + encodeURIComponent(S.theater));
  } catch (e) { return; }
  if (!d.available) return;
  ICONS.manifest = d;
  let pending = 0;
  for (const colour in d.colours) {
    const img = new Image();
    pending++;
    img.onload = img.onerror = () => {
      if (--pending === 0) { ICONS.ready = true; if (redraw) redraw(); }
    };
    img.src = d.colours[colour].url;
    ICONS.images[colour] = img;
  }
  if (!pending) ICONS.ready = true;
}

function mapReset() {
  M.data = null;
  M.img = null;
  M.selected = null;
  M.hover = null;
  M.place = null;
  M.hiddenTeams = new Set();
  M.hiddenKinds = new Set();
  M.hiddenCats = new Set(['terrain']);
}

async function mapPanel(file) {
  const data = await api('/api/map?theater=' + encodeURIComponent(S.theater) +
                         '&file=' + encodeURIComponent(file));
  const changedFile = M.file !== file;
  if (changedFile) { mapReset(); M.file = file; }
  M.data = data;

  // The script's targets, keyed by objective index. A campaign whose endgames
  // name no place (some Israel ones are event and timer only) opens showing
  // every control condition instead, or the layer would look empty.
  const script = data.script || {targets: []};
  M.scriptTargets = new Map();
  for (const t of (script.targets || [])) M.scriptTargets.set(t.n, t);
  if (changedFile) {
    M.scriptAll = !(script.targets || []).some(t => t.endgame);
  }

  const scriptTarget = o => (M.showScript && M.showObjectives
    ? M.scriptTargets.get(o.n) : null);
  const scriptVisible = t => !!t && (M.scriptAll || t.endgame);
  const scriptShown = () => [...M.scriptTargets.values()]
    .filter(t => M.scriptAll || t.endgame).length;

  const teamName = team => (data.teams[team] || {}).name || ('team ' + team);

  // "ROK must hold (any of 3) · endgame" -- what a ring means, in words.
  function scriptLabel(t) {
    const parts = [];
    const pos = t.conditions.find(c => !c.negated);
    const neg = t.conditions.find(c => c.negated);
    if (pos) {
      parts.push(teamName(pos.team) + ' must hold' +
                 (pos.mode === 'O' ? ' (any of ' + pos.count + ')' : ' (all)'));
    }
    if (neg) parts.push(teamName(neg.team) + ' must not hold');
    if (t.endgame) parts.push('endgame');
    return parts.join(' \u00b7 ');
  }

  // A ring around a place the script watches: the team's colour, solid for
  // "must hold" and dashed for a condition in an #ELSE branch. A second,
  // thinner ring marks an endgame target -- one that ends the campaign rather
  // than firing a front-line event.
  function drawScriptRing(g, t, x, y, s) {
    const cond = t.conditions.find(c => !c.negated) || t.conditions[0];
    if (!cond) return;
    g.save();
    g.translate(x, y);
    g.strokeStyle = teamColour(cond.team);
    g.lineWidth = Math.max(1.4, s / 3.5);
    if (cond.negated) g.setLineDash([3, 3]);
    g.beginPath();
    g.arc(0, 0, s * 2.1, 0, Math.PI * 2);
    g.stroke();
    if (t.endgame) {
      g.beginPath();
      g.arc(0, 0, s * 2.9, 0, Math.PI * 2);
      g.lineWidth = Math.max(1, s / 6);
      g.stroke();
    }
    g.restore();
  }

  const box = el('div', {class: 'map-canvas-box'});
  const canvas = el('canvas');
  box.appendChild(canvas);
  const hud = el('div', {class: 'map-hud', text: '—'});
  box.appendChild(hud);
  const tip = el('div', {class: 'map-tip', style: 'display:none'});
  box.appendChild(tip);

  const detail = el('div', {class: 'detail'});

  let scriptLegend = null;
  let scriptLegendCount = null;
  const scriptCountText = () => scriptShown() + ' of ' + script.targets.length +
    ' place' + (script.targets.length === 1 ? '' : 's') + ' shown';

  // The layer toggles appear both here and in the legend, so they are built
  // once and handed to whichever container asks; `refreshSidebar` rebuilds the
  // sidebar copy after either one changes.
  function renderScriptToggles() {
    const box = el('div', {class: 'script-toggles'});
    const sync = () => {
      draw();
      refreshSidebar();
      if (scriptLegend) {
        scriptLegend.replaceWith(scriptLegend = renderScriptToggles());
        scriptLegend.classList.add('legend-script');
      }
      if (scriptLegendCount) scriptLegendCount.textContent = scriptCountText();
    };
    box.appendChild(el('label', {class: 'chk'}, [
      el('input', {type: 'checkbox', checked: M.showScript,
        onchange: e => { M.showScript = e.target.checked; sync(); }}),
      el('span', {text: 'Rings on the map'}),
    ]));
    if (script.targets.some(t => !t.endgame)) {
      box.appendChild(el('label', {class: 'chk'}, [
        el('input', {type: 'checkbox', checked: M.scriptAll,
          onchange: e => { M.scriptAll = e.target.checked; sync(); }}),
        el('span', {text: 'include event conditions'}),
      ]));
    }
    return box;
  }

  // The map's copy of the Victory tab's question: which places does the script
  // actually watch, and what has to happen to them. The rings are the fast
  // version; this is the readable one, and where a place can be clicked.
  function scriptPanel() {
    if (!script.available || !M.scriptTargets.size) return null;

    const byN = new Map(data.objectives.map(o => [o.n, o]));
    const box = el('div', {class: 'group'});
    box.appendChild(el('h4', {text: 'Objectives the script watches'}));
    box.appendChild(el('p', {class: 'note', text:
      script.file + (script.fromScenario
        ? ' \u2014 the script of ' + script.fromScenario : '') +
      ' names ' + script.targets.length + ' place' +
      (script.targets.length === 1 ? '' : 's') + ' in control conditions. ' +
      'Solid ring: the team must hold it. Dashed: it must not. Double ring: ' +
      'the condition ends the campaign.'}));
    if (!M.scriptAll && !script.targets.some(t => t.endgame)) {
      box.appendChild(el('p', {class: 'note warn', text:
        'No endgame in this script names a place \u2014 it ends on events or ' +
        'a timer \u2014 so every control condition is shown.'}));
    }
    box.appendChild(renderScriptToggles());

    // One row per condition, not per place: the same objective can be named by
    // several conditions, with different teams and polarities, and two
    // conditions that happen to share a team and mode are still separate sets.
    const groups = new Map();
    for (const t of script.targets) {
      if (!M.scriptAll && !t.endgame) continue;
      for (const c of t.conditions) {
        const key = [c.team, c.mode, c.negated, t.endgame, c.line].join('|');
        let g = groups.get(key);
        if (!g) {
          g = {team: c.team, mode: c.mode, negated: c.negated,
               count: c.count, endgame: t.endgame, items: []};
          groups.set(key, g);
        }
        g.items.push(t);
      }
    }
    const rows = [...groups.values()].sort((a, b) =>
      (b.endgame - a.endgame) || (a.team - b.team) ||
      (a.negated - b.negated) || (a.mode < b.mode ? 1 : -1));
    for (const g of rows) {
      box.appendChild(el('div', {class: 'script-cond'}, [
        el('div', {class: 'script-head'}, [
          el('span', {class: 'swatch',
            style: 'background:' + teamColour(g.team)}),
          el('span', {text: teamName(g.team)}),
          el('span', {text: g.negated ? 'must not hold' : 'must hold'}),
          el('span', {class: 'hint',
            text: g.mode === 'O' ? 'any of ' + g.count
                                 : 'all of ' + g.count}),
          g.endgame ? el('span', {class: 'tag', text: 'endgame'}) : null,
        ]),
        el('div', {class: 'script-chips'}, g.items.map(t =>
          el('button', {class: 'script-chip', title: 'camp id ' + t.campId,
            onclick: () => {
              const o = byN.get(t.n);
              if (o) { mapFocus(o.x, o.y); show({sort: 'objective', item: o}); }
            }}, (byN.get(t.n) || {}).name || ('objective ' + t.campId)))),
      ]));
    }

    const notes = [];
    if (script.dead) {
      notes.push(script.dead + ' watched id' + (script.dead === 1 ? '' : 's') +
                 ' match no objective here');
    }
    if (script.otherConditions) {
      notes.push(script.otherConditions + ' other condition' +
                 (script.otherConditions === 1 ? '' : 's') +
                 ' (events, ratios, timers) name no place');
    }
    if (notes.length) {
      box.appendChild(el('p', {class: 'note', text: notes.join('; ') + '.'}));
    }
    return box;
  }

  const resetDetail = () => {
    detail.textContent = '';
    const pad = el('div', {class: 'pad'});
    pad.appendChild(el('p', {style: 'color:var(--ink-faint)', text:
      data.error
        ? 'The unit list in this file could not be decoded: ' + data.error
        : 'Click anything to inspect it. Shift-drag a unit to move it. ' +
          'Right-click the map to place a new unit.'}));
    const panel = scriptPanel();
    if (panel) pad.appendChild(panel);
    detail.appendChild(pad);
  };
  const refreshSidebar = () => { if (!M.selected) resetDetail(); };
  resetDetail();

  // --- projection ----------------------------------------------------------

  const toScreen = (gx, gy) => [
    gx * M.view.scale + M.view.ox,
    (data.height - gy) * M.view.scale + M.view.oy,
  ];
  const toGrid = (sx, sy) => [
    (sx - M.view.ox) / M.view.scale,
    data.height - (sy - M.view.oy) / M.view.scale,
  ];

  function fit() {
    const r = box.getBoundingClientRect();
    if (!r.width || !r.height) return;
    const s = Math.min(r.width / data.width, r.height / data.height) * 0.96;
    M.view.scale = s;
    M.view.ox = (r.width - data.width * s) / 2;
    M.view.oy = (r.height - data.height * s) / 2;
  }

  function zoomAt(fx, fy, factor) {
    const r = box.getBoundingClientRect();
    const px = r.width * fx, py = r.height * fy;
    const [gx, gy] = toGrid(px, py);
    M.view.scale = Math.max(0.05, Math.min(32, M.view.scale * factor));
    M.view.ox = px - gx * M.view.scale;
    M.view.oy = py - (data.height - gy) * M.view.scale;
    draw();
  }

  const unitVisible = u =>
    M.showUnits && !M.hiddenTeams.has(u.owner) && !M.hiddenKinds.has(u.kind);
  const objVisible = o =>
    M.showObjectives && !M.hiddenCats.has(o.cat) &&
    M.view.scale >= (LAYER_MIN_ZOOM[o.cat] || 0);

  const markSize = () => Math.max(4.5, Math.min(15, 7 * Math.sqrt(M.view.scale)));
  const objSize = () => Math.max(2.5, Math.min(9, 4.5 * Math.sqrt(M.view.scale)));

  // The game draws its icons at a fixed pixel size; growing them a little with
  // zoom keeps a fitted map from turning into a wall of overlapping symbols.
  const iconScale = () =>
    Math.max(0.45, Math.min(1.6, Math.sqrt(M.view.scale)));

  // Blit one icon from its team's atlas. Returns false when there is nothing
  // to draw, so the caller can fall back to the plain marker.
  function drawIcon(g, item, x, y, isSel, isHover) {
    if (!M.gameIcons || !item.icon) return false;
    const at = teamAtlas(item.owner);
    if (!at) return false;
    const f = at.frames[item.icon];
    if (!f) return false;
    const k = iconScale();
    const w = f.w * k, h = f.h * k;
    const dx = Math.round(x - f.cx * k), dy = Math.round(y - f.cy * k);
    g.drawImage(at.img, f.x, f.y, f.w, f.h, dx, dy, w, h);
    if (isSel || isHover) {
      g.beginPath();
      g.rect(dx - 2, dy - 2, w + 4, h + 4);
      g.strokeStyle = isSel ? '#ffffff' : 'rgba(255,255,255,.55)';
      g.lineWidth = isSel ? 1.6 : 1;
      g.stroke();
    }
    return true;
  }

  // --- drawing -------------------------------------------------------------

  function draw() {
    const r = box.getBoundingClientRect();
    if (!r.width || !r.height) return;
    const d = window.devicePixelRatio || 1;
    if (canvas.width !== Math.round(r.width * d) ||
        canvas.height !== Math.round(r.height * d)) {
      canvas.width = Math.round(r.width * d);
      canvas.height = Math.round(r.height * d);
    }
    const g = canvas.getContext('2d');
    g.setTransform(d, 0, 0, d, 0, 0);
    g.clearRect(0, 0, r.width, r.height);

    const w = data.width * M.view.scale, h = data.height * M.view.scale;
    g.fillStyle = '#131922';
    g.fillRect(M.view.ox, M.view.oy, w, h);

    let drewTerrain = false;
    if (M.base === 'terrain' && TERRAIN.info) {
      drewTerrain = drawTerrain(g, r);
    }
    if (!drewTerrain && M.img) {
      g.imageSmoothingEnabled = M.view.scale < 2;
      g.drawImage(M.img, M.view.ox, M.view.oy, w, h);
    }
    g.strokeStyle = 'rgba(120,150,190,.25)';
    g.lineWidth = 1;
    g.strokeRect(M.view.ox, M.view.oy, w, h);

    // Objectives first, so units sit on top of them. A script target draws
    // even when its category is below its zoom threshold -- the handful of
    // places that decide the war should be findable zoomed out.
    const os = objSize();
    const byCat = {};
    for (const o of data.objectives) {
      if (!objVisible(o) && !scriptVisible(scriptTarget(o))) continue;
      (byCat[o.cat] = byCat[o.cat] || []).push(o);
    }
    for (const [cat, glyph] of OBJ_LAYERS) {
      for (const o of (byCat[cat] || [])) {
        const [sx, sy] = toScreen(o.x, o.y);
        if (sx < -12 || sy < -12 || sx > r.width + 12 || sy > r.height + 12) continue;
        const t = scriptTarget(o);
        drawObjective(g, o, sx, sy, os, glyph,
                      isSelected('objective', o), o === M.hover);
        if (scriptVisible(t)) drawScriptRing(g, t, sx, sy, os);
      }
    }

    const size = markSize();
    for (const u of data.units) {
      if (!unitVisible(u)) continue;
      const [sx, sy] = toScreen(u.x, u.y);
      if (sx < -20 || sy < -20 || sx > r.width + 20 || sy > r.height + 20) continue;
      drawUnit(g, u, sx, sy, size, isSelected('unit', u), u === M.hover);
    }

    if (M.showTacan) drawTacan(g, r);

    if (data.bullseye && data.bullseye[0]) {
      const [bx, by] = toScreen(data.bullseye[0], data.bullseye[1]);
      g.strokeStyle = 'rgba(240,220,120,.85)';
      g.lineWidth = 1.4;
      g.beginPath(); g.arc(bx, by, 7, 0, Math.PI * 2); g.stroke();
      g.beginPath();
      g.moveTo(bx - 11, by); g.lineTo(bx + 11, by);
      g.moveTo(bx, by - 11); g.lineTo(bx, by + 11);
      g.stroke();
    }
  }

  // Pick the pyramid level whose tiles land closest to 1:1 on screen, then
  // blit every tile that overlaps the viewport. Anything still loading falls
  // through to whatever is underneath, so panning never flashes empty.
  function drawTerrain(g, r) {
    const info = TERRAIN.info;
    const maxZ = info.maxZoom;
    const scale = M.view.scale;                     // screen px per km
    // A tile at zoom z covers sizeKm >> z kilometres and is tilePx wide, so
    // it lands 1:1 on screen when (sizeKm >> z) * scale == tilePx. Round up,
    // which errs toward a sharper level rather than an upscaled one.
    let z = Math.ceil(Math.log2(
      Math.max(1e-6, info.sizeKm * scale / info.tilePx)));
    z = Math.max(info.minZoom, Math.min(maxZ, z));

    const km = info.sizeKm >> z;                    // km covered by one tile
    const span = 1 << z;
    const side = km * scale;                        // its size on screen
    if (side <= 0) return false;

    const x0 = Math.max(0, Math.floor((-M.view.ox) / side));
    const x1 = Math.min(span - 1, Math.ceil((r.width - M.view.ox) / side));
    const y0 = Math.max(0, Math.floor((-M.view.oy) / side));
    const y1 = Math.min(span - 1, Math.ceil((r.height - M.view.oy) / side));
    if (x1 < x0 || y1 < y0) return false;

    g.imageSmoothingEnabled = true;
    let drew = 0;
    for (let ty = y0; ty <= y1; ty++) {
      for (let tx = x0; tx <= x1; tx++) {
        const img = terrainTile(z, tx, ty, draw);
        if (!img) continue;
        // +1 on the size closes the hairline seams sub-pixel placement leaves.
        g.drawImage(img,
                    M.view.ox + tx * side, M.view.oy + ty * side,
                    side + 1, side + 1);
        drew++;
      }
    }
    return drew > 0;
  }

  const isSelected = (sort, item) =>
    M.selected && M.selected.sort === sort && M.selected.item === item;

  function drawObjective(g, o, x, y, s, glyph, isSel, isHover) {
    if (drawIcon(g, o, x, y, isSel, isHover)) return;
    g.save();
    g.translate(x, y);
    g.lineWidth = 1;
    const col = teamColour(o.owner);
    g.strokeStyle = 'rgba(0,0,0,.6)';
    g.fillStyle = col;

    g.beginPath();
    if (glyph === 'dot') {
      g.arc(0, 0, s * 0.42, 0, Math.PI * 2);
    } else if (glyph === 'tick') {
      g.rect(-s * 0.28, -s * 0.28, s * 0.56, s * 0.56);
    } else if (glyph === 'square') {
      g.rect(-s * 0.6, -s * 0.6, s * 1.2, s * 1.2);
    } else if (glyph === 'gear') {
      for (let i = 0; i < 6; i++) {
        const a = (i / 6) * Math.PI * 2;
        const px = Math.cos(a) * s * 0.75, py = Math.sin(a) * s * 0.75;
        if (i === 0) g.moveTo(px, py); else g.lineTo(px, py);
      }
      g.closePath();
    } else if (glyph === 'anchor') {
      g.moveTo(-s * 0.7, s * 0.5); g.lineTo(0, -s * 0.7);
      g.lineTo(s * 0.7, s * 0.5); g.closePath();
    } else if (glyph === 'flag') {
      g.rect(-s * 0.15, -s * 0.85, s * 0.3, s * 1.7);
      g.rect(s * 0.1, -s * 0.85, s * 0.8, s * 0.6);
    } else if (glyph === 'tri') {
      g.moveTo(0, -s * 0.9); g.lineTo(s * 0.8, s * 0.6);
      g.lineTo(-s * 0.8, s * 0.6); g.closePath();
    } else {                                  // runway
      g.rect(-s * 1.1, -s * 0.34, s * 2.2, s * 0.68);
    }
    g.fill();
    if (s > 3.5) g.stroke();

    if (glyph === 'runway') {
      g.beginPath();
      g.strokeStyle = 'rgba(255,255,255,.85)';
      g.lineWidth = Math.max(1, s / 5);
      g.moveTo(-s * 0.8, 0); g.lineTo(s * 0.8, 0);
      g.stroke();
    }

    if (isSel || isHover) {
      g.beginPath();
      g.arc(0, 0, s * 1.9, 0, Math.PI * 2);
      g.strokeStyle = isSel ? '#ffffff' : 'rgba(255,255,255,.55)';
      g.lineWidth = isSel ? 2 : 1.2;
      g.stroke();
    }
    g.restore();
  }

  // The station marker the game draws: name above, filled disc, channel below.
  function drawTacan(g, r) {
    if (M.view.scale < 0.55) return;                // unreadable when zoomed out
    const labels = M.view.scale >= 0.9;
    g.save();
    g.textAlign = 'center';
    g.font = '600 11px "Segoe UI", system-ui, sans-serif';
    for (const o of data.objectives) {
      if (!o.tacan) continue;
      const [sx, sy] = toScreen(o.x, o.y);
      if (sx < -60 || sy < -40 || sx > r.width + 60 || sy > r.height + 40) continue;

      g.beginPath();
      g.arc(sx, sy, 5, 0, Math.PI * 2);
      g.fillStyle = '#ffffff';
      g.fill();
      g.beginPath();
      g.arc(sx, sy, 2.2, 0, Math.PI * 2);
      g.fillStyle = '#10151c';
      g.fill();

      if (!labels) continue;
      const name = (o.name || '').toUpperCase();
      g.lineWidth = 3;
      g.strokeStyle = 'rgba(8,11,16,.85)';
      g.fillStyle = '#f2f6fb';
      if (name) {
        g.strokeText(name, sx, sy - 9);
        g.fillText(name, sx, sy - 9);
      }
      g.strokeText(o.tacan, sx, sy + 19);
      g.fillText(o.tacan, sx, sy + 19);
    }
    g.restore();
  }

  function drawUnit(g, u, x, y, s, isSel, isHover) {
    if (drawIcon(g, u, x, y, isSel, isHover)) return;
    const shape = KIND_SHAPE[u.kind] || 'bar';
    g.save();
    g.translate(x, y);
    g.lineWidth = Math.max(1, s / 6);
    g.strokeStyle = 'rgba(0,0,0,.65)';
    g.fillStyle = teamColour(u.owner);

    g.beginPath();
    if (shape === 'bar' || shape === 'bar2') {
      g.rect(-s * 0.9, -s * 0.34, s * 1.8, s * 0.68);
    } else if (shape === 'wing') {
      g.moveTo(-s, 0); g.lineTo(0, -s * 0.58);
      g.lineTo(s, 0); g.lineTo(0, s * 0.58);
      g.closePath();
    } else if (shape === 'ship') {
      g.moveTo(-s, -s * 0.3); g.lineTo(s * 0.6, -s * 0.3);
      g.lineTo(s, 0); g.lineTo(s * 0.6, s * 0.3); g.lineTo(-s, s * 0.3);
      g.closePath();
    } else {
      g.moveTo(0, -s * 0.8); g.lineTo(s * 0.8, 0);
      g.lineTo(0, s * 0.8); g.lineTo(-s * 0.8, 0);
      g.closePath();
    }
    g.fill();
    g.stroke();

    if (shape === 'bar2') {            // brigade: a lighter stripe on the bar
      g.beginPath();
      g.rect(-s * 0.9, -s * 0.34, s * 1.8, s * 0.2);
      g.fillStyle = 'rgba(255,255,255,.45)';
      g.fill();
    }

    if (isSel || isHover) {
      g.beginPath();
      g.arc(0, 0, s * 1.5, 0, Math.PI * 2);
      g.strokeStyle = isSel ? '#ffffff' : 'rgba(255,255,255,.55)';
      g.lineWidth = isSel ? 2 : 1.2;
      g.stroke();
    }
    g.restore();
  }

  // Units win ties, because they are what you are usually reaching for.
  // Pick radius has to follow whatever is actually on screen, or clicking a
  // game icon misses by half its width.
  function pickRadius(item, fallback) {
    if (M.gameIcons && item.icon) {
      const at = teamAtlas(item.owner);
      const f = at && at.frames[item.icon];
      if (f) {
        const k = iconScale();
        return Math.max(f.w, f.h) * k * 0.6;
      }
    }
    return fallback;
  }

  function pick(sx, sy) {
    const us = markSize() * 1.6;
    let best = null, bestD = Infinity;
    for (const u of data.units) {
      if (!unitVisible(u)) continue;
      const [ux, uy] = toScreen(u.x, u.y);
      const r = pickRadius(u, us);
      const d = (ux - sx) * (ux - sx) + (uy - sy) * (uy - sy);
      if (d <= r * r && d < bestD) { bestD = d; best = {sort: 'unit', item: u}; }
    }
    if (best) return best;

    const os = objSize() * 2.2;
    bestD = Infinity;
    for (const o of data.objectives) {
      if (!objVisible(o)) continue;
      const [ox, oy] = toScreen(o.x, o.y);
      const r = pickRadius(o, os);
      const d = (ox - sx) * (ox - sx) + (oy - sy) * (oy - sy);
      if (d <= r * r && d < bestD) {
        bestD = d; best = {sort: 'objective', item: o};
      }
    }
    return best;
  }

  // --- detail panes --------------------------------------------------------

  const show = guard(async (hit) => {
    M.selected = hit;
    if (!hit) { resetDetail(); draw(); return; }
    if (hit.sort === 'unit') {
      const info = await api('/api/unit?theater=' + encodeURIComponent(S.theater) +
                             '&file=' + encodeURIComponent(file) + '&n=' + hit.item.n);
      detail.textContent = '';
      detail.appendChild(await unitDetail(info, hit.item, file, draw, reload));
    } else {
      const info = await api('/api/objective?theater=' + encodeURIComponent(S.theater) +
                             '&file=' + encodeURIComponent(file) + '&n=' + hit.item.n);
      detail.textContent = '';
      detail.appendChild(objectiveDetail(info, hit.item, file, draw,
                                         () => show(hit)));
    }
    draw();
  });

  const reload = guard(async () => {
    S.rowIndex = null;
    await drawView();
  });

  // A quick action from the right-click menu reloads the whole panel. Re-open
  // whatever was selected, against the fresh data, so the sidebar does not go
  // blank under the cursor.
  if (M.selected && M.selected.item) {
    const sort = M.selected.sort;
    const pool = sort === 'unit' ? data.units : data.objectives;
    const fresh = pool.find(x => x.n === M.selected.item.n);
    if (fresh) await show({sort: sort, item: fresh});
    else { M.selected = null; resetDetail(); }
  }

  // --- interaction ---------------------------------------------------------

  let panning = false, dragUnit = null, moved = false, last = [0, 0];

  box.addEventListener('contextmenu', e => {
    e.preventDefault();
    const r = box.getBoundingClientRect();
    const sx = e.clientX - r.left, sy = e.clientY - r.top;
    const [gx, gy] = toGrid(sx, sy);
    const hit = pick(sx, sy);
    openMapMenu(box, sx, sy, Math.round(gx), Math.round(gy), hit, file, reload, show);
  });

  box.addEventListener('mousedown', e => {
    if (e.button !== 0) return;
    const menu = $('#map-menu');
    if (menu && menu.contains(e.target)) return;   // let the menu handle it
    const r = box.getBoundingClientRect();
    const sx = e.clientX - r.left, sy = e.clientY - r.top;
    closeMapMenu();
    const [gx, gy] = toGrid(sx, sy);

    if (M.place) {
      placeUnit(file, Math.round(gx), Math.round(gy), reload);
      return;
    }

    const hit = pick(sx, sy);
    moved = false;
    last = [sx, sy];
    if (hit && hit.sort === 'unit' && e.shiftKey) dragUnit = hit.item;
    else { panning = true; box.classList.add('dragging'); }
    show(hit);
  });

  const onUp = guard(async () => {
    box.classList.remove('dragging');
    const u = dragUnit;
    const didMove = moved;
    dragUnit = null;
    panning = false;
    if (u && didMove) {
      await api('/api/unit/edit', {
        theater: S.theater, file: file, n: u.n,
        values: {x: Math.round(u.x), y: Math.round(u.y)},
      });
      toast('Moved ' + (u.name || u.kind) + ' to ' + u.x + ', ' + u.y, 'ok');
      await refreshPending();
    }
  });
  window.addEventListener('mouseup', onUp);

  box.addEventListener('mousemove', e => {
    const r = box.getBoundingClientRect();
    const sx = e.clientX - r.left, sy = e.clientY - r.top;
    const [gx, gy] = toGrid(sx, sy);
    hud.textContent = Math.round(gx) + ', ' + Math.round(gy) + '   ' +
                      M.view.scale.toFixed(2) + 'x' +
                      (M.place ? '   placing ' + M.place.name : '');

    if (dragUnit) {
      moved = true;
      dragUnit.x = Math.max(0, Math.min(data.width - 1, Math.round(gx)));
      dragUnit.y = Math.max(0, Math.min(data.height - 1, Math.round(gy)));
      draw();
      return;
    }
    if (panning) {
      moved = true;
      M.view.ox += sx - last[0];
      M.view.oy += sy - last[1];
      last = [sx, sy];
      draw();
      return;
    }

    const hit = pick(sx, sy);
    const item = hit && hit.item;
    if (item !== M.hover) { M.hover = item; draw(); }
    if (hit) {
      const t = data.teams[item.owner];
      const who = (t && t.name) || ('team ' + item.owner);
      if (hit.sort === 'unit') {
        const bits = [item.sqName || item.name || item.kind];
        if (item.aircraft) bits.push(item.aircraft);
        bits.push(who, item.x + ', ' + item.y);
        if (item.supply !== undefined) bits.push('supply ' + item.supply);
        if (item.losses) bits.push('losses ' + item.losses + '%');
        tip.textContent = bits.join('  ·  ');
      } else {
        const bits = [item.name || item.type, item.type, who];
        const t = scriptTarget(item);
        if (t) bits.push(scriptLabel(t));
        tip.textContent = bits.join('  \u00b7  ');
      }
      tip.style.display = '';
      tip.style.left = Math.min(sx + 14, r.width - tip.offsetWidth - 8) + 'px';
      tip.style.top = (sy + 16) + 'px';
    } else {
      tip.style.display = 'none';
    }
  });

  box.addEventListener('mouseleave', () => {
    tip.style.display = 'none';
    if (M.hover) { M.hover = null; draw(); }
  });

  box.addEventListener('wheel', e => {
    e.preventDefault();
    const r = box.getBoundingClientRect();
    zoomAt((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height,
           e.deltaY < 0 ? 1.15 : 1 / 1.15);
  }, {passive: false});

  window.addEventListener('keydown', function esc(e) {
    if (e.key === 'Escape' && M.place) { M.place = null; drawArmed(); }
  });

  // --- legend --------------------------------------------------------------

  const perTeam = new Map(), perKind = new Map(), perCat = new Map();
  const perTeamObj = new Map(), perTeamAB = new Map();
  for (const u of data.units) {
    perTeam.set(u.owner, (perTeam.get(u.owner) || 0) + 1);
    perKind.set(u.kind, (perKind.get(u.kind) || 0) + 1);
  }
  for (const o of data.objectives) {
    perCat.set(o.cat, (perCat.get(o.cat) || 0) + 1);
    perTeamObj.set(o.owner, (perTeamObj.get(o.owner) || 0) + 1);
    if (o.cat === 'airbase') {
      perTeamAB.set(o.owner, (perTeamAB.get(o.owner) || 0) + 1);
    }
  }

  const legend = el('div', {class: 'map-legend' + (M.legendOpen ? '' : ' shut')});
  legend.appendChild(el('button', {class: 'legend-toggle',
    title: 'Show or hide the layer list',
    onclick: () => {
      M.legendOpen = !M.legendOpen;
      legend.classList.toggle('shut', !M.legendOpen);
    }}, 'Layers'));
  const section = (title, toggle) => {
    const h = el('h5', {}, [title]);
    if (toggle) h.appendChild(toggle);
    legend.appendChild(h);
  };

  // Biggest holder first: the question this section answers is "who is the
  // enemy here", and that is the objectives far more than the unit count.
  section('Teams');
  const teamRows = [...new Set([...perTeam.keys(), ...perTeamObj.keys()])]
    .map(team => [team, perTeam.get(team) || 0, perTeamObj.get(team) || 0,
                  perTeamAB.get(team) || 0])
    .sort((a, b) => (b[2] - a[2]) || (b[1] - a[1]) || (a[0] - b[0]));
  for (const [team, n, objs, bases] of teamRows) {
    const t = data.teams[team];
    const detail = objs + ' objective' + (objs === 1 ? '' : 's') +
                   ' \u00b7 ' + bases + ' airbase' + (bases === 1 ? '' : 's');
    legend.appendChild(el('label', {
      title: ((t && t.name) || ('team ' + team)) + ': ' + n + ' unit' +
             (n === 1 ? '' : 's') + ', ' + detail,
    }, [
      el('input', {type: 'checkbox', checked: !M.hiddenTeams.has(team),
        onchange: e => {
          if (e.target.checked) M.hiddenTeams.delete(team);
          else M.hiddenTeams.add(team);
          draw();
        }}),
      el('span', {class: 'swatch', style: 'background:' + teamColour(team)}),
      el('span', {text: (t && t.name) || ('team ' + team)}),
      el('span', {class: 'n', text: String(n)}),
      el('span', {class: 'sub', text: detail}),
    ]));
  }

  section('Units', el('button', {class: 'mini', onclick: e => {
    M.showUnits = !M.showUnits;
    e.target.textContent = M.showUnits ? 'hide' : 'show';
    draw();
  }}, M.showUnits ? 'hide' : 'show'));
  for (const [kind, n] of [...perKind.entries()].sort((a, b) => b[1] - a[1])) {
    legend.appendChild(el('label', {}, [
      el('input', {type: 'checkbox', checked: !M.hiddenKinds.has(kind),
        onchange: e => {
          if (e.target.checked) M.hiddenKinds.delete(kind);
          else M.hiddenKinds.add(kind);
          draw();
        }}),
      el('span', {text: kind}),
      el('span', {class: 'n', text: String(n)}),
    ]));
  }

  if (perCat.size) {
    section('Objectives', el('button', {class: 'mini', onclick: e => {
      M.showObjectives = !M.showObjectives;
      e.target.textContent = M.showObjectives ? 'hide' : 'show';
      draw();
    }}, M.showObjectives ? 'hide' : 'show'));
    for (const [cat] of OBJ_LAYERS) {
      const n = perCat.get(cat);
      if (!n) continue;
      legend.appendChild(el('label', {}, [
        el('input', {type: 'checkbox', checked: !M.hiddenCats.has(cat),
          onchange: e => {
            if (e.target.checked) M.hiddenCats.delete(cat);
            else M.hiddenCats.add(cat);
            draw();
          }}),
        el('span', {text: data.categories[cat] || cat}),
        el('span', {class: 'n', text: String(n)}),
      ]));
    }
  }
  if (script.available && script.targets.length) {
    section('Script');
    scriptLegend = renderScriptToggles();
    scriptLegend.classList.add('legend-script');
    legend.appendChild(scriptLegend);
    scriptLegendCount = el('div', {class: 'hint', text: scriptCountText()});
    legend.appendChild(scriptLegendCount);
  }
  if (data.bullseye && data.bullseye[0]) {
    section('Reference');
    legend.appendChild(el('div', {class: 'bullseye-key'}, [
      el('span', {class: 'bullseye-mark', text: '\u271b'}),
      el('span', {text: 'Bullseye ' + data.bullseye[0] + ', ' +
                        data.bullseye[1]}),
    ]));
  }
  box.appendChild(legend);

  // --- armed-placement banner ---------------------------------------------

  const armed = el('div', {class: 'map-armed', style: 'display:none'});
  box.appendChild(armed);

  function drawArmed() {
    if (!M.place) { armed.style.display = 'none'; box.classList.remove('placing'); return; }
    armed.style.display = '';
    box.classList.add('placing');
    armed.textContent = '';
    const t = data.teams[M.place.owner];
    armed.appendChild(el('span', {class: 'swatch',
      style: 'background:' + teamColour(M.place.owner)}));
    armed.appendChild(el('span', {text: 'Click to place ' + M.place.name +
      ' (' + ((t && t.name) || ('team ' + M.place.owner)) + ')'}));
    armed.appendChild(el('button', {class: 'mini', onclick: () => {
      M.place = null; drawArmed();
    }}, 'cancel (esc)'));
  }
  mapPanel._drawArmed = drawArmed;
  drawArmed();

  // --- toolbar -------------------------------------------------------------

  const toolbar = el('div', {class: 'toolbar'}, [
    el('span', {class: 'stat', text:
      data.units.length.toLocaleString() + ' units  ·  ' +
      data.objectives.length.toLocaleString() + ' objectives  ·  ' +
      data.width + ' x ' + data.height + ' km'}),
    data.objectivesFrom !== file
      ? el('span', {class: 'stat warn',
          text: 'objectives from ' + data.objectivesFrom})
      : null,
    el('span', {class: 'spacer'}),
    el('button', {class: 'btn btn-sm btn-primary',
      onclick: () => openPlacePicker(file)}, 'Place unit…'),
    el('button', {class: 'btn btn-sm' + (M.gameIcons ? ' on' : ''),
      title: 'Draw the campaign map’s own symbols, or plain markers',
      onclick: e => {
        M.gameIcons = !M.gameIcons;
        e.target.classList.toggle('on', M.gameIcons);
        draw();
      }}, 'Game icons'),
    data.hasTerrain
      ? el('select', {class: 'mini-sel', title: 'Base layer',
          onchange: e => { M.base = e.target.value; draw(); }}, [
          el('option', {value: 'terrain', selected: M.base === 'terrain'},
             'Terrain'),
          el('option', {value: 'kneemap', selected: M.base === 'kneemap'},
             'Kneeboard'),
        ])
      : null,
    data.tacanCount
      ? el('button', {class: 'btn btn-sm' + (M.showTacan ? ' on' : ''),
          title: data.tacanCount + ' airbase TACAN stations',
          onclick: e => {
            M.showTacan = !M.showTacan;
            e.target.classList.toggle('on', M.showTacan);
            draw();
          }}, 'TACAN')
      : null,
    el('button', {class: 'btn btn-sm', onclick: () => { fit(); draw(); }}, 'Fit'),
    el('button', {class: 'btn btn-sm', onclick: () => zoomAt(0.5, 0.5, 1.4)}, '+'),
    el('button', {class: 'btn btn-sm', onclick: () => zoomAt(0.5, 0.5, 1 / 1.4)}, '−'),
  ]);

  const wrap = el('div', {class: 'map-wrap'},
                  [el('div', {class: 'map-pane'}, [toolbar, box]), detail]);

  // The panel is built before it is in the document, so its box has no size
  // yet: fitting here would centre on nothing. drawView calls _afterAttach
  // once the node is in the tree and laid out.
  let fitted = false;
  mapFocus._apply = () => {
    if (!mapFocusTarget) return;
    const r = box.getBoundingClientRect();
    if (!r.width || !r.height) return;
    M.view.scale = Math.max(M.view.scale, 6);
    M.view.ox = r.width / 2 - mapFocusTarget.x * M.view.scale;
    M.view.oy = r.height / 2 - (data.height - mapFocusTarget.y) * M.view.scale;
    mapFocusTarget = null;
    fitted = true;          // do not re-fit over the caller's choice
    draw();
  };
  wrap._afterAttach = () => {
    const r = box.getBoundingClientRect();
    if (!r.width || !r.height) return;
    if (mapFocusTarget) { mapFocus._apply(); return; }
    if (!fitted) { fitted = true; fit(); }
    draw();
  };
  const ro = new ResizeObserver(() => wrap._afterAttach());
  ro.observe(box);
  if (mapPanel._ro) mapPanel._ro.disconnect();
  mapPanel._ro = ro;

  if (data.hasImage) {
    const img = new Image();
    img.onload = () => { M.img = img; draw(); };
    img.src = '/api/map/image?theater=' + encodeURIComponent(S.theater) +
              '&file=' + encodeURIComponent(file);
  }
  loadIcons(draw);
  loadTerrain(draw);
  if (ICONS.ready) draw();

  return wrap;
}

// --- placement ---------------------------------------------------------------

let PLACEABLE = null;

async function loadPlaceable() {
  if (PLACEABLE && PLACEABLE.theater === S.theater) return PLACEABLE;
  const d = await api('/api/placeable?theater=' + encodeURIComponent(S.theater));
  PLACEABLE = {theater: S.theater, items: d.items, orders: d.orders,
               moveTypes: d.moveTypes};
  return PLACEABLE;
}

// guard() is defined in app.js, which the page loads after this file, so it
// cannot be called while this module is still evaluating. Wrap at call time.
async function openPlacePickerImpl(file, gx, gy) {
  const p = await loadPlaceable();
  const body = $('#modal-body');
  body.textContent = '';

  const teams = teamsList().filter(t => t.name && t.name !== 'XX');
  const teamSel = el('select', {}, teams.map(t =>
    el('option', {value: t.index, selected: t.index === (M.place ? M.place.owner : 6)},
       t.index + '  ' + t.name)));
  const kindSel = el('select', {}, ['battalion', 'brigade', 'taskforce'].map(k =>
    el('option', {value: k}, k)));
  const search = el('input', {type: 'text', placeholder: 'Filter by name…',
                              spellcheck: 'false'});
  const orderSel = el('select', {}, p.orders.map((o, i) =>
    el('option', {value: i, selected: i === 6}, o)));
  const list = el('div', {class: 'pick-list'});

  function refill() {
    const kind = kindSel.value;
    const needle = search.value.trim().toLowerCase();
    const team = Number(teamSel.value);
    list.textContent = '';
    const items = p.items.filter(i =>
      i.kind === kind &&
      (!needle || i.name.toLowerCase().includes(needle) ||
       i.makeup.join(' ').toLowerCase().includes(needle)));
    for (const i of items.slice(0, 500)) {
      list.appendChild(el('button', {
        class: 'pick',
        onclick: () => {
          M.place = {classIndex: i.classIndex, name: i.name, kind: i.kind,
                     owner: team, orders: Number(orderSel.value)};
          $('#modal').hidden = true;
          if (mapPanel._drawArmed) mapPanel._drawArmed();
          if (gx !== undefined) placeUnit(file, gx, gy, null);
          else toast('Click the map to place ' + i.name, 'info');
        },
      }, [
        el('span', {class: 'nm', text: i.name}),
        el('span', {class: 'cc', text: i.makeup.join(', ')}),
        el('span', {class: 'n', text: i.vehicles + ' grp'}),
      ]));
    }
    if (!items.length) list.appendChild(el('div', {class: 'pad',
      style: 'color:var(--ink-faint)', text: 'Nothing matches.'}));
  }

  kindSel.addEventListener('change', refill);
  teamSel.addEventListener('change', refill);
  search.addEventListener('input', refill);
  refill();

  body.appendChild(el('div', {class: 'grid3'}, [
    el('label', {class: 'field'}, [el('span', {text: 'Team'}), teamSel]),
    el('label', {class: 'field'}, [el('span', {text: 'Kind'}), kindSel]),
    el('label', {class: 'field'}, [el('span', {text: 'Orders'}), orderSel]),
  ]));
  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Type'}), search]));
  body.appendChild(list);
  body.appendChild(el('p', {class: 'note', text:
    'A placed unit starts at full strength with no waypoints and no parent ' +
    'formation. The campaign engine gives it orders on the first tick.'}));

  $('#modal-title').textContent = gx === undefined
    ? 'Place a unit' : 'Place a unit at ' + gx + ', ' + gy;
  $('#modal-ok').style.display = 'none';
  $('#modal').hidden = false;
}

const openPlacePicker = (...a) => guard(openPlacePickerImpl)(...a);

async function placeUnitImpl(file, gx, gy, reload) {
  if (!M.place) return;
  const res = await api('/api/unit/add', {
    theater: S.theater, file: file,
    classIndex: M.place.classIndex,
    x: gx, y: gy, owner: M.place.owner, orders: M.place.orders,
  });
  toast('Placed ' + (res.name || M.place.name) + ' at ' + gx + ', ' + gy +
        '  (unit ' + res.n + ' of ' + res.total + ')', 'ok');
  await refreshPending();
  if (reload) await reload();
  else await drawView();
}

const placeUnit = (...a) => guard(placeUnitImpl)(...a);

// --- right-click menu --------------------------------------------------------

let mapMenuOutside = null;

function closeMapMenu() {
  const m = $('#map-menu');
  if (m) m.remove();
  if (mapMenuOutside) {
    window.removeEventListener('mousedown', mapMenuOutside, true);
    mapMenuOutside = null;
  }
}

function openMapMenu(box, sx, sy, gx, gy, hit, file, reload, show) {
  closeMapMenu();
  const menu = el('div', {class: 'map-menu', id: 'map-menu'});
  const item = (label, fn, cls) => menu.appendChild(
    el('button', {class: cls || '', onclick: () => { closeMapMenu(); fn(); }}, label));
  const sep = () => menu.appendChild(el('div', {class: 'sep'}));
  const head = (text) => menu.appendChild(el('div', {class: 'head', text: text}));

  // A labelled dropdown inside the menu, for the two fields a click should
  // not have to open the sidebar for: orders and team.
  const dropdown = (label, options, value, apply) => {
    const sel = el('select', {class: 'menu-select'}, options.map(o =>
      el('option', {value: o.value, selected: o.value === value}, o.label)));
    sel.addEventListener('change', guard(async () => { apply(Number(sel.value)); }));
    menu.appendChild(el('label', {class: 'menu-field'}, [
      el('span', {text: label}), sel]));
  };

  const setUnit = (u, values, note) => guard(async () => {
    await api('/api/unit/edit',
              {theater: S.theater, file: file, n: u.n, values: values});
    toast((note || 'Updated') + ' — ' + (u.sqName || u.name || u.kind), 'ok');
    await refreshPending();
    await reload();
  });

  menu.appendChild(el('div', {class: 'head', text: gx + ', ' + gy}));

  item('Place a unit here…', () => openPlacePicker(file, gx, gy));
  if (M.place) {
    item('Place ' + M.place.name + ' here',
         () => placeUnit(file, gx, gy, reload));
  }

  if (hit && hit.sort === 'unit') {
    const u = hit.item;
    const label = u.sqName || u.name || u.kind;
    sep();
    head(label);
    item('Inspect', () => show(hit));
    item('Move here', setUnit(u, {x: gx, y: gy}, 'Moved'));
    item('Duplicate here', guard(async () => {
      const res = await api('/api/unit/add', {
        theater: S.theater, file: file, classIndex: u.type,
        x: gx, y: gy, owner: u.owner,
      });
      toast('Duplicated as unit ' + res.n, 'ok');
      await refreshPending();
      await reload();
    }));

    // The condition quick actions, exactly the fields the sidebar edits.
    sep();
    if (u.supply !== undefined) {
      item('Supply to 100', setUnit(u, {supply: 100}, 'Resupplied'));
    }
    if (u.morale !== undefined) {
      item('Morale to 100', setUnit(u, {morale: 100}, 'Morale restored'));
    }
    if (u.fatigue !== undefined) {
      item('Clear fatigue', setUnit(u, {fatigue: 0}, 'Fatigue cleared'));
    }
    if (u.losses !== undefined) {
      item('Clear losses', setUnit(u, {losses: 0}, 'Losses cleared'));
    }
    if (u.orders !== undefined) {
      dropdown('Orders', ((M.data && M.data.orders) || []).map((o, i) =>
        ({value: i, label: i + '  ' + o})), u.orders,
        v => setUnit(u, {orders: v}, 'Orders set')());
    }
    if (u.kind === 'squadron') {
      sep();
      item('Resupply squadron', guard(async () => {
        const r = await api('/api/unit/stores',
          {theater: S.theater, file: file, n: u.n, action: 'resupply'});
        toast(r.changed + ' weapon(s) refilled', 'ok');
        await refreshPending();
        await reload();
      }));
      item('Empty the rack', guard(async () => {
        const r = await api('/api/unit/stores',
          {theater: S.theater, file: file, n: u.n, action: 'clear'});
        toast(r.changed + ' weapon(s) emptied', 'ok');
        await refreshPending();
        await reload();
      }));
    }

    sep();
    dropdown('Team', teamsList().map(t =>
      ({value: t.index, label: t.index + '  ' + (t.name || ('team ' + t.index))})),
      u.owner, v => setUnit(u, {owner: v}, 'Team changed')());
    item('Delete', guard(async () => {
      const res = await api('/api/unit/delete',
                            {theater: S.theater, file: file, n: u.n});
      toast('Deleted. ' + res.total + ' units left.', 'ok');
      M.selected = null;
      await refreshPending();
      await reload();
    }), 'danger');
  } else if (hit && hit.sort === 'objective') {
    const o = hit.item;
    const setObj = (body, note) => guard(async () => {
      await api('/api/objective/edit',
                {theater: S.theater, file: file, n: o.n, values: body});
      toast(note + ' — ' + (o.name || o.type), 'ok');
      await refreshPending();
      await reload();
    });
    const featureAction = (action, note) => guard(async () => {
      const r = await api('/api/objective/feature',
        {theater: S.theater, file: file, n: o.n, action: action});
      toast(r.changed + ' ' + note + ' — ' + (o.name || o.type), 'ok');
      await refreshPending();
      await reload();
    });

    sep();
    head(o.name || o.type);
    item('Inspect', () => show(hit));
    item('Repair all features', featureAction('repair-all', 'features repaired'));
    item('Destroy all features',
         featureAction('destroy-all', 'features destroyed'));
    item('Supply and fuel to 100', setObj({supply: 100, fuel: 100}, 'Resupplied'));
    dropdown('Team', teamsList().map(t =>
      ({value: t.index, label: t.index + '  ' + (t.name || ('team ' + t.index))})),
      o.owner, v => setObj({owner: v}, 'Team changed')());
  }

  box.appendChild(menu);
  const r = box.getBoundingClientRect();
  menu.style.left = Math.min(sx, r.width - menu.offsetWidth - 8) + 'px';
  menu.style.top = Math.min(sy, r.height - menu.offsetHeight - 8) + 'px';

  // Close on a click anywhere else, but not on the menu itself -- removing it
  // during mousedown would eat the click that follows.
  mapMenuOutside = (e) => { if (!menu.contains(e.target)) closeMapMenu(); };
  setTimeout(() => {
    window.addEventListener('mousedown', mapMenuOutside, true);
  }, 0);
}

// --- detail panes ------------------------------------------------------------

async function unitDetail(info, u, file, redraw, reload) {
  const wrap = el('div');
  const v = info.values;

  const pending = {};
  // Every field the pane edits registers its input here, so a value the
  // server clamped (supply 999 -> 100) is shown back as what was written.
  const fields = {};
  const push = guard(async () => {
    if (!Object.keys(pending).length) return;
    const res = await api('/api/unit/edit',
              {theater: S.theater, file: file, n: info.n, values: pending});
    for (const k in pending) {
      const got = (res.values && res.values[k] !== undefined)
        ? res.values[k] : pending[k];
      v[k] = got;
      u[k] = got;
      if (fields[k]) fields[k].value = String(got);
      delete pending[k];
    }
    redraw();
    await refreshPending();
  });

  const num = (key, label, lo, hi) => {
    const input = el('input', {type: 'number', value: String(v[key]),
      min: lo === undefined ? null : String(lo),
      max: hi === undefined ? null : String(hi),
      oninput: e => {
        pending[key] = Number(e.target.value);
        e.target.classList.add('changed');
      },
      onchange: push});
    fields[key] = input;
    return el('label', {class: 'field'}, [
      el('span', {text: label || key}), input,
    ]);
  };

  // A percentage with a bar under it, the same glanceable shape the stores
  // availability uses.
  const pct = (key, label) => {
    const val = Math.max(0, Math.min(100, Number(v[key]) || 0));
    const fill = el('div', {
      class: 'cond-fill ' + (key === 'losses' ? 'bad' : 'good'),
      style: 'width:' + val + '%'});
    const input = el('input', {type: 'number', min: '0', max: '100',
      value: String(v[key]),
      oninput: e => {
        pending[key] = Number(e.target.value);
        fill.style.width =
          Math.max(0, Math.min(100, Number(e.target.value) || 0)) + '%';
        e.target.classList.add('changed');
      },
      onchange: push});
    fields[key] = input;
    return el('label', {class: 'field'}, [
      el('span', {text: label}), input,
      el('div', {class: 'cond-bar'}, fill),
    ]);
  };

  wrap.appendChild(el('div', {class: 'detail-head'}, [
    el('h3', {text: info.title || info.name || info.kind}),
    el('div', {class: 'sub mono',
      text: info.kind + '  ·  camp id ' + v.campId +
            '  ·  class row ' + v.classIndex}),
    el('div', {class: 'row-actions'}, [
      el('button', {class: 'btn btn-sm btn-danger', onclick: guard(async () => {
        const res = await api('/api/unit/delete',
                              {theater: S.theater, file: file, n: info.n});
        toast('Deleted. ' + res.total + ' units left.', 'ok');
        M.selected = null;
        await refreshPending();
        await reload();
      })}, 'Delete unit'),
    ]),
  ]));

  // Condition first: it is what the campaign screen leads with, and it is
  // what you reach for while the map is on screen.
  const COND = [['supply', 'supply %'], ['morale', 'morale %'],
                ['fatigue', 'fatigue %'], ['losses', 'losses %']];
  const present = COND.filter(([k]) => v[k] !== undefined);
  const orders = (M.data && M.data.orders) || [];
  if (present.length || v.orders !== undefined || v.fuel !== undefined) {
    const box = el('div', {class: 'group'});
    box.appendChild(el('h4', {text: 'Condition'}));
    const grid = el('div', {class: 'grid2'});
    for (const [k, label] of present) grid.appendChild(pct(k, label));
    if (v.fuel !== undefined) grid.appendChild(num('fuel', 'fuel'));
    if (v.orders !== undefined) {
      grid.appendChild(el('label', {class: 'field'}, [
        el('span', {text: 'orders'}),
        el('select', {onchange: e => {
          pending.orders = Number(e.target.value); push();
        }}, orders.map((o, i) =>
          el('option', {value: i, selected: i === v.orders}, i + '  ' + o))),
      ]));
    }
    box.appendChild(grid);
    wrap.appendChild(box);
  }

  wrap.appendChild(el('div', {class: 'group'}, [
    el('h4', {text: 'Position and allegiance'}),
    el('div', {class: 'grid2'}, [num('x', 'grid x (east)'),
                                 num('y', 'grid y (north)')]),
    el('label', {class: 'field'}, [
      el('span', {text: 'owner'}),
      el('select', {onchange: e => { pending.owner = Number(e.target.value); push(); }},
         teamsList().map(t =>
           el('option', {value: t.index, selected: t.index === v.owner},
              t.index + '  ' + (t.name || 'team ' + t.index)))),
    ]),
  ]));

  {
    const head = squadronHeader(info);
    if (head) wrap.appendChild(head);
    const stores = storesPanel(file, info);
    if (stores) wrap.appendChild(stores);
  }

  // The aeroplane's hardpoints, editable here because this is where a
  // squadron is being looked at -- the same control the Database tab uses.
  if (info.loadout && info.loadout.hardpoints) {
    const lo = info.loadout;
    wrap.appendChild(el('p', {class: 'note warn', text:
      'The hardpoints below are class data: they belong to every '
      + (lo.aircraft || 'aircraft') + ' in this theater, and ' + lo.sharedWith
      + ' unit type' + (lo.sharedWith === 1 ? '' : 's') + ' fly it. Editing '
      + 'here moves all of them.'}));
    wrap.appendChild(await hardpointEditor({
      weapon: lo.hardpoints.map(h => h.weapon),
      shots: lo.hardpoints.map(h => h.shots),
      resolved: lo.hardpoints.map(h => h.name),
      title: 'Aircraft loadout — ' + (lo.aircraft || ''),
      onCommit: guard(async (weapon, shots) => {
        await api('/api/edit', {theater: S.theater, table: 'vehicle',
                                index: lo.vehicleRow,
                                values: {Weapon: weapon, Weapons: shots}});
        toast('Hardpoints updated for every ' + (lo.aircraft || 'aircraft'));
        await refreshPending();
      }),
    }));
  }

  if (info.composition && info.composition.length) {
    wrap.appendChild(el('div', {class: 'group'}, [
      el('h4', {text: 'Composition, from the unit table'}),
      el('ul', {class: 'comp'}, info.composition.map(c =>
        el('li', {}, [el('span', {text: c.name || ('type ' + c.type)}),
                      el('span', {class: 'n', text: 'x' + c.count})]))),
      el('button', {class: 'btn btn-sm', style: 'margin-top:8px',
        onclick: guard(async () => {
          openTable('unit', info.name || '');
        })}, 'Edit this unit type'),
    ]));
  }

  const READOUT = [
    ['roster', 'roster'], ['unitFlags', 'flags'], ['moved', 'moved'],
    ['division', 'division'], ['specialty', 'specialty'],
    ['missionsFlown', 'missions flown'], ['totalLosses', 'total losses'],
    ['mission', 'mission'], ['elements', 'elements'],
  ];
  const stats = [];
  for (const [k, label] of READOUT) {
    if (v[k] === undefined) continue;
    stats.push(el('div', {class: 'field'}, [
      el('span', {text: label}),
      el('div', {class: 'mono', text: String(v[k])}),
    ]));
  }
  if (stats.length) {
    wrap.appendChild(el('div', {class: 'group'}, [
      el('h4', {text: 'State, as saved'}),
      el('div', {class: 'grid3'}, stats),
    ]));
  }

  if (info.waypoints && info.waypoints.length) {
    wrap.appendChild(el('div', {class: 'group'}, [
      el('h4', {text: 'Waypoints'}),
      el('ul', {class: 'wp-list'}, info.waypoints.map((w, i) =>
        el('li', {}, [
          el('span', {class: 'i', text: String(i)}),
          el('span', {class: 'mono', text: w.x + ', ' + w.y + '   alt ' + w.z}),
          el('span', {class: 'i', text: 'act ' + w.action}),
        ]))),
    ]));
  }

  return wrap;
}

function objectiveDetail(info, o, file, redraw, reload) {
  const wrap = el('div');
  const v = info.values;
  const canEdit = info.canEdit;

  const pending = {};
  const fields = {};
  const push = guard(async () => {
    if (!Object.keys(pending).length) return;
    const res = await api('/api/objective/edit',
              {theater: S.theater, file: file, n: info.n, values: pending});
    for (const k in pending) {
      const got = (res.values && res.values[k] !== undefined)
        ? res.values[k] : pending[k];
      v[k] = got;
      o[k] = got;
      if (fields[k]) fields[k].value = String(got);
      delete pending[k];
    }
    redraw();
    await refreshPending();
  });

  wrap.appendChild(el('div', {class: 'detail-head'}, [
    el('h3', {text: info.name || info.typeName}),
    el('div', {class: 'sub mono',
      text: info.typeName + '  ·  camp id ' + v.campId +
            '  ·  class row ' + v.classIndex}),
    el('div', {class: 'sub', text: info.className}),
  ]));

  if (!canEdit) {
    wrap.appendChild(el('div', {class: 'group'},
      el('p', {class: 'note warn', text:
        'This file has no objective list of its own, so objectives here are ' +
        'read-only. Open the scenario it was started from to edit them.'})));
  }

  const num = (key, label, lo, hi) => {
    const input = el('input', {type: 'number', value: String(v[key]),
      disabled: !canEdit,
      min: lo === undefined ? null : String(lo),
      max: hi === undefined ? null : String(hi),
      oninput: e => {
        pending[key] = Number(e.target.value);
        e.target.classList.add('changed');
      },
      onchange: push});
    fields[key] = input;
    return el('label', {class: 'field'}, [
      el('span', {text: label || key}), input,
    ]);
  };

  wrap.appendChild(el('div', {class: 'group'}, [
    el('h4', {text: 'Position and allegiance'}),
    el('div', {class: 'grid2'}, [num('x', 'grid x (east)'),
                                 num('y', 'grid y (north)')]),
    el('label', {class: 'field'}, [
      el('span', {text: 'owner'}),
      el('select', {disabled: !canEdit,
        onchange: e => { pending.owner = Number(e.target.value); push(); }},
        teamsList().map(t =>
          el('option', {value: t.index, selected: t.index === v.owner},
             t.index + '  ' + (t.name || 'team ' + t.index)))),
    ]),
    num('priority', 'priority'),
  ]));

  // Condition: supply and fuel drive the sortie rate, losses are what the
  // side has already lost. Editable here so an airbase can be resupplied or
  // run down without leaving the map.
  wrap.appendChild(el('div', {class: 'group'}, [
    el('h4', {text: 'Condition'}),
    el('div', {class: 'grid3'}, [
      num('supply', 'supply %', 0, 100),
      num('fuel', 'fuel %', 0, 100),
      num('losses', 'losses %', 0, 100),
    ]),
  ]));

  if (info.tacan) {
    const t = info.tacan;
    wrap.appendChild(el('div', {class: 'group'}, [
      el('h4', {text: 'TACAN'}),
      el('div', {class: 'grid3'}, [
        el('div', {class: 'field'}, [el('span', {text: 'channel'}),
          el('div', {class: 'mono', text: t.label})]),
        el('div', {class: 'field'}, [el('span', {text: 'range'}),
          el('div', {class: 'mono', text: t.range + ' nm'})]),
        el('div', {class: 'field'}, [el('span', {text: 'ILS'}),
          el('div', {class: 'mono', text: t.ils ? t.ils.toFixed(1) : '—'})]),
      ]),
      el('div', {class: 'field-doc',
        text: 'from stations.dat, keyed by campaign id ' + info.values.campId}),
    ]));
  }

  const stats = [];
  for (const [k, label] of [['objFlags', 'flags'], ['nameId', 'name id'],
                            ['firstOwner', 'first owner'],
                            ['objType', 'type code']]) {
    if (v[k] === undefined) continue;
    stats.push(el('div', {class: 'field'}, [
      el('span', {text: label}),
      el('div', {class: 'mono', text: String(v[k])}),
    ]));
  }
  wrap.appendChild(el('div', {class: 'group'}, [
    el('h4', {text: 'State, as saved'}),
    el('div', {class: 'grid3'}, stats),
  ]));

  if (info.features && info.features.length) {
    const box = el('div', {class: 'group'});
    box.appendChild(el('h4', {text: 'Features (' + info.featureCount + ')'}));
    box.appendChild(el('p', {class: 'note', text:
      'Damage is stored as two bits per feature and the engine recomputes the '
      + "objective's condition from them on load, so this is the repair / "
      + 'destroy switch. Destroying or repairing one walks its critical links, '
      + 'the way the engine does.'}));

    const list = el('div', {class: 'feat-list'});
    for (const f of info.features) {
      list.appendChild(el('div', {
        class: 'feat' + (f.status === 3 ? ' dead' : ''),
      }, [
        el('span', {class: 'feat-name', title: 'feature slot ' + f.slot,
          text: f.name || ('feature ' + f.slot)}),
        el('span', {class: 'feat-val', text: f.value + '%'}),
        el('select', {disabled: !canEdit,
          onchange: guard(async (e) => {
            const r = await api('/api/objective/feature',
              {theater: S.theater, file: file, n: info.n, feature: f.slot,
               status: Number(e.target.value)});
            info.features = r.features;
            toast(r.changed > 1
              ? r.changed + ' linked features set to ' + r.statusName.toLowerCase()
              : 'Feature set to ' + r.statusName.toLowerCase());
            await refreshPending();
            await reload();
          })}, FEATURE_STATUS.map((s, i) =>
            el('option', {value: i, selected: i === f.status}, s))),
      ]));
    }
    box.appendChild(list);

    box.appendChild(el('div', {class: 'stores-tools'}, [
      el('button', {class: 'btn btn-sm', text: 'Repair all',
        disabled: !canEdit,
        onclick: guard(async () => {
          const r = await api('/api/objective/feature',
            {theater: S.theater, file: file, n: info.n, action: 'repair-all'});
          toast(r.changed + ' features repaired');
          await refreshPending();
          await reload();
        })}),
      el('button', {class: 'btn btn-sm', text: 'Destroy all',
        disabled: !canEdit,
        onclick: guard(async () => {
          const r = await api('/api/objective/feature',
            {theater: S.theater, file: file, n: info.n, action: 'destroy-all'});
          toast(r.changed + ' features destroyed');
          await refreshPending();
          await reload();
        })}),
      el('button', {class: 'btn btn-sm', style: 'margin-left:auto',
        onclick: () => openTable('objective', info.className || '')},
        'Edit this objective type'),
      info.featureCount > info.features.length
        ? el('span', {class: 'hint',
            text: 'showing ' + info.features.length + ' of ' + info.featureCount})
        : null,
    ]));
    wrap.appendChild(box);
  }

  if (info.links && info.links.length) {
    wrap.appendChild(el('div', {class: 'group'}, [
      el('h4', {text: 'Links to neighbours (' + info.links.length + ')'}),
      el('ul', {class: 'wp-list'}, info.links.map((l, i) =>
        el('li', {}, [
          el('span', {class: 'i', text: String(i)}),
          el('span', {class: 'mono', text: 'id ' + l.id[0]}),
          el('span', {class: 'i', text: 'costs ' + l.costs.join('/')}),
        ]))),
    ]));
  }

  return wrap;
}


// --- squadron stores --------------------------------------------------------

// Two tables meet on this panel and they are not the same kind of thing.
//
//   COUNT is the squadron's own stock, in the .cam entity. Editing it changes
//         this squadron and nothing else.
//   MAX   is class data, one row of FALCON4.SSD shared by every squadron class
//         with the same SpecialIndex. A max of 0 means the squadron is never
//         issued the weapon -- resupply skips it -- so the loadout screen shows
//         it OUT for the whole war. Max is the real enable/disable, and it
//         moves other squadrons with it.
//
// The engine's own availability is (count * 4) / max, clamped 0..4, which is
// what the bar shows and what the loadout screen colours by.
function storesPanel(file, info) {
  const st = info.stores;
  const box = el('div', {class: 'group'});

  if (!st || !st.available) {
    return null;
  }

  let showAll = false;

  const post = (body) => api('/api/unit/stores', Object.assign(
    {theater: S.theater, file: file, n: info.n, showAll: showAll}, body));

  const redraw = (fresh) => {
    info.stores = fresh;
    const next = storesPanel(file, info);
    if (next) box.replaceWith(next);
  };

  box.appendChild(el('h4', {text: 'Stores'}));
  box.appendChild(el('p', {class: 'note', text:
    'These are the squadron\u2019s shelves, not one jet\u2019s pylons: Stock '
    + 'is how much of each weapon it holds (in supply units), and resupply '
    + 'tops it up to the second number. What one aircraft can carry is on the '
    + 'Aircraft tab. ' + st.shown + ' of ' + st.length + ' weapon slots '
    + 'shown; the resupply level comes from stores row ' + st.specialIndex
    + ', shared by '
    + st.sharedWith + ' squadron ' + (st.sharedWith === 1 ? 'class' : 'classes')
    + ' in every campaign of this theater.'}));

  if (st.locked) {
    const cap = el('input', {type: 'number', class: 'st-n', min: '1',
                             max: '255', value: '100'});
    box.appendChild(el('div', {class: 'note warn stores-locked'}, [
      el('div', {text: st.locked + ' weapon' + (st.locked === 1 ? '' : 's')
        + ' this aircraft can load ' + (st.locked === 1 ? 'is' : 'are')
        + ' never issued (max 0), so the loadout screen shows '
        + (st.locked === 1 ? 'it' : 'them') + ' OUT for the whole war. '
        + 'Unlocking gives each a maximum and fills this squadron now; '
        + 'resupply keeps it topped up after that.'}),
      el('div', {class: 'stores-tools'}, [
        el('span', {class: 'hint', text: 'max'}), cap,
        el('button', {class: 'btn btn-sm btn-primary',
          text: 'Unlock ' + st.locked + ' locked weapon'
                + (st.locked === 1 ? '' : 's'),
          onclick: guard(async () => {
            const r = await post({action: 'unlock', max: Number(cap.value)});
            toast(r.changed + ' weapon(s) unlocked');
            await refreshPending();
            redraw(r.stores);
          })}),
      ]),
    ]));
  }

  const inf = [];
  for (const [k, label] of [['aa', 'AA'], ['ag', 'AG'], ['gun', 'gun']]) {
    if (st.infinite[k]) {
      inf.push((st.infiniteNames[k] || ('weapon ' + st.infinite[k]))
               + ' (' + label + ')');
    }
  }
  if (inf.length) {
    box.appendChild(el('p', {class: 'note', text:
      'Never runs out: ' + inf.join(', ') + '.'}));
  }

  const tools = el('div', {class: 'stores-tools'}, [
    el('button', {class: 'btn btn-sm', text: 'Resupply to max',
      onclick: guard(async () => {
        const r = await post({action: 'resupply'});
        toast(r.changed + ' weapon(s) refilled');
        await refreshPending();
        redraw(r.stores);
      })}),
    el('button', {class: 'btn btn-sm', text: 'Empty the rack',
      onclick: guard(async () => {
        const r = await post({action: 'clear'});
        toast(r.changed + ' weapon(s) emptied');
        await refreshPending();
        redraw(r.stores);
      })}),
    el('label', {class: 'chk'}, [
      el('input', {type: 'checkbox', onchange: guard(async (ev) => {
        showAll = ev.target.checked;
        const fresh = await api('/api/unit?' + qs(
          {file: file, n: info.n, allstores: showAll ? 1 : 0}));
        redraw(fresh.stores);
      })}),
      el('span', {text: 'Show all'}),
    ]),
  ]);
  box.appendChild(tools);

  // The full array is 600 slots; with Show all ticked a name filter is the
  // only way to find one.
  const filter = el('input', {type: 'text', class: 'st-filter', spellcheck: 'false',
    placeholder: 'Filter weapons…'});
  filter.addEventListener('input', () => refill());
  box.appendChild(filter);

  const body = el('tbody');

  const refill = () => {
    body.textContent = '';
    const needle = filter.value.trim().toLowerCase();
    for (const r of st.rows) {
      if (needle && !r.name.toLowerCase().includes(needle)) continue;

      const cnt = el('input', {type: 'number', class: 'st-n', min: '0',
                               max: '255', value: String(r.count)});
      cnt.addEventListener('change', guard(async () => {
        const res = await post({index: r.index, count: Number(cnt.value)});
        await refreshPending();
        redraw(res.stores);
      }));

      const cap = el('input', {type: 'number', class: 'st-n', min: '0',
                               max: '255', value: String(r.max)});
      cap.addEventListener('change', guard(async () => {
        const res = await post({index: r.index, max: Number(cap.value)});
        await refreshPending();
        redraw(res.stores);
      }));

      const on = el('input', {type: 'checkbox'});
      on.checked = r.carryable;
      on.addEventListener('change', guard(async () => {
        // Disabling means taking the class maximum to zero -- nothing else
        // actually stops the weapon being loaded.
        const res = await post({index: r.index,
                                max: on.checked ? (r.max || 100) : 0,
                                fill: on.checked});
        await refreshPending();
        redraw(res.stores);
      }));

      body.appendChild(el('tr', {class: r.carryable ? '' : 'st-off'}, [
        el('td', {}, on),
        el('td', {class: 'st-name', title: 'FALCON4.WCD row ' + r.index
                  + (r.loadable ? ' \u00b7 on this aircraft\u2019s hardpoints'
                                : '')}, [
          r.name,
          r.locked ? el('span', {class: 'st-tag', text: 'OUT \u00b7 locked'})
          : r.stockOnly ? el('span', {class: 'st-tag warn', title:
              'Resupply level is 0, so none is ever added, but the stock '
              + 'still on the shelf can be loaded until it runs out.',
              text: 'no resupply'}) : null]),
        el('td', {}, cnt),
        el('td', {}, r.infinite ? el('span', {class: 'hint', text: 'inf'}) : cap),
        el('td', {}, el('span', {class: 'st-bar av' + r.avail,
                                 title: 'availability ' + r.avail + ' of 4'})),
      ]));
    }
  };
  refill();

  box.appendChild(el('table', {class: 'data stores'}, [
    el('thead', {}, el('tr', {}, ['', 'Weapon', 'Stock', 'Resupply to', ''].map(
      h => el('th', {text: h})))),
    body,
  ]));

  return box;
}


// --- the squadron header ----------------------------------------------------

// The things Mission Commander puts at the top of a squadron page. All of it is
// read-only here: the patch, the aeroplane and the name are class data or
// composed by the engine, not fields you can type into.
function squadronHeader(info) {
  const sq = info.squadron;
  if (!sq) return null;

  const box = el('div', {class: 'group sq-head'});

  const left = el('div', {class: 'sq-patch'});
  if (sq.patch !== null && sq.patch !== undefined) {
    const img = el('img', {
      alt: sq.patchName || ('patch ' + sq.patch),
      title: (sq.patchName || '') + '  (patch ' + sq.patch + ')',
      src: '/api/patch?theater=' + encodeURIComponent(S.theater)
           + '&i=' + sq.patch,
    });
    // A theater without the patch sheet just gets no picture.
    img.addEventListener('error', () => img.remove());
    left.appendChild(img);
  }

  const rows = [];
  const add = (label, value) => {
    if (value === '' || value === null || value === undefined) return;
    rows.push(el('div', {class: 'field'}, [
      el('span', {text: label}),
      el('div', {class: 'mono', text: String(value)}),
    ]));
  };
  add('aircraft', sq.aircraft);
  add('airbase', sq.airbase);
  add('specialty', sq.specialtyName || sq.specialty);
  add('patch', sq.patchName ? (sq.patchName + '  #' + sq.patch)
                            : ('#' + sq.patch));

  const right = el('div', {class: 'sq-id'}, [
    el('h4', {text: sq.name || 'Squadron'}),
    sq.selectable
      ? el('span', {class: 'tag', text: 'player selectable'})
      : el('span', {class: 'hint', text: 'not in the campaign\u2019s squadron list'}),
    el('div', {class: 'grid2'}, rows),
  ]);

  box.appendChild(el('div', {class: 'sq-top'}, [left, right]));

  const stat = (label, value) => el('div', {class: 'field'}, [
    el('span', {text: label}),
    el('div', {class: 'mono', text: String(value)}),
  ]);
  box.appendChild(el('div', {class: 'grid4'}, [
    stat('A-A kills', sq.kills.aa),
    stat('A-G kills', sq.kills.ag),
    stat('A-S kills', sq.kills.as),
    stat('A-N kills', sq.kills.an),
    stat('missions', sq.missionsFlown),
    stat('score', sq.missionScore),
    stat('aircraft lost', sq.totalLosses),
    stat('pilots lost', sq.pilotLosses),
  ]));

  if (sq.pilots && sq.pilots.length) {
    const body = el('tbody', {}, sq.pilots.map((p, i) => el('tr', {}, [
      el('td', {class: 'idx', text: String(i)}),
      el('td', {class: 'num', text: String(p.id)}),
      el('td', {class: 'num', text: String(p.skill)}),
      el('td', {class: 'num', text: String(p.rating)}),
      el('td', {text: p.statusName}),
      el('td', {class: 'num', text: String(p.kills.aa)}),
      el('td', {class: 'num', text: String(p.kills.ag)}),
      el('td', {class: 'num', text: String(p.missions)}),
    ])));

    const table = el('table', {class: 'data pilots'}, [
      el('thead', {}, el('tr', {}, ['#', 'Id', 'Skill', 'Rating', 'Status',
                                    'AA', 'AG', 'Msn']
        .map(h => el('th', {text: h})))),
      body,
    ]);

    const wrap = el('div', {class: 'pilots-wrap', hidden: true}, table);
    const toggle = el('button', {class: 'btn btn-sm',
      text: 'Pilots (' + sq.pilots.length + ')',
      onclick: () => { wrap.hidden = !wrap.hidden; }});

    box.appendChild(el('div', {style: 'margin-top:10px'}, toggle));
    box.appendChild(wrap);
    box.appendChild(el('p', {class: 'note', text:
      'Pilot names live in the campaign\u2019s callsign table, which the editor '
      + 'does not read yet \u2014 these are the ids the game looks them up by.'}));
  }

  return box;
}

