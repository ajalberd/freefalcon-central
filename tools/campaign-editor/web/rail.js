'use strict';

// The Rail overlay: railway lines built from OpenStreetMap by osm_rail.py
// (rail.json in the theater's terrain folder), drawn over the campaign map.
// Coordinates are campaign kilometres as floats, the same frame the terrain
// tiles are drawn in. "Rail fit" adds what the projection was checked
// against: the OSM coastline as projected (it should trace the terrain's
// shore) and, per airbase, a line from where Falcon has it to where the fit
// puts the real field.

const RAIL = {theater: null, data: null, loading: false};

async function loadRail(redraw) {
  if (RAIL.theater === S.theater || RAIL.loading) return;
  RAIL.loading = true;
  try {
    const d = await api('/api/rail?debug=1&theater=' +
                        encodeURIComponent(S.theater));
    RAIL.theater = S.theater;
    RAIL.data = d.available ? d : null;
    if (redraw) redraw();
  } catch (e) {
    RAIL.theater = S.theater;          // an editor without rail still works
    RAIL.data = null;
  } finally {
    RAIL.loading = false;
  }
}

function railStyle(line) {
  if (line.railway === 'narrow_gauge') return ['#b9a27a', 1.0];
  if (line.usage === 'main') return ['#ffd34d', 1.8];
  if (line.usage === 'branch') return ['#f0b060', 1.3];
  return ['#c9ccd4', 1.0];                         // industrial, military, ''
}

function railPath(g, pts, from, to, toScreen) {
  g.beginPath();
  for (let i = from; i <= to; i++) {
    const [sx, sy] = toScreen(pts[i][0], pts[i][1]);
    if (i === from) g.moveTo(sx, sy); else g.lineTo(sx, sy);
  }
}

// Visible when any part of the line's box is on screen.
function railOnScreen(line, toScreen, r) {
  if (!line._box) {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const [x, y] of line.pts) {
      if (x < x0) x0 = x; if (x > x1) x1 = x;
      if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    line._box = [x0, y0, x1, y1];
  }
  const [x0, y0, x1, y1] = line._box;
  const [ax, ay] = toScreen(x0, y1), [bx, by] = toScreen(x1, y0);
  return bx >= 0 && by >= 0 && ax <= r.width && ay <= r.height;
}

function drawRailLines(g, r, toScreen, scale) {
  const d = RAIL.data;
  if (!d) return;
  const zoom = Math.min(2.2, Math.max(0.7, Math.sqrt(scale)));
  g.save();
  g.lineCap = 'round';
  g.lineJoin = 'round';
  // A dark casing under everything first, so the lines read on any ground.
  for (const line of d.lines) {
    if (!railOnScreen(line, toScreen, r)) continue;
    const [, w] = railStyle(line);
    g.strokeStyle = 'rgba(10,12,16,.7)';
    g.lineWidth = (w + 1.6) * zoom;
    railPath(g, line.pts, 0, line.pts.length - 1, toScreen);
    g.stroke();
  }
  for (const line of d.lines) {
    if (!railOnScreen(line, toScreen, r)) continue;
    const [colour, w] = railStyle(line);
    const seg = line.seg;
    // Runs of one flag: plain, bridge (drawn pale), tunnel (dashed).
    let a = 0;
    const n = line.pts.length - 1;
    while (a < n) {
      const f = seg ? seg[a] : '-';
      let b = a + 1;
      while (b < n && seg && seg[b] === f) b++;
      g.setLineDash(f === 't' ? [3 * zoom, 3 * zoom] : []);
      g.strokeStyle = f === 'b' ? '#ffffff' : colour;
      g.lineWidth = w * zoom;
      railPath(g, line.pts, a, b, toScreen);
      g.stroke();
      a = b;
    }
  }
  g.restore();
}

function drawRailFit(g, r, toScreen) {
  const d = RAIL.data;
  if (!d) return;
  g.save();
  const coast = (d.debug && d.debug.coast) || [];
  g.strokeStyle = 'rgba(80,230,255,.9)';
  g.lineWidth = 1;
  for (const pts of coast) {
    railPath(g, pts, 0, pts.length - 1, toScreen);
    g.stroke();
  }
  g.font = '600 11px "Segoe UI", system-ui, sans-serif';
  g.textAlign = 'left';
  for (const c of d.control || []) {
    if (!c.fit) continue;
    const [gx, gy] = toScreen(c.grid[0], c.grid[1]);
    const [fx, fy] = toScreen(c.fit[0], c.fit[1]);
    const colour = c.used ? '#ff5fd2' : '#8a8f99';
    g.strokeStyle = colour;
    g.fillStyle = colour;
    g.lineWidth = 1.5;
    g.beginPath(); g.moveTo(gx, gy); g.lineTo(fx, fy); g.stroke();
    g.beginPath(); g.arc(gx, gy, 3, 0, Math.PI * 2); g.stroke();   // Falcon
    g.beginPath(); g.arc(fx, fy, 2.5, 0, Math.PI * 2); g.fill();   // real
    const label = c.name + ' ' + c.errKm.toFixed(1) + ' km' +
                  (c.used ? '' : ' (dropped)');
    g.lineWidth = 3;
    g.strokeStyle = 'rgba(8,11,16,.85)';
    g.strokeText(label, gx + 6, gy - 5);
    g.fillText(label, gx + 6, gy - 5);
  }
  g.restore();
}

// One line for the toolbar tooltip: what the file is and how good the fit is.
function railSummary() {
  const d = RAIL.data;
  if (!d) return '';
  const p = d.projection || {};
  return d.lines.length + ' lines, ' + Math.round(d.trackKm) + ' km of track' +
    ' · coast within 1.5 km: ' + Math.round((p.shoreWithin1_5Km || 0) * 100) +
    '% · airbases ' + (p.airbaseRmsKm || 0).toFixed(1) + ' km RMS' +
    '\n' + (d.attribution || '');
}
